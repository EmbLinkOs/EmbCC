/* Virtual tables with virtual bases (Itanium 2.5, 2.6): the complete
 * object's base subobjects as a tree (a virtual base once, shared), the
 * vtable group of a class and the construction vtable groups its VTT
 * points into, vcall and vbase offsets, and what a constructor stores in
 * each vptr. The order of everything follows g++'s (class.cc:
 * build_vcall_and_vbase_vtbl_entries, accumulate_vtbl_inits,
 * build_vtt_inits), which is what makes the tables interchangeable.
 *
 * A class without virtual bases gets the same tables this way as it did
 * before virtual bases existed: no vcall or vbase offsets, no VTT. */
#include "cxx.h"

#include <string.h>

#include "../driver/util.h"

/* ---- subobjects ---- */

struct binfo {
    struct cclass *cls;
    long off;                 /* in the complete object */
    int virt;                 /* a virtual base (the one shared instance) */
    struct binfo *parent;     /* the inheritance chain: reached first from */
    struct binfo **kids;      /* per direct base (a virtual one: shared) */
    int nkids;
    struct binfo *primary;    /* the base that shares its vptr */
    int lost;                 /* ... would: a virtual one another subobject
                               * has as its primary (it is elsewhere) */
    int is_primary;           /* it is some binfo's primary base */
    int vtt_index;            /* its entry in the class's own VTT (-1) */
    int subvtt;               /* where its sub-VTT (or virtual VTT) starts */
    int seen;                 /* scratch */
};

static int vbase_slot(struct cclass *c, struct cclass *v)
{
    for (int i = 0; i < c->nvbases; i++)
        if (c->vbases[i].cls == v)
            return i;
    return -1;
}

static struct binfo *build(struct cclass *c, long off, int virt,
                           struct binfo *parent, struct binfo **vb,
                           struct cclass *root)
{
    struct binfo *x = xcalloc(1, sizeof *x);
    x->cls = c;
    x->off = off;
    x->virt = virt;
    x->parent = parent;
    x->vtt_index = x->subvtt = -1;
    x->nkids = c->nbases;
    x->kids = xcalloc((size_t)(c->nbases ? c->nbases : 1), sizeof *x->kids);
    for (int i = 0; i < c->nbases; i++) {
        struct cbase *b = &c->bases[i];
        if (b->is_virtual) {
            int k = vbase_slot(root, b->cls);
            if (k < 0)
                cx_error(NULL, "internal: virtual base '%s' of '%s' not "
                               "laid out", b->cls->name, root->name);
            if (!vb[k])
                vb[k] = build(b->cls, root->vbases[k].off, 1, x, vb, root);
            x->kids[i] = vb[k];
        } else {
            x->kids[i] = build(b->cls, off + b->off, 0, x, vb, root);
        }
    }
    for (int i = 0; c->primary && !c->primary_virt && i < c->nbases; i++)
        if (c->bases[i].cls == c->primary && !c->bases[i].is_virtual) {
            x->primary = x->kids[i];
            x->kids[i]->is_primary = 1;
            break;
        }
    return x;
}

/* Virtual primary bases: each is the primary of the first subobject (in
 * inheritance graph order) that has it as its class's primary, and is
 * reached through it (class.c's layout put it there). */
static void claim(struct binfo *x, struct binfo **vb, struct cclass *root)
{
    if (x->seen)
        return;
    x->seen = 1;
    struct cclass *c = x->cls;
    if (c->primary && c->primary_virt) {
        struct binfo *v = vb[vbase_slot(root, c->primary)];
        if (!v->is_primary) {
            v->is_primary = 1;
            v->parent = x;
        } else {
            x->lost = 1;
        }
        x->primary = v;
    }
    for (int i = 0; i < x->nkids; i++)
        claim(x->kids[i], vb, root);
}

static void unseen(struct binfo *x)
{
    x->seen = 0;
    for (int i = 0; i < x->nkids; i++)
        unseen(x->kids[i]);
}

/* The complete object of c, and its virtual bases' shared subobjects
 * (per c->vbases). */
struct tree {
    struct binfo *root;
    struct binfo **vb;
};

static struct tree tree_of(struct cclass *c)
{
    struct tree t;
    t.vb = xcalloc((size_t)(c->nvbases ? c->nvbases : 1), sizeof *t.vb);
    t.root = build(c, 0, 0, NULL, t.vb, c);
    claim(t.root, t.vb, c);
    unseen(t.root);
    return t;
}

/* Is y reached from r through a virtual base? */
static int via_virtual(struct binfo *y, struct binfo *r)
{
    for (struct binfo *b = y; b && b != r; b = b->parent)
        if (b->virt)
            return 1;
    return 0;
}

/* ---- overriding ---- */

static int same_sig(struct cfunc *a, struct cfunc *b)
{
    if (a->is_dtor || b->is_dtor)
        return a->is_dtor && b->is_dtor;
    if (strcmp(a->name, b->name) != 0)
        return 0;
    struct cty *x = a->type, *y = b->type;
    if (x->np != y->np || x->variadic != y->variadic || x->fq != y->fq ||
        x->refq != y->refq)
        return 0;
    for (int i = 0; i < x->np; i++)
        if (!ct_same_unqual(x->params[i], y->params[i]))
            return 0;
    return 1;
}

/* c's virtual member functions, in declaration order. */
static int virtuals_of(struct cclass *c, struct cfunc ***out)
{
    int n = 0, cap = 8;
    struct cfunc **v = xmalloc((size_t)cap * sizeof *v);
    for (struct cfunc *f = cx_funcs; f; f = f->all_next) {
        if (f->cls != c || f->is_ctor || f->is_static || !f->is_virtual)
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

/* c's own declaration with fn's signature (it or an overrider), if any. */
static struct cfunc *declared_here(struct cclass *c, struct cfunc *fn)
{
    struct cfunc **v;
    int n = virtuals_of(c, &v);
    for (int i = 0; i < n; i++)
        if (v[i] == fn || same_sig(v[i], fn))
            return v[i];
    return NULL;
}

/* Is b a subobject of a (a itself, or reached from it)? */
static int within(struct binfo *a, struct binfo *b)
{
    if (a == b)
        return 1;
    for (int i = 0; i < a->nkids; i++)
        if (within(a->kids[i], b))
            return 1;
    return 0;
}

struct cand {
    struct binfo *at;
    struct cfunc *fn;
};

static void overrider_paths(struct binfo *x, struct binfo *s,
                            struct cfunc *fn, struct binfo **path, int depth,
                            struct cand *c, int *nc)
{
    path[depth] = x;
    if (x == s) {
        /* on this path, the most derived class declaring it */
        for (int i = 0; i <= depth; i++) {
            struct cfunc *g = declared_here(path[i]->cls, fn);
            if (!g)
                continue;
            int have = 0;
            for (int k = 0; k < *nc; k++)
                have |= c[k].at == path[i];
            if (!have && *nc < 64) {
                c[*nc].at = path[i];
                c[*nc].fn = g;
                (*nc)++;
            }
            break;
        }
        return;
    }
    if (depth >= 255)
        return;
    for (int i = 0; i < x->nkids; i++)
        overrider_paths(x->kids[i], s, fn, path, depth + 1, c, nc);
}

/* The final overrider, within r's subobject, of fn as declared by s. */
static struct cand final_overrider(struct binfo *r, struct binfo *s,
                                   struct cfunc *fn)
{
    struct binfo *path[256];
    struct cand c[64];
    int nc = 0;
    overrider_paths(r, s, fn, path, 0, c, &nc);
    for (int i = 0; i < nc; i++) {
        int all = 1;
        for (int k = 0; k < nc && all; k++)
            all = within(c[i].at, c[k].at);
        if (all)
            return c[i];
    }
    if (nc == 0)
        cx_error(NULL, "internal: no overrider of '%s' in '%s'", fn->name,
                 r->cls->name);
    cx_error(NULL, "'%s' has no unique final overrider of '%s'", r->cls->name,
             fn->name);
    return c[0];
}

/* ---- vcall and vbase offsets ---- */

struct vid {
    struct binfo *binfo;      /* the vtable's subobject */
    struct binfo *rtti;       /* the group's root: overriders are its */
    struct binfo *derived;    /* the complete object's root */
    struct tree *t;
    int primary_vtbl;         /* binfo is the complete class's own: the
                               * vbase offsets' places are the class's */
    long index;               /* the next entry, in bytes from the address
                               * point (negative) */
    long *vals;               /* the entries, nearest first */
    int nvals, capvals;
    struct cfunc **fns;       /* functions with a vcall offset already */
    int nfns, capfns;
    struct binfo *vbase;      /* whose vcall offsets are being made */
    int generate;
    struct binfo **marked;    /* virtual bases with a vbase offset already */
    int nmarked;
    struct cclass *record;    /* computing this class's vcall indices */
};

static void push_val(struct vid *v, long x)
{
    if (v->nvals == v->capvals) {
        v->capvals = v->capvals ? v->capvals * 2 : 8;
        v->vals = xrealloc(v->vals, (size_t)v->capvals * sizeof *v->vals);
    }
    v->vals[v->nvals++] = x;
}

static void vbase_entries(struct binfo *y, struct vid *v)
{
    struct cclass *c = y->cls;
    for (int k = 0; k < c->nvbases; k++) {
        int m = vbase_slot(v->derived->cls, c->vbases[k].cls);
        struct binfo *b = v->t->vb[m];
        int have = 0;
        for (int i = 0; i < v->nmarked; i++)
            have |= v->marked[i] == b;
        if (have)
            continue;
        v->marked = xrealloc(v->marked, (size_t)(v->nmarked + 1) *
                                        sizeof *v->marked);
        v->marked[v->nmarked++] = b;
        if (v->primary_vtbl)
            v->derived->cls->vbases[m].vbindex = v->index;
        v->index -= 8;
        push_val(v, b->off - v->binfo->off);
    }
}

static void add_vcall(struct cfunc *fn, struct binfo *y, struct vid *v)
{
    for (int i = 0; i < v->nfns; i++)
        if (same_sig(v->fns[i], fn))
            return;
    if (v->record) {
        struct cclass *c = v->record;
        c->vcfns = xrealloc(c->vcfns, (size_t)(c->nvc + 1) *
                                      sizeof *c->vcfns);
        c->vcidx = xrealloc(c->vcidx, (size_t)(c->nvc + 1) *
                                      sizeof *c->vcidx);
        c->vcfns[c->nvc] = fn;
        c->vcidx[c->nvc] = v->index;
        c->nvc++;
    }
    v->index -= 8;
    if (v->nfns == v->capfns) {
        v->capfns = v->capfns ? v->capfns * 2 : 8;
        v->fns = xrealloc(v->fns, (size_t)v->capfns * sizeof *v->fns);
    }
    v->fns[v->nfns++] = fn;
    if (v->generate) {
        struct cand o = final_overrider(v->rtti, y, fn);
        push_val(v, o.at->off - v->binfo->off);
    }
}

static void add_vcall_r(struct binfo *y, struct vid *v)
{
    if (y->virt && v->vbase != y)
        return;
    if (y->primary)
        add_vcall_r(y->primary, v);
    struct cfunc **fs;
    int n = virtuals_of(y->cls, &fs);
    for (int i = 0; i < n; i++)
        add_vcall(fs[i], y, v);
    for (int i = 0; i < y->nkids; i++)
        if (y->kids[i] != y->primary)
            add_vcall_r(y->kids[i], v);
}

static void vcall_entries(struct binfo *y, struct vid *v)
{
    /* a virtual base's vtable has them — not when it is the root of a
     * construction group (a primary vtable there) */
    if (y != v->derived && !(y->virt && y != v->rtti))
        return;
    v->vbase = y;
    if (!y->virt)
        v->generate = 0;
    add_vcall_r(y, v);
}

static void vcall_and_vbase(struct binfo *y, struct vid *v)
{
    if (y->primary)
        vcall_and_vbase(y->primary, v);
    vbase_entries(y, v);
    vcall_entries(y, v);
}

/* The vcall offsets' places in a vtable of c as a virtual base: the
 * functions of c's non-virtual hierarchy, each once (for virtual thunks'
 * names and code). */
static void vcall_indices(struct cclass *c)
{
    if (c->vc_done)
        return;
    c->vc_done = 1;
    struct tree t = tree_of(c);
    struct vid v;
    memset(&v, 0, sizeof v);
    v.binfo = v.rtti = v.derived = t.root;
    v.t = &t;
    v.index = -24;
    v.record = c;
    vcall_and_vbase(t.root, &v);
}

static long vcall_index(struct cclass *c, struct cfunc *fn)
{
    vcall_indices(c);
    for (int i = 0; i < c->nvc; i++)
        if (same_sig(c->vcfns[i], fn))
            return c->vcidx[i];
    cx_error(NULL, "internal: no vcall offset for '%s' in '%s'", fn->name,
             c->name);
    return 0;
}

/* ---- vtables ---- */

/* Would b's virtual primary be claimed by b in r's own hierarchy (the
 * first subobject there, in inheritance graph order, having it)? */
static struct binfo *claimer_in(struct binfo *y, struct binfo *v)
{
    if (y->seen)
        return NULL;
    y->seen = 1;
    if (y->primary == v && y->cls->primary_virt)
        return y;
    for (int i = 0; i < y->nkids; i++) {
        struct binfo *c = claimer_in(y->kids[i], v);
        if (c)
            return c;
    }
    return NULL;
}

static int lost_in(struct binfo *r, struct binfo *b)
{
    if (!b->primary || !b->cls->primary_virt)
        return 0;
    if (!r)
        return b->lost;
    struct binfo *c = claimer_in(r, b->primary);
    unseen(r);
    return c != b;
}

/* Where fn, a slot of y's vtable, is declared along y's primary chain;
 * *lost when the chain passes a lost primary (lost in r's own hierarchy
 * when r, a construction group's root, is given). */
static struct binfo *first_defn(struct binfo *y, struct cfunc *fn, int *lost,
                                struct binfo *r)
{
    for (struct binfo *b = y; b; b = b->primary) {
        if (b->cls == fn->cls)
            return b;
        if (lost_in(r, b))
            *lost = 1;
    }
    cx_error(NULL, "internal: '%s' is in no primary base of '%s'", fn->name,
             y->cls->name);
    return y;
}

static struct vtbl build_vtbl(struct binfo *x, struct binfo *r,
                              struct tree *t, int ctor)
{
    struct vtbl out;
    memset(&out, 0, sizeof out);
    struct vid v;
    memset(&v, 0, sizeof v);
    v.binfo = x;
    v.rtti = r;
    v.derived = t->root;
    v.t = t;
    v.primary_vtbl = x == t->root;
    v.index = -24;
    v.generate = 1;
    vcall_and_vbase(x, &v);
    out.npre = v.nvals;
    out.pre = xmalloc((size_t)(v.nvals ? v.nvals : 1) * sizeof *out.pre);
    for (int i = 0; i < v.nvals; i++)
        out.pre[i] = v.vals[v.nvals - 1 - i];       /* farthest first */
    out.ott = r->off - x->off;
    out.rtti = r->cls;
    out.off = x->off - r->off;
    struct cclass *c = x->cls;
    out.nfns = c->nvtab;
    out.fns = xcalloc((size_t)(c->nvtab ? c->nvtab : 1), sizeof *out.fns);
    for (int i = 0; i < c->nvtab; i++) {
        struct vslot *s = &c->vtab[i];
        struct vfn *e = &out.fns[i];
        e->deleting = s->deleting;
        if (!s->f) {
            e->pure = 1;
            continue;
        }
        if (ctor && s->f->is_dtor)
            continue;                  /* never called while constructing */
        int lost = 0;
        struct binfo *fd = first_defn(x, s->f, &lost, ctor ? r : NULL);
        if (lost)
            continue;                  /* a lost primary's: never used */
        struct cand o = final_overrider(r, fd, s->f);
        if (o.fn->is_dtor && class_abstract(o.fn->cls))
            continue;                  /* no object of it is ever deleted */
        e->f = o.fn;
        e->pure = o.fn->is_pure;
        struct binfo *vb = NULL;
        for (struct binfo *b = fd; b; b = b->parent) {
            if (b->cls == o.at->cls)
                break;
            if (b->virt) {
                vb = b;
                break;
            }
        }
        if (vb) {
            e->delta = vb->off - fd->off;
            e->vcall = vcall_index(vb->cls, s->f);
        } else if (!lost) {
            e->delta = o.at->off - x->off;
        }                     /* through a lost primary: never called so */
    }
    return out;
}

struct gbuild {
    struct tree *t;
    struct binfo *r;
    int ctor;
    struct vtbl *v;
    struct binfo **of;        /* each vtable's subobject */
    int n, cap;
};

static void add_vtbl(struct gbuild *g, struct binfo *x)
{
    if (g->n == g->cap) {
        g->cap = g->cap ? g->cap * 2 : 4;
        g->v = xrealloc(g->v, (size_t)g->cap * sizeof *g->v);
        g->of = xrealloc(g->of, (size_t)g->cap * sizeof *g->of);
    }
    g->v[g->n] = build_vtbl(x, g->r, g->t, g->ctor);
    g->of[g->n] = x;
    g->n++;
}

/* Does x use another subobject's vtable in g? A primary base does its
 * derived class's — but in a construction group, a virtual primary only
 * when the one it is primary of (maybe through more primaries) is the
 * group's root or a virtual base within it; else it gets its own. */
static int shares_vtable(struct gbuild *g, struct binfo *x)
{
    if (x == g->r || !x->is_primary)
        return 0;
    if (!g->ctor || !x->virt)
        return 1;
    struct binfo *b = x, *last = NULL;
    while (b->is_primary && b->parent) {
        b = b->parent;
        last = b;
        if (b->virt || b == g->r)
            goto found;
    }
    for (b = last; b; b = b->parent)
        if (b->virt || b == g->r)
            break;
found:
    return b == g->r || (b && b->virt && vbase_slot(g->r->cls, b->cls) >= 0);
}

static void accumulate(struct gbuild *g, struct binfo *x)
{
    if (!x->cls->dynamic)
        return;
    if (g->ctor && !x->cls->nvbases && !via_virtual(x, g->r))
        return;
    if (!shares_vtable(g, x))
        add_vtbl(g, x);
    for (int i = 0; i < x->nkids; i++)
        if (!x->kids[i]->virt)
            accumulate(g, x->kids[i]);
}

static struct vgroup *group_of(struct tree *t, struct binfo *r, int ctor)
{
    struct gbuild g;
    memset(&g, 0, sizeof g);
    g.t = t;
    g.r = r;
    g.ctor = ctor;
    accumulate(&g, r);
    struct cclass *rc = r->cls;
    for (int k = 0; k < rc->nvbases; k++) {
        int m = vbase_slot(t->root->cls, rc->vbases[k].cls);
        accumulate(&g, t->vb[m]);
    }
    struct vgroup *out = xcalloc(1, sizeof *out);
    out->v = g.v;
    out->of = (void **)g.of;
    out->n = g.n;
    int at = 0;
    for (int i = 0; i < g.n; i++) {
        g.v[i].point = at + g.v[i].npre + 2;
        at += g.v[i].npre + 2 + g.v[i].nfns;
    }
    out->words = at;
    return out;
}

/* The address point, in g, of the vtable x uses (its own, or the one of
 * the class it is the primary base of). */
static int point_in(struct vgroup *g, struct binfo *x, struct binfo *r)
{
    for (;;) {
        for (int i = 0; i < g->n; i++)
            if (g->of[i] == x)
                return g->v[i].point;
        if (x == r || !x->parent)
            break;
        x = x->parent;
    }
    cx_error(NULL, "internal: no vtable for a subobject of '%s'",
             r->cls->name);
    return 0;
}

/* ---- the class's tables ---- */

struct vinfo {
    struct tree t;
    int tables;               /* the groups and the VTT are made */
    struct vgroup *main;
    struct ctorgrp *ctors;
    int nctors;
    struct vttent *vtt;
    int nvtt;
    struct vstore *stores;
    int nstores;
};

static void push_vtt(struct vinfo *vi, int ctor, int point)
{
    vi->vtt = xrealloc(vi->vtt, (size_t)(vi->nvtt + 1) * sizeof *vi->vtt);
    vi->vtt[vi->nvtt].ctor = ctor;
    vi->vtt[vi->nvtt].point = point;
    vi->nvtt++;
}

struct vttwalk {
    struct vinfo *vi;
    struct binfo *x;
    int top;
    int grp;                  /* -1: the main group, else a ctor group */
    struct vgroup *g;
};

static void secondary_vptrs(struct vttwalk *w, struct binfo *y)
{
    if (y->seen)
        return;
    y->seen = 1;
    if (!y->cls->dynamic)
        return;
    if (y != w->x) {
        if (!y->cls->nvbases && !via_virtual(y, w->x))
            return;
        if (y->virt || !y->is_primary) {
            struct binfo *use = y;
            if (w->top) {
                y->vtt_index = w->vi->nvtt;
                while (use->is_primary && use->parent)
                    use = use->parent;
            }
            push_vtt(w->vi, w->grp, point_in(w->g, use, w->x));
        }
    }
    for (int i = 0; i < y->nkids; i++)
        secondary_vptrs(w, y->kids[i]);
}

static void clear_seen(struct binfo *y)
{
    y->seen = 0;
    for (int i = 0; i < y->nkids; i++)
        clear_seen(y->kids[i]);
}

static void vtt_inits(struct vinfo *vi, struct binfo *x, int top)
{
    if (!x->cls->nvbases)
        return;
    struct vgroup *g = vi->main;
    int grp = -1;
    if (!top) {
        grp = vi->nctors;
        vi->ctors = xrealloc(vi->ctors, (size_t)(vi->nctors + 1) *
                                        sizeof *vi->ctors);
        vi->ctors[grp].base = x->cls;
        vi->ctors[grp].off = x->off;
        vi->ctors[grp].g = g = group_of(&vi->t, x, 1);
        vi->nctors++;
        x->subvtt = vi->nvtt;
    }
    if (top)
        x->vtt_index = vi->nvtt;
    push_vtt(vi, grp, point_in(g, x, x));
    for (int i = 0; i < x->nkids; i++)
        if (!x->kids[i]->virt)
            vtt_inits(vi, x->kids[i], 0);
    struct vttwalk w = { vi, x, top, grp, g };
    clear_seen(vi->t.root);
    secondary_vptrs(&w, x);
    if (top)
        for (int k = 0; k < x->cls->nvbases; k++)
            vtt_inits(vi, vi->t.vb[k], 0);
}

/* c's subobjects, and where its primary vtable keeps each virtual base's
 * offset (what a conversion needs, before any vtable is made). */
static struct vinfo *tree_info(struct cclass *c)
{
    if (c->vinfo)
        return c->vinfo;
    struct vinfo *vi = xcalloc(1, sizeof *vi);
    c->vinfo = vi;
    vi->t = tree_of(c);
    struct vid v;
    memset(&v, 0, sizeof v);
    v.binfo = v.rtti = v.derived = vi->t.root;
    v.t = &vi->t;
    v.primary_vtbl = 1;
    v.index = -24;
    vcall_and_vbase(vi->t.root, &v);  /* as the primary vtable lays them */
    return vi;
}

static struct vinfo *vinfo_of(struct cclass *c)
{
    struct vinfo *vi = tree_info(c);
    if (vi->tables)
        return vi;
    vi->tables = 1;
    vi->main = group_of(&vi->t, vi->t.root, 0);
    vtt_inits(vi, vi->t.root, 1);
    /* a constructor's vptr stores: each vtable of the group, the primary
     * first (the others' virtual bases are found through it) */
    struct vgroup *g = vi->main;
    vi->stores = xcalloc((size_t)(g->n ? g->n : 1), sizeof *vi->stores);
    for (int i = 0; i < g->n; i++) {
        struct binfo *y = g->of[i];
        struct vstore *s = &vi->stores[vi->nstores++];
        s->point = g->v[i].point;
        s->vtt = c->nvbases ? y->vtt_index : -1;
        struct binfo *vb = NULL;
        for (struct binfo *b = y; b; b = b->parent)
            if (b->virt) {
                vb = b;
                break;
            }
        if (vb) {
            s->virt = 1;
            s->vbindex = c->vbases[vbase_slot(c, vb->cls)].vbindex;
            s->off = y->off - vb->off;
        } else {
            s->off = y->off;
        }
    }
    return vi;
}

struct vgroup *vtable_group(struct cclass *c)
{
    return vinfo_of(c)->main;
}

int class_ctor_groups(struct cclass *c, struct ctorgrp **out)
{
    struct vinfo *vi = vinfo_of(c);
    *out = vi->ctors;
    return vi->nctors;
}

int class_vtt(struct cclass *c, struct vttent **out)
{
    struct vinfo *vi = vinfo_of(c);
    *out = vi->vtt;
    return vi->nvtt;
}

int class_vstores(struct cclass *c, struct vstore **out)
{
    struct vinfo *vi = vinfo_of(c);
    *out = vi->stores;
    return vi->nstores;
}

/* Where direct base i's sub-VTT starts in c's VTT (-1: it needs none). */
int class_subvtt(struct cclass *c, int i)
{
    struct vinfo *vi = vinfo_of(c);
    struct binfo *b = vi->t.root->kids[i];
    return b->virt ? -1 : b->subvtt;
}

/* Where virtual base k's VTT starts in c's (-1: it needs none). */
int class_vvtt(struct cclass *c, int k)
{
    return vinfo_of(c)->t.vb[k]->subvtt;
}

long class_vbindex(struct cclass *c, struct cclass *v)
{
    tree_info(c);
    int k = vbase_slot(c, v);
    return k < 0 ? 0 : c->vbases[k].vbindex;
}

/* A pure virtual function whose final overrider in c is pure. */
static int any_pure(struct binfo *root, struct binfo *y)
{
    if (y->seen)
        return 0;
    y->seen = 1;
    struct cfunc **fs;
    int n = virtuals_of(y->cls, &fs);
    for (int i = 0; i < n; i++)
        if (fs[i]->is_pure && final_overrider(root, y, fs[i]).fn->is_pure)
            return 1;
    for (int i = 0; i < y->nkids; i++)
        if (any_pure(root, y->kids[i]))
            return 1;
    return 0;
}

int class_abstract(struct cclass *c)
{
    if (!c->dynamic)
        return 0;
    if (!c->abstract_known) {
        struct vinfo *vi = tree_info(c);
        c->abstract_known = 1;
        c->abstract = any_pure(vi->t.root, vi->t.root);
        unseen(vi->t.root);
    }
    return c->abstract;
}

/* The flags of c's __vmi_class_type_info: a non-virtual base repeated
 * (1), a virtual one reached twice (2). */
static void hint_walk(struct binfo *x, struct cclass **mk, int *mkv, int *n,
                      int *hint)
{
    int i;
    for (i = 0; i < *n; i++)
        if (mk[i] == x->cls)
            break;
    if (i == *n) {
        mk[i] = x->cls;
        mkv[i] = 0;
        (*n)++;
    }
    if (x->virt) {
        if (mkv[i] & 1)
            *hint |= 1;
        if (mkv[i] & 2)
            *hint |= 2;
        mkv[i] |= 2;
    } else {
        if (mkv[i])
            *hint |= 1;
        mkv[i] |= 1;
    }
    for (int k = 0; k < x->nkids; k++)
        hint_walk(x->kids[k], mk, mkv, n, hint);
}

int class_rtti_flags(struct cclass *c)
{
    struct vinfo *vi = tree_info(c);
    struct cclass *mk[512];
    int mkv[512], n = 0, hint = 0;
    hint_walk(vi->t.root, mk, mkv, &n, &hint);
    return hint;
}

/* The base subobjects of c of class b: how many (a virtual base once),
 * and the path to the one there is: *vb the last virtual base on it
 * (NULL: none), *off the offset from it (or from c). */
static void paths(struct binfo *x, struct cclass *b, struct binfo *vb,
                  struct binfo **found, int *n, struct binfo **fvb)
{
    if (x->virt)
        vb = x;
    if (x->cls == b) {
        int have = 0;
        for (int i = 0; i < *n && i < 64; i++)
            have |= found[i] == x;
        if (!have) {
            if (*n < 64)
                found[*n] = x;
            if (*n == 0)
                *fvb = vb;
            (*n)++;
        }
        return;
    }
    for (int i = 0; i < x->nkids; i++)
        paths(x->kids[i], b, vb, found, n, fvb);
}

int class_base_path(struct cclass *c, struct cclass *b, struct cclass **vb,
                    long *off)
{
    class_ensure(c);
    if (!c->complete)
        return 0;
    struct vinfo *vi = tree_info(c);
    struct binfo *found[64], *fvb = NULL;
    int n = 0;
    paths(vi->t.root, b, NULL, found, &n, &fvb);
    if (n == 1) {
        *vb = fvb ? fvb->cls : NULL;
        *off = found[0]->off - (fvb ? fvb->off : 0);
    }
    return n;
}
