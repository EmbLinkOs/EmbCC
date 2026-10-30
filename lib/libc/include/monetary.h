/* <monetary.h> -- XSI's strfmon (src/stdlib/strfmon.c): amounts of money
 * as the locale writes them, which in "C" means with '.' and no
 * currency symbol, as glibc writes them there. */
#ifndef _MONETARY_H
#define _MONETARY_H

#include <stddef.h>
#include <sys/types.h>
#include <bits/locale_t.h>

#ifdef __cplusplus
extern "C" {
#endif

ssize_t strfmon(char *__restrict s, size_t max, const char *__restrict fmt, ...);
ssize_t strfmon_l(char *__restrict s, size_t max, locale_t l,
                  const char *__restrict fmt, ...);

#ifdef __cplusplus
}
#endif

#endif
