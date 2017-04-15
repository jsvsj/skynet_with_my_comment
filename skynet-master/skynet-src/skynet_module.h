#ifndef SKYNET_MODULE_H
#define SKYNET_MODULE_H

struct skynet_context;

typedef void * (*skynet_dl_create)(void);
typedef int (*skynet_dl_init)(void * inst, struct skynet_context *, const char * parm);
typedef void (*skynet_dl_release)(void * inst);
typedef void (*skynet_dl_signal)(void * inst, int signal);


//管理加载的 动态库的结构体 
struct skynet_module {
			
	const char * name;	//模块名称,动态库名

	//动态库句柄
	void * module;		//用于保存dlopen返回的handle

	//契约函数之一，用来创建用户对象
	//实质就是函数指针，动态库中符合类型的函数指针才能被加载，调用
	skynet_dl_create create;	//用于保存xxx_create函数入口地址

	//用来初始化对象
	skynet_dl_init init;		//用于保存xxx_init函数入口地址

	//用来释放对象
	skynet_dl_release release;	//用于保存xxx_release函数入口地址

	//用来实现信号功能
	skynet_dl_signal signal;	//用于保存xxx_signal函数入口地址
};	

void skynet_module_insert(struct skynet_module *mod);
struct skynet_module * skynet_module_query(const char * name);
void * skynet_module_instance_create(struct skynet_module *);
int skynet_module_instance_init(struct skynet_module *, void * inst, struct skynet_context *ctx, const char * parm);
void skynet_module_instance_release(struct skynet_module *, void *inst);
void skynet_module_instance_signal(struct skynet_module *, void *inst, int signal);

void skynet_module_init(const char *path);

#endif

