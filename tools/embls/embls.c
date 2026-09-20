/* embls — EmbCC's language server (LSP over stdio, docs/TOOLING.md T5).
 *
 * What an editor asks a compiler, answered by the compiler itself rather
 * than by a second parser that drifts from it:
 *
 *   diagnostics   every error and warning as you type, with the fix-its the
 *                 engine attaches — from `embcc -fsyntax-only
 *                 -fdiagnostics-format=json`, so what the editor shows is
 *                 exactly what the build will say
 *   completion    members after `.` or `->` (from the real struct), locals
 *                 in scope, globals, functions, typedefs, enumerators,
 *                 macros' names, keywords
 *   hover         the declaration: type and signature
 *   definition    where the name comes from, headers included
 *   symbols       the file's functions and globals
 *
 * The index behind the last four comes from EmbCC's own preprocessor and
 * parser, run over the buffer — but run in a FORKED CHILD, which is what
 * makes it safe to use a compiler front end this way: a front end ends the
 * process when it meets something it cannot continue past, and a server
 * must not end. The child writes what it learned down a pipe; if it dies
 * early the parent simply has a smaller index, and the next keystroke
 * tries again.
 *
 * Positions are LSP's: 0-based line and character. A character is a UTF-16
 * code unit in the protocol; this counts bytes, which agree for ASCII
 * sources and are off by the multi-byte characters in a line otherwise.
 */
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../../src/driver/util.h"
#include "../../src/parse/ast.h"
#include "../../src/sema/type.h"
#include "../../src/cpp/cpp.h"
#include "../../src/cxx/cxx.h"
#include "../../src/cxx/translate.h"
#include "../../src/arch/target.h"
#include "../../src/arch/predef.h"

struct unit *parse_unit(const char *file, const char *src);

/* ---- a small JSON reader ---------------------------------------------------
 *
 * Enough of JSON to read what an editor sends: objects, arrays, strings
 * (with escapes), numbers, the three literals. Values point into a private
 * copy of the message, so a string is a length and a pointer, unescaped in
 * place. */
enum { JNULL, JBOOL, JNUM, JSTR, JARR, JOBJ };

struct jv {
    int kind;
    double num;
    int bval;
    char *str;                 /* JSTR: NUL-terminated, unescaped */
    struct jv **kids;          /* JARR: elements; JOBJ: values */
    char **keys;               /* JOBJ: the names */
    int n;
};

static const char *js_skip(const char *p)
{
    while (*p && (unsigned char)*p <= ' ')
        p++;
    return p;
}

static struct jv *js_value(const char **pp);

static char *js_string(const char **pp)
{
    const char *p = *pp + 1;                    /* past the quote */
    size_t cap = 32, n = 0;
    char *s = xmalloc(cap);
    while (*p && *p != '"') {
        unsigned c = (unsigned char)*p++;
        if (c == '\\' && *p) {
            char e = *p++;
            switch (e) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u': {
                unsigned v = 0;
                for (int i = 0; i < 4 && *p; i++) {
                    char h = *p++;
                    v = v * 16 + (unsigned)(h <= '9' ? h - '0'
                                            : (h | 32) - 'a' + 10);
                }
                /* UTF-8 of the BMP code point; a surrogate pair arrives as
                 * two escapes and is written as two sequences, which is
                 * wrong only for text outside the BMP in a file name. */
                if (v < 0x80) {
                    c = v;
                } else if (v < 0x800) {
                    if (n + 2 >= cap) { cap *= 2; s = xrealloc(s, cap); }
                    s[n++] = (char)(0xc0 | (v >> 6));
                    c = 0x80 | (v & 0x3f);
                } else {
                    if (n + 3 >= cap) { cap *= 2; s = xrealloc(s, cap); }
                    s[n++] = (char)(0xe0 | (v >> 12));
                    s[n++] = (char)(0x80 | ((v >> 6) & 0x3f));
                    c = 0x80 | (v & 0x3f);
                }
                break;
            }
            default: c = (unsigned char)e;
            }
        }
        if (n + 1 >= cap) { cap *= 2; s = xrealloc(s, cap); }
        s[n++] = (char)c;
    }
    s[n] = 0;
    *pp = *p == '"' ? p + 1 : p;
    return s;
}

static struct jv *js_new(int kind)
{
    struct jv *v = xcalloc(1, sizeof *v);
    v->kind = kind;
    return v;
}

static struct jv *js_value(const char **pp)
{
    const char *p = js_skip(*pp);
    struct jv *v;
    if (*p == '{') {
        v = js_new(JOBJ);
        p = js_skip(p + 1);
        while (*p && *p != '}') {
            char *k = js_string(&p);
            p = js_skip(p);
            if (*p == ':') p++;
            struct jv *val = js_value(&p);
            v->keys = xrealloc(v->keys, (size_t)(v->n + 1) * sizeof *v->keys);
            v->kids = xrealloc(v->kids, (size_t)(v->n + 1) * sizeof *v->kids);
            v->keys[v->n] = k;
            v->kids[v->n] = val;
            v->n++;
            p = js_skip(p);
            if (*p == ',') p = js_skip(p + 1);
        }
        if (*p == '}') p++;
    } else if (*p == '[') {
        v = js_new(JARR);
        p = js_skip(p + 1);
        while (*p && *p != ']') {
            struct jv *e = js_value(&p);
            v->kids = xrealloc(v->kids, (size_t)(v->n + 1) * sizeof *v->kids);
            v->kids[v->n++] = e;
            p = js_skip(p);
            if (*p == ',') p = js_skip(p + 1);
        }
        if (*p == ']') p++;
    } else if (*p == '"') {
        v = js_new(JSTR);
        v->str = js_string(&p);
    } else if (!strncmp(p, "true", 4) || !strncmp(p, "false", 5)) {
        v = js_new(JBOOL);
        v->bval = *p == 't';
        p += v->bval ? 4 : 5;
    } else if (!strncmp(p, "null", 4)) {
        v = js_new(JNULL);
        p += 4;
    } else {
        v = js_new(JNUM);
        char *end;
        v->num = strtod(p, &end);
        p = end;
    }
    *pp = p;
    return v;
}

static struct jv *jget(struct jv *o, const char *key)
{
    if (!o || o->kind != JOBJ)
        return NULL;
    for (int i = 0; i < o->n; i++)
        if (!strcmp(o->keys[i], key))
            return o->kids[i];
    return NULL;
}

static const char *jstr(struct jv *o, const char *key)
{
    struct jv *v = jget(o, key);
    return v && v->kind == JSTR ? v->str : NULL;
}

static int jint(struct jv *o, const char *key)
{
    struct jv *v = jget(o, key);
    return v && v->kind == JNUM ? (int)v->num : 0;
}

/* ---- writing JSON ---- */

struct sb { char *p; size_t n, cap; };

static void sb_add(struct sb *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2;
        b->p = xrealloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

static void sb_str(struct sb *b, const char *s) { sb_add(b, s, strlen(s)); }

static void sb_fmt(struct sb *b, const char *fmt, ...)
{
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof tmp) { sb_add(b, tmp, (size_t)n); return; }
    char *big = xmalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    sb_add(b, big, (size_t)n);
    free(big);
}

static void sb_json_str(struct sb *b, const char *s)
{
    sb_str(b, "\"");
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"':  sb_str(b, "\\\""); break;
        case '\\': sb_str(b, "\\\\"); break;
        case '\n': sb_str(b, "\\n"); break;
        case '\r': sb_str(b, "\\r"); break;
        case '\t': sb_str(b, "\\t"); break;
        default:
            if (c < 0x20) sb_fmt(b, "\\u%04x", c);
            else sb_add(b, (const char *)&c, 1);
        }
    }
    sb_str(b, "\"");
}

/* ---- the protocol's frame ---- */

static void send_raw(const char *body)
{
    printf("Content-Length: %zu\r\n\r\n%s", strlen(body), body);
    fflush(stdout);
}

static void send_result(struct jv *id, const char *result_json)
{
    struct sb b = { 0, 0, 0 };
    sb_str(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
    if (id && id->kind == JSTR) sb_json_str(&b, id->str);
    else if (id && id->kind == JNUM) sb_fmt(&b, "%d", (int)id->num);
    else sb_str(&b, "null");
    sb_str(&b, ",\"result\":");
    sb_str(&b, result_json && *result_json ? result_json : "null");
    sb_str(&b, "}");
    send_raw(b.p);
    free(b.p);
}

static void send_notify(const char *method, const char *params_json)
{
    struct sb b = { 0, 0, 0 };
    sb_str(&b, "{\"jsonrpc\":\"2.0\",\"method\":");
    sb_json_str(&b, method);
    sb_str(&b, ",\"params\":");
    sb_str(&b, params_json);
    sb_str(&b, "}");
    send_raw(b.p);
    free(b.p);
}

/* ---- documents ---- */

struct doc {
    char *uri;
    char *path;                 /* the uri's file part */
    char *text;
};
static struct doc *g_docs;
static int g_ndocs;

/* C++ by its suffix, as the driver decides it. */
static int is_cxx_path(const char *p)
{
    static const char *const sfx[] = { ".cc", ".cpp", ".cxx", ".C", ".c++",
                                       ".hpp", ".hh", ".hxx" };
    size_t n = strlen(p);
    for (size_t i = 0; i < sizeof sfx / sizeof sfx[0]; i++) {
        size_t k = strlen(sfx[i]);
        if (n > k && !strcmp(p + n - k, sfx[i]))
            return 1;
    }
    return 0;
}

static char *uri_to_path(const char *uri)
{
    if (strncmp(uri, "file://", 7) != 0)
        return xstrndup(uri, strlen(uri));
    const char *p = uri + 7;
    size_t n = strlen(p);
    char *out = xmalloc(n + 1);
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == '%' && i + 2 < n) {
            int hi = p[i + 1], lo = p[i + 2];
            out[k++] = (char)((isdigit(hi) ? hi - '0' : (hi | 32) - 'a' + 10) * 16 +
                              (isdigit(lo) ? lo - '0' : (lo | 32) - 'a' + 10));
            i += 2;
        } else {
            out[k++] = p[i];
        }
    }
    out[k] = 0;
    return out;
}

static struct doc *doc_find(const char *uri)
{
    for (int i = 0; i < g_ndocs; i++)
        if (!strcmp(g_docs[i].uri, uri))
            return &g_docs[i];
    return NULL;
}

static struct doc *doc_put(const char *uri, const char *text)
{
    struct doc *d = doc_find(uri);
    if (!d) {
        g_docs = xrealloc(g_docs, (size_t)(g_ndocs + 1) * sizeof *g_docs);
        d = &g_docs[g_ndocs++];
        memset(d, 0, sizeof *d);
        d->uri = xstrndup(uri, strlen(uri));
        d->path = uri_to_path(uri);
    }
    free(d->text);
    d->text = xstrndup(text, strlen(text));
    return d;
}

/* ---- the flags a project compiles with ------------------------------------
 *
 * compile_flags.txt in the file's directory or above it, one flag a line —
 * the format clangd reads, so a project that works with clangd works here.
 * EMBLS_FLAGS adds to it, for a quick try without a file. */
static char *g_flags[64];
static int g_nflags;

static void flags_add(const char *f)
{
    if (g_nflags < (int)(sizeof g_flags / sizeof g_flags[0]) && *f)
        g_flags[g_nflags++] = xstrndup(f, strlen(f));
}

/* Reloaded on every edit, so it must START empty: appending left the list
 * holding one more copy of every flag per keystroke, until it hit the cap
 * and silently dropped whatever came next. */
static void flags_clear(void)
{
    for (int i = 0; i < g_nflags; i++)
        free(g_flags[i]);
    g_nflags = 0;
}

static void flags_load(const char *path)
{
    flags_clear();
    char dir[4096];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0; else strcpy(dir, ".");
    for (int up = 0; up < 8; up++) {
        char f[4200];
        snprintf(f, sizeof f, "%s/compile_flags.txt", dir);
        FILE *fp = fopen(f, "r");
        if (fp) {
            char line[1024];
            while (fgets(line, sizeof line, fp)) {
                size_t n = strlen(line);
                while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
                    line[--n] = 0;
                if (n) flags_add(line);
            }
            fclose(fp);
            break;
        }
        slash = strrchr(dir, '/');
        if (!slash) break;
        *slash = 0;
    }
    const char *env = getenv("EMBLS_FLAGS");
    if (env) {
        char *copy = xstrndup(env, strlen(env));
        for (char *t = strtok(copy, " "); t; t = strtok(NULL, " "))
            flags_add(t);
    }
}

/* ---- the index ------------------------------------------------------------
 *
 * One record per thing a question can be about. Built by the front end in a
 * forked child (see the file's head) and read back as tab-separated lines. */
enum { SYM_FUNC, SYM_VAR, SYM_LOCAL, SYM_PARAM, SYM_TYPE, SYM_ENUM,
       SYM_MEMBER, SYM_MACRO,
       /* Not a declaration: one PLACE a name is used. The parse knows an
        * identifier in expression position from one inside a comment, a
        * string or a member name, which is the whole difference between
        * "find references" and grep. detail says which: "v" a name in
        * expression position, "c" the callee of a call, "m" a member after
        * . or ->; owner is the function it sits in. */
       SYM_REF };

struct sym {
    int kind;
    char *name;
    char *detail;              /* the type, or the signature */
    char *owner;               /* SYM_MEMBER: its struct tag; SYM_LOCAL: its
                                * function */
    char *file;
    int line, col;
    int scope_start, scope_end; /* SYM_LOCAL/SYM_PARAM: the function's lines */
};

static struct sym *g_syms;
static int g_nsyms, g_capsyms;

static void sym_add(int kind, const char *name, const char *detail,
                    const char *owner, const char *file, int line, int col,
                    int s0, int s1)
{
    if (!name || !*name)
        return;
    if (g_nsyms == g_capsyms) {
        g_capsyms = g_capsyms ? g_capsyms * 2 : 256;
        g_syms = xrealloc(g_syms, (size_t)g_capsyms * sizeof *g_syms);
    }
    struct sym *s = &g_syms[g_nsyms++];
    memset(s, 0, sizeof *s);
    s->kind = kind;
    s->name = xstrndup(name, strlen(name));
    s->detail = xstrndup(detail ? detail : "", strlen(detail ? detail : ""));
    s->owner = xstrndup(owner ? owner : "", strlen(owner ? owner : ""));
    s->file = xstrndup(file ? file : "", strlen(file ? file : ""));
    s->line = line;
    s->col = col;
    s->scope_start = s0;
    s->scope_end = s1;
}

static void index_clear(void)
{
    for (int i = 0; i < g_nsyms; i++) {
        free(g_syms[i].name); free(g_syms[i].detail);
        free(g_syms[i].owner); free(g_syms[i].file);
    }
    g_nsyms = 0;
}

/* The child's side: walk what the parser built and write it down the pipe.
 * Every field is tab-separated and newline-terminated; a field never holds
 * a tab (type names and identifiers cannot). */
static void emit_rec(FILE *f, int kind, const char *name, const char *detail,
                     const char *owner, const char *file, int line, int col,
                     int s0, int s1)
{
    fprintf(f, "%d\t%s\t%s\t%s\t%s\t%d\t%d\t%d\t%d\n", kind,
            name ? name : "", detail ? detail : "", owner ? owner : "",
            file ? file : "", line, col, s0, s1);
}

/* the last line a function's body reaches, so a local is offered only
 * inside the function that declares it */
static int stmt_last_line(struct stmt *s, int best)
{
    for (; s; s = s->next) {
        if (s->line > best) best = s->line;
        best = stmt_last_line(s->body, best);
        best = stmt_last_line(s->thn, best);
        best = stmt_last_line(s->els, best);
        best = stmt_last_line(s->initdecl, best);
    }
    return best;
}

static void emit_locals(FILE *f, struct stmt *s, const char *fn,
                        const char *file, int s0, int s1)
{
    for (; s; s = s->next) {
        if (s->kind == STMT_DECL && s->name)
            emit_rec(f, SYM_LOCAL, s->name, ty_name(s->dty), fn, file,
                     s->line, s->col, s0, s1);
        emit_locals(f, s->body, fn, file, s0, s1);
        emit_locals(f, s->thn, fn, file, s0, s1);
        emit_locals(f, s->els, fn, file, s0, s1);
        emit_locals(f, s->initdecl, fn, file, s0, s1);
    }
}

/* Every place a name is USED, from the tree rather than from the text.
 * An identifier inside a comment or a string literal is not a node, so it
 * cannot be found here; a member name after a `.` is a different node from
 * a variable of the same spelling, so the two do not collide. */
static void emit_refs_expr(FILE *f, struct expr *e, const char *fn,
                           const char *file)
{
    if (!e)
        return;
    switch (e->kind) {
    case EXPR_VAR:
        emit_rec(f, SYM_REF, e->name, "v", fn, file, e->line, e->col, 0, 0);
        break;
    case EXPR_INCDEC:
        if (e->name)
            emit_rec(f, SYM_REF, e->name, "v", fn, file, e->line, e->col, 0, 0);
        break;
    case EXPR_MEMBER:
        /* the member's own name, not the base's: `p.x` and a local `x` are
         * different names that happen to be spelt the same */
        if (e->name)
            emit_rec(f, SYM_REF, e->name, "m", fn, file, e->line, e->col, 0, 0);
        break;
    case EXPR_CALL:
        if (e->lhs && e->lhs->kind == EXPR_VAR && e->lhs->name) {
            emit_rec(f, SYM_REF, e->lhs->name, "c", fn, file,
                     e->lhs->line, e->lhs->col, 0, 0);
            for (int i = 0; i < e->nargs; i++)
                emit_refs_expr(f, e->args[i], fn, file);
            return;                      /* the callee is already recorded */
        }
        break;
    default:
        break;
    }
    emit_refs_expr(f, e->lhs, fn, file);
    emit_refs_expr(f, e->rhs, fn, file);
    for (int i = 0; i < e->nargs; i++)
        emit_refs_expr(f, e->args[i], fn, file);
    for (int i = 0; i < e->nelems; i++)
        emit_refs_expr(f, e->elems[i], fn, file);
    for (int i = 0; i < e->ngen; i++)
        emit_refs_expr(f, e->gexprs[i], fn, file);
}

static void emit_refs(FILE *f, struct stmt *s, const char *fn,
                      const char *file)
{
    for (; s; s = s->next) {
        emit_refs_expr(f, s->expr, fn, file);
        emit_refs_expr(f, s->cond, fn, file);
        emit_refs_expr(f, s->init, fn, file);
        emit_refs_expr(f, s->step, fn, file);
        emit_refs(f, s->initdecl, fn, file);
        emit_refs(f, s->thn, fn, file);
        emit_refs(f, s->els, fn, file);
        emit_refs(f, s->body, fn, file);
    }
}

static void emit_index(FILE *f, struct unit *u)
{
    for (struct func *fn = u->funcs; fn; fn = fn->next) {
        if (fn->absorbed)
            continue;
        struct sb sig = { 0, 0, 0 };
        sb_fmt(&sig, "%s %s(", ty_name(fn->ret_ty), fn->name);
        for (int i = 0; i < fn->nparams; i++)
            sb_fmt(&sig, "%s%s%s%s", i ? ", " : "", ty_name(fn->param_tys[i]),
                   fn->params[i] ? " " : "", fn->params[i] ? fn->params[i] : "");
        sb_fmt(&sig, "%s)", fn->is_varargs ? ", ..." : "");
        emit_rec(f, SYM_FUNC, fn->name, sig.p, "", fn->file, fn->line, 1, 0, 0);
        free(sig.p);
        if (!fn->body)
            continue;
        int end = stmt_last_line(fn->body, fn->line);
        for (int i = 0; i < fn->nparams; i++)
            if (fn->params[i])
                emit_rec(f, SYM_PARAM, fn->params[i], ty_name(fn->param_tys[i]),
                         fn->name, fn->file,
                         fn->param_lines[i] ? fn->param_lines[i] : fn->line,
                         fn->param_cols[i] ? fn->param_cols[i] : 1,
                         fn->line, end);
        emit_locals(f, fn->body, fn->name, fn->file, fn->line, end);
        emit_refs(f, fn->body, fn->name, fn->file);
    }
    for (struct global *g = u->globals; g; g = g->next)
        if (!g->absorbed)
            emit_rec(f, SYM_VAR, g->name, ty_name(g->ty), "", g->file,
                     g->line, 1, 0, 0);
    for (struct econst *e = u->econsts; e; e = e->next) {
        char v[32];
        snprintf(v, sizeof v, "= %ld", e->val);
        emit_rec(f, SYM_ENUM, e->name, v, "", u->file, 0, 0, 0, 0);
    }
    for (struct typedefent *t = u->typedefs; t; t = t->next)
        emit_rec(f, SYM_TYPE, t->name, ty_name(t->ty), "", u->file, 0, 0, 0, 0);
    for (struct tagdef *t = u->tags; t; t = t->next) {
        const char *kind = t->kind == TAG_UNION ? "union"
                         : t->kind == TAG_ENUM ? "enum" : "struct";
        char detail[256];
        snprintf(detail, sizeof detail, "%s %s", kind, t->tag ? t->tag : "");
        emit_rec(f, SYM_TYPE, t->tag, detail, "", u->file, 0, 0, 0, 0);
        if (!t->ty || t->kind == TAG_ENUM)
            continue;
        for (int i = 0; i < t->ty->nmembers; i++) {
            struct member *m = &t->ty->members[i];
            if (m->name)
                emit_rec(f, SYM_MEMBER, m->name, ty_name(m->ty), t->tag,
                         u->file, 0, 0, 0, 0);
        }
    }
}

/* The same for C++, from the C++ front end's own tables: its functions
 * (with their signatures as written, not mangled), its classes and their
 * members, its namespace-scope variables. A member is recorded under its
 * class's name, which is what completion after a `.` looks up. */
static void emit_cxx_locals(FILE *f, struct cstmt *s, const char *fn,
                            const char *file, int s0, int s1)
{
    for (; s; s = s->next) {
        if (s->k == S_DECL)
            for (struct cstmt *d = s; d; d = d->more)
                if (d->var && d->var->name)
                    emit_rec(f, SYM_LOCAL, d->var->name, ct_name(d->var->type),
                             fn, file, d->var->line, 1, s0, s1);
        emit_cxx_locals(f, s->body, fn, file, s0, s1);
        emit_cxx_locals(f, s->els, fn, file, s0, s1);
    }
}

static int cxx_last_line(struct cstmt *s, int best)
{
    for (; s; s = s->next) {
        if (s->line > best) best = s->line;
        best = cxx_last_line(s->body, best);
        best = cxx_last_line(s->els, best);
    }
    return best;
}

static void emit_cxx_index(FILE *f)
{
    for (struct cfunc *fn = cx_funcs; fn; fn = fn->all_next) {
        if (!fn->name || fn->name[0] == '<')
            continue;
        struct sb sig = { 0, 0, 0 };
        struct cty *t = fn->type;
        sb_fmt(&sig, "%s %s%s%s(", t && t->to ? ct_name(t->to) : "auto",
               fn->cls && fn->cls->name ? fn->cls->name : "",
               fn->cls && fn->cls->name ? "::" : "", fn->name);
        for (int i = 0; t && i < t->np; i++)
            sb_fmt(&sig, "%s%s%s%s", i ? ", " : "", ct_name(t->params[i]),
                   fn->pnames && fn->pnames[i] ? " " : "",
                   fn->pnames && fn->pnames[i] ? fn->pnames[i] : "");
        sb_fmt(&sig, "%s)", t && t->variadic ? ", ..." : "");
        emit_rec(f, SYM_FUNC, fn->name, sig.p, fn->cls ? fn->cls->name : "",
                 fn->file, fn->line, 1, 0, 0);
        free(sig.p);
        if (!fn->body)
            continue;
        int end = cxx_last_line(fn->body, fn->line);
        for (int i = 0; t && fn->pnames && i < t->np; i++)
            if (fn->pnames[i])
                emit_rec(f, SYM_PARAM, fn->pnames[i], ct_name(t->params[i]),
                         fn->name, fn->file, fn->line, 1, fn->line, end);
        emit_cxx_locals(f, fn->body, fn->name, fn->file, fn->line, end);
    }
    for (int i = 0; i < cx_nclasses; i++) {
        struct cclass *c = cx_classes[i];
        if (!c || !c->name)
            continue;
        char detail[256];
        snprintf(detail, sizeof detail, "%s %s",
                 c->is_union ? "union" : c->is_struct ? "struct" : "class",
                 c->name);
        emit_rec(f, SYM_TYPE, c->name, detail, "", "", 0, 0, 0, 0);
        for (int k = 0; k < c->nfields; k++)
            if (c->fields[k] && c->fields[k]->name)
                emit_rec(f, SYM_MEMBER, c->fields[k]->name,
                         ct_name(c->fields[k]->type), c->name, "", 0, 0, 0, 0);
        /* member functions are members too: completion after a `.` should
         * offer what you can call, not only what you can read */
        for (struct cfunc *m = cx_funcs; m; m = m->all_next)
            if (m->cls == c && m->name && m->name[0] != '<' && !m->is_ctor &&
                !m->is_dtor && !m->is_implicit && !m->is_deleted)
                emit_rec(f, SYM_MEMBER, m->name,
                         m->type && m->type->to ? ct_name(m->type->to) : "auto",
                         c->name, m->file, m->line, 1, 0, 0);
    }
    for (int i = 0; i < cx_ngvars; i++)
        if (cx_gvars[i] && cx_gvars[i]->name && !cx_gvars[i]->is_local)
            emit_rec(f, SYM_VAR, cx_gvars[i]->name, ct_name(cx_gvars[i]->type),
                     "", cx_gvars[i]->file, cx_gvars[i]->line, 1, 0, 0);
}

/* The parent's side: fork, let the child parse, read what it wrote. The
 * child's stderr goes nowhere — its diagnostics are the compiler's job, and
 * this run is only for the index. */
static void index_build(struct doc *d)
{
    index_clear();
    /* the buffer as a file, so the preprocessor can read it (and so the
     * diagnostics run below sees exactly the same bytes) */
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s.embls-tmp.c", d->path);
    FILE *tf = fopen(tmp, "w");
    if (!tf)
        return;
    fputs(d->text, tf);
    fclose(tf);

    int fds[2];
    if (pipe(fds) != 0) { unlink(tmp); return; }
    pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        FILE *out = fdopen(fds[1], "w");
        freopen("/dev/null", "w", stderr);
        const char *incs[64];
        int ninc = 0;
        char selfdir[4096] = ".";
        char *slash = strrchr(d->path, '/');
        if (slash) snprintf(selfdir, sizeof selfdir, "%.*s",
                            (int)(slash - d->path), d->path);
        incs[ninc++] = selfdir;
        for (int i = 0; i < g_nflags && ninc < 60; i++) {
            if (!strncmp(g_flags[i], "-I", 2) && g_flags[i][2])
                incs[ninc++] = g_flags[i] + 2;
            else if (!strncmp(g_flags[i], "-isystem", 8) && g_flags[i][8])
                incs[ninc++] = g_flags[i] + 8;
            else if ((!strcmp(g_flags[i], "-I") || !strcmp(g_flags[i], "-isystem"))
                     && i + 1 < g_nflags)
                incs[ninc++] = g_flags[++i];
        }
        char *src = NULL;
        long len = 0;
        FILE *rf = fopen(tmp, "rb");
        if (rf) {
            fseek(rf, 0, SEEK_END);
            len = ftell(rf);
            fseek(rf, 0, SEEK_SET);
            src = xmalloc((size_t)len + 1);
            if (fread(src, 1, (size_t)len, rf) != (size_t)len)
                len = 0;
            src[len] = 0;
            fclose(rf);
        }
        if (src) {
            cpp_set_tolerant(1);     /* a header we cannot find is not fatal */
            int cxx = is_cxx_path(d->path);
            predef_set_cxx(cxx);
            if (cxx)
                cpp_set_cxx(NULL, 1);
            char *pp = cpp_process(tmp, src, incs, ninc);
            if (cxx) {
                /* The C++ front end's own tables, so what an editor is
                 * told matches what the compiler sees. */
                scope_init();
                cx_tokenize(d->path, pp);
                cx_parse_unit();
                emit_cxx_index(out);
            } else {
                struct unit *u = parse_unit(tmp, pp);
                emit_index(out, u);
            }
        }
        fflush(out);
        _exit(0);
    }
    close(fds[1]);
    if (pid < 0) { close(fds[0]); unlink(tmp); return; }
    FILE *in = fdopen(fds[0], "r");
    char line[8192];
    while (in && fgets(line, sizeof line, in)) {
        char *f[9];
        int nf = 0;
        char *p = line;
        while (nf < 9) {
            f[nf++] = p;
            char *t = strchr(p, nf == 9 ? '\n' : '\t');
            if (!t) break;
            *t = 0;
            p = t + 1;
        }
        if (nf < 9) continue;
        char *nl = strchr(f[8], '\n');
        if (nl) *nl = 0;
        /* The temporary file stands in for the document: report the
         * document's own path, so an editor can open what it names. */
        const char *file = !strcmp(f[4], tmp) ? d->path : f[4];
        sym_add(atoi(f[0]), f[1], f[2], f[3], file, atoi(f[5]), atoi(f[6]),
                atoi(f[7]), atoi(f[8]));
    }
    if (in) fclose(in);
    int st;
    waitpid(pid, &st, 0);
    unlink(tmp);
}

/* ---- diagnostics ---------------------------------------------------------
 *
 * From the compiler itself: what the editor shows is what the build says. */
static void publish_diagnostics(struct doc *d)
{
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s.embls-diag%s", d->path,
             strstr(d->path, ".c") && !strstr(d->path, ".cc") ? ".c" : ".cc");
    FILE *tf = fopen(tmp, "w");
    if (!tf)
        return;
    fputs(d->text, tf);
    fclose(tf);

    struct sb cmd = { 0, 0, 0 };
    const char *cc = getenv("EMBLS_EMBCC");
    sb_fmt(&cmd, "%s -fsyntax-only -fdiagnostics-format=json",
           cc && *cc ? cc : "embcc");
    char selfdir[4096] = ".";
    char *slash = strrchr(d->path, '/');
    if (slash) snprintf(selfdir, sizeof selfdir, "%.*s",
                        (int)(slash - d->path), d->path);
    sb_fmt(&cmd, " -I '%s'", selfdir);
    for (int i = 0; i < g_nflags; i++)
        sb_fmt(&cmd, " '%s'", g_flags[i]);
    sb_fmt(&cmd, " -c '%s' 2>&1", tmp);

    struct sb out = { 0, 0, 0 };
    FILE *p = popen(cmd.p, "r");
    if (p) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, p)) > 0)
            sb_add(&out, buf, n);
        pclose(p);
    }
    free(cmd.p);
    unlink(tmp);

    struct sb b = { 0, 0, 0 };
    sb_str(&b, "{\"uri\":");
    sb_json_str(&b, d->uri);
    sb_str(&b, ",\"diagnostics\":[");
    const char *jp = out.p ? strchr(out.p, '[') : NULL;
    int any = 0;
    if (jp) {
        const char *q = jp;
        struct jv *arr = js_value(&q);
        for (int i = 0; arr && arr->kind == JARR && i < arr->n; i++) {
            struct jv *dg = arr->kids[i];
            const char *kind = jstr(dg, "kind");
            const char *msg = jstr(dg, "message");
            struct jv *locs = jget(dg, "locations");
            if (!msg || !locs || locs->kind != JARR || !locs->n)
                continue;
            struct jv *caret = jget(locs->kids[0], "caret");
            struct jv *fin = jget(locs->kids[0], "finish");
            int line = jint(caret, "line") - 1;
            int col = jint(caret, "column") - 1;
            int ecol = fin ? jint(fin, "column") : col + 1;
            if (line < 0) line = 0;
            if (col < 0) col = 0;
            sb_fmt(&b, "%s{\"range\":{\"start\":{\"line\":%d,\"character\":%d},"
                       "\"end\":{\"line\":%d,\"character\":%d}},"
                       "\"severity\":%d,\"source\":\"embcc\",\"message\":",
                   any ? "," : "", line, col, line, ecol,
                   kind && !strcmp(kind, "warning") ? 2 : 1);
            /* the notes under it belong to the message an editor shows */
            struct sb full = { 0, 0, 0 };
            sb_str(&full, msg);
            struct jv *kids = jget(dg, "children");
            for (int k = 0; kids && kids->kind == JARR && k < kids->n; k++) {
                const char *nm = jstr(kids->kids[k], "message");
                if (nm) sb_fmt(&full, "\n  note: %s", nm);
            }
            sb_json_str(&b, full.p ? full.p : msg);
            free(full.p);
            sb_str(&b, "}");
            any = 1;
        }
    }
    sb_str(&b, "]}");
    send_notify("textDocument/publishDiagnostics", b.p);
    free(b.p);
    free(out.p);
}

/* ---- the questions -------------------------------------------------------- */

/* The line `n` (0-based) of a document, and its length. */
static const char *doc_line(struct doc *d, int n, int *len)
{
    const char *p = d->text;
    for (int i = 0; i < n && p; i++) {
        p = strchr(p, '\n');
        if (p) p++;
    }
    if (!p) { *len = 0; return NULL; }
    const char *e = strchr(p, '\n');
    *len = e ? (int)(e - p) : (int)strlen(p);
    return p;
}

static int ident_char(int c) { return isalnum(c) || c == '_'; }

/* The identifier the cursor is in or just after, into `buf`. */
static void word_at(struct doc *d, int line, int ch, char *buf, size_t cap)
{
    int len;
    const char *ln = doc_line(d, line, &len);
    buf[0] = 0;
    if (!ln) return;
    if (ch > len) ch = len;
    int s = ch, e = ch;
    while (s > 0 && ident_char((unsigned char)ln[s - 1])) s--;
    while (e < len && ident_char((unsigned char)ln[e])) e++;
    if (e <= s) return;
    size_t n = (size_t)(e - s) < cap - 1 ? (size_t)(e - s) : cap - 1;
    memcpy(buf, ln + s, n);
    buf[n] = 0;
}

/* Is the cursor right after a `.` or `->` (with an optional partial name)?
 * If so, `base` gets the identifier before it. */
static int member_context(struct doc *d, int line, int ch, char *base,
                          size_t cap)
{
    int len;
    const char *ln = doc_line(d, line, &len);
    base[0] = 0;
    if (!ln) return 0;
    if (ch > len) ch = len;
    int i = ch;
    while (i > 0 && ident_char((unsigned char)ln[i - 1])) i--;   /* partial */
    int dot = -1;
    if (i >= 1 && ln[i - 1] == '.') dot = i - 1;
    else if (i >= 2 && ln[i - 2] == '-' && ln[i - 1] == '>') dot = i - 2;
    if (dot < 0) return 0;
    int e = dot;
    while (e > 0 && isspace((unsigned char)ln[e - 1])) e--;
    int s = e;
    while (s > 0 && ident_char((unsigned char)ln[s - 1])) s--;
    if (e <= s) return 0;
    size_t n = (size_t)(e - s) < cap - 1 ? (size_t)(e - s) : cap - 1;
    memcpy(base, ln + s, n);
    base[n] = 0;
    return 1;
}

/* The class a type names: "struct P *" -> "P" in C, "const Point &" ->
 * "Point" in C++, where a class name stands on its own. The first word
 * that is not a qualifier or an aggregate keyword is it. */
static void tag_of_type(const char *detail, char *tag, size_t cap)
{
    static const char *const skip[] = { "const", "volatile", "struct",
                                        "union", "class", "enum" };
    tag[0] = 0;
    if (!detail)
        return;
    const char *p = detail;
    for (;;) {
        while (*p && !ident_char((unsigned char)*p))
            p++;
        if (!*p)
            return;
        size_t n = 0;
        while (p[n] && ident_char((unsigned char)p[n]))
            n++;
        int skipit = 0;
        for (size_t i = 0; i < sizeof skip / sizeof skip[0]; i++)
            if (n == strlen(skip[i]) && !memcmp(p, skip[i], n))
                skipit = 1;
        if (!skipit) {
            if (n >= cap)
                n = cap - 1;
            memcpy(tag, p, n);
            tag[n] = 0;
            return;
        }
        p += n;
    }
}

/* A name's type, looking at locals of the enclosing function first. */
static const char *type_of_name(const char *name, int line)
{
    const struct sym *best = NULL;
    for (int i = 0; i < g_nsyms; i++) {
        struct sym *s = &g_syms[i];
        if (strcmp(s->name, name) != 0)
            continue;
        if ((s->kind == SYM_LOCAL || s->kind == SYM_PARAM) &&
            line + 1 >= s->scope_start && line + 1 <= s->scope_end)
            return s->detail;                       /* the nearest one wins */
        if (s->kind == SYM_VAR && !best)
            best = s;
    }
    return best ? best->detail : NULL;
}

static const char *kind_name(int k)
{
    switch (k) {
    case SYM_FUNC: return "function";
    case SYM_VAR: return "variable";
    case SYM_LOCAL: return "local";
    case SYM_PARAM: return "parameter";
    case SYM_TYPE: return "type";
    case SYM_ENUM: return "enumerator";
    case SYM_MEMBER: return "member";
    default: return "macro";
    }
}

/* LSP CompletionItemKind */
static int lsp_kind(int k)
{
    switch (k) {
    case SYM_FUNC: return 3;      /* Function */
    case SYM_VAR: return 6;       /* Variable */
    case SYM_LOCAL: return 6;
    case SYM_PARAM: return 6;
    case SYM_TYPE: return 7;      /* Class/Struct */
    case SYM_ENUM: return 20;     /* EnumMember */
    case SYM_MEMBER: return 5;    /* Field */
    default: return 14;           /* Keyword */
    }
}

static const char *const c_keywords[] = {
    "auto", "break", "case", "char", "const", "continue", "default", "do",
    "double", "else", "enum", "extern", "float", "for", "goto", "if",
    "inline", "int", "long", "register", "restrict", "return", "short",
    "signed", "sizeof", "static", "struct", "switch", "typedef", "union",
    "unsigned", "void", "volatile", "while", "_Alignas", "_Alignof",
    "_Atomic", "_Bool", "_Generic", "_Noreturn", "_Static_assert",
};

/* On an `#include` line: the headers the compiler would actually search
 * for, from the same -I list the index was built with, plus the document's
 * own directory. A name offered here is a name that will resolve. */
static int include_context(struct doc *d, int line, int ch, char *pre,
                           size_t cap)
{
    int len = 0;
    const char *lp = doc_line(d, line, &len);
    if (!lp) return 0;
    if (ch > len) ch = len;
    int i = 0;
    while (i < ch && (lp[i] == ' ' || lp[i] == '\t')) i++;
    if (i >= ch || lp[i] != '#') return 0;
    i++;
    while (i < ch && (lp[i] == ' ' || lp[i] == '\t')) i++;
    if (ch - i < 7 || strncmp(lp + i, "include", 7)) return 0;
    i += 7;
    while (i < ch && (lp[i] == ' ' || lp[i] == '\t')) i++;
    if (i >= ch || (lp[i] != '<' && lp[i] != '"')) return 0;
    i++;
    size_t n = (size_t)(ch - i);
    if (n >= cap) n = cap - 1;
    memcpy(pre, lp + i, n);
    pre[n] = 0;
    return 1;
}

static void complete_includes(struct jv *id, struct doc *d, const char *pre)
{
    /* the same directories the child searches, in the same order */
    const char *dirs[64];
    int nd = 0;
    char selfdir[4096] = ".";
    char *slash = strrchr(d->path, '/');
    if (slash)
        snprintf(selfdir, sizeof selfdir, "%.*s",
                 (int)(slash - d->path), d->path);
    dirs[nd++] = selfdir;
    for (int i = 0; i < g_nflags && nd < 60; i++) {
        if (!strncmp(g_flags[i], "-I", 2) && g_flags[i][2])
            dirs[nd++] = g_flags[i] + 2;
        else if (!strncmp(g_flags[i], "-isystem", 8) && g_flags[i][8])
            dirs[nd++] = g_flags[i] + 8;
        else if ((!strcmp(g_flags[i], "-I") || !strcmp(g_flags[i], "-isystem"))
                 && i + 1 < g_nflags)
            dirs[nd++] = g_flags[++i];
    }
    /* a partial path after the quote: `sys/` searches inside it */
    char sub[1024] = "";
    const char *tail = pre;
    const char *last = strrchr(pre, '/');
    if (last) {
        size_t n = (size_t)(last - pre) + 1;
        if (n >= sizeof sub) n = sizeof sub - 1;
        memcpy(sub, pre, n);
        sub[n] = 0;
        tail = last + 1;
    }
    struct sb b = { 0, 0, 0 };
    sb_str(&b, "{\"isIncomplete\":false,\"items\":[");
    int any = 0, shown = 0;
    for (int i = 0; i < nd && shown < 400; i++) {
        char path[4096];
        snprintf(path, sizeof path, "%s/%s", dirs[i], sub);
        DIR *dp = opendir(path);
        if (!dp) continue;
        struct dirent *de;
        while ((de = readdir(dp)) && shown < 400) {
            if (de->d_name[0] == '.') continue;
            size_t nl = strlen(de->d_name);
            int isdir = 0;
            char full[8192];
            snprintf(full, sizeof full, "%s/%s", path, de->d_name);
            struct stat st;
            if (!stat(full, &st) && S_ISDIR(st.st_mode)) isdir = 1;
            if (!isdir && !(nl > 2 && !strcmp(de->d_name + nl - 2, ".h")) &&
                !(nl > 4 && !strcmp(de->d_name + nl - 4, ".hpp")) &&
                strchr(de->d_name, '.'))
                continue;                /* not a header */
            if (*tail && strncmp(de->d_name, tail, strlen(tail)))
                continue;
            sb_fmt(&b, "%s{\"label\":", any ? "," : "");
            sb_json_str(&b, de->d_name);
            /* 17 File, 19 Folder */
            sb_fmt(&b, ",\"kind\":%d,\"detail\":", isdir ? 19 : 17);
            sb_json_str(&b, dirs[i]);
            sb_str(&b, "}");
            any = 1;
            shown++;
        }
        closedir(dp);
    }
    sb_str(&b, "]}");
    send_result(id, b.p);
    free(b.p);
}

static void completion(struct jv *id, struct doc *d, int line, int ch)
{
    char base[256], tag[256];
    char pre[1024];
    if (include_context(d, line, ch, pre, sizeof pre)) {
        complete_includes(id, d, pre);
        return;
    }
    struct sb b = { 0, 0, 0 };
    sb_str(&b, "{\"isIncomplete\":false,\"items\":[");
    int any = 0;
    if (member_context(d, line, ch, base, sizeof base)) {
        /* after `.` or `->`: the members of that variable's struct */
        const char *ty = type_of_name(base, line);
        tag_of_type(ty, tag, sizeof tag);
        for (int i = 0; i < g_nsyms && *tag; i++) {
            struct sym *s = &g_syms[i];
            if (s->kind != SYM_MEMBER || strcmp(s->owner, tag) != 0)
                continue;
            sb_fmt(&b, "%s{\"label\":", any ? "," : "");
            sb_json_str(&b, s->name);
            sb_fmt(&b, ",\"kind\":%d,\"detail\":", lsp_kind(s->kind));
            sb_json_str(&b, s->detail);
            sb_str(&b, "}");
            any = 1;
        }
    } else {
        for (int i = 0; i < g_nsyms; i++) {
            struct sym *s = &g_syms[i];
            if (s->kind == SYM_MEMBER)
                continue;                     /* only after a `.` */
            if (s->kind == SYM_REF)
                continue;                     /* a use is not a candidate */
            if ((s->kind == SYM_LOCAL || s->kind == SYM_PARAM) &&
                !(line + 1 >= s->scope_start && line + 1 <= s->scope_end))
                continue;                     /* not in scope here */
            sb_fmt(&b, "%s{\"label\":", any ? "," : "");
            sb_json_str(&b, s->name);
            sb_fmt(&b, ",\"kind\":%d,\"detail\":", lsp_kind(s->kind));
            sb_json_str(&b, s->detail);
            sb_str(&b, "}");
            any = 1;
        }
        for (size_t i = 0; i < sizeof c_keywords / sizeof c_keywords[0]; i++) {
            sb_fmt(&b, "%s{\"label\":", any ? "," : "");
            sb_json_str(&b, c_keywords[i]);
            sb_str(&b, ",\"kind\":14}");
            any = 1;
        }
    }
    sb_str(&b, "]}");
    send_result(id, b.p);
    free(b.p);
}

static struct sym *lookup_at(struct doc *d, int line, int ch, char *word,
                             size_t cap)
{
    word_at(d, line, ch, word, cap);
    if (!*word)
        return NULL;
    struct sym *best = NULL;
    for (int i = 0; i < g_nsyms; i++) {
        struct sym *s = &g_syms[i];
        if (s->kind == SYM_REF)
            continue;                    /* a use, not what declares it */
        if (strcmp(s->name, word) != 0)
            continue;
        if (s->kind == SYM_LOCAL || s->kind == SYM_PARAM) {
            /* Only where it is actually in scope. Another function's local
             * of the same spelling is a different name entirely, and
             * falling back to it made `total` at file scope resolve to a
             * parameter three functions away. */
            if (line + 1 >= s->scope_start && line + 1 <= s->scope_end)
                return s;
            continue;
        }
        if (!best) best = s;
    }
    return best;
}

static void hover(struct jv *id, struct doc *d, int line, int ch)
{
    char word[256];
    struct sym *s = lookup_at(d, line, ch, word, sizeof word);
    if (!s) { send_result(id, "null"); return; }
    struct sb b = { 0, 0, 0 };
    struct sb md = { 0, 0, 0 };
    sb_fmt(&md, "```c\n%s\n```\n\n%s", s->detail[0] ? s->detail : s->name,
           kind_name(s->kind));
    if (s->file[0] && s->line)
        sb_fmt(&md, " — %s:%d", s->file, s->line);
    sb_str(&b, "{\"contents\":{\"kind\":\"markdown\",\"value\":");
    sb_json_str(&b, md.p);
    sb_str(&b, "}}");
    send_result(id, b.p);
    free(b.p);
    free(md.p);
}

static void definition(struct jv *id, struct doc *d, int line, int ch)
{
    char word[256];
    struct sym *s = lookup_at(d, line, ch, word, sizeof word);
    if (!s || !s->file[0] || s->line <= 0) { send_result(id, "null"); return; }
    struct sb b = { 0, 0, 0 };
    sb_str(&b, "{\"uri\":");
    struct sb uri = { 0, 0, 0 };
    if (s->file[0] == '/') sb_fmt(&uri, "file://%s", s->file);
    else sb_fmt(&uri, "%s", d->uri);
    sb_json_str(&b, uri.p);
    sb_fmt(&b, ",\"range\":{\"start\":{\"line\":%d,\"character\":%d},"
               "\"end\":{\"line\":%d,\"character\":%d}}}",
           s->line - 1, s->col > 0 ? s->col - 1 : 0,
           s->line - 1, s->col > 0 ? s->col - 1 : 0);
    send_result(id, b.p);
    free(b.p);
    free(uri.p);
}

/* ---- references, and the rename built on them ----------------------------
 *
 * grep finds a name. This finds the THING: the parse recorded every place
 * an identifier stands in expression position, so a match inside a comment
 * or a string is not a match at all, a member `p.x` is not the local `x`,
 * and a local named `n` in one function is not the global `n` used in
 * another. Renaming is then the same set with a new spelling, which is why
 * it is safe to offer.
 */

/* Does a local or parameter of this name shadow the file-scope one, in the
 * function `owner`, at line `line` (1-based)? */
static int shadowed(const char *name, const char *owner, int line)
{
    if (!owner || !*owner)
        return 0;
    for (int i = 0; i < g_nsyms; i++) {
        struct sym *s = &g_syms[i];
        if (s->kind != SYM_LOCAL && s->kind != SYM_PARAM)
            continue;
        if (strcmp(s->name, name) || strcmp(s->owner, owner))
            continue;
        if (line >= s->scope_start && line <= s->scope_end)
            return 1;
    }
    return 0;
}

/* Every use of what `target` declares. Returns indices into g_syms. */
static int collect_refs(struct sym *target, int **out)
{
    int cap = 32, n = 0;
    int *v = xmalloc((size_t)cap * sizeof *v);
    int local = target->kind == SYM_LOCAL || target->kind == SYM_PARAM;
    int member = target->kind == SYM_MEMBER;
    for (int i = 0; i < g_nsyms; i++) {
        struct sym *s = &g_syms[i];
        if (s->kind != SYM_REF || strcmp(s->name, target->name))
            continue;
        int is_member_use = s->detail[0] == 'm';
        if (member != is_member_use)
            continue;                    /* `p.x` is not the variable `x` */
        if (local) {
            /* the same function, and inside the declaration's scope */
            if (strcmp(s->owner, target->owner) ||
                s->line < target->scope_start || s->line > target->scope_end)
                continue;
        } else if (!member && shadowed(s->name, s->owner, s->line)) {
            continue;                    /* a local of that name owns this */
        }
        if (n == cap) {
            cap *= 2;
            v = xrealloc(v, (size_t)cap * sizeof *v);
        }
        v[n++] = i;
    }
    *out = v;
    return n;
}

/* The URI a symbol's file belongs to: the document itself unless the parse
 * placed it in a header it could name outright. */
static void sym_uri(struct sb *b, struct sym *s, struct doc *d)
{
    if (s->file[0] == '/')
        sb_fmt(b, "file://%s", s->file);
    else
        sb_fmt(b, "%s", d->uri);
}

static void one_location(struct sb *b, struct sym *s, struct doc *d, int len)
{
    struct sb uri = { 0, 0, 0 };
    sym_uri(&uri, s, d);
    sb_str(b, "{\"uri\":");
    sb_json_str(b, uri.p);
    int c0 = s->col > 0 ? s->col - 1 : 0;
    sb_fmt(b, ",\"range\":{\"start\":{\"line\":%d,\"character\":%d},"
              "\"end\":{\"line\":%d,\"character\":%d}}}",
           s->line - 1, c0, s->line - 1, c0 + len);
    free(uri.p);
}

static void references(struct jv *id, struct doc *d, int line, int ch,
                       int want_decl)
{
    char word[256];
    struct sym *t = lookup_at(d, line, ch, word, sizeof word);
    if (!t) { send_result(id, "null"); return; }
    int *v = NULL;
    int n = collect_refs(t, &v);
    int len = (int)strlen(t->name);
    struct sb b = { 0, 0, 0 };
    sb_str(&b, "[");
    int any = 0;
    if (want_decl && t->line > 0) {
        one_location(&b, t, d, len);
        any = 1;
    }
    for (int i = 0; i < n; i++) {
        struct sym *s = &g_syms[v[i]];
        if (want_decl && t->line == s->line && t->col == s->col)
            continue;                    /* the declaration, already in */
        if (any) sb_str(&b, ",");
        one_location(&b, s, d, len);
        any = 1;
    }
    sb_str(&b, "]");
    send_result(id, b.p);
    free(b.p);
    free(v);
}

/* The identifier under the cursor, so the editor can show what it is about
 * to rename before asking for the new spelling. */
static void prepare_rename(struct jv *id, struct doc *d, int line, int ch)
{
    char word[256];
    struct sym *t = lookup_at(d, line, ch, word, sizeof word);
    if (!t) { send_result(id, "null"); return; }
    int len = 0, start = ch;
    const char *lp = doc_line(d, line, &len);
    if (!lp) { send_result(id, "null"); return; }
    if (start > len) start = len;
    while (start > 0 && ident_char((unsigned char)lp[start - 1]))
        start--;
    int end = start;
    while (end < len && ident_char((unsigned char)lp[end]))
        end++;
    struct sb b = { 0, 0, 0 };
    sb_fmt(&b, "{\"start\":{\"line\":%d,\"character\":%d},"
               "\"end\":{\"line\":%d,\"character\":%d}}", line, start,
           line, end);
    send_result(id, b.p);
    free(b.p);
}

static void rename_sym(struct jv *id, struct doc *d, int line, int ch,
                       const char *newname)
{
    char word[256];
    struct sym *t = lookup_at(d, line, ch, word, sizeof word);
    if (!t || !newname || !*newname) { send_result(id, "null"); return; }
    int *v = NULL;
    int n = collect_refs(t, &v);
    int len = (int)strlen(t->name);
    struct sb uri = { 0, 0, 0 };
    sym_uri(&uri, t, d);
    struct sb b = { 0, 0, 0 };
    sb_str(&b, "{\"changes\":{");
    sb_json_str(&b, uri.p);
    sb_str(&b, ":[");
    int any = 0;
    /* The declaration too: a rename that leaves it behind does not compile. */
    if (t->line > 0) {
        sb_fmt(&b, "{\"range\":{\"start\":{\"line\":%d,\"character\":%d},"
                   "\"end\":{\"line\":%d,\"character\":%d}},\"newText\":",
               t->line - 1, t->col > 0 ? t->col - 1 : 0,
               t->line - 1, (t->col > 0 ? t->col - 1 : 0) + len);
        sb_json_str(&b, newname);
        sb_str(&b, "}");
        any = 1;
    }
    for (int i = 0; i < n; i++) {
        struct sym *s = &g_syms[v[i]];
        if (t->line == s->line && t->col == s->col)
            continue;
        if (any) sb_str(&b, ",");
        int c0 = s->col > 0 ? s->col - 1 : 0;
        sb_fmt(&b, "{\"range\":{\"start\":{\"line\":%d,\"character\":%d},"
                   "\"end\":{\"line\":%d,\"character\":%d}},\"newText\":",
               s->line - 1, c0, s->line - 1, c0 + len);
        sb_json_str(&b, newname);
        sb_str(&b, "}");
        any = 1;
    }
    sb_str(&b, "]}}");
    send_result(id, b.p);
    free(b.p);
    free(uri.p);
    free(v);
}

/* ---- signature help ------------------------------------------------------
 *
 * Inside a call's parentheses: which function, and which argument the
 * cursor is on. The signature comes from the index, so it is the one the
 * compiler parsed, parameter names included.
 */
static void signature_help(struct jv *id, struct doc *d, int line, int ch)
{
    int len = 0;
    const char *lp = doc_line(d, line, &len);
    if (!lp) { send_result(id, "null"); return; }
    if (ch > len) ch = len;
    /* Walk back to the '(' that is still open, counting the commas at that
     * depth on the way -- those are the arguments already given. */
    int depth = 0, comma = 0, i = ch - 1;
    for (; i >= 0; i--) {
        char c = lp[i];
        if (c == ')') depth++;
        else if (c == '(') { if (depth == 0) break; depth--; }
        else if (c == ',' && depth == 0) comma++;
    }
    if (i < 0) { send_result(id, "null"); return; }
    int e = i;                           /* the callee's name ends before '(' */
    while (e > 0 && (lp[e - 1] == ' ' || lp[e - 1] == '\t')) e--;
    int s0 = e;
    while (s0 > 0 && ident_char((unsigned char)lp[s0 - 1])) s0--;
    if (s0 == e) { send_result(id, "null"); return; }
    char name[256];
    int nl = e - s0 < (int)sizeof name - 1 ? e - s0 : (int)sizeof name - 1;
    memcpy(name, lp + s0, (size_t)nl);
    name[nl] = 0;

    struct sym *fn = NULL;
    for (int k = 0; k < g_nsyms; k++)
        if (g_syms[k].kind == SYM_FUNC && !strcmp(g_syms[k].name, name)) {
            fn = &g_syms[k];
            break;
        }
    if (!fn || !fn->detail[0]) { send_result(id, "null"); return; }

    /* Split the recorded signature's parameter list, so each parameter can
     * be highlighted in turn. */
    const char *op = strchr(fn->detail, '(');
    const char *cp = op ? strrchr(fn->detail, ')') : NULL;
    struct sb b = { 0, 0, 0 };
    sb_str(&b, "{\"signatures\":[{\"label\":");
    sb_json_str(&b, fn->detail);
    sb_str(&b, ",\"parameters\":[");
    int np = 0;
    if (op && cp && cp > op + 1) {
        const char *p = op + 1;
        int dep = 0;
        const char *start = p;
        for (; p <= cp; p++) {
            if (p < cp && (*p == '(' || *p == '[')) dep++;
            else if (p < cp && (*p == ')' || *p == ']')) dep--;
            if (p == cp || (*p == ',' && dep == 0)) {
                while (start < p && *start == ' ') start++;
                const char *end = p;
                while (end > start && end[-1] == ' ') end--;
                if (end > start) {
                    char par[256];
                    int pl = (int)(end - start);
                    if (pl > (int)sizeof par - 1) pl = (int)sizeof par - 1;
                    memcpy(par, start, (size_t)pl);
                    par[pl] = 0;
                    if (np) sb_str(&b, ",");
                    sb_str(&b, "{\"label\":");
                    sb_json_str(&b, par);
                    sb_str(&b, "}");
                    np++;
                }
                start = p + 1;
            }
        }
    }
    if (comma >= np && np > 0)
        comma = np - 1;                  /* a varargs tail stays on the last */
    sb_fmt(&b, "]}],\"activeSignature\":0,\"activeParameter\":%d}",
           np ? comma : 0);
    send_result(id, b.p);
    free(b.p);
}

static void document_symbols(struct jv *id, struct doc *d)
{
    struct sb b = { 0, 0, 0 };
    sb_str(&b, "[");
    int any = 0;
    for (int i = 0; i < g_nsyms; i++) {
        struct sym *s = &g_syms[i];
        if ((s->kind != SYM_FUNC && s->kind != SYM_VAR) || s->line <= 0)
            continue;
        if (strcmp(s->file, d->path) != 0)
            continue;                        /* this file's own, not headers' */
        sb_fmt(&b, "%s{\"name\":", any ? "," : "");
        sb_json_str(&b, s->name);
        sb_fmt(&b, ",\"kind\":%d,\"range\":{\"start\":{\"line\":%d,"
                   "\"character\":0},\"end\":{\"line\":%d,\"character\":0}},"
                   "\"selectionRange\":{\"start\":{\"line\":%d,\"character\":0},"
                   "\"end\":{\"line\":%d,\"character\":0}},\"detail\":",
               s->kind == SYM_FUNC ? 12 : 13, s->line - 1, s->line - 1,
               s->line - 1, s->line - 1);
        sb_json_str(&b, s->detail);
        sb_str(&b, "}");
        any = 1;
    }
    sb_str(&b, "]");
    send_result(id, b.p);
    free(b.p);
}

/* ---- the loop ---- */

static void refresh(struct doc *d)
{
    flags_load(d->path);
    index_build(d);
    publish_diagnostics(d);
}

static char *read_message(void)
{
    char line[1024];
    long len = -1;
    for (;;) {
        if (!fgets(line, sizeof line, stdin))
            return NULL;
        if (line[0] == '\r' || line[0] == '\n')
            break;                                   /* end of the headers */
        if (!strncasecmp(line, "Content-Length:", 15))
            len = strtol(line + 15, NULL, 10);
    }
    if (len < 0)
        return NULL;
    char *body = xmalloc((size_t)len + 1);
    long got = 0;
    while (got < len) {
        size_t n = fread(body + got, 1, (size_t)(len - got), stdin);
        if (!n) break;
        got += (long)n;
    }
    body[got] = 0;
    return body;
}

int main(void)
{
    /* The index is built by EmbCC's own front end; it needs a target to
     * predefine for, and the host one will do for answering questions. */
    target_set(TARGET_X86_64);
    setvbuf(stdout, NULL, _IOFBF, 1 << 16);

    for (;;) {
        char *body = read_message();
        if (!body)
            break;
        const char *p = body;
        struct jv *msg = js_value(&p);
        const char *method = jstr(msg, "method");
        struct jv *id = jget(msg, "id");
        struct jv *params = jget(msg, "params");
        if (!method) { free(body); continue; }

        if (!strcmp(method, "initialize")) {
            send_result(id,
                "{\"capabilities\":{"
                "\"textDocumentSync\":1,"
                "\"completionProvider\":{\"triggerCharacters\":[\".\",\">\"]},"
                "\"hoverProvider\":true,"
                "\"definitionProvider\":true,"
                "\"referencesProvider\":true,"
                "\"renameProvider\":{\"prepareProvider\":true},"
                "\"signatureHelpProvider\":{\"triggerCharacters\":[\"(\",\",\"]},"
                "\"documentSymbolProvider\":true},"
                "\"serverInfo\":{\"name\":\"embls\",\"version\":\"0.1\"}}");
        } else if (!strcmp(method, "shutdown")) {
            send_result(id, "null");
        } else if (!strcmp(method, "exit")) {
            free(body);
            break;
        } else if (!strcmp(method, "textDocument/didOpen")) {
            struct jv *td = jget(params, "textDocument");
            const char *uri = jstr(td, "uri"), *text = jstr(td, "text");
            if (uri && text)
                refresh(doc_put(uri, text));
        } else if (!strcmp(method, "textDocument/didChange")) {
            struct jv *td = jget(params, "textDocument");
            struct jv *ch = jget(params, "contentChanges");
            const char *uri = jstr(td, "uri");
            if (uri && ch && ch->kind == JARR && ch->n) {
                const char *text = jstr(ch->kids[ch->n - 1], "text");
                if (text)
                    refresh(doc_put(uri, text));
            }
        } else if (!strcmp(method, "textDocument/didClose")) {
            /* the document stays indexed; nothing to answer */
        } else if (!strcmp(method, "textDocument/completion") ||
                   !strcmp(method, "textDocument/hover") ||
                   !strcmp(method, "textDocument/definition") ||
                   !strcmp(method, "textDocument/references") ||
                   !strcmp(method, "textDocument/prepareRename") ||
                   !strcmp(method, "textDocument/rename") ||
                   !strcmp(method, "textDocument/signatureHelp") ||
                   !strcmp(method, "textDocument/documentSymbol")) {
            struct jv *td = jget(params, "textDocument");
            const char *uri = jstr(td, "uri");
            struct doc *d = uri ? doc_find(uri) : NULL;
            if (!d) {
                send_result(id, "null");
            } else if (!strcmp(method, "textDocument/documentSymbol")) {
                document_symbols(id, d);
            } else {
                struct jv *pos = jget(params, "position");
                int line = jint(pos, "line"), ch = jint(pos, "character");
                if (!strcmp(method, "textDocument/completion"))
                    completion(id, d, line, ch);
                else if (!strcmp(method, "textDocument/hover"))
                    hover(id, d, line, ch);
                else if (!strcmp(method, "textDocument/references")) {
                    struct jv *ctx = jget(params, "context");
                    struct jv *inc = ctx ? jget(ctx, "includeDeclaration")
                                         : NULL;
                    references(id, d, line, ch,
                               !inc || inc->kind != JBOOL || inc->bval);
                } else if (!strcmp(method, "textDocument/prepareRename"))
                    prepare_rename(id, d, line, ch);
                else if (!strcmp(method, "textDocument/rename"))
                    rename_sym(id, d, line, ch, jstr(params, "newName"));
                else if (!strcmp(method, "textDocument/signatureHelp"))
                    signature_help(id, d, line, ch);
                else
                    definition(id, d, line, ch);
            }
        } else if (id) {
            send_result(id, "null");         /* a request we do not answer */
        }
        free(body);
    }
    return 0;
}
