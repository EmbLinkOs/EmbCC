// CX2: operator overloading — member and non-member, unary and binary,
// compound assignment, comparisons, [] () -> and ++/-- (both forms), a
// chaining operator<<, operators on enums; argument-dependent lookup; and
// conversions — converting constructors, conversion functions, explicit
// operator bool.
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

namespace geo {

struct Vec {
    int x, y;
    Vec() : x(0), y(0) {}
    Vec(int a, int b) : x(a), y(b) {}
    Vec operator+(const Vec &o) const { return Vec(x + o.x, y + o.y); }
    Vec operator-() const { return Vec(-x, -y); }
    Vec &operator+=(const Vec &o) { x += o.x; y += o.y; return *this; }
    bool operator==(const Vec &o) const { return x == o.x && y == o.y; }
    bool operator!=(const Vec &o) const { return !(*this == o); }
    int &operator[](int i) { return i == 0 ? x : y; }
    int operator()(int k) const { return x * k + y; }
};

// non-member, found by argument-dependent lookup from outside geo
Vec operator*(const Vec &v, int k) { return Vec(v.x * k, v.y * k); }
Vec operator*(int k, const Vec &v) { return v * k; }
int dot(const Vec &a, const Vec &b) { return a.x * b.x + a.y * b.y; }

}

struct Counter {
    int n = 0;
    Counter &operator++() { ++n; return *this; }
    Counter operator++(int) { Counter old = *this; ++n; return old; }
    Counter &operator--() { --n; return *this; }
    Counter operator--(int) { Counter old = *this; --n; return old; }
};

struct Sink {
    char buf[64];
    int len = 0;
    Sink() { buf[0] = 0; }
    Sink &operator<<(const char *s)
    {
        while (*s)
            buf[len++] = *s++;
        buf[len] = 0;
        return *this;
    }
    Sink &operator<<(int v)
    {
        char t[16];
        int k = 0;
        if (v == 0)
            t[k++] = '0';
        while (v) {
            t[k++] = (char)('0' + v % 10);
            v /= 10;
        }
        while (k)
            buf[len++] = t[--k];
        buf[len] = 0;
        return *this;
    }
};

struct Node {
    int value;
};
struct Handle {
    Node *p;
    Node *operator->() const { return p; }
    Node &operator*() const { return *p; }
};

enum class Flags : unsigned { None = 0, A = 1, B = 2, C = 4 };
constexpr Flags operator|(Flags a, Flags b)
{
    return static_cast<Flags>(static_cast<unsigned>(a) |
                              static_cast<unsigned>(b));
}
constexpr bool has(Flags set, Flags f)
{
    return (static_cast<unsigned>(set) & static_cast<unsigned>(f)) != 0;
}

struct Meters {
    double v;
    Meters(double d) : v(d) {}                    // converting
    operator double() const { return v; }        // conversion function
};
static double twice(Meters m) { return m.v * 2; }

struct Opt {
    int *p;
    explicit operator bool() const { return p != nullptr; }
};

struct Id {
    int v;
    explicit Id(int x) : v(x) {}
};
static int take_id(Id i) { return i.v; }
static int take_id(long) { return -1; }        // Id(int) is explicit

int main()
{
    geo::Vec a(1, 2), b(3, 4);
    geo::Vec c = a + b;
    check("member operator+", c.x == 4 && c.y == 6);
    check("unary operator-", (-a).x == -1 && (-a).y == -2);
    c += a;
    check("compound assignment", c == geo::Vec(5, 8));
    check("operator!= via ==", c != a);
    c[0] = 10;
    check("operator[] returns a reference", c.x == 10 && c[1] == 8);
    check("operator()", a(10) == 12);
    geo::Vec d = a * 3, e = 2 * b;
    check("non-member operators found by ADL", d == geo::Vec(3, 6) &&
                                               e == geo::Vec(6, 8));
    check("ADL for an ordinary call", dot(a, b) == 11);

    Counter k;
    Counter old = k++;
    ++k;
    check("postfix and prefix ++", old.n == 0 && k.n == 2);
    --k;
    k--;
    check("postfix and prefix --", k.n == 0);

    Sink s;
    s << "x=" << 42 << ", y=" << 7;
    check("chained operator<<", strcmp(s.buf, "x=42, y=7") == 0);

    Node n = { 41 };
    Handle h = { &n };
    h->value++;
    check("operator-> and unary *", (*h).value == 42);

    Flags f = Flags::A | Flags::C;
    check("operators on an enum class", has(f, Flags::C) &&
                                        !has(f, Flags::B));

    Meters m = 2.5;
    double sum = m + 1.0;
    check("converting constructor and conversion function",
          twice(4.0) == 8.0 && sum == 3.5);
    int x = 3;
    Opt some = { &x }, none = { nullptr };
    int both = 0;
    if (some)
        both++;
    if (!none)
        both++;
    check("explicit operator bool in conditions", both == 2);
    check("an explicit constructor is not a conversion",
          take_id(5) == -1 && take_id(Id(9)) == 9);

    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 42;
}
