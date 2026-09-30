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
/* a variable-length array: the frame is addressed from r7, which must
 * come back as it went in */
u32 vla(u32 a, u32 b, u32 c, u32 d)
{
    u32 v[(a & 7) + 4];
    for (u32 i = 0; i < (a & 7) + 4; i++) v[i] = i * b + c;
    return v[(a & 7) + 3] ^ v[0] ^ d ^ (u32)sizeof v;
}
/* the LOW scratch: a slot-resident value is worked on in whichever of
 * r0-r7 holds nothing live, and r4-r7 count only once the prologue saves
 * them. finalize() is __cxa_finalize's shape, where they first did not:
 * r0-r3 are all busy at the struct copy's address (this backend keeps a
 * copy's addresses in slots), r4 holds `dso` across the call, and r5-r7
 * are unsaved -- so a pick among them would be a clobber the probe sees
 * through lowscr, which calls it. */
struct hnd { void (*fn)(void *); void *arg; void *dso; };
struct hnd g_hs[4];
int g_hn;
static void hfn(void *p) { vsink = vsink * 3 + (u32)p; }
void finalize(void *dso)
{
    while (g_hn > 0) {
        int i = g_hn - 1;
        struct hnd h = g_hs[i];
        g_hn = i;
        if (dso && h.dso != dso)
            continue;
        if (h.fn)
            h.fn(h.arg);
    }
}
u32 lowscr(u32 a, u32 b, u32 c, u32 d)
{
    for (int k = 0; k < 4; k++) {
        g_hs[k].fn = k == 2 ? 0 : hfn;
        g_hs[k].arg = (void *)(a + b * (u32)k);
        g_hs[k].dso = (void *)(k & 1 ? c : d);
    }
    g_hn = 4;
    vsink = 0;
    finalize((void *)d);
    g_hn = 4;
    finalize(0);
    return vsink;
}
