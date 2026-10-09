/* ---- idiom recognition: a copy or clear loop -------------------------
 *
 * A loop that walks an array writing a constant, or copying one array
 * to another, is memset and memcpy -- and the library versions are word
 * at a time, or better. Recognising the shape is worth more than any
 * amount of optimizing the loop itself.
 *
 *      for (i = 0; i < N; i++) a[i] = 0;      ->  memzero(a, N * esize)
 *      for (i = 0; i < N; i++) a[i] = b[i];   ->  memcpy(a, b, N * esize)
 *
 * IR_MEMZERO and IR_MEMCPY already exist -- irgen builds them for
 * aggregate assignment -- so this is recognition, not new code
 * generation. They take a byte COUNT that must be known, so this fires
 * only on a constant trip count; a runtime one would need a multiply
 * and a call, which is a different shape.
 *
 * The refusals are the same family as the vectorizer's, for the same
 * reason -- both are rewriting a whole loop into one operation:
 * nothing else may happen in the body, and for a copy the two arrays
 * must not overlap, which here means both bases are distinct globals
 * (memcpy, unlike memmove, may not overlap at all). */

#include "opt_int.h"

static int idiom_one(struct ir_func *fn)
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
    struct defs d;
    compute_defs(fn, &d);
    char *in = xmalloc((size_t)nbb);
    int done = 0;

    for (int oi = norder - 1; oi >= 0 && !done; oi--) {
        int h = order[oi];
        if (h == 0 || bb[h].end <= bb[h].start ||
            fn->ins[bb[h].start].op != IR_LABEL)
            continue;
        int Lh = fn->ins[bb[h].start].label;
        int latch = -1, nback = 0;
        for (int q = 0; q < bb[h].npred; q++) {   /* back edges into h */
            int p = bb[h].pred[q];
            if (bb_dominates(bb, h, p)) {
                latch = p; nback++;
            }
        }
        if (nback != 1 || bb[latch].end <= bb[latch].start)
            continue;
        struct ir_ins *br = &fn->ins[bb[latch].end - 1];
        if (br->op != IR_BRNZ || br->label != Lh)
            continue;
        memset(in, 0, (size_t)nbb);
        loop_body(bb, nbb, h, latch, in);
        struct vecloop L;
        L.lo = bb[h].start; L.hi = bb[latch].end;
        int ok = 1;
        for (int b = 0; b < nbb && ok; b++)
            if (in[b] != (bb[b].start >= L.lo && bb[b].end <= L.hi))
                ok = 0;
        if (!ok)
            continue;

        /* the counted-loop shape, as the vectorizer reads it */
        int cn = (br->a >= 0 && br->a < fn->nvregs && d.cnt[br->a] == 1)
                 ? d.ins[br->a] : -1;
        if (cn < L.lo || cn >= L.hi)
            continue;
        struct ir_ins *cmp = &fn->ins[cn];
        if (cmp->op != IR_CMP || cmp->pred != B_LT || !cmp->sign ||
            !const_b(fn, &d, cmp, &L.bound) || L.bound <= 0)
            continue;
        L.iv = cmp->a;
        L.cmp_ins = cn;
        L.copy_ins = -1;
        for (int n = L.lo; n < L.hi; n++)
            if (fn->ins[n].op == IR_MOV && fn->ins[n].dst == L.iv)
                L.copy_ins = n;
        if (L.copy_ins < 0)
            continue;
        int nxt = fn->ins[L.copy_ins].a;
        if (nxt < 0 || nxt >= fn->nvregs || d.cnt[nxt] != 1)
            continue;
        L.step_ins = d.ins[nxt];
        if (L.step_ins < L.lo || L.step_ins >= L.hi)
            continue;
        long step;
        if (fn->ins[L.step_ins].op != IR_ADD ||
            fn->ins[L.step_ins].a != L.iv ||
            !const_b(fn, &d, &fn->ins[L.step_ins], &step) || step != 1)
            continue;
        /* it must start at zero, or the byte count is not bound*esize */
        int init_zero = 1;
        for (int n = 0; n < fn->nins && init_zero; n++) {
            if (n >= L.lo && n < L.hi) continue;
            struct ir_ins *i = &fn->ins[n];
            if (def_target(i) != L.iv) continue;
            if (!(i->op == IR_MOV && i->a >= 0 && i->a < fn->nvregs &&
                  d.cnt[i->a] == 1 && d.ins[i->a] >= 0 &&
                  fn->ins[d.ins[i->a]].op == IR_CONST &&
                  fn->ins[d.ins[i->a]].imm == 0))
                init_zero = 0;
        }
        if (!init_zero)
            continue;

        /* Exactly one store, at most one load, and nothing else that
         * does anything -- the same rule the vectorizer uses, because
         * the whole body is about to become one operation. */
        int st = -1, ld = -1;
        for (int n = L.lo; n < L.hi && ok; n++) {
            struct ir_ins *i = &fn->ins[n];
            if (n == L.step_ins || n == L.copy_ins || n == L.cmp_ins ||
                n == L.hi - 1 || i->op == IR_LABEL)
                continue;
            if (i->op == IR_STORE) { if (st >= 0) ok = 0; st = n; continue; }
            if (i->op == IR_LOAD)  { if (ld >= 0) ok = 0; ld = n; continue; }
            if (!is_pure(i->op) || i->vol) ok = 0;
        }
        if (!ok || st < 0)
            continue;
        struct ir_ins *store = &fn->ins[st];
        if (store->vol || (store->size != 1 && store->size != 2 &&
                           store->size != 4 && store->size != 8))
            continue;
        L.esize = store->size;
        int dbase = addr_of_iv(fn, &d, &L, store->a, L.esize);
        if (dbase < 0)
            continue;
        int is_copy = ld >= 0;
        int sbase = -1;
        if (is_copy) {
            struct ir_ins *load = &fn->ins[ld];
            if (load->vol || load->size != L.esize || store->b != load->dst)
                continue;
            sbase = addr_of_iv(fn, &d, &L, load->a, L.esize);
            if (sbase < 0 || sbase == dbase)
                continue;
            /* memcpy may not overlap, so both must be objects that
             * cannot: two DISTINCT globals. */
            int db = d.ins[dbase], sb = d.ins[sbase];
            if (db < 0 || sb < 0 || fn->ins[db].op != IR_GADDR ||
                fn->ins[sb].op != IR_GADDR)
                continue;
        } else {
            /* the stored value must be a constant zero: memzero is the
             * only fill this IR has */
            int sv = store->b;
            if (sv < 0 || sv >= fn->nvregs || d.cnt[sv] != 1)
                continue;
            int vn = d.ins[sv];
            if (vn < 0 || fn->ins[vn].op != IR_CONST || fn->ins[vn].imm != 0)
                continue;
        }
        /* nothing the loop computes may be read after it */
        for (int n = L.lo; n < L.hi && ok; n++) {
            int t = def_target(&fn->ins[n]);
            if (t < 0 || t == L.iv) continue;
            for (int m = 0; m < fn->nins && ok; m++)
                if ((m < L.lo || m >= L.hi) && ins_reads(&fn->ins[m], t))
                    ok = 0;
        }
        if (!ok)
            continue;

        /* Replace the whole loop with one operation. */
        struct ibuf nb = { 0, 0, 0 };
        int *newpos = fn->var_scope_lo
            ? xmalloc((size_t)(fn->nins + 1) * sizeof *newpos) : NULL;
        for (int n = 0; n < fn->nins; n++) {
            if (newpos) newpos[n] = nb.n;
            if (n == L.lo) {
                struct ir_ins *m = ib_push(&nb);
                m->op = is_copy ? IR_MEMCPY : IR_MEMZERO;
                m->a = dbase; m->b = is_copy ? sbase : -1;
                m->size = (int)(L.bound * L.esize);
                m->line = fn->ins[st].line; m->col = fn->ins[st].col;
            }
            if (n >= L.lo && n < L.hi)
                continue;
            *ib_push(&nb) = fn->ins[n];
        }
        if (newpos) {
            newpos[fn->nins] = nb.n;
            for (int v = 0; v < fn->nvars; v++) {
                int lo2 = fn->var_scope_lo[v], hi2 = fn->var_scope_hi[v];
                if (lo2 >= 0 && lo2 <= fn->nins) fn->var_scope_lo[v] = newpos[lo2];
                if (hi2 >= 0 && hi2 <= fn->nins) fn->var_scope_hi[v] = newpos[hi2];
            }
            free(newpos);
        }
        free(fn->ins);
        fn->ins = nb.p; fn->nins = nb.n; fn->cap = nb.cap;
        g_did.idiom++;
        if (remarks_on() && fn->src)
            remark_add("opt", is_copy ? "recognized-memcpy" : "recognized-memzero",
                       fn->name, "idiom/loop", fn->file, fn->line,
                       "%ld bytes", L.bound * L.esize);
        done = 1;
    }

    free(in); free_defs(&d); free(order); free(l2b);
    free_cfg(bb, nbb);
    return done;
}

int pass_idiom(struct ir_func *fn)
{
    int changed = 0, guard = 0;
    while (guard++ < 16 && idiom_one(fn))
        changed = 1;
    return changed;
}
