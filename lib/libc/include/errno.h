/* <errno.h> — C11 §7.5, plus the POSIX numbers a backend needs to report
 * what an operating system told it. The values match Linux's, because a
 * program that prints an errno and a person who looks it up should agree. */
#ifndef _ERRNO_H
#define _ERRNO_H

#ifdef __cplusplus
extern "C" {
#endif

/* A function, so that a threaded backend can give each thread its own
 * without every caller changing. */
int *__errno_location(void);
#define errno (*__errno_location())

#define EPERM    1
#define ENOENT   2
#define ESRCH    3
#define EINTR    4
#define EIO      5
#define ENXIO    6
#define E2BIG    7
#define ENOEXEC  8
#define EBADF    9
#define ECHILD  10
#define EAGAIN  11
#define ENOMEM  12
#define EACCES  13
#define EFAULT  14
#define EBUSY   16
#define EEXIST  17
#define EXDEV   18
#define ENODEV  19
#define ENOTDIR 20
#define EISDIR  21
#define EINVAL  22
#define ENFILE  23
#define EMFILE  24
#define ENOTTY  25
#define EFBIG   27
#define ENOSPC  28
#define ESPIPE  29
#define EROFS   30
#define EMLINK  31
#define EPIPE   32
#define EDOM    33
#define ERANGE  34
#define EDEADLK 35
#define ENAMETOOLONG 36
#define ENOLCK  37
#define ENOSYS  38
#define ENOTEMPTY 39
#define ELOOP   40

/* The rest of the POSIX set. Nothing in this library returns most of
 * them -- there are no sockets here -- but <system_error>'s `errc` is
 * SPECIFIED in terms of these names, and a C++ standard library missing
 * half of that enum is not one. They are also what a backend needs to
 * report faithfully: an OS that has sockets can say EADDRINUSE through
 * the seam without inventing a number, and the number it uses agrees
 * with what a person looking it up will find. */
#define ENOMSG  42
#define EIDRM   43
#define ENOLINK 67
#define EPROTO  71
#define EBADMSG 74
#define EOVERFLOW 75
#define ENOTSOCK 88
#define EDESTADDRREQ 89
#define EMSGSIZE 90
#define EPROTOTYPE 91
#define ENOPROTOOPT 92
#define EPROTONOSUPPORT 93
#define EOPNOTSUPP 95
#define ENOTSUP EOPNOTSUPP
#define EAFNOSUPPORT 97
#define EADDRINUSE 98
#define EADDRNOTAVAIL 99
#define ENETDOWN 100
#define ENETUNREACH 101
#define ENETRESET 102
#define ECONNABORTED 103
#define ECONNRESET 104
#define ENOBUFS 105
#define EISCONN 106
#define ENOTCONN 107
#define ETIMEDOUT 110
#define ECONNREFUSED 111
#define EHOSTUNREACH 113
#define EALREADY 114
#define EINPROGRESS 115
#define ECANCELED 125
#define EOWNERDEAD 130
#define ENOTRECOVERABLE 131

/* Linux gives EWOULDBLOCK the same value as EAGAIN, and code in the
 * wild tests either. Keeping them equal is what makes that code right
 * rather than subtly wrong on this target. */
#define EWOULDBLOCK EAGAIN

#define EILSEQ  84

#ifdef __cplusplus
}
#endif

#endif
