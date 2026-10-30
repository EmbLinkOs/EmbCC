/* getaddrinfo_a: lookups that run while the caller goes on, as glibc's
 * libanl has them. Each request is a getaddrinfo on a thread of its own;
 * gai_error says EAI_INPROGRESS until it is done, gai_suspend waits for
 * one of a list, and the list's sigevent is told when all of it is. A
 * request whose thread has not started yet can be cancelled. Where a
 * thread cannot be started the lookup is done before getaddrinfo_a
 * returns, as aio.c does its requests. */
#define _GNU_SOURCE 1
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "posix.h"

enum { QUEUED = 1, RUNNING, DONE, CANCELED };

static pthread_mutex_t g_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_done = PTHREAD_COND_INITIALIZER;

static void *lookup(void *arg)
{
    struct gaicb *cb = arg;
    pthread_mutex_lock(&g_m);
    if (cb->__state == CANCELED) {
        pthread_mutex_unlock(&g_m);
        return 0;
    }
    cb->__state = RUNNING;
    pthread_mutex_unlock(&g_m);
    struct addrinfo *res = 0;
    int r = getaddrinfo(cb->ar_name, cb->ar_service, cb->ar_request, &res);
    pthread_mutex_lock(&g_m);
    cb->ar_result = r ? 0 : res;
    cb->__return = r;
    cb->__state = DONE;
    pthread_cond_broadcast(&g_done);
    pthread_mutex_unlock(&g_m);
    return 0;
}

static int pending(const struct gaicb *cb)
{
    return cb && (cb->__state == QUEUED || cb->__state == RUNNING);
}

static void notify(const struct sigevent *ev)
{
    if (ev->sigev_notify == SIGEV_SIGNAL)
        sigqueue(getpid(), ev->sigev_signo, ev->sigev_value);
    else if (ev->sigev_notify == SIGEV_THREAD && ev->sigev_notify_function)
        ev->sigev_notify_function(ev->sigev_value);
}

struct batch {
    int n;
    struct sigevent ev;
    struct gaicb *list[];
};

static void *batch_waiter(void *arg)
{
    struct batch *b = arg;
    pthread_mutex_lock(&g_m);
    for (int i = 0; i < b->n; i++)
        while (pending(b->list[i]))
            pthread_cond_wait(&g_done, &g_m);
    pthread_mutex_unlock(&g_m);
    notify(&b->ev);
    free(b);
    return 0;
}

static int detached(void *(*fn)(void *), void *arg)
{
    pthread_attr_t a;
    pthread_t t;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    int e = pthread_create(&t, &a, fn, arg);
    pthread_attr_destroy(&a);
    return e;
}

WEAK int getaddrinfo_a(int mode, struct gaicb *list[], int ent, struct sigevent *sig)
{
    if ((mode != GAI_WAIT && mode != GAI_NOWAIT) || ent < 0) {
        errno = EINVAL;
        return EAI_SYSTEM;
    }
    for (int i = 0; i < ent; i++) {
        struct gaicb *cb = list[i];
        if (!cb)
            continue;
        pthread_mutex_lock(&g_m);
        cb->__return = EAI_INPROGRESS;
        cb->__state = QUEUED;
        cb->ar_result = 0;
        pthread_mutex_unlock(&g_m);
        if (detached(lookup, cb))
            lookup(cb);                /* no thread: done now */
    }
    if (mode == GAI_WAIT) {
        pthread_mutex_lock(&g_m);
        for (int i = 0; i < ent; i++)
            while (pending(list[i]))
                pthread_cond_wait(&g_done, &g_m);
        pthread_mutex_unlock(&g_m);
        return 0;
    }
    if (sig && sig->sigev_notify != SIGEV_NONE) {
        struct batch *b = malloc(sizeof *b + (size_t)ent * sizeof *list);
        if (!b)
            return EAI_AGAIN;
        b->n = ent;
        b->ev = *sig;
        memcpy(b->list, list, (size_t)ent * sizeof *list);
        if (detached(batch_waiter, b))
            batch_waiter(b);           /* no thread: everything is done */
    }
    return 0;
}

WEAK int gai_error(struct gaicb *req)
{
    pthread_mutex_lock(&g_m);
    int r = req->__return;
    pthread_mutex_unlock(&g_m);
    return r;
}

WEAK int gai_cancel(struct gaicb *req)
{
    int r;
    pthread_mutex_lock(&g_m);
    if (req->__state == QUEUED) {
        req->__state = CANCELED;
        req->__return = EAI_CANCELED;
        pthread_cond_broadcast(&g_done);
        r = EAI_CANCELED;
    } else {
        r = req->__state == RUNNING ? EAI_NOTCANCELED : EAI_ALLDONE;
    }
    pthread_mutex_unlock(&g_m);
    return r;
}

/* Waiting for the requests in the list still in progress: 0 once one of
 * them is done, EAI_AGAIN when the timeout passes first, EAI_ALLDONE when
 * none was in progress -- finished ones included, as glibc has it. */
WEAK int gai_suspend(const struct gaicb *const list[], int ent, const struct timespec *timeout)
{
    struct timespec until;
    if (timeout) {
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += timeout->tv_sec;
        until.tv_nsec += timeout->tv_nsec;
        if (until.tv_nsec >= 1000000000L) {
            until.tv_sec++;
            until.tv_nsec -= 1000000000L;
        }
    }
    int r = EAI_ALLDONE;
    pthread_mutex_lock(&g_m);
    int waiting = 0;
    for (int i = 0; i < ent; i++)
        waiting += pending(list[i]);
    while (waiting) {
        int still = 0;
        for (int i = 0; i < ent; i++)
            still += pending(list[i]);
        if (still < waiting) {
            r = 0;
            break;
        }
        if (timeout) {
            if (pthread_cond_timedwait(&g_done, &g_m, &until) == ETIMEDOUT) {
                r = EAI_AGAIN;
                break;
            }
        } else {
            pthread_cond_wait(&g_done, &g_m);
        }
    }
    pthread_mutex_unlock(&g_m);
    return r;
}
