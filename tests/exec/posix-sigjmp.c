/* sigsetjmp and siglongjmp: the mask comes back when it was saved and
 * stays as it is when it was not, and a handler left by siglongjmp
 * leaves its signal unblocked for the next time -- glibc 2.36 agrees. */
// expect-exit: 42
// needs-libc: sigsetjmp, from lib/libc
#include <setjmp.h>
#include <signal.h>
#include <string.h>

static sigjmp_buf g_env;
static volatile int g_hits;

static void handler(int sig)
{
    (void)sig;
    g_hits++;
    siglongjmp(g_env, g_hits);
}

static int blocked(int sig)
{
    sigset_t now;
    sigprocmask(SIG_BLOCK, NULL, &now);
    return sigismember(&now, sig);
}

int main(void)
{
    sigset_t usr1;
    sigemptyset(&usr1);
    sigaddset(&usr1, SIGUSR1);

    /* saved: the block made after sigsetjmp is undone by the jump */
    volatile int round = 0;
    if (sigsetjmp(g_env, 1) == 0) {
        round = 1;
        sigprocmask(SIG_BLOCK, &usr1, NULL);
        if (!blocked(SIGUSR1))
            return 1;
        siglongjmp(g_env, 7);
    }
    if (round != 1 || blocked(SIGUSR1))
        return 2;

    /* not saved: the block survives the jump */
    int r = sigsetjmp(g_env, 0);
    if (r == 0) {
        sigprocmask(SIG_BLOCK, &usr1, NULL);
        siglongjmp(g_env, 0);   /* 0 arrives as 1 */
    }
    if (r != 1 || !blocked(SIGUSR1))
        return 3;
    sigprocmask(SIG_UNBLOCK, &usr1, NULL);

    /* out of a handler, twice: the first jump must unblock SIGUSR1 */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handler;
    sigaction(SIGUSR1, &sa, NULL);
    r = sigsetjmp(g_env, 1);
    if (r < 2)
        raise(SIGUSR1);
    if (r != 2 || g_hits != 2 || blocked(SIGUSR1))
        return 4;
    return 42;
}
