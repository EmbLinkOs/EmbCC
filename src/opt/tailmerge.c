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
           x->ty == y->ty;
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

/* Do the runs [ks, ke) and [ds, de) compute the same, and their block
 * ends (ke, de: a JMP, a RET, or the label fallen into) agree? */
static int tm_match(struct ir_func *fn, const struct defs *d,
                    const int *maxr, int ks, int ke, int ds, int de,
                    int *map)
{
    int L = ke - ks, ok = 1, nm = 0, mk[64], md[64];
    if (L > 64)
        return 0;
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
            int x = rb[q];
            if (x >= 0 && x < fn->nvregs && map[x] >= 0)
                x = map[x];
            if (x != ra[q])
                ok = 0;
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

int pass_tailmerge(struct ir_func *fn)
{
    int nv = fn->nvregs, N = fn->nins;
    if (N < 6 || nv == 0)
        return 0;
    /* the last reader of each vreg; an asm's or anything tm_reads does not
     * see counts as reading everything, so nothing it might read looks
     * like a run's own */
    int *maxr = xmalloc((size_t)nv * sizeof *maxr);
    for (int v = 0; v < nv; v++)
        maxr[v] = -1;
    for (int n = 0; n < N; n++) {
        int ops[32], k = tm_reads(&fn->ins[n], ops);
        for (int q = 0; q < k; q++)
            if (ops[q] >= 0 && ops[q] < nv)
                maxr[ops[q]] = n;
        if (fn->ins[n].op == IR_ASM || fn->ins[n].op == IR_LANDING ||
            (fn->ins[n].op == IR_CALL && fn->ins[n].nargs > 32))
            for (int v = 0; v < nv; v++)
                maxr[v] = N;
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
    int bestL = 1, bk = -1, bd = -1;
    for (int x = 0; x < nb; x++)
        for (int y = 0; y < nb; y++) {
            if (x == y || (inloop[y] && (fn->ins[be[x]].op != IR_LABEL ||
                                         fn->ins[be[y]].op != IR_JMP)))
                continue;
            int kx = be[x], dy = be[y];
            const struct ir_ins *ex = &fn->ins[kx], *ey = &fn->ins[dy];
            /* (tm_match decides whether the two ends agree) */
            if ((ex->op == IR_RET) != (ey->op == IR_RET))
                continue;
            int L = 0;
            while (L < be[x] - bs[x] && L < be[y] - bs[y] && L < 64 &&
                   fn->ins[kx - L - 1].op == fn->ins[dy - L - 1].op &&
                   tm_op_ok(&fn->ins[kx - L - 1]))
                L++;
            /* y keeps x's copy: x before y, so the jump goes back to a
             * copy that is already placed; the longer run, x's first */
            for (int k = L; k > bestL; k--)
                if (tm_match(fn, &d, maxr, kx - k, kx, dy - k, dy, map)) {
                    bestL = k; bk = x; bd = y;
                    break;
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
        struct ibuf nbuf = { 0, 0, 0 };
        int *newpos = fn->var_scope_lo
            ? xmalloc((size_t)(fn->nins + 1) * sizeof *newpos) : NULL;
        for (int n = 0; n < fn->nins; n++) {
            if (newpos) newpos[n] = nbuf.n;
            if (n == ks && newlab) {
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
                       "%d instructions shared with an identical block end",
                       bestL);
    }
    free(bs); free(be);
    free(inloop);
    free(map);
    free(maxr);
    free_defs(&d);
    return done;
}
