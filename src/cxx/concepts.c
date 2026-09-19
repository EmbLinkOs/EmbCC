/* Constraints (C++20, 13.5): requires-clauses, requires-expressions and
 * concepts — decided the way everything template-related is here, by
 * replaying tokens with the parameters bound.
 *
 * A requires-clause is kept as its token range; whether it is satisfied
 * for some arguments is read from those tokens in the scope binding them:
 * its operands (atomic constraints) joined by && and ||, each an
 * expression that must be a constant true — an operand whose
 * substitution fails is simply not satisfied (13.5.2.3), which is why the
 * operands are read one by one, each with errors caught, rather than as
 * one expression. A requires-expression is true when each of its
 * requirements is well-formed, tried in order the same way. A concept is
 * a named constraint, its value for arguments cached. */
#include "cxx.h"

#include <setjmp.h>
#include <string.h>

#include "../driver/util.h"

/* ---- skipping (when a template is declared) ---- */

/* a requires-clause's operand: a primary expression */
static void skip_primary(void)
{
    while (cx_kind() == TOK_BANG)
        cx_advance();
    switch (cx_kind()) {
    case TOK_LPAREN:
        cx_skip_balanced();
        return;
    case TOK_CX_REQUIRES:
        cx_advance();
        if (cx_kind() == TOK_LPAREN)
            cx_skip_balanced();
        if (cx_kind() != TOK_LBRACE)
            cx_error(cx_cur(), "expected '{' in a requires-expression");
        cx_skip_balanced();
        return;
    case TOK_NUM: case TOK_CX_TRUE: case TOK_CX_FALSE:
        cx_advance();
        return;
    default:
        break;
    }
    /* an id-expression (C<T>, std::is_same_v<A, B>, X<T>::value), or a
     * trait intrinsic's call (__is_enum(T)) */
    if (cx_kind() == TOK_COLONCOLON)
        cx_advance();
    for (;;) {
        if (cx_kind() == TOK_CX_TEMPLATE)
            cx_advance();
        if (cx_kind() != TOK_IDENT)
            cx_error(cx_cur(), "expected a constraint before %s",
                     tok_describe(&cx_cur()->t));
        cx_advance();
        if (cx_kind() == TOK_LT)
            skip_template_args();
        else if (cx_kind() == TOK_LPAREN)
            cx_skip_balanced();
        if (cx_kind() == TOK_COLONCOLON) {
            cx_advance();
            continue;
        }
        return;
    }
}

void skip_constraint(int general)
{
    if (general) {
        /* a concept's definition: any expression, to its `;` */
        while (cx_kind() != TOK_SEMI) {
            if (cx_kind() == TOK_EOF || cx_kind() == TOK_RBRACE)
                cx_error(cx_cur(), "expected ';' after a concept");
            if (cx_kind() == TOK_LPAREN || cx_kind() == TOK_LBRACKET ||
                cx_kind() == TOK_LBRACE)
                cx_skip_balanced();
            else
                cx_advance();
        }
        return;
    }
    skip_primary();
    while (cx_kind() == TOK_ANDAND || cx_kind() == TOK_OROR) {
        cx_advance();
        skip_primary();
    }
}

/* ---- satisfaction ---- */

static int eval_or(int end);

/* past an operand that could not be read: to the next && or || outside
 * brackets (a `<` after a name counted as one), or the end */
static void skip_atom(int end)
{
    int depth = 0, angle = 0;
    while (cx_pos < end) {
        enum tok_kind k = cx_toks[cx_pos].t.kind;
        if (!depth && !angle && (k == TOK_ANDAND || k == TOK_OROR))
            return;
        if (k == TOK_LPAREN || k == TOK_LBRACKET || k == TOK_LBRACE)
            depth++;
        else if (k == TOK_RPAREN || k == TOK_RBRACKET || k == TOK_RBRACE)
            depth--;
        else if (k == TOK_LT && cx_pos > 0 &&
                 cx_toks[cx_pos - 1].t.kind == TOK_IDENT)
            angle++;
        else if (k == TOK_GT && angle)
            angle--;
        else if (k == TOK_SHR && angle)
            angle = angle >= 2 ? angle - 2 : 0;
        cx_pos++;
    }
    cx_half_gt = 0;
}

/* an atomic constraint: a constant expression of type bool */
static int eval_atom(int end)
{
    int start = cx_pos;
    jmp_buf jb;
    void *saved = cx_sfinae;
    struct parse_state *st = parse_save();
    if (setjmp(jb)) {
        parse_restore(st);
        cx_sfinae = saved;
        cx_pos = start;
        skip_atom(end);
        return 0;
    }
    cx_sfinae = &jb;
    struct cexpr *e = expr_parse_binary(3);     /* stops at && and || */
    long v = 0;
    int ok = expr_const(e, &v);
    cx_sfinae = saved;
    return ok && v;
}

/* Is the parenthesized group [open, close] a fold-expression (a `...`
 * directly inside)? */
static int is_fold(int open, int close)
{
    int depth = 0;
    for (int i = open + 1; i < close; i++) {
        enum tok_kind k = cx_toks[i].t.kind;
        if (k == TOK_LPAREN || k == TOK_LBRACKET || k == TOK_LBRACE)
            depth++;
        else if (k == TOK_RPAREN || k == TOK_RBRACKET || k == TOK_RBRACE)
            depth--;
        else if (!depth && k == TOK_ELLIPSIS)
            return 1;
    }
    return 0;
}

static int eval_primary(int end)
{
    if (cx_kind() == TOK_LPAREN) {
        /* ( constraint ): its && and || count too — unless the
         * parentheses are part of a larger expression, or a fold (one
         * atomic constraint, 13.5.2.5) */
        int open = cx_pos;
        cx_skip_balanced();
        int close = cx_pos - 1;
        if (!is_fold(open, close) &&
            (cx_pos >= end || cx_kind() == TOK_ANDAND ||
             cx_kind() == TOK_OROR)) {
            cx_pos = open + 1;
            int v = eval_or(close);
            if (cx_pos == close) {
                cx_pos = close + 1;
                return v;
            }
        }
        cx_pos = open;
    }
    return eval_atom(end);
}

static int eval_and(int end)
{
    int v = eval_primary(end);
    while (cx_pos < end && cx_kind() == TOK_ANDAND) {
        cx_advance();
        int w = eval_primary(end);
        v = v && w;
    }
    return v;
}

static int eval_or(int end)
{
    int v = eval_and(end);
    while (cx_pos < end && cx_kind() == TOK_OROR) {
        cx_advance();
        int w = eval_and(end);
        v = v || w;
    }
    return v;
}

int constraint_satisfied(int start, int end, struct cscope *scope)
{
    struct parse_state *st = parse_save();
    cx_scope = scope;
    cx_pos = start;
    cx_half_gt = 0;
    cx_pattern = 0;
    cx_in_targs = 0;
    int v = eval_or(end);
    parse_restore(st);
    return v;
}

/* ---- concepts named where a type is ---- */

struct ctemplate *concept_at(int *ntok, int *args_tok)
{
    int save = cx_pos, save_half = cx_half_gt;
    struct qname q = peek_qname();
    struct ctemplate *c = NULL;
    if (!q.bad && !q.dep && cx_kind_at(q.fin) == TOK_IDENT) {
        cx_pos += q.fin;
        const char *n = cx_cur()->t.text;
        struct csym *y = q.scope ? lookup_in(q.scope, n) : lookup(cx_scope, n);
        if (y && y->k == CS_TEMPLATE && y->tmpl->kind == TK_CONCEPT) {
            c = y->tmpl;
            cx_advance();
            *args_tok = -1;
            if (cx_kind() == TOK_LT) {
                *args_tok = cx_pos;
                skip_template_args();
            }
            *ntok = cx_pos - save;
        }
    }
    cx_pos = save;
    cx_half_gt = save_half;
    return c;
}

/* type-constraint C<A...> on the type t: C<t, A...> (the arguments at
 * args_tok, read in the current scope) */
int type_constraint_holds(struct ctemplate *c, int args_tok, struct cty *t,
                          const struct ctok *at)
{
    struct ctarg *given = NULL;
    int ng = 0;
    if (args_tok >= 0) {
        int save = cx_pos, save_half = cx_half_gt;
        cx_pos = args_tok;
        cx_half_gt = 0;
        ng = parse_template_args(NULL, &given);
        cx_pos = save;
        cx_half_gt = save_half;
    }
    struct ctarg *args = xcalloc((size_t)ng + 1, sizeof *args);
    args[0].kind = TP_TYPE;
    args[0].type = t;
    for (int i = 0; i < ng; i++)
        args[i + 1] = given[i];
    return concept_satisfied(c, args, ng + 1, at);
}

/* ---- requires-expressions ---- */

/* past a requirement (to its `;`) */
static void skip_requirement(void)
{
    while (cx_kind() != TOK_SEMI) {
        if (cx_kind() == TOK_EOF || cx_kind() == TOK_RBRACE)
            cx_error(cx_cur(), "expected ';' after a requirement");
        if (cx_kind() == TOK_LPAREN || cx_kind() == TOK_LBRACKET ||
            cx_kind() == TOK_LBRACE)
            cx_skip_balanced();
        else
            cx_advance();
    }
    cx_advance();
}

/* one requirement: is it met? (the cursor after its `;` either way) */
static int requirement(void)
{
    int start = cx_pos;
    jmp_buf jb;
    void *saved = cx_sfinae;
    struct parse_state *st = parse_save();
    if (setjmp(jb)) {
        parse_restore(st);
        cx_sfinae = saved;
        cx_pos = start;
        skip_requirement();
        return 0;
    }
    cx_sfinae = &jb;
    int ok = 1;
    if (cx_kind() == TOK_CX_TYPENAME) {
        /* typename T::type; : the type exists */
        parse_type_id();
    } else if (cx_kind() == TOK_CX_REQUIRES) {
        /* requires C<T>; : a nested constraint */
        cx_advance();
        int b = cx_pos;
        skip_constraint(1);
        int e = cx_pos;
        cx_sfinae = saved;
        ok = constraint_satisfied(b, e, cx_scope);
        cx_sfinae = &jb;
    } else if (cx_kind() == TOK_LBRACE) {
        /* { e } noexcept -> C<A...>; */
        cx_advance();
        struct cexpr *e = expr_parse();
        cx_expect(TOK_RBRACE, "'}' in a compound requirement");
        if (cx_accept(TOK_CX_NOEXCEPT) && !cx_expr_nothrow(e))
            ok = 0;
        if (cx_accept(TOK_ARROW)) {
            int n, args_tok;
            const struct ctok *at = cx_cur();
            struct ctemplate *c = concept_at(&n, &args_tok);
            if (!c)
                cx_error(at, "expected a type-constraint after '->'");
            /* decltype((e)): its type, a reference by its category */
            struct cty *t = e->vc == VC_LVALUE ? ct_ref(e->t, 0)
                            : e->vc == VC_XVALUE ? ct_ref(e->t, 1) : e->t;
            cx_sfinae = saved;
            if (ok && !type_constraint_holds(c, args_tok, t, at))
                ok = 0;
            cx_sfinae = &jb;
            cx_pos += n;
        }
    } else {
        /* e; : well-formed */
        expr_parse();
    }
    cx_expect(TOK_SEMI, "';' after a requirement");
    cx_sfinae = saved;
    return ok;
}

struct cexpr *parse_requires_expr(void)
{
    const struct ctok *at = cx_cur();
    cx_expect(TOK_CX_REQUIRES, "'requires'");
    if (cx_pattern) {
        /* in a pattern: known only for arguments (a dependent value) */
        if (cx_kind() == TOK_LPAREN)
            cx_skip_balanced();
        if (cx_kind() != TOK_LBRACE)
            cx_error(at, "expected '{' in a requires-expression");
        cx_skip_balanced();
        return ex_int(0, ct_basic(CT_BOOL));
    }
    scope_push(SC_BLOCK, NULL);
    if (cx_kind() == TOK_LPAREN) {
        /* its parameters: names for the requirements' operands */
        struct cty *ft = parse_param_list();
        for (int i = 0; i < ft->np; i++) {
            if (!ft->pnames || !ft->pnames[i])
                continue;
            struct cvar *v = xcalloc(1, sizeof *v);
            v->name = v->cname = ft->pnames[i];
            v->type = ft->pdecl ? ft->pdecl[i] : ft->params[i];
            v->is_param = 1;
            struct csym *y = scope_add(cx_scope, CS_VAR, v->name);
            y->var = v;
        }
    }
    cx_expect(TOK_LBRACE, "'{' in a requires-expression");
    cx_unevaluated++;
    int all = 1;
    while (cx_kind() != TOK_RBRACE) {
        if (cx_kind() == TOK_EOF)
            cx_error(at, "unterminated requires-expression");
        if (!all) {                 /* after a failure: not substituted */
            skip_requirement();
            continue;
        }
        all = requirement();
    }
    cx_advance();
    cx_unevaluated--;
    scope_pop();
    return ex_int(all, ct_basic(CT_BOOL));
}
