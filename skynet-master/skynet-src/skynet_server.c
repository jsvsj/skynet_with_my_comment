#include "skynet.h"

#include "skynet_server.h"
#include "skynet_module.h"
#include "skynet_handle.h"
#include "skynet_mq.h"
#include "skynet_timer.h"
#include "skynet_harbor.h"
#include "skynet_env.h"
#include "skynet_monitor.h"
#include "skynet_imp.h"
#include "skynet_log.h"
#include "skynet_timer.h"
#include "spinlock.h"
#include "atomic.h"

#include <pthread.h>

#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>

#ifdef CALLING_CHECK

#define CHECKCALLING_BEGIN(ctx) if (!(spinlock_trylock(&ctx->calling))) { assert(0); }
#define CHECKCALLING_END(ctx) spinlock_unlock(&ctx->calling);
#define CHECKCALLING_INIT(ctx) spinlock_init(&ctx->calling);
#define CHECKCALLING_DESTROY(ctx) spinlock_destroy(&ctx->calling);
#define CHECKCALLING_DECL struct spinlock calling;

#else

#define CHECKCALLING_BEGIN(ctx)
#define CHECKCALLING_END(ctx)
#define CHECKCALLING_INIT(ctx)
#define CHECKCALLING_DESTROY(ctx)
#define CHECKCALLING_DECL

#endif


//skynet ��Ҫ���� ���ط����֪ͨ����
/*
*	һ��ģ��(.so)���ص�skynet����У�����������һ��ʵ������һ������
*	Ϊÿ���������һ��skynet_context�ṹ
*/

//ÿһ�������Ӧ��sy==skynet_ctx�ṹ

//��Ҫ���ؿ�ܵĵ����߼� ���  sc

struct skynet_context {

	//��Լ����create����
	//���ö�̬���е�xxxxx_create()�����ķ���ֵ  ��modָ��Ľṹ���е�,�Զ������ģ����صĽṹ��
	void * instance;		//ģ��xxx_create�������ص�ʵ�� ��Ӧģ��ľ��

	//Ƥ�Ҷ���
	struct skynet_module * mod;		//ģ��,���� ��װ��̬��Ľṹ�� 


	//�ص��������û�����
	void * cb_ud;		//���ݸ��ص������Ĳ�����һ����xxx_create�������ص�ʵ��

	//������Ϣ�Ļص���������Ƥ���߼���ע��
	//typedef int (*skynet_cb)(struct skynet_context * context, void *ud, int type, int session, uint32_t source , const void * msg, size_t sz);

	skynet_cb cb;		//�ص�����

	//actor�����䣬����ܵ�����Ϣ
	struct message_queue *queue;	//һ����Ϣ����

	FILE * logfile;		//��־�ļ����

	//��¼���ûص�����������Ϣ����ʱ��
	uint64_t cpu_cost;	// in microsec
	uint64_t cpu_start;	// in microsec

	//handler��16�����ַ������ڴ���
	char result[32];	//��������ִ�з��ؽ��

	//handle��һ��uint32_t���������߰�λ��ʾԶ�̽ڵ�(���ǿ���Դ��ļ�Ⱥ��ʩ������ķ����������Ӹò���
	
	uint32_t handle;	//������,ʵ�ʾ��Ǹýṹ��ָ������һ��ȫ�ֵ�ָ�������е�һ�����

	//��һ�η����session,���ڷ��䲻�ظ���session
	int session_id;		//�Ựid

	//���ü���
	int ref;			//�̰߳�ȫ�����ü�������֤��ʹ�õ�ʱ��û�б������߳��ͷ�
	int message_count;	//����

	bool init;	//�Ƿ��ʼ��

	//�Ƿ��ڴ�����Ϣʱ��ѭ��
	bool endless;	//�Ƿ�����ѭ��

	bool profile;

	CHECKCALLING_DECL
};


//skynet �Ľڵ�ṹ
struct skynet_node {

	int total;	//һ��skynet_node�ķ����� һ��node�ķ�������

	int init;

	uint32_t monitor_exit;

	pthread_key_t handle_key;
	bool profile;	// default is off
};

//ȫ�ֱ���
static struct skynet_node G_NODE;


//��ȡ��skynet_node�� total
int 
skynet_context_total() {
	return G_NODE.total;
}


//����skynet_node�� total
static void
context_inc() {
	ATOM_INC(&G_NODE.total);
}


//����skynet_node�� total
static void
context_dec() {
	ATOM_DEC(&G_NODE.total);
}


//��ȡ�߳�ȫ�ֱ���
uint32_t 
skynet_current_handle(void) {
	if (G_NODE.init) {
		void * handle = pthread_getspecific(G_NODE.handle_key);
		return (uint32_t)(uintptr_t)handle;
	} else {
		uint32_t v = (uint32_t)(-THREAD_MAIN);
		return v;
	}
}


//��idת����16���Ƶ�str
static void
id_to_hex(char * str, uint32_t id) {
	int i;
	static char hex[16] = { '0','1','2','3','4','5','6','7','8','9','A','B','C','D','E','F' };
	str[0] = ':';
	for (i=0;i<8;i++) {

		//ת�� 16���Ƶ�0xff ff ff ff 8λ
		str[i+1] = hex[(id >> ((7-i) * 4))&0xf];//����ȡ4λ������ߵ�4λ��ʼȡ
		
	}
	str[9] = '\0';
}

struct drop_t {
	uint32_t handle;
};


//����message
static void
drop_message(struct skynet_message *msg, void *ud) {

	struct drop_t *d = ud;

	skynet_free(msg->data);
	
	uint32_t source = d->handle;
	assert(source);
	
	// report error to the message source
	//������Ϣ
	skynet_send(NULL, source, msg->source, PTYPE_ERROR, 0, NULL, 0);
}

//�����µ�ctx
/*
1.����skynet_module���󣬵���createȡ���û�����
2.����sc,ע��handle,��������
3.����init��ʼ���û�����

֮���Ե�����һЩCALLINGCHECK�꣬��Ҫ��Ϊ�˼������Ƿ���ȷ����Ϊskynet����ʱ��ÿ��actorֻ�ᱻ
һ���̳߳��е��ȣ�Ҳ������Ϣ����ʱ���̵߳�

*/
struct skynet_context * 
skynet_context_new(const char * name, const char *param) {
	//����boostrapģ��ʱ�������� snlua  boostrap
	
	//�������ƴ�ȫ�ֵ�modules �в���ģ��(��̬��)�����û���ҵ����ú����л�����ظ�ģ�� ģ���Ӧһ��.so�⣬����̬��,���ʼ��
	//skynet_module�еĺ���ָ��
	//ģ���·�� optstring("cpath","./cservice/?.so");
	struct skynet_module * mod = skynet_module_query(name);

	if (mod == NULL)
		return NULL;

	//����ģ�鴴������
	//���m->create����ָ�벻Ϊ�������m->create,��create����Ҳ���ڶ�̬���ж����,instΪ���õķ���ֵ
	//���m->create����ָ��Ϊ�գ��򷵻� return (void *)(intptr_t)(~0);

	//ʵ�ʾ��ǵ��ö�̬���е�xxxx_create����
	
	void *inst = skynet_module_instance_create(mod);
	
	if (inst == NULL)
		return NULL;

	//�����ڴ�,һ��ģ��(��̬��ĳ���)����һ��skynet_context
	struct skynet_context * ctx = skynet_malloc(sizeof(*ctx));
	//#define CHECKCALLING_INIT(ctx) spinlock_init(&ctx->calling);
	CHECKCALLING_INIT(ctx)

	//skynet_context
	//��ʼ��ģ��ָ�룬����õ��Ķ�̬��ṹ��ĵ�ַ���ýṹ�����ж�̬���еĺ���ָ��
	ctx->mod = mod;

	//inst�ǵ���ģ���е�xxxx_create�����ķ���ֵ����һ���ṹ��ָ��
	ctx->instance = inst;

   //��ʼ��skynet_context�����ü���Ϊ2
	ctx->ref = 2;

	//���ûص�����ָ��Ϊ��
	ctx->cb = NULL;
	ctx->cb_ud = NULL;

	
	ctx->session_id = 0;
	
	ctx->logfile = NULL;

	//��ʾδ��ʼ��
	ctx->init = false;
	ctx->endless = false;

	ctx->cpu_cost = 0;
	ctx->cpu_start = 0;

	//��Ϣ�����е���Ϣ����
	ctx->message_count = 0;
	ctx->profile = G_NODE.profile;
	
	// Should set to 0 first to avoid skynet_handle_retireall get an uninitialized handle
	ctx->handle = 0;	


	//ע�ᣬ�õ�һ��Ψһ�ľ��
	//ʵ�ʾ����н� ������ctxָ�� ���浽һ��ȫ�ֵ� ָ�������У�������hash�洢�ģ�
	//ֻ�����������װ�ɽṹ��  handler_storage
	//handle���� ���ݴ��λ�ã��Լ�һЩ���Լ��������һ��ֵ���൱��һ�����
	ctx->handle = skynet_handle_register(ctx);


	//Ϊһ����Ϣ���� �����ڴ�
	struct message_queue * queue = ctx->queue = skynet_mq_create(ctx->handle);

	//�ڵ��������1
	// init function maybe use ctx->handle, so it must init at last
	context_inc();

	//#define CHECKCALLING_BEGIN(ctx) if (!(spinlock_trylock(&ctx->calling))) { assert(0); }
	CHECKCALLING_BEGIN(ctx)

	//����mod�ĳ�ʼ��
	//�ú����ڲ� return m->init(inst, ctx, parm);
	//��Ҫ�ǻ�����ctx�Ļص�����ָ��						boostrap


	//���ö�̬���xxxx_init����
	//���ʼ��skynet_context�Ļص�����
	int r = skynet_module_instance_init(mod, inst, ctx, param);
	
	CHECKCALLING_END(ctx)
		
	if (r == 0) {

		//�����ü�����һ,��Ϊ0ʱ��ɾ��skynet_context,�����٣�����NULL
		struct skynet_context * ret = skynet_context_release(ctx);
		if (ret) {
			ctx->init = true;
		}
		/*
			ctx�ĳ�ʼ�������ǿ��Է�����Ϣ��ȥ��(ͬʱҲ���Խ�����Ϣ)�����ڳ�ʼ���������֮ǰ
			���ܵ�����Ϣ�����뻺����mq�У����ܴ���������һ��С���ɽ��������⣬�����ڳ�ʼ��
			���̿�ʼǰ����װֻ��globalmq��(������mq�е�һ�����λ������)������������������Ϣ��
			�������mqѹ��globalmq,��ȻҲ���ᱻ�����߳�ȡ�����ȳ�ʼ�����̽�������ǿ�ư�mqѹ��
			globalmq(�����Ƿ�Ϊ��),��ʹʧ��ҲҪ�����������
		*/

		//��ʼ�����̽ṹ�����ctx��Ӧ��mqǿ��ѹ�� globalmq

		//���� һ����Ϣ���м��뵽 ȫ�ֵĶ�����Ϣ����(��Ϣ�����д����Ϣ����)�У�
		skynet_globalmq_push(queue);
		
		if (ret) {
			skynet_error(ret, "LAUNCH %s %s", name, param ? param : "");
		}
		return ret;
	} else {
		skynet_error(ctx, "FAILED launch %s", name);
		uint32_t handle = ctx->handle;


		//�����ü�����һ,��Ϊ0ʱ��ɾ��skynet_context
		skynet_context_release(ctx);

		//���ݱ��handleɾ��ָ��
		skynet_handle_retire(handle);

		struct drop_t d = { handle };
		//����queue
		skynet_mq_release(queue, drop_message, &d);
		return NULL;
	}
}

//����skynet_context����һ��sesssion id
int
skynet_context_newsession(struct skynet_context *ctx) {
	// session always be a positive number
	int session = ++ctx->session_id;
	if (session <= 0) {
		ctx->session_id = 1;
		return 1;
	}
	return session;
}


//skynet_context���ü�����1
void 
skynet_context_grab(struct skynet_context *ctx) {
	ATOM_INC(&ctx->ref);
}

//�ڵ��Ӧ�ķ������� 1
void
skynet_context_reserve(struct skynet_context *ctx) {
	skynet_context_grab(ctx);
	// don't count the context reserved, because skynet abort (the worker threads terminate) only when the total context is 0 .
	// the reserved context will be release at last.

	//����total
	context_dec();
}

/*
	�����������:
	handle �� ctx �İ󶨹�ϵ���� ctx ģ���ⲿ�����ģ���ȻҲ������ ctx ����ȷ���٣���

	�޷�ȷ���� handle ȷ�϶�Ӧ�� ctx ��Ч��ͬʱ��ctx ����Ѿ��������ˡ�
	���ԣ��������߳��ж� mq ��������ʱ����Ӧ�� handle ��Ч����ctx ���ܻ����ţ���һ�������̻߳����������ã���
	������� ctx �Ĺ����߳̿������������������һ�̣����䷢����Ϣ����� mq �Ѿ������ˡ�

	�� ctx ����ǰ���������� mq ����һ�������ǡ�Ȼ���� globalmq ȡ�� mq �������Ѿ��Ҳ��� handle ��Ӧ�� ctx ʱ��
	���ж��Ƿ��������ǡ����û�У��ٽ� mq �طŽ� globalmq ��ֱ����������Ч�������� mq ��
*/

//����skynet_context
static void 
delete_context(struct skynet_context *ctx) {
	//�ر���־�ļ����
	if (ctx->logfile) {
		fclose(ctx->logfile);
	}

	//���ö�Ӧ��̬���xxxx_release����
	skynet_module_instance_release(ctx->mod, ctx->instance);
	
	//���ñ��λ,��Ǹ�ѭ������Ҫɾ�������Ұ���ѹ�� global mq
	skynet_mq_mark_release(ctx->queue);
	
	CHECKCALLING_DESTROY(ctx)

	skynet_free(ctx);

	//����ڵ��Ӧ�ķ�����Ҳ �� 1
	context_dec();
}

//�����ü�����һ , ��Ϊ0ʱ��ɾ��skynet_context
struct skynet_context * 
skynet_context_release(struct skynet_context *ctx) {
	//���ü����� 1����Ϊ0ʱ��ɾ��skynet_context
	if (ATOM_DEC(&ctx->ref) == 0) {
		
		delete_context(ctx);
		return NULL;
	}
	return ctx;
}

//��handle��ʶ��skynet_context����Ϣ�����в���һ����Ϣ
//handle����һ����ţ����е�skynet_context��ָ�붼���浽һ��������,ͨ��handle���Ի�ö�Ӧ��ָ��
int
skynet_context_push(uint32_t handle, struct skynet_message *message) {

	//ͨ��handle��ȡskynet_context,skynet_context�����ü�����1
	
	struct skynet_context * ctx = skynet_handle_grab(handle);
	
	if (ctx == NULL) {
		return -1;
	}

	//��message���뵽cte��queue��
	skynet_mq_push(ctx->queue, message);

	//���ü�����һ,��Ϊ0ʱ��ɾ��skynet_context
	skynet_context_release(ctx);

	return 0;
}


//����ctx->endless = true;
void 
skynet_context_endless(uint32_t handle) {

	//����handle��Ż��skynet_cotextָ��
	struct skynet_context * ctx = skynet_handle_grab(handle);
	if (ctx == NULL) {
		return;
	}
	
	ctx->endless = true;

	skynet_context_release(ctx);
}


//�ж��Ƿ���Զ����Ϣ
int 
skynet_isremote(struct skynet_context * ctx, uint32_t handle, int * harbor) {

	//�ж��Ƿ���Զ����Ϣ
	int ret = skynet_harbor_message_isremote(handle);
	
	if (harbor) {
		//����harbor(ע:�߰�λ�����harbor)
		*harbor = (int)(handle >> HANDLE_REMOTE_SHIFT);
	}
	return ret;
}


//���skynet_context�еĻص�����ָ�벻�ǿ� ,���ô˺���,��Ϣ�ַ�
static void
dispatch_message(struct skynet_context *ctx, struct skynet_message *msg) {

	assert(ctx->init);
	CHECKCALLING_BEGIN(ctx)

	pthread_setspecific(G_NODE.handle_key, (void *)(uintptr_t)(ctx->handle));

	//type����������
	int type = msg->sz >> MESSAGE_TYPE_SHIFT;	//��8λ��Ϣ���
	size_t sz = msg->sz & MESSAGE_TYPE_MASK;	//��24λ��Ϣ��С
	
	//��ӡ��־
	if (ctx->logfile) {
		skynet_log_output(ctx->logfile, msg->source, type, msg->session, msg->data, sz);
	}

	
	++ctx->message_count;
	
	int reserve_msg;

	if (ctx->profile) {
		ctx->cpu_start = skynet_thread_time();
		
		//���ûص�����,���ݷ���ֵ�����Ƿ�Ҫ�ͷ���Ϣ
		reserve_msg = ctx->cb(ctx, ctx->cb_ud, type, msg->session, msg->source, msg->data, sz);

		uint64_t cost_time = skynet_thread_time() - ctx->cpu_start;
		ctx->cpu_cost += cost_time;
	} 
	else 
	{
		//���ûص�����,���ݷ���ֵ�����Ƿ�Ҫ�ͷ���Ϣ
		reserve_msg = ctx->cb(ctx, ctx->cb_ud, type, msg->session, msg->source, msg->data, sz);
	}
	if (!reserve_msg) {
		skynet_free(msg->data);
	}

	CHECKCALLING_END(ctx)
}

//��skynet_context����Ϣ�����е� ���е�msg����dispatch_message
//����ctx��ѭ����Ϣ�����е�������Ϣ,���� �ص�����
void 
skynet_context_dispatchall(struct skynet_context * ctx) {
	// for skynet_error
	struct skynet_message msg;
	struct message_queue *q = ctx->queue;
	
	while (!skynet_mq_pop(q,&msg)) {
		dispatch_message(ctx, &msg);
	}
}


//message_queue�ĵ���,�ڹ����߳��б�����
//�����������ǣ����ȴ����2�����У���������һ���ɵ��ȵ�2������
struct message_queue * 
skynet_context_message_dispatch(struct skynet_monitor *sm, struct message_queue *q, int weight) {
	//�ڹ����߳��У������q==NULL
	if (q == NULL) {
		//��ȫ�ֵĶ�����Ϣ���е�ͷ�� ����һ����Ϣ����
		q = skynet_globalmq_pop();
		if (q==NULL)
			return NULL;
	}

	//�õ���Ϣ���������ķ�����,

	//ʵ�ʾ��ǻ����Ϣ����������skynet_context�ı��
	uint32_t handle = skynet_mq_handle(q);

	//����handle��ȡskynet_conext
	struct skynet_context * ctx = skynet_handle_grab(handle);
	
	if (ctx == NULL) {
		struct drop_t d = { handle };
		skynet_mq_release(q, drop_message, &d);
		return skynet_globalmq_pop();
	}

	int i,n=1;
	struct skynet_message msg;

	for (i=0;i<n;i++) {

		//��2������Ϊ��ʱ��û�н���ѹ��1�����У������Ӵ���ʧ���𣬲��ǣ���������Ϊ�˼��ٿ�ת1������
		//�����������������ôʱ��ѹ�ص��أ���message_queue�У���һ��in_global����Ƿ���1��������
		//��2�����еĳ���(skynet_mq_pop)ʧ��ʱ��������λ0���ڶ����������ʱ(skynet_mq_push)
		//���ж������ǣ����Ϊ0,�ͻὫ�Լ�ѹ��1�����С�(skynet_mq_mark_realeasҲ���ж�)
		//�������2���������´����ʱ�� ѹ��

		//�Ӷ�����ȡ��һ����Ϣ,msgΪ��������
		if (skynet_mq_pop(q,&msg)) {
			
			skynet_context_release(ctx);
			return skynet_globalmq_pop();
			
		} else if (i==0 && weight >= 0) {

			//�޸���n�Ĵ�С,���޸���forѭ���Ĵ�����Ҳ����ÿ�ε��ȴ����������Ϣ�������ʱ�봫���weight
			//�йء�
			n = skynet_mq_length(q);

			//��ž��ǣ��ѹ����̷߳��飬ǰ����ÿ��8���������Ĺ�������顣�
			//A,E��ÿ�ε��ȴ���һ����Ϣ��B��ÿ�δ���n/2����C��ÿ�δ���n/4����
			//D��ÿ�δ���n/8������Ϊ�˾���ʹ�ö��
			n >>= weight;
		}

		//����һ�������жϡ����ص���ֵ��1024������Ҳֻ�ǽ������һ��log���Ѷ���
		int overload = skynet_mq_overload(q);
		if (overload) {
			skynet_error(ctx, "May overload, message queue length = %d", overload);
		}

		//������һ��monitor,�����������������Ϣ�����Ƿ�������ѭ��������Ҳֻ�����һ��lig����һ��
		//�������Ƿ���һ��ר�ŵļ���߳������ģ��ж���ѭ����ʱ����5��
		/*
		void 
		skynet_monitor_trigger(struct skynet_monitor *sm, uint32_t source, uint32_t destination) {
			sm->source = source;
			sm->destination = destination;
			ATOM_INC(&sm->version);
		}

		*/

		//sm�Ǵ��룬���޸�,��monitrr�߳��л���sm��ֵ
		skynet_monitor_trigger(sm, msg.source , handle);

		//���skynet_context�Ļص������ĺ���ָ��Ϊ��
		if (ctx->cb == NULL) 
		{
			skynet_free(msg.data);
		}
		else 
		{
			/*
					struct skynet_message {
						uint32_t source;	//��ϢԴ������skynet�ı��
						int session;		//�����������ĵı�ʶ
						void * data;		//��Ϣָ��
						size_t sz;			//��Ϣ����,��Ϣ���������Ͷ����ڸ߰�λ
						};

			*/
		
			//��Ϣ�ַ�	ʵ�ʾ��ǵ���skynet_context�еĻص�����������Ϣ
			dispatch_message(ctx, &msg);
		}

		
		//������һ��monitor,�����������������Ϣ�����Ƿ�������ѭ��������Ҳֻ�����һ��lig����һ��
		//�������Ƿ���һ��ר�ŵļ���߳������ģ��ж���ѭ����ʱ����5��
		skynet_monitor_trigger(sm, 0,0);
	}

	assert(q == ctx->queue);

	//��ȫ�ֶ�����Ϣ�������Ƴ�һ����Ϣ���У���ͷ���Ƴ�
	struct message_queue *nq = skynet_globalmq_pop();
	if (nq) {
		// If global mq is not empty , push q back, and return next queue (nq)
		// Else (global mq is empty or block, don't push q back, and return q again (for next dispatch)

		//���������Ϣ����Ϣ����q���¼��뵽ȫ�ֵ���Ϣ������
		skynet_globalmq_push(q);

		q = nq;
	} 
	skynet_context_release(ctx);

	return q;
}

//��addrָ������ݸ��Ƶ�name��
static void
copy_name(char name[GLOBALNAME_LENGTH], const char * addr) {
	int i;
	for (i=0;i<GLOBALNAME_LENGTH && addr[i];i++) {
		name[i] = addr[i];
	}
	for (;i<GLOBALNAME_LENGTH;i++) {
		name[i] = '\0';
	}
}


//�����ַ�������handle��ţ�name������handle��Ӧ�����֣�Ҳ����ʹ :xxxxx
uint32_t 
skynet_queryname(struct skynet_context * context, const char * name) {
	switch(name[0]) {
	case ':':
		//strtoul  ���ַ�������ת��Ϊunsigned long����
		return strtoul(name+1,NULL,16);
	case '.':
		
		//�������Ʋ���handle
		return skynet_handle_findname(name + 1);
	}
	skynet_error(context, "Don't support query global name %s",name);
	return 0;
}

//��handle��Ӧ��skynet_context��handle_storage��ɾ��
static void
handle_exit(struct skynet_context * context, uint32_t handle) {
	if (handle == 0) {
		handle = context->handle;
		skynet_error(context, "KILL self");
	} else {
		skynet_error(context, "KILL :%0x", handle);
	}
	if (G_NODE.monitor_exit) {
		skynet_send(context,  handle, G_NODE.monitor_exit, PTYPE_CLIENT, 0, NULL, 0);
	}
	
	//�ջ�handle,������handle��Ӧ��skynet_context�������е�ָ��
	skynet_handle_retire(handle);
}

// skynet command

//����������ص�������Ӧ�Ľṹ��
struct command_func {
	const char *name;
	const char * (*func)(struct skynet_context * context, const char * param);
};

//"timeout"�����Ӧ�Ļص�����
//ע���ǵ��� skynet_timeout(),���붨ʱ��
static const char *
cmd_timeout(struct skynet_context * context, const char * param) {
	char * session_ptr = NULL;

	//long int strtol (const char* str, char** endptr, int base);
	//base�ǽ���  param����Ҫת�����ַ���  session_ptr==NULLʱ���ò�����Ч,

	//ͨ���ַ����������
	//��paramתΪ10������
	int ti = strtol(param, &session_ptr, 10);

		//���һ��session   �Ựid
	int session = skynet_context_newsession(context);
	
	//���붨ʱ����time�ĵ�λ��0.01�룬��time=300,��ʾ3��
	skynet_timeout(context->handle, ti, session);

	sprintf(context->result, "%d", session);
	return context->result;
}

//"reg"��Ӧ�ص����� ,Ϊcontext����
static const char *
cmd_reg(struct skynet_context * context, const char * param) {

	if (param == NULL || param[0] == '\0') 
	{
		sprintf(context->result, ":%x", context->handle);
		return context->result;
	}
		//�����ǣ���ʽ��        .����
	else if (param[0] == '.') 
	{
		//ʵ�ʾ��ǽ�handle,name���뵽handle_storage��name������
		//Ϊskynet_contextͨ��һ������
		return skynet_handle_namehandle(context->handle, param + 1);
	}
	else 
	{
		skynet_error(context, "Can't register global name %s in C", param);
		return NULL;
	}
}

//"query"��Ӧ�Ļص�����
//�������ֲ���context��Ӧ��handle���
static const char *
cmd_query(struct skynet_context * context, const char * param) {
	//���ֵĸ�ʽ  .����
	if (param[0] == '.') {
		
		uint32_t handle = skynet_handle_findname(param+1);

		//��handleд����context��result�� 
		if (handle) {
			sprintf(context->result, ":%x", handle);
			return context->result;
		}
	}
	return NULL;
}

//"name"��Ӧ�Ļص�����
//Ϊhandle��Ӧ��skynet_context����,ֻ��handle,name����param��
static const char *
cmd_name(struct skynet_context * context, const char * param) {
	int size = strlen(param);
	char name[size+1];
	char handle[size+1];
	
	sscanf(param,"%s %s",name,handle);

	if (handle[0] != ':') {
		return NULL;
	}
	//�õ�handle
	uint32_t handle_id = strtoul(handle+1, NULL, 16);
	if (handle_id == 0) {
		return NULL;
	}
	
	if (name[0] == '.') {
		//ʵ�ʾ��ǽ�handle,name���뵽handle_storage��name������
		//Ϊskynet_contextͨ��һ������
		return skynet_handle_namehandle(handle_id, name + 1);
	} else {
		skynet_error(context, "Can't set global name %s in C", name);
	}
	return NULL;
}


//"exit"��Ӧ�Ļص�����
//���� handle_exit()
//��context�˳�
static const char *
cmd_exit(struct skynet_context * context, const char * param) {
	handle_exit(context, 0);
	return NULL;
}

//���ݲ������handle
static uint32_t
tohandle(struct skynet_context * context, const char * param) {
	uint32_t handle = 0;
	//����� :xxxxx
	if (param[0] == ':') {
		
		handle = strtoul(param+1, NULL, 16);
	} else if (param[0] == '.') {
		//����� .����
		handle = skynet_handle_findname(param+1);
	} else {
		skynet_error(context, "Can't convert %s to handle",param);
	}

	return handle;
}


//"kill"��Ӧ�Ļص�����
//��param��Ӧ��handle�˳�
static const char *
cmd_kill(struct skynet_context * context, const char * param) {
	uint32_t handle = tohandle(context, param);
	if (handle) {
		handle_exit(context, handle);
	}
	return NULL;
}

//"launch"��Ӧ�Ļص�����,����ģ��(��̬��),
//����һ��skynet_context
static const char *
cmd_launch(struct skynet_context * context, const char * param) {
	size_t sz = strlen(param);
	char tmp[sz+1];
	strcpy(tmp,param);
	char * args = tmp;

	// aaa bbb ccc

	//�ָ��ַ���
	char * mod = strsep(&args, " \t\r\n");

	//�ָ��ַ���
	args = strsep(&args, "\r\n");
													//ģ���� ����
	struct skynet_context * inst = skynet_context_new(mod,args);
	
	if (inst == NULL) {
		return NULL;
	} else {
	//��handleд�뵽result��
		id_to_hex(context->result, inst->handle);
		return context->result;
	}
}

//"getenv"��Ӧ�Ļص�����
//��ȡluaȫ�ֱ���   
static const char *
cmd_getenv(struct skynet_context * context, const char * param) {
	return skynet_getenv(param);
}

//"setenv"��Ӧ�Ļص�����
//����luaȫ�ֱ���   ������ ֵ
static const char *
cmd_setenv(struct skynet_context * context, const char * param) {
	size_t sz = strlen(param);
	//������Ǳ����� 
	char key[sz+1];
	int i;


	//��ñ����� 
	for (i=0;param[i] != ' ' && param[i];i++) {
		key[i] = param[i];
	}
	if (param[i] == '\0')
		return NULL;

	key[i] = '\0';
	param += i+1;

	//����
	skynet_setenv(key,param);
	return NULL;
}

//"starttime"��Ӧ�Ļص����� 
//����skynet_starttime() ��ȡ��������ʱ��
static const char *
cmd_starttime(struct skynet_context * context, const char * param) {
	//���������ó����ʱ��
	uint32_t sec = skynet_starttime();

	//��ʱ��д�뵽result��
	sprintf(context->result,"%u",sec);
	return context->result;
}

//"abort"��Ӧ�Ļص�����

//skynet_handle_retireall()
//�������е�handler
//����handle_storage�е�ָ�������е�����skynet_contextָ��

static const char *
cmd_abort(struct skynet_context * context, const char * param) {
	skynet_handle_retireall();
	return NULL;
}

//"monitor"��Ӧ�Ļص�����
static const char *
cmd_monitor(struct skynet_context * context, const char * param) {
	uint32_t handle=0;
	if (param == NULL || param[0] == '\0') {
		if (G_NODE.monitor_exit) {
			// return current monitor serivce
			sprintf(context->result, ":%x", G_NODE.monitor_exit);
			return context->result;
		}
		return NULL;
	} else {
		handle = tohandle(context, param);
	}
	G_NODE.monitor_exit = handle;
	return NULL;
}

//"stat"��Ӧ�Ļص�����
//��ȡskynet_context�е�һЩ״̬��Ϣ
static const char *
cmd_stat(struct skynet_context * context, const char * param) {

	if (strcmp(param, "mqlen") == 0) {
		
		//��ȡskynet_context�е���Ϣ�����е���Ϣ����
		int len = skynet_mq_length(context->queue);
		//�����д�뵽result��
		sprintf(context->result, "%d", len);

	} else if (strcmp(param, "endless") == 0) {
		if (context->endless) {
			strcpy(context->result, "1");
			context->endless = false;
		} else {
			strcpy(context->result, "0");
		}
	} else if (strcmp(param, "cpu") == 0) {
		double t = (double)context->cpu_cost / 1000000.0;	// microsec
		sprintf(context->result, "%lf", t);
	} else if (strcmp(param, "time") == 0) {
		if (context->profile) {
			uint64_t ti = skynet_thread_time() - context->cpu_start;
			double t = (double)ti / 1000000.0;	// microsec
			sprintf(context->result, "%lf", t);
		} else {
			strcpy(context->result, "0");
		}
	} else if (strcmp(param, "message") == 0) {
		sprintf(context->result, "%d", context->message_count);
	} else {
		context->result[0] = '\0';
	}
	return context->result;
}

//"logon"��Ӧ�Ļص�����
//�����Ǵ�param��ָ����handle��Ӧ��skynet_context����־�ļ�
static const char *
cmd_logon(struct skynet_context * context, const char * param) {

	//ͨ��param���handle
	uint32_t handle = tohandle(context, param);

	if (handle == 0)
		return NULL;

	//���ݱ��handle��ö�Ӧ��skynet_contextָ��
	struct skynet_context * ctx = skynet_handle_grab(handle);
	if (ctx == NULL)
		return NULL;
	
	FILE *f = NULL;
	FILE * lastf = ctx->logfile;
	if (lastf == NULL) {
		//����־�ļ�
		f = skynet_log_open(context, handle);
		if (f) {
			if (!ATOM_CAS_POINTER(&ctx->logfile, NULL, f)) {
				// logfile opens in other thread, close this one.
				fclose(f);
			}
		}
	}
	skynet_context_release(ctx);
	return NULL;
}

//"logoff"��Ӧ�Ļص�����
//�����ǹر�param�е�handle��Ӧ�� skynet_context��Ӧ����־�ļ�

static const char *
cmd_logoff(struct skynet_context * context, const char * param) {

	//ͨ��param���handle
	uint32_t handle = tohandle(context, param);
	if (handle == 0)
		return NULL;

	//���handle��Ӧ��skynet_context
	struct skynet_context * ctx = skynet_handle_grab(handle);
	if (ctx == NULL)
		return NULL;
	FILE * f = ctx->logfile;
	if (f) {
		// logfile may close in other thread
		if (ATOM_CAS_POINTER(&ctx->logfile, f, NULL)) {
			skynet_log_close(context, f, handle);
		}
	}
	skynet_context_release(ctx);
	return NULL;
}
//"SIGNAL" �����Ӧ�Ļص�����
//�����ǵ��� param��handle��Ӧ�� skynet_context��Ӧ�Ķ�̬���е�xxxx_signal()����
static const char *
cmd_signal(struct skynet_context * context, const char * param) {

	//���handle
	uint32_t handle = tohandle(context, param);
	
	if (handle == 0)
		return NULL;
	//���ݱ�Ż�ö�Ӧ��skynet_context
	struct skynet_context * ctx = skynet_handle_grab(handle);
	
	if (ctx == NULL)
		return NULL;
	
	param = strchr(param, ' ');
	int sig = 0;

	if (param) {
		sig = strtol(param, NULL, 0);
	}
	// NOTICE: the signal function should be thread safe.

	//����skynet_context��Ӧ�Ķ�̬���е�xxxx_signal()����
	skynet_module_instance_signal(ctx->mod, ctx->instance, sig);

	skynet_context_release(ctx);
	return NULL;
}

//���������뺯��ָ���Ӧ�Ľṹ������
static struct command_func cmd_funcs[] = {
	{ "TIMEOUT", cmd_timeout },
	{ "REG", cmd_reg },
	{ "QUERY", cmd_query },
	{ "NAME", cmd_name },
	{ "EXIT", cmd_exit },
	{ "KILL", cmd_kill },
	{ "LAUNCH", cmd_launch },
	{ "GETENV", cmd_getenv },
	{ "SETENV", cmd_setenv },
	{ "STARTTIME", cmd_starttime },
	{ "ABORT", cmd_abort },
	{ "MONITOR", cmd_monitor },
	{ "STAT", cmd_stat },
	{ "LOGON", cmd_logon },
	{ "LOGOFF", cmd_logoff },
	{ "SIGNAL", cmd_signal },
	{ NULL, NULL },
};

//�����������Ʋ��Ҷ�Ӧ�Ļص����������ҵ���
const char * 
skynet_command(struct skynet_context * context, const char * cmd , const char * param) {
	struct command_func * method = &cmd_funcs[0];
	while(method->name) {

		//����ҵ� ��ִ�лص�����
		if (strcmp(cmd, method->name) == 0) {
			return method->func(context, param);
		}
		++method;
	}

	return NULL;
}

//��������
//����type������������
/*
1.(type & PTYPE_TAG_DONTCOPY) == 0
�Ὣdata����һ������ʵ�ʷ��ͣ����������ԭ����data��Ҫ�ɵ����߸����ͷ�
2.(type & PTYPE_TAG_ALLOCSESSION)>0
	���sesson����������һ��session

�������type��ϲ���sz�ĸ�8λ
*/
static void
_filter_args(struct skynet_context * context, int type, int *session, void ** data, size_t * sz) {

	//�Ƿ�                        0x10000
	int needcopy = !(type & PTYPE_TAG_DONTCOPY);
								//0x20000
	int allocsession = type & PTYPE_TAG_ALLOCSESSION;

	type &= 0xff;

	//���sesson����������һ��session
	if (allocsession) {
		assert(*session == 0);
		//�õ�һ���µ�session id,���ǽ�ԭ����context��Ӧ��session+1 
		*session = skynet_context_newsession(context);
	}

	//�Ὣdata���ݸ���  ���������ԭ����data��Ҫ�ɵ����߸����ͷ�
	if (needcopy && *data) {
		char * msg = skynet_malloc(*sz+1);
		memcpy(msg, *data, *sz);
		msg[*sz] = '\0';
		*data = msg;
	}

	//�������type��ϲ���sz�ĸ�8λ   24
	*sz |= (size_t)type << MESSAGE_TYPE_SHIFT;
}


/*
 * ��handleΪdestination�ķ�������Ϣ(ע��handleΪdestination�ķ���һ���Ǳ��ص�)
 * type�к��� PTYPE_TAG_ALLOCSESSION ����session������0
 * type�к��� PTYPE_TAG_DONTCOPY ������Ҫ��������
 */

//������Ϣ
int
skynet_send(struct skynet_context * context, uint32_t source, uint32_t destination , int type, int session, void * data, size_t sz) {

	//����Ϣ�������� MESSAGE_TYPE_MASK���� SIZE_MAX >> 8,���ֻ��Ϊ2^24,16MB
	if ((sz & MESSAGE_TYPE_MASK) != sz) {
		skynet_error(context, "The message to %x is too large", destination);
		if (type & PTYPE_TAG_DONTCOPY) {
			skynet_free(data);
		}
		return -1;
	}

	//��������
	//����type������������
	/*
		1.(type & PTYPE_TAG_DONTCOPY) == 0
			�Ὣdata����һ������ʵ�ʷ��ͣ����������ԭ����data��Ҫ�ɵ����߸����ͷ�
		2.(type & PTYPE_TAG_ALLOCSESSION)>0
		���sesson����������һ��session
		
	*/
	_filter_args(context, type, &session, (void **)&data, &sz);

	if (source == 0) {
		source = context->handle;
	}

	if (destination == 0) {
		return session;
	}

	//Ͷ�ݵ������ߵ�����,���ݽ����߾���ж��Ƿ�ΪԶ�̽ڵ㣬����Ǿ���harbo����(���õļ�Ⱥ������
	//�����Ѿ����Ƽ�ʹ��),�ɹ�����session,ʧ�ܷ���-1,�����ͷ�data

	// �����Ϣʱ����Զ�̵�
	if (skynet_harbor_message_isremote(destination)) {
		struct remote_message * rmsg = skynet_malloc(sizeof(*rmsg));
		rmsg->destination.handle = destination;
		rmsg->message = data;
		rmsg->sz = sz;
		
		skynet_harbor_send(rmsg, source, session);
	}
	// ������Ϣ ֱ��ѹ����Ϣ����
	else
	{
		struct skynet_message smsg;
		smsg.source = source;
		smsg.session = session;
		smsg.data = data;
		smsg.sz = sz;

		//destinationĿ�ĵص�skynet_context��Ӧ�ı��
		//���ǽ���Ϣ���͵�destination��Ӧ����Ϣ������
		if (skynet_context_push(destination, &smsg)) {
			skynet_free(data);
			return -1;
		}
	}
	return session;
}

//���������ҵ�Ŀ�ĵ� skynet_context��������Ϣ
int
skynet_sendname(struct skynet_context * context, uint32_t source, const char * addr , int type, int session, void * data, size_t sz) {
	if (source == 0) {
		source = context->handle;
	}
	uint32_t des = 0;

	//�����ַ�� :xxxxx
	if (addr[0] == ':') 
	{
		//xxxx����handle
		des = strtoul(addr+1, NULL, 16);
	}
	//�����ַ�� .����
	else if (addr[0] == '.') 
	{
	//���������ҵ���Ӧ��skynet_context�ı��
		des = skynet_handle_findname(addr + 1);
		if (des == 0) 
		{
			if (type & PTYPE_TAG_DONTCOPY) {
				skynet_free(data);
			}
			return -1;
		}
	} 
	else
	{
		_filter_args(context, type, &session, (void **)&data, &sz);

	//Զ����Ϣ
		struct remote_message * rmsg = skynet_malloc(sizeof(*rmsg));
		copy_name(rmsg->destination.name, addr);
		rmsg->destination.handle = 0;
		rmsg->message = data;
		rmsg->sz = sz;

		skynet_harbor_send(rmsg, source, session);
		return session;
	}
	
	return skynet_send(context, source, des, type, session, data, sz);
}

//��ȡskynet_context��Ӧ�ı��handle
uint32_t 
skynet_context_handle(struct skynet_context *ctx) {
	return ctx->handle;
}

//��ʼ��struct skynet_context�Ļص�����ָ�룬�ص���������
void 
skynet_callback(struct skynet_context * context, void *ud, skynet_cb cb) {
	context->cb = cb;
	context->cb_ud = ud;
}


// ��ctx��������Ϣ(ע��ctx����һ���Ǳ��ص�)
//ʵ�ʾ��ǽ���Ϣ���뵽skynet_context����Ϣ������
void
skynet_context_send(struct skynet_context * ctx, void * msg, size_t sz, uint32_t source, int type, int session) {
	struct skynet_message smsg;
	smsg.source = source;
	smsg.session = session;
	smsg.data = msg;
	smsg.sz = sz | (size_t)type << MESSAGE_TYPE_SHIFT;

	// ѹ����Ϣ����
	skynet_mq_push(ctx->queue, &smsg);
}


//��skynet_mian.c��main�����е���
void 
skynet_globalinit(void) {

	G_NODE.total = 0;
	G_NODE.monitor_exit = 0;
	G_NODE.init = 1;

	//�����߳�ȫ�ֱ���
	if (pthread_key_create(&G_NODE.handle_key, NULL)) {
		fprintf(stderr, "pthread_key_create failed");
		exit(1);
	}
	// set mainthread's key  	��#define THREAD_MAIN 1
	skynet_initthread(THREAD_MAIN); 
}

//�����߳�ȫ�ֱ���
void 
skynet_globalexit(void) {
	pthread_key_delete(G_NODE.handle_key);
}

//��ʼ���߳�,��ʼ���߳�ȫ�ֱ���
void
skynet_initthread(int m) {
	uintptr_t v = (uint32_t)(-m);
	pthread_setspecific(G_NODE.handle_key, (void *)v);
}

void
skynet_profile_enable(int enable) {
	G_NODE.profile = (bool)enable;
}

