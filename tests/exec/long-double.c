/* long double as its own type: x87 80-bit extended on x86-64, IEEE
 * binary128 on aarch64 — 16 bytes, 16-aligned, on both. Checks that it is
 * wider than double, its arithmetic, conversions (the unsigned 64-bit ones
 * included), comparisons (NaN, -0.0), the calling convention (arguments past
 * the registers, returns, varargs, structs holding it), arrays, and that
 * compile-time folding (a static initializer) agrees with run-time
 * arithmetic bit for bit.
 */
// expect-exit: 42
#include <stdarg.h>
#include <string.h>

struct one { long double v; };
struct two { long double a, b; };
struct mixed { int tag; long double v; char c; };

static long double sum3(long double a, long double b, long double c)
{
    return a + b + c;
}

/* ten long doubles and some ints/doubles between them: past every
 * register file on both targets */
static long double many(int i0, long double a, double d0, long double b,
                        long double c, int i1, long double d, long double e,
                        long double f, long double g, double d1,
                        long double h, long double i, long double j)
{
    return a + b + c + d + e + f + g + h + i + j + i0 + i1 + d0 + d1;
}

static long double vsum(int n, ...)
{
    va_list ap;
    va_start(ap, n);
    long double s = 0;
    for (int k = 0; k < n; k++) {
        if (k % 3 == 2)
            s += va_arg(ap, int);
        else
            s += va_arg(ap, long double);
    }
    va_end(ap);
    return s;
}

static long double take_one(struct one o) { return o.v * 2; }
static struct two make_two(long double x) { struct two t = { x, -x }; return t; }
static long double take_mixed(struct mixed m) { return m.v + m.tag + m.c; }

static const long double folded[] = {
    1.0L / 3, 2.0L / 3 * 3, 0.1L + 0.2L, -(1.5L * 4), (long double)0.1,
    (long double)(1.0 / 3.0), 1e300L * 1e300L, 18446744073709551615.0L,
};

int main(void)
{
    if (sizeof(long double) != 16 || _Alignof(long double) != 16)
        return 1;

    /* wider than double: 1 + 2^-60 is exact in both formats' significands */
    volatile long double one = 1.0L, tiny = 0x1p-60L;
    if (one + tiny == one) return 2;
    if ((double)(one + tiny) != 1.0) return 3;       /* rounds away in double */

    /* run-time arithmetic == the compile-time fold, bit for bit */
    volatile long double three = 3, two = 2, ten = 10, pt1 = 0.1L, pt2 = 0.2L;
    long double rt[8];
    rt[0] = one / three;
    rt[1] = two / three * three;
    rt[2] = pt1 + pt2;
    rt[3] = -(1.5L * 4 * one);
    volatile double d01 = 0.1;
    rt[4] = d01;
    volatile double dthird = 1.0 / 3.0;
    rt[5] = dthird;
    volatile long double big = 1e300L;
    rt[6] = big * big;
    volatile unsigned long umax = 18446744073709551615UL;
    rt[7] = umax;
    for (int k = 0; k < 8; k++)
        if (memcmp(&rt[k], &folded[k], 10) != 0 || rt[k] != folded[k])
            return 10 + k;

    /* conversions */
    volatile long double x = -7.9L;
    if ((int)x != -7 || (long)x != -7 || (short)x != -7) return 20;
    volatile long double huge = 18000000000000000000.0L;   /* > 2^63 */
    if ((unsigned long)huge != 18000000000000000000UL) return 21;
    if ((unsigned long)(long double)umax != umax) return 22;  /* exact both ways */
    volatile unsigned int u32 = 4000000000U;
    if ((long double)u32 != 4000000000.0L) return 23;
    volatile int neg = -5;
    if ((long double)neg != -5.0L) return 24;
    volatile float fl = 0.1f;
    if ((long double)fl != (long double)(double)0.1f) return 25;
    if ((float)(long double)fl != fl) return 26;

    /* comparisons */
    volatile long double nz = -0.0L, pz = 0.0L, nan = 0.0L / pz;
    if (!(nz == pz) || nz < pz || nz > pz) return 30;
    if (nan == nan || nan < one || nan > one || nan <= one || !(nan != nan))
        return 31;
    if (!(one < three) || !(three >= three) || three <= one) return 32;
    if (nz) return 33;                         /* -0.0 is false */
    if (!nan) return 34;                       /* NaN is true */

    /* operators */
    long double acc = 1.5L;
    acc += 2; acc *= 3; acc -= 0.5L; acc /= 2;        /* ((1.5+2)*3-.5)/2 = 5 */
    if (acc != 5) return 40;
    acc++;
    --acc;
    long double post = acc--;
    if (post != 5 || acc != 4) return 41;
    if ((acc > 3 ? acc : -acc) != 4) return 42;
    if (!acc || !(acc && three)) return 43;
    if (-acc != -4.0L) return 44;

    /* the calling convention */
    if (sum3(1, 2, 3) != 6) return 50;
    if (many(1, 1, 0.5, 2, 3, 2, 4, 5, 6, 7, 0.25, 8, 9, 10) != 58.75L) return 51;
    if (vsum(6, 1.5L, 2.5L, 3, 4.0L, 5.0L, 6) != 22) return 52;
    struct one o = { 21.0L };
    if (take_one(o) != 42) return 53;
    struct two t = make_two(8);
    if (t.a != 8 || t.b != -8) return 54;
    struct mixed m = { 3, 0.5L, 1 };
    if (take_mixed(m) != 4.5L) return 55;

    /* arrays and pointers */
    long double arr[5];
    for (int k = 0; k < 5; k++)
        arr[k] = k * 0.5L;
    long double *p = arr + 4;
    if (*p != 2 || p - arr != 4 || (char *)p - (char *)arr != 64) return 60;
    return 42;
}
