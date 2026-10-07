// Side B of the C++ interop test: what abi.h says B defines.
#include "abi.h"

namespace abi {

// ---- virtual calls ----
Square::Square(int i, long s) : Shape(i), side(s) {}
Square::~Square() {}
long Square::area() const { return side * side; }
const char *Square::name() const { return "square"; }

long total_area(Shape *const *s, int n)
{
    long t = 0;
    for (int i = 0; i < n; i++)
        t += s[i]->area() + s[i]->Shape::area();
    return t;
}

Sink::~Sink() {}

int pump(Source *from, Sink *to, int n)
{
    int last = 0;
    for (int i = 0; i < n; i++)
        last = to->put(from->get());
    return last;
}

namespace {
struct Doubler : Sink {
    int total = 0;
    int put(int v) override { puts++; total += 2 * v; return total; }
};
}
Sink *make_doubler() { return new Doubler; }

int Rnode::value() const { return tag + r; }
int diamond_sum(Diamond *x)
{
    Node *n = x;
    Lnode *l = x;
    Rnode *r = x;
    return n->tag + l->l + r->r + x->d + l->value() + r->value();
}

// ---- mangling ----
namespace detail {
long mix(size_t a, ptrdiff_t b, long c, unsigned long d, wchar_t e,
         char16_t f, char32_t g, signed char h, unsigned char i, short j,
         long long k, unsigned long long l, float m, double n, bool o)
{
    long long s = (long long)(a % 1000) + b + c + (long long)d + e + f + g +
                  h + i + j + k % 1000 + (long long)(l % 1000) +
                  (long long)(m * 2) + (long long)(n * 4) + o;
    return (long)s;
}
}

template <typename T> T twice(T v) { return v + v; }
template int twice<int>(int);
template long twice<long>(long);
template unsigned twice<unsigned>(unsigned);

Money Money::operator+(const Money &o) const { return Money{ cents + o.cents }; }

static long add(long a, long b) { return a + b; }
static long mul(long a, long b) { return a * b; }
binop pick_binop(int which) { return which ? mul : add; }

// ---- member pointers ----
void Pad::pad() {}
Calc2::Calc2(int b) : Calc(b) { z[0] = z[1] = z[2] = -1; }
int Calc2::scale(int x) { return base * x + 1000; }
Op pick_op(int which) { return which ? &Calc::scale : &Calc::add; }
Op2 widen_op(Op op) { return op; }
int Calc::*pick_field() { return &Calc::base; }

// ---- static initialization ----
Reg reg_b(20);

namespace {
struct Lazy {
    int v;
    Lazy() : v(record(77)) {}
};
}
int static_in_b()
{
    static Lazy lazy;
    return ++lazy.v;
}
int shared_count_from_b() { return shared_count(); }

// ---- new[] / delete[] ----
void free_elems(Elem *e) { delete[] e; }
long long *make_wide(int n)
{
    long long *p = new long long[n];
    for (int i = 0; i < n; i++)
        p[i] = (1LL << 33) + i;
    return p;
}

// ---- class values ----
int Big::copies;
Big::Big(long v) { for (int i = 0; i < 5; i++) a[i] = v * (i + 1); }
Big::Big(const Big &o) { copies++; for (int i = 0; i < 5; i++) a[i] = o.a[i] + 1; }
Big::~Big() {}
Big make_big(long v) { return Big(v); }
Pair swap_pair(Pair p) { return Pair{ (short)p.y, p.x }; }

Calc *new_calc_from_b(int base) { return new Calc(base); }

}  // namespace abi
