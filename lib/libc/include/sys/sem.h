/* <sys/sem.h> -- XSI semaphore sets (src/posix/ipc.c). semid_ds is laid
 * out as the kernel of each 64-bit architecture has it. */
#ifndef _SYS_SEM_H
#define _SYS_SEM_H

#include <sys/ipc.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SEM_UNDO 0x1000
#define GETPID  11
#define GETVAL  12
#define GETALL  13
#define GETNCNT 14
#define GETZCNT 15
#define SETVAL  16
#define SETALL  17

struct sembuf {
    unsigned short sem_num;
    short sem_op;
    short sem_flg;
};

struct semid_ds {
    struct ipc_perm sem_perm;
    time_t sem_otime;
#if defined(__x86_64__)
    unsigned long __unused1;
#endif
    time_t sem_ctime;
#if defined(__x86_64__)
    unsigned long __unused2;
#endif
    unsigned long sem_nsems;
    unsigned long __unused3, __unused4;
};

struct timespec;

int semget(key_t key, int nsems, int flags);
int semctl(int id, int num, int cmd, ...);
int semop(int id, struct sembuf *ops, size_t n);
int semtimedop(int id, struct sembuf *ops, size_t n,
               const struct timespec *timeout);

#ifdef __cplusplus
}
#endif

#endif
