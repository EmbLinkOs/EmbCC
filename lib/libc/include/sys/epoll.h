/* <sys/epoll.h> -- Linux's event polling (src/posix/linuxapi.c). Where
 * the kernel is not Linux each call fails with ENOSYS. */
#ifndef _SYS_EPOLL_H
#define _SYS_EPOLL_H

#include <stdint.h>
#include <bits/sigset.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EPOLL_CLOEXEC 02000000

#define EPOLLIN      0x001
#define EPOLLPRI     0x002
#define EPOLLOUT     0x004
#define EPOLLRDNORM  0x040
#define EPOLLRDBAND  0x080
#define EPOLLWRNORM  0x100
#define EPOLLWRBAND  0x200
#define EPOLLMSG     0x400
#define EPOLLERR     0x008
#define EPOLLHUP     0x010
#define EPOLLRDHUP   0x2000
#define EPOLLEXCLUSIVE (1U << 28)
#define EPOLLWAKEUP  (1U << 29)
#define EPOLLONESHOT (1U << 30)
#define EPOLLET      (1U << 31)

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

typedef union epoll_data {
    void *ptr;
    int fd;
    uint32_t u32;
    uint64_t u64;
} epoll_data_t;

/* packed on x86-64, as the kernel lays it out there */
struct epoll_event {
    uint32_t events;
    epoll_data_t data;
}
#if defined(__x86_64__)
__attribute__((packed))
#endif
;

struct timespec;

int epoll_create(int size);
int epoll_create1(int flags);
int epoll_ctl(int epfd, int op, int fd, struct epoll_event *ev);
int epoll_wait(int epfd, struct epoll_event *evs, int max, int timeout);
int epoll_pwait(int epfd, struct epoll_event *evs, int max, int timeout,
                const sigset_t *mask);
int epoll_pwait2(int epfd, struct epoll_event *evs, int max,
                 const struct timespec *timeout, const sigset_t *mask);

#ifdef __cplusplus
}
#endif

#endif
