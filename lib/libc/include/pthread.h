/* <pthread.h> -- POSIX threads, over the same seam C11's <threads.h> uses
 * (lib/libc/os/backend.h: thread_create/join/detach/self and the futex).
 *
 * The constants are glibc's, so a program's numbers mean what it meant.
 * The types are this library's own: every object here is reached only
 * through these functions, and a static program has nobody else's to
 * agree with. Every lock is the three-state futex word; a target whose
 * backend has no threads runs the same code with one thread, where a
 * lock never waits.
 */
#ifndef _PTHREAD_H
#define _PTHREAD_H

#include <features.h>
#include <sched.h>
#include <time.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long pthread_t;

typedef struct {
    int __detach, __inherit, __policy, __scope;
    struct sched_param __param;
    size_t __stacksize, __guardsize;
    void *__stackaddr;
    void *__affinity;          /* pthread_attr_setaffinity_np's set, a copy */
    size_t __affsize;
} pthread_attr_t;

typedef struct {
    int __state;               /* 0 free, 1 held, 2 held with waiters */
    int __type;
    unsigned long __owner;
    unsigned __count;
    int __flags;
} pthread_mutex_t;
typedef struct { int __type, __pshared, __protocol, __robust, __ceiling; } pthread_mutexattr_t;

typedef struct { int __seq; int __clock; } pthread_cond_t;
typedef struct { int __clock, __pshared; } pthread_condattr_t;

typedef struct {
    int __lock, __seq;
    unsigned __readers;
    int __writer;
    unsigned long __owner;
} pthread_rwlock_t;
typedef struct { int __pshared, __kind; } pthread_rwlockattr_t;

typedef struct {
    int __lock, __seq;
    unsigned __count, __waiting;
} pthread_barrier_t;
typedef struct { int __pshared; } pthread_barrierattr_t;

typedef volatile int pthread_spinlock_t;
typedef int pthread_once_t;
typedef unsigned pthread_key_t;

#define PTHREAD_CREATE_JOINABLE  0
#define PTHREAD_CREATE_DETACHED  1
#define PTHREAD_INHERIT_SCHED    0
#define PTHREAD_EXPLICIT_SCHED   1
#define PTHREAD_SCOPE_SYSTEM     0
#define PTHREAD_SCOPE_PROCESS    1
#define PTHREAD_PROCESS_PRIVATE  0
#define PTHREAD_PROCESS_SHARED   1

#define PTHREAD_MUTEX_NORMAL     0
#define PTHREAD_MUTEX_RECURSIVE  1
#define PTHREAD_MUTEX_ERRORCHECK 2
#define PTHREAD_MUTEX_DEFAULT    PTHREAD_MUTEX_NORMAL
#define PTHREAD_PRIO_NONE        0
#define PTHREAD_PRIO_INHERIT     1
#define PTHREAD_PRIO_PROTECT     2
#define PTHREAD_MUTEX_STALLED    0
#define PTHREAD_MUTEX_ROBUST     1
#if defined(__EMB_GNU)
#define PTHREAD_MUTEX_TIMED_NP      PTHREAD_MUTEX_NORMAL
#define PTHREAD_MUTEX_RECURSIVE_NP  PTHREAD_MUTEX_RECURSIVE
#define PTHREAD_MUTEX_ERRORCHECK_NP PTHREAD_MUTEX_ERRORCHECK
#define PTHREAD_MUTEX_ADAPTIVE_NP   PTHREAD_MUTEX_NORMAL
#define PTHREAD_RWLOCK_PREFER_READER_NP 0
#define PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP 2
#endif

#define PTHREAD_CANCEL_ENABLE       0
#define PTHREAD_CANCEL_DISABLE      1
#define PTHREAD_CANCEL_DEFERRED     0
#define PTHREAD_CANCEL_ASYNCHRONOUS 1
#define PTHREAD_CANCELED            ((void *)-1)

#define PTHREAD_ONCE_INIT               0
#define PTHREAD_BARRIER_SERIAL_THREAD   (-1)

#define PTHREAD_MUTEX_INITIALIZER   { 0, PTHREAD_MUTEX_NORMAL, 0, 0, 0 }
#if defined(__EMB_GNU)
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP \
    { 0, PTHREAD_MUTEX_RECURSIVE, 0, 0, 0 }
#define PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP \
    { 0, PTHREAD_MUTEX_ERRORCHECK, 0, 0, 0 }
#endif
#define PTHREAD_COND_INITIALIZER    { 0, CLOCK_REALTIME }
#define PTHREAD_RWLOCK_INITIALIZER  { 0, 0, 0, 0, 0 }

/* threads */
int  pthread_create(pthread_t *__restrict t, const pthread_attr_t *__restrict a,
                    void *(*fn)(void *), void *__restrict arg);
int  pthread_join(pthread_t t, void **result);
int  pthread_detach(pthread_t t);
#ifdef __cplusplus
[[noreturn]] void pthread_exit(void *result);
#else
_Noreturn void pthread_exit(void *result);
#endif
pthread_t pthread_self(void);
int  pthread_equal(pthread_t a, pthread_t b);
int  pthread_once(pthread_once_t *once, void (*fn)(void));
int  pthread_atfork(void (*prepare)(void), void (*parent)(void),
                    void (*child)(void));
int  pthread_getconcurrency(void);
int  pthread_setconcurrency(int level);
int  pthread_getschedparam(pthread_t t, int *__restrict policy,
                           struct sched_param *__restrict param);
int  pthread_setschedparam(pthread_t t, int policy,
                           const struct sched_param *param);
int  pthread_setschedprio(pthread_t t, int prio);
int  pthread_getcpuclockid(pthread_t t, clockid_t *clock);

/* cancellation: there are no cancellation points, so a cancel request is
 * refused (ENOTSUP) and the state calls only keep their state */
int  pthread_cancel(pthread_t t);
int  pthread_setcancelstate(int state, int *old);
int  pthread_setcanceltype(int type, int *old);
void pthread_testcancel(void);

/* attributes */
int  pthread_attr_init(pthread_attr_t *a);
int  pthread_attr_destroy(pthread_attr_t *a);
int  pthread_attr_getdetachstate(const pthread_attr_t *a, int *s);
int  pthread_attr_setdetachstate(pthread_attr_t *a, int s);
int  pthread_attr_getstacksize(const pthread_attr_t *__restrict a,
                               size_t *__restrict n);
int  pthread_attr_setstacksize(pthread_attr_t *a, size_t n);
int  pthread_attr_getguardsize(const pthread_attr_t *__restrict a,
                               size_t *__restrict n);
int  pthread_attr_setguardsize(pthread_attr_t *a, size_t n);
int  pthread_attr_getstack(const pthread_attr_t *__restrict a,
                           void **__restrict addr, size_t *__restrict n);
int  pthread_attr_setstack(pthread_attr_t *a, void *addr, size_t n);
int  pthread_attr_getinheritsched(const pthread_attr_t *__restrict a,
                                  int *__restrict s);
int  pthread_attr_setinheritsched(pthread_attr_t *a, int s);
int  pthread_attr_getschedpolicy(const pthread_attr_t *__restrict a,
                                 int *__restrict p);
int  pthread_attr_setschedpolicy(pthread_attr_t *a, int p);
int  pthread_attr_getschedparam(const pthread_attr_t *__restrict a,
                                struct sched_param *__restrict p);
int  pthread_attr_setschedparam(pthread_attr_t *__restrict a,
                                const struct sched_param *__restrict p);
int  pthread_attr_getscope(const pthread_attr_t *__restrict a,
                           int *__restrict s);
int  pthread_attr_setscope(pthread_attr_t *a, int s);

/* mutexes */
int  pthread_mutex_init(pthread_mutex_t *__restrict m,
                        const pthread_mutexattr_t *__restrict a);
int  pthread_mutex_destroy(pthread_mutex_t *m);
int  pthread_mutex_lock(pthread_mutex_t *m);
int  pthread_mutex_trylock(pthread_mutex_t *m);
int  pthread_mutex_timedlock(pthread_mutex_t *__restrict m,
                             const struct timespec *__restrict abstime);
int  pthread_mutex_clocklock(pthread_mutex_t *__restrict m, clockid_t clock,
                             const struct timespec *__restrict abstime);
int  pthread_mutex_unlock(pthread_mutex_t *m);
int  pthread_mutex_consistent(pthread_mutex_t *m);
int  pthread_mutex_getprioceiling(const pthread_mutex_t *__restrict m,
                                  int *__restrict ceiling);
int  pthread_mutex_setprioceiling(pthread_mutex_t *__restrict m, int ceiling,
                                  int *__restrict old);
int  pthread_mutexattr_init(pthread_mutexattr_t *a);
int  pthread_mutexattr_destroy(pthread_mutexattr_t *a);
int  pthread_mutexattr_gettype(const pthread_mutexattr_t *__restrict a,
                               int *__restrict t);
int  pthread_mutexattr_settype(pthread_mutexattr_t *a, int t);
int  pthread_mutexattr_getpshared(const pthread_mutexattr_t *__restrict a,
                                  int *__restrict s);
int  pthread_mutexattr_setpshared(pthread_mutexattr_t *a, int s);
int  pthread_mutexattr_getprotocol(const pthread_mutexattr_t *__restrict a,
                                   int *__restrict p);
int  pthread_mutexattr_setprotocol(pthread_mutexattr_t *a, int p);
int  pthread_mutexattr_getrobust(const pthread_mutexattr_t *__restrict a,
                                 int *__restrict r);
int  pthread_mutexattr_setrobust(pthread_mutexattr_t *a, int r);

/* condition variables */
int  pthread_cond_init(pthread_cond_t *__restrict c,
                       const pthread_condattr_t *__restrict a);
int  pthread_cond_destroy(pthread_cond_t *c);
int  pthread_cond_signal(pthread_cond_t *c);
int  pthread_cond_broadcast(pthread_cond_t *c);
int  pthread_cond_wait(pthread_cond_t *__restrict c,
                       pthread_mutex_t *__restrict m);
int  pthread_cond_timedwait(pthread_cond_t *__restrict c,
                            pthread_mutex_t *__restrict m,
                            const struct timespec *__restrict abstime);
int  pthread_cond_clockwait(pthread_cond_t *__restrict c,
                            pthread_mutex_t *__restrict m, clockid_t clock,
                            const struct timespec *__restrict abstime);
int  pthread_condattr_init(pthread_condattr_t *a);
int  pthread_condattr_destroy(pthread_condattr_t *a);
int  pthread_condattr_getclock(const pthread_condattr_t *__restrict a,
                               clockid_t *__restrict c);
int  pthread_condattr_setclock(pthread_condattr_t *a, clockid_t c);
int  pthread_condattr_getpshared(const pthread_condattr_t *__restrict a,
                                 int *__restrict s);
int  pthread_condattr_setpshared(pthread_condattr_t *a, int s);

/* read-write locks */
int  pthread_rwlock_init(pthread_rwlock_t *__restrict l,
                         const pthread_rwlockattr_t *__restrict a);
int  pthread_rwlock_destroy(pthread_rwlock_t *l);
int  pthread_rwlock_rdlock(pthread_rwlock_t *l);
int  pthread_rwlock_tryrdlock(pthread_rwlock_t *l);
int  pthread_rwlock_timedrdlock(pthread_rwlock_t *__restrict l,
                                const struct timespec *__restrict abstime);
int  pthread_rwlock_clockrdlock(pthread_rwlock_t *__restrict l, clockid_t clock,
                                const struct timespec *__restrict abstime);
int  pthread_rwlock_wrlock(pthread_rwlock_t *l);
int  pthread_rwlock_trywrlock(pthread_rwlock_t *l);
int  pthread_rwlock_timedwrlock(pthread_rwlock_t *__restrict l,
                                const struct timespec *__restrict abstime);
int  pthread_rwlock_clockwrlock(pthread_rwlock_t *__restrict l, clockid_t clock,
                                const struct timespec *__restrict abstime);
int  pthread_rwlock_unlock(pthread_rwlock_t *l);
int  pthread_rwlockattr_init(pthread_rwlockattr_t *a);
int  pthread_rwlockattr_destroy(pthread_rwlockattr_t *a);
int  pthread_rwlockattr_getpshared(const pthread_rwlockattr_t *__restrict a,
                                   int *__restrict s);
int  pthread_rwlockattr_setpshared(pthread_rwlockattr_t *a, int s);

/* barriers and spin locks */
int  pthread_barrier_init(pthread_barrier_t *__restrict b,
                          const pthread_barrierattr_t *__restrict a,
                          unsigned count);
int  pthread_barrier_destroy(pthread_barrier_t *b);
int  pthread_barrier_wait(pthread_barrier_t *b);
int  pthread_barrierattr_init(pthread_barrierattr_t *a);
int  pthread_barrierattr_destroy(pthread_barrierattr_t *a);
int  pthread_barrierattr_getpshared(const pthread_barrierattr_t *__restrict a,
                                    int *__restrict s);
int  pthread_barrierattr_setpshared(pthread_barrierattr_t *a, int s);
int  pthread_spin_init(pthread_spinlock_t *l, int pshared);
int  pthread_spin_destroy(pthread_spinlock_t *l);
int  pthread_spin_lock(pthread_spinlock_t *l);
int  pthread_spin_trylock(pthread_spinlock_t *l);
int  pthread_spin_unlock(pthread_spinlock_t *l);

/* thread-specific data */
int  pthread_key_create(pthread_key_t *key, void (*dtor)(void *));
int  pthread_key_delete(pthread_key_t key);
void *pthread_getspecific(pthread_key_t key);
int  pthread_setspecific(pthread_key_t key, const void *value);

#if defined(__EMB_GNU)
int  pthread_setname_np(pthread_t t, const char *name);
int  pthread_getname_np(pthread_t t, char *name, size_t n);
int  pthread_tryjoin_np(pthread_t t, void **result);
int  pthread_timedjoin_np(pthread_t t, void **result,
                          const struct timespec *abstime);
int  pthread_yield(void);
int  pthread_clockjoin_np(pthread_t t, void **result, clockid_t clock,
                          const struct timespec *abstime);
int  pthread_setaffinity_np(pthread_t t, size_t n, const cpu_set_t *set);
int  pthread_getaffinity_np(pthread_t t, size_t n, cpu_set_t *set);
int  pthread_attr_setaffinity_np(pthread_attr_t *a, size_t n, const cpu_set_t *set);
int  pthread_attr_getaffinity_np(const pthread_attr_t *a, size_t n, cpu_set_t *set);
int  pthread_getattr_np(pthread_t t, pthread_attr_t *a);
int  pthread_setattr_default_np(const pthread_attr_t *a);
int  pthread_getattr_default_np(pthread_attr_t *a);
union sigval;
int  pthread_sigqueue(pthread_t t, int sig, const union sigval value);
int  pthread_mutexattr_getrobust_np(const pthread_mutexattr_t *a, int *r);
int  pthread_mutexattr_setrobust_np(pthread_mutexattr_t *a, int r);
int  pthread_mutex_consistent_np(pthread_mutex_t *m);
int  pthread_mutexattr_getkind_np(const pthread_mutexattr_t *a, int *k);
int  pthread_mutexattr_setkind_np(pthread_mutexattr_t *a, int k);
#define PTHREAD_RWLOCK_PREFER_READER_NP 0
#define PTHREAD_RWLOCK_PREFER_WRITER_NP 1
#define PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP 2
int  pthread_rwlockattr_getkind_np(const pthread_rwlockattr_t *a, int *k);
int  pthread_rwlockattr_setkind_np(pthread_rwlockattr_t *a, int k);
int  pthread_attr_getstackaddr(const pthread_attr_t *a, void **addr);
int  pthread_attr_setstackaddr(pthread_attr_t *a, void *addr);
void pthread_kill_other_threads_np(void);
#endif
int  pthread_mutexattr_getprioceiling(const pthread_mutexattr_t *__restrict a,
                                      int *__restrict ceiling);
int  pthread_mutexattr_setprioceiling(pthread_mutexattr_t *a, int ceiling);

/* <signal.h>'s, declared here too, as POSIX lists them in both */
int  pthread_kill(pthread_t t, int sig);

#ifdef __cplusplus
}
#endif

#endif
