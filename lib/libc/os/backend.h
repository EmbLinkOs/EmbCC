/* The seam between the C library and an operating system.
 *
 * Everything above this is portable C; everything below is one small file
 * per OS. A new target implements these and gets the library — the same
 * bargain `src/platform/platform.h` makes for the compiler.
 *
 * The set is deliberately the classic one, because it is the set that is
 * actually sufficient: a hosted C library needs to move bytes to and from
 * file descriptors, to grow its heap, to know the time, and to stop. It
 * does not need anything else, and a backend that offers more invites the
 * library above it to depend on a particular OS.
 *
 * The threading primitives at the end are the one OPTIONAL group. A
 * target without them is not broken -- it is single-threaded, and the
 * C++ library above says so by failing where a thread would have been
 * created rather than by pretending. Every one of them returns ENOSYS
 * when the target provides nothing, and that answer travels up to
 * std::thread's constructor as a system_error. The alternative, a
 * std::thread that runs its function on the calling thread, would be a
 * lie that deadlocks the first time somebody joins from inside it.
 */
#ifndef EMBLIBC_OS_BACKEND_H
#define EMBLIBC_OS_BACKEND_H

#include <stddef.h>

/* The C++ runtime's terminate handler writes its final message through
 * __os_write, for the same reason assert does: a diagnostic that needs a
 * working allocator disappears in the cases worth diagnosing. So this
 * contract is reached from C++ and carries C linkage explicitly. */
#ifdef __cplusplus
extern "C" {
#endif

/* Bytes in and out. Return the count moved, or -1 with errno set. A short
 * write is not an error and the caller loops — which is why stdio, not the
 * backend, owns buffering. */
long __os_write(int fd, const void *buf, size_t n);
long __os_read(int fd, void *buf, size_t n);

/* The flags __os_open takes. They are part of THIS contract, not of any
 * one OS's headers: the library has to name them to ask for "create it,
 * truncate it", and a backend whose kernel numbers them differently
 * translates. The values are the ones Linux x86 chose and nearly every
 * kernel since copied, so most backends translate by doing nothing --
 * EmbLinkOS's numbers are identical, and tests/golden/libc-emblinkos.sh
 * asserts that rather than trusting it. */
#define __OS_O_RDONLY 0x0000
#define __OS_O_WRONLY 0x0001
#define __OS_O_RDWR   0x0002
#define __OS_O_CREAT  0x0040
#define __OS_O_EXCL   0x0080
#define __OS_O_TRUNC  0x0200
#define __OS_O_APPEND 0x0400

/* Files by name. A backend with no filesystem returns -1 and sets ENOSYS;
 * everything that does not need one still works. */
int  __os_open(const char *path, int flags, int mode);
int  __os_close(int fd);
long __os_lseek(int fd, long off, int whence);

/* Files by name, removed and renamed. C11 §7.21.4 requires both, and a
 * target with no way to remove a file returns -1 with ENOSYS rather than
 * pretending to succeed -- a caller that cannot tell a removal from a
 * no-op writes a program that leaves its temporary files behind. */
int  __os_remove(const char *path);
int  __os_rename(const char *from, const char *to);

/* The heap. Returns the previous break, or (void *)-1. A backend that
 * cannot grow returns -1 and malloc reports failure honestly rather than
 * handing back memory it does not have. */
void *__os_sbrk(long increment);

/* Seconds since the epoch, or -1 where the OS has no clock. */
long __os_time(void);

/* Nanoseconds of CPU time used, or -1: what clock() reports. */
long __os_clock_ns(void);

/* Stop. Never returns. */
void __os_exit(int status);

/* Is this descriptor a terminal? stdio uses it to choose line buffering. */
int  __os_isatty(int fd);

/* Random bytes for the few library functions that must not be predictable.
 * 0 on success, -1 when the OS cannot provide them. */
int  __os_getentropy(void *buf, size_t n);

/* ---- threads, and blocking ------------------------------------------------
 *
 * Optional: a target that implements none of these is single-threaded,
 * and each returns -1 with errno ENOSYS there.
 *
 * There are two ideas here and only two. A thread is something that
 * runs a function and can be waited for. Blocking is something that
 * sleeps until a WORD IN MEMORY changes -- the futex operation, which
 * is the one primitive every mutex, condition variable, semaphore and
 * latch in the C++ library is built from. Offering a "mutex" primitive
 * instead would push the policy into the OS and make every target
 * reimplement the same fairness and recursion decisions; offering the
 * word is enough and leaves the policy here.
 */

/* Run fn(arg) on a new thread. 0 on success, with *id set to a handle
 * the target chooses; -1 with errno otherwise. The thread ends when fn
 * returns. */
int  __os_thread_create(unsigned long *id, void (*fn)(void *), void *arg);

/* Wait for it to end. 0, or -1 with errno. Joining a thread twice, or
 * joining a detached one, is the caller's mistake and is not checked
 * here -- the C++ library above checks, because it has the state to. */
int  __os_thread_join(unsigned long id);

/* Give up the claim to join it: its resources are released when it
 * ends. 0, or -1 with errno. */
int  __os_thread_detach(unsigned long id);

/* This thread's handle. 0 on a target with no threads, which is a valid
 * answer: there is exactly one thread and it needs no name. */
unsigned long __os_thread_self(void);

/* Let another runnable thread have the processor. A no-op is a correct
 * implementation. */
void __os_thread_yield(void);

/* Sleep for at least ns nanoseconds. 0, or -1 with errno. */
int  __os_sleep_ns(long ns);

/* Sleep until *addr differs from expected, or until somebody wakes this
 * address, or until timeout_ns has passed (negative: no timeout). The
 * comparison and the sleep must be ATOMIC with respect to a wake --
 * that is the entire difficulty and the entire reason this is a
 * primitive rather than a loop. A spurious return is allowed and every
 * caller here loops. 0, or -1 with errno. */
int  __os_futex_wait(const volatile int *addr, int expected, long timeout_ns);

/* Wake up to `count` waiters on addr (-1: all of them). The number
 * woken, or -1 with errno.
 *
 * A backend must TRANSLATE the -1; it is this seam's spelling, not any
 * kernel's. Linux's FUTEX_WAKE takes a count and stops when it has
 * woken that many, so handing it a negative one wakes nobody -- and
 * returns 0 rather than an error, so the caller learns nothing and its
 * waiters sleep forever. Every broadcast in the C++ library is this
 * call (condition_variable::notify_all, atomic::notify_all, latch,
 * barrier, the static-initialisation guard), so getting it wrong
 * silently hangs all of them. tests/golden/threadsafe.sh checks it. */
int  __os_futex_wake(const volatile int *addr, int count);

/* ---- the filesystem --------------------------------------------------------
 *
 * The second OPTIONAL group, on the same terms as the threads above: a
 * target that implements none of these has no filesystem beyond the
 * file descriptors at the top of this file, each returns -1 with errno
 * ENOSYS, and <filesystem> reports that as a filesystem_error carrying
 * errc::function_not_supported.
 *
 * The set is the one <filesystem> actually needs and no more. What it
 * needs, and why each is here rather than derivable from the others:
 *
 *   stat    everything about a path that is not its contents. exists,
 *           is_directory, file_size and last_write_time are all one
 *           call, so asking four questions costs one syscall rather
 *           than four.
 *   lstat   the same WITHOUT following a symlink. The distinction is
 *           the whole of symlink_status versus status, and a recursive
 *           walk that confuses them follows a loop forever.
 *   opendir/readdir/closedir
 *           directory_iterator, which cannot be built from anything
 *           else: a directory's contents are not readable as a file.
 *   getcwd/chdir
 *           current_path, and what an empty relative path resolves
 *           against.
 *   statfs  space(), which reports three numbers and not one --
 *           "free" and "available to this caller" differ wherever
 *           space is reserved for the superuser.
 *
 * The rest -- mkdir, rmdir, unlink, chmod, truncate, utime, symlink,
 * readlink, link -- are the mutations. Each is one operation because
 * each is one syscall on every OS that has them.
 */

/* What a path is. A symlink reports SYMLINK only from lstat; stat
 * follows it and reports what it points at. */
#define __OS_FT_UNKNOWN   0
#define __OS_FT_NOT_FOUND 1
#define __OS_FT_REGULAR   2
#define __OS_FT_DIRECTORY 3
#define __OS_FT_SYMLINK   4
#define __OS_FT_CHARDEV   5
#define __OS_FT_BLOCKDEV  6
#define __OS_FT_FIFO      7
#define __OS_FT_SOCKET    8

struct __os_fileinfo {
    unsigned long long size;
    long mtime;                /* seconds since the epoch */
    unsigned int mode;         /* the permission bits, POSIX-shaped */
    int type;                  /* __OS_FT_* */
    /* Identity. Two paths name the same file when BOTH match, which is
     * what equivalent() asks and what a comparison of paths cannot
     * answer -- two different paths may be one file. */
    unsigned long long dev, ino;
    unsigned long long nlink;
    /* What POSIX's struct stat adds. A backend that does not know one
     * leaves it zero -- every filler clears the structure first -- and
     * a backend with no owners reports uid and gid 0. */
    unsigned int uid, gid;
    unsigned long long rdev;
    long long atime, ctime;                 /* seconds since the epoch */
    long atime_nsec, mtime_nsec, ctime_nsec;
    long long blocks;                       /* 512-byte units */
    long blksize;                           /* preferred I/O size */
};

int  __os_stat(const char *path, struct __os_fileinfo *out);
int  __os_lstat(const char *path, struct __os_fileinfo *out);
int  __os_mkdir(const char *path, unsigned mode);
int  __os_rmdir(const char *path);
int  __os_unlink(const char *path);
int  __os_chmod(const char *path, unsigned mode);
int  __os_truncate(const char *path, long long size);
int  __os_utime(const char *path, long mtime);
int  __os_symlink(const char *target, const char *linkpath);
long __os_readlink(const char *path, char *buf, size_t n);
int  __os_link(const char *target, const char *linkpath);
int  __os_getcwd(char *buf, size_t n);
int  __os_chdir(const char *path);
int  __os_statfs(const char *path, unsigned long long *capacity,
                 unsigned long long *freespace, unsigned long long *available);

/* Reading a directory. The handle is whatever the target wants it to
 * be; NULL means failure, with errno set.
 *
 * __os_readdir returns 1 for an entry, 0 at the end, -1 on error --
 * three outcomes, because "no more entries" and "something went wrong"
 * are different and a caller that cannot tell them apart silently
 * truncates a listing. `.` and `..` are NOT returned: every caller
 * filters them, so filtering once here is one place to get it right
 * rather than one per caller. */
void *__os_opendir(const char *path);
int   __os_readdir(void *dir, char *name, size_t n, int *type);
void  __os_closedir(void *dir);

/* =========================================================================
 * POSIX's operating system
 *
 * Everything from here on is what POSIX asks of a kernel beyond C's few
 * needs -- descriptors that can be duplicated and polled, processes and
 * their ids, signals, clocks, mapped memory, sockets, terminals. All of
 * it is OPTIONAL in the sense the threads and the filesystem are: a
 * backend that cannot do one of these defines it anyway and fails with
 * ENOSYS (os/absent.inc is the list, and the bare-metal backends include
 * it whole). The POSIX functions in src/posix sit on these and nothing
 * else.
 *
 * Each is shaped like the POSIX call it serves -- the same arguments,
 * the same meaning -- because there is nothing to gain by inventing a
 * second vocabulary for "duplicate this descriptor". Where the meaning
 * needs a number (a flag, a command, a signal, an ioctl request, a
 * socket address's layout) the number is the one Linux uses, which is
 * what nearly every kernel since copied; a backend whose kernel numbers
 * something differently translates, and EmbLinkOS's has the most to
 * translate. Every call returns -1 with errno set on failure unless it
 * says otherwise.
 * ========================================================================= */

/* ---- descriptors ---------------------------------------------------------- */

/* the rest of open's flags; O_DIRECTORY and O_NOFOLLOW are x86's numbers,
 * which aarch64's kernel swaps with others and its backend translates */
#define __OS_O_NOCTTY    0x00000100
#define __OS_O_NONBLOCK  0x00000800
#define __OS_O_DSYNC     0x00001000
#if __INT_MAX__ > 0x7fff
#define __OS_O_DIRECTORY 0x00010000
#define __OS_O_NOFOLLOW  0x00020000
#define __OS_O_CLOEXEC   0x00080000
#define __OS_O_SYNC      0x00101000
#else            /* a 16-bit int (AVR): as <fcntl.h> says, 0 */
#define __OS_O_DIRECTORY 0
#define __OS_O_NOFOLLOW  0
#define __OS_O_CLOEXEC   0
#define __OS_O_SYNC      __OS_O_DSYNC
#endif

/* fcntl's commands, and its one descriptor flag */
#define __OS_F_DUPFD          0
#define __OS_F_GETFD          1
#define __OS_F_SETFD          2
#define __OS_F_GETFL          3
#define __OS_F_SETFL          4
#define __OS_F_GETLK          5
#define __OS_F_SETLK          6
#define __OS_F_SETLKW         7
#define __OS_F_DUPFD_CLOEXEC  1030
#define __OS_FD_CLOEXEC       1

/* fcntl's lock description: struct flock, as both 64-bit Linux ABIs lay
 * it out (the C library's <fcntl.h> declares this very structure) */
struct __os_flock {
    short l_type, l_whence;
    long long l_start, l_len;
    int l_pid;
};

/* dup, as F_DUPFD: the lowest free descriptor at or above minfd */
int  __os_dup(int fd, int minfd, int cloexec);
/* dup3: newfd becomes a copy of fd. fd == newfd is dup2's no-op, which
 * succeeds when fd is open -- dup3 alone refuses it. */
int  __os_dup2(int fd, int newfd, int cloexec);
/* pipe2: fds[0] reads, fds[1] writes; flags are O_NONBLOCK and O_CLOEXEC */
int  __os_pipe(int fds[2], int flags);
/* every fcntl command but the dups; arg is an int or a struct __os_flock * */
int  __os_fcntl(int fd, int cmd, long arg);
int  __os_fstat(int fd, struct __os_fileinfo *out);
long __os_pread(int fd, void *buf, size_t n, long long off);
long __os_pwrite(int fd, const void *buf, size_t n, long long off);
/* readv and writev, one call each: a pipe or socket sees one write */
struct __os_iovec { void *base; size_t len; };
long __os_readv(int fd, const struct __os_iovec *iov, int n);
long __os_writev(int fd, const struct __os_iovec *iov, int n);
int  __os_fsync(int fd, int data_only);
int  __os_ftruncate(int fd, long long size);
int  __os_fchmod(int fd, unsigned mode);
int  __os_fchdir(int fd);
/* the requests are Linux's (TCGETS, TIOCGWINSZ, FIONREAD, ...) */
int  __os_ioctl(int fd, unsigned long request, void *arg);
/* flock(2)'s whole-file advisory lock: LOCK_SH 1, LOCK_EX 2, LOCK_NB 4,
 * LOCK_UN 8 */
int  __os_flock_file(int fd, int op);
/* The absolute path the descriptor is open on, for the *at functions,
 * which resolve (dirfd, relative path) through it: openat, unlinkat and
 * the rest exist precisely so a program can name a file relative to a
 * directory it holds open. ENOSYS where the OS cannot say. */
int  __os_fdpath(int fd, char *buf, size_t n);

/* ---- the filesystem, the rest -------------------------------------------- */

/* access(2)'s question, for the real ids: F_OK 0, X_OK 1, W_OK 2, R_OK 4 */
int  __os_access(const char *path, int mode);
/* The *at forms, relative to a directory descriptor (dirfd AT_FDCWD,
 * -100, is the working directory); flags are Linux's AT_SYMLINK_NOFOLLOW
 * 0x100, AT_REMOVEDIR 0x200, AT_SYMLINK_FOLLOW 0x400, AT_EMPTY_PATH
 * 0x1000. A backend without them answers ENOSYS, and src/posix resolves
 * the directory to a path itself (__os_fdpath) and calls the plain
 * form -- which a kernel with real *at calls should not be made to do:
 * it needs /proc on Linux, and races with a rename of the directory. */
int  __os_openat(int dirfd, const char *path, int flags, int mode);
int  __os_fstatat(int dirfd, const char *path, struct __os_fileinfo *out,
                  int flags);
int  __os_mkdirat(int dirfd, const char *path, unsigned mode);
int  __os_unlinkat(int dirfd, const char *path, int flags);
int  __os_renameat(int fromfd, const char *from, int tofd, const char *to);
int  __os_linkat(int fromfd, const char *from, int tofd, const char *to,
                 int flags);
int  __os_symlinkat(const char *target, int dirfd, const char *path);
long __os_readlinkat(int dirfd, const char *path, char *buf, size_t n);
int  __os_fchmodat(int dirfd, const char *path, unsigned mode, int flags);
int  __os_faccessat(int dirfd, const char *path, int mode, int flags);
int  __os_mknodat(int dirfd, const char *path, unsigned mode,
                  unsigned long long dev);
/* the old mask; a backend with none answers 022 and changes nothing */
unsigned __os_umask(unsigned mask);
/* path NULL: the descriptor's file; else path relative to the directory
 * fd (AT_FDCWD for the working directory). nofollow: a symlink itself. */
int  __os_chown(const char *path, int fd, unsigned uid, unsigned gid,
                int nofollow);
/* times[0] access, times[1] modification; a nsec of UTIME_NOW
 * (0x3fffffff) or UTIME_OMIT (0x3ffffffe), as Linux's. path and fd as
 * for __os_chown. */
struct __os_timespec { long long sec; long nsec; };
int  __os_utimens(const char *path, int fd,
                  const struct __os_timespec times[2], int nofollow);
int  __os_mknod(const char *path, unsigned mode, unsigned long long dev);
int  __os_chroot(const char *path);
void __os_sync(void);
/* statvfs: everything a filesystem's superblock says, in its own units */
struct __os_fsinfo {
    unsigned long bsize, frsize;
    unsigned long long blocks, bfree, bavail, files, ffree, favail;
    unsigned long fsid, flag, namemax;
};
/* path NULL: the descriptor's filesystem */
int  __os_statvfs(const char *path, int fd, struct __os_fsinfo *out);

/* ---- directories, as POSIX reads them ------------------------------------
 *
 * __os_readdir above serves <filesystem>, which wants neither `.` nor
 * `..` nor an inode. POSIX's readdir wants all three, and a position to
 * come back to. These read the same handles __os_opendir makes. */
struct __os_dirent {
    unsigned long long ino;
    int type;                  /* __OS_FT_*; UNKNOWN when the OS says not */
    char name[256];
};
/* 1 with *e filled, 0 at the end, -1 on error -- `.` and `..` included
 * when the OS reports them */
int   __os_readdir_entry(void *dir, struct __os_dirent *e);
void  __os_rewinddir(void *dir);
/* the descriptor the handle reads, or -1 with ENOTSUP when it has none */
int   __os_dirfd(void *dir);
/* a handle on an open directory descriptor, which it then owns */
void *__os_fdopendir(int fd);

/* ---- processes ------------------------------------------------------------ */

#define __OS_ID_PID   0
#define __OS_ID_PPID  1
#define __OS_ID_UID   2
#define __OS_ID_EUID  3
#define __OS_ID_GID   4
#define __OS_ID_EGID  5
/* this process's id of the kind asked; no backend fails it -- one with no
 * users answers 0 for the four user and group ids */
long __os_getid(int which);
/* setuid, seteuid, setgid, setegid: which is UID, EUID, GID or EGID */
int  __os_setid(int which, unsigned id);
int  __os_getgroups(int n, unsigned *list);
int  __os_setgroups(int n, const unsigned *list);
int  __os_getpgid(int pid);
int  __os_setpgid(int pid, int pgid);
int  __os_getsid(int pid);
int  __os_setsid(void);

/* fork: the child's pid in the parent, 0 in the child. A kernel that
 * cannot copy a process -- EmbLinkOS starts new ones -- fails it with
 * ENOSYS, and a program has posix_spawn. */
int  __os_fork(void);
/* returns only on failure */
int  __os_execve(const char *path, char *const argv[], char *const envp[]);

/* posix_spawn, whole: a NEW process running path, with the file actions
 * applied in order in the child and the attributes set. The kinds and
 * the attribute flags are POSIX's, numbered as glibc numbers them. */
#define __OS_SPAWN_OPEN    1
#define __OS_SPAWN_CLOSE   2
#define __OS_SPAWN_DUP2    3
#define __OS_SPAWN_CHDIR   4
#define __OS_SPAWN_FCHDIR  5
struct __os_spawn_action {
    int kind;
    int fd, newfd;             /* OPEN: fd is the target; DUP2: fd -> newfd */
    int oflag;
    unsigned mode;
    const char *path;          /* OPEN, CHDIR */
};
#define __OS_SPAWN_RESETIDS    0x01
#define __OS_SPAWN_SETPGROUP   0x02
#define __OS_SPAWN_SETSIGDEF   0x04
#define __OS_SPAWN_SETSIGMASK  0x08
#define __OS_SPAWN_SETSID      0x80
struct __os_spawn_attr {
    int flags;
    int pgroup;
    unsigned long long sigdefault, sigmask;
};
/* The attribute flags this backend's spawn honours. posix_spawnattr_setflags
 * refuses the others with ENOTSUP at the moment they are set -- a flag
 * that would be silently ignored is a promise nobody keeps. */
int  __os_spawn_flags(void);
/* A file action as it is added, before it joins the list: 0, or the error
 * number posix_spawn_file_actions_add* is to return for an action this
 * backend's spawn could never carry out -- one too many, a path too long
 * for it, a kind it has not got. `count` is how many the list holds
 * already. The error then belongs to the call that asked for the
 * impossible, not to a posix_spawn several calls later. */
int  __os_spawn_check(const struct __os_spawn_action *action, int count);
/* 0 with *pid set, or -1 with errno -- including an error in the child
 * before it ran path (a file action that failed, a path not found),
 * which the parent learns of here rather than as an exit status */
int  __os_spawn(int *pid, const char *path,
                const struct __os_spawn_action *actions, int nactions,
                const struct __os_spawn_attr *attr,
                char *const argv[], char *const envp[]);

/* waitpid, with POSIX's status encoding as Linux makes it -- an exit is
 * (code & 0xff) << 8, a signal its number -- and its options WNOHANG 1,
 * WUNTRACED 2, WCONTINUED 8. 0 for WNOHANG with nothing ready. */
int  __os_waitpid(int pid, int *status, int options);

/* ---- signals ---------------------------------------------------------------
 *
 * Linux's numbers (SIGHUP 1 ... SIGSYS 31, and the real-time ones to 64)
 * and a 64-bit set, bit sig-1. A backend that delivers no signals fails
 * sigaction and the rest with ENOSYS; kill may still work, as EmbLinkOS's
 * does, ending the process it names. */
#define __OS_SA_NOCLDSTOP 0x00000001
#define __OS_SA_NOCLDWAIT 0x00000002
#define __OS_SA_SIGINFO   0x00000004
#define __OS_SA_ONSTACK   0x08000000
#define __OS_SA_RESTART   0x10000000
#define __OS_SA_NODEFER   0x40000000
#define __OS_SA_RESETHAND 0x80000000
struct __os_sigaction {
    void *handler;             /* 1-arg, or 3-arg with SA_SIGINFO;
                                * (void *)0 SIG_DFL, (void *)1 SIG_IGN */
    unsigned long long mask;
    int flags;
};
int  __os_kill(int pid, int sig);
int  __os_thread_kill(unsigned long thread, int sig);
int  __os_sigaction(int sig, const struct __os_sigaction *act,
                    struct __os_sigaction *old);
/* how: SIG_BLOCK 0, SIG_UNBLOCK 1, SIG_SETMASK 2; either set may be NULL */
int  __os_sigprocmask(int how, const unsigned long long *set,
                      unsigned long long *old);
int  __os_sigpending(unsigned long long *set);
/* always -1, EINTR once a signal has been handled */
int  __os_sigsuspend(const unsigned long long *mask);
/* the signal taken from set, info (a siginfo_t, 128 bytes, Linux's) filled
 * if not NULL; timeout NULL waits for ever */
int  __os_sigtimedwait(const unsigned long long *set, void *info,
                       const struct __os_timespec *timeout);
/* sigaltstack: {sp, flags, size}, Linux's stack_t */
int  __os_sigaltstack(const void *ss, void *old);
/* setitimer: which REAL 0, VIRTUAL 1, PROF 2; val {interval, value} as
 * seconds and microseconds */
struct __os_itimer { long long isec, iusec, vsec, vusec; };
int  __os_setitimer(int which, const struct __os_itimer *val,
                    struct __os_itimer *old);
int  __os_getitimer(int which, struct __os_itimer *cur);

/* ---- time ------------------------------------------------------------------
 *
 * Linux's clock ids: REALTIME 0, MONOTONIC 1, PROCESS_CPUTIME_ID 2,
 * THREAD_CPUTIME_ID 3, MONOTONIC_RAW 4, REALTIME_COARSE 5,
 * MONOTONIC_COARSE 6, BOOTTIME 7. A clock the target has not got fails
 * with EINVAL. */
int  __os_clock_gettime(int clock, struct __os_timespec *t);
int  __os_clock_getres(int clock, struct __os_timespec *t);
int  __os_clock_settime(int clock, const struct __os_timespec *t);
/* abs: req is a time on the clock, not an interval. On EINTR *rem, if
 * not NULL, is what was left. */
int  __os_clock_nanosleep(int clock, int abs, const struct __os_timespec *req,
                          struct __os_timespec *rem);
/* times(2): the four in clock ticks (sysconf's _SC_CLK_TCK), and the
 * ticks since an arbitrary point */
long long __os_times(long long out[4]);
/* getrusage's who: SELF 0, CHILDREN -1, THREAD 1. Only what POSIX names:
 * user and system time in microseconds, and the largest resident size. */
struct __os_rusage { long long utime_us, stime_us; long maxrss_kb; };
int  __os_getrusage(int who, struct __os_rusage *out);
/* getrlimit and setrlimit: Linux's resources (CPU 0 ... AS 9 ...),
 * RLIM_INFINITY ~0 */
int  __os_getrlimit(int resource, unsigned long long lim[2]);
int  __os_setrlimit(int resource, const unsigned long long lim[2]);

/* ---- memory ----------------------------------------------------------------
 *
 * mmap's: PROT_READ 1, WRITE 2, EXEC 4; MAP_SHARED 1, PRIVATE 2,
 * FIXED 0x10, ANONYMOUS 0x20. (void *)-1 on failure. */
void *__os_mmap(void *addr, size_t len, int prot, int flags, int fd,
                long long off);
int   __os_munmap(void *addr, size_t len);
int   __os_mprotect(void *addr, size_t len, int prot);
/* MS_ASYNC 1, MS_INVALIDATE 2, MS_SYNC 4 */
int   __os_msync(void *addr, size_t len, int flags);
int   __os_madvise(void *addr, size_t len, int advice);
/* lock nonzero: mlock, else munlock */
int   __os_mlock(const void *addr, size_t len, int lock);

/* ---- the system ------------------------------------------------------------ */

struct __os_utsname {
    char sysname[65], nodename[65], release[65], version[65], machine[65];
};
int  __os_uname(struct __os_utsname *out);
int  __os_sethostname(const char *name, size_t len);
/* What only the running system knows, for sysconf: NPROCESSORS_CONF 0,
 * NPROCESSORS_ONLN 1, PHYS_PAGES 2, AVPHYS_PAGES 3, PAGESIZE 4. -1 with
 * EINVAL for what it cannot say. */
#define __OS_INFO_NPROCESSORS_CONF 0
#define __OS_INFO_NPROCESSORS_ONLN 1
#define __OS_INFO_PHYS_PAGES       2
#define __OS_INFO_AVPHYS_PAGES     3
#define __OS_INFO_PAGESIZE         4
long __os_sysinfo(int what);

/* ---- waiting on descriptors ---------------------------------------------
 *
 * ppoll: struct pollfd's layout and POLLIN 1, PRI 2, OUT 4, ERR 8,
 * HUP 16, NVAL 32. timeout NULL waits for ever; sigmask NULL keeps the
 * mask. The number ready, 0 at the timeout. select is built on it. */
struct __os_pollfd { int fd; short events, revents; };
int  __os_poll(struct __os_pollfd *fds, unsigned long n,
               const struct __os_timespec *timeout,
               const unsigned long long *sigmask);

/* ---- sockets ---------------------------------------------------------------
 *
 * Addresses are struct sockaddr as Linux lays them out -- a 16-bit
 * family first, then the family's own fields in network order -- which
 * is the layout <sys/socket.h> declares; families AF_UNIX 1, AF_INET 2,
 * AF_INET6 10; types SOCK_STREAM 1, SOCK_DGRAM 2, SOCK_RAW 3, with
 * SOCK_NONBLOCK 0x800 and SOCK_CLOEXEC 0x80000 or'd in. The levels and
 * option names are Linux's too (SOL_SOCKET 1, SO_REUSEADDR 2, ...), as
 * is struct msghdr. */
int  __os_socket(int domain, int type, int protocol);
int  __os_socketpair(int domain, int type, int protocol, int sv[2]);
int  __os_bind(int fd, const void *addr, unsigned len);
int  __os_connect(int fd, const void *addr, unsigned len);
int  __os_listen(int fd, int backlog);
/* accept4: flags SOCK_NONBLOCK, SOCK_CLOEXEC */
int  __os_accept(int fd, void *addr, unsigned *len, int flags);
long __os_sendto(int fd, const void *buf, size_t n, int flags,
                 const void *addr, unsigned alen);
long __os_recvfrom(int fd, void *buf, size_t n, int flags,
                   void *addr, unsigned *alen);
long __os_sendmsg(int fd, const void *msg, int flags);
long __os_recvmsg(int fd, void *msg, int flags);
int  __os_shutdown(int fd, int how);
int  __os_getsockopt(int fd, int level, int name, void *val, unsigned *len);
int  __os_setsockopt(int fd, int level, int name, const void *val,
                     unsigned len);
int  __os_getsockname(int fd, void *addr, unsigned *len);
int  __os_getpeername(int fd, void *addr, unsigned *len);
/* A name looked up by the OS itself, where it has a resolver of its own
 * (EmbLinkOS does): up to max addresses of the family (AF_INET: 4 bytes
 * each, AF_INET6: 16), in network order, into addrs; the count, 0 for a
 * name that is not there. ENOSYS where the C library must ask DNS
 * itself, which src/posix/resolv.c then does. */
int  __os_resolve(const char *name, int family, unsigned char *addrs, int max);

/* ---- terminals ------------------------------------------------------------
 *
 * struct termios as glibc lays it out -- four 32-bit flag words, a line
 * discipline byte, 32 control characters, two speeds -- with Linux's
 * flag bits and c_cc indices. Linux's kernel has a shorter one, which
 * its backend converts. action TCSANOW 0, TCSADRAIN 1, TCSAFLUSH 2. */
struct __os_termios {
    unsigned iflag, oflag, cflag, lflag;
    unsigned char line;
    unsigned char cc[32];
    unsigned ispeed, ospeed;
};
int  __os_tcgetattr(int fd, struct __os_termios *t);
int  __os_tcsetattr(int fd, int action, const struct __os_termios *t);
/* the terminal device fd is, as a path */
int  __os_ttyname(int fd, char *buf, size_t n);
/* a pseudo-terminal's controlling side, its other side's name, and
 * permission to open that other side */
int  __os_openpt(int flags);
int  __os_ptsname(int fd, char *buf, size_t n);
int  __os_unlockpt(int fd);

#ifdef __cplusplus
}
#endif

#endif
