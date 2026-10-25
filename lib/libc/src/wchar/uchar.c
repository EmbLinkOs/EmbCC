/* UTF-8, UTF-16 and UTF-32 conversion, C11 §7.28 and C23 §7.30.
 *
 * Every one of these decodes or encodes Unicode's whole range, whatever
 * wchar_t is: char16_t and char32_t hold all of it, and a wchar_t of 16
 * bits (Xtensa's) does not -- so they are internal/utf8.h's decoder and
 * encoder at U+10FFFF, not calls through mbrtowc and wcrtomb, which stop
 * at what a wchar_t holds (mbrtoc32 once wrote two bytes of a four-byte
 * char32_t there).
 *
 * The UTF-16 pair is where the work is. A code point above U+FFFF does
 * not fit one 16-bit unit, so it is written as a SURROGATE PAIR: a high
 * surrogate in D800..DBFF and a low one in DC00..DFFF, each carrying ten
 * of the twenty bits of (c - 0x10000). That makes both directions
 * stateful in a way UTF-8 is not:
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
 *
 * C23's UTF-8 pair, char8_t, is the same shape with up to four units:
 * mbrtoc8 owes the rest of a character's bytes, one per call, each -3;
 * c8rtomb takes them one at a time and writes nothing until the last.
 */
#include <uchar.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "../internal/utf8.h"

/* The mbstate_t, as mb.c reads it: a partly decoded character -- or,
 * marked by `have`, units owed from one already decoded. The two uses
 * cannot overlap: one is between bytes, the other between code units. */
#define OWE16 0xFF            /* a low surrogate, in wch */
#define OWE8  0xF0            /* | how many UTF-8 units of wch are owed */

static struct __utf8 *st(mbstate_t *ps)
{
    static mbstate_t internal;
    return (struct __utf8 *)(ps ? ps : &internal);
}

/* One code point from at most n bytes: mbrtowc's results, for all of
 * Unicode. */
static size_t decode(uint_least32_t *c, const char *s, size_t n,
                     struct __utf8 *k)
{
    for (size_t used = 0; used < n; ) {
        int r = __utf8_step_to(k, (unsigned char)s[used++], c, 0x10FFFF);
        if (r == 0)
            continue;
        if (r < 0) {
            errno = EILSEQ;
            return (size_t)-1;
        }
        return *c ? used : 0;
    }
    return (size_t)-2;
}

/* One code point's bytes into s, or the NUL's when s is null. */
static size_t encode(char *s, uint_least32_t c)
{
    char buf[4];
    if (!s) {
        s = buf;
        c = 0;
    }
    int r = __utf8_put(c, s);
    if (r < 0) {
        errno = EILSEQ;
        return (size_t)-1;
    }
    return (size_t)r;
}

size_t mbrtoc32(char32_t *__restrict pc32, const char *__restrict s, size_t n,
                mbstate_t *__restrict ps)
{
    struct __utf8 *k = st(ps);
    uint_least32_t c;
    if (!s) {
        memset(k, 0, sizeof *k);
        return 0;
    }
    size_t r = decode(&c, s, n, k);
    if (r != (size_t)-1 && r != (size_t)-2 && pc32)
        *pc32 = (char32_t)c;
    return r;
}

size_t c32rtomb(char *__restrict s, char32_t c32, mbstate_t *__restrict ps)
{
    memset(st(ps), 0, sizeof(struct __utf8));
    return encode(s, (uint_least32_t)c32);
}

size_t mbrtoc16(char16_t *__restrict pc16, const char *__restrict s, size_t n,
                mbstate_t *__restrict ps)
{
    struct __utf8 *k = st(ps);

    /* A low surrogate is owed from the previous call: deliver it and
     * consume NOTHING. -3 is the standard's way of saying "this unit
     * came from input you already gave me". */
    if (k->have == OWE16) {
        if (pc16)
            *pc16 = (char16_t)k->wch;
        memset(k, 0, sizeof *k);
        return (size_t)-3;
    }
    if (!s) {
        memset(k, 0, sizeof *k);
        return 0;
    }

    uint_least32_t c;
    size_t r = decode(&c, s, n, k);
    if (r == (size_t)-1 || r == (size_t)-2)
        return r;
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
    k->have = OWE16;
    k->want = 0;
    return r;
}

size_t c16rtomb(char *__restrict s, char16_t c16, mbstate_t *__restrict ps)
{
    struct __utf8 *k = st(ps);
    uint_least32_t u = c16;

    if (k->have == OWE16) {
        /* A high surrogate is pending. This unit must be its low half;
         * anything else is a lone surrogate, which is not a character. */
        uint_least32_t hi = k->wch;
        memset(k, 0, sizeof *k);
        if (u < 0xDC00u || u > 0xDFFFu) {
            errno = EILSEQ;
            return (size_t)-1;
        }
        return encode(s, 0x10000u + ((hi - 0xD800u) << 10) + (u - 0xDC00u));
    }
    memset(k, 0, sizeof *k);
    if (u >= 0xD800u && u <= 0xDBFFu) {
        /* A high surrogate: nothing can be written yet. Zero means
         * exactly that -- not an error, and not an empty character. */
        k->wch = u;
        k->have = OWE16;
        return 0;
    }
    /* a low surrogate with no high one before it is refused by encode */
    return encode(s, u);
}

size_t mbrtoc8(unsigned char *__restrict pc8, const char *__restrict s, size_t n,
               mbstate_t *__restrict ps)
{
    struct __utf8 *k = st(ps);
    char b[4];

    /* The rest of a character decoded before: its next unit, and no
     * input consumed. */
    if ((k->have & 0xF0) == OWE8 && k->have != OWE16) {
        int len = __utf8_put(k->wch, b), owed = k->have & 0x0F;
        if (pc8)
            *pc8 = (unsigned char)b[len - owed];
        if (--owed)
            k->have = (unsigned char)(OWE8 | owed);
        else
            memset(k, 0, sizeof *k);
        return (size_t)-3;
    }
    if (!s) {
        memset(k, 0, sizeof *k);
        return 0;
    }

    uint_least32_t c;
    size_t r = decode(&c, s, n, k);
    if (r == (size_t)-1 || r == (size_t)-2)
        return r;
    int len = __utf8_put(c, b);
    if (pc8)
        *pc8 = (unsigned char)b[0];
    if (len > 1) {
        k->wch = c;
        k->want = 0;
        k->have = (unsigned char)(OWE8 | (len - 1));
    }
    return r;
}

size_t c8rtomb(char *__restrict s, unsigned char c8, mbstate_t *__restrict ps)
{
    struct __utf8 *k = st(ps);
    uint_least32_t c;
    char buf[4];
    if (!s) {                      /* as c8rtomb(buf, u8'\0', ps) */
        s = buf;
        c8 = 0;
    }
    int r = __utf8_step_to(k, (unsigned char)c8, &c, 0x10FFFF);
    if (r == 0)
        return 0;                  /* the character is not complete yet */
    if (r < 0) {
        errno = EILSEQ;
        return (size_t)-1;
    }
    return encode(s, c);
}
