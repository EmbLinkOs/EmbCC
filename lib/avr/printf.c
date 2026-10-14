/* The formatted-output functions of the AVR library (lib/avr): sprintf,
 * snprintf, vsprintf, vsnprintf, and avr-libc's _P forms of each, whose
 * format string is in flash.
 *
 * Not lib/libc/src/stdio: that formatter prints every double exactly,
 * which takes 17 KB of AVR code, and its streams keep 8 KB of buffers --
 * more than an ATmega328P has of flash and of SRAM. This one is the size
 * of avr-libc's default vfprintf, and does what it does:
 *
 *   flags - + space # 0, a width and a precision (either may be *), the
 *   length modifiers hh h l ll z t j, and the conversions d i u o x X c s
 *   p n %, with avr-libc's %S, a string in flash. A floating-point
 *   conversion (e f g E F G a A) takes its double and prints "?", as
 *   avr-libc's does unless linked with -lprintf_flt.
 *
 * Numbers are converted a byte at a time (8- by 16-bit division), so a
 * long long costs no 64-bit division routine. Each entry point is an
 * object of its own (tools/build-rt.sh builds each EMB_AVR_ name alone),
 * and the engine, __emb_avr_vformat, another. */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct __emb_avr_out {
    char *buf;
    size_t cap;         /* bytes the buffer holds, the terminator among them */
    size_t n;           /* characters produced, written or not */
};

int __emb_avr_vformat(struct __emb_avr_out *o, const char *fmt, int pgm,
                      va_list ap);

#ifdef EMB_AVR_VFORMAT

static void put(struct __emb_avr_out *o, char c)
{
    if (o->n + 1 < o->cap)
        o->buf[o->n] = c;
    o->n++;
}

static void pad(struct __emb_avr_out *o, char c, int n)
{
    while (n-- > 0)
        put(o, c);
}

/* A format character, from RAM or from flash. */
static char at(const char *p, int pgm)
{
    return pgm ? *(const __flash char *)(uint16_t)p : *p;
}

/* Divides the little-endian number v[0..nb) by base in place and returns
 * the remainder. */
static uint8_t divmod(uint8_t *v, uint8_t nb, uint8_t base)
{
    uint16_t r = 0;
    uint8_t i = nb;
    while (i--) {
        r = (uint16_t)((r << 8) | v[i]);
        v[i] = (uint8_t)(r / base);
        r = (uint16_t)(r % base);
    }
    return (uint8_t)r;
}

static int is_zero(const uint8_t *v, uint8_t nb)
{
    while (nb--)
        if (v[nb])
            return 0;
    return 1;
}

enum { F_MINUS = 1, F_PLUS = 2, F_SPACE = 4, F_HASH = 8, F_ZERO = 16 };

int __emb_avr_vformat(struct __emb_avr_out *o, const char *fmt, int pgm,
                      va_list ap)
{
    for (;;) {
        char c = at(fmt++, pgm);
        if (!c)
            break;
        if (c != '%') {
            put(o, c);
            continue;
        }
        int flags = 0, width = 0, prec = -1, len = 0;   /* len: -2 hh .. 2 ll */
        for (;; fmt++) {
            c = at(fmt, pgm);
            if (c == '-') flags |= F_MINUS;
            else if (c == '+') flags |= F_PLUS;
            else if (c == ' ') flags |= F_SPACE;
            else if (c == '#') flags |= F_HASH;
            else if (c == '0') flags |= F_ZERO;
            else break;
        }
        if (at(fmt, pgm) == '*') {
            fmt++;
            width = va_arg(ap, int);
            if (width < 0) {
                flags |= F_MINUS;
                width = -width;
            }
        } else {
            while ((c = at(fmt, pgm)) >= '0' && c <= '9') {
                width = width * 10 + (c - '0');
                fmt++;
            }
        }
        if (at(fmt, pgm) == '.') {
            fmt++;
            prec = 0;
            if (at(fmt, pgm) == '*') {
                fmt++;
                prec = va_arg(ap, int);
                if (prec < 0)
                    prec = -1;
            } else {
                while ((c = at(fmt, pgm)) >= '0' && c <= '9') {
                    prec = prec * 10 + (c - '0');
                    fmt++;
                }
            }
        }
        for (;; fmt++) {
            c = at(fmt, pgm);
            if (c == 'h') len = len < 0 ? -2 : -1;
            else if (c == 'l') len = len > 0 ? 2 : 1;
            else if (c == 'z' || c == 't') len = 0;   /* size_t is an int here */
            else if (c == 'j') len = 2;
            else break;
        }
        c = at(fmt++, pgm);
        switch (c) {
        case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': case 'p': {
            uint8_t v[8] = { 0 }, nb;
            int neg = 0;
            char sign = 0;
            const char *pfx = "";
            uint8_t base = c == 'o' ? 8 : (c == 'x' || c == 'X' || c == 'p') ? 16 : 10;
            if (c == 'p') {
                uint16_t u = (uint16_t)(uintptr_t)va_arg(ap, void *);
                v[0] = (uint8_t)u;
                v[1] = (uint8_t)(u >> 8);
                nb = 2;
                flags |= F_HASH;
            } else if (len == 2) {
                unsigned long long u = va_arg(ap, unsigned long long);
                for (nb = 0; nb < 8; nb++)
                    v[nb] = (uint8_t)(u >> (8 * nb));
            } else if (len == 1) {
                unsigned long u = va_arg(ap, unsigned long);
                for (nb = 0; nb < 4; nb++)
                    v[nb] = (uint8_t)(u >> (8 * nb));
            } else {
                unsigned u = va_arg(ap, unsigned);
                if (len == -1)
                    u = (c == 'd' || c == 'i') ? (unsigned)(int)(short)u
                                               : (unsigned)(unsigned short)u;
                if (len == -2)
                    u = (c == 'd' || c == 'i') ? (unsigned)(int)(signed char)u
                                               : (unsigned)(unsigned char)u;
                v[0] = (uint8_t)u;
                v[1] = (uint8_t)(u >> 8);
                nb = 2;
            }
            if ((c == 'd' || c == 'i') && (v[nb - 1] & 0x80)) {
                /* negative: the magnitude, by two's complement */
                uint8_t k, carry = 1;
                neg = 1;
                for (k = 0; k < nb; k++) {
                    uint16_t t = (uint16_t)((uint8_t)~v[k] + carry);
                    v[k] = (uint8_t)t;
                    carry = (uint8_t)(t >> 8);
                }
            }
            if (c == 'd' || c == 'i')
                sign = neg ? '-' : (flags & F_PLUS) ? '+' : (flags & F_SPACE) ? ' ' : 0;
            int zero = is_zero(v, nb);
            char dg[24];
            int nd = 0;
            if (!(zero && prec == 0)) {
                const char *digits = c == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
                do {
                    dg[nd++] = digits[divmod(v, nb, base)];
                } while (!is_zero(v, nb));
            }
            if ((flags & F_HASH) && !zero && base == 16)
                pfx = c == 'X' ? "0X" : "0x";
            if ((flags & F_HASH) && base == 8 && (prec <= nd))
                prec = nd + 1;              /* a leading 0 */
            int body = nd > prec ? nd : prec;
            int npfx = pfx[0] ? 2 : 0;
            int total = body + npfx + (sign != 0);
            int zpad = 0;
            if ((flags & F_ZERO) && !(flags & F_MINUS) && prec < 0 && width > total)
                zpad = width - total;
            if (!(flags & F_MINUS))
                pad(o, ' ', width - total - zpad);
            if (sign)
                put(o, sign);
            for (int k = 0; k < npfx; k++)
                put(o, pfx[k]);
            pad(o, '0', zpad + body - nd);
            while (nd)
                put(o, dg[--nd]);
            if (flags & F_MINUS)
                pad(o, ' ', width - total);
            break;
        }
        case 'c':
            if (!(flags & F_MINUS))
                pad(o, ' ', width - 1);
            put(o, (char)va_arg(ap, int));
            if (flags & F_MINUS)
                pad(o, ' ', width - 1);
            break;
        case 's': case 'S': {
            const char *s = va_arg(ap, const char *);
            int fl = c == 'S';
            int n = 0;
            if (!s) {
                s = "(null)";
                fl = 0;
            }
            while ((prec < 0 || n < prec) && at(s + n, fl))
                n++;
            if (!(flags & F_MINUS))
                pad(o, ' ', width - n);
            for (int k = 0; k < n; k++)
                put(o, at(s + k, fl));
            if (flags & F_MINUS)
                pad(o, ' ', width - n);
            break;
        }
        case 'n': {
            int *q = va_arg(ap, int *);
            if (q)
                *q = (int)o->n;
            break;
        }
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G':
        case 'a': case 'A':
            (void)va_arg(ap, double);
            if (!(flags & F_MINUS))
                pad(o, ' ', width - 1);
            put(o, '?');
            if (flags & F_MINUS)
                pad(o, ' ', width - 1);
            break;
        case 0:
            return (int)o->n;               /* a trailing '%' */
        default:
            put(o, c);                      /* %% and anything unknown */
            break;
        }
    }
    return (int)o->n;
}
#endif /* EMB_AVR_VFORMAT */

#ifndef EMB_AVR_VFORMAT
/* Into a buffer of `cap` bytes, terminated whenever there is room. */
static int to_buf(char *s, size_t cap, const char *fmt, int pgm, va_list ap)
{
    struct __emb_avr_out o = { s, cap, 0 };
    int r = __emb_avr_vformat(&o, fmt, pgm, ap);
    if (cap)
        s[o.n < cap ? o.n : cap - 1] = 0;
    return r;
}

#endif

/* What vsprintf can write: as much as the data space holds. */
#define NOLIMIT ((size_t)0x7fff)

#ifdef EMB_AVR_VSNPRINTF
int vsnprintf(char *__restrict s, size_t n, const char *__restrict fmt, va_list ap)
{
    return to_buf(s, n, fmt, 0, ap);
}
#endif

#ifdef EMB_AVR_VSPRINTF
int vsprintf(char *__restrict s, const char *__restrict fmt, va_list ap)
{
    return to_buf(s, NOLIMIT, fmt, 0, ap);
}
#endif

#ifdef EMB_AVR_SNPRINTF
int snprintf(char *__restrict s, size_t n, const char *__restrict fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = to_buf(s, n, fmt, 0, ap);
    va_end(ap);
    return r;
}
#endif

#ifdef EMB_AVR_SPRINTF
int sprintf(char *__restrict s, const char *__restrict fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = to_buf(s, NOLIMIT, fmt, 0, ap);
    va_end(ap);
    return r;
}
#endif

#ifdef EMB_AVR_VSNPRINTF_P
int vsnprintf_P(char *s, size_t n, const char *fmt, va_list ap)
{
    return to_buf(s, n, fmt, 1, ap);
}
#endif

#ifdef EMB_AVR_VSPRINTF_P
int vsprintf_P(char *s, const char *fmt, va_list ap)
{
    return to_buf(s, NOLIMIT, fmt, 1, ap);
}
#endif

#ifdef EMB_AVR_SNPRINTF_P
int snprintf_P(char *s, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = to_buf(s, n, fmt, 1, ap);
    va_end(ap);
    return r;
}
#endif

#ifdef EMB_AVR_SPRINTF_P
int sprintf_P(char *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = to_buf(s, NOLIMIT, fmt, 1, ap);
    va_end(ap);
    return r;
}
#endif
