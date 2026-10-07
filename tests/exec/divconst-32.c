// expect-exit: 42
/* 32-bit division and remainder by a constant, as a 32-bit machine with a
 * widening multiply does them: the high word of x * M (mulh, umull,
 * smmul, multu...) and the corrections around it -- the add-back for an
 * unsigned divisor whose magic needs 33 bits, the add or subtract of x
 * for a signed one whose magic has the wrong sign, the post-shift, and
 * the +1 of a negative quotient. Each is checked against the same
 * division by a volatile divisor, which no compiler can see through.
 *
 * Quotient alone, remainder alone, and both together (which the
 * optimizer shares): three functions per divisor. Divisors include the
 * ones whose quotient is 0 or 1 (0x80000000 and up, unsigned), INT_MIN,
 * and -1 (not with INT_MIN, which is undefined). */
#include <stdint.h>

typedef int32_t i32;
typedef uint32_t u32;

static volatile i32 vs;
static volatile u32 vu;
static int bad;

#define SDIV(N, D)                                                        \
    static void sq_##N(i32 x) { vs = (D); if (x / (D) != x / vs) bad++; } \
    static void sr_##N(i32 x) { vs = (D); if (x % (D) != x % vs) bad++; } \
    static void sb_##N(i32 x)                                             \
    {                                                                     \
        i32 q = x / (D), r = x % (D);                                     \
        vs = (D);                                                         \
        if (q != x / vs || r != x % vs || q * (D) + r != x)               \
            bad++;                                                        \
    }                                                                     \
    static void s_##N(i32 x) { sq_##N(x); sr_##N(x); sb_##N(x); }
#define UDIV(N, D)                                                        \
    static void uq_##N(u32 x) { vu = (D); if (x / (D) != x / vu) bad++; } \
    static void ur_##N(u32 x) { vu = (D); if (x % (D) != x % vu) bad++; } \
    static void ub_##N(u32 x)                                             \
    {                                                                     \
        u32 q = x / (D), r = x % (D);                                     \
        vu = (D);                                                         \
        if (q != x / vu || r != x % vu || q * (D) + r != x)               \
            bad++;                                                        \
    }                                                                     \
    static void u_##N(u32 x) { uq_##N(x); ur_##N(x); ub_##N(x); }

SDIV(1, 1) SDIV(2, 2) SDIV(3, 3) SDIV(5, 5) SDIV(6, 6) SDIV(7, 7)
SDIV(10, 10) SDIV(25, 25) SDIV(125, 125) SDIV(255, 255) SDIV(641, 641)
SDIV(1000, 1000) SDIV(60000, 60000) SDIV(max, 0x7fffffff)
SDIV(min, INT32_MIN)
SDIV(n2, -2) SDIV(n3, -3) SDIV(n5, -5) SDIV(n6, -6) SDIV(n7, -7)
SDIV(n10, -10) SDIV(n255, -255) SDIV(n641, -641) SDIV(n1000, -1000)
SDIV(nmax, -0x7fffffff)

static void s_n1(i32 x)
{
    if (x == INT32_MIN)
        return;
    vs = -1;
    if (x / -1 != x / vs || x % -1 != x % vs)
        bad++;
}

UDIV(1, 1u) UDIV(2, 2u) UDIV(3, 3u) UDIV(5, 5u) UDIV(6, 6u) UDIV(7, 7u)
UDIV(10, 10u) UDIV(12, 12u) UDIV(14, 14u) UDIV(19, 19u) UDIV(25, 25u)
UDIV(28, 28u) UDIV(255, 255u) UDIV(641, 641u) UDIV(1000, 1000u)
UDIV(65537, 65537u) UDIV(1e9, 1000000007u) UDIV(max, 0x7fffffffu)
UDIV(h, 0x80000000u) UDIV(h1, 0x80000001u) UDIV(fe, 0xfffffffeu)
UDIV(ff, 0xffffffffu) UDIV(big, 0xc0000000u) UDIV(b3, 3000000000u)

static void one(u32 r)
{
    i32 x = (i32)r;
    s_1(x); s_2(x); s_3(x); s_5(x); s_6(x); s_7(x); s_10(x); s_25(x);
    s_125(x); s_255(x); s_641(x); s_1000(x); s_60000(x); s_max(x);
    s_min(x); s_n1(x);
    s_n2(x); s_n3(x); s_n5(x); s_n6(x); s_n7(x); s_n10(x); s_n255(x);
    s_n641(x); s_n1000(x); s_nmax(x);
    u_1(r); u_2(r); u_3(r); u_5(r); u_6(r); u_7(r); u_10(r); u_12(r);
    u_14(r); u_19(r); u_25(r); u_28(r); u_255(r); u_641(r); u_1000(r);
    u_65537(r); u_1e9(r); u_max(r); u_h(r); u_h1(r); u_fe(r); u_ff(r);
    u_big(r); u_b3(r);
}

int main(void)
{
    static const u32 edge[] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 14, 15, 24, 25, 26, 27,
        99, 100, 101, 124, 125, 126, 254, 255, 256, 640, 641, 642, 999,
        1000, 1001, 59999, 60000, 60001, 65536u, 65537u, 131074u,
        0x7ffffffdu, 0x7ffffffeu, 0x7fffffffu, 0x80000000u, 0x80000001u,
        0x80000002u, 0xbfffffffu, 0xc0000000u, 0xc0000001u, 2999999999u,
        3000000000u, 3000000001u, 0xfffffff6u, 0xfffffff9u, 0xffffff9cu,
        0xfffffc18u, 0xfffffffdu, 0xfffffffeu, 0xffffffffu,
        1000000006u, 1000000007u, 1000000008u, 0x92492492u, 0x24924924u,
        0xccccccccu, 0x33333333u, 0x55555555u, 0xaaaaaaaau
    };
    u32 seed = 2463534242u;
    for (unsigned i = 0; i < sizeof edge / sizeof edge[0]; i++)
        one(edge[i]);
    for (int i = 0; i < 3000; i++) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        one(seed);
        one(seed >> (i & 31));
        one(0u - (seed >> (i & 31)));
    }
    return bad == 0 ? 42 : 1;
}
