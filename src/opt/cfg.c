/* The control-flow graph: basic blocks, reverse postorder, the dominator
 * tree (Cooper-Harvey-Kennedy), dominance frontiers and natural loops --
 * what mem2reg, global CSE, SCCP, LICM, PRE and the loop passes reason
 * with -- and opt_cfg_dump, which prints that same graph. */

#include "opt_int.h"

void free_cfg(struct bb *bb, int nbb)
{
    for (int i = 0; i < nbb; i++) { free(bb[i].pred); free(bb[i].succ); }
    free(bb);
}

/* Add t to block i's successors, once. build_cfg adds every successor
 * of block i before moving on to i + 1, so whether t is already there is
 * a stamp to ask, not a list to walk -- a switch of 4000 cases has 4000
 * successors. */
static void cfg_succ(struct bb *bb, int *seen, int i, int t)
{
    if (seen[t] == i + 1)
        return;                     /* an edge once is an edge */
    seen[t] = i + 1;
    struct bb *b = &bb[i];
    if (b->nsucc == b->capsucc) {
        b->capsucc = b->capsucc ? b->capsucc * 2 : 2;
        b->succ = xrealloc(b->succ, (size_t)b->capsucc * sizeof *b->succ);
    }
    b->succ[b->nsucc++] = t;
}

/* Build basic blocks + succ/pred + a label->block map. */
struct bb *build_cfg(struct ir_func *fn, int *nbb_out, int **l2b_out)
{
    int N = fn->nins;
    char *lead = xcalloc((size_t)(N ? N : 1), 1);
    if (N) lead[0] = 1;
    for (int i = 0; i < N; i++) {
        enum ir_op op = fn->ins[i].op;
        if (op == IR_LABEL) lead[i] = 1;
        if ((op == IR_JMP || op == IR_BRZ || op == IR_BRNZ ||
             op == IR_RET || op == IR_UD2 || op == IR_IGOTO ||
             op == IR_SWITCH) && i + 1 < N)
            lead[i + 1] = 1;
    }
    int nbb = 0;
    for (int i = 0; i < N; i++) if (lead[i]) nbb++;
    if (nbb == 0) nbb = 1;
    struct bb *bb = xcalloc((size_t)nbb, sizeof *bb);
    int b = 0, prev = 0;
    for (int i = 1; i <= N; i++)
        if (i == N || lead[i]) { bb[b].start = prev; bb[b].end = i; prev = i; b++; }
    for (int i = 0; i < nbb; i++) {
        bb[i].idom = -1; bb[i].rpo = -1;
        bb[i].dpre = bb[i].dpost = -1;
    }

    int *l2b = xmalloc((size_t)(fn->nlabels ? fn->nlabels : 1) * sizeof *l2b);
    for (int l = 0; l < fn->nlabels; l++) l2b[l] = -1;
    for (int i = 0; i < nbb; i++)
        if (bb[i].end > bb[i].start && fn->ins[bb[i].start].op == IR_LABEL)
            l2b[fn->ins[bb[i].start].label] = i;

    /* Which labels a computed goto could land on: the ones something
     * takes the address of. Nothing else can be the target of `goto *p`
     * -- the value has to have come from a `&&label` somewhere in this
     * function, because that is the only thing that produces one. */
    char *taken_lbl = NULL;
    for (int i = 0; i < N; i++)
        if (fn->ins[i].op == IR_LABELADDR && fn->ins[i].label >= 0 &&
            fn->ins[i].label < fn->nlabels) {
            if (!taken_lbl)
                taken_lbl = xcalloc((size_t)(fn->nlabels ? fn->nlabels : 1), 1);
            taken_lbl[fn->ins[i].label] = 1;
        }

    /* seen[t] == i + 1: t is already a successor of i (cfg_succ) */
    int *seen = xcalloc((size_t)nbb, sizeof *seen);
    for (int i = 0; i < nbb; i++) {
        enum ir_op op = bb[i].end > bb[i].start ? fn->ins[bb[i].end - 1].op : IR_UD2;
        int L = bb[i].end > bb[i].start ? fn->ins[bb[i].end - 1].label : -1;
        if (op == IR_RET || op == IR_UD2) {
            /* no successors */
        } else if (op == IR_JMP) {
            if (l2b[L] >= 0) cfg_succ(bb, seen, i, l2b[L]);
        } else if (op == IR_BRZ || op == IR_BRNZ) {
            if (l2b[L] >= 0) cfg_succ(bb, seen, i, l2b[L]);
            if (i + 1 < nbb) cfg_succ(bb, seen, i, i + 1);
        } else if (op == IR_SWITCH) {
            /* the default, and every entry of the table */
            const struct ir_ins *sw = &fn->ins[bb[i].end - 1];
            if (L >= 0 && L < fn->nlabels && l2b[L] >= 0)
                cfg_succ(bb, seen, i, l2b[L]);
            for (int k = 0; k < fn->jt[sw->jt].n; k++) {
                int tl = fn->jt[sw->jt].labels[k];
                if (tl >= 0 && tl < fn->nlabels && l2b[tl] >= 0)
                    cfg_succ(bb, seen, i, l2b[tl]);
            }
        } else if (op == IR_IGOTO) {
            /* An edge to every address-taken label. Without these the
             * blocks they open look unreachable, and the passes that
             * DELETE unreachable code delete the program. */
            for (int l = 0; taken_lbl && l < fn->nlabels; l++)
                if (taken_lbl[l] && l2b[l] >= 0)
                    cfg_succ(bb, seen, i, l2b[l]);
        } else if (i + 1 < nbb) {
            cfg_succ(bb, seen, i, i + 1);
        }
    }
    free(taken_lbl);
    free(seen);
    /* Predecessors by source block, then by successor. A block's
     * successors are distinct, so none is a duplicate, and each list is
     * allocated once at its size: added one at a time, each checked every
     * earlier one and grew the array by one, and for the block after a
     * 4000-case switch that was the hottest thing in the -O2 compile. */
    for (int i = 0; i < nbb; i++)
        for (int k = 0; k < bb[i].nsucc; k++)
            bb[bb[i].succ[k]].npred++;
    for (int i = 0; i < nbb; i++) {
        if (bb[i].npred)
            bb[i].pred = xmalloc((size_t)bb[i].npred * sizeof *bb[i].pred);
        bb[i].npred = 0;
    }
    for (int i = 0; i < nbb; i++)
        for (int k = 0; k < bb[i].nsucc; k++) {
            struct bb *t = &bb[bb[i].succ[k]];
            t->pred[t->npred++] = i;
        }
    free(lead);
    *nbb_out = nbb;
    *l2b_out = l2b;
    return bb;
}

/* Reverse-postorder numbering from the entry (block 0). */
void compute_rpo(struct bb *bb, int nbb, int *order, int *norder)
{
    char *seen = xcalloc((size_t)nbb, 1);
    int *stk = xmalloc((size_t)nbb * sizeof *stk), *it = xmalloc((size_t)nbb * sizeof *it);
    int sp = 0, po = 0, *post = xmalloc((size_t)nbb * sizeof *post);
    stk[sp] = 0; it[sp] = 0; seen[0] = 1;
    while (sp >= 0) {
        int u = stk[sp];
        if (it[sp] < bb[u].nsucc) {
            int w = bb[u].succ[it[sp]++];
            if (!seen[w]) { seen[w] = 1; sp++; stk[sp] = w; it[sp] = 0; }
        } else { post[po++] = u; sp--; }
    }
    *norder = po;
    for (int i = 0; i < po; i++) order[i] = post[po - 1 - i];   /* reverse */
    for (int i = 0; i < po; i++) bb[order[i]].rpo = i;
    free(seen); free(stk); free(it); free(post);
}

static int idom_intersect(struct bb *bb, int a, int b)
{
    while (a != b) {
        while (bb[a].rpo > bb[b].rpo) a = bb[a].idom;
        while (bb[b].rpo > bb[a].rpo) b = bb[b].idom;
    }
    return a;
}

/* Immediate dominators (Cooper-Harvey-Kennedy) over the reachable blocks. */
void compute_idom(struct bb *bb, int *order, int norder)
{
    bb[0].idom = 0;
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int i = 1; i < norder; i++) {       /* skip entry, RPO order */
            int b = order[i], nd = -1;
            for (int k = 0; k < bb[b].npred; k++) {
                int p = bb[b].pred[k];
                if (bb[p].idom < 0) continue;    /* not yet processed */
                nd = nd < 0 ? p : idom_intersect(bb, nd, p);
            }
            if (nd >= 0 && bb[b].idom != nd) { bb[b].idom = nd; changed = 1; }
        }
    }
    /* Number the dominator tree depth-first, so that "does s dominate b"
     * is two comparisons (bb_dominates) rather than a walk up b's
     * dominators -- which on a chain of 4000 else-ifs is a walk of
     * thousands, asked of every edge by every loop pass. */
    int nbb = 0;
    for (int i = 0; i < norder; i++)
        if (order[i] + 1 > nbb) nbb = order[i] + 1;
    int *kid = xcalloc((size_t)nbb + 1, sizeof *kid);
    int *kids = xmalloc((size_t)(norder ? norder : 1) * sizeof *kids);
    int *stk = xmalloc((size_t)(norder ? norder : 1) * sizeof *stk);
    int *it = xmalloc((size_t)(norder ? norder : 1) * sizeof *it);
    for (int i = 0; i < norder; i++) {
        int b = order[i];
        bb[b].dpre = bb[b].dpost = -1;
        if (b != 0 && bb[b].idom >= 0 && bb[b].idom < nbb) kid[bb[b].idom + 1]++;
    }
    for (int b = 0; b < nbb; b++) kid[b + 1] += kid[b];
    {
        int *fill = xmalloc((size_t)(nbb ? nbb : 1) * sizeof *fill);
        for (int b = 0; b < nbb; b++) fill[b] = kid[b];
        for (int i = 0; i < norder; i++) {
            int b = order[i];
            if (b != 0 && bb[b].idom >= 0 && bb[b].idom < nbb)
                kids[fill[bb[b].idom]++] = b;
        }
        free(fill);
    }
    int pre = 0, post = 0, sp = 0;
    if (norder > 0) { stk[sp] = 0; it[sp] = kid[0]; sp++; bb[0].dpre = pre++; }
    while (sp > 0) {
        int u = stk[sp - 1];
        if (it[sp - 1] < kid[u + 1]) {
            int c = kids[it[sp - 1]++];
            if (bb[c].dpre >= 0) continue;
            bb[c].dpre = pre++;
            stk[sp] = c; it[sp] = kid[c]; sp++;
        } else {
            bb[u].dpost = post++;
            sp--;
        }
    }
    free(kid); free(kids); free(stk); free(it);
}

/* Dominance frontiers. df[b] holds the blocks on b's frontier. */
void compute_df(struct bb *bb, int nbb, int **df, int *ndf)
{
    /* b only ever joins a frontier while b is the block being looked at,
     * so a repeat of b can only be the last entry: that is the whole
     * duplicate test, where a scan of the frontier so far was one per
     * step -- and each list grows by doubling, not by one. */
    int *cap = xcalloc((size_t)(nbb ? nbb : 1), sizeof *cap);
    for (int b = 0; b < nbb; b++) {
        if (bb[b].npred < 2) continue;
        for (int k = 0; k < bb[b].npred; k++) {
            int r = bb[b].pred[k];
            while (r >= 0 && r != bb[b].idom) {
                if (!(ndf[r] > 0 && df[r][ndf[r] - 1] == b)) {
                    if (ndf[r] == cap[r]) {
                        cap[r] = cap[r] ? cap[r] * 2 : 4;
                        df[r] = xrealloc(df[r], (size_t)cap[r] * sizeof(int));
                    }
                    df[r][ndf[r]++] = b;
                }
                r = bb[r].idom;
            }
        }
    }
    free(cap);
}

/* Where p is in b's predecessor list, or -1. build_cfg lists them in
 * increasing block order, once each, so a binary search finds the one
 * index a scan from the front would: a join after a 4000-case switch has
 * 4000 predecessors, and mem2reg asked this once per incoming edge. */
int bb_pred_index(const struct bb *b, int p)
{
    int lo = 0, hi = b->npred - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (b->pred[mid] == p) return mid;
        if (b->pred[mid] < p) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}
/* Does block `s` dominate block `b`? The idom chain is already built;
 * the entry is its own idom, which is where the walk stops. */
int bb_dominates(struct bb *bb, int s, int b)
{
    if (b == s)
        return 1;
    /* s is an ancestor of b in the dominator tree (compute_idom's
     * numbering). Off the tree -- unreachable, or no dominators
     * computed -- nothing dominates but the block itself. */
    if (bb[b].dpre < 0 || bb[s].dpre < 0)
        return 0;
    return bb[s].dpre <= bb[b].dpre && bb[b].dpost <= bb[s].dpost;
}

/* The natural loop of the back edge tail -> h: h, plus every block that
 * reaches tail without passing through h. */
void loop_body(struct bb *bb, int nbb, int h, int tail, char *in)
{
    int *stk = xmalloc((size_t)nbb * sizeof *stk), sp = 0;
    in[h] = 1;
    if (!in[tail]) { in[tail] = 1; stk[sp++] = tail; }
    while (sp) {
        int b = stk[--sp];
        for (int k = 0; k < bb[b].npred; k++) {
            int p = bb[b].pred[k];
            if (!in[p]) { in[p] = 1; stk[sp++] = p; }
        }
    }
    free(stk);
}

/* ---- the CFG, as a report (opt.h, vision §18) ----
 *
 * Built by build_cfg, the same function mem2reg, global CSE and SCCP use,
 * so what this prints is the graph the passes reason about. Dominators are
 * computed too, because "which block dominates which" is the question a
 * mem2reg or a CSE bug always turns into.
 */
void opt_cfg_dump(struct outbuf *b, struct ir_func *fn)
{
    if (fn->nins == 0) {
        ob_fmt(b, "function %s: no instructions\n", fn->name);
        return;
    }
    int nbb, *l2b;
    struct bb *bb = build_cfg(fn, &nbb, &l2b);
    int *order = xmalloc((size_t)(nbb ? nbb : 1) * sizeof *order), norder;
    compute_rpo(bb, nbb, order, &norder);
    int reachable = norder == nbb;
    if (reachable)
        compute_idom(bb, order, norder);

    ob_fmt(b, "function %s: %d blocks, %d instructions\n",
           fn->name, nbb, fn->nins);
    if (!reachable)
        ob_fmt(b, "  (%d block%s unreachable: dominators not computed)\n",
               nbb - norder, nbb - norder == 1 ? "" : "s");
    for (int i = 0; i < nbb; i++) {
        ob_fmt(b, "  B%-3d ins [%d,%d)", i, bb[i].start, bb[i].end);
        /* The label a block carries, when it has one: it is what the
         * instruction dump calls it, so the two can be read together. */
        if (bb[i].end > bb[i].start && fn->ins[bb[i].start].op == IR_LABEL)
            ob_fmt(b, " L%d", fn->ins[bb[i].start].label);
        if (bb[i].end > bb[i].start) {
            const struct ir_ins *t = &fn->ins[bb[i].end - 1];
            ob_fmt(b, "  ends %s", ir_opname(t->op));
            if (t->line)
                ob_fmt(b, " (line %d)", t->line);
        }
        ob_str(b, "\n");
        if (bb[i].npred) {
            ob_str(b, "       from");
            for (int k = 0; k < bb[i].npred; k++)
                ob_fmt(b, " B%d", bb[i].pred[k]);
            ob_str(b, "\n");
        }
        if (bb[i].nsucc) {
            ob_str(b, "       to  ");
            for (int k = 0; k < bb[i].nsucc; k++)
                ob_fmt(b, " B%d", bb[i].succ[k]);
            ob_str(b, "\n");
        } else {
            ob_str(b, "       to   (exit)\n");
        }
        if (reachable && bb[i].idom >= 0 && bb[i].idom != i)
            ob_fmt(b, "       idom B%d\n", bb[i].idom);
        /* A back edge is a successor that dominates this block: that is
         * what makes it a loop, and it is worth naming. */
        for (int k = 0; reachable && k < bb[i].nsucc; k++) {
            int sdom = bb[i].succ[k], q = i;
            while (q >= 0 && q != sdom)
                q = bb[q].idom == q ? -1 : bb[q].idom;
            if (q == sdom)
                ob_fmt(b, "       back edge to B%d (a loop)\n", sdom);
        }
    }
    free(order);
    free(l2b);
    free_cfg(bb, nbb);
}
