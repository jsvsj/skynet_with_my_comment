#ifndef skynet_databuffer_h
#define skynet_databuffer_h

#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define MESSAGEPOOL 1023

// gate 服务中应用层msg的缓冲区实现
struct message {
	char * buffer;
	int size;		//buffer大小
	struct message * next;
};

// 数据缓冲区链表 用来保存应用层的消息数据,存放的是 struct message的链表
struct databuffer {

	int header;


	//头结点中数据开始的位置
	int offset;
	
	int size; //所有的节点的数据的大小的和
	
	struct message * head;
	struct message * tail;
};

// msg_pool_list 消息池链表节点

struct messagepool_list {
	struct messagepool_list *next;
				//MESSAGEPOOL  1023
				//存放meeeage类型是的数组
	struct message pool[MESSAGEPOOL];
};


// 消息池,存放的是struct messagepool_list 链表
struct messagepool {
	struct messagepool_list * pool;

	//存储在databuffer中被清空数据的message节点
	//还有创建messagepool_list中的message数组中的message
	struct message * freelist;  // 记录消息指针在释放的时候用, 头插法
};

// use memset init struct 

//销毁struct messagepool *pool中的messagepool_list和freelist
static void 
messagepool_free(struct messagepool *pool) {
	struct messagepool_list *p = pool->pool;
	while(p) {
		struct messagepool_list *tmp = p;
		p=p->next;
		skynet_free(tmp);
	}
	pool->pool = NULL;
	pool->freelist = NULL;
}

//清空databuffer的一个头节点中的数据,并将该头结点插入到struct messagepool *mp中的freelist中
static inline void
_return_message(struct databuffer *db, struct messagepool *mp) {
	struct message *m = db->head;
	if (m->next == NULL) {
		assert(db->tail == m);
		db->head = db->tail = NULL;
	} else {
		db->head = m->next;
	}
	//销毁
	skynet_free(m->buffer);
	m->buffer = NULL;
	m->size = 0;

	//在freelist头部插入节点   头插法
	m->next = mp->freelist; 
	mp->freelist = m;
}

//从databuffer中读取sz大小的数据,通过buffer传出
static void
databuffer_read(struct databuffer *db, struct messagepool *mp, void * buffer, int sz) {
	assert(db->size >= sz);
	db->size -= sz;
	
	for (;;) {
		//指向头结点
		struct message *current = db->head;

		//头结点中所拥有的数据大小
		int bsz = current->size - db->offset;

		//如果头结点中所拥有的数据大小>需要读取的数据大小sz
		//直接从头结点中读取数据即可
		if (bsz > sz) {
			memcpy(buffer, current->buffer + db->offset, sz);
			db->offset += sz;
			return;
		}

		//如果正好将头结点数据读完,
		if (bsz == sz) {
			memcpy(buffer, current->buffer + db->offset, sz);

			//调整头结点中数据开始的位置
			db->offset = 0;
			//清空头结点数据，将原来头结点的写一个节点作为头结点
			//将原来的头结点加入到mp的freelist链表中
			_return_message(db, mp);
			return;
		} 

		//如果当前头结点中的数据<需要读取的数据
		else 
		{
		//先读取一部分，之后循环接着读
			memcpy(buffer, current->buffer + db->offset, bsz);

			_return_message(db, mp);
			db->offset = 0;

			buffer+=bsz;

			sz-=bsz;
		}
	}
}

//将data,大小为sz的数据加入到 db链表中
static void
databuffer_push(struct databuffer *db, struct messagepool *mp, void *data, int sz) {
	struct message * m;
	
	//如果在freelist中有空闲的message,直接拿来使用
	if (mp->freelist) {
		m = mp->freelist;
		mp->freelist = m->next;
	} else {

		//否则，调用  malloc(),messagepool_list中有一个message数组
		struct messagepool_list * mpl = skynet_malloc(sizeof(*mpl));

		//指向message数组头部
		struct message * temp = mpl->pool;

		int i;

		//将数组中的message初始化，并且通过指针链接起来 ,
		//注意是从一开始的
		for (i=1;i<MESSAGEPOOL;i++) {
			temp[i].buffer = NULL;
			temp[i].size = 0;

			//通过指针链接起来
			temp[i].next = &temp[i+1];
		}
		
		temp[MESSAGEPOOL-1].next = NULL;

		//将创建的 mpl  使用头插法插入到 struct messagepool *mp中
 		mpl->next = mp->pool;
		mp->pool = mpl;

		//m指向了数组中的第一个元素
		m = &temp[0];


		//剩余的元素加入到了mp的freelist中,因为只有原来的freelist没有元素了，才会执行到这里
		mp->freelist = &temp[1];
	}

	
	m->buffer = data;
	m->size = sz;
	m->next = NULL;
	//增加总的节点的数据量
	db->size += sz;

	//将message加入到db的链表中
	if (db->head == NULL) {
		assert(db->tail == NULL);
		db->head = db->tail = m;
	} else {
		db->tail->next = m;
		db->tail = m;
	}
}

static int
databuffer_readheader(struct databuffer *db, struct messagepool *mp, int header_size) {

	if (db->header == 0) {
		// parser header (2 or 4)
		if (db->size < header_size) {
			return -1;
		}
		uint8_t plen[4];
		
		databuffer_read(db,mp,(char *)plen,header_size);

		// big-endian
		if (header_size == 2) {
			db->header = plen[0] << 8 | plen[1];
		} else {
			db->header = plen[0] << 24 | plen[1] << 16 | plen[2] << 8 | plen[3];
		}
	}
	if (db->size < db->header)
		return -1;
	return db->header;
}


//将databuffer的header=0
static inline void
databuffer_reset(struct databuffer *db) {
	db->header = 0;
}

//销毁 databuffer中的数据
static void
databuffer_clear(struct databuffer *db, struct messagepool *mp) {

	while (db->head) {
		//清空头结点数据，将原来头结点的写一个节点作为头结点
		//将原来的头结点加入到mp的freelist链表中
		_return_message(db,mp);
	}

	memset(db, 0, sizeof(*db));
}

#endif
