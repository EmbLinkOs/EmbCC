/* The seam between the C library and an operating system.
 *
 * Everything above this is portable C; everything below is one small file
 * per OS. A new target implements these and gets the library — the same
 * bargain `src/platform/platform.h` makes for the compiler.
 *
 * The set is deliberately the classic one, because it is the set that is
 * actually sufficient: a hosted C library needs to move bytes to and from
 * file descriptors, to grow its heap, to know the time, and to stop. It
 * does not need anything else, and a backend that offers more invites the
 * library above it to depend on a particular OS.
 */
#ifndef EMBLIBC_OS_BACKEND_H
#define EMBLIBC_OS_BACKEND_H

#include <stddef.h>

/* The C++ runtime's terminate handler writes its final message through
 * __os_write, for the same reason assert does: a diagnostic that needs a
 * working allocator disappears in the cases worth diagnosing. So this
 * contract is reached from C++ and carries C linkage explicitly. */
#ifdef __cplusplus
extern "C" {
#endif

/* Bytes in and out. Return the count moved, or -1 with errno set. A short
 * write is not an error and the caller loops — which is why stdio, not the
 * backend, owns buffering. */
long __os_write(int fd, const void *buf, size_t n);
long __os_read(int fd, void *buf, size_t n);

/* The flags __os_open takes. They are part of THIS contract, not of any
 * one OS's headers: the library has to name them to ask for "create it,
 * truncate it", and a backend whose kernel numbers them differently
 * translates. The values are the ones Linux x86 chose and nearly every
 * kernel since copied, so most backends translate by doing nothing --
 * EmbLinkOS's numbers are identical, and tests/golden/libc-emblinkos.sh
 * asserts that rather than trusting it. */
#define __OS_O_RDONLY 0x0000
#define __OS_O_WRONLY 0x0001
#define __OS_O_RDWR   0x0002
#define __OS_O_CREAT  0x0040
#define __OS_O_EXCL   0x0080
#define __OS_O_TRUNC  0x0200
#define __OS_O_APPEND 0x0400

/* Files by name. A backend with no filesystem returns -1 and sets ENOSYS;
 * everything that does not need one still works. */
int  __os_open(const char *path, int flags, int mode);
int  __os_close(int fd);
long __os_lseek(int fd, long off, int whence);

/* The heap. Returns the previous break, or (void *)-1. A backend that
 * cannot grow returns -1 and malloc reports failure honestly rather than
 * handing back memory it does not have. */
void *__os_sbrk(long increment);

/* Seconds since the epoch, or -1 where the OS has no clock. */
long __os_time(void);

/* Nanoseconds of CPU time used, or -1: what clock() reports. */
long __os_clock_ns(void);

/* Stop. Never returns. */
void __os_exit(int status);

/* Is this descriptor a terminal? stdio uses it to choose line buffering. */
int  __os_isatty(int fd);

/* Random bytes for the few library functions that must not be predictable.
 * 0 on success, -1 when the OS cannot provide them. */
int  __os_getentropy(void *buf, size_t n);

#ifdef __cplusplus
}
#endif

#endif
