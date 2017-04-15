#include "skynet.h"

#include "skynet_monitor.h"
#include "skynet_server.h"
#include "skynet.h"
#include "atomic.h"

#include <stdlib.h>
#include <string.h>


//工作线程监控表项
struct skynet_monitor {
	int version;
	int check_version;
	
	uint32_t source;
	uint32_t destination;
};

//创建skynet_monitor结构体
struct skynet_monitor * 
skynet_monitor_new() {
	struct skynet_monitor * ret = skynet_malloc(sizeof(*ret));
	memset(ret, 0, sizeof(*ret));
	return ret;
}

//销毁skynet_monitor结构体
void 
skynet_monitor_delete(struct skynet_monitor *sm) {
	skynet_free(sm);
}



//触发了一下monitor,这个监控是用来检测消息处理是否发生了死循环，不过也只是输出一条lig提醒一下
//这个检测是放在一个专门的监控线程里做的，判断死循环的时间是5秒
void 
skynet_monitor_trigger(struct skynet_monitor *sm, uint32_t source, uint32_t destination) {
	sm->source = source;
	sm->destination = destination;
	
	//#define ATOM_INC(ptr) __sync_add_and_fetch(ptr, 1)  先加1,再获取加1后的值

	//就是将version+1
	ATOM_INC(&sm->version);
}

void 
skynet_monitor_check(struct skynet_monitor *sm) {
	if (sm->version == sm->check_version) {
		if (sm->destination) {
			//设置ctx->endless = true;   destination对应一个skynet_context
			skynet_context_endless(sm->destination);
			skynet_error(NULL, "A message from [ :%08x ] to [ :%08x ] maybe in an endless loop (version = %d)", sm->source , sm->destination, sm->version);
		}
	} else {
		sm->check_version = sm->version;
	}
}
