/* <time.h>: the calendar, and whatever clock the backend has.
 *
 * The calendar conversions are the whole of this file's difficulty, and
 * the usual implementations get them wrong at the edges -- before 1970,
 * across 1900 and 2100 (not leap years) and 2000 (which is), and for
 * mktime given a tm that is out of range on purpose (`tm_mday = 0` to mean
 * "the last day of the previous month" is a documented idiom).
 *
 * So the conversion is done with the days-from-civil algorithm rather than
 * by counting years in a loop: shift the year so it starts in March, which
 * puts the leap day at the END of the year where it perturbs nothing, and
 * the month lengths become a closed form. It is exact for the whole range
 * of time_t, branch-free, and needs no table.
 */
#include <time.h>

#include <stdio.h>
#include <string.h>

#include "../../os/backend.h"

#define SEC_DAY 86400L

/* Days since 1970-01-01 for a civil (proleptic Gregorian) date. y is the
 * full year, m is 1..12, d is 1..31. */
static long days_from_civil(long y, unsigned m, unsigned d)
{
    y -= m <= 2;                               /* year starts in March */
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);  /* 0..399 */
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

/* The inverse. */
static void civil_from_days(long z, long *y, unsigned *m, unsigned *d)
{
    z += 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);          /* 0..146096 */
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long yy = (long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;                    /* 0..11, Mar=0 */
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp + (mp < 10 ? 3 : -9);
    *y = yy + (*m <= 2);
}

static int is_leap(long y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

/* Floor division and its remainder: C's / truncates toward zero, which is
 * wrong for times before the epoch -- -1 seconds is 23:59:59 of the
 * previous day, not -0:00:01 of this one. */
static long fdiv(long a, long b)
{
    long q = a / b;
    if (a % b != 0 && (a < 0) != (b < 0))
        q--;
    return q;
}

static long fmod_(long a, long b) { return a - fdiv(a, b) * b; }

struct tm *gmtime_r(const time_t *restrict t, struct tm *restrict out)
{
    long secs = *t;
    long days = fdiv(secs, SEC_DAY);
    long rem = fmod_(secs, SEC_DAY);

    out->tm_hour = (int)(rem / 3600);
    out->tm_min = (int)(rem % 3600 / 60);
    out->tm_sec = (int)(rem % 60);

    /* 1970-01-01 was a Thursday, so day 0 is wday 4. */
    out->tm_wday = (int)fmod_(days + 4, 7);

    long y;
    unsigned m, d;
    civil_from_days(days, &y, &m, &d);
    out->tm_year = (int)(y - 1900);
    out->tm_mon = (int)m - 1;
    out->tm_mday = (int)d;
    out->tm_yday = (int)(days - days_from_civil(y, 1, 1));
    out->tm_isdst = 0;
    return out;
}

time_t timegm(struct tm *tm)
{
    /* Normalising the month first is what lets the rest work on an
     * out-of-range tm: a caller that sets tm_mon = 12 means January of the
     * next year, and the day count below then needs no special case. */
    long y = (long)tm->tm_year + 1900;
    long mon = tm->tm_mon;
    y += fdiv(mon, 12);
    mon = fmod_(mon, 12);

    long days = days_from_civil(y, (unsigned)mon + 1, 1) + (tm->tm_mday - 1);
    long secs = days * SEC_DAY + tm->tm_hour * 3600L + tm->tm_min * 60L +
                tm->tm_sec;

    /* §7.27.2.3p2: mktime writes the normalised fields back, so a caller
     * can use it to answer "what day is 90 days from now". */
    time_t out = secs;
    gmtime_r(&out, tm);
    return out;
}

time_t mktime(struct tm *tm) { return timegm(tm); }

/* No timezone database, so local time is UTC. That is a real limitation
 * and it is stated in the header rather than hidden behind a TZ variable
 * this library would silently ignore. */
struct tm *localtime_r(const time_t *restrict t, struct tm *restrict out)
{
    return gmtime_r(t, out);
}

static struct tm tm_static;

struct tm *gmtime(const time_t *t) { return gmtime_r(t, &tm_static); }
struct tm *localtime(const time_t *t) { return gmtime_r(t, &tm_static); }

double difftime(time_t end, time_t start) { return (double)(end - start); }

time_t time(time_t *t)
{
    long now = __os_time();
    if (t)
        *t = now;
    return now;
}

clock_t clock(void)
{
    long ns = __os_clock_ns();
    if (ns < 0)
        return (clock_t)-1;
    return (clock_t)(ns / (1000000000L / CLOCKS_PER_SEC));
}

int timespec_get(struct timespec *ts, int base)
{
    if (base != TIME_UTC)
        return 0;
    long now = __os_time();
    if (now < 0)
        return 0;
    ts->tv_sec = now;
    ts->tv_nsec = 0;
    return base;
}

/* The "C" locale's names, which is the only locale this library has. They
 * are data, not a locale database: %A is "Sunday" in the C locale, so it
 * is "Sunday" here rather than the abbreviation a lazy strftime returns. */
static const char wday_name[7][4] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};
static const char wday_full[7][10] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday",
    "Saturday"
};
static const char mon_name[12][4] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};
static const char mon_full[12][10] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

/* §7.27.3.1: the exact format, including the space-padded day. */
char *asctime_r(const struct tm *restrict tm, char *restrict buf)
{
    unsigned w = (unsigned)tm->tm_wday, m = (unsigned)tm->tm_mon;
    snprintf(buf, 26, "%.3s %.3s%3d %.2d:%.2d:%.2d %d\n",
             w < 7 ? wday_name[w] : "???", m < 12 ? mon_name[m] : "???",
             tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec,
             1900 + tm->tm_year);
    return buf;
}

static char asctime_static[32];

char *asctime(const struct tm *tm)
{
    return asctime_r(tm, asctime_static);
}

char *ctime_r(const time_t *restrict t, char *restrict buf)
{
    struct tm tmp;
    return asctime_r(localtime_r(t, &tmp), buf);
}

char *ctime(const time_t *t) { return asctime(localtime(t)); }

/* ---- strftime ----------------------------------------------------------
 * The one place this file has to be careful about the return value: 0 on
 * overflow, and 0 is ALSO what a format that legitimately produces nothing
 * returns, which the standard acknowledges and callers must handle. The
 * buffer is written through a bounds-checked appender so a conversion that
 * would overflow stops the whole call rather than truncating silently.
 */
struct sb { char *p; size_t cap, len; int over; };

static void sb_put(struct sb *b, const char *s, size_t n)
{
    if (b->over)
        return;
    if (b->len + n >= b->cap) { b->over = 1; return; }
    memcpy(b->p + b->len, s, n);
    b->len += n;
}

static void sb_str(struct sb *b, const char *s) { sb_put(b, s, strlen(s)); }

/* The compound conversions (%c, %D, %F, %T ...) are defined in terms of
 * other conversions, so they recurse. Through a scratch buffer rather than
 * in place: a nested call that overflows returns 0 having written nothing
 * useful, and appending strlen() of that is a read of whatever was in the
 * caller's buffer. */
static void sb_fmt(struct sb *b, const char *f, const struct tm *tm)
{
    if (b->over)
        return;
    char tmp[128];
    size_t n = strftime(tmp, sizeof tmp, f, tm);
    if (n == 0) { b->over = 1; return; }
    sb_put(b, tmp, n);
}

static void sb_num(struct sb *b, long v, int width, char pad)
{
    char t[24];
    int n = 0, neg = v < 0;
    unsigned long u = (unsigned long)(neg ? -v : v);
    do { t[n++] = (char)('0' + u % 10); u /= 10; } while (u);
    if (neg) t[n++] = '-';
    while (n < width) t[n++] = pad;
    while (n--) sb_put(b, &t[n], 1);
}

/* ISO 8601 week numbering: the week containing the year's first Thursday
 * is week 1, which is why this needs the year as well as the week and
 * cannot be computed from tm_yday alone. */
static void iso_week(const struct tm *tm, int *week, long *year)
{
    long y = tm->tm_year + 1900L;
    int wd = (tm->tm_wday + 6) % 7;           /* Monday = 0 */
    int w = (tm->tm_yday - wd + 10) / 7;
    if (w < 1) {                               /* last week of last year */
        y--;
        int pdays = is_leap(y) ? 366 : 365;
        w = (tm->tm_yday + pdays - wd + 10) / 7;
    } else if (w > 52) {
        int days = is_leap(y) ? 366 : 365;
        if (days - tm->tm_yday < 4 - wd)       /* week 1 of next year */
            { w = 1; y++; }
    }
    *week = w;
    *year = y;
}

size_t strftime(char *restrict s, size_t max, const char *restrict fmt,
                const struct tm *restrict tm)
{
    struct sb b;
    b.p = s; b.cap = max; b.len = 0; b.over = 0;

    unsigned wd = (unsigned)tm->tm_wday, mo = (unsigned)tm->tm_mon;
    const char *wname = wd < 7 ? wday_name[wd] : "???";
    const char *mname = mo < 12 ? mon_name[mo] : "???";

    for (const char *f = fmt; *f; f++) {
        if (*f != '%') { sb_put(&b, f, 1); continue; }
        f++;
        if (*f == 'E' || *f == 'O')            /* locale alternatives: none */
            f++;
        switch (*f) {
        case 'a': sb_put(&b, wname, 3); break;
        case 'A': sb_str(&b, wd < 7 ? wday_full[wd] : "???"); break;
        case 'b': case 'h': sb_put(&b, mname, 3); break;
        case 'B': sb_str(&b, mo < 12 ? mon_full[mo] : "???"); break;
        case 'c': sb_fmt(&b, "%a %b %e %H:%M:%S %Y", tm); break;
        case 'C': sb_num(&b, (tm->tm_year + 1900) / 100, 2, '0'); break;
        case 'd': sb_num(&b, tm->tm_mday, 2, '0'); break;
        case 'D': sb_fmt(&b, "%m/%d/%y", tm); break;
        case 'e': sb_num(&b, tm->tm_mday, 2, ' '); break;
        case 'F': sb_fmt(&b, "%Y-%m-%d", tm); break;
        case 'g': { int w; long y; iso_week(tm, &w, &y);
                    sb_num(&b, y % 100, 2, '0'); } break;
        case 'G': { int w; long y; iso_week(tm, &w, &y);
                    sb_num(&b, y, 4, '0'); } break;
        case 'H': sb_num(&b, tm->tm_hour, 2, '0'); break;
        case 'I': { int h = tm->tm_hour % 12; if (!h) h = 12;
                    sb_num(&b, h, 2, '0'); } break;
        case 'j': sb_num(&b, tm->tm_yday + 1, 3, '0'); break;
        case 'm': sb_num(&b, tm->tm_mon + 1, 2, '0'); break;
        case 'M': sb_num(&b, tm->tm_min, 2, '0'); break;
        case 'n': sb_put(&b, "\n", 1); break;
        case 'p': sb_str(&b, tm->tm_hour < 12 ? "AM" : "PM"); break;
        case 'r': sb_fmt(&b, "%I:%M:%S %p", tm); break;
        case 'R': sb_fmt(&b, "%H:%M", tm); break;
        case 'S': sb_num(&b, tm->tm_sec, 2, '0'); break;
        case 't': sb_put(&b, "\t", 1); break;
        case 'T': sb_fmt(&b, "%H:%M:%S", tm); break;
        case 'u': sb_num(&b, tm->tm_wday == 0 ? 7 : tm->tm_wday, 0, '0');
                  break;
        case 'U': sb_num(&b, (tm->tm_yday + 7 - tm->tm_wday) / 7, 2, '0');
                  break;
        case 'V': { int w; long y; iso_week(tm, &w, &y);
                    sb_num(&b, w, 2, '0'); } break;
        case 'w': sb_num(&b, tm->tm_wday, 0, '0'); break;
        case 'W': sb_num(&b, (tm->tm_yday + 7 - (tm->tm_wday + 6) % 7) / 7,
                         2, '0'); break;
        case 'x': sb_fmt(&b, "%m/%d/%y", tm); break;
        case 'X': sb_fmt(&b, "%H:%M:%S", tm); break;
        case 'y': sb_num(&b, (tm->tm_year + 1900) % 100, 2, '0'); break;
        case 'Y': sb_num(&b, tm->tm_year + 1900, 0, '0'); break;
        case 'z': sb_str(&b, "+0000"); break;  /* UTC, per localtime */
        case 'Z': sb_str(&b, "UTC"); break;
        case '%': sb_put(&b, "%", 1); break;
        case 0:   f--; break;                  /* trailing '%': stop */
        default:  sb_put(&b, "%", 1); sb_put(&b, f, 1); break;
        }
    }
    if (b.over || b.len >= b.cap)
        return 0;
    b.p[b.len] = 0;
    return b.len;
}
