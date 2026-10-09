/* ---- conditional constant propagation (the reachability half of SCCP) ----
 *
 * A conditional branch whose condition is a known constant has one live edge.
 * Resolve it (BRZ/BRNZ -> unconditional jump, or fall-through), then drop every
 * block no longer reachable over the live edges. The constant conditions come
 * from inlining a call with a constant argument, mem2reg + folding collapsing a
 * flag, config constants — code the earlier passes leave as `test; jz` over a
 * value they have already proven constant, plus the now-dead arm behind it.
 *
 * One rebuild handles both: emit each reachable block, replacing a resolved
 * branch with a jump to its live successor (or nothing when that successor is
 * the fall-through), and skip unreachable blocks entirely. */

#include "opt_int.h"

static int sccp_core(struct ir_func *fn, int fold_branches)
{
    if (fn->nins == 0)
        return 0;
    struct defs d;
    compute_defs(fn, &d);
    int nbb, *l2b;
    struct bb *bb = build_cfg(fn, &nbb, &l2b);

    /* live_only[b] = index into bb[b].succ of the sole live edge, or -1 = all.
     * succ[0] is the branch-taken target, succ[1] the fall-through. */
    int *live_only = xmalloc((size_t)nbb * sizeof *live_only);
    for (int b = 0; b < nbb; b++) {
        live_only[b] = -1;
        if (bb[b].end <= bb[b].start)
            continue;
        struct ir_ins *t = &fn->ins[bb[b].end - 1];
        if (!fold_branches ||
            (t->op != IR_BRZ && t->op != IR_BRNZ) || t->a < 0 ||
            d.cnt[t->a] != 1 || d.ins[t->a] < 0 ||
            fn->ins[d.ins[t->a]].op != IR_CONST)
            continue;
        long v = fn->ins[d.ins[t->a]].imm;
        int taken = (t->op == IR_BRZ) ? (v == 0) : (v != 0);
        int want = taken ? 0 : 1;
        if (want < bb[b].nsucc) {     /* only if that edge actually exists */
            live_only[b] = want;
            /* A branch whose condition is a constant always goes the same
             * way, which is worth saying out loud: it is as often a bug in
             * the program as an optimization in the compiler.
             *
             * `taken` means the BRANCH jumps, not that the source condition
             * was true -- for `if (c)` irgen emits `BRZ c -> else`, so a
             * taken branch is a FALSE condition. Rather than guess at the
             * source's polarity from the lowering, the remark states the IR
             * fact, which is the one that is certainly true. */
            remark_add("sccp",
                       taken ? "branch-always-jumps" : "branch-never-jumps",
                       fn->src ? fn->name : NULL,
                       "condition-is-a-constant",
                       fn->src ? fn->file : NULL, t->line,
                       "the condition folded to %ld, so one arm is "
                       "unreachable", v);
        }
    }

    /* Reachability over the live edges only. */
    char *reach = xcalloc((size_t)nbb, 1);
    int *wl = xmalloc((size_t)nbb * sizeof *wl), nwl = 0;
    reach[0] = 1; wl[nwl++] = 0;
    while (nwl) {
        int b = wl[--nwl];
        for (int s = 0; s < bb[b].nsucc; s++) {
            if (live_only[b] >= 0 && s != live_only[b])
                continue;
            int sb = bb[b].succ[s];
            if (!reach[sb]) { reach[sb] = 1; wl[nwl++] = sb; }
        }
    }

    int work = 0;
    for (int b = 0; b < nbb; b++)
        if (!reach[b] || live_only[b] >= 0) { work = 1; break; }
    if (!work) {
        free(live_only); free(reach); free(wl); free(l2b);
        free_cfg(bb, nbb); free_defs(&d);
        return 0;
    }

    struct ibuf nb = { 0, 0, 0 };
    for (int b = 0; b < nbb; b++) {
        if (!reach[b])
            continue;                       /* unreachable: drop the whole block */
        if (live_only[b] >= 0) {
            for (int n = bb[b].start; n < bb[b].end - 1; n++)  /* body, not branch */
                *ib_push(&nb) = fn->ins[n];
            int ls = bb[b].succ[live_only[b]];
            /* Jump to the live successor by label; if it has none it is the
             * fall-through (b+1, reachable, emitted next) — just fall in. */
            if (bb[ls].end > bb[ls].start && fn->ins[bb[ls].start].op == IR_LABEL) {
                struct ir_ins *j = ib_push(&nb);
                j->op = IR_JMP; j->dst = -1; j->a = -1; j->b = -1;
                j->label = fn->ins[bb[ls].start].label;
                /* It replaces the branch that ended this block, so it is
                 * that branch's `if` as far as the programmer is concerned
                 * (R3) -- not a jump from nowhere. */
                j->line = fn->ins[bb[b].end - 1].line;
                j->col = fn->ins[bb[b].end - 1].col;
                j->synth = fn->ins[bb[b].end - 1].synth;
            }
        } else {
            for (int n = bb[b].start; n < bb[b].end; n++)
                *ib_push(&nb) = fn->ins[n];
        }
    }
    free(fn->ins);
    fn->ins = nb.p; fn->nins = nb.n; fn->cap = nb.cap;

    /* The rebuild renumbered every instruction, so the local scope ranges (used
     * by codegen to coalesce disjoint-lifetime locals) are stale — drop them,
     * as mem2reg does; each surviving local then takes its own slot. */
    if (fn->var_scope_lo) {
        free(fn->var_scope_lo); free(fn->var_scope_hi);
        fn->var_scope_lo = fn->var_scope_hi = NULL;
    }

    free(live_only); free(reach); free(wl); free(l2b);
    free_cfg(bb, nbb); free_defs(&d);
    return 1;
}

int pass_sccp(struct ir_func *fn)
{
    return sccp_core(fn, 1);
}

/* Only the second half: drop the blocks nothing reaches. mem2reg needs
 * it -- its dominators are wrong over a block no path enters, so it
 * refused any function with one -- and irgen leaves them wherever a
 * statement follows a jump: the `break` after a `return` or `continue`,
 * the code after a call to a noreturn function, the end of a loop that
 * never exits. Across lib/libc and EmbLinkOs that was 149 functions --
 * sin, cos, pow and most of fdlibm among them -- whose every local
 * stayed in memory through the whole optimizer. */
int drop_unreachable(struct ir_func *fn)
{
    return sccp_core(fn, 0);
}
