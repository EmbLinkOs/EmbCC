/* A function inlined whose closing brace is reached only through a call
 * that does not return -- `long long llabs(long long b) { abort(); }`,
 * GCC's torture execute/20021127-1.c, and 981019-1.c's f2 -- has no
 * `return` at all, so its result had no definition anywhere once it was
 * spliced into the caller, and the caller still read it. The AVR backend
 * gives such a temp no slot and stopped on the read ("internal error:
 * avr: main: a frame access in a function whose frame pointer was not
 * set up", -O1 and -Os); EMBCC_VERIFY stopped every target ("reads temp
 * %3 with no definition"). The inliner now writes 0 on the path that
 * falls off the callee's end, as irgen's falling-off return of an
 * integer does. stop() is never called: `never` is 0. */
// expect-exit: 42
__attribute__((noreturn)) void stop(void);
volatile int never;

/* each reaches its closing brace only through stop(), which does not
 * return: there is no `return` in it at all */
static long long ll_stop(long long b) { (void)b; stop(); }
static int int_stop(void) { stop(); }
static float flt_stop(void) { stop(); }

int main(void)
{
    int r = 42;
    if (never && ll_stop(never) != 1)
        r = 1;
    if (never && int_stop())
        r = 2;
    if (never && flt_stop() > 1.0f)
        r = 3;
    return r;
}

void stop(void)
{
    for (;;)
        never++;
}
