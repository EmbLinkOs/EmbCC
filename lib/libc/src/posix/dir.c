/* <dirent.h>: a DIR is the backend's handle (__os_opendir) and the one
 * entry readdir last returned.
 *
 * A position is the count of entries read since the start, which every
 * backend can honour: seekdir rewinds and reads that many again. A
 * kernel offset (Linux's d_off) would be cheaper, and is not something
 * every OS has to give. */
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "posix.h"

struct __dirstream {
    void *h;
    long pos;
    struct dirent ent;
};

static const unsigned char dt[] = {
    [__OS_FT_UNKNOWN] = DT_UNKNOWN, [__OS_FT_REGULAR] = DT_REG,
    [__OS_FT_DIRECTORY] = DT_DIR, [__OS_FT_SYMLINK] = DT_LNK,
    [__OS_FT_CHARDEV] = DT_CHR, [__OS_FT_BLOCKDEV] = DT_BLK,
    [__OS_FT_FIFO] = DT_FIFO, [__OS_FT_SOCKET] = DT_SOCK,
};

static DIR *wrap(void *h)
{
    if (!h)
        return NULL;
    DIR *d = calloc(1, sizeof *d);
    if (!d) {
        __os_closedir(h);
        errno = ENOMEM;
        return NULL;
    }
    d->h = h;
    return d;
}

WEAK DIR *opendir(const char *path) { return wrap(__os_opendir(path)); }
WEAK DIR *fdopendir(int fd) { return wrap(__os_fdopendir(fd)); }

/* errno is left as it was at the end of the directory: that is how a
 * caller tells the end from an error, and POSIX requires it */
WEAK struct dirent *readdir(DIR *d)
{
    struct __os_dirent e;
    int r = __os_readdir_entry(d->h, &e);
    if (r <= 0)
        return NULL;
    d->ent.d_ino = (ino_t)e.ino;
    d->ent.d_off = ++d->pos;
    d->ent.d_reclen = sizeof d->ent;
    d->ent.d_type = e.type >= 0 && e.type < (int)sizeof dt ? dt[e.type]
                                                           : DT_UNKNOWN;
    memcpy(d->ent.d_name, e.name, sizeof d->ent.d_name);
    return &d->ent;
}

WEAK int readdir_r(DIR *restrict d, struct dirent *restrict out,
                   struct dirent **restrict result)
{
    int saved = errno;
    errno = 0;
    struct dirent *e = readdir(d);
    int err = errno;
    errno = saved;
    if (!e) {
        *result = NULL;
        return err;
    }
    *out = *e;
    *result = out;
    return 0;
}

WEAK void rewinddir(DIR *d)
{
    __os_rewinddir(d->h);
    d->pos = 0;
}

WEAK long telldir(DIR *d) { return d->pos; }

WEAK void seekdir(DIR *d, long at)
{
    rewinddir(d);
    while (d->pos < at && readdir(d))
        ;
}

WEAK int closedir(DIR *d)
{
    if (!d) {
        errno = EBADF;
        return -1;
    }
    __os_closedir(d->h);
    free(d);
    return 0;
}

WEAK int dirfd(DIR *d) { return __os_dirfd(d->h); }

WEAK int alphasort(const struct dirent **a, const struct dirent **b)
{
    return strcoll((*a)->d_name, (*b)->d_name);
}

WEAK int versionsort(const struct dirent **a, const struct dirent **b)
{
    return strverscmp((*a)->d_name, (*b)->d_name);
}

/* the caller's comparison, which takes struct dirent ** -- called with
 * its own type, not through a cast of its pointer */
struct by { int (*cmp)(const struct dirent **, const struct dirent **); };

static int by_cmp(const void *a, const void *b, void *c)
{
    return ((struct by *)c)->cmp((const struct dirent **)a,
                                 (const struct dirent **)b);
}

/* The entries keep() accepts, each malloc'd, in a malloc'd array sorted
 * by cmp; the count, or -1 with errno and nothing left allocated. */
WEAK int scandir(const char *path, struct dirent ***list,
                 int (*keep)(const struct dirent *),
                 int (*cmp)(const struct dirent **, const struct dirent **))
{
    DIR *d = opendir(path);
    if (!d)
        return -1;
    struct dirent **v = NULL, *e;
    size_t n = 0, cap = 0;
    int saved = errno;
    errno = 0;
    while ((e = readdir(d))) {
        if (keep && !keep(e))
            continue;
        if (n == cap) {
            size_t nc = cap ? 2 * cap : 16;
            struct dirent **nv = realloc(v, nc * sizeof *nv);
            if (!nv)
                goto oom;
            v = nv;
            cap = nc;
        }
        if (!(v[n] = malloc(sizeof *e)))
            goto oom;
        memcpy(v[n++], e, sizeof *e);
    }
    if (errno) {
        int err = errno;
        while (n)
            free(v[--n]);
        free(v);
        closedir(d);
        errno = err;
        return -1;
    }
    closedir(d);
    errno = saved;
    if (cmp && n > 1) {
        struct by c = { cmp };
        qsort_r(v, n, sizeof *v, by_cmp, &c);
    }
    *list = v;
    return (int)n;
oom:
    while (n)
        free(v[--n]);
    free(v);
    closedir(d);
    errno = ENOMEM;
    return -1;
}
