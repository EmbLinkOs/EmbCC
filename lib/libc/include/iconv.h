/* <iconv.h> -- text from one character encoding to another: POSIX's
 * iconv_open, iconv and iconv_close. The encodings, and the //TRANSLIT
 * and //IGNORE suffixes, are listed in src/posix/iconv.c. */
#ifndef _ICONV_H
#define _ICONV_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *iconv_t;

iconv_t iconv_open(const char *tocode, const char *fromcode);
size_t  iconv(iconv_t cd, char **__restrict in, size_t *__restrict inleft,
              char **__restrict out, size_t *__restrict outleft);
int     iconv_close(iconv_t cd);

#ifdef __cplusplus
}
#endif

#endif
