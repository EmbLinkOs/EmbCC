/* A double's words written through a union, as fdlibm's SET_HIGH_WORD,
 * SET_LOW_WORD and INSERT_WORDS write them (src/opt/memfwd.c,
 * pass_punfwd): the double stored, one word stored over half of it, the
 * double loaded back -- which the optimizer turns into the double's bits
 * with that half replaced. Each result is checked against the same bits
 * put together with shifts and memcpy, for both halves, after a read of
 * the other half, twice in a row, and with the word a float's bits.
 * SET_HIGH_WORD(x, GET_HIGH_WORD(x) OP c) is then one 64-bit OP of x
 * (src/opt/fold.c, fold_word_insert): and, or, xor, add and sub, with
 * the carry out of the high word lost as the 32-bit operation loses it. */
// expect-exit: 42
#include <string.h>

typedef unsigned int u32;
typedef unsigned long long u64;

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
typedef union { double v; struct { u32 msw, lsw; } p; } shape;
#else
typedef union { double v; struct { u32 lsw, msw; } p; } shape;
#endif

static u64 bits(double d) { u64 b; memcpy(&b, &d, 8); return b; }

__attribute__((noinline)) double set_high(double d, u32 h)
{
    shape u; u.v = d; u.p.msw = h; return u.v;
}
__attribute__((noinline)) double set_low(double d, u32 l)
{
    shape u; u.v = d; u.p.lsw = l; return u.v;
}
__attribute__((noinline)) double my_fabs(double d)
{
    shape u; u.v = d; u32 h = u.p.msw;
    shape w; w.v = d; w.p.msw = h & 0x7fffffffu; return w.v;
}
#define HIGH_OP(name, op)                                              \
    __attribute__((noinline)) double name(double d)                    \
    {                                                                  \
        shape u; u.v = d; u32 h = u.p.msw;                             \
        shape w; w.v = d; w.p.msw = h op; return w.v;                  \
    }
HIGH_OP(high_or, | 0x3ff00000u)
HIGH_OP(high_xor, ^ 0x80000000u)
HIGH_OP(high_add, + 0x00100000u)
HIGH_OP(high_sub, - 0x7ff00001u)
HIGH_OP(high_and, & 0x800fffffu)

__attribute__((noinline)) double neg_low_twice(double d, u32 a, u32 b)
{
    shape u; u.v = d; u.p.msw ^= 0x80000000u; u.p.lsw = a; u.p.lsw += b;
    return u.v;
}
__attribute__((noinline)) double from_float(double d, float f)
{
    shape u; u.v = d; memcpy(&u.p.lsw, &f, 4); return u.v;
}

int main(void)
{
    if (sizeof(double) != 8)
        return 42;      /* RX and AVR: a double is four bytes, no words */
    static const double in[] = { 0.0, -0.0, 1.0, -2.5, 3.141592653589793,
                                 1e-310, 1e300, -7e22 };
    int bad = 0;
    for (unsigned i = 0; i < sizeof in / sizeof in[0]; i++) {
        double d = in[i];
        u64 b = bits(d);
        u32 h = 0x400921fbu + i, l = 0x54442d18u * (i + 1);
        if (bits(set_high(d, h)) != ((b & 0xffffffffull) | (u64)h << 32))
            bad |= 1;
        if (bits(set_low(d, l)) != ((b & ~0xffffffffull) | l))
            bad |= 2;
        if (bits(my_fabs(d)) != (b & 0x7fffffffffffffffull))
            bad |= 4;
        if (bits(neg_low_twice(d, l, h)) !=
            (((b ^ 0x8000000000000000ull) & ~0xffffffffull) | (u32)(l + h)))
            bad |= 8;
        u32 hb = (u32)(b >> 32);
        u64 lw = b & 0xffffffffull;
        if (bits(high_or(d)) != ((u64)(hb | 0x3ff00000u) << 32 | lw) ||
            bits(high_xor(d)) != ((u64)(hb ^ 0x80000000u) << 32 | lw) ||
            bits(high_add(d)) != ((u64)(hb + 0x00100000u) << 32 | lw) ||
            bits(high_sub(d)) != ((u64)(hb - 0x7ff00001u) << 32 | lw) ||
            bits(high_and(d)) != ((u64)(hb & 0x800fffffu) << 32 | lw))
            bad |= 32;
        float f = (float)i * 0.5f;
        u32 fb;
        memcpy(&fb, &f, 4);
        if (bits(from_float(d, f)) != ((b & ~0xffffffffull) | fb))
            bad |= 16;
    }
    return bad ? bad : 42;
}
