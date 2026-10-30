/* C11 threads: POSIX's, under C's names (pthread.c holds the one
 * implementation -- the thread table, thread-specific data, the futex
 * word), and the same seam the C++ <mutex> and <thread> use, so a target
 * that gains threads lights up all three at once.
 *
 * C11's timed waits take a DEADLINE on TIME_UTC, not a duration -- the
 * reading this file once got wrong, which turned any real deadline into
 * a wait of decades. */
#define _GNU_SOURCE 1
#include <threads.h>
#include <pthread.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../../os/backend.h"
#include "thr.h"

/* ---- threads ------------------------------------------------------------- */

/* A C thread's function returns an int and a POSIX one a pointer: the
 * int travels in the pointer. */
struct c11 { thrd_start_t fn; void *arg; };

static void *c11_start(void *p)
{
    struct c11 c = *(struct c11 *)p;
    free(p);
    return (void *)(intptr_t)c.fn(c.arg);
}

int thrd_create(thrd_t *thr, thrd_start_t func, void *arg)
{
    if (!thr || !func)
        return thrd_error;
    struct c11 *c = malloc(sizeof *c);
    if (!c)
        return thrd_nomem;
    c->fn = func;
    c->arg = arg;
    int e = __thr_create(thr, NULL, c11_start, c);
    if (e) {
        free(c);
        /* thrd_error, not thrd_nomem, where there are no threads: a
         * caller told nomem would sensibly retry */
        return e == ENOMEM ? thrd_nomem : thrd_error;
    }
    return thrd_success;
}

int thrd_equal(thrd_t a, thrd_t b) { return a == b; }

thrd_t thrd_current(void) { return (thrd_t)__os_thread_self(); }

void thrd_yield(void) { __os_thread_yield(); }

int thrd_sleep(const struct timespec *duration, struct timespec *remaining)
{
    if (!duration)
        return -1;
    long ns = duration->tv_sec * 1000000000L + duration->tv_nsec;
    if (__os_sleep_ns(ns) != 0) {
        /* Nothing slept, so nothing remains to sleep: reporting the
         * whole duration would invite a caller to loop forever. */
        if (remaining) {
            remaining->tv_sec = 0;
            remaining->tv_nsec = 0;
        }
        return -1;
    }
    if (remaining) {
        remaining->tv_sec = 0;
        remaining->tv_nsec = 0;
    }
    return 0;
}

/* The program's first thread ending this way ends the program when the
 * last thread does, with EXIT_SUCCESS, as C11 7.26.5.5 says. */
_Noreturn void thrd_exit(int res)
{
    pthread_exit((void *)(intptr_t)res);
}

int thrd_detach(thrd_t thr)
{
    return pthread_detach(thr) ? thrd_error : thrd_success;
}

int thrd_join(thrd_t thr, int *res)
{
    void *v;
    if (pthread_join(thr, &v))
        return thrd_error;
    if (res)
        *res = (int)(intptr_t)v;
    return thrd_success;
}

/* ---- mutexes ------------------------------------------------------------ */

int mtx_init(mtx_t *mtx, int type)
{
    if (!mtx)
        return thrd_error;
    mtx->__state = 0;
    mtx->__type = type;
    mtx->__owner = 0;
    mtx->__count = 0;
    return thrd_success;
}

void mtx_destroy(mtx_t *mtx) { (void)mtx; }

static int mtx_take(mtx_t *mtx, int try, const struct timespec *abs)
{
    if (!mtx)
        return thrd_error;
    unsigned long me = __os_thread_self();
    if ((mtx->__type & mtx_recursive) && mtx->__count &&
        mtx->__owner == me) {
        mtx->__count++;
        return thrd_success;
    }
    if (try) {
        if (!__word_trylock(&mtx->__state))
            return thrd_busy;
    } else if (__word_lock(&mtx->__state, abs, CLOCK_REALTIME)) {
        return thrd_timedout;
    }
    mtx->__owner = me;
    mtx->__count = 1;
    return thrd_success;
}

int mtx_lock(mtx_t *mtx) { return mtx_take(mtx, 0, NULL); }
int mtx_trylock(mtx_t *mtx) { return mtx_take(mtx, 1, NULL); }

int mtx_timedlock(mtx_t *__restrict mtx, const struct timespec *__restrict ts)
{
    if (!ts || ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000L)
        return thrd_error;
    return mtx_take(mtx, 0, ts);
}

int mtx_unlock(mtx_t *mtx)
{
    if (!mtx)
        return thrd_error;
    if ((mtx->__type & mtx_recursive) && mtx->__count > 1) {
        mtx->__count--;
        return thrd_success;
    }
    mtx->__owner = 0;
    mtx->__count = 0;
    __word_unlock(&mtx->__state);
    return thrd_success;
}

/* ---- condition variables ------------------------------------------------ */

int cnd_init(cnd_t *cond)
{
    if (!cond)
        return thrd_error;
    cond->__seq = 0;
    return thrd_success;
}

void cnd_destroy(cnd_t *cond) { (void)cond; }

int cnd_signal(cnd_t *cond)
{
    if (!cond)
        return thrd_error;
    __atomic_fetch_add(&cond->__seq, 1, __ATOMIC_RELEASE);
    __os_futex_wake(&cond->__seq, 1);
    return thrd_success;
}

int cnd_broadcast(cnd_t *cond)
{
    if (!cond)
        return thrd_error;
    __atomic_fetch_add(&cond->__seq, 1, __ATOMIC_RELEASE);
    __os_futex_wake(&cond->__seq, -1);
    return thrd_success;
}

static int cnd_wait_until(cnd_t *cond, mtx_t *mtx, const struct timespec *abs)
{
    if (!cond || !mtx)
        return thrd_error;
    /* read with the mutex held: a signal after this moves the number the
     * sleep compares, so it cannot fall between unlock and sleep */
    int s = __atomic_load_n(&cond->__seq, __ATOMIC_RELAXED);
    unsigned count = mtx->__count;
    unsigned long owner = mtx->__owner;
    mtx->__owner = 0;
    mtx->__count = 0;
    __word_unlock(&mtx->__state);
    long ns = __thr_until(abs, CLOCK_REALTIME);
    if (!abs || ns > 0) {
        int saved = errno;
        if (__os_futex_wait(&cond->__seq, s, ns) < 0 && errno == ENOSYS) {
            /* one thread, nothing to signal it: a deadline is waited out */
            __os_thread_yield();
            while (abs && __atomic_load_n(&cond->__seq, __ATOMIC_RELAXED) == s &&
                   __thr_until(abs, CLOCK_REALTIME) > 0)
                __os_thread_yield();
        }
        errno = saved;
    }
    __word_lock(&mtx->__state, NULL, CLOCK_REALTIME);
    mtx->__owner = owner;
    mtx->__count = count;
    if (abs && __atomic_load_n(&cond->__seq, __ATOMIC_RELAXED) == s &&
        __thr_until(abs, CLOCK_REALTIME) == 0)
        return thrd_timedout;
    return thrd_success;
}

int cnd_wait(cnd_t *cond, mtx_t *mtx) { return cnd_wait_until(cond, mtx, NULL); }

int cnd_timedwait(cnd_t *__restrict cond, mtx_t *__restrict mtx,
                  const struct timespec *__restrict ts)
{
    if (!ts || ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000L)
        return thrd_error;
    return cnd_wait_until(cond, mtx, ts);
}

/* ---- call_once ------------------------------------------------------------ */

void call_once(once_flag *flag, void (*func)(void))
{
    if (flag && func)
        pthread_once(&flag->__state, func);
}

/* ---- thread-specific storage ------------------------------------------- */

/* POSIX's keys: a table per thread in its record, so no thread-local
 * storage is needed underneath -- one key works on every target. */
int tss_create(tss_t *key, tss_dtor_t dtor)
{
    pthread_key_t k;
    if (!key || pthread_key_create(&k, dtor))
        return thrd_error;
    *key = (tss_t)k;
    return thrd_success;
}

void *tss_get(tss_t key) { return pthread_getspecific((pthread_key_t)key); }

int tss_set(tss_t key, void *val)
{
    return pthread_setspecific((pthread_key_t)key, val) ? thrd_error
                                                        : thrd_success;
}

void tss_delete(tss_t key) { pthread_key_delete((pthread_key_t)key); }
