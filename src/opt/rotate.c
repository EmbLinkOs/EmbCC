/* ==== loop rotation (-O2) =================================================== *
 *
 * irgen lowers every loop top-tested, which costs two branches an
 * iteration: the test at the top, and an unconditional jump at the
 * bottom to get back to it.
 *
 *      L0:  t = i < n
 *           brz t -> Lexit
 *           <body>
 *           i = i + 1
 *           jmp L0
 *      Lexit:
 *
 * Rotation copies the test to the bottom and lets the original stand as
 * a guard, so the steady state is one conditional branch that is taken
 * every iteration but the last:
 *
 *           t = i < n              <- runs once
 *           brz t -> Lexit
 *      Lbody:
 *           <body>
 *           i = i + 1
 *           t' = i < n             <- the copy
 *           brnz t' -> Lbody
 *      Lexit:
 *
 * The win is not only the branch. The header and the body were two
 * blocks and become one, so every block-local pass -- value numbering,
 * the copy propagation above -- now sees the whole body at once, which
 * is why this runs before them in the round rather than after.
 *
 * What makes it safe is that the test is DUPLICATED, not moved: the
 * guard still decides whether the body runs at all, so a loop that
 * executes zero times still executes zero times. The copy needs fresh
 * temps, and the values it computes must not be read anywhere but the
 * header -- otherwise the body would read the guard's copy on every
 * iteration and see a stale test. */

#include "opt_int.h"

static int rotate_one(struct ir_func *fn)
{
    if (fn->nins == 0)
        return 0;
    int nbb, *l2b;
    struct bb *bb = build_cfg(fn, &nbb, &l2b);
    int *order = xmalloc((size_t)nbb * sizeof *order), norder;
    compute_rpo(bb, nbb, order, &norder);
    if (norder != nbb) {
        free(order); free(l2b);
        free_cfg(bb, nbb); return 0;
    }
    compute_idom(bb, order, norder);

    int done = 0;
    for (int oi = norder - 1; oi >= 0 && !done; oi--) {
        int h = order[oi];
        if (h == 0 || h + 1 >= nbb)
            continue;
        if (bb[h].end - bb[h].start < 2 ||
            fn->ins[bb[h].start].op != IR_LABEL)
            continue;
        int Lh = fn->ins[bb[h].start].label;
        struct ir_ins *br = &fn->ins[bb[h].end - 1];
        if (br->op != IR_BRZ && br->op != IR_BRNZ)
            continue;

        /* The taken edge must leave the loop and the fall-through stay
         * in it. The other way round, the loop would be entered by the
         * branch and the rotated latch would fall through to the wrong
         * block -- a case that needs a jump to fix and so is not worth
         * taking. */
        int Lt = br->label, texit = l2b[Lt], body = h + 1;
        if (texit < 0 || texit == body)
            continue;

        /* Exactly one back edge, from a latch that reaches the header by
         * an unconditional jump -- which is the shape irgen emits, and
         * the only one where the jump can simply become the new test. */
        int latch = -1, nback = 0;
        for (int q = 0; q < bb[h].npred; q++) {   /* back edges into h */
            int p = bb[h].pred[q];
            if (bb_dominates(bb, h, p)) {
                latch = p; nback++;
            }
        }
        if (nback != 1 || latch == h || bb[latch].end <= bb[latch].start)
            continue;
        if (fn->ins[bb[latch].end - 1].op != IR_JMP ||
            fn->ins[bb[latch].end - 1].label != Lh)
            continue;

        /* The loop, so "leaves the loop" and "used outside the header"
         * are answerable. */
        char *in = xcalloc((size_t)nbb, 1);
        loop_body(bb, nbb, h, latch, in);
        if (!in[body] || in[texit]) { free(in); continue; }

        /* Every instruction between the label and the branch is copied,
         * so each must define a temp read only here -- and must be one
         * that may appear twice.
         *
         * A LOAD may. It is not `is_pure` (it can fault, so nothing may
         * SPECULATE one), but rotation speculates nothing: the header's
         * computation runs once in the guard and once per latch, which
         * for N iterations is the N+1 times the header itself ran, in
         * the same order and at the same points. Refusing it refused
         * `while (*p) p++;` -- every string walk, every list walk --
         * which then paid two branches an iteration for the life of the
         * program. Volatile is still refused: the ACCESS is the effect
         * there, and moving one is not a thing to do on this argument. */
        /* A value the header computes and the body (or the code after
         * the loop) also reads -- `while (*s) h ^= *s++;` once value
         * numbering has given the body the header's load -- is written
         * by the copy under its OWN name: the body is entered from the
         * guard or from the copy, and either way that name holds this
         * iteration's value; so does every path to the exit. It is
         * assigned twice then, which is the price; refusing it cost
         * every such loop a jump an iteration. One read only in the
         * header gets a fresh temp, so the guard's stands untouched. */
        int ok = 1;
        int ncopy = bb[h].end - 1 - (bb[h].start + 1);
        char *outside = xcalloc((size_t)(ncopy ? ncopy : 1), 1);
        for (int n = bb[h].start + 1; n < bb[h].end - 1 && ok; n++) {
            struct ir_ins *i = &fn->ins[n];
            /* A STORE may appear twice too, by the argument the load
             * makes: it runs once in the guard and once per latch, the
             * N+1 times the header ran, in the same order against the
             * body. `while ((d[n] = s[n]) != 0) n++;` -- strcpy, and
             * the hash table's key copy -- has its test after the
             * store, and stayed a jump and an index re-extended every
             * iteration. It defines nothing, so nothing is renamed. Not
             * at -Os: the guard is a second copy of the store, and the
             * jump it saves is the same two bytes. */
            if (i->op == IR_STORE && !i->vol && !g_opt_size &&
                !getenv("EMBCC_NO_ROTSTORE"))
                continue;
            if (!(is_pure(i->op) || i->op == IR_LOAD) || i->vol)
                { ok = 0; break; }
            int t = def_target(i);
            if (t < fn->nvars || t >= fn->nvregs) { ok = 0; break; }
            for (int m = 0; m < fn->nins; m++) {
                if (m >= bb[h].start && m < bb[h].end)
                    continue;
                if (ins_reads(&fn->ins[m], t)) {
                    outside[n - (bb[h].start + 1)] = 1;
                    break;
                }
            }
        }
        if (!ok) { free(in); free(outside); continue; }

        /* A value with no operands -- a constant, an address -- is the
         * same on every iteration: the copy reads the guard's, which
         * dominates the latch, rather than writing it again. Written
         * twice it would stop being a known constant, and folding,
         * LICM and unrolling all ask for exactly one definition (the
         * loop bound `j < 24` shared by value numbering with `i * 24`
         * left the workload's matrix checksum un-unrolled). */
        int *map = xmalloc((size_t)(ncopy ? ncopy : 1) * sizeof *map);
        for (int k = 0; k < ncopy; k++) {
            enum ir_op op = fn->ins[bb[h].start + 1 + k].op;
            int fixed = op == IR_CONST || op == IR_GADDR || op == IR_ADDR ||
                        op == IR_STRADDR || op == IR_FADDR;
            map[k] = op == IR_STORE ? -2     /* copied, and names nothing */
                   : fixed ? -1
                   : outside[k] ? def_target(&fn->ins[bb[h].start + 1 + k])
                   : fn->nvregs++;
        }
        free(outside);
        int Lbody = fn->nlabels++;

        struct ibuf nb = { 0, 0, 0 };
        int *newpos = fn->var_scope_lo
            ? xmalloc((size_t)(fn->nins + 1) * sizeof *newpos) : NULL;
        for (int n = 0; n < fn->nins; n++) {
            if (n == bb[body].start) {       /* the body gets a name */
                struct ir_ins *l = ib_push(&nb);
                l->op = IR_LABEL;
                l->label = Lbody;
                l->line = fn->ins[n].line;
                l->col = fn->ins[n].col;
                l->synth = 1;
            }
            if (newpos) newpos[n] = nb.n;
            if (n == bb[latch].end - 1) {    /* the jump becomes the test */
                /* Old temp -> the copy's temp, for every value the
                 * header computes. A read inside the copy has to follow
                 * the copy rather than the guard; a read of anything
                 * else is left as it is. */
                int *tbl = xmalloc((size_t)fn->nvregs * sizeof *tbl);
                for (int v = 0; v < fn->nvregs; v++)
                    tbl[v] = -1;
                for (int q = 0; q < ncopy; q++) {
                    int old = def_target(&fn->ins[bb[h].start + 1 + q]);
                    if (old >= 0 && old < fn->nvregs && map[q] >= 0 &&
                        map[q] != old)
                        tbl[old] = map[q];
                }
                for (int k = 0; k < ncopy; k++) {
                    if (map[k] == -1)
                        continue;            /* the guard's value stands */
                    struct ir_ins *c = ib_push(&nb);
                    *c = fn->ins[bb[h].start + 1 + k];
                    ins_own_args(c);
                    struct lcopy lc = { tbl, fn->nvregs, 0 };
                    each_read(c, lcopy_cb, &lc);
                    if (map[k] >= 0)
                        c->dst = map[k];
                }
                struct ir_ins *nbr = ib_push(&nb);
                *nbr = *br;
                nbr->op = br->op == IR_BRZ ? IR_BRNZ : IR_BRZ;
                nbr->label = Lbody;
                { struct lcopy lc = { tbl, fn->nvregs, 0 };
                  each_read(nbr, lcopy_cb, &lc); }
                free(tbl);
                /* The exit used to be reached by falling out of the
                 * header; now it is reached by falling out of the latch,
                 * which only lands there if the blocks are adjacent. */
                if (bb[texit].start != bb[latch].end) {
                    struct ir_ins *j = ib_push(&nb);
                    j->op = IR_JMP;
                    j->label = Lt;
                    j->line = br->line;
                    j->col = br->col;
                    j->synth = 1;
                }
                continue;                    /* the old jmp is gone */
            }
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
        free(map); free(in);
        if (remarks_on() && fn->src)
            remark_add("opt", "rotated", fn->name, "licm/bottom-tested-loop",
                       fn->file, fn->line,
                       "one branch an iteration instead of two");
        done = 1;
    }

    free(order); free(l2b);
    free_cfg(bb, nbb);
    return done;
}

int pass_rotate(struct ir_func *fn)
{
    int changed = 0, guard = 0;
    while (guard++ < 64 && rotate_one(fn))
        changed = 1;
    return changed;
}
