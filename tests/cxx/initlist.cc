// CX6: std::initializer_list (declared as libstdc++'s <initializer_list>
// declares it) — made from a braced list for an argument (its array a
// temporary of the call's full-expression) or a variable (living as long
// as the variable); auto x = { ... }; initializer-list constructors chosen
// first in list-initialization, but {} still the default constructor;
// deduced from a braced list (initializer_list<T>); preferred by
// overload resolution; range-based for over a braced list.
// expect-exit: 42
#include <stdio.h>
#include <string.h>
typedef unsigned long size_t;
// std::initializer_list as libstdc++'s <initializer_list> declares it
namespace std {
template <class _E> class initializer_list {
public:
    typedef _E value_type;
    typedef const _E &reference;
    typedef const _E &const_reference;
    typedef size_t size_type;
    typedef const _E *iterator;
    typedef const _E *const_iterator;
private:
    iterator _M_array;
    size_type _M_len;
    constexpr initializer_list(const_iterator __a, size_type __l)
        : _M_array(__a), _M_len(__l) {}
public:
    constexpr initializer_list() noexcept : _M_array(0), _M_len(0) {}
    constexpr size_type size() const noexcept { return _M_len; }
    constexpr const_iterator begin() const noexcept { return _M_array; }
    constexpr const_iterator end() const noexcept { return begin() + size(); }
};
template <class _Tp> constexpr const _Tp *begin(initializer_list<_Tp> __ils) noexcept
{ return __ils.begin(); }
template <class _Tp> constexpr const _Tp *end(initializer_list<_Tp> __ils) noexcept
{ return __ils.end(); }
}
static int fails;
static void check(const char *what, bool ok)
{
    if (!ok)
        fails++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}
static int sum(std::initializer_list<int> l)
{
    int s = 0;
    for (int x : l)
        s += x;
    return s;
}
struct Vec {
    int d[8];
    int n;
    int how;
    Vec(std::initializer_list<int> l) : n(0), how(1)
    {
        for (const int *p = l.begin(); p != l.end(); ++p)
            d[n++] = *p;
    }
    Vec(int count, int v) : n(count), how(2)
    {
        for (int i = 0; i < count; i++)
            d[i] = v;
    }
    Vec() : n(0), how(3) {}
};
static int live;
struct Obj {
    int v;
    Obj(int x) : v(x) { live++; }
    Obj(const Obj &o) : v(o.v) { live++; }
    ~Obj() { live--; }
};
static int total(std::initializer_list<Obj> l)
{
    int s = 0;
    for (const Obj &o : l)
        s += o.v;
    return s + live * 1000;
}
template <class T> T tsum(std::initializer_list<T> l)
{
    T s = 0;
    for (auto x : l)
        s += x;
    return s;
}
static const char *pick(std::initializer_list<int>) { return "list"; }
static const char *pick(int) { return "int"; }
int main()
{
    check("argument", sum({ 1, 2, 3, 4 }) == 10 && sum({}) == 0);
    std::initializer_list<int> il = { 5, 6, 7 };
    check("a variable (the array lives as long)", il.size() == 3 && sum(il) == 18);
    auto al = { 1, 2, 3 };
    check("auto x = { ... }", al.size() == 3 && *al.begin() == 1);
    Vec a{ 1, 2, 3 };
    Vec b(3, 9);
    Vec c{ 3, 9 };
    Vec d{};
    Vec e = { 4 };
    check("initializer-list constructors first", a.how == 1 && a.n == 3 && a.d[2] == 3);
    check("... not with parentheses", b.how == 2 && b.n == 3 && b.d[0] == 9);
    check("... even when another fits", c.how == 1 && c.n == 2 && c.d[1] == 9);
    check("{} is the default constructor", d.how == 3);
    check("copy-list", e.how == 1 && e.n == 1 && e.d[0] == 4);
    int s = 0;
    for (int x : { 10, 20, 30 })
        s += x;
    check("range-for over a braced list", s == 60);
    int t = total({ Obj(1), Obj(2) });
    check("objects, destroyed after the call", t == 3 + 2000 && live == 0);
    check("deduced", tsum({ 1, 2, 3 }) == 6 && tsum({ 1.5, 2.5 }) == 4.0);
    check("overloads prefer the list", strcmp(pick({ 1 }), "list") == 0 && strcmp(pick(1), "int") == 0);
    const std::initializer_list<int> &rl = { 8, 9 };
    check("a reference to one", rl.size() == 2 && rl.begin()[1] == 9);
    std::initializer_list<int> empty;
    check("empty", empty.size() == 0 && empty.begin() == empty.end());
    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 42;
}
