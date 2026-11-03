/* <argz.h> -- GNU's argz vectors (src/posix/argz.c): strings back to
 * back in one buffer, each ending in its NUL, the buffer's length
 * counting every NUL. Empty is a null pointer and a length of 0. */
#ifndef _ARGZ_H
#define _ARGZ_H

#include <errno.h>
#ifndef __error_t_defined
#define __error_t_defined 1
typedef int error_t;
#endif
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

error_t argz_create(char *const argv[], char **argz, size_t *len);
error_t argz_create_sep(const char *s, int sep, char **argz, size_t *len);
size_t  argz_count(const char *argz, size_t len);
void    argz_extract(const char *argz, size_t len, char **argv);
void    argz_stringify(char *argz, size_t len, int sep);
error_t argz_append(char **argz, size_t *len, const char *buf, size_t buf_len);
error_t argz_add(char **argz, size_t *len, const char *s);
error_t argz_add_sep(char **argz, size_t *len, const char *s, int sep);
void    argz_delete(char **argz, size_t *len, char *entry);
error_t argz_insert(char **argz, size_t *len, char *before, const char *entry);
error_t argz_replace(char **argz, size_t *len, const char *str, const char *with,
                     unsigned *count);
char   *argz_next(const char *argz, size_t len, const char *entry);

#ifdef __cplusplus
}
#endif

#endif
