/* getenv, setenv, unsetenv, putenv and clearenv, and the environment
 * they share.
 *
 * `environ` lives here rather than in the backend because the environment
 * is not an OS primitive: it is a NUL-terminated "NAME=value" vector that
 * the program's startup code publishes, and every target this library runs
 * on passes it the same way. A backend that has no environment leaves the
 * pointer null and getenv correctly finds nothing.
 *
 * ---- who owns what ---------------------------------------------------------
 *
 * Changing the environment means owning storage, and three kinds of
 * string end up in the one vector:
 *
 *   the startup vector's   on the initial stack; never freed
 *   setenv's               malloc'd here; freed when replaced or removed
 *   putenv's               the CALLER's, by POSIX's definition -- a later
 *                          change to that string changes the environment
 *                          -- so never copied and never freed
 *
 * Only the second kind is freed, so this file keeps the list of the
 * strings it made. The vector itself is copied into malloc'd storage the
 * first time anything changes, and again if the program has since
 * pointed `environ` somewhere else -- an assignment to environ is
 * legal, and writing into a vector the program owns would be writing
 * into memory this file was never given.
 *
 * One lock for all of it. POSIX does not make setenv thread-safe against
 * getenv (the pointer getenv returned can be freed under a caller's feet
 * and no lock fixes that), but two setenvs at once must not lose an entry.
 */
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "../internal/lock.h"

char **environ = 0;

static __lock_t env_lock = LOCK_INIT;
static char **own_vec;          /* the vector this file allocated, if any */
static size_t own_cap;          /* its capacity, in entries */
static char **made;             /* the strings setenv malloc'd */
static size_t nmade, made_cap;

/* "NAME=" matches entry e, for a name of n bytes. The '=' test is not
 * optional: without it "PATHX=..." answers a lookup of "PATH". */
static int names(const char *e, const char *name, size_t n)
{
    return strncmp(e, name, n) == 0 && e[n] == '=';
}

char *getenv(const char *name)
{
    if (!environ || !name)
        return 0;
    size_t n = strlen(name);
    if (n == 0)
        return 0;
    for (char **p = environ; *p; p++)
        if (names(*p, name, n))
            return *p + n + 1;
    return 0;
}

/* A name POSIX lets setenv and unsetenv take: not null, not empty, and
 * without the '=' that would make "A=B" set A to "B=value". */
static int bad_name(const char *name)
{
    return !name || !*name || strchr(name, '=');
}

static void release(char *s)
{
    for (size_t i = 0; i < nmade; i++)
        if (made[i] == s) {
            free(s);
            made[i] = made[--nmade];
            return;
        }
}

static size_t count(void)
{
    size_t n = 0;
    if (environ)
        while (environ[n])
            n++;
    return n;
}

/* A vector this file may write into, with room for one more entry and
 * its terminator. -1 when there is no memory for one. */
static int writable(void)
{
    size_t n = count();
    if (environ && environ == own_vec && n + 2 <= own_cap)
        return 0;
    size_t cap = n + 2 > 16 ? 2 * (n + 2) : 16;
    char **v = malloc(cap * sizeof *v);
    if (!v)
        return -1;
    if (n)
        memcpy(v, environ, n * sizeof *v);
    v[n] = 0;
    if (own_vec && environ == own_vec)
        free(own_vec);
    own_vec = environ = v;
    own_cap = cap;
    return 0;
}

/* Put entry s, named by its first n bytes, in place of any entry of that
 * name or at the end. */
static int put(char *s, size_t n)
{
    if (writable() < 0)
        return -1;
    size_t i = 0;
    for (; environ[i]; i++)
        if (names(environ[i], s, n)) {
            char *old = environ[i];
            environ[i] = s;
            if (old != s)
                release(old);
            return 0;
        }
    environ[i] = s;
    environ[i + 1] = 0;
    return 0;
}

__attribute__((weak)) int setenv(const char *name, const char *value, int overwrite)
{
    if (bad_name(name)) {
        errno = EINVAL;
        return -1;
    }
    __lock(&env_lock);
    int r = 0;
    if (!overwrite && getenv(name))
        goto out;
    size_t n = strlen(name), v = strlen(value);
    char *s = malloc(n + v + 2);
    if (!s) {
        r = -1;
        goto out;
    }
    if (nmade == made_cap) {
        size_t cap = made_cap ? 2 * made_cap : 16;
        char **m = realloc(made, cap * sizeof *m);
        if (!m) {
            free(s);
            r = -1;
            goto out;
        }
        made = m;
        made_cap = cap;
    }
    memcpy(s, name, n);
    s[n] = '=';
    memcpy(s + n + 1, value, v + 1);
    made[nmade++] = s;
    if (put(s, n) < 0) {
        release(s);
        r = -1;
    }
out:
    __unlock(&env_lock);
    return r;
}

/* Every entry of the name goes, not the first: a startup vector may hold
 * two, and leaving the second would make the variable come back. */
__attribute__((weak)) int unsetenv(const char *name)
{
    if (bad_name(name)) {
        errno = EINVAL;
        return -1;
    }
    __lock(&env_lock);
    size_t n = strlen(name);
    int r = 0;
    for (size_t i = 0; environ && environ[i]; ) {
        if (!names(environ[i], name, n)) {
            i++;
            continue;
        }
        if (writable() < 0) {
            r = -1;
            break;
        }
        char *old = environ[i];
        size_t k = i;
        do
            environ[k] = environ[k + 1];
        while (environ[k++]);
        release(old);
    }
    __unlock(&env_lock);
    return r;
}

/* The caller's string itself becomes the entry. One without an '=' is
 * glibc's way of asking for an unsetenv, and is taken that way. */
__attribute__((weak)) int putenv(char *s)
{
    char *eq = s ? strchr(s, '=') : 0;
    if (!eq)
        return s && *s ? unsetenv(s) : (errno = EINVAL, -1);
    if (eq == s) {
        errno = EINVAL;
        return -1;
    }
    __lock(&env_lock);
    int r = put(s, (size_t)(eq - s));
    __unlock(&env_lock);
    return r;
}

__attribute__((weak)) int clearenv(void)
{
    __lock(&env_lock);
    while (nmade)
        free(made[--nmade]);
    if (own_vec && environ == own_vec)
        free(own_vec);
    own_vec = 0;
    own_cap = 0;
    environ = 0;
    __unlock(&env_lock);
    return 0;
}

/* system() is src/posix/proc.c's: /bin/sh, through posix_spawn. */
