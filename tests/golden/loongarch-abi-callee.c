#include <stdarg.h>
#include "loongarch-abi.h"
int g_counter = 5;
long g_arr[4] = { 10, 20, 30, 40 };
unsigned u_ret(unsigned x) { return x | 0x80000000u; }
long u_take(unsigned x, int y) { return (long)x + y; }
int c_take(char c, signed char sc, unsigned char uc, short s,
           unsigned short us)
{ return c * 100000 + sc * 1000 + uc * 7 + s * 3 + us + (c < 0) * 1000000; }
char c_ret(int v) { return (char)v; }
float f_sum(float a, double b, float c, int d) { return a + (float)b * c + d; }
double d_var(int n, ...)
{
    va_list ap;
    double t = 0;
    va_start(ap, n);
    for (int i = 0; i < n; i++)
        t = t * 10 + va_arg(ap, double);
    va_end(ap);
    return t;
}
struct f2 f2_make(float a, float b) { struct f2 r = { a * 2, b + 1 }; return r; }
struct d2 d2_make(double a, double b) { struct d2 r = { a - 1, b * 4 }; return r; }
struct fd fd_make(float f, double d) { struct fd r = { f * 3, d / 2 }; return r; }
double d2_take(int a, struct d2 x, struct d2 y)
{ return a + x.a * 10 + x.b * 100 + y.a * 1000 + y.b * 10000; }
double d2_split(int a, int b, int c, int d, int e, int f, int g, struct d2 x)
{ return a + b + c + d + e + f + g + x.a * 100 + x.b * 1000; }
long big_sum(struct big b, int k) { return b.a + b.b * 10 + b.c * 100 + k; }
void big_mod(struct big b) { b.a = 999; b.b = 999; b.c = 999; g_counter += (int)b.a; }
long many(long a, long b, long c, long d, long e, long f, long g, long h,
          int s0, long s1, char s2, short s3, unsigned s4, long s5)
{
    return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8 +
           s0 * 9 + s1 * 10 + s2 * 11 + s3 * 12 + (long)s4 * 13 + s5 * 14;
}
long v_mix(int n, ...)
{
    va_list ap;
    long t;
    __int128 w;
    struct a16 s;
    va_start(ap, n);
    t = va_arg(ap, int);
    w = va_arg(ap, __int128);
    t = t * 7 + (long)(w >> 64) * 3 + (long)w;
    t = t * 5 + va_arg(ap, int);
    s = va_arg(ap, struct a16);
    t = t * 11 + s.lo + s.hi * 2;
    t = t + va_arg(ap, long) * 13 + n;
    va_end(ap);
    return t;
}
struct mixc mixc_make(int k)
{ struct mixc r = { (signed char)-k, (unsigned char)(k + 200), (short)(k * -300), k * 1000 }; return r; }
_Complex float cf_make(float re, float im)
{ _Complex float z; __real__ z = re * 2; __imag__ z = im * 3; return z; }
_Complex double cd_make(double re, double im)
{ _Complex double z; __real__ z = re + 1; __imag__ z = im - 1; return z; }
int bump(int by) { g_counter += by; g_arr[2] += by; return g_counter; }
static int twice(int x) { return 2 * x; }
static int square(int x) { return x * x; }
int (*pick_fn(int which))(int) { return which ? square : twice; }
int call_fn(int (*f)(int), int x) { return f(x) + 1; }
