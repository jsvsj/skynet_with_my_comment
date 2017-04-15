#ifndef skynet_hashid_h
#define skynet_hashid_h

#include <assert.h>
#include <stdlib.h>
#include <string.h>

//hashid_node节点
struct hashid_node {
	int id;
	struct hashid_node *next;
};


struct hashid {

	//通过次数来计算插入的节点的头指针在 hash指针数组中的位置
	int hashmod;
	
	int cap; // id 数组的容量

	int count;	//已经存储的个数

	//存放hashid_node数组
	struct hashid_node *id;

	//存放hashid_node *指针数组    容量比id的容量大，正好到达16的倍数

	//其中的元素是指针，相当于是id数组中某几个元素连接而成的链表的头结点的指针
	struct hashid_node **hash;
};

//初始化hashid结构体
static void
hashid_init(struct hashid *hi, int max) {
	int i;
	int hashcap;
	hashcap = 16;

	//hashcap为16的倍数
	while (hashcap < max) {
		hashcap *= 2;
	}

	
	hi->hashmod = hashcap - 1;

	//容量
	hi->cap = max;

	hi->count = 0;

	//分配i数字空间
	hi->id = skynet_malloc(max * sizeof(struct hashid_node));

	for (i=0;i<max;i++) {
		hi->id[i].id = -1;
		hi->id[i].next = NULL;
	}
	
	hi->hash = skynet_malloc(hashcap * sizeof(struct hashid_node *));
	memset(hi->hash, 0, hashcap * sizeof(struct hashid_node *));
}

//清空hashid的元素
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


//在hashid中查找id元素
static int
hashid_lookup(struct hashid *hi, int id) {
	//找出id对应的头指针在hi的 hash指针数组中的位置
	int h = id & hi->hashmod;

	//头结点指针
	struct hashid_node * c = hi->hash[h];


	while(c) {
		//找到元素
		if (c->id == id)
			return c - hi->id;
		c = c->next;
	}
	return -1;
}

//销毁元素
static int
hashid_remove(struct hashid *hi, int id) {

	//找出id对应的头指针在hi的 hash指针数组中的位置
	int h = id & hi->hashmod;

	//指向头结点
	struct hashid_node * c = hi->hash[h];
	
	if (c == NULL)
		return -1;

	//乳沟头结点就是要删除的节点
	if (c->id == id) {
		
		//移动头结点指针的指向
		hi->hash[h] = c->next;
		goto _clear;
	}
	
	while(c->next) {
		//在链表中找到要删除的节点
		if (c->next->id == id) {
			
			struct hashid_node * temp = c->next;
			c->next = temp->next;
			c = temp;

			goto _clear;
		}
		c = c->next;
	}
	//如果没有找到要删除的节点
	return -1;
	
_clear:
	//表示没有被使用
	c->id = -1;

	c->next = NULL;

	//减少容量
	--hi->count;
	return c - hi->id;
}

//插入元素
static int
hashid_insert(struct hashid * hi, int id) {
	struct hashid_node *c = NULL;
	int i;
	for (i=0;i<hi->cap;i++) {
		int index = (i+id) % hi->cap;
		
		//找到一个还未使用的位置
		if (hi->id[index].id == -1) {
			c = &hi->id[index];
			break;
		}
	}
	assert(c);
	//增加元素个数
	++hi->count;
	
	c->id = id;

	assert(c->next == NULL);

	//找出id对应的头指针在hi的 hash指针数组中的位置
	int h = id & hi->hashmod;


	//形成链表
	if (hi->hash[h]) {
		c->next = hi->hash[h];
	}

	//相当于头插法
	hi->hash[h] = c;

	
	return c - hi->id;
}

//判断hashid的结构体中的 id 数组的容量是否和 存放的数量相同
static inline int
hashid_full(struct hashid *hi) {
	return hi->count == hi->cap;
}

#endif
