// expect-exit: 42
/* __bf16, bfloat16: the top half of a float -- its 8-bit exponent, an
 * 8-bit significand -- in two bytes. Every operation and conversion bit
 * for bit against clang's bfloat16 on an Apple M-series, each checked one
 * as a single assignment or cast (where evaluating in float and rounding
 * at the assignment agrees with rounding each operation). The conversions
 * from double and from integers round ONCE: 2^24 + 2^16 + 1 through a
 * float would land on a half-way point and go down. */

#include <string.h>

typedef __bf16 b16;

static unsigned short bits(b16 h)
{
    unsigned short u;
    memcpy(&u, &h, 2);
    return u;
}

static b16 of(unsigned short u)
{
    b16 h;
    memcpy(&h, &u, 2);
    return h;
}

static int bad;
#define CHECK(n, got, want) do { if ((got) != (want)) bad |= 1 << (n); } while (0)

/* a, b, a+b, a-b, a*b, a/b (0 where b is a zero) */
static const unsigned short ops[][6] = {
    { 0x0000, 0x007f, 0x007f, 0x807f, 0x0000, 0x0000 },
    { 0x8000, 0x4049, 0x4049, 0xc049, 0x8000, 0x8000 },
    { 0x0001, 0x8081, 0x8080, 0x0082, 0x8000, 0xbbfe },
    { 0x007f, 0x0080, 0x00ff, 0x8001, 0x0000, 0x3f7e },
    { 0x0080, 0x7f7f, 0x7f7f, 0xff7f, 0x407f, 0x0000 },
    { 0x3eab, 0x7f00, 0x7f00, 0xff00, 0x7e2b, 0x0015 },
    { 0x3f80, 0x3eab, 0x3fab, 0x3f2a, 0x3eab, 0x4040 },
    { 0x3f81, 0xc080, 0xc040, 0x40a0, 0xc081, 0xbe81 },
    { 0x3f7f, 0x4b80, 0x4b80, 0xcb80, 0x4b7f, 0x337f },
    { 0x4000, 0x3f80, 0x4040, 0x3f80, 0x4000, 0x4000 },
    { 0x4049, 0xbecd, 0x402f, 0x4063, 0xbfa1, 0xc0fb },
    { 0x7f7f, 0x0000, 0x7f7f, 0x7f7f, 0x0000, 0x0000 },
    { 0xc080, 0x3f81, 0xc040, 0xc0a0, 0xc081, 0xc07e },
    { 0xbecd, 0x1234, 0xbecd, 0xbecd, 0x9190, 0xec12 },
    { 0x1234, 0x8000, 0x1234, 0x1234, 0x8000, 0x0000 },
    { 0x6a55, 0x3f7f, 0x6a55, 0x6a55, 0x6a54, 0x6a56 },
    { 0x00ff, 0x6a55, 0x6a55, 0xea55, 0x2bd4, 0x0000 },
    { 0x8081, 0x0001, 0x8080, 0x8082, 0x8000, 0xc301 },
    { 0x7f00, 0x4000, 0x7f00, 0x7f00, 0x7f80, 0x7e80 },
    { 0x4b80, 0x00ff, 0x4b80, 0x4b80, 0x0cff, 0x7f80 },
};

static const struct { float f; unsigned short h; } fl[] = {
    { 0x1.01p+0f, 0x3f80 },      { 0x1.03p+0f, 0x3f82 },
    { 0x1.fep+127f, 0x7f7f },    { 0x1.ff933cp+127f, 0x7f80 },
    { 0x1p-133f, 0x0001 },       { 0x1p-134f, 0x0000 },
    { 0x1.99999ap-4f, 0x3dcd },  { -0x1.99999ap-4f, 0xbdcd },
    { 0x1.921fb6p+1f, 0x4049 },  { 0x1.16c2p-133f, 0x0001 },
    { -0x1.93e594p+99f, 0xf14a }, { 0x1.ffcp+15f, 0x4780 },
};

/* the first two lie 2^-40 either side of a half-way point */
static const struct { double d; unsigned short h; } dl[] = {
    { 0x1.0100000001p+0, 0x3f81 },  { 0x1.00ffffffffp+0, 0x3f80 },
    { 0x1.999999999999ap-4, 0x3dcd }, { 0x1.5555555555555p-1, 0x3f2b },
    { 0x1.7e43c8800759cp+996, 0x7f80 }, { 0x1.0000004p-134, 0x0001 },
    { 0x1.c363cbf21f28ap+127, 0x7f62 },
};

static const struct { long long i; unsigned short h; } il[] = {
    { 0LL, 0x0000 }, { 1LL, 0x3f80 }, { -3LL, 0xc040 }, { 257LL, 0x4380 },
    { 259LL, 0x4382 },
    { 16842753LL, 0x4b81 },                     /* 2^24 + 2^16 + 1 */
    { -2147483647LL - 1, 0xcf00 }, { 4294967295LL, 0x4f80 },
    { 9042383626829825LL, 0x5a01 }, { -9223372036854775807LL - 1, 0xdf00 },
    { 123456789012345LL, 0x56e1 },
};

static b16 s_third = (b16)1.0 / (b16)3.0, s_tenth = 0.1;
static b16 s_arr[3] = { 1, 0.5f, -3 };

struct rec { char c; b16 h; b16 v[2]; };

__attribute__((noinline)) static b16 add(b16 a, b16 b) { return a + b; }

int main(void)
{
    for (unsigned k = 0; k < sizeof ops / sizeof ops[0]; k++) {
        volatile b16 a = of(ops[k][0]), b = of(ops[k][1]);
        b16 s = a + b, d = a - b, p = a * b;
        CHECK(0, bits(s), ops[k][2]);
        CHECK(1, bits(d), ops[k][3]);
        CHECK(2, bits(p), ops[k][4]);
        if (b != 0) {
            b16 q = a / b;
            CHECK(3, bits(q), ops[k][5]);
        }
    }
    for (unsigned k = 0; k < sizeof fl / sizeof fl[0]; k++) {
        volatile float f = fl[k].f;
        CHECK(4, bits((b16)f), fl[k].h);
    }
    for (unsigned k = 0; sizeof(double) == 8 && k < sizeof dl / sizeof dl[0];
         k++) {
        volatile double d = dl[k].d;
        CHECK(5, bits((b16)d), dl[k].h);
    }
    for (unsigned k = 0; k < sizeof il / sizeof il[0]; k++) {
        volatile long long i = il[k].i;
        CHECK(6, bits((b16)i), il[k].h);
        if (i >= -2147483647 - 1 && i <= 2147483647) {
            volatile long l = (long)i;          /* 32 bits on most */
            CHECK(6, bits((b16)l), il[k].h);
        }
    }
    {
        volatile unsigned long long u = 18446744073709551615ULL;
        CHECK(7, bits((b16)u), 0x5f80);
        volatile _Float16 h = (_Float16)0.1;
        CHECK(7, bits((b16)h), 0x3dcd);
        volatile b16 t = of(0x3eab);            /* 1/3 */
        _Float16 th = t;
        unsigned short thb;
        memcpy(&thb, &th, 2);
        CHECK(7, thb, 0x3558);
    }
    {
        volatile b16 h = of(0x3fc0), n = of(0xbfc0), m = of(0x7f7f);
        CHECK(8, (float)h, 1.5f);
        CHECK(8, (int)h, 1);
        CHECK(8, (int)n, -1);
        CHECK(8, (double)m, 0x1.fep127);
        CHECK(8, (_Bool)of(0x0001), 1);
        CHECK(8, (_Bool)of(0x8000), 0);
    }
    CHECK(9, bits(s_third), 0x3eab);
    CHECK(9, bits(s_tenth), 0x3dcd);
    CHECK(9, bits(s_arr[0]) | (unsigned long)bits(s_arr[1]) << 16,
          0x3f80 | 0x3f00UL << 16);
    CHECK(9, bits(s_arr[2]), 0xc040);
#if defined(__EMBCC__) || (defined(__GNUC__) && !defined(__clang__))
    CHECK(9, bits(0.1bf16), 0x3dcd);          /* GCC's suffix */
#endif
    {
        volatile b16 z = 0, nz = of(0x8000), one = 1, two = 2;
        volatile b16 nan = z / z;
        CHECK(10, z == nz, 1);
        CHECK(10, one < two && two > one && !(two < one), 1);
        CHECK(10, nan != nan && !(nan < one), 1);
        CHECK(10, (one ? two : z) == 2, 1);
    }
    {
        b16 h = 256;
        h++;
        CHECK(11, bits(h), 0x4380);             /* 257 is a tie: 256 */
        h = 258;
        ++h;
        CHECK(11, bits(h), 0x4382);             /* 259, a tie: 260 */
        h *= 3;
        CHECK(11, bits(h), 0x4443);             /* 780, exactly */
        h = -h;
        CHECK(11, bits(h), 0xc443);
    }
    {
        struct rec r = { 'x', 1.25f, { 3, -0.0f } };
        struct { char c; short h; short v[2]; } sr;
        b16 arr[4] = { 0 };
        b16 *p = &arr[1];
        CHECK(12, sizeof(b16) == 2 && _Alignof(b16) == _Alignof(short) &&
                  sizeof r == sizeof sr, 1);
        CHECK(12, bits(r.h), 0x3fa0);
        *p = 3.0f;
        p[1] = *p * 2;
        arr[3] = add(arr[1], arr[2]);
        CHECK(12, bits(arr[1]) | (unsigned long)bits(arr[2]) << 16,
              0x4040 | 0x40c0UL << 16);
        CHECK(12, bits(arr[3]), 0x4110);
    }
    {
        b16 a = 1;
        _Float16 hf = 1;
        CHECK(13, sizeof(a + a) == 2 && sizeof(a + 1) == 2 &&
                  sizeof(a + 1.0f) == 4, 1);
        CHECK(13, _Generic(a + a, __bf16: 1, default: 0), 1);
        CHECK(13, _Generic(a + hf, _Float16: 1, default: 0), 1);
#ifdef __BFLT16_MANT_DIG__                     /* GCC's and EmbCC's */
        CHECK(13, __BFLT16_MANT_DIG__ == 8 && __BFLT16_MAX_EXP__ == 128, 1);
#endif
    }
    return bad ? bad & 0x3fff ? 1 : 2 : 42;
}
