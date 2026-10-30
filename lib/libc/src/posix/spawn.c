/* <spawn.h>: a list of file actions and a set of attributes, handed whole
 * to __os_spawn, which makes the process -- by fork and exec on Linux,
 * natively on EmbLinkOS. These functions return an error number, as
 * POSIX's spawn functions do, rather than setting errno.
 *
 * The two types are handles: init allocates, destroy frees and sets the
 * handle back to NULL, and a NULL handle -- or a pointer to one -- is
 * EINVAL rather than a crash. */
#include <spawn.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <paths.h>

#include "posix.h"

struct __spawn_actions {
    int n, cap;
    struct __os_spawn_action *a;
};

struct __spawn_attr {
    short flags;
    pid_t pgroup;
    sigset_t sigdefault, sigmask;
    int policy;
    struct sched_param param;
};

WEAK int posix_spawn_file_actions_init(posix_spawn_file_actions_t *fa)
{
    if (!fa)
        return EINVAL;
    *fa = calloc(1, sizeof **fa);
    return *fa ? 0 : ENOMEM;
}

WEAK int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *fa)
{
    if (!fa || !*fa)
        return EINVAL;
    for (int i = 0; i < (*fa)->n; i++)
        free((char *)(*fa)->a[i].path);
    free((*fa)->a);
    free(*fa);
    *fa = NULL;
    return 0;
}

static int add(posix_spawn_file_actions_t *fa, int kind, int fd, int newfd,
               const char *path, int oflag, mode_t mode)
{
    if (!fa || !*fa)
        return EINVAL;
    /* POSIX's EBADF: negative, or at or past OPEN_MAX */
    long max = sysconf(_SC_OPEN_MAX);
    if (fd < 0 || (max > 0 && fd >= max) ||
        (kind == __OS_SPAWN_DUP2 && (newfd < 0 || (max > 0 && newfd >= max))))
        return EBADF;
    struct __spawn_actions *s = *fa;
    struct __os_spawn_action probe = { kind, fd, newfd, oflag, mode, path };
    int no = __os_spawn_check(&probe, s->n);
    if (no)
        return no;
    if (s->n == s->cap) {
        int cap = s->cap ? 2 * s->cap : 8;
        struct __os_spawn_action *a = realloc(s->a, (size_t)cap * sizeof *a);
        if (!a)
            return ENOMEM;
        s->a = a;
        s->cap = cap;
    }
    char *copy = NULL;
    if (path && !(copy = strdup(path)))
        return ENOMEM;
    struct __os_spawn_action *a = &s->a[s->n++];
    a->kind = kind;
    a->fd = fd;
    a->newfd = newfd;
    a->oflag = oflag;
    a->mode = mode;
    a->path = copy;
    return 0;
}

WEAK int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *restrict fa,
                                          int fd, const char *restrict path,
                                          int oflag, mode_t mode)
{
    if (!path || (oflag & O_ACCMODE) == O_ACCMODE)
        return EINVAL;
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
    if (!path)
        return EINVAL;
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
    if (!a)
        return EINVAL;
    *a = calloc(1, sizeof **a);
    return *a ? 0 : ENOMEM;
}

WEAK int posix_spawnattr_destroy(posix_spawnattr_t *a)
{
    if (!a || !*a)
        return EINVAL;
    free(*a);
    *a = NULL;
    return 0;
}

#define ALL_FLAGS (POSIX_SPAWN_RESETIDS | POSIX_SPAWN_SETPGROUP | \
                   POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK | \
                   POSIX_SPAWN_SETSCHEDPARAM | POSIX_SPAWN_SETSCHEDULER | \
                   POSIX_SPAWN_USEVFORK | POSIX_SPAWN_SETSID)

/* An unknown flag is EINVAL. A known one this OS's spawn cannot honour is
 * ENOTSUP, now, rather than a spawn that later ignores it -- including
 * the scheduling ones, which no backend applies. USEVFORK is only a hint
 * about how, and is always accepted. */
WEAK int posix_spawnattr_setflags(posix_spawnattr_t *a, short flags)
{
    if (!a || !*a || (flags & ~ALL_FLAGS))
        return EINVAL;
    int honoured = __os_spawn_flags() | POSIX_SPAWN_USEVFORK;
    if (flags & ~honoured)
        return ENOTSUP;
    (*a)->flags = flags;
    return 0;
}

#define ATTR_GET(name, type, field) \
    WEAK int posix_spawnattr_get##name(const posix_spawnattr_t *restrict a, \
                                       type *restrict out) \
    { \
        if (!a || !*a || !out) \
            return EINVAL; \
        *out = (*a)->field; \
        return 0; \
    }

ATTR_GET(flags, short, flags)
ATTR_GET(pgroup, pid_t, pgroup)
ATTR_GET(sigdefault, sigset_t, sigdefault)
ATTR_GET(sigmask, sigset_t, sigmask)
ATTR_GET(schedpolicy, int, policy)
ATTR_GET(schedparam, struct sched_param, param)

WEAK int posix_spawnattr_setpgroup(posix_spawnattr_t *a, pid_t g)
{
    if (!a || !*a)
        return EINVAL;
    (*a)->pgroup = g;
    return 0;
}

WEAK int posix_spawnattr_setschedpolicy(posix_spawnattr_t *a, int p)
{
    if (!a || !*a)
        return EINVAL;
    (*a)->policy = p;
    return 0;
}

WEAK int posix_spawnattr_setsigdefault(posix_spawnattr_t *restrict a,
                                       const sigset_t *restrict s)
{
    if (!a || !*a || !s)
        return EINVAL;
    (*a)->sigdefault = *s;
    return 0;
}

WEAK int posix_spawnattr_setsigmask(posix_spawnattr_t *restrict a,
                                    const sigset_t *restrict s)
{
    if (!a || !*a || !s)
        return EINVAL;
    (*a)->sigmask = *s;
    return 0;
}

WEAK int posix_spawnattr_setschedparam(posix_spawnattr_t *restrict a,
                                       const struct sched_param *restrict p)
{
    if (!a || !*a || !p)
        return EINVAL;
    (*a)->param = *p;
    return 0;
}

/* The environment is passed as given -- NULL included, which the child
 * receives as no environment at all (EmbLinkOS's rule, and Linux's execve
 * reads it the same way). The caller's errno is left as it was. */
WEAK int posix_spawn(pid_t *restrict pid, const char *restrict path,
                     const posix_spawn_file_actions_t *fa,
                     const posix_spawnattr_t *restrict attr,
                     char *const argv[restrict], char *const envp[restrict])
{
    if (!path || (fa && !*fa) || (attr && !*attr))
        return EINVAL;
    if (!*path)
        return ENOENT;
    struct __os_spawn_attr k = { 0, 0, 0, 0 };
    if (attr) {
        k.flags = (*attr)->flags & (POSIX_SPAWN_RESETIDS |
                                    POSIX_SPAWN_SETPGROUP |
                                    POSIX_SPAWN_SETSIGDEF |
                                    POSIX_SPAWN_SETSIGMASK |
                                    POSIX_SPAWN_SETSID);
        k.pgroup = (*attr)->pgroup;
        k.sigdefault = (*attr)->sigdefault;
        k.sigmask = (*attr)->sigmask;
    }
    int saved = errno, child;
    if (__os_spawn(&child, path, fa ? (*fa)->a : NULL, fa ? (*fa)->n : 0,
                   attr ? &k : NULL, argv, envp) < 0) {
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
 * (ENOEXEC): /bin/sh runs it as a script. With no PATH, the system's
 * program directories -- EmbLinkOS's /system/bin among them. */
WEAK int posix_spawnp(pid_t *restrict pid, const char *restrict file,
                      const posix_spawn_file_actions_t *fa,
                      const posix_spawnattr_t *restrict attr,
                      char *const argv[restrict], char *const envp[restrict])
{
    if (!file)
        return EINVAL;
    if (strchr(file, '/'))
        return posix_spawn(pid, file, fa, attr, argv, envp);
    const char *path = getenv("PATH");
    if (!path)
        path = "/system/bin:/usr/local/bin:/bin:/usr/bin";
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
            if (e == 0)
                return 0;
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
