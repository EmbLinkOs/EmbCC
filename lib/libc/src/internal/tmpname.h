/* The randomness behind every name this library makes up for a temporary
 * file: tmpnam's and tmpfile's (stdio/stdio.c) and mkstemp's family
 * (stdlib/tempfile.c).
 *
 * A name another program can predict is one it can create first -- as a
 * link to somewhere else -- so the bits come from the OS's entropy. A
 * target with none mixes the clock in instead, and a count keeps one
 * program's names apart either way. errno is left as it was: a caller
 * asking for a name has not failed because the entropy source did. */
#ifndef EMBLIBC_TMPNAME_H
#define EMBLIBC_TMPNAME_H

unsigned long long __tmp_bits(void);

#endif
