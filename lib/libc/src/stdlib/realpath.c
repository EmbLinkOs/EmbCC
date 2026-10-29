/* realpath: the absolute name of a file with every ".", ".." and symbolic
 * link taken out of it.
 *
 * One component at a time, left to right, with each looked at (lstat)
 * before the next is added. The order is the point: "a/../b" is NOT "b"
 * when a is a symlink -- ".." leaves whatever a points at -- so ".." may
 * only be applied to a prefix that has already been resolved, and that
 * prefix is then free of links and ".." can simply drop its last
 * component. A link's target is put in front of what remains of the
 * path and resolved like the rest of it; forty of them in one walk is
 * Linux's ELOOP, and the answer here too.
 */
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>

#include "../../os/backend.h"

#define MAX_LINKS 40

static int too_long(void)
{
    errno = ENAMETOOLONG;
    return -1;
}

/* out, of olen bytes, without its last component. "/" stays "/". */
static size_t parent(const char *out, size_t olen)
{
    while (olen > 1 && out[olen - 1] != '/')
        olen--;
    return olen > 1 ? olen - 1 : 1;
}

/* rest and link are the caller's, PATH_MAX bytes each: on the heap, since
 * three path buffers are 12 KB, which is a whole stack on a small target */
static int resolve(const char *path, char *out, char *rest, char *link)
{
    size_t olen, links = 0;

    if (strlen(path) >= PATH_MAX)
        return too_long();
    strcpy(rest, path);
    if (rest[0] == '/') {
        out[0] = '/';
        olen = 1;
    } else {
        if (__os_getcwd(out, PATH_MAX) < 0)
            return -1;
        olen = strlen(out);
    }

    char *p = rest;
    for (;;) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        char *end = p;
        while (*end && *end != '/')
            end++;
        size_t n = (size_t)(end - p);
        if (n == 1 && p[0] == '.') {
            p = end;
            continue;
        }
        if (n == 2 && p[0] == '.' && p[1] == '.') {
            olen = parent(out, olen);
            p = end;
            continue;
        }
        size_t before = olen;
        if (olen + (olen > 1) + n >= PATH_MAX)
            return too_long();
        if (olen > 1)
            out[olen++] = '/';
        memcpy(out + olen, p, n);
        olen += n;
        out[olen] = 0;
        p = end;

        struct __os_fileinfo fi;
        if (__os_lstat(out, &fi) < 0)
            return -1;
        if (fi.type == __OS_FT_SYMLINK) {
            if (++links > MAX_LINKS) {
                errno = ELOOP;
                return -1;
            }
            long k = __os_readlink(out, link, PATH_MAX - 1);
            if (k < 0)
                return -1;
            /* the target, then what was left after the link's name */
            size_t left = strlen(p);
            if ((size_t)k + left + 1 >= PATH_MAX)
                return too_long();
            memmove(rest + k, p, left + 1);
            memcpy(rest, link, (size_t)k);
            p = rest;
            if (link[0] == '/')
                olen = 1;
            else
                olen = before;
            out[olen] = 0;
            continue;
        }
        /* Anything after this must be inside it -- including a lone
         * trailing slash, which names "the directory a" and is an error
         * when a is a file. */
        if (*p && fi.type != __OS_FT_DIRECTORY) {
            errno = ENOTDIR;
            return -1;
        }
    }
    out[olen] = 0;
    return 0;
}

char *realpath(const char *restrict path, char *restrict resolved)
{
    if (!path) {
        errno = EINVAL;
        return NULL;
    }
    if (!*path) {
        errno = ENOENT;
        return NULL;
    }
    char *work = malloc(3 * PATH_MAX);
    if (!work)
        return NULL;
    char *out = work, *r = NULL;
    if (resolve(path, out, work + PATH_MAX, work + 2 * PATH_MAX) == 0) {
        r = resolved ? resolved : malloc(strlen(out) + 1);
        if (r)
            strcpy(r, out);
    }
    free(work);
    return r;
}
