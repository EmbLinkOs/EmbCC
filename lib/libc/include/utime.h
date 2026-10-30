/* <utime.h> -- a file's access and modification times, in seconds */
#ifndef _UTIME_H
#define _UTIME_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct utimbuf { time_t actime, modtime; };

int utime(const char *path, const struct utimbuf *t);

#ifdef __cplusplus
}
#endif

#endif
