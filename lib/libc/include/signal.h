/* <signal.h> — C11 §7.14.
 *
 * The C standard's signal handling, which is a much smaller thing than
 * POSIX's: six signal numbers, one handler per signal, and a very short
 * list of what a handler may legally do. POSIX's -- sigaction, masks,
 * siginfo, waiting -- follows it, at the end of this header.
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

#include <features.h>

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

/* ---- POSIX's signals --------------------------------------------------------
 *
 * Where the OS delivers signals (Linux), they are delivered: sigaction
 * installs a handler with the kernel, and `signal` and `raise` above go
 * through it. Where it does not, sigaction fails with ENOSYS and C's
 * model above is what there is. The numbers are Linux's. */
#if defined(__EMB_POSIX)
#include <sys/types.h>

#define SIGHUP     1
#define SIGQUIT    3
#define SIGTRAP    5
#define SIGIOT     SIGABRT
#define SIGBUS     7
#define SIGKILL    9
#define SIGUSR1   10
#define SIGUSR2   12
#define SIGPIPE   13
#define SIGALRM   14
#define SIGSTKFLT 16
#define SIGCHLD   17
#define SIGCONT   18
#define SIGSTOP   19
#define SIGTSTP   20
#define SIGTTIN   21
#define SIGTTOU   22
#define SIGURG    23
#define SIGXCPU   24
#define SIGXFSZ   25
#define SIGVTALRM 26
#define SIGPROF   27
#define SIGWINCH  28
#define SIGIO     29
#define SIGPOLL   SIGIO
#define SIGPWR    30
#define SIGSYS    31
/* glibc keeps 32 and 33 for its threads; this library's threads need
 * none, but a program that counts from SIGRTMIN expects glibc's 34 */
#define SIGRTMIN  34
#define SIGRTMAX  64
#define NSIG      65
#define _NSIG     NSIG

#define SIG_HOLD  ((void (*)(int))2)

#include <bits/sigset.h>

union sigval { int sival_int; void *sival_ptr; };

/* how a timer or a message queue says something happened */
#define SIGEV_SIGNAL    0
#define SIGEV_NONE      1
#define SIGEV_THREAD    2
#define SIGEV_THREAD_ID 4
struct sigevent {
    union sigval sigev_value;
    int sigev_signo;
    int sigev_notify;
    union {
        struct {
            void (*_function)(union sigval);
            void *_attribute;           /* a pthread_attr_t *, or NULL */
        } _thread;
        int _tid;                       /* SIGEV_THREAD_ID: Linux's */
        char __pad[64 - 2 * sizeof(int) - sizeof(union sigval)];
    } __u;
};
#define sigev_notify_function   __u._thread._function
#define sigev_notify_attributes __u._thread._attribute
#define sigev_notify_thread_id  __u._tid

/* Linux's siginfo_t: 128 bytes, the three ints first, then per-signal
 * fields at offset 16 on both 64-bit ABIs */
typedef struct {
    int si_signo, si_errno, si_code;
    int __pad0;
    union {
        struct { pid_t si_pid; uid_t si_uid; int si_status; } __child;
        struct { pid_t si_pid; uid_t si_uid; union sigval si_value; } __rt;
        struct { void *si_addr; } __fault;
        struct { long si_band; int si_fd; } __poll;
        char __pad[112];
    } __u;
} siginfo_t;
#define si_pid    __u.__child.si_pid
#define si_uid    __u.__child.si_uid
#define si_status __u.__child.si_status
#define si_value  __u.__rt.si_value
#define si_addr   __u.__fault.si_addr
#define si_band   __u.__poll.si_band
#define si_fd     __u.__poll.si_fd

/* si_code */
#define SI_USER    0
#define SI_KERNEL  0x80
#define SI_QUEUE   (-1)
#define SI_TIMER   (-2)
#define SI_TKILL   (-6)
#define CLD_EXITED    1
#define CLD_KILLED    2
#define CLD_DUMPED    3
#define CLD_TRAPPED   4
#define CLD_STOPPED   5
#define CLD_CONTINUED 6
#define SEGV_MAPERR 1
#define SEGV_ACCERR 2
#define BUS_ADRALN 1
#define BUS_ADRERR 2
#define FPE_INTDIV 1
#define FPE_INTOVF 2
#define FPE_FLTDIV 3
#define ILL_ILLOPC 1

struct sigaction {
    union {
        void (*sa_handler)(int);
        void (*sa_sigaction)(int, siginfo_t *, void *);
    } __sa;
    sigset_t sa_mask;
    int sa_flags;
    void (*sa_restorer)(void);
};
#define sa_handler   __sa.sa_handler
#define sa_sigaction __sa.sa_sigaction

#define SA_NOCLDSTOP 0x00000001
#define SA_NOCLDWAIT 0x00000002
#define SA_SIGINFO   0x00000004
#define SA_ONSTACK   0x08000000
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000
#define SA_NOMASK    SA_NODEFER
#define SA_ONESHOT   SA_RESETHAND

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

typedef struct {
    void *ss_sp;
    int ss_flags;
    size_t ss_size;
} stack_t;
#define SS_ONSTACK 1
#define SS_DISABLE 2
#if defined(__aarch64__)
#define MINSIGSTKSZ 5120
#define SIGSTKSZ    16384
#else
#define MINSIGSTKSZ 2048
#define SIGSTKSZ    8192
#endif

#ifndef _STRUCT_TIMESPEC_DECLARED
#define _STRUCT_TIMESPEC_DECLARED
struct timespec { time_t tv_sec; long tv_nsec; };
#endif

int  kill(pid_t pid, int sig);
int  killpg(pid_t pgrp, int sig);
int  sigaction(int sig, const struct sigaction *__restrict act,
               struct sigaction *__restrict old);
int  sigprocmask(int how, const sigset_t *__restrict set,
                 sigset_t *__restrict old);
int  sigpending(sigset_t *set);
int  sigsuspend(const sigset_t *mask);
int  sigwait(const sigset_t *__restrict set, int *__restrict sig);
int  sigwaitinfo(const sigset_t *__restrict set, siginfo_t *__restrict info);
int  sigtimedwait(const sigset_t *__restrict set, siginfo_t *__restrict info,
                  const struct timespec *__restrict timeout);
int  sigaltstack(const stack_t *__restrict ss, stack_t *__restrict old);
int  sigqueue(pid_t pid, int sig, const union sigval value);
int  sigemptyset(sigset_t *set);
int  sigfillset(sigset_t *set);
int  sigaddset(sigset_t *set, int sig);
int  sigdelset(sigset_t *set, int sig);
int  sigismember(const sigset_t *set, int sig);
int  siginterrupt(int sig, int flag);
void psignal(int sig, const char *msg);
void psiginfo(const siginfo_t *info, const char *msg);
int  pthread_kill(unsigned long thread, int sig);
int  pthread_sigmask(int how, const sigset_t *__restrict set,
                     sigset_t *__restrict old);
#endif

#if defined(__EMB_GNU)
typedef void (*sighandler_t)(int);
typedef void (*sig_t)(int);
int sigisemptyset(const sigset_t *set);
int sigorset(sigset_t *dst, const sigset_t *a, const sigset_t *b);
int sigandset(sigset_t *dst, const sigset_t *a, const sigset_t *b);
extern const char *const sys_siglist[NSIG];
#endif

#ifdef __cplusplus
}
#endif

#endif
