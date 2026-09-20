/* Process control, C11 §7.22.4.
 *
 * atexit handlers run in reverse order of registration, which is what lets
 * a later handler rely on what an earlier one set up -- and is also how C++
 * static destructors get their order, since __cxa_atexit registers through
 * here.
 */
#include <stdlib.h>
#include <stdio.h>

/* stdio owns the buffers; exit has to drain them (see stdio/stdio.c). */
void __stdio_flush_all(void);

#include "../../os/backend.h"

#define MAX_ATEXIT 64

static void (*g_fns[MAX_ATEXIT])(void);
static int g_n;

int atexit(void (*f)(void))
{
    if (!f || g_n >= MAX_ATEXIT)
        return -1;               /* C11 guarantees only 32; say no honestly */
    g_fns[g_n++] = f;
    return 0;
}

void exit(int status)
{
    while (g_n > 0)
        g_fns[--g_n]();          /* reverse order */
    __stdio_flush_all();    /* buffered output is not optional on exit */
    __os_exit(status);
    for (;;) {}
}

void _Exit(int status)
{
    /* No handlers, no flush: the point of _Exit is that nothing runs. */
    __os_exit(status);
    for (;;) {}
}

void abort(void)
{
    /* SIGABRT would be raised here if this library had signals. Until then,
     * a distinctive status is better than a silent stop. */
    __stdio_flush_all();
    __os_exit(134);              /* 128 + SIGABRT, what a shell reports */
    for (;;) {}
}

int abs(int x)             { return x < 0 ? -x : x; }
long labs(long x)          { return x < 0 ? -x : x; }
long long llabs(long long x) { return x < 0 ? -x : x; }

div_t div(int num, int den)
{
    div_t r; r.quot = num / den; r.rem = num % den; return r;
}
ldiv_t ldiv(long num, long den)
{
    ldiv_t r; r.quot = num / den; r.rem = num % den; return r;
}
lldiv_t lldiv(long long num, long long den)
{
    lldiv_t r; r.quot = num / den; r.rem = num % den; return r;
}

/* A 64-bit linear congruential generator, returning its top 31 bits --
 * the low bits of an LCG have short periods, and handing those out is how
 * a shuffle ends up visibly biased. RAND_MAX is 2^31-1 to match. */
static unsigned long long g_seed = 1;

int rand(void)
{
    g_seed = g_seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return (int)(g_seed >> 33);
}

void srand(unsigned seed) { g_seed = seed; }
