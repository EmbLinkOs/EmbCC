/* EmbCC's freestanding <string.h>: the standard declarations only. The
 * implementations come from the program's own libc / kernel (this header
 * takes no position on where). Enough of C's string.h for real code; grow
 * it as needed. */
#ifndef _STRING_H
#define _STRING_H

#include <stddef.h>   /* size_t, NULL */

#ifdef __cplusplus
extern "C" {
#endif

void   *memcpy(void *dst, const void *src, size_t n);
void   *memmove(void *dst, const void *src, size_t n);
void   *memset(void *s, int c, size_t n);
int     memcmp(const void *a, const void *b, size_t n);
void   *memchr(const void *s, int c, size_t n);

size_t  strlen(const char *s);
char   *strcpy(char *dst, const char *src);
char   *strncpy(char *dst, const char *src, size_t n);
char   *strcat(char *dst, const char *src);
char   *strncat(char *dst, const char *src, size_t n);
int     strcmp(const char *a, const char *b);
int     strncmp(const char *a, const char *b, size_t n);
char   *strchr(const char *s, int c);
char   *strrchr(const char *s, int c);
char   *strstr(const char *hay, const char *needle);
/* The rest of C11 §7.24's search and token set. These were absent
 * because nothing in this tree had needed them -- the header above says
 * "grow it as needed" -- and what needed them was the compiler linking
 * its own debug-info reader. A declaration missing here is not a link
 * error: it is a call compiled against a guess at the signature. */
size_t  strcspn(const char *s, const char *reject);
size_t  strspn(const char *s, const char *accept);
char   *strpbrk(const char *s, const char *accept);
char   *strtok(char *s, const char *sep);
char   *strerror(int errnum);

#ifdef __cplusplus
}
#endif
#endif
