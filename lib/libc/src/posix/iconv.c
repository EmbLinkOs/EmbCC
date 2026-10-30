/* <iconv.h>: one character at a time, through Unicode. Each input
 * character is decoded to its code point and encoded again, so any
 * encoding below converts to any other.
 *
 *   UTF-8
 *   UTF-16, UTF-16LE, UTF-16BE      UTF-16 and UTF-32 read a byte-order
 *   UTF-32, UTF-32LE, UTF-32BE      mark, and assume this machine's order
 *                                   without one; they write one in this
 *                                   machine's order, then go on in it --
 *                                   all as glibc does
 *   UCS-2, UCS-2LE, UCS-2BE         the Basic Multilingual Plane only
 *   UCS-4, UCS-4LE, UCS-4BE
 *   WCHAR_T                         this platform's wchar_t
 *   ASCII (US-ASCII, ANSI_X3.4-1968), ISO-8859-1 (LATIN1),
 *   ISO-8859-15 (LATIN-9), CP1252 (WINDOWS-1252)
 *
 * Names are matched ignoring case and the punctuation people vary
 * ("utf8", "UTF-8", "Utf_8"); "" and "CHAR" mean the locale's, which is
 * UTF-8 here.
 *
 * A character the target cannot hold stops the conversion with EILSEQ,
 * unless the target's name ends in //TRANSLIT (it becomes what glibc's
 * C locale makes it -- EUR for the euro sign, "1/2" for one half, A for
 * a mathematical bold A; '?' for anything else -- counted as
 * irreversible; iconv-translit.h has the table, read from glibc 2.36 by
 * converting every code point to ASCII//TRANSLIT) or //IGNORE (it is dropped, and the call says EILSEQ
 * once it has converted everything else -- glibc's convention). //IGNORE
 * skips bytes that are not a character of the source encoding too.
 */
#define _GNU_SOURCE 1
#include <iconv.h>
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wchar.h>

#include "posix.h"

enum enc {
    E_UTF8, E_UTF16, E_UTF16LE, E_UTF16BE, E_UTF32, E_UTF32LE, E_UTF32BE,
    E_UCS2, E_UCS2LE, E_UCS2BE, E_UCS4, E_UCS4LE, E_UCS4BE, E_WCHAR,
    E_ASCII, E_LATIN1, E_LATIN9, E_CP1252
};

static const struct name {
    const char *name;          /* upper case, no punctuation */
    unsigned char enc;
} g_names[] = {
    {"UTF8", E_UTF8}, {"", E_UTF8}, {"CHAR", E_UTF8},
    {"UTF16", E_UTF16}, {"UTF16LE", E_UTF16LE}, {"UTF16BE", E_UTF16BE},
    {"UTF32", E_UTF32}, {"UTF32LE", E_UTF32LE}, {"UTF32BE", E_UTF32BE},
    {"UCS2", E_UCS2}, {"UCS2LE", E_UCS2LE}, {"UCS2BE", E_UCS2BE},
    {"UNICODELITTLE", E_UCS2LE}, {"UNICODEBIG", E_UCS2BE},
    {"UCS4", E_UCS4}, {"UCS4LE", E_UCS4LE}, {"UCS4BE", E_UCS4BE},
    {"WCHART", E_WCHAR},
    {"ASCII", E_ASCII}, {"USASCII", E_ASCII}, {"ANSIX341968", E_ASCII},
    {"ISO646US", E_ASCII}, {"US", E_ASCII},
    {"ISO88591", E_LATIN1}, {"LATIN1", E_LATIN1}, {"L1", E_LATIN1},
    {"ISO885915", E_LATIN9}, {"LATIN9", E_LATIN9}, {"L9", E_LATIN9},
    {"CP1252", E_CP1252}, {"WINDOWS1252", E_CP1252},
};

/* CP1252's 0x80-0x9F; 0 where the code page has no character */
static const uint16_t g_cp1252[32] = {
    0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
    0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178,
};

/* where ISO-8859-15 parts from ISO-8859-1 */
static const struct { unsigned char b; uint16_t u; } g_latin9[8] = {
    {0xA4, 0x20AC}, {0xA6, 0x0160}, {0xA8, 0x0161}, {0xB4, 0x017D},
    {0xB8, 0x017E}, {0xBC, 0x0152}, {0xBD, 0x0153}, {0xBE, 0x0178},
};

struct conv {
    unsigned char from, to;
    unsigned char translit, ignore;
    unsigned char in_order;    /* 0 not yet known, 1 big-endian, 2 little */
    unsigned char out_marked;  /* our byte-order mark is written */
};

static int big_host(void)
{
    const uint16_t one = 1;
    return !*(const unsigned char *)&one;
}

/* the encoding a name means, -1 for none of ours; //TRANSLIT and
 * //IGNORE, in either order or after one // with a comma between */
static int lookup(const char *s, int *translit, int *ignore)
{
    char key[32];
    size_t k = 0;
    for (; *s && *s != '/'; s++) {
        if (*s == '-' || *s == '_' || *s == '.' || *s == ' ' || *s == ':')
            continue;
        if (k + 1 >= sizeof key)
            return -1;
        key[k++] = (char)toupper((unsigned char)*s);
    }
    key[k] = 0;
    for (;;) {
        while (*s == '/' || *s == ',')
            s++;
        if (!*s)
            break;
        if (!strncasecmp(s, "TRANSLIT", 8)) {
            *translit = 1;
            s += 8;
        } else if (!strncasecmp(s, "IGNORE", 6)) {
            *ignore = 1;
            s += 6;
        } else {
            return -1;
        }
    }
    for (size_t i = 0; i < sizeof g_names / sizeof g_names[0]; i++)
        if (!strcmp(key, g_names[i].name))
            return g_names[i].enc;
    return -1;
}

WEAK iconv_t iconv_open(const char *tocode, const char *fromcode)
{
    int tt = 0, ti = 0, ft = 0, fi = 0;
    int to = lookup(tocode, &tt, &ti);
    int from = lookup(fromcode, &ft, &fi);
    if (to < 0 || from < 0) {
        errno = EINVAL;
        return (iconv_t)-1;
    }
    struct conv *c = calloc(1, sizeof *c);
    if (!c)
        return (iconv_t)-1;
    c->from = (unsigned char)from;
    c->to = (unsigned char)to;
    c->translit = (unsigned char)tt;
    c->ignore = (unsigned char)(ti | fi);
    return c;
}

WEAK int iconv_close(iconv_t cd)
{
    if (cd == (iconv_t)-1 || !cd) {
        errno = EBADF;
        return -1;
    }
    free(cd);
    return 0;
}

/* ---- decoding ------------------------------------------------------------------- */

enum { BAD = -1, SHORT = -2 };

static uint32_t rd(const unsigned char *p, int n, int big)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++)
        v |= (uint32_t)p[big ? i : n - 1 - i] << (8 * (n - 1 - i));
    return v;
}

static int is_surrogate(uint32_t u) { return u >= 0xD800 && u <= 0xDFFF; }

/* One character from p (n bytes there): its length, with *u its code
 * point; BAD for bytes that are no character, SHORT for the start of one
 * that does not finish before the input does. A byte-order mark the
 * encoding reads is taken here, as a character of length >0 and *u
 * 0xFFFFFFFF, which the caller skips. */
static int decode(struct conv *c, const unsigned char *p, size_t n, uint32_t *u)
{
    switch (c->from) {
    case E_UTF8: {
        unsigned char b = p[0];
        if (b < 0x80) {
            *u = b;
            return 1;
        }
        int len = b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC0 ? 2 : 0;
        if (!len || b > 0xF4 || b == 0xC0 || b == 0xC1)
            return BAD;
        uint32_t v = b & (0x7F >> len);
        for (int i = 1; i < len; i++) {
            if ((size_t)i >= n)
                return SHORT;
            if ((p[i] & 0xC0) != 0x80)
                return BAD;
            v = v << 6 | (p[i] & 0x3F);
            /* overlong, surrogate or past U+10FFFF: known at the second byte */
            if (i == 1 && ((len == 3 && v < 0x20) || (len == 4 && v < 0x10) ||
                           (len == 3 && v >= 0x360 && v < 0x380) ||
                           (len == 4 && v > 0x10F)))
                return BAD;
        }
        *u = v;
        return len;
    }
    case E_UTF16: case E_UTF16LE: case E_UTF16BE:
    case E_UCS2: case E_UCS2LE: case E_UCS2BE: {
        if (n < 2)
            return SHORT;
        if (c->from == E_UTF16 && !c->in_order) {
            uint32_t w = rd(p, 2, 1);
            c->in_order = big_host() ? 1 : 2;
            if (w == 0xFEFF || w == 0xFFFE) {
                c->in_order = w == 0xFEFF ? 1 : 2;
                *u = 0xFFFFFFFF;
                return 2;
            }
        }
        int big = c->from == E_UTF16BE || c->from == E_UCS2BE ||
                  (c->from == E_UCS2 && big_host()) ||
                  (c->from == E_UTF16 && c->in_order == 1);
        uint32_t w = rd(p, 2, big);
        if (c->from >= E_UCS2) {
            if (is_surrogate(w))
                return BAD;
            *u = w;
            return 2;
        }
        if (w >= 0xDC00 && w <= 0xDFFF)
            return BAD;
        if (w < 0xD800 || w > 0xDBFF) {
            *u = w;
            return 2;
        }
        if (n < 4)
            return SHORT;
        uint32_t lo = rd(p + 2, 2, big);
        if (lo < 0xDC00 || lo > 0xDFFF)
            return BAD;
        *u = 0x10000 + ((w - 0xD800) << 10) + (lo - 0xDC00);
        return 4;
    }
    case E_UTF32: case E_UTF32LE: case E_UTF32BE:
    case E_UCS4: case E_UCS4LE: case E_UCS4BE: case E_WCHAR: {
        int size = c->from == E_WCHAR ? (int)sizeof(wchar_t) : 4;
        if (n < (size_t)size)
            return SHORT;
        if (c->from == E_UTF32 && !c->in_order) {
            uint32_t w = rd(p, 4, 1);
            c->in_order = big_host() ? 1 : 2;
            if (w == 0xFEFF || w == 0xFFFE0000) {
                c->in_order = w == 0xFEFF ? 1 : 2;
                *u = 0xFFFFFFFF;
                return 4;
            }
        }
        int big = c->from == E_UTF32BE || c->from == E_UCS4BE ||
                  c->from == E_UCS4 ||
                  (c->from == E_WCHAR && big_host()) ||
                  (c->from == E_UTF32 && c->in_order == 1);
        uint32_t w = rd(p, size, big);
        if (w > 0x10FFFF || is_surrogate(w))
            return BAD;
        *u = w;
        return size;
    }
    case E_ASCII:
        if (p[0] > 0x7F)
            return BAD;
        *u = p[0];
        return 1;
    case E_LATIN9:
        for (int i = 0; i < 8; i++)
            if (p[0] == g_latin9[i].b) {
                *u = g_latin9[i].u;
                return 1;
            }
        *u = p[0];
        return 1;
    case E_CP1252:
        if (p[0] >= 0x80 && p[0] < 0xA0) {
            if (!g_cp1252[p[0] - 0x80])
                return BAD;
            *u = g_cp1252[p[0] - 0x80];
            return 1;
        }
        *u = p[0];
        return 1;
    default:                   /* E_LATIN1 */
        *u = p[0];
        return 1;
    }
}

/* ---- encoding ------------------------------------------------------------------- */

static void wr(unsigned char *p, uint32_t v, int n, int big)
{
    for (int i = 0; i < n; i++)
        p[big ? n - 1 - i : i] = (unsigned char)(v >> (8 * i));
}

/* u in the target encoding, into out: the length, or BAD if the target
 * has no such character */
static int encode(const struct conv *c, uint32_t u, unsigned char *out)
{
    switch (c->to) {
    case E_UTF8:
        if (u < 0x80) {
            out[0] = (unsigned char)u;
            return 1;
        }
        if (u < 0x800) {
            out[0] = (unsigned char)(0xC0 | u >> 6);
            out[1] = (unsigned char)(0x80 | (u & 0x3F));
            return 2;
        }
        if (u < 0x10000) {
            out[0] = (unsigned char)(0xE0 | u >> 12);
            out[1] = (unsigned char)(0x80 | (u >> 6 & 0x3F));
            out[2] = (unsigned char)(0x80 | (u & 0x3F));
            return 3;
        }
        out[0] = (unsigned char)(0xF0 | u >> 18);
        out[1] = (unsigned char)(0x80 | (u >> 12 & 0x3F));
        out[2] = (unsigned char)(0x80 | (u >> 6 & 0x3F));
        out[3] = (unsigned char)(0x80 | (u & 0x3F));
        return 4;
    case E_UTF16: case E_UTF16LE: case E_UTF16BE: {
        int big = c->to == E_UTF16BE || (c->to == E_UTF16 && big_host());
        if (u < 0x10000) {
            wr(out, u, 2, big);
            return 2;
        }
        u -= 0x10000;
        wr(out, 0xD800 + (u >> 10), 2, big);
        wr(out + 2, 0xDC00 + (u & 0x3FF), 2, big);
        return 4;
    }
    case E_UCS2: case E_UCS2LE: case E_UCS2BE:
        if (u > 0xFFFF)
            return BAD;
        wr(out, u, 2, c->to == E_UCS2BE || (c->to == E_UCS2 && big_host()));
        return 2;
    case E_UTF32: case E_UTF32LE: case E_UTF32BE:
    case E_UCS4: case E_UCS4LE: case E_UCS4BE:
        wr(out, u, 4, c->to == E_UTF32BE || c->to == E_UCS4BE ||
                      c->to == E_UCS4 || (c->to == E_UTF32 && big_host()));
        return 4;
    case E_WCHAR:
        if (sizeof(wchar_t) < 4 && u > 0xFFFF)
            return BAD;
        wr(out, u, (int)sizeof(wchar_t), big_host());
        return (int)sizeof(wchar_t);
    case E_ASCII:
        if (u > 0x7F)
            return BAD;
        out[0] = (unsigned char)u;
        return 1;
    case E_LATIN9:
        for (int i = 0; i < 8; i++) {
            if (u == g_latin9[i].u) {
                out[0] = g_latin9[i].b;
                return 1;
            }
            if (u == g_latin9[i].b)
                return BAD;    /* ISO-8859-1's character there is gone */
        }
        if (u > 0xFF)
            return BAD;
        out[0] = (unsigned char)u;
        return 1;
    case E_CP1252:
        if (u < 0x80 || (u >= 0xA0 && u <= 0xFF)) {
            out[0] = (unsigned char)u;
            return 1;
        }
        for (int i = 0; i < 32; i++)
            if (g_cp1252[i] && g_cp1252[i] == u) {
                out[0] = (unsigned char)(0x80 + i);
                return 1;
            }
        return BAD;
    default:                   /* E_LATIN1 */
        if (u > 0xFF)
            return BAD;
        out[0] = (unsigned char)u;
        return 1;
    }
}

/* ---- transliteration ------------------------------------------------------------- */

#include "iconv-translit.h"

/* what the C locale puts for u, NUL-terminated ASCII, or NULL */
static const char *tr_text(uint32_t u, char *one)
{
    size_t lo = 0, hi = sizeof g_tr_run / sizeof g_tr_run[0];
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (g_tr_run[mid] >> 9 <= u)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (!lo)
        return 0;
    uint32_t r = g_tr_run[lo - 1], start = r >> 9;
    if (u - start > (r >> 1 & 0xFF))
        return 0;
    const char *t = g_tr_text + g_tr_off[lo - 1];
    if (!(r & 1))
        return t;
    one[0] = (char)(t[0] + (u - start));
    one[1] = 0;
    return one;
}

/* u, which the target has not got, as the C locale transliterates it,
 * or '?' where it does not (or the target lacks part of what it gives) */
static int translit(const struct conv *c, uint32_t u, unsigned char *out)
{
    char one[2];
    const char *t = tr_text(u, one);
    if (t) {
        int k = 0;
        for (; *t; t++) {
            int n = encode(c, (unsigned char)*t, out + k);
            if (n == BAD)
                break;
            k += n;
        }
        if (!*t)
            return k;
    }
    return encode(c, '?', out);
}

/* the byte-order mark UTF-16 and UTF-32 write first */
static int mark(const struct conv *c, unsigned char *out)
{
    if (c->to == E_UTF16) {
        wr(out, 0xFEFF, 2, big_host());
        return 2;
    }
    if (c->to == E_UTF32) {
        wr(out, 0xFEFF, 4, big_host());
        return 4;
    }
    return 0;
}

WEAK size_t iconv(iconv_t cd, char **restrict in, size_t *restrict inleft,
                  char **restrict out, size_t *restrict outleft)
{
    struct conv *c = cd;
    if (cd == (iconv_t)-1 || !cd) {
        errno = EBADF;
        return (size_t)-1;
    }
    /* no input: back to the initial state, which for these encodings has
     * no bytes to write */
    if (!in || !*in) {
        c->in_order = 0;
        c->out_marked = 0;
        return 0;
    }
    size_t irreversible = 0;
    int skipped = 0;
    while (*inleft) {
        const unsigned char *p = (const unsigned char *)*in;
        uint32_t u;
        int len = decode(c, p, *inleft, &u);
        if (len == SHORT) {
            errno = EINVAL;
            return (size_t)-1;
        }
        if (len == BAD) {
            if (!c->ignore) {
                errno = EILSEQ;
                return (size_t)-1;
            }
            skipped = 1;
            ++*in;
            --*inleft;
            continue;
        }
        if (u == 0xFFFFFFFF) {           /* a byte-order mark read */
            *in += len;
            *inleft -= len;
            continue;
        }
        unsigned char buf[4 + 8 * 4];
        int m = 0;
        if (!c->out_marked)
            m = mark(c, buf);
        int k = encode(c, u, buf + m);
        if (k == BAD) {
            if (c->translit) {
                k = translit(c, u, buf + m);
                irreversible++;
            } else if (c->ignore) {
                skipped = 1;
                *in += len;
                *inleft -= len;
                continue;
            } else {
                errno = EILSEQ;
                return (size_t)-1;
            }
        }
        if ((size_t)(m + k) > *outleft) {
            /* the mark goes out on its own if it fits, as glibc's does */
            if (m && (size_t)m <= *outleft) {
                memcpy(*out, buf, (size_t)m);
                *out += m;
                *outleft -= (size_t)m;
                c->out_marked = 1;
            }
            errno = E2BIG;
            return (size_t)-1;
        }
        memcpy(*out, buf, (size_t)(m + k));
        *out += m + k;
        *outleft -= (size_t)(m + k);
        if (m)
            c->out_marked = 1;
        *in += len;
        *inleft -= len;
    }
    if (skipped) {
        errno = EILSEQ;
        return (size_t)-1;
    }
    return irreversible;
}
