/* <aio.h>. A request is done by a thread of its own -- pread, pwrite or
 * fsync -- which then notifies as the request's sigevent says (a queued
 * signal, or the function called on that thread) and wakes anyone in
 * aio_suspend. Where a thread cannot be started (a target without
 * threads, or none left) the request is done before aio_read returns,
 * which POSIX permits: completion may be immediate.
 *
 * A request already running cannot be called back: aio_cancel answers
 * AIO_NOTCANCELED for those and AIO_ALLDONE for finished ones. */
#define _GNU_SOURCE 1
#include <aio.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "posix.h"

enum { IDLE, RUNNING, DONE };
enum { OP_READ, OP_WRITE, OP_FSYNC, OP_DSYNC };

static pthread_mutex_t g_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_done = PTHREAD_COND_INITIALIZER;

static void perform(struct aiocb *cb)
{
    ssize_t r;
    switch (cb->__op) {
    case OP_READ:
        r = pread(cb->aio_fildes, (void *)cb->aio_buf, cb->aio_nbytes,
                  cb->aio_offset);
        break;
    case OP_WRITE:
        /* a descriptor open for appending writes at the end, as POSIX says */
        if (fcntl(cb->aio_fildes, F_GETFL) & O_APPEND)
            r = write(cb->aio_fildes, (const void *)cb->aio_buf, cb->aio_nbytes);
        else
            r = pwrite(cb->aio_fildes, (const void *)cb->aio_buf,
                       cb->aio_nbytes, cb->aio_offset);
        break;
    case OP_DSYNC:
        r = fdatasync(cb->aio_fildes);
        break;
    default:
        r = fsync(cb->aio_fildes);
        break;
    }
    int e = r < 0 ? errno : 0;
    pthread_mutex_lock(&g_m);
    cb->__result = r;
    cb->__error = e;
    cb->__state = DONE;
    pthread_cond_broadcast(&g_done);
    pthread_mutex_unlock(&g_m);

    struct sigevent ev = cb->aio_sigevent;
    if (ev.sigev_notify == SIGEV_SIGNAL)
        sigqueue(getpid(), ev.sigev_signo, ev.sigev_value);
    else if (ev.sigev_notify == SIGEV_THREAD && ev.sigev_notify_function)
        ev.sigev_notify_function(ev.sigev_value);
}

static void *worker(void *arg)
{
    perform(arg);
    return 0;
}

static int submit(struct aiocb *cb, int op)
{
    if (!cb || cb->aio_fildes < 0 || (op <= OP_WRITE && cb->aio_offset < 0) ||
        (op <= OP_WRITE && fcntl(cb->aio_fildes, F_GETFL) < 0)) {
        errno = cb && cb->aio_fildes < 0 ? EBADF : EINVAL;
        if (cb && fcntl(cb->aio_fildes, F_GETFL) < 0)
            errno = EBADF;
        return -1;
    }
    cb->__op = op;
    cb->__error = EINPROGRESS;
    cb->__result = -1;
    cb->__state = RUNNING;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    int e = pthread_create(&t, &a, worker, cb);
    pthread_attr_destroy(&a);
    if (e)
        perform(cb);           /* no thread: done now */
    return 0;
}

WEAK int aio_read(struct aiocb *cb) { return submit(cb, OP_READ); }
WEAK int aio_write(struct aiocb *cb) { return submit(cb, OP_WRITE); }

WEAK int aio_fsync(int op, struct aiocb *cb)
{
    if (op != O_SYNC && op != O_DSYNC) {
        errno = EINVAL;
        return -1;
    }
    return submit(cb, op == O_DSYNC ? OP_DSYNC : OP_FSYNC);
}

WEAK int aio_error(const struct aiocb *cb)
{
    pthread_mutex_lock(&g_m);
    int e = cb->__state == DONE ? cb->__error : EINPROGRESS;
    pthread_mutex_unlock(&g_m);
    return e;
}

WEAK ssize_t aio_return(struct aiocb *cb)
{
    pthread_mutex_lock(&g_m);
    int done = cb->__state == DONE;
    ssize_t r = cb->__result;
    int e = cb->__error;
    if (done)
        cb->__state = IDLE;    /* its result is taken: once only */
    pthread_mutex_unlock(&g_m);
    if (!done) {
        errno = EINVAL;
        return -1;
    }
    if (r < 0)
        errno = e;
    return r;
}

WEAK int aio_cancel(int fd, struct aiocb *cb)
{
    if (fcntl(fd, F_GETFL) < 0) {
        errno = EBADF;
        return -1;
    }
    if (cb && cb->aio_fildes != fd) {
        errno = EINVAL;
        return -1;
    }
    if (!cb)
        return AIO_ALLDONE;    /* requests are not tracked by descriptor */
    pthread_mutex_lock(&g_m);
    int done = cb->__state != RUNNING;
    pthread_mutex_unlock(&g_m);
    return done ? AIO_ALLDONE : AIO_NOTCANCELED;
}

WEAK int aio_suspend(const struct aiocb *const list[], int n,
                     const struct timespec *timeout)
{
    struct timespec deadline;
    if (timeout) {
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += timeout->tv_sec;
        deadline.tv_nsec += timeout->tv_nsec;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_nsec -= 1000000000L;
            deadline.tv_sec++;
        }
    }
    pthread_mutex_lock(&g_m);
    for (;;) {
        for (int i = 0; i < n; i++)
            if (list[i] && list[i]->__state != RUNNING) {
                pthread_mutex_unlock(&g_m);
                return 0;
            }
        int e = timeout ? pthread_cond_timedwait(&g_done, &g_m, &deadline)
                        : pthread_cond_wait(&g_done, &g_m);
        if (e == ETIMEDOUT) {
            pthread_mutex_unlock(&g_m);
            errno = EAGAIN;
            return -1;
        }
    }
}

struct batch {
    int n;
    struct sigevent ev;
    struct aiocb *list[];
};

static void *batch_waiter(void *arg)
{
    struct batch *b = arg;
    pthread_mutex_lock(&g_m);
    for (int i = 0; i < b->n; i++)
        while (b->list[i] && b->list[i]->aio_lio_opcode != LIO_NOP &&
               b->list[i]->__state == RUNNING)
            pthread_cond_wait(&g_done, &g_m);
    pthread_mutex_unlock(&g_m);
    if (b->ev.sigev_notify == SIGEV_SIGNAL)
        sigqueue(getpid(), b->ev.sigev_signo, b->ev.sigev_value);
    else if (b->ev.sigev_notify == SIGEV_THREAD && b->ev.sigev_notify_function)
        b->ev.sigev_notify_function(b->ev.sigev_value);
    free(b);
    return 0;
}

WEAK int lio_listio(int mode, struct aiocb *restrict const list[restrict],
                    int n, struct sigevent *restrict ev)
{
    if ((mode != LIO_WAIT && mode != LIO_NOWAIT) || n < 0) {
        errno = EINVAL;
        return -1;
    }
    int failed = 0;
    for (int i = 0; i < n; i++) {
        struct aiocb *cb = list[i];
        if (!cb || cb->aio_lio_opcode == LIO_NOP)
            continue;
        int r = cb->aio_lio_opcode == LIO_READ ? aio_read(cb)
                : cb->aio_lio_opcode == LIO_WRITE ? aio_write(cb) : (errno = EINVAL, -1);
        if (r) {
            cb->__state = DONE;
            cb->__error = errno;
            cb->__result = -1;
            failed = 1;
        }
    }
    if (mode == LIO_WAIT) {
        pthread_mutex_lock(&g_m);
        for (int i = 0; i < n; i++)
            while (list[i] && list[i]->aio_lio_opcode != LIO_NOP &&
                   list[i]->__state == RUNNING)
                pthread_cond_wait(&g_done, &g_m);
        pthread_mutex_unlock(&g_m);
    } else if (ev && ev->sigev_notify != SIGEV_NONE) {
        /* the whole list's completion, told by a thread that waits for it */
        struct batch *b = malloc(sizeof *b + (size_t)n * sizeof *list);
        if (b) {
            b->n = n;
            b->ev = *ev;
            memcpy(b->list, list, (size_t)n * sizeof *list);
            pthread_attr_t a;
            pthread_t t;
            pthread_attr_init(&a);
            pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
            if (pthread_create(&t, &a, batch_waiter, b))
                batch_waiter(b);       /* no thread: everything is done */
            pthread_attr_destroy(&a);
        }
    }
    if (failed) {
        errno = EIO;
        return -1;
    }
    return 0;
}
