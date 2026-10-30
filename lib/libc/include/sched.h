/* <sched.h> -- giving up the processor, and the scheduling policies'
 * names. The priority ranges are the policies' own definitions (Linux's),
 * not a question for the kernel. */
#ifndef _SCHED_H
#define _SCHED_H

#include <features.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCHED_OTHER 0
#define SCHED_FIFO  1
#define SCHED_RR    2

#define SCHED_BATCH 3
#define SCHED_IDLE  5
#define SCHED_RESET_ON_FORK 0x40000000

struct sched_param { int sched_priority; };
struct timespec;

int sched_yield(void);
int sched_get_priority_max(int policy);
int sched_get_priority_min(int policy);
int sched_getparam(pid_t pid, struct sched_param *param);
int sched_setparam(pid_t pid, const struct sched_param *param);
int sched_getscheduler(pid_t pid);
int sched_setscheduler(pid_t pid, int policy, const struct sched_param *param);
int sched_rr_get_interval(pid_t pid, struct timespec *t);

#if defined(__EMB_GNU)
/* GNU's: which processors a thread may run on, as a bit set */
#define CPU_SETSIZE 1024
typedef struct { unsigned long __bits[CPU_SETSIZE / (8 * sizeof(long))]; } cpu_set_t;
#define __CPU_W(cpu) ((cpu) / (8 * sizeof(long)))
#define __CPU_B(cpu) (1UL << ((cpu) % (8 * sizeof(long))))
#define CPU_ALLOC_SIZE(n) \
    ((((n) + 8 * sizeof(long) - 1) / (8 * sizeof(long))) * sizeof(long))
#define CPU_ZERO_S(size, set) ((void)__builtin_memset((set), 0, (size)))
#define CPU_SET_S(cpu, size, set) \
    ((size_t)(cpu) / 8 < (size) ? ((set)->__bits[__CPU_W(cpu)] |= __CPU_B(cpu)) : 0)
#define CPU_CLR_S(cpu, size, set) \
    ((size_t)(cpu) / 8 < (size) ? ((set)->__bits[__CPU_W(cpu)] &= ~__CPU_B(cpu)) : 0)
#define CPU_ISSET_S(cpu, size, set) \
    ((size_t)(cpu) / 8 < (size) && ((set)->__bits[__CPU_W(cpu)] & __CPU_B(cpu)) != 0)
#define CPU_COUNT_S(size, set) __sched_cpucount((size), (set))
#define CPU_EQUAL_S(size, a, b) (__builtin_memcmp((a), (b), (size)) == 0)
#define CPU_ZERO(set)        CPU_ZERO_S(sizeof(cpu_set_t), set)
#define CPU_SET(cpu, set)    CPU_SET_S(cpu, sizeof(cpu_set_t), set)
#define CPU_CLR(cpu, set)    CPU_CLR_S(cpu, sizeof(cpu_set_t), set)
#define CPU_ISSET(cpu, set)  CPU_ISSET_S(cpu, sizeof(cpu_set_t), set)
#define CPU_COUNT(set)       CPU_COUNT_S(sizeof(cpu_set_t), set)
#define CPU_EQUAL(a, b)      CPU_EQUAL_S(sizeof(cpu_set_t), a, b)
int  __sched_cpucount(size_t size, const cpu_set_t *set);
int  sched_setaffinity(pid_t pid, size_t size, const cpu_set_t *set);
int  sched_getaffinity(pid_t pid, size_t size, cpu_set_t *set);
int  sched_getcpu(void);
int  getcpu(unsigned *cpu, unsigned *node);

/* namespaces, Linux's */
#define CLONE_NEWTIME   0x00000080
#define CLONE_VM        0x00000100
#define CLONE_FS        0x00000200
#define CLONE_FILES     0x00000400
#define CLONE_SIGHAND   0x00000800
#define CLONE_PIDFD     0x00001000
#define CLONE_PTRACE    0x00002000
#define CLONE_VFORK     0x00004000
#define CLONE_PARENT    0x00008000
#define CLONE_THREAD    0x00010000
#define CLONE_NEWNS     0x00020000
#define CLONE_SYSVSEM   0x00040000
#define CLONE_SETTLS    0x00080000
#define CLONE_NEWCGROUP 0x02000000
#define CLONE_NEWUTS    0x04000000
#define CLONE_NEWIPC    0x08000000
#define CLONE_NEWUSER   0x10000000
#define CLONE_NEWPID    0x20000000
#define CLONE_NEWNET    0x40000000
#define CLONE_IO        0x80000000
int  unshare(int flags);
int  setns(int fd, int nstype);
#endif

#ifdef __cplusplus
}
#endif

#endif
