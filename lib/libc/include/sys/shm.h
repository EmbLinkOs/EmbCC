/* <sys/shm.h> -- XSI shared memory (src/posix/ipc.c). */
#ifndef _SYS_SHM_H
#define _SYS_SHM_H

#include <sys/ipc.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHM_RDONLY 010000
#define SHM_RND    020000
#define SHM_REMAP  040000
#define SHM_EXEC   0100000
#define SHM_LOCK   11
#define SHM_UNLOCK 12
#define SHMLBA     4096

typedef unsigned long shmatt_t;

struct shmid_ds {
    struct ipc_perm shm_perm;
    size_t shm_segsz;
    time_t shm_atime, shm_dtime, shm_ctime;
    pid_t shm_cpid, shm_lpid;
    shmatt_t shm_nattch;
    unsigned long __unused4, __unused5;
};

void *shmat(int id, const void *addr, int flags);
int   shmdt(const void *addr);
int   shmget(key_t key, size_t size, int flags);
int   shmctl(int id, int cmd, struct shmid_ds *buf);

#ifdef __cplusplus
}
#endif

#endif
