/* <sys/times.h> -- processor time, in clock ticks */
#ifndef _SYS_TIMES_H
#define _SYS_TIMES_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct tms { clock_t tms_utime, tms_stime, tms_cutime, tms_cstime; };

clock_t times(struct tms *t);

#ifdef __cplusplus
}
#endif

#endif
