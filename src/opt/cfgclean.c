/* ---- CFG cleanup: jump threading and block merging -----------------
 *
 * Three shapes that irgen and the loop passes leave behind, none of
 * which any existing pass removes:
 *
 *   A jump to a jump. `goto L1` where L1 holds only `goto L2` should go
 *   straight to L2. Rotation and if-conversion both create these when
 *   they delete the block in between.
 *
 *   A branch whose two arms are the same label. After SCCP folds one
 *   side of a condition the two edges can converge, and the branch is
 *   then a jump -- but the condition is still computed and tested.
 *
 *   A jump to the very next instruction, which is nothing at all.
 *
 * They are worth removing for their own sake and because every pass
 * that reasons about blocks gets a smaller graph: a label with no
 * remaining jumps to it stops splitting a block, so the block-local
 * passes see more at once. */

#include "opt_int.h"

/* Jump threading on a merged boolean: what `a && b` and `a || b` leave in
 * a condition.
 *
 *       %t = mov %c        ; the comparison, on one arm
 *       jmp L
 *       ...
 *       %t = const 0       ; the short-circuit, on the other
 *       jmp L
 *   L:  brz %t -> Lx       ; %t's only reader
 *       <Lafter>
 *
 * Each arm already knows where it is going: the constant one straight to
 * Lx or Lafter, the copying one on %c itself. So an arm becomes a jump
 * there, and the 0/1 is never built, merged and tested again -- 128 of
 * these in lib/libc, and on AVR each 0/1 was four bytes wide.
 *
 * Lafter is the label right after the branch, or the target of a jmp
 * there; when it is neither, one is inserted, and only then. The skipped
 * `jmp L` is left dead for cfgclean. A copying arm that FALLS into L is
 * left alone, since a branch there would leave %t unset on the way
 * through. */
/* Move fn->var_scope_lo/hi with a renumbering: newpos[n] is where old
 * instruction n now is, newpos[oldn] the new end (DCE's idiom). */
void remap_scopes(struct ir_func *fn, const int *newpos, int oldn)
{
    if (!fn->var_scope_lo || !newpos)
        return;
    for (int v = 0; v < fn->nvars; v++) {
        int lo = fn->var_scope_lo[v], hi = fn->var_scope_hi[v];
        if (lo >= 0 && lo <= oldn) fn->var_scope_lo[v] = newpos[lo];
        if (hi >= 0 && hi <= oldn) fn->var_scope_hi[v] = newpos[hi];
    }
}

static int thread_arm(const struct ir_func *fn, int k, int t, int L,
                      const struct defs *d)
{
    const struct ir_ins *def = &fn->ins[k], *go = &fn->ins[k + 1];
    int jumps = go->op == IR_JMP && go->label == L;
    int falls = k + 1 < fn->nins && go->op == IR_LABEL && go->label == L;
    if (def->dst != t || def->flt || (!falls && !jumps))
        return 0;
    if (def->op == IR_CONST)
        return 1;
    if (def->op != IR_MOV || def->a < 0 || def->a >= fn->nvregs ||
        d->cnt[def->a] != 1 || d->ins[def->a] < 0)
        return 0;
    /* a copy of a constant is a constant arm (the merge temp is written
     * twice, so copy propagation leaves `%t = mov %k` where %k = const) */
    if (fn->ins[d->ins[def->a]].op == IR_CONST && !fn->ins[d->ins[def->a]].flt)
        return 1;
    return jumps && fn->ins[d->ins[def->a]].op == IR_CMP;
}

/* The constant an arm sets, when thread_arm accepted it as one. */
static int thread_arm_const(const struct ir_func *fn, const struct ir_ins *def,
                            const struct defs *d, long *v)
{
    if (def->op == IR_CONST) {
        *v = def->imm;
        return 1;
    }
    if (def->op == IR_MOV && def->a >= 0 && def->a < fn->nvregs &&
        d->cnt[def->a] == 1 && d->ins[def->a] >= 0 &&
        fn->ins[d->ins[def->a]].op == IR_CONST) {
        *v = fn->ins[d->ins[def->a]].imm;
        return 1;
    }
    return 0;
}

static int thread_site(const struct ir_func *fn, int n, const int *use)
{
    const struct ir_ins *lab = &fn->ins[n], *br = &fn->ins[n + 1];
    int t = br->a;
    return n + 2 < fn->nins && lab->op == IR_LABEL &&
           (br->op == IR_BRZ || br->op == IR_BRNZ) &&
           t >= fn->nvars && t < fn->nvregs && use[t] == 1 && br->w <= 8;
}

/* A branch on `%u = cmp ne %t, 0` is a branch on %t, and one on
 * `cmp eq %t, 0` the opposite branch on %t, when nothing else reads %u.
 * C writes the first wherever a truth value is compared with pdFALSE or
 * 0 -- FreeRTOS's `listLIST_IS_EMPTY(l) == pdFALSE` is a 1/0 merged from
 * two arms and then tested -- and the compare in between hid the merge
 * from the threading below. The compare is left for DCE.
 *
 * Unsigned, `%t < 1` and `%t <= 0` are `%t == 0`, and `%t >= 1` and
 * `%t > 0` are `%t != 0`: FreeRTOS asserts `uxIndexToNotify <
 * configTASK_NOTIFICATION_ARRAY_ENTRIES`, which is 1 by default, and the
 * compare-and-branch it made was a `cmp #1; blo` where a test of the
 * value is one cbz. */
static int branch_on_cmp0(struct ir_func *fn, const int *use,
                          const struct defs *d)
{
    int changed = 0;
    for (int n = 0; n + 1 < fn->nins; n++) {
        const struct ir_ins *c = &fn->ins[n];
        struct ir_ins *br = &fn->ins[n + 1];
        long k;
        int eq;
        if (c->op != IR_CMP || c->flt || c->w > 8 ||
            (br->op != IR_BRZ && br->op != IR_BRNZ) || br->a != c->dst ||
            c->dst < 0 || c->dst >= fn->nvregs || use[c->dst] != 1 ||
            c->a < 0 || c->a >= fn->nvregs)
            continue;
        if (c->imm_b)
            k = c->imm;
        else if (c->b >= 0 && c->b < fn->nvregs && d->cnt[c->b] == 1 &&
                 d->ins[c->b] >= 0 && fn->ins[d->ins[c->b]].op == IR_CONST)
            k = fn->ins[d->ins[c->b]].imm;
        else
            continue;
        if (k == 0 && (c->pred == B_EQ || c->pred == B_NE))
            eq = c->pred == B_EQ;
        else if (!c->sign && k == 1 && (c->pred == B_LT || c->pred == B_GE))
            eq = c->pred == B_LT;
        else if (!c->sign && k == 0 && (c->pred == B_LE || c->pred == B_GT))
            eq = c->pred == B_LE;
        else
            continue;
        br->a = c->a;
        br->w = c->w;
        if (eq)
            br->op = br->op == IR_BRZ ? IR_BRNZ : IR_BRZ;
        changed = 1;
    }
    return changed;
}

int pass_thread(struct ir_func *fn)
{
    int changed = 0, nv = fn->nvregs;
    int *use = xcalloc((size_t)(nv ? nv : 1), sizeof *use);
    struct ucount uc = { use, nv };
    struct defs d;

    for (int n = 0; n < fn->nins; n++)
        each_read(&fn->ins[n], count_cb, &uc);
    compute_defs(fn, &d);
    /* the branch now reads %t, and the uses counted are stale: the next
     * round, after DCE, threads it */
    if (branch_on_cmp0(fn, use, &d)) {
        free(use);
        free(d.cnt); free(d.ins);
        return 1;
    }
    /* A label after each branch that will be threaded and has none. */
    {
        int need = 0;
        char *mark = xcalloc((size_t)fn->nins + 1, 1);
        for (int n = 0; n + 2 < fn->nins; n++) {
            const struct ir_ins *nx = &fn->ins[n + 2];
            if (!thread_site(fn, n, use) || nx->op == IR_LABEL ||
                nx->op == IR_JMP)
                continue;
            for (int k = 0; k + 1 < fn->nins; k++)
                if (thread_arm(fn, k, fn->ins[n + 1].a, fn->ins[n].label, &d)) {
                    mark[n + 1] = 1;
                    need = 1;
                    break;
                }
        }
        if (need) {
            struct ibuf nb = { 0, 0, 0 };
            int *newpos = xmalloc((size_t)(fn->nins + 1) * sizeof *newpos);
            for (int n = 0; n < fn->nins; n++) {
                newpos[n] = nb.n;
                *ib_push(&nb) = fn->ins[n];
                if (mark[n]) {
                    struct ir_ins *l = ib_push(&nb);
                    memset(l, 0, sizeof *l);
                    l->op = IR_LABEL;
                    l->label = fn->nlabels++;
                    l->dst = l->a = l->b = -1;
                    l->line = fn->ins[n].line; l->col = fn->ins[n].col;
                    l->synth = 1;
                }
            }
            newpos[fn->nins] = nb.n;
            remap_scopes(fn, newpos, fn->nins);
            free(newpos);
            free(fn->ins);
            fn->ins = nb.p; fn->nins = nb.n; fn->cap = nb.cap;
            free(d.cnt); free(d.ins);
            compute_defs(fn, &d);
        }
        free(mark);
    }
    for (int n = 0; n + 2 < fn->nins; n++) {
        const struct ir_ins *lab = &fn->ins[n], *br = &fn->ins[n + 1];
        const struct ir_ins *nx = &fn->ins[n + 2];
        int t = br->a, L = lab->label, Lx = br->label, Lafter;
        if (!thread_site(fn, n, use))
            continue;
        if (nx->op == IR_LABEL)     Lafter = nx->label;
        else if (nx->op == IR_JMP)  Lafter = nx->label;
        else                        continue;
        for (int k = 0; k + 1 < fn->nins; k++) {
            struct ir_ins *def = &fn->ins[k], *go = &fn->ins[k + 1];
            long kv;
            if (!thread_arm(fn, k, t, L, &d))
                continue;
            if (thread_arm_const(fn, def, &d, &kv)) {
                unsigned long mask = br->w >= 8 ? ~0UL
                                   : (1UL << (8 * (br->w > 0 ? br->w : 4))) - 1;
                int zero = ((unsigned long)kv & mask) == 0;
                int taken = br->op == IR_BRZ ? zero : !zero;
                int line = def->line, col = def->col;
                memset(def, 0, sizeof *def);
                def->op = IR_JMP;
                def->label = taken ? Lx : Lafter;
                def->dst = def->a = def->b = -1;
                def->line = line; def->col = col;
                changed = 1;
            } else {                               /* a copied comparison */
                int c = def->a;
                def->op = br->op;
                def->a = c;
                def->b = -1;
                def->dst = -1;
                def->label = Lx;
                def->w = br->w;
                def->sign = br->sign;
                def->imm_b = 0;
                go->label = Lafter;
                changed = 1;
            }
        }
    }
    free(use);
    free(d.cnt); free(d.ins);
    return changed;
}

struct fwdctx { const int *fwd; int n, changed; };
static void fwd_cb(int *p, void *ctx)
{
    struct fwdctx *c = ctx;
    int l = *p;
    if (l < 0 || l >= c->n || c->fwd[l] < 0 || c->fwd[l] == l)
        return;
    *p = c->fwd[l];
    c->changed = 1;
}
struct reachctx { char *reached; int n; };
static void reach_cb(int *p, void *ctx)
{
    struct reachctx *c = ctx;
    if (*p >= 0 && *p < c->n) c->reached[*p] = 1;
}

int pass_cfgclean(struct ir_func *fn)
{
    if (fn->nins == 0 || fn->nlabels == 0)
        return 0;
    int changed = 0;

    /* Where each label sits, and whether the block it opens is nothing
     * but an unconditional jump elsewhere. */
    int *at = xmalloc((size_t)fn->nlabels * sizeof *at);
    int *fwd = xmalloc((size_t)fn->nlabels * sizeof *fwd);
    for (int l = 0; l < fn->nlabels; l++) { at[l] = -1; fwd[l] = -1; }
    for (int n = 0; n < fn->nins; n++)
        if (fn->ins[n].op == IR_LABEL && fn->ins[n].label >= 0 &&
            fn->ins[n].label < fn->nlabels)
            at[fn->ins[n].label] = n;
    for (int l = 0; l < fn->nlabels; l++) {
        int n = at[l];
        if (n < 0) continue;
        /* skip any further labels on the same spot */
        while (n + 1 < fn->nins && fn->ins[n + 1].op == IR_LABEL) n++;
        if (n + 1 < fn->nins && fn->ins[n + 1].op == IR_JMP)
            fwd[l] = fn->ins[n + 1].label;
    }
    /* Follow the chains, with a bound: `L: goto L` is a real loop and
     * must not be collapsed into itself. */
    for (int l = 0; l < fn->nlabels; l++) {
        int t = fwd[l], hops = 0;
        while (t >= 0 && t < fn->nlabels && fwd[t] >= 0 && fwd[t] != t &&
               hops++ < 16)
            t = fwd[t];
        fwd[l] = t;
    }
    {
        struct fwdctx fc = { fwd, fn->nlabels, 0 };
        for (int n = 0; n < fn->nins; n++) {
            struct ir_ins *i = &fn->ins[n];
            if (i->op != IR_JMP && i->op != IR_BRZ && i->op != IR_BRNZ &&
                i->op != IR_SWITCH)
                continue;
            each_label(fn, i, fwd_cb, &fc);
        }
        if (fc.changed)
            changed = 1;
    }

    /* A conditional branch whose taken target is the fall-through is a
     * jump; so is one whose two arms are the same label. */
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        if (i->op != IR_BRZ && i->op != IR_BRNZ)
            continue;
        int m = n + 1;
        while (m < fn->nins && fn->ins[m].op == IR_LABEL) {
            if (fn->ins[m].label == i->label) {
                i->op = IR_JMP; i->a = -1; changed = 1;
                break;
            }
            m++;
        }
    }

    /* A second branch on a condition the one above it already decided.
     * Control reaches it only by falling out of the first, so the
     * condition is known there: the same test cannot fire twice, and
     * the opposite test cannot fail. Nothing can jump between them --
     * there is no label -- which is the whole of the argument.
     *
     * Unrolling makes these on purpose: its entry test repeats the
     * loop's own guard, because the block it opens is the target of a
     * back edge and the guard runs once. Threading a jump makes them
     * too. */
    char *dead = xcalloc((size_t)fn->nins, 1);
    /* A branch on a temp the same block has just set to a constant.
     * pass_fold decides a rotated loop's guard, `i = 0; c = i < 24`, from
     * the block's own `i = 0` (lk_note), and value numbering then gives
     * the constant it made the name it already had in the block -- the
     * loop-carried i -- so the branch reads a temp with two definitions
     * and nothing else here can decide it. The same table decides it
     * here. */
    if (!getenv("EMBCC_NO_LKCONST")) {
        struct lkconst lk = {
            xcalloc((size_t)fn->nvregs, sizeof(int)),
            xmalloc((size_t)fn->nvregs * sizeof(long)),
            xmalloc((size_t)fn->nvregs * sizeof(int)), 1, fn->nvregs
        };
        /* Only for a name with more than one definition. One defined
         * once by a constant is SCCP's to decide, and SCCP says so in a
         * remark (`if (DEBUG)` with `const int DEBUG = 0`: the branch
         * always goes one way, which is as often a bug as an
         * optimization). Deciding it here first took that remark away. */
        struct defs dd;
        compute_defs(fn, &dd);
        for (int n = 0; n < fn->nins; n++) {
            if (n > 0)
                lk_note(&lk, &fn->ins[n - 1]);
            struct ir_ins *i = &fn->ins[n];
            long v;
            if ((i->op != IR_BRZ && i->op != IR_BRNZ) ||
                !lk_get(&lk, i->a, i->w, &v))
                continue;
            if (i->a >= 0 && i->a < fn->nvregs && dd.cnt[i->a] == 1 &&
                dd.ins[i->a] >= 0 && fn->ins[dd.ins[i->a]].op == IR_CONST)
                continue;
            if ((i->op == IR_BRZ) == (norm(v, i->w) == 0)) {
                i->op = IR_JMP; i->a = -1;      /* it is always taken */
            } else {
                dead[n] = 1;                    /* it is never taken */
            }
            changed = 1;
        }
        free(lk.gen_of); free(lk.val); free(lk.w);
        free_defs(&dd);
    }
    {
        int last_cond = -1;
        enum ir_op last_op = IR_JMP;
        for (int n = 0; n < fn->nins; n++) {
            struct ir_ins *i = &fn->ins[n];
            if (i->op == IR_LABEL) { last_cond = -1; continue; }
            if (dead[n])
                continue;                       /* decided above */
            int t = def_target(i);
            if (t >= 0 && t == last_cond)
                last_cond = -1;                 /* a different value now */
            if (i->op != IR_BRZ && i->op != IR_BRNZ)
                continue;
            if (last_cond >= 0 && i->a == last_cond) {
                if (i->op == last_op) {
                    dead[n] = 1;                /* it cannot fire */
                    changed = 1;
                    continue;                   /* and decides nothing */
                }
                i->op = IR_JMP; i->a = -1;      /* it cannot fail */
                changed = 1;
                last_cond = -1;
                continue;
            }
            last_cond = i->a; last_op = i->op;
        }
    }

    /* A label nothing can jump to is not a block boundary, it is only
     * pretending to be one -- and every block-local pass stops at it:
     * value numbering, copy propagation, store forwarding and dead-
     * store elimination all reason between labels, so one that no
     * branch names splits a straight line into two for nothing. The
     * loop passes leave these behind by the handful (rotation's old
     * header, a latch that was once its own block), and threading a
     * jump above makes more of them on the spot.
     *
     * A landing pad's label IS named, by the exception region rather
     * than by a branch, and the unwinder is what jumps to it. */
    /* A conditional branch AROUND an unconditional one:
     *
     *      brz  c, L1              brnz c, L2
     *      jmp  L2         ==>
     *   L1:                     L1:
     *
     * is one branch on the opposite sense. irgen writes the first shape
     * for every `?:` and `if` whose arm ends in a jump, and on a machine
     * with conditional branches it cost a branch per test. The inversion
     * is exact on the IR's 0/1 value -- brz and brnz are each other's
     * complement whatever produced it, NaN compares included. The jump
     * is left aimed at L1, where the rule below drops it. */
    for (int n = 0; n + 2 < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n], *j = &fn->ins[n + 1];
        /* Not a branch the rule above already condemned: it is only
         * MARKED dead, and removed at the end -- inverted first, the
         * removal took the jump's target with it. `brnz c; brnz c -> L9;
         * jmp L4; L9:` lost its way to L4 (a switch's case 0, at -Os,
         * where switches are compare trees) and SCCP then deleted the
         * case as unreachable. */
        if (dead[n] || dead[n + 1])
            continue;
        if ((i->op != IR_BRZ && i->op != IR_BRNZ) || j->op != IR_JMP ||
            j->label == i->label)
            continue;
        int m = n + 2, hit = 0;
        while (m < fn->nins && fn->ins[m].op == IR_LABEL)
            if (fn->ins[m++].label == i->label) { hit = 1; break; }
        if (!hit)
            continue;
        {
            int l2 = j->label;       /* where the branch goes now */
            i->op = i->op == IR_BRZ ? IR_BRNZ : IR_BRZ;
            j->label = i->label;     /* a jump to the fall-through */
            i->label = l2;
        }
        changed = 1;
    }

    char *reached = xcalloc((size_t)(fn->nlabels ? fn->nlabels : 1), 1);
    {
        struct reachctx rc = { reached, fn->nlabels };
        for (int n = 0; n < fn->nins; n++)
            each_label(fn, &fn->ins[n], reach_cb, &rc);
    }
    for (int e = 0; e < fn->neh; e++)
        if (fn->eh[e].lp_label >= 0 && fn->eh[e].lp_label < fn->nlabels)
            reached[fn->eh[e].lp_label] = 1;

    /* Drop a jump to the label that immediately follows it, and any
     * instruction that can never be reached: after an unconditional
     * transfer, until the next label. */
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        if (i->op == IR_LABEL && i->label >= 0 && i->label < fn->nlabels &&
            !reached[i->label]) {
            dead[n] = 1;
            continue;
        }
        if (i->op == IR_JMP) {
            int m = n + 1;
            while (m < fn->nins && fn->ins[m].op == IR_LABEL) {
                if (fn->ins[m].label == i->label) { dead[n] = 1; break; }
                m++;
            }
        }
        if ((i->op == IR_JMP && !dead[n]) || i->op == IR_RET ||
            i->op == IR_UD2 || i->op == IR_SWITCH) {
            for (int m = n + 1; m < fn->nins; m++) {
                if (fn->ins[m].op == IR_LABEL)
                    break;
                dead[m] = 1;
            }
        }
    }
    int ndead = 0;
    for (int n = 0; n < fn->nins; n++) ndead += dead[n];
    if (ndead) {
        int *newpos = fn->var_scope_lo
            ? xmalloc((size_t)(fn->nins + 1) * sizeof *newpos) : NULL;
        int j = 0;
        for (int n = 0; n < fn->nins; n++) {
            if (newpos) newpos[n] = j;
            if (dead[n]) continue;
            if (j != n) fn->ins[j] = fn->ins[n];
            j++;
        }
        if (newpos) {
            newpos[fn->nins] = j;
            for (int v = 0; v < fn->nvars; v++) {
                int lo = fn->var_scope_lo[v], hi = fn->var_scope_hi[v];
                if (lo >= 0 && lo <= fn->nins) fn->var_scope_lo[v] = newpos[lo];
                if (hi >= 0 && hi <= fn->nins) fn->var_scope_hi[v] = newpos[hi];
            }
            free(newpos);
        }
        fn->nins = j;
        changed = 1;
    }
    g_did.cfgclean += changed;
    free(dead); free(reached); free(at); free(fwd);
    return changed;
}

/* ---- a branch a dominating branch has already decided ------------------
 *
 * `if (!f || !(f->flags & W)) { if (f) f->flags |= ERR; return EOF; }`:
 * the inner test of f is on a path where the outer one found it null or
 * found it not, and either way it has an answer. The block-local version
 * above (last_cond) catches the two in one block; across blocks it takes
 * dominators. A branch on value v in block B is decided when some
 * dominator D of B ends in a branch on the same v and one of D's two
 * successors S both dominates B and is entered from D alone: every path
 * to B then takes the edge D -> S, so at B's branch v is what D's branch
 * left it. (S dominating B is not enough by itself: S may be the join
 * the other arm reaches too -- the `if (f)` inside `if (!f || ...)`.)
 *
 * That holds for a single-definition v even inside a loop: the one
 * definition dominates D (D reads it), and a path from D back round to B
 * that executed it again would pass D again -- every path to B passes D
 * -- where the branch tests the new value. A merge temporary (a `?:`,
 * an `&&`) has several definitions and is left alone. */
int pass_brdom(struct ir_func *fn)
{
    if (fn->nins == 0)
        return 0;
    struct defs d;
    compute_defs(fn, &d);
    int nbb, *l2b;
    struct bb *bb = build_cfg(fn, &nbb, &l2b);
    int *order = xmalloc((size_t)(nbb ? nbb : 1) * sizeof *order), norder;
    compute_rpo(bb, nbb, order, &norder);
    if (norder != nbb) {              /* unreachable blocks: SCCP's first */
        free(order); free(l2b); free_cfg(bb, nbb); free_defs(&d);
        return 0;
    }
    compute_idom(bb, order, norder);
    char *dead = xcalloc((size_t)fn->nins, 1);
    int changed = 0;
    for (int b = 0; b < nbb; b++) {
        int e = bb[b].end - 1;
        if (e < bb[b].start)
            continue;
        struct ir_ins *i = &fn->ins[e];
        if ((i->op != IR_BRZ && i->op != IR_BRNZ) || i->a < 0 ||
            i->a >= fn->nvregs || d.cnt[i->a] != 1)
            continue;
        int v = i->a;
        for (int D = bb[b].idom; D >= 0 && D != b; D = bb[D].idom) {
            int ed = bb[D].end - 1;
            const struct ir_ins *j = ed >= bb[D].start ? &fn->ins[ed] : NULL;
            if (j && (j->op == IR_BRZ || j->op == IR_BRNZ) && j->a == v &&
                j->label >= 0 && l2b[j->label] >= 0) {
                int T = l2b[j->label], F = D + 1 < nbb ? D + 1 : -1;
                int dt = bb[T].npred == 1 && bb_dominates(bb, T, b);
                int df = F >= 0 && bb[F].npred == 1 && bb_dominates(bb, F, b);
                int known = -1;           /* v at B: 1 nonzero, 0 zero */
                if (T != F && dt && !df)
                    known = j->op == IR_BRZ ? 0 : 1;
                else if (T != F && df && !dt)
                    known = j->op == IR_BRZ ? 1 : 0;
                if (known >= 0) {
                    int fires = i->op == IR_BRZ ? known == 0 : known != 0;
                    if (fires) { i->op = IR_JMP; i->a = -1; }   /* always */
                    else       dead[e] = 1;                     /* never */
                    changed = 1;
                    break;
                }
            }
            if (D == 0)
                break;
        }
    }
    if (changed) {
        int *newpos = fn->var_scope_lo
            ? xmalloc((size_t)(fn->nins + 1) * sizeof *newpos) : NULL;
        int j = 0;
        for (int n = 0; n < fn->nins; n++) {
            if (newpos) newpos[n] = j;
            if (dead[n]) continue;
            if (j != n) fn->ins[j] = fn->ins[n];
            j++;
        }
        if (newpos) {
            newpos[fn->nins] = j;
            for (int v = 0; v < fn->nvars; v++) {
                int lo = fn->var_scope_lo[v], hi = fn->var_scope_hi[v];
                if (lo >= 0 && lo <= fn->nins) fn->var_scope_lo[v] = newpos[lo];
                if (hi >= 0 && hi <= fn->nins) fn->var_scope_hi[v] = newpos[hi];
            }
            free(newpos);
        }
        fn->nins = j;
    }
    free(dead); free(order); free(l2b); free_cfg(bb, nbb); free_defs(&d);
    return changed;
}
