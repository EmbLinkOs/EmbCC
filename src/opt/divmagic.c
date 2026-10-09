/* ---- division by a constant ----------------------------------------
 *
 * `x / 3` is a hardware divide, which is twenty to forty cycles where
 * almost everything else is one. It does not have to be: for any
 * constant d there is a multiplier M and a shift s with
 * x / d == (x * M) >> s for every x in range, which is Hacker's Delight
 * chapter 10 and what every other compiler emits.
 *
 * Only 32-bit divisions are done here, and that is what makes it cheap:
 * the algorithm needs the HIGH half of a 32x32 multiply, which is the
 * low half of a 64x64 one -- an operation EmbIR already has. A 64-bit
 * division would need a 128-bit multiply, which on these targets is a
 * call into lib/rt and slower than the divide it replaced.
 *
 * Powers of two were already handled above, unsigned only, by turning
 * the divide into a shift. Signed powers of two need a bias first
 * (rounding toward zero, not toward minus infinity), which is why they
 * were left out; they fall into the general path here. */

#include "opt_int.h"

/* The unsigned magic: q = mulhu(x, M), with one correction step when
 * `add` is set. */
struct magicu { unsigned long m; int add, s; };
static struct magicu magic_u32(unsigned long d)
{
    struct magicu r = { 0, 0, 0 };
    int p = 31;
    unsigned long nc = 0xFFFFFFFFUL - (0xFFFFFFFFUL - d + 1) % d;
    unsigned long q1 = 0x80000000UL / nc, r1 = 0x80000000UL - q1 * nc;
    unsigned long q2 = 0x7FFFFFFFUL / d,  r2 = 0x7FFFFFFFUL - q2 * d;
    /* Assigned in the do-while body below, which always runs before the
     * condition reads it -- but EmbCC's own -Wmaybe-uninitialized does
     * not model do-while and warns. Initialising costs nothing (the
     * store is dead and DSE removes it) and keeps the compiler's source
     * clean under its own warnings, which tests/golden/warnings-uninit
     * asserts. The analysis gap is real and is its own piece of work. */
    unsigned long delta = 0;
    do {
        p++;
        if (r1 >= nc - r1) { q1 = 2 * q1 + 1; r1 = 2 * r1 - nc; }
        else               { q1 = 2 * q1;     r1 = 2 * r1; }
        if (r2 + 1 >= d - r2) {
            if (q2 >= 0x7FFFFFFFUL) r.add = 1;
            q2 = 2 * q2 + 1; r2 = 2 * r2 + 1 - d;
        } else {
            if (q2 >= 0x80000000UL) r.add = 1;
            q2 = 2 * q2; r2 = 2 * r2 + 1;
        }
        delta = d - 1 - r2;
    } while (p < 64 && (q1 < delta || (q1 == delta && r1 == 0)));
    r.m = (q2 + 1) & 0xFFFFFFFFUL;
    r.s = p - 32;
    return r;
}

/* The signed magic: q = mulhs(x, M), then a correction for the sign. */
struct magics { long m; int s; };
static struct magics magic_s32(long d)
{
    struct magics r = { 0, 0 };
    unsigned long ad = (unsigned long)(d < 0 ? -d : d);
    unsigned long t = 0x80000000UL + ((unsigned long)d >> 31 & 1);
    unsigned long anc = t - 1 - t % ad;
    int p = 31;
    unsigned long q1 = 0x80000000UL / anc, r1 = 0x80000000UL - q1 * anc;
    unsigned long q2 = 0x80000000UL / ad,  r2 = 0x80000000UL - q2 * ad;
    unsigned long delta = 0;        /* see magic_u32 on do-while */
    do {
        p++;
        q1 = 2 * q1; r1 = 2 * r1;
        if (r1 >= anc) { q1++; r1 -= anc; }
        q2 = 2 * q2; r2 = 2 * r2;
        if (r2 >= ad) { q2++; r2 -= ad; }
        delta = ad - r2;
    } while (q1 < delta || (q1 == delta && r1 == 0));
    long m = (long)(int)(q2 + 1);
    r.m = d < 0 ? -m : m;
    r.s = p - 32;
    return r;
}

static int log2_pow2_l(unsigned long v)
{
    int k = 0;
    if (!v || (v & (v - 1))) return -1;
    while (v > 1) { v >>= 1; k++; }
    return k;
}

/* Emitting through functions rather than macros, deliberately.
 *
 * These were comma-expression macros, and nesting one inside another --
 * OP(IR_SHR, t, K(32, 4), ...) -- is a buffer-overrun waiting to
 * happen: the macro takes its instruction pointer from ib_push FIRST,
 * then evaluates the argument, which pushes again and may reallocate.
 * The pointer is then dangling and the field writes land in freed
 * memory. It showed up as `shr has no source location`, because the
 * line was written to the wrong instruction.
 *
 * A function argument is fully evaluated before the call begins, so the
 * inner push completes and the outer one takes a fresh pointer. Same
 * expression, no aliasing. */
static int dm_const(struct ibuf *nb, struct ir_func *fn, long v, int w,
                    const struct ir_ins *src)
{
    int t = fn->nvregs++;
    struct ir_ins *p = ib_push(nb);
    p->op = IR_CONST; p->dst = t; p->w = w; p->imm = v;
    p->line = src->line; p->col = src->col; p->synth = 1;
    return t;
}

static int dm_op(struct ibuf *nb, struct ir_func *fn, enum ir_op o,
                 int a, int b, int w, int sg, int size,
                 const struct ir_ins *src)
{
    int t = fn->nvregs++;
    struct ir_ins *p = ib_push(nb);
    p->op = o; p->dst = t; p->a = a; p->b = b;
    p->w = w; p->sign = sg; p->size = size;
    p->line = src->line; p->col = src->col; p->synth = 1;
    return t;
}

/* x / D for a 32-bit machine with a widening multiply: the high half of
 * the 32x32 product is one IR_MULH, so the magic number needs no 64-bit
 * value at all. Hacker's Delight 10-1 (signed) and 10-8 (unsigned), all
 * in 32 bits: the multiplier is the low word of magic_s32's, and its
 * corrections are decided by THAT word's sign -- for a negative D the
 * negated multiplier is what the algorithm defines, wrapped. D is the
 * divisor as the IR holds it (a 32-bit constant, sign-extended). Returns
 * the vreg holding the quotient. */
static int dm_div32(struct ibuf *nb, struct ir_func *fn, int x, long D,
                    int sg, const struct ir_ins *src)
{
    if (!sg) {
        unsigned long ud = (unsigned long)D & 0xFFFFFFFFUL;
        int k = log2_pow2_l(ud);
        if (k >= 0)
            return dm_op(nb, fn, IR_SHR, x, dm_const(nb, fn, k, 4, src),
                         4, 0, 0, src);
        if (ud >= 0x80000000UL) {
            /* The quotient is 0 or 1: one unsigned compare. The constant
             * first -- a pointer from ib_push dangles across another. */
            int c = dm_const(nb, fn, (long)(int)ud, 4, src);
            int t = fn->nvregs++;
            struct ir_ins *p = ib_push(nb);
            p->op = IR_CMP; p->dst = t; p->a = x; p->b = c; p->w = 4;
            p->sign = 0; p->pred = B_GE;
            p->line = src->line; p->col = src->col; p->synth = 1;
            return t;
        }
        struct magicu mg = magic_u32(ud);
        int hi = dm_op(nb, fn, IR_MULH, x,
                       dm_const(nb, fn, (long)(int)mg.m, 4, src), 4, 0, 0, src);
        if (!mg.add)
            return mg.s ? dm_op(nb, fn, IR_SHR, hi,
                                dm_const(nb, fn, mg.s, 4, src), 4, 0, 0, src)
                        : hi;
        /* ((x - hi) >> 1) + hi, which cannot carry out of 32 bits */
        int t4 = dm_op(nb, fn, IR_SUB, x, hi, 4, 0, 0, src);
        int t5 = dm_op(nb, fn, IR_SHR, t4, dm_const(nb, fn, 1, 4, src),
                       4, 0, 0, src);
        int t6 = dm_op(nb, fn, IR_ADD, t5, hi, 4, 0, 0, src);
        return mg.s > 1 ? dm_op(nb, fn, IR_SHR, t6,
                                dm_const(nb, fn, mg.s - 1, 4, src),
                                4, 0, 0, src)
                        : t6;
    }
    struct magics mg = magic_s32(D);
    long m = (long)(int)mg.m;
    int t = dm_op(nb, fn, IR_MULH, x, dm_const(nb, fn, m, 4, src),
                  4, 1, 0, src);
    if (D > 0 && m < 0)
        t = dm_op(nb, fn, IR_ADD, t, x, 4, 1, 0, src);
    else if (D < 0 && m > 0)
        t = dm_op(nb, fn, IR_SUB, t, x, 4, 1, 0, src);
    if (mg.s)
        t = dm_op(nb, fn, IR_SHR, t, dm_const(nb, fn, mg.s, 4, src),
                  4, 1, 0, src);
    /* plus one where the quotient so far is negative: toward zero */
    int sb = dm_op(nb, fn, IR_SHR, t, dm_const(nb, fn, 31, 4, src),
                   4, 0, 0, src);
    return dm_op(nb, fn, IR_ADD, t, sb, 4, 1, 0, src);
}

/* Replace 32-bit `x / C` and `x % C` with a multiply and shifts. */
int pass_divmagic(struct ir_func *fn)
{
    if (fn->nins == 0)
        return 0;
    /* The transform replaces a 32-bit divide with a 64-bit multiply and
     * takes the high half, which needs a machine with 64-bit registers.
     * On ARMv7-M it would need a legalisation pass to become the umull
     * the hardware actually has -- and it would be a LOSS there anyway:
     * the Cortex-M3 and up have `sdiv` and `udiv` in hardware, two bytes
     * each, where the magic sequence is a multiply, a shift and a
     * correction. So it is off where a pointer is four bytes, and the
     * divide stays a divide. */
    /* ...but a SIGNED power of two is shifts and an add at 32 bits on any
     * machine: `x / 2048` and `x % 2048` took sdiv (2 to 12 cycles on a
     * Cortex-M4, 30-odd on many RISC-V cores) where clang and GCC shift.
     * At -Os only where the divide is a library call (ARMv6-M, AVR): with
     * a divide instruction the shifts are a few bytes longer than it. */
    /* A 32-bit machine with a widening multiply has the high half in one
     * instruction (target_has_mulh), and takes the magic number as a
     * 64-bit one does -- at -Os too where the divide is a library call
     * (ARM state: __aeabi_uidiv), which is the slower by far and hardly
     * the shorter once its argument registers are counted. Where it is
     * an instruction, -Os keeps it: four bytes against a dozen. */
    int narrow = target_ptr_size() < 8;
    if (narrow && g_opt_size) {
        struct ir_ins probe;
        memset(&probe, 0, sizeof probe);
        probe.op = IR_DIV; probe.w = 4; probe.sign = 1;
        if (!target_op_calls_helper(&probe))
            return 0;
    }
    int mulh32 = narrow && target_has_mulh() && !getenv("EMBCC_NO_MULH");
    int pow2_only = narrow && !mulh32;
    struct defs d;
    compute_defs(fn, &d);
    struct ibuf nb = { 0, 0, 0 };
    int *newpos = fn->var_scope_lo
        ? xmalloc((size_t)(fn->nins + 1) * sizeof *newpos) : NULL;
    int changed = 0;

    for (int n = 0; n < fn->nins; n++) {
        if (newpos) newpos[n] = nb.n;
        struct ir_ins *src = &fn->ins[n];
        long D;
        /* (an unsigned divide by 0xffffffff, which the IR holds as -1,
         * is a compare on the 32-bit path) */
        if ((src->op != IR_DIV && src->op != IR_MOD) || src->flt ||
            src->w != 4 || src->dst < 0 || !const_b(fn, &d, src, &D) ||
            D == 0 || D == 1 || (D == -1 && !(mulh32 && !src->sign))) {
            *ib_push(&nb) = *src;
            continue;
        }
        if (pow2_only && !(src->sign &&
                           log2_pow2_l((unsigned long)(D < 0 ? -D : D)) >= 0)) {
            *ib_push(&nb) = *src;
            continue;
        }
        int is_mod = src->op == IR_MOD, sg = src->sign, x = src->a;
        /* An unsigned power of two is already a shift by the time this
         * runs; a signed one is not, and its bias is cheaper than a
         * multiply, so take it here. */
        int q;
        if (mulh32 &&
            !(sg && log2_pow2_l((unsigned long)(D < 0 ? -D : D)) >= 0)) {
            q = dm_div32(&nb, fn, x, (long)(int)D, sg, src);
        } else if (sg && log2_pow2_l((unsigned long)(D < 0 ? -D : D)) >= 0) {
            int sh = log2_pow2_l((unsigned long)(D < 0 ? -D : D));
            /* q = (x + ((x >> 31) >>u (32 - sh))) >> sh */
            int t1 = dm_op(&nb, fn, IR_SHR, x, dm_const(&nb, fn, 31, 4, src),
                           4, 1, 0, src);
            int t2 = sh == 0 ? t1
                   : dm_op(&nb, fn, IR_SHR, t1,
                           dm_const(&nb, fn, 32 - sh, 4, src), 4, 0, 0, src);
            int t3 = dm_op(&nb, fn, IR_ADD, x, t2, 4, 1, 0, src);
            q = sh == 0 ? t3
              : dm_op(&nb, fn, IR_SHR, t3, dm_const(&nb, fn, sh, 4, src),
                      4, 1, 0, src);
            if (D < 0)
                q = dm_op(&nb, fn, IR_SUB, dm_const(&nb, fn, 0, 4, src), q,
                          4, 1, 0, src);
        } else if (sg) {
            struct magics mg = magic_s32(D);
            int xe = dm_op(&nb, fn, IR_EXT, x, -1, 8, 1, 4, src);
            int hi = dm_op(&nb, fn, IR_MUL, xe,
                           dm_const(&nb, fn, mg.m, 8, src), 8, 1, 0, src);
            /* With no correction between them, the two arithmetic shifts
             * -- the high half, then mg.s -- are one: |q| < 2^31, so its
             * low 32 bits are the quotient whichever width reads them. */
            int fold = !(D > 0 && mg.m < 0) && !(D < 0 && mg.m > 0) &&
                       mg.s > 0 && !getenv("EMBCC_NO_DIVSHIFT");
            int t3 = dm_op(&nb, fn, IR_SHR, hi,
                           dm_const(&nb, fn, 32 + (fold ? mg.s : 0), 8, src),
                           8, 1, 0, src);
            if (fold)
                mg.s = 0;
            if (D > 0 && mg.m < 0)
                t3 = dm_op(&nb, fn, IR_ADD, t3, x, 4, 1, 0, src);
            else if (D < 0 && mg.m > 0)
                t3 = dm_op(&nb, fn, IR_SUB, t3, x, 4, 1, 0, src);
            int t4 = mg.s ? dm_op(&nb, fn, IR_SHR, t3,
                                  dm_const(&nb, fn, mg.s, 4, src), 4, 1, 0, src)
                          : t3;
            int t5 = dm_op(&nb, fn, IR_SHR, t4,
                           dm_const(&nb, fn, 31, 4, src), 4, 0, 0, src);
            q = dm_op(&nb, fn, IR_ADD, t4, t5, 4, 1, 0, src);
        } else {
            struct magicu mg = magic_u32((unsigned long)D & 0xFFFFFFFFUL);
            int xe = dm_op(&nb, fn, IR_EXT, x, -1, 8, 0, 4, src);
            int hi = dm_op(&nb, fn, IR_MUL, xe,
                           dm_const(&nb, fn, (long)mg.m, 8, src), 8, 0, 0, src);
            /* The high half and the final shift are one shift when no
             * correction comes between them: `shr #32; shr #3` was two
             * instructions on x86-64 and aarch64 for every x / 10. The
             * quotient is below 2^31, so it reads the same at 32 bits on
             * every 64-bit target, RV64's sign-extended registers
             * included. */
            int fold = !mg.add && mg.s > 0 && !getenv("EMBCC_NO_DIVSHIFT");
            int t3 = dm_op(&nb, fn, IR_SHR, hi,
                           dm_const(&nb, fn, 32 + (fold ? mg.s : 0), 8, src),
                           8, 0, 0, src);
            if (fold)
                mg.s = 0;
            if (!mg.add) {
                q = mg.s ? dm_op(&nb, fn, IR_SHR, t3,
                                 dm_const(&nb, fn, mg.s, 4, src), 4, 0, 0, src)
                         : t3;
            } else {
                int t4 = dm_op(&nb, fn, IR_SUB, x, t3, 4, 0, 0, src);
                int t5 = dm_op(&nb, fn, IR_SHR, t4,
                               dm_const(&nb, fn, 1, 4, src), 4, 0, 0, src);
                int t6 = dm_op(&nb, fn, IR_ADD, t5, t3, 4, 0, 0, src);
                q = mg.s > 1 ? dm_op(&nb, fn, IR_SHR, t6,
                                     dm_const(&nb, fn, mg.s - 1, 4, src),
                                     4, 0, 0, src)
                             : t6;
            }
        }
        /* The remainder needs q*D first, and THAT pushes instructions --
         * so nothing may be pushed for the result until after it.
         * Pushing the result slot early and filling it later left a
         * zeroed instruction in the buffer, which reads as IR_CONST
         * writing vreg 0 with no source location: a parameter
         * overwritten, and tests/golden/provenance.sh saw the hole. */
        int mq = is_mod ? dm_op(&nb, fn, IR_MUL, q,
                                dm_const(&nb, fn, D, 4, src), 4, sg, 0, src)
                        : -1;
        struct ir_ins *out = ib_push(&nb);
        memset(out, 0, sizeof *out);
        if (is_mod) {
            out->op = IR_SUB; out->a = x; out->b = mq;
        } else {
            out->op = IR_MOV; out->a = q; out->b = -1;
        }
        out->dst = src->dst; out->w = 4; out->sign = sg;
        out->line = src->line; out->col = src->col;
        changed = 1;
    }
    if (!changed) { free(nb.p); free(newpos); free_defs(&d); return 0; }
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
    free_defs(&d);
    g_did.divmagic++;
    return changed;
}

/* ---- the widening multiply ---------------------------------------------
 *
 * `(int64_t)a * b` with 32-bit a and b is, in the IR, a 64-bit multiply
 * of two extended values -- and on a 32-bit machine a 64x64 multiply is
 * three or four multiplies and the adds between them. The product of two
 * 32-bit values needs one: umull/smull, mul + mulh(u), multu. This makes
 * the multiply an IR_MULW of the 32-bit values themselves, where
 * target_has_mulh() says the backend has one, and leaves the extensions
 * to dead-code elimination.
 *
 * An operand qualifies when it is the extension of a 32-bit value
 * (ext.8:4, signed or not) or a constant that the same extension would
 * produce; both must agree on the signedness, which is the multiply's.
 * The value extended must itself be 32 bits wide where it is defined --
 * a narrow read of a wide value would hand the backend a register pair
 * where it reads one register. After the fixpoint, so that nothing
 * which reasons about a 64-bit multiply (strength reduction, the
 * induction variables) meets this instead. */
static int mw_narrow(struct ir_func *fn, const struct defs *d, int v)
{
    if (v < 0 || v >= fn->nvregs)
        return 0;
    if (v < fn->nvars)
        return fn->locals[v].size <= 4 && fn->locals[v].is_int_or_ptr &&
               !fn->locals[v].is_scalar_float;
    if (d->cnt[v] != 1 || d->ins[v] < 0)
        return 0;
    const struct ir_ins *e = &fn->ins[d->ins[v]];
    return e->w == 4 && !e->flt && writes_temp(e->op) && e->op != IR_CALL;
}

/* An operand of the 64-bit multiply as a 32-bit one: *x the vreg (or -1
 * with *k the constant), *s 1 when it is a sign extension, 0 a zero one,
 * and 2 when a constant reads the same either way. 0 if it is neither. */
static int mw_opnd(struct ir_func *fn, const struct defs *d, int v,
                   int *x, long *k, int *s)
{
    if (v < 0 || v >= fn->nvregs || d->cnt[v] != 1 || d->ins[v] < 0)
        return 0;
    const struct ir_ins *e = &fn->ins[d->ins[v]];
    if (e->op == IR_CONST && !e->flt) {
        long c = e->imm;
        *x = -1; *k = c;
        if (c >= 0 && c <= 0x7fffffffL)          *s = 2;
        else if (c < 0 && c >= -0x80000000L)     *s = 1;
        else if (c > 0 && c <= 0xffffffffL)      *s = 0;
        else return 0;
        return 1;
    }
    if (e->op != IR_EXT || e->flt || e->size != 4 || e->w != 8 ||
        !mw_narrow(fn, d, e->a))
        return 0;
    *x = e->a; *s = e->sign ? 1 : 0;
    return 1;
}

int pass_mulwiden(struct ir_func *fn)
{
    if (fn->nins == 0 || target_ptr_size() >= 8 || !target_has_mulh() ||
        getenv("EMBCC_NO_MULW"))
        return 0;
    struct defs d;
    compute_defs(fn, &d);
    struct ibuf nb = { 0, 0, 0 };
    int *newpos = fn->var_scope_lo
        ? xmalloc((size_t)(fn->nins + 1) * sizeof *newpos) : NULL;
    int changed = 0;
    for (int n = 0; n < fn->nins; n++) {
        if (newpos) newpos[n] = nb.n;
        struct ir_ins at = fn->ins[n];
        int xa = -1, xb = -1, sa = -1, sb = -1, ok;
        long ka = 0, kb = 0;
        ok = at.op == IR_MUL && at.w == 8 && !at.flt && at.dst >= 0 &&
             mw_opnd(fn, &d, at.a, &xa, &ka, &sa);
        if (ok && at.imm_b) {
            long c = at.imm;
            xb = -1; kb = c;
            if (c >= 0 && c <= 0x7fffffffL)          sb = 2;
            else if (c < 0 && c >= -0x80000000L)     sb = 1;
            else if (c > 0 && c <= 0xffffffffL)      sb = 0;
            else ok = 0;
        } else if (ok) {
            ok = mw_opnd(fn, &d, at.b, &xb, &kb, &sb);
        }
        /* the signedness both agree on; two constants are the folder's */
        int s = sa == 2 ? sb : sa;
        ok = ok && (xa >= 0 || xb >= 0) &&
             (sa == 2 || sb == 2 || sa == sb) && s != 2;
        if (!ok) {
            *ib_push(&nb) = at;
            continue;
        }
        if (xa < 0)
            xa = dm_const(&nb, fn, (long)(int)ka, 4, &at);
        if (xb < 0)
            xb = dm_const(&nb, fn, (long)(int)kb, 4, &at);
        struct ir_ins *p = ib_push(&nb);
        *p = at;
        p->op = IR_MULW; p->a = xa; p->b = xb; p->sign = s;
        p->imm_b = 0; p->imm = 0;
        changed = 1;
    }
    if (!changed) { free(nb.p); free(newpos); free_defs(&d); return 0; }
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
    free_defs(&d);
    return 1;
}

/* ---- divisibility by a multiply ----------------------------------------
 *
 * `x % C == 0` asks only whether C divides x, and that needs no division
 * (Granlund and Montgomery; LLVM's prepareUREMEqFold/prepareSREMEqFold).
 * With C = D0 * 2^K, D0 odd, and P the inverse of D0 modulo 2^32:
 *
 *   unsigned:  C divides x  <=>  rotr(x * P, K)      <=u (2^32 - 1) / C
 *   signed:    C divides x  <=>  rotr(x * P + A, K)  <=u Q
 *              A = ((2^31 - 1) / D0) with the low K bits cleared,
 *              Q = 2A / 2^K
 *
 * Only the low half of the product is needed, so it is a multiply on
 * every target -- where `x % C` was sdiv and mls on a Cortex-M (2 to 12
 * cycles), a library call on ARMv6-M and AVR, and a high multiply, shifts
 * and a multiply-subtract where divmagic runs. The remainder's only
 * reader must test it against zero (cmp eq/ne 0, brz, brnz); the
 * remainder is then replaced by `rotr(...) >u Q`, which is zero exactly
 * when C divides x, so that reader is left as it was. A power of two
 * (1 and INT_MIN among them) divides x exactly when x's low bits are
 * zero, signed or not: that remainder becomes `x & (C - 1)`, where a
 * signed one was a sign-bias sequence or sdiv. 32 bits only. */
int pass_divtest(struct ir_func *fn)
{
    if (fn->nins == 0 || getenv("EMBCC_NO_DIVTEST"))
        return 0;
    struct defs d;
    compute_defs(fn, &d);
    int *use = xcalloc((size_t)(fn->nvregs ? fn->nvregs : 1), sizeof *use);
    int *rdr = xmalloc((size_t)(fn->nvregs ? fn->nvregs : 1) * sizeof *rdr);
    struct ucount uc = { use, fn->nvregs };
    for (int n = 0; n < fn->nins; n++)
        each_read(&fn->ins[n], count_cb, &uc);
    /* the one reader of each single-use vreg */
    for (int v = 0; v < fn->nvregs; v++)
        rdr[v] = -1;
    for (int n = 0; n < fn->nins; n++) {
        struct opnds o = { { 0 }, 0, 0 };
        each_read(&fn->ins[n], opnd_cb, &o);
        for (int k = 0; k < o.n; k++)
            if (o.v[k] >= 0 && o.v[k] < fn->nvregs)
                rdr[o.v[k]] = n;
    }
    struct ibuf nb = { 0, 0, 0 };
    int changed = 0;
    int *newpos = fn->var_scope_lo
        ? xmalloc((size_t)(fn->nins + 1) * sizeof *newpos) : NULL;
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *src = &fn->ins[n];
        long C;
        if (newpos) newpos[n] = nb.n;
        int ok = src->op == IR_MOD && !src->flt && src->w == 4 &&
                 src->dst >= fn->nvars && src->dst < fn->nvregs &&
                 d.cnt[src->dst] == 1 && use[src->dst] == 1 &&
                 src->a >= 0 && !src->imm_b && get_const(fn, &d, src->b, &C);
        if (ok) {
            long D = src->sign ? (long)(int)C : (long)(unsigned)C;
            unsigned long ud = (unsigned long)(D < 0 ? -D : D) & 0xffffffffUL;
            const struct ir_ins *r = rdr[src->dst] >= 0 ? &fn->ins[rdr[src->dst]] : NULL;
            long Z = 1;
            ok = ud != 0 && r &&
                 (((r->op == IR_BRZ || r->op == IR_BRNZ) && r->w == 4 &&
                   r->a == src->dst) ||
                  (r->op == IR_CMP && !r->flt && r->w == 4 &&
                   (r->pred == B_EQ || r->pred == B_NE) && r->a == src->dst &&
                   (r->imm_b ? r->imm == 0 : get_const(fn, &d, r->b, &Z) && Z == 0)));
        }
        if (!ok) {
            *ib_push(&nb) = *src;
            continue;
        }
        long Dl = src->sign ? (long)(int)C : (long)(unsigned)C;
        unsigned long D = (unsigned long)(Dl < 0 ? -Dl : Dl) & 0xffffffffUL;
        struct ir_ins at = *src;
        if ((D & (D - 1)) == 0) {
            /* the constant first: an ib_push pointer held across another
             * push dangles when that push moves the buffer */
            int mask = dm_const(&nb, fn, (long)(int)(D - 1), 4, &at);
            struct ir_ins *m = ib_push(&nb);
            *m = at;
            m->op = IR_AND; m->sign = 0; m->b = mask;
            changed = 1;
            continue;
        }
        int K = 0;
        while (!((D >> K) & 1))
            K++;
        unsigned long D0 = D >> K, P = D0;
        for (int it = 0; it < 5; it++)          /* Newton: P = 1/D0 mod 2^32 */
            P = (P * (2 - D0 * P)) & 0xffffffffUL;
        unsigned long A = 0, Q;
        if (src->sign) {
            A = (0x7fffffffUL / D0) & ~((1UL << K) - 1);
            Q = (2 * A) >> K;
        } else {
            Q = 0xffffffffUL / D;
        }
        int t = dm_op(&nb, fn, IR_MUL, src->a,
                      dm_const(&nb, fn, (long)(int)P, 4, &at), 4, 0, 4, &at);
        if (src->sign)
            t = dm_op(&nb, fn, IR_ADD, t,
                      dm_const(&nb, fn, (long)(int)A, 4, &at), 4, 0, 4, &at);
        if (K) {
            int lo = dm_op(&nb, fn, IR_SHR, t, dm_const(&nb, fn, K, 4, &at),
                           4, 0, 4, &at);
            int hi = dm_op(&nb, fn, IR_SHL, t,
                           dm_const(&nb, fn, 32 - K, 4, &at), 4, 0, 4, &at);
            t = dm_op(&nb, fn, IR_OR, lo, hi, 4, 0, 4, &at);
        }
        int q = dm_const(&nb, fn, (long)(int)Q, 4, &at);
        struct ir_ins *c = ib_push(&nb);
        memset(c, 0, sizeof *c);
        c->op = IR_CMP; c->dst = at.dst; c->a = t; c->b = q;
        c->pred = B_GT; c->sign = 0; c->w = 4; c->size = 4;
        c->line = at.line; c->col = at.col;
        changed = 1;
    }
    free(use); free(rdr); free_defs(&d);
    if (!changed) {
        free(nb.p);
        free(newpos);
        return 0;
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
    return 1;
}

/* ---- a remainder from the quotient beside it ----
 *
 * `q = a / b; r = a % b;` divided twice: two idivs on x86-64, two calls
 * to the 64-bit helper on Thumb, RISC-V without M and AVR, where a divide
 * is a library routine of hundreds of cycles. C's division truncates, so
 * a == (a / b) * b + a % b whenever a / b is defined, and the remainder is
 * a - q * b -- a multiply and a subtract, at the operation's own width,
 * where the wrap of both gives exactly the remainder's bits. gcc and clang
 * do the same; division by a CONSTANT is already pass_divmagic's.
 *
 * Either order: the digit loop's `d = v % base; v = v / base;` puts the
 * remainder first, so the quotient is computed there instead, and the
 * division that follows becomes a copy of it. Within a block, by operand
 * names: an entry dies when a, b or its result is written again. */
struct dm_ent { int a, b, w, sign, res, at, is_div; };

int pass_divmod(struct ir_func *fn)
{
    struct dm_ent tab[16];
    int ntab = 0, nrw = 0;
    /* A CONSTANT divisor is left alone: pass_divmagic has made it a
     * multiply where that pays, and where it did not -- a core with a
     * hardware divide, Thumb's `udiv; mls` -- the remainder is already
     * one fused instruction, which a separate multiply and subtract
     * (strength-reduced to shifts and adds, the constant rematerialised
     * in the loop) only made longer: the workload's utoa ran 6.7% more
     * instructions on Cortex-M4. */
    struct defs dd;
    compute_defs(fn, &dd);
    /* per instruction: mulq -- this MOD becomes a - q*b with q given;
     * preq -- a DIV into the given fresh q is inserted before this MOD,
     * which then becomes a - q*b; movq -- this DIV becomes a copy of q */
    int *mulq = NULL, *preq = NULL, *movq = NULL;
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        if (i->op == IR_LABEL || i->op == IR_ASM || i->op == IR_LANDING) {
            ntab = 0;
            continue;
        }
        long kb;
        /* ...except on Thumb, where nothing makes a constant divide a
         * multiply (pass_divmagic needs 64-bit registers) and a
         * remainder is `udiv; mls` either way: paired, the quotient's
         * udiv is shared and `a - q*b` is one mls, because the multiply
         * keeps its constant in a register for it (mla_keeps_reg). */
        int kok = target_get() == TARGET_THUMB && i->w == 4 &&
                  !getenv("EMBCC_NO_DIVMOD_CONST");
        int pairable = (i->op == IR_MOD || i->op == IR_DIV) && !i->flt &&
                       !i->imm_b && (i->w == 4 || i->w == 8) &&
                       i->a >= 0 && i->b >= 0 && i->dst >= 0 &&
                       i->dst != i->a && i->dst != i->b &&
                       (kok || !get_const(fn, &dd, i->b, &kb));
        int matched = 0;
        if (pairable) {
            for (int k = 0; k < ntab; k++) {
                struct dm_ent *e = &tab[k];
                if (e->a != i->a || e->b != i->b || e->w != i->w ||
                    e->sign != i->sign || e->is_div == (i->op == IR_DIV))
                    continue;
                if (!mulq) {
                    mulq = xmalloc((size_t)fn->nins * sizeof *mulq);
                    preq = xmalloc((size_t)fn->nins * sizeof *preq);
                    movq = xmalloc((size_t)fn->nins * sizeof *movq);
                    for (int m = 0; m < fn->nins; m++)
                        mulq[m] = preq[m] = movq[m] = -1;
                }
                if (e->is_div) {                /* div then mod */
                    mulq[n] = e->res;
                } else if (preq[e->at] < 0 && mulq[e->at] < 0) {
                    int q = fn->nvregs++;       /* mod then div */
                    preq[e->at] = q;
                    movq[n] = q;
                } else {
                    continue;
                }
                nrw++;
                matched = 1;
                e->a = -2;                      /* used: one pairing each */
                break;
            }
        }
        int t = def_target(i);
        if (t >= 0) {                   /* forget what named t */
            int j = 0;
            for (int k = 0; k < ntab; k++)
                if (tab[k].a != t && tab[k].b != t && tab[k].res != t &&
                    tab[k].a != -2)
                    tab[j++] = tab[k];
            ntab = j;
        }
        if (pairable && !matched && ntab < 16) {
            tab[ntab].a = i->a; tab[ntab].b = i->b; tab[ntab].w = i->w;
            tab[ntab].sign = i->sign; tab[ntab].res = i->dst;
            tab[ntab].at = n; tab[ntab].is_div = i->op == IR_DIV;
            ntab++;
        }
    }
    free_defs(&dd);
    if (!nrw)
        return 0;
    struct ibuf nb = { 0, 0, 0 };
    int *newpos = xmalloc((size_t)(fn->nins + 1) * sizeof *newpos);
    for (int n = 0; n < fn->nins; n++) {
        newpos[n] = nb.n;
        const struct ir_ins o = fn->ins[n];
        if (movq[n] >= 0) {                     /* the division, done above */
            struct ir_ins *m = ib_push(&nb);
            *m = o;
            m->op = IR_MOV; m->a = movq[n]; m->b = -1;
            continue;
        }
        int q = mulq[n] >= 0 ? mulq[n] : preq[n];
        if (q < 0) {
            *ib_push(&nb) = o;
            continue;
        }
        if (preq[n] >= 0) {
            struct ir_ins *d = ib_push(&nb);
            *d = o;
            d->op = IR_DIV; d->dst = q;
        }
        int prod = fn->nvregs++;
        struct ir_ins *m = ib_push(&nb);
        m->op = IR_MUL; m->dst = prod; m->a = q; m->b = o.b; m->w = o.w;
        m->line = o.line; m->col = o.col;
        struct ir_ins *r = ib_push(&nb);
        r->op = IR_SUB; r->dst = o.dst; r->a = o.a; r->b = prod; r->w = o.w;
        r->line = o.line; r->col = o.col;
    }
    newpos[fn->nins] = nb.n;
    remap_scopes(fn, newpos, fn->nins);
    free(newpos);
    free(mulq); free(preq); free(movq);
    free(fn->ins);
    fn->ins = nb.p; fn->nins = nb.n; fn->cap = nb.cap;
    return 1;
}
