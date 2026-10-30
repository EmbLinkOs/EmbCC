/* <poll.h> -- waiting for any of several descriptors */
#ifndef _POLL_H
#define _POLL_H

#include <features.h>

#ifdef __cplusplus
extern "C" {
#endif

#define POLLIN     0x001
#define POLLPRI    0x002
#define POLLOUT    0x004
#define POLLERR    0x008
#define POLLHUP    0x010
#define POLLNVAL   0x020
#define POLLRDNORM 0x040
#define POLLRDBAND 0x080
#define POLLWRNORM 0x100
#define POLLWRBAND 0x200

typedef unsigned long nfds_t;

struct pollfd { int fd; short events, revents; };

int poll(struct pollfd *fds, nfds_t n, int timeout_ms);
#if defined(__EMB_GNU)
struct timespec;
int ppoll(struct pollfd *fds, nfds_t n, const struct timespec *timeout,
          const unsigned long long *sigmask);
#endif

#ifdef __cplusplus
}
#endif

#endif
