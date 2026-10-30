/* <sys/time.h> -- microsecond time, interval timers, and select's
 * timeout (src/posix/time.c) */
#ifndef _SYS_TIME_H
#define _SYS_TIME_H

#include <sys/types.h>
#include <sys/select.h>

#ifdef __cplusplus
extern "C" {
#endif

struct timezone { int tz_minuteswest, tz_dsttime; };

#define ITIMER_REAL    0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF    2

struct itimerval { struct timeval it_interval, it_value; };

int gettimeofday(struct timeval *__restrict tv, void *__restrict tz);
int settimeofday(const struct timeval *tv, const struct timezone *tz);
int getitimer(int which, struct itimerval *cur);
int setitimer(int which, const struct itimerval *__restrict val,
              struct itimerval *__restrict old);
int utimes(const char *path, const struct timeval times[2]);
int futimes(int fd, const struct timeval times[2]);
int lutimes(const char *path, const struct timeval times[2]);

#define timerisset(t) ((t)->tv_sec || (t)->tv_usec)
#define timerclear(t) ((t)->tv_sec = (t)->tv_usec = 0)
#define timercmp(a, b, op) ((a)->tv_sec == (b)->tv_sec ? \
    (a)->tv_usec op (b)->tv_usec : (a)->tv_sec op (b)->tv_sec)
#define timeradd(a, b, r) do { \
    (r)->tv_sec = (a)->tv_sec + (b)->tv_sec; \
    (r)->tv_usec = (a)->tv_usec + (b)->tv_usec; \
    if ((r)->tv_usec >= 1000000) { (r)->tv_sec++; (r)->tv_usec -= 1000000; } \
} while (0)
#define timersub(a, b, r) do { \
    (r)->tv_sec = (a)->tv_sec - (b)->tv_sec; \
    (r)->tv_usec = (a)->tv_usec - (b)->tv_usec; \
    if ((r)->tv_usec < 0) { (r)->tv_sec--; (r)->tv_usec += 1000000; } \
} while (0)

#ifdef __cplusplus
}
#endif

#endif
