/* The optimizer's benchmark set: small kernels, each chosen because a
 * named transformation is what makes it fast.
 *
 * Every one is self-checking -- it prints a checksum that depends on
 * every computed value, so a compiler that gets faster by computing
 * something else is caught rather than celebrated. The same source is
 * built by embcc, gcc and clang and run on the same kernel, so the
 * numbers are comparable and the checksums must agree.
 *
 * What each kernel is FOR, which is the part a bare benchmark hides:
 *
 *   loop_invariant   an expression that does not change inside the
 *                    loop. LICM should hoist it out; without that it
 *                    is recomputed every iteration.
 *   strength         `i * 37` down an induction variable. Strength
 *                    reduction turns the multiply into an add.
 *   bounds           `a[i]` where the address is a linear function of
 *                    i: induction-variable simplification should walk
 *                    a pointer instead of recomputing base + i*4.
 *   unroll           a short loop whose body is cheaper than its
 *                    branch.
 *   inline_hot       a tiny function called in a loop.
 *   dead_branch      a condition the compiler can prove, guarding work
 *                    it can then delete.
 *   redundant_load   the same load twice with no store between.
 *   switch_dispatch  the decision tree, on a hot opcode loop.
 *   call_overhead    a leaf call in a loop, where the frame is the cost.
 *   memory_stream    a sequential read/write, where addressing is the
 *                    cost.
 */
#include <stdio.h>
#include <time.h>

#ifndef SCALE
#define SCALE 1
#endif

static double now(void) { return (double)clock() / CLOCKS_PER_SEC; }

/* ---- 1. loop-invariant code motion ------------------------------------- */
static long loop_invariant(long n, long a, long b)
{
    long s = 0, i;
    for (i = 0; i < n; i++)
        s += (a * b + a) ^ i;          /* a*b+a never changes */
    return s;
}

/* ---- 2. strength reduction ---------------------------------------------- */
static long strength(long n)
{
    long s = 0, i;
    for (i = 0; i < n; i++)
        s += i * 37 + (i << 3);
    return s;
}

/* ---- 3. induction variables / address arithmetic ------------------------ */
static int arr[4096];
static long bounds(long n)
{
    long s = 0, i, k;
    for (k = 0; k < n; k++)
        for (i = 0; i < 4096; i++)
            s += arr[i] + arr[(i + 1) & 4095];
    return s;
}

/* ---- 4. a short hot loop ------------------------------------------------ */
static long unroll(long n)
{
    long s = 0, i;
    for (i = 0; i < n; i++)
        s += i & 7;
    return s;
}

/* ---- 5. inlining -------------------------------------------------------- */
static long tiny(long x) { return x * 3 + 1; }
static long inline_hot(long n)
{
    long s = 0, i;
    for (i = 0; i < n; i++)
        s += tiny(i);
    return s;
}

/* ---- 6. a provable branch ----------------------------------------------- */
static long dead_branch(long n)
{
    long s = 0, i;
    for (i = 0; i < n; i++) {
        long k = i * 2;
        if (k & 1) s += 999999;        /* k is even: never taken */
        else s += k;
    }
    return s;
}

/* ---- 7. a redundant load ------------------------------------------------ */
static long glob;
static long redundant_load(long n)
{
    long s = 0, i;
    for (i = 0; i < n; i++)
        s += glob + glob + glob;       /* one load, not three */
    return s;
}

/* ---- 8. switch dispatch ------------------------------------------------- */
static long switch_dispatch(long n)
{
    long s = 0, i;
    for (i = 0; i < n; i++) {
        switch ((int)(i & 31)) {
        case 0: s += 1; break;   case 1: s += 2; break;
        case 2: s += 3; break;   case 3: s += 4; break;
        case 4: s += 5; break;   case 5: s += 6; break;
        case 6: s += 7; break;   case 7: s += 8; break;
        case 8: s += 9; break;   case 9: s += 10; break;
        case 10: s += 11; break; case 11: s += 12; break;
        case 12: s += 13; break; case 13: s += 14; break;
        case 14: s += 15; break; case 15: s += 16; break;
        case 16: s += 17; break; case 17: s += 18; break;
        case 18: s += 19; break; case 19: s += 20; break;
        case 20: s += 21; break; case 21: s += 22; break;
        case 22: s += 23; break; case 23: s += 24; break;
        case 24: s += 25; break; case 25: s += 26; break;
        case 26: s += 27; break; case 27: s += 28; break;
        case 28: s += 29; break; case 29: s += 30; break;
        case 30: s += 31; break; default: s += 32; break;
        }
    }
    return s;
}

/* ---- 9. call overhead --------------------------------------------------- */
long opaque_add(long a, long b);
static long call_overhead(long n)
{
    long s = 0, i;
    for (i = 0; i < n; i++)
        s = opaque_add(s, i);
    return s;
}

/* ---- 10. memory streaming ----------------------------------------------- */
static long memory_stream(long n)
{
    long s = 0, i, k;
    for (k = 0; k < n; k++) {
        for (i = 0; i < 4096; i++) arr[i] = (int)(arr[i] * 3 + 1);
        for (i = 0; i < 4096; i++) s += arr[i];
    }
    return s;
}

#define BENCH(name, expr)                                             \
    do {                                                              \
        double t0 = now();                                            \
        long v = (expr);                                              \
        double t1 = now();                                            \
        h = h * 1000003UL + (unsigned long)v;                         \
        printf("%-16s %8.3f\n", name, t1 - t0);                       \
    } while (0)

int main(void)
{
    unsigned long h = 0;
    int i;
    for (i = 0; i < 4096; i++) arr[i] = i * 7 + 1;
    glob = 11;

    BENCH("loop_invariant", loop_invariant(3000000L * SCALE, 5, 7));
    BENCH("strength",       strength(3000000L * SCALE));
    BENCH("bounds",         bounds(300L * SCALE));
    BENCH("unroll",         unroll(5000000L * SCALE));
    BENCH("inline_hot",     inline_hot(3000000L * SCALE));
    BENCH("dead_branch",    dead_branch(3000000L * SCALE));
    BENCH("redundant_load", redundant_load(3000000L * SCALE));
    BENCH("switch_dispatch", switch_dispatch(2000000L * SCALE));
    BENCH("call_overhead",  call_overhead(2000000L * SCALE));
    BENCH("memory_stream",  memory_stream(300L * SCALE));

    printf("checksum %lu\n", h);
    return 42;
}
