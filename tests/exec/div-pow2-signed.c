/* Signed division and remainder by a constant power of two, which the
 * optimizer turns into shifts and an add on every target (pass_divmagic):
 * each divisor from 2 to 2^30, its negative, and INT_MIN, against the
 * same division by a divisor the compiler cannot see (a real divide),
 * for dividends at both extremes, around zero and around the divisor. */
// expect-exit: 42
#include <limits.h>

#define NI __attribute__((noinline))
#define D(k)                                                           \
    NI static int q##k(int x) { return x / (1 << k); }                  \
    NI static int r##k(int x) { return x % (1 << k); }                  \
    NI static int nq##k(int x) { return x / -(1 << k); }                \
    NI static int nr##k(int x) { return x % -(1 << k); }
D(1) D(2) D(3) D(5) D(8) D(11) D(15) D(16) D(20) D(29) D(30)
NI static int qmin(int x) { return x / INT_MIN; }
NI static int rmin(int x) { return x % INT_MIN; }

static volatile int dv;

static int check(int k, int (*q)(int), int (*r)(int), int (*nq)(int),
                 int (*nr)(int))
{
    static const int xs[] = { 0, 1, -1, 2, -2, 7, -7, 100, -100, 2047, -2047,
                              2048, -2048, 2049, -2049, 65535, -65537,
                              INT_MAX, INT_MIN, INT_MAX - 1, INT_MIN + 1,
                              0x40000000, -0x40000000, 123456789, -987654321 };
    for (unsigned i = 0; i < sizeof xs / sizeof xs[0]; i++) {
        int x = xs[i], d = 1 << k;
        dv = d;
        if (q(x) != x / dv || r(x) != x % dv) return 1;
        dv = -d;
        if (!(x == INT_MIN && d == 1) && (nq(x) != x / dv || nr(x) != x % dv))
            return 2;
        /* around the divisor */
        for (int e = -1; e <= 1; e++) {
            int y = d + e;
            dv = d;
            if (q(y) != y / dv || r(y) != y % dv) return 3;
            if (q(-y) != -y / dv || r(-y) != -y % dv) return 4;
        }
    }
    return 0;
}

int main(void)
{
    static const struct {
        int k; int (*q)(int), (*r)(int), (*nq)(int), (*nr)(int);
    } t[] = {
#define E(k) { k, q##k, r##k, nq##k, nr##k },
        E(1) E(2) E(3) E(5) E(8) E(11) E(15) E(16) E(20) E(29) E(30)
    };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++)
        if (check(t[i].k, t[i].q, t[i].r, t[i].nq, t[i].nr))
            return 10 + (int)i;
    static const int xs[] = { 0, 1, -1, INT_MAX, INT_MIN, INT_MIN + 1, 12345 };
    for (unsigned i = 0; i < sizeof xs / sizeof xs[0]; i++) {
        dv = INT_MIN;
        if (qmin(xs[i]) != xs[i] / dv || rmin(xs[i]) != xs[i] % dv)
            return 30;
    }
    return 42;
}
