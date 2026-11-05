// #pragma pack in C++, as in C: (n), (), push/pop, a class with bases and
// a vptr, a union, a template (its definition's pack, not its use's), a
// nested class packed tighter; the layouts are clang's (sizeof, alignof,
// offsets asserted), and members read and written through them -- the
// misaligned ones included, and through a virtual call.
// expect-exit: 42
#include <string.h>
#define OFF(T, m) __builtin_offsetof(T, m)
#pragma pack(1)
struct A { char c; int i; double d; };
#pragma pack()
#pragma pack(push, 2)
struct B { char c; int i; long long l; short s; };
union U { char c; int i; double d; };
struct Base { char b; int x; };
struct D : Base { char e; double y; };
struct V { virtual int get() const { return (int)q; } virtual ~V() {} char c; long long q; };
struct W : V { int get() const override { return (int)q * 2; } };
template <class T> struct Tm { char c; T v; };
#pragma pack(push, 4)
struct Inner { char c; double d; };
#pragma pack(pop)
struct Outer { char c; Inner in; };
#pragma pack(pop)
struct After { char c; double d; };
static_assert(sizeof(A) == 13 && alignof(A) == 1 && OFF(A, i) == 1 && OFF(A, d) == 5, "A");
static_assert(sizeof(B) == 16 && alignof(B) == 2 && OFF(B, l) == 6 && OFF(B, s) == 14, "B");
static_assert(sizeof(U) == 8 && alignof(U) == 2, "U");
static_assert(sizeof(D) == 16 && alignof(D) == 2, "D");
static_assert(sizeof(Tm<double>) == 10 && alignof(Tm<double>) == 2, "Tm");
static_assert(sizeof(Inner) == 12 && sizeof(Outer) == 14, "Outer");
static_assert(sizeof(After) == 16, "After");

static int fails;
static void check(bool ok) { fails += !ok; }

int main()
{
    A as[2];
    for (int k = 0; k < 2; k++) {
        as[k].c = (char)k;
        as[k].i = 1000 + k;
        as[k].d = 2.5 * k;
    }
    check(as[1].i == 1001 && as[1].d == 2.5);
    check((char *)&as[1].i - (char *)&as[0] == 14);
    A copy = as[1];
    check(copy.i == 1001 && memcmp(&copy, &as[1], sizeof copy) == 0);
    B b = { 1, 2, 0x123456789LL, 7 };
    check(b.l == 0x123456789LL && b.s == 7);
    D d;
    d.b = 1; d.x = 70000; d.e = 3; d.y = 1.25;
    Base &base = d;
    check(base.x == 70000 && d.y == 1.25);
    W w;
    w.q = 21;
    V &v = w;
    check(v.get() == 42);
    Tm<double> t = { 'x', 0.5 };
    check(t.v == 0.5);
    Outer o;
    o.in.d = 4.0;
    check(o.in.d == 4.0 && (char *)&o.in.d - (char *)&o == 6);
    U u;
    u.d = 8.0;
    check(u.d == 8.0);
    return fails == 0 ? 42 : 1;
}
