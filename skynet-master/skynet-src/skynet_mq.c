#include "skynet.h"
#include "skynet_mq.h"
#include "skynet_handle.h"
#include "spinlock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>

//skynet ʹ���˶������� ��ȫ�ֵ�globe_mq ��ȡ mq������

#define DEFAULT_QUEUE_SIZE 64	//Ĭ�϶��еĴ�С

//64k,��������������64k,���globle_mq�������ֵҲ��64k
//�����id�ռ���2^24��16M

#define MAX_GLOBAL_MQ 0x10000	

// 0 means mq is not in global mq.
// 1 means mq is in global mq , or the message is dispatching.

#define MQ_IN_GLOBAL 1
#define MQ_OVERLOAD 1024

/*
��Ϣ�Ľṹ��
struct skynet_message {
		uint32_t source;	//��ϢԴ�ľ��
		int session;		//�����������ĵı�ʶ
		void * data;		//��Ϣָ��
		size_t sz;			//��Ϣ����,��Ϣ���������Ͷ����ڸ߰�λ
		};
*/


//��Ϣ����(ѭ������),�������̶�����������
//��Ϣ����mq�Ľṹ

struct message_queue {
	//��
	struct spinlock lock;
 
	uint32_t handle;  //��������handle
	int cap;		  //����
	int head;		  //��ͷ 
	int tail;		  //��β

	//��Ϣ�����ͷű�ǣ���Ҫ�ͷ�һ�������ʱ��������
	//���������ͷŸ÷����Ӧ����Ϣ����(��ʱ�����̻߳��ڲ���mq),����Ҫ����һ����ǣ�����Ƿ�����ͷ�
	int release;	 
	
	
	int in_global;
	int overload;
	int overload_threshold;

	//��Ϣ����  ��������ʵ�ֵ�һ��ѭ������  
	struct skynet_message *queue;

	//Ϊ�˽��ö��д�����ȫ�ֵĶ�ά����Ϣ������
	struct message_queue *next;
};

//ȫ�ֶ���(ѭ�����У���������),����message_queue
struct global_queue {
	struct message_queue *head;
	struct message_queue *tail;

	//��
	struct spinlock lock;
};

//ȫ�ֱ���
static struct global_queue *Q = NULL;


//����struct message_queue   ��queue���뵽global_queueβ��
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

//��ȫ�ֶ�����   �Ƴ�һ����Ϣ����message_queue,��ͷ���Ƴ�,���ظ�ָ��
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


//������Ϣ����message_queue
struct message_queue * 
skynet_mq_create(uint32_t handle) {

	struct message_queue *q = skynet_malloc(sizeof(*q));
	q->handle = handle;
	q->cap = DEFAULT_QUEUE_SIZE;    //64
	q->head = 0;
	q->tail = 0;

	//��ʼ����
	SPIN_INIT(q)
	
	// When the queue is create (always between service create and service init) ,
	// set in_global flag to avoid push it to global queue .
	// If the service init success, skynet_context_new will call skynet_mq_push to push it to global queue.

	//���Ϊ��ȫ�ֵĶ�ά��Ϣ������
	q->in_global = MQ_IN_GLOBAL;

	q->release = 0;
	q->overload = 0;
	q->overload_threshold = MQ_OVERLOAD;   // 1024
 
	//Ϊ��Ϣ��������ڴ�
	q->queue = skynet_malloc(sizeof(struct skynet_message) * q->cap);
	q->next = NULL;

	return q;
}


//������Ϣ����message_queue
static void 
_release(struct message_queue *q) {
	assert(q->next == NULL);
	SPIN_DESTROY(q)
	skynet_free(q->queue);
	skynet_free(q);
}


//��ȡstruct message_queue��handle,handle����skynet_context���������ĵ�һ�����
uint32_t 
skynet_mq_handle(struct message_queue *q) {
	return q->handle;
}


//��ȡѭ����Ϣ����message_queue����Ϣ�ĳ���
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


//��ѭ����Ϣ����message_queue��ȡ��һ����Ϣstruct skynet_message   message�Ǵ�������
int
skynet_mq_pop(struct message_queue *q, struct skynet_message *message) {
	int ret = 1;

	//����
	SPIN_LOCK(q)

	//���ѭ����Ϣ�����д�����Ϣ
	if (q->head != q->tail) {
		//����һ����Ϣ
		*message = q->queue[q->head++];

		ret = 0;
		int head = q->head;
		int tail = q->tail;
		int cap = q->cap;

		if (head >= cap) {
			q->head = head = 0;
		}
		
		//���ѭ����Ϣ�����е���Ϣ����
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

	//���ѭ����Ϣ������û����Ϣ��=0��Ǹ�ѭ������û����ȫ�ֵĶ�ά��Ϣ������
	if (ret) {
		q->in_global = 0;
	}
	
	SPIN_UNLOCK(q)

	return ret;
}


//����Ϣ����message_queue   ��queue������������
static void
expand_queue(struct message_queue *q) {
	struct skynet_message *new_queue = skynet_malloc(sizeof(struct skynet_message) * q->cap * 2);
	int i;
	//������ǰ��
	for (i=0;i<q->cap;i++) {
		new_queue[i] = q->queue[(q->head + i) % q->cap];
	}
	q->head = 0;
	q->tail = q->cap;
	q->cap *= 2;

	//������ǰ�� 
	skynet_free(q->queue);
	
	q->queue = new_queue;
}


//����Ϣ���뵽message_queue �� queue��β��
void 
skynet_mq_push(struct message_queue *q, struct skynet_message *message) {
	assert(message);
	SPIN_LOCK(q)

	q->queue[q->tail] = *message;
	if (++ q->tail >= q->cap) {
		q->tail = 0;
	}

	//����
	if (q->head == q->tail) {
		expand_queue(q);
	}

	if (q->in_global == 0) {
		//���Ϊ��ȫ�ֵĶ�ά��Ϣ������
		q->in_global = MQ_IN_GLOBAL;

		//����Ϣ���м��뵽ȫ�ֵ���Ϣ������ 
		skynet_globalmq_push(q);
	}
	
	SPIN_UNLOCK(q)
}


//��ʼ��struct global_queue ȫ�ֵĶ�ά��Ϣ���У����洢��Ϣ���еĶ���
void 
skynet_mq_init() {
	struct global_queue *q = skynet_malloc(sizeof(*q));
	memset(q,0,sizeof(*q));
	SPIN_INIT(q);
	Q=q;
}

//���message_queue��release
void 
skynet_mq_mark_release(struct message_queue *q) {

	//����
	SPIN_LOCK(q)
	assert(q->release == 0);
	q->release = 1;

	//���û�м��뵽ȫ�ֵ���Ϣ������,�����
	if (q->in_global != MQ_IN_GLOBAL) {
		
		skynet_globalmq_push(q);
	}
	SPIN_UNLOCK(q)
}


//ɾ����Ϣ����message_queue

//message_drop��һ������ָ������ 
static void
_drop_queue(struct message_queue *q, message_drop drop_func, void *ud) {
	struct skynet_message msg;
	//���ϵĴ���Ϣ����message_queue�л��һ����Ϣ,�����msg�� 
	while(!skynet_mq_pop(q, &msg)) {

		//���ûص�����
		drop_func(&msg, ud);
	}
	//�ͷ��ڴ�
	_release(q);
}

//����message_queue
void 
skynet_mq_release(struct message_queue *q, message_drop drop_func, void *ud) {
	SPIN_LOCK(q)
	
	if (q->release) {
		//���ͷű�ǣ���ɾ����Ϣ����q
		
		SPIN_UNLOCK(q)
		
		_drop_queue(q, drop_func, ud);
	} else {

		//û�У�������ѹ��ȫ�ֶ���
		skynet_globalmq_push(q);
		SPIN_UNLOCK(q)
	}
}

