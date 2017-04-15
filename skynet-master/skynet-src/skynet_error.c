#include "skynet.h"
#include "skynet_handle.h"
#include "skynet_mq.h"
#include "skynet_server.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define LOG_MESSAGE_SIZE 256	//日志的大小

//skynet 对错误处理的封装

//将msg封装成skynet_message类型，发送给对应的skynet_context
void 
skynet_error(struct skynet_context * context, const char *msg, ...) {
	static uint32_t logger = 0;
	if (logger == 0) {
		//根据名称查找handle  查找服务
		logger = skynet_handle_findname("logger");
	}
	if (logger == 0) {
		return;
	}

	char tmp[LOG_MESSAGE_SIZE];
	char *data = NULL;

	//可变参数
	va_list ap;   //ap是一个指针

	//起始位置是msg ....
	//ap指向msg后面的一个参数
	va_start(ap,msg);

	//vsnprintf()将可变参数格式化输出到一个字符数组
	int len = vsnprintf(tmp, LOG_MESSAGE_SIZE, msg, ap);

	va_end(ap);

	
	if (len >=0 && len < LOG_MESSAGE_SIZE) {

		//strup()将串拷贝到新建的位置处，得到实际的msg
		data = skynet_strdup(tmp);
	} else {

		//如果原来的缓冲区存放不下数据
		int max_size = LOG_MESSAGE_SIZE;
		for (;;) {
			max_size *= 2;

			//msg大于 LOG_MESSAGE_SIZE 尝试分配更大的空间来存放
			data = skynet_malloc(max_size);

			va_start(ap,msg);

			//将msg格式化到data中
			len = vsnprintf(data, max_size, msg, ap);

			va_end(ap);
			//知道写入 data的数据不比max_size大，实际就是data能存放msg
			if (len < max_size) {
				break;
			}
			skynet_free(data);
		}
	}

	
	if (len < 0) {
		skynet_free(data);
		perror("vsnprintf error :");
		return;
	}

/*
	struct skynet_message {
		uint32_t source;	//消息源的句柄
		int session;		//用来做上下文的标识
		void * data;		//消息指针
		size_t sz;			//消息长度,消息的请求类型定义在高八位
		};
*/

	struct skynet_message smsg;
	if (context == NULL) {
		smsg.source = 0;
	} else {
		smsg.source = skynet_context_handle(context);
	}
	smsg.session = 0;
	smsg.data = data;
	smsg.sz = len | ((size_t)PTYPE_TEXT << MESSAGE_TYPE_SHIFT);

	//将消息发送到对应的 handler中处理
	skynet_context_push(logger, &smsg);
}

