/* The attribute hints EmbCC acts on, checked by what they DO rather
 * than by the compiler agreeing with itself.
 *
 * Every attribute now has a stated disposition (src/parse/parse.c):
 * honoured, refused, or a no-op for a named reason. The refused ones
 * are in tests/compile/reject-unimplemented.sh, because their whole
 * point is that they do not compile. These are the honoured ones whose
 * effect a running program can observe.
 *
 * `visibility` and `used` are checked in tests/golden/attrs.sh, where
 * the symbol table can be read; neither changes what a program
 * computes, which is exactly why they need a different kind of test.
 */
// expect-exit: 42

/* always_inline past the size budget, and noinline below it: the
 * inliner's two overrides, and the program has to give the same answer
 * either way -- that is the point of an inlining decision. */
__attribute__((noinline)) static int slow(int a) { return a + 1; }

__attribute__((always_inline)) static int wide(int a)
{
    int t = 0;
    t += a * 1; t += a * 2; t += a * 3; t += a * 4; t += a * 5;
    t += a * 6; t += a * 7; t += a * 8; t += a * 9; t += a * 10;
    t += a * 11; t += a * 12; t += a * 13; t += a * 14; t += a * 15;
    return t;
}

/* unused: the author says not to warn. Nothing calls it, and -Wall is
 * on for this suite, so an unhonoured attribute shows up as a warning
 * rather than as a wrong answer. */
__attribute__((unused)) static int never_called(void) { return 0; }

int main(void)
{
    __attribute__((unused)) int spare;     /* leading form */
    int other __attribute__((unused));     /* trailing form */

    /* wide(1) is 1+2+...+15 = 120, slow(-79) is -78: 42. */
    return wide(1) + slow(-79);
}
