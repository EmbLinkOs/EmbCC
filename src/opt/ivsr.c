/* ==== induction-variable strength reduction (-O2) =========================== *
 *
 * `a[i]` costs three instructions an iteration that have nothing to do
 * with a[i]: widen i, shift it by the element size, add the base. The
 * value they compute is base + i*scale, which changes by scale*step
 * every iteration -- so it can be an induction variable of its own,
 * walked by one add, instead of being rebuilt from i each time.
 *
 *      ext  t1, i           p = base                 (preheader)
 *      shl  t2, t1, #2      ...
 *      add  t3, base, t2        load [p]
 *      load [t3]            p = p + 4                (latch)
 *
 * The chain then has no users and DCE takes it, so three instructions
 * become one. It is the oldest loop optimization there is and it
 * applies to every array loop, vectorized or not: in a vectorized loop
 * the index steps by VF, so the pointer steps by 16 and the saving is
 * the same.
 *
 * ---- why this runs last ----
 *
 * It DESTROYS the shape the vectorizer matches on. addr_of_iv looks for
 * exactly `add(base, shl(ext(i), k))`, and a loop whose addressing has
 * already been reduced to a walking pointer does not have it. So this
 * runs once, after the loop passes have converged, and never inside
 * their round -- otherwise it would race the vectorizer for the same
 * loops and win, and the lanes would be lost to save an add.
 *
 * ---- what may be reduced ----
 *
 * The value must be linear in the induction variable with a
 * loop-invariant base, and -- the part that is easy to forget -- it must
 * not be read after the loop. The new pointer ends one step PAST where
 * the old chain last computed, because it is incremented at the latch
 * like the induction variable itself, so a use outside the loop would
 * see base + n*scale where the chain would have given base + (n-1)*scale.
 */

#include "opt_int.h"

/* Is `x` the induction variable scaled by a constant -- iv, a widening of
 * it, a shift of that by a constant, or a multiply by one? Fills the
 * scale. A multiply is how a ROW of a 2-D array is indexed (`m[k][j]`
 * is base + k*48 + j*2), and the shift is how an element is. */
static int scaled_iv(struct ir_func *fn, struct defs *d, int lo, int hi,
                     int x, int iv, long *scale_out)
{
    long sc = 1;
    if (x >= 0 && x < fn->nvregs && d->cnt[x] == 1) {
        int xn = d->ins[x];
        if (xn >= lo && xn < hi &&
            (fn->ins[xn].op == IR_SHL || fn->ins[xn].op == IR_MUL)) {
            struct ir_ins *s = &fn->ins[xn];
            long c;
            if (!const_b(fn, d, s, &c))
                return 0;
            if (s->op == IR_SHL) {
                if (c < 0 || c > 30) return 0;
                sc = 1L << c;
            } else {
                if (c <= 0 || c > (1L << 20)) return 0;
                sc = c;
            }
            x = s->a;
        }
    }
    if (x >= 0 && x < fn->nvregs && d->cnt[x] == 1) {
        int xn = d->ins[x];
        if (xn >= lo && xn < hi && fn->ins[xn].op == IR_EXT)
            x = fn->ins[xn].a;
    }
    if (x != iv)
        return 0;
    *scale_out = sc;
    return 1;
}

/* Defined once, and outside [lo, hi): a value the loop cannot change. */
static int invariant_vreg(struct ir_func *fn, struct defs *d, int lo, int hi, int v)
{
    if (v < 0 || v >= fn->nvregs || d->cnt[v] != 1)
        return 0;
    int bd = d->ins[v];
    return !(bd >= lo && bd < hi);
}
static int is_const_vreg(struct ir_func *fn, struct defs *d, int v)
{
    return v >= 0 && v < fn->nvregs && d->cnt[v] == 1 && d->ins[v] >= 0 &&
           fn->ins[d->ins[v]].op == IR_CONST;
}

/* Is `v` the value base + iv*scale (+ base2), every base invariant? The
 * second base is the column of a row walk: (m + k*48) + j*2, where j*2
 * is fixed while k moves. base2 is -1 when there is none. */
static int linear_in_iv2(struct ir_func *fn, struct defs *d, int lo, int hi,
                         int v, int iv, int *base_out, int *base2_out,
                         long *scale_out)
{
    if (v < 0 || v >= fn->nvregs || d->cnt[v] != 1)
        return 0;
    int an = d->ins[v];
    if (an < lo || an >= hi || fn->ins[an].op != IR_ADD || fn->ins[an].imm_b)
        return 0;
    const struct ir_ins *add = &fn->ins[an];
    for (int side = 0; side < 2; side++) {
        int p = side ? add->b : add->a, q = side ? add->a : add->b;
        if (!invariant_vreg(fn, d, lo, hi, p))
            continue;
        /* base + scaled iv -- where the base is something to walk from:
         * a CONSTANT base is counter arithmetic, `i + 1` with its one
         * hoisted into a vreg, and taking that for an address turned
         * every counted loop's increment into a second induction
         * variable, which the unroller then no longer recognised (the
         * `unroll` kernel ran 1.6 times slower). */
        if (!is_const_vreg(fn, d, p) &&
            scaled_iv(fn, d, lo, hi, q, iv, scale_out)) {
            *base_out = p; *base2_out = -1;
            return 1;
        }
        /* (base + scaled iv) + base2, the inner sum computed only here */
        if (q >= 0 && q < fn->nvregs && d->cnt[q] == 1) {
            int qn = d->ins[q];
            if (qn < lo || qn >= hi || fn->ins[qn].op != IR_ADD || fn->ins[qn].imm_b)
                continue;
            const struct ir_ins *in2 = &fn->ins[qn];
            for (int s2 = 0; s2 < 2; s2++) {
                int b1 = s2 ? in2->b : in2->a, r = s2 ? in2->a : in2->b;
                if (invariant_vreg(fn, d, lo, hi, b1) &&
                    !(is_const_vreg(fn, d, b1) && is_const_vreg(fn, d, p)) &&
                    scaled_iv(fn, d, lo, hi, r, iv, scale_out)) {
                    *base_out = b1; *base2_out = p;
                    return 1;
                }
            }
        }
    }
    return 0;
}

/* Is `v` the value base + iv*scale, with base invariant? Fills base and
 * scale. The chain is irgen's: an optional widening of the index, a
 * shift by the log of the element size, and an add. */
static int linear_in_iv(struct ir_func *fn, struct defs *d, int lo, int hi,
                        int v, int iv, int *base_out, long *scale_out)
{
    if (v < 0 || v >= fn->nvregs || d->cnt[v] != 1)
        return 0;
    int an = d->ins[v];
    if (an < lo || an >= hi || fn->ins[an].op != IR_ADD)
        return 0;
    struct ir_ins *add = &fn->ins[an];
    for (int side = 0; side < 2; side++) {
        int base = side ? add->b : add->a;
        int idx  = side ? add->a : add->b;
        if (base < 0 || base >= fn->nvregs || d->cnt[base] != 1)
            continue;
        int bd = d->ins[base];
        if (bd >= lo && bd < hi)
            continue;                    /* the base must not move */
        if (idx < 0 || idx >= fn->nvregs || d->cnt[idx] != 1)
            continue;
        int sn = d->ins[idx];
        if (sn < lo || sn >= hi || fn->ins[sn].op != IR_SHL)
            continue;
        long sh;
        if (!const_b(fn, d, &fn->ins[sn], &sh) || sh < 0 || sh > 60)
            continue;
        int x = fn->ins[sn].a;
        if (x >= 0 && x < fn->nvregs && d->cnt[x] == 1) {
            int xn = d->ins[x];
            if (xn >= lo && xn < hi && fn->ins[xn].op == IR_EXT)
                x = fn->ins[xn].a;
        }
        if (x != iv)
            continue;
        *base_out = base;
        *scale_out = 1L << sh;
        return 1;
    }
    return 0;
}

/* Every definition and every read of each vreg, as chains built once:
 * ivsr_one asked "who defines the counter outside the loop" and "does
 * anything outside the loop read this address" by walking the whole
 * function, for every loop and every candidate in it -- 40% of the time
 * EmbCC took to compile its own src/arch/regalloc.c at -O2. The chains
 * answer the same questions, in the same order, from the vreg's own
 * list. A definition here is def_target's, IR_LANDING included (unlike
 * defs_lists); a read is ins_reads's, a frame slot's LDVAR/ADDR not. */
struct vchains {
    int nv;
    int *dfirst, *dnext;             /* definitions, by instruction */
    int *rfirst, *rnext, *rins;      /* reads: entries, newest first */
    int nr, cap, cur;
};
static void vchains_read_cb(int *p, void *ctx)
{
    struct vchains *c = ctx;
    int v = *p;
    if (v < 0 || v >= c->nv)
        return;
    if (c->rfirst[v] >= 0 && c->rins[c->rfirst[v]] == c->cur)
        return;                       /* read twice by one instruction */
    if (c->nr == c->cap) {
        c->cap = c->cap ? 2 * c->cap : 64;
        c->rnext = xrealloc(c->rnext, (size_t)c->cap * sizeof *c->rnext);
        c->rins = xrealloc(c->rins, (size_t)c->cap * sizeof *c->rins);
    }
    c->rins[c->nr] = c->cur;
    c->rnext[c->nr] = c->rfirst[v];
    c->rfirst[v] = c->nr++;
}
static void vchains_build(struct ir_func *fn, struct vchains *c)
{
    memset(c, 0, sizeof *c);
    c->nv = fn->nvregs;
    size_t nv = (size_t)(fn->nvregs ? fn->nvregs : 1);
    c->dfirst = xmalloc(nv * sizeof *c->dfirst);
    c->rfirst = xmalloc(nv * sizeof *c->rfirst);
    c->dnext = xmalloc((size_t)(fn->nins ? fn->nins : 1) * sizeof *c->dnext);
    for (int v = 0; v < fn->nvregs; v++)
        c->dfirst[v] = c->rfirst[v] = -1;
    for (int n = fn->nins - 1; n >= 0; n--) {   /* ascending chains */
        int t = def_target(&fn->ins[n]);
        c->dnext[n] = -1;
        if (t >= 0 && t < fn->nvregs) {
            c->dnext[n] = c->dfirst[t];
            c->dfirst[t] = n;
        }
    }
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        if (i->op == IR_LDVAR || i->op == IR_ADDR)
            continue;
        c->cur = n;
        each_read(i, vchains_read_cb, c);
    }
}
static void vchains_free(struct vchains *c)
{
    free(c->dfirst); free(c->dnext);
    free(c->rfirst); free(c->rnext); free(c->rins);
}
/* Does an instruction outside [lo, hi) read v? */
static int vchains_read_outside(const struct vchains *c, int v, int lo, int hi)
{
    if (v < 0 || v >= c->nv)
        return 0;
    for (int k = c->rfirst[v]; k >= 0; k = c->rnext[k])
        if (c->rins[k] < lo || c->rins[k] >= hi)
            return 1;
    return 0;
}

static int ivsr_one(struct ir_func *fn)
{
    if (fn->nins == 0)
        return 0;
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
    struct vchains vc;
    vchains_build(fn, &vc);
    char *in = xmalloc((size_t)nbb);
    int done = 0;

    for (int oi = norder - 1; oi >= 0 && !done; oi--) {
        int h = order[oi];
        if (h == 0 || bb[h].end <= bb[h].start ||
            fn->ins[bb[h].start].op != IR_LABEL)
            continue;
        int Lh = fn->ins[bb[h].start].label;
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
            continue;
        memset(in, 0, (size_t)nbb);
        loop_body(bb, nbb, h, latch, in);
        int lo = bb[h].start, hi = bb[latch].end, ok = 1;
        for (int b = 0; b < nbb && ok; b++)
            if (in[b] != (bb[b].start >= lo && bb[b].end <= hi))
                ok = 0;
        if (!ok)
            continue;

        /* the induction variable and its step, as the vectorizer finds them */
        int copy_ins = -1;
        for (int n = lo; n < hi; n++)
            if (fn->ins[n].op == IR_MOV && fn->ins[n].dst >= 0 &&
                d.cnt[fn->ins[n].dst] == 2) {
                int nxt = fn->ins[n].a;
                if (nxt < 0 || nxt >= fn->nvregs || d.cnt[nxt] != 1)
                    continue;
                int sn = d.ins[nxt];
                if (sn < lo || sn >= hi || fn->ins[sn].op != IR_ADD)
                    continue;
                long st;
                if (fn->ins[sn].a != fn->ins[n].dst ||
                    !const_b(fn, &d, &fn->ins[sn], &st))
                    continue;
                copy_ins = n;
                break;
            }
        if (copy_ins < 0)
            continue;
        int iv = fn->ins[copy_ins].dst;
        int step_ins = d.ins[fn->ins[copy_ins].a];
        long step = 0;
        (void)const_b(fn, &d, &fn->ins[step_ins], &step);
        if (step <= 0)
            continue;

        /* Where the walk happens: immediately before the compare that
         * feeds the back branch, which is AFTER every phi copy in the
         * latch.
         *
         * Putting it next to `i += 1` looked natural and was wrong. The
         * phi copies sit after the step -- `p = mov <the address>` among
         * them -- so the pointer had already moved on by the time one
         * was taken, and `p = &a[i]` came out as &a[i+1]. The answer
         * differed at -O2 and nowhere else. */
        int inc_at = -1;
        if (br->a >= 0 && br->a < fn->nvregs && d.cnt[br->a] == 1)
            inc_at = d.ins[br->a];
        if (inc_at <= lo || inc_at >= hi) {
            VDBG("ivsr h=%d no compare to insert before (br->a=%d cnt=%d"
                 " inc_at=%d lo=%d hi=%d)\n", h, br->a,
                 br->a >= 0 && br->a < fn->nvregs ? d.cnt[br->a] : -1,
                 inc_at, lo, hi);
            continue;
        }

        /* the induction variable must start at a known place, because
         * the new pointer has to be initialised to match it: its one
         * other definition is OUTSIDE the loop, and is a zero.
         *
         * "Outside" is the part that was missing. With both definitions
         * inside, this loop had nothing to check and said yes -- which is
         * what an INNER loop's counter looks like from the loop around
         * it: `for (j..) for (i = k; i < 16; i++) s += a[i]` walked its
         * pointer once per j, from a, and the inner loop read a[j]
         * sixteen times. Only an inner counter starting at zero escaped,
         * because that inner loop was rewritten first and its counter
         * was gone before the outer one looked. */
        int init_zero = 1, ninit = 0;
        for (int n = iv >= 0 && iv < vc.nv ? vc.dfirst[iv] : -1;
             n >= 0 && init_zero; n = vc.dnext[n]) {
            if (n >= lo && n < hi)
                continue;
            struct ir_ins *i = &fn->ins[n];
            ninit++;
            if (!((i->op == IR_MOV && i->a >= 0 && i->a < fn->nvregs &&
                   d.cnt[i->a] == 1 && d.ins[i->a] >= 0 &&
                   fn->ins[d.ins[i->a]].op == IR_CONST &&
                   fn->ins[d.ins[i->a]].imm == 0) ||
                  (i->op == IR_CONST && i->imm == 0)))
                init_zero = 0;
            /* A copy of a temp written more than once -- value numbering
             * shares one `const 0` between `s = 0` and `k = 0`, and s is
             * then the loop's accumulator too. What the copy reads is the
             * nearest write of that temp before it in the same block: if
             * that is a zero, so is the counter. */
            if (!init_zero && i->op == IR_MOV && i->a >= 0 &&
                i->a < fn->nvregs && d.cnt[i->a] > 1) {
                for (int m = n - 1; m >= 0; m--) {
                    const struct ir_ins *w = &fn->ins[m];
                    if (w->op == IR_LABEL || w->op == IR_ASM ||
                        w->op == IR_LANDING)
                        break;
                    if (def_target(w) != i->a)
                        continue;
                    if (w->op == IR_CONST && w->imm == 0 && !w->flt)
                        init_zero = 1;
                    break;
                }
            }
        }
        if (!init_zero || ninit != 1)
            continue;

        /* every candidate: an address computed from the index, whose
         * value nothing outside the loop reads */
        int cand[16], cbase[16], cbase2[16], nc = 0;
        long cscale[16];
        for (int n = lo; n < hi && nc < 16; n++) {
            int t = def_target(&fn->ins[n]);
            int base, base2 = -1; long scale;
            /* At POINTER width, and only there: the walk is an add of that
             * width, so it reproduces the chain exactly modulo 2^PTRW and
             * nothing else. A 64-bit integer on a 32-bit target --
             * `m[i][k] = i * 1000000000LL + k` -- is linear in k too, and
             * walking it as a pointer kept its low word and dropped the
             * high one. */
            if (t < 0 || fn->ins[n].op != IR_ADD || fn->ins[n].w != PTRW ||
                fn->ins[n].flt)
                continue;
            /* Computed AFTER the induction variable's own update, it is
             * the address for the NEXT value of the index -- and the
             * pointer moves only at inc_at, further on, so it would
             * still hold this iteration's. Rotation puts exactly such an
             * address in the latch: the copy of the header's test goes
             * after `i = mov i1`, and `while (s[n]) n++;` read s[n - 1]
             * there on every trip -- strlen by index was one short of
             * its answer at -O2 and -Os on every target. */
            if (n > copy_ins)
                continue;
            if (!linear_in_iv(fn, &d, lo, hi, t, iv, &base, &scale) &&
                !linear_in_iv2(fn, &d, lo, hi, t, iv, &base, &base2, &scale))
                continue;
            if (vchains_read_outside(&vc, t, lo, hi))
                continue;
            int dup = 0;
            for (int k = 0; k < nc; k++)
                if (cand[k] == t) dup = 1;
            if (dup)
                continue;
            cand[nc] = t; cbase[nc] = base; cbase2[nc] = base2;
            cscale[nc] = scale; nc++;
        }
        if (nc == 0)
            continue;

        /* ---- and then the loop can stop counting ----------------------
         *
         * The test `i < bound` and the pointer `p = base + i*scale` are
         * about the same iterations, so the test can be made about the
         * POINTER instead: p walks from base in steps of scale*step and
         * reaches base + T*scale*step exactly when i reaches T*step,
         * where T = ceil(bound/step) is the trip count. `!=` rather than
         * `<` because p hits that value exactly -- it steps by a fixed
         * amount from a fixed start -- which also keeps the question
         * away from whether a pointer compare is signed.
         *
         * What it buys is the counter: once nothing reads i, `i += step`
         * and the copy that carries it are a cycle that no live
         * instruction reaches, and the marking dead-code pass takes
         * both. Two instructions an iteration, and one live value fewer
         * across the whole loop. Measured on the vectorized copy loop of
         * tests/bench/kernels.c memory_stream: ten instructions an
         * iteration down to eight, which is gcc's nine.
         *
         * Only for a CONSTANT bound, because T is otherwise a division
         * by step in the preheader, and only when the index is dead
         * outside the loop, because otherwise the counter stays and the
         * two extra instructions here are all that happens. */
        int lftr_lim = -1, lftr_k = -1, lftr_iv = -1;
        long lftr_off = 0;
        {
            struct ir_ins *cmpi = &fn->ins[inc_at];
            long bound = 0;
            long scale = cscale[0], dv = scale * step;
            if (cmpi->op == IR_CMP && cmpi->pred == B_LT &&
                cmpi->sign && !cmpi->flt && cmpi->a == iv &&
                const_b(fn, &d, cmpi, &bound) &&
                bound > 0 && bound <= (1L << 40) &&
                scale > 0 && scale <= (1L << 20) && step <= (1L << 20)) {
                long T = (bound + step - 1) / step;
                if (T > 0 && dv > 0 && T <= (1L << 40) / dv) {
                    lftr_off = T * dv;
                    lftr_k = fn->nvregs++;
                    lftr_lim = fn->nvregs++;
                    /* The index gets a NAME OF ITS OWN inside the loop.
                     * It has two definitions -- the one before the loop
                     * and the copy at the latch -- and the loop's GUARD
                     * reads it, which is a use no amount of rewriting
                     * inside the loop removes. One vreg with a live read
                     * keeps every definition of it, so the latch copy
                     * would survive and the counter with it. Split in
                     * two and the guard keeps the value it was always
                     * reading (the one from before the loop) while the
                     * loop's copy becomes a cycle nothing outside can
                     * reach. */
                    lftr_iv = fn->nvregs++;
                }
            }
        }

        /* ---- rewrite: a pointer per candidate ---- */
        int ptr[16], delta[16], bsum[16];
        for (int k = 0; k < nc; k++) {
            ptr[k] = fn->nvregs++;
            delta[k] = fn->nvregs++;
            bsum[k] = cbase2[k] >= 0 ? fn->nvregs++ : -1;
        }
        struct ibuf nb = { 0, 0, 0 };
        int *newpos = fn->var_scope_lo
            ? xmalloc((size_t)(fn->nins + 1) * sizeof *newpos) : NULL;
        /* The renames, made once: every candidate to its pointer, and the
         * index to its own name inside the loop. They were a table of
         * every vreg, allocated and filled for each instruction of the
         * function and each candidate -- instructions x vregs a rewrite,
         * most of EmbCC's -O2 time on its own regalloc.c. One table holds
         * all the candidates: a pointer is a fresh vreg and never a
         * candidate, so renaming them together is renaming them in turn. */
        int nvmap = fn->nvregs;
        int *cmap = xmalloc((size_t)nvmap * sizeof *cmap);
        int *imap = lftr_iv >= 0 ? xmalloc((size_t)nvmap * sizeof *imap) : NULL;
        for (int v = 0; v < nvmap; v++) {
            cmap[v] = -1;
            if (imap) imap[v] = -1;
        }
        for (int k = 0; k < nc; k++)
            cmap[cand[k]] = ptr[k];
        if (imap)
            imap[iv] = lftr_iv;
        for (int n = 0; n < fn->nins; n++) {
            if (n == lo) {
                /* a two-part base, summed once before the loop */
                for (int k = 0; k < nc; k++) {
                    if (bsum[k] < 0)
                        continue;
                    struct ir_ins *a = ib_push(&nb);
                    a->op = IR_ADD; a->dst = bsum[k]; a->a = cbase[k];
                    a->b = cbase2[k]; a->w = PTRW;
                    a->line = fn->ins[d.ins[cand[k]]].line; a->synth = 1;
                    cbase[k] = bsum[k];
                }
                for (int k = 0; k < nc; k++) {
                    /* the line by value: `c` is not valid after the next
                     * ib_push, which may move the buffer -- reading it
                     * there faulted once the push landed on a growth */
                    int ln = fn->ins[cand[k] >= 0 ? d.ins[cand[k]] : n].line;
                    struct ir_ins *c = ib_push(&nb);
                    c->op = IR_CONST; c->dst = delta[k];
                    c->w = PTRW; c->imm = cscale[k] * step;
                    c->line = ln;
                    c->synth = 1;
                    struct ir_ins *m = ib_push(&nb);
                    m->op = IR_MOV; m->dst = ptr[k]; m->a = cbase[k];
                    m->w = PTRW;
                    m->line = ln; m->synth = 1;
                }
                if (lftr_lim >= 0) {
                    struct ir_ins *c = ib_push(&nb);
                    c->op = IR_CONST; c->dst = lftr_k; c->w = PTRW;
                    c->imm = lftr_off;
                    c->line = fn->ins[inc_at].line; c->synth = 1;
                    struct ir_ins *a3 = ib_push(&nb);
                    a3->op = IR_ADD; a3->dst = lftr_lim; a3->a = cbase[0];
                    a3->b = lftr_k; a3->w = PTRW;
                    a3->line = fn->ins[inc_at].line; a3->synth = 1;
                    struct ir_ins *m2 = ib_push(&nb);
                    m2->op = IR_MOV; m2->dst = lftr_iv; m2->a = iv;
                    m2->w = fn->ins[copy_ins].w ? fn->ins[copy_ins].w : PTRW;
                    m2->line = fn->ins[inc_at].line; m2->synth = 1;
                }
            }
            if (n == inc_at) {          /* walk each pointer, after the copies */
                for (int k = 0; k < nc; k++) {
                    struct ir_ins *a2 = ib_push(&nb);
                    a2->op = IR_ADD; a2->dst = ptr[k]; a2->a = ptr[k];
                    a2->b = delta[k]; a2->w = PTRW;
                    a2->line = fn->ins[n].line; a2->synth = 1;
                }
            }
            if (newpos) newpos[n] = nb.n;
            /* the chain's add disappears; its users read the pointer */
            int skip = 0;
            for (int k = 0; k < nc; k++)
                if (def_target(&fn->ins[n]) == cand[k] && n >= lo && n < hi)
                    skip = 1;
            if (skip)
                continue;
            if (lftr_lim >= 0 && n == inc_at) {
                struct ir_ins *o = ib_push(&nb);
                *o = fn->ins[n];
                o->pred = B_NE; o->a = ptr[0]; o->b = lftr_lim;
                o->w = PTRW; o->sign = 0; o->imm_b = 0; o->imm = 0;
                continue;
            }
            struct ir_ins *o = ib_push(&nb);
            *o = fn->ins[n];
            {
                struct lcopy lc = { cmap, nvmap, 0 };
                each_read(o, lcopy_cb, &lc);
            }
            /* Inside the loop, the index goes by its own name. */
            if (lftr_iv >= 0 && n >= lo && n < hi) {
                struct lcopy lc = { imap, nvmap, 0 };
                each_read(o, lcopy_cb, &lc);
                if (def_target(o) == iv)
                    o->dst = lftr_iv;
            }

        }
        if (newpos) {
            newpos[fn->nins] = nb.n;
            for (int v = 0; v < fn->nvars; v++) {
                int l2 = fn->var_scope_lo[v], h2 = fn->var_scope_hi[v];
                if (l2 >= 0 && l2 <= fn->nins) fn->var_scope_lo[v] = newpos[l2];
                if (h2 >= 0 && h2 <= fn->nins) fn->var_scope_hi[v] = newpos[h2];
            }
            free(newpos);
        }
        free(fn->ins);
        free(cmap); free(imap);
        fn->ins = nb.p; fn->nins = nb.n; fn->cap = nb.cap;
        if (remarks_on() && fn->src)
            remark_add("opt", "strength-reduced", fn->name, "ivsr/address",
                       fn->file, fn->line,
                       "%d address%s walked instead of recomputed",
                       nc, nc == 1 ? "" : "es");
        done = 1;
    }

    free(in); free_defs(&d); vchains_free(&vc);
    free(order); free(l2b);
    free_cfg(bb, nbb);
    return done;
}

int pass_ivsr(struct ir_func *fn)
{
    int changed = 0, guard = 0;
    while (guard++ < 32 && ivsr_one(fn))
        changed = 1;
    return changed;
}
