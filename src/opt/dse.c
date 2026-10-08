/* ---- dead store elimination ----------------------------------------
 *
 * The inverse of store forwarding: a store whose value nothing reads
 * because a later store to the same bytes comes first.
 *
 *      p->x = 1;        <- dead
 *      p->x = 2;
 *
 * DCE already drops a store to a local that is never read AT ALL. This
 * is the other case, and it needs the alias analysis above: between the
 * two stores there must be no read that could see the first one, and
 * "could see" is exactly may_alias.
 *
 * Block-local and backward. Block-local because a store dead on one
 * path out of a block is not dead on another, and proving otherwise
 * wants the available-expressions machinery load elimination has --
 * this is the cheap half. Backward because "is there a later store"
 * is the question, and walking backward makes it "have I already seen
 * one".
 *
 * Two stores kill each other only when the later one writes every byte
 * of the earlier: the SAME address temp at the same width, or the same
 * base value with the earlier's range inside the later's (acc_covers).
 * That is must-alias, not may-alias: a wrong answer here deletes a write
 * the program made. A read in between keeps the earlier store when it
 * may overlap it (acc_overlap), so a load of p->y does not keep alive a
 * store to p->x that p->x = 2 overwrites. */

#include "opt_int.h"

int pass_dse(struct ir_func *fn)
{
    if (fn->nins == 0)
        return 0;
    int nvars = fn->nvars;
    struct defs d;
    compute_defs(fn, &d);
    int nbb, *l2b;
    struct bb *bb = build_cfg(fn, &nbb, &l2b);

    char *taken = slots_taken(fn);

    char *dead = xcalloc((size_t)fn->nins, 1);
    /* Stores seen later in this block, as (address temp, width) and the
     * bytes they write. A slot store records its var with a negative
     * marker so the two kinds share one list. */
    int *sa = xmalloc((size_t)fn->nins * sizeof *sa);
    int *ssz = xmalloc((size_t)fn->nins * sizeof *ssz);
    struct maccess *sacc = xmalloc((size_t)fn->nins * sizeof *sacc);
    int changed = 0;

    for (int b = 0; b < nbb; b++) {
        int ns = 0;
        for (int i = bb[b].end - 1; i >= bb[b].start; i--) {
            struct ir_ins *ins = &fn->ins[i];
            if (ins->op == IR_STORE && !ins->vol && ins->a >= 0 &&
                ins->a < fn->nvregs && d.cnt[ins->a] == 1) {
                struct maccess w = mem_access(fn, &d, ins->a, ins->size);
                int killed = 0;
                for (int k = 0; k < ns; k++)
                    if ((sa[k] == ins->a && ssz[k] == ins->size) ||
                        acc_covers(sacc[k], w)) { killed = 1; break; }
                if (killed) { dead[i] = 1; changed = 1; continue; }
                sa[ns] = ins->a; ssz[ns] = ins->size; sacc[ns] = w; ns++;
                continue;
            }
            if (ins->op == IR_STVAR && !ins->vol && ins->dst >= 0 &&
                ins->dst < nvars && !taken[ins->dst] && !target_keep_vars()) {
                int killed = 0;
                for (int k = 0; k < ns; k++)
                    if (sa[k] == -1 - ins->dst && ssz[k] == ins->size) { killed = 1; break; }
                if (killed) { dead[i] = 1; changed = 1; continue; }
                sa[ns] = -1 - ins->dst; ssz[ns] = ins->size;
                sacc[ns] = slot_access(ins->dst); ns++;
                continue;
            }
            /* A read that could see one of them un-kills it. A call,
             * inline asm, an atomic or a fence could see anything. */
            struct maccess r = slot_access(-1);
            int reads = 0, everything = 0;
            switch (ins->op) {
            case IR_LOAD:
                r = mem_access(fn, &d, ins->a, ins->size); reads = 1; break;
            case IR_LDVAR:
                r = slot_access(ins->a); reads = 1; break;
            case IR_MEMCPY:
                r = obj_access(fn, &d, ins->b); reads = 1; break;
            case IR_STORE: case IR_STVAR:
                reads = 0; everything = 1; break;   /* volatile or unkeyed */
            case IR_CALL:
                /* A call that reads no memory the caller can see cannot
                 * observe a store, and one that writes none cannot be
                 * the reason to keep it. */
                if (!ins->indirect && ins->callee &&
                    ins->callee->inf_no_read && ins->callee->inf_no_write)
                    break;
                everything = 1; break;
            case IR_ASM: case IR_VA_START: case IR_FENCE:
            case IR_XCHG: case IR_XADD: case IR_CMPXCHG: case IR_ARMW:
            case IR_CAS: case IR_CAS16: case IR_MEMZERO: case IR_ALLOCA:
            case IR_VLOAD:          /* sixteen bytes nothing here describes */
                everything = 1; break;
            default:
                break;
            }
            if (everything) { ns = 0; continue; }
            if (!reads)
                continue;
            int j = 0;
            for (int k = 0; k < ns; k++)
                if (!acc_overlap(sacc[k], r, taken, nvars)) {
                    sa[j] = sa[k]; ssz[j] = ssz[k]; sacc[j] = sacc[k]; j++;
                }
            ns = j;
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
        g_did.dse += fn->nins - j;
        fn->nins = j;
    }

    free(dead); free(sa); free(ssz); free(sacc); free(taken); free(l2b);
    free_cfg(bb, nbb); free_defs(&d);
    return changed;
}
