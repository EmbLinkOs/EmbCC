/* Signals: C's signal and raise, and POSIX's sigaction and the rest.
 *
 * Two models, and the backend decides which. Where the OS delivers
 * signals (Linux), sigaction installs a handler with the kernel, raise
 * sends a real signal to the calling thread, and signal is sigaction with
 * BSD's semantics -- the handler stays, interrupted calls restart -- as
 * glibc has it.
 *
 * Where it does not (bare metal, EmbLinkOS today), the kernel's half of
 * every call answers ENOSYS and this file is the whole of it: a table
 * of handlers, a mask and a pending set. raise is then the only way a
 * signal arrives, and it is a direct call -- which is not a
 * simplification of the real thing, it IS the real thing for a target
 * that cannot interrupt a program. A signal raised while blocked waits
 * in the pending set and is delivered when sigprocmask unblocks it, so a
 * program written for POSIX behaves the same here, short of what only
 * an OS can do (a SIGINT from a keyboard, a SIGALRM from a timer).
 *
 * What a handler may do is the part worth knowing, because nearly every
 * handler in the wild breaks it: C11 §7.14.1.1p5 allows a handler to call
 * only abort, _Exit, quick_exit and signal for its own signal, and to
 * assign to a volatile sig_atomic_t. POSIX widens that to its list of
 * async-signal-safe functions -- write, _exit, sigaction, ... -- which
 * printf and malloc are not on.
 */
#define _GNU_SOURCE 1
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "../../os/backend.h"
#include "../stdio/file.h"

/* ---- which model ------------------------------------------------------------
 *
 * Asked once, by reading SIGINT's disposition: a backend that delivers
 * answers, one that does not says ENOSYS. */
static int g_kernel = -1;

static int kernel(void)
{
    if (g_kernel < 0) {
        int saved = errno;
        struct __os_sigaction old;
        g_kernel = __os_sigaction(SIGINT, NULL, &old) == 0;
        errno = saved;
    }
    return g_kernel;
}

static int valid(int sig) { return sig > 0 && sig < NSIG; }

#define BIT(sig) (1ULL << ((sig) - 1))

/* ---- the model with no kernel -------------------------------------------- */

static struct sigaction g_act[NSIG];
static sigset_t g_mask, g_pending;

/* The default action, where there is nobody else to take it: the
 * signals POSIX says are ignored by default are, and every other one
 * ends the program with the status a shell reports for it, 128 + sig --
 * after stdio is flushed, since nothing else will flush it. */
static void default_action(int sig)
{
    switch (sig) {
    case SIGCHLD: case SIGCONT: case SIGURG: case SIGWINCH:
        return;
    }
    __stdio_flush_all();
    __os_exit(128 + sig);
}

static void deliver(int sig)
{
    struct sigaction *a = &g_act[sig];
    void (*h)(int) = a->sa_handler;
    if (h == SIG_IGN)
        return;
    if (h == SIG_DFL) {
        default_action(sig);
        return;
    }
    struct sigaction run = *a;
    if (a->sa_flags & SA_RESETHAND)
        a->sa_handler = SIG_DFL;
    /* the handler's mask, and the signal itself unless SA_NODEFER, are
     * blocked while it runs, as a kernel would */
    sigset_t saved = g_mask;
    g_mask |= run.sa_mask;
    if (!(run.sa_flags & SA_NODEFER))
        g_mask |= BIT(sig);
    if (run.sa_flags & SA_SIGINFO) {
        siginfo_t info;
        memset(&info, 0, sizeof info);
        info.si_signo = sig;
        info.si_code = SI_USER;
        info.si_pid = (pid_t)__os_getid(__OS_ID_PID);
        run.sa_sigaction(sig, &info, NULL);
    } else
        h(sig);
    g_mask = saved;
}

static void deliver_pending(void)
{
    for (int sig = 1; sig < NSIG; sig++)
        if ((g_pending & BIT(sig)) && !(g_mask & BIT(sig))) {
            g_pending &= ~BIT(sig);
            deliver(sig);
        }
}

/* A backend that can deliver a signal of its own calls this; raise in
 * the model with no kernel calls it. */
int __raise_signal(int sig)
{
    if (!valid(sig)) {
        errno = EINVAL;
        return -1;
    }
    if (g_mask & BIT(sig)) {
        g_pending |= BIT(sig);
        return 0;
    }
    deliver(sig);
    return 0;
}

/* ---- sets ------------------------------------------------------------------ */

__attribute__((weak)) int sigemptyset(sigset_t *s) { *s = 0; return 0; }
__attribute__((weak)) int sigfillset(sigset_t *s) { *s = ~0ULL; return 0; }

__attribute__((weak)) int sigaddset(sigset_t *s, int sig)
{
    if (!valid(sig)) { errno = EINVAL; return -1; }
    *s |= BIT(sig);
    return 0;
}

__attribute__((weak)) int sigdelset(sigset_t *s, int sig)
{
    if (!valid(sig)) { errno = EINVAL; return -1; }
    *s &= ~BIT(sig);
    return 0;
}

__attribute__((weak)) int sigismember(const sigset_t *s, int sig)
{
    if (!valid(sig)) { errno = EINVAL; return -1; }
    return (*s & BIT(sig)) != 0;
}

__attribute__((weak)) int sigisemptyset(const sigset_t *s) { return *s == 0; }

__attribute__((weak)) int sigorset(sigset_t *d, const sigset_t *a, const sigset_t *b)
{
    *d = *a | *b;
    return 0;
}

__attribute__((weak)) int sigandset(sigset_t *d, const sigset_t *a, const sigset_t *b)
{
    *d = *a & *b;
    return 0;
}

/* ---- dispositions ----------------------------------------------------------- */

__attribute__((weak)) int sigaction(int sig, const struct sigaction *restrict act,
              struct sigaction *restrict old)
{
    if (!valid(sig) ||
        (act && (sig == SIGKILL || sig == SIGSTOP) &&
         act->sa_handler != SIG_DFL)) {
        errno = EINVAL;
        return -1;
    }
    if (kernel()) {
        struct __os_sigaction k, ko;
        if (act) {
            k.handler = (act->sa_flags & SA_SIGINFO)
                            ? (void *)act->sa_sigaction
                            : (void *)act->sa_handler;
            k.mask = act->sa_mask;
            k.flags = act->sa_flags;
        }
        if (__os_sigaction(sig, act ? &k : NULL, old ? &ko : NULL) < 0)
            return -1;
        if (old) {
            memset(old, 0, sizeof *old);
            if (ko.flags & SA_SIGINFO)
                old->sa_sigaction =
                    (void (*)(int, siginfo_t *, void *))ko.handler;
            else
                old->sa_handler = (void (*)(int))ko.handler;
            old->sa_mask = ko.mask;
            old->sa_flags = ko.flags;
        }
        return 0;
    }
    if (old)
        *old = g_act[sig];
    if (act) {
        g_act[sig] = *act;
        if (act->sa_handler == SIG_IGN)
            g_pending &= ~BIT(sig);
    }
    return 0;
}

/* C's signal: BSD's semantics on a kernel (the handler stays installed,
 * an interrupted call restarts), C's own without one -- the handler is
 * reset before it runs, as C11 7.14.1.1p3 describes. */
void (*signal(int sig, void (*handler)(int)))(int)
{
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handler;
    sa.sa_flags = kernel() ? SA_RESTART : SA_RESETHAND | SA_NODEFER;
    if (sigaction(sig, &sa, &old) < 0)
        return SIG_ERR;
    return old.sa_handler;
}

__attribute__((weak)) int siginterrupt(int sig, int flag)
{
    struct sigaction sa;
    if (sigaction(sig, NULL, &sa) < 0)
        return -1;
    if (flag)
        sa.sa_flags &= ~SA_RESTART;
    else
        sa.sa_flags |= SA_RESTART;
    return sigaction(sig, &sa, NULL);
}

/* ---- the mask -------------------------------------------------------------- */

__attribute__((weak)) int sigprocmask(int how, const sigset_t *restrict set, sigset_t *restrict old)
{
    if (kernel())
        return __os_sigprocmask(how, set, old);
    if (old)
        *old = g_mask;
    if (set) {
        switch (how) {
        case SIG_BLOCK:   g_mask |= *set; break;
        case SIG_UNBLOCK: g_mask &= ~*set; break;
        case SIG_SETMASK: g_mask = *set; break;
        default: errno = EINVAL; return -1;
        }
        g_mask &= ~(BIT(SIGKILL) | BIT(SIGSTOP));
        deliver_pending();
    }
    return 0;
}

__attribute__((weak)) int pthread_sigmask(int how, const sigset_t *restrict set,
                    sigset_t *restrict old)
{
    int saved = errno;
    int r = sigprocmask(how, set, old) < 0 ? errno : 0;
    errno = saved;
    return r;
}

__attribute__((weak)) int sigpending(sigset_t *set)
{
    if (kernel())
        return __os_sigpending(set);
    *set = g_pending;
    return 0;
}

/* With no kernel, nothing can arrive while suspended: a pending signal
 * the new mask lets through is delivered at once, and otherwise the call
 * would wait for ever -- so it says it cannot. */
__attribute__((weak)) int sigsuspend(const sigset_t *mask)
{
    if (kernel())
        return __os_sigsuspend(mask);
    sigset_t saved = g_mask;
    g_mask = *mask;
    if (g_pending & ~g_mask) {
        deliver_pending();
        g_mask = saved;
        errno = EINTR;
        return -1;
    }
    g_mask = saved;
    errno = ENOSYS;
    return -1;
}

__attribute__((weak)) int sigtimedwait(const sigset_t *restrict set, siginfo_t *restrict info,
                 const struct timespec *restrict timeout)
{
    if (kernel()) {
        struct __os_timespec t, *tp = NULL;
        if (timeout) {
            t.sec = timeout->tv_sec;
            t.nsec = timeout->tv_nsec;
            tp = &t;
        }
        return __os_sigtimedwait(set, info, tp);
    }
    for (int sig = 1; sig < NSIG; sig++)
        if (g_pending & *set & BIT(sig)) {
            g_pending &= ~BIT(sig);
            if (info) {
                memset(info, 0, sizeof *info);
                info->si_signo = sig;
                info->si_code = SI_USER;
            }
            return sig;
        }
    errno = timeout ? EAGAIN : ENOSYS;
    return -1;
}

__attribute__((weak)) int sigwaitinfo(const sigset_t *restrict set, siginfo_t *restrict info)
{
    return sigtimedwait(set, info, NULL);
}

__attribute__((weak)) int sigwait(const sigset_t *restrict set, int *restrict sig)
{
    int saved = errno;
    int r = sigtimedwait(set, NULL, NULL);
    int err = errno;
    errno = saved;
    if (r < 0)
        return err;
    *sig = r;
    return 0;
}

__attribute__((weak)) int sigaltstack(const stack_t *restrict ss, stack_t *restrict old)
{
    return __os_sigaltstack(ss, old);
}

/* ---- sending ---------------------------------------------------------------- */

int raise(int sig)
{
    if (!valid(sig)) {
        errno = EINVAL;
        return -1;
    }
    if (kernel())
        return __os_thread_kill(0, sig);
    return __raise_signal(sig);
}

/* Where the kernel cannot send a signal, this program can still send
 * one to itself -- the library delivers it. To any other process it is
 * ENOSYS: there may well be one with that id (EmbLinkOS has processes,
 * just no signals between them), and ESRCH would say there is not. */
__attribute__((weak)) int kill(pid_t pid, int sig)
{
    int saved = errno;
    if (__os_kill(pid, sig) == 0)
        return 0;
    if (errno != ENOSYS)
        return -1;
    if (pid != 0 && pid != -1 && pid != (pid_t)__os_getid(__OS_ID_PID))
        return -1;
    errno = saved;
    if (sig == 0)
        return 0;
    return __raise_signal(sig);
}

__attribute__((weak)) int killpg(pid_t pgrp, int sig)
{
    if (pgrp < 0) {
        errno = EINVAL;
        return -1;
    }
    return kill(-pgrp, sig);
}

__attribute__((weak)) int pthread_kill(unsigned long thread, int sig)
{
    int saved = errno;
    int r;
    if (kernel())
        r = __os_thread_kill(thread, sig) < 0 ? errno : 0;
    else
        r = __raise_signal(sig) < 0 ? errno : 0;
    errno = saved;
    return r;
}

/* ---- names ----------------------------------------------------------------- */

__attribute__((weak)) const char *const sys_siglist[NSIG] = {
    [SIGHUP] = "Hangup", [SIGINT] = "Interrupt", [SIGQUIT] = "Quit",
    [SIGILL] = "Illegal instruction", [SIGTRAP] = "Trace/breakpoint trap",
    [SIGABRT] = "Aborted", [SIGBUS] = "Bus error",
    [SIGFPE] = "Floating point exception", [SIGKILL] = "Killed",
    [SIGUSR1] = "User defined signal 1", [SIGSEGV] = "Segmentation fault",
    [SIGUSR2] = "User defined signal 2", [SIGPIPE] = "Broken pipe",
    [SIGALRM] = "Alarm clock", [SIGTERM] = "Terminated",
    [SIGSTKFLT] = "Stack fault", [SIGCHLD] = "Child exited",
    [SIGCONT] = "Continued", [SIGSTOP] = "Stopped (signal)",
    [SIGTSTP] = "Stopped", [SIGTTIN] = "Stopped (tty input)",
    [SIGTTOU] = "Stopped (tty output)", [SIGURG] = "Urgent I/O condition",
    [SIGXCPU] = "CPU time limit exceeded",
    [SIGXFSZ] = "File size limit exceeded",
    [SIGVTALRM] = "Virtual timer expired",
    [SIGPROF] = "Profiling timer expired", [SIGWINCH] = "Window changed",
    [SIGIO] = "I/O possible", [SIGPWR] = "Power failure",
    [SIGSYS] = "Bad system call",
};

/* glibc's words, so a program's message is the one its users know */
__attribute__((weak)) char *strsignal(int sig)
{
    static char buf[32];
    if (valid(sig) && sys_siglist[sig])
        return (char *)sys_siglist[sig];
    if (sig >= SIGRTMIN && sig <= SIGRTMAX)
        snprintf(buf, sizeof buf, "Real-time signal %d", sig - SIGRTMIN);
    else
        snprintf(buf, sizeof buf, "Unknown signal %d", sig);
    return buf;
}

__attribute__((weak)) void psignal(int sig, const char *msg)
{
    if (msg && *msg)
        fprintf(stderr, "%s: %s\n", msg, strsignal(sig));
    else
        fprintf(stderr, "%s\n", strsignal(sig));
}

__attribute__((weak)) void psiginfo(const siginfo_t *info, const char *msg)
{
    psignal(info->si_signo, msg);
}

/* ---- abort -----------------------------------------------------------------
 *
 * C11 7.22.4.1 and POSIX: SIGABRT is raised, so a handler may run -- and
 * may not return, by longjmp or by ending the program itself. If it
 * returns, abort still ends the program: the handler is taken away, the
 * signal unblocked and raised again, and if even that came back, the
 * program stops with the status a shell reports for SIGABRT. */
void abort(void)
{
    raise(SIGABRT);
    struct sigaction dfl;
    memset(&dfl, 0, sizeof dfl);
    dfl.sa_handler = SIG_DFL;
    sigaction(SIGABRT, &dfl, NULL);
    sigset_t s = BIT(SIGABRT);
    sigprocmask(SIG_UNBLOCK, &s, NULL);
    raise(SIGABRT);
    __stdio_flush_all();
    __os_exit(128 + SIGABRT);
    for (;;)
        ;
}

/* ---- sigsetjmp's other half (<setjmp.h>) -------------------------------------- */

#include <setjmp.h>

__attribute__((weak)) int __sigjmp_save(sigjmp_buf env, int savemask)
{
    env->__saved = savemask != 0;
    if (savemask)
        sigprocmask(SIG_BLOCK, NULL, &env->__mask);
    return 0;
}

/* the mask first: leaving a handler by siglongjmp is what makes the
 * signal it blocked deliverable again */
__attribute__((weak)) void siglongjmp(sigjmp_buf env, int val)
{
    if (env->__saved)
        sigprocmask(SIG_SETMASK, &env->__mask, NULL);
    longjmp(env->__jb, val);
}

/* ---- glibc's names for signals ---------------------------------------------- */

static const char *const g_abbrev[NSIG] = {
    [SIGHUP] = "HUP", [SIGINT] = "INT", [SIGQUIT] = "QUIT", [SIGILL] = "ILL",
    [SIGTRAP] = "TRAP", [SIGABRT] = "ABRT", [SIGBUS] = "BUS", [SIGFPE] = "FPE",
    [SIGKILL] = "KILL", [SIGUSR1] = "USR1", [SIGSEGV] = "SEGV",
    [SIGUSR2] = "USR2", [SIGPIPE] = "PIPE", [SIGALRM] = "ALRM",
    [SIGTERM] = "TERM", [SIGSTKFLT] = "STKFLT", [SIGCHLD] = "CHLD",
    [SIGCONT] = "CONT", [SIGSTOP] = "STOP", [SIGTSTP] = "TSTP",
    [SIGTTIN] = "TTIN", [SIGTTOU] = "TTOU", [SIGURG] = "URG",
    [SIGXCPU] = "XCPU", [SIGXFSZ] = "XFSZ", [SIGVTALRM] = "VTALRM",
    [SIGPROF] = "PROF", [SIGWINCH] = "WINCH", [SIGIO] = "IO",
    [SIGPWR] = "PWR", [SIGSYS] = "SYS",
};

/* "HUP" for SIGHUP; NULL for a number with no name */
__attribute__((weak)) const char *sigabbrev_np(int sig)
{
    return valid(sig) ? g_abbrev[sig] : NULL;
}

/* strsignal's words, without its "Unknown signal N" */
__attribute__((weak)) const char *sigdescr_np(int sig)
{
    return valid(sig) ? sys_siglist[sig] : NULL;
}
