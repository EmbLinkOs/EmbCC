// GNU complex types in C++ (what libstdc++'s std::complex is built on):
// __complex__ float/double/long double, arithmetic with complex and real
// operands, == and !=, __real__/__imag__ as values and as lvalues, a
// complex made of its two parts ({re, im}), conversions between element
// types, overloading on them (mangled C<type>, as g++ does), and passing
// them by value both ways.
// expect-exit: 42
#include <stdio.h>

static int fails;
static void check(const char *what, bool ok)
{
    if (!ok)
        fails++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

typedef __complex__ double cd;
typedef __complex__ float cf;

static cd make(double re, double im)
{
    cd z = { re, im };
    return z;
}

static int kind(cd) { return 2; }
static int kind(cf) { return 1; }
static int kind(double) { return 0; }

struct Holder {
    cd value;
    Holder(double r, double i) : value{ r, i } {}
    double re() const { return __real__ value; }
    void set_im(double i) { __imag__ value = i; }
};

int main()
{
    cd a = make(1.0, 2.0), b = make(3.0, -4.0);
    cd c = a * b + 1.0;                  // 12 + 2i
    check("multiply, add a real", __real__ c == 12.0 && __imag__ c == 2.0);
    cd d = c / make(0.0, 1.0);           // 2 - 12i
    check("divide", __real__ d == 2.0 && __imag__ d == -12.0);
    c -= 2.0;
    c *= 2.0;
    check("compound assignment", __real__ c == 20.0 && __imag__ c == 4.0);
    __real__ c = 7.0;
    __imag__ c += 1.0;
    check("parts as lvalues", __real__ c == 7.0 && __imag__ c == 5.0);
    check("== and !=", a == make(1.0, 2.0) && a != b && !(a == b));
    cf f = d;
    check("to __complex__ float", __real__ f == 2.0f && __imag__ f == -12.0f);
    check("negation", __real__ (-a) == -1.0 && __imag__ (+a) == 2.0);
    check("overloads by type", kind(a) == 2 && kind(f) == 1 && kind(1.0) == 0);
    Holder h(3.0, 4.0);
    h.set_im(6.0);
    check("a member, made of parts", h.re() == 3.0 && __imag__ h.value == 6.0);
    __complex__ long double l = a;
    check("long double parts", __real__ l == 1.0L && sizeof l == 32);
    check("sizes", sizeof(cf) == 8 && sizeof(cd) == 16);

    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 42;
}
