/* <sys/socket.h> -- sockets (src/posix/socket.c). Linux's layouts and
 * numbers, which os/backend.h makes the seam's. */
#ifndef _SYS_SOCKET_H
#define _SYS_SOCKET_H

#include <features.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int socklen_t;
typedef unsigned short sa_family_t;

#ifndef _STRUCT_IOVEC_DECLARED
#define _STRUCT_IOVEC_DECLARED
struct iovec { void *iov_base; size_t iov_len; };
#endif

struct sockaddr {
    sa_family_t sa_family;
    char sa_data[14];
};

struct sockaddr_storage {
    sa_family_t ss_family;
    char __ss_pad[118];
    unsigned long __ss_align;
};

struct msghdr {
    void *msg_name;
    socklen_t msg_namelen;
    struct iovec *msg_iov;
    size_t msg_iovlen;
    void *msg_control;
    size_t msg_controllen;
    int msg_flags;
};

struct cmsghdr {
    size_t cmsg_len;
    int cmsg_level, cmsg_type;
};

#define __CMSG_ALIGN(n) (((n) + sizeof(size_t) - 1) & ~(sizeof(size_t) - 1))
#define CMSG_DATA(c)    ((unsigned char *)((struct cmsghdr *)(c) + 1))
#define CMSG_SPACE(n)   (__CMSG_ALIGN(n) + __CMSG_ALIGN(sizeof(struct cmsghdr)))
#define CMSG_LEN(n)     (__CMSG_ALIGN(sizeof(struct cmsghdr)) + (n))
#define CMSG_FIRSTHDR(m) ((size_t)(m)->msg_controllen >= sizeof(struct cmsghdr) \
    ? (struct cmsghdr *)(m)->msg_control : (struct cmsghdr *)0)
#define CMSG_NXTHDR(m, c) \
    ((c)->cmsg_len < sizeof(struct cmsghdr) || \
     __CMSG_ALIGN((c)->cmsg_len) + sizeof(struct cmsghdr) >= \
         (size_t)((unsigned char *)(m)->msg_control + (m)->msg_controllen - \
                  (unsigned char *)(c)) \
     ? (struct cmsghdr *)0 \
     : (struct cmsghdr *)((unsigned char *)(c) + __CMSG_ALIGN((c)->cmsg_len)))

struct linger { int l_onoff, l_linger; };

#define SOCK_STREAM    1
#define SOCK_DGRAM     2
#define SOCK_RAW       3
#define SOCK_RDM       4
#define SOCK_SEQPACKET 5
#define SOCK_NONBLOCK  04000
#if __INT_MAX__ > 0x7fff
#define SOCK_CLOEXEC   02000000
#else
#define SOCK_CLOEXEC   0       /* no exec where an int is 16 bits */
#endif

#define AF_UNSPEC 0
#define AF_UNIX   1
#define AF_LOCAL  AF_UNIX
#define AF_INET   2
#define AF_INET6  10
#define AF_PACKET 17
#define PF_UNSPEC AF_UNSPEC
#define PF_UNIX   AF_UNIX
#define PF_LOCAL  AF_LOCAL
#define PF_INET   AF_INET
#define PF_INET6  AF_INET6

#define SOL_SOCKET   1
#define SO_DEBUG     1
#define SO_REUSEADDR 2
#define SO_TYPE      3
#define SO_ERROR     4
#define SO_DONTROUTE 5
#define SO_BROADCAST 6
#define SO_SNDBUF    7
#define SO_RCVBUF    8
#define SO_KEEPALIVE 9
#define SO_OOBINLINE 10
#define SO_LINGER    13
#define SO_REUSEPORT 15
#define SO_RCVLOWAT  18
#define SO_SNDLOWAT  19
#define SO_RCVTIMEO  20
#define SO_SNDTIMEO  21
#define SO_ACCEPTCONN 30
#define SO_DOMAIN    39
#define SO_PROTOCOL  38

#define SCM_RIGHTS 1

#define MSG_OOB       0x0001
#define MSG_PEEK      0x0002
#define MSG_DONTROUTE 0x0004
#define MSG_CTRUNC    0x0008
#define MSG_TRUNC     0x0020
#define MSG_DONTWAIT  0x0040
#define MSG_EOR       0x0080
#define MSG_WAITALL   0x0100
#define MSG_NOSIGNAL  0x4000
#define MSG_CMSG_CLOEXEC 0x40000000

#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

#define SOMAXCONN 4096

int     accept(int fd, struct sockaddr *__restrict addr,
               socklen_t *__restrict len);
int     accept4(int fd, struct sockaddr *__restrict addr,
                socklen_t *__restrict len, int flags);
int     bind(int fd, const struct sockaddr *addr, socklen_t len);
int     connect(int fd, const struct sockaddr *addr, socklen_t len);
int     getpeername(int fd, struct sockaddr *__restrict addr,
                    socklen_t *__restrict len);
int     getsockname(int fd, struct sockaddr *__restrict addr,
                    socklen_t *__restrict len);
int     getsockopt(int fd, int level, int name, void *__restrict val,
                   socklen_t *__restrict len);
int     listen(int fd, int backlog);
ssize_t recv(int fd, void *buf, size_t n, int flags);
ssize_t recvfrom(int fd, void *__restrict buf, size_t n, int flags,
                 struct sockaddr *__restrict addr, socklen_t *__restrict len);
ssize_t recvmsg(int fd, struct msghdr *msg, int flags);
ssize_t send(int fd, const void *buf, size_t n, int flags);
ssize_t sendmsg(int fd, const struct msghdr *msg, int flags);
ssize_t sendto(int fd, const void *buf, size_t n, int flags,
               const struct sockaddr *addr, socklen_t len);
int     setsockopt(int fd, int level, int name, const void *val,
                   socklen_t len);
int     shutdown(int fd, int how);
int     sockatmark(int fd);
int     socket(int domain, int type, int protocol);
int     socketpair(int domain, int type, int protocol, int sv[2]);

#if defined(__EMB_GNU)
/* several messages in one call, Linux's (src/posix/linuxapi.c) */
struct mmsghdr {
    struct msghdr msg_hdr;
    unsigned int msg_len;
};
struct timespec;
int recvmmsg(int fd, struct mmsghdr *v, unsigned n, int flags, struct timespec *timeout);
int sendmmsg(int fd, struct mmsghdr *v, unsigned n, int flags);
#endif

#ifdef __cplusplus
}
#endif

#endif
