#include "skynet_log.h"
#include "skynet_timer.h"
#include "skynet.h"
#include "skynet_socket.h"
#include <string.h>
#include <time.h>



//打开日志文件
FILE * 
skynet_log_open(struct skynet_context * ctx, uint32_t handle) {

	//获取的是 lua 的全局变量 logpath 值
	const char * logpath = skynet_getenv("logpath");
	
	if (logpath == NULL)
		return NULL;
	size_t sz = strlen(logpath);
	char tmp[sz + 16];

	//获得日志文件名称
	sprintf(tmp, "%s/%08x.log", logpath, handle);

	//打开日志文件
	FILE *f = fopen(tmp, "ab");

	if (f) {
		//启动时间
		uint32_t starttime = skynet_starttime();

		/当前时间
		uint64_t currenttime = skynet_now();

		time_t ti = starttime + currenttime/100;
		skynet_error(ctx, "Open log file %s", tmp);

							//char * ctime(onst time_t *timep)将日期时间已字符串表示
		fprintf(f, "open time: %u %s", (uint32_t)currenttime, ctime(&ti));
		fflush(f);
	} else {
		skynet_error(ctx, "Open log file %s fail", tmp);
	}
	return f;
}


//关闭日志文件 
void
skynet_log_close(struct skynet_context * ctx, FILE *f, uint32_t handle) {
	skynet_error(ctx, "Close log file :%08x", handle);
	fprintf(f, "close time: %u\n", (uint32_t)skynet_now());
	fclose(f);
}


//往日志文件中写入buffer数组
static void
log_blob(FILE *f, void * buffer, size_t sz) {
	size_t i;
	uint8_t * buf = buffer;
	for (i=0;i!=sz;i++) {
		//%02x 不足2位时，前面补0
		//printf("%02X", 0x1); //输出  01
		fprintf(f, "%02x", buf[i]);
	}
}

static void
log_socket(FILE * f, struct skynet_socket_message * message, size_t sz) {

	fprintf(f, "[socket] %d %d %d ", message->type, message->id, message->ud);

	if (message->buffer == NULL) {
		const char *buffer = (const char *)(message + 1);
		sz -= sizeof(*message);
		const char * eol = memchr(buffer, '\0', sz);
		if (eol) {
			sz = eol - buffer;
		}
		fprintf(f, "[%*s]", (int)sz, (const char *)buffer);
	} else {
		sz = message->ud;
		log_blob(f, message->buffer, sz);
	}
	fprintf(f, "\n");
	fflush(f);
}

//打印日志
void 
skynet_log_output(FILE *f, uint32_t source, int type, int session, void * buffer, size_t sz) {
	//判断类型 
	if (type == PTYPE_SOCKET) {
		log_socket(f, buffer, sz);
	} else {
		uint32_t ti = (uint32_t)skynet_now();
		fprintf(f, ":%08x %d %d %u ", source, type, session, ti);
		log_blob(f, buffer, sz);
		fprintf(f,"\n");
		fflush(f);
	}
}


