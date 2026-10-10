/* The _P string functions of <avr/pgmspace.h> (lib/avr): avr-libc's, each
 * with one argument in flash, read through EmbCC's __flash (lpm).
 * One object per function (tools/build-rt.sh builds each EMB_AVR_ name on
 * its own), so an image carries only the ones it calls. */
#include <avr/pgmspace.h>
#include <stdint.h>

/* A flash address as the __flash pointer that reads it. */
#define FP(p) ((const __flash char *)(uint16_t)(p))

#if defined(EMB_AVR_STRCASECMP_P) || defined(EMB_AVR_STRNCASECMP_P)
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }
#endif

#ifdef EMB_AVR_MEMCPY_P
void *memcpy_P(void *dst, PGM_VOID_P src, size_t n)
{
    char *d = dst;
    const __flash char *s = FP(src);
    while (n--)
        *d++ = *s++;
    return dst;
}
#endif

#ifdef EMB_AVR_MEMCMP_P
int memcmp_P(const void *s1, PGM_VOID_P s2, size_t n)
{
    const unsigned char *a = s1;
    const __flash unsigned char *b = (const __flash unsigned char *)(uint16_t)s2;
    for (; n; n--, a++, b++)
        if (*a != *b)
            return *a - *b;
    return 0;
}
#endif

#ifdef EMB_AVR_MEMCHR_P
PGM_VOID_P memchr_P(PGM_VOID_P s, int c, size_t n)
{
    const __flash unsigned char *p = (const __flash unsigned char *)(uint16_t)s;
    for (; n; n--, p++)
        if (*p == (unsigned char)c)
            return (PGM_VOID_P)(uint16_t)p;
    return NULL;
}
#endif

#ifdef EMB_AVR_STRLEN_P
size_t strlen_P(PGM_P s)
{
    const __flash char *p = FP(s);
    size_t n = 0;
    while (p[n])
        n++;
    return n;
}
#endif

#ifdef EMB_AVR_STRNLEN_P
size_t strnlen_P(PGM_P s, size_t max)
{
    const __flash char *p = FP(s);
    size_t n = 0;
    while (n < max && p[n])
        n++;
    return n;
}
#endif

#ifdef EMB_AVR_STRCMP_P
int strcmp_P(const char *s1, PGM_P s2)
{
    const __flash char *b = FP(s2);
    for (; *s1 && *s1 == *b; s1++, b++) {
    }
    return (unsigned char)*s1 - (unsigned char)*b;
}
#endif

#ifdef EMB_AVR_STRNCMP_P
int strncmp_P(const char *s1, PGM_P s2, size_t n)
{
    const __flash char *b = FP(s2);
    for (; n; n--, s1++, b++)
        if (*s1 != *b || !*s1)
            return (unsigned char)*s1 - (unsigned char)*b;
    return 0;
}
#endif

#ifdef EMB_AVR_STRCASECMP_P
int strcasecmp_P(const char *s1, PGM_P s2)
{
    const __flash char *b = FP(s2);
    for (;; s1++, b++) {
        int x = lower((unsigned char)*s1), y = lower((unsigned char)*b);
        if (x != y || !x)
            return x - y;
    }
}
#endif

#ifdef EMB_AVR_STRNCASECMP_P
int strncasecmp_P(const char *s1, PGM_P s2, size_t n)
{
    const __flash char *b = FP(s2);
    for (; n; n--, s1++, b++) {
        int x = lower((unsigned char)*s1), y = lower((unsigned char)*b);
        if (x != y || !x)
            return x - y;
    }
    return 0;
}
#endif

#ifdef EMB_AVR_STRCPY_P
char *strcpy_P(char *dst, PGM_P src)
{
    char *d = dst;
    const __flash char *s = FP(src);
    while ((*d++ = *s++) != 0) {
    }
    return dst;
}
#endif

#ifdef EMB_AVR_STRNCPY_P
char *strncpy_P(char *dst, PGM_P src, size_t n)
{
    char *d = dst;
    const __flash char *s = FP(src);
    for (; n && *s; n--)
        *d++ = *s++;
    for (; n; n--)
        *d++ = 0;
    return dst;
}
#endif

#ifdef EMB_AVR_STRCAT_P
char *strcat_P(char *dst, PGM_P src)
{
    char *d = dst;
    const __flash char *s = FP(src);
    while (*d)
        d++;
    while ((*d++ = *s++) != 0) {
    }
    return dst;
}
#endif

#ifdef EMB_AVR_STRNCAT_P
char *strncat_P(char *dst, PGM_P src, size_t n)
{
    char *d = dst;
    const __flash char *s = FP(src);
    while (*d)
        d++;
    for (; n && *s; n--)
        *d++ = *s++;
    *d = 0;
    return dst;
}
#endif

#ifdef EMB_AVR_STRCHR_P
PGM_P strchr_P(PGM_P s, int c)
{
    const __flash char *p = FP(s);
    for (;; p++) {
        if (*p == (char)c)
            return (PGM_P)(uint16_t)p;
        if (!*p)
            return NULL;
    }
}
#endif

#ifdef EMB_AVR_STRRCHR_P
PGM_P strrchr_P(PGM_P s, int c)
{
    const __flash char *p = FP(s);
    PGM_P r = NULL;
    for (;; p++) {
        if (*p == (char)c)
            r = (PGM_P)(uint16_t)p;
        if (!*p)
            return r;
    }
}
#endif

#ifdef EMB_AVR_STRSTR_P
char *strstr_P(const char *s1, PGM_P s2)
{
    const __flash char *n = FP(s2);
    if (!*n)
        return (char *)s1;
    for (; *s1; s1++) {
        const char *a = s1;
        const __flash char *b = n;
        while (*a && *a == *b) {
            a++;
            b++;
        }
        if (!*b)
            return (char *)s1;
    }
    return NULL;
}
#endif
