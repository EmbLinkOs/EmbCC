/* <libgen.h> -- POSIX's basename and dirname (src/posix/libgen.c). */
#ifndef _LIBGEN_H
#define _LIBGEN_H

#ifdef __cplusplus
extern "C" {
#endif

char *basename(char *path);
char *dirname(char *path);

#ifdef __cplusplus
}
#endif

#endif
