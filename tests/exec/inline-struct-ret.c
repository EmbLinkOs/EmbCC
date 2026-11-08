/* Functions that take and return structs, inlined (src/opt/inline.c).
 * An RTOS wrote its time constructors as macros because a function
 * returning a struct was never inlined; now its result is the local the
 * callee returns, or a copy in a local of the caller's, and a struct
 * parameter is a copy of the argument. Each path is here: one local
 * returned everywhere (no copy), different locals per return (copied),
 * a parameter returned, a one-member struct some ABIs return as a
 * scalar, _Complex, a callee that itself calls a struct-returning
 * function (its result buffer must not be the caller's), a parameter
 * the callee writes (the caller's object must not change), results held
 * across later calls, and calls in a loop. */
// expect-exit: 42
#include <stdint.h>

typedef struct { uint64_t ns; } emb_time;
typedef struct { int32_t a, b, c; } trip;
typedef struct { int32_t only; } one;

static inline emb_time ms(uint64_t m) { emb_time t = { m * 1000000u }; return t; }
static inline emb_time add(emb_time a, emb_time b) { emb_time t = { a.ns + b.ns }; return t; }
static inline emb_time bump(emb_time a) { a.ns += 7; return a; }

static inline trip pick(int k, trip x)
{
    trip p = { 1, 2, 3 }, q = { 4, 5, 6 };
    if (k > 0)
        return p;
    if (k < 0)
        return q;
    return x;
}

static inline one wrap(int v) { one o = { v * 3 }; return o; }
static inline int unwrap(one o) { return o.only - 1; }

static inline trip twice(int k)          /* calls a struct-returning one */
{
    trip t0 = { 10, 20, 30 };
    trip a = pick(k, t0), b = pick(-k, t0);
    trip r = { a.a + b.a, a.b + b.b, a.c + b.c };
    return r;
}

static inline trip scribble(trip t) { t.a = 99; t.b = 98; return t; }

static inline _Complex double cmul2(_Complex double z) { return z * 2.0; }

__attribute__((noinline)) static uint64_t opaque(uint64_t x) { return x; }

int main(void)
{
    volatile uint64_t five = 5;
    emb_time t = add(ms(opaque(five)), ms(3));
    if (t.ns != 8000000u) return 1;
    if (bump(t).ns != 8000007u || t.ns != 8000000u) return 2;

    trip x = { 7, 8, 9 };
    trip p = pick(1, x), q = pick(-1, x), r = pick(0, x);
    if (p.a != 1 || p.c != 3 || q.b != 5 || r.a != 7 || r.c != 9) return 3;

    if (unwrap(wrap((int)opaque(4))) != 11) return 4;

    trip w = twice(1);                   /* (1+4, 2+5, 3+6) */
    if (w.a != 5 || w.b != 7 || w.c != 9) return 5;
    trip w0 = twice(0);                  /* (10+10, 20+20, 30+30) */
    if (w0.a != 20 || w0.c != 60) return 6;

    trip keep = { 1, 1, 1 };
    trip s = scribble(keep);
    if (keep.a != 1 || keep.b != 1 || s.a != 99 || s.c != 1) return 7;

    trip held = pick(1, x);              /* held across later calls */
    trip other = pick(-1, x);
    if (held.a != 1 || other.a != 4) return 8;

    uint64_t acc = 0;
    for (int i = 0; i < 4; i++)
        acc += add(ms((uint64_t)i), bump(ms(1))).ns;
    if (acc != 4u * 1000007u + 6000000u) return 9;

    _Complex double z = cmul2(1.5 + 2.0 * 1.0i);
    if (__real__ z != 3.0 || __imag__ z != 4.0) return 10;
    return 42;
}
