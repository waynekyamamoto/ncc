#ifndef _STDATOMIC_H
#define _STDATOMIC_H

// Minimal stdatomic.h stub for single-threaded compilation
typedef enum { memory_order_relaxed, memory_order_consume, memory_order_acquire,
               memory_order_release, memory_order_acq_rel, memory_order_seq_cst } memory_order;

#define _Atomic(T) T
#define ATOMIC_VAR_INIT(value) (value)
#define atomic_init(obj, value) (*(obj) = (value))
#define atomic_load(obj) (*(obj))
#define atomic_load_explicit(obj, order) (*(obj))
#define atomic_store(obj, value) (*(obj) = (value))
#define atomic_store_explicit(obj, value, order) (*(obj) = (value))
#define atomic_exchange(obj, value) __sync_lock_test_and_set(obj, value)
#define atomic_exchange_explicit(obj, value, order) __sync_lock_test_and_set(obj, value)
#define atomic_compare_exchange_strong(obj, expected, desired) \
    __atomic_compare_exchange_n(obj, expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
#define atomic_compare_exchange_weak(obj, expected, desired) \
    __atomic_compare_exchange_n(obj, expected, desired, 1, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
#define atomic_fetch_add(obj, arg) __sync_fetch_and_add(obj, arg)
#define atomic_fetch_add_explicit(obj, arg, order) __sync_fetch_and_add(obj, arg)
#define atomic_fetch_sub(obj, arg) __sync_fetch_and_sub(obj, arg)
#define atomic_fetch_sub_explicit(obj, arg, order) __sync_fetch_and_sub(obj, arg)
#define atomic_fetch_or(obj, arg) __sync_fetch_and_or(obj, arg)
#define atomic_fetch_or_explicit(obj, arg, order) __sync_fetch_and_or(obj, arg)
#define atomic_fetch_and(obj, arg) __sync_fetch_and_and(obj, arg)
#define atomic_fetch_and_explicit(obj, arg, order) __sync_fetch_and_and(obj, arg)
#define atomic_thread_fence(order) __sync_synchronize()
#define atomic_signal_fence(order) __sync_synchronize()

typedef _Atomic(int) atomic_int;
typedef _Atomic(unsigned int) atomic_uint;
typedef _Atomic(long) atomic_long;
typedef _Atomic(unsigned long) atomic_ulong;
typedef _Atomic(long long) atomic_llong;
typedef _Atomic(unsigned long long) atomic_ullong;
typedef _Atomic(_Bool) atomic_bool;
typedef _Atomic(char) atomic_char;
typedef _Atomic(signed char) atomic_schar;
typedef _Atomic(unsigned char) atomic_uchar;
typedef _Atomic(short) atomic_short;
typedef _Atomic(unsigned short) atomic_ushort;
typedef _Atomic(void *) atomic_intptr_t;
typedef _Atomic(unsigned long) atomic_uintptr_t;
typedef _Atomic(unsigned long) atomic_size_t;

#define ATOMIC_INT_LOCK_FREE 2
#define ATOMIC_LONG_LOCK_FREE 2
#define ATOMIC_LLONG_LOCK_FREE 2
#define ATOMIC_POINTER_LOCK_FREE 2

#endif
