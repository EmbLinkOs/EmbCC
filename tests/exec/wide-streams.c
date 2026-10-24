/* Wide streams: fputwc, fputws, fwprintf out; fgetwc, fgetws, ungetwc,
 * fwscanf back; fwide's orientation; an encoding error in either direction;
 * and a byte fscanf that gives back the character it read ahead -- "12x"
 * through %d left nothing for the next fgetc.
 *
 * On a temporary file, so asked where tmpfile opens -- tests/golden/
 * linux-libc-exec.sh, on Linux, requires it (EMBCC_TEST_HAS_FS) -- and of
 * lib/libc, found by a weak reference to a function each of its backends
 * defines (Apple's linker refuses one nothing defines, so a Darwin host is
 * not asked). */
// expect-exit: 42
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#if !defined(__APPLE__)
extern long __os_write(int fd, const void *buf, size_t n)
    __attribute__((weak));
FILE *tmpfile(void) __attribute__((weak));
#endif

int main(void)
{
#if defined(__APPLE__)
    return 42;
#else
    if (!&__os_write)
        return 42;
    FILE *f = tmpfile();
#ifdef EMBCC_TEST_HAS_FS
    if (!f)
        return 1;
#endif
    if (!f)
        return 42;

    if (fwide(f, 0) != 0)
        return 2;                       /* no orientation yet */
    if (fputwc(L'h', f) != L'h' || fwide(f, 0) <= 0 || fwide(f, -1) <= 0)
        return 3;                       /* wide, and it stays wide */
    if (fputws(L"\x00e9 \x20ac\n", f) < 0 ||
        fwprintf(f, L"%d %ls %s|\n", 42, L"\x00e9t\x00e9", "\xe2\x82\xac") != 10)
        return 4;
    rewind(f);
    if (fgetwc(f) != L'h' || fgetwc(f) != 0xE9)
        return 5;
    if (ungetwc(0x20AC, f) != 0x20AC || fgetwc(f) != 0x20AC)
        return 6;                       /* one wide character of pushback */
    wchar_t line[16];
    if (!fgetws(line, 16, f) || wcscmp(line, L" \x20ac\n") != 0)
        return 7;
    int n = 0;
    wchar_t w[8];
    char s[8];
    if (fwscanf(f, L"%d %ls %[^|]", &n, w, s) != 3 || n != 42 ||
        wcscmp(w, L"\x00e9t\x00e9") != 0 || strcmp(s, "\xe2\x82\xac") != 0)
        return 8;
    if (fgetwc(f) != L'|')
        return 9;                       /* the scan gave back what ended it */
    if (fgetwc(f) != L'\n' || fgetwc(f) != WEOF || !feof(f))
        return 10;
    static const wchar_t lone[] = { 'a', 0xDFFF, 0 };
    clearerr(f);
    if (fputws(lone, f) != EOF || !ferror(f))
        return 11;                      /* no UTF-8 for a lone surrogate */
    fclose(f);

    /* bytes that are not UTF-8, read as wide characters */
    FILE *g = tmpfile();
    if (!g || fputs("ok\xff", g) < 0)
        return 12;
    rewind(g);
    if (fgetwc(g) != L'o' || fgetwc(g) != L'k' || fgetwc(g) != WEOF ||
        !ferror(g))
        return 13;
    fclose(g);

    /* a byte stream's scan gives back the character it read ahead */
    FILE *h = tmpfile();
    if (!h || fputs("12x 7;", h) < 0)
        return 14;
    rewind(h);
    int a = 0, b = 0;
    if (fscanf(h, "%d", &a) != 1 || a != 12 || fgetc(h) != 'x')
        return 15;
    if (fscanf(h, "%d", &b) != 1 || b != 7 || fgetc(h) != ';')
        return 16;
    if (fwide(h, 0) >= 0)
        return 17;                      /* byte-oriented now */
    fclose(h);
    return 42;
#endif
}
