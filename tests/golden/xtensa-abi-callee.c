#include "xtensa-abi.h"
double dadd(double a, double b) { return a + b; }
float fmul3(float a, float b, float c) { return a * b * c; }
double d_after_int(int a, double b) { return a + b; }
long long ll_after_5(int a, int b, int c, int d, int e, long long f, int g)
{ return f * 2 + a + b * 10 + c * 100 + d * 1000 + e * 10000 + g * 100000; }
int s12_after_4(int a, int b, int c, int d, struct s12 s, int z)
{ return a + b + c + d + s.a * 10 + s.b * 100 + s.c * 1000 + z * 10000; }
long long ll_stack(int a, int b, int c, int d, int e, int f, int g,
                   long long x, int h, long long y)
{ return y - x * 3 + a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8; }
double many_d(double a, double b, double c, double e, int f)
{ return a + b * 2 + c * 4 + e * 8 + f * 16; }
float f_mixed(int a, float b, long long c, float d) { return a + b + (float)c + d; }
struct d1 ret_d1(double x) { struct d1 r; r.d = x * 2; return r; }
struct di ret_di(int i, double d) { struct di r; r.i = i + 1; r.d = d + 1; return r; }
struct c3 ret_c3(int k) { struct c3 r; r.a = (char)k; r.b = (char)(k + 1); r.c = (char)(k + 2); return r; }
struct s12 ret_s12(int k) { struct s12 r; r.a = k; r.b = k * 2; r.c = k * 3; return r; }
struct s16 ret_s16(int k) { struct s16 r; r.a = k; r.b = -k; r.c = k * 5; r.d = k + 7; return r; }
struct s20 ret_s20(int k) { struct s20 r; for (int i = 0; i < 5; i++) r.v[i] = k + i * i; return r; }
struct big ret_big(int k) { struct big r; for (int i = 0; i < 9; i++) r.v[i] = k * i; return r; }
_Complex float ret_cf(float re, float im)
{ _Complex float z; __real__ z = re * 2; __imag__ z = im * 3; return z; }
_Complex double ret_cd(double re, double im)
{ _Complex double z; __real__ z = re + 1; __imag__ z = im - 1; return z; }
double take_cd(int a, _Complex double z, int b)
{ return a + __real__ z * 10 + __imag__ z * 100 + b * 1000; }
float take_cf(int a, int b, int c, int d, int e, _Complex float z)
{ return a + b + c + d + e + __real__ z * 10 + __imag__ z * 100; }
double take_di(int a, struct di s) { return a + s.i * 10 + s.d; }
int take_c3(int a, int b, int c, struct c3 s) { return a + b + c + s.a + s.b * 10 + s.c * 100; }
int take_big(int a, struct big s, int z)
{ int t = a; for (int i = 0; i < 9; i++) t += s.v[i] * (i + 1); return t + z * 1000; }
int take_fl2(struct fl2 s, int k) { return (int)(s.a * 10 + s.b) + k; }
int take_narrow(signed char a, unsigned char b, short c, unsigned short d,
                int e, signed char f, short g, unsigned char h)
{ return a + b * 2 + c * 3 + d + e + f * 5 + g * 7 + h * 11 +
         (a < 0) * 1000 + (c < 0) * 10000 + (g < 0) * 100000; }
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
/* five named words: the first unnamed is in a7, the second on the stack */
int vlate(int a, int b, int c, int d, int e, ...)
{
    va_list ap; int t = a + b + c + d + e;
    va_start(ap, e);
    t += va_arg(ap, int) * 10;
    t += (int)va_arg(ap, long long) * 100;
    t += va_arg(ap, int) * 1000;
    va_end(ap);
    return t;
}
int vsix(int a, int b, int c, int d, int e, int f, ...)
{
    va_list ap; int t = a + b + c + d + e + f;
    va_start(ap, f);
    t += va_arg(ap, int) * 10;
    t += (int)va_arg(ap, double) * 100;
    va_end(ap);
    return t;
}
int vstraddle(int a, int b, int c, int d, struct s12 s, ...)
{
    va_list ap; int t = a + b + c + d + s.a + s.b * 2 + s.c * 3;
    va_start(ap, s);
    t += va_arg(ap, int) * 10;
    t += va_arg(ap, int) * 100;
    va_end(ap);
    return t;
}
int vlist(int n, va_list ap)
{
    int t = 0;
    for (int i = 0; i < n; i++) t = t * 10 + va_arg(ap, int);
    return t;
}
int vforward(int n, ...)
{
    va_list ap; int r;
    va_start(ap, n);
    r = caller_vlist(n, ap);
    va_end(ap);
    return r;
}
static int twice(int x) { return 2 * x; }
static int thrice(int x) { return 3 * x; }
int (*pick(int k))(int) { return k ? thrice : twice; }
