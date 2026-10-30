/* What the system is: sysconf and confstr, uname and the host name,
 * limits and usage, the scheduling queries.
 *
 * sysconf's answers are of two kinds. Most are this library's own
 * limits -- a line it reads, a name it stores -- and are the same on
 * every target. A few only the running system knows (processors,
 * memory, the page size), and those are asked of the backend. */
#include <unistd.h>
#include <limits.h>
#include <sched.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/utsname.h>

#include "posix.h"

/* The page size is not like the processor count: a program needs one to
 * do arithmetic with, and where there is no MMU any power of two is
 * right -- so 4096 stands in where the backend has no answer. */
static long ask(int what, long otherwise)
{
    int saved = errno;
    long r = __os_sysinfo(what);
    errno = saved;
    return r > 0 ? r : otherwise;
}

WEAK long sysconf(int name)
{
    switch (name) {
    case _SC_ARG_MAX:          return ARG_MAX;
    case _SC_CHILD_MAX:        return 32;
    case _SC_CLK_TCK:          return 100;
    case _SC_NGROUPS_MAX:      return NGROUPS_MAX;
    case _SC_OPEN_MAX: {
        unsigned long long lim[2];
        int saved = errno;
        if (__os_getrlimit(7 /* RLIMIT_NOFILE */, lim) == 0 &&
            lim[0] < (unsigned long long)LONG_MAX)
            return (long)lim[0];
        errno = saved;
        return OPEN_MAX;
    }
    case _SC_STREAM_MAX:       return FOPEN_MAX;
    case _SC_TZNAME_MAX:       return 6;
    case _SC_JOB_CONTROL:      return 1;
    case _SC_SAVED_IDS:        return 1;
    case _SC_VERSION:          return _POSIX_VERSION;
    case _SC_2_VERSION:        return _POSIX2_VERSION;
    case _SC_XOPEN_VERSION:    return _XOPEN_VERSION;
    case _SC_PAGESIZE:         return ask(__OS_INFO_PAGESIZE, 4096);
    case _SC_RTSIG_MAX:        return 64 - 34 + 1;   /* SIGRTMAX - SIGRTMIN + 1 */
    case _SC_SIGQUEUE_MAX:     return 32;
    case _SC_BC_BASE_MAX:      return 99;
    case _SC_BC_DIM_MAX:       return 2048;
    case _SC_BC_SCALE_MAX:     return 99;
    case _SC_BC_STRING_MAX:    return 1000;
    case _SC_COLL_WEIGHTS_MAX: return 2;
    case _SC_EXPR_NEST_MAX:    return 32;
    case _SC_LINE_MAX:         return 2048;
    case _SC_RE_DUP_MAX:       return 255;
    case _SC_IOV_MAX:          return 1024;
    case _SC_GETGR_R_SIZE_MAX: return 1024;
    case _SC_GETPW_R_SIZE_MAX: return 1024;
    case _SC_LOGIN_NAME_MAX:   return 256;
    case _SC_TTY_NAME_MAX:     return 32;
    case _SC_HOST_NAME_MAX:    return 64;
    case _SC_SYMLOOP_MAX:      return 40;
    case _SC_ATEXIT_MAX:       return 32;
    case _SC_THREAD_STACK_MIN: return 16384;
    case _SC_FSYNC:
    case _SC_MAPPED_FILES:
    case _SC_MONOTONIC_CLOCK:
    case _SC_SPAWN:            return _POSIX_VERSION;
    /* A count the system cannot give is -1, not a plausible 1 that a
     * caller could not tell from a measured one (os.cpu_count() then
     * says None, which is the truth). */
    case _SC_NPROCESSORS_CONF: return __os_sysinfo(__OS_INFO_NPROCESSORS_CONF);
    case _SC_NPROCESSORS_ONLN: return __os_sysinfo(__OS_INFO_NPROCESSORS_ONLN);
    case _SC_PHYS_PAGES:
    case _SC_AVPHYS_PAGES: {
        long r = __os_sysinfo(name == _SC_PHYS_PAGES
                                  ? __OS_INFO_PHYS_PAGES
                                  : __OS_INFO_AVPHYS_PAGES);
        return r;                /* -1 with errno where it cannot say */
    }
    /* options this library does not offer: -1 with errno unchanged,
     * which is POSIX's "no such option" */
    case _SC_REALTIME_SIGNALS:
    case _SC_TIMERS:
    case _SC_SEMAPHORES:
    case _SC_THREADS:
    case _SC_THREAD_SAFE_FUNCTIONS:
    case _SC_SEM_NSEMS_MAX:
    case _SC_THREAD_KEYS_MAX:
    case _SC_THREAD_THREADS_MAX:
        return -1;
    }
    errno = EINVAL;
    return -1;
}

/* the value's length with its terminator; 0 with EINVAL for no such name */
WEAK size_t confstr(int name, char *buf, size_t n)
{
    const char *v;
    switch (name) {
    case _CS_PATH: v = "/bin:/usr/bin"; break;
    default: errno = EINVAL; return 0;
    }
    size_t len = strlen(v) + 1;
    if (buf && n) {
        size_t k = len < n ? len : n;
        memcpy(buf, v, k - 1);
        buf[k - 1] = 0;
    }
    return len;
}

WEAK int getpagesize(void) { return (int)sysconf(_SC_PAGESIZE); }
WEAK int getdtablesize(void) { return (int)sysconf(_SC_OPEN_MAX); }

WEAK int uname(struct utsname *u)
{
    struct __os_utsname k;
    if (__os_uname(&k) < 0)
        return -1;
    memset(u, 0, sizeof *u);
    memcpy(u->sysname, k.sysname, 65);
    memcpy(u->nodename, k.nodename, 65);
    memcpy(u->release, k.release, 65);
    memcpy(u->version, k.version, 65);
    memcpy(u->machine, k.machine, 65);
    return 0;
}

/* POSIX leaves a name that does not fit unterminated; this one is
 * terminated and reported, as glibc reports it, with ENAMETOOLONG */
WEAK int gethostname(char *name, size_t n)
{
    struct utsname u;
    if (uname(&u) < 0)
        return -1;
    size_t k = strlen(u.nodename);
    if (k + 1 > n) {
        if (n) {
            memcpy(name, u.nodename, n - 1);
            name[n - 1] = 0;
        }
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(name, u.nodename, k + 1);
    return 0;
}

WEAK int sethostname(const char *name, size_t n)
{
    return __os_sethostname(name, n);
}

WEAK int getrlimit(int resource, struct rlimit *lim)
{
    unsigned long long k[2];
    if (__os_getrlimit(resource, k) < 0)
        return -1;
    lim->rlim_cur = k[0];
    lim->rlim_max = k[1];
    return 0;
}

WEAK int setrlimit(int resource, const struct rlimit *lim)
{
    unsigned long long k[2] = { lim->rlim_cur, lim->rlim_max };
    return __os_setrlimit(resource, k);
}

WEAK int getrusage(int who, struct rusage *ru)
{
    struct __os_rusage k;
    if (__os_getrusage(who, &k) < 0)
        return -1;
    memset(ru, 0, sizeof *ru);
    ru->ru_utime.tv_sec = (time_t)(k.utime_us / 1000000);
    ru->ru_utime.tv_usec = (suseconds_t)(k.utime_us % 1000000);
    ru->ru_stime.tv_sec = (time_t)(k.stime_us / 1000000);
    ru->ru_stime.tv_usec = (suseconds_t)(k.stime_us % 1000000);
    ru->ru_maxrss = k.maxrss_kb;
    return 0;
}

/* ---- scheduling ---------------------------------------------------------- */

WEAK int sched_yield(void)
{
    __os_thread_yield();
    return 0;
}

/* the policies' own ranges, Linux's: they define the policy, so they
 * are not a question for the kernel */
WEAK int sched_get_priority_max(int policy)
{
    if (policy == SCHED_FIFO || policy == SCHED_RR)
        return 99;
    if (policy == SCHED_OTHER)
        return 0;
    errno = EINVAL;
    return -1;
}

WEAK int sched_get_priority_min(int policy)
{
    if (policy == SCHED_FIFO || policy == SCHED_RR)
        return 1;
    if (policy == SCHED_OTHER)
        return 0;
    errno = EINVAL;
    return -1;
}
