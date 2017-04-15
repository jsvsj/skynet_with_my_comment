#ifndef SKYNET_HARBOR_H
#define SKYNET_HARBOR_H

#include <stdint.h>
#include <stdlib.h>

#define GLOBALNAME_LENGTH 16	//全局名字的长度
#define REMOTE_MAX 256


//远程服务名对应的handle
struct remote_name {
	char name[GLOBALNAME_LENGTH];
	uint32_t handle;
};


//远程消息
struct remote_message {
	struct remote_name destination;
	const void * message;
	size_t sz;
};

//向远程服务发送消息
void skynet_harbor_send(struct remote_message *rmsg, uint32_t source, int session);


//向master注册服务名  master用来统一管理所以节点
int skynet_harbor_message_isremote(uint32_t handle);

void skynet_harbor_init(int harbor);

void skynet_harbor_start(void * ctx);

void skynet_harbor_exit();

#endif
