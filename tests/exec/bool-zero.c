/* `x != 0` and `x == 0` as values, plain and of a mask: what every is*()
 * of ctype and every `return (flags & BIT) != 0;` is. The Thumb backend
 * writes them as clang does -- `movs`/`ands` setting the flags, then
 * `it ne; mov d, #1` over the 0 already there, and `clz; lsrs #5` for
 * == -- so the 0 or 1 has to come out right whichever register the value
 * and the result land in, for zero, one, negative and sign-bit values.
 * The inputs come through a volatile, so the folder cannot answer.
 * Exit 0, or the number of the first wrong result. */
// expect-exit: 0

static volatile int vals[] = { 0, 1, -1, 2, 7, 8, 0x80000000, 0x7fffffff, 0x100, 6 };

__attribute__((noinline)) static int ne0(int x) { return x != 0; }
__attribute__((noinline)) static int eq0(int x) { return x == 0; }
__attribute__((noinline)) static int mask_ne(int x) { return (x & 7) != 0; }
__attribute__((noinline)) static int mask_eq(int x) { return (x & 7) == 0; }
__attribute__((noinline)) static int bigmask_ne(int x) { return (x & 0x80000100) != 0; }
__attribute__((noinline)) static int regmask_ne(int x, int m) { return (x & m) != 0; }
/* the result in another register than the value, which is still live */
__attribute__((noinline)) static int both(int x) { int b = x != 0; return b * 100 + (x & 0xff); }
__attribute__((noinline)) static int both_eq(int x) { int b = x == 0; return b * 100 + (x & 0xff); }

static int nth, first_bad;

static void check(long got, long want)
{
    nth++;
    if (got != want && !first_bad)
        first_bad = nth;
}

int main(void)
{
    for (unsigned k = 0; k < sizeof vals / sizeof vals[0]; k++) {
        int x = vals[k];
        check(ne0(x), x != 0 ? 1 : 0);
        check(eq0(x), x == 0 ? 1 : 0);
        check(mask_ne(x), (x % 8 != 0) ? 1 : 0);
        check(mask_eq(x), (x % 8 == 0) ? 1 : 0);
        check(bigmask_ne(x), (x == (int)0x80000000 || x == 0x100 || x == -1 ||
                              x == 0x7fffffff) ? 1 : 0);
        check(regmask_ne(x, 6), (x == 2 || x == 7 || x == 6 || x == -1 ||
                                 x == 0x7fffffff) ? 1 : 0);
        check(both(x), (x != 0 ? 100 : 0) + (x & 0xff));
        check(both_eq(x), (x == 0 ? 100 : 0) + (x & 0xff));
    }
    return first_bad;
}
