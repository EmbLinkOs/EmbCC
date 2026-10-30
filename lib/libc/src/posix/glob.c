/* <glob.h>: a pattern a component at a time. A component with no magic
 * is taken as it is (its backslashes removed); one with magic is matched
 * by fnmatch against the names in the directory so far, which is the
 * only place glob reads a directory. Braces and a leading tilde are
 * GNU's and are expanded first, into whole patterns. */
#define _GNU_SOURCE 1
#include <glob.h>
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "posix.h"

struct ctx {
    int flags;
    int (*errfunc)(const char *, int);
    glob_t *g;
    char **v;                  /* the matches so far, this call's */
    size_t n, cap;
    int aborted, nospace;
};

/* magic: an unescaped * or ?, or a [ that a ] closes, as glibc decides */
static int magic(const char *s, size_t n, int noescape)
{
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\\' && !noescape && i + 1 < n) {
            i++;
            continue;
        }
        if (s[i] == '*' || s[i] == '?')
            return 1;
        if (s[i] == '[')
            for (size_t j = i + 1; j < n; j++)
                if (s[j] == ']' && j > i + 1)
                    return 1;
    }
    return 0;
}

static int add(struct ctx *c, char *path)
{
    if (!path) {
        c->nospace = 1;
        return -1;
    }
    if (c->n == c->cap) {
        size_t cap = c->cap ? c->cap * 2 : 16;
        char **v = realloc(c->v, cap * sizeof *v);
        if (!v) {
            free(path);
            c->nospace = 1;
            return -1;
        }
        c->v = v;
        c->cap = cap;
    }
    c->v[c->n++] = path;
    return 0;
}

static int stat_of(struct ctx *c, const char *p, struct stat *st, int follow)
{
    if (c->flags & GLOB_ALTDIRFUNC)
        return follow ? c->g->gl_stat(p, st) : c->g->gl_lstat(p, st);
    return follow ? stat(p, st) : lstat(p, st);
}

static int is_dir(struct ctx *c, const char *p)
{
    struct stat st;
    return stat_of(c, p, &st, 1) == 0 && S_ISDIR(st.st_mode);
}

/* base followed by n bytes of name, with GLOB_MARK's slash if asked */
static char *join(const char *base, const char *name, size_t n, int slash)
{
    size_t b = strlen(base);
    char *p = malloc(b + n + 2);
    if (!p)
        return NULL;
    memcpy(p, base, b);
    memcpy(p + b, name, n);
    p[b + n] = 0;
    if (slash && (b + n == 0 || p[b + n - 1] != '/')) {
        p[b + n] = '/';
        p[b + n + 1] = 0;
    }
    return p;
}

static void unescape(char *s, int noescape)
{
    if (noescape)
        return;
    char *o = s;
    for (; *s; s++) {
        if (*s == '\\' && s[1])
            s++;
        *o++ = *s;
    }
    *o = 0;
}

static int found(struct ctx *c, char *path)
{
    int dir = (c->flags & (GLOB_MARK | GLOB_ONLYDIR)) && is_dir(c, path);
    if ((c->flags & GLOB_ONLYDIR) && !dir) {
        free(path);
        return 0;
    }
    if ((c->flags & GLOB_MARK) && dir) {
        char *m = join(path, "", 0, 1);
        free(path);
        path = m;
    }
    return add(c, path);
}

static void expand(struct ctx *c, const char *base, const char *rest)
{
    if (c->aborted || c->nospace)
        return;
    /* slashes belong to the base: "/a//b" keeps them as written */
    size_t sl = strspn(rest, "/");
    char *b = join(base, rest, sl, 0);
    if (!b) {
        c->nospace = 1;
        return;
    }
    rest += sl;
    if (!*rest) {
        struct stat st;
        if (*b && stat_of(c, b, &st, 0) == 0)
            found(c, b);
        else
            free(b);
        return;
    }
    const char *slash = strchr(rest, '/');
    size_t clen = slash ? (size_t)(slash - rest) : strlen(rest);
    int noesc = c->flags & GLOB_NOESCAPE;

    if (!magic(rest, clen, noesc)) {
        char *comp = join("", rest, clen, 0);
        if (!comp) {
            free(b);
            c->nospace = 1;
            return;
        }
        unescape(comp, noesc);
        char *nb = join(b, comp, strlen(comp), 0);
        free(comp);
        free(b);
        if (!nb) {
            c->nospace = 1;
            return;
        }
        expand(c, nb, rest + clen);
        free(nb);
        return;
    }

    char *pat = join("", rest, clen, 0);
    if (!pat) {
        free(b);
        c->nospace = 1;
        return;
    }
    const char *dir = *b ? b : ".";
    void *d = (c->flags & GLOB_ALTDIRFUNC) ? c->g->gl_opendir(dir)
                                           : (void *)opendir(dir);
    if (!d) {
        int e = errno;
        if (e != ENOENT && e != ENOTDIR &&
            ((c->errfunc && c->errfunc(dir, e)) || (c->flags & GLOB_ERR)))
            c->aborted = 1;
        free(pat);
        free(b);
        return;
    }
    int fl = (noesc ? FNM_NOESCAPE : 0) |
             ((c->flags & GLOB_PERIOD) ? 0 : FNM_PERIOD);
    struct dirent *e;
    for (;;) {
        e = (c->flags & GLOB_ALTDIRFUNC) ? c->g->gl_readdir(d)
                                         : readdir((DIR *)d);
        if (!e || c->aborted || c->nospace)
            break;
        const char *nm = e->d_name;
        /* GLOB_PERIOD lets a wildcard match a leading dot, but "." and
         * ".." still only by a pattern that starts with one */
        if ((c->flags & GLOB_PERIOD) && pat[0] != '.' &&
            (!strcmp(nm, ".") || !strcmp(nm, "..")))
            continue;
        if (fnmatch(pat, nm, fl) != 0)
            continue;
        char *p = join(b, nm, strlen(nm), 0);
        if (!p) {
            c->nospace = 1;
            break;
        }
        if (slash) {
            if (is_dir(c, p)) {
                char *sub = join(p, "/", 1, 0);
                if (sub)
                    expand(c, sub, slash + 1);
                else
                    c->nospace = 1;
                free(sub);
            }
            free(p);
        } else {
            found(c, p);
        }
    }
    if (c->flags & GLOB_ALTDIRFUNC)
        c->g->gl_closedir(d);
    else
        closedir((DIR *)d);
    free(pat);
    free(b);
}

static int by_name(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* one pattern, after braces: 1 if it matched or was kept, 0 if not */
static int glob_one(struct ctx *c, const char *pattern)
{
    size_t start = c->n;
    char *owned = NULL;
    const char *pat = pattern;
    if ((c->flags & (GLOB_TILDE | GLOB_TILDE_CHECK)) && pat[0] == '~') {
        const char *end = strchr(pat, '/');
        size_t ul = end ? (size_t)(end - pat - 1) : strlen(pat + 1);
        const char *home = NULL;
        if (ul == 0) {
            home = getenv("HOME");
            if (!home) {
                struct passwd *pw = getpwuid(getuid());
                home = pw ? pw->pw_dir : NULL;
            }
        } else {
            char user[256];
            if (ul < sizeof user) {
                memcpy(user, pat + 1, ul);
                user[ul] = 0;
                struct passwd *pw = getpwnam(user);
                home = pw ? pw->pw_dir : NULL;
            }
        }
        if (home) {
            owned = join(home, end ? end : "", end ? strlen(end) : 0, 0);
            if (!owned) {
                c->nospace = 1;
                return 0;
            }
            pat = owned;
        } else if (c->flags & GLOB_TILDE_CHECK) {
            return 0;
        }
    }
    int m = magic(pat, strlen(pat), c->flags & GLOB_NOESCAPE);
    if (m)
        c->g->gl_flags |= GLOB_MAGCHAR;
    expand(c, "", pat);
    free(owned);
    if (c->aborted || c->nospace)
        return 0;
    if (c->n == start) {
        if ((c->flags & GLOB_NOCHECK) || ((c->flags & GLOB_NOMAGIC) && !m)) {
            char *p = malloc(strlen(pattern) + 1);
            if (p)
                strcpy(p, pattern);
            add(c, p);
            return !c->nospace;
        }
        return 0;
    }
    if (!(c->flags & GLOB_NOSORT))
        qsort(c->v + start, c->n - start, sizeof *c->v, by_name);
    return 1;
}

/* GNU's braces: the first {a,b} at this level, each alternative expanded
 * in turn and in order, nested ones by the recursion */
static int braces(struct ctx *c, const char *p)
{
    int noesc = c->flags & GLOB_NOESCAPE;
    const char *open = NULL;
    for (const char *s = p; *s; s++) {
        if (*s == '\\' && !noesc && s[1]) {
            s++;
            continue;
        }
        if (*s == '{') {
            open = s;
            break;
        }
    }
    if (!open)
        return glob_one(c, p);
    /* its close, at depth 0, and the commas at depth 1 */
    int depth = 0;
    const char *close = NULL;
    for (const char *s = open; *s; s++) {
        if (*s == '\\' && !noesc && s[1]) {
            s++;
            continue;
        }
        if (*s == '{')
            depth++;
        else if (*s == '}' && --depth == 0) {
            close = s;
            break;
        }
    }
    if (!close)
        return glob_one(c, p);         /* an unclosed brace is literal */
    size_t pre = (size_t)(open - p), post = strlen(close + 1);
    int any = 0;
    const char *alt = open + 1;
    depth = 0;
    for (const char *s = open + 1; s <= close; s++) {
        if (*s == '\\' && !noesc && s[1] && s < close) {
            s++;
            continue;
        }
        if (*s == '{')
            depth++;
        else if (*s == '}' && depth)
            depth--;
        else if ((*s == ',' && depth == 0) || s == close) {
            size_t al = (size_t)(s - alt);
            char *q = malloc(pre + al + post + 1);
            if (!q) {
                c->nospace = 1;
                return any;
            }
            memcpy(q, p, pre);
            memcpy(q + pre, alt, al);
            memcpy(q + pre + al, close + 1, post + 1);
            any |= braces(c, q);
            free(q);
            if (c->aborted || c->nospace)
                return any;
            alt = s + 1;
        }
    }
    return any;
}

WEAK int glob(const char *__restrict pattern, int flags,
              int (*errfunc)(const char *, int), glob_t *__restrict g)
{
    if (!(flags & GLOB_APPEND)) {
        g->gl_pathc = 0;
        g->gl_pathv = NULL;
        if (!(flags & GLOB_DOOFFS))
            g->gl_offs = 0;
    }
    g->gl_flags = flags & ~GLOB_MAGCHAR;
    struct ctx c = { flags, errfunc, g, NULL, 0, 0, 0, 0 };
    int any = (flags & GLOB_BRACE) ? braces(&c, pattern)
                                   : glob_one(&c, pattern);
    if (c.nospace || c.aborted) {
        for (size_t i = 0; i < c.n; i++)
            free(c.v[i]);
        free(c.v);
        return c.nospace ? GLOB_NOSPACE : GLOB_ABORTED;
    }
    if (!any && !c.n) {
        free(c.v);
        return GLOB_NOMATCH;
    }
    /* gl_offs nulls, what was there (GLOB_APPEND), these, a null */
    size_t offs = (flags & GLOB_DOOFFS) ? g->gl_offs : 0;
    size_t old = g->gl_pathv ? g->gl_pathc : 0;
    char **v = realloc(g->gl_pathv, (offs + old + c.n + 1) * sizeof *v);
    if (!v) {
        for (size_t i = 0; i < c.n; i++)
            free(c.v[i]);
        free(c.v);
        return GLOB_NOSPACE;
    }
    if (!g->gl_pathv)
        for (size_t i = 0; i < offs; i++)
            v[i] = NULL;
    memcpy(v + offs + old, c.v, c.n * sizeof *v);
    v[offs + old + c.n] = NULL;
    free(c.v);
    g->gl_pathv = v;
    g->gl_pathc = old + c.n;
    return 0;
}

WEAK void globfree(glob_t *g)
{
    if (!g->gl_pathv)
        return;
    for (size_t i = 0; i < g->gl_pathc; i++)
        free(g->gl_pathv[g->gl_offs + i]);
    free(g->gl_pathv);
    g->gl_pathv = NULL;
    g->gl_pathc = 0;
}

WEAK int glob_pattern_p(const char *pattern, int quote)
{
    return magic(pattern, strlen(pattern), !quote);
}
