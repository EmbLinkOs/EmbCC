/* <spawn.h>: a list of file actions and a set of attributes, handed whole
 * to __os_spawn, which makes the process -- by fork and exec on Linux,
 * natively on EmbLinkOS. These functions return an error number, as
 * POSIX's spawn functions do, rather than setting errno. */
#include <spawn.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <paths.h>

#include "posix.h"

WEAK int posix_spawn_file_actions_init(posix_spawn_file_actions_t *fa)
{
    memset(fa, 0, sizeof *fa);
    return 0;
}

WEAK int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *fa)
{
    for (int i = 0; i < fa->__n; i++)
        free(fa->__paths[i]);
    free(fa->__a);
    free(fa->__paths);
    memset(fa, 0, sizeof *fa);
    return 0;
}

static int add(posix_spawn_file_actions_t *fa, int kind, int fd, int newfd,
               const char *path, int oflag, mode_t mode)
{
    if (fd < 0 || (kind == __OS_SPAWN_DUP2 && newfd < 0))
        return EBADF;
    if (fa->__n == fa->__cap) {
        int cap = fa->__cap ? 2 * fa->__cap : 8;
        struct __os_spawn_action *a = realloc(fa->__a, cap * sizeof *a);
        if (!a)
            return ENOMEM;
        fa->__a = a;
        char **p = realloc(fa->__paths, cap * sizeof *p);
        if (!p)
            return ENOMEM;
        fa->__paths = p;
        fa->__cap = cap;
    }
    char *copy = NULL;
    if (path && !(copy = strdup(path)))
        return ENOMEM;
    struct __os_spawn_action *a = &fa->__a[fa->__n];
    a->kind = kind;
    a->fd = fd;
    a->newfd = newfd;
    a->oflag = oflag;
    a->mode = mode;
    a->path = copy;
    fa->__paths[fa->__n++] = copy;
    return 0;
}

WEAK int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *restrict fa,
                                          int fd, const char *restrict path,
                                          int oflag, mode_t mode)
{
    return add(fa, __OS_SPAWN_OPEN, fd, 0, path, oflag, mode);
}

WEAK int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *fa,
                                           int fd)
{
    return add(fa, __OS_SPAWN_CLOSE, fd, 0, NULL, 0, 0);
}

WEAK int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *fa,
                                          int fd, int newfd)
{
    return add(fa, __OS_SPAWN_DUP2, fd, newfd, NULL, 0, 0);
}

WEAK int posix_spawn_file_actions_addchdir(posix_spawn_file_actions_t *restrict fa,
                                           const char *restrict path)
{
    return add(fa, __OS_SPAWN_CHDIR, 0, 0, path, 0, 0);
}

WEAK int posix_spawn_file_actions_addfchdir(posix_spawn_file_actions_t *fa,
                                            int fd)
{
    return add(fa, __OS_SPAWN_FCHDIR, fd, 0, NULL, 0, 0);
}

WEAK int posix_spawn_file_actions_addchdir_np(posix_spawn_file_actions_t *restrict fa,
                                              const char *restrict path)
{
    return posix_spawn_file_actions_addchdir(fa, path);
}

WEAK int posix_spawn_file_actions_addfchdir_np(posix_spawn_file_actions_t *fa,
                                               int fd)
{
    return posix_spawn_file_actions_addfchdir(fa, fd);
}

WEAK int posix_spawnattr_init(posix_spawnattr_t *a)
{
    memset(a, 0, sizeof *a);
    return 0;
}

WEAK int posix_spawnattr_destroy(posix_spawnattr_t *a) { (void)a; return 0; }

#define ALL_FLAGS (POSIX_SPAWN_RESETIDS | POSIX_SPAWN_SETPGROUP | \
                   POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK | \
                   POSIX_SPAWN_SETSCHEDPARAM | POSIX_SPAWN_SETSCHEDULER | \
                   POSIX_SPAWN_USEVFORK | POSIX_SPAWN_SETSID)

WEAK int posix_spawnattr_setflags(posix_spawnattr_t *a, short flags)
{
    if (flags & ~ALL_FLAGS)
        return EINVAL;
    a->__flags = flags;
    return 0;
}

WEAK int posix_spawnattr_getflags(const posix_spawnattr_t *restrict a,
                                  short *restrict flags)
{
    *flags = a->__flags;
    return 0;
}

WEAK int posix_spawnattr_setpgroup(posix_spawnattr_t *a, pid_t g)
{
    a->__pgroup = g;
    return 0;
}

WEAK int posix_spawnattr_getpgroup(const posix_spawnattr_t *restrict a,
                                   pid_t *restrict g)
{
    *g = a->__pgroup;
    return 0;
}

WEAK int posix_spawnattr_setsigdefault(posix_spawnattr_t *restrict a,
                                       const sigset_t *restrict s)
{
    a->__sigdefault = *s;
    return 0;
}

WEAK int posix_spawnattr_getsigdefault(const posix_spawnattr_t *restrict a,
                                       sigset_t *restrict s)
{
    *s = a->__sigdefault;
    return 0;
}

WEAK int posix_spawnattr_setsigmask(posix_spawnattr_t *restrict a,
                                    const sigset_t *restrict s)
{
    a->__sigmask = *s;
    return 0;
}

WEAK int posix_spawnattr_getsigmask(const posix_spawnattr_t *restrict a,
                                    sigset_t *restrict s)
{
    *s = a->__sigmask;
    return 0;
}

WEAK int posix_spawnattr_setschedpolicy(posix_spawnattr_t *a, int p)
{
    a->__policy = p;
    return 0;
}

WEAK int posix_spawnattr_getschedpolicy(const posix_spawnattr_t *restrict a,
                                        int *restrict p)
{
    *p = a->__policy;
    return 0;
}

WEAK int posix_spawnattr_setschedparam(posix_spawnattr_t *restrict a,
                                       const struct sched_param *restrict p)
{
    a->__param = *p;
    return 0;
}

WEAK int posix_spawnattr_getschedparam(const posix_spawnattr_t *restrict a,
                                       struct sched_param *restrict p)
{
    *p = a->__param;
    return 0;
}

/* The scheduling attributes are accepted and not applied: a backend has
 * no primitive for them, and a spawned program run at the default
 * policy is what every caller that asks for SCHED_OTHER wanted. */
WEAK int posix_spawn(pid_t *restrict pid, const char *restrict path,
                     const posix_spawn_file_actions_t *fa,
                     const posix_spawnattr_t *restrict attr,
                     char *const argv[restrict], char *const envp[restrict])
{
    struct __os_spawn_attr k = { 0, 0, 0, 0 };
    if (attr) {
        k.flags = attr->__flags & (POSIX_SPAWN_RESETIDS |
                                   POSIX_SPAWN_SETPGROUP |
                                   POSIX_SPAWN_SETSIGDEF |
                                   POSIX_SPAWN_SETSIGMASK |
                                   POSIX_SPAWN_SETSID);
        k.pgroup = attr->__pgroup;
        k.sigdefault = attr->__sigdefault;
        k.sigmask = attr->__sigmask;
    }
    extern char **environ;
    int saved = errno, child;
    if (__os_spawn(&child, path, fa ? fa->__a : NULL, fa ? fa->__n : 0,
                   attr ? &k : NULL, argv, envp ? envp : environ) < 0) {
        int err = errno;
        errno = saved;
        return err;
    }
    errno = saved;
    if (pid)
        *pid = child;
    return 0;
}

/* exec*p's search, and its rule for a file the kernel cannot run
 * (ENOEXEC): /bin/sh runs it as a script */
WEAK int posix_spawnp(pid_t *restrict pid, const char *restrict file,
                      const posix_spawn_file_actions_t *fa,
                      const posix_spawnattr_t *restrict attr,
                      char *const argv[restrict], char *const envp[restrict])
{
    if (strchr(file, '/'))
        return posix_spawn(pid, file, fa, attr, argv, envp);
    const char *path = getenv("PATH");
    if (!path)
        path = "/usr/local/bin:/bin:/usr/bin";
    size_t fl = strlen(file);
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
            int e = posix_spawn(pid, buf, fa, attr, argv, envp);
            if (e == ENOEXEC) {
                int n = 0;
                while (argv[n])
                    n++;
                char **v = malloc((size_t)(n + 2) * sizeof *v);
                if (!v)
                    return ENOMEM;
                v[0] = (char *)"sh";
                v[1] = buf;
                for (int i = 1; i <= n; i++)
                    v[i + 1] = argv[i];
                if (n == 0)
                    v[2] = NULL;
                e = posix_spawn(pid, _PATH_BSHELL, fa, attr, v, envp);
                free(v);
                return e;
            }
            if (e == EACCES)
                seen_eacces = 1;
            else if (e != ENOENT && e != ENOTDIR)
                return e;
        }
        if (!end)
            break;
        p = end;
    }
    return seen_eacces ? EACCES : ENOENT;
}
