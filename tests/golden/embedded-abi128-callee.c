#include "embedded-abi128.h"
#include <stdarg.h>

s128 r_ret(long k)
{
    unsigned __int128 u = (unsigned __int128)k;  /* no signed overflow */
    return (s128)(u * 0x100000001UL * 0x10000UL + (u << 100));
}

s128 p_odd(int a, s128 x) { return x * 3 + a; }

s128 p_split(int a, int b, int c, int d, int e, int f, int g, s128 x)
{
    return x - (a + b + c + d + e + f + g);
}

s128 p_stack(int a, int b, int c, int d, int e, int f, int g, int h,
             int i, s128 x, s128 y)
{
    return (x ^ y) + (a + b + c + d + e + f + g + h) * (s128)i;
}

long double l_odd(int a, long double x, long double y)
{
    return x * (long double)a - y;
}

long double l_split(int a, int b, int c, int d, int e, int f, int g,
                    long double x, long double y)
{
    return (x + (long double)(a + b + c + d + e + f + g)) / y;
}

s128 v128(int n, ...)
{
    va_list ap;
    s128 r = 0;
    va_start(ap, n);
    for (int k = 0; k < n; k++) {
        int m = va_arg(ap, int);
        r = r * 7 + va_arg(ap, s128) * m;
    }
    va_end(ap);
    return r;
}

long double vld(int n, ...)
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

struct w128 s_odd(int a, struct w128 x, struct wld y)
{
    struct w128 r;
    r.v = x.v + a + (s128)(y.d * 4);
    return r;
}
