/* Threads on Linux: clone(2), and the bookkeeping around it.
 *
 * This is the part of the seam that could not be written until
 * something here could RUN a Linux program. `clone` returns into the
 * child on a stack the caller allocated, with no frame and no return
 * address, so the entry has to be assembly, per architecture, and it
 * cannot be got approximately right -- a wrong one compiles, links, and
 * crashes somewhere inside whatever the new thread was supposed to do.
 * It was left as ENOSYS for exactly as long as that was untestable.
 * tests/golden/linux.sh boots a real kernel now, so it is written.
 *
 * ---- what a thread is here ------------------------------------------------
 *
 * One `struct lthread`, allocated by the creator and owned by whoever
 * reclaims it. Its first field is the one the KERNEL writes:
 *
 *   CLONE_PARENT_SETTID   the kernel stores the new thread's id there
 *                         before the child runs.
 *   CLONE_CHILD_CLEARTID  the kernel zeroes it when the thread ends,
 *                         and wakes anything waiting on that address.
 *
 * Those two are the whole of join. There is no exit handshake to write
 * and no window where a thread has ended but not yet said so: the
 * kernel does both as part of tearing the thread down, so a joiner that
 * arrives late sees zero and a joiner that arrives early is woken.
 *
 * ---- thread-local storage --------------------------------------------------
 *
 * Every thread gets its own block, copied from the program's PT_TLS
 * template by ./tls.c and installed by the kernel through CLONE_SETTLS
 * as part of the same clone that creates it -- so a thread's storage is
 * private from its first instruction, with no window in which it shares
 * the creator's.
 *
 * That is what makes errno per-thread, and it is also how a thread
 * knows which one it is: `self` below is a thread-local pointer written
 * once at the top of the new thread, so __os_thread_self() is a load
 * rather than the list scan it used to be. recursive_mutex calls it on
 * every lock.
 */
#include "../backend.h"
#include "syscall.h"

#include <errno.h>
#include <stdlib.h>

/* ---- the entry stub -------------------------------------------------------
 *
 * __embcc_clone(fn, stack_top, flags, arg, tidptr, tls) issues the
 * clone syscall and, in the child, calls fn(arg) on the new stack and
 * ends the thread when it returns.
 *
 * `tidptr` serves as BOTH the parent-set and child-cleared tid word --
 * they are the same address here, which is what makes the pair work as
 * a join -- so six arguments cover a syscall that takes five plus the
 * two this wrapper needs, and nothing has to reach the stack.
 *
 * Written as data with its disassembly beside it, because EmbCC has no
 * general text assembler (src/arch/x86_64/topasm.c) -- the same form
 * lib/libc/src/setjmp and ./start.c use. These bytes are not hand
 * encoded: they are what x86_64-elf-as and aarch64-elf-as produce for
 * the mnemonics in the comments, and tests/golden/linux.sh disassembles
 * them again and checks that an independent tool reads them back the
 * same way.
 *
 * The child must not return through C. Its stack is fresh, so there is
 * nothing to return TO, and the frame pointer and link register are
 * zeroed so an unwinder walking out of it stops here. It leaves through
 * SYS_exit -- the one that ends a THREAD -- rather than exit_group,
 * which would take the process with it.
 */
#if defined(__x86_64__)

__asm__(
    ".globl __embcc_clone\n"
    "__embcc_clone:\n"
    /* the child's stack: aligned, with fn and arg placed at its top */
    ".byte 0x48,0x83,0xe6,0xf0\n"      /* and  $-16,%rsi        */
    ".byte 0x48,0x83,0xee,0x10\n"      /* sub  $0x10,%rsi       */
    ".byte 0x48,0x89,0x3e\n"           /* mov  %rdi,(%rsi)      fn  */
    ".byte 0x48,0x89,0x4e,0x08\n"      /* mov  %rcx,0x8(%rsi)   arg */
    /* clone(flags, newsp, ptid, ctid, tls) -- x86-64's argument order,
     * which is NOT aarch64's: there, tls and ctid are the other way
     * round. r8 is read twice before it is overwritten, because ptid
     * and ctid are the same address. */
    ".byte 0x48,0x89,0xd7\n"           /* mov  %rdx,%rdi        flags */
    ".byte 0x4d,0x89,0xc2\n"           /* mov  %r8,%r10         ctid  */
    ".byte 0x4c,0x89,0xc2\n"           /* mov  %r8,%rdx         ptid  */
    ".byte 0x4d,0x89,0xc8\n"           /* mov  %r9,%r8          tls   */
    ".byte 0xb8,0x38,0x00,0x00,0x00\n" /* mov  $56,%eax         SYS_clone */
    ".byte 0x0f,0x05\n"                /* syscall */
    ".byte 0x85,0xc0\n"                /* test %eax,%eax */
    ".byte 0x75,0x10\n"                /* jne  1f   (parent, or failure) */
    /* the child, on its own stack */
    ".byte 0x31,0xed\n"                /* xor  %ebp,%ebp   end of the chain */
    ".byte 0x58\n"                     /* pop  %rax             fn  */
    ".byte 0x5f\n"                     /* pop  %rdi             arg */
    ".byte 0xff,0xd0\n"                /* call *%rax            fn(arg) */
    ".byte 0x31,0xff\n"                /* xor  %edi,%edi */
    ".byte 0xb8,0x3c,0x00,0x00,0x00\n" /* mov  $60,%eax         SYS_exit */
    ".byte 0x0f,0x05\n"                /* syscall */
    ".byte 0xf4\n"                     /* hlt -- unreachable */
    ".byte 0xc3\n"                     /* 1: ret  (the parent's return) */
);

#elif defined(__aarch64__)

__asm__(
    ".globl __embcc_clone\n"
    "__embcc_clone:\n"
    ".long 0x927cec21\n"   /* and  x1, x1, #-16                          */
    ".long 0xa9bf0c20\n"   /* stp  x0, x3, [x1, #-16]!   fn, arg on it   */
    /* clone(flags, newsp, ptid, tls, ctid) -- aarch64 selects
     * CLONE_BACKWARDS, so tls comes BEFORE ctid here and after it on
     * x86-64. Getting that pair the wrong way round would hand the
     * kernel a tid pointer as a TLS base. */
    ".long 0xaa0203e0\n"   /* mov  x0, x2                flags          */
    ".long 0xaa0403e2\n"   /* mov  x2, x4                ptid           */
    ".long 0xaa0503e3\n"   /* mov  x3, x5                tls            */
    /* ctid is x4 already: it is the same word as ptid. */
    ".long 0xd2801b88\n"   /* mov  x8, #220              SYS_clone      */
    ".long 0xd4000001\n"   /* svc  #0                                   */
    ".long 0xb4000040\n"   /* cbz  x0, 1f                the child      */
    ".long 0xd65f03c0\n"   /* ret                        parent/failure */
    ".long 0xd280001d\n"   /* 1: mov x29, #0             end of chain   */
    ".long 0xd280001e\n"   /* mov  x30, #0               no return addr */
    ".long 0xa8c103e1\n"   /* ldp  x1, x0, [sp], #16     fn, arg        */
    ".long 0xd63f0020\n"   /* blr  x1                    fn(arg)        */
    ".long 0xaa1f03e0\n"   /* mov  x0, xzr                              */
    ".long 0xd2800ba8\n"   /* mov  x8, #93               SYS_exit       */
    ".long 0xd4000001\n"   /* svc  #0                                   */
);

#else
#error "the Linux thread backend has no clone stub for this architecture"
#endif

long __embcc_clone(void (*fn)(void *), void *stack_top, long flags,
                   void *arg, volatile int *tidptr, void *tls);

/* ./tls.c: one thread block, copied from the program's PT_TLS
 * template. NULL when the program has no thread-local data at all, in
 * which case there is nothing to install and CLONE_SETTLS is left out
 * rather than pointing the thread register at nothing. */
void *__libc_tls_new(void);
void  __libc_tls_free(void *tp);

/* CLONE_THREAD is what makes this a thread rather than a process: one
 * thread group, one process id, one signal disposition. It requires
 * SIGHAND, which requires VM. FS, FILES and SYSVSEM share the working
 * directory, the descriptor table and the semaphore undo list, which is
 * what "the same program" means. */
#define LCLONE_VM              0x00000100
#define LCLONE_FS              0x00000200
#define LCLONE_FILES           0x00000400
#define LCLONE_SIGHAND         0x00000800
#define LCLONE_THREAD          0x00010000
#define LCLONE_SYSVSEM         0x00040000
#define LCLONE_SETTLS          0x00080000
#define LCLONE_PARENT_SETTID   0x00100000
#define LCLONE_CHILD_CLEARTID  0x00200000

#define LTHREAD_FLAGS (LCLONE_VM | LCLONE_FS | LCLONE_FILES | \
                       LCLONE_SIGHAND | LCLONE_THREAD | LCLONE_SYSVSEM | \
                       LCLONE_PARENT_SETTID | LCLONE_CHILD_CLEARTID)

#define LPROT_NONE   0
#define LPROT_READ   1
#define LPROT_WRITE  2
#define LMAP_PRIVATE   0x02
#define LMAP_ANONYMOUS 0x20

/* One megabyte of address space, of which only the pages a thread
 * actually touches are ever backed by memory, and the lowest page is
 * unmapped so running off the end faults instead of quietly writing
 * into whatever mmap put below it. */
#define LSTACK_SIZE (1024 * 1024)
#define LGUARD_SIZE 4096

struct lthread {
    /* The kernel writes this one: the thread id while it runs, zero
     * when it has ended. Nothing in this file stores to it. */
    volatile int tid;
    int detached;
    void *tls;              /* this thread's block, freed with it */
    void *stack;
    unsigned long stacksz;
    struct lthread *next;
};

static struct lthread *g_threads;
static volatile int g_lock;

/* Which thread this is. Written once by the trampoline below, before
 * the caller's function runs; zero on the thread the program started
 * on, which is what the seam documents and what recursive_mutex needs
 * "nobody" to look like. */
static __thread struct lthread *self;

/* What the clone stub actually enters. The seam hands out a `void
 * (*)(void *)` and an argument; the thread needs one more thing than
 * that -- to know which thread it is -- and there is nowhere to put it
 * except here, between the kernel and the caller's function. */
struct start_arg {
    void (*fn)(void *);
    void *arg;
    struct lthread *t;
};

static void thread_entry(void *p)
{
    struct start_arg *sa = (struct start_arg *)p;
    void (*fn)(void *) = sa->fn;
    void *arg = sa->arg;
    self = sa->t;
    free(sa);
    fn(arg);
}

/* A spin lock rather than a mutex, because a mutex in this library is
 * built ON this file and the recursion would not end. It is held for a
 * few list operations at a time, and a contending thread gives up its
 * slice rather than burning it. */
static void lock(void)
{
    while (__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE))
        __os_thread_yield();
}

static void unlock(void)
{
    __atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE);
}

static void unlink_locked(struct lthread *t)
{
    struct lthread **pp = &g_threads;
    while (*pp) {
        if (*pp == t) {
            *pp = t->next;
            return;
        }
        pp = &(*pp)->next;
    }
}

static void release(struct lthread *t)
{
    __libc_tls_free(t->tls);
    lsys2(LSYS_munmap, (long)t->stack, (long)t->stacksz);
    free(t);
}

/* A detached thread cannot free its own stack -- it is standing on it --
 * and there is no third party watching. So the reclamation happens
 * here, on the next call that takes the lock anyway: any detached
 * thread the kernel has marked dead has its stack and block released.
 * Memory is therefore bounded by the number of detached threads alive
 * at once, rather than by the number ever created. */
static void reap_locked(void)
{
    struct lthread **pp = &g_threads;
    while (*pp) {
        struct lthread *t = *pp;
        if (t->detached && __atomic_load_n(&t->tid, __ATOMIC_ACQUIRE) == 0) {
            *pp = t->next;
            release(t);
        } else {
            pp = &t->next;
        }
    }
}

int __os_thread_create(unsigned long *id, void (*fn)(void *), void *arg)
{
    if (!id || !fn) {
        errno = EINVAL;
        return -1;
    }
    lock();
    reap_locked();
    unlock();

    struct lthread *t = (struct lthread *)calloc(1, sizeof *t);
    if (!t) {
        errno = ENOMEM;
        return -1;
    }
    /* Freed by the new thread once it has read it. It cannot live on
     * this function's stack: the caller returns as soon as clone does,
     * and the new thread may not have looked yet. */
    struct start_arg *sa = (struct start_arg *)malloc(sizeof *sa);
    if (!sa) {
        free(t);
        errno = ENOMEM;
        return -1;
    }
    sa->fn = fn;
    sa->arg = arg;
    sa->t = t;

    long base = lsys6(LSYS_mmap, 0, LSTACK_SIZE, LPROT_READ | LPROT_WRITE,
                      LMAP_PRIVATE | LMAP_ANONYMOUS, -1, 0);
    /* mmap reports failure as a small negative value rather than by
     * sign: an address with the top bit set is a legitimate result on
     * some configurations, so the test is the -errno RANGE. */
    if ((unsigned long)base >= (unsigned long)-4095L) {
        free(sa);
        free(t);
        errno = (int)-base;
        return -1;
    }
    lsys3(LSYS_mprotect, base, LGUARD_SIZE, LPROT_NONE);

    t->stack = (void *)base;
    t->stacksz = LSTACK_SIZE;
    /* Its own copy of every __thread object, so that the thread about
     * to start does not share errno -- or anything else -- with the one
     * starting it. */
    t->tls = __libc_tls_new();
    /* Not zero, which is what "already ended" looks like: until the
     * kernel writes the real id, a joiner must not conclude the thread
     * is gone. */
    t->tid = -1;

    lock();
    t->next = g_threads;
    g_threads = t;
    unlock();

    long flags = LTHREAD_FLAGS;
    if (t->tls)
        flags |= LCLONE_SETTLS;
    long r = __embcc_clone(thread_entry, (char *)base + LSTACK_SIZE, flags,
                           sa, &t->tid, t->tls);
    if (r < 0) {
        lock();
        unlink_locked(t);
        unlock();
        release(t);
        free(sa);
        errno = (int)-r;
        return -1;
    }
    *id = (unsigned long)t;
    return 0;
}

int __os_thread_join(unsigned long id)
{
    struct lthread *t = (struct lthread *)id;
    if (!t) {
        errno = EINVAL;
        return -1;
    }
    for (;;) {
        int tid = __atomic_load_n(&t->tid, __ATOMIC_ACQUIRE);
        if (tid == 0)
            break;
        /* A SHARED futex wait, not the private one __os_futex_wait
         * uses. The waker here is the KERNEL, tearing the thread down,
         * and it wakes the CHILD_CLEARTID address without the private
         * flag -- so a private wait would key on something the wake
         * never touches and this loop would never end. Matching the
         * waker is not optional. */
        lsys4(LSYS_futex, (long)&t->tid, LFUTEX_WAIT, tid, 0);
    }
    lock();
    unlink_locked(t);
    unlock();
    release(t);
    return 0;
}

int __os_thread_detach(unsigned long id)
{
    struct lthread *t = (struct lthread *)id;
    if (!t) {
        errno = EINVAL;
        return -1;
    }
    lock();
    t->detached = 1;
    reap_locked();          /* it may already have ended */
    unlock();
    return 0;
}

/* The handle this thread was created with, or 0 for the thread the
 * program started on -- which never had one, and for which 0 is the
 * answer the seam documents. recursive_mutex relies on that: it treats
 * 0 as "nobody", and the main thread must not collide with a real
 * handle. */
unsigned long __os_thread_self(void)
{
    return (unsigned long)self;
}
