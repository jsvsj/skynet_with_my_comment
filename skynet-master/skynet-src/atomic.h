#ifndef SKYNET_ATOMIC_H
#define SKYNET_ATOMIC_H

//ԭ�Ӳ���
//�ȱȽ� ptr �� oval�Ƿ���ȣ������ȣ�ptr��Ϊnval 
#define ATOM_CAS(ptr, oval, nval) __sync_bool_compare_and_swap(ptr, oval, nval)
#define ATOM_CAS_POINTER(ptr, oval, nval) __sync_bool_compare_and_swap(ptr, oval, nval)


//ԭ������    ������1���ڻ�ȡ
#define ATOM_INC(ptr) __sync_add_and_fetch(ptr, 1)


//�Ȼ�ȡ��������
#define ATOM_FINC(ptr) __sync_fetch_and_add(ptr, 1)

//���Լ����ٻ�ȡ
#define ATOM_DEC(ptr) __sync_sub_and_fetch(ptr, 1)


#define ATOM_FDEC(ptr) __sync_fetch_and_sub(ptr, 1)

#define ATOM_ADD(ptr,n) __sync_add_and_fetch(ptr, n)
#define ATOM_SUB(ptr,n) __sync_sub_and_fetch(ptr, n)
#define ATOM_AND(ptr,n) __sync_and_and_fetch(ptr, n)

#endif
