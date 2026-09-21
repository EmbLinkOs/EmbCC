/* <string.h> — C11 §7.24. */
/* `__restrict`, not `restrict`, throughout these headers: `restrict` is a
 * C keyword and C++ has no such keyword, so the C spelling makes every
 * header unusable from C++ -- and this library has to serve the C++
 * runtime that sits on it. __restrict is the spelling every compiler
 * accepts in both languages. */
#ifndef _STRING_H
#define _STRING_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *memcpy(void *__restrict d, const void *__restrict s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int   memcmp(const void *a, const void *b, size_t n);
void *memchr(const void *s, int c, size_t n);

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

#ifdef __cplusplus
}
#endif

#endif
