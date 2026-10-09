/* ---- IR verifier (opt-in via EMBCC_VERIFY) --------------------------------
 * A cheap post-optimization sanity net for the two invariants a silent
 * miscompile breaks, and which the 99-test suite did NOT catch when var_scope
 * went stale: (1) every TEMP a surviving instruction reads still has a
 * definition — a pass that drops a live value trips this; (2) var_scope_lo/hi,
 * when present, index into the CURRENT instruction stream — a pass that
 * renumbers instructions without remapping (the DCE bug) trips this. Off by
 * default so normal builds pay nothing; the test suite runs with it set.
 * Aborts loudly (THE RULE) rather than let wrong code through. */

#include "opt_int.h"

struct vrfy { struct defs *d; int np, nv; struct ir_func *fn; const char *tag; };
static void vrfy_read_cb(int *p, void *ctx)
{
    struct vrfy *v = ctx;
    int r = *p;
    if (r < 0 || r < v->nv)          /* param or local: a local may be read uninit'd */
        return;
    if (r < v->fn->nvregs && v->d->cnt[r] > 0)   /* a temp with a definition: fine */
        return;
    diag_fatal(v->fn->file, 0,
        "internal: %s reads temp %%%d with no definition (after %s) — an optimizer "
        "pass dropped a value that is still used", v->fn->name, r, v->tag);
}
/* pointer order, for the duplicate test only: nothing printed or emitted
 * depends on it */
static int cmp_argv_ptr(const void *a, const void *b)
{
    uintptr_t x = (uintptr_t)*(const struct ir_arg *const *)a;
    uintptr_t y = (uintptr_t)*(const struct ir_arg *const *)b;
    return x < y ? -1 : x > y;
}

void verify_func(struct ir_func *fn, const char *tag)
{
    struct defs d;
    compute_defs(fn, &d);
    struct vrfy v = { &d, fn->nparams, fn->nvars, fn, tag };
    for (int n = 0; n < fn->nins; n++)
        each_read(&fn->ins[n], vrfy_read_cb, &v);
    /* (3) every target a switch names is a label this function places:
     * a table entry left behind by a pass that renumbered or dropped
     * labels is a jump into nowhere. */
    {
        char *placed = xcalloc((size_t)(fn->nlabels ? fn->nlabels : 1), 1);
        for (int n = 0; n < fn->nins; n++)
            if (fn->ins[n].op == IR_LABEL && fn->ins[n].label >= 0 &&
                fn->ins[n].label < fn->nlabels)
                placed[fn->ins[n].label] = 1;
        for (int n = 0; n < fn->nins; n++) {
            const struct ir_ins *i = &fn->ins[n];
            if (i->op != IR_SWITCH)
                continue;
            if (i->jt < 0 || i->jt >= fn->njt || fn->jt[i->jt].n < 1)
                diag_fatal(fn->file, 0, "internal: %s has a switch with no "
                           "table (after %s)", fn->name, tag);
            for (int k = -1; k < fn->jt[i->jt].n; k++) {
                int l = k < 0 ? i->label : fn->jt[i->jt].labels[k];
                if (l < 0 || l >= fn->nlabels || !placed[l])
                    diag_fatal(fn->file, 0, "internal: %s: a switch names "
                               "label L%d, which nothing places (after %s)",
                               fn->name, l, tag);
            }
        }
        free(placed);
    }
    /* (4) every call's argument array is its own (ir.h, argv): two calls
     * sharing one -- an instruction duplicated without ir_args_copy --
     * means renaming either renames both. */
    {
        int nc = 0;
        for (int n = 0; n < fn->nins; n++)
            if (fn->ins[n].op == IR_CALL && fn->ins[n].argv)
                nc++;
        if (nc > 1) {
            const struct ir_arg **pv = xmalloc((size_t)nc * sizeof *pv);
            nc = 0;
            for (int n = 0; n < fn->nins; n++)
                if (fn->ins[n].op == IR_CALL && fn->ins[n].argv)
                    pv[nc++] = fn->ins[n].argv;
            qsort(pv, (size_t)nc, sizeof *pv, cmp_argv_ptr);
            for (int k = 1; k < nc; k++)
                if (pv[k] == pv[k - 1])
                    diag_fatal(fn->file, 0, "internal: %s has two calls "
                               "sharing one argument array (after %s) -- a "
                               "pass copied a call without ir_args_copy",
                               fn->name, tag);
            free(pv);
        }
    }
    if (fn->var_scope_lo)
        for (int i = 0; i < fn->nvars; i++) {
            int lo = fn->var_scope_lo[i], hi = fn->var_scope_hi[i];
            if (lo < 0 || lo > fn->nins || hi < lo || hi > fn->nins)
                diag_fatal(fn->file, 0,
                    "internal: %s local %d has out-of-range scope [%d,%d] for nins=%d "
                    "(after %s) — a pass renumbered instructions without remapping "
                    "var_scope", fn->name, i, lo, hi, fn->nins, tag);
        }

    /* R3 / §9.1: "Every instruction carries a debug location. The verifier
     * rejects instructions without one, except where explicitly marked
     * compiler-synthesized."
     *
     * This is what stops provenance rotting quietly. A pass that builds a
     * replacement instruction from scratch, instead of copying the one it
     * replaces, drops the location -- and nothing downstream complains,
     * because a line table with a hole still links. The verifier is the only
     * place that can notice, and it runs after every optimizing compile
     * under EMBCC_VERIFY (which the whole test suite sets).
     */
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        if (i->line || i->synth)
            continue;
        diag_fatal(fn->file, 0,
            "internal: %s instruction %d (%s) has no source location after %s "
            "— a pass built it without copying the location of what it "
            "replaced; if it corresponds to no source construct, mark it "
            "synth", fn->name, n, ir_opname(i->op), tag);
    }
    free_defs(&d);
}
