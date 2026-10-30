/* <mqueue.h> -- POSIX message queues (src/posix/ipc.c): Linux's, through
 * its system calls; ENOSYS where the kernel is not Linux. */
#ifndef _MQUEUE_H
#define _MQUEUE_H

#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int mqd_t;

struct mq_attr {
    long mq_flags, mq_maxmsg, mq_msgsize, mq_curmsgs;
    long __reserved[4];
};

struct timespec;

mqd_t   mq_open(const char *name, int oflag, ...);
int     mq_close(mqd_t mq);
int     mq_unlink(const char *name);
int     mq_send(mqd_t mq, const char *msg, size_t len, unsigned prio);
int     mq_timedsend(mqd_t mq, const char *msg, size_t len, unsigned prio,
                     const struct timespec *deadline);
ssize_t mq_receive(mqd_t mq, char *msg, size_t len, unsigned *prio);
ssize_t mq_timedreceive(mqd_t mq, char *__restrict msg, size_t len,
                        unsigned *__restrict prio,
                        const struct timespec *__restrict deadline);
int     mq_getattr(mqd_t mq, struct mq_attr *attr);
int     mq_setattr(mqd_t mq, const struct mq_attr *__restrict attr,
                   struct mq_attr *__restrict old);
int     mq_notify(mqd_t mq, const struct sigevent *ev);

#ifdef __cplusplus
}
#endif

#endif
