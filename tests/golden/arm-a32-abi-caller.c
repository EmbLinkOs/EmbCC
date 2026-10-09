#include "arm-a32-abi.h"
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
int main(void)
{
    putd(dadd(1.5, 2.25)); putf(fmul3(1.5f, 2.0f, -3.0f));
    putd(d_after_int(7, 0.5)); putd(d_after_3(1, 2, 3, 0.25));
    { long long r = ll_after_3(1, 2, 3, 0x123456789LL);
      putn((long)(unsigned)r); putn((long)(unsigned)(r >> 32)); }
    putd(many_d(1.0, 2.0, 3.0, 4.0, 5)); putf(f_mixed(1, 2.5f, 3, 4.25f));
    writec('\n');
    putd(ret_d1(1.25).d);
    { struct di s = ret_di(4, 2.5); putn(s.i); putd(s.d); }
    { struct c3 s = ret_c3(65); putn(s.a); putn(s.b); putn(s.c); }
    { struct big b = ret_big(3); putn(b.v[0]); putn(b.v[4]); putn(b.v[8]); }
    { _Complex float z = ret_cf(1.5f, 2.5f); putf(__real__ z); putf(__imag__ z); }
    { _Complex double z = ret_cd(1.5, 2.5); putd(__real__ z); putd(__imag__ z); }
    writec('\n');
    { struct di s = { 3, 0.75 }; putd(take_di(9, s)); }
    { struct c3 s = { 1, 2, 3 }; putn(take_c3(10, 20, 30, s)); }
    { struct big b; for (int i = 0; i < 9; i++) b.v[i] = i + 1; putn(take_big(5, b, 7)); }
    { struct fl2 s = { 1.5f, 2.0f }; putn(take_fl2(s, 3)); }
    putn(take_narrow(-5, 250, -300, 65000, 7, -128));
    putn(ret_sc(200)); putn(ret_uc(-3)); putn(ret_ss(40000)); putn(ret_us(-2));
    putn(ret_sc(200) < 0); putn(ret_ss(40000) < 0);
    writec('\n');
    putd(vdsum(3, 1.0, 2.5, 4.25)); putd(vdsum(1, -0.5));
    { struct c3 s = { 1, 2, 3 };
      putn(vmixed(8, 10, 2.75, 3000000000LL, s, 20, 4.5, 7LL, s)); }
    for (int i = 0; i < 10000; i++) far_array[i] = i * 3;
    putn(far_array[9000]); putn(far_elt(9001)); putn(far_array[0]);
    putn(pick(0)(21)); putn(pick(1)(7));
    puts_("\n==END==\n");
    return 0;
}
