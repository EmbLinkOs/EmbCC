/* Every primitive of os/backend.h's "POSIX's operating system" half,
 * defined as a kernel that has not got it: -1 with ENOSYS (or the
 * contract's answer where it promises one -- umask's 022, getid's pid 1
 * and ids 0). A backend includes this after its own definitions,
 * having #defined OS_HAS_<name> for each it implements; the bare-metal
 * ones include it whole.
 *
 * Generated from os/backend.h; tests/golden/libc-backends.sh checks
 * that every backend defines every primitive that header names, which is
 * what keeps this list and that one the same. */
#include <errno.h>

#define ABSENT_RETURN(v) do { errno = ENOSYS; return v; } while (0)

#ifndef OS_HAS_dup
int __os_dup(int fd, int minfd, int cloexec)
{
    (void)fd; (void)minfd; (void)cloexec; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_dup2
int __os_dup2(int fd, int newfd, int cloexec)
{
    (void)fd; (void)newfd; (void)cloexec; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_pipe
int __os_pipe(int fds[2], int flags)
{
    (void)fds; (void)flags; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_fcntl
int __os_fcntl(int fd, int cmd, long arg)
{
    (void)fd; (void)cmd; (void)arg; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_fstat
int __os_fstat(int fd, struct __os_fileinfo *out)
{
    (void)fd; (void)out; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_pread
long __os_pread(int fd, void *buf, size_t n, long long off)
{
    (void)fd; (void)buf; (void)n; (void)off; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_pwrite
long __os_pwrite(int fd, const void *buf, size_t n, long long off)
{
    (void)fd; (void)buf; (void)n; (void)off; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_readv
long __os_readv(int fd, const struct __os_iovec *iov, int n)
{
    (void)fd; (void)iov; (void)n; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_writev
long __os_writev(int fd, const struct __os_iovec *iov, int n)
{
    (void)fd; (void)iov; (void)n; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_fsync
int __os_fsync(int fd, int data_only)
{
    (void)fd; (void)data_only; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_ftruncate
int __os_ftruncate(int fd, long long size)
{
    (void)fd; (void)size; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_fchmod
int __os_fchmod(int fd, unsigned mode)
{
    (void)fd; (void)mode; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_fchdir
int __os_fchdir(int fd)
{
    (void)fd; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_ioctl
int __os_ioctl(int fd, unsigned long request, void *arg)
{
    (void)fd; (void)request; (void)arg; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_flock_file
int __os_flock_file(int fd, int op)
{
    (void)fd; (void)op; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_fdpath
int __os_fdpath(int fd, char *buf, size_t n)
{
    (void)fd; (void)buf; (void)n; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_openat
int __os_openat(int dirfd, const char *path, int flags, int mode)
{
    (void)dirfd; (void)path; (void)flags; (void)mode; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_fstatat
int __os_fstatat(int dirfd, const char *path, struct __os_fileinfo *out,
                 int flags)
{
    (void)dirfd; (void)path; (void)out; (void)flags; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_mkdirat
int __os_mkdirat(int dirfd, const char *path, unsigned mode)
{
    (void)dirfd; (void)path; (void)mode; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_unlinkat
int __os_unlinkat(int dirfd, const char *path, int flags)
{
    (void)dirfd; (void)path; (void)flags; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_renameat
int __os_renameat(int fromfd, const char *from, int tofd, const char *to)
{
    (void)fromfd; (void)from; (void)tofd; (void)to; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_linkat
int __os_linkat(int fromfd, const char *from, int tofd, const char *to,
                int flags)
{
    (void)fromfd; (void)from; (void)tofd; (void)to; (void)flags;
    ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_symlinkat
int __os_symlinkat(const char *target, int dirfd, const char *path)
{
    (void)target; (void)dirfd; (void)path; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_readlinkat
long __os_readlinkat(int dirfd, const char *path, char *buf, size_t n)
{
    (void)dirfd; (void)path; (void)buf; (void)n; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_fchmodat
int __os_fchmodat(int dirfd, const char *path, unsigned mode, int flags)
{
    (void)dirfd; (void)path; (void)mode; (void)flags; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_faccessat
int __os_faccessat(int dirfd, const char *path, int mode, int flags)
{
    (void)dirfd; (void)path; (void)mode; (void)flags; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_mknodat
int __os_mknodat(int dirfd, const char *path, unsigned mode,
                 unsigned long long dev)
{
    (void)dirfd; (void)path; (void)mode; (void)dev; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_access
int __os_access(const char *path, int mode)
{
    (void)path; (void)mode; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_umask
unsigned __os_umask(unsigned mask)
{
    (void)mask; return 022;
}
#endif

#ifndef OS_HAS_chown
int __os_chown(const char *path, int fd, unsigned uid, unsigned gid, int nofollow)
{
    (void)path; (void)fd; (void)uid; (void)gid; (void)nofollow; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_utimens
int __os_utimens(const char *path, int fd, const struct __os_timespec times[2], int nofollow)
{
    (void)path; (void)fd; (void)times; (void)nofollow; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_mknod
int __os_mknod(const char *path, unsigned mode, unsigned long long dev)
{
    (void)path; (void)mode; (void)dev; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_chroot
int __os_chroot(const char *path)
{
    (void)path; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_sync
void __os_sync(void)
{
    
}
#endif

#ifndef OS_HAS_statvfs
int __os_statvfs(const char *path, int fd, struct __os_fsinfo *out)
{
    (void)path; (void)fd; (void)out; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_readdir_entry
int __os_readdir_entry(void *dir, struct __os_dirent *e)
{
    (void)dir; (void)e; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_rewinddir
void __os_rewinddir(void *dir)
{
    (void)dir;
}
#endif

#ifndef OS_HAS_dirfd
int __os_dirfd(void *dir)
{
    (void)dir; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_fdopendir
void *__os_fdopendir(int fd)
{
    (void)fd; ABSENT_RETURN((void *)0);
}
#endif

#ifndef OS_HAS_getid
long __os_getid(int which)
{
    return which == __OS_ID_PID ? 1 : 0;
}
#endif

#ifndef OS_HAS_setid
int __os_setid(int which, unsigned id)
{
    (void)which; (void)id; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_getgroups
int __os_getgroups(int n, unsigned *list)
{
    (void)n; (void)list; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_setgroups
int __os_setgroups(int n, const unsigned *list)
{
    (void)n; (void)list; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_getpgid
int __os_getpgid(int pid)
{
    (void)pid; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_setpgid
int __os_setpgid(int pid, int pgid)
{
    (void)pid; (void)pgid; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_getsid
int __os_getsid(int pid)
{
    (void)pid; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_setsid
int __os_setsid(void)
{
    ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_fork
int __os_fork(void)
{
    ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_execve
int __os_execve(const char *path, char *const argv[], char *const envp[])
{
    (void)path; (void)argv; (void)envp; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_spawn_flags
int __os_spawn_flags(void)
{
    return 0;
}
#endif

#ifndef OS_HAS_spawn
int __os_spawn(int *pid, const char *path, const struct __os_spawn_action *actions, int nactions, const struct __os_spawn_attr *attr, char *const argv[], char *const envp[])
{
    (void)pid; (void)path; (void)actions; (void)nactions; (void)attr; (void)argv; (void)envp; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_waitpid
int __os_waitpid(int pid, int *status, int options)
{
    (void)pid; (void)status; (void)options; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_kill
int __os_kill(int pid, int sig)
{
    (void)pid; (void)sig; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_thread_kill
int __os_thread_kill(unsigned long thread, int sig)
{
    (void)thread; (void)sig; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_sigaction
int __os_sigaction(int sig, const struct __os_sigaction *act, struct __os_sigaction *old)
{
    (void)sig; (void)act; (void)old; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_sigprocmask
int __os_sigprocmask(int how, const unsigned long long *set, unsigned long long *old)
{
    (void)how; (void)set; (void)old; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_sigpending
int __os_sigpending(unsigned long long *set)
{
    (void)set; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_sigsuspend
int __os_sigsuspend(const unsigned long long *mask)
{
    (void)mask; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_sigtimedwait
int __os_sigtimedwait(const unsigned long long *set, void *info, const struct __os_timespec *timeout)
{
    (void)set; (void)info; (void)timeout; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_sigaltstack
int __os_sigaltstack(const void *ss, void *old)
{
    (void)ss; (void)old; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_setitimer
int __os_setitimer(int which, const struct __os_itimer *val, struct __os_itimer *old)
{
    (void)which; (void)val; (void)old; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_getitimer
int __os_getitimer(int which, struct __os_itimer *cur)
{
    (void)which; (void)cur; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_clock_gettime
int __os_clock_gettime(int clock, struct __os_timespec *t)
{
    (void)clock; (void)t; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_clock_getres
int __os_clock_getres(int clock, struct __os_timespec *t)
{
    (void)clock; (void)t; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_clock_settime
int __os_clock_settime(int clock, const struct __os_timespec *t)
{
    (void)clock; (void)t; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_clock_nanosleep
int __os_clock_nanosleep(int clock, int abs, const struct __os_timespec *req, struct __os_timespec *rem)
{
    (void)clock; (void)abs; (void)req; (void)rem; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_times
long long __os_times(long long out[4])
{
    (void)out; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_getrusage
int __os_getrusage(int who, struct __os_rusage *out)
{
    (void)who; (void)out; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_getrlimit
int __os_getrlimit(int resource, unsigned long long lim[2])
{
    (void)resource; (void)lim; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_setrlimit
int __os_setrlimit(int resource, const unsigned long long lim[2])
{
    (void)resource; (void)lim; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_mmap
void *__os_mmap(void *addr, size_t len, int prot, int flags, int fd, long long off)
{
    (void)addr; (void)len; (void)prot; (void)flags; (void)fd; (void)off; ABSENT_RETURN((void *)-1);
}
#endif

#ifndef OS_HAS_munmap
int __os_munmap(void *addr, size_t len)
{
    (void)addr; (void)len; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_mprotect
int __os_mprotect(void *addr, size_t len, int prot)
{
    (void)addr; (void)len; (void)prot; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_msync
int __os_msync(void *addr, size_t len, int flags)
{
    (void)addr; (void)len; (void)flags; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_madvise
int __os_madvise(void *addr, size_t len, int advice)
{
    (void)addr; (void)len; (void)advice; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_mlock
int __os_mlock(const void *addr, size_t len, int lock)
{
    (void)addr; (void)len; (void)lock; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_uname
int __os_uname(struct __os_utsname *out)
{
    (void)out; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_sethostname
int __os_sethostname(const char *name, size_t len)
{
    (void)name; (void)len; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_sysinfo
long __os_sysinfo(int what)
{
    (void)what; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_poll
int __os_poll(struct __os_pollfd *fds, unsigned long n, const struct __os_timespec *timeout, const unsigned long long *sigmask)
{
    (void)fds; (void)n; (void)timeout; (void)sigmask; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_socket
int __os_socket(int domain, int type, int protocol)
{
    (void)domain; (void)type; (void)protocol; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_socketpair
int __os_socketpair(int domain, int type, int protocol, int sv[2])
{
    (void)domain; (void)type; (void)protocol; (void)sv; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_bind
int __os_bind(int fd, const void *addr, unsigned len)
{
    (void)fd; (void)addr; (void)len; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_connect
int __os_connect(int fd, const void *addr, unsigned len)
{
    (void)fd; (void)addr; (void)len; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_listen
int __os_listen(int fd, int backlog)
{
    (void)fd; (void)backlog; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_accept
int __os_accept(int fd, void *addr, unsigned *len, int flags)
{
    (void)fd; (void)addr; (void)len; (void)flags; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_sendto
long __os_sendto(int fd, const void *buf, size_t n, int flags, const void *addr, unsigned alen)
{
    (void)fd; (void)buf; (void)n; (void)flags; (void)addr; (void)alen; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_recvfrom
long __os_recvfrom(int fd, void *buf, size_t n, int flags, void *addr, unsigned *alen)
{
    (void)fd; (void)buf; (void)n; (void)flags; (void)addr; (void)alen; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_sendmsg
long __os_sendmsg(int fd, const void *msg, int flags)
{
    (void)fd; (void)msg; (void)flags; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_recvmsg
long __os_recvmsg(int fd, void *msg, int flags)
{
    (void)fd; (void)msg; (void)flags; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_shutdown
int __os_shutdown(int fd, int how)
{
    (void)fd; (void)how; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_getsockopt
int __os_getsockopt(int fd, int level, int name, void *val, unsigned *len)
{
    (void)fd; (void)level; (void)name; (void)val; (void)len; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_setsockopt
int __os_setsockopt(int fd, int level, int name, const void *val, unsigned len)
{
    (void)fd; (void)level; (void)name; (void)val; (void)len; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_getsockname
int __os_getsockname(int fd, void *addr, unsigned *len)
{
    (void)fd; (void)addr; (void)len; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_getpeername
int __os_getpeername(int fd, void *addr, unsigned *len)
{
    (void)fd; (void)addr; (void)len; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_resolve
int __os_resolve(const char *name, int family, unsigned char *addrs, int max)
{
    (void)name; (void)family; (void)addrs; (void)max; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_tcgetattr
int __os_tcgetattr(int fd, struct __os_termios *t)
{
    (void)fd; (void)t; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_tcsetattr
int __os_tcsetattr(int fd, int action, const struct __os_termios *t)
{
    (void)fd; (void)action; (void)t; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_ttyname
int __os_ttyname(int fd, char *buf, size_t n)
{
    (void)fd; (void)buf; (void)n; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_openpt
int __os_openpt(int flags)
{
    (void)flags; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_ptsname
int __os_ptsname(int fd, char *buf, size_t n)
{
    (void)fd; (void)buf; (void)n; ABSENT_RETURN(-1);
}
#endif

#ifndef OS_HAS_unlockpt
int __os_unlockpt(int fd)
{
    (void)fd; ABSENT_RETURN(-1);
}
#endif
