// expect-exit: 42
/* Casts to narrow types of values whose high bits may or may not already
 * be zero. The optimizer drops an extension only when every bit it would
 * change is known zero (known_zero, including a join of several
 * definitions); each case here is one it must keep or may drop, and the
 * answers are worked out by hand. */
#include <stdint.h>

#define NI __attribute__((noinline))

/* May drop: a logical shift leaves eight bits. */
NI static unsigned top8(uint32_t s) { return (uint8_t)(s >> 24); }
/* Must keep: nine bits survive the shift. */
NI static unsigned top9(uint32_t s) { return (uint8_t)(s >> 23); }
/* Must keep: an arithmetic shift copies the sign into the high bits. */
NI static unsigned stop8(int32_t s) { return (uint8_t)(s >> 24); }
/* Signed: seven bits known, so the sign bit is 0 -- may drop. */
NI static int s7(uint32_t s) { return (int8_t)(s >> 25); }
/* Signed: eight bits, the top one may be set -- must keep. */
NI static int s8(uint32_t s) { return (int8_t)(s >> 24); }
/* A mask: may drop for 0x7f, must keep the sign extension for 0xff. */
NI static int m7(int x) { return (int8_t)(x & 0x7f); }
NI static int m8(int x) { return (int8_t)(x & 0xff); }
/* A signed byte load sign-extends; zero-extending it must not be dropped. */
NI static unsigned ld(const signed char *p) { return (uint8_t)p[0]; }
/* A join of two signed byte loads: each arm may be negative -- must keep. */
NI static unsigned ldjoin(const signed char *p, const signed char *q, int c)
{
    int v;
    if (c) v = p[0];
    else v = q[1];
    return (uint8_t)v;
}
/* A join where every arm is narrow -- may drop. */
NI static unsigned join_ok(uint32_t s, int i)
{
    uint8_t b = (uint8_t)(s >> 24);
    if ((i & 31) == 0)
        b = 0x7e;
    return (uint8_t)b + 1u;
}
/* A join where one arm is not -- must keep. */
NI static unsigned join_bad(uint32_t s, int i, uint32_t wide)
{
    uint32_t b = s >> 24;
    if (i & 1)
        b = wide;
    return (uint8_t)b;
}
/* A value carried round a loop, narrow on every definition. */
NI static unsigned loop_ok(const uint8_t *p, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++)
        v = (v ^ p[i]) & 0xf0;
    return (uint8_t)v;
}
/* ...and one that is not narrow on the second trip. */
NI static unsigned loop_bad(int n)
{
    uint32_t v = 3;
    for (int i = 0; i < n; i++)
        v = v * 0x101;
    return (uint16_t)v;
}
/* 64 bits: the high half of a 64-bit value is known zero after a shift. */
NI static uint64_t hi32(uint64_t x) { return (uint32_t)(x >> 32); }
NI static uint64_t hi33(uint64_t x) { return (uint32_t)(x >> 31); }

int main(void)
{
    int bad = 0;
    if (top8(0xab123456u) != 0xab || top9(0xab923456u) != 0x57) bad |= 1;
    if (stop8(INT32_MIN) != 0x80 || stop8(-1) != 0xff) bad |= 2;
    if (s7(0xfe000000u) != 0x7f || s8(0xfe000000u) != -2) bad |= 4;
    if (m7(-1) != 0x7f || m8(0x80) != -128 || m8(0x7f) != 127) bad |= 8;
    signed char sc = -3;
    if (ld(&sc) != 253) bad |= 16;
    static const signed char sb[2] = { -3, -128 };
    if (ldjoin(sb, sb, 1) != 253 || ldjoin(sb, sb, 0) != 128) bad |= 16;
    if (join_ok(0x41000000u, 1) != 0x42 || join_ok(0x41000000u, 32) != 0x7f)
        bad |= 32;
    if (join_bad(0x41000000u, 0, 0x1234) != 0x41 ||
        join_bad(0x41000000u, 1, 0x1234) != 0x34) bad |= 64;
    static const uint8_t bytes[] = { 0x12, 0x34, 0x56, 0xff };
    /* v: 0x10, 0x20 (0x10^0x34 = 0x24 & 0xf0), 0x70, 0x80 */
    if (loop_ok(bytes, 4) != 0x80 || loop_ok(bytes, 0) != 0) bad |= 128;
    /* 3, 0x303, 0x30603, 0x3090903 */
    if (loop_bad(2) != 0x0603 || loop_bad(3) != 0x0903) bad |= 256;
    if (hi32(0xfedcba9876543210ull) != 0xfedcba98u ||
        hi33(0xfedcba9876543210ull) != 0xfdb97530u) bad |= 512;
    return bad ? 1 + bad % 41 : 42;      /* 1..41: never 42 */
}
