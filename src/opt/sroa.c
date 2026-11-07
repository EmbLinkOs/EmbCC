/* ---- SROA: scalar replacement of aggregates (-O2) ------------------------
 *
 * mem2reg promotes a local out of memory only when nothing takes its
 * address -- and reading a struct field IS taking its address. `s.b = 1`
 * lowers to `addr s`, `add #4`, `store`, so every struct local, however
 * private, stays on the stack and every field access is a real memory
 * reference: invisible to value numbering, to store forwarding, and to
 * the register allocator alike. Across lib/libc and lib/libcxx that is
 * where most of the remaining stack traffic at -O2 came from.
 *
 * It does not have to be. When a local's address never leaves the
 * `addr` / `add constant` / `load` / `store` shape, the bytes it is
 * accessed at are independent scalars -- nothing in the program can
 * observe that they share an object. So the local is SPLIT: one fresh
 * scalar local per distinct (offset, size), and each access becomes an
 * ordinary LDVAR/STVAR of it. This pass only renames memory. What makes
 * the code faster is mem2reg, which now finds variables where it used to
 * find a struct, and everything downstream of that.
 *
 * Every condition is a form of "the address does not escape, and the
 * pieces do not overlap":
 *
 *   - each use of `addr L` is a load or a store, directly or through an
 *     add of a CONSTANT. A variable index could name any field, so no;
 *     and the address reaching a call, a store, a compare or anything
 *     else at all means the object is still one object.
 *   - no access is volatile, and neither is the local.
 *   - each (offset, size) lies inside the object, and two distinct ones
 *     never overlap. Two accesses at the same offset with different
 *     widths -- a union punned, a byte pulled out of an int -- are one
 *     object and not two, and the local is refused rather than split
 *     into pieces that would each hold half the truth.
 *
 * The new locals are appended after the existing ones, so every TEMP
 * renumbers UP by however many were added: slots and temps share one
 * numbering space with the slots first (each_read's comment). That is
 * the same shift the inliner performs, through the same remap.
 */

#include "opt_int.h"

#define SROA_MAX_FIELDS 16    /* distinct pieces one local may split into */
#define SROA_MAX_SIZE  128    /* bytes; past this it is a buffer, not a record */

struct sroa_fld { int off, size, nl; };

/* The scalar member type covering [off, off+size) in `t`, or NULL when
 * those bytes are not exactly one scalar. The STORAGE would be right
 * either way -- a slot of `size` bytes written and read at `size` bytes
 * holds any field -- but naming a double a double is what lets mem2reg's
 * float path see it, and what makes the remark readable. */
static struct type *sroa_field_ty(struct type *t, int off, int size)
{
    for (int hop = 0; t && hop < 8; hop++) {
        if (off == 0 && ty_size(t) == size &&
            (ty_is_integer(t) || t->kind == TY_PTR || ty_is_float(t)))
            return t;
        if (t->kind == TY_ARRAY) {
            int es = ty_size(t->pointee);
            if (es <= 0 || off % es != 0)
                return NULL;
            off %= es;
            t = t->pointee;
            continue;
        }
        if (t->kind != TY_STRUCT || !t->complete)
            return NULL;
        struct member *m = NULL;
        for (int k = 0; k < t->nmembers; k++) {
            struct member *c = &t->members[k];
            int cs = c->ty ? ty_size(c->ty) : 0;
            if (c->is_bitfield || cs <= 0)
                continue;
            if (off >= c->off && off + size <= c->off + cs) { m = c; break; }
        }
        if (!m)
            return NULL;
        off -= m->off;
        t = m->ty;
    }
    return NULL;
}

/* A vreg's value, when it is a constant, by EVALUATING the chain rather
 * than waiting for the folder.
 *
 * This pass runs before the fixpoint, so nothing has been folded yet and
 * `a[2]` is still four instructions -- `const 2`, `ext`, `mul 4`, `add`.
 * Accepting only a literal would refuse every array subscript in the
 * program, which is most of what there is to split. */
static int sroa_const(struct ir_func *fn, struct defs *d, int v, long *out,
                      int hop)
{
    if (hop > 6 || v < 0 || v >= fn->nvregs || d->cnt[v] != 1)
        return 0;
    int n = d->ins[v];
    if (n < 0)
        return 0;                       /* a parameter: not a constant */
    struct ir_ins *i = &fn->ins[n];
    long a, b;
    switch (i->op) {
    case IR_CONST:
        *out = i->imm;
        return 1;
    case IR_MOV:
        return sroa_const(fn, d, i->a, out, hop + 1);
    case IR_EXT:
        if (i->flt || !sroa_const(fn, d, i->a, &a, hop + 1))
            return 0;
        *out = fold_ext(a, i->size, i->sign, i->w);
        return 1;
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_SHL:
        if (i->flt || !sroa_const(fn, d, i->a, &a, hop + 1))
            return 0;
        if (i->imm_b)
            b = i->imm;
        else if (!sroa_const(fn, d, i->b, &b, hop + 1))
            return 0;
        /* In unsigned, because a signed overflow here is undefined and
         * an offset that wraps is one the caller will reject anyway. */
        {
            unsigned long A = (unsigned long)a, B = (unsigned long)b, r;
            if (i->op == IR_ADD)      r = A + B;
            else if (i->op == IR_SUB) r = A - B;
            else if (i->op == IR_MUL) r = A * B;
            else if (b < 0 || b > 63) return 0;
            else                      r = A << B;
            *out = norm((long)r, i->w);
        }
        return 1;
    default:
        return 0;
    }
}

/* An address widened from the target's pointer width with zeros: the same
 * address. AVR's pointers are two bytes in four-byte values, and a pointer
 * variable mem2reg has promoted is read as `ext.4:2` of what it held --
 * which hid `int *p = &x.f` from the second look. */
static int sroa_addr_ext(const struct ir_ins *i)
{
    return i->op == IR_EXT && !i->sign && !i->flt &&
           i->size == target_ptr_size() && i->w > i->size;
}

/* A read of a tracked address anywhere this pass cannot account for: the
 * object escapes, so it stays whole. */
struct sroa_ctx { char *cand; const char **why; const int *abase; int nvr; };
static void sroa_esc_cb(int *p, void *ctx)
{
    struct sroa_ctx *c = ctx;
    int v = *p;
    if (v >= 0 && v < c->nvr && c->abase[v] >= 0 && c->cand[c->abase[v]]) {
        c->cand[c->abase[v]] = 0;
        c->why[c->abase[v]] = "address-escapes";
    }
}

/* One line per local this pass had an opinion about, named as the
 * programmer named it. `nfld` is read only for a local that split, so
 * the early "nothing here was eligible" exit may pass NULL. */
static void sroa_report(struct ir_func *fn, int nparams, int nvars,
                        const char *cand, const int *nfld,
                        const char **why, int report_refusals)
{
    if (!remarks_on() || !fn->src)
        return;
    for (int L = 0; L < nvars; L++) {
        if (L < nparams && (!cand[L] || !nfld))
            continue;               /* a parameter is reported when it splits */
        const struct ir_dbgvar *v = local_var(fn, L);
        if (!v || !v->name || v->name[0] == '<')  /* a compiler-invented name */
            continue;
        int line = v->line ? v->line : fn->line;
        if (cand[L])
            remark_add("sroa", "split-into-scalars", v->name,
                       "address-never-escapes", fn->file, line,
                       "%d bytes -> %d variable%s", fn->locals[L].size,
                       nfld[L], nfld[L] == 1 ? "" : "s");
        else if (why[L] && report_refusals)
            remark_add("sroa", "kept-whole", v->name, why[L],
                       fn->file, line, NULL);
    }
}

/* `report_refusals` belongs to the LAST look this function gets. The
 * pass runs twice -- once before mem2reg and once after -- and a
 * refusal from the first is provisional: the whole point of the second
 * is that some of them stop being true. Only the last one has anything
 * worth telling a person. */
/* ---- a compare-exchange's `expected`, by value --------------------------
 *
 * __atomic_compare_exchange_n(obj, &expected, desired, ...) takes
 * `expected` by ADDRESS, and the address is all that keeps it in memory.
 * A lock's fast path is exactly this --
 *
 *     int c = 0;
 *     if (__atomic_compare_exchange_n(&l->v, &c, 1, 0, ACQ, RLX)) return;
 *
 * -- and mem2reg refuses `c` for having its address taken, so the lock
 * stored a zero to the frame, took its address, loaded it back for the
 * compare-exchange and stored the value seen through it again, where a
 * register would have done (lib/libc's __lock, mtx_lock, call_once).
 *
 * When every address of a scalar local feeds compare-exchanges as their
 * `expected` and nothing else, the local is private: nobody else can see
 * it, so reading it before and writing the value seen after is the same
 * program. That is IR_CAS, the by-value form the __sync builtins use:
 *
 *     e = ldvar v ; s = cas [obj], e, desired ; stvar v, s ; r = (s == e)
 *
 * (s re-extended first when the object is narrower than four bytes, as
 * irgen does for __sync_bool_compare_and_swap). Then nothing takes v's
 * address and mem2reg promotes it. Writing v on a match as well stores
 * the value it already holds, which a private local cannot show. */
struct cx_read { struct ir_ins *i; const int *of; int nv; signed char *st; };
static void cx_read_cb(int *p, void *ctx)
{
    struct cx_read *r = ctx;
    int t = *p;
    if (t < 0 || t >= r->nv || r->of[t] < 0)
        return;
    /* the expected operand of a compare-exchange as wide as the local,
     * and not also its object or its desired value */
    if (r->i->op == IR_CMPXCHG && p == &r->i->b && r->i->a != t &&
        r->i->c != t)
        return;
    r->st[r->of[t]] = -1;
}

int pass_cxlocal(struct ir_func *fn)
{
    int nv = fn->nvregs, nvars = fn->nvars;
    if (nvars == 0 || fn->nins == 0 || fn->neh)
        return 0;
    struct defs d;
    compute_defs(fn, &d);
    /* per local: 0 untouched, 1 a candidate, -1 refused */
    signed char *st = xcalloc((size_t)nvars, 1);
    int *of = xmalloc((size_t)nv * sizeof *of);   /* temp -> its local */
    for (int v = 0; v < nv; v++)
        of[v] = -1;
    int any = 0;
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        if (i->op != IR_ADDR || i->a < 0 || i->a >= nvars)
            continue;
        if (i->dst < nvars || i->dst >= nv || d.cnt[i->dst] != 1) {
            st[i->a] = -1;
            continue;
        }
        of[i->dst] = i->a;
        if (st[i->a] == 0)
            st[i->a] = 1;
    }
    for (int L = 0; L < nvars; L++) {
        const struct ir_local *Li = &fn->locals[L];
        if (st[L] == 1 &&
            (!Li->is_int_or_ptr || Li->is_int128 || Li->is_volatile ||
             (Li->size != 1 && Li->size != 2 && Li->size != 4 &&
              Li->size != 8)))
            st[L] = -1;
    }
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        struct cx_read r = { i, of, nv, st };
        if (i->op == IR_ADDR)
            continue;                     /* its `a` is the slot itself */
        each_read(i, cx_read_cb, &r);
        if (i->op == IR_CMPXCHG && i->b >= 0 && i->b < nv && of[i->b] >= 0 &&
            (i->size != fn->locals[of[i->b]].size || i->flt))
            st[of[i->b]] = -1;
    }
    for (int L = 0; L < nvars; L++)
        if (st[L] == 1)
            any = 1;
    if (!any) {
        free(st); free(of); free_defs(&d);
        return 0;
    }

    struct ibuf nb = { 0, 0, 0 };
    int *newpos = fn->var_scope_lo
        ? xmalloc((size_t)(fn->nins + 1) * sizeof *newpos) : NULL;
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins o = fn->ins[n];
        if (newpos) newpos[n] = nb.n;
        if (o.op == IR_ADDR && o.a >= 0 && o.a < nvars && st[o.a] == 1)
            continue;                     /* read by nothing now */
        if (o.op != IR_CMPXCHG || o.b < 0 || o.b >= nv || of[o.b] < 0 ||
            st[of[o.b]] != 1) {
            *ib_push(&nb) = o;
            continue;
        }
        int L = of[o.b];
        int e = fn->nvregs++, sv = fn->nvregs++, x = sv;
        struct ir_ins *p = ib_push(&nb);
        p->op = IR_LDVAR; p->a = L; p->dst = e;
        p->size = o.size; p->sign = o.sign; p->w = o.w;
        p->line = o.line; p->col = o.col; p->synth = o.synth;
        p = ib_push(&nb);
        p->op = IR_CAS; p->a = o.a; p->b = e; p->c = o.c; p->dst = sv;
        p->size = o.size; p->sign = o.sign; p->w = o.w; p->mo = o.mo;
        p->line = o.line; p->col = o.col; p->synth = o.synth;
        if (o.size < 4) {
            x = fn->nvregs++;
            p = ib_push(&nb);
            p->op = IR_EXT; p->a = sv; p->dst = x;
            p->size = o.size; p->sign = o.sign; p->w = o.w;
            p->line = o.line; p->col = o.col; p->synth = o.synth;
        }
        p = ib_push(&nb);
        p->op = IR_STVAR; p->dst = L; p->a = x; p->size = o.size;
        p->line = o.line; p->col = o.col; p->synth = o.synth;
        p = ib_push(&nb);
        p->op = IR_CMP; p->pred = B_EQ; p->a = x; p->b = e; p->dst = o.dst;
        p->w = o.w; p->sign = o.sign;
        p->line = o.line; p->col = o.col; p->synth = o.synth;
    }
    if (newpos) {
        newpos[fn->nins] = nb.n;
        for (int v = 0; v < nvars; v++) {
            int lo = fn->var_scope_lo[v], hi = fn->var_scope_hi[v];
            if (lo >= 0 && lo <= fn->nins) fn->var_scope_lo[v] = newpos[lo];
            if (hi >= 0 && hi <= fn->nins) fn->var_scope_hi[v] = newpos[hi];
        }
        free(newpos);
    }
    free(fn->ins);
    fn->ins = nb.p; fn->nins = nb.n; fn->cap = nb.cap;
    free(st); free(of); free_defs(&d);
    return 1;
}

/* ---- a small struct copied whole: copied field by field ----------------
 *
 * `h = get();`, `*out = h;`, `k = h;` -- a struct assignment is one
 * memcpy of the whole object, and a memcpy is an address use SROA cannot
 * account for, so the struct stayed in memory however private it was.
 * The typed handles of an RTOS API are exactly this: a one-field struct
 * returned, assigned and returned again, never touched by field.
 *
 * When a local's address is used only by loads, stores, constant offsets
 * and whole-object copies, each copy that touches it becomes one load and
 * one store per field, at the field's own offset and width. That is what
 * SROA can split, and what mem2reg then promotes. The fields are the
 * struct type's own: integers and pointers only (a float field would be
 * copied through an integer register, which the float pieces SROA names
 * would not match), no bit-field (whose unit is shared), no union (whose
 * members overlap), at most four, each aligned in an aligned struct --
 * so a packed one, whose fields may straddle, is left to memcpy. */
#define AGG_MAX_LEAVES 4
struct agg_leaf { int off, size, sign, align; };

static int agg_leaves(const struct type *t, int base, struct agg_leaf *out,
                      int n)
{
    if (!t || t->is_volatile)
        return -1;
    if (ty_is_integer(t) || t->kind == TY_PTR) {
        if (t->kind == TY_INT128 || n >= AGG_MAX_LEAVES)
            return -1;
        out[n].off = base;
        out[n].size = ty_size(t);
        out[n].sign = ty_signed_int(t);
        out[n].align = ty_align(t);
        return n + 1;
    }
    if (t->kind == TY_ARRAY) {
        if (t->count <= 0 || t->vla_len)
            return -1;
        for (int k = 0; k < t->count && n >= 0; k++)
            n = agg_leaves(t->pointee, base + k * ty_size(t->pointee), out, n);
        return n;
    }
    if (t->kind != TY_STRUCT || t->is_union || t->is_complex || !t->complete)
        return -1;
    for (int k = 0; k < t->nmembers && n >= 0; k++) {
        const struct member *m = &t->members[k];
        if (m->is_bitfield || !m->ty)
            return -1;
        n = agg_leaves(m->ty, base + m->off, out, n);
    }
    return n;
}

struct agg_ctx { struct ir_ins *i; const int *of; const long *off;
                 const struct ir_func *fn; signed char *st; int nv; };
static void agg_read_cb(int *p, void *ctx)
{
    struct agg_ctx *c = ctx;
    const struct ir_ins *i = c->i;
    int v = *p;
    if (v < 0 || v >= c->nv)
        return;
    int L = c->of[v];
    if (L < 0)
        return;
    if ((i->op == IR_LOAD && p == &i->a) ||
        (i->op == IR_STORE && p == &i->a) ||
        (i->op == IR_ADD && p == &i->a && c->of[i->dst] == L) ||
        ((i->op == IR_MOV || sroa_addr_ext(i)) && c->of[i->dst] == L) ||
        (i->op == IR_MEMCPY && (p == &i->a || p == &i->b) &&
         c->off[v] == 0 && i->size == c->fn->locals[L].size &&
         i->a != i->b))
        return;
    c->st[L] = -1;
}

int pass_aggcopy(struct ir_func *fn)
{
    int nvars = fn->nvars, nparams = fn->nparams, nv = fn->nvregs;
    int aw = target_ptr_size();
    if (nvars <= nparams || fn->nins == 0 || !fn->src || !fn->src->var_tys)
        return 0;
    struct defs d;
    compute_defs(fn, &d);
    int *of = xmalloc((size_t)(nv ? nv : 1) * sizeof *of);
    long *off = xmalloc((size_t)(nv ? nv : 1) * sizeof *off);
    for (int v = 0; v < nv; v++) { of[v] = -1; off[v] = 0; }
    for (int round = 0; round < 2; round++)
        for (int n = 0; n < fn->nins; n++) {
            const struct ir_ins *i = &fn->ins[n];
            int dst = def_target(i);
            if (dst < 0 || dst >= nv || d.cnt[dst] != 1)
                continue;
            if (i->op == IR_ADDR && i->a >= nparams && i->a < nvars) {
                of[dst] = i->a;
                off[dst] = 0;
            } else if ((i->op == IR_MOV || sroa_addr_ext(i)) && i->a >= 0 &&
                       i->a < nv && of[i->a] >= 0) {
                of[dst] = of[i->a];
                off[dst] = off[i->a];
            } else if (i->op == IR_ADD && i->w == aw && i->a >= 0 &&
                       i->a < nv && of[i->a] >= 0) {
                long k;
                if (i->imm_b)
                    k = i->imm;
                else if (!sroa_const(fn, &d, i->b, &k, 0))
                    continue;
                of[dst] = of[i->a];
                off[dst] = off[i->a] + k;
            }
        }
    /* per local: 0 no whole copy, 1 a candidate, -1 refused */
    signed char *st = xcalloc((size_t)nvars, 1);
    struct agg_leaf *lv = xcalloc((size_t)nvars * AGG_MAX_LEAVES, sizeof *lv);
    int *nl = xcalloc((size_t)nvars, sizeof *nl);
    int any = 0;
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        if (i->op != IR_MEMCPY || i->vol)
            continue;
        for (int k = 0; k < 2; k++) {
            int v = k ? i->b : i->a;
            if (v >= 0 && v < nv && of[v] >= 0 && off[v] == 0 &&
                st[of[v]] == 0 && i->size == fn->locals[of[v]].size)
                st[of[v]] = 1;
        }
    }
    for (int L = nparams; L < nvars; L++) {
        if (st[L] != 1)
            continue;
        const struct ir_local *Li = &fn->locals[L];
        const struct type *t = fn->src->var_tys[L];
        int k = Li->is_volatile || Li->size > 16 || !t ? -1
              : agg_leaves(t, 0, &lv[L * AGG_MAX_LEAVES], 0);
        for (int j = 0; j < k; j++) {
            const struct agg_leaf *f = &lv[L * AGG_MAX_LEAVES + j];
            if (f->off % f->align || ty_align(t) < f->align)
                k = -1;                    /* packed: may straddle */
        }
        if (k <= 0)
            st[L] = -1;
        else
            nl[L] = k;
    }
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        struct agg_ctx c = { i, of, off, fn, st, nv };
        if (i->op == IR_ADDR)
            continue;
        each_read(i, agg_read_cb, &c);
    }
    for (int L = nparams; L < nvars; L++)
        if (st[L] == 1)
            any = 1;
    if (!any) {
        free(of); free(off); free(st); free(lv); free(nl); free_defs(&d);
        return 0;
    }

    struct ibuf nb = { 0, 0, 0 };
    int *newpos = fn->var_scope_lo
        ? xmalloc((size_t)(fn->nins + 1) * sizeof *newpos) : NULL;
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins o = fn->ins[n];
        if (newpos) newpos[n] = nb.n;
        int La = o.op == IR_MEMCPY && o.a >= 0 && o.a < nv ? of[o.a] : -1;
        int Lb = o.op == IR_MEMCPY && o.b >= 0 && o.b < nv ? of[o.b] : -1;
        int L = La >= 0 && st[La] == 1 ? La : Lb >= 0 && st[Lb] == 1 ? Lb : -1;
        if (L < 0 || o.vol) {
            *ib_push(&nb) = o;
            continue;
        }
        for (int j = 0; j < nl[L]; j++) {
            const struct agg_leaf *f = &lv[L * AGG_MAX_LEAVES + j];
            int sa = o.b, da = o.a, t;
            struct ir_ins *p;
            if (f->off) {
                /* as irgen forms a member's address: a constant and an
                 * add of it -- an immediate form is the backends' (immfold
                 * makes it last), and value numbering keys an add by its
                 * operands, so two `add #k` of one base would be one */
                int k = fn->nvregs++;
                p = ib_push(&nb);
                p->op = IR_CONST; p->imm = f->off; p->w = aw; p->dst = k;
                p->line = o.line; p->col = o.col; p->synth = o.synth;
                sa = fn->nvregs++;
                p = ib_push(&nb);
                p->op = IR_ADD; p->a = o.b; p->b = k; p->w = aw; p->dst = sa;
                p->line = o.line; p->col = o.col; p->synth = o.synth;
                da = fn->nvregs++;
                p = ib_push(&nb);
                p->op = IR_ADD; p->a = o.a; p->b = k; p->w = aw; p->dst = da;
                p->line = o.line; p->col = o.col; p->synth = o.synth;
            }
            t = fn->nvregs++;
            p = ib_push(&nb);
            p->op = IR_LOAD; p->a = sa; p->dst = t; p->size = f->size;
            p->sign = f->sign; p->w = f->size == 8 ? 8 : 4; p->natural = 1;
            p->line = o.line; p->col = o.col; p->synth = o.synth;
            p = ib_push(&nb);
            p->op = IR_STORE; p->a = da; p->b = t; p->size = f->size;
            p->natural = 1;
            p->line = o.line; p->col = o.col; p->synth = o.synth;
        }
    }
    if (newpos) {
        newpos[fn->nins] = nb.n;
        for (int v = 0; v < nvars; v++) {
            int lo = fn->var_scope_lo[v], hi = fn->var_scope_hi[v];
            if (lo >= 0 && lo <= fn->nins) fn->var_scope_lo[v] = newpos[lo];
            if (hi >= 0 && hi <= fn->nins) fn->var_scope_hi[v] = newpos[hi];
        }
        free(newpos);
    }
    free(fn->ins);
    fn->ins = nb.p; fn->nins = nb.n; fn->cap = nb.cap;
    free(of); free(off); free(st); free(lv); free(nl); free_defs(&d);
    return 1;
}

/* The places a split object is read or written whole (pass_sroa, step 3):
 * instruction n (-1: the function's entry), the slot, and whether the
 * pieces go to memory before it and come back after. */
struct sroa_bnd { int n, L, before, after, ret; };
struct sroa_bnds { struct sroa_bnd *e; int n, cap; };

static void sroa_bnd_add(struct sroa_bnds *b, int n, int L, int before,
                         int after)
{
    if (b->n == b->cap) {
        b->cap = b->cap ? 2 * b->cap : 8;
        b->e = xrealloc(b->e, (size_t)b->cap * sizeof *b->e);
    }
    b->e[b->n].n = n; b->e[b->n].L = L;
    b->e[b->n].before = before; b->e[b->n].after = after;
    b->e[b->n].ret = 0;
    b->n++;
}

/* Emit one piece's transfer between its variable and the slot:
 * `addr L (+ off)` and a load or a store of `size` bytes. */
static void sroa_xfer(struct ir_func *fn, struct ibuf *nb, int L,
                      const struct sroa_fld *F, int to_mem,
                      const struct ir_ins *at)
{
    int pw = target_ptr_size();
    int q = fn->nvregs++, t = fn->nvregs++;
    struct ir_ins *e = ib_push(nb);
    memset(e, 0, sizeof *e);
    e->op = IR_ADDR; e->dst = q; e->a = L; e->b = -1; e->w = pw;
    e->line = at ? at->line : fn->line; e->synth = 1;
    if (F->off) {
        int k = fn->nvregs++, q2 = fn->nvregs++;
        e = ib_push(nb);
        memset(e, 0, sizeof *e);
        e->op = IR_CONST; e->dst = k; e->imm = F->off; e->w = pw; e->sign = 1;
        e->a = e->b = -1; e->synth = 1;
        e = ib_push(nb);
        memset(e, 0, sizeof *e);
        e->op = IR_ADD; e->dst = q2; e->a = q; e->b = k; e->w = pw; e->sign = 1;
        e->synth = 1;
        q = q2;
    }
    int w = F->size < 4 ? 4 : F->size;
    if (to_mem) {
        e = ib_push(nb);
        memset(e, 0, sizeof *e);
        e->op = IR_LDVAR; e->dst = t; e->a = F->nl; e->b = -1;
        e->size = F->size; e->w = w; e->synth = 1;
        e = ib_push(nb);
        memset(e, 0, sizeof *e);
        e->op = IR_STORE; e->dst = -1; e->a = q; e->b = t;
        e->size = F->size; e->w = w; e->synth = 1;
    } else {
        e = ib_push(nb);
        memset(e, 0, sizeof *e);
        e->op = IR_LOAD; e->dst = t; e->a = q; e->b = -1;
        e->size = F->size; e->w = w; e->synth = 1;
        e = ib_push(nb);
        memset(e, 0, sizeof *e);
        e->op = IR_STVAR; e->dst = F->nl; e->a = t; e->b = -1;
        e->size = F->size; e->w = w; e->synth = 1;
    }
}

static void sroa_sync(struct ir_func *fn, const struct sroa_bnds *bl,
                      const char *cand, const struct sroa_fld *fld,
                      const int *nfld)
{
    struct ibuf nb = { 0, 0, 0 };
    int *newpos = fn->var_scope_lo
        ? xmalloc((size_t)(fn->nins + 1) * sizeof *newpos) : NULL;
    /* the entry's loads: after the function's leading labels */
    for (int k = 0; k < bl->n; k++) {
        const struct sroa_bnd *b = &bl->e[k];
        if (b->n >= 0 || !cand[b->L])
            continue;
        for (int f = 0; f < nfld[b->L]; f++)
            sroa_xfer(fn, &nb, b->L, &fld[(size_t)b->L * SROA_MAX_FIELDS + f],
                      0, NULL);
    }
    for (int n = 0; n < fn->nins; n++) {
        if (newpos) newpos[n] = nb.n;
        struct ir_ins at = fn->ins[n];
        for (int k = 0; k < bl->n; k++) {
            const struct sroa_bnd *b = &bl->e[k];
            if (b->n != n || !b->before || !cand[b->L])
                continue;
            for (int f = 0; f < nfld[b->L]; f++)
                sroa_xfer(fn, &nb, b->L,
                          &fld[(size_t)b->L * SROA_MAX_FIELDS + f], 1, &at);
        }
        for (int k = 0; k < bl->n; k++) {
            const struct sroa_bnd *b = &bl->e[k];
            if (b->n != n || !b->ret || !cand[b->L])
                continue;
            /* the pieces as values, in the units' order */
            int uo[2], us[2], uf[2], tv[2] = { -1, -1 };
            int nu = target_ret_units(fn, uo, us, uf, 2);
            for (int q = 0; q < nu; q++) {
                const struct sroa_fld *F = NULL;
                for (int f = 0; f < nfld[b->L]; f++)
                    if (fld[(size_t)b->L * SROA_MAX_FIELDS + f].off == uo[q])
                        F = &fld[(size_t)b->L * SROA_MAX_FIELDS + f];
                tv[q] = fn->nvregs++;
                struct ir_ins *e = ib_push(&nb);
                memset(e, 0, sizeof *e);
                e->op = IR_LDVAR; e->dst = tv[q]; e->a = F->nl; e->b = -1;
                e->size = F->size; e->w = F->size < 4 ? 4 : F->size;
                e->line = at.line; e->synth = 1;
            }
            at.a = -1;
            at.nrp = nu;
            at.b = tv[0];
            at.c = nu > 1 ? tv[1] : -1;
        }
        *ib_push(&nb) = at;
        for (int k = 0; k < bl->n; k++) {
            const struct sroa_bnd *b = &bl->e[k];
            if (b->n != n || !b->after || !cand[b->L])
                continue;
            for (int f = 0; f < nfld[b->L]; f++)
                sroa_xfer(fn, &nb, b->L,
                          &fld[(size_t)b->L * SROA_MAX_FIELDS + f], 0, &at);
        }
    }
    if (newpos) {
        newpos[fn->nins] = nb.n;
        for (int v = 0; v < fn->nvars; v++) {
            int lo = fn->var_scope_lo[v], hi = fn->var_scope_hi[v];
            if (lo >= 0 && lo <= fn->nins) fn->var_scope_lo[v] = newpos[lo];
            if (hi >= 0 && hi <= fn->nins) fn->var_scope_hi[v] = newpos[hi];
        }
        free(newpos);
    }
    free(fn->ins);
    fn->ins = nb.p; fn->nins = nb.n; fn->cap = nb.cap;
}

int pass_sroa(struct ir_func *fn, int report_refusals)
{
    int nvars = fn->nvars, nparams = fn->nparams, nvr = fn->nvregs;
    static int no_bnd = -1;
    if (no_bnd < 0)
        no_bnd = getenv("EMBCC_NO_SROAB") != NULL;
    /* p0: the first slot considered -- the parameters too, with the
     * boundaries below */
    int p0 = no_bnd ? nparams : 0;
    if (nvars <= p0 || fn->nins == 0 || !fn->src || !fn->src->var_tys)
        return 0;

    struct defs d;
    compute_defs(fn, &d);

    /* 1. Which locals are worth asking about. Aggregates are the point,
     * but a SCALAR belongs here too: mem2reg refuses one whose address
     * is taken, and `int x; int *p = &x; ... *p` is that -- a one-field
     * object, split into one variable, which mem2reg can then promote.
     * A local nothing takes the address of falls out with no fields and
     * costs a scan. Parameters stay out: a struct one is written to its
     * slot by the PROLOGUE, which is not IR this pass can see. */
    char *cand = xcalloc((size_t)nvars, 1);
    const char **why = xcalloc((size_t)nvars, sizeof *why);
    int ncand = 0;
    /* Only a local something takes the ADDRESS of is this pass's
     * business. Without that there is nothing to split and nothing to
     * explain -- mem2reg already answers for it -- and asking anyway
     * would put a line in the remarks for every variable in the
     * program. */
    for (int n = 0; n < fn->nins; n++)
        if (fn->ins[n].op == IR_ADDR && fn->ins[n].a >= p0 &&
            fn->ins[n].a < nvars)
            cand[fn->ins[n].a] = 1;
    for (int L = p0; L < nvars; L++) {
        const struct ir_local *Li = &fn->locals[L];
        struct type *t = fn->src->var_tys[L];
        if (!cand[L])                                continue;
        cand[L] = 0;
        if (!t || Li->size <= 0)                     continue;
        if (Li->is_volatile)      { why[L] = "declared-volatile";   continue; }
        if (Li->is_int128 || Li->is_ldouble)
                                  { why[L] = "is-a-wide-scalar";    continue; }
        if (Li->size > SROA_MAX_SIZE)
                                  { why[L] = "too-large-to-split";  continue; }
        if (t->kind == TY_ARRAY && !t->count)
                                  { why[L] = "is-a-variable-length-array"; continue; }
        cand[L] = 1; ncand++;
    }
    if (!ncand) {
        /* Not nothing to say: "declared volatile" is a refusal, and the
         * only local in its function can be the one that was refused. */
        sroa_report(fn, nparams, nvars, cand, NULL, why, report_refusals);
        free(cand); free(why); free_defs(&d);
        return 0;
    }

    /* 2. Follow `&L + constant` forward. Two rounds, because a use can
     * textually precede its definition across a back edge; the chains
     * themselves are two or three instructions long. */
    int *abase = xmalloc((size_t)(nvr ? nvr : 1) * sizeof *abase);
    long *aoff = xmalloc((size_t)(nvr ? nvr : 1) * sizeof *aoff);
    for (int v = 0; v < nvr; v++) { abase[v] = -1; aoff[v] = 0; }
    for (int round = 0; round < 2; round++)
        for (int n = 0; n < fn->nins; n++) {
            struct ir_ins *in = &fn->ins[n];
            int dst = def_target(in);
            int single = dst >= 0 && dst < nvr && d.cnt[dst] == 1;
            if (in->op == IR_ADDR) {
                int L = in->a;
                if (L < 0 || L >= nvars || !cand[L])
                    continue;
                if (!single) {          /* the address lands where we cannot follow it */
                    cand[L] = 0; why[L] = "address-held-in-a-reassigned-temp";
                    continue;
                }
                abase[dst] = L; aoff[dst] = 0;
            } else if (in->op == IR_MOV || sroa_addr_ext(in)) {
                if (in->a >= 0 && in->a < nvr && abase[in->a] >= 0 && single) {
                    abase[dst] = abase[in->a];
                    aoff[dst] = aoff[in->a];
                }
            } else if (in->op == IR_ADD && in->w == target_ptr_size()) {
                /* an address is the target's pointer width: 4 on the
                 * 32-bit machines and 2 on AVR, where this was 8 alone
                 * and no field past the first was ever followed */
                int bs = -1; long k = 0; int have = 0;
                if (in->a >= 0 && in->a < nvr && abase[in->a] >= 0) {
                    bs = in->a;
                    have = in->imm_b ? (k = in->imm, 1)
                                     : sroa_const(fn, &d, in->b, &k, 0);
                } else if (!in->imm_b && in->b >= 0 && in->b < nvr &&
                           abase[in->b] >= 0) {
                    bs = in->b;
                    have = sroa_const(fn, &d, in->a, &k, 0);
                }
                if (bs >= 0 && have && single) {
                    abase[dst] = abase[bs];
                    aoff[dst] = aoff[bs] + k;
                }
            }
        }

    /* 3. Check every use, and collect the pieces. */
    struct sroa_fld *fld =
        xcalloc((size_t)nvars * SROA_MAX_FIELDS, sizeof *fld);
    int *nfld = xcalloc((size_t)nvars, sizeof *nfld);
    struct sroa_ctx sc = { cand, why, abase, nvr };
    struct sroa_bnds bl = { 0, 0, 0 };
    int *nacc = xcalloc((size_t)nvars, sizeof *nacc);
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *in = &fn->ins[n];
        switch (in->op) {
        case IR_ADDR:
            break;                  /* the root; its `a` is a slot, not a read */
        case IR_MOV: case IR_ADD: case IR_EXT:
            /* Accounted for when the result was tracked above. Otherwise
             * the address flowed into arithmetic this pass cannot follow. */
            if (in->dst >= 0 && in->dst < nvr && abase[in->dst] >= 0)
                break;
            each_read(in, sroa_esc_cb, &sc);
            break;
        case IR_LDVAR: case IR_LOAD: case IR_STORE: case IR_STVAR: {
            /* A store whose VALUE is the address is the address escaping. */
            if (in->op == IR_STORE && in->b >= 0 && in->b < nvr &&
                abase[in->b] >= 0 && cand[abase[in->b]]) {
                cand[abase[in->b]] = 0; why[abase[in->b]] = "address-escapes";
            }
            if (in->op == IR_STVAR)
                each_read(in, sroa_esc_cb, &sc);   /* the value it writes */
            /* A whole-object read or write is an access like any other:
             * at offset 0, of the object's own width. For an aggregate
             * that width is not 1, 2, 4 or 8 and the local is refused
             * just below; for a SCALAR whose address was taken it is
             * exactly the one piece, so `int x = a; int *p = &x; *p = 2`
             * splits instead of being refused for mentioning x by name. */
            int L, sz = in->size;
            long off;
            if (in->op == IR_LDVAR || in->op == IR_STVAR) {
                L = in->op == IR_LDVAR ? in->a : in->dst;
                if (L < 0 || L >= nvars || !cand[L])
                    break;
                off = 0;
            } else {
                int ad = in->a;
                if (ad < 0 || ad >= nvr || abase[ad] < 0)
                    break;
                L = abase[ad];
                if (!cand[L])
                    break;
                off = aoff[ad];
            }
            if (in->vol) {
                cand[L] = 0; why[L] = "access-is-volatile"; break;
            }
            if (sz != 1 && sz != 2 && sz != 4 && sz != 8) {
                cand[L] = 0; why[L] = "access-is-not-1-2-4-or-8-bytes"; break;
            }
            if (off < 0 || off + sz > fn->locals[L].size) {
                cand[L] = 0; why[L] = "access-runs-outside-the-object"; break;
            }
            struct sroa_fld *F = &fld[(size_t)L * SROA_MAX_FIELDS];
            int k = 0;
            for (; k < nfld[L]; k++) {
                if (F[k].off == off && F[k].size == sz)
                    break;                        /* a piece already named */
                if (off < F[k].off + F[k].size && F[k].off < off + sz) {
                    cand[L] = 0;
                    why[L] = "two-accesses-overlap-at-different-widths";
                    break;
                }
            }
            if (cand[L])
                nacc[L]++;
            if (!cand[L] || k < nfld[L])
                break;
            if (nfld[L] == SROA_MAX_FIELDS) {
                cand[L] = 0; why[L] = "too-many-pieces"; break;
            }
            F[nfld[L]].off = (int)off;
            F[nfld[L]].size = sz;
            nfld[L]++;
            break;
        }
        /* The whole object read or written at once -- passed by value,
         * returned, copied from or into, cleared: a BOUNDARY. The pieces
         * are stored back before one and loaded again after it, so the
         * slot is right exactly when something reads it as a whole. */
        case IR_CALL:
            if (no_bnd) { each_read(in, sroa_esc_cb, &sc); break; }
            for (int k = 0; k < in->nargs; k++) {
                int v = in->argv[k].vreg;
                if (v < 0 || v >= nvr || abase[v] < 0 || !cand[abase[v]])
                    continue;
                int L = abase[v];
                if ((in->argv[k].is_struct || (k == 0 && in->sret_first)) &&
                    aoff[v] == 0 &&
                    (in->sret_first && k == 0 ? 1
                                              : in->argv[k].size == fn->locals[L].size))
                    sroa_bnd_add(&bl, n, L, 1, 1);
                else {
                    cand[L] = 0; why[L] = "address-escapes";
                }
            }
            if (in->indirect)
                sroa_esc_cb(&in->a, &sc);
            break;
        case IR_RET:
            if (in->a >= 0 && in->a < nvr && abase[in->a] >= 0 &&
                cand[abase[in->a]]) {
                int L = abase[in->a];
                if (!no_bnd && fn->ret_abi.is_struct && aoff[in->a] == 0 &&
                    fn->ret_abi.size == fn->locals[L].size)
                    sroa_bnd_add(&bl, n, L, 1, 0);
                else {
                    cand[L] = 0; why[L] = "address-escapes";
                }
            }
            break;
        case IR_MEMCPY: case IR_MEMZERO: {
            int sides = in->op == IR_MEMCPY ? 2 : 1;
            for (int sd = 0; sd < sides; sd++) {
                int v = sd ? in->b : in->a;
                if (v < 0 || v >= nvr || abase[v] < 0 || !cand[abase[v]])
                    continue;
                int L = abase[v];
                if (!no_bnd && aoff[v] == 0 && in->size == fn->locals[L].size &&
                    !in->vol)
                    sroa_bnd_add(&bl, n, L, sd == 1, sd == 0);
                else {
                    cand[L] = 0; why[L] = "address-escapes";
                }
            }
            break;
        }
        default:
            each_read(in, sroa_esc_cb, &sc);
            break;
        }
    }
    /* a parameter's slot is written by the prologue: its pieces are
     * loaded first of all */
    for (int L = p0; L < nparams; L++)
        if (cand[L])
            sroa_bnd_add(&bl, -1, L, 0, 1);

    /* Worth it only when the accesses split away outnumber what the
     * boundaries then cost: a piece stored back before each whole read
     * and loaded after each whole write. An argument built for one call
     * -- two fields stored, the call -- was two stores before and is two
     * stores, an address and two loads after. */
    /* A return whose register units (target_ret_units) are exactly the
     * pieces costs nothing: the pieces go back as values, IR_RET's nrp,
     * and the slot is not written for it at all. */
    for (int k = 0; k < bl.n; k++) {
        int L = bl.e[k].L, n = bl.e[k].n;
        if (!cand[L] || n < 0 || fn->ins[n].op != IR_RET ||
            getenv("EMBCC_NO_RETPARTS"))
            continue;
        int uo[2], us[2], uf[2];
        int nu = target_ret_units(fn, uo, us, uf, 2);
        if (nu < 1 || nu != nfld[L])
            continue;
        int all = 1;
        for (int q = 0; q < nu && all; q++) {
            int hit = 0;
            for (int f = 0; f < nfld[L]; f++)
                if (fld[(size_t)L * SROA_MAX_FIELDS + f].off == uo[q] &&
                    fld[(size_t)L * SROA_MAX_FIELDS + f].size == us[q])
                    hit = 1;
            all = hit;
        }
        if (all) {
            bl.e[k].ret = 1;
            bl.e[k].before = 0;
        }
    }
    for (int k = 0; k < bl.n; k++) {
        int L = bl.e[k].L;
        if (cand[L])
            nacc[L] -= nfld[L] * (bl.e[k].before + bl.e[k].after);
    }
    for (int k = 0; k < bl.n; k++) {
        int L = bl.e[k].L;
        if (cand[L] && nacc[L] <= 0) {
            cand[L] = 0; why[L] = "whole-object-uses-cost-more";
        }
    }
    free(nacc);

    /* 4. Number the new locals. A candidate nothing ever reached through
     * the chain has nothing to split. */
    int nnew = 0;
    for (int L = p0; L < nvars; L++) {
        if (cand[L] && nfld[L] == 0)
            cand[L] = 0;          /* nothing ever reached it through the chain */
        if (cand[L])
            nnew += nfld[L];
    }
    sroa_report(fn, nparams, nvars, cand, nfld, why, report_refusals);
    if (!nnew) {
        free(cand); free(why); free(abase); free(aoff);
        free(fld); free(nfld); free_defs(&d); free(bl.e);
        return 0;
    }
    int next = nvars;
    for (int L = p0; L < nvars; L++)
        if (cand[L])
            for (int k = 0; k < nfld[L]; k++)
                fld[(size_t)L * SROA_MAX_FIELDS + k].nl = next++;

    /* 5. Decide each rewrite BEFORE renumbering, because abase/aoff are
     * indexed by the old vreg numbers and the new locals occupy exactly
     * the range the temps are about to move out of. */
    int *rw = xcalloc((size_t)(fn->nins ? fn->nins : 1), sizeof *rw);
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *in = &fn->ins[n];
        int L, off;
        if (in->op == IR_LDVAR || in->op == IR_STVAR) {
            L = in->op == IR_LDVAR ? in->a : in->dst;
            off = 0;
            if (L < 0 || L >= nvars || !cand[L])
                continue;
        } else if (in->op == IR_LOAD || in->op == IR_STORE) {
            int ad = in->a;
            if (ad < 0 || ad >= nvr || abase[ad] < 0 || !cand[abase[ad]])
                continue;
            L = abase[ad];
            off = (int)aoff[ad];
        } else {
            continue;
        }
        struct sroa_fld *F = &fld[(size_t)L * SROA_MAX_FIELDS];
        for (int k = 0; k < nfld[L]; k++)
            if (F[k].off == off && F[k].size == in->size) {
                rw[n] = F[k].nl + 1;
                break;
            }
    }

    /* 6. Grow the slot metadata -- both representations together, which is
     * the invariant ir.h warns about -- and shift the temps up. */
    int nv = nvars + nnew;
    struct type **vt = xmalloc((size_t)nv * sizeof *vt);
    int *va = xmalloc((size_t)nv * sizeof *va);
    for (int k = 0; k < nvars; k++) {
        vt[k] = fn->src->var_tys[k];
        va[k] = fn->src->var_aligns ? fn->src->var_aligns[k] : 0;
    }
    for (int L = p0; L < nvars; L++) {
        if (!cand[L])
            continue;
        struct sroa_fld *F = &fld[(size_t)L * SROA_MAX_FIELDS];
        for (int k = 0; k < nfld[L]; k++) {
            struct type *mt =
                sroa_field_ty(fn->src->var_tys[L], F[k].off, F[k].size);
            /* A piece no member types (an eight-byte read across two
             * words) is the integer exactly its size: `long` is four
             * bytes on ILP32 and `int` two on AVR, and a slot that size
             * held half the piece -- ColdFire's backend refused the
             * eight-byte read of a four-byte slot. (1, 2, 4 and 8 are
             * the only sizes a piece has, and each exists everywhere.) */
            if (!mt || mt->is_volatile || mt->kind == TY_LDOUBLE ||
                mt->kind == TY_INT128)
                mt = ty_int_of_size(F[k].size, 0);
            vt[F[k].nl] = mt;
            va[F[k].nl] = 0;        /* a piece nothing addresses needs no more
                                     * than its type's natural alignment */
        }
    }
    fn->src->var_tys = vt;
    fn->src->var_aligns = va;

    if (fn->var_scope_lo) {
        /* Each piece lives exactly as long as the object it came out of. */
        int *lo = xmalloc((size_t)nv * sizeof *lo);
        int *hi = xmalloc((size_t)nv * sizeof *hi);
        memcpy(lo, fn->var_scope_lo, (size_t)nvars * sizeof *lo);
        memcpy(hi, fn->var_scope_hi, (size_t)nvars * sizeof *hi);
        for (int L = p0; L < nvars; L++) {
            if (!cand[L])
                continue;
            struct sroa_fld *F = &fld[(size_t)L * SROA_MAX_FIELDS];
            for (int k = 0; k < nfld[L]; k++) {
                lo[F[k].nl] = lo[L];
                hi[F[k].nl] = hi[L];
            }
        }
        free(fn->var_scope_lo); free(fn->var_scope_hi);
        fn->var_scope_lo = lo; fn->var_scope_hi = hi;
    }

    struct rmp shift = { 0, nvars, nnew, 0, 0 };
    for (int n = 0; n < fn->nins; n++)
        remap_ins(&fn->ins[n], &shift);
    fn->nvregs = nvr + nnew;
    fn->nvars = nv;
    ir_locals_fill(fn, fn->src, nv);

    /* 7. And the accesses become ordinary variable reads and writes. The
     * size/sign/width of each one is carried over untouched: an LDVAR
     * extends on the way in exactly as the LOAD it replaces did. */
    int nrw = 0;
    for (int n = 0; n < fn->nins; n++) {
        if (!rw[n])
            continue;
        struct ir_ins *in = &fn->ins[n];
        int NL = rw[n] - 1;
        switch (in->op) {
        case IR_LOAD:
            in->op = IR_LDVAR;
            in->a = NL;
            break;
        case IR_STORE:
            in->op = IR_STVAR;
            in->a = in->b;          /* IR_STORE's value operand */
            in->b = -1;
            in->dst = NL;
            break;
        case IR_LDVAR:
            in->a = NL;             /* already a variable read: just retarget */
            break;
        default:                    /* IR_STVAR */
            in->dst = NL;
            break;
        }
        nrw++;
    }
    g_did.sroa += nrw;

    /* 8. The boundaries: each piece stored to the slot before a whole
     * read, loaded from it after a whole write (the prologue's, for a
     * parameter, is before everything). */
    int nb = 0;
    for (int k = 0; k < bl.n; k++)
        if (cand[bl.e[k].L])
            nb++;
    if (nb)
        sroa_sync(fn, &bl, cand, fld, nfld);

    free(rw); free(cand); free(why); free(abase); free(aoff);
    free(fld); free(nfld); free_defs(&d); free(bl.e);
    return 1;
}
