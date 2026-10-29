// expect-exit: 42
/* __fp16, ARM's binary16 storage format (ACLE): the same bits as
 * _Float16, but a value read from one is a float, as a char read is an
 * int -- `a + b` of two __fp16 is a float sum, rounded once, at the
 * store. Through `...` it is promoted to double. It and _Float16 are
 * distinct types. GCC has __fp16 on ARM alone; elsewhere it is clang's
 * and EmbCC's. */

#include <stdarg.h>
#include <string.h>

#if defined(__EMBCC__) || defined(__clang__) || defined(__arm__) || \
    defined(__aarch64__)

static unsigned short bits(const __fp16 *p)
{
    unsigned short u;
    memcpy(&u, p, 2);
    return u;
}

static double first(int n, ...)
{
    va_list ap;
    va_start(ap, n);
    double d = va_arg(ap, double);
    va_end(ap);
    return d;
}

static __fp16 store[4];

int main(void)
{
    volatile __fp16 a = 1.0, b = 0x1p-11;       /* 2^-11: half an ulp of 1 */
    __fp16 s = a + b;                           /* float 1+2^-11, a tie: 1 */
    if (bits(&s) != 0x3c00)
        return 1;
    /* the sum is a float: a + b + b is 1 + 2^-10 in float, then stored */
    s = a + b + b;
    if (bits(&s) != 0x3c01)
        return 2;
    if (sizeof(a + b) != sizeof(float) || sizeof(-a) != sizeof(float) ||
        sizeof(a) != 2)
        return 3;
    if (_Generic(a + a, float: 1, default: 0) != 1 ||
        _Generic((__fp16)0, __fp16: 1, _Float16: 2, default: 0) != 1 ||
        _Generic((_Float16)0, __fp16: 1, _Float16: 2, default: 0) != 2)
        return 4;
    store[0] = 0.1;                             /* one rounding from double */
    store[1] = 65520.0f;                        /* overflows */
    store[2] = -0x1p-24f;                       /* the least subnormal */
    store[3] = 3;
    if (bits(&store[0]) != 0x2e66 || bits(&store[1]) != 0x7c00 ||
        bits(&store[2]) != 0x8001 || bits(&store[3]) != 0x4200)
        return 5;
    if (first(1, store[3]) != 3.0 || first(1, a) != 1.0)
        return 6;
    _Float16 h = store[0];                      /* the same value */
    if (h != store[0] || (float)h != (float)store[0])
        return 7;
    store[3] += 0.5f;
    if (store[3] != 3.5f)
        return 8;
    return 42;
}

#else
int main(void) { return 42; }
#endif
