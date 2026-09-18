/* Classes: layout, what copying and destroying them entail, and building
 * an object — the constructor a declaration or expression calls, with the
 * member initialization that constructor performs.
 *
 * Layout is C's (the Itanium ABI's for a class with no bases and no
 * virtual functions, which is all CX1 has): fields in order, each at its
 * alignment, an empty class one byte. The special members the compiler
 * provides are made only when something needs their code: a trivial one
 * is spelled directly in C (a struct copy, nothing at all). */
#include "cxx.h"

#include <string.h>

#include "../driver/util.h"

struct cfield *class_find_field(struct cclass *c, const char *name)
{
    for (int i = 0; i < c->nfields; i++)
        if (c->fields[i]->name && strcmp(c->fields[i]->name, name) == 0)
            return c->fields[i];
    return NULL;
}

static long round_up(long v, long a)
{
    return (v + a - 1) / a * a;
}

/* The element type under any array dimensions. */
static struct cty *base_elem(struct cty *t)
{
    while (t->k == CT_ARRAY)
        t = t->to;
    return t;
}

/* ---- layout (Itanium 2.4) ---- */

/* The empty subobjects placed so far in the class being laid out: two of
 * the same type may not share an address. */
struct esub {
    struct cclass *c;
    long off;
};
static struct esub *esubs;
static int nesubs, capesubs;

static void esub_add(struct cclass *c, long off)
{
    if (!c->empty)
        return;
    if (nesubs == capesubs) {
        capesubs = capesubs ? capesubs * 2 : 16;
        esubs = xrealloc(esubs, (size_t)capesubs * sizeof *esubs);
    }
    esubs[nesubs].c = c;
    esubs[nesubs].off = off;
    nesubs++;
    for (int i = 0; i < c->nbases; i++)
        esub_add(c->bases[i].cls, off + c->bases[i].off);
}

static int esub_conflict(struct cclass *c, long off)
{
    if (!c->empty)
        return 0;
    for (int i = 0; i < nesubs; i++)
        if (esubs[i].c == c && esubs[i].off == off)
            return 1;
    for (int i = 0; i < c->nbases; i++)
        if (esub_conflict(c->bases[i].cls, off + c->bases[i].off))
            return 1;
    return 0;
}

/* What a base subobject of c occupies: a POD's whole size (its tail
 * padding is never reused), else its data. */
static long base_dsize(const struct cclass *c)
{
    return c->pod_layout ? c->size : c->nvsize;
}

static void layout(struct cclass *c)
{
    long bits = 0, align = 1, size = 0;
    nesubs = 0;
    c->primary = NULL;
    for (int i = 0; i < c->nbases && !c->primary; i++)
        if (c->bases[i].cls->dynamic && !c->bases[i].is_virtual)
            c->primary = c->bases[i].cls;
    if (c->dynamic && !c->primary) {
        bits = 64;                           /* the vptr */
        size = align = 8;
    }
    /* the primary base first, at 0; then the other bases in order */
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < c->nbases; i++) {
            struct cbase *b = &c->bases[i];
            struct cclass *bc = b->cls;
            if ((pass == 0) != (bc == c->primary))
                continue;
            long ba = bc->nvalign;
            if (bc->empty) {
                long off = 0;
                if (esub_conflict(bc, off)) {
                    off = (bits + 7) / 8;
                    while (esub_conflict(bc, off))
                        off += ba;
                }
                b->off = off;
                esub_add(bc, off);
                if (off + bc->size > size)
                    size = off + bc->size;
            } else {
                long off = ((bits + 7) / 8 + ba - 1) / ba * ba;
                b->off = off;
                bits = (off + base_dsize(bc)) * 8;
                if (off + bc->size > size)
                    size = off + bc->size;
            }
            if (ba > align)
                align = ba;
        }
    for (int i = 0; i < c->nfields; i++) {
        struct cfield *fl = c->fields[i];
        struct cty *t = fl->type;
        long fs = ct_is_ref(t) ? 8 : ct_size(t);
        long fa = ct_is_ref(t) ? 8 : ct_align(t);
        if (c->packed)
            fa = 1;
        if (c->is_union) {
            fl->off = 0;
            long sz = fl->bitwidth >= 0 ? (fl->bitwidth + 7) / 8 : fs;
            if (sz > size)
                size = sz;
            if (fl->name || fl->bitwidth < 0)
                if (fa > align)
                    align = fa;
            continue;
        }
        if (fl->bitwidth >= 0) {
            long w = fl->bitwidth, unit = fs * 8;
            if (w == 0) {
                bits = (bits + fa * 8 - 1) / (fa * 8) * (fa * 8);
                continue;
            }
            if (!c->packed && bits / unit != (bits + w - 1) / unit)
                bits = (bits + unit - 1) / unit * unit;
            fl->off = bits / 8 / fa * fa;
            bits += w;
            if (fl->name && fa > align)
                align = fa;
            continue;
        }
        long off = round_up((bits + 7) / 8, fa);
        struct cty *e = base_elem(t);
        if (e->k == CT_CLASS && e->cls->empty && t->k == CT_CLASS)
            while (esub_conflict(e->cls, off))
                off += fa;
        fl->off = off;
        if (e->k == CT_CLASS && t->k == CT_CLASS)
            esub_add(e->cls, off);
        bits = (off + fs) * 8;
        if (fa > align)
            align = fa;
    }
    long dsize = (bits + 7) / 8;
    if (!c->is_union && dsize > size)
        size = dsize;
    if (c->align_attr > align)
        align = c->align_attr;
    c->dsize = c->is_union ? size : dsize;
    c->nvsize = c->dsize;
    c->nvalign = align;
    size = round_up(size, align);
    if (size == 0)
        size = 1;                     /* an empty class is one byte */
    c->size = size;
    c->align = align;
}

/* ---- virtual functions ---- */

static int same_params(struct cty *a, struct cty *b)
{
    if (a->np != b->np || a->variadic != b->variadic || a->fq != b->fq ||
        a->refq != b->refq)
        return 0;
    for (int i = 0; i < a->np; i++)
        if (!ct_same_unqual(a->params[i], b->params[i]))
            return 0;
    return 1;
}

/* Does f (a member function of a derived class) override g? */
static int overrides(struct cfunc *f, struct cfunc *g)
{
    if (!g->is_virtual)
        return 0;
    if (f->is_dtor || g->is_dtor)
        return f->is_dtor && g->is_dtor;
    if (f->is_ctor || f->is_static || strcmp(f->name, g->name) != 0)
        return 0;
    return same_params(f->type, g->type);
}

/* A virtual function of c or of any of its bases that f overrides. */
static struct cfunc *overridden(struct cclass *c, struct cfunc *f)
{
    for (int i = 0; i < c->nbases; i++) {
        struct cclass *b = c->bases[i].cls;
        if (b->dtor && overrides(f, b->dtor))
            return b->dtor;
        for (struct csym *y = b->scope->syms; y; y = y->next)
            if (y->k == CS_FUNC)
                for (struct cfunc *g = y->fns; g; g = g->next)
                    if (overrides(f, g))
                        return g;
        struct cfunc *g = overridden(b, f);
        if (g)
            return g;
    }
    return NULL;
}

/* c's member functions in declaration order (the vtable's order). */
static int member_functions(struct cclass *c, struct cfunc ***out)
{
    int n = 0, cap = 16;
    struct cfunc **v = xmalloc((size_t)cap * sizeof *v);
    for (struct cfunc *f = cx_funcs; f; f = f->all_next) {
        if (f->cls != c || f->is_ctor || f->is_static)
            continue;
        if (n == cap) {
            cap *= 2;
            v = xrealloc(v, (size_t)cap * sizeof *v);
        }
        v[n++] = f;
    }
    *out = v;
    return n;
}

/* Virtual by `virtual`, or by overriding a base's virtual function. */
static int mark_virtuals(struct cclass *c)
{
    struct cfunc **fs;
    int n = member_functions(c, &fs), any = 0;
    for (int i = 0; i < n; i++) {
        if (!fs[i]->is_virtual && overridden(c, fs[i]))
            fs[i]->is_virtual = 1;
        any |= fs[i]->is_virtual;
    }
    return any;
}

/* A vtable of c's group: for the subobject at `off` (0: the primary). */
struct ventry {
    long off;
    struct vslot *slots;
    int n;
};

/* A slot's `adjust` here is the offset, in c, of the subobject whose type
 * is the function's class — the thunk subtracts off - adjust. */
static struct ventry *vgroup_of(struct cclass *c, int *n);

static void push_slot(struct vslot **v, int *n, int *cap, struct cfunc *f,
                      int deleting, long impl)
{
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 8;
        *v = xrealloc(*v, (size_t)*cap * sizeof **v);
    }
    (*v)[*n].f = f;
    (*v)[*n].deleting = deleting;
    (*v)[*n].adjust = impl;
    (*n)++;
}

/* c's own overrider of g, if it declares one. */
static struct cfunc *own_overrider(struct cclass *c, struct cfunc *g)
{
    struct cfunc **fs;
    int n = member_functions(c, &fs);
    for (int i = 0; i < n; i++)
        if (fs[i]->is_virtual && fs[i] != g && overrides(fs[i], g))
            return fs[i];
    return NULL;
}

static void build_vtables(struct cclass *c)
{
    struct vslot *v = NULL;
    int n = 0, cap = 0;
    if (c->primary) {
        for (int i = 0; i < c->primary->nvtab; i++) {
            struct vslot s = c->primary->vtab[i];
            struct cfunc *o = s.f ? own_overrider(c, s.f) : NULL;
            push_slot(&v, &n, &cap, o ? o : s.f, s.deleting,
                      o ? 0 : s.adjust);
        }
    }
    struct cfunc **fs;
    int nf = member_functions(c, &fs);
    for (int i = 0; i < nf; i++) {
        struct cfunc *f = fs[i];
        if (!f->is_virtual)
            continue;
        int k;
        for (k = 0; k < n; k++)
            if (v[k].f == f)
                break;
        if (k < n) {
            f->vslot = k;
            f->vclass = c;
            continue;
        }
        f->vslot = n;
        f->vclass = c;
        push_slot(&v, &n, &cap, f, 0, 0);
        if (f->is_dtor)
            push_slot(&v, &n, &cap, f, 1, 0);
    }
    c->vtab = v;
    c->nvtab = n;
    /* the key function: the first virtual one neither pure nor inline */
    c->key = NULL;
    for (int i = 0; i < nf && !c->key; i++)
        if (fs[i]->is_virtual && !fs[i]->is_pure && !fs[i]->is_inline &&
            !fs[i]->is_implicit && !fs[i]->is_defaulted)
            c->key = fs[i];
}

static struct ventry *vgroup_of(struct cclass *c, int *n)
{
    int cap = 4;
    struct ventry *g = xmalloc((size_t)cap * sizeof *g);
    *n = 0;
    if (!c->dynamic)
        return g;
    g[0].off = 0;
    g[0].slots = c->vtab;
    g[0].n = c->nvtab;
    *n = 1;
    for (int i = 0; i < c->nbases; i++) {
        struct cbase *b = &c->bases[i];
        if (!b->cls->dynamic)
            continue;
        int bn;
        struct ventry *bg = vgroup_of(b->cls, &bn);
        for (int k = 0; k < bn; k++) {
            if (b->cls == c->primary && k == 0)
                continue;                  /* shared with c's primary */
            struct ventry e;
            e.off = b->off + bg[k].off;
            e.n = bg[k].n;
            e.slots = xmalloc((size_t)(e.n ? e.n : 1) * sizeof *e.slots);
            for (int s = 0; s < e.n; s++) {
                struct vslot sl = bg[k].slots[s];
                struct cfunc *o = sl.f ? own_overrider(c, sl.f) : NULL;
                e.slots[s].f = o ? o : sl.f;
                e.slots[s].deleting = sl.deleting;
                e.slots[s].adjust = o ? 0 : b->off + sl.adjust;
            }
            if (*n == cap) {
                cap *= 2;
                g = xrealloc(g, (size_t)cap * sizeof *g);
            }
            g[(*n)++] = e;
        }
    }
    return g;
}

/* The vtable group of c (emit.c): the primary, then the secondaries. */
int class_vtables(struct cclass *c, long **offs, struct vslot ***slots,
                  int **counts)
{
    int n;
    struct ventry *g = vgroup_of(c, &n);
    *offs = xmalloc((size_t)(n ? n : 1) * sizeof **offs);
    *slots = xmalloc((size_t)(n ? n : 1) * sizeof **slots);
    *counts = xmalloc((size_t)(n ? n : 1) * sizeof **counts);
    for (int i = 0; i < n; i++) {
        (*offs)[i] = g[i].off;
        (*slots)[i] = g[i].slots;
        (*counts)[i] = g[i].n;
    }
    return n;
}

/* An object of c cannot be made while a slot's final overrider is pure. */
int class_abstract(struct cclass *c)
{
    int n;
    struct ventry *g = vgroup_of(c, &n);
    for (int i = 0; i < n; i++)
        for (int s = 0; s < g[i].n; s++)
            if (g[i].slots[s].f && g[i].slots[s].f->is_pure)
                return 1;
    return 0;
}

/* The special member f is, by its signature: a copy or move constructor
 * or assignment of c (0 if neither). */
static int copy_kind(struct cfunc *f, struct cclass *c)
{
    struct cty *ft = f->type;
    if (ft->np < 1 || (ft->np > 1 && (!f->defargs || !f->defargs[1])))
        return 0;
    struct cty *p = ft->params[0];
    struct cty *pb = ct_strip_ref(p);
    if (pb->k != CT_CLASS || pb->cls != c)
        return 0;
    int move = p->k == CT_RREF;
    if (f->is_ctor)
        return move ? SP_MOVE : p->k == CT_LREF ? SP_COPY : 0;
    return move ? SP_MOVE_ASSIGN : SP_COPY_ASSIGN;
}

int class_indirect(const struct cty *t)
{
    return t->k == CT_CLASS && t->cls->complete && !t->cls->trivial_for_calls;
}

static struct cfunc *assign_set(struct cclass *c)
{
    struct csym *y = scope_find_here(c->scope, "operator=");
    return y && y->k == CS_FUNC ? y->fns : NULL;
}

/* Declare one of the special members the standard declares implicitly
 * (11.4.5-11.4.7); it is defined only if it is used and runs code. */
static struct cfunc *declare_implicit(struct cclass *c, int sp, int trivial,
                                      int deleted)
{
    struct cfunc *f = xcalloc(1, sizeof *f);
    const char *cn = c->name ? c->name : "__cx_anon";
    struct cty *self = ct_class(c);
    struct cty *param = NULL;
    struct cty *ret = ct_basic(CT_VOID);
    switch (sp) {
    case SP_COPY: case SP_COPY_ASSIGN:
        param = ct_ref(ct_qual(self, CQ_CONST), 0);
        break;
    case SP_MOVE: case SP_MOVE_ASSIGN:
        param = ct_ref(self, 1);
        break;
    default:
        break;
    }
    if (sp == SP_COPY_ASSIGN || sp == SP_MOVE_ASSIGN)
        ret = ct_ref(self, 0);
    f->type = ct_func(ret, &param, param ? 1 : 0, 0);
    f->name = sp == SP_DTOR ? cx_fmt("~%s", cn)
              : sp == SP_COPY_ASSIGN || sp == SP_MOVE_ASSIGN ? "operator="
              : cn;
    f->owner = c->scope;
    f->cls = c;
    f->is_ctor = sp == SP_DEFAULT || sp == SP_COPY || sp == SP_MOVE;
    f->is_dtor = sp == SP_DTOR;
    f->is_implicit = 1;
    f->is_inline = 1;
    f->vslot = -1;
    f->special = sp;
    f->trivial = trivial;
    f->is_deleted = deleted;
    f->access = CA_PUBLIC;
    f->body_tok = f->mi_tok = -1;
    func_register(f);
    struct cfunc **set;
    if (f->is_ctor) {
        set = &c->ctors;
    } else if (f->is_dtor) {
        c->dtor = f;
        return f;
    } else {
        struct csym *y = scope_find_here(c->scope, "operator=");
        if (!y || y->k != CS_FUNC)
            y = scope_add(c->scope, CS_FUNC, "operator=");
        set = &y->fns;
    }
    while (*set)
        set = &(*set)->next;
    *set = f;
    return f;
}

void class_complete(struct cclass *c)
{
    int virt = mark_virtuals(c);
    c->dynamic = virt;
    for (int i = 0; i < c->nbases; i++)
        c->dynamic |= c->bases[i].cls->dynamic;
    layout(c);
    c->complete = 1;
    c->explicit_layout = c->nbases > 0 || c->dynamic;
    int named = 0;
    for (int i = 0; i < c->nfields; i++)
        named |= c->fields[i]->name != NULL ||
                 c->fields[i]->bitwidth != 0;
    c->empty = !c->dynamic && !named;
    for (int i = 0; i < c->nbases; i++)
        c->empty &= c->bases[i].cls->empty;
    if (c->explicit_layout)
        for (int i = 0; i < c->nfields; i++)
            if (c->fields[i]->bitwidth >= 0)
                cx_error(cx_cur(), "bit-fields in a class with bases or "
                                   "virtual functions are not supported "
                                   "yet");
    int private_field = 0;
    c->user_ctors = c->ctors != NULL;
    struct cfunc *ucopy = NULL, *umove = NULL, *ucopy_as = NULL,
                 *umove_as = NULL;
    for (struct cfunc *f = c->ctors; f; f = f->next) {
        int k = copy_kind(f, c);
        if (k == SP_COPY) ucopy = f;
        if (k == SP_MOVE) umove = f;
        if (f->type->np == 0 || (f->defargs && f->defargs[0]))
            c->has_default_ctor = 1;
    }
    for (struct cfunc *f = assign_set(c); f; f = f->next) {
        int k = copy_kind(f, c);
        if (k == SP_COPY_ASSIGN) ucopy_as = f;
        if (k == SP_MOVE_ASSIGN) umove_as = f;
    }
    struct cfunc *udtor = c->dtor;
    c->user_copy_ctor = ucopy && !ucopy->is_defaulted;
    c->user_move_ctor = umove && !umove->is_defaulted;
    c->user_copy_assign = ucopy_as && !ucopy_as->is_defaulted;
    c->user_move_assign = umove_as && !umove_as->is_defaulted;
    c->user_dtor = udtor != NULL;
    if (ucopy_as || umove_as)
        c->copy_assign = ucopy_as ? ucopy_as : umove_as;

    /* what the members make of copying, destroying, building, assigning */
    int mdtor = 1, mcopy = 1, mdefault = 1, massign = 1, mpod = 1;
    for (int i = 0; i < c->nfields; i++) {
        struct cfield *fl = c->fields[i];
        if (fl->access != CA_PUBLIC)
            private_field = 1;
        if (fl->dflt_tok >= 0)
            mdefault = mpod = 0;
        if (ct_is_ref(fl->type))
            mpod = 0;
        struct cty *e = base_elem(fl->type);
        if (e->k != CT_CLASS)
            continue;
        struct cclass *m = e->cls;
        mdtor &= m->trivial_dtor;
        mcopy &= m->trivial_copy;
        mdefault &= m->trivial_default;
        massign &= m->trivial_assign;
        mpod &= m->pod_layout;
    }
    int vdtor = udtor && udtor->is_virtual;
    for (int i = 0; i < c->nbases; i++) {
        struct cclass *b = c->bases[i].cls;
        mdtor &= b->trivial_dtor;
        mcopy &= b->trivial_copy;
        mdefault &= b->trivial_default;
        massign &= b->trivial_assign;
        if (b->dtor && b->dtor->is_virtual)
            vdtor = 1;
    }
    /* a class with virtual functions copies and builds its vptr: never
     * trivially (its destructor still may be) */
    if (c->dynamic)
        mcopy = mdefault = massign = 0;
    c->trivial_dtor = mdtor && !vdtor && !(udtor && !udtor->is_defaulted);
    c->trivial_copy = mcopy && !c->user_copy_ctor && !c->user_move_ctor;
    c->trivial_assign = massign && !c->user_copy_assign &&
                        !c->user_move_assign;
    /* a user-provided default constructor runs code however empty */
    int udefault = 0;
    for (struct cfunc *f = c->ctors; f; f = f->next)
        if ((f->type->np == 0 || (f->defargs && f->defargs[0])) &&
            !f->is_defaulted)
            udefault = 1;
    c->trivial_default = mdefault && !udefault &&
                         (!c->user_ctors || c->has_default_ctor);
    c->trivial_for_calls = c->trivial_copy && c->trivial_dtor;
    int plain_bases = 1;
    for (int i = 0; i < c->nbases; i++)
        plain_bases &= !c->bases[i].is_virtual &&
                       c->bases[i].access == CA_PUBLIC;
    c->aggregate = !c->user_ctors && !private_field && !c->dynamic &&
                   plain_bases;
    /* POD for the purpose of layout: C++03's POD (Itanium 2.2) */
    c->pod_layout = !c->user_ctors && !private_field && !c->nbases &&
                    !c->dynamic && mpod && !ucopy_as && !umove_as &&
                    !udtor;
    if (c->pod_layout)
        c->nvsize = c->dsize = c->size;
    if (!c->user_ctors)
        c->has_default_ctor = 1;

    /* the user's `= default`s: trivial or defined memberwise */
    for (struct cfunc *f = c->ctors; f; f = f->next)
        if (f->is_defaulted) {
            f->special = f->type->np == 0 ? SP_DEFAULT : copy_kind(f, c);
            f->trivial = f->special == SP_DEFAULT ? mdefault
                         : c->trivial_copy;
        }
    for (struct cfunc *f = assign_set(c); f; f = f->next)
        if (f->is_defaulted) {
            f->special = copy_kind(f, c);
            f->trivial = c->trivial_assign;
        }
    if (udtor && udtor->is_defaulted) {
        udtor->special = SP_DTOR;
        udtor->trivial = c->trivial_dtor;
    }

    /* the implicit declarations (11.4.5.2, 11.4.5.3, 11.4.6, 11.4.7) */
    int any_user_copy = ucopy || umove || ucopy_as || umove_as;
    if (!c->user_ctors)
        declare_implicit(c, SP_DEFAULT, c->trivial_default, 0);
    if (!ucopy)
        declare_implicit(c, SP_COPY, c->trivial_copy, umove || umove_as);
    if (!any_user_copy && !udtor)
        declare_implicit(c, SP_MOVE, c->trivial_copy, 0);
    if (!ucopy_as)
        declare_implicit(c, SP_COPY_ASSIGN, c->trivial_assign,
                         umove || umove_as);
    if (!any_user_copy && !udtor)
        declare_implicit(c, SP_MOVE_ASSIGN, c->trivial_assign, 0);
    if (!udtor) {
        struct cfunc *d = declare_implicit(c, SP_DTOR, c->trivial_dtor, 0);
        d->is_virtual = vdtor;            /* overriding a virtual one */
    }
    if (c->dynamic)
        build_vtables(c);
}

static int building_base;       /* constructing a base subobject, which
                                 * an abstract class may be */

/* The this parameter of a member function of c. */
static struct cvar *this_param(struct cclass *c, unsigned cv)
{
    struct cvar *tv = xcalloc(1, sizeof *tv);
    tv->name = tv->cname = "this";
    tv->type = ct_ptr(ct_qual(ct_class(c), cv));
    tv->is_local = tv->is_param = 1;
    return tv;
}

/* The member of the parameter `other` a copy or move reads: moved from
 * (an xvalue) by a move. */
static struct cexpr *source_member(struct cexpr *other, struct cfield *fl,
                                   int move)
{
    struct cexpr *m = ex_member(other, fl);
    if (move && !ct_is_ref(fl->type)) {
        struct cexpr *x = ex_new(E_CAST, m->t, VC_XVALUE);
        x->a = xmalloc(sizeof *x->a);
        x->a[0] = m;
        x->na = 1;
        x->lvcast = 1;
        return x;
    }
    return m;
}

/* A member array copied as a whole: its bytes, when its elements are
 * trivially copyable. */
static struct cexpr *array_copy(struct cfield *fl, struct cexpr *src)
{
    struct cty *e = base_elem(fl->type);
    if (e->k == CT_CLASS && !e->cls->trivial_copy)
        cx_error(cx_cur(), "copying an array of '%s' is not supported yet",
                 e->cls->name ? e->cls->name : "class");
    struct cexpr *r = ex_new(E_CONSTRUCT, fl->type, VC_PRVALUE);
    r->a = xmalloc(sizeof *r->a);
    r->a[0] = src;
    r->na = 1;
    return r;
}

void define_implicit(struct cfunc *f)
{
    if (f->defined)
        return;
    struct cclass *c = f->cls;
    f->defined = 1;
    f->is_inline = 1;
    f->body = st_new(S_BLOCK);
    f->this_var = this_param(c, 0);
    int np = f->type->np;
    f->params = xcalloc((size_t)(np ? np : 1), sizeof *f->params);
    struct cexpr *other = NULL;
    if (np) {
        struct cvar *o = xcalloc(1, sizeof *o);
        o->name = o->cname = "__cx_other";
        o->type = f->type->params[0];
        o->is_local = o->is_param = 1;
        o->fn = f;
        f->params[0] = o;
        other = ex_new(E_VAR, ct_strip_ref(o->type), VC_LVALUE);
        other->var = o;
    }
    struct cfunc *savefn = cx_curfn;
    cx_curfn = f;
    int move = f->special == SP_MOVE || f->special == SP_MOVE_ASSIGN;
    switch (f->special) {
    case SP_DEFAULT:
        ctor_meminit(f, NULL, 0);
        break;
    case SP_COPY: case SP_MOVE:
        f->baseinit = xcalloc((size_t)(c->nbases ? c->nbases : 1),
                              sizeof *f->baseinit);
        building_base = 1;
        for (int b = 0; b < c->nbases; b++) {
            struct cexpr *src = to_base(other, c->bases[b].cls, 0);
            if (move) {
                struct cexpr *x = ex_new(E_CAST, src->t, VC_XVALUE);
                x->a = xmalloc(sizeof *x->a);
                x->a[0] = src;
                x->na = 1;
                x->lvcast = 1;
                src = x;
            }
            struct cexpr *e = init_object(ct_class(c->bases[b].cls),
                                          INIT_DIRECT, &src, 1, NULL);
            if (e && e->k == E_CONSTRUCT)
                e->baseobj = 1;
            f->baseinit[b] = e;
        }
        building_base = 0;
        f->meminit = xcalloc((size_t)(c->nfields ? c->nfields : 1),
                             sizeof *f->meminit);
        for (int i = 0; i < c->nfields; i++) {
            struct cfield *fl = c->fields[i];
            if (!fl->name)
                continue;
            struct cexpr *m = source_member(other, fl, move);
            if (ct_is_ref(fl->type))
                f->meminit[i] = bind_ref(m, fl->type, "a copy");
            else if (fl->type->k == CT_ARRAY)
                f->meminit[i] = array_copy(fl, m);
            else
                f->meminit[i] = init_object(fl->type, INIT_DIRECT, &m, 1,
                                            NULL);
        }
        break;
    case SP_COPY_ASSIGN: case SP_MOVE_ASSIGN: {
        struct cstmt **tail = &f->body->body;
        struct cexpr *self = ex_deref(ex_this());
        for (int b = 0; b < c->nbases; b++) {
            struct cclass *bc = c->bases[b].cls;
            struct cexpr *src = to_base(other, bc, 0);
            if (move) {
                struct cexpr *x = ex_new(E_CAST, src->t, VC_XVALUE);
                x->a = xmalloc(sizeof *x->a);
                x->a[0] = src;
                x->na = 1;
                x->lvcast = 1;
                src = x;
            }
            struct cstmt *st = st_new(S_EXPR);
            st->e = expr_assign(TOK_ASSIGN, to_base(self, bc, 0), src);
            *tail = st;
            tail = &st->next;
        }
        for (int i = 0; i < c->nfields; i++) {
            struct cfield *fl = c->fields[i];
            if (!fl->name)
                continue;
            if (ct_is_ref(fl->type))
                cx_error(cx_cur(), "'%s' has a reference member: its copy "
                                   "assignment is deleted", c->name);
            struct cexpr *m = source_member(other, fl, move);
            struct cstmt *st = st_new(S_EXPR);
            if (fl->type->k == CT_ARRAY) {
                struct cexpr *dst = ex_member(self, fl);
                struct cexpr *cp = ex_new(E_BUILTIN,
                                          ct_ptr(ct_basic(CT_VOID)),
                                          VC_PRVALUE);
                cp->name = "__builtin_memcpy";
                cp->a = xmalloc(3 * sizeof *cp->a);
                cp->a[0] = ex_addr(dst);
                cp->a[1] = ex_addr(m);
                cp->a[2] = ex_int(ct_size(fl->type), ct_size_t());
                cp->na = 3;
                struct cty *el = base_elem(fl->type);
                if (el->k == CT_CLASS && !el->cls->trivial_assign)
                    cx_error(cx_cur(), "assigning an array of objects is "
                                       "not supported yet");
                st->e = cp;
            } else {
                st->e = expr_assign(TOK_ASSIGN, ex_member(self, fl), m);
            }
            *tail = st;
            tail = &st->next;
        }
        struct cstmt *r = st_new(S_RETURN);
        r->e = bind_ref(self, f->type->to, "return");
        *tail = r;
        break;
    }
    default:
        break;                  /* a destructor: emit.c destroys members */
    }
    cx_curfn = savefn;
}

/* A user's `= default`, defined when first used. */
static void define_defaulted(struct cfunc *f)
{
    if (f->defined)
        return;
    if (!f->special)
        cx_error(cx_cur(), "'%s' cannot be defaulted", f->name);
    define_implicit(f);
}

struct cfunc *class_dtor(struct cclass *c)
{
    if (c->trivial_dtor)
        return NULL;
    struct cfunc *d = c->dtor;
    if (d->is_implicit)
        define_implicit(d);
    else if (d->is_defaulted)
        define_defaulted(d);
    if (d->is_deleted)
        cx_error(cx_cur(), "the destructor of '%s' is deleted", c->name);
    d->used = 1;
    return d;
}

static int runs_no_code(struct cfunc *f)
{
    return (f->is_implicit || f->is_defaulted) && f->trivial;
}

static struct cexpr *ctor_call(struct cclass *c, struct cfunc *f,
                               struct cexpr **args, int na,
                               const struct ctok *at)
{
    if (f->is_deleted)
        cx_error(at, "use of deleted %s constructor of '%s'",
                 f->special == SP_COPY ? "copy" : f->special == SP_MOVE
                 ? "move" : "a", c->name ? c->name : "class");
    struct cexpr *e = ex_new(E_CONSTRUCT, ct_class(c), VC_PRVALUE);
    e->line = at->t.line;
    e->file = at->file;
    if (runs_no_code(f)) {
        /* a trivial default constructor does nothing; a trivial copy or
         * move is the object's bytes */
        if (f->special != SP_DEFAULT) {
            e->a = xmalloc(sizeof *e->a);
            e->a[0] = args[0];
            e->na = 1;
        }
        return e;
    }
    if (f->is_implicit)
        define_implicit(f);
    else if (f->is_defaulted)
        define_defaulted(f);
    struct cexpr **conv;
    int n = convert_args(f, args, na, at, &conv);
    e->fn = f;
    e->a = conv;
    e->na = n;
    f->used = 1;
    return e;
}

static struct cexpr *default_construct(struct cclass *c, int value,
                                       const struct ctok *at)
{
    struct cfunc *f = resolve(c->ctors, NULL, NULL, 0, NULL, NULL);
    if (!f)
        cx_error(at, "'%s' has no default constructor",
                 c->name ? c->name : "class");
    if (!value && runs_no_code(f))
        return NULL;
    struct cexpr *e = ctor_call(c, f, NULL, 0, at);
    /* value-initialization zeroes first where the constructor is not
     * the user's own */
    if (value && (f->is_implicit || f->is_defaulted))
        e->zero = 1;
    return e;
}

/* The copy (or move) of the object e, a glvalue or prvalue of class c. */
static struct cexpr *copy_from(struct cclass *c, struct cexpr *e,
                               int allow_explicit, const struct ctok *at)
{
    if (e->vc == VC_PRVALUE)
        return e;                 /* guaranteed copy elision */
    struct cfunc *f = resolve_ex(c->ctors, NULL, &e, 1, at,
                                 c->name ? c->name : "class",
                                 allow_explicit ? 0 : RS_NO_EXPLICIT);
    return ctor_call(c, f, &e, 1, at);
}

static int same_class(const struct cexpr *e, const struct cclass *c)
{
    return e->t && e->t->k == CT_CLASS && e->t->cls == c &&
           !(e->k == E_INITLIST && !e->t);
}

struct cexpr *construct(struct cclass *c, enum init_form form,
                        struct cexpr **args, int na, const struct ctok *at)
{
    if (!c->complete)
        cx_error(at, "'%s' is incomplete here", c->name ? c->name : "class");
    if (!building_base && c->dynamic && class_abstract(c))
        cx_error(at, "an object of abstract class '%s'", c->name);
    switch (form) {
    case INIT_DEFAULT:
        return default_construct(c, 0, at);
    case INIT_VALUE:
        return default_construct(c, 1, at);
    case INIT_COPY: {
        struct cexpr *e = args[0];
        if (e->k == E_INITLIST && !e->t)
            return construct(c, INIT_COPY_LIST, args, na, at);
        if (same_class(e, c))
            return copy_from(c, e, 0, at);
        struct cfunc *f = resolve_ex(c->ctors, NULL, &e, 1, at,
                                     c->name ? c->name : "class",
                                     RS_NO_USER | RS_NO_EXPLICIT);
        return ctor_call(c, f, &e, 1, at);
    }
    case INIT_DIRECT: {
        if (na == 1 && same_class(args[0], c))
            return copy_from(c, args[0], 1, at);
        if (!c->user_ctors) {
            if (c->aggregate) {
                /* C++20: an aggregate from a parenthesized list */
                struct cexpr l;
                memset(&l, 0, sizeof l);
                l.k = E_INITLIST;
                l.a = args;
                l.na = na;
                return init_aggregate(ct_class(c), &l, at);
            }
            cx_error(at, "no constructor of '%s' takes these arguments",
                     c->name);
        }
        struct cfunc *f = resolve(c->ctors, NULL, args, na, at,
                                  c->name ? c->name : "class");
        return ctor_call(c, f, args, na, at);
    }
    case INIT_LIST: case INIT_COPY_LIST: {
        struct cexpr *l = args[0];
        if (c->aggregate) {
            if (l->na == 1 && same_class(l->a[0], c))
                return copy_from(c, l->a[0], form == INIT_LIST, at);
            return init_aggregate(ct_class(c), l, at);
        }
        if (l->na == 0 && c->has_default_ctor)
            return default_construct(c, 1, at);
        if (l->na == 1 && same_class(l->a[0], c))
            return copy_from(c, l->a[0], form == INIT_LIST, at);
        struct cfunc *f = resolve(c->ctors, NULL, l->a, l->na, at,
                                  c->name ? c->name : "class");
        if (f->is_explicit && form == INIT_COPY_LIST)
            cx_error(at, "copy-list-initialization of '%s' chose an "
                         "explicit constructor", c->name);
        return ctor_call(c, f, l->a, l->na, at);
    }
    }
    return NULL;
}

void ctor_meminit(struct cfunc *f, struct meminit_raw *mi, int n)
{
    struct cclass *c = f->cls;
    const struct ctok *at = cx_cur();
    if (n == 1 && c->name && strcmp(mi[0].name, c->name) == 0) {
        /* a delegating constructor */
        f->delegate = mi[0].braced
                      ? construct(c, INIT_LIST, mi[0].args, 1, mi[0].at)
                      : mi[0].na == 0
                      ? construct(c, INIT_VALUE, NULL, 0, mi[0].at)
                      : construct(c, INIT_DIRECT, mi[0].args, mi[0].na,
                                  mi[0].at);
        return;
    }
    struct meminit_raw **by = xcalloc((size_t)(c->nfields ? c->nfields : 1),
                                      sizeof *by);
    struct meminit_raw **bby = xcalloc((size_t)(c->nbases ? c->nbases : 1),
                                       sizeof *bby);
    for (int i = 0; i < n; i++) {
        if (c->name && strcmp(mi[i].name, c->name) == 0)
            cx_error(mi[i].at, "a delegating constructor initializes "
                               "nothing else");
        int b;
        for (b = 0; b < c->nbases; b++)
            if (c->bases[b].cls->name &&
                strcmp(c->bases[b].cls->name, mi[i].name) == 0)
                break;
        if (b < c->nbases) {
            if (bby[b])
                cx_error(mi[i].at, "base '%s' is initialized twice",
                         mi[i].name);
            bby[b] = &mi[i];
            continue;
        }
        int k = 0;
        while (k < c->nfields && !(c->fields[k]->name &&
                                   strcmp(c->fields[k]->name,
                                          mi[i].name) == 0))
            k++;
        if (k == c->nfields)
            cx_error(mi[i].at, "'%s' has no member named '%s'",
                     c->name ? c->name : "class", mi[i].name);
        if (by[k])
            cx_error(mi[i].at, "'%s' is initialized twice", mi[i].name);
        by[k] = &mi[i];
    }
    /* the bases, in declaration order: as written, else by default */
    f->baseinit = xcalloc((size_t)(c->nbases ? c->nbases : 1),
                          sizeof *f->baseinit);
    building_base = 1;
    for (int b = 0; b < c->nbases; b++) {
        struct cclass *bc = c->bases[b].cls;
        struct meminit_raw *r = bby[b];
        struct cexpr *e;
        if (!r)
            e = construct(bc, INIT_DEFAULT, NULL, 0, at);
        else if (r->braced)
            e = construct(bc, INIT_LIST, r->args, 1, r->at);
        else if (r->na == 0)
            e = construct(bc, INIT_VALUE, NULL, 0, r->at);
        else
            e = construct(bc, INIT_DIRECT, r->args, r->na, r->at);
        if (e && e->k == E_CONSTRUCT)
            e->baseobj = 1;
        f->baseinit[b] = e;
    }
    building_base = 0;
    f->meminit = xcalloc((size_t)(c->nfields ? c->nfields : 1),
                         sizeof *f->meminit);
    for (int i = 0; i < c->nfields; i++) {
        struct cfield *fl = c->fields[i];
        struct meminit_raw *r = by[i];
        if (!fl->name)
            continue;
        if (r) {
            if (ct_is_ref(fl->type)) {
                struct cexpr *a = r->na == 1 ? r->args[0] : NULL;
                if (a && r->braced && a->k == E_INITLIST && a->na == 1)
                    a = a->a[0];
                if (!a)
                    cx_error(r->at, "reference member '%s' takes one value",
                             fl->name);
                f->meminit[i] = bind_ref(a, fl->type, "a mem-initializer");
            } else if (r->braced) {
                f->meminit[i] = init_object(fl->type, INIT_LIST, r->args, 1,
                                            r->at);
                if (!f->meminit[i])
                    f->meminit[i] = init_object(fl->type, INIT_VALUE, NULL,
                                                0, r->at);
            } else if (r->na == 0) {
                f->meminit[i] = init_object(fl->type, INIT_VALUE, NULL, 0,
                                            r->at);
            } else {
                f->meminit[i] = init_object(fl->type, INIT_DIRECT, r->args,
                                            r->na, r->at);
            }
            continue;
        }
        if (fl->dflt_tok >= 0 && !fl->dflt)
            field_parse_default(c, fl);
        if (fl->dflt) {
            f->meminit[i] = fl->dflt;
            continue;
        }
        if (ct_is_ref(fl->type))
            cx_error(at, "reference member '%s' of '%s' is not initialized",
                     fl->name, c->name ? c->name : "class");
        if (c->is_union)
            continue;
        f->meminit[i] = init_object(fl->type, INIT_DEFAULT, NULL, 0, at);
    }
}
