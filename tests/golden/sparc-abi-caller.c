#include "sparc-abi.h"
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
    putn((long)(unsigned)(v >> 32));
    putn((long)(unsigned)v);
}
static void putld(long double v)
{
    union { long double d; unsigned u[4]; } x;
    x.d = v;
    for (int k = 0; k < 4; k++) putn((long)x.u[k]);
}
int main(void)
{
    putd(d_after_1(7, 0.5)); putd(d_split(1, 2, 3, 4, 5, 0.25));
    putd(d_stack(1, 2, 3, 4, 5, 6, -1.5));
    putll(ll_after_1(3, 0x123456789LL)); putll(ll_split(1, 2, 3, 4, 5, 0x500000001LL));
    putll(ll_stack(1, 2, 3, 4, 5, 6, 0x700000000LL, 9)); putll(ll_ret(0x89abcdefu, 0x01234567u));
    putf(f_mixed(1, 2.5f, 3, 4.25f));
    writec('\n');
    { struct s1 s = { 200 }; putn(take_s1(s, 4)); }
    { struct s3 s = { 1, 2, 3 }; putn(take_s3(10, s)); }
    { struct s8 s = { 3, 4 }; putn(take_s8_write(s)); putn(s.a); putn(s.b); }
    { struct big b; for (int i = 0; i < 9; i++) b.v[i] = i + 1;
      putn(take_big(5, b, 7)); putn(b.v[0]); }
    { union un u; u.i = 0; u.c[0] = 3; u.c[5] = 4; putn(take_un(u, 1)); }
    { struct s3 g = { 1, 2, 3 }; struct s8 h = { 4, 5 };
      putn(take_many(1, 2, 3, 4, 5, 6, g, h, 7)); }
    { _Complex double z; __real__ z = 1.5; __imag__ z = 2.25; putd(take_cd(z, 3)); }
    { _Complex float z; __real__ z = 4.5f; __imag__ z = 0.5f; putf(take_cf(3, z)); }
    writec('\n');
    { struct s1 s = ret_s1(5); putn(s.a); }
    { struct s3 s = ret_s3(65); putn(s.a); putn(s.b); putn(s.c); }
    putd(ret_sd(1.25).d);
    { struct big b = ret_big(3); putn(b.v[0]); putn(b.v[4]); putn(b.v[8]); }
    { union un u = ret_un(7); putn(u.c[0]); putn(u.c[5]); }
    { _Complex float z = ret_cf(1.5f, 2.5f); putf(__real__ z); putf(__imag__ z); }
    { _Complex double z = ret_cd(1.5, 2.5); putd(__real__ z); putd(__imag__ z); }
    putld(ld_pick(0, 1.5L, -2.75L)); putld(ld_pick(1, 1.5L, -2.75L));
    putld(ld_stack(1, 2, 3, 4, 5, 6, 3.0L));
    writec('\n');
    putn(take_narrow(-5, 250, -300, 65000, 7, -128, -2));
    putn(ret_sc(200)); putn(ret_uc(-3)); putn(ret_ss(40000)); putn(ret_us(-2));
    putn(ret_sc(200) < 0); putn(ret_ss(40000) < 0);
    writec('\n');
    putd(vdsum(3, 1.0, 2.5, 4.25)); putd(vdsum(1, -0.5));
    putll(vll(4, 0x100000002LL, 5, 0x300000004LL, 6));
    { struct s3 a = { 1, 2, 3 }; struct s8 b = { 4, 5 };
      struct s3 c = { 6, 7, 8 }; struct s8 d = { 9, 10 };
      putn(vstruct(2, a, 2.5, b, c, 7.0, d)); }
    far_array[9000] = 77; putn(far_elt(3));
    putn(pick(0)(5)); putn(pick(1)(5));
    puts_("\n==END==\n");
    return 0;
}
