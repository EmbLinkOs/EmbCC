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

/* The internal state, which is what the opaque mbstate_t really is.
 * Laid out so a zeroed mbstate_t is the initial state, as the standard
 * requires. */
struct state {
    unsigned int wch;
    unsigned char want;
    unsigned char have;
    unsigned int lowbound;
};

static struct state *st(mbstate_t *ps)
{
    static mbstate_t internal;
    return (struct state *)(ps ? ps : &internal);
}

int mbsinit(const mbstate_t *ps)
{
    return !ps || ((const struct state *)ps)->want == 0;
}

static int is_surrogate(unsigned int c) { return c >= 0xD800 && c <= 0xDFFF; }
static int too_big(unsigned int c) { return c > 0x10FFFF; }

size_t mbrtowc(wchar_t *__restrict pwc, const char *__restrict s, size_t n,
               mbstate_t *__restrict ps)
{
    struct state *k = st(ps);

    /* A null s means "reset to the initial state", and the standard
     * spells it as mbrtowc(0, "", 1, ps). */
    if (!s) {
        memset(k, 0, sizeof *k);
        return 0;
    }
    if (n == 0)
        return (size_t)-2;        /* incomplete: nothing to look at */

    size_t used = 0;
    while (used < n) {
        unsigned char b = (unsigned char)s[used++];

        if (k->want == 0) {
            if (b < 0x80) {
                if (pwc)
                    *pwc = (wchar_t)b;
                return b ? used : 0;   /* a NUL converts and returns 0 */
            }
            /* A continuation byte with nothing to continue, or one of
             * the two bytes that can never begin a sequence. */
            if (b < 0xC2 || b > 0xF4) {
                errno = EILSEQ;
                return (size_t)-1;
            }
            if (b < 0xE0) { k->want = 1; k->wch = b & 0x1Fu; k->lowbound = 0x80; }
            else if (b < 0xF0) { k->want = 2; k->wch = b & 0x0Fu; k->lowbound = 0x800; }
            else { k->want = 3; k->wch = b & 0x07u; k->lowbound = 0x10000; }
            k->have = 1;
            continue;
        }

        if ((b & 0xC0u) != 0x80u) {
            /* A byte that is not a continuation, in the middle of a
             * sequence. The sequence is broken; the state is cleared so
             * the caller can resynchronise rather than stay wedged. */
            memset(k, 0, sizeof *k);
            errno = EILSEQ;
            return (size_t)-1;
        }
        k->wch = (k->wch << 6) | (b & 0x3Fu);
        k->have++;
        if (--k->want)
            continue;

        unsigned int c = k->wch;
        unsigned int low = k->lowbound;
        memset(k, 0, sizeof *k);
        if (c < low || is_surrogate(c) || too_big(c)) {
            errno = EILSEQ;
            return (size_t)-1;
        }
        if (pwc)
            *pwc = (wchar_t)c;
        return c ? used : 0;
    }
    /* Ran out of input mid-character: -2, and the state remembers where
     * we were so the next call continues rather than restarts. */
    return (size_t)-2;
}

size_t mbrlen(const char *__restrict s, size_t n, mbstate_t *__restrict ps)
{
    return mbrtowc(NULL, s, n, ps);
}

size_t wcrtomb(char *__restrict s, wchar_t wc, mbstate_t *__restrict ps)
{
    struct state *k = st(ps);
    memset(k, 0, sizeof *k);      /* UTF-8 encoding is stateless */

    /* A null s asks for the length of the shift sequence to return to
     * the initial state -- one, for the NUL, in a stateless encoding. */
    char buf[4];
    if (!s) {
        s = buf;
        wc = 0;
    }
    unsigned int c = (unsigned int)wc;
    if (is_surrogate(c) || too_big(c)) {
        errno = EILSEQ;
        return (size_t)-1;
    }
    if (c < 0x80) {
        s[0] = (char)c;
        return 1;
    }
    if (c < 0x800) {
        s[0] = (char)(0xC0u | (c >> 6));
        s[1] = (char)(0x80u | (c & 0x3Fu));
        return 2;
    }
    if (c < 0x10000) {
        s[0] = (char)(0xE0u | (c >> 12));
        s[1] = (char)(0x80u | ((c >> 6) & 0x3Fu));
        s[2] = (char)(0x80u | (c & 0x3Fu));
        return 3;
    }
    s[0] = (char)(0xF0u | (c >> 18));
    s[1] = (char)(0x80u | ((c >> 12) & 0x3Fu));
    s[2] = (char)(0x80u | ((c >> 6) & 0x3Fu));
    s[3] = (char)(0x80u | (c & 0x3Fu));
    return 4;
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
