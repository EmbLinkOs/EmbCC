/* <strings.h> -- POSIX's BSD-era string functions. */
#ifndef _STRINGS_H
#define _STRINGS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void  bzero(void *s, size_t n);
void  bcopy(const void *s, void *d, size_t n);
int   bcmp(const void *a, const void *b, size_t n);
char *index(const char *s, int c);
char *rindex(const char *s, int c);
int   strcasecmp(const char *a, const char *b);
int   strncasecmp(const char *a, const char *b, size_t n);
int   ffs(int i);

#include <bits/locale_t.h>
int strcasecmp_l(const char *a, const char *b, locale_t l);
int strncasecmp_l(const char *a, const char *b, size_t n, locale_t l);
int ffsl(long v);
int ffsll(long long v);

#ifdef __cplusplus
}
#endif

#endif
