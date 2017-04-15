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



//��ȡ�����������,��int���͵ı������뵽struct skynet_env *E ��lua stateȫ�ֱ���
static int
optint(const char *key, int opt) {
	//���ȴӴ�����ȫ�ֱ���struct skynet_env *E��ջ�л�ȡkey
	//����ȫ�ֱ��л�ȡkey����
	const char * str = skynet_getenv(key);

	//���ȫ�ֱ��л�������key����
	if (str == NULL) {
		char tmp[20];
		sprintf(tmp,"%d",opt);
		//��key��������ȫ�ֱ���,ֵΪtmp
		skynet_setenv(key, tmp);
		return opt;
	}
	return strtol(str, NULL, 10);
}


//��ȡ�����������,��boolean���͵ı������뵽struct skynet_env *E ��lua stateȫ�ֱ���
static int
optboolean(const char *key, int opt) {
	const char * str = skynet_getenv(key);
	if (str == NULL) {
		skynet_setenv(key, opt ? "true" : "false");
		return opt;
	}
	return strcmp(str,"true")==0;
}


//��ȡ�����������,��char *���͵ı������뵽struct skynet_env *E ��lua stateȫ�ֱ���

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

	while (lua_next(L, -2) != 0) {	//����key����ȫ�ֱ������е�ȫ�ֱ�����ֲ�����ѹ��ջ
		
		int keyt = lua_type(L, -2);		//��ȡkey���� 
		
		if (keyt != LUA_TSTRING) {
			fprintf(stderr, "Invalid config table\n");
			exit(1);
		}
		
		const char * key = lua_tostring(L,-2);	//��ȡkey

		//ֵ����
		if (lua_type(L,-1) == LUA_TBOOLEAN) {
			int b = lua_toboolean(L,-1);
			skynet_setenv(key,b ? "true" : "false" );
			
		} else {
			const char * value = lua_tostring(L,-1);	//��ȡvalue
			if (value == NULL) {
				fprintf(stderr, "Invalid config table key = %s\n", key);
				exit(1);
			}
			skynet_setenv(key,value);
		}

		//����value,����key,�Ա�����һ�ε���
		lua_pop(L,1);
	}

	//����ȫ�ֱ�����
	lua_pop(L,1);
}


int sigign() {
	struct sigaction sa;

	sa.sa_handler = SIG_IGN;
	//�����ź� 
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

	//��ʼ��lua����
	luaS_initshr();


	//��ʼ��skynet_node
	//ȫ�ֳ�ʼ������Ҫ������Ϊ�߳��������ݴ���һ��Key,ʹ��pthread_create()����
	//��ʹ��pthread_setspecifig()����������������key��valueֵ
	skynet_globalinit();

	//��ʼ�� struct skynet_env ,��ʼ��lua����
	/*
		struct skynet_env {
		
			struct spinlock lock;
			lua_State *L;
		};

	*/
	//��Ҫ����һ��ȫ�ֵ����ݽṹstruct skynet_env *E,����ʼ���ṹ��ֵ  E->L = luaL_newstate();
	skynet_env_init();

	//����SIGPIPE
	sigign();

	//����������Ľṹ��
	struct skynet_config config;

	//��lua��صĳ�ʼ�� 
	//�½�lua״̬��
	struct lua_State *L = luaL_newstate();

	//��lua��׼��
	luaL_openlibs(L);	// link lua lib

	//luaL_loadstring����������load_config����
	int err = luaL_loadstring(L, load_config);
	
	assert(err == LUA_OK);
	
	//config_file  �����ļ���,����ѹ�뵽ջ��
	lua_pushstring(L, config_file);

	//lua_pcall(lua_State *L, int nargs, int nresults, int errfunc)
	//ִ�м��ص�lua�ű��ַ������⽫�����config_file�����lua�ű�����Skynet����
	err = lua_pcall(L, 1, 1, 0);
	if (err) {
		fprintf(stderr,"%s\n",lua_tostring(L,-1));
		lua_close(L);
		return 1;
	}

	//��ʼ�� lua ����
	//����Skynet���ýű���key��value,������ΪLua����������������ö�ӦKey��ֵ��
	//struct skynet_config config�ṹ�У�����skynet_start()
	//����ʱ�������ò���
	_init_env(L);

	//����������,��ȫ�ֱ��м��أ����������,�Ὣ���������skynet_env_init();��luaȫ�ֱ��� ,,��ʼ��config������,
	config.thread =  optint("thread",8);

	//ģ��(��̬��)���ص�·��
	config.module_path = optstring("cpath","./cservice/?.so");

	config.harbor = optint("harbor", 1);
	config.bootstrap = optstring("bootstrap","snlua bootstrap");
	config.daemon = optstring("daemon", NULL);
	config.logger = optstring("logger", NULL);

	//�Ὣlogservrece:logger���뵽������
	//���ص���   "logger"
	config.logservice = optstring("logservice", "logger");

	config.profile = optboolean("profile", 1);


//�رմ�����Lua״̬��
	lua_close(L);


	//�������ò���������Skynet�ĸ���������߳�
	//�������������skynet_start.c�ļ���
	skynet_start(&config);


	//�����߳�ȫ�ֱ���
	//��Ӧ�����skynet_globalinit(),����ɾ���̴߳洢key
	skynet_globalexit();

	luaS_exitshr();

	return 0;
}
