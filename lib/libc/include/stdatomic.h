/* <stdatomic.h> — C11 §7.17.
 *
 * C's atomics, over the same `__atomic_*` builtins the C++ <atomic> uses
 * (lib/libcxx/include/atomic). One lowering, two spellings: the right
 * instruction for a sequentially consistent store is `xchg` on x86-64
 * and `stlr` on aarch64, and that knowledge belongs to the compiler's
 * back end rather than to either library.
 *
 * ---- what _Atomic means here -------------------------------------------
 *
 * The qualifier is accepted by the compiler and the operations below
 * take a POINTER to the object, so `atomic_load(&x)` works whether or
 * not `x` was declared `_Atomic`. What the qualifier does NOT do here
 * is make a plain `x = 1` atomic -- C says it does, and EmbCC does not
 * implement that rewriting, so a program that relies on it is relying
 * on something this compiler will compile as an ordinary store.
 *
 * Use the functions. They are what the C++ library uses too, and they
 * say at every call which ordering was meant -- which is the half that
 * a bare assignment hides even where it works.
 *
 * ---- what is not here ---------------------------------------------------
 *
 * ATOMIC_VAR_INIT, which C17 deprecated and C23 removed. The initializer
 * is just the value.
 */
#ifndef _STDATOMIC_H
#define _STDATOMIC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    memory_order_relaxed = __ATOMIC_RELAXED,
    memory_order_consume = __ATOMIC_CONSUME,
    memory_order_acquire = __ATOMIC_ACQUIRE,
    memory_order_release = __ATOMIC_RELEASE,
    memory_order_acq_rel = __ATOMIC_ACQ_REL,
    memory_order_seq_cst = __ATOMIC_SEQ_CST
} memory_order;

/* The named types. Each is the plain type with the qualifier, so an
 * atomic_int and an int have the same layout and the same alignment --
 * which is what lets one be passed to a function taking the other's
 * address. */
typedef _Atomic _Bool              atomic_bool;
typedef _Atomic char               atomic_char;
typedef _Atomic signed char        atomic_schar;
typedef _Atomic unsigned char      atomic_uchar;
typedef _Atomic short              atomic_short;
typedef _Atomic unsigned short     atomic_ushort;
typedef _Atomic int                atomic_int;
typedef _Atomic unsigned int       atomic_uint;
typedef _Atomic long               atomic_long;
typedef _Atomic unsigned long      atomic_ulong;
typedef _Atomic long long          atomic_llong;
typedef _Atomic unsigned long long atomic_ullong;
typedef _Atomic size_t             atomic_size_t;
typedef _Atomic ptrdiff_t          atomic_ptrdiff_t;
typedef _Atomic intptr_t           atomic_intptr_t;
typedef _Atomic uintptr_t          atomic_uintptr_t;
typedef _Atomic intmax_t           atomic_intmax_t;
typedef _Atomic uintmax_t          atomic_uintmax_t;
typedef _Atomic int8_t             atomic_int_least8_t;
typedef _Atomic uint8_t            atomic_uint_least8_t;
typedef _Atomic int16_t            atomic_int_least16_t;
typedef _Atomic uint16_t           atomic_uint_least16_t;
typedef _Atomic int32_t            atomic_int_least32_t;
typedef _Atomic uint32_t           atomic_uint_least32_t;
typedef _Atomic int64_t            atomic_int_least64_t;
typedef _Atomic uint64_t           atomic_uint_least64_t;

/* Every one of these is lock-free on both targets. The macros say 2 --
 * "always" -- rather than 1, which would mean "sometimes" and would
 * make a program test at run time for something that is a property of
 * the target. */
#define ATOMIC_BOOL_LOCK_FREE     2
#define ATOMIC_CHAR_LOCK_FREE     2
#define ATOMIC_CHAR16_T_LOCK_FREE 2
#define ATOMIC_CHAR32_T_LOCK_FREE 2
#define ATOMIC_WCHAR_T_LOCK_FREE  2
#define ATOMIC_SHORT_LOCK_FREE    2
#define ATOMIC_INT_LOCK_FREE      2
#define ATOMIC_LONG_LOCK_FREE     2
#define ATOMIC_LLONG_LOCK_FREE    2
#define ATOMIC_POINTER_LOCK_FREE  2

/* ---- the generic operations -------------------------------------------
 *
 * Macros, not functions, because each works on any atomic type and C
 * has no templates. The builtins behind them take the object's address
 * and the compiler knows its width.
 */
#define atomic_init(obj, value)  ((void)(*(obj) = (value)))

#define atomic_thread_fence(order) __atomic_thread_fence(order)
#define atomic_signal_fence(order) __atomic_signal_fence(order)

#define atomic_is_lock_free(obj) __atomic_is_lock_free(sizeof(*(obj)), (obj))

#define atomic_store_explicit(obj, value, order) \
    __atomic_store_n((obj), (value), (order))
#define atomic_store(obj, value) \
    atomic_store_explicit((obj), (value), memory_order_seq_cst)

#define atomic_load_explicit(obj, order) __atomic_load_n((obj), (order))
#define atomic_load(obj) atomic_load_explicit((obj), memory_order_seq_cst)

#define atomic_exchange_explicit(obj, value, order) \
    __atomic_exchange_n((obj), (value), (order))
#define atomic_exchange(obj, value) \
    atomic_exchange_explicit((obj), (value), memory_order_seq_cst)

/* The failure order may not be release or acq_rel -- a failed exchange
 * performs no store, so there is nothing to release -- and callers get
 * that right by passing the same order twice, which these macros do for
 * the non-explicit forms. */
#define atomic_compare_exchange_strong_explicit(obj, expected, desired, \
                                                succ, fail)             \
    __atomic_compare_exchange_n((obj), (expected), (desired), 0, (succ), (fail))
#define atomic_compare_exchange_strong(obj, expected, desired)      \
    atomic_compare_exchange_strong_explicit((obj), (expected),      \
                                            (desired),              \
                                            memory_order_seq_cst,   \
                                            memory_order_seq_cst)

/* The weak form may fail SPURIOUSLY -- report a mismatch when the bytes
 * did match -- and on a load-linked/store-conditional machine like
 * aarch64 that is the cheap one, because the strong form has to loop
 * internally. Weak belongs in a loop you were going to write anyway. */
#define atomic_compare_exchange_weak_explicit(obj, expected, desired,   \
                                              succ, fail)               \
    __atomic_compare_exchange_n((obj), (expected), (desired), 1, (succ), (fail))
#define atomic_compare_exchange_weak(obj, expected, desired)        \
    atomic_compare_exchange_weak_explicit((obj), (expected),        \
                                          (desired),                \
                                          memory_order_seq_cst,     \
                                          memory_order_seq_cst)

/* Each returns the value BEFORE the operation, which is what makes
 * fetch_add a ticket dispenser: every caller gets a different number
 * and none is skipped. */
#define atomic_fetch_add_explicit(obj, arg, order) \
    __atomic_fetch_add((obj), (arg), (order))
#define atomic_fetch_add(obj, arg) \
    atomic_fetch_add_explicit((obj), (arg), memory_order_seq_cst)
#define atomic_fetch_sub_explicit(obj, arg, order) \
    __atomic_fetch_sub((obj), (arg), (order))
#define atomic_fetch_sub(obj, arg) \
    atomic_fetch_sub_explicit((obj), (arg), memory_order_seq_cst)
#define atomic_fetch_or_explicit(obj, arg, order) \
    __atomic_fetch_or((obj), (arg), (order))
#define atomic_fetch_or(obj, arg) \
    atomic_fetch_or_explicit((obj), (arg), memory_order_seq_cst)
#define atomic_fetch_xor_explicit(obj, arg, order) \
    __atomic_fetch_xor((obj), (arg), (order))
#define atomic_fetch_xor(obj, arg) \
    atomic_fetch_xor_explicit((obj), (arg), memory_order_seq_cst)
#define atomic_fetch_and_explicit(obj, arg, order) \
    __atomic_fetch_and((obj), (arg), (order))
#define atomic_fetch_and(obj, arg) \
    atomic_fetch_and_explicit((obj), (arg), memory_order_seq_cst)

/* ---- atomic_flag --------------------------------------------------------
 *
 * The one type C guarantees is lock-free everywhere, and the only one
 * that needs no support beyond test-and-set. test_and_set returns what
 * the flag WAS, so the first caller sees false -- which is what makes it
 * a lock.
 */
typedef struct { _Atomic unsigned char __v; } atomic_flag;

#define ATOMIC_FLAG_INIT { 0 }

#define atomic_flag_test_and_set_explicit(obj, order) \
    __atomic_test_and_set(&(obj)->__v, (order))
#define atomic_flag_test_and_set(obj) \
    atomic_flag_test_and_set_explicit((obj), memory_order_seq_cst)
#define atomic_flag_clear_explicit(obj, order) \
    __atomic_clear(&(obj)->__v, (order))
#define atomic_flag_clear(obj) \
    atomic_flag_clear_explicit((obj), memory_order_seq_cst)

#ifdef __cplusplus
}
#endif

#endif
