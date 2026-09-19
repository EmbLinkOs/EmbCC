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

/* A conjunction's right operand is checked only when its left one is
 * satisfied, a disjunction's only when its left one is not (13.5.2.2):
 * checking it could instantiate what depends on the answer (a class
 * being defined, the one whose partial specialization is being chosen) */
static int eval_and(int end)
{
    int v = eval_primary(end);
    while (cx_pos < end && cx_kind() == TOK_ANDAND) {
        cx_advance();
        if (v)
            v = eval_primary(end);
        else
            skip_atom(end);
    }
    return v;
}

static int eval_or(int end)
{
    int v = eval_and(end);
    while (cx_pos < end && cx_kind() == TOK_OROR) {
        cx_advance();
        if (!v) {
            v = eval_and(end);
            continue;
        }
        skip_atom(end);
        while (cx_pos < end && cx_kind() == TOK_ANDAND) {
            cx_advance();
            skip_atom(end);
        }
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

/* ---- subsumption (13.5.4, 13.5.5) ---- */

/* A constraint in normal form: atomic constraints joined by conjunction
 * and disjunction, a concept-id replaced by its definition. An atom is
 * its expression's tokens with the arguments of the concept it appears
 * in (NULL outside any): two are the same only when both are. */
struct cnorm {
    int k;                    /* 0 an atom, 1 and, 2 or */
    struct cnorm *l, *r;
    int tok;
    struct ctarg *map;
    int nmap;
};

static struct cnorm *cn_new(int k, struct cnorm *l, struct cnorm *r)
{
    if (k && !l)
        return r;             /* (a missing side: none of it) */
    if (k && !r)
        return l;
    struct cnorm *n = xcalloc(1, sizeof *n);
    n->k = k;
    n->l = l;
    n->r = r;
    return n;
}

static struct cnorm *norm_or(int end, struct ctarg *map, int nmap, int depth);

/* concept c for args: its definition, normalized with them bound */
static struct cnorm *norm_concept(struct ctemplate *c, struct ctarg *args,
                                  int n, int depth)
{
    if (depth > 64)
        return NULL;
    struct ctarg *fitted;
    struct parse_state *st = parse_save();
    cx_scope = concept_bind(c, args, n, &fitted, cx_cur());
    cx_pos = c->req_start;
    cx_half_gt = 0;
    struct cnorm *r = norm_or(c->req_end, fitted, c->nparams, depth + 1);
    parse_restore(st);
    return r;
}

static struct cnorm *norm_primary(int end, struct ctarg *map, int nmap,
                                  int depth)
{
    if (cx_kind() == TOK_LPAREN) {
        int open = cx_pos;
        cx_skip_balanced();
        int close = cx_pos - 1;
        if (!is_fold(open, close) &&
            (cx_pos >= end || cx_kind() == TOK_ANDAND ||
             cx_kind() == TOK_OROR)) {
            cx_pos = open + 1;
            struct cnorm *v = norm_or(close, map, nmap, depth);
            if (cx_pos == close) {
                cx_pos = close + 1;
                return v;
            }
        }
        cx_pos = open;
    }
    int ntok, args_tok;
    struct ctemplate *c = concept_at(&ntok, &args_tok);
    if (c) {
        enum tok_kind after = cx_pos + ntok < end
                              ? cx_toks[cx_pos + ntok].t.kind : TOK_EOF;
        if (cx_pos + ntok >= end || after == TOK_ANDAND ||
            after == TOK_OROR) {
            /* a concept-id alone: its normal form, its arguments bound */
            struct ctarg *args = NULL;
            int n = 0, next = cx_pos + ntok;
            jmp_buf jb;
            void *saved = cx_sfinae;
            struct parse_state *st = parse_save();
            if (setjmp(jb)) {
                parse_restore(st);
                cx_sfinae = saved;
                c = NULL;             /* (an atom, then) */
            } else {
                cx_sfinae = &jb;
                if (args_tok >= 0) {
                    cx_pos = args_tok;
                    n = parse_template_args(c, &args);
                }
                struct cnorm *r = norm_concept(c, args, n, depth);
                cx_sfinae = saved;
                parse_restore(st);
                cx_pos = next;
                cx_half_gt = 0;
                return r;
            }
        }
    }
    struct cnorm *a = cn_new(0, NULL, NULL);
    a->tok = cx_pos;
    a->map = map;
    a->nmap = nmap;
    skip_atom(end);
    return a;
}

static struct cnorm *norm_and(int end, struct ctarg *map, int nmap,
                              int depth)
{
    struct cnorm *v = norm_primary(end, map, nmap, depth);
    while (cx_pos < end && cx_kind() == TOK_ANDAND) {
        cx_advance();
        v = cn_new(1, v, norm_primary(end, map, nmap, depth));
    }
    return v;
}

static struct cnorm *norm_or(int end, struct ctarg *map, int nmap, int depth)
{
    struct cnorm *v = norm_and(end, map, nmap, depth);
    while (cx_pos < end && cx_kind() == TOK_OROR) {
        cx_advance();
        v = cn_new(2, v, norm_and(end, map, nmap, depth));
    }
    return v;
}

struct cnorm *constraints_normal(struct ctparam *ps, int np, int req_start,
                                 int req_end, struct cscope *scope)
{
    return constraints_normal2(ps, np, req_start, req_end, 0, 0, scope);
}

struct cnorm *constraints_normal2(struct ctparam *ps, int np, int req_start,
                                  int req_end, int treq, int treq_end,
                                  struct cscope *scope)
{
    struct cnorm *r = NULL;
    struct parse_state *st = parse_save();
    cx_scope = scope;
    cx_pattern = 0;
    cx_in_targs = 0;
    for (int i = 0; i < np; i++) {
        struct ctparam *p = &ps[i];
        if (!p->tc || !p->name)
            continue;
        struct csym *y = lookup_in(scope, p->name);
        if (!y || y->k != CS_TYPEDEF)
            continue;             /* (a pack's: left out) */
        /* C<A...> T: C<T, A...> */
        struct ctarg *given = NULL;
        int ng = 0;
        if (p->tc_args >= 0) {
            cx_pos = p->tc_args;
            cx_half_gt = 0;
            ng = parse_template_args(NULL, &given);
        }
        struct ctarg *args = xcalloc((size_t)ng + 1, sizeof *args);
        args[0].kind = TP_TYPE;
        args[0].type = y->type;
        for (int k = 0; k < ng; k++)
            args[k + 1] = given[k];
        r = cn_new(1, r, norm_concept(p->tc, args, ng + 1, 0));
    }
    if (req_start) {
        cx_pos = req_start;
        cx_half_gt = 0;
        r = cn_new(1, r, norm_or(req_end, NULL, 0, 0));
    }
    if (treq) {
        cx_pos = treq;
        cx_half_gt = 0;
        r = cn_new(1, r, norm_or(treq_end, NULL, 0, 0));
    }
    parse_restore(st);
    return r;
}

/* clauses: each a list of atoms */
struct clauses {
    struct cnorm ***c;
    int *len;
    int n;
    int over;                 /* too many to decide */
};

static void cl_add(struct clauses *s, struct cnorm **atoms, int len)
{
    if (s->n >= 512) {
        s->over = 1;
        return;
    }
    s->c = xrealloc(s->c, (size_t)(s->n + 1) * sizeof *s->c);
    s->len = xrealloc(s->len, (size_t)(s->n + 1) * sizeof *s->len);
    s->c[s->n] = atoms;
    s->len[s->n] = len;
    s->n++;
}

/* n's clauses: disjunctive (dnf: an or of ands) or conjunctive normal
 * form — the operator that distributes is the other one */
static struct clauses clauses_of(struct cnorm *n, int dnf)
{
    struct clauses r = { NULL, NULL, 0, 0 };
    if (!n)
        return r;
    if (n->k == 0) {
        struct cnorm **one = xmalloc(sizeof *one);
        one[0] = n;
        cl_add(&r, one, 1);
        return r;
    }
    struct clauses a = clauses_of(n->l, dnf), b = clauses_of(n->r, dnf);
    r.over = a.over || b.over;
    if ((n->k == 2) == dnf) {
        /* the clause joiner: the clauses of both */
        for (int i = 0; i < a.n; i++)
            cl_add(&r, a.c[i], a.len[i]);
        for (int i = 0; i < b.n; i++)
            cl_add(&r, b.c[i], b.len[i]);
        return r;
    }
    /* the other: every pairing, merged */
    for (int i = 0; i < a.n && !r.over; i++)
        for (int j = 0; j < b.n && !r.over; j++) {
            int len = a.len[i] + b.len[j];
            struct cnorm **m = xmalloc((size_t)len * sizeof *m);
            memcpy(m, a.c[i], (size_t)a.len[i] * sizeof *m);
            memcpy(m + a.len[i], b.c[j], (size_t)b.len[j] * sizeof *m);
            cl_add(&r, m, len);
        }
    return r;
}

static int atom_same(const struct cnorm *a, const struct cnorm *b)
{
    if (a->tok != b->tok)
        return 0;
    if (!a->map || !b->map)
        return !a->map && !b->map;
    return targs_same(a->map, a->nmap, b->map, b->nmap);
}

int constraint_subsumes(struct cnorm *p, struct cnorm *q)
{
    if (!q)
        return 1;             /* everything subsumes no constraint */
    if (!p)
        return 0;
    struct clauses dp = clauses_of(p, 1), cq = clauses_of(q, 0);
    if (dp.over || cq.over)
        return 0;
    for (int i = 0; i < dp.n; i++)
        for (int j = 0; j < cq.n; j++) {
            int found = 0;
            for (int x = 0; x < dp.len[i] && !found; x++)
                for (int y = 0; y < cq.len[j] && !found; y++)
                    found = atom_same(dp.c[i][x], cq.c[j][y]);
            if (!found)
                return 0;
        }
    return 1;
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
