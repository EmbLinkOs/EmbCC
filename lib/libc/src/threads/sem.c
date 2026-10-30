/* <semaphore.h>: a counter and a futex on it. A waiter sleeps only while
 * the count is 0 -- the futex compares that under the kernel's lock --
 * so a post between its look and its sleep is never lost; the waiter
 * count lets a post skip the wake when nobody sleeps. These functions
 * set errno and return -1, as POSIX's semaphores do. */
#include <semaphore.h>
#include <errno.h>
#include <limits.h>

#include "../../os/backend.h"
#include "thr.h"

WEAK int sem_init(sem_t *s, int pshared, unsigned value)
{
    if (value > SEM_VALUE_MAX) {
        errno = EINVAL;
        return -1;
    }
    if (pshared) {
        errno = ENOTSUP;           /* every futex here is private */
        return -1;
    }
    s->__value = (int)value;
    s->__waiters = 0;
    return 0;
}

WEAK int sem_destroy(sem_t *s) { (void)s; return 0; }

static int take(sem_t *s)
{
    int v = __atomic_load_n(&s->__value, __ATOMIC_RELAXED);
    while (v > 0)
        if (__atomic_compare_exchange_n(&s->__value, &v, v - 1, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return 1;
    return 0;
}

WEAK int sem_trywait(sem_t *s)
{
    if (take(s))
        return 0;
    errno = EAGAIN;
    return -1;
}

static int wait_until(sem_t *s, const struct timespec *abs, clockid_t clock)
{
    if (abs && (abs->tv_nsec < 0 || abs->tv_nsec >= 1000000000L)) {
        errno = EINVAL;
        return -1;
    }
    for (;;) {
        if (take(s))
            return 0;
        long ns = __thr_until(abs, clock);
        if (abs && ns == 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        __atomic_fetch_add(&s->__waiters, 1, __ATOMIC_ACQ_REL);
        int saved = errno;
        if (__os_futex_wait(&s->__value, 0, ns) < 0 && errno == ENOSYS)
            __os_thread_yield();
        errno = saved;
        __atomic_fetch_sub(&s->__waiters, 1, __ATOMIC_ACQ_REL);
    }
}

WEAK int sem_wait(sem_t *s) { return wait_until(s, NULL, CLOCK_REALTIME); }

WEAK int sem_timedwait(sem_t *__restrict s,
                       const struct timespec *__restrict abstime)
{
    if (!abstime) {
        errno = EINVAL;
        return -1;
    }
    return wait_until(s, abstime, CLOCK_REALTIME);
}

WEAK int sem_clockwait(sem_t *__restrict s, clockid_t clock,
                       const struct timespec *__restrict abstime)
{
    if (!abstime || (clock != CLOCK_REALTIME && clock != CLOCK_MONOTONIC)) {
        errno = EINVAL;
        return -1;
    }
    return wait_until(s, abstime, clock);
}

WEAK int sem_post(sem_t *s)
{
    int v = __atomic_load_n(&s->__value, __ATOMIC_RELAXED);
    do {
        if (v == SEM_VALUE_MAX) {
            errno = EOVERFLOW;
            return -1;
        }
    } while (!__atomic_compare_exchange_n(&s->__value, &v, v + 1, 0,
                                          __ATOMIC_RELEASE, __ATOMIC_RELAXED));
    if (__atomic_load_n(&s->__waiters, __ATOMIC_ACQUIRE))
        __os_futex_wake(&s->__value, 1);
    return 0;
}

WEAK int sem_getvalue(sem_t *__restrict s, int *__restrict value)
{
    *value = __atomic_load_n(&s->__value, __ATOMIC_RELAXED);
    return 0;
}

WEAK sem_t *sem_open(const char *name, int oflag, ...)
{
    (void)name;
    (void)oflag;
    errno = ENOSYS;
    return SEM_FAILED;
}

WEAK int sem_close(sem_t *s)
{
    (void)s;
    errno = EINVAL;            /* no semaphore was ever opened */
    return -1;
}

WEAK int sem_unlink(const char *name)
{
    (void)name;
    errno = ENOSYS;
    return -1;
}
