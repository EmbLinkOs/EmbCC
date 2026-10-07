/* The interface between side_a.cc and side_b.cc (tests/golden/
 * cxx-abi-ilp32.sh): one side is compiled by EmbCC and the other by
 * clang++, both ways round, and each calls into the other. A name mangled
 * differently fails the link; a vtable, a member-function pointer, a guard
 * or an array cookie laid out differently fails a check or prints a
 * different line. */
#ifndef CXX_ABI_ILP32_H
#define CXX_ABI_ILP32_H

#include <stddef.h>

namespace abi {

// ---- virtual calls across objects --------------------------------------
// Shape's key function (area) is in side A, so its vtable is there;
// Square's (its destructor) is in side B.
struct Shape {
    explicit Shape(int id);
    virtual ~Shape();
    virtual long area() const;
    virtual const char *name() const = 0;
    int id;
};

struct Square : Shape {
    Square(int id, long side);
    ~Square() override;
    long area() const override;
    const char *name() const override;
    long side;
};

Shape *make_circle(int id, long r);              // A: a class only A knows
long total_area(Shape *const *s, int n);         // B: dispatches on both
int destroyed_shapes();                          // A

// Two interfaces on one object: calls through the second base go through
// this-adjusting thunks, which each side writes for its own classes.
struct Source {
    virtual ~Source();
    virtual int get() = 0;
};
struct Sink {
    virtual ~Sink();
    virtual int put(int v) = 0;
    int puts = 0;
};
struct Pipe : Source, Sink {                     // A
    Pipe();
    ~Pipe() override;
    int get() override;
    int put(int v) override;
    int buf[4];
    int head, n;
};
int pump(Source *from, Sink *to, int n);         // B
Sink *make_doubler();                            // B: a Sink B defines

// A shared virtual base: vbase offsets and VTTs.
struct Node { int tag = 5; virtual ~Node() {} virtual int value() const; };
struct Lnode : virtual Node { int l = 1; };
struct Rnode : virtual Node { int r = 2; int value() const override; };
struct Diamond : Lnode, Rnode { int d = 3; Diamond(); };     // A
int diamond_sum(Diamond *x);                                  // B

// ---- mangled names -------------------------------------------------------
namespace detail {
long mix(size_t a, ptrdiff_t b, long c, unsigned long d, wchar_t e,
         char16_t f, char32_t g, signed char h, unsigned char i, short j,
         long long k, unsigned long long l, float m, double n, bool o); // B
}
long by_ref(const long &a, long *const b, int (&arr)[4]);    // A
template <typename T> T twice(T v);                          // B: explicit
extern template int twice<int>(int);
extern template long twice<long>(long);
extern template unsigned twice<unsigned>(unsigned);
struct Money {
    long cents;
    Money operator+(const Money &o) const;                   // B
    bool operator<(const Money &o) const;                    // A
};
typedef long (*binop)(long, long);
binop pick_binop(int which);                                 // B
long apply_binop(binop f, long a, long b);                   // A

// ---- member pointers -----------------------------------------------------
struct Calc {                                    // A
    explicit Calc(int b);
    virtual ~Calc();
    int add(int x);
    virtual int scale(int x);
    int base;
};
struct Pad { int z[3]; virtual void pad(); };    // B
struct Calc2 : Pad, Calc {                       // B: Calc at a non-zero offset
    explicit Calc2(int b);
    int scale(int x) override;
};
typedef int (Calc::*Op)(int);
typedef int (Calc2::*Op2)(int);
Op pick_op(int which);                           // B: &Calc::add, &Calc::scale
Op2 widen_op(Op op);                             // B: converted to Calc2's
int call_op(Calc *c, Op op, int x);              // A
int call_op2(Calc2 *c, Op2 op, int x);           // A
bool same_op(Op a, Op b);                        // A
int Calc::*pick_field();                         // B

// ---- static initialization ----------------------------------------------
int record(int event);                           // A: counts events
int events();                                    // A
struct Reg {
    explicit Reg(int who);
    ~Reg();
    int who;
};
extern Reg reg_a;                                // A, dynamic
extern Reg reg_b;                                // B, dynamic
int static_in_b();                               // B: a guarded local static
// Defined in both units (inline): ONE static and ONE guard after linking,
// whichever unit's copy the linker keeps -- so both must lay the guard out
// alike and test it alike.
inline int shared_count()
{
    static int n = record(1000);
    return ++n;
}
int shared_count_from_b();                       // B: calls shared_count

// ---- new[] and delete[] across ------------------------------------------
struct Elem {
    Elem();
    ~Elem();
    int v;
    static int alive;                            // A
};
Elem *make_elems(int n);                         // A: new Elem[n]
void free_elems(Elem *e);                        // B: delete[] e
long long *make_wide(int n);                     // B: no cookie, new long long[n]
void free_wide(long long *p);                    // A

// ---- class values across: by value, and the hidden return slot ---------
struct Big {
    explicit Big(long v);
    Big(const Big &o);
    ~Big();
    long a[5];
    static int copies;                           // B
};
Big make_big(long v);                            // B
long sum_big(Big b);                             // A
struct Pair { short x; long long y; };           // trivial: by value
Pair swap_pair(Pair p);                          // B

// Constructors on the other side, called through new: on ARM they return
// `this`, and a caller may use that register instead of its own copy.
Square *new_square_from_a(int id, long side);    // A
Calc *new_calc_from_b(int base);                 // B

}  // namespace abi

#endif
