// CX8, what compiling libstdc++'s own sources needed: explicit
// instantiation of an instance's static data members, destructor and
// nested members (and GNU's `inline template class`); default arguments
// read with the class complete; a constexpr local read in a lambda without
// a capture; variable-length arrays; `= default` after the class; an
// inherited default constructor; classes and functions of an inline
// namespace defined through the enclosing one, and overloads there one
// set; pointers to inherited members; B* over void*; p->~X<T>(); a
// parenthesized dependent value as a template argument; raw strings and
// #elifdef; __builtin_powi; GNU's member-function-pointer conversions;
// weak references; asm statements.
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

// ---- explicit instantiation ----
template <class T> struct Rep {
    static T storage[4];
    static int count;
    T get(int i) const { return storage[i]; }
    ~Rep() { count++; }
    struct Inner { T v; Inner(T x) : v(x) {} T twice() const; };
};
template <class T> T Rep<T>::storage[4] = { 1, 2, 3, 4 };
template <class T> int Rep<T>::count = 0;
template <class T> T Rep<T>::Inner::twice() const { return v * 2; }
extern template long Rep<long>::storage[4];     // (another unit's)
template long Rep<long>::storage[4];            // ... no: this one's
template Rep<int>::~Rep();
template int Rep<int>::Inner::twice() const;
template struct Rep<short>;

template <class T> struct Poly { virtual ~Poly() {} virtual T id() const { return 7; } };
inline template struct Poly<int>;               // its vtable, not members

// ---- default arguments: the class complete ----
struct Path {
    int v;
    Path(int x = limit + 1) : v(x) {}
    int rep(const Path &p = Path()) const { return v * 100 + p.v; }
    static const int limit = 8;
};

// ---- inherited default constructors ----
struct Impl { int p; Impl() : p(5) {} Impl(int x) : p(x) {} };
struct Data : Impl { using Impl::Impl; Data(Data &&) = default; };

// ---- = default after the class ----
static int killed;
struct Mem { ~Mem() { killed++; } };
struct Holder { Mem m; int n; Holder(); Holder &operator=(const Holder &); ~Holder(); };
Holder::Holder() = default;
Holder &Holder::operator=(const Holder &) = default;
Holder::~Holder() = default;

// ---- inline namespaces, from outside ----
namespace lib {
inline namespace v1 {
struct Dir;
int hash(int);
int over(int) { return 1; }
}
int over(long) { return 2; }
}
struct lib::Dir { int fd = 3; };
int lib::hash(int x) { return x * 31; }

// ---- pointers to inherited members ----
struct Base { int field = 4; int fn(int x) const { return x + field; } };
struct Derived : Base {};

// ---- B* over void* ----
static int pick(const void *) { return 1; }
static int pick(Base *) { return 2; }

// ---- ~X<T>() ----
template <class T> struct Buf { T x; static int gone; ~Buf() { gone++; } };
template <class T> int Buf<T>::gone = 0;

// ---- a parenthesized dependent value ----
template <bool, class T> struct en { };
template <class T> struct en<true, T> { typedef T type; };
template <class A, class B> struct same { static const bool value = false; };
template <class A> struct same<A, A> { static const bool value = true; };
template <class T> struct Cont { typedef T *pointer; };
template <class It, class C> struct Iter {
    It cur;
    Iter(It p) : cur(p) {}
    template <class I2>
    Iter(const Iter<I2, typename en<(same<I2, typename C::pointer>::value),
                                    C>::type> &o) : cur(o.cur) {}
};

// ---- GNU: a bound member function's address ----
struct Facet { virtual int get() const { return 1; } virtual ~Facet() {} };
struct Custom : Facet { int get() const override { return 2; } };
static bool overridden(const Facet *f)
{
    return (void *)(f->*(&Facet::get)) != (void *)(&Facet::get);
}

// ---- weak references ----
extern "C" int no_such_function(int) __attribute__((weak));

// ---- raw strings and #elifdef ----
static const char raw[] = R"x(a\n"b"
#define RAW_LEAK 1
)x";
#ifdef RAW_LEAK
#error a directive inside a raw string was obeyed
#endif
#if 0
#elifdef __cplusplus
static const int elifdef_seen = 1;
#else
static const int elifdef_seen = 0;
#endif
// (C++23's, and GNU's before it — not strict C++20's, as g++ reads it)
#if defined __STRICT_ANSI__ && __cplusplus <= 202002L
static const int elifdef_ok = !elifdef_seen;
#else
static const int elifdef_ok = elifdef_seen;
#endif

int main()
{
    check("an instance's static array, explicitly", Rep<long>::storage[3] == 4);
    {
        Rep<int> r;
        check("an explicitly instantiated destructor", r.get(1) == 2);
    }
    check("... it ran", Rep<int>::count == 1);
    check("a nested class's member", Rep<int>::Inner(21).twice() == 42);
    check("a class instantiated whole", Rep<short>().get(0) == 1);
    Poly<int> *pp = new Poly<int>;
    check("inline template class: the vtable", pp->id() == 7);
    delete pp;

    check("default arguments read with the class complete",
          Path(2).rep() == 200 + 9);
    Data d;
    check("an inherited default constructor", d.p == 5 && Data(9).p == 9);
    {
        Holder a, b;
        a.n = 1;
        b = a;
        check("= default after the class (no temporary copied)",
              killed == 0 && b.n == 1);
    }
    check("... and destroyed", killed == 2);

    lib::Dir dir;
    check("struct N::X for an inline namespace's X", dir.fd == 3);
    check("N::f() { } for an inline namespace's f", lib::hash(2) == 62);
    check("one overload set across an inline namespace",
          lib::over(1) == 1 && lib::over(1L) == 2);

    int (Derived::*pf)(int) const = &Derived::fn;
    int Derived::*pm = &Derived::field;
    Derived dv;
    check("pointers to inherited members", (dv.*pf)(1) == 5 && dv.*pm == 4);
    check("Derived* prefers Base* over void*", pick(&dv) == 2);

    Buf<int> *bp = new Buf<int>;
    bp->~Buf<int>();
    ::operator delete(bp);
    check("p->~X<T>()", Buf<int>::gone == 1);

    int xs[2] = { 5, 6 };
    Iter<int *, Cont<int>> it(xs);
    Iter<int *, Cont<int>> it2(it);
    check("(dependent value) as a template argument", *it2.cur == 5);

    constexpr int bits = 40;
    auto width = [] { return bits <= 32 ? 32 : 64; };
    check("a constexpr local in a lambda without a capture", width() == 64);

    int n = 5;
    char vla[n + 1];
    for (int i = 0; i < n; i++)
        vla[i] = (char)('a' + i);
    vla[n] = 0;
    check("a variable-length array", strcmp(vla, "abcde") == 0);

    check("__builtin_powi", __builtin_powi(2.0, 10) == 1024.0 &&
                            __builtin_powif(3.0f, 2) == 9.0f);
    Facet plain;
    Custom custom;
    check("GNU: a bound member function's address", !overridden(&plain) &&
                                                    overridden(&custom));
    check("a weak reference, undefined: null", no_such_function == 0);
    check("a raw string", strlen(raw) == 26 && raw[1] == '\\' &&
                          raw[3] == '"' && elifdef_ok);
#ifdef __x86_64__
    unsigned long in = 41, out;
    asm("mov %1, %0" : "=r"(out) : "r"(in));
    asm volatile("" ::: "memory");
    check("an asm statement", out == 41);
#endif

    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 42;
}
