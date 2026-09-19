// CX4: variadic templates — type, value and function parameter packs;
// sizeof...; expansions in template arguments, call arguments, braced
// lists, parameter lists, base lists and mem-initializers; packs deduced
// from calls and from partial specialization patterns (tuple<T, Rest...>);
// fold expressions (unary and binary, left and right, empty); forwarding;
// an index sequence driving an apply.
// expect-exit: 42
#include <stdio.h>
#include <string.h>

static int fails;
static void check(const char *what, bool ok)
{
    if (!ok)
        fails++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

// ---- sizeof... and folds ----
template <class... Ts> int count(Ts... xs)
{
    return sizeof...(Ts) * 10 + sizeof...(xs);
}
template <class... Ts> int sum_right(Ts... xs) { return (xs + ... + 0); }
template <class... Ts> int sum_left(Ts... xs) { return (0 + ... + xs); }
template <class... Ts> int sub_right(Ts... xs) { return (xs - ...); }
template <class... Ts> int sub_left(Ts... xs) { return (... - xs); }
template <class... Ts> bool all(Ts... xs) { return (xs && ...); }
template <class... Ts> bool any(Ts... xs) { return (... || xs); }

static char trail[64];
static void note(const char *s) { strcat(trail, s); }
template <class... Ts> void note_all(Ts... xs) { (note(xs), ...); }

// ---- recursion over a pack ----
int total() { return 0; }
template <class T, class... Rest> int total(T first, Rest... rest)
{
    return (int)first + total(rest...);
}

template <class... Ts> struct Count { static const int n = 0; };
template <class T, class... Rest> struct Count<T, Rest...> {
    static const int n = 1 + Count<Rest...>::n;
};
template <class... Ts> struct Shape { static const int v = 0; };
template <class T, class... Ts> struct Shape<T, Ts...> { static const int v = 1; };
template <class T> struct Shape<T> { static const int v = 2; };
template <class T> struct Shape<T, T> { static const int v = 3; };

// ---- expansions ----
int take3(int a, int b, int c) { return a * 100 + b * 10 + c; }
template <class... Ts> int pass(Ts... xs) { return take3(xs...); }
template <class... Ts> int doubled(Ts... xs) { return take3((xs * 2)...); }
int sq(int x) { return x * x; }
template <class... Ts> int squares(Ts... xs) { return take3(sq(xs)...); }
template <class... As, class... Bs> int zip(As... a) { return take3(a...); }
template <class... Ts> int sizes_sum()
{
    int s[] = { (int)sizeof(Ts)..., 0 };
    int r = 0;
    for (int i = 0; i < (int)sizeof...(Ts); i++)
        r += s[i];
    return r;
}
template <class... Ts> int weighted(const Ts &...xs)
{
    return ((xs * (int)sizeof(Ts)) + ... + 0);
}

// ---- forwarding ----
template <class T> struct remove_ref { typedef T type; };
template <class T> struct remove_ref<T &> { typedef T type; };
template <class T> struct remove_ref<T &&> { typedef T type; };
template <class T> T &&forward(typename remove_ref<T>::type &x)
{
    return static_cast<T &&>(x);
}

static char cats[16];
static void cat1(int &) { strcat(cats, "L"); }
static void cat1(int &&) { strcat(cats, "R"); }
template <class... Args> void categories(Args &&...args)
{
    (cat1(forward<Args>(args)), ...);
}

struct Pt {
    int x, y;
    Pt(int a, int b) : x(a), y(b) { }
};
template <class T, class... Args> T make(Args &&...args)
{
    return T(forward<Args>(args)...);
}

// ---- tuple by partial specialization ----
template <class... Ts> struct tuple;
template <> struct tuple<> { };
template <class T, class... Rest> struct tuple<T, Rest...> : tuple<Rest...> {
    T head;
    tuple(const T &h, const Rest &...r) : tuple<Rest...>(r...), head(h) { }
};

template <int N, class Tup> struct getter;
template <class T, class... Rest> struct getter<0, tuple<T, Rest...>> {
    typedef T type;
    static T &get(tuple<T, Rest...> &t) { return t.head; }
};
template <int N, class T, class... Rest> struct getter<N, tuple<T, Rest...>> {
    typedef typename getter<N - 1, tuple<Rest...>>::type type;
    static type &get(tuple<T, Rest...> &t)
    {
        return getter<N - 1, tuple<Rest...>>::get(t);
    }
};
template <int N, class... Ts>
typename getter<N, tuple<Ts...>>::type &get(tuple<Ts...> &t)
{
    return getter<N, tuple<Ts...>>::get(t);
}
template <class... Ts> tuple<Ts...> make_tuple(const Ts &...xs)
{
    return tuple<Ts...>(xs...);
}
template <class... Ts> int arity(const tuple<Ts...> &) { return sizeof...(Ts); }

// ---- index sequences ----
template <int... Is> struct seq { };
template <int N, int... Is> struct make_seq : make_seq<N - 1, N - 1, Is...> { };
template <int... Is> struct make_seq<0, Is...> { typedef seq<Is...> type; };

long mix(int a, long b, short c) { return a + b * 10 + c * 100; }
template <class Tup, int... Is> long apply_impl(Tup &t, seq<Is...>)
{
    return mix(get<Is>(t)...);
}
template <class... Ts> long apply(tuple<Ts...> &t)
{
    return apply_impl(t, typename make_seq<sizeof...(Ts)>::type());
}
template <int... Is> int seq_sum(seq<Is...>) { return (Is + ... + 0); }

// ---- packs of bases ----
struct A { int a; A(int x) : a(x) { } int fa() { return a; } };
struct B { int b; B(int x) : b(x) { } int fb() { return b; } };
template <class... Bases> struct Multi : Bases... {
    Multi(const Bases &...bs) : Bases(bs)... { }
};

int main()
{
    check("sizeof... of types and parameters", count(1, 'c', 2.5) == 33 &&
                                                count() == 0);
    check("binary folds", sum_right(1, 2, 3, 4) == 10 && sum_left() == 0);
    check("unary folds: right and left", sub_right(10, 4, 3) == 9 &&
                                         sub_left(10, 4, 3) == 3);
    check("empty && and || folds", all() && !any() && all(1, 2) &&
                                   !all(1, 0) && any(0, 3));
    note_all("a", "b", "c");
    check("comma fold, in order", strcmp(trail, "abc") == 0);

    check("recursion over a pack", total(1, 2L, (short)3, 'd') == 106);
    check("recursion in class templates", Count<int, char, long>::n == 3 &&
                                          Count<>::n == 0);

    check("partial specializations with packs ordered",
          Shape<>::v == 0 && Shape<int>::v == 2 && Shape<int, char>::v == 1 &&
          Shape<int, int>::v == 3 && Shape<int, int, int>::v == 1);

    check("expanded arguments", pass(1, 2, 3) == 123);
    check("patterns expanded", doubled(1, 2, 3) == 246 &&
                               squares(1, 2, 3) == 149);
    check("a pack given explicitly", zip<int, int, int>(3, 2, 1) == 321);
    check("expanded in a braced list",
          sizes_sum<char, short, int, long>() == 1 + 2 + 4 + 8);
    check("types and parameters expanded together",
          weighted(1, (short)2, 3L) == 1 * 4 + 2 * 2 + 3 * 8);

    int i = 0;
    categories(i, 5, i, static_cast<int &&>(i));
    check("forwarding keeps value categories", strcmp(cats, "LRLR") == 0);
    Pt p = make<Pt>(3, i);
    check("forwarded to a constructor", p.x == 3 && p.y == 0);

    tuple<int, long, short> t(1, 20L, (short)300);
    check("tuple get", get<0>(t) == 1 && get<1>(t) == 20 && get<2>(t) == 300);
    get<1>(t) = 40;
    check("tuple get as an lvalue", get<1>(t) == 40);
    const char *seven = "seven";
    tuple<int, const char *> u = make_tuple(7, seven);
    check("make_tuple deduces", get<0>(u) == 7 &&
                                strcmp(get<1>(u), "seven") == 0 &&
                                arity(u) == 2 && arity(t) == 3);
    check("apply through an index sequence", apply(t) == 1 + 400 + 30000);
    check("value packs deduced", seq_sum(seq<1, 2, 3, 4>()) == 10 &&
                                 seq_sum(make_seq<5>::type()) == 10);

    Multi<A, B> m(A(5), B(6));
    check("bases and mem-initializers expanded", m.fa() == 5 && m.fb() == 6);

    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 42;
}
