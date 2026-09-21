/* The backend for Linux.
 *
 * Linux is the third operating system under lib/libc/os/backend.h, and
 * the first one this library reaches WITHOUT another C library
 * underneath it. The EmbLinkOS backend calls that kernel directly; the
 * posixlike backend forwards to whatever host library is already there.
 * This one is the first kind: it issues syscalls, so a program built
 * against it needs no glibc, no musl, no dynamic loader and no crt from
 * anyone else. `embcc hello.c` produces a static ELF whose only
 * dependency is the kernel.
 *
 * That choice is deliberate and it is the same choice made for
 * EmbLinkOS. A backend that called glibc's write() would give a program
 * two stdio implementations, two errno variables and two heaps, and the
 * seam exists precisely so that does not happen.
 *
 * ---- what this file assumes, and what checks it ---------------------------
 *
 * Every number it depends on -- syscall numbers, errno values, open
 * flags, statx offsets -- is Linux's, not ours, and none of them can be
 * read out of a header on a macOS build machine. So they are written
 * down in syscall.h with their source named, and tests/golden/linux.sh
 * re-derives them from the kernel's own headers when it runs ON Linux.
 * Here, it reports that it could not. That is the honest arrangement:
 * the numbers are checked where they are checkable, and the test says
 * plainly which machine it was on.
 *
 * ---- errno ----------------------------------------------------------------
 *
 * The kernel returns a small negative errno. Those numbers and the ones
 * in lib/libc/include/errno.h are the same numbers -- EPERM 1, EBADF 9,
 * EINVAL 22, ENOSYS 38, ELOOP 40 -- because both copied the same
 * traditional table. So the mapping is the identity, exactly as it is
 * for EmbLinkOS, and saying so here is better than a translation table
 * that would rot the first time either side added an entry.
 */
#include "../backend.h"
#include "syscall.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The kernel's -errno convention turned into the seam's. Every result
 * that can fail goes through one of these two, so there is one place
 * where errno is set and one place to get it wrong. */
static long fail(long r)
{
    errno = (int)-r;
    return -1;
}

static long ret(long r)
{
    return r < 0 ? fail(r) : r;
}

/* A syscall whose only outcomes are "worked" and "did not": the seam
 * wants 0 or -1, not the kernel's 0-or-negative. */
static int ret0(long r)
{
    return r < 0 ? (int)fail(r) : 0;
}

/* ---- bytes ---------------------------------------------------------------- */

long __os_write(int fd, const void *buf, size_t n)
{
    return ret(lsys3(LSYS_write, fd, (long)buf, (long)n));
}

long __os_read(int fd, void *buf, size_t n)
{
    return ret(lsys3(LSYS_read, fd, (long)buf, (long)n));
}

/* ---- files by descriptor --------------------------------------------------
 *
 * The seam's __OS_O_* values are the ones Linux x86 chose, and
 * asm-generic/fcntl.h -- which is what aarch64 uses -- copied them. So
 * the flags pass straight through on both architectures. The golden
 * test asserts that against <fcntl.h> rather than leaving it as a
 * comment.
 */
int __os_open(const char *path, int flags, int mode)
{
    return (int)ret(lsys4(LSYS_openat, LAT_FDCWD, (long)path, flags, mode));
}

int __os_close(int fd)
{
    return ret0(lsys1(LSYS_close, fd));
}

long __os_lseek(int fd, long off, int whence)
{
    return ret(lsys3(LSYS_lseek, fd, off, whence));
}

/* ---- files by name --------------------------------------------------------
 *
 * C's remove() takes down a file OR an empty directory, and Linux
 * splits that across one syscall with two flag settings. The retry is
 * on EISDIR because that is what unlinkat reports for a directory --
 * not a guess about which of the two the caller meant.
 */
int __os_remove(const char *path)
{
    long r = lsys3(LSYS_unlinkat, LAT_FDCWD, (long)path, 0);
    if (r == -EISDIR)
        r = lsys3(LSYS_unlinkat, LAT_FDCWD, (long)path, LAT_REMOVEDIR);
    return ret0(r);
}

int __os_rename(const char *from, const char *to)
{
    return ret0(lsys4(LSYS_renameat, LAT_FDCWD, (long)from,
                      LAT_FDCWD, (long)to));
}

/* ---- the heap -------------------------------------------------------------
 *
 * brk(2) is the one syscall here that does not use the -errno
 * convention: it returns the break the process ENDED UP WITH, which on
 * failure is the break it already had. So failure is detected by
 * comparing, not by a sign. A raw brk is also the reason this library
 * wants a static image -- with no dynamic loader in the address space
 * there is nobody else to grow the same break.
 */
void *__os_sbrk(long increment)
{
    long cur = lsys1(LSYS_brk, 0);
    if (cur <= 0) {
        errno = ENOMEM;
        return (void *)-1;
    }
    if (increment == 0)
        return (void *)cur;

    long want = cur + increment;
    if (increment > 0 ? want < cur : want > cur) {   /* wrapped */
        errno = ENOMEM;
        return (void *)-1;
    }
    long got = lsys1(LSYS_brk, want);
    if (got != want) {
        errno = ENOMEM;
        return (void *)-1;
    }
    return (void *)cur;
}

/* ---- time -----------------------------------------------------------------
 *
 * Both clocks come from clock_gettime rather than from time(2) and
 * times(2): aarch64's syscall table has neither of the latter, and one
 * call that both architectures have is better than two that need an
 * #ifdef.
 *
 * On a kernel with a vDSO, glibc would read these without entering the
 * kernel at all. This does not, and a program that calls clock() in a
 * tight loop pays a syscall each time. That is a speed difference, not
 * a correctness one, and the vDSO can be taught later; going through
 * the vDSO requires parsing the auxiliary vector and an ELF image in
 * the process's own address space, which is a great deal of machinery
 * for a first port.
 */
long __os_time(void)
{
    struct ltimespec ts;
    if (lsys2(LSYS_clock_gettime, LCLOCK_REALTIME, (long)&ts) < 0)
        return -1;
    return ts.tv_sec;
}

long __os_clock_ns(void)
{
    struct ltimespec ts;
    if (lsys2(LSYS_clock_gettime, LCLOCK_PROCESS_CPUTIME, (long)&ts) < 0)
        return -1;
    return ts.tv_sec * 1000000000L + ts.tv_nsec;
}

/* ---- stopping -------------------------------------------------------------
 *
 * exit_group, not exit: exit(2) ends the calling THREAD and leaves the
 * others running, which for a program that called exit() is not what
 * happened. The loop after it is unreachable and exists so that a
 * kernel that somehow returned does not fall off the end of a function
 * declared noreturn.
 */
void __os_exit(int status)
{
    lsys1(LSYS_exit_group, status);
    for (;;)
        lsys1(LSYS_exit, status);
}

/* Asking the terminal driver a question only a terminal can answer.
 * The buffer is `struct termios`, which this file never reads -- only
 * whether the kernel was willing to fill it. 64 bytes is comfortably
 * more than the 36 or 60 the structure occupies. */
int __os_isatty(int fd)
{
    char termios[64];
    return lsys3(LSYS_ioctl, fd, LTCGETS, (long)termios) == 0;
}

/* getrandom can return short -- it is interruptible once it has blocked
 * for entropy -- so this loops. The 256-byte limit is getentropy's own
 * contract, kept here so that a caller asking for more is told rather
 * than quietly given a partially-filled buffer. */
int __os_getentropy(void *buf, size_t n)
{
    if (n > 256) {
        errno = EIO;
        return -1;
    }
    char *p = (char *)buf;
    while (n) {
        long r = lsys3(LSYS_getrandom, (long)p, (long)n, 0);
        if (r < 0) {
            if (r == -EINTR)
                continue;
            return (int)fail(r);
        }
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

/* ---- threads, and blocking ------------------------------------------------
 *
 * The lifecycle half -- create, join, detach, self -- is in thread.c,
 * because it needs clone(2) and a per-architecture assembly entry and
 * is a different kind of thing from a syscall wrapper. What is left
 * here is what is: yield, sleep, and the futex operation every mutex,
 * condition variable, semaphore and latch in the C++ library is built
 * from.
 */
void __os_thread_yield(void)
{
    lsys0(LSYS_sched_yield);
}

int __os_sleep_ns(long ns)
{
    struct ltimespec req;
    req.tv_sec = ns / 1000000000L;
    req.tv_nsec = ns % 1000000000L;
    return ret0(lsys2(LSYS_nanosleep, (long)&req, 0));
}

/* The primitive everything above is built from, and the reason the seam
 * asks for a word rather than a mutex. EAGAIN from FUTEX_WAIT means the
 * word had already changed before the kernel looked -- the caller's
 * condition is satisfied, so it is success here, not failure. */
int __os_futex_wait(const volatile int *addr, int expected, long timeout_ns)
{
    struct ltimespec ts, *tp = 0;
    if (timeout_ns >= 0) {
        ts.tv_sec = timeout_ns / 1000000000L;
        ts.tv_nsec = timeout_ns % 1000000000L;
        tp = &ts;
    }
    long r = lsys4(LSYS_futex, (long)addr, LFUTEX_WAIT | LFUTEX_PRIVATE,
                   expected, (long)tp);
    if (r == -EAGAIN || r == -EINTR)
        return 0;
    return ret0(r);
}

int __os_futex_wake(const volatile int *addr, int count)
{
    return (int)ret(lsys3(LSYS_futex, (long)addr,
                          LFUTEX_WAKE | LFUTEX_PRIVATE, count));
}

/* ---- the filesystem -------------------------------------------------------
 *
 * statx, not fstatat, and the reason is in syscall.h: `struct stat` is
 * laid out differently on x86-64 and on aarch64, so one shared backend
 * using it would need two structures and would read the wrong offsets
 * the moment either was wrong. statx has one layout everywhere.
 *
 * The cost is a kernel floor: statx arrived in Linux 4.11, March 2017.
 * An older kernel returns ENOSYS and <filesystem> reports that as a
 * filesystem_error, which is a program that says what is wrong rather
 * than one that reads a file's size out of its permission bits.
 */
static int type_of_mode(unsigned mode)
{
    switch (mode & LS_IFMT) {
    case LS_IFREG:  return __OS_FT_REGULAR;
    case LS_IFDIR:  return __OS_FT_DIRECTORY;
    case LS_IFLNK:  return __OS_FT_SYMLINK;
    case LS_IFCHR:  return __OS_FT_CHARDEV;
    case LS_IFBLK:  return __OS_FT_BLOCKDEV;
    case LS_IFIFO:  return __OS_FT_FIFO;
    case LS_IFSOCK: return __OS_FT_SOCKET;
    default:        return __OS_FT_UNKNOWN;
    }
}

/* statx reports a device as its major and minor separately; the seam
 * wants one number, and only ever compares it against another this
 * function produced (equivalent() asks whether two paths are one file).
 * The encoding is glibc's makedev anyway, so the number also means what
 * it means elsewhere. */
static unsigned long long makedev(unsigned maj, unsigned min)
{
    return ((unsigned long long)(maj & 0xfffff000u) << 32)
         | ((unsigned long long)(maj & 0x00000fffu) << 8)
         | ((unsigned long long)(min & 0xffffff00u) << 12)
         |  (unsigned long long)(min & 0x000000ffu);
}

static int do_stat(const char *path, struct __os_fileinfo *out, int flags)
{
    struct lstatx sx;
    memset(&sx, 0, sizeof sx);
    long r = lsys5(LSYS_statx, LAT_FDCWD, (long)path,
                   flags | LAT_STATX_SYNC_AS_STAT,
                   LSTATX_BASIC_STATS, (long)&sx);
    if (r < 0) {
        /* A path that is not there is not an error worth a message --
         * exists() asks this question constantly -- so it is reported
         * as the type NOT_FOUND with errno still set, which is what
         * the seam's other backends do. */
        if (r == -ENOENT || r == -ENOTDIR) {
            memset(out, 0, sizeof *out);
            out->type = __OS_FT_NOT_FOUND;
            errno = (int)-r;
            return -1;
        }
        return (int)fail(r);
    }
    out->size   = sx.stx_size;
    out->mtime  = (long)sx.stx_mtime.tv_sec;
    out->mode   = sx.stx_mode & 07777u;
    out->type   = type_of_mode(sx.stx_mode);
    out->dev    = makedev(sx.stx_dev_major, sx.stx_dev_minor);
    out->ino    = sx.stx_ino;
    out->nlink  = sx.stx_nlink;
    return 0;
}

int __os_stat(const char *path, struct __os_fileinfo *out)
{
    return do_stat(path, out, 0);
}

int __os_lstat(const char *path, struct __os_fileinfo *out)
{
    return do_stat(path, out, LAT_SYMLINK_NOFOLLOW);
}

int __os_mkdir(const char *path, unsigned mode)
{
    return ret0(lsys3(LSYS_mkdirat, LAT_FDCWD, (long)path, (long)mode));
}

int __os_rmdir(const char *path)
{
    return ret0(lsys3(LSYS_unlinkat, LAT_FDCWD, (long)path, LAT_REMOVEDIR));
}

int __os_unlink(const char *path)
{
    return ret0(lsys3(LSYS_unlinkat, LAT_FDCWD, (long)path, 0));
}

int __os_chmod(const char *path, unsigned mode)
{
    return ret0(lsys4(LSYS_fchmodat, LAT_FDCWD, (long)path, (long)mode, 0));
}

int __os_truncate(const char *path, long long size)
{
    return ret0(lsys2(LSYS_truncate, (long)path, (long)size));
}

/* last_write_time sets the modification time and nothing else, so the
 * access time is left alone rather than set to the same value.
 * UTIME_OMIT is the kernel's word for "leave this one". */
#define LUTIME_OMIT 0x3ffffffeL

int __os_utime(const char *path, long mtime)
{
    struct ltimespec times[2];
    times[0].tv_sec = 0;
    times[0].tv_nsec = LUTIME_OMIT;
    times[1].tv_sec = mtime;
    times[1].tv_nsec = 0;
    return ret0(lsys4(LSYS_utimensat, LAT_FDCWD, (long)path, (long)times, 0));
}

/* symlinkat and linkat take their arguments in the order the syscalls
 * chose, which is not the order this seam names them in. Getting this
 * backwards produces a link that exists and points the wrong way, so
 * the mapping is written out rather than passed through. */
int __os_symlink(const char *target, const char *linkpath)
{
    return ret0(lsys3(LSYS_symlinkat, (long)target,
                      LAT_FDCWD, (long)linkpath));
}

int __os_link(const char *target, const char *linkpath)
{
    return ret0(lsys5(LSYS_linkat, LAT_FDCWD, (long)target,
                      LAT_FDCWD, (long)linkpath, 0));
}

/* readlinkat does NOT terminate what it writes, and returns the length
 * it wrote. A caller that forgot that reads past the link. */
long __os_readlink(const char *path, char *buf, size_t n)
{
    return ret(lsys4(LSYS_readlinkat, LAT_FDCWD, (long)path,
                     (long)buf, (long)n));
}

int __os_getcwd(char *buf, size_t n)
{
    return ret0(lsys2(LSYS_getcwd, (long)buf, (long)n));
}

int __os_chdir(const char *path)
{
    return ret0(lsys1(LSYS_chdir, (long)path));
}

/* space() wants three numbers because "free" and "free to this caller"
 * differ wherever the superuser has a reservation, and statfs reports
 * both. The unit is f_frsize where the filesystem has one -- f_bsize is
 * a preferred I/O size, not necessarily the size of a block in
 * f_blocks -- so the fallback is only for filesystems that leave it
 * zero. */
int __os_statfs(const char *path, unsigned long long *capacity,
                unsigned long long *freespace, unsigned long long *available)
{
    struct lstatfs sf;
    memset(&sf, 0, sizeof sf);
    long r = lsys2(LSYS_statfs, (long)path, (long)&sf);
    if (r < 0)
        return (int)fail(r);
    unsigned long long unit = sf.f_frsize ? sf.f_frsize : sf.f_bsize;
    *capacity  = sf.f_blocks * unit;
    *freespace = sf.f_bfree  * unit;
    *available = sf.f_bavail * unit;
    return 0;
}

/* ---- reading a directory --------------------------------------------------
 *
 * getdents64 fills a buffer with variable-length records, so the handle
 * holds the buffer and a position in it and refills when it runs out.
 * The buffer is part of the handle rather than a separate allocation
 * because a directory read that succeeded and then failed to allocate
 * would be a second failure path for no benefit.
 */
struct ldir {
    int fd;
    int pos, len;
    char buf[4096];
};

void *__os_opendir(const char *path)
{
    long fd = lsys4(LSYS_openat, LAT_FDCWD, (long)path,
                    __OS_O_RDONLY | LO_DIRECTORY, 0);
    if (fd < 0) {
        fail(fd);
        return 0;
    }
    struct ldir *d = (struct ldir *)malloc(sizeof *d);
    if (!d) {
        lsys1(LSYS_close, fd);
        errno = ENOMEM;
        return 0;
    }
    d->fd = (int)fd;
    d->pos = d->len = 0;
    return d;
}

static int type_of_dtype(unsigned char t)
{
    switch (t) {
    case LDT_REG:  return __OS_FT_REGULAR;
    case LDT_DIR:  return __OS_FT_DIRECTORY;
    case LDT_LNK:  return __OS_FT_SYMLINK;
    case LDT_CHR:  return __OS_FT_CHARDEV;
    case LDT_BLK:  return __OS_FT_BLOCKDEV;
    case LDT_FIFO: return __OS_FT_FIFO;
    case LDT_SOCK: return __OS_FT_SOCKET;
    default:       return __OS_FT_UNKNOWN;
    }
}

int __os_readdir(void *dir, char *name, size_t n, int *type)
{
    struct ldir *d = (struct ldir *)dir;
    for (;;) {
        if (d->pos >= d->len) {
            long r = lsys3(LSYS_getdents64, d->fd, (long)d->buf,
                           (long)sizeof d->buf);
            if (r < 0)
                return (int)fail(r);
            if (r == 0)
                return 0;                       /* end, not an error */
            d->len = (int)r;
            d->pos = 0;
        }
        struct ldirent64 *e = (struct ldirent64 *)(d->buf + d->pos);
        d->pos += e->d_reclen;

        /* The seam filters `.` and `..` once, here, rather than in
         * every caller -- and a caller that forgot would walk a
         * directory tree forever. */
        if (e->d_name[0] == '.' &&
            (e->d_name[1] == 0 ||
             (e->d_name[1] == '.' && e->d_name[2] == 0)))
            continue;

        size_t len = strlen(e->d_name);
        if (len + 1 > n) {
            errno = ENAMETOOLONG;
            return -1;
        }
        memcpy(name, e->d_name, len + 1);
        if (type)
            *type = type_of_dtype(e->d_type);
        return 1;
    }
}

void __os_closedir(void *dir)
{
    struct ldir *d = (struct ldir *)dir;
    if (!d)
        return;
    lsys1(LSYS_close, d->fd);
    free(d);
}
