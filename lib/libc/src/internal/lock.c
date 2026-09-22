/* See lock.h for the shape and why it is this one. */
#include "lock.h"

#include "../../os/backend.h"

void __lock(__lock_t *l)
{
    int c = 0;
    /* The whole fast path: one compare-exchange, no syscall. This is the
     * branch a single-threaded program always takes. */
    if (__atomic_compare_exchange_n(&l->v, &c, 1, 0, __ATOMIC_ACQUIRE,
                                    __ATOMIC_RELAXED))
        return;

    /* Contended. Claim the "somebody might be waiting" state before
     * sleeping, because the unlocker only wakes when it sees it -- and
     * the exchange is what makes claiming it and reading the old value
     * one step, so a lock released in between cannot be missed. */
    if (c != 2)
        c = __atomic_exchange_n(&l->v, 2, __ATOMIC_ACQUIRE);
    while (c != 0) {
        /* A failure here means no futex (ENOSYS) or an interruption;
         * both are handled by going round again, and the yield keeps
         * the no-futex case from being a bare spin. The wait compares
         * against 2 itself, so a release between the exchange above and
         * this call returns immediately rather than sleeping. */
        if (__os_futex_wait(&l->v, 2, -1) < 0)
            __os_thread_yield();
        c = __atomic_exchange_n(&l->v, 2, __ATOMIC_ACQUIRE);
    }
}

void __unlock(__lock_t *l)
{
    /* Decrement rather than store: if the old value was 1 the lock was
     * uncontended and is now 0, with no syscall. Anything else means a
     * waiter may exist, so it is cleared and one is woken. */
    if (__atomic_fetch_sub(&l->v, 1, __ATOMIC_RELEASE) != 1) {
        __atomic_store_n(&l->v, 0, __ATOMIC_RELEASE);
        __os_futex_wake(&l->v, 1);
    }
}
