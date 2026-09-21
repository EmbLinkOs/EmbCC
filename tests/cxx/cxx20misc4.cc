// CX7/CX8, the rules <ranges> and <functional> needed: explicit object
// parameters (C++23 `this Self &&self`, which libstdc++ uses whenever the
// compiler says it is g++ 14 or later); conversion function templates;
// constraint subsumption ordering partial specializations and overloads;
// a member's constraints checked when it is used, not when its class
// template is instantiated (view_interface<D> of an incomplete D); a
// conjunction's right side not checked when its left one fails; hidden
// friends found only by argument-dependent lookup, which also looks in
// base classes' and template arguments' namespaces, and never for a
// qualified name; a default member initializer read only when used; a
// function bound to a reference to its type or to a const reference to a
// pointer, and a parameter with no template parameter left out of
// deduction; [[no_unique_address]] (an empty member taking no room, as
// g++ lays it out — std::tuple's, so unordered_map's — and its copy
// writing nothing); if constexpr (...) static_assert(...).
// expect-exit: 42
#include <stdio.h>

static int fails;
static void check(const char *what, bool ok)
{
    if (!ok)
        fails++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

// ---- explicit object parameters ----
struct Self {
    int v = 1;
    template <class S> int get(this S &&self) { return self.v + (__is_reference(S) ? 10 : 100); }
    int times(this const Self &self, int k) { return self.v * k; }
    template <class S> int operator()(this S &&self, int k) { return self.v + k; }
};
struct SelfD : Self { int w = 5; };
struct Guarded {
    int n;
    Guarded(Guarded *) : n(7) {}
    int make() { Guarded g(this); return g.n; }      // (this): an argument
};

// ---- conversion function templates ----
template <class T> concept big = sizeof(T) >= 4;
struct Any { int v; template <class T> operator T() const { return T(v); } };
struct Both {
    operator int() const { return 1; }
    template <class T> operator T() const { return T(2); }
};
struct Wide { template <class T> requires big<T> explicit operator T() const { return T(7); } };

// ---- subsumption ----
template <class T> concept walk = requires(T t) { t.step(); };
template <class T> concept jump = walk<T> && requires(T t) { t.leap(); };
template <class T> struct Cache { static constexpr int kind = 0; };
template <walk T> struct Cache<T> { static constexpr int kind = 1; };
template <jump T> struct Cache<T> { static constexpr int kind = 2; };
struct Walker { void step(); };
struct Jumper { void step(); void leap(); };
template <walk T> int how(T) { return 1; }
template <jump T> int how(T) { return 2; }
template <class T> struct Range {
    int end() const { return 1; }
    int end() const requires jump<T> { return 2; }  // more constrained
};

// ---- a member's constraints, checked on use ----
template <class T> concept sized = requires(T &t) { t.size(); };
template <class D> struct Iface {
    int n() requires sized<D> { return static_cast<D *>(this)->size(); }
};
struct Vec : Iface<Vec> { int size() { return 3; } };   // Vec incomplete in Iface<Vec>

// ---- a conjunction's right side ----
template <class T> struct Bad { using type = typename T::missing; };
template <class T> concept never = false && requires { typename Bad<T>::type; };

// ---- lookup ----
namespace nsf {
struct S { friend int get(S) { return 1; } };
inline namespace cpo { inline constexpr int get = 42; }
}
namespace ma { struct B {}; int by_base(B) { return 3; } struct T {}; int by_arg(...) { return 4; } }
struct DB : ma::B {};
template <class X> struct Wrap {};
namespace qa { struct X {}; int f(X) { return 2; } }
namespace qb { int f(qa::X, int = 0) { return 1; } }

// ---- default member initializers ----
struct NoDefault { int v; NoDefault(int x) : v(x) {} };
template <class T> struct Holds {
    T t = T();                     // T() is not valid: never read here
    Holds(int x) : t(x) {}
};

// ---- a function bound to a reference ----
static int sub(int a, int b) { return a - b; }
template <class F> int call(F &&f) { return f(50, 8); }
static int call_ref(int (&f)(int, int)) { return f(50, 8); }

using FP = int (*)(int, int);
struct Holder { FP f; template <class... A> Holder(const FP &g, A &&...) : f(g) {} };
template <class A> int via_ptr(const FP &g, A &&) { return g(50, 8); }

// ---- [[no_unique_address]] ----
struct Empty {};
struct Solo { [[no_unique_address]] Empty e; };
struct AfterSolo : Solo { int x; };
struct Packed { [[no_unique_address]] Empty e; int x; };
template <int I, class H> struct HeadBase { HeadBase(const H &h) : m(h) {} [[no_unique_address]] H m; };
struct Tail : HeadBase<1, int> { Tail(int v) : HeadBase<1, int>(v) {} };
struct Tup : Tail, HeadBase<0, Empty> { Tup(Empty e, int v) : Tail(v), HeadBase<0, Empty>(e) {} };

template <class T> int checked()
{
    if constexpr (sizeof(T) > 0)
        static_assert(sizeof(T) < 64, "small");
    return 1;
}

int main()
{
    Self s;
    SelfD d;
    check("explicit object parameters", s.get() == 11 &&
                                        static_cast<Self &&>(s).get() == 101 &&
                                        s.times(7) == 7 && s(41) == 42 &&
                                        d.get() == 11 && Self{}(2) == 3);
    Guarded g(nullptr);
    check("(this) is an argument", g.make() == 7);

    Any a{5};
    int ai = a;
    double ad = a;
    Both b;
    int bi = b;
    double bd = b;
    Wide w;
    check("conversion function templates", ai == 5 && ad == 5.0 && bi == 1 &&
                                           bd == 2.0 && static_cast<long>(w) == 7);

    check("partial specializations by subsumption",
          Cache<int>::kind == 0 && Cache<Walker>::kind == 1 && Cache<Jumper>::kind == 2);
    check("overloads by subsumption", how(Walker{}) == 1 && how(Jumper{}) == 2);
    check("members by their constraints", Range<Walker>{}.end() == 1 &&
                                          Range<Jumper>{}.end() == 2);
    Vec v;
    check("a member's constraints checked on use", v.n() == 3);
    check("a conjunction stops at false", !never<int>);

    check("a hidden friend: ADL only", get(nsf::S{}) == 1 && nsf::get == 42);
    check("ADL: bases and template arguments", by_base(DB{}) == 3 &&
                                               by_arg(Wrap<ma::T>{}) == 4);
    check("a qualified call: no ADL", qb::f(qa::X{}) == 1 && f(qa::X{}) == 2);

    Holds<NoDefault> h(40);
    check("a default member initializer read when used", h.t.v == 40);
    check("a function bound to a reference", call(sub) == 42 && call_ref(sub) == 42);
    Holder hd(sub, 1);
    check("... to a const reference to a pointer", hd.f(50, 8) == 42 &&
                                                   via_ptr(sub, 1) == 42);
    Tup tup(Empty{}, 10);
    Packed pk{};
    pk.x = 42;
    check("[[no_unique_address]]", sizeof(Solo) == 1 && sizeof(AfterSolo) == 4 &&
                                   sizeof(Packed) == 4 && sizeof(Tup) == 4 &&
                                   static_cast<Tail &>(tup).m == 10 && pk.x == 42);
    check("if constexpr ... static_assert", checked<int>() == 1);

    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 42;
}
