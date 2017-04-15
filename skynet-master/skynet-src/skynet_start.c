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
//¹¤×÷Ïß³Ì¼à¿Ø±íÏî
struct skynet_monitor {
	int version;
	int check_version;
	uint32_t source;
	uint32_t destination;
};


*/


// ¼à¿Ø,¹ÜÀískynet_monitorÖ¸ÕëÊı×é
struct monitor {				

	int count;			// ¹¤×÷ÕßÏß³ÌÊı skynetÄÚ²¿Êµ¼ÊÉÏÊÇ  count + 3 ¶àÁË3¸öÏß³ÌµÄ	

	//skynet_monitorÖ¸ÕëÊı×é
	struct skynet_monitor ** m;		// monitor ¹¤×÷Ïß³Ì¼à¿Ø±í				
	
	pthread_cond_t cond;	// Ìõ¼ş±äÁ¿ 			
	pthread_mutex_t mutex;	// »¥³âËø		 Ìõ¼ş±äÁ¿ºÍ»¥³âËøÊµÏÖÏß³ÌµÄÍ¬²½ 		
	int sleep;		// Ë¯ÃßÖĞµÄ¹¤×÷ÕßÏß³ÌÊı 		
	int quit;		//ÊÇ·ñÍË³ö
};


// ÓÃÓÚÏß³Ì²ÎÊı ¹¤×÷Ïß³Ì
struct worker_parm {
	struct monitor *m;
	int id;
	int weight;
};

static int SIG = 0;


//SIGHUPĞÅºÅµÄ»Øµ÷º¯Êı
static void
handle_hup(int signal) {
	if (signal == SIGHUP) {
		SIG = 1;
	}
}

#define CHECK_ABORT if (skynet_context_total()==0) break;	// ·şÎñÊıÎª0



//´´½¨Ïß³Ì
static void
create_thread(pthread_t *thread, void *(*start_routine) (void *), void *arg) {
	if (pthread_create(thread,NULL, start_routine, arg)) {
		fprintf(stderr, "Create thread failed");
		exit(1);
	}
}


// È«²¿Ïß³Ì¶¼Ë¯ÃßµÄÇé¿öÏÂ²Å»½ĞÑÒ»¸ö¹¤×÷Ïß³Ì(¼´Ö»ÒªÓĞ¹¤×÷Ïß³Ì´¦ÓÚ¹¤×÷×´Ì¬£¬Ôò²»ĞèÒª»½ĞÑ)
static void
wakeup(struct monitor *m, int busy) {

	//m->count ´´½¨µÄ¹¤×÷Ïß³ÌÊı
	//m->sleep µÈ´ıµÄ¹¤×÷Ïß³ÌÊı
	//busyÎª0Ê±£¬±íÊ¾Ö»ÓĞÈ«²¿................
	
	if (m->sleep >= m->count - busy) {	// Ë¯ÃßµÄÏß³Ì
		// signal sleep worker, "spurious wakeup" is harmless

		//mµÄÒ»¸ö×öÓÃ£¬ÈÃ¶à¸öÏß³Ì¹²Ïí»¥³âÁ¿,Ìõ¼ş±äÁ¿
		pthread_cond_signal(&m->cond);
	}
}


//socketÏß³Ì  ÍøÂçÊÂ¼şÏß³Ì£¬µ÷ÓÃepoll_wait()
static void *
thread_socket(void *p) {
	struct monitor * m = p;

	//³õÊ¼»¯Ïß³Ì,³õÊ¼»¯Ïß³ÌÈ«¾Ö±äÁ¿
	//#define THREAD_SOCKET 2
	skynet_initthread(THREAD_SOCKET);
	
	for (;;) {

		//Ê×ÏÈµ÷ÓÃ	socket_server_poll(),¸Ãº¯ÊıÏÈ¼ì²âÃüÁî£¬È»ºóµ÷ÓÃ sp_wait(),ÔÚlinuxÏÂ£¬¸Ãº¯Êıµ÷ÓÃepoll_wait()
		int r = skynet_socket_poll();

		//·µ»Ø0±íÊ¾ÒªÍË³ö
		if (r==0)
			break;

		//±íÊ¾ÏûÏ¢´¦Àí´íÎó£¬»òÔçÃ»ÓĞµ÷ÓÃepoll_wait()
		if (r<0) {
			
			//#define CHECK_ABORT if (skynet_context_total()==0) break;	// ·şÎñÊıÎª0
			CHECK_ABORT
			continue;
		}
		// ÓĞsocketÏûÏ¢·µ»Ø
		wakeup(m,0);	// È«²¿Ïß³Ì¶¼Ë¯ÃßµÄÇé¿öÏÂ²Å»½ĞÑÒ»¸ö¹¤×÷Ïß³Ì(¼´Ö»ÒªÓĞ¹¤×÷Ïß³Ì´¦ÓÚ¹¤×÷×´Ì¬£¬Ôò²»ĞèÒª»½ĞÑ)
	}
	return NULL;
}

//Ïú»Ùstruct monitor
static void
free_monitor(struct monitor *m) {
	int i;
	int n = m->count;
	//Ïú»ÙÃ¿Ò»Ïî
	for (i=0;i<n;i++) {
		skynet_monitor_delete(m->m[i]);
	}

	//Ïú»ÙËø
	pthread_mutex_destroy(&m->mutex);
	pthread_cond_destroy(&m->cond);
	skynet_free(m->m);
	skynet_free(m);
}


// ÓÃÓÚ¼à¿ØÊÇ·ñÓĞÏûÏ¢Ã»ÓĞ¼´Ê±´¦Àí
static void *
thread_monitor(void *p) {
	struct monitor * m = p;
	int i;

	//¹¤×÷Ïß³ÌÊı
	int n = m->count;

	//³õÊÔ»¯Ïß³Ì¾Ö²¿±äÁ¿
	skynet_initthread(THREAD_MONITOR);
	
	for (;;) {

		//#define CHECK_ABORT if (skynet_context_total()==0) break;	// ·şÎñÊıÎª0
		
		CHECK_ABORT
		for (i=0;i<n;i++) {
			skynet_monitor_check(m->m[i]);
		}
		
		//Ë¯Ãß5Ãë
		for (i=0;i<5;i++) {

			//#define CHECK_ABORT if (skynet_context_total()==0) break;	// ·şÎñÊıÎª0
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


// ÓÃÓÚ¶¨Ê±Æ÷
static void *
thread_timer(void *p) {
	struct monitor * m = p;
	
	skynet_initthread(THREAD_TIMER);
	
	for (;;) {

		//´¦Àí³¬Ê±ÊÂ¼ş,Ö»ÊÇÏñÌØ¶¨µÄskynet_context·¢ËÍÏûÏ¢,²¢Ã»ÓĞÌØ¶¨µÄ´¦Àí
		skynet_updatetime();
		CHECK_ABORT
		//m->count¹¤×÷Ïß³ÌÊı
		wakeup(m,m->count-1);	// Ö»ÒªÓĞÒ»¸öË¯ÃßÏß³Ì¾Í»½ĞÑ£¬ÈÃ¹¤×÷Ïß³ÌÈÈÆğÀ´
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


// ¹¤×÷Ïß³Ì
static void *
thread_worker(void *p) {
	struct worker_parm *wp = p;
	
	int id = wp->id;
	int weight = wp->weight;
	
	//µÃµ½monitor½á¹¹Ìå
	struct monitor *m = wp->m;
	
	//»ñÈ¡¶ÔÓ¦µÄskynet_monitor
	struct skynet_monitor *sm = m->m[id];

	//³õÊ¼»¯Ïß³Ì,³õÊ¼»¯Ïß³ÌÈ«¾Ö±äÁ¿  THREAD_WORKER  ´ú±í 0
	skynet_initthread(THREAD_WORKER);
	
	struct message_queue * q = NULL;

	//µ±Ã»ÓĞÍË³öµÄÊ±ºò
	while (!m->quit) {

		//´¦ÀíÒ»ÌõÏûÏ¢£¬µ÷ÓÃÁËskynet_contextµÄ»Øµ÷º¯Êı
		q = skynet_context_message_dispatch(sm, q, weight);
		
		if (q == NULL) {
			if (pthread_mutex_lock(&m->mutex) == 0) {

				// ¼Ù×°µÄĞÑÀ´Ê±ÎŞº¦µÄ ÒòÎª skynet_ctx_msg_dispatch() ¿ÉÒÔÔÚÈÎºÎÊ±ºò±»µ÷ÓÃ

				//sleep,ÓÃÀ´ÅĞ¶ÏÒª²»Òªµ÷ÓÃpthread_cond_signalµÄ
				//µÈ´ıµÄ¹¤×÷Ïß³Ì¿ÉÔÚsocketÏß³ÌºÍtimerÏß³ÌÖĞ»½ĞÑ,
				//Ç°Õß£¬ÓĞsocketÏûÏ¢Ê±»áµ÷ÓÃÒ»´Î
				//ºóÕßÃ¿¸öË¢ĞÂÊ±¼ä»á»½ĞÑÒ»´Î

				//½«Ë¯ÃßµÄ¹¤×÷Ïß³ÌÊı+1
				++ m->sleep;
				
				// "spurious wakeup" is harmless,
				// because skynet_context_message_dispatch() can be call at any time.
	
				//µÈ´ı»½ĞÑ£¬¼´ÓĞÏûÏ¢µ½À´
				if (!m->quit)
					pthread_cond_wait(&m->cond, &m->mutex);


				//Ë¯ÃßµÄ¹¤×÷Ïß³ÌÊı-1
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


//´´½¨Ïß³Ì
static void
start(int thread) {

	// Ïß³ÌÊı+3 3¸öÏß³Ì·Ö±ğÓÃÓÚ _monitor _timer  _socket ¼à¿Ø ¶¨Ê±Æ÷ socket IO
	pthread_t pid[thread+3]; 

	//´´½¨¼à¿ØÏß³ÌµÄ½á¹¹Ìå
	struct monitor *m = skynet_malloc(sizeof(*m));
	memset(m, 0, sizeof(*m));
	m->count = thread;
	m->sleep = 0;

	//Îª struct skynet_monitor *Ö¸ÕëÊı×é·ÖÅäÄÚ´æ¿Õ¼ä
	m->m = skynet_malloc(thread * sizeof(struct skynet_monitor *));
	int i;
	
	for (i=0;i<thread;i++) {
		//´´½¨struct skynet_monitor,·µ»ØÖ¸Õë,Êı¾İ¶¼³õÊ¼»¯Îª 0
		m->m[i] = skynet_monitor_new();
	}

	//³õÊ¼»¯mµÄËø
	if (pthread_mutex_init(&m->mutex, NULL)) {
		fprintf(stderr, "Init mutex error");
		exit(1);
	}
	//³õÊ¼»¯mµÄÌõ¼ş±äÁ¿
	if (pthread_cond_init(&m->cond, NULL)) {
		fprintf(stderr, "Init cond error");
		exit(1);
	}


	//´´½¨¼à¿ØÏß³Ì£¬µ×²ãµ÷ÓÃµÄÊÇpthread_create(),thread_monitorÊÇ»Øµ÷º¯Êı,mÊÇ»Øµ÷º¯ÊıµÄ²ÎÊı
	create_thread(&pid[0], thread_monitor, m);


	//´´½¨timer()¶¨Ê±Æ÷Ïß³Ì
	create_thread(&pid[1], thread_timer, m);

	//´´½¨socketÍøÂçÏß³Ì£¬
	create_thread(&pid[2], thread_socket, m);



	//´ó¸Å¾ÍÊÇ£¬°Ñ¹¤×÷Ïß³Ì·Ö×é£¬Ç°ËÄ×éÃ¿×é8¸ö£¬³¬¹ıµÄ¹éÈëµÚÎå×é¡£¬
	//A,E×éÃ¿´Îµ÷¶È´¦ÀíÒ»ÌõÏûÏ¢£¬B×éÃ¿´Î´¦Àín/2Ìõ£¬C×éÃ¿´Î´¦Àín/4Ìõ£¬
	//D×éÃ¿´Î´¦Àín/8Ìõ¡£ÊÇÎªÁË¾ùÔÈÊ¹ÓÃ¶àºË

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
	//ÓÃÓÚ´«µİ¸ø¹¤×÷Ïß³ÌµÄ»Øµ÷º¯Êı
	struct worker_parm wp[thread];

	for (i=0;i<thread;i++) {
		wp[i].m = m;
		wp[i].id = i;
		//Èç¹û i µÄÖµĞ¡ÓÚÊı×é³¤¶È
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
		// µÈ´ıËùÓĞÏß³ÌÍË³ö
		pthread_join(pid[i], NULL); 
	}

	//Ïú»Ùstruct monitor
	free_monitor(m);
}


//¼ÓÔØÒıµ¼Ä£¿é¡£Ö÷ÒªÔÚSkynetÅäÖÃÎÄ¼şÖĞ¶¨Òå£¬Ä¬ÈÏÎªboostrap="snlua boostrap",±íÊ¾Òıµ¼
//³ÌĞò¼ÓÔØsnlua.soÄ£¿é£¬²¢ÓĞsnlua·şÎñÆô¶¯boostrap.lua½Å±¾¡£Èç¹û²»Ê¹ÓÃsnluaÒ²¿ÉÒÔÖ±½ÓÆô¶¯
//ÆäËû·şÎñµÄ¶¯Ì¬¿â

static void
bootstrap(struct skynet_context * logger, const char * cmdline) {
	//cmdline Îª snlua boostrap
	
	int sz = strlen(cmdline);
	char name[sz+1];	// snlua
	char args[sz+1];	// boostrap
	sscanf(cmdline, "%s %s", name, args);

	//¼ÓÔØÄ£¿é 										snlua  boostrap
	struct skynet_context *ctx = skynet_context_new(name, args);
	if (ctx == NULL) {
		skynet_error(NULL, "Bootstrap error : %s\n", cmdline);
		//´¦ÀíctxµÄÑ­»·ÏûÏ¢¶ÓÁĞÖĞµÄËùÓĞÏûÏ¢,µ÷ÓÃ »Øµ÷º¯Êı
		skynet_context_dispatchall(logger);
		exit(1);
	}
}

// skynet Æô¶¯µÄÊ±ºò ³õÊ¼»¯
//ÔÚskynet_main.cµÄmianº¯ÊıÖĞµ÷ÓÃ

void 
skynet_start(struct skynet_config * config) {
	// register SIGHUP for log file reopen
	struct sigaction sa;

	//ĞÅºÅµÄ»Øµ÷º¯Êı
	sa.sa_handler = &handle_hup;
	sa.sa_flags = SA_RESTART;
	sigfillset(&sa.sa_mask);
	sigaction(SIGHUP, &sa, NULL);


	//configÎª±£´æÅäÖÃ²ÎÊı±äÁ¿µÄ½á¹¹Ìå
	
	if (config->daemon) {

		//³õÊ¼»¯ÊØ»¤Ïß½ø³Ì£¬ÓÉÅäÖÃÎÄ¼şÈ·¶¨ÊÇ·ñÆôÓÃ¡£¸Ãº¯ÊıÔÚskynet_damenon.cÖĞ
		if (daemon_init(config->daemon)) {
			exit(1);
		}
	}

	//³õÊ¼»¯½ÚµãÄ£¿é£¬ÓÃÓÚ¼¯Èº£¬×ª·¢Ô¶³Ì½ÚµãµÄÏûÏ¢£¬¸Ãº¯Êı¶¨ÒåÔÚskynet_horbor.cÖĞ
	skynet_harbor_init(config->harbor);

	//³õÊ¼»¯¾ä±úÄ£¿é£¬ÓÃÓÚ¸øÃ¿¸öSkynet

	//³õÊ¼»¯handler_storage,Ò»¸ö´æ´¢skynet_contextÖ¸ÕëµÄÊı×é
	skynet_handle_init(config->harbor);

	//³õÊ¼»¯È«¾ÖµÄÏûÏ¢¶ÓÁĞÄ£¿é£¬ÕâÊÇSkynetµÄÖ÷ÒªÊı¾İ½á¹¹¡£Õâ¸öº¯Êı¶¨ÒåÔÚskynet_mq.cÖĞ
	skynet_mq_init();

	//³õÊ¼»¯·şÎñ¶¯Ì¬¿â¼ÓÔØÄ£¿é£¬Ö÷ÒªÓÃÓÚ¼ÓÔØ·ûºÏSkynet·şÎñÄ£¿é½Ó¿ÚµÄ¶¯Ì¬Á´½Ó¿â¡£
	//Õâ¸öº¯Êı¶¨ÒåÔÚskynet_module.cÖĞ
	//³õÊ¼»¯È«¾ÖµÄmodules,ÊµÖÊ¾ÍÊÇÓĞÒ»¸öÖ¸ÕëÊı×é£¬»º´æskynet_mudules
	skynet_module_init(config->module_path);

	//³õÊ¼»¯¶¨Ê±Æ÷Ä£¿é,¸Ãº¯Êı¶¨ÒåÔÚskynet_socket.cÖĞ
	//³õÊ¼»¯ static struct timer * TI 
	skynet_timer_init();

	//³õÊ¼»¯ÍøÂçÄ£¿é¡£Õâ¸öº¯Êı¶¨ÒåÔÚskynet_socket.c ÖĞ
	//µ×²ã³õÊ¼»¯ÁËÒ»¸ö socket_server½á¹¹Ìå, µ÷ÓÃµÄepoll_create()º¯Êı
	skynet_socket_init();

	
	skynet_profile_enable(config->profile);

	//´´½¨ skynet_context¶ÔÏó ,¼ÓÔØÈÕÖ¾Ä£¿é            ("logger",NULL)
	struct skynet_context *ctx = skynet_context_new(config->logservice, config->logger);
	
	if (ctx == NULL) {
		fprintf(stderr, "Can't launch %s service\n", config->logservice);
		exit(1);
	}

	//¼ÓÔØÒıµ¼Ä£¿é¡£Ö÷ÒªÔÚSkynetÅäÖÃÎÄ¼şÖĞ¶¨Òå£¬Ä¬ÈÏÎªboostrap="snlua boostrap",±íÊ¾Òıµ¼
	//³ÌĞò¼ÓÔØsnlua.soÄ£¿é£¬²¢ÓĞsnlua·şÎñÆô¶¯boostrap.lua½Å±¾¡£Èç¹û²»Ê¹ÓÃsnluaÒ²¿ÉÒÔÖ±½ÓÆô¶¯
	//ÆäËû·şÎñµÄ¶¯Ì¬¿â
	bootstrap(ctx, config->bootstrap);

	//´´½¨monitor()¼àÊÓÏß³Ì£¬ÓÃcreate_thread()´´½¨£¬create_thread()·â×°ÁËÏµÍ³º¯Êıpthread_create()
//´´½¨socketÍøÂçÏß³Ì£¬
//´´½¨timer()¶¨Ê±Æ÷Ïß³Ì
//´´½¨worker()¹¤×÷Ïß³Ì£¬¹¤×÷Ïß³ÌµÄÊıÁ¿ÓĞSkynetÅäÖÃÎÄ¼şÖĞµÄthread=8¶¨Òå¡£Ò»°ã¸ù¾İ
//·şÎñÆ÷µÄCPUºËÊıÀ´

	//configÖĞ±£´æÁË´ÓÅäÖÃÎÄ¼ş¶ÁÈ¡µÄworkÏß³ÌÊı
	start(config->thread);


	// harbor_exit may call socket send, so it should exit before socket_free
	skynet_harbor_exit();


	//ÊÍ·ÅÍøÂçÄ£¿é
	skynet_socket_free();

	
	if (config->daemon) {
		//ÍË³öÊØ»¤½ø³Ì¡£
		daemon_exit(config->daemon);
	}
}
