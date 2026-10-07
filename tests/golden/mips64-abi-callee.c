#include <stdarg.h>
#include "mips64-abi.h"
long many_l(long a, long b, long c, long d, long e, long f, long g, long h,
            long i, long j)
{ return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8 + i * 9 + j * 10; }
double many_d(double a, double b, double c, double d, double e, double f,
              double g, double h, double i, int j)
{ return a + b * 2 + c * 4 + d * 8 + e + f + g + h * 0.5 + i * 0.25 + j; }
int many_i(int a, int b, int c, int d, int e, int f, int g, int h, int i,
           unsigned j)
{ return a - b + c - d + e - f + g - h + i + (j == 0xffffffffu) * 1000; }
float fmul3(float a, float b, float c) { return a * b * c; }
unsigned uret(unsigned a, unsigned b) { return a - b; }
int is_max(unsigned u) { return u == 0xffffffffu; }
long sext_back(int x) { return x; }
signed char ret_sc(int v) { return (signed char)v; }
unsigned char ret_uc(int v) { return (unsigned char)v; }
short ret_ss(int v) { return (short)v; }
unsigned short ret_us(int v) { return (unsigned short)v; }
long double ld_add(long double a, long double b) { return a + b; }
long double ld_after_int(int a, long double b) { return a + b; }
long double ld_stack(int a, int b, int c, int d, int e, int f, int g,
                     long double h, int i)
{ return h * (a + b + c + d + e + f + g) + i; }
__int128 i128_after_int(int a, __int128 b) { return b * 3 + a; }
__int128 i128_at7(int a, int b, int c, int d, int e, int f, int g, __int128 h)
{ return h - (a + b + c + d + e + f + g); }
unsigned __int128 i128_mul(unsigned __int128 a, unsigned __int128 b) { return a * b; }
int take_c3(int a, struct c3 s, int k) { return a + s.a * 100 + s.b * 10 + s.c + k * 10000; }
int take_i3(struct i3 s, int k) { return s.a + s.b * 10 + s.c * 100 + k; }
long take_l3(int a, struct l3 s) { return a + s.a + s.b * 3 + s.c * 5; }
long take_big(int a, int b, struct big s, int z)
{ long t = a + b; for (int i = 0; i < 10; i++) t += s.v[i] * (i + 1); return t + z * 1000; }
long take_al16(int a, struct al16 s, int z) { return a + s.a * 7 + s.b * 11 + z; }
long take_i16(int a, struct i16 s) { return a + (long)(s.x >> 64) + (long)s.x; }
int take_c3_stack(long a, long b, long c, long d, long e, long f, long g,
                  long h, struct c3 s, struct s16 t)
{ return (int)(a + b + c + d + e + f + g + h) + s.a * 1000 + s.c + t.h[0] + t.h[7] * 3; }
struct c3 ret_c3(int k) { struct c3 r; r.a = (char)k; r.b = (char)(k + 1); r.c = (char)(k + 2); return r; }
struct i3 ret_i3(int k) { struct i3 r; r.a = k; r.b = -k; r.c = k * 3; return r; }
struct l3 ret_l3(long k) { struct l3 r; r.a = k; r.b = k << 33; r.c = -k; return r; }
struct big ret_big(long k) { struct big r; for (int i = 0; i < 10; i++) r.v[i] = k * i; return r; }
struct f1 ret_f1(float x) { struct f1 r; r.a = x * 2; return r; }
struct f2 ret_f2(float x, float y) { struct f2 r; r.a = x + 1; r.b = -y; return r; }
struct df ret_df(double x, float y) { struct df r; r.a = x * 3; r.b = y * 3; return r; }
struct fd ret_fd(float x, double y) { struct fd r; r.a = x - 1; r.b = y - 1; return r; }
struct d2 ret_d2(double x, double y) { struct d2 r; r.a = y; r.b = x; return r; }
struct fi ret_fi(float x, int y) { struct fi r; r.a = x * 4; r.b = y * 4; return r; }
struct s16 ret_s16(int k) { struct s16 r; for (int i = 0; i < 8; i++) r.h[i] = (short)(k - i * 1000); return r; }
struct ld1 ret_ld1(long double x) { struct ld1 r; r.x = x / 3; return r; }
_Complex float ret_cf(float re, float im)
{ _Complex float z; __real__ z = re * 2; __imag__ z = im * 3; return z; }
_Complex double ret_cd(double re, double im)
{ _Complex double z; __real__ z = re + 1; __imag__ z = im - 1; return z; }
_Complex long double ret_cld(long double re, long double im)
{ _Complex long double z; __real__ z = re * 2; __imag__ z = im + 1; return z; }
long vsum(int n, ...)
{
    va_list ap; long t = 0;
    va_start(ap, n);
    for (int i = 0; i < n; i++) t = t * 3 + va_arg(ap, long);
    va_end(ap);
    return t;
}
double vdsum(int n, ...)
{
    va_list ap; double t = 0;
    va_start(ap, n);
    for (int i = 0; i < n; i++) t += va_arg(ap, double);
    va_end(ap);
    return t;
}
long double vldsum(int n, ...)
{
    va_list ap; long double t = 0;
    va_start(ap, n);
    for (int i = 0; i < n; i++) {
        t += va_arg(ap, int);
        t += va_arg(ap, long double);
    }
    va_end(ap);
    return t;
}
long vmixed(int n, ...)
{
    va_list ap; long t = 0;
    va_start(ap, n);
    for (int i = 0; i < n; i++) {
        switch (i % 5) {
        case 0: t += va_arg(ap, int); break;
        case 1: t += (long)va_arg(ap, double); break;
        case 2: t += va_arg(ap, long); break;
        case 3: { struct c3 s = va_arg(ap, struct c3); t += s.a + s.b + s.c; } break;
        default: { struct l3 s = va_arg(ap, struct l3); t += s.a - s.b + s.c; } break;
        }
    }
    va_end(ap);
    return t;
}
long far_array[300000];
long far_elt(long i) { return far_array[i]; }
static int twice(int x) { return 2 * x; }
static int thrice(int x) { return 3 * x; }
int (*pick(int k))(int) { return k ? thrice : twice; }
