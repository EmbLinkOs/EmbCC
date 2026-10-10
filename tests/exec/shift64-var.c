/* 64-bit shifts by a variable count, every count from 0 to 63, left,
 * logical right and arithmetic right, over values whose words differ.
 * On a 32-bit target each is a sequence over a register pair: the arm
 * for counts of 32 and more, the arm below, and the count of zero, where
 * the bits crossing from one word to the other must be none -- RISC-V
 * takes a shift count's low five bits, so `x << (32 - n)` at n == 0 is
 * not zero. The checksum is the host's.
 */
// expect-exit: 42
typedef unsigned long long u64;
typedef long long s64;

static volatile int vone = 1;

__attribute__((noinline)) static u64 shl(u64 x, int n) { return x << n; }
__attribute__((noinline)) static u64 shr(u64 x, int n) { return x >> n; }
__attribute__((noinline)) static s64 sar(s64 x, int n) { return x >> n; }

/* the count and the value in the same pair's neighbourhood, inlined */
static u64 mix(u64 x, u64 y, int n)
{
    return (x << n) ^ (y >> (63 - n)) ^ (u64)((s64)y >> n);
}

int main(void)
{
    static const u64 v[] = {
        0x0123456789abcdefull, 0xfedcba9876543210ull, 0x8000000000000001ull,
        0x00000000ffffffffull, 0xffffffff00000000ull, 1ull, ~0ull,
        0x7fffffffffffffffull,
    };
    u64 h = 0xcbf29ce484222325ull;
    for (unsigned k = 0; k < sizeof v / sizeof v[0]; k++)
        for (int n = 0; n < 64; n++) {
            int c = n * vone;
            h = (h ^ shl(v[k], c)) * 0x100000001b3ull;
            h = (h ^ shr(v[k], c)) * 0x100000001b3ull;
            h = (h ^ (u64)sar((s64)v[k], c)) * 0x100000001b3ull;
            h = (h ^ mix(v[k], v[(k + 1) % 8], c)) * 0x100000001b3ull;
        }
    return h == 0xa86f415eed7d8065ull ? 42 : (int)(h & 0x3f) + 1;
}
