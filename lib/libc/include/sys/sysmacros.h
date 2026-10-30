/* <sys/sysmacros.h> -- a device number's major and minor, as glibc
 * encodes them */
#ifndef _SYS_SYSMACROS_H
#define _SYS_SYSMACROS_H

#include <sys/types.h>

#define major(d) ((unsigned)((((d) >> 32) & 0xfffff000u) | (((d) >> 8) & 0xfffu)))
#define minor(d) ((unsigned)((((d) >> 12) & 0xffffff00u) | ((d) & 0xffu)))
#define makedev(ma, mi) \
    ((((dev_t)(ma) & 0xfffff000u) << 32) | (((dev_t)(ma) & 0xfffu) << 8) | \
     (((dev_t)(mi) & 0xffffff00u) << 12) | ((dev_t)(mi) & 0xffu))

#endif
