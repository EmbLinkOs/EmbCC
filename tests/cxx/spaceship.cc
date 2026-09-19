// CX7: three-way comparison — the built-in <=> (integers, enums and
// pointers give std::strong_ordering, floating types std::partial_ordering,
// unordered with a NaN); a class's operator<=> used for <, >, <=, >= through
// the rewritten candidates (x <=> y) @ 0, and reversed, 0 @ (y <=> x);
// != as !(x == y), and == reversed; defaulted == and <=> (bases, then
// members, in order; `auto` the weakest category of the members'); a
// defaulted relational operator; a static operator(); a hidden friend
// template. The comparison categories are declared here the way
// libstdc++'s <compare> declares them (tests/cxx programs see newlib only).
// expect-exit: 42
#include <stdio.h>

namespace std {
namespace __cmp_cat {
using type = signed char;
enum class _Ord : type { equivalent = 0, less = -1, greater = 1, unordered = -128 };
struct __literal_zero { consteval __literal_zero(__literal_zero *) noexcept {} };
}
class partial_ordering {
    __cmp_cat::type _M_value;
    constexpr explicit partial_ordering(__cmp_cat::_Ord v) noexcept : _M_value(__cmp_cat::type(v)) {}
public:
    static const partial_ordering less, equivalent, greater, unordered;
    friend constexpr bool operator==(partial_ordering v, __cmp_cat::__literal_zero) noexcept { return v._M_value == 0; }
    friend constexpr bool operator==(partial_ordering, partial_ordering) noexcept = default;
    friend constexpr bool operator<(partial_ordering v, __cmp_cat::__literal_zero) noexcept { return v._M_value == -1; }
    friend constexpr bool operator>(partial_ordering v, __cmp_cat::__literal_zero) noexcept { return v._M_value == 1; }
    friend constexpr bool operator<=(partial_ordering v, __cmp_cat::__literal_zero) noexcept { return v._M_value == -1 || v._M_value == 0; }
    friend constexpr bool operator>=(partial_ordering v, __cmp_cat::__literal_zero) noexcept { return v._M_value == 1 || v._M_value == 0; }
    friend constexpr bool operator<(__cmp_cat::__literal_zero, partial_ordering v) noexcept { return v._M_value == 1; }
    friend constexpr bool operator>(__cmp_cat::__literal_zero, partial_ordering v) noexcept { return v._M_value == -1; }
    friend constexpr bool operator<=(__cmp_cat::__literal_zero, partial_ordering v) noexcept { return v._M_value == 1 || v._M_value == 0; }
    friend constexpr bool operator>=(__cmp_cat::__literal_zero, partial_ordering v) noexcept { return v._M_value == -1 || v._M_value == 0; }
    friend constexpr partial_ordering operator<=>(partial_ordering v, __cmp_cat::__literal_zero) noexcept { return v; }
    friend constexpr partial_ordering operator<=>(__cmp_cat::__literal_zero, partial_ordering v) noexcept
    { return v._M_value == 1 ? less : v._M_value == -1 ? greater : v; }
    int raw() const { return _M_value; }
};
inline constexpr partial_ordering partial_ordering::less(__cmp_cat::_Ord::less);
inline constexpr partial_ordering partial_ordering::equivalent(__cmp_cat::_Ord::equivalent);
inline constexpr partial_ordering partial_ordering::greater(__cmp_cat::_Ord::greater);
inline constexpr partial_ordering partial_ordering::unordered(__cmp_cat::_Ord::unordered);

class strong_ordering {
    __cmp_cat::type _M_value;
    constexpr explicit strong_ordering(__cmp_cat::_Ord v) noexcept : _M_value(__cmp_cat::type(v)) {}
public:
    static const strong_ordering less, equal, equivalent, greater;
    constexpr operator partial_ordering() const noexcept
    { return _M_value == 0 ? partial_ordering::equivalent : _M_value < 0 ? partial_ordering::less : partial_ordering::greater; }
    friend constexpr bool operator==(strong_ordering v, __cmp_cat::__literal_zero) noexcept { return v._M_value == 0; }
    friend constexpr bool operator==(strong_ordering, strong_ordering) noexcept = default;
    friend constexpr bool operator<(strong_ordering v, __cmp_cat::__literal_zero) noexcept { return v._M_value < 0; }
    friend constexpr bool operator>(strong_ordering v, __cmp_cat::__literal_zero) noexcept { return v._M_value > 0; }
    friend constexpr bool operator<=(strong_ordering v, __cmp_cat::__literal_zero) noexcept { return v._M_value <= 0; }
    friend constexpr bool operator>=(strong_ordering v, __cmp_cat::__literal_zero) noexcept { return v._M_value >= 0; }
    friend constexpr bool operator<(__cmp_cat::__literal_zero, strong_ordering v) noexcept { return 0 < v._M_value; }
    friend constexpr bool operator>(__cmp_cat::__literal_zero, strong_ordering v) noexcept { return 0 > v._M_value; }
    friend constexpr bool operator<=(__cmp_cat::__literal_zero, strong_ordering v) noexcept { return 0 <= v._M_value; }
    friend constexpr bool operator>=(__cmp_cat::__literal_zero, strong_ordering v) noexcept { return 0 >= v._M_value; }
    friend constexpr strong_ordering operator<=>(strong_ordering v, __cmp_cat::__literal_zero) noexcept { return v; }
    friend constexpr strong_ordering operator<=>(__cmp_cat::__literal_zero, strong_ordering v) noexcept
    { return strong_ordering(__cmp_cat::_Ord(-v._M_value)); }
    int raw() const { return _M_value; }
};
inline constexpr strong_ordering strong_ordering::less(__cmp_cat::_Ord::less);
inline constexpr strong_ordering strong_ordering::equal(__cmp_cat::_Ord::equivalent);
inline constexpr strong_ordering strong_ordering::equivalent(__cmp_cat::_Ord::equivalent);
inline constexpr strong_ordering strong_ordering::greater(__cmp_cat::_Ord::greater);
}

static int fails;
static void check(const char *what, bool ok)
{
    if (!ok)
        fails++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

// ---- classes ----
struct Version {
    int major, minor;
    std::strong_ordering operator<=>(const Version &o) const
    {
        if (auto c = major <=> o.major; c != 0)
            return c;
        return minor <=> o.minor;
    }
    bool operator==(const Version &o) const { return major == o.major && minor == o.minor; }
};

struct Base { int id; auto operator<=>(const Base &) const = default; bool operator==(const Base &) const = default; };
struct Point : Base {
    int x, y;
    auto operator<=>(const Point &) const = default;
    bool operator==(const Point &) const = default;
};
struct Temp {
    double deg;
    auto operator<=>(const Temp &) const = default;   // partial: a double member
    bool operator==(const Temp &) const = default;
};
struct Tag {
    int k;
    friend bool operator==(const Tag &, const Tag &) = default;
    bool operator<(const Tag &o) const = default;         // through <=>
    std::strong_ordering operator<=>(const Tag &o) const { return k <=> o.k; }
};
struct Meters {
    int v;
    friend std::strong_ordering operator<=>(const Meters &a, int b) { return a.v <=> b; }
    friend bool operator==(const Meters &a, int b) { return a.v == b; }
};

struct Less {
    static bool operator()(int a, int b) { return (a <=> b) < 0; }   // static
};

template <class T> struct Box {
    T v;
    template <class U>
    friend bool operator==(const Box &a, const Box<U> &b) { return a.v == b.v; }
};

int main()
{
    // ---- built in ----
    check("ints", (1 <=> 2) < 0 && (2 <=> 2) == 0 && (3 <=> 2) > 0 &&
                  (1 <=> 2).raw() == -1 && (5 <=> 1).raw() == 1);
    check("unsigned and mixed", (2u <=> 1u) > 0 && (-1 <=> 1L) < 0);
    enum E { A, B, C };
    check("enums", (A <=> C) < 0 && (C <=> B) > 0);
    int arr[3];
    check("pointers", (&arr[0] <=> &arr[2]) < 0 && (&arr[1] <=> &arr[1]) == 0);
    double nan = 0.0 / 0.0;
    std::partial_ordering p = 1.5 <=> 0.5;
    check("floating", p > 0 && (0.5 <=> 1.5) < 0 && (2.0 <=> 2.0) == 0);
    std::partial_ordering u = nan <=> 1.0;
    check("unordered", !(u < 0) && !(u > 0) && !(u == 0) && u.raw() == -128);
    int calls = 0;
    auto next = [&] { return ++calls; };
    check("operands evaluated once", (next() <=> next()) < 0 && calls == 2);

    // ---- rewritten ----
    Version v1{1, 2}, v2{1, 10}, v3{2, 0};
    check("< > <= >= through <=>", v1 < v2 && v3 > v2 && v1 <= v1 && v2 >= v1 &&
                                   !(v2 < v1));
    check("!= through ==", v1 != v2 && !(v1 != v1));
    check("<=> itself", (v1 <=> v3) < 0 && (v3 <=> v3) == 0);
    Meters m{5};
    check("reversed <=>", 3 < m && 7 > m && !(5 < m) && (4 <=> m) < 0);
    check("reversed ==", 5 == m && m == 5 && 4 != m);

    // ---- defaulted ----
    Point a{{1}, 2, 3}, b{{1}, 2, 4}, c{{0}, 9, 9};
    check("defaulted == (bases, members)", a == a && a != b && !(a == c));
    check("defaulted <=>, lexicographic", a < b && c < a && (b <=> a) > 0 &&
                                          (a <=> a) == 0);
    Temp t1{20.5}, t2{21.0}, tn{nan};
    std::partial_ordering tp = t1 <=> t2;
    check("defaulted <=> is partial with a double", tp < 0 &&
          !((tn <=> t1) < 0) && !((tn <=> t1) > 0));
    Tag g1{1}, g2{2};
    check("defaulted friend ==, defaulted <", g1 == g1 && g1 != g2 && g1 < g2 &&
                                              !(g2 < g1));

    // ---- static operator(), friend template ----
    check("static operator()", Less()(1, 2) && !Less{}(2, 1));
    Box<int> bi{3};
    Box<long> bl{3};
    check("hidden friend template", bi == bl && !(bi != bl));

    static_assert((1 <=> 2) < 0 && (2.0 <=> 1.0) > 0);

    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 42;
}
