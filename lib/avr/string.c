/* The string and memory functions of the AVR library (lib/avr), as an
 * avr-libc program calls them.
 *
 * Not lib/libc/src/string/string.c: that object also holds strdup, so
 * linking memcpy from it pulls in malloc and the C library's heap, and
 * every function here is one object of its own so that an image carries
 * only the ones it calls -- on a part with 32 KB of flash. */
#include <stddef.h>
#include <string.h>

#ifdef EMB_AVR_MEMCPY
void *memcpy(void *__restrict d, const void *__restrict s, size_t n)
{
    unsigned char *dp = d;
    const unsigned char *sp = s;
    while (n--)
        *dp++ = *sp++;
    return d;
}
#endif

#ifdef EMB_AVR_MEMMOVE
void *memmove(void *d, const void *s, size_t n)
{
    unsigned char *dp = d;
    const unsigned char *sp = s;
    if (dp < sp) {
        while (n--)
            *dp++ = *sp++;
    } else {
        dp += n;
        sp += n;
        while (n--)
            *--dp = *--sp;
    }
    return d;
}
#endif

#ifdef EMB_AVR_MEMSET
void *memset(void *d, int c, size_t n)
{
    unsigned char *dp = d;
    while (n--)
        *dp++ = (unsigned char)c;
    return d;
}
#endif

#ifdef EMB_AVR_MEMCMP
int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *p = a, *q = b;
    for (; n; n--, p++, q++)
        if (*p != *q)
            return *p - *q;
    return 0;
}
#endif

#ifdef EMB_AVR_MEMCHR
void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;
    for (; n; n--, p++)
        if (*p == (unsigned char)c)
            return (void *)p;
    return NULL;
}
#endif

#ifdef EMB_AVR_STRLEN
size_t strlen(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    return (size_t)(p - s);
}
#endif

#ifdef EMB_AVR_STRNLEN
size_t strnlen(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n])
        n++;
    return n;
}
#endif

#ifdef EMB_AVR_STRCPY
char *strcpy(char *__restrict d, const char *__restrict s)
{
    char *r = d;
    while ((*d++ = *s++) != 0) {
    }
    return r;
}
#endif

#ifdef EMB_AVR_STRNCPY
char *strncpy(char *__restrict d, const char *__restrict s, size_t n)
{
    char *r = d;
    for (; n && *s; n--)
        *d++ = *s++;
    for (; n; n--)
        *d++ = 0;
    return r;
}
#endif

#ifdef EMB_AVR_STRCAT
char *strcat(char *__restrict d, const char *__restrict s)
{
    char *r = d;
    while (*d)
        d++;
    while ((*d++ = *s++) != 0) {
    }
    return r;
}
#endif

#ifdef EMB_AVR_STRNCAT
char *strncat(char *__restrict d, const char *__restrict s, size_t n)
{
    char *r = d;
    while (*d)
        d++;
    for (; n && *s; n--)
        *d++ = *s++;
    *d = 0;
    return r;
}
#endif

#ifdef EMB_AVR_STRCMP
int strcmp(const char *a, const char *b)
{
    for (; *a && *a == *b; a++, b++) {
    }
    return (unsigned char)*a - (unsigned char)*b;
}
#endif

#ifdef EMB_AVR_STRNCMP
int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++)
        if (*a != *b || !*a)
            return (unsigned char)*a - (unsigned char)*b;
    return 0;
}
#endif

#ifdef EMB_AVR_STRCHR
char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c)
            return (char *)s;
        if (!*s)
            return NULL;
    }
}
#endif

#ifdef EMB_AVR_STRRCHR
char *strrchr(const char *s, int c)
{
    const char *r = NULL;
    for (;; s++) {
        if (*s == (char)c)
            r = s;
        if (!*s)
            return (char *)r;
    }
}
#endif

#ifdef EMB_AVR_STRSTR
char *strstr(const char *h, const char *n)
{
    if (!*n)
        return (char *)h;
    for (; *h; h++) {
        const char *a = h, *b = n;
        while (*a && *a == *b) {
            a++;
            b++;
        }
        if (!*b)
            return (char *)h;
    }
    return NULL;
}
#endif

#ifdef EMB_AVR_STRSPN
size_t strspn(const char *s, const char *acc)
{
    size_t n = 0;
    while (s[n] && strchr(acc, s[n]))
        n++;
    return n;
}
#endif

#ifdef EMB_AVR_STRCSPN
size_t strcspn(const char *s, const char *rej)
{
    size_t n = 0;
    while (s[n] && !strchr(rej, s[n]))
        n++;
    return n;
}
#endif

#ifdef EMB_AVR_STRPBRK
char *strpbrk(const char *s, const char *acc)
{
    for (; *s; s++)
        if (strchr(acc, *s))
            return (char *)s;
    return NULL;
}
#endif
