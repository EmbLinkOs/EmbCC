#include "mips-abi.h"
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
    writec('\n');
    { struct b1 s = { 0xA5 }; putn(take_b1(s, 7)); }
    { struct b2 s = { 0x12, 0xF3 }; struct h1 t = { -77 };
      putn(take_b2_h1(4, s, t)); }
    { struct b5 s = { { 1, 2, 3, 4, 5 } }; putn(take_b5_at3(10, 20, 30, s)); }
    { struct b1 s = { 9 }; struct b2 t = { 8, 7 }; struct h1 u = { -6 };
      struct b6 v = { { 100, -200, 300 } };
      putn(take_small_stack(1, 2, 3, 4, s, t, u, v)); }
    { long long r = ll_first(0x123456789ALL, -5);
      putn((long)(unsigned)(r >> 32)); putn((long)(unsigned)r); }
    { long long r = ll_stack(1, 2, 3, 4, 0x7766554433LL, 3);
      putn((long)(unsigned)(r >> 32)); putn((long)(unsigned)r); }
    { long long r = ll_mix(-0x10000001LL, 0x0F0F0F0F0F0FLL);
      putn((long)(unsigned)(r >> 32)); putn((long)(unsigned)r); }
    { unsigned long long r = ull_ret(0xCAFEu, 0xF00Du);
      putn((long)(unsigned)(r >> 32)); putn((long)(unsigned)r); }
    putd(d_stack(1, 2, 3, 4, 5, 0.5));
    { struct b2 s = ret_b2(40); putn(s.a); putn(s.b); }
    { struct h1 s = ret_h1(1234); putn(s.h); }
    { long long r = vll(3, 1LL, -2LL, 0x100000003LL);
      putn((long)(unsigned)(r >> 32)); putn((long)(unsigned)r); }
    { struct b1 x = { 3 }, y = { 250 }; struct b2 p = { 9, 4 }, q = { 1, 200 };
      putn(vsmall(4, x, p, y, q)); }
    puts_("\n==END==\n");
    return 0;
}
