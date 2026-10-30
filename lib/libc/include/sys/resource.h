/* <sys/resource.h> -- limits and usage (src/posix/sysconf.c) */
#ifndef _SYS_RESOURCE_H
#define _SYS_RESOURCE_H

#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long long rlim_t;
#define RLIM_INFINITY  (~(rlim_t)0)
#define RLIM_SAVED_MAX RLIM_INFINITY
#define RLIM_SAVED_CUR RLIM_INFINITY

#define RLIMIT_CPU     0
#define RLIMIT_FSIZE   1
#define RLIMIT_DATA    2
#define RLIMIT_STACK   3
#define RLIMIT_CORE    4
#define RLIMIT_RSS     5
#define RLIMIT_NPROC   6
#define RLIMIT_NOFILE  7
#define RLIMIT_MEMLOCK 8
#define RLIMIT_AS      9
#define RLIM_NLIMITS   16

struct rlimit { rlim_t rlim_cur, rlim_max; };

#define RUSAGE_SELF     0
#define RUSAGE_CHILDREN (-1)
#define RUSAGE_THREAD   1

struct rusage {
    struct timeval ru_utime, ru_stime;
    long ru_maxrss, ru_ixrss, ru_idrss, ru_isrss, ru_minflt, ru_majflt;
    long ru_nswap, ru_inblock, ru_oublock, ru_msgsnd, ru_msgrcv;
    long ru_nsignals, ru_nvcsw, ru_nivcsw;
};

int getrlimit(int resource, struct rlimit *lim);
int setrlimit(int resource, const struct rlimit *lim);
int getrusage(int who, struct rusage *ru);

#ifdef __cplusplus
}
#endif

#endif
