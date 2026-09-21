// CX8: template rules std::map, std::tuple and std::apply depend on —
// a value template parameter whose type is dependent (`typename
// enable_if<C, bool>::type = true`) substituted in deduction, a failure
// removing the candidate; deduction through alias templates (an alias is
// the type it names: index_sequence<I...> is integer_sequence<size_t,
// I...>); a base class template deduced from a class with several
// instances of it among its bases (tuple's _Tuple_impl<I, ...>); member
// templates of class templates defined outside the class; out-of-class
// destructors of class templates; pack expansions in a variable's
// parenthesized initializer; __make_integer_seq; a call's explicit
// template arguments not leaking into the calls resolved while deducing
// it; conversions to an instance not yet instantiated; a defaulted move
// constructor first needed in an unevaluated operand; the out-of-class
// static member of a template not defined for an explicit
// specialization.
// expect-exit: 42
#include <stdio.h>

static int fails;
static void check(const char *what, bool ok)
{
    if (!ok)
        fails++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

typedef unsigned long size_t;
template <bool B, class T = void> struct enable_if {};
template <class T> struct enable_if<true, T> { typedef T type; };
template <class T, T V> struct constant { static constexpr T value = V; };
template <class A, class B> struct same : constant<bool, false> {};
template <class A> struct same<A, A> : constant<bool, true> {};

// ---- dependent value-parameter types ----
template <class U, typename enable_if<sizeof(U) == 1, bool>::type = true>
int width(U &&) { return 1; }
template <class U, typename enable_if<sizeof(U) != 1, bool>::type = false>
int width(U &&) { return 8; }

template <class T1, class T2> struct Pair {
    T1 a; T2 b; int how;
    template <class U1, class U2,
              typename enable_if<sizeof(U1) <= 4, bool>::type = true>
    Pair(U1 &&x, U2 &&y) : a(x), b(y), how(1) {}
    template <class U1, class U2,
              typename enable_if<!(sizeof(U1) <= 4), bool>::type = false>
    explicit Pair(U1 &&x, U2 &&y) : a(x), b(y), how(2) {}
};
static Pair<bool, long> make_pair_b(long n) { return { true, n }; }

// ---- aliases ----
template <class T, T... I> struct seq { static constexpr size_t size = sizeof...(I); };
template <size_t... I> using index_seq = seq<size_t, I...>;
template <class T> using ptr = T *;
template <class T> using twice = Pair<T, T>;
#if __has_builtin(__make_integer_seq)
template <size_t N> using make_index = __make_integer_seq<seq, size_t, N>;
#else
template <size_t N> using make_index = seq<size_t, __integer_pack(N)...>;  // g++
#endif

template <class T> size_t pointee(ptr<T>) { return sizeof(T); }
template <size_t... I> size_t count(index_seq<I...>) { return sizeof...(I); }
template <size_t... I> size_t total(index_seq<I...>) { return (0 + ... + I); }
template <class T> int both(const twice<T> &p) { return p.a + p.b; }

// ---- several base instances ----
template <size_t I, class... T> struct Impl;
template <size_t I, class H, class... T>
struct Impl<I, H, T...> : Impl<I + 1, T...> { H head; };
template <size_t I> struct Impl<I> {};
template <class... T> struct Tup : Impl<0, T...> {};
template <size_t I, class H, class... T> H &get(Impl<I, H, T...> &t) { return t.head; }

// ---- member templates defined outside ----
static int sum() { return 0; }
template <class H, class... R> int sum(H h, R... r) { return h + sum(r...); }
static int dtors;
template <class T> struct Box {
    T v;
    struct Node {
        int total;
        template <class... A> Node(Box &, A &&...a) : total(sum(a...)) {}
    };
    Box();
    ~Box();
    template <class U> int with(U);
    template <class... A> int emplace(A &&...);
    template <class... A> auto later(A &&...) -> int;
};
template <class T> Box<T>::Box() : v(1) {}
template <class T> Box<T>::~Box() { dtors += v; }
template <class T> template <class U> int Box<T>::with(U u) { return v + (int)sizeof(u); }
template <class T> template <class... A> int Box<T>::emplace(A &&...a)
{
    Node n(*this, static_cast<A &&>(a)...);   // a pack in ( )
    return n.total;
}
template <class T> template <class... A> auto Box<T>::later(A &&...a) -> int
{
    return (int)sizeof...(a) * 10;
}

// ---- explicit arguments stay with their call ----
template <class T> struct is_copyable { static constexpr bool value = __is_constructible(T, const T &); };
template <class... B> auto all_fn(int) -> Pair<int, typename enable_if<(B::value && ...), int>::type>;
template <class... B> auto all_fn(...) -> Pair<int, long>;
struct Heavy { Heavy() {} Heavy(const Heavy &) {} };

// ---- a conversion to an instance named, not yet instantiated ----
template <class T> struct Iter { T *p; };
template <class T> struct CIter { const T *p; CIter(Iter<T> i) : p(i.p) {} };
static int deref(CIter<int> c) { return *c.p; }

// ---- static members and explicit specializations ----
template <class C> struct Facet { static int id; };
template <class C> int Facet<C>::id = 7;
template <> struct Facet<char> { static int id; };
int Facet<char>::id = 30;

int main()
{
    long n = 5;
    check("dependent value-parameter type", width('c') == 1 && width(n) == 8);
    Pair<bool, long> p = make_pair_b(n);
    Pair<long, long> q(n, n);
    check("enable_if'd constructors", p.how == 1 && p.b == 5 && q.how == 2);

    double d = 0;
    check("through ptr<T>", pointee(&d) == sizeof(double));
    check("through index_seq<I...>",
          count(seq<size_t, 4, 5, 6>{}) == 3 && total(index_seq<1, 2, 3>{}) == 6);
    check("through twice<T>", both(Pair<int, int>(20, 22)) == 42);
    check("__make_integer_seq", same<make_index<3>, seq<size_t, 0, 1, 2>>::value &&
                                total(make_index<5>{}) == 10 && make_index<0>::size == 0);

    Tup<int, char, long> t;
    get<0>(t) = 40;
    get<1>(t) = 'x';
    get<2>(t) = 2;
    check("each base instance tried", get<0>(t) + get<2>(t) == 42 && get<1>(t) == 'x');

    {
        Box<int> b;
        int x = 2;
        check("member templates outside", b.with(1.0) == 9 && b.emplace(10, x, 30) == 42 &&
                                          b.later(1, 2) == 20);
    }
    check("destructor outside", dtors == 1);

    check("explicit arguments stay with their call",
          same<decltype(all_fn<is_copyable<Heavy>, is_copyable<int>>(0)), Pair<int, int>>::value);

    int k = 42;
    Iter<int> it = { &k };
    check("conversion to a fresh instance", deref(it) == 42);

    check("explicit specialization's own static", Facet<char>::id + Facet<int>::id == 37);

    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 42;
}
