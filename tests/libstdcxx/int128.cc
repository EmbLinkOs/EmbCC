// __int128 through libstdc++: its traits (is_integral, make_unsigned),
// numeric_limits (constants computed in 128 bits), to_chars/from_chars
// (libstdc++'s __int128 overloads, whose overflow checks use
// __builtin_mul_overflow in 128 bits), a template over it, and
// uniform_int_distribution (Lemire's method multiplies in __uint128_t).
#include <charconv>
#include <cstdio>
#include <limits>
#include <random>
#include <type_traits>
#include <cstdint>
using u128 = unsigned __int128;
using i128 = __int128;
static void pr(const char *what, u128 v)
{
    std::printf("%s %016lx%016lx\n", what, (unsigned long)(v >> 64), (unsigned long)v);
}
template <class T> T twice(T x) { return x * 2; }
static u128 f(u128 a, int b) { return a << b; }
int main()
{
    static_assert(std::is_integral_v<i128> && std::is_signed_v<i128> && std::is_unsigned_v<u128>);
    static_assert(std::is_same_v<std::make_unsigned_t<i128>, u128>);
    static_assert(sizeof(i128) == 16 && alignof(i128) == 16);
    pr("max", std::numeric_limits<i128>::max());
    pr("min", (u128)std::numeric_limits<i128>::min());
    pr("umax", std::numeric_limits<u128>::max());
    std::printf("digits %d %d\n", std::numeric_limits<i128>::digits, std::numeric_limits<u128>::digits10);
    char buf[64];
    i128 big = (i128)1 << 100;
    auto r = std::to_chars(buf, buf + sizeof buf, -big);
    *r.ptr = 0;
    std::printf("to_chars %s\n", buf);
    i128 back = 0;
    std::from_chars(buf, r.ptr, back);
    std::printf("from_chars %d\n", back == -big);
    pr("twice", twice<u128>(big));
    pr("f", f(3, 70));
    std::mt19937_64 g(42);
    std::uniform_int_distribution<std::uint64_t> d(0, 1000);
    unsigned long s = 0;
    for (int i = 0; i < 100; i++) s += d(g);
    std::printf("uniform %lu\n", s);
    return 0;
}
