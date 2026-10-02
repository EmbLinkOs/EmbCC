/* 64-bit integers converted to float and double, checked bit for bit
 * against a reference that rounds in integer arithmetic: round to nearest,
 * ties to even, exactly once. unsigned long long -> float went through a
 * double sum that is not exact and was rounded a second time when
 * narrowed: 2^60 + 2^36 + 1 came out 2^60 rather than 2^60 + 2^37 on
 * x86-64 and aarch64 (random programs found it). Values: the edges (0,
 * 1, 2^63, 2^64 - 1), ties and their neighbours at every bit position
 * where rounding happens, and a walk of mixed bit patterns; signed and
 * unsigned, both widths of result. */
// expect-exit: 42
typedef unsigned long long u64;
typedef long long i64;
union f32 { float f; unsigned u; };
union f64 { double d; u64 u; };

/* the correctly rounded bits of |x| as a float (mb = 23) or double (52) */
static u64 ref_bits(u64 x, int mb, int bias)
{
    if (x == 0)
        return 0;
    int p = 63;
    while (!(x >> p & 1)) p--;
    u64 mant;
    if (p <= mb) {
        mant = x << (mb - p);
    } else {
        int sh = p - mb;
        mant = x >> sh;
        u64 rem = x & ((1ULL << sh) - 1), half = 1ULL << (sh - 1);
        if (rem > half || (rem == half && (mant & 1)))
            mant++;
        if (mant >> (mb + 1)) { mant >>= 1; p++; }
    }
    return ((u64)(p + bias) << mb) | (mant & ((1ULL << mb) - 1));
}
static unsigned ref_u32f(u64 x) { return (unsigned)ref_bits(x, 23, 127); }
static u64 ref_u64d(u64 x) { return ref_bits(x, 52, 1023); }
static unsigned ref_s32f(i64 x)
{ return x < 0 ? 0x80000000u | ref_u32f(0 - (u64)x) : ref_u32f((u64)x); }
static u64 ref_s64d(i64 x)
{ return x < 0 ? (1ULL << 63) | ref_u64d(0 - (u64)x) : ref_u64d((u64)x); }

__attribute__((noinline)) static float  uf(u64 x) { return (float)x; }
__attribute__((noinline)) static double ud(u64 x) { return (double)x; }
__attribute__((noinline)) static float  sf(i64 x) { return (float)x; }
__attribute__((noinline)) static double sd(i64 x) { return (double)x; }

static int bad;
static void check(u64 x)
{
    union f32 a; union f64 b;
    a.f = uf(x); if (a.u != ref_u32f(x)) bad |= 1;
    b.d = ud(x); if (b.u != ref_u64d(x)) bad |= 2;
    a.f = sf((i64)x); if (a.u != ref_s32f((i64)x)) bad |= 4;
    b.d = sd((i64)x); if (b.u != ref_s64d((i64)x)) bad |= 8;
}

int main(void)
{
    check(0); check(1); check(~0ULL); check(1ULL << 63); check((1ULL << 63) - 1);
    check((1ULL << 60) + (1ULL << 36) + 1);
    for (int p = 24; p < 64; p++)          /* ties and neighbours, float */
        for (int s = 1; s <= p - 23 && s < 64; s += (p - 23) / 3 + 1) {
            u64 base = 1ULL << p, unit = 1ULL << (p - 23);
            u64 tie = base + unit / 2;
            check(tie); check(tie - 1); check(tie + 1);
            check(tie + unit); check(base | 1); check(base + (1ULL << (s - 1)));
        }
    for (int p = 53; p < 64; p++) {        /* and double */
        u64 base = 1ULL << p, unit = 1ULL << (p - 52);
        u64 tie = base + unit / 2;
        check(tie); check(tie - 1); check(tie + 1); check(tie + unit);
    }
    u64 x = 0x9E3779B97F4A7C15ULL;
    for (int i = 0; i < 2000; i++) {       /* mixed patterns, every magnitude */
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
        check(x); check(x >> (i % 64)); check(x | (1ULL << 63)); check(x & 0xFFFFFF0000000001ULL);
    }
    return bad ? bad : 42;
}
