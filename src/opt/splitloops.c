/* ==== splitting a live range around a loop ================================
 *
 * A value that crosses a call takes a callee-saved register, and keeps
 * it everywhere it lives. When the call it crosses is OUTSIDE a loop and
 * the loop has none, the loop pays on every iteration: the loop's own
 * call hands the value over in an argument register and back in the
 * return register, and each trip copies it into the callee-saved one
 * and out again (kernels_main's `s = opaque_add(s, i)`: nine
 * instructions an iteration where seven do). The allocator cannot see
 * this -- one vreg, one register -- so the IR gives it two: inside the
 * loop the value goes by a new name, copied in on the entry edge and
 * back on every exit edge, and the new name crosses nothing.
 *
 * Shapes taken: a natural loop with one entry, from the block that falls
 * into its header; no switch, no indirect jump, no inline asm or landing
 * pad inside; exits that fall out, jump out, or branch out through a
 * trampoline that does the copy. Values taken: temps live into the
 * header, read or written inside, live across an IR_CALL in a block
 * outside the loop and across NONE inside it -- a call inside is fine
 * when the value is what it passes and returns (`s = opaque_add(s, i)`
 * hands s over in r0 and gets it back there), which is exactly the case
 * that pays. A call is an IR_CALL or an op the backend lowers to a
 * runtime helper (target_op_calls_helper: a soft-float divide after the
 * loop is what the Cortex-M4 bench crosses); the allocator judges by the
 * same predicate. Not at -Os: the copies are bytes. This runs last,
 * after every copy propagation that would fold the copies back in, one
 * loop per call. */

#include "opt_int.h"

struct splitrd { int from, to; };
static void split_rd_cb(int *p, void *ctx)
{
    struct splitrd *c = ctx;
    if (*p == c->from) *p = c->to;
}
struct splituse { unsigned long *use, *def; int nv; };
static void split_use_cb(int *p, void *ctx)
{
    struct splituse *c = ctx;
    int v = *p;
    if (v < 0 || v >= c->nv) return;
    if (!(c->def[v >> 6] & (1UL << (v & 63))))
        c->use[v >> 6] |= 1UL << (v & 63);
}
#define BIT(set, v) ((set)[(v) >> 6] & (1UL << ((v) & 63)))

int pass_splitloops(struct ir_func *fn)
{
    if (fn->nins == 0 || fn->nvregs == 0)
        return 0;
    int nbb, *l2b;
    struct bb *bb = build_cfg(fn, &nbb, &l2b);
    int *order = xmalloc((size_t)(nbb ? nbb : 1) * sizeof *order), norder;
    compute_rpo(bb, nbb, order, &norder);
    int changed = 0;
    if (norder != nbb)
        goto out;
    compute_idom(bb, order, norder);
    /* Nothing to split without a loop, and the liveness below is blocks x
     * vregs bits: a 4000-case switch, which has no loop at all, built and
     * swept 200 MB of it here. The headers looked for are the ones the
     * walk below would take. */
    {
        int any_loop = 0;
        for (int h = 1; h < nbb && !any_loop; h++) {
            if (bb[h].end <= bb[h].start || fn->ins[bb[h].start].op != IR_LABEL)
                continue;
            for (int q = 0; q < bb[h].npred && !any_loop; q++)
                if (bb_dominates(bb, h, bb[h].pred[q]))
                    any_loop = 1;
        }
        if (!any_loop)
            goto out;
    }
    int nv = fn->nvregs, words = (nv + 63) / 64;

    /* The width and class of each temp, from any definition. */
    int *vw = xcalloc((size_t)nv, sizeof *vw);
    char *vflt = xcalloc((size_t)nv, 1);
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        int t = i->op == IR_STVAR ? -1 : def_target(i);
        if (t < 0 || t >= nv || vw[t]) continue;
        /* (a compare's w and flt are its operands': ir_result_w) */
        vw[t] = i->op == IR_CALL ? (i->w ? i->w : 8) : ir_result_w(i);
        vflt[t] = (char)ir_result_flt(i);
    }

    /* Block liveness. */
    unsigned long *use = xcalloc((size_t)nbb * words, sizeof *use);
    unsigned long *def = xcalloc((size_t)nbb * words, sizeof *def);
    unsigned long *lin = xcalloc((size_t)nbb * words, sizeof *lin);
    unsigned long *lout = xcalloc((size_t)nbb * words, sizeof *lout);
    for (int b = 0; b < nbb; b++) {
        struct splituse su = { use + (size_t)b * words, def + (size_t)b * words, nv };
        for (int n = bb[b].start; n < bb[b].end; n++) {
            each_read(&fn->ins[n], split_use_cb, &su);
            int t = def_target(&fn->ins[n]);
            if (t >= 0 && t < nv) su.def[t >> 6] |= 1UL << (t & 63);
        }
    }
    for (int again = 1; again; ) {
        again = 0;
        for (int b = nbb - 1; b >= 0; b--) {
            unsigned long *o = lout + (size_t)b * words;
            for (int w = 0; w < words; w++) o[w] = 0;
            for (int k = 0; k < bb[b].nsucc; k++) {
                unsigned long *si = lin + (size_t)bb[b].succ[k] * words;
                for (int w = 0; w < words; w++) o[w] |= si[w];
            }
            unsigned long *ii = lin + (size_t)b * words;
            for (int w = 0; w < words; w++) {
                unsigned long nvl = use[(size_t)b * words + w] |
                                    (o[w] & ~def[(size_t)b * words + w]);
                if (nvl != ii[w]) { ii[w] = nvl; again = 1; }
            }
        }
    }
    /* Per block: the temps live across a call in it. */
    unsigned long *xcall = xcalloc((size_t)nbb * words, sizeof *xcall);
    {
        unsigned long *live = xmalloc((size_t)words * sizeof *live);
        for (int b = 0; b < nbb; b++) {
            for (int w = 0; w < words; w++) live[w] = lout[(size_t)b * words + w];
            for (int n = bb[b].end - 1; n >= bb[b].start; n--) {
                const struct ir_ins *i = &fn->ins[n];
                int t = def_target(i);
                if (i->op == IR_CALL || target_op_calls_helper(i))
                    for (int w = 0; w < words; w++) {
                        unsigned long m = live[w];
                        if (t >= 0 && (t >> 6) == w) m &= ~(1UL << (t & 63));
                        xcall[(size_t)b * words + w] |= m;
                    }
                if (t >= 0 && t < nv) live[t >> 6] &= ~(1UL << (t & 63));
                struct splituse su = { live, live, nv };   /* def = live: always set */
                /* a read is live before the instruction whatever follows */
                su.def = xcalloc((size_t)words, sizeof *su.def);
                each_read(&fn->ins[n], split_use_cb, &su);
                free(su.def);
            }
        }
        free(live);
    }

    char *in = xmalloc((size_t)(nbb ? nbb : 1));
    unsigned long *xout = xmalloc((size_t)words * sizeof *xout);
    unsigned long *inside = xmalloc((size_t)words * sizeof *inside);
    /* Headers innermost first. */
    for (int oi = norder - 1; oi >= 0 && !changed; oi--) {
        int h = order[oi];
        if (h == 0 || bb[h].end <= bb[h].start ||
            fn->ins[bb[h].start].op != IR_LABEL)
            continue;
        int Lh = fn->ins[bb[h].start].label;
        memset(in, 0, (size_t)nbb);
        int any_back = 0;
        for (int q = 0; q < bb[h].npred; q++) {   /* back edges into h */
            int p = bb[h].pred[q];
            if (bb_dominates(bb, h, p)) {
                loop_body(bb, nbb, h, p, in);
                any_back = 1;
            }
        }
        if (!any_back)
            continue;
        /* One entry, from the block that falls into the header. */
        int ok = 1, pe = -1;
        for (int b = 0; b < nbb && ok; b++) {
            if (!in[b]) continue;
            for (int k = 0; k < bb[b].npred; k++) {
                int p = bb[b].pred[k];
                if (in[p]) continue;
                if (b != h || pe >= 0) ok = 0;
                else pe = p;
            }
        }
        if (!ok || pe != h - 1 || bb[pe].end != bb[h].start || bb[pe].end <= bb[pe].start)
            continue;
        {
            const struct ir_ins *lt = &fn->ins[bb[pe].end - 1];
            if (lt->op == IR_JMP || lt->op == IR_RET || lt->op == IR_UD2 ||
                lt->op == IR_IGOTO || lt->op == IR_SWITCH)
                continue;                       /* no fall-through edge */
            if ((lt->op == IR_BRZ || lt->op == IR_BRNZ) && lt->label == Lh)
                continue;                       /* the taken edge skips the copy */
        }
        /* Nothing inside the loop a copy could not be placed around. */
        for (int w = 0; w < words; w++) inside[w] = 0;
        for (int b = 0; b < nbb && ok; b++) {
            if (!in[b]) continue;
            for (int n = bb[b].start; n < bb[b].end; n++) {
                const struct ir_ins *i = &fn->ins[n];
                if (i->op == IR_SWITCH || i->op == IR_IGOTO ||
                    i->op == IR_ASM || i->op == IR_LANDING || i->op == IR_LABELADDR)
                    { ok = 0; break; }
                int t = def_target(i);
                if (t >= 0 && t < nv) inside[t >> 6] |= 1UL << (t & 63);
            }
            for (int w = 0; w < words; w++) inside[w] |= use[(size_t)b * words + w];
        }
        if (!ok)
            continue;
        /* xout: live across a call outside the loop; xin: across one inside */
        unsigned long *xin = inside;      /* reused below, after the candidate test */
        unsigned long *xinb = xmalloc((size_t)words * sizeof *xinb);
        for (int w = 0; w < words; w++) { xout[w] = 0; xinb[w] = 0; }
        for (int b = 0; b < nbb; b++)
            for (int w = 0; w < words; w++)
                (in[b] ? xinb : xout)[w] |= xcall[(size_t)b * words + w];
        (void)xin;
        /* The candidates: live into the header, touched inside, live
         * across a call outside and across none inside. */
        int v = -1;
        for (int c = fn->nvars; c < nv; c++)
            if (BIT(lin + (size_t)h * words, c) && BIT(inside, c) && BIT(xout, c) &&
                !BIT(xinb, c) && (vw[c] == 4 || vw[c] == 8)) { v = c; break; }
        free(xinb);
        if (v < 0)
            continue;
        /* ---- rewrite ---- */
        int v2 = fn->nvregs++;
        int lineh = fn->ins[bb[h].start].line, colh = fn->ins[bb[h].start].col;
        struct splitrd rd = { v, v2 };
        for (int b = 0; b < nbb; b++) {
            if (!in[b]) continue;
            for (int n = bb[b].start; n < bb[b].end; n++) {
                struct ir_ins *i = &fn->ins[n];
                each_read(i, split_rd_cb, &rd);
                if (i->op != IR_STVAR && def_target(i) == v)
                    i->dst = v2;
            }
        }
        /* Where the copies go: `at` is an instruction index, kind 0 = before
         * it, 1 = after it (at == nins: the end). Trampolines are appended. */
        struct cp { int at, kind, dst, src; } *cps = NULL; int ncps = 0, ccps = 0;
        struct tr { int lbl, dst, src, to; } *trs = NULL; int ntrs = 0, ctrs = 0;
#define ADDCP(a, k, d, s) do { if (ncps == ccps) { ccps = ccps ? ccps * 2 : 8; \
            cps = xrealloc(cps, (size_t)ccps * sizeof *cps); } \
            cps[ncps].at = (a); cps[ncps].kind = (k); cps[ncps].dst = (d); \
            cps[ncps].src = (s); ncps++; } while (0)
        ADDCP(bb[h].start, 0, v2, v);                 /* the entry edge */
        for (int b = 0; b < nbb; b++) {
            if (!in[b]) continue;
            struct ir_ins *lt = &fn->ins[bb[b].end - 1];
            for (int k = 0; k < bb[b].nsucc; k++) {
                int s = bb[b].succ[k];
                if (in[s]) continue;
                if (!BIT(lin + (size_t)s * words, v))
                    continue;                     /* dead past this exit */
                int Ls = bb[s].end > bb[s].start && fn->ins[bb[s].start].op == IR_LABEL
                         ? fn->ins[bb[s].start].label : -1;
                int falls = bb[b].end == bb[s].start &&
                            lt->op != IR_JMP && lt->op != IR_RET && lt->op != IR_UD2;
                if (lt->op == IR_JMP && lt->label == Ls) {
                    ADDCP(bb[b].end - 1, 0, v, v2);  /* before the jump */
                } else if ((lt->op == IR_BRZ || lt->op == IR_BRNZ) && lt->label == Ls &&
                           Ls >= 0) {
                    if (ntrs == ctrs) { ctrs = ctrs ? ctrs * 2 : 4;
                        trs = xrealloc(trs, (size_t)ctrs * sizeof *trs); }
                    trs[ntrs].lbl = fn->nlabels++; trs[ntrs].dst = v;
                    trs[ntrs].src = v2; trs[ntrs].to = Ls;
                    lt->label = trs[ntrs].lbl;
                    ntrs++;
                    if (falls && bb[b].end == bb[s].start)
                        ADDCP(bb[b].end - 1, 1, v, v2);  /* both arms leave */
                } else if (falls) {
                    ADDCP(bb[b].end - 1, 1, v, v2);      /* after the block */
                } else {
                    ok = 0;                            /* an edge of no known shape */
                }
            }
        }
        if (!ok) {
            /* undo the rename: the shapes above are the only ones, so this
             * cannot happen, but a split half done is a miscompile */
            diag_fatal(fn->file, lineh, "internal: %s: a loop exit of a shape "
                       "the live-range split does not know", fn->name);
        }
        /* ---- rebuild ---- */
        struct ibuf nb = { 0, 0, 0 };
        int *newpos = fn->var_scope_lo
            ? xmalloc((size_t)(fn->nins + 1) * sizeof *newpos) : NULL;
        for (int n = 0; n <= fn->nins; n++) {
            if (newpos) newpos[n] = nb.n;
            for (int c = 0; c < ncps; c++)
                if (cps[c].at == n && cps[c].kind == 0) {
                    struct ir_ins *m = ib_push(&nb);
                    memset(m, 0, sizeof *m);
                    m->op = IR_MOV; m->dst = cps[c].dst; m->a = cps[c].src; m->b = -1;
                    m->w = vw[v]; m->flt = vflt[v]; m->line = lineh; m->col = colh;
                    m->synth = 1;
                }
            if (n == fn->nins) break;
            *ib_push(&nb) = fn->ins[n];
            for (int c = 0; c < ncps; c++)
                if (cps[c].at == n && cps[c].kind == 1) {
                    struct ir_ins *m = ib_push(&nb);
                    memset(m, 0, sizeof *m);
                    m->op = IR_MOV; m->dst = cps[c].dst; m->a = cps[c].src; m->b = -1;
                    m->w = vw[v]; m->flt = vflt[v]; m->line = lineh; m->col = colh;
                    m->synth = 1;
                }
        }
        /* The trampolines go after the last instruction -- so if that
         * one can fall through, it would fall INTO the first of them: a
         * void function's implicit return ran the exit copy and jumped
         * back to the code after the loop, and lib/libcxx's rb-tree
         * erase never returned. Cap it with the return it stands for,
         * as mem2reg's trampolines do. */
        if (ntrs > 0) {
            enum ir_op lt = nb.n ? nb.p[nb.n - 1].op : IR_UD2;
            if (lt != IR_JMP && lt != IR_RET && lt != IR_UD2 &&
                lt != IR_IGOTO && lt != IR_SWITCH) {
                struct ir_ins *r = ib_push(&nb);
                memset(r, 0, sizeof *r);
                r->op = IR_RET; r->dst = r->a = r->b = -1;
                r->synth = 1;
            }
        }
        for (int t = 0; t < ntrs; t++) {
            struct ir_ins *l = ib_push(&nb);
            memset(l, 0, sizeof *l);
            l->op = IR_LABEL; l->label = trs[t].lbl; l->dst = l->a = l->b = -1;
            l->line = lineh; l->col = colh; l->synth = 1;
            struct ir_ins *m = ib_push(&nb);
            memset(m, 0, sizeof *m);
            m->op = IR_MOV; m->dst = trs[t].dst; m->a = trs[t].src; m->b = -1;
            m->w = vw[v]; m->flt = vflt[v]; m->line = lineh; m->col = colh; m->synth = 1;
            struct ir_ins *j = ib_push(&nb);
            memset(j, 0, sizeof *j);
            j->op = IR_JMP; j->label = trs[t].to; j->dst = j->a = j->b = -1;
            j->line = lineh; j->col = colh; j->synth = 1;
        }
        if (newpos) {
            remap_scopes(fn, newpos, fn->nins);
            free(newpos);
        }
        free(fn->ins);
        fn->ins = nb.p; fn->nins = nb.n; fn->cap = nb.cap;
        free(cps); free(trs);
#undef ADDCP
        if (remarks_on() && fn->src)
            remark_add("regalloc", "split", fn->name, "loop-live-range",
                       fn->file, lineh ? lineh : fn->line,
                       "a value live across a call outside a loop goes by "
                       "another name inside it");
        changed = 1;
    }
    free(in); free(xout); free(inside);
    free(use); free(def); free(lin); free(lout); free(xcall);
    free(vw); free(vflt);
out:
    free(order); free(l2b);
    free_cfg(bb, nbb);
    return changed;
}
#undef BIT
