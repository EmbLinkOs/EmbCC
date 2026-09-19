// CX5: exceptions — throw of scalars, structs, class objects and pointers;
// catch by value, by reference and by pointer, through base classes, in
// order, catch (...); rethrow; try inside a handler; destructors of the
// frames unwound, of temporaries, of a partly constructed object's
// members (not of the object); new's storage freed when the constructor
// throws; returning from try blocks and handlers.
// expect-exit: 42
#include <stdio.h>
#include <string.h>
static char trail[512];
static void T(const char *s) { strcat(trail, s); }
static int fails;
static void check(const char *what, bool ok) { if (!ok) fails++; printf("%s %s\n", ok ? "ok  " : "FAIL", what); }

struct G { const char *n; G(const char *s) : n(s) { T("+"); T(n); } ~G() { T("-"); T(n); } };
struct Err { int code; const char *msg; };
struct Base { int b = 1; virtual ~Base() {} virtual int kind() const { return 1; } };
struct Derived : Base { int d = 2; int kind() const override { return 2; } };

static void thrower(int k)
{
    G g("t");
    if (k == 1) throw 7;
    if (k == 2) throw Err{42, "err"};
    if (k == 3) throw Derived();
    if (k == 4) throw 2.5;
    if (k == 5) { static Derived dd; throw &dd; }
}
static int middle(int k) { G g("m"); thrower(k); return 0; }

struct Tmp { int v; Tmp(int x) : v(x) { T("<"); } ~Tmp() { T(">"); } int boom() const { throw v; } };

struct Part {
    G a, b, c;
    Part(bool fail) : a("a"), b("b"), c(fail ? (throw 3, "c") : "c") { T("P"); }
    ~Part() { T("~P"); }
};

static int news, dels;
struct Counted {
    int x;
    Counted(int k) : x(k) { if (k < 0) throw k; }
    static void *operator new(unsigned long n) { news++; return ::operator new(n); }
    static void operator delete(void *p) { dels++; ::operator delete(p); }
};

static int rethrow_it()
{
    try {
        try { throw Err{9, "inner"}; }
        catch (Err &e) { e.code += 1; throw; }
    } catch (const Err &e) { return e.code; }
    return 0;
}

static int in_handler()
{
    int r = 0;
    try { throw 1; }
    catch (int) {
        try { throw 2; } catch (int j) { r = j * 10; }
        r += 1;
    }
    return r;
}

static int value_in_try(int k)
{
    try { if (k) throw k; return 5; } catch (int v) { return v * 2; }
}

int main()
{
    int got = 0;
    try { middle(1); } catch (int v) { got = v; }
    check("catch int through frames, locals destroyed", got == 7 && strcmp(trail, "+m+t-t-m") == 0);
    trail[0] = 0;
    try { middle(2); } catch (const Err &e) { got = e.code; check("catch a struct by const reference", e.code == 42 && strcmp(e.msg, "err") == 0); }
    try { middle(3); } catch (Base &b) { check("catch derived by base reference", b.kind() == 2); }
    try { middle(4); } catch (int) { got = -1; } catch (double d) { check("the first matching handler", d == 2.5); } catch (...) { got = -2; }
    try { middle(1); } catch (...) { check("catch (...)", true); }
    try { middle(5); } catch (Base *p) { check("catch a pointer to base", p->kind() == 2); }
    try { middle(3); } catch (Derived d) { check("catch by value", d.d == 2 && d.kind() == 2); }
    trail[0] = 0;
    try { Tmp(5).boom(); } catch (int v) { check("temporaries destroyed", v == 5 && strcmp(trail, "<>") == 0); }
    trail[0] = 0;
    try { Part p(true); } catch (int v) { check("constructed members destroyed, not the object", v == 3 && strcmp(trail, "+a+b-b-a") == 0); }
    trail[0] = 0;
    { Part p(false); }
    check("... and all of it when it succeeds", strcmp(trail, "+a+b+cP~P-c-b-a") == 0);
    try { new Counted(-4); } catch (int v) { check("new: freed when the constructor throws", v == -4 && news == 1 && dels == 1); }
    check("rethrow", rethrow_it() == 10);
    check("try inside a handler", in_handler() == 21);
    check("returning from try and handler", value_in_try(0) == 5 && value_in_try(4) == 8);
    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 42;
}
