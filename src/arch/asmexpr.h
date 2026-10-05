/* A constant expression in an assembly operand, as GNU as reads one.
 *
 * Shared by the Cortex-M and RISC-V instruction assemblers, which see
 * one statement at a time and no symbols: C's integer literals (decimal,
 * 0x, 0b, octal, u/l suffixes) and C's integer operators
 * (~ * / % + - << >> & ^ |) with parentheses, in long long. A name, or
 * anything else, makes it not an expression -- the caller reports that.
 * Division by zero is not a value. Static functions in a header, so that
 * neither assembler grows a file the build lists must name.
 *
 * FreeRTOS's ports are what need it: ARM_CM4F enables the FPU with
 * `orr r1, r1, #( 0xf << 20 )`, and the RISC-V port makes its frame with
 * `addi sp, sp, -( portCONTEXT_SIZE )` and addresses it with
 * `1 * portWORD_SIZE( sp )`. */
#ifndef EMBCC_ASMEXPR_H
#define EMBCC_ASMEXPR_H

#include <ctype.h>
#include <string.h>

struct cx { const char *p, *e; int bad; };

static void cx_sp(struct cx *x)
{
    while (x->p < x->e && isspace((unsigned char)*x->p))
        x->p++;
}

static int cx_is(struct cx *x, const char *op)
{
    size_t n = strlen(op);
    cx_sp(x);
    if ((size_t)(x->e - x->p) < n || strncmp(x->p, op, n) != 0)
        return 0;
    /* `<` is not `<<`, `&` not `&&`: those are not in this grammar */
    if (n == 1 && x->p + 1 < x->e && x->p[1] == x->p[0] &&
        (op[0] == '<' || op[0] == '>' || op[0] == '&' || op[0] == '|'))
        return 0;
    x->p += n;
    return 1;
}

static long long cx_or(struct cx *x);

static long long cx_unary(struct cx *x)
{
    cx_sp(x);
    if (x->p >= x->e) { x->bad = 1; return 0; }
    if (*x->p == '-') {
        x->p++;
        return (long long)(0ULL - (unsigned long long)cx_unary(x));
    }
    if (*x->p == '+') { x->p++; return cx_unary(x); }
    if (*x->p == '~') { x->p++; return ~cx_unary(x); }
    if (*x->p == '(') {
        x->p++;
        long long v = cx_or(x);
        if (!cx_is(x, ")")) x->bad = 1;
        return v;
    }
    if (!isdigit((unsigned char)*x->p)) { x->bad = 1; return 0; }
    {
        int base = 10;
        unsigned long long v = 0;
        if (*x->p == '0' && x->p + 1 < x->e &&
            (x->p[1] == 'x' || x->p[1] == 'X')) {
            base = 16;
            x->p += 2;
        } else if (*x->p == '0' && x->p + 1 < x->e &&
                   (x->p[1] == 'b' || x->p[1] == 'B')) {
            base = 2;
            x->p += 2;
        } else if (*x->p == '0') {
            base = 8;
        }
        int any = base == 8;
        while (x->p < x->e && isxdigit((unsigned char)*x->p)) {
            int d = isdigit((unsigned char)*x->p) ? *x->p - '0'
                  : tolower((unsigned char)*x->p) - 'a' + 10;
            if (d >= base) { x->bad = 1; return 0; }
            v = v * (unsigned long long)base + (unsigned long long)d;
            any = 1;
            x->p++;
        }
        /* C's suffixes: 20UL is 20 */
        while (x->p < x->e && (*x->p == 'u' || *x->p == 'U' ||
                               *x->p == 'l' || *x->p == 'L'))
            x->p++;
        if (!any) x->bad = 1;
        return (long long)v;
    }
}

static long long cx_mul(struct cx *x)
{
    long long v = cx_unary(x);
    for (;;) {
        if (cx_is(x, "*")) {
            v = (long long)((unsigned long long)v *
                            (unsigned long long)cx_unary(x));
        } else if (cx_is(x, "/") || cx_is(x, "%")) {
            int div = x->p[-1] == '/';
            long long r = cx_unary(x);
            if (r == 0) { x->bad = 1; return 0; }
            v = div ? v / r : v % r;
        } else {
            return v;
        }
    }
}

static long long cx_add(struct cx *x)
{
    long long v = cx_mul(x);
    for (;;) {
        if (cx_is(x, "+"))
            v = (long long)((unsigned long long)v +
                            (unsigned long long)cx_mul(x));
        else if (cx_is(x, "-"))
            v = (long long)((unsigned long long)v -
                            (unsigned long long)cx_mul(x));
        else
            return v;
    }
}

static long long cx_shift(struct cx *x)
{
    long long v = cx_add(x);
    for (;;) {
        if (cx_is(x, "<<")) {
            long long n = cx_add(x);
            v = n >= 0 && n < 64
                ? (long long)((unsigned long long)v << n) : 0;
        } else if (cx_is(x, ">>")) {
            long long n = cx_add(x);
            v = n >= 0 && n < 64 ? v >> n : v < 0 ? -1 : 0;
        } else {
            return v;
        }
    }
}

static long long cx_and(struct cx *x)
{
    long long v = cx_shift(x);
    while (cx_is(x, "&"))
        v &= cx_shift(x);
    return v;
}

static long long cx_xor(struct cx *x)
{
    long long v = cx_and(x);
    while (cx_is(x, "^"))
        v ^= cx_and(x);
    return v;
}

static long long cx_or(struct cx *x)
{
    long long v = cx_xor(x);
    while (cx_is(x, "|"))
        v |= cx_xor(x);
    return v;
}

/* The whole of s[0..len) as one expression: 1 and *out, or 0. */
static int asm_const_expr(const char *s, int len, long long *out)
{
    struct cx x;
    x.p = s;
    x.e = s + len;
    x.bad = 0;
    long long v = cx_or(&x);
    cx_sp(&x);
    if (x.bad || x.p != x.e)
        return 0;
    *out = v;
    return 1;
}

#endif
