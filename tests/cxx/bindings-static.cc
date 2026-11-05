// Structured bindings with static storage: at namespace scope (an array's
// elements, a class's members, a tuple-like class's parts, a class with a
// destructor), in a namespace, static in a function (C++20: initialized
// once), and thread_local (declared only: the bare-metal harness has no
// thread pointer) -- the hidden object initialized with the unit's other
// globals, in order, and read from functions and lambdas. And what the
// tuple-like names need: a temporary bound to a reference with static
// storage lives as long (a static of its own, not a block-local that was
// gone -- its destructor run -- before main). And the members of a base
// class, when they are the only ones (at any depth).
// expect-exit: 42
#include <stdio.h>
#include <string.h>
typedef decltype(sizeof 0) size_t;
namespace std {
template <class T> struct tuple_size;
template <size_t I, class T> struct tuple_element;
}
static int fails;
static void check(const char *what, bool ok) { if (!ok) fails++; printf("%s %s\n", ok ? "ok  " : "FAIL", what); }

static char trail[64];
static int counter;
static int next() { return ++counter; }

struct Pt { int x, y; };
struct Pair {
    int a, b;
    template <size_t I> int get() const { return I == 0 ? a : b; }
};
namespace std {
template <> struct tuple_size<Pair> { static constexpr size_t value = 2; };
template <size_t I> struct tuple_element<I, Pair> { using type = int; };
}
struct B { int p, q; };
struct D : B { int sum() const { return p + q; } };
struct DD : D {};
struct Noisy {
    int v, w;
    ~Noisy() { strcat(trail, "~"); }
};

int arr[3] = { 7, 8, 9 };
auto [a0, a1, a2] = arr;                // a copy, made at start-up
auto &[r0, r1, r2] = arr;               // the array itself
auto [px, py] = Pt{ next(), next() };   // in the unit's order
auto [ta, tb] = Pair{ 30, 12 };         // tuple-like: get<0>, get<1>
const auto [nv, nw] = Noisy{ 5, 6 };
namespace ns {
auto [u, v] = Pt{ 100, 200 };
}
thread_local auto [tl0, tl1] = Pt{ 3, 4 };   // (declared only)
const int &gr = arr[0] + 70;
const Noisy &gn = Noisy{ 1, 2 };

static int once()
{
    static auto [s0, s1] = Pt{ next(), 0 };
    return s0 + s1;
}
static int ref_static(int k)
{
    static const Noisy &n = Noisy{ k, k * 2 };
    return n.v + n.w;
}

int main()
{
    check("array copy", a0 == 7 && a1 == 8 && a2 == 9);
    arr[1] = 80;
    check("the copy kept its value, the reference sees the array",
          a1 == 8 && r1 == 80);
    r2 = 90;
    check("writing through a reference binding", arr[2] == 90);
    check("members, initialized in the unit's order", px == 1 && py == 2);
    check("tuple-like parts", ta == 30 && tb == 12);
    check("a class with a destructor", nv == 5 && nw == 6);
    check("in a namespace", ns::u == 100 && ns::v == 200);
    auto f = [] { return px + ta; };
    check("seen from a lambda without a capture", f() == 31);
    int first = once();
    int second = once();
    check("a static binding is initialized once", first == 3 && second == 3 &&
          counter == 3);
    DD d1;
    d1.p = 4;
    d1.q = 6;
    auto &[lp, lq] = d1;
    lp = 40;
    auto [cp, cq] = d1;
    check("a base's members, two levels down", d1.p == 40 && lq == 6 &&
          cp == 40 && cq == 6 && d1.sum() == 46);
    check("a reference's temporary outlives start-up",
          gr == 77 && gn.v == 1 && gn.w == 2);
    check("... and a static reference's, made once",
          ref_static(5) == 15 && ref_static(9) == 15);
    check("no destructor ran before exit", trail[0] == 0);
    return fails == 0 ? 42 : 1;
}
