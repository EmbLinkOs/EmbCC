/* ==== two-sided range checks ==============================================
 *
 * `c >= '0' && c <= '9'` is two compares and two branches, and so is
 * `c < 'a' || c > 'z'`. Both ask one question -- is c inside [lo, hi] --
 * and (unsigned)(c - lo) <= hi - lo answers it in one compare, signed or
 * unsigned, because the subtraction wraps every value outside the range
 * past hi - lo. It is what every parser, every ctype test and every
 * bounds check against constants is made of, and CoreMark's state
 * machine spent a fifth of its instructions on the second compare.
 *
 * The shape, as irgen and the folding passes leave it:
 *
 *      %a = cmp P1 X, K1          single use: the branch
 *      br1 %a -> T1               falls into the next block...
 *      [consts]                   ...which nothing else jumps into
 *      %b = cmp P2 X, K2          single use: the branch
 *      br2 %b -> T2               falls through to F
 *
 * Each branch LEAVES (to its target) under a condition on X; call them
 * out1 and out2. When T1 == T2, the pair falls through exactly when
 * neither leaves; when T1 is F, it reaches T2 exactly when the first
 * stays and the second leaves. Either way one side is a conjunction of
 * two bounds on X, and when that conjunction is an interval [lo, hi] the
 * pair becomes `%d = sub X, lo; %b = cmp ult %d, hi - lo + 1` and one
 * branch. Equality and inequality are not bounds and are left alone. */

#include "opt_int.h"

/* The condition `X pred K` (or its negation) as an interval [lo, hi] at
 * width w; 0 when it is not one bound. Signed or unsigned per `sign`. */
static int bound_of(enum binop pred, int sign, long k, int w, int negate,
                    long *lo, long *hi)
{
    long min = sign ? (w == 8 ? (long)(-0x7fffffffffffffffL - 1) : (long)-0x80000000L) : 0;
    long max = sign ? (w == 8 ? 0x7fffffffffffffffL : 0x7fffffffL)
                    : (w == 8 ? -1L : 0xffffffffL);
    if (negate)
        switch (pred) {
        case B_LT: pred = B_GE; break;
        case B_LE: pred = B_GT; break;
        case B_GT: pred = B_LE; break;
        case B_GE: pred = B_LT; break;
        default: return 0;
        }
    /* compare k against the domain the way the compare itself will */
    unsigned long uk = w == 8 ? (unsigned long)k : (unsigned long)(unsigned)k;
    if (!sign) {
        k = (long)uk;
    } else if (w == 4) {
        k = (long)(int)k;
    }
    switch (pred) {
    case B_GE: *lo = k; *hi = max; return 1;
    case B_GT:
        if (sign ? k == max : (unsigned long)k == (unsigned long)max) return 0;
        *lo = k + 1; *hi = max; return 1;
    case B_LE: *lo = min; *hi = k; return 1;
    case B_LT:
        if (sign ? k == min : k == 0) return 0;
        *lo = min; *hi = k - 1; return 1;
    default:
        return 0;
    }
}

struct lblcount { int *cnt; int n; };
static void lblcount_cb(int *p, void *ctx)
{
    struct lblcount *c = ctx;
    if (*p >= 0 && *p < c->n) c->cnt[*p]++;
}

/* A `const` instruction for the range check, located where `at` is. */
static void rc_const(struct ir_ins *slot, int dst, long v, int w, const struct ir_ins *at)
{
    memset(slot, 0, sizeof *slot);
    slot->op = IR_CONST; slot->dst = dst; slot->a = slot->b = -1;
    slot->imm = v; slot->w = w; slot->sign = 0;
    slot->line = at->line; slot->col = at->col; slot->synth = at->synth;
}

int pass_rangecheck(struct ir_func *fn)
{
    /* Not on AVR: a value of four bytes there is four registers, and a
     * subtract and an unsigned compare across all four cost more than the
     * two signed compares they replace, which can usually stop at the
     * high byte -- lib/libc grew by 198 bytes. */
    if (fn->nins < 4 || target_get() == TARGET_AVR)
        return 0;
    /* how many instructions name each label, and where each is placed:
     * the value form below needs a block only the first branch enters */
    int *lref = xcalloc((size_t)(fn->nlabels ? fn->nlabels : 1), sizeof *lref);
    int *lat = xmalloc((size_t)(fn->nlabels ? fn->nlabels : 1) * sizeof *lat);
    for (int l = 0; l < fn->nlabels; l++) lat[l] = -1;
    {
        struct lblcount lc = { lref, fn->nlabels };
        for (int n = 0; n < fn->nins; n++) {
            if (fn->ins[n].op == IR_LABEL) {
                if (fn->ins[n].label >= 0 && fn->ins[n].label < fn->nlabels)
                    lat[fn->ins[n].label] = n;
                continue;
            }
            each_label(fn, &fn->ins[n], lblcount_cb, &lc);
        }
        for (int e = 0; e < fn->neh; e++)
            if (fn->eh[e].lp_label >= 0 && fn->eh[e].lp_label < fn->nlabels)
                lref[fn->eh[e].lp_label]++;
    }
    struct defs d;
    compute_defs(fn, &d);
    int *use = xcalloc((size_t)(fn->nvregs ? fn->nvregs : 1), sizeof *use);
    struct ucount uc = { use, fn->nvregs };
    for (int n = 0; n < fn->nins; n++)
        each_read(&fn->ins[n], count_cb, &uc);
    int changed = 0;
    char *del = xcalloc((size_t)fn->nins, 1);
    /* Constants go in as `const` temps, never as immediates: whether a
     * value fits an instruction is the target's question (pass_immfold
     * asks it later), and x86-64 truncated a 64-bit bound to 32 bits when
     * one was written straight into the compare. pre[n] is emitted just
     * before instruction n. */
    struct ir_ins *pre = xcalloc((size_t)fn->nins, sizeof *pre);
    char *haspre = xcalloc((size_t)fn->nins, 1);
    for (int n1 = 1; n1 < fn->nins; n1++) {
        struct ir_ins *br1 = &fn->ins[n1];
        if (br1->op != IR_BRZ && br1->op != IR_BRNZ)
            continue;
        struct ir_ins *a = &fn->ins[n1 - 1];
        if (a->op != IR_CMP || a->dst != br1->a || a->flt || use[a->dst] != 1 ||
            (a->w != 4 && a->w != 8))
            continue;
        long k1;
        if (!const_b(fn, &d, a, &k1))
            continue;
        /* ---- the || VALUE form: `return x < lo || x > hi;` ----
         *
         *      %a = cmp ...; brz %a -> L2
         *      %r = const 1; jmp L1
         *    L2: [consts] %b = cmp ...; %r = mov %b; (L1: | jmp L1)
         *
         * r is a || b = !(!a && !b): out of one interval. The first
         * branch becomes a jump to L2, whose compare becomes the test;
         * the `r = 1` block is then unreachable and cfgclean drops it. */
        if (br1->op == IR_BRZ && n1 + 2 < fn->nins &&
            fn->ins[n1 + 1].op == IR_CONST && fn->ins[n1 + 1].imm == 1 &&
            !fn->ins[n1 + 1].flt && fn->ins[n1 + 2].op == IR_JMP &&
            br1->label >= 0 && br1->label < fn->nlabels &&
            lref[br1->label] == 1 && lat[br1->label] > n1 + 2) {
            int r = fn->ins[n1 + 1].dst, L1 = fn->ins[n1 + 2].label;
            int m = lat[br1->label] + 1;
            while (m < fn->nins && fn->ins[m].op == IR_CONST && !fn->ins[m].flt)
                m++;
            struct ir_ins *b2 = m + 1 < fn->nins ? &fn->ins[m] : NULL;
            long k2;
            if (b2 && b2->op == IR_CMP && !b2->flt && b2->a == a->a &&
                b2->w == a->w && b2->sign == a->sign && use[b2->dst] == 1 &&
                fn->ins[m + 1].op == IR_MOV && fn->ins[m + 1].a == b2->dst &&
                fn->ins[m + 1].dst == r && m + 2 < fn->nins &&
                ((fn->ins[m + 2].op == IR_LABEL && fn->ins[m + 2].label == L1) ||
                 (fn->ins[m + 2].op == IR_JMP && fn->ins[m + 2].label == L1)) &&
                const_b(fn, &d, b2, &k2)) {
                int w = a->w, sign = a->sign;
                long lo1, hi1, lo2, hi2, lo, hi;
                /* inside = !a && !b */
                if (bound_of(a->pred, sign, k1, w, 1, &lo1, &hi1) &&
                    bound_of(b2->pred, sign, k2, w, 1, &lo2, &hi2)) {
                    int ok = 1;
                    if (sign) {
                        lo = lo1 > lo2 ? lo1 : lo2; hi = hi1 < hi2 ? hi1 : hi2;
                        if (lo > hi) ok = 0;
                    } else {
                        lo = (unsigned long)lo1 > (unsigned long)lo2 ? lo1 : lo2;
                        hi = (unsigned long)hi1 < (unsigned long)hi2 ? hi1 : hi2;
                        if ((unsigned long)lo > (unsigned long)hi) ok = 0;
                    }
                    unsigned long span = ok ? (unsigned long)hi - (unsigned long)lo : 0;
                    if (w == 4) span &= 0xffffffffUL;
                    if (ok && !((w == 4 && span >= 0xffffffffUL) || (w == 8 && span == ~0UL))) {
                        int dv = fn->nvregs++, klo = fn->nvregs++, ksp = fn->nvregs++;
                        struct ir_ins sub;
                        memset(&sub, 0, sizeof sub);
                        sub.op = IR_SUB; sub.dst = dv; sub.a = a->a; sub.b = klo;
                        sub.w = w; sub.sign = 0;
                        sub.line = b2->line; sub.col = b2->col; sub.synth = b2->synth;
                        rc_const(&pre[n1 - 1], klo, w == 4 ? (long)(int)lo : lo, w, &sub);
                        haspre[n1 - 1] = 1;
                        *a = sub;
                        int L2 = br1->label;
                        memset(br1, 0, sizeof *br1);
                        br1->op = IR_JMP; br1->label = L2; br1->dst = br1->a = br1->b = -1;
                        br1->line = sub.line; br1->col = sub.col; br1->synth = 1;
                        struct ir_ins cmpn = *b2;
                        cmpn.a = dv; cmpn.b = ksp; cmpn.imm_b = 0; cmpn.imm = 0;
                        cmpn.pred = B_GE; cmpn.sign = 0;      /* r is `outside` */
                        rc_const(&pre[m], ksp, w == 4 ? (long)(int)(unsigned)(span + 1)
                                                      : (long)(span + 1), w, &cmpn);
                        haspre[m] = 1;
                        fn->ins[m] = cmpn;
                        changed = 1;
                        continue;
                    }
                }
            }
        }
        /* the second block: consts, then the compare and its branch */
        int n2 = n1 + 1;
        while (n2 < fn->nins && fn->ins[n2].op == IR_CONST && !fn->ins[n2].flt)
            n2++;
        if (n2 + 1 >= fn->nins)
            continue;
        struct ir_ins *b = &fn->ins[n2], *br2 = &fn->ins[n2 + 1];
        if (b->op != IR_CMP || b->flt || use[b->dst] != 1 ||
            b->a != a->a || b->w != a->w || b->sign != a->sign || a->a < 0)
            continue;
        /* ---- the VALUE form: `return c >= lo && c <= hi;` ----
         *
         *      %a = cmp ...; br1 %a -> L0
         *      [consts] %b = cmp ...; %r = mov %b; jmp L1
         *    L0: %r = const v0; (jmp L1 | L1:)
         *
         * with L0 entered only from br1. r is (stay1 && b) when v0 is 0
         * and !(stay1 && !b) when it is 1; either way a test of X against
         * one interval, and L0 is left with nothing reaching it. */
        if (br2->op == IR_MOV && br2->a == b->dst && !br2->flt &&
            n2 + 2 < fn->nins && fn->ins[n2 + 2].op == IR_JMP &&
            br1->label >= 0 && br1->label < fn->nlabels &&
            lref[br1->label] == 1 && lat[br1->label] >= 0 &&
            lat[br1->label] + 2 < fn->nins) {
            int L1 = fn->ins[n2 + 2].label, z = lat[br1->label];
            const struct ir_ins *cz = &fn->ins[z + 1], *after = &fn->ins[z + 2];
            long k2;
            if (cz->op == IR_CONST && cz->dst == br2->dst && !cz->flt &&
                cz->imm == 0 && br1->op == IR_BRZ &&
                ((after->op == IR_JMP && after->label == L1) ||
                 (after->op == IR_LABEL && after->label == L1)) &&
                const_b(fn, &d, b, &k2)) {
                int w = a->w, sign = a->sign, v0 = (int)cz->imm;
                long lo1, hi1, lo2, hi2;
                if (bound_of(a->pred, sign, k1, w, br1->op == IR_BRNZ, &lo1, &hi1) &&
                    bound_of(b->pred, sign, k2, w, v0 == 1, &lo2, &hi2)) {
                    long lo, hi;
                    int ok = 1;
                    if (sign) {
                        lo = lo1 > lo2 ? lo1 : lo2; hi = hi1 < hi2 ? hi1 : hi2;
                        if (lo > hi) ok = 0;
                    } else {
                        lo = (unsigned long)lo1 > (unsigned long)lo2 ? lo1 : lo2;
                        hi = (unsigned long)hi1 < (unsigned long)hi2 ? hi1 : hi2;
                        if ((unsigned long)lo > (unsigned long)hi) ok = 0;
                    }
                    unsigned long span = ok ? (unsigned long)hi - (unsigned long)lo : 0;
                    if (w == 4) span &= 0xffffffffUL;
                    if (ok && !((w == 4 && span >= 0xffffffffUL) || (w == 8 && span == ~0UL))) {
                        int dv = fn->nvregs++, klo = fn->nvregs++, ksp = fn->nvregs++;
                        struct ir_ins sub;
                        memset(&sub, 0, sizeof sub);
                        sub.op = IR_SUB; sub.dst = dv; sub.a = a->a; sub.b = klo;
                        sub.w = w; sub.sign = 0;
                        sub.line = b->line; sub.col = b->col; sub.synth = b->synth;
                        struct ir_ins cmpn = *b;
                        cmpn.a = dv; cmpn.b = ksp; cmpn.imm_b = 0; cmpn.imm = 0;
                        cmpn.pred = B_LT; cmpn.sign = 0;   /* v0 is 0: r is `inside` */
                        rc_const(&pre[n1 - 1], klo, w == 4 ? (long)(int)lo : lo, w, &sub);
                        haspre[n1 - 1] = 1;
                        *a = sub;
                        del[n1] = 1;
                        /* the span's const goes where the first branch was */
                        rc_const(&fn->ins[n1], ksp, w == 4 ? (long)(int)(unsigned)(span + 1)
                                                           : (long)(span + 1), w, &cmpn);
                        del[n1] = 0;
                        fn->ins[n2] = cmpn;
                        changed = 1;
                        n1 = n2 + 2;
                        continue;
                    }
                }
            }
        }
        if ((br2->op != IR_BRZ && br2->op != IR_BRNZ) || br2->a != b->dst)
            continue;
        long k2;
        if (!const_b(fn, &d, b, &k2))
            continue;
        /* the fall-through of br2: the label right after it, if any */
        int F = n2 + 2 < fn->nins && fn->ins[n2 + 2].op == IR_LABEL
                ? fn->ins[n2 + 2].label : -1;
        int w = a->w, sign = a->sign;
        long lo1, hi1, lo2, hi2;
        int rewrite = 0, op2 = 0;        /* op2: the new branch's opcode */
        if (br1->label == br2->label) {
            /* falls through iff neither leaves: stay1 && stay2 */
            if (!bound_of(a->pred, sign, k1, w, br1->op == IR_BRNZ, &lo1, &hi1) ||
                !bound_of(b->pred, sign, k2, w, br2->op == IR_BRNZ, &lo2, &hi2))
                continue;
            rewrite = 1; op2 = IR_BRZ;   /* leave when NOT inside */
        } else if (F >= 0 && br1->label == F) {
            /* reaches T2 iff stay1 && leave2 */
            if (!bound_of(a->pred, sign, k1, w, br1->op == IR_BRNZ, &lo1, &hi1) ||
                !bound_of(b->pred, sign, k2, w, br2->op == IR_BRZ, &lo2, &hi2))
                continue;
            rewrite = 1; op2 = IR_BRNZ;  /* leave when inside */
        }
        if (!rewrite)
            continue;
        long lo, hi;
        if (sign) {
            lo = lo1 > lo2 ? lo1 : lo2;
            hi = hi1 < hi2 ? hi1 : hi2;
            if (lo > hi) continue;
        } else {
            lo = (unsigned long)lo1 > (unsigned long)lo2 ? lo1 : lo2;
            hi = (unsigned long)hi1 < (unsigned long)hi2 ? hi1 : hi2;
            if ((unsigned long)lo > (unsigned long)hi) continue;
        }
        unsigned long span = (unsigned long)hi - (unsigned long)lo;
        if (w == 4) span &= 0xffffffffUL;
        /* both bounds must matter, or nothing is gained -- and a span
         * covering the whole width has no `+ 1` */
        if ((w == 4 && span >= 0xffffffffUL) || (w == 8 && span == ~0UL))
            continue;
        /* rewrite: the first compare and branch go; the second becomes
         * the interval test */
        int X = a->a, line = b->line, col = b->col;
        int dv = fn->nvregs++, klo = fn->nvregs++, ksp = fn->nvregs++;
        struct ir_ins sub;
        memset(&sub, 0, sizeof sub);
        sub.op = IR_SUB; sub.dst = dv; sub.a = X; sub.b = klo;
        sub.w = w; sub.sign = 0;
        sub.line = line; sub.col = col; sub.synth = b->synth;
        struct ir_ins cmpn = *b;
        cmpn.a = dv; cmpn.b = ksp; cmpn.imm_b = 0; cmpn.imm = 0;
        cmpn.pred = B_LT; cmpn.sign = 0;
        struct ir_ins brn = *br2;
        brn.op = op2;
        rc_const(&pre[n1 - 1], klo, w == 4 ? (long)(int)lo : lo, w, &sub);
        haspre[n1 - 1] = 1;
        /* the first compare's slot takes the subtraction (it read X, so
         * X is defined there); the first branch goes; the second pair
         * becomes the interval test. The consts between are left for dce. */
        *a = sub;
        /* the span's const takes the first branch's slot */
        rc_const(&fn->ins[n1], ksp, w == 4 ? (long)(int)(unsigned)(span + 1)
                                           : (long)(span + 1), w, &cmpn);
        fn->ins[n2] = cmpn;
        fn->ins[n2 + 1] = brn;
        changed = 1;
        n1 = n2 + 1;
    }
    if (changed) {
        struct ibuf nb = { 0, 0, 0 };
        int *newpos = fn->var_scope_lo
            ? xmalloc((size_t)(fn->nins + 1) * sizeof *newpos) : NULL;
        for (int n = 0; n < fn->nins; n++) {
            if (newpos) newpos[n] = nb.n;
            if (haspre[n])
                *ib_push(&nb) = pre[n];
            if (!del[n])
                *ib_push(&nb) = fn->ins[n];
        }
        /* the end position too: a scope may close at the last
         * instruction, and remap_scopes reads newpos[nins] for it */
        if (newpos) newpos[fn->nins] = nb.n;
        if (newpos) {
            remap_scopes(fn, newpos, fn->nins);
            free(newpos);
        }
        free(fn->ins);
        fn->ins = nb.p; fn->nins = nb.n; fn->cap = nb.cap;
    }
    free(del); free(pre); free(haspre);
    free(use);
    free(lref); free(lat);
    free_defs(&d);
    return changed;
}
