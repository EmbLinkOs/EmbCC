/* Local time: the TZ variable, tzset, localtime and mktime.
 *
 * TZ is read as POSIX writes it -- "CET-1CEST,M3.5.0,M10.5.0/3": a name,
 * an offset WEST of UTC, and optionally a summer name, its offset, and
 * the two rules for when it starts and ends (Mm.w.d, Jn or n, each with an
 * optional time, which may be negative or past 24 hours as POSIX.1-2024
 * allows). A name in angle brackets may hold digits and signs ("<+0530>").
 *
 * A TZ that is a ZONE's name ("Europe/Paris", ":Europe/Paris") is read
 * from the compiled time zone file of that name under TZDIR or
 * /usr/share/zoneinfo -- TZif, versions 1 to 3, with its footer's POSIX
 * rule for times after its last transition. No TZ at all means
 * /etc/localtime where there is one, and UTC where there is not.
 *
 * A TZ that is neither -- unparseable, or naming a file that is not there
 * -- is UTC, with the abbreviation "UTC". That is not an error: a program
 * started with a TZ it cannot use must still tell the time.
 */
#define _GNU_SOURCE 1
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>

#include "../internal/lock.h"

char *tzname[2] = { (char *)"UTC", (char *)"UTC" };
long timezone;
int daylight;

/* ---- the calendar this needs ------------------------------------------------ */

static long days_from_civil(long y, unsigned m, unsigned d)
{
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

static int is_leap(long y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static const unsigned char mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30,
                                         31, 30, 31 };

/* ---- a POSIX TZ rule --------------------------------------------------------- */

struct rule {
    char kind;                 /* 'J' (1..365, no Feb 29), 'D' (0..365), 'M' */
    int m, w, d, n;
    long time;                 /* seconds after local midnight */
};

struct zone {
    char std[16], dst[16];
    long std_east, dst_east;   /* seconds east of UTC */
    int has_dst;
    struct rule start, end;
    /* from a TZif file: transitions, and the types they switch to */
    long long *when;
    unsigned char *which;
    int ntrans;
    struct { long east; int isdst; char abbr[16]; } type[32];
    int ntypes;
    int has_rule;              /* a POSIX rule after the last transition */
};

static struct zone g;
static char g_tzkey[256];
static int g_set;
static __lock_t g_lock = LOCK_INIT;

static const char *parse_name(const char *s, char *out, size_t cap)
{
    size_t n = 0;
    if (*s == '<') {
        s++;
        while (*s && *s != '>') {
            if (n + 1 < cap)
                out[n++] = *s;
            s++;
        }
        if (*s != '>')
            return NULL;
        s++;
    } else
        while (isalpha((unsigned char)*s)) {
            if (n + 1 < cap)
                out[n++] = *s;
            s++;
        }
    out[n] = 0;
    return n >= 3 ? s : NULL;
}

/* [+-]hh[:mm[:ss]], hours up to 167 as POSIX.1-2024 allows for rule times */
static const char *parse_hms(const char *s, long *out)
{
    int neg = 0;
    if (*s == '+' || *s == '-')
        neg = *s++ == '-';
    if (!isdigit((unsigned char)*s))
        return NULL;
    long h = 0, m = 0, sec = 0;
    while (isdigit((unsigned char)*s))
        h = h * 10 + (*s++ - '0');
    if (*s == ':') {
        s++;
        while (isdigit((unsigned char)*s))
            m = m * 10 + (*s++ - '0');
        if (*s == ':') {
            s++;
            while (isdigit((unsigned char)*s))
                sec = sec * 10 + (*s++ - '0');
        }
    }
    if (h > 167 || m > 59 || sec > 59)
        return NULL;
    long v = h * 3600 + m * 60 + sec;
    *out = neg ? -v : v;
    return s;
}

static const char *parse_rule(const char *s, struct rule *r)
{
    r->time = 7200;                       /* 02:00, POSIX's default */
    if (*s == 'M') {
        r->kind = 'M';
        char *e;
        r->m = (int)strtol(s + 1, &e, 10);
        if (*e != '.')
            return NULL;
        r->w = (int)strtol(e + 1, &e, 10);
        if (*e != '.')
            return NULL;
        r->d = (int)strtol(e + 1, &e, 10);
        if (r->m < 1 || r->m > 12 || r->w < 1 || r->w > 5 || r->d < 0 ||
            r->d > 6)
            return NULL;
        s = e;
    } else {
        r->kind = 'D';
        if (*s == 'J') {
            r->kind = 'J';
            s++;
        }
        if (!isdigit((unsigned char)*s))
            return NULL;
        char *e;
        r->n = (int)strtol(s, &e, 10);
        if (r->kind == 'J' ? r->n < 1 || r->n > 365 : r->n > 365)
            return NULL;
        s = e;
    }
    if (*s == '/' && !(s = parse_hms(s + 1, &r->time)))
        return NULL;
    return s;
}

/* the TZ string into z; 0, or -1 when it is not one */
static int parse_posix(const char *s, struct zone *z)
{
    long off;
    if (!(s = parse_name(s, z->std, sizeof z->std)) ||
        !(s = parse_hms(s, &off)))
        return -1;
    z->std_east = -off;
    z->dst_east = z->std_east;
    z->has_dst = 0;
    strcpy(z->dst, z->std);
    if (!*s)
        return 0;
    if (!(s = parse_name(s, z->dst, sizeof z->dst)))
        return -1;
    z->has_dst = 1;
    z->dst_east = z->std_east + 3600;
    if (*s && *s != ',') {
        if (!(s = parse_hms(s, &off)))
            return -1;
        z->dst_east = -off;
    }
    if (!*s) {
        /* no rules: the United States' since 2007, which is what glibc's
         * posixrules file has said for as long as there has been one */
        z->start = (struct rule){ 'M', 3, 2, 0, 0, 7200 };
        z->end = (struct rule){ 'M', 11, 1, 0, 0, 7200 };
        return 0;
    }
    if (*s != ',' || !(s = parse_rule(s + 1, &z->start)) || *s != ',' ||
        !(s = parse_rule(s + 1, &z->end)) || *s)
        return -1;
    return 0;
}

/* the day of year y (0-based) a rule names */
static long rule_day(const struct rule *r, long y)
{
    if (r->kind == 'J')
        return r->n - 1 + (is_leap(y) && r->n >= 60);
    if (r->kind == 'D')
        return r->n;
    long first = days_from_civil(y, (unsigned)r->m, 1);
    long yday0 = first - days_from_civil(y, 1, 1);
    int wday = (int)(((first % 7) + 11) % 7);   /* 1970-01-01 a Thursday */
    long day = (r->d - wday + 7) % 7 + (long)(r->w - 1) * 7;
    int len = mdays[r->m - 1] + (r->m == 2 && is_leap(y));
    while (day >= len)
        day -= 7;
    return yday0 + day;
}

/* the year (proleptic Gregorian) holding day `days` since 1970-01-01 */
static long year_of(long long days)
{
    long long z = days + 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    long long doe = z - era * 146097;
    long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long long mp = (5 * doy + 2) / 153;
    return (long)(yoe + era * 400 + (mp >= 10));
}

/* Whether t (UTC) is in summer time, by the zone's rules: in the year that
 * holds t in standard time, summer starts at the start rule's time in
 * standard time and ends at the end rule's time in summer time. A zone
 * whose start comes after its end in the year (the southern hemisphere's)
 * is in summer outside that span rather than inside it. */
static int rule_isdst(const struct zone *z, long long t)
{
    if (!z->has_dst)
        return 0;
    long long ls = t + z->std_east;
    long y = year_of(ls >= 0 ? ls / 86400 : (ls - 86399) / 86400);
    long long base = (long long)days_from_civil(y, 1, 1) * 86400;
    long long st = base + rule_day(&z->start, y) * 86400LL + z->start.time -
                   z->std_east;
    long long en = base + rule_day(&z->end, y) * 86400LL + z->end.time -
                   z->dst_east;
    if (st < en)
        return t >= st && t < en;
    return !(t >= en && t < st);
}

/* ---- the compiled zone files (TZif) ---------------------------------------- */

static long long be(const unsigned char *p, int n)
{
    long long v = 0;
    for (int i = 0; i < n; i++)
        v = v << 8 | p[i];
    if (n == 4)
        v = (long long)(int)(unsigned)v;
    return v;
}

/* The whole file, in a buffer that grows: a zone's is a few kilobytes,
 * the largest under 100, and 256 KiB is the most this will hold -- less
 * where size_t is 16 bits, as on AVR. */
static unsigned char *slurp(FILE *f, size_t *n)
{
    size_t cap = 4096, len = 0;
    unsigned char *buf = malloc(cap);
    while (buf) {
        len += fread(buf + len, 1, cap - len, f);
        if (len < cap)
            break;
        if (cap >= 262144UL || cap > (size_t)-1 / 4) {
            free(buf);
            return NULL;
        }
        unsigned char *b = realloc(buf, cap * 2);
        if (!b)
            free(buf);
        buf = b;
        cap *= 2;
    }
    *n = len;
    return buf;
}

static int load_tzif(const char *path, struct zone *z)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    size_t n = 0;
    unsigned char *buf = slurp(f, &n);
    fclose(f);
    if (!buf || n < 44 || memcmp(buf, "TZif", 4)) {
        free(buf);
        return -1;
    }
    const unsigned char *p = buf;
    int version = buf[4];
    int sz = 4;
    for (int pass = 0; pass < 2; pass++) {
        long isut = (long)be(p + 20, 4), isstd = (long)be(p + 24, 4),
             leap = (long)be(p + 28, 4), timecnt = (long)be(p + 32, 4),
             typecnt = (long)be(p + 36, 4), charcnt = (long)be(p + 40, 4);
        const unsigned char *d = p + 44;
        size_t body = (size_t)(timecnt * sz + timecnt + typecnt * 6 + charcnt +
                               leap * (sz + 4) + isstd + isut);
        if ((size_t)(d - buf) + body > n || typecnt < 1 || typecnt > 32) {
            free(buf);
            return -1;
        }
        if (pass == 0 && version >= '2') {
            p = d + body;                   /* the 64-bit block follows */
            sz = 8;
            continue;
        }
        free(z->when);
        free(z->which);
        z->when = malloc((size_t)(timecnt ? timecnt : 1) * sizeof *z->when);
        z->which = malloc((size_t)(timecnt ? timecnt : 1));
        if (!z->when || !z->which) {
            free(buf);
            return -1;
        }
        for (long i = 0; i < timecnt; i++)
            z->when[i] = be(d + i * sz, sz);
        d += timecnt * sz;
        memcpy(z->which, d, (size_t)timecnt);
        d += timecnt;
        const unsigned char *types = d, *chars = d + typecnt * 6;
        z->ntrans = (int)timecnt;
        z->ntypes = (int)typecnt;
        for (long i = 0; i < typecnt; i++) {
            z->type[i].east = (long)be(types + i * 6, 4);
            z->type[i].isdst = types[i * 6 + 4];
            int idx = types[i * 6 + 5];
            snprintf(z->type[i].abbr, sizeof z->type[i].abbr, "%s",
                     idx < charcnt ? (const char *)chars + idx : "");
        }
        for (int i = 0; i < z->ntrans; i++)
            if (z->which[i] >= z->ntypes)
                z->which[i] = 0;
        /* the footer: a POSIX TZ for the times after the last transition */
        z->has_rule = 0;
        const unsigned char *foot = d + typecnt * 6 + charcnt +
                                    leap * (sz + 4) + isstd + isut;
        if (sz == 8 && foot < buf + n && *foot == '\n') {
            char rule[128];
            size_t k = 0;
            for (const unsigned char *q = foot + 1;
                 q < buf + n && *q != '\n' && k + 1 < sizeof rule; q++)
                rule[k++] = (char)*q;
            rule[k] = 0;
            struct zone r;
            memset(&r, 0, sizeof r);
            if (k && parse_posix(rule, &r) == 0) {
                memcpy(z->std, r.std, sizeof z->std);
                memcpy(z->dst, r.dst, sizeof z->dst);
                z->std_east = r.std_east;
                z->dst_east = r.dst_east;
                z->has_dst = r.has_dst;
                z->start = r.start;
                z->end = r.end;
                z->has_rule = 1;
            }
        }
        if (!z->has_rule) {
            /* the names of the last standard and summer types */
            for (int i = 0; i < z->ntypes; i++) {
                char *dst = z->type[i].isdst ? z->dst : z->std;
                snprintf(dst, sizeof z->std, "%s", z->type[i].abbr);
                if (z->type[i].isdst)
                    z->has_dst = 1, z->dst_east = z->type[i].east;
                else
                    z->std_east = z->type[i].east;
            }
        }
        break;
    }
    free(buf);
    return 0;
}

/* ---- tzset ---------------------------------------------------------------------- */

static void set_utc(struct zone *z)
{
    strcpy(z->std, "UTC");
    strcpy(z->dst, "UTC");
    z->std_east = z->dst_east = 0;
    z->has_dst = 0;
    z->ntrans = 0;
    z->has_rule = 0;
}

static void tzset_locked(void)
{
    const char *tz = getenv("TZ");
    const char *key = tz ? tz : "\x01localtime";
    if (g_set && !strncmp(key, g_tzkey, sizeof g_tzkey - 1))
        return;
    snprintf(g_tzkey, sizeof g_tzkey, "%s", key);
    g_set = 1;
    set_utc(&g);
    if (!tz) {
        load_tzif("/etc/localtime", &g);
    } else {
        const char *name = *tz == ':' ? tz + 1 : tz;
        int posix = *tz != ':' && parse_posix(tz, &g) == 0;
        if (!posix && *name && !strstr(name, "..")) {
            char path[512];
            const char *dir = getenv("TZDIR");
            if (*name == '/')
                snprintf(path, sizeof path, "%s", name);
            else
                snprintf(path, sizeof path, "%s/%s",
                         dir && *dir ? dir : "/usr/share/zoneinfo", name);
            if (load_tzif(path, &g) < 0)
                set_utc(&g);
        } else if (!posix)
            set_utc(&g);
    }
    tzname[0] = g.std;
    tzname[1] = g.dst;
    timezone = -g.std_east;
    daylight = g.has_dst;
}

void tzset(void)
{
    __lock(&g_lock);
    tzset_locked();
    __unlock(&g_lock);
}

/* ---- the offset at a time ---------------------------------------------------------- */

static void offset_at(long long t, long *east, int *isdst, const char **abbr)
{
    if (g.ntrans && (!g.has_rule || t < g.when[g.ntrans - 1])) {
        int i;
        if (t < g.when[0]) {
            /* before the first transition: the first standard type */
            i = 0;
            for (int k = 0; k < g.ntypes; k++)
                if (!g.type[k].isdst) {
                    i = k;
                    break;
                }
        } else {
            int lo = 0, hi = g.ntrans - 1;
            while (lo < hi) {
                int mid = (lo + hi + 1) / 2;
                if (g.when[mid] <= t)
                    lo = mid;
                else
                    hi = mid - 1;
            }
            i = g.which[lo];
        }
        *east = g.type[i].east;
        *isdst = g.type[i].isdst;
        *abbr = g.type[i].abbr;
        return;
    }
    int d = rule_isdst(&g, t);
    *east = d ? g.dst_east : g.std_east;
    *isdst = d;
    *abbr = d ? g.dst : g.std;
}

struct tm *localtime_r(const time_t *restrict t, struct tm *restrict out)
{
    long east;
    int isdst;
    const char *abbr;
    __lock(&g_lock);
    tzset_locked();
    offset_at(*t, &east, &isdst, &abbr);
    __unlock(&g_lock);
    time_t local = *t + east;
    if (!gmtime_r(&local, out))
        return NULL;
    out->tm_isdst = isdst;
    out->tm_gmtoff = east;
    out->tm_zone = abbr;
    return out;
}

struct tm *localtime(const time_t *t)
{
    static struct tm tm;
    return localtime_r(t, &tm);
}

/* Local fields to a time. The fields are read at an offset, and which
 * offset is what tm_isdst says, as glibc reads it: > 0 the summer one,
 * 0 the standard one -- even where that offset is not the one in force
 * then, so "02:30, standard time" on the morning the clocks go forward is
 * 03:30 summer time -- and < 0 whichever is in force: the earlier of the
 * two where the time happens twice (the hour the clocks go back), the
 * standard one where it does not happen at all. The two candidates are
 * the offsets either side of the time, a day out, so a zone's history is
 * honoured as well as its rule. */
static int valid_at(long long as_utc, long east)
{
    long e;
    int d;
    const char *a;
    offset_at(as_utc - east, &e, &d, &a);
    return e == east;
}

time_t mktime(struct tm *tm)
{
    __lock(&g_lock);
    tzset_locked();
    struct tm u = *tm;
    long long as_utc = timegm(&u);
    long e1, e2;
    int d1, d2;
    const char *a;
    offset_at(as_utc - 86400, &e1, &d1, &a);
    offset_at(as_utc + 86400, &e2, &d2, &a);
    long o_std = !d1 ? e1 : !d2 ? e2 : g.std_east;
    long o_dst = d1 ? e1 : d2 ? e2 : g.has_dst ? g.dst_east : o_std;
    if (!d1 && !d2 && e1 != e2 && !valid_at(as_utc, o_std))
        o_std = e2;                /* the standard offset itself changed */
    long long t;
    if (tm->tm_isdst > 0)
        t = as_utc - o_dst;
    else if (tm->tm_isdst == 0)
        t = as_utc - o_std;
    else if (o_dst != o_std && valid_at(as_utc, o_dst))
        t = as_utc - o_dst;
    else
        t = as_utc - o_std;
    __unlock(&g_lock);
    time_t r = (time_t)t;
    localtime_r(&r, tm);
    return r;
}

/* ---- strptime ----------------------------------------------------------------
 *
 * POSIX's conversions in the "C" locale, with glibc's extensions (%k %l %s
 * %z %F %g %G) and glibc's readings wherever POSIX leaves a choice -- each
 * checked against glibc 2.36:
 *
 *  - A space in the format matches any amount of white space, none
 *    included; a number skips white space before it; a name does not.
 *  - A number reads digits only while one more could still fit the field:
 *    "61" for %M is 6, with "1" left over.
 *  - %y is 1969-2068, unless %C gives the century; %C alone is that
 *    century's year 0; %I is joined by %p; %H forgets an earlier %I.
 *  - Once a date field is read, the fields it implies follow: tm_wday and
 *    tm_yday from the year, month and day; the month and day from %j; and
 *    from %U or %W with a weekday, the date itself.
 *
 * Where glibc reads outside its month table (day 366 of a common year,
 * week 0) or truncates a negative year, the calendar is followed instead:
 * December 32, January -6, and the proleptic weekday.
 *
 * The nested formats (%c %D %r ...) share one state with the outer one,
 * so "%D" followed by "%C" combines; a nested format that fails leaves tm
 * as it was. */
static const char *const st_wday[7] = { "sunday", "monday", "tuesday",
    "wednesday", "thursday", "friday", "saturday" };
static const char *const st_mon[12] = { "january", "february", "march",
    "april", "may", "june", "july", "august", "september", "october",
    "november", "december" };
static const unsigned short st_yday[2][13] = {
    { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365 },
    { 0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366 },
};

struct st_state {
    int century, want_century, want_xday;
    int have_I, is_pm, have_wday, have_yday, have_mon, have_mday;
    int have_uweek, have_wweek, week_no;
};

static const char *st_num(const char *s, int min, int max, int digits,
                          int *out)
{
    while (isspace((unsigned char)*s))
        s++;
    if (!isdigit((unsigned char)*s))
        return NULL;
    int v = 0;
    do
        v = v * 10 + (*s++ - '0');
    while (--digits > 0 && v * 10 <= max && isdigit((unsigned char)*s));
    if (v < min || v > max)
        return NULL;
    *out = v;
    return s;
}

/* The longest of the full and the three-letter names that matches. */
static const char *st_name(const char *s, const char *const *names, int n,
                           int *out)
{
    const char *best = NULL;
    for (int i = 0; i < n; i++) {
        size_t full = strlen(names[i]);
        if (!strncasecmp(s, names[i], full)) {
            if (!best || s + full > best) {
                best = s + full;
                *out = i;
            }
        } else if (!strncasecmp(s, names[i], 3) && (!best || s + 3 > best)) {
            best = s + 3;
            *out = i;
        }
    }
    return best;
}

static int st_leap(const struct tm *tm)
{
    return is_leap(1900L + tm->tm_year);
}

static void st_wday_of(struct tm *tm)
{
    long y = 1900L + tm->tm_year;
    long days = days_from_civil(y, (unsigned)tm->tm_mon + 1, 1) +
                tm->tm_mday - 1;
    tm->tm_wday = (int)(((days + 4) % 7 + 7) % 7);
}

static void st_mon_mday_of(struct tm *tm, const struct st_state *st)
{
    const unsigned short *c = st_yday[st_leap(tm)];
    int m = 0;
    /* (int): where int is 16 bits an unsigned short would promote to
     * unsigned, and week 0's negative yday compare as huge */
    while (m < 11 && (int)c[m + 1] <= tm->tm_yday)  /* day 366: Dec 32 */
        m++;
    if (!st->have_mon)
        tm->tm_mon = m;
    if (!st->have_mday)
        tm->tm_mday = tm->tm_yday - (int)c[m] + 1;
}

static const char *st_parse(const char *s, const char *fmt, struct tm *tm,
                            struct st_state *st);

static const char *st_nested(const char *s, const char *fmt, struct tm *tm,
                             struct st_state *st)
{
    struct tm copy = *tm;
    struct st_state inner = *st;
    if (!(s = st_parse(s, fmt, &copy, &inner)))
        return NULL;
    *tm = copy;
    *st = inner;
    return s;
}

static const char *st_parse(const char *s, const char *fmt, struct tm *tm,
                            struct st_state *st)
{
    int v;
    while (*fmt) {
        if (isspace((unsigned char)*fmt)) {
            while (isspace((unsigned char)*s))
                s++;
            fmt++;
            continue;
        }
        if (*fmt != '%') {
            if (*s++ != *fmt++)
                return NULL;
            continue;
        }
        fmt++;
        /* strftime's flags and widths mean nothing here */
        while (*fmt == '-' || *fmt == '_' || *fmt == '0' || *fmt == '^' ||
               *fmt == '#')
            fmt++;
        while (isdigit((unsigned char)*fmt))
            fmt++;
        if (*fmt == 'E' || *fmt == 'O') {     /* the "C" locale has no alt */
            if (!*++fmt || !strchr(*(fmt - 1) == 'E' ? "cCxXyY"
                                                     : "bBhdeHImMSUVWwy",
                                   *fmt))
                return NULL;
        }
        switch (*fmt++) {
        case '%':
            if (*s++ != '%')
                return NULL;
            break;
        case 'a': case 'A':
            if (!(s = st_name(s, st_wday, 7, &tm->tm_wday)))
                return NULL;
            st->have_wday = 1;
            break;
        case 'b': case 'B': case 'h':
            if (!(s = st_name(s, st_mon, 12, &tm->tm_mon)))
                return NULL;
            st->have_mon = st->want_xday = 1;
            break;
        case 'c':
            if (!(s = st_nested(s, "%a %b %e %H:%M:%S %Y", tm, st)))
                return NULL;
            st->want_xday = 1;
            break;
        case 'C':
            if (!(s = st_num(s, 0, 99, 2, &st->century)))
                return NULL;
            st->want_xday = 1;
            break;
        case 'd': case 'e':
            if (!(s = st_num(s, 1, 31, 2, &tm->tm_mday)))
                return NULL;
            st->have_mday = st->want_xday = 1;
            break;
        case 'D': case 'x':
            if (!(s = st_nested(s, "%m/%d/%y", tm, st)))
                return NULL;
            st->want_xday = 1;
            break;
        case 'F':
            if (!(s = st_nested(s, "%Y-%m-%d", tm, st)))
                return NULL;
            st->want_xday = 1;
            break;
        case 'g':                    /* an ISO year alone names no date */
            if (!(s = st_num(s, 0, 99, 2, &v)))
                return NULL;
            break;
        case 'G':
            if (!isdigit((unsigned char)*s))
                return NULL;
            while (isdigit((unsigned char)*s))
                s++;
            break;
        case 'H': case 'k':
            if (!(s = st_num(s, 0, 23, 2, &tm->tm_hour)))
                return NULL;
            st->have_I = 0;
            break;
        case 'I': case 'l':
            if (!(s = st_num(s, 1, 12, 2, &v)))
                return NULL;
            tm->tm_hour = v % 12;
            st->have_I = 1;
            break;
        case 'j':
            if (!(s = st_num(s, 1, 366, 3, &v)))
                return NULL;
            tm->tm_yday = v - 1;
            st->have_yday = 1;
            break;
        case 'm':
            if (!(s = st_num(s, 1, 12, 2, &v)))
                return NULL;
            tm->tm_mon = v - 1;
            st->have_mon = st->want_xday = 1;
            break;
        case 'M':
            if (!(s = st_num(s, 0, 59, 2, &tm->tm_min)))
                return NULL;
            break;
        case 'n': case 't':
            while (isspace((unsigned char)*s))
                s++;
            break;
        case 'p':
            if (!strncasecmp(s, "AM", 2))
                st->is_pm = 0;
            else if (!strncasecmp(s, "PM", 2))
                st->is_pm = 1;
            else
                return NULL;
            s += 2;
            break;
        case 'r':
            if (!(s = st_nested(s, "%I:%M:%S %p", tm, st)))
                return NULL;
            break;
        case 'R':
            if (!(s = st_nested(s, "%H:%M", tm, st)))
                return NULL;
            break;
        case 's': {
            /* seconds since the Epoch, in local time; no sign, as glibc */
            if (!isdigit((unsigned char)*s))
                return NULL;
            unsigned long long t = 0;
            while (isdigit((unsigned char)*s))
                t = t * 10 + (unsigned)(*s++ - '0');
            time_t tt = (time_t)t;
            if (!localtime_r(&tt, tm))
                return NULL;
            break;
        }
        case 'S':                      /* 60 and 61: leap seconds, as C89 */
            if (!(s = st_num(s, 0, 61, 2, &tm->tm_sec)))
                return NULL;
            break;
        case 'T': case 'X':
            if (!(s = st_nested(s, "%H:%M:%S", tm, st)))
                return NULL;
            break;
        case 'u':
            if (!(s = st_num(s, 1, 7, 1, &v)))
                return NULL;
            tm->tm_wday = v % 7;
            st->have_wday = 1;
            break;
        case 'U': case 'W':
            if (!(s = st_num(s, 0, 53, 2, &st->week_no)))
                return NULL;
            st->have_uweek = fmt[-1] == 'U';
            st->have_wweek = fmt[-1] == 'W';
            break;
        case 'V':                   /* an ISO week alone names no date */
            if (!(s = st_num(s, 0, 53, 2, &v)))
                return NULL;
            break;
        case 'w':
            if (!(s = st_num(s, 0, 6, 1, &tm->tm_wday)))
                return NULL;
            st->have_wday = 1;
            break;
        case 'y':
            if (!(s = st_num(s, 0, 99, 2, &v)))
                return NULL;
            tm->tm_year = v >= 69 ? v : v + 100;
            st->want_century = st->want_xday = 1;
            break;
        case 'Y':
            if (!(s = st_num(s, 0, 9999, 4, &v)))
                return NULL;
            tm->tm_year = v - 1900;
            st->want_century = 0;
            st->want_xday = 1;
            break;
        case 'z': {
            /* Z, or a sign and hh, hhmm or hh:mm */
            while (isspace((unsigned char)*s))
                s++;
            if (*s == 'Z') {
                s++;
                tm->tm_gmtoff = 0;
                break;
            }
            if (*s != '+' && *s != '-')
                return NULL;
            int neg = *s++ == '-', n = 0;
            v = 0;
            while (n < 4 && isdigit((unsigned char)*s)) {
                v = v * 10 + (*s++ - '0');
                if (++n == 2 && *s == ':' && isdigit((unsigned char)s[1]))
                    s++;
            }
            if (n == 2)
                v *= 100;
            else if (n != 4 || v % 100 >= 60)
                return NULL;
            tm->tm_gmtoff = (v / 100) * 3600L + (v % 100) * 60L;
            if (neg)
                tm->tm_gmtoff = -tm->tm_gmtoff;
            break;
        }
        case 'Z':                     /* a name: passed over, not read */
            while (isspace((unsigned char)*s))
                s++;
            while (*s && !isspace((unsigned char)*s))
                s++;
            break;
        default:
            return NULL;
        }
    }
    return s;
}

char *strptime(const char *restrict s, const char *restrict fmt,
               struct tm *restrict tm)
{
    struct st_state st;
    memset(&st, 0, sizeof st);
    st.century = -1;
    if (!(s = st_parse(s, fmt, tm, &st)))
        return NULL;

    if (st.have_I && st.is_pm)
        tm->tm_hour += 12;
    if (st.century != -1)
        tm->tm_year = (st.want_century ? tm->tm_year % 100 : 0) +
                      (st.century - 19) * 100;
    if (st.want_xday && !st.have_wday) {
        if (!(st.have_mon && st.have_mday) && st.have_yday) {
            st_mon_mday_of(tm, &st);
            st.have_mon = st.have_mday = 1;
        }
        if ((unsigned)tm->tm_mon <= 11 && tm->tm_year >= -1900)
            st_wday_of(tm);
    }
    if (st.want_xday && !st.have_yday &&
        (st.have_mon || (unsigned)tm->tm_mon <= 11))
        tm->tm_yday = (int)st_yday[st_leap(tm)][tm->tm_mon] + tm->tm_mday - 1;
    if ((st.have_uweek || st.have_wweek) && st.have_wday) {
        /* the weekday of the year's first day places the week */
        int wday = tm->tm_wday, mday = tm->tm_mday, mon = tm->tm_mon;
        int off = st.have_wweek;
        tm->tm_mday = 1;
        tm->tm_mon = 0;
        st_wday_of(tm);
        if (st.have_mday)
            tm->tm_mday = mday;
        if (st.have_mon)
            tm->tm_mon = mon;
        if (!st.have_yday)
            tm->tm_yday = (7 - (tm->tm_wday - off)) % 7 +
                          (st.week_no - 1) * 7 + (wday - off + 7) % 7;
        if (!st.have_mday || !st.have_mon)
            st_mon_mday_of(tm, &st);
        tm->tm_wday = wday;
    }
    return (char *)s;
}
