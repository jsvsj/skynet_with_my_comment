#include "skynet.h"
#include "skynet_server.h"
#include "skynet_imp.h"
#include "skynet_mq.h"
#include "skynet_handle.h"
#include "skynet_module.h"
#include "skynet_timer.h"
#include "skynet_monitor.h"
#include "skynet_socket.h"
#include "skynet_daemon.h"
#include "skynet_harbor.h"

#include <pthread.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

/*
//�����̼߳�ر���
struct skynet_monitor {
	int version;
	int check_version;
	uint32_t source;
	uint32_t destination;
};


*/


// ���,����skynet_monitorָ������
struct monitor {				

	int count;			// �������߳��� skynet�ڲ�ʵ������  count + 3 ����3���̵߳�	

	//skynet_monitorָ������
	struct skynet_monitor ** m;		// monitor �����̼߳�ر�				
	
	pthread_cond_t cond;	// �������� 			
	pthread_mutex_t mutex;	// ������		 ���������ͻ�����ʵ���̵߳�ͬ�� 		
	int sleep;		// ˯���еĹ������߳��� 		
	int quit;		//�Ƿ��˳�
};


// �����̲߳��� �����߳�
struct worker_parm {
	struct monitor *m;
	int id;
	int weight;
};

static int SIG = 0;


//SIGHUP�źŵĻص�����
static void
handle_hup(int signal) {
	if (signal == SIGHUP) {
		SIG = 1;
	}
}

#define CHECK_ABORT if (skynet_context_total()==0) break;	// ������Ϊ0



//�����߳�
static void
create_thread(pthread_t *thread, void *(*start_routine) (void *), void *arg) {
	if (pthread_create(thread,NULL, start_routine, arg)) {
		fprintf(stderr, "Create thread failed");
		exit(1);
	}
}


// ȫ���̶߳�˯�ߵ�����²Ż���һ�������߳�(��ֻҪ�й����̴߳��ڹ���״̬������Ҫ����)
static void
wakeup(struct monitor *m, int busy) {

	//m->count �����Ĺ����߳���
	//m->sleep �ȴ��Ĺ����߳���
	//busyΪ0ʱ����ʾֻ��ȫ��................
	
	if (m->sleep >= m->count - busy) {	// ˯�ߵ��߳�
		// signal sleep worker, "spurious wakeup" is harmless

		//m��һ�����ã��ö���̹߳�������,��������
		pthread_cond_signal(&m->cond);
	}
}


//socket�߳�  �����¼��̣߳�����epoll_wait()
static void *
thread_socket(void *p) {
	struct monitor * m = p;

	//��ʼ���߳�,��ʼ���߳�ȫ�ֱ���
	//#define THREAD_SOCKET 2
	skynet_initthread(THREAD_SOCKET);
	
	for (;;) {

		//���ȵ���	socket_server_poll(),�ú����ȼ�����Ȼ����� sp_wait(),��linux�£��ú�������epoll_wait()
		int r = skynet_socket_poll();

		//����0��ʾҪ�˳�
		if (r==0)
			break;

		//��ʾ��Ϣ������󣬻���û�е���epoll_wait()
		if (r<0) {
			
			//#define CHECK_ABORT if (skynet_context_total()==0) break;	// ������Ϊ0
			CHECK_ABORT
			continue;
		}
		// ��socket��Ϣ����
		wakeup(m,0);	// ȫ���̶߳�˯�ߵ�����²Ż���һ�������߳�(��ֻҪ�й����̴߳��ڹ���״̬������Ҫ����)
	}
	return NULL;
}

//����struct monitor
static void
free_monitor(struct monitor *m) {
	int i;
	int n = m->count;
	//����ÿһ��
	for (i=0;i<n;i++) {
		skynet_monitor_delete(m->m[i]);
	}

	//������
	pthread_mutex_destroy(&m->mutex);
	pthread_cond_destroy(&m->cond);
	skynet_free(m->m);
	skynet_free(m);
}


// ���ڼ���Ƿ�����Ϣû�м�ʱ����
static void *
thread_monitor(void *p) {
	struct monitor * m = p;
	int i;

	//�����߳���
	int n = m->count;

	//���Ի��ֲ߳̾�����
	skynet_initthread(THREAD_MONITOR);
	
	for (;;) {

		//#define CHECK_ABORT if (skynet_context_total()==0) break;	// ������Ϊ0
		
		CHECK_ABORT
		for (i=0;i<n;i++) {
			skynet_monitor_check(m->m[i]);
		}
		
		//˯��5��
		for (i=0;i<5;i++) {

			//#define CHECK_ABORT if (skynet_context_total()==0) break;	// ������Ϊ0
			CHECK_ABORT
			sleep(1);
		}
	}

	return NULL;
}

static void
signal_hup() {
	// make log file reopen

	struct skynet_message smsg;
	smsg.source = 0;
	smsg.session = 0;
	smsg.data = NULL;
	smsg.sz = (size_t)PTYPE_SYSTEM << MESSAGE_TYPE_SHIFT;
	uint32_t logger = skynet_handle_findname("logger");
	if (logger) {
		skynet_context_push(logger, &smsg);
	}
}


// ���ڶ�ʱ��
static void *
thread_timer(void *p) {
	struct monitor * m = p;
	
	skynet_initthread(THREAD_TIMER);
	
	for (;;) {

		//����ʱ�¼�,ֻ�����ض���skynet_context������Ϣ,��û���ض��Ĵ���
		skynet_updatetime();
		CHECK_ABORT
		//m->count�����߳���
		wakeup(m,m->count-1);	// ֻҪ��һ��˯���߳̾ͻ��ѣ��ù����߳�������
		usleep(2500);
		if (SIG) {
			signal_hup();
			SIG = 0;
		}
	}
	
	// wakeup socket thread
	skynet_socket_exit();
	// wakeup all worker thread

	pthread_mutex_lock(&m->mutex);
	m->quit = 1;
	pthread_cond_broadcast(&m->cond);

	pthread_mutex_unlock(&m->mutex);
	return NULL;
}


// �����߳�
static void *
thread_worker(void *p) {
	struct worker_parm *wp = p;
	
	int id = wp->id;
	int weight = wp->weight;
	
	//�õ�monitor�ṹ��
	struct monitor *m = wp->m;
	
	//��ȡ��Ӧ��skynet_monitor
	struct skynet_monitor *sm = m->m[id];

	//��ʼ���߳�,��ʼ���߳�ȫ�ֱ���  THREAD_WORKER  ���� 0
	skynet_initthread(THREAD_WORKER);
	
	struct message_queue * q = NULL;

	//��û���˳���ʱ��
	while (!m->quit) {

		//����һ����Ϣ��������skynet_context�Ļص�����
		q = skynet_context_message_dispatch(sm, q, weight);
		
		if (q == NULL) {
			if (pthread_mutex_lock(&m->mutex) == 0) {

				// ��װ������ʱ�޺��� ��Ϊ skynet_ctx_msg_dispatch() �������κ�ʱ�򱻵���

				//sleep,�����ж�Ҫ��Ҫ����pthread_cond_signal��
				//�ȴ��Ĺ����߳̿���socket�̺߳�timer�߳��л���,
				//ǰ�ߣ���socket��Ϣʱ�����һ��
				//����ÿ��ˢ��ʱ��ỽ��һ��

				//��˯�ߵĹ����߳���+1
				++ m->sleep;
				
				// "spurious wakeup" is harmless,
				// because skynet_context_message_dispatch() can be call at any time.
	
				//�ȴ����ѣ�������Ϣ����
				if (!m->quit)
					pthread_cond_wait(&m->cond, &m->mutex);


				//˯�ߵĹ����߳���-1
				-- m->sleep;

				if (pthread_mutex_unlock(&m->mutex)) {
					fprintf(stderr, "unlock mutex error");
					exit(1);
				}
			}
		}
	}
	return NULL;
}


//�����߳�
static void
start(int thread) {

	// �߳���+3 3���̷ֱ߳����� _monitor _timer  _socket ��� ��ʱ�� socket IO
	pthread_t pid[thread+3]; 

	//��������̵߳Ľṹ��
	struct monitor *m = skynet_malloc(sizeof(*m));
	memset(m, 0, sizeof(*m));
	m->count = thread;
	m->sleep = 0;

	//Ϊ struct skynet_monitor *ָ����������ڴ�ռ�
	m->m = skynet_malloc(thread * sizeof(struct skynet_monitor *));
	int i;
	
	for (i=0;i<thread;i++) {
		//����struct skynet_monitor,����ָ��,���ݶ���ʼ��Ϊ 0
		m->m[i] = skynet_monitor_new();
	}

	//��ʼ��m����
	if (pthread_mutex_init(&m->mutex, NULL)) {
		fprintf(stderr, "Init mutex error");
		exit(1);
	}
	//��ʼ��m����������
	if (pthread_cond_init(&m->cond, NULL)) {
		fprintf(stderr, "Init cond error");
		exit(1);
	}


	//��������̣߳��ײ���õ���pthread_create(),thread_monitor�ǻص�����,m�ǻص������Ĳ���
	create_thread(&pid[0], thread_monitor, m);


	//����timer()��ʱ���߳�
	create_thread(&pid[1], thread_timer, m);

	//����socket�����̣߳�
	create_thread(&pid[2], thread_socket, m);



	//��ž��ǣ��ѹ����̷߳��飬ǰ����ÿ��8���������Ĺ�������顣�
	//A,E��ÿ�ε��ȴ���һ����Ϣ��B��ÿ�δ���n/2����C��ÿ�δ���n/4����
	//D��ÿ�δ���n/8������Ϊ�˾���ʹ�ö��

	static int weight[] = { 
		-1, -1, -1, -1, 0, 0, 0, 0,
		1, 1, 1, 1, 1, 1, 1, 1, 
		2, 2, 2, 2, 2, 2, 2, 2, 
		3, 3, 3, 3, 3, 3, 3, 3, };

/*

struct worker_parm {
	struct monitor *m;
	int id;
	int weight;
};
*/
	//���ڴ��ݸ������̵߳Ļص�����
	struct worker_parm wp[thread];

	for (i=0;i<thread;i++) {
		wp[i].m = m;
		wp[i].id = i;
		//��� i ��ֵС�����鳤��
		if (i < sizeof(weight)/sizeof(weight[0]))
		{
			wp[i].weight= weight[i];
		}
		else 
		{
			wp[i].weight = 0;
		}
		create_thread(&pid[i+3], thread_worker, &wp[i]);
	}

	for (i=0;i<thread+3;i++) {
		// �ȴ������߳��˳�
		pthread_join(pid[i], NULL); 
	}

	//����struct monitor
	free_monitor(m);
}


//��������ģ�顣��Ҫ��Skynet�����ļ��ж��壬Ĭ��Ϊboostrap="snlua boostrap",��ʾ����
//�������snlua.soģ�飬����snlua��������boostrap.lua�ű��������ʹ��snluaҲ����ֱ������
//��������Ķ�̬��

static void
bootstrap(struct skynet_context * logger, const char * cmdline) {
	//cmdline Ϊ snlua boostrap
	
	int sz = strlen(cmdline);
	char name[sz+1];	// snlua
	char args[sz+1];	// boostrap
	sscanf(cmdline, "%s %s", name, args);

	//����ģ�� 										snlua  boostrap
	struct skynet_context *ctx = skynet_context_new(name, args);
	if (ctx == NULL) {
		skynet_error(NULL, "Bootstrap error : %s\n", cmdline);
		//����ctx��ѭ����Ϣ�����е�������Ϣ,���� �ص�����
		skynet_context_dispatchall(logger);
		exit(1);
	}
}

// skynet ������ʱ�� ��ʼ��
//��skynet_main.c��mian�����е���

void 
skynet_start(struct skynet_config * config) {
	// register SIGHUP for log file reopen
	struct sigaction sa;

	//�źŵĻص�����
	sa.sa_handler = &handle_hup;
	sa.sa_flags = SA_RESTART;
	sigfillset(&sa.sa_mask);
	sigaction(SIGHUP, &sa, NULL);


	//configΪ�������ò��������Ľṹ��
	
	if (config->daemon) {

		//��ʼ���ػ��߽��̣��������ļ�ȷ���Ƿ����á��ú�����skynet_damenon.c��
		if (daemon_init(config->daemon)) {
			exit(1);
		}
	}

	//��ʼ���ڵ�ģ�飬���ڼ�Ⱥ��ת��Զ�̽ڵ����Ϣ���ú���������skynet_horbor.c��
	skynet_harbor_init(config->harbor);

	//��ʼ�����ģ�飬���ڸ�ÿ��Skynet

	//��ʼ��handler_storage,һ���洢skynet_contextָ�������
	skynet_handle_init(config->harbor);

	//��ʼ��ȫ�ֵ���Ϣ����ģ�飬����Skynet����Ҫ���ݽṹ���������������skynet_mq.c��
	skynet_mq_init();

	//��ʼ������̬�����ģ�飬��Ҫ���ڼ��ط���Skynet����ģ��ӿڵĶ�̬���ӿ⡣
	//�������������skynet_module.c��
	//��ʼ��ȫ�ֵ�modules,ʵ�ʾ�����һ��ָ�����飬����skynet_mudules
	skynet_module_init(config->module_path);

	//��ʼ����ʱ��ģ��,�ú���������skynet_socket.c��
	//��ʼ�� static struct timer * TI 
	skynet_timer_init();

	//��ʼ������ģ�顣�������������skynet_socket.c ��
	//�ײ��ʼ����һ�� socket_server�ṹ��, ���õ�epoll_create()����
	skynet_socket_init();

	
	skynet_profile_enable(config->profile);

	//���� skynet_context���� ,������־ģ��            ("logger",NULL)
	struct skynet_context *ctx = skynet_context_new(config->logservice, config->logger);
	
	if (ctx == NULL) {
		fprintf(stderr, "Can't launch %s service\n", config->logservice);
		exit(1);
	}

	//��������ģ�顣��Ҫ��Skynet�����ļ��ж��壬Ĭ��Ϊboostrap="snlua boostrap",��ʾ����
	//�������snlua.soģ�飬����snlua��������boostrap.lua�ű��������ʹ��snluaҲ����ֱ������
	//��������Ķ�̬��
	bootstrap(ctx, config->bootstrap);

	//����monitor()�����̣߳���create_thread()������create_thread()��װ��ϵͳ����pthread_create()
//����socket�����̣߳�
//����timer()��ʱ���߳�
//����worker()�����̣߳������̵߳�������Skynet�����ļ��е�thread=8���塣һ�����
//��������CPU������

	//config�б����˴������ļ���ȡ��work�߳���
	start(config->thread);


	// harbor_exit may call socket send, so it should exit before socket_free
	skynet_harbor_exit();


	//�ͷ�����ģ��
	skynet_socket_free();

	
	if (config->daemon) {
		//�˳��ػ����̡�
		daemon_exit(config->daemon);
	}
}
