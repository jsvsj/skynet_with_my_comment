#include "skynet.h"
#include "skynet_mq.h"
#include "skynet_handle.h"
#include "spinlock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>

//skynet 使用了二级队列 从全局的globe_mq 中取 mq来处理

#define DEFAULT_QUEUE_SIZE 64	//默认队列的大小

//64k,单机服务上限是64k,因而globle_mq数量最大值也是64k
//服务的id空间是2^24即16M

#define MAX_GLOBAL_MQ 0x10000	

// 0 means mq is not in global mq.
// 1 means mq is in global mq , or the message is dispatching.

#define MQ_IN_GLOBAL 1
#define MQ_OVERLOAD 1024

/*
消息的结构体
struct skynet_message {
		uint32_t source;	//消息源的句柄
		int session;		//用来做上下文的标识
		void * data;		//消息指针
		size_t sz;			//消息长度,消息的请求类型定义在高八位
		};
*/


//消息队列(循环队列),容量不固定，按需增长
//消息队列mq的结构

struct message_queue {
	//锁
	struct spinlock lock;
 
	uint32_t handle;  //所属服务handle
	int cap;		  //容量
	int head;		  //对头 
	int tail;		  //队尾

	//消息队列释放标记，当要释放一个服务的时候，清理标记
	//不能立即释放该服务对应的消息队列(有时候工作线程还在操作mq),就需要设置一个标记，标记是否可以释放
	int release;	 
	
	
	int in_global;
	int overload;
	int overload_threshold;

	//消息队列  利用数组实现的一个循环队列  
	struct skynet_message *queue;

	//为了将该队列串联到全局的二维的消息队列中
	struct message_queue *next;
};

//全局队列(循环队列，无锁队列),管理message_queue
struct global_queue {
	struct message_queue *head;
	struct message_queue *tail;

	//锁
	struct spinlock lock;
};

//全局变量
static struct global_queue *Q = NULL;


//插入struct message_queue   将queue加入到global_queue尾部
void 
skynet_globalmq_push(struct message_queue * queue) {
	struct global_queue *q= Q;

	SPIN_LOCK(q)
	assert(queue->next == NULL);
	if(q->tail) {
		q->tail->next = queue;
		q->tail = queue;
	} else {
		q->head = q->tail = queue;
	}
	SPIN_UNLOCK(q)
}

//从全局队列中   移除一个消息队列message_queue,从头部移除,返回该指针
struct message_queue * 
skynet_globalmq_pop() {
	struct global_queue *q = Q;

	SPIN_LOCK(q)
	struct message_queue *mq = q->head;
	
	if(mq) {
		q->head = mq->next;
		
		if(q->head == NULL) {
			assert(mq == q->tail);
			q->tail = NULL;
		}
		mq->next = NULL;
	}
	SPIN_UNLOCK(q)

	return mq;
}


//创建消息队列message_queue
struct message_queue * 
skynet_mq_create(uint32_t handle) {

	struct message_queue *q = skynet_malloc(sizeof(*q));
	q->handle = handle;
	q->cap = DEFAULT_QUEUE_SIZE;    //64
	q->head = 0;
	q->tail = 0;

	//初始化锁
	SPIN_INIT(q)
	
	// When the queue is create (always between service create and service init) ,
	// set in_global flag to avoid push it to global queue .
	// If the service init success, skynet_context_new will call skynet_mq_push to push it to global queue.

	//标记为在全局的二维消息队列中
	q->in_global = MQ_IN_GLOBAL;

	q->release = 0;
	q->overload = 0;
	q->overload_threshold = MQ_OVERLOAD;   // 1024
 
	//为消息数组分配内存
	q->queue = skynet_malloc(sizeof(struct skynet_message) * q->cap);
	q->next = NULL;

	return q;
}


//销毁消息队列message_queue
static void 
_release(struct message_queue *q) {
	assert(q->next == NULL);
	SPIN_DESTROY(q)
	skynet_free(q->queue);
	skynet_free(q);
}


//获取struct message_queue的handle,handle就是skynet_context存放在数组的的一个编号
uint32_t 
skynet_mq_handle(struct message_queue *q) {
	return q->handle;
}


//获取循环消息队列message_queue中消息的长度
int
skynet_mq_length(struct message_queue *q) {
	int head, tail,cap;

	SPIN_LOCK(q)
	head = q->head;
	tail = q->tail;
	cap = q->cap;
	SPIN_UNLOCK(q)
	
	if (head <= tail) {
		return tail - head;
	}
	return tail + cap - head;
}



int
skynet_mq_overload(struct message_queue *q) {
	if (q->overload) {
		int overload = q->overload;
		q->overload = 0;
		return overload;
	} 
	return 0;
}


//从循环消息队列message_queue中取出一条消息struct skynet_message   message是传出参数
int
skynet_mq_pop(struct message_queue *q, struct skynet_message *message) {
	int ret = 1;

	//加锁
	SPIN_LOCK(q)

	//如果循环消息队列中存在消息
	if (q->head != q->tail) {
		//传出一个消息
		*message = q->queue[q->head++];

		ret = 0;
		int head = q->head;
		int tail = q->tail;
		int cap = q->cap;

		if (head >= cap) {
			q->head = head = 0;
		}
		
		//获得循环消息队列中的消息个数
		int length = tail - head;

		if (length < 0) {
			length += cap;
		}

		while (length > q->overload_threshold) {
			q->overload = length;
			q->overload_threshold *= 2;
		}
		
	} else {
	
		// reset overload_threshold when queue is empty
		q->overload_threshold = MQ_OVERLOAD;
	}

	//如果循环消息队列中没有消息，=0标记该循环队列没有在全局的二维消息队列中
	if (ret) {
		q->in_global = 0;
	}
	
	SPIN_UNLOCK(q)

	return ret;
}


//将消息队列message_queue   的queue容量扩大两倍
static void
expand_queue(struct message_queue *q) {
	struct skynet_message *new_queue = skynet_malloc(sizeof(struct skynet_message) * q->cap * 2);
	int i;
	//复制以前的
	for (i=0;i<q->cap;i++) {
		new_queue[i] = q->queue[(q->head + i) % q->cap];
	}
	q->head = 0;
	q->tail = q->cap;
	q->cap *= 2;

	//销毁以前的 
	skynet_free(q->queue);
	
	q->queue = new_queue;
}


//将消息加入到message_queue 的 queue的尾部
void 
skynet_mq_push(struct message_queue *q, struct skynet_message *message) {
	assert(message);
	SPIN_LOCK(q)

	q->queue[q->tail] = *message;
	if (++ q->tail >= q->cap) {
		q->tail = 0;
	}

	//扩容
	if (q->head == q->tail) {
		expand_queue(q);
	}

	if (q->in_global == 0) {
		//标记为在全局的二维消息队列中
		q->in_global = MQ_IN_GLOBAL;

		//将消息队列加入到全局的消息队列中 
		skynet_globalmq_push(q);
	}
	
	SPIN_UNLOCK(q)
}


//初始化struct global_queue 全局的二维消息队列，即存储消息队列的队列
void 
skynet_mq_init() {
	struct global_queue *q = skynet_malloc(sizeof(*q));
	memset(q,0,sizeof(*q));
	SPIN_INIT(q);
	Q=q;
}

//标记message_queue的release
void 
skynet_mq_mark_release(struct message_queue *q) {

	//加锁
	SPIN_LOCK(q)
	assert(q->release == 0);
	q->release = 1;

	//如果没有加入到全局的消息队列中,则加入
	if (q->in_global != MQ_IN_GLOBAL) {
		
		skynet_globalmq_push(q);
	}
	SPIN_UNLOCK(q)
}


//删除消息队列message_queue

//message_drop是一个函数指针类型 
static void
_drop_queue(struct message_queue *q, message_drop drop_func, void *ud) {
	struct skynet_message msg;
	//不断的从消息队列message_queue中获得一个消息,存放在msg中 
	while(!skynet_mq_pop(q, &msg)) {

		//调用回调函数
		drop_func(&msg, ud);
	}
	//释放内存
	_release(q);
}

//销毁message_queue
void 
skynet_mq_release(struct message_queue *q, message_drop drop_func, void *ud) {
	SPIN_LOCK(q)
	
	if (q->release) {
		//有释放标记，则删除消息队列q
		
		SPIN_UNLOCK(q)
		
		_drop_queue(q, drop_func, ud);
	} else {

		//没有，则重新压入全局队列
		skynet_globalmq_push(q);
		SPIN_UNLOCK(q)
	}
}

