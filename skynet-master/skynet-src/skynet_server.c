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


//skynet Ö÷Òª¹¦ÄÜ ¼ÓÔØ·şÎñºÍÍ¨Öª·şÎñ
/*
*	Ò»¸öÄ£¿é(.so)¼ÓÔØµ½skynet¿ò¼ÜÖĞ£¬´´½¨³öÀ´µÄÒ»¸öÊµÀı¾ÍÊÇÒ»¸ö·şÎñ
*	ÎªÃ¿¸ö·şÎñ·ÖÅäÒ»¸öskynet_context½á¹¹
*/

//Ã¿Ò»¸ö·şÎñ¶ÔÓ¦µÄsy==skynet_ctx½á¹¹

//Ö÷Òª³ĞÔØ¿ò¼ÜµÄµ÷¶ÈÂß¼­ ¼ò³Æ  sc

struct skynet_context {

	//ÆõÔ¼º¯Êıcreate´´½¨
	//µ÷ÓÃ¶¯Ì¬¿âÖĞµÄxxxxx_create()º¯ÊıµÄ·µ»ØÖµ  ¼´modÖ¸ÏòµÄ½á¹¹ÌåÖĞµÄ,×Ô¶¨ÒåµÄÓëÄ£¿éÏà¹ØµÄ½á¹¹Ìå
	void * instance;		//Ä£¿éxxx_createº¯Êı·µ»ØµÄÊµÀı ¶ÔÓ¦Ä£¿éµÄ¾ä±ú

	//Æ¤ÄÒ¶ÔÏó
	struct skynet_module * mod;		//Ä£¿é,¾ÍÊÇ ·â×°¶¯Ì¬¿âµÄ½á¹¹Ìå 


	//»Øµ÷º¯ÊıµÄÓÃ»§Êı¾İ
	void * cb_ud;		//´«µİ¸ø»Øµ÷º¯ÊıµÄ²ÎÊı£¬Ò»°ãÊÇxxx_createº¯Êı·µ»ØµÄÊµÀı

	//´¦ÀíÏûÏ¢µÄ»Øµ÷º¯Êı£¬ÓÉÆ¤ÄÒÂß¼­Àï×¢²á
	//typedef int (*skynet_cb)(struct skynet_context * context, void *ud, int type, int session, uint32_t source , const void * msg, size_t sz);

	skynet_cb cb;		//»Øµ÷º¯Êı

	//actorµÄĞÅÏä£¬´æ·ÅÊÜµ½µÄÏûÏ¢
	struct message_queue *queue;	//Ò»¼¶ÏûÏ¢¶ÓÁĞ

	FILE * logfile;		//ÈÕÖ¾ÎÄ¼ş¾ä±ú

	//¼ÇÂ¼µ÷ÓÃ»Øµ÷º¯Êı´¦ÀíÏûÏ¢ËùÓÃÊ±¼ä
	uint64_t cpu_cost;	// in microsec
	uint64_t cpu_start;	// in microsec

	//handlerµÄ16½øÖÆ×Ö·û£¬±ãÓÚ´«µİ
	char result[32];	//±£´æÃüÁîÖ´ĞĞ·µ»Ø½á¹û

	//handleÊÇÒ»¸öuint32_tµÄÕûÊı£¬¸ß°ËÎ»±íÊ¾Ô¶³Ì½Úµã(ÕâÊÇ¿ò¼Ü×Ô´øµÄ¼¯ÈºÉèÊ©£¬ºóÃæµÄ·ÖÎö¶¼»áÎŞÊÓ¸Ã²¿·Ö
	
	uint32_t handle;	//·şÎñ¾ä±ú,ÊµÖÊ¾ÍÊÇ¸Ã½á¹¹ÌåÖ¸Õë´æ·ÅÔÚÒ»¸öÈ«¾ÖµÄÖ¸ÕëÊı×éÖĞµÄÒ»¸ö±àºÅ

	//ÉÏÒ»´Î·ÖÅäµÄsession,ÓÃÓÚ·ÖÅä²»ÖØ¸´µÄsession
	int session_id;		//»á»°id

	//ÒıÓÃ¼ÆÊı
	int ref;			//Ïß³Ì°²È«µÄÒıÓÃ¼ÆÊı£¬±£Ö¤ÔÚÊ¹ÓÃµÄÊ±ºò£¬Ã»ÓĞ±»ÆäËûÏß³ÌÊÍ·Å
	int message_count;	//¸öÊı

	bool init;	//ÊÇ·ñ³õÊ¼»¯

	//ÊÇ·ñÔÚ´¦ÀíÏûÏ¢Ê±ËÀÑ­»·
	bool endless;	//ÊÇ·ñÎŞÏŞÑ­»·

	bool profile;

	CHECKCALLING_DECL
};


//skynet µÄ½Úµã½á¹¹
struct skynet_node {

	int total;	//Ò»¸öskynet_nodeµÄ·şÎñÊı Ò»¸önodeµÄ·şÎñÊıÁ¿

	int init;

	uint32_t monitor_exit;

	pthread_key_t handle_key;
	bool profile;	// default is off
};

//È«¾Ö±äÁ¿
static struct skynet_node G_NODE;


//»ñÈ¡¸Ãskynet_nodeµÄ total
int 
skynet_context_total() {
	return G_NODE.total;
}


//Ôö¼Óskynet_nodeµÄ total
static void
context_inc() {
	ATOM_INC(&G_NODE.total);
}


//¼õÉÙskynet_nodeµÄ total
static void
context_dec() {
	ATOM_DEC(&G_NODE.total);
}


//»ñÈ¡Ïß³ÌÈ«¾Ö±äÁ¿
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


//½«id×ª»»³É16½øÖÆµÄstr
static void
id_to_hex(char * str, uint32_t id) {
	int i;
	static char hex[16] = { '0','1','2','3','4','5','6','7','8','9','A','B','C','D','E','F' };
	str[0] = ':';
	for (i=0;i<8;i++) {

		//×ª»» 16½øÖÆµÄ0xff ff ff ff 8Î»
		str[i+1] = hex[(id >> ((7-i) * 4))&0xf];//ÒÀ´ÎÈ¡4Î»£¬´Ó×î¸ßµÄ4Î»¿ªÊ¼È¡
		
	}
	str[9] = '\0';
}

struct drop_t {
	uint32_t handle;
};


//Ïú»Ùmessage
static void
drop_message(struct skynet_message *msg, void *ud) {

	struct drop_t *d = ud;

	skynet_free(msg->data);
	
	uint32_t source = d->handle;
	assert(source);
	
	// report error to the message source
	//·¢ËÍÏûÏ¢
	skynet_send(NULL, source, msg->source, PTYPE_ERROR, 0, NULL, 0);
}

//´´½¨ĞÂµÄctx
/*
1.¼ÓÔØskynet_module¶ÔÏó£¬µ÷ÓÃcreateÈ¡µÃÓÃ»§¶ÔÏó
2.·ÖÅäsc,×¢²áhandle,·ÖÅäĞÅÏä
3.µ÷ÓÃinit³õÊ¼»¯ÓÃ»§¶ÔÏó

Ö®ËùÒÔµ½´¦ÓĞÒ»Ğ©CALLINGCHECKºê£¬Ö÷ÒªÊÇÎªÁË¼ì²âµ÷¶ÈÊÇ·ñÕıÈ·£¬ÒòÎªskynetµ÷¶ÈÊ±£¬Ã¿¸öactorÖ»»á±»
Ò»¸öÏß³Ì³ÖÓĞµ÷¶È£¬Ò²¾ÍÊÇÏûÏ¢´¦ÀíÊ±µ¥Ïß³ÌµÄ

*/
struct skynet_context * 
skynet_context_new(const char * name, const char *param) {
	//¼ÓÔØboostrapÄ£¿éÊ±£¬²ÎÊıÊÇ snlua  boostrap
	
	//¸ù¾İÃû³Æ´ÓÈ«¾ÖµÄmodules ÖĞ²éÕÒÄ£¿é(¶¯Ì¬¿â)£¬Èç¹ûÃ»ÓĞÕÒµ½£¬¸Ãº¯ÊıÖĞ»áÔò¼ÓÔØ¸ÃÄ£¿é Ä£¿é¶ÔÓ¦Ò»¸ö.so¿â£¬¼´¶¯Ì¬¿â,»á³õÊ¼»¯
	//skynet_moduleÖĞµÄº¯ÊıÖ¸Õë
	//Ä£¿éµÄÂ·¾¶ optstring("cpath","./cservice/?.so");
	struct skynet_module * mod = skynet_module_query(name);

	if (mod == NULL)
		return NULL;

	//µ÷ÓÃÄ£¿é´´½¨º¯Êı
	//Èç¹ûm->createº¯ÊıÖ¸Õë²»Îª¿ÕÔòµ÷ÓÃm->create,¸Ãcreateº¯ÊıÒ²ÊÇÔÚ¶¯Ì¬¿âÖĞ¶¨ÒåµÄ,instÎªµ÷ÓÃµÄ·µ»ØÖµ
	//Èç¹ûm->createº¯ÊıÖ¸ÕëÎª¿Õ£¬Ôò·µ»Ø return (void *)(intptr_t)(~0);

	//ÊµÖÊ¾ÍÊÇµ÷ÓÃ¶¯Ì¬¿âÖĞµÄxxxx_createº¯Êı
	
	void *inst = skynet_module_instance_create(mod);
	
	if (inst == NULL)
		return NULL;

	//·ÖÅäÄÚ´æ,Ò»¸öÄ£¿é(¶¯Ì¬¿âµÄ³éÏó)ÊôÓÚÒ»¸öskynet_context
	struct skynet_context * ctx = skynet_malloc(sizeof(*ctx));
	//#define CHECKCALLING_INIT(ctx) spinlock_init(&ctx->calling);
	CHECKCALLING_INIT(ctx)

	//skynet_context
	//³õÊ¼»¯Ä£¿éÖ¸Õë£¬¼´»ñµÃµ½µÄ¶¯Ì¬¿â½á¹¹ÌåµÄµØÖ·£¬¸Ã½á¹¹ÌåÖĞÓĞ¶¯Ì¬¿âÖĞµÄº¯ÊıÖ¸Õë
	ctx->mod = mod;

	//instÊÇµ÷ÓÃÄ£¿éÖĞµÄxxxx_createº¯ÊıµÄ·µ»ØÖµ£¬ÊÇÒ»¸ö½á¹¹ÌåÖ¸Õë
	ctx->instance = inst;

   //³õÊ¼»¯skynet_contextµÄÒıÓÃ¼ÆÊıÎª2
	ctx->ref = 2;

	//ÉèÖÃ»Øµ÷º¯ÊıÖ¸ÕëÎª¿Õ
	ctx->cb = NULL;
	ctx->cb_ud = NULL;

	
	ctx->session_id = 0;
	
	ctx->logfile = NULL;

	//±íÊ¾Î´³õÊ¼»¯
	ctx->init = false;
	ctx->endless = false;

	ctx->cpu_cost = 0;
	ctx->cpu_start = 0;

	//ÏûÏ¢¶ÓÁĞÖĞµÄÏûÏ¢ÊıÁ¿
	ctx->message_count = 0;
	ctx->profile = G_NODE.profile;
	
	// Should set to 0 first to avoid skynet_handle_retireall get an uninitialized handle
	ctx->handle = 0;	


	//×¢²á£¬µÃµ½Ò»¸öÎ¨Ò»µÄ¾ä±ú
	//ÊµÖÊ¾ÍÊÇÓĞ½« ´´½¨µÄctxÖ¸Õë ±£´æµ½Ò»¸öÈ«¾ÖµÄ Ö¸ÕëÊı×éÖĞ£¬ÊÇÀûÓÃhash´æ´¢µÄ£¬
	//Ö»²»¹ı¸ÃÊı×é·â×°³É½á¹¹Ìå  handler_storage
	//handle¾ÍÊÇ ¸ù¾İ´æ·ÅÎ»ÖÃ£¬ÒÔ¼°Ò»Ğ©ÌØĞÔ¼ÆËã³öÀ´µÄÒ»¸öÖµ£¬Ïàµ±ÓÚÒ»¸ö±êºÅ
	ctx->handle = skynet_handle_register(ctx);


	//ÎªÒ»¼¶ÏûÏ¢¶ÓÁĞ ·ÖÅäÄÚ´æ
	struct message_queue * queue = ctx->queue = skynet_mq_create(ctx->handle);

	//½Úµã·şÎñÊı¼Ó1
	// init function maybe use ctx->handle, so it must init at last
	context_inc();

	//#define CHECKCALLING_BEGIN(ctx) if (!(spinlock_trylock(&ctx->calling))) { assert(0); }
	CHECKCALLING_BEGIN(ctx)

	//µ÷ÓÃmodµÄ³õÊ¼»¯
	//¸Ãº¯ÊıÄÚ²¿ return m->init(inst, ctx, parm);
	//Ö÷ÒªÊÇ»áÉèÖÃctxµÄ»Øµ÷º¯ÊıÖ¸Õë						boostrap


	//µ÷ÓÃ¶¯Ì¬¿âµÄxxxx_initº¯Êı
	//»á³õÊ¼»¯skynet_contextµÄ»Øµ÷º¯Êı
	int r = skynet_module_instance_init(mod, inst, ctx, param);
	
	CHECKCALLING_END(ctx)
		
	if (r == 0) {

		//½«ÒıÓÃ¼ÆÊı¼õÒ»,¼õÎª0Ê±ÔòÉ¾³ıskynet_context,ÈôÏú»Ù£¬·µ»ØNULL
		struct skynet_context * ret = skynet_context_release(ctx);
		if (ret) {
			ctx->init = true;
		}
		/*
			ctxµÄ³õÊ¼»¯Á÷³ÌÊÇ¿ÉÒÔ·¢ËÍÏûÏ¢³öÈ¥µÄ(Í¬Ê±Ò²¿ÉÒÔ½ÓÊÜÏûÏ¢)£¬µ«ÔÚ³õÊ¼»¯Á÷³ÌÍê³ÉÖ®Ç°
			½ÓÊÜµ½µÄÏûÏ¢¶¼±ØĞë»º´æÔÚmqÖĞ£¬²»ÄÜ´¦Àí¡£ÎÒÓÃÁËÒ»¸öĞ¡¼¼ÇÉ½â¾öÕâ¸öÎÊÌâ£¬¾ÍÊÇÔÚ³õÊ¼»¯
			Á÷³Ì¿ªÊ¼Ç°£¬¼Ù×°Ö»ÔÚglobalmqÖĞ(ÕâÊÇÓĞmqÖĞµÄÒ»¸ö±ê¼ÇÎ»¾ö¶¨µÄ)¡£ÕâÑù£¬ÏòËû·¢ËÍÏûÏ¢£¬
			²¢²»»á°ÑmqÑ¹Èëglobalmq,×ÔÈ»Ò²²»»á±»¹¤×÷Ïß³ÌÈ¡µ½¡£µÈ³õÊ¼»¯Á÷³Ì½áÊø£¬ÔÚÇ¿ÖÆ°ÑmqÑ¹Èë
			globalmq(ÎŞÂÛÊÇ·ñÎª¿Õ),¼´Ê¹Ê§°ÜÒ²Òª½øĞĞÕâ¸ö²Ù×÷
		*/

		//³õÊ¼»¯Á÷³Ì½á¹¹ºó½«Õâ¸öctx¶ÔÓ¦µÄmqÇ¿ÖÆÑ¹Èë globalmq

		//¼´½« Ò»¼¶ÏûÏ¢¶ÓÁĞ¼ÓÈëµ½ È«¾ÖµÄ¶ş¼¶ÏûÏ¢¶ÓÁĞ(ÏûÏ¢¶ÓÁĞÖĞ´æ·ÅÏûÏ¢¶ÓÁĞ)ÖĞ£¬
		skynet_globalmq_push(queue);
		
		if (ret) {
			skynet_error(ret, "LAUNCH %s %s", name, param ? param : "");
		}
		return ret;
	} else {
		skynet_error(ctx, "FAILED launch %s", name);
		uint32_t handle = ctx->handle;


		//½«ÒıÓÃ¼ÆÊı¼õÒ»,¼õÎª0Ê±ÔòÉ¾³ıskynet_context
		skynet_context_release(ctx);

		//¸ù¾İ±êºÅhandleÉ¾³ıÖ¸Õë
		skynet_handle_retire(handle);

		struct drop_t d = { handle };
		//Ïú»Ùqueue
		skynet_mq_release(queue, drop_message, &d);
		return NULL;
	}
}

//¸ù¾İskynet_context·ÖÅäÒ»¸ösesssion id
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


//skynet_contextÒıÓÃ¼ÆÊı¼Ó1
void 
skynet_context_grab(struct skynet_context *ctx) {
	ATOM_INC(&ctx->ref);
}

//½Úµã¶ÔÓ¦µÄ·şÎñÊı¼õ 1
void
skynet_context_reserve(struct skynet_context *ctx) {
	skynet_context_grab(ctx);
	// don't count the context reserved, because skynet abort (the worker threads terminate) only when the total context is 0 .
	// the reserved context will be release at last.

	//¼õÉÙtotal
	context_dec();
}

/*
	ÎÊÌâ¾ÍÔÚÕâÀï:
	handle ºÍ ctx µÄ°ó¶¨¹ØÏµÊÇÔÚ ctx Ä£¿éÍâ²¿²Ù×÷µÄ£¨²»È»Ò²×ö²»µ½ ctx µÄÕıÈ·Ïú»Ù£©£¬

	ÎŞ·¨È·±£´Ó handle È·ÈÏ¶ÔÓ¦µÄ ctx ÎŞĞ§µÄÍ¬Ê±£¬ctx ÕæµÄÒÑ¾­±»Ïú»ÙÁË¡£
	ËùÒÔ£¬µ±¹¤×÷Ïß³ÌÅĞ¶¨ mq ¿ÉÒÔÏú»ÙÊ±£¨¶ÔÓ¦µÄ handle ÎŞĞ§£©£¬ctx ¿ÉÄÜ»¹»î×Å£¨ÁíÒ»¸ö¹¤×÷Ïß³Ì»¹³ÖÓĞÆäÒıÓÃ£©£¬
	³ÖÓĞÕâ¸ö ctx µÄ¹¤×÷Ïß³Ì¿ÉÄÜÕıÔÚËüÉúÃüµÄ×îºóÒ»¿Ì£¬ÏòÆä·¢ËÍÏûÏ¢¡£½á¹û mq ÒÑ¾­Ïú»ÙÁË¡£

	µ± ctx Ïú»ÙÇ°£¬ÓÉËüÏòÆä mq ÉèÈëÒ»¸öÇåÀí±ê¼Ç¡£È»ºóÔÚ globalmq È¡³ö mq £¬·¢ÏÖÒÑ¾­ÕÒ²»µ½ handle ¶ÔÓ¦µÄ ctx Ê±£¬
	ÏÈÅĞ¶ÏÊÇ·ñÓĞÇåÀí±ê¼Ç¡£Èç¹ûÃ»ÓĞ£¬ÔÙ½« mq ÖØ·Å½ø globalmq £¬Ö±µ½ÇåÀí±ê¼ÇÓĞĞ§£¬ÔÚÏú»Ù mq ¡£
*/

//Ïú»Ùskynet_context
static void 
delete_context(struct skynet_context *ctx) {
	//¹Ø±ÕÈÕÖ¾ÎÄ¼ş¾ä±ú
	if (ctx->logfile) {
		fclose(ctx->logfile);
	}

	//µ÷ÓÃ¶ÔÓ¦¶¯Ì¬¿âµÄxxxx_releaseº¯Êı
	skynet_module_instance_release(ctx->mod, ctx->instance);
	
	//ÉèÖÃ±ê¼ÇÎ»,±ê¼Ç¸ÃÑ­»·¶ÓÁĞÒªÉ¾³ı£¬²¢ÇÒ°ÑËüÑ¹Èë global mq
	skynet_mq_mark_release(ctx->queue);
	
	CHECKCALLING_DESTROY(ctx)

	skynet_free(ctx);

	//Õâ¸ö½Úµã¶ÔÓ¦µÄ·şÎñÊıÒ² ¼õ 1
	context_dec();
}

//½«ÒıÓÃ¼ÆÊı¼õÒ» , ¼õÎª0Ê±ÔòÉ¾³ıskynet_context
struct skynet_context * 
skynet_context_release(struct skynet_context *ctx) {
	//ÒıÓÃ¼ÆÊı¼õ 1£¬¼õÎª0Ê±ÔòÉ¾³ıskynet_context
	if (ATOM_DEC(&ctx->ref) == 0) {
		
		delete_context(ctx);
		return NULL;
	}
	return ctx;
}

//ÔÚhandle±êÊ¶µÄskynet_contextµÄÏûÏ¢¶ÓÁĞÖĞ²åÈëÒ»ÌõÏûÏ¢
//handle¾ÍÊÇÒ»¸ö±êºÅ£¬ËùÓĞµÄskynet_contextµÄÖ¸Õë¶¼±£´æµ½Ò»¸öÊı×éÖĞ,Í¨¹ıhandle¿ÉÒÔ»ñµÃ¶ÔÓ¦µÄÖ¸Õë
int
skynet_context_push(uint32_t handle, struct skynet_message *message) {

	//Í¨¹ıhandle»ñÈ¡skynet_context,skynet_contextµÄÒıÓÃ¼ÆÊı¼Ó1
	
	struct skynet_context * ctx = skynet_handle_grab(handle);
	
	if (ctx == NULL) {
		return -1;
	}

	//½«message¼ÓÈëµ½cteµÄqueueÖĞ
	skynet_mq_push(ctx->queue, message);

	//ÒıÓÃ¼ÆÊı¼õÒ»,¼õÎª0Ê±ÔòÉ¾³ıskynet_context
	skynet_context_release(ctx);

	return 0;
}


//ÉèÖÃctx->endless = true;
void 
skynet_context_endless(uint32_t handle) {

	//¸ù¾İhandle±êºÅ»ñµÃskynet_cotextÖ¸Õë
	struct skynet_context * ctx = skynet_handle_grab(handle);
	if (ctx == NULL) {
		return;
	}
	
	ctx->endless = true;

	skynet_context_release(ctx);
}


//ÅĞ¶ÏÊÇ·ñÊÇÔ¶³ÌÏûÏ¢
int 
skynet_isremote(struct skynet_context * ctx, uint32_t handle, int * harbor) {

	//ÅĞ¶ÏÊÇ·ñÊÇÔ¶³ÌÏûÏ¢
	int ret = skynet_harbor_message_isremote(handle);
	
	if (harbor) {
		//·µ»Øharbor(×¢:¸ß°ËÎ»´æµÄÊÇharbor)
		*harbor = (int)(handle >> HANDLE_REMOTE_SHIFT);
	}
	return ret;
}


//Èç¹ûskynet_contextÖĞµÄ»Øµ÷º¯ÊıÖ¸Õë²»ÊÇ¿Õ ,µ÷ÓÃ´Ëº¯Êı,ÏûÏ¢·Ö·¢
static void
dispatch_message(struct skynet_context *ctx, struct skynet_message *msg) {

	assert(ctx->init);
	CHECKCALLING_BEGIN(ctx)

	pthread_setspecific(G_NODE.handle_key, (void *)(uintptr_t)(ctx->handle));

	//typeÊÇÇëÇóÀàĞÍ
	int type = msg->sz >> MESSAGE_TYPE_SHIFT;	//¸ß8Î»ÏûÏ¢Àà±ğ
	size_t sz = msg->sz & MESSAGE_TYPE_MASK;	//µÍ24Î»ÏûÏ¢´óĞ¡
	
	//´òÓ¡ÈÕÖ¾
	if (ctx->logfile) {
		skynet_log_output(ctx->logfile, msg->source, type, msg->session, msg->data, sz);
	}

	
	++ctx->message_count;
	
	int reserve_msg;

	if (ctx->profile) {
		ctx->cpu_start = skynet_thread_time();
		
		//µ÷ÓÃ»Øµ÷º¯Êı,¸ù¾İ·µ»ØÖµ¾ö¶¨ÊÇ·ñÒªÊÍ·ÅÏûÏ¢
		reserve_msg = ctx->cb(ctx, ctx->cb_ud, type, msg->session, msg->source, msg->data, sz);

		uint64_t cost_time = skynet_thread_time() - ctx->cpu_start;
		ctx->cpu_cost += cost_time;
	} 
	else 
	{
		//µ÷ÓÃ»Øµ÷º¯Êı,¸ù¾İ·µ»ØÖµ¾ö¶¨ÊÇ·ñÒªÊÍ·ÅÏûÏ¢
		reserve_msg = ctx->cb(ctx, ctx->cb_ud, type, msg->session, msg->source, msg->data, sz);
	}
	if (!reserve_msg) {
		skynet_free(msg->data);
	}

	CHECKCALLING_END(ctx)
}

//¶Ôskynet_contextµÄÏûÏ¢¶ÓÁĞÖĞµÄ ËùÓĞµÄmsgµ÷ÓÃdispatch_message
//´¦ÀíctxµÄÑ­»·ÏûÏ¢¶ÓÁĞÖĞµÄËùÓĞÏûÏ¢,µ÷ÓÃ »Øµ÷º¯Êı
void 
skynet_context_dispatchall(struct skynet_context * ctx) {
	// for skynet_error
	struct skynet_message msg;
	struct message_queue *q = ctx->queue;
	
	while (!skynet_mq_pop(q,&msg)) {
		dispatch_message(ctx, &msg);
	}
}


//message_queueµÄµ÷¶È,ÔÚ¹¤×÷Ïß³ÌÖĞ±»µ÷ÓÃ
//º¯ÊıµÄ×öÓÃÊÇ£»µ÷¶È´«ÈëµÄ2¼¶¶ÓÁĞ£¬²¢·µ»ØÏÂÒ»¸ö¿Éµ÷¶ÈµÄ2¼¶¶ÓÁĞ
struct message_queue * 
skynet_context_message_dispatch(struct skynet_monitor *sm, struct message_queue *q, int weight) {
	//ÔÚ¹¤×÷Ïß³ÌÖĞ£¬´«ÈëµÄq==NULL
	if (q == NULL) {
		//´ÓÈ«¾ÖµÄ¶ş¼¶ÏûÏ¢¶ÓÁĞµÄÍ·²¿ µ¯³öÒ»¸öÏûÏ¢¶ÓÁĞ
		q = skynet_globalmq_pop();
		if (q==NULL)
			return NULL;
	}

	//µÃµ½ÏûÏ¢¶ÓÁĞËùÊôµÄ·şÎñ¾ä±ú,

	//ÊµÖÊ¾ÍÊÇ»ñµÃÏûÏ¢¶ÓÁĞËùÊôµÄskynet_contextµÄ±êºÅ
	uint32_t handle = skynet_mq_handle(q);

	//¸ù¾İhandle»ñÈ¡skynet_conext
	struct skynet_context * ctx = skynet_handle_grab(handle);
	
	if (ctx == NULL) {
		struct drop_t d = { handle };
		skynet_mq_release(q, drop_message, &d);
		return skynet_globalmq_pop();
	}

	int i,n=1;
	struct skynet_message msg;

	for (i=0;i<n;i++) {

		//µ±2¼¶¶ÓÁĞÎª¿ÕÊ±²¢Ã»ÓĞ½«ÆäÑ¹Èë1¼¶¶ÓÁĞ£¬ÄÇËü´Ó´ËÏûÊ§ÁËÂğ£¬²»ÊÇ£¬ÕâÑù×öÊÇÎªÁË¼õÉÙ¿Õ×ª1¼¶¶ÓÁĞ
		//ÄÇÕâ¸ö¶ş¼¶¶ÓÁĞÊÇÊÇÃ´Ê±ºòÑ¹»ØµÄÄØ£¬ÔÚmessage_queueÖĞ£¬ÓĞÒ»¸öin_global±ê¼ÇÊÇ·ñÔÚ1¼¶¶ÓÁĞÖĞ
		//µ±2¼¶¶ÓÁĞµÄ³ö¶Ó(skynet_mq_pop)Ê§°ÜÊ±£¬Õâ¸ö±ê¼ÇÎ»0£¬ÔÚ¶ş¼¶¶ÓÁĞÈë¶ÓÊ±(skynet_mq_push)
		//»áÅĞ¶ÏÕâ¸ö±ê¼Ç£¬Èç¹ûÎª0,¾Í»á½«×Ô¼ºÑ¹Èë1¼¶¶ÓÁĞ¡£(skynet_mq_mark_realeasÒ²»áÅĞ¶Ï)
		//ËùÒÔÕâ¸ö2¼¶¶ÓÁĞÔÚÏÂ´ÎÈë¶ÓÊ±»á Ñ¹»Ø

		//´Ó¶ÓÁĞÖĞÈ¡³öÒ»ÌõÏûÏ¢,msgÎª´«³ö²ÎÊı
		if (skynet_mq_pop(q,&msg)) {
			
			skynet_context_release(ctx);
			return skynet_globalmq_pop();
			
		} else if (i==0 && weight >= 0) {

			//ĞŞ¸ÄÁËnµÄ´óĞ¡,¼´ĞŞ¸ÄÁËforÑ­»·µÄ´ÎÊı£¬Ò²¾ÍÊÇÃ¿´Îµ÷¶È´¦Àí¶àÉÙÌõÏûÏ¢¡£Õâ¸ö´ËÊ±Óë´«ÈëµÄweight
			//ÓĞ¹Ø¡£
			n = skynet_mq_length(q);

			//´ó¸Å¾ÍÊÇ£¬°Ñ¹¤×÷Ïß³Ì·Ö×é£¬Ç°ËÄ×éÃ¿×é8¸ö£¬³¬¹ıµÄ¹éÈëµÚÎå×é¡£¬
			//A,E×éÃ¿´Îµ÷¶È´¦ÀíÒ»ÌõÏûÏ¢£¬B×éÃ¿´Î´¦Àín/2Ìõ£¬C×éÃ¿´Î´¦Àín/4Ìõ£¬
			//D×éÃ¿´Î´¦Àín/8Ìõ¡£ÊÇÎªÁË¾ùÔÈÊ¹ÓÃ¶àºË
			n >>= weight;
		}

		//×öÁËÒ»¸ö¸ºÔØÅĞ¶Ï¡£¸ºÔØµÄãĞÖµÊÇ1024¡£²»¹ıÒ²Ö»ÊÇ½ö½öÊä³öÒ»ÌõlogÌáĞÑ¶øÒÑ
		int overload = skynet_mq_overload(q);
		if (overload) {
			skynet_error(ctx, "May overload, message queue length = %d", overload);
		}

		//´¥·¢ÁËÒ»ÏÂmonitor,Õâ¸ö¼à¿ØÊÇÓÃÀ´¼ì²âÏûÏ¢´¦ÀíÊÇ·ñ·¢ÉúÁËËÀÑ­»·£¬²»¹ıÒ²Ö»ÊÇÊä³öÒ»ÌõligÌáĞÑÒ»ÏÂ
		//Õâ¸ö¼ì²âÊÇ·ÅÔÚÒ»¸ö×¨ÃÅµÄ¼à¿ØÏß³ÌÀï×öµÄ£¬ÅĞ¶ÏËÀÑ­»·µÄÊ±¼äÊÇ5Ãë
		/*
		void 
		skynet_monitor_trigger(struct skynet_monitor *sm, uint32_t source, uint32_t destination) {
			sm->source = source;
			sm->destination = destination;
			ATOM_INC(&sm->version);
		}

		*/

		//smÊÇ´«Èë£¬±»ĞŞ¸Ä,ÔÚmonitrrÏß³ÌÖĞ»á¼ì²âsmµÄÖµ
		skynet_monitor_trigger(sm, msg.source , handle);

		//Èç¹ûskynet_contextµÄ»Øµ÷º¯ÊıµÄº¯ÊıÖ¸ÕëÎª¿Õ
		if (ctx->cb == NULL) 
		{
			skynet_free(msg.data);
		}
		else 
		{
			/*
					struct skynet_message {
						uint32_t source;	//ÏûÏ¢Ô´ËùÊôµÄskynetµÄ±êºÅ
						int session;		//ÓÃÀ´×öÉÏÏÂÎÄµÄ±êÊ¶
						void * data;		//ÏûÏ¢Ö¸Õë
						size_t sz;			//ÏûÏ¢³¤¶È,ÏûÏ¢µÄÇëÇóÀàĞÍ¶¨ÒåÔÚ¸ß°ËÎ»
						};

			*/
		
			//ÏûÏ¢·Ö·¢	ÊµÖÊ¾ÍÊÇµ÷ÓÃskynet_contextÖĞµÄ»Øµ÷º¯Êı´¦ÀíÏûÏ¢
			dispatch_message(ctx, &msg);
		}

		
		//´¥·¢ÁËÒ»ÏÂmonitor,Õâ¸ö¼à¿ØÊÇÓÃÀ´¼ì²âÏûÏ¢´¦ÀíÊÇ·ñ·¢ÉúÁËËÀÑ­»·£¬²»¹ıÒ²Ö»ÊÇÊä³öÒ»ÌõligÌáĞÑÒ»ÏÂ
		//Õâ¸ö¼ì²âÊÇ·ÅÔÚÒ»¸ö×¨ÃÅµÄ¼à¿ØÏß³ÌÀï×öµÄ£¬ÅĞ¶ÏËÀÑ­»·µÄÊ±¼äÊÇ5Ãë
		skynet_monitor_trigger(sm, 0,0);
	}

	assert(q == ctx->queue);

	//´ÓÈ«¾Ö¶ş¼¶ÏûÏ¢¶ÓÁĞÖĞÒÆ³ıÒ»¸öÏûÏ¢¶ÓÁĞ£¬´ÓÍ·²¿ÒÆ³ı
	struct message_queue *nq = skynet_globalmq_pop();
	if (nq) {
		// If global mq is not empty , push q back, and return next queue (nq)
		// Else (global mq is empty or block, don't push q back, and return q again (for next dispatch)

		//½«´¦ÀíµÄÏûÏ¢µÄÏûÏ¢¶ÓÁĞqÖØĞÂ¼ÓÈëµ½È«¾ÖµÄÏûÏ¢¶ÓÁĞÖĞ
		skynet_globalmq_push(q);

		q = nq;
	} 
	skynet_context_release(ctx);

	return q;
}

//½«addrÖ¸ÏòµÄÊı¾İ¸´ÖÆµ½nameÖĞ
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


//¸ù¾İ×Ö·û´®²éÕÒhandle±êºÅ£¬name¿ÉÒÔÊÇhandle¶ÔÓ¦µÄÃû×Ö£¬Ò²¿ÉÒÔÊ¹ :xxxxx
uint32_t 
skynet_queryname(struct skynet_context * context, const char * name) {
	switch(name[0]) {
	case ':':
		//strtoul  ½«×Ö·û´®ÀàĞÍ×ª»»Îªunsigned longÀàĞÍ
		return strtoul(name+1,NULL,16);
	case '.':
		
		//¸ù¾İÃû³Æ²éÕÒhandle
		return skynet_handle_findname(name + 1);
	}
	skynet_error(context, "Don't support query global name %s",name);
	return 0;
}

//½«handle¶ÔÓ¦µÄskynet_context´Óhandle_storageÖĞÉ¾³ı
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
	
	//ÊÕ»Øhandle,¼´Ïú»Ùhandle¶ÔÓ¦µÄskynet_contextÔÚÊı×éÖĞµÄÖ¸Õë
	skynet_handle_retire(handle);
}

// skynet command

//ÃüÁîÃû³ÆÓë»Øµ÷º¯Êı¶ÔÓ¦µÄ½á¹¹Ìå
struct command_func {
	const char *name;
	const char * (*func)(struct skynet_context * context, const char * param);
};

//"timeout"ÃüÁî¶ÔÓ¦µÄ»Øµ÷º¯Êı
//×¢ÒâÊÇµ÷ÓÃ skynet_timeout(),²åÈë¶¨Ê±Æ÷
static const char *
cmd_timeout(struct skynet_context * context, const char * param) {
	char * session_ptr = NULL;

	//long int strtol (const char* str, char** endptr, int base);
	//baseÊÇ½øÖÆ  paramÊÇĞèÒª×ª»»µÄ×Ö·û´®  session_ptr==NULLÊ±£¬¸Ã²ÎÊıÎŞĞ§,

	//Í¨¹ı×Ö·û´®»ñµÃÊı×Ö
	//½«param×ªÎª10½øÖÆÊı
	int ti = strtol(param, &session_ptr, 10);

		//»ñµÃÒ»¸ösession   »á»°id
	int session = skynet_context_newsession(context);
	
	//²åÈë¶¨Ê±Æ÷£¬timeµÄµ¥Î»ÊÇ0.01Ãë£¬Èçtime=300,±íÊ¾3Ãë
	skynet_timeout(context->handle, ti, session);

	sprintf(context->result, "%d", session);
	return context->result;
}

//"reg"¶ÔÓ¦»Øµ÷º¯Êı ,ÎªcontextÃüÃû
static const char *
cmd_reg(struct skynet_context * context, const char * param) {

	if (param == NULL || param[0] == '\0') 
	{
		sprintf(context->result, ":%x", context->handle);
		return context->result;
	}
		//ÃüÃûÊÇ£¬¸ñÊ½ÊÇ        .Ãû×Ö
	else if (param[0] == '.') 
	{
		//ÊµÖÊ¾ÍÊÇ½«handle,name²åÈëµ½handle_storageµÄnameÊı×éÖĞ
		//Îªskynet_contextÍ¨¹ıÒ»¸öÃû³Æ
		return skynet_handle_namehandle(context->handle, param + 1);
	}
	else 
	{
		skynet_error(context, "Can't register global name %s in C", param);
		return NULL;
	}
}

//"query"¶ÔÓ¦µÄ»Øµ÷º¯Êı
//¸ù¾İÃû×Ö²éÕÒcontext¶ÔÓ¦µÄhandle±êºÅ
static const char *
cmd_query(struct skynet_context * context, const char * param) {
	//Ãû×ÖµÄ¸ñÊ½  .Ãû×Ö
	if (param[0] == '.') {
		
		uint32_t handle = skynet_handle_findname(param+1);

		//½«handleĞ´ÈëÔÚcontextµÄresultÖĞ 
		if (handle) {
			sprintf(context->result, ":%x", handle);
			return context->result;
		}
	}
	return NULL;
}

//"name"¶ÔÓ¦µÄ»Øµ÷º¯Êı
//Îªhandle¶ÔÓ¦µÄskynet_contextÃüÃû,Ö»ÊÇhandle,name¶¼ÔÚparamÖĞ
static const char *
cmd_name(struct skynet_context * context, const char * param) {
	int size = strlen(param);
	char name[size+1];
	char handle[size+1];
	
	sscanf(param,"%s %s",name,handle);

	if (handle[0] != ':') {
		return NULL;
	}
	//µÃµ½handle
	uint32_t handle_id = strtoul(handle+1, NULL, 16);
	if (handle_id == 0) {
		return NULL;
	}
	
	if (name[0] == '.') {
		//ÊµÖÊ¾ÍÊÇ½«handle,name²åÈëµ½handle_storageµÄnameÊı×éÖĞ
		//Îªskynet_contextÍ¨¹ıÒ»¸öÃû³Æ
		return skynet_handle_namehandle(handle_id, name + 1);
	} else {
		skynet_error(context, "Can't set global name %s in C", name);
	}
	return NULL;
}


//"exit"¶ÔÓ¦µÄ»Øµ÷º¯Êı
//µ÷ÓÃ handle_exit()
//½«contextÍË³ö
static const char *
cmd_exit(struct skynet_context * context, const char * param) {
	handle_exit(context, 0);
	return NULL;
}

//¸ù¾İ²ÎÊı»ñµÃhandle
static uint32_t
tohandle(struct skynet_context * context, const char * param) {
	uint32_t handle = 0;
	//Èç¹ûÊÇ :xxxxx
	if (param[0] == ':') {
		
		handle = strtoul(param+1, NULL, 16);
	} else if (param[0] == '.') {
		//Èç¹ûÊÇ .Ãû×Ö
		handle = skynet_handle_findname(param+1);
	} else {
		skynet_error(context, "Can't convert %s to handle",param);
	}

	return handle;
}


//"kill"¶ÔÓ¦µÄ»Øµ÷º¯Êı
//½«param¶ÔÓ¦µÄhandleÍË³ö
static const char *
cmd_kill(struct skynet_context * context, const char * param) {
	uint32_t handle = tohandle(context, param);
	if (handle) {
		handle_exit(context, handle);
	}
	return NULL;
}

//"launch"¶ÔÓ¦µÄ»Øµ÷º¯Êı,¼ÓÔØÄ£¿é(¶¯Ì¬¿â),
//´´½¨Ò»¸öskynet_context
static const char *
cmd_launch(struct skynet_context * context, const char * param) {
	size_t sz = strlen(param);
	char tmp[sz+1];
	strcpy(tmp,param);
	char * args = tmp;

	// aaa bbb ccc

	//·Ö¸î×Ö·û´®
	char * mod = strsep(&args, " \t\r\n");

	//·Ö¸î×Ö·û´®
	args = strsep(&args, "\r\n");
													//Ä£¿éÃû ²ÎÊı
	struct skynet_context * inst = skynet_context_new(mod,args);
	
	if (inst == NULL) {
		return NULL;
	} else {
	//½«handleĞ´Èëµ½resultÖĞ
		id_to_hex(context->result, inst->handle);
		return context->result;
	}
}

//"getenv"¶ÔÓ¦µÄ»Øµ÷º¯Êı
//»ñÈ¡luaÈ«¾Ö±äÁ¿   
static const char *
cmd_getenv(struct skynet_context * context, const char * param) {
	return skynet_getenv(param);
}

//"setenv"¶ÔÓ¦µÄ»Øµ÷º¯Êı
//ÉèÖÃluaÈ«¾Ö±äÁ¿   ±äÁ¿Ãû Öµ
static const char *
cmd_setenv(struct skynet_context * context, const char * param) {
	size_t sz = strlen(param);
	//±£´æµÄÊÇ±äÁ¿Ãû 
	char key[sz+1];
	int i;


	//»ñµÃ±äÁ¿Ãû 
	for (i=0;param[i] != ' ' && param[i];i++) {
		key[i] = param[i];
	}
	if (param[i] == '\0')
		return NULL;

	key[i] = '\0';
	param += i+1;

	//ÉèÖÃ
	skynet_setenv(key,param);
	return NULL;
}

//"starttime"¶ÔÓ¦µÄ»Øµ÷º¯Êı 
//µ÷ÓÃskynet_starttime() »ñÈ¡³ÌĞòÆô¶¯Ê±¼ä
static const char *
cmd_starttime(struct skynet_context * context, const char * param) {
	//·µ»ØÆô¶¯¸Ã³ÌĞòµÄÊ±¼ä
	uint32_t sec = skynet_starttime();

	//½«Ê±¼äĞ´Èëµ½resultÖĞ
	sprintf(context->result,"%u",sec);
	return context->result;
}

//"abort"¶ÔÓ¦µÄ»Øµ÷º¯Êı

//skynet_handle_retireall()
//»ØÊÕËùÓĞµÄhandler
//»ØÊÕhandle_storageÖĞµÄÖ¸ÕëÊı×éÖĞµÄËùÓĞskynet_contextÖ¸Õë

static const char *
cmd_abort(struct skynet_context * context, const char * param) {
	skynet_handle_retireall();
	return NULL;
}

//"monitor"¶ÔÓ¦µÄ»Øµ÷º¯Êı
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

//"stat"¶ÔÓ¦µÄ»Øµ÷º¯Êı
//»ñÈ¡skynet_contextÖĞµÄÒ»Ğ©×´Ì¬ĞÅÏ¢
static const char *
cmd_stat(struct skynet_context * context, const char * param) {

	if (strcmp(param, "mqlen") == 0) {
		
		//»ñÈ¡skynet_contextÖĞµÄÏûÏ¢¶ÓÁĞÖĞµÄÏûÏ¢¸öÊı
		int len = skynet_mq_length(context->queue);
		//½«½á¹ûĞ´Èëµ½resultÖĞ
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

//"logon"¶ÔÓ¦µÄ»Øµ÷º¯Êı
//×÷ÓÃÊÇ´ò¿ªparamÖĞÖ¸¶¨µÄhandle¶ÔÓ¦µÄskynet_contextµÄÈÕÖ¾ÎÄ¼ş
static const char *
cmd_logon(struct skynet_context * context, const char * param) {

	//Í¨¹ıparam»ñµÃhandle
	uint32_t handle = tohandle(context, param);

	if (handle == 0)
		return NULL;

	//¸ù¾İ±êºÅhandle»ñµÃ¶ÔÓ¦µÄskynet_contextÖ¸Õë
	struct skynet_context * ctx = skynet_handle_grab(handle);
	if (ctx == NULL)
		return NULL;
	
	FILE *f = NULL;
	FILE * lastf = ctx->logfile;
	if (lastf == NULL) {
		//´ò¿ªÈÕÖ¾ÎÄ¼ş
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

//"logoff"¶ÔÓ¦µÄ»Øµ÷º¯Êı
//×÷ÓÃÊÇ¹Ø±ÕparamÖĞµÄhandle¶ÔÓ¦µÄ skynet_context¶ÔÓ¦µÄÈÕÖ¾ÎÄ¼ş

static const char *
cmd_logoff(struct skynet_context * context, const char * param) {

	//Í¨¹ıparam»ñµÃhandle
	uint32_t handle = tohandle(context, param);
	if (handle == 0)
		return NULL;

	//»ñµÃhandle¶ÔÓ¦µÄskynet_context
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
//"SIGNAL" ÃüÁî¶ÔÓ¦µÄ»Øµ÷º¯Êı
//×÷ÓÃÊÇµ÷ÓÃ paramµÄhandle¶ÔÓ¦µÄ skynet_context¶ÔÓ¦µÄ¶¯Ì¬¿âÖĞµÄxxxx_signal()º¯Êı
static const char *
cmd_signal(struct skynet_context * context, const char * param) {

	//»ñµÃhandle
	uint32_t handle = tohandle(context, param);
	
	if (handle == 0)
		return NULL;
	//¸ù¾İ±êºÅ»ñµÃ¶ÔÓ¦µÄskynet_context
	struct skynet_context * ctx = skynet_handle_grab(handle);
	
	if (ctx == NULL)
		return NULL;
	
	param = strchr(param, ' ');
	int sig = 0;

	if (param) {
		sig = strtol(param, NULL, 0);
	}
	// NOTICE: the signal function should be thread safe.

	//µ÷ÓÃskynet_context¶ÔÓ¦µÄ¶¯Ì¬¿âÖĞµÄxxxx_signal()º¯Êı
	skynet_module_instance_signal(ctx->mod, ctx->instance, sig);

	skynet_context_release(ctx);
	return NULL;
}

//ÃüÁîÃû³ÆÓëº¯ÊıÖ¸Õë¶ÔÓ¦µÄ½á¹¹ÌåÊı×é
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

//¸ù¾İÃüÁîÃû³Æ²éÕÒ¶ÔÓ¦µÄ»Øµ÷º¯Êı£¬²¢ÇÒµ÷ÓÃ
const char * 
skynet_command(struct skynet_context * context, const char * cmd , const char * param) {
	struct command_func * method = &cmd_funcs[0];
	while(method->name) {

		//Èç¹ûÕÒµ½ £¬Ö´ĞĞ»Øµ÷º¯Êı
		if (strcmp(cmd, method->name) == 0) {
			return method->func(context, param);
		}
		++method;
	}

	return NULL;
}

//²ÎÊı¹ıÂË
//¸ù¾İtype×öÁËÁ½¸ö´¦Àí
/*
1.(type & PTYPE_TAG_DONTCOPY) == 0
»á½«data¸´ÖÆÒ»·İÓÃ×÷Êµ¼Ê·¢ËÍ£¬ÕâÖÖÇé¿öÏÂÔ­À´µÄdata¾ÍÒªÓÉµ÷ÓÃÕß¸ºÔğÊÍ·Å
2.(type & PTYPE_TAG_ALLOCSESSION)>0
	»á´Ósesson¼ÆÊıÆ÷·ÖÅäÒ»¸ösession

´¦ÀíÍêºó£¬type»áºÏ²¢µ½szµÄ¸ß8Î»
*/
static void
_filter_args(struct skynet_context * context, int type, int *session, void ** data, size_t * sz) {

	//ÊÇ·ñ                        0x10000
	int needcopy = !(type & PTYPE_TAG_DONTCOPY);
								//0x20000
	int allocsession = type & PTYPE_TAG_ALLOCSESSION;

	type &= 0xff;

	//»á´Ósesson¼ÆÊıÆ÷·ÖÅäÒ»¸ösession
	if (allocsession) {
		assert(*session == 0);
		//µÃµ½Ò»¸öĞÂµÄsession id,¾ÍÊÇ½«Ô­À´µÄcontext¶ÔÓ¦µÄsession+1 
		*session = skynet_context_newsession(context);
	}

	//»á½«dataÊı¾İ¸´ÖÆ  ÕâÖÖÇé¿öÏÂÔ­À´µÄdata¾ÍÒªÓÉµ÷ÓÃÕß¸ºÔğÊÍ·Å
	if (needcopy && *data) {
		char * msg = skynet_malloc(*sz+1);
		memcpy(msg, *data, *sz);
		msg[*sz] = '\0';
		*data = msg;
	}

	//´¦ÀíÍêºó£¬type»áºÏ²¢µ½szµÄ¸ß8Î»   24
	*sz |= (size_t)type << MESSAGE_TYPE_SHIFT;
}


/*
 * ÏòhandleÎªdestinationµÄ·şÎñ·¢ËÍÏûÏ¢(×¢£ºhandleÎªdestinationµÄ·şÎñ²»Ò»¶¨ÊÇ±¾µØµÄ)
 * typeÖĞº¬ÓĞ PTYPE_TAG_ALLOCSESSION £¬Ôòsession±ØĞëÊÇ0
 * typeÖĞº¬ÓĞ PTYPE_TAG_DONTCOPY £¬Ôò²»ĞèÒª¿½±´Êı¾İ
 */

//·¢ËÍÏûÏ¢
int
skynet_send(struct skynet_context * context, uint32_t source, uint32_t destination , int type, int session, void * data, size_t sz) {

	//¶ÔÏûÏ¢³¤¶ÈÏŞÖÆ MESSAGE_TYPE_MASK¾ÍÊÇ SIZE_MAX >> 8,×î´óÖ»ÄÜÎª2^24,16MB
	if ((sz & MESSAGE_TYPE_MASK) != sz) {
		skynet_error(context, "The message to %x is too large", destination);
		if (type & PTYPE_TAG_DONTCOPY) {
			skynet_free(data);
		}
		return -1;
	}

	//²ÎÊı¹ıÂË
	//¸ù¾İtype×öÁËÁ½¸ö´¦Àí
	/*
		1.(type & PTYPE_TAG_DONTCOPY) == 0
			»á½«data¸´ÖÆÒ»·İÓÃ×÷Êµ¼Ê·¢ËÍ£¬ÕâÖÖÇé¿öÏÂÔ­À´µÄdata¾ÍÒªÓÉµ÷ÓÃÕß¸ºÔğÊÍ·Å
		2.(type & PTYPE_TAG_ALLOCSESSION)>0
		»á´Ósesson¼ÆÊıÆ÷·ÖÅäÒ»¸ösession
		
	*/
	_filter_args(context, type, &session, (void **)&data, &sz);

	if (source == 0) {
		source = context->handle;
	}

	if (destination == 0) {
		return session;
	}

	//Í¶µİµ½½ÓÊÜÕßµÄĞÅÏä,¸ù¾İ½ÓÊÜÕß¾ä±úÅĞ¶ÁÊÇ·ñÎªÔ¶³Ì½Úµã£¬Èç¹ûÊÇ¾ÍÓÃharbo·¢ËÍ(ÄÚÖÃµÄ¼¯Èº·½°¸£¬
	//ÏÖÔÚÒÑ¾­²»ÍÆ¼öÊ¹ÓÃ),³É¹¦·µ»Øsession,Ê§°Ü·µ»Ø-1,²¢ÇÒÊÍ·Ådata

	// Èç¹ûÏûÏ¢Ê±·¢¸øÔ¶³ÌµÄ
	if (skynet_harbor_message_isremote(destination)) {
		struct remote_message * rmsg = skynet_malloc(sizeof(*rmsg));
		rmsg->destination.handle = destination;
		rmsg->message = data;
		rmsg->sz = sz;
		
		skynet_harbor_send(rmsg, source, session);
	}
	// ±¾»úÏûÏ¢ Ö±½ÓÑ¹ÈëÏûÏ¢¶ÓÁĞ
	else
	{
		struct skynet_message smsg;
		smsg.source = source;
		smsg.session = session;
		smsg.data = data;
		smsg.sz = sz;

		//destinationÄ¿µÄµØµÄskynet_context¶ÔÓ¦µÄ±êºÅ
		//¾ÍÊÇ½«ÏûÏ¢·¢ËÍµ½destination¶ÔÓ¦µÄÏûÏ¢¶ÓÁĞÖĞ
		if (skynet_context_push(destination, &smsg)) {
			skynet_free(data);
			return -1;
		}
	}
	return session;
}

//¸ù¾İÃû×ÖÕÒµ½Ä¿µÄµØ skynet_context£¬·¢ËÍÏûÏ¢
int
skynet_sendname(struct skynet_context * context, uint32_t source, const char * addr , int type, int session, void * data, size_t sz) {
	if (source == 0) {
		source = context->handle;
	}
	uint32_t des = 0;

	//Èç¹ûµØÖ·ÊÇ :xxxxx
	if (addr[0] == ':') 
	{
		//xxxx¾ÍÊÇhandle
		des = strtoul(addr+1, NULL, 16);
	}
	//Èç¹ûµØÖ·ÊÇ .Ãû×Ö
	else if (addr[0] == '.') 
	{
	//¸ù¾İÃû³ÆÕÒµ½¶ÔÓ¦µÄskynet_contextµÄ±êºÅ
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

	//Ô¶³ÌÏûÏ¢
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

//»ñÈ¡skynet_context¶ÔÓ¦µÄ±êºÅhandle
uint32_t 
skynet_context_handle(struct skynet_context *ctx) {
	return ctx->handle;
}

//³õÊ¼»¯struct skynet_contextµÄ»Øµ÷º¯ÊıÖ¸Õë£¬»Øµ÷º¯Êı²ÎÊı
void 
skynet_callback(struct skynet_context * context, void *ud, skynet_cb cb) {
	context->cb = cb;
	context->cb_ud = ud;
}


// Ïòctx·şÎñ·¢ËÍÏûÏ¢(×¢£ºctx·şÎñÒ»¶¨ÊÇ±¾µØµÄ)
//ÊµÖÊ¾ÍÊÇ½«ÏûÏ¢¼ÓÈëµ½skynet_contextµÄÏûÏ¢¶ÓÁĞÖĞ
void
skynet_context_send(struct skynet_context * ctx, void * msg, size_t sz, uint32_t source, int type, int session) {
	struct skynet_message smsg;
	smsg.source = source;
	smsg.session = session;
	smsg.data = msg;
	smsg.sz = sz | (size_t)type << MESSAGE_TYPE_SHIFT;

	// Ñ¹ÈëÏûÏ¢¶ÓÁĞ
	skynet_mq_push(ctx->queue, &smsg);
}


//ÔÚskynet_mian.cµÄmainº¯ÊıÖĞµ÷ÓÃ
void 
skynet_globalinit(void) {

	G_NODE.total = 0;
	G_NODE.monitor_exit = 0;
	G_NODE.init = 1;

	//´´½¨Ïß³ÌÈ«¾Ö±äÁ¿
	if (pthread_key_create(&G_NODE.handle_key, NULL)) {
		fprintf(stderr, "pthread_key_create failed");
		exit(1);
	}
	// set mainthread's key  	ÓĞ#define THREAD_MAIN 1
	skynet_initthread(THREAD_MAIN); 
}

//Ïú»ÙÏß³ÌÈ«¾Ö±äÁ¿
void 
skynet_globalexit(void) {
	pthread_key_delete(G_NODE.handle_key);
}

//³õÊ¼»¯Ïß³Ì,³õÊ¼»¯Ïß³ÌÈ«¾Ö±äÁ¿
void
skynet_initthread(int m) {
	uintptr_t v = (uint32_t)(-m);
	pthread_setspecific(G_NODE.handle_key, (void *)v);
}

void
skynet_profile_enable(int enable) {
	G_NODE.profile = (bool)enable;
}

