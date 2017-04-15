#include "skynet.h"
#include "skynet_env.h"
#include "spinlock.h"

#include <lua.h>
#include <lauxlib.h>

#include <stdlib.h>
#include <assert.h>

struct skynet_env {

	//ʹ��ԭ�Ӳ���ʵ�ֵ���,������pthread_mutex_t,
	struct spinlock lock;
	
	lua_State *L;
};

//skynet ���� ���ã���Ҫ�ǻ�ȡlua�Ļ�������
static struct skynet_env *E = NULL;


//��ȡ���� lua ��ȫ�ֱ��� key ֵ
const char * 
skynet_getenv(const char *key) {

//#define SPIN_LOCK(q) spinlock_lock(&(q)->lock);
	//����
	SPIN_LOCK(E)

	lua_State *L = E->L;

	//��ȡluaȫ�ֱ���key��ֵ����ѹ��luaջ, ��ջ��
	lua_getglobal(L, key);

	//��luaջ�е����ñ���ֵ������ֵ��result
	const char * result = lua_tostring(L, -1);

	//�����ñ���ֵ
	lua_pop(L, 1);

	//����
	SPIN_UNLOCK(E)

	return result;
}

//����luaȫ�ֱ���
void 
skynet_setenv(const char *key, const char *value) {
	SPIN_LOCK(E)
	
	lua_State *L = E->L;

	//��ȡluaȫ�ֱ���key��ֵ����ѹ��luaջ
	lua_getglobal(L, key);

	//���Ըñ���ֵһ���ǿյ�
	assert(lua_isnil(L, -1));

	//�����ñ���ֵ
	lua_pop(L,1);

	//��vaueѹ��luaջ
	lua_pushstring(L,value);

	//��luaջ�е���value,��lua����ֵ��Ϊvalue
	//key�����luaȫ�ֱ���
	lua_setglobal(L,key);

	SPIN_UNLOCK(E)
}

//��ʼ������,���� struct skynet_env  luaL_newstate
void
skynet_env_init() {
	E = skynet_malloc(sizeof(*E));
	//��ʼ����
	SPIN_INIT(E)
	
	E->L = luaL_newstate();
}
