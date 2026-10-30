/* The Linux backend's half of POSIX's operating system: the primitives
 * os/backend.h lists after "POSIX's operating system", each a syscall or
 * a few.
 *
 * The seam's numbers are Linux's by design -- its flags, commands, signal
 * numbers, socket layouts and ioctl requests -- so most of this file is
 * argument passing. What is not is said where it happens: the open flags
 * aarch64 numbers differently, the structures whose kernel layout is not
 * the C library's (termios, utsname, rusage), the signal trampoline, and
 * posix_spawn, which Linux has no syscall for.
 */
#include "../backend.h"
#include "syscall.h"

#include <errno.h>
#include <string.h>

static long fail(long r)
{
    errno = (int)-r;
    return -1;
}

static long ret(long r) { return r < 0 ? fail(r) : r; }
static int ret0(long r) { return r < 0 ? (int)fail(r) : 0; }

int __linux_statx(int dirfd, const char *path, int flags,
                  struct __os_fileinfo *out);

/* ---- open's flags ---------------------------------------------------------
 *
 * The seam numbers O_DIRECTORY and O_NOFOLLOW as x86 does. aarch64's
 * table, like every later port's, moved them -- and gave 0200000, x86's
 * O_DIRECTORY, to O_DIRECT. So there they are translated both ways, and
 * every other flag passes through. */
#define X86_O_DIRECTORY 0200000
#define X86_O_NOFOLLOW  0400000
#define X86_O_DIRECT    0040000
#define A64_O_DIRECTORY 0040000
#define A64_O_NOFOLLOW  0100000
#define A64_O_DIRECT    0200000

int __linux_oflags(int f)
{
#if defined(__aarch64__)
    int out = f & ~(X86_O_DIRECTORY | X86_O_NOFOLLOW | X86_O_DIRECT);
    if (f & X86_O_DIRECTORY) out |= A64_O_DIRECTORY;
    if (f & X86_O_NOFOLLOW)  out |= A64_O_NOFOLLOW;
    if (f & X86_O_DIRECT)    out |= A64_O_DIRECT;
    return out;
#else
    return f;
#endif
}

static int seam_oflags(int f)
{
#if defined(__aarch64__)
    int out = f & ~(A64_O_DIRECTORY | A64_O_NOFOLLOW | A64_O_DIRECT);
    if (f & A64_O_DIRECTORY) out |= X86_O_DIRECTORY;
    if (f & A64_O_NOFOLLOW)  out |= X86_O_NOFOLLOW;
    if (f & A64_O_DIRECT)    out |= X86_O_DIRECT;
    return out;
#else
    return f;
#endif
}

/* ---- descriptors ---------------------------------------------------------- */

int __os_dup(int fd, int minfd, int cloexec)
{
    return (int)ret(lsys3(LSYS_fcntl, fd,
                          cloexec ? __OS_F_DUPFD_CLOEXEC : __OS_F_DUPFD,
                          minfd));
}

/* dup3 refuses fd == newfd; dup2 succeeds when fd is open, so that case
 * asks whether it is and changes nothing */
int __os_dup2(int fd, int newfd, int cloexec)
{
    if (fd == newfd) {
        long r = lsys3(LSYS_fcntl, fd, __OS_F_GETFD, 0);
        return r < 0 ? (int)fail(r) : newfd;
    }
    return (int)ret(lsys3(LSYS_dup3, fd, newfd,
                          cloexec ? __OS_O_CLOEXEC : 0));
}

int __os_pipe(int fds[2], int flags)
{
    return ret0(lsys2(LSYS_pipe2, (long)fds, flags));
}

int __os_fcntl(int fd, int cmd, long arg)
{
    if (cmd == __OS_F_SETFL)
        arg = __linux_oflags((int)arg);
    long r = lsys3(LSYS_fcntl, fd, cmd, arg);
    if (r >= 0 && cmd == __OS_F_GETFL)
        r = seam_oflags((int)r);
    return (int)ret(r);
}

/* statx of the descriptor itself: an empty path and AT_EMPTY_PATH */
int __os_fstat(int fd, struct __os_fileinfo *out)
{
    return __linux_statx(fd, "", LAT_EMPTY_PATH, out);
}

long __os_pread(int fd, void *buf, size_t n, long long off)
{
    return ret(lsys4(LSYS_pread64, fd, (long)buf, (long)n, (long)off));
}

long __os_pwrite(int fd, const void *buf, size_t n, long long off)
{
    return ret(lsys4(LSYS_pwrite64, fd, (long)buf, (long)n, (long)off));
}

/* struct __os_iovec is the kernel's struct iovec: a pointer and a size */
long __os_readv(int fd, const struct __os_iovec *iov, int n)
{
    return ret(lsys3(LSYS_readv, fd, (long)iov, n));
}

long __os_writev(int fd, const struct __os_iovec *iov, int n)
{
    return ret(lsys3(LSYS_writev, fd, (long)iov, n));
}

int __os_fsync(int fd, int data_only)
{
    return ret0(lsys1(data_only ? LSYS_fdatasync : LSYS_fsync, fd));
}

int __os_ftruncate(int fd, long long size)
{
    return ret0(lsys2(LSYS_ftruncate, fd, (long)size));
}

int __os_fchmod(int fd, unsigned mode)
{
    return ret0(lsys2(LSYS_fchmod, fd, mode));
}

int __os_fchdir(int fd) { return ret0(lsys1(LSYS_fchdir, fd)); }

int __os_ioctl(int fd, unsigned long request, void *arg)
{
    return (int)ret(lsys3(LSYS_ioctl, fd, (long)request, (long)arg));
}

int __os_flock_file(int fd, int op) { return ret0(lsys2(LSYS_flock, fd, op)); }

static void fd_link(int fd, char out[32])
{
    char num[12];
    int k = 0;
    unsigned u = (unsigned)fd;
    memcpy(out, "/proc/self/fd/", 15);
    do
        num[k++] = (char)('0' + u % 10);
    while (u /= 10);
    size_t at = 14;
    while (k)
        out[at++] = num[--k];
    out[at] = 0;
}

/* /proc/self/fd/N is a link to what N is open on. A directory removed
 * while open reads as "path (deleted)", which no path resolves -- the
 * *at call then fails with ENOENT, as it would have anyway. */
int __os_fdpath(int fd, char *buf, size_t n)
{
    char link[32];
    fd_link(fd, link);
    long r = lsys4(LSYS_readlinkat, LAT_FDCWD, (long)link, (long)buf, (long)n);
    if (r < 0)
        return (int)fail(r == -ENOENT ? -EBADF : r);
    if ((size_t)r >= n) {
        errno = ENAMETOOLONG;
        return -1;
    }
    buf[r] = 0;
    return 0;
}

/* ---- the filesystem, the rest -------------------------------------------- */

/* The *at calls are the kernel's own; the plain forms in backend.c are
 * these with AT_FDCWD. AT_* flag values are the uapi's, the same on
 * both architectures. */
int __os_openat(int dirfd, const char *path, int flags, int mode)
{
    return (int)ret(lsys4(LSYS_openat, dirfd, (long)path,
                          __linux_oflags(flags), mode));
}

int __os_fstatat(int dirfd, const char *path, struct __os_fileinfo *out,
                 int flags)
{
    return __linux_statx(dirfd, path, flags & (LAT_SYMLINK_NOFOLLOW |
                                               LAT_EMPTY_PATH), out);
}

int __os_mkdirat(int dirfd, const char *path, unsigned mode)
{
    return ret0(lsys3(LSYS_mkdirat, dirfd, (long)path, mode));
}

int __os_unlinkat(int dirfd, const char *path, int flags)
{
    return ret0(lsys3(LSYS_unlinkat, dirfd, (long)path, flags));
}

int __os_renameat(int fromfd, const char *from, int tofd, const char *to)
{
    return ret0(lsys4(LSYS_renameat, fromfd, (long)from, tofd, (long)to));
}

int __os_linkat(int fromfd, const char *from, int tofd, const char *to,
                int flags)
{
    return ret0(lsys5(LSYS_linkat, fromfd, (long)from, tofd, (long)to,
                      flags));
}

int __os_symlinkat(const char *target, int dirfd, const char *path)
{
    return ret0(lsys3(LSYS_symlinkat, (long)target, dirfd, (long)path));
}

long __os_readlinkat(int dirfd, const char *path, char *buf, size_t n)
{
    return ret(lsys4(LSYS_readlinkat, dirfd, (long)path, (long)buf,
                     (long)n));
}

/* the kernel's fchmodat has no flags; a nofollow request is refused, as
 * glibc refuses it, since a link's own mode means nothing on Linux */
int __os_fchmodat(int dirfd, const char *path, unsigned mode, int flags)
{
    if (flags & LAT_SYMLINK_NOFOLLOW) {
        errno = EOPNOTSUPP;
        return -1;
    }
    return ret0(lsys4(LSYS_fchmodat, dirfd, (long)path, mode, 0));
}

int __os_faccessat(int dirfd, const char *path, int mode, int flags)
{
    (void)flags;               /* AT_EACCESS: the effective ids, which a
                                * process that never changed them shares */
    return ret0(lsys4(LSYS_faccessat, dirfd, (long)path, mode, 0));
}

int __os_mknodat(int dirfd, const char *path, unsigned mode,
                 unsigned long long dev)
{
    return ret0(lsys4(LSYS_mknodat, dirfd, (long)path, mode, (long)dev));
}

int __os_access(const char *path, int mode)
{
    return ret0(lsys4(LSYS_faccessat, LAT_FDCWD, (long)path, mode, 0));
}

unsigned __os_umask(unsigned mask)
{
    return (unsigned)lsys1(LSYS_umask, mask & 0777);
}

int __os_chown(const char *path, int fd, unsigned uid, unsigned gid,
               int nofollow)
{
    if (!path)
        return ret0(lsys5(LSYS_fchownat, fd, (long)"", uid, gid,
                          LAT_EMPTY_PATH));
    return ret0(lsys5(LSYS_fchownat, fd, (long)path, uid, gid,
                      nofollow ? LAT_SYMLINK_NOFOLLOW : 0));
}

int __os_utimens(const char *path, int fd,
                 const struct __os_timespec times[2], int nofollow)
{
    struct ltimespec k[2], *kp = 0;
    if (times) {
        for (int i = 0; i < 2; i++) {
            k[i].tv_sec = (long)times[i].sec;
            k[i].tv_nsec = times[i].nsec;
        }
        kp = k;
    }
    /* a NULL path with a descriptor is futimens, which utimensat is */
    return ret0(lsys4(LSYS_utimensat, fd, (long)path,
                      (long)kp, nofollow ? LAT_SYMLINK_NOFOLLOW : 0));
}

int __os_mknod(const char *path, unsigned mode, unsigned long long dev)
{
    return ret0(lsys4(LSYS_mknodat, LAT_FDCWD, (long)path, mode, (long)dev));
}

int __os_chroot(const char *path) { return ret0(lsys1(LSYS_chroot, (long)path)); }

void __os_sync(void) { lsys0(LSYS_sync); }

#define LST_VALID 0x0020        /* statfs's "f_flags means something" */

int __os_statvfs(const char *path, int fd, struct __os_fsinfo *out)
{
    struct lstatfs sf;
    memset(&sf, 0, sizeof sf);
    long r = path ? lsys2(LSYS_statfs, (long)path, (long)&sf)
                  : lsys2(LSYS_fstatfs, fd, (long)&sf);
    if (r < 0)
        return (int)fail(r);
    out->bsize = (unsigned long)sf.f_bsize;
    out->frsize = (unsigned long)(sf.f_frsize ? sf.f_frsize : sf.f_bsize);
    out->blocks = sf.f_blocks;
    out->bfree = sf.f_bfree;
    out->bavail = sf.f_bavail;
    out->files = sf.f_files;
    out->ffree = sf.f_ffree;
    out->favail = sf.f_ffree;
    out->fsid = (unsigned long)sf.f_fsid;
    out->flag = (unsigned long)(sf.f_flags & ~(unsigned long long)LST_VALID);
    out->namemax = (unsigned long)sf.f_namelen;
    return 0;
}

/* ---- directories, as POSIX reads them ------------------------------------
 *
 * The same handle as backend.c's __os_opendir -- its layout is repeated
 * here, and the two must agree. */
struct ldir {
    int fd;
    int pos, len;
    char buf[4096];
};

void *__os_opendir(const char *path);
void *__os_fdopendir(int fd);

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

int __os_readdir_entry(void *dir, struct __os_dirent *e)
{
    struct ldir *d = dir;
    if (d->pos >= d->len) {
        long r = lsys3(LSYS_getdents64, d->fd, (long)d->buf,
                       (long)sizeof d->buf);
        if (r < 0)
            return (int)fail(r);
        if (r == 0)
            return 0;
        d->len = (int)r;
        d->pos = 0;
    }
    struct ldirent64 *k = (struct ldirent64 *)(d->buf + d->pos);
    d->pos += k->d_reclen;
    e->ino = k->d_ino;
    e->type = type_of_dtype(k->d_type);
    size_t n = strlen(k->d_name);
    if (n >= sizeof e->name)
        n = sizeof e->name - 1;     /* NAME_MAX is 255: never, in fact */
    memcpy(e->name, k->d_name, n);
    e->name[n] = 0;
    return 1;
}

void __os_rewinddir(void *dir)
{
    struct ldir *d = dir;
    lsys3(LSYS_lseek, d->fd, 0, 0);
    d->pos = d->len = 0;
}

int __os_dirfd(void *dir) { return ((struct ldir *)dir)->fd; }

void *__os_fdopendir(int fd)
{
    struct __os_fileinfo fi;
    if (__os_fstat(fd, &fi) < 0)
        return 0;
    if (fi.type != __OS_FT_DIRECTORY) {
        errno = ENOTDIR;
        return 0;
    }
    extern void *malloc(size_t);
    struct ldir *d = malloc(sizeof *d);
    if (!d) {
        errno = ENOMEM;
        return 0;
    }
    d->fd = fd;
    d->pos = d->len = 0;
    return d;
}

/* ---- processes ------------------------------------------------------------ */

long __os_getid(int which)
{
    static const short nr[] = { LSYS_getpid, LSYS_getppid, LSYS_getuid,
                                LSYS_geteuid, LSYS_getgid, LSYS_getegid };
    if (which < 0 || which > __OS_ID_EGID)
        return 0;
    return lsys0(nr[which]);
}

/* seteuid and setegid are setreuid(-1, id) and setregid(-1, id) */
int __os_setid(int which, unsigned id)
{
    switch (which) {
    case __OS_ID_UID:  return ret0(lsys1(LSYS_setuid, id));
    case __OS_ID_GID:  return ret0(lsys1(LSYS_setgid, id));
    case __OS_ID_EUID: return ret0(lsys2(LSYS_setreuid, -1, id));
    case __OS_ID_EGID: return ret0(lsys2(LSYS_setregid, -1, id));
    }
    errno = EINVAL;
    return -1;
}

int __os_getgroups(int n, unsigned *list)
{
    return (int)ret(lsys2(LSYS_getgroups, n, (long)list));
}

int __os_setgroups(int n, const unsigned *list)
{
    return ret0(lsys2(LSYS_setgroups, n, (long)list));
}

int __os_getpgid(int pid) { return (int)ret(lsys1(LSYS_getpgid, pid)); }
int __os_setpgid(int pid, int pgid)
{
    return ret0(lsys2(LSYS_setpgid, pid, pgid));
}
int __os_getsid(int pid) { return (int)ret(lsys1(LSYS_getsid, pid)); }
int __os_setsid(void) { return (int)ret(lsys0(LSYS_setsid)); }

/* fork is clone with only the signal the parent is told by: SIGCHLD.
 * Both architectures have clone; only x86-64 has fork itself. */
#define LSIGCHLD 17

int __os_fork(void)
{
    return (int)ret(lsys5(LSYS_clone, LSIGCHLD, 0, 0, 0, 0));
}

int __os_execve(const char *path, char *const argv[], char *const envp[])
{
    return ret0(lsys3(LSYS_execve, (long)path, (long)argv, (long)envp));
}

/* posix_spawn, as Linux has no syscall for it: fork, then in the child
 * the attributes and the file actions, then execve. What went wrong in
 * the child comes back to the parent over a pipe whose ends close on
 * exec -- an exec that succeeds closes it with nothing written, and the
 * parent reads end-of-file; one that fails writes its errno first. So
 * the parent learns of a missing program as an error from posix_spawn,
 * which POSIX allows and every caller wants, rather than as a child
 * that exits 127.
 *
 * fork rather than vfork: a vfork child shares the parent's memory,
 * and every library call it made before exec would be made on the
 * parent's stack. That is a speed difference for a large parent and
 * nothing else. */
static void child_fail(int fd, int err)
{
    lsys3(LSYS_write, fd, (long)&err, sizeof err);
    lsys1(LSYS_exit_group, 127);
    for (;;)
        ;
}

int __os_spawn_flags(void)
{
    return __OS_SPAWN_RESETIDS | __OS_SPAWN_SETPGROUP | __OS_SPAWN_SETSIGDEF |
           __OS_SPAWN_SETSIGMASK | __OS_SPAWN_SETSID;
}

/* fork carries out any action there is: nothing to refuse in advance */
int __os_spawn_check(const struct __os_spawn_action *action, int count)
{
    (void)action;
    (void)count;
    return 0;
}

int __os_spawn(int *pid, const char *path,
               const struct __os_spawn_action *a, int na,
               const struct __os_spawn_attr *attr,
               char *const argv[], char *const envp[])
{
    int p[2];
    long r = lsys2(LSYS_pipe2, (long)p, __OS_O_CLOEXEC);
    if (r < 0)
        return (int)fail(r);
    long child = lsys5(LSYS_clone, LSIGCHLD, 0, 0, 0, 0);
    if (child < 0) {
        lsys1(LSYS_close, p[0]);
        lsys1(LSYS_close, p[1]);
        return (int)fail(child);
    }
    if (child == 0) {
        lsys1(LSYS_close, p[0]);
        int f = attr ? attr->flags : 0;
        if ((f & __OS_SPAWN_SETSID) && (r = lsys0(LSYS_setsid)) < 0)
            child_fail(p[1], (int)-r);
        if ((f & __OS_SPAWN_SETPGROUP) &&
            (r = lsys2(LSYS_setpgid, 0, attr->pgroup)) < 0)
            child_fail(p[1], (int)-r);
        if (f & __OS_SPAWN_RESETIDS) {
            if ((r = lsys1(LSYS_setgid, lsys0(LSYS_getgid))) < 0 ||
                (r = lsys1(LSYS_setuid, lsys0(LSYS_getuid))) < 0)
                child_fail(p[1], (int)-r);
        }
        if (f & __OS_SPAWN_SETSIGDEF)
            for (int sig = 1; sig <= 64; sig++)
                if (attr->sigdefault >> (sig - 1) & 1) {
                    struct __os_sigaction dfl = { 0, 0, 0 };
                    __os_sigaction(sig, &dfl, 0);
                }
        if (f & __OS_SPAWN_SETSIGMASK)
            lsys4(LSYS_rt_sigprocmask, 2, (long)&attr->sigmask, 0, 8);
        for (int i = 0; i < na; i++) {
            const struct __os_spawn_action *x = &a[i];
            switch (x->kind) {
            case __OS_SPAWN_OPEN: {
                r = lsys4(LSYS_openat, LAT_FDCWD, (long)x->path,
                          __linux_oflags(x->oflag), x->mode);
                if (r >= 0 && r != x->fd) {
                    long d = lsys3(LSYS_dup3, r, x->fd, 0);
                    lsys1(LSYS_close, r);
                    r = d;
                }
                break;
            }
            case __OS_SPAWN_CLOSE:
                r = lsys1(LSYS_close, x->fd);
                break;
            case __OS_SPAWN_DUP2:
                /* POSIX: dup2 onto itself clears close-on-exec */
                if (x->fd == x->newfd) {
                    r = lsys3(LSYS_fcntl, x->fd, __OS_F_GETFD, 0);
                    if (r >= 0)
                        r = lsys3(LSYS_fcntl, x->fd, __OS_F_SETFD,
                                  r & ~__OS_FD_CLOEXEC);
                } else
                    r = lsys3(LSYS_dup3, x->fd, x->newfd, 0);
                break;
            case __OS_SPAWN_CHDIR:
                r = lsys1(LSYS_chdir, (long)x->path);
                break;
            case __OS_SPAWN_FCHDIR:
                r = lsys1(LSYS_fchdir, x->fd);
                break;
            default:
                r = -EINVAL;
            }
            if (r < 0)
                child_fail(p[1], (int)-r);
        }
        r = lsys3(LSYS_execve, (long)path, (long)argv, (long)envp);
        child_fail(p[1], (int)-r);
    }
    lsys1(LSYS_close, p[1]);
    int err = 0;
    long got;
    do
        got = lsys3(LSYS_read, p[0], (long)&err, sizeof err);
    while (got == -EINTR);
    lsys1(LSYS_close, p[0]);
    if (got == (long)sizeof err) {
        /* reaped here, since the caller never learns this pid */
        int st;
        lsys4(LSYS_wait4, child, (long)&st, 0, 0);
        errno = err;
        return -1;
    }
    *pid = (int)child;
    return 0;
}

/* EINTR goes to the caller, as POSIX's waitpid has it: a shell waiting
 * for its job must see the SIGINT that interrupted the wait */
int __os_waitpid(int pid, int *status, int options)
{
    return (int)ret(lsys4(LSYS_wait4, pid, (long)status, options, 0));
}

/* ---- signals ---------------------------------------------------------------
 *
 * The kernel's struct sigaction is the handler, the flags as a long, the
 * restorer, and the 64-bit mask -- the same order on both architectures.
 *
 * The RESTORER is where a handler returns to: code that calls
 * rt_sigreturn with the stack exactly as the kernel left it. No C
 * function can be that code (its prologue moves the stack), so it is
 * two instructions placed as bytes, the encodings beside them --
 * tests/golden/libc.sh assembles the comments and compares. x86-64
 * requires it (SA_RESTORER); aarch64 would use the vDSO's without one,
 * and is given its own anyway, so a static image needs no vDSO. */
#define LSA_RESTORER 0x04000000

struct ksigaction {
    void *handler;
    unsigned long flags;
    void (*restorer)(void);
    unsigned long long mask;
};

void __libc_restore_rt(void);

#if defined(__x86_64__)
__asm__(
".text\n"
".globl __libc_restore_rt\n"
"__libc_restore_rt:\n"
"    .byte 0xb8,0x0f,0x00,0x00,0x00  # mov  $0xf, %eax\n"
"    .byte 0x0f,0x05                 # syscall\n"
);
#elif defined(__aarch64__)
__asm__(
".text\n"
".globl __libc_restore_rt\n"
".p2align 2\n"
"__libc_restore_rt:\n"
"    .long 0xd2801168                // mov  x8, #0x8b\n"
"    .long 0xd4000001                // svc  #0x0\n"
);
#endif

int __os_sigaction(int sig, const struct __os_sigaction *act,
                   struct __os_sigaction *old)
{
    struct ksigaction k, ko;
    if (act) {
        k.handler = act->handler;
        k.flags = (unsigned long)(unsigned)act->flags | LSA_RESTORER;
        k.restorer = __libc_restore_rt;
        k.mask = act->mask;
    }
    long r = lsys4(LSYS_rt_sigaction, sig, act ? (long)&k : 0,
                   old ? (long)&ko : 0, 8);
    if (r < 0)
        return (int)fail(r);
    if (old) {
        old->handler = ko.handler;
        old->flags = (int)(ko.flags & ~(unsigned long)LSA_RESTORER);
        old->mask = ko.mask;
    }
    return 0;
}

int __os_kill(int pid, int sig) { return ret0(lsys2(LSYS_kill, pid, sig)); }

/* a thread's handle is its kernel tid (thread.c) */
int __os_thread_kill(unsigned long thread, int sig)
{
    long tid = thread ? (long)thread : lsys0(LSYS_gettid);
    return ret0(lsys3(LSYS_tgkill, lsys0(LSYS_getpid), tid, sig));
}

int __os_sigprocmask(int how, const unsigned long long *set,
                     unsigned long long *old)
{
    return ret0(lsys4(LSYS_rt_sigprocmask, how, (long)set, (long)old, 8));
}

int __os_sigpending(unsigned long long *set)
{
    return ret0(lsys2(LSYS_rt_sigpending, (long)set, 8));
}

int __os_sigsuspend(const unsigned long long *mask)
{
    return ret0(lsys2(LSYS_rt_sigsuspend, (long)mask, 8));
}

int __os_sigtimedwait(const unsigned long long *set, void *info,
                      const struct __os_timespec *timeout)
{
    struct ltimespec k, *kp = 0;
    if (timeout) {
        k.tv_sec = (long)timeout->sec;
        k.tv_nsec = timeout->nsec;
        kp = &k;
    }
    return (int)ret(lsys4(LSYS_rt_sigtimedwait, (long)set, (long)info,
                          (long)kp, 8));
}

int __os_sigaltstack(const void *ss, void *old)
{
    return ret0(lsys2(LSYS_sigaltstack, (long)ss, (long)old));
}

struct kitimerval { long isec, iusec, vsec, vusec; };

int __os_setitimer(int which, const struct __os_itimer *val,
                   struct __os_itimer *old)
{
    struct kitimerval k = { (long)val->isec, (long)val->iusec,
                            (long)val->vsec, (long)val->vusec }, ko;
    long r = lsys3(LSYS_setitimer, which, (long)&k, old ? (long)&ko : 0);
    if (r < 0)
        return (int)fail(r);
    if (old)
        *old = (struct __os_itimer){ ko.isec, ko.iusec, ko.vsec, ko.vusec };
    return 0;
}

int __os_getitimer(int which, struct __os_itimer *cur)
{
    struct kitimerval k;
    long r = lsys2(LSYS_getitimer, which, (long)&k);
    if (r < 0)
        return (int)fail(r);
    *cur = (struct __os_itimer){ k.isec, k.iusec, k.vsec, k.vusec };
    return 0;
}

/* ---- time ------------------------------------------------------------------ */

int __os_clock_gettime(int clock, struct __os_timespec *t)
{
    struct ltimespec k;
    long r = lsys2(LSYS_clock_gettime, clock, (long)&k);
    if (r < 0)
        return (int)fail(r);
    t->sec = k.tv_sec;
    t->nsec = k.tv_nsec;
    return 0;
}

int __os_clock_getres(int clock, struct __os_timespec *t)
{
    struct ltimespec k;
    long r = lsys2(LSYS_clock_getres, clock, (long)&k);
    if (r < 0)
        return (int)fail(r);
    if (t) {
        t->sec = k.tv_sec;
        t->nsec = k.tv_nsec;
    }
    return 0;
}

int __os_clock_settime(int clock, const struct __os_timespec *t)
{
    struct ltimespec k = { (long)t->sec, t->nsec };
    return ret0(lsys2(LSYS_clock_settime, clock, (long)&k));
}

#define LTIMER_ABSTIME 1

/* clock_nanosleep returns its error instead of setting errno; the seam
 * sets errno as everywhere else */
int __os_clock_nanosleep(int clock, int abs, const struct __os_timespec *req,
                         struct __os_timespec *rem)
{
    struct ltimespec k = { (long)req->sec, req->nsec }, kr = { 0, 0 };
    long r = lsys4(LSYS_clock_nanosleep, clock, abs ? LTIMER_ABSTIME : 0,
                   (long)&k, rem ? (long)&kr : 0);
    if (r < 0 && rem) {
        rem->sec = kr.tv_sec;
        rem->nsec = kr.tv_nsec;
    }
    return ret0(r);
}

long long __os_times(long long out[4])
{
    long k[4];
    long r = lsys1(LSYS_times, (long)k);
    if (r < 0 && r > -4096)
        return fail(r);
    for (int i = 0; i < 4; i++)
        out[i] = k[i];
    return r;
}

struct krusage {
    long utime_sec, utime_usec, stime_sec, stime_usec;
    long maxrss, rest[13];
};

int __os_getrusage(int who, struct __os_rusage *out)
{
    struct krusage k;
    long r = lsys2(LSYS_getrusage, who, (long)&k);
    if (r < 0)
        return (int)fail(r);
    out->utime_us = (long long)k.utime_sec * 1000000 + k.utime_usec;
    out->stime_us = (long long)k.stime_sec * 1000000 + k.stime_usec;
    out->maxrss_kb = k.maxrss;
    return 0;
}

int __os_getrlimit(int resource, unsigned long long lim[2])
{
    return ret0(lsys4(LSYS_prlimit64, 0, resource, 0, (long)lim));
}

int __os_setrlimit(int resource, const unsigned long long lim[2])
{
    return ret0(lsys4(LSYS_prlimit64, 0, resource, (long)lim, 0));
}

/* ---- memory ---------------------------------------------------------------- */

void *__os_mmap(void *addr, size_t len, int prot, int flags, int fd,
                long long off)
{
    long r = lsys6(LSYS_mmap, (long)addr, (long)len, prot, flags, fd,
                   (long)off);
    if (r < 0 && r > -4096) {
        fail(r);
        return (void *)-1;
    }
    return (void *)r;
}

int __os_munmap(void *addr, size_t len)
{
    return ret0(lsys2(LSYS_munmap, (long)addr, (long)len));
}

int __os_mprotect(void *addr, size_t len, int prot)
{
    return ret0(lsys3(LSYS_mprotect, (long)addr, (long)len, prot));
}

int __os_msync(void *addr, size_t len, int flags)
{
    return ret0(lsys3(LSYS_msync, (long)addr, (long)len, flags));
}

int __os_madvise(void *addr, size_t len, int advice)
{
    return ret0(lsys3(LSYS_madvise, (long)addr, (long)len, advice));
}

int __os_mlock(const void *addr, size_t len, int lock)
{
    return ret0(lsys2(lock ? LSYS_mlock : LSYS_munlock, (long)addr,
                      (long)len));
}

/* ---- the system ------------------------------------------------------------ */

/* the kernel's has a sixth field, the NIS domain name, POSIX's has not */
struct kutsname { char f[6][65]; };

int __os_uname(struct __os_utsname *out)
{
    struct kutsname k;
    long r = lsys1(LSYS_uname, (long)&k);
    if (r < 0)
        return (int)fail(r);
    memcpy(out->sysname, k.f[0], 65);
    memcpy(out->nodename, k.f[1], 65);
    memcpy(out->release, k.f[2], 65);
    memcpy(out->version, k.f[3], 65);
    memcpy(out->machine, k.f[4], 65);
    return 0;
}

int __os_sethostname(const char *name, size_t len)
{
    return ret0(lsys2(LSYS_sethostname, (long)name, (long)len));
}

struct ksysinfo {
    long uptime;
    unsigned long loads[3];
    unsigned long totalram, freeram, sharedram, bufferram;
    unsigned long totalswap, freeswap;
    unsigned short procs, pad;
    unsigned long totalhigh, freehigh;
    unsigned mem_unit;
    char reserved[256];
};

extern unsigned long __libc_pagesize;

static long pagesize(void)
{
    return __libc_pagesize ? (long)__libc_pagesize : 4096;
}

/* The processors this process may run on, which is what a program
 * sizing a thread pool means by "how many"; the kernel counts them in a
 * mask as wide as it was built for, so the mask here is generous. */
static long processors(void)
{
    unsigned long mask[128];
    long r = lsys3(LSYS_sched_getaffinity, 0, sizeof mask, (long)mask);
    if (r < 0)
        return fail(r);
    long n = 0;
    for (long i = 0; i < r / (long)sizeof mask[0]; i++)
        n += __builtin_popcountl(mask[i]);
    return n ? n : 1;
}

long __os_sysinfo(int what)
{
    struct ksysinfo k;
    switch (what) {
    case __OS_INFO_NPROCESSORS_CONF:
    case __OS_INFO_NPROCESSORS_ONLN:
        return processors();
    case __OS_INFO_PAGESIZE:
        return pagesize();
    case __OS_INFO_PHYS_PAGES:
    case __OS_INFO_AVPHYS_PAGES: {
        long r = lsys1(LSYS_sysinfo, (long)&k);
        if (r < 0)
            return fail(r);
        unsigned long long bytes =
            (unsigned long long)(what == __OS_INFO_PHYS_PAGES ? k.totalram
                                                             : k.freeram) *
            (k.mem_unit ? k.mem_unit : 1);
        return (long)(bytes / (unsigned long long)pagesize());
    }
    }
    errno = EINVAL;
    return -1;
}

/* ---- waiting on descriptors ---------------------------------------------- */

int __os_poll(struct __os_pollfd *fds, unsigned long n,
              const struct __os_timespec *timeout,
              const unsigned long long *sigmask)
{
    struct ltimespec k, *kp = 0;
    if (timeout) {
        k.tv_sec = (long)timeout->sec;
        k.tv_nsec = timeout->nsec;
        kp = &k;
    }
    return (int)ret(lsys5(LSYS_ppoll, (long)fds, (long)n, (long)kp,
                          (long)sigmask, 8));
}

/* ---- sockets --------------------------------------------------------------- */

int __os_socket(int domain, int type, int protocol)
{
    return (int)ret(lsys3(LSYS_socket, domain, type, protocol));
}

int __os_socketpair(int domain, int type, int protocol, int sv[2])
{
    return ret0(lsys4(LSYS_socketpair, domain, type, protocol, (long)sv));
}

int __os_bind(int fd, const void *addr, unsigned len)
{
    return ret0(lsys3(LSYS_bind, fd, (long)addr, len));
}

int __os_connect(int fd, const void *addr, unsigned len)
{
    return ret0(lsys3(LSYS_connect, fd, (long)addr, len));
}

int __os_listen(int fd, int backlog) { return ret0(lsys2(LSYS_listen, fd, backlog)); }

int __os_accept(int fd, void *addr, unsigned *len, int flags)
{
    return (int)ret(lsys4(LSYS_accept4, fd, (long)addr, (long)len, flags));
}

long __os_sendto(int fd, const void *buf, size_t n, int flags,
                 const void *addr, unsigned alen)
{
    return ret(lsys6(LSYS_sendto, fd, (long)buf, (long)n, flags, (long)addr,
                     alen));
}

long __os_recvfrom(int fd, void *buf, size_t n, int flags, void *addr,
                   unsigned *alen)
{
    return ret(lsys6(LSYS_recvfrom, fd, (long)buf, (long)n, flags,
                     (long)addr, (long)alen));
}

long __os_sendmsg(int fd, const void *msg, int flags)
{
    return ret(lsys3(LSYS_sendmsg, fd, (long)msg, flags));
}

long __os_recvmsg(int fd, void *msg, int flags)
{
    return ret(lsys3(LSYS_recvmsg, fd, (long)msg, flags));
}

int __os_shutdown(int fd, int how) { return ret0(lsys2(LSYS_shutdown, fd, how)); }

int __os_getsockopt(int fd, int level, int name, void *val, unsigned *len)
{
    return ret0(lsys5(LSYS_getsockopt, fd, level, name, (long)val, (long)len));
}

int __os_setsockopt(int fd, int level, int name, const void *val,
                    unsigned len)
{
    return ret0(lsys5(LSYS_setsockopt, fd, level, name, (long)val, len));
}

int __os_getsockname(int fd, void *addr, unsigned *len)
{
    return ret0(lsys3(LSYS_getsockname, fd, (long)addr, (long)len));
}

int __os_getpeername(int fd, void *addr, unsigned *len)
{
    return ret0(lsys3(LSYS_getpeername, fd, (long)addr, (long)len));
}

/* Linux has no resolver of its own: the C library asks DNS itself */
int __os_resolve(const char *name, int family, unsigned char *addrs, int max)
{
    (void)name; (void)family; (void)addrs; (void)max;
    errno = ENOSYS;
    return -1;
}

/* ---- terminals --------------------------------------------------------------
 *
 * TCGETS fills the kernel's struct termios, which stops after 19 control
 * characters and keeps the speed in c_cflag; glibc's -- the seam's --
 * has 32 and the speeds as fields too. The speeds here are the CBAUD
 * bits, which is what cfgetospeed reports. */
#define LTCSETS   0x5402
#define LCBAUD    0010017
#define LTIOCGPTN 0x80045430
#define LTIOCSPTLCK 0x40045431

struct ktermios {
    unsigned iflag, oflag, cflag, lflag;
    unsigned char line;
    unsigned char cc[19];
};

int __os_tcgetattr(int fd, struct __os_termios *t)
{
    struct ktermios k;
    long r = lsys3(LSYS_ioctl, fd, LTCGETS, (long)&k);
    if (r < 0)
        return (int)fail(r);
    memset(t, 0, sizeof *t);
    t->iflag = k.iflag;
    t->oflag = k.oflag;
    t->cflag = k.cflag;
    t->lflag = k.lflag;
    t->line = k.line;
    memcpy(t->cc, k.cc, sizeof k.cc);
    t->ispeed = t->ospeed = k.cflag & LCBAUD;
    return 0;
}

/* TCSETS, TCSETSW, TCSETSF: TCSANOW, TCSADRAIN, TCSAFLUSH in order */
int __os_tcsetattr(int fd, int action, const struct __os_termios *t)
{
    if (action < 0 || action > 2) {
        errno = EINVAL;
        return -1;
    }
    struct ktermios k;
    k.iflag = t->iflag;
    k.oflag = t->oflag;
    k.cflag = t->cflag;
    k.lflag = t->lflag;
    k.line = t->line;
    memcpy(k.cc, t->cc, sizeof k.cc);
    return ret0(lsys3(LSYS_ioctl, fd, LTCSETS + action, (long)&k));
}

/* a terminal's name is the path its descriptor is open on */
int __os_ttyname(int fd, char *buf, size_t n)
{
    struct ktermios t;
    long r = lsys3(LSYS_ioctl, fd, LTCGETS, (long)&t);
    if (r < 0)
        return (int)fail(r);
    if (__os_fdpath(fd, buf, n) < 0) {
        if (errno == ENAMETOOLONG)
            errno = ERANGE;
        return -1;
    }
    return 0;
}

int __os_openpt(int flags)
{
    return (int)ret(lsys4(LSYS_openat, LAT_FDCWD, (long)"/dev/ptmx",
                          __linux_oflags(flags), 0));
}

int __os_ptsname(int fd, char *buf, size_t n)
{
    unsigned num;
    long r = lsys3(LSYS_ioctl, fd, LTIOCGPTN, (long)&num);
    if (r < 0)
        return (int)fail(r);
    char tmp[32] = "/dev/pts/";
    char d[12];
    int k = 0;
    do
        d[k++] = (char)('0' + num % 10);
    while (num /= 10);
    size_t at = strlen(tmp);
    while (k)
        tmp[at++] = d[--k];
    tmp[at] = 0;
    if (at + 1 > n) {
        errno = ERANGE;
        return -1;
    }
    memcpy(buf, tmp, at + 1);
    return 0;
}

int __os_unlockpt(int fd)
{
    int zero = 0;
    return ret0(lsys3(LSYS_ioctl, fd, LTIOCSPTLCK, (long)&zero));
}
