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

//ȫ�ֵķ�����socket���Ӳ��ֵĳ���Ľṹ��
static struct socket_server * SOCKET_SERVER = NULL;


//��ʼ��ȫ�ֵ�SOCKET_SERVER,���õ� epoll_create(),��
void 
skynet_socket_init() {
	
	//����socker_server�ṹ��
	SOCKET_SERVER = socket_server_create();
}


//�����˳�
void
skynet_socket_exit() {
	
	//�����˳�,����X����
	socket_server_exit(SOCKET_SERVER);
}



//����socket_server�ṹ��
void
skynet_socket_free() {
	socket_server_release(SOCKET_SERVER);
	SOCKET_SERVER = NULL;
}

// mainloop thread
//��skynet_socket_poll�б�����
//ת����Ϣ��������Ϣ���͵���Ӧ��skynet_context����Ϣ������     result�Ǵ������
static void
forward_message(int type, bool padding, struct socket_message * result) {

/*
struct skynet_socket_message {
	int type;
	int id;
	int ud;
	//��������ʵ��buffer�ĳ�������
	char * buffer;
};

*/
	struct skynet_socket_message *sm;

	size_t sz = sizeof(*sm);

	//�����������,����accept,������ ���ӵ�IP,�����Ļ������ڷ���������� buffer�У�ֻ����resultҲָ��������
	//�������ĺô��Ǽ������ڴ������
	//��skynet_

	//�����result->data�л�������
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

	//����ж�������
	if (padding) {
		sm->buffer = NULL;
		memcpy(sm+1, result->data, sz - sizeof(*sm));
	} else {
		//ָ���˶���������
		sm->buffer = result->data;
	}

	//skynet_context�е���Ϣ����
	
	struct skynet_message message;
	message.source = 0;
	message.session = 0;
	//����������
	message.data = sm;
	message.sz = sz | ((size_t)PTYPE_SOCKET << MESSAGE_TYPE_SHIFT);


	// �������Ϣ���͵���Ӧ�� handle 
	// ��Ҫ���� skynet_socket_close �ǻ����������¼�ѭ��

	//opaque��handle,Ҫ���ݸ���Ӧ��skynet_context�ı��
	if (skynet_context_push((uint32_t)result->opaque, &message)) {
		// todo: report somewhere to close socket
		// don't call skynet_socket_close here (It will block mainloop)
		skynet_free(sm->buffer);
		skynet_free(sm);
	}
}


// ����socket_server_poll()�����õ���Ϣ��ǰ������Ϣ

int 
skynet_socket_poll() {
	struct socket_server *ss = SOCKET_SERVER;
	assert(ss);

/*
struct socket_message {
	int id;				// Ӧ�ò��socket fd
	//��64λ��uintptr_t��unsigned long int �ı���,��32λ�ϣ���unsigned int �ı���
	uintptr_t opaque;	// ��skynet�ж�Ӧһ��actorʵ���handler

	// ����accept������˵�������ӵ�fd �������ݵ��������ݵĴ�С
	int ud;	// for accept, ud is new connection id ; for data, ud is size of data 
	
	char * data;
};


*/


	//result�Ǵ�������	
	struct socket_message result;

	int more = 1;

	//result�Ǵ��봫������,more�Ǵ��봫������   result�ܽ����������ݴ���
	int type = socket_server_poll(ss, &result, &more);

	switch (type) {
	case SOCKET_EXIT:
		return 0;
		//����������ݵ���
	case SOCKET_DATA:
		forward_message(SKYNET_SOCKET_TYPE_DATA, false, &result);
		break;
		//����ǹر����׽���
	case SOCKET_CLOSE:
		forward_message(SKYNET_SOCKET_TYPE_CLOSE, false, &result);
		break;

		//���ӽ������������߱����������Ѽ��뵽epoll��
	case SOCKET_OPEN:
		forward_message(SKYNET_SOCKET_TYPE_CONNECT, true, &result);
		break;

	case SOCKET_ERROR:
		forward_message(SKYNET_SOCKET_TYPE_ERROR, true, &result);
		break;

	//�����׽������¼�������socket_server_poll�Ѿ����յ����ӣ���������accept����
	//���ɵ����������� struct socketָ���Ѿ����뵽�� ���������� socket_server�е������У�
	//������������û�м��뵽epoll�й���
	case SOCKET_ACCEPT:
		//����Ϣ���뵽�� ��Ϣ������
		forward_message(SKYNET_SOCKET_TYPE_ACCEPT, true, &result);
		break;

	//udp�׽������ݵ���
	case SOCKET_UDP:
		forward_message(SKYNET_SOCKET_TYPE_UDP, false, &result);
		break;
	default:
		skynet_error(NULL, "Unknown socket message type %d.",type);
		return -1;
	}
	//��socket_server_poll()�ĵ��õ� sp_wait(),��epoll_wait()ʱ��more�ᱻ�޸�Ϊ0
	//���û�е���spoll_wait()��moreΪ1��more�ᱻ�޸�Ϊ0,����-1
	if (more) {
		return -1;
	}
	
	return 1;
}

//��鷢�ͻ��������ݴ�С��̫��ͷ��;���
static int
check_wsz(struct skynet_context *ctx, int id, void *buffer, int64_t wsz) {

	if (wsz < 0) {
		return -1;
	}
	//���ʣ������ > 1Mb
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


//���������� ��Ӧ�ĵ���send_socket()����, ����D���
int
skynet_socket_send(struct skynet_context *ctx, int id, void *buffer, int sz) {
	//���ص��Ƿ��ͻ�������Ϊʣ�����������
	
	int64_t wsz = socket_server_send(SOCKET_SERVER, id, buffer, sz);

	return check_wsz(ctx, id, buffer, wsz);
}


//����������  ��Ӧ�ĵ��� send_socket()����,ʹ�õ��ǵ����ȼ��Ļ�����
void
skynet_socket_send_lowpriority(struct skynet_context *ctx, int id, void *buffer, int sz) {
	socket_server_send_lowpriority(SOCKET_SERVER, id, buffer, sz);
}

//socket()  bind()  listen()  ����socket����   ��û�м��뵽epoll�й���
int 
skynet_socket_listen(struct skynet_context *ctx, const char *host, int port, int backlog) {
	uint32_t source = skynet_context_handle(ctx);
	//socket()  bind()  listen()  ����socket����   ��û�м��뵽epoll�й���
	return socket_server_listen(SOCKET_SERVER, source, host, port, backlog);
}

//������������  ��Ӧ�ĵ��� open_socket()���� ��ܵ�����O���� 
int  
skynet_socket_connect(struct skynet_context *ctx, const char *host, int port) {
	uint32_t source = skynet_context_handle(ctx);
	//������������  ��Ӧ�ĵ��� open_socket()���� ��ܵ�����O���� 
	return socket_server_connect(SOCKET_SERVER, source, host, port);
}

//����'B'��������
// ��Ӧ�ĵ��� bind_socket �����������뵽socket_server��socket������ ����״̬���Ϊ SOCKET_TYPE_BIND
int 
skynet_socket_bind(struct skynet_context *ctx, int fd) {
	uint32_t source = skynet_context_handle(ctx);

	//����'B'��������
	// ��Ӧ�ĵ��� bind_socket �����������뵽socket_server��socket������ ����״̬���Ϊ SOCKET_TYPE_BIND
	return socket_server_bind(SOCKET_SERVER, source, fd);
}



//����ر�  ��Ӧ�ĵ��� close_socket()
void 
skynet_socket_close(struct skynet_context *ctx, int id) {
	uint32_t source = skynet_context_handle(ctx);
	socket_server_close(SOCKET_SERVER, source, id);
}

//�����ر�  ��Ӧ�ĵ���close_socket()
void 
skynet_socket_shutdown(struct skynet_context *ctx, int id) {
	uint32_t source = skynet_context_handle(ctx);
	socket_server_shutdown(SOCKET_SERVER, source, id);
}


//����'S'��������
//��Ӧ�ĵ��� start_socket 
//��SOCKET_TYPE_PACCEPT����SOCKET_TYPE_PLISTEN����������socket,�������epoll������������״̬
void 
skynet_socket_start(struct skynet_context *ctx, int id) {
	uint32_t source = skynet_context_handle(ctx);
	socket_server_start(SOCKET_SERVER, source, id);
}



//����'T'��������
//��Ӧ�ĵ���setopt_socket  ����setsockopt()�������������� 
//����������TCP_NODELAY����
void
skynet_socket_nodelay(struct skynet_context *ctx, int id) {
	socket_server_nodelay(SOCKET_SERVER, id);
}



//����'U'��������
//��Ӧ�ĵ���add_udp_socket  ��udp���͵�  fd���뵽socket_server��socket������,���뵽epoll�й���
int 
skynet_socket_udp(struct skynet_context *ctx, const char * addr, int port) {
	uint32_t source = skynet_context_handle(ctx);
	return socket_server_udp(SOCKET_SERVER, source, addr, port);
}


//����'C'��������
//��Ӧ�ĵ���set_udp_address ����udp�ĵ�ַ��Ϣ
int 
skynet_socket_udp_connect(struct skynet_context *ctx, int id, const char * addr, int port) {
	return socket_server_udp_connect(SOCKET_SERVER, id, addr, port);
}



//����'A'��������
//��Ӧ�ĵ��� send_socket()����udp����
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
