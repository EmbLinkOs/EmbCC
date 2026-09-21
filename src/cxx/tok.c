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
    cx_pos = 0;
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
 * reported (docs/tools/diagnostics.md T2). NULL means the error ends the compile, as
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
