/* POSIX's signals, in what every target can do: handlers by sigaction,
 * raise and kill to oneself, the mask and the pending set, sigwait on a
 * pending signal, SA_SIGINFO and SA_RESETHAND, the set functions, and
 * strsignal's words (glibc's).
 *
 * The same program passes where the kernel delivers signals (Linux) and
 * where lib/libc does it all itself (the boards): there raise is a call,
 * a blocked signal waits in the pending set, and unblocking delivers it
 * -- which is POSIX's behaviour, from either. Where the kernel delivers,
 * alarm interrupts pause and abort ends a child with SIGABRT. */
// expect-exit: 42
// needs-libc: POSIX's signals, kernel-delivered or lib/libc's own
#define _GNU_SOURCE 1
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static volatile sig_atomic_t got, count, code;

static void on_sig(int s) { got = s; count++; }

static void on_info(int s, siginfo_t *i, void *u)
{
    (void)u;
    got = s;
    code = i->si_code;
}

static int sets(void)
{
    sigset_t s;
    if (sigemptyset(&s) || sigismember(&s, SIGINT) != 0 ||
        sigaddset(&s, SIGINT) || sigismember(&s, SIGINT) != 1 ||
        sigaddset(&s, 64) || sigismember(&s, 64) != 1 ||
        sigdelset(&s, SIGINT) || sigismember(&s, SIGINT) != 0)
        return 1;
    errno = 0;
    if (sigaddset(&s, 0) != -1 || errno != EINVAL ||
        sigismember(&s, 65) != -1)
        return 2;
    if (sigfillset(&s) || sigismember(&s, SIGTERM) != 1)
        return 3;
    return 0;
}

static int handlers(void)
{
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sig;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGUSR1, &sa, &old) || old.sa_handler != SIG_DFL)
        return 10;
    got = 0;
    if (raise(SIGUSR1) || got != SIGUSR1)
        return 11;
    got = 0;
    if (kill(getpid(), SIGUSR1) || got != SIGUSR1)
        return 12;

    /* blocked: pending, and delivered when unblocked */
    sigset_t m, om, pend;
    sigemptyset(&m);
    sigaddset(&m, SIGUSR1);
    if (sigprocmask(SIG_BLOCK, &m, &om))
        return 13;
    got = 0;
    raise(SIGUSR1);
    if (got || sigpending(&pend) || sigismember(&pend, SIGUSR1) != 1)
        return 14;
    if (sigprocmask(SIG_SETMASK, &om, NULL) || got != SIGUSR1)
        return 15;
    if (sigpending(&pend) || sigismember(&pend, SIGUSR1) != 0)
        return 16;

    /* sigwait takes a pending signal instead of a handler running */
    sigprocmask(SIG_BLOCK, &m, NULL);
    raise(SIGUSR1);
    got = 0;
    int which = 0;
    if (sigwait(&m, &which) || which != SIGUSR1 || got)
        return 17;
    sigprocmask(SIG_UNBLOCK, &m, NULL);

    /* SA_SIGINFO: the three-argument handler, and how it was sent */
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_info;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGUSR2, &sa, NULL);
    got = 0;
    code = 99;
    kill(getpid(), SIGUSR2);
    if (got != SIGUSR2 || code != SI_USER)
        return 18;

    /* SA_RESETHAND: once, then the default -- here, ignored, since the
     * default for SIGWINCH is to ignore it */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sig;
    sa.sa_flags = SA_RESETHAND;
    sigaction(SIGWINCH, &sa, NULL);
    count = 0;
    raise(SIGWINCH);
    raise(SIGWINCH);
    if (count != 1 || sigaction(SIGWINCH, NULL, &old) ||
        old.sa_handler != SIG_DFL)
        return 19;

    /* SIG_IGN: nothing at all */
    signal(SIGUSR1, SIG_IGN);
    got = 0;
    raise(SIGUSR1);
    if (got)
        return 20;

    /* C's signal returns the previous handler */
    if (signal(SIGUSR1, on_sig) != SIG_IGN || signal(SIGUSR1, SIG_DFL) != on_sig)
        return 21;
    errno = 0;
    if (sigaction(SIGKILL, &sa, NULL) != -1 || errno != EINVAL ||
        signal(0, on_sig) != SIG_ERR)
        return 22;
    return 0;
}

static int names(void)
{
    if (strcmp(strsignal(SIGINT), "Interrupt") ||
        strcmp(strsignal(SIGSEGV), "Segmentation fault") ||
        strcmp(strsignal(SIGRTMIN + 6), "Real-time signal 6") ||
        strcmp(strsignal(99), "Unknown signal 99"))
        return 30;
    return 0;
}

#ifdef EMBCC_TEST_HAS_FS
static void on_abrt(int s) { (void)s; count = 100; }

/* what only a kernel that delivers can do */
static int kernel(void)
{
    signal(SIGALRM, on_sig);
    got = 0;
    alarm(1);
    pause();
    if (got != SIGALRM)
        return 40;
    pid_t c = fork();
    if (c == 0)
        abort();
    int st;
    if (waitpid(c, &st, 0) != c || !WIFSIGNALED(st) || WTERMSIG(st) != SIGABRT)
        return 41;
    /* a handler that returns does not stop abort */
    c = fork();
    if (c == 0) {
        signal(SIGABRT, on_abrt);
        abort();
    }
    if (waitpid(c, &st, 0) != c || !WIFSIGNALED(st) || WTERMSIG(st) != SIGABRT)
        return 43;
    struct timespec zero = { 0, 0 };
    sigset_t m;
    sigemptyset(&m);
    sigaddset(&m, SIGUSR1);
    errno = 0;
    if (sigtimedwait(&m, NULL, &zero) != -1 || errno != EAGAIN)
        return 42;
    return 0;
}
#endif

int main(void)
{
    int r;
    if ((r = sets()) || (r = handlers()) || (r = names()))
        return r;
#ifdef EMBCC_TEST_HAS_FS
    if ((r = kernel()))
        return r;
#endif
    return 42;
}
