/* Local time: TZ as POSIX writes it, tzset's variables, localtime's
 * tm_isdst, tm_gmtoff and tm_zone, mktime's reading of tm_isdst on the
 * mornings the clocks change, strftime's %z and %Z, and strptime.
 *
 * Every value is what glibc 2.36 gave for the same call (the program was
 * run against both on Linux); posixdemo's cases are EmbLinkOS's own test
 * of its time zones, which this library now serves there. No files are
 * read, so it runs wherever there is a setenv -- the boards too. */
// expect-exit: 42
// needs-libc: POSIX's time zones and strptime, from lib/libc
#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int at(const char *tz, long long t, const char *want, int isdst,
              long gmtoff)
{
    setenv("TZ", tz, 1);
    tzset();
    time_t tt = (time_t)t;
    struct tm tm;
    char b[64];
    if (!localtime_r(&tt, &tm))
        return 1;
    strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S %Z %z", &tm);
    if (strcmp(b, want) || tm.tm_isdst != isdst || tm.tm_gmtoff != gmtoff)
        return 1;
    struct tm back = tm;
    return mktime(&back) != tt;      /* and back again, exactly */
}

static int zones(void)
{
    static const struct {
        const char *tz;
        long long t;
        const char *want;
        int isdst;
        long gmtoff;
    } c[] = {
        { "UTC0", 1700000000, "2023-11-14 22:13:20 UTC +0000", 0, 0 },
        { "EST5EDT,M3.2.0,M11.1.0", 1700000000,
          "2023-11-14 17:13:20 EST -0500", 0, -18000 },
        { "EST5EDT,M3.2.0,M11.1.0", 1689000000,
          "2023-07-10 10:40:00 EDT -0400", 1, -14400 },
        { "CET-1CEST,M3.5.0,M10.5.0/3", 1700000000,
          "2023-11-14 23:13:20 CET +0100", 0, 3600 },
        { "CET-1CEST,M3.5.0,M10.5.0/3", 1689000000,
          "2023-07-10 16:40:00 CEST +0200", 1, 7200 },
        { "IST-5:30", 1700000000, "2023-11-15 03:43:20 IST +0530", 0, 19800 },
        /* the southern hemisphere: summer across New Year */
        { "AEST-10AEDT,M10.1.0,M4.1.0/3", 1700000000,
          "2023-11-15 09:13:20 AEDT +1100", 1, 39600 },
        { "AEST-10AEDT,M10.1.0,M4.1.0/3", 1689000000,
          "2023-07-11 00:40:00 AEST +1000", 0, 36000 },
        /* the transition instants themselves, each side */
        { "EST5EDT,M3.2.0,M11.1.0", 1710054000,
          "2024-03-10 03:00:00 EDT -0400", 1, -14400 },
        { "EST5EDT,M3.2.0,M11.1.0", 1710053999,
          "2024-03-10 01:59:59 EST -0500", 0, -18000 },
        { "EST5EDT,M3.2.0,M11.1.0", 1730613600,
          "2024-11-03 01:00:00 EST -0500", 0, -18000 },
        { "EST5EDT,M3.2.0,M11.1.0", 1730613599,
          "2024-11-03 01:59:59 EDT -0400", 1, -14400 },
        /* no rules: the United States' */
        { "PST8PDT", 1689000000, "2023-07-10 07:40:00 PDT -0700", 1, -25200 },
        /* a quoted name, Julian days, a rule time past midnight */
        { "<+0330>-3:30", 1700000000, "2023-11-15 01:43:20 +0330 +0330", 0,
          12600 },
        { "XXX3YYY,J60/1,J300/1", 1689000000,
          "2023-07-10 12:40:00 YYY -0200", 1, -7200 },
        { "EST5EDT,M3.2.0/-1,M11.1.0/26", 1730613600,
          "2024-11-03 02:00:00 EDT -0400", 1, -14400 },
    };
    for (size_t i = 0; i < sizeof c / sizeof *c; i++)
        if (at(c[i].tz, c[i].t, c[i].want, c[i].isdst, c[i].gmtoff))
            return 1 + (int)i;

    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
    if (strcmp(tzname[0], "CET") || strcmp(tzname[1], "CEST") ||
        timezone != -3600 || !daylight)
        return 20;
    /* gmtime ignores TZ */
    time_t t = 1700000000;
    struct tm g;
    gmtime_r(&t, &g);
    if (g.tm_hour != 22 || g.tm_gmtoff != 0)
        return 21;
    return 0;
}

/* tm_isdst as glibc reads it, where a local time is ambiguous (the
 * clocks go back) or missing (they go forward) */
static int hints(void)
{
    setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);
    tzset();
    static const long long fall[3] = { 1730611800, 1730615400, 1730611800 };
    static const long long gap[3] = { 1710055800, 1710055800, 1710052200 };
    for (int h = -1; h <= 1; h++) {
        struct tm f = { .tm_year = 124, .tm_mon = 10, .tm_mday = 3,
                        .tm_hour = 1, .tm_min = 30, .tm_isdst = h };
        struct tm s = { .tm_year = 124, .tm_mon = 2, .tm_mday = 10,
                        .tm_hour = 2, .tm_min = 30, .tm_isdst = h };
        if (mktime(&f) != fall[h + 1] || mktime(&s) != gap[h + 1])
            return 30 + h;
    }
    /* out-of-range fields are normalised, and the result follows */
    struct tm n = { .tm_year = 123, .tm_mon = 12, .tm_mday = 32,
                    .tm_hour = 25, .tm_isdst = -1 };
    time_t r = mktime(&n);
    if (n.tm_year != 124 || n.tm_mon != 1 || n.tm_mday != 2 ||
        n.tm_hour != 1 || r != 1706853600)
        return 33;
    return 0;
}

static int parse(void)
{
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    char *e = strptime("2024-03-10 14:05:09", "%Y-%m-%d %H:%M:%S", &tm);
    if (!e || *e || tm.tm_year != 124 || tm.tm_mon != 2 || tm.tm_mday != 10 ||
        tm.tm_hour != 14 || tm.tm_min != 5 || tm.tm_sec != 9)
        return 40;
    memset(&tm, 0, sizeof tm);
    e = strptime("Tue,   5 nOv 69 11:30 PM rest", "%a, %d %b %y %I:%M %p",
                 &tm);
    if (!e || strcmp(e, " rest") || tm.tm_wday != 2 || tm.tm_mon != 10 ||
        tm.tm_mday != 5 || tm.tm_year != 69 || tm.tm_hour != 23 ||
        tm.tm_min != 30)
        return 41;
    /* a name is not preceded by blanks the format does not have */
    if (strptime("  Tue", "%a", &tm))
        return 45;
    memset(&tm, 0, sizeof tm);
    if (!strptime("12/31/68", "%D", &tm) || tm.tm_year != 168)
        return 42;
    memset(&tm, 0, sizeof tm);
    if (!strptime("366 +0530", "%j %z", &tm) || tm.tm_yday != 365 ||
        tm.tm_gmtoff != 19800)
        return 43;
    memset(&tm, 0, sizeof tm);
    if (strptime("2024-13-01", "%Y-%m-%d", &tm) || strptime("x", "%d", &tm) ||
        strptime("-5", "%Y", &tm) || strptime("+1260", "%z", &tm) ||
        strptime("10:59:62", "%T", &tm))
        return 44;
    /* a number stops where one more digit could not fit its field */
    memset(&tm, 0, sizeof tm);
    e = strptime("10:61", "%H:%M", &tm);
    if (!e || strcmp(e, "1") || tm.tm_min != 6)
        return 46;
    /* the fields a date implies: weekday and day of the year */
    memset(&tm, 0, sizeof tm);
    if (!strptime("2024-02-29", "%F", &tm) || tm.tm_wday != 4 ||
        tm.tm_yday != 59)
        return 47;
    memset(&tm, 0, sizeof tm);
    if (!strptime("60 2024", "%j %Y", &tm) || tm.tm_mon != 1 ||
        tm.tm_mday != 29 || tm.tm_wday != 4)
        return 48;
    memset(&tm, 0, sizeof tm);
    if (!strptime("2024 12 3", "%Y %U %w", &tm) || tm.tm_mon != 2 ||
        tm.tm_mday != 27 || tm.tm_yday != 86)
        return 49;
    memset(&tm, 0, sizeof tm);
    if (!strptime("Mon 2024 10", "%a %Y %W", &tm) || tm.tm_mon != 2 ||
        tm.tm_mday != 4)
        return 50;
    /* %D's year takes a later %C's century; %I joins %p, %H does not */
    memset(&tm, 0, sizeof tm);
    if (!strptime("03/09/24 21", "%D %C", &tm) || tm.tm_year != 224 ||
        tm.tm_wday != 4)
        return 51;
    memset(&tm, 0, sizeof tm);
    if (!strptime("12 AM", "%I %p", &tm) || tm.tm_hour != 0 ||
        !strptime("3 PM", "%H %p", &tm) || tm.tm_hour != 3)
        return 52;
    return 0;
}

int main(void)
{
    int r;
    if ((r = zones()) || (r = hints()) || (r = parse()))
        return r;
    return 42;
}
