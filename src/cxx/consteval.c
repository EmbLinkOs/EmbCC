/* Constant evaluation (7.7): the value of a constant expression, calls of
 * constexpr functions included — an interpreter over the front-end's own
 * trees, the ones emit.c writes as C.
 *
 * An object is a block of bytes laid out as the target lays it out (both
 * targets are little-endian), so members, bases and array elements are
 * offsets, and copying an object is copying its bytes. A pointer is a
 * block and an offset in it (or a function); stored in an object, it is
 * the index of a handle holding the two, so a copied object's pointers
 * still point where they did. Each local of a call gets a block of its
 * own; a reference is bound to the object it names.
 *
 * What the evaluation cannot do — read a variable that is not a constant,
 * call a function that is not constexpr (or has no body), go past an
 * array's end, divide by zero, run too long — fails it quietly: the
 * expression is then simply not a constant, and the caller says so if it
 * needed one. Not modelled: virtual bases, dynamic allocation, long
 * double, unions' active members, the end of lifetimes (destructors do
 * not run).
 *
 * A failure is either what makes an expression not a constant one —
 * undefined behaviour, a call of a function that is not constexpr (a
 * throw's helper), a throw, a write to a constant, a read of what is not
 * one — or only what this interpreter does not do. notconst() says the
 * first (with why): a consteval function's call that fails so is an
 * error (cx_immediate); one that fails otherwise runs at run time.
 *
 * Virtual calls go by the object's dynamic type: a constructor, its bases
 * and members built, records its class for the object (and so for its
 * base subobjects); the final overrider is the first class on the path
 * from there down to the called function's that declares one.
 *
 * An integer is 128 bits wide here, i its low half and hi its high one:
 * for the narrower types hi is what extending i as the type says gives
 * (fit), for __int128 its own; its arithmetic is w128's. */
#include "cxx.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../driver/util.h"
#include "../sema/ldfloat.h"
#include "../sema/w128.h"

struct cblk {
    unsigned char *b;
    long size;
    int readonly;             /* a constant's: writing fails */
    int opaque;               /* a non-constant's (only its address is
                               * known): reading fails too */
};

struct cptr {
    int bw, bo;               /* a bit-field's lvalue: its width, and its
                               * first bit in the byte at off */
    struct cblk *blk;         /* NULL (with no fn): the null pointer */
    long off;
    struct cfunc *fn;         /* a pointer to a function */
};

enum { V_VOID, V_INT, V_FLT, V_PTR, V_OBJ };

struct cval {
    int k;
    long i;                   /* V_INT (the low half) */
    long hi;                  /* V_INT: the high half (fit) */
    double f;                 /* V_FLT */
    /* V_FLT of type long double: the EXACT value in the target's format.
     * `f` still holds its double approximation so every path that only
     * needs a double keeps working, but arithmetic and comparison must
     * use this one -- the host's own long double may be narrower than the
     * target's (plain double on arm64 macOS), and two distinct target
     * long doubles can round to the same double, so deciding `a < b`
     * through `f` would answer a different question. NULL for float and
     * double. */
    struct ldf *ld;
    struct cptr p;            /* V_PTR; V_OBJ: where the object is */
};

/* a local: its object (a reference: the object it is bound to) */
struct cbind {
    struct cvar *v;
    struct cptr at;
};

struct cframe {
    struct cbind *b;
    int n, cap;
    struct cptr self;         /* `this` */
    int has_self;
    struct cval ret;          /* what a return gave */
    struct cptr slot;         /* a class result: built here */
    int has_slot;
};

enum { X_NORMAL, X_BREAK, X_CONTINUE, X_RETURN };

static struct cptr *handles;
static long nhandles, caphandles;

static jmp_buf *fail_to;
static struct cframe *fr;
static long steps;
static int depth;
static struct cstmt *seek;    /* a switch's jump: the label looked for */

#define MAX_STEPS 20000000L
#define MAX_DEPTH 512

/* noreturn, and it has to SAY so: it leaves through longjmp, which no
 * analysis can see, so without the attribute every `... else no();` looks
 * like a path that falls through with nothing assigned. */
__attribute__((noreturn)) static void no(void)
{
    longjmp(*fail_to, 1);
}

static int definite;          /* the failure makes it not a constant */
static char why[256];

static void notconst(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, sizeof why, fmt, ap);
    va_end(ap);
    definite = 1;
    no();
}

static void step(void)
{
    if (++steps > MAX_STEPS)
        no();
}

static struct cblk *new_blk(long size)
{
    struct cblk *k = xcalloc(1, sizeof *k);
    k->size = size;
    k->b = xcalloc((size_t)(size > 0 ? size : 1), 1);
    return k;
}

static struct cptr at_off(struct cptr p, long off)
{
    p.off += off;
    return p;
}

/* a member of the object at p: a bit-field's lvalue by its bits */
static struct cptr field_at(struct cptr p, struct cfield *fl)
{
    if (fl->bitwidth < 0)
        return at_off(p, fl->off);
    p.off += fl->bitpos / 8;
    p.bo = (int)(fl->bitpos % 8);
    p.bw = fl->bitwidth;
    return p;
}

static struct cval v_int(long i)
{
    struct cval v;
    memset(&v, 0, sizeof v);
    v.k = V_INT;
    v.i = i;
    v.hi = i < 0 ? -1 : 0;
    return v;
}

static struct cval v_w(struct w128 w)
{
    struct cval v = v_int((long)w.lo);
    v.hi = (long)w.hi;
    return v;
}

static struct w128 w_of(struct cval v)
{
    return w_make((unsigned long)v.i, (unsigned long)v.hi);
}

static int is128(const struct cty *t)
{
    return t && (t->k == CT_INT128 || t->k == CT_UINT128);
}

/* does a long hold v's value (as its signed 128 bits say)? */
static int fits_long(struct cval v)
{
    return v.hi == (v.i < 0 ? -1 : 0);
}

static struct cval v_flt(double f)
{
    struct cval v;
    memset(&v, 0, sizeof v);
    v.k = V_FLT;
    v.f = f;
    return v;
}

/* A long double value, exact. */
static struct cval v_ldf(struct ldf *x)
{
    struct cval v;
    memset(&v, 0, sizeof v);
    v.k = V_FLT;
    v.ld = x;
    v.f = ldf_to_double(x);
    return v;
}

/* The exact value of a V_FLT, whatever width it came from. */
static struct ldf *as_ldf(struct cval v)
{
    return v.ld ? v.ld : ldf_from_double(v.f);
}

static struct cval v_ptr(struct cptr p)
{
    struct cval v;
    memset(&v, 0, sizeof v);
    v.k = V_PTR;
    v.p = p;
    return v;
}

static struct cval v_obj(struct cptr p)
{
    struct cval v = v_ptr(p);
    v.k = V_OBJ;
    return v;
}

static struct cval v_void(void)
{
    struct cval v;
    memset(&v, 0, sizeof v);
    return v;
}

static long trunc_to(long v, const struct cty *t)
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

/* the integer v as a value of integer type t: truncated to it, and its
 * high half what extending it as t says */
static struct cval fit(struct cval v, const struct cty *t)
{
    if (v.k != V_INT || !ct_is_integer(t))
        return v;
    if (t->k == CT_BOOL)
        return v_int(v.i != 0 || v.hi != 0);
    if (is128(t))
        return v;
    struct cval r = v_int(trunc_to(v.i, t));
    if (ct_size(t) == 8 && !ct_is_signed(t))
        r.hi = 0;
    return r;
}

/* ---- memory ---- */

static void check_range(struct cptr p, long n, int write)
{
    if (!p.blk)
        notconst("a null pointer is dereferenced");
    if (p.off < 0 || p.off + n > p.blk->size)
        notconst("an access is outside its object");
    if (p.blk->opaque)
        notconst("an object that is not a constant is read");
    if (write && p.blk->readonly)
        notconst("a constant is modified");
}

static long put_handle(struct cptr p)
{
    if (!p.blk && !p.fn)
        return 0;
    if (nhandles == caphandles) {
        caphandles = caphandles ? caphandles * 2 : 256;
        handles = xrealloc(handles, (size_t)caphandles * sizeof *handles);
    }
    handles[nhandles++] = p;
    return nhandles;
}

static struct cptr get_handle(long h)
{
    struct cptr p;
    memset(&p, 0, sizeof p);
    if (h == 0)
        return p;
    if (h < 0 || h > nhandles)
        no();
    return handles[h - 1];
}

static int scalar_kind(const struct cty *t)
{
    if (ct_is_integer(t))
        return V_INT;
    if (t->k == CT_FLOAT || t->k == CT_DOUBLE || t->k == CT_LDOUBLE)
        return V_FLT;
    if (t->k == CT_PTR || t->k == CT_NULLPTR || ct_is_ref(t))
        return V_PTR;
    no();
    return 0;
}

/* a bit-field's bytes: from its first, as many as its bits reach */
static struct w128 bits_read(struct cptr p, int nb)
{
    unsigned char *q = p.blk->b + p.off;
    struct w128 w = w_make(0, 0);
    for (int i = nb - 1; i >= 0; i--)
        w = w_shl(w, 8), w.lo |= q[i];
    return w;
}

static struct cval load_bits(struct cptr p, const struct cty *t)
{
    int nb = (p.bo + p.bw + 7) / 8;
    if (nb > 16)
        no();
    check_range(p, nb, 0);
    struct w128 w = w_shr(bits_read(p, nb), p.bo, 0);
    w = w_shr(w_shl(w, 128 - p.bw), 128 - p.bw,
              ct_is_signed(t) && t->k != CT_BOOL);
    return fit(v_w(w), t);
}

static struct cval load(struct cptr p, const struct cty *t)
{
    if (p.bw)
        return load_bits(p, t);
    int k = scalar_kind(t);
    long n = ct_is_ref(t) ? 8 : ct_size(t);
    check_range(p, n, 0);
    unsigned char *q = p.blk->b + p.off;
    if (k == V_FLT) {
        if (n == 4) {
            float f;
            memcpy(&f, q, 4);
            return v_flt(f);
        }
        if (n > 8)
            /* A long double in memory. Reading it back exactly needs a
             * decoder from the target's format, which ldfloat.c does not
             * have yet (it only encodes). Refusing is the honest answer:
             * the alternative is to read the low eight bytes as a double,
             * which is not a narrower answer but a wrong one. See
             * docs/developer/todo.md. */
            notconst("reading a long double object back");
        double d;
        memcpy(&d, q, 8);
        return v_flt(d);
    }
    unsigned long u = 0, uh = 0;
    for (long i = (n > 8 ? 8 : n) - 1; i >= 0; i--)
        u = u << 8 | q[i];
    for (long i = n - 1; i >= 8; i--)
        uh = uh << 8 | q[i];
    if (k == V_PTR)
        return v_ptr(get_handle((long)u));
    if (n == 16)
        return v_w(w_make(u, uh));
    return fit(v_int((long)u), t);
}

/* v as a value of scalar type t (a conversion the tree left implicit) */
static struct cval as_type(struct cval v, const struct cty *t)
{
    int k = scalar_kind(t);
    if (k == V_INT) {
        if (v.k == V_INT)
            return fit(v, t);
        if (v.k == V_FLT) {
            if (t->k == CT_BOOL)
                return v_int(v.f != 0);
            if (v.f != v.f)
                notconst("a NaN is converted to an integer");
            if (is128(t) && (v.f >= 9.3e18 || v.f <= -9.3e18)) {
                /* truncated toward zero, in two halves (2^64 each) */
                double m = v.f < 0 ? -v.f : v.f;
                if (m >= (t->k == CT_INT128 ? 1.7014118346046923e38
                                            : 3.4028236692093846e38) ||
                    (v.f < 0 && t->k == CT_UINT128))
                    notconst("a floating value out of the integer's range");
                unsigned long h = (unsigned long)(m / 18446744073709551616.0);
                unsigned long l = (unsigned long)(m - (double)h *
                                                  18446744073709551616.0);
                struct w128 w = w_make(l, h);
                return v_w(v.f < 0 ? w_neg(w) : w);
            }
            if (v.f >= 9.3e18 || v.f <= -9.3e18)
                notconst("a floating value out of the integer's range");
            return fit(v_int((long)v.f), t);
        }
        if (v.k == V_PTR && t->k == CT_BOOL)
            return v_int(v.p.blk || v.p.fn);
        no();
    }
    if (k == V_FLT) {
        /* To long double: keep (or gain) the exact representation, rounded
         * once to the target's format, as one IEEE conversion. */
        if (t->k == CT_LDOUBLE) {
            if (v.k == V_FLT)
                return v_ldf(ldf_round(as_ldf(v), ldf_target_fmt()));
            if (v.k == V_INT && fits_long(v))
                return v_ldf(ldf_round(ldf_from_int(v.i, 0),
                                       ldf_target_fmt()));
            if (v.k == V_INT && v.hi == 0)
                return v_ldf(ldf_round(ldf_from_int(v.i, 1),
                                       ldf_target_fmt()));
            no();
        }
        double d;
        if (v.k == V_FLT)
            /* From long double: round through the exact value, not
             * through v.f -- v.f is already a double, and rounding it
             * again to float would round twice. */
            d = v.ld ? ldf_to_double(v.ld) : v.f;
        else if (v.k == V_INT && fits_long(v))
            d = (double)v.i;
        else if (v.k == V_INT && v.hi == 0)
            d = (double)(unsigned long)v.i;
        else if (v.k == V_INT)             /* (a signed __int128's) */
            d = (double)v.hi * 18446744073709551616.0 +
                (double)(unsigned long)v.i;
        else
            no();
        if (t->k == CT_FLOAT)
            d = (float)d;
        return v_flt(d);
    }
    if (v.k == V_PTR)
        return v;
    if (v.k == V_INT && v.i == 0)
        return v_ptr(get_handle(0));
    no();
    return v;
}

static void store(struct cptr p, const struct cty *t, struct cval v)
{
    v = as_type(v, t);
    if (p.bw) {
        int nb = (p.bo + p.bw + 7) / 8;
        if (nb > 16)
            no();
        check_range(p, nb, 1);
        struct w128 ones = w_make(~0UL, ~0UL);
        struct w128 fm = w_shr(ones, 128 - p.bw, 0);
        struct w128 m = w_shl(fm, p.bo), x = w_of(v);
        x = w_shl(w_make(x.lo & fm.lo, x.hi & fm.hi), p.bo);
        struct w128 w = bits_read(p, nb);
        w = w_make((w.lo & ~m.lo) | x.lo, (w.hi & ~m.hi) | x.hi);
        unsigned char *q = p.blk->b + p.off;
        for (int i = 0; i < nb; i++, w = w_shr(w, 8, 0))
            q[i] = (unsigned char)w.lo;
        return;
    }
    long n = ct_is_ref(t) ? 8 : ct_size(t);
    check_range(p, n, 1);
    unsigned char *q = p.blk->b + p.off;
    if (v.k == V_FLT) {
        if (n == 4) {
            float f = (float)v.f;
            memcpy(q, &f, 4);
        } else {
            memcpy(q, &v.f, 8);
        }
        return;
    }
    unsigned long u = v.k == V_PTR ? (unsigned long)put_handle(v.p)
                                   : (unsigned long)v.i;
    for (long i = 0; i < n && i < 8; i++, u >>= 8)
        q[i] = (unsigned char)u;
    u = (unsigned long)v.hi;
    for (long i = 8; i < n; i++, u >>= 8)
        q[i] = (unsigned char)u;
}

static void copy_bytes(struct cptr to, struct cptr from, long n)
{
    check_range(from, n, 0);
    check_range(to, n, 1);
    memmove(to.blk->b + to.off, from.blk->b + from.off, (size_t)n);
}

static void zero_bytes(struct cptr to, long n)
{
    check_range(to, n, 1);
    memset(to.blk->b + to.off, 0, (size_t)n);
}

/* ---- variables ---- */

static void bind(struct cvar *v, struct cptr at)
{
    if (fr->n == fr->cap) {
        fr->cap = fr->cap ? fr->cap * 2 : 8;
        fr->b = xrealloc(fr->b, (size_t)fr->cap * sizeof *fr->b);
    }
    fr->b[fr->n].v = v;
    fr->b[fr->n].at = at;
    fr->n++;
}

static struct cval ev(struct cexpr *e);
static struct cptr lv(struct cexpr *e);
static void init_at(struct cptr at, struct cty *t, struct cexpr *init);

/* A variable not local to the evaluation: a constant's object, its value
 * evaluated once (from its own initializer, as a fresh evaluation) —
 * or, for any other, an object only its address is known of. */
static struct cptr outside_var(struct cvar *v)
{
    if (v->ce_state == 1)
        no();                                /* its own initializer */
    if (v->ce_state == 2 || v->ce_state == 3)
        return *(struct cptr *)v->ce_obj;
    if (v->is_member_static && !v->defined)
        member_var_from_outdef(v);
    int is_const = v->is_constexpr ||
                   ((ct_strip_ref(v->type)->q & CQ_CONST) &&
                    !(ct_strip_ref(v->type)->q & CQ_VOLATILE));
    struct cptr *res = xcalloc(1, sizeof *res);
    if (!is_const || (!v->init && !v->ctor) || ct_is_ref(v->type)) {
        if (ct_is_ref(v->type) && is_const && v->init) {
            /* a reference: whatever its initializer names */
        } else if (ct_is_ref(v->type)) {
            no();
        } else {
            struct cblk *k = new_blk(ct_size(v->type));
            k->opaque = 1;
            res->blk = k;
            v->ce_obj = res;
            v->ce_state = 3;
            return *res;
        }
    }
    /* evaluated apart: its own frame, its own failure */
    struct cframe f, *savefr = fr;
    memset(&f, 0, sizeof f);
    jmp_buf jb, *savejb = fail_to;
    struct cstmt *saveseek = seek;
    v->ce_state = 1;
    if (setjmp(jb)) {
        fr = savefr;
        fail_to = savejb;
        seek = saveseek;
        v->ce_state = 0;
        no();
    }
    fail_to = &jb;
    fr = &f;
    seek = NULL;
    if (ct_is_ref(v->type)) {
        struct cval p = ev(v->init);
        if (p.k != V_PTR)
            no();
        *res = p.p;
    } else {
        struct cblk *k = new_blk(ct_size(v->type));
        res->blk = k;
        init_at(*res, v->type, v->ctor ? v->ctor : v->init);
        k->readonly = 1;
    }
    fr = savefr;
    fail_to = savejb;
    seek = saveseek;
    v->ce_obj = res;
    v->ce_state = 2;
    return *res;
}

static struct cptr var_at(struct cvar *v)
{
    for (int i = fr->n - 1; i >= 0; i--)
        if (fr->b[i].v == v)
            return fr->b[i].at;
    if (v->is_local && !v->is_static && !v->has_const && !v->is_constexpr &&
        !(ct_strip_ref(v->type)->q & CQ_CONST))
        no();                   /* a local of a function being run, not here */
    return outside_var(v);
}

/* ---- dynamic types ---- */

struct dynrec {
    struct cblk *blk;
    long off;
    struct cclass *cls;
};
static struct dynrec *dyns;
static long ndyns, capdyns;

/* the records of c's base subobjects at off (as the object's now) gone */
static void drop_bases(struct cblk *blk, long off, struct cclass *c)
{
    for (int i = 0; i < c->nbases; i++) {
        long bo = off + c->bases[i].off;
        for (long k = 0; k < ndyns; k++)
            if (dyns[k].blk == blk && dyns[k].off == bo) {
                dyns[k] = dyns[--ndyns];
                break;
            }
        drop_bases(blk, bo, c->bases[i].cls);
    }
}

/* the object at at is (for now) a c: its bases and members built */
static void set_dyn(struct cptr at, struct cclass *c)
{
    drop_bases(at.blk, at.off, c);
    for (long k = 0; k < ndyns; k++)
        if (dyns[k].blk == at.blk && dyns[k].off == at.off) {
            dyns[k].cls = c;
            return;
        }
    if (ndyns == capdyns) {
        capdyns = capdyns ? capdyns * 2 : 64;
        dyns = xrealloc(dyns, (size_t)capdyns * sizeof *dyns);
    }
    dyns[ndyns].blk = at.blk;
    dyns[ndyns].off = at.off;
    dyns[ndyns].cls = c;
    ndyns++;
}

/* the innermost object with a dynamic type that p is (a subobject of) */
static struct dynrec *dyn_of(struct cptr p)
{
    struct dynrec *best = NULL;
    for (long k = 0; k < ndyns; k++) {
        struct dynrec *r = &dyns[k];
        if (r->blk == p.blk && r->off <= p.off &&
            p.off < r->off + r->cls->size && (!best || r->off > best->off))
            best = r;
    }
    return best;
}

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

/* c's own declaration of fn or of what overrides it */
static struct cfunc *declared_in(struct cclass *c, struct cfunc *fn)
{
    if (fn->cls == c)
        return fn;
    for (struct cfunc *g = cx_funcs; g; g = g->all_next)
        if (g->cls == c && g->is_virtual && !g->is_ctor && !g->is_static &&
            same_sig(g, fn))
            return g;
    return NULL;
}

/* the path from c (at 0) to its base subobject of class s at offset rel:
 * the classes and their offsets, c's first */
static int base_path(struct cclass *c, long off, struct cclass *s, long rel,
                     struct cclass **pc, long *po, int depth)
{
    if (depth >= 64)
        return 0;
    pc[depth] = c;
    po[depth] = off;
    if (c == s && off == rel)
        return depth + 1;
    for (int i = 0; i < c->nbases; i++) {
        if (c->bases[i].is_virtual)
            continue;
        int n = base_path(c->bases[i].cls, off + c->bases[i].off, s, rel,
                          pc, po, depth + 1);
        if (n)
            return n;
    }
    return 0;
}

/* a virtual call of fn on the object at *self: the final overrider, and
 * *self moved to its subobject */
static struct cfunc *overrider(struct cfunc *fn, struct cptr *self)
{
    struct dynrec *r = dyn_of(*self);
    if (!r)
        no();
    struct cclass *pc[64];
    long po[64];
    int n = base_path(r->cls, 0, fn->cls, self->off - r->off, pc, po, 0);
    if (!n)
        no();
    for (int i = 0; i < n; i++) {
        struct cfunc *g = declared_in(pc[i], fn);
        if (!g)
            continue;
        if (g->is_pure)
            notconst("pure virtual '%s' is called", fn->name);
        self->off = r->off + po[i];
        return g;
    }
    no();
    return fn;
}

/* ---- calls ---- */

static int exec(struct cstmt *s);
static int exec_list(struct cstmt *s);

static void need_body(struct cfunc *f)
{
    if (f->is_deleted || f->is_builtin)
        no();
    if (!f->defined) {
        if (f->is_defaulted && !f->special && is_defaultable_cmp(f))
            define_defaulted_cmp(f);
        else if (f->is_implicit || f->is_defaulted)
            define_implicit(f);
        else
            func_ensure_body(f);
    }
    if (!f->defined || !f->body)
        no();
    if (!f->is_constexpr && !f->lambda && !f->is_implicit &&
        !f->is_defaulted)
        notconst("'%s' is called, which is not constexpr", f->name);
    if (f->cls && f->cls->nvbases)
        no();
}

/* f's parameters bound in the new frame nf, from args (evaluated in the
 * caller's frame) */
static void bind_params(struct cfunc *f, struct cframe *nf,
                        struct cexpr **args, int na)
{
    struct cty *ft = f->type;
    if (na != ft->np || ft->variadic)
        no();
    struct cptr *at = xcalloc((size_t)(na ? na : 1), sizeof *at);
    for (int i = 0; i < na; i++) {
        struct cty *pt = ft->params[i];
        if (ct_is_ref(pt)) {
            struct cval p = ev(args[i]);
            if (p.k != V_PTR || !p.p.blk)
                no();
            at[i] = p.p;
        } else {
            struct cblk *k = new_blk(ct_size(pt));
            at[i].blk = k;
            at[i].off = 0;
            init_at(at[i], pt, args[i]);
        }
    }
    struct cframe *save = fr;
    fr = nf;
    for (int i = 0; i < na; i++)
        bind(f->params[i], at[i]);
    fr = save;
}

static void run_body(struct cfunc *f, struct cframe *nf)
{
    if (++depth > MAX_DEPTH)
        no();
    struct cframe *save = fr;
    fr = nf;
    struct cstmt *saveseek = seek;
    seek = NULL;
    exec(f->body);
    seek = saveseek;
    fr = save;
    depth--;
}

/* a call of f (a member's object at self), its class result built at
 * slot */
/* a string literal an argument passes (a throw helper's message), or NULL */
static const char *str_arg(struct cexpr *e)
{
    while (e && (e->k == E_CAST || e->k == E_ADDR) && e->na >= 1)
        e = e->a[0];
    return e && e->k == E_STR && e->swidth == 1 ? e->text : NULL;
}

static struct cval call(struct cfunc *f, struct cexpr **args, int na,
                        struct cptr *self, struct cptr *slot)
{
    step();
    if (!f->is_constexpr && !f->lambda && !f->is_implicit &&
        !f->is_defaulted && na > 0 && str_arg(args[0]))
        notconst("'%s' is called, which is not constexpr: \"%s\"", f->name,
                 str_arg(args[0]));
    need_body(f);
    struct cframe nf;
    memset(&nf, 0, sizeof nf);
    if (self) {
        if (!self->blk)
            no();
        nf.self = *self;
        nf.has_self = 1;
    }
    struct cty *rt = f->type->to;
    struct cblk *own = NULL;
    if (rt->k == CT_CLASS) {
        if (slot) {
            nf.slot = *slot;
        } else {
            own = new_blk(ct_size(rt));
            nf.slot.blk = own;
        }
        nf.has_slot = 1;
    }
    bind_params(f, &nf, args, na);
    nf.ret = v_void();
    run_body(f, &nf);
    if (rt->k == CT_CLASS)
        return v_obj(nf.slot);
    if (rt->k != CT_VOID && nf.ret.k == V_VOID)
        notconst("'%s' ends without returning a value", f->name);
    return nf.ret;
}

/* constructor f of the object at `at` (a class's) */
static void construct_with(struct cfunc *f, struct cptr at, struct cexpr **args,
                           int na)
{
    step();
    need_body(f);
    struct cclass *c = f->cls;
    if (c->nvbases)
        no();
    struct cframe nf;
    memset(&nf, 0, sizeof nf);
    nf.self = at;
    nf.has_self = 1;
    bind_params(f, &nf, args, na);
    struct cframe *save = fr;
    fr = &nf;
    if (++depth > MAX_DEPTH)
        no();
    if (f->delegate) {
        init_at(at, ct_class(c), f->delegate);
    } else {
        if (f->baseinit)
            for (int i = 0; i < c->nbases; i++)
                if (f->baseinit[i])
                    init_at(at_off(at, c->bases[i].off),
                            ct_class(c->bases[i].cls), f->baseinit[i]);
        if (f->meminit)
            for (int i = 0; i < c->nfields; i++) {
                struct cfield *fl = c->fields[i];
                if (!f->meminit[i] || !fl->name)
                    continue;
                init_at(field_at(at, fl), fl->type, f->meminit[i]);
            }
    }
    if (c->dynamic)
        set_dyn(at, c);
    struct cstmt *saveseek = seek;
    seek = NULL;
    exec(f->body);
    seek = saveseek;
    depth--;
    fr = save;
}

/* ---- initialization ---- */

static void construct_at(struct cptr at, struct cexpr *e)
{
    struct cty *t = e->t;
    if (t->k == CT_ARRAY) {
        if (!e->fn && e->na == 1) {
            copy_bytes(at, lv(e->a[0]), ct_size(t));
            return;
        }
        long n = 1;
        struct cty *el = t;
        while (el->k == CT_ARRAY) {
            n *= el->n;
            el = el->to;
        }
        long sz = ct_size(el);
        for (long i = 0; i < n; i++)
            init_at(at_off(at, i * sz), el, e->init);
        return;
    }
    struct cclass *c = t->cls;
    long bytes = e->baseobj ? c->nvsize : c->dsize < c->size ? c->dsize
                                                              : c->size;
    if (e->zero)
        zero_bytes(at, bytes);
    if (e->fn) {
        construct_with(e->fn, at, e->a, e->na);
    } else if (e->na == 1) {
        struct cexpr *s = e->a[0];
        struct cptr from;
        if (s->t->k == CT_CLASS) {
            struct cval o = ev(s);
            if (o.k != V_OBJ)
                no();
            from = o.p;
        } else {
            from = lv(s);
        }
        copy_bytes(at, from, bytes);
    }
}

static void init_at(struct cptr at, struct cty *t, struct cexpr *init)
{
    step();
    if (!init)
        return;
    if (at.bw) {                        /* a bit-field: its value */
        struct cval v;
        if ((init->k == E_CONSTRUCT || init->k == E_INITLIST) && !init->fn &&
            init->na <= 1)
            v = init->na ? ev(init->a[0]) : v_int(0);
        else
            v = ev(init);
        store(at, t, v);
        return;
    }
    if (ct_is_ref(t)) {
        struct cval p = ev(init);
        if (p.k != V_PTR)
            no();
        store(at, t, p);
        return;
    }
    switch (init->k) {
    case E_CONSTRUCT:
        construct_at(at, init);
        return;
    case E_INITLIST: {
        struct cty *lt = init->t;
        if (!lt)
            no();
        if (lt->k == CT_CLASS && lt->cls->is_union)
            no();
        long whole = ct_size(lt);
        zero_bytes(at, whole);
        if (init->binit)
            for (int i = 0; i < lt->cls->nbases; i++)
                if (init->binit[i])
                    init_at(at_off(at, lt->cls->bases[i].off),
                            ct_class(lt->cls->bases[i].cls), init->binit[i]);
        for (int i = 0; i < init->na; i++) {
            if (!init->a[i])
                continue;
            if (lt->k == CT_ARRAY) {
                init_at(at_off(at, i * ct_size(lt->to)), lt->to, init->a[i]);
            } else if (lt->k == CT_CLASS) {
                struct cfield *fl = lt->cls->fields[i];
                init_at(field_at(at, fl), fl->type, init->a[i]);
            } else {
                init_at(at, lt, init->a[i]);
            }
        }
        return;
    }
    case E_STR:
        if (t->k == CT_ARRAY) {
            long n = t->n >= 0 ? t->n : init->slen;
            long k = init->slen < n ? init->slen : n;
            check_range(at, n * init->swidth, 1);
            memcpy(at.blk->b + at.off, init->text, (size_t)(k * init->swidth));
            return;
        }
        break;
    case E_COND:
        if (t->k == CT_CLASS) {
            struct cval c = as_type(ev(init->a[0]), ct_basic(CT_BOOL));
            init_at(at, t, init->a[c.i ? 1 : 2]);
            return;
        }
        break;
    case E_COMMA:
        if (t->k == CT_CLASS) {
            ev(init->a[0]);
            init_at(at, t, init->a[1]);
            return;
        }
        break;
    case E_CALL:
        if (t->k == CT_CLASS && init->fn->type->to->k == CT_CLASS) {
            struct cfunc *f = init->fn;
            int member = f->cls && !f->is_static;
            struct cptr self;
            if (member) {
                struct cval o = ev(init->a[0]);
                if (o.k != V_PTR)
                    no();
                self = o.p;
                if (f->is_virtual && !init->nonvirt)
                    f = overrider(f, &self);
            }
            call(f, init->a + member, init->na - member,
                 member ? &self : NULL, &at);
            return;
        }
        break;
    default:
        break;
    }
    if (t->k == CT_CLASS || t->k == CT_ARRAY) {
        struct cval o = ev(init);
        if (o.k != V_OBJ)
            no();
        copy_bytes(at, o.p, ct_size(t));
        return;
    }
    store(at, t, ev(init));
}

/* ---- expressions ---- */

/* the type of the object a glvalue expression names (before any decay
 * rvalue() applied to the node) */
static struct cty *object_type(struct cexpr *e)
{
    switch (e->k) {
    case E_VAR:
        return ct_strip_ref(e->var->type);
    case E_MEMBER:
        return ct_strip_ref(e->field->type);
    case E_DEREF:
        if (e->a[0]->t->k == CT_PTR)
            return e->a[0]->t->to;
        return e->t;
    default:
        return e->t;
    }
}

static long elem_size(struct cty *pt)
{
    if (pt->k != CT_PTR)
        no();
    struct cty *el = pt->to;
    if (el->k == CT_VOID || el->k == CT_FUNC)
        no();
    return ct_size(el);
}

/* a op b, of __int128s (b a shift's count: any integer) */
static struct cval arith128(int op, struct cval a, struct cval b,
                            struct cty *ta, struct cty *rt)
{
    int sign = ct_unqual(ta)->k == CT_INT128;
    struct w128 x = w_of(a), y = w_of(b), r;
    int lt = sign ? w_slt(x, y) : w_ult(x, y);
    int gt = sign ? w_slt(y, x) : w_ult(y, x);
    switch (op) {
    case TOK_PLUS: r = w_add(x, y); break;
    case TOK_MINUS: r = w_add(x, w_neg(y)); break;
    case TOK_STAR: r = w_mul(x, y); break;
    case TOK_SLASH: case TOK_PERCENT:
        if (sign && x.hi == 1UL << 63 && !x.lo && y.hi == ~0UL &&
            y.lo == ~0UL)
            notconst("a quotient overflows");
        if (!w_div(x, y, sign, op == TOK_PERCENT, &r))
            notconst("a division by zero");
        break;
    case TOK_SHL: case TOK_SHR:
        if (!fits_long(b) || b.i < 0 || b.i >= 128)
            notconst("a shift by a count out of range");
        r = op == TOK_SHL ? w_shl(x, (int)b.i) : w_shr(x, (int)b.i, sign);
        break;
    case TOK_AMP: r = w_make(x.lo & y.lo, x.hi & y.hi); break;
    case TOK_PIPE: r = w_make(x.lo | y.lo, x.hi | y.hi); break;
    case TOK_CARET: r = w_make(x.lo ^ y.lo, x.hi ^ y.hi); break;
    case TOK_EQEQ: return v_int(!lt && !gt);
    case TOK_NEQ: return v_int(lt || gt);
    case TOK_LT: return v_int(lt);
    case TOK_GT: return v_int(gt);
    case TOK_LE: return v_int(!gt);
    case TOK_GE: return v_int(!lt);
    case TOK_ANDAND: return v_int((a.i || a.hi) && (b.i || b.hi));
    case TOK_OROR: return v_int(a.i || a.hi || b.i || b.hi);
    default: no(); return a;
    }
    return fit(v_w(r), rt);
}

static struct cval arith(int op, struct cval a, struct cval b,
                         struct cty *ta, struct cty *tb, struct cty *rt)
{
    if (a.k == V_PTR || b.k == V_PTR) {
        if (op == TOK_EQEQ || op == TOK_NEQ) {
            if (a.k != V_PTR)
                a = as_type(a, tb);
            if (b.k != V_PTR)
                b = as_type(b, ta);
            int same = a.p.blk == b.p.blk && a.p.off == b.p.off &&
                       a.p.fn == b.p.fn;
            return v_int(op == TOK_EQEQ ? same : !same);
        }
        if (a.k == V_PTR && b.k == V_PTR) {
            if (!a.p.blk || a.p.blk != b.p.blk)
                no();
            switch (op) {
            case TOK_MINUS:
                return v_int((a.p.off - b.p.off) / elem_size(ta));
            case TOK_LT: return v_int(a.p.off < b.p.off);
            case TOK_GT: return v_int(a.p.off > b.p.off);
            case TOK_LE: return v_int(a.p.off <= b.p.off);
            case TOK_GE: return v_int(a.p.off >= b.p.off);
            default: no();
            }
        }
        if (op == TOK_PLUS || op == TOK_MINUS) {
            struct cval p = a.k == V_PTR ? a : b;
            struct cval n = a.k == V_PTR ? b : a;
            struct cty *pt = a.k == V_PTR ? ta : tb;
            if (n.k != V_INT || !p.p.blk)
                no();
            long d = n.i * elem_size(pt);
            p.p.off += op == TOK_PLUS ? d : -d;
            if (p.p.off < 0 || p.p.off > p.p.blk->size)
                no();
            return p;
        }
        if (op == TOK_ANDAND || op == TOK_OROR) {
            long x = a.k == V_PTR ? (a.p.blk || a.p.fn) : a.i || a.hi;
            long y = b.k == V_PTR ? (b.p.blk || b.p.fn) : b.i || b.hi;
            return v_int(op == TOK_ANDAND ? x && y : x || y);
        }
        no();
    }
    if (a.k == V_FLT || b.k == V_FLT) {
        /* If either side is a long double, the whole operation happens at
         * long double precision -- which is what the usual arithmetic
         * conversions say, and what the target would do at run time. */
        if (a.ld || b.ld) {
            struct ldf *x = a.k == V_FLT ? as_ldf(a)
                          : as_ldf(as_type(a, ct_basic(CT_LDOUBLE)));
            struct ldf *y = b.k == V_FLT ? as_ldf(b)
                          : as_ldf(as_type(b, ct_basic(CT_LDOUBLE)));
            int c;
            switch (op) {
            case TOK_PLUS: case TOK_MINUS: case TOK_STAR:
                return as_type(v_ldf(ldf_binop(op == TOK_PLUS ? '+'
                                               : op == TOK_MINUS ? '-' : '*',
                                               x, y, ldf_target_fmt())), rt);
            case TOK_SLASH:
                if (ldf_is_zero(y))
                    notconst("a division by zero");
                return as_type(v_ldf(ldf_binop('/', x, y,
                                               ldf_target_fmt())), rt);
            case TOK_ANDAND: return v_int(!ldf_is_zero(x) && !ldf_is_zero(y));
            case TOK_OROR:   return v_int(!ldf_is_zero(x) || !ldf_is_zero(y));
            default: break;
            }
            c = ldf_cmp(x, y);
            /* An unordered comparison (a NaN operand) is false for every
             * relation except !=, which is the one rule people forget. */
            if (c == LDF_UNORDERED)
                return v_int(op == TOK_NEQ);
            switch (op) {
            case TOK_EQEQ: return v_int(c == 0);
            case TOK_NEQ:  return v_int(c != 0);
            case TOK_LT:   return v_int(c < 0);
            case TOK_GT:   return v_int(c > 0);
            case TOK_LE:   return v_int(c <= 0);
            case TOK_GE:   return v_int(c >= 0);
            default: no();
            }
        }
        double x = a.k == V_FLT ? a.f : as_type(a, ct_basic(CT_DOUBLE)).f;
        double y = b.k == V_FLT ? b.f : as_type(b, ct_basic(CT_DOUBLE)).f;
        double r;
        switch (op) {
        case TOK_PLUS: r = x + y; break;
        case TOK_MINUS: r = x - y; break;
        case TOK_STAR: r = x * y; break;
        case TOK_SLASH:
            if (y == 0)
                notconst("a division by zero");
            r = x / y;
            break;
        case TOK_EQEQ: return v_int(x == y);
        case TOK_NEQ: return v_int(x != y);
        case TOK_LT: return v_int(x < y);
        case TOK_GT: return v_int(x > y);
        case TOK_LE: return v_int(x <= y);
        case TOK_GE: return v_int(x >= y);
        case TOK_ANDAND: return v_int(x != 0 && y != 0);
        case TOK_OROR: return v_int(x != 0 || y != 0);
        default: no(); return a;
        }
        return as_type(v_flt(r), rt);
    }
    if (a.k != V_INT || b.k != V_INT)
        no();
    if (is128(ct_unqual(ta)))
        return arith128(op, a, b, ta, rt);
    long x = a.i, y = b.i;
    int uns = !ct_is_signed(ta);
    unsigned long ux = (unsigned long)x, uy = (unsigned long)y;
    long r;
    switch (op) {
    case TOK_PLUS: r = (long)(ux + uy); break;
    case TOK_MINUS: r = (long)(ux - uy); break;
    case TOK_STAR: r = (long)(ux * uy); break;
    case TOK_SLASH:
        if (!y || (!uns && y == -1 && x == (long)(1UL << 63)))
            notconst(y ? "a quotient overflows" : "a division by zero");
        r = uns ? (long)(ux / uy) : x / y;
        break;
    case TOK_PERCENT:
        if (!y || (!uns && y == -1))
            return y ? v_int(0) : (notconst("a division by zero"), a);
        r = uns ? (long)(ux % uy) : x % y;
        break;
    case TOK_SHL: case TOK_SHR: {
        long w = ct_size(ta) * 8;
        if (y < 0 || y >= (w < 32 ? 32 : w))
            notconst("a shift by a count out of range");
        if (op == TOK_SHL)
            r = (long)(ux << y);
        else
            r = uns ? (long)(ux >> y) : x >> y;
        break;
    }
    case TOK_AMP: r = x & y; break;
    case TOK_PIPE: r = x | y; break;
    case TOK_CARET: r = x ^ y; break;
    case TOK_EQEQ: return v_int(x == y);
    case TOK_NEQ: return v_int(x != y);
    case TOK_LT: return v_int(uns ? ux < uy : x < y);
    case TOK_GT: return v_int(uns ? ux > uy : x > y);
    case TOK_LE: return v_int(uns ? ux <= uy : x <= y);
    case TOK_GE: return v_int(uns ? ux >= uy : x >= y);
    case TOK_ANDAND: return v_int(x && y);
    case TOK_OROR: return v_int(x || y);
    default: no(); return a;
    }
    return fit(v_int(r), rt);
}

static int truth(struct cval v)
{
    switch (v.k) {
    case V_INT: return v.i != 0 || v.hi != 0;
    case V_FLT: return v.f != 0;
    case V_PTR: return v.p.blk || v.p.fn;
    default: no(); return 0;
    }
}

static int bits_of(struct cexpr *e)
{
    return (int)ct_size(e->t) * 8;
}

static struct cval builtin(struct cexpr *e)
{
    const char *n = e->name;
    if (strncmp(n, "__builtin_", 10) == 0)
        n += 10;
    if (strcmp(n, "is_constant_evaluated") == 0)
        return v_int(1);
    if (strcmp(n, "unreachable") == 0 || strcmp(n, "trap") == 0)
        notconst("__builtin_%s is reached", n);
    if (strcmp(n, "expect") == 0 && e->na == 2)
        return ev(e->a[0]);
    if (strcmp(n, "constant_p") == 0)
        return v_int(1);
    if (strcmp(n, "huge_val") == 0 || strcmp(n, "inf") == 0)
        return v_flt(1e308 * 10);
    if (strcmp(n, "huge_valf") == 0 || strcmp(n, "inff") == 0)
        return v_flt(1e308 * 10);
    if (e->na != 1)
        no();
    struct cval a = ev(e->a[0]);
    if (strcmp(n, "strlen") == 0) {
        if (a.k != V_PTR || !a.p.blk)
            no();
        long k = 0;
        for (;; k++) {
            check_range(at_off(a.p, k), 1, 0);
            if (!a.p.blk->b[a.p.off + k])
                break;
        }
        return v_int(k);
    }
    if (a.k != V_INT)
        no();
    unsigned long u = (unsigned long)a.i;
    int w = bits_of(e->a[0]);
    if (w < 64)
        u &= (1UL << w) - 1;
    if (strncmp(n, "clz", 3) == 0) {
        if (!u)
            no();
        int k = 0;
        for (int b = w - 1; b >= 0 && !(u >> b & 1); b--)
            k++;
        return v_int(k);
    }
    if (strncmp(n, "ctz", 3) == 0) {
        if (!u)
            no();
        int k = 0;
        while (!(u >> k & 1))
            k++;
        return v_int(k);
    }
    if (strncmp(n, "popcount", 8) == 0) {
        int k = 0;
        for (; u; u &= u - 1)
            k++;
        return v_int(k);
    }
    if (strncmp(n, "parity", 6) == 0) {
        int k = 0;
        for (; u; u &= u - 1)
            k++;
        return v_int(k & 1);
    }
    if (strcmp(n, "bswap16") == 0)
        return v_int((long)(((u & 0xff) << 8) | (u >> 8 & 0xff)));
    if (strcmp(n, "bswap32") == 0 || strcmp(n, "bswap64") == 0) {
        int nb = n[5] == '3' ? 4 : 8;
        unsigned long r = 0;
        for (int i = 0; i < nb; i++)
            r = r << 8 | (u >> (8 * i) & 0xff);
        return fit(v_int((long)r), e->t);
    }
    if (strcmp(n, "abs") == 0 || strcmp(n, "labs") == 0 ||
        strcmp(n, "llabs") == 0)
        return v_int(a.i < 0 ? -a.i : a.i);
    no();
    return a;
}

static struct cty *call_ret(struct cexpr *e)
{
    if (e->k == E_CALL)
        return e->fn->type->to;
    struct cty *pt = e->a[0]->t;
    if (pt->k == CT_PTR)
        pt = pt->to;
    if (pt->k != CT_FUNC)
        no();
    return pt->to;
}

/* a call: its value (a reference's: the pointer; a class's: the object) */
static struct cval eval_call(struct cexpr *e)
{
    if (e->k == E_CALL) {
        struct cfunc *f = e->fn;
        if (f->cls && !f->is_static && !f->is_ctor) {
            struct cval o = ev(e->a[0]);
            if (o.k != V_PTR)
                no();
            if (f->is_virtual && !e->nonvirt)
                f = overrider(f, &o.p);
            return call(f, e->a + 1, e->na - 1, &o.p, NULL);
        }
        return call(f, e->a, e->na, NULL, NULL);
    }
    if (e->k == E_ICALL) {
        struct cval p = ev(e->a[0]);
        if (p.k != V_PTR || !p.p.fn)
            no();
        struct cfunc *f = p.p.fn;
        if (f->cls && !f->is_static)
            no();
        return call(f, e->a + 1, e->na - 1, NULL, NULL);
    }
    no();
    return v_void();
}

/* the usual arithmetic conversions' common type of a and b (7.4) */
static struct cty *common_type(struct cty *a, struct cty *b)
{
    if (ct_is_float(a) || ct_is_float(b)) {
        if (a->k == CT_LDOUBLE || b->k == CT_LDOUBLE)
            no();
        if (a->k == CT_DOUBLE || b->k == CT_DOUBLE || !ct_is_float(a) ||
            !ct_is_float(b))
            return ct_basic(a->k == CT_FLOAT && b->k == CT_FLOAT
                            ? CT_FLOAT
                            : (a->k == CT_FLOAT && !ct_is_float(b)) ||
                              (b->k == CT_FLOAT && !ct_is_float(a))
                            ? CT_FLOAT : CT_DOUBLE);
        return ct_basic(CT_FLOAT);
    }
    long sa = ct_size(a) < 4 ? 4 : ct_size(a);
    long sb = ct_size(b) < 4 ? 4 : ct_size(b);
    long sz = sa > sb ? sa : sb;
    int uns = (ct_size(a) >= 4 && sa == sz && !ct_is_signed(a)) ||
              (ct_size(b) >= 4 && sb == sz && !ct_is_signed(b));
    if (sz == 16)
        return ct_basic(uns ? CT_UINT128 : CT_INT128);
    if (sz == 8)
        return ct_basic(uns ? CT_ULONG : CT_LONG);
    return ct_basic(uns ? CT_UINT : CT_INT);
}

static struct cval assign(struct cexpr *e, struct cptr *where)
{
    struct cexpr *l = e->a[0];
    struct cptr at = lv(l);
    struct cty *t = l->t;
    if (t->k == CT_CLASS || t->k == CT_ARRAY) {
        if (e->op != TOK_ASSIGN)
            no();
        struct cval o = ev(e->a[1]);
        if (o.k != V_OBJ)
            no();
        copy_bytes(at, o.p, ct_size(t));
        *where = at;
        return v_obj(at);
    }
    struct cval r = ev(e->a[1]);
    if (e->op != TOK_ASSIGN) {
        struct cval old = load(at, t);
        struct cty *rt = e->a[1]->t;
        if (old.k == V_PTR) {
            r = arith(e->op, old, r, t, rt, t);
        } else {
            /* in the operands' common type (a shift: the left's,
             * promoted), then back to the left's */
            struct cty *ct = e->op == TOK_SHL || e->op == TOK_SHR
                             ? common_type(t, ct_basic(CT_INT))
                             : common_type(t, rt);
            r = arith(e->op, as_type(old, ct),
                      e->op == TOK_SHL || e->op == TOK_SHR ? r
                                                           : as_type(r, ct),
                      ct, rt, ct);
        }
    }
    store(at, t, r);
    *where = at;
    return load(at, t);
}

static struct cval incdec(struct cexpr *e, struct cptr *where)
{
    struct cexpr *l = e->a[0];
    struct cptr at = lv(l);
    struct cty *t = l->t;
    struct cval old = load(at, t);
    struct cval nw;
    if (old.k == V_PTR) {
        nw = arith(TOK_PLUS, old, v_int(e->ival), t, ct_basic(CT_LONG), t);
    } else if (old.k == V_FLT) {
        nw = as_type(v_flt(old.f + (double)e->ival), t);
    } else if (t->k == CT_BOOL) {
        nw = v_int(e->ival > 0 ? 1 : !old.i);
    } else {
        nw = fit(v_w(w_add(w_of(old), w_from(e->ival, 1))), t);
    }
    store(at, t, nw);
    *where = at;
    return e->post ? old : nw;
}

static struct cblk *str_blk(struct cexpr *e)
{
    struct cblk *k = new_blk(e->slen * e->swidth);
    memcpy(k->b, e->text, (size_t)(e->slen * e->swidth));
    k->readonly = 1;
    return k;
}

/* the glvalue e: where its object is */
static struct cptr lv(struct cexpr *e)
{
    step();
    struct cptr p;
    memset(&p, 0, sizeof p);
    switch (e->k) {
    case E_VAR:
        return var_at(e->var);
    case E_DEREF: {
        struct cval v = ev(e->a[0]);
        if (v.k != V_PTR || !v.p.blk)
            no();
        return v.p;
    }
    case E_MEMBER: {
        struct cexpr *o = e->a[0];
        struct cptr base;
        if (o->vc == VC_PRVALUE && o->k != E_VAR && o->k != E_MEMBER &&
            o->k != E_DEREF) {
            struct cval ov = ev(o);
            if (ov.k != V_OBJ)
                no();
            base = ov.p;
        } else {
            base = lv(o);
        }
        struct cfield *fl = e->field;
        p = field_at(base, fl);
        if (ct_is_ref(fl->type)) {
            struct cval r = load(p, fl->type);
            if (!r.p.blk)
                no();
            return r.p;
        }
        return p;
    }
    case E_TEMP: {
        struct cblk *k = new_blk(ct_size(e->t));
        p.blk = k;
        init_at(p, ct_unqual(e->t), e->a[0]);
        return p;
    }
    case E_STR:
        p.blk = str_blk(e);
        return p;
    case E_BASE:
        if (e->is_array || e->vbindex)
            no();
        return at_off(lv(e->a[0]), e->ival);
    case E_CAST:
        if (e->lvcast)
            return lv(e->a[0]);
        break;
    case E_CALL: case E_ICALL: {
        struct cty *rt = call_ret(e);
        struct cval r = eval_call(e);
        if (ct_is_ref(rt)) {
            if (r.k != V_PTR || !r.p.blk)
                no();
            return r.p;
        }
        if (r.k == V_OBJ)
            return r.p;
        break;
    }
    case E_ASSIGN: {
        assign(e, &p);
        return p;
    }
    case E_INCDEC:
        if (e->post)
            break;
        incdec(e, &p);
        return p;
    case E_COMMA:
        ev(e->a[0]);
        return lv(e->a[1]);
    case E_COND:
        return lv(e->a[truth(ev(e->a[0])) ? 1 : 2]);
    case E_THIS:
        break;
    default:
        break;
    }
    if (e->t && (e->t->k == CT_CLASS || e->t->k == CT_ARRAY)) {
        struct cval o = ev(e);
        if (o.k == V_OBJ)
            return o.p;
    }
    no();
    return p;
}

/* the value of e (a class or array's: the object) */
static struct cval ev_(struct cexpr *e);

/* (nesting bounded: a runaway evaluation is simply not a constant) */
static struct cval ev(struct cexpr *e)
{
    static int nest;
    if (nest > 4000)
        no();
    nest++;
    jmp_buf jb, *saved = fail_to;
    if (setjmp(jb)) {
        nest--;
        fail_to = saved;
        no();
    }
    fail_to = &jb;
    struct cval v = ev_(e);
    fail_to = saved;
    nest--;
    return v;
}

static struct cval ev_(struct cexpr *e)
{
    step();
    switch (e->k) {
    case E_INT:
        return v_int(e->ival);
    case E_FLT:
        /* A long double literal is converted from its EXACT decimal text,
         * never through a double: `0.1L` has more significant bits than a
         * double holds, and going via one would round twice and lose the
         * last of them. e->text is the literal without its suffix, which
         * is what ldf_from_text wants. */
        if (ct_unqual(e->t)->k == CT_LDOUBLE && e->text) {
            struct ldf *x = ldf_from_text(e->text, ldf_target_fmt());
            if (x)
                return v_ldf(x);
        }
        return as_type(v_flt(e->fval), e->t);
    case E_NULLPTR: {
        struct cptr p;
        memset(&p, 0, sizeof p);
        return v_ptr(p);
    }
    case E_STR: {
        struct cptr p;
        memset(&p, 0, sizeof p);
        p.blk = str_blk(e);
        return e->t->k == CT_PTR ? v_ptr(p) : v_obj(p);
    }
    case E_FUNC: {
        struct cptr p;
        memset(&p, 0, sizeof p);
        p.fn = e->fn;
        return v_ptr(p);
    }
    case E_VAR: {
        struct cvar *v = e->var;
        int local = 0;
        for (int i = fr->n - 1; i >= 0 && !local; i--)
            local = fr->b[i].v == v;
        if (v->has_const && ct_is_integer(e->t) && !ct_is_ref(v->type) &&
            !local)
            return fit(v_int(v->const_val), e->t);
        if (!local && v->is_local && !v->is_static && !v->has_const &&
            !v->is_constexpr && !(ct_strip_ref(v->type)->q & CQ_CONST) &&
            !ct_is_ref(v->type) && e->t->k != CT_CLASS &&
            e->t->k != CT_ARRAY)
            notconst("the value of '%s' is not a constant", v->name);
    }
        /* fall through */
    case E_MEMBER: case E_DEREF: case E_TEMP: case E_BASE: {
        if (e->k == E_BASE && e->is_array) {
            struct cval v = ev(e->a[0]);
            if (v.k != V_PTR || e->vbindex)
                no();
            if (v.p.blk)
                v.p.off += e->ival;
            return v;
        }
        if (e->k == E_DEREF && (e->t->k == CT_FUNC ||
                                (e->t->k == CT_PTR &&
                                 e->a[0]->t->k == CT_PTR &&
                                 e->a[0]->t->to->k == CT_FUNC)))
            return ev(e->a[0]);
        struct cptr at = lv(e);
        struct cty *ot = e->k == E_TEMP ? e->t : object_type(e);
        if (ot->k == CT_ARRAY && e->t->k == CT_PTR)
            return v_ptr(at);                 /* decayed */
        if (e->t->k == CT_CLASS || e->t->k == CT_ARRAY)
            return v_obj(at);
        return load(at, e->t);
    }
    case E_THIS:
        if (!fr->has_self)
            no();
        return v_ptr(fr->self);
    case E_ADDR: {
        struct cexpr *o = e->a[0];
        if (o->k == E_FUNC || o->k == E_OVL) {
            struct cptr p;
            memset(&p, 0, sizeof p);
            p.fn = o->fn;
            if (o->fn->cls && !o->fn->is_static)
                no();
            return v_ptr(p);
        }
        if (e->t->k != CT_PTR)
            no();                            /* a pointer to member */
        return v_ptr(lv(o));
    }
    case E_CALL: case E_ICALL: {
        struct cty *rt = call_ret(e);
        struct cval r = eval_call(e);
        if (ct_is_ref(rt)) {
            if (r.k != V_PTR || !r.p.blk)
                no();
            if (e->t->k == CT_CLASS || e->t->k == CT_ARRAY)
                return v_obj(r.p);
            return load(r.p, e->t);
        }
        return r;
    }
    case E_BUILTIN:
        return builtin(e);
    case E_UNARY: {
        struct cval a = ev(e->a[0]);
        switch (e->op) {
        case TOK_BANG:
            return v_int(!truth(a));
        case TOK_PLUS:
            return a;
        case TOK_MINUS:
            if (a.k == V_FLT)
                return as_type(v_flt(-a.f), e->t);
            if (a.k != V_INT)
                no();
            return fit(v_w(w_neg(w_of(a))), e->t);
        case TOK_TILDE:
            if (a.k != V_INT)
                no();
            return fit(v_w(w_make(~(unsigned long)a.i, ~(unsigned long)a.hi)),
                       e->t);
        default:
            no();
        }
        return a;
    }
    case E_BINARY: {
        if (e->op == TOK_ANDAND || e->op == TOK_OROR) {
            int x = truth(ev(e->a[0]));
            if (e->op == TOK_ANDAND && !x)
                return v_int(0);
            if (e->op == TOK_OROR && x)
                return v_int(1);
            return v_int(truth(ev(e->a[1])));
        }
        struct cval a = ev(e->a[0]);
        struct cval b = ev(e->a[1]);
        return arith(e->op, a, b, e->a[0]->t, e->a[1]->t, e->t);
    }
    case E_CMP3: {
        struct cval a = ev(e->a[0]);
        struct cval b = ev(e->a[1]);
        struct cty *ot = e->a[0]->t, *bt = ct_basic(CT_BOOL);
        long v = truth(arith(TOK_LT, a, b, ot, ot, bt)) ? -1
                 : truth(arith(TOK_GT, a, b, ot, ot, bt)) ? 1
                 : !e->ival || truth(arith(TOK_EQEQ, a, b, ot, ot, bt)) ? 0
                 : -128;
        struct cptr p;
        memset(&p, 0, sizeof p);
        p.blk = new_blk(ct_size(e->t));
        p.blk->b[0] = (unsigned char)(signed char)v;
        return v_obj(p);
    }
    case E_ASSIGN: {
        struct cptr at;
        return assign(e, &at);
    }
    case E_INCDEC: {
        struct cptr at;
        return incdec(e, &at);
    }
    case E_COND: {
        int c = truth(ev(e->a[0]));
        return ev(e->a[c ? 1 : 2]);
    }
    case E_COMMA:
        ev(e->a[0]);
        return ev(e->a[1]);
    case E_CAST: {
        if (e->lvcast) {
            struct cptr at = lv(e->a[0]);
            if (e->t->k == CT_CLASS || e->t->k == CT_ARRAY)
                return v_obj(at);
            return load(at, e->t);
        }
        struct cval a = ev(e->a[0]);
        if (e->t->k == CT_VOID)
            return v_void();
        if (e->t->k == CT_CLASS)
            return a;
        if (e->t->k == CT_PTR && a.k == V_PTR)
            return a;
        if (a.k == V_INT && ct_is_float(e->t) &&
            ct_is_integer(e->a[0]->t) && !ct_is_signed(e->a[0]->t))
            return as_type(v_flt((double)(unsigned long)a.hi *
                                 18446744073709551616.0 +
                                 (double)(unsigned long)a.i), e->t);
        return as_type(a, e->t);
    }
    case E_CONSTRUCT: case E_INITLIST: {
        if (!e->t || (e->t->k != CT_CLASS && e->t->k != CT_ARRAY)) {
            if (e->k == E_INITLIST && e->t && e->na == 1 && e->a[0])
                return as_type(ev(e->a[0]), e->t);
            if (e->k == E_INITLIST && e->t && e->na == 0)
                return as_type(v_int(0), e->t);
            no();
        }
        struct cptr p;
        memset(&p, 0, sizeof p);
        p.blk = new_blk(ct_size(e->t));
        init_at(p, e->t, e);
        return v_obj(p);
    }
    case E_THROW:
        notconst("an exception is thrown");
        break;
    case E_STMTEXPR: {
        struct cstmt *last = NULL;
        for (struct cstmt *s = e->body->body; s; s = s->next)
            last = s;
        for (struct cstmt *s = e->body->body; s; s = s->next) {
            if (s == last && s->k == S_EXPR)
                return ev(s->e);
            if (exec(s) != X_NORMAL)
                no();
        }
        return v_void();
    }
    default:
        no();
    }
    return v_void();
}

/* ---- statements ---- */

static int contains(struct cstmt *s, struct cstmt *t)
{
    for (; s; s = s->next) {
        if (s == t)
            return 1;
        if ((s->k == S_BLOCK || s->k == S_CASE || s->k == S_DEFAULT ||
             s->k == S_LABEL) && contains(s->body, t))
            return 1;
    }
    return 0;
}

static int contains_one(struct cstmt *s, struct cstmt *t)
{
    if (s == t)
        return 1;
    if (s->k == S_BLOCK || s->k == S_CASE || s->k == S_DEFAULT ||
        s->k == S_LABEL)
        return contains(s->body, t);
    return 0;
}

static struct cstmt *find_case(struct cstmt *s, long v, struct cstmt **dflt)
{
    for (; s; s = s->next) {
        if (s->k == S_CASE) {
            if (s->is_range ? v >= s->cval && v <= s->cval2 : v == s->cval)
                return s;
        } else if (s->k == S_DEFAULT) {
            *dflt = s;
        }
        if (s->k == S_BLOCK || s->k == S_CASE || s->k == S_DEFAULT ||
            s->k == S_LABEL) {
            struct cstmt *r = find_case(s->body, v, dflt);
            if (r)
                return r;
        }
    }
    return NULL;
}

static void decl_one(struct cstmt *s)
{
    struct cvar *v = s->var;
    if (v->is_static)
        no();
    if (ct_is_ref(v->type)) {
        if (!v->init)
            no();
        struct cval p = ev(v->init);
        if (p.k != V_PTR || !p.p.blk)
            no();
        bind(v, p.p);
        return;
    }
    struct cptr at;
    memset(&at, 0, sizeof at);
    at.blk = new_blk(ct_size(v->type));
    bind(v, at);
    init_at(at, v->type, v->ctor ? v->ctor : v->init);
}

static void exec_inits(struct cstmt *s)
{
    for (; s; s = s->more) {
        if (s->k == S_DECL)
            decl_one(s);
        else if (exec(s) != X_NORMAL)
            no();
    }
}

static int exec_list(struct cstmt *s)
{
    for (; s; s = s->next) {
        if (seek && !contains_one(s, seek))
            continue;
        int r = exec(s);
        if (r != X_NORMAL)
            return r;
    }
    return X_NORMAL;
}

static int exec(struct cstmt *s)
{
    step();
    if (seek) {
        switch (s->k) {
        case S_BLOCK:
            return exec_list(s->body);
        case S_CASE: case S_DEFAULT: case S_LABEL:
            if (s == seek)
                seek = NULL;
            return exec(s->body);
        default:
            no();
        }
    }
    switch (s->k) {
    case S_NULL:
        return X_NORMAL;
    case S_EXPR:
        ev(s->e);
        return X_NORMAL;
    case S_DECL:
        for (struct cstmt *d = s; d; d = d->more)
            decl_one(d);
        return X_NORMAL;
    case S_BLOCK: {
        int mark = fr->n;
        int r = exec_list(s->body);
        fr->n = mark;
        return r;
    }
    case S_IF: {
        int mark = fr->n;
        exec_inits(s->init);
        int r = X_NORMAL;
        if (truth(ev(s->e)))
            r = exec(s->body);
        else if (s->els)
            r = exec(s->els);
        fr->n = mark;
        return r;
    }
    case S_WHILE:
        for (;;) {
            int mark = fr->n;
            exec_inits(s->init);
            if (!truth(ev(s->e))) {
                fr->n = mark;
                return X_NORMAL;
            }
            int r = exec(s->body);
            fr->n = mark;
            if (r == X_BREAK)
                return X_NORMAL;
            if (r == X_RETURN)
                return r;
        }
    case S_DO:
        for (;;) {
            int r = exec(s->body);
            if (r == X_BREAK)
                return X_NORMAL;
            if (r == X_RETURN)
                return r;
            if (!truth(ev(s->e)))
                return X_NORMAL;
        }
    case S_FOR: {
        int mark = fr->n;
        exec_inits(s->init);
        for (;;) {
            if (s->e && !truth(ev(s->e)))
                break;
            int r = exec(s->body);
            if (r == X_BREAK)
                break;
            if (r == X_RETURN) {
                fr->n = mark;
                return r;
            }
            if (s->e2)
                ev(s->e2);
        }
        fr->n = mark;
        return X_NORMAL;
    }
    case S_SWITCH: {
        int mark = fr->n;
        exec_inits(s->init);
        struct cval v = ev(s->e);
        if (v.k != V_INT)
            no();
        struct cstmt *dflt = NULL;
        struct cstmt *to = find_case(s->body, v.i, &dflt);
        if (is128(ct_unqual(s->e->t)) &&
            (!fits_long(v) || (v.i < 0 && ct_unqual(s->e->t)->k == CT_UINT128)))
            to = NULL;                  /* (no label, a long, is it) */
        if (!to)
            to = dflt;
        int r = X_NORMAL;
        if (to) {
            seek = to;
            r = exec(s->body);
            seek = NULL;
        }
        fr->n = mark;
        return r == X_BREAK ? X_NORMAL : r;
    }
    case S_CASE: case S_DEFAULT: case S_LABEL:
        return exec(s->body);
    case S_BREAK:
        return X_BREAK;
    case S_CONTINUE:
        return X_CONTINUE;
    case S_RETURN:
        if (s->e) {
            if (fr->has_slot) {
                init_at(fr->slot, ct_unqual(s->e->t), s->e);
                fr->ret = v_obj(fr->slot);
            } else {
                fr->ret = ev(s->e);
            }
        }
        return X_RETURN;
    case S_TRY:
        return exec(s->body);
    default:
        no();
    }
    return X_NORMAL;
}

/* ---- the entry ---- */

/* e evaluated from outside any call: its value, or 0 (definite says
 * whether that is because it is not a constant expression). A class's or
 * an array's value is built in a block of its own. */
static int eval_top(struct cexpr *e, struct cval *out)
{
    if (cx_pattern || !e || !e->t)
        return 0;
    jmp_buf jb, *savejb = fail_to;
    struct cframe f, *savefr = fr;
    long savesteps = steps;
    int savedepth = depth;
    struct cstmt *saveseek = seek;
    memset(&f, 0, sizeof f);
    definite = 0;
    why[0] = 0;
    if (setjmp(jb)) {
        fail_to = savejb;
        fr = savefr;
        steps = savesteps;
        depth = savedepth;
        seek = saveseek;
        return 0;
    }
    fail_to = &jb;
    fr = &f;
    steps = 0;
    depth = 0;
    seek = NULL;
    struct cval v;
    struct cty *t = ct_unqual(e->t);
    if (t->k == CT_CLASS || t->k == CT_ARRAY) {
        struct cptr p;
        memset(&p, 0, sizeof p);
        p.blk = new_blk(ct_size(t));
        init_at(p, t, e);
        v = v_obj(p);
    } else {
        v = ev(e);
    }
    fail_to = savejb;
    fr = savefr;
    steps = savesteps;
    depth = savedepth;
    seek = saveseek;
    *out = v;
    return 1;
}

/* e evaluated from outside any call: its value, or 0 */
static int run(struct cexpr *e, struct cval *out)
{
    if (!e || !e->t || !ct_is_integer(e->t) || !eval_top(e, out))
        return 0;
    struct cval v = *out;
    if (v.k == V_PTR && e->t->k == CT_BOOL)
        v = v_int(v.p.blk || v.p.fn);
    if (v.k != V_INT)
        return 0;
    *out = fit(v, ct_unqual(e->t));
    return 1;
}

/* An immediate invocation (a consteval function's call): 1 if it is a
 * constant expression (*val its value, an integer's), -1 if it is not
 * (*reason why), 0 if this interpreter cannot tell (it runs at run
 * time, as a constexpr call would). */
int cx_immediate(struct cexpr *e, struct w128 *val, const char **reason)
{
    struct cval v;
    if (eval_top(e, &v)) {
        if (v.k == V_INT && ct_is_integer(e->t))
            *val = w_of(fit(v, ct_unqual(e->t)));
        return 1;
    }
    *reason = why;
    return definite ? -1 : 0;
}

int cx_consteval_int(struct cexpr *e, long *out)
{
    struct cval v;
    if (!run(e, &v))
        return 0;
    if (is128(ct_unqual(e->t)) &&
        (ct_unqual(e->t)->k == CT_INT128 ? !fits_long(v)
                                         : v.hi != 0 || v.i < 0))
        return 0;                 /* (as expr_fold: a long must hold it) */
    *out = v.i;
    return 1;
}

/* e's value in 128 bits, extended as its type says */
int cx_consteval_w128(struct cexpr *e, struct w128 *out)
{
    struct cval v;
    if (!run(e, &v))
        return 0;
    *out = w_of(v);
    return 1;
}
