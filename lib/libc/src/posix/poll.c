/* poll, ppoll, select and pselect, all on the one primitive: ppoll.
 * select's descriptor sets become a pollfd list and back. */
#define _GNU_SOURCE 1
#include <poll.h>
#include <stdlib.h>
#include <sys/select.h>

#include "posix.h"

_Static_assert(sizeof(struct pollfd) == sizeof(struct __os_pollfd),
               "struct pollfd is not the seam's");

/* a negative timeout waits for ever */
WEAK int poll(struct pollfd *fds, nfds_t n, int timeout_ms)
{
    struct __os_timespec t = { timeout_ms / 1000,
                               (long)(timeout_ms % 1000) * 1000000L };
    return __os_poll((struct __os_pollfd *)(void *)fds, n,
                     timeout_ms < 0 ? NULL : &t, NULL);
}

WEAK int ppoll(struct pollfd *fds, nfds_t n, const struct timespec *timeout,
               const unsigned long long *sigmask)
{
    struct __os_timespec t;
    if (timeout) {
        t.sec = timeout->tv_sec;
        t.nsec = timeout->tv_nsec;
    }
    return __os_poll((struct __os_pollfd *)(void *)fds, n,
                     timeout ? &t : NULL, sigmask);
}

#define RD (POLLIN | POLLHUP | POLLERR)
#define WR (POLLOUT | POLLERR)
#define EX POLLPRI

static int do_select(int n, fd_set *r, fd_set *w, fd_set *e,
                     const struct __os_timespec *timeout,
                     const unsigned long long *sigmask)
{
    if (n < 0 || n > FD_SETSIZE) {
        errno = EINVAL;
        return -1;
    }
    struct __os_pollfd small[64], *p = small;
    int count = 0;
    for (int fd = 0; fd < n; fd++)
        if ((r && FD_ISSET(fd, r)) || (w && FD_ISSET(fd, w)) ||
            (e && FD_ISSET(fd, e)))
            count++;
    if (count > 64 && !(p = malloc((size_t)count * sizeof *p))) {
        errno = ENOMEM;
        return -1;
    }
    int k = 0;
    for (int fd = 0; fd < n; fd++) {
        short ev = 0;
        if (r && FD_ISSET(fd, r)) ev |= POLLIN;
        if (w && FD_ISSET(fd, w)) ev |= POLLOUT;
        if (e && FD_ISSET(fd, e)) ev |= POLLPRI;
        if (ev) {
            p[k].fd = fd;
            p[k].events = ev;
            p[k++].revents = 0;
        }
    }
    int got = __os_poll(p, (unsigned long)k, timeout, sigmask);
    if (got >= 0) {
        got = 0;
        for (int i = 0; i < k; i++)
            if (p[i].revents & POLLNVAL) {
                got = -1;
                errno = EBADF;
                break;
            }
        if (got == 0) {
            if (r) FD_ZERO(r);
            if (w) FD_ZERO(w);
            if (e) FD_ZERO(e);
            for (int i = 0; i < k; i++) {
                int fd = p[i].fd;
                short re = p[i].revents, ev = p[i].events;
                if ((ev & POLLIN) && (re & RD)) { FD_SET(fd, r); got++; }
                if ((ev & POLLOUT) && (re & WR)) { FD_SET(fd, w); got++; }
                if ((ev & POLLPRI) && (re & EX)) { FD_SET(fd, e); got++; }
            }
        }
    }
    if (p != small)
        free(p);
    return got;
}

WEAK int select(int n, fd_set *restrict r, fd_set *restrict w,
                fd_set *restrict e, struct timeval *restrict timeout)
{
    struct __os_timespec t;
    if (timeout) {
        if (timeout->tv_sec < 0 || timeout->tv_usec < 0) {
            errno = EINVAL;
            return -1;
        }
        t.sec = timeout->tv_sec + timeout->tv_usec / 1000000;
        t.nsec = (timeout->tv_usec % 1000000) * 1000L;
    }
    return do_select(n, r, w, e, timeout ? &t : NULL, NULL);
}

WEAK int pselect(int n, fd_set *restrict r, fd_set *restrict w,
                 fd_set *restrict e, const struct timespec *restrict timeout,
                 const unsigned long long *restrict sigmask)
{
    struct __os_timespec t;
    if (timeout) {
        t.sec = timeout->tv_sec;
        t.nsec = timeout->tv_nsec;
    }
    return do_select(n, r, w, e, timeout ? &t : NULL, sigmask);
}
