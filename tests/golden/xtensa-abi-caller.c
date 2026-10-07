#include "xtensa-abi.h"
extern void writec(int c); extern void puts_(const char *s); extern void putn(long v);
/* A double's or float's value, printed exactly: its bits. */
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
    putn((long)(unsigned)v);
    putn((long)(unsigned)(v >> 32));
}
int caller_vlist(int n, va_list ap)
{
    int t = 0;
    for (int i = 0; i < n; i++) t += va_arg(ap, int) * (i + 1);
    return t;
}
static int pass_on(int n, ...)
{
    va_list ap; int r;
    va_start(ap, n);
    r = vlist(n, ap);
    va_end(ap);
    return r;
}
int main(void)
{
    putd(dadd(1.5, 2.25)); putf(fmul3(1.5f, 2.0f, -3.0f));
    putd(d_after_int(7, 0.5));
    putll(ll_after_5(1, 2, 3, 4, 5, 0x123456789LL, 6));
    { struct s12 s = { 7, 8, 9 }; putn(s12_after_4(1, 2, 3, 4, s, 5)); }
    putll(ll_stack(1, 2, 3, 4, 5, 6, 7, 3000000000LL, 8, 90000000000LL));
    putd(many_d(1.0, 2.0, 3.0, 4.0, 5)); putf(f_mixed(1, 2.5f, 3, 4.25f));
    writec('\n');
    putd(ret_d1(1.25).d);
    { struct di s = ret_di(4, 2.5); putn(s.i); putd(s.d); }
    { struct c3 s = ret_c3(65); putn(s.a); putn(s.b); putn(s.c); }
    { struct s12 s = ret_s12(11); putn(s.a); putn(s.b); putn(s.c); }
    { struct s16 s = ret_s16(13); putn(s.a); putn(s.b); putn(s.c); putn(s.d); }
    { struct s20 s = ret_s20(17); putn(s.v[0]); putn(s.v[2]); putn(s.v[4]); }
    { struct big b = ret_big(3); putn(b.v[0]); putn(b.v[4]); putn(b.v[8]); }
    { _Complex float z = ret_cf(1.5f, 2.5f); putf(__real__ z); putf(__imag__ z); }
    { _Complex double z = ret_cd(1.5, 2.5); putd(__real__ z); putd(__imag__ z); }
    writec('\n');
    { _Complex double z; __real__ z = 0.5; __imag__ z = 0.25; putd(take_cd(3, z, 4)); }
    { _Complex float z; __real__ z = 1.5f; __imag__ z = 2.5f; putf(take_cf(1, 2, 3, 4, 5, z)); }
    { struct di s = { 3, 0.75 }; putd(take_di(9, s)); }
    { struct c3 s = { 1, 2, 3 }; putn(take_c3(10, 20, 30, s)); }
    { struct big b; for (int i = 0; i < 9; i++) b.v[i] = i + 1; putn(take_big(5, b, 7)); }
    { struct fl2 s = { 1.5f, 2.0f }; putn(take_fl2(s, 3)); }
    putn(take_narrow(-5, 250, -300, 65000, 7, -128, -2, 200));
    putn(ret_sc(200)); putn(ret_uc(-3)); putn(ret_ss(40000)); putn(ret_us(-2));
    putn(ret_sc(200) < 0); putn(ret_ss(40000) < 0);
    writec('\n');
    putd(vdsum(3, 1.0, 2.5, 4.25)); putd(vdsum(1, -0.5));
    putd(vdsum(5, 1.0, 2.0, 3.0, 4.0, 5.0));
    { struct c3 s = { 1, 2, 3 };
      putn(vmixed(8, 10, 2.75, 3000000000LL, s, 20, 4.5, 7LL, s)); }
    putn(vlate(1, 2, 3, 4, 5, 6, 7LL, 8));
    putn(vsix(1, 2, 3, 4, 5, 6, 7, 8.0));
    { struct s12 s = { 7, 8, 9 }; putn(vstraddle(1, 2, 3, 4, s, 5, 6)); }
    putn(pass_on(4, 1, 2, 3, 4)); putn(pass_on(7, 1, 2, 3, 4, 5, 6, 7));
    putn(vforward(3, 5, 6, 7)); putn(vforward(8, 1, 1, 1, 1, 1, 1, 1, 9));
    putn(pick(0)(21)); putn(pick(1)(7));
    puts_("\n==END==\n");
    return 0;
}
