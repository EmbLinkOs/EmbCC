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

static void layout(struct cclass *c)
{
    long bits = 0, align = 1, size = 0;
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
                bits = round_up(bits, fa * 8);
                continue;
            }
            if (!c->packed && bits / unit != (bits + w - 1) / unit)
                bits = round_up(bits, unit);
            fl->off = bits / 8 / fa * fa;
            bits += w;
            if (fl->name && fa > align)
                align = fa;
            continue;
        }
        long off = round_up((bits + 7) / 8, fa);
        fl->off = off;
        bits = (off + fs) * 8;
        if (fa > align)
            align = fa;
    }
    if (!c->is_union)
        size = (bits + 7) / 8;
    if (c->align_attr > align)
        align = c->align_attr;
    size = round_up(size, align);
    if (size == 0)
        size = 1;                     /* an empty class is one byte */
    c->size = size;
    c->align = align;
}

static int is_copy_ctor(struct cfunc *f, struct cclass *c, int rvalue)
{
    struct cty *ft = f->type;
    if (ft->np < 1 || (ft->np > 1 && (!f->defargs || !f->defargs[1])))
        return 0;
    struct cty *p = ft->params[0];
    if (p->k != (rvalue ? CT_RREF : CT_LREF))
        return 0;
    return p->to->k == CT_CLASS && p->to->cls == c;
}

void class_complete(struct cclass *c)
{
    layout(c);
    c->complete = 1;
    int user_ctor = 0, private_field = 0;
    for (struct cfunc *f = c->ctors; f; f = f->next) {
        user_ctor = 1;
        if (is_copy_ctor(f, c, 0) && !f->is_defaulted)
            c->user_copy_ctor = 1;
        if (is_copy_ctor(f, c, 1) && !f->is_defaulted)
            c->user_move_ctor = 1;
        if (f->type->np == 0 || (f->defargs && f->defargs[0]))
            c->has_default_ctor = 1;
    }
    struct csym *as = scope_find_here(c->scope, "operator=");
    if (as && as->k == CS_FUNC)
        for (struct cfunc *f = as->fns; f; f = f->next) {
            if (f->type->np != 1)
                continue;
            struct cty *p = f->type->params[0];
            struct cty *pb = ct_strip_ref(p);
            if (pb->k != CT_CLASS || pb->cls != c)
                continue;
            if (p->k == CT_RREF)
                c->user_move_assign = 1;
            else
                c->user_copy_assign = 1;
            c->copy_assign = f;
        }
    int user_dtor = c->dtor && !c->dtor->is_defaulted;
    c->user_dtor = c->dtor != NULL;
    int tdtor = !user_dtor, tcopy = !c->user_copy_ctor && !c->user_move_ctor;
    int tdefault = 1;
    for (int i = 0; i < c->nfields; i++) {
        struct cfield *fl = c->fields[i];
        if (fl->access != CA_PUBLIC)
            private_field = 1;
        if (fl->dflt_tok >= 0)
            tdefault = 0;
        struct cty *e = base_elem(fl->type);
        if (e->k != CT_CLASS)
            continue;
        struct cclass *m = e->cls;
        tdtor &= m->trivial_dtor;
        tcopy &= m->trivial_copy;
        tdefault &= m->trivial_default;
    }
    /* a user-provided default constructor runs code however empty */
    for (struct cfunc *f = c->ctors; f; f = f->next)
        if ((f->type->np == 0 || (f->defargs && f->defargs[0])) &&
            !f->is_defaulted)
            tdefault = 0;
    c->trivial_dtor = tdtor;
    c->trivial_copy = tcopy;
    c->trivial_default = tdefault && !user_ctor;
    c->trivial_for_calls = tcopy && tdtor;
    c->aggregate = !user_ctor && !private_field;
    if (!user_ctor)
        c->has_default_ctor = 1;
}

/* The this parameter of a member function of c. */
static struct cvar *this_param(struct cclass *c, unsigned cv)
{
    struct cvar *tv = xcalloc(1, sizeof *tv);
    tv->name = tv->cname = "this";
    tv->type = ct_ptr(ct_qual(ct_class(c), cv));
    tv->is_local = tv->is_param = 1;
    return tv;
}

/* A special member the compiler defines: an empty body, the member
 * initialization or destruction added around it (emit.c). */
static struct cfunc *make_special(struct cclass *c, int dtor)
{
    if (!c->name)
        cx_error(cx_cur(), "an unnamed class needs a constructor or "
                           "destructor: not supported yet");
    struct cfunc *f = xcalloc(1, sizeof *f);
    f->name = dtor ? cx_fmt("~%s", c->name) : c->name;
    f->type = ct_func(ct_basic(CT_VOID), NULL, 0, 0);
    f->owner = c->scope;
    f->cls = c;
    f->is_ctor = !dtor;
    f->is_dtor = dtor;
    f->is_implicit = 1;
    f->is_inline = 1;
    f->defined = 1;
    f->body_tok = f->mi_tok = -1;
    f->this_var = this_param(c, 0);
    f->body = st_new(S_BLOCK);
    f->params = xcalloc(1, sizeof *f->params);
    func_register(f);
    if (!dtor)
        ctor_meminit(f, NULL, 0);
    return f;
}

/* A `= default` special member, defined when first used. */
static void define_defaulted(struct cfunc *f)
{
    if (f->defined)
        return;
    f->defined = 1;
    f->is_inline = 1;
    f->body = st_new(S_BLOCK);
    f->this_var = this_param(f->cls, 0);
    f->params = xcalloc((size_t)(f->type->np ? f->type->np : 1),
                        sizeof *f->params);
    if (f->is_ctor) {
        if (f->type->np != 0)
            cx_error(cx_cur(), "a defaulted copy or move constructor of '%s' "
                               "is not supported yet (CX2)", f->cls->name);
        ctor_meminit(f, NULL, 0);
    }
}

struct cfunc *class_dtor(struct cclass *c)
{
    if (c->trivial_dtor)
        return NULL;
    if (!c->dtor)
        c->dtor = make_special(c, 1);
    else if (c->dtor->is_defaulted)
        define_defaulted(c->dtor);
    if (c->dtor->is_deleted)
        cx_error(cx_cur(), "the destructor of '%s' is deleted", c->name);
    c->dtor->used = 1;
    return c->dtor;
}

static struct cexpr *ctor_call(struct cclass *c, struct cfunc *f,
                               struct cexpr **args, int na,
                               const struct ctok *at)
{
    if (f->is_deleted)
        cx_error(at, "use of deleted constructor of '%s'", c->name);
    if (f->is_defaulted)
        define_defaulted(f);
    struct cexpr **conv;
    int n = convert_args(f, args, na, at, &conv);
    struct cexpr *e = ex_new(E_CONSTRUCT, ct_class(c), VC_PRVALUE);
    e->line = at->t.line;
    e->file = at->file;
    e->fn = f;
    e->a = conv;
    e->na = n;
    f->used = 1;
    return e;
}

static struct cexpr *default_construct(struct cclass *c, int value,
                                       const struct ctok *at)
{
    if (c->ctors) {
        struct cfunc *f = resolve(c->ctors, NULL, NULL, 0, NULL, NULL);
        if (!f)
            cx_error(at, "'%s' has no default constructor", c->name);
        struct cexpr *e = ctor_call(c, f, NULL, 0, at);
        /* value-init zeroes first where the constructor is not the
         * user's own */
        if (value && f->is_defaulted)
            e->zero = 1;
        return e;
    }
    if (!value && c->trivial_default)
        return NULL;
    struct cexpr *e = ex_new(E_CONSTRUCT, ct_class(c), VC_PRVALUE);
    e->line = at->t.line;
    e->file = at->file;
    e->zero = value;
    if (!c->trivial_default) {
        if (!c->implicit_ctor)
            c->implicit_ctor = make_special(c, 0);
        e->fn = c->implicit_ctor;
        e->fn->used = 1;
    }
    return e;
}

/* The copy (or move) of the object e, a glvalue or prvalue of class c. */
static struct cexpr *copy_from(struct cclass *c, struct cexpr *e,
                               int allow_explicit, const struct ctok *at)
{
    if (e->vc == VC_PRVALUE)
        return e;                 /* guaranteed copy elision */
    if (c->user_copy_ctor || c->user_move_ctor) {
        struct cfunc *f = resolve_ex(c->ctors, NULL, &e, 1, at, c->name,
                                     allow_explicit ? 0 : RS_NO_EXPLICIT);
        return ctor_call(c, f, &e, 1, at);
    }
    if (!c->trivial_copy)
        cx_error(at, "copying a '%s' needs its implicit copy constructor, "
                     "which is not supported yet (CX2)", c->name);
    struct cexpr *r = ex_new(E_CONSTRUCT, ct_class(c), VC_PRVALUE);
    r->line = e->line;
    r->file = e->file;
    r->a = xmalloc(sizeof *r->a);
    r->a[0] = e;
    r->na = 1;
    return r;
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
        if (!c->ctors)
            cx_error(at, "cannot convert '%s' to '%s'", ct_name(e->t),
                     c->name);
        struct cfunc *f = resolve_ex(c->ctors, NULL, &e, 1, at, c->name,
                                     RS_NO_USER | RS_NO_EXPLICIT);
        return ctor_call(c, f, &e, 1, at);
    }
    case INIT_DIRECT: {
        if (na == 1 && same_class(args[0], c))
            return copy_from(c, args[0], 1, at);
        if (!c->ctors) {
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
        struct cfunc *f = resolve(c->ctors, NULL, args, na, at, c->name);
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
        struct cfunc *f = resolve(c->ctors, NULL, l->a, l->na, at, c->name);
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
    for (int i = 0; i < n; i++) {
        if (c->name && strcmp(mi[i].name, c->name) == 0)
            cx_error(mi[i].at, "a delegating constructor initializes "
                               "nothing else");
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
