#include "skynet.h"
#include "skynet_env.h"
#include "spinlock.h"

#include <lua.h>
#include <lauxlib.h>

#include <stdlib.h>
#include <assert.h>

struct skynet_env {

	//使用原子操作实现的锁,或者是pthread_mutex_t,
	struct spinlock lock;
	
	lua_State *L;
};

//skynet 环境 配置，主要是获取lua的环境变量
static struct skynet_env *E = NULL;


//获取的是 lua 的全局变量 key 值
const char * 
skynet_getenv(const char *key) {

//#define SPIN_LOCK(q) spinlock_lock(&(q)->lock);
	//加锁
	SPIN_LOCK(E)

	lua_State *L = E->L;

	//获取lua全局变量key的值，并压入lua栈, 在栈顶
	lua_getglobal(L, key);

	//从lua栈中弹出该变量值，并赋值给result
	const char * result = lua_tostring(L, -1);

	//弹出该变量值
	lua_pop(L, 1);

	//解锁
	SPIN_UNLOCK(E)

	return result;
}

//设置lua全局变量
void 
skynet_setenv(const char *key, const char *value) {
	SPIN_LOCK(E)
	
	lua_State *L = E->L;

	//获取lua全局变量key的值，并压入lua栈
	lua_getglobal(L, key);

	//断言该变量值一定是空的
	assert(lua_isnil(L, -1));

	//弹出该变量值
	lua_pop(L,1);

	//将vaue压入lua栈
	lua_pushstring(L,value);

	//从lua栈中弹出value,将lua变量值设为value
	//key存放在lua全局表中
	lua_setglobal(L,key);

	SPIN_UNLOCK(E)
}

//初始化环境,创建 struct skynet_env  luaL_newstate
void
skynet_env_init() {
	E = skynet_malloc(sizeof(*E));
	//初始化锁
	SPIN_INIT(E)
	
	E->L = luaL_newstate();
}
