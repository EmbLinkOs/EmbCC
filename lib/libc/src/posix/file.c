/* The filesystem by name: stat and its forms, the links, the
 * directories, modes, owners and times, and their *at forms -- which
 * resolve (dirfd, path) to a path (posix.h) and call the plain one. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <utime.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>

#include "posix.h"

/* ---- stat ------------------------------------------------------------------ */

static const mode_t ifmt[] = {
    [__OS_FT_REGULAR] = S_IFREG, [__OS_FT_DIRECTORY] = S_IFDIR,
    [__OS_FT_SYMLINK] = S_IFLNK, [__OS_FT_CHARDEV] = S_IFCHR,
    [__OS_FT_BLOCKDEV] = S_IFBLK, [__OS_FT_FIFO] = S_IFIFO,
    [__OS_FT_SOCKET] = S_IFSOCK,
};

static int to_stat(int r, const struct __os_fileinfo *fi, struct stat *st)
{
    if (r < 0)
        return -1;
    memset(st, 0, sizeof *st);
    st->st_dev = fi->dev;
    st->st_ino = fi->ino;
    st->st_mode = (fi->mode & 07777) |
                  (fi->type >= 0 && fi->type < (int)(sizeof ifmt / sizeof *ifmt)
                       ? ifmt[fi->type] : 0);
    st->st_nlink = (nlink_t)fi->nlink;
    st->st_uid = fi->uid;
    st->st_gid = fi->gid;
    st->st_rdev = fi->rdev;
    st->st_size = (off_t)fi->size;
    st->st_blksize = fi->blksize ? fi->blksize : 4096;
    /* a backend that does not know the blocks: the size, rounded up */
    st->st_blocks = fi->blocks ? fi->blocks
                               : (blkcnt_t)((fi->size + 511) / 512);
    st->st_atim.tv_sec = fi->atime ? fi->atime : fi->mtime;
    st->st_atim.tv_nsec = fi->atime_nsec;
    st->st_mtim.tv_sec = fi->mtime;
    st->st_mtim.tv_nsec = fi->mtime_nsec;
    st->st_ctim.tv_sec = fi->ctime ? fi->ctime : fi->mtime;
    st->st_ctim.tv_nsec = fi->ctime_nsec;
    return 0;
}

WEAK int stat(const char *restrict path, struct stat *restrict st)
{
    struct __os_fileinfo fi;
    return to_stat(__os_stat(path, &fi), &fi, st);
}

WEAK int lstat(const char *restrict path, struct stat *restrict st)
{
    struct __os_fileinfo fi;
    return to_stat(__os_lstat(path, &fi), &fi, st);
}

WEAK int fstat(int fd, struct stat *st)
{
    struct __os_fileinfo fi;
    return to_stat(__os_fstat(fd, &fi), &fi, st);
}

WEAK int fstatat(int dirfd, const char *restrict path,
                 struct stat *restrict st, int flags)
{
    if ((flags & AT_EMPTY_PATH) && path && !*path)
        return fstat(dirfd, st);
    struct __os_fileinfo fi;
    int saved = errno;
    int r = __os_fstatat(dirfd, path, &fi, flags);
    if (!__at_absent(r, saved))
        return to_stat(r, &fi, st);
    char buf[PATH_MAX];
    const char *p = __at_path(dirfd, path, buf);
    if (!p)
        return -1;
    return flags & AT_SYMLINK_NOFOLLOW ? lstat(p, st) : stat(p, st);
}

/* ---- names ----------------------------------------------------------------- */

WEAK int access(const char *path, int mode) { return __os_access(path, mode); }

WEAK int faccessat(int dirfd, const char *path, int mode, int flags)
{
    int saved = errno;
    int r = __os_faccessat(dirfd, path, mode, flags);
    if (!__at_absent(r, saved))
        return r;
    char buf[PATH_MAX];
    const char *p = __at_path(dirfd, path, buf);
    return p ? __os_access(p, mode) : -1;
}

WEAK int unlink(const char *path) { return __os_unlink(path); }
WEAK int rmdir(const char *path) { return __os_rmdir(path); }

WEAK int unlinkat(int dirfd, const char *path, int flags)
{
    int saved = errno;
    int r = __os_unlinkat(dirfd, path, flags);
    if (!__at_absent(r, saved))
        return r;
    char buf[PATH_MAX];
    const char *p = __at_path(dirfd, path, buf);
    if (!p)
        return -1;
    return flags & AT_REMOVEDIR ? __os_rmdir(p) : __os_unlink(p);
}

WEAK int link(const char *target, const char *path)
{
    return __os_link(target, path);
}

WEAK int linkat(int fromfd, const char *from, int tofd, const char *to,
                int flags)
{
    int saved = errno;
    int r = __os_linkat(fromfd, from, tofd, to, flags);
    if (!__at_absent(r, saved))
        return r;
    char b1[PATH_MAX], b2[PATH_MAX];
    const char *f = __at_path(fromfd, from, b1), *t = __at_path(tofd, to, b2);
    return f && t ? __os_link(f, t) : -1;
}

WEAK int symlink(const char *target, const char *path)
{
    return __os_symlink(target, path);
}

/* the target is a string stored in the link, never resolved */
WEAK int symlinkat(const char *target, int dirfd, const char *path)
{
    int saved = errno;
    int r = __os_symlinkat(target, dirfd, path);
    if (!__at_absent(r, saved))
        return r;
    char buf[PATH_MAX];
    const char *p = __at_path(dirfd, path, buf);
    return p ? __os_symlink(target, p) : -1;
}

WEAK ssize_t readlink(const char *restrict path, char *restrict buf, size_t n)
{
    return __os_readlink(path, buf, n);
}

WEAK ssize_t readlinkat(int dirfd, const char *restrict path,
                        char *restrict buf, size_t n)
{
    int saved = errno;
    long r = __os_readlinkat(dirfd, path, buf, n);
    if (!__at_absent((int)(r < 0 ? -1 : 0), saved))
        return r;
    char tmp[PATH_MAX];
    const char *p = __at_path(dirfd, path, tmp);
    return p ? __os_readlink(p, buf, n) : -1;
}

WEAK int renameat(int fromfd, const char *from, int tofd, const char *to)
{
    int saved = errno;
    int r = __os_renameat(fromfd, from, tofd, to);
    if (!__at_absent(r, saved))
        return r;
    char b1[PATH_MAX], b2[PATH_MAX];
    const char *f = __at_path(fromfd, from, b1), *t = __at_path(tofd, to, b2);
    return f && t ? __os_rename(f, t) : -1;
}

WEAK int truncate(const char *path, off_t size)
{
    return __os_truncate(path, size);
}

/* ---- directories ----------------------------------------------------------- */

WEAK int mkdir(const char *path, mode_t mode) { return __os_mkdir(path, mode); }

WEAK int mkdirat(int dirfd, const char *path, mode_t mode)
{
    int saved = errno;
    int r = __os_mkdirat(dirfd, path, mode);
    if (!__at_absent(r, saved))
        return r;
    char buf[PATH_MAX];
    const char *p = __at_path(dirfd, path, buf);
    return p ? __os_mkdir(p, mode) : -1;
}

WEAK int chdir(const char *path) { return __os_chdir(path); }
WEAK int fchdir(int fd) { return __os_fchdir(fd); }
WEAK int chroot(const char *path) { return __os_chroot(path); }

/* NULL asks for a buffer of the size needed, as glibc and the BSDs
 * allocate it; POSIX leaves that case to the implementation */
WEAK char *getcwd(char *buf, size_t n)
{
    if (buf) {
        if (n == 0) {
            errno = EINVAL;
            return NULL;
        }
        return __os_getcwd(buf, n) < 0 ? NULL : buf;
    }
    char tmp[PATH_MAX];
    if (__os_getcwd(tmp, sizeof tmp) < 0)
        return NULL;
    size_t len = strlen(tmp) + 1;
    if (n && n < len) {
        errno = ERANGE;
        return NULL;
    }
    char *r = malloc(n > len ? n : len);
    return r ? memcpy(r, tmp, len) : NULL;
}

/* ---- modes and owners ------------------------------------------------------- */

WEAK int chmod(const char *path, mode_t mode) { return __os_chmod(path, mode); }
WEAK int fchmod(int fd, mode_t mode) { return __os_fchmod(fd, mode); }

WEAK int fchmodat(int dirfd, const char *path, mode_t mode, int flags)
{
    int saved = errno;
    int r = __os_fchmodat(dirfd, path, mode, flags);
    if (!__at_absent(r, saved))
        return r;
    if (flags & AT_SYMLINK_NOFOLLOW) {
        errno = EOPNOTSUPP;      /* a link's own mode means nothing here */
        return -1;
    }
    char buf[PATH_MAX];
    const char *p = __at_path(dirfd, path, buf);
    return p ? __os_chmod(p, mode) : -1;
}

WEAK mode_t umask(mode_t mask) { return __os_umask(mask); }

WEAK int chown(const char *path, uid_t uid, gid_t gid)
{
    return __os_chown(path, AT_FDCWD, uid, gid, 0);
}

WEAK int lchown(const char *path, uid_t uid, gid_t gid)
{
    return __os_chown(path, AT_FDCWD, uid, gid, 1);
}

WEAK int fchown(int fd, uid_t uid, gid_t gid)
{
    return __os_chown(NULL, fd, uid, gid, 0);
}

WEAK int fchownat(int dirfd, const char *path, uid_t uid, gid_t gid,
                  int flags)
{
    if ((flags & AT_EMPTY_PATH) && path && !*path)
        return fchown(dirfd, uid, gid);
    int saved = errno;
    int r = __os_chown(path, dirfd, uid, gid, flags & AT_SYMLINK_NOFOLLOW);
    if (!__at_absent(r, saved) || dirfd == AT_FDCWD)
        return r;
    char buf[PATH_MAX];
    const char *p = __at_path(dirfd, path, buf);
    return p ? __os_chown(p, AT_FDCWD, uid, gid, flags & AT_SYMLINK_NOFOLLOW)
             : -1;
}

WEAK int mknod(const char *path, mode_t mode, dev_t dev)
{
    return __os_mknod(path, mode, dev);
}

WEAK int mknodat(int dirfd, const char *path, mode_t mode, dev_t dev)
{
    int saved = errno;
    int r = __os_mknodat(dirfd, path, mode, dev);
    if (!__at_absent(r, saved))
        return r;
    char buf[PATH_MAX];
    const char *p = __at_path(dirfd, path, buf);
    return p ? __os_mknod(p, mode, dev) : -1;
}

WEAK int mkfifo(const char *path, mode_t mode)
{
    return __os_mknod(path, (mode & 07777) | S_IFIFO, 0);
}

WEAK int mkfifoat(int dirfd, const char *path, mode_t mode)
{
    return mknodat(dirfd, path, (mode & 07777) | S_IFIFO, 0);
}

/* ---- times --------------------------------------------------------------- */

static const struct __os_timespec *ts_in(const struct timespec *t,
                                         struct __os_timespec out[2])
{
    if (!t)
        return NULL;
    for (int i = 0; i < 2; i++) {
        out[i].sec = t[i].tv_sec;
        out[i].nsec = t[i].tv_nsec;
    }
    return out;
}

WEAK int futimens(int fd, const struct timespec times[2])
{
    struct __os_timespec k[2];
    return __os_utimens(NULL, fd, ts_in(times, k), 0);
}

WEAK int utimensat(int dirfd, const char *path, const struct timespec times[2],
                   int flags)
{
    struct __os_timespec k[2];
    int saved = errno;
    int r = __os_utimens(path, dirfd, ts_in(times, k),
                         flags & AT_SYMLINK_NOFOLLOW);
    if (!__at_absent(r, saved) || dirfd == AT_FDCWD || !path)
        return r;
    char buf[PATH_MAX];
    const char *p = __at_path(dirfd, path, buf);
    if (!p)
        return -1;
    return __os_utimens(p, AT_FDCWD, ts_in(times, k),
                        flags & AT_SYMLINK_NOFOLLOW);
}

/* A kernel with no utimens may still set a modification time
 * (__os_utime): asked for a time and nothing more, that is the answer. */
WEAK int utime(const char *path, const struct utimbuf *t)
{
    struct __os_timespec k[2];
    if (t) {
        k[0].sec = t->actime;
        k[1].sec = t->modtime;
        k[0].nsec = k[1].nsec = 0;
    }
    int r = __os_utimens(path, AT_FDCWD, t ? k : NULL, 0);
    if (r < 0 && errno == ENOSYS && t)
        return __os_utime(path, t->modtime);
    return r;
}

static int tv_utimens(const char *path, int fd, const struct timeval tv[2],
                      int nofollow)
{
    struct __os_timespec k[2];
    if (tv)
        for (int i = 0; i < 2; i++) {
            k[i].sec = tv[i].tv_sec;
            k[i].nsec = tv[i].tv_usec * 1000;
        }
    return __os_utimens(path, fd, tv ? k : NULL, nofollow);
}

WEAK int utimes(const char *path, const struct timeval times[2])
{
    return tv_utimens(path, AT_FDCWD, times, 0);
}

WEAK int lutimes(const char *path, const struct timeval times[2])
{
    return tv_utimens(path, AT_FDCWD, times, 1);
}

WEAK int futimes(int fd, const struct timeval times[2])
{
    return tv_utimens(NULL, fd, times, 0);
}

/* ---- filesystems ------------------------------------------------------------ */

static int to_statvfs(int r, const struct __os_fsinfo *k, struct statvfs *out)
{
    if (r < 0)
        return -1;
    memset(out, 0, sizeof *out);
    out->f_bsize = k->bsize;
    out->f_frsize = k->frsize;
    out->f_blocks = k->blocks;
    out->f_bfree = k->bfree;
    out->f_bavail = k->bavail;
    out->f_files = k->files;
    out->f_ffree = k->ffree;
    out->f_favail = k->favail;
    out->f_fsid = k->fsid;
    out->f_flag = k->flag;
    out->f_namemax = k->namemax;
    return 0;
}

WEAK int statvfs(const char *restrict path, struct statvfs *restrict out)
{
    struct __os_fsinfo k;
    return to_statvfs(__os_statvfs(path, -1, &k), &k, out);
}

WEAK int fstatvfs(int fd, struct statvfs *out)
{
    struct __os_fsinfo k;
    return to_statvfs(__os_statvfs(NULL, fd, &k), &k, out);
}

/* What POSIX lets a file's filesystem say. This library's answers are
 * the same for every filesystem: the limits it was built with, which
 * are what it will refuse past anyway. */
static long path_limit(int name)
{
    switch (name) {
    case _PC_LINK_MAX:         return 127;
    case _PC_MAX_CANON:        return 255;
    case _PC_MAX_INPUT:        return 255;
    case _PC_NAME_MAX:         return NAME_MAX;
    case _PC_PATH_MAX:         return PATH_MAX;
    case _PC_PIPE_BUF:         return 4096;
    case _PC_CHOWN_RESTRICTED: return 1;
    case _PC_NO_TRUNC:         return 1;
    case _PC_VDISABLE:         return 0;
    case _PC_SYNC_IO:          return 1;
    case _PC_FILESIZEBITS:     return 64;
    case _PC_SYMLINK_MAX:      return -1;   /* no limit */
    }
    errno = EINVAL;
    return -1;
}

WEAK long pathconf(const char *path, int name)
{
    struct __os_fileinfo fi;
    if (__os_stat(path, &fi) < 0)
        return -1;
    return path_limit(name);
}

WEAK long fpathconf(int fd, int name)
{
    struct __os_fileinfo fi;
    if (__os_fstat(fd, &fi) < 0 && errno == EBADF)
        return -1;
    return path_limit(name);
}
