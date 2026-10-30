/* <features.h> -- which names beyond ISO C the C headers declare.
 *
 * <stdio.h> is C's header, and C reserves to the program every name it
 * does not itself define there: a strictly conforming program may have
 * its own `getline` or `fileno`, and a header that declared POSIX's in
 * the same scope would reject that program for nothing it did wrong.
 * POSIX's answer is the feature-test macros, and this is where they are
 * read, once, for every header that has both kinds of name:
 *
 *   __EMB_POSIX   POSIX.1-2024, XSI included. On unless the program
 *                 asked for ISO C alone (-std=c11 and no feature macro);
 *                 any of _POSIX_C_SOURCE, _XOPEN_SOURCE, _DEFAULT_SOURCE,
 *                 _BSD_SOURCE or _GNU_SOURCE turns it back on.
 *   __EMB_GNU     what only GNU and the BSDs have -- strchrnul, memrchr,
 *                 getopt_long, clearenv. On by default as well, and on
 *                 under _GNU_SOURCE, _DEFAULT_SOURCE or _BSD_SOURCE. A
 *                 program that names a POSIX version and nothing more
 *                 (-D_POSIX_C_SOURCE=200809L) gets POSIX without these,
 *                 as with glibc: it was written expecting to, and may
 *                 define a `strchrnul` of its own.
 *
 * A header that is POSIX's own (<unistd.h>, <fcntl.h>, ...) declares
 * what it declares without asking: including it is the request.
 *
 * Asking for an OLDER POSIX (_POSIX_C_SOURCE=200112L) does not hide what
 * came later. glibc does that, and it buys a program nothing but a name
 * it then has to declare itself.
 */
#ifndef _FEATURES_H
#define _FEATURES_H

#if defined(_GNU_SOURCE) || defined(_DEFAULT_SOURCE) || \
    defined(_BSD_SOURCE) || \
    (!defined(__STRICT_ANSI__) && !defined(_POSIX_C_SOURCE) && \
     !defined(_POSIX_SOURCE) && !defined(_XOPEN_SOURCE))
#define __EMB_GNU 1
#endif

#if defined(__EMB_GNU) || defined(_POSIX_C_SOURCE) || \
    defined(_POSIX_SOURCE) || defined(_XOPEN_SOURCE)
#define __EMB_POSIX 1
#endif

/* The version this library implements, for a program that asks with
 * #if _POSIX_VERSION >= ... (from <unistd.h>, where POSIX puts it). */
#define __EMB_POSIX_VERSION 202405L

#endif
