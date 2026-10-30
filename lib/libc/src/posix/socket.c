/* <sys/socket.h>: each call its primitive. The layouts are the seam's
 * own (Linux's), so nothing is converted. */
#include <sys/socket.h>
#include <sys/ioctl.h>

#include "posix.h"

WEAK int socket(int domain, int type, int protocol)
{
    return __os_socket(domain, type, protocol);
}

WEAK int socketpair(int domain, int type, int protocol, int sv[2])
{
    return __os_socketpair(domain, type, protocol, sv);
}

WEAK int bind(int fd, const struct sockaddr *addr, socklen_t len)
{
    return __os_bind(fd, addr, len);
}

WEAK int connect(int fd, const struct sockaddr *addr, socklen_t len)
{
    return __os_connect(fd, addr, len);
}

WEAK int listen(int fd, int backlog) { return __os_listen(fd, backlog); }

WEAK int accept(int fd, struct sockaddr *restrict addr,
                socklen_t *restrict len)
{
    return __os_accept(fd, addr, len, 0);
}

WEAK int accept4(int fd, struct sockaddr *restrict addr,
                 socklen_t *restrict len, int flags)
{
    return __os_accept(fd, addr, len, flags);
}

WEAK ssize_t send(int fd, const void *buf, size_t n, int flags)
{
    return __os_sendto(fd, buf, n, flags, NULL, 0);
}

WEAK ssize_t sendto(int fd, const void *buf, size_t n, int flags,
                    const struct sockaddr *addr, socklen_t len)
{
    return __os_sendto(fd, buf, n, flags, addr, len);
}

WEAK ssize_t recv(int fd, void *buf, size_t n, int flags)
{
    return __os_recvfrom(fd, buf, n, flags, NULL, NULL);
}

WEAK ssize_t recvfrom(int fd, void *restrict buf, size_t n, int flags,
                      struct sockaddr *restrict addr, socklen_t *restrict len)
{
    return __os_recvfrom(fd, buf, n, flags, addr, len);
}

WEAK ssize_t sendmsg(int fd, const struct msghdr *msg, int flags)
{
    return __os_sendmsg(fd, msg, flags);
}

WEAK ssize_t recvmsg(int fd, struct msghdr *msg, int flags)
{
    return __os_recvmsg(fd, msg, flags);
}

WEAK int shutdown(int fd, int how) { return __os_shutdown(fd, how); }

WEAK int getsockopt(int fd, int level, int name, void *restrict val,
                    socklen_t *restrict len)
{
    return __os_getsockopt(fd, level, name, val, len);
}

WEAK int setsockopt(int fd, int level, int name, const void *val,
                    socklen_t len)
{
    return __os_setsockopt(fd, level, name, val, len);
}

WEAK int getsockname(int fd, struct sockaddr *restrict addr,
                     socklen_t *restrict len)
{
    return __os_getsockname(fd, addr, len);
}

WEAK int getpeername(int fd, struct sockaddr *restrict addr,
                     socklen_t *restrict len)
{
    return __os_getpeername(fd, addr, len);
}

/* SIOCATMARK: whether the next byte is the urgent one */
WEAK int sockatmark(int fd)
{
    int at = 0;
    return __os_ioctl(fd, 0x8905, &at) < 0 ? -1 : at;
}
