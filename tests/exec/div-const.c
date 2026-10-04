// expect-exit: 42
/* Division and remainder by a constant, which the optimizer turns into a
 * multiply by a "magic" number and shifts, checked against the same
 * division done by the hardware through a volatile divisor. The divisors
 * cover both shapes the expansion takes -- with a correction step (7
 * unsigned; 7, -10 signed) and without (10, 26, 641 unsigned; 3, 10, -7
 * signed) -- powers of two, and the extremes. The dividends are the edges
 * (0, +-1, the limits, multiples of the divisor and their neighbours) and
 * a pseudo-random sweep. */
typedef unsigned u32;

static volatile int vs;
static volatile u32 vu;
static int bad;

#define SDIV(D)                                                           \
    static void s_##D(int x)                                              \
    {                                                                     \
        vs = (D);                                                         \
        if (x / (D) != x / vs || x % (D) != x % vs)                       \
            bad++;                                                        \
    }
#define UDIV(D)                                                           \
    static void u_##D(u32 x)                                              \
    {                                                                     \
        vu = (D##u);                                                      \
        if (x / (D##u) != x / vu || x % (D##u) != x % vu)                 \
            bad++;                                                        \
    }

SDIV(3) SDIV(5) SDIV(6) SDIV(7) SDIV(10) SDIV(12) SDIV(25) SDIV(26)
SDIV(100) SDIV(641) SDIV(1000) SDIV(16) SDIV(1024) SDIV(2147483647)
#define m3 (-3)
#define m7 (-7)
#define m10 (-10)
#define m100 (-100)
#define m16 (-16)
static void s_m3(int x)   { vs = m3;   if (x / m3 != x / vs || x % m3 != x % vs) bad++; }
static void s_m7(int x)   { vs = m7;   if (x / m7 != x / vs || x % m7 != x % vs) bad++; }
static void s_m10(int x)  { vs = m10;  if (x / m10 != x / vs || x % m10 != x % vs) bad++; }
static void s_m100(int x) { vs = m100; if (x / m100 != x / vs || x % m100 != x % vs) bad++; }
static void s_m16(int x)  { vs = m16;  if (x / m16 != x / vs || x % m16 != x % vs) bad++; }

UDIV(3) UDIV(5) UDIV(7) UDIV(10) UDIV(13) UDIV(26) UDIV(100) UDIV(641)
UDIV(1000) UDIV(19) UDIV(2147483647) UDIV(2147483649) UDIV(4294967295)
UDIV(65537) UDIV(1000000007)

static void one(u32 r)
{
    int x = (int)r;
    s_3(x); s_5(x); s_6(x); s_7(x); s_10(x); s_12(x); s_25(x); s_26(x);
    s_100(x); s_641(x); s_1000(x); s_16(x); s_1024(x); s_2147483647(x);
    s_m3(x); s_m7(x); s_m10(x); s_m100(x); s_m16(x);
    u_3(r); u_5(r); u_7(r); u_10(r); u_13(r); u_26(r); u_100(r); u_641(r);
    u_1000(r); u_19(r); u_2147483647(r); u_2147483649(r); u_4294967295(r);
    u_65537(r); u_1000000007(r);
}

int main(void)
{
    static const u32 edge[] = {
        0, 1, 2, 3, 6, 7, 9, 10, 11, 25, 26, 27, 99, 100, 101, 640, 641, 642,
        999, 1000, 1001, 0x7ffffffeu, 0x7fffffffu, 0x80000000u, 0x80000001u,
        0xfffffffeu, 0xffffffffu, 0xfffffff6u, 0xfffffff9u, 0xffffff9cu,
        1000000006u, 1000000007u, 1000000008u, 65536u, 65537u, 131074u
    };
    u32 seed = 12345u;
    for (unsigned i = 0; i < sizeof edge / sizeof edge[0]; i++)
        one(edge[i]);
    for (int i = 0; i < 20000; i++) {
        seed = seed * 1103515245u + 12345u;
        one(seed ^ (seed >> 7));
        one(seed >> (i & 31));
    }
    return bad == 0 ? 42 : 1;
}
