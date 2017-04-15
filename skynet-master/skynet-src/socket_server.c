#include "skynet.h"

#include "socket_server.h"
#include "socket_poll.h"
#include "atomic.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>

#define MAX_INFO 128
// MAX_SOCKET will be 2^MAX_SOCKET_P
#define MAX_SOCKET_P 16

#define MAX_EVENT 64		// 用于epoll_wait的第三个参数，即每次epoll返回的最多事件数

#define MIN_READ_BUFFER 64	// read最小分配的缓冲区大小

#define SOCKET_TYPE_INVALID 0 	//无效的套接字

#define SOCKET_TYPE_RESERVE 1	// 预留，已被申请，即将投入使用

#define SOCKET_TYPE_PLISTEN 2	// 监听套接字，未加入epoll管理

#define SOCKET_TYPE_LISTEN 3	// 监听套接字，已加入epoll管理
#define SOCKET_TYPE_CONNECTING 4	// 尝试连接中的套接字
#define SOCKET_TYPE_CONNECTED 5		// 已连接套接字，主动或被动(connect,accept成功，并已加入epoll管理)

// 应用层已发起关闭套接字请求，应用层发送缓冲区尚未发送完，未调用close
#define SOCKET_TYPE_HALFCLOSE 6	

#define SOCKET_TYPE_PACCEPT 7  // accept返回的已连接套接字，但未加入epoll管理
#define SOCKET_TYPE_BIND 8     // 其它类型的文件描述符，比如stdin,stdout等


#define MAX_SOCKET (1<<MAX_SOCKET_P)     // 最多支持64K个socket

#define PRIORITY_HIGH 0
#define PRIORITY_LOW 1

#define HASH_ID(id) (((unsigned)id) % MAX_SOCKET)

#define PROTOCOL_TCP 0
#define PROTOCOL_UDP 1
#define PROTOCOL_UDPv6 2

//udp_address缓冲区大小			16+2+1
#define UDP_ADDRESS_SIZE 19	// ipv6 128bit + port 16bit + 1 byte type


//udpbuffer 数组大小
#define MAX_UDP_PACKAGE 65535

// EAGAIN and EWOULDBLOCK may be not the same value.
#if (EAGAIN != EWOULDBLOCK)
#define AGAIN_WOULDBLOCK EAGAIN : case EWOULDBLOCK
#else
#define AGAIN_WOULDBLOCK EAGAIN
#endif

//发送缓冲区节点
// 发送缓冲区构成一个链表
struct write_buffer {
	struct write_buffer * next;
	
	void *buffer;	// 发送缓冲区
	char *ptr;      // 指向当前未发送的数据首部
	int sz;			// 当前块未发送的字节数

	//是否使用 struct socket_object_interface soi;
	bool userobject;

	//udp_address缓冲区大小			16+2+1
	//保存的是数据要发送的地址的信息
	uint8_t udp_address[UDP_ADDRESS_SIZE];
};


//标准宏 #define offsetof(s, m)   (size_t)&(((s *)0)->m)
//结构体s 中有一个成员m,利用该宏得出m的偏移地址

//得到udp_address[0]的偏移量
#define SIZEOF_TCPBUFFER (offsetof(struct write_buffer, udp_address[0]))

//得到发送缓冲区的大小
#define SIZEOF_UDPBUFFER (sizeof(struct write_buffer))



//发送缓冲区链式队列

struct wb_list {
	struct write_buffer * head;
	struct write_buffer * tail;
};


// 应用层对每个描述符 的抽象
struct socket {

	//将该描述符上的有关数据最终发送到来opaque对应的skynet_context消息队列中]
	uintptr_t opaque;  // 在skynet中用于保存服务的handle,即skynet_context对应的标号

	//高优先级发送队列
	struct wb_list high;

	//低优先级发送队列
	struct wb_list low;

	int64_t wb_size;	// 发送缓冲区未发送的数据
	
	int fd;			// 文件描述符

	//实质就是socket存放在socket_server中的数组中的标号
	int id;			// 应用层维护的一个与fd相对应的id
	
	uint16_t protocol;	//socket协议 TCP/UDP
	
	uint16_t type;		//socket状态(读，写，监听......)

	union {
				
		int size;  //读缓存预估计的大小		
	
		uint8_t udp_address[UDP_ADDRESS_SIZE];
	} p;
};


//服务器 socket部分的抽象,存放socket的数组
struct socket_server {
	
	int recvctrl_fd;	// 管道读端，用于接受控制命令
	int sendctrl_fd;	// 管道写端，用于发送控制命令

	int checkctrl;		// 是否检查控制命令

	poll_fd event_fd;	// epoll fd
	
	int alloc_id;		// 用于分配id
	int event_n;		// epoll_wait返回的事件个数
	int event_index;	// 当前处理的事件序号，从0开始

	//结构体中有一些函数指针
	/*
		struct socket_object_interface {
			void * (*buffer)(void *);
			int (*size)(void *);
			void (*free)(void *);
		};

	*/
	struct socket_object_interface soi;

	//epoll_wait得到的活跃的描述符
	struct event ev[MAX_EVENT];			 // epoll_wait返回的事件集
	
	struct socket slot[MAX_SOCKET];		// 应用层预先分配的socket数组(即socket池)
	char buffer[MAX_INFO];				// 临时数据，比如保存新建连接的对等端的地址信息
	
	uint8_t udpbuffer[MAX_UDP_PACKAGE];
	
	fd_set rfds;		// 用于select的fd集合
};



// 以下几个结构体是 控制命令数据包 包体结构

struct request_open {
	int id;
	int port;
	uintptr_t opaque;
	char host[1];
};

struct request_send {
	int id;

		//sz是buffer的大小
	int sz;
	char * buffer;
};

struct request_send_udp {

	struct request_send send;
	
	uint8_t address[UDP_ADDRESS_SIZE];
};

struct request_setudp {
	int id;
	uint8_t address[UDP_ADDRESS_SIZE];
};

struct request_close {
	int id;
	int shutdown;
	uintptr_t opaque;
};

struct request_listen {
	int id;
	int fd;
	uintptr_t opaque;
	char host[1];
};

struct request_bind {
	int id;
	int fd;
	uintptr_t opaque;
};

struct request_start {
	int id;
	uintptr_t opaque;
};

struct request_setopt {
	int id;
	int what;
	int value;
};

struct request_udp {
	int id;
	int fd;
	int family;
	uintptr_t opaque;
};

/*
	The first byte is TYPE

	S Start socket
	B Bind socket
	L Listen socket
	K Close socket
	O Connect to (Open)
	X Exit
	D Send package (high)
	P Send package (low)
	A Send UDP package
	T Set opt
	U Create UDP socket
	C set udp address
 */


// 控制命令请求包


struct request_package {

	//6个字节未用 [0-5] [6]是type [7]长度 长度指的是包体的长度
	uint8_t header[8];	// 6 bytes dummy

	union {
		char buffer[256];
		struct request_open open;
		struct request_send send;
		struct request_send_udp send_udp;
		struct request_close close;
		struct request_listen listen;
		struct request_bind bind;
		struct request_start start;
		struct request_setopt setopt;
		struct request_udp udp;
		struct request_setudp set_udp;
	} u;
	uint8_t dummy[256];
};

// 网际IP地址
union sockaddr_all {
	struct sockaddr s;
	struct sockaddr_in v4;
	struct sockaddr_in6 v6;
};


struct send_object {
	void * buffer;
	int sz;
	void (*free_func)(void *);
};

// 本质上还是 #define skynet_malloc malloc
#define MALLOC skynet_malloc
#define FREE skynet_free


//初始化 struct send_object
static inline bool
send_object_init(struct socket_server *ss, struct send_object *so, void *object, int sz) {
	if (sz < 0) {
		so->buffer = ss->soi.buffer(object);
		so->sz = ss->soi.size(object);
		so->free_func = ss->soi.free;
		return true;
	} else {
		so->buffer = object;
		so->sz = sz;
		so->free_func = FREE;
		return false;
	}
}

//释放一个发送缓冲节点
static inline void
write_buffer_free(struct socket_server *ss, struct write_buffer *wb) {

	//判断是否使用userobject
	if (wb->userobject) {
		ss->soi.free(wb->buffer);
	} else {
		FREE(wb->buffer);
	}
	FREE(wb);
}


//设置实现心跳功能
static void
socket_keepalive(int fd) {
	int keepalive = 1;
	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (void *)&keepalive , sizeof(keepalive));  
}


// 从socket池中获取一个空的socket，并为其分配一个id(0~2147483647即2^31-1)
// 在socket池中的位置 池的大小是64K socket_id的范围远大与64K

static int
reserve_id(struct socket_server *ss) {
	int i;
	for (i=0;i<MAX_SOCKET;i++) {
		int id = ATOM_INC(&(ss->alloc_id));  // 原子的++

		// 小于0   已经最大了 说明 从0开始
		if (id < 0) {

			// 此时id = 0x80000000即-2147483648
			id = ATOM_AND(&(ss->alloc_id), 0x7fffffff);
		}

		// 从socket池中取出socket
		struct socket *s = &ss->slot[HASH_ID(id)];

		//如果该位置没有被使用		
		if (s->type == SOCKET_TYPE_INVALID) {

			// 如果相等就交换成 SOCKET_TYPE_RESERVE 设置为已用
			// 这里由于没有加锁 可能多个线程操作 所以还需要判断一次

			//将状态变为 SOCKET_TYPE_RESERVE 预留
			if (ATOM_CAS(&s->type, SOCKET_TYPE_INVALID, SOCKET_TYPE_RESERVE)) {
				s->id = id;
				s->fd = -1;
				return id;
			} else {
				// retry
				--i;
			}
		}
	}
	return -1;
}


//清除发送缓冲队列
static inline void
clear_wb_list(struct wb_list *list) {
	list->head = NULL;
	list->tail = NULL;
}



//创建socker_server
struct socket_server * 
socket_server_create() {
	int i;
	int fd[2];

	//sp_create() 调用  epoll_create(1024);
	poll_fd efd = sp_create();

	//错误判断
	//return efd == -1;
	if (sp_invalid(efd)) {
		fprintf(stderr, "socket-server: create event pool failed.\n");
		return NULL;
	}

	//创建匿名管道
	if (pipe(fd)) {

		//实质调用 close(efd);
		sp_release(efd);
		fprintf(stderr, "socket-server: create socket pair failed.\n");
		return NULL;
	}

	// epoll 关注管道的可读事件
	if (sp_add(efd, fd[0], NULL)) {
		// add recvctrl_fd to event poll
		fprintf(stderr, "socket-server: can't add server fd to event pool.\n");
		close(fd[0]);
		close(fd[1]);
		sp_release(efd);
		return NULL;
	}

	//MALLOC  就是malloc
	//socket_server就是对服务器网络部分的抽象
	struct socket_server *ss = MALLOC(sizeof(*ss));
	
	ss->event_fd = efd;
	
	ss->recvctrl_fd = fd[0];	//管道输入端
	ss->sendctrl_fd = fd[1];	//管道输出端
	ss->checkctrl = 1;


	//初始化容量 64k个socket数组
	for (i=0;i<MAX_SOCKET;i++) {
		struct socket *s = &ss->slot[i];
		
		//标记为未使用
		s->type = SOCKET_TYPE_INVALID;
		clear_wb_list(&s->high);
		clear_wb_list(&s->low);
	}
	ss->alloc_id = 0;
	ss->event_n = 0;
	ss->event_index = 0;
	memset(&ss->soi, 0, sizeof(ss->soi));

	//将select 所需集合设置为0
	FD_ZERO(&ss->rfds);
	
	assert(ss->recvctrl_fd < FD_SETSIZE);

	return ss;
}

//释放发送缓冲队列list
static void
free_wb_list(struct socket_server *ss, struct wb_list *list) {

	struct write_buffer *wb = list->head;
	while (wb) {
		struct write_buffer *tmp = wb;
		wb = wb->next;
		//释放内存
		write_buffer_free(ss, tmp);
	}
	list->head = NULL;
	list->tail = NULL;
}


//强制关闭套接字,并且清理socket_server中对应的 struct socket *s  result是传入传出参数
static void
force_close(struct socket_server *ss, struct socket *s, struct socket_message *result) {

	result->id = s->id;
	result->ud = 0;
	result->data = NULL;
	result->opaque = s->opaque;

	//如果s是未使用的
	if (s->type == SOCKET_TYPE_INVALID) {
		return;
	}

	//断言s不是预留的
	assert(s->type != SOCKET_TYPE_RESERVE);
	
	//销毁发送缓冲区
	free_wb_list(ss,&s->high);
	free_wb_list(ss,&s->low);


	//SOCKET_TYPE_PACCEPT   SOCKET_TYPE_PLISTEN代表未加入到epoll中管理
	if (s->type != SOCKET_TYPE_PACCEPT && s->type != SOCKET_TYPE_PLISTEN) {

		// epoll取消关注该套接字
		sp_del(ss->event_fd, s->fd);
	}

	//如果套接字类型不是stdin,stdout 就关闭
	if (s->type != SOCKET_TYPE_BIND) {
		
		if (close(s->fd) < 0) {
			perror("close socket:");
		}
	}

	//将状态变为未使用
	s->type = SOCKET_TYPE_INVALID;
}


//销毁socket_server
void 
socket_server_release(struct socket_server *ss) {
	int i;
	
	struct socket_message dummy;

	for (i=0;i<MAX_SOCKET;i++) {
		struct socket *s = &ss->slot[i];

		//如果不是预留的socket结构体
		if (s->type != SOCKET_TYPE_RESERVE) {

			//dummy是传出参数
			force_close(ss, s , &dummy);
		}
	}

	//关闭管道
	close(ss->sendctrl_fd);
	close(ss->recvctrl_fd);

	//调用 close() 关闭 epoll描述符
	sp_release(ss->event_fd);

	//free
	FREE(ss);
}


//断言发送缓冲区队列为空
static inline void
check_wb_list(struct wb_list *s) {
	assert(s->head == NULL);
	assert(s->tail == NULL);
}


//为新预留的socket初始化，如果add为true还会将该套接字加入epoll管理
// struct socket是对每个套接字的抽象
//id就是 该socket对象指针在服务器对象struct socket_server中的数组中hash之前的位置
static struct socket *
new_fd(struct socket_server *ss, int id, int fd, int protocol, uintptr_t opaque, bool add) {

	//根据HASH_ID(id)取出一个位置
	struct socket * s = &ss->slot[HASH_ID(id)];

	//断言是预留的
	assert(s->type == SOCKET_TYPE_RESERVE);

	//如果add为true,将描述符加入epoll管理
	if (add) {
		if (sp_add(ss->event_fd, fd, s)) {
			s->type = SOCKET_TYPE_INVALID;
			return NULL;
		}
	}

	s->id = id;
	s->fd = fd;

	//socket协议 TCP/UDP
	s->protocol = protocol;

	//估计的读的缓冲区大小
	s->p.size = MIN_READ_BUFFER;
	
	s->opaque = opaque;

	//发送缓冲区未发生数据的大小
	s->wb_size = 0;

	//断言为空
	check_wb_list(&s->high);
	check_wb_list(&s->low);

	return s;
}

// return -1 when connecting
//用于connect()
//连接成功返回SOCKET_OPEN,未连接成功(处于连接中)返回-1,出错返回SOCKET_ERROR
//result是传出参数
static int
open_socket(struct socket_server *ss, struct request_open * request, struct socket_message *result) {

	int id = request->id;
	result->opaque = request->opaque;
	result->id = id;
	result->ud = 0;
	result->data = NULL;
	
	struct socket *ns;
	int status;
	
	struct addrinfo ai_hints;


	//得到的sockaddr链表指针
	struct addrinfo *ai_list = NULL;
	
	struct addrinfo *ai_ptr = NULL;
	
	char port[16];
	sprintf(port, "%d", request->port);
	memset(&ai_hints, 0, sizeof( ai_hints ) );

	//
	ai_hints.ai_family = AF_UNSPEC;
	ai_hints.ai_socktype = SOCK_STREAM;
	ai_hints.ai_protocol = IPPROTO_TCP;

	//获取地址列表,(不用gethostbyname,gethostbuaddr,这两个函数仅支持IPV4)
	//第一个参数是IP地址或主机名称，第二个参数是服务器名(可以是端口号或服务名称，如http,ftp)
	status = getaddrinfo( request->host, port, &ai_hints, &ai_list );
	if ( status != 0 ) {

		//getaddrinfo出错返回非零值,gai_strerror根据返回的值获取执行错误信息的字符串的指针
		result->data = (void *)gai_strerror(status);
		goto _failed;
	}
	
	int sock= -1;
	for (ai_ptr = ai_list; ai_ptr != NULL; ai_ptr = ai_ptr->ai_next ) {

		//调用socket函数
		sock = socket( ai_ptr->ai_family, ai_ptr->ai_socktype, ai_ptr->ai_protocol );
		if ( sock < 0 ) {
			continue;
		}

		//设置socket属性,非阻塞，心跳
		socket_keepalive(sock);
		sp_nonblocking(sock);

		//调用connect函数
		status = connect( sock, ai_ptr->ai_addr, ai_ptr->ai_addrlen);
		if ( status != 0 && errno != EINPROGRESS) {
			close(sock);
			sock = -1;

			//连接出错，跳过本次循环，连接到下一个地址
			continue;
		}
		break;
	}

	if (sock < 0) {
		result->data = strerror(errno);
		goto _failed;
	}

	//加入epoll管理,还会在socket_server的socket数组中
	ns = new_fd(ss, id, sock, PROTOCOL_TCP, request->opaque, true);
	if (ns == NULL) {
		close(sock);
		result->data = "reach skynet socket number limit";
		goto _failed;
	}

	
	if(status == 0) {
		//说明connect成功
		ns->type = SOCKET_TYPE_CONNECTED;

		struct sockaddr * addr = ai_ptr->ai_addr;
		void * sin_addr = (ai_ptr->ai_family == AF_INET) ? (void*)&((struct sockaddr_in *)addr)->sin_addr : (void*)&((struct sockaddr_in6 *)addr)->sin6_addr;

		if (inet_ntop(ai_ptr->ai_family, sin_addr, ss->buffer, sizeof(ss->buffer))) {
			//得到ip
			result->data = ss->buffer;
		}

		//标准库的释放函数
		freeaddrinfo( ai_list );
		return SOCKET_OPEN;
	} else {

		//说明非阻塞套接字尝试连接中
		ns->type = SOCKET_TYPE_CONNECTING;
		//非阻塞套接字尝试连接中，必须关注其可写事件，稍后epoll才能捕获到连接出错了还是成功了
		sp_write(ss->event_fd, ns->fd, ns, true);
	}

	freeaddrinfo( ai_list );
	return -1;
_failed:
	freeaddrinfo( ai_list );
	ss->slot[HASH_ID(id)].type = SOCKET_TYPE_INVALID;
	return SOCKET_ERROR;
}

//将发送缓冲区中的数据发送 tcp类型
//result是传出参数
static int
send_list_tcp(struct socket_server *ss, struct socket *s, struct wb_list *list, struct socket_message *result) {
	while (list->head) {
		struct write_buffer * tmp = list->head;

		//不断的调用write()
		for (;;) {

			////tmp->sz表示该结点中未发送的数据大小
			int sz = write(s->fd, tmp->ptr, tmp->sz);
			
			if (sz < 0) {
				switch(errno) {
				case EINTR:
					continue;
				case AGAIN_WOULDBLOCK:
					return -1;
				}
				force_close(ss,s, result);
				//1
				return SOCKET_CLOSE;
			}

			//wb_size表示发送缓冲区中未发送的数据
			s->wb_size -= sz;

			//tmp->sz表示该结点中未发送的数据
			if (sz != tmp->sz) {
				
				tmp->ptr += sz;
				tmp->sz -= sz;
				return -1;
			}
			
			break;
		}

		list->head = tmp->next;
		write_buffer_free(ss,tmp);
	}
	
	list->tail = NULL;

	return -1;
}

// 得到网际IP地址union sockaddr_all
/*
union sockaddr_all {
	struct sockaddr s;
	struct sockaddr_in v4;
	struct sockaddr_in6 v6;
};


*/

//通过udp_address获取 ip 端口信息。通过sa传出
static socklen_t
udp_socket_address(struct socket *s, const uint8_t udp_address[UDP_ADDRESS_SIZE], union sockaddr_all *sa) {

	//获取协议类型
	int type = (uint8_t)udp_address[0];
	if (type != s->protocol)
		return 0;
	
	uint16_t port = 0;

	//获取端口号
	memcpy(&port, udp_address+1, sizeof(uint16_t));

	//判断协议类型  tcp/udp
	//获取地址
	switch (s->protocol) {
		
	case PROTOCOL_UDP:
		memset(&sa->v4, 0, sizeof(sa->v4));
		sa->s.sa_family = AF_INET;
		sa->v4.sin_port = port;
		memcpy(&sa->v4.sin_addr, udp_address + 1 + sizeof(uint16_t), sizeof(sa->v4.sin_addr));	// ipv4 address is 32 bits
		return sizeof(sa->v4);

	case PROTOCOL_UDPv6:
		memset(&sa->v6, 0, sizeof(sa->v6));
		sa->s.sa_family = AF_INET6;
		sa->v6.sin6_port = port;
		memcpy(&sa->v6.sin6_addr, udp_address + 1 + sizeof(uint16_t), sizeof(sa->v6.sin6_addr)); // ipv6 address is 128 bits
		return sizeof(sa->v6);
	}
	return 0;
}



//从发送队列中  sendto 发送udp类型的数据
static int
send_list_udp(struct socket_server *ss, struct socket *s, struct wb_list *list, struct socket_message *result) {
	while (list->head) {
		
		struct write_buffer * tmp = list->head;

		union sockaddr_all sa;
		//获得地址信息，存放在sa中
		socklen_t sasz = udp_socket_address(s, tmp->udp_address, &sa);
		
		// 发送
		int err = sendto(s->fd, tmp->ptr, tmp->sz, 0, &sa.s, sasz);

		if (err < 0) {
			switch(errno) {
			case EINTR:
			case AGAIN_WOULDBLOCK:
				return -1;
			}
			fprintf(stderr, "socket-server : udp (%d) sendto error %s.\n",s->id, strerror(errno));
			return -1;
/*			// ignore udp sendto error
			
			result->opaque = s->opaque;
			result->id = s->id;
			result->ud = 0;
			result->data = NULL;

			return SOCKET_ERROR;
*/
		}

		s->wb_size -= tmp->sz;
		list->head = tmp->next;
		write_buffer_free(ss,tmp);
	}
	list->tail = NULL;

	return -1;
}


//根据协议类型tcp还是udp 发送缓冲区队列中的数据，
static int
send_list(struct socket_server *ss, struct socket *s, struct wb_list *list, struct socket_message *result) {

	if (s->protocol == PROTOCOL_TCP) {	

		//tcp
		return send_list_tcp(ss, s, list, result);
	} else {

		//udp
		return send_list_udp(ss, s, list, result);
	}
}


//判断发送数据的缓冲区是否未发送完，如果未发送完，返回真
static inline int
list_uncomplete(struct wb_list *s) {
	struct write_buffer *wb = s->head;
	if (wb == NULL)
		return 0;
	
	return (void *)wb->ptr != wb->buffer;
}


//将低优先级缓冲区队列的 第一个节点(头结点) 移动到高优先级的队列中,此时高优先级链表中没有节点
static void
raise_uncomplete(struct socket * s) {

	//低优先级发送缓冲区队列
	struct wb_list *low = &s->low;
	
	struct write_buffer *tmp = low->head;

	//移动低优先级链表的头部
	low->head = tmp->next;
	if (low->head == NULL) {
		low->tail = NULL;
	}

	// move head of low list (tmp) to the empty high list
	//将低优先级缓冲区队列的第一个节点移动到高优先级的队列中
	struct wb_list *high = &s->high;

	//断言高优先级缓冲区没有数据节点
	assert(high->head == NULL);

	tmp->next = NULL;

	//将低优先级缓冲区队列的第一个节点移动到高优先级的队列中	
	high->head = high->tail = tmp;
}

/*
	Each socket has two write buffer list, high priority and low priority.

	1. send high list as far as possible.
	2. If high list is empty, try to send low list.
	3. If low list head is uncomplete (send a part before), move the head of low list to empty high list (call raise_uncomplete) .
	4. If two lists are both empty, turn off the event. (call check_close)
 */

//可写事件到来，从应用层缓冲区中发送数据,result是传入传出参数
static int
send_buffer(struct socket_server *ss, struct socket *s, struct socket_message *result) {


	//断言低优先级的缓冲区中
	assert(!list_uncomplete(&s->low));

	
	// step 1
	//发送高优先级队列中的数据   SOCKET_CLOSE表示套接字关闭了，发送数据不成功
	if (send_list(ss,s,&s->high,result) == SOCKET_CLOSE) {
		return SOCKET_CLOSE;
	}
	if (s->high.head == NULL) {
		// step 2
		//发送低优先级队列中的数据
		if (s->low.head != NULL) {
			if (send_list(ss,s,&s->low,result) == SOCKET_CLOSE) {
				return SOCKET_CLOSE;
			}

			//如果低优先级数据没有发送完,
			// step 3
			if (list_uncomplete(&s->low)) {
				//将低优先级缓冲区队列的 第一个节点(头结点) 移动到高优先级的队列中
				raise_uncomplete(s);
			}
		} else {
			//数据发送完毕
			// step 4
			//不再关注可写事件，关注可读事件
			sp_write(ss->event_fd, s->fd, s, false);

			//如果之前标记了要关闭套接字，但由于套接字数据没有发送完，只是标记要关闭，则
			//  此时数据发送完了  直接强制关闭,销毁套接字
			if (s->type == SOCKET_TYPE_HALFCLOSE) {
				force_close(ss, s, result);
				return SOCKET_CLOSE;
			}
		}
	}

	return -1;
}


//将未发送完的数据追加到write_buffe 中，n表示从第n个字节开始,就是原来已经发送了n个字节
static struct write_buffer *
append_sendbuffer_(struct socket_server *ss, struct wb_list *s, struct request_send * request, int size, int n) {

	//创建一个节点
	struct write_buffer * buf = MALLOC(size);

	struct send_object so;

	//bool   初始化struct send_object so;
	buf->userobject = send_object_init(ss, &so, request->buffer, request->sz);
	
	buf->ptr = (char*)so.buffer+n;
	buf->sz = so.sz - n;
	buf->buffer = request->buffer;
	buf->next = NULL;

	//将接节点加入到缓冲区队列中
	if (s->head == NULL) {
		s->head = s->tail = buf;
	} else {
		assert(s->tail != NULL);
		assert(s->tail->next == NULL);
		s->tail->next = buf;
		s->tail = buf;
	}

	return buf;
}

//将udp发送数据添加到队列中
static inline void
append_sendbuffer_udp(struct socket_server *ss, struct socket *s, int priority, struct request_send * request, const uint8_t udp_address[UDP_ADDRESS_SIZE]) {
	//判断优先级
	struct wb_list *wl = (priority == PRIORITY_HIGH) ? &s->high : &s->low;

	
	struct write_buffer *buf = append_sendbuffer_(ss, wl, request, SIZEOF_UDPBUFFER, 0);
	//将数据要发送的地址信息写入
	memcpy(buf->udp_address, udp_address, UDP_ADDRESS_SIZE);

	s->wb_size += buf->sz;
}


//将数据添加到高优先级队列中
static inline void
append_sendbuffer(struct socket_server *ss, struct socket *s, struct request_send * request, int n) {
	struct write_buffer *buf = append_sendbuffer_(ss, &s->high, request, SIZEOF_TCPBUFFER, n);
	s->wb_size += buf->sz;
}



//将数据添加到第优先级的队列中
static inline void
append_sendbuffer_low(struct socket_server *ss,struct socket *s, struct request_send * request) {
	struct write_buffer *buf = append_sendbuffer_(ss, &s->low, request, SIZEOF_TCPBUFFER, 0);
	s->wb_size += buf->sz;
}


//判断应用层的连个缓冲区是否未空
static inline int
send_buffer_empty(struct socket *s) {
	return (s->high.head == NULL && s->low.head == NULL);
}

/*
	When send a package , we can assign the priority : PRIORITY_HIGH or PRIORITY_LOW

	If socket buffer is empty, write to fd directly.
		If write a part, append the rest part to high list. (Even priority is PRIORITY_LOW)
	Else append package to high (PRIORITY_HIGH) or low (PRIORITY_LOW) list.
 */

//发送数据 result是传出参数
static int
send_socket(struct socket_server *ss, struct request_send * request, struct socket_message *result, int priority, const uint8_t *udp_address) {
	int id = request->id;
	
	//得到要发送数据的socket
	struct socket * s = &ss->slot[HASH_ID(id)];

	struct send_object so;
	send_object_init(ss, &so, request->buffer, request->sz);

	//如果套接字是无效的  关闭的 
	if (s->type == SOCKET_TYPE_INVALID || s->id != id 
		|| s->type == SOCKET_TYPE_HALFCLOSE
		|| s->type == SOCKET_TYPE_PACCEPT) {
		so.free_func(request->buffer);
		return -1;
	}

	//如果套接字没有添加到epoll中管理
	if (s->type == SOCKET_TYPE_PLISTEN || s->type == SOCKET_TYPE_LISTEN) {
		fprintf(stderr, "socket-server: write to listen fd %d.\n", id);
		so.free_func(request->buffer);
		return -1;
	}

	//如果 应用层缓冲区 没有遗留的为发送完的数据     直接发送
	if (send_buffer_empty(s) && s->type == SOCKET_TYPE_CONNECTED) {
		if (s->protocol == PROTOCOL_TCP) {

			//tcp数据
			//直接发送
			int n = write(s->fd, so.buffer, so.sz);
			if (n<0) {
				switch(errno) {
				case EINTR:
				case AGAIN_WOULDBLOCK:	// 内核缓冲区满了
					n = 0;
					break;
				default:
					//对方关闭的套接字
					fprintf(stderr, "socket-server: write to %d (fd=%d) error :%s.\n",id,s->fd,strerror(errno));
					force_close(ss,s,result);
					so.free_func(request->buffer);
					return SOCKET_CLOSE;
				}
			}

			// 可以把数据拷贝到内核缓冲区中
			if (n == so.sz) {
				so.free_func(request->buffer);
				return -1;
			}

			//运行到这里，说明未将要发送的数据全部拷贝到内核缓冲区
			// 将未发送的部分添加到应用层缓冲区中 
			append_sendbuffer(ss, s, request, n);	// add to high priority list, even priority == PRIORITY_LOW


		} else {

			// udp
			//udp数据
			if (udp_address == NULL) {
				udp_address = s->p.udp_address;
			}
			union sockaddr_all sa;
			//获得地址
			socklen_t sasz = udp_socket_address(s, udp_address, &sa);

			//直接发送
			int n = sendto(s->fd, so.buffer, so.sz, 0, &sa.s, sasz);

			if (n != so.sz) {
				//数据没有发送完
				//将数据添加到缓冲区
				append_sendbuffer_udp(ss,s,priority,request,udp_address);
			} else {
			
				so.free_func(request->buffer);
				return -1;
			}
		}

		//运行到这里，说明数据没有发送完，要关注可写事件
		sp_write(ss->event_fd, s->fd, s, true);

		//如果应用层缓冲区原来就有遗留的数据,直接将要发送的数据添加到缓冲区中	
	} else {
	
		if (s->protocol == PROTOCOL_TCP) {
			if (priority == PRIORITY_LOW) {
				append_sendbuffer_low(ss, s, request);
			} else {
				append_sendbuffer(ss, s, request, 0);
			}
		} else {
			if (udp_address == NULL) {
				udp_address = s->p.udp_address;
			}
			append_sendbuffer_udp(ss,s,priority,request,udp_address);
		}
	}
	return -1;
}



//将描述符加入到socket_server的socket数组中，并没有加入到epoll中，只是将状态标记为SOCKET_TYPE_PLISTEN
static int
listen_socket(struct socket_server *ss, struct request_listen * request, struct socket_message *result) {

	//id是已经申请到的socket数组中预留的位置
	int id = request->id;
	int listen_fd = request->fd;

	//初始化一个socket,  false表示不会加入到epoll中管理
	//就是将socket描述符存入到socket_server中的数组
	struct socket *s = new_fd(ss, id, listen_fd, PROTOCOL_TCP, request->opaque, false);
	if (s == NULL) {
		goto _failed;
	}
	s->type = SOCKET_TYPE_PLISTEN;
	return -1;
_failed:
	close(listen_fd);
	result->opaque = request->opaque;
	result->id = id;
	result->ud = 0;
	result->data = "reach skynet socket number limit";
	ss->slot[HASH_ID(id)].type = SOCKET_TYPE_INVALID;

	return SOCKET_ERROR;
}


//关闭套接字 result是传出参数
static int
close_socket(struct socket_server *ss, struct request_close *request, struct socket_message *result) {
	int id = request->id;
	struct socket * s = &ss->slot[HASH_ID(id)];

	//说明已经关闭或者无效的socket
	if (s->type == SOCKET_TYPE_INVALID || s->id != id) {
		result->id = id;
		result->opaque = request->opaque;
		result->ud = 0;
		result->data = NULL;
		return SOCKET_CLOSE;
	}

	//如果应用层缓冲区不为空
	if (!send_buffer_empty(s)) {

		//发送应用层缓冲区数据
		int type = send_buffer(ss,s,result);
		if (type != -1)
			return type;
	}

	//如果已经发送完
	if (request->shutdown || send_buffer_empty(s)) {
		force_close(ss,s,result);
		result->id = id;
		result->opaque = request->opaque;
		return SOCKET_CLOSE;
	}
	//如果数据没有发送完,标记要关闭
	s->type = SOCKET_TYPE_HALFCLOSE;

	return -1;
}


//将stdin,stdout这种类型的文件描述符加入到epoll管理，这种类型称为SOCKET_TYPE_BIND
//result是传出参数
static int
bind_socket(struct socket_server *ss, struct request_bind *request, struct socket_message *result) {

	int id = request->id;
	result->id = id;
	result->opaque = request->opaque;
	result->ud = 0;
	
	//将fd存入到socket_server中的socket数组    true表示加入到epoll中管理
	struct socket *s = new_fd(ss, id, request->fd, PROTOCOL_TCP, request->opaque, true);
	if (s == NULL) {
		result->data = "reach skynet socket number limit";
		return SOCKET_ERROR;
	}
	//设置非阻塞
	sp_nonblocking(request->fd);
	//标记状态
	s->type = SOCKET_TYPE_BIND;
	
	result->data = "binding";
	return SOCKET_OPEN;
}


//SOCKET_TYPE_PACCEPT已经连接，当没有加入到epoll中
//SOCKET_TYPE_PLISTEN,监听套接字，当并没有加入到epoll中

//将SOCKET_TYPE_PACCEPT或者SOCKET_TYPE_PLISTEN这两种类型socket,将其加入epoll管理，并更新其状态
//result是传出参数
static int
start_socket(struct socket_server *ss, struct request_start *request, struct socket_message *result) {
	int id = request->id;
	result->id = id;
	result->opaque = request->opaque;
	result->ud = 0;
	result->data = NULL;
	
	struct socket *s = &ss->slot[HASH_ID(id)];

	//无效的socket
	if (s->type == SOCKET_TYPE_INVALID || s->id !=id) {
		result->data = "invalid socket";
		return SOCKET_ERROR;
	}

	//将SOCKET_TYPE_PACCEPT或者SOCKET_TYPE_PLISTEN这两种类型
	if (s->type == SOCKET_TYPE_PACCEPT || s->type == SOCKET_TYPE_PLISTEN) {

		//加入到epoll管理
		if (sp_add(ss->event_fd, s->fd, s)) {
			force_close(ss, s, result);
			result->data = strerror(errno);
			return SOCKET_ERROR;
		}
		//修改状态   SOCKET_TYPE_CONNECTED已经连接的，并且已经加入到了epoll中
		s->type = (s->type == SOCKET_TYPE_PACCEPT) ? SOCKET_TYPE_CONNECTED : SOCKET_TYPE_LISTEN;
		s->opaque = request->opaque;
		result->data = "start";
		return SOCKET_OPEN;

		//如果是已经连接的，并且加入到epoll中的
	} else if (s->type == SOCKET_TYPE_CONNECTED) {
		// todo: maybe we should send a message SOCKET_TRANSFER to s->opaque
		s->opaque = request->opaque;
		result->data = "transfer";
		return SOCKET_OPEN;
	}
	// if s->type == SOCKET_TYPE_HALFCLOSE , SOCKET_CLOSE message will send later
	return -1;
}

//调用setsockopt函数设置socket属性
static void
setopt_socket(struct socket_server *ss, struct request_setopt *request) {
	int id = request->id;

	//找到socket
	struct socket *s = &ss->slot[HASH_ID(id)];
	if (s->type == SOCKET_TYPE_INVALID || s->id !=id) {
		return;
	}
	int v = request->value;

	//调用setsockopt()设置
	setsockopt(s->fd, IPPROTO_TCP, request->what, &v, sizeof(v));
}


//从管道读取数据,读到buffer中
static void
block_readpipe(int pipefd, void *buffer, int sz) {

	for (;;) {
		
		int n = read(pipefd, buffer, sz);
		if (n<0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "socket-server : read pipe error %s.\n",strerror(errno));
			return;
		}
		// must atomic read from a pipe
		assert(n == sz);
		return;
	}
}


// 判断管道是否有命令 使用select来管理 没有使用epoll时为了提高命令的检测频率
static int
has_cmd(struct socket_server *ss) {
	
	struct timeval tv = {0,0};
	int retval;

	//recvctrl_fd是创建的管道的读端
	//将管道读端的描述符加入早集合中，集合只监听一个描述符
	FD_SET(ss->recvctrl_fd, &ss->rfds);

	//使用select  立即返回
	retval = select(ss->recvctrl_fd+1, &ss->rfds, NULL, NULL, &tv);

	//因为select只监听管道读端描述符
	if (retval == 1) {
		return 1;
	}
	return 0;
}


//添加udp的socket
//将fd加入到socket_server的socket数组中,加入到epoll中管理
static void
add_udp_socket(struct socket_server *ss, struct request_udp *udp) {
	int id = udp->id;
	int protocol;
	if (udp->family == AF_INET6) {
		protocol = PROTOCOL_UDPv6;
	} else {
		protocol = PROTOCOL_UDP;
	}
	//将fd加入到socket_server的socket数组中，true表示还会加入到epoll中管理
	struct socket *ns = new_fd(ss, id, udp->fd, protocol, udp->opaque, true);
	if (ns == NULL) {
		close(udp->fd);
		ss->slot[HASH_ID(id)].type = SOCKET_TYPE_INVALID;
		return;
	}
	ns->type = SOCKET_TYPE_CONNECTED;
	memset(ns->p.udp_address, 0, sizeof(ns->p.udp_address));
}




//设置udp的地址信息  result是传出参数
static int 
set_udp_address(struct socket_server *ss, struct request_setudp *request, struct socket_message *result) {
	int id = request->id;
	struct socket *s = &ss->slot[HASH_ID(id)];
	if (s->type == SOCKET_TYPE_INVALID || s->id !=id) {
		return -1;
	}
	int type = request->address[0];
	if (type != s->protocol) {
		// protocol mismatch
		result->opaque = s->opaque;
		result->id = s->id;
		result->ud = 0;
		result->data = "protocol mismatch";

		return SOCKET_ERROR;
	}
	//设置
	if (type == PROTOCOL_UDP) {
		memcpy(s->p.udp_address, request->address, 1+2+4);	// 1 type, 2 port, 4 ipv4
	} else {
		memcpy(s->p.udp_address, request->address, 1+2+16);	// 1 type, 2 port, 16 ipv6
	}
	return -1;
}

// return type

// 有命令 把命令解析出来 根据相应的类型调用相应的函数
//result是传入传出参数
static int
ctrl_cmd(struct socket_server *ss, struct socket_message *result) {

	//管道读端的描述符
	int fd = ss->recvctrl_fd;
	
	// the length of message is one byte, so 256+8 buffer size is enough.
	uint8_t buffer[256];
	uint8_t header[2];

	//从管道中读取数据		抱头  读入到header缓冲区中
	block_readpipe(fd, header, sizeof(header));

	int type = header[0];
	int len = header[1];

	//从管道中读取数据		包体   读到buffer中
	block_readpipe(fd, buffer, len);

	// ctrl command only exist in local fd, so don't worry about endian.

	//根据命令调用相应的函数
	switch (type) {
		
	case 'S':
		//将SOCKET_TYPE_PACCEPT或者SOCKET_TYPE_PLISTEN这两种类型socket,将其加入epoll管理，并更新其状态
		return start_socket(ss,(struct request_start *)buffer, result);
	case 'B':
		// 将描述符加入到socket_server的socket数组中 并将状态标记为 SOCKET_TYPE_BIND
		return bind_socket(ss,(struct request_bind *)buffer, result);
	case 'L':

		//将描述符加入到socket_server的socket数组中，并没有加入到epoll中，只是将状态标记为SOCKET_TYPE_PLISTEN
		return listen_socket(ss,(struct request_listen *)buffer, result);
	case 'K':
		//关闭套接字
		return close_socket(ss,(struct request_close *)buffer, result);
	case 'O':

		//先调用socket(),创建套接字
		// 调用connect()
		//连接成功返回SOCKET_OPEN,未连接成功(处于连接中)返回-1,出错返回SOCKET_ERROR
		return open_socket(ss, (struct request_open *)buffer, result);

		//表示要推出
	case 'X':
		result->opaque = 0;
		result->id = 0;
		result->ud = 0;
		result->data = NULL;
		return SOCKET_EXIT;
	case 'D':
		//发送数据,使用的是高优先级的缓冲区
		return send_socket(ss, (struct request_send *)buffer, result, PRIORITY_HIGH, NULL);
		
	case 'P':
		//发送数据,使用的是低优先级的缓冲区
		return send_socket(ss, (struct request_send *)buffer, result, PRIORITY_LOW, NULL);

	case 'A': {
		//发送udp数据 ,使用的是高优先级的缓冲区
		struct request_send_udp * rsu = (struct request_send_udp *)buffer;
		return send_socket(ss, &rsu->send, result, PRIORITY_HIGH, rsu->address);
	}

	case 'C':
		//设置udp的地址信息
		return set_udp_address(ss, (struct request_setudp *)buffer, result);

	case 'T':
		//调用setsockopt()设置描述符属性
		setopt_socket(ss, (struct request_setopt *)buffer);
		return -1;
		
	case 'U':
		//将udp类型的  fd加入到socket_server的socket数组中,加入到epoll中管理
		add_udp_socket(ss, (struct request_udp *)buffer);
		return -1;
	default:
		fprintf(stderr, "socket-server: Unknown ctrl %c.\n",type);
		return -1;
	};

	return -1;
}

// return -1 (ignore) when error
//tcp类型描述符 可读事件到来，执行该函数 主要是调用read(),通过result传出读出的数据，在堆上 
static int
forward_message_tcp(struct socket_server *ss, struct socket *s, struct socket_message * result) {

	int sz = s->p.size;
	char * buffer = MALLOC(sz);

	//调用read()读取数据 
	int n = (int)read(s->fd, buffer, sz);
	
	if (n<0) {
		FREE(buffer);
		switch(errno) {
		case EINTR:
			break;
		case AGAIN_WOULDBLOCK:
			fprintf(stderr, "socket-server: EAGAIN capture.\n");
			break;
		default:
			// close when error
			//错误则强制关闭
			force_close(ss, s, result);
			result->data = strerror(errno);
			return SOCKET_ERROR;
		}
		return -1;
	}

	//对方关闭
	if (n==0) {
		FREE(buffer);
		//关闭套接字
		force_close(ss, s, result);
		return SOCKET_CLOSE;
	}

	// half close 半关闭丢掉消息,即之前标记了要关闭的套接字
	if (s->type == SOCKET_TYPE_HALFCLOSE) {
		// discard recv data
		FREE(buffer);
		return -1;
	}

	//调整读取数据缓冲区的大小
	if (n == sz) {
		s->p.size *= 2;
	} else if (sz > MIN_READ_BUFFER && n*2 < sz) {
		s->p.size /= 2;
	}

	//handle
	result->opaque = s->opaque;
	
	result->id = s->id;
	
	//ud保存的是数据的大小
	result->ud = n;
	//buffer在堆中分配
	result->data = buffer;
	return SOCKET_DATA;
}

//从 union sockaddr_all *sa中 获取udp地址  通过udp_address传出
static int
gen_udp_address(int protocol, union sockaddr_all *sa, uint8_t * udp_address) {
	int addrsz = 1;
	udp_address[0] = (uint8_t)protocol;

	if (protocol == PROTOCOL_UDP) {
		memcpy(udp_address+addrsz, &sa->v4.sin_port, sizeof(sa->v4.sin_port));
		addrsz += sizeof(sa->v4.sin_port);
		memcpy(udp_address+addrsz, &sa->v4.sin_addr, sizeof(sa->v4.sin_addr));
		addrsz += sizeof(sa->v4.sin_addr);
	} else {
		memcpy(udp_address+addrsz, &sa->v6.sin6_port, sizeof(sa->v6.sin6_port));
		addrsz += sizeof(sa->v6.sin6_port);
		memcpy(udp_address+addrsz, &sa->v6.sin6_addr, sizeof(sa->v6.sin6_addr));
		addrsz += sizeof(sa->v6.sin6_addr);
	}
	return addrsz;
}

//udp描述符，可读时调用该函数 调用recvfrom()函数 ,result是传出参数，可以将读到的数据传出
static int
forward_message_udp(struct socket_server *ss, struct socket *s, struct socket_message * result) {
	union sockaddr_all sa;
	socklen_t slen = sizeof(sa);

	//接受数据,存入到udpbuffer中 
	int n = recvfrom(s->fd, ss->udpbuffer,MAX_UDP_PACKAGE,0,&sa.s,&slen);
	if (n<0) {
		switch(errno) {
		case EINTR:
		case AGAIN_WOULDBLOCK:
			break;
		default:
			// close when error
			force_close(ss, s, result);
			result->data = strerror(errno);
			return SOCKET_ERROR;
		}
		return -1;
	}
	uint8_t * data;
	if (slen == sizeof(sa.v4)) {
		if (s->protocol != PROTOCOL_UDP)
			return -1;
		data = MALLOC(n + 1 + 2 + 4);
		//将地址消息存入到data末尾中
		gen_udp_address(PROTOCOL_UDP, &sa, data + n);
	} else {
		if (s->protocol != PROTOCOL_UDPv6)
			return -1;
		data = MALLOC(n + 1 + 2 + 16);
		gen_udp_address(PROTOCOL_UDPv6, &sa, data + n);
	}

	//将接收到的数据复制到data中
	memcpy(data, ss->udpbuffer, n);

	result->opaque = s->opaque;
	result->id = s->id;
	result->ud = n;
	result->data = (char *)data;

	return SOCKET_UDP;
}


// 尝试连接中的套接字可写事件发生, 可能成功，也可能出错
static int
report_connect(struct socket_server *ss, struct socket *s, struct socket_message *result) {
	int error;
	
	socklen_t len = sizeof(error);  
	int code = getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &error, &len);  

	//如果出错信息
	if (code < 0 || error) {  
		force_close(ss,s, result);
		if (code >= 0)
			result->data = strerror(error);
		else
			result->data = strerror(errno);
		return SOCKET_ERROR;
	} else {

	//如果是连接成功
		s->type = SOCKET_TYPE_CONNECTED;
		result->opaque = s->opaque;
		result->id = s->id;
		result->ud = 0;
		//发送数据 缓存区 为空
		if (send_buffer_empty(s)) {
			//关注可读事件
			sp_write(ss->event_fd, s->fd, s, false);
		}
		union sockaddr_all u;
		socklen_t slen = sizeof(u);
		if (getpeername(s->fd, &u.s, &slen) == 0) {
			void * sin_addr = (u.s.sa_family == AF_INET) ? (void*)&u.v4.sin_addr : (void *)&u.v6.sin6_addr;
			if (inet_ntop(u.s.sa_family, sin_addr, ss->buffer, sizeof(ss->buffer))) {
				result->data = ss->buffer;
				return SOCKET_OPEN;
			}
		}
		result->data = NULL;
		return SOCKET_OPEN;
	}
}

// return 0 when failed, or -1 when file limit
//监听套接字可读事件到来，调用该函数,调用了accept

//这里的s结构体对象保存了监听套接字
//调用accept函数 ,将新的描述符加入到了socket_server的socket数组中，但并没有加入到epoll中
//result是传出参数
static int
report_accept(struct socket_server *ss, struct socket *s, struct socket_message *result) {

	union sockaddr_all u;
	socklen_t len = sizeof(u);

	//调用了accept
	int client_fd = accept(s->fd, &u.s, &len);
	if (client_fd < 0) {
		if (errno == EMFILE || errno == ENFILE) {
			result->opaque = s->opaque;
			result->id = s->id;
			result->ud = 0;
			result->data = strerror(errno);
			return -1;
		} else {
			return 0;
		}
	}

	//从socket池中申请一个socket,实质就是得到一个存储 socket指针的数组位置
	
	//struct socket_server *ss是服务器网络部分的一个抽象,struct socket是每个描述符的抽象
	//ss中有保存每个描述符对应的结构体指针在数组中位值hash前的值

	//从socket_server数组中得到一个可用的位置
	int id = reserve_id(ss);

	//说明应用层socket已经用完
	if (id < 0) {
		close(client_fd);
		return 0;
	}

	//设置心跳
	socket_keepalive(client_fd);
	
	//设置为非阻塞
	sp_nonblocking(client_fd);

	//初始化申请到的socket,为加入epoll管理,
	//struct socket是对每一个套接字的抽象,一个包含了套接字的结构体
	//得到一个strutc socket对象，存储到了服务器对象struct socket_server的数组中，false表示该描述符并未添加到epoll管理

	//将client_fd存入到socket_server的socket数组的中,false表示不会加入到epoll中 
	struct socket *ns = new_fd(ss, id, client_fd, PROTOCOL_TCP, s->opaque, false);
	if (ns == NULL) {
		close(client_fd);
		return 0;
	}

	//标记状态是已经连接，但并未加入到epoll中管理
	ns->type = SOCKET_TYPE_PACCEPT;

	//skynet_context对应的标号handle
	result->opaque = s->opaque;

	//socket结构体对应的在skynet_server中的socket数组中的标号
	result->id = s->id;

	//此时ud为
	result->ud = id;

	result->data = NULL;

	void * sin_addr = (u.s.sa_family == AF_INET) ? (void*)&u.v4.sin_addr : (void *)&u.v6.sin6_addr;
	
	int sin_port = ntohs((u.s.sa_family == AF_INET) ? u.v4.sin_port : u.v6.sin6_port);
	char tmp[INET6_ADDRSTRLEN];

	// 将对等方的ip port保存下来,返回1表示成功
	if (inet_ntop(u.s.sa_family, sin_addr, tmp, sizeof(tmp))) {

		//将数据存储到了服务器对象的 buffer缓冲区中
		snprintf(ss->buffer, sizeof(ss->buffer), "%s:%d", tmp, sin_port);

		
		result->data = ss->buffer;
	}

	return 1;
}


//清除关闭的event
static inline void 
clear_closed_event(struct socket_server *ss, struct socket_message * result, int type) {

	if (type == SOCKET_CLOSE || type == SOCKET_ERROR) {
		int id = result->id;
		int i;
		//event_n是epoll_wait返回的活跃的描述符的个数
		for (i=ss->event_index; i<ss->event_n; i++) {

			//ss->ev是存储epoll_wait()返回的活跃描述符对应的结构体
			struct event *e = &ss->ev[i];

			//得到结构体指向的socket
			struct socket *s = e->s;

			if (s) {
				
				if (s->type == SOCKET_TYPE_INVALID && s->id == id) {
					e->s = NULL;
					break;
				}
			}
		}
	}
}


// return type
// 有命令的话 优先检测命令
// 没有命令的时候
//调用epoll

//result,more都是传入传出参数,传入时,more==1
int 
socket_server_poll(struct socket_server *ss, struct socket_message * result, int * more) {

	for (;;) {

		//检测命令
		if (ss->checkctrl) {

			// has_cmd内部调用select函数 判断管道是否有命令  使用select来管理 没有使用epoll时为了提高命令的检测频率
			if (has_cmd(ss)) {

				// 处理命令,根据命令字符串，调用相应的处理函数
				int type = ctrl_cmd(ss, result);
				if (type != -1) {
					//关闭存储epoll_wait返回的数组中的关闭的
					clear_closed_event(ss, result, type);

					return type;
				} else
					continue;
			} 
			else 
			{
			
			//已经没有控制命令了
				ss->checkctrl = 0;
			}
		}

		 // 当前的处理序号最大了 即处理完了 继续等待事件的到来
		if (ss->event_index == ss->event_n) {


			//调用epoll_wait函数,
			//int epoll_wait(int epfd, struct epoll_event * events, intmaxevents, int timeout);
			//原来是struct epoll_events结构体，events->data.ptr==s

			//ss->ev是返回的活跃的描述符对应的结构体的数组
			//event_n是epoll_wait()的返回值
			ss->event_n = sp_wait(ss->event_fd, ss->ev, MAX_EVENT);
			
			ss->checkctrl = 1;
			//传入时   more=1
			//现在修改为0,可以标记调用了 sp_wait()
			if (more) {
				*more = 0;
			}

			ss->event_index = 0;
			if (ss->event_n <= 0) {
				ss->event_n = 0;
				return -1;
			}
		}

		/*
			struct event {
				void * s;
				bool read;
				bool write;
			};	
		*/

		//得到一个活跃的描述符对应的结构体
		struct event *e = &ss->ev[ss->event_index++];

		//得到对应的socket对象指针
		struct socket *s = e->s;
		if (s == NULL) {
			// dispatch pipe message at beginning
			continue;
		}

		//得到有事件到来的文件描述符 对应的 socket对象的 类型
		switch (s->type) {
			
	// 尝试连接中的套接字 可写事件发生, 可能成功，也可能出错
		case SOCKET_TYPE_CONNECTING:
			
			return report_connect(ss, s, result);

		//如果是监听套接字有事件发生
		case SOCKET_TYPE_LISTEN: {

			//调用,成功返回1    
			//调用accept函数 ,将新的描述符加入到了socket_server的socket数组中，但并没有加入到epoll中
			int ok = report_accept(ss, s, result);
			if (ok > 0) {
				return SOCKET_ACCEPT;
			} if (ok < 0 ) {
				return SOCKET_ERROR;
			}
			// when ok == 0, retry
			break;
		}
		case SOCKET_TYPE_INVALID:
			fprintf(stderr, "socket-server: invalid socket\n");
			break;

			
		default:
			//已经连接的套接字,即调用accept得到的描述符活跃

			//如果是可读
			if (e->read) {
				int type;
				//如果是tcp
				if (s->protocol == PROTOCOL_TCP) {

					//读取数据，数据在堆上，通过result传出
					type = forward_message_tcp(ss, s, result);
				} else {
				//udp类型
					type = forward_message_udp(ss, s, result);
					if (type == SOCKET_UDP) {
						// try read again
						--ss->event_index;
						return SOCKET_UDP;
					}
				}
				if (e->write && type != SOCKET_CLOSE && type != SOCKET_ERROR) {
					// Try to dispatch write message next step if write flag set.
					e->read = false;
					--ss->event_index;
				}
				if (type == -1)
					break;				
				return type;
			}

			//如果可写
			if (e->write) {
				// 可写事件 从应用层缓冲区 取出数据发送
				
				int type = send_buffer(ss, s, result);
				if (type == -1)
					break;
				return type;
			}
			break;
		}
	}
}


//向管道发送请求
static void
send_request(struct socket_server *ss, struct request_package *request, char type, int len) {
	request->header[6] = (uint8_t)type;
	request->header[7] = (uint8_t)len;
	for (;;) {

		//向管道写数据
		int n = write(ss->sendctrl_fd, &request->header[6], len+2);

		if (n<0) {
			if (errno != EINTR) {
				fprintf(stderr, "socket-server : send ctrl command error %s.\n", strerror(errno));
			}
			continue;
		}
		assert(n == len+2);
		return;
	}
}


//用于connect,准备一个请求包 struct request_package 
//req是传入传出参数
static int
open_request(struct socket_server *ss, struct request_package *req, uintptr_t opaque, const char *addr, int port) {
	int len = strlen(addr);

	//如果数据过大
	if (len + sizeof(req->u.open) >= 256) {
		fprintf(stderr, "socket-server : Invalid addr %s.\n",addr);
		return -1;
	}

	//申请一个应用层socket,就是在skynet_server的socket数组中得到一个可以使用的位置
	int id = reserve_id(ss);

	
	if (id < 0)
		return -1;

	
	req->u.open.opaque = opaque;
	req->u.open.id = id;
	req->u.open.port = port;

	memcpy(req->u.open.host, addr, len);
	req->u.open.host[len] = '\0';

	return len;
}

 
//非阻塞的连接  对应的调用 open_socket()函数
int 
socket_server_connect(struct socket_server *ss, uintptr_t opaque, const char * addr, int port) {
	struct request_package request;

	//组装一个请求包,通过request传出
	int len = open_request(ss, &request, opaque, addr, port);
	if (len < 0)
		return -1;

	//向管道发送O命令   ,在循环中对应的调用 open_socket()函数
	send_request(ss, &request, 'O', sizeof(request.u.open) + len);

	return request.u.open.id;
}



//销毁buffer
static void
free_buffer(struct socket_server *ss, const void * buffer, int sz) {
	struct send_object so;
	send_object_init(ss, &so, (void *)buffer, sz);
	so.free_func((void *)buffer);
}



// return -1 when error
//请求发送数据 对应的调用send_socket()函数,
int64_t 
socket_server_send(struct socket_server *ss, int id, const void * buffer, int sz) {
	struct socket * s = &ss->slot[HASH_ID(id)];
	if (s->id != id || s->type == SOCKET_TYPE_INVALID) {
		free_buffer(ss, buffer, sz);
		return -1;
	}

	struct request_package request;
	request.u.send.id = id;
	request.u.send.sz = sz;
	request.u.send.buffer = (char *)buffer;

	//发送D命令，对应的调用 send_socket()函数,使用的是高优先级的缓冲区
	send_request(ss, &request, 'D', sizeof(request.u.send));
	return s->wb_size;
}


//请求发送数据  对应的调用 send_socket()函数,使用的是低优先级的缓冲区
void 
socket_server_send_lowpriority(struct socket_server *ss, int id, const void * buffer, int sz) {
	struct socket * s = &ss->slot[HASH_ID(id)];
	if (s->id != id || s->type == SOCKET_TYPE_INVALID) {
		free_buffer(ss, buffer, sz);
		return;
	}

	struct request_package request;
	request.u.send.id = id;
	request.u.send.sz = sz;
	request.u.send.buffer = (char *)buffer;

	//发送P命令  对应的调用 send_socket()函数,使用的是低优先级的缓冲区
	send_request(ss, &request, 'P', sizeof(request.u.send));
}

//请求退出
void
socket_server_exit(struct socket_server *ss) {
	struct request_package request;
	send_request(ss, &request, 'X', 0);
}


//请求关闭  对应的调用 close_socket()
void
socket_server_close(struct socket_server *ss, uintptr_t opaque, int id) {
	struct request_package request;
	request.u.close.id = id;
	request.u.close.shutdown = 0;
	request.u.close.opaque = opaque;
	//对应的调用 close_socket()
	send_request(ss, &request, 'K', sizeof(request.u.close));
}


//请求半关闭  对应的调用close_socket()
void
socket_server_shutdown(struct socket_server *ss, uintptr_t opaque, int id) {
	struct request_package request;
	request.u.close.id = id;
	request.u.close.shutdown = 1;
	request.u.close.opaque = opaque;

	//对应的调用close_socket()
	send_request(ss, &request, 'K', sizeof(request.u.close));
}


// return -1 means failed
// or return AF_INET or AF_INET6
//调用socket函数，创建套接字，并且调用了bind函数
static int
do_bind(const char *host, int port, int protocol, int *family) {

	int fd;
	int status;
	int reuse = 1;
	struct addrinfo ai_hints;
	struct addrinfo *ai_list = NULL;
	char portstr[16];
	if (host == NULL || host[0] == 0) {
		host = "0.0.0.0";	// INADDR_ANY
	}
	sprintf(portstr, "%d", port);
	memset( &ai_hints, 0, sizeof( ai_hints ) );
	ai_hints.ai_family = AF_UNSPEC;
	if (protocol == IPPROTO_TCP) {
		ai_hints.ai_socktype = SOCK_STREAM;
	} else {
		assert(protocol == IPPROTO_UDP);
		ai_hints.ai_socktype = SOCK_DGRAM;
	}
	ai_hints.ai_protocol = protocol;

	status = getaddrinfo( host, portstr, &ai_hints, &ai_list );
	if ( status != 0 ) {
		return -1;
	}
	*family = ai_list->ai_family;

	//调用socket函数,创建套接字
	fd = socket(*family, ai_list->ai_socktype, 0);
	if (fd < 0) {
		goto _failed_fd;
	}

	//设置地址重复利用
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (void *)&reuse, sizeof(int))==-1) {
		goto _failed;
	}

	//调用bind函数
	status = bind(fd, (struct sockaddr *)ai_list->ai_addr, ai_list->ai_addrlen);
	if (status != 0)
		goto _failed;

	freeaddrinfo( ai_list );
	return fd;
_failed:
	close(fd);
_failed_fd:
	freeaddrinfo( ai_list );
	return -1;
}

//调用listen函数,先调用do_bind() 函数
static int
do_listen(const char * host, int port, int backlog) {
	int family = 0;
					//socket()  bind()
	int listen_fd = do_bind(host, port, IPPROTO_TCP, &family);

	if (listen_fd < 0) {
		return -1;
	}

	//设置为监听
	if (listen(listen_fd, backlog) == -1) {
		close(listen_fd);
		return -1;
	}
	return listen_fd;
}


//调用do_listen()函数,请求   L命令
//对应的调用 listen_socket()函数
//将描述符加入到socket_server的socket数组中，并没有加入到epoll中，只是将状态标记为SOCKET_TYPE_PLISTEN
//相当于
//socket()  bind()  listen()  存入socket数组   但没有加入到epoll中管理
int 
socket_server_listen(struct socket_server *ss, uintptr_t opaque, const char * addr, int port, int backlog) {

				//socket()  bind()  listen() 
	int fd = do_listen(addr, port, backlog);
	if (fd < 0) {
		return -1;
	}
	struct request_package request;

	//得到一个预留的位置
	int id = reserve_id(ss);
	
	if (id < 0) {
		close(fd);
		return id;
	}
	request.u.listen.opaque = opaque;
	request.u.listen.id = id;
	request.u.listen.fd = fd;

	//对应的调用 listen_socket()函数
	//将描述符加入到socket_server的socket数组中，并没有加入到epoll中，只是将状态标记为SOCKET_TYPE_PLISTEN

	send_request(ss, &request, 'L', sizeof(request.u.listen));

	return id;
}


//发送'B'命令请求
// 对应的调用 bind_socket 将描述符加入到socket_server的socket数组中 并将状态标记为 SOCKET_TYPE_BIND
int
socket_server_bind(struct socket_server *ss, uintptr_t opaque, int fd) {
	struct request_package request;
	int id = reserve_id(ss);
	if (id < 0)
		return -1;
	request.u.bind.opaque = opaque;
	request.u.bind.id = id;
	request.u.bind.fd = fd;
	// 对应的调用 bind_socket 将描述符加入到socket_server的socket数组中 并将状态标记为 SOCKET_TYPE_BIND
	send_request(ss, &request, 'B', sizeof(request.u.bind));
	return id;
}


//发送'S'命令请求
//对应的调用 start_socket 
//将SOCKET_TYPE_PACCEPT或者SOCKET_TYPE_PLISTEN这两种类型socket,将其加入epoll管理，并更新其状态
void 
socket_server_start(struct socket_server *ss, uintptr_t opaque, int id) {
	struct request_package request;
	request.u.start.id = id;
	request.u.start.opaque = opaque;

	
	//对应的调用 start_socket 
	//将SOCKET_TYPE_PACCEPT或者SOCKET_TYPE_PLISTEN这两种类型socket,将其加入epoll管理，并更新其状态
	send_request(ss, &request, 'S', sizeof(request.u.start));
}


//发送'T'命令请求
//对应的调用setopt_socket  调用setsockopt()设置描述符属性 
//这里是设置TCP_NODELAY属性
void
socket_server_nodelay(struct socket_server *ss, int id) {
	struct request_package request;
	request.u.setopt.id = id;
	request.u.setopt.what = TCP_NODELAY;
	request.u.setopt.value = 1;
	
	//对应的调用setopt_socket  调用setsockopt()设置描述符属性 
	send_request(ss, &request, 'T', sizeof(request.u.setopt));
}


void 
socket_server_userobject(struct socket_server *ss, struct socket_object_interface *soi) {
	ss->soi = *soi;
}

// UDP

//发送'U'命令请求
//对应的调用add_udp_socket  将udp类型的  fd加入到socket_server的socket数组中,加入到epoll中管理
int 
socket_server_udp(struct socket_server *ss, uintptr_t opaque, const char * addr, int port) {
	int fd;
	int family;
	if (port != 0 || addr != NULL) {
		// bind
		fd = do_bind(addr, port, IPPROTO_UDP, &family);
		if (fd < 0) {
			return -1;
		}
	} else {
		family = AF_INET;
		fd = socket(family, SOCK_DGRAM, 0);
		if (fd < 0) {
			return -1;
		}
	}
	sp_nonblocking(fd);

	int id = reserve_id(ss);
	if (id < 0) {
		close(fd);
		return -1;
	}
	struct request_package request;
	request.u.udp.id = id;
	request.u.udp.fd = fd;
	request.u.udp.opaque = opaque;
	request.u.udp.family = family;
	
	//对应的调用add_udp_socket  将udp类型的  fd加入到socket_server的socket数组中,加入到epoll中管理
	send_request(ss, &request, 'U', sizeof(request.u.udp));	
	return id;
}

//发送'A'命令请求
//对应的调用 send_socket()发送udp数据

int64_t 
socket_server_udp_send(struct socket_server *ss, int id, const struct socket_udp_address *addr, const void *buffer, int sz) {
	struct socket * s = &ss->slot[HASH_ID(id)];
	if (s->id != id || s->type == SOCKET_TYPE_INVALID) {
		free_buffer(ss, buffer, sz);
		return -1;
	}

	struct request_package request;
	request.u.send_udp.send.id = id;
	request.u.send_udp.send.sz = sz;
	request.u.send_udp.send.buffer = (char *)buffer;

	const uint8_t *udp_address = (const uint8_t *)addr;
	int addrsz;
	switch (udp_address[0]) {
	case PROTOCOL_UDP:
		addrsz = 1+2+4;		// 1 type, 2 port, 4 ipv4
		break;
	case PROTOCOL_UDPv6:
		addrsz = 1+2+16;	// 1 type, 2 port, 16 ipv6
		break;
	default:
		free_buffer(ss, buffer, sz);
		return -1;
	}

	memcpy(request.u.send_udp.address, udp_address, addrsz);	

	//对应的调用 send_socket()发送udp数据
	send_request(ss, &request, 'A', sizeof(request.u.send_udp.send)+addrsz);
	return s->wb_size;
}

//发送'C'命令请求
//对应的调用set_udp_address 设置udp的地址信息
int
socket_server_udp_connect(struct socket_server *ss, int id, const char * addr, int port) {
	int status;
	struct addrinfo ai_hints;
	struct addrinfo *ai_list = NULL;
	char portstr[16];
	sprintf(portstr, "%d", port);
	memset( &ai_hints, 0, sizeof( ai_hints ) );
	ai_hints.ai_family = AF_UNSPEC;
	ai_hints.ai_socktype = SOCK_DGRAM;
	ai_hints.ai_protocol = IPPROTO_UDP;

	status = getaddrinfo(addr, portstr, &ai_hints, &ai_list );
	if ( status != 0 ) {
		return -1;
	}
	struct request_package request;
	request.u.set_udp.id = id;
	int protocol;

	if (ai_list->ai_family == AF_INET) {
		protocol = PROTOCOL_UDP;
	} else if (ai_list->ai_family == AF_INET6) {
		protocol = PROTOCOL_UDPv6;
	} else {
		freeaddrinfo( ai_list );
		return -1;
	}

	int addrsz = gen_udp_address(protocol, (union sockaddr_all *)ai_list->ai_addr, request.u.set_udp.address);

	freeaddrinfo( ai_list );

	//对应的调用set_udp_address 设置udp的地址信息
	send_request(ss, &request, 'C', sizeof(request.u.set_udp) - sizeof(request.u.set_udp.address) +addrsz);

	return 0;
}


const struct socket_udp_address *
socket_server_udp_address(struct socket_server *ss, struct socket_message *msg, int *addrsz) {
	uint8_t * address = (uint8_t *)(msg->data + msg->ud);
	int type = address[0];
	switch(type) {
	case PROTOCOL_UDP:
		*addrsz = 1+2+4;
		break;
	case PROTOCOL_UDPv6:
		*addrsz = 1+2+16;
		break;
	default:
		return NULL;
	}
	return (const struct socket_udp_address *)address;
}


