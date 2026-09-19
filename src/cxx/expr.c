/* Expressions: parsed and typed together. Every operand's conversion is
 * made explicit here (E_CAST, reference binding as an address, a
 * temporary's materialization), so emit.c only spells the tree in C.
 *
 * Also here: implicit conversion sequences and their ranking — overload
 * resolution (12.2) — and initialization (9.4), which declarations,
 * arguments, returns and new-expressions all share. */
#include "cxx.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../driver/util.h"

static struct cexpr *parse_throw(void);

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
    if (cx_sfinae)                  /* a substitution failure, not an error */
        longjmp(*(jmp_buf *)cx_sfinae, 1);
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

struct cexpr *ex_materialize(struct cexpr *e)
{
    return materialize(e);
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
                ex_error(e, "a member function is called, or its address "
                            "taken as &%s::%s", e->fn->cls->name,
                         e->fn->name);
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
    struct cclass *base_to;   /* a derived-to-base conversion, to this */
    struct cfunc *user;       /* the user-defined conversion */
};

/* ---- bases ---- */

static int path_count(struct cclass *d, struct cclass *b)
{
    if (d == b)
        return 1;
    class_ensure(d);
    if (d->nvbases) {
        /* a virtual base is one subobject however it is reached */
        struct cclass *vb;
        long off;
        return class_base_path(d, b, &vb, &off);
    }
    int n = 0;
    for (int i = 0; i < d->nbases; i++)
        n += path_count(d->bases[i].cls, b);
    return n;
}

/* The offset of the (first) b subobject in d, or -1 (or reached through a
 * virtual base: not fixed). */
static long base_offset(struct cclass *d, struct cclass *b)
{
    if (d == b)
        return 0;
    if (d->nvbases) {
        struct cclass *vb;
        long off;
        if (class_base_path(d, b, &vb, &off) != 1 || vb)
            return -1;
        return off;
    }
    for (int i = 0; i < d->nbases; i++) {
        long o = base_offset(d->bases[i].cls, b);
        if (o >= 0)
            return d->bases[i].off + o;
    }
    return -1;
}

int class_derives(struct cclass *d, struct cclass *b, int *ambiguous)
{
    int n = path_count(d, b);
    if (ambiguous)
        *ambiguous = n > 1;
    return n > 0;
}

/* d is a proper, unambiguous base-of relation away from b. */
static int is_proper_base(struct cclass *d, struct cclass *b)
{
    return d != b && path_count(d, b) == 1;
}

static struct cexpr *shift_object(struct cexpr *e, struct cclass *to,
                                  long off, int ptr)
{
    unsigned q = ptr ? e->t->to->q : e->t->q;
    struct cty *bt = ct_qual(ct_class(to), q);
    struct cexpr *r = ex_new(E_BASE, ptr ? ct_ptr(bt) : bt,
                             ptr ? VC_PRVALUE : e->vc);
    r->a = xmalloc(sizeof *r->a);
    r->a[0] = e;
    r->na = 1;
    r->ival = off;
    r->is_array = ptr;
    r->line = e->line;
    r->file = e->file;
    return r;
}

struct cexpr *to_base(struct cexpr *e, struct cclass *b, int ptr)
{
    struct cclass *d = ptr ? e->t->to->cls : e->t->cls;
    if (d == b)
        return e;
    int n = path_count(d, b);
    if (n == 0)
        ex_error(e, "'%s' is not a base of '%s'", b->name, d->name);
    if (n > 1)
        ex_error(e, "'%s' is an ambiguous base of '%s'", b->name, d->name);
    if (d->nvbases) {
        struct cclass *vb;
        long off;
        class_base_path(d, b, &vb, &off);
        struct cexpr *r = shift_object(e, b, off, ptr);
        if (vb)
            r->vbindex = class_vbindex(d, vb);
        return r;
    }
    return shift_object(e, b, base_offset(d, b), ptr);
}

/* The object (or pointer) of base class b converted to its derived d:
 * static_cast's downcast. */
static struct cexpr *to_derived(struct cexpr *e, struct cclass *d, int ptr)
{
    struct cclass *b = ptr ? e->t->to->cls : e->t->cls;
    if (!is_proper_base(d, b))
        ex_error(e, "'%s' is not an unambiguous base of '%s'", b->name,
                 d->name);
    if (base_offset(d, b) < 0)
        ex_error(e, "'%s' is a virtual base of '%s': a cast down from it "
                    "needs dynamic_cast", b->name, d->name);
    return shift_object(e, d, -base_offset(d, b), ptr);
}

static struct ics ics_of(struct cexpr *e, struct cty *to);

/* A pointer to member of base B as one of derived class t->cls: the
 * base's offset added (not through a virtual base: no fixed offset). */
static struct cexpr *mptr_to_derived(struct cexpr *e, struct cty *t)
{
    long off = base_offset(t->cls, e->t->cls);
    if (off < 0)
        ex_error(e, "'%s' converted to '%s' through a virtual base",
                 ct_name(e->t), ct_name(t));
    if (!off)
        return ex_cast(e, t);
    struct cexpr *r = ex1(E_MPCONV, ct_unqual(t), VC_PRVALUE, e);
    r->ival = off;
    return r;
}

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
    if (e->k == E_OVL && e->memptr) {
        if (ct_is_pmf(tu))
            for (struct cfunc *f = e->fn; f; f = f->next)
                if (f->cls == tu->cls && ct_same_unqual(f->type, tu->to))
                    r.rank = R_EXACT;
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
        } else if (from->k == CT_PTR || from->k == CT_NULLPTR ||
                   from->k == CT_MPTR) {
            r.rank = R_CONV;
            r.ptr_bool = 1;
        }
        return r;
    }
    if (tu->k == CT_MPTR) {
        if (is_null_const(e) || from->k == CT_NULLPTR)
            r.rank = R_CONV;
        else if (from->k == CT_MPTR && from->cls == tu->cls &&
                 ct_same_unqual(from->to, tu->to))
            r.rank = R_EXACT;
        else if (from->k == CT_MPTR && ct_same_unqual(from->to, tu->to)) {
            /* B::* to D::*, B a base of D (7.3.13) */
            int amb;
            if (class_derives(tu->cls, from->cls, &amb) && !amb)
                r.rank = R_CONV;
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
        if (pf->k == CT_CLASS && pt->k == CT_CLASS &&
            is_proper_base(pf->cls, pt->cls)) {
            r.rank = R_CONV;
            r.base_to = pt->cls;
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
    int related = ct_same_unqual(e->t, T);
    int derived = !related && e->t->k == CT_CLASS && T->k == CT_CLASS &&
                  is_proper_base(e->t->cls, T->cls);
    if (derived) {
        /* binding to a base: a derived-to-base Conversion (12.2.4.2) */
        related = 1;
        r.base_to = T->cls;
    }
    int compat = related && (T->q & e->t->q) == e->t->q;
    int lv = e->vc == VC_LVALUE && !is_bitfield(e);
    r.is_ref = 1;
    r.ref_cv = T->q;
    /* reference-related but not compatible (it would drop cv): no
     * temporary is made — the binding is not viable (9.4.4) */
    if (related && !compat && !is_bitfield(e))
        return r;
    int ok_rank = derived ? R_CONV : R_EXACT;
    if (compat && !is_bitfield(e)) {
        if (!rv && lv) {
            r.rank = ok_rank;
            return r;
        }
        if (rv && !lv) {
            r.rank = ok_rank;
            r.binds_rvref = 1;
            return r;
        }
        if (!rv && cref && !lv) {
            r.rank = ok_rank;
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
static struct cfunc *conv_function(struct cexpr *e, struct cty *to,
                                   int allow_explicit, struct ics *second);

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
        if (e->k != E_INITLIST && e->t && e->t->k == CT_CLASS &&
            is_proper_base(e->t->cls, to->cls)) {
            r.rank = R_CONV;              /* slicing to a base */
            r.base_to = to->cls;
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
        if (!f && e->t && e->t->k == CT_CLASS)
            f = conv_function(e, to, 0, NULL);
        if (f) {
            r.rank = R_USER;
            r.user = f;
        }
        return r;
    }
    if (e->t && e->t->k == CT_CLASS && e->k != E_INITLIST) {
        struct ics second;
        struct cfunc *f = conv_function(e, to, 0, &second);
        if (f) {
            r = second;
            r.rank = R_USER;
            r.user = f;
        }
        return r;
    }
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
    if (a->base_to && b->base_to && a->base_to != b->base_to) {
        /* to a base that derives from the other's: the nearer is better */
        if (is_proper_base(a->base_to, b->base_to))
            return 1;
        if (is_proper_base(b->base_to, a->base_to))
            return -1;
    }
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

/* A candidate being considered: its conversion per operand. In call form
 * index 0 is the implicit object (when there is one) and i+1 argument i;
 * in operator form index i is operand i, the left one being a member
 * candidate's object. */
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
    if (better)
        return 1;
    /* equal conversions: a non-template beats a template's
     * specialization (12.2.4.3) */
    if (!a->f->spec_of && b->f->spec_of)
        return 1;
    /* two templates' specializations: the more specialized template */
    if (a->f->spec_of && b->f->spec_of && a->f->spec_of != b->f->spec_of &&
        more_specialized(a->f->spec_of, b->f->spec_of,
                         a->has_obj && b->has_obj ? a->n - 1 : a->n - 1))
        return 1;
    return 0;
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

/* The implicit object parameter's conversion: `cv C&` (or && for an
 * &&-qualified member) — which, unless &-qualified, an rvalue binds too. */
static struct ics object_ics(struct cexpr *obj, struct cfunc *f)
{
    struct cty *ft = f->type;
    struct cexpr lv = *obj;
    if (ft->refq == 0)
        lv.vc = VC_LVALUE;
    struct cty *self = ct_ref(ct_qual(ct_class(f->cls), ft->fq),
                              ft->refq == 2);
    if (obj->t->k != CT_CLASS || !class_derives(obj->t->cls, f->cls, NULL)) {
        struct ics bad;
        memset(&bad, 0, sizeof bad);
        bad.rank = R_BAD;
        return bad;
    }
    return ref_ics(&lv, self);
}

static int resolve_ambiguous;

/* A call's explicit template arguments (f<int>(x)), for the template
 * candidates of the next resolution. */
static struct ctarg *cur_targs;
static int cur_ntargs, cur_has_targs;

/* The candidates with each function template replaced by its
 * specialization for these arguments — or dropped, when deduction or
 * substitution fails (13.10.3). */
static int instantiate_candidates(struct cfunc **fs, int nf,
                                  struct cexpr *obj, struct cexpr **args,
                                  int na, int opform, struct cfunc ***out)
{
    struct cfunc **r = xmalloc((size_t)(nf ? nf : 1) * sizeof *r);
    int n = 0;
    (void)obj;
    for (int k = 0; k < nf; k++) {
        struct cfunc *f = fs[k];
        if (!f->tmpl) {
            if (!cur_has_targs)
                r[n++] = f;
            continue;
        }
        int member = f->cls && !f->is_static && !f->is_ctor;
        struct cexpr **xa = opform && member ? args + 1 : args;
        int nx = opform && member ? na - 1 : na;
        struct ctarg *targs;
        int nt;
        if (!deduce_call(f->tmpl, cur_targs, cur_ntargs, xa, nx, &targs,
                         &nt))
            continue;
        struct cfunc *spec = func_instance(f->tmpl, targs, nt);
        if (spec)
            r[n++] = spec;
    }
    *out = r;
    return n;
}

/* The best of the candidates fs[0..nf) for the arguments. In call form,
 * obj (a pointer, or NULL) is the object member candidates are called
 * on; in operator form (opform) args[0] is the left operand, a member
 * candidate's object and a non-member's first argument. With `at` NULL
 * it probes: NULL (resolve_ambiguous telling why) instead of an error. */
static struct cfunc *best_of(struct cfunc **fs, int nf, struct cexpr *obj,
                             struct cexpr **args, int na, int opform,
                             const struct ctok *at, const char *what,
                             int flags)
{
    int templ = 0;
    for (int k = 0; k < nf; k++)
        templ |= fs[k]->tmpl != NULL;
    if (templ || cur_has_targs) {
        const char *w = what ? what : nf ? fs[0]->name : "?";
        nf = instantiate_candidates(fs, nf, obj, args, na, opform, &fs);
        what = w;
    }
    struct cand *cs = xcalloc((size_t)(nf ? nf : 1), sizeof *cs);
    int nv = 0;
    resolve_ambiguous = 0;
    struct cexpr *objlv = obj ? ex_deref(obj) : NULL;
    for (int k = 0; k < nf; k++) {
        struct cfunc *f = fs[k];
        struct cty *ft = f->type;
        int member = f->cls && !f->is_static && !f->is_ctor;
        int nx = opform && member ? na - 1 : na;    /* explicit arguments */
        struct cexpr **xa = opform && member ? args + 1 : args;
        if (nx > ft->np && !ft->variadic)
            continue;
        if (nx < ft->np && (!f->defargs || !f->defargs[nx]))
            continue;
        if ((flags & RS_NO_EXPLICIT) && f->is_explicit)
            continue;
        if (member && !opform && !obj)
            continue;
        struct cand *c = &cs[nv];
        c->f = f;
        c->n = opform ? na : na + 1;
        c->has_obj = opform || member;
        c->ics = xcalloc((size_t)(c->n ? c->n : 1), sizeof *c->ics);
        int ok = 1, base = opform ? 0 : 1;
        if (member) {
            c->ics[0] = object_ics(opform ? args[0] : objlv, f);
            ok = c->ics[0].rank != R_BAD;
            if (opform)
                base = 1;
        }
        for (int i = 0; ok && i < nx; i++) {
            struct ics *ic = &c->ics[base + i];
            if (i >= ft->np) {
                ic->rank = R_ELLIPSIS;
                continue;
            }
            *ic = ics_of(xa[i], ft->params[i]);
            ok = ic->rank != R_BAD;
            if ((flags & RS_NO_USER) && ic->rank == R_USER)
                ok = 0;
        }
        if (ok)
            nv++;
    }
    if (nv == 0) {
        if (!at)
            return NULL;
        cx_error(at, "no matching function for call to '%s(%s)'",
                 what ? what : nf ? fs[0]->name : "?", arg_types(args, na));
    }
    int best = 0;
    for (int i = 1; i < nv; i++)
        if (cand_better(&cs[i], &cs[best]))
            best = i;
    for (int i = 0; i < nv; i++) {
        if (i == best)
            continue;
        if (!cand_better(&cs[best], &cs[i])) {
            resolve_ambiguous = 1;
            if (!at)
                return NULL;
            cx_error(at, "call to '%s(%s)' is ambiguous",
                     what ? what : fs[0]->name, arg_types(args, na));
        }
    }
    return cs[best].f;
}

/* ---- candidate sets ---- */

struct fnset {
    struct cfunc **f;
    int n, cap;
};

static void set_add(struct fnset *s, struct cfunc *f)
{
    for (int i = 0; i < s->n; i++)
        if (s->f[i] == f)
            return;
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 16;
        s->f = xrealloc(s->f, (size_t)s->cap * sizeof *s->f);
    }
    s->f[s->n++] = f;
}

static void set_add_all(struct fnset *s, struct cfunc *chain)
{
    for (struct cfunc *f = chain; f; f = f->next)
        set_add(s, f);
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
    struct fnset fs = { NULL, 0, 0 };
    set_add_all(&fs, set);
    return best_of(fs.f, fs.n, obj, args, na, 0, at, what, flags);
}

/* The namespaces argument-dependent lookup searches for an argument of
 * type t (6.5.4): a class's or enum's innermost enclosing namespace (and
 * the namespace enclosing an inline one), looking through pointers,
 * references, arrays, and a function type's parameters and result. */
static void assoc_ns(struct cty *t, struct cscope **ns, int *n, int depth)
{
    if (!t || depth > 8)
        return;
    while (t->k == CT_PTR || ct_is_ref(t) || t->k == CT_ARRAY)
        t = t->to;
    struct cscope *s = NULL;
    if (t->k == CT_CLASS)
        s = enclosing_ns(t->cls->owner);
    else if (t->k == CT_ENUM)
        s = enclosing_ns(t->en->owner);
    else if (t->k == CT_FUNC) {
        assoc_ns(t->to, ns, n, depth + 1);
        for (int i = 0; i < t->np; i++)
            assoc_ns(t->params[i], ns, n, depth + 1);
        return;
    }
    for (; s; s = s->is_inline ? s->parent : NULL) {
        int have = 0;
        for (int i = 0; i < *n; i++)
            have |= ns[i] == s;
        if (!have && *n < 32)
            ns[(*n)++] = s;
        if (!s->is_inline)
            break;
    }
}

static void adl(struct fnset *fs, const char *name, struct cexpr **args,
                int na)
{
    struct cscope *ns[32];
    int n = 0;
    for (int i = 0; i < na; i++)
        if (args[i]->t)
            assoc_ns(args[i]->t, ns, &n, 0);
    for (int i = 0; i < n; i++) {
        struct csym *y = lookup_in(ns[i], name);
        if (y && y->k == CS_FUNC)
            set_add_all(fs, y->fns);
    }
}

/* An operator's or unqualified call's ordinary lookup, member functions
 * ignored: the functions `name` in the nearest enclosing namespace or
 * block scope declaring any. */
static void nonmember_lookup(struct fnset *fs, const char *name)
{
    for (struct cscope *s = cx_scope; s; s = s->parent) {
        if (s->k == SC_CLASS || s->k == SC_ENUM)
            continue;
        struct csym *y = lookup_in(s, name);
        if (y) {
            if (y->k == CS_FUNC)
                set_add_all(fs, y->fns);
            return;
        }
    }
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
        struct ics s;
        if (ct_is_ref(p)) {
            s = ref_ics(e, p);
        } else if (pb->k == CT_CLASS) {
            memset(&s, 0, sizeof s);
            s.rank = R_BAD;
        } else {
            s = std_conv(e, p);
        }
        if (s.rank >= R_USER)
            continue;
        if (found)
            return resolve_ex(c->ctors, NULL, &e, 1, NULL, NULL,
                              allow_explicit ? 0 : RS_NO_EXPLICIT);
        found = f;
    }
    return found;
}

/* The conversion function of e's class that takes e to `to` with a
 * standard conversion after it (12.2.2.6, 12.2.2.7): the best one, or
 * NULL (none, or two equally good). *second gets that conversion. */
static struct cfunc *conv_function(struct cexpr *e, struct cty *to,
                                   int allow_explicit, struct ics *second)
{
    struct cclass *c = e->t->cls;
    if (!c->complete)
        return NULL;
    struct cfunc *best = NULL;
    struct ics bs;
    int tie = 0;
    memset(&bs, 0, sizeof bs);
    for (struct csym *y = c->scope->syms; y; y = y->next) {
        if (y->k != CS_FUNC)
            continue;
        for (struct cfunc *f = y->fns; f; f = f->next) {
            if (!f->is_conv || (f->is_explicit && !allow_explicit))
                continue;
            if ((e->t->q & CQ_CONST) && !(f->type->fq & CQ_CONST))
                continue;
            struct cty *R = f->type->to;
            struct cexpr r;
            memset(&r, 0, sizeof r);
            r.k = E_CALL;
            r.t = ct_strip_ref(R);
            r.vc = R->k == CT_LREF ? VC_LVALUE : R->k == CT_RREF
                   ? VC_XVALUE : VC_PRVALUE;
            struct ics s;
            memset(&s, 0, sizeof s);
            s.rank = R_BAD;
            if (ct_is_ref(to))
                s = ref_ics(&r, to);
            else if (to->k == CT_CLASS)
                s.rank = r.t->k == CT_CLASS && r.t->cls == to->cls
                         ? R_EXACT : R_BAD;
            else if (r.t->k != CT_CLASS)
                s = std_conv(&r, to);
            if (s.rank > R_CONV)
                continue;
            int cmp = best ? ics_cmp(&s, &bs) : 1;
            if (cmp > 0) {
                best = f;
                bs = s;
                tie = 0;
            } else if (cmp == 0) {
                tie = 1;
            }
        }
    }
    if (tie)
        return NULL;
    if (best && second)
        *second = bs;
    return best;
}

/* e (a class object) converted by one of its conversion functions to t:
 * the call, then the standard conversion after it. */
static struct cexpr *through_conv(struct cexpr *e, struct cty *t,
                                  int allow_explicit, const char *ctx)
{
    struct cfunc *f = conv_function(e, t, allow_explicit, NULL);
    if (!f)
        ex_error(e, "no conversion from '%s' to '%s' in %s", ct_name(e->t),
                 ct_name(t), ctx);
    struct cexpr *obj = e->vc == VC_PRVALUE ? materialize(e) : e;
    const struct ctok *at = cx_cur();
    struct cexpr *call = make_call(f, ex_addr(obj), NULL, 0, at);
    call->line = e->line;
    call->file = e->file;
    if (ct_is_ref(t))
        return call;
    return convert(call, t, ctx);
}

struct cexpr *convert(struct cexpr *e, struct cty *t, const char *ctx)
{
    if (e->k == E_INITLIST && !e->t)
        return init_object(t, INIT_COPY_LIST, &e, 1, NULL);
    if (t->k == CT_CLASS)
        return init_object(t, INIT_COPY, &e, 1, NULL);
    if (t->k == CT_VOID)
        return ex_cast(e, ct_basic(CT_VOID));
    if (e->k == E_OVL && e->memptr) {
        if (ct_is_pmf(t))
            for (struct cfunc *f = e->fn; f; f = f->next)
                if ((f->cls == t->cls ||
                     class_derives(t->cls, f->cls, NULL)) &&
                    ct_same_unqual(f->type, t->to)) {
                    struct cexpr *r = ex_new(E_MEMPTR,
                                             ct_mptr(f->cls, t->to),
                                             VC_PRVALUE);
                    r->fn = f;
                    r->line = e->line;
                    r->file = e->file;
                    return f->cls == t->cls ? r
                           : mptr_to_derived(r, ct_unqual(t));
                }
        ex_error(e, "no overload of '%s' converts to '%s' in %s",
                 e->fn->name, ct_name(t), ctx);
    }
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
    if (e->t->k == CT_CLASS)
        return through_conv(e, t, 0, ctx);
    struct ics s = std_conv(e, t);
    if (s.rank == R_BAD)
        ex_error(e, "cannot convert '%s' to '%s' in %s", ct_name(e->t),
                 ct_name(t), ctx);
    e = rvalue(e);
    struct cty *tu = ct_unqual(t);
    if (ct_same(e->t, tu))
        return e;
    if (s.base_to && e->t->k == CT_PTR) {
        e = to_base(e, s.base_to, 1);
        if (ct_same(e->t, tu))
            return e;
    }
    if (tu->k == CT_MPTR && e->t->k == CT_MPTR && e->t->cls != tu->cls)
        return mptr_to_derived(e, tu);
    return ex_cast(e, tu);
}

struct cexpr *convert_bool(struct cexpr *e, const char *ctx)
{
    e = rvalue(e);
    if (e->t->k == CT_CLASS)   /* contextually: explicit operator bool too */
        return through_conv(e, ct_basic(CT_BOOL), 1, ctx);
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
    if (e->t->k == CT_CLASS && T->k == CT_CLASS &&
        is_proper_base(e->t->cls, T->cls) && e->vc != VC_PRVALUE)
        e = to_base(e, T->cls, 0);
    int related = ct_same_unqual(e->t, T);
    int compat = related && (T->q & e->t->q) == e->t->q;
    if (related && !compat && !is_bitfield(e))
        ex_error(e, "binding a reference to '%s' to a '%s' drops a qualifier "
                    "(%s)", ct_name(T), ct_name(e->t), ctx);
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
        struct cexpr *v = init_object(p, arg->k == E_INITLIST && !arg->t
                                         ? INIT_COPY_LIST : INIT_COPY,
                                      &arg, 1, at);
        /* Itanium: a class that is not trivially copyable is passed as
         * the address of a temporary the caller builds — and destroys,
         * at the end of the full-expression */
        if (class_indirect(p))
            return ex_addr(materialize(v));
        return v;
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
    if (ret->k == CT_CLASS && !ct_is_complete(ret))
        cx_error(at, "calling '%s', which returns incomplete '%s'", fn->name,
                 ct_name(ret));
    if ((fn->is_implicit || fn->is_defaulted) && !fn->defined)
        define_implicit(fn);
    func_ensure_body(fn);
    struct cexpr **conv;
    int n = convert_args(fn, args, na, at, &conv);
    struct cexpr *e = call_result(ret);
    e->fn = fn;
    int member = fn->cls && !fn->is_static && !fn->is_ctor;
    if (member && obj && obj->t->to->k == CT_CLASS &&
        obj->t->to->cls != fn->cls)
        obj = to_base(obj, fn->cls, 1);
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

/* operator new/delete for objects of type t: the class's own (unless
 * `::new`), else the global one. */
static struct cexpr *call_alloc_op(const char *name, struct cty *t,
                                   int global, struct cexpr **args, int na,
                                   const struct ctok *at)
{
    if (!global && t->k == CT_CLASS && ct_is_complete(t)) {
        struct csym *y = scope_find_here(t->cls->scope, name);
        if (y && y->k == CS_FUNC) {
            struct cfunc *f = resolve(y->fns, NULL, args, na, NULL, name);
            if (!f && na == 2 && strncmp(name, "operator delete", 15) == 0)
                f = resolve(y->fns, NULL, args, 1, at, name);  /* unsized */
            if (!f)
                f = resolve(y->fns, NULL, args, na, at, name);
            return make_call(f, NULL, args, f->type->np < na ? f->type->np
                                                             : na, at);
        }
    }
    return call_global_op(name, args, na, at);
}

struct cexpr *call_delete_op(struct cclass *c, struct cexpr **args)
{
    return call_alloc_op("operator delete", ct_class(c), 0, args, 2,
                         cx_cur());
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

/* An element of an expression list — an argument, an initializer: the
 * expressions it gives appended to v[*n] (capacity *cap). A pattern ending
 * in `...` gives one per element of the packs it names. */
static void list_element(struct cexpr ***v, int *n, int *cap)
{
    struct csym *packs[8];
    int ell;
    int ex = expansion_at(0, packs, 8, &ell);
    if (ex < 0)
        cx_error(&cx_toks[ell], "'...' expands no parameter pack");
    int reps = ex > 0 ? expansion_length(packs, ex) : 1;
    int start = cx_pos;
    for (int e = 0; e < reps; e++) {
        cx_pos = start;
        for (int k = 0; k < ex; k++)
            pack_push(packs[k], e);
        if (*n == *cap) {
            *cap = *cap ? *cap * 2 : 8;
            *v = xrealloc(*v, (size_t)*cap * sizeof **v);
        }
        (*v)[(*n)++] = cx_kind() == TOK_LBRACE ? parse_braced_list()
                                               : expr_parse_assign();
        if (ex > 0)
            pack_pop(ex);
    }
    if (ex > 0) {
        cx_pos = ell;
        cx_expect(TOK_ELLIPSIS, "'...'");
    }
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
        list_element(&L->a, &L->na, &cap);
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

static int class_or_enum(const struct cexpr *e)
{
    return e->t && (e->t->k == CT_CLASS || e->t->k == CT_ENUM);
}

static const char *op_fn_name(int op)
{
    switch (op) {
    case TOK_PLUS: return "operator+"; case TOK_MINUS: return "operator-";
    case TOK_STAR: return "operator*"; case TOK_SLASH: return "operator/";
    case TOK_PERCENT: return "operator%"; case TOK_AMP: return "operator&";
    case TOK_PIPE: return "operator|"; case TOK_CARET: return "operator^";
    case TOK_SHL: return "operator<<"; case TOK_SHR: return "operator>>";
    case TOK_EQEQ: return "operator=="; case TOK_NEQ: return "operator!=";
    case TOK_LT: return "operator<"; case TOK_GT: return "operator>";
    case TOK_LE: return "operator<="; case TOK_GE: return "operator>=";
    case TOK_ANDAND: return "operator&&"; case TOK_OROR: return "operator||";
    case TOK_TILDE: return "operator~"; case TOK_BANG: return "operator!";
    case TOK_ASSIGN: return "operator=";
    case TOK_PLUSEQ: return "operator+="; case TOK_MINUSEQ: return "operator-=";
    case TOK_STAREQ: return "operator*="; case TOK_SLASHEQ: return "operator/=";
    case TOK_PERCENTEQ: return "operator%=";
    case TOK_AMPEQ: return "operator&="; case TOK_PIPEEQ: return "operator|=";
    case TOK_CARETEQ: return "operator^=";
    case TOK_SHLEQ: return "operator<<="; case TOK_SHREQ: return "operator>>=";
    case TOK_PLUSPLUS: return "operator++";
    case TOK_MINUSMINUS: return "operator--";
    case TOK_SPACESHIP: return "operator<=>";
    case TOK_COMMA: return "operator,";
    default: return NULL;
    }
}

/* `a @ b` or `@a` with a class or enum operand: the operator function
 * chosen among a's members and the non-members found by ordinary lookup
 * and argument-dependent lookup (12.2.2.3) — or NULL when none is viable,
 * and the built-in operator applies. */
static struct cexpr *overloaded(const char *name, struct cexpr **args, int na,
                                int member_only, const struct ctok *at)
{
    int any = 0;
    for (int i = 0; i < na; i++)
        any |= class_or_enum(args[i]);
    if (!any || !name)
        return NULL;
    struct fnset fs = { NULL, 0, 0 };
    struct cexpr *l = args[0];
    if (l->t && l->t->k == CT_CLASS) {
        if (!ct_is_complete(l->t))
            ex_error(l, "'%s' on incomplete '%s'", name, ct_name(l->t));
        struct csym *y = scope_find_here(l->t->cls->scope, name);
        if (y && y->k == CS_FUNC)
            set_add_all(&fs, y->fns);
    }
    if (!member_only) {
        nonmember_lookup(&fs, name);
        adl(&fs, name, args, na);
    }
    if (!fs.n)
        return NULL;
    struct cfunc *f = best_of(fs.f, fs.n, NULL, args, na, 1, NULL, name, 0);
    if (!f) {
        if (resolve_ambiguous)
            best_of(fs.f, fs.n, NULL, args, na, 1, at, name, 0);
        return NULL;
    }
    if (f->cls && !f->is_static) {
        if ((f->is_implicit || f->is_defaulted) && f->trivial &&
            (f->special == SP_COPY_ASSIGN || f->special == SP_MOVE_ASSIGN)) {
            /* a trivial assignment is the struct's bytes */
            need_modifiable(l, "assignment");
            struct cexpr *r = args[1];
            struct cexpr *v = init_object(ct_unqual(l->t),
                                          r->k == E_INITLIST && !r->t
                                          ? INIT_COPY_LIST : INIT_COPY,
                                          &r, 1, at);
            struct cexpr *e = ex2(E_ASSIGN, l->t, VC_LVALUE, l, v);
            e->op = TOK_ASSIGN;
            return e;
        }
        struct cexpr *obj = l->vc == VC_PRVALUE ? materialize(l) : l;
        return make_call(f, ex_addr(obj), args + 1, na - 1, at);
    }
    return make_call(f, NULL, args, na, at);
}

/* A class operand of a built-in operator, through its one non-explicit
 * conversion function to a scalar — if it has exactly one. */
static struct cexpr *class_to_builtin(struct cexpr *e)
{
    if (!e->t || e->t->k != CT_CLASS || !ct_is_complete(e->t))
        return e;
    struct cfunc *only = NULL;
    int n = 0;
    for (struct csym *y = e->t->cls->scope->syms; y; y = y->next)
        if (y->k == CS_FUNC)
            for (struct cfunc *f = y->fns; f; f = f->next)
                if (f->is_conv && !f->is_explicit &&
                    ct_is_scalar(ct_strip_ref(f->type->to))) {
                    only = f;
                    n++;
                }
    if (n != 1)
        return e;
    return through_conv(e, ct_unqual(ct_strip_ref(only->type->to)), 0,
                        "an operator");
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
    if (a->to->k == CT_CLASS && b->to->k == CT_CLASS) {
        if (is_proper_base(a->to->cls, b->to->cls)) {
            *l = to_base(*l, b->to->cls, 1);
            return;
        }
        if (is_proper_base(b->to->cls, a->to->cls)) {
            *r = to_base(*r, a->to->cls, 1);
            return;
        }
    }
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

static struct cexpr *deref(struct cexpr *e);

/* obj.*pm, p->*pm: the member pm names, of the object. A member
 * function's is only for calling (call() takes it). */
static struct cexpr *member_via_pointer(int op, struct cexpr *l,
                                        struct cexpr *r)
{
    struct cexpr *obj = op == TOK_ARROWSTAR ? deref(l) : l;
    r = rvalue(r);
    if (r->t->k != CT_MPTR)
        ex_error(r, "'%s' is not a pointer to member",
                 ct_name(r->t));
    int amb = 0;
    if (obj->t->k != CT_CLASS ||
        (obj->t->cls != r->t->cls &&
         (!class_derives(obj->t->cls, r->t->cls, &amb) || amb)))
        ex_error(obj, "'%s' has no members of '%s'", ct_name(obj->t),
                 ct_name(r->t));
    if (obj->t->cls != r->t->cls)
        obj = to_base(obj, r->t->cls, 0);   /* the base it is a member of */
    if (obj->vc == VC_PRVALUE)
        obj = materialize(obj);
    if (ct_is_pmf(r->t))
        return ex2(E_PMEM, r->t->to, VC_PRVALUE, obj, r);
    return ex2(E_PMEM, ct_qual(r->t->to, obj->t->q),
               obj->vc == VC_LVALUE ? VC_LVALUE : VC_XVALUE, obj, r);
}

static struct cexpr *binary(int op, struct cexpr *l, struct cexpr *r)
{
    if (op == TOK_DOTSTAR || op == TOK_ARROWSTAR)
        return member_via_pointer(op, l, r);
    if ((op == TOK_EQEQ || op == TOK_NEQ) &&
        ((l->t && l->t->k == CT_MPTR) || (r->t && r->t->k == CT_MPTR))) {
        struct cty *mt = ct_unqual(l->t->k == CT_MPTR ? l->t : r->t);
        struct cexpr *e = binop(op, convert(l, mt, "a comparison"),
                                convert(r, mt, "a comparison"));
        e->t = ct_basic(CT_BOOL);
        return e;
    }
    if (class_or_enum(l) || class_or_enum(r)) {
        struct cexpr *args[2] = { l, r };
        struct cexpr *o = overloaded(op_fn_name(op), args, 2, 0, cx_cur());
        if (o)
            return o;
        l = class_to_builtin(l);
        r = class_to_builtin(r);
    }
    const char *sp = "";
    if (op == TOK_ANDAND || op == TOK_OROR) {  /* contextual bool: below */
    } else {
        no_class_operand(l, sp);
        no_class_operand(r, sp);
    }
    if (op == TOK_ANDAND || op == TOK_OROR) {
        struct cexpr *e = binop(op, convert_bool(l, "&&/||"),
                                convert_bool(r, "&&/||"));
        e->t = ct_basic(CT_BOOL);
        return e;
    }
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
    if (l->t->k == CT_CLASS || (class_or_enum(r) && op != TOK_ASSIGN) ||
        (l->t->k == CT_ENUM && op != TOK_ASSIGN)) {
        struct cexpr *args[2] = { l, r };
        struct cexpr *o = overloaded(op_fn_name(op), args, 2,
                                     op == TOK_ASSIGN, cx_cur());
        if (o)
            return o;
        if (l->t->k == CT_CLASS)
            ex_error(l, "no viable '%s' for '%s' and '%s'", op_fn_name(op),
                     ct_name(l->t), r->k == E_INITLIST && !r->t ? "{...}"
                     : ct_name(r->t));
        r = class_to_builtin(r);
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
    if (class_or_enum(e)) {
        /* postfix: operator++(int), with a dummy 0 */
        struct cexpr *args[2] = { e, ex_int(0, ct_basic(CT_INT)) };
        struct cexpr *o = overloaded(op_fn_name(delta > 0 ? TOK_PLUSPLUS
                                                          : TOK_MINUSMINUS),
                                     args, post ? 2 : 1, 0, cx_cur());
        if (o)
            return o;
    }
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
    if (class_or_enum(e)) {
        struct cexpr *o = overloaded(op_fn_name(op), &e, 1, 0, cx_cur());
        if (o)
            return o;
        if (op != TOK_BANG)       /* ! converts contextually, below */
            e = class_to_builtin(e);
    }
    if (op != TOK_BANG)
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
    if (class_or_enum(e)) {
        struct cexpr *o = overloaded("operator*", &e, 1, 0, cx_cur());
        if (o)
            return o;
    }
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
        if ((kind == CAST_STATIC || kind == CAST_C) &&
            e->t->k == CT_CLASS && T->k == CT_CLASS && e->t->cls != T->cls) {
            struct cexpr *r;
            if (is_proper_base(e->t->cls, T->cls))
                r = to_base(e, T->cls, 0);
            else
                r = to_derived(e, T->cls, 0);
            r->t = ct_qual(ct_class(T->cls), T->q);
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
        return through_conv(e, ct_unqual(t), 1, "a cast");
    struct cty *tu = ct_unqual(t);
    if (ct_same(f, tu))
        return e;
    if (kind == CAST_CONST && !(f->k == CT_PTR && tu->k == CT_PTR))
        cx_error(at, "const_cast to a non-pointer type");
    if ((kind == CAST_STATIC || kind == CAST_C) && f->k == CT_PTR &&
        tu->k == CT_PTR && f->to->k == CT_CLASS && tu->to->k == CT_CLASS &&
        f->to->cls != tu->to->cls) {
        struct cclass *fc = f->to->cls, *tc = tu->to->cls;
        struct cexpr *r = NULL;
        if (is_proper_base(fc, tc))
            r = to_base(e, tc, 1);
        else if (is_proper_base(tc, fc))
            r = to_derived(e, tc, 1);
        if (r)
            return ct_same(r->t, tu) ? r : ex_cast(r, tu);
    }
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

/* std::type_info, which typeid's result is (from <typeinfo>). */
static struct cty *type_info_type(const struct ctok *at)
{
    struct csym *std = lookup_in(cx_global, "std");
    struct csym *y = std && std->k == CS_NAMESPACE
                     ? lookup_in(std->ns, "type_info") : NULL;
    if (!y || y->k != CS_CLASS)
        cx_error(at, "typeid needs std::type_info: #include <typeinfo>");
    return y->type;
}

/* dynamic_cast<T>(e) (7.6.1.7): to a base, a static conversion; else the
 * run-time check libsupc++'s __dynamic_cast makes. */
static struct cexpr *dynamic_cast_to(struct cty *t, struct cexpr *e,
                                     const struct ctok *at)
{
    int ref = ct_is_ref(t);
    struct cty *tt = ref ? t->to : ct_unqual(t)->k == CT_PTR ? t->to : NULL;
    if (!tt || (tt->k != CT_CLASS && !(tt->k == CT_VOID && !ref)))
        cx_error(at, "dynamic_cast to '%s': a pointer or reference to a "
                     "class", ct_name(t));
    struct cexpr *p;
    if (ref) {
        if (e->t->k != CT_CLASS || e->vc == VC_PRVALUE)
            cx_error(at, "dynamic_cast to a reference needs a class glvalue");
        p = ex_addr(e);
    } else {
        p = rvalue(e);
        if (p->t->k != CT_PTR || p->t->to->k != CT_CLASS)
            cx_error(at, "dynamic_cast of a non-pointer-to-class");
    }
    struct cclass *src = p->t->to->cls;
    if (tt->k == CT_CLASS && class_derives(src, tt->cls, NULL)) {
        struct cexpr *b = to_base(p, tt->cls, 1);
        return ref ? ex_deref(b) : b;
    }
    if (!src->dynamic)
        cx_error(at, "dynamic_cast from non-polymorphic '%s'", src->name);
    struct cexpr *d = ex_new(E_DYNCAST, ct_ptr(ct_qual(tt, p->t->to->q)),
                             VC_PRVALUE);
    d->a = xmalloc(sizeof *d->a);
    d->a[0] = p;
    d->na = 1;
    d->alloc_t = p->t->to;
    d->is_array = tt->k == CT_VOID;
    d->zero = ref;
    /* the hint: src is a unique public base of the target at this offset
     * (-1: unknown, -2: not a base at all) */
    d->ival = -1;
    if (tt->k == CT_CLASS) {
        int amb;
        if (class_derives(tt->cls, src, &amb) && !amb)
            d->ival = base_offset(tt->cls, src);    /* -1 if virtual */
        else if (!class_derives(tt->cls, src, NULL))
            d->ival = -2;
    }
    return ref ? ex_deref(d) : d;
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
        list_element(&args, &na, &cap);
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
            cx_skip_peek(n);
            continue;
        }
        break;
    }
    switch (cx_kind()) {
    case TOK_RPAREN: case TOK_STAR: case TOK_AMP: case TOK_ANDAND:
    case TOK_LBRACKET: case TOK_KW_CONST: case TOK_KW_VOLATILE:
        r = 1;
        break;
    case TOK_IDENT: case TOK_COLONCOLON: {
        struct qname q = peek_qname();      /* int C::* */
        r = !q.bad && q.scope && q.fin > 0 && cx_kind_at(q.fin) == TOK_STAR;
        break;
    }
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

static struct cexpr *parse_new(const struct ctok *at, int global)
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
    struct cexpr *call = call_alloc_op(count ? "operator new[]"
                                             : "operator new", elem, global,
                                       oa, nplace + 1, at);
    e->fn = call->fn;
    e->a = call->a;
    e->na = call->na;
    return e;
}

static struct cexpr *parse_delete(const struct ctok *at, int global)
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
        if (!ct_is_complete(t))
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
    struct cexpr *call = call_alloc_op(arr ? "operator delete[]"
                                           : "operator delete", t, global,
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

static int call_args_rest(struct cexpr ***out);

/* ( arguments ): the count, and the arguments in *out. */
static int call_args(struct cexpr ***out)
{
    cx_expect(TOK_LPAREN, "'('");
    return call_args_rest(out);
}

/* After `(`: arguments to the `)`. */
int expr_call_args_rest(struct cexpr ***out)
{
    return call_args_rest(out);
}

static int call_args_rest(struct cexpr ***out)
{
    struct cexpr **args = NULL;
    int na = 0, cap = 0;
    int saved = cx_in_targs;
    cx_in_targs = 0;
    while (cx_kind() != TOK_RPAREN) {
        list_element(&args, &na, &cap);
        if (!cx_accept(TOK_COMMA))
            break;
    }
    cx_expect(TOK_RPAREN, "')' to close the arguments");
    cx_in_targs = saved;
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

static struct cexpr *member_of(struct cexpr *obj, struct cfield *fl);

struct cexpr *ex_member(struct cexpr *obj, struct cfield *fl)
{
    return member_of(obj, fl);
}

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

/* The first function template in an overload set, if any. */
static struct ctemplate *set_template(struct cfunc *set)
{
    for (struct cfunc *f = set; f; f = f->next)
        if (f->tmpl)
            return f->tmpl;
    return NULL;
}

/* After a function template's name: `<args>` makes them explicit. */
static void explicit_targs(struct cexpr *e)
{
    struct ctemplate *t = e->fn ? set_template(e->fn) : NULL;
    if (!t || cx_kind() != TOK_LT)
        return;
    e->ntargs = parse_template_args(t, &e->targs);
    e->has_targs = 1;
}

/* An unqualified or qualified name, looked up (after the name was read). */
static struct cexpr *name_expr(struct csym *y, const char *name,
                               const struct ctok *at)
{
    switch (y->k) {
    case CS_TEMPLATE: {
        if (y->tmpl->kind != TK_VAR || cx_kind() != TOK_LT)
            cx_error(at, "template '%s' used without its arguments", name);
        struct ctarg *args;
        int n = parse_template_args(y->tmpl, &args);
        struct cvar *v = var_instance(y->tmpl, args, n, at);
        struct cexpr *e = ex_new(E_VAR, ct_strip_ref(v->type), VC_LVALUE);
        e->var = v;
        e->line = at->t.line;
        e->file = at->file;
        v->used = 1;
        return e;
    }
    case CS_PACK:
        cx_error(at, "parameter pack '%s' must be expanded with '...'", name);
        return NULL;
    case CS_VAR: {
        struct cvar *v = y->var;
        if (v->is_local && !v->is_static && v->fn != cx_curfn &&
            !(v->fn == NULL))
            cx_error(at, "'%s' is a local of another function", name);
        if (v->is_member_static && !v->defined)
            member_var_from_outdef(v);
        struct cexpr *e = ex_new(E_VAR, ct_strip_ref(v->type), VC_LVALUE);
        e->var = v;
        e->line = at->t.line;
        e->file = at->file;
        v->used = 1;
        return e;
    }
    case CS_FIELD: {
        struct cclass *c = y->scope->cls;
        if (!cx_curfn || !cx_curfn->this_var ||
            !class_derives(cx_curfn->cls, c, NULL))
            cx_error(at, "member '%s' used without an object", name);
        return member_of(to_base(ex_deref(ex_this()), c, 0), y->field);
    }
    case CS_FUNC: {
        struct cexpr *e = ex_new(E_OVL, y->fns->type, VC_LVALUE);
        e->fn = y->fns;
        e->name = name;
        e->line = at->t.line;
        e->file = at->file;
        e->adl = y->scope->k == SC_NAMESPACE;
        if (y->scope->k == SC_CLASS && cx_curfn && cx_curfn->this_var &&
            class_derives(cx_curfn->cls, y->scope->cls, NULL))
            e->obj = ex_this();
        explicit_targs(e);
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

static int is_assign_op(enum tok_kind k);

static int fold_op(enum tok_kind k)
{
    switch (k) {
    case TOK_PLUS: case TOK_MINUS: case TOK_STAR: case TOK_SLASH:
    case TOK_PERCENT: case TOK_CARET: case TOK_AMP: case TOK_PIPE:
    case TOK_SHL: case TOK_SHR: case TOK_EQEQ: case TOK_NEQ: case TOK_LT:
    case TOK_GT: case TOK_LE: case TOK_GE: case TOK_ANDAND: case TOK_OROR:
    case TOK_COMMA:
        return 1;
    default:
        return is_assign_op(k);
    }
}

/* At `(`: a fold expression's `...` (13.7.4 / 7.5.6), else -1. */
static int fold_at(void)
{
    int depth = 0;
    for (int i = cx_pos + 1; i < cx_ntoks; i++) {
        enum tok_kind k = cx_toks[i].t.kind;
        if (k == TOK_EOF)
            return -1;
        if (k == TOK_LPAREN || k == TOK_LBRACKET || k == TOK_LBRACE) {
            depth++;
            continue;
        }
        if (k == TOK_RPAREN || k == TOK_RBRACKET || k == TOK_RBRACE) {
            if (depth-- == 0)
                return -1;
            continue;
        }
        if (depth || k != TOK_ELLIPSIS)
            continue;
        enum tok_kind prev = cx_toks[i - 1].t.kind;
        enum tok_kind next = cx_toks[i + 1].t.kind;
        if (i == cx_pos + 1 && fold_op(next))
            return i;
        if (fold_op(prev) && (next == TOK_RPAREN || next == prev))
            return i;
    }
    return -1;
}

static struct cexpr *fold_apply(enum tok_kind op, struct cexpr *l,
                                struct cexpr *r)
{
    if (op == TOK_COMMA)
        return comma(l, r);
    if (is_assign_op(op))
        return assign(op, l, r);
    return binary(op, l, r);
}

/* One operand of a fold, tokens from..to: a cast-expression. */
static struct cexpr *fold_operand(int from, int to)
{
    cx_pos = from;
    struct cexpr *e = parse_cast();
    if (cx_pos != to)
        cx_error(cx_cur(), "a fold expression's operand must be a "
                           "cast-expression (parenthesize it)");
    return e;
}

/* ( ... op E ), ( E op ... ), ( I op ... op E ), ( E op ... op I ): E
 * once per element of the packs it names, joined by op to the left or
 * right, around I if given. */
static struct cexpr *parse_fold(int ell)
{
    const struct ctok *at = cx_cur();
    int open = cx_pos, close = cx_pos;
    cx_skip_balanced();
    close = cx_pos - 1;
    enum tok_kind op;
    int left, e_from, e_to, i_from = -1, i_to = -1;
    struct csym *packs[8];
    if (ell == open + 1) {
        op = cx_toks[ell + 1].t.kind;
        left = 1;
        e_from = ell + 2;
        e_to = close;
    } else {
        op = cx_toks[ell - 1].t.kind;
        if (cx_toks[ell + 1].t.kind == TOK_RPAREN) {
            left = 0;
            e_from = open + 1;
            e_to = ell - 1;
        } else if (packs_in(open + 1, ell - 1, packs, 8) > 0) {
            left = 0;
            e_from = open + 1;
            e_to = ell - 1;
            i_from = ell + 2;
            i_to = close;
        } else {
            left = 1;
            i_from = open + 1;
            i_to = ell - 1;
            e_from = ell + 2;
            e_to = close;
        }
    }
    int ex = packs_in(e_from, e_to, packs, 8);
    if (ex <= 0)
        cx_error(at, "a fold expression's operand names no parameter pack");
    int len = expansion_length(packs, ex);
    struct cexpr **el = xcalloc((size_t)(len + 1), sizeof *el);
    for (int e = 0; e < len; e++) {
        for (int k = 0; k < ex; k++)
            pack_push(packs[k], e);
        el[e] = fold_operand(e_from, e_to);
        pack_pop(ex);
    }
    struct cexpr *acc = i_from >= 0 ? fold_operand(i_from, i_to) : NULL;
    cx_pos = close + 1;
    if (len == 0 && !acc) {
        /* an empty unary fold: its operator's identity */
        if (op == TOK_ANDAND || op == TOK_OROR)
            return ex_int(op == TOK_ANDAND, ct_basic(CT_BOOL));
        if (op == TOK_COMMA)
            return ex_cast(ex_int(0, ct_basic(CT_INT)),
                           ct_basic(CT_VOID));
        cx_error(at, "a unary fold over '%s' of an empty pack",
                 tok_describe(&cx_toks[ell - 1].t));
    }
    if (left) {
        int i = 0;
        if (!acc)
            acc = el[i++];
        for (; i < len; i++)
            acc = fold_apply(op, acc, el[i]);
    } else {
        int i = len - 1;
        if (!acc)
            acc = el[i--];
        for (; i >= 0; i--)
            acc = fold_apply(op, el[i], acc);
    }
    return acc;
}

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
        {
            int ell = fold_at();
            if (ell >= 0)
                return parse_fold(ell);
        }
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
        {
            int saved = cx_in_targs;
            cx_in_targs = 0;
            e = expr_parse();
            cx_in_targs = saved;
        }
        cx_expect(TOK_RPAREN, "')'");
        e->paren = 1;
        return e;
    case TOK_CX_STATIC_CAST: case TOK_CX_REINTERPRET_CAST:
    case TOK_CX_CONST_CAST: case TOK_CX_DYNAMIC_CAST: {
        enum tok_kind k = cx_kind();
        cx_advance();
        cx_expect(TOK_LT, "'<' after the cast");
        struct cty *t = parse_type_id();
        cx_expect(TOK_GT, "'>' after the cast's type");
        cx_expect(TOK_LPAREN, "'('");
        struct cexpr *x = expr_parse();
        cx_expect(TOK_RPAREN, "')'");
        if (k == TOK_CX_DYNAMIC_CAST)
            return dynamic_cast_to(t, x, at);
        return cast_to(t, x, k == TOK_CX_STATIC_CAST ? CAST_STATIC
                             : k == TOK_CX_CONST_CAST ? CAST_CONST
                             : CAST_REINTERPRET, at);
    }
    case TOK_CX_TYPEID: {
        cx_advance();
        if (cx_kind() != TOK_LPAREN)
            cx_error(cx_cur(), "expected '(' after typeid");
        int is_type = paren_type_id();
        cx_advance();
        struct cty *ti = type_info_type(at);
        struct cexpr *e = ex_new(E_TYPEID, ct_qual(ti, CQ_CONST), VC_LVALUE);
        if (is_type) {
            e->alloc_t = ct_unqual(ct_strip_ref(parse_type_id()));
        } else {
            struct cexpr *x = expr_parse();
            struct cty *xt = ct_unqual(x->t);
            if (xt->k == CT_CLASS && xt->cls->dynamic && x->vc != VC_PRVALUE) {
                e->a = xmalloc(sizeof *e->a);
                e->a[0] = x;              /* its dynamic type, at run time */
                e->na = 1;
            }
            e->alloc_t = xt;
        }
        cx_expect(TOK_RPAREN, "')'");
        return e;
    }
    case TOK_CX_THROW:
        return parse_throw();
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
            cx_skip_peek(n);
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
        class_lookup_ambiguous = 0;
        struct csym *y = q.scope ? lookup_in(q.scope, name)
                                 : lookup(cx_scope, name);
        if (class_lookup_ambiguous)
            cx_error(at, "'%s' is found in more than one base class", name);
        if (y && q.scope && y->k == CS_FUNC) {
            struct cexpr *o = name_expr(y, name, at);
            o->nonvirt = 1;           /* C::f() calls C's f, not an override */
            return o;
        }
        if (!y) {
            if (!q.scope && strncmp(name, "__builtin_", 10) == 0)
                return parse_builtin(name, at);
            if (!q.scope && strcmp(name, "__null") == 0) {
                e = ex_int(0, ct_basic(CT_LONG));
                e->is_null_const = 1;
                return e;
            }
            if (!q.scope && cx_kind() == TOK_LPAREN) {
                /* only argument-dependent lookup can find it */
                e = ex_new(E_OVL, ct_basic(CT_VOID), VC_LVALUE);
                e->name = name;
                e->adl = 1;
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
        struct fnset fs = { NULL, 0, 0 };
        set_add_all(&fs, f->fn);
        const char *name = f->fn ? f->fn->name : f->name;
        if (f->adl)
            adl(&fs, name, args, na);
        if (!fs.n)
            cx_error(at, "'%s' was not declared in this scope", name);
        cur_targs = f->targs;
        cur_ntargs = f->ntargs;
        cur_has_targs = f->has_targs;
        struct cfunc *fn = best_of(fs.f, fs.n, f->obj, args, na, 0, at, name,
                                   0);
        cur_has_targs = 0;
        cur_ntargs = 0;
        cur_targs = NULL;
        struct cexpr *r = make_call(fn, fn->cls && !fn->is_static ? f->obj
                                                                  : NULL,
                                    args, na, at);
        r->nonvirt = f->nonvirt;
        return r;
    }
    if (f->k == E_PMEM && f->t->k == CT_FUNC) {
        /* (obj.*pmf)(args) */
        struct cty *ft = f->t;
        struct cexpr *obj = f->a[0];
        if ((obj->t->q & CQ_CONST) && !(ft->fq & CQ_CONST))
            cx_error(at, "calling a non-const member function through a "
                         "pointer, on a const object");
        if (na < ft->np || (na > ft->np && !ft->variadic))
            cx_error(at, "wrong number of arguments to a pointer to member "
                         "function");
        struct cexpr **conv;
        int n = convert_args_ft(ft, NULL, args, na, at, &conv);
        struct cexpr *e = call_result(ft->to);
        e->k = E_PMCALL;
        e->line = at->t.line;
        e->file = at->file;
        e->a = xmalloc((size_t)(n + 2) * sizeof *e->a);
        e->a[0] = ex_addr(obj);
        e->a[1] = f->a[1];
        memcpy(e->a + 2, conv, (size_t)n * sizeof *conv);
        e->na = n + 2;
        return e;
    }
    if (f->t->k == CT_CLASS) {
        struct cexpr **all = xmalloc((size_t)(na + 1) * sizeof *all);
        all[0] = f;
        memcpy(all + 1, args, (size_t)na * sizeof *args);
        struct cexpr *o = overloaded("operator()", all, na + 1, 1, at);
        if (!o)
            cx_error(at, "'%s' has no viable operator() for (%s)",
                     ct_name(f->t), arg_types(args, na));
        return o;
    }
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
        /* operator->, applied until it yields a pointer */
        for (int depth = 0; obj->t->k == CT_CLASS; depth++) {
            struct cexpr *o = overloaded("operator->", &obj, 1, 1, at);
            if (!o || depth > 32)
                cx_error(at, "'%s' has no operator->", ct_name(obj->t));
            obj = o;
        }
        obj = rvalue(obj);
        if (obj->t->k != CT_PTR || obj->t->to->k != CT_CLASS)
            cx_error(at, "'->' on a non-pointer-to-class '%s'",
                     ct_name(obj->t));
        obj = ex_deref(obj);
    }
    if (obj->t->k != CT_CLASS)
        cx_error(at, "request for a member of non-class type '%s'",
                 ct_name(obj->t));
    struct cclass *c = obj->t->cls;
    class_ensure(c);
    if (!c->complete && !c->defining)
        cx_error(at, "member access into incomplete '%s'", ct_name(obj->t));
    if (obj->vc == VC_PRVALUE)
        obj = materialize(obj);
    cx_accept(TOK_CX_TEMPLATE);          /* x.template f<T>() */
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
    struct cclass *in = c;              /* x.B::m looks in B */
    int qualified = q.fin > 0;
    if (qualified) {
        if (q.bad || !q.scope || q.scope->k != SC_CLASS ||
            !class_derives(c, q.scope->cls, NULL))
            cx_error(at, "'%s' names no base of '%s'", tok_describe(&at->t),
                     ct_name(obj->t));
        in = q.scope->cls;
        cx_pos += q.fin;
    }
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
    class_lookup_ambiguous = 0;
    struct csym *y = class_member(in, name);
    if (!y)
        cx_error(at, "'%s' has no member named '%s'", ct_name(obj->t), name);
    if (class_lookup_ambiguous)
        cx_error(at, "member '%s' is found in more than one base of '%s'",
                 name, ct_name(obj->t));
    switch (y->k) {
    case CS_FIELD:
        return member_of(to_base(obj, y->scope->cls, 0), y->field);
    case CS_VAR: {
        if (y->var->is_member_static && !y->var->defined)
            member_var_from_outdef(y->var);
        struct cexpr *e = ex_new(E_VAR, ct_strip_ref(y->var->type),
                                 VC_LVALUE);
        e->var = y->var;
        y->var->used = 1;
        return e;
    }
    case CS_FUNC: {
        struct cexpr *e = ex_new(E_OVL, y->fns->type, VC_LVALUE);
        e->fn = y->fns;
        e->name = name;
        e->obj = ex_addr(obj);
        e->nonvirt = qualified;
        explicit_targs(e);
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
            if (e->t->k == CT_CLASS) {
                struct cexpr *args[2] = { e, i };
                struct cexpr *o = overloaded("operator[]", args, 2, 1, at);
                if (!o)
                    cx_error(at, "'%s' has no viable operator[]",
                             ct_name(e->t));
                e = o;
                continue;
            }
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

/* After `&`: `C::m` naming a non-static member forms a pointer to it
 * (7.6.2.2) — anything else (a static member, `&(C::m)`) is an ordinary
 * address, NULL here. */
static struct cexpr *member_pointer(void)
{
    if (cx_kind() != TOK_IDENT && cx_kind() != TOK_COLONCOLON)
        return NULL;
    struct qname q = peek_qname();
    if (q.bad || !q.scope || q.scope->k != SC_CLASS || q.fin == 0)
        return NULL;
    struct cclass *c = q.scope->cls;
    int save = cx_pos;
    const struct ctok *at = cx_cur();
    cx_pos += q.fin;
    const char *name;
    if (cx_kind() == TOK_IDENT) {
        name = cx_cur()->t.text;
        cx_advance();
    } else if (cx_kind() == TOK_CX_OPERATOR) {
        struct cty *conv = NULL;
        name = parse_operator_name(&conv);
    } else {
        cx_pos = save;
        return NULL;
    }
    struct csym *y = scope_find_here(c->scope, name);
    if (y && y->k == CS_FIELD) {
        if (ct_is_ref(y->field->type))
            cx_error(at, "a pointer to reference member '%s'", name);
        if (y->field->bitwidth >= 0)
            cx_error(at, "a pointer to bit-field '%s'", name);
        struct cexpr *e = ex_new(E_MEMPTR, ct_mptr(c, y->field->type),
                                 VC_PRVALUE);
        e->field = y->field;
        return e;
    }
    if (y && y->k == CS_FUNC) {
        int stat = 0;
        for (struct cfunc *f = y->fns; f; f = f->next)
            stat |= f->is_static;
        if (!stat) {
            if (!y->fns->next) {
                struct cexpr *e = ex_new(E_MEMPTR,
                                         ct_mptr(c, y->fns->type),
                                         VC_PRVALUE);
                e->fn = y->fns;
                return e;
            }
            struct cexpr *e = ex_new(E_OVL, y->fns->type, VC_LVALUE);
            e->fn = y->fns;
            e->name = name;
            e->memptr = 1;
            return e;
        }
    }
    cx_pos = save;
    return NULL;
}

static struct cexpr *parse_unary(void)
{
    const struct ctok *at = cx_cur();
    enum tok_kind k = cx_kind();
    switch (k) {
    case TOK_PLUSPLUS: case TOK_MINUSMINUS:
        cx_advance();
        return incdec(parse_cast(), k == TOK_PLUSPLUS ? 1 : -1, 0);
    case TOK_AMP: {
        cx_advance();
        struct cexpr *m = member_pointer();
        if (m)
            return parse_postfix(m);
        return address_of(parse_cast());
    }
    case TOK_STAR:
        cx_advance();
        return deref(parse_cast());
    case TOK_PLUS: case TOK_MINUS: case TOK_TILDE: case TOK_BANG:
        cx_advance();
        return unary(k, parse_cast());
    case TOK_KW_SIZEOF: case TOK_KW_ALIGNOF: {
        cx_advance();
        if (k == TOK_KW_SIZEOF && cx_accept(TOK_ELLIPSIS)) {
            /* sizeof...(pack): its length */
            cx_expect(TOK_LPAREN, "'(' after 'sizeof...'");
            const struct ctok *nt = cx_cur();
            if (cx_kind() != TOK_IDENT)
                cx_error(nt, "expected a parameter pack's name");
            struct csym *y = lookup_raw(cx_scope, nt->t.text);
            if (!y || y->k != CS_PACK)
                cx_error(nt, "'%s' is not a parameter pack", nt->t.text);
            cx_advance();
            cx_expect(TOK_RPAREN, "')'");
            return ex_int(pack_length(y), ct_size_t());
        }
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
        return parse_new(at, 0);
    case TOK_CX_DELETE:
        return parse_delete(at, 0);
    case TOK_COLONCOLON:
        if (cx_kind_at(1) == TOK_CX_NEW) {
            cx_advance();
            return parse_new(cx_cur(), 1);
        }
        if (cx_kind_at(1) == TOK_CX_DELETE) {
            cx_advance();
            return parse_delete(cx_cur(), 1);
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
    if (cx_in_targs > 0 && (k == TOK_GT || k == TOK_SHR))
        return 0;                 /* it closes the template arguments */
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

struct cexpr *expr_assign(int op, struct cexpr *l, struct cexpr *r)
{
    return assign(op, l, r);
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

/* throw [assignment-expression]: the exception object, of the operand's
 * decayed, unqualified type, initialized from it (the object is
 * __cxa_allocate_exception's); without one, a rethrow. */
static struct cexpr *parse_throw(void)
{
    const struct ctok *at = cx_cur();
    cx_advance();
    if (!cx_exceptions)
        cx_error(at, "'throw' with exceptions disabled (-fno-exceptions)");
    struct cexpr *r = ex_new(E_THROW, ct_basic(CT_VOID), VC_PRVALUE);
    r->line = at->t.line;
    r->file = at->file;
    enum tok_kind k = cx_kind();
    if (k == TOK_SEMI || k == TOK_RPAREN || k == TOK_COMMA ||
        k == TOK_COLON || k == TOK_RBRACKET || k == TOK_RBRACE)
        return r;                            /* throw; */
    struct cexpr *e = expr_parse_assign();
    struct cty *t = ct_unqual(ct_decay(ct_strip_ref(e->t)));
    if (t->k == CT_VOID || (t->k == CT_CLASS && !ct_is_complete(t)))
        cx_error(at, "throwing an object of incomplete type '%s'",
                 ct_name(t));
    if (t->k == CT_CLASS && class_abstract(t->cls))
        cx_error(at, "throwing an object of abstract class '%s'",
                 ct_name(t));
    r->alloc_t = t;
    r->a = xmalloc(sizeof *r->a);
    r->a[0] = init_object(t, INIT_COPY, &e, 1, at);
    r->na = 1;
    return r;
}

struct cexpr *expr_parse_assign(void)
{
    if (cx_kind() == TOK_CX_THROW)
        return parse_throw();
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
