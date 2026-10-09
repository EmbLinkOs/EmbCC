/* ---- a loop's back-edge copies, before the branch ----------------------
 *
 * Out of SSA, the values a loop carries are copied on its back edge, and
 * a bottom-tested loop gets them as a block of its own:
 *
 *        brnz %c -> L5        ...falls through to the exit
 *     L5: %62 = mov %52
 *        %64 = mov %11
 *        jmp L0              the header
 *
 * -- one jump on every iteration (417 such blocks in a sample of
 * EmbLinkOs). When nothing on the exit path reads what the copies write,
 * they can run before the branch whichever way it goes, and the branch
 * can go to the header itself: the copies execute once more on the way
 * out and write values nobody reads. Only for a copy block nothing else
 * enters -- one branch to its label, no fall-through into it -- and
 * never when a copy writes the branch's own condition.
 *
 * The copies go above the COMPARE that feeds the branch, not between the
 * two: every backend fuses a compare into the branch right after it, and
 * copies in between turned `cmp; jl` into `setl; test; jne` -- a loss on
 * every loop of the x86 workload. A copy that writes the compare's own
 * operand cannot go above it, and that loop is left alone. */

#include "opt_int.h"

struct lc_ref { int *n; };
static void lc_ref_cb(int *p, void *ctx)
{
    struct lc_ref *r = ctx;
    if (*p >= 0)
        r->n[*p]++;
}

static int lc_falls_through(enum ir_op op)
{
    return !(op == IR_JMP || op == IR_RET || op == IR_UD2 ||
             op == IR_IGOTO || op == IR_SWITCH);
}

int pass_latch_copies(struct ir_func *fn)
{
    int N = fn->nins, nl = fn->nlabels, nv = fn->nvregs;
    if (N < 4 || !nl || !nv)
        return 0;
    int *refs = xcalloc((size_t)nl, sizeof *refs);
    int *lpos = xmalloc((size_t)nl * sizeof *lpos);
    struct lc_ref r = { refs };
    for (int k = 0; k < nl; k++)
        lpos[k] = -1;
    for (int n = 0; n < N; n++) {
        each_label(fn, &fn->ins[n], lc_ref_cb, &r);
        if (fn->ins[n].op == IR_LABEL && fn->ins[n].label >= 0 &&
            fn->ins[n].label < nl)
            lpos[fn->ins[n].label] = n;
    }
    /* candidates: br[p] -> copy block [b0, b1) ending in jmp at b1 */
    int *cand_br = xmalloc((size_t)N * sizeof *cand_br);
    int ncand = 0;
    char *in_cand = xcalloc((size_t)N, 1);
    for (int p = 0; p + 1 < N; p++) {
        const struct ir_ins *br = &fn->ins[p];
        if ((br->op != IR_BRZ && br->op != IR_BRNZ) || br->label < 0 ||
            br->label >= nl || refs[br->label] != 1)
            continue;
        int b = lpos[br->label];
        if (b <= 0 || b == p + 1 || lc_falls_through(fn->ins[b - 1].op))
            continue;
        int e = b + 1;
        while (e < N && fn->ins[e].op == IR_MOV && !fn->ins[e].vol &&
               fn->ins[e].dst >= fn->nvars && fn->ins[e].dst < nv &&
               fn->ins[e].dst != br->a)
            e++;
        if (e == b + 1 || e >= N || fn->ins[e].op != IR_JMP)
            continue;
        if (in_cand[p] || in_cand[b])
            continue;
        cand_br[ncand++] = p;
        in_cand[p] = 1;
        for (int k = b; k <= e; k++)
            in_cand[k] = 1;
    }
    int changed = 0;
    if (ncand) {
        int *first = xmalloc((size_t)nv * sizeof *first);
        int *last = xmalloc((size_t)nv * sizeof *last);
        struct ra_live *lv = ra_live_compute(fn, first, last);
        int any_live = fn->nins > 0 && nv > 0;
        char *take = xcalloc((size_t)N, 1);    /* br index: rewrite it */
        char *above = xcalloc((size_t)N, 1);   /* ...with copies above p-1 */
        char *drop = xcalloc((size_t)N, 1);    /* the copy block's ins */
        for (int c = 0; c < ncand && any_live; c++) {
            int p = cand_br[c], b = lpos[fn->ins[p].label], e = b + 1;
            const struct ir_ins *cmp = p > 0 ? &fn->ins[p - 1] : NULL;
            int fused = cmp && cmp->op == IR_CMP &&
                        cmp->dst == fn->ins[p].a && !in_cand[p - 1];
            int ok = 1;
            while (fn->ins[e].op == IR_MOV) {
                int d = fn->ins[e].dst;
                if (ra_live_in_at(lv, fn, p + 1, d))
                    ok = 0;
                if (fused && (d == cmp->a || (!cmp->imm_b && d == cmp->b)))
                    ok = 0;
                e++;
            }
            if (!ok)
                continue;
            take[p] = 1;
            above[p] = (char)fused;
            for (int k = b; k <= e; k++)
                drop[k] = 1;
        }
        struct ibuf nb = { 0, 0, 0 };
        int *newpos = xmalloc((size_t)(N + 1) * sizeof *newpos);
        for (int n = 0; n < N; n++) {
            newpos[n] = nb.n;
            if (drop[n])
                continue;
            /* the compare of a branch whose copies go above it: they
             * first, then the compare, then (next n) the branch */
            int brn = n + 1 < N && take[n + 1] && above[n + 1] ? n + 1 :
                      take[n] && !above[n] ? n : -1;
            if (brn >= 0) {
                int k = lpos[fn->ins[brn].label] + 1;
                for (; fn->ins[k].op == IR_MOV; k++)
                    *ib_push(&nb) = fn->ins[k];
                changed = 1;
                g_did.latch++;
            }
            *ib_push(&nb) = fn->ins[n];
            if (take[n]) {
                int k = lpos[fn->ins[n].label] + 1;
                while (fn->ins[k].op == IR_MOV)
                    k++;
                nb.p[nb.n - 1].label = fn->ins[k].label;   /* the header */
            }
        }
        newpos[N] = nb.n;
        if (changed) {
            remap_scopes(fn, newpos, N);
            free(fn->ins);
            fn->ins = nb.p; fn->nins = nb.n; fn->cap = nb.cap;
        } else {
            free(nb.p);
        }
        free(newpos); free(take); free(above); free(drop);
        free(first); free(last); ra_live_free(lv);
    }
    free(refs); free(lpos); free(cand_br); free(in_cand);
    return changed;
}

/* ---- a jump to a block of copies takes the copies with it --------------
 *
 * A loop whose body ends in several places -- a switch's cases, an
 * interpreter's opcodes -- sends each end to one block that copies the
 * loop's carried values and jumps to the header: two jumps per trip
 * where one would do. A `jmp` to such a block (copies and a jump,
 * nothing else) can do the copies itself and jump on to the block's
 * target: the same instructions on the same path, so nothing about
 * liveness changes. The block goes once nothing reaches it. Not at
 * -Os, where it trades a jump executed for copies stored once per
 * predecessor. */
#define THREAD_MAX_COPIES 4

int pass_thread_copies(struct ir_func *fn)
{
    int N = fn->nins, nl = fn->nlabels;
    if (g_opt_size || N < 3 || !nl)
        return 0;
    int *lpos = xmalloc((size_t)nl * sizeof *lpos);
    int *refs = xcalloc((size_t)nl, sizeof *refs);
    struct lc_ref r = { refs };
    for (int k = 0; k < nl; k++)
        lpos[k] = -1;
    for (int n = 0; n < N; n++) {
        each_label(fn, &fn->ins[n], lc_ref_cb, &r);
        if (fn->ins[n].op == IR_LABEL && fn->ins[n].label >= 0 &&
            fn->ins[n].label < nl)
            lpos[fn->ins[n].label] = n;
    }
    /* copies[l]: the number of copies in label l's block when it is a
     * copy block ending in a jump elsewhere, else -1 */
    int *copies = xmalloc((size_t)nl * sizeof *copies);
    for (int l = 0; l < nl; l++) {
        copies[l] = -1;
        int b = lpos[l];
        if (b < 0)
            continue;
        int e = b + 1;
        while (e < N && fn->ins[e].op == IR_MOV && !fn->ins[e].vol)
            e++;
        if (e - b - 1 <= THREAD_MAX_COPIES && e < N &&
            fn->ins[e].op == IR_JMP && fn->ins[e].label != l &&
            e > b + 1)
            copies[l] = e - b - 1;
    }
    int changed = 0, *threaded = xcalloc((size_t)nl, sizeof *threaded);
    struct ibuf nb = { 0, 0, 0 };
    int *newpos = xmalloc((size_t)(N + 1) * sizeof *newpos);
    for (int n = 0; n < N; n++) {
        const struct ir_ins *in = &fn->ins[n];
        newpos[n] = nb.n;
        if (in->op == IR_JMP && in->label >= 0 && in->label < nl &&
            copies[in->label] > 0 && lpos[in->label] != n + 1) {
            int b = lpos[in->label];
            for (int k = 1; k <= copies[in->label]; k++)
                *ib_push(&nb) = fn->ins[b + k];
            struct ir_ins *j = ib_push(&nb);
            *j = *in;
            j->label = fn->ins[b + 1 + copies[in->label]].label;
            threaded[in->label]++;
            changed = 1;
            continue;
        }
        *ib_push(&nb) = *in;
    }
    newpos[N] = nb.n;
    if (changed) {
        remap_scopes(fn, newpos, N);
        free(fn->ins);
        fn->ins = nb.p; fn->nins = nb.n; fn->cap = nb.cap;
        /* a copy block every reference to which went: unreachable now
         * unless something falls into it, and DCE of blocks takes it */
    } else {
        free(nb.p);
    }
    free(newpos); free(threaded); free(copies); free(lpos); free(refs);
    return changed;
}
