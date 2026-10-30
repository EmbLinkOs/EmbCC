/* What the POSIX layer's sources share.
 *
 * Every public function in src/posix is WEAK. None of POSIX's names is
 * reserved to the implementation the way C's are -- a C program may have
 * its own `read`, `link` or `index` -- and these files hold many
 * functions each, so a program that calls `open` pulls in fd.c's `read`
 * too. Weak, the program's own definition wins rather than colliding.
 */
#ifndef EMBLIBC_POSIX_H
#define EMBLIBC_POSIX_H

#include <limits.h>
#include <string.h>
#include <errno.h>

#include "../../os/backend.h"

#define WEAK __attribute__((weak))

#ifndef AT_FDCWD
#define AT_FDCWD (-100)
#endif

/* The path an *at function means by (dirfd, path): path itself when it
 * is absolute or dirfd is AT_FDCWD, else the directory dirfd is open on
 * with path after it, in buf (PATH_MAX bytes). NULL with errno when the
 * OS cannot say where dirfd is (ENOSYS) or the result is too long. */
static inline const char *__at_path(int dirfd, const char *path, char *buf)
{
    if (!path || path[0] == '/' || dirfd == AT_FDCWD)
        return path;
    if (__os_fdpath(dirfd, buf, PATH_MAX) < 0)
        return 0;
    size_t d = strlen(buf), p = strlen(path);
    if (!*path)
        return buf;
    if (d + 1 + p + 1 > PATH_MAX) {
        errno = ENAMETOOLONG;
        return 0;
    }
    if (d == 0 || buf[d - 1] != '/')
        buf[d++] = '/';
    memcpy(buf + d, path, p + 1);
    return buf;
}

/* Whether a backend's *at primitive answered ENOSYS -- the cue to
 * resolve (dirfd, path) here and call the plain form instead. errno is
 * restored, so the fallback starts clean. */
static inline int __at_absent(int r, int saved)
{
    if (r < 0 && errno == ENOSYS) {
        errno = saved;
        return 1;
    }
    return 0;
}

#endif
