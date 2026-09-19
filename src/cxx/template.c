/* Templates (D-013, CX4): instantiation by replaying tokens.
 *
 * A template's declaration is kept as its tokens. An instance — a class
 * template's class for some arguments, a function template's
 * specialization — is made by parsing those tokens again in a scope that
 * binds each template parameter to its argument (a typedef, a constant, a
 * template, a pack), so every instance is ordinary C++ to the rest of the
 * front-end. A class instance is made when first named and defined when
 * first needed complete (class_ensure); a function template's
 * specialization has its declaration instantiated when overload resolution
 * considers it — where a substitution failure only removes the candidate
 * (SFINAE: errors unwind to the attempt) — and its body when used.
 *
 * A function template's declaration is also read once as a pattern, its
 * template parameters as placeholder types (CT_TPARAM, CT_TID, CT_DEP):
 * what argument deduction matches call arguments against, and what the
 * Itanium mangling of a specialization is spelled from. */
#include "cxx.h"

#include <setjmp.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "../driver/util.h"

int cx_pattern;
int cx_unevaluated;
int cx_in_targs;

/* ---- packs ---- */

struct pexp {
    struct csym *pack;
    int index;
    struct csym *elem;        /* the element as a symbol, made on demand */
};
static struct pexp pexps[64];
static int npexps;

void pack_push(struct csym *pack, int index)
{
    if (npexps == 64)
        cx_error(cx_cur(), "pack expansions nested too deeply");
    pexps[npexps].pack = pack;
    pexps[npexps].index = index;
    pexps[npexps].elem = NULL;
    npexps++;
}

void pack_pop(int n)
{
    npexps -= n;
}

int pack_mark(void)
{
    return npexps;
}

void pack_reset(int mark)
{
    npexps = mark;
}

int pack_length(struct csym *y)
{
    return y->npack;
}

struct csym *pack_current(struct csym *y)
{
    if (!y || y->k != CS_PACK)
        return y;
    for (int i = npexps - 1; i >= 0; i--) {
        struct pexp *e = &pexps[i];
        if (e->pack != y)
            continue;
        if (e->elem)
            return e->elem;
        struct csym *el = xcalloc(1, sizeof *el);
        el->name = y->name;
        el->scope = y->scope;
        if (y->pvars) {
            el->k = CS_VAR;
            el->var = y->pvars[e->index];
        } else {
            struct ctarg *a = &y->pack[e->index];
            if (a->kind == TP_TYPE) {
                el->k = CS_TYPEDEF;
                el->type = a->type;
            } else if (a->kind == TP_VALUE) {
                el->k = CS_ENUMERATOR;
                el->type = a->vtype ? a->vtype : ct_basic(CT_INT);
                el->value = a->value;
            } else {
                el->k = CS_TEMPLATE;
                el->tmpl = a->tmpl;
            }
        }
        e->elem = el;
        return el;
    }
    return y;
}

int packs_in(int from, int to, struct csym **packs, int max);

/* Does the `<` at token i open template arguments? After a template's
 * name, a qualified name (`std::forward<`), `template` or a cast. */
static int opens_targs(int i)
{
    if (i == 0)
        return 0;
    const struct ctok *p = &cx_toks[i - 1];
    switch (p->t.kind) {
    case TOK_CX_STATIC_CAST: case TOK_CX_DYNAMIC_CAST:
    case TOK_CX_CONST_CAST: case TOK_CX_REINTERPRET_CAST:
        return 1;
    case TOK_IDENT:
        break;
    default:
        return 0;
    }
    if (i >= 2 && (cx_toks[i - 2].t.kind == TOK_COLONCOLON ||
                   cx_toks[i - 2].t.kind == TOK_CX_TEMPLATE))
        return 1;
    struct csym *y = lookup_raw(cx_scope, p->t.text);
    if (!y)
        return 0;
    if (y->k == CS_TEMPLATE || (y->k == CS_PACK && y->value == TP_TEMPLATE))
        return 1;
    return (y->k == CS_CLASS || y->k == CS_TYPEDEF) && y->type &&
           y->type->k == CT_CLASS && y->type->cls->tmpl;
}

/* At the start of a list element (a template argument if `angles`, else
 * an expression or initializer): if it is a pattern ending in `...`, the
 * packs it names (up to max) and the `...`'s position. 0: not an
 * expansion; -1: `...` naming no bound pack (in a pattern, kept). */
int expansion_at(int angles, struct csym **packs, int max, int *ellipsis)
{
    char stk[256];                /* ( [ { < as opened */
    int sp = 0, np = 0;
    int i;
    for (i = cx_pos; i < cx_ntoks; i++) {
        enum tok_kind k = cx_toks[i].t.kind;
        if (k == TOK_EOF)
            return 0;
        if (k == TOK_LPAREN || k == TOK_LBRACKET || k == TOK_LBRACE) {
            if (sp < 256)
                stk[sp] = (char)k;
            sp++;
            continue;
        }
        if (k == TOK_LT && ((angles && sp == 0) || opens_targs(i))) {
            if (sp < 256)
                stk[sp] = '<';
            sp++;
            continue;
        }
        if (k == TOK_RPAREN || k == TOK_RBRACKET || k == TOK_RBRACE) {
            if (sp == 0)
                break;
            while (sp > 0 && sp <= 256 && stk[sp - 1] == '<')
                sp--;             /* a `<` that was a comparison after all */
            if (sp == 0)
                break;
            sp--;
            continue;
        }
        if (k == TOK_GT || k == TOK_SHR) {
            int n = k == TOK_SHR ? 2 : 1, popped = 0;
            while (n > 0 && sp > 0 && sp <= 256 && stk[sp - 1] == '<') {
                sp--;
                n--;
                popped = 1;
            }
            if (n > 0 && sp == 0 && angles) {
                if (popped)
                    return 0;     /* `X<Ts...>>`: the element ends in `>` */
                break;            /* the list's own closer */
            }
            continue;
        }
        if (sp == 0 && (k == TOK_COMMA || k == TOK_SEMI))
            break;
    }
    if (i == cx_pos || cx_toks[i - 1].t.kind != TOK_ELLIPSIS)
        return 0;
    *ellipsis = i - 1;
    np = packs_in(cx_pos, i - 1, packs, max);
    return np ? np : -1;
}

static int is_closer(enum tok_kind k)
{
    return k == TOK_RPAREN || k == TOK_RBRACKET || k == TOK_RBRACE;
}

/* The bound packs a pattern (tokens from..to-1) expands, up to max; how
 * many. Names inside a nested expansion are that one's (the innermost
 * `...` expands every pack in its pattern, 13.7.4), and sizeof...(X)
 * names X without expanding it. */
int packs_in(int from, int to, struct csym **packs, int max)
{
    enum { MAXD = 128 };
    char kind[MAXD];
    int open[MAXD], elem[MAXD];
    int sp = 0, np = 0, fold = -1;
    char *skip = xcalloc((size_t)(to - from + 1), 1);
    for (int j = from; j < to; j++) {
        enum tok_kind k = cx_toks[j].t.kind;
        if (fold >= 0)
            skip[j - from] = 1;
        if (k == TOK_LPAREN || k == TOK_LBRACKET || k == TOK_LBRACE ||
            (k == TOK_LT && opens_targs(j))) {
            if (sp < MAXD) {
                kind[sp] = k == TOK_LT ? '<' : (char)k;
                open[sp] = j;
                elem[sp] = j + 1;
            }
            sp++;
            continue;
        }
        if (is_closer(k)) {
            while (sp > 0 && sp <= MAXD && kind[sp - 1] == '<')
                sp--;
            if (sp > 0)
                sp--;
            if (fold >= sp)
                fold = -1;
            continue;
        }
        if ((k == TOK_GT || k == TOK_SHR) && sp > 0 && sp <= MAXD &&
            kind[sp - 1] == '<') {
            sp--;
            if (k == TOK_SHR && sp > 0 && sp <= MAXD && kind[sp - 1] == '<')
                sp--;
            continue;
        }
        if (k == TOK_COMMA && sp > 0 && sp <= MAXD) {
            elem[sp - 1] = j + 1;
            continue;
        }
        if (k != TOK_ELLIPSIS)
            continue;
        if (j > from && cx_toks[j - 1].t.kind == TOK_KW_SIZEOF) {
            /* sizeof...( X ) */
            int e = j + 1;
            if (e < to && cx_toks[e].t.kind == TOK_LPAREN)
                for (; e < to && cx_toks[e].t.kind != TOK_RPAREN; e++)
                    skip[e - from] = 1;
            continue;
        }
        if (sp == 0 || sp > MAXD)
            continue;
        enum tok_kind next = j + 1 < cx_ntoks ? cx_toks[j + 1].t.kind
                                              : TOK_EOF;
        int list_end = next == TOK_COMMA || is_closer(next) ||
                       (kind[sp - 1] == '<' &&
                        (next == TOK_GT || next == TOK_SHR));
        int start = list_end ? elem[sp - 1] : open[sp - 1];
        for (int e = start; e <= j; e++)
            if (e >= from)
                skip[e - from] = 1;
        if (!list_end)
            fold = sp - 1;        /* a fold: its parentheses are its own */
    }
    for (int j = from; j < to; j++) {
        if (skip[j - from] || cx_toks[j].t.kind != TOK_IDENT)
            continue;
        if (j > from && (cx_toks[j - 1].t.kind == TOK_DOT ||
                         cx_toks[j - 1].t.kind == TOK_ARROW ||
                         cx_toks[j - 1].t.kind == TOK_COLONCOLON))
            continue;             /* a member's name */
        struct csym *y = lookup_raw(cx_scope, cx_toks[j].t.text);
        if (!y || y->k != CS_PACK)
            continue;
        int have = 0;
        for (int k = 0; k < np; k++)
            have |= packs[k] == y;
        if (!have && np < max)
            packs[np++] = y;
    }
    free(skip);
    return np;
}

/* The common length of the packs an expansion names. */
int expansion_length(struct csym **packs, int n)
{
    int len = pack_length(packs[0]);
    for (int k = 1; k < n; k++)
        if (pack_length(packs[k]) != len)
            cx_error(cx_cur(), "packs '%s' and '%s' expanded together have "
                               "different lengths (%d and %d)",
                     packs[0]->name, packs[k]->name, len,
                     pack_length(packs[k]));
    return len;
}

/* ---- arguments ---- */

static struct ctarg *args_copy(const struct ctarg *a, int n)
{
    struct ctarg *r = xcalloc((size_t)(n ? n : 1), sizeof *r);
    memcpy(r, a, (size_t)n * sizeof *r);
    return r;
}

static int args_same(const struct ctarg *a, int na, const struct ctarg *b,
                     int nb)
{
    if (na != nb)
        return 0;
    for (int i = 0; i < na; i++)
        if (!targ_same(&a[i], &b[i]))
            return 0;
    return 1;
}

/* Bind parameters to arguments in a new scope under `parent`: the scope
 * an instance's tokens are read in. Arguments beyond the parameters (none,
 * normally) are ignored; a pack parameter takes a pack argument. */
struct cscope *tparam_scope(struct ctparam *ps, int np, struct ctarg *args,
                            int na, struct cscope *parent)
{
    struct cscope *s = scope_new(SC_TEMPLATE, NULL, parent);
    for (int i = 0; i < np && i < na; i++) {
        struct ctparam *p = &ps[i];
        struct ctarg *a = &args[i];
        if (!p->name)
            continue;
        if (p->pack || a->is_pack) {
            struct csym *y = scope_add(s, CS_PACK, p->name);
            y->pack = a->elems;
            y->npack = a->nelems;
            y->value = p->kind;
            continue;
        }
        switch (p->kind) {
        case TP_TYPE: {
            struct csym *y = scope_add(s, CS_TYPEDEF, p->name);
            y->type = a->type;
            break;
        }
        case TP_VALUE: {
            struct csym *y = scope_add(s, CS_ENUMERATOR, p->name);
            y->type = a->vtype ? a->vtype : ct_basic(CT_INT);
            y->value = a->value;
            break;
        }
        default: {
            struct csym *y = scope_add(s, CS_TEMPLATE, p->name);
            y->tmpl = a->tmpl;
            break;
        }
        }
    }
    return s;
}

/* The argument a default template argument gives, read in a scope binding
 * the parameters before it. */
static struct ctarg default_arg(struct ctemplate *t, struct ctparam *ps,
                                int np, int i, struct ctarg *prev,
                                struct cscope *scope, const struct ctok *at)
{
    struct ctarg a;
    memset(&a, 0, sizeof a);
    struct ctparam *p = &ps[i];
    if (p->def_tok < 0)
        cx_error(at, "too few template arguments for '%s'", t->name);
    struct parse_state *st = parse_save();
    cx_scope = tparam_scope(ps, np, prev, i, scope);
    cx_pos = p->def_tok;
    cx_half_gt = 0;
    cx_in_targs = 1;
    a.kind = p->kind;
    if (p->kind == TP_TYPE) {
        a.type = parse_type_id();
    } else if (p->kind == TP_VALUE) {
        struct cexpr *e = expr_parse_cond();
        long v;
        if (!expr_const(e, &v))
            cx_error(at, "a default template argument of '%s' is not a "
                         "constant", t->name);
        a.value = v;
        a.vtype = ct_unqual(e->t);
        if (p->vtype && !ct_dependent(p->vtype) && ct_is_integer(p->vtype)) {
            /* converted to the parameter's type, as a given one is */
            a.vtype = ct_unqual(p->vtype);
            if (a.vtype->k == CT_BOOL)
                a.value = v != 0;
        }
    } else {
        struct qname q = peek_qname();
        cx_pos += q.fin;
        struct csym *y = q.scope ? lookup_in(q.scope, cx_cur()->t.text)
                                 : lookup(cx_scope, cx_cur()->t.text);
        if (!y || y->k != CS_TEMPLATE)
            cx_error(at, "a default template template argument is not a "
                         "template");
        a.tmpl = y->tmpl;
    }
    parse_restore(st);
    return a;
}

/* The arguments as written, fitted to the parameters: a trailing pack
 * parameter collects the rest, missing ones take their defaults. */
static struct ctarg *fit_args(struct ctemplate *t, struct ctparam *ps,
                              int np, struct ctarg *args, int na,
                              struct cscope *scope, const struct ctok *at)
{
    struct ctarg *r = xcalloc((size_t)(np ? np : 1), sizeof *r);
    int k = 0;
    for (int i = 0; i < np; i++) {
        struct ctparam *p = &ps[i];
        if (p->pack) {
            r[i].kind = p->kind;
            r[i].is_pack = 1;
            if (k < na && args[k].is_pack && !args[k].expansion &&
                na - k == 1) {
                r[i] = args[k++];
                continue;
            }
            r[i].nelems = na > k ? na - k : 0;
            r[i].elems = args_copy(args + k, r[i].nelems);
            k = na;
            continue;
        }
        if (k < na) {
            r[i] = args[k++];
            if (r[i].kind != p->kind && !(r[i].kind == TP_TYPE &&
                                           p->kind == TP_TEMPLATE))
                cx_error(at, "template argument %d of '%s' is a %s, not a "
                             "%s", i + 1, t->name,
                         r[i].kind == TP_TYPE ? "type" : "value",
                         p->kind == TP_TYPE ? "type" : "value");
            continue;
        }
        r[i] = default_arg(t, ps, np, i, r, scope, at);
    }
    if (k < na)
        cx_error(at, "too many template arguments for '%s'", t->name);
    return r;
}

/* ---- constraints ---- */

int concept_satisfied(struct ctemplate *c, struct ctarg *args, int n,
                      const struct ctok *at)
{
    struct ctarg *a = fit_args(c, c->params, c->nparams, args, n, c->scope,
                               at);
    for (struct cinst *in = c->insts; in; in = in->next)
        if (args_same(in->args, in->nargs, a, c->nparams)) {
            if (in->failed == 2)
                cx_error(at, "concept '%s' depends on itself", c->name);
            return !in->failed;
        }
    struct cinst *in = xcalloc(1, sizeof *in);
    in->args = a;
    in->nargs = c->nparams;
    in->failed = 2;                     /* being decided */
    in->next = c->insts;
    c->insts = in;
    struct cscope *s = tparam_scope(c->params, c->nparams, a, c->nparams,
                                    c->scope);
    cx_inst_push(cx_fmt("concept '%s'", c->name), at);
    int v = constraint_satisfied(c->req_start, c->req_end, s);
    cx_inst_pop();
    in->failed = !v;
    return v;
}

int template_constraints(struct ctparam *ps, int np, int req_start,
                         int req_end, struct cscope *scope,
                         const struct ctok *at)
{
    for (int i = 0; i < np; i++) {
        struct ctparam *p = &ps[i];
        if (!p->tc || !p->name)
            continue;
        struct csym *y = lookup_in(scope, p->name);
        if (!y)
            continue;
        struct cscope *saved = cx_scope;
        cx_scope = scope;
        int ok = 1;
        if (y->k == CS_PACK) {
            for (int k = 0; ok && k < y->npack; k++)
                ok = type_constraint_holds(p->tc, p->tc_args,
                                           y->pack[k].type, at);
        } else if (y->k == CS_TYPEDEF) {
            ok = type_constraint_holds(p->tc, p->tc_args, y->type, at);
        }
        cx_scope = saved;
        if (!ok)
            return 0;
    }
    return !req_start || constraint_satisfied(req_start, req_end, scope);
}

/* ---- deduction ---- */

static int deduce(struct cty *P, struct cty *A, struct ctarg *out,
                  int *set, int np);

/* Matching a partial specialization (or ordering two): the argument must
 * BE the pattern with the parameters substituted — not convert to it. */
static int deduce_exact;
/* ... a dependent part of the pattern was skipped: the match is known
 * only once the pattern is substituted */
static int deduce_deferred;
/* Ordering two templates (patterns against patterns): an alias whose type
 * does not depend on its arguments is that type (__void_t<...> is void) */
static int deduce_ordering;

static struct cty *alias_plain(struct cty *t)
{
    while (t->k == CT_DEP && t->tmpl && t->tmpl->kind == TK_ALIAS &&
           !t->tmpl->tparam) {
        struct cty *pat = alias_pattern(t->tmpl);
        if (!pat || ct_dependent(pat))
            break;
        t = t->q ? ct_qual(pat, pat->q | t->q) : pat;
    }
    return t;
}

/* Is A (a class) or one of its bases an instance of template tm? */
static struct cclass *instance_base(struct cclass *c, struct ctemplate *tm)
{
    if (c->tmpl == tm)
        return c;
    class_ensure(c);
    for (int i = 0; i < c->nbases; i++) {
        struct cclass *r = instance_base(c->bases[i].cls, tm);
        if (r)
            return r;
    }
    return NULL;
}

static int deduce_arg(struct ctarg *P, struct ctarg *A, struct ctarg *out,
                      int *set, int np);

/* Arguments with a trailing pack (not an expansion) spread out. */
static struct ctarg *flatten(struct ctarg *a, int *n)
{
    int k = *n;
    if (!k || !a[k - 1].is_pack || a[k - 1].expansion)
        return a;
    struct ctarg *last = &a[k - 1];
    struct ctarg *r = xcalloc((size_t)(k + last->nelems), sizeof *r);
    memcpy(r, a, (size_t)(k - 1) * sizeof *r);
    memcpy(r + k - 1, last->elems, (size_t)last->nelems * sizeof *r);
    *n = k - 1 + last->nelems;
    return r;
}

/* Argument patterns against arguments, in order; an expansion (`Ts...`)
 * takes all the rest, each deducing one element of the packs its pattern
 * names (13.10.3.6 p9). */
static int deduce_seq(struct ctarg *P, int nP, struct ctarg *A, int nA,
                      struct ctarg *out, int *set, int np)
{
    int k = 0;
    for (int i = 0; i < nP; i++) {
        if (!P[i].expansion) {
            if (k >= nA || !deduce_arg(&P[i], &A[k++], out, set, np))
                return 0;
            continue;
        }
        if (i != nP - 1)
            continue;                 /* not last: a non-deduced context */
        struct ctarg *pat = &P[i].elems[0];
        struct ctarg *o1 = xcalloc((size_t)(np ? np : 1), sizeof *o1);
        int *s1 = xcalloc((size_t)(np ? np : 1), sizeof *s1);
        int *mark = xcalloc((size_t)(np ? np : 1), sizeof *mark);
        for (; k < nA; k++) {
            memset(o1, 0, (size_t)np * sizeof *o1);
            memset(s1, 0, (size_t)np * sizeof *s1);
            /* against another pattern's expansion: pattern to pattern */
            struct ctarg *a = A[k].expansion ? &A[k].elems[0] : &A[k];
            if (!deduce_arg(pat, a, o1, s1, np))
                return 0;
            for (int j = 0; j < np; j++) {
                if (!s1[j])
                    continue;
                struct ctarg *pk = &out[j];
                if (!mark[j]) {
                    memset(pk, 0, sizeof *pk);
                    pk->kind = o1[j].kind;
                    pk->is_pack = 1;
                    mark[j] = 1;
                }
                pk->elems = xrealloc(pk->elems, (size_t)(pk->nelems + 1) *
                                                sizeof *pk->elems);
                pk->elems[pk->nelems++] = o1[j];
                set[j] = 1;
            }
        }
        return 1;
    }
    return k == nA;
}

static int deduce_arg(struct ctarg *P, struct ctarg *A, struct ctarg *out,
                      int *set, int np)
{
    if (P->expansion)
        return A->is_pack ? deduce_seq(P, 1, A->elems, A->nelems, out, set,
                                       np)
                          : deduce_seq(P, 1, A, 1, out, set, np);
    if (P->is_pack || A->is_pack) {
        if (!P->is_pack || !A->is_pack)
            return 0;
        return deduce_seq(P->elems, P->nelems, A->elems, A->nelems, out, set,
                          np);
    }
    if (P->kind == TP_TYPE && A->kind == TP_TYPE)
        return deduce(P->type, A->type, out, set, np);
    if (P->kind == TP_VALUE && A->kind == TP_VALUE) {
        if (P->vtype && P->vtype->k == CT_TPARAM) {
            int i = (int)P->vtype->n;           /* a value parameter */
            if (i < np) {
                if (set[i])
                    return out[i].value == A->value;
                out[i] = *A;
                set[i] = 1;
                return 1;
            }
        }
        if (P->mexpr || A->mexpr ||
            (A->vtype && A->vtype->k == CT_TPARAM))
            return 0;             /* dependent: not known to be equal */
        return P->value == A->value;
    }
    if (P->kind == TP_TEMPLATE && A->kind == TP_TEMPLATE)
        return P->tmpl == A->tmpl;
    return 0;
}

static struct cty *strip_elem_cv(struct cty *a, unsigned q)
{
    if (a->k != CT_ARRAY)
        return ct_qual(ct_unqual(a), a->q & ~q);
    return ct_array(strip_elem_cv(a->to, q), a->n);
}

/* The bases of c (c itself first, then outward) that are instances of
 * template tm */
static void instance_bases(struct cclass *c, struct ctemplate *tm,
                           struct cclass ***v, int *n, int *cap)
{
    if (c->tmpl == tm) {
        for (int i = 0; i < *n; i++)
            if ((*v)[i] == c)
                return;
        if (*n == *cap) {
            *cap = *cap ? *cap * 2 : 8;
            *v = xrealloc(*v, (size_t)*cap * sizeof **v);
        }
        (*v)[(*n)++] = c;
    }
    class_ensure(c);
    for (int i = 0; i < c->nbases; i++)
        instance_bases(c->bases[i].cls, tm, v, n, cap);
}

/* P = B<...> against a class deriving from instances of B: each tried
 * (13.10.3.2/4.3) — `_Tuple_impl<I, H, T...>&` from a tuple, whose bases
 * are _Tuple_impl<0, ...>, <1, ...>, ...: the one that deduces */
static int deduce_from_bases(struct cty *P, struct cclass *c,
                             struct ctarg *out, int *set, int np)
{
    struct cclass **v = NULL;
    int n = 0, cap = 0;
    instance_bases(c, P->tmpl, &v, &n, &cap);
    struct ctarg *o1 = xcalloc((size_t)(np ? np : 1), sizeof *o1);
    int *s1 = xcalloc((size_t)(np ? np : 1), sizeof *s1);
    int nP = P->ntargs;
    struct ctarg *pf = flatten(P->targs, &nP);
    for (int k = 0; k < n; k++) {
        memcpy(o1, out, (size_t)np * sizeof *o1);
        memcpy(s1, set, (size_t)np * sizeof *s1);
        int na = v[k]->ntargs;
        struct ctarg *flat = flatten(v[k]->targs, &na);
        if (deduce_seq(pf, nP, flat, na, o1, s1, np)) {
            memcpy(out, o1, (size_t)np * sizeof *out);
            memcpy(set, s1, (size_t)np * sizeof *set);
            return 1;
        }
    }
    return 0;
}

/* P an alias template-id with dependent arguments (index_sequence<I...>,
 * an alias being the type it names, 13.7.8): the alias's own parameters
 * deduced from A through its pattern (integer_sequence<size_t, I...>),
 * then P's arguments as written from those. Parameters the pattern leaves
 * undeduced (a non-deduced context, as in enable_if_t) deduce nothing. */
static int deduce_alias(struct cty *P, struct cty *A, struct ctarg *out,
                        int *set, int np)
{
    struct ctemplate *al = P->tmpl;
    struct cty *pat = alias_pattern(al);
    if (!pat)
        return 1;
    int anp = al->nparams;
    struct ctarg *aout = xcalloc((size_t)(anp ? anp : 1), sizeof *aout);
    int *aset = xcalloc((size_t)(anp ? anp : 1), sizeof *aset);
    if (!deduce(pat, A, aout, aset, anp))
        return 0;
    int nw = P->ntargs, k = 0;
    struct ctarg *w = flatten(P->targs, &nw);
    for (int i = 0; i < anp && k < nw; i++) {
        struct ctparam *p = &al->params[i];
        if (p->pack) {
            if (aset[i] && !deduce_seq(w + k, nw - k, aout[i].elems,
                                       aout[i].nelems, out, set, np))
                return 0;
            break;
        }
        if (w[k].expansion)
            break;            /* spread over parameters: not deduced */
        if (aset[i] && !deduce_arg(&w[k], &aout[i], out, set, np))
            return 0;
        k++;
    }
    return 1;
}

/* Deduce the template parameters P mentions from the argument type A
 * (13.10.3.6): out[i] for parameter i, set[i] once known. */
static int deduce(struct cty *P, struct cty *A, struct ctarg *out,
                  int *set, int np)
{
    if (deduce_ordering) {
        P = alias_plain(P);
        A = alias_plain(A);
    }
    if (deduce_exact && P->k != CT_TPARAM && P->q != A->q)
        return 0;
    switch (P->k) {
    case CT_TPARAM: {
        int i = (int)P->n;
        if (deduce_exact && (P->q & ~A->q) && A->k != CT_ARRAY)
            return 0;             /* const T does not match int */
        if (i >= np || set[i] >= 2)
            return 1;             /* given explicitly: the argument converts */
        /* const T against const int: T is int; against int, T is int
         * too (the parameter may be more qualified, 13.10.3.2) */
        struct cty *v = ct_qual(ct_unqual(A), A->q & ~P->q);
        if (A->k == CT_ARRAY && P->q)
            v = strip_elem_cv(A, P->q);     /* an array's cv is its
                                             * elements' */
        if (set[i])
            return out[i].kind == TP_TYPE && ct_same(out[i].type, v);
        out[i].kind = TP_TYPE;
        out[i].type = v;
        set[i] = 1;
        return 1;
    }
    case CT_PTR: case CT_LREF: case CT_RREF:
        if (A->k != P->k)
            return 0;
        return deduce(P->to, A->to, out, set, np);
    case CT_ARRAY:
        if (A->k != CT_ARRAY)
            return 0;
        if (P->n == -2 && P->bparam < np) {     /* T (&)[N] */
            int i = P->bparam;
            if (set[i])
                return out[i].value == A->n;
            out[i].kind = TP_VALUE;
            out[i].value = A->n;
            out[i].vtype = ct_size_t();
            set[i] = 1;
        } else if (P->n >= 0 && P->n != A->n) {
            return 0;
        }
        return deduce(P->to, A->to, out, set, np);
    case CT_FUNC: {
        int pexp = P->np && P->params[P->np - 1]->pack_expansion;
        if (A->k != CT_FUNC || (!pexp && A->np != P->np) ||
            (pexp && A->np < P->np - 1))
            return 0;
        if (deduce_exact && (A->variadic != P->variadic || A->fq != P->fq ||
                             A->refq != P->refq))
            return 0;
        if (!deduce(P->to, A->to, out, set, np))
            return 0;
        if (pexp) {
            /* R(A, B, Ts...): Ts from the parameters after the others */
            struct ctarg *pa = xcalloc((size_t)P->np, sizeof *pa);
            struct ctarg *aa = xcalloc((size_t)(A->np ? A->np : 1),
                                       sizeof *aa);
            for (int i = 0; i < P->np; i++) {
                pa[i].kind = TP_TYPE;
                pa[i].type = P->params[i];
            }
            struct ctarg *pat = xcalloc(1, sizeof *pat);
            pat->kind = TP_TYPE;
            pat->type = xmalloc(sizeof *pat->type);
            *pat->type = *P->params[P->np - 1];
            pat->type->pack_expansion = 0;
            pa[P->np - 1].type = NULL;
            pa[P->np - 1].expansion = 1;
            pa[P->np - 1].is_pack = 1;
            pa[P->np - 1].elems = pat;
            pa[P->np - 1].nelems = 1;
            for (int i = 0; i < A->np; i++) {
                aa[i].kind = TP_TYPE;
                aa[i].type = A->params[i];
            }
            return deduce_seq(pa, P->np, aa, A->np, out, set, np);
        }
        for (int i = 0; i < P->np; i++)
            if (!deduce(P->params[i], A->params[i], out, set, np))
                return 0;
        return 1;
    }
    case CT_MPTR:
        if (A->k != CT_MPTR)
            return 0;
        if (P->mclass) {
            /* T C::*: C deduced too */
            if (!deduce(P->mclass, A->mclass ? A->mclass : ct_class(A->cls),
                        out, set, np))
                return 0;
        } else if (deduce_exact && A->cls != P->cls) {
            return 0;
        }
        return deduce(P->to, A->to, out, set, np);
    case CT_TID: {
        if (A->k == CT_TID && A->tmpl == P->tmpl && !P->tmpl->tparam) {
            /* pattern against pattern (ordering) */
            int na = A->ntargs, nP = P->ntargs;
            struct ctarg *af = flatten(A->targs, &na);
            struct ctarg *pf = flatten(P->targs, &nP);
            return deduce_seq(pf, nP, af, na, out, set, np);
        }
        if (A->k != CT_CLASS)
            return 0;
        struct cclass *c;
        if (P->tmpl->tparam && P->tmpl->tparam <= np) {
            /* TT<...>: TT is A's template */
            int i = P->tmpl->tparam - 1;
            c = A->cls;
            if (!c->tmpl)
                return 0;
            if (set[i] && !(out[i].kind == TP_TEMPLATE &&
                            out[i].tmpl == c->tmpl))
                return 0;
            if (!set[i]) {
                out[i].kind = TP_TEMPLATE;
                out[i].tmpl = c->tmpl;
                set[i] = 1;
            }
        } else {
            c = instance_base(A->cls, P->tmpl);
            if (c && c != A->cls && !deduce_exact)
                return deduce_from_bases(P, A->cls, out, set, np);
        }
        if (!c)
            return 0;
        /* the class's arguments against the pattern's as written */
        int na = c->ntargs, nP = P->ntargs;
        struct ctarg *flat = flatten(c->targs, &na);
        struct ctarg *pf = flatten(P->targs, &nP);
        return deduce_seq(pf, nP, flat, na, out, set, np);
    }
    case CT_DEP:
        if (P->tmpl && A->k == CT_DEP && A->tmpl == P->tmpl &&
            P->tmpl->kind == TK_ALIAS) {
            /* the same alias, pattern against pattern (ordering) */
            int na = A->ntargs, nP = P->ntargs;
            struct ctarg *af = flatten(A->targs, &na);
            struct ctarg *pf = flatten(P->targs, &nP);
            return deduce_seq(pf, nP, af, na, out, set, np);
        }
        if (P->tmpl && P->tmpl->kind == TK_ALIAS && !P->tmpl->tparam &&
            alias_pattern(P->tmpl)) {
            /* matching a partial specialization, the pattern is still
             * substituted afterwards to confirm (index_sequence<I...>) */
            if (deduce_exact && !deduce_ordering)
                deduce_deferred = 1;
            return deduce_alias(P, A, out, set, np);
        }
        /* fall through */
    default:
        /* non-deduced: checked by conversion later — or, matching a
         * partial specialization, the same type; a dependent one
         * (typename T::x, an alias of T) once the rest is deduced, by
         * substituting it (deduce_deferred) */
        if (deduce_exact && P->k == CT_DEP) {
            deduce_deferred = 1;
            return 1;
        }
        return deduce_exact ? ct_same(P, A) : 1;
    }
}

/* Partial ordering (13.10.3.5): template a is at least as specialized as
 * b if b's parameters deduce from a's parameter types (a's own template
 * parameters standing as unique types), for the first n parameters. */
static int at_least_as_specialized(struct ctemplate *a, struct ctemplate *b,
                                   int n)
{
    struct cty *fa = a->pattern->type, *fb = b->pattern->type;
    int np = b->nparams;
    struct ctarg *out = xcalloc((size_t)(np > 0 ? np : 1), sizeof *out);
    int *set = xcalloc((size_t)(np > 0 ? np : 1), sizeof *set);
    for (int i = 0; i < n && i < fa->np && i < fb->np; i++) {
        struct cty *A = fa->pdecl ? fa->pdecl[i] : fa->params[i];
        struct cty *P = fb->pdecl ? fb->pdecl[i] : fb->params[i];
        /* a's parameter pack cannot stand for b's single parameter
         * (13.10.3.5/8): f(T&&) is more specialized than f(Ts&&...) */
        if ((fa->params[i]->pack_expansion || A->pack_expansion) &&
            !(fb->params[i]->pack_expansion || P->pack_expansion))
            return 0;
        int aref = A->k == CT_LREF ? 1 : A->k == CT_RREF ? 2 : 0;
        int pref = P->k == CT_LREF ? 1 : P->k == CT_RREF ? 2 : 0;
        if (ct_is_ref(A))
            A = A->to;
        if (ct_is_ref(P))
            P = P->to;
        /* exactly: a type the other template names concretely does not
         * take a's unique one; and consistently across the pairs (a
         * parameter deduced from two pairs must get one value: (T, U) is
         * not as specialized as (const V&, V)) */
        int saved = deduce_exact, saved_ord = deduce_ordering;
        deduce_exact = deduce_ordering = 1;
        int ok = deduce(ct_unqual(P), ct_unqual(A), out, set, np);
        deduce_exact = saved;
        deduce_ordering = saved_ord;
        if (!ok)
            return 0;
        /* T&& does not take the place of T& */
        if (aref == 2 && pref == 1)
            return 0;
    }
    return 1;
}

int more_specialized(struct ctemplate *a, struct ctemplate *b, int n)
{
    return at_least_as_specialized(a, b, n) &&
           !at_least_as_specialized(b, a, n);
}

/* A value parameter whose type is dependent (`typename enable_if<C,
 * bool>::type = true`): its type substituted with the arguments before
 * it — failing, deduction fails (13.10.3.1) — and its argument converted
 * to it. */
static int value_type_substitutes(struct ctemplate *t, int i,
                                  struct ctarg *out, int np)
{
    struct ctparam *p = &t->params[i];
    if (p->kind != TP_VALUE || p->pack || p->vtype_tok < 0 || !p->vtype ||
        !ct_dependent(p->vtype) || out[i].is_pack)
        return 1;
    jmp_buf jb;
    void *saved = cx_sfinae;
    struct parse_state *st = parse_save();
    if (setjmp(jb)) {
        parse_restore(st);
        cx_sfinae = saved;
        return 0;
    }
    cx_sfinae = &jb;
    cx_scope = tparam_scope(t->params, np, out, i, t->scope);
    cx_half_gt = 0;
    cx_in_targs = 1;
    struct cty *vt = parse_value_tparam_type(p->vtype_tok);
    cx_sfinae = saved;
    parse_restore(st);
    if (ct_is_integer(vt) && !ct_dependent(vt)) {
        out[i].vtype = ct_unqual(vt);
        if (out[i].vtype->k == CT_BOOL)
            out[i].value = out[i].value != 0;
    }
    return 1;
}

/* Template t's arguments deduced from a function type A (its address
 * taken where a pointer to A is wanted, 13.10.3.3): explicit ones first,
 * the rest from A's parameters and return type, then defaults. */
int deduce_func_type(struct ctemplate *t, struct ctarg *expl, int nexpl,
                     struct cty *A, struct ctarg **outp, int *nout)
{
    int np = t->nparams;
    struct ctarg *out = xcalloc((size_t)(np ? np : 1), sizeof *out);
    int *set = xcalloc((size_t)(np ? np : 1), sizeof *set);
    for (int i = 0, k = 0; k < nexpl; i++) {
        if (i >= np)
            return 0;
        if (t->params[i].pack) {
            out[i].kind = t->params[i].kind;
            out[i].is_pack = 1;
            out[i].nelems = nexpl - k;
            out[i].elems = args_copy(expl + k, nexpl - k);
            set[i] = 2;
            k = nexpl;
            break;
        }
        out[i] = expl[k++];
        set[i] = 3;
    }
    if (!deduce(t->pattern->type, A, out, set, np))
        return 0;
    for (int i = 0; i < np; i++) {
        if (set[i]) {
            if (!value_type_substitutes(t, i, out, np))
                return 0;
            continue;
        }
        if (t->params[i].pack) {
            out[i].kind = t->params[i].kind;
            out[i].is_pack = 1;
            continue;
        }
        if (t->params[i].def_tok < 0)
            return 0;
        jmp_buf jb;
        void *saved = cx_sfinae;
        struct parse_state *st = parse_save();
        if (setjmp(jb)) {
            parse_restore(st);
            cx_sfinae = saved;
            return 0;
        }
        cx_sfinae = &jb;
        out[i] = default_arg(t, t->params, np, i, out, t->scope, cx_cur());
        cx_sfinae = saved;
        parse_restore(st);
        if (!value_type_substitutes(t, i, out, np))
            return 0;
    }
    *outp = out;
    *nout = np;
    return 1;
}

int deduce_call(struct ctemplate *t, struct ctarg *expl, int nexpl,
                struct cexpr **args, int na, struct ctarg **outp, int *nout)
{
    int np = t->nparams;
    struct ctarg *out = xcalloc((size_t)(np ? np : 1), sizeof *out);
    int *set = xcalloc((size_t)(np ? np : 1), sizeof *set);
    /* explicit arguments first; a pack parameter takes the rest */
    for (int i = 0, k = 0; k < nexpl; i++) {
        if (i >= np)
            return 0;
        if (t->params[i].pack) {
            out[i].kind = t->params[i].kind;
            out[i].is_pack = 1;
            out[i].nelems = nexpl - k;
            out[i].elems = args_copy(expl + k, nexpl - k);
            set[i] = 2;                    /* may still grow: no */
            k = nexpl;
            break;
        }
        if (expl[k].kind != t->params[i].kind)
            return 0;
        out[i] = expl[k++];
        set[i] = 3;
    }
    struct cty *ft = t->pattern->type;
    /* packs a function parameter pack builds, and how far one deduced
     * earlier (from another parameter) has been matched */
    int *built = xcalloc((size_t)(np ? np : 1), sizeof *built);
    int *used = xcalloc((size_t)(np ? np : 1), sizeof *used);
    for (int i = 0; i < na; i++) {
        struct cty *P;
        int ppack = -1;
        if (i < ft->np) {
            P = ft->pdecl ? ft->pdecl[i] : ft->params[i];
            if (P->pack_expansion)
                ppack = i;
        } else if (ft->np && (ft->pdecl ? ft->pdecl[ft->np - 1]
                                        : ft->params[ft->np - 1])
                                 ->pack_expansion) {
            P = ft->pdecl ? ft->pdecl[ft->np - 1] : ft->params[ft->np - 1];
            ppack = ft->np - 1;
        } else if (ft->variadic) {
            continue;
        } else {
            return 0;
        }
        struct cexpr *e = args[i];
        if (e->k == E_INITLIST && !e->t) {
            /* std::initializer_list<P'> (or a reference to one): P'
             * deduced from each element; else non-deduced (13.10.3.2) */
            struct cty *Pl = ct_is_ref(P) ? P->to : P;
            if (Pl->k == CT_TID && is_std_il(Pl->tmpl) && Pl->ntargs == 1 &&
                Pl->targs[0].kind == TP_TYPE && ppack < 0)
                for (int k = 0; k < e->na; k++) {
                    struct cexpr *el = e->a[k];
                    if (el->k == E_INITLIST && !el->t)
                        continue;
                    struct cty *A = el->k == E_OVL ? el->fn->type : el->t;
                    if (!deduce(ct_unqual(Pl->targs[0].type),
                                ct_unqual(ct_decay(A)), out, set, np))
                        return 0;
                }
            continue;
        }
        if (e->k == E_OVL && !e->memptr) {
            if (e->fn->next || e->fn->tmpl)
                continue;                    /* an overload set: skip */
        }
        struct cty *A = e->k == E_OVL ? e->fn->type : e->t;
        if (ppack >= 0) {
            /* a function parameter pack: each argument deduces one
             * element of the template parameter packs its pattern names
             * (a pack given explicitly keeps its elements) */
            struct cty *Pe = xmalloc(sizeof *Pe);
            *Pe = *P;
            Pe->pack_expansion = 0;
            if (ct_is_ref(Pe)) {
                if (Pe->k == CT_RREF && Pe->to->k == CT_TPARAM &&
                    !Pe->to->q && e->vc == VC_LVALUE)
                    A = ct_ref(A, 0);
                Pe = Pe->to;
            } else {
                A = ct_unqual(ct_decay(A));
                Pe = ct_unqual(Pe);
            }
            struct ctarg *o1 = xcalloc((size_t)(np ? np : 1), sizeof *o1);
            int *s1 = xcalloc((size_t)(np ? np : 1), sizeof *s1);
            for (int j = 0; j < np; j++)
                s1[j] = set[j] >= 2 ? 3 : 0;
            if (!deduce(Pe, A, o1, s1, np))
                return 0;
            for (int j = 0; j < np; j++) {
                if (s1[j] != 1)
                    continue;
                struct ctarg *pk = &out[j];
                if (set[j] == 1 && !built[j] && pk->is_pack) {
                    /* deduced before: this argument must agree */
                    if (used[j] >= pk->nelems ||
                        !args_same(&pk->elems[used[j]], 1, &o1[j], 1))
                        return 0;
                    used[j]++;
                    continue;
                }
                built[j] = 1;
                if (!set[j]) {
                    memset(pk, 0, sizeof *pk);
                    pk->kind = o1[j].kind;
                    pk->is_pack = 1;
                }
                pk->elems = xrealloc(pk->elems, (size_t)(pk->nelems + 1) *
                                                sizeof *pk->elems);
                pk->elems[pk->nelems++] = o1[j];
                set[j] = 1;
            }
            continue;
        }
        if (ct_is_ref(P)) {
            /* T&& with an lvalue: T is an lvalue reference (forwarding) */
            if (P->k == CT_RREF && P->to->k == CT_TPARAM && !P->to->q &&
                e->vc == VC_LVALUE)
                A = ct_ref(A, 0);
            P = P->to;
        } else {
            A = ct_unqual(ct_decay(A));
            P = ct_unqual(P);
        }
        if (!deduce(P, A, out, set, np))
            return 0;
    }
    /* a deduced value takes its parameter's declared type (N from an
     * array bound is an int if declared int) */
    for (int i = 0; i < np; i++) {
        struct ctparam *p = &t->params[i];
        if (set[i] && p->kind == TP_VALUE && !out[i].is_pack && p->vtype &&
            !ct_dependent(p->vtype) && ct_is_integer(p->vtype))
            out[i].vtype = ct_unqual(p->vtype);
    }
    /* a pack deduced from nothing is empty; the rest default */
    for (int i = 0; i < np; i++) {
        if (set[i]) {
            if (!value_type_substitutes(t, i, out, np))
                return 0;
            continue;
        }
        if (t->params[i].pack) {
            out[i].kind = t->params[i].kind;
            out[i].is_pack = 1;
            set[i] = 1;
            continue;
        }
        if (t->params[i].def_tok < 0)
            return 0;
        /* a default that cannot be substituted: deduction fails (the
         * candidate is dropped), no error — as std::_RequireInputIter
         * counts on (13.10.3.1) */
        jmp_buf jb;
        void *saved = cx_sfinae;
        struct parse_state *st = parse_save();
        if (setjmp(jb)) {
            parse_restore(st);
            cx_sfinae = saved;
            return 0;
        }
        cx_sfinae = &jb;
        out[i] = default_arg(t, t->params, np, i, out, t->scope, cx_cur());
        cx_sfinae = saved;
        parse_restore(st);
        set[i] = 1;
        if (!value_type_substitutes(t, i, out, np))
            return 0;
    }
    *outp = out;
    *nout = np;
    return 1;
}

/* ---- instances ---- */

struct cclass *class_instance(struct ctemplate *t, struct ctarg *args,
                              int nargs, const struct ctok *at)
{
    if (t->kind != TK_CLASS)
        cx_error(at, "'%s' is not a class template", t->name);
    struct ctarg *a = fit_args(t, t->params, t->nparams, args, nargs,
                               t->scope, at);
    for (struct cinst *in = t->insts; in; in = in->next)
        if (args_same(in->args, in->nargs, a, t->nparams))
            return in->cls;
    struct cclass *c = class_new(t->name, t->scope);
    c->tmpl = t;
    c->targs = a;
    c->ntargs = t->nparams;
    c->is_struct = t->key != TOK_CX_CLASS;
    c->is_union = t->key == TOK_KW_UNION;
    if (targs_local(a, t->nparams)) {
        /* for a class with no linkage: none either */
        c->local = 1;
        c->cname = cx_fmt("__cx_%s_%d", t->name, cx_uid());
    }
    if (!c->local)
        c->cname = cx_fmt("_C%s", mangle_class_name(c));
    c->inst_pending = 1;
    struct cinst *in = xcalloc(1, sizeof *in);
    in->args = a;
    in->nargs = t->nparams;
    in->cls = c;
    in->next = t->insts;
    t->insts = in;
    return c;
}

/* Partial specialization p's pattern with its parameters bound: the
 * arguments a? (Read again from its tokens; a substitution failure is no
 * match.) */
static int pattern_substitutes(struct cpartial *p, struct ctarg *bound,
                               struct ctarg *a, int na, struct ctemplate *t)
{
    jmp_buf jb;
    void *saved = cx_sfinae;
    struct parse_state *st = parse_save();
    if (setjmp(jb)) {
        parse_restore(st);
        cx_sfinae = saved;
        return 0;
    }
    cx_sfinae = &jb;
    cx_scope = tparam_scope(p->params, p->nparams, bound, p->nparams,
                            t->scope);
    cx_pos = p->pat_tok;
    cx_half_gt = 0;
    cx_pattern = 0;
    cx_in_targs = 0;
    struct ctarg *sub;
    int ns = parse_template_args(t, &sub);
    cx_sfinae = saved;
    parse_restore(st);
    struct ctarg *full = fit_args(t, t->params, t->nparams, sub, ns,
                                  t->scope, cx_cur());
    int nf = t->nparams, nb = na;
    struct ctarg *ff = flatten(full, &nf);
    struct ctarg *af = flatten(a, &nb);
    return args_same(ff, nf, af, nb);
}

/* Does partial specialization p's pattern match arguments a (np of p's
 * parameters bound into out)? t: its template (NULL when ordering). */
static int partial_matches(struct cpartial *p, struct ctarg *a, int na,
                           struct ctarg **bound, struct ctemplate *t)
{
    int np = p->nparams;
    struct ctarg *out = xcalloc((size_t)(np ? np : 1), sizeof *out);
    int *set = xcalloc((size_t)(np ? np : 1), sizeof *set);
    int nP = p->npattern;
    struct ctarg *pf = flatten(p->pattern, &nP);
    struct ctarg *af = flatten(a, &na);
    int saved = deduce_exact, saved_def = deduce_deferred;
    deduce_exact = 1;
    deduce_deferred = 0;
    int ok = deduce_seq(pf, nP, af, na, out, set, np);
    int deferred = deduce_deferred;
    deduce_exact = saved;
    deduce_deferred = saved_def;
    for (int i = 0; ok && i < np; i++)
        ok = set[i] || p->params[i].pack;
    *bound = out;
    if (ok && deferred && p->pat_tok > 0 && t)
        ok = pattern_substitutes(p, out, a, na, t);
    return ok;
}

/* Is partial a at least as specialized as b: does b's pattern match a's
 * (a's parameters standing as unique types, 13.7.6.3)? */
static int partial_at_least(struct cpartial *a, struct cpartial *b)
{
    struct ctarg *bound;
    int saved = deduce_ordering;
    deduce_ordering = 1;
    int r = partial_matches(b, a->pattern, a->npattern, &bound, NULL);
    deduce_ordering = saved;
    return r;
}

/* Are partial specialization p's constraints satisfied, its parameters
 * bound (13.7.6.2)? */
static int partial_constraints(struct cpartial *p, struct ctarg *bound,
                               struct ctemplate *t)
{
    int any = p->req_start;
    for (int i = 0; i < p->nparams; i++)
        any |= p->params[i].tc != NULL;
    if (!any)
        return 1;
    struct cscope *s = tparam_scope(p->params, p->nparams, bound, p->nparams,
                                    t->scope);
    return template_constraints(p->params, p->nparams, p->req_start,
                                p->req_end, s, cx_cur());
}

/* Of two partial specializations whose patterns are alike, is a the more
 * constrained? (Constrained over unconstrained: subsumption as far as
 * EmbCC decides it.) */
static int more_constrained(struct cpartial *a, struct cpartial *b)
{
    int ca = a->req_start != 0, cb = b->req_start != 0;
    for (int i = 0; i < a->nparams; i++)
        ca |= a->params[i].tc != NULL;
    for (int i = 0; i < b->nparams; i++)
        cb |= b->params[i].tc != NULL;
    return ca && !cb;
}

/* The partial specialization whose pattern the arguments match — of
 * several, the one more specialized than each other (13.7.6.2). */
static struct cpartial *match_partial(struct ctemplate *t, struct ctarg *a,
                                      struct ctarg **bound)
{
    struct cpartial *m[64];
    struct ctarg *mb[64];
    int n = 0;
    for (struct cpartial *p = t->partials; p && n < 64; p = p->next)
        if (partial_matches(p, a, t->nparams, &mb[n], t) &&
            partial_constraints(p, mb[n], t))
            m[n++] = p;
    if (n == 0)
        return NULL;
    for (int i = 0; i < n; i++) {
        int best = 1;
        for (int j = 0; best && j < n; j++)
            best = i == j || (partial_at_least(m[i], m[j]) &&
                              (!partial_at_least(m[j], m[i]) ||
                               more_constrained(m[i], m[j])));
        if (best) {
            *bound = mb[i];
            return m[i];
        }
    }
    cx_error(cx_cur(), "ambiguous partial specializations of '%s' match "
                       "these arguments", t->name);
    return NULL;
}

static int inst_depth;

/* ---- the instantiation stack (an error's notes) ---- */

static struct { const char *what; const struct ctok *at; } insts[64];
static int ninsts;

void cx_inst_push(const char *what, const struct ctok *at)
{
    if (ninsts < 64) {
        insts[ninsts].what = what;
        insts[ninsts].at = at;
    }
    ninsts++;
}

void cx_inst_pop(void)
{
    if (ninsts > 0)
        ninsts--;
}

int cx_inst_mark(void)
{
    return ninsts;
}

void cx_inst_reset(int mark)
{
    ninsts = mark;
}

void cx_inst_notes(void)
{
    int shown = 0;
    for (int i = (ninsts < 64 ? ninsts : 64) - 1; i >= 0 && shown < 12;
         i--, shown++) {
        const struct ctok *at = insts[i].at;
        diag_note_at(at ? at->file : "<c++>", at ? at->t.line : 0,
                     at ? at->t.col : 0, "in the instantiation of %s",
                     insts[i].what);
    }
}

void class_ensure(struct cclass *c)
{
    if (!c->inst_pending) {
        if (!c->complete && !c->defining && c->lazy_pos) {
            /* a member class of an instance, needed now */
            int pos = c->lazy_pos;
            c->lazy_pos = 0;
            cx_inst_push(ct_name(ct_class(c)), cx_cur());
            class_define_from(c, pos, c->lazy_key, c->lazy_scope);
            cx_inst_pop();
            return;
        }
        if (!c->complete && !c->defining && c->name && c->owner &&
            c->owner->k == SC_CLASS)
            member_class_from_outdef(c);
        return;
    }
    if (c->inst_pending == 2)
        return;             /* choosing its partial specialization: its
                             * constraints asked for it (incomplete) */
    struct ctemplate *t = c->tmpl;
    struct ctarg *bound = NULL;
    c->inst_pending = 2;
    struct cpartial *p = match_partial(t, c->targs, &bound);
    c->inst_pending = 1;
    if (!p && t->head_end < 0)
        return;                        /* declared, not (yet) defined */
    c->inst_pending = 0;
    if (++inst_depth > 900)
        cx_error(cx_cur(), "template instantiation depth exceeds 900 (in "
                           "'%s')", t->name);
    struct cscope *ps = p ? tparam_scope(p->params, p->nparams, bound,
                                         p->nparams, t->scope)
                          : tparam_scope(t->params, t->nparams, c->targs,
                                         c->ntargs, t->scope);
    c->inst_partial = p;
    c->inst_bound = p ? bound : NULL;
    c->is_final = p ? p->is_final : t->is_final;
    cx_inst_push(ct_name(ct_class(c)), cx_cur());
    class_define_from(c, p ? p->head_end : t->head_end, p ? p->key : t->key,
                      ps);
    cx_inst_pop();
    inst_depth--;
}

struct cfunc *func_instance(struct ctemplate *t, struct ctarg *args,
                            int nargs)
{
    for (struct cinst *in = t->insts; in; in = in->next)
        if (args_same(in->args, in->nargs, args, nargs))
            return in->failed ? NULL : in->fn;
    struct cinst *in = xcalloc(1, sizeof *in);
    in->args = args_copy(args, nargs);
    in->nargs = nargs;
    in->next = t->insts;
    t->insts = in;
    struct cscope *ps = tparam_scope(t->params, t->nparams, in->args, nargs,
                                     t->scope);
    jmp_buf jb;
    void *saved = cx_sfinae;
    struct parse_state *st = parse_save();
    if (setjmp(jb)) {
        /* substitution failed: not a candidate (13.10.3.1) */
        parse_restore(st);
        cx_sfinae = saved;
        in->failed = 1;
        return NULL;
    }
    cx_sfinae = &jb;
    cx_inst_push(cx_fmt("the declaration of '%s'", t->name), cx_cur());
    struct cfunc *f = func_decl_replay(t, ps);
    if (!template_constraints(t->params, t->nparams, t->req_start,
                              t->req_end, ps, cx_cur()))
        cx_error(cx_cur(), "the constraints of '%s' are not satisfied",
                 t->name);
    cx_inst_pop();
    cx_sfinae = saved;
    parse_restore(st);
    f->spec_of = t;
    f->targs = in->args;
    f->local_inst = targs_local(in->args, nargs);
    f->ntargs = nargs;
    f->inst_scope = ps;
    in->fn = f;
    return f;
}

/* f's deduced return type, needed (a call): read its body now if it has
 * not been — a delayed in-class body, an instance's. */
void func_deduce_return(struct cfunc *f, const struct ctok *at)
{
    if (!ct_has_auto(f->type->to))
        return;
    if (!f->defined && f->is_defaulted && is_defaultable_cmp(f))
        define_defaulted_cmp(f);           /* auto operator<=> = default */
    if (!f->defined && !f->deducing) {
        f->deducing = 1;
        if (!f->lazy && f->body_tok >= 0 && f->def_scope)
            func_define_from(f);
        else
            func_ensure_body(f);
        f->deducing = 0;
    }
    if (ct_has_auto(f->type->to))
        cx_error(at, "'%s' is used before its return type is deduced",
                 f->name);
}

void func_ensure_body(struct cfunc *f)
{
    if (f->defined || f->is_deleted || f->tmpl)
        return;
    if (f->is_defaulted && !f->special && is_defaultable_cmp(f)) {
        define_defaulted_cmp(f);
        return;
    }
    if (f->inherited) {
        define_inherited_ctor(f);
        return;
    }
    if (!f->lazy) {
        /* a member of an instance, defined outside its class */
        if (f->cls && !f->is_implicit && !f->is_defaulted &&
            f->body_tok < 0 && (f->cls->tmpl || f->cls->scope->parent->k
                                                == SC_TEMPLATE ||
                                f->cls->owner->k == SC_CLASS))
            member_from_outdef(f);
        return;
    }
    /* (`extern template` leaves a class's non-inline members to another
     * unit; one defined in the class is inline, instantiated here as
     * g++ does, 13.9.3/10) */
    f->lazy = 0;
    if (++inst_depth > 900)
        cx_error(cx_cur(), "template instantiation depth exceeds 900 (in "
                           "'%s')", f->name);
    cx_inst_push(cx_fmt("the body of '%s'", f->name), cx_cur());
    func_define_from(f);
    cx_inst_pop();
    inst_depth--;
}

struct cty *alias_pattern(struct ctemplate *t)
{
    if (t->alias_pat_done)
        return t->alias_pat;
    t->alias_pat_done = 1;
    if (!t->pscope || t->builtin)
        return NULL;
    jmp_buf jb;
    void *saved = cx_sfinae;
    struct parse_state *st = parse_save();
    if (setjmp(jb)) {
        parse_restore(st);
        cx_sfinae = saved;
        return NULL;
    }
    cx_sfinae = &jb;
    cx_scope = t->pscope;
    cx_pos = t->decl_tok;
    cx_half_gt = 0;
    cx_in_targs = 0;
    cx_pattern = 1;
    t->alias_pat = parse_type_id();
    cx_sfinae = saved;
    parse_restore(st);
    return t->alias_pat;
}

/* __make_integer_seq<TT, T, N>: TT<T, 0, 1, ..., N - 1> */
static struct cty *make_integer_seq(struct ctemplate *t, struct ctarg *a,
                                    const struct ctok *at)
{
    if (!a[0].tmpl || a[0].tmpl->tparam || !a[1].type ||
        !ct_is_integer(a[1].type))
        cx_error(at, "'%s' takes a template, an integer type and a count",
                 t->name);
    long num = a[2].value;
    if (num < 0 || num > 100000)
        cx_error(at, "'%s' of %ld elements", t->name, num);
    struct ctarg *r = xcalloc((size_t)num + 1, sizeof *r);
    r[0].kind = TP_TYPE;
    r[0].type = a[1].type;
    for (long i = 0; i < num; i++) {
        r[i + 1].kind = TP_VALUE;
        r[i + 1].value = i;
        r[i + 1].vtype = ct_unqual(a[1].type);
    }
    struct ctemplate *tt = a[0].tmpl;
    if (tt->kind == TK_ALIAS)
        return alias_instance(tt, r, (int)num + 1, at);
    if (tt->kind != TK_CLASS)
        cx_error(at, "'%s' takes a class or alias template", t->name);
    return ct_class(class_instance(tt, r, (int)num + 1, at));
}

struct cty *alias_instance(struct ctemplate *t, struct ctarg *args, int n,
                           const struct ctok *at)
{
    struct ctarg *a = fit_args(t, t->params, t->nparams, args, n, t->scope,
                               at);
    if (t->builtin == BT_MAKE_INTEGER_SEQ)
        return make_integer_seq(t, a, at);
    for (struct cinst *in = t->insts; in; in = in->next)
        if (args_same(in->args, in->nargs, a, t->nparams))
            return in->type;
    struct parse_state *st = parse_save();
    cx_inst_push(cx_fmt("alias template '%s'", t->name), cx_cur());
    cx_scope = tparam_scope(t->params, t->nparams, a, t->nparams, t->scope);
    cx_pos = t->decl_tok;
    cx_half_gt = 0;
    cx_in_targs = 0;
    cx_pattern = 0;
    struct cty *ty = parse_type_id();
    parse_restore(st);
    struct cinst *in = xcalloc(1, sizeof *in);
    in->args = a;
    in->nargs = t->nparams;
    in->type = ty;
    in->next = t->insts;
    t->insts = in;
    return ty;
}

struct cvar *var_instance(struct ctemplate *t, struct ctarg *args, int n,
                          const struct ctok *at)
{
    struct ctarg *a = fit_args(t, t->params, t->nparams, args, n, t->scope,
                               at);
    for (struct cinst *in = t->insts; in; in = in->next)
        if (args_same(in->args, in->nargs, a, t->nparams))
            return in->var;
    struct cinst *in = xcalloc(1, sizeof *in);
    in->args = a;
    in->nargs = t->nparams;
    in->next = t->insts;
    t->insts = in;
    struct ctarg *bound = NULL;
    struct cpartial *p = t->partials ? match_partial(t, a, &bound) : NULL;
    cx_inst_push(cx_fmt("variable template '%s'", t->name), at);
    if (p)
        in->var = var_define_from(t, p->head_end, a,
                                  tparam_scope(p->params, p->nparams, bound,
                                               p->nparams, t->scope));
    else
        in->var = var_define_from(t, t->decl_tok, a,
                                  tparam_scope(t->params, t->nparams, a,
                                               t->nparams, t->scope));
    cx_inst_pop();
    return in->var;
}

void var_explicit_spec(struct ctemplate *t, struct ctarg *args, int n,
                       struct cvar *v, const struct ctok *at)
{
    struct ctarg *a = fit_args(t, t->params, t->nparams, args, n, t->scope,
                               at);
    for (struct cinst *in = t->insts; in; in = in->next)
        if (args_same(in->args, in->nargs, a, t->nparams))
            cx_error(at, "'%s' is specialized after its instantiation",
                     t->name);
    struct cinst *in = xcalloc(1, sizeof *in);
    in->args = a;
    in->nargs = t->nparams;
    in->var = v;
    in->next = t->insts;
    t->insts = in;
    v->targs = a;
    v->ntargs = t->nparams;
    v->cname = mangle_var(v, t->scope);
}
