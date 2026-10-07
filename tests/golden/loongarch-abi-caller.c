#include "loongarch-abi.h"
extern void writec(int c); extern void puts_(const char *s); extern void putn(long v);
/* A double's or float's value, printed exactly: its bits. */
static void putd(double d)
{
    union { double d; unsigned long u; } x;
    x.d = d;
    putn((long)(x.u >> 32));
    putn((long)(x.u & 0xffffffffUL));
}
static void putf(float f)
{
    union { float f; unsigned u; } x;
    x.f = f;
    putn((long)x.u);
}
static int neg(int x) { return -x; }
int main(void)
{
    volatile unsigned big_u = 0x80000001u;
    putn(u_ret(1) == 0x80000001u); putn((long)u_ret(1));
    putn(u_take(big_u, 3)); putn(u_take(7u, -1));
    putn(c_take((char)-5, -7, 250, -300, 65000)); putn(c_ret(200));
    putn(c_ret(200) < 0);
    writec('\n');
    putf(f_sum(1.5f, 2.25, -3.0f, 4));
    putd(d_var(3, 1.0, 2.5f, 4.25)); putd(d_var(1, -0.5));
    { struct f2 s = f2_make(1.25f, 2.5f); putf(s.a); putf(s.b); }
    { struct d2 s = d2_make(1.5, 2.5); putd(s.a); putd(s.b); }
    { struct fd s = fd_make(1.5f, 3.0); putf(s.f); putd(s.d); }
    { struct d2 x = { 1.0, 2.0 }, y = { 3.0, 4.0 };
      putd(d2_take(5, x, y)); putd(d2_split(1, 2, 3, 4, 5, 6, 7, x)); }
    writec('\n');
    { struct big b = { 1, 2, 3 };
      putn(big_sum(b, 4)); big_mod(b); putn(b.a); putn(b.b); putn(b.c); }
    putn(many(1, 2, 3, 4, 5, 6, 7, 8, -9, 10, -11, -12, 0x80000000u, 14));
    { struct a16 s = { 100, 200 };
      __int128 w = ((__int128)5 << 64) | 6;
      putn(v_mix(9, 2, w, 3, s, -4L)); }
    { struct mixc m = mixc_make(7); putn(m.c); putn(m.u); putn(m.s); putn(m.i); }
    { _Complex float z = cf_make(1.5f, 2.5f); putf(__real__ z); putf(__imag__ z); }
    { _Complex double z = cd_make(1.5, 2.5); putd(__real__ z); putd(__imag__ z); }
    writec('\n');
    putn(g_counter); putn(g_arr[3]); putn(bump(3)); putn(g_counter);
    putn(g_arr[2]); g_arr[0] = -1; putn(g_arr[0]);
    putn(pick_fn(0)(21)); putn(pick_fn(1)(7)); putn(call_fn(neg, 5));
    putn(call_fn(pick_fn(1), 9));
    puts_("\n==END==\n");
    return 0;
}
