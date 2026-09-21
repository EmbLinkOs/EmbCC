// consteval (7.7): immediate functions. Each call outside another
// immediate function is evaluated when the program is compiled — an
// integer's value written as a constant, a class's built at run time by
// the same (constexpr) code once the evaluation succeeded. Inside an
// immediate function, and in `if consteval`, calls with its parameters are
// fine. The evaluator follows what libstdc++'s format checking needs:
// virtual calls by the object's dynamic type (through a base, overridden
// twice, in a constructor's body), and bit-fields read, written and
// initialized. (Ill-formed calls: tests/golden/cxx-reject.sh.)
// expect-exit: 42
#include <stdio.h>

consteval int sq(int x) { return x * x; }
consteval int sum_sq(int a, int b) { return sq(a) + sq(b); }   // params: fine
constexpr int twice(int x)
{
    if consteval {
        return sq(x) / x * 2;           // an immediate context: x may be read
    } else {
        return x + x;
    }
}

struct Shape {
    constexpr virtual int sides() const = 0;
    constexpr virtual int weight() const { return sides() * 10; }
    constexpr virtual ~Shape() = default;
};
struct Tri : Shape {
    constexpr int sides() const override { return 3; }
};
struct Quad : Shape {
    int built_as;
    constexpr Quad() : built_as(0) { built_as = sides(); }
    constexpr int sides() const override { return 4; }
};
struct Square : Quad {
    constexpr int weight() const override { return 1 + Quad::weight(); }
};
struct Pair { int pad; Square sq; };
consteval int shapes()
{
    Tri t;
    Square s;
    Pair p{};
    const Shape *all[3] = { &t, &s, &p.sq };
    int r = 0;
    for (const Shape *x : all)
        r = r * 100 + x->weight();
    return r * 10 + s.built_as;          // 30, 41, 41; built as a Quad: 4
}

struct Flags {
    unsigned kind : 3;
    int delta : 5;
    bool on : 1;
    unsigned long long big : 40;
};
consteval long flags()
{
    Flags f{5, -7, true, 0xfffffffffULL};
    f.kind += 4;                         // 9 in 3 bits: 1
    f.delta -= 10;                       // -17 in 5 signed bits: 15
    f.big <<= 5;                         // the top bits fall off
    f.on = !f.on;
    return f.kind * 1000000L + f.delta * 1000L + f.on * 100L +
           (long)(f.big >> 34);
}

struct Id {
    int v;
    consteval Id(int x) : v(x * 3) {}    // a class's value: built at run time
};

int global = sum_sq(3, 4);               // 25, as a constant
int from_ctor = Id(7).v;

int main()
{
    int fails = 0;
#define CHECK(c) do { if (!(c)) { fails++; printf("FAIL %s\n", #c); } } while (0)
    CHECK(sq(9) == 81 && sum_sq(1, 2) == 5);
    CHECK(global == 25 && from_ctor == 21);
    int n = 5;
    CHECK(twice(n) == 10);               // at run time: the else branch
    static_assert(twice(6) == 12);
    CHECK(shapes() == 3041414);
    CHECK(flags() == 1015000 + 63);
    Id id(4);
    CHECK(id.v == 12);
    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 42;
}
