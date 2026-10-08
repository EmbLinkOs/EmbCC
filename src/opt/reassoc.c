/* ==== reassociation ========================================================
 *
 * `(x + 1) + 1` is two adds, and the second waits for the first. `x + 2`
 * is one add that waits for nothing, and it is the same value. Both
 * forms are everywhere: address arithmetic builds them, and unrolling
 * builds four in a row out of one `i++` -- a dependency chain four deep
 * through what ought to be four independent increments.
 *
 * Only a CONSTANT is moved. Reassociating two variables is where such a
 * pass starts costing more than it returns: it changes which values are
 * live across which points, and without a cost model that is a guess.
 * The case here is unambiguous -- an operation with a constant whose
 * other operand is the same kind of operation with a constant -- and it
 * is the one the rest of the pipeline actually produces.
 *
 * The inner operation is NOT required to die. When something else reads
 * it, it stays and the only thing that changes is that THIS instruction
 * no longer waits for it; when nothing does, the marking dead-code pass
 * takes it. Either way the chain is one shorter.
 */

#include "opt_int.h"

static int reassoc_fold(enum ir_op outer, enum ir_op inner, long c1, long c2,
                        int w, enum ir_op *op_out, long *c_out)
{
    unsigned long a = (unsigned long)c1, b = (unsigned long)c2;
    int bits = w * 8;
    switch (outer) {
    case IR_ADD:                                   /* (x ± c1) + c2 */
        if (inner == IR_ADD) { *op_out = IR_ADD; *c_out = norm((long)(a + b), w); return 1; }
        if (inner == IR_SUB) { *op_out = IR_ADD; *c_out = norm((long)(b - a), w); return 1; }
        return 0;
    case IR_SUB:                                   /* (x ± c1) - c2 */
        if (inner == IR_ADD) { *op_out = IR_ADD; *c_out = norm((long)(a - b), w); return 1; }
        if (inner == IR_SUB) { *op_out = IR_SUB; *c_out = norm((long)(a + b), w); return 1; }
        return 0;
    case IR_MUL:
        if (inner == IR_MUL) { *op_out = IR_MUL; *c_out = norm((long)(a * b), w); return 1; }
        return 0;
    case IR_AND:
        if (inner == IR_AND) { *op_out = IR_AND; *c_out = norm((long)(a & b), w); return 1; }
        return 0;
    case IR_OR:
        if (inner == IR_OR)  { *op_out = IR_OR;  *c_out = norm((long)(a | b), w); return 1; }
        return 0;
    case IR_XOR:
        if (inner == IR_XOR) { *op_out = IR_XOR; *c_out = norm((long)(a ^ b), w); return 1; }
        return 0;
    /* Two shifts are one shift of the sum only while the sum still fits:
     * past the width the answer is zero (or all sign bits), which is a
     * different instruction and not this pass's business. */
    case IR_SHL:
        if (inner == IR_SHL && c1 >= 0 && c2 >= 0 && c1 + c2 < bits) {
            *op_out = IR_SHL; *c_out = c1 + c2; return 1;
        }
        return 0;
    case IR_SHR:
        if (inner == IR_SHR && c1 >= 0 && c2 >= 0 && c1 + c2 < bits) {
            *op_out = IR_SHR; *c_out = c1 + c2; return 1;
        }
        return 0;
    default:
        return 0;
    }
}

/* Read an instruction as `x OP c`: the vreg and the constant, in that
 * order. A commutative op takes its constant from either side; SUB,
 * SHL and SHR only from the right, because `c - x` and `c << x` are not
 * this shape at all. */
int as_op_const(struct ir_func *fn, struct defs *d, struct ir_ins *i,
                int *x, long *c)
{
    if (i->flt || i->w == 16 || i->imm_b)
        return 0;
    long A, B;
    int ka = get_const(fn, d, i->a, &A), kb = get_const(fn, d, i->b, &B);
    switch (i->op) {
    case IR_ADD: case IR_MUL: case IR_AND: case IR_OR: case IR_XOR:
        if (kb && !ka) { *x = i->a; *c = B; return 1; }
        if (ka && !kb) { *x = i->b; *c = A; return 1; }
        return 0;
    case IR_SUB: case IR_SHL: case IR_SHR:
        if (kb && !ka) { *x = i->a; *c = B; return 1; }
        return 0;
    default:
        return 0;
    }
}

int pass_reassoc(struct ir_func *fn)
{
    if (fn->nins == 0)
        return 0;
    struct defs d;
    compute_defs(fn, &d);
    /* Decided first, applied second: the rewrite inserts a constant
     * before each instruction it changes, so the indices everything is
     * reasoned about move as soon as one is emitted. */
    enum ir_op *nop = xmalloc((size_t)fn->nins * sizeof *nop);
    int *nx = xmalloc((size_t)fn->nins * sizeof *nx);
    long *nc = xmalloc((size_t)fn->nins * sizeof *nc);
    char *doit = xcalloc((size_t)fn->nins, 1);
    int any = 0;
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        int xo; long c2;
        if (!as_op_const(fn, &d, i, &xo, &c2))
            continue;
        if (xo < 0 || xo >= fn->nvregs || d.cnt[xo] != 1)
            continue;
        int in = d.ins[xo];
        if (in < 0 || in >= fn->nins)
            continue;
        struct ir_ins *inner = &fn->ins[in];
        if (inner->w != i->w)
            continue;
        /* Two shifts of different KINDS are not one shift: an arithmetic
         * shift after a logical one keeps different bits. */
        if ((i->op == IR_SHR || i->op == IR_SHL) && inner->sign != i->sign)
            continue;
        int xi; long c1;
        if (!as_op_const(fn, &d, inner, &xi, &c1))
            continue;
        if (!reassoc_fold(i->op, inner->op, c1, c2, i->w, &nop[n], &nc[n]))
            continue;
        nx[n] = xi;
        doit[n] = 1;
        any = 1;
    }
    if (!any) {
        free(nop); free(nx); free(nc); free(doit); free_defs(&d);
        return 0;
    }

    struct ibuf nb = { 0, 0, 0 };
    int *newpos = fn->var_scope_lo
        ? xmalloc((size_t)(fn->nins + 1) * sizeof *newpos) : NULL;
    for (int n = 0; n < fn->nins; n++) {
        if (newpos) newpos[n] = nb.n;
        if (doit[n]) {
            int k = fn->nvregs++;
            struct ir_ins *c = ib_push(&nb);
            c->op = IR_CONST; c->dst = k; c->imm = nc[n]; c->w = fn->ins[n].w;
            c->line = fn->ins[n].line; c->col = fn->ins[n].col;
            c->synth = fn->ins[n].line ? 0 : 1;
            struct ir_ins *o = ib_push(&nb);
            *o = fn->ins[n];
            o->op = nop[n]; o->a = nx[n]; o->b = k;
            o->imm_b = 0; o->imm = 0;
            continue;
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
    fn->ins = nb.p; fn->nins = nb.n; fn->cap = nb.cap;
    free(nop); free(nx); free(nc); free(doit); free_defs(&d);
    return 1;
}

/* ---- a constant in an index, moved into the address ----
 *
 * `a[i - 1]` is `a + ((i - 1) << 2)`: an add, a shift and another add
 * before the load. In the machine's arithmetic `(i - 1) << 2` IS
 * `(i << 2) - 4`, so the address is `(a + (i << 2)) - 4`, and the -4 is
 * the load's displacement once the backend folds it (ra_fold_memoff): a
 * shift, an add and the load. `a[i - 1]`, `a[i]` and `a[i + 1]` then share
 * one `a + (i << 2)`, which value numbering finds -- an interpreter's
 * `stack[sp - 1] += stack[sp]` and a filter's `x[n - 1]` are this.
 *
 * Every step is modular at one width, so the rewrite holds for every
 * value of i and assumes nothing about overflow. That is also why it
 * needs no extension in the chain: an `int` index on a 64-bit target is
 * sign-extended AFTER the add, and `(long)(i - 1)` is not `(long)i - 1`
 * when `i - 1` wraps, which -fwrapv says it may. So it is the 32-bit
 * targets' `int` index and anyone's `long` one.
 *
 * Only where it pays, which is narrower than where it is true. RISC-V
 * has no indexed addressing, so `a[i - 1]` there is an add, a shift, an
 * add and the load, and becomes three. Thumb, aarch64 and x86-64 scale a
 * register inside the access (`ldr r0, [r1, r2, lsl #2]`), so the old
 * form was already two and the rewrite only moved work around: the
 * workload's interpreter ran 2.7% more instructions on the M4 and its
 * sort 1.0%. So RISC-V only. There the address must be used by loads and
 * stores and nothing else, so the constant does become a displacement;
 * the scaled index must have no other use, so the old shift dies; and so
 * must `i - 1`. When it is wanted anyway -- `prog[pc++]` reads at the
 * new pc -- the rewrite keeps it and adds the shared base besides, and
 * the interpreter ran 1.2% slower than with no rewrite at all. The
 * displacement is kept within 255 bytes either way. */
int pass_idxoff(struct ir_func *fn)
{
    if (fn->nins == 0 || getenv("EMBCC_NO_IDXOFF") ||
        (target_get() != TARGET_RISCV32 && target_get() != TARGET_RISCV64))
        return 0;
    int nv = fn->nvregs;
    struct defs d;
    compute_defs(fn, &d);
    int *use = xcalloc((size_t)nv, sizeof *use);
    int *ause = xcalloc((size_t)nv, sizeof *ause);
    struct ucount uc = { use, nv };
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        each_read(i, count_cb, &uc);
        if ((i->op == IR_LOAD || i->op == IR_STORE) && !i->memoff &&
            i->a >= 0 && i->a < nv && !(i->op == IR_STORE && i->b == i->a))
            ause[i->a]++;
    }
    /* per address: the base, the unscaled index, the shift and the
     * displacement */
    int *rb = xmalloc((size_t)fn->nins * sizeof *rb);
    int *rx = xmalloc((size_t)fn->nins * sizeof *rx);
    long *rk = xmalloc((size_t)fn->nins * sizeof *rk);
    long *rc = xmalloc((size_t)fn->nins * sizeof *rc);
    char *doit = xcalloc((size_t)fn->nins, 1);
    int any = 0;
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        if (i->op != IR_ADD || i->flt || i->imm_b || (i->w != 4 && i->w != 8))
            continue;
        if (i->dst < 0 || i->dst >= nv || d.cnt[i->dst] != 1 ||
            !use[i->dst] || use[i->dst] != ause[i->dst])
            continue;
        for (int side = 0; side < 2 && !doit[n]; side++) {
            int base = side ? i->b : i->a, sv = side ? i->a : i->b;
            if (base < 0 || base >= nv || sv < 0 || sv >= nv ||
                d.cnt[sv] != 1 || use[sv] != 1 || d.ins[sv] < 0)
                continue;
            struct ir_ins *sh = &fn->ins[d.ins[sv]];
            int y; long k;
            if (sh->op != IR_SHL || sh->w != i->w ||
                !as_op_const(fn, &d, sh, &y, &k) || k < 0 || k > 3)
                continue;
            if (y < 0 || y >= nv || d.cnt[y] != 1 || d.ins[y] < 0 ||
                use[y] != 1)
                continue;
            struct ir_ins *ad = &fn->ins[d.ins[y]];
            int x; long c;
            if ((ad->op != IR_ADD && ad->op != IR_SUB) || ad->w != i->w ||
                !as_op_const(fn, &d, ad, &x, &c) || x < 0 || x >= nv)
                continue;
            unsigned long uc2 = (unsigned long)c;
            if (ad->op == IR_SUB) uc2 = -uc2;
            long disp = norm((long)(uc2 << k), i->w);
            if (disp == 0 || disp < -255 || disp > 255)
                continue;
            rb[n] = base; rx[n] = x; rk[n] = k; rc[n] = disp;
            doit[n] = 1;
            any = 1;
        }
    }
    free(use); free(ause);
    if (!any) {
        free(rb); free(rx); free(rk); free(rc); free(doit); free_defs(&d);
        return 0;
    }
    struct ibuf nb = { 0, 0, 0 };
    int *newpos = fn->var_scope_lo
        ? xmalloc((size_t)(fn->nins + 1) * sizeof *newpos) : NULL;
    for (int n = 0; n < fn->nins; n++) {
        if (newpos) newpos[n] = nb.n;
        if (!doit[n]) {
            *ib_push(&nb) = fn->ins[n];
            continue;
        }
        struct ir_ins o = fn->ins[n];
        int w = o.w, kk = fn->nvregs++, t = fn->nvregs++;
        int q = fn->nvregs++, cc = fn->nvregs++;
        struct ir_ins *e = ib_push(&nb);
        memset(e, 0, sizeof *e);
        e->op = IR_CONST; e->dst = kk; e->imm = rk[n]; e->w = w;
        e->a = e->b = -1; e->line = o.line; e->col = o.col; e->synth = 1;
        e = ib_push(&nb);
        memset(e, 0, sizeof *e);
        e->op = IR_SHL; e->dst = t; e->a = rx[n]; e->b = kk; e->w = w;
        e->line = o.line; e->col = o.col; e->synth = 1;
        e = ib_push(&nb);
        memset(e, 0, sizeof *e);
        e->op = IR_ADD; e->dst = q; e->a = rb[n]; e->b = t; e->w = w;
        e->line = o.line; e->col = o.col; e->synth = 1;
        e = ib_push(&nb);
        memset(e, 0, sizeof *e);
        e->op = IR_CONST; e->dst = cc; e->imm = rc[n]; e->w = w;
        e->a = e->b = -1; e->line = o.line; e->col = o.col; e->synth = 1;
        e = ib_push(&nb);
        *e = o;
        e->a = q; e->b = cc; e->imm_b = 0; e->imm = 0;
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
    fn->ins = nb.p; fn->nins = nb.n; fn->cap = nb.cap;
    free(rb); free(rx); free(rk); free(rc); free(doit); free_defs(&d);
    return 1;
}
