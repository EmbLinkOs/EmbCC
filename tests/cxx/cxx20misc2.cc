// CX7/CX8, the rules <format> and <chrono> needed: a class whose only
// bases are empty laid out as the plain struct (bit-fields and all);
// partial specializations matched with defaulted trailing arguments and
// through template template parameters (is_specialization_of); a
// parameter only in non-deduced contexts (type_identity_t, format_string)
// leaves the argument to its conversion; decltype(e)::type; switch on a
// scoped enum; __remove_cv of an array; a block's using-declaration keeps
// argument-dependent lookup; && member functions called on an rvalue of a
// derived class; virtual functions of a constructed instance defined for
// its vtable; __builtin_alloca.
// expect-exit: 42
#include <stdio.h>

static int fails;
static void check(const char *what, bool ok)
{
    if (!ok)
        fails++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

template <class T> struct type_identity { using type = T; };
template <class T> using type_identity_t = typename type_identity<T>::type;
template <class A, class B> struct same { static const bool value = false; };
template <class A> struct same<A, A> { static const bool value = true; };

// ---- empty bases and bit-fields ----
struct Empty {};
struct Spec : Empty {
    unsigned short width;
    unsigned align : 2;
    unsigned sign : 2;
    unsigned alt : 1;
    unsigned type : 4;
};

// ---- partial specializations ----
template <class A, class B, class = void> struct Ref { static const int v = 0; };
template <class A, class B> struct Ref<A &, B &&> { static const int v = 1; };
template <class T, template <class...> class C> constexpr bool is_spec = false;
template <template <class...> class C, class... A> constexpr bool is_spec<C<A...>, C> = true;
template <class T, class U = int> struct Two {};
template <class T> struct One {};

// ---- non-deduced contexts ----
template <class... A> struct FmtStr {
    const char *s;
    FmtStr(const char *p) : s(p) {}
};
template <class... A> using fmt_string = FmtStr<type_identity_t<A>...>;
template <class... A> int count_args(fmt_string<A...> f, A &&...) { return f.s[0] + (int)sizeof...(A); }
template <class C> int width_of(type_identity_t<C> c) { return (int)sizeof(c); }

// ---- decltype(e)::type ----
template <class T> type_identity<T> wrap(T);
template <class T> using Unwrap = typename decltype(wrap(T()))::type;

// ---- switch on a scoped enum ----
enum class Kind : unsigned char { none, integer, text };
static int kind_value(Kind k)
{
    switch (k) {
    case Kind::none: return 1;
    case Kind::integer: return 2;
    case Kind::text: return 3;
    }
    return 0;
}

// ---- using-declaration and ADL ----
namespace adl_only { void make(); }
namespace lib {
    enum class errc { bad = 7 };
    int make(errc e) { return (int)e * 6; }
}
static int via_adl(lib::errc e)
{
    using adl_only::make;
    return make(e);
}

// ---- && members through a base ----
template <class T> T &&mv(T &x) { return static_cast<T &&>(x); }
struct Sink { int v = 40; int get() && { return v + 2; } int get() const & { return v; } };
struct StrSink : Sink {};

// ---- virtual functions of an instance ----
template <class T> struct Scanner {
    virtual ~Scanner() {}
    virtual T on_chars(T x) { return x + 1; }
    T run(T x) { return on_chars(x); }
};
template <class T> struct Checking : Scanner<T> {
    T on_chars(T x) override { return x * 2; }
};

static int alloca_sum(int n)
{
    char *p = (char *)__builtin_alloca(n);
    for (int i = 0; i < n; i++)
        p[i] = 2;
    int s = 0;
    for (int i = 0; i < n; i++)
        s += p[i];
    return s;
}

int main()
{
    Spec sp{};
    sp.width = 9;
    sp.align = 3;
    sp.type = 10;
    check("empty base, bit-fields", sizeof(Spec) == 4 && sp.align == 3 && sp.type == 10 &&
                                    sp.width == 9 && sp.sign == 0);
    check("partial specializations", Ref<int &, char &&>::v == 1 && Ref<int, int>::v == 0 &&
                                     is_spec<One<int>, One> && is_spec<Two<char>, Two> &&
                                     !is_spec<One<int>, Two>);
    check("non-deduced contexts", count_args("x", 1, 2) == 'x' + 2 && width_of<long>(1) == 8);
    check("decltype(e)::type", same<Unwrap<char>, char>::value);
    check("switch on a scoped enum", kind_value(Kind::text) == 3 && kind_value(Kind::none) == 1);
    check("__remove_cv of an array", same<__remove_cv(const char[3]), char[3]>::value);
    check("using-declaration keeps ADL", via_adl(lib::errc::bad) == 42);
    StrSink ss;
    check("&& member of a base", mv(ss).get() == 42 && ss.get() == 40);
    Checking<int> ck;
    Scanner<int> *sc = &ck;
    check("virtual functions of an instance", sc->run(21) == 42);
    check("__builtin_alloca", alloca_sum(21) == 42);

    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 42;
}
