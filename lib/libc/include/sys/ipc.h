/* <sys/ipc.h> -- XSI's interprocess communication keys and permissions
 * (src/posix/ipc.c), laid out as Linux's 64-bit kernels have them. */
#ifndef _SYS_IPC_H
#define _SYS_IPC_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IPC_CREAT  01000
#define IPC_EXCL   02000
#define IPC_NOWAIT 04000
#define IPC_PRIVATE ((key_t)0)
#define IPC_RMID 0
#define IPC_SET  1
#define IPC_STAT 2
#define IPC_INFO 3

struct ipc_perm {
    key_t __key;
    uid_t uid;
    gid_t gid;
    uid_t cuid;
    gid_t cgid;
    unsigned int mode;
    unsigned short __seq;
    unsigned short __pad2;
    unsigned long __unused1, __unused2;
};

key_t ftok(const char *path, int id);

#ifdef __cplusplus
}
#endif

#endif
