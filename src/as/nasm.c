/* See nasm.h. Two layers, as NASM has them: the preprocessor (lines in,
 * lines out: conditionals, macros, %rep, %include) and the reading of
 * each line it lets through (labels, directives, data, instructions),
 * which writes GNU-syntax text for gas.c with, for every line written,
 * the file and line it came from. */
#include "nasm.h"
#include "gas.h"

#include "../arch/target.h"
#include "../arch/x86_64/att.h"
#include "../arch/x86_64/intel.h"
#include "../driver/util.h"
#include "../platform/platform.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- strings ------------------------------------------------------------ */

struct sb { char *p; size_t n, cap; };

static void sb_putn(struct sb *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2 + 64;
        b->p = xrealloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void sb_puts(struct sb *b, const char *s) { sb_putn(b, s, strlen(s)); }
static void sb_putc(struct sb *b, char c) { sb_putn(b, &c, 1); }
static void sb_printf(struct sb *b, const char *fmt, ...)
{
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (k < (int)sizeof tmp) { sb_putn(b, tmp, (size_t)k); return; }
    char *big = xmalloc((size_t)k + 1);
    va_start(ap, fmt);
    vsnprintf(big, (size_t)k + 1, fmt, ap);
    va_end(ap);
    sb_putn(b, big, (size_t)k);
    free(big);
}
static char *sb_take(struct sb *b)
{
    char *p = b->p ? b->p : xcalloc(1, 1);
    b->p = NULL; b->n = b->cap = 0;
    return p;
}
static char *xstrdup0(const char *s) { return xstrndup(s, strlen(s)); }

static const char *skip_sp(const char *p)
{
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

static void rtrim(char *s)
{
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
}

/* NASM's name characters */
static int id0(int c) { return isalpha(c) || c == '_' || c == '.' || c == '?' || c == '$' || c == '@'; }
static int idc(int c) { return isalnum(c) || c == '_' || c == '.' || c == '?' || c == '$' || c == '@' || c == '#' || c == '~'; }

/* (case-blind, in ISO C: strncasecmp is POSIX's) */
static int ncase(const char *a, const char *b, size_t n)
{
    for (size_t k = 0; k < n; k++) {
        int x = tolower((unsigned char)a[k]), y = tolower((unsigned char)b[k]);
        if (x != y) return x - y;
        if (!x) return 0;
    }
    return 0;
}

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

/* ---- the state ----------------------------------------------------------- */

struct loc { const char *file; int line; };
struct pl { char *t; struct loc at; };

struct smac { char *name; int np; char **par; char *body; int icase, busy; };
struct mmac {
    char *name;
    int minp, maxp, greedy;
    char **dflt;
    int ndflt;
    struct pl *body;
    int nbody;
};
struct mctx {
    struct mmac *m;
    char **arg;
    int narg;
    const char *label;
    long id;
};
struct cond { int active, taken; };

struct nas {
    const struct nasm_opts *o;
    const char *path;
    int errors;
    /* the preprocessor */
    struct smac *sm; int nsm, capsm;
    struct mmac *mm; int nmm, capmm;
    struct cond cs[128]; int ncs;
    long uniq;
    int depth, exitrep;
    char **files; int nfiles;
    /* what it writes */
    struct sb out;
    struct gas_loc *locs; int nlocs, caplocs;
    struct sb pp;                   /* -E */
    /* the reading */
    int mode, default_rel;
    char lastg[256];
    char **secs; int nsec, cur;
    int org_set; long long org;
    int abs_on; long long abs_pc;
    char struc[256];
    long istruc_id; char istruc[256];
    struct loc at;
};

static void nerr(struct nas *n, struct loc at, const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "%s:%d: error: ", at.file, at.line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    n->errors++;
}

static void nwarn(struct loc at, const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "%s:%d: warning: ", at.file, at.line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* one line of gas text, from where the line being read came */
static void emit(struct nas *n, const char *fmt, ...)
{
    char tmp[2048];
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (k >= (int)sizeof tmp) {
        char *big = xmalloc((size_t)k + 1);
        va_start(ap, fmt);
        vsnprintf(big, (size_t)k + 1, fmt, ap);
        va_end(ap);
        sb_puts(&n->out, big);
        free(big);
    } else
        sb_puts(&n->out, tmp);
    sb_putc(&n->out, '\n');
    if (n->nlocs == n->caplocs) {
        n->caplocs = n->caplocs ? n->caplocs * 2 : 256;
        n->locs = xrealloc(n->locs, (size_t)n->caplocs * sizeof *n->locs);
    }
    n->locs[n->nlocs].file = n->at.file;
    n->locs[n->nlocs].line = n->at.line;
    n->nlocs++;
}

/* ---- numbers and strings ------------------------------------------------- */

/* a NASM number token (0x1f, 1fh, $1f, 0b101, 101b, 17o, 1_000): 1 */
static int num_tok(const char *s, int n, unsigned long long *v)
{
    char d[128];
    int k = 0;
    for (int i = 0; i < n && k < 127; i++)
        if (s[i] != '_') d[k++] = s[i];
    d[k] = 0;
    if (!k) return 0;
    int base = 10;
    char *p = d;
    if (p[0] == '$') { base = 16; p++; if (!isdigit((unsigned char)*p)) return 0; }
    else if (p[0] == '0' && p[1] && strchr("xXhH", p[1])) { base = 16; p += 2; }
    else if (p[0] == '0' && p[1] && strchr("dDtT", p[1])) { base = 10; p += 2; }
    else if (p[0] == '0' && p[1] && strchr("oOqQ", p[1])) { base = 8; p += 2; }
    else if (p[0] == '0' && p[1] && strchr("bByY", p[1]) && p[2]) { base = 2; p += 2; }
    else {
        size_t l = strlen(p);
        char c = (char)tolower((unsigned char)p[l - 1]);
        if (c == 'h') { base = 16; p[l - 1] = 0; }
        else if (c == 'q' || c == 'o') { base = 8; p[l - 1] = 0; }
        else if ((c == 'b' || c == 'y') && l > 1 && strspn(p, "01") == l - 1) { base = 2; p[l - 1] = 0; }
        else if ((c == 'd' || c == 't') && l > 1 && strspn(p, "0123456789") == l - 1) { base = 10; p[l - 1] = 0; }
    }
    if (!*p) return 0;
    unsigned long long r = 0;
    for (; *p; p++) {
        int dv = isdigit((unsigned char)*p) ? *p - '0'
               : isalpha((unsigned char)*p) ? tolower((unsigned char)*p) - 'a' + 10 : 99;
        if (dv >= base) return 0;
        r = r * (unsigned)base + (unsigned)dv;
    }
    *v = r;
    return 1;
}

/* a quoted string at s ('..', "..", `..` with escapes): its bytes, the
 * length, and where it ends; -1 when it does not close */
static int str_tok(const char *s, unsigned char *buf, int cap, const char **end)
{
    char q = *s;
    int n = 0;
    const char *p = s + 1;
    while (*p && *p != q) {
        int c = (unsigned char)*p++;
        if (q == '`' && c == '\\' && *p) {
            c = (unsigned char)*p++;
            switch (c) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case '0': c = 0; break;
            case 'a': c = 7; break;
            case 'b': c = 8; break;
            case 'f': c = 12; break;
            case 'v': c = 11; break;
            case 'e': c = 27; break;
            case 'x': {
                int v = 0, k = 0;
                while (k < 2 && isxdigit((unsigned char)*p)) {
                    v = v * 16 + (isdigit((unsigned char)*p) ? *p - '0'
                                  : tolower((unsigned char)*p) - 'a' + 10);
                    p++; k++;
                }
                c = v;
                break;
            }
            default: break;
            }
        }
        if (n < cap) buf[n] = (unsigned char)c;
        n++;
    }
    if (*p != q) return -1;
    *end = p + 1;
    return n;
}

/* ---- the preprocessor's expressions -------------------------------------- */

struct ev { const char *p; int bad; struct nas *n; struct loc at; };

static long long ev_or(struct ev *e);

static void ev_ws(struct ev *e) { e->p = skip_sp(e->p); }

static long long ev_prim(struct ev *e)
{
    ev_ws(e);
    const char *p = e->p;
    if (*p == '(') {
        e->p++;
        long long v = ev_or(e);
        ev_ws(e);
        if (*e->p != ')') { e->bad = 1; return 0; }
        e->p++;
        return v;
    }
    if (*p == '\'' || *p == '"' || *p == '`') {
        unsigned char b[16];
        const char *end;
        int len = str_tok(p, b, (int)sizeof b, &end);
        if (len < 0 || len > 8) { e->bad = 1; return 0; }
        unsigned long long v = 0;
        for (int k = len - 1; k >= 0; k--) v = v << 8 | b[k];
        e->p = end;
        return (long long)v;
    }
    if (isdigit((unsigned char)*p) || (*p == '$' && isdigit((unsigned char)p[1]))) {
        const char *q = p + 1;
        while (isalnum((unsigned char)*q) || *q == '_') q++;
        unsigned long long v;
        if (!num_tok(p, (int)(q - p), &v)) { e->bad = 1; return 0; }
        e->p = q;
        return (long long)v;
    }
    e->bad = 2;                         /* a name: not a constant here */
    return 0;
}

static long long ev_un(struct ev *e)
{
    ev_ws(e);
    char c = *e->p;
    if (c == '-' || c == '+' || c == '~' || c == '!') {
        e->p++;
        long long v = ev_un(e);
        return c == '-' ? -v : c == '+' ? v : c == '~' ? ~v : !v;
    }
    return ev_prim(e);
}

static long long ev_mul(struct ev *e)
{
    long long v = ev_un(e);
    for (;;) {
        ev_ws(e);
        const char *p = e->p;
        if (p[0] == '*') { e->p++; v *= ev_un(e); }
        else if (p[0] == '/' && p[1] == '/') {
            e->p += 2; long long r = ev_un(e);
            if (!r) { e->bad = 3; return 0; }
            v /= r;
        } else if (p[0] == '/') {
            e->p++; long long r = ev_un(e);
            if (!r) { e->bad = 3; return 0; }
            v = (long long)((unsigned long long)v / (unsigned long long)r);
        } else if (p[0] == '%' && p[1] == '%') {
            e->p += 2; long long r = ev_un(e);
            if (!r) { e->bad = 3; return 0; }
            v %= r;
        } else if (p[0] == '%') {
            e->p++; long long r = ev_un(e);
            if (!r) { e->bad = 3; return 0; }
            v = (long long)((unsigned long long)v % (unsigned long long)r);
        } else return v;
    }
}

static long long ev_add(struct ev *e)
{
    long long v = ev_mul(e);
    for (;;) {
        ev_ws(e);
        if (*e->p == '+') { e->p++; v += ev_mul(e); }
        else if (*e->p == '-') { e->p++; v -= ev_mul(e); }
        else return v;
    }
}

static long long ev_sh(struct ev *e)
{
    long long v = ev_add(e);
    for (;;) {
        ev_ws(e);
        const char *p = e->p;
        if (p[0] == '<' && p[1] == '<') {
            e->p += p[2] == '<' ? 3 : 2;
            v = (long long)((unsigned long long)v << (ev_add(e) & 63));
        } else if (p[0] == '>' && p[1] == '>' && p[2] == '>') {
            e->p += 3; v >>= ev_add(e) & 63;
        } else if (p[0] == '>' && p[1] == '>') {
            e->p += 2; v = (long long)((unsigned long long)v >> (ev_add(e) & 63));
        } else return v;
    }
}

static long long ev_band(struct ev *e)
{
    long long v = ev_sh(e);
    for (;;) {
        ev_ws(e);
        if (e->p[0] == '&' && e->p[1] != '&') { e->p++; v &= ev_sh(e); }
        else return v;
    }
}
static long long ev_bxor(struct ev *e)
{
    long long v = ev_band(e);
    for (;;) {
        ev_ws(e);
        if (e->p[0] == '^' && e->p[1] != '^') { e->p++; v ^= ev_band(e); }
        else return v;
    }
}
static long long ev_bor(struct ev *e)
{
    long long v = ev_bxor(e);
    for (;;) {
        ev_ws(e);
        if (e->p[0] == '|' && e->p[1] != '|') { e->p++; v |= ev_bxor(e); }
        else return v;
    }
}
static long long ev_cmp(struct ev *e)
{
    long long v = ev_bor(e);
    for (;;) {
        ev_ws(e);
        const char *p = e->p;
        if (p[0] == '=' && p[1] == '=') { e->p += 2; v = v == ev_bor(e); }
        else if (p[0] == '!' && p[1] == '=') { e->p += 2; v = v != ev_bor(e); }
        else if (p[0] == '<' && p[1] == '>') { e->p += 2; v = v != ev_bor(e); }
        else if (p[0] == '<' && p[1] == '=' && p[2] == '>') {
            e->p += 3; long long r = ev_bor(e); v = v < r ? -1 : v > r;
        } else if (p[0] == '<' && p[1] == '=') { e->p += 2; v = v <= ev_bor(e); }
        else if (p[0] == '>' && p[1] == '=') { e->p += 2; v = v >= ev_bor(e); }
        else if (p[0] == '<' && p[1] != '<') { e->p++; v = v < ev_bor(e); }
        else if (p[0] == '>' && p[1] != '>') { e->p++; v = v > ev_bor(e); }
        else if (p[0] == '=') { e->p++; v = v == ev_bor(e); }
        else return v;
    }
}
static long long ev_and(struct ev *e)
{
    long long v = ev_cmp(e);
    for (;;) {
        ev_ws(e);
        if (e->p[0] == '&' && e->p[1] == '&') { e->p += 2; long long r = ev_cmp(e); v = v && r; }
        else return v;
    }
}
static long long ev_xor(struct ev *e)
{
    long long v = ev_and(e);
    for (;;) {
        ev_ws(e);
        if (e->p[0] == '^' && e->p[1] == '^') { e->p += 2; long long r = ev_and(e); v = !v != !r; }
        else return v;
    }
}
static long long ev_or(struct ev *e)
{
    long long v = ev_xor(e);
    for (;;) {
        ev_ws(e);
        if (e->p[0] == '|' && e->p[1] == '|') { e->p += 2; long long r = ev_xor(e); v = v || r; }
        else return v;
    }
}

/* a constant expression (already expanded): 1, or 0 with a message */
static int peval(struct nas *n, struct loc at, const char *s, long long *v,
                 const char *what)
{
    struct ev e = { s, 0, n, at };
    *v = ev_or(&e);
    ev_ws(&e);
    if (e.bad || *e.p) {
        nerr(n, at, "%s \"%s\" is not a constant expression%s", what, s,
             e.bad == 2 ? " (it names something that is not a number here)"
             : e.bad == 3 ? " (it divides by zero)" : "");
        return 0;
    }
    return 1;
}

/* ---- single-line macros -------------------------------------------------- */

static struct smac *smac_find(struct nas *n, const char *name, size_t len)
{
    for (int k = n->nsm - 1; k >= 0; k--) {
        struct smac *m = &n->sm[k];
        if (strlen(m->name) != len) continue;
        if (m->icase ? !ncase(m->name, name, len) : !strncmp(m->name, name, len))
            return m;
    }
    return NULL;
}

static void smac_undef(struct nas *n, const char *name)
{
    for (int k = 0; k < n->nsm; k++)
        if (!strcmp(n->sm[k].name, name)) {
            free(n->sm[k].name); free(n->sm[k].body);
            for (int j = 0; j < n->sm[k].np; j++) free(n->sm[k].par[j]);
            free(n->sm[k].par);
            n->sm[k] = n->sm[--n->nsm];
            k--;
        }
}

static void smac_def(struct nas *n, const char *name, int np, char **par,
                     const char *body, int icase)
{
    smac_undef(n, name);
    if (n->nsm == n->capsm) {
        n->capsm = n->capsm ? n->capsm * 2 : 32;
        n->sm = xrealloc(n->sm, (size_t)n->capsm * sizeof *n->sm);
    }
    struct smac *m = &n->sm[n->nsm++];
    m->name = xstrdup0(name);
    m->np = np;
    m->par = par;
    m->body = xstrdup0(body);
    m->icase = icase;
    m->busy = 0;
}

static char *expand(struct nas *n, const char *s, int depth);

/* the arguments of name(...) at p (p at the '('): their count, and p
 * past the ')' */
static int smac_args(const char *p, char ***out, const char **end)
{
    int depth = 0, na = 0, cap = 4;
    char **a = xmalloc((size_t)cap * sizeof *a);
    const char *st = p + 1;
    for (const char *q = p + 1; *q; q++) {
        if (*q == '\'' || *q == '"' || *q == '`') {
            char c = *q++;
            while (*q && *q != c) q++;
            if (!*q) break;
            continue;
        }
        if (*q == '(') depth++;
        else if ((*q == ',' && depth == 0) || (*q == ')' && depth == 0)) {
            if (na == cap) { cap *= 2; a = xrealloc(a, (size_t)cap * sizeof *a); }
            const char *b = skip_sp(st);
            size_t l = (size_t)(q - b);
            while (l && isspace((unsigned char)b[l - 1])) l--;
            a[na++] = xstrndup(b, l);
            st = q + 1;
            if (*q == ')') { *out = a; *end = q + 1; return na; }
        } else if (*q == ')') depth--;
    }
    for (int k = 0; k < na; k++) free(a[k]);
    free(a);
    return -1;
}

/* the single-line macros in s expanded (not in strings, not after a
 * name's own dot) */
static char *expand(struct nas *n, const char *s, int depth)
{
    struct sb b = { 0 };
    const char *p = s;
    if (depth > 64) {
        nerr(n, n->at, "a %%define expands into itself more than 64 deep");
        return xstrdup0(s);
    }
    while (*p) {
        if (*p == '\'' || *p == '"' || *p == '`') {
            char c = *p;
            const char *q = p + 1;
            while (*q && *q != c) { if (c == '`' && *q == '\\' && q[1]) q++; q++; }
            if (*q) q++;
            sb_putn(&b, p, (size_t)(q - p));
            p = q;
            continue;
        }
        if (isdigit((unsigned char)*p)) {
            const char *q = p;
            while (idc((unsigned char)*q)) q++;
            sb_putn(&b, p, (size_t)(q - p));
            p = q;
            continue;
        }
        if (id0((unsigned char)*p) || *p == '%') {
            const char *q = p + (*p == '%');
            while (idc((unsigned char)*q)) q++;
            struct smac *m = q > p ? smac_find(n, p, (size_t)(q - p)) : NULL;
            if (m && !m->busy) {
                const char *after = q;
                char **args = NULL;
                int na = 0;
                if (m->np >= 0) {
                    const char *r = skip_sp(q);
                    if (*r != '(') { sb_putn(&b, p, (size_t)(q - p)); p = q; continue; }
                    na = smac_args(r, &args, &after);
                    if (na < 0 || (na != m->np && !(m->np == 0 && na == 1 && !args[0][0]))) {
                        if (na >= 0) { for (int k = 0; k < na; k++) free(args[k]); free(args); }
                        sb_putn(&b, p, (size_t)(q - p)); p = q; continue;
                    }
                }
                /* the body with each parameter's argument */
                struct sb body = { 0 };
                const char *x = m->body;
                while (*x) {
                    if (m->np > 0 && id0((unsigned char)*x)) {
                        const char *y = x;
                        while (idc((unsigned char)*y)) y++;
                        int hit = -1;
                        for (int k = 0; k < m->np; k++)
                            if (strlen(m->par[k]) == (size_t)(y - x) &&
                                !strncmp(m->par[k], x, (size_t)(y - x)))
                                hit = k;
                        if (hit >= 0) sb_puts(&body, args[hit]);
                        else sb_putn(&body, x, (size_t)(y - x));
                        x = y;
                        continue;
                    }
                    sb_putc(&body, *x++);
                }
                for (int k = 0; k < na; k++) free(args[k]);
                free(args);
                char *bt = sb_take(&body);
                m->busy = 1;
                char *ex = expand(n, bt, depth + 1);
                m->busy = 0;
                sb_puts(&b, ex);
                free(ex);
                free(bt);
                p = after;
                continue;
            }
            sb_putn(&b, p, (size_t)(q - p));
            p = q;
            continue;
        }
        sb_putc(&b, *p++);
    }
    return sb_take(&b);
}

/* ---- source lines --------------------------------------------------------- */

static void strip_comment(char *s)
{
    char q = 0;
    for (char *p = s; *p; p++) {
        if (q) {
            if (q == '`' && *p == '\\' && p[1]) { p++; continue; }
            if (*p == q) q = 0;
            continue;
        }
        if (*p == '\'' || *p == '"' || *p == '`') { q = *p; continue; }
        if (*p == ';') { *p = 0; break; }
    }
    rtrim(s);
}

static const char *keep_name(struct nas *n, const char *f)
{
    n->files = xrealloc(n->files, (size_t)(n->nfiles + 1) * sizeof *n->files);
    n->files[n->nfiles] = xstrdup0(f);
    return n->files[n->nfiles++];
}

/* a file's text as lines (a `\` at the end joins the next) */
static struct pl *split_lines(struct nas *n, const char *text, const char *file, int *nl)
{
    const char *fname = keep_name(n, file);
    int cap = 256, k = 0, line = 1;
    struct pl *L = xmalloc((size_t)cap * sizeof *L);
    const char *p = text;
    while (*p) {
        struct sb b = { 0 };
        int first = line;
        for (;;) {
            const char *e = p;
            while (*e && *e != '\n') e++;
            size_t l = (size_t)(e - p);
            if (l && p[l - 1] == '\r') l--;
            int cont = l && p[l - 1] == '\\';
            sb_putn(&b, p, cont ? l - 1 : l);
            p = *e ? e + 1 : e;
            line++;
            if (!cont || !*p) break;
        }
        if (k == cap) { cap *= 2; L = xrealloc(L, (size_t)cap * sizeof *L); }
        L[k].t = b.p ? sb_take(&b) : xcalloc(1, 1);
        L[k].at.file = fname;
        L[k].at.line = first;
        k++;
    }
    *nl = k;
    return L;
}

static char *find_file(struct nas *n, const char *name, const char *from)
{
    long len;
    char *d;
    if (name[0] == '/') return xstrdup0(name);
    {
        FILE *f = fopen(name, "rb");
        if (f) { fclose(f); return xstrdup0(name); }
    }
    /* beside the file that names it */
    const char *sl = strrchr(from, '/');
    if (sl) {
        char full[2048];
        snprintf(full, sizeof full, "%.*s/%s", (int)(sl - from), from, name);
        d = plat_read_file(full, &len);
        if (d) { free(d); return xstrdup0(full); }
    }
    for (int k = 0; k < n->o->nincdirs; k++) {
        char full[2048];
        const char *dir = n->o->incdirs[k];
        size_t dl = strlen(dir);
        snprintf(full, sizeof full, "%s%s%s", dir, dl && dir[dl - 1] == '/' ? "" : "/", name);
        d = plat_read_file(full, &len);
        if (d) { free(d); return xstrdup0(full); }
    }
    return NULL;
}

static void asm_line(struct nas *n, char *s);
static void pp_lines(struct nas *n, struct pl *L, int nl, struct mctx *ctx);

/* a macro body's line with its parameters (%1 %{2} %0 %00 %%local %? %+) */
static char *subst(struct nas *n, struct mctx *c, const char *s)
{
    struct sb b = { 0 };
    const char *p = s;
    (void)n;
    while (*p) {
        if (*p == '\'' || *p == '"' || *p == '`') {
            char q = *p;
            const char *e = p + 1;
            while (*e && *e != q) e++;
            if (*e) e++;
            sb_putn(&b, p, (size_t)(e - p));
            p = e;
            continue;
        }
        if (*p != '%') { sb_putc(&b, *p++); continue; }
        if (p[1] == '%' && idc((unsigned char)p[2])) {
            sb_printf(&b, "..@%ld.", c->id);
            p += 2;
            continue;
        }
        if (p[1] == '0' && p[2] == '0') { sb_puts(&b, c->label ? c->label : ""); p += 3; continue; }
        if (p[1] == '?') { sb_puts(&b, c->m->name); p += 2; continue; }
        if (p[1] == '+') {
            /* concatenation: the spaces either side go */
            while (b.n && isspace((unsigned char)b.p[b.n - 1])) b.p[--b.n] = 0;
            p = skip_sp(p + 2);
            continue;
        }
        int neg = 0, k = 0, br = 0;
        const char *q = p + 1;
        if (*q == '{') { br = 1; q++; if (*q == '-') { neg = 1; q++; } }
        if (!isdigit((unsigned char)*q)) { sb_putc(&b, *p++); continue; }
        while (isdigit((unsigned char)*q)) k = k * 10 + (*q++ - '0');
        if (br) { if (*q != '}') { sb_putc(&b, *p++); continue; } q++; }
        if (k == 0 && !neg) sb_printf(&b, "%d", c->narg);
        else {
            int i = neg ? c->narg - k : k - 1;
            if (i >= 0 && i < c->narg) sb_puts(&b, c->arg[i]);
        }
        p = q;
    }
    return sb_take(&b);
}

/* the arguments of a multi-line macro's call: commas, braces grouping */
static int macro_args(const char *s, char ***out)
{
    int na = 0, cap = 8, depth = 0;
    char **a = xmalloc((size_t)cap * sizeof *a);
    s = skip_sp(s);
    if (!*s) { *out = a; return 0; }
    const char *st = s;
    for (const char *q = s;; q++) {
        if (*q == '\'' || *q == '"' || *q == '`') {
            char c = *q++;
            while (*q && *q != c) q++;
            if (!*q) q--;
            continue;
        }
        if (*q == '{' || *q == '(' || *q == '[') depth++;
        else if (*q == '}' || *q == ')' || *q == ']') depth--;
        if (!*q || (*q == ',' && depth == 0)) {
            const char *b = skip_sp(st);
            size_t l = (size_t)(q - b);
            while (l && isspace((unsigned char)b[l - 1])) l--;
            if (l >= 2 && b[0] == '{' && b[l - 1] == '}') { b++; l -= 2; }
            if (na == cap) { cap *= 2; a = xrealloc(a, (size_t)cap * sizeof *a); }
            a[na++] = xstrndup(b, l);
            if (!*q) break;
            st = q + 1;
        }
    }
    *out = a;
    return na;
}

static struct mmac *mmac_find(struct nas *n, const char *name, size_t len, int na)
{
    for (int k = n->nmm - 1; k >= 0; k--) {
        struct mmac *m = &n->mm[k];
        if (strlen(m->name) != len || strncmp(m->name, name, len)) continue;
        if (na < 0) return m;
        if (na >= m->minp - m->ndflt && (na <= m->maxp || m->greedy))
            return m;
    }
    return NULL;
}

/* the directive word at s (`%define`): its length past the %, 0 if none */
static int pp_word(const char *s, char *w, int cap)
{
    if (*s != '%') return 0;
    int k = 0;
    s++;
    while (isalpha((unsigned char)s[k]) && k < cap - 1) { w[k] = (char)tolower((unsigned char)s[k]); k++; }
    w[k] = 0;
    return k;
}

static int cond_active(struct nas *n)
{
    for (int k = 0; k < n->ncs; k++)
        if (!n->cs[k].active) return 0;
    return 1;
}

/* the value of an %if-family test, after its word */
static int cond_test(struct nas *n, const char *w, const char *arg, struct loc at)
{
    const char *base = w + (w[0] == 'e' && w[1] == 'l' ? 4 : 2);   /* past if / elif */
    int neg = 0;
    if (base[0] == 'n' && strcmp(base, "num")) { neg = 1; base++; }
    int v = 0;
    if (!*base) {
        char *ex = expand(n, arg, 0);
        long long r;
        v = peval(n, at, ex, &r, "%if's condition") && r != 0;
        free(ex);
    } else if (!strcmp(base, "def")) {
        const char *a = skip_sp(arg);
        size_t l = 0;
        while (idc((unsigned char)a[l])) l++;
        v = smac_find(n, a, l) != NULL;
    } else if (!strcmp(base, "macro")) {
        const char *a = skip_sp(arg);
        size_t l = 0;
        while (idc((unsigned char)a[l])) l++;
        v = mmac_find(n, a, l, -1) != NULL;
    } else if (!strcmp(base, "idn") || !strcmp(base, "idni")) {
        char *ex = expand(n, arg, 0);
        char *c = strchr(ex, ',');
        if (!c) { nerr(n, at, "%%ifidn takes two texts and a comma"); free(ex); return 0; }
        *c = 0;
        char *a = (char *)skip_sp(ex), *b = (char *)skip_sp(c + 1);
        rtrim(a); rtrim(b);
        v = base[3] == 'i' ? ieq(a, b) : !strcmp(a, b);
        free(ex);
    } else if (!strcmp(base, "empty")) {
        v = !*skip_sp(arg);
    } else if (!strcmp(base, "num")) {
        char *ex = expand(n, arg, 0);
        const char *a = skip_sp(ex);
        unsigned long long u;
        size_t l = strlen(a);
        while (l && isspace((unsigned char)a[l - 1])) l--;
        v = l && num_tok(a, (int)l, &u);
        free(ex);
    } else if (!strcmp(base, "str")) {
        char *ex = expand(n, arg, 0);
        const char *a = skip_sp(ex);
        v = *a == '\'' || *a == '"' || *a == '`';
        free(ex);
    } else if (!strcmp(base, "id")) {
        char *ex = expand(n, arg, 0);
        const char *a = skip_sp(ex);
        v = id0((unsigned char)*a);
        free(ex);
    } else {
        nerr(n, at, "%%%s is not a conditional EmbCC's NASM preprocessor has", w);
        return 0;
    }
    return neg ? !v : v;
}

/* where %rep/%macro's body ends: the index of its %endrep/%endmacro */
static int body_end(struct pl *L, int nl, int i, const char *open1, const char *open2,
                    const char *close1, const char *close2)
{
    int depth = 1;
    for (int j = i + 1; j < nl; j++) {
        char w[32];
        const char *s = skip_sp(L[j].t);
        if (!pp_word(s, w, sizeof w)) continue;
        if (!strcmp(w, open1) || (open2 && !strcmp(w, open2))) depth++;
        else if (!strcmp(w, close1) || (close2 && !strcmp(w, close2))) {
            if (--depth == 0) return j;
        }
    }
    return -1;
}

static void pp_lines(struct nas *n, struct pl *L, int nl, struct mctx *ctx)
{
    for (int i = 0; i < nl && !n->exitrep; i++) {
        struct loc at = L[i].at;
        n->at = at;
        char *t = ctx ? subst(n, ctx, L[i].t) : xstrdup0(L[i].t);
        strip_comment(t);
        const char *s = skip_sp(t);
        char w[32];
        int wl = pp_word(s, w, sizeof w);
        const char *arg = wl ? skip_sp(s + 1 + wl) : NULL;
        int active = cond_active(n);
        /* the conditionals: tracked inactive too, for their nesting */
        if (wl && !strncmp(w, "if", 2)) {
            if (n->ncs == 128) { nerr(n, at, "%%if nests more than 128 deep"); free(t); continue; }
            int v = active ? cond_test(n, w, arg, at) : 0;
            n->cs[n->ncs].active = active && v;
            n->cs[n->ncs].taken = !active || v;
            n->ncs++;
            free(t);
            continue;
        }
        if (wl && (!strncmp(w, "elif", 4) || !strcmp(w, "else") || !strcmp(w, "endif"))) {
            if (!n->ncs) { nerr(n, at, "%%%s with no %%if before it", w); free(t); continue; }
            struct cond *c = &n->cs[n->ncs - 1];
            if (!strcmp(w, "endif")) { n->ncs--; free(t); continue; }
            n->ncs--;
            int outer = cond_active(n);
            n->ncs++;
            if (!strcmp(w, "else")) {
                c->active = outer && !c->taken;
                c->taken = 1;
            } else {
                int v = outer && !c->taken ? cond_test(n, w, arg, at) : 0;
                c->active = outer && !c->taken && v;
                if (v) c->taken = 1;
            }
            free(t);
            continue;
        }
        if (!active) {
            /* (a %rep or %macro body skipped whole: its %endrep is not ours) */
            if (wl && (!strcmp(w, "rep") || !strcmp(w, "macro") || !strcmp(w, "imacro"))) {
                int e = !strcmp(w, "rep")
                    ? body_end(L, nl, i, "rep", NULL, "endrep", "endr")
                    : body_end(L, nl, i, "macro", "imacro", "endmacro", "endm");
                if (e > 0) i = e;
            }
            free(t);
            continue;
        }
        if (n->o->pp_only && !wl) {
            char *ex = expand(n, t, 0);
            sb_printf(&n->pp, "%s\n", ex);
            free(ex);
            free(t);
            continue;
        }
        if (wl) {
            if (!strcmp(w, "define") || !strcmp(w, "xdefine") || !strcmp(w, "idefine") ||
                !strcmp(w, "xidefine")) {
                const char *p = arg;
                size_t l = 0;
                while (idc((unsigned char)p[l])) l++;
                if (!l) { nerr(n, at, "%%%s needs a name", w); free(t); continue; }
                char *name = xstrndup(p, l);
                p += l;
                int np = -1;
                char **par = NULL;
                if (*p == '(') {
                    const char *e;
                    np = smac_args(p, &par, &e);
                    if (np < 0) { nerr(n, at, "%%%s %s(: the parameters do not close", w, name); free(name); free(t); continue; }
                    if (np == 1 && !par[0][0]) { free(par[0]); np = 0; }
                    p = e;
                }
                const char *body = skip_sp(p);
                char *bx = w[0] == 'x' ? expand(n, body, 0) : xstrdup0(body);
                smac_def(n, name, np, par, bx, w[0] == 'i' || w[1] == 'i');
                free(bx);
                free(name);
            } else if (!strcmp(w, "undef")) {
                char *name = xstrdup0(arg);
                rtrim(name);
                smac_undef(n, name);
                free(name);
            } else if (!strcmp(w, "assign") || !strcmp(w, "iassign")) {
                const char *p = arg;
                size_t l = 0;
                while (idc((unsigned char)p[l])) l++;
                char *name = xstrndup(p, l);
                char *ex = expand(n, p + l, 0);
                long long v;
                if (peval(n, at, ex, &v, "%assign's value")) {
                    char num[32];
                    snprintf(num, sizeof num, "%lld", v);
                    smac_def(n, name, -1, NULL, num, w[0] == 'i');
                }
                free(ex);
                free(name);
            } else if (!strcmp(w, "strlen")) {
                const char *p = arg;
                size_t l = 0;
                while (idc((unsigned char)p[l])) l++;
                char *name = xstrndup(p, l);
                char *ex = expand(n, p + l, 0);
                const char *q = skip_sp(ex), *end;
                unsigned char buf[4096];
                int len = (*q == '\'' || *q == '"' || *q == '`')
                              ? str_tok(q, buf, (int)sizeof buf, &end) : -1;
                if (len < 0) nerr(n, at, "%%strlen takes a name and a string");
                else {
                    char num[32];
                    snprintf(num, sizeof num, "%d", len);
                    smac_def(n, name, -1, NULL, num, 0);
                }
                free(ex);
                free(name);
            } else if (!strcmp(w, "macro") || !strcmp(w, "imacro")) {
                int e = body_end(L, nl, i, "macro", "imacro", "endmacro", "endm");
                if (e < 0) { nerr(n, at, "%%macro with no %%endmacro"); free(t); return; }
                const char *p = arg;
                size_t l = 0;
                while (idc((unsigned char)p[l])) l++;
                if (!l) { nerr(n, at, "%%macro needs a name"); free(t); i = e; continue; }
                if (n->nmm == n->capmm) {
                    n->capmm = n->capmm ? n->capmm * 2 : 16;
                    n->mm = xrealloc(n->mm, (size_t)n->capmm * sizeof *n->mm);
                }
                struct mmac *m = &n->mm[n->nmm++];
                memset(m, 0, sizeof *m);
                m->name = xstrndup(p, l);
                p = skip_sp(p + l);
                char *e2;
                m->minp = (int)strtol(p, &e2, 10);
                m->maxp = m->minp;
                p = e2;
                if (*p == '-') {
                    p++;
                    if (*p == '*') { m->maxp = 1 << 20; p++; }
                    else { m->maxp = (int)strtol(p, &e2, 10); p = e2; }
                }
                if (*p == '+') { m->greedy = 1; p++; }
                p = skip_sp(p);
                if (!ncase(p, ".nolist", 7)) p = skip_sp(p + 7);
                if (*p) {
                    char **d;
                    m->ndflt = macro_args(p, &d);
                    m->dflt = d;
                }
                m->nbody = e - i - 1;
                m->body = xmalloc((size_t)(m->nbody + 1) * sizeof *m->body);
                for (int j = 0; j < m->nbody; j++) {
                    m->body[j].t = xstrdup0(L[i + 1 + j].t);
                    m->body[j].at = L[i + 1 + j].at;
                }
                i = e;
            } else if (!strcmp(w, "unmacro")) {
                /* (NASM's: the macro gone) */
                const char *p = arg;
                size_t l = 0;
                while (idc((unsigned char)p[l])) l++;
                struct mmac *m = mmac_find(n, p, l, -1);
                if (m) m->name[0] = 0;
            } else if (!strcmp(w, "rep")) {
                int e = body_end(L, nl, i, "rep", NULL, "endrep", "endr");
                if (e < 0) { nerr(n, at, "%%rep with no %%endrep"); free(t); return; }
                char *ex = expand(n, arg, 0);
                long long cnt;
                if (peval(n, at, ex, &cnt, "%rep's count")) {
                    if (cnt > 1000000) nerr(n, at, "%%rep %lld: more than a million", cnt);
                    for (long long k = 0; k < cnt && !n->exitrep && n->errors < 50; k++)
                        pp_lines(n, L + i + 1, e - i - 1, ctx);
                    n->exitrep = 0;
                }
                free(ex);
                i = e;
            } else if (!strcmp(w, "exitrep")) {
                n->exitrep = 1;
            } else if (!strcmp(w, "rotate")) {
                if (!ctx) nerr(n, at, "%%rotate outside a macro");
                else {
                    char *ex = expand(n, arg, 0);
                    long long r;
                    if (peval(n, at, ex, &r, "%rotate's count") && ctx->narg) {
                        int k = (int)(((r % ctx->narg) + ctx->narg) % ctx->narg);
                        while (k--) {
                            char *f = ctx->arg[0];
                            memmove(ctx->arg, ctx->arg + 1, (size_t)(ctx->narg - 1) * sizeof *ctx->arg);
                            ctx->arg[ctx->narg - 1] = f;
                        }
                    }
                    free(ex);
                }
            } else if (!strcmp(w, "include")) {
                char *ex = expand(n, arg, 0);
                const char *q = skip_sp(ex), *end;
                unsigned char nm[1024];
                int len = (*q == '"' || *q == '\'' || *q == '<')
                              ? (*q == '<' ? -1 : str_tok(q, nm, (int)sizeof nm - 1, &end)) : -1;
                if (*q == '<') {
                    const char *g = strchr(q, '>');
                    if (g) { len = (int)(g - q - 1); memcpy(nm, q + 1, (size_t)len); }
                }
                if (len < 0) nerr(n, at, "%%include takes a file name in quotes");
                else {
                    nm[len] = 0;
                    char *path = find_file(n, (const char *)nm, at.file);
                    long flen;
                    char *src = path ? plat_read_file(path, &flen) : NULL;
                    if (!src) nerr(n, at, "%%include: cannot find '%s'", (const char *)nm);
                    else if (n->depth > 64) nerr(n, at, "%%include nests more than 64 deep");
                    else {
                        int nn;
                        struct pl *IL = split_lines(n, src, path, &nn);
                        n->depth++;
                        pp_lines(n, IL, nn, NULL);
                        n->depth--;
                        for (int k = 0; k < nn; k++) free(IL[k].t);
                        free(IL);
                    }
                    free(src);
                    free(path);
                }
                free(ex);
            } else if (!strcmp(w, "error") || !strcmp(w, "fatal")) {
                char *ex = expand(n, arg, 0);
                nerr(n, at, "%s", ex);
                free(ex);
                if (w[0] == 'f') { free(t); return; }
            } else if (!strcmp(w, "warning")) {
                char *ex = expand(n, arg, 0);
                nwarn(at, "%s", ex);
                free(ex);
            } else if (!strcmp(w, "line") || !strcmp(w, "pragma") || !strcmp(w, "use") ||
                       !strcmp(w, "clear")) {
                /* (nothing here: line markers, pragmas, packages) */
            } else {
                nerr(n, at, "%%%s is not a directive EmbCC's NASM preprocessor has", w);
            }
            free(t);
            continue;
        }
        /* a multi-line macro's call, a label before it or not */
        char *ex = expand(n, t, 0);
        free(t);
        {
            const char *p = skip_sp(ex), *lab = NULL;
            size_t ll = 0;
            const char *q = p;
            while (idc((unsigned char)*q)) q++;
            struct mmac *m = NULL;
            const char *rest = NULL;
            if (q > p) {
                char **args;
                const char *r = skip_sp(q);
                int na;
                if (*r != ':') {
                    na = macro_args(r, &args);
                    m = mmac_find(n, p, (size_t)(q - p), na);
                    for (int k = 0; k < na; k++) free(args[k]);
                    free(args);
                    rest = r;
                }
                if (!m) {
                    /* label: macro args */
                    const char *r2 = r;
                    if (*r2 == ':') r2 = skip_sp(r2 + 1);
                    const char *q2 = r2;
                    while (idc((unsigned char)*q2)) q2++;
                    if (q2 > r2) {
                        na = macro_args(q2, &args);
                        m = mmac_find(n, r2, (size_t)(q2 - r2), na);
                        for (int k = 0; k < na; k++) free(args[k]);
                        free(args);
                        if (m) { lab = p; ll = (size_t)(q - p); rest = q2; }
                    }
                }
            }
            if (m && m->name[0]) {
                struct mctx c = { m, NULL, 0, NULL, ++n->uniq };
                char *label = lab ? xstrndup(lab, ll) : NULL;
                if (label) {
                    char *lt = xmalloc(ll + 2);
                    snprintf(lt, ll + 2, "%s:", label);
                    asm_line(n, lt);
                    free(lt);
                }
                c.label = label;
                c.narg = macro_args(rest, &c.arg);
                /* the greedy last parameter takes the rest, commas and all */
                if (m->greedy && c.narg > m->maxp && m->maxp > 0) {
                    struct sb g = { 0 };
                    for (int k = m->maxp - 1; k < c.narg; k++) {
                        if (k > m->maxp - 1) sb_putc(&g, ',');
                        sb_puts(&g, c.arg[k]);
                        free(c.arg[k]);
                    }
                    c.arg[m->maxp - 1] = sb_take(&g);
                    c.narg = m->maxp;
                }
                /* defaults for the parameters not given */
                if (c.narg < m->maxp && m->ndflt) {
                    int want = m->maxp < m->minp + m->ndflt ? m->maxp : m->minp + m->ndflt;
                    c.arg = xrealloc(c.arg, (size_t)(want + 1) * sizeof *c.arg);
                    while (c.narg < want) {
                        int di = c.narg - m->minp;
                        c.arg[c.narg] = xstrdup0(di >= 0 && di < m->ndflt ? m->dflt[di] : "");
                        c.narg++;
                    }
                }
                if (n->depth > 200) nerr(n, at, "macro '%s' nests more than 200 deep (does it call itself?)", m->name);
                else {
                    n->depth++;
                    pp_lines(n, m->body, m->nbody, &c);
                    n->depth--;
                }
                for (int k = 0; k < c.narg; k++) free(c.arg[k]);
                free(c.arg);
                free(label);
                free(ex);
                continue;
            }
        }
        n->at = at;
        asm_line(n, ex);
        free(ex);
    }
}

/* ---- names and expressions into GNU syntax ------------------------------- */

/* a NASM name as the object's: .local is the last label's, ..@x a
 * macro's own (an assembler-local .L name), $name the name */
static void map_sym(struct nas *n, const char *s, size_t l, struct sb *b)
{
    if (l >= 3 && !strncmp(s, "..@", 3)) {
        sb_puts(b, ".Lm.");
        for (size_t k = 3; k < l; k++) sb_putc(b, s[k] == '@' ? '_' : s[k]);
        return;
    }
    if (l >= 2 && s[0] == '.' && s[1] == '.') { sb_putn(b, s, l); return; }
    if (s[0] == '.') { sb_puts(b, n->lastg); sb_putn(b, s, l); return; }
    if (s[0] == '$') { s++; l--; }
    sb_putn(b, s, l);
}

static int bad_name_char(const char *s, size_t l)
{
    for (size_t k = 0; k < l; k++)
        if (s[k] == '?' || s[k] == '#' || s[k] == '~' || (s[k] == '@' && !(k == 2 && s[0] == '.' && s[1] == '.')))
            return s[k];
    return 0;
}

/* an expression or an instruction's operands, NASM's into GNU's: numbers
 * in C's spelling, strings as numbers, $ as ., $$ as the section's
 * start, names mapped, // and %% as / and %, `wrt ..plt` as @PLT */
static char *nexpr(struct nas *n, const char *s)
{
    struct sb b = { 0 };
    const char *p = s;
    while (*p) {
        if (*p == '{') {                 /* AVX-512's {k1} {z} {1to16}: as they are */
            const char *e = strchr(p, '}');
            if (e) { sb_putn(&b, p, (size_t)(e + 1 - p)); p = e + 1; continue; }
        }
        if (*p == '\'' || *p == '"' || *p == '`') {
            unsigned char buf[16];
            const char *end;
            int len = str_tok(p, buf, (int)sizeof buf, &end);
            if (len < 0) { nerr(n, n->at, "a string that does not close"); break; }
            if (len > 8) nerr(n, n->at, "a %d-character string is not a number", len);
            unsigned long long v = 0;
            for (int k = (len > 8 ? 8 : len) - 1; k >= 0; k--) v = v << 8 | buf[k];
            sb_printf(&b, "0x%llx", v);
            p = end;
            continue;
        }
        if (isdigit((unsigned char)*p) || (*p == '$' && isdigit((unsigned char)p[1]))) {
            const char *q = p + 1;
            while (isalnum((unsigned char)*q) || *q == '_') q++;
            unsigned long long v;
            if (!num_tok(p, (int)(q - p), &v)) {
                nerr(n, n->at, "'%.*s' is not a number", (int)(q - p), p);
                v = 0;
            }
            sb_printf(&b, v > 0x7fffffffULL ? "0x%llx" : "%llu", v);
            p = q;
            continue;
        }
        if (*p == '$' && p[1] == '$') {
            sb_printf(&b, ".Lss.%d", n->cur);
            p += 2;
            continue;
        }
        if (*p == '$' && !idc((unsigned char)p[1])) { sb_putc(&b, '.'); p++; continue; }
        if (id0((unsigned char)*p)) {
            const char *q = p + 1;
            while (idc((unsigned char)*q)) q++;
            size_t l = (size_t)(q - p);
            /* `X wrt ..plt` */
            const char *r = skip_sp(q);
            if (!ncase(r, "wrt", 3) && isspace((unsigned char)r[3])) {
                const char *k = skip_sp(r + 3);
                const char *ke = k;
                while (idc((unsigned char)*ke)) ke++;
                map_sym(n, p, l, &b);
                if (ke - k == 5 && !ncase(k, "..plt", 5)) sb_puts(&b, "@PLT");
                else if (ke - k == 10 && !ncase(k, "..gotpcrel", 10)) sb_puts(&b, "@GOTPCREL");
                else if (ke - k == 5 && !ncase(k, "..sym", 5)) ;
                else nerr(n, n->at, "'wrt %.*s' is not one EmbCC writes", (int)(ke - k), k);
                p = ke;
                continue;
            }
            int bad = bad_name_char(p, l);
            if (bad && !(l == 1 && *p == '?'))
                nerr(n, n->at, "'%.*s': EmbCC's object names have no '%c'", (int)l, p, bad);
            map_sym(n, p, l, &b);
            p = q;
            continue;
        }
        if (p[0] == '/' && p[1] == '/') { sb_putc(&b, '/'); p += 2; continue; }
        if (p[0] == '%' && p[1] == '%') { sb_putc(&b, '%'); p += 2; continue; }
        if (*p == ';' || *p == '#') { nerr(n, n->at, "'%c' in an expression", *p); p++; continue; }
        sb_putc(&b, *p++);
    }
    return sb_take(&b);
}

/* ---- the directives ------------------------------------------------------ */

static int sec_of(struct nas *n, const char *name, int *fresh)
{
    for (int k = 0; k < n->nsec; k++)
        if (!strcmp(n->secs[k], name)) { *fresh = 0; return k; }
    n->secs = xrealloc(n->secs, (size_t)(n->nsec + 1) * sizeof *n->secs);
    n->secs[n->nsec] = xstrdup0(name);
    *fresh = 1;
    return n->nsec++;
}

static void do_section(struct nas *n, const char *a)
{
    char name[256];
    const char *p = skip_sp(a);
    size_t l = 0;
    while (p[l] && !isspace((unsigned char)p[l])) l++;
    if (!l || l >= sizeof name) { nerr(n, n->at, "section needs a name"); return; }
    memcpy(name, p, l); name[l] = 0;
    p = skip_sp(p + l);
    /* NASM's defaults for ELF, by name */
    int alloc = 1, exec = 0, write = 0, nobits = 0, note = 0, tls = 0;
    long align = 1;
    if (!strcmp(name, ".text")) { exec = 1; align = 16; }
    else if (!strcmp(name, ".rodata") || !strncmp(name, ".rodata.", 8) ||
             !strcmp(name, ".lrodata")) align = 4;
    else if (!strcmp(name, ".data") || !strncmp(name, ".data.", 6) || !strcmp(name, ".ldata")) { write = 1; align = 4; }
    else if (!strcmp(name, ".bss") || !strncmp(name, ".bss.", 5) || !strcmp(name, ".lbss")) { write = 1; nobits = 1; align = 4; }
    else if (!strcmp(name, ".tdata")) { write = 1; tls = 1; align = 4; }
    else if (!strcmp(name, ".tbss")) { write = 1; tls = 1; nobits = 1; align = 4; }
    else if (!strcmp(name, ".comment")) { alloc = 0; }
    else if (!strncmp(name, ".note", 5)) { alloc = 0; note = 1; align = 4; }
    if (n->o->format == NASM_BIN)
        align = !strcmp(name, ".text") ? 1 : 4;
    while (*p) {
        const char *e = p;
        while (*e && !isspace((unsigned char)*e)) e++;
        size_t wl = (size_t)(e - p);
#define W(s) (wl == strlen(s) && !ncase(p, s, wl))
        if (W("alloc")) alloc = 1;
        else if (W("noalloc")) alloc = 0;
        else if (W("exec")) exec = 1;
        else if (W("noexec")) exec = 0;
        else if (W("write")) write = 1;
        else if (W("nowrite")) write = 0;
        else if (W("progbits")) nobits = 0;
        else if (W("nobits")) nobits = 1;
        else if (W("note")) note = 1;
        else if (W("tls")) tls = 1;
        else if (wl > 6 && !ncase(p, "align=", 6)) {
            align = strtol(p + 6, NULL, 0);
            if (align < 1 || (align & (align - 1))) {
                nerr(n, n->at, "section %s: align=%ld is not a power of two", name, align);
                align = 1;
            }
        } else if (n->o->format == NASM_BIN &&
                   (wl > 6 && (!ncase(p, "start=", 6) || !ncase(p, "vstart=", 7) ||
                               !ncase(p, "follows=", 8) || !ncase(p, "vfollows=", 9)))) {
            nerr(n, n->at, "section %s: %.*s: EmbCC's -f bin places sections one after "
                 "another from org, and takes no start/follows yet", name, (int)wl, p);
        } else
            nerr(n, n->at, "section %s: '%.*s' is not a section attribute", name, (int)wl, p);
#undef W
        p = skip_sp(e);
    }
    int fresh;
    n->cur = sec_of(n, name, &fresh);
    n->abs_on = 0;
    emit(n, ".section %s,\"%s%s%s%s\",@%s", name, alloc ? "a" : "", write ? "w" : "",
         exec ? "x" : "", tls ? "T" : "", nobits ? "nobits" : note ? "note" : "progbits");
    if (fresh) {
        emit(n, ".emb_secalign %ld", align);
        emit(n, ".Lss.%d:", n->cur);
    }
}

static void do_global(struct nas *n, const char *a)
{
    char *s = xstrdup0(a);
    for (char *it = strtok(s, ","); it; it = strtok(NULL, ",")) {
        const char *p = skip_sp(it);
        const char *q = p;
        while (idc((unsigned char)*q)) q++;
        if (q == p) { nerr(n, n->at, "global needs a name"); continue; }
        struct sb nm = { 0 };
        map_sym(n, p, (size_t)(q - p), &nm);
        emit(n, ".globl %s", nm.p);
        const char *r = skip_sp(q);
        if (*r == ':') {
            r = skip_sp(r + 1);
            while (*r) {
                const char *e = r;
                while (*e && !isspace((unsigned char)*e)) e++;
                size_t wl = (size_t)(e - r);
                if ((wl == 8 && !ncase(r, "function", 8)) || (wl == 4 && !ncase(r, "func", 4)))
                    emit(n, ".type %s,@function", nm.p);
                else if ((wl == 4 && !ncase(r, "data", 4)) || (wl == 6 && !ncase(r, "object", 6)))
                    emit(n, ".type %s,@object", nm.p);
                else if (wl == 6 && !ncase(r, "hidden", 6)) emit(n, ".hidden %s", nm.p);
                else if (wl == 9 && !ncase(r, "protected", 9)) emit(n, ".protected %s", nm.p);
                else if (wl == 8 && !ncase(r, "internal", 8)) emit(n, ".internal %s", nm.p);
                else if (wl == 7 && !ncase(r, "default", 7)) ;
                else if (wl == 4 && !ncase(r, "weak", 4)) emit(n, ".weak %s", nm.p);
                else {
                    /* the rest is the size */
                    char *x = nexpr(n, r);
                    emit(n, ".size %s, %s", nm.p, x);
                    free(x);
                    break;
                }
                r = skip_sp(e);
            }
        }
        free(nm.p);
    }
    free(s);
}

/* db/dw/dd/dq/dt/do/dy/dz's items: one gas line (`times` repeats it) */
static void do_data(struct nas *n, int size, const char *a)
{
    struct sb b = { 0 };
    const char *gd = size == 1 ? ".byte" : size == 2 ? ".word" : size == 4 ? ".long"
                   : size == 8 ? ".quad" : ".byte";
    sb_puts(&b, gd);
    int first = 1;
    const char *p = skip_sp(a);
    if (!*p) { nerr(n, n->at, "the data directive has no values"); free(b.p); return; }
    while (*p) {
        /* the item: up to a comma outside strings and parentheses */
        const char *st = p;
        int depth = 0;
        while (*p) {
            if (*p == '\'' || *p == '"' || *p == '`') {
                char q = *p++;
                while (*p && *p != q) { if (q == '`' && *p == '\\' && p[1]) p++; p++; }
                if (*p) p++;
                continue;
            }
            if (*p == '(') depth++;
            else if (*p == ')') depth--;
            else if (*p == ',' && depth == 0) break;
            p++;
        }
        char *item = xstrndup(st, (size_t)(p - st));
        rtrim(item);
        const char *it = skip_sp(item);
        if (*p == ',') p = skip_sp(p + 1);
        const char *end = NULL;
        unsigned char sbuf[4096];
        int slen = -1;
        if (*it == '\'' || *it == '"' || *it == '`') {
            slen = str_tok(it, sbuf, (int)sizeof sbuf, &end);
            if (slen >= 0 && *skip_sp(end)) slen = -1;   /* 'a'+1: an expression */
        }
        if (slen >= 0) {
            /* a string: its bytes, padded to the item's size */
            int padded = size > 1 ? (slen + size - 1) / size * size : slen;
            if (size <= 8) {
                for (int k = 0; k < padded; k += size) {
                    unsigned long long v = 0;
                    for (int j = size - 1; j >= 0; j--)
                        v = v << 8 | (k + j < slen ? sbuf[k + j] : 0);
                    sb_printf(&b, "%s0x%llx", first ? " " : ", ", v);
                    first = 0;
                }
            } else {
                for (int k = 0; k < padded; k++) {
                    sb_printf(&b, "%s%d", first ? " " : ", ", k < slen ? sbuf[k] : 0);
                    first = 0;
                }
            }
            free(item);
            continue;
        }
        /* a float: dd's single, dq's double */
        int isf = 0;
        {
            const char *q = it;
            if (*q == '-' || *q == '+') q++;
            if (isdigit((unsigned char)*q)) {
                while (isdigit((unsigned char)*q)) q++;
                if (*q == '.' || ((*q == 'e' || *q == 'E') && (isdigit((unsigned char)q[1]) || q[1] == '-' || q[1] == '+'))) {
                    char *e2;
                    strtod(it, &e2);
                    isf = !*skip_sp(e2);
                }
            }
        }
        if (isf) {
            unsigned long long bits = 0;
            if (size == 4) { float f = strtof(it, NULL); unsigned u; memcpy(&u, &f, 4); bits = u; }
            else if (size == 8) { double d = strtod(it, NULL); memcpy(&bits, &d, 8); }
            else nerr(n, n->at, "a float in a %d-byte item: EmbCC's NASM writes dd and dq ones", size);
            sb_printf(&b, "%s0x%llx", first ? " " : ", ", bits);
            first = 0;
            free(item);
            continue;
        }
        if (!strcmp(it, "?")) {
            if (size <= 8) sb_printf(&b, "%s0", first ? " " : ", ");
            else for (int k = 0; k < size; k++) sb_printf(&b, "%s0", first && !k ? " " : ", ");
            first = 0;
            free(item);
            continue;
        }
        if (size > 8) {
            /* dt/do/dy/dz: a constant, its bytes */
            char *ex = nexpr(n, it);
            long long v;
            if (peval(n, n->at, it, &v, "the value")) {
                for (int k = 0; k < size; k++) {
                    sb_printf(&b, "%s%d", first ? " " : ", ",
                              k < 8 ? (int)((unsigned long long)v >> (8 * k) & 0xff) : (v < 0 ? 0xff : 0));
                    first = 0;
                }
            }
            free(ex);
            free(item);
            continue;
        }
        char *ex = nexpr(n, it);
        sb_printf(&b, "%s%s", first ? " " : ", ", ex);
        first = 0;
        free(ex);
        free(item);
    }
    emit(n, "%s", b.p);
    free(b.p);
}

/* ---- a line -------------------------------------------------------------- */

static int data_size(const char *w, size_t l, int *res)
{
    static const struct { const char *w; int sz, res; } d[] = {
        {"db",1,0},{"dw",2,0},{"dd",4,0},{"dq",8,0},{"dt",10,0},{"do",16,0},{"dy",32,0},{"dz",64,0},
        {"resb",1,1},{"resw",2,1},{"resd",4,1},{"resq",8,1},{"rest",10,1},{"reso",16,1},
        {"resy",32,1},{"resz",64,1},
    };
    for (unsigned k = 0; k < sizeof d / sizeof *d; k++)
        if (strlen(d[k].w) == l && !ncase(w, d[k].w, l)) { *res = d[k].res; return d[k].sz; }
    return 0;
}

static int is_dir_word(const char *w, size_t l)
{
    static const char *const d[] = {
        "equ","times","incbin","align","alignb","section","segment","global","globl","extern",
        "common","static","bits","use16","use32","use64","org","default","cpu","absolute",
        "struc","endstruc","istruc","at","iend","sectalign","float","warning","required", NULL };
    int r;
    if (data_size(w, l, &r)) return 1;
    for (int k = 0; d[k]; k++)
        if (strlen(d[k]) == l && !ncase(w, d[k], l)) return 1;
    return 0;
}

/* the instructions with no operand, which a word alone on a line names
 * (anything else alone is a label) */
static int is_bare_insn(const char *w, size_t l)
{
    static const char *const z[] = {
        "nop","hlt","cli","sti","cld","std","clc","stc","cmc","pause","int3","int1","ud2",
        "cpuid","rdtsc","rdtscp","rdmsr","wrmsr","rdpmc","syscall","sysret","sysretq","swapgs",
        "iret","iretd","iretq","iretw","ret","retf","retn","leave","mfence","lfence","sfence",
        "wbinvd","invd","pushf","popf","pushfd","popfd","pushfq","popfq","pushfw","popfw",
        "pusha","popa","pushad","popad","pushaw","popaw","sahf","lahf","xgetbv","xsetbv",
        "monitor","mwait","stac","clac","fninit","finit","fwait","wait","emms","vzeroupper",
        "vzeroall","endbr64","endbr32","cbw","cwde","cdqe","cwd","cdq","cqo","movsb","movsw",
        "movsd","movsq","stosb","stosw","stosd","stosq","lodsb","lodsw","lodsd","lodsq",
        "cmpsb","cmpsw","cmpsd","cmpsq","scasb","scasw","scasd","scasq","insb","insw","insd",
        "outsb","outsw","outsd","daa","das","aaa","aas","aam","aad","into","salc","xlat",
        "xlatb","rsm","clts","sysenter","sysexit","icebp","fnop","fldz","fld1","fchs","fabs",
        "fsqrt","fsin","fcos","fptan","fpatan","fxch","fcompp","fucompp","fnclex","fclex",
        "lock","rep","repe","repz","repne","repnz", NULL };
    for (int k = 0; z[k]; k++)
        if (strlen(z[k]) == l && !ncase(w, z[k], l)) return 1;
    return 0;
}

static void stmt(struct nas *n, const char *s);

static void define_label(struct nas *n, const char *p, size_t l)
{
    int bad = bad_name_char(p, l);
    if (bad) nerr(n, n->at, "'%.*s': EmbCC's object names have no '%c'", (int)l, p, bad);
    struct sb nm = { 0 };
    map_sym(n, p, l, &nm);
    if (n->abs_on) emit(n, ".set %s, %lld", nm.p, n->abs_pc);
    else emit(n, "%s:", nm.p);
    /* a name not .local (nor ..@, nor ..special) is the one .locals join */
    if (p[0] != '.' && !(l >= 3 && !strncmp(p, "..@", 3))) {
        size_t c = l < sizeof n->lastg - 1 ? l : sizeof n->lastg - 1;
        memcpy(n->lastg, p[0] == '$' ? p + 1 : p, c - (p[0] == '$'));
        n->lastg[c - (p[0] == '$')] = 0;
    }
    free(nm.p);
}

static void asm_line(struct nas *n, char *line)
{
    char *s = line;
    while (isspace((unsigned char)*s)) s++;
    rtrim(s);
    if (!*s) return;
    /* [bits 16], [section .text]: the primitive form */
    if (*s == '[') {
        size_t l = strlen(s);
        if (s[l - 1] == ']') {
            s[l - 1] = 0;
            s++;
            while (isspace((unsigned char)*s)) s++;
            rtrim(s);
            const char *q = s;
            while (isalpha((unsigned char)*q)) q++;
            if (!is_dir_word(s, (size_t)(q - s))) {
                /* ([list +], [map]: listing controls, nothing to assemble) */
                if (!((q - s == 4 && !ncase(s, "list", 4)) || (q - s == 3 && !ncase(s, "map", 3))))
                    nwarn(n->at, "[%s]: not a directive EmbCC's NASM has; ignored", s);
                return;
            }
            stmt(n, s);
            return;
        }
    }
    /* the label */
    const char *p = s, *q = s;
    while (idc((unsigned char)*q) || (q == p && *q == '$')) q++;
    if (q > p && (id0((unsigned char)*p))) {
        const char *r = skip_sp(q);
        size_t l = (size_t)(q - p);
        if (*r == ':') {
            define_label(n, p, l);
            stmt(n, skip_sp(r + 1));
            return;
        }
        /* `name db 1`, `name equ 3`, `name times 4 db 0`, `name` alone */
        const char *w2 = r, *w2e = r;
        while (idc((unsigned char)*w2e)) w2e++;
        if (!is_dir_word(p, l) && !is_bare_insn(p, l)) {
            if (w2e > w2 && w2e - w2 == 3 && !ncase(w2, "equ", 3)) {
                struct sb nm = { 0 };
                map_sym(n, p, l, &nm);
                char *ex = nexpr(n, skip_sp(w2e));
                emit(n, ".set %s, %s", nm.p, ex);
                free(ex);
                free(nm.p);
                return;
            }
            int rr;
            if (!*r || (w2e > w2 && (data_size(w2, (size_t)(w2e - w2), &rr) ||
                                      (w2e - w2 == 5 && !ncase(w2, "times", 5))))) {
                define_label(n, p, l);
                stmt(n, r);
                return;
            }
        }
    }
    stmt(n, s);
}

/* a statement after its label */
static void stmt(struct nas *n, const char *s)
{
    s = skip_sp(s);
    if (!*s) return;
    const char *q = s;
    while (idc((unsigned char)*q)) q++;
    size_t wl = (size_t)(q - s);
    const char *a = skip_sp(q);
#define IS(w) (wl == strlen(w) && !ncase(s, w, wl))
    int res, sz = data_size(s, wl, &res);
    if (n->abs_on && !(sz && res) && !IS("alignb") && !IS("align") && !IS("endstruc") &&
        !IS("section") && !IS("segment") && !IS("absolute") && !IS("bits")) {
        nerr(n, n->at, "only resb..resz go in an absolute block or a struc");
        return;
    }
    if (sz && res) {
        char *ex = expand(n, a, 0);
        if (n->abs_on) {
            long long v;
            if (peval(n, n->at, ex, &v, "the count")) n->abs_pc += v * sz;
        } else {
            char *x = nexpr(n, ex);
            emit(n, ".skip (%s)*%d", x, sz);
            free(x);
        }
        free(ex);
        return;
    }
    if (sz) { do_data(n, sz, a); return; }
    if (IS("times")) {
        /* the count, up to where the statement starts */
        const char *p = a;
        int depth = 0;
        while (*p) {
            if (*p == '(') depth++;
            else if (*p == ')') depth--;
            else if (depth == 0 && isspace((unsigned char)*p)) {
                const char *r = skip_sp(p);
                /* (an operator after a space continues the count) */
                if (!strchr("+-*/%|&^<>~(", *r) && !(p > a && strchr("+-*/%|&^<>~(", p[-1])))
                    break;
            }
            p++;
        }
        char *cnt = xstrndup(a, (size_t)(p - a));
        char *x = nexpr(n, cnt);
        emit(n, ".emb_times %s", x);
        free(x);
        free(cnt);
        const char *r = skip_sp(p);
        if (!*r) { nerr(n, n->at, "times with nothing to repeat"); emit(n, ".skip 0"); return; }
        size_t before = n->nlocs;
        stmt(n, r);
        if ((size_t)n->nlocs != before + 1)
            nerr(n, n->at, "times repeats one statement, and this is %d lines here",
                 (int)(n->nlocs - (int)before));
        return;
    }
    if (IS("equ")) { nerr(n, n->at, "equ needs a name before it"); return; }
    if (IS("incbin")) {
        unsigned char nm[1024];
        const char *end;
        int l = (*a == '"' || *a == '\'' || *a == '`') ? str_tok(a, nm, (int)sizeof nm - 1, &end) : -1;
        if (l < 0) { nerr(n, n->at, "incbin takes a file name in quotes"); return; }
        nm[l] = 0;
        char *path = find_file(n, (const char *)nm, n->at.file);
        if (!path) { nerr(n, n->at, "incbin: cannot find '%s'", (const char *)nm); return; }
        const char *r = skip_sp(end);
        char *x = *r == ',' ? nexpr(n, r) : xstrdup0("");
        emit(n, ".incbin \"%s\"%s", path, x);
        free(x);
        free(path);
        return;
    }
    if (IS("align") || IS("alignb")) {
        const char *c = strchr(a, ',');
        char *v = xstrndup(a, c ? (size_t)(c - a) : strlen(a));
        char *x = nexpr(n, v);
        int fill = IS("align") ? 0x90 : 0;
        if (c) {
            const char *r = skip_sp(c + 1);
            if (!ncase(r, "nop", 3)) fill = 0x90;
            else if (!ncase(r, "db", 2) || !ncase(r, "resb", 4)) {
                const char *t = skip_sp(r + (r[0] == 'd' || r[0] == 'D' ? 2 : 4));
                long long fv = 0;
                if (r[0] == 'd' || r[0] == 'D') {
                    char *fe = nexpr(n, t);
                    if (!peval(n, n->at, t, &fv, "align's fill")) fv = 0;
                    free(fe);
                }
                fill = (int)(fv & 0xff);
            } else nerr(n, n->at, "align's second operand is nop, db N or resb 1");
        }
        if (n->abs_on) {
            long long al;
            if (peval(n, n->at, v, &al, "the alignment") && al > 0)
                n->abs_pc = (n->abs_pc + al - 1) / al * al;
        } else
            emit(n, ".balign %s, %d", x, fill);
        free(x);
        free(v);
        return;
    }
    if (IS("section") || IS("segment")) { do_section(n, a); return; }
    if (IS("global") || IS("globl")) { do_global(n, a); return; }
    if (IS("extern")) {
        /* (its place in the symbol table: NASM's order) */
        char *x = xstrdup0(a);
        for (char *it = strtok(x, ","); it; it = strtok(NULL, ",")) {
            const char *p = skip_sp(it), *e = p;
            while (idc((unsigned char)*e)) e++;
            if (e == p) continue;
            struct sb nm = { 0 };
            map_sym(n, p, (size_t)(e - p), &nm);
            emit(n, ".emb_extern %s", nm.p);
            free(nm.p);
        }
        free(x);
        return;
    }
    if (IS("static") || IS("required")) return;
    if (IS("common")) {
        const char *p = a, *e = p;
        while (idc((unsigned char)*e)) e++;
        struct sb nm = { 0 };
        map_sym(n, p, (size_t)(e - p), &nm);
        const char *r = skip_sp(e);
        const char *col = strchr(r, ':');
        char *szs = xstrndup(r, col ? (size_t)(col - r) : strlen(r));
        char *x = nexpr(n, szs);
        if (n->o->format == NASM_BIN)
            nerr(n, n->at, "common is an object's (a flat binary has no linker to merge it)");
        else if (col) {
            char *y = nexpr(n, skip_sp(col + 1));
            emit(n, ".emb_common %s, %s, %s", nm.p, x, y);
            free(y);
        } else
            emit(n, ".emb_common %s, %s", nm.p, x);
        free(x); free(szs); free(nm.p);
        return;
    }
    if (IS("bits") || IS("use16") || IS("use32") || IS("use64")) {
        long long b = 0;
        if (IS("bits")) { if (!peval(n, n->at, a, &b, "bits")) return; }
        else b = s[3] == '1' ? 16 : s[3] == '3' ? 32 : 64;
        if (b != 16 && b != 32 && b != 64) { nerr(n, n->at, "bits %lld: 16, 32 or 64", b); return; }
        n->mode = (int)b;
        emit(n, ".code%d", (int)b);
        return;
    }
    if (IS("org")) {
        if (n->o->format != NASM_BIN) {
            nerr(n, n->at, "org is a -f bin directive (an object's addresses are the linker's)");
            return;
        }
        long long v;
        char *ex = expand(n, a, 0);
        if (peval(n, n->at, ex, &v, "org's address")) {
            if (n->org_set && v != n->org) nerr(n, n->at, "org is given twice");
            n->org = v;
            n->org_set = 1;
        }
        free(ex);
        return;
    }
    if (IS("default")) {
        if (!ncase(a, "rel", 3)) n->default_rel = 1;
        else if (!ncase(a, "abs", 3)) n->default_rel = 0;
        else if (ncase(a, "bnd", 3) && ncase(a, "nobnd", 5))
            nerr(n, n->at, "default %s: rel or abs", a);
        return;
    }
    if (IS("cpu") || IS("float") || IS("warning") || IS("sectalign")) return;
    if (IS("absolute")) {
        long long v;
        char *ex = expand(n, a, 0);
        if (peval(n, n->at, ex, &v, "absolute's address")) { n->abs_on = 1; n->abs_pc = v; }
        free(ex);
        return;
    }
    if (IS("struc")) {
        const char *e = a;
        while (idc((unsigned char)*e)) e++;
        long long off = 0;
        const char *c = skip_sp(e);
        if (*c == ',') { if (!peval(n, n->at, skip_sp(c + 1), &off, "struc's offset")) off = 0; }
        snprintf(n->struc, sizeof n->struc, "%.*s", (int)(e - a), a);
        n->abs_on = 1;
        n->abs_pc = off;
        define_label(n, a, (size_t)(e - a));
        return;
    }
    if (IS("endstruc")) {
        if (!n->struc[0]) { nerr(n, n->at, "endstruc with no struc"); return; }
        emit(n, ".set %s_size, %lld", n->struc, n->abs_pc);
        n->struc[0] = 0;
        n->abs_on = 0;
        return;
    }
    if (IS("istruc")) {
        const char *e = a;
        while (idc((unsigned char)*e)) e++;
        snprintf(n->istruc, sizeof n->istruc, "%.*s", (int)(e - a), a);
        n->istruc_id++;
        emit(n, ".Lis.%ld:", n->istruc_id);
        return;
    }
    if (IS("at")) {
        const char *c = a;
        int depth = 0;
        while (*c && !(*c == ',' && depth == 0)) { if (*c == '(') depth++; if (*c == ')') depth--; c++; }
        char *f = xstrndup(a, (size_t)(c - a));
        char *x = nexpr(n, f);
        emit(n, ".emb_times (%s) - (. - .Lis.%ld)", x, n->istruc_id);
        emit(n, ".byte 0");
        free(x); free(f);
        if (*c == ',') stmt(n, c + 1);
        return;
    }
    if (IS("iend")) {
        emit(n, ".emb_times %s_size - (. - .Lis.%ld)", n->istruc, n->istruc_id);
        emit(n, ".byte 0");
        return;
    }
#undef IS
    /* an instruction */
    char *x = nexpr(n, s);
    struct x86intel_opts io = { X86I_NASM, n->mode, n->default_rel, 0 };
    char att[1024], err[256];
    if (x86intel_to_att(x, &io, att, (int)sizeof att, err, (int)sizeof err) < 0)
        nerr(n, n->at, "\"%s\": %s", s, err);
    else
        emit(n, "%s", att);
    free(x);
}

/* ---- the file ------------------------------------------------------------ */

int nasm_assemble(const char *in_path, const char *out_path, const struct nasm_opts *o)
{
    long len;
    char *src = plat_read_file(in_path, &len);
    if (!src) {
        fprintf(stderr, "embas: error: cannot read '%s'\n", in_path);
        return 1;
    }
    struct nas n;
    memset(&n, 0, sizeof n);
    n.o = o;
    n.path = in_path;
    n.mode = o->format == NASM_BIN ? 16 : 64;
    n.at.file = in_path;
    n.at.line = 1;
    /* -D and -U, and what NASM defines itself */
    smac_def(&n, "__NASM_MAJOR__", -1, NULL, "2", 0);
    smac_def(&n, "__?NASM_MAJOR?__", -1, NULL, "2", 0);
    smac_def(&n, "__EMBAS__", -1, NULL, "1", 0);
    smac_def(&n, "__OUTPUT_FORMAT__", -1, NULL, o->format == NASM_BIN ? "bin" : "elf64", 0);
    for (int k = 0; k < o->ndefines; k++) {
        const char *d = o->defines[k], *eq = strchr(d, '=');
        char *name = xstrndup(d, eq ? (size_t)(eq - d) : strlen(d));
        smac_def(&n, name, -1, NULL, eq ? eq + 1 : "", 0);
        free(name);
    }
    for (int k = 0; k < o->nundefs; k++)
        smac_undef(&n, o->undefs[k]);
    /* the start: .text, at the code size the format starts in */
    if (!o->pp_only) {
        /* (the implicit .text: not named, so it goes last as NASM's) */
        if (n.mode != 64) emit(&n, ".code%d", n.mode);
        int fresh;
        n.cur = sec_of(&n, ".text", &fresh);
        emit(&n, ".emb_secalign %d", o->format == NASM_BIN ? 1 : 16);
        emit(&n, ".Lss.0:");
    }
    int nl;
    struct pl *L = split_lines(&n, src, in_path, &nl);
    pp_lines(&n, L, nl, NULL);
    if (n.ncs && !n.errors) {
        struct loc end = { in_path, nl };
        nerr(&n, end, "%%if with no %%endif");
    }
    int rc = 1;
    if (o->pp_only) {
        if (!n.errors) {
            FILE *f = out_path && strcmp(out_path, "-") ? fopen(out_path, "w") : stdout;
            if (f) {
                fputs(n.pp.p ? n.pp.p : "", f);
                if (f != stdout) fclose(f);
                rc = 0;
            }
        }
    } else if (!n.errors) {
        struct gas_text_opts go = { n.locs, n.nlocs, o->format == NASM_BIN, n.org, 1 };
        if (getenv("EMBAS_DUMP"))
            fputs(n.out.p ? n.out.p : "", stderr);
        rc = gas_assemble_text(n.out.p ? n.out.p : "", in_path, out_path, &go);
    }
    for (int k = 0; k < nl; k++) free(L[k].t);
    free(L);
    free(src);
    free(n.out.p);
    free(n.pp.p);
    free(n.locs);
    for (int k = 0; k < n.nsm; k++) {
        free(n.sm[k].name); free(n.sm[k].body);
        for (int j = 0; j < n.sm[k].np; j++) free(n.sm[k].par[j]);
        free(n.sm[k].par);
    }
    free(n.sm);
    for (int k = 0; k < n.nmm; k++) {
        struct mmac *m = &n.mm[k];
        for (int j = 0; j < m->nbody; j++) free(m->body[j].t);
        free(m->body);
        for (int j = 0; j < m->ndflt; j++) free(m->dflt[j]);
        free(m->dflt);
        free(m->name);
    }
    free(n.mm);
    for (int k = 0; k < n.nsec; k++) free(n.secs[k]);
    free(n.secs);
    for (int k = 0; k < n.nfiles; k++) free(n.files[k]);
    free(n.files);
    return rc ? 1 : 0;
}

/* ---- embas's command line ------------------------------------------------- */

static void embas_usage(FILE *f)
{
    fprintf(f,
        "usage: embas [--target=TRIPLE] [-mcpu=...] [-o OUT] [-I DIR] [-D NAME[=V]] FILE.s|.S\n"
        "       embas [-f elf64|bin] [-o OUT] [-I DIR] [-D NAME[=V]] [-U NAME] [-E] FILE.asm\n"
        "  EmbCC's assembler, for every target it compiles for: GNU syntax (a .s,\n"
        "  or a .S the C preprocessor reads first) for the target --target names,\n"
        "  as that target's `as` reads it; NASM syntax (a .asm, or -f) for x86 --\n"
        "  16-, 32- and 64-bit code, NASM's preprocessor and directives, an ELF\n"
        "  object or a flat image.\n");
}

static int has_ext(const char *f, const char *e)
{
    size_t fl = strlen(f), el = strlen(e);
    return fl > el && !ncase(f + fl - el, e, el);
}

int embas_main(int argc, char **argv, int (*driver)(int, char **))
{
    struct nasm_opts o;
    memset(&o, 0, sizeof o);
    const char *in = NULL, *out = NULL, *target = NULL;
    int nasm = -1;                       /* -1: by the file's name */
    const char **inc = xcalloc((size_t)argc + 1, sizeof *inc);
    const char **def = xcalloc((size_t)argc + 1, sizeof *def);
    const char **und = xcalloc((size_t)argc + 1, sizeof *und);
    /* what goes to the driver for a GNU-syntax file: the target's own
     * options as they are (-mcpu=, -march=, -mfloat-abi=, -mthumb, -g) */
    char **pass = xcalloc((size_t)argc * 2 + 8, sizeof *pass);
    int np = 0;
    o.incdirs = inc; o.defines = def; o.undefs = und;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = NULL;
#define TAKE(opt) (!strncmp(a, opt, strlen(opt)) && \
                   ((a[strlen(opt)] && (v = a + strlen(opt))) || \
                    (i + 1 < argc && (v = argv[++i]))))
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { embas_usage(stdout); return 0; }
        if (!strcmp(a, "-v") || !strcmp(a, "--version")) {
            printf("embas (EmbCC's assembler: GNU syntax for every target, NASM syntax for x86)\n");
            return 0;
        }
        if (!strcmp(a, "-E") || !strcmp(a, "-e")) { o.pp_only = 1; continue; }
        if (a[0] != '-' || !a[1]) {
            if (in) { fprintf(stderr, "embas: one input file (and '%s' is a second)\n", a); return 2; }
            in = a;
            continue;
        }
        if (!strncmp(a, "--target=", 9)) { target = a + 9; continue; }
        if ((!strcmp(a, "-target") || !strcmp(a, "--target")) && i + 1 < argc) { target = argv[++i]; continue; }
        if (!strcmp(a, "--syntax=nasm")) { nasm = 1; continue; }
        if (!strcmp(a, "--syntax=gnu")) { nasm = 0; continue; }
        if (!strncmp(a, "-m", 2) || !strcmp(a, "-g") || !strncmp(a, "-g", 2)) {
            pass[np++] = (char *)a;
            continue;
        }
        if (!strncmp(a, "-O", 2) || !strncmp(a, "-w", 2) || !strncmp(a, "-W", 2) || !strcmp(a, "-t"))
            continue;                    /* (NASM's optimization, warnings: as NASM's -Ox) */
        if (!strncmp(a, "-f", 2)) {
            if (!TAKE("-f")) { fprintf(stderr, "embas: -f needs a format\n"); return 2; }
            if (nasm < 0) nasm = 1;
            if (!strcmp(v, "elf64") || !strcmp(v, "elfx64")) o.format = NASM_ELF64;
            else if (!strcmp(v, "bin")) o.format = NASM_BIN;
            else if (!strcmp(v, "elf") || !strcmp(v, "elf32") || !strcmp(v, "win64") ||
                     !strcmp(v, "win32") || !strcmp(v, "macho64") || !strcmp(v, "obj") ||
                     !strcmp(v, "coff") || !strcmp(v, "ith") || !strcmp(v, "srec")) {
                fprintf(stderr, "embas: -f %s: EmbCC writes elf64 objects and bin images\n", v);
                return 2;
            } else { fprintf(stderr, "embas: unknown format '%s'\n", v); return 2; }
            continue;
        }
        if (!strncmp(a, "-o", 2)) {
            if (!TAKE("-o")) { fprintf(stderr, "embas: -o needs a file\n"); return 2; }
            out = v;
            continue;
        }
        if (!strncmp(a, "-I", 2) || !strncmp(a, "-i", 2)) {
            if (!TAKE(a[1] == 'I' ? "-I" : "-i")) { fprintf(stderr, "embas: -I needs a directory\n"); return 2; }
            inc[o.nincdirs++] = v;
            continue;
        }
        if (!strncmp(a, "-D", 2) || !strncmp(a, "-d", 2)) {
            if (!TAKE(a[1] == 'D' ? "-D" : "-d")) { fprintf(stderr, "embas: -D needs a name\n"); return 2; }
            def[o.ndefines++] = v;
            continue;
        }
        if (!strncmp(a, "-U", 2) || !strncmp(a, "-u", 2)) {
            if (!TAKE(a[1] == 'U' ? "-U" : "-u")) { fprintf(stderr, "embas: -U needs a name\n"); return 2; }
            und[o.nundefs++] = v;
            continue;
        }
        if (!strncmp(a, "-l", 2) || !strncmp(a, "-M", 2) || !strncmp(a, "-F", 2)) {
            fprintf(stderr, "embas: %s: listings, dependency files and debug formats are "
                            "not written yet\n", a);
            return 2;
        }
#undef TAKE
        fprintf(stderr, "embas: unknown option '%s'\n", a);
        return 2;
    }
    if (!in) { embas_usage(stderr); return 2; }
    if (nasm < 0)
        nasm = has_ext(in, ".asm") || has_ext(in, ".nasm");
    if (!nasm) {
        /* GNU syntax: the driver's `embcc [--target=T] [-m...] -c FILE -o OUT` */
        char **dv = xcalloc((size_t)argc * 4 + 16, sizeof *dv);
        int n = 0;
        char tbuf[256], obuf[1024];
        dv[n++] = (char *)"embcc";
        if (target) { snprintf(tbuf, sizeof tbuf, "--target=%s", target); dv[n++] = tbuf; }
        for (int k = 0; k < np; k++) dv[n++] = pass[k];
        for (int k = 0; k < o.nincdirs; k++) { dv[n++] = (char *)"-I"; dv[n++] = (char *)inc[k]; }
        for (int k = 0; k < o.ndefines; k++) { dv[n++] = (char *)"-D"; dv[n++] = (char *)def[k]; }
        for (int k = 0; k < o.nundefs; k++) { dv[n++] = (char *)"-U"; dv[n++] = (char *)und[k]; }
        dv[n++] = (char *)(o.pp_only ? "-E" : "-c");
        dv[n++] = (char *)in;
        if (!out && !o.pp_only) {
            const char *b = strrchr(in, '/');
            b = b ? b + 1 : in;
            const char *dot = strrchr(b, '.');
            snprintf(obuf, sizeof obuf, "%.*s.o", dot ? (int)(dot - b) : (int)strlen(b), b);
            out = obuf;
        }
        if (out) { dv[n++] = (char *)"-o"; dv[n++] = (char *)out; }
        dv[n] = NULL;
        int rc = driver(n, dv);
        free(dv); free(pass); free(inc); free(def); free(und);
        return rc;
    }
    if (target) {
        enum target_arch ta;
        enum target_os tos;
        enum target_fmt tf;
        if (!target_from_triple(target, &ta, &tos, &tf) || ta != TARGET_X86_64) {
            fprintf(stderr, "embas: '%s' is NASM syntax, which is x86's; the target is %s\n",
                    in, target);
            return 2;
        }
    }
    /* NASM's names: the input's with .o for an object, none for an image */
    char defout[1024];
    if (!out && !o.pp_only) {
        const char *b = strrchr(in, '/');
        b = b ? b + 1 : in;
        const char *dot = strrchr(b, '.');
        size_t l = dot ? (size_t)(dot - b) : strlen(b);
        snprintf(defout, sizeof defout, "%.*s%s", (int)l, b, o.format == NASM_BIN ? "" : ".o");
        if (!strcmp(defout, b)) snprintf(defout, sizeof defout, "%.*s.bin", (int)l, b);
        out = defout;
    }
    enum target_arch arch;
    enum target_os os;
    enum target_fmt fmt;
    if (!target_from_triple("x86_64-elf", &arch, &os, &fmt)) {
        fprintf(stderr, "embas: this EmbCC was built without x86\n");
        return 1;
    }
    target_set(arch);
    target_os_set(os);
    target_fmt_set(fmt);
    int rc = nasm_assemble(in, out, &o);
    free(inc); free(def); free(und); free(pass);
    return rc;
}
