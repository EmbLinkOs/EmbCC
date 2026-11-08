/* A bit tested by its number, `(x & (1 << n)) != 0`, which the optimizer
 * turns into `(x >> n) & 1` (pass_bittest): for every bit of 8-, 16-,
 * 32- and 64-bit values, signed and unsigned, as a value (!= 0 and
 * == 0) and as a branch, against the same bit found by division. Signed
 * values have their top bit set, where an arithmetic shift would bring
 * copies of it down -- the bit at 0 is the same either way, which is the
 * point. A mask used for anything else as well must be left alone: the
 * last form keeps it and uses it twice. */
// expect-exit: 42
typedef unsigned long long u64;

__attribute__((noinline)) int ne8(unsigned char x, int n) { return (x & (1u << n)) != 0; }
__attribute__((noinline)) int eq16(unsigned short x, int n) { return (x & (1u << n)) == 0; }
__attribute__((noinline)) int ne32(unsigned x, int n) { return (x & (1u << n)) != 0; }
__attribute__((noinline)) int eq32s(int x, int n) { return (x & (1 << n)) == 0; }
__attribute__((noinline)) int ne64(u64 x, int n) { return (x & (1ull << n)) != 0; }
__attribute__((noinline)) int eq64s(long long x, int n) { return (x & (1ll << n)) == 0; }
__attribute__((noinline)) int br32(unsigned x, int n)
{
    if (x & (1u << n))
        return 7;
    return 3;
}
__attribute__((noinline)) int br64(u64 x, int n)
{
    if (!(x & (1ull << n)))
        return 3;
    return 7;
}
/* the mask is also returned: no rewrite */
__attribute__((noinline)) unsigned both(unsigned x, int n, unsigned *mask)
{
    unsigned m = 1u << n;
    *mask = m;
    return (x & m) != 0;
}

static int bit_of(u64 x, int n)
{
    while (n-- > 0)
        x /= 2;
    return (int)(x % 2);
}

int main(void)
{
    static const u64 vals[] = { 0x8000000000000001ULL, 0x0123456789abcdefULL,
                                0xfedcba9876543210ULL, 0, ~0ULL, 0x80000000ULL };
    for (unsigned k = 0; k < sizeof vals / sizeof vals[0]; k++) {
        u64 v = vals[k];
        for (int n = 0; n < 64; n++) {
            int b = bit_of(v, n);
            if (n < 8 && ne8((unsigned char)v, n) != bit_of((unsigned char)v, n))
                return 1;
            if (n < 16 && eq16((unsigned short)v, n) != !bit_of((unsigned short)v, n))
                return 2;
            if (n < 32 && ne32((unsigned)v, n) != bit_of((unsigned)v, n))
                return 3;
            if (n < 31 && eq32s((int)(unsigned)v, n) != !bit_of((unsigned)v, n))
                return 4;
            if (ne64(v, n) != b)
                return 5;
            if (n < 63 && eq64s((long long)v, n) != !b)
                return 6;
            if (n < 32 && br32((unsigned)v, n) != (bit_of((unsigned)v, n) ? 7 : 3))
                return 7;
            if (br64(v, n) != (b ? 7 : 3))
                return 8;
            if (n < 32) {
                unsigned m = 0;
                if (both((unsigned)v, n, &m) != (unsigned)bit_of((unsigned)v, n) ||
                    m != 1u << n)
                    return 9;
            }
        }
    }
    return 42;
}
