#ifndef SKYNET_IMP_H
#define SKYNET_IMP_H


//skynet 的配置 信息
struct skynet_config {

	int thread;		//线程数
	int harbor;		//harbor
	int profile;	
	const char * daemon;
	const char * module_path;  // 模块 即服务的路径 .so文件路径
	const char * bootstrap;
	
	const char * logger;	//日志服务
	const char * logservice;	
};

#define THREAD_WORKER 0
#define THREAD_MAIN 1
#define THREAD_SOCKET 2
#define THREAD_TIMER 3
#define THREAD_MONITOR 4

void skynet_start(struct skynet_config * config);

#endif
