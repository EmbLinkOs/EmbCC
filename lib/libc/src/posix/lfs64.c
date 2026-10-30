/* The large-file names as symbols: off_t is 64 bits here, so each is the
 * call without the 64. The headers make them macros (under _GNU_SOURCE
 * or _LARGEFILE64_SOURCE); these are for a program that declares one
 * itself -- a configure script's link test, say. */
#define _GNU_SOURCE 1
#include <dirent.h>
#include <fcntl.h>
#include <ftw.h>
#include <glob.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#undef open64
#undef openat64
#undef creat64
#undef lseek64
#undef pread64
#undef pwrite64
#undef truncate64
#undef ftruncate64
#undef stat64
#undef fstat64
#undef lstat64
#undef fstatat64
#undef fopen64
#undef freopen64
#undef tmpfile64
#undef fseeko64
#undef ftello64
#undef fgetpos64
#undef fsetpos64
#undef readdir64
#undef mmap64
#undef getrlimit64
#undef setrlimit64
#undef statvfs64
#undef fstatvfs64
#undef mkstemp64
#undef posix_fadvise64
#undef posix_fallocate64

#define WEAK __attribute__((weak))

WEAK int open64(const char *path, int flags, ...)
{
    va_list ap;
    va_start(ap, flags);
    int mode = (flags & O_CREAT) ? va_arg(ap, int) : 0;
    va_end(ap);
    return open(path, flags, mode);
}

WEAK int openat64(int dirfd, const char *path, int flags, ...)
{
    va_list ap;
    va_start(ap, flags);
    int mode = (flags & O_CREAT) ? va_arg(ap, int) : 0;
    va_end(ap);
    return openat(dirfd, path, flags, mode);
}

WEAK int creat64(const char *path, mode_t mode) { return creat(path, mode); }
WEAK off_t lseek64(int fd, off_t off, int whence) { return lseek(fd, off, whence); }
WEAK ssize_t pread64(int fd, void *buf, size_t n, off_t off) { return pread(fd, buf, n, off); }
WEAK ssize_t pwrite64(int fd, const void *buf, size_t n, off_t off) { return pwrite(fd, buf, n, off); }
WEAK int truncate64(const char *path, off_t n) { return truncate(path, n); }
WEAK int ftruncate64(int fd, off_t n) { return ftruncate(fd, n); }
WEAK int stat64(const char *restrict path, struct stat *restrict st) { return stat(path, st); }
WEAK int fstat64(int fd, struct stat *st) { return fstat(fd, st); }
WEAK int lstat64(const char *restrict path, struct stat *restrict st) { return lstat(path, st); }
WEAK int fstatat64(int dirfd, const char *restrict path, struct stat *restrict st, int flags)
{
    return fstatat(dirfd, path, st, flags);
}
WEAK FILE *fopen64(const char *restrict path, const char *restrict mode) { return fopen(path, mode); }
WEAK FILE *freopen64(const char *restrict path, const char *restrict mode, FILE *restrict f)
{
    return freopen(path, mode, f);
}
WEAK FILE *tmpfile64(void) { return tmpfile(); }
WEAK int fseeko64(FILE *f, off_t off, int whence) { return fseeko(f, off, whence); }
WEAK off_t ftello64(FILE *f) { return ftello(f); }
WEAK int fgetpos64(FILE *restrict f, fpos_t *restrict pos) { return fgetpos(f, pos); }
WEAK int fsetpos64(FILE *f, const fpos_t *pos) { return fsetpos(f, pos); }
WEAK struct dirent *readdir64(DIR *d) { return readdir(d); }
WEAK void *mmap64(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    return mmap(addr, len, prot, flags, fd, off);
}
WEAK int getrlimit64(int r, struct rlimit *l) { return getrlimit(r, l); }
WEAK int setrlimit64(int r, const struct rlimit *l) { return setrlimit(r, l); }
WEAK int statvfs64(const char *restrict path, struct statvfs *restrict v) { return statvfs(path, v); }
WEAK int fstatvfs64(int fd, struct statvfs *v) { return fstatvfs(fd, v); }
WEAK int mkstemp64(char *tmpl) { return mkstemp(tmpl); }
WEAK int posix_fadvise64(int fd, off_t off, off_t len, int advice)
{
    return posix_fadvise(fd, off, len, advice);
}
WEAK int posix_fallocate64(int fd, off_t off, off_t len)
{
    return posix_fallocate(fd, off, len);
}
