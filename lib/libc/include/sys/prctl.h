/* <sys/prctl.h> -- Linux's process controls (src/posix/linuxapi.c). The
 * options are the kernel's numbers; the common ones are named here. */
#ifndef _SYS_PRCTL_H
#define _SYS_PRCTL_H

#ifdef __cplusplus
extern "C" {
#endif

#define PR_SET_PDEATHSIG   1
#define PR_GET_PDEATHSIG   2
#define PR_GET_DUMPABLE    3
#define PR_SET_DUMPABLE    4
#define PR_GET_KEEPCAPS    7
#define PR_SET_KEEPCAPS    8
#define PR_GET_TIMERSLACK  30
#define PR_SET_TIMERSLACK  29
#define PR_SET_NAME        15
#define PR_GET_NAME        16
#define PR_GET_SECCOMP     21
#define PR_SET_SECCOMP     22
#define PR_CAPBSET_READ    23
#define PR_CAPBSET_DROP    24
#define PR_SET_CHILD_SUBREAPER 36
#define PR_GET_CHILD_SUBREAPER 37
#define PR_SET_NO_NEW_PRIVS 38
#define PR_GET_NO_NEW_PRIVS 39
#define PR_GET_TID_ADDRESS 40
#define PR_SET_THP_DISABLE 41
#define PR_GET_THP_DISABLE 42

int prctl(int option, ...);

#ifdef __cplusplus
}
#endif

#endif
