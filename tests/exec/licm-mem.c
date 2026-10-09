/* Memory in loops: a load hoisted out of one, and a location kept in a
 * register across one (src/opt/opt.c, "memory in loops"). Each function
 * is a case the optimizer must refuse or must get exactly right, and
 * each one's answer changes when it does not: two pointers into one
 * array, a callee that writes what the loop reads, a callee that only
 * READS what the loop writes, a call through a pointer, an atomic, a
 * memcpy over the location, a load that would fault if it ran when the
 * program did not ask for it, a store that would be invented on a path
 * that never stored, and exits by break and by return after a promoted
 * store. The cases it SHOULD transform are here as well, because a
 * promotion that stores the wrong value back is just as wrong. */
// expect-exit: 42

#include <string.h>

#define NOINL __attribute__((noinline))

int g, g2, g3;
static void (*fp)(void);

/* 1. a[i] can be *b: the store in the loop changes what *b reads */
NOINL static int alias_arr(int *a, const int *b, int n)
{
    int s = 0;
    for (int i = 0; i < n; i++) { a[i] += *b; s += a[i]; }
    return s;
}

/* 2. *cnt and *other are one int: the increment must read the store */
NOINL static void alias_promote(int *cnt, int *other, int n)
{
    for (int i = 0; i < n; i++) { *cnt += 1; *other = i * 10; }
}

/* 3. a callee that writes the global the loop reads */
NOINL static void bump(void) { g += 3; }
NOINL static int reads_g(int n)
{
    int s = 0;
    for (int i = 0; i < n; i++) { s += g; bump(); }
    return s;
}

/* 4. a callee that only READS the global the loop increments */
NOINL static int peek(void) { return g2; }
NOINL static int incr_peek(int n)
{
    int s = 0;
    for (int i = 0; i < n; i++) { g2++; s += peek(); }
    return s;
}

/* 5. the same through a pointer, which could be anything */
NOINL static int reads_g_ind(int n)
{
    int s = 0;
    for (int i = 0; i < n; i++) { s += g; fp(); }
    return s;
}

/* 6. an atomic increment of what the loop reads */
NOINL static int atomic_reads(int n)
{
    int s = 0;
    for (int i = 0; i < n; i++) { s += g3; __atomic_fetch_add(&g3, 2, __ATOMIC_SEQ_CST); }
    return s;
}

/* 7. a memcpy over the location the loop reads */
NOINL static int copy_over(int *p, const int *src, int n)
{
    int s = 0;
    for (int i = 0; i < n; i++) { s += *p; memcpy(p, &src[i], sizeof *p); }
    return s;
}

/* 8. *p is read only when a flag says so, and p may be null then */
NOINL static int cond_load(const int *p, const char *flags, int n)
{
    int s = 0;
    for (int i = 0; i < n; i++)
        if (flags[i])
            s += *p;
    return s;
}

/* 9. the store happens only after the test that can leave first: a
 * store on the way out would write a value nothing computed */
NOINL static void store_after_exit(int *p, int n, int stop)
{
    for (int i = 0; i < n; i++) {
        if (i * 3 == stop)
            break;
        *p = i * 7;
    }
}

/* ...and the same store BEFORE the test, where every way out has
 * stored: only the last value need reach memory */
NOINL static void store_before_exit(int *p, int n, int stop)
{
    for (int i = 0; i < n; i++) {
        *p = i * 7;
        if (i * 3 == stop)
            break;
    }
}

/* 10. a promoted counter leaving by return and by the loop's end */
struct ctr { int hits; int other; };
NOINL static int early_return(struct ctr *c, int n, int stop)
{
    for (int i = 0; i < n; i++) {
        c->hits += i;
        if (c->hits > stop)
            return i;
    }
    return -1;
}

/* 11. ...and by break, into code that reads it back */
NOINL static int early_break(struct ctr *c, int n, int stop)
{
    int i;
    for (i = 0; i < n; i++) {
        c->hits += 2;
        if (c->hits >= stop)
            break;
    }
    return c->hits * 100 + i;
}

/* 12. narrow locations: what a load reads back is the stored value
 * truncated and re-extended, every iteration */
signed char sc;
unsigned char uc;
short sh;
NOINL static int narrow(int n)
{
    int neg = 0;
    for (int i = 0; i < n; i++) {
        sc += 1;
        uc += 3;
        sh += 1000;
        if (sc < 0) neg++;
        if (sh < 0) neg += 100;
    }
    return neg;
}

/* 13. a 64-bit location, a register pair on the 32-bit targets */
long long acc64;
NOINL static void wide(int n)
{
    for (int i = 0; i < n; i++)
        acc64 += 0x100000001LL * (i + 1);
}

/* 14. nested: the inner loop's location, then the outer loop's */
int nest_cnt;
NOINL static void nested(int n)
{
    for (int i = 0; i < n; i++)
        for (int j = 0; j <= i; j++)
            nest_cnt += j;
}

/* 15. an invariant load whose address is a field of an invariant load */
struct tab { int n; int *data; };
NOINL static int chained(const struct tab *t, int reps)
{
    int s = 0;
    for (int r = 0; r < reps; r++)
        s += t->data[t->n - 1] + t->n;
    return s;
}

/* 16. the location's address stored and reloaded inside the loop */
NOINL static int escapes(int *p, int **slot, int n)
{
    int s = 0;
    for (int i = 0; i < n; i++) {
        *p += 1;
        *slot = p;
        s += **slot;
    }
    return s;
}

int main(void)
{
    int arr[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    /* *b is arr[2]: 3 until a[2] += 3 makes it 6 */
    if (alias_arr(arr, &arr[2], 5) != (4 + 5 + 6 + 10 + 11)) return 1;

    int x = 100;
    alias_promote(&x, &x, 6);              /* last: x = 50, after x = 41 + 1 */
    if (x != 50) return 2;

    g = 1;
    if (reads_g(4) != 1 + 4 + 7 + 10 || g != 13) return 3;

    g2 = 5;
    if (incr_peek(4) != 6 + 7 + 8 + 9 || g2 != 9) return 4;

    g = 0; fp = bump;
    if (reads_g_ind(3) != 0 + 3 + 6) return 5;

    g3 = 1;
    if (atomic_reads(3) != 1 + 3 + 5 || g3 != 7) return 6;

    int cell = 10, src[4] = { 20, 30, 40, 50 };
    if (copy_over(&cell, src, 4) != 10 + 20 + 30 + 40 || cell != 50) return 7;

    char none[6] = { 0, 0, 0, 0, 0, 0 }, some[6] = { 0, 1, 0, 1, 1, 0 };
    if (cond_load((const int *)0, none, 6) != 0) return 8;
    int seven = 7;
    if (cond_load(&seven, some, 6) != 21) return 9;

    int kept = 1234;
    store_after_exit(&kept, 10, 0);         /* leaves before any store */
    if (kept != 1234) return 10;
    store_after_exit(&kept, 10, 9);         /* stores i = 0, 1, 2 */
    if (kept != 14) return 11;
    store_after_exit(&kept, 4, 100);        /* runs out: last i = 3 */
    if (kept != 21) return 12;

    kept = 1234;
    store_before_exit(&kept, 10, 0);        /* stores i = 0, leaves */
    if (kept != 0) return 24;
    store_before_exit(&kept, 10, 9);        /* stores 0..3 */
    if (kept != 21) return 25;
    kept = 1234;
    store_before_exit(&kept, 0, 9);         /* never runs: untouched */
    if (kept != 1234) return 26;

    struct ctr c = { 0, 77 };
    if (early_return(&c, 100, 20) != 6 || c.hits != 21 || c.other != 77) return 13;
    c.hits = 0;
    if (early_return(&c, 4, 1000) != -1 || c.hits != 6) return 14;
    c.hits = 1;
    if (early_break(&c, 100, 10) != 11 * 100 + 4) return 15;
    c.hits = 0;
    if (early_break(&c, 3, 1000) != 6 * 100 + 3) return 16;
    c.hits = 0;
    if (early_break(&c, 0, 1000) != 0) return 17;

    sc = 120; uc = 250; sh = 30000;
    if (narrow(10) != 3 + 8 * 100) return 18;   /* sc < 0 three times, sh eight */
    if (sc != -126 || uc != (unsigned char)280 || sh != (short)40000) return 19;

    acc64 = 5;
    wide(4);                                 /* 10 * 0x100000001 + 5 */
    if (acc64 != 0xa0000000fLL) return 20;

    nest_cnt = 3;
    nested(4);                               /* 0 + 1 + 3 + 6 = 10 */
    if (nest_cnt != 13) return 21;

    int data[3] = { 4, 9, 16 };
    struct tab t = { 3, data };
    if (chained(&t, 5) != 5 * (16 + 3)) return 22;

    int cnt = 0, *slot = 0;
    if (escapes(&cnt, &slot, 4) != 1 + 2 + 3 + 4 || cnt != 4 || slot != &cnt) return 23;
    return 42;
}
