/* <signal.h> — C11 §7.14.
 *
 * The C standard's signal handling, which is a much smaller thing than
 * POSIX's: six signal numbers, one handler per signal, and a very short
 * list of what a handler may legally do. Everything else -- sigaction,
 * masks, siginfo, waiting -- is POSIX and is not here.
 *
 * What a handler may do is the part worth knowing, because nearly every
 * handler in the wild breaks it. C11 §7.14.1.1p5 allows a handler to
 * call only `abort`, `_Exit`, `quick_exit`, `signal` for its own
 * signal, and to assign to a `volatile sig_atomic_t`. Not printf, not
 * malloc, not free: the handler can interrupt those functions in the
 * middle and re-entering them finds a broken invariant. The one safe
 * pattern is to set a flag and return.
 *
 * On a target with no signals, `signal` reports failure rather than
 * pretending to install something that will never be called -- a
 * program that checks the return value then knows, and one that does
 * not is no worse off.
 */
#ifndef _SIGNAL_H
#define _SIGNAL_H

#ifdef __cplusplus
extern "C" {
#endif

/* The one type a handler may touch: an integer the hardware can read
 * and write in a single uninterruptible access. */
typedef int sig_atomic_t;

/* The six C11 requires. The numbers follow the usual Unix ones so a
 * program that prints a signal number agrees with what a person looks
 * up. */
#define SIGINT   2
#define SIGILL   4
#define SIGABRT  6
#define SIGFPE   8
#define SIGSEGV 11
#define SIGTERM 15

/* Distinguishable from any real function address, as the standard
 * requires: these are not addresses a function could have. */
#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)
#define SIG_ERR ((void (*)(int))-1)

/* Install a handler; returns the previous one, or SIG_ERR. */
void (*signal(int sig, void (*handler)(int)))(int);

/* Send a signal to this program. Returns zero on success. */
int raise(int sig);

#ifdef __cplusplus
}
#endif

#endif
