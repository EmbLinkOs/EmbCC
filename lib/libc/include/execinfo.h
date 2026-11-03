/* <execinfo.h> -- the call stack, as GNU's backtrace gives it
 * (src/posix/execinfo.c): the return addresses from the caller out, and
 * those addresses as text. */
#ifndef _EXECINFO_H
#define _EXECINFO_H

#ifdef __cplusplus
extern "C" {
#endif

int    backtrace(void **buffer, int size);
char **backtrace_symbols(void *const *buffer, int size);
void   backtrace_symbols_fd(void *const *buffer, int size, int fd);

#ifdef __cplusplus
}
#endif

#endif
