/* The wide printfs (swprintf, vswprintf) and what changed in the byte ones
 * with them: %lc and %ls are wide characters, converted, and %n stores into
 * the type its length modifier names. And wcsftime.
 *
 * Asked of lib/libc, found by a weak reference to a function each of its
 * backends defines, and not of the newlib the x86-64 QEMU harness links,
 * whose byte printf stops a %ls of a four-byte wchar_t string after its
 * first character. A Darwin host's libc is asked the C locale's part --
 * ASCII, the same in every library (its linker refuses a weak reference
 * nothing defines, so it is never asked what takes UTF-8: a multibyte %s
 * in a wide printf, a non-ASCII %ls in a byte one, an encoding error). */
// expect-exit: 42
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <wchar.h>

#if !defined(__APPLE__)
extern long __os_write(int fd, const void *buf, size_t n)
    __attribute__((weak));
#endif

static int vsw(wchar_t *b, size_t n, const wchar_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vswprintf(b, n, fmt, ap);
    va_end(ap);
    return r;
}

static int c_locale(void)
{
    wchar_t b[64];
    int n = -1;

    if (swprintf(b, 64, L"%d|%5.2f|%-4s|%x|%ls%n", -42, 3.14159, "ab",
                 255u, L"wide", &n) != 22 ||
        wcscmp(b, L"-42| 3.14|ab  |ff|wide") != 0 || n != 22)
        return 1;
    if (swprintf(b, 64, L"[%3lc][%c][%.2ls][%5ls]", (wint_t)L'w', 'n',
                 L"xyz", L"ab") != 19 ||
        wcscmp(b, L"[  w][n][xy][   ab]") != 0)
        return 2;
    if (swprintf(b, 64, L"%%%s%%", "lit") != 5 || wcscmp(b, L"%lit%") != 0)
        return 3;
    /* a result that does not fit is a failure, unlike snprintf's */
    if (swprintf(b, 4, L"%s", "toolong") >= 0 || swprintf(b, 4, L"abcd") >= 0)
        return 4;
    if (swprintf(b, 4, L"abc") != 3 || wcscmp(b, L"abc") != 0)
        return 5;
    if (vsw(b, 64, L"%ld+%lld=%zu", 1L, 2LL, (size_t)3) != 5 ||
        wcscmp(b, L"1+2=3") != 0)
        return 6;

    /* the byte printfs' %lc, %ls and %n */
    char c[64];
    if (snprintf(c, sizeof c, "%ls|%lc|%3.2ls|", L"wide", (wint_t)L'c',
                 L"abc") != 11 || strcmp(c, "wide|c| ab|") != 0)
        return 7;
    signed char hh = 0x55;
    short h = 0;
    long l = 0;
    long long ll = 0;
    intmax_t j = 0;
    size_t z = 0;
    struct { signed char v; char guard[7]; } g = { 0, "guard!" };
    snprintf(c, sizeof c, "abc%hhn%hn%ln%lln%jn%zn%hhn", &hh, &h, &l, &ll,
             &j, &z, &g.v);
    if (hh != 3 || h != 3 || l != 3 || ll != 3 || j != 3 || z != 3 ||
        g.v != 3 || strcmp(g.guard, "guard!") != 0)
        return 8;                       /* %hhn wrote an int */

    struct tm tm;
    memset(&tm, 0, sizeof tm);
    tm.tm_year = 126;
    tm.tm_mon = 9;
    tm.tm_mday = 24;
    tm.tm_hour = 7;
    tm.tm_min = 5;
    tm.tm_wday = 6;
    if (wcsftime(b, 64, L"%Y-%m-%d %H:%M %a %%", &tm) != 22 ||
        wcscmp(b, L"2026-10-24 07:05 Sat %") != 0)
        return 9;
    if (wcsftime(b, 5, L"%Y-%m", &tm) != 0)
        return 10;                      /* does not fit: 0 */
    return 0;
}

static int utf8(void)
{
    wchar_t b[64];
    /* a multibyte %s is wide characters, counted as such by width and
     * precision */
    if (swprintf(b, 64, L"[%5s][%.2s]", "\xc3\xa9t\xc3\xa9",
                 "\xe2\x82\xac\xe2\x82\xac\xe2\x82\xac") != 11 ||
        wcscmp(b, L"[  \x00e9t\x00e9][\x20ac\x20ac]") != 0)
        return 20;
    if (swprintf(b, 64, L"%lc%ls", (wint_t)0x00E9, L"\x20ac") != 2 ||
        b[0] != 0xE9 || b[1] != 0x20AC)
        return 21;
    if (swprintf(b, 64, L"%s", "a\xff") >= 0)
        return 22;                      /* not UTF-8: an encoding error */

    char c[64];
    /* a byte %ls is the bytes, and a precision never cuts a character */
    if (snprintf(c, sizeof c, "%ls|%.4ls|%lc", L"\x00e9t\x20ac",
                 L"\x20ac\x20ac", (wint_t)0x00E9) != 13 ||
        strcmp(c, "\xc3\xa9t\xe2\x82\xac|\xe2\x82\xac|\xc3\xa9") != 0)
        return 23;
    static const wchar_t lone[] = { 'a', 0xD800, 0 };
    if (snprintf(c, sizeof c, "%ls", lone) >= 0)
        return 24;                      /* a lone surrogate has no bytes */
    return 0;
}

int main(void)
{
#if !defined(__APPLE__)
    if (!&__os_write)
        return 42;
#endif
    int r = c_locale();
    if (r)
        return r;
#if defined(__APPLE__)
    return 42;
#endif
    r = utf8();
    if (r)
        return r;
    return 42;
}
