/* <sys/stat.h> -- what a file is, and the calls that change its mode
 * (src/posix/file.c). The mode bits are POSIX's own, which every Unix
 * numbers alike. */
#ifndef _SYS_STAT_H
#define _SYS_STAT_H

#include <features.h>

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _STRUCT_TIMESPEC_DECLARED
#define _STRUCT_TIMESPEC_DECLARED
struct timespec { time_t tv_sec; long tv_nsec; };
#endif

struct stat {
    dev_t st_dev;
    ino_t st_ino;
    mode_t st_mode;
    nlink_t st_nlink;
    uid_t st_uid;
    gid_t st_gid;
    dev_t st_rdev;
    off_t st_size;
    blksize_t st_blksize;
    blkcnt_t st_blocks;
    struct timespec st_atim, st_mtim, st_ctim;
};
#define st_atime st_atim.tv_sec
#define st_mtime st_mtim.tv_sec
#define st_ctime st_ctim.tv_sec

#define S_IFMT   0170000
#define S_IFSOCK 0140000
#define S_IFLNK  0120000
#define S_IFREG  0100000
#define S_IFBLK  0060000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_IFIFO  0010000

#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)

#define S_ISUID 04000
#define S_ISGID 02000
#define S_ISVTX 01000
#define S_IRWXU 0700
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRWXG 070
#define S_IRGRP 040
#define S_IWGRP 020
#define S_IXGRP 010
#define S_IRWXO 07
#define S_IROTH 04
#define S_IWOTH 02
#define S_IXOTH 01
#define S_IREAD  S_IRUSR
#define S_IWRITE S_IWUSR
#define S_IEXEC  S_IXUSR
#define ACCESSPERMS (S_IRWXU | S_IRWXG | S_IRWXO)
#define ALLPERMS    (S_ISUID | S_ISGID | S_ISVTX | ACCESSPERMS)
#define DEFFILEMODE (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH)

#define UTIME_NOW  0x3fffffffL
#define UTIME_OMIT 0x3ffffffeL

int    stat(const char *__restrict path, struct stat *__restrict st);
int    lstat(const char *__restrict path, struct stat *__restrict st);
int    fstat(int fd, struct stat *st);
int    fstatat(int dirfd, const char *__restrict path,
               struct stat *__restrict st, int flags);
int    chmod(const char *path, mode_t mode);
int    fchmod(int fd, mode_t mode);
int    fchmodat(int dirfd, const char *path, mode_t mode, int flags);
int    mkdir(const char *path, mode_t mode);
int    mkdirat(int dirfd, const char *path, mode_t mode);
int    mkfifo(const char *path, mode_t mode);
int    mkfifoat(int dirfd, const char *path, mode_t mode);
int    mknod(const char *path, mode_t mode, dev_t dev);
int    mknodat(int dirfd, const char *path, mode_t mode, dev_t dev);
mode_t umask(mode_t mask);
int    futimens(int fd, const struct timespec times[2]);
int    utimensat(int dirfd, const char *path, const struct timespec times[2],
                 int flags);

#if defined(__EMB_GNU) || defined(_LARGEFILE64_SOURCE)
/* off_t is 64 bits here: the large-file names are the same calls */
#define stat64 stat
#define fstat64 fstat
#define lstat64 lstat
#define fstatat64 fstatat
#endif

#ifdef __cplusplus
}
#endif

#endif
