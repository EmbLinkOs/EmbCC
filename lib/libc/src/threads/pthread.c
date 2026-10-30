/* POSIX threads, over the OS seam (lib/libc/os/backend.h), and the core
 * C11's <threads.h> is built on too (threads.c).
 *
 * ---- the thread table ------------------------------------------------------
 *
 * One record per thread this library started, linked under one lock, and
 * one made on first need for a thread it did not start (the program's
 * first, or a C++ std::thread, which goes to the seam directly). A
 * record holds what a thread leaves behind for its joiner -- its result
 * -- and what it carries while it runs: its thread-specific data, its
 * name, and the jump that pthread_exit takes.
 *
 * pthread_exit is a longjmp back to the start of the thread, where its
 * keys' destructors run and the record is marked done: the seam ends a
 * thread when its function returns, and has no other way to. Frames it
 * jumps over are not unwound -- a C++ destructor between pthread_exit
 * and the thread's function does not run, as it does not under musl.
 *
 * ---- what a backend without threads gets ----------------------------------
 *
 * The same code with one thread: pthread_create fails with EAGAIN, every
 * lock is taken at once, and thread-specific data works -- one record,
 * the program's. A timed wait waits no longer than one try, since there
 * is nobody who could make it succeed.
 */
#define _GNU_SOURCE 1
#include <pthread.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <unistd.h>
#include <signal.h>
#include <sys/syscall.h>

#include "../../os/backend.h"
#include "../internal/lock.h"
#include "thr.h"

/* ---- the futex word ------------------------------------------------------- */

long __thr_until(const struct timespec *abs, clockid_t clock)
{
    if (!abs)
        return -1;
    struct timespec now;
    if (clock_gettime(clock, &now) != 0)
        return 0;                       /* no clock: the deadline is now */
    long long ns = (long long)(abs->tv_sec - now.tv_sec) * 1000000000LL +
                   (abs->tv_nsec - now.tv_nsec);
    if (ns <= 0)
        return 0;
    /* a long may be 32 bits: wait that long and look again */
    return ns > LONG_MAX ? LONG_MAX : (long)ns;
}

static void sleep_on(volatile int *w, int expected, long ns)
{
    int saved = errno;
    if (__os_futex_wait(w, expected, ns) < 0 && errno == ENOSYS)
        __os_thread_yield();
    errno = saved;
}

int __word_trylock(volatile int *w)
{
    int z = 0;
    return __atomic_compare_exchange_n(w, &z, 1, 0, __ATOMIC_ACQUIRE,
                                       __ATOMIC_RELAXED);
}

/* The three-state lock in its standard form: the exchange that marks the
 * word contended is also the attempt to take it, so a lock released
 * between a failed try and the sleep is taken, never slept on. */
int __word_lock(volatile int *w, const struct timespec *abs, clockid_t clock)
{
    int c = 0;
    if (__atomic_compare_exchange_n(w, &c, 1, 0, __ATOMIC_ACQUIRE,
                                    __ATOMIC_RELAXED))
        return 0;
    if (c != 2)
        c = __atomic_exchange_n(w, 2, __ATOMIC_ACQUIRE);
    while (c != 0) {
        long ns = __thr_until(abs, clock);
        if (abs && ns == 0)
            return ETIMEDOUT;
        sleep_on(w, 2, ns);
        c = __atomic_exchange_n(w, 2, __ATOMIC_ACQUIRE);
    }
    return 0;
}

void __word_unlock(volatile int *w)
{
    if (__atomic_exchange_n(w, 0, __ATOMIC_RELEASE) == 2)
        __os_futex_wake(w, 1);
}

static int bad_time(const struct timespec *t)
{
    return !t || t->tv_nsec < 0 || t->tv_nsec >= 1000000000L;
}

/* ---- the thread table ----------------------------------------------------- */

static __lock_t g_tlock = LOCK_INIT;
static struct __thr *g_threads;
static unsigned long g_main_id;
static int g_main_known;
#if defined(__linux__)
static __thread struct __thr *g_me;   /* a lookup per call otherwise */
#endif

static void link_thr(struct __thr *t)
{
    t->next = g_threads;
    g_threads = t;
}

static void unlink_thr(struct __thr *t)
{
    for (struct __thr **p = &g_threads; *p; p = &(*p)->next)
        if (*p == t) {
            *p = t->next;
            return;
        }
}

/* a running thread's record: a finished one may still hold its id until
 * it is joined, and the kernel may already have given the id away */
static struct __thr *find_live(unsigned long id)
{
    for (struct __thr *t = g_threads; t; t = t->next)
        if (t->id == id && !t->done)
            return t;
    return NULL;
}

static struct __thr *find_any(unsigned long id)
{
    struct __thr *live = find_live(id);
    if (live)
        return live;
    for (struct __thr *t = g_threads; t; t = t->next)
        if (t->id == id && t->ours)
            return t;
    return NULL;
}

struct __thr *__thr_self(void)
{
#if defined(__linux__)
    if (g_me)
        return g_me;
#endif
    unsigned long me = __os_thread_self();
    __lock(&g_tlock);
    if (!g_main_known) {
        /* the first thread to ask, before any other was started here;
         * from now on a fork's child must hear that it has one thread */
        g_main_id = me;
        g_main_known = 1;
        __atfork_hook = __pthread_atfork_run;
    }
    struct __thr *t = find_live(me);
    if (!t) {
        t = calloc(1, sizeof *t);
        if (t) {
            t->id = me;
            t->tid = gettid();
            link_thr(t);
        }
    }
    __unlock(&g_tlock);
    if (!t)                    /* no memory: a record nobody else shares */
        abort();
#if defined(__linux__)
    g_me = t;
#endif
    return t;
}

/* ---- thread-specific data ------------------------------------------------- */

static __lock_t g_klock = LOCK_INIT;
static struct { int used; void (*dtor)(void *); } g_keys[__THR_KEYS];

WEAK int pthread_key_create(pthread_key_t *key, void (*dtor)(void *))
{
    __lock(&g_klock);
    for (unsigned k = 0; k < __THR_KEYS; k++)
        if (!g_keys[k].used) {
            g_keys[k].used = 1;
            g_keys[k].dtor = dtor;
            /* a key used before belongs to nobody now: no thread may
             * find the old one's value under the new one */
            __lock(&g_tlock);
            for (struct __thr *t = g_threads; t; t = t->next)
                t->tsd[k] = NULL;
            __unlock(&g_tlock);
            __unlock(&g_klock);
            *key = k;
            return 0;
        }
    __unlock(&g_klock);
    return EAGAIN;
}

WEAK int pthread_key_delete(pthread_key_t key)
{
    if (key >= __THR_KEYS)
        return EINVAL;
    __lock(&g_klock);
    int was = g_keys[key].used;
    g_keys[key].used = 0;
    g_keys[key].dtor = NULL;
    __unlock(&g_klock);
    return was ? 0 : EINVAL;
}

WEAK void *pthread_getspecific(pthread_key_t key)
{
    return key < __THR_KEYS ? __thr_self()->tsd[key] : NULL;
}

WEAK int pthread_setspecific(pthread_key_t key, const void *value)
{
    if (key >= __THR_KEYS || !g_keys[key].used)
        return EINVAL;
    __thr_self()->tsd[key] = (void *)value;
    return 0;
}

/* POSIX's rounds: a destructor may set a value again, and up to
 * PTHREAD_DESTRUCTOR_ITERATIONS passes are made while any is left */
static void run_dtors(struct __thr *t)
{
    for (int round = 0; round < __THR_DTOR_ROUNDS; round++) {
        int any = 0;
        for (unsigned k = 0; k < __THR_KEYS; k++) {
            void *v = t->tsd[k];
            void (*d)(void *) = g_keys[k].used ? g_keys[k].dtor : NULL;
            if (v && d) {
                t->tsd[k] = NULL;
                d(v);
                any = 1;
            }
        }
        if (!any)
            return;
    }
}

/* ---- threads -------------------------------------------------------------- */

static void finish(struct __thr *t)
{
    __lock(&g_tlock);
    t->done = 1;
    /* detached and already handed over: nobody will join, so the record
     * goes now. A thread detached before pthread_create returned is
     * freed there instead (registered == 0). */
    int drop = t->detached == 1 && t->registered;
    if (drop)
        unlink_thr(t);
    __unlock(&g_tlock);
    if (drop)
        free(t);
}

static void start(void *p)
{
    struct __thr *t = p;
    __atomic_store_n(&t->id, __os_thread_self(), __ATOMIC_RELEASE);
    t->tid = gettid();
#if defined(__linux__)
    g_me = t;
#endif
    if (t->aff) {              /* pthread_attr_setaffinity_np's */
        sched_setaffinity(0, t->affsize, t->aff);
        free(t->aff);
        t->aff = 0;
    }
    if (!setjmp(t->exit_to))
        t->result = t->fn(t->arg);
    run_dtors(t);
    finish(t);
}

const pthread_attr_t *__thr_default_attr(void);

/* errno-style, for both spellings: ENOMEM and the backend's own reason
 * are kept apart here and mapped by each caller */
int __thr_create(pthread_t *out, const void *attr_,
                 void *(*fn)(void *), void *arg)
{
    const pthread_attr_t *a = attr_;
    if (!a)
        a = __thr_default_attr();   /* pthread_setattr_default_np's */
    if (!out || !fn)
        return EINVAL;
    if (a && a->__stackaddr)
        return EINVAL;         /* the seam starts a thread on its own stack */
    if (a && a->__inherit == PTHREAD_EXPLICIT_SCHED &&
        (a->__policy != SCHED_OTHER || a->__param.sched_priority != 0))
        return EPERM;          /* no policy can be set here: say so */
    struct __thr *t = calloc(1, sizeof *t);
    if (!t)
        return ENOMEM;
    t->fn = fn;
    t->arg = arg;
    t->ours = 1;
    t->detached = a && a->__detach == PTHREAD_CREATE_DETACHED;
    if (a && a->__affinity) {
        t->aff = malloc(a->__affsize);
        if (!t->aff) {
            free(t);
            return ENOMEM;
        }
        memcpy(t->aff, a->__affinity, a->__affsize);
        t->affsize = a->__affsize;
    }
    (void)__thr_self();        /* the creator has a record before any child */
    __lock(&g_tlock);
    link_thr(t);
    __unlock(&g_tlock);
    unsigned long id = 0;
    int saved = errno;
    if (__os_thread_create(&id, start, t) != 0) {
        int e = errno;
        errno = saved;
        __lock(&g_tlock);
        unlink_thr(t);
        __unlock(&g_tlock);
        free(t);
        return e == ENOMEM ? ENOMEM : EAGAIN;
    }
    errno = saved;
    __lock(&g_tlock);
    t->id = id;
    t->registered = 1;
    int gone = t->detached && t->done;
    if (gone)
        unlink_thr(t);
    __unlock(&g_tlock);
    if (t->detached)
        __os_thread_detach(id);
    if (gone)
        free(t);
    *out = id;
    return 0;
}

WEAK int pthread_create(pthread_t *__restrict out,
                        const pthread_attr_t *__restrict a,
                        void *(*fn)(void *), void *__restrict arg)
{
    int e = __thr_create(out, a, fn, arg);
    return e == ENOMEM ? EAGAIN : e;
}

WEAK pthread_t pthread_self(void) { return __os_thread_self(); }

WEAK int pthread_equal(pthread_t a, pthread_t b) { return a == b; }

static int join(pthread_t id, void **result, int wait,
                const struct timespec *abs, clockid_t clock)
{
    if (id == __os_thread_self())
        return EDEADLK;
    __lock(&g_tlock);
    struct __thr *t = find_any(id);
    if (!t || !t->ours) {
        __unlock(&g_tlock);
        return ESRCH;
    }
    if (t->detached) {
        __unlock(&g_tlock);
        return EINVAL;
    }
    if (!wait && !t->done) {
        __unlock(&g_tlock);
        return EBUSY;
    }
    __unlock(&g_tlock);
    if (abs) {
        /* the seam's join has no deadline: look until it is done */
        while (!__atomic_load_n(&t->done, __ATOMIC_ACQUIRE)) {
            long ns = __thr_until(abs, clock);
            if (ns == 0)
                return ETIMEDOUT;
            __os_sleep_ns(ns < 1000000 ? ns : 1000000);
        }
    }
    __lock(&g_tlock);
    t->detached = 2;           /* joined: nobody else may */
    __unlock(&g_tlock);
    int saved = errno;
    __os_thread_join(id);
    errno = saved;
    if (result)
        *result = t->result;
    __lock(&g_tlock);
    unlink_thr(t);
    __unlock(&g_tlock);
    free(t);
    return 0;
}

WEAK int pthread_join(pthread_t t, void **result)
{
    return join(t, result, 1, NULL, CLOCK_REALTIME);
}

WEAK int pthread_tryjoin_np(pthread_t t, void **result)
{
    return join(t, result, 0, NULL, CLOCK_REALTIME);
}

WEAK int pthread_clockjoin_np(pthread_t t, void **result, clockid_t clock,
                              const struct timespec *abstime)
{
    if (bad_time(abstime) || (clock != CLOCK_REALTIME && clock != CLOCK_MONOTONIC))
        return EINVAL;
    return join(t, result, 1, abstime, clock);
}

WEAK int pthread_timedjoin_np(pthread_t t, void **result,
                              const struct timespec *abstime)
{
    if (bad_time(abstime))
        return EINVAL;
    return join(t, result, 1, abstime, CLOCK_REALTIME);
}

WEAK int pthread_detach(pthread_t id)
{
    __lock(&g_tlock);
    struct __thr *t = find_any(id);
    if (!t || !t->ours) {
        __unlock(&g_tlock);
        return ESRCH;
    }
    if (t->detached) {
        __unlock(&g_tlock);
        return EINVAL;
    }
    t->detached = 1;
    int done = t->done && t->registered;
    if (done)
        unlink_thr(t);
    __unlock(&g_tlock);
    int saved = errno;
    __os_thread_detach(id);
    errno = saved;
    if (done)
        free(t);
    return 0;
}

static int others_running(unsigned long me)
{
    __lock(&g_tlock);
    int n = 0;
    for (struct __thr *t = g_threads; t; t = t->next)
        if (t->ours && !t->done && t->id != me)
            n++;
    __unlock(&g_tlock);
    return n;
}

WEAK _Noreturn void pthread_exit(void *result)
{
    struct __thr *t = __thr_self();
    if (t->ours) {
        t->result = result;
        longjmp(t->exit_to, 1);
    }
    run_dtors(t);
    if (g_main_known && t->id == g_main_id) {
        /* the program's first thread: the process ends when its last
         * thread does, with status 0 */
        while (others_running(t->id))
            if (__os_sleep_ns(1000000) != 0)
                __os_thread_yield();
        exit(0);
    }
    /* a thread started outside this library cannot be unwound to its
     * start; it stops here for good */
    for (;;)
        if (__os_sleep_ns(1000000000L) != 0)
            __os_thread_yield();
}

WEAK int pthread_once(pthread_once_t *once, void (*fn)(void))
{
    if (__atomic_load_n(once, __ATOMIC_ACQUIRE) == 2)
        return 0;
    for (;;) {
        int e = 0;
        if (__atomic_compare_exchange_n(once, &e, 1, 0, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED)) {
            fn();
            __atomic_store_n(once, 2, __ATOMIC_RELEASE);
            __os_futex_wake(once, -1);
            return 0;
        }
        if (e == 2)
            return 0;
        sleep_on(once, 1, -1);
    }
}

/* ---- fork's handlers ------------------------------------------------------- */

#define ATFORK_MAX 32
static struct { void (*prepare)(void), (*parent)(void), (*child)(void); }
    g_atfork[ATFORK_MAX];
static int g_natfork;

WEAK int pthread_atfork(void (*prepare)(void), void (*parent)(void),
                        void (*child)(void))
{
    __lock(&g_tlock);
    if (g_natfork == ATFORK_MAX) {
        __unlock(&g_tlock);
        return ENOMEM;
    }
    __atfork_hook = __pthread_atfork_run;
    g_atfork[g_natfork].prepare = prepare;
    g_atfork[g_natfork].parent = parent;
    g_atfork[g_natfork].child = child;
    g_natfork++;
    __unlock(&g_tlock);
    return 0;
}

/* fork() calls this (proc.c, through the hook pthread_atfork sets: a
 * program that never names a pthread function does not link this file):
 * 0 before, 1 in the parent after, 2 in the child. POSIX's order --
 * prepare handlers in reverse of registration, the others forward. */
void __pthread_atfork_run(int phase)
{
    if (phase == 0) {
        for (int i = g_natfork - 1; i >= 0; i--)
            if (g_atfork[i].prepare)
                g_atfork[i].prepare();
        return;
    }
    if (phase == 2) {
        /* the child is one thread, the one that forked, under a new id:
         * every other record names a thread that is not there */
        struct __thr *me = __thr_self();
        g_threads = me;
        me->next = NULL;
        me->id = __os_thread_self();
        g_main_id = me->id;
        g_tlock.v = 0;
        g_klock.v = 0;
    }
    for (int i = 0; i < g_natfork; i++) {
        void (*f)(void) = phase == 1 ? g_atfork[i].parent : g_atfork[i].child;
        if (f)
            f();
    }
}

/* ---- scheduling, cancellation, names ---------------------------------------- */

static int g_concurrency;
WEAK int pthread_getconcurrency(void) { return g_concurrency; }
WEAK int pthread_setconcurrency(int level)
{
    if (level < 0)
        return EINVAL;
    g_concurrency = level;
    return 0;
}

/* Every thread runs under SCHED_OTHER at priority 0, and this library
 * has no way to change that, so asking for anything else is EPERM --
 * what an unprivileged Linux process is told. */
WEAK int pthread_getschedparam(pthread_t t, int *__restrict policy,
                               struct sched_param *__restrict param)
{
    (void)t;
    *policy = SCHED_OTHER;
    param->sched_priority = 0;
    return 0;
}

WEAK int pthread_setschedparam(pthread_t t, int policy,
                               const struct sched_param *param)
{
    (void)t;
    if (policy != SCHED_OTHER && policy != SCHED_FIFO && policy != SCHED_RR)
        return EINVAL;
    return policy == SCHED_OTHER && param->sched_priority == 0 ? 0 : EPERM;
}

WEAK int pthread_setschedprio(pthread_t t, int prio)
{
    (void)t;
    return prio == 0 ? 0 : EINVAL;
}

WEAK int pthread_getcpuclockid(pthread_t t, clockid_t *clock)
{
    if (t != __os_thread_self())
        return ENOENT;         /* only a thread's own time can be read */
    *clock = CLOCK_THREAD_CPUTIME_ID;
    return 0;
}

/* No call here is a cancellation point, so a request could never be
 * acted on: it is refused rather than accepted and ignored. */
WEAK int pthread_cancel(pthread_t t)
{
    (void)t;
    return ENOTSUP;
}

WEAK int pthread_setcancelstate(int state, int *old)
{
    if (state != PTHREAD_CANCEL_ENABLE && state != PTHREAD_CANCEL_DISABLE)
        return EINVAL;
    struct __thr *t = __thr_self();
    if (old)
        *old = t->cancel_state;
    t->cancel_state = state;
    return 0;
}

WEAK int pthread_setcanceltype(int type, int *old)
{
    if (type != PTHREAD_CANCEL_DEFERRED && type != PTHREAD_CANCEL_ASYNCHRONOUS)
        return EINVAL;
    struct __thr *t = __thr_self();
    if (old)
        *old = t->cancel_type;
    t->cancel_type = type;
    return 0;
}

WEAK void pthread_testcancel(void) {}

WEAK int pthread_yield(void) { __os_thread_yield(); return 0; }

static struct __thr *named(pthread_t id)
{
    if (id == __os_thread_self())
        return __thr_self();
    __lock(&g_tlock);
    struct __thr *t = find_live(id);
    __unlock(&g_tlock);
    return t;
}

WEAK int pthread_setname_np(pthread_t id, const char *name)
{
    if (strlen(name) >= sizeof ((struct __thr *)0)->name)
        return ERANGE;
    struct __thr *t = named(id);
    if (!t)
        return ESRCH;
    strcpy(t->name, name);
    return 0;
}

WEAK int pthread_getname_np(pthread_t id, char *name, size_t n)
{
    struct __thr *t = named(id);
    if (!t)
        return ESRCH;
    if (strlen(t->name) >= n)
        return ERANGE;
    strcpy(name, t->name);
    return 0;
}

/* ---- attributes ------------------------------------------------------------- */

#define DEFAULT_STACK (8UL * 1024 * 1024)

WEAK int pthread_attr_init(pthread_attr_t *a)
{
    memset(a, 0, sizeof *a);
    a->__stacksize = DEFAULT_STACK;
    a->__guardsize = 4096;
    return 0;
}

WEAK int pthread_attr_destroy(pthread_attr_t *a)
{
    free(a->__affinity);
    a->__affinity = 0;
    return 0;
}

WEAK int pthread_attr_getdetachstate(const pthread_attr_t *a, int *s)
{
    *s = a->__detach;
    return 0;
}

WEAK int pthread_attr_setdetachstate(pthread_attr_t *a, int s)
{
    if (s != PTHREAD_CREATE_JOINABLE && s != PTHREAD_CREATE_DETACHED)
        return EINVAL;
    a->__detach = s;
    return 0;
}

/* The size is kept and reported. Whether a thread gets it is the
 * backend's: Linux's threads have 8 MiB, mapped on demand, as glibc's
 * do. */
WEAK int pthread_attr_getstacksize(const pthread_attr_t *__restrict a,
                                   size_t *__restrict n)
{
    *n = a->__stacksize;
    return 0;
}

WEAK int pthread_attr_setstacksize(pthread_attr_t *a, size_t n)
{
    if (n < PTHREAD_STACK_MIN)
        return EINVAL;
    a->__stacksize = n;
    return 0;
}

WEAK int pthread_attr_getguardsize(const pthread_attr_t *__restrict a,
                                   size_t *__restrict n)
{
    *n = a->__guardsize;
    return 0;
}

WEAK int pthread_attr_setguardsize(pthread_attr_t *a, size_t n)
{
    a->__guardsize = n;
    return 0;
}

WEAK int pthread_attr_getstack(const pthread_attr_t *__restrict a,
                               void **__restrict addr, size_t *__restrict n)
{
    *addr = a->__stackaddr;
    *n = a->__stacksize;
    return 0;
}

/* kept, and refused by pthread_create: the seam starts every thread on
 * a stack of its own making */
WEAK int pthread_attr_setstack(pthread_attr_t *a, void *addr, size_t n)
{
    if (n < PTHREAD_STACK_MIN)
        return EINVAL;
    a->__stackaddr = addr;
    a->__stacksize = n;
    return 0;
}

WEAK int pthread_attr_getinheritsched(const pthread_attr_t *__restrict a,
                                      int *__restrict s)
{
    *s = a->__inherit;
    return 0;
}

WEAK int pthread_attr_setinheritsched(pthread_attr_t *a, int s)
{
    if (s != PTHREAD_INHERIT_SCHED && s != PTHREAD_EXPLICIT_SCHED)
        return EINVAL;
    a->__inherit = s;
    return 0;
}

WEAK int pthread_attr_getschedpolicy(const pthread_attr_t *__restrict a,
                                     int *__restrict p)
{
    *p = a->__policy;
    return 0;
}

WEAK int pthread_attr_setschedpolicy(pthread_attr_t *a, int p)
{
    if (p != SCHED_OTHER && p != SCHED_FIFO && p != SCHED_RR)
        return EINVAL;
    a->__policy = p;
    return 0;
}

WEAK int pthread_attr_getschedparam(const pthread_attr_t *__restrict a,
                                    struct sched_param *__restrict p)
{
    *p = a->__param;
    return 0;
}

WEAK int pthread_attr_setschedparam(pthread_attr_t *__restrict a,
                                    const struct sched_param *__restrict p)
{
    a->__param = *p;
    return 0;
}

WEAK int pthread_attr_getscope(const pthread_attr_t *__restrict a,
                               int *__restrict s)
{
    *s = a->__scope;
    return 0;
}

/* every thread is the kernel's: PROCESS scope is not there to have */
WEAK int pthread_attr_setscope(pthread_attr_t *a, int s)
{
    if (s == PTHREAD_SCOPE_PROCESS)
        return ENOTSUP;
    if (s != PTHREAD_SCOPE_SYSTEM)
        return EINVAL;
    a->__scope = s;
    return 0;
}

/* ---- GNU's: processors, attributes of a running thread, defaults --------------- */

WEAK int pthread_setaffinity_np(pthread_t id, size_t n, const cpu_set_t *set)
{
    struct __thr *t = named(id);
    if (!t)
        return ESRCH;
    int saved = errno, e = 0;
    if (sched_setaffinity(t->tid, n, set))
        e = errno;
    errno = saved;
    return e;
}

WEAK int pthread_getaffinity_np(pthread_t id, size_t n, cpu_set_t *set)
{
    struct __thr *t = named(id);
    if (!t)
        return ESRCH;
    int saved = errno, e = 0;
    if (sched_getaffinity(t->tid, n, set))
        e = errno;
    errno = saved;
    return e;
}

WEAK int pthread_attr_setaffinity_np(pthread_attr_t *a, size_t n,
                                     const cpu_set_t *set)
{
    void *copy = 0;
    if (set && n) {
        if (!(copy = malloc(n)))
            return ENOMEM;
        memcpy(copy, set, n);
    }
    free(a->__affinity);
    a->__affinity = copy;
    a->__affsize = copy ? n : 0;
    return 0;
}

/* none set: every processor */
WEAK int pthread_attr_getaffinity_np(const pthread_attr_t *a, size_t n,
                                     cpu_set_t *set)
{
    if (!a->__affinity) {
        memset(set, 0xff, n);
        return 0;
    }
    if (n < a->__affsize)
        return EINVAL;
    memcpy(set, a->__affinity, a->__affsize);
    memset((char *)set + a->__affsize, 0, n - a->__affsize);
    return 0;
}

/* a running thread's attributes, its stack where the backend can say */
WEAK int pthread_getattr_np(pthread_t id, pthread_attr_t *a)
{
    struct __thr *t = named(id);
    if (!t)
        return ESRCH;
    pthread_attr_init(a);
    a->__detach = t->detached == 1 ? PTHREAD_CREATE_DETACHED
                                   : PTHREAD_CREATE_JOINABLE;
    void *addr;
    size_t size;
    int saved = errno;
    if (__os_thread_stack(t->id, &addr, &size) == 0) {
        a->__stackaddr = addr;
        a->__stacksize = size;
    }
    errno = saved;
    return 0;
}

static pthread_attr_t g_dflt;
static int g_dflt_set;

WEAK int pthread_setattr_default_np(const pthread_attr_t *a)
{
    if (a->__stackaddr || a->__detach != PTHREAD_CREATE_JOINABLE)
        return EINVAL;         /* not attributes a default can carry */
    void *aff = 0;
    if (a->__affinity) {
        if (!(aff = malloc(a->__affsize)))
            return ENOMEM;
        memcpy(aff, a->__affinity, a->__affsize);
    }
    __lock(&g_tlock);
    free(g_dflt.__affinity);
    g_dflt = *a;
    g_dflt.__affinity = aff;
    g_dflt_set = 1;
    __unlock(&g_tlock);
    return 0;
}

WEAK int pthread_getattr_default_np(pthread_attr_t *a)
{
    __lock(&g_tlock);
    if (g_dflt_set) {
        *a = g_dflt;
        a->__affinity = 0;
        a->__affsize = 0;
        __unlock(&g_tlock);
        return g_dflt.__affinity ?
               pthread_attr_setaffinity_np(a, g_dflt.__affsize, g_dflt.__affinity) : 0;
    }
    __unlock(&g_tlock);
    return pthread_attr_init(a);
}

const pthread_attr_t *__thr_default_attr(void)
{
    return g_dflt_set ? &g_dflt : 0;
}

/* a signal with a value, to one thread */
WEAK int pthread_sigqueue(pthread_t id, int sig, const union sigval value)
{
    struct __thr *t = named(id);
    if (!t)
        return ESRCH;
#if defined(__linux__)
    siginfo_t si;
    memset(&si, 0, sizeof si);
    si.si_signo = sig;
    si.si_code = SI_QUEUE;
    si.si_pid = getpid();
    si.si_uid = getuid();
    si.si_value = value;
    int saved = errno, e = 0;
    if (syscall(SYS_rt_tgsigqueueinfo, (long)getpid(), (long)t->tid, (long)sig,
                (long)&si))
        e = errno;
    errno = saved;
    return e;
#else
    (void)value;
    return pthread_kill(id, sig);
#endif
}

WEAK int pthread_attr_getstackaddr(const pthread_attr_t *a, void **addr)
{
    *addr = a->__stackaddr;
    return 0;
}

WEAK int pthread_attr_setstackaddr(pthread_attr_t *a, void *addr)
{
    a->__stackaddr = addr;
    return 0;
}

/* LinuxThreads' leftover, before an exec: nothing to do with one process */
WEAK void pthread_kill_other_threads_np(void) {}
