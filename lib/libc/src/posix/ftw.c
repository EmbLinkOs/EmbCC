/* <ftw.h>: depth first, one directory open at a time per level -- the
 * descriptor budget (`fds`) is a limit POSIX lets an implementation keep
 * by closing and reopening, and this one reads each directory whole
 * before descending, so it never holds more than one open. The order of
 * a directory's entries is readdir's, as glibc's is. */
#define _GNU_SOURCE 1
#include <ftw.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "posix.h"

typedef int (*nftw_fn)(const char *, const struct stat *, int, struct FTW *);

struct walk {
    nftw_fn fn;
    int (*old)(const char *, const struct stat *, int);   /* ftw's */
    int flags;
    dev_t dev;
    char path[PATH_MAX];
};

#define STOPPED(r) ((w->flags & FTW_ACTIONRETVAL) ? (r) == FTW_STOP : (r) != 0)

static int visit(struct walk *w, size_t len, int base, int level);

static int call(struct walk *w, const struct stat *st, int type,
                struct FTW *f)
{
    return w->fn ? w->fn(w->path, st, type, f) : w->old(w->path, st, type);
}

/* the entries of the directory in w->path, read and closed first */
static char **list(const char *dir, int *err)
{
    DIR *d = opendir(dir);
    if (!d) {
        *err = errno;
        return NULL;
    }
    size_t n = 0, cap = 16;
    char **v = malloc(cap * sizeof *v);
    while (v) {
        struct dirent *e = readdir(d);
        if (!e)
            break;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        if (n + 1 == cap) {
            char **nv = realloc(v, (cap *= 2) * sizeof *v);
            if (!nv) {
                for (size_t i = 0; i < n; i++)
                    free(v[i]);
                free(v);
                v = NULL;
                break;
            }
            v = nv;
        }
        if (!(v[n] = strdup(e->d_name))) {
            for (size_t i = 0; i < n; i++)
                free(v[i]);
            free(v);
            v = NULL;
            break;
        }
        n++;
    }
    closedir(d);
    if (!v) {
        *err = ENOMEM;
        return NULL;
    }
    v[n] = NULL;
    return v;
}

static int visit(struct walk *w, size_t len, int base, int level)
{
    struct stat st;
    struct FTW f = { base, level };
    int type, r;
    int (*st_fn)(const char *, struct stat *) =
        (w->flags & FTW_PHYS) ? lstat : stat;
    if (st_fn(w->path, &st) != 0) {
        if (level == 0)
            return -1;         /* the start itself: nftw's error, errno set */
        /* a link whose target is gone, when links are followed */
        if (!(w->flags & FTW_PHYS) && lstat(w->path, &st) == 0 &&
            S_ISLNK(st.st_mode))
            type = FTW_SLN;
        else if (errno == EACCES || errno == ENOENT || errno == ENOTDIR ||
                 errno == ELOOP)
            type = FTW_NS;
        else
            return -1;
    } else if (S_ISDIR(st.st_mode)) {
        type = (w->flags & FTW_DEPTH) ? FTW_DP : FTW_D;
    } else if (S_ISLNK(st.st_mode)) {
        type = FTW_SL;
    } else {
        type = FTW_F;
    }
    if (type == FTW_NS)
        memset(&st, 0, sizeof st);
    if (level == 0)
        w->dev = st.st_dev;
    else if ((w->flags & FTW_MOUNT) && type != FTW_NS && st.st_dev != w->dev)
        return 0;

    int is_dir = type == FTW_D || type == FTW_DP;
    char **names = NULL;
    if (is_dir) {
        int err = 0;
        names = list(w->path, &err);
        if (!names) {
            if (err == ENOMEM)
                return -1;
            type = FTW_DNR;
            is_dir = 0;
        }
    }
    if (type != FTW_DP) {
        r = call(w, &st, type, &f);
        if (STOPPED(r) || ((w->flags & FTW_ACTIONRETVAL) &&
                           r == FTW_SKIP_SIBLINGS)) {
            if (names) {
                for (char **p = names; *p; p++)
                    free(*p);
                free(names);
            }
            return r;
        }
        if ((w->flags & FTW_ACTIONRETVAL) && r == FTW_SKIP_SUBTREE)
            is_dir = 0;
    }
    if (names) {
        r = 0;
        int cwd = -1;
        if (is_dir && (w->flags & FTW_CHDIR)) {
            cwd = open(".", O_RDONLY | O_DIRECTORY);
            /* below the start, cwd is the parent: in by the name */
            if (chdir(level ? w->path + base : w->path) != 0) {
                if (cwd >= 0)
                    close(cwd);
                cwd = -2;
            }
        }
        for (char **p = names; is_dir && *p && cwd != -2; p++) {
            size_t nl = strlen(*p);
            size_t add = (len && w->path[len - 1] != '/') ? 1 : 0;
            if (len + add + nl >= sizeof w->path) {
                errno = ENAMETOOLONG;
                r = -1;
                break;
            }
            if (add)
                w->path[len] = '/';
            memcpy(w->path + len + add, *p, nl + 1);
            r = visit(w, len + add + nl, (int)(len + add), level + 1);
            w->path[len] = 0;
            if ((w->flags & FTW_ACTIONRETVAL) && r == FTW_SKIP_SIBLINGS) {
                r = 0;
                break;
            }
            if (STOPPED(r) || r < 0)
                break;
        }
        if (cwd >= 0) {
            (void)!fchdir(cwd);
            close(cwd);
        }
        for (char **p = names; *p; p++)
            free(*p);
        free(names);
        if (STOPPED(r) || r < 0)
            return r;
        if (type == FTW_DP) {
            r = call(w, &st, FTW_DP, &f);
            if ((w->flags & FTW_ACTIONRETVAL) && r != FTW_STOP)
                r = 0;
            return r;
        }
    }
    return 0;
}

WEAK int nftw(const char *path, nftw_fn fn, int fds, int flags)
{
    if (fds < 1) {
        errno = EINVAL;
        return -1;
    }
    struct walk w;
    w.fn = fn;
    w.old = NULL;
    w.flags = flags;
    size_t len = strlen(path);
    if (len >= sizeof w.path) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(w.path, path, len + 1);
    /* where the last component starts, past any trailing slashes */
    size_t e = len;
    while (e > 1 && w.path[e - 1] == '/')
        e--;
    size_t b = e;
    while (b > 0 && w.path[b - 1] != '/')
        b--;
    int r = visit(&w, len, (int)b, 0);
    if ((flags & FTW_ACTIONRETVAL) && r > 0 && r != FTW_STOP)
        r = 0;
    return r;
}

/* ftw: nftw following links, with a callback that takes no struct FTW
 * -- the walk carries either kind, so neither needs the other's shape */
WEAK int ftw(const char *path,
             int (*fn)(const char *, const struct stat *, int), int fds)
{
    if (fds < 1) {
        errno = EINVAL;
        return -1;
    }
    struct walk w;
    w.fn = NULL;
    w.old = fn;
    w.flags = 0;
    size_t len = strlen(path);
    if (len >= sizeof w.path) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(w.path, path, len + 1);
    return visit(&w, len, 0, 0);
}
