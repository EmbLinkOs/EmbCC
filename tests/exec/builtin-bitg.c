/* GCC's type-generic bit builtins -- __builtin_clzg and ctzg (with and
 * without the value for 0), popcountg, parityg, ffsg and clrsbg -- and
 * C23's __builtin_stdc_* (GCC 15), on every unsigned width and on
 * unsigned __int128 where there is one, against loops that count the
 * bits one at a time. And the bit builtins of constants are constants:
 * in _Static_assert, a static initializer, an array size, a case label. */
// expect-exit: 42

typedef unsigned long long ull;

static int rclz(ull x, int b) { int n = 0; while (n < b && !(x >> (b - 1 - n) & 1)) n++; return n; }
static int rctz(ull x, int b) { int n = 0; while (n < b && !(x >> n & 1)) n++; return n; }
static int rpop(ull x, int b) { int n = 0; for (int i = 0; i < b; i++) n += (int)(x >> i & 1); return n; }
static ull mask(int b) { return b == 64 ? ~0ull : (1ull << b) - 1; }
static ull rrotl(ull x, long long n, int b)
{
    int k = (int)(((n % b) + b) % b);
    return k ? ((x << k) | (x >> (b - k))) & mask(b) : x;
}

static volatile ull seeds[] = {
    0, 1, 2, 3, 5, 0x80, 0x81, 0xff, 0x100, 0x7fff, 0x8000, 0x8001, 0xffff,
    0x12345678, 0x80000000, 0x80000001, 0xffffffff, 0x100000000ull,
    0xdeadbeefcafef00dull, 0x8000000000000000ull, 0x8000000000000001ull,
    0xffffffffffffffffull, 0x0123456789abcdefull,
};
/* rotate counts; a negative one turns EmbCC's rotate the other way, and
 * is undefined behaviour in GCC's */
static volatile long long amounts[] = {
    0, 1, 7, 8, 15, 31, 33, 63, 64, 65, 1000,
#ifdef __EMBCC__
    -1, -9,
#endif
};

#define N(a) (sizeof (a) / sizeof (a)[0])

/* every form on T (unsigned, B bits) and S (its signed twin); bit k of bad */
#define TESTW(T, S, B_, k) do {                                              \
    const int B = (B_);                                                      \
    for (unsigned i = 0; i < N(seeds); i++) {                                \
        T x = (T)seeds[i];                                                   \
        ull v = (ull)x, nv = ~v & mask(B);                                   \
        int lz = rclz(v, B), tz = rctz(v, B), pc = rpop(v, B);               \
        int f = 0;                                                           \
        f |= __builtin_clzg(x, 99) != (v ? lz : 99);                         \
        f |= __builtin_ctzg(x, -7) != (v ? tz : -7);                         \
        if (v) f |= __builtin_clzg(x) != lz || __builtin_ctzg(x) != tz;      \
        f |= __builtin_popcountg(x) != pc;                                   \
        f |= __builtin_parityg(x) != (pc & 1);                               \
        f |= __builtin_ffsg((S)x) != (v ? tz + 1 : 0);                       \
        f |= __builtin_clrsbg((S)x) != rclz(v >> (B - 1) ? nv : v, B) - 1;   \
        f |= __builtin_stdc_leading_zeros(x) != (unsigned)lz;                \
        f |= __builtin_stdc_leading_ones(x) != (unsigned)rclz(nv, B);        \
        f |= __builtin_stdc_trailing_zeros(x) != (unsigned)tz;               \
        f |= __builtin_stdc_trailing_ones(x) != (unsigned)rctz(nv, B);       \
        f |= __builtin_stdc_first_leading_zero(x) != (nv ? (unsigned)rclz(nv, B) + 1 : 0u); \
        f |= __builtin_stdc_first_leading_one(x) != (v ? (unsigned)lz + 1 : 0u); \
        f |= __builtin_stdc_first_trailing_zero(x) != (nv ? (unsigned)rctz(nv, B) + 1 : 0u); \
        f |= __builtin_stdc_first_trailing_one(x) != (v ? (unsigned)tz + 1 : 0u); \
        f |= __builtin_stdc_count_zeros(x) != (unsigned)(B - pc);            \
        f |= __builtin_stdc_count_ones(x) != (unsigned)pc;                   \
        f |= __builtin_stdc_has_single_bit(x) != (pc == 1);                  \
        f |= __builtin_stdc_bit_width(x) != (unsigned)(B - lz);              \
        f |= __builtin_stdc_bit_floor(x) != (T)(v ? 1ull << (B - 1 - lz) : 0); \
        {                                                                    \
            int c = v <= 1 ? 0 : B - rclz(v - 1, B);                         \
            f |= __builtin_stdc_bit_ceil(x) != (T)(v <= 1 ? 1 : c >= B ? 0 : 1ull << c); \
        }                                                                    \
        for (unsigned j = 0; j < N(amounts); j++) {                          \
            long long n = amounts[j];                                        \
            f |= __builtin_stdc_rotate_left(x, n) != (T)rrotl(v, n, B);      \
            f |= __builtin_stdc_rotate_right(x, n) != (T)rrotl(v, -(n % B), B); \
        }                                                                    \
        if (f)                                                               \
            bad |= 1 << (k);                                                 \
    }                                                                        \
} while (0)

#define IS(e, T) _Generic((e), T: 1, default: 0)

/* constants: GCC and clang fold these, and so must EmbCC */
_Static_assert(__builtin_popcount(0xf0u) == 4, "popcount");
_Static_assert(__builtin_clz(1) == 8 * sizeof(int) - 1, "clz");
_Static_assert(__builtin_ctzll(1ull << 40) == 40, "ctzll");
_Static_assert(__builtin_ffsl(0x100) == 9, "ffsl");
_Static_assert(__builtin_parity(7) == 1, "parity");
_Static_assert(__builtin_clrsb(-1) == 8 * sizeof(int) - 1, "clrsb");
_Static_assert(__builtin_bswap16(0x1234) == 0x3412, "bswap16");
_Static_assert(__builtin_bswap32(0x12345678) == 0x78563412, "bswap32");
_Static_assert(__builtin_bswap64(0x0102030405060708ull) == 0x0807060504030201ull, "bswap64");
_Static_assert(__builtin_clzg((unsigned char)1) == 7, "clzg");
_Static_assert(__builtin_ctzg(0u, 77) == 77, "ctzg of 0");
_Static_assert(__builtin_stdc_bit_ceil(5u) == 8, "bit_ceil");
_Static_assert(__builtin_stdc_bit_ceil(0x80000001u) == 0, "bit_ceil past the type");
_Static_assert(__builtin_stdc_bit_floor((unsigned short)0x1234) == 0x1000, "bit_floor");
_Static_assert(__builtin_stdc_rotate_left((unsigned char)0x81, 7) == 0xc0, "rotate_left");
_Static_assert(__builtin_stdc_has_single_bit(64u), "has_single_bit");
static const int field_shift = __builtin_ctz(0x40);
static char sized[__builtin_popcountg(0x7u)];

static int classify(unsigned v)
{
    switch (v) {
    case __builtin_ffs(8): return 1;
    case __builtin_stdc_bit_width(255u): return 2;
    default: return 0;
    }
}

/* -DBITG_ONLY=k runs width k alone (0 char ... 4 long long, 6 the
 * rest): an ATmega328P's 32 KiB hold one at a time (avr-bitg.sh) */
#ifdef BITG_ONLY
#define W(k) (BITG_ONLY == (k))
#else
#define W(k) 1
#endif

int main(void)
{
    int bad = 0;
    if (W(0)) TESTW(unsigned char, signed char, 8, 0);
    if (W(1)) TESTW(unsigned short, short, 16, 1);
    if (W(2)) TESTW(unsigned int, int, 8 * (int)sizeof(int), 2);
    if (W(3)) TESTW(unsigned long, long, 8 * (int)sizeof(long), 3);
    if (W(4)) TESTW(unsigned long long, long long, 64, 4);
#ifdef __SIZEOF_INT128__
    {
        /* unsigned __int128: values whose halves differ, against the
         * 64-bit references on each half */
        static const struct { ull hi, lo; } w[] = {
            { 0, 0 }, { 0, 1 }, { 1, 0 }, { 0x8000000000000000ull, 0 },
            { 0, 0x8000000000000000ull }, { ~0ull, ~0ull }, { 0x10, 0x20 },
            { 0x8000000000000000ull, 1 }, { 0, ~0ull }, { ~0ull, 0 },
        };
        int f = 0;
        for (unsigned i = 0; i < N(w); i++) {
            unsigned __int128 x = (unsigned __int128)w[i].hi << 64 | w[i].lo;
            unsigned __int128 nx = ~x;
            ull hi = w[i].hi, lo = w[i].lo;
            int lz = hi ? rclz(hi, 64) : 64 + rclz(lo, 64);
            int tz = lo ? rctz(lo, 64) : 64 + rctz(hi, 64);
            int pc = rpop(hi, 64) + rpop(lo, 64);
            int nlz = ~hi ? rclz(~hi, 64) : 64 + rclz(~lo, 64);
            f |= __builtin_clzg(x, -1) != (x ? lz : -1);
            f |= __builtin_ctzg(x, -1) != (x ? tz : -1);
            f |= __builtin_popcountg(x) != pc || __builtin_parityg(x) != (pc & 1);
            f |= __builtin_ffsg((__int128)x) != (x ? tz + 1 : 0);
            f |= __builtin_clrsbg((__int128)x) != (hi >> 63 ? nlz : lz) - 1;
            f |= __builtin_stdc_leading_ones(x) != (unsigned)nlz;
            f |= __builtin_stdc_first_leading_zero(x) != (nx ? (unsigned)nlz + 1 : 0u);
            f |= __builtin_stdc_bit_width(x) != (unsigned)(128 - lz);
            f |= __builtin_stdc_bit_floor(x) != (x ? (unsigned __int128)1 << (127 - lz) : 0);
            {
                unsigned __int128 c = __builtin_stdc_bit_ceil(x), p = 1;
                while (p && p < x)
                    p <<= 1;
                f |= c != (x <= 1 ? 1 : p);
            }
            f |= __builtin_stdc_rotate_left(x, 64) != ((unsigned __int128)lo << 64 | hi);
            f |= __builtin_stdc_rotate_right(x, 4) != (x >> 4 | x << 124);
#ifdef __EMBCC__
            f |= __builtin_stdc_rotate_left(x, -4) != (x >> 4 | x << 124);
#endif
        }
        bad |= f << 5;
    }
#endif
    if (W(6)) {
        unsigned char c = 0x81;
        int f = 0;
        /* the result types: int, unsigned int, bool, the operand's */
        f |= !IS(__builtin_clzg(c), int) || !IS(__builtin_popcountg(c), int);
        f |= !IS(__builtin_stdc_leading_zeros(c), unsigned int);
        f |= !IS(__builtin_stdc_has_single_bit(c), _Bool);
        f |= !IS(__builtin_stdc_bit_floor(c), unsigned char);
        f |= !IS(__builtin_stdc_rotate_left(c, 1), unsigned char);
        f |= field_shift != 6 || sizeof sized != 3;
        f |= classify(4) != 1 || classify(8) != 2 || classify(5) != 0;
        bad |= f << 6;
    }
    return bad ? bad : 42;
}
