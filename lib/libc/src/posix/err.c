/* <err.h>'s diagnostics, and the program's name they begin with.
 *
 * The name is what startup code records from argv[0]: the whole of it in
 * program_invocation_name, and the part after its last '/' in
 * program_invocation_short_name, which is what a message starts with --
 * "ls: cannot access", not "/bin/ls: cannot access". Before startup has
 * set them, or on a target whose startup has no argv, they are NULL and
 * a message goes without the prefix rather than with a made-up one. */
#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

__attribute__((weak)) char *program_invocation_name, *program_invocation_short_name;

__attribute__((weak)) const char *getprogname(void) { return program_invocation_short_name; }

__attribute__((weak)) void setprogname(const char *name)
{
    const char *slash = strrchr(name, '/');
    program_invocation_short_name = (char *)(slash ? slash + 1 : name);
}

/* One lock for the whole line: a diagnostic split across threads reads
 * as two different messages. */
static void say(const char *fmt, va_list ap, int with_errno)
{
    int e = errno;
    flockfile(stderr);
    if (program_invocation_short_name)
        fprintf(stderr, "%s: ", program_invocation_short_name);
    if (fmt) {
        vfprintf(stderr, fmt, ap);
        if (with_errno)
            fputs(": ", stderr);
    }
    if (with_errno)
        fputs(strerror(e), stderr);
    fputc('\n', stderr);
    funlockfile(stderr);
}

__attribute__((weak)) void vwarn(const char *fmt, va_list ap)  { say(fmt, ap, 1); }
__attribute__((weak)) void vwarnx(const char *fmt, va_list ap) { say(fmt, ap, 0); }

__attribute__((weak)) void verr(int status, const char *fmt, va_list ap)
{
    say(fmt, ap, 1);
    exit(status);
}

__attribute__((weak)) void verrx(int status, const char *fmt, va_list ap)
{
    say(fmt, ap, 0);
    exit(status);
}

__attribute__((weak)) void warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vwarn(fmt, ap);
    va_end(ap);
}

__attribute__((weak)) void warnx(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vwarnx(fmt, ap);
    va_end(ap);
}

__attribute__((weak)) void err(int status, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    verr(status, fmt, ap);
}

__attribute__((weak)) void errx(int status, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    verrx(status, fmt, ap);
}

/* ---- <error.h>, GNU's ------------------------------------------------------------
 *
 * The program's whole name (program_invocation_name, as glibc's error
 * prints it), the message, ": " and the errno's words when errnum is
 * not 0; stdout flushed first so the two streams stay in order, and an
 * exit when status is not 0. */
#include <error.h>

__attribute__((weak)) void (*error_print_progname)(void);
__attribute__((weak)) unsigned error_message_count;
__attribute__((weak)) int error_one_per_line;

static void report(int status, int errnum, const char *file, unsigned line,
                   const char *fmt, va_list ap)
{
    fflush(stdout);
    flockfile(stderr);
    if (error_print_progname)
        error_print_progname();
    else if (program_invocation_name)
        fprintf(stderr, file ? "%s:" : "%s: ", program_invocation_name);
    if (file)
        fprintf(stderr, "%s:%u: ", file, line);
    vfprintf(stderr, fmt, ap);
    if (errnum)
        fprintf(stderr, ": %s", strerror(errnum));
    fputc('\n', stderr);
    funlockfile(stderr);
    error_message_count++;
    if (status)
        exit(status);
}

__attribute__((weak)) void error(int status, int errnum, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    report(status, errnum, 0, 0, fmt, ap);
    va_end(ap);
}

__attribute__((weak)) void error_at_line(int status, int errnum, const char *file,
                                         unsigned line, const char *fmt, ...)
{
    static const char *last_file;
    static unsigned last_line;
    if (error_one_per_line && file && last_file && last_line == line &&
        (last_file == file || !strcmp(last_file, file)))
        return;
    last_file = file;
    last_line = line;
    va_list ap;
    va_start(ap, fmt);
    report(status, errnum, file, line, fmt, ap);
    va_end(ap);
}
