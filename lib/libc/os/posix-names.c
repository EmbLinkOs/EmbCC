/* The POSIX names that a backend's primitives are themselves built on,
 * where a backend is built on names at all.
 *
 * The bare-metal and posix-like backends implement __os_write by calling
 * write, __os_sbrk by calling sbrk, and so on: those names are the
 * program's, or the board's (a weak default the program replaces), as
 * newlib's retargeting names are. Defined here too, they would be two
 * weak definitions of one name, and whichever an image happened to link
 * first would win -- this file's write calling __os_write calling write,
 * round and round until the stack ran out (posix-threads did that on
 * Xtensa once proc.o came first).
 *
 * So this file is built only for backends whose primitives are the
 * kernel's system calls -- Linux and EmbLinkOS (the Makefile names it for
 * those) -- where these names have nobody else to come from. */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <unistd.h>

#include "backend.h"

#define WEAK __attribute__((weak))

/* the mode argument is there only when the flags create */
WEAK int open(const char *path, int flags, ...)
{
    unsigned mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, unsigned);
        va_end(ap);
    }
    return __os_open(path, flags, (int)mode);
}

WEAK int close(int fd) { return __os_close(fd); }

WEAK ssize_t read(int fd, void *buf, size_t n)
{
    return __os_read(fd, buf, n);
}

WEAK ssize_t write(int fd, const void *buf, size_t n)
{
    return __os_write(fd, buf, n);
}

WEAK off_t lseek(int fd, off_t off, int whence)
{
    return __os_lseek(fd, off, whence);
}

/* 0 with EBADF for a descriptor that is not open, ENOTTY for one that
 * is open on something else -- the backend's isatty says only yes or no */
WEAK int isatty(int fd)
{
    if (__os_isatty(fd))
        return 1;
    struct __os_fileinfo fi;
    if (__os_fstat(fd, &fi) < 0 && errno == EBADF)
        return 0;
    errno = ENOTTY;
    return 0;
}

WEAK _Noreturn void _exit(int status)
{
    __os_exit(status);
    for (;;)
        ;
}

/* the heap's edge, for programs from before malloc */
WEAK void *sbrk(intptr_t increment) { return __os_sbrk((long)increment); }
