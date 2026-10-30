/* Wide-character input and output, C11 §7.29.3, and the wide printf and
 * scanf families, §7.29.2.
 *
 * A wide stream is a byte stream whose bytes are UTF-8 -- the one
 * multibyte encoding this library has (wchar/mb.c) -- so each function
 * here is a byte one with a conversion on the way: fputwc is wcrtomb and
 * a write, fgetwc reads bytes until mbrtowc has a character. The
 * conversion state lives in the call, not the stream: a character is
 * written whole and read whole inside one call, so nothing is left over
 * between calls to keep, and a FILE costs nothing more for being able to
 * be wide.
 *
 * A byte that is not UTF-8, a character the end of the file cuts short,
 * or a wide character with no UTF-8 form (a lone surrogate) is an
 * encoding error: WEOF, errno EILSEQ, and the stream's error indicator
 * (C11 7.29.3.1p3, 7.29.3.3p3).
 *
 * The first wide operation on a stream that has none yet makes it wide
 * (7.21.2p4); file.h has how orientation is kept.
 */
#include "file.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>

#include "../internal/utf8.h"

static void wide_orient(FILE *f)
{
    if (!(f->flags & (F_WIDE | F_BYTE)))
        f->flags |= F_WIDE;
}

int fwide(FILE *f, int mode)
{
    if (!f)
        return 0;
    __flockfile(f);
    if (mode > 0)
        wide_orient(f);
    else if (mode < 0)
        __byte_orient(f);
    int r = f->flags & F_WIDE ? 1 : f->flags & F_BYTE ? -1 : 0;
    __funlockfile(f);
    return r;
}

/* One character in; WEOF at the end, and WEOF with *bad set on an encoding
 * error. */
static wint_t wide_getc_raw(FILE *f, int *bad)
{
    wide_orient(f);
    if (f->ungot >= 0) {
        wint_t c = (wint_t)f->ungot;
        f->ungot = -1;
        return c;
    }
    if (!(f->flags & F_READ))
        return WEOF;
    struct __utf8 k = { 0, 0, 0, 0 };
    for (;;) {
        int c = __getc_unlocked(f), r = -1;
        uint_least32_t wc;
        if (c != EOF)
            r = __utf8_step(&k, (unsigned char)c, &wc);
        else if (!k.want)
            return WEOF;               /* the end, between characters */
        if (r == 0)
            continue;
        if (r < 0) {                   /* not UTF-8, or cut short */
            errno = EILSEQ;
            f->flags |= F_ERR;
            if (bad) *bad = 1;
            return WEOF;
        }
        return (wint_t)wc;
    }
}

static wint_t wide_putc_raw(wchar_t c, FILE *f)
{
    wide_orient(f);
    char b[4];
    int r = __utf8_put((uint_least32_t)c, b);
    if (r < 0) {
        errno = EILSEQ;
        f->flags |= F_ERR;
        return WEOF;
    }
    if (__fwrite_unlocked(b, 1, (size_t)r, f) != (size_t)r)
        return WEOF;
    return (wint_t)c;
}

wint_t fgetwc(FILE *f)
{
    if (!f)
        return WEOF;
    __flockfile(f);
    wint_t c = wide_getc_raw(f, NULL);
    __funlockfile(f);
    return c;
}

wint_t getwc(FILE *f)  { return fgetwc(f); }
wint_t getwchar(void)  { return fgetwc(stdin); }

wint_t fputwc(wchar_t c, FILE *f)
{
    if (!f)
        return WEOF;
    __flockfile(f);
    wint_t r = wide_putc_raw(c, f);
    __funlockfile(f);
    return r;
}

wint_t putwc(wchar_t c, FILE *f) { return fputwc(c, f); }
wint_t putwchar(wchar_t c)       { return fputwc(c, stdout); }

/* One wide character of pushback, as C guarantees -- held as itself, not
 * as its bytes, in the slot a byte stream's ungetc uses. */
wint_t ungetwc(wint_t c, FILE *f)
{
    if (!f || c == WEOF)
        return WEOF;
    __flockfile(f);
    wide_orient(f);
    wint_t r = WEOF;
    if (f->ungot < 0) {
        f->ungot = (int)c;
        f->flags &= ~F_EOF;
        r = c;
    }
    __funlockfile(f);
    return r;
}

/* As fgets: up to n - 1 characters, the newline kept. A read or encoding
 * error makes the result a null pointer, whatever was read before it. */
wchar_t *fgetws(wchar_t *restrict s, int n, FILE *restrict f)
{
    if (n <= 0 || !f)
        return NULL;
    int i = 0, bad = 0;
    __flockfile(f);
    while (i < n - 1) {
        wint_t c = wide_getc_raw(f, &bad);
        if (c == WEOF)
            break;
        s[i++] = (wchar_t)c;
        if (c == (wint_t)L'\n')
            break;
    }
    __funlockfile(f);
    if (bad || i == 0)
        return NULL;
    s[i] = 0;
    return s;
}

int fputws(const wchar_t *restrict s, FILE *restrict f)
{
    if (!f)
        return EOF;
    int r = 0;
    __flockfile(f);
    wide_orient(f);
    for (; *s; s++)
        if (wide_putc_raw(*s, f) == WEOF) {
            r = EOF;
            break;
        }
    __funlockfile(f);
    return r;
}

/* ---- the wide printfs ----
 *
 * The format engine's wide instance (format-run.h, as format.c's): a wide
 * format, and a sink of wide characters. Its own %c and %s take bytes and
 * write them as wide characters, decoded as btowc and mbrtowc do; its %lc
 * and %ls are wide characters already. A byte that is not UTF-8 is an
 * encoding error: nothing more is written, and the call returns -1 with
 * errno EILSEQ (C11 7.29.2.1p14). */
#include "format.h"

static void emit_w(struct out *o, const wchar_t *s, size_t n)
{
    o->wsink(o->ctx, s, n);
    o->n += (int)n;
}

/* The wide characters of the multibyte string s, at most `prec` of them;
 * -1 on an encoding error. A NUL inside a character is not a continuation
 * byte, so nothing past the terminator is read. */
static long wide_of_mb(struct out *o, const char *s, int prec, int write)
{
    struct __utf8 k = { 0, 0, 0, 0 };
    long n = 0;
    while (prec < 0 || n < prec) {
        uint_least32_t c;
        int r;
        do
            r = __utf8_step(&k, (unsigned char)*s++, &c);
        while (r == 0);
        if (r < 0)
            return -1;
        if (c == 0)
            break;
        if (write) {
            wchar_t wc = (wchar_t)c;
            emit_w(o, &wc, 1);
        }
        n++;
    }
    return n;
}

static void put_wchar(struct out *o, int is_l, va_list *ap, int flags,
                      int width)
{
    wchar_t wc;
    if (is_l) {
        wc = (wchar_t)va_arg(*ap, wint_t);
    } else {
        /* as btowc: a byte is a character only if it is ASCII */
        unsigned char u = (unsigned char)va_arg(*ap, int);
        if (u >= 0x80) { o->bad = 1; return; }
        wc = u;
    }
    if (!(flags & F_MINUS)) __fmt_pad(o, ' ', width - 1);
    emit_w(o, &wc, 1);
    if (flags & F_MINUS) __fmt_pad(o, ' ', width - 1);
}

static void put_wstr(struct out *o, int is_l, va_list *ap, int flags,
                     int width, int prec)
{
    long n = 0;
    const wchar_t *ws = NULL;
    const char *s = NULL;
    if (is_l) {
        ws = va_arg(*ap, const wchar_t *);
        if (!ws) ws = L"(null)";
        /* a precision is a maximum: the array need not end within it */
        while ((prec < 0 || n < prec) && ws[n]) n++;
    } else {
        s = va_arg(*ap, const char *);
        if (!s) s = "(null)";
        n = wide_of_mb(o, s, prec, 0);
        if (n < 0) { o->bad = 1; return; }
    }
    if (!(flags & F_MINUS)) __fmt_pad(o, ' ', width - (int)n);
    if (ws) emit_w(o, ws, (size_t)n); else wide_of_mb(o, s, prec, 1);
    if (flags & F_MINUS) __fmt_pad(o, ' ', width - (int)n);
}

#define FMT_CH wchar_t
#define FMT_RUN format_wide
#define FMT_TEXT(o, s, n) emit_w((o), (s), (n))
#define FMT_CHAR put_wchar
#define FMT_STR put_wstr
#include "format-run.h"

static int wformat(void (*wsink)(void *, const wchar_t *, size_t), void *ctx,
                   const wchar_t *fmt, va_list ap)
{
    struct out o = { NULL, wsink, ctx, 0, 0 };
    va_list cp;
    va_copy(cp, ap);
    int r = format_wide(&o, fmt, &cp);
    va_end(cp);
    return r;
}

struct wfile { FILE *f; int bad; };

static void sink_wfile(void *ctx, const wchar_t *s, size_t n)
{
    struct wfile *w = ctx;
    for (size_t i = 0; i < n; i++)
        if (wide_putc_raw(s[i], w->f) == WEOF)
            w->bad = 1;
}

struct wbuf { wchar_t *p; size_t cap, n; };

static void sink_wbuf(void *ctx, const wchar_t *s, size_t n)
{
    struct wbuf *b = ctx;
    for (size_t i = 0; i < n; i++, b->n++)
        if (b->n + 1 < b->cap)
            b->p[b->n] = s[i];
}

int vfwprintf(FILE *restrict f, const wchar_t *restrict fmt, va_list ap)
{
    if (!f)
        return -1;
    __flockfile(f);
    wide_orient(f);
    struct wfile w = { f, 0 };
    int r = wformat(sink_wfile, &w, fmt, ap);
    __funlockfile(f);
    return w.bad ? -1 : r;
}

int vwprintf(const wchar_t *restrict fmt, va_list ap)
{
    return vfwprintf(stdout, fmt, ap);
}

/* Unlike vsnprintf, a result that does not fit is a failure: C11
 * 7.29.2.7p3 makes it negative rather than the length it would have had.
 * What fits is still written, and terminated. */
int vswprintf(wchar_t *restrict s, size_t n, const wchar_t *restrict fmt,
              va_list ap)
{
    struct wbuf b = { s, n, 0 };
    int r = wformat(sink_wbuf, &b, fmt, ap);
    if (n)
        s[b.n < n - 1 ? b.n : n - 1] = 0;
    if (r < 0 || (size_t)r >= n)
        return -1;
    return r;
}

int fwprintf(FILE *restrict f, const wchar_t *restrict fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vfwprintf(f, fmt, ap);
    va_end(ap);
    return r;
}

int wprintf(const wchar_t *restrict fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vfwprintf(stdout, fmt, ap);
    va_end(ap);
    return r;
}

int swprintf(wchar_t *restrict s, size_t n, const wchar_t *restrict fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vswprintf(s, n, fmt, ap);
    va_end(ap);
    return r;
}

/* ---- the wide scanfs ----
 *
 * The scan engine's wide instance (scan-run.h, as scan.c's): wide input,
 * a wide format. Its %c, %s and %[ store wide characters with l and
 * otherwise their UTF-8 -- a lone surrogate, which has none, is a matching
 * failure with errno EILSEQ. A %[ set is of wide characters, so it is
 * read from the format each time rather than made into a table. */
#include "scan.h"
#include <limits.h>
#include <wctype.h>

struct wdest {
    char *c;              /* or */
    wchar_t *w;
    size_t n;             /* elements stored */
};

static void wd_open(struct wdest *d, int assign, int is_l, va_list *ap)
{
    d->c = NULL;
    d->w = NULL;
    d->n = 0;
    if (!assign)
        return;
    if (is_l) d->w = va_arg(*ap, wchar_t *);
    else      d->c = va_arg(*ap, char *);
}

static int wd_put(struct wdest *d, int is_l, int c)
{
    if (is_l) {
        if (d->w) d->w[d->n] = (wchar_t)c;
        d->n++;
        return 1;
    }
    char b[4];
    int r = __utf8_put((uint_least32_t)c, b);
    if (r < 0)
        return 0;
    if (d->c) memcpy(d->c + d->n, b, (size_t)r);
    d->n += (size_t)r;
    return 1;
}

static int wd_close(struct wdest *d, int is_l, int terminate)
{
    (void)is_l;
    if (terminate) {
        if (d->w) d->w[d->n] = 0;
        if (d->c) d->c[d->n] = 0;
    }
    return 1;
}

/* A '-' first or last is a literal '-'; between two others it is a
 * range. */
struct wset { const wchar_t *at, *end; };

static void wset_setup(struct wset *set, const wchar_t *at,
                       const wchar_t *end)
{
    set->at = at;
    set->end = end;
}

static int wset_in(const struct wset *set, int c)
{
    for (const wchar_t *f = set->at; f < set->end; f++) {
        if (f + 2 < set->end && f[1] == L'-') {
            if (c >= (int)f[0] && c <= (int)f[2])
                return 1;
            f += 2;
            continue;
        }
        if (c == (int)*f)
            return 1;
    }
    return 0;
}

#define SCAN_CH wchar_t
#define SCAN_RUN scan_wide
#define SCAN_FVAL(f) ((int)(f))
#define SCAN_FSPACE(f) iswspace((wint_t)(f))
#define SCAN_SPACE(c) iswspace((wint_t)(c))
#define SCAN_DEST struct wdest
#define SCAN_DOPEN wd_open
#define SCAN_DPUT wd_put
#define SCAN_DCLOSE wd_close
#define SCAN_SET struct wset
#define SCAN_SETUP wset_setup
#define SCAN_IN wset_in
#include "scan-run.h"

static int wscan(int (*get)(void *), void (*unget)(void *, int), void *ctx,
                 const wchar_t *fmt, va_list ap)
{
    struct scan s = { get, unget, ctx, 0, 0, 0, 0 };
    va_list cp;
    va_copy(cp, ap);
    int r = scan_wide(&s, fmt, &cp);
    va_end(cp);
    return r;
}

static int get_wfile(void *ctx)
{
    wint_t c = wide_getc_raw((FILE *)ctx, NULL);
    return c == WEOF ? EOF : (int)c;
}

static void unget_wfile(void *ctx, int c)
{
    FILE *f = ctx;
    if (c != EOF && f->ungot < 0) {
        f->ungot = c;
        f->flags &= ~F_EOF;
    }
}

struct wstr { const wchar_t *p; size_t i; };

static int get_wstr(void *ctx)
{
    struct wstr *s = ctx;
    return s->p[s->i] ? (int)s->p[s->i++] : EOF;
}

static void unget_wstr(void *ctx, int c)
{
    struct wstr *s = ctx;
    if (c != EOF && s->i)
        s->i--;
}

int vfwscanf(FILE *restrict f, const wchar_t *restrict fmt, va_list ap)
{
    if (!f)
        return EOF;
    __flockfile(f);
    wide_orient(f);
    int r = wscan(get_wfile, unget_wfile, f, fmt, ap);
    __funlockfile(f);
    return r;
}

int vwscanf(const wchar_t *restrict fmt, va_list ap)
{
    return vfwscanf(stdin, fmt, ap);
}

int vswscanf(const wchar_t *restrict s, const wchar_t *restrict fmt,
             va_list ap)
{
    struct wstr src = { s, 0 };
    return wscan(get_wstr, unget_wstr, &src, fmt, ap);
}

int fwscanf(FILE *restrict f, const wchar_t *restrict fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vfwscanf(f, fmt, ap);
    va_end(ap);
    return r;
}

int wscanf(const wchar_t *restrict fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vfwscanf(stdin, fmt, ap);
    va_end(ap);
    return r;
}

int swscanf(const wchar_t *restrict s, const wchar_t *restrict fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vswscanf(s, fmt, ap);
    va_end(ap);
    return r;
}

/* ---- the unlocked forms, GNU's: the lock nests, so these are the
 * locked calls (see stdio/posix.c) -------------------------------------------------- */

__attribute__((weak)) wint_t fgetwc_unlocked(FILE *f) { return fgetwc(f); }
__attribute__((weak)) wint_t getwc_unlocked(FILE *f) { return fgetwc(f); }
__attribute__((weak)) wint_t getwchar_unlocked(void) { return fgetwc(stdin); }
__attribute__((weak)) wint_t fputwc_unlocked(wchar_t c, FILE *f) { return fputwc(c, f); }
__attribute__((weak)) wint_t putwc_unlocked(wchar_t c, FILE *f) { return fputwc(c, f); }
__attribute__((weak)) wint_t putwchar_unlocked(wchar_t c) { return fputwc(c, stdout); }
__attribute__((weak)) wchar_t *fgetws_unlocked(wchar_t *restrict s, int n, FILE *restrict f)
{
    return fgetws(s, n, f);
}
__attribute__((weak)) int fputws_unlocked(const wchar_t *restrict s, FILE *restrict f)
{
    return fputws(s, f);
}
