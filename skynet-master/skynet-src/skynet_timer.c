#include "skynet.h"

#include "skynet_timer.h"
#include "skynet_mq.h"
#include "skynet_server.h"
#include "skynet_handle.h"
#include "spinlock.h"

#include <time.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#if defined(__APPLE__)
#include <sys/time.h>
#include <mach/task.h>
#include <mach/mach.h>
#endif

//skynet ��ʱ����ʵ��Ϊlinux�ں˵ı�׼����  ����Ϊ 0.01s ����Ϸһ����˵���� �߾��ȵĶ�ʱ���ܷ�CPU


typedef void (*timer_execute_func)(void *ud,void *arg);


#define TIME_NEAR_SHIFT 8
#define TIME_NEAR (1 << TIME_NEAR_SHIFT)     // 2^8 = 256
#define TIME_LEVEL_SHIFT 6
#define TIME_LEVEL (1 << TIME_LEVEL_SHIFT)		// 64
#define TIME_NEAR_MASK (TIME_NEAR-1)			 // 255
#define TIME_LEVEL_MASK (TIME_LEVEL-1)			  // 63
	
struct timer_event {
	uint32_t handle;
	int session;
};


//ʱ��ڵ�
struct timer_node {
	struct timer_node *next;
	uint32_t expire;		// ��ʱ�δ���� ����ʱ���
};

//ʱ��ڵ����
struct link_list {
	struct timer_node head;
	struct timer_node *tail;
};


//��ʱ���¼�����ĳ���ṹ 
struct timer {

	//near��ŵ������ʱ��ڵ��ʱ����ǵ�ǰʱ�䣬������Ҫ�ȴ���ʱ��ڵ� 

	//TIME_NEAR   256
	struct link_list near[TIME_NEAR];		// ��ʱ�������� ����˲�ͬ�Ķ�ʱ������

	//TIME_LEVEL  64
	struct link_list t[4][TIME_LEVEL];		// 4���ݶӣ�4����ͬ�Ķ�ʱ��
	
	struct spinlock lock;				//����������pthread_mutex_t  ��ԭ�Ӳ���

	uint32_t time;						//��ǰ�Ѿ������ĵδ����

	//�������ʱ����  xxxxxx�� yyyy΢��

	//���ɵ�λ�� 0.01�� 
	//��starttime=xxxxxx*100
	uint32_t starttime;					//���������¼�(����ʱ��)

	//current = yyyy / 10000
	uint64_t current;					//��ǰʱ�䣬�ֶ���ϵͳ�Ŀ���ʱ��(���ʱ��)

	//���п�ܺ󣬳�ʼ��ʱ��ṹ��ʱ��׼ȷʱ�䣬��λ��0.01��
	//����ʱ���߳���ÿ�ε��� skynet_updatetime()ʱ��ʱ��
	uint64_t current_point;
};

static struct timer * TI = NULL;


//���link_list,����link_list.head=0 ������ԭ������ĵ�һ�ڵ�ָ��
static inline struct timer_node *
link_clear(struct link_list *list) {
	struct timer_node * ret = list->head.next;
	list->head.next = 0;
	list->tail = &(list->head);

	return ret;
}

//��node���뵽����link_listβ��
static inline void
link(struct link_list *list,struct timer_node *node) {
	list->tail->next = node;
	list->tail = node;
	node->next=0;
}


//��timer_node���뵽timer�� �ʺϵ�link_list��
static void
add_node(struct timer *T,struct timer_node *node) {
	uint32_t time=node->expire;	//��ʱ�ĵδ���
	uint32_t current_time=T->time;

	//������ǵ�ǰʱ�䣬û�г�ʱ  TIME_NEAR_MASK   255    11111111   ����ǰ8λ
	if ((time|TIME_NEAR_MASK)==(current_time|TIME_NEAR_MASK)) {

		//��node���뵽near��  struct link_list near[TIME_NEAR];	
		//��ŵ������λ���� time&TIME_NEAR_MASK  �� time&1111111  ��ȡtime��ǰ8λ  
		link(&T->near[time&TIME_NEAR_MASK],node);
	} 
	else
	{

		//TIME_NEAR 256 =100000000    TIME_LEVEL_SHIFT  6
		int i;
		//maskΪ 100 0000 0000 0000
		uint32_t mask=TIME_NEAR << TIME_LEVEL_SHIFT;  
		
		for (i=0;i<3;i++) {
			//mask-1      11 1111 1111 1111  ������ǰ14λ
			if ((time|(mask-1))==(current_time|(mask-1))) {
				break;
			}
	// 6    10000 0000 0000 0000 0000   10000 0000 0000 0000 0000 000000  
			// 10000 0000 0000 0000 0000 0000 0000 0000 
			// ����   ǰ  14 20   26 
			mask <<= TIME_LEVEL_SHIFT;
		}

		//���뵽struct link_list t[4][TIME_LEVEL];		
		link(&T->t[i][((time>>(TIME_NEAR_SHIFT + i*TIME_LEVEL_SHIFT)) & TIME_LEVEL_MASK)],node);	
	}
}


//����time�� ����timer_node���뵽timer�е�link_list��
//time�����ʱ�� ��ʱʱ��
static void
timer_add(struct timer *T,void *arg,size_t sz,int time) {

	//����mode�ڵ�,ע�������ڴ��� sizeof(*node)+sz,��timer_nodeҪ��
	struct timer_node *node = (struct timer_node *)skynet_malloc(sizeof(*node)+sz);

	//���ڴ���뵽���棬����time_event
	memcpy(node+1,arg,sz);

	//����
	SPIN_LOCK(T);

		node->expire=time+T->time;
		add_node(T,node);

	SPIN_UNLOCK(T);
}

//�ƶ�timer_node,����struct link_list t��ά�����е�ĳ��Ԫ��ָ�������Ľڵ����µ���add_node
static void
move_list(struct timer *T, int level, int idx) {

	//�õ�T->t[level][idx]�洢��ʱ��ڵ������ͷָ��
	struct timer_node *current = link_clear(&T->t[level][idx]);

	//����������,  ��timer_node���뵽timer�� �ʺϵ�link_list��
	while (current) {
		struct timer_node *temp=current->next;
		add_node(T,current);
		current=temp;
	}
}


//ƫ�ƶ�ʱ�����ҷַ���ʱ����Ϣ����ʱ��Ǩ�Ƶ����Ϸ�������λ��
static void
timer_shift(struct timer *T) {
	//255
	int mask = TIME_NEAR;
	
	uint32_t ct = ++T->time;

	if (ct == 0) {
		move_list(T, 3, 0);
	} else {
		// TIME_NEAR_SHIFT Ϊ 8
		uint32_t time = ct >> TIME_NEAR_SHIFT;
		int i=0;

		while ((ct & (mask-1))==0) {
			int idx=time & TIME_LEVEL_MASK;
			if (idx!=0) {

				//���洢��T �� t�е�ʱ��ڵ����¼��뵽timer��Ӧ�������� 
				//���������ʱ���Ὣ��ʱ�Ľڵ�����near�� 
				move_list(T, i, idx);

				break;				
			}
			
			mask <<= TIME_LEVEL_SHIFT;
			time >>= TIME_LEVEL_SHIFT;
			++i;
		}
	}
}


//����currentʱ��ڵ����������е�����ʱ��ڵ㣬
static inline void
dispatch_list(struct timer_node *current) {
	do {

		//�õ�time_node�е�time_event�ṹ��
		struct timer_event * event = (struct timer_event *)(current+1);
		
		struct skynet_message message;
		
		message.source = 0;
		message.session = event->session;
		message.data = NULL;
		message.sz = (size_t)PTYPE_RESPONSE << MESSAGE_TYPE_SHIFT;

	//����Ϣ���͵���Ӧ��handleȥ����,�����ǽ�message���뵽handle��Ӧ��skynet_context����Ϣ������
		skynet_context_push(event->handle, &message);
		
		struct timer_node * temp = current;
		current=current->next;
		skynet_free(temp);	
	} while (current);
}


//�ӳ�ʱ�б���ȡ����ʱ����Ϣ���ַ�
static inline void
timer_execute(struct timer *T) {
	int idx = T->time & TIME_NEAR_MASK;
	
	while (T->near[idx].head.next) {
		
		struct timer_node *current = link_clear(&T->near[idx]);

		SPIN_UNLOCK(T);
		// dispatch_list don't need lock T
		//���������е�����ʱ��ڵ㣬��skynet_context������Ϣ
		dispatch_list(current);
		SPIN_LOCK(T);
	}
}


//ʱ��ÿ��һ���δ�ִ��һ�θú���
static void 
timer_update(struct timer *T) {
	SPIN_LOCK(T);

	// try to dispatch timeout 0 (rare condition)
	//�ӳ�ʱ�б���ȡ����ʱ����Ϣ���ַ�
	timer_execute(T);

	// shift time first, and then dispatch timer message
	
	//ƫ�ƶ�ʱ�����ҷַ���ʱ����Ϣ����ʱ��Ǩ�Ƶ����Ϸ�������λ��
	//�Ὣ��ʱ��ʱ��ڵ�����near��
	timer_shift(T);

	//�ӳ�ʱ�б���ȡ����ʱ����Ϣ���ַ�
	timer_execute(T);

	SPIN_UNLOCK(T);
}

//����timer�ṹ��
static struct timer *
timer_create_timer() {
	//�����ڴ�
	struct timer *r=(struct timer *)skynet_malloc(sizeof(struct timer));
	memset(r,0,sizeof(*r));

	int i,j;

	//��ʼ������
	for (i=0;i<TIME_NEAR;i++) {
		link_clear(&r->near[i]);
	}

	for (i=0;i<4;i++) {
		for (j=0;j<TIME_LEVEL;j++) {
			link_clear(&r->t[i][j]);
		}
	}

	SPIN_INIT(r)

	r->current = 0;

	return r;
}

//���붨ʱ����time�ĵ�λ��0.01�룬��time=300,��ʾ3��
//time�����ʱ��
int
skynet_timeout(uint32_t handle, int time, int session) {

	//time<=0, ����ʱʱ��Ϊ0 ������handle��Ӧ��skynet_context������Ϣ
	if (time <= 0) {
		struct skynet_message message;
		message.source = 0;
		message.session = session;
		message.data = NULL;
		message.sz = (size_t)PTYPE_RESPONSE << MESSAGE_TYPE_SHIFT;

		//��handle��Ӧ��skynet_context������Ϣ
		if (skynet_context_push(handle, &message)) {
			return -1;
		}
	} 
	else
	{ 
		struct timer_event event;
		event.handle = handle;
		event.session = session;
		//�Ὣevent���뵽ʱ��ڵ�ṹ���ڴ���棬
		timer_add(TI, &event, sizeof(event), time);
	}

	return session;
}

// centisecond: 1/100 second

//�õ�ϵͳʱ�� ��λ�� 0.01��
static void
systime(uint32_t *sec, uint32_t *cs) {
#if !defined(__APPLE__)

	struct timespec ti;
	clock_gettime(CLOCK_REALTIME, &ti);

	//��  
	*sec = (uint32_t)ti.tv_sec;

	//����
	*cs = (uint32_t)(ti.tv_nsec / 10000000);
#else
	struct timeval tv;
	gettimeofday(&tv, NULL);
	//��
	*sec = tv.tv_sec;
	//΢��
	*cs = tv.tv_usec / 10000;
#endif
}


//����ϵͳ���������ڵ�ʱ�䣬��λ�� 0.01��
static uint64_t
gettime() {
	uint64_t t;
#if !defined(__APPLE__)
	struct timespec ti;
	clock_gettime(CLOCK_MONOTONIC, &ti);
	t = (uint64_t)ti.tv_sec * 100;
	t += ti.tv_nsec / 10000000;
#else
	struct timeval tv;
	gettimeofday(&tv, NULL);
	//tv_sec��ʾ ��,
	t = (uint64_t)tv.tv_sec * 100;
	//΢��,����ɵ�λ�� 0.01��
	t += tv.tv_usec / 10000;
#endif
	return t;
}



//��ǰ����ȫ�ֱ���  static struct timer * TI = NULL;


//��ʱ���߳��е���
void
skynet_updatetime(void) {

	//��ȡ��ǰ��׼ȷʱ��,��λ��0.01��
	uint64_t cp = gettime();
	
	if(cp < TI->current_point) 
	{	
		//ʱ�����һ�㲻���ܳ���
		skynet_error(NULL, "time diff error: change from %lld to %lld", cp, TI->current_point);
		TI->current_point = cp;
	} 
	else if (cp != TI->current_point) //�����ǰʱ������ϴε���skynet_updatetime��ʱ��
	{
		
		//�õ�������ʱ���ֵ
		uint32_t diff = (uint32_t)(cp - TI->current_point);

		//����ʱ��
		TI->current_point = cp;

		//�������ʱ��current
		TI->current += diff;
		
		int i;
		for (i=0;i<diff;i++)
		{
			//�ᴦ��ʱ
			timer_update(TI);
		}
	}
}

uint32_t
skynet_starttime(void) {
	return TI->starttime;
}

uint64_t 
skynet_now(void) {
	return TI->current;
}

//��ʼ��timer�ṹ��,���Ⱦͱ�����
void 
skynet_timer_init(void) {
	//�����ڴ�
	TI = timer_create_timer();
	
	uint32_t current = 0;

	//�õ���ǰʱ��,
	//starttime������ǵ�ǰʱ�������(�����1970.)
	//current��������������ϵ�΢����
	//����+΢����/10^6=�õ���׼ȷʱ��(��)
	//���ߵĵ�λ�ǲ�ͬ�ģ�һ�����룬��һ����΢��

	//current��΢����*10000,��ΪԼ����λʱ����0.01��
	systime(&TI->starttime, &current);

	//��ǰʱ���΢��������
	TI->current = current;

	//׼ȷ��ʱ�䣬���� ����+΢����
	TI->current_point = gettime();
}

// for profile

#define NANOSEC 1000000000
#define MICROSEC 1000000

//�õ���ǰʱ���΢����
uint64_t
skynet_thread_time(void) {
#if  !defined(__APPLE__)

	struct timespec ti;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ti);

	return (uint64_t)ti.tv_sec * MICROSEC + (uint64_t)ti.tv_nsec / (NANOSEC / MICROSEC);
#else
	struct task_thread_times_info aTaskInfo;
	mach_msg_type_number_t aTaskInfoCount = TASK_THREAD_TIMES_INFO_COUNT;
	if (KERN_SUCCESS != task_info(mach_task_self(), TASK_THREAD_TIMES_INFO, (task_info_t )&aTaskInfo, &aTaskInfoCount)) {
		return 0;
	}

	return (uint64_t)(aTaskInfo.user_time.seconds) + (uint64_t)aTaskInfo.user_time.microseconds;
#endif
}


