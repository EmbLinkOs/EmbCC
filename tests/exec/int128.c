/* GNU __int128 (and __int128_t / __uint128_t): two eightbytes, 16-aligned.
 * Inline add/sub (with the carry), the bitwise ops, negation, equality and
 * ordering (signed and not), extension from and truncation to the narrower
 * types; libgcc's multiplication, division, shifts and float conversions;
 * static initializers folded in 128 bits; arguments, returns, members and
 * loops. Expected values computed independently (Python). */
// expect-exit: 42
typedef unsigned __int128 u128;
typedef __int128 i128;

#define W(hi, lo) ((u128)(hi) << 64 | (u128)(lo))

u128 g = W(0x0123456789abcdefUL, 0xfedcba9876543210UL);
static const i128 neg_static = -((i128)1 << 100) + 7;
__uint128_t alias_u = 3;
__int128_t alias_i = -3;

struct S { char c; u128 v; };

static u128 mul64(unsigned long a, unsigned long b) { return (u128)a * b; }
static i128 mix(int a, i128 b, long c, u128 d) { return a + b + c + (i128)d; }
static int eq(u128 a, u128 b) { return a == b; }

int main(void)
{
    if (!eq(mul64(0xffffffffffffffffUL, 0xffffffffffffffffUL),
            W(0xfffffffffffffffeUL, 1)))
        return 1;
    i128 n = -5;
    if ((u128)n != W(~0UL, 0xfffffffffffffffbUL) || n >= 0 || !(n < 0))
        return 2;
    if ((u128)0 - 1 != W(~0UL, ~0UL) || W(0, ~0UL) + 1 != W(1, 0))
        return 3;
    if ((u128)1 << 100 != W(1UL << 36, 0) || g >> 68 != W(0, 0x00123456789abcdeUL))
        return 4;
    if ((u128)((i128)-1024 >> 3) != W(~0UL, 0xffffffffffffff80UL))
        return 5;
    if ((g & 0xff00ff00ff00ff00UL) != W(0, 0xfe00ba0076003200UL) ||
        (g | (u128)1 << 127) != W(0x8123456789abcdefUL, 0xfedcba9876543210UL) ||
        (g ^ g ^ 7) != 7 || ~(u128)0 != W(~0UL, ~0UL))
        return 6;
    if (g / 1000000007u != W(0x0000000004e2fff8UL, 0xa480a8f47507e0e0UL) ||
        g % 1000000007u != 0x24ec4bf0)
        return 7;
    i128 big = (i128)-1000000000000000000L * 1000;
    if ((u128)(big / 7) != W(0xfffffffffffffff8UL, 0x4175797604c4924aUL) ||
        big % 7 != -6)
        return 8;
    if ((u128)1e30 != W(0x0000000c9f2c9cd0UL, 0x4675000000000000UL) ||
        (double)(i128)-3 != -3.0 || (double)g < 1.5123e36 || (double)g > 1.5124e36)
        return 9;
    if (!(g > mul64(3, 5)) || mul64(3, 5) > g || !((i128)-1 < 0) ||
        !((u128)-1 > 0) || (i128)g < -(i128)g)
        return 10;
    if ((unsigned long)g != 0xfedcba9876543210UL || (int)(g >> 120) != 1 ||
        (signed char)(g >> 64) != -17)
        return 11;
    unsigned u32 = (unsigned)(g >> 40);     /* all 32 bits, not 16 */
    long s32 = (int)(g >> 40);
    if (u32 != 0xeffedcbau || s32 != -268510022L ||
        (unsigned long)(unsigned)(g >> 40) != 0xeffedcbaUL)
        return 17;
    if ((u128)mix(1, 2, 3, 4) != 10 || neg_static >= 0 ||
        neg_static + ((i128)1 << 100) != 7)
        return 12;
    if (alias_u + (u128)alias_i != 0 || sizeof(i128) != 16 ||
        __alignof__(u128) != 16)
        return 13;
    struct S s = { 1, g };
    int off = (int)((char *)&s.v - (char *)&s);
    if (off != 16 || s.v != g)
        return 14;
    int bits = 0;
    for (u128 x = g; x; x >>= 1)
        bits += (int)(x & 1);
    if (bits != 64)
        return 15;
    u128 acc = 1;
    for (int k = 0; k < 30; k++)
        acc *= 17;
    if (acc != W(0x062a010ca13bb1fdUL, 0x4afad51525ac74e1UL))
        return 16;
    return 42;
}
