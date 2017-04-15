#ifndef SKYNET_RWLOCK_H
#define SKYNET_RWLOCK_H

#ifndef USE_PTHREAD_LOCK



//ʹ��ԭ�Ӳ���ʵ�ֶ�д��
struct rwlock {
	int write;
	int read;
};

static inline void
rwlock_init(struct rwlock *lock) {
	lock->write = 0;
	lock->read = 0;
}

static inline void
rwlock_rlock(struct rwlock *lock) {
	for (;;) {
		
		while(lock->write) {	//�ȴ�д���ٶ�	
			
			__sync_synchronize();
		}

		//����
		__sync_add_and_fetch(&lock->read,1);


		//�����д,�ͷ���
		if (lock->write) {
			__sync_sub_and_fetch(&lock->read,1);
		} else {
			break;
		}
	}
}

static inline void
rwlock_wlock(struct rwlock *lock) {

	//���Լ�д��
	while (__sync_lock_test_and_set(&lock->write,1)) {}

	//����ڶ����ȵȴ�����
	while(lock->read) {
		__sync_synchronize();
	}
}


static inline void
rwlock_wunlock(struct rwlock *lock) {
	__sync_lock_release(&lock->write);
}

static inline void
rwlock_runlock(struct rwlock *lock) {
	__sync_sub_and_fetch(&lock->read,1);
}

#else

#include <pthread.h>

// only for some platform doesn't have __sync_*
// todo: check the result of pthread api

//����pthread_rwlock_t��װ��д��
struct rwlock {
	pthread_rwlock_t lock;
};

static inline void
rwlock_init(struct rwlock *lock) {
	pthread_rwlock_init(&lock->lock, NULL);
}

//����
static inline void
rwlock_rlock(struct rwlock *lock) {
	 pthread_rwlock_rdlock(&lock->lock);
}


//����
static inline void
rwlock_wlock(struct rwlock *lock) {
	 pthread_rwlock_wrlock(&lock->lock);
}


//�ͷ���
static inline void
rwlock_wunlock(struct rwlock *lock) {
	pthread_rwlock_unlock(&lock->lock);
}


//�ͷ���
static inline void
rwlock_runlock(struct rwlock *lock) {
	pthread_rwlock_unlock(&lock->lock);
}

#endif

#endif
