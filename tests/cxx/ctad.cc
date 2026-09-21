// CX7 (C++17/20): class template argument deduction — from constructors
// (the implicit guides, constructor templates included), from deduction
// guides (template and not), the copy deduction candidate, braced lists
// (std::initializer_list constructors first), aggregates (C++20), and in
// expressions (C(args), C{args}); explicit guides and constructors not
// used by copy-initialization. And inheriting constructors (using B::B),
// through two levels and with constructor templates.
// expect-exit: 42
#include <stdio.h>

namespace std {
template <class E> class initializer_list {
    const E *b;
    unsigned long n;
public:
    constexpr initializer_list() : b(0), n(0) {}
    constexpr unsigned long size() const { return n; }
    constexpr const E *begin() const { return b; }
    constexpr const E *end() const { return b + n; }
};
}

static int fails;
static void check(const char *what, bool ok)
{
    if (!ok)
        fails++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

template <class A, class B> struct same { static const bool value = false; };
template <class A> struct same<A, A> { static const bool value = true; };

template <class T, class U> struct Pair {
    T first;
    U second;
    Pair(T a, U b) : first(a), second(b) {}
};

template <class T> struct Box {
    T v;
    Box(T x) : v(x) {}
};

template <class T> struct Vec {
    int n = 0;
    T sum{};
    Vec(std::initializer_list<T> l) { for (T x : l) { sum += x; n++; } }
    Vec(int count, T fill) : n(count), sum(fill * count) {}
};

template <class T> struct Agg { T x, y; };           // no constructors

template <class T> struct Holder {
    T value;
    template <class It> Holder(It first, It last) : value(*first + *last) {}
};
template <class It> Holder(It, It) -> Holder<long>;  // an explicit guide

template <class T> struct Name { const char *s; T tag; Name(const char *p, T t) : s(p), tag(t) {} };
Name(const char *) -> Name<int>;                      // a guide, no template
template <class T> struct Name1 { T t; };

template <class T> struct Strict {
    T v;
    explicit Strict(T x) : v(x) {}
};

// ---- inheriting constructors ----
struct Base {
    int v;
    Base(int x) : v(x) {}
    Base(int x, int y) : v(x * y) {}
    template <class T> Base(T *p) : v((int)sizeof(*p)) {}
};
struct Derived : Base {
    using Base::Base;
    int extra = 5;
};
struct Twice : Derived {
    using Derived::Derived;
};
template <class T> struct TBase {
    T v;
    template <class... A> TBase(int, A... a) : v((T)sizeof...(a)) {}
};
template <class T> struct TDerived : TBase<T> {
    using TBase<T>::TBase;
};
struct Own : Base {
    using Base::Base;
    Own(int x) : Base(x + 100) {}      // hides Base(int)
};

int main()
{
    Pair p(1, 2.5);
    check("from a constructor", same<decltype(p), Pair<int, double>>::value &&
                                p.first == 1 && p.second == 2.5);
    Box b = 7;
    Box c{b};
    Box d(b);
    check("copy deduction", same<decltype(c), Box<int>>::value &&
                            same<decltype(d), Box<int>>::value && d.v == 7);
    Vec v{1, 2, 3};
    Vec w(4, 1.5);
    check("braced list: initializer_list first", same<decltype(v), Vec<int>>::value &&
                                                 v.n == 3 && v.sum == 6);
    check("parentheses: the other constructors", same<decltype(w), Vec<double>>::value &&
                                                 w.n == 4 && w.sum == 6.0);
    Agg ag{3, 4};
    check("an aggregate (C++20)", same<decltype(ag), Agg<int>>::value && ag.x + ag.y == 7);
    int arr[2] = { 10, 20 };
    Holder h(arr, arr + 1);
    check("a deduction guide", same<decltype(h), Holder<long>>::value && h.value == 30);
    Name n("x", 'c');
    check("a constructor beside guides", same<decltype(n), Name<char>>::value);
    auto e = Pair(3, 'z');
    auto f = Box{2.0};
    check("in expressions", same<decltype(e), Pair<int, char>>::value &&
                            same<decltype(f), Box<double>>::value && e.second == 'z');
    Strict s(5);
    check("an explicit constructor, direct-initialization", s.v == 5);

    Derived d1(3), d2(4, 5);
    double dd;
    Derived d3(&dd);
    check("inherited constructors", d1.v == 3 && d2.v == 20 && d3.v == 8 &&
                                    d1.extra == 5);
    Twice t2(6, 7);
    check("inherited twice", t2.v == 42);
    TDerived<long> td(0, 1, 2);
    check("inherited template constructors", td.v == 2);
    Own o1(1), o2(2, 3);
    check("a declared constructor hides the inherited one", o1.v == 101 && o2.v == 6);

    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 42;
}
