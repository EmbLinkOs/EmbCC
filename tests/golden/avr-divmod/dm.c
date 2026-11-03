/* / and % at every width, as clang compiles them for AVR: calls of
 * avr-gcc's __udivmodqi4 ... __divmodsi4, which lib/rt/avrdivmod.c is,
 * against a long division written with shifts alone. Values live across
 * the calls in the registers the conventions promise to keep. */
void puts_(const char *s);
void putn(long v);

typedef unsigned long u32;
typedef long i32;

static u32 udiv_ref(u32 a, u32 b, u32 *rem, int bits)
{
    u32 q = 0, r = 0;
    for (int k = bits - 1; k >= 0; k--) {
        r = (r << 1) | ((a >> k) & 1);
        if (r >= b) { r -= b; q |= (u32)1 << k; }
    }
    *rem = r;
    return q;
}

/* the signed one: truncated toward zero, the remainder the dividend's sign */
static i32 sdiv_ref(i32 a, i32 b, i32 *rem, int bits)
{
    u32 m = bits == 32 ? 0xffffffffUL : ((u32)1 << bits) - 1;
    u32 ua = (u32)(a < 0 ? -(u32)a : (u32)a) & m, ub = (u32)(b < 0 ? -(u32)b : (u32)b) & m, r;
    u32 q = udiv_ref(ua, ub, &r, bits);
    *rem = a < 0 ? -(i32)r : (i32)r;
    return (a < 0) != (b < 0) ? -(i32)q : (i32)q;
}

static volatile u32 seed = 12345;

int main(void)
{
    int bad = 0;
    u32 x = seed;
    for (int k = 0; k < 400; k++) {
        x = x * 1103515245UL + 12345UL;
        u32 a = x, b = (x >> 7) ^ (x << 3);
        if (k & 1) b >>= (k % 24);          /* small divisors too */
        if (!b) b = 1;
        u32 r, q;
        /* unsigned, 8, 16 and 32 bits */
        unsigned char a8 = (unsigned char)a, b8 = (unsigned char)(b | 1);
        q = udiv_ref(a8, b8, &r, 8);
        bad |= ((unsigned char)(a8 / b8) != q || (unsigned char)(a8 % b8) != r) << 0;
        unsigned a16 = (unsigned)a, b16 = (unsigned)(b | 1);
        q = udiv_ref(a16, b16, &r, 16);
        bad |= ((unsigned)(a16 / b16) != q || (unsigned)(a16 % b16) != r) << 1;
        q = udiv_ref(a, b, &r, 32);
        bad |= (a / b != q || a % b != r) << 2;
        /* signed, 8, 16 and 32 bits (not the one overflowing quotient) */
        signed char s8 = (signed char)a, t8 = (signed char)(b | 1);
        i32 sr, sq;
        if (!(s8 == -128 && t8 == -1)) {
            sq = sdiv_ref(s8, t8, &sr, 8);
            bad |= ((signed char)(s8 / t8) != sq || (signed char)(s8 % t8) != sr) << 3;
        }
        int s16 = (int)a, t16 = (int)(b | 1);
        if (!(s16 == -32767 - 1 && t16 == -1)) {
            sq = sdiv_ref(s16, t16, &sr, 16);
            bad |= ((int)(s16 / t16) != sq || (int)(s16 % t16) != sr) << 4;
        }
        i32 s32 = (i32)a, t32 = (i32)b;
        if (!(s32 == -2147483647L - 1 && t32 == -1)) {
            sq = sdiv_ref(s32, t32, &sr, 32);
            bad |= (s32 / t32 != sq || s32 % t32 != sr) << 5;
        }
    }
    putn(bad ? bad : 42);
    puts_("\n==END==\n");
    for (;;)
        ;
}
