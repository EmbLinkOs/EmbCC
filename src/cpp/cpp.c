#include "cpp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../driver/util.h"
#include "../lex/lex.h"
#include "../arch/predef.h"

#define MAX_MACRO_PARAMS 16
#define MAX_INCLUDE_DEPTH 50
#define MAX_COND_DEPTH 64

/* ---- growable text buffer ---- */

struct tbuf {
    char *p;
    size_t len, cap;
};

static void tb_putn(struct tbuf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        while (cap < b->len + n + 1)
            cap *= 2;
        b->p = xrealloc(b->p, cap);
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

static void tb_puts(struct tbuf *b, const char *s) { tb_putn(b, s, strlen(s)); }
static void tb_putc(struct tbuf *b, char c) { tb_putn(b, &c, 1); }

/* ---- macro table ---- */

struct macro {
    const char *name;
    int is_func;
    int is_varargs;       /* trailing ...; extras become __VA_ARGS__ */
    int nparams;
    const char *params[MAX_MACRO_PARAMS];
    const char *body;      /* spliced, comment-stripped replacement text */
    int expanding;         /* self-reference guard */
    int builtin;           /* predefined: a header may redefine it */
    struct macro *next;
};

struct cpp {
    struct macro *macros;
    const char **incdirs;
    int nincdirs;
    int depth;             /* include nesting */
};

/* One input file being scanned. */
struct src {
    struct cpp *cpp;
    const char *file;
    const char *p;
    int line;
    /* Index into incdirs of the directory this file was found in, or -1
     * for the main file / a "..."-relative hit. #include_next resumes
     * the search AFTER it — that is the whole mechanism, and it is how
     * a header can wrap the system one of the same name. */
    int incdir_idx;
};

static void cerr(struct src *s, const char *msg, const char *arg)
{
    if (arg)
        diag_fatal(s->file, s->line, msg, arg);
    diag_fatal(s->file, s->line, "%s", msg);
}

/* A non-fatal preprocessor diagnostic (the compile continues). */
static void cwarn(struct src *s, const char *msg, const char *arg)
{
    fprintf(stderr, "embcc: %s:%d: warning: ", s->file, s->line);
    if (arg)
        fprintf(stderr, msg, arg);
    else
        fprintf(stderr, "%s", msg);
    fprintf(stderr, "\n");
}

static struct macro *find_macro(struct cpp *cpp, const char *name, size_t n)
{
    for (struct macro *m = cpp->macros; m; m = m->next)
        if (strlen(m->name) == n && memcmp(m->name, name, n) == 0)
            return m;
    return NULL;
}

static void undef_macro(struct cpp *cpp, const char *name, size_t n)
{
    for (struct macro **pm = &cpp->macros; *pm; pm = &(*pm)->next)
        if (strlen((*pm)->name) == n && !memcmp((*pm)->name, name, n)) {
            *pm = (*pm)->next;
            return;
        }
}

/* ---- character helpers ---- */

static int is_id0(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int is_idc(char c) { return is_id0(c) || (c >= '0' && c <= '9'); }

/* Copies a string or char literal verbatim; returns chars consumed. */
static size_t copy_literal(const char *p, struct tbuf *out)
{
    char q = p[0];
    size_t i = 1;
    while (p[i] && p[i] != q && p[i] != '\n') {
        if (p[i] == '\\' && p[i + 1])
            i++;
        i++;
    }
    if (p[i] == q)
        i++;
    if (out)
        tb_putn(out, p, i);
    return i;
}

/* ---- expansion ---- */

static void expand_text(struct src *s, const char *text, struct tbuf *out);

/* Collects one macro argument (depth-0 comma or ')' ends it).
 * Returns chars consumed; *out gets the raw argument text, trimmed. */
static size_t collect_arg(struct src *s, const char *p, struct tbuf *arg)
{
    size_t i = 0;
    int depth = 0;
    while (p[i] == ' ' || p[i] == '\t') /* leading space is not arg text */
        i++;
    while (p[i]) {
        char c = p[i];
        if (c == '(') depth++;
        if (c == ')') {
            if (depth == 0)
                break;
            depth--;
        }
        if (c == ',' && depth == 0)
            break;
        if (c == '"' || c == '\'') {
            i += copy_literal(p + i, arg);
            continue;
        }
        if (c == '\n') {
            tb_putc(arg, ' '); /* args may span lines */
            s->line++;
            i++;
            continue;
        }
        tb_putc(arg, c);
        i++;
    }
    /* trim */
    while (arg->len && (arg->p[arg->len - 1] == ' ' ||
                        arg->p[arg->len - 1] == '\t'))
        arg->p[--arg->len] = 0;
    return i;
}

/* Substitutes params/#/## in a function-like macro body. Args are
 * substituted pre-expanded except as # or ## operands (C99 rules,
 * minus the corner cases a compiler earns later). */
static char *subst_body(struct src *s, struct macro *m,
                        char **raw, char **exp)
{
    struct tbuf out = { 0, 0, 0 };
    const char *p = m->body;

    while (*p) {
        if (*p == '"' || *p == '\'') {
            p += copy_literal(p, &out);
            continue;
        }
        if (p[0] == '#' && p[1] == '#') { /* paste: drop the operator,
                * and back up over trailing spaces already emitted */
            while (out.len && out.p[out.len - 1] == ' ')
                out.p[--out.len] = 0;
            p += 2;
            while (*p == ' ' || *p == '\t')
                p++;
            continue;
        }
        if (p[0] == '#' && (is_id0(p[1]) || p[1] == ' ')) { /* stringize */
            const char *q = p + 1;
            while (*q == ' ' || *q == '\t')
                q++;
            size_t n = 0;
            while (is_idc(q[n]))
                n++;
            int pi = -1;
            for (int k = 0; k < m->nparams; k++)
                if (strlen(m->params[k]) == n &&
                    !memcmp(m->params[k], q, n))
                    pi = k;
            if (pi < 0)
                cerr(s, "'#' must be followed by a macro parameter",
                     NULL);
            tb_putc(&out, '"');
            for (const char *r = raw[pi]; *r; r++) {
                if (*r == '"' || *r == '\\')
                    tb_putc(&out, '\\');
                tb_putc(&out, *r);
            }
            tb_putc(&out, '"');
            p = q + n;
            continue;
        }
        if (is_id0(*p)) {
            size_t n = 0;
            while (is_idc(p[n]))
                n++;
            int pi = -1;
            for (int k = 0; k < m->nparams; k++)
                if (strlen(m->params[k]) == n &&
                    !memcmp(m->params[k], p, n))
                    pi = k;
            if (pi >= 0) {
                /* paste operand? use raw text (no pre-expansion) */
                const char *q = p + n;
                while (*q == ' ' || *q == '\t')
                    q++;
                int pasted = (q[0] == '#' && q[1] == '#');
                tb_puts(&out, pasted ? raw[pi] : exp[pi]);
            } else {
                tb_putn(&out, p, n);
            }
            p += n;
            continue;
        }
        tb_putc(&out, *p++);
    }
    return out.p ? out.p : xstrndup("", 0);
}

/* Invokes function-like macro m; *pp points at the '('. Collects the
 * arguments, substitutes, expands the result into out, and leaves *pp
 * just past the ')'. */
static void expand_funclike(struct src *s, struct macro *m,
                            const char **pp, struct tbuf *out)
{
    const char *p = *pp + 1; /* past '(' */
    char *raw[MAX_MACRO_PARAMS];
    char *exp[MAX_MACRO_PARAMS];
    int nargs = 0;

    while (*p == ' ' || *p == '\t')
        p++;
    if (*p != ')') {
        for (;;) {
            if (m->is_varargs && nargs == m->nparams - 1) {
                /* the rest, commas included, is __VA_ARGS__ */
                struct tbuf a = { 0, 0, 0 };
                for (;;) {
                    p += collect_arg(s, p, &a);
                    if (*p == ',') {
                        tb_putc(&a, ',');
                        p++;
                        continue;
                    }
                    break;
                }
                raw[nargs] = a.p ? a.p : xstrndup("", 0);
                struct tbuf e = { 0, 0, 0 };
                expand_text(s, raw[nargs], &e);
                exp[nargs] = e.p ? e.p : xstrndup("", 0);
                nargs++;
                break;
            }
            if (nargs >= m->nparams)
                cerr(s, "too many arguments to macro '%s'", m->name);
            struct tbuf a = { 0, 0, 0 };
            p += collect_arg(s, p, &a);
            raw[nargs] = a.p ? a.p : xstrndup("", 0);
            struct tbuf e = { 0, 0, 0 };
            expand_text(s, raw[nargs], &e);
            exp[nargs] = e.p ? e.p : xstrndup("", 0);
            nargs++;
            if (*p == ',') {
                p++;
                continue;
            }
            break;
        }
    }
    if (*p != ')')
        cerr(s, "unterminated call to macro '%s'", m->name);
    p++;
    if (m->is_varargs && nargs == m->nparams - 1) {
        raw[nargs] = xstrndup("", 0); /* empty __VA_ARGS__ is legal */
        exp[nargs] = xstrndup("", 0);
        nargs++;
    }
    if (nargs == 0 && m->nparams == 1) {
        raw[0] = xstrndup("", 0);    /* M(): one empty argument (C99) */
        exp[0] = xstrndup("", 0);
        nargs = 1;
    }
    if (nargs != m->nparams)
        cerr(s, "wrong number of arguments to macro '%s'", m->name);

    char *body = subst_body(s, m, raw, exp);
    m->expanding = 1;
    expand_text(s, body, out);
    m->expanding = 0;
    free(body);
    for (int k = 0; k < nargs; k++) {
        free(raw[k]);
        free(exp[k]);
    }
    *pp = p;
}

/* Expands `text` (already comment-stripped and spliced) into out. */
static void expand_text(struct src *s, const char *text, struct tbuf *out)
{
    const char *p = text;

    while (*p) {
        if (*p == '"' || *p == '\'') {
            p += copy_literal(p, out);
            continue;
        }
        if (!is_id0(*p)) {
            tb_putc(out, *p++);
            continue;
        }
        size_t n = 0;
        while (is_idc(p[n]))
            n++;

        /* dynamic predefined macros */
        if (n == 8 && !memcmp(p, "__FILE__", 8)) {
            tb_putc(out, '"');
            tb_puts(out, s->file);
            tb_putc(out, '"');
            p += n;
            continue;
        }
        if (n == 8 && !memcmp(p, "__LINE__", 8)) {
            char buf[16];
            snprintf(buf, sizeof buf, "%d", s->line);
            tb_puts(out, buf);
            p += n;
            continue;
        }

        struct macro *m = find_macro(s->cpp, p, n);
        if (!m || m->expanding) {
            tb_putn(out, p, n);
            p += n;
            continue;
        }
        if (m->is_func) {
            /* function-like: only with an argument list */
            const char *q = p + n;
            while (*q == ' ' || *q == '\t')
                q++;
            if (*q != '(') {
                tb_putn(out, p, n);
                p += n;
                continue;
            }
            p = q;
            diag_register_expansion(s->file, s->line, m->name);
            expand_funclike(s, m, &p, out);
        } else {
            diag_register_expansion(s->file, s->line, m->name);
            struct tbuf tmp = { 0, 0, 0 };
            m->expanding = 1;
            expand_text(s, m->body, &tmp);
            m->expanding = 0;
            p += n;
            /* Alias-to-function-like (the cdefs.h idiom
             * '#define A B' with B function-like): the expansion ends
             * in B's name, and OUR input supplies the '(args)'. */
            size_t tn = tmp.len;
            while (tn && is_idc(tmp.p[tn - 1]))
                tn--;
            if (tmp.len && tn < tmp.len && is_id0(tmp.p[tn])) {
                struct macro *fm = find_macro(s->cpp, tmp.p + tn,
                                              tmp.len - tn);
                const char *q2 = p;
                while (*q2 == ' ' || *q2 == '\t')
                    q2++;
                if (fm && fm->is_func && !fm->expanding && *q2 == '(') {
                    tb_putn(out, tmp.p, tn);
                    p = q2;
                    expand_funclike(s, fm, &p, out);
                    free(tmp.p);
                    continue;
                }
            }
            if (tmp.p) {
                tb_putn(out, tmp.p, tmp.len);
                free(tmp.p);
            }
        }
    }
}

/* ---- #if expression evaluation ---- */

struct evalp {
    struct src *s;
    const char *p;
};

static long eval_or(struct evalp *e);

static void eskip(struct evalp *e)
{
    while (*e->p == ' ' || *e->p == '\t')
        e->p++;
}

static long eval_primary(struct evalp *e)
{
    eskip(e);
    if (*e->p == '(') {
        e->p++;
        long v = eval_or(e);
        eskip(e);
        if (*e->p != ')')
            cerr(e->s, "expected ')' in #if expression", NULL);
        e->p++;
        return v;
    }
    if (*e->p == '!') {
        e->p++;
        return !eval_primary(e);
    }
    if (*e->p == '~') {
        e->p++;
        return ~eval_primary(e);
    }
    if (*e->p == '-') {
        e->p++;
        return -eval_primary(e);
    }
    if (*e->p == '+') {
        e->p++;
        return eval_primary(e);
    }
    /* A character constant, optionally L/u/U-prefixed: decoded and valued
     * by the SAME functions the lexer uses (lex.c), so #if agrees with the
     * code it guards — including '\xFF' being -1 where char is signed. */
    int cpfx = 0;
    if ((*e->p == 'L' || *e->p == 'u' || *e->p == 'U') && e->p[1] == '\'') {
        cpfx = *e->p;
        e->p++;
    }
    if (*e->p == '\'') {
        const char *q = e->p + 1;
        struct litch c;
        if (*q == '\\') {
            q++;
            c = lit_decode(&q, 1, e->s->file, e->s->line);
        } else if (*q && *q != '\'' && *q != '\n') {
            c = lit_decode(&q, 0, e->s->file, e->s->line);
        } else {
            cerr(e->s, "bad character constant in #if", NULL);
            return 0;
        }
        if (*q != '\'')
            cerr(e->s, "bad character constant in #if", NULL);
        e->p = q + 1;
        int uns;
        return lit_char_value(c, cpfx, &uns, e->s->file, e->s->line);
    }
    if (*e->p >= '0' && *e->p <= '9') {
        char *end;
        long v = strtol(e->p, &end, 0);
        while (*end == 'u' || *end == 'U' || *end == 'l' || *end == 'L')
            end++;
        e->p = end;
        return v;
    }
    if (is_id0(*e->p)) { /* surviving identifiers evaluate to 0 */
        while (is_idc(*e->p))
            e->p++;
        return 0;
    }
    cerr(e->s, "cannot parse #if expression", NULL);
    return 0;
}

#define EVAL_LEVEL(name, next, body)                                     \
    static long name(struct evalp *e)                                    \
    {                                                                    \
        long a = next(e);                                                \
        for (;;) {                                                       \
            eskip(e);                                                    \
            body                                                         \
            return a;                                                    \
        }                                                                \
    }

EVAL_LEVEL(eval_mul, eval_primary, {
    if (*e->p == '*') { e->p++; a *= eval_primary(e); continue; }
    if (*e->p == '/' ) { e->p++; long b = eval_primary(e);
        if (!b) cerr(e->s, "division by zero in #if", NULL);
        a /= b; continue; }
    if (*e->p == '%') { e->p++; long b = eval_primary(e);
        if (!b) cerr(e->s, "division by zero in #if", NULL);
        a %= b; continue; }
})
EVAL_LEVEL(eval_add, eval_mul, {
    if (*e->p == '+') { e->p++; a += eval_mul(e); continue; }
    if (*e->p == '-') { e->p++; a -= eval_mul(e); continue; }
})
EVAL_LEVEL(eval_shift, eval_add, {
    if (e->p[0] == '<' && e->p[1] == '<') { e->p += 2;
        a <<= eval_add(e); continue; }
    if (e->p[0] == '>' && e->p[1] == '>') { e->p += 2;
        a >>= eval_add(e); continue; }
})
EVAL_LEVEL(eval_rel, eval_shift, {
    if (e->p[0] == '<' && e->p[1] == '=') { e->p += 2;
        a = a <= eval_shift(e); continue; }
    if (e->p[0] == '>' && e->p[1] == '=') { e->p += 2;
        a = a >= eval_shift(e); continue; }
    if (e->p[0] == '<' && e->p[1] != '<') { e->p++;
        a = a < eval_shift(e); continue; }
    if (e->p[0] == '>' && e->p[1] != '>') { e->p++;
        a = a > eval_shift(e); continue; }
})
EVAL_LEVEL(eval_eq, eval_rel, {
    if (e->p[0] == '=' && e->p[1] == '=') { e->p += 2;
        a = a == eval_rel(e); continue; }
    if (e->p[0] == '!' && e->p[1] == '=') { e->p += 2;
        a = a != eval_rel(e); continue; }
})
EVAL_LEVEL(eval_band, eval_eq, {
    if (e->p[0] == '&' && e->p[1] != '&') { e->p++;
        a &= eval_eq(e); continue; }
})
EVAL_LEVEL(eval_bxor, eval_band, {
    if (*e->p == '^') { e->p++; a ^= eval_band(e); continue; }
})
EVAL_LEVEL(eval_bor, eval_bxor, {
    if (e->p[0] == '|' && e->p[1] != '|') { e->p++;
        a |= eval_bxor(e); continue; }
})
EVAL_LEVEL(eval_and, eval_bor, {
    if (e->p[0] == '&' && e->p[1] == '&') { e->p += 2;
        long b = eval_bor(e); a = a && b; continue; }
})
EVAL_LEVEL(eval_or_, eval_and, {
    if (e->p[0] == '|' && e->p[1] == '|') { e->p += 2;
        long b = eval_and(e); a = a || b; continue; }
})

static long eval_or(struct evalp *e) { return eval_or_(e); }

static char *read_file_or_null(const char *path, long *len);
static int (*cxx_has_builtin)(const char *name);
static int cxx_exceptions;

void cpp_set_cxx(int (*has_builtin)(const char *name), int exceptions)
{
    cxx_has_builtin = has_builtin;
    cxx_exceptions = exceptions;
}

/* C++'s (and GNU's) feature-test operators, which a #if may use, and
 * which `defined` / #ifdef count as defined. */
static int has_operator(const char *p, size_t n)
{
    static const char *const ops[] = {
        "__has_include", "__has_include_next", "__has_builtin",
        "__has_attribute", "__has_cpp_attribute", "__has_feature",
        "__has_extension",
    };
    if (!predef_is_cxx())
        return 0;
    for (size_t i = 0; i < sizeof ops / sizeof ops[0]; i++)
        if (strlen(ops[i]) == n && !memcmp(ops[i], p, n))
            return 1;
    return 0;
}

/* The name inside an attribute test, its __x__ spelling reduced to x
 * (and a gnu:: or __gnu__:: scope dropped). */
static void attr_name(const char *p, size_t n, char *out, size_t cap)
{
    if (n > 5 && !memcmp(p, "gnu::", 5))
        p += 5, n -= 5;
    else if (n > 9 && !memcmp(p, "__gnu__::", 9))
        p += 9, n -= 9;
    if (n > 4 && !memcmp(p, "__", 2) && !memcmp(p + n - 2, "__", 2))
        p += 2, n -= 4;
    if (n >= cap)
        n = cap - 1;
    memcpy(out, p, n);
    out[n] = 0;
}

static int gnu_attribute(const char *a)
{
    /* those the C++ front-end honours or can safely ignore */
    static const char *const ok[] = {
        "aligned", "packed", "section", "weak", "used", "noreturn",
        "unused", "deprecated", "always_inline", "noinline", "const",
        "pure", "nothrow", "warn_unused_result", "format", "format_arg",
        "nonnull", "returns_nonnull", "malloc", "cold", "hot",
        "artificial", "gnu_inline", "alloc_size", "alloc_align",
        "abi_tag", "visibility", "externally_visible", "leaf", "flatten",
        "may_alias", "sentinel", "unavailable", "access", "noipa",
        "no_sanitize", "no_sanitize_address", "nodiscard", "maybe_unused",
        "fallthrough", "likely", "unlikely",
    };
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++)
        if (!strcmp(ok[i], a))
            return 1;
    return 0;
}

static long cpp_attribute(const char *a)
{
    static const struct { const char *name; long v; } std[] = {
        { "nodiscard", 201907 }, { "maybe_unused", 201603 },
        { "deprecated", 201309 }, { "fallthrough", 201910 },
        { "likely", 201803 }, { "unlikely", 201803 },
        { "noreturn", 200809 },
    };
    for (size_t i = 0; i < sizeof std / sizeof std[0]; i++)
        if (!strcmp(std[i].name, a))
            return std[i].v;
    return 0;
}

static int include_exists(struct src *s, const char *fname, int angle,
                          int is_next)
{
    char path[512];
    long len;
    char *text = NULL;
    if (!angle && !is_next) {
        const char *slash = strrchr(s->file, '/');
        if (slash)
            snprintf(path, sizeof path, "%.*s/%s", (int)(slash - s->file),
                     s->file, fname);
        else
            snprintf(path, sizeof path, "%s", fname);
        text = read_file_or_null(path, &len);
    }
    for (int i = is_next ? s->incdir_idx + 1 : 0;
         !text && i < s->cpp->nincdirs; i++) {
        snprintf(path, sizeof path, "%s/%s", s->cpp->incdirs[i], fname);
        text = read_file_or_null(path, &len);
    }
    int found = text != NULL;
    free(text);
    return found;
}

/* At a feature-test operator's name (n chars at p): its value, the
 * cursor moved past its parenthesized operand. */
static long eval_has(struct src *s, const char *p, size_t n,
                     const char **pp)
{
    const char *q = p + n;
    while (*q == ' ' || *q == '\t')
        q++;
    if (*q != '(')
        cerr(s, "expected '(' after a feature-test operator", NULL);
    q++;
    while (*q == ' ' || *q == '\t')
        q++;
    long v = 0;
    if (n >= 13 && !memcmp(p, "__has_include", 13)) {
        int angle = *q == '<';
        if (!angle && *q != '"')
            cerr(s, "__has_include needs a header name", NULL);
        char close = angle ? '>' : '"';
        const char *b = ++q;
        while (*q && *q != close)
            q++;
        if (!*q)
            cerr(s, "malformed __has_include", NULL);
        char fname[256];
        size_t fl = (size_t)(q - b) < sizeof fname - 1 ? (size_t)(q - b)
                                                       : sizeof fname - 1;
        memcpy(fname, b, fl);
        fname[fl] = 0;
        q++;
        v = include_exists(s, fname, angle, n == 18);
    } else {
        const char *b = q;
        while (is_idc(*q) || *q == ':')
            q++;
        size_t an = (size_t)(q - b);
        char name[128];
        if (n == 13 && !memcmp(p, "__has_builtin", 13)) {
            if (an >= sizeof name)
                an = sizeof name - 1;
            memcpy(name, b, an);
            name[an] = 0;
            v = cxx_has_builtin ? cxx_has_builtin(name) : 0;
        } else if (n == 15 && !memcmp(p, "__has_attribute", 15)) {
            attr_name(b, an, name, sizeof name);
            v = gnu_attribute(name);
        } else if (n == 19 && !memcmp(p, "__has_cpp_attribute", 19)) {
            int gnu = (an > 5 && !memcmp(b, "gnu::", 5)) ||
                      (an > 9 && !memcmp(b, "__gnu__::", 9));
            attr_name(b, an, name, sizeof name);
            v = gnu ? gnu_attribute(name) : cpp_attribute(name);
        }
        /* __has_feature / __has_extension: clang's, 0 */
    }
    while (*q == ' ' || *q == '\t')
        q++;
    if (*q != ')')
        cerr(s, "expected ')' after a feature-test operand", NULL);
    *pp = q + 1;
    return v;
}

/* Evaluates a #if line: replace defined(X)/defined X first, then
 * macro-expand, then parse the arithmetic. */
static long eval_if(struct src *s, const char *line)
{
    struct tbuf pre = { 0, 0, 0 };
    const char *p = line;
    while (*p) {
        if (*p == '\'' || *p == '"') {
            p += copy_literal(p, &pre);
            continue;
        }
        if (is_id0(*p)) {
            size_t n = 0;
            while (is_idc(p[n]))
                n++;
            if (n == 7 && !memcmp(p, "defined", 7)) {
                const char *q = p + 7;
                while (*q == ' ' || *q == '\t')
                    q++;
                int paren = *q == '(';
                if (paren) {
                    q++;
                    while (*q == ' ' || *q == '\t')
                        q++;
                }
                if (!is_id0(*q))
                    cerr(s, "'defined' needs a name", NULL);
                size_t idn = 0;
                while (is_idc(q[idn]))
                    idn++;
                int have = find_macro(s->cpp, q, idn) != NULL ||
                           (idn == 8 && (!memcmp(q, "__FILE__", 8) ||
                                         !memcmp(q, "__LINE__", 8))) ||
                           has_operator(q, idn);
                q += idn;
                if (paren) {
                    while (*q == ' ' || *q == '\t')
                        q++;
                    if (*q != ')')
                        cerr(s, "expected ')' after defined(", NULL);
                    q++;
                }
                tb_putc(&pre, have ? '1' : '0');
                p = q;
                continue;
            }
            if (has_operator(p, n) && !find_macro(s->cpp, p, n)) {
                char num[32];
                snprintf(num, sizeof num, "%ldL", eval_has(s, p, n, &p));
                tb_putn(&pre, num, strlen(num));
                continue;
            }
            tb_putn(&pre, p, n);
            p += n;
            continue;
        }
        tb_putc(&pre, *p++);
    }

    struct tbuf ex = { 0, 0, 0 };
    expand_text(s, pre.p ? pre.p : "", &ex);
    free(pre.p);
    if (predef_is_cxx() && ex.p) {
        /* operators a macro's expansion produced
         * (libstdc++'s _GLIBCXX_HAS_BUILTIN(B) is __has_builtin(B)) */
        struct tbuf ex2 = { 0, 0, 0 };
        const char *q = ex.p;
        while (*q) {
            if (*q == '\'' || *q == '"') {
                q += copy_literal(q, &ex2);
                continue;
            }
            if (is_id0(*q)) {
                size_t n = 0;
                while (is_idc(q[n]))
                    n++;
                if (has_operator(q, n)) {
                    char num[32];
                    snprintf(num, sizeof num, "%ldL", eval_has(s, q, n, &q));
                    tb_putn(&ex2, num, strlen(num));
                    continue;
                }
                tb_putn(&ex2, q, n);
                q += n;
                continue;
            }
            tb_putc(&ex2, *q++);
        }
        free(ex.p);
        ex = ex2;
    }

    struct evalp e;
    e.s = s;
    e.p = ex.p ? ex.p : "";
    long v = eval_or(&e);
    eskip(&e);
    if (*e.p)
        cerr(s, "trailing junk in #if expression", NULL);
    free(ex.p);
    return v;
}

/* A function-like macro invocation may span physical lines:
 *     int select __P ((int __n, fd_set *__readfds,
 *                      struct timeval *__timeout));
 * The scanner is line-oriented, so an invocation whose ')' has not
 * arrived yet needs more input before it can be expanded. Returns 1
 * when the text ends inside such an argument list. */
static int needs_more_input(struct cpp *cpp, const char *text)
{
    const char *p = text;

    while (*p) {
        if (*p == '"' || *p == '\'') {
            p += copy_literal(p, NULL);
            continue;
        }
        if (!is_id0(*p)) {
            p++;
            continue;
        }
        size_t n = 0;
        while (is_idc(p[n]))
            n++;
        struct macro *m = find_macro(cpp, p, n);
        const char *q = p + n;
        while (*q == ' ' || *q == '\t')
            q++;
        if (m && m->is_func && *q == '(') {
            int depth = 0;
            const char *r = q;
            while (*r) {
                if (*r == '"' || *r == '\'') {
                    r += copy_literal(r, NULL);
                    continue;
                }
                if (*r == '(') {
                    depth++;
                } else if (*r == ')') {
                    if (--depth == 0)
                        break;
                }
                r++;
            }
            if (!*r)
                return 1; /* the ')' never arrived on this line */
            p = r + 1;
            continue;
        }
        p += n;
    }
    return 0;
}

/* ---- the line-oriented driver ---- */

/* Reads one logical line (backslash-newline spliced, comments
 * stripped) from s into out. Returns 0 at EOF. Leaves s->line at the
 * FIRST line of the logical line; *nl gets the newline count. */
static int read_logical_line(struct src *s, struct tbuf *out, int *nl)
{
    *nl = 0;
    if (!*s->p)
        return 0;
    for (;;) {
        char c = *s->p;
        if (!c)
            break;
        if (c == '\n') {
            s->p++;
            (*nl)++;
            break;
        }
        if (c == '\\' && s->p[1] == '\n') {
            s->p += 2;
            (*nl)++;
            continue;
        }
        if (c == '/' && s->p[1] == '/') {
            while (*s->p && *s->p != '\n')
                s->p++;
            continue;
        }
        if (c == '/' && s->p[1] == '*') {
            s->p += 2;
            while (*s->p && !(s->p[0] == '*' && s->p[1] == '/')) {
                if (*s->p == '\n')
                    (*nl)++;
                s->p++;
            }
            if (!*s->p)
                cerr(s, "unterminated comment", NULL);
            s->p += 2;
            tb_putc(out, ' ');
            continue;
        }
        if (c == '"' || c == '\'') {
            s->p += copy_literal(s->p, out);
            continue;
        }
        tb_putc(out, c);
        s->p++;
    }
    return 1;
}

static char *read_file_or_null(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = xmalloc((size_t)*len + 1);
    if (fread(buf, 1, (size_t)*len, f) != (size_t)*len) {
        fclose(f);
        free(buf);
        return NULL;
    }
    buf[*len] = 0;
    fclose(f);
    return buf;
}

static void process_file(struct cpp *cpp, const char *path,
                         const char *src, struct tbuf *out, int incdir_idx);

static void do_include(struct src *s, const char *arg, struct tbuf *out,
                       int is_next)
{
    char path[512];
    char fname[256];
    int angle;
    const char *p = arg;

    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == '"' || *p == '<') {
        angle = *p == '<';
        char close = angle ? '>' : '"';
        p++;
        size_t n = 0;
        while (p[n] && p[n] != close && n < sizeof fname - 1)
            n++;
        if (p[n] != close)
            cerr(s, "malformed #include", NULL);
        memcpy(fname, p, n);
        fname[n] = 0;
    } else {
        cerr(s, "malformed #include", NULL);
        return;
    }

    if (s->cpp->depth >= MAX_INCLUDE_DEPTH)
        cerr(s, "#include nested too deeply", NULL);

    char *text = NULL;
    long len;
    int found_idx = -1;
    /* #include_next starts after the directory THIS file came from;
     * a plain #include starts at the beginning. */
    int start = is_next ? s->incdir_idx + 1 : 0;

    if (!angle && !is_next) {
        /* relative to the including file's directory first */
        const char *slash = strrchr(s->file, '/');
        if (slash)
            snprintf(path, sizeof path, "%.*s/%s",
                     (int)(slash - s->file), s->file, fname);
        else
            snprintf(path, sizeof path, "%s", fname);
        text = read_file_or_null(path, &len);
    }
    for (int i = start; !text && i < s->cpp->nincdirs; i++) {
        snprintf(path, sizeof path, "%s/%s", s->cpp->incdirs[i], fname);
        text = read_file_or_null(path, &len);
        if (text)
            found_idx = i;
    }
    if (!text)
        cerr(s, is_next ? "cannot find a NEXT include file \"%s\""
                        : "cannot find include file \"%s\"", fname);

    s->cpp->depth++;
    {
        char *ipath = xstrndup(path, strlen(path));
        diag_register_source(ipath, text);   /* header errors show their lines */
        process_file(s->cpp, ipath, text, out, found_idx);
    }
    s->cpp->depth--;
}

static int defining_builtins;

static void define_macro(struct src *s, const char *line)
{
    const char *p = line;
    while (*p == ' ' || *p == '\t')
        p++;
    if (!is_id0(*p))
        cerr(s, "#define needs a name", NULL);
    size_t n = 0;
    while (is_idc(p[n]))
        n++;

    struct macro *m = xcalloc(1, sizeof *m);
    m->name = xstrndup(p, n);
    p += n;

    if (*p == '(') { /* function-like: no space before '(' */
        m->is_func = 1;
        p++;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p != ')') {
            for (;;) {
                while (*p == ' ' || *p == '\t')
                    p++;
                if (p[0] == '.' && p[1] == '.' && p[2] == '.') {
                    m->is_varargs = 1;
                    if (m->nparams >= MAX_MACRO_PARAMS)
                        cerr(s, "too many macro parameters", NULL);
                    m->params[m->nparams++] = "__VA_ARGS__";
                    p += 3;
                    while (*p == ' ' || *p == '\t')
                        p++;
                    break;
                }
                if (!is_id0(*p))
                    cerr(s, "bad parameter in #define", NULL);
                size_t pn = 0;
                while (is_idc(p[pn]))
                    pn++;
                if (m->nparams >= MAX_MACRO_PARAMS)
                    cerr(s, "too many macro parameters", NULL);
                m->params[m->nparams++] = xstrndup(p, pn);
                p += pn;
                while (*p == ' ' || *p == '\t')
                    p++;
                if (*p == ',') {
                    p++;
                    continue;
                }
                break;
            }
        }
        if (*p != ')')
            cerr(s, "expected ')' in #define", NULL);
        p++;
    }
    while (*p == ' ' || *p == '\t')
        p++;
    m->body = xstrndup(p, strlen(p));

    struct macro *old = find_macro(s->cpp, m->name, strlen(m->name));
    if (old && old->builtin) {
        undef_macro(s->cpp, m->name, strlen(m->name));
    } else if (old) {
        if (strcmp(old->body, m->body) != 0 ||
            old->is_func != m->is_func || old->nparams != m->nparams) {
            /* gcc warns and installs the new definition rather than refusing;
             * real headers redefine macros (and a -D can clash with a header
             * default), so match that and keep the LAST definition. */
            cwarn(s, "macro '%s' redefined", m->name);
            undef_macro(s->cpp, m->name, strlen(m->name));
            /* fall through to install the new definition */
        } else {
            return; /* identical redefinition is legal */
        }
    }
    m->builtin = defining_builtins;
    m->next = s->cpp->macros;
    s->cpp->macros = m;
}

/* Conditional stack entry state. */
enum cond_state { COND_LIVE, COND_DEAD, COND_DONE };

static void process_file(struct cpp *cpp, const char *path,
                         const char *src, struct tbuf *out, int incdir_idx)
{
    struct src s;
    s.cpp = cpp;
    s.file = path;
    s.p = src;
    s.line = 1;
    s.incdir_idx = incdir_idx;

    enum cond_state cond[MAX_COND_DEPTH];
    int ncond = 0;
    char marker[600];

    snprintf(marker, sizeof marker, "# %d \"%s\"\n", 1, path);
    tb_puts(out, marker);

    struct tbuf lineb = { 0, 0, 0 };
    int nl;
    while (1) {
        lineb.len = 0;
        if (lineb.p)
            lineb.p[0] = 0;
        int startline = s.line;
        if (!read_logical_line(&s, &lineb, &nl))
            break;
        const char *lp = lineb.p ? lineb.p : "";
        while (*lp == ' ' || *lp == '\t')
            lp++;

        int live = 1;
        for (int i = 0; i < ncond; i++)
            if (cond[i] != COND_LIVE)
                live = 0;

        if (*lp == '#') {
            lp++;
            while (*lp == ' ' || *lp == '\t')
                lp++;
            size_t dn = 0;
            while (is_idc(lp[dn]))
                dn++;
            const char *arg = lp + dn;
            while (*arg == ' ' || *arg == '\t')
                arg++;

#define DIR(x) (dn == strlen(x) && !memcmp(lp, x, dn))
            if (DIR("ifdef") || DIR("ifndef")) {
                if (ncond >= MAX_COND_DEPTH)
                    cerr(&s, "conditionals nested too deeply", NULL);
                int have = 0;
                if (live) {
                    size_t idn = 0;
                    while (is_idc(arg[idn]))
                        idn++;
                    if (!idn)
                        cerr(&s, "#ifdef needs a name", NULL);
                    have = find_macro(cpp, arg, idn) != NULL ||
                           has_operator(arg, idn);
                    if (DIR("ifndef"))
                        have = !have;
                }
                cond[ncond++] = !live ? COND_DONE
                                      : have ? COND_LIVE : COND_DEAD;
            } else if (DIR("if")) {
                if (ncond >= MAX_COND_DEPTH)
                    cerr(&s, "conditionals nested too deeply", NULL);
                cond[ncond++] = !live ? COND_DONE
                                      : eval_if(&s, arg) ? COND_LIVE
                                                         : COND_DEAD;
            } else if (DIR("elif")) {
                if (!ncond)
                    cerr(&s, "#elif without #if", NULL);
                if (cond[ncond - 1] == COND_LIVE)
                    cond[ncond - 1] = COND_DONE;
                else if (cond[ncond - 1] == COND_DEAD) {
                    int outer_live = 1;
                    for (int i = 0; i < ncond - 1; i++)
                        if (cond[i] != COND_LIVE)
                            outer_live = 0;
                    if (outer_live && eval_if(&s, arg))
                        cond[ncond - 1] = COND_LIVE;
                }
            } else if (DIR("else")) {
                if (!ncond)
                    cerr(&s, "#else without #if", NULL);
                cond[ncond - 1] = cond[ncond - 1] == COND_DEAD
                                      ? COND_LIVE : COND_DONE;
            } else if (DIR("endif")) {
                if (!ncond)
                    cerr(&s, "#endif without #if", NULL);
                ncond--;
            } else if (!live) {
                /* any other directive in a dead block: ignored */
            } else if (DIR("define")) {
                define_macro(&s, arg);
            } else if (DIR("undef")) {
                size_t idn = 0;
                while (is_idc(arg[idn]))
                    idn++;
                if (!idn)
                    cerr(&s, "#undef needs a name", NULL);
                undef_macro(cpp, arg, idn);
            } else if (DIR("include") || DIR("include_next")) {
                do_include(&s, arg, out, DIR("include_next"));
                s.line = startline + nl;
                snprintf(marker, sizeof marker, "# %d \"%s\"\n",
                         s.line, path);
                tb_puts(out, marker);
                continue;
            } else if (DIR("error")) {
                cerr(&s, "#error: %s", arg);
            } else if (DIR("warning")) {
                fprintf(stderr, "embcc: %s:%d: warning: %s\n",
                        s.file, startline, arg);
            } else if (DIR("pragma")) {
                /* no pragmas mean anything to us yet */
            } else if (dn == 0) {
                /* '#' alone: the null directive */
            } else {
                cerr(&s, "unknown directive '#%s'",
                     xstrndup(lp, dn));
            }
            /* keep the output line-synced */
            for (int i = 0; i < nl; i++)
                tb_putc(out, '\n');
            s.line = startline + nl;
            continue;
        }

        if (live) {
            /* Pull continuation lines while a function-like macro
             * invocation is still open (see needs_more_input). */
            while (needs_more_input(cpp, lineb.p ? lineb.p : "")) {
                int more_nl = 0;
                tb_putc(&lineb, ' ');
                if (!read_logical_line(&s, &lineb, &more_nl)) {
                    cerr(&s, "unterminated macro argument list at end "
                             "of file", NULL);
                    break;
                }
                nl += more_nl;
            }
            s.line = startline;
            expand_text(&s, lineb.p ? lineb.p : "", out);
        }
        for (int i = 0; i < nl; i++)
            tb_putc(out, '\n');
        s.line = startline + nl;
    }
    if (ncond)
        cerr(&s, "unterminated conditional at end of file", NULL);
    free(lineb.p);
}

/* Loads the selected target's predefined table (tools/gen-predef.sh).
 * Function-like entries carry their parameter list in the name:
 * "__INT64_C(c)", and — since gcc 16's aarch64 set includes the SME
 * attribute helpers — variadic ones like "__arm_in(...)". */
static void load_predefined(struct cpp *cpp)
{
    int predef_count;
    const struct predef_macro *predefs = predef_table(&predef_count);
    for (int i = 0; i < predef_count; i++) {
        const char *name = predefs[i].name;
        const char *paren = strchr(name, '(');
        struct macro *m = xcalloc(1, sizeof *m);
        if (paren) {
            m->name = xstrndup(name, (size_t)(paren - name));
            m->is_func = 1;
            const char *p = paren + 1;
            while (*p && *p != ')') {
                if (m->nparams >= MAX_MACRO_PARAMS) {
                    fprintf(stderr, "embcc: internal error: predefined macro "
                                    "'%s' has too many parameters\n", name);
                    exit(1);
                }
                if (p[0] == '.' && p[1] == '.' && p[2] == '.') {
                    m->is_varargs = 1;
                    m->params[m->nparams++] = "__VA_ARGS__";
                    p += 3;
                    continue;
                }
                size_t n = 0;
                while (is_idc(p[n]))
                    n++;
                if (n == 0) {
                    /* Not an identifier and not '...': advancing is the only
                     * thing that matters here — a zero-length token would
                     * otherwise spin, filling params[] until it overran it
                     * (which is exactly what gcc 16's "__arm_in(...)" did). */
                    p++;
                    continue;
                }
                m->params[m->nparams++] = xstrndup(p, n);
                p += n;
                if (*p == ',')
                    p++;
            }
        } else {
            m->name = name;
        }
        m->body = predefs[i].value;
        m->builtin = 1;
        m->next = cpp->macros;
        cpp->macros = m;
    }
}

char *cpp_process(const char *path, const char *src,
                  const char **incdirs, int nincdirs)
{
    struct cpp cpp;
    memset(&cpp, 0, sizeof cpp);
    cpp.incdirs = incdirs;
    cpp.nincdirs = nincdirs;
    load_predefined(&cpp);

    /* the compiler identifies itself — honestly, as itself */
    defining_builtins = 1;
    struct src boot;
    boot.cpp = &cpp;
    boot.file = "<built-in>";
    boot.p = "";
    boot.line = 0;
    boot.incdir_idx = -1;
    define_macro(&boot, "__EMBCC__ 1");
    /* gcc-isms real headers use unconditionally; semantically no-ops */
    define_macro(&boot, "__extension__");
    define_macro(&boot, "__restrict");
    define_macro(&boot, "__restrict__");
    define_macro(&boot, "__inline");
    define_macro(&boot, "__inline__");
    defining_builtins = 0;
    define_macro(&boot, "__STDC__ 1");
    if (!predef_is_cxx())      /* C++ has __cplusplus instead */
        define_macro(&boot, "__STDC_VERSION__ 199901L");
    define_macro(&boot, "__STDC_HOSTED__ 1");
    if (predef_is_cxx()) {
        /* the C++ features EmbCC implements (docs/CXX.md), at the
         * values g++ gives the standard they come from; those it does
         * not yet (concepts, coroutines, designated initializers,
         * CTAD, consteval, aligned new, ...) are left undefined, so
         * libstdc++ takes its paths without them */
        static const char *const feats[] = {
            "__GNUG__ 16",
            "__VERSION__ \"16.2.0 (EmbCC)\"",
            "__GXX_RTTI 1", "__cpp_rtti 199711L",
            "__cpp_aggregate_nsdmi 201304L",
            "__cpp_aggregate_paren_init 201902L",
            "__cpp_alias_templates 200704L", "__cpp_attributes 200809L",
            "__cpp_binary_literals 201304L",
            "__cpp_capture_star_this 201603L", "__cpp_char8_t 202207L",
            "__cpp_concepts 201907L",
            "__cpp_conditional_explicit 201806L",
            "__cpp_constexpr 201603L", "__cpp_decltype 200707L",
            /* constexpr destructors, and std::construct_at (which C++20
             * libstdc++ uses unconditionally); new in a constant
             * evaluation makes it not constant, as anything the
             * interpreter cannot do */
            "__cpp_constexpr_dynamic_alloc 201907L",
            "__cpp_decltype_auto 201304L",
            "__cpp_deduction_guides 201703L",
            "__cpp_delegating_constructors 200604L",
            "__cpp_enumerator_attributes 201411L",
            "__cpp_fold_expressions 201603L",
            "__cpp_generic_lambdas 201304L",
            "__cpp_guaranteed_copy_elision 201606L",
            "__cpp_hex_float 201603L", "__cpp_if_constexpr 201606L",
            "__cpp_impl_three_way_comparison 201907L",
            "__cpp_init_captures 201304L",
            "__cpp_initializer_lists 200806L",
            "__cpp_inline_variables 201606L", "__cpp_lambdas 200907L",
            "__cpp_namespace_attributes 201411L",
            "__cpp_nested_namespace_definitions 201411L",
            "__cpp_nsdmi 200809L", "__cpp_range_based_for 201603L",
            "__cpp_ref_qualifiers 200710L",
            "__cpp_return_type_deduction 201304L",
            "__cpp_rvalue_references 200610L",
            "__cpp_static_assert 201411L",
            "__cpp_structured_bindings 201606L",
            "__cpp_threadsafe_static_init 200806L",
            "__cpp_unicode_characters 200704L",
            "__cpp_unicode_literals 200710L",
            "__cpp_user_defined_literals 200809L",
            "__cpp_using_enum 201907L",
            "__cpp_variable_templates 201304L",
            "__cpp_variadic_templates 200704L",
        };
        for (size_t i = 0; i < sizeof feats / sizeof feats[0]; i++)
            define_macro(&boot, feats[i]);
        /* C++ units present as g++ to the headers: libstdc++ is GCC's
         * own library, built against GCC's view of newlib (va_list,
         * __func__, attributes); C units keep EmbCC's own identity */
        define_macro(&boot, "__GNUC__ 16");
        define_macro(&boot, "__GNUC_MINOR__ 2");
        define_macro(&boot, "__GNUC_PATCHLEVEL__ 0");
        if (cxx_exceptions) {
            define_macro(&boot, "__EXCEPTIONS 1");
            define_macro(&boot, "__cpp_exceptions 199711L");
        }
    }

    struct tbuf out = { 0, 0, 0 };
    process_file(&cpp, path, src, &out, -1);
    return out.p ? out.p : xstrndup("", 0);
}
