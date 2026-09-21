// CX2: copy and move — user-written and implicit (memberwise) copy/move
// constructors and assignments, the named return value built in the
// caller's slot, guaranteed elision of prvalues, classes that are not
// trivially copyable passed and returned by value (Itanium: by reference to
// the caller's temporary, and through the return slot), and std::move's
// static_cast. Every construction, copy, move and destruction is logged, so
// agreeing with g++ means doing exactly the same ones.
// expect-exit: 42
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int fails;
static void check(const char *what, bool ok)
{
    if (!ok)
        fails++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

static int ctors, copies, moves, cassigns, massigns, dtors;

class Str {
public:
    Str(const char *s = "") : n(strlen(s)), p((char *)malloc(n + 1))
    {
        memcpy(p, s, n + 1);
        ctors++;
    }
    Str(const Str &o) : n(o.n), p((char *)malloc(o.n + 1))
    {
        memcpy(p, o.p, n + 1);
        copies++;
    }
    Str(Str &&o) : n(o.n), p(o.p)
    {
        o.p = nullptr;
        o.n = 0;
        moves++;
    }
    Str &operator=(const Str &o)
    {
        if (this != &o) {
            char *q = (char *)malloc(o.n + 1);
            memcpy(q, o.p, o.n + 1);
            free(p);
            p = q;
            n = o.n;
        }
        cassigns++;
        return *this;
    }
    Str &operator=(Str &&o)
    {
        if (this != &o) {
            free(p);
            p = o.p;
            n = o.n;
            o.p = nullptr;
            o.n = 0;
        }
        massigns++;
        return *this;
    }
    ~Str()
    {
        free(p);
        dtors++;
    }
    const char *c_str() const { return p ? p : "(moved)"; }
    unsigned long size() const { return n; }
    Str operator+(const Str &o) const
    {
        Str r;
        free(r.p);
        r.n = n + o.n;
        r.p = (char *)malloc(r.n + 1);
        memcpy(r.p, p, n);
        memcpy(r.p + n, o.p, o.n + 1);
        return r;                               // the named return value
    }
private:
    unsigned long n;
    char *p;
};

// rule of zero: every special member implicit, and memberwise
struct Person {
    Str name;
    int age;
    Str city;
};

static Str make(const char *s) { return Str(s); }          // elided
static Str named(const char *s)
{
    Str r(s);
    return r;                                               // NRVO
}
static Str pick(bool first, const char *a, const char *b)
{
    Str x(a), y(b);
    if (first)
        return x;                                           // moved
    return y;
}
static unsigned long by_value(Str s) { return s.size(); }
static Str echo(Str s) { return s; }                       // moved out

static void report(const char *what)
{
    printf("%-16s c=%d cp=%d mv=%d ca=%d ma=%d d=%d\n", what, ctors, copies,
           moves, cassigns, massigns, dtors);
}

int main()
{
    {
        Str a("abc");
        Str b = a;
        Str c = static_cast<Str &&>(a);
        check("copy and move construction", strcmp(b.c_str(), "abc") == 0 &&
              strcmp(c.c_str(), "abc") == 0 && a.size() == 0);
        a = b;
        b = static_cast<Str &&>(c);
        check("copy and move assignment", strcmp(a.c_str(), "abc") == 0 &&
              c.size() == 0 && b.size() == 3);
        a = a;
        check("self-assignment", strcmp(a.c_str(), "abc") == 0);
    }
    report("basic");

    {
        Str m = make("made");
        Str n = named("named");
        check("elision and NRVO", strcmp(m.c_str(), "made") == 0 &&
              strcmp(n.c_str(), "named") == 0);
    }
    report("returns");

    {
        Str p = pick(true, "x", "y");
        check("return of a local moves", strcmp(p.c_str(), "x") == 0);
        Str q = Str("ab") + Str("cd");
        check("operator+ by value", strcmp(q.c_str(), "abcd") == 0);
    }
    report("moves");

    {
        Str s("four");
        unsigned long k = by_value(s);
        unsigned long t = by_value(Str("tmp!!"));
        Str e = echo(s);
        check("pass by value", k == 4 && t == 5 &&
              strcmp(e.c_str(), "four") == 0);
    }
    report("by value");

    {
        Person p1 = { Str("ann"), 30, Str("oslo") };
        Person p2 = p1;
        Person p3 = static_cast<Person &&>(p1);
        check("implicit copy and move are memberwise",
              strcmp(p2.name.c_str(), "ann") == 0 && p3.age == 30 &&
              strcmp(p3.city.c_str(), "oslo") == 0 && p1.name.size() == 0);
        p1 = p2;
        check("implicit copy assignment", strcmp(p1.city.c_str(), "oslo") == 0);
    }
    report("members");

    check("every object destroyed",
          ctors + copies + moves == dtors);
    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 42;
}
