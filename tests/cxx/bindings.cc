// CX6: structured bindings — to an array's elements (copied, or
// referred to), to a class's data members, to a tuple-like class's parts
// through std::tuple_size / std::tuple_element and get<i> (a member
// template, or found by argument-dependent lookup); in a range-based for;
// seen from a lambda — and decltype(auto), for variables and returns.
// expect-exit: 42
#include <stdio.h>
namespace std {
template <class T> struct tuple_size;
template <unsigned long I, class T> struct tuple_element;
}
static int fails;
static void check(const char *what, bool ok)
{
    if (!ok)
        fails++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}
struct P { int x; double y; };
struct Trio {
    int a = 1, b = 2, c = 3;
    template <unsigned long I> int &get() { return I == 0 ? a : I == 1 ? b : c; }
};
namespace std {
template <> struct tuple_size<Trio> { static constexpr unsigned long value = 3; };
template <unsigned long I> struct tuple_element<I, Trio> { using type = int; };
}
struct Pair2 { int k; long v; };
namespace mine {
struct KV { int key; const char *val; };
template <unsigned long I> auto get(const KV &kv)
{
    if constexpr (I == 0)
        return kv.key;
    else
        return kv.val;
}
}
namespace std {
template <> struct tuple_size<mine::KV> { static constexpr unsigned long value = 2; };
template <> struct tuple_element<0, mine::KV> { using type = int; };
template <> struct tuple_element<1, mine::KV> { using type = const char *; };
}
static P make() { return P{ 4, 1.5 }; }
decltype(auto) ident(int &r) { return r; }
decltype(auto) paren(int &r) { return (r); }
int glob = 9;
decltype(auto) gref() { return (glob); }
int main()
{
    P p{ 1, 2.5 };
    auto [x, y] = p;
    x = 10;
    check("by copy", x == 10 && p.x == 1 && y == 2.5);
    auto &[rx, ry] = p;
    rx = 7;
    check("by reference", p.x == 7 && ry == 2.5);
    auto [mx, my] = make();
    check("from a prvalue", mx == 4 && my == 1.5);
    int arr[3] = { 5, 6, 7 };
    auto [a0, a1, a2] = arr;
    arr[0] = 50;
    check("array copy", a0 == 5 && a1 == 6 && a2 == 7);
    auto &[r0, r1, r2] = arr;
    r2 = 70;
    check("array reference", arr[2] == 70 && r0 == 50);
    Trio t;
    auto &[ta, tb, tc] = t;
    tb = 20;
    check("tuple-like, member get", t.b == 20 && ta == 1 && tc == 3);
    mine::KV kv{ 3, "three" };
    auto [k, v] = kv;
    check("tuple-like, get by ADL", k == 3 && v[0] == 't');
    Pair2 ps[2] = { { 1, 10 }, { 2, 20 } };
    long sum = 0;
    for (auto [kk, vv] : ps)
        sum += kk * vv;
    for (auto &[kk, vv] : ps)
        vv = 0;
    check("in range-for", sum == 50 && ps[1].v == 0);
    const auto [cx, cy] = p;
    check("const", cx == 7);
    int z = 3;
    auto l = [&] { return x + a1 + z; };
    check("in a lambda", l() == 19);
    int q = 1;
    decltype(auto) d1 = ident(q);
    decltype(auto) d2 = paren(q);
    d2 = 5;
    decltype(auto) d3 = q;
    decltype(auto) d4 = (q);
    d4 = 8;
    gref() = 11;
    check("decltype(auto)", sizeof(d1) == sizeof(int) && q == 8 && d3 == 5 && glob == 11);
    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 42;
}
