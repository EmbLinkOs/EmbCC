/* <time.h> — C11 §7.27.
 *
 * The calendar is complete and exact; the CLOCK is whatever the OS
 * backend offers (__os_time, __os_clock_ns) and may be nothing at all.
 * Those are separate concerns and this header keeps them separate: a
 * program that converts a time_t it got from elsewhere works on a target
 * with no clock.
 *
 * There is no timezone database. localtime is UTC, and says so rather
 * than pretending -- see src/time/time.c.
 */
#ifndef _TIME_H
#define _TIME_H

#include <features.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _TIME_T_DECLARED          /* (and in <sys/types.h>) */
#define _TIME_T_DECLARED
typedef long time_t;
#endif
#ifndef _CLOCK_T_DECLARED
#define _CLOCK_T_DECLARED
typedef long clock_t;
#endif

#define CLOCKS_PER_SEC 1000000L

struct tm {
    int tm_sec;    /* 0..60, 60 for a leap second */
    int tm_min;    /* 0..59 */
    int tm_hour;   /* 0..23 */
    int tm_mday;   /* 1..31 */
    int tm_mon;    /* 0..11 */
    int tm_year;   /* years since 1900 */
    int tm_wday;   /* 0..6, Sunday is 0 */
    int tm_yday;   /* 0..365 */
    int tm_isdst;  /* always 0: no timezone database */
#if defined(__APPLE__)
    /* Apple's C library has two more, and its mktime, localtime_r and
     * gmtime_r fill them in: a 36-byte struct tm on the caller's stack
     * was overrun by 20 bytes. */
    long tm_gmtoff;
    char *tm_zone;
#endif
};

#ifndef _STRUCT_TIMESPEC_DECLARED    /* (and in <sys/stat.h>, <signal.h>) */
#define _STRUCT_TIMESPEC_DECLARED
struct timespec { time_t tv_sec; long tv_nsec; };
#endif

#define TIME_UTC 1

time_t time(time_t *t);
clock_t clock(void);
double difftime(time_t end, time_t start);
int timespec_get(struct timespec *ts, int base);
/* C23: how fine timespec_get's answer is in that base. */
int timespec_getres(struct timespec *ts, int base);

time_t mktime(struct tm *tm);
/* Not in C11, but the honest name for what mktime does here, and what a
 * program that means UTC should call. */
time_t timegm(struct tm *tm);
struct tm *gmtime(const time_t *t);
struct tm *localtime(const time_t *t);
struct tm *gmtime_r(const time_t *__restrict t, struct tm *__restrict out);
struct tm *localtime_r(const time_t *__restrict t, struct tm *__restrict out);

char *asctime(const struct tm *tm);
char *ctime(const time_t *t);
char *asctime_r(const struct tm *__restrict tm, char *__restrict buf);
char *ctime_r(const time_t *__restrict t, char *__restrict buf);
size_t strftime(char *__restrict s, size_t max, const char *__restrict fmt,
                const struct tm *__restrict tm);

/* POSIX's clocks and sleeps, and the time zone (src/posix/time.c) */
#if defined(__EMB_POSIX)
#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID  3
#define CLOCK_MONOTONIC_RAW      4
#define CLOCK_REALTIME_COARSE    5
#define CLOCK_MONOTONIC_COARSE   6
#define CLOCK_BOOTTIME           7
#define TIMER_ABSTIME            1

#ifndef _CLOCKID_T_DECLARED
#define _CLOCKID_T_DECLARED
typedef int clockid_t;
#endif

int clock_gettime(clockid_t clock, struct timespec *t);
int clock_getres(clockid_t clock, struct timespec *t);
int clock_settime(clockid_t clock, const struct timespec *t);
int clock_nanosleep(clockid_t clock, int flags, const struct timespec *req,
                    struct timespec *rem);
int nanosleep(const struct timespec *req, struct timespec *rem);

/* TZ, as POSIX spells it ("CET-1CEST,M3.5.0,M10.5.0/3"), or a zone
 * file's name where the system has /usr/share/zoneinfo */
extern char *tzname[2];
extern long timezone;
extern int daylight;
void tzset(void);
char *strptime(const char *__restrict s, const char *__restrict fmt,
               struct tm *__restrict tm);
#endif

#ifdef __cplusplus
}
#endif

#endif
