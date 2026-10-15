/* A struct argument that goes on the stack, after arguments that fill the
 * registers, with an alignment below its size: its words are gathered
 * byte by byte. On ARMv6-M that takes a third scratch register, and with
 * r0-r5 all holding the call's other arguments there was none -- an
 * internal error for five arguments at -O2. */
// expect-exit: 42

struct cs { signed char c; short s; };

__attribute__((noinline)) static int take(long long a, float b, double c,
                                          struct cs d, unsigned e)
{
    return (int)a + (int)(b * 2) + (int)(c * 4) + d.c + d.s + (int)e;
}

int main(void)
{
    /* -3 + 1 + 1 + 6 - 6 + 43 */
    return take(-3LL, 0.5f, 0.25, (struct cs){ 6, -6 }, 43u);
}
