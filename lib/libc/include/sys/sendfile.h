/* <sys/sendfile.h> -- copying between descriptors in the kernel
 * (src/posix/linuxapi.c); a read-and-write loop where it is not Linux. */
#ifndef _SYS_SENDFILE_H
#define _SYS_SENDFILE_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

ssize_t sendfile(int out, int in, off_t *offset, size_t count);
#if defined(_LARGEFILE64_SOURCE)
#define sendfile64 sendfile
#endif

#ifdef __cplusplus
}
#endif

#endif
