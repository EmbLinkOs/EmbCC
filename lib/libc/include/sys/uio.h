/* <sys/uio.h> -- reads and writes of several buffers in one call */
#ifndef _SYS_UIO_H
#define _SYS_UIO_H

#include <features.h>

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IOV_MAX 1024

#ifndef _STRUCT_IOVEC_DECLARED
#define _STRUCT_IOVEC_DECLARED
struct iovec { void *iov_base; size_t iov_len; };
#endif

ssize_t readv(int fd, const struct iovec *iov, int n);
ssize_t writev(int fd, const struct iovec *iov, int n);
ssize_t preadv(int fd, const struct iovec *iov, int n, off_t off);
ssize_t pwritev(int fd, const struct iovec *iov, int n, off_t off);

#if defined(__EMB_GNU)
/* Linux's, with per-call flags (src/posix/linuxapi.c) */
#define RWF_HIPRI  0x00000001
#define RWF_DSYNC  0x00000002
#define RWF_SYNC   0x00000004
#define RWF_NOWAIT 0x00000008
#define RWF_APPEND 0x00000010
ssize_t preadv2(int fd, const struct iovec *iov, int n, off_t off, int flags);
ssize_t pwritev2(int fd, const struct iovec *iov, int n, off_t off, int flags);
#define preadv64 preadv
#define pwritev64 pwritev
#define preadv64v2 preadv2
#define pwritev64v2 pwritev2
#endif

#ifdef __cplusplus
}
#endif

#endif
