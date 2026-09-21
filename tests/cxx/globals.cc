// CX1: namespace-scope objects are built before main in declaration order
// (.init_array), destroyed after it in reverse (__cxa_atexit); a function's
// local static is built once, on first use (__cxa_guard_acquire), and
// destroyed at exit too.
// expect-exit: 42
#include <stdio.h>

static int seq;

struct Global {
    const char *name;
    int order;
    Global(const char *n) : name(n), order(++seq) {}
    ~Global() { printf("exit: ~%s\n", name); }
};

Global g_one("one");
static Global g_two("two");
namespace deep {
Global g_three("three");
}

static int built;
struct Lazy {
    int v;
    Lazy(int x) : v(x) { built++; printf("lazy %d built\n", x); }
    ~Lazy() { printf("exit: ~lazy %d\n", v); }
};

static int get(int x)
{
    static Lazy l(x);
    return l.v;
}

struct Plain {
    int a, b;
};
static Plain g_plain = { 4, 2 };        // constant: no code runs
static int g_answer = g_plain.a * 10 + g_plain.b;

int main()
{
    int fails = 0;
    if (g_one.order != 1 || g_two.order != 2 || deep::g_three.order != 3)
        fails++;
    printf("order %d %d %d\n", g_one.order, g_two.order,
           deep::g_three.order);
    if (get(7) != 7 || get(8) != 7 || built != 1)
        fails++;
    if (g_answer != 42)
        fails++;
    printf("main done, %d failure(s)\n", fails);
    return fails ? 1 : 42;
}
