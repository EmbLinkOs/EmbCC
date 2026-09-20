/* The stage dumps behind `embcc inspect` (vision §18, R6).
 *
 * "Every arrow is a library boundary with a dump format." The IR's own form
 * lives in src/ir/irprint.c, next to the thing it prints; the rest are here,
 * because they are views ACROSS stages that the driver assembles — the
 * symbol table is the semantic model's answer, the type table is what layout
 * decided, and neither belongs to one file in src/parse or src/sema.
 *
 * These are reports, not serializations. §9.1's round-trip requirement is
 * about EmbIR, and the reason it cannot be met yet is recorded in
 * src/ir/irprint.c; the dumps below have no such ambition — they exist so a
 * person can see what a stage built, and so a test can assert on it without
 * reading machine code.
 */
#include "inspect.h"

#include <stdio.h>
#include <string.h>

#include "util.h"
#include "../lex/lex.h"
#include "../parse/ast.h"
#include "../ir/ir.h"
#include "../sema/type.h"

/* ---- tokens -------------------------------------------------------------
 *
 * After preprocessing, which is the only place the token stream is a real
 * thing: before it, `#include` has not happened; after parsing, the tokens
 * are gone. One per line with its position, because the questions this
 * answers are "what did the lexer make of that?" and "which line is it
 * blaming?".
 */
void inspect_tokens(struct outbuf *b, const char *file, const char *pp,
                    int cxx)
{
    struct lexer lx;
    lex_init_mode(&lx, file, pp, cxx);
    long n = 0;
    while (lx.tok.kind != TOK_EOF) {
        const struct token *t = &lx.tok;
        ob_fmt(b, "%4d:%-3d %-18s", t->line, t->col, tok_describe(t));
        switch (t->kind) {
        case TOK_IDENT:
            ob_fmt(b, " %s", t->text ? t->text : "");
            break;
        case TOK_NUM:
            ob_fmt(b, " %ld%s%s", t->num, t->num_uns ? "u" : "",
                   t->num_llong ? "ll" : t->num_long ? "l" : "");
            if (t->char_lit)
                ob_str(b, "  (character constant)");
            break;
        case TOK_FNUM:
            ob_fmt(b, " %g%s", t->fnum, t->fnum_is_float ? "f" : "");
            break;
        case TOK_STR:
            /* The decoded bytes, not the spelling: escapes are already
             * resolved here, which is usually what the reader is checking. */
            ob_str(b, " \"");
            for (long k = 0; t->text && k + 1 < t->num; k++) {
                char c = t->text[k];
                if (c == '\n')      ob_str(b, "\\n");
                else if (c == '\t') ob_str(b, "\\t");
                else if (c == '"')  ob_str(b, "\\\"");
                else if (c < 32)    ob_fmt(b, "\\x%02x", (unsigned char)c);
                else                ob_ch(b, c);
            }
            ob_ch(b, '"');
            if (t->str_prefix)
                ob_fmt(b, "  (%c, %d bytes/element)", t->str_prefix,
                       t->str_width);
            break;
        default:
            break;
        }
        ob_ch(b, '\n');
        n++;
        lex_next(&lx);
    }
    ob_fmt(b, "; %ld tokens\n", n);
}

/* ---- the AST ------------------------------------------------------------
 *
 * Indented, one node per line, with the source position. Enough to see the
 * SHAPE the parser built — which is the question an AST dump answers, and
 * why the expression forms print their operator rather than their operands'
 * types (those belong to the symbol dump).
 */
static void ind(struct outbuf *b, int d)
{
    for (int i = 0; i < d; i++)
        ob_str(b, "  ");
}

static const char *binop_name(enum binop op)
{
    switch (op) {
    case B_ADD: return "+";   case B_SUB: return "-";
    case B_MUL: return "*";   case B_DIV: return "/";
    case B_MOD: return "%";   case B_AND: return "&";
    case B_OR:  return "|";   case B_XOR: return "^";
    case B_SHL: return "<<";  case B_SHR: return ">>";
    case B_EQ:  return "==";  case B_NE:  return "!=";
    case B_LT:  return "<";   case B_LE:  return "<=";
    case B_GT:  return ">";   case B_GE:  return ">=";
    case B_LAND: return "&&"; case B_LOR: return "||";
    default: return "?";
    }
}

static void pexpr(struct outbuf *b, const struct expr *e, int d)
{
    if (!e)
        return;
    ind(b, d);
    switch (e->kind) {
    case EXPR_NUM:    ob_fmt(b, "num %ld", e->num); break;
    case EXPR_FNUM:   ob_fmt(b, "fnum %g", e->fnum); break;
    case EXPR_STR:    ob_fmt(b, "string (%ld bytes)", e->num); break;
    case EXPR_VAR:    ob_fmt(b, "var %s", e->name ? e->name : "?"); break;
    case EXPR_BINOP:  ob_fmt(b, "binop %s", binop_name(e->op)); break;
    case EXPR_ASSIGN: ob_str(b, "assign"); break;
    case EXPR_COMPOUND: ob_fmt(b, "compound-assign %s=", binop_name(e->op));
                      break;
    case EXPR_CALL:   ob_fmt(b, "call (%d args)", e->nargs); break;
    case EXPR_MEMBER: ob_fmt(b, "member %s%s", e->is_arrow ? "->" : ".",
                             e->name ? e->name : "?"); break;
    case EXPR_INCDEC: ob_fmt(b, "%s%s %s", e->delta > 0 ? "inc" : "dec",
                             e->is_post ? "-post" : "-pre",
                             e->name ? e->name : ""); break;
    case EXPR_COND:   ob_str(b, "conditional ?:"); break;
    case EXPR_CAST:   ob_fmt(b, "cast to %s",
                             e->cast_ty ? ty_name(e->cast_ty) : "?"); break;
    case EXPR_DEREF:  ob_str(b, "deref *"); break;
    case EXPR_ADDR:   ob_str(b, "addr-of &"); break;
    case EXPR_NEG:    ob_str(b, "negate"); break;
    case EXPR_NOT:    ob_str(b, "logical-not"); break;
    case EXPR_BNOT:   ob_str(b, "bitwise-not"); break;
    case EXPR_SIZEOF: ob_str(b, "sizeof"); break;
    case EXPR_COMMA:  ob_str(b, "comma"); break;
    case EXPR_INITLIST: ob_fmt(b, "init-list (%d)", e->nelems); break;
    default:          ob_fmt(b, "expr kind %d", (int)e->kind); break;
    }
    if (e->ty)
        ob_fmt(b, " : %s", ty_name(e->ty));
    if (e->line)
        ob_fmt(b, "\t; %d:%d", e->line, e->col);
    ob_ch(b, '\n');

    pexpr(b, e->lhs, d + 1);
    pexpr(b, e->rhs, d + 1);
    for (int i = 0; i < e->nargs; i++)
        pexpr(b, e->args[i], d + 1);
    for (int i = 0; i < e->nelems; i++)
        pexpr(b, e->elems[i], d + 1);
}

static void pstmt(struct outbuf *b, const struct stmt *s, int d)
{
    for (; s; s = s->next) {
        ind(b, d);
        switch (s->kind) {
        case STMT_DECL:
            ob_fmt(b, "decl %s : %s", s->name ? s->name : "?",
                   s->dty ? ty_name(s->dty) : "?");
            if (s->is_static) ob_str(b, " static");
            break;
        case STMT_EXPR:     ob_str(b, "expr-statement"); break;
        case STMT_RETURN:   ob_str(b, "return"); break;
        case STMT_IF:       ob_str(b, "if"); break;
        case STMT_WHILE:    ob_str(b, "while"); break;
        case STMT_DO:       ob_str(b, "do-while"); break;
        case STMT_FOR:      ob_str(b, "for"); break;
        case STMT_SWITCH:   ob_str(b, "switch"); break;
        case STMT_CASE:     ob_fmt(b, "case %ld", s->cval); break;
        case STMT_DEFAULT:  ob_str(b, "default"); break;
        case STMT_BREAK:    ob_str(b, "break"); break;
        case STMT_CONTINUE: ob_str(b, "continue"); break;
        case STMT_BLOCK:    ob_str(b, "block"); break;
        case STMT_LABEL:    ob_fmt(b, "label %s", s->name ? s->name : "?");
                            break;
        case STMT_GOTO:     ob_fmt(b, "goto %s", s->name ? s->name : "?");
                            break;
        case STMT_ASM:      ob_str(b, "asm"); break;
        default:            ob_fmt(b, "stmt kind %d", (int)s->kind); break;
        }
        if (s->line)
            ob_fmt(b, "\t; %d:%d", s->line, s->col);
        ob_ch(b, '\n');

        pexpr(b, s->expr, d + 1);
        pexpr(b, s->cond, d + 1);
        pexpr(b, s->init, d + 1);
        pexpr(b, s->step, d + 1);
        pstmt(b, s->initdecl, d + 1);
        pstmt(b, s->thn, d + 1);
        pstmt(b, s->els, d + 1);
        pstmt(b, s->body, d + 1);
    }
}

void inspect_ast(struct outbuf *b, const struct unit *u)
{
    for (const struct func *f = u->funcs; f; f = f->next) {
        if (f->absorbed || !f->body)
            continue;
        ob_fmt(b, "function %s : %s(", f->name,
               f->ret_ty ? ty_name(f->ret_ty) : "?");
        for (int i = 0; i < f->nparams; i++)
            ob_fmt(b, "%s%s %s", i ? ", " : "",
                   f->param_tys[i] ? ty_name(f->param_tys[i]) : "?",
                   f->params[i] ? f->params[i] : "");
        ob_fmt(b, "%s)\t; %s:%d\n", f->is_varargs ? ", ..." : "",
               f->file ? f->file : "", f->line);
        pstmt(b, f->body, 1);
        ob_ch(b, '\n');
    }
}

/* ---- symbols ------------------------------------------------------------
 *
 * What the unit declares, after semantic analysis has resolved it: the
 * answer to "is this name what I think it is", which is the question that
 * sends people to `nm` on an object file and then to guessing.
 */
void inspect_symbols(struct outbuf *b, const struct unit *u)
{
    for (const struct func *f = u->funcs; f; f = f->next) {
        if (f->absorbed)
            continue;
        ob_fmt(b, "%-9s %-24s %s(", f->has_defn ? "function" : "declared",
               f->name, f->ret_ty ? ty_name(f->ret_ty) : "?");
        for (int i = 0; i < f->nparams; i++)
            ob_fmt(b, "%s%s", i ? ", " : "",
                   f->param_tys[i] ? ty_name(f->param_tys[i]) : "?");
        ob_fmt(b, "%s)", f->is_varargs ? ", ..." : "");
        if (f->is_static) ob_str(b, "  static");
        if (f->is_weak)   ob_str(b, "  weak");
        if (f->is_noreturn) ob_str(b, "  noreturn");
        ob_fmt(b, "\t; %s:%d\n", f->file ? f->file : "", f->line);
    }
    for (const struct global *g = u->globals; g; g = g->next) {
        if (g->absorbed)
            continue;
        ob_fmt(b, "%-9s %-24s %s", g->is_extern ? "extern" : "variable",
               g->name, g->ty ? ty_name(g->ty) : "?");
        if (g->ty)
            ob_fmt(b, "  (%d bytes, align %d)", ty_size(g->ty),
                   ty_align(g->ty));
        if (g->is_static) ob_str(b, "  static");
        ob_fmt(b, "\t; %s:%d\n", g->file ? g->file : "", g->line);
    }
    for (const struct econst *e = u->econsts; e; e = e->next)
        ob_fmt(b, "%-9s %-24s = %ld\n", "enumerator", e->name, e->val);
    for (const struct typedefent *t = u->typedefs; t; t = t->next)
        ob_fmt(b, "%-9s %-24s %s\n", "typedef", t->name,
               t->ty ? ty_name(t->ty) : "?");
}

/* ---- types --------------------------------------------------------------
 *
 * Layout is a decision the compiler makes and then never explains, and it is
 * the one an ABI argument always comes down to: offsets, padding, and the
 * size the padding made. A bit-field prints its bit position too, because
 * that is the part nobody can predict from the source.
 */
void inspect_types(struct outbuf *b, const struct unit *u)
{
    for (const struct tagdef *t = u->tags; t; t = t->next) {
        const char *kind = t->kind == TAG_UNION ? "union"
                         : t->kind == TAG_ENUM ? "enum" : "struct";
        ob_fmt(b, "%s %s", kind, t->tag ? t->tag : "(anonymous)");
        if (!t->ty || t->kind == TAG_ENUM) {
            ob_str(b, "\n");
            continue;
        }
        ob_fmt(b, "  (%d bytes, align %d)\n", ty_size(t->ty),
               ty_align(t->ty));
        int prev_end = 0;
        for (int i = 0; i < t->ty->nmembers; i++) {
            const struct member *m = &t->ty->members[i];
            /* Padding is invisible in the source and expensive in an
             * embedded target's RAM, so it is named rather than implied by
             * a gap in the offsets. */
            if (m->off > prev_end)
                ob_fmt(b, "  %4s  %-20s %d byte%s\n", "", "(padding)",
                       m->off - prev_end, m->off - prev_end == 1 ? "" : "s");
            ob_fmt(b, "  %4d  %-20s %s", m->off,
                   m->name ? m->name : "(unnamed)",
                   m->ty ? ty_name(m->ty) : "?");
            if (m->is_bitfield)
                ob_fmt(b, " : %d  (bits %d-%d of the unit at %d)",
                       m->bit_width, m->bit_off,
                       m->bit_off + m->bit_width - 1, m->off);
            else if (m->ty)
                ob_fmt(b, "  (%d byte%s)", ty_size(m->ty),
                       ty_size(m->ty) == 1 ? "" : "s");
            ob_ch(b, '\n');
            /* A bit-field ends at the byte its last BIT falls in, not at
             * its storage unit's end -- otherwise the padding after a group
             * of them is reported against the wrong place. */
            if (m->ty) {
                int end = m->is_bitfield
                    ? m->off + (m->bit_off + m->bit_width + 7) / 8
                    : m->off + ty_size(m->ty);
                if (end > prev_end)
                    prev_end = end;
            }
        }
        if (ty_size(t->ty) > prev_end)
            ob_fmt(b, "  %4s  %-20s %d byte%s\n", "", "(tail padding)",
                   ty_size(t->ty) - prev_end,
                   ty_size(t->ty) - prev_end == 1 ? "" : "s");
        ob_ch(b, '\n');
    }
}

/* ---- the call graph (§18) --------------------------------------------
 *
 * Who calls whom, within this unit, from the IR rather than from the
 * source — so a call the front end generated (a constructor, a temporary's
 * destructor, a libgcc helper for a 128-bit divide) appears exactly as a
 * hand-written one does. That is the whole point of reading it here: the
 * graph the linker and the stack analysis will see, not the one the source
 * suggests.
 */
void inspect_callgraph(struct outbuf *b, const struct ir_unit *u)
{
    for (int i = 0; i < u->nfuncs; i++) {
        const struct ir_func *f = &u->funcs[i];
        ob_fmt(b, "%s", f->name ? f->name : "?");
        if (f->is_static)
            ob_str(b, " (static)");
        ob_str(b, "\n");
        int any = 0;
        /* Each callee once, in first-call order: a loop calling the same
         * helper thirty times is one edge, and the order is the order a
         * reader finds them in the body. */
        for (int n = 0; n < f->nins; n++) {
            const struct ir_ins *in = &f->ins[n];
            if (in->op != IR_CALL && in->op != IR_FADDR)
                continue;
            if (in->indirect) {
                int seen = 0;
                for (int m = 0; m < n; m++)
                    if (f->ins[m].op == IR_CALL && f->ins[m].indirect)
                        seen = 1;
                if (!seen) {
                    ob_str(b, "    -> (through a function pointer)\n");
                    any = 1;
                }
                continue;
            }
            int sym = in->callee_sym;
            if (sym < 0 || sym >= u->nsyms)
                continue;
            int seen = 0;
            for (int m = 0; m < n && !seen; m++)
                if ((f->ins[m].op == IR_CALL || f->ins[m].op == IR_FADDR) &&
                    f->ins[m].callee_sym == sym)
                    seen = 1;
            if (seen)
                continue;
            ob_fmt(b, "    -> %s", u->syms[sym].name);
            if (in->op == IR_FADDR)
                ob_str(b, "   (address taken, not called here)");
            else if (!u->syms[sym].defined)
                ob_str(b, "   (not defined in this unit)");
            if (in->line)
                ob_fmt(b, "\t; line %d", in->line);
            ob_str(b, "\n");
            any = 1;
        }
        if (!any)
            ob_str(b, "    (calls nothing)\n");
    }
    /* A unit's leaves are what a worst-case stack analysis starts from
     * (§20.2), so they are worth naming even before that exists. */
    int leaves = 0;
    for (int i = 0; i < u->nfuncs; i++) {
        int calls = 0;
        for (int n = 0; n < u->funcs[i].nins; n++)
            if (u->funcs[i].ins[n].op == IR_CALL)
                calls = 1;
        if (!calls)
            leaves++;
    }
    ob_fmt(b, "\n; %d function%s, %d of them leaves\n", u->nfuncs,
           u->nfuncs == 1 ? "" : "s", leaves);
}
