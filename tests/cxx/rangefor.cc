// CX6: range-based for (over arrays, over classes with begin()/end()
// members, over begin()/end() found by argument-dependent lookup, over a
// temporary — kept alive for the loop —, by value, by reference, with
// break and continue) and deduced return types (auto, auto &, const
// auto &, several returns, void, members used before they are read,
// function templates).
// expect-exit: 42
#include <stdio.h>

static int fails;
static void check(const char *what, bool ok)
{
    if (!ok)
        fails++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

// ---- range-for ----
struct Vec {
    int d[5] = { 1, 2, 3, 4, 5 };
    int n = 5;
    int *begin() { return d; }
    int *end() { return d + n; }
};
namespace ns {
struct Bag { int a[3] = { 10, 20, 30 }; };
struct It {
    int *p;
    int &operator*() const { return *p; }
    It &operator++() { ++p; return *this; }
    bool operator!=(const It &o) const { return p != o.p; }
};
It begin(Bag &b) { return It{ b.a }; }
It end(Bag &b) { return It{ b.a + 3 }; }
}
static int copies;
struct Noisy {
    int v;
    Noisy(int x) : v(x) {}
    Noisy(const Noisy &o) : v(o.v) { copies++; }
};
struct Holder {
    Noisy xs[2] = { Noisy(7), Noisy(8) };
    Noisy *begin() { return xs; }
    Noisy *end() { return xs + 2; }
};
static int alive;
struct Temp {
    int d[3] = { 1, 2, 3 };
    Temp() { alive++; }
    ~Temp() { alive--; }
    int *begin() { return d; }
    int *end() { return d + 3; }
};

// ---- deduced return types ----
auto twice(int x) { return x * 2; }
auto half(double d)
{
    if (d < 0)
        return 0.0;
    return d / 2;
}
static int touched;
auto nothing() { touched = 1; }
static int g = 5;
auto &ref() { return g; }
const auto &cref() { return g; }
struct S {
    int v = 3;
    auto later() const { return 10; }
    auto get() const { return later() + v; }   // later: read on first use
    auto self() -> S & { return *this; }
};
template <class T> auto sum(T a, T b) { return a + b; }
template <class T> auto first(T *p) { return *p; }

int main()
{
    int arr[] = { 3, 4, 5 };
    int s = 0;
    for (int x : arr)
        s += x;
    check("over an array", s == 12);
    Vec v;
    for (int &x : v)
        x *= 2;
    s = 0;
    for (auto x : v)
        s += x;
    check("members begin/end, by reference", s == 30 && v.d[4] == 10);
    ns::Bag bag;
    s = 0;
    for (const auto &x : bag)
        s += x;
    check("begin/end by ADL", s == 60);
    s = 0;
    for (int x : Vec()) {
        if (x == 3)
            continue;
        if (x == 5)
            break;
        s += x * 100;
    }
    check("break and continue", s == 700);
    s = 0;
    for (int x : Temp())
        s += x * alive;
    check("a temporary lives through the loop", s == 6 && alive == 0);
    Holder h;
    s = 0;
    for (Noisy n : h)
        s += n.v;
    check("by value copies", s == 15 && copies == 2);
    for (const Noisy &n : h)
        s += n.v;
    check("by reference does not", s == 30 && copies == 2);

    check("auto", twice(4) == 8 && sizeof(twice(1)) == sizeof(int));
    check("several returns", half(3) == 1.5 && half(-1) == 0.0);
    nothing();
    check("void", touched == 1);
    ref() = 7;
    check("auto &", g == 7 && &ref() == &g && cref() == 7);
    S st;
    check("members", st.get() == 13 && &st.self() == &st);
    int two[2] = { 9, 8 };
    check("templates", sum(1, 2) == 3 && sum(1.5, 2.0) == 3.5 && first(two) == 9);

    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 42;
}
