// CX2: pointers to members — to data members (an offset; null is -1) and
// to member functions ({ptr, adj}) — formed with &C::m, used with .* and
// ->*, compared, tested, passed and stored; overloaded members picked by
// the target type.
// expect-exit: 42
#include <stdio.h>

static int fails;
static void check(const char *what, bool ok)
{
    if (!ok)
        fails++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

struct Point {
    int x, y, z;
    int sum() const { return x + y + z; }
    int scaled(int k) const { return (x + y + z) * k; }
    void add(int d) { x += d; y += d; z += d; }
    int pick(int) const { return 1; }
    int pick(long) const { return 2; }
};

static int Point::*axis(int i)
{
    return i == 0 ? &Point::x : i == 1 ? &Point::y : &Point::z;
}

static int apply(const Point &p, int (Point::*f)(int) const, int arg)
{
    return (p.*f)(arg);
}

struct Table {
    const char *name;
    int (Point::*get)() const;
};
static const Table table[] = {
    { "sum", &Point::sum },
};
static int Point::*g_member = &Point::y;
static int Point::*g_null = nullptr;

int main()
{
    Point p = { 1, 2, 3 };
    int Point::*m = &Point::y;
    check("data member pointer", p.*m == 2);
    p.*m = 20;
    check("assignment through it", p.y == 20);
    Point *pp = &p;
    pp->*axis(2) += 30;
    check("->* and a returned member pointer", p.z == 33);
    check("offsets", sizeof(int Point::*) == 8 && axis(0) != axis(1));

    int (Point::*s)() const = &Point::sum;
    check("member function pointer", (p.*s)() == 54);
    void (Point::*a)(int) = &Point::add;
    (pp->*a)(1);
    check("through ->*", p.x == 2 && p.y == 21);
    check("as a parameter", apply(p, &Point::scaled, 2) == 114);
    check("in a constant table", (p.*table[0].get)() == 57);
    int (Point::*pl)(long) const = &Point::pick;
    check("an overloaded member chosen by type", (p.*pl)(0L) == 2);

    int Point::*none = nullptr;
    void (Point::*nf)(int) = nullptr;
    check("null member pointers", !none && none == nullptr && !nf &&
          nf == nullptr && a != nullptr && g_null == nullptr);
    check("comparisons", s == &Point::sum && a != nf && m == g_member &&
          sizeof(s) == 16);

    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 42;
}
