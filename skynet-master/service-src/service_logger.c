#include "skynet.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

struct logger {
	//日志输出的文件
	FILE * handle;
	
	//日志的文件名
	char * filename;

	int close;
};

//创建struct logger结构体，返回指针
//会在skynet_context_new()中加载后调用
struct logger *
logger_create(void) {
	struct logger * inst = skynet_malloc(sizeof(*inst));
	inst->handle = NULL;
	inst->close = 0;
	inst->filename = NULL;

	return inst;
}

void
logger_release(struct logger * inst) {
	if (inst->close) {
		fclose(inst->handle);
	}
	skynet_free(inst->filename);
	skynet_free(inst);
}


//该模块有消息到来时的 回调函数
static int
logger_cb(struct skynet_context * context, void *ud, int type, int session, uint32_t source, const void * msg, size_t sz) {
	struct logger * inst = ud;
	switch (type) {
	case PTYPE_SYSTEM:
		if (inst->filename) {
			inst->handle = freopen(inst->filename, "a", inst->handle);
		}
		break;
		//打印日志
	case PTYPE_TEXT:
		fprintf(inst->handle, "[:%08x] ",source);
		fwrite(msg, sz , 1, inst->handle);
		fprintf(inst->handle, "\n");
		fflush(inst->handle);
		break;
	}

	return 0;
}

//在加载模块后会被调用,即skynet_context_new()中调用
//，主要是初始化模块对应的skynet_context中的回调函数
int
logger_init(struct logger * inst, struct skynet_context *ctx, const char * parm) {
	//如果参数不是空，打开文件
	if (parm) {
		inst->handle = fopen(parm,"w");
		if (inst->handle == NULL) {
			return 1;
		}
		inst->filename = skynet_malloc(strlen(parm)+1);
		strcpy(inst->filename, parm);
		inst->close = 1;
	} 
	else 
	{
		//默认日志输出到stdout
		inst->handle = stdout;
	}
	if (inst->handle) {
		//设置skynte_context 中的回调函数指针,以及回调函数的参数
		skynet_callback(ctx, inst, logger_cb);
		
		//根据命令名称查找对应的回调函数，并且调用
		//skynet_command(struct skynet_context * context, const char * cmd , const char * param)
		//调用了 cmd_reg()函数  为context命名为logger
		skynet_command(ctx, "REG", ".logger");
		return 0;
	}
	return 1;
}



