// CX1: classes — constructors (default, with arguments, delegating, member
// initializers, default member initializers), destructors and the order of
// both, static and const members, nested types, arrays of objects, and the
// lifetime of temporaries.
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

static char trail[256];
static void note(const char *s) { strcat(trail, s); }

struct Log {
    char tag;
    Log(char t) : tag(t) { char s[3] = { '+', t, 0 }; note(s); }
    ~Log() { char s[3] = { '-', tag, 0 }; note(s); }
};

class Counter {
public:
    Counter() : n(0) {}
    explicit Counter(int start) : n(start) {}
    Counter(int a, int b) : Counter(a * b) {}          // delegating
    void add(int k = 1) { n += k; }
    int get() const { return n; }
    static int made;
    static int twice(int v) { return 2 * v; }
private:
    int n;
};
int Counter::made = 3;

struct Defaults {
    int a = 7;
    int b{8};
    int c;
    Defaults() : c(a + b) {}
    Defaults(int x) : a(x), c(a + b) {}
};

struct Holder {
    Log first;
    Log second;
    Holder() : second('b'), first('a') {}              // declaration order wins
};

struct Outer {
    struct Inner {
        int v;
        int twice() const { return v * 2; }
    };
    enum Kind { A = 1, B = 2 };
    Inner in;
    Kind k;
};

struct Big {
    long vals[4];
    int sum() const { return (int)(vals[0] + vals[1] + vals[2] + vals[3]); }
};

static Big make_big(long base)
{
    Big b = { { base, base + 1, base + 2, base + 3 } };
    return b;
}

struct Refs {
    int &r;
    const int &cr;
    Refs(int &x, const int &y) : r(x), cr(y) {}
};

struct Tracked {
    static int live;
    int id;
    Tracked() : id(++live) {}
    ~Tracked() { live--; }
};
int Tracked::live = 0;

static int use(const Log &l) { return l.tag; }

int main()
{
    Counter c0;
    Counter c1(5);
    Counter c2(3, 4);
    c0.add();
    c1.add(10);
    check("constructors", c0.get() == 1 && c1.get() == 15 && c2.get() == 12);
    check("static members", Counter::made == 3 && Counter::twice(21) == 42);
    Counter::made++;
    check("static data member is shared", Counter::made == 4);

    Defaults d0, d1(1);
    check("default member initializers", d0.a == 7 && d0.b == 8 &&
                                         d0.c == 15 && d1.a == 1 &&
                                         d1.c == 9);

    trail[0] = 0;
    {
        Holder h;
        note("|");
    }
    check("members: built in order, destroyed in reverse",
          strcmp(trail, "+a+b|-b-a") == 0);

    trail[0] = 0;
    {
        Log x('x');
        Log y('y');
        {
            Log z('z');
        }
        note("|");
    }
    check("locals destroyed in reverse", strcmp(trail, "+x+y+z-z|-y-x") == 0);

    trail[0] = 0;
    int t = use(Log('t'));
    note("|");
    check("a temporary lives to the end of its full-expression",
          t == 't' && strcmp(trail, "+t-t|") == 0);

    trail[0] = 0;
    {
        const Log &kept = Log('k');
        note("|");
        check("a temporary bound to a reference lives as long",
              kept.tag == 'k' && strcmp(trail, "+k|") == 0);
    }
    check("... and dies with it", strcmp(trail, "+k|-k") == 0);

    Outer o;
    o.in.v = 21;
    o.k = Outer::B;
    Outer::Inner in2 = { 4 };
    check("nested types", o.in.twice() == 42 && o.k == 2 && in2.v == 4 &&
                          Outer::A == 1);

    check("class returned by value", make_big(10).sum() == 46);

    int x = 1, y = 2;
    Refs rf(x, y);
    rf.r = 41;
    check("reference members", x == 41 && rf.cr == 2);

    {
        Tracked arr[3];
        check("array of objects constructed in order",
              Tracked::live == 3 && arr[0].id == 1 && arr[2].id == 3);
    }
    check("array of objects destroyed", Tracked::live == 0);

    Tracked *many = new Tracked[4];
    check("new[] of objects", Tracked::live == 4 && many[3].id == 4);
    delete[] many;
    check("delete[] of objects", Tracked::live == 0);

    check("sizeof a class", sizeof(Counter) == sizeof(int) &&
                            sizeof(Big) == 4 * sizeof(long) &&
                            sizeof(Tracked) == sizeof(int));
    struct Empty {};
    check("an empty class is one byte", sizeof(Empty) == 1);

    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 42;
}
