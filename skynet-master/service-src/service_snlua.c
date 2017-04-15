#include "skynet.h"

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define MEMORY_WARNING_REPORT (1024 * 1024 * 32)

//skynet-lua的宿主是snlua,
/*
宿主完成的任务
1.创建一个lua虚拟机
2.加载执行一个lua服务的启动脚本
3.将回调函数的数据传递给lua层

*/

struct snlua {
	lua_State * L;
	struct skynet_context * ctx;
	size_t mem;
	size_t mem_report;
	size_t mem_limit;
};

// LUA_CACHELIB may defined in patched lua for shared proto
#ifdef LUA_CACHELIB

#define codecache luaopen_cache

#else

static int
cleardummy(lua_State *L) {
  return 0;
}

static int 
codecache(lua_State *L) {
	luaL_Reg l[] = {
		{ "clear", cleardummy },
		{ "mode", cleardummy },
		{ NULL, NULL },
	};
	luaL_newlib(L,l);
	lua_getglobal(L, "loadfile");
	lua_setfield(L, -2, "loadfile");
	return 1;
}

#endif

static int 
traceback (lua_State *L) {
	const char *msg = lua_tostring(L, 1);
	if (msg)
		luaL_traceback(L, L, msg, 1);
	else {
		lua_pushliteral(L, "(no error message)");
	}
	return 1;
}

static void
report_launcher_error(struct skynet_context *ctx) {
	// sizeof "ERROR" == 5
	//根据名字找到目的地 skynet_context，发送消息
	skynet_sendname(ctx, 0, ".launcher", PTYPE_TEXT, 0, "ERROR", 5);
}

//获取环境变量 key对应的值
static const char *
optstring(struct skynet_context *ctx, const char *key, const char * str) {
	const char * ret = skynet_command(ctx, "GETENV", key);
	if (ret == NULL) {
		return str;
	}
	return ret;
}

//初始化函数
//设置一些虚拟机环境变量后，就加载执行了loader.lua文件，同时把真正要加载的文件
//这个时候是bootsrap作为参数传递给他,控制权就开始转到lua层
//loader.lua是用来加载lua文件的,在loader.lua中会判断是否需要preload,最终会加载执行bootsrap.lua文件

//skynet_context的回调函数调用了这个函数
static int
init_cb(struct snlua *l, struct skynet_context *ctx, const char * args, size_t sz) {

	lua_State *L = l->L;
	l->ctx = ctx;

	//该函数控制垃圾收集器,LUA_GCSTOP表示 停止垃圾收集器
	lua_gc(L, LUA_GCSTOP, 0);
	
	lua_pushboolean(L, 1);  /* signal for libraries to ignore env. vars. */

	//设置注册表中  相当于 LUA_NOENV=1  LUA_REGISTRYINDEX注册表位置，是元表
	//lua_setfield是用来设置元表 
	lua_setfield(L, LUA_REGISTRYINDEX, "LUA_NOENV");

	luaL_openlibs(L);

	//轻量级usrdata,将指针入栈
	lua_pushlightuserdata(L, ctx);


	//设置注册表  {skynet_context=ctx}
	lua_setfield(L, LUA_REGISTRYINDEX, "skynet_context");
	
	luaL_requiref(L, "skynet.codecache", codecache , 0);

	//从栈中弹出一个元素
	lua_pop(L,1);

	const char *path = optstring(ctx, "lua_path","./lualib/?.lua;./lualib/?/init.lua");
	lua_pushstring(L, path);

	//设置各项lua全局变量  LUA_PATH LUA_CPATH LUA_SERVICE LUA_PRELOAD
	
	 // #define lua_setglobal(L,s)  lua_setfield(L, LUA_GLOBALSINDEX, s)
	 
	 //从栈中弹出 path,设置全局变量 LUA_PATH=./lualib/?.lua;./lualib/?/init.lua
	lua_setglobal(L, "LUA_PATH");
	
	const char *cpath = optstring(ctx, "lua_cpath","./luaclib/?.so");
	lua_pushstring(L, cpath);

	 //从栈中弹出 cpath,设置全局变量 LUA_CPATH=./luaclib/?.so
	lua_setglobal(L, "LUA_CPATH");
	const char *service = optstring(ctx, "luaservice", "./service/?.lua");
	lua_pushstring(L, service);

	 //从栈中弹出 service,设置全局变量 LUA_SERVICE=./service/?.lua
	lua_setglobal(L, "LUA_SERVICE");

	// skynet_command 根据命令名称查找对应的回调函数，并且调用
	//为获取preload
	const char *preload = skynet_command(ctx, "GETENV", "preload");

	lua_pushstring(L, preload);

	 //从栈中弹出 preload,设置全局变量 LUA_PRELOAD=
	lua_setglobal(L, "LUA_PRELOAD");
	//在这之前，保存sc(即skynet_context)，加载配置项(config里的lua_path,lua_cpath,lua_server等)

	//将c函数入栈
	lua_pushcfunction(L, traceback);

	//返回栈定元素的索引，该索引值就是站中元素的个数,断言栈中只有一个元素
	assert(lua_gettop(L) == 1);

	const char * loader = optstring(ctx, "lualoader", "./lualib/loader.lua");

	//以bootsrap为参数，执行lualib/loader.lua:  loader的做用是以 cmd参数为名去各项代码目录查找lua文件
	//找到后loadfile并执行(等效于dofile)  
	//只编译，不运行  dofile运行代码
	//会将编译后代码作为一个类似于匿名函数的，压入栈顶

	//loader就是 ./lualib/loader.lua,该函数编译该lua文件
	//把一个编译好的代码块作为一个 Lua 函数压到栈顶
	int r = luaL_loadfile(L,loader);
	
	if (r != LUA_OK) {
		skynet_error(ctx, "Can't load %s : %s", loader, lua_tostring(L, -1));
		report_launcher_error(ctx);
		return 1;
	}

	//压入参数 
						//boostrap
	lua_pushlstring(L, args, sz);

	//以bootsrap为参数，执行lualib/loader.lua
	r = lua_pcall(L,1,0,1);

	if (r != LUA_OK) {
		skynet_error(ctx, "lua loader error : %s", lua_tostring(L, -1));
		report_launcher_error(ctx);
		return 1;
	}
	lua_settop(L,0);
//在这之前，加载了一个lua的加载器脚本，用它来设置这些配置项，并运行入口脚本,各个配置项含义如下
/*
	lua_service:lua服务(actor)所在的路径，与lua_path的语义一样,不占用lua本身的lua_path,但有一个小
	行为，当服务名不是一个文件名，而是一个目录名时，会把目录加入lua_path,也就是搜索路径为:
	/?/main.lua,服务名为foo,会把/foo/?.lua加入lua_path。这样为了不同服务之间的脚步加载发方便。
	//服务名来自init_cb的args,args[0]为服务名，后续为入口脚本的参数


*/

/*
	lua_path,lua_cpath与lua本身的语义一致，加载器仅仅将他们赋值给package.path,package.cpath
	lua_preload:如果指定，则在运行入口脚本前先运行他
	之所以要用一个lua加载器来间接运行入口脚本，主要是为了实现起来方便，加载器最后还会写入两个全局变量
	:SERVICE_NAME(服务名),SERVICE_PATH(服务目录)
*/



//第三步:看看脚本有没有设置memlimit(vm内存上限)，如果有，就设置到snlua_ud,这样vm在分配内存时就可以
//做判断。这个参数也只可能在这个时机里设置，因为后面snlua就打酱油了，再也回不到它的领空了
	if (lua_getfield(L, LUA_REGISTRYINDEX, "memlimit") == LUA_TNUMBER) {
		size_t limit = lua_tointeger(L, -1);
		l->mem_limit = limit;
		skynet_error(ctx, "Set memory limit to %.2f M", (float)limit / (1024 * 1024));
		lua_pushnil(L);
		lua_setfield(L, LUA_REGISTRYINDEX, "memlimit");
	}
	lua_pop(L, 1);

	lua_gc(L, LUA_GCRESTART, 0);


//至此，一个lua服务就得以运行，入口脚本只需利用skynet的lua api注册一次回调函数，
//那么就可以接管消息处理了
	return 0;
}


//snlua_init中注册的 回调函数
//消息到来时会调用
static int
launch_cb(struct skynet_context * context, void *ud, int type, int session, uint32_t source , const void * msg, size_t sz) {
	assert(type == 0 && session == 0);
	struct snlua *l = ud;

	//将回调函数指针设置为NULL,把自己的回调函数给注销了，使他不在接受消息，为的是在lua层重新注册他
	//吧消息通过lua接口来接受
	skynet_callback(context, NULL, NULL);

	//调用初始化函数init_cb()
	int err = init_cb(l, context, msg, sz);
	
	if (err) {
		skynet_command(context, "EXIT", NULL);
	}

	return 0;
}

//注册回调，然后什么也没有做，真正的初始化推迟到了第一条消息的处理上做了
//初始化模块对应的skynet_context的回调函数指针
//在skyney_context_new()中被调用
int																//boostrap
snlua_init(struct snlua *l, struct skynet_context *ctx, const char * args) {
	int sz = strlen(args);
	char * tmp = skynet_malloc(sz);
	memcpy(tmp, args, sz);

	//初始化struct skynet_context的回调函数指针，回调函数参数
	//即注册回调,l为参数 
	skynet_callback(ctx, l , launch_cb);

	//根据"REG"执行函数,为ctx命名，如果为null,并不是命名，而是获取handle的字符串形式 
	//self是对于的skynet_context中的handle
	const char * self = skynet_command(ctx, "REG", NULL);

	//获得ctx对应的 handle
	uint32_t handle_id = strtoul(self+1, NULL, 16);

//skynet_send(struct skynet_context * context, 
//                 uint32_t source, uint32_t destination , int type, int session, void * data, size_t sz)

	// it must be first message
	//发送消息,到自己的模块对应的skynet_context的消息队列中，处理消息时，会调用回调函数
	skynet_send(ctx, 0, handle_id, PTYPE_TAG_DONTCOPY,0, tmp, sz);
	return 0;
}

//创建的lua虚拟机中的内存分配调用的函数
//会判断内存限制
static void *
lalloc(void * ud, void *ptr, size_t osize, size_t nsize) {

	struct snlua *l = ud;
	size_t mem = l->mem;
	l->mem += nsize;

	if (ptr)
		l->mem -= osize;
	if (l->mem_limit != 0 && l->mem > l->mem_limit) {
		if (ptr == NULL || nsize > osize) {
			l->mem = mem;
			return NULL;
		}
	}

	if (l->mem > l->mem_report) {
		l->mem_report *= 2;
		skynet_error(l->ctx, "Memory warning %.2f M", (float)l->mem / (1024 * 1024));
	}
	return skynet_lalloc(ptr, osize, nsize);
}


//创建一个lua vm(虚拟机),且注入了自己的分配器，为了使用jemalloc,获取vm的内存量，限制vm内存上限
//在skynet_context_new()函数中被调用，
//相当于每次加载一个动态库，都会调用xxx_create()函数，还有xxx_init()函数
struct snlua *
snlua_create(void) {
	struct snlua * l = skynet_malloc(sizeof(*l));
	memset(l,0,sizeof(*l));
	l->mem_report = MEMORY_WARNING_REPORT;
	l->mem_limit = 0;

	//创建虚拟机vm     lalloc是内存分配函数  l是每次调用lalloc是传入的一个参数
	l->L = lua_newstate(lalloc, l);
	return l;
}


//销毁虚拟机
void
snlua_release(struct snlua *l) {
	lua_close(l->L);
	skynet_free(l);
}

void
snlua_signal(struct snlua *l, int signal) {
	skynet_error(l->ctx, "recv a signal %d", signal);
	if (signal == 0) {
#ifdef lua_checksig
	// If our lua support signal (modified lua version by skynet), trigger it.
	skynet_sig_L = l->L;
#endif
	} else if (signal == 1) {
		skynet_error(l->ctx, "Current Memory %.3fK", (float)l->mem / 1024);
	}
}



