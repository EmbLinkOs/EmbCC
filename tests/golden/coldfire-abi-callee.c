#include <stdarg.h>
#include "coldfire-abi.h"

int narrow(signed char a, unsigned char b, short c, unsigned short d, int e)
{ return a * 1000000 + b * 10000 + c * 100 + d + e; }
int smalls(struct c1 a, int x, struct c2 b, struct c3 c, int y)
{ return a.a + x * 2 + b.a * 3 + b.b * 5 + c.a * 7 + c.b * 11 + c.c * 13 + y * 17; }
long long mids(struct c5 a, struct c7 b, long long z, struct c10 c)
{ return a.a + a.b * 3LL + b.a * 5 + b.b + b.c + b.d + b.e + b.f + z * 7 +
         c.a + c.b * 11 + c.c; }
struct c3 r3(int k) { struct c3 s = { (char)k, (char)(k + 1), (char)(k + 2) }; return s; }
struct c5 r5(struct c5 s, int k) { s.a += (char)k; s.b *= k; return s; }
struct c10 r10(long long v) { struct c10 s = { 1, v * 3, 2 }; return s; }
_Complex float cf(float re, float im) { _Complex float z; __real__ z = re * 2; __imag__ z = im + 1; return z; }
_Complex double cd(double re, double im, int k)
{ _Complex double z; __real__ z = re * k; __imag__ z = im - k; return z; }
char *ptr_ret(char *base, int off) { return base + off; }
double dmix(char a, double b, short c, float d, long long e)
{ return a + b * 2 + c * 3 + d * 4 + (double)e * 5; }
long long vstructs(int n, ...)
{
    va_list ap; long long t = 0;
    va_start(ap, n);
    for (int k = 0; k < n; k++) {
        struct c3 s3 = va_arg(ap, struct c3);
        long long l = va_arg(ap, long long);
        struct c5 s5 = va_arg(ap, struct c5);
        double d = va_arg(ap, double);
        int i = va_arg(ap, int);
        t = t * 3 + s3.a + s3.b * 2 + s3.c * 4 + l + s5.a + s5.b * 5 +
            (long long)(d * 8) + i;
    }
    va_end(ap);
    return t;
}
int keep_across(int a, int b, int c, int d) { return a * b - c * d; }
