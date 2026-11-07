/* ---- if-conversion -------------------------------------------------
 *
 * A branch whose two arms each compute one value and join is a select,
 * and both targets have one without a branch: cmov on x86-64, csel on
 * aarch64. Neither backend emitted either.
 *
 *      %5 = cmp gt %3, %4              %5 = cmp gt %3, %4
 *      brz %5 -> L0                    %2 = select %5 ? %3 : %4
 *      %2 = mov %3
 *      jmp L1
 *   L0: %2 = mov %4
 *   L1:
 *
 * That exact shape is what mem2reg's phi destruction leaves behind for
 * `c ? a : b` and for `if (c) x = a; else x = b;`, so it is worth
 * recognising narrowly rather than generally.
 *
 * ---- what makes it safe, and what makes it worth it ----
 *
 * A select EVALUATES BOTH ARMS. That is the correctness constraint, not
 * a heuristic: neither arm may fault or have an effect, or a branch
 * that was protecting one of them stops protecting it. A load behind a
 * null check is the classic way to get this wrong, which is why only
 * arms that are already VALUES -- a move of something computed before
 * the branch -- are taken here.
 *
 * And it is only a win when the branch was unpredictable. Replacing a
 * well-predicted branch with a data dependency is slower, which is why
 * this fires on one move per arm and not on a long body: a two-value
 * choice is exactly the case where the branch buys nothing. */

#include "opt_int.h"

/* How wide the value in `v` actually is.
 *
 * NOT the width on the move that copies it: mem2reg's phi copies carry
 * the width of the VARIABLE they came from, which for `long x = c ? a :
 * b` is 4 on a move of an 8-byte value. Codegen copies the whole slot
 * either way, so the branch form is right and the select form -- which
 * turns the width into a cmov operand size -- moved half the value. */
static int sel_width(struct ir_func *fn, struct defs *d, int v)
{
    if (v < 0 || v >= fn->nvregs || d->cnt[v] != 1)
        return 0;
    int n = d->ins[v];
    if (n < 0) {
        /* No defining instruction: an incoming parameter. Its width is the
         * one it was DECLARED with. This used to answer 8 -- "a parameter,
         * full width" -- which is harmless where a select is a register move
         * and 8 is the register, and wrong anywhere else: on AVR an 8-byte
         * select is a byte-at-a-time chain through the frame, so a `?:` on
         * two 4-byte parameters asked the backend to move eight bytes out of
         * a value that only ever had four. It presented as a refusal rather
         * than as bad code, which is the only reason it cost an hour and not
         * a day (lib/rt/avrfp.c's `is_inf(ua) ? ub : ua`, at -Os only,
         * because -O0 leaves the branch alone). */
        if (v < fn->nvars && fn->locals[v].is_int_or_ptr) {
            int sz = fn->locals[v].size;
            return sz == 4 || sz == 8 ? sz : 0;
        }
        return 0;
    }
    int w = fn->ins[n].w;
    /* An address is pointer-sized whatever its w says: irgen leaves w at
     * its default 4 on these. Read as 4, `c ? "a" : "b"` -- once value
     * numbering had made both arms plain moves of addresses computed
     * earlier -- became a 32-bit select, and on x86-64 and aarch64 the
     * pointer lost its top half (embsvd crashed in sprintf at -O2). */
    switch (fn->ins[n].op) {
    case IR_STRADDR: case IR_GADDR: case IR_FADDR: case IR_ADDR:
        w = PTRW;
        break;
    default:
        break;
    }
    return w == 4 || w == 8 ? w : 0;
}

static int ifconv_one(struct ir_func *fn)
{
    if (fn->nins == 0)
        return 0;
    int nbb, *l2b;
    struct bb *bb = build_cfg(fn, &nbb, &l2b);
    struct defs d;
    compute_defs(fn, &d);
    int done = 0;

    for (int b = 0; b < nbb && !done; b++) {
        if (bb[b].end - bb[b].start < 1)
            continue;
        struct ir_ins *br = &fn->ins[bb[b].end - 1];
        if ((br->op != IR_BRZ && br->op != IR_BRNZ) || br->a < 0)
            continue;
        /* The taken arm is the labelled block; the other is the next
         * one. For brz the labelled arm runs when the condition is
         * FALSE, which is what decides the order of the select. */
        int els = l2b[br->label], thn = b + 1;
        if (els < 0 || thn >= nbb || els == thn || els <= b)
            continue;
        /* The two arms must be the branch's own, and adjacent.
         *
         * Neither was checked at first and both are miscompiles. If
         * anything else jumps to the else label, deleting that block
         * removes a target something still reaches for; and if any
         * block sits BETWEEN the two arms, deleting only the arms
         * leaves it running unconditionally. The second one is what
         * made an -O2 answer differ here. */
        if (els != thn + 1)
            continue;
        if (bb[thn].npred != 1 || bb[els].npred != 1)
            continue;
        if (bb[thn].pred[0] != b || bb[els].pred[0] != b)
            continue;
        /* then: exactly `dst = mov v` and a jump past the else */
        if (bb[thn].end - bb[thn].start != 2)
            continue;
        struct ir_ins *tm = &fn->ins[bb[thn].start];
        struct ir_ins *tj = &fn->ins[bb[thn].start + 1];
        if (tm->op != IR_MOV || tm->dst < 0 || tm->a < 0 || tm->vol)
            continue;
        if (tj->op != IR_JMP)
            continue;
        /* else: a label and the same one move, falling through to the
         * block the then-arm jumps to */
        if (bb[els].end - bb[els].start != 2)
            continue;
        if (fn->ins[bb[els].start].op != IR_LABEL)
            continue;
        struct ir_ins *em = &fn->ins[bb[els].start + 1];
        if (em->op != IR_MOV || em->dst != tm->dst || em->a < 0 || em->vol)
            continue;
        if (els + 1 >= nbb || l2b[tj->label] != els + 1)
            continue;
        if (tm->flt || em->flt)
            continue;           /* a float select wants its own move */
        int wt = sel_width(fn, &d, tm->a), we = sel_width(fn, &d, em->a);
        if (!wt || wt != we)
            continue;
        if (br->w != 4 && br->w != 8)
            continue;           /* the select tests its condition at w */

        /* Rewrite: the branch becomes the select, and both arms go. */
        int dst = tm->dst;
        int vtrue  = br->op == IR_BRZ ? tm->a : em->a;
        int vfalse = br->op == IR_BRZ ? em->a : tm->a;
        struct ibuf nb = { 0, 0, 0 };
        int *newpos = fn->var_scope_lo
            ? xmalloc((size_t)(fn->nins + 1) * sizeof *newpos) : NULL;
        int lo_t = bb[thn].start, hi_t = bb[thn].end;
        int lo_e = bb[els].start, hi_e = bb[els].end;
        for (int n = 0; n < fn->nins; n++) {
            if (newpos) newpos[n] = nb.n;
            if (n == bb[b].end - 1) {           /* the branch */
                struct ir_ins *sel = ib_push(&nb);
                sel->op = IR_SELECT; sel->dst = dst;
                sel->a = br->a; sel->b = vtrue; sel->c = vfalse;
                sel->w = wt; sel->sign = tm->sign;
                sel->size = br->w;              /* the condition's own */
                sel->line = br->line; sel->col = br->col;
                continue;
            }
            if ((n >= lo_t && n < hi_t) || (n >= lo_e && n < hi_e))
                continue;                       /* both arms */
            *ib_push(&nb) = fn->ins[n];
        }
        if (newpos) {
            newpos[fn->nins] = nb.n;
            for (int v = 0; v < fn->nvars; v++) {
                int l2 = fn->var_scope_lo[v], h2 = fn->var_scope_hi[v];
                if (l2 >= 0 && l2 <= fn->nins) fn->var_scope_lo[v] = newpos[l2];
                if (h2 >= 0 && h2 <= fn->nins) fn->var_scope_hi[v] = newpos[h2];
            }
            free(newpos);
        }
        free(fn->ins);
        fn->ins = nb.p; fn->nins = nb.n; fn->cap = nb.cap;
        g_did.ifconv++;
        done = 1;
    }

    free_defs(&d); free(l2b);
    free_cfg(bb, nbb);
    return done;
}

/* Where a select is a conditional move rather than a branch: x86-64's
 * cmov, aarch64's csel, and an IT block on ARMv7-M and up. Elsewhere
 * (RISC-V, MIPS, AVR, ARMv6-M) the select is lowered with a branch, so
 * turning a branch into one buys nothing and costs moves. */
int target_cheap_select(void)
{
    return target_get() == TARGET_X86_64 || target_get() == TARGET_AARCH64 ||
           (target_get() == TARGET_THUMB && target_thumb_arch() >= 7);
}

/* One arm of a diamond that ifconv_any can fold: a block reached only
 * from the branch at the end of block `b`, holding one move or constant
 * (after a label, if it has one) and then falling through, or jumping, to
 * the block it continues to (*cont). *mi is the move's index, *jmp the
 * jump's, or -1. The move may follow constants into temps defined only
 * there -- irgen's `%3 = const 2; %1 = mov %3`, which copy propagation
 * folds only after if-conversion has run -- and *hs is the first of
 * them: a constant costs nothing to compute on both paths. */
static int arm_one(const struct ir_func *fn, const struct defs *d,
                   const struct bb *bb, int nbb, const int *l2b, int arm,
                   int b, int *hs, int *mi, int *jmp, int *cont)
{
    if (arm <= b || arm >= nbb || bb[arm].npred != 1 || bb[arm].pred[0] != b)
        return 0;
    int s = bb[arm].start, e = bb[arm].end;
    if (s < e && fn->ins[s].op == IR_LABEL)
        s++;
    *jmp = -1;
    if (e - 1 > s && fn->ins[e - 1].op == IR_JMP) {
        *jmp = e - 1;
        e--;
    }
    *hs = s;
    while (s + 1 < e && fn->ins[s].op == IR_CONST && !fn->ins[s].flt &&
           fn->ins[s].dst >= fn->nvars && d->cnt[fn->ins[s].dst] == 1)
        s++;
    if (e - s != 1)
        return 0;
    const struct ir_ins *m = &fn->ins[s];
    if ((m->op != IR_MOV && m->op != IR_CONST) || m->dst < 0 || m->vol ||
        m->flt || (m->op == IR_MOV && m->a < 0))
        return 0;
    *mi = s;
    *cont = *jmp >= 0 ? l2b[fn->ins[*jmp].label] : arm + 1;
    return *cont >= 0;
}

/* The diamonds ifconv_one leaves, where a select is cheap
 * (target_cheap_select): arms that are constants as well as moves
 * (`st = len ? 2 : 0`), and an else arm that block layout put out of
 * line and that jumps back. Each was a branch where clang has an IT
 * block or a cmov. A constant arm becomes a fresh temp, defined -- with
 * any constants the arms computed first -- ahead of the compare that
 * makes the condition, so the backend still finds the compare right
 * before the select and can select on its flags. Both arms go, except a
 * jump the fall-through arm ended in, which the select's successor
 * still needs.
 *
 * A branch with ONE arm, `if (c) dst = v;`, would be `dst = c ? v :
 * dst`; it is not taken, because it never arrives in that shape: phi
 * destruction gives both arms their move (218 diamonds over the tests,
 * libc and the bench at -O2, none one-armed). */
static int ifconv_any(struct ir_func *fn)
{
    if (fn->nins == 0 || !target_cheap_select())
        return 0;
    int nbb, *l2b;
    struct bb *bb = build_cfg(fn, &nbb, &l2b);
    struct defs d;
    compute_defs(fn, &d);
    int done = 0;
    for (int b = 0; b + 1 < nbb && !done; b++) {
        if (bb[b].end - bb[b].start < 1)
            continue;
        struct ir_ins *br = &fn->ins[bb[b].end - 1];
        if ((br->op != IR_BRZ && br->op != IR_BRNZ) || br->a < 0 ||
            (br->w != 4 && br->w != 8) || br->label < 0)
            continue;
        int thn = b + 1, els = l2b[br->label];
        int th, tm, tj, tc, eh = -1, em = -1, ej = -1, ec;
        if (els < 0 ||
            !arm_one(fn, &d, bb, nbb, l2b, thn, b, &th, &tm, &tj, &tc))
            continue;
        if (els == thn || els == tc ||
            !arm_one(fn, &d, bb, nbb, l2b, els, b, &eh, &em, &ej, &ec) ||
            ec != tc || fn->ins[em].dst != fn->ins[tm].dst)
            continue;
        const struct ir_ins *mt = &fn->ins[tm];
        const struct ir_ins *me = &fn->ins[em];
        int wt = mt->op == IR_CONST ? mt->w : sel_width(fn, &d, mt->a);
        int we = me->op == IR_CONST ? me->w : sel_width(fn, &d, me->a);
        if ((wt != 4 && wt != 8) || wt != we)
            continue;
        int dst = mt->dst;
        int vt = mt->a, ve = me->a;
        /* The constants go before the compare that makes the condition
         * when that is the instruction before the branch, else before
         * the select. */
        int at = bb[b].end - 1;
        if (at > bb[b].start && fn->ins[at - 1].op == IR_CMP &&
            fn->ins[at - 1].dst == br->a)
            at--;
        struct ibuf nb = { 0, 0, 0 };
        int *newpos = fn->var_scope_lo
            ? xmalloc((size_t)(fn->nins + 1) * sizeof *newpos) : NULL;
        int ts = bb[thn].start, te = bb[thn].end;
        int es = bb[els].start, ee = bb[els].end;
        for (int n = 0; n < fn->nins; n++) {
            if (newpos) newpos[n] = nb.n;
            if (n == at) {
                for (int h = th; h < tm; h++)
                    *ib_push(&nb) = fn->ins[h];
                for (int h = eh; h < em; h++)
                    *ib_push(&nb) = fn->ins[h];
                if (mt->op == IR_CONST) {
                    struct ir_ins *k = ib_push(&nb);
                    *k = *mt;
                    k->dst = vt = fn->nvregs++;
                }
                if (me->op == IR_CONST) {
                    struct ir_ins *k = ib_push(&nb);
                    *k = *me;
                    k->dst = ve = fn->nvregs++;
                }
            }
            if (n == bb[b].end - 1) {
                struct ir_ins *sel = ib_push(&nb);
                memset(sel, 0, sizeof *sel);
                sel->op = IR_SELECT; sel->dst = dst; sel->a = br->a;
                /* the fall-through arm runs when brz's condition is
                 * NOT zero, brnz's when it is */
                sel->b = br->op == IR_BRZ ? vt : ve;
                sel->c = br->op == IR_BRZ ? ve : vt;
                sel->w = wt; sel->sign = mt->sign;
                sel->size = br->w;
                sel->line = br->line; sel->col = br->col;
                continue;
            }
            if (n >= ts && n < te && n != tj)
                continue;                       /* the then arm */
            if (n >= es && n < ee)
                continue;                       /* the else arm */
            *ib_push(&nb) = fn->ins[n];
        }
        if (newpos) {
            newpos[fn->nins] = nb.n;
            for (int v = 0; v < fn->nvars; v++) {
                int l2 = fn->var_scope_lo[v], h2 = fn->var_scope_hi[v];
                if (l2 >= 0 && l2 <= fn->nins) fn->var_scope_lo[v] = newpos[l2];
                if (h2 >= 0 && h2 <= fn->nins) fn->var_scope_hi[v] = newpos[h2];
            }
            free(newpos);
        }
        free(fn->ins);
        fn->ins = nb.p; fn->nins = nb.n; fn->cap = nb.cap;
        g_did.ifconv++;
        done = 1;
    }
    free_defs(&d); free(l2b);
    free_cfg(bb, nbb);
    return done;
}

/* ---- a choice made with arithmetic -------------------------------------
 *
 * Where a select is a branch again (RISC-V, MIPS, LoongArch) but a
 * comparison's 0 or 1 is one instruction (snez, seqz, sltu), two shapes
 * of diamond are cheaper as arithmetic on that 0 or 1 than as branches:
 *
 *   st = len ? 2 : 0;          st = (len != 0) << 1
 *   if (!len) errors++;        errors = errors + (len == 0)
 *
 * -- two constants that differ by a power of two, and a value against
 * the same value plus a power of two (or minus one). clang does both;
 * EmbCC had a branch, a jump and two loads of a constant on RISC-V, in
 * the state machine of the bench. The 0 or 1 is the condition itself
 * when it is a comparison already, at the right polarity. */
int target_setcc_cheap(void)
{
    switch (target_get()) {
    case TARGET_RISCV32: case TARGET_RISCV64:
    case TARGET_MIPS32: case TARGET_MIPS64: case TARGET_LOONGARCH64:
        return 1;
    default:
        return 0;
    }
}

/* One arm of such a diamond: a block reached only from the branch at
 * the end of `b`, which sets one name to a constant (kind 0), to a value
 * (kind 1) or to a value plus a constant (kind 2), and goes on to *cont.
 * Constants may come through a temp the arm defines. */
struct ar_arm { int kind, x, v, w, s, e, jmp, cont; long k; };

static int ar_arm_parse(struct ir_func *fn, struct defs *d, const struct bb *bb,
                        int nbb, const int *l2b, int arm, int b,
                        struct ar_arm *A)
{
    if (arm <= b || arm >= nbb || bb[arm].npred != 1 || bb[arm].pred[0] != b)
        return 0;
    int s = bb[arm].start, e = bb[arm].end;
    if (s < e && fn->ins[s].op == IR_LABEL)
        s++;
    A->jmp = -1;
    if (e - 1 > s && fn->ins[e - 1].op == IR_JMP) {
        A->jmp = e - 1;
        e--;
    }
    if (e - s < 1 || e - s > 3)
        return 0;
    A->s = s; A->e = e;
    /* every instruction but the last defines a temp only it reads */
    for (int k = s; k + 1 < e; k++) {
        const struct ir_ins *m = &fn->ins[k];
        if ((m->op != IR_CONST && m->op != IR_ADD && m->op != IR_SUB) ||
            m->flt || m->vol || m->dst < fn->nvars || d->cnt[m->dst] != 1)
            return 0;
    }
    const struct ir_ins *last = &fn->ins[e - 1];
    if (last->dst < 0 || last->vol || last->flt)
        return 0;
    A->x = last->dst;
    const struct ir_ins *m = last;
    if (m->op == IR_MOV && m->a >= 0 && m->a < fn->nvregs &&
        d->cnt[m->a] == 1 && d->ins[m->a] >= s && d->ins[m->a] < e - 1)
        m = &fn->ins[d->ins[m->a]];     /* the value, through its temp */
    else if (m->op == IR_MOV && e - s != 1)
        return 0;
    long k;
    A->w = m->op == IR_CONST ? m->w : m->w ? m->w : 4;
    if (m->op == IR_CONST) {
        A->kind = 0; A->k = m->imm; A->v = -1;
    } else if (m->op == IR_MOV && m->a >= 0) {
        A->kind = 1; A->v = m->a; A->k = 0;
    } else if ((m->op == IR_ADD || m->op == IR_SUB) && !m->flt &&
               (m->imm_b ? (k = m->imm, 1) : get_const(fn, d, m->b, &k)) &&
               m->a >= 0) {
        A->kind = 2; A->v = m->a; A->k = m->op == IR_SUB ? -k : k;
    } else {
        return 0;
    }
    /* a value must come from before the branch, not from this arm */
    if (A->v >= 0 && A->v < fn->nvregs && d->cnt[A->v] == 1 &&
        d->ins[A->v] >= s && d->ins[A->v] < e)
        return 0;
    A->cont = A->jmp >= 0 ? l2b[fn->ins[A->jmp].label] : arm + 1;
    return A->cont >= 0;
}

static int ar_log2(unsigned long v)
{
    if (!v || (v & (v - 1)))
        return -1;
    int n = 0;
    while (v > 1) { v >>= 1; n++; }
    return n;
}

static int ifconv_arith(struct ir_func *fn)
{
    if (fn->nins == 0 || !target_setcc_cheap() ||
        getenv("EMBCC_NO_IFARITH"))
        return 0;
    int nbb, *l2b;
    struct bb *bb = build_cfg(fn, &nbb, &l2b);
    struct defs d;
    compute_defs(fn, &d);
    int done = 0, pw = target_ptr_size();
    for (int b = 0; b + 1 < nbb && !done; b++) {
        if (bb[b].end - bb[b].start < 1)
            continue;
        struct ir_ins *br = &fn->ins[bb[b].end - 1];
        if ((br->op != IR_BRZ && br->op != IR_BRNZ) || br->a < 0 ||
            br->label < 0 || (br->w != 4 && br->w != pw))
            continue;
        /* a float comparison's 0 or 1 is not one instruction where float
         * is soft (a helper's result, then sltz) -- the branch on the
         * helper's result is what tests/golden/riscv-inplace.sh asks for */
        if (br->a < fn->nvregs && d.cnt[br->a] == 1 && d.ins[br->a] >= 0 &&
            fn->ins[d.ins[br->a]].op == IR_CMP && fn->ins[d.ins[br->a]].flt)
            continue;
        int thn = b + 1, els = l2b[br->label];
        struct ar_arm T, E;
        if (els < 0 || els == thn ||
            !ar_arm_parse(fn, &d, bb, nbb, l2b, thn, b, &T) ||
            !ar_arm_parse(fn, &d, bb, nbb, l2b, els, b, &E) ||
            T.x != E.x || T.cont != E.cont || els == T.cont)
            continue;
        int w = T.w;
        if (w != E.w || (w != 4 && w != pw))
            continue;
        /* then = the fall-through arm: it runs when brz's condition is
         * not zero, brnz's when it is */
        int then_nz = br->op == IR_BRZ;
        /* the form: x = base + (cond ? delta : 0), cond the arm's own */
        int base, use_then;
        long kbase = 0, delta;
        if (T.kind == 0 && E.kind == 0) {
            base = -1;
            if (E.k == 0 || T.k != 0) {
                kbase = E.k; delta = T.k - E.k; use_then = 1;
            } else {
                kbase = T.k; delta = E.k - T.k; use_then = 0;
            }
        } else if (T.kind == 2 && E.kind == 1 && T.v == E.v) {
            base = E.v; delta = T.k; use_then = 1;
        } else if (E.kind == 2 && T.kind == 1 && T.v == E.v) {
            base = T.v; delta = E.k; use_then = 0;
        } else {
            continue;
        }
        if (w == 4)
            delta = (long)(int)delta;
        int neg = delta < 0;
        int sh = ar_log2(neg ? -(unsigned long)delta : (unsigned long)delta);
        if (sh < 0 || sh > (w == 8 ? 62 : 30))
            continue;
        /* the condition as 0 or 1: `ne` when the chosen arm runs on a
         * non-zero condition, `eq` when on zero */
        int want_nz = use_then ? then_nz : !then_nz;
        int x = T.x;
        struct ibuf nb = { 0, 0, 0 };
        int *newpos = xmalloc((size_t)(fn->nins + 1) * sizeof *newpos);
        int ts = bb[thn].start, te = bb[thn].end;
        int es = bb[els].start, ee = bb[els].end;
        for (int n = 0; n < fn->nins; n++) {
            newpos[n] = nb.n;
            if (n == bb[b].end - 1) {
                struct ir_ins t;
                memset(&t, 0, sizeof t);
                t.a = t.b = t.c = -1;
                t.line = br->line; t.col = br->col;
                int c = br->a, bit;
                const struct ir_ins *cd =
                    c < fn->nvregs && d.cnt[c] == 1 && d.ins[c] >= 0
                        ? &fn->ins[d.ins[c]] : NULL;
                if (cd && cd->op == IR_CMP && !cd->flt && want_nz) {
                    bit = c;                    /* already 0 or 1 */
                } else {
                    struct ir_ins *z = ib_push(&nb);
                    *z = t;
                    z->op = IR_CONST; z->imm = 0; z->w = br->w;
                    int zero = z->dst = fn->nvregs++;
                    struct ir_ins *q = ib_push(&nb);
                    *q = t;
                    q->op = IR_CMP; q->pred = want_nz ? B_NE : B_EQ;
                    q->a = c; q->b = zero; q->w = br->w;
                    bit = q->dst = fn->nvregs++;
                }
                if (w == 8) {
                    struct ir_ins *x8 = ib_push(&nb);
                    *x8 = t;
                    x8->op = IR_EXT; x8->a = bit; x8->size = 4; x8->w = 8;
                    bit = x8->dst = fn->nvregs++;
                }
                if (sh > 0) {
                    struct ir_ins *k = ib_push(&nb);
                    *k = t;
                    k->op = IR_CONST; k->imm = sh; k->w = w;
                    int ks = k->dst = fn->nvregs++;
                    struct ir_ins *sl = ib_push(&nb);
                    *sl = t;
                    sl->op = IR_SHL; sl->a = bit; sl->b = ks; sl->w = w;
                    bit = sl->dst = fn->nvregs++;
                }
                if (base < 0 && kbase == 0 && !neg) {
                    struct ir_ins *mv = ib_push(&nb);
                    *mv = t;
                    mv->op = IR_MOV; mv->a = bit; mv->w = w;
                    mv->dst = x;
                } else {
                    if (base < 0) {
                        struct ir_ins *k = ib_push(&nb);
                        *k = t;
                        k->op = IR_CONST; k->imm = kbase; k->w = w;
                        base = k->dst = fn->nvregs++;
                    }
                    struct ir_ins *op = ib_push(&nb);
                    *op = t;
                    op->op = neg ? IR_SUB : IR_ADD;
                    op->a = base; op->b = bit; op->w = w;
                    op->dst = x;
                }
                continue;
            }
            if (n >= ts && n < te && n != T.jmp)
                continue;                       /* the then arm */
            if (n >= es && n < ee)
                continue;                       /* the else arm */
            *ib_push(&nb) = fn->ins[n];
        }
        newpos[fn->nins] = nb.n;
        remap_scopes(fn, newpos, fn->nins);
        free(newpos);
        free(fn->ins);
        fn->ins = nb.p; fn->nins = nb.n; fn->cap = nb.cap;
        g_did.ifconv++;
        done = 1;
    }
    free_defs(&d); free(l2b);
    free_cfg(bb, nbb);
    return done;
}

int pass_ifconv(struct ir_func *fn)
{
    int changed = 0, guard = 0;
    while (guard++ < 64 &&
           (ifconv_arith(fn) || ifconv_one(fn) || ifconv_any(fn)))
        changed = 1;
    return changed;
}
