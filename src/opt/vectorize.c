/* ==== automatic vectorization (-O2) ========================================= *
 *
 * `for (i = 0; i < 4096; i++) a[i] = a[i] * 3 + 1;` does one element at
 * a time where the machine will do four. Both targets have 128-bit SIMD
 * in their base instruction set -- SSE2 is part of x86-64, Advanced SIMD
 * part of aarch64 -- so four ints an iteration needs no feature test.
 *
 * ---- the shape of the transform ------------------------------------------
 *
 * The usual construction is a vector loop over floor(n/VF)*VF followed by
 * a scalar loop for the remainder, which is two loops, a new block, and
 * an induction variable that has to start where the other stopped. This
 * does none of that. It only vectorizes a loop whose trip count is a
 * CONSTANT multiple of the vector factor, and then the whole transform
 * is local to the existing loop:
 *
 *   - the induction step becomes VF instead of 1
 *   - each scalar op becomes its lane-wise equivalent
 *
 * and nothing else moves. The address is already `base + i * esize`, so
 * stepping i by 4 steps the address by 16 on its own; the existing test
 * `i < 4096` already stops after 1024 iterations of four. No remainder,
 * no second loop, no new blocks, and the guard that decides whether the
 * loop runs at all is untouched.
 *
 * That is a real restriction -- `i < n` for a runtime n is not covered --
 * but it is the whole of the risk budget spent on the part that pays:
 * fixed-size array loops are common, and a vectorizer that quietly gets
 * the remainder wrong is a wrong answer in the most ordinary code there
 * is. The general trip count is the next step, not this one.
 *
 * ---- what may be vectorized ----------------------------------------------
 *
 * Every memory reference must be `global + i * esize` at the same i and
 * the same scale, so lane k of every reference is element i+k of its
 * array: there is no cross-lane dependence to get wrong, and two
 * distinct globals cannot overlap. A base that came from a pointer could
 * alias something and is refused. Every value is then either loaded,
 * loop-invariant (broadcast once, before the loop), or computed from
 * those by an op SSE2 and NEON both have lane-wise.
 *
 * Multiplication is the interesting refusal. A packed 32-bit multiply is
 * SSE4.1, not SSE2, so a multiply by a constant is decomposed into
 * shifts and adds -- `x * 3` is `(x << 1) + x` -- and a multiply by
 * anything else is simply not vectorized. gcc does the same
 * decomposition here for the same reason. */

#include "opt_int.h"

#define VEC_BYTES 16

/* Is `v` defined outside [lo, hi)? A loop-invariant operand, which a
 * vector op reaches by broadcasting it once before the loop. */
static int defined_outside(struct ir_func *fn, struct defs *d, int v,
                           int lo, int hi)
{
    if (v < 0 || v >= fn->nvregs || d->cnt[v] != 1)
        return 0;
    int def = d->ins[v];
    return def < 0 || def < lo || def >= hi;   /* < 0: a parameter */
}

/* Does the address in temp `p` read `base + iv * scale`, with base a
 * global's address defined outside the loop? Returns the base temp, or
 * -1. The chain irgen builds is add(gaddr, shl(ext(iv), log2 scale)),
 * with the ext absent when the index is already 64-bit. */
int addr_of_iv(struct ir_func *fn, struct defs *d, struct vecloop *L,
               int p, int scale)
{
    if (p < 0 || p >= fn->nvregs || d->cnt[p] != 1)
        return -1;
    int an = d->ins[p];
    if (an < 0 || fn->ins[an].op != IR_ADD)
        return -1;
    struct ir_ins *add = &fn->ins[an];
    /* Either operand may be the base; the other is the scaled index. */
    for (int side = 0; side < 2; side++) {
        int base = side ? add->b : add->a;
        int idx  = side ? add->a : add->b;
        if (base < 0 || base >= fn->nvregs || d->cnt[base] != 1)
            continue;
        /* Loop-invariant is all that is asked HERE; whether the bases
         * can alias each other is decided once, over all of them, by
         * the caller. A global's address qualifies wherever the
         * instruction sits, since it is the same every iteration. */
        int bd = d->ins[base];
        if (bd >= 0 && (bd >= L->lo && bd < L->hi) &&
            fn->ins[bd].op != IR_GADDR)
            continue;               /* computed inside the loop */
        if (idx < 0 || idx >= fn->nvregs || d->cnt[idx] != 1)
            continue;
        int sn = d->ins[idx];
        long sh;
        if (sn < 0 || fn->ins[sn].op != IR_SHL ||
            !const_b(fn, d, &fn->ins[sn], &sh) || sh < 0 || sh > 6)
            continue;
        if ((1L << sh) != scale)
            continue;
        int x = fn->ins[sn].a;
        if (x >= 0 && x < fn->nvregs && d->cnt[x] == 1) {
            int xn = d->ins[x];
            if (xn >= 0 && fn->ins[xn].op == IR_EXT)
                x = fn->ins[xn].a;   /* the widened index */
        }
        if (x == L->iv)
            return base;
    }
    return -1;
}

/* The lane-wise opcode character for a scalar op, or 0 for one SSE2 and
 * NEON do not both have. */
static int vec_op_char(enum ir_op op)
{
    switch (op) {
    case IR_ADD: return '+';
    case IR_SUB: return '-';
    case IR_AND: return '&';
    case IR_OR:  return '|';
    case IR_XOR: return '^';
    case IR_SHL: return '<';
    case IR_SHR: return '>';
    default:     return 0;
    }
}

/* Remap the vregs an instruction READS, for the copied vector body. */
struct vremap { const int *map; int n; };
static void vremap_cb(int *p, void *ctx)
{
    struct vremap *r = ctx;
    if (*p >= 0 && *p < r->n && r->map[*p] >= 0)
        *p = r->map[*p];
}

/* Build one lane-wise instruction from a scalar one, into `v`.
 * `a`/`b` are the operands already remapped; `splat` is the broadcast of
 * b when b was a constant, or -1. Returns 0 if the op has no lane-wise
 * form, which the recognizer should already have refused. */
static int vec_build(struct ir_ins *v, const struct ir_ins *src, int dst,
                     int a, int b, int splat, long kb, int hask, int esize)
{
    memset(v, 0, sizeof *v);
    v->op = IR_VBIN; v->dst = dst; v->a = a;
    v->size = esize; v->w = 8;
    v->line = src->line; v->col = src->col;
    int c = vec_op_char(src->op);
    if (!c)
        return 0;
    v->imm = c;
    if ((c == '<' || c == '>') && hask) {
        v->b = -1; v->c = (int)kb; v->sign = src->sign;
    } else if (hask) {
        v->b = splat;
    } else {
        v->b = b;
    }
    return 1;
}

/* Vectorize a loop whose trip count is only known at run time.
 *
 * The constant-count path above rewrites the loop in place, which it can
 * because the count is known to divide evenly. Here it does not, so the
 * loop is preceded by a VECTOR COPY of itself that runs over the whole
 * vectors, and the original is left to finish the remainder:
 *
 *       guard: if (0 >= n) goto done          <- already there
 *       nvec = n & ~(VF-1)                    <- n >= 1 by the guard
 *       vi = 0
 *       goto vcond
 *   vbody:  <the body, lane-wise, indexed by vi>
 *       vi += VF
 *   vcond:  if (vi < nvec) goto vbody
 *       <fold the reduction, if any>
 *       i = nvec
 *       if (i >= n) goto skip                 <- no remainder
 *       <the original loop, unchanged>
 *   skip:
 *
 * The guard means n >= 1 here, which is what makes `n & ~(VF-1)`
 * non-negative without a clamp -- for a negative n it would be negative,
 * the vector loop would be skipped, and the remainder would then start
 * at a negative index. The second test is needed because the first one
 * has already been passed: entering the original loop's body with
 * i == n would run it once too often.
 *
 * A reduction folds BETWEEN the two loops, so the remainder carries on
 * adding into an accumulator that already holds the vector part. */
static int vec_rewrite_runtime(struct ir_func *fn, struct defs *d,
                               struct vecloop *L, char *vec)
{
    int nv0 = fn->nvregs;
    int *map = xmalloc((size_t)nv0 * sizeof *map);
    for (int v = 0; v < nv0; v++)
        map[v] = -1;

    int vi = fn->nvregs++, vin = fn->nvregs++, nvec = fn->nvregs++;
    int kmask = fn->nvregs++, kzero = fn->nvregs++, kstep = fn->nvregs++;
    /* The guard's compare and the latch's compare are DIFFERENT values
     * of the same expression, one before the loop and one at the end of
     * each iteration, so they need different temps. Sharing one made it
     * doubly assigned, and every later pass that asks "where was this
     * defined" -- strength reduction among them -- gave up on the loop. */
    int tg = fn->nvregs++, tv = fn->nvregs++, t3 = fn->nvregs++;
    int vacc = -1, vzero = -1, redtmp = -1;
    if (L->red_acc >= 0) {
        vzero = fn->nvregs++; vacc = fn->nvregs++; redtmp = fn->nvregs++;
    }
    int Lvb = fn->nlabels++, Lvc = fn->nlabels++, Lskip = fn->nlabels++;
    int iw = fn->ins[L->cmp_ins].w, isign = fn->ins[L->cmp_ins].sign;

    map[L->iv] = vi;
    for (int n = L->lo; n < L->hi; n++) {
        if (n == L->step_ins || n == L->copy_ins || n == L->cmp_ins ||
            n == L->red_copy || n == L->red_add)
            continue;
        struct ir_ins *i = &fn->ins[n];
        if (i->op == IR_LABEL || i->op == IR_JMP ||
            i->op == IR_BRZ || i->op == IR_BRNZ)
            continue;
        int t = def_target(i);
        if (t >= 0 && t < nv0 && map[t] < 0)
            map[t] = fn->nvregs++;
    }

    struct ibuf nb = { 0, 0, 0 };
    int *newpos = fn->var_scope_lo
        ? xmalloc((size_t)(fn->nins + 1) * sizeof *newpos) : NULL;
#define EM(V) struct ir_ins *V = ib_push(&nb); \
              V->line = fn->ins[L->cmp_ins].line; \
              V->col = fn->ins[L->cmp_ins].col; V->synth = 1
    for (int n = 0; n < fn->nins; n++) {
        if (n == L->lo) {
            { EM(k); k->op = IR_CONST; k->dst = kzero; k->w = iw; }
            { EM(k); k->op = IR_CONST; k->dst = kstep; k->w = iw;
              k->imm = L->vf; }
            { EM(k); k->op = IR_CONST; k->dst = kmask; k->w = iw;
              k->imm = -(long)L->vf; }        /* ~(VF-1), VF a power of two */
            /* broadcasts of the constants the body adds lane-wise */
            for (int m = L->lo; m < L->hi; m++) {
                struct ir_ins *src = &fn->ins[m];
                int t = def_target(src);
                long kb;
                if (t < 0 || t >= nv0 || !vec[t] || m == L->red_add)
                    continue;
                if (src->op == IR_SHL || src->op == IR_SHR ||
                    src->op == IR_MUL || !const_b(fn, d, src, &kb))
                    continue;
                { EM(k); k->op = IR_CONST; k->dst = fn->nvregs;
                  k->w = L->esize; k->imm = kb; }
                { EM(sp); sp->op = IR_VSPLAT; sp->dst = fn->nvregs + 1;
                  sp->a = fn->nvregs; sp->size = L->esize; sp->w = 8; }
                src->c = fn->nvregs + 1;
                fn->nvregs += 2;
            }
            if (L->red_acc >= 0) {
                { EM(z); z->op = IR_CONST; z->dst = vzero; z->w = L->esize; }
                { EM(sp); sp->op = IR_VSPLAT; sp->dst = vacc; sp->a = vzero;
                  sp->size = L->esize; sp->w = 8; }
            }
            { EM(k); k->op = IR_AND; k->dst = nvec; k->a = L->bound_reg;
              k->b = kmask; k->w = iw; k->sign = isign; }
            { EM(k); k->op = IR_MOV; k->dst = vi; k->a = kzero; k->w = iw; }
            /* Guarded and bottom-tested, not top-tested with a jump in.
             * One branch an iteration rather than two, for the reason
             * rotation exists -- and, less obviously, it is what makes
             * this a NATURAL loop: entering at the test would mean the
             * header does not dominate the latch, and every later pass
             * that reasons about loops (strength reduction above all)
             * would decline to touch it. */
            { EM(k); k->op = IR_CMP; k->dst = tg; k->a = vi; k->b = nvec;
              k->w = iw; k->sign = isign; k->pred = B_LT; }
            { EM(k); k->op = IR_BRZ; k->a = tg; k->label = Lvc;
              k->w = fn->ins[L->hi - 1].w; }
            { EM(k); k->op = IR_LABEL; k->label = Lvb; }

            /* ---- the body, lane-wise, indexed by vi ---- */
            for (int m = L->lo; m < L->hi; m++) {
                struct ir_ins *src = &fn->ins[m];
                if (m == L->step_ins || m == L->copy_ins || m == L->cmp_ins ||
                    m == L->red_copy || src->op == IR_LABEL ||
                    src->op == IR_JMP || src->op == IR_BRZ ||
                    src->op == IR_BRNZ)
                    continue;
                int t = def_target(src);
                int nt = t >= 0 && t < nv0 && map[t] >= 0 ? map[t] : t;
                int ra = src->a >= 0 && src->a < nv0 && map[src->a] >= 0
                         ? map[src->a] : src->a;
                int rb = src->b >= 0 && src->b < nv0 && map[src->b] >= 0
                         ? map[src->b] : src->b;
                if (m == L->red_add) {
                    int rv = L->red_vec < nv0 && map[L->red_vec] >= 0
                             ? map[L->red_vec] : L->red_vec;
                    EM(v); v->op = IR_VBIN; v->dst = vacc; v->a = vacc;
                    v->b = rv; v->imm = '+'; v->size = L->esize; v->w = 8;
                    v->line = src->line; v->col = src->col; v->synth = 0;
                    continue;
                }
                if (src->op == IR_LOAD && t >= 0 && t < nv0 && vec[t]) {
                    EM(v); v->op = IR_VLOAD; v->dst = nt; v->a = ra;
                    v->size = L->esize; v->w = 8;
                    v->line = src->line; v->col = src->col; v->synth = 0;
                    continue;
                }
                if (src->op == IR_STORE) {
                    EM(v); v->op = IR_VSTORE; v->dst = -1; v->a = ra;
                    v->b = rb; v->size = L->esize; v->w = 8;
                    v->line = src->line; v->col = src->col; v->synth = 0;
                    continue;
                }
                if (t >= 0 && t < nv0 && vec[t]) {
                    long kb = 0;
                    int hask = const_b(fn, d, src, &kb);
                    if (src->op == IR_MUL) {
                        int sh = 0;
                        long mlt = kb;
                        int p2 = (mlt & (mlt - 1)) == 0;
                        while ((1L << sh) < (p2 ? mlt : mlt - 1)) sh++;
                        if (p2) {
                            EM(v); v->op = IR_VBIN; v->dst = nt; v->a = ra;
                            v->b = -1; v->imm = '<'; v->c = sh;
                            v->size = L->esize; v->w = 8;
                            v->line = src->line; v->col = src->col;
                            v->synth = 0;
                        } else {
                            int tmp = fn->nvregs++;
                            { EM(v); v->op = IR_VBIN; v->dst = tmp; v->a = ra;
                              v->b = -1; v->imm = '<'; v->c = sh;
                              v->size = L->esize; v->w = 8;
                              v->line = src->line; v->col = src->col;
                              v->synth = 0; }
                            { EM(w2); w2->op = IR_VBIN; w2->dst = nt;
                              w2->a = tmp; w2->b = ra; w2->imm = '+';
                              w2->size = L->esize; w2->w = 8;
                              w2->line = src->line; w2->col = src->col;
                              w2->synth = 0; }
                        }
                        continue;
                    }
                    EM(v);
                    if (!vec_build(v, src, nt, ra, rb, src->c, kb, hask,
                                   L->esize)) {
                        free(map); free(nb.p); free(newpos);
                        return 0;       /* the recognizer let one through */
                    }
                    continue;
                }
                /* the address chain and anything else scalar: copied
                 * with its operands pointed at the vector index */
                EM(v); *v = *src; v->synth = 0;
                struct vremap r = { map, nv0 };
                each_read(v, vremap_cb, &r);
                if (t >= 0)
                    v->dst = nt;
            }
            { EM(k); k->op = IR_ADD; k->dst = vin; k->a = vi; k->b = kstep;
              k->w = iw; k->sign = isign; }
            { EM(k); k->op = IR_MOV; k->dst = vi; k->a = vin; k->w = iw; }
            { EM(k); k->op = IR_CMP; k->dst = tv; k->a = vi; k->b = nvec;
              k->w = iw; k->sign = isign; k->pred = B_LT; }
            { EM(k); k->op = IR_BRNZ; k->a = tv; k->label = Lvb;
              k->w = fn->ins[L->hi - 1].w; }
            { EM(k); k->op = IR_LABEL; k->label = Lvc; }   /* past the vectors */
            if (L->red_acc >= 0) {
                { EM(r); r->op = IR_VREDADD; r->dst = redtmp; r->a = vacc;
                  r->size = L->esize; r->w = fn->ins[L->red_add].w; }
                { EM(a2); a2->op = IR_ADD; a2->dst = L->red_acc;
                  a2->a = L->red_acc; a2->b = redtmp;
                  a2->w = fn->ins[L->red_add].w;
                  a2->sign = fn->ins[L->red_add].sign; }
            }
            { EM(k); k->op = IR_MOV; k->dst = L->iv; k->a = nvec; k->w = iw; }
            { EM(k); k->op = IR_CMP; k->dst = t3; k->a = L->iv;
              k->b = L->bound_reg; k->w = iw; k->sign = isign;
              k->pred = B_LT; }
            { EM(k); k->op = IR_BRZ; k->a = t3; k->label = Lskip;
              k->w = fn->ins[L->hi - 1].w; }
        }
        if (newpos) newpos[n] = nb.n;
        *ib_push(&nb) = fn->ins[n];         /* the scalar loop, unchanged */
        if (n == L->hi - 1) {
            EM(k); k->op = IR_LABEL; k->label = Lskip;
        }
    }
#undef EM
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
    free(map);
    return 1;
}

/* Is `i` (defining t) a reduction's add -- `next = acc + v`, with a phi
 * copy `acc = mov next` inside the loop closing the circle? */
static int vec_self_accum(struct ir_func *fn, struct defs *d, int lo, int hi,
                          const struct ir_ins *i, int t)
{
    if (i->op != IR_ADD || t < 0 || t >= fn->nvregs)
        return 0;
    for (int n = lo; n < hi; n++) {
        const struct ir_ins *m = &fn->ins[n];
        if (m->op != IR_MOV || m->a != t)
            continue;
        if (m->dst < 0 || m->dst >= fn->nvregs || d->cnt[m->dst] != 2)
            continue;
        if (m->dst == i->a || m->dst == i->b)
            return 1;
    }
    return 0;
}

static int vectorize_one(struct ir_func *fn)
{
    if (fn->nins == 0)
        return 0;
    VDBG("considering %s\n", fn->name);
    int nbb, *l2b;
    struct bb *bb = build_cfg(fn, &nbb, &l2b);
    int *order = xmalloc((size_t)nbb * sizeof *order), norder;
    compute_rpo(bb, nbb, order, &norder);
    if (norder != nbb) {
        free(order); free(l2b);
        free_cfg(bb, nbb); return 0;
    }
    compute_idom(bb, order, norder);
    struct defs d;
    compute_defs(fn, &d);
    char *in = xmalloc((size_t)nbb);
    int done = 0;

    for (int oi = norder - 1; oi >= 0 && !done; oi--) {
        int h = order[oi];
        if (h == 0 || bb[h].end <= bb[h].start ||
            fn->ins[bb[h].start].op != IR_LABEL)
            continue;
        int Lh = fn->ins[bb[h].start].label;

        /* One back edge, from a latch that branches to the header --
         * the rotated shape, which is the only one whose test is at the
         * bottom where the step is. */
        int latch = -1, nback = 0;
        for (int q = 0; q < bb[h].npred; q++) {   /* back edges into h */
            int p = bb[h].pred[q];
            if (bb_dominates(bb, h, p)) {
                latch = p; nback++;
            }
        }
        if (nback != 1 || bb[latch].end <= bb[latch].start)
            continue;
        struct ir_ins *br = &fn->ins[bb[latch].end - 1];
        if (br->op != IR_BRNZ || br->label != Lh)
            { VDBG("h=%d not a rotated latch\n", h); continue; }

        memset(in, 0, (size_t)nbb);
        loop_body(bb, nbb, h, latch, in);
        /* The loop must be one contiguous run of instructions, so the
         * body can be read and rewritten as a straight line. */
        struct vecloop L;
        L.lo = bb[h].start; L.hi = bb[latch].end; L.header = h;
        int ok = 1;
        for (int b = 0; b < nbb && ok; b++)
            if (in[b] != (bb[b].start >= L.lo && bb[b].end <= L.hi))
                ok = 0;
        if (!ok)
            { VDBG("h=%d loop not contiguous\n", h); continue; }

        /* The test: `c = cmp lt iv, #bound`, feeding the back branch. */
        int cn = (br->a >= 0 && br->a < fn->nvregs && d.cnt[br->a] == 1)
                 ? d.ins[br->a] : -1;
        if (cn < L.lo || cn >= L.hi)
            continue;
        struct ir_ins *cmp = &fn->ins[cn];
        if (cmp->op != IR_CMP || cmp->pred != B_LT || !cmp->sign)
            { VDBG("h=%d test is not `cmp lt iv, ...`\n", h); continue; }
        L.cmp_ins = cn;
        L.iv = cmp->a;
        L.bound = 0; L.bound_reg = -1;
        if (!const_b(fn, &d, cmp, &L.bound)) {
            /* A runtime trip count. The bound itself must not change
             * inside the loop, or "how many whole vectors fit" is not a
             * question with one answer. */
            L.bound_reg = cmp->b;
            if (!defined_outside(fn, &d, L.bound_reg, L.lo, L.hi))
                { VDBG("h=%d the bound is not invariant\n", h); continue; }
        } else if (L.bound <= 0) {
            VDBG("h=%d bad bound\n", h); continue;
        }
        if (L.iv < 0 || L.iv >= fn->nvregs)
            { VDBG("h=%d bad iv\n", h); continue; }

        /* `iv = mov next` and `next = iv + 1`, both in the loop. */
        L.copy_ins = L.step_ins = -1;
        for (int n = L.lo; n < L.hi; n++) {
            struct ir_ins *i = &fn->ins[n];
            if (i->op == IR_MOV && i->dst == L.iv)
                L.copy_ins = n;
        }
        if (L.copy_ins < 0)
            { VDBG("h=%d no phi copy for iv %d\n", h, L.iv); continue; }
        int next = fn->ins[L.copy_ins].a;
        if (next < 0 || next >= fn->nvregs || d.cnt[next] != 1)
            continue;
        L.step_ins = d.ins[next];
        if (L.step_ins < L.lo || L.step_ins >= L.hi)
            { VDBG("h=%d step outside loop\n", h); continue; }
        struct ir_ins *st = &fn->ins[L.step_ins];
        long stepk;
        if (st->op != IR_ADD || st->a != L.iv ||
            !const_b(fn, &d, st, &stepk) || stepk != 1)
            { VDBG("h=%d step is not +1\n", h); continue; }

        /* It has to start at zero, or lane k is not element i+k. The
         * other definition of the phi temp is the one before the loop. */
        int init_ok = 0;
        for (int n = 0; n < fn->nins; n++) {
            if (n >= L.lo && n < L.hi)
                continue;
            struct ir_ins *i = &fn->ins[n];
            if (def_target(i) != L.iv)
                continue;
            if (i->op == IR_MOV && i->a >= 0 && i->a < fn->nvregs &&
                d.cnt[i->a] == 1 && d.ins[i->a] >= 0 &&
                fn->ins[d.ins[i->a]].op == IR_CONST &&
                fn->ins[d.ins[i->a]].imm == 0)
                init_ok = 1;
            else if (i->op == IR_CONST && i->imm == 0)
                init_ok = 1;
            else
                { init_ok = 0; break; }
        }
        if (!init_ok)
            { VDBG("h=%d iv does not start at 0\n", h); continue; }

        /* ---- classify the body -------------------------------------
         *
         * vec[v] marks a temp that becomes a vector. Loads seed it and
         * arithmetic spreads it; anything else reading a vector temp,
         * or any op that is not lane-wise, refuses the loop. */
        char *vec = xcalloc((size_t)fn->nvregs, 1);
        L.esize = 0;
        int nmem = 0, nbase = 0, bases[8];
        for (int n = L.lo; n < L.hi && ok; n++) {
            struct ir_ins *i = &fn->ins[n];
            if (i->op != IR_LOAD && i->op != IR_STORE)
                continue;
            if (i->vol || (i->size != 4 && i->size != 8)) { ok = 0; break; }
            if (L.esize && i->size != L.esize) { ok = 0; break; }
            L.esize = i->size;
            int base = addr_of_iv(fn, &d, &L, i->a, i->size);
            if (base < 0) { ok = 0; break; }
            int seen = 0;
            for (int k = 0; k < nbase; k++)
                if (bases[k] == base) seen = 1;
            if (!seen) {
                if (nbase == (int)(sizeof bases / sizeof bases[0]))
                    { ok = 0; break; }
                bases[nbase++] = base;
            }
            if (i->op == IR_LOAD)
                vec[i->dst] = 1;
            nmem++;
        }
        /* ---- can the bases alias each other? ------------------------
         *
         * Two distinct globals cannot overlap, so any number of those is
         * fine. A pointer could point anywhere, so more than one of them
         * -- or one of them beside a global -- would need a run-time
         * check that the ranges are disjoint, which is not built. But
         * ONE base, whatever it is, cannot alias itself: every reference
         * is then the same address at the same index, so lane k is
         * element i+k of the one array, read and written by the same
         * lane. That single case is most of what real code does
         * (`for (i...) p[i] = p[i] * 2`), and it is safe with no check
         * at all. */
        if (ok && nbase > 0) {
            int all_global = 1;
            for (int k = 0; k < nbase; k++) {
                int bd = d.ins[bases[k]];
                if (bd < 0 || fn->ins[bd].op != IR_GADDR)
                    all_global = 0;
            }
            if (!all_global && nbase > 1) {
                VDBG("h=%d %d bases, not all globals: they may alias\n",
                     h, nbase);
                ok = 0;
            }
        }
        if (!ok || !L.esize || nmem == 0) {
            VDBG("h=%d memory refs: ok=%d esize=%d n=%d\n", h, ok, L.esize, nmem);
            free(vec); continue; }
        L.vf = VEC_BYTES / L.esize;
        if (L.bound_reg < 0 && L.bound % L.vf != 0) {
            VDBG("h=%d trip %ld not a multiple of %d\n", h, L.bound, L.vf);
            free(vec); continue; }

        /* Spread vectorness to a fixpoint, and refuse anything that
         * cannot carry it. */
        for (int again = 1; again && ok; ) {
            again = 0;
            for (int n = L.lo; n < L.hi && ok; n++) {
                struct ir_ins *i = &fn->ins[n];
                if (i->op == IR_LOAD || i->op == IR_STORE ||
                    n == L.red_add || n == L.red_copy ||
                    n == L.step_ins || n == L.copy_ins || n == L.cmp_ins ||
                    i->op == IR_LABEL || i->op == IR_JMP ||
                    i->op == IR_BRNZ || i->op == IR_BRZ)
                    continue;
                /* A move into a multiply-assigned temp is a phi copy --
                 * mem2reg's way of carrying a value round the loop. It
                 * has no lane-wise form and refusing it here would
                 * refuse every reduction before the detector below has
                 * had a chance to claim it. Left alone: if the detector
                 * does not claim it, the escape check refuses it, which
                 * is the same answer arrived at properly. */
                if (i->op == IR_MOV && i->dst >= 0 && i->dst < fn->nvregs &&
                    d.cnt[i->dst] == 2)
                    continue;
                /* Likewise a widening of a vector: it has no single
                 * lane-wise form -- four narrow lanes become two wide
                 * ones, so one of these is TWO vectors -- and the
                 * reduction detector is what knows whether that is
                 * wanted. Refusing it here refused every `long s +=
                 * a[i]` over an int array before anything looked. */
                if (i->op == IR_EXT)
                    continue;
                /* And a self-accumulating add -- `next = acc + v` with
                 * `acc = mov next` closing the circle -- is a REDUCTION,
                 * which the detector below is what judges. It has no
                 * lane-wise form of its own: the accumulator is a scalar
                 * carried round the loop, not sixteen bytes.
                 *
                 * Which side the accumulator lands on is not fixed.
                 * `s += v` and `s = v + s` are one expression, and the
                 * canonical operand order may put either first -- so
                 * this asks about the SHAPE rather than about operand b,
                 * which is what the test below used to stand in for and
                 * what stopped working the day the order was made
                 * canonical. */
                if (vec_self_accum(fn, &d, L.lo, L.hi, i, def_target(i)))
                    continue;
                struct opnds o;
                value_opnds(i, &o);
                int any = 0;
                for (int k = 0; k < o.n; k++)
                    if (o.v[k] >= 0 && o.v[k] < fn->nvregs && vec[o.v[k]])
                        any = 1;
                if (!any)
                    continue;               /* wholly scalar: it may stay */
                int t = def_target(i);
                /* Floating-point arithmetic has no lane form here: the
                 * lane operations are INTEGER ones (paddd, psll...), and
                 * `a[i] = b[i] + c[i]` over floats was added as integers
                 * -- wrong answers at -O2 on x86-64. The reduction below
                 * already refused a float sum; a copy, being only loads
                 * and stores, is still vectorized. */
                if (i->flt) { ok = 0; break; }
                /* A multiply by a constant becomes shifts and adds; any
                 * other multiply, and anything not lane-wise, stops us. */
                int c = vec_op_char(i->op);
                long kb;
                int hask = const_b(fn, &d, i, &kb);
                if (i->op == IR_MUL) {
                    /* only powers of two and (2^k)+1, which is one shift
                     * and one add -- the rest is not worth a chain, and
                     * a packed 32-bit multiply is not SSE2 anyway */
                    if (!hask || kb <= 0) { ok = 0; break; }
                    int p2 = (kb & (kb - 1)) == 0;
                    int p2p1 = ((kb - 1) & (kb - 2)) == 0 && kb >= 3;
                    if (!p2 && !p2p1) { ok = 0; break; }
                } else if (!c) {
                    ok = 0; break;
                } else if ((i->op == IR_SHL || i->op == IR_SHR) && !hask) {
                    ok = 0; break;          /* a per-lane shift count */
                } else if (i->op == IR_SHR && i->sign && L.esize == 8) {
                    /* SSE2 has no psraq. Refused HERE rather than at
                     * emission: the backend's refusal is a loud internal
                     * error, and a loop we simply do not vectorize is
                     * not an error at all. */
                    ok = 0; break;
                } else if (!hask && i->b >= 0 && i->b < fn->nvregs &&
                           !vec[i->b] &&
                           !defined_outside(fn, &d, i->b, L.lo, L.hi)) {
                    ok = 0; break;          /* a scalar that varies per lane */
                }
                if (t < 0 || t >= fn->nvregs) { ok = 0; break; }
                if (i->w != L.esize && i->op != IR_SHL && i->op != IR_SHR)
                    { ok = 0; break; }      /* a widening op changes lanes */
                if (!vec[t]) { vec[t] = 1; again = 1; }
            }
        }
        if (!ok) { VDBG("h=%d body not lane-wise\n", h); free(vec); continue; }

        /* ---- a sum reduction ----------------------------------------
         *
         * `s += a[i]` carries a value ACROSS iterations, which nothing
         * else here is allowed to do. It is vectorizable anyway because
         * addition is associative: keep VF running sums, one per lane,
         * and add them together once the loop is done. The lanes are
         * summed in a different order than the source says, which for
         * integers changes nothing (it would for floats, which is why
         * this is integer-only and why gcc needs -ffast-math for them).
         *
         * The shape is the induction variable's, one value over: a phi
         * temp copied at the latch from something the body computed.
         *
         * This runs AFTER the vectorness fixpoint, not before: the value
         * being added is often computed rather than loaded -- `s += a[i]
         * * 2` adds a shift of a load -- and before the fixpoint only a
         * bare load is marked, so a reduction over anything at all was
         * missed and the whole loop refused as "not lane-wise". */
        L.red_acc = L.red_next = L.red_add = L.red_copy = L.red_vec = -1;
        L.red_ext = -1;
        for (int n = L.lo; n < L.hi && ok; n++) {
            struct ir_ins *i = &fn->ins[n];
            if (i->op != IR_MOV || i->dst == L.iv || i->dst < 0)
                continue;
            int acc = i->dst, nxt = i->a;
            /* Not `vec[nxt]`: in a WIDENING sum neither the extension
             * nor the add is marked (the fixpoint skips the extension
             * on purpose), so the condition that matters is on the
             * operand being summed, checked below. */
            if (nxt < 0 || nxt >= fn->nvregs || d.cnt[nxt] != 1)
                continue;
            int an = d.ins[nxt];
            if (an < L.lo || an >= L.hi)
                continue;
            struct ir_ins *ad = &fn->ins[an];
            long junk;
            if (ad->op != IR_ADD || const_b(fn, &d, ad, &junk))
                continue;
            int other = ad->a == acc ? ad->b : ad->b == acc ? ad->a : -1;
            if (other < 0 || other >= fn->nvregs)
                continue;
            int widen_ext = -1, rvec = other;
            if (ad->flt)
                continue;
            if (ad->w == L.esize) {
                if (!vec[other])
                    continue;           /* summing something that is not a
                                         * vector at all */
            } else {
                /* A widening sum: `long s += a[i]` over an int array.
                 * The operand is an EXT of the loaded vector, and the
                 * accumulator is twice the element width. */
                if (ad->w != 2 * L.esize || L.esize != 4)
                    continue;
                if (other < 0 || other >= fn->nvregs || d.cnt[other] != 1)
                    continue;
                int en = d.ins[other];
                if (en < L.lo || en >= L.hi || fn->ins[en].op != IR_EXT)
                    continue;
                if (fn->ins[en].size != L.esize || fn->ins[en].w != ad->w)
                    continue;
                int src = fn->ins[en].a;
                if (src < 0 || src >= fn->nvregs || !vec[src])
                    continue;
                widen_ext = en; rvec = src;
            }
            if (L.red_acc >= 0) { ok = 0; break; }   /* one is enough */
            L.red_acc = acc; L.red_next = nxt; L.red_add = an;
            L.red_copy = n;  L.red_vec = rvec; L.red_ext = widen_ext;
            /* The running total is NOT a vector value -- it is a scalar
             * carried across iterations that happens to be computed
             * from one. Unmarking it is what stops the escape check
             * below from refusing the phi copy that carries it. */
            vec[nxt] = 0;
        }
        if (!ok) { VDBG("h=%d more than one reduction\n", h); free(vec); continue; }
        if (L.red_acc >= 0) {
            /* The accumulator may be read after the loop -- that is the
             * point of it -- but inside, only by its own sum. Anything
             * else reading a partial total would see one lane's share. */
            for (int n = L.lo; n < L.hi && ok; n++) {
                if (n == L.red_add || n == L.red_copy)
                    continue;
                if (ins_reads(&fn->ins[n], L.red_acc) ||
                    ins_reads(&fn->ins[n], L.red_next))
                    ok = 0;
            }
            if (L.red_ext >= 0) {
                int w = def_target(&fn->ins[L.red_ext]);
                for (int n = 0; n < fn->nins && ok; n++)
                    if (n != L.red_add && ins_reads(&fn->ins[n], w))
                        ok = 0;     /* the widened value feeds only the sum */
            }
            for (int n = 0; n < fn->nins && ok; n++)
                if ((n < L.lo || n >= L.hi) && ins_reads(&fn->ins[n], L.red_next))
                    ok = 0;             /* the partial sum must not escape */
            if (!ok) {
                VDBG("h=%d the accumulator is read elsewhere\n", h);
                free(vec); continue;
            }
        }

        /* Every store's VALUE must be a vector too.
         *
         * Without this, `a[i] = i` became a vstore of the induction
         * variable's eight-byte slot read as sixteen, and the array
         * filled with garbage -- the loop after it then computed
         * 0 * 3 + 1 for every element and the answer was 64 where it
         * should have been 6112. A scalar stored here is one of two
         * things and only one of them is safe: a loop-invariant value,
         * which every lane could share, or something that varies with i
         * like the index itself, which no single lane value can stand
         * for. Telling them apart is worth doing; until it is, neither
         * is vectorized. */
        for (int n = L.lo; n < L.hi && ok; n++) {
            struct ir_ins *i = &fn->ins[n];
            if (i->op != IR_STORE)
                continue;
            if (i->b < 0 || i->b >= fn->nvregs || !vec[i->b])
                ok = 0;
        }
        if (!ok) {
            VDBG("h=%d a store's value is not lane-wise\n", h);
            free(vec); continue;
        }
        /* ---- nothing may happen a quarter as often ------------------
         *
         * The vector loop runs the body once per VF elements, so ANY
         * instruction in it that is not part of the lane-wise work
         * happens a quarter as many times. A call is the obvious one --
         * `for (...) { a[i]++; g(); }` vectorized and called g() 256
         * times instead of 1024, which is a wrong answer with nothing
         * wrong-looking about it -- but so is a volatile access, a
         * fence, an atomic, or a store to anything but the arrays being
         * walked.
         *
         * The rule is therefore positive rather than a list of banned
         * opcodes: every instruction here is either the loop's own
         * control, one of the memory references already checked to be
         * base + i*esize, the reduction, or something PURE. is_pure is
         * exactly "no side effect and cannot fault", which is the
         * property needed -- a pure value computed a quarter as often
         * is only a wasted computation, not a changed program. */
        for (int n = L.lo; n < L.hi && ok; n++) {
            struct ir_ins *i = &fn->ins[n];
            if (n == L.step_ins || n == L.copy_ins || n == L.cmp_ins ||
                n == L.red_add || n == L.red_copy || n == L.hi - 1)
                continue;
            if (i->op == IR_LABEL || i->op == IR_LOAD || i->op == IR_STORE)
                continue;               /* the references, already checked */
            if (!is_pure(i->op) || i->vol)
                ok = 0;
        }
        if (!ok) {
            VDBG("h=%d the body does something a quarter as often\n", h);
            free(vec); continue;
        }

        /* ---- and nothing may carry a value out of it ----------------
         *
         * A pure computation is safe to run fewer times only if nobody
         * reads the result afterwards. `m = i` inside the loop leaves m
         * at the last VECTOR index, not the last one -- and in the
         * runtime form, where the remainder loop may not run at all, it
         * leaves the original temp never written. The induction
         * variable is the exception: it ends at the trip count either
         * way. The accumulator is the other, and it is folded by hand. */
        for (int n = L.lo; n < L.hi && ok; n++) {
            int t = def_target(&fn->ins[n]);
            if (t < 0 || t >= fn->nvregs || t == L.iv || t == L.red_acc)
                continue;
            for (int m = 0; m < fn->nins && ok; m++)
                if ((m < L.lo || m >= L.hi) && ins_reads(&fn->ins[m], t)) {
                    VDBG("h=%d temp %%%d (def at %d) read at %d; iv=%%%d"
                         " acc=%%%d\n", h, t, n, m, L.iv, L.red_acc);
                    ok = 0;
                }
        }
        if (!ok) {
            VDBG("h=%d a value escapes the loop\n", h);
            free(vec); continue;
        }

        /* A value that becomes a vector may only be read by something
         * that also becomes one: a lane-wise op inside the loop, or the
         * store that consumes it. Anything else -- a use after the loop,
         * a scalar op inside it -- would read sixteen bytes as eight. */
        for (int n = 0; n < fn->nins && ok; n++) {
            struct ir_ins *i = &fn->ins[n];
            int inside = n >= L.lo && n < L.hi;
            int t = def_target(i);
            int consumer = inside && (i->op == IR_STORE || n == L.red_add ||
                                      n == L.red_ext ||
                                      (t >= 0 && t < fn->nvregs && vec[t]));
            if (consumer)
                continue;
            for (int v = 0; v < fn->nvregs && ok; v++)
                if (vec[v] && ins_reads(i, v))
                    ok = 0;
        }
        if (!ok) { VDBG("h=%d a vector value escapes\n", h); free(vec); continue; }

        /* ---- rewrite ------------------------------------------------ */
        /* A frame slot written in the body -- a local kept in memory
         * because its address is taken -- is one place written on every
         * iteration. Neither rewrite keeps that: the copying one renames
         * every definition, slots included, and the in-place one would
         * write it once per vector of iterations. */
        {
            int slot_write = 0;
            for (int q = L.lo; q < L.hi; q++)
                slot_write |= fn->ins[q].op == IR_STVAR;
            if (slot_write) {
                VDBG("h=%d writes a local kept in memory\n", h);
                free(vec); continue;
            }
        }
        if (L.bound_reg >= 0 && L.red_ext >= 0) {
            VDBG("h=%d a widening sum with a runtime count\n", h);
            free(vec); continue;        /* the copy path does not widen */
        }
        if (L.bound_reg >= 0) {
            /* A runtime count needs a remainder, so the loop is copied
             * rather than rewritten in place. */
            if (!vec_rewrite_runtime(fn, &d, &L, vec)) {
                VDBG("h=%d the runtime rewrite refused\n", h);
                free(vec); continue;
            }
            free(vec);
            if (remarks_on() && fn->src)
                remark_add("opt", "vectorized", fn->name,
                           "vec/runtime-trip-count", fn->file, fn->line,
                           "%d lanes of %d bytes, with a scalar remainder%s",
                           L.vf, L.esize,
                           L.red_acc >= 0 ? " (a sum reduction)" : "");
            done = 1;
            continue;
        }
        struct ibuf nb = { 0, 0, 0 };
        int *newpos = fn->var_scope_lo
            ? xmalloc((size_t)(fn->nins + 1) * sizeof *newpos) : NULL;
        int vfk = fn->nvregs++;      /* the new induction step, VF */
        int vacc = -1, vzero = -1, redtmp = -1, vacc2 = -1, wlo = -1, whi = -1;
        int wsize = L.esize;            /* the accumulator's lane width */
        if (L.red_acc >= 0) {
            vzero = fn->nvregs++; vacc = fn->nvregs++; redtmp = fn->nvregs++;
            if (L.red_ext >= 0) {
                /* A widening sum needs TWO accumulators: one load's four
                 * narrow lanes become two wide ones twice over. */
                vacc2 = fn->nvregs++; wlo = fn->nvregs++; whi = fn->nvregs++;
                wsize = L.esize * 2;
            }
        }
        for (int n = 0; n < fn->nins; n++) {
            if (n == L.lo) {
                struct ir_ins *k = ib_push(&nb);
                k->op = IR_CONST; k->dst = vfk;
                k->w = fn->ins[L.step_ins].w; k->imm = L.vf;
                k->line = fn->ins[L.step_ins].line;
                k->col = fn->ins[L.step_ins].col;
                if (L.red_acc >= 0) {
                    /* Every lane starts at zero, HERE and not before the
                     * guard: the preheader runs exactly when the loop is
                     * entered, so a loop that runs zero times leaves the
                     * scalar accumulator alone -- and an inner loop is
                     * re-zeroed on every pass of the outer one. */
                    /* (the location by value: each ib_push may move the
                     * buffer the previous one returned a pointer into) */
                    int zl = fn->ins[L.red_add].line, zc = fn->ins[L.red_add].col;
                    struct ir_ins *z = ib_push(&nb);
                    z->op = IR_CONST; z->dst = vzero;
                    z->w = wsize; z->imm = 0;
                    z->line = zl;
                    z->col = zc;
                    struct ir_ins *sp = ib_push(&nb);
                    sp->op = IR_VSPLAT; sp->dst = vacc; sp->a = vzero;
                    sp->size = wsize; sp->w = 8;
                    sp->line = zl; sp->col = zc;
                    if (vacc2 >= 0) {
                        struct ir_ins *s2 = ib_push(&nb);
                        s2->op = IR_VSPLAT; s2->dst = vacc2; s2->a = vzero;
                        s2->size = wsize; s2->w = 8;
                        s2->line = zl; s2->col = zc;
                    }
                }
                /* Before the header label: broadcast every constant the
                 * body needs, once. The back edge jumps past this, the
                 * same way it jumps past a hoisted expression. */
                for (int m = L.lo; m < L.hi; m++) {
                    struct ir_ins *s = &fn->ins[m];
                    int t = def_target(s);
                    long kb;
                    if (t < 0 || t >= fn->nvregs || !vec[t])
                        continue;
                    if (s->op == IR_SHL || s->op == IR_SHR || s->op == IR_MUL)
                        continue;           /* folded into the lane op */
                    if (!const_b(fn, &d, s, &kb))
                        continue;           /* both operands are vectors */
                    struct ir_ins *k = ib_push(&nb);
                    k->op = IR_CONST; k->dst = fn->nvregs;
                    k->w = L.esize; k->imm = kb;
                    k->line = s->line; k->col = s->col;
                    struct ir_ins *sp = ib_push(&nb);
                    sp->op = IR_VSPLAT; sp->dst = fn->nvregs + 1;
                    sp->a = fn->nvregs; sp->size = L.esize;
                    sp->line = s->line; sp->col = s->col;
                    s->c = fn->nvregs + 1;  /* remember the splat for below */
                    fn->nvregs += 2;
                }
            }
            if (newpos) newpos[n] = nb.n;
            struct ir_ins *i = &fn->ins[n];
            if (n < L.lo || n >= L.hi) { *ib_push(&nb) = *i; continue; }

            if (n == L.red_copy)
                continue;               /* the scalar carry is gone */
            if (n == L.red_ext)
                continue;   /* emitted with the add, so each half goes
                             * straight from xmm0 into its accumulator
                             * instead of out to a slot and back */
            if (n == L.red_add) {       /* vacc += the loaded vector */
                if (vacc2 >= 0) {
                    /* widen a half, add it, then the other: each value
                     * is consumed by the instruction after it. */
                    for (int half = 0; half < 2; half++) {
                        struct ir_ins *w2 = ib_push(&nb);
                        memset(w2, 0, sizeof *w2);
                        w2->op = IR_VWIDEN; w2->dst = half ? whi : wlo;
                        w2->a = L.red_vec; w2->c = half;
                        w2->size = L.esize;
                        w2->sign = fn->ins[L.red_ext].sign; w2->w = 8;
                        w2->line = i->line; w2->col = i->col;
                        struct ir_ins *v2 = ib_push(&nb);
                        memset(v2, 0, sizeof *v2);
                        v2->op = IR_VBIN;
                        v2->dst = half ? vacc2 : vacc;
                        v2->a = half ? vacc2 : vacc;
                        v2->b = half ? whi : wlo;
                        v2->imm = '+'; v2->size = wsize; v2->w = 8;
                        v2->line = i->line; v2->col = i->col;
                    }
                    continue;
                }
                struct ir_ins *v = ib_push(&nb);
                memset(v, 0, sizeof *v);
                v->op = IR_VBIN; v->dst = vacc; v->a = vacc;
                v->b = L.red_vec;
                v->imm = '+'; v->size = wsize; v->w = 8;
                v->line = i->line; v->col = i->col;
                continue;
            }
            if (n == L.step_ins) {          /* i += VF, not i += 1 */
                struct ir_ins *s = ib_push(&nb);
                *s = *i;
                /* Through a CONST temp, not imm_b: pass_immfold runs
                 * after this and the passes before it are documented
                 * never to reason about the folded form. */
                s->imm_b = 0; s->imm = 0; s->b = vfk;
                continue;
            }
            int t = def_target(i);
            if (i->op == IR_LOAD && t >= 0 && vec[t]) {
                struct ir_ins *v = ib_push(&nb);
                memset(v, 0, sizeof *v);
                v->op = IR_VLOAD; v->dst = t; v->a = i->a;
                v->size = L.esize; v->w = 8;
                v->line = i->line; v->col = i->col;
                continue;
            }
            if (i->op == IR_STORE) {
                struct ir_ins *v = ib_push(&nb);
                memset(v, 0, sizeof *v);
                v->op = IR_VSTORE; v->a = i->a; v->b = i->b; v->dst = -1;
                v->size = L.esize; v->w = 8;
                v->line = i->line; v->col = i->col;
                continue;
            }
            if (t >= 0 && t < fn->nvregs && vec[t]) {
                int c = vec_op_char(i->op);
                if (i->op == IR_MUL) {
                    long m = 0;
                    (void)const_b(fn, &d, i, &m);
                    if ((m & (m - 1)) == 0) {          /* a power of two */
                        int sh = 0;
                        while ((1L << sh) < m) sh++;
                        struct ir_ins *v = ib_push(&nb);
                        memset(v, 0, sizeof *v);
                        v->op = IR_VBIN; v->dst = t; v->a = i->a;
                        v->b = -1; v->imm = '<'; v->c = sh;
                        v->size = L.esize; v->w = 8;
                        v->line = i->line; v->col = i->col;
                    } else {                            /* (1 << k) + 1 */
                        int sh = 0;
                        while ((1L << sh) < m - 1) sh++;
                        int tmp = fn->nvregs++;
                        struct ir_ins *v = ib_push(&nb);
                        memset(v, 0, sizeof *v);
                        v->op = IR_VBIN; v->dst = tmp; v->a = i->a;
                        v->b = -1; v->imm = '<'; v->c = sh;
                        v->size = L.esize; v->w = 8;
                        v->line = i->line; v->col = i->col;
                        struct ir_ins *w = ib_push(&nb);
                        memset(w, 0, sizeof *w);
                        w->op = IR_VBIN; w->dst = t; w->a = tmp; w->b = i->a;
                        w->imm = '+'; w->size = L.esize; w->w = 8;
                        w->line = i->line; w->col = i->col;
                    }
                    continue;
                }
                struct ir_ins *v = ib_push(&nb);
                memset(v, 0, sizeof *v);
                v->op = IR_VBIN; v->dst = t; v->a = i->a;
                v->size = L.esize; v->w = 8;
                v->imm = c;
                v->line = i->line; v->col = i->col;
                long kb;
                int hask = const_b(fn, &d, i, &kb);
                if ((c == '<' || c == '>') && hask) {
                    v->b = -1; v->c = (int)kb; v->sign = i->sign;
                } else if (hask) {
                    v->b = i->c;              /* the splat made above */
                } else {
                    v->b = i->b;
                }
                continue;
            }
            *ib_push(&nb) = *i;
            if (L.red_acc >= 0 && n == L.hi - 1) {
                /* Right AFTER the back branch and before whatever label
                 * follows, so it runs on the way out of the loop and is
                 * skipped by the guard's jump straight to the exit --
                 * the preheader trick, mirrored. */
                if (vacc2 >= 0) {       /* the two halves, added lane-wise */
                    struct ir_ins *c2 = ib_push(&nb);
                    memset(c2, 0, sizeof *c2);
                    c2->op = IR_VBIN; c2->dst = vacc; c2->a = vacc;
                    c2->b = vacc2; c2->imm = '+'; c2->size = wsize; c2->w = 8;
                    c2->line = fn->ins[L.red_add].line;
                    c2->col = fn->ins[L.red_add].col;
                }
                struct ir_ins *r = ib_push(&nb);
                memset(r, 0, sizeof *r);
                r->op = IR_VREDADD; r->dst = redtmp; r->a = vacc;
                r->size = wsize; r->w = fn->ins[L.red_add].w;
                r->line = fn->ins[L.red_add].line;
                r->col = fn->ins[L.red_add].col;
                struct ir_ins *ad = ib_push(&nb);
                memset(ad, 0, sizeof *ad);
                ad->op = IR_ADD; ad->dst = L.red_acc; ad->a = L.red_acc;
                ad->b = redtmp; ad->w = fn->ins[L.red_add].w;
                ad->sign = fn->ins[L.red_add].sign;
                ad->line = fn->ins[L.red_add].line;
                ad->col = fn->ins[L.red_add].col;
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
        free(vec);
        if (remarks_on() && fn->src)
            remark_add("opt", "vectorized", fn->name, "vec/constant-trip-count",
                       fn->file, fn->line,
                       "%d lanes of %d bytes, %ld iterations -> %ld%s",
                       L.vf, L.esize, L.bound, L.bound / L.vf,
                       L.red_acc >= 0 ? " (a sum reduction)" : "");
        done = 1;
    }

    free(in);
    free_defs(&d);
    free(order); free(l2b);
    free_cfg(bb, nbb);
    return done;
}

int pass_vectorize(struct ir_func *fn)
{
    int changed = 0, guard = 0;
    while (guard++ < 32 && vectorize_one(fn))
        changed = 1;
    return changed;
}
