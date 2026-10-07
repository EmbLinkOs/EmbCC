#include <stdarg.h>
#include "rx-abi.h"
int after_c6(struct c6 s, int a) { return s.v[0] * 100 + s.v[5] * 10 + a; }
long long after3(int a, int b, int c, long long d, int e)
{ return a + b * 10 + c * 100 + d + (long long)e * 1000000000000LL; }
int s12_regs(int a, struct s12 s) { return a + s.a * 10 + s.b * 100 + s.c * 1000; }
int s12_stk(int a, int b, struct s12 s, int z)
{ return a + b * 2 + s.a * 10 + s.b * 100 + s.c * 1000 + z * 10000; }
int narrow(signed char a, unsigned char b, short c, unsigned short d,
           signed char e, short f, unsigned char g)
{ return a + b + c + d + e + f + g; }
signed char ret_sc(int x) { return (signed char)(x + 1); }
unsigned short ret_us(int x) { return (unsigned short)(x * 3); }
struct s16 ret16(int k) { struct s16 r = { k, k + 1, k + 2, k + 3 }; return r; }
struct c6 ret6(int k) { struct c6 r = { { (char)k, 2, 3, 4, 5, (char)(k + 6) } }; return r; }
struct s4c ret4c(int k) { struct s4c r = { (char)k, 9, 8, (char)(k * 2) }; return r; }
_Complex float retcf(float re, float im) { _Complex float z; __real__ z = re; __imag__ z = im; return z; }
long long ret64(long long a, long long b) { return a * 3 - b; }
double dmul(double a, float b, double c) { return a * b + c; }
int vlast(int a, int b, ...)
{
    va_list ap; int s = a * 100 + b * 10;
    va_start(ap, b); s += va_arg(ap, int); s += va_arg(ap, int) * 1000; va_end(ap);
    return s;
}
double vdbl(int n, ...)
{
    va_list ap; double s = 0;
    va_start(ap, n);
    while (n--) s += va_arg(ap, double);
    va_end(ap);
    return s;
}
int vstruct(int n, ...)
{
    va_list ap; int s = 0;
    va_start(ap, n);
    while (n--) { struct c6 c = va_arg(ap, struct c6); s = s * 7 + c.v[0] + c.v[5]; s += va_arg(ap, int); }
    va_end(ap);
    return s;
}
long stk_al16(int a, int b, int c, int d, struct al16 s, int z)
{ return a + b + c + d + s.x * 100 + z * 10000; }
int bf_byval(int k, struct msbf s) { return k * 1000 + s.a * 10 + s.b; }
