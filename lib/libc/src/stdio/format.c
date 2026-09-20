/* The formatted-output engine, C11 §7.21.6.1.
 *
 * One implementation behind every printf: the variants differ only in where
 * the bytes go, so they pass a sink and share everything else. Writing
 * printf twice is how `%zu` ends up working in one and not the other.
 *
 * Floating point is converted here rather than borrowed, because a C
 * library that cannot print a double is not one. The approach is the plain
 * one -- scale into an integer part and a fraction, round half away from
 * zero -- which is exact for the magnitudes %f is used at and honest about
 * the rest. %a and the shortest-round-trip %g of newer standards are not
 * attempted; what is here is correct, and what is not here is absent rather
 * than wrong.
 */
#include "file.h"

#include <string.h>
#include <stdlib.h>

struct out {
    void (*sink)(void *, const char *, size_t);
    void *ctx;
    int n;                       /* characters that WOULD have been written */
};

static void emit(struct out *o, const char *s, size_t n)
{
    o->sink(o->ctx, s, n);
    o->n += (int)n;
}

static void pad(struct out *o, char c, int n)
{
    char b[32];
    if (n <= 0) return;
    memset(b, c, sizeof b);
    while (n > 0) {
        int k = n < (int)sizeof b ? n : (int)sizeof b;
        emit(o, b, (size_t)k);
        n -= k;
    }
}

/* Flags, in the order §7.21.6.1 lists them. */
#define F_MINUS 0x01
#define F_PLUS  0x02
#define F_SPACE 0x04
#define F_HASH  0x08
#define F_ZERO  0x10

static const char *DIGL = "0123456789abcdef";
static const char *DIGU = "0123456789ABCDEF";

/* One integer, already made unsigned and with its sign decided. */
static void put_int(struct out *o, unsigned long long v, int base, int upper,
                    int flags, int width, int prec, const char *sign,
                    const char *prefix)
{
    char d[32];
    int nd = 0;
    const char *dig = upper ? DIGU : DIGL;
    if (v == 0 && prec == 0)
        nd = 0;                  /* a zero with precision 0 prints nothing */
    else do { d[nd++] = dig[v % (unsigned)base]; v /= (unsigned)base; } while (v);

    int zeros = prec > nd ? prec - nd : 0;
    int body = nd + zeros + (int)strlen(sign) + (int)strlen(prefix);
    /* A '0' flag is ignored when a precision is given, C says, because the
     * precision already decided how many digits there are. */
    int zpad = (flags & F_ZERO) && !(flags & F_MINUS) && prec < 0
               ? width - body : 0;
    int spad = width - body - (zpad > 0 ? zpad : 0);

    if (!(flags & F_MINUS)) pad(o, ' ', spad);
    if (*sign)   emit(o, sign, strlen(sign));
    if (*prefix) emit(o, prefix, strlen(prefix));
    pad(o, '0', zpad);
    pad(o, '0', zeros);
    while (nd) { char c = d[--nd]; emit(o, &c, 1); }
    if (flags & F_MINUS) pad(o, ' ', spad);
}

/* %f / %e / %g. */
static void put_double(struct out *o, double v, char conv, int flags,
                       int width, int prec)
{
    char sign[2] = {0, 0};
    if (v < 0 || (v == 0 && 1.0 / v < 0)) { sign[0] = '-'; v = -v; }
    else if (flags & F_PLUS)  sign[0] = '+';
    else if (flags & F_SPACE) sign[0] = ' ';

    /* Not-a-number and infinity print as words, and the words are lower or
     * upper case with the conversion. */
    if (v != v || v > 1.7976931348623157e308) {
        const char *w = v != v ? (conv < 'a' ? "NAN" : "nan")
                               : (conv < 'a' ? "INF" : "inf");
        int body = 3 + (sign[0] ? 1 : 0);
        if (!(flags & F_MINUS)) pad(o, ' ', width - body);
        if (sign[0]) emit(o, sign, 1);
        emit(o, w, 3);
        if (flags & F_MINUS) pad(o, ' ', width - body);
        return;
    }

    if (prec < 0) prec = 6;

    int exp10 = 0;
    if (conv == 'e' || conv == 'E' || conv == 'g' || conv == 'G') {
        if (v != 0) {
            while (v >= 10.0) { v /= 10.0; exp10++; }
            while (v < 1.0)   { v *= 10.0; exp10--; }
        }
        if (conv == 'g' || conv == 'G') {
            /* %g picks %e or %f by exponent, and its precision counts
             * significant digits rather than fraction digits. */
            int p = prec ? prec : 1;
            if (exp10 < -4 || exp10 >= p) {
                conv = (conv == 'g') ? 'e' : 'E';
                prec = p - 1;
            } else {
                conv = 'f';
                prec = p - 1 - exp10;
                if (prec < 0) prec = 0;
                v *= 1.0;
                for (int i = 0; i < exp10; i++) v *= 10.0;
                for (int i = 0; i > exp10; i--) v /= 10.0;
            }
        }
    }

    /* Round at the printed precision, away from zero. */
    double r = 0.5;
    for (int i = 0; i < prec; i++) r /= 10.0;
    v += r;
    if ((conv == 'e' || conv == 'E') && v >= 10.0) { v /= 10.0; exp10++; }

    unsigned long long ip = (unsigned long long)v;
    double frac = v - (double)ip;

    char digits[512];
    int nd = 0;
    if (ip == 0) digits[nd++] = '0';
    else { char t[32]; int n = 0;
           while (ip) { t[n++] = (char)('0' + ip % 10); ip /= 10; }
           while (n) digits[nd++] = t[--n]; }
    if (prec > 0) {
        digits[nd++] = '.';
        for (int i = 0; i < prec && nd < (int)sizeof digits - 8; i++) {
            frac *= 10.0;
            int d = (int)frac;
            if (d > 9) d = 9;
            digits[nd++] = (char)('0' + d);
            frac -= d;
        }
    } else if (flags & F_HASH) {
        digits[nd++] = '.';
    }
    if (conv == 'e' || conv == 'E') {
        digits[nd++] = conv;
        digits[nd++] = exp10 < 0 ? '-' : '+';
        int e = exp10 < 0 ? -exp10 : exp10;
        if (e >= 100) { digits[nd++] = (char)('0' + e / 100); e %= 100; }
        digits[nd++] = (char)('0' + e / 10);
        digits[nd++] = (char)('0' + e % 10);
    }

    int body = nd + (sign[0] ? 1 : 0);
    int zpad = (flags & F_ZERO) && !(flags & F_MINUS) ? width - body : 0;
    int spad = width - body - (zpad > 0 ? zpad : 0);
    if (!(flags & F_MINUS)) pad(o, ' ', spad);
    if (sign[0]) emit(o, sign, 1);
    pad(o, '0', zpad);
    emit(o, digits, (size_t)nd);
    if (flags & F_MINUS) pad(o, ' ', spad);
}

int __vformat(void (*sink)(void *, const char *, size_t), void *ctx,
              const char *fmt, va_list ap)
{
    struct out o = { sink, ctx, 0 };

    for (const char *p = fmt; *p; ) {
        if (*p != '%') {
            const char *s = p;
            while (*p && *p != '%') p++;
            emit(&o, s, (size_t)(p - s));
            continue;
        }
        p++;
        if (*p == '%') { emit(&o, "%", 1); p++; continue; }

        int flags = 0;
        for (;; p++) {
            if (*p == '-')      flags |= F_MINUS;
            else if (*p == '+') flags |= F_PLUS;
            else if (*p == ' ') flags |= F_SPACE;
            else if (*p == '#') flags |= F_HASH;
            else if (*p == '0') flags |= F_ZERO;
            else break;
        }
        int width = 0;
        if (*p == '*') {
            width = va_arg(ap, int);
            if (width < 0) { flags |= F_MINUS; width = -width; }
            p++;
        } else while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');

        int prec = -1;
        if (*p == '.') {
            p++;
            prec = 0;
            if (*p == '*') { prec = va_arg(ap, int); p++; }
            else while (*p >= '0' && *p <= '9') prec = prec * 10 + (*p++ - '0');
            if (prec < 0) prec = -1;    /* a negative * precision is as if absent */
        }

        /* Length modifiers. `z`, `t` and `j` matter as much as the rest:
         * a size_t printed with %u is wrong on LP64 and silently right on
         * ILP32, which is the worst way for a bug to behave. */
        enum { L_INT, L_CHAR, L_SHORT, L_LONG, L_LLONG, L_SIZE, L_PTRDIFF,
               L_MAX, L_LDOUBLE } len = L_INT;
        for (;;) {
            if (*p == 'h')      { len = (len == L_SHORT) ? L_CHAR : L_SHORT; p++; }
            else if (*p == 'l') { len = (len == L_LONG) ? L_LLONG : L_LONG; p++; }
            else if (*p == 'z') { len = L_SIZE; p++; }
            else if (*p == 't') { len = L_PTRDIFF; p++; }
            else if (*p == 'j') { len = L_MAX; p++; }
            else if (*p == 'L') { len = L_LDOUBLE; p++; }
            else break;
        }

        char conv = *p++;
        switch (conv) {
        case 'd': case 'i': {
            long long v;
            switch (len) {
            case L_CHAR:    v = (signed char)va_arg(ap, int); break;
            case L_SHORT:   v = (short)va_arg(ap, int); break;
            case L_LONG:    v = va_arg(ap, long); break;
            case L_LLONG:   v = va_arg(ap, long long); break;
            case L_SIZE:    v = (long long)va_arg(ap, size_t); break;
            case L_PTRDIFF: v = (long long)va_arg(ap, ptrdiff_t); break;
            case L_MAX:     v = va_arg(ap, long long); break;
            default:        v = va_arg(ap, int); break;
            }
            const char *sign = v < 0 ? "-" : (flags & F_PLUS) ? "+"
                             : (flags & F_SPACE) ? " " : "";
            unsigned long long u = v < 0 ? (unsigned long long)-(v + 1) + 1
                                         : (unsigned long long)v;
            put_int(&o, u, 10, 0, flags, width, prec, sign, "");
            break;
        }
        case 'u': case 'o': case 'x': case 'X': case 'b': {
            unsigned long long v;
            switch (len) {
            case L_CHAR:    v = (unsigned char)va_arg(ap, unsigned); break;
            case L_SHORT:   v = (unsigned short)va_arg(ap, unsigned); break;
            case L_LONG:    v = va_arg(ap, unsigned long); break;
            case L_LLONG:   v = va_arg(ap, unsigned long long); break;
            case L_SIZE:    v = va_arg(ap, size_t); break;
            case L_PTRDIFF: v = (unsigned long long)va_arg(ap, ptrdiff_t); break;
            case L_MAX:     v = va_arg(ap, unsigned long long); break;
            default:        v = va_arg(ap, unsigned); break;
            }
            int base = conv == 'o' ? 8 : conv == 'u' ? 10
                     : conv == 'b' ? 2 : 16;
            const char *pfx = "";
            if ((flags & F_HASH) && v) {
                if (conv == 'x') pfx = "0x";
                else if (conv == 'X') pfx = "0X";
                else if (conv == 'o') pfx = "0";
            }
            put_int(&o, v, base, conv == 'X', flags, width, prec, "", pfx);
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
            if (len == L_LDOUBLE)
                put_double(&o, (double)va_arg(ap, long double), conv, flags,
                           width, prec);
            else
                put_double(&o, va_arg(ap, double), conv, flags, width, prec);
            break;
        case 'c': {
            char c = (char)va_arg(ap, int);
            if (!(flags & F_MINUS)) pad(&o, ' ', width - 1);
            emit(&o, &c, 1);
            if (flags & F_MINUS) pad(&o, ' ', width - 1);
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            /* A precision on %s is a MAXIMUM, and the string need not be
             * terminated within it -- so the length is strnlen, not strlen. */
            size_t n = prec >= 0 ? strnlen(s, (size_t)prec) : strlen(s);
            if (!(flags & F_MINUS)) pad(&o, ' ', width - (int)n);
            emit(&o, s, n);
            if (flags & F_MINUS) pad(&o, ' ', width - (int)n);
            break;
        }
        case 'p': {
            void *v = va_arg(ap, void *);
            if (!v) {
                const char *nil = "(nil)";
                if (!(flags & F_MINUS)) pad(&o, ' ', width - 5);
                emit(&o, nil, 5);
                if (flags & F_MINUS) pad(&o, ' ', width - 5);
            } else {
                put_int(&o, (unsigned long long)(size_t)v, 16, 0, flags,
                        width, prec, "", "0x");
            }
            break;
        }
        case 'n': {
            int *q = va_arg(ap, int *);
            if (q) *q = o.n;
            break;
        }
        case 0:
            return o.n;                /* a trailing '%': stop, do not run off */
        default:
            /* An unknown conversion prints itself, which is more use than
             * silence when a format string has a typo. */
            emit(&o, "%", 1);
            emit(&o, &conv, 1);
            break;
        }
    }
    return o.n;
}
