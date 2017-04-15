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

//skynet 定时器的实现为linux内核的标准做法  精度为 0.01s 对游戏一般来说够了 高精度的定时器很费CPU


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


//时间节点
struct timer_node {
	struct timer_node *next;
	uint32_t expire;		// 超时滴答计数 即超时间隔
};

//时间节点队列
struct link_list {
	struct timer_node head;
	struct timer_node *tail;
};


//对时间事件管理的抽象结构 
struct timer {

	//near存放的是添加时间节点的时后就是当前时间，即不需要等待的时间节点 

	//TIME_NEAR   256
	struct link_list near[TIME_NEAR];		// 定时器容器组 存放了不同的定时器容器

	//TIME_LEVEL  64
	struct link_list t[4][TIME_LEVEL];		// 4级梯队，4级不同的定时器
	
	struct spinlock lock;				//锁，可能是pthread_mutex_t  或原子操作

	uint32_t time;						//当前已经流过的滴答计数

	//如果启动时间是  xxxxxx秒 yyyy微秒

	//化成单位是 0.01秒 
	//则starttime=xxxxxx*100
	uint32_t starttime;					//开机启动事件(绝对时间)

	//current = yyyy / 10000
	uint64_t current;					//当前时间，现对于系统的开机时间(相对时间)

	//运行框架后，初始化时间结构体时的准确时间，单位是0.01秒
	//或在时间线程中每次调用 skynet_updatetime()时的时间
	uint64_t current_point;
};

static struct timer * TI = NULL;


//清除link_list,即将link_list.head=0 ，返回原来链表的第一节点指针
static inline struct timer_node *
link_clear(struct link_list *list) {
	struct timer_node * ret = list->head.next;
	list->head.next = 0;
	list->tail = &(list->head);

	return ret;
}

//将node加入到链表link_list尾部
static inline void
link(struct link_list *list,struct timer_node *node) {
	list->tail->next = node;
	list->tail = node;
	node->next=0;
}


//将timer_node加入到timer中 适合的link_list中
static void
add_node(struct timer *T,struct timer_node *node) {
	uint32_t time=node->expire;	//超时的滴答数
	uint32_t current_time=T->time;

	//如果就是当前时间，没有超时  TIME_NEAR_MASK   255    11111111   屏蔽前8位
	if ((time|TIME_NEAR_MASK)==(current_time|TIME_NEAR_MASK)) {

		//将node加入到near中  struct link_list near[TIME_NEAR];	
		//存放的链表的位置是 time&TIME_NEAR_MASK  即 time&1111111  即取time的前8位  
		link(&T->near[time&TIME_NEAR_MASK],node);
	} 
	else
	{

		//TIME_NEAR 256 =100000000    TIME_LEVEL_SHIFT  6
		int i;
		//mask为 100 0000 0000 0000
		uint32_t mask=TIME_NEAR << TIME_LEVEL_SHIFT;  
		
		for (i=0;i<3;i++) {
			//mask-1      11 1111 1111 1111  即屏蔽前14位
			if ((time|(mask-1))==(current_time|(mask-1))) {
				break;
			}
	// 6    10000 0000 0000 0000 0000   10000 0000 0000 0000 0000 000000  
			// 10000 0000 0000 0000 0000 0000 0000 0000 
			// 屏蔽   前  14 20   26 
			mask <<= TIME_LEVEL_SHIFT;
		}

		//加入到struct link_list t[4][TIME_LEVEL];		
		link(&T->t[i][((time>>(TIME_NEAR_SHIFT + i*TIME_LEVEL_SHIFT)) & TIME_LEVEL_MASK)],node);	
	}
}


//根据time等 创建timer_node加入到timer中的link_list中
//time是相对时间 超时时间
static void
timer_add(struct timer *T,void *arg,size_t sz,int time) {

	//创建mode节点,注意分配的内存是 sizeof(*node)+sz,比timer_node要大
	struct timer_node *node = (struct timer_node *)skynet_malloc(sizeof(*node)+sz);

	//将内存加入到后面，就是time_event
	memcpy(node+1,arg,sz);

	//加锁
	SPIN_LOCK(T);

		node->expire=time+T->time;
		add_node(T,node);

	SPIN_UNLOCK(T);
}

//移动timer_node,即将struct link_list t二维数组中的某个元素指向的链表的节点重新调用add_node
static void
move_list(struct timer *T, int level, int idx) {

	//得到T->t[level][idx]存储的时间节点链表的头指针
	struct timer_node *current = link_clear(&T->t[level][idx]);

	//遍历该链表,  将timer_node加入到timer中 适合的link_list中
	while (current) {
		struct timer_node *temp=current->next;
		add_node(T,current);
		current=temp;
	}
}


//偏移定时器并且分发定时器消息，定时器迁移到它合法的容器位置
static void
timer_shift(struct timer *T) {
	//255
	int mask = TIME_NEAR;
	
	uint32_t ct = ++T->time;

	if (ct == 0) {
		move_list(T, 3, 0);
	} else {
		// TIME_NEAR_SHIFT 为 8
		uint32_t time = ct >> TIME_NEAR_SHIFT;
		int i=0;

		while ((ct & (mask-1))==0) {
			int idx=time & TIME_LEVEL_MASK;
			if (idx!=0) {

				//将存储在T 的 t中的时间节点重新加入到timer相应的容器中 
				//在重新添加时，会将到时的节点存放在near中 
				move_list(T, i, idx);

				break;				
			}
			
			mask <<= TIME_LEVEL_SHIFT;
			time >>= TIME_LEVEL_SHIFT;
			++i;
		}
	}
}


//处理current时间节点所在链表中的所有时间节点，
static inline void
dispatch_list(struct timer_node *current) {
	do {

		//得到time_node中的time_event结构体
		struct timer_event * event = (struct timer_event *)(current+1);
		
		struct skynet_message message;
		
		message.source = 0;
		message.session = event->session;
		message.data = NULL;
		message.sz = (size_t)PTYPE_RESPONSE << MESSAGE_TYPE_SHIFT;

	//将消息发送到对应的handle去处理,即就是将message加入到handle对应的skynet_context的消息队列中
		skynet_context_push(event->handle, &message);
		
		struct timer_node * temp = current;
		current=current->next;
		skynet_free(temp);	
	} while (current);
}


//从超时列表中取出到时的消息来分发
static inline void
timer_execute(struct timer *T) {
	int idx = T->time & TIME_NEAR_MASK;
	
	while (T->near[idx].head.next) {
		
		struct timer_node *current = link_clear(&T->near[idx]);

		SPIN_UNLOCK(T);
		// dispatch_list don't need lock T
		//处理链表中的所有时间节点，向skynet_context发送消息
		dispatch_list(current);
		SPIN_LOCK(T);
	}
}


//时间每过一个滴答，执行一次该函数
static void 
timer_update(struct timer *T) {
	SPIN_LOCK(T);

	// try to dispatch timeout 0 (rare condition)
	//从超时列表中取出到时的消息来分发
	timer_execute(T);

	// shift time first, and then dispatch timer message
	
	//偏移定时器并且分发定时器消息，定时器迁移到它合法的容器位置
	//会将超时的时间节点存放在near中
	timer_shift(T);

	//从超时列表中取出到时的消息来分发
	timer_execute(T);

	SPIN_UNLOCK(T);
}

//创建timer结构体
static struct timer *
timer_create_timer() {
	//分配内存
	struct timer *r=(struct timer *)skynet_malloc(sizeof(struct timer));
	memset(r,0,sizeof(*r));

	int i,j;

	//初始化容器
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

//插入定时器，time的单位是0.01秒，如time=300,表示3秒
//time是相对时间
int
skynet_timeout(uint32_t handle, int time, int session) {

	//time<=0, 即定时时间为0 立即向handle对应的skynet_context发送消息
	if (time <= 0) {
		struct skynet_message message;
		message.source = 0;
		message.session = session;
		message.data = NULL;
		message.sz = (size_t)PTYPE_RESPONSE << MESSAGE_TYPE_SHIFT;

		//向handle对应的skynet_context发送消息
		if (skynet_context_push(handle, &message)) {
			return -1;
		}
	} 
	else
	{ 
		struct timer_event event;
		event.handle = handle;
		event.session = session;
		//会将event加入到时间节点结构体内存后面，
		timer_add(TI, &event, sizeof(event), time);
	}

	return session;
}

// centisecond: 1/100 second

//得到系统时间 单位是 0.01秒
static void
systime(uint32_t *sec, uint32_t *cs) {
#if !defined(__APPLE__)

	struct timespec ti;
	clock_gettime(CLOCK_REALTIME, &ti);

	//秒  
	*sec = (uint32_t)ti.tv_sec;

	//纳秒
	*cs = (uint32_t)(ti.tv_nsec / 10000000);
#else
	struct timeval tv;
	gettimeofday(&tv, NULL);
	//秒
	*sec = tv.tv_sec;
	//微秒
	*cs = tv.tv_usec / 10000;
#endif
}


//返回系统开机到现在的时间，单位是 0.01秒
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
	//tv_sec表示 秒,
	t = (uint64_t)tv.tv_sec * 100;
	//微秒,换算成单位是 0.01秒
	t += tv.tv_usec / 10000;
#endif
	return t;
}



//在前面有全局变量  static struct timer * TI = NULL;


//在时间线程中调用
void
skynet_updatetime(void) {

	//获取当前的准确时间,单位是0.01秒
	uint64_t cp = gettime();
	
	if(cp < TI->current_point) 
	{	
		//时间错误，一般不可能出现
		skynet_error(NULL, "time diff error: change from %lld to %lld", cp, TI->current_point);
		TI->current_point = cp;
	} 
	else if (cp != TI->current_point) //如果当前时间大于上次调用skynet_updatetime的时间
	{
		
		//得到流经的时间差值
		uint32_t diff = (uint32_t)(cp - TI->current_point);

		//更新时间
		TI->current_point = cp;

		//更新相对时间current
		TI->current += diff;
		
		int i;
		for (i=0;i<diff;i++)
		{
			//会处理超时
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

//初始化timer结构体,首先就被调用
void 
skynet_timer_init(void) {
	//分配内存
	TI = timer_create_timer();
	
	uint32_t current = 0;

	//得到当前时间,
	//starttime保存的是当前时间的秒数(相对于1970.)
	//current保存的是在秒数上的微秒数
	//秒数+微秒数/10^6=得到的准确时间(秒)
	//两者的单位是不同的，一个是秒，另一个是微秒

	//current是微秒数*10000,因为约定单位时间是0.01秒
	systime(&TI->starttime, &current);

	//当前时间的微秒数部分
	TI->current = current;

	//准确的时间，就是 秒数+微秒数
	TI->current_point = gettime();
}

// for profile

#define NANOSEC 1000000000
#define MICROSEC 1000000

//得到当前时间的微秒数
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


