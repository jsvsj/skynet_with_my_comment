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

#define MAX_EVENT 64		// ����epoll_wait�ĵ�������������ÿ��epoll���ص�����¼���

#define MIN_READ_BUFFER 64	// read��С����Ļ�������С

#define SOCKET_TYPE_INVALID 0 	//��Ч���׽���

#define SOCKET_TYPE_RESERVE 1	// Ԥ�����ѱ����룬����Ͷ��ʹ��

#define SOCKET_TYPE_PLISTEN 2	// �����׽��֣�δ����epoll����

#define SOCKET_TYPE_LISTEN 3	// �����׽��֣��Ѽ���epoll����
#define SOCKET_TYPE_CONNECTING 4	// ���������е��׽���
#define SOCKET_TYPE_CONNECTED 5		// �������׽��֣������򱻶�(connect,accept�ɹ������Ѽ���epoll����)

// Ӧ�ò��ѷ���ر��׽�������Ӧ�ò㷢�ͻ�������δ�����꣬δ����close
#define SOCKET_TYPE_HALFCLOSE 6	

#define SOCKET_TYPE_PACCEPT 7  // accept���ص��������׽��֣���δ����epoll����
#define SOCKET_TYPE_BIND 8     // �������͵��ļ�������������stdin,stdout��


#define MAX_SOCKET (1<<MAX_SOCKET_P)     // ���֧��64K��socket

#define PRIORITY_HIGH 0
#define PRIORITY_LOW 1

#define HASH_ID(id) (((unsigned)id) % MAX_SOCKET)

#define PROTOCOL_TCP 0
#define PROTOCOL_UDP 1
#define PROTOCOL_UDPv6 2

//udp_address��������С			16+2+1
#define UDP_ADDRESS_SIZE 19	// ipv6 128bit + port 16bit + 1 byte type


//udpbuffer �����С
#define MAX_UDP_PACKAGE 65535

// EAGAIN and EWOULDBLOCK may be not the same value.
#if (EAGAIN != EWOULDBLOCK)
#define AGAIN_WOULDBLOCK EAGAIN : case EWOULDBLOCK
#else
#define AGAIN_WOULDBLOCK EAGAIN
#endif

//���ͻ������ڵ�
// ���ͻ���������һ������
struct write_buffer {
	struct write_buffer * next;
	
	void *buffer;	// ���ͻ�����
	char *ptr;      // ָ��ǰδ���͵������ײ�
	int sz;			// ��ǰ��δ���͵��ֽ���

	//�Ƿ�ʹ�� struct socket_object_interface soi;
	bool userobject;

	//udp_address��������С			16+2+1
	//�����������Ҫ���͵ĵ�ַ����Ϣ
	uint8_t udp_address[UDP_ADDRESS_SIZE];
};


//��׼�� #define offsetof(s, m)   (size_t)&(((s *)0)->m)
//�ṹ��s ����һ����Աm,���øú�ó�m��ƫ�Ƶ�ַ

//�õ�udp_address[0]��ƫ����
#define SIZEOF_TCPBUFFER (offsetof(struct write_buffer, udp_address[0]))

//�õ����ͻ������Ĵ�С
#define SIZEOF_UDPBUFFER (sizeof(struct write_buffer))



//���ͻ�������ʽ����

struct wb_list {
	struct write_buffer * head;
	struct write_buffer * tail;
};


// Ӧ�ò��ÿ�������� �ĳ���
struct socket {

	//�����������ϵ��й��������շ��͵���opaque��Ӧ��skynet_context��Ϣ������]
	uintptr_t opaque;  // ��skynet�����ڱ�������handle,��skynet_context��Ӧ�ı��

	//�����ȼ����Ͷ���
	struct wb_list high;

	//�����ȼ����Ͷ���
	struct wb_list low;

	int64_t wb_size;	// ���ͻ�����δ���͵�����
	
	int fd;			// �ļ�������

	//ʵ�ʾ���socket�����socket_server�е������еı��
	int id;			// Ӧ�ò�ά����һ����fd���Ӧ��id
	
	uint16_t protocol;	//socketЭ�� TCP/UDP
	
	uint16_t type;		//socket״̬(����д������......)

	union {
				
		int size;  //������Ԥ���ƵĴ�С		
	
		uint8_t udp_address[UDP_ADDRESS_SIZE];
	} p;
};


//������ socket���ֵĳ���,���socket������
struct socket_server {
	
	int recvctrl_fd;	// �ܵ����ˣ����ڽ��ܿ�������
	int sendctrl_fd;	// �ܵ�д�ˣ����ڷ��Ϳ�������

	int checkctrl;		// �Ƿ����������

	poll_fd event_fd;	// epoll fd
	
	int alloc_id;		// ���ڷ���id
	int event_n;		// epoll_wait���ص��¼�����
	int event_index;	// ��ǰ������¼���ţ���0��ʼ

	//�ṹ������һЩ����ָ��
	/*
		struct socket_object_interface {
			void * (*buffer)(void *);
			int (*size)(void *);
			void (*free)(void *);
		};

	*/
	struct socket_object_interface soi;

	//epoll_wait�õ��Ļ�Ծ��������
	struct event ev[MAX_EVENT];			 // epoll_wait���ص��¼���
	
	struct socket slot[MAX_SOCKET];		// Ӧ�ò�Ԥ�ȷ����socket����(��socket��)
	char buffer[MAX_INFO];				// ��ʱ���ݣ����籣���½����ӵĶԵȶ˵ĵ�ַ��Ϣ
	
	uint8_t udpbuffer[MAX_UDP_PACKAGE];
	
	fd_set rfds;		// ����select��fd����
};



// ���¼����ṹ���� �����������ݰ� ����ṹ

struct request_open {
	int id;
	int port;
	uintptr_t opaque;
	char host[1];
};

struct request_send {
	int id;

		//sz��buffer�Ĵ�С
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


// �������������


struct request_package {

	//6���ֽ�δ�� [0-5] [6]��type [7]���� ����ָ���ǰ���ĳ���
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

// ����IP��ַ
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

// �����ϻ��� #define skynet_malloc malloc
#define MALLOC skynet_malloc
#define FREE skynet_free


//��ʼ�� struct send_object
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

//�ͷ�һ�����ͻ���ڵ�
static inline void
write_buffer_free(struct socket_server *ss, struct write_buffer *wb) {

	//�ж��Ƿ�ʹ��userobject
	if (wb->userobject) {
		ss->soi.free(wb->buffer);
	} else {
		FREE(wb->buffer);
	}
	FREE(wb);
}


//����ʵ����������
static void
socket_keepalive(int fd) {
	int keepalive = 1;
	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (void *)&keepalive , sizeof(keepalive));  
}


// ��socket���л�ȡһ���յ�socket����Ϊ�����һ��id(0~2147483647��2^31-1)
// ��socket���е�λ�� �صĴ�С��64K socket_id�ķ�ΧԶ����64K

static int
reserve_id(struct socket_server *ss) {
	int i;
	for (i=0;i<MAX_SOCKET;i++) {
		int id = ATOM_INC(&(ss->alloc_id));  // ԭ�ӵ�++

		// С��0   �Ѿ������ ˵�� ��0��ʼ
		if (id < 0) {

			// ��ʱid = 0x80000000��-2147483648
			id = ATOM_AND(&(ss->alloc_id), 0x7fffffff);
		}

		// ��socket����ȡ��socket
		struct socket *s = &ss->slot[HASH_ID(id)];

		//�����λ��û�б�ʹ��		
		if (s->type == SOCKET_TYPE_INVALID) {

			// �����Ⱦͽ����� SOCKET_TYPE_RESERVE ����Ϊ����
			// ��������û�м��� ���ܶ���̲߳��� ���Ի���Ҫ�ж�һ��

			//��״̬��Ϊ SOCKET_TYPE_RESERVE Ԥ��
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


//������ͻ������
static inline void
clear_wb_list(struct wb_list *list) {
	list->head = NULL;
	list->tail = NULL;
}



//����socker_server
struct socket_server * 
socket_server_create() {
	int i;
	int fd[2];

	//sp_create() ����  epoll_create(1024);
	poll_fd efd = sp_create();

	//�����ж�
	//return efd == -1;
	if (sp_invalid(efd)) {
		fprintf(stderr, "socket-server: create event pool failed.\n");
		return NULL;
	}

	//���������ܵ�
	if (pipe(fd)) {

		//ʵ�ʵ��� close(efd);
		sp_release(efd);
		fprintf(stderr, "socket-server: create socket pair failed.\n");
		return NULL;
	}

	// epoll ��ע�ܵ��Ŀɶ��¼�
	if (sp_add(efd, fd[0], NULL)) {
		// add recvctrl_fd to event poll
		fprintf(stderr, "socket-server: can't add server fd to event pool.\n");
		close(fd[0]);
		close(fd[1]);
		sp_release(efd);
		return NULL;
	}

	//MALLOC  ����malloc
	//socket_server���ǶԷ��������粿�ֵĳ���
	struct socket_server *ss = MALLOC(sizeof(*ss));
	
	ss->event_fd = efd;
	
	ss->recvctrl_fd = fd[0];	//�ܵ������
	ss->sendctrl_fd = fd[1];	//�ܵ������
	ss->checkctrl = 1;


	//��ʼ������ 64k��socket����
	for (i=0;i<MAX_SOCKET;i++) {
		struct socket *s = &ss->slot[i];
		
		//���Ϊδʹ��
		s->type = SOCKET_TYPE_INVALID;
		clear_wb_list(&s->high);
		clear_wb_list(&s->low);
	}
	ss->alloc_id = 0;
	ss->event_n = 0;
	ss->event_index = 0;
	memset(&ss->soi, 0, sizeof(ss->soi));

	//��select ���輯������Ϊ0
	FD_ZERO(&ss->rfds);
	
	assert(ss->recvctrl_fd < FD_SETSIZE);

	return ss;
}

//�ͷŷ��ͻ������list
static void
free_wb_list(struct socket_server *ss, struct wb_list *list) {

	struct write_buffer *wb = list->head;
	while (wb) {
		struct write_buffer *tmp = wb;
		wb = wb->next;
		//�ͷ��ڴ�
		write_buffer_free(ss, tmp);
	}
	list->head = NULL;
	list->tail = NULL;
}


//ǿ�ƹر��׽���,��������socket_server�ж�Ӧ�� struct socket *s  result�Ǵ��봫������
static void
force_close(struct socket_server *ss, struct socket *s, struct socket_message *result) {

	result->id = s->id;
	result->ud = 0;
	result->data = NULL;
	result->opaque = s->opaque;

	//���s��δʹ�õ�
	if (s->type == SOCKET_TYPE_INVALID) {
		return;
	}

	//����s����Ԥ����
	assert(s->type != SOCKET_TYPE_RESERVE);
	
	//���ٷ��ͻ�����
	free_wb_list(ss,&s->high);
	free_wb_list(ss,&s->low);


	//SOCKET_TYPE_PACCEPT   SOCKET_TYPE_PLISTEN����δ���뵽epoll�й���
	if (s->type != SOCKET_TYPE_PACCEPT && s->type != SOCKET_TYPE_PLISTEN) {

		// epollȡ����ע���׽���
		sp_del(ss->event_fd, s->fd);
	}

	//����׽������Ͳ���stdin,stdout �͹ر�
	if (s->type != SOCKET_TYPE_BIND) {
		
		if (close(s->fd) < 0) {
			perror("close socket:");
		}
	}

	//��״̬��Ϊδʹ��
	s->type = SOCKET_TYPE_INVALID;
}


//����socket_server
void 
socket_server_release(struct socket_server *ss) {
	int i;
	
	struct socket_message dummy;

	for (i=0;i<MAX_SOCKET;i++) {
		struct socket *s = &ss->slot[i];

		//�������Ԥ����socket�ṹ��
		if (s->type != SOCKET_TYPE_RESERVE) {

			//dummy�Ǵ�������
			force_close(ss, s , &dummy);
		}
	}

	//�رչܵ�
	close(ss->sendctrl_fd);
	close(ss->recvctrl_fd);

	//���� close() �ر� epoll������
	sp_release(ss->event_fd);

	//free
	FREE(ss);
}


//���Է��ͻ���������Ϊ��
static inline void
check_wb_list(struct wb_list *s) {
	assert(s->head == NULL);
	assert(s->tail == NULL);
}


//Ϊ��Ԥ����socket��ʼ�������addΪtrue���Ὣ���׽��ּ���epoll����
// struct socket�Ƕ�ÿ���׽��ֵĳ���
//id���� ��socket����ָ���ڷ���������struct socket_server�е�������hash֮ǰ��λ��
static struct socket *
new_fd(struct socket_server *ss, int id, int fd, int protocol, uintptr_t opaque, bool add) {

	//����HASH_ID(id)ȡ��һ��λ��
	struct socket * s = &ss->slot[HASH_ID(id)];

	//������Ԥ����
	assert(s->type == SOCKET_TYPE_RESERVE);

	//���addΪtrue,������������epoll����
	if (add) {
		if (sp_add(ss->event_fd, fd, s)) {
			s->type = SOCKET_TYPE_INVALID;
			return NULL;
		}
	}

	s->id = id;
	s->fd = fd;

	//socketЭ�� TCP/UDP
	s->protocol = protocol;

	//���ƵĶ��Ļ�������С
	s->p.size = MIN_READ_BUFFER;
	
	s->opaque = opaque;

	//���ͻ�����δ�������ݵĴ�С
	s->wb_size = 0;

	//����Ϊ��
	check_wb_list(&s->high);
	check_wb_list(&s->low);

	return s;
}

// return -1 when connecting
//����connect()
//���ӳɹ�����SOCKET_OPEN,δ���ӳɹ�(����������)����-1,������SOCKET_ERROR
//result�Ǵ�������
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


	//�õ���sockaddr����ָ��
	struct addrinfo *ai_list = NULL;
	
	struct addrinfo *ai_ptr = NULL;
	
	char port[16];
	sprintf(port, "%d", request->port);
	memset(&ai_hints, 0, sizeof( ai_hints ) );

	//
	ai_hints.ai_family = AF_UNSPEC;
	ai_hints.ai_socktype = SOCK_STREAM;
	ai_hints.ai_protocol = IPPROTO_TCP;

	//��ȡ��ַ�б�,(����gethostbyname,gethostbuaddr,������������֧��IPV4)
	//��һ��������IP��ַ���������ƣ��ڶ��������Ƿ�������(�����Ƕ˿ںŻ�������ƣ���http,ftp)
	status = getaddrinfo( request->host, port, &ai_hints, &ai_list );
	if ( status != 0 ) {

		//getaddrinfo�����ط���ֵ,gai_strerror���ݷ��ص�ֵ��ȡִ�д�����Ϣ���ַ�����ָ��
		result->data = (void *)gai_strerror(status);
		goto _failed;
	}
	
	int sock= -1;
	for (ai_ptr = ai_list; ai_ptr != NULL; ai_ptr = ai_ptr->ai_next ) {

		//����socket����
		sock = socket( ai_ptr->ai_family, ai_ptr->ai_socktype, ai_ptr->ai_protocol );
		if ( sock < 0 ) {
			continue;
		}

		//����socket����,������������
		socket_keepalive(sock);
		sp_nonblocking(sock);

		//����connect����
		status = connect( sock, ai_ptr->ai_addr, ai_ptr->ai_addrlen);
		if ( status != 0 && errno != EINPROGRESS) {
			close(sock);
			sock = -1;

			//���ӳ�����������ѭ�������ӵ���һ����ַ
			continue;
		}
		break;
	}

	if (sock < 0) {
		result->data = strerror(errno);
		goto _failed;
	}

	//����epoll����,������socket_server��socket������
	ns = new_fd(ss, id, sock, PROTOCOL_TCP, request->opaque, true);
	if (ns == NULL) {
		close(sock);
		result->data = "reach skynet socket number limit";
		goto _failed;
	}

	
	if(status == 0) {
		//˵��connect�ɹ�
		ns->type = SOCKET_TYPE_CONNECTED;

		struct sockaddr * addr = ai_ptr->ai_addr;
		void * sin_addr = (ai_ptr->ai_family == AF_INET) ? (void*)&((struct sockaddr_in *)addr)->sin_addr : (void*)&((struct sockaddr_in6 *)addr)->sin6_addr;

		if (inet_ntop(ai_ptr->ai_family, sin_addr, ss->buffer, sizeof(ss->buffer))) {
			//�õ�ip
			result->data = ss->buffer;
		}

		//��׼����ͷź���
		freeaddrinfo( ai_list );
		return SOCKET_OPEN;
	} else {

		//˵���������׽��ֳ���������
		ns->type = SOCKET_TYPE_CONNECTING;
		//�������׽��ֳ��������У������ע���д�¼����Ժ�epoll���ܲ������ӳ����˻��ǳɹ���
		sp_write(ss->event_fd, ns->fd, ns, true);
	}

	freeaddrinfo( ai_list );
	return -1;
_failed:
	freeaddrinfo( ai_list );
	ss->slot[HASH_ID(id)].type = SOCKET_TYPE_INVALID;
	return SOCKET_ERROR;
}

//�����ͻ������е����ݷ��� tcp����
//result�Ǵ�������
static int
send_list_tcp(struct socket_server *ss, struct socket *s, struct wb_list *list, struct socket_message *result) {
	while (list->head) {
		struct write_buffer * tmp = list->head;

		//���ϵĵ���write()
		for (;;) {

			////tmp->sz��ʾ�ý����δ���͵����ݴ�С
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

			//wb_size��ʾ���ͻ�������δ���͵�����
			s->wb_size -= sz;

			//tmp->sz��ʾ�ý����δ���͵�����
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

// �õ�����IP��ַunion sockaddr_all
/*
union sockaddr_all {
	struct sockaddr s;
	struct sockaddr_in v4;
	struct sockaddr_in6 v6;
};


*/

//ͨ��udp_address��ȡ ip �˿���Ϣ��ͨ��sa����
static socklen_t
udp_socket_address(struct socket *s, const uint8_t udp_address[UDP_ADDRESS_SIZE], union sockaddr_all *sa) {

	//��ȡЭ������
	int type = (uint8_t)udp_address[0];
	if (type != s->protocol)
		return 0;
	
	uint16_t port = 0;

	//��ȡ�˿ں�
	memcpy(&port, udp_address+1, sizeof(uint16_t));

	//�ж�Э������  tcp/udp
	//��ȡ��ַ
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



//�ӷ��Ͷ�����  sendto ����udp���͵�����
static int
send_list_udp(struct socket_server *ss, struct socket *s, struct wb_list *list, struct socket_message *result) {
	while (list->head) {
		
		struct write_buffer * tmp = list->head;

		union sockaddr_all sa;
		//��õ�ַ��Ϣ�������sa��
		socklen_t sasz = udp_socket_address(s, tmp->udp_address, &sa);
		
		// ����
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


//����Э������tcp����udp ���ͻ����������е����ݣ�
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


//�жϷ������ݵĻ������Ƿ�δ�����꣬���δ�����꣬������
static inline int
list_uncomplete(struct wb_list *s) {
	struct write_buffer *wb = s->head;
	if (wb == NULL)
		return 0;
	
	return (void *)wb->ptr != wb->buffer;
}


//�������ȼ����������е� ��һ���ڵ�(ͷ���) �ƶ��������ȼ��Ķ�����,��ʱ�����ȼ�������û�нڵ�
static void
raise_uncomplete(struct socket * s) {

	//�����ȼ����ͻ���������
	struct wb_list *low = &s->low;
	
	struct write_buffer *tmp = low->head;

	//�ƶ������ȼ������ͷ��
	low->head = tmp->next;
	if (low->head == NULL) {
		low->tail = NULL;
	}

	// move head of low list (tmp) to the empty high list
	//�������ȼ����������еĵ�һ���ڵ��ƶ��������ȼ��Ķ�����
	struct wb_list *high = &s->high;

	//���Ը����ȼ�������û�����ݽڵ�
	assert(high->head == NULL);

	tmp->next = NULL;

	//�������ȼ����������еĵ�һ���ڵ��ƶ��������ȼ��Ķ�����	
	high->head = high->tail = tmp;
}

/*
	Each socket has two write buffer list, high priority and low priority.

	1. send high list as far as possible.
	2. If high list is empty, try to send low list.
	3. If low list head is uncomplete (send a part before), move the head of low list to empty high list (call raise_uncomplete) .
	4. If two lists are both empty, turn off the event. (call check_close)
 */

//��д�¼���������Ӧ�ò㻺�����з�������,result�Ǵ��봫������
static int
send_buffer(struct socket_server *ss, struct socket *s, struct socket_message *result) {


	//���Ե����ȼ��Ļ�������
	assert(!list_uncomplete(&s->low));

	
	// step 1
	//���͸����ȼ������е�����   SOCKET_CLOSE��ʾ�׽��ֹر��ˣ��������ݲ��ɹ�
	if (send_list(ss,s,&s->high,result) == SOCKET_CLOSE) {
		return SOCKET_CLOSE;
	}
	if (s->high.head == NULL) {
		// step 2
		//���͵����ȼ������е�����
		if (s->low.head != NULL) {
			if (send_list(ss,s,&s->low,result) == SOCKET_CLOSE) {
				return SOCKET_CLOSE;
			}

			//��������ȼ�����û�з�����,
			// step 3
			if (list_uncomplete(&s->low)) {
				//�������ȼ����������е� ��һ���ڵ�(ͷ���) �ƶ��������ȼ��Ķ�����
				raise_uncomplete(s);
			}
		} else {
			//���ݷ������
			// step 4
			//���ٹ�ע��д�¼�����ע�ɶ��¼�
			sp_write(ss->event_fd, s->fd, s, false);

			//���֮ǰ�����Ҫ�ر��׽��֣��������׽�������û�з����ֻ꣬�Ǳ��Ҫ�رգ���
			//  ��ʱ���ݷ�������  ֱ��ǿ�ƹر�,�����׽���
			if (s->type == SOCKET_TYPE_HALFCLOSE) {
				force_close(ss, s, result);
				return SOCKET_CLOSE;
			}
		}
	}

	return -1;
}


//��δ�����������׷�ӵ�write_buffe �У�n��ʾ�ӵ�n���ֽڿ�ʼ,����ԭ���Ѿ�������n���ֽ�
static struct write_buffer *
append_sendbuffer_(struct socket_server *ss, struct wb_list *s, struct request_send * request, int size, int n) {

	//����һ���ڵ�
	struct write_buffer * buf = MALLOC(size);

	struct send_object so;

	//bool   ��ʼ��struct send_object so;
	buf->userobject = send_object_init(ss, &so, request->buffer, request->sz);
	
	buf->ptr = (char*)so.buffer+n;
	buf->sz = so.sz - n;
	buf->buffer = request->buffer;
	buf->next = NULL;

	//���ӽڵ���뵽������������
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

//��udp����������ӵ�������
static inline void
append_sendbuffer_udp(struct socket_server *ss, struct socket *s, int priority, struct request_send * request, const uint8_t udp_address[UDP_ADDRESS_SIZE]) {
	//�ж����ȼ�
	struct wb_list *wl = (priority == PRIORITY_HIGH) ? &s->high : &s->low;

	
	struct write_buffer *buf = append_sendbuffer_(ss, wl, request, SIZEOF_UDPBUFFER, 0);
	//������Ҫ���͵ĵ�ַ��Ϣд��
	memcpy(buf->udp_address, udp_address, UDP_ADDRESS_SIZE);

	s->wb_size += buf->sz;
}


//��������ӵ������ȼ�������
static inline void
append_sendbuffer(struct socket_server *ss, struct socket *s, struct request_send * request, int n) {
	struct write_buffer *buf = append_sendbuffer_(ss, &s->high, request, SIZEOF_TCPBUFFER, n);
	s->wb_size += buf->sz;
}



//��������ӵ������ȼ��Ķ�����
static inline void
append_sendbuffer_low(struct socket_server *ss,struct socket *s, struct request_send * request) {
	struct write_buffer *buf = append_sendbuffer_(ss, &s->low, request, SIZEOF_TCPBUFFER, 0);
	s->wb_size += buf->sz;
}


//�ж�Ӧ�ò�������������Ƿ�δ��
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

//�������� result�Ǵ�������
static int
send_socket(struct socket_server *ss, struct request_send * request, struct socket_message *result, int priority, const uint8_t *udp_address) {
	int id = request->id;
	
	//�õ�Ҫ�������ݵ�socket
	struct socket * s = &ss->slot[HASH_ID(id)];

	struct send_object so;
	send_object_init(ss, &so, request->buffer, request->sz);

	//����׽�������Ч��  �رյ� 
	if (s->type == SOCKET_TYPE_INVALID || s->id != id 
		|| s->type == SOCKET_TYPE_HALFCLOSE
		|| s->type == SOCKET_TYPE_PACCEPT) {
		so.free_func(request->buffer);
		return -1;
	}

	//����׽���û����ӵ�epoll�й���
	if (s->type == SOCKET_TYPE_PLISTEN || s->type == SOCKET_TYPE_LISTEN) {
		fprintf(stderr, "socket-server: write to listen fd %d.\n", id);
		so.free_func(request->buffer);
		return -1;
	}

	//��� Ӧ�ò㻺���� û��������Ϊ�����������     ֱ�ӷ���
	if (send_buffer_empty(s) && s->type == SOCKET_TYPE_CONNECTED) {
		if (s->protocol == PROTOCOL_TCP) {

			//tcp����
			//ֱ�ӷ���
			int n = write(s->fd, so.buffer, so.sz);
			if (n<0) {
				switch(errno) {
				case EINTR:
				case AGAIN_WOULDBLOCK:	// �ں˻���������
					n = 0;
					break;
				default:
					//�Է��رյ��׽���
					fprintf(stderr, "socket-server: write to %d (fd=%d) error :%s.\n",id,s->fd,strerror(errno));
					force_close(ss,s,result);
					so.free_func(request->buffer);
					return SOCKET_CLOSE;
				}
			}

			// ���԰����ݿ������ں˻�������
			if (n == so.sz) {
				so.free_func(request->buffer);
				return -1;
			}

			//���е����˵��δ��Ҫ���͵�����ȫ���������ں˻�����
			// ��δ���͵Ĳ�����ӵ�Ӧ�ò㻺������ 
			append_sendbuffer(ss, s, request, n);	// add to high priority list, even priority == PRIORITY_LOW


		} else {

			// udp
			//udp����
			if (udp_address == NULL) {
				udp_address = s->p.udp_address;
			}
			union sockaddr_all sa;
			//��õ�ַ
			socklen_t sasz = udp_socket_address(s, udp_address, &sa);

			//ֱ�ӷ���
			int n = sendto(s->fd, so.buffer, so.sz, 0, &sa.s, sasz);

			if (n != so.sz) {
				//����û�з�����
				//��������ӵ�������
				append_sendbuffer_udp(ss,s,priority,request,udp_address);
			} else {
			
				so.free_func(request->buffer);
				return -1;
			}
		}

		//���е����˵������û�з����꣬Ҫ��ע��д�¼�
		sp_write(ss->event_fd, s->fd, s, true);

		//���Ӧ�ò㻺����ԭ����������������,ֱ�ӽ�Ҫ���͵�������ӵ���������	
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



//�����������뵽socket_server��socket�����У���û�м��뵽epoll�У�ֻ�ǽ�״̬���ΪSOCKET_TYPE_PLISTEN
static int
listen_socket(struct socket_server *ss, struct request_listen * request, struct socket_message *result) {

	//id���Ѿ����뵽��socket������Ԥ����λ��
	int id = request->id;
	int listen_fd = request->fd;

	//��ʼ��һ��socket,  false��ʾ������뵽epoll�й���
	//���ǽ�socket���������뵽socket_server�е�����
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


//�ر��׽��� result�Ǵ�������
static int
close_socket(struct socket_server *ss, struct request_close *request, struct socket_message *result) {
	int id = request->id;
	struct socket * s = &ss->slot[HASH_ID(id)];

	//˵���Ѿ��رջ�����Ч��socket
	if (s->type == SOCKET_TYPE_INVALID || s->id != id) {
		result->id = id;
		result->opaque = request->opaque;
		result->ud = 0;
		result->data = NULL;
		return SOCKET_CLOSE;
	}

	//���Ӧ�ò㻺������Ϊ��
	if (!send_buffer_empty(s)) {

		//����Ӧ�ò㻺��������
		int type = send_buffer(ss,s,result);
		if (type != -1)
			return type;
	}

	//����Ѿ�������
	if (request->shutdown || send_buffer_empty(s)) {
		force_close(ss,s,result);
		result->id = id;
		result->opaque = request->opaque;
		return SOCKET_CLOSE;
	}
	//�������û�з�����,���Ҫ�ر�
	s->type = SOCKET_TYPE_HALFCLOSE;

	return -1;
}


//��stdin,stdout�������͵��ļ����������뵽epoll�����������ͳ�ΪSOCKET_TYPE_BIND
//result�Ǵ�������
static int
bind_socket(struct socket_server *ss, struct request_bind *request, struct socket_message *result) {

	int id = request->id;
	result->id = id;
	result->opaque = request->opaque;
	result->ud = 0;
	
	//��fd���뵽socket_server�е�socket����    true��ʾ���뵽epoll�й���
	struct socket *s = new_fd(ss, id, request->fd, PROTOCOL_TCP, request->opaque, true);
	if (s == NULL) {
		result->data = "reach skynet socket number limit";
		return SOCKET_ERROR;
	}
	//���÷�����
	sp_nonblocking(request->fd);
	//���״̬
	s->type = SOCKET_TYPE_BIND;
	
	result->data = "binding";
	return SOCKET_OPEN;
}


//SOCKET_TYPE_PACCEPT�Ѿ����ӣ���û�м��뵽epoll��
//SOCKET_TYPE_PLISTEN,�����׽��֣�����û�м��뵽epoll��

//��SOCKET_TYPE_PACCEPT����SOCKET_TYPE_PLISTEN����������socket,�������epoll������������״̬
//result�Ǵ�������
static int
start_socket(struct socket_server *ss, struct request_start *request, struct socket_message *result) {
	int id = request->id;
	result->id = id;
	result->opaque = request->opaque;
	result->ud = 0;
	result->data = NULL;
	
	struct socket *s = &ss->slot[HASH_ID(id)];

	//��Ч��socket
	if (s->type == SOCKET_TYPE_INVALID || s->id !=id) {
		result->data = "invalid socket";
		return SOCKET_ERROR;
	}

	//��SOCKET_TYPE_PACCEPT����SOCKET_TYPE_PLISTEN����������
	if (s->type == SOCKET_TYPE_PACCEPT || s->type == SOCKET_TYPE_PLISTEN) {

		//���뵽epoll����
		if (sp_add(ss->event_fd, s->fd, s)) {
			force_close(ss, s, result);
			result->data = strerror(errno);
			return SOCKET_ERROR;
		}
		//�޸�״̬   SOCKET_TYPE_CONNECTED�Ѿ����ӵģ������Ѿ����뵽��epoll��
		s->type = (s->type == SOCKET_TYPE_PACCEPT) ? SOCKET_TYPE_CONNECTED : SOCKET_TYPE_LISTEN;
		s->opaque = request->opaque;
		result->data = "start";
		return SOCKET_OPEN;

		//������Ѿ����ӵģ����Ҽ��뵽epoll�е�
	} else if (s->type == SOCKET_TYPE_CONNECTED) {
		// todo: maybe we should send a message SOCKET_TRANSFER to s->opaque
		s->opaque = request->opaque;
		result->data = "transfer";
		return SOCKET_OPEN;
	}
	// if s->type == SOCKET_TYPE_HALFCLOSE , SOCKET_CLOSE message will send later
	return -1;
}

//����setsockopt��������socket����
static void
setopt_socket(struct socket_server *ss, struct request_setopt *request) {
	int id = request->id;

	//�ҵ�socket
	struct socket *s = &ss->slot[HASH_ID(id)];
	if (s->type == SOCKET_TYPE_INVALID || s->id !=id) {
		return;
	}
	int v = request->value;

	//����setsockopt()����
	setsockopt(s->fd, IPPROTO_TCP, request->what, &v, sizeof(v));
}


//�ӹܵ���ȡ����,����buffer��
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


// �жϹܵ��Ƿ������� ʹ��select������ û��ʹ��epollʱΪ���������ļ��Ƶ��
static int
has_cmd(struct socket_server *ss) {
	
	struct timeval tv = {0,0};
	int retval;

	//recvctrl_fd�Ǵ����Ĺܵ��Ķ���
	//���ܵ����˵������������缯���У�����ֻ����һ��������
	FD_SET(ss->recvctrl_fd, &ss->rfds);

	//ʹ��select  ��������
	retval = select(ss->recvctrl_fd+1, &ss->rfds, NULL, NULL, &tv);

	//��Ϊselectֻ�����ܵ�����������
	if (retval == 1) {
		return 1;
	}
	return 0;
}


//���udp��socket
//��fd���뵽socket_server��socket������,���뵽epoll�й���
static void
add_udp_socket(struct socket_server *ss, struct request_udp *udp) {
	int id = udp->id;
	int protocol;
	if (udp->family == AF_INET6) {
		protocol = PROTOCOL_UDPv6;
	} else {
		protocol = PROTOCOL_UDP;
	}
	//��fd���뵽socket_server��socket�����У�true��ʾ������뵽epoll�й���
	struct socket *ns = new_fd(ss, id, udp->fd, protocol, udp->opaque, true);
	if (ns == NULL) {
		close(udp->fd);
		ss->slot[HASH_ID(id)].type = SOCKET_TYPE_INVALID;
		return;
	}
	ns->type = SOCKET_TYPE_CONNECTED;
	memset(ns->p.udp_address, 0, sizeof(ns->p.udp_address));
}




//����udp�ĵ�ַ��Ϣ  result�Ǵ�������
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
	//����
	if (type == PROTOCOL_UDP) {
		memcpy(s->p.udp_address, request->address, 1+2+4);	// 1 type, 2 port, 4 ipv4
	} else {
		memcpy(s->p.udp_address, request->address, 1+2+16);	// 1 type, 2 port, 16 ipv6
	}
	return -1;
}

// return type

// ������ ������������� ������Ӧ�����͵�����Ӧ�ĺ���
//result�Ǵ��봫������
static int
ctrl_cmd(struct socket_server *ss, struct socket_message *result) {

	//�ܵ����˵�������
	int fd = ss->recvctrl_fd;
	
	// the length of message is one byte, so 256+8 buffer size is enough.
	uint8_t buffer[256];
	uint8_t header[2];

	//�ӹܵ��ж�ȡ����		��ͷ  ���뵽header��������
	block_readpipe(fd, header, sizeof(header));

	int type = header[0];
	int len = header[1];

	//�ӹܵ��ж�ȡ����		����   ����buffer��
	block_readpipe(fd, buffer, len);

	// ctrl command only exist in local fd, so don't worry about endian.

	//�������������Ӧ�ĺ���
	switch (type) {
		
	case 'S':
		//��SOCKET_TYPE_PACCEPT����SOCKET_TYPE_PLISTEN����������socket,�������epoll������������״̬
		return start_socket(ss,(struct request_start *)buffer, result);
	case 'B':
		// �����������뵽socket_server��socket������ ����״̬���Ϊ SOCKET_TYPE_BIND
		return bind_socket(ss,(struct request_bind *)buffer, result);
	case 'L':

		//�����������뵽socket_server��socket�����У���û�м��뵽epoll�У�ֻ�ǽ�״̬���ΪSOCKET_TYPE_PLISTEN
		return listen_socket(ss,(struct request_listen *)buffer, result);
	case 'K':
		//�ر��׽���
		return close_socket(ss,(struct request_close *)buffer, result);
	case 'O':

		//�ȵ���socket(),�����׽���
		// ����connect()
		//���ӳɹ�����SOCKET_OPEN,δ���ӳɹ�(����������)����-1,������SOCKET_ERROR
		return open_socket(ss, (struct request_open *)buffer, result);

		//��ʾҪ�Ƴ�
	case 'X':
		result->opaque = 0;
		result->id = 0;
		result->ud = 0;
		result->data = NULL;
		return SOCKET_EXIT;
	case 'D':
		//��������,ʹ�õ��Ǹ����ȼ��Ļ�����
		return send_socket(ss, (struct request_send *)buffer, result, PRIORITY_HIGH, NULL);
		
	case 'P':
		//��������,ʹ�õ��ǵ����ȼ��Ļ�����
		return send_socket(ss, (struct request_send *)buffer, result, PRIORITY_LOW, NULL);

	case 'A': {
		//����udp���� ,ʹ�õ��Ǹ����ȼ��Ļ�����
		struct request_send_udp * rsu = (struct request_send_udp *)buffer;
		return send_socket(ss, &rsu->send, result, PRIORITY_HIGH, rsu->address);
	}

	case 'C':
		//����udp�ĵ�ַ��Ϣ
		return set_udp_address(ss, (struct request_setudp *)buffer, result);

	case 'T':
		//����setsockopt()��������������
		setopt_socket(ss, (struct request_setopt *)buffer);
		return -1;
		
	case 'U':
		//��udp���͵�  fd���뵽socket_server��socket������,���뵽epoll�й���
		add_udp_socket(ss, (struct request_udp *)buffer);
		return -1;
	default:
		fprintf(stderr, "socket-server: Unknown ctrl %c.\n",type);
		return -1;
	};

	return -1;
}

// return -1 (ignore) when error
//tcp���������� �ɶ��¼�������ִ�иú��� ��Ҫ�ǵ���read(),ͨ��result�������������ݣ��ڶ��� 
static int
forward_message_tcp(struct socket_server *ss, struct socket *s, struct socket_message * result) {

	int sz = s->p.size;
	char * buffer = MALLOC(sz);

	//����read()��ȡ���� 
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
			//������ǿ�ƹر�
			force_close(ss, s, result);
			result->data = strerror(errno);
			return SOCKET_ERROR;
		}
		return -1;
	}

	//�Է��ر�
	if (n==0) {
		FREE(buffer);
		//�ر��׽���
		force_close(ss, s, result);
		return SOCKET_CLOSE;
	}

	// half close ��رն�����Ϣ,��֮ǰ�����Ҫ�رյ��׽���
	if (s->type == SOCKET_TYPE_HALFCLOSE) {
		// discard recv data
		FREE(buffer);
		return -1;
	}

	//������ȡ���ݻ������Ĵ�С
	if (n == sz) {
		s->p.size *= 2;
	} else if (sz > MIN_READ_BUFFER && n*2 < sz) {
		s->p.size /= 2;
	}

	//handle
	result->opaque = s->opaque;
	
	result->id = s->id;
	
	//ud����������ݵĴ�С
	result->ud = n;
	//buffer�ڶ��з���
	result->data = buffer;
	return SOCKET_DATA;
}

//�� union sockaddr_all *sa�� ��ȡudp��ַ  ͨ��udp_address����
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

//udp���������ɶ�ʱ���øú��� ����recvfrom()���� ,result�Ǵ������������Խ����������ݴ���
static int
forward_message_udp(struct socket_server *ss, struct socket *s, struct socket_message * result) {
	union sockaddr_all sa;
	socklen_t slen = sizeof(sa);

	//��������,���뵽udpbuffer�� 
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
		//����ַ��Ϣ���뵽dataĩβ��
		gen_udp_address(PROTOCOL_UDP, &sa, data + n);
	} else {
		if (s->protocol != PROTOCOL_UDPv6)
			return -1;
		data = MALLOC(n + 1 + 2 + 16);
		gen_udp_address(PROTOCOL_UDPv6, &sa, data + n);
	}

	//�����յ������ݸ��Ƶ�data��
	memcpy(data, ss->udpbuffer, n);

	result->opaque = s->opaque;
	result->id = s->id;
	result->ud = n;
	result->data = (char *)data;

	return SOCKET_UDP;
}


// ���������е��׽��ֿ�д�¼�����, ���ܳɹ���Ҳ���ܳ���
static int
report_connect(struct socket_server *ss, struct socket *s, struct socket_message *result) {
	int error;
	
	socklen_t len = sizeof(error);  
	int code = getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &error, &len);  

	//���������Ϣ
	if (code < 0 || error) {  
		force_close(ss,s, result);
		if (code >= 0)
			result->data = strerror(error);
		else
			result->data = strerror(errno);
		return SOCKET_ERROR;
	} else {

	//��������ӳɹ�
		s->type = SOCKET_TYPE_CONNECTED;
		result->opaque = s->opaque;
		result->id = s->id;
		result->ud = 0;
		//�������� ������ Ϊ��
		if (send_buffer_empty(s)) {
			//��ע�ɶ��¼�
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
//�����׽��ֿɶ��¼����������øú���,������accept

//�����s�ṹ����󱣴��˼����׽���
//����accept���� ,���µ����������뵽��socket_server��socket�����У�����û�м��뵽epoll��
//result�Ǵ�������
static int
report_accept(struct socket_server *ss, struct socket *s, struct socket_message *result) {

	union sockaddr_all u;
	socklen_t len = sizeof(u);

	//������accept
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

	//��socket��������һ��socket,ʵ�ʾ��ǵõ�һ���洢 socketָ�������λ��
	
	//struct socket_server *ss�Ƿ��������粿�ֵ�һ������,struct socket��ÿ���������ĳ���
	//ss���б���ÿ����������Ӧ�Ľṹ��ָ����������λֵhashǰ��ֵ

	//��socket_server�����еõ�һ�����õ�λ��
	int id = reserve_id(ss);

	//˵��Ӧ�ò�socket�Ѿ�����
	if (id < 0) {
		close(client_fd);
		return 0;
	}

	//��������
	socket_keepalive(client_fd);
	
	//����Ϊ������
	sp_nonblocking(client_fd);

	//��ʼ�����뵽��socket,Ϊ����epoll����,
	//struct socket�Ƕ�ÿһ���׽��ֵĳ���,һ���������׽��ֵĽṹ��
	//�õ�һ��strutc socket���󣬴洢���˷���������struct socket_server�������У�false��ʾ����������δ��ӵ�epoll����

	//��client_fd���뵽socket_server��socket�������,false��ʾ������뵽epoll�� 
	struct socket *ns = new_fd(ss, id, client_fd, PROTOCOL_TCP, s->opaque, false);
	if (ns == NULL) {
		close(client_fd);
		return 0;
	}

	//���״̬���Ѿ����ӣ�����δ���뵽epoll�й���
	ns->type = SOCKET_TYPE_PACCEPT;

	//skynet_context��Ӧ�ı��handle
	result->opaque = s->opaque;

	//socket�ṹ���Ӧ����skynet_server�е�socket�����еı��
	result->id = s->id;

	//��ʱudΪ
	result->ud = id;

	result->data = NULL;

	void * sin_addr = (u.s.sa_family == AF_INET) ? (void*)&u.v4.sin_addr : (void *)&u.v6.sin6_addr;
	
	int sin_port = ntohs((u.s.sa_family == AF_INET) ? u.v4.sin_port : u.v6.sin6_port);
	char tmp[INET6_ADDRSTRLEN];

	// ���Եȷ���ip port��������,����1��ʾ�ɹ�
	if (inet_ntop(u.s.sa_family, sin_addr, tmp, sizeof(tmp))) {

		//�����ݴ洢���˷���������� buffer��������
		snprintf(ss->buffer, sizeof(ss->buffer), "%s:%d", tmp, sin_port);

		
		result->data = ss->buffer;
	}

	return 1;
}


//����رյ�event
static inline void 
clear_closed_event(struct socket_server *ss, struct socket_message * result, int type) {

	if (type == SOCKET_CLOSE || type == SOCKET_ERROR) {
		int id = result->id;
		int i;
		//event_n��epoll_wait���صĻ�Ծ���������ĸ���
		for (i=ss->event_index; i<ss->event_n; i++) {

			//ss->ev�Ǵ洢epoll_wait()���صĻ�Ծ��������Ӧ�Ľṹ��
			struct event *e = &ss->ev[i];

			//�õ��ṹ��ָ���socket
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
// ������Ļ� ���ȼ������
// û�������ʱ��
//����epoll

//result,more���Ǵ��봫������,����ʱ,more==1
int 
socket_server_poll(struct socket_server *ss, struct socket_message * result, int * more) {

	for (;;) {

		//�������
		if (ss->checkctrl) {

			// has_cmd�ڲ�����select���� �жϹܵ��Ƿ�������  ʹ��select������ û��ʹ��epollʱΪ���������ļ��Ƶ��
			if (has_cmd(ss)) {

				// ��������,���������ַ�����������Ӧ�Ĵ�����
				int type = ctrl_cmd(ss, result);
				if (type != -1) {
					//�رմ洢epoll_wait���ص������еĹرյ�
					clear_closed_event(ss, result, type);

					return type;
				} else
					continue;
			} 
			else 
			{
			
			//�Ѿ�û�п���������
				ss->checkctrl = 0;
			}
		}

		 // ��ǰ�Ĵ����������� ���������� �����ȴ��¼��ĵ���
		if (ss->event_index == ss->event_n) {


			//����epoll_wait����,
			//int epoll_wait(int epfd, struct epoll_event * events, intmaxevents, int timeout);
			//ԭ����struct epoll_events�ṹ�壬events->data.ptr==s

			//ss->ev�Ƿ��صĻ�Ծ����������Ӧ�Ľṹ�������
			//event_n��epoll_wait()�ķ���ֵ
			ss->event_n = sp_wait(ss->event_fd, ss->ev, MAX_EVENT);
			
			ss->checkctrl = 1;
			//����ʱ   more=1
			//�����޸�Ϊ0,���Ա�ǵ����� sp_wait()
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

		//�õ�һ����Ծ����������Ӧ�Ľṹ��
		struct event *e = &ss->ev[ss->event_index++];

		//�õ���Ӧ��socket����ָ��
		struct socket *s = e->s;
		if (s == NULL) {
			// dispatch pipe message at beginning
			continue;
		}

		//�õ����¼��������ļ������� ��Ӧ�� socket����� ����
		switch (s->type) {
			
	// ���������е��׽��� ��д�¼�����, ���ܳɹ���Ҳ���ܳ���
		case SOCKET_TYPE_CONNECTING:
			
			return report_connect(ss, s, result);

		//����Ǽ����׽������¼�����
		case SOCKET_TYPE_LISTEN: {

			//����,�ɹ�����1    
			//����accept���� ,���µ����������뵽��socket_server��socket�����У�����û�м��뵽epoll��
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
			//�Ѿ����ӵ��׽���,������accept�õ�����������Ծ

			//����ǿɶ�
			if (e->read) {
				int type;
				//�����tcp
				if (s->protocol == PROTOCOL_TCP) {

					//��ȡ���ݣ������ڶ��ϣ�ͨ��result����
					type = forward_message_tcp(ss, s, result);
				} else {
				//udp����
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

			//�����д
			if (e->write) {
				// ��д�¼� ��Ӧ�ò㻺���� ȡ�����ݷ���
				
				int type = send_buffer(ss, s, result);
				if (type == -1)
					break;
				return type;
			}
			break;
		}
	}
}


//��ܵ���������
static void
send_request(struct socket_server *ss, struct request_package *request, char type, int len) {
	request->header[6] = (uint8_t)type;
	request->header[7] = (uint8_t)len;
	for (;;) {

		//��ܵ�д����
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


//����connect,׼��һ������� struct request_package 
//req�Ǵ��봫������
static int
open_request(struct socket_server *ss, struct request_package *req, uintptr_t opaque, const char *addr, int port) {
	int len = strlen(addr);

	//������ݹ���
	if (len + sizeof(req->u.open) >= 256) {
		fprintf(stderr, "socket-server : Invalid addr %s.\n",addr);
		return -1;
	}

	//����һ��Ӧ�ò�socket,������skynet_server��socket�����еõ�һ������ʹ�õ�λ��
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

 
//������������  ��Ӧ�ĵ��� open_socket()����
int 
socket_server_connect(struct socket_server *ss, uintptr_t opaque, const char * addr, int port) {
	struct request_package request;

	//��װһ�������,ͨ��request����
	int len = open_request(ss, &request, opaque, addr, port);
	if (len < 0)
		return -1;

	//��ܵ�����O����   ,��ѭ���ж�Ӧ�ĵ��� open_socket()����
	send_request(ss, &request, 'O', sizeof(request.u.open) + len);

	return request.u.open.id;
}



//����buffer
static void
free_buffer(struct socket_server *ss, const void * buffer, int sz) {
	struct send_object so;
	send_object_init(ss, &so, (void *)buffer, sz);
	so.free_func((void *)buffer);
}



// return -1 when error
//���������� ��Ӧ�ĵ���send_socket()����,
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

	//����D�����Ӧ�ĵ��� send_socket()����,ʹ�õ��Ǹ����ȼ��Ļ�����
	send_request(ss, &request, 'D', sizeof(request.u.send));
	return s->wb_size;
}


//����������  ��Ӧ�ĵ��� send_socket()����,ʹ�õ��ǵ����ȼ��Ļ�����
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

	//����P����  ��Ӧ�ĵ��� send_socket()����,ʹ�õ��ǵ����ȼ��Ļ�����
	send_request(ss, &request, 'P', sizeof(request.u.send));
}

//�����˳�
void
socket_server_exit(struct socket_server *ss) {
	struct request_package request;
	send_request(ss, &request, 'X', 0);
}


//����ر�  ��Ӧ�ĵ��� close_socket()
void
socket_server_close(struct socket_server *ss, uintptr_t opaque, int id) {
	struct request_package request;
	request.u.close.id = id;
	request.u.close.shutdown = 0;
	request.u.close.opaque = opaque;
	//��Ӧ�ĵ��� close_socket()
	send_request(ss, &request, 'K', sizeof(request.u.close));
}


//�����ر�  ��Ӧ�ĵ���close_socket()
void
socket_server_shutdown(struct socket_server *ss, uintptr_t opaque, int id) {
	struct request_package request;
	request.u.close.id = id;
	request.u.close.shutdown = 1;
	request.u.close.opaque = opaque;

	//��Ӧ�ĵ���close_socket()
	send_request(ss, &request, 'K', sizeof(request.u.close));
}


// return -1 means failed
// or return AF_INET or AF_INET6
//����socket�����������׽��֣����ҵ�����bind����
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

	//����socket����,�����׽���
	fd = socket(*family, ai_list->ai_socktype, 0);
	if (fd < 0) {
		goto _failed_fd;
	}

	//���õ�ַ�ظ�����
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (void *)&reuse, sizeof(int))==-1) {
		goto _failed;
	}

	//����bind����
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

//����listen����,�ȵ���do_bind() ����
static int
do_listen(const char * host, int port, int backlog) {
	int family = 0;
					//socket()  bind()
	int listen_fd = do_bind(host, port, IPPROTO_TCP, &family);

	if (listen_fd < 0) {
		return -1;
	}

	//����Ϊ����
	if (listen(listen_fd, backlog) == -1) {
		close(listen_fd);
		return -1;
	}
	return listen_fd;
}


//����do_listen()����,����   L����
//��Ӧ�ĵ��� listen_socket()����
//�����������뵽socket_server��socket�����У���û�м��뵽epoll�У�ֻ�ǽ�״̬���ΪSOCKET_TYPE_PLISTEN
//�൱��
//socket()  bind()  listen()  ����socket����   ��û�м��뵽epoll�й���
int 
socket_server_listen(struct socket_server *ss, uintptr_t opaque, const char * addr, int port, int backlog) {

				//socket()  bind()  listen() 
	int fd = do_listen(addr, port, backlog);
	if (fd < 0) {
		return -1;
	}
	struct request_package request;

	//�õ�һ��Ԥ����λ��
	int id = reserve_id(ss);
	
	if (id < 0) {
		close(fd);
		return id;
	}
	request.u.listen.opaque = opaque;
	request.u.listen.id = id;
	request.u.listen.fd = fd;

	//��Ӧ�ĵ��� listen_socket()����
	//�����������뵽socket_server��socket�����У���û�м��뵽epoll�У�ֻ�ǽ�״̬���ΪSOCKET_TYPE_PLISTEN

	send_request(ss, &request, 'L', sizeof(request.u.listen));

	return id;
}


//����'B'��������
// ��Ӧ�ĵ��� bind_socket �����������뵽socket_server��socket������ ����״̬���Ϊ SOCKET_TYPE_BIND
int
socket_server_bind(struct socket_server *ss, uintptr_t opaque, int fd) {
	struct request_package request;
	int id = reserve_id(ss);
	if (id < 0)
		return -1;
	request.u.bind.opaque = opaque;
	request.u.bind.id = id;
	request.u.bind.fd = fd;
	// ��Ӧ�ĵ��� bind_socket �����������뵽socket_server��socket������ ����״̬���Ϊ SOCKET_TYPE_BIND
	send_request(ss, &request, 'B', sizeof(request.u.bind));
	return id;
}


//����'S'��������
//��Ӧ�ĵ��� start_socket 
//��SOCKET_TYPE_PACCEPT����SOCKET_TYPE_PLISTEN����������socket,�������epoll������������״̬
void 
socket_server_start(struct socket_server *ss, uintptr_t opaque, int id) {
	struct request_package request;
	request.u.start.id = id;
	request.u.start.opaque = opaque;

	
	//��Ӧ�ĵ��� start_socket 
	//��SOCKET_TYPE_PACCEPT����SOCKET_TYPE_PLISTEN����������socket,�������epoll������������״̬
	send_request(ss, &request, 'S', sizeof(request.u.start));
}


//����'T'��������
//��Ӧ�ĵ���setopt_socket  ����setsockopt()�������������� 
//����������TCP_NODELAY����
void
socket_server_nodelay(struct socket_server *ss, int id) {
	struct request_package request;
	request.u.setopt.id = id;
	request.u.setopt.what = TCP_NODELAY;
	request.u.setopt.value = 1;
	
	//��Ӧ�ĵ���setopt_socket  ����setsockopt()�������������� 
	send_request(ss, &request, 'T', sizeof(request.u.setopt));
}


void 
socket_server_userobject(struct socket_server *ss, struct socket_object_interface *soi) {
	ss->soi = *soi;
}

// UDP

//����'U'��������
//��Ӧ�ĵ���add_udp_socket  ��udp���͵�  fd���뵽socket_server��socket������,���뵽epoll�й���
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
	
	//��Ӧ�ĵ���add_udp_socket  ��udp���͵�  fd���뵽socket_server��socket������,���뵽epoll�й���
	send_request(ss, &request, 'U', sizeof(request.u.udp));	
	return id;
}

//����'A'��������
//��Ӧ�ĵ��� send_socket()����udp����

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

	//��Ӧ�ĵ��� send_socket()����udp����
	send_request(ss, &request, 'A', sizeof(request.u.send_udp.send)+addrsz);
	return s->wb_size;
}

//����'C'��������
//��Ӧ�ĵ���set_udp_address ����udp�ĵ�ַ��Ϣ
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

	//��Ӧ�ĵ���set_udp_address ����udp�ĵ�ַ��Ϣ
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


