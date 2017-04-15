#include "skynet.h"
#include "skynet_socket.h"
#include "databuffer.h"
#include "hashid.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>

#define BACKLOG 32


// connection结构保存了客户端的连接信息

struct connection {
	int id;				// skynet_socket id      socket对应的在socket数组中的标号
	
	uint32_t agent;
	uint32_t client;
	char remote_name[32];
	struct databuffer buffer;
};


// 1.watchdog 模式，由 gate 加上包头，同时处理控制信息和数据信息的所有数据；
// 2.agent    模式，让每个 agent 处理独立连接；
// 3.broker   模式，由一个 broker 服务处理不同连接上的所有数据包。

// 无论是哪种模式，控制信息都是交给 watchdog 去处理的，而数据包如果不发给 watchdog 
//而是发送给 agent 或 broker 的话，
// 则不会有额外的数据头（也减少了数据拷贝）。识别这些包是从外部发送进来的方法是
//检查消息包的类型是否为 PTYPE_CLIENT 。当然，你也可以自己定制消息类型让 gate 通知你。



// gate服务用与skynet对外的TCP通信 它将外部的消息格式转化成skynet内部的消息

//gate  门
// 对外的 tcp连接
struct gate {

	struct skynet_context *ctx;

	
	int listen_id;         //对应的是监听套接字的 应用层id,即标号

	uint32_t watchdog;
	uint32_t broker;
	int client_tag;
	int header_size;

	int max_connection;             //conn数组的容量


	//存储id的哈希表
	struct hashid hash;

	//保存connection的数组
	struct connection *conn;

	
	// todo: save message pool ptr for release
	// 消息池,存放的是struct messagepool_list 链表
	struct messagepool mp;

};


//在skynet_context_new()中被调用
struct gate *
gate_create(void) {

	//创建gate结构体
	struct gate * g = skynet_malloc(sizeof(*g));
	memset(g,0,sizeof(*g));
	g->listen_id = -1;
	return g;
}

//销毁gate
void
gate_release(struct gate *g) {
	int i;
	struct skynet_context *ctx = g->ctx;

	//循环遍历，关闭所有套接字
	for (i=0;i<g->max_connection;i++) {
		struct connection *c = &g->conn[i];

		if (c->id >=0) 
		{
			// 主动关闭和客户端的连接
			skynet_socket_close(ctx, c->id);
		}
	}

		//请求关闭套接字
	if (g->listen_id >= 0) {
		skynet_socket_close(ctx, g->listen_id);
	}

	messagepool_free(&g->mp);

	//销毁hash表
	hashid_clear(&g->hash);

	skynet_free(g->conn);
	skynet_free(g);
}

//将命令后面的参数字符串复制到command的开头 
//command_sz是命令本身的大小     mmm cccc
static void
_parm(char *msg, int sz, int command_sz) {
	while (command_sz < sz) {
		if (msg[command_sz] != ' ')
			break;
		++command_sz;
	}
	
	int i;
	for (i=command_sz;i<sz;i++) {
		msg[i-command_sz] = msg[i];
	}
	msg[i-command_sz] = '\0';
}

static void
_forward_agent(struct gate * g, int fd, uint32_t agentaddr, uint32_t clientaddr) {
	int id = hashid_lookup(&g->hash, fd);
	if (id >=0) {
		struct connection * agent = &g->conn[id];
		agent->agent = agentaddr;
		agent->client = clientaddr;
	}
}


// 控制命令的处理
//sz表示msg大小
static void
_ctrl(struct gate * g, const void * msg, int sz) {
	struct skynet_context * ctx = g->ctx;

	char tmp[sz+1];

	//将msg复制到tmp中
	memcpy(tmp, msg, sz);
	tmp[sz] = '\0';

	
	char * command = tmp;

	int i;

	//如果msg大小为0
	if (sz == 0)
		return;

	
	//找到第一个空格的位置   xxxxx yyyyyy
	for (i=0;i<sz;i++) {
		if (command[i]==' ') {
			break;
		}
	}

	//如果命令是kick	
	if (memcmp(command,"kick",i)==0) {

		//将命令后面的参数字符串复制到command的开头
		_parm(tmp, sz, i);

		//将参数转换为10进制整数
		int uid = strtol(command , NULL, 10);

		//在id的哈希表中查找id
		int id = hashid_lookup(&g->hash, uid);

		//关闭套接字
		if (id>=0) {
			skynet_socket_close(ctx, uid);
		}

		return;
	}

	//如果命令是forward  向前传递消息
	if (memcmp(command,"forward",i)==0) {

		//将命令后面的参数字符串复制到tmp的开头
		_parm(tmp, sz, i);

		char * client = tmp;
						//分割字符串的函数
					//按照 " "分割成两部分     idstr指向第一部分    client指向第二部分
		char * idstr = strsep(&client, " ");

		if (client == NULL) {
			return;
		}

		//转换成整数
		int id = strtol(idstr , NULL, 10);

		//再次分割
		char * agent = strsep(&client, " ");

		if (client == NULL) {
			return;
		}

		//转换成16进制整数
		uint32_t agent_handle = strtoul(agent+1, NULL, 16);
		
		uint32_t client_handle = strtoul(client+1, NULL, 16);


		_forward_agent(g, id, agent_handle, client_handle);
		return;
	}

	//如果是broker命令
	if (memcmp(command,"broker",i)==0) {
		
		//将命令后面的参数字符串复制到tmp的开头
		_parm(tmp, sz, i);

		//根据名称查找handle
		g->broker = skynet_queryname(ctx, command);
		return;
	}

	//如果是start命令
	if (memcmp(command,"start",i) == 0) {

		//将命令后面的参数字符串复制到tmp的开头
		_parm(tmp, sz, i);

		
		int uid = strtol(command , NULL, 10);

		//查询uid
		int id = hashid_lookup(&g->hash, uid);

		
		//将监听套接字加入到epoll中管理
		if (id>=0) {
			skynet_socket_start(ctx, uid);
		}
		return;
	}

	//如果命令是close
	if (memcmp(command, "close", i) == 0) {

		//关闭监听套接字
		if (g->listen_id >= 0) {
			skynet_socket_close(ctx, g->listen_id);
			g->listen_id = -1;
		}
		return;
	}
	skynet_error(ctx, "[gate] Unkown command : %s", command);
}


//关键是调用skynet_send函数，将消息加入到ctx的消息队列中
static void
_report(struct gate * g, const char * data, ...) {
	if (g->watchdog == 0) {
		return;
	}
	struct skynet_context * ctx = g->ctx;

	//为了使用可变参数
	va_list ap;
	
	va_start(ap, data);
	char tmp[1024];
	int n = vsnprintf(tmp, sizeof(tmp), data, ap);
	va_end(ap);
//skynet_send(struct skynet_context * context, uint32_t source, 
//			uint32_t destination , int type, int session, void * data, size_t sz) 	

	//将消息加入到watchdog 对应的消息队列
	skynet_send(ctx, 0, g->watchdog, PTYPE_TEXT,  0, tmp, n);
}

static void
_forward(struct gate *g, struct connection * c, int size) {
	struct skynet_context * ctx = g->ctx;
	if (g->broker) {
		void * temp = skynet_malloc(size);
		databuffer_read(&c->buffer,&g->mp,temp, size);
		skynet_send(ctx, 0, g->broker, g->client_tag | PTYPE_TAG_DONTCOPY, 0, temp, size);
		return;
	}
	if (c->agent) {
		void * temp = skynet_malloc(size);
		databuffer_read(&c->buffer,&g->mp,temp, size);
		skynet_send(ctx, c->client, c->agent, g->client_tag | PTYPE_TAG_DONTCOPY, 0 , temp, size);
	} else if (g->watchdog) {
		char * tmp = skynet_malloc(size + 32);
		int n = snprintf(tmp,32,"%d data ",c->id);
		databuffer_read(&c->buffer,&g->mp,tmp+n,size);
		skynet_send(ctx, 0, g->watchdog, PTYPE_TEXT | PTYPE_TAG_DONTCOPY, 0, tmp, size + n);
	}
}

static void
dispatch_message(struct gate *g, struct connection *c, int id, void * data, int sz) {
	databuffer_push(&c->buffer,&g->mp, data, sz);
	for (;;) {
		int size = databuffer_readheader(&c->buffer, &g->mp, g->header_size);
		if (size < 0) {
			return;
		} else if (size > 0) {
			if (size >= 0x1000000) {
				struct skynet_context * ctx = g->ctx;
				databuffer_clear(&c->buffer,&g->mp);
				skynet_socket_close(ctx, id);
				skynet_error(ctx, "Recv socket message > 16M");
				return;
			} else {
				_forward(g, c, size);
				databuffer_reset(&c->buffer);
			}
		}
	}
}


//当类型是 PTYPE_SOCKET:时，调用该函数处理
// socket消息的处理
static void
dispatch_socket_message(struct gate *g, const struct skynet_socket_message * message, int sz) {

	struct skynet_context * ctx = g->ctx;

	switch(message->type) {
	case SKYNET_SOCKET_TYPE_DATA: {
		int id = hashid_lookup(&g->hash, message->id);
		if (id>=0) {
			struct connection *c = &g->conn[id];
			dispatch_message(g, c, message->id, message->buffer, message->ud);
		} else {
			skynet_error(ctx, "Drop unknown connection %d message", message->id);
			skynet_socket_close(ctx, message->id);
			skynet_free(message->buffer);
		}
		break;
	}
	case SKYNET_SOCKET_TYPE_CONNECT: {
		if (message->id == g->listen_id) {
			// start listening
			break;
		}
		int id = hashid_lookup(&g->hash, message->id);
		if (id<0) {
			skynet_error(ctx, "Close unknown connection %d", message->id);
			skynet_socket_close(ctx, message->id);
		}
		break;
	}
	case SKYNET_SOCKET_TYPE_CLOSE:
	case SKYNET_SOCKET_TYPE_ERROR: {
		int id = hashid_remove(&g->hash, message->id);
		if (id>=0) {
			struct connection *c = &g->conn[id];
			databuffer_clear(&c->buffer,&g->mp);
			memset(c, 0, sizeof(*c));
			c->id = -1;
			_report(g, "%d close", message->id);
		}
		break;
	}

	//接受连接后
	case SKYNET_SOCKET_TYPE_ACCEPT:
		// report accept, then it will be get a SKYNET_SOCKET_TYPE_CONNECT message
		assert(g->listen_id == message->id);
		
		if (hashid_full(&g->hash)) {
			skynet_socket_close(ctx, message->ud);
		}
		else
		{
		/*
			struct connection {
				int id;	// skynet_socket id
				uint32_t agent;
				uint32_t client;
				char remote_name[32];
				struct databuffer buffer;
			};
		*/
			struct connection *c = &g->conn[hashid_insert(&g->hash, message->ud)];
			if (sz >= sizeof(c->remote_name)) {
				sz = sizeof(c->remote_name) - 1;
			}
			c->id = message->ud;
			memcpy(c->remote_name, message+1, sz);
			c->remote_name[sz] = '\0';
			
			_report(g, "%d open %d %s:0",c->id, c->id, c->remote_name);

			skynet_error(ctx, "socket open: %x", c->id);
		}
		break;
	case SKYNET_SOCKET_TYPE_WARNING:
		skynet_error(ctx, "fd (%d) send buffer (%d)K", message->id, message->ud);
		break;
	}
}

//获取全局消息队列中的一个消息后调用的函数,回调函数
static int
_cb(struct skynet_context * ctx, void * ud, int type, int session, uint32_t source, const void * msg, size_t sz) {
	struct gate *g = ud;
	
	switch(type) {

		// skynet内部的文本协议 一般来说是控制命令
	case PTYPE_TEXT:
		_ctrl(g , msg , (int)sz);
		break;

		// 客户端的消息
	case PTYPE_CLIENT: {
		if (sz <=4 ) {
			skynet_error(ctx, "Invalid client message from %x",source);
			break;
		}
		// The last 4 bytes in msg are the id of socket, write following bytes to it

		// msg的后4个字节是socket的id 之后是剩下的字节
		const uint8_t * idbuf = msg + sz - 4;
		uint32_t uid = idbuf[0] | idbuf[1] << 8 | idbuf[2] << 16 | idbuf[3] << 24;

		// 找到这个socket id即在应用层维护的socket fd
		int id = hashid_lookup(&g->hash, uid);
		if (id>=0) {
			// don't send id (last 4 bytes)
			skynet_socket_send(ctx, uid, (void*)msg, sz-4);
			// return 1 means don't free msg
			return 1;
		} else {
			skynet_error(ctx, "Invalid client id %d from %x",(int)uid,source);
			break;
		}
	}


	// socket的消息类型 分发消息
	//accept后会调用
	case PTYPE_SOCKET:
		// recv socket message from skynet_socket
		// socket消息的处理
		dispatch_socket_message(g, msg, (int)(sz-sizeof(struct skynet_socket_message)));
		break;

	}
	return 0;
}


//在xxxx_init()函数中被调用
static int
start_listen(struct gate *g, char * listen_addr) {
	struct skynet_context * ctx = g->ctx;
	
	char * portstr = strchr(listen_addr,':');
	const char * host = "";

	int port;

	//如果没有 :
	if (portstr == NULL) {

		//得到端口
		port = strtol(listen_addr, NULL, 10);
		if (port <= 0) {
			skynet_error(ctx, "Invalid gate address %s",listen_addr);
			return 1;
		}
	} 
	else
	{
		//得到端口
		port = strtol(portstr + 1, NULL, 10);
		
		if (port <= 0)
		{
			skynet_error(ctx, "Invalid gate address %s",listen_addr);
			return 1;
		}
		portstr[0] = '\0';
		host = listen_addr;
	}
	
	//socket()  bind()  listen()  存入socket数组   但没有加入到epoll中管理
	g->listen_id = skynet_socket_listen(ctx, host, port, BACKLOG);
	
	if (g->listen_id < 0) {
		return 1;
	}

	//将监听套接字加入到epoll中管理
	skynet_socket_start(ctx, g->listen_id);
	return 0;
}

//在skynet_context_new()中被调用
int
gate_init(struct gate *g , struct skynet_context * ctx, char * parm) {
	if (parm == NULL)
		return 1;
	int max = 0;
	
	int sz = strlen(parm)+1;
	char watchdog[sz];
	char binding[sz];
	int client_tag = 0;
	char header;

	int n = sscanf(parm, "%c %s %s %d %d", &header, watchdog, binding, &client_tag, &max);
	if (n<4) {
		skynet_error(ctx, "Invalid gate parm %s",parm);
		return 1;
	}
	
	if (max <=0 ) {
		skynet_error(ctx, "Need max connection");
		return 1;
	}
	
	if (header != 'S' && header !='L') {
		skynet_error(ctx, "Invalid data header style");
		return 1;
	}

	if (client_tag == 0) {
		client_tag = PTYPE_CLIENT;
	}
	if (watchdog[0] == '!') {
		g->watchdog = 0;
	} else {
		g->watchdog = skynet_queryname(ctx, watchdog);
		if (g->watchdog == 0) {
			skynet_error(ctx, "Invalid watchdog %s",watchdog);
			return 1;
		}
	}

	g->ctx = ctx;

	//初始化存储id的 hash表
	hashid_init(&g->hash, max);

	g->conn = skynet_malloc(max * sizeof(struct connection));
	memset(g->conn, 0, max *sizeof(struct connection));

	g->max_connection = max;
	int i;
	for (i=0;i<max;i++) {
		g->conn[i].id = -1;
	}
	
	g->client_tag = client_tag;
	g->header_size = header=='S' ? 2 : 4;

	//设置skynet_context 的回调函数
	skynet_callback(ctx,g,_cb);

	//创建的监听套接字
	return start_listen(g,binding);
}



