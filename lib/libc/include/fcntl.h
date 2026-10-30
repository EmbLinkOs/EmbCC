/* <fcntl.h> -- open, fcntl and their flags (src/posix/fd.c). The numbers
 * are Linux's, which os/backend.h makes the seam's. */
#ifndef _FCNTL_H
#define _FCNTL_H

#include <features.h>

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define O_ACCMODE   0003
#define O_RDONLY    00
#define O_WRONLY    01
#define O_RDWR      02
#define O_CREAT     0100
#define O_EXCL      0200
#define O_NOCTTY    0400
#define O_TRUNC     01000
#define O_APPEND    02000
#define O_NONBLOCK  04000
#define O_NDELAY    O_NONBLOCK
#define O_DSYNC     010000
#define O_ASYNC     020000
#if __INT_MAX__ > 0x7fff
#define O_DIRECTORY 0200000
#define O_NOFOLLOW  0400000
#define O_CLOEXEC   02000000
#define O_SYNC      04010000
#else
/* Where an int is 16 bits (AVR) these do not fit, and mean nothing:
 * there are no programs to exec, no directories to refuse, no links. */
#define O_DIRECTORY 0
#define O_NOFOLLOW  0
#define O_CLOEXEC   0
#define O_SYNC      O_DSYNC
#endif
#define O_RSYNC     O_SYNC
#define O_LARGEFILE 0
/* POSIX.1-2024's: open for searching or executing only. Linux's O_PATH
 * comes closest; this library has no such open, and asks read-only. */
#define O_SEARCH    O_RDONLY
#define O_EXEC      O_RDONLY

#define F_DUPFD          0
#define F_GETFD          1
#define F_SETFD          2
#define F_GETFL          3
#define F_SETFL          4
#define F_GETLK          5
#define F_SETLK          6
#define F_SETLKW         7
#define F_DUPFD_CLOEXEC  1030
#define FD_CLOEXEC       1

#define F_RDLCK 0
#define F_WRLCK 1
#define F_UNLCK 2

#define AT_FDCWD            (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR        0x200
#define AT_EACCESS          0x200
#define AT_SYMLINK_FOLLOW   0x400
#define AT_EMPTY_PATH       0x1000

#define POSIX_FADV_NORMAL     0
#define POSIX_FADV_RANDOM     1
#define POSIX_FADV_SEQUENTIAL 2
#define POSIX_FADV_WILLNEED   3
#define POSIX_FADV_DONTNEED   4
#define POSIX_FADV_NOREUSE    5

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

struct flock {
    short l_type, l_whence;
    off_t l_start, l_len;
    pid_t l_pid;
};

int open(const char *path, int flags, ...);
int openat(int dirfd, const char *path, int flags, ...);
int creat(const char *path, mode_t mode);
int fcntl(int fd, int cmd, ...);
int posix_fadvise(int fd, off_t off, off_t len, int advice);
int posix_fallocate(int fd, off_t off, off_t len);

#if defined(__EMB_GNU)
/* Linux's (src/posix/linuxapi.c); ENOSYS where it is not Linux */
#define FALLOC_FL_KEEP_SIZE      0x01
#define FALLOC_FL_PUNCH_HOLE     0x02
#define FALLOC_FL_COLLAPSE_RANGE 0x08
#define FALLOC_FL_ZERO_RANGE     0x10
#define FALLOC_FL_INSERT_RANGE   0x20
#define SPLICE_F_MOVE     1
#define SPLICE_F_NONBLOCK 2
#define SPLICE_F_MORE     4
#define SPLICE_F_GIFT     8
#define SYNC_FILE_RANGE_WAIT_BEFORE 1
#define SYNC_FILE_RANGE_WRITE       2
#define SYNC_FILE_RANGE_WAIT_AFTER  4
struct iovec;
int     fallocate(int fd, int mode, off_t off, off_t len);
ssize_t readahead(int fd, off_t off, size_t count);
int     sync_file_range(int fd, off_t off, off_t n, unsigned flags);
ssize_t splice(int in, off_t *in_off, int out, off_t *out_off, size_t len,
               unsigned flags);
ssize_t tee(int in, int out, size_t len, unsigned flags);
ssize_t vmsplice(int fd, const struct iovec *iov, size_t n, unsigned flags);
#endif

#if defined(__EMB_GNU) || defined(_LARGEFILE64_SOURCE)
/* off_t is 64 bits here: the large-file names are the same calls */
#define open64 open
#define openat64 openat
#define creat64 creat
#define posix_fadvise64 posix_fadvise
#define posix_fallocate64 posix_fallocate
#define fallocate64 fallocate
#define flock64 flock
#define F_GETLK64 F_GETLK
#define F_SETLK64 F_SETLK
#define F_SETLKW64 F_SETLKW
#endif

#ifdef __cplusplus
}
#endif

#endif
