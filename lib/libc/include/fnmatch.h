/* <fnmatch.h> -- shell wildcard matching (src/posix/fnmatch.c). The flag
 * values are glibc's, as is FNM_NOMATCH, so a program's numbers agree
 * with the system it was written on. */
#ifndef _FNMATCH_H
#define _FNMATCH_H

#ifdef __cplusplus
extern "C" {
#endif

#define FNM_PATHNAME    0x01   /* a '/' is matched only by a '/' */
#define FNM_NOESCAPE    0x02   /* '\' is an ordinary character */
#define FNM_PERIOD      0x04   /* a leading '.' only by a '.' */
#define FNM_LEADING_DIR 0x08   /* the pattern may match a leading dir/ */
#define FNM_CASEFOLD    0x10   /* letters match either case */
#define FNM_IGNORECASE  FNM_CASEFOLD
#define FNM_FILE_NAME   FNM_PATHNAME

#define FNM_NOMATCH 1
#define FNM_NOSYS   (-1)

int fnmatch(const char *pattern, const char *string, int flags);

#ifdef __cplusplus
}
#endif

#endif
