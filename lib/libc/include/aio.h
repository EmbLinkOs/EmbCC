/* <aio.h> -- POSIX asynchronous I/O (src/posix/aio.c): each request on a
 * thread of its own, as glibc runs them in user space too; synchronously
 * where no thread can be started. */
#ifndef _AIO_H
#define _AIO_H

#include <signal.h>
#include <time.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AIO_CANCELED    0
#define AIO_NOTCANCELED 1
#define AIO_ALLDONE     2
#define LIO_READ  0
#define LIO_WRITE 1
#define LIO_NOP   2
#define LIO_WAIT   0
#define LIO_NOWAIT 1

struct aiocb {
    int aio_fildes;
    int aio_lio_opcode;
    int aio_reqprio;
    volatile void *aio_buf;
    size_t aio_nbytes;
    struct sigevent aio_sigevent;
    off_t aio_offset;
    /* the library's: where the request has got to */
    int __state;
    int __error;
    ssize_t __result;
    int __op;
};

int     aio_read(struct aiocb *cb);
int     aio_write(struct aiocb *cb);
int     aio_fsync(int op, struct aiocb *cb);
int     aio_error(const struct aiocb *cb);
ssize_t aio_return(struct aiocb *cb);
int     aio_cancel(int fd, struct aiocb *cb);
int     aio_suspend(const struct aiocb *const list[], int n,
                    const struct timespec *timeout);
int     lio_listio(int mode, struct aiocb *__restrict const list[__restrict],
                   int n, struct sigevent *__restrict ev);

#ifdef __cplusplus
}
#endif

#endif
