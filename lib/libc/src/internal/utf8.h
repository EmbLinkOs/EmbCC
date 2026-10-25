/* UTF-8, one character at a time: the decoder and the encoder this
 * library's multibyte functions (wchar/mb.c) are built on, and the ones
 * printf's %ls and %lc, scanf's and the wide streams use -- the same rules
 * everywhere, without a byte printf linking all of mb.c and wcs.c for a
 * conversion most programs never ask for (a Cortex-M printf was 3 KB
 * bigger that way).
 *
 * mb.c says what is refused and why: overlong forms, surrogates, anything
 * past U+10FFFF -- and, where wchar_t is 16 bits (Xtensa's, as GCC has
 * it), anything past what a wchar_t holds, which has no wide form there.
 */
#ifndef EMBLIBC_UTF8_H
#define EMBLIBC_UTF8_H

#include <stdint.h>

#if __WCHAR_MAX__ < 0x10FFFF
#define __UTF8_LAST __WCHAR_MAX__
#else
#define __UTF8_LAST 0x10FFFF
#endif

/* A character partly read: what mbstate_t is, laid out so that a zeroed one
 * is the initial state. A code point is 21 bits, so 32 of them -- an
 * unsigned int is 16 on AVR. */
struct __utf8 {
    uint_least32_t wch;       /* the bits so far */
    unsigned char want;       /* bytes still to come (0: none pending) */
    unsigned char have;       /* bytes taken */
    uint_least32_t lowbound;  /* the least value this length may encode */
};

static inline int __utf8_bad(uint_least32_t c, uint_least32_t last)
{
    return (c >= 0xD800 && c <= 0xDFFF) || c > last;
}

/* One more byte. 1: *out is a whole character (0 for the NUL byte); 0: more
 * bytes wanted; -1: this is not UTF-8, or a code point past `last`. After
 * 1 or -1 the state is the initial one again, so a caller can go on from
 * the next byte. */
static inline int __utf8_step_to(struct __utf8 *k, unsigned char b,
                                 uint_least32_t *out, uint_least32_t last)
{
    if (k->want == 0) {
        if (b < 0x80) {
            *out = b;
            return 1;
        }
        /* a continuation byte with nothing to continue, or one of the
         * bytes that can never begin a sequence */
        if (b < 0xC2 || b > 0xF4)
            return -1;
        if (b < 0xE0)      { k->want = 1; k->wch = b & 0x1Fu; k->lowbound = 0x80; }
        else if (b < 0xF0) { k->want = 2; k->wch = b & 0x0Fu; k->lowbound = 0x800; }
        else               { k->want = 3; k->wch = b & 0x07u; k->lowbound = 0x10000; }
        k->have = 1;
        return 0;
    }
    uint_least32_t c = (k->wch << 6) | (b & 0x3Fu), low = k->lowbound;
    int cont = (b & 0xC0u) == 0x80u;
    if (cont && --k->want) {
        k->wch = c;
        k->have++;
        return 0;
    }
    k->wch = 0;
    k->want = 0;
    k->have = 0;
    k->lowbound = 0;
    if (!cont || c < low || __utf8_bad(c, last))
        return -1;
    *out = c;
    return 1;
}

/* ... for a wchar_t: up to what one holds. char16_t and char32_t take all
 * of Unicode (wchar/uchar.c). */
static inline int __utf8_step(struct __utf8 *k, unsigned char b,
                              uint_least32_t *out)
{
    return __utf8_step_to(k, b, out, __UTF8_LAST);
}

/* The bytes of one character, into s (room for four): their count, or -1
 * for a value that has none. */
static inline int __utf8_put(uint_least32_t c, char *s)
{
    if (__utf8_bad(c, 0x10FFFF))
        return -1;
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

#endif
