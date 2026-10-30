/* <sys/param.h> -- BSD's system constants and a few macros */
#ifndef _SYS_PARAM_H
#define _SYS_PARAM_H

#include <limits.h>
#include <endian.h>

#define MAXPATHLEN     PATH_MAX
#define MAXHOSTNAMELEN 64
#define MAXSYMLINKS    40
#define NOFILE         256
#define NGROUPS        NGROUPS_MAX
#define NBBY           8
#define DEV_BSIZE      512

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#define howmany(x, y)  (((x) + ((y) - 1)) / (y))
#define roundup(x, y)  ((((x) + ((y) - 1)) / (y)) * (y))
#define powerof2(x)    ((((x) - 1) & (x)) == 0)
#define nitems(a)      (sizeof(a) / sizeof((a)[0]))
#define setbit(a, i)   ((a)[(i) / NBBY] |= 1 << ((i) % NBBY))
#define clrbit(a, i)   ((a)[(i) / NBBY] &= ~(1 << ((i) % NBBY)))
#define isset(a, i)    ((a)[(i) / NBBY] & (1 << ((i) % NBBY)))
#define isclr(a, i)    (((a)[(i) / NBBY] & (1 << ((i) % NBBY))) == 0)

#endif
