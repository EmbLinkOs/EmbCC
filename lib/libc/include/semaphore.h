/* <semaphore.h> -- POSIX's counting semaphores, on the futex the locks
 * use. Unnamed ones only: a named semaphore (sem_open) and a process-
 * shared one (sem_init's pshared) need memory another process maps and
 * a futex it can wake, which this library does not have -- sem_open
 * fails with ENOSYS and sem_init with pshared != 0 with ENOTSUP. */
#ifndef _SEMAPHORE_H
#define _SEMAPHORE_H

#include <features.h>
#include <time.h>
#include <fcntl.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { int __value, __waiters; } sem_t;

#define SEM_FAILED ((sem_t *)0)

int  sem_init(sem_t *s, int pshared, unsigned value);
int  sem_destroy(sem_t *s);
int  sem_wait(sem_t *s);
int  sem_trywait(sem_t *s);
int  sem_timedwait(sem_t *__restrict s, const struct timespec *__restrict abstime);
int  sem_clockwait(sem_t *__restrict s, clockid_t clock,
                   const struct timespec *__restrict abstime);
int  sem_post(sem_t *s);
int  sem_getvalue(sem_t *__restrict s, int *__restrict value);
sem_t *sem_open(const char *name, int oflag, ...);
int  sem_close(sem_t *s);
int  sem_unlink(const char *name);

#ifdef __cplusplus
}
#endif

#endif
