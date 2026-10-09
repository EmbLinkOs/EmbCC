/* ==== an update next to the copy that ends it ============================
 *
 * `t[n++] = c` computes n + 1 before the store reads n: phi destruction
 * leaves `n1 = n + 1; ...; t[n] = c; ...; n = mov n1`, and n and n1 are
 * both live from the add to the store. They cannot share a register, so
 * the latch keeps its copy -- a `mov` every trip of the loop, the same in
 * `*p++ = x` and in `b[m++] = t[--n]`. Clang has none.
 *
 * So `d = x OP c`, whose first reader is the copy `x = mov d` later in
 * the same block, moves down to sit right before that copy. Then d is
 * born where x dies, the two are one register, and the copy is a move
 * to itself. Nothing in between may write x or read d; the operation is
 * pure and reads only x and a constant, so moving it changes no value,
 * and x lives until the new position, where d no longer does -- the
 * same pressure, one register for one. */

#include "opt_int.h"

static int sinkupd_op(const struct ir_ins *i)
{
    if (i->flt || i->vol || i->w == 16)
        return 0;
    switch (i->op) {
    case IR_ADD: case IR_SUB: case IR_AND: case IR_OR: case IR_XOR:
    case IR_SHL: case IR_SHR:
        return 1;
    default:
        return 0;
    }
}
int pass_sinkupd(struct ir_func *fn)
{
    int N = fn->nins, nv = fn->nvregs;
    if (N < 3 || nv == 0 || getenv("EMBCC_NO_SINKUPD"))
        return 0;
    struct defs d;
    compute_defs(fn, &d);
    int *to = xmalloc((size_t)N * sizeof *to);    /* move n to before to[n] */
    char *dest = xcalloc((size_t)N, 1);
    int changed = 0;
    for (int n = 0; n < N; n++)
        to[n] = -1;
    for (int n = 0; n < N; n++) {
        const struct ir_ins *i = &fn->ins[n];
        if (!sinkupd_op(i))
            continue;
        int t = i->dst, x = i->a;
        long c;
        if (t < fn->nvars || t >= nv || d.cnt[t] != 1 || x < 0 || x >= nv ||
            x == t || !const_b(fn, &d, (struct ir_ins *)i, &c))
            continue;
        /* the first reader of t, in this block */
        int u = -1;
        for (int m = n + 1; m < N && u < 0; m++) {
            const struct ir_ins *q = &fn->ins[m];
            if (q->op == IR_LABEL)
                break;
            if (ins_reads((struct ir_ins *)q, t)) {
                u = m;
                break;
            }
            if (q->op == IR_JMP || q->op == IR_BRZ || q->op == IR_BRNZ ||
                q->op == IR_RET || q->op == IR_UD2 || q->op == IR_IGOTO ||
                q->op == IR_SWITCH)
                break;
        }
        if (u < 0 || u == n + 1 || dest[u] || to[u] >= 0)
            continue;
        const struct ir_ins *cp = &fn->ins[u];
        if (cp->op != IR_MOV || cp->a != t || cp->dst != x || cp->vol)
            continue;
        /* nothing between writes x, the constant, or anything it reads */
        int ok = 1;
        for (int m = n + 1; m < u && ok; m++) {
            const struct ir_ins *q = &fn->ins[m];
            int w = q->op == IR_STVAR ? q->dst : def_target(q);
            if (q->op == IR_ASM || q->op == IR_LANDING || w == x ||
                (!i->imm_b && w == i->b) || to[m] >= 0 || dest[m])
                ok = 0;
        }
        if (!ok)
            continue;
        to[n] = u;
        dest[u] = 1;
        changed = 1;
    }
    if (changed) {
        struct ibuf nb = { 0, 0, 0 };
        int *newpos = fn->var_scope_lo
            ? xmalloc((size_t)(N + 1) * sizeof *newpos) : NULL;
        int *at = xmalloc((size_t)N * sizeof *at);  /* who goes before m */
        for (int m = 0; m < N; m++)
            at[m] = -1;
        for (int n = 0; n < N; n++)
            if (to[n] >= 0)
                at[to[n]] = n;
        for (int n = 0; n < N; n++) {
            if (newpos) newpos[n] = nb.n;
            if (at[n] >= 0)
                *ib_push(&nb) = fn->ins[at[n]];
            if (to[n] < 0)
                *ib_push(&nb) = fn->ins[n];
        }
        if (newpos) newpos[N] = nb.n;
        if (newpos) {
            remap_scopes(fn, newpos, N);
            free(newpos);
        }
        free(at);
        free(fn->ins);
        fn->ins = nb.p; fn->nins = nb.n; fn->cap = nb.cap;
    }
    free(to); free(dest); free_defs(&d);
    return changed;
}

/* ==== an address next to its access ======================================
 *
 * Every backend fuses an address computation into the access it feeds --
 * x86-64's `[base + idx*4]`, Thumb's `ldr r, [rn, rm, lsl #2]`, aarch64's
 * `[xn, xm, lsl #2]`, and the offset fold of every target -- and every
 * one of those looks at the instructions immediately before the access.
 * An interpreter's `stack[sp++] = prog[pc++]` computes the store's
 * address, then loads the value, then stores: the address is two
 * instructions away and is materialised whole, a shift and an add the
 * store did not need.
 *
 * So a single-use chain of address arithmetic -- add, shift or multiply
 * by a constant, widening -- defined earlier in the access's own block is
 * moved down to sit right before it, deepest operand first. Nothing about
 * the values changes: the chain is pure, its result is read only by the
 * access, and it moves only when no instruction it passes writes any of
 * its operands. */
static int sinkaddr_op(const struct ir_ins *i)
{
    if (i->flt || i->vol)
        return 0;
    switch (i->op) {
    case IR_ADD: case IR_SUB: case IR_EXT:
        return 1;
    case IR_SHL: case IR_MUL:
        return i->imm_b;              /* by a constant only */
    default:
        return 0;
    }
}
struct sa_rd { const int *wr; int lo, hi, bad, nv; };
int pass_sinkaddr(struct ir_func *fn)
{
    int N = fn->nins, nv = fn->nvregs;
    /* Not on AVR: its pointer registers are three pairs, and an address
     * formed early and held is often what keeps one of them from being
     * reloaded -- lib/libc grew by 92 bytes when they were moved. */
    if (N < 3 || nv == 0 || target_get() == TARGET_AVR)
        return 0;
    int *use = xcalloc((size_t)nv, sizeof *use);
    struct ucount uc = { use, nv };
    for (int n = 0; n < N; n++)
        each_read(&fn->ins[n], count_cb, &uc);
    struct defs d;
    compute_defs(fn, &d);
    /* block start of each instruction: the index of the last label or
     * terminator before it, so "same block" is one comparison */
    int *bstart = xmalloc((size_t)N * sizeof *bstart);
    for (int n = 0, b = 0; n < N; n++) {
        enum ir_op op = fn->ins[n].op;
        if (op == IR_LABEL) b = n;
        bstart[n] = b;
        if (op == IR_JMP || op == IR_BRZ || op == IR_BRNZ || op == IR_RET ||
            op == IR_UD2 || op == IR_IGOTO || op == IR_SWITCH)
            b = n + 1;
    }
    char *moved = xcalloc((size_t)N, 1);
    int *before = xmalloc((size_t)N * 4 * sizeof *before);  /* up to 4 per access */
    int *nbefore = xcalloc((size_t)N, sizeof *nbefore);
    int changed = 0;
    for (int n = 0; n < N; n++) {
        const struct ir_ins *acc = &fn->ins[n];
        if (acc->op != IR_LOAD && acc->op != IR_STORE)
            continue;
        /* the chain, root first: the address, then its operands */
        int chain[4], nc = 0, frontier[4], nf = 0;
        frontier[nf++] = acc->a;
        while (nf && nc < 4) {
            int v = frontier[--nf];
            if (v < fn->nvars || v >= nv || d.cnt[v] != 1 || use[v] != 1)
                continue;
            int dn = d.ins[v];
            if (dn < 0 || dn >= n || moved[dn] || bstart[dn] != bstart[n])
                continue;
            if (!sinkaddr_op(&fn->ins[dn]))
                continue;
            chain[nc++] = dn;
            const struct ir_ins *c = &fn->ins[dn];
            if (c->a >= 0 && nf < 4) frontier[nf++] = c->a;
            if (!c->imm_b && c->b >= 0 && c->op != IR_EXT && nf < 4) frontier[nf++] = c->b;
        }
        if (nc == 0)
            continue;
        /* already in place? the chain occupies the slots right before n */
        int lowest = n;
        for (int k = 0; k < nc; k++) if (chain[k] < lowest) lowest = chain[k];
        if (lowest == n - nc) {
            int contiguous = 1;
            for (int m = n - nc; m < n; m++) {
                int in = 0;
                for (int k = 0; k < nc; k++) if (chain[k] == m) in = 1;
                if (!in) contiguous = 0;
            }
            if (contiguous)
                continue;
        }
        /* nothing between a chain member and n may write its operands,
         * or another member's (they read each other, which is fine: the
         * order is kept) */
        int ok = 1;
        for (int k = 0; k < nc && ok; k++) {
            const struct ir_ins *c = &fn->ins[chain[k]];
            int ops[2] = { c->a, (!c->imm_b && c->op != IR_EXT) ? c->b : -1 };
            for (int m = chain[k] + 1; m < n && ok; m++) {
                int t = fn->ins[m].op == IR_STVAR ? fn->ins[m].dst : def_target(&fn->ins[m]);
                if (fn->ins[m].op == IR_ASM || fn->ins[m].op == IR_LANDING)
                    ok = 0;
                for (int q = 0; q < 2; q++)
                    if (ops[q] >= 0 && t == ops[q]) ok = 0;
            }
        }
        if (!ok)
            continue;
        /* emit in original order (each member's operands come before it) */
        for (int x = 0; x < nc; x++)
            for (int y = x + 1; y < nc; y++)
                if (chain[y] < chain[x]) { int t = chain[x]; chain[x] = chain[y]; chain[y] = t; }
        for (int k = 0; k < nc; k++) {
            moved[chain[k]] = 1;
            before[n * 4 + nbefore[n]++] = chain[k];
        }
        changed = 1;
    }
    if (changed) {
        struct ibuf nb = { 0, 0, 0 };
        int *newpos = fn->var_scope_lo
            ? xmalloc((size_t)(N + 1) * sizeof *newpos) : NULL;
        for (int n = 0; n < N; n++) {
            if (newpos) newpos[n] = nb.n;
            for (int k = 0; k < nbefore[n]; k++)
                *ib_push(&nb) = fn->ins[before[n * 4 + k]];
            if (!moved[n])
                *ib_push(&nb) = fn->ins[n];
        }
        if (newpos) newpos[N] = nb.n;   /* a scope ending at the end */
        if (newpos) {
            remap_scopes(fn, newpos, N);
            free(newpos);
        }
        free(fn->ins);
        fn->ins = nb.p; fn->nins = nb.n; fn->cap = nb.cap;
    }
    free(use); free(bstart); free(moved); free(before); free(nbefore);
    free_defs(&d);
    return changed;
}
