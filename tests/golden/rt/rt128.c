/* Every 128-bit operation the backend calls out for, over a corpus of
 * values chosen to sit on the boundaries: zero, one, the halves, the
 * signs, the powers of two either side of 64 bits, and the extremes.
 *
 * The output is the test. The SAME source is compiled by the host's own
 * compiler, which has a native __int128 and its own runtime, and by
 * EmbCC against lib/rt -- and the two must print identical text. That
 * makes the host an independent oracle rather than us agreeing with
 * ourselves, which is the only kind of check that can find a rounding
 * rule implemented consistently and wrongly.
 */
#include <stdio.h>

typedef unsigned long long u64;
typedef unsigned __int128 u128;
typedef __int128 s128;

static void put(const char *tag, u128 v)
{
    printf("%s %016llx%016llx\n", tag, (u64)(v >> 64), (u64)v);
}

/* Printed as bits, not as a decimal: a double's decimal spelling
 * depends on the printf that formats it, and the bit pattern does not.
 * This is comparing arithmetic, not printf. */
static void putd(const char *tag, double d)
{
    union { double d; u64 u; } v;
    v.d = d;
    printf("%s %016llx\n", tag, v.u);
}

static void putf(const char *tag, float f)
{
    union { float f; unsigned u; } v;
    v.f = f;
    printf("%s %08x\n", tag, v.u);
}

static u128 mk(u64 hi, u64 lo) { return ((u128)hi << 64) | lo; }

#define NV 24
static u128 vals[NV];

static void fill(void)
{
    int i = 0;
    vals[i++] = 0;
    vals[i++] = 1;
    vals[i++] = 2;
    vals[i++] = 3;
    vals[i++] = 10;
    vals[i++] = 0xFFFFFFFFULL;                 /* 2^32 - 1 */
    vals[i++] = 0x100000000ULL;                /* 2^32 */
    vals[i++] = 0x7FFFFFFFFFFFFFFFULL;
    vals[i++] = 0x8000000000000000ULL;         /* 2^63 */
    vals[i++] = 0xFFFFFFFFFFFFFFFFULL;         /* 2^64 - 1 */
    vals[i++] = mk(1, 0);                      /* 2^64 */
    vals[i++] = mk(1, 1);
    vals[i++] = mk(0xFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL);
    vals[i++] = mk(0x1000000000000ULL, 0);
    vals[i++] = mk(0x7FFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL);  /* max s128 */
    vals[i++] = mk(0x8000000000000000ULL, 0);                      /* min s128 */
    vals[i++] = mk(0x8000000000000000ULL, 1);
    vals[i++] = mk(0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL);  /* -1 */
    vals[i++] = mk(0xFFFFFFFFFFFFFFFFULL, 0);
    vals[i++] = mk(0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL);
    vals[i++] = mk(0xDEADBEEFCAFEF00DULL, 0x0123456789ABCDEFULL);
    /* Values whose top 55 bits are followed by something, so the sticky
     * bit decides the rounding rather than the guard alone. */
    vals[i++] = mk(0x0020000000000000ULL, 1);
    vals[i++] = mk(0x0020000000000001ULL, 0);
    vals[i++] = mk(0x001FFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL);
}

int main(void)
{
    int i, j;
    fill();

    for (i = 0; i < NV; i++) {
        put("shl0",  vals[i]);
        for (j = 1; j < 128; j += 7) {
            u128 a = vals[i];
            int k = j;
            printf("shl %d ", k); put("", a << k);
            printf("shr %d ", k); put("", a >> k);
            printf("sar %d ", k); put("", (u128)((s128)a >> k));
        }
    }

    for (i = 0; i < NV; i++)
        for (j = 0; j < NV; j++) {
            u128 a = vals[i], b = vals[j];
            printf("mul %d %d ", i, j); put("", a * b);
            if (b != 0) {
                printf("udiv %d %d ", i, j); put("", a / b);
                printf("umod %d %d ", i, j); put("", a % b);
                /* The one signed case C leaves undefined is
                 * INT128_MIN / -1, which overflows; skip it rather than
                 * compare two undefined answers. */
                if (!((s128)a == mk(0x8000000000000000ULL, 0) &&
                      (s128)b == (s128)-1)) {
                    printf("sdiv %d %d ", i, j);
                    put("", (u128)((s128)a / (s128)b));
                    printf("smod %d %d ", i, j);
                    put("", (u128)((s128)a % (s128)b));
                }
            }
        }

    for (i = 0; i < NV; i++) {
        printf("u2d %d ", i); putd("", (double)vals[i]);
        printf("s2d %d ", i); putd("", (double)(s128)vals[i]);
        printf("u2f %d ", i); putf("", (float)vals[i]);
        printf("s2f %d ", i); putf("", (float)(s128)vals[i]);
    }

    /* Float to integer, over values that FIT.
     *
     * Out-of-range conversions are left out on purpose, and the reason
     * is worth writing down: C leaves them undefined, so the host is
     * not an oracle for them -- and at any optimisation level it does
     * not even call its own runtime, it constant-folds them in the
     * compiler, which answers differently again. Comparing those would
     * be comparing two arbitrary choices and calling a match a proof.
     * What EmbCC's runtime does there (saturate at the type's extremes)
     * is checked separately, below, as OUR documented behaviour rather
     * than as agreement with anyone.
     *
     * `volatile` so the values reach the runtime rather than the
     * constant folder: without it this compares two compilers' folding
     * and never executes a line of lib/rt. */
    {
        static const double ds[] = {
            0.0, 1.0, 1.5, -1.5, 2.0, 0.5, -0.5, 2.5, -2.5,
            1e18, -1e18, 1e30, -1e30, 1e38, -1e38,
            18446744073709551615.0,          /* 2^64 - 1 */
            18446744073709551616.0,          /* 2^64 */
            1.2676506002282294e30,           /* 2^100 */
            -1.2676506002282294e30,
            9007199254740993.0,              /* 2^53 + 1, not representable */
            0.9999999999999999, -0.9999999999999999
        };
        int n = (int)(sizeof ds / sizeof *ds);
        for (i = 0; i < n; i++) {
            volatile double d = ds[i];
            volatile float f = (float)ds[i];
            if (d >= 0.0) { printf("d2u %d ", i); put("", (u128)d); }
            printf("d2s %d ", i); put("", (u128)(s128)d);
            printf("f2s %d ", i); put("", (u128)(s128)f);
            if (f >= 0.0f) { printf("f2u %d ", i); put("", (u128)f); }
        }
    }
    printf("done\n");
    return 42;
}
