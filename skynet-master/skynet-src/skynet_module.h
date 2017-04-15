#ifndef SKYNET_MODULE_H
#define SKYNET_MODULE_H

struct skynet_context;

typedef void * (*skynet_dl_create)(void);
typedef int (*skynet_dl_init)(void * inst, struct skynet_context *, const char * parm);
typedef void (*skynet_dl_release)(void * inst);
typedef void (*skynet_dl_signal)(void * inst, int signal);


//������ص� ��̬��Ľṹ�� 
struct skynet_module {
			
	const char * name;	//ģ������,��̬����

	//��̬����
	void * module;		//���ڱ���dlopen���ص�handle

	//��Լ����֮һ�����������û�����
	//ʵ�ʾ��Ǻ���ָ�룬��̬���з������͵ĺ���ָ����ܱ����أ�����
	skynet_dl_create create;	//���ڱ���xxx_create������ڵ�ַ

	//������ʼ������
	skynet_dl_init init;		//���ڱ���xxx_init������ڵ�ַ

	//�����ͷŶ���
	skynet_dl_release release;	//���ڱ���xxx_release������ڵ�ַ

	//����ʵ���źŹ���
	skynet_dl_signal signal;	//���ڱ���xxx_signal������ڵ�ַ
};	

void skynet_module_insert(struct skynet_module *mod);
struct skynet_module * skynet_module_query(const char * name);
void * skynet_module_instance_create(struct skynet_module *);
int skynet_module_instance_init(struct skynet_module *, void * inst, struct skynet_context *ctx, const char * parm);
void skynet_module_instance_release(struct skynet_module *, void *inst);
void skynet_module_instance_signal(struct skynet_module *, void *inst, int signal);

void skynet_module_init(const char *path);

#endif

