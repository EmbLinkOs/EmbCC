/* <sys/appio.h> -- XC32's debugger application I/O (APPIN/APPOUT over an
 * MPLAB debugger), as EmbCC gives it: with __APPIO_DEBUG undefined, which
 * is a release build's case, every DB macro is nothing, as in XC32. With
 * it defined, the I/O needs XC32's library, which EmbCC has no part of.
 */
#ifndef _SYS_APPIO_H
#define _SYS_APPIO_H

#ifdef __APPIO_DEBUG
#error "__APPIO_DEBUG: debugger application I/O needs XC32's libpic32, which EmbCC does not provide; print through a UART instead"
#endif

#define DBINIT()
#define DBPRINTF(...)
#define DBPUTS(s)
#define DBPUTC(c)
#define DBGETC(c)
#define DBGETWORD(w)
#define DBSCANF(...)
#define DBGETS(s, n)
#define DBPUTWORD(w)

#endif
