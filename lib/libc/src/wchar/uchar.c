/* UTF-16 and UTF-32 conversion, C11 §7.28.
 *
 * The UTF-32 pair is a rename of the wchar_t one -- wchar_t is 32 bits
 * here, so char32_t and wchar_t hold the same thing. Writing them as
 * calls rather than copies says that, and keeps one decoder.
 *
 * The UTF-16 pair is where the work is, and it is the reason this
 * header exists at all. A code point above U+FFFF does not fit one
 * 16-bit unit, so it is written as a SURROGATE PAIR: a high surrogate
 * in D800..DBFF and a low one in DC00..DFFF, each carrying ten of the
 * twenty bits of (c - 0x10000).
 *
 * That makes both directions stateful in a way UTF-8 is not:
 *
 *   c16rtomb is handed ONE code unit at a time. Given a high surrogate
 *   it can emit nothing yet -- it must remember it and wait for the low
 *   one. It returns 0, which the standard defines as "no bytes written,
 *   the character is not complete".
 *
 *   mbrtoc16 decodes one code point from bytes and may owe TWO units.
 *   It returns the high surrogate and remembers the low, then returns
 *   the low on the next call WITHOUT consuming any input -- which is
 *   what the return value -3 means, and is the only way a function
 *   returning one unit at a time can report it.
 */
#include <uchar.h>
#include <errno.h>
#include <string.h>

/* The same layout as <wchar.h>'s mbstate_t, reused: a pending surrogate
 * goes in the field that otherwise holds a partly decoded character.
 * The two uses cannot overlap -- one is between bytes, the other
 * between code units -- which is why one object serves both. */
struct state {
    unsigned int wch;
    unsigned char want;
    unsigned char have;
    unsigned int lowbound;
};

static struct state *st16(mbstate_t *ps)
{
    static mbstate_t internal;
    return (struct state *)(ps ? ps : &internal);
}

size_t mbrtoc32(char32_t *__restrict pc32, const char *__restrict s, size_t n,
                mbstate_t *__restrict ps)
{
    /* char32_t and wchar_t are the same width here, so this is the same
     * decoder. On a target where they differ this is the file that
     * would change. */
    return mbrtowc((wchar_t *)pc32, s, n, ps);
}

size_t c32rtomb(char *__restrict s, char32_t c32, mbstate_t *__restrict ps)
{
    return wcrtomb(s, (wchar_t)c32, ps);
}

size_t mbrtoc16(char16_t *__restrict pc16, const char *__restrict s, size_t n,
                mbstate_t *__restrict ps)
{
    struct state *k = st16(ps);

    /* A low surrogate is owed from the previous call: deliver it and
     * consume NOTHING. -3 is the standard's way of saying "this unit
     * came from input you already gave me". */
    if (k->have == 0xFF) {
        if (pc16)
            *pc16 = (char16_t)k->wch;
        memset(k, 0, sizeof *k);
        return (size_t)-3;
    }
    if (!s) {
        memset(k, 0, sizeof *k);
        return 0;
    }

    wchar_t wc;
    size_t r = mbrtowc(&wc, s, n, ps);
    if (r == (size_t)-1 || r == (size_t)-2)
        return r;

    unsigned int c = (unsigned int)wc;
    if (c < 0x10000) {
        if (pc16)
            *pc16 = (char16_t)c;
        return r;                 /* r is 0 for a NUL, as required */
    }
    /* Above the basic plane: the high surrogate now, the low one on the
     * next call. */
    c -= 0x10000;
    if (pc16)
        *pc16 = (char16_t)(0xD800u + (c >> 10));
    k->wch = 0xDC00u + (c & 0x3FFu);
    k->have = 0xFF;               /* the marker for "a low unit is owed" */
    k->want = 0;
    return r;
}

size_t c16rtomb(char *__restrict s, char16_t c16, mbstate_t *__restrict ps)
{
    struct state *k = st16(ps);
    unsigned int u = (unsigned int)c16;

    if (k->have == 0xFF) {
        /* A high surrogate is pending. This unit must be its low half;
         * anything else is a lone surrogate, which is not a character. */
        if (u < 0xDC00u || u > 0xDFFFu) {
            memset(k, 0, sizeof *k);
            errno = EILSEQ;
            return (size_t)-1;
        }
        unsigned int c = 0x10000u + ((k->wch - 0xD800u) << 10) +
                         (u - 0xDC00u);
        memset(k, 0, sizeof *k);
        return wcrtomb(s, (wchar_t)c, ps);
    }

    if (u >= 0xD800u && u <= 0xDBFFu) {
        /* A high surrogate: nothing can be written yet. Zero means
         * exactly that -- not an error, and not an empty character. */
        k->wch = u;
        k->have = 0xFF;
        k->want = 0;
        return 0;
    }
    if (u >= 0xDC00u && u <= 0xDFFFu) {
        /* A low surrogate with no high one before it. */
        errno = EILSEQ;
        return (size_t)-1;
    }
    return wcrtomb(s, (wchar_t)u, ps);
}
