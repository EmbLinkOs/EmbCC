// Side A of cxx-abi.sh (compiled by embcc): defines the class and some of
// the functions; calls everything side B (g++) defines.
#include "abi.h"
#include <stdio.h>
#include <string.h>

namespace abi {

int Account::open_count = 0;

Account::Account(int start) : total(start), owner("anon") { open_count++; }
Account::Account(const char *name, int start) : total(start), owner(name)
{
    open_count++;
}
Account::~Account() { open_count--; }
void Account::deposit(int n) { total += n; }
int Account::balance() const { return total; }
int Account::opened() { return open_count; }

int Buf::live = 0;
Buf::Buf(int k) : n(k), data(new int[k])
{
    for (int i = 0; i < k; i++)
        data[i] = i + 1;
    live++;
}
Buf::Buf(const Buf &o) : n(o.n), data(new int[o.n])
{
    for (int i = 0; i < n; i++)
        data[i] = o.data[i];
    live++;
}
Buf::~Buf()
{
    delete[] data;
    live--;
}
int Buf::size() const { return n; }
int Buf::sum() const
{
    int s = 0;
    for (int i = 0; i < n; i++)
        s += data[i];
    return s;
}
Buf Buf::operator+(const Buf &o) const
{
    Buf r(n + o.n);
    for (int i = 0; i < n; i++)
        r.data[i] = data[i];
    for (int i = 0; i < o.n; i++)
        r.data[n + i] = o.data[i];
    return r;
}
Buf &Buf::operator+=(int k)
{
    for (int i = 0; i < n; i++)
        data[i] += k;
    return *this;
}
Buf::operator int() const { return sum(); }
bool Buf::operator==(const Buf &o) const { return sum() == o.sum(); }

Buf twice(const Buf &b) { return b + b; }

Shape::~Shape() {}                                   // the key function
const char *Shape::name() const { return "shape"; }
Tagged::~Tagged() {}
long Tagged::tag() const { return t; }
int Both::area() const { return 7; }
long Both::tag() const { return t + 1; }

int AbiErr::live = 0;
AbiErr::AbiErr(int c) : code(c) { live++; }
AbiErr::AbiErr(const AbiErr &o) : code(o.code) { live++; }
AbiErr::~AbiErr() { live--; }

static int emb_unwound;
struct EmbGuard { ~EmbGuard() { emb_unwound++; } };
static void emb_throw(int k)
{
    EmbGuard g;
    if (k > 0)
        throw AbiErr(k);
    throw k;
}

long vb_trail = 0;
VBase::~VBase() { vb_trail = vb_trail * 10 + 9; }
int VBase::who() const { return base; }
Left::Left() {}
Left::~Left() { vb_trail = vb_trail * 10 + 1; }
int Left::who() const { return 10 + l; }
Join::Join() {}
Join::~Join() { vb_trail = vb_trail * 10 + 3; }
int Join::who() const { return 30 + j; }

namespace detail {
int Pt::*member_of(int which) { return which ? &Pt::y : &Pt::x; }
int (Pt::*method())() const { return &Pt::sum; }
}

int counter = 40;
const char *tag = "side-a";

int deep_call() { return counter + 2; }

namespace detail {
int mix(int a, long b, unsigned c, char d, signed char e, unsigned char f)
{
    return a + (int)b + (int)c + d + e + f;
}
Pod make_pod(int k)
{
    Pod p = { k, k * 2L, (char)(k + 1) };
    return p;
}
}

}

extern "C" int c_fn(int x) { return x * 3; }

template <class T> std::Holder<T>::operator Holder<long>() const
{
    return Holder<long>{(long)v * 2};
}
template struct std::Holder<int>;

tags::W tags::make_w(int x) { return W{x}; }
int tags::take(T t, W w) { return t.x * w.x; }
tags::W *tags::wptr(T) { return nullptr; }
int tags::S::own() { return 100; }

static int sq(int x) { return x * x; }
static void nothing() {}
static int podcmp(abi::Pod &a, abi::Pod &b) { return a.a - b.a; }

int main()
{
    using namespace abi;
    using namespace abi::detail;
    int fails = 0;
#define CHECK(what, cond) do { int ok_ = (cond); fails += !ok_; \
        printf("%s %s\n", ok_ ? "ok  " : "FAIL", what); } while (0)

    CHECK("mix #2 (g++)", mix((short)1, (unsigned short)2, 3LL, 4ULL, true) == 11);
    CHECK("floats", fl(1.5f, 2.5, 3.0L) == 7.0);
    const char *list[] = { "x", "yz" };
    char buf[4] = "abc";
    CHECK("strings", str("hello", buf, list) == 5 + 3 + 3);
    int i = 1, j = 2;
    Pod p = { 1, 2, 3 }, q = { 4, 5, 6 };
    CHECK("references", refs(i, j, 30, p, q) == 1 + 2 + 30 + 1 + 4 && i == 11);
    int *pi = &i;
    CHECK("pointers", ptrs(&i, &pi, &j, &pi, &j, &i) == 6);
    CHECK("substitutions", same(&p, &q, p, &q) == 1 + 4 + 1 + 4);
    CHECK("function pointers", fn(sq, nothing, podcmp) == 9 + 1 + (1 - 4));
    int a4[4] = { 1, 2, 3, 4 }, a3[3] = { 5, 6, 7 };
    CHECK("arrays", arr(&a4, a3) == 10 + 18);
    Color c = Blue;
    CHECK("enums", en(Green, Small::B, &c) == 1 + 200 + 2);
    CHECK("nullptr_t", nul(nullptr) == 42);
    CHECK("character types", chars(L'a', u'b', U'c', (char8_t)'d') ==
                             'a' + 'b' + 'c' + 'd');
    CHECK("variadic", var(3, 10, 20, 30) == 60);
    Account::Entry e2 = { 5, nullptr }, e1 = { 7, &e2 };
    CHECK("nested class", nested(&e1, e2) == 7 + 5 + 5);
    CHECK("class by value", pod_sum(make_pod(10)) == 10 + 20 + 11);
    CHECK("std::", std::std_name(41) == 42);
    CHECK("std:: substitutions", std::two_holders(std::Holder<int>{20},
                                                  std::Holder<char>{2}) == 42);
    CHECK("global namespace", global_fn(&p, &q) == 5);
    CHECK("ABI tags: g++'s tagged names", tags::make_t(1).x == 1 &&
          tags::wrap(2).x == 4 && tags::tvar.x == 5 &&
          tags::S().get().x == 7);
    CHECK("ABI tags: embcc's, called by g++", tags::tags_b() == 3 + 20 + 100);
    {
        Buf b = make_buf(3);                       // g++ fills our slot
        CHECK("class returned through the slot", b.size() == 3 && b.sum() == 6);
        CHECK("class passed by reference to a copy", consume(b, 10) == 36 &&
                                                     b.sum() == 6);
        Buf c = b + make_buf(2);                   // member with a slot
        CHECK("operator+ returning a class", c.size() == 5 && (int)c == 9);
        c += 1;
        CHECK("operator+= and operator int", (int)c == 14);
        CHECK("g++ calling embcc's slot functions", b_checks() == 7);
    }
    CHECK("every Buf destroyed", Buf::live == 0);
    {
        Shape *c = make_circle(3);                   // g++'s vtable
        CHECK("calling g++'s virtuals", c->area() == 27 &&
              strcmp(c->name(), "circle") == 0 && c->id == 1);
        Both b;
        CHECK("g++ calling embcc's virtuals", shape_area(b) == 7 &&
              tagged_tag(&b) == 101);
        CHECK("dynamic_cast by g++ on embcc's typeinfo",
              as_circle(c) == static_cast<Circle *>(c) &&
              as_circle(&b) == nullptr);
        CHECK("typeinfo names agree",
              strcmp(type_name(b), "N3abi4BothE") == 0 &&
              strcmp(type_name(*c), "N3abi6CircleE") == 0);
        delete c;                                    // g++'s D0
        Tagged *tp = new Both;
        delete tp;                                   // embcc's thunk to D0
        TailUser tu;
        EboUser eu;
        long mine = (long)((char *)&tu.d - (char *)&tu) * 1000000 +
                    (long)sizeof(TailUser) * 10000 + (long)sizeof(EboUser) * 100 +
                    (long)sizeof(Both);
        CHECK("layouts agree", layout_code() == mine && sizeof(eu) == 4);
    }
    {
        vb_trail = 0;
        {
            Join jn;                                 // g++'s Right inside
            VBase *pb = &jn;
            Left *pl = &jn;
            Right *pr = &jn;
            CHECK("virtual bases: embcc's class over g++'s",
                  pb->who() == 33 && pl->who() == 33 && vbase_who(jn) == 33 &&
                  pr->right() == 7 && right_of(jn) == 7 && jn.base == 5 &&
                  pr->base == 5 && pl->l == 1 && pr->r == 2);
            CHECK("g++'s dynamic_cast from a virtual base",
                  as_join(pb) == &jn && as_join(pr) == &jn);
            long mine = (long)sizeof(Join) * 1000000 +
                        (long)((char *)pb - (char *)&jn) * 1000 +
                        (long)((char *)pr - (char *)&jn);
            CHECK("virtual base layouts agree", vb_layout() == mine);
            vb_trail = 0;                            // g++'s Join is gone
        }
        CHECK("... destroyed in order through both", vb_trail == 3219);
        vb_trail = 0;
        VBase *g = make_join2();                     // embcc's Left inside
        Right *gr = dynamic_cast<Right *>(g);
        CHECK("g++'s class over embcc's", g->who() == 11 && gr &&
              gr->right() == 44 && dynamic_cast<Left *>(g)->l == 1 &&
              g->base == 5);
        delete g;                                    // g++'s D0
        CHECK("... destroyed in order through both", vb_trail == 4129);
        Right r;
        CHECK("g++'s constructor, complete", r.right() == 7 && r.who() == 5);
    }
    {
        using namespace detail;
        CHECK("g++ catches embcc's exceptions",
              gxx_catches(emb_throw, 5) == 5 && gxx_catches(emb_throw, -3) == 3 &&
              emb_unwound == 2 && gxx_unwound == 4 && AbiErr::live == 0);
        int got = 0;
        try {
            EmbGuard g;
            gxx_throws(7);
        } catch (const AbiErr &e) {
            got = e.code;
        }
        try {
            gxx_throws(-8);
        } catch (int i) {
            got += i * 100;
        }
        CHECK("embcc catches g++'s, unwinding both",
              got == 7 - 800 && emb_unwound == 3 && gxx_unwound == 6 &&
              AbiErr::live == 0);
    }
    Pt pt = { 3, 4 };
    CHECK("member pointers to and from g++",
          via(&pt, &Pt::mul, &Pt::y) == 7 * 4 + 4 && (pt.*method())() == 7);
    CHECK("g++ built Accounts with embcc's constructors", deep_call_b() == 41);
    CHECK("... and destroyed them", Account::opened() == 0);
    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 42;
}
