/* <unistd.h> and <fcntl.h>'s calls on descriptors: open and close, read
 * and write in their forms, dup and pipe, fcntl, sync, locks. Each is
 * its primitive (os/backend.h); what is here is argument shuffling and
 * what POSIX adds above the kernel -- lockf over fcntl, preadv over
 * pread. */
#include <fcntl.h>
#include <unistd.h>
#include <stdarg.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/uio.h>

#include "posix.h"

WEAK int openat(int dirfd, const char *path, int flags, ...)
{
    unsigned mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, unsigned);
        va_end(ap);
    }
    int saved = errno;
    int r = __os_openat(dirfd, path, flags, (int)mode);
    if (!__at_absent(r, saved))
        return r;
    char buf[PATH_MAX];
    const char *p = __at_path(dirfd, path, buf);
    return p ? __os_open(p, flags, (int)mode) : -1;
}

WEAK int creat(const char *path, mode_t mode)
{
    return __os_open(path, O_WRONLY | O_CREAT | O_TRUNC, (int)mode);
}


WEAK ssize_t pread(int fd, void *buf, size_t n, off_t off)
{
    return __os_pread(fd, buf, n, off);
}

WEAK ssize_t pwrite(int fd, const void *buf, size_t n, off_t off)
{
    return __os_pwrite(fd, buf, n, off);
}

/* struct iovec and the seam's __os_iovec are one layout: a pointer and
 * a size_t */
_Static_assert(sizeof(struct iovec) == sizeof(struct __os_iovec),
               "struct iovec is not the seam's");

WEAK ssize_t readv(int fd, const struct iovec *iov, int n)
{
    return __os_readv(fd, (const struct __os_iovec *)(const void *)iov, n);
}

WEAK ssize_t writev(int fd, const struct iovec *iov, int n)
{
    return __os_writev(fd, (const struct __os_iovec *)(const void *)iov, n);
}

/* buffer by buffer at advancing offsets: positioned I/O has no file
 * position to race with, so the pieces need not be one call */
WEAK ssize_t preadv(int fd, const struct iovec *iov, int n, off_t off)
{
    ssize_t total = 0;
    for (int i = 0; i < n; i++) {
        long r = __os_pread(fd, iov[i].iov_base, iov[i].iov_len, off + total);
        if (r < 0)
            return total ? total : -1;
        total += r;
        if ((size_t)r < iov[i].iov_len)
            break;
    }
    return total;
}

WEAK ssize_t pwritev(int fd, const struct iovec *iov, int n, off_t off)
{
    ssize_t total = 0;
    for (int i = 0; i < n; i++) {
        long r = __os_pwrite(fd, iov[i].iov_base, iov[i].iov_len, off + total);
        if (r < 0)
            return total ? total : -1;
        total += r;
        if ((size_t)r < iov[i].iov_len)
            break;
    }
    return total;
}


WEAK int dup(int fd) { return __os_dup(fd, 0, 0); }
WEAK int dup2(int fd, int newfd) { return __os_dup2(fd, newfd, 0); }

/* dup3 refuses what dup2 allows: the same descriptor twice */
WEAK int dup3(int fd, int newfd, int flags)
{
    if (fd == newfd || (flags & ~O_CLOEXEC)) {
        errno = EINVAL;
        return -1;
    }
    return __os_dup2(fd, newfd, flags & O_CLOEXEC);
}

WEAK int pipe(int fds[2]) { return __os_pipe(fds, 0); }
WEAK int pipe2(int fds[2], int flags) { return __os_pipe(fds, flags); }

/* the third argument is an int, or a struct flock * for the locks */
WEAK int fcntl(int fd, int cmd, ...)
{
    va_list ap;
    va_start(ap, cmd);
    long arg = 0;
    if (cmd == F_GETLK || cmd == F_SETLK || cmd == F_SETLKW)
        arg = (long)va_arg(ap, struct flock *);
    else if (cmd != F_GETFD && cmd != F_GETFL)
        arg = va_arg(ap, int);
    va_end(ap);
    if (cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC)
        return __os_dup(fd, (int)arg, cmd == F_DUPFD_CLOEXEC);
    if (cmd != F_GETLK && cmd != F_SETLK && cmd != F_SETLKW)
        return __os_fcntl(fd, cmd, arg);
    /* struct flock has off_t's fields, which are 32 bits where a long
     * is; the seam's are always 64 */
    struct flock *fl = (struct flock *)arg;
    struct __os_flock k = { fl->l_type, fl->l_whence, fl->l_start,
                            fl->l_len, fl->l_pid };
    int r = __os_fcntl(fd, cmd, (long)&k);
    if (r == 0 && cmd == F_GETLK) {
        fl->l_type = k.l_type;
        fl->l_whence = k.l_whence;
        fl->l_start = (off_t)k.l_start;
        fl->l_len = (off_t)k.l_len;
        fl->l_pid = k.l_pid;
    }
    return r;
}

WEAK int fsync(int fd) { return __os_fsync(fd, 0); }
WEAK int fdatasync(int fd) { return __os_fsync(fd, 1); }
WEAK int ftruncate(int fd, off_t size) { return __os_ftruncate(fd, size); }
WEAK void sync(void) { __os_sync(); }

WEAK int ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    return __os_ioctl(fd, request, arg);
}

WEAK int flock(int fd, int op) { return __os_flock_file(fd, op); }

/* lockf: a write lock from the current position, as fcntl's */
WEAK int lockf(int fd, int cmd, off_t len)
{
    struct flock fl = { F_WRLCK, SEEK_CUR, 0, len, 0 };
    switch (cmd) {
    case F_ULOCK:
        fl.l_type = F_UNLCK;
        return fcntl(fd, F_SETLK, &fl);
    case F_LOCK:
        return fcntl(fd, F_SETLKW, &fl);
    case F_TLOCK:
        return fcntl(fd, F_SETLK, &fl);
    case F_TEST:
        fl.l_type = F_RDLCK;
        if (fcntl(fd, F_GETLK, &fl) < 0)
            return -1;
        if (fl.l_type == F_UNLCK || fl.l_pid == getpid())
            return 0;
        errno = EACCES;
        return -1;
    }
    errno = EINVAL;
    return -1;
}

/* advice, which a kernel may take or leave: taking none is a correct
 * answer, and the call checks only what POSIX says it must */
WEAK int posix_fadvise(int fd, off_t off, off_t len, int advice)
{
    (void)off;
    struct __os_fileinfo fi;
    if (advice < 0 || advice > POSIX_FADV_NOREUSE || len < 0)
        return EINVAL;
    if (__os_fstat(fd, &fi) < 0 && errno == EBADF)
        return EBADF;
    return 0;
}

WEAK int getentropy(void *buf, size_t n)
{
    if (n > 256) {
        errno = EIO;
        return -1;
    }
    return __os_getentropy(buf, n);
}

/* getrandom over getentropy's 256-byte pieces */
WEAK ssize_t getrandom(void *buf, size_t n, unsigned flags)
{
    (void)flags;
    unsigned char *p = buf;
    size_t left = n;
    while (left) {
        size_t k = left > 256 ? 256 : left;
        if (__os_getentropy(p, k) < 0)
            return n - left ? (ssize_t)(n - left) : -1;
        p += k;
        left -= k;
    }
    return (ssize_t)n;
}
