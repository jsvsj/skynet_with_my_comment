#include "skynet.h"

#include "skynet_module.h"
#include "spinlock.h"

#include <assert.h>
#include <string.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

//��̬���ӿ� .so �ļ���

//ģ���������32,������ģ������Ӧ�ķ����������32  һ��ģ����Զ�Ӧ�������
#define MAX_MODULE_TYPE 32


//�ýṹ������������ص�ģ��,����struct skynet_module����
struct modules {
			
	int count;		//�Ѿ����ص�ģ������

	struct spinlock lock;	//��

	//��ʾ��̬�������·��,��lua��package����һ��
	const char * path;	//ģ������·��		

	//��������skynet_module,���Ϊ32����  MAX_MODULE_TYPE 32
	struct skynet_module m[MAX_MODULE_TYPE];	//ģ������,��Ŷ�̬�������
};


//������ص�ģ��,�ڲ���һ������
static struct modules * M = NULL;


//���Դ�һ��ģ��(��̬��)    name�Ƕ�̬������
static void *
_try_open(struct modules *m, const char * name) {
	const char *l;

	//����·��
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

		//������˽�β
		if (*path == '\0') break;

		//��ѯ;λ��
		l = strchr(path, ';');

		if (l == NULL) 
			l = path + strlen(path);

		int len = l - path;
		int i;
		for (i=0;path[i]!='?' && i < len ;i++) {
			tmp[i] = path[i];
		}
		//��Ҫ�Ƕ�path�������·���Ľ�����path���Զ���һ��·��ģ��,ÿ��·��ʹ��;����
		//��"./server/?.so;./http/?.so"
		//���ģ����Ϊfoo,��ô�ͻ����Ϊ,./server/foo.so;./http/foo.so
		//Ȼ����������·�����ε��� dlopen,ֱ���гɹ����ص�

		//·��+����
		memcpy(tmp+i,name,name_size);
		
		if (path[i] == '?') {
			strncpy(tmp+i+name_size,path+i+1,len - i - 1);
		} else {
			fprintf(stderr,"Invalid C service path\n");
			exit(1);
		}

		//dlopen()��һ����̬���ӿ⣬�����ض�̬���ӿ�ľ��
		dl = dlopen(tmp, RTLD_NOW | RTLD_GLOBAL);
		
		path = l;
	}while(dl == NULL);

	if (dl == NULL) {
		fprintf(stderr, "try open %s failed : %s\n",name,dlerror());
	}

	return dl;
}

//��struct modules�в�ѯ
//���ݶ�̬�������������в���
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

//��ʼ��XXX_create,xxx_init,xxx_release��ַ
//�ɹ�����0,ʧ�ܷ���1
//_open_sym�ǵ���dlsym��ȡ�ĸ���Լ������ָ�룬��ʵ�ֿ���֪������Լ��������������Ϊ
// ģ����_������
//���sm����m


static int
_open_sym(struct skynet_module *mod) {
	size_t name_size = strlen(mod->name);

	// ģ����_������
	char tmp[name_size + 9]; // create/init/release/signal , longest name is release (7)
	memcpy(tmp, mod->name, name_size);
	strcpy(tmp+name_size, "_create");

	//dlsym  �������Ƽ��غ������õ�����ָ��
	mod->create = dlsym(mod->module, tmp);

	strcpy(tmp+name_size, "_init");

	mod->init = dlsym(mod->module, tmp);

	strcpy(tmp+name_size, "_release");

	// dlsym()���ݶ�̬���ӿ�����������ţ����ط��Ŷ�Ӧ�ĵ�ַ

	mod->release = dlsym(mod->module, tmp);
	strcpy(tmp+name_size, "_signal");
	mod->signal = dlsym(mod->module, tmp);

	//����һ��Ҫ��xxxx_init() ����
	return mod->init == NULL;
}


//�������Ʋ���ģ�飬���û���ҵ�������ظ�ģ��
struct skynet_module * 
skynet_module_query(const char * name) {

	//����ģ��
	//�ӻ��濪ʼ����,ʹ������������֤�̰߳�ȫ
	struct skynet_module * result = _query(name);
	if (result)
		return result;

	//����
	SPIN_LOCK(M)

	//�ֲ�����һ�λ��棬��Ϊ��������������ʱ�������̼߳�����һ��ͬ����so
	result = _query(name); // double check

	//���û�����ģ�飬���Դ����ģ��
	if (result == NULL && M->count < MAX_MODULE_TYPE) {
		int index = M->count;

			//���ض�̬���ӿ�
		void * dl = _try_open(M,name);


		//���򿪵�ģ��ṹ��ָ��洢ȫ�ֱ��� M ��	
		if (dl) {
			M->m[index].name = name;
			M->m[index].module = dl;


		// ��ʼ��xxx_create xxx_init xxx_release ��ַ

			//_open_sym�ǵ���dlsym��ȡ�ĸ���Լ������ָ�룬��ʵ�ֿ���֪������Լ��������������Ϊ
			// ģ����_������
			//��� skynet_module ����m

			//���ʼ��skynet_modul��ֵ
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

//����ģ�鵽������
void 
skynet_module_insert(struct skynet_module *mod) {
	SPIN_LOCK(M)

	//�ȸ������ֲ����Ѿ����صĶ�̬��
	struct skynet_module * m = _query(mod->name);

	//���������ģ��
	assert(m == NULL && M->count < MAX_MODULE_TYPE);
	int index = M->count;

	//�����ģ��ŵ���Ӧλ��
	M->m[index] = *mod;
	++M->count;

	SPIN_UNLOCK(M)
}

//,������ڣ�����ö�̬���е�create����
void * 
skynet_module_instance_create(struct skynet_module *m) {
	if (m->create) {
		return m->create();
	} else {
		//intptr_t����32Ϊ������int,����64λ������long int
		//C99�涨intptr_t���Ա���ָ��ֵ�������(~0)��ת��Ϊintptr_t��ת��Ϊvoid*
		return (void *)(intptr_t)(~0);
	}
}

//���ö�̬���е�init����
int
skynet_module_instance_init(struct skynet_module *m, void * inst, struct skynet_context *ctx, const char * parm) {
	return m->init(inst, ctx, parm);
}

//������� ���ö�̬���е�release����
void 
skynet_module_instance_release(struct skynet_module *m, void *inst) {
	if (m->release) {
		m->release(inst);
	}
}

//�������,���ö�̬���е�signal����
void
skynet_module_instance_signal(struct skynet_module *m, void *inst, int signal) {
	if (m->signal) {
		m->signal(inst, signal);
	}
}

//��ʼ��ȫ�ֵ�modules,ʵ�ʾ�����һmodel����
void 
skynet_module_init(const char *path) {
	struct modules *m = skynet_malloc(sizeof(*m));
	m->count = 0;

	//��ʼ����̬�����·��
	m->path = skynet_strdup(path);	//��ʼ��ģ������·��

	//��ʼ����
	SPIN_INIT(m)

	M = m;
}

