/* GNU's argz and envz vectors: strings back to back in one malloc'd
 * buffer, each with its NUL. The splitting calls keep glibc's answers to
 * the edge cases -- a leading or doubled separator makes no entry, a
 * trailing one an empty last entry -- since programs written against
 * glibc see those. */
#define _GNU_SOURCE 1
#include <argz.h>
#include <envz.h>
#include <stdlib.h>
#include <string.h>

#include "posix.h"

WEAK error_t argz_create(char *const argv[], char **argz, size_t *len)
{
    size_t n = 0;
    for (char *const *a = argv; *a; a++)
        n += strlen(*a) + 1;
    *len = n;
    if (!n) {
        *argz = 0;
        return 0;
    }
    char *p = malloc(n);
    if (!p)
        return ENOMEM;
    *argz = p;
    for (char *const *a = argv; *a; a++)
        p = stpcpy(p, *a) + 1;
    return 0;
}

/* s split at sep into dst (room for strlen(s) + 1): how many bytes */
static size_t split(char *dst, const char *s, int sep, const char *start)
{
    char *w = dst;
    do {
        if (*s == sep) {
            if (w > start && w[-1])
                *w++ = 0;
        } else {
            *w++ = *s;
        }
    } while (*s++);
    return (size_t)(w - dst);
}

WEAK error_t argz_create_sep(const char *s, int sep, char **argz, size_t *len)
{
    size_t n = strlen(s) + 1;
    *argz = 0;
    *len = 0;
    if (n == 1)
        return 0;
    char *p = malloc(n);
    if (!p)
        return ENOMEM;
    *argz = p;
    *len = split(p, s, sep, p);
    return 0;
}

WEAK error_t argz_add_sep(char **argz, size_t *len, const char *s, int sep)
{
    size_t n = strlen(s) + 1;
    if (n == 1)
        return 0;
    char *p = realloc(*argz, *len + n);
    if (!p)
        return ENOMEM;
    *argz = p;
    *len += split(p + *len, s, sep, p);
    return 0;
}

WEAK size_t argz_count(const char *argz, size_t len)
{
    size_t n = 0;
    while (len) {
        size_t k = strlen(argz) + 1;
        argz += k;
        len -= k;
        n++;
    }
    return n;
}

WEAK void argz_extract(const char *argz, size_t len, char **argv)
{
    while (len) {
        size_t k = strlen(argz) + 1;
        *argv++ = (char *)argz;
        argz += k;
        len -= k;
    }
    *argv = 0;
}

/* every NUL but the last becomes sep */
WEAK void argz_stringify(char *argz, size_t len, int sep)
{
    while (len) {
        size_t k = strnlen(argz, len);
        argz += k;
        len -= k;
        if (len-- <= 1)
            break;
        *argz++ = (char)sep;
    }
}

WEAK error_t argz_append(char **argz, size_t *len, const char *buf, size_t buf_len)
{
    char *p = realloc(*argz, *len + buf_len);
    if (!p && *len + buf_len)
        return ENOMEM;
    if (buf_len)
        memcpy(p + *len, buf, buf_len);
    *argz = p;
    *len += buf_len;
    return 0;
}

WEAK error_t argz_add(char **argz, size_t *len, const char *s)
{
    return argz_append(argz, len, s, strlen(s) + 1);
}

WEAK void argz_delete(char **argz, size_t *len, char *entry)
{
    if (!entry)
        return;
    size_t k = strlen(entry) + 1;
    *len -= k;
    memmove(entry, entry + k, *len - (size_t)(entry - *argz));
    if (!*len) {
        free(*argz);
        *argz = 0;
    }
}

/* entry before the entry that holds before; at the end when before is
 * null */
WEAK error_t argz_insert(char **argz, size_t *len, char *before, const char *entry)
{
    if (!before)
        return argz_add(argz, len, entry);
    if (before < *argz || before >= *argz + *len)
        return EINVAL;
    while (before > *argz && before[-1])
        before--;
    size_t at = (size_t)(before - *argz), k = strlen(entry) + 1;
    char *p = realloc(*argz, *len + k);
    if (!p)
        return ENOMEM;
    memmove(p + at + k, p + at, *len - at);
    memcpy(p + at, entry, k);
    *argz = p;
    *len += k;
    return 0;
}

WEAK char *argz_next(const char *argz, size_t len, const char *entry)
{
    if (!entry)
        return len ? (char *)argz : 0;
    if (entry < argz + len)
        entry = strchr(entry, 0) + 1;
    return entry >= argz + len ? 0 : (char *)entry;
}

/* every str in every entry becomes with; count, when given, counts the
 * entries that changed (glibc's count, not the replacements) */
WEAK error_t argz_replace(char **argz, size_t *len, const char *str, const char *with,
                          unsigned *count)
{
    if (!str || !*str)
        return 0;
    size_t sl = strlen(str), wl = strlen(with);
    char *out = 0;
    size_t olen = 0;
    int changed = 0;
    for (char *e = argz_next(*argz, *len, 0); e; e = argz_next(*argz, *len, e)) {
        size_t n = 0;
        for (char *f = strstr(e, str); f; f = strstr(f + sl, str))
            n++;
        if (!n) {
            if (argz_add(&out, &olen, e))
                goto nomem;
            continue;
        }
        changed = 1;
        if (count)
            (*count)++;            /* glibc's count: entries changed */
        size_t el = strlen(e) + n * wl - n * sl;
        char *t = malloc(el + 1), *w = t;
        if (!t)
            goto nomem;
        const char *r = e;
        for (char *f = strstr(r, str); f; f = strstr(r, str)) {
            memcpy(w, r, (size_t)(f - r));
            w += f - r;
            memcpy(w, with, wl);
            w += wl;
            r = f + sl;
        }
        strcpy(w, r);
        error_t err = argz_add(&out, &olen, t);
        free(t);
        if (err)
            goto nomem;
    }
    if (changed) {
        free(*argz);
        *argz = out;
        *len = olen;
    } else {
        free(out);
    }
    return 0;
nomem:
    free(out);
    return ENOMEM;
}

/* ---- envz ---------------------------------------------------------------------------- */

/* the entry named name (name may itself go on with "=value") */
WEAK char *envz_entry(const char *envz, size_t len, const char *name)
{
    while (len) {
        const char *p = name, *entry = envz;
        while (len && *p == *envz && *p && *p != '=') {
            p++;
            envz++;
            len--;
        }
        if ((*envz == 0 || *envz == '=') && (*p == 0 || *p == '='))
            return (char *)entry;
        while (len && *envz) {
            envz++;
            len--;
        }
        if (len) {
            envz++;
            len--;
        }
    }
    return 0;
}

WEAK char *envz_get(const char *envz, size_t len, const char *name)
{
    char *e = envz_entry(envz, len, name);
    if (!e)
        return 0;
    while (*e && *e != '=')
        e++;
    return *e ? e + 1 : 0;
}

WEAK void envz_remove(char **envz, size_t *len, const char *name)
{
    char *e = envz_entry(*envz, *len, name);
    if (e)
        argz_delete(envz, len, e);
}

WEAK error_t envz_add(char **envz, size_t *len, const char *name, const char *value)
{
    envz_remove(envz, len, name);
    if (!value)
        return argz_add(envz, len, name);
    size_t nl = strlen(name), vl = strlen(value);
    char *e = malloc(nl + 1 + vl + 1);
    if (!e)
        return ENOMEM;
    memcpy(e, name, nl);
    e[nl] = '=';
    memcpy(e + nl + 1, value, vl + 1);
    error_t err = argz_append(envz, len, e, nl + vl + 2);
    free(e);
    return err;
}

WEAK error_t envz_merge(char **envz, size_t *len, const char *envz2, size_t len2,
                        int override)
{
    error_t err = 0;
    while (len2 && !err) {
        char *old = envz_entry(*envz, *len, envz2);
        size_t k = strlen(envz2) + 1;
        if (!old) {
            err = argz_append(envz, len, envz2, k);
        } else if (override) {
            argz_delete(envz, len, old);
            err = argz_append(envz, len, envz2, k);
        }
        envz2 += k;
        len2 -= k;
    }
    return err;
}

/* drop the entries with no value */
WEAK void envz_strip(char **envz, size_t *len)
{
    char *e = *envz;
    size_t left = *len;
    while (left) {
        size_t k = strlen(e) + 1;
        left -= k;
        if (!strchr(e, '='))
            memmove(e, e + k, left);
        else
            e += k;
    }
    *len = (size_t)(e - *envz);
}
