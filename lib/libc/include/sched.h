/* <sched.h> -- giving up the processor, and the scheduling policies'
 * names. The priority ranges are the policies' own definitions (Linux's),
 * not a question for the kernel. */
#ifndef _SCHED_H
#define _SCHED_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCHED_OTHER 0
#define SCHED_FIFO  1
#define SCHED_RR    2

struct sched_param { int sched_priority; };

int sched_yield(void);
int sched_get_priority_max(int policy);
int sched_get_priority_min(int policy);

#ifdef __cplusplus
}
#endif

#endif
