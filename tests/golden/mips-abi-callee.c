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
