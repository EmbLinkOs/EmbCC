/* mkstemp and its family, mkdtemp and mktemp: a file or directory made
 * under a name the caller shapes, "prefixXXXXXX", the X's replaced.
 *
 * mkstemp's whole value is that it creates the file in the same step that
 * picks the name -- O_EXCL, so the open fails if the name was taken in
 * between -- and tries another name when it was. mktemp only names, which
 * leaves the race to its caller; it is here because programs still call
 * it, and POSIX's last edition to list it says so too.
 */
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "../../os/backend.h"
#include "../internal/tmpname.h"

unsigned long long __tmp_bits(void)
{
    static unsigned long count;
    unsigned long long r;
    int saved = errno;
    if (__os_getentropy(&r, sizeof r) != 0)
        r = (unsigned long long)__os_clock_ns() ^
            (unsigned long long)__os_time() << 32;
    errno = saved;
    return r + (unsigned long long)++count * 0x9E3779B97F4A7C15ull;
}

/* The six X's before a suffix of `suffix` bytes, or NULL with EINVAL:
 * fewer than six is a template the caller got wrong, and filling what
 * there is would quietly make guessing the name easy. */
static char *xs(char *t, int suffix)
{
    size_t n = strlen(t);
    if (suffix < 0 || n < (size_t)suffix + 6 ||
        memcmp(t + n - suffix - 6, "XXXXXX", 6) != 0) {
        errno = EINVAL;
        return NULL;
    }
    return t + n - suffix - 6;
}

static void fill(char *x)
{
    static const char abc[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    unsigned long long r = __tmp_bits();
    for (int i = 0; i < 6; i++, r /= 62)
        x[i] = abc[r % 62];
}

/* As many tries as glibc makes. A collision is a name some other file
 * already has; this many in a row is a directory full of them, or a
 * failure that is not EEXIST and has already been returned. */
#define TRIES (62 * 62 * 62)

__attribute__((weak)) int mkostemps(char *t, int suffix, int flags)
{
    char *x = xs(t, suffix);
    if (!x)
        return -1;
    flags &= ~(__OS_O_RDONLY | __OS_O_WRONLY | __OS_O_RDWR);
    for (int i = 0; i < TRIES; i++) {
        fill(x);
        int fd = __os_open(t, flags | __OS_O_RDWR | __OS_O_CREAT | __OS_O_EXCL,
                           0600);
        if (fd >= 0 || errno != EEXIST)
            return fd;
    }
    memcpy(x, "XXXXXX", 6);
    return -1;
}

__attribute__((weak)) int mkstemps(char *t, int suffix)  { return mkostemps(t, suffix, 0); }
__attribute__((weak)) int mkostemp(char *t, int flags)   { return mkostemps(t, 0, flags); }
__attribute__((weak)) int mkstemp(char *t)               { return mkostemps(t, 0, 0); }

__attribute__((weak)) char *mkdtemp(char *t)
{
    char *x = xs(t, 0);
    if (!x)
        return NULL;
    for (int i = 0; i < TRIES; i++) {
        fill(x);
        if (__os_mkdir(t, 0700) == 0)
            return t;
        if (errno != EEXIST)
            return NULL;
    }
    memcpy(x, "XXXXXX", 6);
    return NULL;
}

/* A name nothing has: free if lstat finds nothing there. On failure the
 * template becomes the empty string, which is how mktemp reports one. */
__attribute__((weak)) char *mktemp(char *t)
{
    char *x = xs(t, 0);
    if (!x) {
        if (t)
            *t = 0;
        return t;
    }
    int saved = errno;
    for (int i = 0; i < TRIES; i++) {
        struct __os_fileinfo fi;
        fill(x);
        if (__os_lstat(t, &fi) == 0)
            continue;              /* taken */
        if (errno == ENOENT) {
            errno = saved;
            return t;
        }
        break;                     /* cannot tell, so no name is safe */
    }
    *t = 0;
    return t;
}
