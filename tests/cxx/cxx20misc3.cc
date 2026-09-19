// CX7/CX8, the rules coroutines' libraries needed: ?: between different
// class types (one converting to the other; a derived and a base lvalue
// giving the base lvalue); `x = {}` choosing the move assignment over the
// copy; a lambda's trailing return type naming its parameters; an
// explicit specialization named with its namespace (template<> struct
// ns::T<int>); a conversion function of a class template defined outside
// it; an explicit instantiation defining the members defined outside the
// class.
// expect-exit: 42
#include <stdio.h>

static int fails;
static void check(const char *what, bool ok)
{
    if (!ok)
        fails++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

// ---- ?: with class operands ----
struct Handle { int id; };
struct Special {
    int id;
    operator Handle() const { return Handle{id * 10}; }
};
static Handle pick(bool b, Handle h, Special s) { return b ? h : s; }
struct Base { int v = 1; };
struct Derived : Base { int w = 2; };

// ---- = {} ----
static int copies, moves;
struct Slot {
    int v = 5;
    Slot() = default;
    Slot(const Slot &) = default;
    Slot &operator=(const Slot &o) { copies++; v = o.v; return *this; }
    Slot &operator=(Slot &&o) { moves++; v = o.v; return *this; }
};

// ---- a qualified explicit specialization ----
namespace traits { template <class T> struct Size { static constexpr int value = 0; }; }
template <> struct traits::Size<int> { static constexpr int value = 4; };

// ---- members of a class template defined outside it ----
template <class T> struct Wrap {
    T v;
    operator Wrap<long>() const;
    T twice() const;
};
template <class T> Wrap<T>::operator Wrap<long>() const { return Wrap<long>{(long)v + 1}; }
template <class T> T Wrap<T>::twice() const { return v + v; }
template struct Wrap<int>;                   // both defined here

int main()
{
    Handle h{3};
    Special s{4};
    check("?: converting one class operand", pick(true, h, s).id == 3 &&
                                             pick(false, h, s).id == 40);
    Derived d;
    Base b;
    bool which = true;
    (which ? d : b).v = 9;                   // an lvalue: the Base part of d
    check("?: a derived and a base lvalue", d.v == 9 && b.v == 1);

    Slot x;
    x = {};
    check("= {} moves", moves == 1 && copies == 0 && x.v == 5);

    auto gl = [](auto p) -> decltype(p + 1) { return p + 1; };
    auto l2 = [](int a, long b) -> decltype(a + b) { return a + b; };
    check("a trailing return type naming the parameters",
          sizeof(gl(1.0f)) == sizeof(float) && sizeof(l2(1, 2)) == 8 &&
          l2(40, 2) == 42);

    check("template<> struct ns::T<int>", traits::Size<int>::value == 4 &&
                                           traits::Size<char>::value == 0);

    Wrap<int> w{20};
    Wrap<long> wl = w;
    check("members defined outside a class template", wl.v == 21 &&
                                                      w.twice() == 40);

    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 42;
}
