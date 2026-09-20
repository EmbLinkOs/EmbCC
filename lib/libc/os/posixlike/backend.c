/* The backend for a target that offers the classic syscall set: the QEMU
 * test harness, EmbLinkOS, and anything Unix-shaped.
 *
 * It is this short on purpose. Everything above it -- buffering, formatting,
 * the allocator's policy -- is portable and shared; what a new OS has to
 * write is this file and nothing else (os/backend.h).
 */
#include "../backend.h"

#include <errno.h>

/* Provided by the target: the raw calls, with the classic signatures. A
 * bare-metal harness implements them over a serial port; an OS over its
 * syscalls. */
extern long write(int, const void *, unsigned long);
extern long read(int, void *, unsigned long);
extern int  open(const char *, int, int);
extern int  close(int);
extern long lseek(int, long, int);
extern void *sbrk(long);
extern void _exit(int);
extern int  isatty(int);

long __os_write(int fd, const void *buf, size_t n)
{
    return write(fd, buf, (unsigned long)n);
}

long __os_read(int fd, void *buf, size_t n)
{
    return read(fd, buf, (unsigned long)n);
}

int  __os_open(const char *path, int flags, int mode)
{
    return open(path, flags, mode);
}

int  __os_close(int fd)  { return close(fd); }

long __os_lseek(int fd, long off, int whence)
{
    return lseek(fd, off, whence);
}

void *__os_sbrk(long increment) { return sbrk(increment); }

void __os_exit(int status) { _exit(status); for (;;) {} }

int __os_isatty(int fd) { return isatty(fd); }

/* No clock on a bare harness. Reporting -1 is the honest answer, and
 * time()/clock() pass it through rather than inventing a number. */
long __os_time(void)     { errno = ENOSYS; return -1; }
long __os_clock_ns(void) { errno = ENOSYS; return -1; }

int __os_getentropy(void *buf, size_t n)
{
    (void)buf; (void)n;
    errno = ENOSYS;
    return -1;
}
