/* Process control, C11 §7.22.4.
 *
 * atexit handlers run in reverse order of registration, which is what lets
 * a later handler rely on what an earlier one set up.
 *
 * __cxa_atexit registers on the SAME list, and that is the whole reason it
 * lives here rather than in the C++ runtime. C++ static destructors and C
 * atexit handlers have to interleave by registration order -- a C++ object
 * constructed before an atexit() call must be destroyed after that
 * handler runs. Two lists cannot express that ordering no matter which is
 * drained first, and the bug it produces is a destructor reading something
 * a handler has already torn down.
 */
#include <stdlib.h>
#include <stdio.h>

/* stdio owns the buffers; exit has to drain them (see stdio/stdio.c). */
void __stdio_flush_all(void);

#include "../../os/backend.h"

/* A fixed table rather than a growable one: this list is walked during
 * exit, when calling malloc may be the last thing a failing program should
 * do. 256 is far past what a program here registers, and overflow is
 * REPORTED rather than ignored -- a destructor silently dropped is a file
 * never flushed. */
#define MAX_ATEXIT 256

struct handler {
    void (*fn)(void *);     /* the __cxa_atexit form; atexit adapts to it */
    void *arg;
    void *dso;
};

static struct handler g_h[MAX_ATEXIT];
static int g_n;
static int g_overflow;

/* atexit's handler takes no argument, so it is stored as the one-argument
 * form with the function itself as the argument and this as the thunk --
 * which keeps ONE list and one ordering, at the cost of one indirect call
 * per plain atexit handler. */
static void call_plain(void *f)
{
    ((void (*)(void))f)();
}

int atexit(void (*f)(void))
{
    if (!f)
        return -1;
    return __cxa_atexit(call_plain, (void *)f, 0);
}

int __cxa_atexit(void (*f)(void *), void *arg, void *dso)
{
    if (g_n >= MAX_ATEXIT) {
        /* C11 guarantees only 32 registrations, so saying no is allowed --
         * but saying it silently is not. */
        g_overflow = 1;
        return -1;
    }
    g_h[g_n].fn = f;
    g_h[g_n].arg = arg;
    g_h[g_n].dso = dso;
    g_n++;
    return 0;
}

/* Run the handlers for one DSO, or for everything when `dso` is null.
 * g_n is re-read each time round because a destructor may register another
 * handler -- a static local first touched during shutdown does exactly
 * that -- and that registration has to be honoured. */
void __cxa_finalize(void *dso)
{
    while (g_n > 0) {
        int i = g_n - 1;
        struct handler h = g_h[i];
        g_n = i;
        if (dso && h.dso != dso)
            continue;
        if (h.fn)
            h.fn(h.arg);
    }
}

void exit(int status)
{
    __cxa_finalize(0);           /* atexit and C++ static destructors */
    __stdio_flush_all();    /* buffered output is not optional on exit */
    if (g_overflow) {
        /* Handlers were dropped, so the program did not finish tidying up.
         * Reporting that is more useful than the status it asked for. */
        static const char m[] =
            "exit: too many atexit/__cxa_atexit handlers; some were dropped\n";
        __os_write(2, m, sizeof m - 1);
    }
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
