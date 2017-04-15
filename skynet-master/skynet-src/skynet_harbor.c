#include "skynet.h"
#include "skynet_harbor.h"
#include "skynet_server.h"
#include "skynet_mq.h"
#include "skynet_handle.h"

#include <string.h>
#include <stdio.h>
#include <assert.h>

//harbor 用来与远程主机通信 master 统一来管理


//harbor 服务对于的 skynet_context指针
static struct skynet_context * REMOTE = 0;
static unsigned int HARBOR = ~0;



//向远程服务发送消息
void 
skynet_harbor_send(struct remote_message *rmsg, uint32_t source, int session) {
	int type = rmsg->sz >> MESSAGE_TYPE_SHIFT;
	rmsg->sz &= MESSAGE_TYPE_MASK;
	assert(type != PTYPE_SYSTEM && type != PTYPE_HARBOR && REMOTE);

	skynet_context_send(REMOTE, rmsg, sizeof(*rmsg) , source, type , session);
}


//判断消息是不是来自远程主机
int 
skynet_harbor_message_isremote(uint32_t handle) {

	assert(HARBOR != ~0);

	//取高八位   HANDLE_MASK   0xffffff    00000000 11111111 11111111 11111111
 	int h = (handle & ~HANDLE_MASK);
	return h != HARBOR && h !=0;
}

void
skynet_harbor_init(int harbor) {

	//高八位急救室对于远程主机通信的 harbor
	//                   HANDLE_REMOTE_SHIFT 24   将harbor左移24位
	HARBOR = (unsigned int)harbor << HANDLE_REMOTE_SHIFT;
}

void
skynet_harbor_start(void *ctx) {
	// the HARBOR must be reserved to ensure the pointer is valid.
	// It will be released at last by calling skynet_harbor_exit
	//节点对应的服务数减 1
	skynet_context_reserve(ctx);
	REMOTE = ctx;
}

void
skynet_harbor_exit() {
	struct skynet_context * ctx = REMOTE;
	REMOTE= NULL;
	if (ctx) {
		skynet_context_release(ctx);
	}
}
