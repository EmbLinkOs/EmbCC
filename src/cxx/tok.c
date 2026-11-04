/* The C++ unit as a token array. C++ needs to look back and replay: a
 * member function defined in a class body is parsed only once the class is
 * complete (its body may use members declared after it), and a template is
 * instantiated by replaying its tokens — so the whole preprocessed unit is
 * lexed up front, with the file each token came from (line markers). */
#include "cxx.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../driver/util.h"

struct ctok *cx_toks;
int cx_ntoks;
int cx_pos;
int cx_half_gt;
void *cx_sfinae;

static void strip_packs(void);

void cx_tokenize(const char *file, const char *src)
{
    struct lexer lx;
    int cap = 1024;
    cx_toks = xmalloc((size_t)cap * sizeof *cx_toks);
    cx_ntoks = 0;
    lex_init_mode(&lx, file, src, 1);
    for (;;) {
        if (cx_ntoks == cap) {
            cap *= 2;
            cx_toks = xrealloc(cx_toks, (size_t)cap * sizeof *cx_toks);
        }
        cx_toks[cx_ntoks].t = lx.tok;
        cx_toks[cx_ntoks].file = lx.file;
        cx_ntoks++;
        if (lx.tok.kind == TOK_EOF)
            break;
        lex_next(&lx);
    }
    strip_packs();
    cx_pos = 0;
}

/* `#pragma pack(...)`, which the preprocessor hands on as
 * `__embcc_pack(args)`: taken out of the token array, each change kept
 * with the position of the token after it. A class's members are aligned
 * by the value in effect where its body starts (cx_pack_at); a template's
 * instance replays its definition's tokens, so it has its definition's.
 * (n), (), (push), (push, n), (pop), (show), as in C. */
struct pack_ev {
    int pos;
    int push, pop, set, n;
};
static struct pack_ev *pack_evs;
static int npack_evs, cap_pack_evs;

int cx_pack_at(int pos)
{
    int cur = 0, stack[32], depth = 0;
    for (int i = 0; i < npack_evs && pack_evs[i].pos <= pos; i++) {
        const struct pack_ev *e = &pack_evs[i];
        if (e->pop) {
            cur = depth ? stack[--depth] : 0;
            continue;
        }
        if (e->push && depth < 32)
            stack[depth++] = cur;
        if (e->set)
            cur = e->n;
    }
    return cur;
}

/* Is there a pack change in (from, to)? (A class body's: refused.) */
int cx_pack_between(int from, int to)
{
    for (int i = 0; i < npack_evs; i++)
        if (pack_evs[i].pos > from && pack_evs[i].pos < to)
            return 1;
    return 0;
}

static void strip_packs(void)
{
    int out = 0, depth = 0;
    npack_evs = 0;
    for (int i = 0; i < cx_ntoks; i++) {
        struct ctok *k = &cx_toks[i];
        if (!(k->t.kind == TOK_IDENT && strcmp(k->t.text, "__embcc_pack") == 0 &&
              i + 1 < cx_ntoks && cx_toks[i + 1].t.kind == TOK_LPAREN)) {
            cx_toks[out++] = *k;
            continue;
        }
        struct pack_ev e = { out, 0, 0, 0, 0 };
        int j = i + 2;
        struct ctok *a = &cx_toks[j];
        if (a->t.kind == TOK_IDENT && (!strcmp(a->t.text, "push") ||
                                      !strcmp(a->t.text, "pop") ||
                                      !strcmp(a->t.text, "show"))) {
            const char *w = a->t.text;
            j++;
            if (!strcmp(w, "pop")) {
                if (cx_toks[j].t.kind != TOK_RPAREN)
                    cx_error(k, "#pragma pack(pop, ...) with a name or count "
                                "is not supported");
                if (depth == 0)
                    cx_error(k, "#pragma pack(pop) without a matching push");
                depth--;
                e.pop = 1;
            } else if (!strcmp(w, "push")) {
                e.push = 1;
                if (depth++ == 32)
                    cx_error(k, "#pragma pack(push) nested too deeply");
                if (cx_toks[j].t.kind == TOK_COMMA) {
                    j++;
                    if (cx_toks[j].t.kind == TOK_IDENT)
                        cx_error(k, "#pragma pack(push, name) is not "
                                    "supported");
                    e.set = 1;
                }
            }
        } else if (a->t.kind != TOK_RPAREN) {
            e.set = 1;
        } else {
            e.set = 1;                     /* pack(): back to none */
            e.n = 0;
        }
        if (e.set && cx_toks[j].t.kind != TOK_RPAREN) {
            long v = cx_toks[j].t.num;
            if (cx_toks[j].t.kind != TOK_NUM ||
                (v != 0 && v != 1 && v != 2 && v != 4 && v != 8 && v != 16))
                cx_error(k, "#pragma pack wants 1, 2, 4, 8 or 16");
            e.n = (int)v;
            j++;
        }
        if (cx_toks[j].t.kind != TOK_RPAREN)
            cx_error(k, "')' to close #pragma pack");
        if (npack_evs == cap_pack_evs) {
            cap_pack_evs = cap_pack_evs ? cap_pack_evs * 2 : 8;
            pack_evs = xrealloc(pack_evs,
                                (size_t)cap_pack_evs * sizeof *pack_evs);
        }
        pack_evs[npack_evs++] = e;
        i = j;                             /* (past the `)`) */
    }
    cx_ntoks = out;
}

void cx_advance(void)
{
    cx_half_gt = 0;
    if (cx_pos < cx_ntoks - 1)
        cx_pos++;
}

void cx_close_angle(void)
{
    if (cx_toks[cx_pos].t.kind == TOK_SHR && !cx_half_gt) {
        /* (C++98 reads `>>` as a shift: `> >` there) */
        cx_need_std(cx_cur(), 2011, "'>>' closing two template argument lists");
        cx_half_gt = 1;             /* the second `>` closes the outer */
        return;
    }
    if (cx_kind() != TOK_GT)
        cx_error(cx_cur(), "expected '>' to close the template arguments "
                           "before %s", tok_describe(&cx_cur()->t));
    cx_advance();
}

int cx_accept(enum tok_kind k)
{
    if (cx_kind() == k) {
        cx_advance();
        return 1;
    }
    return 0;
}

void cx_expect(enum tok_kind k, const char *what)
{
    if (cx_kind() != k)
        cx_error(cx_cur(), "expected %s before %s", what,
                 tok_describe(&cx_cur()->t));
    cx_advance();
}

int cx_is_ident(const char *name)
{
    return cx_kind() == TOK_IDENT && strcmp(cx_cur()->t.text, name) == 0;
}

/* Past a bracketed group starting at the current ( [ or {, nested ones
 * included. */
void cx_skip_balanced(void)
{
    cx_half_gt = 0;
    int depth = 0;
    const struct ctok *open = cx_cur();
    do {
        switch (cx_kind()) {
        case TOK_LPAREN: case TOK_LBRACKET: case TOK_LBRACE:
            depth++;
            break;
        case TOK_RPAREN: case TOK_RBRACKET: case TOK_RBRACE:
            depth--;
            break;
        case TOK_EOF:
            cx_error(open, "unbalanced %s", tok_describe(&open->t));
            break;
        default:
            break;
        }
        cx_advance();
    } while (depth > 0);
}

struct ctok *cx_tok_at(int pos)
{
    return &cx_toks[pos < cx_ntoks ? pos : cx_ntoks - 1];
}

void cx_warn(const struct ctok *at, const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    diag_warn_at(at ? at->file : "<c++>", at ? at->t.line : 0,
                 at ? at->t.col : 0, "%s", msg);
}

/* Where the C++ front end resumes after an error, and how many it has
 * reported (docs/manual/diagnostics.md T2). NULL means the error ends the compile, as
 * every C++ error once did — which is still the case once parsing is over
 * and instantiation has begun, where there is no statement boundary to
 * resume at. */
void *cx_recover;
int cx_nerrors;

void cx_error(const struct ctok *at, const char *fmt, ...)
{
    if (cx_sfinae)                  /* a substitution failure, not an error */
        longjmp(*(jmp_buf *)cx_sfinae, 1);
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    diag_error_at(at ? at->file : "<c++>", at ? at->t.line : 0,
                  at ? at->t.col : 0, "%s", msg);
    cx_inst_notes();
    cx_nerrors++;
    if (cx_recover)
        longjmp(*(jmp_buf *)cx_recover, 1);
    fatal_unwind();
}

int cx_std_at(const struct ctok *t)
{
    return t && t->file && diag_in_system_header(t->file) ? 9999
                                                          : lex_cxx_year();
}

void cx_need_std(const struct ctok *t, int year, const char *what)
{
    if (cx_std_at(t) < year)
        cx_error(t, "%s is C++%02d's, and the standard here is C++%02d",
                 what, year % 100, lex_cxx_year() % 100);
}

/* each place said once: a template's body is read again for each
 * instance */
static int pedantic_once(const struct ctok *t)
{
    static const struct ctok *seen[256];
    static unsigned next;
    for (int i = 0; i < 256; i++)
        if (seen[i] == t)
            return 0;
    seen[next++ % 256] = t;
    return 1;
}

static int pedantic_here(const struct ctok *t)
{
    return t && diag_warning_enabled("pedantic") &&
           !(t->file && diag_in_system_header(t->file));
}

void cx_pedantic(const struct ctok *t, int since, const char *what)
{
    if (!pedantic_here(t) || (since && cx_std_at(t) >= since) ||
        !pedantic_once(t))
        return;
    if (since)
        diag_warn_opt(t->file, t->t.line, t->t.col, "pedantic",
                      "%s is new in C++%02d, and the standard here is C++%02d",
                      what, since % 100, lex_cxx_year() % 100);
    else
        diag_warn_opt(t->file, t->t.line, t->t.col, "pedantic",
                      "%s is a GNU extension, not ISO C++", what);
}

void cx_pedantic_removed(const struct ctok *t, int removed, const char *what)
{
    if (!pedantic_here(t) || cx_std_at(t) < removed || !pedantic_once(t))
        return;
    diag_warn_opt(t->file, t->t.line, t->t.col, "pedantic",
                  "%s is not C++%02d: that standard removed it", what,
                  removed % 100);
}

char *cx_strdup(const char *s)
{
    return xstrndup(s, strlen(s));
}

char *cx_fmt(const char *fmt, ...)
{
    char buf[1024];
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < (int)sizeof buf) {
        va_end(ap2);
        return cx_strdup(buf);
    }
    char *big = xmalloc((size_t)n + 1);        /* longer than the buffer */
    vsnprintf(big, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return big;
}

int cx_uid(void)
{
    static int n;
    return ++n;
}
