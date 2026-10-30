/* <fcntl.h> -- open, fcntl and their flags (src/posix/fd.c). The numbers
 * are Linux's, which os/backend.h makes the seam's. */
#ifndef _FCNTL_H
#define _FCNTL_H

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

#ifdef __cplusplus
}
#endif

#endif
