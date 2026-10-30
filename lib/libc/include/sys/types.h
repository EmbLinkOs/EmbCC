/* <sys/types.h> -- POSIX's type names.
 *
 * The widths are a hosted system's, on every target: a file's inode
 * number and its device are 64 bits, its owner 32, so that struct stat
 * says on EmbLinkOS what it says on Linux and a program's
 * `ino_t` does not wrap at 65536 files. (They were newlib's widths once,
 * 16 bits for most of them, from before this library had a <sys/stat.h>
 * to fill them in; embedded code that names the types does not depend
 * on their size.) off_t is a long: 64 bits on every target with a disk
 * big enough to need it.
 *
 * time_t and clock_t are <time.h>'s, declared once whichever header comes
 * first; so are ssize_t and off_t, <stdio.h>'s too.
 */
#ifndef _SYS_TYPES_H
#define _SYS_TYPES_H

#include <stddef.h>
#include <stdint.h>

#ifndef _TIME_T_DECLARED
#define _TIME_T_DECLARED
typedef long time_t;
#endif
#ifndef _CLOCK_T_DECLARED
#define _CLOCK_T_DECLARED
typedef long clock_t;
#endif

/* declared once, whichever of this, <stdio.h> and <unistd.h> is first */
#ifndef _SSIZE_T_DECLARED
#define _SSIZE_T_DECLARED
typedef __PTRDIFF_TYPE__ ssize_t;
#endif
#ifndef _OFF_T_DECLARED
#define _OFF_T_DECLARED
typedef long off_t;
#endif
#ifndef _OFF64_T_DECLARED
#define _OFF64_T_DECLARED
typedef long long off64_t;
#endif
#ifndef _PID_T_DECLARED
#define _PID_T_DECLARED
typedef int pid_t;
#endif
#ifndef _MODE_T_DECLARED
#define _MODE_T_DECLARED
typedef unsigned int mode_t;
#endif
#ifndef _UID_T_DECLARED
#define _UID_T_DECLARED
typedef unsigned int uid_t;
typedef unsigned int gid_t;
#endif
typedef unsigned int id_t;
typedef unsigned long long dev_t;
typedef unsigned long long ino_t;
typedef unsigned long nlink_t;
typedef long long blkcnt_t;
typedef long blksize_t;
typedef unsigned long long fsblkcnt_t;
typedef unsigned long long fsfilcnt_t;
typedef unsigned int useconds_t;
typedef long suseconds_t;
typedef int key_t;
#ifndef _CLOCKID_T_DECLARED
#define _CLOCKID_T_DECLARED
typedef int clockid_t;
#endif
#ifndef _TIMER_T_DECLARED
#define _TIMER_T_DECLARED
typedef void *timer_t;
#endif
typedef long daddr_t;
typedef char *caddr_t;

/* BSD's short names */
typedef unsigned char u_char;
typedef unsigned short u_short;
typedef unsigned int u_int;
typedef unsigned long u_long;
typedef unsigned short ushort;
typedef unsigned int uint;
typedef unsigned long ulong;
typedef uint8_t u_int8_t;
typedef uint16_t u_int16_t;
typedef uint32_t u_int32_t;
typedef uint64_t u_int64_t;
typedef int64_t quad_t;
typedef uint64_t u_quad_t;

#endif
