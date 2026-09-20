/* The backend for EmbLinkOS.
 *
 * This file is what replaces `emlibc`. Not a port of it -- a REPLACEMENT of
 * everything except its bottom edge: EmbLinkOS keeps its syscall ABI and
 * its path conventions, and gets the same printf, strtod, malloc and math
 * as every other target. That was the whole point of the seam. emlibc's
 * `rim/syscalls.c` was 184 lines of OS-specific code under ~7000 lines of
 * portable C that duplicated newlib's; only the 184 lines were ever really
 * EmbLinkOS's, and they are what survives here.
 *
 * The ABI comes from the OS's own <embk.h>/<embk_syscall.h> rather than numbers
 * copied into this repository. The syscall numbers belong to the kernel and
 * are hand-synchronised with it (that header says so in capitals); a second
 * copy here would be a second thing to forget. Build with
 * `make libc-emblinkos EMBLINKOS=/path/to/EmbLinkOs`.
 */
#include "../backend.h"

#include <errno.h>
#include <string.h>

/* embk.h is header-only and usable from a freestanding program, so this
 * backend needs nothing linked from the OS side -- only the ABI.
 *
 * stdbool first because embk.h's mutex helpers pass `false` to
 * __atomic_compare_exchange_n without including it; under newlib something
 * else always had. Including it here is a one-line courtesy rather than an
 * edit to that repository's header. */
#include <stdbool.h>
#include <embk.h>

/* The kernel returns -errno on failure, using the SAME numbers this
 * library's <errno.h> defines (EPERM 1, ENOENT 2, EBADF 9, EINVAL 22 ...):
 * kernel/include/errno.h and lib/libc/include/errno.h agree value for
 * value. So the mapping is the identity, and saying that here is better
 * than a translation table that would silently rot if either side moved. */
static long fail(long ret)
{
    errno = (int)-ret;
    return -1;
}

/* ---- paths ---------------------------------------------------------------
 * The kernel resolves absolute paths only, so the working directory is a
 * userspace fact. It lives HERE, in the backend, rather than in the
 * portable library: "what does this name refer to" is part of how an
 * operating system names files, which is exactly what a backend is for.
 * A target with no filesystem never compiles this.
 */
#define EM_PATH_MAX 1024

static char g_cwd[EM_PATH_MAX] = "/";

static const char *path_abs(const char *in, char *buf, size_t cap)
{
    if (in && in[0] == '/')
        return in;                          /* already absolute */
    size_t cl = strlen(g_cwd), il = in ? strlen(in) : 0;
    if (cl + 1 + il + 1 > cap)
        return in;                          /* too long: let the kernel judge */
    memcpy(buf, g_cwd, cl);
    size_t p = cl;
    if (p == 0 || buf[p - 1] != '/')
        buf[p++] = '/';
    if (in) { memcpy(buf + p, in, il); p += il; }
    buf[p] = 0;
    return buf;
}

/* Not C11, and so not declared in any header this library installs -- but
 * EmbLinkOS programs use them and they have nowhere else to live. */
char *getcwd(char *buf, size_t size)
{
    size_t n = strlen(g_cwd);
    if (!buf || size <= n) { errno = ERANGE; return 0; }
    memcpy(buf, g_cwd, n + 1);
    return buf;
}

int chdir(const char *path)
{
    char tmp[EM_PATH_MAX];
    const char *ap = path_abs(path, tmp, sizeof tmp);
    struct embk_stat st;
    long r = (long)embk_syscall2(EMBK_SYS_stat, (int64_t)(intptr_t)ap,
                                 (int64_t)(intptr_t)&st);
    if (embk_is_err(r))
        return (int)fail(r);
    size_t n = strlen(ap);
    if (n >= sizeof g_cwd) { errno = ENAMETOOLONG; return -1; }
    memcpy(g_cwd, ap, n + 1);
    return 0;
}

/* crt0 calls this after publishing environ: a parent names the child's
 * start directory with PWD, and nothing is inherited unless it does. */
extern char *getenv(const char *name);

void embk_cwd_init_from_env(void)
{
    const char *pwd = getenv("PWD");
    if (pwd && pwd[0] == '/' && strlen(pwd) < sizeof g_cwd)
        memcpy(g_cwd, pwd, strlen(pwd) + 1);
}

/* ---- the eleven primitives ---------------------------------------------- */

long __os_write(int fd, const void *buf, size_t n)
{
    long r = (long)embk_syscall3(EMBK_SYS_write, fd, (int64_t)(intptr_t)buf,
                                 (int64_t)n);
    return embk_is_err(r) ? fail(r) : r;
}

long __os_read(int fd, void *buf, size_t n)
{
    long r = (long)embk_syscall3(EMBK_SYS_read, fd, (int64_t)(intptr_t)buf,
                                 (int64_t)n);
    return embk_is_err(r) ? fail(r) : r;
}

int __os_open(const char *path, int flags, int mode)
{
    char tmp[EM_PATH_MAX];
    const char *ap = path_abs(path, tmp, sizeof tmp);
    long r = (long)embk_syscall3(EMBK_SYS_open, (int64_t)(intptr_t)ap,
                                 (int64_t)flags, (int64_t)mode);
    return embk_is_err(r) ? (int)fail(r) : (int)r;
}

int __os_close(int fd)
{
    long r = (long)embk_syscall1(EMBK_SYS_close, fd);
    return embk_is_err(r) ? (int)fail(r) : 0;
}

long __os_lseek(int fd, long off, int whence)
{
    long r = (long)embk_syscall3(EMBK_SYS_lseek, fd, (int64_t)off,
                                 (int64_t)whence);
    return embk_is_err(r) ? fail(r) : r;
}

/* Robust against whether the kernel returns the old break or the new one:
 * callers use sbrk(0) to read it and sbrk(+n) only to grow, so exactly one
 * of the two conventions has to hold and it does not matter which. */
void *__os_sbrk(long increment)
{
    long r = (long)embk_syscall1(EMBK_SYS_sbrk, (int64_t)increment);
    if (embk_is_err(r)) { fail(r); return (void *)-1; }
    return (void *)(uintptr_t)r;
}

int __os_remove(const char *path)
{
    char tmp[EM_PATH_MAX];
    const char *ap = path_abs(path, tmp, sizeof tmp);
    long r = (long)embk_syscall1(EMBK_SYS_unlink, (int64_t)(intptr_t)ap);
    return embk_is_err(r) ? (int)fail(r) : 0;
}

/* The kernel's rename is STRICT: it fails -EEXIST when the destination
 * exists, where C's rename replaces it. Replacing is the libc's job, so
 * the destination is removed first -- and only after the source is known
 * to exist, or a failed rename would have destroyed the target. */
int __os_rename(const char *from, const char *to)
{
    char fb[EM_PATH_MAX], tb[EM_PATH_MAX];
    const char *fa = path_abs(from, fb, sizeof fb);
    struct embk_stat st;
    long r = (long)embk_syscall2(EMBK_SYS_stat, (int64_t)(intptr_t)fa,
                                 (int64_t)(intptr_t)&st);
    if (embk_is_err(r))
        return (int)fail(r);
    const char *ta = path_abs(to, tb, sizeof tb);
    embk_syscall1(EMBK_SYS_unlink, (int64_t)(intptr_t)ta);   /* may fail */
    r = (long)embk_syscall2(EMBK_SYS_rename, (int64_t)(intptr_t)fa,
                            (int64_t)(intptr_t)ta);
    return embk_is_err(r) ? (int)fail(r) : 0;
}

long __os_time(void)
{
    uint64_t out[2];          /* [0] seconds, [1] microseconds */
    long r = (long)embk_syscall1(EMBK_SYS_gettimeofday,
                                 (int64_t)(intptr_t)out);
    if (embk_is_err(r))
        return fail(r);
    return (long)out[0];
}

/* clock() is CPU time, and this kernel has no per-process CPU accounting
 * (user/lib/syscalls.c refuses the CPU-time clock_gettime clocks for the
 * same reason). SYS_uptime_ms is available and would produce a plausible
 * number, which is precisely why it is not used here: under any load it
 * would be wrong in a way the caller cannot detect. Failing is the answer
 * the OS's own rule demands -- what the kernel does not provide is absent,
 * not stubbed to lie. Give it CPU accounting and this becomes three lines. */
long __os_clock_ns(void)
{
    errno = ENOSYS;
    return -1;
}

void __os_exit(int status)
{
    embk_syscall1(EMBK_SYS_exit, status);
    for (;;) { }              /* the kernel never returns; defensive */
}

/* Ask what actually backs the descriptor rather than assuming 0/1/2 are the
 * console. A stdio slot the shell redirected into a pipeline correctly
 * reports not-a-tty, which is what makes line buffering come out right. */
int __os_isatty(int fd)
{
    struct embk_stat st;
    long r = (long)embk_syscall2(EMBK_SYS_fstat, fd,
                                 (int64_t)(intptr_t)&st);
    if (embk_is_err(r))
        return 0;
    return (st.mode & 0170000) == 0020000;         /* S_IFCHR */
}

int __os_getentropy(void *buf, size_t n)
{
#if defined(__x86_64__)
    unsigned char *p = buf;
    while (n) {
        unsigned long long v;
        unsigned char ok = 0;
        for (int tries = 0; tries < 32 && !ok; tries++)
            __asm__ volatile ("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
        if (!ok) { errno = EIO; return -1; }  /* honest failure, not a byte */
        size_t take = n < sizeof v ? n : sizeof v;
        memcpy(p, &v, take);
        p += take;
        n -= take;
    }
    return 0;
#else
    /* aarch64's equivalent is FEAT_RNG's RNDR, which EL0 may execute -- but
     * only where it exists, and the feature bit saying so is in
     * ID_AA64ISAR0_EL1, which EL0 may not read: the MRS traps to EL1 and
     * this kernel does not emulate it. There is no way to ask before
     * executing, and executing it on a part without it is UNDEFINED. So
     * fail, exactly as the x86 path fails when RDRAND is absent: a caller
     * handed -1 can decide, a caller handed predictable bytes cannot. */
    (void)buf; (void)n;
    errno = ENOSYS;
    return -1;
#endif
}

/* ---- threads ---------------------------------------------------------------
 *
 * EmbLinkOS does not have them yet. Saying so here, in the one file
 * that knows about EmbLinkOS, is the point of the seam: the C++ library
 * above learns from ENOSYS that this target is single-threaded and
 * reports it as a system_error where a thread would have been created.
 * Nothing above this file is conditional on which OS it is.
 *
 * When the kernel grows threads, this is the file that changes, and it
 * needs two ideas and no more: run a function on a new thread, and
 * sleep until a word in memory changes. Every mutex, condition
 * variable, semaphore and latch in the C++ library is built from the
 * second (see os/backend.h).
 */
int __os_thread_create(unsigned long *id, void (*fn)(void *), void *arg)
{
    (void)id; (void)fn; (void)arg;
    errno = ENOSYS;
    return -1;
}

int __os_thread_join(unsigned long id)
{
    (void)id;
    errno = ENOSYS;
    return -1;
}

int __os_thread_detach(unsigned long id)
{
    (void)id;
    errno = ENOSYS;
    return -1;
}

/* One thread, so it needs no name. */
unsigned long __os_thread_self(void) { return 0ul; }

/* Nobody to yield to. */
void __os_thread_yield(void) {}

int __os_sleep_ns(long ns)
{
    (void)ns;
    errno = ENOSYS;
    return -1;
}

int __os_futex_wait(const volatile int *addr, int expected, long timeout_ns)
{
    (void)addr; (void)expected; (void)timeout_ns;
    errno = ENOSYS;
    return -1;
}

int __os_futex_wake(const volatile int *addr, int count)
{
    (void)addr; (void)count;
    errno = ENOSYS;
    return -1;
}
