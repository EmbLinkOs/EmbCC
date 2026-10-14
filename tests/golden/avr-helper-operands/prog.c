/* AVR's 32-bit multiply, divide and remainder are calls to libgcc's
 * helpers (__mulsi3, __udivmodsi4, ...), which take their operands in
 * r22-r25 and r18-r21. Each operand is loaded there from its home, and a
 * home can be the OTHER operand's argument registers: the low word of a
 * 64-bit argument still in r18-r25, or two words passed in each other's
 * places. Loading them in a fixed order overwrote one with the other --
 * `(uint32_t)x * m` multiplied m by itself. Every function below is
 * such a pair; the host runs the same program and
 * tests/golden/avr-helper-operands.sh compares the board with it. */
#include <stdint.h>
void puts_(const char *s);
void putn(long v);

static void out(uint32_t v) { putn((long)(int32_t)v); }

volatile uint64_t d64[3] = { 0xFEDCBA9876543210ull, 0x8000000000000001ull, 10 };
volatile uint32_t w32[4] = { 0xFFFFFFFFu, 0x89ABCDEFu, 123456789u, 3 };

/* a: the low word of x, in r18-r21 -- b's argument registers */
__attribute__((noinline)) uint32_t mul_lo(uint64_t x, uint32_t m)
{
    return (uint32_t)x * m + 5u;
}
__attribute__((noinline)) uint32_t div_lo(uint64_t x, uint32_t m)
{
    return (uint32_t)x / m;
}
__attribute__((noinline)) uint32_t mod_lo(uint64_t x, uint32_t m)
{
    return (uint32_t)x % m;
}
/* the high word, in r22-r25 -- a's own argument registers */
__attribute__((noinline)) uint32_t mul_hi(uint64_t x, uint32_t m)
{
    return (uint32_t)(x >> 32) * m;
}
/* operands swapped: b is the low word, a the other argument */
__attribute__((noinline)) uint32_t mul_sw(uint64_t x, uint32_t m)
{
    return m * (uint32_t)x;
}
/* p in r22-r25, q in r18-r21, and the helper wants q first: each in
 * the other's place */
__attribute__((noinline)) uint32_t mul_xy(uint32_t p, uint32_t q)
{
    return q * p;
}
__attribute__((noinline)) uint32_t div_xy(uint32_t p, uint32_t q)
{
    return q / p;
}
__attribute__((noinline)) uint32_t mod_xy(uint32_t p, uint32_t q)
{
    return q % p;
}

int main(void)
{
    out(mul_lo(d64[0], w32[1])); out(mul_lo(d64[2], w32[2]));
    out(div_lo(d64[0], w32[3])); out(div_lo(d64[1], w32[2]));
    out(mod_lo(d64[0], w32[2])); out(mod_lo(d64[2], w32[3]));
    puts_("\n");
    out(mul_hi(d64[0], w32[1])); out(mul_sw(d64[0], w32[1])); out(mul_sw(d64[1], w32[0]));
    puts_("\n");
    out(mul_xy(w32[1], w32[2])); out(div_xy(w32[3], w32[1])); out(mod_xy(w32[2], w32[0]));
    out(div_xy(w32[2], w32[3]));
    puts_("\n");
    puts_("==END==\n");
    return 0;
}
