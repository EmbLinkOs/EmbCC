/* <wordexp.h>: a string's words, expanded as the shell expands a
 * command's -- in its order: tildes, then parameters, command
 * substitutions and arithmetic, then field splitting on IFS, then
 * pathname expansion, then quote removal.
 *
 * ---- one buffer, two kinds of character ------------------------------------
 *
 * The step that makes this hard is the last pair. Whether a `*` is a
 * pattern depends on whether it was quoted, and quote removal happened
 * before glob in the shell's own telling. So the field being built is
 * kept as a PATTERN: a character that came from quotes, a backslash or
 * a quoted expansion and is special to glob goes in behind a backslash;
 * an unquoted one goes in as itself. At the end of the field glob reads
 * that pattern exactly as written, and a field it does not match (or
 * that has nothing to match) loses its backslashes instead -- which IS
 * quote removal, done once, at the right time.
 *
 * ---- what a command substitution runs ----------------------------------------
 *
 * The text, by /bin/sh -c, as POSIX says, with its standard error
 * discarded unless WRDE_SHOWERR. Where the OS can start no shell the
 * call fails with WRDE_NOSPACE, glibc's answer for the same.
 *
 * Positional parameters and $@, $*, $!, $- have no meaning outside a
 * shell and are unset; $$ is this process, $? and $# are 0.
 */
#define _GNU_SOURCE 1
#include <wordexp.h>
#include <ctype.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <glob.h>
#include <paths.h>
#include <pwd.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include "posix.h"

struct buf {
    char *s;
    size_t n, cap;
};

struct st {
    int flags;
    const char *ifs;
    int nested;                /* expanding a ${...} word: no fields */
    struct buf w;              /* this field, as a pattern */
    int have;                  /* a field has begun, if only with "" */
    int ws_split;              /* IFS white space just ended a field */
    char **v;                  /* the finished words */
    size_t n, cap;
    int err;
};

#define GLOB_SPECIAL "*?[\\"

static int put(struct st *t, struct buf *b, char c)
{
    if (b->n + 2 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 64;
        char *s = realloc(b->s, cap);
        if (!s) {
            t->err = WRDE_NOSPACE;
            return -1;
        }
        b->s = s;
        b->cap = cap;
    }
    b->s[b->n++] = c;
    b->s[b->n] = 0;
    return 0;
}

/* c as itself, whatever it is: behind a backslash if glob would read it */
static int lit(struct st *t, char c)
{
    if (strchr(GLOB_SPECIAL, c) && put(t, &t->w, '\\'))
        return -1;
    t->have = 1;
    return put(t, &t->w, c);
}

/* the pattern with its backslashes taken out */
static void unescape(char *s)
{
    char *d = s;
    for (; *s; s++) {
        if (*s == '\\' && s[1])
            s++;
        *d++ = *s;
    }
    *d = 0;
}

/* an unescaped * or ?, or a [ that a ] closes: glob has work to do */
static int magic(const char *s)
{
    for (; *s; s++) {
        if (*s == '\\' && s[1]) {
            s++;
            continue;
        }
        if (*s == '*' || *s == '?' || (*s == '[' && strchr(s + 1, ']')))
            return 1;
    }
    return 0;
}

static int add_word(struct st *t, char *s)
{
    if (!s) {
        t->err = WRDE_NOSPACE;
        return -1;
    }
    if (t->n == t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 8;
        char **v = realloc(t->v, cap * sizeof *v);
        if (!v) {
            free(s);
            t->err = WRDE_NOSPACE;
            return -1;
        }
        t->v = v;
        t->cap = cap;
    }
    t->v[t->n++] = s;
    return 0;
}

/* the field is complete: pathname expansion, then quote removal */
static int finish(struct st *t)
{
    char *pat = t->w.s ? t->w.s : "";
    int r = 0;
    if (magic(pat)) {
        glob_t g;
        int gr = glob(pat, 0, 0, &g);
        if (gr == 0) {
            for (size_t i = 0; i < g.gl_pathc && !r; i++)
                r = add_word(t, strdup(g.gl_pathv[i]));
            globfree(&g);
            goto done;
        }
        if (gr == GLOB_NOSPACE) {
            t->err = WRDE_NOSPACE;
            return -1;
        }
    }
    {
        char *s = strdup(pat);
        if (s)
            unescape(s);
        r = add_word(t, s);
    }
done:
    t->w.n = 0;
    if (t->w.s)
        t->w.s[0] = 0;
    t->have = 0;
    return r;
}

/* An expansion's result. Quoted, or inside a ${...} word, it is text;
 * otherwise IFS splits it into fields, and what is left of it is a
 * pattern. White space in IFS runs together and trims; any other IFS
 * character ends a field even an empty one, with the white space around
 * it part of the same separator. */
static int emit(struct st *t, const char *s, size_t n, int quoted)
{
    if (quoted) {
        t->have = 1;
        for (size_t i = 0; i < n; i++)
            if (lit(t, s[i]))
                return -1;
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!t->nested && c && strchr(t->ifs, c)) {
            if (isspace((unsigned char)c)) {
                if (t->have) {
                    if (finish(t))
                        return -1;
                    t->ws_split = 1;
                }
            } else {
                if (!t->have && t->ws_split)
                    t->ws_split = 0;     /* the same separator */
                else if (finish(t))
                    return -1;
            }
            continue;
        }
        t->ws_split = 0;
        t->have = 1;
        if ((c == '\\' && put(t, &t->w, '\\')) || put(t, &t->w, c))
            return -1;
    }
    return 0;
}

/* ---- finding where things end ---------------------------------------------- */

/* past a quoted run starting at s[i] (a ' or "), or n if it never ends */
static size_t skip_quote(const char *s, size_t n, size_t i)
{
    char q = s[i++];
    for (; i < n; i++) {
        if (q == '"' && s[i] == '\\' && i + 1 < n)
            i++;
        else if (s[i] == q)
            return i + 1;
    }
    return n + 1;
}

/* the index of the `close` that ends what `open` at s[i] began */
static size_t match(const char *s, size_t n, size_t i, char open, char close)
{
    int depth = 0;
    while (i < n) {
        char c = s[i];
        if (c == '\\') {
            i += 2;
            continue;
        }
        if (c == '\'' || c == '"') {
            i = skip_quote(s, n, i);
            continue;
        }
        if (c == open)
            depth++;
        else if (c == close && --depth == 0)
            return i;
        i++;
    }
    return n;
}

/* ---- parameters ------------------------------------------------------------- */

static char g_pid[24];

/* a parameter's value, or NULL when it is unset */
static const char *param(const char *name, size_t len)
{
    char nm[256];
    if (len == 1) {
        switch (*name) {
        case '$':
            snprintf(g_pid, sizeof g_pid, "%ld", (long)getpid());
            return g_pid;
        case '?':
        case '#':
            return "0";
        }
    }
    if (!len || len >= sizeof nm || !(isalpha((unsigned char)*name) || *name == '_'))
        return 0;
    memcpy(nm, name, len);
    nm[len] = 0;
    return getenv(nm);
}

static size_t name_len(const char *s, size_t n)
{
    if (!n)
        return 0;
    if (strchr("$?#@*!-", *s) || isdigit((unsigned char)*s))
        return 1;
    size_t i = 0;
    if (isalpha((unsigned char)*s) || *s == '_')
        while (i < n && (isalnum((unsigned char)s[i]) || s[i] == '_'))
            i++;
    return i;
}

static int expand(struct st *t, const char *s, size_t n);

/* A ${...} word, expanded with nothing split: NUL-terminated, in
 * pattern form; the caller frees it. NULL with t->err set. */
static char *sub_word(struct st *t, const char *s, size_t n)
{
    struct st u;
    memset(&u, 0, sizeof u);
    u.flags = t->flags;
    u.ifs = t->ifs;
    u.nested = 1;
    if (expand(&u, s, n) || put(&u, &u.w, 0)) {
        t->err = u.err;
        free(u.w.s);
        return 0;
    }
    return u.w.s;
}

static char *sub_text(struct st *t, const char *s, size_t n)
{
    char *w = sub_word(t, s, n);
    if (w)
        unescape(w);
    return w;
}

/* strip what a pattern matches from one end: % and %% the shortest and
 * longest suffix, # and ## the shortest and longest prefix */
static char *strip(const char *val, const char *pat, int suffix, int longest)
{
    size_t len = strlen(val);
    char *s = strdup(val);
    if (!s)
        return 0;
    for (size_t k = 0; k <= len; k++) {
        size_t cut = longest ? len - k : k;      /* how much to try */
        if (suffix) {
            if (fnmatch(pat, val + len - cut, 0) == 0) {
                s[len - cut] = 0;
                return s;
            }
        } else {
            char save = s[cut];
            s[cut] = 0;
            int m = fnmatch(pat, s, 0) == 0;
            s[cut] = save;
            if (m) {
                memmove(s, s + cut, len - cut + 1);
                return s;
            }
        }
    }
    return s;
}

/* ${...}: s is what is between the braces */
static int brace(struct st *t, const char *s, size_t n, int quoted)
{
    int length = 0;
    if (n > 1 && s[0] == '#') {
        length = 1;
        s++;
        n--;
    }
    size_t nl = name_len(s, n);
    if (!nl)
        return t->err = WRDE_SYNTAX;
    const char *val = param(s, nl);
    const char *op = s + nl, *word = op;
    size_t rest = n - nl;
    if (length) {
        if (rest)
            return t->err = WRDE_SYNTAX;
        if (!val && (t->flags & WRDE_UNDEF))
            return t->err = WRDE_BADVAL;
        char num[24];
        snprintf(num, sizeof num, "%zu", val ? strlen(val) : (size_t)0);
        return emit(t, num, strlen(num), quoted);
    }
    if (!rest) {
        if (!val && (t->flags & WRDE_UNDEF))
            return t->err = WRDE_BADVAL;
        return val ? emit(t, val, strlen(val), quoted) : 0;
    }

    int colon = *op == ':';
    if (colon) {
        word++;
        rest--;
    }
    if (!rest)
        return t->err = WRDE_SYNTAX;
    char o = *word++;
    rest--;
    int twice = 0;
    if (!colon && (o == '%' || o == '#') && rest && *word == o) {
        twice = 1;
        word++;
        rest--;
    }

    if (o == '%' || o == '#') {
        if (colon)
            return t->err = WRDE_SYNTAX;
        if (!val && (t->flags & WRDE_UNDEF))
            return t->err = WRDE_BADVAL;
        if (!val)
            return 0;
        char *pat = sub_word(t, word, rest);
        if (!pat)
            return -1;
        char *r = strip(val, pat, o == '%', twice);
        free(pat);
        if (!r)
            return t->err = WRDE_NOSPACE;
        int e = emit(t, r, strlen(r), quoted);
        free(r);
        return e;
    }

    int unset = !val || (colon && !*val);
    char *w = 0;
    switch (o) {
    case '-':
        if (!unset)
            return emit(t, val, strlen(val), quoted);
        break;
    case '+':
        if (unset)
            return 0;
        break;
    case '=':
        if (!unset)
            return emit(t, val, strlen(val), quoted);
        if (!(isalpha((unsigned char)*s) || *s == '_'))
            return t->err = WRDE_SYNTAX;
        if (!(w = sub_text(t, word, rest)))
            return -1;
        {
            char nm[256];
            if (nl >= sizeof nm) {
                free(w);
                return t->err = WRDE_SYNTAX;
            }
            memcpy(nm, s, nl);
            nm[nl] = 0;
            if (setenv(nm, w, 1)) {
                free(w);
                return t->err = WRDE_NOSPACE;
            }
        }
        goto out;
    case '?':
        if (!unset)
            return emit(t, val, strlen(val), quoted);
        if (t->flags & WRDE_SHOWERR) {
            char *m = sub_text(t, word, rest);
            fprintf(stderr, "%.*s: %s\n", (int)nl, s,
                    m && *m ? m : "parameter null or not set");
            free(m);
        }
        return t->err = WRDE_BADVAL;
    default:
        return t->err = WRDE_SYNTAX;
    }
    if (!(w = sub_text(t, word, rest)))
        return -1;
out:;
    int e = emit(t, w, strlen(w), quoted);
    free(w);
    return e;
}

/* ---- arithmetic ---------------------------------------------------------------- */

struct ar {
    const char *s;
    int err;
};

static void ar_ws(struct ar *a)
{
    while (isspace((unsigned char)*a->s))
        a->s++;
}

static long ar_cond(struct ar *a);

static long ar_prim(struct ar *a)
{
    ar_ws(a);
    char c = *a->s;
    if (c == '(') {
        a->s++;
        long v = ar_cond(a);
        ar_ws(a);
        if (*a->s != ')')
            a->err = 1;
        else
            a->s++;
        return v;
    }
    if (c == '-' || c == '+' || c == '!' || c == '~') {
        a->s++;
        long v = ar_prim(a);
        return c == '-' ? -v : c == '+' ? v : c == '!' ? !v : ~v;
    }
    if (isdigit((unsigned char)c)) {
        char *end;
        long v = strtol(a->s, &end, 0);
        if (isalnum((unsigned char)*end) || *end == '_')
            a->err = 1;
        a->s = end;
        return v;
    }
    if (isalpha((unsigned char)c) || c == '_') {
        size_t n = name_len(a->s, strlen(a->s));
        const char *v = param(a->s, n);
        a->s += n;
        if (!v || !*v)
            return 0;
        char *end;
        long x = strtol(v, &end, 0);
        if (*end)
            a->err = 1;
        return x;
    }
    a->err = 1;
    return 0;
}

/* binary operators by precedence, loosest first, as C's */
static const char *const g_ops[][5] = {
    {"||"}, {"&&"}, {"|"}, {"^"}, {"&"}, {"==", "!="},
    {"<=", ">=", "<", ">"}, {"<<", ">>"}, {"+", "-"}, {"*", "/", "%"},
};
#define LEVELS (int)(sizeof g_ops / sizeof g_ops[0])

static const char *ar_op(struct ar *a, int lv)
{
    ar_ws(a);
    for (int i = 0; i < 5 && g_ops[lv][i]; i++) {
        const char *o = g_ops[lv][i];
        size_t n = strlen(o);
        if (strncmp(a->s, o, n))
            continue;
        /* not the first half of a longer operator of another level */
        if (n == 1 && (a->s[1] == '=' || (a->s[1] == *o && strchr("|&<>", *o))))
            continue;
        a->s += n;
        return o;
    }
    return 0;
}

static long ar_bin(struct ar *a, int lv)
{
    if (lv == LEVELS)
        return ar_prim(a);
    long v = ar_bin(a, lv + 1);
    while (!a->err) {
        const char *o = ar_op(a, lv);
        if (!o)
            break;
        long r = ar_bin(a, lv + 1);
        switch (o[0] | o[1] << 8) {
        case '|' | '|' << 8: v = v || r; break;
        case '&' | '&' << 8: v = v && r; break;
        case '|': v |= r; break;
        case '^': v ^= r; break;
        case '&': v &= r; break;
        case '=' | '=' << 8: v = v == r; break;
        case '!' | '=' << 8: v = v != r; break;
        case '<' | '=' << 8: v = v <= r; break;
        case '>' | '=' << 8: v = v >= r; break;
        case '<': v = v < r; break;
        case '>': v = v > r; break;
        case '<' | '<' << 8: v = (long)((unsigned long)v << (r & 63)); break;
        case '>' | '>' << 8: v >>= (r & 63); break;
        case '+': v = (long)((unsigned long)v + (unsigned long)r); break;
        case '-': v = (long)((unsigned long)v - (unsigned long)r); break;
        case '*': v = (long)((unsigned long)v * (unsigned long)r); break;
        case '/':
        case '%':
            if (!r || (r == -1 && v < -__LONG_MAX__)) {
                a->err = 1;
                return 0;
            }
            v = o[0] == '/' ? v / r : v % r;
            break;
        }
    }
    return v;
}

static long ar_cond(struct ar *a)
{
    long c = ar_bin(a, 0);
    ar_ws(a);
    if (*a->s != '?')
        return c;
    a->s++;
    long x = ar_cond(a);
    ar_ws(a);
    if (*a->s != ':') {
        a->err = 1;
        return 0;
    }
    a->s++;
    long y = ar_cond(a);
    return c ? x : y;
}

static int arith(struct st *t, const char *s, size_t n, int quoted)
{
    char *e = sub_text(t, s, n);
    if (!e)
        return -1;
    struct ar a = {e, 0};
    long v = ar_cond(&a);
    ar_ws(&a);
    int bad = a.err || *a.s;
    free(e);
    if (bad)
        return t->err = WRDE_SYNTAX;
    char num[24];
    snprintf(num, sizeof num, "%ld", v);
    return emit(t, num, strlen(num), quoted);
}

/* ---- command substitution -------------------------------------------------------- */

/* /bin/sh with these arguments, its output into *out (when out is not
 * NULL) and its standard error to /dev/null unless `keep_err`: the exit
 * status, or -1 if it could not be started. With no /dev/null the
 * shell's standard error is closed, which discards as well -- a
 * redirection written into the command would make a shell without
 * /dev/null exit instead. */
static int run_sh(struct st *t, char **argv, struct buf *out, int keep_err)
{
    int p[2] = {-1, -1}, null = -1, st = -1;
    posix_spawn_file_actions_t fa;
    if (posix_spawn_file_actions_init(&fa))
        return -1;
    if (pipe(p))
        goto done;
    fcntl(p[0], F_SETFD, FD_CLOEXEC);
    if (out) {
        posix_spawn_file_actions_adddup2(&fa, p[1], 1);
    } else if ((null = open("/dev/null", O_WRONLY | O_CLOEXEC)) >= 0) {
        posix_spawn_file_actions_adddup2(&fa, null, 1);
    }
    if (!keep_err) {
        if (null < 0)
            null = open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (null >= 0)
            posix_spawn_file_actions_adddup2(&fa, null, 2);
        else
            posix_spawn_file_actions_addclose(&fa, 2);
    }
    posix_spawn_file_actions_addclose(&fa, p[1]);
    pid_t pid;
    if (posix_spawn(&pid, _PATH_BSHELL, &fa, 0, argv, environ))
        goto done;
    close(p[1]);
    p[1] = -1;
    char chunk[256];
    ssize_t n;
    while ((n = read(p[0], chunk, sizeof chunk)) != 0) {
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        for (ssize_t i = 0; out && i < n; i++)
            if (put(t, out, chunk[i]))
                break;
    }
    int ws;
    while (waitpid(pid, &ws, 0) < 0)
        if (errno != EINTR) {
            ws = -1;
            break;
        }
    st = ws != -1 && WIFEXITED(ws) ? WEXITSTATUS(ws) : 128;
done:
    posix_spawn_file_actions_destroy(&fa);
    if (p[0] >= 0)
        close(p[0]);
    if (p[1] >= 0)
        close(p[1]);
    if (null >= 0)
        close(null);
    return st;
}

/* $(...) or `...`: the output, less its trailing newlines. A command
 * that fails is still a substitution -- its output is the value -- unless
 * the shell cannot even read it (`sh -n` says so), which is
 * WRDE_SYNTAX, as glibc decides. A shell that cannot be started is
 * WRDE_NOSPACE, as glibc answers too. */
static int command(struct st *t, const char *s, size_t n, int quoted)
{
    if (t->flags & WRDE_NOCMD)
        return t->err = WRDE_CMDSUB;
    char *cmd = malloc(n + 1);
    if (!cmd)
        return t->err = WRDE_NOSPACE;
    memcpy(cmd, s, n);
    cmd[n] = 0;
    struct buf out = {0, 0, 0};
    char *argv[] = {"sh", "-c", cmd, 0};
    fflush(0);
    int st = run_sh(t, argv, &out, t->flags & WRDE_SHOWERR);
    if (st > 0 && !t->err) {
        char *check[] = {"sh", "-n", "-c", cmd, 0};
        if (run_sh(t, check, 0, t->flags & WRDE_SHOWERR) != 0)
            t->err = WRDE_SYNTAX;
    }
    free(cmd);
    if (st < 0 && !t->err)
        t->err = WRDE_NOSPACE;
    if (t->err) {
        free(out.s);
        return -1;
    }
    while (out.n && out.s[out.n - 1] == '\n')
        out.n--;
    int e = emit(t, out.s ? out.s : "", out.n, quoted);
    free(out.s);
    return e;
}

/* `...`: a backslash keeps its meaning only before $, ` and \ */
static int backquote(struct st *t, const char *s, size_t n, size_t *i,
                     int quoted)
{
    struct buf b = {0, 0, 0};
    size_t j = *i + 1;
    for (; j < n && s[j] != '`'; j++) {
        if (s[j] == '\\' && j + 1 < n &&
            (s[j + 1] == '$' || s[j + 1] == '`' || s[j + 1] == '\\' ||
             (quoted && s[j + 1] == '"')))
            j++;
        if (put(t, &b, s[j])) {
            free(b.s);
            return -1;
        }
    }
    if (j >= n) {
        free(b.s);
        return t->err = WRDE_SYNTAX;
    }
    *i = j + 1;
    int e = command(t, b.s ? b.s : "", b.n, quoted);
    free(b.s);
    return e;
}

/* ---- the words ------------------------------------------------------------------- */

/* $ at s[*i]: whichever expansion follows, and *i past it */
static int dollar(struct st *t, const char *s, size_t n, size_t *i, int quoted)
{
    size_t j = *i + 1;
    if (j < n && s[j] == '(') {
        if (j + 1 < n && s[j + 1] == '(') {
            /* $(( ... )): arithmetic if the parenthesis that closes the
             * second ( is followed at once by the one closing the first */
            int depth = 0;
            for (size_t k = j + 2; k < n; k++) {
                if (s[k] == '(')
                    depth++;
                else if (s[k] == ')' && depth-- == 0) {
                    if (k + 1 < n && s[k + 1] == ')') {
                        *i = k + 2;
                        return arith(t, s + j + 2, k - j - 2, quoted);
                    }
                    break;
                }
            }
        }
        size_t e = match(s, n, j, '(', ')');
        if (e >= n)
            return t->err = WRDE_SYNTAX;
        *i = e + 1;
        return command(t, s + j + 1, e - j - 1, quoted);
    }
    if (j < n && s[j] == '{') {
        size_t e = match(s, n, j, '{', '}');
        if (e >= n)
            return t->err = WRDE_SYNTAX;
        *i = e + 1;
        return brace(t, s + j + 1, e - j - 1, quoted);
    }
    size_t nl = name_len(s + j, n - j);
    if (!nl) {
        *i = j;
        return lit(t, '$');
    }
    *i = j + nl;
    const char *v = param(s + j, nl);
    if (!v && (t->flags & WRDE_UNDEF))
        return t->err = WRDE_BADVAL;
    return v ? emit(t, v, strlen(v), quoted) : 0;
}

/* ~ or ~user at the start of a word, up to a / or the word's end */
static int tilde(struct st *t, const char *s, size_t n, size_t *i)
{
    size_t j = *i + 1;
    while (j < n && s[j] != '/' && !(!t->nested && strchr(" \t", s[j]))) {
        if (strchr("\\'\"$`|&;<>(){}\n", s[j]))
            return lit(t, s[(*i)++]);           /* quoted: no tilde */
        j++;
    }
    const char *dir = 0;
    if (j == *i + 1) {
        dir = getenv("HOME");
        if (!dir) {
            struct passwd *pw = getpwuid(getuid());
            dir = pw ? pw->pw_dir : 0;
        }
    } else {
        char user[256];
        size_t ul = j - *i - 1;
        if (ul < sizeof user) {
            memcpy(user, s + *i + 1, ul);
            user[ul] = 0;
            struct passwd *pw = getpwnam(user);
            dir = pw ? pw->pw_dir : 0;
        }
    }
    if (!dir)
        return lit(t, s[(*i)++]);
    *i = j;
    t->have = 1;
    return emit(t, dir, strlen(dir), 1);
}

static int expand(struct st *t, const char *s, size_t n)
{
    size_t i = 0;
    while (i < n && !t->err) {
        char c = s[i];
        if (!t->nested && (c == ' ' || c == '\t')) {
            if (t->have)
                finish(t);
            t->ws_split = 0;
            i++;
            continue;
        }
        if (!t->nested && strchr("\n|&;<>(){}", c))
            return t->err = WRDE_BADCHAR;
        switch (c) {
        case '\\':
            if (i + 1 < n) {
                lit(t, s[i + 1]);
                i += 2;
            } else {
                lit(t, '\\');
                i++;
            }
            continue;
        case '\'': {
            size_t e = skip_quote(s, n, i);
            if (e > n)
                return t->err = WRDE_SYNTAX;
            t->have = 1;
            for (i++; i < e - 1 && !t->err; i++)
                lit(t, s[i]);
            i = e;
            continue;
        }
        case '"': {
            t->have = 1;
            for (i++; i < n && s[i] != '"' && !t->err;) {
                char d = s[i];
                if (d == '\\' && i + 1 < n && strchr("$`\"\\\n", s[i + 1])) {
                    if (s[i + 1] != '\n')
                        lit(t, s[i + 1]);
                    i += 2;
                } else if (d == '$') {
                    dollar(t, s, n, &i, 1);
                } else if (d == '`') {
                    backquote(t, s, n, &i, 1);
                } else {
                    lit(t, d);
                    i++;
                }
            }
            if (t->err)
                return -1;
            if (i >= n)
                return t->err = WRDE_SYNTAX;
            i++;
            continue;
        }
        case '$':
            dollar(t, s, n, &i, 0);
            continue;
        case '`':
            backquote(t, s, n, &i, 0);
            continue;
        case '~':
            if (!t->have && !t->w.n) {
                tilde(t, s, n, &i);
                continue;
            }
            break;
        }
        t->ws_split = 0;
        t->have = 1;
        put(t, &t->w, c);
        i++;
    }
    return t->err ? -1 : 0;
}

WEAK void wordfree(wordexp_t *we)
{
    if (!we || !we->we_wordv)
        return;
    for (size_t i = 0; i < we->we_wordc; i++)
        free(we->we_wordv[we->we_offs + i]);
    free(we->we_wordv);
    we->we_wordv = 0;
    we->we_wordc = 0;
}

WEAK int wordexp(const char *restrict words, wordexp_t *restrict we, int flags)
{
    if (flags & WRDE_REUSE)
        wordfree(we);
    if (!(flags & WRDE_APPEND)) {
        we->we_wordc = 0;
        we->we_wordv = 0;
        if (!(flags & WRDE_DOOFFS))
            we->we_offs = 0;
    }

    struct st t;
    memset(&t, 0, sizeof t);
    t.flags = flags;
    t.ifs = getenv("IFS");
    if (!t.ifs)
        t.ifs = " \t\n";
    if (!expand(&t, words, strlen(words)) && t.have)
        finish(&t);
    free(t.w.s);

    if (!t.err) {
        size_t offs = flags & WRDE_DOOFFS ? we->we_offs : 0;
        size_t old = we->we_wordv ? we->we_wordc : 0;
        char **v = realloc(we->we_wordv, (offs + old + t.n + 1) * sizeof *v);
        if (!v) {
            t.err = WRDE_NOSPACE;
        } else {
            if (!we->we_wordv)
                for (size_t i = 0; i < offs; i++)
                    v[i] = 0;
            memcpy(v + offs + old, t.v, t.n * sizeof *v);
            v[offs + old + t.n] = 0;
            we->we_wordv = v;
            we->we_wordc = old + t.n;
            we->we_offs = offs;
            free(t.v);
            return 0;
        }
    }
    for (size_t i = 0; i < t.n; i++)
        free(t.v[i]);
    free(t.v);
    return t.err;
}
