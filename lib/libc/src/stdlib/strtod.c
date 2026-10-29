/* strtod, strtof, strtold — C11 §7.22.1.3.
 *
 * Decimal text to a binary float, CORRECTLY ROUNDED: the result is the
 * representable value nearest the decimal one, ties to even, for every
 * input, at each type's own precision -- strtof rounds once to float, not
 * to double and then to float, and strtold keeps a long double's 64 or
 * 113 bits.
 *
 * ---- how --------------------------------------------------------------
 *
 * The digits are a big integer D and the input is D x 10^E exactly. With
 * 10^E = 5^E x 2^E, that is num/den x 2^b for big integers num and den
 * (one of them a power of five). The quotient is found a bit at a time --
 * only the bits the type needs, plus a guard bit and whether anything is
 * left over -- so no big division is needed, only shifts, compares and
 * subtractions; and then it is rounded once. Hexadecimal input is the
 * same with den = 1.
 *
 * Digits past the first MAXDIG significant ones only say whether the
 * value is above the truncated one: every midpoint between two values of
 * the type has at most that many significant digits (767 for a double,
 * 11,563 for binary128), so it is either among the kept digits or beyond
 * them, and a nonzero digit past them is a "sticky" bit.
 *
 * The big integers live on the stack while they fit 1 KB -- every input
 * a person writes -- and are allocated beyond that; were that to fail,
 * the answer would come from the arithmetic that preceded this, one unit
 * in the last place off at worst, which C11 7.22.1.3p10 permits.
 */
#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---- big unsigned integers: 32-bit limbs, little end first ---- */

struct big {
    uint32_t *l;
    int n;                       /* limbs in use; 0 is the value 0 */
};

static void big_trim(struct big *b)
{
    while (b->n && !b->l[b->n - 1])
        b->n--;
}

static void big_mul_add(struct big *b, uint32_t m, uint32_t add)
{
    uint64_t carry = add;
    for (int i = 0; i < b->n; i++) {
        uint64_t t = (uint64_t)b->l[i] * m + carry;
        b->l[i] = (uint32_t)t;
        carry = t >> 32;
    }
    if (carry)
        b->l[b->n++] = (uint32_t)carry;
}

static void big_mul_pow5(struct big *b, long k)
{
    for (; k >= 13; k -= 13)
        big_mul_add(b, 1220703125u, 0);              /* 5^13 */
    uint32_t r = 1;
    while (k-- > 0)
        r *= 5;
    if (r != 1)
        big_mul_add(b, r, 0);
}

static int big_bits(const struct big *b)
{
    if (!b->n)
        return 0;
    uint32_t top = b->l[b->n - 1];
    int k = 0;
    while (top) {
        k++;
        top >>= 1;
    }
    return (b->n - 1) * 32 + k;
}

static void big_shl(struct big *b, long k)
{
    if (!b->n || k <= 0)
        return;
    int w = (int)(k / 32), s = (int)(k % 32);
    if (s) {
        uint32_t carry = 0;
        for (int i = 0; i < b->n; i++) {
            uint32_t v = b->l[i];
            b->l[i] = v << s | carry;
            carry = v >> (32 - s);
        }
        if (carry)
            b->l[b->n++] = carry;
    }
    if (w) {
        memmove(b->l + w, b->l, (size_t)b->n * sizeof *b->l);
        memset(b->l, 0, (size_t)w * sizeof *b->l);
        b->n += w;
    }
}

static void big_shr1(struct big *b)
{
    for (int i = 0; i < b->n; i++)
        b->l[i] = b->l[i] >> 1 | (i + 1 < b->n ? b->l[i + 1] << 31 : 0);
    big_trim(b);
}

static int big_cmp(const struct big *a, const struct big *b)
{
    if (a->n != b->n)
        return a->n < b->n ? -1 : 1;
    for (int i = a->n - 1; i >= 0; i--)
        if (a->l[i] != b->l[i])
            return a->l[i] < b->l[i] ? -1 : 1;
    return 0;
}

static void big_sub(struct big *a, const struct big *b)    /* a >= b */
{
    uint64_t borrow = 0;
    for (int i = 0; i < a->n; i++) {
        uint64_t t = (uint64_t)a->l[i] - (i < b->n ? b->l[i] : 0) - borrow;
        a->l[i] = (uint32_t)t;
        borrow = t >> 63;
    }
    big_trim(a);
}

/* ---- the type being produced ---- */

struct fmt {
    int p;                       /* significand bits */
    int emin;                    /* the least normal is 2^emin */
    int emax;                    /* the greatest finite is below 2^(emax+1) */
    int maxdig;                  /* significant digits that can matter */
};

static const struct fmt F_FLT = { FLT_MANT_DIG, FLT_MIN_EXP - 1,
                                  FLT_MAX_EXP - 1, 120 };
static const struct fmt F_DBL = { DBL_MANT_DIG, DBL_MIN_EXP - 1,
                                  DBL_MAX_EXP - 1,
                                  DBL_MANT_DIG > 24 ? 800 : 120 };
static const struct fmt F_LDBL = { LDBL_MANT_DIG, LDBL_MIN_EXP - 1,
                                   LDBL_MAX_EXP - 1,
                                   LDBL_MANT_DIG > 53 ? 11600
                                   : LDBL_MANT_DIG > 24 ? 800 : 120 };

/* kept x 2^e2 as a long double: exact, kept having at most f->p bits */
static long double build(const uint32_t q[5], long e2, const struct fmt *f)
{
    long double v = 0;
    for (int i = 4; i >= 0; i--)
        v = v * 4294967296.0L + q[i];
    if (f->p <= DBL_MANT_DIG)
        return ldexp((double)v, (int)e2);
    return ldexpl(v, (int)e2);
}

/* num/den x 2^b, and above that by less than a unit of den when sticky,
 * rounded to the type: 0, or ERANGE for an overflow or an inexact result
 * below the normal range. num and den have room for the shifts. */
static int round_q(struct big *num, struct big *den, long b, int sticky,
                   const struct fmt *f, long double *out)
{
    int p = f->p;
    long sh = (long)p + 3 - (big_bits(num) - big_bits(den));
    if (sh > 0)
        big_shl(num, sh);
    else
        big_shl(den, -sh);
    b -= sh;

    /* now 2^(p+2) <= num/den < 2^(p+4): the quotient a bit at a time */
    uint32_t q[5] = { 0, 0, 0, 0, 0 };
    big_shl(den, p + 4);
    for (int i = p + 4; i >= 0; i--) {
        if (big_cmp(num, den) >= 0) {
            big_sub(num, den);
            q[i / 32] |= 1u << (i % 32);
        }
        big_shr1(den);
    }
    sticky |= num->n != 0;

    int qbits = 0;
    for (int i = 4 * 32 + 31; i >= 0; i--)
        if (q[i / 32] >> (i % 32) & 1) {
            qbits = i + 1;
            break;
        }
    long lead = qbits - 1 + b;
    if (lead > f->emax) {
        *out = HUGE_VALL;
        return ERANGE;
    }
    /* the bits to drop: down to p of them, or down to the least
     * subnormal's place */
    long drop = lead >= f->emin ? qbits - p : (f->emin - p + 1) - b;
    int half = 0, rest = sticky;
    for (long i = 0; i < drop && i < 160; i++) {
        int bit = q[i / 32] >> (i % 32) & 1;
        if (i == drop - 1)
            half = bit;
        else
            rest |= bit;
    }
    /* q >>= drop */
    uint32_t k[5] = { 0, 0, 0, 0, 0 };
    if (drop < 160)
        for (long i = drop; i < 160; i++)
            if (q[i / 32] >> (i % 32) & 1)
                k[(i - drop) / 32] |= 1u << ((i - drop) % 32);
    if (half && (rest || (k[0] & 1))) {
        for (int i = 0; i < 5 && !++k[i]; i++)
            ;
        /* a carry to 2^p: one bit more, the exponent one more */
        if (k[p / 32] >> (p % 32) & 1) {
            for (int i = 0; i < 5; i++)
                k[i] = k[i] >> 1 | (i < 4 ? k[i + 1] << 31 : 0);
            drop++;
            if (lead + 1 > f->emax) {
                *out = HUGE_VALL;
                return ERANGE;
            }
        }
    }
    *out = build(k, b + drop, f);
    return lead < f->emin && (half || rest) ? ERANGE : 0;
}

/* ---- the syntax ---- */

/* The significant digits of [p, end) -- leading zeros already skipped,
 * one '.' among them -- in base 10 or 16, into a big integer: at most
 * maxdig of them, the rest only a sticky bit. Returns the count kept. */
static int read_digits(const char *p, const char *end, int hex, int maxdig,
                       struct big *d, int *sticky)
{
    uint32_t chunk = 0, scale = 1, full = hex ? 0x10000000u : 1000000000u;
    int n = 0;
    *sticky = 0;
    d->n = 0;
    for (; p < end; p++) {
        if (*p == '.')
            continue;
        int v = isdigit((unsigned char)*p) ? *p - '0'
              : tolower((unsigned char)*p) - 'a' + 10;
        if (n == maxdig) {
            if (v)
                *sticky = 1;
            continue;
        }
        chunk = chunk * (hex ? 16 : 10) + (uint32_t)v;
        scale *= hex ? 16 : 10;
        n++;
        if (scale == full) {
            big_mul_add(d, scale, chunk);
            chunk = 0;
            scale = 1;
        }
    }
    if (scale != 1)
        big_mul_add(d, scale, chunk);
    return n;
}

/* The decimal or hexadecimal number at s, after the sign, converted
 * exactly for the format f. Returns the end of what was read, s when
 * nothing was; *err is ERANGE when the result over- or underflowed. */
static const char *number(const char *s, const struct fmt *f, int neg,
                          long double *out, int *err)
{
    const char *p = s;
    int hex = 0;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X') &&
        (isxdigit((unsigned char)p[2]) ||
         (p[2] == '.' && isxdigit((unsigned char)p[3])))) {
        hex = 1;
        p += 2;
    }

    /* the digits: leading zeros skipped, the point remembered */
    const char *dstart = NULL, *dend, *point = NULL;
    long intdig = 0;             /* significant digits before the point */
    long lead_frac_zeros = 0;    /* zeros after the point before any */
    int any = 0, seen_point = 0;
    for (;; p++) {
        if (*p == '.' && !seen_point) {
            seen_point = 1;
            point = p;
            continue;
        }
        if (!(hex ? isxdigit((unsigned char)*p) : isdigit((unsigned char)*p)))
            break;
        any = 1;
        if (!dstart) {
            if (*p == '0') {
                if (seen_point)
                    lead_frac_zeros++;
                continue;
            }
            dstart = p;
        }
        if (!seen_point)
            intdig++;
    }
    dend = p;
    if (!any)
        return s;

    long ex = 0;
    if ((!hex && (*p == 'e' || *p == 'E')) || (hex && (*p == 'p' || *p == 'P'))) {
        const char *q = p + 1;
        int eneg = 0;
        if (*q == '+' || *q == '-')
            eneg = *q++ == '-';
        if (isdigit((unsigned char)*q)) {
            for (; isdigit((unsigned char)*q); q++)
                if (ex < 100000000)
                    ex = ex * 10 + (*q - '0');
            ex = eneg ? -ex : ex;
            p = q;
        }
        /* "1e" with no digits: the 'e' is not part of the number. The
         * binary exponent of a hex float is optional too: strtod reads
         * "0x1" as 1, where a C source constant needs the p. */
    }

    *err = 0;
    if (!dstart) {               /* all zeros */
        *out = neg ? -0.0L : 0.0L;
        return p;
    }

    /* Where the value is, in powers of the base, before any arithmetic:
     * far past the range is infinity or zero without it. */
    long mag;                    /* value is below base^mag */
    if (hex)
        mag = 4 * (intdig ? intdig : -lead_frac_zeros) + ex;
    else
        mag = (intdig ? intdig : -lead_frac_zeros) + ex;
    long top = hex ? f->emax + 5 : (long)(f->emax + 1) * 30103 / 100000 + 2;
    long bottom = hex ? f->emin - f->p
                      : (long)(f->emin - f->p - 1) * 30103 / 100000 - 2;
    if (mag > top) {
        *out = neg ? -HUGE_VALL : HUGE_VALL;
        *err = ERANGE;
        return p;
    }
    if (mag < bottom) {
        *out = neg ? -0.0L : 0.0L;
        *err = ERANGE;
        return p;
    }

    /* Room for the integers: the digits' bits, a power of five, and the
     * shifts round_q makes -- each of num and den needs at most the bits
     * of both, plus two significands. */
    long ndig = (long)(dend - dstart) - (point && point > dstart);
    int maxdig = hex ? (f->p + 8) / 4 + 2 : f->maxdig;
    long kd = ndig < maxdig ? ndig : maxdig;
    long e = (intdig ? intdig : -lead_frac_zeros) - kd;   /* in digits */
    long ae = e + ex < 0 ? -(e + ex) : e + ex;
    long bits = hex ? kd * 4 + 2L * f->p + 64
                    : kd * 4 + ae * 5 + 2L * f->p + 64;
    int cap = (int)(bits / 32) + 8;
    uint32_t stackbuf[256], *mem = stackbuf;
    if (2 * cap > 256) {
        mem = malloc((size_t)2 * (size_t)cap * sizeof *mem);
        if (!mem)
            return NULL;         /* the caller falls back */
    }
    memset(mem, 0, (size_t)2 * (size_t)cap * sizeof *mem);
    struct big num = { mem, 0 }, den = { mem + cap, 1 };
    den.l[0] = 1;

    int sticky;
    read_digits(dstart, dend, hex, maxdig, &num, &sticky);
    if (hex) {
        /* the last kept digit is worth 16^(intdig - kd), the exponent is
         * a power of two already */
        *err = round_q(&num, &den, 4 * e + ex, sticky, f, out);
    } else {
        long ee = e + ex;        /* value = num x 10^ee, 10^ee = 5^ee 2^ee */
        if (ee >= 0)
            big_mul_pow5(&num, ee);
        else
            big_mul_pow5(&den, -ee);
        *err = round_q(&num, &den, ee, sticky, f, out);
    }
    if (mem != stackbuf)
        free(mem);
    if (neg)
        *out = -*out;
    return p;
}

/* The arithmetic before this file was exact, for the fallback when the
 * big integers could not be allocated: the digits' first 19, scaled. */
static long double approx(const char *s, int neg)
{
    long double m = 0, scale = 1;
    long e = 0;
    int n = 0, point = 0;
    const char *p = s;
    for (; isdigit((unsigned char)*p) || (*p == '.' && !point); p++) {
        if (*p == '.') { point = 1; continue; }
        if (n < 19) { m = m * 10 + (*p - '0'); if (m) n++; if (point) e--; }
        else if (!point) e++;
    }
    if (*p == 'e' || *p == 'E')
        e += strtol(p + 1, NULL, 10);
    long k = e < 0 ? -e : e;
    for (long double t = 10; k; k >>= 1, t *= t)
        if (k & 1)
            scale *= t;
    m = e < 0 ? m / scale : m * scale;
    return neg ? -m : m;
}

static long double conv(const char *s, char **endp, const struct fmt *f)
{
    const char *p = s;
    while (isspace((unsigned char)*p))
        p++;

    int neg = 0;
    if (*p == '+' || *p == '-')
        neg = *p++ == '-';

    /* inf / infinity / nan, case-insensitively, before the digits: they
     * are values a printf can produce, and a strtod that cannot read back
     * what printf wrote is a round trip that silently loses data. */
    if (tolower((unsigned char)p[0]) == 'i' &&
        tolower((unsigned char)p[1]) == 'n' &&
        tolower((unsigned char)p[2]) == 'f') {
        p += 3;
        if (tolower((unsigned char)p[0]) == 'i' &&
            tolower((unsigned char)p[1]) == 'n' &&
            tolower((unsigned char)p[2]) == 'i' &&
            tolower((unsigned char)p[3]) == 't' &&
            tolower((unsigned char)p[4]) == 'y')
            p += 5;
        if (endp)
            *endp = (char *)p;
        return neg ? -HUGE_VALL : HUGE_VALL;
    }
    if (tolower((unsigned char)p[0]) == 'n' &&
        tolower((unsigned char)p[1]) == 'a' &&
        tolower((unsigned char)p[2]) == 'n') {
        p += 3;
        if (*p == '(') {                 /* nan(chars): the payload */
            const char *q = p + 1;
            while (isalnum((unsigned char)*q) || *q == '_')
                q++;
            if (*q == ')')
                p = q + 1;
        }
        if (endp)
            *endp = (char *)p;
        return neg ? -__builtin_nanl("") : __builtin_nanl("");
    }

    long double v = 0;
    int err = 0;
    const char *e = number(p, f, neg, &v, &err);
    if (!e) {
        /* no memory for the exact arithmetic */
        v = approx(p, neg);
        e = p;
        while (isdigit((unsigned char)*e) || *e == '.') e++;
        if (*e == 'e' || *e == 'E') strtol(e + 1, (char **)&e, 10);
    } else if (e == p) {
        if (endp)
            *endp = (char *)s;           /* nothing converted */
        return 0;
    }
    /* Overflow and underflow are reported, not silently returned: a
     * caller that cannot tell HUGE_VAL from a number that happened to be
     * huge has no way to validate its input. */
    if (err)
        errno = err;
    if (endp)
        *endp = (char *)e;
    return v;
}

double strtod(const char *__restrict s, char **__restrict end)
{
    return (double)conv(s, end, &F_DBL);
}

float strtof(const char *__restrict s, char **__restrict end)
{
    return (float)conv(s, end, &F_FLT);
}

long double strtold(const char *__restrict s, char **__restrict end)
{
    return conv(s, end, &F_LDBL);
}

double atof(const char *s) { return strtod(s, NULL); }
