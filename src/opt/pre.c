/* ==== partial redundancy elimination (-O2) ================================= *
 *
 * Local value numbering reuses a value computed earlier in the same
 * block. Global CSE reuses one computed in a block that DOMINATES this
 * one. Between them they answer "was this computed on EVERY path to
 * here", and there is a third case neither can see:
 *
 *      if (c) sink(a * b);       <- computed on one path
 *      return a * b;             <- and again here, on both
 *
 * The second multiply is redundant when `c` was true and new when it was
 * false, so it is PARTIALLY redundant: no single earlier computation
 * dominates it, and both earlier passes decline. Compute it on the path
 * that lacked it and the partial redundancy becomes a full one; the
 * bottom copy is then a move.
 *
 * ---- why this is not speculation ------------------------------------
 *
 * Inserting a computation on a path that would not have performed it is
 * work invented, and the only thing that stops that is WHERE the
 * insertion goes: at the end of a predecessor whose sole successor is
 * the redundant block. Every execution reaching that point reaches the
 * block, so the expression runs exactly as often after as before -- once
 * per path instead of once on some and twice on others. The count never
 * goes up on any path, including a path through a loop, which is why
 * this needs no loop reasoning at all.
 *
 * A predecessor with two successors gives no such point: anything put
 * before its branch runs for the other successor too. That edge is
 * CRITICAL, and it is also the shape the example above has -- `if (c)`
 * branches straight to the merge. Refusing it would mean refusing very
 * nearly every real opportunity, so instead the edge is SPLIT: the
 * branch is retargeted to a new block that holds the insertion and jumps
 * on to the original. The new block is on exactly one edge, so the
 * invariant above is restored rather than bent.
 *
 * ---- and the half that pays for itself -------------------------------
 *
 * The result of an insertion is a vreg written in more than one
 * predecessor -- a phi in all but name, and the same shape irgen
 * already emits for a `?:`. Copy propagation declines to touch a
 * multiply-assigned vreg (it checks), so those moves reach the register
 * allocator to be coalesced.
 *
 * It also means the value is now held under ONE name on every path
 * while being dominated by no single definition -- so global CSE, which
 * asks about dominance, cannot see it, and the insertion would buy
 * nothing past the block it was made for. The fix is to ask the weaker
 * question the analysis here already answers: has every path to this
 * point passed a definition, under one name? Availability implies
 * dominance for a value with one definition, so this strictly contains
 * what global CSE does, and it is what makes the merged name usable
 * downstream.
 *
 * The two halves are one mechanism, not two features: the reuse exists
 * to make an insertion worth making, and on its own it can only find
 * what global CSE already finds. "What is worth inserting, and what
 * this is worth" below has the measurement, including the part where
 * that turns out to be nothing at all on lib/libc.
 *
 * ---- what may move ---------------------------------------------------
 *
 * The expressions global CSE numbers, minus divide and remainder, whose
 * fault on a zero divisor is a path-dependent EFFECT and not a value.
 * Every operand must be single-assignment (`vn_stable`, for the reason
 * it guards CSE), so a value never stops being its value and
 * availability has nothing to kill -- a plain forward must-analysis.
 *
 * An operand also has to exist where the copy goes:
 *
 *   - a literal is rematerialised there, one instruction, no question;
 *   - a read of a slot that is never written and never addressed has
 *     one value for the whole function, so it too is rematerialised.
 *     This case is not a detail: parameters stay in their slots here
 *     (mem2reg skips them -- their value is live on entry, so SSA has
 *     no version to seed a read with), so `a * b` on two paths is two
 *     multiplies of two DIFFERENT pairs of temps. Without recognising
 *     that both pairs read the same unwritten slots, the example at the
 *     top of this comment does not match itself;
 *   - anything else must be defined in a block that dominates the
 *     predecessor, which is what makes reading it there legal.
 */

#include "opt_int.h"

/* Where a new instruction goes at the end of block b: before its
 * terminator, which has to stay last. */
static int pre_tail(struct ir_func *fn, struct bb *bb, int b)
{
    int e = bb[b].end;
    if (e > bb[b].start) {
        enum ir_op op = fn->ins[e - 1].op;
        if (op == IR_JMP || op == IR_BRZ || op == IR_BRNZ ||
            op == IR_RET || op == IR_UD2 || op == IR_IGOTO ||
            op == IR_SWITCH)
            return e - 1;
    }
    return e;
}

/* Slots whose contents cannot change: never written, never addressed,
 * not volatile, every read plain and full width. A parameter that is
 * only ever read is the overwhelmingly common case. */
static char *pre_invariant_slots(struct ir_func *fn)
{
    int n = fn->nvars;
    char *inv = xmalloc((size_t)(n ? n : 1));
    for (int L = 0; L < n; L++)
        inv[L] = !fn->locals[L].is_volatile;
    for (int i = 0; i < fn->nins; i++) {
        struct ir_ins *in = &fn->ins[i];
        if ((in->op == IR_ADDR || in->op == IR_LDVAR) &&
            in->a >= 0 && in->a < n) {
            if (in->op == IR_ADDR || in->vol)
                inv[in->a] = 0;
        }
        if (in->op == IR_STVAR && in->dst >= 0 && in->dst < n)
            inv[in->dst] = 0;
    }
    return inv;
}

/* The instruction defining v, when v has exactly one definition and it
 * is a rematerialisable read of an unwritten slot. */
static struct ir_ins *pre_slot_read(struct ir_func *fn, struct defs *d,
                                    const char *inv, int v)
{
    if (v < 0 || v >= fn->nvregs || d->cnt[v] != 1)
        return NULL;
    int def = d->ins[v];
    if (def < 0)
        return NULL;
    struct ir_ins *in = &fn->ins[def];
    if (in->op != IR_LDVAR || in->vol ||
        in->a < 0 || in->a >= fn->nvars || !inv[in->a])
        return NULL;
    return in;
}

/* Is vreg v readable at the end of block p? A parameter's vreg is live
 * throughout; anything else needs its defining block to dominate p, and
 * if that block IS p, the definition must precede the insertion. */
static int pre_have(struct ir_func *fn, struct defs *d, struct bb *bb,
                    int nbb, const int *i2b, int v, int p, int at)
{
    if (v < 0)
        return 1;                       /* absent operand */
    if (v >= fn->nvregs || d->cnt[v] != 1)
        return 0;
    int def = d->ins[v];
    if (def < 0)
        return 1;                       /* live on entry */
    int db = i2b[def];
    if (db < 0 || db >= nbb)
        return 0;
    if (db == p)
        return def < at;
    return bb_dominates(bb, db, p);
}

/* ---- what is worth inserting, and what this is worth --------------
 *
 * Reusing a value costs nothing: an instruction becomes a move, and
 * copy propagation usually deletes even that. INSERTING is not free. It
 * costs a move in every predecessor that already held the value, plus
 * the one at the use, and buys back the whole expression on the paths
 * that recomputed it.
 *
 * Measured over the 84 files of lib/libc and lib/libcxx at -O2, .text
 * summed per target, against this same compiler with `-fno-pre`
 * (x86-64 247583, aarch64 444044):
 *
 *     insert nothing, reuse only        247583   444044
 *     insert multiplies  (what ships)   247583   444044
 *     ... or shifts and compares too    247613   444056
 *     insert every numberable op        247658   444204
 *
 * Read the first two rows carefully, because they say this pass does
 * not change one byte of that corpus, and the reason is worth more than
 * the result:
 *
 *   - no multiply insertion fires anywhere in it. The shape at the top
 *     of this comment is real, but it is not what libc is made of;
 *   - and with no insertion, the reuse half cannot beat global CSE.
 *     That is not luck, it is a theorem: with retreating edges dropped,
 *     an available name that has ONE definition is available only
 *     because every path passed that definition, which is what it means
 *     for it to dominate. Availability beats dominance exactly when the
 *     name has SEVERAL definitions, and the only thing that makes one
 *     is an insertion.
 *
 * So the 59 computations it removes there are 59 that global CSE would
 * have taken on the next round -- the final IR is instruction-for-
 * instruction identical either way, which is how this was confirmed
 * rather than assumed. Compile time over the same corpus is 1.30s
 * against 1.31s: free.
 *
 * It is kept, on at -O2, for the case it was built for and the tests
 * hold it to. It is restricted to multiplies so that where it does not
 * apply it cannot cost anything -- the two rows below the fold are what
 * a wider rule does, on both targets. A shift or a compare is one
 * instruction, so trading it for a move is a wash the longer live range
 * then loses.
 */
static int pre_worth_it(const struct ir_ins *i)
{
    return i->op == IR_MUL;
}

/* Which temp holds each key as a block is walked, and the inverse, so
 * that a write can cheaply un-name whatever the vreg used to hold.
 *
 * The inverse is not a convenience. A vreg written on two paths -- the
 * shape irgen emits for `?:` and for the va_arg result -- can carry a
 * key down one path and something else down the other, and a table that
 * only ever RECORDS names goes on believing the first long after the
 * second overwrote it. That is a wrong answer, not a missed one.
 *
 * `stamp` dates each entry so the table costs nothing to clear between
 * blocks: an entry from an older generation simply does not exist. */
struct pretab { int *st, *own, *stamp; int gen, nk, nv; };

static void pre_name(struct pretab *t, int k, int v)
{
    t->st[k] = v;
    if (v >= 0 && v < t->nv) { t->own[v] = k; t->stamp[v] = t->gen; }
}

/* Start a block from the availability computed for its entry. */
static void pre_enter(struct pretab *t, const int *in)
{
    t->gen++;
    for (int k = 0; k < t->nk; k++) { t->st[k] = -1; }
    for (int k = 0; k < t->nk; k++) if (in[k] >= 0) pre_name(t, k, in[k]);
}

/* One instruction's effect. Whatever it writes stops holding what it
 * held; a numbered instruction then names its key if nothing already
 * held it, and a MOV renames the key it copies -- that one overwrites,
 * because the new name is the one the other paths will agree on. */
static void pre_step(struct ir_func *fn, const int *keyidx, int i,
                     struct pretab *t)
{
    struct ir_ins *in = &fn->ins[i];
    int dst = in->dst, src = -1;
    if (dst >= 0 && dst < t->nv && t->stamp[dst] == t->gen) {
        int k0 = t->own[dst];
        if (k0 >= 0 && t->st[k0] == dst) t->st[k0] = -1;
        t->stamp[dst] = 0;                      /* names nothing now */
    }
    int k = keyidx[i];
    if (k >= 0) {
        if (t->st[k] < 0) pre_name(t, k, dst);
        return;
    }
    if (in->op == IR_MOV && dst >= 0 && (src = in->a) >= 0 && src < t->nv &&
        t->stamp[src] == t->gen && t->own[src] >= 0)
        pre_name(t, t->own[src], dst);
}

#define PRE_MAX_KEYS 400      /* the dataflow is blocks x keys */

static int pre_one(struct ir_func *fn)
{
    if (fn->nins == 0 || fn->nlabels == 0)
        return 0;
    int nbb, *l2b;
    struct bb *bb = build_cfg(fn, &nbb, &l2b);
    int *order = xmalloc((size_t)(nbb ? nbb : 1) * sizeof *order), norder;
    compute_rpo(bb, nbb, order, &norder);
    if (norder != nbb) {        /* unreachable blocks: dominance is partial */
        free(order); free(l2b); free_cfg(bb, nbb); return 0;
    }
    compute_idom(bb, order, norder);
    struct defs d;
    compute_defs(fn, &d);
    char *inv = pre_invariant_slots(fn);

    int *i2b = xmalloc((size_t)fn->nins * sizeof *i2b);
    for (int b = 0; b < nbb; b++)
        for (int i = bb[b].start; i < bb[b].end; i++)
            i2b[i] = b;

    /* An operand's identity. A read of an unwritten slot is keyed by the
     * SLOT, so the same read in two blocks -- two temps -- is one value. */
    int *pcan = xmalloc((size_t)(fn->nvregs ? fn->nvregs : 1) * sizeof *pcan);
    for (int v = 0; v < fn->nvregs; v++) {
        struct ir_ins *r = pre_slot_read(fn, &d, inv, v);
        pcan[v] = r ? -(1 << 20) - r->a : v;
    }

    /* The expressions worth numbering, and which one each instruction
     * computes. */
    struct vn *keys = NULL;
    int nk = 0, capk = 0;
    int *keyidx = xmalloc((size_t)fn->nins * sizeof *keyidx);
    for (int i = 0; i < fn->nins; i++) {
        keyidx[i] = -1;
        struct ir_ins *in = &fn->ins[i];
        struct vn k;
        /* Not a float operation. It would be sound (nothing here
         * speculates, and FENV_ACCESS is off), but allowing them changed
         * no function in lib/libc for Cortex-M, so the merge temps stay
         * as they were: written by integer operations and moves only. */
        if (in->dst < 0 || !gcse_numberable(in->op) || in->flt ||
            in->op == IR_DIV || in->op == IR_MOD ||
            !vn_stable(fn, &d, in) || !vn_key(in, 0, &k))
            continue;
        gcse_key_consts(fn, &d, &k);
        if (k.a >= 0) k.a = pcan[k.a];
        if (k.b >= 0) k.b = pcan[k.b];
        int found = -1;
        for (int j = 0; j < nk; j++)
            if (vn_eq(&keys[j], &k)) { found = j; break; }
        if (found < 0) {
            if (nk == PRE_MAX_KEYS)
                continue;
            if (nk == capk) {
                capk = capk ? capk * 2 : 32;
                keys = xrealloc(keys, (size_t)capk * sizeof *keys);
            }
            keys[nk] = k; found = nk++;
        }
        keyidx[i] = found;
    }
    if (nk == 0) {
        free(order); free(l2b); free(i2b); free(keyidx); free(keys);
        free(pcan); free(inv);
        free_cfg(bb, nbb); free_defs(&d); return 0;
    }

    /* Availability. Nothing kills an entry: every operand is
     * single-assignment, so a value computed stays that value. */
    int *aout = xmalloc((size_t)nbb * (size_t)nk * sizeof *aout);
    int *ain  = xmalloc((size_t)nbb * (size_t)nk * sizeof *ain);
    for (int i = 0; i < nbb * nk; i++) { aout[i] = -2; ain[i] = -1; }
    /* A RETREATING edge carries nothing.
     *
     * This is the whole soundness argument, so it is worth stating.
     * "Available in vreg v" has to mean v holds the value THIS execution
     * would compute, and around a cycle it does not: a loop body that
     * leaves its result in one name hands the next iteration the
     * PREVIOUS value, computed from operands that have since moved on.
     * An earlier draft of this pass let that through and turned a hash
     * loop into `%63 = mov %63`.
     *
     * Dropping every retreating edge in the meet fixes it, and the
     * argument is short: after the last retreating edge an execution
     * takes, the rest of its path is acyclic, so no definition site on
     * it runs twice and every name still holds what this pass through
     * put there. The target of that edge starts from nothing, so
     * nothing older can leak in.
     *
     * What it costs is availability at a loop header, where a value
     * from before the loop would otherwise be reusable inside it. That
     * case needs the definition to dominate the use, which is the
     * question global CSE already answers, so nothing is actually lost.
     *
     * It also makes this a DAG problem: one pass in reverse post-order
     * is the fixpoint, with no optimistic initialization to have to
     * distrust. */
    int nv = fn->nvregs ? fn->nvregs : 1;
    int *st = xmalloc((size_t)nk * sizeof *st);
    int *own = xmalloc((size_t)nv * sizeof *own);
    int *stamp = xcalloc((size_t)nv, sizeof *stamp);
    struct pretab tab = { st, own, stamp, 0, nk, fn->nvregs };
    for (int oi = 0; oi < nbb; oi++) {
        int b = order[oi];
        int *in = ain + (size_t)b * nk;
        for (int k = 0; k < nk; k++) in[k] = -2;
        int nfwd = 0;
        for (int p = 0; p < bb[b].npred; p++) {
            int pb = bb[b].pred[p];
            if (bb[pb].rpo >= bb[b].rpo)
                continue;                       /* retreating: carries nothing */
            nfwd++;
            int *po = aout + (size_t)pb * nk;
            for (int k = 0; k < nk; k++) {
                int m = in[k], v = po[k];       /* three-valued meet */
                in[k] = (m == -1 || v == -1) ? -1
                      : (m == -2) ? v : (v == -2) ? m
                      : (m == v) ? m : -1;
            }
        }
        for (int k = 0; k < nk; k++)
            if (in[k] == -2 || nfwd != bb[b].npred) in[k] = -1;
        pre_enter(&tab, in);
        for (int i = bb[b].start; i < bb[b].end; i++)
            pre_step(fn, keyidx, i, &tab);
        int *out = aout + (size_t)b * nk;
        for (int k = 0; k < nk; k++) out[k] = st[k];
    }

    /* ---- reuse what is already available ---------------------------- *
     *
     * Global CSE reuses a value whose definition DOMINATES the use.
     * Availability is the weaker condition and so catches strictly more:
     * it is enough that every path here has passed a definition, under
     * one name. That is exactly the state an insertion below leaves --
     * a value arriving from several predecessors, dominated by none of
     * them -- so without this step the insertions would buy nothing
     * past the one block they were made for. */
    int nreuse = 0;
    for (int b = 0; b < nbb; b++) {
        pre_enter(&tab, ain + (size_t)b * nk);
        for (int i = bb[b].start; i < bb[b].end; i++) {
            int k = keyidx[i];
            if (k >= 0 && st[k] >= 0 && st[k] != fn->ins[i].dst) {
                to_mov(&fn->ins[i], st[k]);
                keyidx[i] = -1;         /* now a rename, not a definition */
                nreuse++;
            }
            pre_step(fn, keyidx, i, &tab);
        }
    }
    if (nreuse) {
        g_did.pre += nreuse;
        free(order); free(l2b); free(i2b); free(keyidx); free(keys);
        free(aout); free(ain); free(st); free(own); free(stamp); free(inv); free(pcan);
        free_cfg(bb, nbb); free_defs(&d);
        return 1;
    }

    /* One candidate: an expression computed here whose value some -- but
     * not all -- predecessors already hold. */
    int cand = -1, cb = -1;
    for (int oi = 0; oi < norder && cand < 0; oi++) {
        int b = order[oi];
        if (bb[b].npred < 2 || bb[b].end == bb[b].start ||
            fn->ins[bb[b].start].op != IR_LABEL)
            continue;           /* a merge point always opens with a label */
        int *in = ain + (size_t)b * nk;
        for (int i = bb[b].start; i < bb[b].end && cand < 0; i++) {
            int k = keyidx[i];
            if (k < 0 || in[k] >= 0)
                continue;               /* not numbered, or already reused */
            struct ir_ins *e = &fn->ins[i];
            if (!pre_worth_it(e))
                continue;
            /* only the first computation in this block; a second one is
             * local value numbering's to make */
            int first = 1;
            for (int j = bb[b].start; j < i; j++)
                if (keyidx[j] == k) { first = 0; break; }
            if (!first)
                continue;
            int have = 0, ok = 1;
            for (int p = 0; p < bb[b].npred && ok; p++) {
                int pb = bb[b].pred[p];
                if (bb[pb].rpo >= bb[b].rpo) { ok = 0; break; }
                if (aout[(size_t)pb * nk + k] >= 0) { have++; continue; }
                if (bb[pb].end == bb[pb].start) { ok = 0; break; }
                /* a critical edge is split rather than refused, but only
                 * a conditional branch gives somewhere to put the split */
                if (bb[pb].nsucc != 1) {
                    enum ir_op t = fn->ins[bb[pb].end - 1].op;
                    if (t != IR_BRZ && t != IR_BRNZ) { ok = 0; break; }
                }
                int at = pre_tail(fn, bb, pb);
                long cv;
                for (int w = 0; w < 2 && ok; w++) {
                    int v = w ? e->b : e->a;
                    if (v < 0) continue;
                    if (get_const(fn, &d, v, &cv)) continue;
                    if (pre_slot_read(fn, &d, inv, v)) continue;
                    if (!pre_have(fn, &d, bb, nbb, i2b, v, pb, at)) ok = 0;
                }
            }
            if (ok && have > 0 && have < bb[b].npred) { cand = i; cb = b; }
        }
    }
    if (cand < 0) {
        free(order); free(l2b); free(i2b); free(keyidx); free(keys);
        free(aout); free(ain); free(st); free(own); free(stamp); free(inv); free(pcan);
        free_cfg(bb, nbb); free_defs(&d);
        return 0;
    }

    /* ---- rewrite ---------------------------------------------------- */
    int key = keyidx[cand];
    int t = fn->nvregs++;
    struct ir_ins proto = fn->ins[cand];
    int line = proto.line, col = proto.col;
    int cblab = fn->ins[bb[cb].start].label;

    /* What each predecessor contributes. `psrc >= 0`: it holds the value
     * already, so a move names it. `pins`: where the contribution goes.
     * `pnew >= 0`: the edge was critical, so the contribution goes in a
     * new block with that label and `pjmp` says how the branch reaches
     * it. */
    int *pins = xmalloc((size_t)nbb * sizeof *pins);
    int *psrc = xmalloc((size_t)nbb * sizeof *psrc);
    int *pnew = xmalloc((size_t)nbb * sizeof *pnew);
    char *pjmp = xcalloc((size_t)nbb, 1);   /* 1: append a jmp after the branch */
    for (int b = 0; b < nbb; b++) { pins[b] = -1; psrc[b] = -1; pnew[b] = -1; }
    for (int p = 0; p < bb[cb].npred; p++) {
        int pb = bb[cb].pred[p];
        int src = aout[(size_t)pb * nk + key];
        psrc[pb] = src;
        if (src < 0 && bb[pb].nsucc != 1) {
            pnew[pb] = fn->nlabels++;
            /* retarget the branch when cb is its target; otherwise cb is
             * the fall-through, and a jmp after the branch takes it */
            struct ir_ins *term = &fn->ins[bb[pb].end - 1];
            /* a switch never falls through: cb is in its table */
            if (term->op == IR_SWITCH || term->label == cblab)
                pins[pb] = bb[pb].end - 1;                    /* retarget */
            else { pins[pb] = bb[pb].end; pjmp[pb] = 1; }
        } else {
            pins[pb] = pre_tail(fn, bb, pb);
        }
    }

    /* Emit the contribution of predecessor pb into `nb`. */
    struct ibuf nb = { 0, 0, 0 };
    int *newpos = fn->var_scope_lo
        ? xmalloc((size_t)(fn->nins + 1) * sizeof *newpos) : NULL;

#define PRE_EMIT(pb)                                                       \
    do {                                                                   \
        if (psrc[pb] >= 0) {                                               \
            struct ir_ins *m = ib_push(&nb);                               \
            memset(m, 0, sizeof *m);                                       \
            m->op = IR_MOV; m->dst = t; m->a = psrc[pb]; m->b = -1;        \
            m->w = ir_result_w(&proto); m->line = line; m->col = col;      \
            m->synth = line ? 0 : 1;                                       \
        } else {                                                           \
            struct ir_ins e = proto;                                       \
            for (int w = 0; w < 2; w++) {                                  \
                int v = w ? proto.b : proto.a;                             \
                long cv;                                                   \
                struct ir_ins *r;                                          \
                if (v < 0) continue;                                       \
                if (get_const(fn, &d, v, &cv)) {                           \
                    int c = fn->nvregs++;                                  \
                    struct ir_ins *ci = ib_push(&nb);                      \
                    memset(ci, 0, sizeof *ci);                             \
                    ci->op = IR_CONST; ci->dst = c; ci->imm = cv;          \
                    ci->a = -1; ci->b = -1;                                \
                    ci->w = proto.w; ci->line = line; ci->col = col;       \
                    ci->synth = line ? 0 : 1;                              \
                    if (w) e.b = c; else e.a = c;                          \
                } else if ((r = pre_slot_read(fn, &d, inv, v)) != NULL) {  \
                    int c = fn->nvregs++;                                  \
                    struct ir_ins *ci = ib_push(&nb);                      \
                    *ci = *r; ci->dst = c;                                 \
                    if (w) e.b = c; else e.a = c;                          \
                }                                                          \
            }                                                              \
            e.dst = t;                                                     \
            *ib_push(&nb) = e;                                             \
        }                                                                  \
    } while (0)

    for (int n = 0; n < fn->nins; n++) {
        if (newpos) newpos[n] = nb.n;
        for (int b = 0; b < nbb; b++) {
            if (pins[b] != n || pnew[b] >= 0)
                continue;
            PRE_EMIT(b);
        }
        struct ir_ins *out;
        if (n == cand) {
            out = ib_push(&nb); *out = fn->ins[n]; to_mov(out, t);
            /* a copy of the value: a compare's operands' width and class
             * are not its result's (ir_result_w) */
            out->w = ir_result_w(&fn->ins[n]);
            out->flt = ir_result_flt(&fn->ins[n]);
        } else {
            out = ib_push(&nb); *out = fn->ins[n];
        }
        /* By index from here: a jump pushed below may move the buffer,
         * and `out` would then point into freed memory. */
        int out_at = nb.n - 1;
        /* the two forms a split edge takes */
        for (int b = 0; b < nbb; b++) {
            if (pnew[b] < 0 || pins[b] != n) continue;
            if (pjmp[b]) {
                struct ir_ins *j = ib_push(&nb);
                memset(j, 0, sizeof *j);
                j->op = IR_JMP; j->dst = -1; j->a = -1; j->b = -1;
                j->label = pnew[b]; j->line = line; j->col = col;
                j->synth = line ? 0 : 1;
            } else if (nb.p[out_at].op == IR_SWITCH) {
                struct retarget rt = { cblab, pnew[b] };
                each_label(fn, &nb.p[out_at], retarget_cb, &rt);  /* every entry that was cb */
            } else {
                nb.p[out_at].label = pnew[b];   /* branch now enters the split */
            }
        }
    }
    /* The split blocks themselves, past the end: each on exactly one
     * edge, each falling out to where the branch used to go. */
    for (int b = 0; b < nbb; b++) {
        if (pnew[b] < 0) continue;
        struct ir_ins *l = ib_push(&nb);
        memset(l, 0, sizeof *l);
        l->op = IR_LABEL; l->dst = -1; l->a = -1; l->b = -1;
        l->label = pnew[b]; l->line = line; l->col = col; l->synth = 1;
        PRE_EMIT(b);
        struct ir_ins *j = ib_push(&nb);
        memset(j, 0, sizeof *j);
        j->op = IR_JMP; j->dst = -1; j->a = -1; j->b = -1;
        j->label = cblab; j->line = line; j->col = col; j->synth = 1;
    }
#undef PRE_EMIT

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
    g_did.pre++;
    free(pins); free(psrc); free(pnew); free(pjmp);
    free(order); free(l2b); free(i2b); free(keyidx); free(keys);
    free(aout); free(ain); free(st); free(own); free(stamp); free(inv); free(pcan);
    free_cfg(bb, nbb); free_defs(&d);
    return 1;
}

int pass_pre(struct ir_func *fn)
{
    int changed = 0, guard = 0;
    while (guard++ < 64 && pre_one(fn))
        changed = 1;
    return changed;
}
