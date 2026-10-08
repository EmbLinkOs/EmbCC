/* ---- copy propagation ---- */

#include "opt_int.h"

struct repl { int from, to, n; };

static void repl_cb(int *p, void *ctx)
{
    struct repl *r = ctx;
    if (*p == r->from) {
        *p = r->to;
        r->n++;
    }
}

/* The rule, one move at a time: walking the function in order, each
 * eligible `dst = mov a` rewrites every read of dst, everywhere, to
 * whatever its `a` holds at that moment. That is a walk over the whole
 * function per move, and a function of 8000 statements spent most of
 * its -O2 compile there (each_read and repl_cb on top of the profile,
 * 30 s for the thumb compile).
 *
 * pass_copyprop computes the same rewrite in one walk. It is used when
 * the eligible moves make no cycle (`x = mov y` and `y = mov x`, each
 * the only definition: values nothing ever computes), which is every
 * function in practice; a cycle falls back to this, whose answer then
 * depends on the order in a way not worth reproducing. */
static int copyprop_by_move(struct ir_func *fn, struct defs *d)
{
    int changed = 0;
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        if (i->op != IR_MOV || i->a < 0 || i->dst < 0 || i->a == i->dst)
            continue;
        /* Both ends must be single-assignment: the source so its value is
         * invariant, and the DEST so every use of it comes from THIS move.
         * A dest written in two branches (irgen's phi-style merge, e.g. the
         * va_arg result address) has cnt > 1 — propagating it would wrongly
         * force one branch's value onto the other's uses. */
        if (d->cnt[i->a] != 1 || d->cnt[i->dst] != 1)
            continue;
        struct repl r = { i->dst, i->a, 0 };
        for (int m = 0; m < fn->nins; m++)
            each_read(&fn->ins[m], repl_cb, &r);
        if (r.n) {
            changed = 1;   /* the MOV is now dead; DCE removes it */
            g_did.copy += r.n;
        }
    }
    return changed;
}

/* What the one-at-a-time rule comes to. Each eligible move is an edge
 * dst -> a, and a dst has at most one (it has one definition), so the
 * edges form chains, and every read of a vreg on a chain ends up naming
 * the chain's last vreg, its root -- in whatever order the moves are
 * met, because a move met later than the one that copied FROM it
 * rewrites that copy's readers again. Which is also why the order shows
 * in one place: how many rewrites a read took, which the remark counts.
 * A read of u is rewritten when u's move is met, and lands on the
 * vreg the move's own operand holds by then -- so it takes one rewrite
 * for every step down the chain where the next move comes LATER in the
 * function than the one before it, with the root counting as last of
 * all. nrw[] is that number. */
struct cpmap { const int *root, *nrw; int nv; long n; };
static void cpmap_cb(int *p, void *ctx)
{
    struct cpmap *c = ctx;
    int v = *p;
    if (v >= 0 && v < c->nv && c->root[v] >= 0) {
        *p = c->root[v];
        c->n += c->nrw[v];
    }
}

int pass_copyprop(struct ir_func *fn)
{
    struct defs d;
    compute_defs(fn, &d);
    int nv = fn->nvregs;
    /* src[v]/pos[v]: v's eligible move, by the same test as
     * copyprop_by_move's, and where it is; -1 for none. */
    int *src = xmalloc((size_t)(nv ? nv : 1) * sizeof *src);
    int *pos = xmalloc((size_t)(nv ? nv : 1) * sizeof *pos);
    int nmov = 0;
    for (int v = 0; v < nv; v++)
        src[v] = -1;
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        if (i->op != IR_MOV || i->a < 0 || i->dst < 0 || i->a == i->dst)
            continue;
        if (d.cnt[i->a] != 1 || d.cnt[i->dst] != 1)
            continue;
        src[i->dst] = i->a;
        pos[i->dst] = n;
        nmov++;
    }
    if (nmov == 0) {
        free(src); free(pos); free_defs(&d);
        return 0;
    }
    /* root[v]: where a read of v ends up (-1: v has no move). state: 0
     * not yet seen, 1 on the chain being walked, 2 resolved. */
    int *root = xmalloc((size_t)nv * sizeof *root);
    int *nrw = xmalloc((size_t)nv * sizeof *nrw);
    int *stk = xmalloc((size_t)nv * sizeof *stk);
    char *state = xcalloc((size_t)nv, 1);
    int cycle = 0;
    for (int v = 0; v < nv && !cycle; v++) {
        if (src[v] < 0 || state[v] == 2)
            continue;
        int sp = 0, u = v;
        while (src[u] >= 0 && state[u] == 0) {
            state[u] = 1;
            stk[sp++] = u;
            u = src[u];
        }
        if (src[u] >= 0 && state[u] == 1) {
            cycle = 1;
            break;
        }
        /* u is a root, or a vreg already resolved */
        int r = src[u] < 0 ? u : root[u];
        while (sp > 0) {
            int w = stk[--sp];
            int s = src[w];
            int later = src[s] < 0 || pos[s] > pos[w];
            root[w] = r;
            nrw[w] = (src[s] < 0 ? 0 : nrw[s]) + later;
            state[w] = 2;
        }
    }
    int changed;
    if (cycle) {
        changed = copyprop_by_move(fn, &d);
    } else {
        for (int v = 0; v < nv; v++)
            if (src[v] < 0)
                root[v] = -1;
        struct cpmap c = { root, nrw, nv, 0 };
        for (int n = 0; n < fn->nins; n++)
            each_read(&fn->ins[n], cpmap_cb, &c);
        changed = c.n > 0;   /* the MOVs are now dead; DCE removes them */
        g_did.copy += c.n;
    }
    free(src); free(pos); free(root); free(nrw); free(stk); free(state);
    free_defs(&d);
    return changed;
}

/* ---- block-local copy propagation ----
 *
 * pass_copyprop is global, so it needs its source to be
 * single-assignment. The values that matter most in a loop are exactly
 * the ones that are not: mem2reg destructs a phi into copies, so an
 * induction variable and an accumulator are each assigned on every
 * incoming edge, and every read of one goes through a fresh `mov` that
 * the global pass may not touch. A loop counter therefore gets copied
 * three times per iteration -- once for the test, once for the body,
 * once for the increment -- and all three copies survive to codegen.
 *
 * Inside ONE block the question is easy. `%9 = mov %27` makes %9 and
 * %27 the same value until something redefines %27, and a block is
 * straight-line code, so "until" is just a position. Walking the block
 * with a table of current copies, cleared on every def, rewrites those
 * reads with no dominance reasoning at all. The table is dropped at each
 * block boundary, so a value carried around a back edge is never assumed
 * to survive one.
 *
 * The table is indexed by vreg and a frame slot is a vreg (see
 * each_read), which is safe here for the same reason it is safe there:
 * an entry is only ever made for a MOV's dst, and a MOV's dst is always
 * a temp, so a slot's entry stays empty and IR_LDVAR's slot operand is
 * never rewritten. */
void lcopy_cb(int *p, void *ctx)
{
    struct lcopy *c = ctx;
    if (*p >= 0 && *p < c->nv && c->cp[*p] >= 0) { *p = c->cp[*p]; c->n++; }
}

/* The table is cleared at every block and every definition clears the
 * entries copied FROM what it defines, and both used to be a sweep over
 * every vreg: blocks x vregs plus definitions x vregs, which on one
 * function of 4000 plain statements was most of an -O2 compile once
 * pass_copyprop stopped being. So each entry is also filed under its
 * source (`by`), and a block's entries are remembered in the order they
 * were made, to be undone one by one. A filing goes stale when its entry
 * is cleared or replaced; it is checked against cp[] when used, not
 * removed. */
struct lcfile { int v, src, next; };
struct lctab { int *cp, *by; struct lcfile *f; int nf, capf; };

static void lctab_reset(struct lctab *t)
{
    for (int k = 0; k < t->nf; k++) {
        t->cp[t->f[k].v] = -1;
        t->by[t->f[k].src] = -1;
    }
    t->nf = 0;
}

static void lctab_set(struct lctab *t, int v, int src)
{
    if (t->nf == t->capf) {
        t->capf = t->capf ? t->capf * 2 : 64;
        t->f = xrealloc(t->f, (size_t)t->capf * sizeof *t->f);
    }
    t->f[t->nf].v = v;
    t->f[t->nf].src = src;
    t->f[t->nf].next = t->by[src];
    t->by[src] = t->nf++;
    t->cp[v] = src;
}

int pass_copyprop_local(struct ir_func *fn)
{
    if (fn->nins == 0 || fn->nvregs == 0)
        return 0;
    int nbb, *l2b;
    struct bb *bb = build_cfg(fn, &nbb, &l2b);
    struct defs d;
    compute_defs(fn, &d);
    struct lctab tb = { NULL, NULL, NULL, 0, 0 };
    tb.cp = xmalloc((size_t)fn->nvregs * sizeof *tb.cp);
    tb.by = xmalloc((size_t)fn->nvregs * sizeof *tb.by);
    for (int v = 0; v < fn->nvregs; v++)
        tb.cp[v] = tb.by[v] = -1;
    int *cp = tb.cp;
    int changed = 0;
    for (int b = 0; b < nbb; b++) {
        lctab_reset(&tb);
        for (int n = bb[b].start; n < bb[b].end; n++) {
            struct ir_ins *i = &fn->ins[n];
            /* Inline asm names its OUTPUT temps through each_read, and a
             * landing pad writes two temps that def_target cannot both
             * report. Neither is worth modelling for a copy table: drop
             * it and carry on from here. */
            if (i->op == IR_ASM || i->op == IR_LANDING) {
                lctab_reset(&tb);
                continue;
            }
            struct lcopy c = { cp, fn->nvregs, 0 };
            each_read(i, lcopy_cb, &c);
            if (c.n) {
                changed = 1;
                g_did.copy += c.n;
            }
            int t = def_target(i);
            if (t >= 0 && t < fn->nvregs) {
                cp[t] = -1;                  /* it is a new value now */
                for (int k = tb.by[t]; k >= 0; k = tb.f[k].next)
                    if (cp[tb.f[k].v] == t)  /* and so is anything copied
                                              * from what it just replaced */
                        cp[tb.f[k].v] = -1;
                tb.by[t] = -1;
            }
            /* Only a single-assignment dst becomes a copy. The reverse --
             * rewriting reads of a PHI temp to the value moved into it --
             * is legal and costs more than it saves: it keeps the source
             * live past the copy that ends it, so the two interfere and
             * the allocator can no longer give them one register. That
             * turns `i = i + 1` into a move, an add and a move back. A
             * phi temp is still a fine copy SOURCE, which is the case
             * worth having. */
            if (i->op == IR_MOV && !i->vol && i->dst >= 0 && i->a >= 0 &&
                i->dst != i->a && i->dst < fn->nvregs && i->a < fn->nvregs &&
                d.cnt[i->dst] == 1)
                lctab_set(&tb, i->dst, cp[i->a] >= 0 ? cp[i->a] : i->a);
        }
    }
    free_defs(&d);
    free(tb.cp); free(tb.by); free(tb.f); free(l2b);
    free_cfg(bb, nbb);
    return changed;
}
