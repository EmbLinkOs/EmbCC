/* <sys/random.h> -- bytes from the OS's entropy */
#ifndef _SYS_RANDOM_H
#define _SYS_RANDOM_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GRND_NONBLOCK 1
#define GRND_RANDOM   2

ssize_t getrandom(void *buf, size_t n, unsigned flags);
int getentropy(void *buf, size_t n);

#ifdef __cplusplus
}
#endif

#endif
