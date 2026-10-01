/* Two compares of one value against constants, joined by && or ||, fold
 * into one unsigned interval test: (x - lo) <u hi - lo + 1. Every pair of
 * bounds (lt/le/gt/ge on each side, either order), both branch forms (an
 * `if` with && and with ||) and both value forms (`return a && b`,
 * `return a || b`), at int, unsigned, char, long long and unsigned long
 * long, including intervals that are empty, ones where one bound is
 * redundant, and bounds at the very ends of the type -- each swept across
 * both edges and the extremes. The reference ANDs and ORs separately
 * computed compares without a branch, which is a shape the fold does not
 * touch. */
// expect-exit: 42
#define NI __attribute__((noinline))
typedef long long ll; typedef unsigned long long ull;

/* F(name, T, cond): an `if` form and a value form of cond(x), plus the
 * branch-free reference */
#define F(name, T, A, B, OP, BOP) \
    NI int name##_if(T x) { if (A OP B) return 7; return 3; } \
    NI int name##_val(T x) { return A OP B; } \
    NI int name##_ref(T x) { int p = A, q = B; return p BOP q; }

F(a1, int, x >= 10, x <= 20, &&, &)
F(a2, int, x > 10, x < 20, &&, &)
F(a3, int, x <= 20, x >= 10, &&, &)      /* upper bound first */
F(a4, int, x < 20, x > -5, &&, &)
F(a5, int, x >= 30, x <= 20, &&, &)      /* empty */
F(a6, int, x >= 10, x >= 5, &&, &)       /* both lower: not an interval pair */
F(o1, int, x < 10, x > 20, ||, |)
F(o2, int, x <= 10, x >= 20, ||, |)
F(o3, int, x > 20, x < 10, ||, |)
F(o4, int, x < -100, x > 100, ||, |)
F(e1, int, x >= -2147483647 - 1, x <= 5, &&, &)   /* lower bound at the minimum */
F(e2, int, x >= 5, x <= 2147483647, &&, &)        /* upper bound at the maximum */
F(e3, int, x > -2147483647 - 1, x < 2147483647, &&, &)
F(u1, unsigned, x >= 10u, x <= 20u, &&, &)
F(u2, unsigned, x < 10u, x > 0xFFFFFFF0u, ||, |)
F(u3, unsigned, x >= 0x7FFFFFF0u, x <= 0x80000010u, &&, &)  /* straddles the sign bit */
F(c1, char, x >= '0', x <= '9', &&, &)
F(c2, signed char, x < -100, x > 100, ||, |)
F(l1, ll, x >= -5000000000LL, x <= 5000000000LL, &&, &)
F(l2, ll, x < 0, x > 4294967296LL, ||, |)
F(l3, ull, x >= 10ull, x <= 0x8000000000000005ull, &&, &)

static int bad;
#define CHECK(name, T, v) do { T _x = (T)(v); \
    if (name##_if(_x) != (name##_ref(_x) ? 7 : 3) || name##_val(_x) != name##_ref(_x)) \
        bad = __LINE__; } while (0)
#define SWEEP(name, T, k) do { for (long long _d = -3; _d <= 3; _d++) CHECK(name, T, (long long)(k) + _d); } while (0)

int main(void)
{
    static const long long ints[] = { -2147483647LL - 1, -2147483647LL, -101, -100, -99, -6, -5, -4,
        -1, 0, 1, 4, 5, 6, 9, 10, 11, 19, 20, 21, 29, 30, 31, 99, 100, 101, 2147483646LL, 2147483647LL };
    for (unsigned i = 0; i < sizeof ints / sizeof *ints; i++) {
        long long v = ints[i];
        CHECK(a1, int, v); CHECK(a2, int, v); CHECK(a3, int, v); CHECK(a4, int, v);
        CHECK(a5, int, v); CHECK(a6, int, v); CHECK(o1, int, v); CHECK(o2, int, v);
        CHECK(o3, int, v); CHECK(o4, int, v); CHECK(e1, int, v); CHECK(e2, int, v);
        CHECK(e3, int, v);
    }
    SWEEP(u1, unsigned, 10); SWEEP(u1, unsigned, 20); SWEEP(u1, unsigned, 0); SWEEP(u1, unsigned, 0xFFFFFFFFu);
    SWEEP(u2, unsigned, 10); SWEEP(u2, unsigned, 0xFFFFFFF0u); SWEEP(u2, unsigned, 0); SWEEP(u2, unsigned, 0xFFFFFFFCu);
    SWEEP(u3, unsigned, 0x7FFFFFF0u); SWEEP(u3, unsigned, 0x80000010u); SWEEP(u3, unsigned, 0x80000000u);
    for (int c = -128; c < 128; c++) { CHECK(c1, char, c); CHECK(c2, signed char, c); }
    SWEEP(l1, ll, -5000000000LL); SWEEP(l1, ll, 5000000000LL); SWEEP(l1, ll, 0);
    CHECK(l1, ll, (ll)0x8000000000000000ull); CHECK(l1, ll, 0x7FFFFFFFFFFFFFFFLL);
    SWEEP(l2, ll, 0); SWEEP(l2, ll, 4294967296LL); CHECK(l2, ll, (ll)0x8000000000000000ull);
    SWEEP(l3, ull, 10); SWEEP(l3, ull, 0x8000000000000005ull); SWEEP(l3, ull, 0); CHECK(l3, ull, ~0ull);
    if (bad) return bad & 0x7f ? bad & 0x7f : 1;
    return 42;
}
