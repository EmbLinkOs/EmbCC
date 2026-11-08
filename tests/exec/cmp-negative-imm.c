/* Compares with a negative constant, and masks whose complement is the
 * small one. On ARMv7-M a compare with -60 is `cmn r, #60` (r + 60 sets
 * the flags of r - -60: N, Z, C and V alike), and `x & 0xffffff3f` is
 * `bic x, #0xc0`. Every predicate, signed and unsigned -- an unsigned
 * `x < 0xffffffc4u` reads the carry, which CMN makes from the addition --
 * at the constant and on either side of it, and at the values where the
 * flags turn over: 0, INT_MIN, INT_MAX, -1. INT_MIN itself is a constant
 * CMN cannot take (its negation is itself). Each result is checked
 * against the same comparison of a volatile copy of the constant, which
 * the compiler cannot fold into the instruction. */
// expect-exit: 42
#include <limits.h>

#define K(name, k)                                                        \
    __attribute__((noinline)) static int name(int x, unsigned u)          \
    {                                                                     \
        return (x < (k)) | (x <= (k)) << 1 | (x > (k)) << 2 |             \
               (x >= (k)) << 3 | (x == (k)) << 4 | (x != (k)) << 5 |      \
               (u < (unsigned)(k)) << 6 | (u <= (unsigned)(k)) << 7 |     \
               (u > (unsigned)(k)) << 8 | (u >= (unsigned)(k)) << 9;      \
    }
K(k60, -60)
K(k1020, -1020)
K(k256, -256)
K(k1, -1)
K(kmin, INT_MIN)
K(kbig, -0x10000)

static int ref(int x, unsigned u, int k)
{
    volatile int vk = k;
    int kk = vk;
    return (x < kk) | (x <= kk) << 1 | (x > kk) << 2 | (x >= kk) << 3 |
           (x == kk) << 4 | (x != kk) << 5 | (u < (unsigned)kk) << 6 |
           (u <= (unsigned)kk) << 7 | (u > (unsigned)kk) << 8 |
           (u >= (unsigned)kk) << 9;
}

__attribute__((noinline)) unsigned clr(unsigned x) { return x & 0xffffff3fu; }
__attribute__((noinline)) unsigned keep(unsigned x) { return x & ~0xff000000u; }
__attribute__((noinline)) int branch(int x)
{
    if (x > -60)
        return 1;
    if (x == -256)
        return 2;
    return 3;
}

int main(void)
{
    static const int ks[] = { -60, -1020, -256, -1, INT_MIN, -0x10000 };
    static int (*const fs[])(int, unsigned) = { k60, k1020, k256, k1, kmin, kbig };
    for (unsigned j = 0; j < sizeof ks / sizeof ks[0]; j++) {
        int k = ks[j];
        int xs[] = { k - 1, k, k + 1, 0, -1, 1, INT_MIN, INT_MAX, INT_MIN + 1 };
        for (unsigned n = 0; n < sizeof xs / sizeof xs[0]; n++) {
            int x = xs[n];
            if (fs[j](x, (unsigned)x) != ref(x, (unsigned)x, k))
                return (int)j + 1;
        }
    }
    if (clr(0xffffffffu) != 0xffffff3fu || clr(0xc0u) != 0)
        return 10;
    if (keep(0xabcdef12u) != 0x00cdef12u)
        return 11;
    if (branch(-59) != 1 || branch(-60) != 3 || branch(-256) != 2 ||
        branch(INT_MIN) != 3)
        return 12;
    return 42;
}
