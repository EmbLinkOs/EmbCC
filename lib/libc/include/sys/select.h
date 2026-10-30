/* <sys/select.h> -- select and its descriptor sets, over poll
 * (src/posix/poll.c) */
#ifndef _SYS_SELECT_H
#define _SYS_SELECT_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _STRUCT_TIMEVAL_DECLARED
#define _STRUCT_TIMEVAL_DECLARED
struct timeval { time_t tv_sec; suseconds_t tv_usec; };
#endif
#ifndef _STRUCT_TIMESPEC_DECLARED
#define _STRUCT_TIMESPEC_DECLARED
struct timespec { time_t tv_sec; long tv_nsec; };
#endif

#define FD_SETSIZE 1024

typedef struct {
    unsigned long fds_bits[FD_SETSIZE / (8 * sizeof(unsigned long))];
} fd_set;

#define __FD_WORD(fd) ((unsigned)(fd) / (8 * sizeof(unsigned long)))
#define __FD_BIT(fd)  (1UL << ((unsigned)(fd) % (8 * sizeof(unsigned long))))
#define FD_ZERO(s) do { \
    for (unsigned __i = 0; __i < sizeof((s)->fds_bits) / sizeof(unsigned long); __i++) \
        (s)->fds_bits[__i] = 0; \
} while (0)
#define FD_SET(fd, s)   ((void)((s)->fds_bits[__FD_WORD(fd)] |= __FD_BIT(fd)))
#define FD_CLR(fd, s)   ((void)((s)->fds_bits[__FD_WORD(fd)] &= ~__FD_BIT(fd)))
#define FD_ISSET(fd, s) (((s)->fds_bits[__FD_WORD(fd)] & __FD_BIT(fd)) != 0)

int select(int n, fd_set *__restrict r, fd_set *__restrict w,
           fd_set *__restrict e, struct timeval *__restrict timeout);
int pselect(int n, fd_set *__restrict r, fd_set *__restrict w,
            fd_set *__restrict e, const struct timespec *__restrict timeout,
            const unsigned long long *__restrict sigmask);

#ifdef __cplusplus
}
#endif

#endif
