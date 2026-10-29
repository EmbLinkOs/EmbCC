/* GCC's __builtin_setjmp and __builtin_longjmp: what a language runtime,
 * an SJLJ unwinder or Ruby's GC uses where it wants a jump with no C
 * library behind it. EmbCC lowers the pair onto an ordinary setjmp whose
 * jmp_buf lives in the setjmp caller's frame (lib/rt/builtin-setjmp.c).
 *
 * - __builtin_setjmp returns 0, then 1 when __builtin_longjmp comes.
 * - Locals not changed since keep their values, and a volatile one
 *   changed after it keeps the new value -- from 40 calls down, through a
 *   function that uses every register it can.
 * - 2,000 jumps in a row: a stack pointer not restored shows up as a
 *   stack that only grows.
 * - Two buffers, nested: an inner jump and an outer one.
 * - The buffer is GCC's five words; nothing past them is written. */
// expect-exit: 42

static void *outer[5], *inner[5];
static void *guarded[7];            /* five words, a canary each side */
static volatile int sink;

static __attribute__((noinline)) int busy(int a, int b, int c, int d)
{
    int e = a * b, f = c ^ d, g = a + c, h = b - d, i = e + f, j = g * h;
    sink = e + f + g + h + i + j;
    return sink & 7;
}

static __attribute__((noinline)) void down(void **buf, int n)
{
    if (n == 0)
        __builtin_longjmp(buf, 1);
    sink += busy(n, n + 1, n + 2, n + 3);
    down(buf, n - 1);
}

static int once(int n)
{
    int a = n, b = n * 3, c = n + 7, d = n ^ 5, e = n - 2, f = n * n;
    volatile int changed = 0;
    if (__builtin_setjmp(outer) == 0) {
        changed = 1;
        down(outer, 40);
        return -1;                       /* not reached */
    }
    /* back: a..f as they were, changed as it was set */
    return (a == n && b == n * 3 && c == n + 7 && d == (n ^ 5) &&
            e == n - 2 && f == n * n && changed == 1) ? 0 : 1;
}

static int nested(void)
{
    volatile int steps = 0;     /* changed between the jumps: volatile, as C asks */
    if (__builtin_setjmp(outer) == 0) {
        steps |= 1;
        if (__builtin_setjmp(inner) == 0) {
            steps |= 2;
            down(inner, 3);
        }
        steps |= 4;
        down(outer, 3);
    }
    return steps == 7 ? 0 : 1;
}

static int five_words(void)
{
    void **buf = guarded + 1;
    guarded[0] = guarded[6] = (void *)0x5a5a;
    if (__builtin_setjmp(buf) == 0)
        down(buf, 2);
    return guarded[0] == (void *)0x5a5a && guarded[6] == (void *)0x5a5a
               ? 0 : 1;
}

int main(void)
{
    int bad = 0;
    for (int k = 0; k < 2000; k++)
        bad |= once(k & 63);
    bad |= nested() << 1;
    bad |= five_words() << 2;
    return bad ? bad : 42;
}
