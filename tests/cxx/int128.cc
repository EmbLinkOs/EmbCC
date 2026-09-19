// GNU __int128 in C++: overloads on it (mangled n and o), templates over
// it, the usual arithmetic conversions (it outranks long long), constant
// expressions computed in 128 bits (static_assert, a template argument, a
// const static member whose value no long holds), and the arithmetic itself
// (lowered to C, which EmbCC's C computes in two eightbytes). Expected
// values computed independently (Python).
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
    u128 fact = 1;
    for (int k = 2; k <= 30; k++)
        fact *= k;
    CHECK((unsigned long)(fact >> 64) == 0xd13f6370f96UL &&   /* 30! */
          (unsigned long)fact == 0x865df5dd54000000UL);
    CHECK(fact / 1000000007 % 1000 == 361);
    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 42;
}
