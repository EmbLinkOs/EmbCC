/* AVR functions shaped to put values in the allocator's call-saved register
 * pairs (r2-r17), for probe.S to check each is restored. uint16_t
 * throughout, so the host computes the same answers. */
#include <stdint.h>
typedef uint16_t u16;
volatile u16 vsink;
static u16 nop(u16 x) { vsink = x; return (u16)(x ^ 0x55); }
u16 (*volatile pnop)(u16) = nop;

u16 leaf(u16 a) { return (u16)(a * 3 + 1); }
/* values live across calls: exactly what a call-saved pair is for */
u16 across(u16 a)
{
    u16 s = (u16)(a + 1), t = (u16)(a + 2), u = (u16)(a ^ 7), v = (u16)(a << 1);
    s = (u16)(s + pnop(t)); t = (u16)(t + pnop(u)); u = (u16)(u + pnop(v));
    v = (u16)(v + pnop(s));
    return (u16)(s * 3 + t * 5 + u * 7 + v * 11);
}
/* a loop with several live 16-bit values */
u16 loop(u16 a)
{
    u16 s = 0, x = a, y = (u16)(a >> 1), z = 1;
    for (u16 i = 0; i < 40; i++) {
        s = (u16)(s + (x ^ i));
        x = (u16)(x + y);
        y = (u16)(y ^ z);
        z = (u16)(z + 3);
        if (s & 1) s = (u16)(s + pnop(z));
    }
    return (u16)(s + x + y + z);
}
/* more live values than there are pairs */
u16 pressure(u16 a)
{
    u16 v0 = a, v1 = (u16)(a + 1), v2 = (u16)(a + 2), v3 = (u16)(a + 3),
        v4 = (u16)(a + 4), v5 = (u16)(a + 5), v6 = (u16)(a + 6),
        v7 = (u16)(a + 7), v8 = (u16)(a + 8), v9 = (u16)(a + 9);
    for (u16 i = 0; i < 5; i++) {
        v0 = (u16)(v0 + v9); v1 = (u16)(v1 ^ v8); v2 = (u16)(v2 + v7);
        v3 = (u16)(v3 ^ v6); v4 = (u16)(v4 + v5); v9 = (u16)(v9 + pnop(v0));
    }
    return (u16)(v0 ^ v1 ^ v2 ^ v3 ^ v4 ^ v5 ^ v6 ^ v7 ^ v8 ^ v9);
}

/* four-byte values: each takes a QUAD, two adjacent pairs */
typedef uint32_t u32;
u16 across32(u16 a)
{
    u32 s = (u32)a * 40503u, t = (u32)a << 17, u = s ^ 0x5a5a5a5au;
    s += pnop((u16)t); t ^= pnop((u16)(s >> 16)); u += pnop((u16)u);
    return (u16)((s + t * 3 + u * 5) >> 7);
}
u16 loop32(u16 a)
{
    u32 s = 1, x = a, y = 0x10001u * a;
    u16 z = 3;
    for (u16 i = 0; i < 25; i++) {
        s = s * 31 + (x ^ i);
        x += y >> 3;
        y ^= s;
        z = (u16)(z + (u16)x);
        if (s & 4) s += pnop(z);
    }
    return (u16)(s ^ (s >> 16) ^ x ^ y ^ z);
}

/* A call whose arguments reach DOWN into the call-saved registers: four
 * longs are r22, r18, r14 and r10. Loading them writes r10-r17, which the
 * caller must therefore save -- avr-gcc does, and EmbCC did not, at any
 * level. */
__attribute__((noinline)) u32 many(u32 a, u32 b, u32 c, u32 d)
{
    return a * 3 + (b ^ c) - d + pnop((u16)(a >> 3));
}
u16 manyargs(u16 a)
{
    return (u16)(many(a, (u32)a * 40503u, a ^ 0x1234u, (u32)a << 9) >> 2);
}

/* A value stored to its home from r22-r25 right before a loop's label,
 * then passed in r22-r25 again first thing in the body. The first time
 * round the registers still hold it; from the back edge they hold the
 * other call's result -- so the reload may be skipped only when nothing,
 * not even a label, came between (vld's last_st). */
__attribute__((noinline)) u32 twice(u32 v) { return v * 2 + (u32)pnop((u16)v); }
u16 relabel(u16 a)
{
    u32 x = twice(a), s = 0;
    u16 n = 4;
    do {
        s += twice(x);
    } while (--n);
    return (u16)(s ^ (s >> 16));
}

/* Values live ACROSS a runtime helper call -- a 32-bit divide and a
 * float add, which the IR does not show as calls. X (r26:r27) is a home
 * for a value that crosses no call, and a helper may use X freely, so
 * the allocator has to be told these ARE calls (a_calls_helper). */
__attribute__((noinline)) u16 acrossdiv(u16 a)
{
    u16 t = (u16)(a * 3 + 1), w = (u16)(a ^ 0x5a5a);
    u32 q = ((u32)a << 11) / (u32)(a | 3);
    u32 r = ((u32)a * 40503u) % (u32)(t | 1);
    return (u16)(t + w + (u16)q + (u16)r);
}
__attribute__((noinline)) u16 acrossflt(u16 a)
{
    u16 t = (u16)(a * 5 + 7);
    float f = (float)a * 1.5f + 0.25f;
    return (u16)(t + (u16)f);
}
