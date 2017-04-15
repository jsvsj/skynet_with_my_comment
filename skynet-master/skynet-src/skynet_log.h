#ifndef skynet_log_h
#define skynet_log_h

#include "skynet_env.h"
#include "skynet.h"

#include <stdio.h>
#include <stdint.h>

//打开日志文件
FILE * skynet_log_open(struct skynet_context * ctx, uint32_t handle);

//关闭日志文件
void skynet_log_close(struct skynet_context * ctx, FILE *f, uint32_t handle);

//打印日志
void skynet_log_output(FILE *f, uint32_t source, int type, int session, void * buffer, size_t sz);

#endif