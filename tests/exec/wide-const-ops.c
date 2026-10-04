/* 64-bit AND, OR and XOR with constants whose halves are each a different
 * case -- all ones, zero, an encodable immediate, its complement, a low
 * mask, anything else -- shifts by every constant from 0 to 63, a shift
 * and mask taking a double's exponent field, and `x >> 63` as a branch.
 * On a 32-bit target each is lowered half by half; every answer here is
 * checked against a reference built one bit at a time in a loop the
 * compiler cannot turn into the same instructions. */
// expect-exit: 42
typedef unsigned long long u64;
typedef long long i64;
static volatile u64 vx[] = {
    0x0123456789abcdefULL, 0xfedcba9876543210ULL, 0x8000000000000001ULL,
    0x00000000ffffffffULL, 0xffffffff00000000ULL, 0x7ff8000000000000ULL, 0
};
static volatile int vz;

static u64 bitwise(u64 a, u64 b, int op)
{
    u64 r = 0;
    for (int i = 0; i < 64; i++) {
        u64 x = (a >> i) & 1, y = (b >> i) & 1;   /* by a variable: no fold */
        u64 z = op == 0 ? (x & y) : op == 1 ? (x | y) : (x ^ y);
        r |= z << i;
    }
    return r;
}
static u64 shl_ref(u64 a, int n) { while (n-- > 0) a *= 2; return a; }
static u64 shr_ref(u64 a, int n) { while (n-- > 0) a /= 2; return a; }
static i64 sar_ref(i64 a, int n)
{
    while (n-- > 0)
        a = a < 0 ? -((-(a + 1)) / 2) - 1 : a / 2;   /* floor(a / 2) */
    return a;
}

#define CHK_AND(k) if ((x & (k)) != bitwise(x, (k), 0)) return 1
#define CHK_OR(k)  if ((x | (k)) != bitwise(x, (k), 1)) return 2
#define CHK_XOR(k) if ((x ^ (k)) != bitwise(x, (k), 2)) return 3

__attribute__((noinline)) static int logic(u64 x)
{
    CHK_AND(0xffffffff00000000ULL); CHK_AND(0x00000000ffffffffULL);
    CHK_AND(0x000fffffffffffffULL); CHK_AND(0x00000000000000ffULL);
    CHK_AND(0xfffffff0ffffff0fULL); CHK_AND(0x12345678fffff000ULL);
    CHK_AND(1ULL);                  CHK_AND(0xff00ff00000000ffULL);
    CHK_OR(0x8000000000000000ULL);  CHK_OR(0x00000000ffffffffULL);
    CHK_OR(0xffffff00fffffff0ULL);  CHK_OR(0x0000000100000001ULL);
    CHK_OR(0x00ff00ff12345678ULL);
    CHK_XOR(0xffffffffffffffffULL); CHK_XOR(0xffffffff00000000ULL);
    CHK_XOR(0x00000000000000ffULL); CHK_XOR(0x8765432112345678ULL);
    return 0;
}

#define SH(n) if ((x << n) != shl_ref(x, n) || (x >> n) != shr_ref(x, n) || \
                  ((i64)x >> n) != sar_ref((i64)x, n)) return 100 + n
__attribute__((noinline)) static int shifts(u64 x)
{
    SH(0);  SH(1);  SH(2);  SH(7);  SH(8);  SH(13); SH(20); SH(31);
    SH(32); SH(33); SH(40); SH(47); SH(52); SH(55); SH(62); SH(63);
    return 0;
}

/* `if (x & K)` with K below 2^32 tests the low word only; with K at or
 * above 2^32 it must not. And `x & K == 0` likewise. */
__attribute__((noinline)) static int tests(u64 x)
{
    int r = 0;
    if (x & 1ULL) r |= 1;
    if (x & 0x80000000ULL) r |= 2;
    if (x & 0x100000000ULL) r |= 4;
    if (x & 0xffffffff00000000ULL) r |= 8;
    if ((x & 0x00ff0000ULL) == 0) r |= 16;
    if ((x & 0x8000000000000000ULL) != 0) r |= 32;
    if (((x >> 52) & 1) == 0) r |= 64;
    if (x & 0xfffff800ULL) r |= 128;
    if ((unsigned)x & ~0x7ffu) r |= 256;      /* andi -2048 at RV64 */
    return r;
}
static int tests_ref(u64 x)
{
    int r = 0;
    if (shr_ref(x, 0) % 2) r |= 1;
    if (shr_ref(x, 31) % 2) r |= 2;
    if (shr_ref(x, 32) % 2) r |= 4;
    if (shr_ref(x, 32)) r |= 8;
    if (shr_ref(x, 16) % 256 == 0) r |= 16;
    if (shr_ref(x, 63)) r |= 32;
    if (shr_ref(x, 52) % 2 == 0) r |= 64;
    if (shr_ref(x, 11) % 2097152) r |= 128;
    if (shr_ref(x, 11) % 2097152) r |= 256;
    return r;
}

__attribute__((noinline)) static int exponent(u64 x) { return (int)(x >> 52) & 0x7ff; }
__attribute__((noinline)) static int sign(u64 x)
{
    if (x >> 63)
        return 1;
    return 0;
}

int main(void)
{
    for (unsigned k = 0; k < sizeof vx / sizeof vx[0]; k++) {
        u64 x = vx[k];
        int r = logic(x);
        if (r) return r;
        r = shifts(x);
        if (r) return r;
        if (exponent(x) != (int)(shr_ref(x, 52) & 0x7ff)) return 4;
        if (sign(x) != (int)shr_ref(x, 63)) return 5;
        if (tests(x) != tests_ref(x)) return 7;
    }
    if (exponent(0x3ff0000000000000ULL + vz) != 1023) return 6;
    return 42;
}
