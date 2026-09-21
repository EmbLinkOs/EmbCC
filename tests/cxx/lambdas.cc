// CX6: lambdas — closure objects of classes of their own; captures by
// copy and by reference, explicit, implicit (capture-defaults) and
// init-captures; `this` captured; mutable; trailing return types; the
// conversion to a pointer to function of a lambda that captures nothing;
// [*this]; lambdas in lambdas (capturing what the enclosing one captured); generic
// lambdas (`auto` parameters: operator() is a member template), including
// `auto...` packs.
// expect-exit: 42
#include <stdio.h>
#include <stdlib.h>

static int fails;
static void check(const char *what, bool ok)
{
    if (!ok)
        fails++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

template <class F> int apply(F f, int x) { return f(x); }
template <class F, class T> auto apply2(F f, T a, T b) { return f(a, b); }
template <class F> int count3(F f, int a, int b, int c)
{
    return f(a) + f(b) + f(c);
}
static int call_ptr(int (*f)(int), int x) { return f(x); }

struct Counter {
    int n = 0;
    int bump(int k)
    {
        auto add = [this, k](int m) { n += k * m; return n; };
        return add(2);
    }
    int via_default() { auto get = [=] { return n + 100; }; return get(); }
    int generic(int k)
    {
        auto g = [=](auto x) { return n + x + k; };
        auto h = [&](auto x) { n += x; return n; };
        h(1);
        return g(2) + h(0);
    }
    int nested()
    {
        return [=] { int a = n; return [=] { return a + n; }(); }();
    }
    auto snapshot() { return [*this] { return n; }; }
    auto live() { return [this] { return n; }; }
};

template <class T> T twice(T v)
{
    auto d = [](auto x) { return x + x; };
    return d(v);
}

struct P { int a, b; };

int main()
{
    // ---- captures ----
    int base = 10;
    auto plus = [base](int x) { return x + base; };
    auto byref = [&base](int x) { base += x; };
    auto all_copy = [=](int x) { return x * base; };
    auto all_ref = [&] { base *= 2; };
    check("by copy", plus(5) == 15);
    byref(3);
    check("by reference", base == 13);
    check("capture-default =", all_copy(2) == 20);
    all_ref();
    check("capture-default &", base == 26);
    auto init = [v = base + 1, &r = base] { r += v; return v; };
    check("init-captures", init() == 27 && base == 53);
    int calls = 0;
    auto mut = [calls]() mutable { return ++calls; };
    mut();
    check("mutable: the closure's own copy", mut() == 2 && calls == 0);
    auto lng = [](int x) -> long { return x; };
    check("trailing return type", lng(7) == 7L && sizeof(lng(1)) == sizeof(long));
    base = 3;
    auto later = [=] { return base; };
    base = 4;
    check("a copy is taken when the lambda is made", later() == 3);

    // ---- this ----
    Counter c;
    check("[this]", c.bump(3) == 6);
    check("[=] and a member", c.via_default() == 106);
    Counter c2;
    check("generic, in a member function", c2.generic(7) == 1 + 2 + 7 + 1);
    check("this through nested lambdas", c2.nested() == 2);
    auto snap = c2.snapshot();
    auto lv = c2.live();
    c2.n = 99;
    check("[*this]: a copy", snap() == 1 && lv() == 99);

    // ---- passed around ----
    check("to a template", apply([](int x) { return x * x; }, 7) == 49);
    int arr[5] = { 5, 3, 9, 1, 7 };
    qsort(arr, 5, sizeof(int), [](const void *a, const void *b) {
        return *(const int *)a - *(const int *)b;
    });
    check("to a function pointer (qsort)", arr[0] == 1 && arr[4] == 9);
    int (*sq)(int) = [](int x) { return x * x; };
    auto mk = [](int a, int b) { return P{ a, b }; };
    P (*pm)(int, int) = mk;
    P p = pm(3, 4);
    check("converted", sq(6) == 36 && p.a == 3 && p.b == 4 &&
                       call_ptr([](int x) { return x + 1; }, 41) == 42);

    // ---- nested ----
    int m = 5;
    auto f1 = [=] { int t = m; return [=] { return m + t; }(); };
    check("outer captured first", f1() == 10);
    auto f2 = [n = 4] { return [=] { return n * 2; }(); };
    check("an init-capture captured again", f2() == 8);
    auto f3 = [n = 4, &m] { return [&] { m += n; return m; }(); };
    check("by reference through two", f3() == 9 && m == 9);
    auto f4 = [&](int x) { return [=](int y) { return x + y + m; }(1); };
    check("[=] in [&]", f4(4) == 14);

    // ---- generic ----
    auto id = [](auto x) { return x; };
    check("generic: identity", id(5) == 5 && id(2.5) == 2.5 && id('c') == 'c');
    auto add = [](auto a, auto b) { return a + b; };
    check("generic: two parameters", add(1, 2) == 3 && add(1.5, 2) == 3.5);
    int k = 10;
    auto addk = [k](auto x) { return x + k; };
    check("generic: explicit capture", addk(1) == 11 && addk(0.5) == 10.5);
    auto grow = [&](auto x) { k += x; return k; };
    grow(5);
    check("generic: capture-default &", k == 15 && grow(1L) == 16);
    int q = 3;
    auto scale = [=](auto x) { return x * q + k; };
    q = 100;
    check("generic: capture-default =", scale(2) == 22);
    auto larger = [](const auto &a, const auto &b) { return a < b ? b : a; };
    check("generic: const auto &", larger(3, 7) == 7 && larger(2.5, 1.0) == 2.5);
    check("generic: to a template",
          apply2(add, 4, 5) == 9 &&
          apply2([](auto a, auto b) { return a * b; }, 3, 4) == 12);
    int lim = 2;
    check("generic: a predicate", count3([&](auto v) { return v > lim; }, 1, 3, 5) == 2);
    check("generic: in a function template", twice(21) == 42 && twice(1.25) == 2.5);
    auto sum = [](auto... xs) { return (0 + ... + xs); };
    check("generic: auto...", sum() == 0 && sum(1, 2, 3) == 6 && sum(1, 2.5) == 3.5);
    auto curry = [=](auto x) { return [=](auto y) { return x * 10 + y + q; }; };
    check("generic: nested", curry(4)(2) == 142);
    auto deep = [=](auto a) {
        return [=] { return [=](auto b) { return a + b + m; }(1); }();
    };
    check("generic: three deep", deep(10) == 20);
    int cnt = 0;
    auto inc = [&cnt](auto by) { return [&cnt, by] { cnt += by; return cnt; }; };
    inc(2)();
    check("generic: returning a lambda", inc(3)() == 5 && cnt == 5);
    auto mixed = [](int a, auto b) { return a - b; };
    check("generic: mixed parameters", mixed(10, 3) == 7);
    auto acc = [n = 0](auto s) mutable { n += s; return n; };
    acc(2);
    check("generic: mutable, init-capture", acc(3) == 5);

    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 42;
}
