/* The wide scanf (swscanf) and what changed in the byte one with it: %lc,
 * %ls and %l[ store wide characters, %j stores a whole intmax_t, and a
 * field the width ends gives nothing back -- "%3s" on "abcdef" left "cdef"
 * for the next, its last character read twice.
 *
 * Asked of lib/libc, found by a weak reference to a function each of its
 * backends defines, and not of the newlib the x86-64 QEMU harness links,
 * whose swscanf's %n counts 15 characters of the first input where there
 * are 18. A Darwin host's libc is asked the C locale's part (its linker
 * refuses a weak reference nothing defines, so it is never asked the
 * UTF-8 one). */
// expect-exit: 42
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#if !defined(__APPLE__)
extern long __os_write(int fd, const void *buf, size_t n)
    __attribute__((weak));
#endif

static int c_locale(void)
{
    int d = 0, n = 0;
    double x = 0;
    wchar_t ws[16], wc[4];
    char s[16], c2[4] = "###";

    if (swscanf(L"  -17 2.5 word ab%z", L"%d %lf %ls %2c%%%n", &d, &x, ws,
                c2, &n) != 4 ||
        d != -17 || x != 2.5 || wcscmp(ws, L"word") != 0 ||
        memcmp(c2, "ab#", 3) != 0 || n != 18)
        return 1;
    if (swscanf(L"hello world", L"%s %3lc", s, wc) != 2 ||
        strcmp(s, "hello") != 0 || wmemcmp(wc, L"wor", 3) != 0)
        return 2;
    if (swscanf(L"abc123def", L"%[a-c]%d%l[^x]", s, &d, ws) != 3 ||
        strcmp(s, "abc") != 0 || d != 123 || wcscmp(ws, L"def") != 0)
        return 3;
    if (swscanf(L"x", L"%d", &d) != 0 || swscanf(L"", L"%d", &d) != EOF)
        return 4;                       /* matching failure; input failure */

    /* the byte scanf's: wide stores, the whole intmax_t, the width */
    char a[8], b[8];
    if (sscanf("abcdef", "%3s%s", a, b) != 2 || strcmp(a, "abc") != 0 ||
        strcmp(b, "def") != 0)
        return 5;
    if (sscanf("wide x yz]q", "%ls %lc %l[^]]", ws, wc, ws + 8) != 3 ||
        wcscmp(ws, L"wide") != 0 || wc[0] != L'x' || wcscmp(ws + 8, L"yz") != 0)
        return 6;
    intmax_t j = 0;
    ptrdiff_t t = 0;
    if (sscanf("-9223372036854775807 -5", "%jd %td", &j, &t) != 2 ||
        j != -INTMAX_C(9223372036854775807) || t != -5)
        return 7;
    return 0;
}

static int utf8(void)
{
    wchar_t ws[16];
    char s[16];
    /* a byte scanf's %ls decodes; a wide one's %s encodes */
    if (sscanf("\xc3\xa9t\xc3\xa9 \xe2\x82\xac", "%ls %ls", ws, ws + 8) != 2 ||
        wcscmp(ws, L"\x00e9t\x00e9") != 0 || wcscmp(ws + 8, L"\x20ac") != 0)
        return 20;
    if (swscanf(L"\x00e9t\x00e9 x", L"%s", s) != 1 ||
        strcmp(s, "\xc3\xa9t\xc3\xa9") != 0)
        return 21;
    /* a width counts the input's characters: bytes here, and a character
     * the width cuts in two is not one */
    if (sscanf("\xc3\xa9\xc3\xa9", "%3ls", ws) == 1)
        return 22;
    if (sscanf("a\xff", "%ls", ws) == 1)
        return 23;                      /* not UTF-8 */
    if (swscanf(L"\x20ac\x20ac", L"%l[\x20ac]", ws) != 1 ||
        wcscmp(ws, L"\x20ac\x20ac") != 0)
        return 24;                      /* a set of wide characters */
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
