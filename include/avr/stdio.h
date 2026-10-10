/* EmbCC's <stdio.h> for AVR: the C library's, and avr-libc's formatted
 * output with the format string in flash (PSTR):
 *
 *   char line[32];
 *   snprintf_P(line, sizeof line, PSTR("t=%u\n"), ticks);
 *
 * On AVR the library (lib/avr) has sprintf, snprintf, vsprintf,
 * vsnprintf and these four, with avr-libc's conversions (%S for a string
 * in flash; floating point prints "?", as avr-libc's default does). It
 * has no streams: printf, puts, FILE and fdevopen are declared by the C
 * library's header and not in the AVR library. */
#include_next <stdio.h>

#ifndef _EMB_AVR_STDIO_H
#define _EMB_AVR_STDIO_H

#include <stdarg.h>
#include <stddef.h>

int sprintf_P(char *s, const char *fmt_P, ...);
int snprintf_P(char *s, size_t n, const char *fmt_P, ...);
int vsprintf_P(char *s, const char *fmt_P, va_list ap);
int vsnprintf_P(char *s, size_t n, const char *fmt_P, va_list ap);

#endif
