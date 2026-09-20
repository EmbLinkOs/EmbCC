/* strerror — the message for an errno.
 *
 * A table, not a switch, and every entry says what a PERSON needs: which
 * operation failed and why, not the macro's name spelled out. An unknown
 * number reports itself rather than returning NULL, because a caller
 * printing the result should never print "(null)".
 */
#include <string.h>
#include <errno.h>
#include <stdio.h>

static const char *const msg[] = {
    [0]         = "Success",
    [EPERM]     = "Operation not permitted",
    [ENOENT]    = "No such file or directory",
    [ESRCH]     = "No such process",
    [EINTR]     = "Interrupted system call",
    [EIO]       = "Input/output error",
    [ENXIO]     = "No such device or address",
    [E2BIG]     = "Argument list too long",
    [ENOEXEC]   = "Exec format error",
    [EBADF]     = "Bad file descriptor",
    [ECHILD]    = "No child processes",
    [EAGAIN]    = "Resource temporarily unavailable",
    [ENOMEM]    = "Cannot allocate memory",
    [EACCES]    = "Permission denied",
    [EFAULT]    = "Bad address",
    [EBUSY]     = "Device or resource busy",
    [EEXIST]    = "File exists",
    [EXDEV]     = "Invalid cross-device link",
    [ENODEV]    = "No such device",
    [ENOTDIR]   = "Not a directory",
    [EISDIR]    = "Is a directory",
    [EINVAL]    = "Invalid argument",
    [ENFILE]    = "Too many open files in system",
    [EMFILE]    = "Too many open files",
    [ENOTTY]    = "Inappropriate ioctl for device",
    [EFBIG]     = "File too large",
    [ENOSPC]    = "No space left on device",
    [ESPIPE]    = "Illegal seek",
    [EROFS]     = "Read-only file system",
    [EMLINK]    = "Too many links",
    [EPIPE]     = "Broken pipe",
    [EDOM]      = "Numerical argument out of domain",
    [ERANGE]    = "Numerical result out of range",
    [EDEADLK]   = "Resource deadlock avoided",
    [ENAMETOOLONG] = "File name too long",
    [ENOLCK]    = "No locks available",
    [ENOSYS]    = "Function not implemented",
    [ENOTEMPTY] = "Directory not empty",
    [EOVERFLOW] = "Value too large for defined data type",
    [EILSEQ]    = "Invalid or incomplete multibyte or wide character",
};

char *strerror(int errnum)
{
    static char unknown[32];
    if (errnum >= 0 && (size_t)errnum < sizeof msg / sizeof msg[0] &&
        msg[errnum])
        return (char *)msg[errnum];
    snprintf(unknown, sizeof unknown, "Unknown error %d", errnum);
    return unknown;
}
