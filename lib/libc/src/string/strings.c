/* POSIX's string functions beyond C's: stpcpy, stpncpy and GNU's mempcpy
 * (<string.h>), and <strings.h>'s bzero, bcopy, bcmp, index, rindex,
 * strcasecmp and strncasecmp. GCC's __builtin_ forms of them become calls
 * here.
 *
 * Each is weak: these names are not reserved to the implementation the
 * way C's are, and a program that defines its own `index` (a common
 * global's name) must keep it rather than collide with this one. */
#include <string.h>
#include <strings.h>
#include <ctype.h>

__attribute__((weak)) char *stpcpy(char *restrict d, const char *restrict s)
{
    while ((*d = *s++))
        d++;
    return d;
}

/* n bytes are written: s up to its NUL, then NULs; the result points at
 * the first NUL written, or at d + n when s filled them all */
__attribute__((weak)) char *stpncpy(char *restrict d, const char *restrict s,
                                    size_t n)
{
    size_t k = strnlen(s, n);
    memcpy(d, s, k);
    memset(d + k, 0, n - k);
    return d + k;
}

__attribute__((weak)) void *mempcpy(void *restrict d, const void *restrict s,
                                    size_t n)
{
    return (char *)memcpy(d, s, n) + n;
}

__attribute__((weak)) void bzero(void *s, size_t n)
{
    memset(s, 0, n);
}

__attribute__((weak)) void bcopy(const void *s, void *d, size_t n)
{
    memmove(d, s, n);
}

__attribute__((weak)) int bcmp(const void *a, const void *b, size_t n)
{
    return memcmp(a, b, n);
}

__attribute__((weak)) char *index(const char *s, int c)
{
    return strchr(s, c);
}

__attribute__((weak)) char *rindex(const char *s, int c)
{
    return strrchr(s, c);
}

__attribute__((weak)) int strcasecmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
        if (x != y || !x)
            return x - y;
    }
}

__attribute__((weak)) int strncasecmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
        if (x != y || !x)
            return x - y;
    }
    return 0;
}

/* the 1-based index of the lowest set bit, 0 for none */
__attribute__((weak)) int ffs(int i)
{
    return __builtin_ffs(i);
}
