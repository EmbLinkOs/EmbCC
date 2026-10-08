/* ==== a join's copies, coalesced ==========================================
 *
 * Leaving SSA puts a copy on every edge into a join, and nested choices
 * make chains of them: `st = c ? (d ? 1 : 2) : 3` is
 *
 *      %197 = const 1 ... jmp L21      L21: %198 = mov %197
 *      %198 = const 2 ... jmp L19      L19: %199 = mov %198
 *                                      L14: %188 = mov %199; jmp L5
 *
 * Every backend's allocator gives the four names one register and the
 * copies vanish -- leaving L21, L19 and L14 as blocks that do nothing but
 * fall or jump to the next, so each arm pays a jump to a jump. CoreMark's
 * state machine took one per character. Done here, in the IR, the copies
 * go and the blocks are empty before cfgclean threads the jumps.
 *
 * `%b = mov %a` takes %a's name away -- every definition of %a writes %b
 * instead, and the copy goes -- when %a is a temp the copy is the one
 * reader of, of the same width and class as %b, and the two never hold
 * different live values at once: %b is dead right after each definition
 * of %a (its old value is not wanted), and %a is dead right after each
 * other definition of %b (no %a value is in flight to the copy when %b
 * is written). That is the allocator's own interference test, made here
 * where the CFG still knows which blocks become empty. Not when a
 * definition of %a reads %b: that is an update in place, which needs
 * nothing from here. Liveness is per block, updated by union as names
 * merge; each question about one point is a walk to the end of its
 * block. Functions with inline asm (whose outputs are not definitions to
 * the rest of this file) or exception edges are left alone. */

#include "opt_int.h"

/* Block liveness as a list per vreg of the blocks it is live out of,
 * ascending -- the least solution of the usual equations over build_cfg's
 * blocks, found one vreg at a time from the blocks that read it before
 * writing it, back through predecessors to the blocks that write it.
 * `slots` says whether an LDVAR's or ADDR's slot operand counts as a read.
 *
 * What it replaces was a bit set per block of every vreg, iterated to a
 * fixpoint: blocks x vregs, which for a function of 4000 if statements
 * is 12000 blocks by 36000 vregs, 54 MB a set and four sets. */
struct vblk { int **b, *n, nv; };

struct vblk_ue { int *ust, *dst, b, nv; int *pv, *pb, np, cap; };
static void vblk_pair(struct vblk_ue *u, int v, int b)
{
    if (u->np == u->cap) {
        u->cap = u->cap ? u->cap * 2 : 256;
        u->pv = xrealloc(u->pv, (size_t)u->cap * sizeof *u->pv);
        u->pb = xrealloc(u->pb, (size_t)u->cap * sizeof *u->pb);
    }
    u->pv[u->np] = v;
    u->pb[u->np] = b;
    u->np++;
}
static void vblk_ue_cb(int *p, void *ctx)
{
    struct vblk_ue *u = ctx;
    int v = *p;
    if (v < 0 || v >= u->nv || u->dst[v] == u->b + 1 || u->ust[v] == u->b + 1)
        return;
    u->ust[v] = u->b + 1;
    vblk_pair(u, v, u->b);
}

/* (v, b) pairs -> per key, the other side, stably */
static void vblk_csr(const int *key, const int *val, int np, int nkey,
                     int **off_out, int **val_out)
{
    int *off = xcalloc((size_t)nkey + 1, sizeof *off);
    int *out = xmalloc((size_t)(np ? np : 1) * sizeof *out);
    for (int j = 0; j < np; j++) off[key[j] + 1]++;
    for (int k = 0; k < nkey; k++) off[k + 1] += off[k];
    int *fill = xmalloc((size_t)(nkey ? nkey : 1) * sizeof *fill);
    for (int k = 0; k < nkey; k++) fill[k] = off[k];
    for (int j = 0; j < np; j++) out[fill[key[j]]++] = val[j];
    free(fill);
    *off_out = off;
    *val_out = out;
}

static void vblk_build(struct ir_func *fn, const struct bb *bb, int nbb,
                       int slots, struct vblk *lv)
{
    int nv = fn->nvregs;
    lv->nv = nv;
    lv->b = xcalloc((size_t)(nv ? nv : 1), sizeof *lv->b);
    lv->n = xcalloc((size_t)(nv ? nv : 1), sizeof *lv->n);
    struct vblk_ue ue = { NULL, NULL, 0, nv, NULL, NULL, 0, 0 };
    struct vblk_ue df = { NULL, NULL, 0, nv, NULL, NULL, 0, 0 };
    ue.ust = xcalloc((size_t)(nv ? nv : 1), sizeof *ue.ust);
    ue.dst = xcalloc((size_t)(nv ? nv : 1), sizeof *ue.dst);
    for (int b = 0; b < nbb; b++) {
        ue.b = b;
        for (int n = bb[b].start; n < bb[b].end; n++) {
            struct ir_ins *i = &fn->ins[n];
            if (slots || (i->op != IR_LDVAR && i->op != IR_ADDR))
                each_read(i, vblk_ue_cb, &ue);
            int t = def_target(i);
            if (t >= 0 && t < nv && ue.dst[t] != b + 1) {
                ue.dst[t] = b + 1;
                vblk_pair(&df, t, b);
            }
        }
    }
    free(ue.ust); free(ue.dst);
    int *ueoff, *ueb, *dfoff, *dfb;
    vblk_csr(ue.pv, ue.pb, ue.np, nv, &ueoff, &ueb);
    vblk_csr(df.pv, df.pb, df.np, nv, &dfoff, &dfb);
    free(ue.pv); free(ue.pb); free(df.pv); free(df.pb);
    int *inmk = xcalloc((size_t)(nbb ? nbb : 1), sizeof *inmk);
    int *outmk = xcalloc((size_t)(nbb ? nbb : 1), sizeof *outmk);
    int *defmk = xcalloc((size_t)(nbb ? nbb : 1), sizeof *defmk);
    int *work = xmalloc((size_t)(nbb ? nbb : 1) * sizeof *work);
    int *tmp = xmalloc((size_t)(nbb ? nbb : 1) * sizeof *tmp);
    for (int v = 0; v < nv; v++) {
        if (ueoff[v] == ueoff[v + 1])
            continue;
        for (int k = dfoff[v]; k < dfoff[v + 1]; k++) defmk[dfb[k]] = v + 1;
        int nw = 0, nt = 0;
        for (int k = ueoff[v]; k < ueoff[v + 1]; k++) {
            inmk[ueb[k]] = v + 1;
            work[nw++] = ueb[k];
        }
        while (nw > 0) {
            int b = work[--nw];
            for (int k = 0; k < bb[b].npred; k++) {
                int p = bb[b].pred[k];
                if (outmk[p] != v + 1) {
                    outmk[p] = v + 1;
                    tmp[nt++] = p;
                }
                if (defmk[p] != v + 1 && inmk[p] != v + 1) {
                    inmk[p] = v + 1;
                    work[nw++] = p;
                }
            }
        }
        if (nt == 0)
            continue;
        /* ascending: an insertion sort is fine for the short lists, and a
         * marked pass over the blocks for a long one */
        int *l = xmalloc((size_t)nt * sizeof *l);
        if (nt <= 32) {
            for (int k = 0; k < nt; k++) {
                int x = tmp[k], j = k;
                while (j > 0 && l[j - 1] > x) { l[j] = l[j - 1]; j--; }
                l[j] = x;
            }
        } else {
            int m = 0;
            for (int b = 0; b < nbb; b++)
                if (outmk[b] == v + 1) l[m++] = b;
        }
        lv->b[v] = l;
        lv->n[v] = nt;
    }
    free(inmk); free(outmk); free(defmk); free(work); free(tmp);
    free(ueoff); free(ueb); free(dfoff); free(dfb);
}

static int vblk_has(const struct vblk *lv, int v, int b)
{
    if (v < 0 || v >= lv->nv)
        return 0;
    const int *l = lv->b[v];
    int lo = 0, hi = lv->n[v] - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (l[mid] == b) return 1;
        if (l[mid] < b) lo = mid + 1;
        else hi = mid - 1;
    }
    return 0;
}

/* b is now live wherever a was, and a nowhere */
static void vblk_merge(struct vblk *lv, int a, int b)
{
    int na = lv->n[a], nb2 = lv->n[b], m = 0, i = 0, j = 0;
    if (na == 0)
        return;
    int *l = xmalloc((size_t)(na + nb2) * sizeof *l);
    const int *x = lv->b[a], *y = lv->b[b];
    while (i < na || j < nb2) {
        int c;
        if (j >= nb2 || (i < na && x[i] < y[j])) c = x[i++];
        else if (i >= na || y[j] < x[i]) c = y[j++];
        else { c = x[i]; i++; j++; }
        l[m++] = c;
    }
    free(lv->b[a]); free(lv->b[b]);
    lv->b[a] = NULL; lv->n[a] = 0;
    lv->b[b] = l; lv->n[b] = m;
}

static void vblk_free(struct vblk *lv)
{
    for (int v = 0; v < lv->nv; v++) free(lv->b[v]);
    free(lv->b); free(lv->n);
}

static int jc_live_after(struct ir_func *fn, const char *gone, int n, int end,
                         int v, const struct vblk *lout, int blk)
{
    for (int k = n + 1; k < end; k++) {
        if (gone[k])
            continue;
        if (ins_reads(&fn->ins[k], v))
            return 1;
        if (def_target(&fn->ins[k]) == v)
            return 0;
    }
    return vblk_has(lout, v, blk);
}
struct jc_cnt { int *use; int nv; };
static void jc_cnt_cb(int *p, void *ctx)
{
    struct jc_cnt *c = ctx;
    if (*p >= 0 && *p < c->nv) c->use[*p]++;
}
int pass_joincopies(struct ir_func *fn)
{
    if (fn->nins == 0 || fn->nvregs == 0 || fn->neh)
        return 0;
    int nv = fn->nvregs, nins = fn->nins;
    for (int n = 0; n < nins; n++)
        if (fn->ins[n].op == IR_ASM)
            return 0;
    int *nuse = xcalloc((size_t)nv, sizeof *nuse);
    int *vw = xcalloc((size_t)nv, sizeof *vw);
    char *vflt = xcalloc((size_t)nv, 1), *mixed = xcalloc((size_t)nv, 1);
    for (int n = 0; n < nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        struct jc_cnt c = { nuse, nv };
        if (i->op != IR_LDVAR && i->op != IR_ADDR)
            each_read(i, jc_cnt_cb, &c);
        int t = i->op == IR_STVAR ? -1 : def_target(i);
        if (t < 0 || t >= nv)
            continue;
        int w = i->op == IR_CALL ? (i->w ? i->w : 8) : i->w;
        if (!vw[t]) { vw[t] = w; vflt[t] = (char)i->flt; }
        else if (vw[t] != w || vflt[t] != (char)i->flt) mixed[t] = 1;
    }
    int any = 0;
    for (int n = 0; n < nins && !any; n++) {
        const struct ir_ins *i = &fn->ins[n];
        if (i->op == IR_MOV && i->a >= fn->nvars && i->a < nv &&
            i->dst >= fn->nvars && i->dst < nv && i->a != i->dst &&
            nuse[i->a] == 1)
            any = 1;
    }
    int changed = 0, nbb, *l2b;
    struct bb *bb = NULL;
    if (!any)
        goto out0;
    bb = build_cfg(fn, &nbb, &l2b);
    int *blk = xmalloc((size_t)nins * sizeof *blk);
    for (int n = 0; n < nins; n++) blk[n] = -1;
    for (int b = 0; b < nbb; b++)
        for (int n = bb[b].start; n < bb[b].end; n++) blk[n] = b;
    struct vblk lout;
    vblk_build(fn, bb, nbb, 0, &lout);
    char *gone = xcalloc((size_t)nins, 1);
    /* The instructions defining each vreg, in a list per vreg, kept true
     * as names merge: the questions below are about the definitions of
     * two vregs, and finding them by asking every instruction was
     * instructions x copies -- on a chain of 4000 joins, most of the
     * -O2 compile once the passes before it were linear. */
    int *dfirst = xmalloc((size_t)nv * sizeof *dfirst);
    int *dnext = xmalloc((size_t)nins * sizeof *dnext);
    for (int v = 0; v < nv; v++) dfirst[v] = -1;
    for (int n = nins - 1; n >= 0; n--) {
        int t = def_target(&fn->ins[n]);
        dnext[n] = -1;
        if (t >= 0 && t < nv) { dnext[n] = dfirst[t]; dfirst[t] = n; }
    }
    for (int m = 0; m < nins; m++) {
        struct ir_ins *i = &fn->ins[m];
        if (gone[m] || i->op != IR_MOV || blk[m] < 0)
            continue;
        int a = i->a, b = i->dst;
        if (a < fn->nvars || a >= nv || b < fn->nvars || b >= nv || a == b ||
            nuse[a] != 1 || mixed[a] || mixed[b] || vw[a] != vw[b] ||
            vw[a] != i->w || vflt[a] != vflt[b] || vflt[a] != (char)i->flt)
            continue;
        int ok = 1, nd = 0;
        for (int pass = 0; pass < 2 && ok; pass++)
        for (int d = dfirst[pass ? b : a]; d >= 0 && ok; d = dnext[d]) {
            if (gone[d] || d == m || blk[d] < 0)
                continue;
            int t = def_target(&fn->ins[d]);
            if (t == a) {
                nd++;
                /* `%a = add %b, 1; %b = mov %a` is a loop's own update:
                 * every allocator already gives the two one register,
                 * and writing it in place only moves its heuristics --
                 * RV32's spilled a pointer in the next loop of the
                 * workload's `text`, 5% more instructions. */
                if (ins_reads(&fn->ins[d], b))
                    ok = 0;
                if (jc_live_after(fn, gone, d, bb[blk[d]].end, b,
                                  &lout, blk[d]))
                    ok = 0;
            } else if (t == b) {
                if (jc_live_after(fn, gone, d, bb[blk[d]].end, a,
                                  &lout, blk[d]))
                    ok = 0;
            }
        }
        if (!ok || nd == 0)
            continue;
        /* a's definitions become b's, and move to b's list (in order,
         * though nothing asks for one) */
        int keep = -1, *kt = &keep;
        for (int d = dfirst[a], nx; d >= 0; d = nx) {
            nx = dnext[d];
            if (!gone[d] && d != m && fn->ins[d].op != IR_STVAR) {
                fn->ins[d].dst = b;
                dnext[d] = dfirst[b];
                dfirst[b] = d;
            } else {
                *kt = d;
                kt = &dnext[d];
            }
        }
        *kt = -1;
        dfirst[a] = keep;
        gone[m] = 1;
        nuse[a] = 0;
        vblk_merge(&lout, a, b);
        changed = 1;
    }
    if (changed) {
        struct ibuf nb = { 0, 0, 0 };
        int *newpos = fn->var_scope_lo
            ? xmalloc((size_t)(nins + 1) * sizeof *newpos) : NULL;
        for (int n = 0; n < nins; n++) {
            if (newpos) newpos[n] = nb.n;
            if (!gone[n])
                *ib_push(&nb) = fn->ins[n];
        }
        if (newpos) {
            newpos[nins] = nb.n;
            remap_scopes(fn, newpos, nins);
            free(newpos);
        }
        free(fn->ins);
        fn->ins = nb.p; fn->nins = nb.n; fn->cap = nb.cap;
    }
    free(gone); free(blk);
    vblk_free(&lout);
    free(dfirst); free(dnext);
    free(l2b);
    free_cfg(bb, nbb);
out0:
    free(nuse); free(vw); free(vflt); free(mixed);
    return changed;
}
