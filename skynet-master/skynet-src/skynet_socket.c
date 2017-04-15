#include "skynet.h"

#include "skynet_socket.h"
#include "socket_server.h"
#include "skynet_server.h"
#include "skynet_mq.h"
#include "skynet_harbor.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

//全局的服务器socket连接部分的抽象的结构体
static struct socket_server * SOCKET_SERVER = NULL;


//初始化全局的SOCKET_SERVER,调用的 epoll_create(),等
void 
skynet_socket_init() {
	
	//创建socker_server结构体
	SOCKET_SERVER = socket_server_create();
}


//请求退出
void
skynet_socket_exit() {
	
	//请求退出,发送X命令
	socket_server_exit(SOCKET_SERVER);
}



//销毁socket_server结构体
void
skynet_socket_free() {
	socket_server_release(SOCKET_SERVER);
	SOCKET_SERVER = NULL;
}

// mainloop thread
//在skynet_socket_poll中被调用
//转发消息，即将消息发送到对应的skynet_context的消息队列中     result是传入参数
static void
forward_message(int type, bool padding, struct socket_message * result) {

/*
struct skynet_socket_message {
	int type;
	int id;
	int ud;
	//这样可以实现buffer的长度任意
	char * buffer;
};

*/
	struct skynet_socket_message *sm;

	size_t sz = sizeof(*sm);

	//如果还有数据,对于accept,数据是 连接的IP,真正的缓冲区在服务器对象的 buffer中，只不过result也指向了他，
	//这样做的好处是减少了内存符拷贝
	//即skynet_

	//即如果result->data中还有数据
	if (padding)
	{
		
		if (result->data)
		{
			size_t msg_sz = strlen(result->data);
			if (msg_sz > 128)
			{
				msg_sz = 128;
			}
			sz += msg_sz;
		}
		else
		{
			result->data = "";
		}
	}
	
	sm = (struct skynet_socket_message *)skynet_malloc(sz);
	
	sm->type = type;
	sm->id = result->id;
	sm->ud = result->ud;

	//如果有额外数据
	if (padding) {
		sm->buffer = NULL;
		memcpy(sm+1, result->data, sz - sizeof(*sm));
	} else {
		//指向了读到的数据
		sm->buffer = result->data;
	}

	//skynet_context中的消息类型
	
	struct skynet_message message;
	message.source = 0;
	message.session = 0;
	//读到的数据
	message.data = sm;
	message.sz = sz | ((size_t)PTYPE_SOCKET << MESSAGE_TYPE_SHIFT);


	// 将这个消息推送到对应的 handle 
	// 不要调用 skynet_socket_close 那会阻塞整个事件循环

	//opaque是handle,要传递给对应的skynet_context的标号
	if (skynet_context_push((uint32_t)result->opaque, &message)) {
		// todo: report somewhere to close socket
		// don't call skynet_socket_close here (It will block mainloop)
		skynet_free(sm->buffer);
		skynet_free(sm);
	}
}


// 调用socket_server_poll()函数得到消息向前传递消息

int 
skynet_socket_poll() {
	struct socket_server *ss = SOCKET_SERVER;
	assert(ss);

/*
struct socket_message {
	int id;				// 应用层的socket fd
	//在64位上uintptr_t是unsigned long int 的别名,在32位上，是unsigned int 的别名
	uintptr_t opaque;	// 在skynet中对应一个actor实体的handler

	// 对于accept连接来说是新连接的fd 对于数据到来是数据的大小
	int ud;	// for accept, ud is new connection id ; for data, ud is size of data 
	
	char * data;
};


*/


	//result是传出参数	
	struct socket_message result;

	int more = 1;

	//result是传入传出参数,more是传入传出参数   result能将读到的数据传出
	int type = socket_server_poll(ss, &result, &more);

	switch (type) {
	case SOCKET_EXIT:
		return 0;
		//如果是由数据到来
	case SOCKET_DATA:
		forward_message(SKYNET_SOCKET_TYPE_DATA, false, &result);
		break;
		//如果是关闭了套接字
	case SOCKET_CLOSE:
		forward_message(SKYNET_SOCKET_TYPE_CLOSE, false, &result);
		break;

		//连接建立（主动或者被动，并且已加入到epoll）
	case SOCKET_OPEN:
		forward_message(SKYNET_SOCKET_TYPE_CONNECT, true, &result);
		break;

	case SOCKET_ERROR:
		forward_message(SKYNET_SOCKET_TYPE_ERROR, true, &result);
		break;

	//监听套接字有事件到来，socket_server_poll已经就收的连接，即调用了accept函数
	//生成的描述符对象 struct socket指针已经存入到了 服务器对象 socket_server中的数组中，
	//不过描述符并没有加入到epoll中管理
	case SOCKET_ACCEPT:
		//将消息加入到了 消息队列中
		forward_message(SKYNET_SOCKET_TYPE_ACCEPT, true, &result);
		break;

	//udp套接字数据到来
	case SOCKET_UDP:
		forward_message(SKYNET_SOCKET_TYPE_UDP, false, &result);
		break;
	default:
		skynet_error(NULL, "Unknown socket message type %d.",type);
		return -1;
	}
	//当socket_server_poll()的调用的 sp_wait(),即epoll_wait()时，more会被修改为0
	//如果没有调用spoll_wait()则more为1，more会被修改为0,返回-1
	if (more) {
		return -1;
	}
	
	return 1;
}

//检查发送缓冲区数据大小，太大就发送警告
static int
check_wsz(struct skynet_context *ctx, int id, void *buffer, int64_t wsz) {

	if (wsz < 0) {
		return -1;
	}
	//如果剩余数据 > 1Mb
	else if (wsz > 1024 * 1024)
	{
		struct skynet_socket_message tmp;
		tmp.type = SKYNET_SOCKET_TYPE_WARNING;
		tmp.id = id;
		tmp.ud = (int)(wsz / 1024);
		tmp.buffer = NULL;
		skynet_send(ctx, 0, skynet_context_handle(ctx), PTYPE_SOCKET, 0 , &tmp, sizeof(tmp));
//		skynet_error(ctx, "%d Mb bytes on socket %d need to send out", (int)(wsz / (1024 * 1024)), id);
	}
	return 0;
}


//请求发送数据 对应的调用send_socket()函数, 发送D命令，
int
skynet_socket_send(struct skynet_context *ctx, int id, void *buffer, int sz) {
	//返回的是发送缓冲区中为剩余的数据数量
	
	int64_t wsz = socket_server_send(SOCKET_SERVER, id, buffer, sz);

	return check_wsz(ctx, id, buffer, wsz);
}


//请求发送数据  对应的调用 send_socket()函数,使用的是低优先级的缓冲区
void
skynet_socket_send_lowpriority(struct skynet_context *ctx, int id, void *buffer, int sz) {
	socket_server_send_lowpriority(SOCKET_SERVER, id, buffer, sz);
}

//socket()  bind()  listen()  存入socket数组   但没有加入到epoll中管理
int 
skynet_socket_listen(struct skynet_context *ctx, const char *host, int port, int backlog) {
	uint32_t source = skynet_context_handle(ctx);
	//socket()  bind()  listen()  存入socket数组   但没有加入到epoll中管理
	return socket_server_listen(SOCKET_SERVER, source, host, port, backlog);
}

//非阻塞的连接  对应的调用 open_socket()函数 向管道发送O命令 
int  
skynet_socket_connect(struct skynet_context *ctx, const char *host, int port) {
	uint32_t source = skynet_context_handle(ctx);
	//非阻塞的连接  对应的调用 open_socket()函数 向管道发送O命令 
	return socket_server_connect(SOCKET_SERVER, source, host, port);
}

//发送'B'命令请求
// 对应的调用 bind_socket 将描述符加入到socket_server的socket数组中 并将状态标记为 SOCKET_TYPE_BIND
int 
skynet_socket_bind(struct skynet_context *ctx, int fd) {
	uint32_t source = skynet_context_handle(ctx);

	//发送'B'命令请求
	// 对应的调用 bind_socket 将描述符加入到socket_server的socket数组中 并将状态标记为 SOCKET_TYPE_BIND
	return socket_server_bind(SOCKET_SERVER, source, fd);
}



//请求关闭  对应的调用 close_socket()
void 
skynet_socket_close(struct skynet_context *ctx, int id) {
	uint32_t source = skynet_context_handle(ctx);
	socket_server_close(SOCKET_SERVER, source, id);
}

//请求半关闭  对应的调用close_socket()
void 
skynet_socket_shutdown(struct skynet_context *ctx, int id) {
	uint32_t source = skynet_context_handle(ctx);
	socket_server_shutdown(SOCKET_SERVER, source, id);
}


//发送'S'命令请求
//对应的调用 start_socket 
//将SOCKET_TYPE_PACCEPT或者SOCKET_TYPE_PLISTEN这两种类型socket,将其加入epoll管理，并更新其状态
void 
skynet_socket_start(struct skynet_context *ctx, int id) {
	uint32_t source = skynet_context_handle(ctx);
	socket_server_start(SOCKET_SERVER, source, id);
}



//发送'T'命令请求
//对应的调用setopt_socket  调用setsockopt()设置描述符属性 
//这里是设置TCP_NODELAY属性
void
skynet_socket_nodelay(struct skynet_context *ctx, int id) {
	socket_server_nodelay(SOCKET_SERVER, id);
}



//发送'U'命令请求
//对应的调用add_udp_socket  将udp类型的  fd加入到socket_server的socket数组中,加入到epoll中管理
int 
skynet_socket_udp(struct skynet_context *ctx, const char * addr, int port) {
	uint32_t source = skynet_context_handle(ctx);
	return socket_server_udp(SOCKET_SERVER, source, addr, port);
}


//发送'C'命令请求
//对应的调用set_udp_address 设置udp的地址信息
int 
skynet_socket_udp_connect(struct skynet_context *ctx, int id, const char * addr, int port) {
	return socket_server_udp_connect(SOCKET_SERVER, id, addr, port);
}



//发送'A'命令请求
//对应的调用 send_socket()发送udp数据
int 
skynet_socket_udp_send(struct skynet_context *ctx, int id, const char * address, const void *buffer, int sz) {
	int64_t wsz = socket_server_udp_send(SOCKET_SERVER, id, (const struct socket_udp_address *)address, buffer, sz);
	return check_wsz(ctx, id, (void *)buffer, wsz);
}

const char *
skynet_socket_udp_address(struct skynet_socket_message *msg, int *addrsz) {
	if (msg->type != SKYNET_SOCKET_TYPE_UDP) {
		return NULL;
	}
	struct socket_message sm;
	sm.id = msg->id;
	sm.opaque = 0;
	sm.ud = msg->ud;
	sm.data = msg->buffer;
	return (const char *)socket_server_udp_address(SOCKET_SERVER, &sm, addrsz);
}
