#include "skynet.h"

#include "skynet_handle.h"
#include "skynet_server.h"
#include "rwlock.h"

#include <stdlib.h>
#include <assert.h>
#include <string.h>

#define DEFAULT_SLOT_SIZE 4
#define MAX_SLOT_SIZE 0x40000000


//skynet 服务号的管理和分配

//handle_name 服务 和对应 handle id的hash表
struct handle_name {
	char * name;

	//handle是一个uint32_t的整数，高八位表示远程节点(这是框架自带的集群设施，后面的分析都会无视该部分
	// 一来，他不是框架的核心，二来这个集群设施已经不被推荐使用)
	uint32_t handle;
};



//存储handler与skynet_contex的对应关系，是一个哈希表
//每个服务skynet_context都对应一个不重复的handler
//通过handler变可以获取skynet_context
//保存了 handler 和 skynet_context的对应


//实质就是管理一个 skynet_context 的指针数组 ,还有一个handle_name数组
struct handle_storage {

	//读写锁
	struct rwlock lock;

	//服务所属 harbor harbor 用于不同主机间通信 
	uint32_t harbor;

	//初始值为1，表示handler句柄起始值从1开始
	uint32_t handle_index;

	//slot数组的大小 
	int slot_size;

	//指针数组，保存skynet_context指针
	struct skynet_context ** slot;	//skynet_context 表空间

	//handler_name容量，初始为2，这里name_cap与slot_size不一样的原因在于，不是每一个handler都有name
	int name_cap;

	
	int name_count;		//handler_name数量

	//存放handle_name的数组	
	struct handle_name *name;	//handler_name表 数组
};

//创建全局变量
static struct handle_storage *H = NULL;

//注册ctx 将ctx保存到 handler_storage哈希表中，并得到一个handler

//实质就是将ctx存入到handle_storage维护的一个指针数组中
uint32_t
skynet_handle_register(struct skynet_context  *ctx) {

	struct handle_storage *s = H;

	//加写锁
	rwlock_wlock(&s->lock);
	
	for (;;) {
		int i;

		//hash选值
		for (i=0;i<s->slot_size;i++) {

			//handle是一个uint32_t的整数，高八位表示远程节点(这是框架自带的集群设施，后面的分析都会无视该部分

			             //#define HANDLE_MASK 0xffffff   就是 00000000 11111111 11111111 11111111
			             //x&HANDLE_MASK 是将x的高八位变成0
			uint32_t handle = (i+s->handle_index) & HANDLE_MASK; 

			//等价于handler % s->slot_size
			int hash = handle & (s->slot_size-1);
			
			if (s->slot[hash] == NULL) {	//找到未使用的 slot 将这个ctx放入到这个slot中 
				
				s->slot[hash] = ctx;
				
				s->handle_index = handle + 1;	//移动 handler_index 方便下次使用

				rwlock_wunlock(&s->lock);

				//harbor 用于不同主机之间的通信，handler高8位用于harbor 低24为用于本机的
				//所以这里要 |=一下
				
				handle |= s->harbor;
				return handle;
			}
		}

		//运行到这里，说明数组满了，
		//确保扩大 2倍空间之后 总共handler即 slot的数量不超过 24位限制
		assert((s->slot_size*2 - 1) <= HANDLE_MASK);

		//哈希表扩大两倍
		struct skynet_context ** new_slot = skynet_malloc(s->slot_size * 2 * sizeof(struct skynet_context *));

		memset(new_slot, 0, s->slot_size * 2 * sizeof(struct skynet_context *));

		//将原来的数据拷贝到新空间
		for (i=0;i<s->slot_size;i++) {

			//映射新的hash值
			int hash = skynet_context_handle(s->slot[i]) & (s->slot_size * 2 - 1);
			assert(new_slot[hash] == NULL);
			new_slot[hash] = s->slot[i];
		}
		//销毁原来的指针数组
		skynet_free(s->slot);
		s->slot = new_slot;
		s->slot_size *= 2;
	}
}


//收回handle
//做用是解除handle映射，而不是递减引用计数
/*
步骤
1.清空handle对应的槽,调用skynet_context_release
2.如果有注册命名，删除对应的节点

*/

//实质就是根据下标在指针数组中 删除指针
int                //handle是对应的一个下标
skynet_handle_retire(uint32_t handle) {
	int ret = 0;
	struct handle_storage *s = H;

	//加锁
	rwlock_wlock(&s->lock);

	//等价于 handler % s->slot_size
	//得到下标
	uint32_t hash = handle & (s->slot_size-1);

	//得到指针数组中存放的ctx
	struct skynet_context * ctx = s->slot[hash];

						//skynet_context_handle 函数返回   ctx->handle;
	if (ctx != NULL && skynet_context_handle(ctx) == handle) {

		//置空，哈希表腾出空间
		s->slot[hash] = NULL;
		ret = 1;
		int i;
		int j=0, n=s->name_count;

		//遍历 handler_name表
		for (i=0; i<n; ++i) {

			
			//2.如果有注册命名，删除对应的节点
			//在name表中找到 handler对应的name  调用skynet_free
			if (s->name[i].handle == handle) {
				skynet_free(s->name[i].name);
				continue;
			} else if (i!=j) {
				//说明free了一个name
				
				s->name[j] = s->name[i];	//因此需要将后续元素移到前面	
			}
			++j;
		}
		//修改名称的数量
		s->name_count = j;
	} else {
		ctx = NULL;
	}

	rwlock_wunlock(&s->lock);

	if (ctx) {
		// release ctx may call skynet_handle_* , so wunlock first.
		//将引用计数减一 , 减为0时则删除skynet_context
		skynet_context_release(ctx);
	}

	return ret;
}



//回收所有的handle
//回收handle_storage中的指针数组中的所有skynet_context指针
void 
skynet_handle_retireall() {
	struct handle_storage *s = H;
	for (;;) {
		int n=0;
		int i;
		for (i=0;i<s->slot_size;i++) {
			rwlock_rlock(&s->lock);
			struct skynet_context * ctx = s->slot[i];
			uint32_t handle = 0;
			if (ctx)
				handle = skynet_context_handle(ctx);
			rwlock_runlock(&s->lock);
			
			if (handle != 0) {

				//
				if (skynet_handle_retire(handle)) {
					++n;
				}
			}
		}
		if (n==0)
			return;
	}
}

//通过handle获取skynet_context*,skynet_context的引用计数加1
struct skynet_context * 
skynet_handle_grab(uint32_t handle) {
	struct handle_storage *s = H;
	struct skynet_context * result = NULL;

	//加读锁
	rwlock_rlock(&s->lock);

	//得到下标
	uint32_t hash = handle & (s->slot_size-1);
	
	struct skynet_context * ctx = s->slot[hash];
	if (ctx && skynet_context_handle(ctx) == handle) {
		result = ctx;

			//引用计数加1
		skynet_context_grab(result);
	}

	rwlock_runlock(&s->lock);

	return result;
}

//根据名称查找handle
uint32_t 
skynet_handle_findname(const char * name) {
	struct handle_storage *s = H;

	rwlock_rlock(&s->lock);

	uint32_t handle = 0;

	int begin = 0;
	int end = s->name_count - 1;

	//这里使用二分查找，听说会有整数溢出，。。。。
	while (begin<=end) {
		
		int mid = (begin+end)/2;
		struct handle_name *n = &s->name[mid];
		int c = strcmp(n->name, name);			//一直在头部插入，实际这样插入后name会按长度大小排序，这样就能使用二分查找了
		
		if (c==0) {
			handle = n->handle;
			break;
		}
		if (c<0) {
			begin = mid + 1;
		} else {
			end = mid - 1;
		}
	}

	rwlock_runlock(&s->lock);

	return handle;
}


//在handle_storage的name数组的before位置插入一个元素
static void
_insert_name_before(struct handle_storage *s, char *name, uint32_t handle, int before) {

	//如果数量和容量相同,需要扩容
	if (s->name_count >= s->name_cap) {
		//将容量扩大两倍
		s->name_cap *= 2;
		assert(s->name_cap <= MAX_SLOT_SIZE);

		//将容量扩大两倍
		struct handle_name * n = skynet_malloc(s->name_cap * sizeof(struct handle_name));
		int i;
		
		//将[0,before)之间的元素复制到新开辟的空间的[0,before)
		for (i=0;i<before;i++) {
			n[i] = s->name[i];
		}
		
		//将[before,name_count)之间的元素复制到新空间的[before+1,name_count+1)中
		//这样就将before位置空出来
		for (i=before;i<s->name_count;i++) {
			n[i+1] = s->name[i];
		}

		
		//销毁原来的数组
		skynet_free(s->name);
		
		s->name = n;
	} else {
		int i;

		//before 之后的元素后移一个位置
		for (i=s->name_count;i>before;i--) {
			s->name[i] = s->name[i-1];
		}
	}
	
	//在name数组的before位置加入一个元素
	s->name[before].name = name;
	s->name[before].handle = handle;
	s->name_count ++;
}


//插入name和handle
static const char *
_insert_name(struct handle_storage *s, const char * name, uint32_t handle) {
	int begin = 0;
	int end = s->name_count - 1;

	//二分查找
	while (begin<=end) {
		int mid = (begin+end)/2;
		struct handle_name *n = &s->name[mid];

		//如果要插入的name已经存在
		int c = strcmp(n->name, name);
		if (c==0) {
			return NULL;    //名称已经存在，这里名称不能重复插入
		}
		if (c<0) {
			begin = mid + 1;
		} else {
			end = mid - 1;
		}
	}
	
	char * result = skynet_strdup(name);

	//一直在头部插入，实际这样插入后name会按长度大小排序，这样就能使用二分查找了
	//result是名字  
	_insert_name_before(s, result, handle, begin);

	return result;
}

//name与handle绑定
//给服务注册一个名称的适合会用到该函数

//实质就是将handler,name插入到handle_storage的name数组中
const char * 
skynet_handle_namehandle(uint32_t handle, const char *name) {
	rwlock_wlock(&H->lock);

	const char * ret = _insert_name(H, name, handle);

	rwlock_wunlock(&H->lock);

	return ret;
}

//初始化一个handler 就是初始化handler_storage,一个存储skynet_context指针的数组
void 
skynet_handle_init(int harbor) {
	assert(H==NULL);
	struct handle_storage * s = skynet_malloc(sizeof(*H));

	//表空间的大小 DEFAULT_SLOT_SIZE   4
	s->slot_size = DEFAULT_SLOT_SIZE;

	//为skynet_ctx*数组分配空间
	s->slot = skynet_malloc(s->slot_size * sizeof(struct skynet_context *));
	memset(s->slot, 0, s->slot_size * sizeof(struct skynet_context *));

	rwlock_init(&s->lock);
	// reserve 0 for system
	s->harbor = (uint32_t) (harbor & 0xff) << HANDLE_REMOTE_SHIFT;

	//handle句柄从1开始，0保留，
	s->handle_index = 1;

	//名字容量初始为2
	s->name_cap = 2;
	s->name_count = 0;
	//为name数组分配空间
	s->name = skynet_malloc(s->name_cap * sizeof(struct handle_name));

	H = s;

	// Don't need to free H
}

