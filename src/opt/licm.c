/* ==== loop-invariant code motion (-O2) ====================================== *
 *
 * An expression inside a loop whose operands all come from outside it
 * computes the same value every iteration, so it belongs before the
 * loop. `s += a * b + a` in a loop over i runs the multiply n times and
 * needs to run it once.
 *
 * The shape is the textbook one -- natural loops from back edges, a
 * preheader, an invariance fixpoint -- with the pieces already here:
 * build_cfg, compute_rpo and compute_idom were built for mem2reg, and a
 * back edge is exactly an edge whose target dominates its source.
 *
 * ---- the preheader, without a jump ---------------------------------------
 *
 * Hoisted code has to land somewhere that runs once on the way in and is
 * skipped by the back edge. The usual construction is a new block that
 * jumps to the header; this one puts the preheader PHYSICALLY where it
 * has to be instead, immediately before the header's label:
 *
 *      Lpre:                  <- new label, out-of-loop entries retargeted
 *          <hoisted>
 *      Lh:                    <- the header, unchanged
 *          ...
 *          brnz Lh            <- the back edge still lands past the hoist
 *
 * so the preheader falls through into the header and no jump is emitted
 * at all. Entries from outside the loop are retargeted from Lh to Lpre;
 * the back edges are left alone, which is what makes the hoisted code
 * run once. A block that FALLS INTO the header would fall into the
 * preheader instead, which is right when it is outside the loop and
 * wrong when it is inside -- so an in-loop block that ends where the
 * header starts is refused rather than handled.
 *
 * ---- what may move -------------------------------------------------------
 *
 * is_pure() already means "no side effect and cannot fault", which is
 * most of the condition: the preheader runs even when the loop body runs
 * zero times, so anything hoisted is speculated, and a trapping
 * instruction (DIV, MOD) or a LOAD may not be. The rest is that every
 * operand is defined outside the loop, or by something already being
 * hoisted -- a fixpoint, because `t = a * b` hoisting makes `t + a`
 * hoistable in the next round.
 *
 * IR_LDVAR is the one that needs more than purity. It reads a frame
 * slot, which is invariant only if nothing in the loop writes it: no
 * IR_STVAR to that slot, and the slot's address never taken anywhere in
 * the function (an address that escaped could be stored through).
 *
 * One loop is transformed per call and the caller rounds again, because
 * moving instructions invalidates every block boundary. Loops are taken
 * innermost first (highest RPO header), so an inner hoist has already
 * happened when the outer loop is considered and the value can move the
 * whole way out in successive rounds. */

#include "opt_int.h"

/* ==== memory in loops: hoisting loads, promoting a location ================
 *
 * is_pure() keeps every IR_LOAD where it is, which is right for the
 * general case and wrong for the common one: `p->n` read in a loop that
 * never writes p->n, a global's table base, `p->count++` in a loop that
 * touches nothing else that could be p->count. Each is a load (and the
 * last a store) on every iteration where clang has one before the loop
 * and one after. The alias analysis above (mem_access, acc_overlap) is
 * what can tell that nothing in the loop reaches those bytes; this is the
 * loop side of it. Off with -fno-licm-mem.
 *
 * ---- what the loop does to memory ----------------------------------------
 *
 * lmem_build sorts every instruction of the loop into: a read or a write
 * whose bytes are described (a load or store, a slot by ldvar/stvar, the
 * object of a memcpy or memzero), a call, or a BARRIER -- inline asm, a
 * fence, every atomic, anything volatile, and any op it does not know.
 * A barrier stops everything, and volatile is in it for two reasons:
 * volatile is how MMIO and data an interrupt shares are marked, and an
 * atomic load IS a volatile load here (irgen's atomic_load), with no
 * fence beside it on RISC-V -- so a seqlock's re-read of its data must
 * not move across one.
 *
 * A call writes memory unless infer_attrs proved it writes none the
 * caller can see; for promotion it must also read none, since the
 * location's newest value is in a register while the loop runs.
 *
 * ---- when a load may move (hoisting) -------------------------------------
 *
 *   - It is not volatile, not a __flash read, and its address is
 *     invariant (the fixpoint in licm_one).
 *   - No write in the loop may overlap its bytes, there is no barrier,
 *     and every call is proven not to write.
 *   - Executing it before the loop cannot fault where the program did
 *     not. One of: (R1) its block dominates every latch and every
 *     exiting block and the loop makes no call, so the first iteration
 *     executes it with the same address; (R2) it reads a global or a
 *     frame slot at a constant offset that lies inside the object; (R3)
 *     the block the preheader follows already loaded the same bytes, and
 *     nothing after that load could have changed the mapping (no call,
 *     no asm, no stack release). R3 is a rotated loop's guard: `while (q
 *     && q->k < x->k)` loads x->k in the guard before the loop is
 *     entered.
 *
 * ---- when a location may live in a register (promotion) -----------------
 *
 * One location -- the same base value, offset and size -- that the loop
 * both stores and may load:
 *   - no barrier, and every call in the loop reads and writes nothing;
 *   - every other access in the loop cannot overlap it (acc_overlap),
 *     and no value stored in the loop or passed to a call is an address
 *     based on it;
 *   - a store to it dominates every exiting block, so every way out has
 *     stored in its last iteration: the store put on the exit edge
 *     writes a location the program wrote, on a path where it wrote it,
 *     and never invents one. A loop with no exit is refused;
 *   - when the loop reads it, the preheader's load obeys the hoisting
 *     rules above;
 *   - a frame slot only when its scope is the whole function: codegen
 *     shares slots between disjoint scopes, and an exit store outside
 *     the scope would write a neighbour.
 * The loads become copies of one temp, the stores assignments to it
 * (re-extended to what a load would have read back), and the exits store
 * it once. */
struct lmem {
    int barrier;            /* something no rule here reasons about */
    int ncall;              /* IR_CALLs in the loop */
    int wcall;              /* one may write memory the caller sees */
    int rwcall;             /* one may read or write it */
    int multi;              /* a switch: an exit no edge copy can take */
    struct maccess *acc;    /* every described access */
    int *at;                /* ...its instruction */
    char *wr;               /* ...and whether it writes */
    int n, cap;
};

#define LMEM_MAX 512        /* accesses compared per loop, beyond: barrier */

static void lmem_add(struct lmem *m, int n, struct maccess a, int write)
{
    if (m->n == LMEM_MAX) { m->barrier = 1; return; }
    if (m->n == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 32;
        m->acc = xrealloc(m->acc, (size_t)m->cap * sizeof *m->acc);
        m->at = xrealloc(m->at, (size_t)m->cap * sizeof *m->at);
        m->wr = xrealloc(m->wr, (size_t)m->cap);
    }
    m->acc[m->n] = a; m->at[m->n] = n; m->wr[m->n] = (char)write;
    m->n++;
}

static void lmem_free(struct lmem *m)
{
    free(m->acc); free(m->at); free(m->wr);
}

/* Ops that neither read nor write memory, and do nothing else a moved
 * access could be reordered with. Anything not here is a barrier. */
static int lmem_inert(enum ir_op op)
{
    switch (op) {
    case IR_CONST: case IR_MOV: case IR_ADD: case IR_SUB: case IR_MUL:
    case IR_DIV: case IR_MOD: case IR_AND: case IR_OR: case IR_XOR:
    case IR_SHL: case IR_SHR: case IR_NEG: case IR_BNOT: case IR_CMP:
    case IR_ADDR: case IR_STRADDR: case IR_GADDR: case IR_FADDR:
    case IR_EXT: case IR_I2F: case IR_F2I: case IR_F2F: case IR_BITCAST:
    case IR_BSWAP: case IR_SQRT: case IR_SELECT:
    case IR_LABEL: case IR_JMP: case IR_BRZ: case IR_BRNZ: case IR_RET:
    case IR_UD2: case IR_SWITCH: case IR_LABELADDR:
    case IR_VBIN: case IR_VSPLAT: case IR_VREDADD: case IR_VWIDEN:
    case IR_FRAMEADDR: case IR_SPSAVE:
        return 1;
    default:
        return 0;
    }
}

static void lmem_build(struct ir_func *fn, struct defs *d, struct bb *bb,
                       int nbb, const char *in, struct lmem *m)
{
    memset(m, 0, sizeof *m);
    for (int b = 0; b < nbb; b++) {
        if (!in[b])
            continue;
        for (int n = bb[b].start; n < bb[b].end; n++) {
            struct ir_ins *i = &fn->ins[n];
            switch (i->op) {
            case IR_LOAD:
                if (i->vol || i->flash) m->barrier = 1;
                else lmem_add(m, n, mem_access(fn, d, i->a, i->size), 0);
                break;
            case IR_STORE:
                if (i->vol) m->barrier = 1;
                else lmem_add(m, n, mem_access(fn, d, i->a, i->size), 1);
                break;
            case IR_LDVAR:
                if (i->vol || i->a < 0 || i->a >= fn->nvars) m->barrier = 1;
                else lmem_add(m, n, slot_access(i->a), 0);
                break;
            case IR_STVAR:
                if (i->vol || i->dst < 0 || i->dst >= fn->nvars) m->barrier = 1;
                else lmem_add(m, n, slot_access(i->dst), 1);
                break;
            case IR_MEMCPY:
                lmem_add(m, n, obj_access(fn, d, i->a), 1);
                lmem_add(m, n, obj_access(fn, d, i->b), 0);
                break;
            case IR_MEMZERO: case IR_VSTORE:
                lmem_add(m, n, obj_access(fn, d, i->a), 1);
                break;
            case IR_VLOAD:
                lmem_add(m, n, obj_access(fn, d, i->a), 0);
                break;
            case IR_CALL: {
                const struct func *t = i->indirect ? NULL : i->callee;
                m->ncall++;
                if (!t || !t->inf_no_write)
                    m->wcall = m->rwcall = 1;
                else if (!t->inf_no_read)
                    m->rwcall = 1;
                break;
            }
            case IR_SWITCH:
                m->multi = 1;
                break;
            default:
                if (!lmem_inert(i->op))
                    m->barrier = 1;
                break;
            }
        }
    }
}

/* May a load of `x` move out of the loop m describes, as far as memory
 * goes: nothing in it writes those bytes? */
static int lmem_no_write(const struct lmem *m, struct maccess x,
                         const char *taken, int nvars)
{
    if (m->barrier || m->wcall)
        return 0;
    for (int k = 0; k < m->n; k++)
        if (m->wr[k] && acc_overlap(x, m->acc[k], taken, nvars))
            return 0;
    return 1;
}

/* Does block `s` run in every iteration that ends -- at a latch or by
 * leaving? It must dominate every latch and every exiting block. */
static int lp_every_iter(struct bb *bb, int nbb, const char *in, int h, int s,
                         int exits_only)
{
    int any = 0;
    for (int b = 0; b < nbb; b++) {
        if (!in[b])
            continue;
        int need = 0;
        for (int k = 0; k < bb[b].nsucc; k++) {
            int t = bb[b].succ[k];
            if (!in[t]) { need = 1; any = 1; }
            else if (t == h && !exits_only) need = 1;
        }
        if (need && !bb_dominates(bb, s, b))
            return 0;
    }
    return exits_only ? any : 1;
}

/* R2: `size` bytes at x lie inside a global or a frame slot, which are
 * mapped wherever the function runs. Only an object whose extent is
 * known here: not weak (an undefined weak one is at address 0), not
 * placed in a named section, not merely declared. */
static int lp_in_bounds(struct ir_func *fn, struct maccess x, int size)
{
    long objsz = 0;
    if (size <= 0)
        return 0;
    if (x.bkind == BASE_SLOT) {
        if (x.bid < 0 || x.bid >= fn->nvars || !fn->locals)
            return 0;
        objsz = fn->locals[x.bid].size;
    } else if (x.bkind == BASE_GLOBAL) {
        const struct global *g = NULL;
        for (int n = 0; n < fn->nins && !g; n++)
            if (fn->ins[n].op == IR_GADDR && fn->ins[n].glob_sym == x.bid)
                g = fn->ins[n].glob;
        if (!g || g->is_weak || g->section || g->absorbed || !g->defined ||
            !g->ty || g->ty->is_volatile)
            return 0;
        objsz = global_size(g);
    } else {
        return 0;
    }
    return objsz > 0 && x.off < (unsigned long)objsz &&
           (unsigned long)objsz - x.off >= (unsigned long)size;
}

/* R3: the block the preheader will follow loaded every byte of x, and
 * nothing after that load could have taken the mapping away. */
static int lp_guard_loaded(struct ir_func *fn, struct defs *d, struct bb *bb,
                           const char *in, int h, struct maccess x)
{
    int D = bb[h].idom;
    if (D < 0 || D == h || in[D])
        return 0;
    for (int k = 0; k < bb[h].npred; k++) {
        int p = bb[h].pred[k];
        if (!in[p] && p != D)
            return 0;               /* entered from somewhere else as well */
    }
    int found = 0;
    for (int n = bb[D].start; n < bb[D].end; n++) {
        const struct ir_ins *i = &fn->ins[n];
        if (i->op == IR_LOAD && !i->vol && !i->flash) {
            if (acc_covers(mem_access(fn, d, i->a, i->size), x))
                found = 1;
            continue;
        }
        if (!found)
            continue;
        /* A plain access leaves the mapping as it was; a call, asm, a
         * stack release or anything volatile (an MPU register) might
         * not. */
        if (i->vol)
            found = 0;
        else if (!lmem_inert(i->op) && i->op != IR_LOAD &&
                 i->op != IR_STORE && i->op != IR_LDVAR && i->op != IR_STVAR)
            found = 0;
    }
    return found;
}

/* Can a load of x, at instruction n of the loop, run in the preheader
 * without faulting where the program would not have? */
static int lp_load_safe(struct ir_func *fn, struct defs *d, struct bb *bb,
                        int nbb, const char *in, int h, const struct lmem *m,
                        int blk, struct maccess x, int size)
{
    if (!m->ncall && lp_every_iter(bb, nbb, in, h, blk, 0))
        return 1;                                         /* R1 */
    if (lp_in_bounds(fn, x, size))
        return 1;                                         /* R2 */
    return lp_guard_loaded(fn, d, bb, in, h, x);          /* R3 */
}

/* Retarget every entry into the loop from outside -- a branch's label or
 * a switch's table entry -- from the header Lh to the preheader Lpre. */
static void lp_retarget_entries(struct ir_func *fn, struct bb *bb, int nbb,
                                const char *in, int h, int Lh, int Lpre)
{
    for (int p = 0; p < nbb; p++) {
        if (in[p] || bb[p].end <= bb[p].start)
            continue;
        int hits = 0;
        for (int k = 0; k < bb[p].nsucc; k++)
            if (bb[p].succ[k] == h) hits = 1;
        if (!hits)
            continue;
        struct ir_ins *t = &fn->ins[bb[p].end - 1];
        if (t->op == IR_JMP || t->op == IR_BRZ || t->op == IR_BRNZ ||
            t->op == IR_SWITCH) {
            struct retarget rt = { Lh, Lpre };
            each_label(fn, t, retarget_cb, &rt);
        }
    }
}

/* Every definition of v is an integer value of width w: what a copy into
 * the promoted temp may carry when no re-extension sits between. */
static int lp_value_width(struct ir_func *fn, struct defs *d, int v, int w)
{
    if (v < 0 || v >= fn->nvregs || d->cnt[v] < 1)
        return 0;
    if (v < fn->nparams) {
        if (d->cnt[v] != 1 || !fn->locals)
            return 0;
        const struct ir_local *L = &fn->locals[v];
        return L->is_int_or_ptr && !L->is_int128 && L->size == w;
    }
    if (v < fn->nvars)
        return 0;
    defs_lists(fn, d);
    int seen = 0;
    for (int n = d->first[v]; n >= 0; n = d->next[n]) {
        const struct ir_ins *i = &fn->ins[n];
        if (i->flt || i->w != w || i->op == IR_CALL || i->op == IR_ASM)
            return 0;
        seen++;
    }
    return seen == d->cnt[v];
}

/* Promote one location of the loop to a temp; 1 when it did. `in` is
 * the loop, h its header, inl_ins its instructions. */
static int licm_promote(struct ir_func *fn, struct defs *d, struct bb *bb,
                        int nbb, const char *in, const char *inl_ins, int h,
                        const struct lmem *m, const char *taken)
{
    if (m->barrier || m->rwcall || m->multi || m->n == 0)
        return 0;
    if (!lp_every_iter(bb, nbb, in, h, h, 1))
        return 0;                       /* no way out: nowhere to store */
    int nvars = fn->nvars;
    int *i2b = xmalloc((size_t)fn->nins * sizeof *i2b);
    for (int b = 0; b < nbb; b++)
        for (int n = bb[b].start; n < bb[b].end; n++)
            i2b[n] = b;
    char *mem = xmalloc((size_t)m->n);
    int done = 0;
    for (int c = 0; c < m->n && !done; c++) {
        if (!m->wr[c] || fn->ins[m->at[c]].op != IR_STORE)
            continue;
        struct maccess K = m->acc[c];
        int size = K.size;
        if (K.bkind == BASE_NONE ||
            (size != 1 && size != 2 && size != 4 && size != 8))
            continue;
        /* the base is one value for the whole loop */
        if (K.bkind == BASE_VREG) {
            int bd = K.bid >= 0 && K.bid < fn->nvregs ? d->ins[K.bid] : -1;
            if (bd >= 0 && inl_ins[bd])
                continue;
        }
        if (K.bkind == BASE_SLOT &&
            (K.bid < 0 || K.bid >= nvars ||
             (fn->var_scope_lo &&
              (fn->var_scope_lo[K.bid] > 0 || fn->var_scope_hi[K.bid] < fn->nins))))
            continue;
        /* its accesses, and nothing else that could reach it */
        int ok = 1, nld = 0, lsize = 0, lsign = 0, lw = 0, A = -1;
        int sblk_ok = 0, guard_blk = -1, st0 = -1, ld0 = -1;
        for (int k = 0; k < m->n && ok; k++) {
            struct maccess a = m->acc[k];
            const struct ir_ins *i = &fn->ins[m->at[k]];
            mem[k] = 0;
            int same = same_base(a, K) && a.off == K.off && a.size == size &&
                       (i->op == IR_LOAD || i->op == IR_STORE);
            if (!same) {
                if (acc_overlap(a, K, taken, nvars)) ok = 0;
                continue;
            }
            mem[k] = 1;
            if (i->flt || i->imm_b)
                ok = 0;
            if (i->op == IR_LOAD) {
                if (nld++ == 0) {
                    lsign = i->sign; lw = i->w; lsize = i->size; ld0 = m->at[k];
                } else if (i->sign != lsign || i->w != lw || i->size != lsize) {
                    ok = 0;
                }
            } else {
                if (st0 < 0) st0 = m->at[k];
                if (lp_every_iter(bb, nbb, in, h, i2b[m->at[k]], 1))
                    sblk_ok = 1;
            }
            if (lp_every_iter(bb, nbb, in, h, i2b[m->at[k]], 0))
                guard_blk = i2b[m->at[k]];
            if (A < 0 && one_value(fn, d, i->a) &&
                (d->ins[i->a] < 0 || !inl_ins[d->ins[i->a]]))
                A = i->a;
        }
        if (!ok || !sblk_ok || A < 0)
            continue;
        if (!nld)
            lw = size == 8 ? 8 : 4;
        if (lw != 4 && lw != 8)
            continue;
        if (size > lw)
            continue;
        /* the loaded value can be put back where the loop started */
        if (nld && !((!m->ncall && guard_blk >= 0) ||
                     lp_in_bounds(fn, K, size) ||
                     lp_guard_loaded(fn, d, bb, in, h, K)))
            continue;
        /* stored values: an address based on it would escape; a value
         * kept without re-extension must already be what a load reads */
        for (int b = 0; b < nbb && ok; b++) {
            if (!in[b]) continue;
            for (int n = bb[b].start; n < bb[b].end && ok; n++) {
                const struct ir_ins *i = &fn->ins[n];
                if (i->op == IR_STORE) {
                    struct maccess v = mem_access(fn, d, i->b, 1);
                    if (v.bkind == K.bkind && v.bid == K.bid) ok = 0;
                } else if (i->op == IR_CALL) {
                    for (int q = 0; q < i->nargs && ok; q++) {
                        struct maccess v = mem_access(fn, d, i->argv[q].vreg, 1);
                        if (v.bkind == K.bkind && v.bid == K.bid) ok = 0;
                    }
                }
            }
        }
        for (int k = 0; k < m->n && ok; k++)
            if (mem[k] && fn->ins[m->at[k]].op == IR_STORE && size == lw &&
                !lp_value_width(fn, d, fn->ins[m->at[k]].b, lw))
                ok = 0;
        if (!ok)
            continue;

        /* ---- the exits: where the store goes ---- */
        struct pcp { int at, after; } *cps = NULL;
        struct ptr { int lbl, to; } *trs = NULL;
        int ncps = 0, ntrs = 0;
        cps = xmalloc((size_t)(2 * nbb + 1) * sizeof *cps);
        trs = xmalloc((size_t)(2 * nbb + 1) * sizeof *trs);
        for (int b = 0; b < nbb && ok; b++) {
            if (!in[b]) continue;
            struct ir_ins *lt = &fn->ins[bb[b].end - 1];
            int did_tr = 0, did_fall = 0;
            for (int k = 0; k < bb[b].nsucc && ok; k++) {
                int s = bb[b].succ[k];
                if (in[s]) continue;
                int Ls = bb[s].end > bb[s].start &&
                         fn->ins[bb[s].start].op == IR_LABEL
                         ? fn->ins[bb[s].start].label : -1;
                int falls = bb[b].end == bb[s].start && lt->op != IR_JMP &&
                            lt->op != IR_RET && lt->op != IR_UD2 &&
                            lt->op != IR_SWITCH && lt->op != IR_IGOTO;
                int handled = 0;
                if (lt->op == IR_JMP && lt->label == Ls && Ls >= 0) {
                    cps[ncps].at = bb[b].end - 1; cps[ncps].after = 0; ncps++;
                    handled = 1;
                } else if ((lt->op == IR_BRZ || lt->op == IR_BRNZ) &&
                           lt->label == Ls && Ls >= 0) {
                    if (!did_tr) {
                        trs[ntrs].lbl = -1; trs[ntrs].to = b; ntrs++;
                        did_tr = 1;
                    }
                    handled = 1;
                }
                if (falls) {
                    if (!did_fall) {
                        cps[ncps].at = bb[b].end - 1; cps[ncps].after = 1;
                        ncps++; did_fall = 1;
                    }
                    handled = 1;
                }
                if (!handled)
                    ok = 0;                 /* an exit of no known shape */
            }
        }
        if (!ok) { free(cps); free(trs); continue; }

        /* ---- rewrite ---- */
        int T = fn->nvregs++;
        const struct ir_ins st = fn->ins[st0];
        int Lh = fn->ins[bb[h].start].label;
        int Lpre = -1;
        if (nld) {
            Lpre = fn->nlabels++;
            lp_retarget_entries(fn, bb, nbb, in, h, Lh, Lpre);
        }
        for (int t = 0; t < ntrs; t++) {
            struct ir_ins *br = &fn->ins[bb[trs[t].to].end - 1];
            int to = br->label;
            trs[t].lbl = fn->nlabels++;
            br->label = trs[t].lbl;
            trs[t].to = to;
        }
        struct ibuf nb = { 0, 0, 0 };
        int *newpos = fn->var_scope_lo
            ? xmalloc((size_t)(fn->nins + 1) * sizeof *newpos) : NULL;
        char *ismem = xcalloc((size_t)fn->nins, 1);
        for (int k = 0; k < m->n; k++)
            if (mem[k]) ismem[m->at[k]] = 1;
#define LP_EXIT_STORE(lab_line, lab_col)                                \
        do {                                                            \
            struct ir_ins *s_ = ib_push(&nb);                           \
            *s_ = st; s_->a = A; s_->b = T;                             \
            s_->line = (lab_line); s_->col = (lab_col);                 \
            s_->synth = (lab_line) ? 0 : 1;                             \
        } while (0)
        for (int n = 0; n <= fn->nins; n++) {
            if (n < fn->nins && n == bb[h].start && nld) {
                struct ir_ins *l = ib_push(&nb);
                l->op = IR_LABEL; l->label = Lpre;
                l->line = fn->ins[n].line; l->col = fn->ins[n].col;
                l->synth = 1;
                struct ir_ins *ld = ib_push(&nb);
                *ld = fn->ins[ld0]; ld->dst = T; ld->a = A;
            }
            if (newpos) newpos[n] = nb.n;
            for (int q = 0; q < ncps; q++)
                if (cps[q].at == n && !cps[q].after)
                    LP_EXIT_STORE(st.line, st.col);
            if (n == fn->nins) break;
            const struct ir_ins *o = &fn->ins[n];
            if (ismem[n] && o->op == IR_LOAD) {
                struct ir_ins *r = ib_push(&nb);
                r->op = IR_MOV; r->dst = o->dst; r->a = T; r->w = lw;
                r->line = o->line; r->col = o->col; r->synth = o->synth;
            } else if (ismem[n]) {
                struct ir_ins *r = ib_push(&nb);
                r->dst = T; r->a = o->b; r->w = lw;
                if (size < lw) {
                    r->op = IR_EXT; r->size = size; r->sign = lsign;
                } else {
                    r->op = IR_MOV;
                }
                r->line = o->line; r->col = o->col; r->synth = o->synth;
            } else {
                *ib_push(&nb) = *o;
            }
            for (int q = 0; q < ncps; q++)
                if (cps[q].at == n && cps[q].after)
                    LP_EXIT_STORE(st.line, st.col);
        }
        if (ntrs > 0) {
            /* the trampolines go at the end; cap a fall-through first */
            enum ir_op lt = nb.n ? nb.p[nb.n - 1].op : IR_UD2;
            if (lt != IR_JMP && lt != IR_RET && lt != IR_UD2 &&
                lt != IR_IGOTO && lt != IR_SWITCH) {
                struct ir_ins *r = ib_push(&nb);
                r->op = IR_RET; r->synth = 1;
            }
        }
        for (int t = 0; t < ntrs; t++) {
            struct ir_ins *l = ib_push(&nb);
            l->op = IR_LABEL; l->label = trs[t].lbl; l->synth = 1;
            l->line = st.line; l->col = st.col;
            LP_EXIT_STORE(st.line, st.col);
            struct ir_ins *j = ib_push(&nb);
            j->op = IR_JMP; j->label = trs[t].to; j->synth = 1;
            j->line = st.line; j->col = st.col;
        }
#undef LP_EXIT_STORE
        if (newpos) {
            remap_scopes(fn, newpos, fn->nins);
            free(newpos);
        }
        free(fn->ins);
        fn->ins = nb.p; fn->nins = nb.n; fn->cap = nb.cap;
        free(ismem); free(cps); free(trs);
        if (remarks_on() && fn->src)
            remark_add("opt", "promoted", fn->name, "licm/promote",
                       fn->file, st.line ? st.line : fn->line,
                       "a %d-byte location lives in a register across a loop "
                       "(%d load%s, stored at %d exit%s)", size, nld,
                       nld == 1 ? "" : "s", ncps + ntrs,
                       ncps + ntrs == 1 ? "" : "s");
        done = 1;
    }
    free(mem); free(i2b);
    return done;
}

/* -flicm-mem (opt_run sets it): the memory half of LICM above. */
int g_licm_mem = 1;

/* Hoist out of one loop, or report that there was nothing to do. */
static int licm_one(struct ir_func *fn)
{
    if (fn->nins == 0)
        return 0;
    int nbb, *l2b;
    struct bb *bb = build_cfg(fn, &nbb, &l2b);
    int *order = xmalloc((size_t)nbb * sizeof *order), norder;
    compute_rpo(bb, nbb, order, &norder);
    if (norder != nbb) {       /* unreachable blocks: dominance is partial */
        free(order); free(l2b);
        free_cfg(bb, nbb); return 0;
    }
    compute_idom(bb, order, norder);

    /* A slot whose address is taken ANYWHERE could be written through a
     * pointer, so a load of it is never loop-invariant. */
    char *addr_taken = xcalloc((size_t)(fn->nvars ? fn->nvars : 1), 1);
    for (int n = 0; n < fn->nins; n++)
        if (fn->ins[n].op == IR_ADDR && fn->ins[n].a >= 0 &&
            fn->ins[n].a < fn->nvars)
            addr_taken[fn->ins[n].a] = 1;

    struct defs d;
    compute_defs(fn, &d);
    char *in = xmalloc((size_t)nbb);
    char *inl_ins = xmalloc((size_t)fn->nins);
    char *wrin = xmalloc((size_t)(fn->nvregs ? fn->nvregs : 1));
    char *inv = xmalloc((size_t)fn->nins);
    char *stored = xmalloc((size_t)(fn->nvars ? fn->nvars : 1));
    int *hoist = xmalloc((size_t)fn->nins * sizeof *hoist);
    int *i2b = xmalloc((size_t)(fn->nins ? fn->nins : 1) * sizeof *i2b);
    /* a load the memory rules refused, so the fixpoint asks once */
    char *nomem = xmalloc((size_t)(fn->nins ? fn->nins : 1));
    for (int b = 0; b < nbb; b++)
        for (int n = bb[b].start; n < bb[b].end; n++)
            i2b[n] = b;
    int done = 0;

    /* Headers innermost first: a higher RPO number is deeper in. */
    for (int oi = norder - 1; oi >= 0 && !done; oi--) {
        int h = order[oi];
        if (h == 0)                      /* the entry has no preheader slot */
            continue;
        if (bb[h].end <= bb[h].start || fn->ins[bb[h].start].op != IR_LABEL)
            continue;                    /* not a branch target: no back edge */
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

        /* Every way into the loop must be through the header, or the
         * preheader would not dominate what was hoisted into it. */
        int ok = 1, nentry = 0;
        for (int b = 0; b < nbb && ok; b++) {
            if (!in[b])
                continue;
            for (int k = 0; k < bb[b].npred; k++) {
                int p = bb[b].pred[k];
                if (in[p])
                    continue;
                if (b != h) ok = 0;      /* a second entry: not a natural loop */
                else nentry++;
            }
        }
        if (!ok || nentry == 0)
            continue;
        /* And no block INSIDE the loop may fall into the header, since
         * it would fall into the preheader instead. A block that ends
         * where the header starts falls into it unless its last
         * instruction leaves unconditionally. */
        for (int p = 0; p < nbb && ok; p++) {
            if (!in[p] || bb[p].end != bb[h].start || bb[p].end <= bb[p].start)
                continue;
            enum ir_op t = fn->ins[bb[p].end - 1].op;
            if (t != IR_JMP && t != IR_RET && t != IR_UD2 && t != IR_SWITCH)
                ok = 0;
        }
        if (!ok)
            continue;

        /* What the loop does to memory, which decides whether a load of
         * a frame slot can move. */
        memset(inl_ins, 0, (size_t)fn->nins);
        memset(stored, 0, (size_t)(fn->nvars ? fn->nvars : 1));
        for (int b = 0; b < nbb; b++) {
            if (!in[b])
                continue;
            for (int n = bb[b].start; n < bb[b].end; n++) {
                inl_ins[n] = 1;
                if (fn->ins[n].op == IR_STVAR && fn->ins[n].dst >= 0 &&
                    fn->ins[n].dst < fn->nvars)
                    stored[fn->ins[n].dst] = 1;
            }
        }
        /* Which temps the loop writes at all, by ANY definition. A temp
         * written more than once -- an enclosing loop's counter, defined
         * at its entry and again at its latch -- is still invariant here
         * when none of those writes is inside this loop: the loop is
         * entered only through its preheader, and nothing in it changes
         * the value it found there. Refusing every multiply-defined
         * operand refused the row address of every 2-D array walk:
         * `ma[i][k]` recomputed i*48 + ma on each trip of the k loop. */
        memset(wrin, 0, (size_t)(fn->nvregs ? fn->nvregs : 1));
        for (int n = 0; n < fn->nins; n++) {
            if (!inl_ins[n])
                continue;
            int t = def_target(&fn->ins[n]);
            if (t >= 0 && t < fn->nvregs) wrin[t] = 1;
            if (fn->ins[n].op == IR_LANDING && fn->ins[n].b >= 0 &&
                fn->ins[n].b < fn->nvregs)
                wrin[fn->ins[n].b] = 1;
            if (fn->ins[n].op == IR_ASM)
                memset(wrin, 1, (size_t)(fn->nvregs ? fn->nvregs : 1));
        }

        /* What the loop does to memory (lmem_build), for the loads that
         * may move and the location that may be promoted. */
        struct lmem lm;
        memset(&lm, 0, sizeof lm);
        if (g_licm_mem)
            lmem_build(fn, &d, bb, nbb, in, &lm);
        else
            lm.barrier = 1;

        /* The invariance fixpoint. */
        memset(inv, 0, (size_t)fn->nins);
        memset(nomem, 0, (size_t)fn->nins);
        int grew = 1, nloads = 0;
        while (grew) {
            grew = 0;
            for (int n = 0; n < fn->nins; n++) {
                if (!inl_ins[n] || inv[n])
                    continue;
                struct ir_ins *i = &fn->ins[n];
                if (i->vol)
                    continue;
                /* A load: when nothing in the loop writes its bytes and
                 * running it before the loop cannot fault (above). */
                int memld = i->op == IR_LOAD && !i->flash && !lm.barrier &&
                            !nomem[n];
                if (!is_pure(i->op) && !memld)
                    continue;
                int t = def_target(i);
                /* Only a temp, and only one the function defines once:
                 * a slot is not SSA and a repeated def is not a value. */
                if (t < fn->nvars || t >= fn->nvregs || d.cnt[t] != 1)
                    continue;
                if (i->op == IR_LDVAR) {
                    if (i->a < 0 || i->a >= fn->nvars || stored[i->a])
                        continue;
                    /* an address-taken slot: unless no write in the loop
                     * can reach it (a frame slot cannot fault) */
                    if (addr_taken[i->a] &&
                        (lm.barrier ||
                         !lmem_no_write(&lm, slot_access(i->a), addr_taken,
                                        fn->nvars)))
                        continue;
                }
                struct opnds o;
                value_opnds(i, &o);
                if (o.over)
                    continue;
                int all = 1;
                for (int k = 0; k < o.n && all; k++) {
                    int v = o.v[k];
                    if (v < 0 || v >= fn->nvregs) { all = 0; break; }
                    if (d.cnt[v] != 1) {
                        /* several writes: invariant iff none in the loop
                         * (a local's slot has its own rules, above) */
                        if (v < fn->nvars || wrin[v]) all = 0;
                        continue;
                    }
                    int def = d.ins[v];
                    if (def < 0)            /* a parameter, bound at entry */
                        continue;
                    if (inl_ins[def] && !inv[def])
                        all = 0;
                }
                if (all && memld) {
                    /* the memory rules do not change as the fixpoint
                     * grows: asked once per load */
                    struct maccess x = mem_access(fn, &d, i->a, i->size);
                    all = lmem_no_write(&lm, x, addr_taken, fn->nvars) &&
                          lp_load_safe(fn, &d, bb, nbb, in, h, &lm, i2b[n],
                                       x, i->size);
                    if (!all)
                        nomem[n] = 1;
                }
                if (all) {
                    inv[n] = 1; grew = 1;
                    if (i->op == IR_LOAD) nloads++;
                }
            }
        }

        int nh = 0;
        for (int n = 0; n < fn->nins; n++)
            if (inv[n])
                hoist[nh++] = n;
        if (nh == 0) {
            /* Nothing left to hoist: the location the loop keeps going
             * back to memory for, if there is one. */
            if (!lm.barrier &&
                licm_promote(fn, &d, bb, nbb, in, inl_ins, h, &lm, addr_taken))
                done = 1;
            lmem_free(&lm);
            continue;
        }
        lmem_free(&lm);
        /* Emitted in their original order, which must already put each
         * definition before its uses. It does, for a single-assignment
         * temp whose def dominates its uses -- but a hoist that broke
         * that would be a miscompile, so check rather than assume. */
        for (int k = 0; k < nh && ok; k++) {
            struct opnds o;
            value_opnds(&fn->ins[hoist[k]], &o);
            for (int q = 0; q < o.n && ok; q++) {
                if (d.cnt[o.v[q]] != 1)
                    continue;                /* written only outside the loop */
                int def = d.ins[o.v[q]];
                if (def < 0 || !inv[def])
                    continue;
                int seen = 0;
                for (int j = 0; j < k; j++)
                    if (hoist[j] == def) { seen = 1; break; }
                if (!seen) ok = 0;
            }
        }
        if (!ok)
            continue;

        /* Retarget every entry from outside the loop to the preheader.
         * A fall-through entry needs nothing: the preheader is placed
         * immediately before the header, so it lands there already. A
         * switch's table entry is an entry too: left on the header, it
         * jumped past the hoisted definitions. */
        int Lpre = fn->nlabels++;
        lp_retarget_entries(fn, bb, nbb, in, h, Lh, Lpre);

        /* Rebuild: the preheader label and the hoisted instructions go in
         * before the header's label, and the originals come out. */
        char *moved = xcalloc((size_t)fn->nins, 1);
        for (int k = 0; k < nh; k++)
            moved[hoist[k]] = 1;
        int *newpos = fn->var_scope_lo
            ? xmalloc((size_t)(fn->nins + 1) * sizeof *newpos) : NULL;
        struct ibuf nb = { 0, 0, 0 };
        for (int n = 0; n < fn->nins; n++) {
            if (n == bb[h].start) {
                struct ir_ins *l = ib_push(&nb);
                l->op = IR_LABEL;
                l->label = Lpre;
                l->line = fn->ins[n].line;
                l->col = fn->ins[n].col;
                l->synth = 1;            /* no source construct: it is a seam */
                for (int k = 0; k < nh; k++)
                    *ib_push(&nb) = fn->ins[hoist[k]];
            }
            if (newpos) newpos[n] = nb.n;
            if (moved[n])
                continue;
            *ib_push(&nb) = fn->ins[n];
        }
        if (newpos) {
            newpos[fn->nins] = nb.n;
            for (int v = 0; v < fn->nvars; v++) {
                int lo = fn->var_scope_lo[v], hi = fn->var_scope_hi[v];
                if (lo >= 0 && lo <= fn->nins) fn->var_scope_lo[v] = newpos[lo];
                if (hi >= 0 && hi <= fn->nins) fn->var_scope_hi[v] = newpos[hi];
            }
            free(newpos);
        }
        free(fn->ins);
        fn->ins = nb.p;
        fn->nins = nb.n;
        fn->cap = nb.cap;
        free(moved);
        if (remarks_on() && fn->src) {
            remark_add("opt", "hoisted", fn->name, "licm/loop-invariant",
                       fn->file, fn->line,
                       "%d instruction%s out of a loop", nh, nh == 1 ? "" : "s");
            if (nloads)
                remark_add("opt", "hoisted", fn->name, "licm/load",
                           fn->file, fn->line, "%d load%s out of a loop: "
                           "nothing in it writes what %s", nloads,
                           nloads == 1 ? "" : "s",
                           nloads == 1 ? "it reads" : "they read");
        }
        done = 1;
    }

    free(i2b); free(nomem);
    free(hoist); free(stored); free(inv); free(inl_ins); free(wrin); free(in);
    free_defs(&d); free(addr_taken);
    free(order); free(l2b);
    free_cfg(bb, nbb);
    return done;
}

int pass_licm(struct ir_func *fn)
{
    int changed = 0, guard = 0;
    while (guard++ < 64 && licm_one(fn))
        changed = 1;
    return changed;
}
