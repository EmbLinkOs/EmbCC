#include <stdarg.h>
#include "sparc-abi.h"
double d_after_1(int a, double b) { return a + b; }
double d_split(int a, int b, int c, int d, int e, double f)
{ return a + b * 2 + c * 3 + d * 4 + e * 5 + f; }
double d_stack(int a, int b, int c, int d, int e, int f, double g)
{ return a + b + c + d + e + f * 10 + g; }
long long ll_after_1(int a, long long b) { return b * 3 + a; }
long long ll_split(int a, int b, int c, int d, int e, long long f)
{ return f - a - b - c - d - e; }
long long ll_stack(int a, int b, int c, int d, int e, int f, long long g,
                   int h)
{ return g + a + b + c + d + e + f + (long long)h * 1000000000LL; }
long long ll_ret(unsigned hi, unsigned lo) { return (long long)((unsigned long long)hi << 32 | lo); }
float f_mixed(int a, float b, long long c, float d) { return a + b + (float)c + d; }
int take_s1(struct s1 s, int k) { return s.a * 3 + k; }
int take_s3(int a, struct s3 s) { return a + s.a + s.b * 10 + s.c * 100; }
/* writes its parameter: the caller's object must not change */
int take_s8_write(struct s8 s) { int r = s.a * 7 + s.b; s.a = -1; s.b = -2; return r + s.a + s.b; }
int take_big(int a, struct big s, int z)
{ int t = a; for (int i = 0; i < 9; i++) t += s.v[i] * (i + 1); s.v[0] = 0; return t + z * 1000; }
int take_un(union un u, int k) { return u.c[0] * 100 + u.c[5] + k; }
int take_many(int a, int b, int c, int d, int e, int f, struct s3 g,
              struct s8 h, int i)
{ return a + b + c + d + e + f + g.a + g.c * 10 + h.a * 100 + h.b * 1000 + i * 10000; }
double take_cd(_Complex double z, int k) { return __real__ z * 2 + __imag__ z + k; }
float take_cf(int k, _Complex float z) { return __real__ z - __imag__ z * k; }
struct s1 ret_s1(int k) { struct s1 r; r.a = (unsigned char)(k * 3); return r; }
struct s3 ret_s3(int k) { struct s3 r; r.a = (char)k; r.b = (char)(k + 1); r.c = (char)(k + 2); return r; }
struct sd ret_sd(double x) { struct sd r; r.d = x * 2; return r; }
struct big ret_big(int k) { struct big r; for (int i = 0; i < 9; i++) r.v[i] = k * i + 1; return r; }
union un ret_un(int k) { union un u; u.i = 0; u.c[0] = (char)k; u.c[5] = (char)(k + 9); return u; }
_Complex float ret_cf(float re, float im)
{ _Complex float z; __real__ z = re * 2; __imag__ z = im * 3; return z; }
_Complex double ret_cd(double re, double im)
{ _Complex double z; __real__ z = re + 1; __imag__ z = im - 1; return z; }
long double ld_pick(int which, long double a, long double b) { return which ? b : a; }
long double ld_stack(int a, int b, int c, int d, int e, int f, long double g)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; return g; }
signed char ret_sc(int v) { return (signed char)v; }
unsigned char ret_uc(int v) { return (unsigned char)v; }
short ret_ss(int v) { return (short)v; }
unsigned short ret_us(int v) { return (unsigned short)v; }
int take_narrow(signed char a, unsigned char b, short c, unsigned short d,
                int e, signed char f, short g)
{ return a + b * 2 + c * 3 + d + e + f * 5 + g * 7 + (a < 0) * 1000 + (c < 0) * 10000; }
double vdsum(int n, ...)
{
    va_list ap; double s = 0;
    va_start(ap, n);
    for (int k = 0; k < n; k++) s = s * 2 + va_arg(ap, double);
    va_end(ap);
    return s;
}
long long vll(int n, ...)
{
    va_list ap; long long s = 0;
    va_start(ap, n);
    for (int k = 0; k < n; k++) {
        if (k & 1) s += va_arg(ap, int);
        else       s = s * 3 + va_arg(ap, long long);
    }
    va_end(ap);
    return s;
}
int vstruct(int n, ...)
{
    va_list ap; int s = 0;
    va_start(ap, n);
    for (int k = 0; k < n; k++) {
        struct s3 a = va_arg(ap, struct s3);
        double d = va_arg(ap, double);
        struct s8 b = va_arg(ap, struct s8);
        s = s * 7 + a.a + a.c * 3 + (int)d + b.a * 5 + b.b;
    }
    va_end(ap);
    return s;
}
int far_array[10000];
int far_elt(int i) { return far_array[9000] + i; }
static int twice(int x) { return x * 2; }
static int thrice(int x) { return x * 3; }
int (*pick(int k))(int) { return k ? thrice : twice; }
