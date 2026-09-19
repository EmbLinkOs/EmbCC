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

/* ---- deduction ---- */

static int deduce(struct cty *P, struct cty *A, struct ctarg *out,
                  int *set, int np);

/* Matching a partial specialization (or ordering two): the argument must
 * BE the pattern with the parameters substituted — not convert to it. */
static int deduce_exact;

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

/* Deduce the template parameters P mentions from the argument type A
 * (13.10.3.6): out[i] for parameter i, set[i] once known. */
static int deduce(struct cty *P, struct cty *A, struct ctarg *out,
                  int *set, int np)
{
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
    case CT_FUNC:
        if (A->k != CT_FUNC || A->np != P->np)
            return 0;
        if (deduce_exact && (A->variadic != P->variadic || A->fq != P->fq ||
                             A->refq != P->refq))
            return 0;
        if (!deduce(P->to, A->to, out, set, np))
            return 0;
        for (int i = 0; i < P->np; i++)
            if (!deduce(P->params[i], A->params[i], out, set, np))
                return 0;
        return 1;
    case CT_MPTR:
        if (A->k != CT_MPTR || (deduce_exact && A->cls != P->cls))
            return 0;
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
        }
        if (!c)
            return 0;
        /* the class's arguments against the pattern's as written */
        int na = c->ntargs, nP = P->ntargs;
        struct ctarg *flat = flatten(c->targs, &na);
        struct ctarg *pf = flatten(P->targs, &nP);
        return deduce_seq(pf, nP, flat, na, out, set, np);
    }
    default:
        /* non-deduced: checked by conversion later — or, matching a
         * partial specialization, the same type */
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
        int aref = A->k == CT_LREF ? 1 : A->k == CT_RREF ? 2 : 0;
        int pref = P->k == CT_LREF ? 1 : P->k == CT_RREF ? 2 : 0;
        if (ct_is_ref(A))
            A = A->to;
        if (ct_is_ref(P))
            P = P->to;
        if (!deduce(ct_unqual(P), ct_unqual(A), out, set, np))
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
        if (e->k == E_INITLIST && !e->t)
            continue;                        /* non-deduced */
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
        if (set[i])
            continue;
        if (t->params[i].pack) {
            out[i].kind = t->params[i].kind;
            out[i].is_pack = 1;
            set[i] = 1;
            continue;
        }
        if (t->params[i].def_tok < 0)
            return 0;
        int saved_sfinae_ok = 1;
        (void)saved_sfinae_ok;
        out[i] = default_arg(t, t->params, np, i, out, t->scope, cx_cur());
        set[i] = 1;
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

/* Does partial specialization p's pattern match arguments a (np of p's
 * parameters bound into out)? */
static int partial_matches(struct cpartial *p, struct ctarg *a, int na,
                           struct ctarg **bound)
{
    int np = p->nparams;
    struct ctarg *out = xcalloc((size_t)(np ? np : 1), sizeof *out);
    int *set = xcalloc((size_t)(np ? np : 1), sizeof *set);
    int nP = p->npattern;
    struct ctarg *pf = flatten(p->pattern, &nP);
    struct ctarg *af = flatten(a, &na);
    int saved = deduce_exact;
    deduce_exact = 1;
    int ok = deduce_seq(pf, nP, af, na, out, set, np);
    deduce_exact = saved;
    for (int i = 0; ok && i < np; i++)
        ok = set[i] || p->params[i].pack;
    *bound = out;
    return ok;
}

/* Is partial a at least as specialized as b: does b's pattern match a's
 * (a's parameters standing as unique types, 13.7.6.3)? */
static int partial_at_least(struct cpartial *a, struct cpartial *b)
{
    struct ctarg *bound;
    return partial_matches(b, a->pattern, a->npattern, &bound);
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
        if (partial_matches(p, a, t->nparams, &mb[n]))
            m[n++] = p;
    if (n == 0)
        return NULL;
    for (int i = 0; i < n; i++) {
        int best = 1;
        for (int j = 0; best && j < n; j++)
            best = i == j || (partial_at_least(m[i], m[j]) &&
                              !partial_at_least(m[j], m[i]));
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

void class_ensure(struct cclass *c)
{
    if (!c->inst_pending)
        return;
    struct ctemplate *t = c->tmpl;
    struct ctarg *bound = NULL;
    struct cpartial *p = match_partial(t, c->targs, &bound);
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
    class_define_from(c, p ? p->head_end : t->head_end, p ? p->key : t->key,
                      ps);
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
    struct cfunc *f = func_decl_replay(t, ps);
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
    if (!f->lazy) {
        /* a member of an instance, defined outside its class */
        if (f->cls && !f->is_implicit && !f->is_defaulted &&
            f->body_tok < 0 && (f->cls->tmpl || f->cls->scope->parent->k
                                                == SC_TEMPLATE ||
                                f->cls->owner->k == SC_CLASS))
            member_from_outdef(f);
        return;
    }
    if (f->cls && f->cls->extern_inst)
        return;                  /* `extern template`: another unit's */
    f->lazy = 0;
    if (++inst_depth > 900)
        cx_error(cx_cur(), "template instantiation depth exceeds 900 (in "
                           "'%s')", f->name);
    func_define_from(f);
    inst_depth--;
}

struct cty *alias_instance(struct ctemplate *t, struct ctarg *args, int n,
                           const struct ctok *at)
{
    struct ctarg *a = fit_args(t, t->params, t->nparams, args, n, t->scope,
                               at);
    for (struct cinst *in = t->insts; in; in = in->next)
        if (args_same(in->args, in->nargs, a, t->nparams))
            return in->type;
    struct parse_state *st = parse_save();
    cx_scope = tparam_scope(t->params, t->nparams, a, t->nparams, t->scope);
    cx_pos = t->decl_tok;
    cx_in_targs = 0;
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
    in->var = var_define_from(t, a,
                              tparam_scope(t->params, t->nparams, a,
                                           t->nparams, t->scope));
    return in->var;
}
