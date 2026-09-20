/* getenv, and the environment it reads.
 *
 * `environ` lives here rather than in the backend because the environment
 * is not an OS primitive: it is a NUL-terminated "NAME=value" vector that
 * the program's startup code publishes, and every target this library runs
 * on passes it the same way. A backend that has no environment leaves the
 * pointer null and getenv correctly finds nothing.
 *
 * setenv/putenv are absent on purpose. They are POSIX, not C11, and doing
 * them properly means owning the vector's storage -- which of the strings
 * were malloc'd, which came from the startup vector -- for a feature no
 * program in this tree uses. An honest absence beats a leak.
 */
#include <stdlib.h>
#include <string.h>

char **environ = 0;

char *getenv(const char *name)
{
    if (!environ || !name)
        return 0;
    size_t n = strlen(name);
    if (n == 0)
        return 0;
    for (char **p = environ; *p; p++) {
        /* The '=' test is not optional: without it "PATHX=..." answers a
         * lookup of "PATH" with "X=...". */
        if (strncmp(*p, name, n) == 0 && (*p)[n] == '=')
            return *p + n + 1;
    }
    return 0;
}

/* §7.22.4.8: system(NULL) reports whether a command processor exists.
 * There is none here, so it reports 0 -- and a real command string gets
 * -1 rather than a silent success that ran nothing. */
int system(const char *cmd)
{
    (void)cmd;
    return cmd ? -1 : 0;
}
