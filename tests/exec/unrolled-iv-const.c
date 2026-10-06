// expect-exit: 42
/* Fully unrolled loops whose induction variable the optimizer must read
 * as the constant each copy has -- shifts, masks, products and sums of
 * it -- including the FNV-1a byte loop of tools/bench's hash kernel.
 * Each answer is compared with the same loop run through a volatile
 * bound, which nothing can unroll. */
#include <stdint.h>

#define NI __attribute__((noinline))

static volatile int four = 4, eight = 8;

NI static uint32_t fnv(uint32_t k)
{
    uint32_t h = 2166136261u;
    for (int b = 0; b < 4; b++)
        h = (h ^ ((k >> (b * 8)) & 255)) * 16777619u;
    return h;
}
NI static uint32_t fnv_v(uint32_t k)
{
    uint32_t h = 2166136261u;
    for (int b = 0; b < four; b++)
        h = (h ^ ((k >> (b * 8)) & 255)) * 16777619u;
    return h;
}

/* Every operation on the counter: + - * & | ^ << >> and a compare. */
NI static uint32_t mix(uint32_t x)
{
    uint32_t s = 0;
    for (int j = 0; j < 8; j++) {
        s += (x << j) ^ (uint32_t)(j * 3 - 5);
        s ^= (x >> (j | 1)) & (uint32_t)(0xf0 >> j);
        s += j > 3 ? (uint32_t)j : 7u;
    }
    return s;
}
NI static uint32_t mix_v(uint32_t x)
{
    uint32_t s = 0;
    for (int j = 0; j < eight; j++) {
        s += (x << j) ^ (uint32_t)(j * 3 - 5);
        s ^= (x >> (j | 1)) & (uint32_t)(0xf0 >> j);
        s += j > 3 ? (uint32_t)j : 7u;
    }
    return s;
}

/* A value known on entry and then redefined from x in the same block,
 * copy after copy: what was known must be forgotten at each write. */
NI static int redefine(int x)
{
    int v = 0;
    for (int k = 0; k < 3; k++)
        v = v * 3 + x + k;
    return v * 4;
}

/* A 64-bit counter. */
NI static uint64_t wide(uint64_t x)
{
    uint64_t s = 1;
    for (uint64_t j = 0; j < 4; j++)
        s = s * 31 + (x >> (j * 16) & 0xffff) + (j << 40);
    return s;
}
NI static uint64_t wide_v(uint64_t x)
{
    uint64_t s = 1;
    for (uint64_t j = 0; j < (uint64_t)four; j++)
        s = s * 31 + (x >> (j * 16) & 0xffff) + (j << 40);
    return s;
}

int main(void)
{
    static const uint32_t ks[] = { 0, 1, 0xdeadbeefu, 0x80000000u, 0x01020304u };
    int bad = 0;
    for (unsigned i = 0; i < sizeof ks / sizeof ks[0]; i++) {
        if (fnv(ks[i]) != fnv_v(ks[i])) bad |= 1;
        if (mix(ks[i]) != mix_v(ks[i])) bad |= 2;
        if (wide(ks[i] * 0x100000001ull) != wide_v(ks[i] * 0x100000001ull))
            bad |= 4;
    }
    /* v: x, 4x + 1, 13x + 5 */
    if (redefine(1) != 18 * 4 || redefine(-2) != -21 * 4) bad |= 16;
    /* and one answer by hand: FNV-1a of the bytes 00 00 00 00 */
    if (fnv(0) != 0x4b95f515u) bad |= 8;
    return bad ? 1 + bad : 42;
}
