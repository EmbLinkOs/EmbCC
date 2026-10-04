#include "embedded-abi128.h"
extern void writec(int c);
extern void puts_(const char *s);

/* clang copies a by-reference argument and zero-fills with calls, and the
 * harness has no C library. */
void *memcpy(void *d, const void *s, __SIZE_TYPE__ n)
{
    unsigned char *p = d;
    const unsigned char *q = s;
    while (n--)
        *p++ = *q++;
    return d;
}

void *memset(void *d, int c, __SIZE_TYPE__ n)
{
    unsigned char *p = d;
    while (n--)
        *p++ = (unsigned char)c;
    return d;
}

static void hx(unsigned long long v)
{
    for (int i = 60; i >= 0; i -= 4)
        writec("0123456789abcdef"[(int)((v >> i) & 0xfULL)]);
}

static void pl(long double d)
{
    union { long double d; unsigned long long w[2]; } u;
    u.d = d;
    hx(u.w[1]);
    writec('_');
    hx(u.w[0]);
    writec('\n');
}

#ifdef __SIZEOF_INT128__
static void p(s128 v)
{
    hx((unsigned long)((unsigned __int128)v >> 64));
    writec('_');
    hx((unsigned long)v);
    writec('\n');
}
#endif

#if __SIZEOF_LONG__ == 8
static volatile long big = 0x123456789abcdefL;
#else
static volatile long big = 0x1234567L;
#endif

int main(void)
{
    long double a = 2.5L, b = -0.375L;
    struct wld d = { b };
    struct wld r;

    pl(l_odd(3, a, b));
    pl(l_split(1, 2, 3, 4, 5, 6, 7, a, b));
    pl(vld(3, 2, a, 3, b, 4, (long double)big));
    r = s_ld(6, d, 2);
    pl(r.d);
    pl(d.d);                            /* untouched by the callee */
#ifdef __SIZEOF_INT128__
    {
        s128 x = ((s128)big << 64) | 0xfedcba9876543210UL;
        s128 y = -x / 5;
        struct w128 w = { x };
        p(r_ret(big));
        p(p_odd(9, x));
        p(p_split(1, 2, 3, 4, 5, 6, 7, x));
        p(p_stack(1, 2, 3, 4, 5, 6, 7, 8, 9, x, y));
        p(v128(3, 1, x, 2, y, 3, (s128)big));
        p(v128(1, 5, y));
        p(s_odd(4, w, d).v);
    }
#endif
    puts_("==END==\n");
    return 0;
}
