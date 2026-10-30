/* POSIX's locks: mutexes, condition variables, read-write locks,
 * barriers and spin locks, each made of the futex word in thr.h.
 *
 * Process-shared objects are refused (ENOTSUP): every futex here is the
 * private kind, which another process cannot wake. So are robust and
 * priority-protocol mutexes, which need the kernel's help. */
#define _GNU_SOURCE 1
#include <pthread.h>
#include <errno.h>
#include <limits.h>
#include <string.h>

#include "../../os/backend.h"
#include "thr.h"

static int bad_time(const struct timespec *t)
{
    return !t || t->tv_nsec < 0 || t->tv_nsec >= 1000000000L;
}

static int bad_clock(clockid_t c)
{
    return c != CLOCK_REALTIME && c != CLOCK_MONOTONIC;
}

/* 1 when there is no futex here -- and so no other thread to wake us */
static int sleep_on(volatile int *w, int expected, long ns)
{
    int saved = errno, absent = 0;
    if (__os_futex_wait(w, expected, ns) < 0 && errno == ENOSYS) {
        __os_thread_yield();
        absent = 1;
    }
    errno = saved;
    return absent;
}

/* ---- mutexes ---------------------------------------------------------------- */

WEAK int pthread_mutexattr_init(pthread_mutexattr_t *a)
{
    memset(a, 0, sizeof *a);
    return 0;
}

WEAK int pthread_mutexattr_destroy(pthread_mutexattr_t *a) { (void)a; return 0; }

WEAK int pthread_mutexattr_gettype(const pthread_mutexattr_t *__restrict a,
                                   int *__restrict t)
{
    *t = a->__type;
    return 0;
}

WEAK int pthread_mutexattr_settype(pthread_mutexattr_t *a, int t)
{
    if (t != PTHREAD_MUTEX_NORMAL && t != PTHREAD_MUTEX_RECURSIVE &&
        t != PTHREAD_MUTEX_ERRORCHECK)
        return EINVAL;
    a->__type = t;
    return 0;
}

WEAK int pthread_mutexattr_getpshared(const pthread_mutexattr_t *__restrict a,
                                      int *__restrict s)
{
    *s = a->__pshared;
    return 0;
}

static int pshared(int s)
{
    if (s == PTHREAD_PROCESS_PRIVATE)
        return 0;
    return s == PTHREAD_PROCESS_SHARED ? ENOTSUP : EINVAL;
}

WEAK int pthread_mutexattr_setpshared(pthread_mutexattr_t *a, int s)
{
    int e = pshared(s);
    if (!e)
        a->__pshared = s;
    return e;
}

WEAK int pthread_mutexattr_getprotocol(const pthread_mutexattr_t *__restrict a,
                                       int *__restrict p)
{
    *p = a->__protocol;
    return 0;
}

WEAK int pthread_mutexattr_setprotocol(pthread_mutexattr_t *a, int p)
{
    if (p == PTHREAD_PRIO_NONE) {
        a->__protocol = p;
        return 0;
    }
    return p == PTHREAD_PRIO_INHERIT || p == PTHREAD_PRIO_PROTECT ? ENOTSUP
                                                                  : EINVAL;
}

WEAK int pthread_mutexattr_getrobust(const pthread_mutexattr_t *__restrict a,
                                     int *__restrict r)
{
    *r = a->__robust;
    return 0;
}

WEAK int pthread_mutexattr_setrobust(pthread_mutexattr_t *a, int r)
{
    if (r == PTHREAD_MUTEX_STALLED) {
        a->__robust = r;
        return 0;
    }
    return r == PTHREAD_MUTEX_ROBUST ? ENOTSUP : EINVAL;
}

WEAK int pthread_mutex_init(pthread_mutex_t *__restrict m,
                            const pthread_mutexattr_t *__restrict a)
{
    memset(m, 0, sizeof *m);
    m->__type = a ? a->__type : PTHREAD_MUTEX_NORMAL;
    return 0;
}

WEAK int pthread_mutex_destroy(pthread_mutex_t *m)
{
    return __atomic_load_n(&m->__state, __ATOMIC_RELAXED) ? EBUSY : 0;
}

/* Ownership is recorded for all three kinds, so that a recursive lock
 * can count and an error-checking one can refuse; a NORMAL mutex relocked
 * by its owner deadlocks, as POSIX says it does. */
static int mutex_lock(pthread_mutex_t *m, const struct timespec *abs,
                      clockid_t clock)
{
    unsigned long me = __os_thread_self();
    if (m->__count && m->__owner == me) {
        if (m->__type == PTHREAD_MUTEX_RECURSIVE) {
            if (m->__count == UINT_MAX)
                return EAGAIN;
            m->__count++;
            return 0;
        }
        if (m->__type == PTHREAD_MUTEX_ERRORCHECK)
            return EDEADLK;
    }
    int e = __word_lock(&m->__state, abs, clock);
    if (e)
        return e;
    m->__owner = me;
    m->__count = 1;
    return 0;
}

WEAK int pthread_mutex_lock(pthread_mutex_t *m)
{
    return mutex_lock(m, NULL, CLOCK_REALTIME);
}

WEAK int pthread_mutex_timedlock(pthread_mutex_t *__restrict m,
                                 const struct timespec *__restrict abstime)
{
    if (bad_time(abstime))
        return EINVAL;
    return mutex_lock(m, abstime, CLOCK_REALTIME);
}

WEAK int pthread_mutex_clocklock(pthread_mutex_t *__restrict m, clockid_t clock,
                                 const struct timespec *__restrict abstime)
{
    if (bad_time(abstime) || bad_clock(clock))
        return EINVAL;
    return mutex_lock(m, abstime, clock);
}

WEAK int pthread_mutex_trylock(pthread_mutex_t *m)
{
    unsigned long me = __os_thread_self();
    if (m->__count && m->__owner == me &&
        m->__type == PTHREAD_MUTEX_RECURSIVE) {
        if (m->__count == UINT_MAX)
            return EAGAIN;
        m->__count++;
        return 0;
    }
    if (!__word_trylock(&m->__state))
        return EBUSY;
    m->__owner = me;
    m->__count = 1;
    return 0;
}

WEAK int pthread_mutex_unlock(pthread_mutex_t *m)
{
    if (m->__type != PTHREAD_MUTEX_NORMAL &&
        (!m->__count || m->__owner != __os_thread_self()))
        return EPERM;
    if (m->__count > 1) {
        m->__count--;
        return 0;
    }
    m->__owner = 0;
    m->__count = 0;
    __word_unlock(&m->__state);
    return 0;
}

WEAK int pthread_mutex_consistent(pthread_mutex_t *m)
{
    (void)m;
    return EINVAL;             /* no mutex here is robust */
}

WEAK int pthread_mutex_getprioceiling(const pthread_mutex_t *__restrict m,
                                      int *__restrict ceiling)
{
    (void)m;
    (void)ceiling;
    return EINVAL;             /* none is PTHREAD_PRIO_PROTECT */
}

WEAK int pthread_mutex_setprioceiling(pthread_mutex_t *__restrict m,
                                      int ceiling, int *__restrict old)
{
    (void)m;
    (void)ceiling;
    (void)old;
    return EINVAL;
}

/* ---- condition variables ------------------------------------------------------ */

WEAK int pthread_condattr_init(pthread_condattr_t *a)
{
    a->__clock = CLOCK_REALTIME;
    a->__pshared = PTHREAD_PROCESS_PRIVATE;
    return 0;
}

WEAK int pthread_condattr_destroy(pthread_condattr_t *a) { (void)a; return 0; }

WEAK int pthread_condattr_getclock(const pthread_condattr_t *__restrict a,
                                   clockid_t *__restrict c)
{
    *c = a->__clock;
    return 0;
}

WEAK int pthread_condattr_setclock(pthread_condattr_t *a, clockid_t c)
{
    if (bad_clock(c))
        return EINVAL;
    a->__clock = c;
    return 0;
}

WEAK int pthread_condattr_getpshared(const pthread_condattr_t *__restrict a,
                                     int *__restrict s)
{
    *s = a->__pshared;
    return 0;
}

WEAK int pthread_condattr_setpshared(pthread_condattr_t *a, int s)
{
    int e = pshared(s);
    if (!e)
        a->__pshared = s;
    return e;
}

WEAK int pthread_cond_init(pthread_cond_t *__restrict c,
                           const pthread_condattr_t *__restrict a)
{
    c->__seq = 0;
    c->__clock = a ? a->__clock : CLOCK_REALTIME;
    return 0;
}

WEAK int pthread_cond_destroy(pthread_cond_t *c) { (void)c; return 0; }

/* Bump, then wake: a waiter about to sleep finds the number moved and
 * does not sleep, so a signal is never lost between its unlock and its
 * sleep. */
WEAK int pthread_cond_signal(pthread_cond_t *c)
{
    __atomic_fetch_add(&c->__seq, 1, __ATOMIC_RELEASE);
    __os_futex_wake(&c->__seq, 1);
    return 0;
}

WEAK int pthread_cond_broadcast(pthread_cond_t *c)
{
    __atomic_fetch_add(&c->__seq, 1, __ATOMIC_RELEASE);
    __os_futex_wake(&c->__seq, -1);
    return 0;
}

static int cond_wait(pthread_cond_t *c, pthread_mutex_t *m,
                     const struct timespec *abs, clockid_t clock)
{
    unsigned long me = __os_thread_self();
    if (m->__type != PTHREAD_MUTEX_NORMAL &&
        (!m->__count || m->__owner != me))
        return EPERM;
    /* read under the mutex: a signal after this changes the number the
     * sleep below compares against */
    int s = __atomic_load_n(&c->__seq, __ATOMIC_RELAXED);
    unsigned count = m->__count;
    m->__owner = 0;
    m->__count = 0;
    __word_unlock(&m->__state);
    long ns = __thr_until(abs, clock);
    /* with no futex there is one thread, which nothing can signal: a
     * timed wait waits out its deadline rather than return at once */
    if ((!abs || ns > 0) && sleep_on(&c->__seq, s, ns) && abs)
        while (__atomic_load_n(&c->__seq, __ATOMIC_RELAXED) == s &&
               __thr_until(abs, clock) > 0)
            __os_thread_yield();
    __word_lock(&m->__state, NULL, CLOCK_REALTIME);
    m->__owner = me;
    m->__count = count;
    /* timed out only if nothing signalled: a wake that raced the
     * deadline is a wake */
    if (abs && __atomic_load_n(&c->__seq, __ATOMIC_RELAXED) == s &&
        __thr_until(abs, clock) == 0)
        return ETIMEDOUT;
    return 0;
}

WEAK int pthread_cond_wait(pthread_cond_t *__restrict c,
                           pthread_mutex_t *__restrict m)
{
    return cond_wait(c, m, NULL, CLOCK_REALTIME);
}

WEAK int pthread_cond_timedwait(pthread_cond_t *__restrict c,
                                pthread_mutex_t *__restrict m,
                                const struct timespec *__restrict abstime)
{
    if (bad_time(abstime))
        return EINVAL;
    return cond_wait(c, m, abstime, c->__clock);
}

WEAK int pthread_cond_clockwait(pthread_cond_t *__restrict c,
                                pthread_mutex_t *__restrict m, clockid_t clock,
                                const struct timespec *__restrict abstime)
{
    if (bad_time(abstime) || bad_clock(clock))
        return EINVAL;
    return cond_wait(c, m, abstime, clock);
}

/* ---- read-write locks -------------------------------------------------------- *
 *
 * Readers first, as glibc's default: a reader waits only while a writer
 * HOLDS the lock, so a thread may take a read lock it already holds
 * without deadlocking behind a waiting writer. A stream of readers can
 * keep a writer waiting; that is the price of the guarantee. */

WEAK int pthread_rwlockattr_init(pthread_rwlockattr_t *a)
{
    memset(a, 0, sizeof *a);
    return 0;
}

WEAK int pthread_rwlockattr_destroy(pthread_rwlockattr_t *a) { (void)a; return 0; }

WEAK int pthread_rwlockattr_getpshared(const pthread_rwlockattr_t *__restrict a,
                                       int *__restrict s)
{
    *s = a->__pshared;
    return 0;
}

WEAK int pthread_rwlockattr_setpshared(pthread_rwlockattr_t *a, int s)
{
    int e = pshared(s);
    if (!e)
        a->__pshared = s;
    return e;
}

WEAK int pthread_rwlock_init(pthread_rwlock_t *__restrict l,
                             const pthread_rwlockattr_t *__restrict a)
{
    (void)a;
    memset(l, 0, sizeof *l);
    return 0;
}

WEAK int pthread_rwlock_destroy(pthread_rwlock_t *l)
{
    return l->__readers || l->__writer ? EBUSY : 0;
}

/* sleep until the lock's state changes, with its word released */
static int rw_wait(pthread_rwlock_t *l, const struct timespec *abs,
                   clockid_t clock)
{
    int s = __atomic_load_n(&l->__seq, __ATOMIC_RELAXED);
    __word_unlock(&l->__lock);
    long ns = __thr_until(abs, clock);
    if (abs && ns == 0) {
        __word_lock(&l->__lock, NULL, CLOCK_REALTIME);
        return ETIMEDOUT;
    }
    sleep_on(&l->__seq, s, ns);
    __word_lock(&l->__lock, NULL, CLOCK_REALTIME);
    return 0;
}

static int rdlock(pthread_rwlock_t *l, int try, const struct timespec *abs,
                  clockid_t clock)
{
    unsigned long me = __os_thread_self();
    __word_lock(&l->__lock, NULL, CLOCK_REALTIME);
    if (l->__writer && l->__owner == me) {
        __word_unlock(&l->__lock);
        return try ? EBUSY : EDEADLK;  /* a try only says it cannot */
    }
    while (l->__writer) {
        if (try || rw_wait(l, abs, clock)) {
            __word_unlock(&l->__lock);
            return try ? EBUSY : ETIMEDOUT;
        }
    }
    if (l->__readers == UINT_MAX) {
        __word_unlock(&l->__lock);
        return EAGAIN;
    }
    l->__readers++;
    __word_unlock(&l->__lock);
    return 0;
}

static int wrlock(pthread_rwlock_t *l, int try, const struct timespec *abs,
                  clockid_t clock)
{
    unsigned long me = __os_thread_self();
    __word_lock(&l->__lock, NULL, CLOCK_REALTIME);
    if (l->__writer && l->__owner == me) {
        __word_unlock(&l->__lock);
        return try ? EBUSY : EDEADLK;
    }
    while (l->__writer || l->__readers) {
        if (try || rw_wait(l, abs, clock)) {
            __word_unlock(&l->__lock);
            return try ? EBUSY : ETIMEDOUT;
        }
    }
    l->__writer = 1;
    l->__owner = me;
    __word_unlock(&l->__lock);
    return 0;
}

WEAK int pthread_rwlock_rdlock(pthread_rwlock_t *l) { return rdlock(l, 0, NULL, 0); }
WEAK int pthread_rwlock_tryrdlock(pthread_rwlock_t *l) { return rdlock(l, 1, NULL, 0); }
WEAK int pthread_rwlock_wrlock(pthread_rwlock_t *l) { return wrlock(l, 0, NULL, 0); }
WEAK int pthread_rwlock_trywrlock(pthread_rwlock_t *l) { return wrlock(l, 1, NULL, 0); }

WEAK int pthread_rwlock_timedrdlock(pthread_rwlock_t *__restrict l,
                                    const struct timespec *__restrict abstime)
{
    return bad_time(abstime) ? EINVAL : rdlock(l, 0, abstime, CLOCK_REALTIME);
}

WEAK int pthread_rwlock_timedwrlock(pthread_rwlock_t *__restrict l,
                                    const struct timespec *__restrict abstime)
{
    return bad_time(abstime) ? EINVAL : wrlock(l, 0, abstime, CLOCK_REALTIME);
}

WEAK int pthread_rwlock_clockrdlock(pthread_rwlock_t *__restrict l,
                                    clockid_t clock,
                                    const struct timespec *__restrict abstime)
{
    if (bad_time(abstime) || bad_clock(clock))
        return EINVAL;
    return rdlock(l, 0, abstime, clock);
}

WEAK int pthread_rwlock_clockwrlock(pthread_rwlock_t *__restrict l,
                                    clockid_t clock,
                                    const struct timespec *__restrict abstime)
{
    if (bad_time(abstime) || bad_clock(clock))
        return EINVAL;
    return wrlock(l, 0, abstime, clock);
}

WEAK int pthread_rwlock_unlock(pthread_rwlock_t *l)
{
    __word_lock(&l->__lock, NULL, CLOCK_REALTIME);
    if (l->__writer) {
        if (l->__owner != __os_thread_self()) {
            __word_unlock(&l->__lock);
            return EPERM;
        }
        l->__writer = 0;
        l->__owner = 0;
    } else if (l->__readers) {
        l->__readers--;
    } else {
        __word_unlock(&l->__lock);
        return EPERM;
    }
    int wake = !l->__writer && !l->__readers;
    if (wake)
        __atomic_fetch_add(&l->__seq, 1, __ATOMIC_RELEASE);
    __word_unlock(&l->__lock);
    if (wake)
        __os_futex_wake(&l->__seq, -1);
    return 0;
}

/* ---- barriers ----------------------------------------------------------------- */

WEAK int pthread_barrierattr_init(pthread_barrierattr_t *a)
{
    a->__pshared = PTHREAD_PROCESS_PRIVATE;
    return 0;
}

WEAK int pthread_barrierattr_destroy(pthread_barrierattr_t *a) { (void)a; return 0; }

WEAK int pthread_barrierattr_getpshared(const pthread_barrierattr_t *__restrict a,
                                        int *__restrict s)
{
    *s = a->__pshared;
    return 0;
}

WEAK int pthread_barrierattr_setpshared(pthread_barrierattr_t *a, int s)
{
    int e = pshared(s);
    if (!e)
        a->__pshared = s;
    return e;
}

WEAK int pthread_barrier_init(pthread_barrier_t *__restrict b,
                              const pthread_barrierattr_t *__restrict a,
                              unsigned count)
{
    (void)a;
    if (count == 0)
        return EINVAL;
    memset(b, 0, sizeof *b);
    b->__count = count;
    return 0;
}

WEAK int pthread_barrier_destroy(pthread_barrier_t *b)
{
    return b->__waiting ? EBUSY : 0;
}

/* The last to arrive opens the barrier for this generation and is told
 * so; the generation number is what the others wait on, so the barrier
 * can be reused at once without a late waker seeing the next round. */
WEAK int pthread_barrier_wait(pthread_barrier_t *b)
{
    __word_lock(&b->__lock, NULL, CLOCK_REALTIME);
    if (++b->__waiting == b->__count) {
        b->__waiting = 0;
        __atomic_fetch_add(&b->__seq, 1, __ATOMIC_RELEASE);
        __word_unlock(&b->__lock);
        __os_futex_wake(&b->__seq, -1);
        return PTHREAD_BARRIER_SERIAL_THREAD;
    }
    int s = __atomic_load_n(&b->__seq, __ATOMIC_RELAXED);
    __word_unlock(&b->__lock);
    while (__atomic_load_n(&b->__seq, __ATOMIC_ACQUIRE) == s)
        sleep_on(&b->__seq, s, -1);
    return 0;
}

/* ---- spin locks ---------------------------------------------------------------- */

WEAK int pthread_spin_init(pthread_spinlock_t *l, int ps)
{
    int e = pshared(ps);
    if (!e)
        *l = 0;
    return e;
}

WEAK int pthread_spin_destroy(pthread_spinlock_t *l) { (void)l; return 0; }

WEAK int pthread_spin_lock(pthread_spinlock_t *l)
{
    while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE))
        while (__atomic_load_n(l, __ATOMIC_RELAXED))
            __os_thread_yield();
    return 0;
}

WEAK int pthread_spin_trylock(pthread_spinlock_t *l)
{
    return __atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE) ? EBUSY : 0;
}

WEAK int pthread_spin_unlock(pthread_spinlock_t *l)
{
    __atomic_store_n(l, 0, __ATOMIC_RELEASE);
    return 0;
}
