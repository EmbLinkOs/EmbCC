/* POSIX message queues and XSI's messages, semaphore sets and shared
 * memory: kernel objects, so each call is Linux's system call
 * (<sys/syscall.h>), and ENOSYS where the kernel is not Linux. ftok is
 * the one that needs no kernel object -- a key made from a file's
 * identity, as glibc makes it. */
#define _GNU_SOURCE 1
#include <errno.h>
#include <mqueue.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/syscall.h>

#include "posix.h"

#if defined(__linux__)
#define SC(...) syscall(__VA_ARGS__)
#else
#define SC(...) (errno = ENOSYS, -1L)
#endif

/* ---- XSI's keys ------------------------------------------------------------------ */

WEAK key_t ftok(const char *path, int id)
{
    struct stat st;
    if (stat(path, &st) < 0)
        return (key_t)-1;
    return (key_t)((uint32_t)(st.st_ino & 0xffff) |
                   (uint32_t)(st.st_dev & 0xff) << 16 |
                   (uint32_t)(id & 0xff) << 24);
}

/* ---- POSIX message queues --------------------------------------------------------- */

/* the kernel's names have no leading slash; POSIX's must have one */
static const char *mq_name(const char *name)
{
    if (name[0] != '/' || !name[1] || strchr(name + 1, '/')) {
        errno = name[0] != '/' ? EINVAL : EACCES;
        return 0;
    }
    return name + 1;
}

WEAK mqd_t mq_open(const char *name, int oflag, ...)
{
    mode_t mode = 0;
    struct mq_attr *attr = 0;
    if (oflag & O_CREAT) {
        va_list ap;
        va_start(ap, oflag);
        mode = (mode_t)va_arg(ap, unsigned);
        attr = va_arg(ap, struct mq_attr *);
        va_end(ap);
    }
    const char *n = mq_name(name);
    if (!n)
        return -1;
    (void)mode;
    (void)attr;
    return (mqd_t)SC(SYS_mq_open, (long)n, (long)(oflag | O_CLOEXEC),
                     (long)mode, (long)attr);
}

WEAK int mq_close(mqd_t mq) { return close(mq); }

WEAK int mq_unlink(const char *name)
{
    const char *n = mq_name(name);
    return n ? (int)SC(SYS_mq_unlink, (long)n) : -1;
}

WEAK int mq_timedsend(mqd_t mq, const char *msg, size_t len, unsigned prio,
                      const struct timespec *deadline)
{
    (void)mq; (void)msg; (void)len; (void)prio; (void)deadline;
    return (int)SC(SYS_mq_timedsend, (long)mq, (long)msg, (long)len,
                   (long)prio, (long)deadline);
}

WEAK int mq_send(mqd_t mq, const char *msg, size_t len, unsigned prio)
{
    return mq_timedsend(mq, msg, len, prio, 0);
}

WEAK ssize_t mq_timedreceive(mqd_t mq, char *restrict msg, size_t len,
                             unsigned *restrict prio,
                             const struct timespec *restrict deadline)
{
    (void)mq; (void)msg; (void)len; (void)prio; (void)deadline;
    return SC(SYS_mq_timedreceive, (long)mq, (long)msg, (long)len,
              (long)prio, (long)deadline);
}

WEAK ssize_t mq_receive(mqd_t mq, char *msg, size_t len, unsigned *prio)
{
    return mq_timedreceive(mq, msg, len, prio, 0);
}

WEAK int mq_getattr(mqd_t mq, struct mq_attr *attr)
{
    (void)mq; (void)attr;
    return (int)SC(SYS_mq_getsetattr, (long)mq, 0L, (long)attr);
}

WEAK int mq_setattr(mqd_t mq, const struct mq_attr *restrict attr,
                    struct mq_attr *restrict old)
{
    (void)mq; (void)attr; (void)old;
    return (int)SC(SYS_mq_getsetattr, (long)mq, (long)attr, (long)old);
}

/* a signal, or nothing; a thread to start is glibc's own machinery
 * (a netlink socket and a helper thread), not the kernel's */
WEAK int mq_notify(mqd_t mq, const struct sigevent *ev)
{
    if (ev && ev->sigev_notify == SIGEV_THREAD) {
        errno = ENOSYS;
        return -1;
    }
    (void)mq;
    return (int)SC(SYS_mq_notify, (long)mq, (long)ev);
}

/* ---- XSI messages -------------------------------------------------------------------- */

WEAK int msgget(key_t key, int flags)
{
    (void)key; (void)flags;
    return (int)SC(SYS_msgget, (long)key, (long)flags);
}

WEAK int msgctl(int id, int cmd, struct msqid_ds *buf)
{
    (void)id; (void)cmd; (void)buf;
    return (int)SC(SYS_msgctl, (long)id, (long)cmd, (long)buf);
}

WEAK int msgsnd(int id, const void *msg, size_t len, int flags)
{
    (void)id; (void)msg; (void)len; (void)flags;
    return (int)SC(SYS_msgsnd, (long)id, (long)msg, (long)len, (long)flags);
}

WEAK ssize_t msgrcv(int id, void *msg, size_t len, long type, int flags)
{
    (void)id; (void)msg; (void)len; (void)type; (void)flags;
    return SC(SYS_msgrcv, (long)id, (long)msg, (long)len, type, (long)flags);
}

/* ---- XSI semaphores --------------------------------------------------------------------- */

WEAK int semget(key_t key, int nsems, int flags)
{
    (void)key; (void)nsems; (void)flags;
    return (int)SC(SYS_semget, (long)key, (long)nsems, (long)flags);
}

/* the fourth argument is a union semun, passed by value; at 64 bits it
 * travels as one word, whichever member it holds */
WEAK int semctl(int id, int num, int cmd, ...)
{
    unsigned long arg = 0;
    switch (cmd) {
    case SETVAL: case GETALL: case SETALL: case IPC_STAT: case IPC_SET:
    case IPC_INFO: {
        va_list ap;
        va_start(ap, cmd);
        arg = va_arg(ap, unsigned long);
        va_end(ap);
        break;
    }
    }
    (void)id; (void)num; (void)arg;
    return (int)SC(SYS_semctl, (long)id, (long)num, (long)cmd, (long)arg);
}

WEAK int semtimedop(int id, struct sembuf *ops, size_t n,
                    const struct timespec *timeout)
{
    (void)id; (void)ops; (void)n; (void)timeout;
    return (int)SC(SYS_semtimedop, (long)id, (long)ops, (long)n, (long)timeout);
}

WEAK int semop(int id, struct sembuf *ops, size_t n)
{
    return semtimedop(id, ops, n, 0);
}

/* ---- XSI shared memory ------------------------------------------------------------------ */

WEAK int shmget(key_t key, size_t size, int flags)
{
    (void)key; (void)size; (void)flags;
    return (int)SC(SYS_shmget, (long)key, (long)size, (long)flags);
}

WEAK void *shmat(int id, const void *addr, int flags)
{
    (void)id; (void)addr; (void)flags;
    return (void *)SC(SYS_shmat, (long)id, (long)addr, (long)flags);
}

WEAK int shmdt(const void *addr)
{
    (void)addr;
    return (int)SC(SYS_shmdt, (long)addr);
}

WEAK int shmctl(int id, int cmd, struct shmid_ds *buf)
{
    (void)id; (void)cmd; (void)buf;
    return (int)SC(SYS_shmctl, (long)id, (long)cmd, (long)buf);
}
