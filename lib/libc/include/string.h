/* <string.h> — C11 §7.24. */
/* `__restrict`, not `restrict`, throughout these headers: `restrict` is a
 * C keyword and C++ has no such keyword, so the C spelling makes every
 * header unusable from C++ -- and this library has to serve the C++
 * runtime that sits on it. __restrict is the spelling every compiler
 * accepts in both languages. */
#ifndef _STRING_H
#define _STRING_H

#include <stddef.h>
#include <features.h>

#ifdef __cplusplus
extern "C" {
#endif

void *memcpy(void *__restrict d, const void *__restrict s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int   memcmp(const void *a, const void *b, size_t n);
void *memchr(const void *s, int c, size_t n);
/* C23: copy up to and including the first c; and a memset that is done
 * even when nothing reads the memory again -- a key being wiped. */
void *memccpy(void *__restrict d, const void *__restrict s, int c, size_t n);
void *memset_explicit(void *d, int c, size_t n);

char  *strcpy(char *__restrict d, const char *__restrict s);
char  *strncpy(char *__restrict d, const char *__restrict s, size_t n);
char  *strcat(char *__restrict d, const char *__restrict s);
char  *strncat(char *__restrict d, const char *__restrict s, size_t n);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
int    strcoll(const char *a, const char *b);
size_t strxfrm(char *__restrict d, const char *__restrict s, size_t n);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
size_t strcspn(const char *s, const char *rej);
char  *strpbrk(const char *s, const char *acc);
size_t strspn(const char *s, const char *acc);
char  *strstr(const char *h, const char *n);
char  *strtok(char *__restrict s, const char *__restrict sep);
size_t strlen(const char *s);
char  *strerror(int errnum);

/* Widely used, and cheap to provide. */
size_t strnlen(const char *s, size_t n);
char  *strdup(const char *s);
char  *strndup(const char *s, size_t n);
char  *stpcpy(char *__restrict d, const char *__restrict s);
char  *stpncpy(char *__restrict d, const char *__restrict s, size_t n);
void  *mempcpy(void *__restrict d, const void *__restrict s, size_t n);

/* POSIX's, which ISO C leaves to the program (see <features.h>) */
#if defined(__EMB_POSIX)
char  *strtok_r(char *__restrict s, const char *__restrict sep,
                char **__restrict save);
/* XSI's form, returning 0 or an errno -- not GNU's, which returns the
 * message and may ignore the buffer */
int    strerror_r(int errnum, char *buf, size_t n);
char  *strsep(char **sp, const char *sep);
size_t strlcpy(char *__restrict d, const char *__restrict s, size_t n);
size_t strlcat(char *__restrict d, const char *__restrict s, size_t n);
void  *memmem(const void *hay, size_t hn, const void *needle, size_t nn);
#endif
#if defined(__EMB_GNU)
char  *strcasestr(const char *h, const char *n);
void  *memrchr(const void *s, int c, size_t n);
char  *strchrnul(const char *s, int c);
void   explicit_bzero(void *s, size_t n);
#endif

#ifdef __cplusplus
}
#endif

#endif
