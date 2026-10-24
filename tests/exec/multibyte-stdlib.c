/* <stdlib.h>'s multibyte conversions without a state -- mblen, mbtowc,
 * wctomb, mbstowcs, wcstombs -- and <wchar.h>'s btowc and wctob, in the C
 * locale, whose characters are the single bytes, which every library
 * agrees on: lib/libc's multibyte encoding is UTF-8, the harnesses' newlib
 * and a host's libc start in "C", and ASCII is the same in each. What
 * only UTF-8 gives -- é is two bytes, a broken sequence is -1 -- and
 * POSIX's null destinations, which newlib does not count, are asked of
 * lib/libc, found by a weak reference to a function each of its
 * backends defines (Apple's linker refuses one nothing defines, so a
 * Darwin host is not asked). And wcstoimax, which <inttypes.h> has
 * alongside them. */
// expect-exit: 42
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#if !defined(__APPLE__)
extern long __os_write(int fd, const void *buf, size_t n)
    __attribute__((weak));
#endif

static int ascii(void)
{
    wchar_t wc = 0;
    char b[8];

    if (mblen(NULL, 0) != 0 || mbtowc(NULL, NULL, 0) != 0 ||
        wctomb(NULL, L'a') != 0)
        return 1;                       /* no shift states */
    if (mblen("a", 1) != 1 || mblen("", 1) != 0 || mblen("ab", 0) != -1)
        return 2;
    if (mbtowc(&wc, "Q", 1) != 1 || wc != L'Q')
        return 3;
    if (mbtowc(&wc, "", 1) != 0 || wc != 0)
        return 4;
    if (wctomb(b, L'z') != 1 || b[0] != 'z')
        return 5;
    if (wctomb(b, 0) != 1 || b[0] != 0)
        return 6;

    wchar_t w[8];
    if (mbstowcs(w, "abc", 8) != 3 || w[0] != L'a' || w[2] != L'c' ||
        w[3] != 0)
        return 7;
    wmemset(w, L'#', 8);
    if (mbstowcs(w, "abcd", 2) != 2 || w[1] != L'b' || w[2] != L'#')
        return 9;                       /* n stops it, unterminated */
    memset(b, '#', sizeof b);
    if (wcstombs(b, L"xyz", sizeof b) != 3 || strcmp(b, "xyz") != 0)
        return 10;

    if (btowc('A') != L'A' || btowc(EOF) != WEOF || btowc(0) != 0)
        return 12;
    if (wctob(L'A') != 'A' || wctob(WEOF) != EOF)
        return 13;
    return 0;
}

static int ours(void)
{
    wchar_t wc = 0;
    char b[8];

    /* POSIX's: a null destination asks for the length */
    if (mbstowcs(NULL, "abcd", 0) != 4 || wcstombs(NULL, L"hello", 0) != 5)
        return 19;

    if (mblen("\xc3\xa9", 2) != 2 || mblen("\xc3\xa9", 1) != -1)
        return 20;                      /* cut short is invalid here */
    if (mblen("\x80", 1) != -1 || mblen("\xc0\x80", 2) != -1)
        return 21;                      /* a stray continuation; overlong */
    /* after a failure the next call starts afresh */
    if (mbtowc(&wc, "\xe2\x82", 2) != -1 || mbtowc(&wc, "A", 1) != 1 ||
        wc != L'A')
        return 22;
    /* past the Basic Multilingual Plane: not a character where wchar_t is
     * 16 bits (Xtensa), rather than one cut down to fit */
    if (WCHAR_MAX >= 0x10FFFF ?
            mbtowc(&wc, "\xf0\x9f\x98\x80", 4) != 4 || wc != 0x1F600 :
            mbtowc(&wc, "\xf0\x9f\x98\x80", 4) != -1)
        return 23;
    if (wctomb(b, 0x20AC) != 3 || memcmp(b, "\xe2\x82\xac", 3) != 0)
        return 24;
    if (wctomb(b, 0xD800) != -1 ||
        (WCHAR_MAX > 0x10FFFF && wctomb(b, (wchar_t)0x110000) != -1))
        return 25;                      /* a surrogate; past U+10FFFF */

    wchar_t w[8];
    if (mbstowcs(w, "a\xc3\xa9z", 8) != 3 || w[1] != 0xE9 || w[2] != L'z')
        return 26;
    if (mbstowcs(w, "a\xffz", 8) != (size_t)-1)
        return 27;
    static const wchar_t euro[] = { 'x', 0x20AC, 0 };
    memset(b, '#', sizeof b);
    if (wcstombs(b, euro, sizeof b) != 4 || strcmp(b, "x\xe2\x82\xac") != 0)
        return 28;
    memset(b, '#', sizeof b);
    if (wcstombs(b, euro, 3) != 1 || b[1] != '#')
        return 29;                      /* a character whole or not at all */
    static const wchar_t bad[] = { 'x', 0xDFFF, 0 };
    if (wcstombs(b, bad, sizeof b) != (size_t)-1)
        return 30;

    if (btowc(0xE9) != WEOF || btowc(0x80) != WEOF)
        return 31;                      /* no byte past 0x7F is a character */
    if (wctob(0xE9) != EOF || wctob(0x20AC) != EOF)
        return 32;
    return 0;
}

static int wide_imax(void)
{
    wchar_t *end;
    if (wcstoimax(L"  -9223372036854775807x", &end, 10) !=
            -INTMAX_C(9223372036854775807) || *end != L'x')
        return 40;
    if (wcstoumax(L"0xffffffffffffffff", &end, 16) != UINTMAX_MAX || *end)
        return 41;
    if (wcstoimax(L"777", NULL, 8) != 511)
        return 43;
    return 0;
}

int main(void)
{
    int r = ascii();
    if (r)
        return r;
#if !defined(__APPLE__)
    if (&__os_write) {
        r = ours();
        if (r)
            return r;
    }
#endif
    r = wide_imax();
    if (r)
        return r;
    return 42;
}
