/* <envz.h> -- GNU's envz vectors (src/posix/argz.c): an argz vector of
 * "name=value" entries, where an entry without '=' is a name with a null
 * value. */
#ifndef _ENVZ_H
#define _ENVZ_H

#include <argz.h>

#ifdef __cplusplus
extern "C" {
#endif

char   *envz_entry(const char *envz, size_t len, const char *name);
char   *envz_get(const char *envz, size_t len, const char *name);
error_t envz_add(char **envz, size_t *len, const char *name, const char *value);
error_t envz_merge(char **envz, size_t *len, const char *envz2, size_t len2,
                   int override);
void    envz_remove(char **envz, size_t *len, const char *name);
void    envz_strip(char **envz, size_t *len);

#ifdef __cplusplus
}
#endif

#endif
