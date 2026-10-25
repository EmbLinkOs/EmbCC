/* Multibyte conversion: UTF-8 in both directions, C11 §7.29.6.
 *
 * C says the multibyte encoding is locale-dependent. This library has
 * one locale and its encoding is UTF-8, which is the only choice that
 * is not arbitrary: it is the one encoding where ASCII is itself, a
 * byte's high bits say how long its character is, and no byte of a
 * multi-byte sequence can be mistaken for the start of another.
 *
 * ---- what has to be rejected ------------------------------------------
 *
 * Most of this file is refusals, and every one of them is a real attack
 * or a real bug:
 *
 *   OVERLONG forms. 0xC0 0x80 decodes arithmetically to U+0000, and a
 *   decoder that accepts it lets a NUL through a string that was
 *   checked for NULs. Every length has a smallest value it may encode
 *   and anything below it is rejected -- which is what __lowbound in
 *   the state is for.
 *
 *   SURROGATES. U+D800..U+DFFF are not characters; they exist only as
 *   UTF-16 code units. Encoded in UTF-8 they are "CESU-8", and letting
 *   them round-trip means two different byte sequences for one string.
 *
 *   Anything above U+10FFFF. Not a code point. The old five- and
 *   six-byte forms encode them and are not UTF-8.
 *
 * ---- the state --------------------------------------------------------
 *
 * A sequence may be split across calls, so the partial character lives
 * in the mbstate_t. A conversion function that ignored it would work on
 * whole strings and mangle anything read in chunks -- which is exactly
 * the case that only shows up in production.
 */
#include <wchar.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "../internal/utf8.h"

/* The internal state, which is what the opaque mbstate_t really is:
 * internal/utf8.h's decoder state, laid out so a zeroed mbstate_t is the
 * initial state, as the standard requires. The decoding and encoding
 * themselves -- what is refused, byte by byte -- are there too, shared
 * with printf, scanf and the wide streams. */
_Static_assert(sizeof(struct __utf8) <= sizeof(mbstate_t),
               "mbstate_t holds the decoder's state");

static struct __utf8 *st(mbstate_t *ps)
{
    static mbstate_t internal;
    return (struct __utf8 *)(ps ? ps : &internal);
}

int mbsinit(const mbstate_t *ps)
{
    return !ps || ((const struct __utf8 *)ps)->want == 0;
}

size_t mbrtowc(wchar_t *__restrict pwc, const char *__restrict s, size_t n,
               mbstate_t *__restrict ps)
{
    struct __utf8 *k = st(ps);

    /* A null s means "reset to the initial state", and the standard
     * spells it as mbrtowc(0, "", 1, ps). */
    if (!s) {
        memset(k, 0, sizeof *k);
        return 0;
    }
    for (size_t used = 0; used < n; ) {
        uint_least32_t c;
        int r = __utf8_step(k, (unsigned char)s[used++], &c);
        if (r == 0)
            continue;
        /* A broken sequence leaves the state initial, so the caller can
         * resynchronise rather than stay wedged. */
        if (r < 0) {
            errno = EILSEQ;
            return (size_t)-1;
        }
        if (pwc)
            *pwc = (wchar_t)c;
        return c ? used : 0;           /* a NUL converts and returns 0 */
    }
    /* Ran out of input mid-character (or had none to look at): -2, and the
     * state remembers where we were so the next call continues rather
     * than restarts. */
    return (size_t)-2;
}

size_t mbrlen(const char *__restrict s, size_t n, mbstate_t *__restrict ps)
{
    return mbrtowc(NULL, s, n, ps);
}

size_t wcrtomb(char *__restrict s, wchar_t wc, mbstate_t *__restrict ps)
{
    struct __utf8 *k = st(ps);
    memset(k, 0, sizeof *k);      /* UTF-8 encoding is stateless */

    /* A null s asks for the length of the shift sequence to return to
     * the initial state -- one, for the NUL, in a stateless encoding. */
    char buf[4];
    if (!s) {
        s = buf;
        wc = 0;
    }
    int r = __utf8_put((uint_least32_t)wc, s);
    if (r < 0) {
        errno = EILSEQ;
        return (size_t)-1;
    }
    return (size_t)r;
}

size_t mbsrtowcs(wchar_t *__restrict d, const char **__restrict s, size_t n,
                 mbstate_t *__restrict ps)
{
    const char *p = *s;
    size_t made = 0;
    for (;;) {
        wchar_t wc;
        /* Four is the most a UTF-8 character can take, so the decoder
         * is never starved by a bound the caller did not give. */
        size_t r = mbrtowc(&wc, p, 4, ps);
        if (r == (size_t)-1)
            return (size_t)-1;
        if (r == (size_t)-2) {
            /* An incomplete character at the end of the input. The
             * source string is NUL-terminated, so this means malformed
             * rather than "more to come". */
            errno = EILSEQ;
            return (size_t)-1;
        }
        if (d && made >= n)
            break;
        if (d)
            d[made] = wc;
        if (wc == 0) {
            /* The terminator is written and NOT counted, and the
             * source pointer is left null -- which is how the caller
             * tells "finished" from "ran out of room". */
            if (d)
                *s = NULL;
            return made;
        }
        made++;
        p += r;
        if (d)
            *s = p;
    }
    *s = p;
    return made;
}

size_t wcsrtombs(char *__restrict d, const wchar_t **__restrict s, size_t n,
                 mbstate_t *__restrict ps)
{
    const wchar_t *p = *s;
    size_t made = 0;
    char buf[4];
    for (;;) {
        size_t r = wcrtomb(buf, *p, ps);
        if (r == (size_t)-1)
            return (size_t)-1;
        if (d) {
            /* A character is written whole or not at all: writing half
             * of one would leave an invalid sequence in the buffer. */
            if (made + r > n)
                break;
            for (size_t i = 0; i < r; i++)
                d[made + i] = buf[i];
        }
        if (*p == 0) {
            if (d)
                *s = NULL;
            return made;           /* the NUL is written, not counted */
        }
        made += r;
        p++;
        if (d)
            *s = p;
    }
    *s = p;
    return made;
}

/* ---- one byte, C11 §7.29.6.1 ---- */

/* The single-byte characters of UTF-8 are ASCII, and no others: a byte
 * from 0x80 up is part of a longer character. */
wint_t btowc(int c)
{
    return c >= 0 && c < 0x80 ? (wint_t)c : WEOF;
}

int wctob(wint_t c)
{
    return (unsigned long)c < 0x80 ? (int)c : EOF;
}

/* ---- the forms without a state, <stdlib.h> §7.22.7 and §7.22.8 ----
 *
 * C89's conversions, before mbstate_t: each keeps a state of its own, and
 * a null s puts it back to the start and asks whether the encoding has
 * shift states. UTF-8 has none, so that answer is 0, and the state is
 * only ever mid-character between the two halves of one call -- they
 * have no "incomplete", so a character n cuts short is invalid (-1, with
 * EILSEQ as POSIX has it) and the next call starts afresh. Which makes a
 * state of their own a fresh one each time: no static, and nothing two
 * threads can share. */
int mbtowc(wchar_t *__restrict pwc, const char *__restrict s, size_t n)
{
    if (!s)
        return 0;
    mbstate_t ms;
    memset(&ms, 0, sizeof ms);
    size_t r = mbrtowc(pwc, s, n, &ms);
    if (r == (size_t)-2)
        errno = EILSEQ;
    if (r == (size_t)-1 || r == (size_t)-2)
        return -1;
    return (int)r;
}

int mblen(const char *s, size_t n)
{
    return mbtowc(NULL, s, n);
}

/* wctomb(s, 0) stores the NUL and returns 1, which wcrtomb does. */
int wctomb(char *s, wchar_t wc)
{
    if (!s)
        return 0;
    mbstate_t ms;
    memset(&ms, 0, sizeof ms);
    size_t r = wcrtomb(s, wc, &ms);
    return r == (size_t)-1 ? -1 : (int)r;
}

/* The string forms are the restartable ones from the initial state, with
 * the source pointer the caller does not get back. */
size_t mbstowcs(wchar_t *__restrict d, const char *__restrict s, size_t n)
{
    mbstate_t ms;
    memset(&ms, 0, sizeof ms);
    const char *p = s;
    return mbsrtowcs(d, &p, n, &ms);
}

size_t wcstombs(char *__restrict d, const wchar_t *__restrict s, size_t n)
{
    mbstate_t ms;
    memset(&ms, 0, sizeof ms);
    const wchar_t *p = s;
    return wcsrtombs(d, &p, n, &ms);
}
