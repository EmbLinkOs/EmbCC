// CX7/CX8: designated initializers; using enum; mem-initializers of an
// anonymous union's members; member classes of a template's instance
// defined only when needed (a holder of an incomplete T); an explicit
// specialization's own static member; using-declarations with a pack
// expansion; partial ordering (a parameter pack is less specialized, and
// deduction must agree across parameters); a qualified friend template;
// a fold whose operand names a type pack in template arguments; labels
// followed by [[fallthrough]]; members of a member class template and of a
// partial specialization defined outside; operator() inherited; a class
// constant as a bool template argument; explicit conversion functions in
// direct-initialization; `<` after a value in a default template
// argument.
// expect-exit: 42
#include <stdio.h>

static int fails;
static void check(const char *what, bool ok)
{
    if (!ok)
        fails++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

// ---- designated initializers ----
struct Point { int x = 1, y = 2, z = 3; };
struct Conf { const char *name; int level; double ratio; Point at; };
static int sum(Point p) { return p.x + p.y + p.z; }

// ---- using enum ----
enum class Color { red = 1, green = 2, blue = 4 };
struct Paint { using enum Color; static int all() { return (int)red + (int)green + (int)blue; } };

// ---- anonymous union members ----
struct Str { int n; Str(int x) : n(x) {} Str(const Str &o) : n(o.n + 1) {} ~Str() {} };
struct Opt {
    Opt() : dummy(), has(false) {}
    Opt(const Str &s) : val(s), has(true) {}
    ~Opt() { if (has) val.~Str(); }
    int get() const { return has ? val.n : dummy; }
    union { char dummy = 0; Str val; };
    bool has;
};

// ---- lazy member classes ----
template <class T> struct Holder {
    struct Slot { T value; };                 // needs a complete T
    Slot *p = nullptr;
    int size() const { return p ? 1 : 0; }
};
struct Node { Holder<Node> children; int v; };   // Node is incomplete here

// ---- explicit specialization's static member ----
template <class C> struct Facet { static int id; };
template <class C> int Facet<C>::id = 7;
template <> int Facet<char>::id = 30;

// ---- using with a pack ----
template <int I, class T> struct F { static int fun(T) { return I; } };
template <class... Fs> struct All : Fs... { using Fs::fun...; };

// ---- partial ordering ----
template <class T> int pick(T &&) { return 1; }
template <class... T> int pick(T &&...) { return 2; }
template <class A, class B> int wrap(A, B) { return 1; }
template <class I> int wrap(const I &, I) { return 2; }

// ---- qualified friend template ----
namespace detail { template <int N, class V> int peek(V &&v) { return v.secret + N; } }
struct Vault {
    template <int N, class V> friend int detail::peek(V &&);
    int secret = 40;
};

// ---- fold over a type pack in template arguments ----
template <class T> struct Big { static constexpr bool value = sizeof(T) > 1; };
template <class... Ts> constexpr bool all_big() { return (Big<Ts>::value && ...); }

// ---- members defined outside ----
struct Outer {
    enum Op { get = 1, put = 2 };
    template <class T> struct M { static int run(Op o, T t); };
    int call() { return M<int>::run(put, 40); }
};
template <class T> int Outer::M<T>::run(Op o, T t) { return (int)o + t; }

template <class T, bool B> struct PS { int f(); };
template <class T> struct PS<T, true> { int f(); int g() { return 100; } };
template <class T> int PS<T, true>::f() { return (int)sizeof(T) + g(); }
template <class T, bool B> int PS<T, B>::f() { return -1; }

// ---- operator() from a base; conversions ----
struct HashBase { int operator()(int x) const { return x * 2; } };
struct Hash : HashBase {};
template <bool B> struct Flag { static constexpr bool value = B; };
struct Yes { constexpr operator bool() const { return true; } };
struct Iter { int v; };
struct Loc { int at; explicit operator Iter() const { return Iter{at}; } };
template <unsigned W, bool = W < 8 * sizeof(int)> struct Fits { static constexpr bool value = false; };
template <unsigned W> struct Fits<W, true> { static constexpr bool value = true; };

static int ends_with_label(int x)
{
    int r = 0;
    switch (x) {
    case 1:
        r = 1;
        [[fallthrough]];
    case 2:
        [[fallthrough]];
    default:
        r += 10;
    }
    return r;
}

int main()
{
    Point p{.x = 10, .z = 30};
    Conf c{.name = "n", .ratio = 0.5, .at{.y = 7}};
    check("designated initializers", p.x + p.y + p.z == 42 && c.level == 0 &&
                                     c.at.x == 1 && c.at.y == 7 && sum({.y = 20}) == 24);
    using enum Color;
    check("using enum", Paint::all() == 7 && (int)green == 2);
    Opt o1, o2(Str(40));
    check("anonymous union mem-initializers", o1.get() == 0 && o2.get() == 41);
    Node n{{}, 5};
    check("a member class defined when needed", n.children.size() == 0 && n.v == 5);
    check("an explicit specialization's static", Facet<char>::id + Facet<int>::id == 37);
    using A = All<F<1, int>, F<2, double>>;
    check("using Fs::fun...", A::fun(1) + A::fun(2.0) * 10 == 21);
    int i = 0, *ip = &i;
    check("partial ordering", pick(1) == 1 && pick(1, 2) == 2 && wrap(ip, ip) == 2 &&
                              wrap(ip, (const int *)ip) == 1);
    Vault v;
    check("a qualified friend template", detail::peek<2>(v) == 42);
    check("fold over template arguments", all_big<int, long>() && !all_big<int, char>());
    check("labels before [[fallthrough]]", ends_with_label(1) == 11 && ends_with_label(2) == 10);
    Outer out;
    PS<long, true> ps;
    PS<int, false> pf;
    check("members defined outside", out.call() == 42 && ps.f() == 108 && pf.f() == -1);
    check("operator() of a base", Hash{}(21) == 42);
    check("a class constant as a bool argument", Flag<Yes{}>::value);
    Loc loc{9};
    check("explicit conversion, direct-initialization", Iter(loc).v == 9);
    check("< in a default template argument", Fits<8>::value && !Fits<64>::value);

    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 42;
}
