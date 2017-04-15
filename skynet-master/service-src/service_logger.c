#include "skynet.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

struct logger {
	//��־������ļ�
	FILE * handle;
	
	//��־���ļ���
	char * filename;

	int close;
};

//����struct logger�ṹ�壬����ָ��
//����skynet_context_new()�м��غ����
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


//��ģ������Ϣ����ʱ�� �ص�����
static int
logger_cb(struct skynet_context * context, void *ud, int type, int session, uint32_t source, const void * msg, size_t sz) {
	struct logger * inst = ud;
	switch (type) {
	case PTYPE_SYSTEM:
		if (inst->filename) {
			inst->handle = freopen(inst->filename, "a", inst->handle);
		}
		break;
		//��ӡ��־
	case PTYPE_TEXT:
		fprintf(inst->handle, "[:%08x] ",source);
		fwrite(msg, sz , 1, inst->handle);
		fprintf(inst->handle, "\n");
		fflush(inst->handle);
		break;
	}

	return 0;
}

//�ڼ���ģ���ᱻ����,��skynet_context_new()�е���
//����Ҫ�ǳ�ʼ��ģ���Ӧ��skynet_context�еĻص�����
int
logger_init(struct logger * inst, struct skynet_context *ctx, const char * parm) {
	//����������ǿգ����ļ�
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
		//Ĭ����־�����stdout
		inst->handle = stdout;
	}
	if (inst->handle) {
		//����skynte_context �еĻص�����ָ��,�Լ��ص������Ĳ���
		skynet_callback(ctx, inst, logger_cb);
		
		//�����������Ʋ��Ҷ�Ӧ�Ļص����������ҵ���
		//skynet_command(struct skynet_context * context, const char * cmd , const char * param)
		//������ cmd_reg()����  Ϊcontext����Ϊlogger
		skynet_command(ctx, "REG", ".logger");
		return 0;
	}
	return 1;
}



