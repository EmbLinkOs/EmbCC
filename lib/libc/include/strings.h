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

#ifdef __cplusplus
}
#endif

#endif
