/* The GCC bit builtins (ctz clz popcount ffs parity clrsb, plain and l/ll)
 * against the obvious loop that defines each, over edge values and a
 * thousand pseudo-random ones at both widths — plus the hint builtins.
 * EmbCC lowers the bit family from ordinary integer ops (irgen gen_bitop);
 * the loops below are the specification it must agree with.
 */
// expect-exit: 42
typedef unsigned long u64;

static int ref_popcount(u64 x) { int n = 0; while (x) { n += x & 1; x >>= 1; } return n; }
static int ref_ctz(u64 x, int bits) { int n = 0; while (n < bits && !(x >> n & 1)) n++; return n; }
static int ref_clz(u64 x, int bits) { int n = 0; while (n < bits && !(x >> (bits - 1 - n) & 1)) n++; return n; }
static int ref_clrsb(u64 x, int bits)
{
    int top = (int)(x >> (bits - 1) & 1), n = 0;
    while (n + 1 < bits && (int)(x >> (bits - 2 - n) & 1) == top) n++;
    return n;
}

static int check(u64 v)
{
    unsigned int u = (unsigned int)v;
    if (__builtin_popcount(u) != ref_popcount(u)) return 1;
    if (__builtin_popcountl(v) != ref_popcount(v)) return 2;
    if (__builtin_popcountll(v) != ref_popcount(v)) return 3;
    if (__builtin_parity(u) != (ref_popcount(u) & 1)) return 4;
    if (__builtin_parityl(v) != (ref_popcount(v) & 1)) return 5;
    if (__builtin_ffs((int)u) != (u ? ref_ctz(u, 32) + 1 : 0)) return 6;
    if (__builtin_ffsl((long)v) != (v ? ref_ctz(v, 64) + 1 : 0)) return 7;
    if (__builtin_clrsb((int)u) != ref_clrsb(u, 32)) return 8;
    if (__builtin_clrsbl((long)v) != ref_clrsb(v, 64)) return 9;
    if (u) {   /* ctz / clz of 0 are undefined */
        if (__builtin_ctz(u) != ref_ctz(u, 32)) return 10;
        if (__builtin_clz(u) != ref_clz(u, 32)) return 11;
    }
    if (v) {
        if (__builtin_ctzl(v) != ref_ctz(v, 64)) return 12;
        if (__builtin_clzll(v) != ref_clz(v, 64)) return 13;
    }
    return 0;
}

int main(void)
{
    static const u64 edge[] = {
        0, 1, 2, 3, 0x80, 0xFF, 0x8000, 0x80000000UL, 0xFFFFFFFFUL,
        0x100000000UL, 0x8000000000000000UL, ~0UL, 0x5555555555555555UL,
        0xAAAAAAAAAAAAAAAAUL, 0x00FF00FF00FF00FFUL, 0x7FFFFFFFFFFFFFFFUL,
    };
    for (unsigned i = 0; i < sizeof edge / sizeof edge[0]; i++) {
        int r = check(edge[i]);
        if (r) return r;
    }
    u64 x = 0x9E3779B97F4A7C15UL;
    for (int i = 0; i < 1000; i++) {
        x = x * 6364136223846793005UL + 1442695040888963407UL;
        int r = check(x);
        if (r) return 20 + r;
        r = check(x >> (i % 64));
        if (r) return 40 + r;
    }

    /* hints */
    int arr[4] = { 1, 2, 3, 4 };
    __builtin_prefetch(arr);
    __builtin_prefetch(arr, 1, 3);
    int *al = __builtin_assume_aligned(arr, 4);
    if (al != arr) return 60;
    if (__builtin_expect_with_probability(arr[2], 3, 0.9) != 3) return 61;
    int k = 5;
    if (__builtin_constant_p(k + arr[0])) return 62;      /* not constant */
    if (!__builtin_constant_p(3 * 7)) return 63;          /* constant */
    _Static_assert(__builtin_constant_p(sizeof(int) * 2), "an ICE");
    if (0)
        __builtin_trap();                                 /* never reached */
    return 42;
}
