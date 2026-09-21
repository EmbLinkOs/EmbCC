// CX3: inheritance — base subobjects laid out as Itanium does (empty
// bases, tail padding of non-POD bases reused), conversions to bases,
// members found in bases, base construction and destruction order, and
// virtual functions: overriding, virtual destructors, pure virtuals,
// calls through base pointers, multiple inheritance with this-adjusting
// thunks, and virtual calls made while constructing and destroying.
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

// ---- layout ----
struct Pod { int x; char c; };
struct NonPod { int x; char c; NonPod() : x(0), c(0) {} };
struct Empty {};
struct D1 : Pod { char d; };
struct D2 : NonPod { char d; };
struct D3 : Empty { int x; };
struct D4 : Empty { Empty e; int x; };

// ---- non-virtual inheritance ----
struct Base {
    int b = 1;
    Base() { note("B"); }
    ~Base() { note("~B"); }
    int twice() const { return 2 * b; }
};
struct Mid : Base {
    int m = 10;
    Mid() { note("M"); }
    ~Mid() { note("~M"); }
};
struct Top : Mid {
    int t = 100;
    Top() { note("T"); }
    ~Top() { note("~T"); }
    int sum() const { return b + m + t; }
};

// ---- virtual functions ----
struct Shape {
    const char *tag;
    Shape(const char *t) : tag(t) { note(kind()); }
    virtual ~Shape() { note("~S"); }
    virtual const char *kind() const { return "shape"; }
    virtual int area() const = 0;
    int describe() const { return area() + 1000; }
};
struct Square : Shape {
    int side;
    Square(int s) : Shape("sq"), side(s) {}
    ~Square() override { note("~Sq"); }
    const char *kind() const override { return "square"; }
    int area() const override { return side * side; }
};
struct Rect final : Square {
    int w;
    Rect(int s, int ww) : Square(s), w(ww) {}
    int area() const override { return side * w; }
};

// ---- multiple inheritance ----
struct Named {
    virtual ~Named() {}
    virtual const char *name() const { return "named"; }
    long id = 7;
};
struct Sized {
    virtual ~Sized() {}
    virtual long size() const { return 0; }
    long extra = 3;
};
struct File : Named, Sized {
    const char *name() const override { return "file"; }
    long size() const override { return 4096 + extra; }
};

int main()
{
    D1 d1;
    check("POD base: no tail reuse",
          (char *)&d1.d - (char *)&d1 == 8 && sizeof(D1) == 12);
    check("non-POD base: tail reused", sizeof(D2) == 8);
    check("empty base optimization", sizeof(D3) == 4 && sizeof(D4) == 8);

    trail[0] = 0;
    {
        Top t;
        check("members of bases", t.sum() == 111 && t.twice() == 2);
        Base *bp = &t;
        Mid &mr = t;
        check("conversions to bases", bp->b == 1 && mr.m == 10 &&
                                      static_cast<Top *>(bp) == &t);
        note("|");
    }
    check("bases built first, destroyed last",
          strcmp(trail, "BMT|~T~M~B") == 0);

    trail[0] = 0;
    Shape *s = new Rect(3, 5);
    check("virtual call through a base", s->area() == 15 &&
                                         s->describe() == 1015);
    check("overrider in the middle", strcmp(s->kind(), "square") == 0);
    delete s;
    check("constructing calls the base's version; delete runs every "
          "destructor", strcmp(trail, "shape~Sq~S") == 0);

    File f;
    Named *n = &f;
    Sized *z = &f;
    check("multiple inheritance: distinct subobjects",
          (void *)n != (void *)z && z->extra == 3 && n->id == 7);
    check("thunks adjust this", strcmp(n->name(), "file") == 0 &&
                                z->size() == 4099);
    Sized *dz = new File;
    delete dz;                               // through the thunked D0
    check("delete through a secondary base", true);

    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 42;
}
