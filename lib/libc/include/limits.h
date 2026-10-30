/* <limits.h> — the C library's half.
 *
 * The protocol EmbCC's own limits.h documents: found first, this one
 * defines the limits that belong to the library, then reaches the
 * compiler's with #include_next for the ones that belong to the target's
 * type widths. Neither is complete alone, and neither duplicates the other.
 */
#ifndef _EMBLIBC_LIMITS_H_
#define _EMBLIBC_LIMITS_H_

/* _LIBC_LIMITS_H_ is the protocol's word for "the C library's half is
 * here", which the compiler's half reads -- and newlib's limits.h uses it
 * as its own guard. So it is defined, not tested at the top: where
 * another C library's limits.h came first (a build putting newlib's
 * headers ahead), that library's limits stand, and this file only passes
 * the chain on to the compiler's half, which holds INT_MAX. */
#ifndef _LIBC_LIMITS_H_
#define _LIBC_LIMITS_H_ 1
#define _EMBLIBC_LIMITS_OWN 1
#endif

#ifdef _EMBLIBC_LIMITS_OWN

/* POSIX-ish limits a hosted program expects to find here. Modest, and
 * honest about what this library actually supports. */
#define PATH_MAX   4096
#define NAME_MAX    255
#ifndef ARG_MAX                 /* a build may have its own, as glibc has none */
#define ARG_MAX    32768
#endif
#define OPEN_MAX     32
#define NGROUPS_MAX  32
#define SSIZE_MAX  __LONG_MAX__

/* wchar_t's range. It belongs here rather than being assumed, because its
 * signedness is a TARGET decision, not a C one: it is signed 32-bit on
 * x86-64 and unsigned 32-bit on aarch64, exactly as those ABIs say. The
 * compiler knows which and publishes it. */
#ifndef WCHAR_MIN           /* <stdint.h> or <wchar.h> may have said so */
#define WCHAR_MIN  __WCHAR_MIN__
#endif
#ifndef WCHAR_MAX
#define WCHAR_MAX  __WCHAR_MAX__
#endif

/* The longest multibyte character this library writes: a UTF-8 sequence
 * is at most four bytes. The compiler's half says 1, for a library with
 * no multibyte encoding; with that, `char buf[MB_LEN_MAX]` handed to
 * wctomb was overrun by the first character past U+007F. */
#define MB_LEN_MAX 4

#endif /* _EMBLIBC_LIMITS_OWN */

/* the compiler's half: CHAR_BIT, INT_MAX and the rest, from the target */
#include_next <limits.h>

#endif
