/* <unistd.h> -- POSIX's operating system calls (src/posix/fd.c, file.c,
 * proc.c, sysconf.c), on the primitives os/backend.h names. What the
 * target's kernel cannot do fails with ENOSYS: fork on EmbLinkOS, which
 * starts new processes rather than copying them (posix_spawn works), and
 * nearly everything on bare metal. */
#ifndef _UNISTD_H
#define _UNISTD_H

#include <features.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define _POSIX_VERSION         200809L
#define _POSIX2_VERSION        200809L
#define _XOPEN_VERSION         700
#define _POSIX_FSYNC           200809L
#define _POSIX_MAPPED_FILES    200809L
#define _POSIX_MEMORY_PROTECTION 200809L
#define _POSIX_MONOTONIC_CLOCK 200809L
#define _POSIX_CLOCK_SELECTION 200809L
#define _POSIX_SAVED_IDS       1
#define _POSIX_JOB_CONTROL     1
#define _POSIX_SPAWN           200809L
#define _POSIX_REGEXP          1
#define _POSIX_SHELL           1
#define _POSIX_VDISABLE        '\0'
#define _POSIX_NO_TRUNC        1
#define _POSIX_CHOWN_RESTRICTED 1

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

/* lockf */
#define F_ULOCK 0
#define F_LOCK  1
#define F_TLOCK 2
#define F_TEST  3

/* sysconf's names, glibc's numbers */
#define _SC_ARG_MAX            0
#define _SC_CHILD_MAX          1
#define _SC_CLK_TCK            2
#define _SC_NGROUPS_MAX        3
#define _SC_OPEN_MAX           4
#define _SC_STREAM_MAX         5
#define _SC_TZNAME_MAX         6
#define _SC_JOB_CONTROL        7
#define _SC_SAVED_IDS          8
#define _SC_REALTIME_SIGNALS   9
#define _SC_TIMERS             11
#define _SC_FSYNC              15
#define _SC_MAPPED_FILES       16
#define _SC_SEMAPHORES         21
#define _SC_VERSION            29
#define _SC_PAGESIZE           30
#define _SC_PAGE_SIZE          _SC_PAGESIZE
#define _SC_RTSIG_MAX          31
#define _SC_SEM_NSEMS_MAX      32
#define _SC_SIGQUEUE_MAX       34
#define _SC_BC_BASE_MAX        36
#define _SC_BC_DIM_MAX         37
#define _SC_BC_SCALE_MAX       38
#define _SC_BC_STRING_MAX      39
#define _SC_COLL_WEIGHTS_MAX   40
#define _SC_EXPR_NEST_MAX      42
#define _SC_LINE_MAX           43
#define _SC_RE_DUP_MAX         44
#define _SC_2_VERSION          46
#define _SC_IOV_MAX            60
#define _SC_THREADS            67
#define _SC_THREAD_SAFE_FUNCTIONS 68
#define _SC_GETGR_R_SIZE_MAX   69
#define _SC_GETPW_R_SIZE_MAX   70
#define _SC_LOGIN_NAME_MAX     71
#define _SC_TTY_NAME_MAX       72
#define _SC_THREAD_KEYS_MAX    74
#define _SC_THREAD_STACK_MIN   75
#define _SC_THREAD_THREADS_MAX 76
#define _SC_NPROCESSORS_CONF   83
#define _SC_NPROCESSORS_ONLN   84
#define _SC_PHYS_PAGES         85
#define _SC_AVPHYS_PAGES       86
#define _SC_ATEXIT_MAX         87
#define _SC_XOPEN_VERSION      89
#define _SC_MONOTONIC_CLOCK    149
#define _SC_SPAWN              159
#define _SC_SYMLOOP_MAX        173
#define _SC_HOST_NAME_MAX      180

#define _PC_LINK_MAX           0
#define _PC_MAX_CANON          1
#define _PC_MAX_INPUT          2
#define _PC_NAME_MAX           3
#define _PC_PATH_MAX           4
#define _PC_PIPE_BUF           5
#define _PC_CHOWN_RESTRICTED   6
#define _PC_NO_TRUNC           7
#define _PC_VDISABLE           8
#define _PC_SYNC_IO            9
#define _PC_FILESIZEBITS       13
#define _PC_SYMLINK_MAX        19

#define _CS_PATH               0

extern char *optarg;
extern int optind, opterr, optopt;

int     access(const char *path, int mode);
int     faccessat(int dirfd, const char *path, int mode, int flags);
unsigned alarm(unsigned seconds);
int     chdir(const char *path);
int     chown(const char *path, uid_t uid, gid_t gid);
int     close(int fd);
size_t  confstr(int name, char *buf, size_t n);
int     dup(int fd);
int     dup2(int fd, int newfd);
int     execl(const char *path, const char *arg, ...);
int     execle(const char *path, const char *arg, ...);
int     execlp(const char *file, const char *arg, ...);
int     execv(const char *path, char *const argv[]);
int     execve(const char *path, char *const argv[], char *const envp[]);
int     execvp(const char *file, char *const argv[]);
__attribute__((__noreturn__)) void _exit(int status);
int     fchdir(int fd);
int     fchown(int fd, uid_t uid, gid_t gid);
int     fchownat(int dirfd, const char *path, uid_t uid, gid_t gid, int flags);
int     fdatasync(int fd);
pid_t   fork(void);
long    fpathconf(int fd, int name);
int     fsync(int fd);
int     ftruncate(int fd, off_t size);
char   *getcwd(char *buf, size_t n);
gid_t   getegid(void);
uid_t   geteuid(void);
gid_t   getgid(void);
int     getgroups(int n, gid_t list[]);
int     gethostname(char *name, size_t n);
char   *getlogin(void);
int     getlogin_r(char *name, size_t n);
int     getopt(int argc, char *const argv[], const char *opts);
pid_t   getpgid(pid_t pid);
pid_t   getpgrp(void);
pid_t   getpid(void);
pid_t   getppid(void);
pid_t   getsid(pid_t pid);
uid_t   getuid(void);
int     isatty(int fd);
int     lchown(const char *path, uid_t uid, gid_t gid);
int     link(const char *target, const char *path);
int     linkat(int fromfd, const char *from, int tofd, const char *to,
               int flags);
int     lockf(int fd, int cmd, off_t len);
off_t   lseek(int fd, off_t off, int whence);
long    pathconf(const char *path, int name);
int     pause(void);
int     pipe(int fds[2]);
ssize_t pread(int fd, void *buf, size_t n, off_t off);
ssize_t pwrite(int fd, const void *buf, size_t n, off_t off);
ssize_t read(int fd, void *buf, size_t n);
ssize_t readlink(const char *__restrict path, char *__restrict buf, size_t n);
ssize_t readlinkat(int dirfd, const char *__restrict path,
                   char *__restrict buf, size_t n);
int     rmdir(const char *path);
int     setegid(gid_t gid);
int     seteuid(uid_t uid);
int     setgid(gid_t gid);
int     setpgid(pid_t pid, pid_t pgid);
pid_t   setsid(void);
int     setuid(uid_t uid);
unsigned sleep(unsigned seconds);
int     symlink(const char *target, const char *path);
int     symlinkat(const char *target, int dirfd, const char *path);
void    sync(void);
long    sysconf(int name);
pid_t   tcgetpgrp(int fd);
int     tcsetpgrp(int fd, pid_t pgrp);
int     truncate(const char *path, off_t size);
char   *ttyname(int fd);
int     ttyname_r(int fd, char *buf, size_t n);
int     unlink(const char *path);
int     unlinkat(int dirfd, const char *path, int flags);
ssize_t write(int fd, const void *buf, size_t n);
/* POSIX.1-2024's */
int     dup3(int fd, int newfd, int flags);
int     pipe2(int fds[2], int flags);
int     getentropy(void *buf, size_t n);

#if defined(__EMB_GNU)
extern char **environ;
int     execvpe(const char *file, char *const argv[], char *const envp[]);
int     getpagesize(void);
int     getdtablesize(void);
int     sethostname(const char *name, size_t n);
int     setreuid(uid_t ruid, uid_t euid);
int     setregid(gid_t rgid, gid_t egid);
int     setpgrp(void);
int     usleep(useconds_t us);
pid_t   vfork(void);
int     chroot(const char *path);
void   *sbrk(intptr_t increment);
int     daemon(int nochdir, int noclose);
char   *getpass(const char *prompt);    /* gone from POSIX in 2001; still called */
#endif

#ifdef __cplusplus
}
#endif

#endif
