// CX1: the "better C" core — bool, nullptr, references, overloading with
// Itanium names, default arguments, namespaces, extern "C", enums, casts,
// auto and decltype.
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

// overloading: the argument's exact type picks the function
static const char *kind(int) { return "int"; }
static const char *kind(long) { return "long"; }
static const char *kind(unsigned) { return "unsigned"; }
static const char *kind(double) { return "double"; }
static const char *kind(char) { return "char"; }
static const char *kind(const char *) { return "const char *"; }
static const char *kind(void *) { return "void *"; }
static const char *kind(bool) { return "bool"; }

static int sum(int a, int b = 10, int c = 100) { return a + b + c; }

static void bump(int &r) { r++; }
static int peek(const int &r) { return r; }
static int take(int &&r) { r += 1; return r; }
static int which(int &) { return 1; }
static int which(const int &) { return 2; }
static int which2(const int &) { return 1; }
static int which2(int &&) { return 2; }

namespace outer {
int value = 1;
namespace inner {
int value = 2;
int get() { return value + outer::value * 10; }
}
int get() { return value; }
}
namespace outer {            // reopened
int more() { return inner::get() + 100; }
}
namespace alias = outer::inner;

namespace {
int hidden() { return 7; }
}

extern "C" int c_linkage_fn(int x) { return x + 1; }

enum Color { Red, Green = 5, Blue };
enum class Mode : unsigned char { Off, On = 200 };

namespace ver {
inline namespace v2 {
int id() { return 2; }
}
}

struct Pair {
    int a, b;
};

int main()
{
    bool t = true, f = false;
    check("bool", t && !f && sizeof(bool) == 1 && (t + t) == 2);
    int *np = nullptr;
    check("nullptr", np == nullptr && !np && np == 0);

    check("overload int", strcmp(kind(1), "int") == 0);
    check("overload long", strcmp(kind(1L), "long") == 0);
    check("overload unsigned", strcmp(kind(1u), "unsigned") == 0);
    check("overload double", strcmp(kind(1.0), "double") == 0);
    check("overload char", strcmp(kind('x'), "char") == 0);
    check("overload const char *", strcmp(kind("s"), "const char *") == 0);
    check("overload void *", strcmp(kind((void *)np), "void *") == 0);
    check("overload bool", strcmp(kind(t), "bool") == 0);
    check("promotion short->int", strcmp(kind((short)3), "int") == 0);
    check("promotion float->double", strcmp(kind(1.5f), "double") == 0);

    check("default args", sum(1) == 111 && sum(1, 2) == 103 &&
                          sum(1, 2, 3) == 6);

    int x = 5;
    int &rx = x;
    rx = 9;
    bump(x);
    check("lvalue reference", x == 10 && &rx == &x);
    const int &ct = 40 + 2;
    check("const ref to a temporary", ct == 42 && peek(3) == 3);
    check("rvalue reference", take(41) == 42);
    const int cx = 1;
    check("ref overloads by constness", which(x) == 1 && which(cx) == 2);
    check("rvalue vs const lvalue ref", which2(x) == 1 && which2(5) == 2);

    check("namespaces", outer::get() == 1 && outer::inner::get() == 12 &&
                        outer::more() == 112 && alias::get() == 12);
    check("unnamed namespace", hidden() == 7);
    check("inline namespace", ver::id() == 2 && ver::v2::id() == 2);
    {
        using namespace outer::inner;
        check("using-directive", get() == 12);
    }
    {
        using outer::more;
        check("using-declaration", more() == 112);
    }
    check("extern \"C\"", c_linkage_fn(41) == 42);

    Color c = Blue;
    check("unscoped enum", c == 6 && Green == 5 && c > Red);
    Mode m = Mode::On;
    check("scoped enum", m == Mode::On && static_cast<int>(m) == 200 &&
                         sizeof(Mode) == 1);

    double d = 3.75;
    check("static_cast", static_cast<int>(d) == 3);
    const char *cs = "abc";
    char *ms = const_cast<char *>(cs);
    check("const_cast", ms == cs);
    long addr = reinterpret_cast<long>(np);
    check("reinterpret_cast", addr == 0);
    check("functional cast", int(d) == 3 && double(1) == 1.0);

    auto a1 = 5;
    auto a2 = 2.5;
    auto *a3 = &x;
    const auto &a4 = x;
    check("auto", sizeof(a1) == sizeof(int) && a2 == 2.5 && *a3 == 10 &&
                  &a4 == &x);
    decltype(x) dx = 3;
    decltype((x)) rdx = x;
    rdx = 11;
    check("decltype", dx == 3 && x == 11);

    Pair p = { 1, 2 };
    Pair q{ 3, 4 };
    Pair r{};
    check("aggregate init", p.a + p.b == 3 && q.a * q.b == 12 &&
                            r.a == 0 && r.b == 0);
    int arr[] = { 1, 2, 3 };
    int zero[4]{};
    check("array init", sizeof(arr) == 3 * sizeof(int) && arr[2] == 3 &&
                        zero[3] == 0);

    int *h = new int(41);
    ++*h;
    int v = *h;
    delete h;
    int *ha = new int[4]();
    ha[3] = 1;
    int hs = ha[0] + ha[3];
    delete[] ha;
    check("new/delete of scalars", v == 42 && hs == 1);

    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 42;
}
