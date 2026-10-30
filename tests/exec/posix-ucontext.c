/* getcontext, setcontext, makecontext and swapcontext: two coroutines
 * passing control back and forth on stacks of their own, a function of
 * eight arguments (two of them on its stack at x86-64), the return into
 * uc_link, a getcontext resumed by setcontext, callee-saved values and a
 * floating-point one surviving the switches, and -- on Linux, where the
 * kernel holds a signal mask -- the mask coming back with the context.
 * Where there is no ucontext (every target but x86-64 and AArch64) this
 * says 42 and nothing more. */
// expect-exit: 42
// needs-libc: <ucontext.h>, from lib/libc
#if defined(__x86_64__) || defined(__aarch64__)
#include <signal.h>
#include <string.h>
#include <ucontext.h>

static ucontext_t g_main, g_a, g_b, g_done;
static char g_sa[65536] __attribute__((aligned(16)));
static char g_sb[65536] __attribute__((aligned(16)));
static char g_sd[65536] __attribute__((aligned(16)));
static int g_log[16], g_n;
static long g_sum;

static void coro_a(void)
{
    for (int i = 0; i < 3; i++) {
        g_log[g_n++] = 10 + i;
        swapcontext(&g_a, &g_b);
    }
}

static void coro_b(void)
{
    for (int i = 0; i < 3; i++) {
        g_log[g_n++] = 20 + i;
        swapcontext(&g_b, &g_a);
    }
}

static void eight(long a, long b, long c, long d, long e, long f, long g, long h)
{
    g_sum = a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h;
}

int main(void)
{
    /* ping-pong: a runs, then b, alternately, until a's loop ends; then
     * a returns into its uc_link, main */
    getcontext(&g_a);
    g_a.uc_stack.ss_sp = g_sa;
    g_a.uc_stack.ss_size = sizeof g_sa;
    g_a.uc_link = &g_main;
    makecontext(&g_a, coro_a, 0);
    getcontext(&g_b);
    g_b.uc_stack.ss_sp = g_sb;
    g_b.uc_stack.ss_size = sizeof g_sb;
    g_b.uc_link = &g_main;
    makecontext(&g_b, coro_b, 0);
    volatile double kept = 2.5;
    volatile long callee = 77;
    if (swapcontext(&g_main, &g_a))
        return 1;
    static const int want[] = { 10, 20, 11, 21, 12, 22 };
    if (g_n != 6 || memcmp(g_log, want, sizeof want))
        return 2;
    if (kept != 2.5 || callee != 77)
        return 3;

    /* eight arguments, then back through uc_link */
    getcontext(&g_done);
    g_done.uc_stack.ss_sp = g_sd;
    g_done.uc_stack.ss_size = sizeof g_sd;
    g_done.uc_link = &g_main;
    makecontext(&g_done, (void (*)(void))eight, 8, 1L, 2L, 3L, 4L, 5L, 6L, 7L, 8L);
    swapcontext(&g_main, &g_done);
    if (g_sum != 1 + 4 + 9 + 16 + 25 + 36 + 49 + 64)
        return 4;

    /* getcontext resumed by setcontext: the loop runs three times */
    volatile int round = 0;
    ucontext_t here;
    getcontext(&here);
    if (++round < 3)
        setcontext(&here);
    if (round != 3)
        return 5;

#if defined(__linux__)
    /* the mask comes back with the context */
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, SIGUSR1);
    volatile int pass = 0;
    ucontext_t m;
    getcontext(&m);
    if (pass++ == 0) {
        sigprocmask(SIG_BLOCK, &s, 0);
        setcontext(&m);
    }
    sigset_t now;
    sigprocmask(SIG_BLOCK, 0, &now);
    if (sigismember(&now, SIGUSR1))
        return 6;
#endif
    return 42;
}
#else
int main(void) { return 42; }
#endif
