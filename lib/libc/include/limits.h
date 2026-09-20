/* <limits.h> — the C library's half.
 *
 * The protocol EmbCC's own limits.h documents: found first, this one
 * defines the limits that belong to the library, then reaches the
 * compiler's with #include_next for the ones that belong to the target's
 * type widths. Neither is complete alone, and neither duplicates the other.
 */
#ifndef _LIBC_LIMITS_H_
#define _LIBC_LIMITS_H_

/* POSIX-ish limits a hosted program expects to find here. Modest, and
 * honest about what this library actually supports. */
#define PATH_MAX   4096
#define NAME_MAX    255
#define ARG_MAX    32768
#define OPEN_MAX     32
#define NGROUPS_MAX  32
#define SSIZE_MAX  __LONG_MAX__

/* the compiler's half: CHAR_BIT, INT_MAX and the rest, from the target */
#include_next <limits.h>

#endif
