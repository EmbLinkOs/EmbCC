/* Calls through a pointer whose value sits in a register: the target may
 * arrive in any argument register, and the call's own argument setup
 * fills those -- so the backend has to read the target before it is
 * overwritten, or call from a register the setup leaves alone. Tail calls
 * through a pointer (`return f(...)`) branch to it with nothing to
 * restore. Each case also passes arguments in an order that moves them
 * between argument registers, a struct, a 64-bit value, a returned
 * struct, and more arguments than registers.
 * Exit 0, or the number of the first wrong result. */
// expect-exit: 0

typedef __INT32_TYPE__ i32;
typedef long long i64;
struct pr { i32 a, b, c; };

static volatile i32 vk = 3;

__attribute__((noinline)) static i32 sub2(i32 a, i32 b) { return a - b; }
__attribute__((noinline)) static i32 mix3(i32 a, i32 b, i32 c) { return a * 100 + b * 10 + c; }
__attribute__((noinline)) static i32 sum4(i32 a, i32 b, i32 c, i32 d) { return a + 2 * b + 3 * c + 4 * d; }
__attribute__((noinline)) static i32 sum6(i32 a, i32 b, i32 c, i32 d, i32 e, i32 f)
{
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f;
}
__attribute__((noinline)) static i32 take_pr(struct pr p, i32 k) { return p.a * 7 + p.b * 5 + p.c * 3 + k; }
__attribute__((noinline)) static i64 wide(i64 x, i32 k) { return x * 3 + k; }
__attribute__((noinline)) static struct pr make_pr(i32 a, i32 b) { struct pr r = { a, b, a ^ b }; return r; }
__attribute__((noinline)) static i32 neg(i32 a) { return -a; }
__attribute__((noinline)) static void bump(i32 *p) { *p += 11; }

/* the target in r0..r3 (its own argument position), then the others */
__attribute__((noinline)) static i32 t_in_r0(i32 (*f)(i32, i32), i32 a, i32 b) { return f(b, a) + 1; }
__attribute__((noinline)) static i32 t_in_r1(i32 a, i32 (*f)(i32, i32), i32 b) { return f(b, a) + 1; }
__attribute__((noinline)) static i32 t_in_r2(i32 a, i32 b, i32 (*f)(i32, i32, i32)) { return f(b, a, b) + 1; }
__attribute__((noinline)) static i32 t_in_r3(i32 a, i32 b, i32 c, i32 (*f)(i32, i32, i32, i32))
{
    return f(c, b, a, c) + 1;
}
/* ...and as tail calls */
__attribute__((noinline)) static i32 tail_r0(i32 (*f)(i32, i32), i32 a, i32 b) { return f(b, a); }
__attribute__((noinline)) static i32 tail_r3(i32 a, i32 b, i32 c, i32 (*f)(i32, i32, i32)) { return f(c, a, b); }
__attribute__((noinline)) static i32 tail_noargs(i32 (*f)(void)) { return f(); }
__attribute__((noinline)) static void tail_void(void (*f)(i32 *), i32 *p) { f(p); }
__attribute__((noinline)) static i32 tail_table(i32 (*const *tab)(i32, i32), i32 s, i32 a, i32 b)
{
    return tab[s](a, b);
}
/* a target live across another call: in a callee-saved register, which
 * a tail call's restores would put back before the branch */
static i32 cellg;
__attribute__((noinline)) static i32 after_call(i32 (*f)(i32, i32), i32 a, i32 b)
{
    bump(&cellg);
    return f(b, a);
}
/* a struct argument, a 64-bit one, a returned struct, stack arguments */
__attribute__((noinline)) static i32 via_struct(i32 (*f)(struct pr, i32), i32 x)
{
    struct pr p = { x, x + 1, x + 2 };
    return f(p, x * 2);
}
__attribute__((noinline)) static i64 via_wide(i64 (*f)(i64, i32), i64 x) { return f(x, (i32)x); }
__attribute__((noinline)) static i32 via_sret(struct pr (*f)(i32, i32), i32 a, i32 b)
{
    struct pr r = f(b, a);
    return r.a * 9 + r.b * 4 + r.c;
}
/* ...and with the target arriving where the hidden result pointer
 * pushes the second argument: a1 is b, a2 (r2) is a, and the target */
__attribute__((noinline)) static i32 via_sret_r2(i32 a, i32 b, struct pr (*f)(i32, i32))
{
    struct pr r = f(b, a);
    return r.a * 9 + r.b * 4 + r.c;
}
__attribute__((noinline)) static i32 via_stack(i32 (*f)(i32, i32, i32, i32, i32, i32), i32 a)
{
    return f(a, a + 1, a + 2, a + 3, a + 4, a + 5);
}
/* a callback in a loop, its pointer live across every call */
__attribute__((noinline)) static i32 loop_cb(i32 (*cb)(i32), i32 n)
{
    i32 s = 0;
    for (i32 i = 0; i < n; i++)
        s += cb(i) * (i + 1);
    return s;
}
static i32 zero(void) { return vk - 3; }
static i32 (*const tab2[3])(i32, i32) = { sub2, sub2, sub2 };

static int nth, first_bad;

static void check(i64 got, i64 want)
{
    nth++;
    if (got != want && !first_bad)
        first_bad = nth;
}

int main(void)
{
    i32 k = vk, cell = 4;
    struct pr p = { k, k + 1, k + 2 };
    check(t_in_r0(sub2, k, 10), 10 - k + 1);
    check(t_in_r1(k, sub2, 10), 10 - k + 1);
    check(t_in_r2(k, 7, mix3), 7 * 100 + k * 10 + 7 + 1);
    check(t_in_r3(1, 2, k, sum4), k + 2 * 2 + 3 * 1 + 4 * k + 1);
    check(tail_r0(sub2, k, 20), 20 - k);
    check(tail_r3(1, 2, k, mix3), k * 100 + 1 * 10 + 2);
    check(tail_noargs(zero), 0);
    tail_void(bump, &cell);
    check(cell, 15);
    check(tail_table(tab2, k - 2, 50, k), 50 - k);
    check(via_struct(take_pr, k), take_pr(p, k * 2));
    check(via_wide(wide, (i64)k << 33 | 5), (((i64)k << 33 | 5) * 3) + 5);
    check(via_sret(make_pr, k, 9), 9 * 9 + k * 4 + (9 ^ k));
    check(via_sret_r2(k, 9, make_pr), 9 * 9 + k * 4 + (9 ^ k));
    check(via_stack(sum6, k), sum6(k, k + 1, k + 2, k + 3, k + 4, k + 5));
    check(loop_cb(neg, 6), -(0 * 1 + 1 * 2 + 2 * 3 + 3 * 4 + 4 * 5 + 5 * 6));
    check(after_call(sub2, k, 30), 30 - k);
    check(cellg, 11);
    return first_bad;
}
