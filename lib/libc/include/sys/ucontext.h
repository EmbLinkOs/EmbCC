/* <sys/ucontext.h> -- a thread's saved machine state, laid out as the
 * kernel lays out a signal handler's third argument and as glibc's
 * ucontext_t is, on x86-64 and AArch64 (src/setjmp/ucontext-*.c). */
#ifndef _SYS_UCONTEXT_H
#define _SYS_UCONTEXT_H

#include <signal.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__x86_64__)

typedef long long greg_t;
#define NGREG 23
typedef greg_t gregset_t[NGREG];
enum {
    REG_R8 = 0, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15,
    REG_RDI, REG_RSI, REG_RBP, REG_RBX, REG_RDX, REG_RAX, REG_RCX, REG_RSP,
    REG_RIP, REG_EFL, REG_CSGSFS, REG_ERR, REG_TRAPNO, REG_OLDMASK, REG_CR2
};

struct _libc_fpxreg {
    unsigned short significand[4];
    unsigned short exponent;
    unsigned short __reserved1[3];
};
struct _libc_xmmreg { unsigned int element[4]; };
struct _libc_fpstate {
    unsigned short cwd, swd, ftw, fop;
    unsigned long long rip, rdp;
    unsigned int mxcsr, mxcr_mask;
    struct _libc_fpxreg _st[8];
    struct _libc_xmmreg _xmm[16];
    unsigned int __reserved1[24];
};
typedef struct _libc_fpstate *fpregset_t;

typedef struct {
    gregset_t gregs;
    fpregset_t fpregs;
    unsigned long long __reserved1[8];
} mcontext_t;

typedef struct ucontext_t {
    unsigned long uc_flags;
    struct ucontext_t *uc_link;
    stack_t uc_stack;
    mcontext_t uc_mcontext;
    sigset_t uc_sigmask;
    unsigned long __sigmask_rest[15];   /* glibc's 1024-bit sigset_t */
    struct _libc_fpstate __fpregs_mem;
    unsigned long long __ssp[4];
} ucontext_t;

#elif defined(__aarch64__)

typedef unsigned long long greg_t;
#define NGREG 34
typedef greg_t gregset_t[NGREG];

typedef struct {
    unsigned long long fault_address;
    unsigned long long regs[31];
    unsigned long long sp, pc, pstate;
    unsigned char __reserved[4096] __attribute__((aligned(16)));
} mcontext_t;

typedef struct ucontext_t {
    unsigned long uc_flags;
    struct ucontext_t *uc_link;
    stack_t uc_stack;
    sigset_t uc_sigmask;
    unsigned long __sigmask_rest[15];   /* glibc's 1024-bit sigset_t */
    mcontext_t uc_mcontext;
} ucontext_t;

#endif

#ifdef __cplusplus
}
#endif

#endif
