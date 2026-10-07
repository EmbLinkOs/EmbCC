#include <stdarg.h>
#include "mips-abi.h"
double dadd(double a, double b) { return a + b; }
float fmul3(float a, float b, float c) { return a * b * c; }
double d_after_int(int a, double b) { return a + b; }
double d_after_3(int a, int b, int c, double d) { return a * 100 + b * 10 + c + d; }
long long ll_after_3(int a, int b, int c, long long d) { return d * 2 + a + b + c; }
double many_d(double a, double b, double c, double e, int f)
{ return a + b * 2 + c * 4 + e * 8 + f * 16; }
float f_mixed(int a, float b, long long c, float d) { return a + b + (float)c + d; }
struct d1 ret_d1(double x) { struct d1 r; r.d = x * 2; return r; }
struct di ret_di(int i, double d) { struct di r; r.i = i + 1; r.d = d + 1; return r; }
struct c3 ret_c3(int k) { struct c3 r; r.a = (char)k; r.b = (char)(k + 1); r.c = (char)(k + 2); return r; }
struct big ret_big(int k) { struct big r; for (int i = 0; i < 9; i++) r.v[i] = k * i; return r; }
_Complex float ret_cf(float re, float im)
{ _Complex float z; __real__ z = re * 2; __imag__ z = im * 3; return z; }
_Complex double ret_cd(double re, double im)
{ _Complex double z; __real__ z = re + 1; __imag__ z = im - 1; return z; }
double take_di(int a, struct di s) { return a + s.i * 10 + s.d; }
int take_c3(int a, int b, int c, struct c3 s) { return a + b + c + s.a + s.b * 10 + s.c * 100; }
int take_big(int a, struct big s, int z)
{ int t = a; for (int i = 0; i < 9; i++) t += s.v[i] * (i + 1); return t + z * 1000; }
int take_fl2(struct fl2 s, int k) { return (int)(s.a * 10 + s.b) + k; }
int take_narrow(signed char a, unsigned char b, short c, unsigned short d,
                int e, signed char f)
{ return a + b * 2 + c * 3 + d + e + f * 5 + (a < 0) * 1000 + (c < 0) * 10000; }
signed char ret_sc(int v) { return (signed char)v; }
unsigned char ret_uc(int v) { return (unsigned char)v; }
short ret_ss(int v) { return (short)v; }
unsigned short ret_us(int v) { return (unsigned short)v; }
double vdsum(int n, ...)
{
    va_list ap; double t = 0;
    va_start(ap, n);
    for (int i = 0; i < n; i++) t += va_arg(ap, double);
    va_end(ap);
    return t;
}
int vmixed(int n, ...)
{
    va_list ap; int t = 0;
    va_start(ap, n);
    for (int i = 0; i < n; i++) {
        switch (i % 4) {
        case 0: t += va_arg(ap, int); break;
        case 1: t += (int)va_arg(ap, double); break;
        case 2: t += (int)va_arg(ap, long long); break;
        default: { struct c3 s = va_arg(ap, struct c3); t += s.a + s.b + s.c; } break;
        }
    }
    va_end(ap);
    return t;
}
int far_array[10000];
int far_elt(int i) { return far_array[i]; }
static int twice(int x) { return 2 * x; }
static int thrice(int x) { return 3 * x; }
int (*pick(int k))(int) { return k ? thrice : twice; }
int take_b1(struct b1 s, int k) { return s.a * 1000 + k; }
int take_b2_h1(int k, struct b2 s, struct h1 t)
{ return k * 1000000 + s.a * 10000 + s.b * 100 + t.h; }
int take_b5_at3(int a, int b, int c, struct b5 s)
{ return a + b + c + s.c[0] * 10000 + s.c[1] * 1000 + s.c[2] * 100 +
         s.c[3] * 10 + s.c[4]; }
int take_small_stack(int a, int b, int c, int d, struct b1 s, struct b2 t,
                     struct h1 u, struct b6 v)
{ return a + b + c + d + s.a * 100000 + t.a * 10000 + t.b * 1000 + u.h * 10 +
         v.h[0] + v.h[1] * 2 + v.h[2] * 3; }
long long ll_first(long long a, int b) { return a * 3 + b; }
long long ll_stack(int a, int b, int c, int d, long long e, int f)
{ return e - (a + b + c + d) + f * 0x100000000LL; }
long long ll_mix(long long a, long long b) { return (a << 4) ^ b; }
unsigned long long ull_ret(unsigned hi, unsigned lo)
{ return (unsigned long long)hi << 32 | lo; }
double d_stack(int a, int b, int c, int d, int e, double f)
{ return f * (a + b + c + d + e); }
struct b2 ret_b2(int k) { struct b2 s; s.a = (unsigned char)k; s.b = (unsigned char)(k + 1); return s; }
struct h1 ret_h1(int k) { struct h1 s; s.h = (short)-k; return s; }
long long vll(int n, ...)
{
    va_list ap; long long t = 0;
    va_start(ap, n);
    for (int i = 0; i < n; i++) t = t * 7 + va_arg(ap, long long);
    va_end(ap);
    return t;
}
int vsmall(int n, ...)
{
    va_list ap; int t = 0;
    va_start(ap, n);
    for (int i = 0; i < n; i++) {
        if (i % 2) { struct b2 s = va_arg(ap, struct b2); t = t * 3 + s.a - s.b; }
        else { struct b1 s = va_arg(ap, struct b1); t = t * 5 + s.a; }
    }
    va_end(ap);
    return t;
}
