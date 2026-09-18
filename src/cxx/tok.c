/* The C++ unit as a token array. C++ needs to look back and replay: a
 * member function defined in a class body is parsed only once the class is
 * complete (its body may use members declared after it), and a template is
 * instantiated by replaying its tokens — so the whole preprocessed unit is
 * lexed up front, with the file each token came from (line markers). */
#include "cxx.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../driver/util.h"

struct ctok *cx_toks;
int cx_ntoks;
int cx_pos;

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
    if (cx_pos < cx_ntoks - 1)
        cx_pos++;
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

void cx_error(const struct ctok *at, const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    diag_error_at(at ? at->file : "<c++>", at ? at->t.line : 0,
                  at ? at->t.col : 0, "%s", msg);
    exit(1);
}

char *cx_strdup(const char *s)
{
    return xstrndup(s, strlen(s));
}

char *cx_fmt(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return cx_strdup(buf);
}

int cx_uid(void)
{
    static int n;
    return ++n;
}
