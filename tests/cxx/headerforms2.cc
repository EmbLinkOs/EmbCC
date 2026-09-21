// CX8: more forms from libstdc++'s headers — anonymous unions in classes
// (members named through them, offsetof), a member class of a class
// template defined outside it, out-of-class member templates of a class
// (constructors too), pseudo-destructor calls, the overflow builtins,
// statement attributes, a partial specialization matched through
// void_t (substituted to check), static_assert on a class with operator
// bool, value-initialized pointer members, template-ids in elaborated
// specifiers, copy-initialization through a conversion function, a
// functional cast opening a condition, GNU atomics.
// expect-exit: 42
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static int fails;
static void check(const char *what, bool ok)
{
    if (!ok)
        fails++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

struct Str {
    char *p;
    unsigned long len;
    union {
        char buf[16];
        unsigned long cap;
    };
    Str(const char *s) : p(buf), len(strlen(s)) { memcpy(buf, s, len + 1); }
};
struct Deep {
    int tag;
    union {
        struct { short lo, hi; };
        int whole;
    };
};

template <class C> class os {
public:
    class sentry;
    int v = 3;
    int use();
};
template <class C> class os<C>::sentry {
    os &o;
public:
    explicit sentry(os &x) : o(x) {}
    int val() const { return o.v * (int)sizeof(C); }
};
template <class C> int os<C>::use() { sentry s(*this); return s.val(); }

struct loc {
    int n = 0;
    template <class F> loc(const loc &other, F *f);
    loc() = default;
};
template <class F> loc::loc(const loc &other, F *f) : n(other.n + *f) {}

template <class T> void destroy(T *p) { p->~T(); }

template <class...> using void_t = void;
template <class T, class = void_t<>> struct has_type { static constexpr bool v = false; };
template <class T> struct has_type<T, void_t<typename T::type>> { static constexpr bool v = true; };
struct WithType { using type = int; };

struct yes { constexpr operator bool() const { return true; } };
constexpr yes probe() { return {}; }
static_assert(probe(), "a class with operator bool");

struct Ptrs {
    int *a, *b;
    Ptrs() : a(), b() {}
};

template <class T> struct holder { friend class holder<T *>; int v = 5; };

struct view { const char *p; };
struct text {
    const char *s;
    operator view() const { return view{ s }; }
};
static int length(view v) { return (int)strlen(v.p); }

int main()
{
    Str s("hi");
    Deep d;
    d.whole = 0;
    d.lo = 1;
    d.hi = 2;
    check("anonymous unions", strcmp(s.buf, "hi") == 0 && s.p == s.buf &&
                              sizeof(Str) == 32 && offsetof(Str, cap) == 16 &&
                              d.whole == 0x20001);
    os<int> o;
    check("a member class defined outside its template", o.use() == 12);
    loc base;
    base.n = 4;
    int add = 3;
    loc l(base, &add);
    check("an out-of-class constructor template", l.n == 7);
    int x = 5;
    destroy(&x);
    check("pseudo-destructors", x == 5);
    unsigned long big = 1UL << 63, r;
    long sr;
    int ir;
    check("overflow builtins",
          __builtin_mul_overflow(big, 2UL, &r) && !__builtin_add_overflow(1UL, 2UL, &r) &&
          r == 3 && __builtin_add_overflow(0x7fffffffffffffffL, 1L, &sr) &&
          __builtin_mul_overflow(65536, 65536, &ir) && !__builtin_sub_overflow(5, 3, &ir) &&
          ir == 2);
    int n = 0;
    for (int i = 0; i < 3; i++)
        if (i > 0) [[likely]] { n += i; }
    check("statement attributes", n == 3);
    check("void_t partial specializations", has_type<WithType>::v && !has_type<int>::v);
    Ptrs ps;
    check("value-initialized pointers", !ps.a && !ps.b);
    holder<int> h;
    check("a friend template-id", h.v == 5);
    text t{ "four" };
    check("copy-initialization through a conversion function", length(t) == 4);
    int w = 7;
    if (wchar_t('0') == L'0' && w > 0)
        w++;
    check("a functional cast opening a condition", w == 8);
    long counter = 1;
    __atomic_fetch_add(&counter, 2, __ATOMIC_ACQ_REL);
    check("GNU atomics", __atomic_load_n(&counter, __ATOMIC_ACQUIRE) == 3);
    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 42;
}
