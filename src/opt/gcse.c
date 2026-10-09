/* ---- global common-subexpression elimination (dominator-scoped VN) ----
 *
 * pass_lvn reuses an identical computation only within a block. Global CSE
 * carries a value across the dominator tree: a value computed in a block is
 * available to every block that block dominates. Sound because the operand
 * temps are single-assignment (equal temps => equal value) and the producing
 * temp, defined in a dominator, is live on every path to the reuse. Only
 * position-independent, non-memory pure ops are numbered — arithmetic, compares,
 * extends, address computations, constants; a memory read (LDVAR/LOAD) depends
 * on a store history that crosses blocks, so pass_lvn keeps those local. */

#include "opt_int.h"

int gcse_numberable(enum ir_op op)
{
    switch (op) {
    /* Only genuinely COMPUTED values. A cheap single-instruction
     * materialization (CONST, a lea for &local, a sign/zero-extend, a
     * bswap) costs less to recompute than to keep live across the
     * dominated region — global-CSEing those only lengthens a live range
     * (forcing a spill or a callee-saved reg) for no win. Redundant
     * arithmetic/compares are the profitable case. Memory reads
     * (LDVAR/LOAD) stay with the memory-versioned pass_lvn. */
    case IR_ADD: case IR_SUB: case IR_MUL:
    case IR_DIV: case IR_MOD: case IR_AND: case IR_OR: case IR_XOR:
    case IR_SHL: case IR_SHR: case IR_CMP: case IR_NEG: case IR_BNOT:
    case IR_MULH: case IR_MULW:
        return 1;
    /* A global's or a string's address is not that cheap. It is two
     * instructions on Thumb (movw/movt), RISC-V (auipc/addi) and aarch64
     * (adrp/add), and a seven-byte lea on x86-64. A function that names
     * the same global in three blocks rebuilt it in each: the hash
     * table's insert, `hused` three times. Measured 2026-10-04: the hash
     * kernel ran 2.4-2.6% fewer instructions on RV32, M4 and aarch64 and
     * 1.2% fewer on x86-64, nothing else moved, and code over lib/libc
     * and the workload shrank 0.2-0.6% on all four. AVR, whose few
     * registers make a long live range dearer, came out even and is left
     * as it was. EMBCC_NO_GCSE_ADDR=1 turns it off, for bisecting. */
    case IR_GADDR: case IR_STRADDR:
        return target_get() != TARGET_AVR && !getenv("EMBCC_NO_GCSE_ADDR");
    default:
        return 0;
    }
}

/* Key a constant operand by its VALUE.
 *
 * IR_CONST is deliberately not numbered by this pass -- rematerialising
 * a literal costs less than keeping one live across a dominated region
 * -- so two blocks that each need `-2` hold it in two different temps.
 * Keyed by temp, `x & -2` in one block and `x & -2` in another were two
 * different values and never matched, which was very nearly ALL of what
 * this pass was missing: over lib/libc and lib/libcxx it found 3
 * redundant expressions, and 97 once an operand could be a literal.
 *
 * What gets kept live is still only the RESULT -- the arithmetic the
 * policy above calls the profitable case. The constant temp is left
 * where it was and dies with the instruction that read it. */
void gcse_key_consts(struct ir_func *fn, struct defs *d, struct vn *k)
{
    long v;
    if (k->a >= 0 && get_const(fn, d, k->a, &v))
        { k->a = -1; k->ca = v; k->has_ca = 1; }
    if (k->b >= 0 && get_const(fn, d, k->b, &v))
        { k->b = -1; k->cb = v; k->has_cb = 1; }
}

int pass_gcse(struct ir_func *fn)
{
    if (fn->nins == 0)
        return 0;
    int nbb, *l2b;
    struct bb *bb = build_cfg(fn, &nbb, &l2b);
    int *order = xmalloc((size_t)nbb * sizeof *order), norder;
    compute_rpo(bb, nbb, order, &norder);
    if (norder != nbb) {   /* unreachable blocks: dominance is not total, bail */
        free(order); free(l2b);
        free_cfg(bb, nbb); return 0;
    }
    compute_idom(bb, order, norder);
    struct defs dfs;
    compute_defs(fn, &dfs);

    /* An active table holding the current block's and its dominators' values,
     * pushed on enter and truncated back on leave — an explicit dom-tree DFS so
     * siblings never see each other's values (they do not dominate each other).
     * A key is entered only when it is not there, so the table holds each
     * once and is hashed (vntab); leaving a block unlinks what it entered,
     * newest first. */
    struct vntab tb;
    vntab_init(&tb, fn->nins, fn->nvregs, 0);
    int changed = 0;
    int *dstk = xmalloc((size_t)nbb * sizeof *dstk);
    int *mark = xmalloc((size_t)nbb * sizeof *mark);
    char *entered = xcalloc((size_t)nbb, 1);
    /* Each block's dominator-tree children, in block order: finding them
     * by asking every block for its idom was blocks x blocks, and a
     * switch of 4000 cases is 8000 blocks. Pushed in that order, so the
     * walk visits them in the order it always has. */
    int *kid = xmalloc((size_t)(nbb + 1) * sizeof *kid);
    int *kids = xmalloc((size_t)(nbb ? nbb : 1) * sizeof *kids);
    for (int b = 0; b <= nbb; b++) kid[b] = 0;
    for (int c = 1; c < nbb; c++)
        if (bb[c].idom >= 0 && bb[c].idom < nbb) kid[bb[c].idom + 1]++;
    for (int b = 0; b < nbb; b++) kid[b + 1] += kid[b];
    {
        int *fill = xmalloc((size_t)(nbb ? nbb : 1) * sizeof *fill);
        for (int b = 0; b < nbb; b++) fill[b] = kid[b];
        for (int c = 1; c < nbb; c++)
            if (bb[c].idom >= 0 && bb[c].idom < nbb)
                kids[fill[bb[c].idom]++] = c;
        free(fill);
    }
    int dsp = 0; dstk[dsp++] = 0;
    while (dsp) {
        int b = dstk[dsp - 1];
        if (!entered[b]) {
            entered[b] = 1;
            mark[b] = tb.ne;
            for (int n = bb[b].start; n < bb[b].end; n++) {
                struct ir_ins *i = &fn->ins[n];
                struct vn k;
                /* vn_stable: a key made of vreg numbers names a value
                 * only where a vreg names one. Dominance is not enough
                 * on its own -- the guard of a rotated loop dominates
                 * its latch, and the induction variable is a different
                 * value at each. */
                if (i->dst < 0 || !gcse_numberable(i->op) ||
                    !vn_stable(fn, &dfs, i) || !vn_key(i, 0, &k))
                    continue;
                gcse_key_consts(fn, &dfs, &k);
                int hit = vntab_find(&tb, &k);
                if (hit >= 0 && hit != i->dst) {
                    vn_to_mov(i, hit); changed = 1; g_did.gcse++;
                } else if (hit < 0) {
                    k.result = i->dst;
                    vntab_add(&tb, &k);
                }
            }
            for (int q = kid[b]; q < kid[b + 1]; q++)
                if (!entered[kids[q]]) dstk[dsp++] = kids[q];
        } else {
            /* leaving b: drop its (and its subtree's) values */
            while (tb.ne > mark[b])
                vntab_unlink(&tb, --tb.ne);
            dsp--;
        }
    }
    vntab_free(&tb); free(kid); free(kids);
    free(dstk); free(mark); free(entered);
    free_defs(&dfs);
    free(order); free(l2b);
    free_cfg(bb, nbb);
    return changed;
}
