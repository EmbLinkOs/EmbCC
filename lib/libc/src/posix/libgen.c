/* <libgen.h>: basename and dirname, POSIX's -- which may write into the
 * string they are given, and do: a trailing slash is cut off in place.
 * That is why they take a char * and not a const char *, and why the
 * GNU basename of <string.h>, which never writes, is a different
 * function under the same name. This library has only this one. */
#include <libgen.h>
#include <string.h>

/* "usr/lib/" -> "lib", "/" -> "/", "" and NULL -> "." */
char *basename(char *s)
{
    if (!s || !*s)
        return (char *)".";
    size_t n = strlen(s);
    while (n > 1 && s[n - 1] == '/')
        s[--n] = 0;
    if (n == 1 && s[0] == '/')
        return s;
    char *slash = strrchr(s, '/');
    return slash ? slash + 1 : s;
}

/* "usr/lib/" -> "usr", "usr" -> ".", "/usr" -> "/". A path that begins
 * with EXACTLY two slashes keeps both when only the root is left -- "//"
 * and "//a" give "//" -- because POSIX lets "//" name something other
 * than "/" (XBD 4.13), and glibc answers so. */
char *dirname(char *s)
{
    if (!s || !*s)
        return (char *)".";
    size_t n = strlen(s);
    while (n > 1 && s[n - 1] == '/')
        n--;
    while (n > 0 && s[n - 1] != '/')
        n--;
    if (n == 0)
        return (char *)".";
    while (n > 1 && s[n - 1] == '/')
        n--;
    if (n == 1 && s[0] == '/' && s[1] == '/' && s[2] != '/')
        n = 2;
    s[n] = 0;
    return s;
}
