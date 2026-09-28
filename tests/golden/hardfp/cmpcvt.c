/* Float COMPARISONS and CONVERSIONS on the FPU (vcmp/vcmpe + vmrs, vcvt),
 * compared with the host bit for bit.
 *
 * The comparisons are where a wrong condition code hides: every predicate
 * but != must be FALSE when either side is a NaN, and ARM's signed
 * conditions (LT, LE) are TRUE on an unordered result -- so a compiler
 * that maps < to LT gets 1 < NaN wrong and nothing else. Each predicate is
 * checked as a VALUE (the 0/1 materialised) and as a BRANCH (fused with the
 * compare), because those are different code paths with different
 * conditions: a branch on the false side uses the inverse. */
void writec(int c);
void puts_(const char *s);
typedef unsigned u32;
union F { float f; u32 u; };
static void hx(u32 v)
{
    for (int i = 28; i >= 0; i -= 4)
        writec("0123456789abcdef"[(v >> i) & 15]);
    writec(' ');
}
static float mk(u32 u) { union F x; x.u = u; return x.f; }
static u32 bits(float f) { union F x; x.f = f; return x.u; }

volatile u32 vals[] = { 0x3f800000u /* 1 */, 0xbf800000u /* -1 */,
                        0x00000000u /* +0 */, 0x80000000u /* -0 */,
                        0x7f800000u /* inf */, 0xff800000u /* -inf */,
                        0x7fc00000u /* NaN */, 0x40490fdbu /* pi */ };
#define NV 8

static u32 as_values(float a, float b)
{
    return (u32)(a < b) | (u32)(a <= b) << 1 | (u32)(a > b) << 2 |
           (u32)(a >= b) << 3 | (u32)(a == b) << 4 | (u32)(a != b) << 5;
}
static u32 as_branches(float a, float b)
{
    u32 r = 0;
    if (a < b)  r |= 1;
    if (a <= b) r |= 2;
    if (a > b)  r |= 4;
    if (a >= b) r |= 8;
    if (a == b) r |= 16;
    if (a != b) r |= 32;
    if (!(a < b))  r |= 64;          /* the inverse, which a NaN makes true */
    if (!(a >= b)) r |= 128;
    return r;
}

volatile int iv[] = { 0, 1, -1, 7, -7, 16777217, -16777217, 2147483647,
                      -2147483647 - 1, 123456789 };
volatile unsigned uv[] = { 0u, 1u, 2147483648u, 4294967295u, 3000000000u,
                           16777217u };
volatile u32 fv[] = { 0x3fe66666u /* 1.8 */, 0xc02ccccdu /* -2.7 */,
                      0x3effffffu /* just under 0.5 */, 0x4f000000u /* 2^31 */,
                      0x4e800000u /* 2^30 */, 0xceffffffu /* just above -2^31 */,
                      0x4f32d05eu /* 3e9 */ };

int main(void)
{
    u32 h = 0, g = 0;
    for (int i = 0; i < NV; i++)
        for (int j = 0; j < NV; j++) {
            float a = mk(vals[i]), b = mk(vals[j]);
            h = h * 31 + as_values(a, b);
            g = g * 31 + as_branches(a, b);
        }
    hx(h); hx(g);
    /* against a constant, which folds into the instruction's operand */
    {
        float x = mk(vals[7]);
        hx((u32)(x > 3.0f) | (u32)(x < 4.0f) << 1 | (u32)(x == 0.0f) << 2);
        hx(bits(x * 2.5f)); hx(bits(x - 1.0f)); hx(bits(x / 3.0f));
    }
    puts_("\n");
    for (int i = 0; i < 10; i++) hx(bits((float)iv[i]));
    for (int i = 0; i < 6; i++) hx(bits((float)uv[i]));
    puts_("\n");
    for (int i = 0; i < 7; i++) {
        float f = mk(fv[i]);
        /* Only in range: converting a float the type cannot hold is
         * undefined, and x86 and ARM really do answer it differently. */
        if (i != 3 && i != 6) {
            hx((u32)(int)f);
            hx((u32)(short)(int)f & 0xffffu);
        }
        if (f >= 0) hx((unsigned)f);
    }
    puts_("\n==END==\n");
    return 0;
}
