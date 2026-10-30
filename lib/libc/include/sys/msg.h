/* <sys/msg.h> -- XSI message queues (src/posix/ipc.c). */
#ifndef _SYS_MSG_H
#define _SYS_MSG_H

#include <sys/ipc.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MSG_NOERROR 010000
#define MSG_EXCEPT  020000
#define MSG_COPY    040000

typedef unsigned long msgqnum_t;
typedef unsigned long msglen_t;

struct msqid_ds {
    struct ipc_perm msg_perm;
    time_t msg_stime, msg_rtime, msg_ctime;
    unsigned long __msg_cbytes;
    msgqnum_t msg_qnum;
    msglen_t msg_qbytes;
    pid_t msg_lspid, msg_lrpid;
    unsigned long __unused4, __unused5;
};

int     msgget(key_t key, int flags);
int     msgctl(int id, int cmd, struct msqid_ds *buf);
int     msgsnd(int id, const void *msg, size_t len, int flags);
ssize_t msgrcv(int id, void *msg, size_t len, long type, int flags);

#ifdef __cplusplus
}
#endif

#endif
