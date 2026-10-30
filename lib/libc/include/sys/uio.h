/* <sys/uio.h> -- reads and writes of several buffers in one call */
#ifndef _SYS_UIO_H
#define _SYS_UIO_H

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

#ifdef __cplusplus
}
#endif

#endif
