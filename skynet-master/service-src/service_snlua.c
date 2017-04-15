#include "skynet.h"

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define MEMORY_WARNING_REPORT (1024 * 1024 * 32)

//skynet-lua��������snlua,
/*
������ɵ�����
1.����һ��lua�����
2.����ִ��һ��lua����������ű�
3.���ص����������ݴ��ݸ�lua��

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
	//���������ҵ�Ŀ�ĵ� skynet_context��������Ϣ
	skynet_sendname(ctx, 0, ".launcher", PTYPE_TEXT, 0, "ERROR", 5);
}

//��ȡ�������� key��Ӧ��ֵ
static const char *
optstring(struct skynet_context *ctx, const char *key, const char * str) {
	const char * ret = skynet_command(ctx, "GETENV", key);
	if (ret == NULL) {
		return str;
	}
	return ret;
}

//��ʼ������
//����һЩ��������������󣬾ͼ���ִ����loader.lua�ļ���ͬʱ������Ҫ���ص��ļ�
//���ʱ����bootsrap��Ϊ�������ݸ���,����Ȩ�Ϳ�ʼת��lua��
//loader.lua����������lua�ļ���,��loader.lua�л��ж��Ƿ���Ҫpreload,���ջ����ִ��bootsrap.lua�ļ�

//skynet_context�Ļص������������������
static int
init_cb(struct snlua *l, struct skynet_context *ctx, const char * args, size_t sz) {

	lua_State *L = l->L;
	l->ctx = ctx;

	//�ú������������ռ���,LUA_GCSTOP��ʾ ֹͣ�����ռ���
	lua_gc(L, LUA_GCSTOP, 0);
	
	lua_pushboolean(L, 1);  /* signal for libraries to ignore env. vars. */

	//����ע�����  �൱�� LUA_NOENV=1  LUA_REGISTRYINDEXע���λ�ã���Ԫ��
	//lua_setfield����������Ԫ�� 
	lua_setfield(L, LUA_REGISTRYINDEX, "LUA_NOENV");

	luaL_openlibs(L);

	//������usrdata,��ָ����ջ
	lua_pushlightuserdata(L, ctx);


	//����ע���  {skynet_context=ctx}
	lua_setfield(L, LUA_REGISTRYINDEX, "skynet_context");
	
	luaL_requiref(L, "skynet.codecache", codecache , 0);

	//��ջ�е���һ��Ԫ��
	lua_pop(L,1);

	const char *path = optstring(ctx, "lua_path","./lualib/?.lua;./lualib/?/init.lua");
	lua_pushstring(L, path);

	//���ø���luaȫ�ֱ���  LUA_PATH LUA_CPATH LUA_SERVICE LUA_PRELOAD
	
	 // #define lua_setglobal(L,s)  lua_setfield(L, LUA_GLOBALSINDEX, s)
	 
	 //��ջ�е��� path,����ȫ�ֱ��� LUA_PATH=./lualib/?.lua;./lualib/?/init.lua
	lua_setglobal(L, "LUA_PATH");
	
	const char *cpath = optstring(ctx, "lua_cpath","./luaclib/?.so");
	lua_pushstring(L, cpath);

	 //��ջ�е��� cpath,����ȫ�ֱ��� LUA_CPATH=./luaclib/?.so
	lua_setglobal(L, "LUA_CPATH");
	const char *service = optstring(ctx, "luaservice", "./service/?.lua");
	lua_pushstring(L, service);

	 //��ջ�е��� service,����ȫ�ֱ��� LUA_SERVICE=./service/?.lua
	lua_setglobal(L, "LUA_SERVICE");

	// skynet_command �����������Ʋ��Ҷ�Ӧ�Ļص����������ҵ���
	//Ϊ��ȡpreload
	const char *preload = skynet_command(ctx, "GETENV", "preload");

	lua_pushstring(L, preload);

	 //��ջ�е��� preload,����ȫ�ֱ��� LUA_PRELOAD=
	lua_setglobal(L, "LUA_PRELOAD");
	//����֮ǰ������sc(��skynet_context)������������(config���lua_path,lua_cpath,lua_server��)

	//��c������ջ
	lua_pushcfunction(L, traceback);

	//����ջ��Ԫ�ص�������������ֵ����վ��Ԫ�صĸ���,����ջ��ֻ��һ��Ԫ��
	assert(lua_gettop(L) == 1);

	const char * loader = optstring(ctx, "lualoader", "./lualib/loader.lua");

	//��bootsrapΪ������ִ��lualib/loader.lua:  loader���������� cmd����Ϊ��ȥ�������Ŀ¼����lua�ļ�
	//�ҵ���loadfile��ִ��(��Ч��dofile)  
	//ֻ���룬������  dofile���д���
	//�Ὣ����������Ϊһ�����������������ģ�ѹ��ջ��

	//loader���� ./lualib/loader.lua,�ú��������lua�ļ�
	//��һ������õĴ������Ϊһ�� Lua ����ѹ��ջ��
	int r = luaL_loadfile(L,loader);
	
	if (r != LUA_OK) {
		skynet_error(ctx, "Can't load %s : %s", loader, lua_tostring(L, -1));
		report_launcher_error(ctx);
		return 1;
	}

	//ѹ����� 
						//boostrap
	lua_pushlstring(L, args, sz);

	//��bootsrapΪ������ִ��lualib/loader.lua
	r = lua_pcall(L,1,0,1);

	if (r != LUA_OK) {
		skynet_error(ctx, "lua loader error : %s", lua_tostring(L, -1));
		report_launcher_error(ctx);
		return 1;
	}
	lua_settop(L,0);
//����֮ǰ��������һ��lua�ļ������ű���������������Щ�������������ڽű�,���������������
/*
	lua_service:lua����(actor)���ڵ�·������lua_path������һ��,��ռ��lua�����lua_path,����һ��С
	��Ϊ��������������һ���ļ���������һ��Ŀ¼��ʱ�����Ŀ¼����lua_path,Ҳ��������·��Ϊ:
	/?/main.lua,������Ϊfoo,���/foo/?.lua����lua_path������Ϊ�˲�ͬ����֮��ĽŲ����ط����㡣
	//����������init_cb��args,args[0]Ϊ������������Ϊ��ڽű��Ĳ���


*/

/*
	lua_path,lua_cpath��lua���������һ�£����������������Ǹ�ֵ��package.path,package.cpath
	lua_preload:���ָ��������������ڽű�ǰ��������
	֮����Ҫ��һ��lua�����������������ڽű�����Ҫ��Ϊ��ʵ���������㣬��������󻹻�д������ȫ�ֱ���
	:SERVICE_NAME(������),SERVICE_PATH(����Ŀ¼)
*/



//������:�����ű���û������memlimit(vm�ڴ�����)������У������õ�snlua_ud,����vm�ڷ����ڴ�ʱ�Ϳ���
//���жϡ��������Ҳֻ���������ʱ�������ã���Ϊ����snlua�ʹ����ˣ���Ҳ�ز������������
	if (lua_getfield(L, LUA_REGISTRYINDEX, "memlimit") == LUA_TNUMBER) {
		size_t limit = lua_tointeger(L, -1);
		l->mem_limit = limit;
		skynet_error(ctx, "Set memory limit to %.2f M", (float)limit / (1024 * 1024));
		lua_pushnil(L);
		lua_setfield(L, LUA_REGISTRYINDEX, "memlimit");
	}
	lua_pop(L, 1);

	lua_gc(L, LUA_GCRESTART, 0);


//���ˣ�һ��lua����͵������У���ڽű�ֻ������skynet��lua apiע��һ�λص�������
//��ô�Ϳ��Խӹ���Ϣ������
	return 0;
}


//snlua_init��ע��� �ص�����
//��Ϣ����ʱ�����
static int
launch_cb(struct skynet_context * context, void *ud, int type, int session, uint32_t source , const void * msg, size_t sz) {
	assert(type == 0 && session == 0);
	struct snlua *l = ud;

	//���ص�����ָ������ΪNULL,���Լ��Ļص�������ע���ˣ�ʹ�����ڽ�����Ϣ��Ϊ������lua������ע����
	//����Ϣͨ��lua�ӿ�������
	skynet_callback(context, NULL, NULL);

	//���ó�ʼ������init_cb()
	int err = init_cb(l, context, msg, sz);
	
	if (err) {
		skynet_command(context, "EXIT", NULL);
	}

	return 0;
}

//ע��ص���Ȼ��ʲôҲû�����������ĳ�ʼ���Ƴٵ��˵�һ����Ϣ�Ĵ���������
//��ʼ��ģ���Ӧ��skynet_context�Ļص�����ָ��
//��skyney_context_new()�б�����
int																//boostrap
snlua_init(struct snlua *l, struct skynet_context *ctx, const char * args) {
	int sz = strlen(args);
	char * tmp = skynet_malloc(sz);
	memcpy(tmp, args, sz);

	//��ʼ��struct skynet_context�Ļص�����ָ�룬�ص���������
	//��ע��ص�,lΪ���� 
	skynet_callback(ctx, l , launch_cb);

	//����"REG"ִ�к���,Ϊctx���������Ϊnull,���������������ǻ�ȡhandle���ַ�����ʽ 
	//self�Ƕ��ڵ�skynet_context�е�handle
	const char * self = skynet_command(ctx, "REG", NULL);

	//���ctx��Ӧ�� handle
	uint32_t handle_id = strtoul(self+1, NULL, 16);

//skynet_send(struct skynet_context * context, 
//                 uint32_t source, uint32_t destination , int type, int session, void * data, size_t sz)

	// it must be first message
	//������Ϣ,���Լ���ģ���Ӧ��skynet_context����Ϣ�����У�������Ϣʱ������ûص�����
	skynet_send(ctx, 0, handle_id, PTYPE_TAG_DONTCOPY,0, tmp, sz);
	return 0;
}

//������lua������е��ڴ������õĺ���
//���ж��ڴ�����
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


//����һ��lua vm(�����),��ע�����Լ��ķ�������Ϊ��ʹ��jemalloc,��ȡvm���ڴ���������vm�ڴ�����
//��skynet_context_new()�����б����ã�
//�൱��ÿ�μ���һ����̬�⣬�������xxx_create()����������xxx_init()����
struct snlua *
snlua_create(void) {
	struct snlua * l = skynet_malloc(sizeof(*l));
	memset(l,0,sizeof(*l));
	l->mem_report = MEMORY_WARNING_REPORT;
	l->mem_limit = 0;

	//���������vm     lalloc���ڴ���亯��  l��ÿ�ε���lalloc�Ǵ����һ������
	l->L = lua_newstate(lalloc, l);
	return l;
}


//���������
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



