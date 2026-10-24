/* ==== loop unrolling (-O2, not -Os) ======================================= *
 *
 * `for (i = 0; i < n; i++) s += i & 7;` spends two of its five
 * instructions on being a loop -- the increment and the test -- and one
 * more on the branch back. Running the body four times per test spends
 * them once instead of four times, and gives the block-local passes four
 * copies in one block to fold, value-number and schedule together.
 *
 * ---- the shape, and why there is no remainder loop -----------------------
 *
 * A runtime trip count is the whole difficulty: `n` is not known, so the
 * body cannot simply be copied U times. The usual answer is a second,
 * scalar loop for the leftover iterations. This does not build one,
 * because it already has one -- THE ORIGINAL LOOP. The unrolled copies
 * are inserted in FRONT of it and fall into it when fewer than U
 * iterations remain:
 *
 *   LU:  if (!(i < n)) goto exit          <- the loop may be over
 *        if ((unsigned)(n - i) < U) goto L  <- fewer than U left
 *        body; i++    (U times, no test between)
 *        goto LU
 *   L:   body; i++; if (i < n) goto L     <- the original loop, untouched
 *   exit:
 *
 * So the transform ADDS code and changes nothing that was there: the
 * original loop is still correct on its own, still the target of its own
 * back edge, and still what runs if anything jumps straight to it.
 *
 * The subtraction is the part worth stating. `n - i` computed as SIGNED
 * can overflow -- `for (i = LONG_MIN; i < LONG_MAX; i++)` is a legal
 * loop -- so the count of remaining iterations is taken in UNSIGNED,
 * where the difference of two values with i < n is exactly n - i and
 * cannot wrap. The `i < n` test above it is what makes that true, which
 * is why it is there and not left to the loop's own guard: this block is
 * the target of a back edge, and the guard runs once.
 *
 * ---- what may be unrolled ------------------------------------------------
 *
 * A rotated, bottom-tested loop -- rotation has already run -- whose only
 * control transfer is its own back edge, whose induction variable steps
 * by one and is compared `< invariant` with a signed compare, and whose
 * body defines nothing that is read after the loop except through the
 * values the loop carries (which keep their numbers, so the last copy's
 * write is the one that escapes). Anything that moves the stack or is
 * its own control flow -- alloca, inline asm, a landing pad, a computed
 * goto -- is refused rather than duplicated.
 */

#include "opt_int.h"

#define UNROLL_MAX_BODY   20  /* instructions in the body worth copying */
#define UNROLL_BUDGET     96  /* and a ceiling on U * body */
#define UNROLL_MAX_COPIES  8
#define UNROLL_FULL_TRIP  32  /* a constant trip count copied whole: at most */
#define UNROLL_FULL_BODY 200  /* ...and at most this many instructions in all */

/* One loop's worth of what the recognizer found. */
struct unrloop {
    int lo, hi;        /* the loop's instruction range, [lo, hi) */
    int header;        /* bb index of the header; Lh is its label */
    int Lh;
    int iv;            /* the induction variable's phi temp */
    int bound;         /* the vreg it is compared against (loop-invariant) */
    long step;         /* what the iv advances by each iteration: 1, or a
                        * pointer walk's element size */
    int cmp_ins;       /* `c = cmp.w lt iv, bound`, or `cmp.w ne iv, bound`
                        * once strength reduction has made the loop walk a
                        * pointer -- not copied */
    int w;             /* 4 or 8: the compare's width */
    int body_lo;       /* [body_lo, cmp_ins): what a copy consists of */
};

/* Is this instruction safe to appear twice? Duplication does not change
 * how many times anything RUNS -- the copies stand in for iterations
 * that would have happened anyway -- so the question is only whether the
 * instruction can be re-emitted at all. */
static int unr_copyable(const struct ir_ins *i)
{
    switch (i->op) {
    case IR_ALLOCA: case IR_SPSAVE: case IR_SPRESTORE:
    case IR_ASM: case IR_VA_START: case IR_LANDING:
    case IR_IGOTO: case IR_LABELADDR: case IR_SWITCH:
    case IR_JMP: case IR_BRZ: case IR_BRNZ: case IR_RET: case IR_UD2:
        return 0;
    case IR_CALL:
        return !i->eh_region;   /* a call in an exception region has an edge */
    default:
        return 1;
    }
}

/* One fresh instruction in the buffer. A FUNCTION and not a macro: its
 * arguments are fully evaluated before ib_push runs, so there is no way
 * for a nested push to reallocate the buffer under a pointer this
 * already returned -- which is how three separate bugs got written. */
static struct ir_ins *unr_emit(struct ibuf *nb, enum ir_op op,
                               int line, int col)
{
    struct ir_ins *p = ib_push(nb);
    p->op = op;
    p->line = line; p->col = col; p->synth = 1;
    return p;
}

/* Find an unrollable loop, or return 0. */
static int unr_find(struct ir_func *fn, struct bb *bb, int nbb, int *order,
                    int norder, struct defs *d, char *in, struct unrloop *L,
                    const char *seen, int nseen)
{
    for (int oi = norder - 1; oi >= 0; oi--) {
        int h = order[oi];
        if (h == 0 || bb[h].end <= bb[h].start ||
            fn->ins[bb[h].start].op != IR_LABEL)
            continue;
        int Lh = fn->ins[bb[h].start].label;
        /* A loop this pass already unrolled still matches, and unrolling
         * it again would only copy the remainder loop into another
         * remainder loop. One pass over each. */
        if (Lh >= 0 && Lh < nseen && seen[Lh])
            continue;
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
        /* The loop's blocks must be exactly the contiguous instruction
         * range [lo, hi): the same test the vectorizer makes, and what
         * lets a copy be a memcpy of a slice. */
        memset(in, 0, (size_t)nbb);
        loop_body(bb, nbb, h, latch, in);
        L->lo = bb[h].start; L->hi = bb[latch].end;
        int ok = 1;
        for (int b = 0; b < nbb && ok; b++)
            if (in[b] != (bb[b].start >= L->lo && bb[b].end <= L->hi))
                ok = 0;
        if (!ok)
            continue;
        /* There must be a label after the loop to send a finished
         * unrolled run to. One is created by the caller when the loop
         * is taken, so only the range has to be inside the function. */
        if (L->hi > fn->nins)
            continue;

        /* `c = cmp.w lt iv, bound`, signed, bound a loop-invariant vreg --
         * or `cmp.w ne iv, bound`, which is what induction-variable
         * strength reduction leaves: the loop walks a pointer up to its
         * end. For `ne` the iterations left are the UNSIGNED, modular
         * (bound - iv) / step exactly when the loop terminates at all,
         * whatever the direction the values were in, so the counted
         * tests below are right for it with no signed guard. */
        int cn = (br->a >= 0 && br->a < fn->nvregs && d->cnt[br->a] == 1)
                 ? d->ins[br->a] : -1;
        if (cn < L->lo || cn >= L->hi || cn != L->hi - 2)
            continue;                    /* the test must be the latch's last */
        struct ir_ins *cmp = &fn->ins[cn];
        if (cmp->op != IR_CMP || cmp->flt || cmp->imm_b ||
            cmp->b < 0 || cmp->b >= fn->nvregs ||
            !((cmp->pred == B_LT && cmp->sign) || cmp->pred == B_NE))
            continue;
        if (cmp->w != 4 && cmp->w != 8)
            continue;
        L->cmp_ins = cn; L->iv = cmp->a; L->bound = cmp->b; L->w = cmp->w;
        L->header = h; L->Lh = Lh;
        if (L->iv < 0 || L->iv >= fn->nvregs)
            continue;
        /* The bound is loop-invariant, and live on entry to the header
         * (the compare reads it), so it is available anywhere on the
         * edge into the header -- which is where the copies go. */
        int bn = d->cnt[L->bound] == 1 ? d->ins[L->bound] : -2;
        if (bn == -2 || (bn >= L->lo && bn < L->hi))
            continue;

        /* `iv = mov next`, `next = iv + 1`, both inside the loop -- or the
         * one instruction `iv = add iv, step`, which is how strength
         * reduction advances the pointer it walks. Either way the iv must
         * be written exactly once inside the loop, so that the renaming
         * of the copies carries it from each to the next. */
        int copy_ins = -1, step_ins = -1, ndef = 0;
        for (int n = L->lo; n < L->hi; n++) {
            const struct ir_ins *q = &fn->ins[n];
            if (def_target(q) != L->iv)
                continue;
            ndef++;
            if (q->op == IR_MOV) copy_ins = n;
            else if (q->op == IR_ADD && q->a == L->iv) step_ins = n;
        }
        if (ndef != 1)
            continue;
        if (copy_ins >= 0) {
            int nxt = fn->ins[copy_ins].a;
            if (nxt < 0 || nxt >= fn->nvregs || d->cnt[nxt] != 1)
                continue;
            step_ins = d->ins[nxt];
            if (step_ins < L->lo || step_ins >= L->hi ||
                fn->ins[step_ins].op != IR_ADD ||
                fn->ins[step_ins].a != L->iv)
                continue;
        } else if (step_ins < 0) {
            continue;
        }
        long step;
        if (!const_b(fn, d, &fn->ins[step_ins], &step) || step < 1 ||
            (cmp->pred == B_LT && step != 1) || step > 4096)
            continue;
        L->step = step;

        /* Everything from just after the header's label up to the test is
         * what a copy consists of. */
        L->body_lo = L->lo + 1;
        int nbody = 0;
        ok = 1;
        for (int n = L->body_lo; n < L->cmp_ins && ok; n++) {
            struct ir_ins *i = &fn->ins[n];
            if (i->op == IR_LABEL) {
                /* An empty label left behind by rotation is fine to drop
                 * from the copies; one anything can still reach is not. */
                for (int m = 0; m < fn->nins && ok; m++) {
                    enum ir_op o = fn->ins[m].op;
                    if ((o == IR_JMP || o == IR_BRZ || o == IR_BRNZ) &&
                        fn->ins[m].label == i->label)
                        ok = 0;
                }
                continue;
            }
            if (!unr_copyable(i)) { ok = 0; break; }
            /* A loop whose body CALLS something gets nothing out of
             * this. What unrolling saves is the test and the branch,
             * which next to a call -- its argument setup, its frame, its
             * return -- is a rounding error, and what it costs is U
             * copies of the call site and U times the pressure across
             * it. Measured: four copies of `s = f(s, i)` ran 63% SLOWER
             * than the loop it replaced. */
            if (i->op == IR_CALL) { ok = 0; break; }
            nbody++;
            int t = def_target(i);
            if (t < 0 || t >= fn->nvregs || d->cnt[t] != 1)
                continue;
            /* A value the body computes and something AFTER the loop
             * reads would, once copied, be the wrong copy's. Values the
             * loop CARRIES are multiply-assigned, and the block ends by
             * copying the last version back into them, so those are
             * fine; a singly-assigned one must not leave the loop. */
            for (int m = 0; m < fn->nins && ok; m++)
                if ((m < L->lo || m >= L->hi) && ins_reads(&fn->ins[m], t))
                    ok = 0;
            /* And it must not be READ before it is written inside the
             * loop either: that is a value carried across the back edge
             * without a copy to carry it, which the renaming below would
             * silently turn into this iteration's. */
            for (int m = L->lo; m < n && ok; m++)
                if (ins_reads(&fn->ins[m], t))
                    ok = 0;
        }
        if (!ok || nbody == 0 || nbody > UNROLL_MAX_BODY)
            continue;
        return nbody;
    }
    return 0;
}

/* each_label's callback for "does anything name this label". */
struct unr_lblctx { int label, hit; };
static void unr_lbl_cb(int *p, void *ctx)
{
    struct unr_lblctx *c = ctx;
    if (*p == c->label) c->hit = 1;
}

/* The loop's trip count when it is a constant, else 0.
 *
 * Constant means the induction variable's ONE definition outside the
 * loop sits in the block that falls into the header, nothing but the
 * back edge jumps to the header -- so every entry passes that definition
 * -- and it and the bound are a pair whose distance is known:
 *
 *   iv = const c0;  ...  iv < const B       B - c0 iterations (step 1)
 *   iv = X;         ...  iv != X + C        C / step  (strength reduction's
 *   iv = X + c1;    ...  iv != X + c2       (c2 - c1) / step   pointer walk)
 *
 * A rotated loop runs its body before its first test, so the count is
 * exactly how many times the body runs on every entry. */
static long unr_trip(struct ir_func *fn, struct defs *d, const struct unrloop *L)
{
    for (int m = 0; m < fn->nins; m++) {
        if (m == L->hi - 1)
            continue;                       /* the back edge itself */
        struct unr_lblctx lc = { L->Lh, 0 };
        if (fn->ins[m].op == IR_LABELADDR && fn->ins[m].label == L->Lh)
            return 0;
        each_label(fn, &fn->ins[m], unr_lbl_cb, &lc);
        if (lc.hit)
            return 0;
    }
    int od = -1, nout = 0;
    for (int m = 0; m < fn->nins; m++)
        if ((m < L->lo || m >= L->hi) && def_target(&fn->ins[m]) == L->iv) {
            od = m;
            nout++;
        }
    if (nout != 1 || od < 0 || od >= L->lo)
        return 0;
    /* od falls into the header: no label between them, and nothing that
     * leaves except a branch that skips the loop altogether */
    for (int m = od + 1; m < L->lo; m++) {
        enum ir_op o = fn->ins[m].op;
        if (o == IR_LABEL || o == IR_JMP || o == IR_SWITCH || o == IR_RET ||
            o == IR_IGOTO || o == IR_UD2 || o == IR_ASM || o == IR_LANDING)
            return 0;
    }
    const struct ir_ins *e = &fn->ins[od];
    int bn = d->cnt[L->bound] == 1 ? d->ins[L->bound] : -1;
    if (bn < 0 || e->w != L->w)
        return 0;
    const struct ir_ins *b = &fn->ins[bn];
    const struct ir_ins *cmp = &fn->ins[L->cmp_ins];
    long t = 0;
    if (cmp->pred == B_LT) {
        long c0, B;
        if (e->op == IR_CONST && !e->flt) c0 = e->imm;
        else if (!(e->op == IR_MOV && get_const(fn, d, e->a, &c0))) return 0;
        if (!get_const(fn, d, L->bound, &B))
            return 0;
        if (L->w == 4) { c0 = (long)(int)c0; B = (long)(int)B; }
        t = B - c0;                          /* step is 1 for `<` */
    } else {
        /* the start and the bound off one base X */
        int x0; long c1 = 0, c2;
        if (e->op == IR_MOV) x0 = e->a;
        else if (!(e->op == IR_ADD && as_op_const(fn, d, (struct ir_ins *)e, &x0, &c1)))
            return 0;
        int x1;
        if (b->op != IR_ADD || b->w != L->w ||
            !as_op_const(fn, d, (struct ir_ins *)b, &x1, &c2))
            return 0;
        /* Either side may name its base through one more constant
         * add -- the preheader's `iv = mov X` with `X = a + 8` against a
         * bound of `a + 40` -- so each is followed back a step. */
        for (int k = 0; k < 2 && x0 != x1; k++) {
            int *xs = k ? &x1 : &x0;
            long *cs = k ? &c2 : &c1;
            int y; long cy;
            if (*xs < 0 || *xs >= fn->nvregs || d->cnt[*xs] != 1 ||
                d->ins[*xs] < 0)
                continue;
            struct ir_ins *a = &fn->ins[d->ins[*xs]];
            if (a->op == IR_ADD && a->w == L->w &&
                as_op_const(fn, d, a, &y, &cy)) {
                *xs = y;
                *cs += cy;
            }
        }
        if (x0 < 0 || x0 != x1 || x0 >= fn->nvregs || d->cnt[x0] != 1)
            return 0;
        long dist = c2 - c1;
        if (L->w == 4) dist = (long)(int)dist;
        if (dist <= 0 || dist % L->step)
            return 0;
        t = dist / L->step;
    }
    return t > 0 ? t : 0;
}

/* unr_local_cb: a read of v at instruction `at`. v stays the body's own
 * only while every read of it is inside the body and after its def. */
struct unr_local { char *local; const struct defs *d; int at, end, nv; };
static void unr_local_cb(int *p, void *ctx)
{
    struct unr_local *c = ctx;
    int v = *p;
    if (v < 0 || v >= c->nv || !c->local[v])
        return;
    if (!(c->at > c->d->ins[v] && c->at < c->end))
        c->local[v] = 0;
}

/* The U copies of the body, appended to nb. cur[v] is the name v
 * currently goes by inside this block, or -1 for "still itself". One
 * running map across every copy, so copy c reads what copy c-1 wrote --
 * and the LAST copy writes the original names, so the block leaves every
 * value exactly where the loop's own body would have, with no restoring
 * moves at the end. */
static void unr_copies(struct ir_func *fn, struct ibuf *nb,
                       const struct unrloop *L, long U, const char *local,
                       int nvr)
{
    int *cur = xmalloc((size_t)(nvr ? nvr : 1) * sizeof *cur);
    for (int v = 0; v < nvr; v++)
        cur[v] = -1;
    for (long c = 0; c < U; c++)
        for (int n2 = L->body_lo; n2 < L->cmp_ins; n2++) {
            if (fn->ins[n2].op == IR_LABEL)
                continue;             /* unreachable; see unr_find */
            int t = def_target(&fn->ins[n2]);
            struct ir_ins *q = ib_push(nb);
            *q = fn->ins[n2];
            ins_own_args(q);
            struct lcopy lc = { cur, nvr, 0 };
            each_read(q, lcopy_cb, &lc);
            /* A TEMP is renamed. A frame slot -- the dst of an stvar, a
             * local that stays in memory because its address is taken --
             * is not a value to rename but a place: every copy has to
             * write that one place, or a read of it through its address
             * sees none of the copies' stores. Renamed, `stvar v2` became
             * `stvar v98`, a slot that does not exist, and loading `*ps`
             * where ps = &s gave the value from before the loop. */
            if (t >= fn->nvars && t < nvr) {
                if (c == U - 1 && !local[t]) { q->dst = t; cur[t] = -1; }
                else { q->dst = fn->nvregs++; cur[t] = q->dst; }
            }
        }
    free(cur);
}

static int unroll_one(struct ir_func *fn, char *seen, int nseen)
{
    if (fn->nins == 0)
        return 0;
    int nbb, *l2b;
    struct bb *bb = build_cfg(fn, &nbb, &l2b);
    int *order = xmalloc((size_t)(nbb ? nbb : 1) * sizeof *order), norder;
    compute_rpo(bb, nbb, order, &norder);
    if (norder != nbb) {           /* unreachable blocks: do not mis-dominate */
        free(order); free(l2b);
        free_cfg(bb, nbb);
        return 0;
    }
    compute_idom(bb, order, norder);
    struct defs d;
    compute_defs(fn, &d);
    char *in = xmalloc((size_t)(nbb ? nbb : 1));
    struct unrloop L;
    int nbody = unr_find(fn, bb, nbb, order, norder, &d, in, &L,
                         seen, nseen);
    free(in);
    if (!nbody) {
        free_defs(&d); free(order); free(l2b);
        free_cfg(bb, nbb);
        return 0;
    }

    /* A constant trip count small enough to copy whole: the loop goes,
     * its test, its branch and the remainder machinery with it (see
     * unr_trip). The compare must have no reader past the loop, since it
     * goes too. */
    long T = getenv("EMBCC_NO_FULLUNROLL") ? 0 : unr_trip(fn, &d, &L);
    if (T < 2 || T > UNROLL_FULL_TRIP || T * nbody > UNROLL_FULL_BODY)
        T = 0;
    for (int m = 0; T && m < fn->nins; m++)
        if ((m < L.lo || m >= L.hi) &&
            ins_reads(&fn->ins[m], fn->ins[L.cmp_ins].dst))
            T = 0;
    /* Nor a body that calls the runtime -- a soft-float `double`, a
     * 64-bit divide -- which unr_find's IR_CALL test does not see. The
     * test and the branch saved are nothing beside the calls, and
     * lgamma's eight Lanczos terms grew by 288 bytes on Thumb for it. */
    for (int m = L.body_lo; T && m < L.cmp_ins; m++)
        if (target_op_calls_helper(&fn->ins[m]))
            T = 0;
    /* How many copies: the most, a power of two up to eight, that keep
     * U * body within the budget. A short body pays most of its cost on
     * the test and the branch, so it gets more of them; a long one
     * already amortises them and would only grow the function.
     *
     * The budget was 32 (a table: 8 copies to 4 instructions, 4 to 8, 2
     * to 16), which gave a CRC's ten-instruction body two copies and a
     * test every other byte. At 96 it gets eight: the workload's crc ran
     * 8% (RV32) to 17% (M4, x86-64) fewer instructions, the matrix 7-8%,
     * nothing ran more, and -O2 code grew 2.4-3.9%. -Os does not unroll.
     * More than eight copies is worse, not better: the leftover
     * iterations, up to U - 1 of them, run in the original loop, and
     * sixteen copies of the matrix's 24-trip loop left a third of the
     * work there (+7% to +14%). */
    int U = 1;
    if (T)
        U = (int)T;
    else
        while (U * 2 <= UNROLL_MAX_COPIES && U * 2 * nbody <= UNROLL_BUDGET)
            U *= 2;
    if (U < 2) {
        free_defs(&d); free(order); free(l2b);
        free_cfg(bb, nbb);
        return 0;
    }

    /* Every value the body computes is renamed, copy by copy, so the
     * copies are straight-line SSA that folding and value numbering can
     * see through. Without it each copy would end by writing the loop's
     * carried temp and the next would read it back -- a dependency chain
     * that exists only because two copies share a name. */
    int nvr = fn->nvregs;
    /* The last copy writes the original names, so that the block leaves
     * every value where the loop's own body would -- but only the values
     * that outlive the body need that: the ones the loop carries, tests,
     * or reads after it. A temp that dies inside the body gets a fresh
     * name in the last copy as in every other. Otherwise its original
     * name is written twice, by that copy and by the remainder loop, and
     * the backends' single-use fusions (`ldr [xn, xm, lsl #2]` is a
     * shift, an add and a load with one use each) refuse it in both:
     * `bounds` in tests/bench ran 5% MORE instructions unrolled. */
    char *local = xmalloc((size_t)(nvr ? nvr : 1));
    for (int v = 0; v < nvr; v++)
        local[v] = d.cnt[v] == 1 && d.ins[v] >= L.body_lo &&
                   d.ins[v] < L.cmp_ins;
    for (int m = 0; m < fn->nins; m++) {
        struct unr_local ul = { local, &d, m, L.cmp_ins, nvr };
        each_read(&fn->ins[m], unr_local_cb, &ul);
    }
    int Lunroll = fn->nlabels++;
    int Lexit = fn->nlabels++;
    int lineh = fn->ins[L.lo].line, colh = fn->ins[L.lo].col;
    int brw = fn->ins[L.hi - 1].w, brs = fn->ins[L.hi - 1].sign;

    struct ibuf nb = { 0, 0, 0 };
    int *newpos = fn->var_scope_lo
        ? xmalloc((size_t)(fn->nins + 1) * sizeof *newpos) : NULL;
    int cstart = 0, cend = 0;           /* where a full unroll's copies are */
    for (int n = 0; n < fn->nins; n++) {
        /* Recorded BEFORE the insertions: a local whose scope began at
         * the header must cover the copies too, or coalesce_locals is
         * free to give its slot to something that overlaps them. */
        if (newpos) newpos[n] = nb.n;
        if (T) {
            /* the header's label, then the copies in place of the loop:
             * its body, its compare and its back edge */
            if (n > L.lo && n < L.hi)
                continue;
            *ib_push(&nb) = fn->ins[n];
            if (n == L.lo) {
                cstart = nb.n;
                unr_copies(fn, &nb, &L, U, local, nvr);
                cend = nb.n;
                free(local);
            }
            continue;
        }
        if (n == L.lo) {
            struct ir_ins *p;
            int kU = fn->nvregs++;
            /* ---- once, on the way in: is there a full run to do? ----
             *
             * lim = bound - U*step, and the copies run while iv <= lim:
             * then iv + U*step <= bound, so every one of the U
             * iterations is one the loop would have run. That holds only
             * if the subtraction did not wrap, which `lim < bound` tests
             * (U*step is positive), at the loop's own signedness -- a
             * signed `iv < bound` loop, or unsigned for `iv != bound`,
             * whose walk is upward to its end. Both tests are paid once;
             * each run of copies then re-tests with one compare, where
             * re-deriving the count (`bound - iv >= U*step`) took a
             * subtract and a compare -- four instructions with x86's
             * two-operand subtract. A loop that fails either test, an
             * `iv != bound` walk that wraps included, runs as the
             * ORIGINAL loop, not the exit: the loop is a rotated one, a
             * do-while, and what it does when entered is what it always
             * did. The subtraction is the machine's, wrapping: that is
             * what the `lim < bound` test reads, and why nothing here may
             * assume a signed difference cannot overflow. */
            int ls = fn->ins[L.cmp_ins].pred == B_LT
                     ? fn->ins[L.cmp_ins].sign : 0;
            int lim = fn->nvregs++, e0 = fn->nvregs++, e1 = fn->nvregs++;
            p = unr_emit(&nb, IR_CONST, lineh, colh);
            p->dst = kU; p->imm = U * L.step; p->w = L.w;   /* U iterations' worth */
            p = unr_emit(&nb, IR_SUB, lineh, colh);
            p->dst = lim; p->a = L.bound; p->b = kU; p->w = L.w; p->sign = ls;
            p = unr_emit(&nb, IR_CMP, lineh, colh);
            p->dst = e0; p->a = lim; p->b = L.bound;
            p->pred = B_LT; p->w = L.w; p->sign = ls;
            p = unr_emit(&nb, IR_BRZ, lineh, colh);
            p->a = e0; p->label = L.Lh; p->w = brw; p->sign = brs;
            p = unr_emit(&nb, IR_CMP, lineh, colh);
            p->dst = e1; p->a = L.iv; p->b = lim;
            p->pred = B_LE; p->w = L.w; p->sign = ls;
            p = unr_emit(&nb, IR_BRZ, lineh, colh);
            p->a = e1; p->label = L.Lh; p->w = brw; p->sign = brs;

            /* ---- the copies ---- */
            p = unr_emit(&nb, IR_LABEL, lineh, colh);
            p->label = Lunroll;
            unr_copies(fn, &nb, &L, U, local, nvr);
            free(local);

            /* ---- the back edge: a compare and a branch per U iterations ----
             *
             * iv only grew by U*step from a value <= lim, so it is <=
             * bound and the same compare stays exact. */
            int e2 = fn->nvregs++;
            p = unr_emit(&nb, IR_CMP, lineh, colh);
            p->dst = e2; p->a = L.iv; p->b = lim;
            p->pred = B_LE; p->w = L.w; p->sign = ls;
            p = unr_emit(&nb, IR_BRNZ, lineh, colh);
            p->a = e2; p->label = Lunroll; p->w = brw; p->sign = brs;

            /* Out of full runs. iv <= bound, so anything left is a
             * partial one -- which is what the original loop below is. */
            int c2 = fn->nvregs++;
            p = ib_push(&nb);
            *p = fn->ins[L.cmp_ins];
            p->dst = c2;
            p = unr_emit(&nb, IR_BRNZ, lineh, colh);
            p->a = c2; p->label = L.Lh; p->w = brw; p->sign = brs;
            p = unr_emit(&nb, IR_JMP, lineh, colh);
            p->label = Lexit;
        }
        if (n == L.hi) {
            struct ir_ins *p = ib_push(&nb);
            p->op = IR_LABEL; p->label = Lexit;
            p->line = lineh; p->col = colh; p->synth = 1;
        }
        *ib_push(&nb) = fn->ins[n];
    }
    if (!T && L.hi == fn->nins) {      /* the loop ends the function */
        struct ir_ins *p = ib_push(&nb);
        p->op = IR_LABEL; p->label = Lexit;
        p->line = lineh; p->col = colh; p->synth = 1;
    }
    if (newpos) {
        newpos[fn->nins] = nb.n;
        for (int v = 0; v < fn->nvars; v++) {
            int lo = fn->var_scope_lo[v], hi = fn->var_scope_hi[v];
            /* A full unroll deleted the loop's own instructions, and a
             * scope that began or ended among them now covers all of the
             * copies that replaced them: from the first to past the
             * last. */
            if (T && lo > L.lo && lo < L.hi) fn->var_scope_lo[v] = cstart;
            else if (lo >= 0 && lo <= fn->nins) fn->var_scope_lo[v] = newpos[lo];
            if (T && hi > L.lo && hi < L.hi) fn->var_scope_hi[v] = cend;
            else if (hi >= 0 && hi <= fn->nins) fn->var_scope_hi[v] = newpos[hi];
        }
        free(newpos);
    }
    free(fn->ins);
    fn->ins = nb.p; fn->nins = nb.n; fn->cap = nb.cap;
    free_defs(&d); free(order); free(l2b);
    free_cfg(bb, nbb);
    if (L.Lh >= 0 && L.Lh < nseen)
        seen[L.Lh] = 1;
    /* ...and the block of copies, which ends in a conditional branch
     * back to itself: a loop to unrx_find, whose copies would be copied
     * again */
    if (!T && Lunroll < nseen)
        seen[Lunroll] = 1;
    if (remarks_on() && fn->src && T)
        remark_add("unroll", "unrolled", fn->name, "constant-trip-count",
                   fn->file, lineh ? lineh : fn->line,
                   "all %d iterations of a %d-instruction body, no loop "
                   "left", U, nbody);
    else if (remarks_on() && fn->src)
        remark_add("unroll", "unrolled", fn->name, "counted-loop",
                   fn->file, lineh ? lineh : fn->line,
                   "%d copies of a %d-instruction body, the original kept "
                   "for the remainder", U, nbody);
    return 1;
}

/* ==== loops whose trip count is not known =================================
 *
 * `while (*pp && (*pp)->key < key) pp = &(*pp)->next;` -- a list walk, a
 * search, a scan for a terminator -- has no count to divide by U: it
 * leaves when its data says so, from the middle of its body as well as at
 * its test. Such a loop is unrolled by keeping every exit: U copies of the
 * body, each with its own exits, and only the last ending in the back
 * edge:
 *
 *   Lh:  body   if (!a) goto X   ...   if (!c) goto out     <- copy 1
 *        ...
 *        body   if (!a) goto X   ...   if (c) goto Lh       <- copy U
 *   out:
 *
 * The values the loop carries are renamed from copy to copy, as the
 * counted unroller renames them, so each copy reads what the one before
 * it computed: a pointer chase alternates registers instead of copying
 * the new node into the old one's every element -- the `mov` in EmbCC's
 * list walk that clang's has not. The last copy writes the original
 * names, so the back edge carries them as the loop always did. An exit
 * taken earlier finds the carried values under their copies' names, and
 * goes through a stub that moves them back and jumps where the exit
 * went; the stubs follow the loop, which falls out over them.
 *
 * What it saves per iteration is the taken branch back, three times in
 * four, and the copy of each carried value. Not a loop that calls (see
 * unr_find), nor a long one: the branches a copy keeps are most of a
 * short body's cost, and none of a long one's.
 */

/* Four copies of a body of at most six instructions, its branches
 * counted. The list walk is six, and four copies of it are what clang
 * makes: two left the Cortex-M4 at 1.21 of clang's cycles where four
 * reach 1.06. Longer bodies, copied twice, bought the workload nothing
 * and lib/libc's -O2 code 3% more (6.6% in all, 3.3% now). */
#define UNRX_MAX_BODY 6
#define UNRX_COPIES   4

struct unrxloop {
    int lo, hi;        /* [lo, hi): the header's label .. the back edge */
    int Lh;
    int nexit;         /* branches out of the body, the back edge's not counted */
};

/* Does anything but instruction `skip` name label L? */
static int unr_label_named(struct ir_func *fn, int L, int skip)
{
    for (int m = 0; m < fn->nins; m++) {
        if (m == skip)
            continue;
        struct unr_lblctx lc = { L, 0 };
        if (fn->ins[m].op == IR_LABELADDR && fn->ins[m].label == L)
            return 1;
        each_label(fn, &fn->ins[m], unr_lbl_cb, &lc);
        if (lc.hit)
            return 1;
    }
    return 0;
}

/* The exit code a loop holds in its middle: `for (;;) { if (*p == c)
 * return p; ... }` lays the return out between the loop's blocks, so the
 * loop's instructions are not one range. Such a run of blocks, which the
 * loop does not come back from, moves to after the function's last
 * instruction, and the branch around it is turned over to branch to it:
 *
 *   brz c, L  ; exit code ; ret  ; L: ...    becomes
 *   brnz c, X ; L: ...  ...  X: exit code ; ret
 *
 * which is also the loop's better layout -- it stays in a straight line
 * and branches out to leave. Up to UNRX_MAX_RUNS of them. */
#define UNRX_MAX_RUNS 8

struct unrx_runs { int n, lo[UNRX_MAX_RUNS], hi[UNRX_MAX_RUNS]; };

static int unrx_find(struct ir_func *fn, struct bb *bb, int nbb, int *order,
                     int norder, const int *l2b, struct defs *d, char *in,
                     struct unrxloop *L, struct unrx_runs *R,
                     const char *seen, int nseen)
{
    (void)d;
    enum ir_op last = fn->nins ? fn->ins[fn->nins - 1].op : IR_LABEL;
    /* the moved exit code goes after the function's last instruction,
     * which must not fall into it */
    if (last != IR_RET && last != IR_JMP && last != IR_UD2 && last != IR_IGOTO)
        return 0;
    for (int oi = norder - 1; oi >= 0; oi--) {
        int h = order[oi];
        if (h == 0 || bb[h].end <= bb[h].start ||
            fn->ins[bb[h].start].op != IR_LABEL)
            continue;
        int Lh = fn->ins[bb[h].start].label;
        if (Lh < 0 || Lh >= nseen || seen[Lh])
            continue;
        int latch = -1, nback = 0;
        for (int q = 0; q < bb[h].npred; q++) {
            int p = bb[h].pred[q];
            if (bb_dominates(bb, h, p)) {
                latch = p; nback++;
            }
        }
        if (nback != 1 || bb[latch].end <= bb[latch].start)
            continue;
        /* a test at the bottom, or `for (;;)`'s jump back, the loop
         * leaving only from its body */
        const struct ir_ins *br = &fn->ins[bb[latch].end - 1];
        if ((br->op != IR_BRNZ && br->op != IR_BRZ && br->op != IR_JMP) ||
            br->label != Lh)
            continue;
        memset(in, 0, (size_t)nbb);
        loop_body(bb, nbb, h, latch, in);
        L->lo = bb[h].start; L->hi = bb[latch].end;
        /* every block of the loop in [lo, hi); any other block there is
         * exit code to move out (unrx_sink) */
        int ok = 1;
        R->n = 0;
        for (int b = 0; b < nbb && ok; b++) {
            int inside = bb[b].start >= L->lo && bb[b].end <= L->hi;
            if (in[b] && !inside)
                ok = 0;
            else if (!in[b] && inside && bb[b].end > bb[b].start) {
                if (R->n && R->hi[R->n - 1] == bb[b].start)
                    R->hi[R->n - 1] = bb[b].end;      /* the run goes on */
                else if (R->n == UNRX_MAX_RUNS)
                    ok = 0;
                else {
                    R->lo[R->n] = bb[b].start;
                    R->hi[R->n++] = bb[b].end;
                }
            }
        }
        /* and an instruction after it, for its last exit to fall into --
         * unless it ends in a jump back, and only leaves from its body */
        if (!ok || (L->hi >= fn->nins && br->op != IR_JMP))
            continue;
        for (int r = 0; r < R->n && ok; r++) {
            /* a run that cannot fall out of its end, after a block that
             * branches around it or does not fall into it at all */
            enum ir_op e = fn->ins[R->hi[r] - 1].op;
            const struct ir_ins *p = &fn->ins[R->lo[r] - 1];
            if (e != IR_RET && e != IR_JMP && e != IR_UD2)
                ok = 0;
            else if (p->op == IR_BRZ || p->op == IR_BRNZ)
                ok = fn->ins[R->hi[r]].op == IR_LABEL &&
                     fn->ins[R->hi[r]].label == p->label;
            else if (p->op != IR_JMP && p->op != IR_RET && p->op != IR_UD2)
                ok = 0;
        }
        if (!ok)
            continue;
        int nbody = 0, nexit = 0, r = 0;
        for (int n = L->lo + 1; n < L->hi - 1 && ok; n++) {
            if (r < R->n && n == R->lo[r]) {
                n = R->hi[r++] - 1;               /* not the loop's */
                continue;
            }
            const struct ir_ins *i = &fn->ins[n];
            if (i->op == IR_LABEL) {
                /* rotation's empty label, which nothing reaches -- or the
                 * one after a run, which only the branch around the run
                 * reaches until unrx_sink turns that over (and the loop
                 * is looked at again) */
                int after_run = 0;
                for (int k = 0; k < R->n; k++)
                    if (n == R->hi[k])
                        after_run = 1;
                if (!after_run && unr_label_named(fn, i->label, -1))
                    ok = 0;
                continue;
            }
            nbody++;
            if (i->op == IR_BRZ || i->op == IR_BRNZ) {
                /* out of the loop, or it is not a body of one block */
                int around = 0;            /* the branch around a run */
                for (int k = 0; k < R->n; k++)
                    if (n == R->lo[k] - 1)
                        around = 1;
                if (!around &&
                    (i->label < 0 || i->label >= fn->nlabels ||
                     l2b[i->label] < 0 || in[l2b[i->label]]))
                    ok = 0;
                nexit++;
                continue;
            }
            /* the counted unroller's reasons, the same here */
            if (!unr_copyable(i) || i->op == IR_CALL ||
                target_op_calls_helper(i))
                ok = 0;
        }
        if (!ok || nbody == 0 || nbody > UNRX_MAX_BODY)
            continue;
        L->Lh = Lh;
        L->nexit = nexit;
        return nbody;
    }
    return 0;
}

/* Move the runs out (see above): the function less them, then each run
 * after its last instruction, under a label of its own that the branch
 * before it, turned over, goes to. A local whose scope held any of a
 * run's instructions keeps its slot to the end of the function, where
 * they now are. */
static void unrx_sink(struct ir_func *fn, const struct unrx_runs *R)
{
    struct ibuf nb = { 0, 0, 0 };
    int *lbl = xmalloc((size_t)R->n * sizeof *lbl);
    int *newpos = xmalloc((size_t)(fn->nins + 1) * sizeof *newpos);
    for (int r = 0; r < R->n; r++)
        lbl[r] = fn->ins[R->lo[r]].op == IR_LABEL ? fn->ins[R->lo[r]].label
                                                  : fn->nlabels++;
    int r = 0;
    for (int n = 0; n < fn->nins; n++) {
        newpos[n] = nb.n;
        if (r < R->n && n == R->lo[r]) {
            n = R->hi[r++] - 1;
            continue;
        }
        struct ir_ins *q = ib_push(&nb);
        *q = fn->ins[n];
        for (int k = 0; k < R->n; k++)
            if (n == R->lo[k] - 1 && (q->op == IR_BRZ || q->op == IR_BRNZ)) {
                q->op = q->op == IR_BRZ ? IR_BRNZ : IR_BRZ;
                q->label = lbl[k];
            }
    }
    newpos[fn->nins] = nb.n;
    int end = nb.n;
    for (r = 0; r < R->n; r++) {
        if (fn->ins[R->lo[r]].op != IR_LABEL) {
            struct ir_ins *p = unr_emit(&nb, IR_LABEL, fn->ins[R->lo[r]].line,
                                        fn->ins[R->lo[r]].col);
            p->label = lbl[r];
        }
        for (int n = R->lo[r]; n < R->hi[r]; n++) {
            newpos[n] = nb.n;
            *ib_push(&nb) = fn->ins[n];
        }
    }
    if (fn->var_scope_lo)
        for (int v = 0; v < fn->nvars; v++) {
            int lo = fn->var_scope_lo[v], hi = fn->var_scope_hi[v];
            int held = 0;
            for (int k = 0; k < R->n; k++)
                if (lo < R->hi[k] && hi > R->lo[k])
                    held = 1;
            if (lo >= 0 && lo <= fn->nins)
                fn->var_scope_lo[v] = lo < fn->nins ? newpos[lo] : end;
            if (held)
                fn->var_scope_hi[v] = nb.n;
            else if (hi >= 0 && hi <= fn->nins)
                fn->var_scope_hi[v] = hi < fn->nins ? newpos[hi] : end;
        }
    free(lbl);
    free(newpos);
    free(fn->ins);
    fn->ins = nb.p; fn->nins = nb.n; fn->cap = nb.cap;
}

/* The stubs an exit from the middle goes through: each moves the carried
 * values that are under a copy's name back to their own, and jumps on. */
struct unrx_stub { int label, target, n, *v, *from; };
struct unrx_stubs { struct unrx_stub *s; int n, cap; };

/* Where an exit to `target` from here goes: target itself when every
 * carried value is under its own name, else a stub -- the same one for
 * the same target and the same names. */
static int unrx_target(struct ir_func *fn, struct unrx_stubs *S, int target,
                       const int *cur, const char *carried, int nvr)
{
    int n = 0;
    for (int v = 0; v < nvr; v++)
        if (carried[v] && cur[v] >= 0)
            n++;
    if (!n)
        return target;
    for (int k = 0; k < S->n; k++) {
        const struct unrx_stub *st = &S->s[k];
        int same = st->target == target && st->n == n;
        for (int j = 0; j < st->n && same; j++)
            if (cur[st->v[j]] != st->from[j])
                same = 0;
        if (same)
            return st->label;
    }
    if (S->n == S->cap) {
        S->cap = S->cap ? 2 * S->cap : 8;
        S->s = xrealloc(S->s, (size_t)S->cap * sizeof *S->s);
    }
    struct unrx_stub *st = &S->s[S->n++];
    st->label = fn->nlabels++;
    st->target = target;
    st->n = 0;
    st->v = xmalloc((size_t)n * sizeof *st->v);
    st->from = xmalloc((size_t)n * sizeof *st->from);
    for (int v = 0; v < nvr; v++)
        if (carried[v] && cur[v] >= 0) {
            st->v[st->n] = v;
            st->from[st->n++] = cur[v];
        }
    return st->label;
}

static int unrx_one(struct ir_func *fn, char *seen, int nseen)
{
    if (fn->nins == 0)
        return 0;
    int nbb, *l2b;
    struct bb *bb = build_cfg(fn, &nbb, &l2b);
    int *order = xmalloc((size_t)(nbb ? nbb : 1) * sizeof *order), norder;
    compute_rpo(bb, nbb, order, &norder);
    if (norder != nbb) {
        free(order); free(l2b);
        free_cfg(bb, nbb);
        return 0;
    }
    compute_idom(bb, order, norder);
    struct defs d;
    compute_defs(fn, &d);
    char *in = xmalloc((size_t)(nbb ? nbb : 1));
    struct unrxloop L;
    struct unrx_runs R;
    int nbody = unrx_find(fn, bb, nbb, order, norder, l2b, &d, in, &L, &R,
                          seen, nseen);
    free(in);
    if (nbody && R.n) {
        /* the exit code out of the way first; the loop is found again,
         * a range of its own, on the next call */
        free_defs(&d); free(order); free(l2b);
        free_cfg(bb, nbb);
        if (fn->nlabels + R.n > nseen)
            return 0;
        unrx_sink(fn, &R);
        return 1;
    }
    int U = UNRX_COPIES;
    /* a label for each stub there can be, and the exit's, still inside
     * `seen` -- or the loop is left alone */
    if (!nbody || fn->nlabels + U * (L.nexit + 1) + 1 > nseen) {
        free_defs(&d); free(order); free(l2b);
        free_cfg(bb, nbb);
        return 0;
    }

    int nvr = fn->nvregs;
    /* local: what the counted unroller calls local, a temp whose every
     * read is in the body after its one write -- a copy's own, renamed
     * in each. carried: every other temp the body writes, which the back
     * edge brings round or something after the loop reads: renamed in
     * the copies but the last, and moved back by the stubs. */
    char *carried = xcalloc((size_t)(nvr ? nvr : 1), 1);
    char *local = xmalloc((size_t)(nvr ? nvr : 1));
    int *cw = xcalloc((size_t)(nvr ? nvr : 1), sizeof *cw);
    char *cf = xcalloc((size_t)(nvr ? nvr : 1), 1);
    for (int v = 0; v < nvr; v++)
        local[v] = d.cnt[v] == 1 && d.ins[v] > L.lo && d.ins[v] < L.hi - 1;
    for (int m = 0; m < fn->nins; m++) {
        struct unr_local ul = { local, &d, m, L.hi, nvr };
        each_read(&fn->ins[m], unr_local_cb, &ul);
    }
    for (int n = L.hi - 2; n > L.lo; n--) {      /* the first write's width */
        int t = def_target(&fn->ins[n]);
        if (t >= fn->nvars && t < nvr && !local[t]) {
            carried[t] = 1;
            cw[t] = ir_result_w(&fn->ins[n]);
            cf[t] = (char)ir_result_flt(&fn->ins[n]);
        }
    }

    int lineh = fn->ins[L.lo].line, colh = fn->ins[L.lo].col;
    /* where the test at the bottom falls out to; a loop that jumps back
     * has no such exit */
    int jmp_back = fn->ins[L.hi - 1].op == IR_JMP;
    int exit_label = !jmp_back && fn->ins[L.hi].op != IR_LABEL;
    int Lexit = jmp_back ? -1
              : exit_label ? fn->nlabels++ : fn->ins[L.hi].label;

    struct unrx_stubs S = { NULL, 0, 0 };
    struct ibuf nb = { 0, 0, 0 };
    int *newpos = fn->var_scope_lo
        ? xmalloc((size_t)(fn->nins + 1) * sizeof *newpos) : NULL;
    int cstart = 0, cend = 0;
    int *cur = xmalloc((size_t)(nvr ? nvr : 1) * sizeof *cur);
    for (int n = 0; n < fn->nins; n++) {
        if (newpos) newpos[n] = nb.n;
        if (n > L.lo && n < L.hi)
            continue;                 /* the body: its copies are below */
        if (n == L.hi && exit_label) {
            struct ir_ins *p = unr_emit(&nb, IR_LABEL, lineh, colh);
            p->label = Lexit;
        }
        *ib_push(&nb) = fn->ins[n];
        if (n != L.lo)
            continue;
        cstart = nb.n;
        for (int v = 0; v < nvr; v++)
            cur[v] = -1;
        for (int c = 0; c < U; c++)
            for (int n2 = L.lo + 1; n2 < L.hi; n2++) {
                if (fn->ins[n2].op == IR_LABEL)
                    continue;
                if (fn->ins[n2].op == IR_JMP && c < U - 1)
                    continue;         /* into the next copy */
                struct ir_ins q = fn->ins[n2];
                ins_own_args(&q);
                struct lcopy lc = { cur, nvr, 0 };
                each_read(&q, lcopy_cb, &lc);
                if (q.op == IR_JMP) {
                    *ib_push(&nb) = q;    /* the last copy's, back to Lh */
                    continue;
                }
                if (q.op == IR_BRZ || q.op == IR_BRNZ) {
                    if (n2 == L.hi - 1 && c < U - 1) {
                        /* the back edge, in a copy that is not the
                         * last: the loop goes on into the next copy, and
                         * leaves where it would have fallen out */
                        q.op = q.op == IR_BRNZ ? IR_BRZ : IR_BRNZ;
                        q.label = unrx_target(fn, &S, Lexit, cur, carried,
                                              nvr);
                    } else if (n2 != L.hi - 1) {
                        q.label = unrx_target(fn, &S, q.label, cur, carried,
                                              nvr);
                    }
                    *ib_push(&nb) = q;
                    continue;
                }
                int t = def_target(&q);
                if (t >= fn->nvars && t < nvr) {
                    if (c == U - 1 && !local[t]) {
                        cur[t] = -1;
                    } else {
                        q.dst = fn->nvregs++;
                        cur[t] = q.dst;
                    }
                }
                *ib_push(&nb) = q;
            }
        /* The stubs, right after the loop: a value a stub reads is live
         * from its copy to the stub, and the stubs at the function's end
         * made every one of them live across whatever followed the loop
         * -- an outer loop's every iteration, on the Cortex-M4 spilled.
         * The test at the bottom falls out over them. */
        if (S.n && !jmp_back) {
            struct ir_ins *p = unr_emit(&nb, IR_JMP, lineh, colh);
            p->label = Lexit;
        }
        for (int k = 0; k < S.n; k++) {
            const struct unrx_stub *st = &S.s[k];
            struct ir_ins *p = unr_emit(&nb, IR_LABEL, lineh, colh);
            p->label = st->label;
            for (int j = 0; j < st->n; j++) {
                /* copies' names are fresh, never another move's
                 * destination, so one after another is the parallel move */
                p = unr_emit(&nb, IR_MOV, lineh, colh);
                p->dst = st->v[j];
                p->a = st->from[j];
                p->b = -1;
                p->w = cw[st->v[j]];
                p->flt = cf[st->v[j]];
            }
            p = unr_emit(&nb, IR_JMP, lineh, colh);
            p->label = st->target;
            free(st->v);
            free(st->from);
        }
        cend = nb.n;
    }
    if (newpos) {
        newpos[fn->nins] = nb.n;
        for (int v = 0; v < fn->nvars; v++) {
            int lo = fn->var_scope_lo[v], hi = fn->var_scope_hi[v];
            /* a scope that began or ended in the body covers the copies */
            if (lo > L.lo && lo < L.hi) fn->var_scope_lo[v] = cstart;
            else if (lo >= 0 && lo <= fn->nins) fn->var_scope_lo[v] = newpos[lo];
            if (hi > L.lo && hi < L.hi) fn->var_scope_hi[v] = cend;
            else if (hi >= 0 && hi <= fn->nins) fn->var_scope_hi[v] = newpos[hi];
        }
        free(newpos);
    }
    int nstub = S.n;
    free(S.s);
    free(cur); free(carried); free(local); free(cw); free(cf);
    free(fn->ins);
    fn->ins = nb.p; fn->nins = nb.n; fn->cap = nb.cap;
    free_defs(&d); free(order); free(l2b);
    free_cfg(bb, nbb);
    seen[L.Lh] = 1;
    if (remarks_on() && fn->src)
        remark_add("unroll", "unrolled", fn->name, "exits-kept",
                   fn->file, lineh ? lineh : fn->line,
                   "%d copies of a %d-instruction body whose trip count is "
                   "not known, each keeping its %d exit%s; %d stub%s move "
                   "the carried values back", U, nbody,
                   L.nexit + !jmp_back, L.nexit + !jmp_back == 1 ? "" : "s",
                   nstub, nstub == 1 ? "" : "s");
    return 1;
}

int pass_unroll(struct ir_func *fn)
{
    /* One pass over each loop. The unrolled block is a loop too -- it
     * ends in a jump back to its own label -- but its latch is a JUMP
     * and the recognizer wants a conditional back edge, so it does not
     * match; the ORIGINAL loop still does, and `seen` is what stops it
     * being unrolled into its own remainder over and over. */
    int cap = fn->nlabels + 64;
    char *seen = xcalloc((size_t)cap, 1);
    int changed = 0, guard = 0;
    while (guard++ < 16 && fn->nlabels + 2 <= cap &&
           unroll_one(fn, seen, cap))
        changed = 1;
    /* then the loops no count can be found for, every exit kept */
    for (guard = 0; guard < 16 && !getenv("EMBCC_NO_UNROLL_EXITS") &&
                    unrx_one(fn, seen, cap); guard++)
        changed = 1;
    free(seen);
    return changed;
}
