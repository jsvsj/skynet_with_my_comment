#ifndef skynet_hashid_h
#define skynet_hashid_h

#include <assert.h>
#include <stdlib.h>
#include <string.h>

//hashid_node�ڵ�
struct hashid_node {
	int id;
	struct hashid_node *next;
};


struct hashid {

	//ͨ���������������Ľڵ��ͷָ���� hashָ�������е�λ��
	int hashmod;
	
	int cap; // id ���������

	int count;	//�Ѿ��洢�ĸ���

	//���hashid_node����
	struct hashid_node *id;

	//���hashid_node *ָ������    ������id�����������õ���16�ı���

	//���е�Ԫ����ָ�룬�൱����id������ĳ����Ԫ�����Ӷ��ɵ������ͷ����ָ��
	struct hashid_node **hash;
};

//��ʼ��hashid�ṹ��
static void
hashid_init(struct hashid *hi, int max) {
	int i;
	int hashcap;
	hashcap = 16;

	//hashcapΪ16�ı���
	while (hashcap < max) {
		hashcap *= 2;
	}

	
	hi->hashmod = hashcap - 1;

	//����
	hi->cap = max;

	hi->count = 0;

	//����i���ֿռ�
	hi->id = skynet_malloc(max * sizeof(struct hashid_node));

	for (i=0;i<max;i++) {
		hi->id[i].id = -1;
		hi->id[i].next = NULL;
	}
	
	hi->hash = skynet_malloc(hashcap * sizeof(struct hashid_node *));
	memset(hi->hash, 0, hashcap * sizeof(struct hashid_node *));
}

//���hashid��Ԫ��
static void
hashid_clear(struct hashid *hi) {
	skynet_free(hi->id);
	skynet_free(hi->hash);
	hi->id = NULL;
	hi->hash = NULL;
	hi->hashmod = 1;
	hi->cap = 0;
	hi->count = 0;
}


//��hashid�в���idԪ��
static int
hashid_lookup(struct hashid *hi, int id) {
	//�ҳ�id��Ӧ��ͷָ����hi�� hashָ�������е�λ��
	int h = id & hi->hashmod;

	//ͷ���ָ��
	struct hashid_node * c = hi->hash[h];


	while(c) {
		//�ҵ�Ԫ��
		if (c->id == id)
			return c - hi->id;
		c = c->next;
	}
	return -1;
}

//����Ԫ��
static int
hashid_remove(struct hashid *hi, int id) {

	//�ҳ�id��Ӧ��ͷָ����hi�� hashָ�������е�λ��
	int h = id & hi->hashmod;

	//ָ��ͷ���
	struct hashid_node * c = hi->hash[h];
	
	if (c == NULL)
		return -1;

	//�鹵ͷ������Ҫɾ���Ľڵ�
	if (c->id == id) {
		
		//�ƶ�ͷ���ָ���ָ��
		hi->hash[h] = c->next;
		goto _clear;
	}
	
	while(c->next) {
		//���������ҵ�Ҫɾ���Ľڵ�
		if (c->next->id == id) {
			
			struct hashid_node * temp = c->next;
			c->next = temp->next;
			c = temp;

			goto _clear;
		}
		c = c->next;
	}
	//���û���ҵ�Ҫɾ���Ľڵ�
	return -1;
	
_clear:
	//��ʾû�б�ʹ��
	c->id = -1;

	c->next = NULL;

	//��������
	--hi->count;
	return c - hi->id;
}

//����Ԫ��
static int
hashid_insert(struct hashid * hi, int id) {
	struct hashid_node *c = NULL;
	int i;
	for (i=0;i<hi->cap;i++) {
		int index = (i+id) % hi->cap;
		
		//�ҵ�һ����δʹ�õ�λ��
		if (hi->id[index].id == -1) {
			c = &hi->id[index];
			break;
		}
	}
	assert(c);
	//����Ԫ�ظ���
	++hi->count;
	
	c->id = id;

	assert(c->next == NULL);

	//�ҳ�id��Ӧ��ͷָ����hi�� hashָ�������е�λ��
	int h = id & hi->hashmod;


	//�γ�����
	if (hi->hash[h]) {
		c->next = hi->hash[h];
	}

	//�൱��ͷ�巨
	hi->hash[h] = c;

	
	return c - hi->id;
}

//�ж�hashid�Ľṹ���е� id ����������Ƿ�� ��ŵ�������ͬ
static inline int
hashid_full(struct hashid *hi) {
	return hi->count == hi->cap;
}

#endif
