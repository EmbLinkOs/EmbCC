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

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef long time_t;
typedef long clock_t;

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
};

struct timespec { time_t tv_sec; long tv_nsec; };

#define TIME_UTC 1

time_t time(time_t *t);
clock_t clock(void);
double difftime(time_t end, time_t start);
int timespec_get(struct timespec *ts, int base);

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

#ifdef __cplusplus
}
#endif

#endif
