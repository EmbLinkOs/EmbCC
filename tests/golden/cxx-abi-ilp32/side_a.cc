// Side A of the C++ interop test (abi.h says what each side defines), and
// the program: main prints a line per property.
#include <stdio.h>
#include <stdlib.h>
#include "abi.h"

// The replaceable array form of operator new, so a test can see what a
// new-expression asks for (abi.h's ABI_FACTS: the cookie); every unit's
// new[] comes here, whichever compiler built it.
size_t abi::last_array_new;
void *operator new[](size_t n)
{
    abi::last_array_new = n;
    return malloc(n);
}
static const void *volatile kept;
void abi::keep(const void *p) { kept = p; }
void operator delete[](void *p) noexcept { free(p); }
void operator delete[](void *p, size_t) noexcept { free(p); }

namespace abi {

// ---- virtual calls ----
static int g_destroyed;
Shape::Shape(int i) : id(i) {}
Shape::~Shape() { g_destroyed++; }
long Shape::area() const { return 0; }
int destroyed_shapes() { return g_destroyed; }

namespace {
struct Circle : Shape {
    long r;
    Circle(int i, long rr) : Shape(i), r(rr) {}
    long area() const override { return 3 * r * r; }
    const char *name() const override { return "circle"; }
};
}
Shape *make_circle(int id, long r) { return new Circle(id, r); }

Source::~Source() {}
Pipe::Pipe() : head(0), n(0) {}
Pipe::~Pipe() {}
int Pipe::get() { int v = buf[head]; head = (head + 1) % 4; n--; return v; }
int Pipe::put(int v) { buf[(head + n) % 4] = v; n++; puts++; return n; }

int Node::value() const { return tag; }
Diamond::Diamond() { tag = 50; }

// ---- mangling ----
long by_ref(const long &a, long *const b, int (&arr)[4])
{
    *b += a;
    return arr[0] + arr[3] + *b;
}
bool Money::operator<(const Money &o) const { return cents < o.cents; }
long sum_va(int n, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, n);
    long r = vsum(n, ap);
    __builtin_va_end(ap);
    return r;
}
long apply_binop(binop f, long a, long b) { return f(a, b); }

// ---- member pointers ----
Calc::Calc(int b) : base(b) {}
Calc::~Calc() {}
int Calc::add(int x) { return base + x; }
int Calc::scale(int x) { return base * x; }
int call_op(Calc *c, Op op, int x) { return (c->*op)(x); }
int call_op2(Calc2 *c, Op2 op, int x) { return (c->*op)(x); }
bool same_op(Op a, Op b) { return a == b; }

// ---- static initialization ----
static int g_events;
int record(int e) { g_events++; return e; }
int events() { return g_events; }
static int g_regs;
Reg::Reg(int w) : who(w + g_regs++) { record(w); }
Reg::~Reg() {}
Reg reg_a(10);

// ---- new[] / delete[] ----
int Elem::alive;
Elem::Elem() : v(alive++) {}
Elem::~Elem() { alive--; }
Elem *make_elems(int n) { return new Elem[n]; }
void free_wide(long long *p) { delete[] p; }

// ---- class values ----
long sum_big(Big b)
{
    long s = 0;
    for (int i = 0; i < 5; i++)
        s += b.a[i];
    return s;
}

Square *new_square_from_a(int id, long side) { return new Square(id, side); }

void facts_a(long *out) { ABI_FACTS(out); }

}  // namespace abi

using namespace abi;

int main()
{
    Shape *s[3] = { make_circle(1, 2), new Square(2, 5),
                    new_square_from_a(3, 4) };
    printf("areas %ld %ld %ld total %ld\n", s[0]->area(), s[1]->area(),
           s[2]->area(), total_area(s, 3));
    printf("names %s %s ids %d %d %d\n", s[0]->name(), s[1]->name(),
           s[0]->id, s[1]->id, s[2]->id);
    for (int i = 0; i < 3; i++)
        delete s[i];
    printf("destroyed %d\n", destroyed_shapes());

    Pipe p;
    for (int i = 1; i <= 3; i++)
        p.put(i * 7);
    Sink *dbl = make_doubler();
    printf("pump %d puts %d sink puts %d\n", pump(&p, dbl, 3), p.puts,
           dbl->puts);
    Source *src = &p;
    Sink *snk = &p;
    snk->put(99);
    printf("through bases %d %d\n", src->get(), p.n);
    delete dbl;

    Diamond dm;
    Node *nd = &dm;
    printf("diamond %d %d\n", diamond_sum(&dm), nd->value());

    long r = detail::mix(4000000000u, -5, -6L, 7UL, L'w', u'x', U'y', -9,
                         250, -300, -12345678901LL, 9876543210ULL, 1.5f,
                         2.25, true);
    long b = 10;
    int arr[4] = { 1, 2, 3, 4 };
    printf("mix %ld by_ref %ld %ld\n", r, by_ref(5L, &b, arr), b);
    printf("twice %d %ld %u\n", twice(21), twice(-4000L), twice(7u));
    printf("va %ld\n", sum_va(4, 10L, -3L, 100L, 7L));
    Money m1 = { 250 }, m2 = { 175 };
    Money m3 = m1 + m2;
    printf("money %ld less %d %d\n", m3.cents, m2 < m1, m1 < m2);
    printf("binop %ld %ld\n", apply_binop(pick_binop(0), 6, 7),
           apply_binop(pick_binop(1), 6, 7));

    Calc c(6);
    Calc2 c2(5);
    Op add = pick_op(0), scl = pick_op(1);
    printf("pmf %d %d %d %d\n", call_op(&c, add, 4), call_op(&c, scl, 4),
           call_op(&c2, add, 4), call_op(&c2, scl, 4));
    Op local_scl = &Calc::scale, local_add = &Calc::add;
    printf("pmf same %d %d %d null %d\n", same_op(scl, local_scl),
           same_op(add, local_add), same_op(add, scl),
           same_op(nullptr, nullptr));
    printf("pmf widened %d %d\n", call_op2(&c2, widen_op(add), 3),
           call_op2(&c2, widen_op(scl), 3));
    int Calc::*f = pick_field();
    printf("pdm %d %d\n", c.*f, c2.*f);

    printf("statics regs %d %d\n", reg_a.who + reg_b.who, events() >= 2);
    printf("static in b %d %d\n", static_in_b(), static_in_b());
    int s1 = shared_count(), s2 = shared_count_from_b(), s3 = shared_count();
    printf("shared %d %d %d\n", s1, s2, s3);

    Elem *e = make_elems(5);
    printf("elems alive %d last %d\n", Elem::alive, e[4].v);
    free_elems(e);
    printf("elems after %d\n", Elem::alive);
    long long *w = make_wide(3);
    printf("wide %lld\n", w[0] + w[1] + w[2]);
    free_wide(w);

    Big big = make_big(3);
    printf("big %ld\n", sum_big(big));
    Pair pr = swap_pair(Pair{ 7, -8 });
    printf("pair %d %lld\n", pr.x, pr.y);

    Calc *nc = new_calc_from_b(9);
    printf("new from b %d %d\n", nc->base, nc->scale(2));
    delete nc;
    long fa[NFACTS], fb[NFACTS];
    facts_a(fa);
    facts_b(fb);
    int agree = 0;
    for (int i = 0; i < NFACTS; i++) {
        if (fa[i] == fb[i])
            agree++;
        else
            printf("fact %d: side a says %ld, side b %ld\n", i, fa[i], fb[i]);
    }
    printf("facts agree %d of %d\n", agree, (int)NFACTS);
    printf("==END==\n");
    fflush(stdout);
    return 0;
}
