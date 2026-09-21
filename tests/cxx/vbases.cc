// CX3b: virtual base classes — one shared subobject however it is
// reached, laid out after the non-virtual part; built once, first, by the
// most derived class (its mem-initializers win), destroyed last;
// conversions and member access through the vtable's vbase offsets;
// virtual calls through virtual thunks (vcall offsets); construction
// vtables (a virtual call while a base is built reaches that base's
// override); nearly empty virtual bases sharing the vptr, including a
// "lost" primary (the interface pattern); a virtual base with virtual
// bases of its own; copies and assignment; dynamic_cast and typeid.
// expect-exit: 42
#include <stdio.h>
#include <string.h>

// std::type_info as libstdc++'s <typeinfo> declares it (CX8 brings it)
namespace std {
class type_info {
public:
    virtual ~type_info();
    const char *name() const { return __name[0] == '*' ? __name + 1 : __name; }
protected:
    const char *__name;
};
}

static int fails;
static void check(const char *what, bool ok)
{
    if (!ok)
        fails++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

static char trail[256];
static void T(const char *s) { strcat(trail, s); }

// ---- a diamond ----
struct A {
    int a;
    A(int x = 1) : a(x) { T("A"); }
    virtual int f() { return a; }
    virtual ~A() { T("~A"); }
};
struct B : virtual A {
    int b = 2;
    B() { T("B"); }
    int f() override { return b * 10 + a; }
    ~B() { T("~B"); }
};
struct C : virtual A {
    int c = 3;
    C() { T("C"); }
    virtual int g() { return c + a; }
    ~C() { T("~C"); }
};
struct D : B, C {
    int d = 4;
    D() : A(7) { T("D"); }
    int f() override { return d * 100 + a; }
    ~D() { T("~D"); }
};

// ---- no virtual functions at all ----
struct V0 { int v; V0() : v(9) {} };
struct X0 : virtual V0 { int x = 1; int vx() { return v; } };
struct Y0 : virtual V0 { int y = 2; };
struct Z0 : X0, Y0 { int z = 3; };

// ---- interfaces: nearly empty virtual bases share the vptr ----
struct IFace {
    virtual int id() const = 0;
    virtual ~IFace() {}
};
struct INamed : virtual IFace { virtual const char *nm() const = 0; };
struct ICount : virtual IFace { virtual int count() const = 0; };
struct Impl : INamed, ICount {          // ICount's primary is lost
    int n = 7;
    int id() const override { return 42; }
    const char *nm() const override { return "impl"; }
    int count() const override { return n; }
};
static int use_face(const IFace &f) { return f.id(); }

// ---- a virtual base with a virtual base of its own ----
struct W {
    int w;
    W(int x = 1) : w(x) { T("W"); }
    virtual int who() { return w; }
    virtual ~W() { T("~W"); }
};
struct V : virtual W {
    int v = 2;
    V() { T("V"); }
    int who() override { return 100 + v; }
    ~V() { T("~V"); }
};
struct P : virtual V { int p = 3; P() { T("P"); } ~P() { T("~P"); } };
struct Q : virtual V {
    int q = 4;
    Q() : W(9) { T("Q"); }
    virtual int qq() { return q + w; }
    ~Q() { T("~Q"); }
};
struct R : P, Q {
    int r = 5;
    R() : W(11) { T("R"); }
    int qq() override { return r * 1000 + w; }
    ~R() { T("~R"); }
};

// ---- while a base is built, its own overrides are the ones called ----
struct CB {
    virtual const char *what() { return "CB"; }
    virtual ~CB() {}
};
struct CM : virtual CB {
    const char *seen;
    CM() { seen = what(); }
    const char *what() override { return "CM"; }
};
struct CD : CM {
    const char *what() override { return "CD"; }
};

// ---- copies and assignment ----
struct Val { int k; Val(int x = 0) : k(x) {} };
struct CV1 : virtual Val { int a = 1; CV1() : Val(3) {} };
struct CV2 : virtual Val { int b = 2; };
struct CVD : CV1, CV2 { int c = 5; CVD() : Val(8) {} };
static int by_value(CVD v) { return v.k * 10 + v.c; }

int main()
{
    {
        D d;
        A *pa = &d;
        B *pb = &d;
        C *pc = &d;
        check("virtual base built first, by the most derived class",
              strcmp(trail, "ABCD") == 0 && d.a == 7);
        check("one shared subobject", pa->f() == 407 && pb->f() == 407 &&
                                      pc->f() == 407 && pc->g() == 10 &&
                                      pb->a == 7 && pc->a == 7);
        check("layout: after the non-virtual part",
              (char *)pa - (char *)&d == 32 && (char *)pc - (char *)&d == 16 &&
              sizeof(D) == 48 && sizeof(B) == 32);
        check("dynamic_cast from a virtual base",
              dynamic_cast<D *>(pa) == &d && dynamic_cast<C *>(pa) == pc &&
              dynamic_cast<B *>(pc) == pb);
        check("typeid through a virtual base",
              strcmp(typeid(*pa).name(), "1D") == 0);
        trail[0] = 0;
    }
    check("virtual base destroyed last", strcmp(trail, "~D~C~B~A") == 0);
    trail[0] = 0;
    A *h = new D;
    trail[0] = 0;
    delete h;
    check("deleting through a virtual base", strcmp(trail, "~D~C~B~A") == 0);

    Z0 z;
    check("virtual bases without virtual functions",
          z.v == 9 && z.vx() == 9 && ((Y0 &)z).v == 9 && sizeof(Z0) == 40);

    Impl im;
    INamed *pn = &im;
    ICount *pk = &im;
    IFace *pf = pk;
    check("interfaces: nearly empty virtual bases",
          im.id() == 42 && strcmp(pn->nm(), "impl") == 0 && pk->count() == 7 &&
          pf->id() == 42 && use_face(im) == 42);
    check("... sharing the vptr", sizeof(Impl) == 24 && (char *)pf == (char *)&im &&
                                  (char *)pk - (char *)&im == 8);

    trail[0] = 0;
    {
        R r;
        check("a virtual base's own virtual base: built first",
              strcmp(trail, "WVPQR") == 0 && r.w == 11);
        W *pw = &r;
        Q *pq = &r;
        check("... and called through", r.who() == 102 && pw->who() == 102 &&
                                        pq->qq() == 5011 && sizeof(R) == 64);
        trail[0] = 0;
    }
    check("... and destroyed last", strcmp(trail, "~R~Q~P~V~W") == 0);

    CD cd;
    check("construction vtables", strcmp(cd.seen, "CM") == 0 &&
                                  strcmp(cd.what(), "CD") == 0);

    CVD a;
    a.k = 20;
    a.a = 21;
    CVD b(a);
    CVD c;
    c = a;
    check("copying with virtual bases", b.k == 20 && b.a == 21 && b.b == 2);
    check("assigning with virtual bases", c.k == 20 && c.a == 21 && c.c == 5);
    check("passing by value", by_value(a) == 205);
    int Val::*mk = &Val::k;
    int CV1::*ma = &CV1::a;
    check("pointers to members of a virtual base", a.*mk == 20 && a.*ma == 21);

    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 42;
}
