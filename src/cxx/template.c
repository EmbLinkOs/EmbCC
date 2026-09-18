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
#include <stdio.h>
#include <string.h>

#include "../driver/util.h"

int cx_pattern;
int cx_in_targs;

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
            if (k < na && args[k].is_pack && na - k == 1) {
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
                      int *set, int np)
{
    if (P->is_pack || A->is_pack) {
        if (!P->is_pack || !A->is_pack || P->nelems != A->nelems)
            return P->is_pack && P->nelems == 1 &&
                   P->elems[0].kind == TP_TYPE &&
                   P->elems[0].type->k == CT_TPARAM ? 1 : 0;
        for (int i = 0; i < P->nelems; i++)
            if (!deduce_arg(&P->elems[i], &A->elems[i], out, set, np))
                return 0;
        return 1;
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
        return P->value == A->value;
    }
    if (P->kind == TP_TEMPLATE && A->kind == TP_TEMPLATE)
        return P->tmpl == A->tmpl;
    return 0;
}

/* Deduce the template parameters P mentions from the argument type A
 * (13.10.3.6): out[i] for parameter i, set[i] once known. */
static int deduce(struct cty *P, struct cty *A, struct ctarg *out,
                  int *set, int np)
{
    switch (P->k) {
    case CT_TPARAM: {
        int i = (int)P->n;
        if (i >= np || set[i] >= 2)
            return 1;             /* given explicitly: the argument converts */
        /* const T against const int: T is int; against int, T is int
         * too (the parameter may be more qualified, 13.10.3.2) */
        struct cty *v = ct_qual(ct_unqual(A), A->q & ~P->q);
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
        if (!deduce(P->to, A->to, out, set, np))
            return 0;
        for (int i = 0; i < P->np; i++)
            if (!deduce(P->params[i], A->params[i], out, set, np))
                return 0;
        return 1;
    case CT_MPTR:
        if (A->k != CT_MPTR)
            return 0;
        return deduce(P->to, A->to, out, set, np);
    case CT_TID: {
        if (A->k != CT_CLASS)
            return 0;
        struct cclass *c = instance_base(A->cls, P->tmpl);
        if (!c)
            return 0;
        for (int i = 0; i < P->ntargs && i < c->ntargs; i++)
            if (!deduce_arg(&P->targs[i], &c->targs[i], out, set, np))
                return 0;
        return 1;
    }
    default:
        return 1;              /* non-deduced: checked by conversion later */
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
             * element of the template parameter pack it names */
            struct cty *base = P;
            while (base->k == CT_LREF || base->k == CT_RREF ||
                   base->k == CT_PTR)
                base = base->to;
            if (base->k != CT_TPARAM || base->n >= np)
                continue;
            int ti = (int)base->n;
            if (set[ti] == 2)
                continue;                    /* given explicitly */
            struct ctarg one[1];
            int s1[1] = { 0 };
            memset(one, 0, sizeof one);
            struct cty *Pe = P;
            if (ct_is_ref(Pe)) {
                if (Pe->k == CT_RREF && Pe->to->k == CT_TPARAM &&
                    !Pe->to->q && e->vc == VC_LVALUE)
                    A = ct_ref(A, 0);
                Pe = Pe->to;
            } else {
                A = ct_unqual(ct_decay(A));
            }
            struct cty *Pl = xmalloc(sizeof *Pl);
            *Pl = *Pe;
            if (Pl->k == CT_TPARAM)
                Pl->n = 0;
            if (!deduce(Pl, A, one, s1, 1))
                return 0;
            struct ctarg *pk = &out[ti];
            pk->kind = TP_TYPE;
            pk->is_pack = 1;
            pk->elems = xrealloc(pk->elems, (size_t)(pk->nelems + 1) *
                                            sizeof *pk->elems);
            pk->elems[pk->nelems++] = one[0];
            set[ti] = 1;
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

/* The partial specialization whose pattern the arguments match (the
 * first; a more specialized one declared later is not yet preferred). */
static struct cpartial *match_partial(struct ctemplate *t, struct ctarg *a,
                                      struct ctarg **bound)
{
    for (struct cpartial *p = t->partials; p; p = p->next) {
        int np = p->nparams;
        struct ctarg *out = xcalloc((size_t)(np ? np : 1), sizeof *out);
        int *set = xcalloc((size_t)(np ? np : 1), sizeof *set);
        int ok = p->npattern == t->nparams;
        for (int i = 0; ok && i < p->npattern; i++)
            ok = deduce_arg(&p->pattern[i], &a[i], out, set, np);
        for (int i = 0; ok && i < np; i++)
            ok = set[i] || p->params[i].pack;
        if (ok) {
            *bound = out;
            return p;
        }
    }
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
    f->ntargs = nargs;
    f->inst_scope = ps;
    in->fn = f;
    return f;
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
