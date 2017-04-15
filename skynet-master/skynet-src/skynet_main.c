#include "skynet.h"

#include "skynet_imp.h"
#include "skynet_env.h"
#include "skynet_server.h"
#include "luashrtbl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include <signal.h>
#include <assert.h>



//获取，如果不存在,将int类型的变量存入到struct skynet_env *E 的lua state全局表中
static int
optint(const char *key, int opt) {
	//首先从创建的全局变量struct skynet_env *E的栈中获取key
	//即从全局表中获取key变量
	const char * str = skynet_getenv(key);

	//如果全局表中还不存在key变量
	if (str == NULL) {
		char tmp[20];
		sprintf(tmp,"%d",opt);
		//将key变量存入全局表中,值为tmp
		skynet_setenv(key, tmp);
		return opt;
	}
	return strtol(str, NULL, 10);
}


//获取，如果不存在,将boolean类型的变量存入到struct skynet_env *E 的lua state全局表中
static int
optboolean(const char *key, int opt) {
	const char * str = skynet_getenv(key);
	if (str == NULL) {
		skynet_setenv(key, opt ? "true" : "false");
		return opt;
	}
	return strcmp(str,"true")==0;
}


//获取，如果不存在,将char *类型的变量存入到struct skynet_env *E 的lua state全局表中

static const char *
optstring(const char *key,const char * opt) {
	const char * str = skynet_getenv(key);
	if (str == NULL) {
		if (opt) {
			skynet_setenv(key, opt);
			opt = skynet_getenv(key);
		}
		return opt;
	}
	return str;
}

static void
_init_env(lua_State *L) {
	
	lua_pushnil(L);  /* first key */

	while (lua_next(L, -2) != 0) {	//弹出key并将全局变量表中的全局变量与局部变量压入栈
		
		int keyt = lua_type(L, -2);		//获取key类型 
		
		if (keyt != LUA_TSTRING) {
			fprintf(stderr, "Invalid config table\n");
			exit(1);
		}
		
		const char * key = lua_tostring(L,-2);	//获取key

		//值类型
		if (lua_type(L,-1) == LUA_TBOOLEAN) {
			int b = lua_toboolean(L,-1);
			skynet_setenv(key,b ? "true" : "false" );
			
		} else {
			const char * value = lua_tostring(L,-1);	//获取value
			if (value == NULL) {
				fprintf(stderr, "Invalid config table key = %s\n", key);
				exit(1);
			}
			skynet_setenv(key,value);
		}

		//弹出value,保留key,以便于下一次迭代
		lua_pop(L,1);
	}

	//弹出全局变量表
	lua_pop(L,1);
}


int sigign() {
	struct sigaction sa;

	sa.sa_handler = SIG_IGN;
	//忽略信号 
	sigaction(SIGPIPE, &sa, 0);
	return 0;
}


static const char * load_config = "\
	local config_name = ...\
	local f = assert(io.open(config_name))\
	local code = assert(f:read \'*a\')\
	local function getenv(name) return assert(os.getenv(name), \'os.getenv() failed: \' .. name) end\
	code = string.gsub(code, \'%$([%w_%d]+)\', getenv)\
	f:close()\
	local result = {}\
	assert(load(code,\'=(load)\',\'t\',result))()\
	return result\
";

int
main(int argc, char *argv[]) {
	const char * config_file = NULL ;
	if (argc > 1) {
		config_file = argv[1];
	} else {
		fprintf(stderr, "Need a config file. Please read skynet wiki : https://github.com/cloudwu/skynet/wiki/Config\n"
			"usage: skynet configfilename\n");
		return 1;
	}

	//初始化lua环境
	luaS_initshr();


	//初始化skynet_node
	//全局初始化，主要功能是为线程特有数据创建一个Key,使用pthread_create()函数
	//并使用pthread_setspecifig()函数设置特有数据key的value值
	skynet_globalinit();

	//初始化 struct skynet_env ,初始化lua环境
	/*
		struct skynet_env {
		
			struct spinlock lock;
			lua_State *L;
		};

	*/
	//主要创建一个全局的数据结构struct skynet_env *E,并初始化结构的值  E->L = luaL_newstate();
	skynet_env_init();

	//忽略SIGPIPE
	sigign();

	//包含配置项的结构体
	struct skynet_config config;

	//与lua相关的初始化 
	//新建lua状态机
	struct lua_State *L = luaL_newstate();

	//打开lua标准库
	luaL_openlibs(L);	// link lua lib

	//luaL_loadstring函数，加载load_config代码
	int err = luaL_loadstring(L, load_config);
	
	assert(err == LUA_OK);
	
	//config_file  配置文件名,将其压入到栈中
	lua_pushstring(L, config_file);

	//lua_pcall(lua_State *L, int nargs, int nresults, int errfunc)
	//执行加载的lua脚本字符串，这将会加载config_file定义的lua脚本用于Skynet配置
	err = lua_pcall(L, 1, 1, 0);
	if (err) {
		fprintf(stderr,"%s\n",lua_tostring(L,-1));
		lua_close(L);
		return 1;
	}

	//初始化 lua 环境
	//弹出Skynet配置脚本的key和value,并设置为Lua环境变量，最后设置对应Key的值到
	//struct skynet_config config结构中，方便skynet_start()
	//调用时传入配置参数
	_init_env(L);

	//加载配置项,从全局表中加载，如果不存在,会将变量存入的skynet_env_init();的lua全局表中 ,,初始化config的属性,
	config.thread =  optint("thread",8);

	//模块(动态库)加载的路径
	config.module_path = optstring("cpath","./cservice/?.so");

	config.harbor = optint("harbor", 1);
	config.bootstrap = optstring("bootstrap","snlua bootstrap");
	config.daemon = optstring("daemon", NULL);
	config.logger = optstring("logger", NULL);

	//会将logservrece:logger存入到环境中
	//返回的是   "logger"
	config.logservice = optstring("logservice", "logger");

	config.profile = optboolean("profile", 1);


//关闭创建的Lua状态机
	lua_close(L);


	//传入配置参数并启动Skynet的各个组件和线程
	//这个函数定义在skynet_start.c文件中
	skynet_start(&config);


	//销毁线程全局变量
	//对应上面的skynet_globalinit(),用于删除线程存储key
	skynet_globalexit();

	luaS_exitshr();

	return 0;
}
