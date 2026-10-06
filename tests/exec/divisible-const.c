// expect-exit: 42
/* `x % C == 0` and `x % C != 0` for constant C, which the optimizer
 * answers with a multiply instead of a division (pass_divtest): odd and
 * even divisors, powers of two, 1, negative divisors and INT_MIN for
 * int, and large ones for unsigned -- as a value and as a branch --
 * against the same remainder taken through a volatile divisor, which
 * nothing can fold. */
#include <limits.h>

#define NI __attribute__((noinline))

static volatile int vd;
static volatile unsigned vu;

#define SD(name, C)                                                         \
    NI static int name##_v(int x) { return x % (C) == 0; }                  \
    NI static int name##_b(int x) { if (x % (C) != 0) return 7; return 9; }
#define UD(name, C)                                                         \
    NI static int name##_v(unsigned x) { return x % (C) == 0; }             \
    NI static int name##_b(unsigned x) { if (x % (C) != 0) return 7; return 9; }

SD(s3, 3) SD(s5, 5) SD(s6, 6) SD(s7, 7) SD(s10, 10) SD(s12, 12)
SD(s100, 100) SD(s400, 400) SD(s641, 641) SD(sm3, -3) SD(sm12, -12)
SD(s1, 1) SD(s4, 4) SD(s1024, 1024) SD(smin, INT_MIN) SD(sbig, 0x7ffffffe)
UD(u3, 3u) UD(u10, 10u) UD(u24, 24u) UD(u1e9, 1000000000u)
UD(ubig, 0xfffffffbu) UD(u8, 8u) UD(uodd, 0x80000001u)

/* The remainder is also a value here: it must stay a remainder. */
NI static int both(int x) { int r = x % 7; return r * 10 + (r == 0); }

static unsigned seed = 12345;
static unsigned next(void) { seed = seed * 1664525u + 1013904223u; return seed; }

#define CHECKS(name, C) do {                                                \
    int x = xs[i]; vd = (C);                                                \
    int want = x % vd == 0;                                                  \
    if (name##_v(x) != want || name##_b(x) != (want ? 9 : 7)) bad = 1;      \
} while (0)
#define CHECKU(name, C) do {                                                \
    unsigned x = (unsigned)xs[i]; vu = (C);                                  \
    int want = x % vu == 0;                                                  \
    if (name##_v(x) != want || name##_b(x) != (want ? 9 : 7)) bad = 2;      \
} while (0)

int main(void)
{
    int xs[400];
    static const int edge[] = { 0, 1, -1, 2, -2, 3, -3, 6, -6, 12, -12, 100,
        -100, 400, -400, 1000, 641, -641, 1024, -1024, INT_MAX, INT_MIN,
        INT_MAX - 1, INT_MIN + 1, 0x7ffffffe, -0x7ffffffe, 2000000000,
        -2000000000, 1000000000, 1200, 1900, 2000, 2100 };
    int n = 0;
    for (unsigned k = 0; k < sizeof edge / sizeof edge[0]; k++)
        xs[n++] = edge[k];
    while (n < 300)
        xs[n++] = (int)next();
    while (n < 400) {                /* multiples, and one off them */
        int m = (int)((next() % 1000) * (n & 1 ? 12u : 641u)) + (n % 3 == 0);
        xs[n] = m;
        n++;
    }
    int bad = 0;
    for (int i = 0; i < n; i++) {
        CHECKS(s3, 3); CHECKS(s5, 5); CHECKS(s6, 6); CHECKS(s7, 7);
        CHECKS(s10, 10); CHECKS(s12, 12); CHECKS(s100, 100);
        CHECKS(s400, 400); CHECKS(s641, 641); CHECKS(sm3, -3);
        CHECKS(sm12, -12); CHECKS(s1, 1); CHECKS(s4, 4); CHECKS(s1024, 1024);
        CHECKS(sbig, 0x7ffffffe);
        /* INT_MIN % INT_MIN is 0, and x % INT_MIN is x otherwise */
        { int x = xs[i];
          int want = x == 0 || x == INT_MIN;
          if (smin_v(x) != want || smin_b(x) != (want ? 9 : 7)) bad = 3; }
        CHECKU(u3, 3u); CHECKU(u10, 10u); CHECKU(u24, 24u);
        CHECKU(u1e9, 1000000000u); CHECKU(ubig, 0xfffffffbu); CHECKU(u8, 8u);
        CHECKU(uodd, 0x80000001u);
        { int x = xs[i]; vd = 7; int r = x % vd;
          if (both(x) != r * 10 + (r == 0)) bad = 4; }
        if (bad) return bad;
    }
    return 42;
}
