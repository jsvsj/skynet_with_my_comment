#include "skynet.h"

#include "skynet_monitor.h"
#include "skynet_server.h"
#include "skynet.h"
#include "atomic.h"

#include <stdlib.h>
#include <string.h>


//�����̼߳�ر���
struct skynet_monitor {
	int version;
	int check_version;
	
	uint32_t source;
	uint32_t destination;
};

//����skynet_monitor�ṹ��
struct skynet_monitor * 
skynet_monitor_new() {
	struct skynet_monitor * ret = skynet_malloc(sizeof(*ret));
	memset(ret, 0, sizeof(*ret));
	return ret;
}

//����skynet_monitor�ṹ��
void 
skynet_monitor_delete(struct skynet_monitor *sm) {
	skynet_free(sm);
}



//������һ��monitor,�����������������Ϣ�����Ƿ�������ѭ��������Ҳֻ�����һ��lig����һ��
//�������Ƿ���һ��ר�ŵļ���߳������ģ��ж���ѭ����ʱ����5��
void 
skynet_monitor_trigger(struct skynet_monitor *sm, uint32_t source, uint32_t destination) {
	sm->source = source;
	sm->destination = destination;
	
	//#define ATOM_INC(ptr) __sync_add_and_fetch(ptr, 1)  �ȼ�1,�ٻ�ȡ��1���ֵ

	//���ǽ�version+1
	ATOM_INC(&sm->version);
}

void 
skynet_monitor_check(struct skynet_monitor *sm) {
	if (sm->version == sm->check_version) {
		if (sm->destination) {
			//����ctx->endless = true;   destination��Ӧһ��skynet_context
			skynet_context_endless(sm->destination);
			skynet_error(NULL, "A message from [ :%08x ] to [ :%08x ] maybe in an endless loop (version = %d)", sm->source , sm->destination, sm->version);
		}
	} else {
		sm->check_version = sm->version;
	}
}
