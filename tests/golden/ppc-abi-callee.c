#include "ppc-abi.h"
long long ll_after_1(int a, long long b) { return b * 3 + a; }
long long ll_after_7(int a, int b, int c, int d, int e, int f, int g,
                     long long h, int i)
{ return h - (a + b + c + d + e + f + g) + (long long)i * 0x100000000LL; }
long long ll_after_6(int a, int b, int c, int d, int e, int f, long long g,
                     int h)
{ return g * 2 + a + b + c + d + e + f + h * 1000; }
int ints_10(int a, int b, int c, int d, int e, int f, int g, int h, int i,
            int j)
{ return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8 + i * 9 + j * 10; }
double d_after_7(int a, int b, int c, int d, int e, int f, int g, double h,
                 float i, double j)
{ return (a + b + c + d + e + f + g) * h + i - j; }
long double ld_add(long double a, long double b) { return a + b * 2; }
int take_mod(struct mod m, int k)
{
    int t = 0;
    for (int i = 0; i < 5; i++) { t += m.v[i] * (i + 1); m.v[i] = -1; }
    return t + k;
}
int take_s3_s8(struct s3 a, struct s8 b, int k)
{ return a.a + a.b * 10 + a.c * 100 + b.a * 1000 + b.b * 10000 + k; }
int structs_late(int a, int b, int c, int d, int e, int f, int g, int h,
                 struct s12 s, struct s1 t)
{ return a + b + c + d + e + f + g + h + s.a * 100 + s.b * 1000 + s.c * 10000 + t.a; }
struct s1 ret_s1(int k) { struct s1 s = { (unsigned char)k }; return s; }
struct s2 ret_s2(int k) { struct s2 s = { (unsigned char)k, (unsigned char)(k + 1) }; return s; }
struct s3 ret_s3(int k)
{ struct s3 s = { (unsigned char)k, (unsigned char)(k + 1), (unsigned char)(k + 2) }; return s; }
struct s5 ret_s5(int k)
{ struct s5 s; for (int i = 0; i < 5; i++) s.c[i] = (unsigned char)(k + i); return s; }
struct s6 ret_s6(int k)
{ struct s6 s; for (int i = 0; i < 3; i++) s.h[i] = (short)(k * (i + 1) - 1000); return s; }
struct s7 ret_s7(int k)
{ struct s7 s; for (int i = 0; i < 7; i++) s.c[i] = (unsigned char)(k + 3 * i); return s; }
struct s8 ret_s8(int k) { struct s8 s = { k, -k }; return s; }
struct s12 ret_s12(int k) { struct s12 s = { k, k * 2, k * 3 }; return s; }
_Complex float ret_cf(float re, float im)
{ _Complex float z; __real__ z = re * 2; __imag__ z = im * 3; return z; }
_Complex double ret_cd(double re, double im)
{ _Complex double z; __real__ z = re + 1; __imag__ z = im - 1; return z; }
double take_cd(_Complex double z, _Complex float w)
{ return __real__ z * 2 + __imag__ z * 3 + __real__ w * 5 + __imag__ w * 7; }
long long vll_late(int n, ...)
{
    va_list ap; long long t = 0;
    va_start(ap, n);
    for (int i = 0; i < n; i++)
        t = t * 5 + (i % 2 ? va_arg(ap, long long) : va_arg(ap, int));
    va_end(ap);
    return t;
}
int vstructs(int n, ...)
{
    va_list ap; int t = 0;
    va_start(ap, n);
    for (int i = 0; i < n; i++) {
        if (i % 2) { struct s12 s = va_arg(ap, struct s12); t = t * 3 + s.a + s.b - s.c; }
        else { struct s3 s = va_arg(ap, struct s3); t = t * 7 + s.a + s.b * 2 + s.c * 3; }
    }
    va_end(ap);
    return t;
}
double vfloat(int n, ...)
{
    va_list ap; double t = 0;
    va_start(ap, n);
    for (int i = 0; i < n; i++)
        t = t * 2 + va_arg(ap, double);
    va_end(ap);
    return t;
}
/* the record the OTHER compiler's va_start built, walked here */
int vlist_sum(int n, va_list ap)
{
    int t = 0;
    for (int i = 0; i < n; i++)
        t = t * 3 + (i % 3 == 1 ? (int)va_arg(ap, long long)
                     : i % 3 == 2 ? (int)va_arg(ap, double) : va_arg(ap, int));
    return t;
}
int vlist_other(int n, ...)
{
    va_list ap; int r;
    va_start(ap, n);
    r = vlist_sum(n, ap);
    va_end(ap);
    return r;
}
