// expect-exit: 42
/* _Float16, IEEE binary16 (C23 Annex X): every operation and conversion,
 * bit for bit against binary16 hardware (the expected bits are an Apple
 * M-series' FCVT and FADD/FMUL/FDIV on half). Each checked operation is
 * one assignment or cast, where a compiler that evaluates _Float16 in
 * float and rounds at assignments (GCC and clang on x86-64) and one that
 * rounds every operation (EmbCC, and any machine with half arithmetic)
 * must agree. Loads and stores go through memory, members, pointers and
 * volatiles; constants are folded once from their digits; static
 * initializers are rounded per operation. */

#include <float.h>
#include <string.h>

#define NOINLINE __attribute__((noinline))

typedef _Float16 h16;

static unsigned short bits(h16 h)
{
    unsigned short u;
    memcpy(&u, &h, 2);
    return u;
}

static h16 of(unsigned short u)
{
    h16 h;
    memcpy(&h, &u, 2);
    return h;
}

static int bad;
#define CHECK(n, got, want) do { if ((got) != (want)) bad |= 1 << (n); } while (0)
#define PAIR(a, b) ((unsigned long)bits(a) | (unsigned long)bits(b) << 16)

/* a, b, a+b, a-b, a*b, a/b (0 where b is a zero) */
static const unsigned short ops[][6] = {
    { 0x0000, 0x03ff, 0x03ff, 0x83ff, 0x0000, 0x0000 },
    { 0x8000, 0x4248, 0x4248, 0xc248, 0x8000, 0x8000 },
    { 0x0001, 0x0200, 0x0201, 0x81ff, 0x0000, 0x1800 },
    { 0x03ff, 0x0400, 0x07ff, 0x8001, 0x0000, 0x3bfe },
    { 0x0400, 0x5640, 0x5640, 0xd640, 0x1e40, 0x000a },
    { 0x3555, 0x8401, 0x3555, 0x3555, 0x8156, 0xed54 },
    { 0x3c00, 0x3555, 0x3d55, 0x3956, 0x3555, 0x4200 },
    { 0x3c01, 0x7bff, 0x7bff, 0xfbff, 0x7c00, 0x0100 },
    { 0x3bff, 0x7800, 0x7800, 0xf800, 0x77ff, 0x0200 },
    { 0x4000, 0x3c00, 0x4200, 0x3c00, 0x4000, 0x4000 },
    { 0x4248, 0xc400, 0xbae0, 0x4724, 0xca48, 0xba48 },
    { 0x5640, 0x0000, 0x5640, 0x5640, 0x0000, 0x0000 },
    { 0x7bff, 0x3c01, 0x7bff, 0x7bff, 0x7c00, 0x7bfd },
    { 0xc400, 0xb666, 0xc466, 0xc333, 0x3e66, 0x4900 },
    { 0xb666, 0x8000, 0xb666, 0xb666, 0x0000, 0x0000 },
    { 0x1234, 0x3bff, 0x3c00, 0xbbfd, 0x1233, 0x1235 },
    { 0x6a55, 0x1234, 0x6a55, 0x6a55, 0x40e9, 0x7c00 },
    { 0x0200, 0x0001, 0x0201, 0x01ff, 0x0000, 0x6000 },
    { 0x8401, 0x4000, 0x4000, 0xc000, 0x8801, 0x8200 },
    { 0x7800, 0x6a55, 0x7865, 0x7735, 0x7c00, 0x490e },
};

/* ties to even, the overflow at 65520, the subnormals and their tie at
 * 2^-25 */
static const struct { float f; unsigned short h; } fl[] = {
    { 0x1.002p+0f, 0x3c00 },     { 0x1.006p+0f, 0x3c02 },
    { 0x1.ffcp+15f, 0x7bff },    { 0x1.ffdffap+15f, 0x7bff },
    { 0x1.ffep+15f, 0x7c00 },    { 0x1p-24f, 0x0001 },
    { 0x1p-25f, 0x0000 },        { 0x1.000002p-25f, 0x0001 },
    { 0x1.99999ap-4f, 0x2e66 },  { -0x1.99999ap-4f, 0xae66 },
    { 0x1.921fb6p+1f, 0x4248 },  { 0x1.0c6f7ap-20f, 0x0011 },
    { -0x1.2a05f2p+33f, 0xfc00 }, { 0x1.ff8p-15f, 0x03ff },
};

/* from double in ONE rounding: the first two lie 2^-40 either side of a
 * half-way point, which a float in between would have rounded onto */
static const struct { double d; unsigned short h; } dl[] = {
    { 0x1.0020000001p+0, 0x3c01 },  { 0x1.001fffffffp+0, 0x3c00 },
    { 0x1.999999999999ap-4, 0x2e66 }, { 0x1.5555555555555p-1, 0x3955 },
    { 0x1.ffdfffffde721p+15, 0x7bff }, { 0x1.7e43c8800759cp+996, 0x7c00 },
    { 0x1.000000002p-25, 0x0001 },  { 0x1p-25, 0x0000 },
};

static const struct { long i; unsigned short h; } il[] = {
    { 0, 0x0000 }, { 1, 0x3c00 }, { -3, 0xc200 }, { 2049, 0x6800 },
    { 2051, 0x6802 }, { 4097, 0x6c00 }, { 65504, 0x7bff },
    { 65519, 0x7bff }, { 65520, 0x7c00 }, { -70000, 0xfc00 },
    { 1000000007, 0x7c00 },
};

/* folded: each from its digits, the sum rounded as an operation, and the
 * cast inside rounding 2049 to 2048 before the subtraction sees it. (The
 * sum's operands are cast: a compiler evaluating _Float16 in float may
 * take the constants themselves at float's precision, C17 5.2.4.2.2p9,
 * and GCC on x86-64 does -- its sum is 0x34cd.) */
static h16 s_tenth = 0.1f16, s_sum = (h16)0.1f16 + (h16)0.2f16;
static h16 s_tiny = 1e-7f16;
static h16 s_step = (h16)(2048.0f16 + 1.0f16) - 2048.0f16;
static h16 s_big = 70000.0, s_conv = 0.1;
static h16 s_arr[3] = { 1, 0.5f16, -65504 };

struct rec { char c; h16 h; h16 v[2]; };

NOINLINE static void add(h16 *r, const h16 *a, const h16 *b) { *r = *a + *b; }

int main(void)
{
    /* arithmetic, through memory */
    for (unsigned k = 0; k < sizeof ops / sizeof ops[0]; k++) {
        volatile h16 a = of(ops[k][0]), b = of(ops[k][1]);
        h16 s = a + b, d = a - b, p = a * b;
        CHECK(0, bits(s), ops[k][2]);
        CHECK(1, bits(d), ops[k][3]);
        CHECK(2, bits(p), ops[k][4]);
        if (b != 0) {
            h16 q = a / b;
            CHECK(3, bits(q), ops[k][5]);
        }
    }
    /* conversions in */
    for (unsigned k = 0; k < sizeof fl / sizeof fl[0]; k++) {
        volatile float f = fl[k].f;
        CHECK(4, bits((h16)f), fl[k].h);
    }
    for (unsigned k = 0; sizeof(double) == 8 && k < sizeof dl / sizeof dl[0];
         k++) {                         /* (AVR's double is binary32) */
        volatile double d = dl[k].d;
        CHECK(5, bits((h16)d), dl[k].h);
    }
    for (unsigned k = 0; k < sizeof il / sizeof il[0]; k++) {
        volatile long i = il[k].i;
        CHECK(6, bits((h16)i), il[k].h);
    }
    {
        volatile long double x = 1.00048828125L + 0x1p-60L;
        /* one rounding where long double holds the 2^-60 */
        CHECK(7, bits((h16)x), LDBL_MANT_DIG >= 61 ? 0x3c01 : 0x3c00);
        volatile unsigned long long u = 18446744073709551615ULL;
        CHECK(7, bits((h16)u), 0x7c00);
    }
    /* conversions out: exact to float and double, truncated to integers */
    {
        volatile h16 h = of(0x3e00), n = of(0xbe00), m = of(0x7bff), t = of(0x0001);
        CHECK(8, (float)h, 1.5f);
        CHECK(8, (double)t, 0x1p-24);
        CHECK(8, (int)h, 1);
        CHECK(8, (int)n, -1);
        CHECK(8, (long)m, 65504);
        CHECK(8, (unsigned)m, 65504u);
        CHECK(8, (_Bool)t, 1);
        CHECK(8, (_Bool)of(0x8000), 0);
        CHECK(8, (double)m * 2, 131008.0);
    }
    /* literals and static initializers */
    CHECK(9, bits(s_tenth), 0x2e66);
    CHECK(9, bits(s_sum), 0x34cc);
    CHECK(9, bits(s_tiny), 0x0002);
    CHECK(9, bits(s_step), 0x0000);
    CHECK(9, bits(s_big), 0x7c00);
    CHECK(9, bits(s_conv), 0x2e66);
    CHECK(9, PAIR(s_arr[0], s_arr[1]), 0x3c00 | 0x3800UL << 16);
    CHECK(9, bits(s_arr[2]), 0xfbff);
    CHECK(9, bits(6.1e-5f16), 0x03ff);
    {
        h16 l = 0.1;                    /* folded in the compiler */
        CHECK(9, bits(l), 0x2e66);
    }
    /* comparisons: as values, -0 equal to 0, a NaN unordered */
    {
        volatile h16 z = 0, nz = of(0x8000), one = 1, two = 2;
        volatile h16 nan = z / z, inf = one / z;
        CHECK(10, z == nz, 1);
        CHECK(10, one < two && two > one && one <= one && !(two < one), 1);
        CHECK(10, nan != nan && !(nan < one) && !(nan >= one), 1);
        CHECK(10, bits(inf), 0x7c00);
        CHECK(10, (h16)0.1f16 < 0.1 && (h16)0.1f16 == (h16)0.1f, 1);
        CHECK(10, (one ? two : z) == 2, 1);
        CHECK(10, !z && !!one, 1);
    }
    /* ++ and compound assignment round as they store */
    {
        h16 h = 2048;
        h++;
        CHECK(11, bits(h), 0x6800);     /* 2049 is a tie: 2048, even */
        h = 2050;
        ++h;
        CHECK(11, bits(h), 0x6802);     /* 2051, a tie: 2052 */
        h -= 0.5;                       /* 2051.5 in double: 2052 */
        CHECK(11, bits(h), 0x6802);
        h *= 3;
        CHECK(11, bits(h), 0x6e03);     /* 6156 */
        h = -h;
        CHECK(11, bits(h), 0xee03);
        volatile h16 v = 1.5f16;
        v += 1;
        CHECK(11, bits(v), 0x4100);
    }
    /* layout, members, arrays, pointers */
    {
        struct rec r = { 'x', 1.25f16, { 3, -0.0f16 } };
        h16 arr[4] = { 0 };
        h16 *p = &arr[1];
        struct srec { char c; short h; short v[2]; };
        CHECK(12, sizeof(h16) == 2 && _Alignof(h16) == _Alignof(short) &&
                  sizeof(struct rec) == sizeof(struct srec), 1);
        CHECK(12, bits(r.h), 0x3d00);
        CHECK(12, PAIR(r.v[0], r.v[1]), 0x4200 | 0x8000UL << 16);
        *p = 3.0f16;
        p[1] = *p * 2;
        add(&arr[3], &arr[1], &arr[2]);
        CHECK(12, PAIR(arr[0], arr[1]), 0x0000 | 0x4200UL << 16);
        CHECK(12, PAIR(arr[2], arr[3]), 0x4600 | 0x4880UL << 16);
        r.h += r.v[0];
        CHECK(12, bits(r.h), 0x4440);   /* 4.25 */
    }
    /* the type: _Float16 + _Float16 is _Float16, + float is float */
    {
        h16 a = 1;
        CHECK(13, sizeof(a + a) == 2 && sizeof(a + 1) == 2 &&
                  sizeof(a + 1.0f) == 4 && sizeof(a + 1.0) == sizeof(double), 1);
        CHECK(13, _Generic(a + a, _Float16: 1, default: 0), 1);
        /* as the compiler describes the type (float.h's FLT16_ names
         * are these, where it has them) */
        CHECK(13, __FLT16_MANT_DIG__ == 11 && __FLT16_MAX__ == 65504 &&
                  __FLT16_MIN_EXP__ == -13 && bits(__FLT16_MIN__) == 0x0400 &&
                  bits(__FLT16_DENORM_MIN__) == 1, 1);
    }
    return bad ? bad & 0x3fff ? 1 : 2 : 42;
}
