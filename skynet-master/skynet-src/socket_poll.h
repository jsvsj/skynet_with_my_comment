#ifndef socket_poll_h
#define socket_poll_h

#include <stdbool.h>

typedef int poll_fd;

//通过调用epoll_wait后，返回的活跃的描述符的结构体
struct event {
	void * s;		//指向的是封装的socket结构体 
	bool read;   //是否可读
	bool write;	 //是否可写	
};

//检查epoll_create()创建的是否有效
static bool sp_invalid(poll_fd fd);


//调用epoll_create()
static poll_fd sp_create();


//调用close()
static void sp_release(poll_fd fd);

//调用epoll_ctl
static int sp_add(poll_fd fd, int sock, void *ud);

//删除
static void sp_del(poll_fd fd, int sock);


static void sp_write(poll_fd, int sock, void *ud, bool enable);

//调用epoll_wait()
static int sp_wait(poll_fd, struct event *e, int max);

//设置文件描述符非阻塞 
static void sp_nonblocking(int sock);

#ifdef __linux__
#include "socket_epoll.h"
#endif

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined (__NetBSD__)
#include "socket_kqueue.h"
#endif

#endif
