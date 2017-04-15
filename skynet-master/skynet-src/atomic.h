#ifndef SKYNET_ATOMIC_H
#define SKYNET_ATOMIC_H

//原子操作
//先比较 ptr 和 oval是否相等，如果相等，ptr变为nval 
#define ATOM_CAS(ptr, oval, nval) __sync_bool_compare_and_swap(ptr, oval, nval)
#define ATOM_CAS_POINTER(ptr, oval, nval) __sync_bool_compare_and_swap(ptr, oval, nval)


//原子自增    先增加1，在获取
#define ATOM_INC(ptr) __sync_add_and_fetch(ptr, 1)


//先获取，再自增
#define ATOM_FINC(ptr) __sync_fetch_and_add(ptr, 1)

//先自减，再获取
#define ATOM_DEC(ptr) __sync_sub_and_fetch(ptr, 1)


#define ATOM_FDEC(ptr) __sync_fetch_and_sub(ptr, 1)

#define ATOM_ADD(ptr,n) __sync_add_and_fetch(ptr, n)
#define ATOM_SUB(ptr,n) __sync_sub_and_fetch(ptr, n)
#define ATOM_AND(ptr,n) __sync_and_and_fetch(ptr, n)

#endif
