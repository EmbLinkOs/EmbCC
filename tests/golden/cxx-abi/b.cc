// Side B of cxx-abi.sh (compiled by the reference g++).
#include "abi.h"
#include <stdarg.h>
#include <typeinfo>

namespace abi {
namespace detail {

int mix(short a, unsigned short b, long long c, unsigned long long d, bool e)
{
    return a + b + (int)c + (int)d + e;
}
double fl(float a, double b, long double c) { return a + b + (double)c; }
int str(const char *a, char *b, const char *const *c)
{
    int n = 0;
    while (a[n]) n++;
    int m = 0;
    while (b[m]) m++;
    int k = 0;
    for (int i = 0; i < 2; i++)
        for (const char *s = c[i]; *s; s++)
            k++;
    return n + m + k;
}
int refs(int &a, const int &b, int &&c, Pod &d, const Pod &e)
{
    int r = a + b + c + d.a + e.a;
    a += 10;
    return r;
}
int ptrs(int *a, int **b, const int *c, int *const *d, void *e, const void *f)
{
    return (a == *b) + (c == (const int *)e) + (*d == (const int *)f) +
           (**b == 11) + (*c == 2) + 1;
}
int same(Pod *a, Pod *b, Pod &c, const Pod *d) { return a->a + b->a + c.a + d->a; }
int fn(int (*f)(int), void (*g)(), int (*h)(Pod &, Pod &))
{
    Pod x = { 1, 0, 0 }, y = { 4, 0, 0 };
    g();
    return f(3) + 1 + h(x, y);
}
int arr(int (*a)[4], int (&b)[3])
{
    return (*a)[0] + (*a)[1] + (*a)[2] + (*a)[3] + b[0] + b[1] + b[2];
}
int en(Color a, Small b, Color *c) { return a + (int)b + (*c == Blue ? 2 : 0); }
int nul(decltype(nullptr)) { return 42; }
int chars(wchar_t a, char16_t b, char32_t c, char8_t d) { return a + b + c + d; }
int var(int n, ...)
{
    va_list ap;
    va_start(ap, n);
    int s = 0;
    for (int i = 0; i < n; i++)
        s += va_arg(ap, int);
    va_end(ap);
    return s;
}
int nested(Account::Entry *e, Account::Entry &f)
{
    int s = 0;
    for (; e; e = e->next)
        s += e->amount;
    return s + f.amount;
}
long pod_sum(Pod p) { return p.a + p.b + p.c; }

Buf make_buf(int n) { return Buf(n); }

Shape *make_circle(int r) { return new Circle(r); }
int shape_area(const Shape &s) { return s.area(); }
long tagged_tag(const Tagged *t) { return t->tag(); }
Circle *as_circle(Shape *s) { return dynamic_cast<Circle *>(s); }
const char *type_name(const Shape &s) { return typeid(s).name(); }
long layout_code()
{
    TailUser tu;
    return (long)((char *)&tu.d - (char *)&tu) * 1000000 +
           (long)sizeof(TailUser) * 10000 + (long)sizeof(EboUser) * 100 +
           (long)sizeof(Both);
}
int gxx_unwound = 0;
struct Unwound { ~Unwound() { gxx_unwound++; } };
int gxx_catches(void (*f)(int), int k)
{
    Unwound u;
    try {
        Unwound v;
        f(k);
    } catch (const AbiErr &e) {
        return e.code;
    } catch (int i) {
        return -i;
    }
    return 0;
}
void gxx_throws(int k)
{
    Unwound u;
    if (k > 0)
        throw AbiErr(k);
    throw k;
}
VBase *make_join2() { return new Join2; }
int vbase_who(const VBase &v) { return v.who(); }
int right_of(const Right &r) { return r.right(); }
Join *as_join(VBase *v) { return dynamic_cast<Join *>(v); }
long vb_layout()
{
    Join j;
    return (long)sizeof(Join) * 1000000 +
           (long)((char *)(VBase *)&j - (char *)&j) * 1000 +
           (long)((char *)(Right *)&j - (char *)&j);
}
int via(const Pt *p, int (Pt::*f)(int) const, int Pt::*d)
{
    return (p->*f)(p->*d) + p->*member_of(1);
}
int consume(Buf b, int k)
{
    b += k;                    // the callee's own copy: the caller's stays
    return (int)b;
}
int b_checks()
{
    int base = Buf::live;
    Buf x(2);                  // {1, 2}
    Buf y = twice(x);          // embcc: {1, 2, 1, 2}
    int ok = 0;
    ok += y.size() == 4;
    ok += y.sum() == 6;
    ok += (x + y).size() == 6;
    ok += consume(y, 1) == 10;
    ok += y == twice(x);
    ok += Buf::live == base + 2;
    ok += (int)(y += 1) == 10;
    return ok;
}

}
}

namespace std {
int std_name(int x) { return x + 1; }
int two_holders(Holder<int> a, Holder<char> b)
{
    Holder<long> l = a;              // side A's conversion
    return (int)l.v + b.v;
}
}

int global_fn(abi::Pod *a, abi::Pod *b) { return a->a + b->a; }

int pmf::Pm::v(int x) const { return x + k; }
int pmf::Pm::nv(int x) const { return x * k; }
pmf::Pm::~Pm() {}
int pmf::Pd::v(int x) const { return x - k; }
int pmf::call(const Pm &o, F f, int x) { return (o.*f)(x); }
pmf::F pmf::pick(int which)
{
    return which == 0 ? &Pm::v : which == 1 ? &Pm::nv : nullptr;
}
bool pmf::same(F a, F b) { return a == b; }

tags::T tags::make_t(int x) { return T{x}; }
tags::W tags::wrap(int x) { return W{x * 2}; }
tags::T tags::tvar = { 5 };
tags::T tags::S::get() const { return T{7}; }
int tags::tags_b()
{
    static W w;
    return make_w(3).x + take(T{4}, W{5}) + (wptr(T{1}) == &w) + S().own();
}

abi::Right::Right() {}
abi::Right::~Right() { vb_trail = vb_trail * 10 + 2; }
int abi::Right::right() const { return r + base; }
abi::Join2::Join2() {}
abi::Join2::~Join2() { vb_trail = vb_trail * 10 + 4; }
int abi::Join2::right() const { return 40 + k; }

abi::Circle::~Circle() {}                           // Circle's key function
int abi::Circle::area() const { return r * r * 3; }
const char *abi::Circle::name() const { return "circle"; }

// B builds and uses the class side A defines
static int use_account()
{
    abi::Account acct("bob", 30);
    acct.deposit(10);
    abi::Account anon(1);
    return acct.balance() + anon.balance() + abi::Account::opened() * 0 +
           (abi::Account::open_count == 2) + (c_fn(1) == 3) +
           (abi::tag[0] == 's') - 3 + (abi::deep_call() - 42);
}
static int b_result = use_account();

namespace abi {
int deep_call_b() { return b_result; }
}
