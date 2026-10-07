#include "ppc-abi.h"
extern void writec(int c); extern void puts_(const char *s); extern void putn(long v);
static void putd(double d)
{
    union { double d; unsigned long long u; } x;
    x.d = d;
    putn((long)(unsigned)(x.u >> 32));
    putn((long)(unsigned)x.u);
}
static void putf(float f)
{
    union { float f; unsigned u; } x;
    x.f = f;
    putn((long)x.u);
}
static void putll(long long v)
{
    putn((long)(unsigned)((unsigned long long)v >> 32));
    putn((long)(unsigned)v);
}
/* va_list built here and walked by the callee (vlist_sum), compiled by
 * whichever compiler built that side */
int vlist_call(int n, ...)
{
    va_list ap; int r;
    va_start(ap, n);
    r = vlist_sum(n, ap);
    va_end(ap);
    return r;
}
int main(void)
{
    putll(ll_after_1(5, 0x123456789ALL));
    putll(ll_after_7(1, 2, 3, 4, 5, 6, 7, 0x7766554433LL, 9));
    putll(ll_after_6(1, 2, 3, 4, 5, 6, -0x10000001LL, 8));
    putn(ints_10(1, 2, 3, 4, 5, 6, 7, 8, 9, 10));
    putd(d_after_7(1, 2, 3, 4, 5, 6, 7, 0.5, 1.25f, 0.125));
    putd((double)ld_add(1.5L, 2.25L));
    writec('\n');
    { struct mod m = { { 1, 2, 3, 4, 5 } };
      putn(take_mod(m, 7)); putn(m.v[0]); putn(m.v[4]); }
    { struct s3 a = { 1, 2, 3 }; struct s8 b = { 4, 5 };
      putn(take_s3_s8(a, b, 6)); }
    { struct s12 s = { 1, 2, 3 }; struct s1 t = { 9 };
      putn(structs_late(1, 2, 3, 4, 5, 6, 7, 8, s, t)); }
    writec('\n');
    { struct s1 s = ret_s1(200); putn(s.a); }
    { struct s2 s = ret_s2(10); putn(s.a); putn(s.b); }
    { struct s3 s = ret_s3(20); putn(s.a); putn(s.b); putn(s.c); }
    { struct s5 s = ret_s5(30); for (int i = 0; i < 5; i++) putn(s.c[i]); }
    { struct s6 s = ret_s6(700); for (int i = 0; i < 3; i++) putn(s.h[i]); }
    { struct s7 s = ret_s7(40); for (int i = 0; i < 7; i++) putn(s.c[i]); }
    { struct s8 s = ret_s8(77); putn(s.a); putn(s.b); }
    { struct s12 s = ret_s12(5); putn(s.a); putn(s.b); putn(s.c); }
    { _Complex float z = ret_cf(1.5f, 2.5f); putf(__real__ z); putf(__imag__ z); }
    { _Complex double z = ret_cd(1.5, 2.5); putd(__real__ z); putd(__imag__ z); }
    { _Complex double z; _Complex float w;
      __real__ z = 1.0; __imag__ z = 2.0; __real__ w = 0.5f; __imag__ w = 0.25f;
      putd(take_cd(z, w)); }
    writec('\n');
    putll(vll_late(9, 1, 2LL, 3, 4LL, 5, 0x100000006LL, 7, 8LL, 9));
    putll(vll_late(3, 1, -2LL, 3));
    { struct s3 a = { 1, 2, 3 }, b = { 4, 5, 6 }; struct s12 x = { 7, 8, 9 }, y = { 10, 20, 30 };
      putn(vstructs(5, a, x, b, y, a)); }
    putd(vfloat(6, 1.0, 2.5, -0.25, 1e10, 3.0, 0.125));
    putn(vlist_call(7, 1, 2LL, 3.0, 4, 5LL, 6.5, 7));
    putn(vlist_other(7, 7, 6LL, 5.0, 4, 3LL, 2.5, 1));
    puts_("\n==END==\n");
    return 0;
}
