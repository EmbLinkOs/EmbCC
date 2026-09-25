/* What unwinding has to get right, checked by what a program observes.
 *
 * An unwinder is not testable by asking it questions -- it either puts
 * the machine back the way a frame left it or it does not, and "not"
 * shows up as a wrong answer somewhere far away. So this is a program
 * whose OUTPUT says what happened: every destructor announces itself,
 * every handler announces which one it is, and the order of those lines
 * is the property under test.
 *
 * The same source runs twice: once on the bare-metal harness, where
 * libgcc's unwinder does the walking, and once on Linux, where
 * lib/rt/unwind.c does. The two must print the same lines. That makes
 * libgcc the oracle without needing it to link into a static image --
 * which it will not, because it wants a crtbegin to register the frame
 * tables for it.
 *
 * The cases are chosen where a not-quite-right unwinder still works for
 * the simple ones: a catch several frames up, destructors in reverse
 * order across those frames, a handler chosen by inheritance rather
 * than by exact type, a rethrow that has to find a DIFFERENT handler,
 * catch-all, a C frame in the middle of the chain, and a throw from
 * inside a destructor's scope.
 */
#include <stdio.h>

static int depth;

/* Announces its construction and destruction, so the trace shows both
 * that the destructor ran and WHEN. */
struct Noisy {
    const char *name;
    explicit Noisy(const char *n) : name(n) { printf("  +%s\n", name); }
    ~Noisy() { printf("  -%s\n", name); }
};

struct Base { virtual ~Base() {} virtual const char *who() const { return "Base"; } };
struct Derived : Base { const char *who() const override { return "Derived"; } };
struct Other { int v; explicit Other(int x) : v(x) {} };

/* ---- 1. a catch several frames up, with destructors in between ---------- */
static void level3() { Noisy a("level3"); throw 7; }
static void level2() { Noisy a("level2"); level3(); }
static void level1() { Noisy a("level1"); level2(); }

/* ---- 2. through a C frame ----------------------------------------------
 *
 * The callback is called from a function with C linkage and no
 * landing pads of its own. Its frame still has to be stepped over, and
 * on a compiler that emits unwind tables only for C++ it cannot be --
 * which is how `throw` through qsort's comparator becomes terminate. */
extern "C" void c_middle(void (*fn)(void));
extern "C" void c_middle(void (*fn)(void)) { fn(); }
static void throws_int() { Noisy a("under-c"); throw 11; }

/* ---- 3. a handler chosen by inheritance -------------------------------- */
static void throw_derived() { Noisy a("derived"); throw Derived(); }

/* ---- 4. rethrow, which has to find a different handler ------------------ */
static void inner_rethrow()
{
    try { Noisy a("rethrow-inner"); throw 22; }
    catch (int) { printf("  inner saw int, rethrowing\n"); throw; }
}

/* ---- 5. an exception carrying a non-trivial object --------------------- */
struct Payload {
    int v;
    explicit Payload(int x) : v(x) { printf("  +payload %d\n", v); }
    Payload(const Payload &o) : v(o.v) { printf("  copy payload %d\n", v); }
    ~Payload() { printf("  -payload %d\n", v); }
};
static void throw_payload() { Noisy a("payload"); throw Payload(5); }

/* ---- 6. deep recursion, so the walk is long ---------------------------- */
static void deep(int n)
{
    Noisy a("deep");
    if (n == 0) throw 99;
    deep(n - 1);
}

int main()
{
    printf("1 catch several frames up\n");
    try { level1(); }
    catch (int x) { printf("  caught int %d\n", x); }

    printf("2 through a C frame\n");
    try { c_middle(throws_int); }
    catch (int x) { printf("  caught int %d\n", x); }

    printf("3 by base class\n");
    try { throw_derived(); }
    catch (const Base &b) { printf("  caught %s as Base\n", b.who()); }

    printf("4 rethrow\n");
    try { inner_rethrow(); }
    catch (int x) { printf("  outer caught int %d\n", x); }

    printf("5 a payload with a destructor\n");
    try { throw_payload(); }
    catch (const Payload &p) { printf("  caught payload %d\n", p.v); }

    printf("6 catch-all, and the type it did not match\n");
    try { throw Other(3); }
    catch (int) { printf("  WRONG: int\n"); }
    catch (...) { printf("  caught by ...\n"); }

    printf("7 deep\n");
    try { deep(12); }
    catch (int x) { printf("  caught int %d\n", x); }

    printf("8 nested try, inner does not match\n");
    try {
        try { Noisy a("nested"); throw 33; }
        catch (const Base &) { printf("  WRONG: Base\n"); }
    } catch (int x) { printf("  outer caught int %d\n", x); }

    printf("9 an exception that escapes a loop\n");
    try {
        for (int i = 0; i < 3; i++) {
            Noisy a("loop");
            if (i == 2) throw 44;
        }
    } catch (int x) { printf("  caught int %d\n", x); }

    printf("10 destructor order within one frame\n");
    try { Noisy a("first"); Noisy b("second"); Noisy c("third"); throw 55; }
    catch (int x) { printf("  caught int %d\n", x); }

    (void)depth;
    printf("done\n");
    return 42;
}
