/* POSIX's per-process timers: timer_create, timer_settime and the rest.
 *
 * Two kinds behind one timer_t:
 *
 *   the kernel's   on Linux, for SIGEV_SIGNAL, SIGEV_NONE and
 *                  SIGEV_THREAD_ID: the system calls, so a signal comes
 *                  with si_code SI_TIMER and overruns are counted where
 *                  they happen
 *   a thread's     SIGEV_THREAD everywhere, and every kind where the
 *                  kernel has no timers: one thread per armed timer,
 *                  waiting on a condition variable (on the timer's own
 *                  clock) until the expiration or a change of setting
 *
 * SIGEV_NONE needs no thread: the setting is kept and timer_gettime
 * works out where it has got to. A target without threads has that
 * kind only; the others fail with EAGAIN there, as timer_create says a
 * lack of resources is. The CPU-time clocks are the kernel's alone.
 */
#define _GNU_SOURCE 1
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>

#define WEAK __attribute__((weak))

struct timer {
    int kernel;                /* the kernel's timer, and its id */
    int kid;
    clockid_t clock;
    struct sigevent ev;
    pthread_t th;
    int has_thread;
    pthread_mutex_t m;
    pthread_cond_t c;
    struct timespec next;      /* the next expiration, on clock */
    struct timespec interval;
    int armed;
    int overrun;               /* of the last expiration delivered */
    int dying;
    int orphan;                /* deleted by its own notify function */
};

#define NS 1000000000L

static int ts_le(const struct timespec *a, const struct timespec *b)
{
    return a->tv_sec < b->tv_sec ||
           (a->tv_sec == b->tv_sec && a->tv_nsec <= b->tv_nsec);
}

static struct timespec ts_add(struct timespec a, struct timespec b)
{
    a.tv_sec += b.tv_sec;
    a.tv_nsec += b.tv_nsec;
    if (a.tv_nsec >= NS) {
        a.tv_nsec -= NS;
        a.tv_sec++;
    }
    return a;
}

static struct timespec ts_sub(struct timespec a, struct timespec b)
{
    a.tv_sec -= b.tv_sec;
    a.tv_nsec -= b.tv_nsec;
    if (a.tv_nsec < 0) {
        a.tv_nsec += NS;
        a.tv_sec--;
    }
    return a;
}

static int ts_zero(const struct timespec *t) { return !t->tv_sec && !t->tv_nsec; }

static int ts_bad(const struct timespec *t)
{
    return t->tv_nsec < 0 || t->tv_nsec >= NS || t->tv_sec < 0;
}

/* Past `now`: move next on by whole intervals until it is ahead, and
 * say how many expirations that skipped. Caller holds the lock. */
static int catch_up(struct timer *t, const struct timespec *now)
{
    if (ts_zero(&t->interval)) {
        t->armed = 0;
        return 0;
    }
    int skipped = 0;
    t->next = ts_add(t->next, t->interval);
    while (ts_le(&t->next, now)) {
        t->next = ts_add(t->next, t->interval);
        if (skipped < __INT_MAX__)
            skipped++;
    }
    return skipped;
}

static void notify(struct timer *t)
{
    if (t->ev.sigev_notify == SIGEV_THREAD)
        t->ev.sigev_notify_function(t->ev.sigev_value);
    else if (t->ev.sigev_notify == SIGEV_SIGNAL)
        sigqueue(getpid(), t->ev.sigev_signo, t->ev.sigev_value);
}

static void *run(void *arg)
{
    struct timer *t = arg;
    pthread_mutex_lock(&t->m);
    while (!t->dying) {
        if (!t->armed) {
            pthread_cond_wait(&t->c, &t->m);
            continue;
        }
        struct timespec now;
        clock_gettime(t->clock, &now);
        if (!ts_le(&t->next, &now)) {
            pthread_cond_timedwait(&t->c, &t->m, &t->next);
            continue;          /* a deadline, a change or a deletion */
        }
        t->overrun = catch_up(t, &now);
        pthread_mutex_unlock(&t->m);
        notify(t);
        pthread_mutex_lock(&t->m);
    }
    int orphan = t->orphan;
    pthread_mutex_unlock(&t->m);
    if (orphan) {
        pthread_cond_destroy(&t->c);
        pthread_mutex_destroy(&t->m);
        free(t);
    }
    return 0;
}

WEAK int timer_create(clockid_t clock, struct sigevent *restrict ev,
                      timer_t *restrict out)
{
    struct sigevent def;
    if (!ev) {
        memset(&def, 0, sizeof def);
        def.sigev_notify = SIGEV_SIGNAL;
        def.sigev_signo = SIGALRM;
        ev = &def;
    }
    if (ev->sigev_notify != SIGEV_SIGNAL && ev->sigev_notify != SIGEV_NONE &&
        ev->sigev_notify != SIGEV_THREAD && ev->sigev_notify != SIGEV_THREAD_ID) {
        errno = EINVAL;
        return -1;
    }
    struct timer *t = calloc(1, sizeof *t);
    if (!t) {
        errno = EAGAIN;
        return -1;
    }
    t->clock = clock;
    t->ev = *ev;
    if (ev == &def)
        t->ev.sigev_value.sival_ptr = t;   /* POSIX: the timer's id */

#if defined(__linux__)
    if (ev->sigev_notify != SIGEV_THREAD) {
        int kid;
        if (syscall(SYS_timer_create, (long)clock, (long)&t->ev, (long)&kid)) {
            free(t);
            return -1;
        }
        t->kernel = 1;
        t->kid = kid;
        *out = t;
        return 0;
    }
#endif
    if (ev->sigev_notify == SIGEV_THREAD_ID) {
        free(t);
        errno = EINVAL;        /* a thread id means Linux's threads */
        return -1;
    }
    if (clock != CLOCK_REALTIME && clock != CLOCK_MONOTONIC &&
        clock != CLOCK_BOOTTIME) {
        struct timespec now;
        free(t);
        errno = clock_gettime(clock, &now) ? EINVAL : ENOTSUP;
        return -1;
    }
    if (clock == CLOCK_BOOTTIME)
        t->clock = CLOCK_MONOTONIC;
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, t->clock);
    pthread_mutex_init(&t->m, 0);
    pthread_cond_init(&t->c, &ca);
    pthread_condattr_destroy(&ca);
    if (ev->sigev_notify != SIGEV_NONE) {
        const pthread_attr_t *a = ev->sigev_notify == SIGEV_THREAD
                                      ? ev->sigev_notify_attributes : 0;
        int e = pthread_create(&t->th, a, run, t);
        if (e) {
            free(t);
            errno = EAGAIN;
            return -1;
        }
        t->has_thread = 1;
    }
    *out = t;
    return 0;
}

WEAK int timer_delete(timer_t timer)
{
    struct timer *t = timer;
    if (!t) {
        errno = EINVAL;
        return -1;
    }
#if defined(__linux__)
    if (t->kernel) {
        if (syscall(SYS_timer_delete, (long)t->kid))
            return -1;
        free(t);
        return 0;
    }
#endif
    if (t->has_thread) {
        int self = pthread_equal(pthread_self(), t->th);
        pthread_mutex_lock(&t->m);
        t->dying = 1;
        t->orphan = self;
        pthread_cond_signal(&t->c);
        pthread_mutex_unlock(&t->m);
        /* a notify function deleting its own timer: its thread frees the
         * record when the function has returned */
        if (self) {
            pthread_detach(t->th);
            return 0;
        }
        pthread_join(t->th, 0);
    }
    pthread_cond_destroy(&t->c);
    pthread_mutex_destroy(&t->m);
    free(t);
    return 0;
}

/* where a thread's timer has got to, its lock held */
static void current(struct timer *t, struct itimerspec *v)
{
    memset(v, 0, sizeof *v);
    if (!t->armed)
        return;
    struct timespec now;
    clock_gettime(t->clock, &now);
    if (!t->has_thread && ts_le(&t->next, &now)) {
        t->overrun = catch_up(t, &now);    /* SIGEV_NONE moves on its own */
        if (!t->armed)
            return;
    }
    v->it_interval = t->interval;
    if (!ts_le(&t->next, &now))
        v->it_value = ts_sub(t->next, now);
    else
        v->it_value.tv_nsec = 1;           /* due: about to fire */
}

WEAK int timer_gettime(timer_t timer, struct itimerspec *v)
{
    struct timer *t = timer;
#if defined(__linux__)
    if (t->kernel)
        return (int)syscall(SYS_timer_gettime, (long)t->kid, (long)v);
#endif
    pthread_mutex_lock(&t->m);
    current(t, v);
    pthread_mutex_unlock(&t->m);
    return 0;
}

WEAK int timer_settime(timer_t timer, int flags,
                       const struct itimerspec *restrict v,
                       struct itimerspec *restrict old)
{
    struct timer *t = timer;
#if defined(__linux__)
    if (t->kernel)
        return (int)syscall(SYS_timer_settime, (long)t->kid, (long)flags,
                            (long)v, (long)old);
#endif
    if (ts_bad(&v->it_value) || ts_bad(&v->it_interval)) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&t->m);
    if (old)
        current(t, old);
    t->interval = v->it_interval;
    t->overrun = 0;
    if (ts_zero(&v->it_value)) {
        t->armed = 0;
    } else {
        if (flags & TIMER_ABSTIME) {
            t->next = v->it_value;
        } else {
            struct timespec now;
            clock_gettime(t->clock, &now);
            t->next = ts_add(now, v->it_value);
        }
        t->armed = 1;
    }
    pthread_cond_signal(&t->c);
    pthread_mutex_unlock(&t->m);
    return 0;
}

WEAK int timer_getoverrun(timer_t timer)
{
    struct timer *t = timer;
#if defined(__linux__)
    if (t->kernel)
        return (int)syscall(SYS_timer_getoverrun, (long)t->kid);
#endif
    pthread_mutex_lock(&t->m);
    int n = t->overrun;
    pthread_mutex_unlock(&t->m);
    return n;
}
