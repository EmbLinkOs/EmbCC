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
#include <stdlib.h>
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
 * EmbLinkOS HAS them, and a futex, so both halves of the seam's
 * optional threading group are real here: the C++ <mutex>, <thread>
 * and <future>, and C's <threads.h>, all light up on this target
 * without a line of either library knowing which OS it is.
 *
 * Two shapes have to be bridged. The kernel's thread entry takes a
 * `long` and the seam's takes a `void *`, so the pointer travels as the
 * long -- which is exact on LP64 and is the only reason this is a cast
 * rather than a table. And the kernel's futex word is a `uint32_t`
 * while the seam's is an `int`; they are the same four bytes and the
 * comparison the kernel does is bitwise, so the cast is safe and is
 * where the difference is written down.
 */
struct __os_start { void (*fn)(void *); void *arg; };

/* The kernel's entry takes a long; the seam's takes a void *. The
 * block is freed by the thread that runs it, so a thread that is
 * never joined still leaks nothing. */
static void __os_thread_trampoline(long a)
{
    struct __os_start *st = (struct __os_start *)(intptr_t)a;
    void (*fn)(void *) = st->fn;
    void *arg = st->arg;
    free(st);
    fn(arg);
    embk_thread_exit(0);
}

int __os_thread_create(unsigned long *id, void (*fn)(void *), void *arg)
{
    /* The entry signature differs, so the seam's function pointer and
     * its argument are packed into one allocation and the trampoline
     * unpacks them. A static table would bound the thread count for no
     * reason; the block is freed by the thread that runs it. */
    struct __os_start *st = malloc(sizeof *st);
    if (!st) {
        errno = ENOMEM;
        return -1;
    }
    st->fn = fn;
    st->arg = arg;
    int64_t r = embk_thread_create(__os_thread_trampoline, (long)(intptr_t)st);
    if (embk_is_err(r)) {
        free(st);
        return (int)fail(r);
    }
    *id = (unsigned long)r;
    return 0;
}

int __os_thread_join(unsigned long id)
{
    int64_t r = embk_thread_join((int)id);
    return embk_is_err(r) ? (int)fail(r) : 0;
}

/* The kernel reclaims a thread when it ends, so there is nothing to
 * release: detaching is giving up the right to join, which is a
 * decision the caller has already made by calling this. */
int __os_thread_detach(unsigned long id)
{
    (void)id;
    return 0;
}

unsigned long __os_thread_self(void)
{
    return (unsigned long)embk_thread_self();
}

void __os_thread_yield(void) { embk_yield(); }

int __os_sleep_ns(long ns)
{
    /* The kernel sleeps in milliseconds. A sub-millisecond request
     * rounds UP to one rather than to zero: a caller asking to sleep
     * has asked to yield the processor, and returning immediately is
     * the one answer that is never what was meant. */
    uint64_t ms = (uint64_t)((ns + 999999L) / 1000000L);
    if (ns > 0 && ms == 0)
        ms = 1;
    int r = embk_sleep_ms(ms);
    return embk_is_err(r) ? (int)fail(r) : 0;
}

int __os_futex_wait(const volatile int *addr, int expected, long timeout_ns)
{
    /* The kernel's futex has no timeout, so a bounded wait cannot be
     * expressed and is refused rather than turned into an unbounded
     * one -- which would hang where the caller asked not to. The C++
     * library handles the refusal (see lib/libcxx/include/mutex). */
    if (timeout_ns >= 0) {
        errno = ENOSYS;
        return -1;
    }
    int64_t r = embk_futex((volatile uint32_t *)(uintptr_t)addr,
                           EMBK_FUTEX_WAIT, (uint32_t)expected);
    return embk_is_err(r) ? (int)fail(r) : 0;
}

int __os_futex_wake(const volatile int *addr, int count)
{
    int64_t r = embk_futex((volatile uint32_t *)(uintptr_t)addr,
                           EMBK_FUTEX_WAKE,
                           count < 0 ? 0x7FFFFFFFu : (uint32_t)count);
    return embk_is_err(r) ? (int)fail(r) : (int)r;
}

/* ---- the filesystem ---------------------------------------------------------
 *
 * EmbLinkOS has the whole set, so <filesystem> works here rather than
 * reporting ENOSYS. Two mappings are worth stating because they are not
 * one-to-one:
 *
 *   readdir reads the WHOLE directory in one call -- the kernel's is
 *   not resumable -- so opendir snapshots it and readdir walks the
 *   snapshot. A directory that changes while it is being walked is
 *   therefore read as it was at the open, which is what the standard
 *   permits and what every implementation does for the same reason.
 *
 *   the kernel numbers its file types itself (EMBK_DT_*), so they are
 *   translated to the seam's rather than passed through. Passing them
 *   through would work until either side renumbered.
 */
static int type_of(uint8_t t, uint32_t mode)
{
    switch (t) {
    case EMBK_DT_REG: return __OS_FT_REGULAR;
    case EMBK_DT_DIR: return __OS_FT_DIRECTORY;
    case EMBK_DT_LNK: return __OS_FT_SYMLINK;
    default: break;
    }
    /* No type from the entry: fall back to the mode bits, which are
     * POSIX-shaped. */
    switch (mode & 0170000u) {
    case 0100000u: return __OS_FT_REGULAR;
    case 0040000u: return __OS_FT_DIRECTORY;
    case 0120000u: return __OS_FT_SYMLINK;
    case 0020000u: return __OS_FT_CHARDEV;
    case 0060000u: return __OS_FT_BLOCKDEV;
    case 0010000u: return __OS_FT_FIFO;
    case 0140000u: return __OS_FT_SOCKET;
    default: return __OS_FT_UNKNOWN;
    }
}

static int fill_info(const struct embk_stat *st, struct __os_fileinfo *out)
{
    out->size = st->size;
    out->mtime = (long)st->mtime;
    out->mode = st->mode & 07777u;
    out->type = type_of(st->type, st->mode);
    out->nlink = st->nlink;
    /* The kernel has no device number, and an inode only in a dirent.
     * equivalent() therefore compares the resolved PATHS here, which is
     * right for a single filesystem and is what this target has. */
    out->dev = 0;
    out->ino = 0;
    return 0;
}

int __os_stat(const char *path, struct __os_fileinfo *out)
{
    char pb[EM_PATH_MAX];
    const char *ap = path_abs(path, pb, sizeof pb);
    struct embk_stat st;
    long r = (long)embk_syscall2(EMBK_SYS_stat, (int64_t)(intptr_t)ap,
                                 (int64_t)(intptr_t)&st);
    if (embk_is_err(r))
        return (int)fail(r);
    return fill_info(&st, out);
}

int __os_lstat(const char *path, struct __os_fileinfo *out)
{
    char pb[EM_PATH_MAX];
    const char *ap = path_abs(path, pb, sizeof pb);
    struct embk_stat st;
    long r = (long)embk_syscall2(EMBK_SYS_lstat, (int64_t)(intptr_t)ap,
                                 (int64_t)(intptr_t)&st);
    if (embk_is_err(r))
        return (int)fail(r);
    return fill_info(&st, out);
}

int __os_mkdir(const char *path, unsigned mode)
{
    char pb[EM_PATH_MAX];
    (void)mode;               /* the kernel's mkdir takes no mode */
    int r = embk_mkdir(path_abs(path, pb, sizeof pb));
    return embk_is_err(r) ? (int)fail(r) : 0;
}

int __os_rmdir(const char *path)
{
    char pb[EM_PATH_MAX];
    int r = embk_rmdir(path_abs(path, pb, sizeof pb));
    return embk_is_err(r) ? (int)fail(r) : 0;
}

int __os_unlink(const char *path)
{
    char pb[EM_PATH_MAX];
    long r = (long)embk_syscall1(EMBK_SYS_unlink,
                                 (int64_t)(intptr_t)path_abs(path, pb,
                                                             sizeof pb));
    return embk_is_err(r) ? (int)fail(r) : 0;
}

int __os_chmod(const char *path, unsigned mode)
{
    char pb[EM_PATH_MAX];
    long r = (long)embk_syscall2(EMBK_SYS_chmod,
                                 (int64_t)(intptr_t)path_abs(path, pb,
                                                             sizeof pb),
                                 (int64_t)mode);
    return embk_is_err(r) ? (int)fail(r) : 0;
}

/* The kernel truncates by DESCRIPTOR, so the file is opened for the
 * one call. Opening write-only is deliberate: resize_file must not
 * need read permission. */
int __os_truncate(const char *path, long long size)
{
    char pb[EM_PATH_MAX];
    int fd = __os_open(path_abs(path, pb, sizeof pb), 1 /* O_WRONLY */, 0);
    if (fd < 0)
        return -1;
    long r = (long)embk_syscall2(EMBK_SYS_ftruncate, fd, (int64_t)size);
    __os_close(fd);
    return embk_is_err(r) ? (int)fail(r) : 0;
}

/* No syscall sets a modification time: the kernel tracks it and does
 * not let userland write it. Saying so is better than succeeding and
 * changing nothing, which would make last_write_time's setter a silent
 * no-op. */
int __os_utime(const char *path, long mtime)
{
    (void)path; (void)mtime;
    errno = ENOSYS;
    return -1;
}

int __os_symlink(const char *target, const char *linkpath)
{
    char lb[EM_PATH_MAX];
    /* The TARGET is a string stored in the link and is not resolved --
     * a relative target stays relative, which is what makes a symlink
     * survive its directory being moved. Only the link's own path is
     * made absolute. */
    int r = embk_symlink(target, path_abs(linkpath, lb, sizeof lb));
    return embk_is_err(r) ? (int)fail(r) : 0;
}

long __os_readlink(const char *path, char *buf, size_t n)
{
    char pb[EM_PATH_MAX];
    int64_t r = embk_readlink(path_abs(path, pb, sizeof pb), buf, n);
    return embk_is_err(r) ? fail((long)r) : (long)r;
}

int __os_link(const char *target, const char *linkpath)
{
    char tb[EM_PATH_MAX], lb[EM_PATH_MAX];
    /* A hard link is a reference to the OBJECT, so both sides resolve. */
    int r = embk_link(path_abs(target, tb, sizeof tb),
                      path_abs(linkpath, lb, sizeof lb));
    return embk_is_err(r) ? (int)fail(r) : 0;
}

int __os_getcwd(char *buf, size_t n)
{
    return getcwd(buf, n) ? 0 : -1;
}

int __os_chdir(const char *path) { return chdir(path); }

int __os_statfs(const char *path, unsigned long long *capacity,
                unsigned long long *freespace, unsigned long long *available)
{
    /* meminfo reports MEMORY, not disk, and reporting it as disk space
     * would be a plausible number that is simply about something else.
     * There is no disk-usage syscall, so this fails. */
    (void)path; (void)capacity; (void)freespace; (void)available;
    errno = ENOSYS;
    return -1;
}

/* One snapshot per open directory, because the kernel's readdir is not
 * resumable: it walks the whole directory in one call. The count is
 * bounded, and a directory with more entries than this reports the
 * first MAXENT -- which is a limitation and is stated rather than
 * silently truncating to zero. */
#define EM_MAXENT 512

struct em_dir {
    struct embk_dirent ents[EM_MAXENT];
    int n;
    int at;
};

void *__os_opendir(const char *path)
{
    char pb[EM_PATH_MAX];
    struct em_dir *d = malloc(sizeof *d);
    if (!d) {
        errno = ENOMEM;
        return 0;
    }
    int64_t r = embk_readdir(path_abs(path, pb, sizeof pb), d->ents,
                             EM_MAXENT);
    if (embk_is_err(r)) {
        fail((long)r);
        free(d);
        return 0;
    }
    d->n = (int)r;
    d->at = 0;
    return d;
}

int __os_readdir(void *dir, char *name, size_t n, int *type)
{
    struct em_dir *d = dir;
    if (!d)
        return -1;
    while (d->at < d->n) {
        struct embk_dirent *e = &d->ents[d->at++];
        /* `.` and `..` are filtered here, once, rather than in every
         * caller -- see os/backend.h. */
        if (e->name[0] == '.' &&
            (e->name[1] == 0 || (e->name[1] == '.' && e->name[2] == 0)))
            continue;
        size_t len = strlen(e->name);
        if (len + 1 > n) {
            errno = ENAMETOOLONG;
            return -1;
        }
        memcpy(name, e->name, len + 1);
        if (type)
            *type = type_of(e->type, 0);
        return 1;
    }
    return 0;
}

void __os_closedir(void *dir) { free(dir); }
