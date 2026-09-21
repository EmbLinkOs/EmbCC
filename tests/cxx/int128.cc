// GNU __int128 in C++: overloads on it (mangled n and o), templates over
// it, the usual arithmetic conversions (it outranks long long), constant
// expressions computed in 128 bits (static_assert, a template argument, a
// const static member whose value no long holds), constexpr functions of
// it (loops, compound assignment, switch, conversions to double) and
// constexpr variables they initialize — statically, as C constants — and
// the arithmetic itself (lowered to C, which EmbCC's C computes in two
// eightbytes). Expected values computed independently (Python).
// expect-exit: 42
#include <stdio.h>

using i128 = __int128;
using u128 = unsigned __int128;

static int which(long) { return 1; }
static int which(i128) { return 2; }
static int which(u128) { return 3; }

template <class T> struct Limits {
    static const T max = T(-1) < T(0) ? T((((T(1) << 126) - 1) << 1) + 1)
                                      : T(~T(0));
};
template <class T> const T Limits<T>::max;

template <class T> T square(T x) { return x * x; }
template <int N> struct Int { static constexpr int value = N; };

static_assert((i128(1) << 100) > (i128(1) << 99), "128-bit comparison");
static_assert((u128(1) << 127) / (u128(1) << 120) == 128, "128-bit quotient");
static_assert(i128(-1) < i128(0) && u128(-1) > u128(0), "signedness");
static_assert(sizeof(i128) == 16 && alignof(u128) == 16, "layout");

constexpr i128 sq(i128 x) { return x * x; }
constexpr u128 fact(int n)
{
    u128 r = 1;
    for (int k = 2; k <= n; k++)
        r *= k;
    return r;
}
constexpr int digits(u128 x)
{
    int n = 0;
    do {
        n++;
        x /= 10;
    } while (x);
    return n;
}
constexpr int kind(i128 x)
{
    switch (x) {
    case 5: return 1;
    case -1: return 2;
    default: return 3;
    }
}
static_assert(sq(3) == 9 && fact(30) / fact(28) == 870, "calls");
static_assert((unsigned long)(fact(30) >> 64) == 0xd13f6370f96UL, "30!");
static_assert(digits(~(u128)0) == 39 && digits(0) == 1, "loops");
static_assert(kind(5) == 1 && kind(-1) == 2 && kind((i128)1 << 90) == 3,
              "switch");
static_assert((double)fact(25) > 1.551e25 && (double)fact(25) < 1.552e25,
              "to double");
constexpr i128 big = sq((i128)1 << 40);
constexpr i128 neg = -sq((i128)3 << 60) / 7;
static_assert(big == (i128)1 << 80 && neg < 0 && -neg > (i128)1 << 120,
              "constexpr variables");
u128 f30 = fact(30);                      // constant-initialized

int main()
{
    int fails = 0;
#define CHECK(c) do { if (!(c)) { fails++; printf("FAIL %s\n", #c); } } while (0)
    CHECK(which(1L) == 1 && which(i128(1)) == 2 && which(u128(1)) == 3);
    long l = -7;
    auto mixed = l + u128(10);            // unsigned __int128
    CHECK(mixed == 3 && sizeof mixed == 16 && which(mixed) == 3);
    auto smixed = i128(5) * -3LL;         // __int128
    CHECK(smixed == -15 && which(smixed) == 2);
    CHECK((u128)Limits<i128>::max == ((u128)1 << 127) - 1);
    CHECK(Limits<u128>::max == ~(u128)0);
    CHECK(square<u128>((u128)1 << 60) == (u128)1 << 120);
    CHECK(Int<int((i128(1) << 100) >> 98)>::value == 4);
    u128 prod = 1;
    for (int k = 2; k <= 30; k++)
        prod *= k;
    CHECK((unsigned long)(prod >> 64) == 0xd13f6370f96UL &&   /* 30! */
          (unsigned long)prod == 0x865df5dd54000000UL);
    CHECK(prod / 1000000007 % 1000 == 361);
    CHECK(f30 == prod && big == (i128)1 << 80 && neg == -sq((i128)3 << 60) / 7);
    static i128 ls = sq(1000000007);
    CHECK(ls == (i128)1000000007 * 1000000007);
    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 42;
}
