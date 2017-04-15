#ifndef socket_poll_h
#define socket_poll_h

#include <stdbool.h>

typedef int poll_fd;

//ͨ������epoll_wait�󣬷��صĻ�Ծ���������Ľṹ��
struct event {
	void * s;		//ָ����Ƿ�װ��socket�ṹ�� 
	bool read;   //�Ƿ�ɶ�
	bool write;	 //�Ƿ��д	
};

//���epoll_create()�������Ƿ���Ч
static bool sp_invalid(poll_fd fd);


//����epoll_create()
static poll_fd sp_create();


//����close()
static void sp_release(poll_fd fd);

//����epoll_ctl
static int sp_add(poll_fd fd, int sock, void *ud);

//ɾ��
static void sp_del(poll_fd fd, int sock);


static void sp_write(poll_fd, int sock, void *ud, bool enable);

//����epoll_wait()
static int sp_wait(poll_fd, struct event *e, int max);

//�����ļ������������� 
static void sp_nonblocking(int sock);

#ifdef __linux__
#include "socket_epoll.h"
#endif

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined (__NetBSD__)
#include "socket_kqueue.h"
#endif

#endif
