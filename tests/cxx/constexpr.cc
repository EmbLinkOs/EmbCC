// CX6: constant evaluation — constexpr functions called where a constant
// is needed (static_assert, array bounds, template arguments, enumerators,
// case labels, a const variable's value), run by an interpreter over the
// front-end's trees: loops, switch, recursion, locals, references, member
// functions and constexpr constructors, aggregates, arrays and pointers
// into them, strings, lambdas, floating point; `if constexpr` discarding
// the other branch unread; __builtin_is_constant_evaluated.
// expect-exit: 42
#include <stdio.h>
static int fails;
static void check(const char *what, bool ok)
{
    if (!ok)
        fails++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}
constexpr int sq(int x) { return x * x; }
constexpr long fact(int n) { return n <= 1 ? 1 : n * fact(n - 1); }
constexpr int fib(int n)
{
    int a = 0, b = 1;
    for (int i = 0; i < n; i++) {
        int t = a + b;
        a = b;
        b = t;
    }
    return a;
}
constexpr int collatz(int n)
{
    int steps = 0;
    while (n != 1) {
        if (n % 2)
            n = 3 * n + 1;
        else
            n /= 2;
        ++steps;
    }
    return steps;
}
constexpr int sw(int k)
{
    switch (k) {
    case 0: return 10;
    case 1:
    case 2: { int r = k * 100; return r; }
    default: break;
    }
    return -1;
}
struct Point {
    int x, y;
    constexpr Point(int a, int b) : x(a), y(b) {}
    constexpr int dot(const Point &o) const { return x * o.x + y * o.y; }
    constexpr Point operator+(const Point &o) const { return Point(x + o.x, y + o.y); }
};
constexpr Point origin(0, 0);
constexpr Point p1(3, 4);
constexpr int arrsum(const int *a, int n)
{
    int s = 0;
    for (int i = 0; i < n; i++)
        s += a[i];
    return s;
}
constexpr int table[] = { 1, 2, 3, 4, 5 };
constexpr int slen(const char *s) { int n = 0; while (*s++) n++; return n; }
template <int N> struct Int { static constexpr int value = N; };
template <class T> constexpr bool is_int() { return false; }
template <> constexpr bool is_int<int>() { return true; }
template <class T> int kind(T v)
{
    if constexpr (is_int<T>())
        return v + 1000;
    else
        return (int)(v * 2);
}
struct Agg { int a; double d; int arr[3]; };
constexpr Agg ag = { 7, 2.5, { 1, 2, 3 } };
constexpr int by_ref(int &r) { r += 5; return r; }
constexpr int use_ref() { int v = 1; by_ref(v); by_ref(v); return v; }
constexpr double half(double d) { return d / 2; }
constexpr int lam() { auto f = [](int x) { return x * 3; }; return f(7); }
constexpr int where() { return __builtin_is_constant_evaluated() ? 1 : 2; }
template <class T> int only_ints(T v)
{
    if constexpr (sizeof(T) == sizeof(int))
        return v;
    else
        return v.no_such_member;     // discarded: never read for int
}
constexpr int cases(int k)
{
    switch (k) {
    case sq(2): return 1;
    case sq(3): return 2;
    }
    return 0;
}
int main()
{
    static_assert(sq(7) == 49, "sq");
    static_assert(fact(10) == 3628800, "fact");
    static_assert(fib(20) == 6765, "fib");
    static_assert(collatz(27) == 111, "collatz");
    static_assert(sw(0) == 10 && sw(2) == 200 && sw(5) == -1, "switch");
    static_assert(p1.dot(p1) == 25, "member");
    static_assert((p1 + p1).y == 8, "operator");
    static_assert(arrsum(table, 5) == 15, "array");
    static_assert(slen("hello") == 5, "string");
    static_assert(Int<sq(3)>::value == 9, "template arg");
    static_assert(ag.arr[2] == 3 && ag.a == 7, "aggregate");
    static_assert(use_ref() == 11, "references");
    static_assert(half(5.0) == 2.5, "double");
    static_assert(lam() == 21, "lambda");
    int arr[sq(4)];
    check("array bound", sizeof arr == 64);
    check("if constexpr", kind(5) == 1005 && kind(2.5) == 5);
    const int n = fib(10);
    int b[n];
    check("const initialized by a call", sizeof b == 55 * sizeof(int));
    enum E { A = sq(5), B };
    check("enumerators", A == 25 && B == 26);
    check("at run time too", sq(n) == 3025);
    constexpr int ce = where();
    int rt = where();
    check("is_constant_evaluated", ce == 1 && rt == 2);
    check("discarded branch", only_ints(9) == 9);
    check("case labels", cases(4) == 1 && cases(9) == 2 && cases(5) == 0);
    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 42;
}
