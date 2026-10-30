/* The rest of POSIX's process and file calls: waitid, posix_fallocate,
 * fexecve, priorities and scheduling parameters, the saved ids, sigqueue,
 * swab, a process's CPU-time clock.
 *
 * On Linux each is its system call (<sys/syscall.h>). Elsewhere it is
 * built from what the backend has where that gives the same answer --
 * waitid from waitpid, posix_fallocate by extending the file, fexecve
 * through the descriptor's path -- and otherwise says what a system with
 * one priority, one user and no real-time scheduling truthfully is.
 */
#define _GNU_SOURCE 1
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#include "posix.h"

#if defined(__linux__)
#define LINUX 1
#else
#define LINUX 0
#endif

/* ---- waitid ------------------------------------------------------------------------ */

WEAK int waitid(idtype_t idtype, id_t id, siginfo_t *info, int options)
{
    if ((unsigned)idtype > P_PGID ||
        !(options & (WEXITED | WSTOPPED | WCONTINUED)) ||
        (options & ~(WNOHANG | WEXITED | WSTOPPED | WCONTINUED | WNOWAIT))) {
        errno = EINVAL;
        return -1;
    }
#if LINUX
    memset(info, 0, sizeof *info);
    return (int)syscall(SYS_waitid, (long)idtype, (long)id, (long)info,
                        (long)options, 0L);
#else
    /* waitpid reports an exit whatever it is asked, and reaps what it
     * reports: it can answer only a question that wants both */
    if (!(options & WEXITED) || (options & WNOWAIT)) {
        errno = ENOSYS;
        return -1;
    }
    pid_t pid = idtype == P_ALL ? -1 : idtype == P_PID ? (pid_t)id : -(pid_t)id;
    int st, opts = (options & WNOHANG) | (options & WSTOPPED ? WUNTRACED : 0) |
                   (options & WCONTINUED);
    pid_t r = waitpid(pid, &st, opts);
    if (r < 0)
        return -1;
    memset(info, 0, sizeof *info);
    if (r == 0)
        return 0;                      /* WNOHANG, nothing ready: si_pid 0 */
    info->si_signo = SIGCHLD;
    info->si_pid = r;
    info->si_uid = getuid();
    if (WIFEXITED(st)) {
        info->si_code = CLD_EXITED;
        info->si_status = WEXITSTATUS(st);
    } else if (WIFSIGNALED(st)) {
        info->si_code = WCOREDUMP(st) ? CLD_DUMPED : CLD_KILLED;
        info->si_status = WTERMSIG(st);
    } else if (WIFSTOPPED(st)) {
        info->si_code = CLD_STOPPED;
        info->si_status = WSTOPSIG(st);
    } else {
        info->si_code = CLD_CONTINUED;
        info->si_status = SIGCONT;
    }
    return 0;
#endif
}

/* ---- files -------------------------------------------------------------------------- */

/* POSIX returns the error here rather than setting errno */
WEAK int posix_fallocate(int fd, off_t off, off_t len)
{
    if (off < 0 || len <= 0)
        return EINVAL;
    int saved = errno, e;
#if LINUX
    if (syscall(SYS_fallocate, (long)fd, 0L, (long)off, (long)len) == 0) {
        errno = saved;
        return 0;
    }
    e = errno;
    if (e != EOPNOTSUPP && e != ENOSYS) {
        errno = saved;
        return e;
    }
#endif
    /* no allocation call: a file at least this long has the space */
    struct stat st;
    if (fstat(fd, &st)) {
        e = errno;
        errno = saved;
        return e;
    }
    if (!S_ISREG(st.st_mode)) {
        errno = saved;
        return S_ISFIFO(st.st_mode) ? ESPIPE : ENODEV;
    }
    if (off > LLONG_MAX - len) {
        errno = saved;
        return EFBIG;
    }
    e = 0;
    if (st.st_size < off + len && ftruncate(fd, off + len))
        e = errno;
    errno = saved;
    return e;
}

WEAK int fexecve(int fd, char *const argv[], char *const envp[])
{
#if LINUX && defined(SYS_execveat)
    /* AT_EMPTY_PATH: the descriptor itself */
    syscall(SYS_execveat, (long)fd, (long)"", (long)argv, (long)envp, 0x1000L);
    if (errno != ENOSYS)
        return -1;
#endif
    char path[PATH_MAX];
    if (__os_fdpath(fd, path, sizeof path) < 0)
        return -1;
    return execve(path, argv, envp);
}

/* ---- priorities and scheduling --------------------------------------------------------- */

WEAK int getpriority(int which, id_t who)
{
    if ((unsigned)which > PRIO_USER) {
        errno = EINVAL;
        return -1;
    }
#if LINUX
    long r = syscall(SYS_getpriority, (long)which, (long)who);
    return r < 0 ? -1 : 20 - (int)r;   /* the kernel answers 20 - nice */
#else
    (void)who;
    return 0;                          /* one priority: the default */
#endif
}

WEAK int setpriority(int which, id_t who, int prio)
{
    if ((unsigned)which > PRIO_USER) {
        errno = EINVAL;
        return -1;
    }
#if LINUX
    return (int)syscall(SYS_setpriority, (long)which, (long)who, (long)prio);
#else
    (void)who;
    if (prio == 0)
        return 0;
    errno = EACCES;
    return -1;
#endif
}

WEAK int nice(int incr)
{
    int saved = errno;
    errno = 0;
    int now = getpriority(PRIO_PROCESS, 0);
    if (now == -1 && errno)
        return -1;
    int want = now + incr;
    if (want > 19)
        want = 19;
    if (want < -20)
        want = -20;
    if (setpriority(PRIO_PROCESS, 0, want)) {
        if (errno == EACCES)
            errno = EPERM;             /* nice's word for it */
        return -1;
    }
    errno = saved;
    return getpriority(PRIO_PROCESS, 0);
}

WEAK int sched_getparam(pid_t pid, struct sched_param *param)
{
#if LINUX
    return (int)syscall(SYS_sched_getparam, (long)pid, (long)param);
#else
    (void)pid;
    param->sched_priority = 0;
    return 0;
#endif
}

WEAK int sched_setparam(pid_t pid, const struct sched_param *param)
{
#if LINUX
    return (int)syscall(SYS_sched_setparam, (long)pid, (long)param);
#else
    (void)pid;
    if (param->sched_priority == 0)
        return 0;
    errno = EINVAL;
    return -1;
#endif
}

WEAK int sched_getscheduler(pid_t pid)
{
#if LINUX
    return (int)syscall(SYS_sched_getscheduler, (long)pid);
#else
    (void)pid;
    return SCHED_OTHER;
#endif
}

WEAK int sched_setscheduler(pid_t pid, int policy,
                            const struct sched_param *param)
{
#if LINUX
    return (int)syscall(SYS_sched_setscheduler, (long)pid, (long)policy,
                        (long)param);
#else
    (void)pid;
    if (policy == SCHED_OTHER && param->sched_priority == 0)
        return SCHED_OTHER;
    errno = policy == SCHED_FIFO || policy == SCHED_RR ? EPERM : EINVAL;
    return -1;
#endif
}

WEAK int sched_rr_get_interval(pid_t pid, struct timespec *t)
{
#if LINUX
    return (int)syscall(SYS_sched_rr_get_interval, (long)pid, (long)t);
#else
    (void)pid;
    (void)t;
    errno = ENOSYS;
    return -1;
#endif
}

/* ---- the saved ids ---------------------------------------------------------------------- */

WEAK int getresuid(uid_t *r, uid_t *e, uid_t *s)
{
#if LINUX
    return (int)syscall(SYS_getresuid, (long)r, (long)e, (long)s);
#else
    *r = getuid();
    *e = geteuid();
    *s = geteuid();
    return 0;
#endif
}

WEAK int getresgid(gid_t *r, gid_t *e, gid_t *s)
{
#if LINUX
    return (int)syscall(SYS_getresgid, (long)r, (long)e, (long)s);
#else
    *r = getgid();
    *e = getegid();
    *s = getegid();
    return 0;
#endif
}

WEAK int setresuid(uid_t r, uid_t e, uid_t s)
{
#if LINUX
    return (int)syscall(SYS_setresuid, (long)(int)r, (long)(int)e, (long)(int)s);
#else
    /* one user: setting the ids to what they are is all there is */
    uid_t now = getuid();
    if ((r == (uid_t)-1 || r == now) && (e == (uid_t)-1 || e == now) &&
        (s == (uid_t)-1 || s == now))
        return 0;
    errno = EPERM;
    return -1;
#endif
}

WEAK int setresgid(gid_t r, gid_t e, gid_t s)
{
#if LINUX
    return (int)syscall(SYS_setresgid, (long)(int)r, (long)(int)e, (long)(int)s);
#else
    gid_t now = getgid();
    if ((r == (gid_t)-1 || r == now) && (e == (gid_t)-1 || e == now) &&
        (s == (gid_t)-1 || s == now))
        return 0;
    errno = EPERM;
    return -1;
#endif
}

/* ---- signals with a value ------------------------------------------------------------------ */

WEAK int sigqueue(pid_t pid, int sig, const union sigval value)
{
#if LINUX
    siginfo_t si;
    memset(&si, 0, sizeof si);
    si.si_signo = sig;
    si.si_code = SI_QUEUE;
    si.si_pid = getpid();
    si.si_uid = getuid();
    si.si_value = value;
    return (int)syscall(SYS_rt_sigqueueinfo, (long)pid, (long)sig, (long)&si);
#else
    /* the value has nowhere to go without a kernel queue; the signal does */
    (void)value;
    return kill(pid, sig);
#endif
}

/* ---- the rest ------------------------------------------------------------------------------ */

WEAK int clock_getcpuclockid(pid_t pid, clockid_t *clock)
{
#if LINUX
    /* the kernel's encoding of a process's CPU clock: ~pid << 3 | 2 */
    clockid_t c = (clockid_t)((~(unsigned)pid << 3) | 2);
    struct timespec t;
    if (pid && syscall(SYS_clock_getres, (long)c, (long)&t))
        return errno == EINVAL ? ESRCH : errno;
    *clock = pid ? c : CLOCK_PROCESS_CPUTIME_ID;
    return 0;
#else
    if (pid && pid != getpid())
        return EPERM;
    *clock = CLOCK_PROCESS_CPUTIME_ID;
    return 0;
#endif
}

WEAK void swab(const void *restrict from, void *restrict to, ssize_t n)
{
    const unsigned char *f = from;
    unsigned char *t = to;
    for (ssize_t i = 0; i + 1 < n; i += 2) {
        t[i] = f[i + 1];
        t[i + 1] = f[i];
    }
}

/* the host's id: the four bytes of /etc/hostid, as glibc keeps it, or 0 */
WEAK long gethostid(void)
{
    int fd = open("/etc/hostid", O_RDONLY);
    if (fd < 0)
        return 0;
    int v;
    ssize_t n = read(fd, &v, sizeof v);
    close(fd);
    return n == (ssize_t)sizeof v ? v : 0;
}

WEAK int sethostid(long id)
{
    int v = (int)id;
    int fd = open("/etc/hostid", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    ssize_t n = write(fd, &v, sizeof v);
    close(fd);
    return n == (ssize_t)sizeof v ? 0 : -1;
}

WEAK char *getwd(char *buf)
{
    if (!getcwd(buf, PATH_MAX)) {
        strncpy(buf, strerror(errno), PATH_MAX - 1);
        buf[PATH_MAX - 1] = 0;
        return 0;
    }
    return buf;
}

/* $PWD when it names this directory, which keeps the symbolic links a
 * user went through; otherwise getcwd's */
WEAK char *get_current_dir_name(void)
{
    const char *pwd = getenv("PWD");
    struct stat a, b;
    if (pwd && *pwd == '/' && stat(pwd, &a) == 0 && stat(".", &b) == 0 &&
        a.st_dev == b.st_dev && a.st_ino == b.st_ino)
        return strdup(pwd);
    char *buf = malloc(PATH_MAX);
    if (buf && !getcwd(buf, PATH_MAX)) {
        free(buf);
        return 0;
    }
    return buf;
}

/* ---- who the terminal belongs to --------------------------------------------------- */

#include <pwd.h>
#include <utmpx.h>

/* the name of the effective user, into s (L_cuserid bytes) or a buffer
 * of its own */
WEAK char *cuserid(char *s)
{
    static char own[9];
    struct passwd *pw = getpwuid(geteuid());
    char *d = s ? s : own;
    if (!pw) {
        if (s)
            *s = 0;
        return s;
    }
    strncpy(d, pw->pw_name, 8);
    d[8] = 0;
    return d;
}

/* the terminal's slot in the login records: its record's index in utmp,
 * or 0 when it has none (glibc's answer then) */
WEAK int ttyslot(void)
{
    const char *tty = 0;
    for (int fd = 0; fd < 3 && !tty; fd++)
        tty = ttyname(fd);
    if (!tty)
        return 0;
    if (!strncmp(tty, "/dev/", 5))
        tty += 5;
    setutxent();
    int slot = 0, found = 0;
    struct utmpx *e;
    for (int i = 0; (e = getutxent()); i++)
        if (!strncmp(e->ut_line, tty, sizeof e->ut_line)) {
            slot = i;
            found = 1;
            break;
        }
    endutxent();
    return found ? slot : 0;
}
