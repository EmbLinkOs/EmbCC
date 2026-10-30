/* <sys/timerfd.h> -- Linux's timers as descriptors (src/posix/linuxapi.c). */
#ifndef _SYS_TIMERFD_H
#define _SYS_TIMERFD_H

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TFD_CLOEXEC  02000000
#define TFD_NONBLOCK 04000
#define TFD_TIMER_ABSTIME       1
#define TFD_TIMER_CANCEL_ON_SET 2

int timerfd_create(int clock, int flags);
int timerfd_settime(int fd, int flags, const struct itimerspec *value,
                    struct itimerspec *old);
int timerfd_gettime(int fd, struct itimerspec *value);

#ifdef __cplusplus
}
#endif

#endif
