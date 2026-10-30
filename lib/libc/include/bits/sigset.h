/* sigset_t, for the headers that use it whatever the program asked of
 * <signal.h> -- <spawn.h> names it in posix_spawnattr_t, and is POSIX's
 * own header, which declares what it declares (see <features.h>).
 * Bit (sig - 1) is signal sig, Linux's 64. */
#ifndef _BITS_SIGSET_H
#define _BITS_SIGSET_H

typedef unsigned long long sigset_t;

#endif
