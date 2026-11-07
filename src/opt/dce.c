/* ---- dead-code elimination ---- */

#include "opt_int.h"

void count_cb(int *p, void *ctx)
{
    struct ucount *u = ctx;
    if (*p >= 0 && *p < u->n)
        u->use[*p]++;
}

/* Mark a temp live, and queue the instructions that define it for the
 * same treatment. */
struct mark { char *live; int n, nins; const int *dhead, *dnext;
              char *live_ins; int *work, nwork; };
static void mark_cb(int *p, void *ctx)
{
    struct mark *m = ctx;
    int v = *p;
    if (v < 0 || v >= m->n || m->live[v])
        return;
    m->live[v] = 1;
    for (int k = m->dhead[v]; k >= 0; k = m->dnext[k]) {
        int x = k < m->nins ? k : k - m->nins;
        if (!m->live_ins[x]) {
            m->live_ins[x] = 1;
            m->work[m->nwork++] = x;
        }
    }
}

/* ---- dead code, by marking what is live rather than counting uses ----
 *
 * Counting uses cannot remove a CYCLE. `i = i + 1` feeding `i = mov
 * next` feeding the add again is two instructions that each have a use
 * -- each other -- so a use count never reaches zero for either, and a
 * loop counter nothing reads survives for the life of the function.
 * That is the ordinary shape left behind whenever a loop's test stops
 * naming its own induction variable, which is exactly what the test
 * replacement in `ivsr_one` does.
 *
 * So liveness is computed the other way round: an instruction is live
 * when its EFFECT is not its result -- a store, a branch, a label, a
 * return, a call that can be observed -- or when something live reads
 * what it defines. Everything else is dead, cycles included, because a
 * cycle no live instruction reaches is never marked.
 *
 * IR_STVAR's "result" is a frame slot, and a slot is read by an LDVAR
 * or an ADDR, both of which each_read reports -- so the same rule
 * covers dead stores to a local with no remaining readers. A volatile
 * one is live regardless: the access itself is the effect.
 *
 * The marking is a worklist: a temp found live queues the instructions
 * that define it, found through a list per temp. It used to sweep the
 * function until a sweep changed nothing, and a sweep carries liveness
 * one step backwards through code that runs forwards -- a chain of 4000
 * joins, each copy read by the next, took 4000 sweeps. Both compute the
 * least set closed under the two rules, so they mark the same set. */
/* A load is not pure -- it may fault -- except one of a string
 * constant's own bytes: its address is the constant's (one straddr) and
 * it reads inside it, which is .rodata the program cannot unmap. Those
 * are what a folded long double leaves behind (ld_fold), and a string's
 * byte read and dropped. */
static int rodata_load(const struct ir_func *fn, const int *dhead,
                       const int *dnext, const struct ir_ins *i)
{
    if (i->op != IR_LOAD || i->vol || i->flash || def_target(i) < 0 ||
        !g_fold_unit || i->a < 0 || i->a >= fn->nvregs)
        return 0;
    int n = dhead[i->a];
    if (n < 0 || dnext[n] >= 0 || fn->ins[n].op != IR_STRADDR)
        return 0;
    int lab = fn->ins[n].label;
    return lab >= 0 && lab < g_fold_unit->nstrs && i->memoff >= 0 &&
           i->size > 0 && i->memoff + i->size <= g_fold_unit->strs[lab].len;
}

int pass_dce(struct ir_func *fn)
{
    int nins = fn->nins, nvr = fn->nvregs;
    if (nins == 0)
        return 0;
    char *live_ins = xcalloc((size_t)nins, 1);
    char *live_t = xcalloc((size_t)(nvr ? nvr : 1), 1);
    /* the instructions defining each temp: def_target, and a landing
     * pad's second temp in `b` */
    int *dhead = xmalloc((size_t)(nvr ? nvr : 1) * sizeof *dhead);
    int *dnext = xmalloc((size_t)nins * 2 * sizeof *dnext);
    int *work = xmalloc((size_t)nins * sizeof *work);
    for (int v = 0; v < nvr; v++)
        dhead[v] = -1;
    for (int n = nins - 1; n >= 0; n--) {
        struct ir_ins *i = &fn->ins[n];
        int t = def_target(i);
        dnext[n] = dnext[nins + n] = -1;
        if (t >= 0 && t < nvr) {
            dnext[n] = dhead[t];
            dhead[t] = n;
        }
        int second = i->op == IR_LANDING ? i->b : -1;
        if (second >= 0 && second < nvr && second != t) {
            /* filed as nins + n, which names instruction n again */
            dnext[nins + n] = dhead[second];
            dhead[second] = nins + n;
        }
    }
    struct mark m = { live_t, nvr, nins, dhead, dnext, live_ins, work, 0 };
    for (int n = 0; n < nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        if (i->op == IR_STVAR) {
            /* -Og: a store to a source variable is how a debugger sees
             * it (target_keep_vars), so it stays even when unread */
            if (i->vol || (target_keep_vars() && i->dst >= 0 &&
                           i->dst < fn->nvars))
                live_ins[n] = 1;
            continue;                 /* otherwise its slot decides */
        }
        /* A call that touches no memory and cannot throw does nothing
         * but produce a value, so it lives or dies with that value. */
        if (i->op == IR_CALL && !i->indirect && i->callee &&
            i->callee->inf_no_read && i->callee->inf_no_write &&
            i->callee->is_nothrow && def_target(i) >= 0)
            continue;
        /* a volatile local's read happens even when nothing uses it:
         * `(void)v;` is a read the program asked for */
        if (is_pure(i->op) && def_target(i) >= 0 && !i->vol)
            continue;
        if (rodata_load(fn, dhead, dnext, i))
            continue;
        live_ins[n] = 1;
    }
    for (int n = 0; n < nins; n++)
        if (live_ins[n])
            work[m.nwork++] = n;
    while (m.nwork > 0) {
        int n = work[--m.nwork];
        each_read(&fn->ins[n], mark_cb, &m);
    }
    /* Removing instructions renumbers the ones that follow. fn->var_scope_lo/hi
     * (irgen-stamped instruction indices, read by codegen's coalesce_locals to
     * decide which address-taken locals may share a stack slot) must move with
     * them — otherwise a stale scope index is compared against a FRESH liveness
     * index and two locals whose lifetimes actually overlap get the same slot,
     * so one's store clobbers the other. newpos[n] is the new index instruction
     * n lands at (a dropped instruction collapses onto the next survivor); it
     * maps the half-open [lo, hi) scope bounds, index nins included. */
    int *newpos = fn->var_scope_lo
        ? xmalloc((size_t)(fn->nins + 1) * sizeof *newpos) : NULL;
    int changed = 0, j = 0;
    for (int n = 0; n < fn->nins; n++) {
        if (newpos) newpos[n] = j;
        if (!live_ins[n]) {
            changed = 1;
            g_did.dce++;
            continue;   /* nothing live reaches what it computes */
        }
        if (j != n)
            fn->ins[j] = fn->ins[n];
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
    fn->nins = j;
    free(live_ins); free(live_t);
    free(dhead); free(dnext); free(work);
    return changed;
}

/* ---- a definition nothing reads, of a value that has others ----------
 *
 * pass_dce asks whether a VALUE is live, and keeps every definition of
 * one that is: a temp with two definitions, one read and one not, keeps
 * both. Unrolling leaves such a definition behind -- the k loop of a FIR
 * filter, fully unrolled, still sets k to 16 halfway through the copies,
 * and the next trip of the loop around it sets k to 0 before reading it
 * -- and every backend then computes a constant nobody reads, inside the
 * loop. Liveness after each definition says which are dead: a walk back
 * through each block from what is live out of it, as the allocator walks
 * (ra_lset). A dead definition's own reads are not counted, so a chain of
 * them in one block goes in one pass.
 *
 * Only temps (variables have slots and debug homes), only definitions
 * pass_dce would call pure, and only of a temp defined more than once: a
 * temp with one definition and no reader is pass_dce's already.
 * EMBCC_NO_DEADDEFS=1 turns it off, for bisecting a difference to it.
 * Over lib/libc and the bench at -O2 it finds some 450 such definitions
 * on Cortex-M and RV32, 384 on x86-64. */
int pass_deaddefs(struct ir_func *fn)
{
    int N = fn->nins, nv = fn->nvregs;
    if (N == 0 || nv <= fn->nvars || fn->neh || getenv("EMBCC_NO_DEADDEFS"))
        return 0;
    struct defs d;
    compute_defs(fn, &d);
    int any = 0;
    for (int v = fn->nvars; v < nv && !any; v++)
        any = d.cnt[v] > 1;
    if (!any) {
        free_defs(&d);
        return 0;
    }
    int *first = xmalloc((size_t)nv * sizeof *first);
    int *last = xmalloc((size_t)nv * sizeof *last);
    struct ra_live *lv = ra_live_compute(fn, first, last);
    struct ra_lset s;
    ra_lset_init(&s, nv);
    char *dead = xcalloc((size_t)N, 1);
    int ndead = 0;
    for (int b = ra_live_nblocks(lv) - 1; b >= 0; b--) {
        ra_lset_out(&s, lv, b);
        for (int i = ra_live_block_start(lv, b + 1) - 1;
             i >= ra_live_block_start(lv, b); i--) {
            const struct ir_ins *in = &fn->ins[i];
            int v = in->dst;
            if (v >= fn->nvars && v < nv && d.cnt[v] > 1 &&
                def_target(in) == v && is_pure(in->op) && !in->vol &&
                s.pos[v] < 0) {
                dead[i] = 1;
                ndead++;
                continue;
            }
            ra_lset_step(&s, in);
        }
    }
    ra_lset_free(&s);
    ra_live_free(lv);
    free(first); free(last);
    free_defs(&d);
    if (!ndead) {
        free(dead);
        return 0;
    }
    struct ibuf nb = { 0, 0, 0 };
    int *newpos = xmalloc((size_t)(N + 1) * sizeof *newpos);
    for (int n = 0; n < N; n++) {
        newpos[n] = nb.n;
        if (!dead[n])
            *ib_push(&nb) = fn->ins[n];
    }
    newpos[N] = nb.n;
    remap_scopes(fn, newpos, N);
    free(newpos);
    free(dead);
    free(fn->ins);
    fn->ins = nb.p; fn->nins = nb.n; fn->cap = nb.cap;
    return 1;
}
