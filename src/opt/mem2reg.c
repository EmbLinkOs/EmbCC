/* ==== SSA-based mem2reg (-O2) =============================================== *
 *
 * Promotes every non-address-taken scalar local out of memory into SSA temps.
 * Builds the CFG, the dominator tree (Cooper-Harvey-Kennedy) and dominance
 * frontiers, inserts phi-functions at the iterated frontier of each variable's
 * defs, renames defs/uses to versioned temps down the dominator tree, then
 * destructs SSA by realising each phi as copies on its incoming edges — the
 * branch-taken edge via a trampoline block, a branch's fall-through with inline
 * copies (they run only when the branch is not taken), single-successor edges by
 * appending. Every copy set is sequenced read-all-then-write-all through fresh
 * temps, so a swap or a self-referential loop phi is safe. This turns
 * STVAR/LDVAR chains that cross basic blocks — loop counters, a value live down
 * one arm of an `if` — into temps that fold/lvn/copyprop then optimise, which is
 * what the block-local store-forwarding could not reach. */

#include "opt_int.h"

/* A full-width plain access (no truncation/extension mismatch between a store
 * and a load) — the same soundness gate mem2reg and store-forwarding share. */
static int m2r_plain(int size, int sign, int w)
{
    return size == 8 || (size == 4 && !(sign && w == 8));
}

/* The width of a promoted local's value: 16 for a long double (binary128
 * or x87, w 16 everywhere else in the IR), else 8 or 4 -- a 16-byte value
 * copied at width 4 moved a quarter of it. */
int m2r_w(int size)
{
    return size == 16 ? 16 : size == 8 ? 8 : 4;
}

/* Emit the phi copies for edge (pred p -> block s): read every incoming value
 * into a fresh temp, then write each phi result — read-all-then-write-all, so a
 * self-referential loop phi or a swap is realised correctly. */
static void emit_edge_copies(struct ibuf *nb, struct bb *bb, int s, int p,
                             struct ir_func *fn)
{
    struct bb *S = &bb[s];
    if (S->nphi == 0) return;
    int pi = bb_pred_index(S, p);
    if (pi < 0) return;
    /* A phi copy runs on the EDGE from p, so it belongs to whatever ends p
     * -- the branch or the fall-through's last instruction (R3). Going out
     * of SSA is the compiler's own bookkeeping, but the copy still executes
     * at a place the programmer wrote, and a line table with a hole here is
     * a debugger stepping into nowhere. */
    int eline = 0, ecol = 0;
    if (bb[p].end > bb[p].start) {
        eline = fn->ins[bb[p].end - 1].line;
        ecol = fn->ins[bb[p].end - 1].col;
    }
    /* A conflict — an incoming value that is another phi result of this block
     * (a swap, a self-referential loop phi) — needs read-all-then-write-all
     * through temps. The common case has none: emit direct copies, no temps. */
    int conflict = 0;
    for (int k = 0; k < S->nphi && !conflict; k++)
        for (int j = 0; j < S->nphi; j++)
            if (S->phi_inc[pi][k] == S->phi_res[j]) { conflict = 1; break; }
    if (!conflict) {
        for (int k = 0; k < S->nphi; k++) {
            struct ir_ins *mv = ib_push(nb);
            mv->op = IR_MOV; mv->dst = S->phi_res[k]; mv->a = S->phi_inc[pi][k];
            mv->w = m2r_w(fn->locals[S->phi_local[k]].size);
            mv->line = eline; mv->col = ecol; mv->synth = !eline;
        }
        return;
    }
    int *tmp = xmalloc((size_t)S->nphi * sizeof *tmp);
    for (int k = 0; k < S->nphi; k++) {
        tmp[k] = fn->nvregs++;
        struct ir_ins *mv = ib_push(nb);
        mv->op = IR_MOV; mv->dst = tmp[k]; mv->a = S->phi_inc[pi][k];
        mv->w = m2r_w(fn->locals[S->phi_local[k]].size);
        mv->line = eline; mv->col = ecol; mv->synth = !eline;
    }
    for (int k = 0; k < S->nphi; k++) {
        struct ir_ins *mv = ib_push(nb);
        mv->op = IR_MOV; mv->dst = S->phi_res[k]; mv->a = tmp[k];
        mv->w = m2r_w(fn->locals[S->phi_local[k]].size);
        mv->line = eline; mv->col = ecol; mv->synth = !eline;
    }
    free(tmp);
}

static void mem2reg_free(struct bb *bb, int nbb, int **df, int *ndf, int *l2b,
                         int *order)
{
    for (int i = 0; i < nbb; i++) {
        free(bb[i].pred); free(bb[i].succ);
        free(bb[i].phi_local); free(bb[i].phi_res);
        if (bb[i].phi_inc) {
            for (int k = 0; k < bb[i].npred; k++) free(bb[i].phi_inc[k]);
            free(bb[i].phi_inc);
        }
        free(df[i]);
    }
    free(bb); free(df); free(ndf); free(l2b); free(order);
}

/* The name a local was written with. irgen records these unconditionally
 * ("harmless when -g is off"), so a remark can name the variable the
 * programmer knows rather than a slot number. */
const struct ir_dbgvar *local_var(const struct ir_func *fn, int L)
{
    for (int i = 0; i < fn->ndbgvars; i++)
        if (fn->dbgvars[i].vreg == L && !fn->dbgvars[i].is_param)
            return &fn->dbgvars[i];
    return NULL;
}

int pass_mem2reg(struct ir_func *fn)
{
    int nvars = fn->nvars;
    if (nvars == 0 || fn->nins == 0)
        return 0;

    /* 1. Promotable locals: a scalar int/ptr of 4 or 8 bytes, never
     * address-taken, every load full-width plain.
     *
     * Each rejection keeps its OWN reason (R2): "why is this variable still
     * on the stack" is the question this pass answers, and five different
     * causes used to leave the same zero behind. */
    int nparams = fn->nparams;
    char *ok = xmalloc((size_t)nvars);
    const char **why = xcalloc((size_t)nvars, sizeof *why);
    for (int L = 0; L < nvars; L++) {
        const struct ir_local *Li = &fn->locals[L];
        /* A PARAMETER promotes too. It used to be excluded for having
         * no defining instruction to seed its first read with -- but it
         * does have one, outside the IR: the prologue, which writes the
         * incoming value into the parameter's own home. Slots and temps
         * share one numbering (vreg L IS local L), so that home is a
         * vreg like any other and the seed is simply L itself. Every
         * read of a parameter that is never assigned then becomes a
         * direct use of the incoming value and the read disappears --
         * 1692 of the 2584 ldvars across lib/libc and lib/libcxx, with
         * only 173 stvars naming a parameter at all. What is really
         * gained is downstream: value numbering, PRE, LICM and strength
         * reduction all used to stop at a parameter read. */
        ok[L] = 1;
        /* A float or double promotes too. The temp it becomes is still
         * read by SSE instructions, which take their operand from a
         * slot, and the register allocator marks anything a float op
         * touches as ineligible -- so it does not end up in a register
         * and nothing about codegen changes. What IS gained is that
         * folding, value numbering and copy propagation can finally see
         * through it: a double loaded twice becomes one load, and a
         * value carried across a branch stops being a store and a
         * reload. 188 locals in lib/libc and lib/libcxx were refused
         * here for being neither an integer nor a pointer. */
        /* ...and a one- or two-byte integer or pointer: every pointer
         * and `int` on AVR, and a char or short anywhere. Each read of
         * it becomes an EXTENSION of the current value from the local's
         * width, which is exactly what the narrow load did; each store
         * keeps the whole value, since only the low bytes are ever read
         * back. Refusing them left every AVR local in memory, where the
         * value numbering, LICM and strength reduction below cannot see. */
        int narrow = Li->is_int_or_ptr && !Li->is_int128 &&
                     (Li->size == 1 || Li->size == 2);
        /* ...and a 16-byte long double (x87 or binary128). Kept in
         * memory, every read was a 16-byte copy into a temp's slot and
         * every call argument another: RV32's roundl was three times
         * clang's size. Its copies are w 16 (m2r_w), and an undefined
         * one's seed a pooled zero. EMBCC_NO_M2R_LD=1 keeps them in
         * memory, for bisecting. */
        int ld16 = Li->is_ldouble && Li->size == 16 &&
                   !getenv("EMBCC_NO_M2R_LD");
        int promotable = Li->is_scalar_int_or_ptr || Li->is_scalar_float ||
                         narrow || ld16;
        if (!Li->size)                          { ok[L] = 0; why[L] = "type-unknown"; }
        else if (!promotable && (Li->size == 4 || Li->size == 8))
                                                { ok[L] = 0; why[L] = "not-a-scalar-integer-pointer-or-float"; }
        else if (!promotable)                   { ok[L] = 0; why[L] = "not-4-or-8-bytes"; }
    }
    for (int L = 0; L < nvars; L++)
        if (ok[L] && fn->locals[L].is_volatile) {
            ok[L] = 0;                          /* volatile: every access must stay */
            why[L] = "declared-volatile";
        }
    for (int i = 0; i < fn->nins; i++) {
        struct ir_ins *in = &fn->ins[i];
        if (in->op == IR_ADDR && in->a >= 0 && in->a < nvars && ok[in->a]) {
            ok[in->a] = 0; why[in->a] = "address-is-taken";
        }
        if (in->op == IR_LDVAR && in->a >= 0 && in->a < nvars && ok[in->a] &&
            (in->vol ||
             (fn->locals[in->a].size < 4
                  ? in->size != fn->locals[in->a].size
                  : fn->locals[in->a].size == 16
                  ? !(in->size == 16 && in->w == 16)
                  : !m2r_plain(in->size, in->sign, in->w)) ||
             /* a narrower read of a wider local is its FIRST bytes,
              * which are the value's low end only little-endian */
             (target_big_endian() &&
              in->size != fn->locals[in->a].size))) {
            ok[in->a] = 0;
            why[in->a] = in->vol ? "read-is-volatile"
                                 : "read-is-partial-or-extending";
        }
        if (in->op == IR_STVAR && in->dst >= 0 && in->dst < nvars &&
            in->vol && ok[in->dst]) {
            ok[in->dst] = 0; why[in->dst] = "write-is-volatile";
        }
        /* A narrow local is promoted only if every write covers all of
         * it: a partial store leaves bytes the new value does not carry. */
        if (in->op == IR_STVAR && in->dst >= 0 && in->dst < nvars &&
            ok[in->dst] && fn->locals[in->dst].size < 4 &&
            in->size != fn->locals[in->dst].size) {
            ok[in->dst] = 0; why[in->dst] = "write-is-partial";
        }
    }
    if (remarks_on())
        for (int L = 0; L < nvars; L++) {
            const struct ir_dbgvar *v = local_var(fn, L);
            if (!v || !v->name || v->name[0] == '<')  /* a compiler-invented name */
                continue;
            const char *file = fn->src ? fn->file : NULL;
            int line = v->line ? v->line : (fn->src ? fn->line : 0);
            if (ok[L])
                remark_add("mem2reg", "promoted-to-register", v->name,
                           "scalar-and-never-addressed", file, line, NULL);
            else
                remark_add("mem2reg", "kept-in-memory", v->name,
                           why[L] ? why[L] : "unknown", file, line, NULL);
        }
    free(why);
    int nprom = 0;
    int *prom = xmalloc((size_t)nvars * sizeof *prom);     /* local -> prom idx */
    int *ploc = xmalloc((size_t)nvars * sizeof *ploc);     /* prom idx -> local */
    for (int L = 0; L < nvars; L++)
        prom[L] = ok[L] ? (ploc[nprom] = L, nprom++) : -1;
    free(ok);
    if (nprom == 0) { free(prom); free(ploc); return 0; }
    /* -g: a promoted variable that is ASSIGNED leaves its slot behind.
     * For a local nothing writes the slot any more, which a backend sees
     * for itself; a parameter's slot is still written once, by the
     * prologue, and only this can say the value there goes stale. */
    for (int i = 0; i < fn->nins; i++) {
        const struct ir_ins *in = &fn->ins[i];
        if (in->op != IR_STVAR || in->dst < 0 || in->dst >= nvars ||
            prom[in->dst] < 0)
            continue;
        for (int d = 0; d < fn->ndbgvars; d++)
            if (fn->dbgvars[d].vreg == in->dst)
                fn->dbgvars[d].moved = 1;
    }

    /* The ENTRY BLOCK must not be a join. It is one whenever the
     * function's first instruction is a label something branches back
     * to -- a `while` at the top of a function, and every tail call
     * pass_tailrec turned into a loop. Such a block has exactly ONE
     * predecessor, the back edge, because the function entry is not a
     * block; compute_df skips it for having npred < 2, no phi is placed,
     * and the value assigned round the loop is silently dropped. For a
     * local that is unreachable in a defined program (nothing could have
     * initialised it before a loop that starts at instruction zero), and
     * for a PARAMETER it is the argument the caller passed:
     * `sum_to(n, acc)` came out as a branch around an empty body, three
     * million tail calls that never returned.
     *
     * One explicit jump gives the entry a block of its own, and the
     * header two predecessors and a phi. pass_cfgclean takes the jump
     * out again afterwards -- it goes to the very next instruction. */
    if (fn->nins > 0 && fn->ins[0].op == IR_LABEL) {
        int L0 = fn->ins[0].label, reached = 0;
        for (int i = 1; i < fn->nins && !reached; i++) {
            enum ir_op op = fn->ins[i].op;
            if ((op == IR_JMP || op == IR_BRZ || op == IR_BRNZ) &&
                fn->ins[i].label == L0)
                reached = 1;
        }
        if (reached) {
            struct ibuf eb = { 0, 0, 0 };
            struct ir_ins *j = ib_push(&eb);
            j->op = IR_JMP; j->label = L0;
            j->dst = -1; j->a = -1; j->b = -1;
            j->line = fn->ins[0].line; j->col = fn->ins[0].col;
            j->synth = 1;
            for (int i = 0; i < fn->nins; i++)
                *ib_push(&eb) = fn->ins[i];
            free(fn->ins);
            fn->ins = eb.p; fn->nins = eb.n; fn->cap = eb.cap;
        }
    }

    /* 2. CFG + dominance. */
    int nbb, *l2b;
    struct bb *bb = build_cfg(fn, &nbb, &l2b);
    int *order = xmalloc((size_t)nbb * sizeof *order), norder;
    compute_rpo(bb, nbb, order, &norder);
    if (norder != nbb) {   /* unreachable blocks: bail rather than mis-dominate */
        free(order); free(l2b);
        free_cfg(bb, nbb); free(prom); free(ploc); return 0;
    }
    compute_idom(bb, order, norder);

    int **df = xcalloc((size_t)nbb, sizeof *df);
    int *ndf = xcalloc((size_t)nbb, sizeof *ndf);
    compute_df(bb, nbb, df, ndf);

    /* 3. Phi insertion at the iterated dominance frontier of each var's defs.
     *
     * The blocks that store each variable are found in one pass, in block
     * order, rather than by a pass over the function per variable; and
     * "this block has its phi / is on the list" is a stamp of the
     * variable's number per block rather than a flag per (block,
     * variable) -- 4000 locals in a function were 4000 passes. */
    int *dcnt = xcalloc((size_t)nprom + 1, sizeof *dcnt), *work0 = NULL;
    int *dlast = xcalloc((size_t)(nprom ? nprom : 1), sizeof *dlast);
    for (int pass = 0; pass < 2; pass++) {
        for (int pidx = 0; pidx < nprom; pidx++) dlast[pidx] = 0;
        for (int bI = 0; bI < nbb; bI++)
            for (int i = bb[bI].start; i < bb[bI].end; i++) {
                const struct ir_ins *in = &fn->ins[i];
                if (in->op != IR_STVAR || in->dst < 0 || in->dst >= nvars ||
                    prom[in->dst] < 0 || dlast[prom[in->dst]] == bI + 1)
                    continue;
                int pidx = prom[in->dst];
                dlast[pidx] = bI + 1;
                if (pass) work0[dcnt[pidx]++] = bI;
                else dcnt[pidx + 1]++;
            }
        if (!pass) {
            for (int pidx = 0; pidx < nprom; pidx++) dcnt[pidx + 1] += dcnt[pidx];
            work0 = xmalloc((size_t)(dcnt[nprom] ? dcnt[nprom] : 1) * sizeof *work0);
        }
    }
    /* the second pass filled each list from its start, so dcnt[pidx] is
     * now the end of pidx's list, and it starts where the one before ends */
    int *hasphi = xcalloc((size_t)(nbb ? nbb : 1), sizeof *hasphi);
    int *ondef = xcalloc((size_t)(nbb ? nbb : 1), sizeof *ondef);
    int *work = xmalloc((size_t)nbb * sizeof *work);
    for (int pidx = 0; pidx < nprom; pidx++) {
        int L = ploc[pidx], nw = 0;
        for (int k = pidx ? dcnt[pidx - 1] : 0; k < dcnt[pidx]; k++) {
            int bI = work0[k];
            ondef[bI] = pidx + 1;
            work[nw++] = bI;
        }
        while (nw) {
            int x = work[--nw];
            for (int j = 0; j < ndf[x]; j++) {
                int d = df[x][j];
                if (hasphi[d] == pidx + 1) continue;
                hasphi[d] = pidx + 1;
                bb[d].phi_local = xrealloc(bb[d].phi_local, (size_t)(bb[d].nphi+1)*sizeof(int));
                bb[d].phi_res   = xrealloc(bb[d].phi_res,   (size_t)(bb[d].nphi+1)*sizeof(int));
                bb[d].phi_local[bb[d].nphi] = L;
                bb[d].phi_res[bb[d].nphi] = fn->nvregs++;
                bb[d].nphi++;
                if (ondef[d] != pidx + 1) { ondef[d] = pidx + 1; work[nw++] = d; }
            }
        }
    }
    free(ondef); free(dcnt); free(dlast); free(work0);
    for (int b = 0; b < nbb; b++) if (bb[b].nphi) {
        bb[b].phi_inc = xcalloc((size_t)bb[b].npred, sizeof *bb[b].phi_inc);
        for (int k = 0; k < bb[b].npred; k++)
            bb[b].phi_inc[k] = xmalloc((size_t)bb[b].nphi * sizeof(int));
    }

    /* 4. Rename down the dominator tree. Each prom has a version stack; an entry
     * "undef" temp (0) gives an uninitialised read a defined value. */
    int *undef = xmalloc((size_t)nprom * sizeof *undef);
    int **stk = xmalloc((size_t)nprom * sizeof *stk);
    int *sp = xcalloc((size_t)nprom, sizeof *sp);
    int *scap = xcalloc((size_t)nprom, sizeof *scap);
    for (int p = 0; p < nprom; p++) {
        /* A parameter's entry version is its own vreg -- the prologue
         * put the incoming value there. Everything else starts at a
         * fresh temp defined to zero below, standing for a read of a
         * variable the program never wrote. */
        undef[p] = ploc[p] < nparams ? ploc[p] : fn->nvregs++;
        stk[p] = xmalloc(sizeof(int) * 8); scap[p] = 8;
        stk[p][sp[p]++] = undef[p];
    }
    /* explicit dominator-tree DFS (children = blocks whose idom is this block).
     * Each block's children are listed once, in block order, and pushed in
     * that order; what a block pushes onto the version stacks is logged,
     * and leaving it pops back to where the log stood on entry. Both used
     * to cost blocks x blocks and blocks x variables. */
    int *dstk = xmalloc((size_t)nbb * sizeof *dstk);
    int *mark = xmalloc((size_t)nbb * sizeof *mark);
    int *plog = NULL, nplog = 0, cplog = 0;     /* pidx of each push */
    char *entered = xcalloc((size_t)nbb, 1);
    int *kid = xcalloc((size_t)nbb + 1, sizeof *kid);
    int *kids = xmalloc((size_t)(nbb ? nbb : 1) * sizeof *kids);
    for (int c = 1; c < nbb; c++)
        if (bb[c].idom >= 0 && bb[c].idom < nbb) kid[bb[c].idom + 1]++;
    for (int c = 0; c < nbb; c++) kid[c + 1] += kid[c];
    {
        int *fill = xmalloc((size_t)(nbb ? nbb : 1) * sizeof *fill);
        for (int c = 0; c < nbb; c++) fill[c] = kid[c];
        for (int c = 1; c < nbb; c++)
            if (bb[c].idom >= 0 && bb[c].idom < nbb)
                kids[fill[bb[c].idom]++] = c;
        free(fill);
    }
    int dsp = 0; dstk[dsp++] = 0;
    while (dsp) {
        int b = dstk[dsp - 1];
        if (!entered[b]) {
            entered[b] = 1;
            mark[b] = nplog;
            /* phi defs become the current version */
            for (int k = 0; k < bb[b].nphi; k++) {
                int pidx = prom[bb[b].phi_local[k]];
                if (sp[pidx] == scap[pidx]) { scap[pidx]*=2; stk[pidx]=xrealloc(stk[pidx],(size_t)scap[pidx]*sizeof(int)); }
                stk[pidx][sp[pidx]++] = bb[b].phi_res[k];
                if (nplog == cplog) { cplog = cplog ? cplog * 2 : 64;
                    plog = xrealloc(plog, (size_t)cplog * sizeof *plog); }
                plog[nplog++] = pidx;
            }
            for (int i = bb[b].start; i < bb[b].end; i++) {
                struct ir_ins *in = &fn->ins[i];
                if (in->op == IR_LDVAR && in->a >= 0 && in->a < nvars && prom[in->a] >= 0) {
                    int pidx = prom[in->a];
                    int fw = m2r_w(in->size);
                    int was_float = in->flt;
                    if (fn->locals[ploc[pidx]].size < 4) {
                        /* A narrow local: the read extends the value's low
                         * `size` bytes, as the load did -- size, sign and
                         * width are the load's own. */
                        in->op = IR_EXT; in->a = stk[pidx][sp[pidx]-1];
                        in->b = -1;
                        continue;
                    }
                    in->op = IR_MOV; in->a = stk[pidx][sp[pidx]-1]; in->b = -1;
                    if (was_float) {
                        /* A copy of the BITS, at the variable's width.
                         * Left as a float move it would go through the
                         * SSE path, which loads from a slot the temp no
                         * longer has. */
                        in->flt = 0; in->sign = 0; in->size = 0; in->w = fw;
                    }
                } else if (in->op == IR_STVAR && in->dst >= 0 && in->dst < nvars && prom[in->dst] >= 0) {
                    int pidx = prom[in->dst];
                    if (sp[pidx] == scap[pidx]) { scap[pidx]*=2; stk[pidx]=xrealloc(stk[pidx],(size_t)scap[pidx]*sizeof(int)); }
                    stk[pidx][sp[pidx]++] = in->a;   /* the stored temp is the new version */
                    if (nplog == cplog) { cplog = cplog ? cplog * 2 : 64;
                        plog = xrealloc(plog, (size_t)cplog * sizeof *plog); }
                    plog[nplog++] = pidx;
                    in->op = IR_MOV; in->dst = -1; in->a = -1;  /* mark: drop in rebuild */
                }
            }
            /* fill successors' phi incoming from this block */
            for (int s = 0; s < bb[b].nsucc; s++) {
                int sb = bb[b].succ[s];
                if (!bb[sb].nphi) continue;
                int pk = bb_pred_index(&bb[sb], b);
                for (int k = 0; k < bb[sb].nphi; k++) {
                    int pidx = prom[bb[sb].phi_local[k]];
                    bb[sb].phi_inc[pk][k] = stk[pidx][sp[pidx]-1];
                }
            }
            /* push dom-tree children */
            for (int q = kid[b]; q < kid[b + 1]; q++)
                if (!entered[kids[q]]) dstk[dsp++] = kids[q];
        } else {
            /* leaving b: pop its versions */
            while (nplog > mark[b])
                sp[plog[--nplog]]--;
            dsp--;
        }
    }
    free(mark); free(plog); free(kid); free(kids);

    /* 5. Rebuild the linear IR out of SSA. */
    struct ibuf nb = { 0, 0, 0 };
    for (int p = 0; p < nprom; p++) {   /* entry undef defs */
        if (ploc[p] < nparams)
            continue;                   /* the prologue defined it */
        if (fn->locals[ploc[p]].size == 16) {
            /* no IR_CONST is 16 bytes wide: the seed is a load of a
             * pooled zero, as irgen makes every long double constant */
            static const char zero16[16];
            struct ir_ins *a = ib_push(&nb);
            memset(a, 0, sizeof *a);
            a->op = IR_STRADDR; a->dst = fn->nvregs++;
            a->label = ir_intern_aligned(g_fold_unit, zero16, 16, 16);
            a->a = a->b = a->c = -1; a->w = 4; a->size = 4; a->sign = 1;
            a->callee_sym = a->glob_sym = -1; a->synth = 1;
            int at = a->dst;
            struct ir_ins *l = ib_push(&nb);
            memset(l, 0, sizeof *l);
            l->op = IR_LOAD; l->dst = undef[p]; l->a = at;
            l->b = l->c = -1; l->size = 16; l->w = 16; l->natural = 1;
            l->label = -1; l->callee_sym = l->glob_sym = -1; l->synth = 1;
            continue;
        }
        struct ir_ins *c = ib_push(&nb);
        c->op = IR_CONST; c->dst = undef[p]; c->imm = 0;
        c->w = fn->locals[ploc[p]].size == 8 ? 8 : 4;
        /* The seed for a variable read before it is written: it stands for
         * a value the program never produced, so it corresponds to no
         * source construct at all. The §9.1 exception, marked so the
         * verifier can tell it from a location a pass forgot to copy. */
        c->synth = 1;
    }
    /* A phi in the ENTRY block has no edge to take the function's
     * incoming value from: the entry is not a block, so the only
     * predecessor such a phi has is the back edge that made block 0 a
     * loop header -- which is every function whose first instruction is
     * a loop's label, and every tail call pass_tailrec turned into one.
     *
     * The value is supplied HERE instead, as the copy it is, ahead of
     * block 0's own label so the back edge jumps past it. For a
     * parameter that value is the parameter; for anything else it is the
     * undef seed above. Without it `sum_to(n, acc)` came out as a branch
     * around an empty body: three million tail calls that never
     * returned. */
    for (int k = 0; k < bb[0].nphi; k++) {
        int pidx = prom[bb[0].phi_local[k]];
        struct ir_ins *c = ib_push(&nb);
        c->op = IR_MOV; c->dst = bb[0].phi_res[k];
        c->a = undef[pidx]; c->b = -1;
        c->w = m2r_w(fn->locals[ploc[pidx]].size);
        c->synth = 1;
    }
    struct { int lbl, from, edge_pred; } *tramp = NULL; int ntramp = 0, ctramp = 0;
    for (int b = 0; b < nbb; b++) {
        int hasterm = bb[b].end > bb[b].start;
        enum ir_op top = hasterm ? fn->ins[bb[b].end - 1].op : IR_UD2;
        int isterm = top == IR_JMP || top == IR_BRZ || top == IR_BRNZ ||
                     top == IR_RET || top == IR_UD2 || top == IR_SWITCH;
        int body_end = (hasterm && isterm) ? bb[b].end - 1 : bb[b].end;
        for (int i = bb[b].start; i < body_end; i++)
            if (!(fn->ins[i].op == IR_MOV && fn->ins[i].dst < 0))   /* dropped store */
                *ib_push(&nb) = fn->ins[i];
        if (top == IR_RET || top == IR_UD2) {
            if (isterm) *ib_push(&nb) = fn->ins[bb[b].end - 1];
        } else if (top == IR_JMP && isterm) {
            emit_edge_copies(&nb, bb, bb[b].succ[0], b, fn);
            *ib_push(&nb) = fn->ins[bb[b].end - 1];
        } else if ((top == IR_BRZ || top == IR_BRNZ) && isterm) {
            struct ir_ins br = fn->ins[bb[b].end - 1];   /* branch-taken = succ[0] */
            int taken = bb[b].succ[0];
            if (bb[taken].nphi) {                        /* trampoline the taken edge */
                int Lt = fn->nlabels++;
                if (ntramp == ctramp) { ctramp = ctramp?ctramp*2:8;
                    tramp = xrealloc(tramp, (size_t)ctramp*sizeof *tramp); }
                tramp[ntramp].lbl = Lt; tramp[ntramp].from = b;
                tramp[ntramp].edge_pred = taken; ntramp++;
                br.label = Lt;
            }
            *ib_push(&nb) = br;
            if (bb[b].nsucc > 1)                         /* fall-through copies (inline) */
                emit_edge_copies(&nb, bb, bb[b].succ[1], b, fn);
        } else if (top == IR_SWITCH && isterm) {
            /* Every edge out of a switch is a taken edge: a successor
             * with phis gets a trampoline, and every entry (and the
             * default) that named it names the trampoline instead. A
             * block may open with several labels; any of them may be
             * what the table says. */
            struct ir_ins sw = fn->ins[bb[b].end - 1];
            for (int s = 0; s < bb[b].nsucc; s++) {
                int sb = bb[b].succ[s];
                if (!bb[sb].nphi) continue;
                int Lt = fn->nlabels++;
                if (ntramp == ctramp) { ctramp = ctramp?ctramp*2:8;
                    tramp = xrealloc(tramp, (size_t)ctramp*sizeof *tramp); }
                tramp[ntramp].lbl = Lt; tramp[ntramp].from = b;
                tramp[ntramp].edge_pred = sb; ntramp++;
                for (int q = bb[sb].start;
                     q < bb[sb].end && fn->ins[q].op == IR_LABEL; q++) {
                    struct retarget rt = { fn->ins[q].label, Lt };
                    each_label(fn, &sw, retarget_cb, &rt);
                }
            }
            *ib_push(&nb) = sw;
        } else {   /* falls through to the next block */
            if (bb[b].nsucc > 0)
                emit_edge_copies(&nb, bb, bb[b].succ[0], b, fn);
        }
    }
    /* The last real block may fall off the end — an implicit return that the
     * original linear IR carries no explicit RET for (codegen returns at the
     * function's physical end). Trampoline blocks are appended next, so a
     * fall-through last block would run straight into one. Cap it with a void
     * RET. Only a block that does NOT end in an unconditional jump/ret can
     * reach the next physical instruction, so only those need the cap. */
    if (ntramp > 0 && nbb > 0) {
        enum ir_op lt = bb[nbb - 1].end > bb[nbb - 1].start
                        ? fn->ins[bb[nbb - 1].end - 1].op : IR_UD2;
        if (lt != IR_JMP && lt != IR_RET && lt != IR_UD2 && lt != IR_SWITCH) {
            struct ir_ins *r = ib_push(&nb);
            r->op = IR_RET; r->a = -1;
            /* A cap so the trampolines below cannot be fallen into: it
             * stands for no `return` the programmer wrote (R3, §9.1). */
            r->synth = 1;
        }
    }
    /* A trampoline block exists because SSA had to be undone on one edge.
     * Its label and its jump are the compiler's own; the copies between
     * them take the edge's location in emit_edge_copies. */
    for (int t = 0; t < ntramp; t++) {   /* trampoline blocks: label; copies; jmp */
        struct ir_ins *lb = ib_push(&nb);
        lb->op = IR_LABEL; lb->label = tramp[t].lbl;
        lb->synth = 1;
        emit_edge_copies(&nb, bb, tramp[t].edge_pred, tramp[t].from, fn);
        struct ir_ins *jp = ib_push(&nb);
        int origlbl = -1;
        for (int i = bb[tramp[t].edge_pred].start; i < bb[tramp[t].edge_pred].end; i++)
            if (fn->ins[i].op == IR_LABEL) { origlbl = fn->ins[i].label; break; }
        jp->op = IR_JMP; jp->label = origlbl;
        jp->synth = 1;
    }
    free(tramp);

    free(fn->ins);
    fn->ins = nb.p; fn->nins = nb.n; fn->cap = nb.cap;

    /* The rebuild renumbered every instruction, so the local scope ranges (which
     * are instruction indices, used by codegen to coalesce disjoint-lifetime
     * locals) are now stale. Drop them: codegen then gives each surviving local
     * its own slot — correct, if a touch larger. Promoted locals are dead. */
    if (fn->var_scope_lo) {
        free(fn->var_scope_lo); free(fn->var_scope_hi);
        fn->var_scope_lo = fn->var_scope_hi = NULL;
    }

    for (int p = 0; p < nprom; p++) free(stk[p]);
    free(undef); free(stk); free(sp); free(scap);
    free(dstk); free(entered);
    free(hasphi); free(work); free(prom); free(ploc);
    mem2reg_free(bb, nbb, df, ndf, l2b, order);
    return 1;
}
