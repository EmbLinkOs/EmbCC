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

/* ==== a switch whose index cannot be out of range ==========================
 *
 * A state machine's switch tests its index against the table on every
 * trip -- `cmp r5, #4; bhs default` -- although the state is only ever
 * one of its cases: every assignment to it is a case constant. When
 * every definition of the index, followed through copies and selects, is
 * a constant inside the table, the switch is marked (`sign`, printed
 * `switch.4s`) and the backends that read the mark leave the bound out.
 * The index is often `st - lo`; then st's constants are asked for
 * [lo, lo + n). A value with a definition that is no instruction (a
 * parameter) or one this cannot read (inline asm writes outside
 * def_target's view) is not known, and the switch keeps its test.
 * EMBCC_NO_SWITCHRANGE=1 turns it off. */
struct sr_walk { int *mark, gen, budget; };

static int sr_in(struct ir_func *fn, struct defs *d, int v, long lo, long hi,
                 struct sr_walk *s)
{
    if (v < 0 || v >= fn->nvregs || v < fn->nparams)
        return 0;
    if (s->mark[v] == s->gen)
        return 1;                       /* on the way already: a cycle */
    s->mark[v] = s->gen;
    int listed = 0;
    for (int n = d->first[v]; n >= 0; n = d->next[n]) {
        if (--s->budget < 0)
            return 0;
        listed++;
        const struct ir_ins *i = &fn->ins[n];
        switch (i->op) {
        case IR_CONST: {
            long k = norm(i->imm, i->w);
            if (i->flt || k < lo || k > hi)
                return 0;
            break;
        }
        case IR_MOV:
            if (!sr_in(fn, d, i->a, lo, hi, s))
                return 0;
            break;
        case IR_SELECT:
            if (i->flt || !sr_in(fn, d, i->b, lo, hi, s) ||
                !sr_in(fn, d, i->c, lo, hi, s))
                return 0;
            break;
        default:
            return 0;
        }
    }
    return listed > 0 && listed == d->cnt[v];
}

int pass_switchrange(struct ir_func *fn)
{
    if (getenv("EMBCC_NO_SWITCHRANGE"))
        return 0;
    int nsw = 0, asm_ = 0;
    for (int n = 0; n < fn->nins; n++) {
        asm_ |= fn->ins[n].op == IR_ASM;
        if (fn->ins[n].op == IR_SWITCH) {
            fn->ins[n].sign = 0;        /* this pass's answer, made anew */
            nsw++;
        }
    }
    if (asm_)
        return 0;
    if (!nsw)
        return 0;
    struct defs d;
    compute_defs(fn, &d);
    defs_lists(fn, &d);
    struct sr_walk s = { xcalloc((size_t)fn->nvregs + 1, sizeof *s.mark), 0, 0 };
    int changed = 0;
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        if (i->op != IR_SWITCH || i->a < 0)
            continue;
        long cnt = fn->jt[i->jt].n, lo = 0, k;
        int v = i->a;
        /* `st - lo`: st's own values, shifted */
        if (v < fn->nvregs && d.cnt[v] == 1 && d.ins[v] >= 0) {
            const struct ir_ins *x = &fn->ins[d.ins[v]];
            if ((x->op == IR_SUB || x->op == IR_ADD) && !x->flt &&
                x->w == i->w &&
                (x->imm_b ? (k = x->imm, 1) : get_const(fn, &d, x->b, &k))) {
                lo = x->op == IR_SUB ? k : -k;
                v = x->a;
            }
        }
        s.gen++;
        s.budget = 256;
        if (sr_in(fn, &d, v, lo, lo + cnt - 1, &s)) {
            i->sign = 1;
            changed = 1;
        }
    }
    free(s.mark);
    free_defs(&d);
    return changed;
}

/* ==== an int index widened without a wrap it cannot have (-O2) ============
 *
 * On a 64-bit target `x[i - k]` with an int i is `ext(i - k)`: the 32-bit
 * subtraction first, then the sign extension, then the scale -- and none
 * of it is shared with `x[i]` or `x[i - 1]`, because `ext(i - k)` is
 * `ext(i) - k` only when `i - k` does not wrap. clang and GCC take that
 * from signed overflow being undefined. EmbCC promises -fwrapv, so it has
 * to be shown instead, and for a counted loop it can be:
 *
 *   - i is assigned outside the loop only constants, lo..hi0;
 *   - inside it once, `i += s` with 0 < s, in the latch block;
 *   - the latch goes back while `i < B` (signed), B a constant or, for
 *     s == 1, anything (i + 1 <= B <= INT_MAX);
 *
 * so every value i has in the loop lies in [lo, max(hi0, Bmax - 1) + s],
 * and the increment never wraps. Then `i + c`, for a constant c that
 * keeps both ends inside int, cannot wrap either, and `ext(i + c)` is
 * rewritten `ext(i) + c` -- the extension one value numbering shares, the
 * constant one the index-offset fold puts into the load. A FIR's sixteen
 * taps go from sub, ext, shift, add, load each to a load at an offset.
 * EMBCC_NO_IVEXT turns it off. */
/* The constant operand b of i -- an immediate, or a temp only a CONST
 * defines -- in *c; 0 when it is not one. */
static int ivx_const_b(struct ir_func *fn, const struct defs *d,
                       const struct ir_ins *i, long long *c)
{
    if (i->imm_b) {
        *c = (long long)(int)i->imm;
        return 1;
    }
    int b = i->b;
    for (int hop = 0; hop < 8; hop++) {         /* through single copies */
        if (b < 0 || b >= fn->nvregs || d->cnt[b] != 1 || d->ins[b] < 0)
            return 0;
        const struct ir_ins *k = &fn->ins[d->ins[b]];
        if (k->op == IR_CONST && k->w <= 4) {
            *c = (long long)(int)k->imm;
            return 1;
        }
        if (k->op != IR_MOV)
            return 0;
        b = k->a;
    }
    return 0;
}

/* The object rule (pass_ivext): `ext(x + c)` that only ever indexes a
 * declared object -- `prog[pc + 1]` -- is `ext(x) + c` too. Were x + c to
 * wrap, the index would be at least 2^31 - |c| elements from the object's
 * start, and forming that pointer is undefined whatever -fwrapv says
 * about the add: a program that runs as written never does it. So every
 * use of the extension must be `base + ext` or `base + (ext << k)` (k <= 3)
 * with base the address of a global or local of at most 1 GiB. The
 * interpreter's three operand reads then share one base. */
static int ivx_objbase(struct ir_func *fn, const struct defs *d, int b)
{
    if (b < 0 || b >= fn->nvregs || d->cnt[b] != 1 || d->ins[b] < 0)
        return 0;
    const struct ir_ins *i = &fn->ins[d->ins[b]];
    long sz = -1;
    if (i->op == IR_GADDR && i->glob && i->glob->ty &&
        (i->glob->ty->kind == TY_ARRAY || i->glob->ty->complete))
        sz = ty_size(i->glob->ty);      /* 0 for an array of unknown size */
    else if (i->op == IR_ADDR && i->a >= 0 && i->a < fn->nvars)
        sz = fn->locals[i->a].size;
    return sz > 0 && sz <= (1L << 30);
}

static int ivx_objects(struct ir_func *fn, const struct defs *d,
                       const int *uses, const int *user, char *eadd,
                       long long *ec)
{
    int nv = fn->nvregs, changed = 0;
    /* bad[v]: some read of v is not an object index (for a shift: of
     * its result) */
    char *bad = xcalloc((size_t)(nv ? nv : 1), 1);
    char *shl = xcalloc((size_t)(nv ? nv : 1), 1);
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        long long k;
        if (i->op == IR_SHL && i->w == 8 && i->a >= 0 && i->a < nv &&
            i->dst >= 0 && i->dst < nv && d->cnt[i->dst] == 1 &&
            ivx_const_b(fn, (struct defs *)d, i, &k) && k >= 0 && k <= 3)
            shl[i->dst] = 1;
    }
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        struct opnds o;
        value_opnds(i, &o);
        for (int q = 0; q < o.n; q++) {
            int v = o.v[q];
            if (v < 0 || v >= nv)
                continue;
            int ok = 0;
            if (i->op == IR_ADD && i->w == 8 && !i->flt && !i->imm_b &&
                ((i->a == v && ivx_objbase(fn, d, i->b)) ||
                 (i->b == v && ivx_objbase(fn, d, i->a))))
                ok = 1;
            else if (i->op == IR_SHL && i->a == v && i->dst >= 0 &&
                     i->dst < nv && shl[i->dst])
                ok = 2;                 /* if the shift's uses are */
            if (!ok)
                bad[v] = 1;
        }
    }
    /* a shift is good when its own uses are */
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        if (i->op == IR_SHL && i->dst >= 0 && i->dst < nv && shl[i->dst] &&
            bad[i->dst] && i->a >= 0 && i->a < nv)
            bad[i->a] = 1;
    }
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *e = &fn->ins[n];
        if (e->op != IR_EXT || e->size != 4 || !e->sign || e->w != 8 ||
            e->dst < 0 || e->dst >= nv || bad[e->dst] || !uses[e->dst] ||
            e->a < 0 || e->a >= nv || d->cnt[e->a] != 1 || d->ins[e->a] < 0 ||
            uses[e->a] != 1 || user[e->a] != n)
            continue;
        struct ir_ins *x = &fn->ins[d->ins[e->a]];
        long long c;
        if ((x->op != IR_ADD && x->op != IR_SUB) || x->w != 4 || x->flt ||
            x->a < 0 || x->a >= nv || !ivx_const_b(fn, (struct defs *)d, x, &c))
            continue;
        if (x->op == IR_SUB) c = -c;
        if (c == 0 || c < -(1 << 20) || c > (1 << 20))
            continue;
        x->op = IR_EXT; x->size = 4; x->sign = 1; x->w = 8;
        x->imm_b = 0; x->imm = 0; x->b = -1;
        eadd[n] = 1;
        ec[n] = c;
        changed = 1;
    }
    free(bad); free(shl);
    return changed;
}

int pass_ivext(struct ir_func *fn)
{
    if (target_ptr_size() != 8 || fn->nins == 0 || getenv("EMBCC_NO_IVEXT"))
        return 0;
    for (int n = 0; n < fn->nins; n++)
        if (fn->ins[n].op == IR_ASM || fn->ins[n].op == IR_LANDING)
            return 0;
    int nbb, *l2b, changed = 0;
    struct bb *bb = build_cfg(fn, &nbb, &l2b);
    int *order = xmalloc((size_t)nbb * sizeof *order), norder;
    compute_rpo(bb, nbb, order, &norder);
    /* unreachable blocks leave dominance partial: the loop rule needs it,
     * the object rule does not */
    int cfg_whole = norder == nbb;
    if (cfg_whole)
        compute_idom(bb, order, norder);
    struct defs d;
    compute_defs(fn, &d);
    int nv = fn->nvregs ? fn->nvregs : 1;
    int *uses = xcalloc((size_t)nv, sizeof *uses);
    int *user = xmalloc((size_t)nv * sizeof *user);
    for (int n = 0; n < fn->nins; n++) {
        struct opnds o;
        value_opnds(&fn->ins[n], &o);
        if (o.over) {
            free(uses); free(user); free(order); free(l2b); free_defs(&d);
            free_cfg(bb, nbb);
            return 0;
        }
        for (int k = 0; k < o.n; k++)
            if (o.v[k] >= 0 && o.v[k] < fn->nvregs) {
                uses[o.v[k]]++;
                user[o.v[k]] = n;
            }
    }
    char *in = xcalloc((size_t)nbb, 1);
    char *eadd = xcalloc((size_t)fn->nins, 1);
    long long *ec = xmalloc((size_t)fn->nins * sizeof *ec);
    int *stk = xmalloc((size_t)nbb * sizeof *stk);
    int *i2b = xmalloc((size_t)fn->nins * sizeof *i2b);
    for (int b = 0; b < nbb; b++)
        for (int n = bb[b].start; n < bb[b].end; n++)
            i2b[n] = b;
    changed |= ivx_objects(fn, &d, uses, user, eadd, ec);
    for (int p = 0; p < nbb && cfg_whole; p++) {
        if (bb[p].end <= bb[p].start)
            continue;
        struct ir_ins *br = &fn->ins[bb[p].end - 1];
        if (br->op != IR_BRNZ || br->a < 0 || br->a >= fn->nvregs ||
            br->label < 0)
            continue;
        int h = l2b[br->label];
        if (h < 0 || !bb_dominates(bb, h, p))
            continue;
        /* the only way back into h is this branch */
        int nback = 0;
        for (int q = 0; q < bb[h].npred; q++)
            if (bb_dominates(bb, h, bb[h].pred[q]))
                nback++;
        if (nback != 1 || d.cnt[br->a] != 1 || d.ins[br->a] < bb[p].start)
            continue;
        struct ir_ins *cmp = &fn->ins[d.ins[br->a]];
        if (cmp->op != IR_CMP || cmp->w != 4 || !cmp->sign || cmp->flt ||
            (cmp->pred != B_LT && cmp->pred != B_LE))
            continue;
        int v = cmp->a;
        /* a do-while's latch tests the new value, `t = i + s`, which the
         * loop writes back into i (checked with i's definitions below) */
        if (v >= 0 && v < fn->nvregs && d.cnt[v] == 1 && d.ins[v] >= 0) {
            const struct ir_ins *t = &fn->ins[d.ins[v]];
            if (t->op == IR_ADD && t->w == 4 && !t->flt && t->a >= 0 &&
                t->a < fn->nvregs && d.cnt[t->a] >= 2)
                v = t->a;
        }
        if (v < 0 || v >= fn->nvregs || d.cnt[v] < 2)
            continue;
        /* B: a constant, or a value the loop does not write */
        long long Bmax;
        int bconst = ivx_const_b(fn, &d, cmp, &Bmax);
        if (!bconst) {
            int b = cmp->b;
            if (b < 0 || b >= fn->nvregs || b == v)
                continue;
            /* not written in the loop: checked with v's defs */
            Bmax = 2147483647LL;
        }
        /* the loop's blocks */
        int nl = 0;
        in[h] = 1; stk[nl++] = h;
        {
            int sp = 0, *ws = xmalloc((size_t)nbb * sizeof *ws);
            if (!in[p]) { in[p] = 1; stk[nl++] = p; ws[sp++] = p; }
            while (sp) {
                int b = ws[--sp];
                for (int k = 0; k < bb[b].npred; k++) {
                    int r = bb[b].pred[k];
                    if (!in[r]) { in[r] = 1; stk[nl++] = r; ws[sp++] = r; }
                }
            }
            free(ws);
        }
        /* v's definitions: one `+= s` inside the loop, in p before the
         * compare; and on every edge into the header from outside, the
         * definition that reaches it is a constant (lo..hi0). Not "every
         * definition outside": a loop inside another one is entered
         * again with whatever its last exit left in v -- a counter not
         * reset before it keeps growing, and may wrap. */
        long long lo = 0, hi0 = 0, s = 0;
        int ok = 1, nout = 0, nin = 0;
        for (int n = 0; n < fn->nins && ok; n++) {
            struct ir_ins *i = &fn->ins[n];
            int t = def_target(i);
            if (!bconst && t == cmp->b && in[i2b[n]]) {
                ok = 0;                 /* the bound moves with the loop */
                break;
            }
            if (t != v || !in[i2b[n]])
                continue;
            if (i2b[n] != p || n > d.ins[br->a] || nin++) { ok = 0; break; }
            const struct ir_ins *inc = i;
            if (i->op == IR_MOV && i->a >= 0 && i->a < fn->nvregs &&
                d.cnt[i->a] == 1 && d.ins[i->a] >= 0)
                inc = &fn->ins[d.ins[i->a]];
            if (inc->op != IR_ADD || inc->w != 4 || inc->flt ||
                inc->a != v || !ivx_const_b(fn, &d, inc, &s)) { ok = 0; break; }
            /* and nothing redefines v between the add and this write */
            if (inc != i && (d.ins[i->a] < bb[p].start || d.ins[i->a] > n))
                ok = 0;
            /* a compare of the new value must be of THIS add's */
            if (cmp->a != v && (inc == i || i->a != cmp->a))
                ok = 0;
        }
        for (int q = 0; q < bb[h].npred && ok; q++) {
            int e = bb[h].pred[q], found = 0;
            if (in[e])
                continue;
            for (int hop = 0; hop < 8 && !found && ok; hop++) {
                for (int n = bb[e].end - 1; n >= bb[e].start; n--) {
                    struct ir_ins *i = &fn->ins[n];
                    if (i->op == IR_ASM || i->op == IR_CALL) {
                        /* (v is a temp: a call does not write it, but
                         * stop at anything unusual) */
                        if (i->op == IR_ASM) { ok = 0; break; }
                    }
                    if (def_target(i) != v)
                        continue;
                    const struct ir_ins *ci = i;
                    if (i->op == IR_MOV && i->a >= 0 && i->a < fn->nvregs &&
                        d.cnt[i->a] == 1 && d.ins[i->a] >= 0)
                        ci = &fn->ins[d.ins[i->a]];
                    if (ci->op != IR_CONST || ci->w != 4) { ok = 0; break; }
                    long long c = (long long)(int)ci->imm;
                    if (!nout++ || c < lo) lo = c;
                    if (nout == 1 || c > hi0) hi0 = c;
                    found = 1;
                    break;
                }
                if (found || !ok)
                    break;
                if (bb[e].npred != 1) { ok = 0; break; }
                e = bb[e].pred[0];
                if (in[e]) { ok = 0; break; }
            }
            if (!found)
                ok = 0;
        }
        if (!ok || !nout || nin != 1 || s <= 0 || s > 65536 ||
            (!bconst && (s != 1 || cmp->pred != B_LT))) {
            for (int k = 0; k < nl; k++) in[stk[k]] = 0;
            continue;
        }
        long long hv = cmp->pred == B_LT ? Bmax - 1 : Bmax;
        if (hi0 > hv) hv = hi0;
        long long hi = hv + s;
        if (hi > 2147483647LL) {
            for (int k = 0; k < nl; k++) in[stk[k]] = 0;
            continue;
        }
        /* each `ext(v + c)` in the loop whose add cannot wrap */
        for (int k = 0; k < nl; k++) {
            int b = stk[k];
            for (int n = bb[b].start; n < bb[b].end; n++) {
                struct ir_ins *x = &fn->ins[n];
                long long c = 0;
                int cp = x->op == IR_MOV;       /* `ext(v)` through a copy */
                if ((x->op != IR_ADD && x->op != IR_SUB && !cp) ||
                    x->w != 4 || x->flt || x->a != v ||
                    (!cp && !ivx_const_b(fn, &d, x, &c)) ||
                    x->dst < 0 ||
                    x->dst >= fn->nvregs || d.cnt[x->dst] != 1 ||
                    uses[x->dst] != 1)
                    continue;
                struct ir_ins *e = &fn->ins[user[x->dst]];
                if (e->op != IR_EXT || e->a != x->dst || e->size != 4 ||
                    !e->sign || e->w != 8)
                    continue;
                if (x->op == IR_SUB) c = -c;
                if (lo + c < -2147483648LL || hi + c > 2147483647LL)
                    continue;
                /* x: ext(v) where it read v; e (below): that plus c, the
                 * constant a temp of its own as the folds that follow
                 * read one */
                x->op = IR_EXT; x->size = 4; x->sign = 1; x->w = 8;
                x->imm_b = 0; x->imm = 0; x->b = -1;
                if (cp) {               /* e is a copy of it */
                    e->op = IR_MOV; e->size = 0;
                } else {
                    eadd[user[x->dst]] = 1;
                    ec[user[x->dst]] = c;
                }
                changed = 1;
            }
        }
        for (int k = 0; k < nl; k++) in[stk[k]] = 0;
    }
    if (changed) {
        struct ibuf nb = { 0, 0, 0 };
        int *newpos = fn->var_scope_lo
            ? xmalloc((size_t)(fn->nins + 1) * sizeof *newpos) : NULL;
        for (int n = 0; n < fn->nins; n++) {
            if (newpos) newpos[n] = nb.n;
            struct ir_ins o = fn->ins[n];
            if (!eadd[n]) {
                *ib_push(&nb) = o;
                continue;
            }
            int k = fn->nvregs++;
            struct ir_ins *c = ib_push(&nb);
            memset(c, 0, sizeof *c);
            c->op = IR_CONST; c->dst = k; c->imm = (long)ec[n]; c->w = 8;
            c->sign = 1; c->a = c->b = -1;
            c->line = o.line; c->col = o.col; c->synth = 1;
            struct ir_ins *e = ib_push(&nb);
            *e = o;
            e->op = IR_ADD; e->b = k; e->imm_b = 0; e->imm = 0;
            e->sign = 1; e->w = 8;
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
    }
    free(in); free(eadd); free(ec); free(stk); free(i2b); free(uses);
    free(user);
    free_defs(&d);
    free(order); free(l2b); free_cfg(bb, nbb);
    return changed;
}
