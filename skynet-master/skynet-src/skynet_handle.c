#include "skynet.h"

#include "skynet_handle.h"
#include "skynet_server.h"
#include "rwlock.h"

#include <stdlib.h>
#include <assert.h>
#include <string.h>

#define DEFAULT_SLOT_SIZE 4
#define MAX_SLOT_SIZE 0x40000000


//skynet ����ŵĹ���ͷ���

//handle_name ���� �Ͷ�Ӧ handle id��hash��
struct handle_name {
	char * name;

	//handle��һ��uint32_t���������߰�λ��ʾԶ�̽ڵ�(���ǿ���Դ��ļ�Ⱥ��ʩ������ķ����������Ӹò���
	// һ���������ǿ�ܵĺ��ģ����������Ⱥ��ʩ�Ѿ������Ƽ�ʹ��)
	uint32_t handle;
};



//�洢handler��skynet_contex�Ķ�Ӧ��ϵ����һ����ϣ��
//ÿ������skynet_context����Ӧһ�����ظ���handler
//ͨ��handler����Ի�ȡskynet_context
//������ handler �� skynet_context�Ķ�Ӧ


//ʵ�ʾ��ǹ���һ�� skynet_context ��ָ������ ,����һ��handle_name����
struct handle_storage {

	//��д��
	struct rwlock lock;

	//�������� harbor harbor ���ڲ�ͬ������ͨ�� 
	uint32_t harbor;

	//��ʼֵΪ1����ʾhandler�����ʼֵ��1��ʼ
	uint32_t handle_index;

	//slot����Ĵ�С 
	int slot_size;

	//ָ�����飬����skynet_contextָ��
	struct skynet_context ** slot;	//skynet_context ��ռ�

	//handler_name��������ʼΪ2������name_cap��slot_size��һ����ԭ�����ڣ�����ÿһ��handler����name
	int name_cap;

	
	int name_count;		//handler_name����

	//���handle_name������	
	struct handle_name *name;	//handler_name�� ����
};

//����ȫ�ֱ���
static struct handle_storage *H = NULL;

//ע��ctx ��ctx���浽 handler_storage��ϣ���У����õ�һ��handler

//ʵ�ʾ��ǽ�ctx���뵽handle_storageά����һ��ָ��������
uint32_t
skynet_handle_register(struct skynet_context  *ctx) {

	struct handle_storage *s = H;

	//��д��
	rwlock_wlock(&s->lock);
	
	for (;;) {
		int i;

		//hashѡֵ
		for (i=0;i<s->slot_size;i++) {

			//handle��һ��uint32_t���������߰�λ��ʾԶ�̽ڵ�(���ǿ���Դ��ļ�Ⱥ��ʩ������ķ����������Ӹò���

			             //#define HANDLE_MASK 0xffffff   ���� 00000000 11111111 11111111 11111111
			             //x&HANDLE_MASK �ǽ�x�ĸ߰�λ���0
			uint32_t handle = (i+s->handle_index) & HANDLE_MASK; 

			//�ȼ���handler % s->slot_size
			int hash = handle & (s->slot_size-1);
			
			if (s->slot[hash] == NULL) {	//�ҵ�δʹ�õ� slot �����ctx���뵽���slot�� 
				
				s->slot[hash] = ctx;
				
				s->handle_index = handle + 1;	//�ƶ� handler_index �����´�ʹ��

				rwlock_wunlock(&s->lock);

				//harbor ���ڲ�ͬ����֮���ͨ�ţ�handler��8λ����harbor ��24Ϊ���ڱ�����
				//��������Ҫ |=һ��
				
				handle |= s->harbor;
				return handle;
			}
		}

		//���е����˵���������ˣ�
		//ȷ������ 2���ռ�֮�� �ܹ�handler�� slot������������ 24λ����
		assert((s->slot_size*2 - 1) <= HANDLE_MASK);

		//��ϣ����������
		struct skynet_context ** new_slot = skynet_malloc(s->slot_size * 2 * sizeof(struct skynet_context *));

		memset(new_slot, 0, s->slot_size * 2 * sizeof(struct skynet_context *));

		//��ԭ�������ݿ������¿ռ�
		for (i=0;i<s->slot_size;i++) {

			//ӳ���µ�hashֵ
			int hash = skynet_context_handle(s->slot[i]) & (s->slot_size * 2 - 1);
			assert(new_slot[hash] == NULL);
			new_slot[hash] = s->slot[i];
		}
		//����ԭ����ָ������
		skynet_free(s->slot);
		s->slot = new_slot;
		s->slot_size *= 2;
	}
}


//�ջ�handle
//�����ǽ��handleӳ�䣬�����ǵݼ����ü���
/*
����
1.���handle��Ӧ�Ĳ�,����skynet_context_release
2.�����ע��������ɾ����Ӧ�Ľڵ�

*/

//ʵ�ʾ��Ǹ����±���ָ�������� ɾ��ָ��
int                //handle�Ƕ�Ӧ��һ���±�
skynet_handle_retire(uint32_t handle) {
	int ret = 0;
	struct handle_storage *s = H;

	//����
	rwlock_wlock(&s->lock);

	//�ȼ��� handler % s->slot_size
	//�õ��±�
	uint32_t hash = handle & (s->slot_size-1);

	//�õ�ָ�������д�ŵ�ctx
	struct skynet_context * ctx = s->slot[hash];

						//skynet_context_handle ��������   ctx->handle;
	if (ctx != NULL && skynet_context_handle(ctx) == handle) {

		//�ÿգ���ϣ���ڳ��ռ�
		s->slot[hash] = NULL;
		ret = 1;
		int i;
		int j=0, n=s->name_count;

		//���� handler_name��
		for (i=0; i<n; ++i) {

			
			//2.�����ע��������ɾ����Ӧ�Ľڵ�
			//��name�����ҵ� handler��Ӧ��name  ����skynet_free
			if (s->name[i].handle == handle) {
				skynet_free(s->name[i].name);
				continue;
			} else if (i!=j) {
				//˵��free��һ��name
				
				s->name[j] = s->name[i];	//�����Ҫ������Ԫ���Ƶ�ǰ��	
			}
			++j;
		}
		//�޸����Ƶ�����
		s->name_count = j;
	} else {
		ctx = NULL;
	}

	rwlock_wunlock(&s->lock);

	if (ctx) {
		// release ctx may call skynet_handle_* , so wunlock first.
		//�����ü�����һ , ��Ϊ0ʱ��ɾ��skynet_context
		skynet_context_release(ctx);
	}

	return ret;
}



//�������е�handle
//����handle_storage�е�ָ�������е�����skynet_contextָ��
void 
skynet_handle_retireall() {
	struct handle_storage *s = H;
	for (;;) {
		int n=0;
		int i;
		for (i=0;i<s->slot_size;i++) {
			rwlock_rlock(&s->lock);
			struct skynet_context * ctx = s->slot[i];
			uint32_t handle = 0;
			if (ctx)
				handle = skynet_context_handle(ctx);
			rwlock_runlock(&s->lock);
			
			if (handle != 0) {

				//
				if (skynet_handle_retire(handle)) {
					++n;
				}
			}
		}
		if (n==0)
			return;
	}
}

//ͨ��handle��ȡskynet_context*,skynet_context�����ü�����1
struct skynet_context * 
skynet_handle_grab(uint32_t handle) {
	struct handle_storage *s = H;
	struct skynet_context * result = NULL;

	//�Ӷ���
	rwlock_rlock(&s->lock);

	//�õ��±�
	uint32_t hash = handle & (s->slot_size-1);
	
	struct skynet_context * ctx = s->slot[hash];
	if (ctx && skynet_context_handle(ctx) == handle) {
		result = ctx;

			//���ü�����1
		skynet_context_grab(result);
	}

	rwlock_runlock(&s->lock);

	return result;
}

//�������Ʋ���handle
uint32_t 
skynet_handle_findname(const char * name) {
	struct handle_storage *s = H;

	rwlock_rlock(&s->lock);

	uint32_t handle = 0;

	int begin = 0;
	int end = s->name_count - 1;

	//����ʹ�ö��ֲ��ң���˵���������������������
	while (begin<=end) {
		
		int mid = (begin+end)/2;
		struct handle_name *n = &s->name[mid];
		int c = strcmp(n->name, name);			//һֱ��ͷ�����룬ʵ�����������name�ᰴ���ȴ�С������������ʹ�ö��ֲ�����
		
		if (c==0) {
			handle = n->handle;
			break;
		}
		if (c<0) {
			begin = mid + 1;
		} else {
			end = mid - 1;
		}
	}

	rwlock_runlock(&s->lock);

	return handle;
}


//��handle_storage��name�����beforeλ�ò���һ��Ԫ��
static void
_insert_name_before(struct handle_storage *s, char *name, uint32_t handle, int before) {

	//���������������ͬ,��Ҫ����
	if (s->name_count >= s->name_cap) {
		//��������������
		s->name_cap *= 2;
		assert(s->name_cap <= MAX_SLOT_SIZE);

		//��������������
		struct handle_name * n = skynet_malloc(s->name_cap * sizeof(struct handle_name));
		int i;
		
		//��[0,before)֮���Ԫ�ظ��Ƶ��¿��ٵĿռ��[0,before)
		for (i=0;i<before;i++) {
			n[i] = s->name[i];
		}
		
		//��[before,name_count)֮���Ԫ�ظ��Ƶ��¿ռ��[before+1,name_count+1)��
		//�����ͽ�beforeλ�ÿճ���
		for (i=before;i<s->name_count;i++) {
			n[i+1] = s->name[i];
		}

		
		//����ԭ��������
		skynet_free(s->name);
		
		s->name = n;
	} else {
		int i;

		//before ֮���Ԫ�غ���һ��λ��
		for (i=s->name_count;i>before;i--) {
			s->name[i] = s->name[i-1];
		}
	}
	
	//��name�����beforeλ�ü���һ��Ԫ��
	s->name[before].name = name;
	s->name[before].handle = handle;
	s->name_count ++;
}


//����name��handle
static const char *
_insert_name(struct handle_storage *s, const char * name, uint32_t handle) {
	int begin = 0;
	int end = s->name_count - 1;

	//���ֲ���
	while (begin<=end) {
		int mid = (begin+end)/2;
		struct handle_name *n = &s->name[mid];

		//���Ҫ�����name�Ѿ�����
		int c = strcmp(n->name, name);
		if (c==0) {
			return NULL;    //�����Ѿ����ڣ��������Ʋ����ظ�����
		}
		if (c<0) {
			begin = mid + 1;
		} else {
			end = mid - 1;
		}
	}
	
	char * result = skynet_strdup(name);

	//һֱ��ͷ�����룬ʵ�����������name�ᰴ���ȴ�С������������ʹ�ö��ֲ�����
	//result������  
	_insert_name_before(s, result, handle, begin);

	return result;
}

//name��handle��
//������ע��һ�����Ƶ��ʺϻ��õ��ú���

//ʵ�ʾ��ǽ�handler,name���뵽handle_storage��name������
const char * 
skynet_handle_namehandle(uint32_t handle, const char *name) {
	rwlock_wlock(&H->lock);

	const char * ret = _insert_name(H, name, handle);

	rwlock_wunlock(&H->lock);

	return ret;
}

//��ʼ��һ��handler ���ǳ�ʼ��handler_storage,һ���洢skynet_contextָ�������
void 
skynet_handle_init(int harbor) {
	assert(H==NULL);
	struct handle_storage * s = skynet_malloc(sizeof(*H));

	//��ռ�Ĵ�С DEFAULT_SLOT_SIZE   4
	s->slot_size = DEFAULT_SLOT_SIZE;

	//Ϊskynet_ctx*�������ռ�
	s->slot = skynet_malloc(s->slot_size * sizeof(struct skynet_context *));
	memset(s->slot, 0, s->slot_size * sizeof(struct skynet_context *));

	rwlock_init(&s->lock);
	// reserve 0 for system
	s->harbor = (uint32_t) (harbor & 0xff) << HANDLE_REMOTE_SHIFT;

	//handle�����1��ʼ��0������
	s->handle_index = 1;

	//����������ʼΪ2
	s->name_cap = 2;
	s->name_count = 0;
	//Ϊname�������ռ�
	s->name = skynet_malloc(s->name_cap * sizeof(struct handle_name));

	H = s;

	// Don't need to free H
}

