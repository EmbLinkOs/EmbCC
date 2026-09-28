/* Functions of every shape that makes the Thumb backend reach for a
 * callee-saved register -- its r9-r11 scratch, or the allocator's r4-r8 --
 * for probe.S to check that each one is restored. All take four words and
 * return one, so one probe calls them all. */
typedef unsigned u32;
typedef unsigned long long u64;
volatile u32 vsink;

u32 leaf(u32 a, u32 b, u32 c, u32 d) { return a + b + c + d; }
/* the arguments rotated: a parallel move with a cycle, which r9 breaks */
static u32 rot4(u32 a, u32 b, u32 c, u32 d) { return a * 1 + b * 2 + c * 3 + d * 4; }
u32 swapper(u32 a, u32 b, u32 c, u32 d) { return rot4(d, a, b, c) + rot4(b, c, d, a); }
/* 64-bit arithmetic: the pair lowerings use r9-r11 */
u32 wide(u32 a, u32 b, u32 c, u32 d)
{
    u64 x = ((u64)a << 32 | b) * ((u64)c << 32 | d);
    u64 y = x / (d | 1);
    return (u32)(x >> 29) ^ (u32)y ^ (u32)(y >> 32);
}
/* a frame past the 16-bit and 12-bit offsets: addresses built in r10 */
u32 bigframe(u32 a, u32 b, u32 c, u32 d)
{
    volatile u32 buf[1200];
    for (u32 i = 0; i < 1200; i++) buf[i] = i * a + b;
    return buf[1199] + buf[c % 1200] + buf[d % 1200] + buf[0];
}
/* values live across calls: the allocator's r4-r8 */
static u32 nop(u32 x) { vsink = x; return x ^ 0x55; }
u32 (*volatile pnop)(u32) = nop;
u32 across(u32 a, u32 b, u32 c, u32 d)
{
    u32 s = a + 1, t = b + 2, u = c + 3, v = d + 4, w = a ^ d;
    s += pnop(t); t += pnop(u); u += pnop(v); v += pnop(w);
    return s * 3 + t * 5 + u * 7 + v * 11 + w;
}
u32 sw(u32 a, u32 b, u32 c, u32 d)
{
    switch (a & 7) {
    case 0: return b; case 1: return c * d; case 2: return b - c;
    case 3: return d << 3; case 4: return b ^ d; case 5: return c / (d | 1);
    default: return a + b + c + d;
    }
}
u32 divs(u32 a, u32 b, u32 c, u32 d)
{
    int x = (int)(a * 7919u) / (int)(b | 1), y = (int)c % (int)(d | 1);
    return (u32)(x * 3 - y) + a / (c | 1) + b % (d | 1);
}
u32 flt(u32 a, u32 b, u32 c, u32 d)
{
    double x = (double)a / (b + 1.0) + (double)c * 0.5;
    float y = (float)d * 1.25f - (float)a;
    return (u32)(x * 1000.0) + (u32)(y > 0 ? y : -y);
}
