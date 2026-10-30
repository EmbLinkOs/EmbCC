/* Linux's own interfaces: event polling, counter, timer and signal
 * descriptors, inotify, the in-kernel copies, statfs and sysinfo,
 * extended attributes, mounting, processor affinity and the rest.
 *
 * Each is a system call where the kernel is Linux -- by its number from
 * <sys/syscall.h>, through syscall(), so this file needs no backend of
 * its own. Elsewhere a call fails with ENOSYS unless it has a meaning
 * this library can give truthfully without that kernel: sendfile and
 * copy_file_range are read-and-write loops, statfs comes from statvfs,
 * sysinfo knows the uptime, close_range closes, the affinity calls
 * describe one processor, and locking memory where nothing pages is
 * already true.
 */
#define _GNU_SOURCE 1
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/auxv.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/klog.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/personality.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/sendfile.h>
#include <sys/signalfd.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/swap.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/timerfd.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <stdio.h>
#include <sys/utsname.h>
#include <sys/xattr.h>

#include "posix.h"

#if defined(__linux__)
#define LINUX 1
#define S0(n)                 syscall(n)
#define S1(n, a)              syscall(n, (long)(a))
#define S2(n, a, b)           syscall(n, (long)(a), (long)(b))
#define S3(n, a, b, c)        syscall(n, (long)(a), (long)(b), (long)(c))
#define S4(n, a, b, c, d)     syscall(n, (long)(a), (long)(b), (long)(c), (long)(d))
#define S5(n, a, b, c, d, e)  syscall(n, (long)(a), (long)(b), (long)(c), \
                                      (long)(d), (long)(e))
#define S6(n, a, b, c, d, e, f) syscall(n, (long)(a), (long)(b), (long)(c), \
                                        (long)(d), (long)(e), (long)(f))
#else
#define LINUX 0
static long enosys(void)
{
    errno = ENOSYS;
    return -1;
}
#define S0(n)                 enosys()
#define S1(n, a)              ((void)(a), enosys())
#define S2(n, a, b)           ((void)(a), (void)(b), enosys())
#define S3(n, a, b, c)        ((void)(a), (void)(b), (void)(c), enosys())
#define S4(n, a, b, c, d)     ((void)(a), (void)(b), (void)(c), (void)(d), enosys())
#define S5(n, a, b, c, d, e)  ((void)(a), (void)(b), (void)(c), (void)(d), \
                               (void)(e), enosys())
#define S6(n, a, b, c, d, e, f) ((void)(a), (void)(b), (void)(c), (void)(d), \
                                 (void)(e), (void)(f), enosys())
#endif

/* ---- event polling ------------------------------------------------------------- */

WEAK int epoll_create(int size)
{
    if (size <= 0) {
        errno = EINVAL;
        return -1;
    }
    return (int)S1(SYS_epoll_create1, 0);
}

WEAK int epoll_create1(int flags) { return (int)S1(SYS_epoll_create1, flags); }

WEAK int epoll_ctl(int epfd, int op, int fd, struct epoll_event *ev)
{
    return (int)S4(SYS_epoll_ctl, epfd, op, fd, ev);
}

/* aarch64 has only the pwait form; a null mask makes it epoll_wait */
WEAK int epoll_pwait(int epfd, struct epoll_event *evs, int max, int timeout,
                     const sigset_t *mask)
{
    return (int)S6(SYS_epoll_pwait, epfd, evs, max, timeout, mask,
                   sizeof(sigset_t));
}

WEAK int epoll_wait(int epfd, struct epoll_event *evs, int max, int timeout)
{
    return epoll_pwait(epfd, evs, max, timeout, 0);
}

WEAK int epoll_pwait2(int epfd, struct epoll_event *evs, int max,
                      const struct timespec *timeout, const sigset_t *mask)
{
#if LINUX && defined(SYS_epoll_pwait2)
    return (int)S6(SYS_epoll_pwait2, epfd, evs, max, timeout, mask,
                   sizeof(sigset_t));
#else
    (void)epfd; (void)evs; (void)max; (void)timeout; (void)mask;
    errno = ENOSYS;
    return -1;
#endif
}

/* ---- counter, timer and signal descriptors -------------------------------------- */

WEAK int eventfd(unsigned initval, int flags)
{
    return (int)S2(SYS_eventfd2, initval, flags);
}

WEAK int eventfd_read(int fd, eventfd_t *value)
{
    return read(fd, value, sizeof *value) == (ssize_t)sizeof *value ? 0 : -1;
}

WEAK int eventfd_write(int fd, eventfd_t value)
{
    return write(fd, &value, sizeof value) == (ssize_t)sizeof value ? 0 : -1;
}

WEAK int timerfd_create(int clock, int flags)
{
    return (int)S2(SYS_timerfd_create, clock, flags);
}

WEAK int timerfd_settime(int fd, int flags, const struct itimerspec *value,
                         struct itimerspec *old)
{
    return (int)S4(SYS_timerfd_settime, fd, flags, value, old);
}

WEAK int timerfd_gettime(int fd, struct itimerspec *value)
{
    return (int)S2(SYS_timerfd_gettime, fd, value);
}

WEAK int signalfd(int fd, const sigset_t *mask, int flags)
{
    return (int)S4(SYS_signalfd4, fd, mask, sizeof(sigset_t), flags);
}

/* ---- inotify -------------------------------------------------------------------- */

WEAK int inotify_init(void) { return (int)S1(SYS_inotify_init1, 0); }
WEAK int inotify_init1(int flags) { return (int)S1(SYS_inotify_init1, flags); }

WEAK int inotify_add_watch(int fd, const char *path, uint32_t mask)
{
    return (int)S3(SYS_inotify_add_watch, fd, path, mask);
}

WEAK int inotify_rm_watch(int fd, int wd)
{
    return (int)S2(SYS_inotify_rm_watch, fd, wd);
}

/* ---- copying between descriptors ------------------------------------------------- */

/* len bytes from in (at *off, which moves, or at its position) to out,
 * through a buffer: what the copies are where the kernel cannot do them */
static ssize_t copy_loop(int in, off_t *off, int out, off_t *out_off,
                         size_t len)
{
    char buf[4096];
    size_t done = 0;
    while (done < len) {
        size_t want = len - done < sizeof buf ? len - done : sizeof buf;
        ssize_t n = off ? pread(in, buf, want, *off) : read(in, buf, want);
        if (n < 0)
            return done ? (ssize_t)done : -1;
        if (n == 0)
            break;
        ssize_t w = 0;
        while (w < n) {
            ssize_t k = out_off ? pwrite(out, buf + w, (size_t)(n - w), *out_off + w)
                                : write(out, buf + w, (size_t)(n - w));
            if (k <= 0) {
                if (off)
                    *off += w;
                if (out_off)
                    *out_off += w;
                done += (size_t)w;
                return done ? (ssize_t)done : -1;
            }
            w += k;
        }
        if (off)
            *off += n;
        if (out_off)
            *out_off += n;
        done += (size_t)n;
        if ((size_t)n < want)
            break;
    }
    return (ssize_t)done;
}

WEAK ssize_t sendfile(int out, int in, off_t *offset, size_t count)
{
#if LINUX
    return S4(SYS_sendfile, out, in, offset, count);
#else
    return copy_loop(in, offset, out, 0, count);
#endif
}

WEAK ssize_t copy_file_range(int in, off_t *in_off, int out, off_t *out_off,
                             size_t len, unsigned flags)
{
    if (flags) {
        errno = EINVAL;
        return -1;
    }
#if LINUX
    ssize_t r = S6(SYS_copy_file_range, in, in_off, out, out_off, len, flags);
    if (r >= 0 || (errno != ENOSYS && errno != EXDEV))
        return r;
#endif
    return copy_loop(in, in_off, out, out_off, len);
}

WEAK ssize_t splice(int in, off_t *in_off, int out, off_t *out_off,
                    size_t len, unsigned flags)
{
    return S6(SYS_splice, in, in_off, out, out_off, len, flags);
}

WEAK ssize_t tee(int in, int out, size_t len, unsigned flags)
{
    return S4(SYS_tee, in, out, len, flags);
}

WEAK ssize_t vmsplice(int fd, const struct iovec *iov, size_t n, unsigned flags)
{
    return S4(SYS_vmsplice, fd, iov, n, flags);
}

/* ---- files ------------------------------------------------------------------------- */

WEAK int fallocate(int fd, int mode, off_t off, off_t len)
{
    return (int)S4(SYS_fallocate, fd, mode, off, len);
}

WEAK ssize_t readahead(int fd, off_t off, size_t count)
{
    return S3(SYS_readahead, fd, off, count);
}

WEAK int sync_file_range(int fd, off_t off, off_t n, unsigned flags)
{
    return (int)S4(SYS_sync_file_range, fd, off, n, flags);
}

WEAK int syncfs(int fd)
{
#if LINUX
    return (int)S1(SYS_syncfs, fd);
#else
    (void)fd;
    sync();
    return 0;
#endif
}

/* everything from first to last closed (or marked close-on-exec): the
 * kernel's where it has close_range, a loop where it has not */
WEAK int close_range(unsigned first, unsigned last, int flags)
{
    if (first > last || (flags & ~(CLOSE_RANGE_UNSHARE | CLOSE_RANGE_CLOEXEC))) {
        errno = EINVAL;
        return -1;
    }
#if LINUX && defined(SYS_close_range)
    long r = S3(SYS_close_range, first, last, flags);
    if (r == 0 || errno != ENOSYS)
        return (int)r;
#endif
    if (flags & CLOSE_RANGE_UNSHARE) {
        errno = EINVAL;           /* sharing a table is the kernel's to undo */
        return -1;
    }
    long max = sysconf(_SC_OPEN_MAX);
    if (max <= 0 || max > 65536)
        max = 65536;
    for (unsigned long fd = first; fd <= last && fd < (unsigned long)max; fd++) {
        if (flags & CLOSE_RANGE_CLOEXEC)
            fcntl((int)fd, F_SETFD, FD_CLOEXEC);
        else
            close((int)fd);
    }
    return 0;
}

WEAK void closefrom(int lowfd)
{
    if (lowfd < 0)
        lowfd = 0;
    close_range((unsigned)lowfd, ~0U, 0);
}

WEAK int memfd_create(const char *name, unsigned flags)
{
    return (int)S2(SYS_memfd_create, name, flags);
}

/* ---- file systems ------------------------------------------------------------------- */

#if !LINUX
static void from_statvfs(const struct statvfs *v, struct statfs *f)
{
    memset(f, 0, sizeof *f);
    f->f_bsize = (long)v->f_bsize;
    f->f_frsize = (long)v->f_frsize;
    f->f_blocks = v->f_blocks;
    f->f_bfree = v->f_bfree;
    f->f_bavail = v->f_bavail;
    f->f_files = v->f_files;
    f->f_ffree = v->f_ffree;
    f->f_fsid.__val[0] = (int)v->f_fsid;
    f->f_namelen = (long)v->f_namemax;
    f->f_flags = (long)v->f_flag;
}
#endif

WEAK int statfs(const char *path, struct statfs *buf)
{
#if LINUX
    return (int)S2(SYS_statfs, path, buf);
#else
    struct statvfs v;
    if (statvfs(path, &v))
        return -1;
    from_statvfs(&v, buf);
    return 0;
#endif
}

WEAK int fstatfs(int fd, struct statfs *buf)
{
#if LINUX
    return (int)S2(SYS_fstatfs, fd, buf);
#else
    struct statvfs v;
    if (fstatvfs(fd, &v))
        return -1;
    from_statvfs(&v, buf);
    return 0;
#endif
}

WEAK int mount(const char *source, const char *target, const char *type,
               unsigned long flags, const void *data)
{
    return (int)S5(SYS_mount, source, target, type, flags, data);
}

WEAK int umount(const char *target) { return (int)S2(SYS_umount2, target, 0); }

WEAK int umount2(const char *target, int flags)
{
    return (int)S2(SYS_umount2, target, flags);
}

WEAK int swapon(const char *path, int flags)
{
    return (int)S2(SYS_swapon, path, flags);
}

WEAK int swapoff(const char *path) { return (int)S1(SYS_swapoff, path); }

/* ---- extended attributes ----------------------------------------------------------- */

WEAK ssize_t getxattr(const char *p, const char *name, void *v, size_t n)
{
    return S4(SYS_getxattr, p, name, v, n);
}
WEAK ssize_t lgetxattr(const char *p, const char *name, void *v, size_t n)
{
    return S4(SYS_lgetxattr, p, name, v, n);
}
WEAK ssize_t fgetxattr(int fd, const char *name, void *v, size_t n)
{
    return S4(SYS_fgetxattr, fd, name, v, n);
}
WEAK int setxattr(const char *p, const char *name, const void *v, size_t n, int fl)
{
    return (int)S5(SYS_setxattr, p, name, v, n, fl);
}
WEAK int lsetxattr(const char *p, const char *name, const void *v, size_t n, int fl)
{
    return (int)S5(SYS_lsetxattr, p, name, v, n, fl);
}
WEAK int fsetxattr(int fd, const char *name, const void *v, size_t n, int fl)
{
    return (int)S5(SYS_fsetxattr, fd, name, v, n, fl);
}
WEAK ssize_t listxattr(const char *p, char *list, size_t n)
{
    return S3(SYS_listxattr, p, list, n);
}
WEAK ssize_t llistxattr(const char *p, char *list, size_t n)
{
    return S3(SYS_llistxattr, p, list, n);
}
WEAK ssize_t flistxattr(int fd, char *list, size_t n)
{
    return S3(SYS_flistxattr, fd, list, n);
}
WEAK int removexattr(const char *p, const char *name)
{
    return (int)S2(SYS_removexattr, p, name);
}
WEAK int lremovexattr(const char *p, const char *name)
{
    return (int)S2(SYS_lremovexattr, p, name);
}
WEAK int fremovexattr(int fd, const char *name)
{
    return (int)S2(SYS_fremovexattr, fd, name);
}

/* ---- the machine ----------------------------------------------------------------------- */

WEAK int sysinfo(struct sysinfo *info)
{
#if LINUX
    return (int)S1(SYS_sysinfo, info);
#else
    memset(info, 0, sizeof *info);
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) == 0)
        info->uptime = (long)t.tv_sec;
    info->procs = 1;
    info->mem_unit = 1;
    return 0;
#endif
}

WEAK int get_nprocs(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

WEAK int get_nprocs_conf(void)
{
    long n = sysconf(_SC_NPROCESSORS_CONF);
    return n > 0 ? (int)n : 1;
}

WEAK long get_phys_pages(void) { return sysconf(_SC_PHYS_PAGES); }
WEAK long get_avphys_pages(void) { return sysconf(_SC_AVPHYS_PAGES); }

WEAK int prctl(int option, ...)
{
    va_list ap;
    va_start(ap, option);
    unsigned long a = va_arg(ap, unsigned long), b = va_arg(ap, unsigned long);
    unsigned long c = va_arg(ap, unsigned long), d = va_arg(ap, unsigned long);
    va_end(ap);
    return (int)S5(SYS_prctl, option, a, b, c, d);
}

WEAK int personality(unsigned long persona)
{
    return (int)S1(SYS_personality, persona);
}

WEAK int klogctl(int type, char *buf, int n)
{
    return (int)S3(SYS_syslog, type, buf, n);
}

WEAK int reboot(int how)
{
    return (int)S4(SYS_reboot, 0xfee1deadL, 672274793L, how, 0);
}

WEAK int acct(const char *path) { return (int)S1(SYS_acct, path); }
WEAK int vhangup(void) { return (int)S0(SYS_vhangup); }

WEAK pid_t gettid(void)
{
#if LINUX
    return (pid_t)S0(SYS_gettid);
#else
    return getpid();
#endif
}

WEAK int getdomainname(char *name, size_t n)
{
    struct utsname u;
    const char *d = uname(&u) == 0 ? u.domainname : "(none)";
    size_t len = strlen(d);
    if (len >= n) {
        errno = EINVAL;
        return -1;
    }
    memcpy(name, d, len + 1);
    return 0;
}

WEAK int setdomainname(const char *name, size_t n)
{
    return (int)S2(SYS_setdomainname, name, n);
}

WEAK unsigned long getauxval(unsigned long type)
{
    const long *a = __os_auxv();
    for (; a && a[0]; a += 2)
        if ((unsigned long)a[0] == type)
            return (unsigned long)a[1];
    errno = ENOENT;
    return 0;
}

/* ---- processors and namespaces ----------------------------------------------------------- */

WEAK int __sched_cpucount(size_t size, const cpu_set_t *set)
{
    int n = 0;
    for (size_t i = 0; i < size / sizeof(long); i++)
        n += __builtin_popcountl(set->__bits[i]);
    return n;
}

WEAK int sched_getaffinity(pid_t pid, size_t size, cpu_set_t *set)
{
#if LINUX
    long r = S3(SYS_sched_getaffinity, pid, size, set);
    if (r < 0)
        return -1;
    /* the kernel says how much it wrote; the rest is no processor */
    if ((size_t)r < size)
        memset((char *)set + r, 0, size - (size_t)r);
    return 0;
#else
    (void)pid;
    memset(set, 0, size);
    int n = get_nprocs();
    for (int i = 0; i < n && (size_t)i / 8 < size; i++)
        set->__bits[i / (8 * sizeof(long))] |= 1UL << (i % (8 * sizeof(long)));
    return 0;
#endif
}

WEAK int sched_setaffinity(pid_t pid, size_t size, const cpu_set_t *set)
{
#if LINUX
    return (int)S3(SYS_sched_setaffinity, pid, size, set);
#else
    (void)pid;
    /* one processor: a set that includes it is the only one possible */
    if (size && (set->__bits[0] & 1))
        return 0;
    errno = EINVAL;
    return -1;
#endif
}

WEAK int getcpu(unsigned *cpu, unsigned *node)
{
#if LINUX
    return (int)S3(SYS_getcpu, cpu, node, 0);
#else
    if (cpu)
        *cpu = 0;
    if (node)
        *node = 0;
    return 0;
#endif
}

WEAK int sched_getcpu(void)
{
    unsigned cpu;
    return getcpu(&cpu, 0) ? -1 : (int)cpu;
}

WEAK int unshare(int flags) { return (int)S1(SYS_unshare, flags); }
WEAK int setns(int fd, int nstype) { return (int)S2(SYS_setns, fd, nstype); }

/* ---- memory ---------------------------------------------------------------------------- */

/* where nothing pages, every page is already locked in */
WEAK int mlockall(int flags)
{
#if LINUX
    return (int)S1(SYS_mlockall, flags);
#else
    (void)flags;
    return 0;
#endif
}

WEAK int munlockall(void)
{
#if LINUX
    return (int)S0(SYS_munlockall);
#else
    return 0;
#endif
}

WEAK int mlock2(const void *addr, size_t len, unsigned flags)
{
    return (int)S3(SYS_mlock2, addr, len, flags);
}

WEAK int mincore(void *addr, size_t len, unsigned char *vec)
{
    return (int)S3(SYS_mincore, addr, len, vec);
}

WEAK void *mremap(void *old, size_t oldlen, size_t newlen, int flags, ...)
{
    void *addr = 0;
    if (flags & MREMAP_FIXED) {
        va_list ap;
        va_start(ap, flags);
        addr = va_arg(ap, void *);
        va_end(ap);
    }
    long r = S5(SYS_mremap, old, oldlen, newlen, flags, addr);
    return r == -1 ? MAP_FAILED : (void *)r;
}

/* ---- limits of another process ------------------------------------------------ */

WEAK int prlimit(pid_t pid, int resource, const struct rlimit *newlim,
                 struct rlimit *old)
{
#if LINUX
    return (int)S4(SYS_prlimit64, pid, resource, newlim, old);
#else
    if (pid && pid != getpid()) {
        errno = ESRCH;
        return -1;
    }
    if (old && getrlimit(resource, old))
        return -1;
    return newlim ? setrlimit(resource, newlim) : 0;
#endif
}

/* ---- the rest of Linux's newer calls ------------------------------------------ */

WEAK ssize_t preadv2(int fd, const struct iovec *iov, int n, off_t off, int flags)
{
    if (!flags && off >= 0)
        return preadv(fd, iov, n, off);
#if LINUX
    /* the kernel takes the offset as two words, low then high */
    return S6(SYS_preadv2, fd, iov, n, off, 0, flags);
#else
    (void)iov; (void)n; (void)off;
    errno = ENOSYS;
    return -1;
#endif
}

WEAK ssize_t pwritev2(int fd, const struct iovec *iov, int n, off_t off, int flags)
{
    if (!flags && off >= 0)
        return pwritev(fd, iov, n, off);
#if LINUX
    return S6(SYS_pwritev2, fd, iov, n, off, 0, flags);
#else
    (void)iov; (void)n; (void)off;
    errno = ENOSYS;
    return -1;
#endif
}

WEAK int recvmmsg(int fd, struct mmsghdr *v, unsigned n, int flags,
                  struct timespec *timeout)
{
    return (int)S5(SYS_recvmmsg, fd, v, n, flags, timeout);
}

WEAK int sendmmsg(int fd, struct mmsghdr *v, unsigned n, int flags)
{
    return (int)S4(SYS_sendmmsg, fd, v, n, flags);
}

WEAK int renameat2(int fromfd, const char *from, int tofd, const char *to,
                   unsigned flags)
{
    if (!flags)
        return renameat(fromfd, from, tofd, to);
    return (int)S5(SYS_renameat2, fromfd, from, tofd, to, flags);
}

WEAK int execveat(int dirfd, const char *path, char *const argv[],
                  char *const envp[], int flags)
{
    return (int)S5(SYS_execveat, dirfd, path, argv, envp, flags);
}

WEAK int tgkill(pid_t tgid, pid_t tid, int sig)
{
    return (int)S3(SYS_tgkill, tgid, tid, sig);
}

WEAK int statx(int dirfd, const char *restrict path, int flags, unsigned mask,
               struct statx *restrict buf)
{
    return (int)S5(SYS_statx, dirfd, path, flags, mask, buf);
}
