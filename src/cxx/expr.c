/* Expressions: parsed and typed together. Every operand's conversion is
 * made explicit here (E_CAST, reference binding as an address, a
 * temporary's materialization), so emit.c only spells the tree in C.
 *
 * Also here: implicit conversion sequences and their ranking — overload
 * resolution (12.2) — and initialization (9.4), which declarations,
 * arguments, returns and new-expressions all share. */
#include "cxx.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../driver/util.h"

struct cexpr *ex_new(enum cexpr_kind k, struct cty *t, int vc)
{
    struct cexpr *e = xcalloc(1, sizeof *e);
    e->k = k;
    e->t = t;
    e->vc = vc;
    e->line = cx_cur()->t.line;
    e->file = cx_cur()->file;
    return e;
}

struct cexpr *ex_int(long v, struct cty *t)
{
    struct cexpr *e = ex_new(E_INT, t, VC_PRVALUE);
    e->ival = v;
    return e;
}

static struct cexpr *ex1(enum cexpr_kind k, struct cty *t, int vc,
                         struct cexpr *a0)
{
    struct cexpr *e = ex_new(k, t, vc);
    e->a = xmalloc(sizeof *e->a);
    e->a[0] = a0;
    e->na = 1;
    e->line = a0->line;
    e->file = a0->file;
    return e;
}

static struct cexpr *ex2(enum cexpr_kind k, struct cty *t, int vc,
                         struct cexpr *a0, struct cexpr *a1)
{
    struct cexpr *e = ex_new(k, t, vc);
    e->a = xmalloc(2 * sizeof *e->a);
    e->a[0] = a0;
    e->a[1] = a1;
    e->na = 2;
    e->line = a0->line;
    e->file = a0->file;
    return e;
}

static void ex_error(const struct cexpr *e, const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    diag_error_at(e && e->file ? e->file : cx_cur()->file,
                  e ? e->line : cx_cur()->t.line, 0, "%s", msg);
    exit(1);
}

struct cexpr *ex_cast(struct cexpr *e, struct cty *t)
{
    return ex1(E_CAST, t, VC_PRVALUE, e);
}

struct cexpr *ex_addr(struct cexpr *e)
{
    return ex1(E_ADDR, ct_ptr(e->t), VC_PRVALUE, e);
}

struct cexpr *ex_deref(struct cexpr *p)
{
    return ex1(E_DEREF, p->t->to, VC_LVALUE, p);
}

struct cexpr *ex_this(void)
{
    if (!cx_curfn || !cx_curfn->this_var)
        cx_error(cx_cur(), "'this' is only available in a non-static "
                           "member function");
    struct cexpr *e = ex_new(E_THIS, cx_curfn->this_var->type, VC_PRVALUE);
    return e;
}

static struct cexpr *materialize(struct cexpr *e)
{
    return ex1(E_TEMP, ct_unqual(e->t), VC_XVALUE, e);
}

static int is_null_const(const struct cexpr *e)
{
    if (e->k == E_NULLPTR)
        return 1;
    if (e->k == E_INT && e->is_null_const)
        return 1;
    return 0;
}

static int is_bitfield(const struct cexpr *e)
{
    return e->k == E_MEMBER && e->field->bitwidth >= 0;
}

/* ---- constant evaluation ---- */

static long truncate_to(long v, const struct cty *t)
{
    if (t->k == CT_BOOL)
        return v != 0;
    long sz = ct_size(t);
    if (sz >= 8 || !ct_is_integer(t))
        return v;
    unsigned long mask = (1UL << (sz * 8)) - 1;
    unsigned long u = (unsigned long)v & mask;
    if (ct_is_signed(t) && (u >> (sz * 8 - 1)))
        u |= ~mask;
    return (long)u;
}

int expr_const(struct cexpr *e, long *out)
{
    long a, b, c;
    switch (e->k) {
    case E_INT:
        *out = e->ival;
        return 1;
    case E_NULLPTR:
        *out = 0;
        return 1;
    case E_VAR:
        if (e->var->has_const) {
            *out = e->var->const_val;
            return 1;
        }
        return 0;
    case E_CAST:
        if (!ct_is_integer(e->t) || !expr_const(e->a[0], &a))
            return 0;
        if (!ct_is_integer(e->a[0]->t) && e->a[0]->t->k != CT_NULLPTR)
            return 0;
        *out = truncate_to(a, e->t);
        return 1;
    case E_UNARY:
        if (!expr_const(e->a[0], &a))
            return 0;
        switch (e->op) {
        case TOK_MINUS: *out = truncate_to(-a, e->t); return 1;
        case TOK_PLUS: *out = a; return 1;
        case TOK_TILDE: *out = truncate_to(~a, e->t); return 1;
        case TOK_BANG: *out = !a; return 1;
        default: return 0;
        }
    case E_BINARY: {
        if (e->op == TOK_ANDAND || e->op == TOK_OROR) {
            if (!expr_const(e->a[0], &a))
                return 0;
            if (e->op == TOK_ANDAND && !a) { *out = 0; return 1; }
            if (e->op == TOK_OROR && a) { *out = 1; return 1; }
            if (!expr_const(e->a[1], &b))
                return 0;
            *out = b != 0;
            return 1;
        }
        if (!ct_is_integer(e->a[0]->t) || !ct_is_integer(e->a[1]->t))
            return 0;
        if (!expr_const(e->a[0], &a) || !expr_const(e->a[1], &b))
            return 0;
        int uns = !ct_is_signed(e->a[0]->t);
        unsigned long ua = (unsigned long)a, ub = (unsigned long)b;
        long r;
        switch (e->op) {
        case TOK_PLUS: r = (long)(ua + ub); break;
        case TOK_MINUS: r = (long)(ua - ub); break;
        case TOK_STAR: r = (long)(ua * ub); break;
        case TOK_SLASH:
            if (!b) return 0;
            r = uns ? (long)(ua / ub) : a / b;
            break;
        case TOK_PERCENT:
            if (!b) return 0;
            r = uns ? (long)(ua % ub) : a % b;
            break;
        case TOK_SHL: r = (long)(ua << (b & 63)); break;
        case TOK_SHR: r = uns ? (long)(ua >> (b & 63)) : a >> (b & 63); break;
        case TOK_AMP: r = a & b; break;
        case TOK_PIPE: r = a | b; break;
        case TOK_CARET: r = a ^ b; break;
        case TOK_EQEQ: r = a == b; break;
        case TOK_NEQ: r = a != b; break;
        case TOK_LT: r = uns ? ua < ub : a < b; break;
        case TOK_GT: r = uns ? ua > ub : a > b; break;
        case TOK_LE: r = uns ? ua <= ub : a <= b; break;
        case TOK_GE: r = uns ? ua >= ub : a >= b; break;
        default: return 0;
        }
        *out = truncate_to(r, e->t);
        return 1;
    }
    case E_COND:
        if (!expr_const(e->a[0], &c))
            return 0;
        return expr_const(e->a[c ? 1 : 2], out);
    default:
        return 0;
    }
}

/* ---- conversions ---- */

struct cexpr *rvalue(struct cexpr *e)
{
    if (e->t->k == CT_ARRAY || e->t->k == CT_FUNC) {
        if (e->k == E_OVL) {
            if (e->fn->next)
                ex_error(e, "'%s' is overloaded: which one is meant is "
                            "unclear here", e->fn->name);
            if (e->fn->cls && !e->fn->is_static)
                ex_error(e, "pointers to member functions are not "
                            "supported yet (CX2)");
            struct cexpr *f = ex_new(E_FUNC, e->fn->type, VC_LVALUE);
            f->fn = e->fn;
            f->line = e->line;
            f->file = e->file;
            e = f;
        }
        struct cexpr *d = xmalloc(sizeof *d);
        *d = *e;
        d->t = ct_decay(e->t);
        d->vc = VC_PRVALUE;
        return d;
    }
    if (e->vc == VC_PRVALUE || e->t->k == CT_CLASS)
        return e;
    struct cexpr *d = xmalloc(sizeof *d);
    *d = *e;
    d->t = ct_unqual(e->t);
    d->vc = VC_PRVALUE;
    return d;
}

enum { R_EXACT, R_PROMO, R_CONV, R_USER, R_ELLIPSIS, R_BAD };

struct ics {
    int rank;
    int ptr_bool;             /* a pointer converted to bool */
    int qual_adj;             /* a qualification conversion */
    int is_ref;               /* a reference binding */
    int binds_rvref;          /* ... of an rvalue reference to an rvalue */
    unsigned ref_cv;          /* ... to this cv-qualified type */
    struct cfunc *user;       /* the user-defined conversion */
};

static struct ics ics_of(struct cexpr *e, struct cty *to);

static int is_integral_promotion(struct cty *from, struct cty *to)
{
    if (!ct_is_integer(from) || !ct_is_integer(to) || to->k == CT_ENUM)
        return 0;
    if (from->k == CT_ENUM && from->en->scoped)
        return 0;
    struct cty *p = ct_promote(from);
    return p->k == to->k && from->k != to->k;
}

/* The standard conversion from e to the non-reference, non-class t. */
static struct ics std_conv(struct cexpr *e, struct cty *to)
{
    struct ics r;
    memset(&r, 0, sizeof r);
    r.rank = R_BAD;
    struct cty *tu = ct_unqual(to);
    if (e->k == E_INITLIST) {
        if (e->t)
            return std_conv(e, to);
        if (e->na == 0) {
            r.rank = R_EXACT;
            return r;
        }
        if (e->na == 1 && e->a[0]->k != E_INITLIST)
            return std_conv(e->a[0], to);
        return r;
    }
    if (e->k == E_OVL || e->k == E_FUNC) {
        struct cty *ft = tu->k == CT_PTR ? tu->to : NULL;
        if (!ft || ft->k != CT_FUNC)
            return r;
        struct cfunc *set = e->fn;
        for (struct cfunc *f = set; f; f = e->k == E_OVL ? f->next : NULL)
            if ((!f->cls || f->is_static) && ct_same_unqual(f->type, ft))
                r.rank = R_EXACT;
        return r;
    }
    if (e->t->k == CT_CLASS)
        return r;
    struct cty *from = ct_unqual(ct_decay(e->t));
    if (ct_same(from, tu)) {
        r.rank = R_EXACT;
        return r;
    }
    if (tu->k == CT_BOOL) {
        if (ct_is_arith(from) && !(from->k == CT_ENUM && from->en->scoped)) {
            r.rank = R_CONV;
        } else if (from->k == CT_PTR || from->k == CT_NULLPTR) {
            r.rank = R_CONV;
            r.ptr_bool = 1;
        }
        return r;
    }
    if (ct_is_arith(tu) && tu->k != CT_ENUM) {
        if (ct_is_arith(from) && !(from->k == CT_ENUM && from->en->scoped)) {
            if (is_integral_promotion(from, tu) ||
                (from->k == CT_FLOAT && tu->k == CT_DOUBLE))
                r.rank = R_PROMO;
            else
                r.rank = R_CONV;
        }
        return r;
    }
    if (tu->k == CT_PTR) {
        if (is_null_const(e) || from->k == CT_NULLPTR) {
            r.rank = R_CONV;
            return r;
        }
        if (from->k != CT_PTR)
            return r;
        struct cty *pf = from->to, *pt = tu->to;
        if ((pt->q & pf->q) != pf->q)
            return r;                         /* would drop a qualifier */
        if (ct_same_unqual(pf, pt)) {
            r.rank = R_EXACT;
            r.qual_adj = pt->q != pf->q;
            return r;
        }
        /* T** -> const T* const* and the like: one level deep */
        if (pf->k == CT_PTR && pt->k == CT_PTR &&
            ct_same_unqual(pf->to, pt->to) &&
            (pt->to->q & pf->to->q) == pf->to->q &&
            (pt->to->q == pf->to->q || (pt->q & CQ_CONST))) {
            r.rank = R_EXACT;
            r.qual_adj = 1;
            return r;
        }
        if (pt->k == CT_VOID && pf->k != CT_FUNC) {
            r.rank = R_CONV;
            return r;
        }
        return r;
    }
    if (tu->k == CT_NULLPTR && is_null_const(e)) {
        r.rank = R_CONV;
        return r;
    }
    return r;
}

static struct ics ref_ics(struct cexpr *e, struct cty *rt)
{
    struct ics r;
    memset(&r, 0, sizeof r);
    r.rank = R_BAD;
    struct cty *T = rt->to;
    int rv = rt->k == CT_RREF;
    int cref = (T->q & CQ_CONST) && !(T->q & CQ_VOLATILE);
    if (e->k == E_INITLIST || e->k == E_OVL || e->k == E_FUNC) {
        if (!rv && !cref && e->k == E_INITLIST)
            return r;
        r = ics_of(e, ct_unqual(T));
        r.is_ref = 1;
        r.binds_rvref = rv;
        r.ref_cv = T->q;
        return r;
    }
    int compat = ct_same_unqual(e->t, T) && (T->q & e->t->q) == e->t->q;
    int lv = e->vc == VC_LVALUE && !is_bitfield(e);
    r.is_ref = 1;
    r.ref_cv = T->q;
    if (compat && !is_bitfield(e)) {
        if (!rv && lv) {
            r.rank = R_EXACT;
            return r;
        }
        if (rv && !lv) {
            r.rank = R_EXACT;
            r.binds_rvref = 1;
            return r;
        }
        if (!rv && cref && !lv) {
            r.rank = R_EXACT;
            return r;
        }
        if (rv && lv)
            return r;
    }
    if (!rv && !cref)
        return r;
    if (compat && e->vc == VC_LVALUE && rv)
        return r;
    struct ics s = ics_of(e, ct_unqual(T));
    if (s.rank == R_BAD)
        return r;
    s.is_ref = 1;
    s.binds_rvref = rv;
    s.ref_cv = T->q;
    return s;
}

static struct cfunc *converting_ctor(struct cclass *c, struct cexpr *e,
                                     int allow_explicit);

static struct ics ics_of(struct cexpr *e, struct cty *to)
{
    if (ct_is_ref(to))
        return ref_ics(e, to);
    struct ics r;
    memset(&r, 0, sizeof r);
    r.rank = R_BAD;
    if (to->k == CT_CLASS) {
        if (e->k != E_INITLIST && e->t && e->t->k == CT_CLASS &&
            e->t->cls == to->cls) {
            r.rank = R_EXACT;
            return r;
        }
        if (e->k == E_INITLIST) {
            if (to->cls->aggregate || e->na == 0) {
                r.rank = R_USER;
                return r;
            }
            struct cfunc *f = resolve(to->cls->ctors, NULL, e->a, e->na,
                                      NULL, NULL);
            if (f) {
                r.rank = R_USER;
                r.user = f;
            }
            return r;
        }
        struct cfunc *f = converting_ctor(to->cls, e, 0);
        if (f) {
            r.rank = R_USER;
            r.user = f;
        }
        return r;
    }
    if (e->t && e->t->k == CT_CLASS)
        return r;                     /* conversion functions: CX2 */
    return std_conv(e, to);
}

/* 1 if a is a better conversion than b, -1 if worse, 0 if neither. */
static int ics_cmp(const struct ics *a, const struct ics *b)
{
    if (a->rank != b->rank)
        return a->rank < b->rank ? 1 : -1;
    if (a->rank == R_USER)
        return 0;
    if (a->ptr_bool != b->ptr_bool)
        return a->ptr_bool ? -1 : 1;
    if (a->is_ref && b->is_ref) {
        if (a->binds_rvref != b->binds_rvref)
            return a->binds_rvref ? 1 : -1;
        if (a->ref_cv != b->ref_cv) {
            if ((b->ref_cv & a->ref_cv) == a->ref_cv)
                return 1;
            if ((a->ref_cv & b->ref_cv) == b->ref_cv)
                return -1;
        }
    }
    if (a->qual_adj != b->qual_adj)
        return a->qual_adj ? -1 : 1;
    return 0;
}

/* A candidate being considered: its conversion per argument (index 0 the
 * implicit object, when there is one). */
struct cand {
    struct cfunc *f;
    struct ics *ics;
    int n;
    int has_obj;
};

static int cand_better(const struct cand *a, const struct cand *b)
{
    int better = 0;
    int start = (a->has_obj && b->has_obj) ? 0 : 1;
    for (int i = start; i < a->n && i < b->n; i++) {
        int c = ics_cmp(&a->ics[i], &b->ics[i]);
        if (c < 0)
            return 0;
        if (c > 0)
            better = 1;
    }
    return better;
}

static const char *arg_types(struct cexpr **args, int na)
{
    static char buf[512];
    size_t n = 0;
    buf[0] = 0;
    for (int i = 0; i < na && n < sizeof buf - 64; i++) {
        const char *t = args[i]->k == E_INITLIST && !args[i]->t
                        ? "{...}" : args[i]->k == E_OVL
                        ? "<overloaded function>" : ct_name(args[i]->t);
        n += (size_t)snprintf(buf + n, sizeof buf - n, "%s%s", i ? ", " : "",
                              t);
    }
    return buf;
}

struct cfunc *resolve(struct cfunc *set, struct cexpr *obj,
                      struct cexpr **args, int na, const struct ctok *at,
                      const char *what)
{
    return resolve_ex(set, obj, args, na, at, what, 0);
}

struct cfunc *resolve_ex(struct cfunc *set, struct cexpr *obj,
                         struct cexpr **args, int na, const struct ctok *at,
                         const char *what, int flags)
{
    int ncand = 0;
    for (struct cfunc *f = set; f; f = f->next)
        ncand++;
    struct cand *cs = xcalloc((size_t)(ncand ? ncand : 1), sizeof *cs);
    int nv = 0;
    struct cexpr *objlv = obj ? ex_deref(obj) : NULL;
    for (struct cfunc *f = set; f; f = f->next) {
        struct cty *ft = f->type;
        if (na > ft->np && !ft->variadic)
            continue;
        if (na < ft->np && (!f->defargs || !f->defargs[na]))
            continue;
        if ((flags & RS_NO_EXPLICIT) && f->is_explicit)
            continue;
        int member = f->cls && !f->is_static && !f->is_ctor;
        if (member && !obj)
            continue;
        struct cand *c = &cs[nv];
        c->f = f;
        c->n = na + 1;
        c->has_obj = member;
        c->ics = xcalloc((size_t)c->n, sizeof *c->ics);
        int ok = 1;
        if (member) {
            struct cty *self = ct_ref(ct_qual(ct_class(f->cls), ft->fq),
                                      ft->refq == 2);
            c->ics[0] = ref_ics(objlv, self);
            ok = c->ics[0].rank != R_BAD;
        }
        for (int i = 0; ok && i < na; i++) {
            if (i >= ft->np) {
                c->ics[i + 1].rank = R_ELLIPSIS;
                continue;
            }
            c->ics[i + 1] = ics_of(args[i], ft->params[i]);
            ok = c->ics[i + 1].rank != R_BAD;
            if ((flags & RS_NO_USER) && c->ics[i + 1].rank == R_USER)
                ok = 0;
        }
        if (ok)
            nv++;
    }
    if (nv == 0) {
        if (!at)
            return NULL;
        cx_error(at, "no matching function for call to '%s(%s)'",
                 what ? what : set ? set->name : "?", arg_types(args, na));
    }
    int best = 0;
    for (int i = 1; i < nv; i++)
        if (cand_better(&cs[i], &cs[best]))
            best = i;
    for (int i = 0; i < nv; i++) {
        if (i == best)
            continue;
        if (!cand_better(&cs[best], &cs[i])) {
            if (!at)
                return NULL;
            cx_error(at, "call to '%s(%s)' is ambiguous",
                     what ? what : set->name, arg_types(args, na));
        }
    }
    return cs[best].f;
}

/* A non-explicit (unless allowed) constructor of c taking e alone, found
 * without user-defined conversions of its own. */
static struct cfunc *converting_ctor(struct cclass *c, struct cexpr *e,
                                     int allow_explicit)
{
    if (!c->complete)
        return NULL;
    struct cfunc *found = NULL;
    for (struct cfunc *f = c->ctors; f; f = f->next) {
        if (f->is_explicit && !allow_explicit)
            continue;
        struct cty *ft = f->type;
        if (ft->np < 1 && !ft->variadic)
            continue;
        if (ft->np > 1 && (!f->defargs || !f->defargs[1]))
            continue;
        struct cty *p = ft->params[0];
        struct cty *pb = ct_strip_ref(p);
        if (pb->k == CT_CLASS && pb->cls == c)
            continue;                         /* a copy/move constructor */
        struct ics s = ct_is_ref(p) ? ref_ics(e, p)
                       : pb->k == CT_CLASS ? (struct ics){ .rank = R_BAD }
                       : std_conv(e, p);
        if (s.rank >= R_USER)
            continue;
        if (found)
            return resolve(c->ctors, NULL, &e, 1, NULL, NULL);
        found = f;
    }
    return found;
}

struct cexpr *convert(struct cexpr *e, struct cty *t, const char *ctx)
{
    if (e->k == E_INITLIST && !e->t)
        return init_object(t, INIT_COPY_LIST, &e, 1, NULL);
    if (t->k == CT_CLASS)
        return init_object(t, INIT_COPY, &e, 1, NULL);
    if (t->k == CT_VOID)
        return ex_cast(e, ct_basic(CT_VOID));
    if (e->k == E_OVL || (e->k == E_FUNC && t->k == CT_PTR)) {
        struct cty *ft = t->k == CT_PTR ? t->to : NULL;
        if (ft && ft->k == CT_FUNC)
            for (struct cfunc *f = e->fn; f;
                 f = e->k == E_OVL ? f->next : NULL)
                if ((!f->cls || f->is_static) &&
                    ct_same_unqual(f->type, ft)) {
                    struct cexpr *r = ex_new(E_FUNC, ct_ptr(f->type),
                                             VC_PRVALUE);
                    r->fn = f;
                    r->line = e->line;
                    r->file = e->file;
                    return r;
                }
        ex_error(e, "no overload of '%s' converts to '%s' in %s",
                 e->fn->name, ct_name(t), ctx);
    }
    struct ics s = std_conv(e, t);
    if (s.rank == R_BAD)
        ex_error(e, "cannot convert '%s' to '%s' in %s", ct_name(e->t),
                 ct_name(t), ctx);
    e = rvalue(e);
    struct cty *tu = ct_unqual(t);
    if (ct_same(e->t, tu))
        return e;
    return ex_cast(e, tu);
}

struct cexpr *convert_bool(struct cexpr *e, const char *ctx)
{
    e = rvalue(e);
    if (e->t->k == CT_CLASS)
        ex_error(e, "a class used as a condition (%s): explicit operator "
                    "bool is not supported yet (CX2)", ctx);
    if (e->t->k == CT_BOOL)
        return e;
    if (!ct_is_scalar(e->t))
        ex_error(e, "'%s' used where a condition is required (%s)",
                 ct_name(e->t), ctx);
    return ex_cast(e, ct_basic(CT_BOOL));
}

/* &e for a reference: e's address, or a materialized temporary's. */
struct cexpr *bind_ref(struct cexpr *e, struct cty *rt, const char *ctx)
{
    struct cty *T = rt->to;
    int rv = rt->k == CT_RREF;
    int cref = (T->q & CQ_CONST) && !(T->q & CQ_VOLATILE);
    if (e->k == E_INITLIST && !e->t) {
        if (!rv && !cref)
            ex_error(e, "a braced list cannot bind to a non-const "
                        "reference (%s)", ctx);
        return ex_addr(materialize(init_object(ct_unqual(T), INIT_COPY_LIST,
                                               &e, 1, NULL)));
    }
    if (e->k == E_OVL || e->k == E_FUNC) {
        if (T->k != CT_FUNC)
            ex_error(e, "cannot bind '%s' to a reference to %s (%s)",
                     e->fn->name, ct_name(T), ctx);
        struct cexpr *f = convert(e, ct_ptr(T), ctx);
        return f;
    }
    int compat = ct_same_unqual(e->t, T) && (T->q & e->t->q) == e->t->q;
    if (compat && !is_bitfield(e)) {
        if (!rv && e->vc == VC_LVALUE)
            return ex_addr(e);
        if (rv && e->vc == VC_LVALUE)
            ex_error(e, "cannot bind an rvalue reference to an lvalue of "
                        "type '%s' (%s)", ct_name(e->t), ctx);
        if (!rv && !cref)
            ex_error(e, "cannot bind a non-const lvalue reference of type "
                        "'%s&' to an rvalue (%s)", ct_name(T), ctx);
        if (e->vc == VC_XVALUE)
            return ex_addr(e);
        return ex_addr(materialize(e));
    }
    if (!rv && !cref)
        ex_error(e, "cannot bind a reference of type '%s&' to a value of "
                    "type '%s' (%s)", ct_name(T), ct_name(e->t), ctx);
    if (compat && e->vc == VC_LVALUE && rv && !is_bitfield(e))
        ex_error(e, "cannot bind an rvalue reference to an lvalue (%s)", ctx);
    struct cexpr *v = convert(e, ct_unqual(T), ctx);
    return ex_addr(materialize(v));
}

/* The value an argument passed through `...` takes: promoted, decayed. */
static struct cexpr *vararg_promote(struct cexpr *e)
{
    if (e->k == E_OVL)
        e = rvalue(e);
    e = rvalue(e);
    struct cty *t = e->t;
    if (t->k == CT_CLASS) {
        if (!t->cls->trivial_for_calls)
            ex_error(e, "passing a non-trivially-copyable '%s' through "
                        "'...'", ct_name(t));
        return e;
    }
    if (t->k == CT_FLOAT)
        return ex_cast(e, ct_basic(CT_DOUBLE));
    if (t->k == CT_NULLPTR)
        return ex_cast(e, ct_ptr(ct_basic(CT_VOID)));
    if (ct_is_integer(t)) {
        struct cty *p = ct_promote(t);
        if (!ct_same(p, t))
            return ex_cast(e, p);
    }
    return e;
}

static struct cexpr *convert_param(struct cexpr *arg, struct cty *p,
                                   const struct ctok *at)
{
    if (ct_is_ref(p))
        return bind_ref(arg, p, "an argument");
    if (p->k == CT_CLASS) {
        if (!p->cls->trivial_for_calls)
            ex_error(arg, "passing a '%s' (a class with a non-trivial copy "
                          "constructor or destructor) by value is not "
                          "supported yet (CX2)", ct_name(p));
        return init_object(p, arg->k == E_INITLIST && !arg->t
                              ? INIT_COPY_LIST : INIT_COPY, &arg, 1, at);
    }
    return convert(arg, p, "an argument");
}

static int convert_args_ft(struct cty *ft, struct cexpr **defs,
                           struct cexpr **args, int na,
                           const struct ctok *at, struct cexpr ***out)
{
    int n = na > ft->np ? na : ft->np;
    struct cexpr **o = xmalloc((size_t)(n ? n : 1) * sizeof *o);
    for (int i = 0; i < n; i++) {
        if (i >= ft->np) {
            o[i] = vararg_promote(args[i]);
            continue;
        }
        struct cexpr *a = i < na ? args[i] : defs ? defs[i] : NULL;
        if (!a)
            cx_error(at, "too few arguments");
        o[i] = convert_param(a, ft->params[i], at);
    }
    *out = o;
    return n;
}

int convert_args(struct cfunc *fn, struct cexpr **args, int na,
                 const struct ctok *at, struct cexpr ***out)
{
    return convert_args_ft(fn->type, fn->defargs, args, na, at, out);
}

static struct cexpr *call_result(struct cty *ret)
{
    if (ret->k == CT_LREF)
        return ex_new(E_CALL, ret->to, VC_LVALUE);
    if (ret->k == CT_RREF)
        return ex_new(E_CALL, ret->to, VC_XVALUE);
    return ex_new(E_CALL, ret, VC_PRVALUE);
}

struct cexpr *make_call(struct cfunc *fn, struct cexpr *obj,
                        struct cexpr **args, int na, const struct ctok *at)
{
    if (fn->is_deleted)
        cx_error(at, "use of deleted function '%s'", fn->name);
    struct cty *ret = fn->type->to;
    if (ret->k == CT_CLASS && ret->cls->complete &&
        !ret->cls->trivial_for_calls)
        cx_error(at, "returning a '%s' (a class with a non-trivial copy "
                     "constructor or destructor) by value is not supported "
                     "yet (CX2)", ct_name(ret));
    struct cexpr **conv;
    int n = convert_args(fn, args, na, at, &conv);
    struct cexpr *e = call_result(ret);
    e->fn = fn;
    int member = fn->cls && !fn->is_static && !fn->is_ctor;
    if (member) {
        if (!obj)
            cx_error(at, "calling member function '%s' without an object",
                     fn->name);
        e->a = xmalloc((size_t)(n + 1) * sizeof *e->a);
        e->a[0] = obj;
        memcpy(e->a + 1, conv, (size_t)n * sizeof *conv);
        e->na = n + 1;
    } else {
        e->a = conv;
        e->na = n;
    }
    fn->used = 1;
    return e;
}

struct cexpr *call_global_op(const char *name, struct cexpr **args, int na,
                             const struct ctok *at)
{
    struct csym *y = lookup_in(cx_global, name);
    if (!y || y->k != CS_FUNC)
        cx_error(at, "'%s' is not declared", name);
    struct cfunc *f = resolve(y->fns, NULL, args, na, at, name);
    return make_call(f, NULL, args, na, at);
}

/* ---- initialization ---- */

static int is_char_type(const struct cty *t)
{
    return t->k == CT_CHAR || t->k == CT_SCHAR || t->k == CT_UCHAR;
}

/* May the string literal s initialize an array of `elem`? */
static int string_fits(const struct cexpr *s, const struct cty *elem)
{
    struct cty *st = s->t->to;
    if (s->swidth == 1 && st->k == CT_CHAR)
        return is_char_type(elem);
    if (st->k == CT_CHAR8)
        return elem->k == CT_CHAR8 || elem->k == CT_CHAR ||
               elem->k == CT_UCHAR;
    return ct_same_unqual(st, elem);
}

static struct cexpr *string_init(struct cty *t, struct cexpr *s,
                                 const struct ctok *at)
{
    (void)at;
    if (t->n >= 0 && s->slen - 1 > t->n)
        ex_error(s, "initializer-string for '%s' is too long", ct_name(t));
    struct cexpr *r = xmalloc(sizeof *r);
    *r = *s;
    r->t = t->n >= 0 ? t : ct_array(t->to, s->slen);
    r->vc = VC_PRVALUE;
    return r;
}

static int is_aggregate(const struct cty *t)
{
    return t->k == CT_ARRAY || (t->k == CT_CLASS && t->cls->aggregate);
}

static struct cexpr *aggregate_init(struct cty *t, struct cexpr **items,
                                    int n, int *idx, const struct ctok *at);

/* Initialize a t from the list's items at *idx: with brace elision, a
 * subaggregate written without braces takes the items it needs. */
static struct cexpr *init_from_items(struct cty *t, struct cexpr **items,
                                     int n, int *idx, const struct ctok *at)
{
    struct cexpr *it = items[*idx];
    int braced = it->k == E_INITLIST && !it->t;
    if (!braced && is_aggregate(t) &&
        !(t->k == CT_CLASS && it->t && it->t->k == CT_CLASS &&
          it->t->cls == t->cls) &&
        !(t->k == CT_ARRAY && it->k == E_STR && string_fits(it, t->to)))
        return aggregate_init(t, items, n, idx, at);
    (*idx)++;
    if (ct_is_ref(t))
        return bind_ref(braced && it->na == 1 ? it->a[0] : it, t,
                        "an initializer");
    return init_object(t, braced ? INIT_COPY_LIST : INIT_COPY, &it, 1, at);
}

/* The value a member or element takes when the list says nothing. */
static struct cexpr *value_init_elem(struct cty *t, const struct ctok *at)
{
    if (t->k == CT_CLASS || t->k == CT_ARRAY)
        return init_object(t, INIT_VALUE, NULL, 0, at);
    return NULL;                              /* zero */
}

static struct cexpr *aggregate_init(struct cty *t, struct cexpr **items,
                                    int n, int *idx, const struct ctok *at)
{
    struct cexpr *L = ex_new(E_INITLIST, t, VC_PRVALUE);
    if (t->k == CT_ARRAY) {
        int cap = t->n >= 0 ? (int)t->n : 8, k = 0;
        struct cexpr **el = xcalloc((size_t)(cap ? cap : 1), sizeof *el);
        while (*idx < n && (t->n < 0 || k < t->n)) {
            if (k == cap) {
                cap *= 2;
                el = xrealloc(el, (size_t)cap * sizeof *el);
            }
            el[k++] = init_from_items(t->to, items, n, idx, at);
        }
        long cnt = t->n >= 0 ? t->n : k;
        if (t->n < 0)
            L->t = ct_array(t->to, cnt);
        if (cnt > cap) {
            el = xrealloc(el, (size_t)cnt * sizeof *el);
            cap = (int)cnt;
        }
        for (long i = k; i < cnt; i++)
            el[i] = value_init_elem(t->to, at);
        L->a = el;
        L->na = (int)cnt;
        return L;
    }
    struct cclass *c = t->cls;
    L->a = xcalloc((size_t)(c->nfields ? c->nfields : 1), sizeof *L->a);
    L->na = c->nfields;
    for (int i = 0; i < c->nfields; i++) {
        struct cfield *fl = c->fields[i];
        if (!fl->name)
            continue;                         /* an unnamed bit-field */
        if (c->is_union && i > 0)
            break;
        if (*idx < n) {
            L->a[i] = init_from_items(fl->type, items, n, idx, at);
        } else if (fl->dflt) {
            L->a[i] = fl->dflt;
        } else if (ct_is_ref(fl->type)) {
            cx_error(at, "reference member '%s' is not initialized",
                     fl->name);
        } else {
            L->a[i] = value_init_elem(fl->type, at);
        }
    }
    return L;
}

struct cexpr *init_aggregate(struct cty *t, struct cexpr *list,
                             const struct ctok *at)
{
    int idx = 0;
    struct cexpr *r = aggregate_init(t, list->a, list->na, &idx, at);
    if (idx < list->na)
        ex_error(list->a[idx], "too many initializers for '%s'", ct_name(t));
    return r;
}

struct cexpr *init_object(struct cty *t, enum init_form form,
                          struct cexpr **args, int na, const struct ctok *at)
{
    if (!at)
        at = cx_cur();
    if (t->k == CT_CLASS)
        return construct(t->cls, form, args, na, at);
    if (t->k == CT_ARRAY) {
        struct cty *el = t->to;
        while (el->k == CT_ARRAY)
            el = el->to;
        if (form == INIT_DEFAULT) {
            if (el->k == CT_CLASS &&
                construct(el->cls, INIT_DEFAULT, NULL, 0, at)) {
                struct cexpr *e = ex_new(E_CONSTRUCT, t, VC_PRVALUE);
                e->init = construct(el->cls, INIT_DEFAULT, NULL, 0, at);
                return e;
            }
            return NULL;
        }
        if (form == INIT_VALUE) {
            struct cexpr none = { .k = E_INITLIST };
            return init_aggregate(t, &none, at);
        }
        struct cexpr *a = na ? args[0] : NULL;
        if (a && a->k == E_STR && (form == INIT_COPY || form == INIT_DIRECT) &&
            string_fits(a, t->to))
            return string_init(t, a, at);
        if (a && a->k == E_INITLIST && !a->t) {
            if (a->na == 1 && a->a[0]->k == E_STR &&
                string_fits(a->a[0], t->to))
                return string_init(t, a->a[0], at);
            return init_aggregate(t, a, at);
        }
        cx_error(at, "an array of %s is initialized by a braced list",
                 ct_name(t->to));
    }
    if (ct_is_ref(t))
        cx_error(at, "internal: init_object on a reference");
    if (t->k == CT_VOID)
        cx_error(at, "an object of type void");
    switch (form) {
    case INIT_DEFAULT:
        return NULL;
    case INIT_VALUE:
        return convert(ex_int(0, ct_basic(CT_INT)), t, "value-initialization");
    case INIT_COPY: case INIT_DIRECT:
        if (na != 1)
            cx_error(at, "a %s is initialized by one value", ct_name(t));
        return convert(args[0], t, "initialization");
    case INIT_LIST: case INIT_COPY_LIST: {
        struct cexpr *l = args[0];
        if (l->na == 0)
            return convert(ex_int(0, ct_basic(CT_INT)), t,
                           "value-initialization");
        if (l->na != 1)
            ex_error(l, "too many initializers for '%s'", ct_name(t));
        return convert(l->a[0], t, "initialization");
    }
    }
    return NULL;
}

struct cexpr *parse_braced_list(void)
{
    struct cexpr *L = ex_new(E_INITLIST, NULL, VC_PRVALUE);
    cx_expect(TOK_LBRACE, "'{'");
    int cap = 0;
    while (cx_kind() != TOK_RBRACE) {
        if (cx_kind() == TOK_DOT && cx_kind_at(1) == TOK_IDENT)
            cx_error(cx_cur(), "designated initializers are not supported "
                               "yet (CX7)");
        struct cexpr *e = cx_kind() == TOK_LBRACE ? parse_braced_list()
                                                  : expr_parse_assign();
        if (L->na == cap) {
            cap = cap ? cap * 2 : 8;
            L->a = xrealloc(L->a, (size_t)cap * sizeof *L->a);
        }
        L->a[L->na++] = e;
        if (!cx_accept(TOK_COMMA))
            break;
    }
    cx_expect(TOK_RBRACE, "'}' to close the initializer list");
    return L;
}

/* ---- operators ---- */

static void need_modifiable(struct cexpr *e, const char *what)
{
    if (e->vc != VC_LVALUE)
        ex_error(e, "%s needs an lvalue", what);
    if (e->t->q & CQ_CONST)
        ex_error(e, "%s of a read-only '%s'", what, ct_name(e->t));
    if (e->t->k == CT_ARRAY)
        ex_error(e, "%s of an array", what);
}

static void no_class_operand(struct cexpr *e, const char *op)
{
    if (e->t && (e->t->k == CT_CLASS ||
                 (e->k == E_INITLIST && !e->t)))
        ex_error(e, "operator%s on a class: operator overloading is not "
                    "supported yet (CX2)", op);
}

static struct cty *arith_pair(struct cexpr **l, struct cexpr **r)
{
    struct cty *t = ct_arith_common((*l)->t, (*r)->t);
    if (!ct_same((*l)->t, t))
        *l = ex_cast(*l, t);
    if (!ct_same((*r)->t, t))
        *r = ex_cast(*r, t);
    return t;
}

static int is_obj_ptr(const struct cty *t)
{
    return t->k == CT_PTR && t->to->k != CT_FUNC;
}

static struct cexpr *binop(int op, struct cexpr *l, struct cexpr *r)
{
    struct cexpr *e = ex2(E_BINARY, NULL, VC_PRVALUE, l, r);
    e->op = op;
    return e;
}

/* Compare pointers: bring both to one pointer type (the composite). */
static void pointer_pair(struct cexpr **l, struct cexpr **r)
{
    struct cty *a = (*l)->t, *b = (*r)->t;
    if (is_null_const(*r) || b->k == CT_NULLPTR) {
        *r = ex_cast(*r, a);
        return;
    }
    if (is_null_const(*l) || a->k == CT_NULLPTR) {
        *l = ex_cast(*l, b);
        return;
    }
    if (ct_same_unqual(a->to, b->to))
        return;
    if (a->to->k == CT_VOID) {
        *r = ex_cast(*r, a);
        return;
    }
    if (b->to->k == CT_VOID) {
        *l = ex_cast(*l, b);
        return;
    }
    ex_error(*l, "comparing distinct pointer types '%s' and '%s'",
             ct_name(a), ct_name(b));
}

static int is_ptrish(const struct cexpr *e)
{
    return e->t->k == CT_PTR || e->t->k == CT_NULLPTR || is_null_const(e);
}

static struct cexpr *binary(int op, struct cexpr *l, struct cexpr *r)
{
    const char *sp = "";
    no_class_operand(l, sp);
    no_class_operand(r, sp);
    if (op == TOK_ANDAND || op == TOK_OROR) {
        struct cexpr *e = binop(op, convert_bool(l, "&&/||"),
                                convert_bool(r, "&&/||"));
        e->t = ct_basic(CT_BOOL);
        return e;
    }
    if (op == TOK_DOTSTAR || op == TOK_ARROWSTAR)
        ex_error(l, "pointers to members are not supported yet (CX2)");
    if (op == TOK_SPACESHIP)
        ex_error(l, "operator<=> is not supported yet (CX7)");
    l = rvalue(l);
    r = rvalue(r);
    struct cty *lt = l->t, *rt = r->t;
    int la = ct_is_arith(lt), ra = ct_is_arith(rt);
    int lscoped = lt->k == CT_ENUM && lt->en->scoped;
    int rscoped = rt->k == CT_ENUM && rt->en->scoped;
    switch (op) {
    case TOK_STAR: case TOK_SLASH: case TOK_PERCENT:
    case TOK_AMP: case TOK_PIPE: case TOK_CARET: {
        int integral = op != TOK_STAR && op != TOK_SLASH;
        if (!la || !ra || lscoped || rscoped ||
            (integral && (!ct_is_integer(lt) || !ct_is_integer(rt))))
            ex_error(l, "invalid operands '%s' and '%s' to a binary "
                        "operator", ct_name(lt), ct_name(rt));
        struct cexpr *e = binop(op, l, r);
        e->t = arith_pair(&e->a[0], &e->a[1]);
        return e;
    }
    case TOK_PLUS: case TOK_MINUS: {
        if (la && ra && !lscoped && !rscoped) {
            struct cexpr *e = binop(op, l, r);
            e->t = arith_pair(&e->a[0], &e->a[1]);
            return e;
        }
        if (is_obj_ptr(lt) && ct_is_integer(rt) && !rscoped) {
            struct cexpr *e = binop(op, l, r);
            e->t = lt;
            if (!ct_is_complete(lt->to) && lt->to->k != CT_VOID)
                ex_error(l, "arithmetic on a pointer to incomplete %s",
                         ct_name(lt->to));
            return e;
        }
        if (op == TOK_PLUS && ct_is_integer(lt) && !lscoped &&
            is_obj_ptr(rt)) {
            struct cexpr *e = binop(op, l, r);
            e->t = rt;
            return e;
        }
        if (op == TOK_MINUS && is_obj_ptr(lt) && is_obj_ptr(rt)) {
            if (!ct_same_unqual(lt->to, rt->to))
                ex_error(l, "subtracting pointers to different types");
            struct cexpr *e = binop(op, l, r);
            e->t = ct_ptrdiff_t();
            return e;
        }
        ex_error(l, "invalid operands '%s' and '%s' to %s", ct_name(lt),
                 ct_name(rt), op == TOK_PLUS ? "+" : "-");
        return NULL;
    }
    case TOK_SHL: case TOK_SHR: {
        if (!ct_is_integer(lt) || !ct_is_integer(rt) || lscoped || rscoped)
            ex_error(l, "invalid operands to a shift");
        struct cty *pl = ct_promote(lt), *pr = ct_promote(rt);
        if (!ct_same(pl, lt))
            l = ex_cast(l, pl);
        if (!ct_same(pr, rt))
            r = ex_cast(r, pr);
        struct cexpr *e = binop(op, l, r);
        e->t = pl;
        return e;
    }
    case TOK_EQEQ: case TOK_NEQ: case TOK_LT: case TOK_GT: case TOK_LE:
    case TOK_GE: {
        struct cexpr *e;
        if (la && ra) {
            if ((lscoped || rscoped) && !ct_same_unqual(lt, rt))
                ex_error(l, "comparing a scoped enum with another type");
            e = binop(op, l, r);
            arith_pair(&e->a[0], &e->a[1]);
        } else if (is_ptrish(l) && is_ptrish(r)) {
            if (lt->k == CT_NULLPTR && rt->k == CT_NULLPTR) {
                e = binop(op, ex_cast(l, ct_ptr(ct_basic(CT_VOID))),
                          ex_cast(r, ct_ptr(ct_basic(CT_VOID))));
            } else {
                pointer_pair(&l, &r);
                e = binop(op, l, r);
            }
        } else if ((lt->k == CT_PTR && ct_is_integer(rt) && !is_null_const(r))
                   || (rt->k == CT_PTR && ct_is_integer(lt))) {
            ex_error(l, "comparing a pointer with an integer");
            return NULL;
        } else {
            ex_error(l, "invalid operands '%s' and '%s' to a comparison",
                     ct_name(lt), ct_name(rt));
            return NULL;
        }
        e->t = ct_basic(CT_BOOL);
        return e;
    }
    default:
        ex_error(l, "unsupported binary operator");
        return NULL;
    }
}

static struct cexpr *assign(int op, struct cexpr *l, struct cexpr *r)
{
    if (l->t->k == CT_CLASS) {
        if (op != TOK_ASSIGN)
            ex_error(l, "compound assignment to a class: operator "
                        "overloading is not supported yet (CX2)");
        struct cclass *c = l->t->cls;
        if (c->copy_assign || c->user_copy_assign || c->user_move_assign ||
            !c->trivial_copy)
            ex_error(l, "assigning a '%s' needs its operator=, which is not "
                        "supported yet (CX2)", ct_name(l->t));
        need_modifiable(l, "assignment");
        struct cexpr *v = init_object(ct_unqual(l->t),
                                      r->k == E_INITLIST && !r->t
                                      ? INIT_COPY_LIST : INIT_COPY,
                                      &r, 1, NULL);
        struct cexpr *e = ex2(E_ASSIGN, l->t, VC_LVALUE, l, v);
        e->op = TOK_ASSIGN;
        return e;
    }
    no_class_operand(r, "=");
    need_modifiable(l, "assignment");
    struct cexpr *e;
    if (op == TOK_ASSIGN) {
        struct cexpr *v = r->k == E_INITLIST && !r->t
                          ? init_object(ct_unqual(l->t), INIT_COPY_LIST, &r, 1,
                                        NULL)
                          : convert(r, ct_unqual(l->t), "assignment");
        e = ex2(E_ASSIGN, l->t, VC_LVALUE, l, v);
        e->op = op;
        return e;
    }
    /* compound: the arithmetic as C does it (e.g. int += double
     * computes in double), after checking the operands */
    r = rvalue(r);
    struct cexpr lr = *l;
    lr.vc = VC_PRVALUE;
    int bop;
    switch (op) {
    case TOK_PLUSEQ: bop = TOK_PLUS; break;
    case TOK_MINUSEQ: bop = TOK_MINUS; break;
    case TOK_STAREQ: bop = TOK_STAR; break;
    case TOK_SLASHEQ: bop = TOK_SLASH; break;
    case TOK_PERCENTEQ: bop = TOK_PERCENT; break;
    case TOK_AMPEQ: bop = TOK_AMP; break;
    case TOK_PIPEEQ: bop = TOK_PIPE; break;
    case TOK_CARETEQ: bop = TOK_CARET; break;
    case TOK_SHLEQ: bop = TOK_SHL; break;
    default: bop = TOK_SHR; break;
    }
    struct cexpr *chk = binary(bop, &lr, r);
    if (l->t->k == CT_ENUM)
        ex_error(l, "compound assignment to an enum");
    if (l->t->k == CT_PTR && bop != TOK_PLUS && bop != TOK_MINUS)
        ex_error(l, "invalid compound assignment to a pointer");
    (void)chk;
    if (r->t->k == CT_ENUM || r->t->k == CT_BOOL)
        r = ex_cast(r, ct_promote(r->t));
    e = ex2(E_ASSIGN, l->t, VC_LVALUE, l, r);
    e->op = bop;
    return e;
}

static struct cexpr *incdec(struct cexpr *e, int delta, int post)
{
    no_class_operand(e, delta > 0 ? "++" : "--");
    need_modifiable(e, delta > 0 ? "increment" : "decrement");
    if (e->t->k == CT_BOOL)
        ex_error(e, "%s of a bool", delta > 0 ? "increment" : "decrement");
    if (!(ct_is_arith(e->t) && e->t->k != CT_ENUM) && !is_obj_ptr(e->t))
        ex_error(e, "invalid operand '%s' to %s", ct_name(e->t),
                 delta > 0 ? "++" : "--");
    struct cexpr *r = ex1(E_INCDEC, post ? ct_unqual(e->t) : e->t,
                          post ? VC_PRVALUE : VC_LVALUE, e);
    r->ival = delta;
    r->post = post;
    return r;
}

static struct cexpr *unary(int op, struct cexpr *e)
{
    no_class_operand(e, op == TOK_MINUS ? "-" : op == TOK_PLUS ? "+"
                        : op == TOK_TILDE ? "~" : "!");
    if (op == TOK_BANG) {
        struct cexpr *r = ex1(E_UNARY, ct_basic(CT_BOOL), VC_PRVALUE,
                              convert_bool(e, "!"));
        r->op = op;
        return r;
    }
    e = rvalue(e);
    if (op == TOK_PLUS && e->t->k == CT_PTR)
        return e;
    if (!ct_is_arith(e->t) || (e->t->k == CT_ENUM && e->t->en->scoped) ||
        (op == TOK_TILDE && !ct_is_integer(e->t)))
        ex_error(e, "invalid operand '%s' to a unary operator",
                 ct_name(e->t));
    struct cty *t = ct_promote(e->t);
    if (!ct_same(t, e->t))
        e = ex_cast(e, t);
    struct cexpr *r = ex1(E_UNARY, t, VC_PRVALUE, e);
    r->op = op;
    return r;
}

static struct cexpr *address_of(struct cexpr *e)
{
    if (e->k == E_OVL) {
        if (e->fn->next)
            ex_error(e, "taking the address of an overloaded function "
                        "needs a target type");
        return rvalue(e);
    }
    if (e->t->k == CT_FUNC)
        return rvalue(e);
    if (e->vc != VC_LVALUE)
        ex_error(e, "taking the address of an rvalue");
    if (is_bitfield(e))
        ex_error(e, "taking the address of a bit-field");
    return ex_addr(e);
}

static struct cexpr *deref(struct cexpr *e)
{
    e = rvalue(e);
    if (e->t->k != CT_PTR)
        ex_error(e, "indirection through a non-pointer '%s'", ct_name(e->t));
    if (e->t->to->k == CT_VOID)
        ex_error(e, "indirection through 'void *'");
    return ex_deref(e);
}

static struct cexpr *comma(struct cexpr *l, struct cexpr *r)
{
    return ex2(E_COMMA, r->t, r->vc, l, r);
}

static struct cexpr *conditional(struct cexpr *c, struct cexpr *a,
                                 struct cexpr *b)
{
    c = convert_bool(c, "?:");
    struct cty *at = a->t, *bt = b->t;
    struct cexpr *e = ex_new(E_COND, NULL, VC_PRVALUE);
    e->line = c->line;
    e->file = c->file;
    e->a = xmalloc(3 * sizeof *e->a);
    e->a[0] = c;
    e->na = 3;
    if (at->k == CT_VOID || bt->k == CT_VOID) {
        if (at->k != bt->k)
            ex_error(a, "one arm of ?: is void and the other is not");
        e->a[1] = a;
        e->a[2] = b;
        e->t = ct_basic(CT_VOID);
        return e;
    }
    if (a->vc == b->vc && a->vc != VC_PRVALUE && ct_same(at, bt) &&
        !is_bitfield(a) && !is_bitfield(b)) {
        e->a[1] = a;
        e->a[2] = b;
        e->t = at;
        e->vc = a->vc;
        return e;
    }
    if (at->k == CT_CLASS || bt->k == CT_CLASS) {
        if (!(at->k == CT_CLASS && bt->k == CT_CLASS &&
              at->cls == bt->cls))
            ex_error(a, "?: between '%s' and '%s' is not supported yet (CX2)",
                     ct_name(at), ct_name(bt));
        struct cty *t = ct_unqual(at);
        e->a[1] = init_object(t, INIT_COPY, &a, 1, NULL);
        e->a[2] = init_object(t, INIT_COPY, &b, 1, NULL);
        e->t = t;
        return e;
    }
    a = rvalue(a);
    b = rvalue(b);
    at = a->t;
    bt = b->t;
    if (ct_is_arith(at) && ct_is_arith(bt)) {
        if (ct_same(at, bt)) {
            e->t = at;
        } else {
            e->t = arith_pair(&a, &b);
        }
    } else if (is_ptrish(a) && is_ptrish(b)) {
        if (at->k == CT_NULLPTR && bt->k == CT_NULLPTR) {
            e->t = at;
        } else {
            pointer_pair(&a, &b);
            e->t = a->t->k == CT_PTR ? a->t : b->t;
            if (!ct_same(a->t, e->t))
                a = ex_cast(a, e->t);
            if (!ct_same(b->t, e->t))
                b = ex_cast(b, e->t);
        }
    } else if (ct_same_unqual(at, bt)) {
        e->t = ct_unqual(at);
    } else {
        ex_error(a, "incompatible operands '%s' and '%s' to ?:", ct_name(at),
                 ct_name(bt));
    }
    e->a[1] = a;
    e->a[2] = b;
    return e;
}

/* ---- casts ---- */

enum { CAST_C, CAST_STATIC, CAST_REINTERPRET, CAST_CONST };

static struct cexpr *cast_to(struct cty *t, struct cexpr *e, int kind,
                             const struct ctok *at)
{
    if (ct_is_ref(t)) {
        struct cty *T = t->to;
        int vc = t->k == CT_LREF ? VC_LVALUE : VC_XVALUE;
        if (e->vc == VC_PRVALUE || e->k == E_OVL) {
            /* (const T&)rvalue, static_cast<T&&>(rvalue): a temporary */
            if (kind != CAST_STATIC && kind != CAST_C)
                cx_error(at, "casting an rvalue to a reference");
            struct cexpr *r = ex_deref(bind_ref(e, t, "a cast"));
            r->vc = vc;
            return r;
        }
        /* a glvalue reinterpreted as a T (static_cast<T&&>(x) is
         * std::move) */
        struct cexpr *r = ex1(E_CAST, T, vc, e);
        r->lvcast = 1;
        return r;
    }
    if (t->k == CT_VOID)
        return ex_cast(e, t);
    if (t->k == CT_CLASS)
        return init_object(t, INIT_DIRECT, &e, 1, at);
    if (e->k == E_OVL)
        return convert(e, t, "a cast");
    e = rvalue(e);
    struct cty *f = e->t;
    if (f->k == CT_CLASS)
        cx_error(at, "casting a class to '%s' needs a conversion function "
                     "(CX2)", ct_name(t));
    struct cty *tu = ct_unqual(t);
    if (ct_same(f, tu))
        return e;
    if (kind == CAST_CONST && !(f->k == CT_PTR && tu->k == CT_PTR))
        cx_error(at, "const_cast to a non-pointer type");
    if (kind == CAST_STATIC) {
        /* the reverse of standard conversions, and enum <-> integer */
        int ok = (ct_is_arith(f) && ct_is_arith(tu)) ||
                 (f->k == CT_PTR && tu->k == CT_PTR &&
                  (f->to->k == CT_VOID || tu->to->k == CT_VOID ||
                   ct_same_unqual(f->to, tu->to))) ||
                 (tu->k == CT_PTR && (is_null_const(e) ||
                                      f->k == CT_NULLPTR)) ||
                 (tu->k == CT_BOOL && ct_is_scalar(f));
        if (!ok)
            cx_error(at, "invalid static_cast from '%s' to '%s'", ct_name(f),
                     ct_name(t));
    }
    if (kind == CAST_REINTERPRET && ct_is_arith(f) && ct_is_arith(tu))
        cx_error(at, "reinterpret_cast between arithmetic types");
    return ex_cast(e, tu);
}

/* T(args) or T{args}: a functional cast, or constructing a T. */
static struct cexpr *functional_cast(struct cty *t, const struct ctok *at)
{
    if (cx_kind() == TOK_LBRACE) {
        struct cexpr *l = parse_braced_list();
        struct cexpr *r = init_object(t, INIT_LIST, &l, 1, at);
        if (!r)
            r = init_object(t, INIT_VALUE, NULL, 0, at);
        return r;
    }
    cx_expect(TOK_LPAREN, "'('");
    struct cexpr **args = NULL;
    int na = 0, cap = 0;
    while (cx_kind() != TOK_RPAREN) {
        if (na == cap) {
            cap = cap ? cap * 2 : 4;
            args = xrealloc(args, (size_t)cap * sizeof *args);
        }
        args[na++] = cx_kind() == TOK_LBRACE ? parse_braced_list()
                                             : expr_parse_assign();
        if (!cx_accept(TOK_COMMA))
            break;
    }
    cx_expect(TOK_RPAREN, "')'");
    if (na == 0) {
        if (t->k == CT_VOID)
            return ex_cast(ex_int(0, ct_basic(CT_INT)), t);
        return init_object(t, INIT_VALUE, NULL, 0, at);
    }
    if (na == 1 && t->k != CT_CLASS)
        return cast_to(t, args[0], CAST_C, at);
    if (t->k != CT_CLASS)
        cx_error(at, "a %s is made from one value", ct_name(t));
    return init_object(t, INIT_DIRECT, args, na, at);
}

/* At `(`: is a type-id inside (a cast) rather than an expression? */
static int paren_type_id(void)
{
    int save = cx_pos;
    cx_advance();
    int r = 0;
    if (!at_type_start())
        goto out;
    for (;;) {
        enum tok_kind k = cx_kind();
        int n;
        if (k == TOK_KW_STRUCT || k == TOK_KW_UNION || k == TOK_CX_CLASS ||
            k == TOK_KW_ENUM || k == TOK_CX_TYPENAME) {
            cx_advance();
            struct qname q = peek_qname();
            cx_pos += q.fin + (cx_kind_at(q.fin) == TOK_IDENT);
            continue;
        }
        if (k == TOK_CX_DECLTYPE || k == TOK_KW_TYPEOF) {
            cx_advance();
            if (cx_kind() == TOK_LPAREN)
                cx_skip_balanced();
            continue;
        }
        if (k == TOK_KW_ATTRIBUTE) {
            cx_advance();
            if (cx_kind() == TOK_LPAREN)
                cx_skip_balanced();
            continue;
        }
        if (k != TOK_IDENT && k != TOK_COLONCOLON && at_type_start()) {
            cx_advance();
            continue;
        }
        if ((k == TOK_IDENT || k == TOK_COLONCOLON) && peek_type_name(&n)) {
            cx_pos += n;
            continue;
        }
        break;
    }
    switch (cx_kind()) {
    case TOK_RPAREN: case TOK_STAR: case TOK_AMP: case TOK_ANDAND:
    case TOK_LBRACKET: case TOK_KW_CONST: case TOK_KW_VOLATILE:
        r = 1;
        break;
    case TOK_LPAREN: {
        enum tok_kind k1 = cx_kind_at(1);
        cx_advance();
        r = k1 == TOK_STAR || k1 == TOK_AMP || k1 == TOK_RPAREN ||
            k1 == TOK_ANDAND || at_type_start();
        break;
    }
    default:
        r = 0;
    }
out:
    cx_pos = save;
    return r;
}

/* ---- new and delete ---- */

static struct cexpr *parse_cast(void);

static struct cexpr *size_of(struct cty *t)
{
    return ex_int(ct_size(t), ct_size_t());
}

static struct cexpr *parse_new(const struct ctok *at)
{
    cx_advance();                               /* new */
    struct cexpr **place = NULL;
    int nplace = 0, cap = 0;
    if (cx_kind() == TOK_LPAREN && !paren_type_id()) {
        cx_advance();
        while (cx_kind() != TOK_RPAREN) {
            if (nplace == cap) {
                cap = cap ? cap * 2 : 4;
                place = xrealloc(place, (size_t)cap * sizeof *place);
            }
            place[nplace++] = expr_parse_assign();
            if (!cx_accept(TOK_COMMA))
                break;
        }
        cx_expect(TOK_RPAREN, "')' after the placement arguments");
    }
    struct cty *t;
    struct cexpr *count = NULL;
    if (cx_kind() == TOK_LPAREN) {
        cx_advance();
        t = parse_type_id();
        cx_expect(TOK_RPAREN, "')'");
    } else {
        t = parse_new_type_id();
        if (cx_kind() == TOK_LBRACKET) {
            cx_advance();
            count = expr_parse();
            cx_expect(TOK_RBRACKET, "']'");
            /* further bounds are constants: part of the element type */
            long dims[8];
            int nd = 0;
            while (cx_kind() == TOK_LBRACKET && nd < 8) {
                cx_advance();
                dims[nd++] = expr_parse_const("an array bound");
                cx_expect(TOK_RBRACKET, "']'");
            }
            while (nd > 0)
                t = ct_array(t, dims[--nd]);
        }
    }
    if (t->k == CT_ARRAY && !count) {
        count = ex_int(t->n, ct_size_t());
        t = t->to;
    }
    if (ct_is_ref(t) || t->k == CT_FUNC || t->k == CT_VOID)
        cx_error(at, "cannot allocate a %s with new", ct_name(t));
    if (!ct_is_complete(t))
        cx_error(at, "new of incomplete type %s", ct_name(t));
    /* the initializer */
    enum init_form form = INIT_DEFAULT;
    struct cexpr **args = NULL;
    int na = 0;
    if (cx_kind() == TOK_LPAREN) {
        cx_advance();
        form = INIT_DIRECT;
        int ac = 0;
        while (cx_kind() != TOK_RPAREN) {
            if (na == ac) {
                ac = ac ? ac * 2 : 4;
                args = xrealloc(args, (size_t)ac * sizeof *args);
            }
            args[na++] = cx_kind() == TOK_LBRACE ? parse_braced_list()
                                                 : expr_parse_assign();
            if (!cx_accept(TOK_COMMA))
                break;
        }
        cx_expect(TOK_RPAREN, "')'");
        if (na == 0)
            form = INIT_VALUE;
    } else if (cx_kind() == TOK_LBRACE) {
        form = INIT_LIST;
        args = xmalloc(sizeof *args);
        args[0] = parse_braced_list();
        na = 1;
        if (args[0]->na == 0)
            form = INIT_VALUE;
    }
    struct cexpr *e = ex_new(E_NEW, ct_ptr(t), VC_PRVALUE);
    e->line = at->t.line;
    e->file = at->file;
    e->alloc_t = t;
    e->is_array = count != NULL;
    struct cty *elem = t;
    while (elem->k == CT_ARRAY)
        elem = elem->to;
    if (count) {
        if (form != INIT_DEFAULT && form != INIT_VALUE)
            cx_error(at, "an initializer for new[] is not supported yet");
        e->count = convert(count, ct_size_t(), "an array new's size");
        e->init = init_object(t, form, NULL, 0, at);
        if (elem->k == CT_CLASS && class_dtor(elem->cls))
            e->cookie = ct_align(elem) > 8 ? ct_align(elem) : 8;
    } else {
        e->init = init_object(t, form, args, na, at);
    }
    /* operator new(size, placement...) */
    struct cexpr **oa = xmalloc((size_t)(nplace + 1) * sizeof *oa);
    if (count) {
        struct cexpr *sz = binary(TOK_STAR, e->count, size_of(t));
        if (e->cookie)
            sz = binary(TOK_PLUS, sz, ex_int(e->cookie, ct_size_t()));
        oa[0] = sz;
    } else {
        oa[0] = size_of(t);
    }
    for (int i = 0; i < nplace; i++)
        oa[i + 1] = place[i];
    struct cexpr *call = call_global_op(count ? "operator new[]"
                                              : "operator new",
                                        oa, nplace + 1, at);
    e->fn = call->fn;
    e->a = call->a;
    e->na = call->na;
    return e;
}

static struct cexpr *parse_delete(const struct ctok *at)
{
    cx_advance();                               /* delete */
    int arr = 0;
    if (cx_kind() == TOK_LBRACKET && cx_kind_at(1) == TOK_RBRACKET) {
        cx_advance();
        cx_advance();
        arr = 1;
    }
    struct cexpr *p = rvalue(parse_cast());
    if (p->t->k != CT_PTR || p->t->to->k == CT_FUNC)
        cx_error(at, "delete of a non-pointer '%s'", ct_name(p->t));
    struct cty *t = p->t->to;
    struct cexpr *e = ex_new(E_DELETE, ct_basic(CT_VOID), VC_PRVALUE);
    e->line = at->t.line;
    e->file = at->file;
    e->alloc_t = t;
    e->is_array = arr;
    if (t->k == CT_CLASS) {
        if (!t->cls->complete)
            cx_error(at, "delete of a pointer to incomplete '%s'",
                     ct_name(t));
        e->dtor = class_dtor(t->cls);
        if (arr && e->dtor)
            e->cookie = ct_align(t) > 8 ? ct_align(t) : 8;
    }
    /* g++ calls the sized forms where it knows the size */
    struct cexpr *vp = ex_new(E_NULLPTR, ct_ptr(ct_basic(CT_VOID)),
                              VC_PRVALUE);
    struct cexpr *sz = ex_int(0, ct_size_t());
    struct cexpr *oa[2] = { vp, sz };
    int sized = t->k != CT_VOID && (!arr || e->cookie);
    struct cexpr *call = call_global_op(arr ? "operator delete[]"
                                            : "operator delete",
                                        oa, sized ? 2 : 1, at);
    e->fn = call->fn;
    e->a = xmalloc(sizeof *e->a);
    e->a[0] = p;
    e->na = 1;
    return e;
}

/* ---- builtins ---- */

static struct cty *builtin_type(const char *n)
{
    static const char *const ints[] = {
        "clz", "ctz", "popcount", "parity", "ffs", "clrsb", "clzl", "ctzl",
        "popcountl", "parityl", "ffsl", "clzll", "ctzll", "popcountll",
        "parityll", "ffsll", "abs", "constant_p", "isnan", "isinf",
        "isfinite", "signbit", "isinf_sign", "memcmp", "strcmp", "strncmp",
        "classify_type", "isgreater", "isless", "isunordered", "fpclassify",
        "isnormal", "islessgreater", "isgreaterequal", "islessequal",
    };
    static const char *const voids[] = {
        "unreachable", "trap", "va_start", "va_end", "va_copy", "prefetch",
        "__clear_cache",
    };
    static const char *const ptrs[] = {
        "memcpy", "memmove", "memset", "return_address", "frame_address",
        "alloca", "extract_return_addr",
    };
    static const char *const flts[] = {
        "huge_val", "inf", "nan", "nans", "fabs", "sqrt", "copysign",
        "floor", "ceil", "trunc", "round", "fmin", "fmax", "fma", "ldexp",
        "frexp", "logb", "scalbn", "nextafter", "fmod", "exp", "log",
        "pow", "sin", "cos",
    };
    for (size_t i = 0; i < sizeof ints / sizeof ints[0]; i++)
        if (strcmp(n, ints[i]) == 0)
            return ct_basic(CT_INT);
    for (size_t i = 0; i < sizeof voids / sizeof voids[0]; i++)
        if (strcmp(n, voids[i]) == 0)
            return ct_basic(CT_VOID);
    for (size_t i = 0; i < sizeof ptrs / sizeof ptrs[0]; i++)
        if (strcmp(n, ptrs[i]) == 0)
            return ct_ptr(ct_basic(CT_VOID));
    if (strcmp(n, "expect") == 0 || strcmp(n, "labs") == 0)
        return ct_basic(CT_LONG);
    if (strcmp(n, "llabs") == 0)
        return ct_basic(CT_LLONG);
    if (strcmp(n, "bswap16") == 0)
        return ct_basic(CT_USHORT);
    if (strcmp(n, "bswap32") == 0)
        return ct_basic(CT_UINT);
    if (strcmp(n, "bswap64") == 0)
        return ct_basic(CT_ULONG);
    if (strcmp(n, "strlen") == 0 || strcmp(n, "object_size") == 0)
        return ct_size_t();
    size_t l = strlen(n);
    for (size_t i = 0; i < sizeof flts / sizeof flts[0]; i++) {
        size_t fl = strlen(flts[i]);
        if (strncmp(n, flts[i], fl) != 0)
            continue;
        if (l == fl)
            return ct_basic(CT_DOUBLE);
        if (l == fl + 1 && n[fl] == 'f')
            return ct_basic(CT_FLOAT);
        if (l == fl + 1 && n[fl] == 'l')
            return ct_basic(CT_LDOUBLE);
    }
    return NULL;
}

/* ( arguments ): the count, and the arguments in *out. */
static int call_args(struct cexpr ***out)
{
    struct cexpr **args = NULL;
    int na = 0, cap = 0;
    cx_expect(TOK_LPAREN, "'('");
    while (cx_kind() != TOK_RPAREN) {
        if (na == cap) {
            cap = cap ? cap * 2 : 4;
            args = xrealloc(args, (size_t)cap * sizeof *args);
        }
        args[na++] = cx_kind() == TOK_LBRACE ? parse_braced_list()
                                             : expr_parse_assign();
        if (!cx_accept(TOK_COMMA))
            break;
    }
    cx_expect(TOK_RPAREN, "')' to close the arguments");
    *out = args;
    return na;
}

static struct cexpr *parse_builtin(const char *name, const struct ctok *at)
{
    const char *n = name + 10;
    if (strcmp(n, "va_arg") == 0) {
        cx_expect(TOK_LPAREN, "'('");
        struct cexpr *ap = expr_parse_assign();
        cx_expect(TOK_COMMA, "','");
        struct cty *t = parse_type_id();
        cx_expect(TOK_RPAREN, "')'");
        struct cexpr *e = ex1(E_VAARG, ct_unqual(t), VC_PRVALUE, ap);
        return e;
    }
    if (strcmp(n, "offsetof") == 0) {
        cx_expect(TOK_LPAREN, "'('");
        struct cty *t = parse_type_id();
        cx_expect(TOK_COMMA, "','");
        long off = 0;
        for (;;) {
            if (t->k != CT_CLASS || cx_kind() != TOK_IDENT)
                cx_error(at, "bad __builtin_offsetof");
            struct cfield *fl = class_find_field(t->cls, cx_cur()->t.text);
            if (!fl)
                cx_error(cx_cur(), "no member '%s' in '%s'", cx_cur()->t.text,
                         ct_name(t));
            cx_advance();
            off += fl->off;
            t = fl->type;
            while (cx_kind() == TOK_LBRACKET) {
                cx_advance();
                long i = expr_parse_const("an index");
                cx_expect(TOK_RBRACKET, "']'");
                if (t->k != CT_ARRAY)
                    cx_error(at, "indexing a non-array in offsetof");
                t = t->to;
                off += i * ct_size(t);
            }
            if (!cx_accept(TOK_DOT))
                break;
        }
        cx_expect(TOK_RPAREN, "')'");
        return ex_int(off, ct_size_t());
    }
    if (strcmp(n, "is_constant_evaluated") == 0) {
        cx_expect(TOK_LPAREN, "'('");
        cx_expect(TOK_RPAREN, "')'");
        return ex_int(0, ct_basic(CT_BOOL));
    }
    struct cexpr **args;
    int na = call_args(&args);
    if (strcmp(n, "addressof") == 0 && na == 1)
        return address_of(args[0]);
    if (strcmp(n, "launder") == 0 && na == 1)
        return rvalue(args[0]);
    if (strcmp(n, "expect") == 0 && na == 2)
        return convert(args[0], ct_basic(CT_LONG), "__builtin_expect");
    if (strcmp(n, "constant_p") == 0 && na == 1) {
        long v;
        return ex_int(expr_const(args[0], &v), ct_basic(CT_INT));
    }
    struct cty *t = builtin_type(n);
    if (!t)
        cx_error(at, "'%s' is not a supported builtin", name);
    struct cexpr *e = ex_new(E_BUILTIN, t, VC_PRVALUE);
    e->name = name;
    e->line = at->t.line;
    e->file = at->file;
    e->a = xmalloc((size_t)(na ? na : 1) * sizeof *e->a);
    for (int i = 0; i < na; i++) {
        struct cexpr *a = args[i];
        if (strncmp(n, "va_", 3) == 0 && i == 0)
            e->a[i] = a;                      /* the va_list itself */
        else
            e->a[i] = rvalue(a);
        if (e->a[i]->t && e->a[i]->t->k == CT_CLASS)
            cx_error(at, "a class passed to '%s'", name);
    }
    e->na = na;
    return e;
}

/* ---- primary and postfix expressions ---- */

static struct cexpr *member_of(struct cexpr *obj, struct cfield *fl)
{
    struct cexpr *e;
    if (ct_is_ref(fl->type)) {
        e = ex1(E_MEMBER, fl->type->to, VC_LVALUE, obj);
    } else {
        unsigned q = obj->t->q;
        if (fl->is_mutable)
            q &= ~(unsigned)CQ_CONST;
        e = ex1(E_MEMBER, ct_qual(fl->type, q),
                obj->vc == VC_LVALUE ? VC_LVALUE
                : obj->vc == VC_XVALUE ? VC_XVALUE : VC_PRVALUE, obj);
    }
    e->field = fl;
    return e;
}

/* An unqualified or qualified name, looked up (after the name was read). */
static struct cexpr *name_expr(struct csym *y, const char *name,
                               const struct ctok *at)
{
    switch (y->k) {
    case CS_VAR: {
        struct cvar *v = y->var;
        if (v->is_local && !v->is_static && v->fn != cx_curfn &&
            !(v->fn == NULL))
            cx_error(at, "'%s' is a local of another function", name);
        struct cexpr *e = ex_new(E_VAR, ct_strip_ref(v->type), VC_LVALUE);
        e->var = v;
        e->line = at->t.line;
        e->file = at->file;
        v->used = 1;
        return e;
    }
    case CS_FIELD: {
        struct cclass *c = y->scope->cls;
        if (!cx_curfn || !cx_curfn->this_var || cx_curfn->cls != c)
            cx_error(at, "member '%s' used without an object", name);
        return member_of(ex_deref(ex_this()), y->field);
    }
    case CS_FUNC: {
        struct cexpr *e = ex_new(E_OVL, y->fns->type, VC_LVALUE);
        e->fn = y->fns;
        e->line = at->t.line;
        e->file = at->file;
        if (y->scope->k == SC_CLASS && cx_curfn && cx_curfn->this_var &&
            cx_curfn->cls == y->scope->cls)
            e->obj = ex_this();
        return e;
    }
    case CS_ENUMERATOR: {
        struct cexpr *e = ex_int(y->value, y->type);
        e->line = at->t.line;
        e->file = at->file;
        return e;
    }
    case CS_NAMESPACE:
        cx_error(at, "namespace '%s' used as a value", name);
        return NULL;
    default:
        cx_error(at, "type '%s' used as a value", name);
        return NULL;
    }
}

static struct cexpr *parse_string(void)
{
    const struct ctok *at = cx_cur();
    int width = at->t.str_width, prefix = at->t.str_prefix;
    size_t n = 0, cap = 0;
    struct litch *lc = NULL;
    while (cx_kind() == TOK_STR) {
        struct token *st = &cx_cur()->t;
        if (st->str_prefix) {
            if (prefix && prefix != st->str_prefix)
                cx_error(cx_cur(), "concatenating differently-prefixed "
                                   "string literals");
            prefix = st->str_prefix;
            width = st->str_width;
        }
        if (n + (size_t)st->nlit > cap) {
            cap = (n + (size_t)st->nlit) * 2 + 8;
            lc = xrealloc(lc, cap * sizeof *lc);
        }
        memcpy(lc + n, st->lit, (size_t)st->nlit * sizeof *lc);
        n += (size_t)st->nlit;
        cx_advance();
    }
    enum cty_kind ek = prefix == 'L' ? CT_WCHAR : prefix == 'u' ? CT_CHAR16
                     : prefix == 'U' ? CT_CHAR32 : prefix == '8' ? CT_CHAR8
                     : CT_CHAR;
    long units;
    char *bytes = lit_encode(lc, (int)n, width, &units, at->file, at->t.line);
    free(lc);
    struct cexpr *e = ex_new(E_STR, ct_array(ct_qual(ct_basic(ek), CQ_CONST),
                                             units), VC_LVALUE);
    e->text = bytes;
    e->slen = units;
    e->swidth = width;
    e->line = at->t.line;
    e->file = at->file;
    return e;
}

static struct cexpr *parse_postfix(struct cexpr *e);

static struct cexpr *parse_primary(void)
{
    const struct ctok *at = cx_cur();
    struct cexpr *e;
    switch (cx_kind()) {
    case TOK_NUM: {
        const struct token *t = &at->t;
        enum cty_kind k;
        if (t->char_lit)
            k = t->str_prefix == 'L' ? CT_WCHAR : t->str_prefix == 'u'
                ? CT_CHAR16 : t->str_prefix == 'U' ? CT_CHAR32 : CT_CHAR;
        else if (t->num_llong)
            k = t->num_uns ? CT_ULLONG : CT_LLONG;
        else if (t->num_long)
            k = t->num_uns ? CT_ULONG : CT_LONG;
        else
            k = t->num_uns ? CT_UINT : CT_INT;
        e = ex_int(t->num, ct_basic(k));
        e->is_null_const = !t->char_lit && t->num == 0;
        cx_advance();
        return e;
    }
    case TOK_FNUM: {
        const struct token *t = &at->t;
        if (t->fnum_is_imag)
            cx_error(at, "imaginary constants are not supported in C++");
        e = ex_new(E_FLT, ct_basic(t->fnum_is_ld ? CT_LDOUBLE
                                   : t->fnum_is_float ? CT_FLOAT
                                   : CT_DOUBLE), VC_PRVALUE);
        e->fval = t->fnum;
        e->text = t->text;
        cx_advance();
        return e;
    }
    case TOK_STR:
        return parse_string();
    case TOK_CX_TRUE: case TOK_CX_FALSE:
        e = ex_int(cx_kind() == TOK_CX_TRUE, ct_basic(CT_BOOL));
        cx_advance();
        return e;
    case TOK_CX_NULLPTR:
        e = ex_new(E_NULLPTR, ct_basic(CT_NULLPTR), VC_PRVALUE);
        cx_advance();
        return e;
    case TOK_CX_THIS:
        cx_advance();
        return ex_this();
    case TOK_LPAREN:
        if (cx_kind_at(1) == TOK_LBRACE) {
            /* GNU statement expression */
            cx_advance();
            struct cstmt *b = parse_compound();
            cx_expect(TOK_RPAREN, "')' to close a statement expression");
            struct cstmt *last = b->body;
            while (last && last->next)
                last = last->next;
            struct cty *t = last && last->k == S_EXPR ? last->e->t
                                                      : ct_basic(CT_VOID);
            if (t->k == CT_CLASS)
                cx_error(at, "a statement expression of class type");
            e = ex_new(E_STMTEXPR, ct_unqual(t), VC_PRVALUE);
            e->body = b;
            return e;
        }
        cx_advance();
        e = expr_parse();
        cx_expect(TOK_RPAREN, "')'");
        e->paren = 1;
        return e;
    case TOK_CX_STATIC_CAST: case TOK_CX_REINTERPRET_CAST:
    case TOK_CX_CONST_CAST: case TOK_CX_DYNAMIC_CAST: {
        enum tok_kind k = cx_kind();
        if (k == TOK_CX_DYNAMIC_CAST)
            cx_error(at, "dynamic_cast is not supported yet (CX3)");
        cx_advance();
        cx_expect(TOK_LT, "'<' after the cast");
        struct cty *t = parse_type_id();
        cx_expect(TOK_GT, "'>' after the cast's type");
        cx_expect(TOK_LPAREN, "'('");
        struct cexpr *x = expr_parse();
        cx_expect(TOK_RPAREN, "')'");
        return cast_to(t, x, k == TOK_CX_STATIC_CAST ? CAST_STATIC
                             : k == TOK_CX_CONST_CAST ? CAST_CONST
                             : CAST_REINTERPRET, at);
    }
    case TOK_CX_TYPEID:
        cx_error(at, "typeid is not supported yet (CX3)");
        return NULL;
    case TOK_CX_THROW:
        cx_error(at, "exceptions are not supported yet (CX5)");
        return NULL;
    case TOK_LBRACKET:
        cx_error(at, "lambdas are not supported yet (CX6)");
        return NULL;
    case TOK_CX_REQUIRES:
        cx_error(at, "requires-expressions are not supported yet (CX7)");
        return NULL;
    case TOK_IDENT: case TOK_COLONCOLON: case TOK_CX_OPERATOR: {
        int n;
        struct cty *t = peek_type_name(&n);
        if (t && (cx_kind_at(n) == TOK_LPAREN || cx_kind_at(n) == TOK_LBRACE)) {
            cx_pos += n;
            return functional_cast(t, at);
        }
        struct qname q = peek_qname();
        if (q.bad)
            cx_error(cx_peek(q.fin), "'%s' is not a namespace or class",
                     cx_peek(q.fin)->t.text);
        cx_pos += q.fin;
        const char *name;
        if (cx_kind() == TOK_CX_OPERATOR) {
            struct cty *conv = NULL;
            name = parse_operator_name(&conv);
            if (conv)
                cx_error(at, "conversion functions are not supported yet "
                             "(CX2)");
        } else if (cx_kind() == TOK_IDENT) {
            name = cx_cur()->t.text;
            cx_advance();
        } else {
            cx_error(cx_cur(), "expected a name");
            return NULL;
        }
        struct csym *y = q.scope ? lookup_in(q.scope, name)
                                 : lookup(cx_scope, name);
        if (!y) {
            if (!q.scope && strncmp(name, "__builtin_", 10) == 0)
                return parse_builtin(name, at);
            if (!q.scope && strcmp(name, "__null") == 0) {
                e = ex_int(0, ct_basic(CT_LONG));
                e->is_null_const = 1;
                return e;
            }
            cx_error(at, "'%s' was not declared in this scope", name);
        }
        return name_expr(y, name, at);
    }
    default:
        if (at_simple_type_kw()) {
            struct cty *t = parse_simple_type_spec();
            return functional_cast(t, at);
        }
        cx_error(at, "expected an expression before %s",
                 tok_describe(&at->t));
        return NULL;
    }
}

static struct cexpr *call(struct cexpr *f, struct cexpr **args, int na,
                          const struct ctok *at)
{
    if (f->k == E_OVL) {
        struct cfunc *fn = resolve(f->fn, f->obj, args, na, at, f->fn->name);
        return make_call(fn, fn->cls && !fn->is_static ? f->obj : NULL, args,
                         na, at);
    }
    if (f->t->k == CT_CLASS)
        cx_error(at, "calling an object: operator() is not supported yet "
                     "(CX2)");
    struct cexpr *p = rvalue(f);
    if (p->t->k != CT_PTR || p->t->to->k != CT_FUNC)
        cx_error(at, "called object of type '%s' is not a function",
                 ct_name(f->t));
    struct cty *ft = p->t->to;
    if (na < ft->np || (na > ft->np && !ft->variadic))
        cx_error(at, "wrong number of arguments to a function pointer");
    struct cexpr **conv;
    int n = convert_args_ft(ft, NULL, args, na, at, &conv);
    struct cexpr *e = call_result(ft->to);
    e->k = E_ICALL;
    e->line = at->t.line;
    e->file = at->file;
    e->a = xmalloc((size_t)(n + 1) * sizeof *e->a);
    e->a[0] = p;
    memcpy(e->a + 1, conv, (size_t)n * sizeof *conv);
    e->na = n + 1;
    return e;
}

static struct cexpr *member_access(struct cexpr *obj, int arrow,
                                   const struct ctok *at)
{
    if (arrow) {
        obj = rvalue(obj);
        if (obj->t->k == CT_CLASS)
            cx_error(at, "operator-> on a class is not supported yet (CX2)");
        if (obj->t->k != CT_PTR || obj->t->to->k != CT_CLASS)
            cx_error(at, "'->' on a non-pointer-to-class '%s'",
                     ct_name(obj->t));
        obj = ex_deref(obj);
    }
    if (obj->t->k != CT_CLASS)
        cx_error(at, "request for a member of non-class type '%s'",
                 ct_name(obj->t));
    struct cclass *c = obj->t->cls;
    if (!c->complete && !c->defining)
        cx_error(at, "member access into incomplete '%s'", ct_name(obj->t));
    if (obj->vc == VC_PRVALUE)
        obj = materialize(obj);
    if (cx_kind() == TOK_CX_TEMPLATE)
        cx_error(cx_cur(), "'.template' is not supported yet (CX4)");
    if (cx_kind() == TOK_TILDE) {
        /* an explicit destructor call: p->~T() */
        cx_advance();
        if (cx_kind() != TOK_IDENT)
            cx_error(cx_cur(), "expected a class name after '~'");
        cx_advance();
        cx_expect(TOK_LPAREN, "'('");
        cx_expect(TOK_RPAREN, "')'");
        struct cfunc *d = class_dtor(c);
        if (!d)
            return ex_cast(ex_int(0, ct_basic(CT_INT)), ct_basic(CT_VOID));
        return make_call(d, ex_addr(obj), NULL, 0, at);
    }
    struct qname q = peek_qname();
    if (q.fin > 0)
        cx_error(at, "qualified member access is not supported yet (CX3)");
    const char *name;
    if (cx_kind() == TOK_CX_OPERATOR) {
        struct cty *conv = NULL;
        name = parse_operator_name(&conv);
    } else {
        if (cx_kind() != TOK_IDENT)
            cx_error(cx_cur(), "expected a member name");
        name = cx_cur()->t.text;
        cx_advance();
    }
    struct csym *y = scope_find_here(c->scope, name);
    if (!y)
        cx_error(at, "'%s' has no member named '%s'", ct_name(obj->t), name);
    switch (y->k) {
    case CS_FIELD:
        return member_of(obj, y->field);
    case CS_VAR: {
        struct cexpr *e = ex_new(E_VAR, ct_strip_ref(y->var->type),
                                 VC_LVALUE);
        e->var = y->var;
        y->var->used = 1;
        return e;
    }
    case CS_FUNC: {
        struct cexpr *e = ex_new(E_OVL, y->fns->type, VC_LVALUE);
        e->fn = y->fns;
        e->obj = ex_addr(obj);
        return e;
    }
    case CS_ENUMERATOR:
        return ex_int(y->value, y->type);
    default:
        cx_error(at, "'%s' is not a data member or member function", name);
        return NULL;
    }
}

static struct cexpr *parse_postfix(struct cexpr *e)
{
    for (;;) {
        const struct ctok *at = cx_cur();
        switch (cx_kind()) {
        case TOK_LBRACKET: {
            if (cx_kind_at(1) == TOK_LBRACKET)
                return e;
            cx_advance();
            struct cexpr *i = expr_parse();
            cx_expect(TOK_RBRACKET, "']'");
            no_class_operand(e, "[]");
            struct cexpr *a = rvalue(e), *b = rvalue(i);
            if (b->t->k == CT_PTR && ct_is_integer(a->t)) {
                struct cexpr *t = a;
                a = b;
                b = t;
            }
            if (!is_obj_ptr(a->t) || !ct_is_integer(b->t))
                cx_error(at, "subscript of '%s' with '%s'", ct_name(e->t),
                         ct_name(i->t));
            e = ex_deref(binary(TOK_PLUS, a, b));
            continue;
        }
        case TOK_LPAREN: {
            struct cexpr **args;
            int na = call_args(&args);
            e = call(e, args, na, at);
            continue;
        }
        case TOK_DOT: case TOK_ARROW: {
            int arrow = cx_kind() == TOK_ARROW;
            cx_advance();
            e = member_access(e, arrow, at);
            continue;
        }
        case TOK_PLUSPLUS: case TOK_MINUSMINUS:
            e = incdec(e, cx_kind() == TOK_PLUSPLUS ? 1 : -1, 1);
            cx_advance();
            continue;
        default:
            return e;
        }
    }
}

static struct cexpr *parse_unary(void)
{
    const struct ctok *at = cx_cur();
    enum tok_kind k = cx_kind();
    switch (k) {
    case TOK_PLUSPLUS: case TOK_MINUSMINUS:
        cx_advance();
        return incdec(parse_cast(), k == TOK_PLUSPLUS ? 1 : -1, 0);
    case TOK_AMP:
        cx_advance();
        return address_of(parse_cast());
    case TOK_STAR:
        cx_advance();
        return deref(parse_cast());
    case TOK_PLUS: case TOK_MINUS: case TOK_TILDE: case TOK_BANG:
        cx_advance();
        return unary(k, parse_cast());
    case TOK_KW_SIZEOF: case TOK_KW_ALIGNOF: {
        cx_advance();
        if (cx_kind() == TOK_ELLIPSIS)
            cx_error(at, "sizeof... is not supported yet (CX4)");
        struct cty *t;
        if (cx_kind() == TOK_LPAREN && paren_type_id()) {
            cx_advance();
            t = parse_type_id();
            cx_expect(TOK_RPAREN, "')'");
        } else {
            t = parse_unary()->t;
        }
        t = ct_strip_ref(t);
        if (t->k == CT_FUNC || (!ct_is_complete(t) && t->k != CT_VOID))
            cx_error(at, "%s of incomplete type %s",
                     k == TOK_KW_SIZEOF ? "sizeof" : "alignof", ct_name(t));
        return ex_int(k == TOK_KW_SIZEOF ? ct_size(t) : ct_align(t),
                      ct_size_t());
    }
    case TOK_CX_NEW:
        return parse_new(at);
    case TOK_CX_DELETE:
        return parse_delete(at);
    case TOK_COLONCOLON:
        if (cx_kind_at(1) == TOK_CX_NEW) {
            cx_advance();
            return parse_new(cx_cur());
        }
        if (cx_kind_at(1) == TOK_CX_DELETE) {
            cx_advance();
            return parse_delete(cx_cur());
        }
        break;
    case TOK_CX_NOEXCEPT: {
        cx_advance();
        cx_expect(TOK_LPAREN, "'(' after noexcept");
        expr_parse();
        cx_expect(TOK_RPAREN, "')'");
        return ex_int(0, ct_basic(CT_BOOL));
    }
    case TOK_KW_REAL: case TOK_KW_IMAG:
        cx_error(at, "__real__/__imag__ are not supported in C++ yet");
        return NULL;
    case TOK_ANDAND:
        cx_error(at, "label addresses are not supported in C++");
        return NULL;
    case TOK_CX_CO_AWAIT:
        cx_error(at, "coroutines are not supported yet (CX7)");
        return NULL;
    default:
        break;
    }
    return parse_postfix(parse_primary());
}

static struct cexpr *parse_cast(void)
{
    if (cx_kind() == TOK_LPAREN && cx_kind_at(1) != TOK_LBRACE &&
        paren_type_id()) {
        const struct ctok *at = cx_cur();
        cx_advance();
        struct cty *t = parse_type_id();
        cx_expect(TOK_RPAREN, "')' after the cast's type");
        if (cx_kind() == TOK_LBRACE) {
            /* (T){...}: a GNU compound literal, as T{...} */
            struct cexpr *l = parse_braced_list();
            struct cexpr *r = init_object(t, INIT_LIST, &l, 1, at);
            if (!r)
                r = init_object(t, INIT_VALUE, NULL, 0, at);
            if (t->k == CT_ARRAY)
                r = materialize(r);
            return parse_postfix(r);
        }
        return cast_to(t, parse_cast(), CAST_C, at);
    }
    return parse_unary();
}

static int binprec(enum tok_kind k)
{
    switch (k) {
    case TOK_OROR: return 1;
    case TOK_ANDAND: return 2;
    case TOK_PIPE: return 3;
    case TOK_CARET: return 4;
    case TOK_AMP: return 5;
    case TOK_EQEQ: case TOK_NEQ: return 6;
    case TOK_LT: case TOK_GT: case TOK_LE: case TOK_GE: return 7;
    case TOK_SPACESHIP: return 8;
    case TOK_SHL: case TOK_SHR: return 9;
    case TOK_PLUS: case TOK_MINUS: return 10;
    case TOK_STAR: case TOK_SLASH: case TOK_PERCENT: return 11;
    case TOK_DOTSTAR: case TOK_ARROWSTAR: return 12;
    default: return 0;
    }
}

static struct cexpr *parse_binary(int minprec)
{
    struct cexpr *l = parse_cast();
    for (;;) {
        enum tok_kind k = cx_kind();
        int p = binprec(k);
        if (!p || p < minprec)
            return l;
        cx_advance();
        struct cexpr *r = parse_binary(p + 1);
        l = binary(k, l, r);
    }
}

struct cexpr *expr_parse_cond(void)
{
    struct cexpr *c = parse_binary(1);
    if (!cx_accept(TOK_QUESTION))
        return c;
    struct cexpr *a = expr_parse();
    cx_expect(TOK_COLON, "':' in ?:");
    struct cexpr *b = expr_parse_assign();
    return conditional(c, a, b);
}

static int is_assign_op(enum tok_kind k)
{
    switch (k) {
    case TOK_ASSIGN: case TOK_PLUSEQ: case TOK_MINUSEQ: case TOK_STAREQ:
    case TOK_SLASHEQ: case TOK_PERCENTEQ: case TOK_AMPEQ: case TOK_PIPEEQ:
    case TOK_CARETEQ: case TOK_SHLEQ: case TOK_SHREQ:
        return 1;
    default:
        return 0;
    }
}

struct cexpr *expr_parse_assign(void)
{
    if (cx_kind() == TOK_CX_THROW)
        cx_error(cx_cur(), "exceptions are not supported yet (CX5)");
    struct cexpr *l = expr_parse_cond();
    enum tok_kind k = cx_kind();
    if (!is_assign_op(k))
        return l;
    cx_advance();
    struct cexpr *r = cx_kind() == TOK_LBRACE ? parse_braced_list()
                                              : expr_parse_assign();
    return assign(k, l, r);
}

struct cexpr *expr_parse(void)
{
    struct cexpr *e = expr_parse_assign();
    while (cx_accept(TOK_COMMA))
        e = comma(e, expr_parse_assign());
    return e;
}

long expr_parse_const(const char *what)
{
    const struct ctok *at = cx_cur();
    struct cexpr *e = expr_parse_cond();
    long v = 0;
    if (!ct_is_integer(e->t) || !expr_const(e, &v))
        cx_error(at, "%s is not an integral constant expression", what);
    return v;
}
