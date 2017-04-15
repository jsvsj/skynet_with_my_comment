#ifndef skynet_databuffer_h
#define skynet_databuffer_h

#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define MESSAGEPOOL 1023

// gate ������Ӧ�ò�msg�Ļ�����ʵ��
struct message {
	char * buffer;
	int size;		//buffer��С
	struct message * next;
};

// ���ݻ��������� ��������Ӧ�ò����Ϣ����,��ŵ��� struct message������
struct databuffer {

	int header;


	//ͷ��������ݿ�ʼ��λ��
	int offset;
	
	int size; //���еĽڵ�����ݵĴ�С�ĺ�
	
	struct message * head;
	struct message * tail;
};

// msg_pool_list ��Ϣ������ڵ�

struct messagepool_list {
	struct messagepool_list *next;
				//MESSAGEPOOL  1023
				//���meeeage�����ǵ�����
	struct message pool[MESSAGEPOOL];
};


// ��Ϣ��,��ŵ���struct messagepool_list ����
struct messagepool {
	struct messagepool_list * pool;

	//�洢��databuffer�б�������ݵ�message�ڵ�
	//���д���messagepool_list�е�message�����е�message
	struct message * freelist;  // ��¼��Ϣָ�����ͷŵ�ʱ����, ͷ�巨
};

// use memset init struct 

//����struct messagepool *pool�е�messagepool_list��freelist
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

//���databuffer��һ��ͷ�ڵ��е�����,������ͷ�����뵽struct messagepool *mp�е�freelist��
static inline void
_return_message(struct databuffer *db, struct messagepool *mp) {
	struct message *m = db->head;
	if (m->next == NULL) {
		assert(db->tail == m);
		db->head = db->tail = NULL;
	} else {
		db->head = m->next;
	}
	//����
	skynet_free(m->buffer);
	m->buffer = NULL;
	m->size = 0;

	//��freelistͷ������ڵ�   ͷ�巨
	m->next = mp->freelist; 
	mp->freelist = m;
}

//��databuffer�ж�ȡsz��С������,ͨ��buffer����
static void
databuffer_read(struct databuffer *db, struct messagepool *mp, void * buffer, int sz) {
	assert(db->size >= sz);
	db->size -= sz;
	
	for (;;) {
		//ָ��ͷ���
		struct message *current = db->head;

		//ͷ�������ӵ�е����ݴ�С
		int bsz = current->size - db->offset;

		//���ͷ�������ӵ�е����ݴ�С>��Ҫ��ȡ�����ݴ�Сsz
		//ֱ�Ӵ�ͷ����ж�ȡ���ݼ���
		if (bsz > sz) {
			memcpy(buffer, current->buffer + db->offset, sz);
			db->offset += sz;
			return;
		}

		//������ý�ͷ������ݶ���,
		if (bsz == sz) {
			memcpy(buffer, current->buffer + db->offset, sz);

			//����ͷ��������ݿ�ʼ��λ��
			db->offset = 0;
			//���ͷ������ݣ���ԭ��ͷ����дһ���ڵ���Ϊͷ���
			//��ԭ����ͷ�����뵽mp��freelist������
			_return_message(db, mp);
			return;
		} 

		//�����ǰͷ����е�����<��Ҫ��ȡ������
		else 
		{
		//�ȶ�ȡһ���֣�֮��ѭ�����Ŷ�
			memcpy(buffer, current->buffer + db->offset, bsz);

			_return_message(db, mp);
			db->offset = 0;

			buffer+=bsz;

			sz-=bsz;
		}
	}
}

//��data,��СΪsz�����ݼ��뵽 db������
static void
databuffer_push(struct databuffer *db, struct messagepool *mp, void *data, int sz) {
	struct message * m;
	
	//�����freelist���п��е�message,ֱ������ʹ��
	if (mp->freelist) {
		m = mp->freelist;
		mp->freelist = m->next;
	} else {

		//���򣬵���  malloc(),messagepool_list����һ��message����
		struct messagepool_list * mpl = skynet_malloc(sizeof(*mpl));

		//ָ��message����ͷ��
		struct message * temp = mpl->pool;

		int i;

		//�������е�message��ʼ��������ͨ��ָ���������� ,
		//ע���Ǵ�һ��ʼ��
		for (i=1;i<MESSAGEPOOL;i++) {
			temp[i].buffer = NULL;
			temp[i].size = 0;

			//ͨ��ָ����������
			temp[i].next = &temp[i+1];
		}
		
		temp[MESSAGEPOOL-1].next = NULL;

		//�������� mpl  ʹ��ͷ�巨���뵽 struct messagepool *mp��
 		mpl->next = mp->pool;
		mp->pool = mpl;

		//mָ���������еĵ�һ��Ԫ��
		m = &temp[0];


		//ʣ���Ԫ�ؼ��뵽��mp��freelist��,��Ϊֻ��ԭ����freelistû��Ԫ���ˣ��Ż�ִ�е�����
		mp->freelist = &temp[1];
	}

	
	m->buffer = data;
	m->size = sz;
	m->next = NULL;
	//�����ܵĽڵ��������
	db->size += sz;

	//��message���뵽db��������
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


//��databuffer��header=0
static inline void
databuffer_reset(struct databuffer *db) {
	db->header = 0;
}

//���� databuffer�е�����
static void
databuffer_clear(struct databuffer *db, struct messagepool *mp) {

	while (db->head) {
		//���ͷ������ݣ���ԭ��ͷ����дһ���ڵ���Ϊͷ���
		//��ԭ����ͷ�����뵽mp��freelist������
		_return_message(db,mp);
	}

	memset(db, 0, sizeof(*db));
}

#endif
