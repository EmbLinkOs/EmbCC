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
    [EPERM] = "Operation not permitted",
    [ENOENT] = "No such file or directory",
    [ESRCH] = "No such process",
    [EINTR] = "Interrupted system call",
    [EIO] = "Input/output error",
    [ENXIO] = "No such device or address",
    [E2BIG] = "Argument list too long",
    [ENOEXEC] = "Exec format error",
    [EBADF] = "Bad file descriptor",
    [ECHILD] = "No child processes",
    [EAGAIN] = "Resource temporarily unavailable",
    [ENOMEM] = "Cannot allocate memory",
    [EACCES] = "Permission denied",
    [EFAULT] = "Bad address",
    [ENOTBLK] = "Block device required",
    [EBUSY] = "Device or resource busy",
    [EEXIST] = "File exists",
    [EXDEV] = "Invalid cross-device link",
    [ENODEV] = "No such device",
    [ENOTDIR] = "Not a directory",
    [EISDIR] = "Is a directory",
    [EINVAL] = "Invalid argument",
    [ENFILE] = "Too many open files in system",
    [EMFILE] = "Too many open files",
    [ENOTTY] = "Inappropriate ioctl for device",
    [ETXTBSY] = "Text file busy",
    [EFBIG] = "File too large",
    [ENOSPC] = "No space left on device",
    [ESPIPE] = "Illegal seek",
    [EROFS] = "Read-only file system",
    [EMLINK] = "Too many links",
    [EPIPE] = "Broken pipe",
    [EDOM] = "Numerical argument out of domain",
    [ERANGE] = "Numerical result out of range",
    [EDEADLK] = "Resource deadlock avoided",
    [ENAMETOOLONG] = "File name too long",
    [ENOLCK] = "No locks available",
    [ENOSYS] = "Function not implemented",
    [ENOTEMPTY] = "Directory not empty",
    [ELOOP] = "Too many levels of symbolic links",
    [ENOMSG] = "No message of desired type",
    [EIDRM] = "Identifier removed",
    [ECHRNG] = "Channel number out of range",
    [EL2NSYNC] = "Level 2 not synchronized",
    [EL3HLT] = "Level 3 halted",
    [EL3RST] = "Level 3 reset",
    [ELNRNG] = "Link number out of range",
    [EUNATCH] = "Protocol driver not attached",
    [ENOCSI] = "No CSI structure available",
    [EL2HLT] = "Level 2 halted",
    [EBADE] = "Invalid exchange",
    [EBADR] = "Invalid request descriptor",
    [EXFULL] = "Exchange full",
    [ENOANO] = "No anode",
    [EBADRQC] = "Invalid request code",
    [EBADSLT] = "Invalid slot",
    [EBFONT] = "Bad font file format",
    [ENOSTR] = "Device not a stream",
    [ENODATA] = "No data available",
    [ETIME] = "Timer expired",
    [ENOSR] = "Out of streams resources",
    [ENONET] = "Machine is not on the network",
    [ENOPKG] = "Package not installed",
    [EREMOTE] = "Object is remote",
    [ENOLINK] = "Link has been severed",
    [EADV] = "Advertise error",
    [ESRMNT] = "Srmount error",
    [ECOMM] = "Communication error on send",
    [EPROTO] = "Protocol error",
    [EMULTIHOP] = "Multihop attempted",
    [EDOTDOT] = "RFS specific error",
    [EBADMSG] = "Bad message",
    [EOVERFLOW] = "Value too large for defined data type",
    [ENOTUNIQ] = "Name not unique on network",
    [EBADFD] = "File descriptor in bad state",
    [EREMCHG] = "Remote address changed",
    [ELIBACC] = "Can not access a needed shared library",
    [ELIBBAD] = "Accessing a corrupted shared library",
    [ELIBSCN] = ".lib section in a.out corrupted",
    [ELIBMAX] = "Attempting to link in too many shared libraries",
    [ELIBEXEC] = "Cannot exec a shared library directly",
    [EILSEQ] = "Invalid or incomplete multibyte or wide character",
    [ERESTART] = "Interrupted system call should be restarted",
    [ESTRPIPE] = "Streams pipe error",
    [EUSERS] = "Too many users",
    [ENOTSOCK] = "Socket operation on non-socket",
    [EDESTADDRREQ] = "Destination address required",
    [EMSGSIZE] = "Message too long",
    [EPROTOTYPE] = "Protocol wrong type for socket",
    [ENOPROTOOPT] = "Protocol not available",
    [EPROTONOSUPPORT] = "Protocol not supported",
    [ESOCKTNOSUPPORT] = "Socket type not supported",
    [EOPNOTSUPP] = "Operation not supported",
    [EPFNOSUPPORT] = "Protocol family not supported",
    [EAFNOSUPPORT] = "Address family not supported by protocol",
    [EADDRINUSE] = "Address already in use",
    [EADDRNOTAVAIL] = "Cannot assign requested address",
    [ENETDOWN] = "Network is down",
    [ENETUNREACH] = "Network is unreachable",
    [ENETRESET] = "Network dropped connection on reset",
    [ECONNABORTED] = "Software caused connection abort",
    [ECONNRESET] = "Connection reset by peer",
    [ENOBUFS] = "No buffer space available",
    [EISCONN] = "Transport endpoint is already connected",
    [ENOTCONN] = "Transport endpoint is not connected",
    [ESHUTDOWN] = "Cannot send after transport endpoint shutdown",
    [ETOOMANYREFS] = "Too many references: cannot splice",
    [ETIMEDOUT] = "Connection timed out",
    [ECONNREFUSED] = "Connection refused",
    [EHOSTDOWN] = "Host is down",
    [EHOSTUNREACH] = "No route to host",
    [EALREADY] = "Operation already in progress",
    [EINPROGRESS] = "Operation now in progress",
    [ESTALE] = "Stale file handle",
    [EUCLEAN] = "Structure needs cleaning",
    [ENOTNAM] = "Not a XENIX named type file",
    [ENAVAIL] = "No XENIX semaphores available",
    [EISNAM] = "Is a named type file",
    [EREMOTEIO] = "Remote I/O error",
    [EDQUOT] = "Disk quota exceeded",
    [ENOMEDIUM] = "No medium found",
    [EMEDIUMTYPE] = "Wrong medium type",
    [ECANCELED] = "Operation canceled",
    [ENOKEY] = "Required key not available",
    [EKEYEXPIRED] = "Key has expired",
    [EKEYREVOKED] = "Key has been revoked",
    [EKEYREJECTED] = "Key was rejected by service",
    [EOWNERDEAD] = "Owner died",
    [ENOTRECOVERABLE] = "State not recoverable",
    [ERFKILL] = "Operation not possible due to RF-kill",
    [EHWPOISON] = "Memory page has hardware error",
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

/* XSI's strerror_r: the message copied into the caller's buffer, so two
 * threads asking at once do not share `unknown` above. 0, or ERANGE when
 * the buffer was too small -- in which case it still holds as much of
 * the message as fit, terminated, because a truncated message is more
 * use to a person than an empty one. EINVAL for a number with no
 * message, whose buffer says "Unknown error N" as strerror would. */
__attribute__((weak)) int strerror_r(int errnum, char *buf, size_t n)
{
    int known = errnum >= 0 && (size_t)errnum < sizeof msg / sizeof msg[0] &&
                msg[errnum];
    char tmp[32];
    const char *m = known ? msg[errnum] : tmp;
    if (!known)
        snprintf(tmp, sizeof tmp, "Unknown error %d", errnum);
    size_t len = strlen(m);
    if (n) {
        size_t k = len < n - 1 ? len : n - 1;
        memcpy(buf, m, k);
        buf[k] = 0;
    }
    return !known ? EINVAL : len >= n ? ERANGE : 0;
}

/* ---- glibc's: the macro's name, and the message without "Unknown" -------- */

static const char *const names[] = {
    [EPERM] = "EPERM",
    [ENOENT] = "ENOENT",
    [ESRCH] = "ESRCH",
    [EINTR] = "EINTR",
    [EIO] = "EIO",
    [ENXIO] = "ENXIO",
    [E2BIG] = "E2BIG",
    [ENOEXEC] = "ENOEXEC",
    [EBADF] = "EBADF",
    [ECHILD] = "ECHILD",
    [EAGAIN] = "EAGAIN",
    [ENOMEM] = "ENOMEM",
    [EACCES] = "EACCES",
    [EFAULT] = "EFAULT",
    [ENOTBLK] = "ENOTBLK",
    [EBUSY] = "EBUSY",
    [EEXIST] = "EEXIST",
    [EXDEV] = "EXDEV",
    [ENODEV] = "ENODEV",
    [ENOTDIR] = "ENOTDIR",
    [EISDIR] = "EISDIR",
    [EINVAL] = "EINVAL",
    [ENFILE] = "ENFILE",
    [EMFILE] = "EMFILE",
    [ENOTTY] = "ENOTTY",
    [ETXTBSY] = "ETXTBSY",
    [EFBIG] = "EFBIG",
    [ENOSPC] = "ENOSPC",
    [ESPIPE] = "ESPIPE",
    [EROFS] = "EROFS",
    [EMLINK] = "EMLINK",
    [EPIPE] = "EPIPE",
    [EDOM] = "EDOM",
    [ERANGE] = "ERANGE",
    [EDEADLK] = "EDEADLK",
    [ENAMETOOLONG] = "ENAMETOOLONG",
    [ENOLCK] = "ENOLCK",
    [ENOSYS] = "ENOSYS",
    [ENOTEMPTY] = "ENOTEMPTY",
    [ELOOP] = "ELOOP",
    [ENOMSG] = "ENOMSG",
    [EIDRM] = "EIDRM",
    [ECHRNG] = "ECHRNG",
    [EL2NSYNC] = "EL2NSYNC",
    [EL3HLT] = "EL3HLT",
    [EL3RST] = "EL3RST",
    [ELNRNG] = "ELNRNG",
    [EUNATCH] = "EUNATCH",
    [ENOCSI] = "ENOCSI",
    [EL2HLT] = "EL2HLT",
    [EBADE] = "EBADE",
    [EBADR] = "EBADR",
    [EXFULL] = "EXFULL",
    [ENOANO] = "ENOANO",
    [EBADRQC] = "EBADRQC",
    [EBADSLT] = "EBADSLT",
    [EBFONT] = "EBFONT",
    [ENOSTR] = "ENOSTR",
    [ENODATA] = "ENODATA",
    [ETIME] = "ETIME",
    [ENOSR] = "ENOSR",
    [ENONET] = "ENONET",
    [ENOPKG] = "ENOPKG",
    [EREMOTE] = "EREMOTE",
    [ENOLINK] = "ENOLINK",
    [EADV] = "EADV",
    [ESRMNT] = "ESRMNT",
    [ECOMM] = "ECOMM",
    [EPROTO] = "EPROTO",
    [EMULTIHOP] = "EMULTIHOP",
    [EDOTDOT] = "EDOTDOT",
    [EBADMSG] = "EBADMSG",
    [EOVERFLOW] = "EOVERFLOW",
    [ENOTUNIQ] = "ENOTUNIQ",
    [EBADFD] = "EBADFD",
    [EREMCHG] = "EREMCHG",
    [ELIBACC] = "ELIBACC",
    [ELIBBAD] = "ELIBBAD",
    [ELIBSCN] = "ELIBSCN",
    [ELIBMAX] = "ELIBMAX",
    [ELIBEXEC] = "ELIBEXEC",
    [EILSEQ] = "EILSEQ",
    [ERESTART] = "ERESTART",
    [ESTRPIPE] = "ESTRPIPE",
    [EUSERS] = "EUSERS",
    [ENOTSOCK] = "ENOTSOCK",
    [EDESTADDRREQ] = "EDESTADDRREQ",
    [EMSGSIZE] = "EMSGSIZE",
    [EPROTOTYPE] = "EPROTOTYPE",
    [ENOPROTOOPT] = "ENOPROTOOPT",
    [EPROTONOSUPPORT] = "EPROTONOSUPPORT",
    [ESOCKTNOSUPPORT] = "ESOCKTNOSUPPORT",
    [EOPNOTSUPP] = "EOPNOTSUPP",
    [EPFNOSUPPORT] = "EPFNOSUPPORT",
    [EAFNOSUPPORT] = "EAFNOSUPPORT",
    [EADDRINUSE] = "EADDRINUSE",
    [EADDRNOTAVAIL] = "EADDRNOTAVAIL",
    [ENETDOWN] = "ENETDOWN",
    [ENETUNREACH] = "ENETUNREACH",
    [ENETRESET] = "ENETRESET",
    [ECONNABORTED] = "ECONNABORTED",
    [ECONNRESET] = "ECONNRESET",
    [ENOBUFS] = "ENOBUFS",
    [EISCONN] = "EISCONN",
    [ENOTCONN] = "ENOTCONN",
    [ESHUTDOWN] = "ESHUTDOWN",
    [ETOOMANYREFS] = "ETOOMANYREFS",
    [ETIMEDOUT] = "ETIMEDOUT",
    [ECONNREFUSED] = "ECONNREFUSED",
    [EHOSTDOWN] = "EHOSTDOWN",
    [EHOSTUNREACH] = "EHOSTUNREACH",
    [EALREADY] = "EALREADY",
    [EINPROGRESS] = "EINPROGRESS",
    [ESTALE] = "ESTALE",
    [EUCLEAN] = "EUCLEAN",
    [ENOTNAM] = "ENOTNAM",
    [ENAVAIL] = "ENAVAIL",
    [EISNAM] = "EISNAM",
    [EREMOTEIO] = "EREMOTEIO",
    [EDQUOT] = "EDQUOT",
    [ENOMEDIUM] = "ENOMEDIUM",
    [EMEDIUMTYPE] = "EMEDIUMTYPE",
    [ECANCELED] = "ECANCELED",
    [ENOKEY] = "ENOKEY",
    [EKEYEXPIRED] = "EKEYEXPIRED",
    [EKEYREVOKED] = "EKEYREVOKED",
    [EKEYREJECTED] = "EKEYREJECTED",
    [EOWNERDEAD] = "EOWNERDEAD",
    [ENOTRECOVERABLE] = "ENOTRECOVERABLE",
    [ERFKILL] = "ERFKILL",
    [EHWPOISON] = "EHWPOISON",
};

__attribute__((weak)) const char *strerrorname_np(int errnum)
{
    if (errnum == 0)
        return "0";
    return errnum > 0 && (size_t)errnum < sizeof names / sizeof names[0] ?
           names[errnum] : NULL;
}

__attribute__((weak)) const char *strerrordesc_np(int errnum)
{
    return errnum >= 0 && (size_t)errnum < sizeof msg / sizeof msg[0] ?
           msg[errnum] : NULL;
}
