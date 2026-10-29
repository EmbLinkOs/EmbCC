/* <err.h> -- the BSD diagnostics every Unix has: the program's name, the
 * caller's message, and (err, warn) what errno says, on stderr; err and
 * errx then exit with the status given (src/posix/err.c). */
#ifndef _ERR_H
#define _ERR_H

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

void warn(const char *fmt, ...);
void warnx(const char *fmt, ...);
void vwarn(const char *fmt, va_list ap);
void vwarnx(const char *fmt, va_list ap);
_Noreturn void err(int status, const char *fmt, ...);
_Noreturn void errx(int status, const char *fmt, ...);
_Noreturn void verr(int status, const char *fmt, va_list ap);
_Noreturn void verrx(int status, const char *fmt, va_list ap);

#ifdef __cplusplus
}
#endif

#endif
