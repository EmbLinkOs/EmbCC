// CX8: forms libstdc++'s headers use — variable templates' explicit and
// partial specializations; a class template's defaults given on its
// forward declaration; an explicit specialization declared, then defined;
// a friend class template; constructor templates (constexpr, in an
// explicit specialization, beside the injected-class-name); using-
// declarations joining overload sets; an alias declared with an
// attribute; parameters in scope in a trailing return type; `(X<T>::v &&
// ...)` in a template argument; the floating classification and library
// builtins; std::max_align_t.
// expect-exit: 42
#include <stddef.h>
#include <stdio.h>

static int fails;
static void check(const char *what, bool ok)
{
    if (!ok)
        fails++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

template <class T> constexpr int kind = 0;
template <> constexpr int kind<int> = 1;
template <class T> constexpr int kind<T *> = 2;
template <class T> constexpr int kind<const T> = 3 + kind<T>;
template <template <class> class> constexpr bool is_box = false;
template <class> struct Box {};
template <> constexpr bool is_box<Box> = true;

template <class T> struct traits;
template <class C, class Tr = traits<C>> class str;
template <> struct traits<char>;
template <> struct traits<char> { static constexpr int id = 7; };
template <class C, class Tr> class str {
public:
    int id() const { return Tr::id; }
    template <class, class> friend class str;
};

struct tag {};
template <class... T> class tup { public: tup() {} };
template <> class tup<> {
public:
    int how = 0;
    tup() = default;
    tup(const tup &) = default;
    template <class A> constexpr tup(tag, const A &) noexcept : how(1) {}
    template <class A> constexpr tup(tag, const A &, const tup &) noexcept : how(2) {}
};

namespace inner {
struct W { int v; };
inline int twice(W w) { return w.v * 2; }
int twice(int x) { return x * 2; }
}
namespace outer {
int twice(double d) { return (int)(d * 4); }
using inner::twice;
}

struct Alloc {
    using always_equal [[deprecated("x")]] = int;
    using plain = long;
};

template <class I> struct rev { I i; I base() const { return i; } };
template <class L, class R>
auto operator-(const rev<L> &x, const rev<R> &y) -> decltype(y.base() - x.base())
{
    return y.base() - x.base();
}

template <class T> struct arith { static constexpr bool value = true; };
template <> struct arith<void *> { static constexpr bool value = false; };
template <bool B, class T> struct enable_if {};
template <class T> struct enable_if<true, T> { using type = T; };
template <class T, class U>
typename enable_if<(arith<T>::value && arith<U>::value), int>::type both(T, U)
{
    return 1;
}

int main()
{
    check("variable template specializations",
          kind<char> == 0 && kind<int> == 1 && kind<char *> == 2 &&
          kind<const int> == 4 && is_box<Box>);
    str<char> s;
    check("defaults from a forward declaration", s.id() == 7);
    tup<> a;
    tup<> b(tag(), 2.0);
    tup<> c(tag(), 'x', a);
    check("constructor templates", a.how == 0 && b.how == 1 && c.how == 2);
    check("using-declarations join overloads",
          outer::twice(1.5) == 6 && outer::twice(3) == 6 &&
          outer::twice(inner::W{ 5 }) == 10);
    check("an alias with an attribute", sizeof(Alloc::plain) == sizeof(long));
    rev<int> r1{ 3 }, r2{ 5 };
    check("parameters in a trailing return type", r1 - r2 == 2);
    check("&& in a template argument", both(1, 2.0) == 1);
    double inf = __builtin_huge_val(), nan = __builtin_nan("");
    check("classification builtins",
          __builtin_isnan(nan) && __builtin_isinf(inf) &&
          !__builtin_isfinite(inf) && __builtin_isnormal(1.0) &&
          !__builtin_isnormal(1e-40f) && __builtin_signbit(-0.0) &&
          !__builtin_signbit(1.0f) && __builtin_isunordered(nan, 1.0) &&
          __builtin_fpclassify(0, 1, 2, 3, 4, 0.0) == 4);
    check("library builtins",
          __builtin_sqrt(16.0) == 4 && __builtin_fabs(-1.5) == 1.5 &&
          __builtin_strlen("abcd") == 4 &&
          __builtin_memcmp("ab", "ac", 2) < 0 && __builtin_powf(2, 3) == 8);
    check("max_align_t", sizeof(max_align_t) == 32 && alignof(max_align_t) == 16);
    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 42;
}
