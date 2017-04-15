#ifndef poll_socket_epoll_h
#define poll_socket_epoll_h

#include <netdb.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

//检查是否有效
static bool 
sp_invalid(int efd) {
	return efd == -1;
}

//调用epoll_create()
static int
sp_create() {
	return epoll_create(1024);
}

//调用close()
static void
sp_release(int efd) {
	close(efd);
}

//关注sock的可读事件   sock是描述符  ud是封装的socket结构体指针
static int 
sp_add(int efd, int sock, void *ud) {
	struct epoll_event ev;
	ev.events = EPOLLIN;
	ev.data.ptr = ud;
	if (epoll_ctl(efd, EPOLL_CTL_ADD, sock, &ev) == -1) {
		return 1;
	}
	return 0;
}


//将sock描述符从epoll中移除
static void 
sp_del(int efd, int sock) {
	epoll_ctl(efd, EPOLL_CTL_DEL, sock , NULL);
}

//根据enable判断是否监听描述符的EPOLLOUT事件
static void 
sp_write(int efd, int sock, void *ud, bool enable) {
	struct epoll_event ev;
	ev.events = EPOLLIN | (enable ? EPOLLOUT : 0);
	ev.data.ptr = ud;
	epoll_ctl(efd, EPOLL_CTL_MOD, sock, &ev);
}


//调用epoll_wait()    通过e返回活跃的描述符
static int 
sp_wait(int efd, struct event *e, int max) {
	struct epoll_event ev[max];
	
	int n = epoll_wait(efd , ev, max, -1);
	int i;
	for (i=0;i<n;i++) {
		//填写e[i]信息
		e[i].s = ev[i].data.ptr;
		unsigned flag = ev[i].events;

		e[i].write = (flag & EPOLLOUT) != 0;
		e[i].read = (flag & EPOLLIN) != 0;
	}

	return n;
}

//设置描述符为非阻塞
static void
sp_nonblocking(int fd) {
	int flag = fcntl(fd, F_GETFL, 0);
	if ( -1 == flag ) {
		return;
	}

	fcntl(fd, F_SETFL, flag | O_NONBLOCK);
}

#endif
