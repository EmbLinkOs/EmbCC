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
