#include "embedded-abi128.h"
extern void writec(int c);
extern void puts_(const char *s);

static void hx(unsigned long v)
{
    for (int i = 60; i >= 0; i -= 4)
        writec("0123456789abcdef"[(int)((v >> i) & 0xfUL)]);
}

static void p(s128 v)
{
    hx((unsigned long)((unsigned __int128)v >> 64));
    writec('_');
    hx((unsigned long)v);
    writec('\n');
}

static void pl(long double d)
{
    union { long double d; s128 v; } u;
    u.d = d;
    p(u.v);
}

static volatile long big = 0x123456789abcdefL;

int main(void)
{
    s128 x = ((s128)big << 64) | 0xfedcba9876543210UL;
    s128 y = -x / 5;
    long double a = 2.5L, b = -0.375L;
    struct w128 w = { x };
    struct wld d = { b };

    p(r_ret(big));
    p(p_odd(9, x));
    p(p_split(1, 2, 3, 4, 5, 6, 7, x));
    p(p_stack(1, 2, 3, 4, 5, 6, 7, 8, 9, x, y));
    pl(l_odd(3, a, b));
    pl(l_split(1, 2, 3, 4, 5, 6, 7, a, b));
    p(v128(3, 1, x, 2, y, 3, (s128)big));
    p(v128(1, 5, y));
    pl(vld(3, 2, a, 3, b, 4, (long double)big));
    p(s_odd(4, w, d).v);
    puts_("==END==\n");
    return 0;
}
