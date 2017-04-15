#include "skynet.h"

#include "skynet_module.h"
#include "spinlock.h"

#include <assert.h>
#include <string.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

//动态连接库 .so 的加载

//模块类型最大32,不代表模块所对应的服务最大数是32  一种模块可以对应多个服务
#define MAX_MODULE_TYPE 32


//该结构体用来管理加载的模块,就是struct skynet_module数组
struct modules {
			
	int count;		//已经加载的模块总数

	struct spinlock lock;	//锁

	//表示动态库的搜索路径,与lua的package语义一样
	const char * path;	//模块所在路径		

	//用来缓存skynet_module,最大为32个，  MAX_MODULE_TYPE 32
	struct skynet_module m[MAX_MODULE_TYPE];	//模块数组,存放动态库的数组
};


//管理加载的模块,内部有一个数组
static struct modules * M = NULL;


//尝试打开一个模块(动态库)    name是动态库名称
static void *
_try_open(struct modules *m, const char * name) {
	const char *l;

	//搜素路径
	const char * path = m->path;
	
	size_t path_size = strlen(path);
	size_t name_size = strlen(name);

	int sz = path_size + name_size;
	
	//search path
	void * dl = NULL;
	char tmp[sz];
	
	do
	{
		memset(tmp,0,sz);
		while (*path == ';') path++;

		//如果到了结尾
		if (*path == '\0') break;

		//查询;位置
		l = strchr(path, ';');

		if (l == NULL) 
			l = path + strlen(path);

		int len = l - path;
		int i;
		for (i=0;path[i]!='?' && i < len ;i++) {
			tmp[i] = path[i];
		}
		//主要是对path这个搜索路径的解析。path可以定义一组路径模板,每个路径使用;隔开
		//如"./server/?.so;./http/?.so"
		//如果模块名为foo,那么就会解析为,./server/foo.so;./http/foo.so
		//然后用这两个路径依次调用 dlopen,直到有成功加载的

		//路径+库名
		memcpy(tmp+i,name,name_size);
		
		if (path[i] == '?') {
			strncpy(tmp+i+name_size,path+i+1,len - i - 1);
		} else {
			fprintf(stderr,"Invalid C service path\n");
			exit(1);
		}

		//dlopen()打开一个动态链接库，并返回动态链接库的句柄
		dl = dlopen(tmp, RTLD_NOW | RTLD_GLOBAL);
		
		path = l;
	}while(dl == NULL);

	if (dl == NULL) {
		fprintf(stderr, "try open %s failed : %s\n",name,dlerror());
	}

	return dl;
}

//从struct modules中查询
//根据动态库名称在数组中查找
static struct skynet_module * 
_query(const char * name) {
	int i;
	for (i=0;i<M->count;i++) {
		if (strcmp(M->m[i].name,name)==0) {
			return &M->m[i];
		}
	}
	return NULL;
}

//初始化XXX_create,xxx_init,xxx_release地址
//成功返回0,失败返回1
//_open_sym是调用dlsym获取四个契约函数的指针，从实现可以知道，契约函数的命名规则为
// 模块名_函数名
//最后将sm存入m


static int
_open_sym(struct skynet_module *mod) {
	size_t name_size = strlen(mod->name);

	// 模块名_函数名
	char tmp[name_size + 9]; // create/init/release/signal , longest name is release (7)
	memcpy(tmp, mod->name, name_size);
	strcpy(tmp+name_size, "_create");

	//dlsym  根据名称加载函数，得到函数指针
	mod->create = dlsym(mod->module, tmp);

	strcpy(tmp+name_size, "_init");

	mod->init = dlsym(mod->module, tmp);

	strcpy(tmp+name_size, "_release");

	// dlsym()根据动态链接库操作句柄与符号，返回符号对应的地址

	mod->release = dlsym(mod->module, tmp);
	strcpy(tmp+name_size, "_signal");
	mod->signal = dlsym(mod->module, tmp);

	//库中一定要有xxxx_init() 函数
	return mod->init == NULL;
}


//根据名称查找模块，如果没有找到，则加载该模块
struct skynet_module * 
skynet_module_query(const char * name) {

	//查找模块
	//从缓存开始查找,使用了自旋锁保证线程安全
	struct skynet_module * result = _query(name);
	if (result)
		return result;

	//加锁
	SPIN_LOCK(M)

	//又查找了一次缓存，因为可能在锁竞争的时候，其他线程加载了一个同样的so
	result = _query(name); // double check

	//如果没有这个模块，测试打开这个模块
	if (result == NULL && M->count < MAX_MODULE_TYPE) {
		int index = M->count;

			//加载动态链接库
		void * dl = _try_open(M,name);


		//将打开的模块结构体指针存储全局变量 M 中	
		if (dl) {
			M->m[index].name = name;
			M->m[index].module = dl;


		// 初始化xxx_create xxx_init xxx_release 地址

			//_open_sym是调用dlsym获取四个契约函数的指针，从实现可以知道，契约函数的命名规则为
			// 模块名_函数名
			//最后将 skynet_module 存入m

			//会初始化skynet_modul的值
			if (_open_sym(&M->m[index]) == 0) {
				M->m[index].name = skynet_strdup(name);
				M->count ++;
				result = &M->m[index];
			}
		}
	}

	SPIN_UNLOCK(M)

	return result;
}

//插入模块到数组中
void 
skynet_module_insert(struct skynet_module *mod) {
	SPIN_LOCK(M)

	//先根据名字查找已经加载的动态库
	struct skynet_module * m = _query(mod->name);

	//不存在这个模块
	assert(m == NULL && M->count < MAX_MODULE_TYPE);
	int index = M->count;

	//将这个模块放到对应位置
	M->m[index] = *mod;
	++M->count;

	SPIN_UNLOCK(M)
}

//,如果存在，则调用动态库中的create函数
void * 
skynet_module_instance_create(struct skynet_module *m) {
	if (m->create) {
		return m->create();
	} else {
		//intptr_t对于32为环境是int,对于64位环境是long int
		//C99规定intptr_t可以保存指针值，因而将(~0)先转换为intptr_t再转换为void*
		return (void *)(intptr_t)(~0);
	}
}

//调用动态库中的init函数
int
skynet_module_instance_init(struct skynet_module *m, void * inst, struct skynet_context *ctx, const char * parm) {
	return m->init(inst, ctx, parm);
}

//如果存在 调用动态库中的release函数
void 
skynet_module_instance_release(struct skynet_module *m, void *inst) {
	if (m->release) {
		m->release(inst);
	}
}

//如果存在,调用动态库中的signal函数
void
skynet_module_instance_signal(struct skynet_module *m, void *inst, int signal) {
	if (m->signal) {
		m->signal(inst, signal);
	}
}

//初始化全局的modules,实质就是有一model数组
void 
skynet_module_init(const char *path) {
	struct modules *m = skynet_malloc(sizeof(*m));
	m->count = 0;

	//初始化动态库加载路径
	m->path = skynet_strdup(path);	//初始化模块所在路径

	//初始化锁
	SPIN_INIT(m)

	M = m;
}

