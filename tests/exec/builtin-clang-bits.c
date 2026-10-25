/* clang's bit builtins -- __builtin_bitreverse8/16/32/64 and
 * __builtin_rotateleft/rotateright of the same widths -- against the
 * plain C of what each means, over values that walk every bit and every
 * amount (a rotate's amount is taken modulo the width, so 0, the width
 * and past it included), at the width's own unsigned type; and GCC's
 * __builtin_LINE, __builtin_FILE, __builtin_FUNCTION and
 * __builtin_speculation_safe_value, and clang's __builtin_assume, which
 * evaluates nothing. */
// expect-exit: 42
// no-gcc-reference: clang's builtins; GCC has no bitreverse or rotate builtins
#include <stdint.h>
#include <string.h>

static uint64_t rev(uint64_t v, int bits)
{
    uint64_t r = 0;
    for (int i = 0; i < bits; i++)
        if (v >> i & 1)
            r |= (uint64_t)1 << (bits - 1 - i);
    return r;
}

static uint64_t rotl(uint64_t v, unsigned n, int bits)
{
    uint64_t m = bits == 64 ? ~(uint64_t)0 : ((uint64_t)1 << bits) - 1;
    n %= (unsigned)bits;
    v &= m;
    return n ? ((v << n) | (v >> (bits - n))) & m : v;
}

static volatile uint64_t seeds[] = {
    0, 1, 0x80, 0x8001, 0x12345678, 0xdeadbeefcafef00dull,
    0xffffffffffffffffull, 0x0123456789abcdefull, 0x5555aaaa3333ccccull,
};

static int side;
static int effect(void) { side++; return 1; }

/* outside a function, the function is "" */
static const char *top = __builtin_FUNCTION();

int main(void)
{
    int bad = 0;
    for (unsigned s = 0; s < sizeof seeds / sizeof seeds[0]; s++) {
        uint64_t v = seeds[s];
        bad |= __builtin_bitreverse8((uint8_t)v) != (uint8_t)rev((uint8_t)v, 8);
        bad |= __builtin_bitreverse16((uint16_t)v) != (uint16_t)rev((uint16_t)v, 16);
        bad |= __builtin_bitreverse32((uint32_t)v) != (uint32_t)rev((uint32_t)v, 32);
        bad |= __builtin_bitreverse64(v) != rev(v, 64);
        for (unsigned n = 0; n < 70; n++) {
            bad |= (__builtin_rotateleft8((uint8_t)v, (uint8_t)n) != (uint8_t)rotl(v, n, 8)) << 1;
            bad |= (__builtin_rotateright8((uint8_t)v, (uint8_t)n) != (uint8_t)rotl(v, 8 - n % 8, 8)) << 1;
            bad |= (__builtin_rotateleft16((uint16_t)v, (uint16_t)n) != (uint16_t)rotl(v, n, 16)) << 2;
            bad |= (__builtin_rotateright16((uint16_t)v, (uint16_t)n) != (uint16_t)rotl(v, 16 - n % 16, 16)) << 2;
            bad |= (__builtin_rotateleft32((uint32_t)v, n) != (uint32_t)rotl(v, n, 32)) << 3;
            bad |= (__builtin_rotateright32((uint32_t)v, n) != (uint32_t)rotl(v, 32 - n % 32, 32)) << 3;
            bad |= (__builtin_rotateleft64(v, n) != rotl(v, n, 64)) << 4;
            bad |= (__builtin_rotateright64(v, n) != rotl(v, 64 - n % 64, 64)) << 4;
        }
    }
    /* each result is the width's own type */
    bad |= (sizeof __builtin_bitreverse8(1) != 1 ||
            sizeof __builtin_rotateleft16(1, 1) != 2 ||
            sizeof __builtin_rotateright32(1, 1) != 4 ||
            sizeof __builtin_bitreverse64(1) != 8) << 5;
    /* where the call is */
    bad |= (__builtin_LINE() != __LINE__) << 6;
    bad |= (strcmp(__builtin_FUNCTION(), __func__) != 0 || top[0]) << 6;
    bad |= (strstr(__builtin_FILE(), "builtin-clang-bits.c") == 0) << 6;
    /* the value itself; and a promise that is not evaluated */
    bad |= (__builtin_speculation_safe_value(seeds[3]) != 0x8001) << 7;
    __builtin_assume(effect());
    bad |= (side != 0) << 7;
    return bad ? bad : 42;
}
