/* ==== -Os on ARM: one copy of a block's tail ==================================
 *
 * C repeats itself where the source does: each `case` of a switch that
 * stores through `va_arg(ap, long *)` (scanf's store_int has five), each
 * `return huge * copysign(huge, x)` in fdlibm, every `errno = ERANGE;
 * return -1`. Each was its own copy of the same instructions, where
 * clang's branch folding keeps one and jumps to it.
 *
 * Two blocks that end the same way -- a jump to the same label, a return
 * of the same value, or a fall into the same label -- and whose last
 * instructions are the same computation keep one copy of those
 * instructions: the other jumps into the first, to a label put in front
 * of the shared run. "The same computation" is operand for operand, where
 * a value a run defines and only that run reads (a temporary defined
 * once) corresponds to the other run's, and every other operand and every
 * other definition is the same name. Plain computations, loads, stores
 * and calls with scalar arguments only. The longest such run of at least
 * two instructions goes first, one at a time. Last of the passes, -Os,
 * on every target: written and measured on ARM, it was gated to it, and
 * lib/libc at -Os then lost 144 bytes on RV32, 90 on RV64, 460 on AVR,
 * 60 each on x86-64 and AArch64 with nothing bigger when the gate went. */

#include "opt_int.h"

static int tm_op_ok(const struct ir_ins *i)
{
    switch (i->op) {
    case IR_CONST: case IR_MOV: case IR_ADD: case IR_SUB: case IR_MUL:
    case IR_DIV: case IR_MOD: case IR_AND: case IR_OR: case IR_XOR:
    case IR_SHL: case IR_SHR: case IR_NEG: case IR_BNOT: case IR_CMP:
    case IR_LDVAR: case IR_STVAR: case IR_ADDR: case IR_STRADDR:
    case IR_GADDR: case IR_FADDR: case IR_LOAD: case IR_STORE: case IR_EXT:
    case IR_I2F: case IR_F2I: case IR_F2F: case IR_BITCAST: case IR_SELECT:
        return 1;
    case IR_CALL:
        if (i->indirect || i->retsize || i->sret_first || i->call_cmse ||
            i->nargs > 16)
            return 0;
        for (int k = 0; k < i->nargs; k++)
            if (i->argv[k].is_struct || i->argv[k].byref)
                return 0;
        return 1;
    default:
        return 0;
    }
}

static int tm_same_arg(const struct ir_arg *x, const struct ir_arg *y)
{
    return x->is_struct == y->is_struct && x->size == y->size &&
           x->nclass == y->nclass && x->cls[0] == y->cls[0] &&
           x->cls[1] == y->cls[1] && x->on_stack == y->on_stack &&
           x->stk_off == y->stk_off && x->align == y->align &&
           x->nat_align == y->nat_align && x->natural == y->natural &&
           x->is_float == y->is_float && x->is_int128 == y->is_int128 &&
           x->hfa_n == y->hfa_n && x->hfa_size == y->hfa_size &&
           x->byref == y->byref && x->copy_off == y->copy_off &&
           /* a scalar's placement is in the fields above; only an
            * aggregate's is decided from its type (RISC-V flattening,
            * AAPCS64) -- and `tm->tm_mday` and `tm->tm_hour` are two
            * type objects for one int */
           (x->ty == y->ty || (!x->is_struct && !y->is_struct &&
                               !x->byref && !y->byref));
}

/* Everything but the operands and the destination. */
static int tm_same_fields(const struct ir_ins *a, const struct ir_ins *b)
{
    if (!gj_same_fields(a, b) || a->indirect != b->indirect ||
        a->sret_first != b->sret_first ||
        a->call_varargs != b->call_varargs || a->call_pcs != b->call_pcs ||
        a->call_cmse != b->call_cmse || a->call_nfixed != b->call_nfixed ||
        a->nargs != b->nargs || a->retsize != b->retsize ||
        a->ret_tybytes != b->ret_tybytes || a->ret_tysign != b->ret_tysign ||
        a->ret_ptr != b->ret_ptr || a->ret_hfa_n != b->ret_hfa_n ||
        a->ret_hfa_size != b->ret_hfa_size || a->ret_byref != b->ret_byref ||
        a->rety != b->rety || a->retnclass != b->retnclass)
        return 0;
    for (int k = 0; a->op == IR_CALL && k < a->nargs; k++)
        if (!tm_same_arg(&a->argv[k], &b->argv[k]))
            return 0;
    return 1;
}

/* The operands i reads, in a fixed order, without writing to i (each_read
 * rewrites a call's argument array in place, and copies share it). */
struct tm_rd { int *out, n; };
static void tm_rd_cb(int *p, void *ctx)
{
    struct tm_rd *r = ctx;
    if (r->n < 32)
        r->out[r->n++] = *p;
}
static int tm_reads(const struct ir_ins *i, int *out)
{
    struct tm_rd r = { out, 0 };
    if (i->op == IR_CALL) {
        for (int k = 0; k < i->nargs && r.n < 32; k++)
            out[r.n++] = i->argv[k].vreg;
        return r.n;
    }
    struct ir_ins c = *i;
    each_read(&c, tm_rd_cb, &r);
    return r.n;
}

/* Is v defined once, inside [s, e), and read only inside [s, e] (e: the
 * block's terminator)? Then it is the run's own and may correspond to the
 * other run's. maxr: the last instruction reading each vreg. */
static int tm_local(struct ir_func *fn, const struct defs *d,
                    const int *maxr, int v, int s, int e)
{
    return v >= fn->nvars && v < fn->nvregs && d->cnt[v] == 1 &&
           d->ins[v] >= s && d->ins[v] < e && maxr[v] <= e;
}

/* The operands two runs may read differently: a merge temp set by each
 * run before the shared copy carries the value in. One pair per operand
 * name, at most TM_MAXP of them; w and flt are the value's as the reading
 * instruction sees it, which is what the moves setting the temp copy. */
#define TM_MAXP 4
struct tm_pairs { int xa[TM_MAXP], yb[TM_MAXP], w[TM_MAXP], flt[TM_MAXP], n; };

/* The width and class of operand q of i as i reads it, or 0 where that
 * is not a value a move can carry (a slot's address, a condition of its
 * own width, a conversion's source, a 16-byte argument). */
static int tm_opnd_w(const struct ir_ins *i, int q, int *flt)
{
    *flt = 0;
    switch (i->op) {
    case IR_CALL:
        if (i->argv[q].size > 8 || i->argv[q].is_int128)
            return 0;
        *flt = i->argv[q].is_float;
        return i->argv[q].size;
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
    case IR_AND: case IR_OR: case IR_XOR: case IR_SHL: case IR_SHR:
    case IR_CMP: case IR_MOV: case IR_NEG: case IR_BNOT:
        *flt = i->flt;
        return i->w;
    case IR_LOAD:
        return target_ptr_size();
    case IR_STORE:
        if (q == 0)
            return target_ptr_size();
        *flt = i->flt;
        return i->size;
    default:
        return 0;
    }
}

/* Does any instruction of [s, e) define v? */
static int tm_def_in(const struct ir_func *fn, int v, int s, int e)
{
    for (int n = s; n < e; n++)
        if (def_target(&fn->ins[n]) == v)
            return 1;
    return 0;
}

/* Do the runs [ks, ke) and [ds, de) compute the same, and their block
 * ends (ke, de: a JMP, a RET, or the label fallen into) agree? With pr,
 * an operand the runs read differently (two values from before the run,
 * neither a run's own) is noted there instead of failing the match. */
static int tm_match(struct ir_func *fn, const struct defs *d,
                    const int *maxr, int ks, int ke, int ds, int de,
                    int *map, struct tm_pairs *pr)
{
    int L = ke - ks, ok = 1, nm = 0, mk[64], md[64];
    if (L > 64)
        return 0;
    if (pr)
        pr->n = 0;
    for (int k = 0; k < L && ok; k++) {
        const struct ir_ins *a = &fn->ins[ks + k], *b = &fn->ins[ds + k];
        int ra[32], rb[32], na, nb;
        if (!tm_op_ok(a) || !tm_op_ok(b) || !tm_same_fields(a, b)) {
            ok = 0;
            break;
        }
        na = tm_reads(a, ra);
        nb = tm_reads(b, rb);
        if (na != nb) { ok = 0; break; }
        for (int q = 0; q < na && ok; q++) {
            int x = rb[q], y = ra[q];
            if (x >= 0 && x < fn->nvregs && map[x] >= 0)
                x = map[x];
            if (x == y)
                continue;
            int flt, w = pr ? tm_opnd_w(a, q, &flt) : 0;
            /* a value either run defines is not one from before it,
             * whatever else defines it: the move that would carry it in
             * runs before the run (fuzz seed 13408: `%v = const 0` was in
             * the dropped run, and the move read %v's other definition) */
            if (!w || x < 0 || y < 0 || x >= fn->nvregs || y >= fn->nvregs ||
                x != rb[q] || tm_def_in(fn, y, ks, ke) ||
                tm_def_in(fn, x, ds, de)) {
                ok = 0;
                break;
            }
            int p;
            for (p = 0; p < pr->n; p++)
                if (pr->xa[p] == y || pr->yb[p] == x ||
                    pr->xa[p] == x || pr->yb[p] == y)
                    break;
            if (p < pr->n) {
                if (pr->xa[p] != y || pr->yb[p] != x || pr->w[p] != w ||
                    pr->flt[p] != flt)
                    ok = 0;
            } else if (pr->n == TM_MAXP) {
                ok = 0;
            } else {
                pr->xa[p] = y; pr->yb[p] = x;
                pr->w[p] = w; pr->flt[p] = flt;
                pr->n++;
            }
        }
        int da = def_target(a), db = def_target(b);
        if (!ok)
            break;
        if (da < 0 || db < 0) {
            if (da != db) ok = 0;
            continue;
        }
        int la = tm_local(fn, d, maxr, da, ks, ke);
        int lb = tm_local(fn, d, maxr, db, ds, de);
        if (la && lb && da != db) {
            map[db] = da;
            md[nm] = db;
            mk[nm++] = da;
        } else if (la != lb || da != db) {
            ok = 0;
        }
    }
    if (ok) {
        const struct ir_ins *a = &fn->ins[ke], *b = &fn->ins[de];
        if ((a->op == IR_RET) != (b->op == IR_RET))
            ok = 0;
        else if (a->op != IR_RET)
            ok = a->label == b->label;   /* JMP or the label fallen into */
        else {
            int x = b->a;
            if (x >= 0 && x < fn->nvregs && map[x] >= 0)
                x = map[x];
            ok = x == a->a && a->w == b->w && a->size == b->size &&
                 a->flt == b->flt;
        }
        /* a paired value the shared copy also reads where both runs read
         * it: renaming that read to the merge temp would hand the other
         * run's value to it */
        for (int p = 0; ok && pr && p < pr->n; p++) {
            if (a->op == IR_RET && a->a == pr->xa[p])
                ok = 0;
            for (int k = 0; ok && k < L; k++) {
                int ra[32], rb[32], na = tm_reads(&fn->ins[ks + k], ra);
                tm_reads(&fn->ins[ds + k], rb);
                for (int q = 0; q < na; q++)
                    if (ra[q] == pr->xa[p] && rb[q] == pr->xa[p])
                        ok = 0;
            }
        }
    }
    for (int k = 0; k < nm; k++)
        map[md[k]] = -1;
    (void)mk;
    return ok;
}

/* Code after an unconditional jump or return and before the next label
 * runs never: cfgclean, retargeting a jump to a jump, leaves the jumps
 * there, two bytes each. Only what the merge may leave (plain code and
 * jumps). */
void tm_sweep(struct ir_func *fn)
{
    int dead = 0, k = 0;
    int *newpos = fn->var_scope_lo
        ? xmalloc((size_t)(fn->nins + 1) * sizeof *newpos) : NULL;
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        if (newpos) newpos[n] = k;
        if (i->op == IR_LABEL)
            dead = 0;
        if (dead && (tm_op_ok(i) || i->op == IR_JMP || i->op == IR_BRZ ||
                     i->op == IR_BRNZ || i->op == IR_RET))
            continue;
        fn->ins[k++] = fn->ins[n];
        if (i->op == IR_JMP || i->op == IR_RET || i->op == IR_UD2 ||
            i->op == IR_SWITCH || i->op == IR_IGOTO)
            dead = 1;
    }
    if (newpos) {
        newpos[fn->nins] = k;
        for (int v = 0; v < fn->nvars; v++) {
            int lo = fn->var_scope_lo[v], hi = fn->var_scope_hi[v];
            if (lo >= 0 && lo <= fn->nins) fn->var_scope_lo[v] = newpos[lo];
            if (hi >= 0 && hi <= fn->nins) fn->var_scope_hi[v] = newpos[hi];
        }
        free(newpos);
    }
    fn->nins = k;
}

/* ---- the merge temps ---------------------------------------------------
 *
 * strftime's twenty `case`s each call sb_num(&b, VALUE, 2, '0') and
 * break: the same call, one operand apart. The runs are still shared:
 * each run sets a merge temp to its value before the shared copy, which
 * reads the temp -- a value a run computes for nothing else is defined
 * as the temp outright (no move), any other is moved into it. Where the
 * shared copy already begins at a label, the temp must be one every
 * block reaching that label sets (a merge temp an earlier merge made),
 * since nothing can be put in front of the label for one path alone. */

/* Is v a value the block [bs, s) makes for its run: defined once there
 * and read nowhere after e? Its definition may write the temp directly. */
static int tm_prefix_local(struct ir_func *fn, const struct defs *d,
                           const int *maxr, int v, int bs, int s, int e)
{
    return v >= fn->nvars && v < fn->nvregs && d->cnt[v] == 1 &&
           d->ins[v] >= bs && d->ins[v] < s && maxr[v] <= e;
}

/* Does the block ending at instruction n (the one before a jump or a
 * label) define v? Back to its label or the branch in front of it. */
static int tm_block_defines(const struct ir_func *fn, int n, int v)
{
    for (int m = n - 1; m >= 0; m--) {
        enum ir_op op = fn->ins[m].op;
        if (op == IR_LABEL || op == IR_JMP || op == IR_BRZ || op == IR_BRNZ ||
            op == IR_RET || op == IR_SWITCH || op == IR_IGOTO || op == IR_UD2)
            return 0;
        if (def_target(&fn->ins[m]) == v)
            return 1;
    }
    return 0;
}

struct tm_lab { int lab, hit; };
static void tm_lab_cb(int *p, void *ctx)
{
    struct tm_lab *l = ctx;
    if (*p == l->lab) l->hit = 1;
}

/* Is v a merge temp of the label at ks - 1: a temp read only in [ks, ke]
 * and set by every block that reaches the label, each by its jump or by
 * falling in? Then one more block may set it and jump there. A branch
 * or a switch to the label ends a block elsewhere: no. */
static int tm_merge_temp(struct ir_func *fn, const int *minr, const int *maxr,
                         int v, int ks, int ke)
{
    if (v < fn->nvars || v >= fn->nvregs || minr[v] < ks || maxr[v] > ke)
        return 0;
    int lab = fn->ins[ks - 1].label;
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        if (i->op == IR_JMP) {
            if (i->label == lab && !tm_block_defines(fn, n, v))
                return 0;
            continue;
        }
        struct tm_lab l = { lab, 0 };
        each_label(fn, i, tm_lab_cb, &l);
        if (l.hit)
            return 0;
    }
    if (ks >= 2) {
        enum ir_op op = fn->ins[ks - 2].op;
        if (op != IR_JMP && op != IR_RET && op != IR_SWITCH &&
            op != IR_IGOTO && op != IR_UD2 && !tm_block_defines(fn, ks - 1, v))
            return 0;
    }
    return 1;
}

/* How each pair reaches its temp: xm/ym 0 = the temp is x's value already
 * (an existing merge temp), 1 = the run's definition writes the temp,
 * 2 = a move. cost: the moves. 0 when no plan is possible. */
struct tm_plan { int m[TM_MAXP], xm[TM_MAXP], ym[TM_MAXP]; int cost; };
static int tm_plan(struct ir_func *fn, const struct defs *d,
                   const int *minr, const int *maxr, int bx, int ks, int ke,
                   int by, int ds, int de, const struct tm_pairs *pr,
                   struct tm_plan *pl)
{
    int newlab = !(ks > 0 && fn->ins[ks - 1].op == IR_LABEL);
    pl->cost = 0;
    for (int p = 0; p < pr->n; p++) {
        if (newlab)
            pl->xm[p] = tm_prefix_local(fn, d, maxr, pr->xa[p], bx, ks, ke) ? 1 : 2;
        else if (tm_merge_temp(fn, minr, maxr, pr->xa[p], ks, ke))
            pl->xm[p] = 0;
        else
            return 0;
        pl->ym[p] = tm_prefix_local(fn, d, maxr, pr->yb[p], by, ds, de) ? 1 : 2;
        pl->cost += (pl->xm[p] == 2) + (pl->ym[p] == 2);
    }
    return 1;
}

/* Every read of `from` in [lo, hi) and its definition read and write `to`. */
struct tm_rn { int from, to; };
static void tm_rn_cb(int *p, void *ctx)
{
    struct tm_rn *r = ctx;
    if (*p == r->from) *p = r->to;
}
static void tm_rename(struct ir_func *fn, int from, int to, int lo, int hi)
{
    struct tm_rn r = { from, to };
    for (int n = lo; n < hi; n++) {
        struct ir_ins *i = &fn->ins[n];
        if (ins_reads(i, from)) {
            ins_own_args(i);
            each_read(i, tm_rn_cb, &r);
        }
        if (def_target(i) == from && i->op != IR_STVAR)
            i->dst = to;
    }
}

static void tm_push_mov(struct ibuf *b, const struct tm_pairs *pr, int p,
                        int dst, int src, const struct ir_ins *at)
{
    struct ir_ins *m = ib_push(b);
    ins_blank(m);
    m->op = IR_MOV;
    m->dst = dst;
    m->a = src;
    m->w = pr->w[p];
    m->flt = pr->flt[p];
    m->line = at->line;
    m->col = at->col;
    m->synth = 1;
}

int pass_tailmerge(struct ir_func *fn)
{
    int nv = fn->nvregs, N = fn->nins;
    if (N < 6 || nv == 0)
        return 0;
    /* the last reader of each vreg; an asm's or anything tm_reads does not
     * see counts as reading everything, so nothing it might read looks
     * like a run's own */
    int *maxr = xmalloc((size_t)nv * sizeof *maxr);
    int *minr = xmalloc((size_t)nv * sizeof *minr);   /* ...and the first */
    for (int v = 0; v < nv; v++) {
        maxr[v] = -1;
        minr[v] = N;
    }
    for (int n = 0; n < N; n++) {
        int ops[32], k = tm_reads(&fn->ins[n], ops);
        for (int q = 0; q < k; q++)
            if (ops[q] >= 0 && ops[q] < nv) {
                maxr[ops[q]] = n;
                if (minr[ops[q]] == N)
                    minr[ops[q]] = n;
            }
        if (fn->ins[n].op == IR_ASM || fn->ins[n].op == IR_LANDING ||
            (fn->ins[n].op == IR_CALL && fn->ins[n].nargs > 32))
            for (int v = 0; v < nv; v++) {
                maxr[v] = N;
                minr[v] = -1;
            }
    }
    struct defs d;
    compute_defs(fn, &d);
    int *map = xmalloc((size_t)nv * sizeof *map);
    for (int v = 0; v < nv; v++)
        map[v] = -1;
    /* the blocks: [start, end), each after a label or a branch, ending
     * at a JMP or a RET or where it falls into a label; one that ends in
     * a conditional branch, a switch or a trap, or holds an asm, is not a
     * candidate */
    int *bs = xmalloc((size_t)N * sizeof *bs), *be = xmalloc((size_t)N * sizeof *be);
    int nb = 0;
    for (int n = 0; n < N; ) {
        int s0 = n, m, ok = 1;
        if (fn->ins[s0].op == IR_LABEL)
            s0++;
        for (m = s0; m < N; m++) {
            enum ir_op op = fn->ins[m].op;
            if (op == IR_LABEL || op == IR_JMP || op == IR_RET)
                break;
            if (op == IR_BRZ || op == IR_BRNZ || op == IR_SWITCH ||
                op == IR_IGOTO || op == IR_UD2) {
                ok = 0;
                break;
            }
            if (op == IR_ASM || op == IR_LANDING)
                ok = 0;
        }
        if (m >= N)
            break;
        if (ok && m > s0) {
            bs[nb] = s0;
            be[nb] = m;
            nb++;
        }
        n = fn->ins[m].op == IR_LABEL ? m : m + 1;
        if (n <= s0 - 1)
            n = s0;
    }
    /* Inside a loop, only where the path through the copy takes no more
     * branches than before: the dropped run ended in a jump, and the copy
     * falls into the same label. Otherwise that path is a jump longer on
     * every trip that takes it -- tools/bench's state machine, a switch
     * in a loop, ran 2.5% more cycles at -Os for it. */
    int *labpos = xmalloc((size_t)(fn->nlabels ? fn->nlabels : 1) * sizeof *labpos);
    for (int l = 0; l < fn->nlabels; l++)
        labpos[l] = -1;
    for (int n = 0; n < N; n++)
        if (fn->ins[n].op == IR_LABEL && fn->ins[n].label >= 0 &&
            fn->ins[n].label < fn->nlabels)
            labpos[fn->ins[n].label] = n;
    char *inloop = xcalloc((size_t)(nb ? nb : 1), 1);
    for (int x = 0; x < nb; x++)
        inloop[x] = (char)gj_in_loop(fn, bs[x], labpos);
    free(labpos);
    /* The best pair: the most instructions dropped (a call counts its
     * arguments, each one set up again) less the moves that bring the
     * differing operands in, at least two. The merged run keeps x's copy:
     * x before y, so the jump goes back to a copy already placed. */
    int best = 1, bestL = 0, bk = -1, bd = -1;
    struct tm_pairs pr, bpr = { {0}, {0}, {0}, {0}, 0 };
    struct tm_plan plan, bplan = { {0}, {0}, {0}, 0 };
    for (int x = 0; x < nb; x++)
        for (int y = 0; y < nb; y++) {
            if (x == y)
                continue;
            /* y's path through the copy takes a jump more each trip: in
             * a loop, only for a call's worth of instructions dropped */
            int tight = inloop[y] && (fn->ins[be[x]].op != IR_LABEL ||
                                      fn->ins[be[y]].op != IR_JMP);
            int floor = tight ? 4 : best;
            int kx = be[x], dy = be[y];
            const struct ir_ins *ex = &fn->ins[kx], *ey = &fn->ins[dy];
            /* (tm_match decides whether the two ends agree) */
            if ((ex->op == IR_RET) != (ey->op == IR_RET))
                continue;
            int L = 0, weight = 0;
            while (L < be[x] - bs[x] && L < be[y] - bs[y] && L < 64 &&
                   fn->ins[kx - L - 1].op == fn->ins[dy - L - 1].op &&
                   tm_op_ok(&fn->ins[kx - L - 1])) {
                const struct ir_ins *i = &fn->ins[kx - L - 1];
                weight += i->op == IR_CALL ? 1 + i->nargs : 1;
                L++;
            }
            for (int k = L; k > 0 && weight > best && weight >= floor; k--) {
                const struct ir_ins *i = &fn->ins[kx - k];
                if (tm_match(fn, &d, maxr, kx - k, kx, dy - k, dy, map, &pr) &&
                    tm_plan(fn, &d, minr, maxr, bs[x], kx - k, kx,
                            bs[y], dy - k, dy, &pr, &plan) &&
                    weight - plan.cost > best && weight - plan.cost >= floor) {
                    best = weight - plan.cost; bestL = k; bk = x; bd = y;
                    bpr = pr; bplan = plan;
                }
                weight -= i->op == IR_CALL ? 1 + i->nargs : 1;
            }
        }
    int done = 0;
    if (bk >= 0) {
        int ks = be[bk] - bestL, ds = be[bd] - bestL, de = be[bd];
        int Lk = -1, newlab = 0;
        if (ks > 0 && fn->ins[ks - 1].op == IR_LABEL)
            Lk = fn->ins[ks - 1].label;
        else {
            Lk = fn->nlabels++;
            newlab = 1;
        }
        /* the merge temps: a run's own value is defined as the temp
         * outright, any other is moved into it -- before the label on
         * x's side, before the jump on y's */
        for (int p = 0; p < bpr.n; p++) {
            int m = bplan.xm[p] == 0 ? bpr.xa[p] : fn->nvregs++;
            bplan.m[p] = m;
            if (bplan.xm[p] == 1)
                tm_rename(fn, bpr.xa[p], m, 0, fn->nins);
            else if (bplan.xm[p] == 2)
                tm_rename(fn, bpr.xa[p], m, ks, be[bk]);
            if (bplan.ym[p] == 1)
                tm_rename(fn, bpr.yb[p], m, 0, fn->nins);
        }
        struct ibuf nbuf = { 0, 0, 0 };
        int *newpos = fn->var_scope_lo
            ? xmalloc((size_t)(fn->nins + 1) * sizeof *newpos) : NULL;
        for (int n = 0; n < fn->nins; n++) {
            if (newpos) newpos[n] = nbuf.n;
            if (n == ks && newlab) {
                for (int p = 0; p < bpr.n; p++)
                    if (bplan.xm[p] == 2)
                        tm_push_mov(&nbuf, &bpr, p, bplan.m[p], bpr.xa[p],
                                    &fn->ins[n]);
                struct ir_ins *l = ib_push(&nbuf);
                l->op = IR_LABEL;
                l->label = Lk;
                l->line = fn->ins[n].line;
                l->col = fn->ins[n].col;
                l->synth = 1;
            }
            if (n >= ds && n <= de) {
                /* the run and its jump or return: one jump; a label it
                 * fell into stays */
                if (n == ds) {
                    for (int p = 0; p < bpr.n; p++)
                        if (bplan.ym[p] == 2)
                            tm_push_mov(&nbuf, &bpr, p, bplan.m[p],
                                        bpr.yb[p], &fn->ins[n]);
                    struct ir_ins *j = ib_push(&nbuf);
                    j->op = IR_JMP;
                    j->label = Lk;
                    j->line = fn->ins[n].line;
                    j->col = fn->ins[n].col;
                    j->synth = 1;
                }
                if (fn->ins[n].op != IR_LABEL)
                    continue;
            }
            *ib_push(&nbuf) = fn->ins[n];
        }
        if (newpos) {
            newpos[fn->nins] = nbuf.n;
            for (int v = 0; v < fn->nvars; v++) {
                int lo = fn->var_scope_lo[v], hi = fn->var_scope_hi[v];
                if (lo >= 0 && lo <= fn->nins) fn->var_scope_lo[v] = newpos[lo];
                if (hi >= 0 && hi <= fn->nins) fn->var_scope_hi[v] = newpos[hi];
            }
            free(newpos);
        }
        free(fn->ins);
        fn->ins = nbuf.p;
        fn->nins = nbuf.n;
        fn->cap = nbuf.cap;
        done = 1;
        if (remarks_on() && fn->src)
            remark_add("opt", "tail-merged", fn->name, "tail-merge",
                       fn->file, fn->line,
                       "%d instructions shared with a block end alike but for %d operands",
                       bestL, bpr.n);
    }
    free(bs); free(be);
    free(inloop);
    free(map);
    free(maxr);
    free(minr);
    free_defs(&d);
    return done;
}
