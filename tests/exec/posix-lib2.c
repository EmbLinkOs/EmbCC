/* The second round of POSIX, XSI and GNU library functions: locale
 * objects and the _l functions, wide-string extras and wcwidth, string
 * and signal names, the reentrant generators, ecvt and radix 64,
 * strfmon, the Bessel functions and C23's narrowing arithmetic, total
 * order and payloads, gettext and dlfcn -- and, where there is a
 * filesystem, the password, shadow and shell databases, utmpx, fts and
 * getdate. Every expected value is glibc 2.36's for the same call (the
 * program ran there as a transcript, line for line), except where a
 * comment says otherwise. */
// expect-exit: 42
// needs-libc: locale objects, <fts.h>, <utmpx.h> and the rest, from lib/libc
#define _GNU_SOURCE 1
#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <fts.h>
#include <libintl.h>
#include <link.h>
#include <locale.h>
#include <math.h>
#include <monetary.h>
#include <pwd.h>
#include <shadow.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <utmpx.h>
#include <wchar.h>
#include <sys/stat.h>

static int g_line;
#define CHECK(c) do { if (!(c)) { g_line = __LINE__; return 1; } } while (0)

static int near(double a, double b)
{
    return fabs(a - b) <= 1e-12 * fabs(b) + 1e-300;
}

static int locales(void)
{
    locale_t l = newlocale(LC_ALL_MASK, "C", 0);
    CHECK(l);
    errno = 0;
    CHECK(!newlocale(LC_ALL_MASK, "xx_YY", 0) && errno == ENOENT);
    CHECK(isalpha_l('a', l) && !isalpha_l('1', l) && toupper_l('q', l) == 'Q');
    CHECK(strtol_l("-17", 0, 10, l) == -17 && strtod_l("2.5x", 0, l) == 2.5);
    CHECK(uselocale(l) == LC_GLOBAL_LOCALE);
    uselocale(LC_GLOBAL_LOCALE);
    freelocale(l);
    CHECK(setlocale(LC_CTYPE, "C.UTF-8") && setlocale(LC_ALL, "POSIX"));
    return 0;
}

static int wide(void)
{
    wchar_t buf[32];
    CHECK(wcscasecmp(L"HeLLo", L"hello") == 0 && wcsncasecmp(L"abX", L"ABy", 2) == 0);
    CHECK(wcpcpy(buf, L"abc") == buf + 3);
    CHECK(wcpncpy(buf, L"ab", 5) == buf + 2 && buf[4] == 0);
    wchar_t *d = wcsdup(L"dup");
    CHECK(d && !wcscmp(d, L"dup"));
    free(d);
    /* a character the byte bound cuts is kept in the state */
    setlocale(LC_CTYPE, "C.UTF-8");
    const char *src = "a\xc3\xa9z", *sp = src;
    mbstate_t st;
    memset(&st, 0, sizeof st);
    CHECK(mbsnrtowcs(buf, &sp, 2, 32, &st) == 1 && sp == src + 2 && !mbsinit(&st));
    CHECK(mbsnrtowcs(buf, &sp, 10, 32, &st) == 2 && buf[0] == 0xe9);
    const wchar_t *wp = L"x\xe9y";
    char mb[16];
    CHECK(wcsnrtombs(mb, &wp, 2, sizeof mb, 0) == 3);
    static const struct { int c, w; } ww[] = {
        { 'a', 1 }, { 0x300, 0 }, { 0x4e00, 2 }, { 0x1f600, 2 }, { 0xad, 1 },
        { 0x200b, 0 }, { 0x7f, -1 }, { 0xe000, 1 }, { 0x10ffff, -1 }, { 0xffff, -1 },
    };
    for (unsigned i = 0; i < sizeof ww / sizeof ww[0]; i++)
        CHECK(wcwidth(ww[i].c) == ww[i].w);
    CHECK(wcswidth(L"a\x4e00z", 10) == 4 && wcswidth(L"a\x7f", 10) == -1);
    return 0;
}

static int strings(void)
{
    CHECK(ffsl(0) == 0 && ffsl(0x100L) == 9 && ffsll(1LL << 40) == 41);
    CHECK(isascii(65) && !isascii(200) && toascii(200) == 72);
    char s[] = "hello";
    CHECK((char *)rawmemchr(s, 'l') == s + 2);
    memfrob(s, 5);
    CHECK(strcmp(s, "hello"));
    memfrob(s, 5);
    CHECK(!strcmp(s, "hello"));
    CHECK(!strcmp(strerrorname_np(ENOENT), "ENOENT") &&
          !strcmp(strerrorname_np(EAGAIN), "EAGAIN") &&
          !strcmp(strerrordesc_np(EPERM), "Operation not permitted"));
    CHECK(!strcmp(sigabbrev_np(SIGHUP), "HUP") && !strcmp(sigabbrev_np(SIGCHLD), "CHLD") &&
          !strcmp(sigdescr_np(SIGINT), "Interrupt"));
    return 0;
}

static int generators(void)
{
    struct random_data rd;
    static char state[64];
    memset(&rd, 0, sizeof rd);
    CHECK(initstate_r(42, state, sizeof state, &rd) == 0);
    static const int32_t want[] = { 2051258974, 339992574, 1379825892 };
    for (int i = 0; i < 3; i++) {
        int32_t r;
        random_r(&rd, &r);
        CHECK(r == want[i]);
    }
    struct drand48_data dd;
    srand48_r(7, &dd);
    long lr;
    double dr;
    lrand48_r(&dd, &lr);
    drand48_r(&dd, &dr);
    CHECK(lr == 572184555);
    if (DBL_MANT_DIG == 53)
        CHECK(dr == 0.68203523019062118);
    CHECK(a64l("./") == 64 && !strcmp(l64a(123456), ".7S"));
    CHECK(!strcmp(l64a(-1), "zzzzz1") && a64l(".7S") == 123456);
    CHECK(rpmatch("yes") == 1 && rpmatch("No") == 0 && rpmatch("?") == -1);
    CHECK(strtoq("-99", 0, 10) == -99 && arc4random_uniform(10) < 10);
    if (DBL_MANT_DIG != 53)
        return 0;
    int dp, sg;
    CHECK(!strcmp(ecvt(3.14159, 4, &dp, &sg), "3142") && dp == 1 && !sg);
    CHECK(!strcmp(fcvt(-0.0123, 3, &dp, &sg), "12") && dp == -1 && sg);
    CHECK(!strcmp(fcvt(1234.5, 0, &dp, &sg), "1234") && dp == 4);
    CHECK(!strcmp(ecvt(0.0, 3, &dp, &sg), "000") && dp == 1);
    CHECK(!strcmp(fcvt(0.0, 2, &dp, &sg), "000") && dp == 1);
    char g[32];
    CHECK(!strcmp(gcvt(1234567.0, 3, g), "1.23e+06"));
    return 0;
}

static int money(void)
{
    if (DBL_MANT_DIG != 53)
        return 0;
    static const struct { const char *fmt, *pos, *neg; } t[] = {
        { "%n", "123.46", "-7.50" },
        { "%i", "123.46", "-7.50" },
        { "%11n", "     123.46", "      -7.50" },
        { "%-11n|", "123.46     |", "-7.50      |" },
        { "%=*#6n", " ***123.46", "-*****7.50" },
        { "%(n", "123.46", "(7.50)" },
        { "%.0n", "123", "-8" },
        { "%#5.1n", "   123.5", "-    7.5" },
        { "%^n", "123.46", "-7.50" },
    };
    char m[64];
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
        CHECK(strfmon(m, sizeof m, t[i].fmt, 123.456) >= 0 && !strcmp(m, t[i].pos));
        CHECK(strfmon(m, sizeof m, t[i].fmt, -7.5) >= 0 && !strcmp(m, t[i].neg));
    }
    return 0;
}

static int maths(void)
{
    if (DBL_MANT_DIG != 53)
        return 0;
    CHECK(near(j0(1.5), 0.511827671735918) && near(j1(1.5), 0.5579365079101) &&
          near(jn(3, 2.5), 0.216600391039113));
    CHECK(near(y0(1.5), 0.382448923797759) && near(y1(1.5), -0.412308626973911) &&
          near(yn(3, 2.5), -0.756055496753671));
    double s, c;
    sincos(0.5, &s, &c);
    CHECK(s == sin(0.5) && c == cos(0.5));
    CHECK(significand(24.0) == 1.5 && scalb(3.0, 4.0) == 48 && drem(7.0, 4.0) == -1);
    CHECK(near(gamma(4.5), 2.45373657084244));
    CHECK(isinf(-INFINITY) == -1 && isinf(1.0) == 0 && finitel(1.0L));
    CHECK(llogb(1000.0) == 9 && fmaxmag(-3.0, 2.0) == -3 && fminmag(-3.0, 2.0) == 2);
    /* each rounded once: a double sum or product rounded again to float
     * would be wrong on the first two */
    CHECK(fadd(1.0, 0x1p-24 + 0x1p-60) == 1.0f + 0x1p-23f);
    CHECK(fadd(1.0, 0x1p-24) == 1.0f);
    CHECK(fmul(1.0 + 0x1p-24, 1.0 + 0x1p-24) == 1.00000012f);
    CHECK(fdiv(1.0, 3.0) == 1.0f / 3.0f && fsqrt(2.0) == sqrtf(2.0f));
    CHECK(daddl(1.0L, 0x1p-60L) == 1.0 && dmull(3.0L, 1.0L / 3.0L) == 1.0);
    double a = -0.0, b = 0.0, na = NAN, pl, cx;
    CHECK(totalorder(&a, &b) && !totalorder(&b, &a) && totalorder(&b, &na));
    CHECK(setpayload(&pl, 42.0) == 0 && isnan(pl) && getpayload(&pl) == 42);
    CHECK(getpayload(&b) == -1 && setpayload(&pl, 0.5) == 1);
    CHECK(canonicalize(&cx, &b) == 0 && cx == 0);
    CHECK(fromfp(2.5, FP_INT_TONEAREST, 32) == 2 && fromfp(2.5, FP_INT_UPWARD, 32) == 3 &&
          fromfp(-2.5, FP_INT_TONEARESTFROMZERO, 8) == -3 &&
          ufromfp(255.4, FP_INT_TOWARDZERO, 8) == 255);
    return 0;
}

static int phdrs;
static int phdr_cb(struct dl_phdr_info *i, size_t sz, void *d)
{
    (void)sz;
    (void)d;
    for (int k = 0; k < i->dlpi_phnum; k++)
        phdrs += i->dlpi_phdr[k].p_type == PT_LOAD;
    return 0;
}

static int intl_dl(void)
{
    CHECK(!strcmp(gettext("hi"), "hi") && !strcmp(ngettext("one", "many", 1), "one") &&
          !strcmp(ngettext("one", "many", 2), "many"));
    CHECK(!strcmp(textdomain(0), "messages") && !strcmp(textdomain("app"), "app"));
    CHECK(!strcmp(bindtextdomain("app", "/opt/loc"), "/opt/loc") &&
          !strcmp(bindtextdomain("app", 0), "/opt/loc"));
    CHECK(dlopen(0, RTLD_NOW));
    CHECK(!dlopen("libnosuch.so", RTLD_NOW) && dlerror() && !dlerror());
    /* glibc's program is dynamically linked and reports five objects;
     * this library links statically, so where the kernel describes the
     * program there is one */
    dl_iterate_phdr(phdr_cb, 0);
    return 0;
}

#ifdef EMBCC_TEST_HAS_FS
static void put(const char *p, const char *s)
{
    FILE *f = fopen(p, "w");
    if (f) {
        fputs(s, f);
        fclose(f);
    }
}

static int by_name(const FTSENT **a, const FTSENT **b)
{
    return strcmp((*a)->fts_name, (*b)->fts_name);
}

static int files(void)
{
    mkdir("/tmp", 0777);
    put("/tmp/pw", "root:x:0:0:root:/root:/bin/sh\n#c\nbob:x:1000:100:Bob:/home/bob:/bin/zsh\n");
    FILE *f = fopen("/tmp/pw", "r");
    struct passwd *pw = fgetpwent(f);
    CHECK(pw && !strcmp(pw->pw_name, "root") && pw->pw_uid == 0);
    pw = fgetpwent(f);
    CHECK(pw && !strcmp(pw->pw_shell, "/bin/zsh") && pw->pw_uid == 1000 && !fgetpwent(f));
    fclose(f);
    struct spwd *sp = sgetspent("bob:$6$h:19000:0:99999:7:::");
    CHECK(sp && sp->sp_lstchg == 19000 && sp->sp_max == 99999 && sp->sp_inact == -1 &&
          sp->sp_flag == ~0UL);
    setusershell();
    CHECK(!strcmp(getusershell(), "/bin/sh") && !strcmp(getusershell(), "/bin/csh") &&
          !getusershell());
    endusershell();

    /* utmpx: a second record with the same id goes after the first when
     * read from where the first was written (POSIX's and glibc's rule) */
    close(open("/tmp/ut", O_CREAT | O_TRUNC | O_RDWR, 0644));
    CHECK(utmpxname("/tmp/ut") == 0);
    struct utmpx u;
    memset(&u, 0, sizeof u);
    u.ut_type = USER_PROCESS;
    strcpy(u.ut_id, "t1");
    strcpy(u.ut_line, "tty1");
    strcpy(u.ut_user, "bob");
    setutxent();
    CHECK(pututxline(&u));
    strcpy(u.ut_user, "eve");
    CHECK(pututxline(&u));
    setutxent();
    struct utmpx key, *e;
    memset(&key, 0, sizeof key);
    strcpy(key.ut_line, "tty1");
    e = getutxline(&key);
    CHECK(e && !strcmp(e->ut_user, "bob"));
    struct stat st;
    CHECK(stat("/tmp/ut", &st) == 0 && st.st_size == 2 * (off_t)sizeof u && sizeof u == 384);
    endutxent();

    mkdir("/tmp/ft", 0755);
    mkdir("/tmp/ft/b", 0755);
    mkdir("/tmp/ft/a", 0755);
    put("/tmp/ft/a/x", "x");
    put("/tmp/ft/c", "c");
    char *roots[] = { "/tmp/ft", 0 };
    static const struct { int info, level; const char *name; } walk[] = {
        { FTS_D, 0, "ft" }, { FTS_D, 1, "a" }, { FTS_F, 2, "x" }, { FTS_DP, 1, "a" },
        { FTS_D, 1, "b" }, { FTS_DP, 1, "b" }, { FTS_F, 1, "c" }, { FTS_DP, 0, "ft" },
    };
    FTS *fts = fts_open(roots, FTS_PHYSICAL | FTS_NOCHDIR, by_name);
    CHECK(fts);
    for (unsigned i = 0; i < sizeof walk / sizeof walk[0]; i++) {
        FTSENT *fe = fts_read(fts);
        CHECK(fe && fe->fts_info == walk[i].info && fe->fts_level == walk[i].level &&
              !strcmp(fe->fts_name, walk[i].name));
    }
    CHECK(!fts_read(fts));
    fts_close(fts);

    put("/tmp/dm", "%Y-%m-%d %H:%M\n%d/%m/%Y\n");
    setenv("DATEMSK", "/tmp/dm", 1);
    setenv("TZ", "UTC", 1);
    tzset();
    struct tm *tm = getdate("2021-03-04 05:06");
    CHECK(tm && tm->tm_year == 121 && tm->tm_mon == 2 && tm->tm_mday == 4 &&
          tm->tm_hour == 5 && tm->tm_min == 6);
    tm = getdate("14/07/2020");
    CHECK(tm && tm->tm_year == 120 && tm->tm_mon == 6 && tm->tm_mday == 14);
    CHECK(!getdate("garbage") && getdate_err == 7);
    CHECK(dysize(2000) == 366 && dysize(1900) == 365);
    return 0;
}
#endif

int main(void)
{
    int r;
    if ((r = locales()) || (r = wide()) || (r = strings()) || (r = generators()) ||
        (r = money()) || (r = maths()) || (r = intl_dl()))
        return r + 100 + 0 * printf("failed at line %d\n", g_line);
#ifdef EMBCC_TEST_HAS_FS
    if (files())
        return 150 + 0 * printf("failed at line %d\n", g_line);
#endif
    return 42;
}
