#include <stdarg.h>
#include "tricore-abi.h"
long long fill(int a, long long b, int c) { return a * 1000000LL + b * 10 + c; }
int ptrs(char *a, int x, char *b, char *c, int y, char *d, char *e)
{ return (*a + *b * 2 + *c * 3 + *d * 5 + *e * 7) * x - y; }
char *ptr_ret(char *base, int off) { return base + off; }
const short *ptr_ret2(int k, const short *tab) { return tab + k; }
double fd(float a, int *p, double b, int c) { return a * 2 + *p + b / 4 + c; }
int big(struct t20 s, int k)
{ int r = 0; for (int i = 0; i < 5; i++) { r += s.v[i] * (i + 1); s.v[i] = -1; } return r + k; }
struct t3 r3(struct t3 s) { struct t3 r = { (char)(s.c + 1), (char)(s.a + 2), (char)(s.b + 3) }; return r; }
struct t8 r8(int a, struct t8 s) { struct t8 r = { s.b * a, s.a - a }; return r; }
struct t20 r20(int k, char *tag)
{ struct t20 r; for (int i = 0; i < 5; i++) r.v[i] = k * i + *tag; return r; }
long long many(int a, int b, int c, int d, int e, int f, int g, int h,
               int i, int j, long long k)
{ return a + 2*b + 3*c + 4*d + 5*e + 6*f + 7*g + 8*h + 9*i + 10*j + k; }
long long vmixed(int n, char *tag, ...)
{
    va_list ap; long long r = *tag;
    va_start(ap, tag);
    for (int i = 0; i < n; i++) {
        switch (i % 5) {
        case 0: r += *va_arg(ap, int *); break;
        case 1: r += (long long)(va_arg(ap, double) * 8); break;
        case 2: r += va_arg(ap, long long); break;
        case 3: { struct t20 s = va_arg(ap, struct t20); r += s.v[0] + s.v[4]; } break;
        default: r += va_arg(ap, int); break;
        }
    }
    va_end(ap);
    return r;
}
int apply(cmpfn f, const int *a, const int *b) { return f(a, b) * 10 + f(b, a); }
int cmp_int(const int *a, const int *b) { return (*a > *b) - (*a < *b); }
