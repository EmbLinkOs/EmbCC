/* <stdlib.h>'s POSIX functions that are a few lines over C's: the
 * allocation forms and getsubopt. The larger ones have files of their
 * own -- env.c, random.c, tempfile.c, realpath.c. */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>

/* aligned_alloc with POSIX's rules and POSIX's way of failing: an
 * alignment must be a power of two AND a multiple of sizeof(void *),
 * and the error is RETURNED, with errno left alone. */
int posix_memalign(void **p, size_t align, size_t n)
{
    if (align < sizeof(void *) || (align & (align - 1)) ||
        align % sizeof(void *))
        return EINVAL;
    int saved = errno;
    void *q = aligned_alloc(align, n);
    int e = errno;
    errno = saved;
    if (!q)
        return e == EINVAL ? EINVAL : ENOMEM;
    *p = q;
    return 0;
}

/* the old name, which code from before C11 still calls */
void *memalign(size_t align, size_t n) { return aligned_alloc(align, n); }

/* realloc of n things of `size` bytes, refusing a count whose product
 * does not fit rather than allocating its wrapped-around remainder */
void *reallocarray(void *p, size_t n, size_t size)
{
    if (size && n > SIZE_MAX / size) {
        errno = ENOMEM;
        return NULL;
    }
    return realloc(p, n * size);
}

/* One "name" or "name=value" from a comma-separated list, as mount(8)
 * options are written: the index of name in tokens, or -1 when it is not
 * there, with *value at what followed '=' (or NULL), and *opt moved past
 * the comma. An unknown suboption is still consumed and *value is then
 * the whole of it, which is what a caller reports. */
int getsubopt(char **opt, char *const *tokens, char **value)
{
    char *s = *opt, *end = strchr(s, ',');
    if (end) {
        *end = 0;
        *opt = end + 1;
    } else
        *opt = s + strlen(s);
    char *eq = strchr(s, '=');
    size_t n = eq ? (size_t)(eq - s) : strlen(s);
    for (int i = 0; tokens[i]; i++)
        if (strncmp(tokens[i], s, n) == 0 && tokens[i][n] == 0) {
            *value = eq ? eq + 1 : NULL;
            return i;
        }
    *value = s;
    return -1;
}
