#include "mips64-abi.h"
extern void writec(int c); extern void puts_(const char *s); extern void putn(long v);
/* Floating values printed exactly: their bits. */
static void putd(double d) { union { double d; long u; } x; x.d = d; putn(x.u); }
static void putf(float f) { union { float f; int u; } x; x.f = f; putn(x.u); }
static void putld(long double d)
{
    union { long double d; long u[2]; } x;
    x.d = d;
    putn(x.u[0]); putn(x.u[1]);
}
static void put128(__int128 v) { putn((long)(v >> 64)); putn((long)v); }
int main(void)
{
    putn(many_l(1, 2, 3, 4, 5, 6, 7, 8, 9, 0x100000000L));
    putd(many_d(1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10));
    putn(many_i(1, 2, 3, 4, 5, 6, 7, 8, 9, 0xffffffffu));
    putf(fmul3(1.5f, 2.0f, -3.0f));
    { static volatile unsigned a = 1, b = 2;
      unsigned r = uret(a, b); putn(r == 0xffffffffu); putn(is_max(r)); putn((long)r); }
    putn(sext_back(-7)); putn(sext_back(0x7fffffff));
    putn(ret_sc(200)); putn(ret_uc(-3)); putn(ret_ss(40000)); putn(ret_us(-2));
    putn(ret_uc(-3) == 253); putn(ret_us(-2) > 60000);
    writec('\n');
    putld(ld_add(1.5L, 0x1p-80L)); putld(ld_after_int(3, 0.25L));
    putld(ld_stack(1, 2, 3, 4, 5, 6, 7, 1.5L, 9));
    put128(i128_after_int(5, (__int128)0x123456789abcdefL << 40));
    put128(i128_at7(1, 2, 3, 4, 5, 6, 7, -((__int128)1 << 100)));
    put128((__int128)i128_mul((unsigned __int128)0xffffffffffffffffUL << 3, 0x10001));
    writec('\n');
    { struct c3 s = { 1, 2, 3 }; putn(take_c3(9, s, 4)); }
    { struct i3 s = { 1, 2, 3 }; putn(take_i3(s, 5)); }
    { struct l3 s = { 10, 20, 30 }; putn(take_l3(1, s)); }
    { struct big b; for (int i = 0; i < 10; i++) b.v[i] = i + 1; putn(take_big(5, 6, b, 7)); }
    { struct al16 s = { 3, 4 }; putn(take_al16(1, s, 2)); }
    { struct i16 s = { ((__int128)5 << 64) | 6 }; putn(take_i16(1, s)); }
    { struct c3 s = { 7, 8, 9 }; struct s16 t = { { 1, 2, 3, 4, 5, 6, 7, 8 } };
      putn(take_c3_stack(1, 2, 3, 4, 5, 6, 7, 8, s, t)); }
    writec('\n');
    { struct c3 s = ret_c3(65); putn(s.a); putn(s.b); putn(s.c); }
    { struct i3 s = ret_i3(17); putn(s.a); putn(s.b); putn(s.c); }
    { struct l3 s = ret_l3(-3); putn(s.a); putn(s.b); putn(s.c); }
    { struct big b = ret_big(3); putn(b.v[0]); putn(b.v[4]); putn(b.v[9]); }
    putf(ret_f1(1.25f).a);
    { struct f2 s = ret_f2(1.5f, 2.5f); putf(s.a); putf(s.b); }
    { struct df s = ret_df(1.5, 2.5f); putd(s.a); putf(s.b); }
    { struct fd s = ret_fd(1.5f, 2.5); putf(s.a); putd(s.b); }
    { struct d2 s = ret_d2(1.5, 2.5); putd(s.a); putd(s.b); }
    { struct fi s = ret_fi(1.5f, -3); putf(s.a); putn(s.b); }
    { struct s16 s = ret_s16(5000); putn(s.h[0]); putn(s.h[3]); putn(s.h[7]); }
    putld(ret_ld1(1.0L).x);
    { _Complex float z = ret_cf(1.5f, 2.5f); putf(__real__ z); putf(__imag__ z); }
    { _Complex double z = ret_cd(1.5, 2.5); putd(__real__ z); putd(__imag__ z); }
    { _Complex long double z = ret_cld(1.5L, 2.5L); putld(__real__ z); putld(__imag__ z); }
    writec('\n');
    putn(vsum(4, 1L, -2L, 3L, 0x100000000L));
    putd(vdsum(3, 1.0, 2.5f, 4.25));
    putld(vldsum(2, 1, 0.5L, 2, 0.25L));
    putld(vldsum(4, 1, 1.0L, 2, 2.0L, 3, 3.0L, 4, 0x1p-70L));
    { struct c3 s = { 1, 2, 3 }; struct l3 l = { 100, 20, 3 };
      putn(vmixed(10, 10, 2.75, 3000000000L, s, l, 20, 4.5, 7L, s, l)); }
    for (long i = 0; i < 300000; i += 1000) far_array[i] = i * 3;
    putn(far_array[299000]); putn(far_elt(298000)); putn(far_elt(0));
    putn(pick(0)(21)); putn(pick(1)(7));
    puts_("\n==END==\n");
    return 0;
}
