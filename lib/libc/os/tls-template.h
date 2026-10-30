/* The thread-local template, as a program's startup hands it over.
 *
 * Where no loader tells the library where PT_TLS is (EmbLinkOS passes no
 * auxiliary vector), the code that built the first thread's block is the
 * one that knows: crt0 found the template's geometry, gave the first
 * thread its copy and installed the thread pointer. It records what it
 * found here, AFTER the pointer is in place, and from then on:
 *
 *   - errno is per thread (stdlib/errno.c), and
 *   - the backend gives each new thread a copy of the same template
 *     before the thread runs anything that could touch one.
 *
 * memsz == 0 means nobody said: either the program has no TLS or its
 * startup predates this hand-over. Both read the same way -- one errno,
 * as before, and threads with no block, which nothing then reads.
 *
 * Built with -D__EMBLIBC_TLS_TEMPLATE; nothing includes this otherwise.
 * The layout is a contract with that OS's crt0, which keeps its own copy:
 * grow both together. */
#ifndef _EMBLIBC_OS_TLS_TEMPLATE_H
#define _EMBLIBC_OS_TLS_TEMPLATE_H

struct __emblibc_tls {
    const void *image;     /* .tdata's initialisers: filesz bytes */
    unsigned long filesz;
    unsigned long memsz;   /* .tdata + .tbss */
    unsigned long align;   /* PT_TLS's p_align, exactly */
};

extern struct __emblibc_tls __emblibc_tls;

#endif
