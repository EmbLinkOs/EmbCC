/* <sys/types.h> -- POSIX's type names, as embedded code asks for them:
 * Microchip Harmony's toolchain_specifics.h includes it, and so does code
 * written against newlib. EmbCC's C library has no processes or files of
 * its own; these are the names, at newlib's widths, so that such code
 * compiles and its structures lay out as they did there.
 *
 * time_t and clock_t are <time.h>'s, declared once whichever header comes
 * first; ssize_t is the signed type of size_t's width.
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
typedef int pid_t;
typedef unsigned int mode_t;
typedef unsigned short uid_t;
typedef unsigned short gid_t;
typedef unsigned int id_t;
typedef short dev_t;
typedef unsigned short ino_t;
typedef unsigned short nlink_t;
typedef long blkcnt_t;
typedef long blksize_t;
typedef unsigned long useconds_t;
typedef long suseconds_t;
typedef long key_t;
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

#endif
