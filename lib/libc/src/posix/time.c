/* POSIX's clocks: clock_gettime and its family, gettimeofday, the
 * interval timers and times(). A backend without clock_gettime still has
 * __os_time for the real-time clock's seconds, and that is used rather
 * than failing a program that asked only for the date. */
#include <time.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/resource.h>
#include <unistd.h>

#include "posix.h"

WEAK int clock_gettime(clockid_t clock, struct timespec *t)
{
    struct __os_timespec k;
    if (__os_clock_gettime(clock, &k) == 0) {
        t->tv_sec = (time_t)k.sec;
        t->tv_nsec = k.nsec;
        return 0;
    }
    if (errno == ENOSYS && clock == CLOCK_REALTIME) {
        long s = __os_time();
        if (s < 0)
            return -1;
        t->tv_sec = s;
        t->tv_nsec = 0;
        return 0;
    }
    return -1;
}

WEAK int clock_getres(clockid_t clock, struct timespec *t)
{
    struct __os_timespec k;
    if (__os_clock_getres(clock, &k) < 0) {
        if (errno != ENOSYS || clock != CLOCK_REALTIME)
            return -1;
        k.sec = 1;               /* __os_time's resolution: a second */
        k.nsec = 0;
    }
    if (t) {
        t->tv_sec = (time_t)k.sec;
        t->tv_nsec = k.nsec;
    }
    return 0;
}

WEAK int clock_settime(clockid_t clock, const struct timespec *t)
{
    struct __os_timespec k = { t->tv_sec, t->tv_nsec };
    return __os_clock_settime(clock, &k);
}

/* returns the error, as POSIX's clock_nanosleep does, not -1 */
WEAK int clock_nanosleep(clockid_t clock, int flags,
                         const struct timespec *req, struct timespec *rem)
{
    struct __os_timespec k = { req->tv_sec, req->tv_nsec }, kr;
    int saved = errno;
    if (__os_clock_nanosleep(clock, flags & TIMER_ABSTIME, &k,
                             rem ? &kr : NULL) == 0)
        return 0;
    int err = errno;
    errno = saved;
    if (err == EINTR && rem && !(flags & TIMER_ABSTIME)) {
        rem->tv_sec = (time_t)kr.sec;
        rem->tv_nsec = kr.nsec;
    }
    return err;
}

/* tz is obsolete, and filled with zeros where it is asked for */
WEAK int gettimeofday(struct timeval *restrict tv, void *restrict tz)
{
    struct timespec t;
    if (tz)
        memset(tz, 0, sizeof(struct timezone));
    if (!tv)
        return 0;
    if (clock_gettime(CLOCK_REALTIME, &t) < 0)
        return -1;
    tv->tv_sec = t.tv_sec;
    tv->tv_usec = t.tv_nsec / 1000;
    return 0;
}

WEAK int settimeofday(const struct timeval *tv, const struct timezone *tz)
{
    (void)tz;
    if (!tv)
        return 0;
    struct timespec t = { tv->tv_sec, tv->tv_usec * 1000 };
    return clock_settime(CLOCK_REALTIME, &t);
}

static void itimer_out(const struct __os_itimer *k, struct itimerval *v)
{
    v->it_interval.tv_sec = (time_t)k->isec;
    v->it_interval.tv_usec = (suseconds_t)k->iusec;
    v->it_value.tv_sec = (time_t)k->vsec;
    v->it_value.tv_usec = (suseconds_t)k->vusec;
}

WEAK int setitimer(int which, const struct itimerval *restrict val,
                   struct itimerval *restrict old)
{
    struct __os_itimer k = { val->it_interval.tv_sec, val->it_interval.tv_usec,
                             val->it_value.tv_sec, val->it_value.tv_usec };
    struct __os_itimer ko;
    if (__os_setitimer(which, &k, old ? &ko : NULL) < 0)
        return -1;
    if (old)
        itimer_out(&ko, old);
    return 0;
}

WEAK int getitimer(int which, struct itimerval *cur)
{
    struct __os_itimer k;
    if (__os_getitimer(which, &k) < 0)
        return -1;
    itimer_out(&k, cur);
    return 0;
}

WEAK clock_t times(struct tms *t)
{
    long long k[4];
    long long r = __os_times(k);
    if (r == -1)
        return (clock_t)-1;
    if (t) {
        t->tms_utime = (clock_t)k[0];
        t->tms_stime = (clock_t)k[1];
        t->tms_cutime = (clock_t)k[2];
        t->tms_cstime = (clock_t)k[3];
    }
    return (clock_t)r;
}
