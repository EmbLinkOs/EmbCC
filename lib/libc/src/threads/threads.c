/* C11 threads, over the OS seam.
 *
 * The same primitives the C++ <mutex> and <thread> use, so a target
 * that gains threads lights up both at once and neither library knows
 * which OS it is on. See lib/libc/os/backend.h.
 *
 * The locks here are the three-state futex mutex, for the reason given
 * at length in lib/libcxx/include/mutex: the uncontended path is one
 * compare-exchange and never enters the kernel, and two states are not
 * enough because unlock has to know whether anybody is waiting.
 */
#include <threads.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "../../os/backend.h"

/* ---- threads ----------------------------------------------------------- */

/* A C thread's function returns an int and the seam's takes none, so the
 * value is held here until somebody joins. One record per live thread,
 * in a small fixed table: a target with no threads never uses it, and
 * one with threads has a bounded number of C11 threads by construction
 * -- there is no way to create one without going through here. */
#define MAXTHREADS 64

struct rec {
    thrd_t id;
    thrd_start_t fn;
    void *arg;
    int result;
    int done;
    int used;
};

static struct rec g_recs[MAXTHREADS];

static struct rec *rec_of(thrd_t id)
{
    for (int i = 0; i < MAXTHREADS; i++)
        if (g_recs[i].used && g_recs[i].id == id)
            return &g_recs[i];
    return NULL;
}

static void trampoline(void *p)
{
    struct rec *r = (struct rec *)p;
    r->result = r->fn(r->arg);
    r->done = 1;
}

int thrd_create(thrd_t *thr, thrd_start_t func, void *arg)
{
    if (!thr || !func)
        return thrd_error;
    struct rec *r = NULL;
    for (int i = 0; i < MAXTHREADS; i++)
        if (!g_recs[i].used) {
            r = &g_recs[i];
            break;
        }
    if (!r)
        return thrd_nomem;
    r->fn = func;
    r->arg = arg;
    r->result = 0;
    r->done = 0;
    r->used = 1;
    unsigned long id = 0;
    if (__os_thread_create(&id, trampoline, r) != 0) {
        r->used = 0;
        /* thrd_error, not thrd_nomem: there is nothing to run out of on
         * a target with no threads, and a caller told nomem would
         * sensibly retry. */
        return errno == ENOMEM ? thrd_nomem : thrd_error;
    }
    r->id = id;
    *thr = id;
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

_Noreturn void thrd_exit(int res)
{
    struct rec *r = rec_of((thrd_t)__os_thread_self());
    if (r) {
        r->result = res;
        r->done = 1;
    }
    /* With no threads the only thread is the program, so ending it is
     * ending the program -- which is what the standard says for the
     * main thread anyway. */
    exit(res);
}

int thrd_detach(thrd_t thr)
{
    struct rec *r = rec_of(thr);
    if (__os_thread_detach(thr) != 0)
        return thrd_error;
    if (r)
        r->used = 0;              /* nobody will join it: release the slot */
    return thrd_success;
}

int thrd_join(thrd_t thr, int *res)
{
    if (__os_thread_join(thr) != 0)
        return thrd_error;
    struct rec *r = rec_of(thr);
    if (res)
        *res = r ? r->result : 0;
    if (r)
        r->used = 0;
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

static int mtx_try_raw(mtx_t *m)
{
    int expected = 0;
    return __atomic_compare_exchange_n(&m->__state, &expected, 1, 0,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

int mtx_trylock(mtx_t *mtx)
{
    if (!mtx)
        return thrd_error;
    unsigned long me = __os_thread_self();
    if ((mtx->__type & mtx_recursive) && mtx->__count &&
        mtx->__owner == me) {
        mtx->__count++;
        return thrd_success;
    }
    if (!mtx_try_raw(mtx))
        return thrd_busy;
    mtx->__owner = me;
    mtx->__count = 1;
    return thrd_success;
}

int mtx_lock(mtx_t *mtx)
{
    if (!mtx)
        return thrd_error;
    unsigned long me = __os_thread_self();
    if ((mtx->__type & mtx_recursive) && mtx->__count &&
        mtx->__owner == me) {
        mtx->__count++;
        return thrd_success;
    }
    for (;;) {
        if (mtx_try_raw(mtx))
            break;
        /* Announce the wait BEFORE sleeping, or an unlock in between
         * sees state 1, skips the wake, and leaves this thread asleep
         * forever. */
        int two = 2;
        __atomic_exchange_n(&mtx->__state, two, __ATOMIC_ACQUIRE);
        if (__atomic_load_n(&mtx->__state, __ATOMIC_RELAXED) == 2)
            if (__os_futex_wait(&mtx->__state, 2, -1) < 0)
                __os_thread_yield();
    }
    mtx->__owner = me;
    mtx->__count = 1;
    return thrd_success;
}

int mtx_timedlock(mtx_t *__restrict mtx, const struct timespec *__restrict ts)
{
    if (!mtx)
        return thrd_error;
    if (mtx_trylock(mtx) == thrd_success)
        return thrd_success;
    long ns = ts ? ts->tv_sec * 1000000000L + ts->tv_nsec : 0;
    /* A bounded wait must END. With no futex there is no second thread
     * that could release it, so one more attempt is the whole of the
     * wait -- see lib/libcxx/include/mutex on why polling a deadline is
     * worse than useless when the clock may not advance either. */
    if (__os_futex_wait(&mtx->__state, mtx->__state, ns) < 0)
        return mtx_trylock(mtx) == thrd_success ? thrd_success
                                                : thrd_timedout;
    return mtx_trylock(mtx) == thrd_success ? thrd_success : thrd_timedout;
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
    if (__atomic_exchange_n(&mtx->__state, 0, __ATOMIC_RELEASE) == 2)
        __os_futex_wake(&mtx->__state, 1);
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
    /* Bump first, wake second: a waiter about to sleep finds the
     * number changed and does not sleep, so an early notify is not
     * lost. */
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

int cnd_wait(cnd_t *cond, mtx_t *mtx)
{
    if (!cond || !mtx)
        return thrd_error;
    /* The sequence is read while the mutex is HELD, so a signal cannot
     * slip between this read and the sleep without changing the number
     * the sleep tests. That indivisibility is the whole reason a
     * condition variable is a primitive. */
    int s = __atomic_load_n(&cond->__seq, __ATOMIC_RELAXED);
    mtx_unlock(mtx);
    if (__os_futex_wait(&cond->__seq, s, -1) < 0)
        __os_thread_yield();
    mtx_lock(mtx);
    return thrd_success;
}

int cnd_timedwait(cnd_t *__restrict cond, mtx_t *__restrict mtx,
                  const struct timespec *__restrict ts)
{
    if (!cond || !mtx)
        return thrd_error;
    long ns = ts ? ts->tv_sec * 1000000000L + ts->tv_nsec : 0;
    int s = __atomic_load_n(&cond->__seq, __ATOMIC_RELAXED);
    mtx_unlock(mtx);
    int r = __os_futex_wait(&cond->__seq, s, ns);
    mtx_lock(mtx);
    /* Timed out only if the sequence really did not move: a wakeup that
     * raced with the timeout is a wakeup. */
    if (r < 0 && __atomic_load_n(&cond->__seq, __ATOMIC_RELAXED) == s)
        return thrd_timedout;
    return thrd_success;
}

/* ---- call_once ----------------------------------------------------------- */

void call_once(once_flag *flag, void (*func)(void))
{
    if (!flag || !func)
        return;
    /* Acquire on the fast path: a thread that sees "done" must also see
     * everything the initializing thread wrote. */
    if (__atomic_load_n(&flag->__state, __ATOMIC_ACQUIRE) == 2)
        return;
    for (;;) {
        int e = 0;
        if (__atomic_compare_exchange_n(&flag->__state, &e, 1, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            func();
            __atomic_store_n(&flag->__state, 2, __ATOMIC_RELEASE);
            __os_futex_wake(&flag->__state, -1);
            return;
        }
        if (e == 2)
            return;
        if (__os_futex_wait(&flag->__state, 1, -1) < 0)
            __os_thread_yield();
    }
}

/* ---- thread-specific storage --------------------------------------------- */

/* Needs thread-local storage underneath, which this compiler does not
 * have. Reporting failure is the only honest answer: a key that every
 * thread shared would be a global under another name, and the failure
 * would not appear until two threads corrupted each other. */
int tss_create(tss_t *key, tss_dtor_t dtor)
{
    (void)key;
    (void)dtor;
    return thrd_error;
}

void *tss_get(tss_t key) { (void)key; return NULL; }
int tss_set(tss_t key, void *val) { (void)key; (void)val; return thrd_error; }
void tss_delete(tss_t key) { (void)key; }
