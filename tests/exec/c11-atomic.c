/* clang's __c11_atomic_* builtins, each form on an _Atomic int and an
 * _Atomic pointer, and C11's <stdatomic.h> fetch_add and fetch_sub on an
 * atomic pointer, which count ELEMENTS (C11 7.17.7.5: the object's value
 * plus the operand) -- as clang's do, and GCC's, which count bytes, do
 * not. */
// expect-exit: 42
// no-gcc-reference: clang's builtins; and GCC's atomic_fetch_add on a pointer counts bytes
#include <stdatomic.h>

static _Atomic int ai;
static int arr[8];
static _Atomic(int *) ap;
static _Atomic(char *) cp;
static char chars[8];

int main(void)
{
    int bad = 0;
    __c11_atomic_init(&ai, 5);
    bad |= (__c11_atomic_load(&ai, __ATOMIC_SEQ_CST) != 5) << 0;
    __c11_atomic_store(&ai, 7, __ATOMIC_RELEASE);
    bad |= (__c11_atomic_exchange(&ai, 9, __ATOMIC_ACQ_REL) != 7) << 1;
    {
        int want = 9;
        bad |= (!__c11_atomic_compare_exchange_strong(&ai, &want, 11,
                    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ||
                __c11_atomic_load(&ai, __ATOMIC_RELAXED) != 11) << 2;
        want = 3;
        bad |= (__c11_atomic_compare_exchange_strong(&ai, &want, 13,
                    __ATOMIC_SEQ_CST, __ATOMIC_RELAXED) || want != 11) << 2;
        while (!__c11_atomic_compare_exchange_weak(&ai, &want, 12,
                   __ATOMIC_SEQ_CST, __ATOMIC_RELAXED))
            ;
        bad |= (__c11_atomic_load(&ai, __ATOMIC_ACQUIRE) != 12) << 3;
    }
    bad |= (__c11_atomic_fetch_add(&ai, 3, __ATOMIC_SEQ_CST) != 12 ||
            __c11_atomic_fetch_sub(&ai, 5, __ATOMIC_SEQ_CST) != 15 ||
            __c11_atomic_fetch_or(&ai, 0x30, __ATOMIC_SEQ_CST) != 10 ||
            __c11_atomic_fetch_and(&ai, 0x1e, __ATOMIC_SEQ_CST) != 0x3a ||
            __c11_atomic_fetch_xor(&ai, 0xff, __ATOMIC_SEQ_CST) != 0x1a ||
            __c11_atomic_fetch_nand(&ai, 0x0f, __ATOMIC_SEQ_CST) != 0xe5 ||
            __c11_atomic_load(&ai, __ATOMIC_SEQ_CST) != ~(0xe5 & 0x0f)) << 4;
    __c11_atomic_thread_fence(__ATOMIC_SEQ_CST);
    __c11_atomic_signal_fence(__ATOMIC_SEQ_CST);
    bad |= (!__c11_atomic_is_lock_free(sizeof(int))) << 5;
    /* a pointer counts elements, as C11 asks */
    __c11_atomic_init(&ap, arr);
    bad |= (__c11_atomic_fetch_add(&ap, 3, __ATOMIC_SEQ_CST) != arr ||
            __c11_atomic_load(&ap, __ATOMIC_SEQ_CST) != arr + 3 ||
            __c11_atomic_fetch_sub(&ap, 1, __ATOMIC_SEQ_CST) != arr + 3 ||
            __c11_atomic_load(&ap, __ATOMIC_SEQ_CST) != arr + 2) << 6;
    atomic_store(&ap, arr);
    bad |= (atomic_fetch_add(&ap, 5) != arr || atomic_load(&ap) != arr + 5 ||
            atomic_fetch_sub_explicit(&ap, 2, memory_order_relaxed) != arr + 5 ||
            atomic_load(&ap) != arr + 3) << 7;
    atomic_store(&cp, chars);
    bad |= (atomic_fetch_add(&cp, 5) != chars || atomic_load(&cp) != chars + 5) << 8;
#if __has_builtin(__c11_atomic_load) && __has_builtin(__c11_atomic_fetch_add)
#else
    bad |= 1 << 9;
#endif
    return bad ? bad : 42;
}
