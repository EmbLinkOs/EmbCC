/* ==== switch threading =====================================================
 *
 * A state machine is a loop around `switch (state)` whose arms set the
 * state to a constant: CoreMark's core_state, a hand-written lexer, a
 * protocol parser. Every arm knows which case the next iteration will
 * take, and the loop forgets it at the latch, so each character pays for
 * the whole dispatch again -- a bounds check, a table load and an
 * indirect jump.
 *
 * This keeps what the arm knew. The path from the latch to the switch --
 * the increment, the loop's own exit tests, anything else between them --
 * is copied once for each case the state can arrive at, and the copy ends
 * in a jump straight to that case instead of in the switch. An arm that
 * leaves the state alone counts as well: the case edge it was entered by
 * says what the state is. Every copy keeps the loop's exits, and writes
 * the state as the original did, for whatever reads it after the loop.
 * An arrival nothing is proven about still takes the original path, which
 * stays.
 *
 * What is known where is a forward dataflow over the few temps the
 * switch's operand is computed from -- copies, constants and arithmetic
 * with a constant, at the switch's own width -- to which a switch's case
 * edge adds the operand's value.
 *
 * Arms seldom end at the latch itself: an if/else inside one ends at a
 * join, which is where `state = x` meets `state = y`, and that block knows
 * nothing. So a block with one way out (a "latch" here) is copied too, and
 * its predecessors asked instead, up to SWT_FEED_DEPTH deep. Each block is
 * copied once per CASE, not once per way of reaching it: the copy of a
 * latch for case c jumps on to the copy of the next block for case c, and
 * all of them to one copy of the path for c. Sharing is sound because
 * every way into a copy for c is an arrival proven to reach c. */

#include "opt_int.h"

#define SWT_MAX_TRACK  32    /* temps followed back from the operand */
#define SWT_MAX_PATH   24    /* instructions on the path to the switch */
#define SWT_MAX_FEED   8     /* in one latch block copied in front of it */
#define SWT_FEED_DEPTH 6     /* how many latches deep */
#define SWT_MAX_COPIES 48    /* copies of blocks, per switch */
#define SWT_MAX_ADDED  400   /* instructions, per function, over every round */

/* 0: nothing known yet (unreached), 1: the constant k, 2: varies. */
struct swv { int st; long k; };

static struct swv swt_val(const int *tix, const struct swv *s, int v)
{
    struct swv r = { 2, 0 };
    if (v >= 0 && tix[v] >= 0)
        r = s[tix[v]];
    return r;
}

/* The value instruction i leaves in its destination, given what is known
 * before it. Only arithmetic at the switch's own width is followed, so
 * the low w bytes -- all the switch reads -- are exact whatever a backend
 * keeps above them; everything is normalised the way pass_fold does it. */
static struct swv swt_eval(struct ir_func *fn, struct defs *d,
                           const struct ir_ins *i, const int *tix,
                           const struct swv *s, int w)
{
    struct swv r = { 2, 0 }, a, b;
    long B;
    if (i->flt || i->w != w)
        return r;
    switch (i->op) {
    case IR_CONST:
        r.st = 1;
        r.k = norm(i->imm, w);
        return r;
    case IR_MOV:
        a = swt_val(tix, s, i->a);
        if (a.st == 1)
            a.k = norm(a.k, w);
        return a;
    case IR_EXT:
        a = swt_val(tix, s, i->a);
        if (a.st == 1)
            a.k = fold_ext(a.k, i->size, i->sign, w);
        return a;
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_AND: case IR_OR:
    case IR_XOR: case IR_SHL: case IR_SHR:
        a = swt_val(tix, s, i->a);
        if (a.st != 1)
            return a;
        if (i->imm_b) {
            b.st = 1; b.k = i->imm;
        } else if (get_const(fn, d, i->b, &B)) {
            b.st = 1; b.k = B;
        } else {
            b = swt_val(tix, s, i->b);
        }
        if (b.st != 1)
            return b;
        if (fold_bin(i->op, a.k, b.k, w, i->sign, B_EQ, &r.k))
            r.st = 1;
        return r;
    default:
        return r;
    }
}

/* Run instructions [lo, hi) over the state s. */
static void swt_run(struct ir_func *fn, struct defs *d, int lo, int hi,
                    const int *tix, struct swv *s, int w)
{
    for (int n = lo; n < hi; n++) {
        int t = def_target(&fn->ins[n]);
        if (t >= 0 && tix[t] >= 0)
            s[tix[t]] = swt_eval(fn, d, &fn->ins[n], tix, s, w);
    }
}

/* What the edge from block p to block t adds to s: a switch's case edge
 * names its operand's value when that case is the only way along it --
 * and, when the operand is `v - c` computed in that block, v's too. */
static void swt_edge(struct ir_func *fn, struct defs *d, const struct bb *bb,
                     int p, int t, const int *l2b, const int *tix,
                     struct swv *s, int w)
{
    if (bb[p].end <= bb[p].start)
        return;
    const struct ir_ins *sw = &fn->ins[bb[p].end - 1];
    if (sw->op != IR_SWITCH || sw->w != w || sw->a < 0 || tix[sw->a] < 0)
        return;
    if (sw->label >= 0 && sw->label < fn->nlabels && l2b[sw->label] == t)
        return;                                   /* the default goes there too */
    const struct ir_jt *jt = &fn->jt[sw->jt];
    int idx = -1, hits = 0;
    for (int k = 0; k < jt->n; k++)
        if (jt->labels[k] >= 0 && jt->labels[k] < fn->nlabels &&
            l2b[jt->labels[k]] == t) {
            idx = k;
            hits++;
        }
    if (hits != 1)
        return;
    s[tix[sw->a]].st = 1;
    s[tix[sw->a]].k = norm(idx, w);
    int x = sw->a, at = d->ins[x];
    if (d->cnt[x] != 1 || at < bb[p].start || at >= bb[p].end - 1)
        return;
    const struct ir_ins *sub = &fn->ins[at];
    long c, v;
    if (sub->op != IR_SUB || sub->flt || sub->w != w || sub->a < 0 ||
        tix[sub->a] < 0 || !const_b(fn, d, (struct ir_ins *)sub, &c))
        return;
    for (int n = at + 1; n < bb[p].end - 1; n++)
        if (def_target(&fn->ins[n]) == sub->a)
            return;
    if (fold_bin(IR_ADD, idx, c, w, 0, B_EQ, &v)) {
        s[tix[sub->a]].st = 1;
        s[tix[sub->a]].k = v;
    }
}

static int swt_meet(struct swv *x, struct swv y)
{
    if (y.st == 0 || x->st == 2)
        return 0;
    if (x->st == 0) { *x = y; return 1; }
    if (y.st == 2 || y.k != x->k) { x->st = 2; x->k = 0; return 1; }
    return 0;
}

/* Can this block be copied: nothing that is not plain code. */
static int swt_copyable(struct ir_func *fn, const struct bb *b, int *count)
{
    for (int n = b->start; n < b->end; n++) {
        switch (fn->ins[n].op) {
        case IR_LABEL:
            continue;
        case IR_ASM: case IR_ALLOCA: case IR_SPSAVE: case IR_SPRESTORE:
        case IR_LANDING: case IR_VA_START: case IR_LABELADDR: case IR_IGOTO:
        case IR_RET: case IR_UD2:
            return 0;
        case IR_SWITCH:
            if (n != b->end - 1)
                return 0;
            break;
        default:
            break;
        }
        (*count)++;
    }
    return 1;
}

static int swt_term(struct ir_func *fn, const struct bb *b)
{
    return b->end > b->start ? (int)fn->ins[b->end - 1].op : -1;
}

/* Does block b fall through into the block after it? */
static int swt_falls(struct ir_func *fn, const struct bb *b)
{
    int op = swt_term(fn, b);
    return op != IR_JMP && op != IR_RET && op != IR_UD2 &&
           op != IR_SWITCH && op != IR_IGOTO;
}

/* An arrival: block p reaches block h (the path's head, or a latch) and
 * is sent to copy g instead. A copy: block blk (-1: the whole path to the
 * switch) for the case whose label is target. */
struct swt_arr { int p, h, g; };
struct swt_grp { int blk, target, label, host, emitted; };

/* Everything the arrival search reads, and what it collects. */
struct swt_ctx {
    struct ir_func *fn;
    struct defs *d;
    const struct bb *bb;
    const int *l2b, *tix;
    int nt, w;
    const struct swv *out;              /* per block, nt each */
    const int *chain;
    int nch, head, sb;
    const struct ir_ins *sw;
    const int *bsize;                   /* a block's instructions, -1: not copyable */
    struct swt_grp grp[SWT_MAX_COPIES];
    int ngrp;
    struct swt_arr arr[128];
    int narr, npath;
    int *budget;
};

/* May block p be sent somewhere else: reachable, off the path, and
 * ending in something that names its targets one by one. */
static int swt_from(const struct swt_ctx *c, int p)
{
    for (int k = 0; k < c->nch; k++)
        if (c->chain[k] == p)
            return 0;
    int op = swt_term(c->fn, &c->bb[p]);
    return c->bb[p].rpo >= 0 && op != IR_SWITCH && op != IR_IGOTO;
}

static int swt_find(const struct swt_ctx *c, int blk, int target)
{
    for (int g = 0; g < c->ngrp; g++)
        if (c->grp[g].blk == blk && c->grp[g].target == target)
            return g;
    return -1;
}

/* The copy a latch's copy leads on to. */
static int swt_next(const struct swt_ctx *c, int g)
{
    int nb2 = c->bb[c->grp[g].blk].succ[0];
    return swt_find(c, nb2 == c->head ? -1 : nb2, c->grp[g].target);
}

/* Block p reaching block h: if what p knows at its end, carried through
 * the latches from h (none when h is the head) and the path, decides the
 * switch, record the arrival, and the copies it runs through. */
static int swt_arrive(struct swt_ctx *c, int p, int h)
{
    struct ir_func *fn = c->fn;
    const struct bb *bb = c->bb;
    struct swv s[SWT_MAX_TRACK];
    memcpy(s, &c->out[(size_t)p * c->nt], (size_t)c->nt * sizeof *s);
    swt_edge(fn, c->d, bb, p, h, c->l2b, c->tix, s, c->w);
    for (int b = h; b != c->head; b = bb[b].succ[0])
        swt_run(fn, c->d, bb[b].start, bb[b].end, c->tix, s, c->w);
    for (int k = 0; k < c->nch; k++)
        swt_run(fn, c->d, bb[c->chain[k]].start,
                k == c->nch - 1 ? bb[c->sb].end - 1 : bb[c->chain[k]].end,
                c->tix, s, c->w);
    struct swv xv = s[c->tix[c->sw->a]];
    if (xv.st != 1)
        return 0;
    const struct ir_jt *jt = &fn->jt[c->sw->jt];
    unsigned long idx = c->w == 8 ? (unsigned long)xv.k
                                  : (unsigned long)(unsigned int)xv.k;
    int target = idx < (unsigned long)jt->n ? jt->labels[idx] : c->sw->label;
    if (target < 0 || target >= fn->nlabels || c->l2b[target] < 0 ||
        c->narr == 128)
        return 0;
    /* the copies this needs that do not exist yet, and what they cost */
    int need = 0, cost = 0;
    for (int b = h; ; b = bb[b].succ[0]) {
        int blk = b == c->head ? -1 : b;
        if (swt_find(c, blk, target) < 0) {
            need++;
            cost += 1 + (blk < 0 ? c->npath : c->bsize[blk]);
        }
        if (blk < 0)
            break;
    }
    if (c->ngrp + need > SWT_MAX_COPIES || cost > *c->budget)
        return 0;
    *c->budget -= cost;
    for (int b = h; ; b = bb[b].succ[0]) {
        int blk = b == c->head ? -1 : b;
        if (swt_find(c, blk, target) < 0) {
            struct swt_grp *g = &c->grp[c->ngrp++];
            g->blk = blk;
            g->target = target;
            g->label = fn->nlabels++;
            g->host = -1;
            g->emitted = 0;
        }
        if (blk < 0)
            break;
    }
    c->arr[c->narr].p = p;
    c->arr[c->narr].h = h;
    c->arr[c->narr].g = swt_find(c, h == c->head ? -1 : h, target);
    c->narr++;
    return 1;
}

/* swt_local_cb: a read of v at position `pos` of the copied run. */
struct swt_lc { char *local; const int *defpos; int pos, nv; };
static void swt_local_cb(int *p, void *ctx)
{
    struct swt_lc *c = ctx;
    int v = *p;
    if (v < 0 || v >= c->nv || !c->local[v])
        return;
    if (c->pos < 0 || c->pos <= c->defpos[v])
        c->local[v] = 0;
}

/* Emit copy g, and after it every copy it leads on to that is not out
 * yet, so that each falls into the next. A temp is renamed when the
 * copy's own run of blocks is the only place it is defined and read;
 * one that crosses into the next copy keeps its name, because that copy
 * may be entered from others too. */
struct swt_emit {
    struct swt_ctx *c;
    struct ibuf *nb;
    const int *zlabel, *blabel;
    int nv;
    int *pos, *defpos, *ren;
    char *local;
};
static void swt_emit_copy(struct swt_emit *e, int g)
{
    struct swt_ctx *c = e->c;
    struct ir_func *fn = c->fn;
    const struct bb *bb = c->bb;
    while (g >= 0 && !c->grp[g].emitted) {
        struct swt_grp *G = &c->grp[g];
        G->emitted = 1;
        int run[64], nrun = 0;
        if (G->blk >= 0) {
            run[nrun++] = G->blk;
        } else {
            for (int k = 0; k < c->nch; k++)
                run[nrun++] = c->chain[k];
        }
        for (int n = 0; n < fn->nins; n++)
            e->pos[n] = -1;
        int ps = 0;
        for (int k = 0; k < nrun; k++)
            for (int n = bb[run[k]].start; n < bb[run[k]].end; n++)
                e->pos[n] = ps++;
        for (int v = 0; v < e->nv; v++) {
            e->defpos[v] = c->d->cnt[v] == 1 && c->d->ins[v] >= 0
                           ? e->pos[c->d->ins[v]] : -1;
            e->local[v] = v >= fn->nvars && e->defpos[v] >= 0;
            e->ren[v] = -1;
        }
        for (int n = 0; n < fn->nins; n++) {
            struct swt_lc lc = { e->local, e->defpos, e->pos[n], e->nv };
            each_read(&fn->ins[n], swt_local_cb, &lc);
        }
        const struct ir_ins *first = &fn->ins[bb[run[0]].start];
        struct ir_ins *l = ib_push(e->nb);
        l->op = IR_LABEL;
        l->label = G->label;
        l->line = first->line; l->col = first->col; l->synth = 1;
        int last_line = first->line, last_col = first->col;
        for (int k = 0; k < nrun; k++) {
            int b = run[k], lastblk = k == nrun - 1;
            int next = lastblk ? -1 : run[k + 1];
            for (int n = bb[b].start; n < bb[b].end; n++) {
                const struct ir_ins *o = &fn->ins[n];
                int term = n == bb[b].end - 1;
                if (o->op == IR_LABEL)
                    continue;
                last_line = o->line; last_col = o->col;
                if (term && (o->op == IR_SWITCH || o->op == IR_JMP))
                    break;          /* the jump on is emitted below */
                int inv = 0;
                if (term && (o->op == IR_BRZ || o->op == IR_BRNZ)) {
                    if (lastblk)
                        break;      /* a latch: both ways lead on */
                    if (c->l2b[o->label] == next) {
                        if (b + 1 == next)
                            break;  /* both ways lead on */
                        inv = 1;    /* on along the taken edge */
                    }
                }
                struct ir_ins *q = ib_push(e->nb);
                *q = *o;
                ins_own_args(q);
                struct lcopy lcp = { e->ren, e->nv, 0 };
                each_read(q, lcopy_cb, &lcp);
                int t = def_target(o);
                if (t >= 0 && t < e->nv) {
                    if (e->local[t]) {
                        q->dst = fn->nvregs++;
                        e->ren[t] = q->dst;
                    } else {
                        e->ren[t] = -1;
                    }
                }
                if (inv) {
                    q->op = o->op == IR_BRZ ? IR_BRNZ : IR_BRZ;
                    q->label = e->blabel[b + 1] >= 0 ? e->blabel[b + 1]
                                                     : e->zlabel[b + 1];
                }
            }
        }
        /* on: to the case, or to the copy of the next block for it */
        int next_g = G->blk >= 0 ? swt_next(c, g) : -1;
        struct ir_ins *j = ib_push(e->nb);
        j->op = IR_JMP;
        j->label = next_g >= 0 ? c->grp[next_g].label : G->target;
        j->line = last_line; j->col = last_col; j->synth = 1;
        g = next_g;
    }
}

static int swt_one(struct ir_func *fn, int *budget)
{
    if (fn->nins == 0 || fn->nvregs == 0)
        return 0;
    int nsw = 0;
    for (int n = 0; n < fn->nins; n++) {
        if (fn->ins[n].op == IR_ASM)
            return 0;               /* its outputs are not def_target's */
        nsw += fn->ins[n].op == IR_SWITCH;
    }
    if (!nsw)
        return 0;
    int nbb, *l2b;
    struct bb *bb = build_cfg(fn, &nbb, &l2b);
    int *order = xmalloc((size_t)(nbb ? nbb : 1) * sizeof *order), norder;
    compute_rpo(bb, nbb, order, &norder);
    struct defs d;
    compute_defs(fn, &d);
    int nv = fn->nvregs, done = 0;
    int *tix = xmalloc((size_t)nv * sizeof *tix);
    struct swv *out = NULL;
    struct swt_ctx *c = xcalloc(1, sizeof *c);
    int *bsize = xmalloc((size_t)nbb * sizeof *bsize);
    for (int b = 0; b < nbb; b++) {
        bsize[b] = 0;
        if (!swt_copyable(fn, &bb[b], &bsize[b]))
            bsize[b] = -1;
    }

    for (int sb = 0; sb < nbb && !done; sb++) {
        if (bb[sb].rpo < 0 || swt_term(fn, &bb[sb]) != IR_SWITCH ||
            bsize[sb] < 0)
            continue;
        const struct ir_ins *sw = &fn->ins[bb[sb].end - 1];
        int w = sw->w, x = sw->a, swline = sw->line;
        if ((w != 4 && w != 8) || x < fn->nvars || x >= nv)
            continue;

        /* ---- the path: back from the switch while there is one way in */
        int chain[64], nch = 0, npath = bsize[sb];
        int cur = sb;
        chain[nch++] = sb;
        while (bb[cur].npred == 1 && nch < 64) {
            int p = bb[cur].pred[0], seen = 0;
            for (int k = 0; k < nch; k++)
                seen |= chain[k] == p;
            int op = swt_term(fn, &bb[p]);
            if (seen || bb[p].rpo < 0 || op == IR_SWITCH || op == IR_IGOTO ||
                bb[p].nsucc > 2 || bsize[p] < 0)
                break;
            npath += bsize[p];
            memmove(chain + 1, chain, (size_t)nch * sizeof *chain);
            chain[0] = p;
            nch++;
            cur = p;
        }
        if (npath > SWT_MAX_PATH)
            continue;
        int head = chain[0];

        /* ---- the temps the operand is computed from ---- */
        int nt = 0;
        for (int v = 0; v < nv; v++)
            tix[v] = -1;
        tix[x] = nt++;
        for (int grew = 1; grew; ) {
            grew = 0;
            for (int n = 0; n < fn->nins; n++) {
                const struct ir_ins *i = &fn->ins[n];
                int t = def_target(i), src[2] = { -1, -1 };
                if (t < 0 || tix[t] < 0 || i->flt || i->w != w)
                    continue;
                switch (i->op) {
                case IR_MOV: case IR_EXT:
                    src[0] = i->a;
                    break;
                case IR_ADD: case IR_SUB: case IR_MUL: case IR_AND:
                case IR_OR: case IR_XOR: case IR_SHL: case IR_SHR:
                    src[0] = i->a;
                    if (!i->imm_b) src[1] = i->b;
                    break;
                default:
                    break;
                }
                for (int k = 0; k < 2; k++) {
                    int v = src[k];
                    if (v >= fn->nvars && v < nv && tix[v] < 0 &&
                        nt < SWT_MAX_TRACK) {
                        tix[v] = nt++;
                        grew = 1;
                    }
                }
            }
        }

        /* ---- what is known at the end of every block ---- */
        free(out);
        out = xcalloc((size_t)nbb * (size_t)nt, sizeof *out);
        struct swv tmp[SWT_MAX_TRACK], nin[SWT_MAX_TRACK];
        for (int again = 1, guard = 0; again && guard++ < 64; ) {
            again = 0;
            for (int oi = 0; oi < norder; oi++) {
                int b = order[oi];
                for (int k = 0; k < nt; k++) {
                    nin[k].st = b == 0 ? 2 : 0;   /* entry: unknown, not unseen */
                    nin[k].k = 0;
                }
                for (int q = 0; b != 0 && q < bb[b].npred; q++) {
                    int p = bb[b].pred[q];
                    if (bb[p].rpo < 0)
                        continue;
                    memcpy(tmp, &out[(size_t)p * nt], (size_t)nt * sizeof *tmp);
                    swt_edge(fn, &d, bb, p, b, l2b, tix, tmp, w);
                    for (int k = 0; k < nt; k++)
                        swt_meet(&nin[k], tmp[k]);
                }
                swt_run(fn, &d, bb[b].start, bb[b].end, tix, nin, w);
                struct swv *o = &out[(size_t)b * nt];
                for (int k = 0; k < nt; k++)
                    if (o[k].st != nin[k].st ||
                        (nin[k].st == 1 && o[k].k != nin[k].k)) {
                        o[k] = nin[k];
                        again = 1;
                    }
            }
        }

        /* ---- the arrivals the analysis proves ----
         *
         * A predecessor of the path's head may know the operand at its
         * end. One that does not may be a latch -- a block with one way
         * out -- whose own predecessors do, so those are asked in turn. */
        memset(c, 0, sizeof *c);
        c->fn = fn; c->d = &d; c->bb = bb; c->l2b = l2b; c->tix = tix;
        c->nt = nt; c->w = w; c->out = out; c->chain = chain; c->nch = nch;
        c->head = head; c->sb = sb; c->sw = sw; c->bsize = bsize;
        c->npath = npath; c->budget = budget;
        int *fdepth = xmalloc((size_t)nbb * sizeof *fdepth);
        for (int b = 0; b < nbb; b++)
            fdepth[b] = -1;
        int wl[64], nwl = 0;
        for (int q = 0; q < bb[head].npred; q++) {
            int Q = bb[head].pred[q];
            if (!swt_from(c, Q) || swt_arrive(c, Q, head))
                continue;
            if (Q != 0 && bb[Q].nsucc == 1 && bb[Q].npred > 0 &&
                bsize[Q] >= 0 && bsize[Q] <= SWT_MAX_FEED &&
                fdepth[Q] < 0 && nwl < 64) {
                fdepth[Q] = 1;
                wl[nwl++] = Q;
            }
        }
        for (int at = 0; at < nwl; at++) {
            int F = wl[at];
            for (int r = 0; r < bb[F].npred; r++) {
                int P = bb[F].pred[r];
                if (!swt_from(c, P) || fdepth[P] >= 0 || P == F ||
                    swt_arrive(c, P, F))
                    continue;
                if (fdepth[F] < SWT_FEED_DEPTH && P != 0 &&
                    bb[P].nsucc == 1 && bb[P].npred > 0 &&
                    bsize[P] >= 0 && bsize[P] <= SWT_MAX_FEED && nwl < 64) {
                    fdepth[P] = fdepth[F] + 1;
                    wl[nwl++] = P;
                }
            }
        }
        free(fdepth);
        if (!c->narr)
            continue;

        /* ---- where each copy goes: after an arrival that can fall into
         * it, the last such in the layout; the copies it leads on to
         * follow it. What has no such arrival goes at the end. ---- */
        char *hosting = xcalloc((size_t)nbb, 1);
        for (int a = 0; a < c->narr; a++) {
            int P = c->arr[a].p, H = c->arr[a].h;
            struct swt_grp *G = &c->grp[c->arr[a].g];
            int canhost = (swt_falls(fn, &bb[P]) && P + 1 == H) ||
                          (swt_term(fn, &bb[P]) == IR_JMP &&
                           bb[P].nsucc == 1 && bb[P].succ[0] == H);
            if (canhost && !hosting[P] && (G->host < 0 || P > G->host)) {
                if (G->host >= 0)
                    hosting[G->host] = 0;
                G->host = P;
                hosting[P] = 1;
            }
        }
        int lastop = fn->ins[fn->nins - 1].op;
        if (lastop != IR_JMP && lastop != IR_RET && lastop != IR_UD2 &&
            lastop != IR_SWITCH && lastop != IR_IGOTO) {
            /* A function that falls off its end: nothing can go after
             * it, so every copy must be placed by a host -- its own, or
             * one leading on to it. */
            int all = 1;
            for (int g = 0; g < c->ngrp; g++) {
                int placed = c->grp[g].host >= 0;
                for (int h = 0; h < c->ngrp && !placed; h++)
                    placed = c->grp[h].blk >= 0 && swt_next(c, h) == g;
                all &= placed;
            }
            if (!all) {
                free(hosting);
                continue;
            }
        }

        /* A block a copy branches to by name, where the original fell
         * into it (a path block that continues along its branch's taken
         * edge, so the copy branches the other way). */
        int *zlabel = xmalloc((size_t)nbb * sizeof *zlabel);
        int *blabel = xmalloc((size_t)nbb * sizeof *blabel);
        for (int b = 0; b < nbb; b++) {
            zlabel[b] = -1;
            blabel[b] = bb[b].end > bb[b].start &&
                        fn->ins[bb[b].start].op == IR_LABEL
                        ? fn->ins[bb[b].start].label : -1;
        }
        for (int k = 0; k + 1 < nch; k++) {
            int cb = chain[k], op = swt_term(fn, &bb[cb]);
            if ((op == IR_BRZ || op == IR_BRNZ) &&
                l2b[fn->ins[bb[cb].end - 1].label] == chain[k + 1] &&
                cb + 1 != chain[k + 1] && blabel[cb + 1] < 0 &&
                zlabel[cb + 1] < 0)
                zlabel[cb + 1] = fn->nlabels++;
        }

        struct ibuf nb = { 0, 0, 0 };
        struct swt_emit e;
        e.c = c; e.nb = &nb; e.zlabel = zlabel; e.blabel = blabel;
        e.nv = nv;
        e.pos = xmalloc((size_t)(fn->nins ? fn->nins : 1) * sizeof *e.pos);
        e.defpos = xmalloc((size_t)nv * sizeof *e.defpos);
        e.ren = xmalloc((size_t)nv * sizeof *e.ren);
        e.local = xmalloc((size_t)nv);
        int *newpos = xmalloc((size_t)(fn->nins + 1) * sizeof *newpos);
        for (int b = 0; b < nbb; b++) {
            if (zlabel[b] >= 0) {
                struct ir_ins *l = ib_push(&nb);
                l->op = IR_LABEL;
                l->label = zlabel[b];
                l->line = fn->ins[bb[b].start].line;
                l->col = fn->ins[bb[b].start].col;
                l->synth = 1;
            }
            for (int n = bb[b].start; n < bb[b].end; n++) {
                newpos[n] = nb.n;
                struct ir_ins *q = ib_push(&nb);
                *q = fn->ins[n];
                if (n != bb[b].end - 1 ||
                    (q->op != IR_JMP && q->op != IR_BRZ && q->op != IR_BRNZ))
                    continue;
                for (int a = 0; a < c->narr; a++)
                    if (c->arr[a].p == b && q->label >= 0 &&
                        q->label < fn->nlabels && l2b[q->label] == c->arr[a].h)
                        q->label = c->grp[c->arr[a].g].label;
            }
            /* an arrival that fell into its block: into the copy now */
            int falls_to = swt_falls(fn, &bb[b]) && b + 1 < nbb ? b + 1 : -1;
            int hosted = -1;
            for (int g = 0; g < c->ngrp; g++)
                if (c->grp[g].host == b)
                    hosted = g;
            for (int a = 0; a < c->narr; a++)
                if (c->arr[a].p == b && c->arr[a].h == falls_to &&
                    c->arr[a].g != hosted) {
                    struct ir_ins *j = ib_push(&nb);
                    j->op = IR_JMP;
                    j->label = c->grp[c->arr[a].g].label;
                    j->line = fn->ins[bb[b].end - 1].line;
                    j->col = fn->ins[bb[b].end - 1].col;
                    j->synth = 1;
                    break;
                }
            if (hosted >= 0)
                swt_emit_copy(&e, hosted);
        }
        for (int g = 0; g < c->ngrp; g++)
            if (!c->grp[g].emitted)
                swt_emit_copy(&e, g);
        newpos[fn->nins] = nb.n;
        remap_scopes(fn, newpos, fn->nins);
        free(newpos);
        free(fn->ins);
        fn->ins = nb.p; fn->nins = nb.n; fn->cap = nb.cap;
        free(e.pos); free(e.defpos); free(e.ren); free(e.local);
        free(zlabel); free(blabel); free(hosting);
        if (remarks_on() && fn->src)
            remark_add("switch-thread", "threaded", fn->name, "state-known",
                       fn->file, swline,
                       "%d arrivals at this switch go straight to their "
                       "case, through %d copied blocks", c->narr, c->ngrp);
        done = 1;
    }
    free(out); free(tix); free(c); free(bsize);
    free_defs(&d); free(order); free(l2b);
    free_cfg(bb, nbb);
    return done;
}

int pass_swthread(struct ir_func *fn)
{
    int budget = SWT_MAX_ADDED, changed = 0, guard = 0;
    while (guard++ < 8 && budget > 0 && swt_one(fn, &budget))
        changed = 1;
    return changed;
}
