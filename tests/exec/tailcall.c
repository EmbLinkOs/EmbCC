/* Tail calls: `return f(...)` made as a jump, after this function has put
 * back everything it saved. Each case keeps values in callee-saved
 * registers ACROSS the call in its caller, and checks them after -- so a
 * tail call that forgot to restore a register, the stack pointer or the
 * frame record changes the answer instead of passing by luck -- and the
 * loop at the end makes the calls thousands of times, so a stack pointer
 * left a few bytes off walks off the end of the stack.
 *
 * Plain portable C; every backend with tail calls runs it at -O2 through
 * tests/golden/regalloc-O2.sh as well as at -O0 here.
 */
// expect-exit: 42

__attribute__((noinline)) int add3(int a, int b, int c) { return a + b + c; }
__attribute__((noinline)) int mix(int a, int b)         { return a * 3 - b; }
__attribute__((noinline)) void put(int *p, int v)       { *p = v; }
volatile int sink;

/* the only call, in tail position: no frame at all where it is made */
__attribute__((noinline)) int t_leaf(int x) { return add3(x, 1, 2); }

/* a value live across an ordinary call, in a callee-saved register, then a
 * tail call: that register must be back before the jump */
__attribute__((noinline)) int t_saved(int x, int y)
{
    int a = add3(x, y, 3);
    return mix(a + x, y);
}

/* enough live values to spill, then a tail call */
__attribute__((noinline)) int t_frame(int a, int b, int c, int d)
{
    int p = add3(a, b, c), q = add3(b, c, d), r = add3(c, d, a);
    int s = add3(d, a, b), u = mix(p, q), w = mix(r, s);
    sink = p + q + r + s + u + w;
    return add3(p + q, r + s, u + w);
}

/* nothing returned: the call is the last thing and falls off the end */
__attribute__((noinline)) void t_void(int *p, int v) { put(p, v * 2); }

/* a chain: each tail-calls the next */
__attribute__((noinline)) int t_chain3(int x) { return mix(x, 1); }
__attribute__((noinline)) int t_chain2(int x) { return t_chain3(x + 1); }
__attribute__((noinline)) int t_chain1(int x) { return t_chain2(x + 1); }

int main(void)
{
    int k0 = 5, k1 = 7, k2 = 11, k3 = 13, k4 = 17, k5 = 19;
    int bad = 0, out = 0;
    long sum = 0;

    for (int i = 0; i < 20000; i++) {
        int a = t_leaf(i & 7);
        int b = t_saved(i & 3, 2);
        int c = t_frame(1, 2, i & 1, 4);
        int d = t_chain1(i & 15);
        t_void(&out, i & 5);
        sum += a + b + c + d + out;
        /* the caller's own values, which the callees' tail calls must
         * not have disturbed */
        if (k0 != 5 || k1 != 7 || k2 != 11 || k3 != 13 || k4 != 17 ||
            k5 != 19)
            bad = 1;
        k0 += 0; k1 += 0;
    }
    if (bad)
        return 1;
    if (t_leaf(4) != 7 || t_saved(1, 2) != 19 || t_chain1(3) != 14)
        return 2;
    if (t_frame(1, 2, 3, 4) != 56)
        return 3;
    t_void(&out, 21);
    if (out != 42)
        return 4;
    return sum == 1940000L ? 42 : 5;
}
