/* Processes: their ids, fork and the exec family, waiting, system, the
 * sleeps and alarm. On a kernel that starts processes rather than
 * copying them (EmbLinkOS) fork fails with ENOSYS and system, popen and
 * posix_spawn still work: they are built on spawn, not on fork. */
#include <unistd.h>
#include <stdarg.h>
#include <stdlib.h>
#include <signal.h>
#include <spawn.h>
#include <time.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <paths.h>

#include "posix.h"

/* ---- ids ------------------------------------------------------------------- */

WEAK pid_t getpid(void)  { return (pid_t)__os_getid(__OS_ID_PID); }
WEAK pid_t getppid(void) { return (pid_t)__os_getid(__OS_ID_PPID); }
WEAK uid_t getuid(void)  { return (uid_t)__os_getid(__OS_ID_UID); }
WEAK uid_t geteuid(void) { return (uid_t)__os_getid(__OS_ID_EUID); }
WEAK gid_t getgid(void)  { return (gid_t)__os_getid(__OS_ID_GID); }
WEAK gid_t getegid(void) { return (gid_t)__os_getid(__OS_ID_EGID); }

WEAK int setuid(uid_t id)  { return __os_setid(__OS_ID_UID, id); }
WEAK int seteuid(uid_t id) { return __os_setid(__OS_ID_EUID, id); }
WEAK int setgid(gid_t id)  { return __os_setid(__OS_ID_GID, id); }
WEAK int setegid(gid_t id) { return __os_setid(__OS_ID_EGID, id); }

/* setreuid(r, e): either may be -1, for "leave it" */
WEAK int setreuid(uid_t r, uid_t e)
{
    if (r != (uid_t)-1 && __os_setid(__OS_ID_UID, r) < 0)
        return -1;
    return e != (uid_t)-1 ? __os_setid(__OS_ID_EUID, e) : 0;
}

WEAK int setregid(gid_t r, gid_t e)
{
    if (r != (gid_t)-1 && __os_setid(__OS_ID_GID, r) < 0)
        return -1;
    return e != (gid_t)-1 ? __os_setid(__OS_ID_EGID, e) : 0;
}

WEAK int getgroups(int n, gid_t list[])
{
    return __os_getgroups(n, (unsigned *)list);
}

WEAK pid_t getpgid(pid_t pid)          { return __os_getpgid(pid); }
WEAK pid_t getpgrp(void)               { return __os_getpgid(0); }
WEAK int   setpgid(pid_t pid, pid_t g) { return __os_setpgid(pid, g); }
WEAK int   setpgrp(void)               { return __os_setpgid(0, 0); }
WEAK pid_t getsid(pid_t pid)           { return __os_getsid(pid); }
WEAK pid_t setsid(void)                { return __os_setsid(); }

/* The login name: LOGNAME as the session set it, which is what POSIX's
 * getlogin answers on a system with no utmp to ask. NULL with ENXIO when
 * there is none. */
WEAK char *getlogin(void)
{
    char *s = getenv("LOGNAME");
    if (!s || !*s)
        s = getenv("USER");
    if (!s || !*s)
        errno = ENXIO;
    return s && *s ? s : NULL;
}

WEAK int getlogin_r(char *name, size_t n)
{
    char *s = getlogin();
    if (!s)
        return errno;
    size_t k = strlen(s);
    if (k + 1 > n)
        return ERANGE;
    memcpy(name, s, k + 1);
    return 0;
}

/* ---- fork and exec --------------------------------------------------------- */

/* pthread_atfork's handlers, when anything registered one. A pointer
 * pthread_atfork sets, so that a program which never names a pthread
 * function does not link the thread library to fork -- and not a weak
 * reference, which left undefined has no PC-relative address in a
 * position-independent program (EmbLinkOS links its apps PIE). */
void (*__atfork_hook)(int phase);

WEAK pid_t fork(void)
{
    void (*hook)(int) = __atfork_hook;
    if (hook)
        hook(0);
    pid_t p = __os_fork();
    if (hook && p >= 0)
        hook(p == 0 ? 2 : 1);
    return p;
}

/* vfork's one promise is that the child only execs or _exits, which a
 * fork keeps too */
WEAK pid_t vfork(void) { return __os_fork(); }

WEAK int execve(const char *path, char *const argv[], char *const envp[])
{
    return __os_execve(path, argv, envp);
}

WEAK int execv(const char *path, char *const argv[])
{
    extern char **environ;
    return __os_execve(path, argv, environ);
}

/* A file that is not a program the kernel recognises (ENOEXEC) is a
 * shell script, as POSIX's exec*p have it: run by /bin/sh with the file
 * as its first argument. */
static int exec_script(const char *path, char *const argv[],
                       char *const envp[])
{
    int n = 0;
    while (argv[n])
        n++;
    char **v = malloc((size_t)(n + 2) * sizeof *v);
    if (!v)
        return -1;
    v[0] = (char *)"sh";
    v[1] = (char *)path;
    for (int i = 1; i <= n; i++)
        v[i + 1] = argv[i];
    if (n == 0)
        v[2] = NULL;
    __os_execve(_PATH_BSHELL, v, envp);
    free(v);
    return -1;
}

/* The search POSIX describes: each PATH element in turn, an empty one
 * meaning the current directory; an EACCES is remembered and reported
 * if nothing later is found, so a directory the user cannot read does
 * not hide a program further along. */
WEAK int execvpe(const char *file, char *const argv[], char *const envp[])
{
    if (!file || !*file) {
        errno = ENOENT;
        return -1;
    }
    if (strchr(file, '/')) {
        __os_execve(file, argv, envp);
        if (errno == ENOEXEC)
            exec_script(file, argv, envp);
        return -1;
    }
    const char *path = getenv("PATH");
    if (!path)
        path = "/usr/local/bin:/bin:/usr/bin";
    size_t fl = strlen(file);
    if (fl > NAME_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }
    int seen_eacces = 0;
    char buf[PATH_MAX];
    for (const char *p = path;; p++) {
        const char *end = strchr(p, ':');
        size_t dl = end ? (size_t)(end - p) : strlen(p);
        if (dl + 1 + fl + 1 <= sizeof buf) {
            memcpy(buf, p, dl);
            size_t at = dl;
            if (dl)
                buf[at++] = '/';
            memcpy(buf + at, file, fl + 1);
            __os_execve(buf, argv, envp);
            if (errno == ENOEXEC) {
                exec_script(buf, argv, envp);
                return -1;
            }
            if (errno == EACCES)
                seen_eacces = 1;
            else if (errno != ENOENT && errno != ENOTDIR && errno != ENOSYS)
                return -1;
            else if (errno == ENOSYS)
                return -1;
        }
        if (!end)
            break;
        p = end;
    }
    errno = seen_eacces ? EACCES : ENOENT;
    return -1;
}

WEAK int execvp(const char *file, char *const argv[])
{
    extern char **environ;
    return execvpe(file, argv, environ);
}

/* the l forms: their arguments, counted, into a vector on the stack */
static int count_args(const char *first, va_list ap)
{
    (void)first;
    int n = 1;
    va_list c;
    va_copy(c, ap);
    while (va_arg(c, char *))
        n++;
    va_end(c);
    return n;
}

WEAK int execl(const char *path, const char *arg, ...)
{
    va_list ap;
    va_start(ap, arg);
    int n = count_args(arg, ap);
    char *v[n + 1];
    v[0] = (char *)arg;
    for (int i = 1; i <= n; i++)
        v[i] = va_arg(ap, char *);
    va_end(ap);
    return execv(path, v);
}

WEAK int execlp(const char *file, const char *arg, ...)
{
    va_list ap;
    va_start(ap, arg);
    int n = count_args(arg, ap);
    char *v[n + 1];
    v[0] = (char *)arg;
    for (int i = 1; i <= n; i++)
        v[i] = va_arg(ap, char *);
    va_end(ap);
    return execvp(file, v);
}

/* the environment comes after the terminating NULL */
WEAK int execle(const char *path, const char *arg, ...)
{
    va_list ap;
    va_start(ap, arg);
    int n = count_args(arg, ap);
    char *v[n + 1];
    v[0] = (char *)arg;
    for (int i = 1; i <= n; i++)
        v[i] = va_arg(ap, char *);
    char **envp = va_arg(ap, char **);
    va_end(ap);
    return execve(path, v, envp);
}

/* ---- waiting ------------------------------------------------------------- */

WEAK pid_t waitpid(pid_t pid, int *status, int options)
{
    return __os_waitpid(pid, status, options);
}

WEAK pid_t wait(int *status) { return __os_waitpid(-1, status, 0); }

/* wait3 and wait4 report usage the seam has no per-child form of: what
 * there is, getrusage's CHILDREN, which a caller reaping one child at a
 * time reads as that child's -- and zeros where even that is not had */
WEAK pid_t wait4(pid_t pid, int *status, int options, struct rusage *ru)
{
    pid_t r = __os_waitpid(pid, status, options);
    if (r > 0 && ru) {
        struct __os_rusage k;
        memset(ru, 0, sizeof *ru);
        int saved = errno;
        if (__os_getrusage(-1, &k) == 0) {
            ru->ru_utime.tv_sec = (time_t)(k.utime_us / 1000000);
            ru->ru_utime.tv_usec = (suseconds_t)(k.utime_us % 1000000);
            ru->ru_stime.tv_sec = (time_t)(k.stime_us / 1000000);
            ru->ru_stime.tv_usec = (suseconds_t)(k.stime_us % 1000000);
            ru->ru_maxrss = k.maxrss_kb;
        }
        errno = saved;
    }
    return r;
}

WEAK pid_t wait3(int *status, int options, struct rusage *ru)
{
    return wait4(-1, status, options, ru);
}

/* ---- system -----------------------------------------------------------------
 *
 * POSIX's, on posix_spawn: /bin/sh -c cmd, with SIGINT and SIGQUIT
 * ignored and SIGCHLD blocked in the caller while the command runs, so a
 * ^C goes to the command and the wait is not stolen by a handler. Where
 * there are no signals those steps fail quietly and the rest stands.
 * system(NULL) asks whether there is a shell to run commands with. */
WEAK int system(const char *cmd)
{
    if (!cmd)
        return __os_access(_PATH_BSHELL, X_OK) == 0;
    extern char **environ;
    struct sigaction ign, oint, oquit;
    sigset_t chld, omask;
    memset(&ign, 0, sizeof ign);
    ign.sa_handler = SIG_IGN;
    /* Only where the kernel delivers signals: elsewhere nothing can
     * interrupt the wait, a child inherits no dispositions, and a spawn
     * asked to set them is refused (EmbLinkOS makes no promises about
     * signals it does not have). */
    int saved = errno;
    struct __os_sigaction probe;
    int kernel = __os_sigaction(SIGINT, NULL, &probe) == 0;
    int have_sig = kernel && sigaction(SIGINT, &ign, &oint) == 0;
    if (have_sig)
        sigaction(SIGQUIT, &ign, &oquit);
    sigemptyset(&chld);
    sigaddset(&chld, SIGCHLD);
    int have_mask = kernel && sigprocmask(SIG_BLOCK, &chld, &omask) == 0;
    errno = saved;

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    short flags = 0;
    if (have_mask) {
        posix_spawnattr_setsigmask(&attr, &omask);
        flags |= POSIX_SPAWN_SETSIGMASK;
    }
    if (have_sig) {
        sigset_t def;
        sigemptyset(&def);
        if (oint.sa_handler != SIG_IGN)
            sigaddset(&def, SIGINT);
        if (oquit.sa_handler != SIG_IGN)
            sigaddset(&def, SIGQUIT);
        posix_spawnattr_setsigdefault(&attr, &def);
        flags |= POSIX_SPAWN_SETSIGDEF;
    }
    posix_spawnattr_setflags(&attr, flags);
    char *argv[] = { (char *)"sh", (char *)"-c", (char *)cmd, NULL };
    pid_t pid;
    int st = -1;
    int err = posix_spawn(&pid, _PATH_BSHELL, NULL, &attr, argv, environ);
    posix_spawnattr_destroy(&attr);
    if (err == 0) {
        while (__os_waitpid(pid, &st, 0) < 0) {
            if (errno != EINTR) {
                st = -1;
                break;
            }
        }
    } else if (err == ENOENT || err == EACCES || err == ENOEXEC) {
        st = 127 << 8;          /* as if the shell had run and failed */
    } else {
        errno = err;
    }
    if (have_sig) {
        sigaction(SIGINT, &oint, NULL);
        sigaction(SIGQUIT, &oquit, NULL);
    }
    if (have_mask)
        sigprocmask(SIG_SETMASK, &omask, NULL);
    return st;
}

/* ---- sleeping ---------------------------------------------------------------- */

WEAK int nanosleep(const struct timespec *req, struct timespec *rem)
{
    if (req->tv_nsec < 0 || req->tv_nsec >= 1000000000L || req->tv_sec < 0) {
        errno = EINVAL;
        return -1;
    }
    struct __os_timespec k = { req->tv_sec, req->tv_nsec }, kr;
    int r = __os_clock_nanosleep(CLOCK_MONOTONIC, 0, &k, rem ? &kr : NULL);
    if (r < 0 && errno == ENOSYS) {
        /* no clock_nanosleep: the plain sleep every backend has */
        long long ns = (long long)req->tv_sec * 1000000000LL + req->tv_nsec;
        return __os_sleep_ns((long)ns);
    }
    if (r < 0 && rem && errno == EINTR) {
        rem->tv_sec = (time_t)kr.sec;
        rem->tv_nsec = kr.nsec;
    }
    return r;
}

/* the seconds left when a signal cut it short */
WEAK unsigned sleep(unsigned seconds)
{
    struct timespec t = { (time_t)seconds, 0 }, rem = { 0, 0 };
    if (nanosleep(&t, &rem) < 0 && errno == EINTR)
        return (unsigned)rem.tv_sec + (rem.tv_nsec > 0);
    return 0;
}

WEAK int usleep(useconds_t us)
{
    struct timespec t = { (time_t)(us / 1000000), (long)(us % 1000000) * 1000 };
    return nanosleep(&t, NULL);
}

/* until a signal is handled: sigsuspend with the mask as it is */
WEAK int pause(void)
{
    sigset_t cur;
    if (__os_sigprocmask(SIG_BLOCK, NULL, &cur) < 0)
        return -1;
    return __os_sigsuspend(&cur);
}

/* the real-time interval timer; the seconds that were left on it */
WEAK unsigned alarm(unsigned seconds)
{
    struct __os_itimer v = { 0, 0, seconds, 0 }, old = { 0, 0, 0, 0 };
    if (__os_setitimer(ITIMER_REAL, &v, &old) < 0)
        return 0;
    return (unsigned)old.vsec + (old.vusec >= 500000);
}

WEAK useconds_t ualarm(useconds_t value, useconds_t interval)
{
    struct __os_itimer v = { interval / 1000000, interval % 1000000,
                             value / 1000000, value % 1000000 }, old;
    if (__os_setitimer(ITIMER_REAL, &v, &old) < 0)
        return 0;
    return (useconds_t)(old.vsec * 1000000 + old.vusec);
}

/* ---- detaching from the terminal ------------------------------------------
 *
 * daemon is fork, setsid, and the standard descriptors to /dev/null --
 * so on a kernel with no fork it fails with ENOSYS before changing
 * anything. */
WEAK int daemon(int nochdir, int noclose)
{
    pid_t p = __os_fork();
    if (p < 0)
        return -1;
    if (p > 0)
        _exit(0);
    if (__os_setsid() < 0)
        return -1;
    if (!nochdir)
        __os_chdir("/");
    if (!noclose) {
        int fd = __os_open(_PATH_DEVNULL, __OS_O_RDWR, 0);
        if (fd >= 0) {
            for (int i = 0; i < 3; i++)
                if (fd != i)
                    __os_dup2(fd, i, 0);
            if (fd > 2)
                __os_close(fd);
        }
    }
    return 0;
}
