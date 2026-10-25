/* One step to the next representable value: nexttoward and nexttowardf,
 * whose long double direction can lie between a value and its neighbour,
 * and nextafterl and nexttowardl at long double's own width -- the x87's
 * 64-bit significand, binary128's 113, or double's 53 -- across zero, the
 * subnormals, the binade edges and infinity. nanf and nanl are NaNs.
 *
 * The long double functions are asked of lib/libc, found by a weak
 * reference to a function each of its backends defines: the x86-64 QEMU
 * harness links newlib. (Apple's linker refuses a weak reference nothing
 * defines; there long double is double, and its libm is asked.) */
// expect-exit: 42
#include <float.h>
#include <math.h>
#include <stddef.h>

#if !defined(__APPLE__)
extern long __os_write(int fd, const void *buf, size_t n)
    __attribute__((weak));
#endif

/* equal, and of the same sign -- which tells -0 from +0 */
static int same(long double a, long double b)
{
    return a == b &&
           __builtin_copysignl(1.0L, a) == __builtin_copysignl(1.0L, b);
}

static int narrow(void)
{
    long double inf = INFINITY;
    double dnan = NAN;

    if (nexttoward(1.0, 2.0L) != 1.0 + DBL_EPSILON)
        return 1;
    if (nexttoward(1.0, 0.0L) != 1.0 - DBL_EPSILON / 2)
        return 2;
    if (nexttoward(1.0, 1.0L) != 1.0)
        return 3;
    if (!same(nexttoward(0.0, -1.0L), -DBL_TRUE_MIN))
        return 4;
    if (nexttoward(DBL_MAX, inf) != INFINITY)
        return 5;
    if (nexttoward(-INFINITY, 0.0L) != -DBL_MAX)
        return 6;
    /* a direction only a long double holds: just above 1, below 1's
     * double neighbour -- it still says up */
    if (LDBL_MANT_DIG > DBL_MANT_DIG &&
        nexttoward(1.0, 1.0L + LDBL_EPSILON) != 1.0 + DBL_EPSILON)
        return 7;
    double n1 = nexttoward(dnan, 0.0L), n2 = nexttoward(1.0, (long double)NAN);
    if (n1 == n1 || n2 == n2)
        return 8;                       /* a NaN either side is a NaN */

    if (nexttowardf(1.0f, 2.0L) != 1.0f + FLT_EPSILON)
        return 10;
    if (nexttowardf(1.0f, -3.0L) != 1.0f - FLT_EPSILON / 2)
        return 11;
    if (nexttowardf(1.0f, 1.0L + LDBL_EPSILON) != 1.0f + FLT_EPSILON)
        return 12;
    if (!same(nexttowardf(-0.0f, 1.0L), FLT_TRUE_MIN))
        return 13;
    if (nexttowardf(FLT_MAX, inf) != INFINITY)
        return 14;

    if (!(nanf("") != nanf("")) || !(nanl("") != nanl("")))
        return 15;
    return 0;
}

static int wide(void)
{
    long double inf = INFINITY;
    long double below_min = LDBL_MIN - LDBL_TRUE_MIN;

    if (nextafterl(1.0L, 2.0L) != 1.0L + LDBL_EPSILON)
        return 20;
    if (nextafterl(1.0L, 0.0L) != 1.0L - LDBL_EPSILON / 2)
        return 21;
    if (nextafterl(-1.0L, -2.0L) != -1.0L - LDBL_EPSILON)
        return 22;
    if (nextafterl(2.0L, 0.0L) != 2.0L - LDBL_EPSILON)
        return 23;                      /* down out of a binade */
    if (nextafterl(2.0L - LDBL_EPSILON, 3.0L) != 2.0L)
        return 24;                      /* and back up into it */
    if (!same(nextafterl(0.0L, 1.0L), LDBL_TRUE_MIN) ||
        !same(nextafterl(-0.0L, -1.0L), -LDBL_TRUE_MIN))
        return 25;
    if (!same(nextafterl(LDBL_TRUE_MIN, 0.0L), 0.0L) ||
        !same(nextafterl(-LDBL_TRUE_MIN, 1.0L), -0.0L))
        return 26;
    if (nextafterl(LDBL_MIN, 0.0L) != below_min || !(below_min < LDBL_MIN))
        return 27;                      /* the largest subnormal */
    if (nextafterl(below_min, 1.0L) != LDBL_MIN)
        return 28;                      /* into the normals */
    if (nextafterl(LDBL_MAX, inf) != inf || nextafterl(inf, 0.0L) != LDBL_MAX)
        return 29;
    if (nextafterl(-inf, 0.0L) != -LDBL_MAX)
        return 30;
    if (nextafterl(3.0L, 3.0L) != 3.0L)
        return 31;
    long double n = nanl("");
    if (!(nextafterl(n, 1.0L) != nextafterl(n, 1.0L)) ||
        !(nextafterl(1.0L, n) != nextafterl(1.0L, n)))
        return 32;
    /* the step is long double's, not double's */
    if (LDBL_MANT_DIG > DBL_MANT_DIG &&
        nextafterl(1.0L, 2.0L) == (long double)(1.0 + DBL_EPSILON))
        return 33;
    if (nexttowardl(1.0L, 2.0L) != 1.0L + LDBL_EPSILON ||
        nexttowardl(-0.0L, 1.0L) != LDBL_TRUE_MIN)
        return 34;
    return 0;
}

int main(void)
{
    int r = narrow();
    if (r)
        return r;
#if !defined(__APPLE__)
    if (!&__os_write)
        return 42;
#endif
    r = wide();
    if (r)
        return r;
    return 42;
}
