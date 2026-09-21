// CX8: template rules libstdc++'s <ostream> and <string> depend on —
// partial ordering of function templates (each parameter pair deduced on
// its own, exactly; for operator syntax every parameter compared); a
// function template's address taken where a pointer to a function is
// wanted (its arguments deduced from that type, as std::endl is);
// explicit template arguments before an overload set whose templates'
// parameters differ in kind; defaults added by a redeclaration;
// __func__ and __PRETTY_FUNCTION__.
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

template <class C> struct tr {};
template <class C, class T = tr<C>> struct os { int k = 0; };
template <class C, class T> os<C, T> &operator<<(os<C, T> &o, const C *) { o.k = 1; return o; }
template <class C, class T> os<C, T> &operator<<(os<C, T> &o, const char *) { o.k = 2; return o; }
template <class T> os<char, T> &operator<<(os<char, T> &o, const char *) { o.k = 3; return o; }
template <class C, class T> os<C, T> &operator<<(os<C, T> &o, os<C, T> &(*f)(os<C, T> &))
{
    return f(o);
}
template <class C, class T> os<C, T> &endl(os<C, T> &o) { o.k = 9; return o; }

struct S {
    int got = 0;
    template <class It> void pick(It, It) { got = 1; }
    template <bool Term> void pick(const char *, unsigned long) { got = Term ? 2 : 3; }
    void run() { pick<true>("x", 1); }
};

template <class A, class B> struct pair_of;
template <class A, class B = int> struct pair_of;
template <class A, class B> struct pair_of { A a; B b; };

struct Named {
    const char *who() { return __func__; }
    const char *full() { return __PRETTY_FUNCTION__; }
};

int main()
{
    os<char> o;
    o << "x";
    check("the most specialized operator<<", o.k == 3);
    os<wchar_t> w;
    w << L"x";
    check("... and the general one", w.k == 1);
    o << endl;
    check("a function template for a function pointer", o.k == 9);
    S s;
    s.run();
    check("f<true>() beside f<It>()", s.got == 2);
    pair_of<char> p{ 'a', 7 };
    check("a default added by a redeclaration", sizeof p.b == sizeof(int) && p.b == 7);
    Named n;
    check("__func__", strcmp(n.who(), "who") == 0 && strstr(n.full(), "full"));
    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 42;
}
