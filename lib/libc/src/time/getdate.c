/* XSI's getdate, GNU's getdate_r, and the small old ones: timelocal,
 * dysize, stime, ftime.
 *
 * getdate tries each template in the file $DATEMSK names, with
 * strptime, until one takes the whole string (trailing blanks allowed).
 * What the template did not give comes from now, as POSIX describes:
 * no year, this year; no month, this month (the next one that has the
 * weekday or day asked for); no day, today -- tomorrow when only a time
 * was given and it has passed; no time, the time it is now. */
#define _GNU_SOURCE 1
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/timeb.h>

#define WEAK __attribute__((weak))

WEAK int getdate_err;

#define UNSET INT_MIN

static int fill(const char *s, const char *fmt, struct tm *tm)
{
    struct tm t;
    memset(&t, 0, sizeof t);
    t.tm_sec = t.tm_min = t.tm_hour = UNSET;
    t.tm_mday = t.tm_mon = t.tm_year = t.tm_wday = UNSET;
    const char *end = strptime(s, fmt, &t);
    if (!end)
        return 0;
    while (*end == ' ' || *end == '\t' || *end == '\n')
        end++;
    if (*end)
        return 0;

    time_t now_t = time(0);
    struct tm now;
    localtime_r(&now_t, &now);
    int has_time = t.tm_hour != UNSET || t.tm_min != UNSET || t.tm_sec != UNSET;
    int has_date = t.tm_mday != UNSET || t.tm_mon != UNSET ||
                   t.tm_year != UNSET || t.tm_wday != UNSET;
    if (t.tm_year == UNSET)
        t.tm_year = now.tm_year;
    if (t.tm_mon == UNSET)
        t.tm_mon = t.tm_mday != UNSET || t.tm_wday != UNSET ? now.tm_mon
                   : has_date ? now.tm_mon : now.tm_mon;
    if (t.tm_mday == UNSET)
        t.tm_mday = has_date && t.tm_wday == UNSET && t.tm_mon != now.tm_mon
                    ? 1 : now.tm_mday;
    if (!has_time) {
        t.tm_hour = now.tm_hour;
        t.tm_min = now.tm_min;
        t.tm_sec = now.tm_sec;
    } else {
        if (t.tm_hour == UNSET)
            t.tm_hour = 0;
        if (t.tm_min == UNSET)
            t.tm_min = 0;
        if (t.tm_sec == UNSET)
            t.tm_sec = 0;
    }
    int wday = t.tm_wday;
    t.tm_wday = 0;
    t.tm_isdst = -1;
    time_t when = mktime(&t);
    if (when == (time_t)-1)
        return 0;
    if (wday != UNSET && t.tm_mday == now.tm_mday && t.tm_mon == now.tm_mon) {
        /* a weekday alone: the first such day from today */
        t.tm_mday += (wday - t.tm_wday + 7) % 7;
        t.tm_isdst = -1;
        when = mktime(&t);
    } else if (has_time && !has_date && when < now_t) {
        t.tm_mday++;           /* a time already past: tomorrow's */
        t.tm_isdst = -1;
        when = mktime(&t);
    }
    *tm = t;
    return 1;
}

/* 0, or getdate_err's number for what went wrong */
WEAK int getdate_r(const char *s, struct tm *tm)
{
    const char *path = getenv("DATEMSK");
    if (!path || !*path)
        return 1;
    struct stat st;
    if (stat(path, &st))
        return 3;
    if (!S_ISREG(st.st_mode))
        return 4;
    FILE *f = fopen(path, "re");
    if (!f)
        return 2;
    char line[512];
    int r = 7;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        if (fill(s, line, tm)) {
            r = 0;
            break;
        }
    }
    if (r && ferror(f))
        r = 5;
    fclose(f);
    return r;
}

WEAK struct tm *getdate(const char *s)
{
    static struct tm tm;
    int e = getdate_r(s, &tm);
    if (e) {
        getdate_err = e;
        return 0;
    }
    return &tm;
}

WEAK time_t timelocal(struct tm *tm) { return mktime(tm); }

WEAK int dysize(int year)
{
    return (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) ? 366 : 365;
}

WEAK int stime(const time_t *t)
{
    struct timespec ts = { *t, 0 };
    return clock_settime(CLOCK_REALTIME, &ts);
}

WEAK int ftime(struct timeb *tb)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    tb->time = ts.tv_sec;
    tb->millitm = (unsigned short)(ts.tv_nsec / 1000000);
    tb->timezone = 0;
    tb->dstflag = 0;
    return 0;
}
