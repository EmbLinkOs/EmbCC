/* <error.h> -- GNU's error reporting (src/posix/err.c): the program's
 * name, the message, the errno's words, and an exit when status is not
 * zero. */
#ifndef _ERROR_H
#define _ERROR_H

#ifdef __cplusplus
extern "C" {
#endif

void error(int status, int errnum, const char *fmt, ...);
void error_at_line(int status, int errnum, const char *file, unsigned line,
                   const char *fmt, ...);

extern void (*error_print_progname)(void);
extern unsigned error_message_count;
extern int error_one_per_line;

#ifdef __cplusplus
}
#endif

#endif
