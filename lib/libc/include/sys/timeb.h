/* <sys/timeb.h> -- the old ftime (src/time/getdate.c). */
#ifndef _SYS_TIMEB_H
#define _SYS_TIMEB_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct timeb {
    time_t time;
    unsigned short millitm;
    short timezone;
    short dstflag;
};

int ftime(struct timeb *tb);

#ifdef __cplusplus
}
#endif

#endif
