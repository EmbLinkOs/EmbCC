/* long double on RISC-V: IEEE binary128 at both widths, two registers at
 * RV64 and four words at RV32 -- where the psABI passes it BY REFERENCE
 * and returns it through a hidden pointer, the runtime's helpers
 * included. Every operation is a call into lib/rt/softtf.c.
 *
 * The expected answers are not written down here. The same source is
 * compiled by clang for the same triple and linked against the same
 * runtime, so a difference is the compiler's: in how it passes, returns,
 * copies or converts a long double, or calls a helper. Every result is
 * printed as its bits.
 */
#include <stdarg.h>

extern void writec(int c);
extern void puts_(const char *s);

/* clang zero-fills an aggregate initializer and copies a by-reference
 * argument with calls, and the harness has no C library. */
void *memset(void *d, int c, __SIZE_TYPE__ n)
{
    unsigned char *p = d;
    while (n--)
        *p++ = (unsigned char)c;
    return d;
}

void *memcpy(void *d, const void *s, __SIZE_TYPE__ n)
{
    unsigned char *p = d;
    const unsigned char *q = s;
    while (n--)
        *p++ = *q++;
    return d;
}

static void hx(unsigned long long v)
{
    for (int i = 60; i >= 0; i -= 4)
        writec("0123456789abcdef"[(int)((v >> i) & 0xfULL)]);
}

static void pld(const char *tag, long double v)
{
    union { long double d; unsigned long long w[2]; } b;
    b.d = v;
    puts_(tag);
    writec(' ');
    hx(b.w[1]);
    writec('_');
    hx(b.w[0]);
    writec('\n');
}

static void pn(const char *tag, long long v)
{
    puts_(tag);
    writec(' ');
    hx((unsigned long long)v);
    writec('\n');
}

static volatile double vd = 1234.5678;
static volatile float vf = -0.15625f;
static volatile int vi = -7;
static volatile long long vll = -1234567890123LL;
static volatile unsigned long long vull = 0xfedcba9876543210ULL;

/* Arguments past the registers: at RV32 each long double is a POINTER,
 * one register or one stack word; at RV64 a pair, split or on the stack
 * 16-aligned. */
__attribute__((noinline)) static long double many(int a, long double x,
                                                  int b, long double y,
                                                  int c, int d, int e,
                                                  long double z, int f,
                                                  long double w)
{
    return x * (long double)a - y / (long double)b + z * (long double)(c + d + e) - w * (long double)f;
}

__attribute__((noinline)) static long double vsum(int n, ...)
{
    va_list ap;
    long double r = 0;
    va_start(ap, n);
    for (int k = 0; k < n; k++) {
        int m = va_arg(ap, int);
        r = r * 3 + va_arg(ap, long double) * (long double)m;
    }
    va_end(ap);
    return r;
}

struct box { char tag; long double v; int n; };

__attribute__((noinline)) static struct box scale(struct box b, long double k)
{
    b.v = b.v * k + (long double)b.n;
    b.tag++;
    return b;
}

/* The callee may write its by-reference parameter: the caller's value has
 * to survive that. */
__attribute__((noinline)) static long double clobber(long double x)
{
    x = x * 2;
    return x + 1;
}

static int cmps(long double a, long double b)
{
    return (a < b) | (a <= b) << 1 | (a > b) << 2 | (a >= b) << 3 |
           (a == b) << 4 | (a != b) << 5;
}

int main(void)
{
    long double x = (long double)vd, y = (long double)vf;
    long double z = x * 3.25L - y / 7.0L;
    long double arr[3] = { x, y, z };
    long double *p = &arr[1];

    pld("c", 3.14159265358979323846264338327950288L);
    pld("add", x + y);
    pld("sub", x - y);
    pld("mul", x * y);
    pld("div", x / y);
    pld("neg", -x);
    pld("negz", -(long double)0);
    pld("z", z);
    pld("ptr", *p + p[1]);
    p[-1] = p[0] * 4;
    pld("st", arr[0]);
    pn("cmp", cmps(x, y) | cmps(y, x) << 6 | cmps(x, x) << 12);
    {
        long double zero = (long double)vd - (long double)vd;
        long double nan = zero / zero;
        pn("nan", cmps(nan, x) | cmps(nan, nan) << 6);
    }
    pld("fromi", (long double)vi);
    pld("fromu", (long double)(unsigned)vi);
    pld("fromll", (long double)vll);
    pld("fromull", (long double)vull);
    pld("fromd", (long double)vd);
    pld("fromf", (long double)vf);
    pn("toi", (int)(z / 100));
    pn("tou", (unsigned)(x * 1000));
    pn("toll", (long long)(-x * 1e12L));
    pn("toull", (long long)(unsigned long long)(x * 1e15L));
    {
        union { double d; unsigned long long u; } dd;
        union { float f; unsigned u; } ff;
        dd.d = (double)z;
        ff.f = (float)z;
        pn("tod", (long long)dd.u);
        pn("tof", (long long)ff.u);
    }
    pld("many", many(3, x, -2, y, 1, 2, 3, z, 5, 1.0L / 3.0L));
    pld("vsum", vsum(3, 2, x, 3, y, 4, (long double)vll));
    {
        struct box b = { 'a', x, 9 };
        struct box r = scale(b, y);
        pld("box", r.v);
        pn("boxt", r.tag * 100 + r.n);
        pld("orig", b.v);
    }
    {
        long double keep = z;
        pld("clob", clobber(keep));
        pld("keep", keep);
    }
    {
        long double _Complex a = x + y * 1.0iL, b = z - 2.0iL;
        long double _Complex m = a * b, q = a / b;
        pld("cmr", __real__ m);
        pld("cmi", __imag__ m);
        pld("cdr", __real__ q);
        pld("cdi", __imag__ q);
    }
    puts_("==END==\n");
    return 0;
}
