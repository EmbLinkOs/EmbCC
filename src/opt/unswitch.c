/* Loop unswitching (-O3): a test the loop cannot change, made once.
 *
 *   for (i = 0; i < n; i++)            if (mode)
 *       if (mode) a[i] += 3;               for (...) a[i] += 3;
 *       else      a[i] = a[i] * 5 - 1;  else
 *                                          for (...) a[i] = a[i] * 5 - 1;
 *
 * The loop is copied whole. The branch on `mode` is tested once, before
 * it, and picks a copy; in each copy that branch's answer is known, so it
 * is gone from one and a jump in the other, and the block-local passes
 * that follow fold what is left. It costs the loop's size again, so it is
 * an -O3 transform, budgeted per loop and per function, and it runs on
 * the outermost loop the test does not change, so the test is made as
 * rarely as it can be.
 *
 * Which loops. A natural loop: a header h, the blocks that reach a back
 * edge into it without passing it, h dominating each back edge's block.
 * Its blocks need not be one range of instructions -- by the time this
 * runs, a loop's rarer side is often laid out after its latch, jumping
 * back in -- and nothing but h is entered from outside it.
 *
 * Which tests. A brz or brnz that is not a back edge, on a value nothing
 * in the loop writes, that is not a constant (SCCP's job), and, when it
 * is a frame slot read as a value, one whose address is never taken: a
 * store through a pointer could otherwise change it inside the loop.
 *
 * The layout:
 *
 *   D: B c -> CP      the test, in a block of its own ahead of h, where
 *                     every way into the loop from outside now goes; B is
 *                     the branch's own brz/brnz
 *   [the loop]        as it was, the branch dropped: never taken here
 *   ...
 *   CP: moves in      after the loop's last block:
 *   [the loop]        the copy, the branch a jmp: always taken here
 *   stubs             moves out, then a jmp to where the exit went
 *
 * In the copy every label the loop defines is a new one and every temp
 * the loop writes has a new name, so the two loops share no definitions:
 * a counter written in both would have three, and an induction variable
 * with three is one strength reduction and the unroller do not see. What
 * the copy reads on the way in (live at h) is moved into its new names
 * first -- or, when one constant reaches the loop, that constant is
 * written there -- and what is still read past an exit (live at the
 * block the exit goes to) is moved back on that exit, in a stub. A frame
 * slot is never renamed: it is a place, not a value. */
#include "opt_int.h"

#define UNSW_MAX_BODY     48   /* instructions in a loop worth copying: at
                                * most 64, one bit each in unsw_live */
#define UNSW_MAX_PER_FUNC 4    /* loops copied in one function */
#define UNSW_GROWTH       160  /* instructions added to one function, in all */
#define UNSW_MAX_BLOCKS   4000 /* a function bigger than this is left alone:
                                * finding its loops is blocks x loops */

/* Can this instruction appear twice? The copies never both run, so only
 * whether it can be emitted twice: a label address, a jump table, an asm
 * (its labels), a frame that moves at run time, a landing pad and an
 * exception region's call (their tables) cannot. */
static int unsw_copyable(const struct ir_ins *i)
{
    switch (i->op) {
    case IR_ALLOCA: case IR_SPSAVE: case IR_SPRESTORE:
    case IR_ASM: case IR_VA_START: case IR_LANDING:
    case IR_IGOTO: case IR_LABELADDR: case IR_SWITCH:
        return 0;
    case IR_CALL:
        return !i->eh_region;
    default:
        return 1;
    }
}

static int unsw_falls(enum ir_op op)
{
    return op != IR_JMP && op != IR_RET && op != IR_UD2 &&
           op != IR_IGOTO && op != IR_SWITCH;
}

static struct ir_ins *unsw_emit(struct ibuf *nb, enum ir_op op,
                                int line, int col)
{
    struct ir_ins *p = ib_push(nb);
    p->op = op;
    p->line = line; p->col = col; p->synth = 1;
    return p;
}

/* each_read: the copy's names */
struct unsw_ren { const int *map; int nv; };
static void unsw_ren_cb(int *p, void *ctx)
{
    const struct unsw_ren *c = ctx;
    if (*p >= 0 && *p < c->nv && c->map[*p] >= 0)
        *p = c->map[*p];
}

/* each_label: a jump from outside to the header goes to the test */
struct unsw_rt { int from, to; };
static void unsw_rt_cb(int *p, void *ctx)
{
    const struct unsw_rt *c = ctx;
    if (*p == c->from)
        *p = c->to;
}

/* each_read for liveness: an upward-exposed read of a carried temp */
struct unsw_use { const int *cidx; int nv; uint64_t def, use; };
static void unsw_use_cb(int *p, void *ctx)
{
    struct unsw_use *c = ctx;
    if (*p < 0 || *p >= c->nv || c->cidx[*p] < 0)
        return;
    uint64_t bit = (uint64_t)1 << c->cidx[*p];
    if (!(c->def & bit))
        c->use |= bit;
}

/* Which of the loop's temps are live into each block. At most
 * UNSW_MAX_BODY of them, so one word per block. */
static uint64_t *unsw_live(struct ir_func *fn, struct bb *bb, int nbb,
                           const int *cidx)
{
    uint64_t *use = xcalloc((size_t)nbb, sizeof *use);
    uint64_t *def = xcalloc((size_t)nbb, sizeof *def);
    uint64_t *in = xcalloc((size_t)nbb, sizeof *in);
    for (int b = 0; b < nbb; b++) {
        struct unsw_use u = { cidx, fn->nvregs, 0, 0 };
        for (int m = bb[b].start; m < bb[b].end; m++) {
            each_read(&fn->ins[m], unsw_use_cb, &u);
            int t = def_target(&fn->ins[m]);
            if (t >= 0 && t < fn->nvregs && cidx[t] >= 0)
                u.def |= (uint64_t)1 << cidx[t];
        }
        use[b] = u.use;
        def[b] = u.def;
        in[b] = u.use;
    }
    for (int changed = 1; changed;) {
        changed = 0;
        for (int b = nbb - 1; b >= 0; b--) {
            uint64_t out = 0;
            for (int s = 0; s < bb[b].nsucc; s++)
                out |= in[bb[b].succ[s]];
            uint64_t ni = use[b] | (out & ~def[b]);
            if (ni != in[b]) {
                in[b] = ni;
                changed = 1;
            }
        }
    }
    free(use); free(def);
    return in;
}

/* The one definition of t that reaches the loop from outside it, when
 * every way in brings the same one and it is a constant (or a copy of a
 * temp whose one definition is), or -1. The copy's moved-in value is
 * then that constant itself rather than a copy of a name the first loop
 * also writes: an induction variable that starts at `mov t` is one whose
 * start strength reduction and the unroller's trip count cannot see. */
static int unsw_reaching(struct ir_func *fn, struct bb *bb, int nbb,
                         const char *in, int h, const struct defs *d, int t)
{
    char *seen = xcalloc((size_t)nbb, 1);
    int *stk = xmalloc((size_t)nbb * sizeof *stk);
    int ns = 0, found = -1, bad = 0;
    for (int q = 0; q < bb[h].npred; q++) {
        int p = bb[h].pred[q];
        if (!in[p] && !seen[p]) {
            seen[p] = 1;
            stk[ns++] = p;
        }
    }
    if (!ns)
        bad = 1;
    while (ns && !bad) {
        int b = stk[--ns], hit = -1;
        for (int m = bb[b].end - 1; m >= bb[b].start; m--)
            if (def_target(&fn->ins[m]) == t) {
                hit = m;
                break;
            }
        if (hit >= 0) {
            if (found >= 0 && found != hit)
                bad = 1;
            found = hit;
            continue;
        }
        if (bb[b].npred == 0 || in[b])
            bad = 1;      /* the entry with no definition, or the loop's own */
        for (int q = 0; q < bb[b].npred && !bad; q++) {
            int p = bb[b].pred[q];
            if (!seen[p]) {
                seen[p] = 1;
                stk[ns++] = p;
            }
        }
    }
    free(seen); free(stk);
    if (bad || found < 0)
        return -1;
    const struct ir_ins *f = &fn->ins[found];
    if (f->op == IR_MOV && f->a >= 0 && f->a < fn->nvregs &&
        d->cnt[f->a] == 1 && d->ins[f->a] >= 0)
        found = d->ins[f->a];
    return fn->ins[found].op == IR_CONST && !fn->ins[found].imm_b
         ? found : -1;
}

/* One loop and the test to take out of it. */
struct unsw_loop {
    int h;            /* the header's block */
    char *in;         /* in[b]: block b is the loop's */
    int k;            /* the branch */
    int size;         /* instructions, labels not counted */
};

/* The outermost loop with a test to take out, or 0.
 * Outermost first: of two loops a test is invariant in, the bigger holds
 * the smaller, and the test is made once per entry to the loop it leaves. */
static int unsw_find(struct ir_func *fn, struct bb *bb, int nbb,
                     int budget, struct unsw_loop *out)
{
    struct defs d;
    compute_defs(fn, &d);
    char *addr = xcalloc((size_t)(fn->nvars ? fn->nvars : 1), 1);
    char *taken = xcalloc((size_t)(fn->nlabels ? fn->nlabels : 1), 1);
    for (int m = 0; m < fn->nins; m++) {
        const struct ir_ins *i = &fn->ins[m];
        if (i->op == IR_ADDR && i->a >= 0 && i->a < fn->nvars)
            addr[i->a] = 1;
        if (i->op == IR_LABELADDR && i->label >= 0 && i->label < fn->nlabels)
            taken[i->label] = 1;
    }
    int void_fn = 1;
    for (int m = 0; m < fn->nins && void_fn; m++)
        if (fn->ins[m].op == IR_RET && fn->ins[m].a >= 0)
            void_fn = 0;
    char *wr = xmalloc((size_t)(fn->nvregs ? fn->nvregs : 1));
    char *in = xmalloc((size_t)nbb);
    /* every header and its loop's size, once */
    int *hs = xmalloc((size_t)nbb * sizeof *hs), *hsize = xmalloc((size_t)nbb * sizeof *hsize);
    int nh = 0, found = 0;
    for (int h = 0; h < nbb; h++) {
        if (bb[h].end <= bb[h].start || fn->ins[bb[h].start].op != IR_LABEL)
            continue;
        int nback = 0;
        for (int q = 0; q < bb[h].npred; q++)
            if (bb_dominates(bb, h, bb[h].pred[q]))
                nback++;
        if (!nback)
            continue;
        memset(in, 0, (size_t)nbb);
        for (int q = 0; q < bb[h].npred; q++)
            if (bb_dominates(bb, h, bb[h].pred[q]))
                loop_body(bb, nbb, h, bb[h].pred[q], in);
        int size = 0;
        for (int b = 0; b < nbb; b++)
            if (in[b])
                for (int m = bb[b].start; m < bb[b].end; m++)
                    size += fn->ins[m].op != IR_LABEL;
        if (size > UNSW_MAX_BODY || size > budget)
            continue;
        hs[nh] = h;
        hsize[nh++] = size;
    }
    for (;;) {
        /* the biggest loop not yet tried */
        int best = -1, bsize = 0, bj = -1;
        for (int j = 0; j < nh; j++)
            if (hs[j] >= 0 && (best < 0 || hsize[j] > bsize)) {
                best = hs[j];
                bsize = hsize[j];
                bj = j;
            }
        if (best < 0)
            break;
        hs[bj] = -1;
        int h = best;
        memset(in, 0, (size_t)nbb);
        for (int q = 0; q < bb[h].npred; q++)
            if (bb_dominates(bb, h, bb[h].pred[q]))
                loop_body(bb, nbb, h, bb[h].pred[q], in);
        int Lh = fn->ins[bb[h].start].label, ok = !taken[Lh];
        memset(wr, 0, (size_t)(fn->nvregs ? fn->nvregs : 1));
        for (int b = 0; b < nbb && ok; b++) {
            if (!in[b])
                continue;
            for (int m = bb[b].start; m < bb[b].end && ok; m++) {
                const struct ir_ins *i = &fn->ins[m];
                if (!unsw_copyable(i))
                    ok = 0;
                if (i->op == IR_LABEL && taken[i->label])
                    ok = 0;
                int t = def_target(i);
                if (t >= 0 && t < fn->nvregs)
                    wr[t] = 1;
            }
            /* falling off the function's end, which is a return only
             * in a function that returns nothing: there the copy says so
             * (unsw_one), and in one that returns a value the path is
             * the program's undefined one, left as it is */
            if (b + 1 == nbb && unsw_falls(fn->ins[bb[b].end - 1].op) &&
                !void_fn)
                ok = 0;
        }
        for (int b = 0; b < nbb && ok && !found; b++) {
            if (!in[b])
                continue;
            int m = bb[b].end - 1;
            const struct ir_ins *br = &fn->ins[m];
            if ((br->op != IR_BRZ && br->op != IR_BRNZ) || br->label == Lh)
                continue;
            int c = br->a;
            long kval;
            if (c < 0 || c >= fn->nvregs || wr[c] || br->flt || br->vol)
                continue;
            if (c < fn->nvars &&
                (addr[c] || (fn->locals && fn->locals[c].is_volatile)))
                continue;
            if (get_const(fn, &d, c, &kval))
                continue;
            out->h = h;
            out->k = m;
            out->size = bsize;
            out->in = in;
            in = NULL;
            found = 1;
        }
        if (found)
            break;
    }
    free(in); free(hs); free(hsize); free(wr); free(addr); free(taken);
    free_defs(&d);
    return found;
}

/* A stub: the copy's exit to `target`, moving `mask`'s temps back */
struct unsw_stub { int label, target; uint64_t mask; };

struct unsw_ctx {
    struct ir_func *fn;
    struct ibuf nb;
    int nc;
    const int *carr, *vmap, *cw, *kdef;
    const char *cf;
    struct unsw_stub *st;
    int nst, capst;
};

/* The copy's moves: into its names (back == 0) or out of them */
static void unsw_moves(struct unsw_ctx *x, uint64_t mask, int back,
                       int line, int col)
{
    for (int j = 0; j < x->nc; j++) {
        if (!(mask & ((uint64_t)1 << j)))
            continue;
        int t = x->carr[j];
        if (!back && x->kdef[j] >= 0) {
            struct ir_ins *p = ib_push(&x->nb);
            *p = x->fn->ins[x->kdef[j]];
            p->dst = x->vmap[t];
            p->synth = 1;
            continue;
        }
        struct ir_ins *p = unsw_emit(&x->nb, IR_MOV, line, col);
        p->dst = back ? t : x->vmap[t];
        p->a = back ? x->vmap[t] : t;
        p->b = -1;
        p->w = x->cw[t];
        p->flt = x->cf[t];
    }
}

/* Where an exit of the copy to label T goes: T, or the stub moving back
 * what is live there */
static int unsw_exit(struct unsw_ctx *x, int T, uint64_t mask)
{
    if (!mask)
        return T;
    for (int s = 0; s < x->nst; s++)
        if (x->st[s].target == T)
            return x->st[s].label;
    if (x->nst == x->capst) {
        x->capst = x->capst ? x->capst * 2 : 4;
        x->st = xrealloc(x->st, (size_t)x->capst * sizeof *x->st);
    }
    struct unsw_stub *s = &x->st[x->nst++];
    s->label = x->fn->nlabels++;
    s->target = T;
    s->mask = mask;
    return s->label;
}

static void unsw_one(struct ir_func *fn, struct bb *bb, int nbb,
                     const int *l2b, const struct unsw_loop *P)
{
    const char *in = P->in;
    int h = P->h, k = P->k;
    int nvr = fn->nvregs;
    int Lh = fn->ins[bb[h].start].label;
    struct ir_ins brcopy = fn->ins[k];
    int lineh = fn->ins[bb[h].start].line, colh = fn->ins[bb[h].start].col;
    int lastb = -1;
    for (int b = 0; b < nbb; b++)
        if (in[b])
            lastb = b;
    char *inins = xcalloc((size_t)fn->nins, 1);
    for (int b = 0; b < nbb; b++)
        if (in[b])
            memset(inins + bb[b].start, 1, (size_t)(bb[b].end - bb[b].start));

    /* every temp the loop writes: a new name in the copy */
    int *vmap = xmalloc((size_t)(nvr ? nvr : 1) * sizeof *vmap);
    int *cidx = xmalloc((size_t)(nvr ? nvr : 1) * sizeof *cidx);
    int *cw = xcalloc((size_t)(nvr ? nvr : 1), sizeof *cw);
    char *cf = xcalloc((size_t)(nvr ? nvr : 1), 1);
    for (int v = 0; v < nvr; v++)
        vmap[v] = cidx[v] = -1;
    int *carr = xmalloc((size_t)(P->size ? P->size : 1) * sizeof *carr);
    int nc = 0;
    for (int m = fn->nins - 1; m >= 0; m--) {    /* the first write's width */
        if (!inins[m])
            continue;
        int t = def_target(&fn->ins[m]);
        if (t >= fn->nvars && t < nvr) {
            cw[t] = ir_result_w(&fn->ins[m]);
            cf[t] = (char)ir_result_flt(&fn->ins[m]);
            if (vmap[t] < 0) {
                vmap[t] = fn->nvregs++;
                cidx[t] = nc;
                carr[nc++] = t;
            }
        }
    }
    uint64_t *live = unsw_live(fn, bb, nbb, cidx);
    struct defs d;
    compute_defs(fn, &d);
    int *kdef = xmalloc((size_t)(nc ? nc : 1) * sizeof *kdef);
    for (int j = 0; j < nc; j++)
        kdef[j] = live[h] & ((uint64_t)1 << j)
                ? unsw_reaching(fn, bb, nbb, in, h, &d, carr[j]) : -1;
    free_defs(&d);

    /* the copy's labels; a label for every block an exit falls into */
    int *lmap = xmalloc((size_t)(fn->nlabels ? fn->nlabels : 1) * sizeof *lmap);
    for (int l = 0; l < fn->nlabels; l++)
        lmap[l] = -1;
    int *blab = xmalloc((size_t)nbb * sizeof *blab);  /* block b's label */
    char *newlab = xcalloc((size_t)nbb, 1);
    for (int b = 0; b < nbb; b++)
        blab[b] = fn->ins[bb[b].start].op == IR_LABEL
                ? fn->ins[bb[b].start].label : -1;
    for (int b = 0; b < nbb; b++) {
        if (!in[b])
            continue;
        if (blab[b] >= 0)
            lmap[blab[b]] = fn->nlabels++;
        if (unsw_falls(fn->ins[bb[b].end - 1].op) && b + 1 < nbb &&
            !in[b + 1] && blab[b + 1] < 0) {
            blab[b + 1] = fn->nlabels++;
            newlab[b + 1] = 1;
        }
    }
    int LD = fn->nlabels++;                  /* the test's block */
    int CP = fn->nlabels++;                  /* the copy's way in */
    uint64_t in_mask = live[h];

    struct unsw_ctx x = { fn, { 0, 0, 0 }, nc, carr, vmap, cw, kdef, cf,
                          NULL, 0, 0 };
    int *newpos = fn->var_scope_lo
        ? xmalloc((size_t)(fn->nins + 1) * sizeof *newpos) : NULL;
    int cend = -1, body_lo = bb[h].start, body_hi = bb[lastb].end;
    for (int b = 0; b < nbb; b++)
        if (in[b] && bb[b].start < body_lo)
            body_lo = bb[b].start;
    for (int b = 0; b < nbb; b++) {
        if (newlab[b]) {
            struct ir_ins *p = unsw_emit(&x.nb, IR_LABEL, lineh, colh);
            p->label = blab[b];
        }
        if (b == h) {
            /* a loop block falling into h goes round without the test */
            if (b > 0 && in[b - 1] && unsw_falls(fn->ins[bb[b - 1].end - 1].op)) {
                struct ir_ins *p = unsw_emit(&x.nb, IR_JMP, lineh, colh);
                p->label = Lh;
            }
            struct ir_ins *p = unsw_emit(&x.nb, IR_LABEL, lineh, colh);
            p->label = LD;
            p = ib_push(&x.nb);
            *p = brcopy;
            p->label = CP;
            p->synth = 1;
        }
        for (int m = bb[b].start; m < bb[b].end; m++) {
            if (newpos) newpos[m] = x.nb.n;
            if (m == k)
                continue;             /* never taken in this copy */
            struct ir_ins *q = ib_push(&x.nb);
            *q = fn->ins[m];
            if (!in[b]) {             /* from outside, into the test */
                struct unsw_rt rt = { Lh, LD };
                each_label(fn, q, unsw_rt_cb, &rt);
            }
        }
        if (b != lastb)
            continue;

        /* ---- the copy, after the loop's last block ---- */
        if (unsw_falls(fn->ins[bb[b].end - 1].op)) {
            /* what followed it, or the function's end: a return */
            struct ir_ins *p = unsw_emit(&x.nb, b + 1 < nbb ? IR_JMP : IR_RET,
                                         lineh, colh);
            if (b + 1 < nbb)
                p->label = blab[b + 1];
            else
                p->a = p->b = -1;
        }
        struct ir_ins *p = unsw_emit(&x.nb, IR_LABEL, lineh, colh);
        p->label = CP;
        unsw_moves(&x, in_mask, 0, lineh, colh);
        /* into the copy's header, falling when it is the loop's first
         * block: strength reduction and the vectorizer put what they
         * set up for a loop in front of its header */
        int firstb = 0;
        while (!in[firstb])
            firstb++;
        if (firstb != h) {
            p = unsw_emit(&x.nb, IR_JMP, lineh, colh);
            p->label = lmap[Lh];
        }
        for (int c = 0; c < nbb; c++) {
            if (!in[c])
                continue;
            for (int m = bb[c].start; m < bb[c].end; m++) {
                struct ir_ins q = fn->ins[m];
                ins_own_args(&q);
                if (q.op == IR_LABEL) {
                    q.label = lmap[q.label];
                    *ib_push(&x.nb) = q;
                    continue;
                }
                struct unsw_ren rc = { vmap, nvr };
                each_read(&q, unsw_ren_cb, &rc);
                int t = def_target(&q);
                if (t >= fn->nvars && t < nvr && vmap[t] >= 0)
                    q.dst = vmap[t];
                if (m == k) {         /* always taken in this one */
                    q.op = IR_JMP;
                    q.a = -1;
                    q.prob = 0;
                }
                if (q.op == IR_JMP || q.op == IR_BRZ || q.op == IR_BRNZ) {
                    int T = q.label;
                    if (lmap[T] >= 0)
                        q.label = lmap[T];
                    else if (l2b[T] >= 0)
                        q.label = unsw_exit(&x, T, live[l2b[T]]);
                }
                *ib_push(&x.nb) = q;
            }
            /* falling out of the block: into the loop's next one, which
             * the copy also has next, or out of the loop, now a jump */
            if (unsw_falls(fn->ins[bb[c].end - 1].op) && c + 1 < nbb &&
                !in[c + 1]) {
                struct ir_ins *j = unsw_emit(&x.nb, IR_JMP, lineh, colh);
                j->label = unsw_exit(&x, blab[c + 1], live[c + 1]);
            } else if (unsw_falls(fn->ins[bb[c].end - 1].op) && c + 1 == nbb) {
                struct ir_ins *r = unsw_emit(&x.nb, IR_RET, lineh, colh);
                r->a = r->b = -1;
            }
        }
        for (int s = 0; s < x.nst; s++) {
            p = unsw_emit(&x.nb, IR_LABEL, lineh, colh);
            p->label = x.st[s].label;
            unsw_moves(&x, x.st[s].mask, 1, lineh, colh);
            p = unsw_emit(&x.nb, IR_JMP, lineh, colh);
            p->label = x.st[s].target;
        }
        cend = x.nb.n;
    }
    if (newpos) {
        newpos[fn->nins] = x.nb.n;
        for (int v = 0; v < fn->nvars; v++) {
            int slo = fn->var_scope_lo[v], shi = fn->var_scope_hi[v];
            if (slo >= 0 && slo <= fn->nins)
                fn->var_scope_lo[v] = newpos[slo];
            /* a scope that ends in the loop now ends after its copy */
            if (shi > body_lo && shi < body_hi && cend >= 0)
                fn->var_scope_hi[v] = cend;
            else if (shi >= 0 && shi <= fn->nins)
                fn->var_scope_hi[v] = newpos[shi];
        }
        free(newpos);
    }
    free(fn->ins);
    fn->ins = x.nb.p; fn->nins = x.nb.n; fn->cap = x.nb.cap;
    free(x.st); free(live); free(kdef); free(inins);
    free(vmap); free(cidx); free(cw); free(cf); free(carr);
    free(lmap); free(blab); free(newlab);
}

int pass_unswitch(struct ir_func *fn)
{
    if (fn->nins == 0 || fn->has_i128)
        return 0;
    int done = 0, budget = UNSW_GROWTH;
    while (done < UNSW_MAX_PER_FUNC && budget > 0) {
        int nbb, *l2b;
        struct bb *bb = build_cfg(fn, &nbb, &l2b);
        int *order = xmalloc((size_t)nbb * sizeof *order), norder;
        compute_rpo(bb, nbb, order, &norder);
        struct unsw_loop P;
        int go = 0;
        if (norder == nbb && nbb <= UNSW_MAX_BLOCKS) {
            compute_idom(bb, order, norder);
            go = unsw_find(fn, bb, nbb, budget, &P);
        }
        if (go) {
            int line = fn->ins[P.k].line;
            unsw_one(fn, bb, nbb, l2b, &P);
            free(P.in);
            budget -= P.size;
            done++;
            if (remarks_on() && fn->src)
                remark_add("unswitch", "unswitched", fn->name, "loop",
                           fn->file, line ? line : fn->line,
                           "a test the loop does not change, made once "
                           "before it: the %d-instruction loop is copied, "
                           "one copy for each answer", P.size);
        }
        free(order); free(l2b);
        free_cfg(bb, nbb);
        if (!go)
            break;
        /* each copy's other side, which nothing reaches now: the next
         * look at the function wants every block reachable */
        drop_unreachable(fn);
    }
    return done > 0;
}
