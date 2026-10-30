/* The backend for EmbLinkOS.
 *
 * Every primitive of os/backend.h on EmbLinkOS's own system calls, read
 * from that OS's <embk.h> and <embk_syscall.h> rather than copied here --
 * the numbers belong to the kernel and are hand-synchronised with it, so a
 * second copy would be a second thing to forget. Build with
 * `make libc-emblinkos EMBLINKOS=/path/to/EmbLinkOs`.
 *
 * ---- where the meaning comes from -----------------------------------------
 *
 * EmbLinkOS's newlib adapter (user/lib/syscalls.c and spawn_compat.inc)
 * already worked out, call by call, what POSIX means on this kernel, and
 * its test suite holds it to that. This file carries those decisions
 * over, under that adapter's three honest categories:
 *
 *   REAL      the kernel can do it: done truthfully
 *   VACUOUS   the promise holds here whatever is done -- FD_CLOEXEC on a
 *             system with no exec, a mask on a system with no signals --
 *             so saying yes is a true statement
 *   ABSENT    the capability does not exist: ENOSYS (os/absent.inc for
 *             everything not named below), never a pretended success
 *
 * ---- what this OS is ---------------------------------------------------------
 *
 *   paths      the kernel resolves absolute paths only, up to 256 bytes;
 *              the working directory is this library's, per process, and
 *              a child's is named in its spawn, never inherited by accident
 *   processes  spawn, not fork: a new program, set up by explicit actions,
 *              waited for through the child handles its parent owns
 *   signals    none delivered: lib/libc's own model (stdlib/signal.c) is
 *              what a program has, and kill reaches only itself
 *   network    IPv4, through the kernel's own sockets and resolver
 *   users      one, with the whole machine's authority: every id is 0
 *
 * errno: the kernel numbers its errors as Linux does, and so does
 * lib/libc's <errno.h>, so a kernel error passes through as itself.
 */
#include "../backend.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* stdbool first because embk.h's mutex helpers pass `false` to
 * __atomic_compare_exchange_n without including it. */
#include <stdbool.h>
#include <embk.h>
#include <embk_child.h>
#include <embk_terminal.h>

static long fail(long ret)
{
    errno = (int)-ret;
    return -1;
}

static int ret0(int64_t r) { return embk_is_err(r) ? (int)fail((long)r) : 0; }
static long retn(int64_t r) { return embk_is_err(r) ? fail((long)r) : (long)r; }

/* ---- paths ---------------------------------------------------------------- */

/* the kernel's SYSCALL_PATH_MAX: a longer path is refused there anyway */
#define EM_PATH_MAX 256

static char g_cwd[EM_PATH_MAX] = "/";

/* in, made absolute against cwd and NORMALIZED -- "." dropped, ".."
 * popping a component, "/.." staying "/" -- into out: the kernel has no
 * rule for ".." and an absolute path with one in it would fail there.
 * 0, or -1 with errno (an empty path is ENOENT, not the root). */
static int path_from(const char *cwd, const char *in, char *out, size_t cap)
{
    if (!in) {
        errno = EFAULT;
        return -1;
    }
    if (!*in) {
        errno = ENOENT;
        return -1;
    }
    char raw[EM_PATH_MAX * 2];
    size_t il = strlen(in), cl = strlen(cwd);
    if (in[0] == '/') {
        if (il >= sizeof raw)
            goto toolong;
        memcpy(raw, in, il + 1);
    } else {
        if (cl + 1 + il >= sizeof raw)
            goto toolong;
        memcpy(raw, cwd, cl);
        size_t at = cl;
        if (!cl || raw[cl - 1] != '/')
            raw[at++] = '/';
        memcpy(raw + at, in, il + 1);
    }
    size_t o = 0;
    out[o++] = '/';
    for (const char *p = raw; *p; ) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        const char *seg = p;
        size_t n = 0;
        while (p[n] && p[n] != '/')
            n++;
        p += n;
        if (n == 1 && seg[0] == '.')
            continue;
        if (n == 2 && seg[0] == '.' && seg[1] == '.') {
            while (o > 1 && out[o - 1] != '/')
                o--;
            if (o > 1)
                o--;
            continue;
        }
        if (o > 1) {
            if (o + 1 >= cap)
                goto toolong;
            out[o++] = '/';
        }
        if (o + n >= cap)
            goto toolong;
        memcpy(out + o, seg, n);
        o += n;
    }
    out[o] = 0;
    return 0;
toolong:
    errno = ENAMETOOLONG;
    return -1;
}

#define ABS(path, buf) \
    char buf[EM_PATH_MAX]; \
    if (path_from(g_cwd, (path), buf, sizeof buf) < 0) \
        return -1

/* The environment a child gets when its parent names none -- the
 * EmbLinkOS default: nothing is inherited unless named. crt0 points
 * environ here then, rather than at NULL, so getenv can walk it. */
char *embk_empty_env[1] = { 0 };

/* crt0 calls this once environ is installed: the child's starting
 * directory as its spawn named it, else PWD (how a native parent hands
 * one over), else the root. A PWD naming somewhere gone must not stop
 * the program, so it is checked and otherwise ignored. */
void embk_cwd_init_from_env(void)
{
    char initial[EM_PATH_MAX];
    int64_t n = embk_syscall2(EMBK_SYS_spawn_cwd, (int64_t)(uintptr_t)initial,
                              sizeof initial);
    if (n > 0 && n < (int64_t)sizeof g_cwd) {
        memcpy(g_cwd, initial, (size_t)n);
        g_cwd[n] = 0;
        return;
    }
    extern char *getenv(const char *name);
    const char *pwd = getenv("PWD");
    if (pwd && pwd[0] == '/')
        __os_chdir(pwd);
}

/* ---- the kernel's file metadata --------------------------------------------- */

static int type_of(uint8_t t, uint32_t mode)
{
    switch (t) {
    case EMBK_DT_REG: return __OS_FT_REGULAR;
    case EMBK_DT_DIR: return __OS_FT_DIRECTORY;
    case EMBK_DT_LNK: return __OS_FT_SYMLINK;
    case 5:           return __OS_FT_CHARDEV;    /* VFS_DT_CHAR: the console */
    case 6:           return __OS_FT_FIFO;       /* VFS_DT_FIFO: a pipe end */
    default: break;
    }
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

/* The kernel reports no owner (there is one user), no device number and
 * no inode through stat -- an inode only in a directory entry -- so those
 * stay zero; equivalent() then compares resolved paths, which on one
 * filesystem is right. */
static int fill_info(int64_t r, const struct embk_stat *st,
                     struct __os_fileinfo *out)
{
    if (embk_is_err(r))
        return (int)fail((long)r);
    memset(out, 0, sizeof *out);
    out->size = st->size;
    out->mtime = (long)st->mtime;
    out->mode = st->mode & 07777u;
    out->type = type_of(st->type, st->mode);
    out->nlink = st->nlink ? st->nlink : 1;
    out->blksize = out->type == __OS_FT_CHARDEV ? 1024 : 4096;
    return 0;
}

/* ---- bytes ------------------------------------------------------------------- */

long __os_write(int fd, const void *buf, size_t n)
{
    return retn(embk_syscall3(EMBK_SYS_write, fd, (int64_t)(intptr_t)buf,
                              (int64_t)n));
}

long __os_read(int fd, void *buf, size_t n)
{
    return retn(embk_syscall3(EMBK_SYS_read, fd, (int64_t)(intptr_t)buf,
                              (int64_t)n));
}

/* ---- files ------------------------------------------------------------------- */

/* The seam's flags are Linux's; the kernel's agree on all but
 * close-on-exec, which it numbers 0x100000. It has no O_DIRECTORY and no
 * O_NOFOLLOW, and both are honoured here rather than dropped: the first
 * by looking at what was opened, the second by looking before. */
#define EM_O_CLOEXEC 0x100000

static int open_flags(int f)
{
    int out = f & (__OS_O_RDONLY | __OS_O_WRONLY | __OS_O_RDWR |
                   __OS_O_CREAT | __OS_O_EXCL | __OS_O_TRUNC |
                   __OS_O_APPEND | __OS_O_NONBLOCK);
    if (f & __OS_O_CLOEXEC)
        out |= EM_O_CLOEXEC;
    return out;
}

int __os_open(const char *path, int flags, int mode)
{
    ABS(path, ap);
    if (flags & __OS_O_NOFOLLOW) {
        struct embk_stat st;
        int64_t r = embk_syscall2(EMBK_SYS_lstat, (int64_t)(intptr_t)ap,
                                  (int64_t)(intptr_t)&st);
        if (!embk_is_err(r) && type_of(st.type, st.mode) == __OS_FT_SYMLINK) {
            errno = ELOOP;
            return -1;
        }
    }
    int64_t fd = embk_syscall3(EMBK_SYS_open, (int64_t)(intptr_t)ap,
                               open_flags(flags), mode);
    if (embk_is_err(fd))
        return (int)fail((long)fd);
    if (flags & __OS_O_DIRECTORY) {
        struct embk_stat st;
        if (embk_is_err(embk_syscall2(EMBK_SYS_fstat, fd,
                                      (int64_t)(intptr_t)&st)) ||
            type_of(st.type, st.mode) != __OS_FT_DIRECTORY) {
            embk_syscall1(EMBK_SYS_close, fd);
            errno = ENOTDIR;
            return -1;
        }
    }
    return (int)fd;
}

int __os_close(int fd) { return ret0(embk_syscall1(EMBK_SYS_close, fd)); }

long __os_lseek(int fd, long off, int whence)
{
    return retn(embk_syscall3(EMBK_SYS_lseek, fd, (int64_t)off, whence));
}

/* C's remove takes a file or an empty directory */
int __os_remove(const char *path)
{
    ABS(path, ap);
    int64_t r = embk_syscall1(EMBK_SYS_unlink, (int64_t)(intptr_t)ap);
    if (embk_is_err(r) && (r == -EISDIR || r == -EPERM)) {
        struct embk_stat st;
        if (!embk_is_err(embk_syscall2(EMBK_SYS_lstat, (int64_t)(intptr_t)ap,
                                       (int64_t)(intptr_t)&st)) &&
            type_of(st.type, st.mode) == __OS_FT_DIRECTORY)
            r = embk_syscall1(EMBK_SYS_rmdir, (int64_t)(intptr_t)ap);
    }
    return ret0(r);
}

/* The kernel replaces an existing destination in the same commit that
 * moves the name -- atomically, which is what git's lockfiles and every
 * write-then-rename save rely on. So this is the one call, not an unlink
 * followed by a rename that a crash between the two would make lose the
 * destination. */
int __os_rename(const char *from, const char *to)
{
    ABS(from, fa);
    ABS(to, ta);
    return ret0(embk_syscall2(EMBK_SYS_rename, (int64_t)(intptr_t)fa,
                              (int64_t)(intptr_t)ta));
}

void *__os_sbrk(long increment)
{
    int64_t r = embk_syscall1(EMBK_SYS_sbrk, (int64_t)increment);
    if (embk_is_err(r)) {
        fail((long)r);
        return (void *)-1;
    }
    return (void *)(uintptr_t)r;
}

/* ---- time --------------------------------------------------------------------- */

long __os_time(void)
{
    uint64_t out[2];          /* [0] seconds, [1] microseconds */
    int64_t r = embk_syscall1(EMBK_SYS_gettimeofday, (int64_t)(intptr_t)out);
    return embk_is_err(r) ? fail((long)r) : (long)out[0];
}

/* there is no per-process CPU accounting to report */
long __os_clock_ns(void)
{
    errno = ENOSYS;
    return -1;
}

/* The clocks this OS has, at the precision they have:
 *   REALTIME   the RTC, whole seconds -- sub-second digits from elsewhere
 *              would be precision the wall clock does not have
 *   MONOTONIC  the HPET's uptime in milliseconds (RAW and BOOTTIME are
 *              the same counter: nothing slews it, and it spans suspend)
 *   THREAD_CPUTIME_ID  the calling thread's CPU time
 * The process's CPU time has no counter, and asking for it is EINVAL. */
int __os_clock_gettime(int clock, struct __os_timespec *t)
{
    switch (clock) {
    case 0: {
        uint64_t out[2];
        int64_t r = embk_syscall1(EMBK_SYS_gettimeofday,
                                  (int64_t)(intptr_t)out);
        if (embk_is_err(r))
            return (int)fail((long)r);
        t->sec = (long long)out[0];
        t->nsec = (long)out[1] * 1000L;
        return 0;
    }
    case 1: case 4: case 7: {
        uint64_t ms = embk_uptime_ms();
        t->sec = (long long)(ms / 1000);
        t->nsec = (long)(ms % 1000) * 1000000L;
        return 0;
    }
    case 3: {
        uint64_t ns = embk_thread_cpu_ns();
        t->sec = (long long)(ns / 1000000000ull);
        t->nsec = (long)(ns % 1000000000ull);
        return 0;
    }
    }
    errno = EINVAL;
    return -1;
}

int __os_clock_getres(int clock, struct __os_timespec *t)
{
    long ns;
    switch (clock) {
    case 0:                    ns = 1000000000L; break;
    case 1: case 4: case 7:    ns = 1000000L;    break;
    case 3:                    ns = 1;           break;
    default:
        errno = EINVAL;
        return -1;
    }
    if (t) {
        t->sec = ns / 1000000000L;
        t->nsec = ns % 1000000000L;
    }
    return 0;
}

/* The kernel sleeps in milliseconds; a request rounds UP, since waking
 * early breaks a pacing loop and a fraction of a millisecond more is
 * harmless. Nothing interrupts a sleep here, so nothing is left over. */
static int sleep_ms_ns(long long ns)
{
    if (ns <= 0)
        return 0;
    uint64_t ms = (uint64_t)((ns + 999999) / 1000000);
    int r = embk_sleep_ms(ms);
    return embk_is_err(r) ? (int)fail(r) : 0;
}

int __os_clock_nanosleep(int clock, int abs, const struct __os_timespec *req,
                         struct __os_timespec *rem)
{
    long long ns = req->sec * 1000000000LL + req->nsec;
    if (abs) {
        struct __os_timespec now;
        if (__os_clock_gettime(clock, &now) < 0)
            return -1;
        ns -= now.sec * 1000000000LL + now.nsec;
    } else if (clock != 0 && clock != 1 && clock != 4 && clock != 7) {
        errno = EINVAL;
        return -1;
    }
    if (rem)
        rem->sec = rem->nsec = 0;
    return sleep_ms_ns(ns);
}

/* VACUOUS: with no CPU accounting, the times that have passed are zero,
 * which clock() and times() then report consistently */
long long __os_times(long long out[4])
{
    out[0] = out[1] = out[2] = out[3] = 0;
    return 0;
}

/* ---- the rest of C's ---------------------------------------------------------- */

void __os_exit(int status)
{
    embk_syscall1(EMBK_SYS_exit, status);
    for (;;) { }
}

/* a terminal is what fstat says is a character device: the console; a
 * pipe-backed standard descriptor is not one */
int __os_isatty(int fd)
{
    struct embk_stat st;
    int64_t r = embk_syscall2(EMBK_SYS_fstat, fd, (int64_t)(intptr_t)&st);
    return !embk_is_err(r) && type_of(st.type, st.mode) == __OS_FT_CHARDEV;
}

/* The kernel's generator, which is seeded from every clock the machine
 * has and reads RNDR at EL1 on an aarch64 part that has it. On x86, the
 * processor's RDRAND where the kernel cannot answer. A security
 * primitive fails rather than return bytes anyone could predict. */
int __os_getentropy(void *buf, size_t n)
{
    if (n > 256) {
        errno = EIO;
        return -1;
    }
    if (embk_getrandom(buf, n) == (int64_t)n)
        return 0;
#if defined(__x86_64__)
    unsigned char *p = buf;
    while (n) {
        unsigned long long v;
        unsigned char ok = 0;
        for (int tries = 0; tries < 10 && !ok; tries++)
            __asm__ volatile ("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
        if (!ok) {
            errno = EIO;
            return -1;
        }
        size_t take = n < sizeof v ? n : sizeof v;
        memcpy(p, &v, take);
        p += take;
        n -= take;
    }
    return 0;
#else
    errno = ENOSYS;
    return -1;
#endif
}

/* ---- threads, and blocking ---------------------------------------------------- */

struct __os_start { void (*fn)(void *); void *arg; };

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
        return (int)fail((long)r);
    }
    *id = (unsigned long)r;
    return 0;
}

int __os_thread_join(unsigned long id)
{
    return ret0(embk_thread_join((int)id));
}

/* the kernel reaps a thread that ends whether or not it is joined */
int __os_thread_detach(unsigned long id)
{
    (void)id;
    return 0;
}

unsigned long __os_thread_self(void) { return (unsigned long)embk_thread_self(); }
void __os_thread_yield(void) { embk_yield(); }
int __os_sleep_ns(long ns) { return sleep_ms_ns(ns); }

/* The kernel's futex has no timeout, so a bounded wait is refused rather
 * than turned into an unbounded one. */
int __os_futex_wait(const volatile int *addr, int expected, long timeout_ns)
{
    if (timeout_ns >= 0) {
        errno = ENOSYS;
        return -1;
    }
    return ret0(embk_futex((volatile uint32_t *)(uintptr_t)addr,
                           EMBK_FUTEX_WAIT, (uint32_t)expected));
}

int __os_futex_wake(const volatile int *addr, int count)
{
    int64_t r = embk_futex((volatile uint32_t *)(uintptr_t)addr,
                           EMBK_FUTEX_WAKE,
                           count < 0 ? 0x7FFFFFFFu : (uint32_t)count);
    return (int)retn(r);
}

/* ---- the filesystem ------------------------------------------------------------ */

int __os_stat(const char *path, struct __os_fileinfo *out)
{
    ABS(path, ap);
    struct embk_stat st;
    return fill_info(embk_syscall2(EMBK_SYS_stat, (int64_t)(intptr_t)ap,
                                   (int64_t)(intptr_t)&st), &st, out);
}

int __os_lstat(const char *path, struct __os_fileinfo *out)
{
    ABS(path, ap);
    struct embk_stat st;
    return fill_info(embk_syscall2(EMBK_SYS_lstat, (int64_t)(intptr_t)ap,
                                   (int64_t)(intptr_t)&st), &st, out);
}

int __os_fstat(int fd, struct __os_fileinfo *out)
{
    struct embk_stat st;
    return fill_info(embk_syscall2(EMBK_SYS_fstat, fd, (int64_t)(intptr_t)&st),
                     &st, out);
}

/* EMBKFS has no permission bits for a mode to set */
int __os_mkdir(const char *path, unsigned mode)
{
    (void)mode;
    ABS(path, ap);
    return ret0(embk_mkdir(ap));
}

int __os_rmdir(const char *path)
{
    ABS(path, ap);
    return ret0(embk_rmdir(ap));
}

int __os_unlink(const char *path)
{
    ABS(path, ap);
    return ret0(embk_syscall1(EMBK_SYS_unlink, (int64_t)(intptr_t)ap));
}

/* the kernel keeps the type bits and takes the rest */
int __os_chmod(const char *path, unsigned mode)
{
    ABS(path, ap);
    return ret0(embk_syscall2(EMBK_SYS_chmod, (int64_t)(intptr_t)ap, mode));
}

int __os_truncate(const char *path, long long size)
{
    int fd = __os_open(path, __OS_O_WRONLY, 0);
    if (fd < 0)
        return -1;
    int r = __os_ftruncate(fd, size);
    int e = errno;
    __os_close(fd);
    errno = e;
    return r;
}

/* the kernel tracks a modification time but has no call that sets one */
int __os_utime(const char *path, long mtime)
{
    (void)path; (void)mtime;
    errno = ENOSYS;
    return -1;
}

/* The target is a string stored in the link and is not resolved: a
 * relative target stays relative, which is what makes a tree of links
 * survive being moved. Only the link's own name is made absolute. */
int __os_symlink(const char *target, const char *linkpath)
{
    if (!target) {
        errno = EINVAL;
        return -1;
    }
    ABS(linkpath, lp);
    return ret0(embk_symlink(target, lp));
}

/* The kernel reports the link's FULL length; POSIX's readlink reports
 * the bytes placed, so it is clamped to the buffer. */
long __os_readlink(const char *path, char *buf, size_t n)
{
    ABS(path, ap);
    int64_t r = embk_readlink(ap, buf, n);
    if (embk_is_err(r))
        return fail((long)r);
    return (size_t)r < n ? (long)r : (long)n;
}

/* a hard link names the OBJECT, so both sides resolve */
int __os_link(const char *target, const char *linkpath)
{
    ABS(target, tp);
    ABS(linkpath, lp);
    return ret0(embk_link(tp, lp));
}

int __os_getcwd(char *buf, size_t n)
{
    size_t k = strlen(g_cwd) + 1;
    if (k > n) {
        errno = ERANGE;
        return -1;
    }
    memcpy(buf, g_cwd, k);
    return 0;
}

/* verified before it moves: a cwd that is not there would turn one bad
 * chdir into ENOENTs far from the cause */
int __os_chdir(const char *path)
{
    ABS(path, ap);
    struct __os_fileinfo fi;
    if (__os_stat(ap, &fi) < 0)
        return -1;
    if (fi.type != __OS_FT_DIRECTORY) {
        errno = ENOTDIR;
        return -1;
    }
    memcpy(g_cwd, ap, strlen(ap) + 1);
    return 0;
}

/* meminfo is memory, not disk: there is no call that says how full a
 * filesystem is, and memory's numbers would be plausible and wrong */
int __os_statfs(const char *path, unsigned long long *capacity,
                unsigned long long *freespace, unsigned long long *available)
{
    (void)path; (void)capacity; (void)freespace; (void)available;
    errno = ENOSYS;
    return -1;
}

/* ---- directories ---------------------------------------------------------------
 *
 * The kernel's readdir is path-based and not resumable -- one call walks
 * the whole directory into a buffer, and it TRUNCATES SILENTLY when the
 * buffer fills, returning exactly the count it was given room for. So a
 * directory is a snapshot taken at opendir, and a full count means the
 * snapshot may be short: the buffer is doubled and the directory read
 * again until a count comes back with room to spare. */
struct em_dir {
    struct embk_dirent *ents;
    int n, at;
};

void *__os_opendir(const char *path)
{
    char ap[EM_PATH_MAX];
    if (path_from(g_cwd, path, ap, sizeof ap) < 0)
        return NULL;
    struct __os_fileinfo fi;
    if (__os_stat(ap, &fi) < 0)
        return NULL;
    if (fi.type != __OS_FT_DIRECTORY) {
        errno = ENOTDIR;
        return NULL;
    }
    for (uint32_t cap = 64;; cap *= 2) {
        struct embk_dirent *ents = calloc(cap, sizeof *ents);
        if (!ents) {
            errno = ENOMEM;
            return NULL;
        }
        int64_t n = embk_readdir(ap, ents, cap);
        if (embk_is_err(n)) {
            free(ents);
            fail((long)n);
            return NULL;
        }
        if ((uint32_t)n < cap) {
            struct em_dir *d = malloc(sizeof *d);
            if (!d) {
                free(ents);
                errno = ENOMEM;
                return NULL;
            }
            d->ents = ents;
            d->n = (int)n;
            d->at = 0;
            return d;
        }
        free(ents);
        if (cap >= (1u << 20)) {
            errno = ENOMEM;
            return NULL;
        }
    }
}

static int is_dot(const char *s)
{
    return s[0] == '.' && (!s[1] || (s[1] == '.' && !s[2]));
}

/* `.` and `..` are filtered here for <filesystem> (os/backend.h says why) */
int __os_readdir(void *dir, char *name, size_t n, int *type)
{
    struct em_dir *d = dir;
    while (d->at < d->n) {
        struct embk_dirent *e = &d->ents[d->at++];
        if (is_dot(e->name))
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

/* POSIX's readdir has every entry the kernel reports, with its inode */
int __os_readdir_entry(void *dir, struct __os_dirent *e)
{
    struct em_dir *d = dir;
    if (d->at >= d->n)
        return 0;
    struct embk_dirent *k = &d->ents[d->at++];
    e->ino = k->ino;
    e->type = type_of(k->type, 0);
    size_t len = strnlen(k->name, sizeof k->name);
    memcpy(e->name, k->name, len);
    e->name[len] = 0;
    return 1;
}

/* the snapshot again from the start; a directory changed since is seen
 * by the next opendir, which POSIX leaves unspecified */
void __os_rewinddir(void *dir) { ((struct em_dir *)dir)->at = 0; }

/* a snapshot has no descriptor behind it, and inventing one would let a
 * caller fstat the wrong object */
int __os_dirfd(void *dir)
{
    (void)dir;
    errno = ENOTSUP;
    return -1;
}

void __os_closedir(void *dir)
{
    struct em_dir *d = dir;
    if (d) {
        free(d->ents);
        free(d);
    }
}

/* ---- descriptors ------------------------------------------------------------------
 *
 * The kernel's fcntl ops: 3 get status (access mode, 4 nonblocking,
 * 8 append), 4 get close-on-exec, 5 set it, 6 set status. */
#define EM_FD_NONBLOCK 4
#define EM_FD_APPEND   8

static int set_cloexec(int fd, int on)
{
    return ret0(embk_syscall3(EMBK_SYS_fcntl, fd, 5, on != 0));
}

/* the lowest free descriptor at or above minfd, sharing fd's open file
 * description and cursor, as dup promises */
int __os_dup(int fd, int minfd, int cloexec)
{
    int r = embk_dup(fd, -1, minfd);
    if (r < 0)
        return (int)fail(r);
    if (cloexec)
        set_cloexec(r, 1);
    return r;
}

int __os_dup2(int fd, int newfd, int cloexec)
{
    int r = embk_dup(fd, newfd, 0);
    if (r < 0)
        return (int)fail(r);
    if (fd != newfd)
        set_cloexec(newfd, cloexec);
    return r;
}

int __os_pipe(int fds[2], int flags)
{
    int r = embk_pipe_fds(fds);
    if (r < 0)
        return (int)fail(r);
    for (int i = 0; i < 2; i++) {
        if (flags & __OS_O_CLOEXEC)
            set_cloexec(fds[i], 1);
        if (flags & __OS_O_NONBLOCK)
            embk_syscall3(EMBK_SYS_fcntl, fds[i], 6, EM_FD_NONBLOCK);
    }
    return 0;
}

/* Close-on-exec and the status flags are real per-descriptor state. Record
 * locks do not exist: F_GETLK, F_SETLK and F_SETLKW are ENOSYS, which a
 * program that needs locking can see, where a lock that locked nothing
 * would let two writers corrupt a file without either knowing. */
int __os_fcntl(int fd, int cmd, long arg)
{
    struct embk_stat st;
    if (embk_is_err(embk_syscall2(EMBK_SYS_fstat, fd, (int64_t)(intptr_t)&st))) {
        errno = EBADF;
        return -1;
    }
    switch (cmd) {
    case __OS_F_GETFD: {
        int64_t r = embk_syscall3(EMBK_SYS_fcntl, fd, 4, 0);
        return embk_is_err(r) ? (int)fail((long)r) : (r ? __OS_FD_CLOEXEC : 0);
    }
    case __OS_F_SETFD:
        if (arg & ~(long)__OS_FD_CLOEXEC) {
            errno = EINVAL;
            return -1;
        }
        return set_cloexec(fd, (int)(arg & __OS_FD_CLOEXEC));
    case __OS_F_GETFL: {
        int64_t r = embk_syscall3(EMBK_SYS_fcntl, fd, 3, 0);
        if (embk_is_err(r))
            return (int)fail((long)r);
        return (int)(r & 3) | (r & EM_FD_NONBLOCK ? __OS_O_NONBLOCK : 0) |
               (r & EM_FD_APPEND ? __OS_O_APPEND : 0);
    }
    case __OS_F_SETFL: {
        /* the access mode cannot change after open, and Linux ignores
         * an attempt rather than failing it, so those bits are dropped */
        int st_bits = (arg & __OS_O_NONBLOCK ? EM_FD_NONBLOCK : 0) |
                      (arg & __OS_O_APPEND ? EM_FD_APPEND : 0);
        return ret0(embk_syscall3(EMBK_SYS_fcntl, fd, 6, st_bits));
    }
    }
    errno = cmd == __OS_F_GETLK || cmd == __OS_F_SETLK ||
            cmd == __OS_F_SETLKW ? ENOSYS : EINVAL;
    return -1;
}

/* There is no positional read: seek, read, seek back. Not atomic against
 * another thread on the same descriptor, which POSIX's pread is; the
 * callers are readers of a file they opened themselves, which is what
 * pread is nearly always for. */
long __os_pread(int fd, void *buf, size_t n, long long off)
{
    long here = __os_lseek(fd, 0, 1);
    if (here < 0 || __os_lseek(fd, (long)off, 0) < 0)
        return -1;
    long r = __os_read(fd, buf, n);
    int e = errno;
    __os_lseek(fd, here, 0);
    errno = e;
    return r;
}

long __os_pwrite(int fd, const void *buf, size_t n, long long off)
{
    long here = __os_lseek(fd, 0, 1);
    if (here < 0 || __os_lseek(fd, (long)off, 0) < 0)
        return -1;
    long r = __os_write(fd, buf, n);
    int e = errno;
    __os_lseek(fd, here, 0);
    errno = e;
    return r;
}

/* One write of the buffers joined, so a pipe or socket sees them as one
 * -- the atomicity writev promises a pipe for what fits in its buffer.
 * Past what a copy is worth, one write each. */
long __os_writev(int fd, const struct __os_iovec *iov, int n)
{
    size_t total = 0;
    for (int i = 0; i < n; i++)
        total += iov[i].len;
    if (total <= 4096) {
        unsigned char tmp[4096], *p = tmp;
        for (int i = 0; i < n; i++) {
            memcpy(p, iov[i].base, iov[i].len);
            p += iov[i].len;
        }
        return __os_write(fd, tmp, total);
    }
    long done = 0;
    for (int i = 0; i < n; i++) {
        long r = __os_write(fd, iov[i].base, iov[i].len);
        if (r < 0)
            return done ? done : -1;
        done += r;
        if ((size_t)r < iov[i].len)
            break;
    }
    return done;
}

long __os_readv(int fd, const struct __os_iovec *iov, int n)
{
    long done = 0;
    for (int i = 0; i < n; i++) {
        long r = __os_read(fd, iov[i].base, iov[i].len);
        if (r < 0)
            return done ? done : -1;
        done += r;
        if ((size_t)r < iov[i].len)
            break;
    }
    return done;
}

/* A real flush: write() returns once the bytes are in the page cache, and
 * this does not return until the file's dirty pages are on the device.
 * EMBKFS commits data and inode in one copy-on-write transaction, so
 * fdatasync has no cheaper flush to be. */
int __os_fsync(int fd, int data_only)
{
    (void)data_only;
    return ret0(embk_fsync(fd));
}

int __os_ftruncate(int fd, long long size)
{
    if (size < 0) {
        errno = EINVAL;
        return -1;
    }
    return ret0(embk_syscall2(EMBK_SYS_ftruncate, fd, (int64_t)size));
}

/* ---- the filesystem, the rest -------------------------------------------------- */

/* With no ids and no permission bits, nothing can deny access to what
 * exists: the answer is whether it exists. If permissions ever land,
 * this must start consulting them. */
int __os_access(const char *path, int mode)
{
    (void)mode;
    struct __os_fileinfo fi;
    return __os_stat(path, &fi);
}

/* nothing for a mask to filter: the mask is 0 and stays so */
unsigned __os_umask(unsigned mask)
{
    (void)mask;
    return 0;
}

/* ---- processes ------------------------------------------------------------------ */

/* One user, with the whole machine's authority. The kernel tracks a
 * parent but no call names it, and authority over a child is a handle,
 * not pid arithmetic: 0 is "not visible". */
long __os_getid(int which)
{
    return which == __OS_ID_PID ? (long)embk_getpid() : 0;
}

/* becoming the only user there is: vacuous; anyone else: there is no one */
int __os_setid(int which, unsigned id)
{
    (void)which;
    if (id == 0)
        return 0;
    errno = EPERM;
    return -1;
}

int __os_getgroups(int n, unsigned *list)
{
    (void)n; (void)list;
    return 0;
}

/* posix_spawn on the kernel's explicit actions: a snapshot of the
 * caller's descriptors, its working directory, and a request that the
 * child's status be kept until it is waited for -- then the caller's own
 * actions in order, their paths made absolute against the directory the
 * actions so far have reached. Attributes are refused: there are no
 * groups, identities or signals for them to set, and accepting one would
 * be a promise nothing keeps. */
#define EM_ACTIONS (EMBK_SPAWN_ACTIONS_MAX - 3)

int __os_spawn(int *pid, const char *path, const struct __os_spawn_action *a,
               int na, const struct __os_spawn_attr *attr,
               char *const argv[], char *const envp[])
{
    if (attr && attr->flags) {
        errno = ENOTSUP;
        return -1;
    }
    if (na > EM_ACTIONS) {
        errno = E2BIG;
        return -1;
    }
    if (!argv) {
        errno = EINVAL;
        return -1;
    }
    int argc = 0;
    while (argv[argc])
        if (++argc >= 128) {
            errno = E2BIG;
            return -1;
        }
    struct embk_spawn_file_action *k = calloc(EMBK_SPAWN_ACTIONS_MAX, sizeof *k);
    if (!k) {
        errno = ENOMEM;
        return -1;
    }
    char cwd[EM_PATH_MAX], exe[EM_PATH_MAX];
    memcpy(cwd, g_cwd, sizeof cwd);
    k[0].kind = EMBK_SPAWN_ACTION_FD_SNAPSHOT;
    k[1].kind = EMBK_SPAWN_ACTION_CWD;
    memcpy(k[1].path, cwd, sizeof k[1].path);
    k[2].kind = EMBK_SPAWN_ACTION_KEEP_STATUS;
    int n = 3, err = 0;
    for (int i = 0; i < na && !err; i++, n++) {
        const struct __os_spawn_action *x = &a[i];
        struct embk_spawn_file_action *y = &k[n];
        if (x->fd < 0 || x->fd >= 64 ||
            (x->kind == __OS_SPAWN_DUP2 && (x->newfd < 0 || x->newfd >= 64))) {
            err = EBADF;
            break;
        }
        switch (x->kind) {
        case __OS_SPAWN_OPEN:
            y->kind = EMBK_SPAWN_ACTION_OPEN;
            y->target_fd = x->fd;
            y->flags = open_flags(x->oflag);
            y->mode = x->mode;
            if (path_from(cwd, x->path, y->path, sizeof y->path) < 0)
                err = errno;
            break;
        case __OS_SPAWN_CLOSE:
            y->kind = EMBK_SPAWN_ACTION_CLOSE;
            y->target_fd = x->fd;
            break;
        case __OS_SPAWN_DUP2:
            y->kind = EMBK_SPAWN_ACTION_DUP2;
            y->target_fd = x->newfd;
            y->src_obj_handle = x->fd;
            break;
        case __OS_SPAWN_CHDIR:
            y->kind = EMBK_SPAWN_ACTION_CWD;
            if (path_from(cwd, x->path, y->path, sizeof y->path) < 0)
                err = errno;
            else
                memcpy(cwd, y->path, sizeof cwd);
            break;
        default:              /* FCHDIR: a descriptor has no path here */
            err = ENOTSUP;
        }
    }
    if (!err && path_from(cwd, path, exe, sizeof exe) < 0)
        err = errno;
    if (err) {
        free(k);
        errno = err;
        return -1;
    }
    int64_t h = embk_syscall6(EMBK_SYS_spawn_pid, (int64_t)(uintptr_t)exe,
                              (int64_t)(uintptr_t)argv, argc,
                              (int64_t)(uintptr_t)k, n,
                              (int64_t)(uintptr_t)envp);
    free(k);
    if (embk_is_err(h))
        return (int)fail((long)h);
    *pid = (int)h;
    return 0;
}

/* The children this process owns, by pid or any (-1): the status as
 * POSIX encodes it, which the kernel copies out itself. A process group
 * (0, or below -1) names nothing here. A cancelled wait is EINTR. */
#define EM_ECANCELED 125

int __os_waitpid(int pid, int *status, int options)
{
    if (options & ~1) {               /* only WNOHANG means anything */
        errno = EINVAL;
        return -1;
    }
    if (pid == 0 || pid < -1) {
        errno = ENOSYS;
        return -1;
    }
    int64_t r = embk_syscall5(EMBK_SYS_child, pid, EMBK_CHILD_WAIT_STATUS,
                              (int64_t)(intptr_t)status,
                              status ? sizeof *status : 0,
                              options & 1 ? EMBK_CHILD_NOHANG : 0);
    if (embk_is_err(r)) {
        errno = r == -EM_ECANCELED ? EINTR : (int)-r;
        return -1;
    }
    return (int)r;
}

/* ---- memory ------------------------------------------------------------------------
 *
 * What the kernel cannot honour is refused at the call, not approximated:
 *   MAP_FIXED, or an address asked for   ENOTSUP -- the kernel picks it
 *   an anonymous MAP_SHARED              ENOTSUP -- shared with nobody,
 *                                         there being no fork to share with
 *   an offset not page-aligned           EINVAL
 *   PROT_WRITE with PROT_EXEC            EINVAL, from the kernel: write,
 *                                         then mprotect to executable */
void *__os_mmap(void *addr, size_t len, int prot, int flags, int fd,
                long long off)
{
    if ((flags & 0x10) || addr) {
        errno = ENOTSUP;
        return (void *)-1;
    }
    if (!len || off < 0 || (off & 0xfff)) {
        errno = EINVAL;
        return (void *)-1;
    }
    if (fd < 0 || (flags & 0x20)) {
        if (off) {
            errno = EINVAL;
            return (void *)-1;
        }
        if (flags & 0x01) {
            errno = ENOTSUP;
            return (void *)-1;
        }
        fd = -1;
    } else if (!(flags & 0x03)) {
        errno = EINVAL;               /* a file mapping must say which */
        return (void *)-1;
    }
    int64_t r = embk_mmap_fd(len, prot, flags, fd, (uint64_t)off);
    if (embk_is_err(r)) {
        fail((long)r);
        return (void *)-1;
    }
    return (void *)(intptr_t)r;
}

int __os_munmap(void *addr, size_t len) { return ret0(embk_munmap(addr, len)); }

int __os_mprotect(void *addr, size_t len, int prot)
{
    return ret0(embk_mprotect(addr, len, prot));
}

/* ---- the system ---------------------------------------------------------------------- */

/* what this OS is, nothing guessed: one machine, no network name */
int __os_uname(struct __os_utsname *u)
{
    memset(u, 0, sizeof *u);
    strcpy(u->sysname, "EmbLink");
    strcpy(u->nodename, "emblink");
    strcpy(u->release, "1.0");
    strcpy(u->version, "EmbLinkOS");
#if defined(__aarch64__)
    strcpy(u->machine, "aarch64");
#else
    strcpy(u->machine, "x86_64");
#endif
    return 0;
}

/* The page size is the VMM's. No call reports the processor count or the
 * memory size in pages, and a plausible guess would be a fabricated answer
 * a caller could not tell from a measured one. */
long __os_sysinfo(int what)
{
    if (what == __OS_INFO_PAGESIZE)
        return 4096;
    errno = EINVAL;
    return -1;
}

/* ---- waiting on descriptors -------------------------------------------------------
 *
 * The kernel answers "is this descriptor ready" one descriptor at a time
 * (fd_poll), with no way to sleep on several. So the set is asked, and if
 * nothing is ready the thread sleeps 10 ms -- which is also what lets the
 * network's receive thread move a connecting socket on -- and asks again,
 * until the timeout. Not a scalable event loop; exactly what a blocking
 * call with a timeout needs. */
int __os_poll(struct __os_pollfd *fds, unsigned long n,
              const struct __os_timespec *timeout,
              const unsigned long long *sigmask)
{
    (void)sigmask;                   /* no signals to mask */
    long long left = timeout ? timeout->sec * 1000 + timeout->nsec / 1000000
                             : -1;
    for (;;) {
        int ready = 0;
        for (unsigned long i = 0; i < n; i++) {
            fds[i].revents = 0;
            if (fds[i].fd < 0)
                continue;
            int want = (fds[i].events & (EMBK_POLLIN | EMBK_POLLOUT)) |
                       EMBK_POLLERR;
            int re = embk_fd_poll(fds[i].fd, want);
            if (re < 0)
                fds[i].revents = 0x20;                 /* POLLNVAL */
            else
                fds[i].revents = (short)(re & (fds[i].events | EMBK_POLLERR |
                                              EMBK_POLLHUP));
            if (fds[i].revents)
                ready++;
        }
        if (ready || left == 0)
            return ready;
        long step = left > 0 && left < 10 ? (long)left : 10;
        embk_sleep_ms((uint64_t)step);
        if (left > 0)
            left -= step;
    }
}

/* ---- sockets ------------------------------------------------------------------------
 *
 * IPv4, over the kernel's own network layer: a socket is a real
 * descriptor, so read, write and close need nothing here. Addresses are
 * network order in a sockaddr_in and host order to the kernel. */
#define EM_AF_INET 2
#define EM_SOCK_STREAM 1
#define EM_SOCK_DGRAM 2

struct em_sin {
    unsigned short family, port;
    unsigned int addr;
    unsigned char zero[8];
};

static unsigned int be32(unsigned int v) { return __builtin_bswap32(v); }
static unsigned short be16(unsigned short v) { return __builtin_bswap16(v); }

static void sin_fill(void *addr, unsigned *len, unsigned ip, unsigned port)
{
    if (!addr || !len || *len < sizeof(struct em_sin))
        return;
    struct em_sin s = { EM_AF_INET, be16((unsigned short)port), be32(ip), { 0 } };
    memcpy(addr, &s, sizeof s);
    *len = sizeof s;
}

static void fd_flags(int fd, int type)
{
    if (type & 0x800)                                 /* SOCK_NONBLOCK */
        embk_syscall3(EMBK_SYS_fcntl, fd, 6, EM_FD_NONBLOCK);
    if (type & 0x80000)                               /* SOCK_CLOEXEC */
        set_cloexec(fd, 1);
}

int __os_socket(int domain, int type, int protocol)
{
    (void)protocol;
    if (domain != EM_AF_INET) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    int base = type & ~(0x800 | 0x80000);
    if (base != EM_SOCK_STREAM && base != EM_SOCK_DGRAM) {
        errno = EPROTONOSUPPORT;
        return -1;
    }
    int fd = embk_net_socket(base);
    if (fd < 0)
        return (int)fail(fd);
    fd_flags(fd, type);
    return fd;
}

int __os_connect(int fd, const void *addr, unsigned len)
{
    struct em_sin s;
    if (!addr || len < sizeof s) {
        errno = EINVAL;
        return -1;
    }
    memcpy(&s, addr, sizeof s);
    if (s.family != EM_AF_INET) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    return ret0(embk_net_connect(fd, be32(s.addr), be16(s.port)));
}

/* the kernel binds a port; the address is every interface there is */
int __os_bind(int fd, const void *addr, unsigned len)
{
    struct em_sin s;
    if (!addr || len < sizeof s) {
        errno = EINVAL;
        return -1;
    }
    memcpy(&s, addr, sizeof s);
    return ret0(embk_net_bind(fd, be16(s.port)));
}

int __os_listen(int fd, int backlog) { return ret0(embk_net_listen(fd, backlog)); }

/* the peer's address is not tracked: AF_INET with zeros */
int __os_accept(int fd, void *addr, unsigned *len, int flags)
{
    int nfd = embk_net_accept(fd);
    if (nfd < 0)
        return (int)fail(nfd);
    sin_fill(addr, len, 0, 0);
    fd_flags(nfd, flags);
    return nfd;
}

long __os_sendto(int fd, const void *buf, size_t n, int flags,
                 const void *addr, unsigned alen)
{
    (void)flags;
    if (!addr)
        return __os_write(fd, buf, n);
    struct em_sin s;
    if (alen < sizeof s) {
        errno = EINVAL;
        return -1;
    }
    memcpy(&s, addr, sizeof s);
    int r = embk_net_sendto(fd, be32(s.addr), be16(s.port), buf, n);
    return r < 0 ? fail(r) : (long)n;
}

long __os_recvfrom(int fd, void *buf, size_t n, int flags, void *addr,
                   unsigned *alen)
{
    (void)flags;
    if (!addr)
        return __os_read(fd, buf, n);
    unsigned int ip = 0;
    unsigned short port = 0;
    int r = embk_net_recvfrom(fd, buf, n, &ip, &port);
    if (r < 0)
        return fail(r);
    sin_fill(addr, alen, ip, port);
    return r;
}

/* No half-close in the kernel: the connection ends at close. Succeeding
 * lets the libraries that shut down before closing go on to close. */
int __os_shutdown(int fd, int how)
{
    (void)fd; (void)how;
    return 0;
}

/* The options are advisory here (SO_REUSEADDR, TCP_NODELAY, ...) and
 * accepted. SO_ERROR is how a caller reaps a non-blocking connect: an
 * error on the descriptor reads as ECONNREFUSED, else none. */
int __os_setsockopt(int fd, int level, int name, const void *val,
                    unsigned len)
{
    (void)fd; (void)level; (void)name; (void)val; (void)len;
    return 0;
}

int __os_getsockopt(int fd, int level, int name, void *val, unsigned *len)
{
    (void)level;
    if (val && len && *len >= sizeof(int)) {
        int v = 0;
        if (name == 4 && (embk_fd_poll(fd, EMBK_POLLERR) & EMBK_POLLERR))
            v = ECONNREFUSED;                          /* SO_ERROR */
        memcpy(val, &v, sizeof v);
        *len = sizeof v;
    }
    return 0;
}

int __os_getsockname(int fd, void *addr, unsigned *len)
{
    (void)fd;
    sin_fill(addr, len, 0, 0);
    return 0;
}

int __os_getpeername(int fd, void *addr, unsigned *len)
{
    return __os_getsockname(fd, addr, len);
}

/* The kernel's resolver, IPv4: one address, or none for a name it cannot
 * find. Asked for IPv6, it has none to give. */
int __os_resolve(const char *name, int family, unsigned char *addrs, int max)
{
    if (family != EM_AF_INET || max < 1)
        return 0;
    unsigned int ip;
    if (embk_net_resolve(name, &ip) != 0)
        return 0;
    unsigned int be = be32(ip);
    memcpy(addrs, &be, 4);
    return 1;
}

/* ---- terminals -------------------------------------------------------------------------
 *
 * The kernel's terminal attributes are Linux's flag bits and control
 * character indices, the first EMBK_TCC_COUNT of them. A mode asking for
 * one past those cannot be set and is refused, as is TCSADRAIN, which
 * the kernel has no way to wait for. */
int __os_tcgetattr(int fd, struct __os_termios *t)
{
    struct embk_terminal_attr a;
    int r = embk_terminal_control(fd, EMBK_TERMINAL_GET_ATTR, &a, sizeof a);
    if (r < 0)
        return (int)fail(r);
    memset(t, 0, sizeof *t);
    t->iflag = a.iflag;
    t->oflag = a.oflag;
    t->cflag = a.cflag;
    t->lflag = a.lflag;
    memcpy(t->cc, a.cc, sizeof a.cc);
    t->ispeed = t->ospeed = a.cflag & 0010017;
    return 0;
}

int __os_tcsetattr(int fd, int action, const struct __os_termios *t)
{
    if (action != 0 && action != 2) {
        errno = action == 1 ? ENOTSUP : EINVAL;
        return -1;
    }
    for (unsigned i = EMBK_TCC_COUNT; i < sizeof t->cc; i++)
        if (t->cc[i]) {
            errno = ENOTSUP;
            return -1;
        }
    struct embk_terminal_attr a;
    memset(&a, 0, sizeof a);
    a.iflag = t->iflag;
    a.oflag = t->oflag;
    a.cflag = t->cflag;
    a.lflag = t->lflag;
    memcpy(a.cc, t->cc, sizeof a.cc);
    int r = embk_terminal_control(fd, action == 2 ? EMBK_TERMINAL_SET_ATTR_FLUSH
                                                  : EMBK_TERMINAL_SET_ATTR,
                                  &a, sizeof a);
    return r < 0 ? (int)fail(r) : 0;
}

/* The requests a program asks of a terminal: its size, its modes, and
 * whether a descriptor blocks. Any other is not one this device knows. */
#define L_TCGETS     0x5401
#define L_TCSETS     0x5402
#define L_TCSETSF    0x5404
#define L_TIOCGWINSZ 0x5413
#define L_TIOCSWINSZ 0x5414
#define L_FIONBIO    0x5421

int __os_ioctl(int fd, unsigned long request, void *arg)
{
    switch (request) {
    case L_TIOCGWINSZ:
    case L_TIOCSWINSZ: {
        unsigned short *w = arg;
        struct embk_terminal_size s;
        if (!w) {
            errno = EFAULT;
            return -1;
        }
        if (request == L_TIOCSWINSZ) {
            s.rows = w[0];
            s.cols = w[1];
            s.xpixel = w[2];
            s.ypixel = w[3];
        }
        int r = embk_terminal_control(fd, request == L_TIOCGWINSZ
                                              ? EMBK_TERMINAL_GET_SIZE
                                              : EMBK_TERMINAL_SET_SIZE,
                                      &s, sizeof s);
        if (r < 0)
            return (int)fail(r);
        if (request == L_TIOCGWINSZ) {
            w[0] = s.rows;
            w[1] = s.cols;
            w[2] = s.xpixel;
            w[3] = s.ypixel;
        }
        return 0;
    }
    case L_TCGETS:
        return __os_tcgetattr(fd, arg);
    case L_TCSETS:
    case L_TCSETSF:
        return __os_tcsetattr(fd, request == L_TCSETS ? 0 : 2, arg);
    case L_FIONBIO: {
        int on = arg ? *(int *)arg : 0;
        int64_t fl = embk_syscall3(EMBK_SYS_fcntl, fd, 3, 0);
        if (embk_is_err(fl))
            return (int)fail((long)fl);
        int bits = (int)fl & (EM_FD_NONBLOCK | EM_FD_APPEND);
        bits = on ? bits | EM_FD_NONBLOCK : bits & ~EM_FD_NONBLOCK;
        return ret0(embk_syscall3(EMBK_SYS_fcntl, fd, 6, bits));
    }
    }
    errno = ENOTTY;
    return -1;
}

/* ---- the rest of POSIX's operating system: what this OS has not got ---------- */

#define OS_HAS_fstat
#define OS_HAS_dup
#define OS_HAS_dup2
#define OS_HAS_pipe
#define OS_HAS_fcntl
#define OS_HAS_pread
#define OS_HAS_pwrite
#define OS_HAS_readv
#define OS_HAS_writev
#define OS_HAS_fsync
#define OS_HAS_ftruncate
#define OS_HAS_ioctl
#define OS_HAS_access
#define OS_HAS_umask
#define OS_HAS_readdir_entry
#define OS_HAS_rewinddir
#define OS_HAS_dirfd
#define OS_HAS_getid
#define OS_HAS_setid
#define OS_HAS_getgroups
#define OS_HAS_spawn
#define OS_HAS_waitpid
#define OS_HAS_clock_gettime
#define OS_HAS_clock_getres
#define OS_HAS_clock_nanosleep
#define OS_HAS_times
#define OS_HAS_mmap
#define OS_HAS_munmap
#define OS_HAS_mprotect
#define OS_HAS_uname
#define OS_HAS_sysinfo
#define OS_HAS_poll
#define OS_HAS_socket
#define OS_HAS_bind
#define OS_HAS_connect
#define OS_HAS_listen
#define OS_HAS_accept
#define OS_HAS_sendto
#define OS_HAS_recvfrom
#define OS_HAS_shutdown
#define OS_HAS_getsockopt
#define OS_HAS_setsockopt
#define OS_HAS_getsockname
#define OS_HAS_getpeername
#define OS_HAS_resolve
#define OS_HAS_tcgetattr
#define OS_HAS_tcsetattr
#include "../absent.inc"
