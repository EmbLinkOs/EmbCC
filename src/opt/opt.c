/* IR optimizer — local, per-function passes over the single-assignment
 * temporaries of EmbIR (see opt.h). Three passes iterate to a fixpoint:
 * constant folding (+ a few algebraic identities), copy propagation, and
 * dead-code elimination. Each is proven safe by the single-assignment
 * property: a value in a vreg with exactly one definition is invariant, so
 * no control-flow analysis is needed to know it is the same everywhere.
 */
#include "opt.h"

#include <stdlib.h>
#include <string.h>

#include <stdio.h>

#include "../arch/target.h"
#include "../driver/remark.h"
#include "../driver/util.h"

/* What the quiet passes did, counted per function.
 *
 * CSE, dead code, copy propagation and load elimination each run many
 * times inside the fixpoint, so a remark per rewrite would bury the
 * output. One line per function, at the end, answers the question
 * anyone actually asks -- "did anything happen, and what" -- and is
 * comparable between two builds. */
static struct { long lvn, gcse, dce, copy, loadcse, dse, divmagic,
                ifconv, cfgclean, tailrec; } g_did;

/* ---- op classification ---- */

/* Writes a fresh temporary as dst. IR_STVAR writes a LOCAL slot and is
 * handled apart; every other dst-writer produces a single-assignment temp. */
static int writes_temp(enum ir_op op)
{
    switch (op) {
    case IR_CONST: case IR_MOV:
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
    case IR_AND: case IR_OR: case IR_XOR: case IR_SHL: case IR_SHR:
    case IR_NEG: case IR_BNOT: case IR_CMP:
    case IR_LDVAR: case IR_ADDR: case IR_STRADDR: case IR_GADDR:
    case IR_FADDR: case IR_LOAD: case IR_EXT: case IR_BSWAP: case IR_SQRT:
    case IR_I2F: case IR_F2I: case IR_F2F: case IR_CALL: case IR_XCHG:
    case IR_XADD: case IR_CMPXCHG: case IR_ARMW: case IR_CAS: case IR_CAS16:
    case IR_FRAMEADDR: case IR_ALLOCA: case IR_SPSAVE:
    case IR_SELECT:
    /* A vector result is a temp like any other, 16 bytes wide. */
    case IR_VLOAD: case IR_VBIN: case IR_VSPLAT: case IR_VREDADD:
    case IR_VWIDEN:
        return 1;
    default:
        return 0;
    }
}

/* No side effect and cannot fault, so removable when its result is unused.
 * DIV/MOD (÷0 traps), LOAD (may fault or be volatile), CALL (effects), and
 * every store/branch/label are deliberately NOT pure. */
static int is_pure(enum ir_op op)
{
    switch (op) {
    case IR_CONST: case IR_MOV:
    case IR_ADD: case IR_SUB: case IR_MUL:
    case IR_AND: case IR_OR: case IR_XOR: case IR_SHL: case IR_SHR:
    case IR_NEG: case IR_BNOT: case IR_CMP:
    case IR_LDVAR: case IR_ADDR: case IR_STRADDR: case IR_GADDR:
    case IR_FADDR: case IR_EXT: case IR_BSWAP: case IR_SQRT:
    case IR_I2F: case IR_F2I: case IR_F2F:
    case IR_SELECT:        /* both arms are values; it cannot trap */
        return 1;
    default:
        return 0;
    }
}

/* The vreg an instruction assigns (a temp, or a local for IR_STVAR); -1 if
 * it assigns nothing. */
static int def_target(const struct ir_ins *i)
{
    if (i->op == IR_STVAR)
        return i->dst;
    if (writes_temp(i->op))
        return i->dst;
    return -1;
}

/* Visit &field for every vreg this instruction READS (never its dst).
 *
 * IR_LDVAR's and IR_ADDR's `a` name a FRAME SLOT, and they are here
 * anyway, which looks like a bug and is not: irgen starts temp numbering
 * at nvars (`fn->nvregs = f->nvars`), so slots and temps share one space
 * with the slots first. A slot index IS a vreg number.
 *
 * What makes rewriting them safe is the other half of that invariant:
 * every dst a pass propagates is a TEMP, so it is >= nvars and cannot
 * equal a slot. writes_temp() is where that holds -- IR_STVAR, the one
 * op whose dst is a slot, is deliberately not in it. Break either half
 * and copy propagation silently starts loading a different variable, so
 * a probe went looking first: over libc and libcxx on four targets and
 * the whole suite, copy propagation never once met a slot index equal to
 * the vreg it was replacing. */
static void each_read(struct ir_ins *i, void (*cb)(int *, void *), void *ctx)
{
    switch (i->op) {
    case IR_MOV: case IR_NEG: case IR_BNOT:
    case IR_I2F: case IR_F2I: case IR_F2F:
    case IR_EXT: case IR_BSWAP: case IR_SQRT:
    case IR_LDVAR: case IR_ADDR: case IR_LOAD:
    case IR_MEMZERO: case IR_VA_START: case IR_STVAR:
    case IR_ALLOCA: case IR_SPRESTORE:
        cb(&i->a, ctx);
        break;
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
    case IR_AND: case IR_OR: case IR_XOR: case IR_SHL: case IR_SHR:
    case IR_CMP:
        cb(&i->a, ctx);
        if (!i->imm_b)           /* b folded to an immediate: not a vreg read */
            cb(&i->b, ctx);
        break;
    case IR_STORE: case IR_MEMCPY: case IR_XCHG:
    case IR_XADD: case IR_ARMW:
    case IR_VSTORE:
        cb(&i->a, ctx);
        cb(&i->b, ctx);
        break;
    case IR_VBIN:
        cb(&i->a, ctx);
        /* A lane-wise shift's count is a constant in `c`, so there is
         * no second vreg and `b` is -1. Reporting it anyway had DCE
         * counting a use of vreg -1 and incrementing the word BEFORE
         * its array -- which faults only when that word happens to sit
         * on an unmapped page, so it showed up as a compiler crash in
         * four of seventy-two parallel builds and never once on its
         * own. */
        if (i->b >= 0)
            cb(&i->b, ctx);
        break;
    case IR_VLOAD: case IR_VSPLAT: case IR_VREDADD: case IR_VWIDEN:
        cb(&i->a, ctx);
        break;
    case IR_CMPXCHG: case IR_CAS: case IR_CAS16: case IR_SELECT:
        cb(&i->a, ctx);
        cb(&i->b, ctx);
        cb(&i->c, ctx);
        break;
    case IR_BRZ: case IR_BRNZ:
        cb(&i->a, ctx);
        break;
    case IR_RET:
        if (i->a >= 0)
            cb(&i->a, ctx);
        break;
    case IR_CALL:
        if (i->indirect)
            cb(&i->a, ctx);
        for (int k = 0; k < i->nargs; k++)
            cb(&i->argv[k].vreg, ctx);
        break;
    case IR_ASM:
        for (int k = 0; k < i->asm_ir->nin; k++)
            cb(&i->asm_ir->in[k].temp, ctx);
        for (int k = 0; k < i->asm_ir->nout; k++)
            cb(&i->asm_ir->out[k].temp, ctx);
        break;
    default:
        break;   /* CONST, STRADDR, GADDR, FADDR, LABEL, JMP: no reads */
    }
}

/* Counting uses is the one visitor that WRITES through an operand
 * index, so it is the one that turns a bad index into corruption rather
 * than a wrong answer. It carries the array's length and checks.
 * each_read's contract is that every index it reports is a real vreg;
 * this is the backstop for when that stops being true, which it did. */
struct ucount { int *use, n; };

/* The vregs an instruction reads AS VALUES. LDVAR and ADDR are left out
 * on purpose: their `a` is a frame slot (see each_read), which is not a
 * value operand at all -- no instruction computes it, so asking where it
 * was defined is the wrong question. `over` marks an instruction with
 * more operands than fit, and every caller treats that as "do not
 * reason about this one". */
struct opnds { int v[4]; int n, over; };
static void opnd_cb(int *p, void *ctx)
{
    struct opnds *o = ctx;
    if (o->n < (int)(sizeof o->v / sizeof o->v[0]))
        o->v[o->n++] = *p;
    else
        o->over = 1;
}
static void value_opnds(struct ir_ins *i, struct opnds *o)
{
    o->n = 0; o->over = 0;
    if (i->op == IR_LDVAR || i->op == IR_ADDR)
        return;
    each_read(i, opnd_cb, o);
}

/* Does this instruction read vreg `v` as a value? Asked directly rather
 * than through value_opnds, because a call has as many operands as it
 * has arguments and the question is still answerable -- refusing to
 * answer it for anything with a call in it would give up on most
 * functions worth optimizing. */
struct findv { int v, found; };
static void findv_cb(int *p, void *ctx)
{
    struct findv *f = ctx;
    if (*p == f->v)
        f->found = 1;
}
static int ins_reads(struct ir_ins *i, int v)
{
    if (i->op == IR_LDVAR || i->op == IR_ADDR)
        return 0;                    /* a frame slot, not a value operand */
    struct findv f = { v, 0 };
    each_read(i, findv_cb, &f);
    return f.found;
}

/* ---- def analysis ---- */

struct defs {
    int *cnt;    /* number of definitions of each vreg */
    int *ins;    /* index of the sole defining instruction when cnt == 1 */
};

static void compute_defs(struct ir_func *fn, struct defs *d)
{
    d->cnt = xcalloc((size_t)fn->nvregs, sizeof *d->cnt);
    d->ins = xmalloc((size_t)fn->nvregs * sizeof *d->ins);
    for (int v = 0; v < fn->nvregs; v++)
        d->ins[v] = -1;
    /* a parameter is bound once at entry — count that as its definition */
    int np = fn->nparams;
    for (int v = 0; v < np && v < fn->nvregs; v++)
        d->cnt[v] = 1;
    for (int n = 0; n < fn->nins; n++) {
        /* IR_LANDING is the one instruction with TWO destinations -- the
         * exception pointer in dst and the selector in b, as the unwinder
         * left them. def_target returns a single index and writes_temp is
         * deliberately silent about it (an instruction in writes_temp is
         * one LVN and GCSE may value-number, and two landing pads are not
         * the same computation however identical they look). So its
         * definitions are counted here, explicitly. Without this its
         * results look undefined and DCE deletes the landing while
         * keeping the stores that read what it produced. */
        if (fn->ins[n].op == IR_LANDING) {
            int o[2] = { fn->ins[n].dst, fn->ins[n].b };
            for (int k = 0; k < 2; k++) {
                int v = o[k];
                if (v < 0 || v >= fn->nvregs)
                    continue;
                if (d->cnt[v]++ == 0)
                    d->ins[v] = n;
                else
                    d->ins[v] = -1;
            }
            continue;
        }
        int t = def_target(&fn->ins[n]);
        if (t < 0)
            continue;
        if (d->cnt[t]++ == 0)
            d->ins[t] = n;
        else
            d->ins[t] = -1;   /* more than one def: not single-assignment */
    }
}

static void free_defs(struct defs *d)
{
    free(d->cnt);
    free(d->ins);
}

/* Operand b as a constant, whether it has been folded into the
 * instruction or is still a CONST temp. pass_immfold runs after the
 * fixpoint this pass sits in, so inside the pipeline it is always the
 * latter -- which is not what the printed IR shows, because that is
 * printed after the fold. Reading only imm_b here found nothing at all. */
static int const_b(struct ir_func *fn, struct defs *d, struct ir_ins *i,
                   long *out)
{
    if (i->imm_b) { *out = i->imm; return 1; }
    int v = i->b;
    if (v < 0 || v >= fn->nvregs || d->cnt[v] != 1)
        return 0;
    int n = d->ins[v];
    if (n < 0 || fn->ins[n].op != IR_CONST)
        return 0;
    *out = fn->ins[n].imm;
    return 1;
}


/* If vreg v holds an integer constant (single-def IR_CONST, not an SSE
 * bit-pattern), return 1 and its value. */
static int get_const(struct ir_func *fn, struct defs *d, int v, long *out)
{
    if (v < 0 || d->cnt[v] != 1 || d->ins[v] < 0)
        return 0;
    struct ir_ins *di = &fn->ins[d->ins[v]];
    if (di->op != IR_CONST || di->flt)
        return 0;
    *out = di->imm;
    return 1;
}

/* ---- constant folding ---- */

/* Normalize a computed result to the operation width: an int-class result
 * is the sign-extended low 32 bits, exactly what a 32-bit op leaves. */
static long norm(long r, int w)
{
    return w == 8 ? r : (long)(int)r;
}

static void to_const(struct ir_ins *i, long val)
{
    i->op = IR_CONST;
    i->imm = val;
    i->a = -1;
    i->b = -1;
}

static void to_mov(struct ir_ins *i, int src)
{
    i->op = IR_MOV;
    i->a = src;
    i->b = -1;
}

/* Fold a binary integer op on two constants, honoring width and signedness. */
static int fold_bin(enum ir_op op, long A, long B, int w, int sign,
                    enum binop pred, long *out)
{
    unsigned long ua = w == 8 ? (unsigned long)A : (unsigned int)A;
    unsigned long ub = w == 8 ? (unsigned long)B : (unsigned int)B;
    long sa = w == 8 ? A : (int)A;
    long sb = w == 8 ? B : (int)B;
    int sh = (int)(ub & (w == 8 ? 63 : 31));
    unsigned long r;
    switch (op) {
    case IR_ADD: r = ua + ub; break;
    case IR_SUB: r = ua - ub; break;
    case IR_MUL: r = ua * ub; break;
    case IR_AND: r = ua & ub; break;
    case IR_OR:  r = ua | ub; break;
    case IR_XOR: r = ua ^ ub; break;
    case IR_SHL: r = ua << sh; break;
    case IR_SHR: r = sign ? (unsigned long)(sa >> sh) : (ua >> sh); break;
    case IR_CMP: {
        int c;
        switch (pred) {
        case B_EQ: c = ua == ub; break;
        case B_NE: c = ua != ub; break;
        case B_LT: c = sign ? sa < sb  : ua < ub;  break;
        case B_LE: c = sign ? sa <= sb : ua <= ub; break;
        case B_GT: c = sign ? sa > sb  : ua > ub;  break;
        case B_GE: c = sign ? sa >= sb : ua >= ub; break;
        default: return 0;
        }
        r = c ? 1 : 0;
        break;
    }
    default:
        return 0;
    }
    *out = norm((long)r, w);
    return 1;
}

/* Fold an IR_EXT of a constant: keep `size` low bytes, then sign/zero-extend. */
static long fold_ext(long A, int size, int sign, int w)
{
    int bits = size * 8;
    unsigned long m = bits >= 64 ? ~0UL : (((unsigned long)1 << bits) - 1);
    unsigned long low = (unsigned long)A & m;
    long r;
    if (sign && bits < 64 && (low & ((unsigned long)1 << (bits - 1))))
        r = (long)(low | ~m);
    else
        r = (long)low;
    return norm(r, w);
}

static void count_cb(int *p, void *ctx);   /* fwd: use-count accumulator (DCE) */

/* If b is 2^k (k>=1), return k; else -1. */
static int log2_pow2(long b)
{
    if (b <= 1 || (b & (b - 1)))
        return -1;
    int k = 0;
    while ((b >>= 1))
        k++;
    return k;
}

/* Retarget a single-use constant temp to a new value in place (used by strength
 * reduction: rewrite `x * 8` as `x << 3` by turning the `8` literal into `3`).
 * Safe only when the constant has exactly one use — CSE may have shared it. */
static int retarget_const(struct ir_func *fn, struct defs *d, const int *use,
                          int ctemp, long newval)
{
    if (ctemp < 0 || d->cnt[ctemp] != 1 || d->ins[ctemp] < 0 || use[ctemp] != 1)
        return 0;
    struct ir_ins *ci = &fn->ins[d->ins[ctemp]];
    if (ci->op != IR_CONST || ci->flt)
        return 0;
    ci->imm = newval;
    return 1;
}

static int pass_fold(struct ir_func *fn)
{
    struct defs d;
    compute_defs(fn, &d);
    /* use counts, for the single-use check strength reduction needs */
    int *use = xcalloc((size_t)fn->nvregs, sizeof *use);
    struct ucount uc = { use, fn->nvregs };
    for (int n = 0; n < fn->nins; n++)
        each_read(&fn->ins[n], count_cb, &uc);
    int changed = 0;
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        if (i->flt)
            continue;   /* never fold an SSE op as an integer */
        /* Nor an __int128 one as a long. `imm` is 64 bits and norm()
         * truncates anything that is not width 8, so a 128-bit constant
         * cannot even be held here, let alone folded. Skipping these is
         * what lets the REST of the pipeline run on a function that
         * computes with __int128, which used to be excluded whole. */
        if (i->w == 16)
            continue;
        long A, B;
        int ka = get_const(fn, &d, i->a, &A);
        int kb = get_const(fn, &d, i->b, &B);

        if (i->op == IR_NEG || i->op == IR_BNOT) {
            if (ka) {
                unsigned long r = i->op == IR_NEG ? -(unsigned long)A
                                                  : ~(unsigned long)A;
                to_const(i, norm((long)r, i->w));
                changed = 1;
            }
            continue;
        }
        if (i->op == IR_EXT) {
            if (ka) {
                to_const(i, fold_ext(A, i->size, i->sign, i->w));
                changed = 1;
            }
            continue;
        }

        long r;
        switch (i->op) {
        case IR_ADD: case IR_SUB: case IR_MUL:
        case IR_AND: case IR_OR: case IR_XOR:
        case IR_SHL: case IR_SHR: case IR_CMP:
        case IR_DIV: case IR_MOD:   /* not const-folded (÷0), but strength-reduced */
            break;
        default:
            continue;
        }
        if (ka && kb) {
            if (fold_bin(i->op, A, B, i->w, i->sign, i->pred, &r)) {
                to_const(i, r);
                changed = 1;
            }
            continue;
        }
        /* one-operand algebraic identities (valid for any width/signedness) */
        switch (i->op) {
        case IR_ADD:
            if (ka && A == 0) { to_mov(i, i->b); changed = 1; }
            else if (kb && B == 0) { to_mov(i, i->a); changed = 1; }
            break;
        case IR_SUB:
            if (kb && B == 0) { to_mov(i, i->a); changed = 1; }
            break;
        case IR_OR:
            if (ka && A == 0) { to_mov(i, i->b); changed = 1; }
            else if (kb && B == 0) { to_mov(i, i->a); changed = 1; }
            break;
        case IR_XOR:
            if (ka && A == 0) { to_mov(i, i->b); changed = 1; }
            else if (kb && B == 0) { to_mov(i, i->a); changed = 1; }
            break;
        case IR_MUL: {
            int sh;
            if ((ka && A == 0) || (kb && B == 0)) { to_const(i, 0); changed = 1; }
            else if (ka && A == 1) { to_mov(i, i->b); changed = 1; }
            else if (kb && B == 1) { to_mov(i, i->a); changed = 1; }
            /* x * 2^k -> x << k (retarget the literal to k). Handle either
             * operand being the constant, since MUL is commutative. */
            else if (kb && (sh = log2_pow2(B)) >= 0 &&
                     retarget_const(fn, &d, use, i->b, sh)) {
                i->op = IR_SHL; changed = 1;   /* x << k, count already in b */
            } else if (ka && (sh = log2_pow2(A)) >= 0 &&
                       retarget_const(fn, &d, use, i->a, sh)) {
                /* A_const * x -> x << k: put x in a, the retargeted count in b */
                int c = i->a; i->a = i->b; i->b = c;
                i->op = IR_SHL; changed = 1;
            }
            break;
        }
        case IR_DIV:
            /* unsigned x / 2^k -> x >> k (logical) */
            if (kb && !i->sign) {
                int sh = log2_pow2(B);
                if (sh >= 0 && retarget_const(fn, &d, use, i->b, sh)) {
                    i->op = IR_SHR; changed = 1;
                } else if (B == 1) { to_mov(i, i->a); changed = 1; }
            }
            break;
        case IR_MOD:
            /* unsigned x % 2^k -> x & (2^k - 1) */
            if (kb && !i->sign && log2_pow2(B) >= 0 &&
                retarget_const(fn, &d, use, i->b, B - 1)) {
                i->op = IR_AND; changed = 1;
            }
            break;
        case IR_AND:
            if ((ka && A == 0) || (kb && B == 0)) { to_const(i, 0); changed = 1; }
            break;
        case IR_SHL:
        case IR_SHR:
            if (kb && B == 0) { to_mov(i, i->a); changed = 1; }
            break;
        default:
            break;
        }
    }
    free(use);
    free_defs(&d);
    return changed;
}

/* ---- local value numbering (CSE within a basic block) ---- */

/* An op that may write memory (so a cached load past it is stale). */
static int writes_memory(enum ir_op op)
{
    switch (op) {
    case IR_STORE: case IR_STVAR: case IR_CALL: case IR_MEMCPY:
    case IR_MEMZERO: case IR_XCHG: case IR_XADD: case IR_CMPXCHG:
    case IR_ARMW: case IR_CAS: case IR_CAS16: case IR_ASM: case IR_VA_START:
    case IR_VSTORE:
    /* A fence writes nothing, but a load cached from before it must not be
     * reused after it: that reuse IS the reordering the fence forbids, and a
     * loop polling a flag across __sync_synchronize() would spin on a stale
     * value forever. */
    case IR_FENCE:
        return 1;
    default:
        return 0;
    }
}

/* One value-number entry: the discriminating fields of a computation plus the
 * temp that first produced it. Two instructions with equal keys in the same
 * block compute the same value. Loads carry `memver` so a store between two
 * loads gives them different keys (no stale reuse). */
struct vn {
    enum ir_op op;
    int a, b, w, sign, size;
    enum binop pred;
    long imm;
    void *ptr;                 /* GADDR glob / FADDR callee */
    int label;                 /* STRADDR string index */
    int memver;                /* LDVAR / LOAD only */
    int result;                /* the temp holding this value */
};

static int vn_eq(const struct vn *x, const struct vn *y)
{
    return x->op == y->op && x->a == y->a && x->b == y->b && x->w == y->w &&
           x->sign == y->sign && x->size == y->size && x->pred == y->pred &&
           x->imm == y->imm && x->ptr == y->ptr && x->label == y->label &&
           x->memver == y->memver;
}

/* Build the value key for a CSE-able instruction; returns 0 if it is not one
 * (float ops, VOLATILE loads/ldvars, calls, stores — anything with an effect or
 * that we don't number). */
static int vn_key(struct ir_ins *i, int memver, struct vn *k)
{
    memset(k, 0, sizeof *k);
    k->op = i->op; k->a = -1; k->b = -1;
    if (i->flt)
        return 0;
    /* An __int128 constant's key would be its low half only, so two
     * different ones would number the same. */
    if (i->w == 16)
        return 0;
    switch (i->op) {
    case IR_CONST:               /* same literal -> one temp, so uses of it CSE */
        k->imm = i->imm; k->w = i->w; return 1;
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
    case IR_AND: case IR_OR: case IR_XOR: case IR_SHL: case IR_SHR:
        k->a = i->a; k->b = i->b; k->w = i->w; k->sign = i->sign; return 1;
    case IR_CMP:
        k->a = i->a; k->b = i->b; k->w = i->w; k->sign = i->sign;
        k->pred = i->pred; return 1;
    case IR_NEG: case IR_BNOT: case IR_SQRT:
        /* `op` is already part of the key, so these three cannot
         * collide with each other. */
        k->a = i->a; k->w = i->w; return 1;
    case IR_EXT:
        k->a = i->a; k->w = i->w; k->sign = i->sign; k->size = i->size; return 1;
    case IR_BSWAP:
        k->a = i->a; k->size = i->size; return 1;
    case IR_ADDR:                       /* &local: a frame-relative constant */
        k->a = i->a; return 1;
    case IR_GADDR: k->ptr = i->glob; return 1;
    case IR_FADDR: k->ptr = i->callee; return 1;
    case IR_STRADDR: k->label = i->label; return 1;
    case IR_LDVAR:                      /* a variable read (memory-versioned) */
        if (i->vol) return 0;
        k->a = i->a; k->w = i->w; k->sign = i->sign; k->size = i->size;
        k->memver = memver; return 1;
    case IR_LOAD:                       /* a pointer deref (memory-versioned) */
        if (i->vol) return 0;
        k->a = i->a; k->w = i->w; k->sign = i->sign; k->size = i->size;
        k->memver = memver; return 1;
    default:
        return 0;
    }
}

/* Replace a computation that reproduces an earlier one in the same block with a
 * copy of that earlier result; fold/copyprop/dce then remove the redundancy.
 * The block is the run between labels; a memory write bumps `memver` (part of a
 * load's key), so a load after a store is never reused. Sound: temps are
 * single-assignment, so equal operand temps => equal value; VOLATILE accesses
 * are never numbered (vn_key rejects them), preserving every MMIO access. */
/* Does this instruction's key actually NAME a value?
 *
 * A key is built out of vreg numbers, so it identifies a value only
 * where a vreg identifies a value -- that is, where the vreg is assigned
 * once. mem2reg's phi destruction breaks that for exactly the vregs a
 * loop revolves around: an induction variable is assigned on entry and
 * again at the latch, so `cmp lt %i, %n` before the loop and the same
 * text inside it are two different comparisons with one key.
 *
 * Nothing had noticed, because irgen emits a loop's test once and there
 * was no second copy to collide with it. Loop rotation makes a second
 * copy on purpose, and GCSE promptly replaced the rotated test with the
 * guard's result -- a loop whose condition was evaluated once, before it
 * started. The number was not wrong; the assumption under it was.
 *
 * So: every value operand, and the result, must be singly assigned. */
static int vn_stable(struct ir_func *fn, const struct defs *d, struct ir_ins *i)
{
    if (i->dst < 0 || i->dst >= fn->nvregs || d->cnt[i->dst] != 1)
        return 0;
    struct opnds o;
    value_opnds(i, &o);
    if (o.over)
        return 0;
    for (int k = 0; k < o.n; k++) {
        int v = o.v[k];
        if (v < 0 || v >= fn->nvregs || d->cnt[v] != 1)
            return 0;
    }
    return 1;
}

/* Drop every entry that mentions vreg `t`, because something just gave
 * `t` a new value and the entries naming it describe the old one.
 *
 * This is what a block-local table can do that a dominator-scoped one
 * cannot: the pass walks straight-line code, so "has an operand been
 * reassigned since" is answered by having watched. It is also why LVN
 * does not need vn_stable's blanket refusal of multiply-assigned vregs,
 * which would throw away most of a loop body -- `arr[i]` reads `i`, and
 * an induction variable is assigned on every incoming edge. */
static int vn_kill(struct vn *tab, int ntab, int t)
{
    int j = 0;
    for (int x = 0; x < ntab; x++)
        if (tab[x].a != t && tab[x].b != t && tab[x].result != t)
            tab[j++] = tab[x];
    return j;
}

static int pass_lvn(struct ir_func *fn)
{
    int changed = 0, memver = 0;
    struct vn *tab = NULL;
    int ntab = 0, cap = 0;
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        enum ir_op op0 = i->op;
        if (op0 == IR_LABEL) { ntab = 0; continue; }   /* block boundary */
        struct vn k;
        if (i->dst >= 0 && vn_key(i, memver, &k)) {
            int hit = -1;
            for (int t = 0; t < ntab; t++)
                if (vn_eq(&tab[t], &k)) { hit = tab[t].result; break; }
            if (hit >= 0 && hit != i->dst) {
                to_mov(i, hit);
                changed = 1;
                g_did.lvn++;
                op0 = IR_MOV;      /* what it is NOW, for the kill below */
            } else if (hit < 0) {
                ntab = vn_kill(tab, ntab, i->dst);
                if (ntab == cap) {
                    cap = cap ? cap * 2 : 32;
                    tab = xrealloc(tab, (size_t)cap * sizeof *tab);
                }
                k.result = i->dst;
                tab[ntab++] = k;
                if (writes_memory(op0))
                    memver++;
                continue;
            }
        }
        /* Anything else that assigns a vreg -- a call's result, a store
         * to a slot, an instruction with no key at all -- invalidates
         * what named it. Inline asm and a landing pad write temps that
         * def_target cannot report, so they clear the table outright. */
        if (op0 == IR_ASM || op0 == IR_LANDING) {
            ntab = 0;
        } else {
            int t = def_target(i);
            if (t >= 0)
                ntab = vn_kill(tab, ntab, t);
        }
        if (writes_memory(op0))
            memver++;
    }
    free(tab);
    return changed;
}

/* ---- copy propagation ---- */

struct repl { int from, to, n; };

static void repl_cb(int *p, void *ctx)
{
    struct repl *r = ctx;
    if (*p == r->from) {
        *p = r->to;
        r->n++;
    }
}

static int pass_copyprop(struct ir_func *fn)
{
    struct defs d;
    compute_defs(fn, &d);
    int changed = 0;
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        if (i->op != IR_MOV || i->a < 0 || i->dst < 0 || i->a == i->dst)
            continue;
        /* Both ends must be single-assignment: the source so its value is
         * invariant, and the DEST so every use of it comes from THIS move.
         * A dest written in two branches (irgen's phi-style merge, e.g. the
         * va_arg result address) has cnt > 1 — propagating it would wrongly
         * force one branch's value onto the other's uses. */
        if (d.cnt[i->a] != 1 || d.cnt[i->dst] != 1)
            continue;
        struct repl r = { i->dst, i->a, 0 };
        for (int m = 0; m < fn->nins; m++)
            each_read(&fn->ins[m], repl_cb, &r);
        if (r.n) {
            changed = 1;   /* the MOV is now dead; DCE removes it */
            g_did.copy += r.n;
        }
    }
    free_defs(&d);
    return changed;
}

/* ---- dead-code elimination ---- */

static void count_cb(int *p, void *ctx)
{
    struct ucount *u = ctx;
    if (*p >= 0 && *p < u->n)
        u->use[*p]++;
}

static int pass_dce(struct ir_func *fn)
{
    int *use = xcalloc((size_t)fn->nvregs, sizeof *use);
    struct ucount uc = { use, fn->nvregs };
    for (int n = 0; n < fn->nins; n++)
        each_read(&fn->ins[n], count_cb, &uc);
    /* Removing instructions renumbers the ones that follow. fn->var_scope_lo/hi
     * (irgen-stamped instruction indices, read by codegen's coalesce_locals to
     * decide which address-taken locals may share a stack slot) must move with
     * them — otherwise a stale scope index is compared against a FRESH liveness
     * index and two locals whose lifetimes actually overlap get the same slot,
     * so one's store clobbers the other. newpos[n] is the new index instruction
     * n lands at (a dropped instruction collapses onto the next survivor); it
     * maps the half-open [lo, hi) scope bounds, index nins included. */
    int *newpos = fn->var_scope_lo
        ? xmalloc((size_t)(fn->nins + 1) * sizeof *newpos) : NULL;
    int changed = 0, j = 0;
    for (int n = 0; n < fn->nins; n++) {
        if (newpos) newpos[n] = j;
        struct ir_ins *i = &fn->ins[n];
        int t = def_target(i);
        if (is_pure(i->op) && t >= 0 && use[t] == 0) {
            changed = 1;
            g_did.dce++;
            continue;   /* drop it */
        }
        /* Dead store: a non-volatile STVAR to a local nothing ever reads (no
         * LDVAR and no address-of, so use[dst] == 0) has no effect — drop it.
         * This is what clears an inlined parameter once store-forwarding has
         * rewritten its loads to the argument. */
        if (i->op == IR_STVAR && !i->vol && i->dst >= 0 &&
            i->dst < fn->nvregs && use[i->dst] == 0) {
            changed = 1;
            g_did.dce++;
            continue;
        }
        if (j != n)
            fn->ins[j] = *i;
        j++;
    }
    if (newpos) {
        newpos[fn->nins] = j;
        for (int v = 0; v < fn->nvars; v++) {
            int lo = fn->var_scope_lo[v], hi = fn->var_scope_hi[v];
            if (lo >= 0 && lo <= fn->nins) fn->var_scope_lo[v] = newpos[lo];
            if (hi >= 0 && hi <= fn->nins) fn->var_scope_hi[v] = newpos[hi];
        }
        free(newpos);
    }
    fn->nins = j;
    free(use);
    return changed;
}

/* ==== SSA-based mem2reg (-O2) =============================================== *
 *
 * Promotes every non-address-taken scalar local out of memory into SSA temps.
 * Builds the CFG, the dominator tree (Cooper-Harvey-Kennedy) and dominance
 * frontiers, inserts phi-functions at the iterated frontier of each variable's
 * defs, renames defs/uses to versioned temps down the dominator tree, then
 * destructs SSA by realising each phi as copies on its incoming edges — the
 * branch-taken edge via a trampoline block, a branch's fall-through with inline
 * copies (they run only when the branch is not taken), single-successor edges by
 * appending. Every copy set is sequenced read-all-then-write-all through fresh
 * temps, so a swap or a self-referential loop phi is safe. This turns
 * STVAR/LDVAR chains that cross basic blocks — loop counters, a value live down
 * one arm of an `if` — into temps that fold/lvn/copyprop then optimise, which is
 * what the block-local store-forwarding could not reach. */

struct bb {
    int start, end;                 /* instruction range [start, end) */
    int succ[2], nsucc;
    int *pred, npred;
    int idom, rpo;                  /* immediate dominator; reverse-postorder # */
    int *phi_local, *phi_res, nphi; /* phi(local) -> result temp, per block */
    int **phi_inc;                  /* phi_inc[p][k] = value on edge from pred p */
};

/* Growable instruction buffer, for rebuilding fn->ins out of SSA. */
struct ibuf { struct ir_ins *p; int n, cap; };
static struct ir_ins *ib_push(struct ibuf *b)
{
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 64;
        b->p = xrealloc(b->p, (size_t)b->cap * sizeof *b->p);
    }
    struct ir_ins *i = &b->p[b->n++];
    memset(i, 0, sizeof *i);
    return i;
}

static void bb_add_pred(struct bb *b, int p)
{
    for (int i = 0; i < b->npred; i++)
        if (b->pred[i] == p) return;
    b->pred = xrealloc(b->pred, (size_t)(b->npred + 1) * sizeof *b->pred);
    b->pred[b->npred++] = p;
}

/* Build basic blocks + succ/pred + a label->block map. */
static struct bb *build_cfg(struct ir_func *fn, int *nbb_out, int **l2b_out)
{
    int N = fn->nins;
    char *lead = xcalloc((size_t)(N ? N : 1), 1);
    if (N) lead[0] = 1;
    for (int i = 0; i < N; i++) {
        enum ir_op op = fn->ins[i].op;
        if (op == IR_LABEL) lead[i] = 1;
        if ((op == IR_JMP || op == IR_BRZ || op == IR_BRNZ ||
             op == IR_RET || op == IR_UD2) && i + 1 < N)
            lead[i + 1] = 1;
    }
    int nbb = 0;
    for (int i = 0; i < N; i++) if (lead[i]) nbb++;
    if (nbb == 0) nbb = 1;
    struct bb *bb = xcalloc((size_t)nbb, sizeof *bb);
    int b = 0, prev = 0;
    for (int i = 1; i <= N; i++)
        if (i == N || lead[i]) { bb[b].start = prev; bb[b].end = i; prev = i; b++; }
    for (int i = 0; i < nbb; i++) { bb[i].idom = -1; bb[i].rpo = -1; }

    int *l2b = xmalloc((size_t)(fn->nlabels ? fn->nlabels : 1) * sizeof *l2b);
    for (int l = 0; l < fn->nlabels; l++) l2b[l] = -1;
    for (int i = 0; i < nbb; i++)
        if (bb[i].end > bb[i].start && fn->ins[bb[i].start].op == IR_LABEL)
            l2b[fn->ins[bb[i].start].label] = i;

    for (int i = 0; i < nbb; i++) {
        enum ir_op op = bb[i].end > bb[i].start ? fn->ins[bb[i].end - 1].op : IR_UD2;
        int L = bb[i].end > bb[i].start ? fn->ins[bb[i].end - 1].label : -1;
        if (op == IR_RET || op == IR_UD2) {
            /* no successors */
        } else if (op == IR_JMP) {
            if (l2b[L] >= 0) bb[i].succ[bb[i].nsucc++] = l2b[L];
        } else if (op == IR_BRZ || op == IR_BRNZ) {
            if (l2b[L] >= 0) bb[i].succ[bb[i].nsucc++] = l2b[L];
            if (i + 1 < nbb) bb[i].succ[bb[i].nsucc++] = i + 1;
        } else if (i + 1 < nbb) {
            bb[i].succ[bb[i].nsucc++] = i + 1;
        }
    }
    for (int i = 0; i < nbb; i++)
        for (int k = 0; k < bb[i].nsucc; k++)
            bb_add_pred(&bb[bb[i].succ[k]], i);
    free(lead);
    *nbb_out = nbb;
    *l2b_out = l2b;
    return bb;
}

/* Reverse-postorder numbering from the entry (block 0). */
static void compute_rpo(struct bb *bb, int nbb, int *order, int *norder)
{
    char *seen = xcalloc((size_t)nbb, 1);
    int *stk = xmalloc((size_t)nbb * sizeof *stk), *it = xmalloc((size_t)nbb * sizeof *it);
    int sp = 0, po = 0, *post = xmalloc((size_t)nbb * sizeof *post);
    stk[sp] = 0; it[sp] = 0; seen[0] = 1;
    while (sp >= 0) {
        int u = stk[sp];
        if (it[sp] < bb[u].nsucc) {
            int w = bb[u].succ[it[sp]++];
            if (!seen[w]) { seen[w] = 1; sp++; stk[sp] = w; it[sp] = 0; }
        } else { post[po++] = u; sp--; }
    }
    *norder = po;
    for (int i = 0; i < po; i++) order[i] = post[po - 1 - i];   /* reverse */
    for (int i = 0; i < po; i++) bb[order[i]].rpo = i;
    free(seen); free(stk); free(it); free(post);
}

static int idom_intersect(struct bb *bb, int a, int b)
{
    while (a != b) {
        while (bb[a].rpo > bb[b].rpo) a = bb[a].idom;
        while (bb[b].rpo > bb[a].rpo) b = bb[b].idom;
    }
    return a;
}

/* Immediate dominators (Cooper-Harvey-Kennedy) over the reachable blocks. */
static void compute_idom(struct bb *bb, int *order, int norder)
{
    bb[0].idom = 0;
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int i = 1; i < norder; i++) {       /* skip entry, RPO order */
            int b = order[i], nd = -1;
            for (int k = 0; k < bb[b].npred; k++) {
                int p = bb[b].pred[k];
                if (bb[p].idom < 0) continue;    /* not yet processed */
                nd = nd < 0 ? p : idom_intersect(bb, nd, p);
            }
            if (nd >= 0 && bb[b].idom != nd) { bb[b].idom = nd; changed = 1; }
        }
    }
}

/* Dominance frontiers. df[b] holds the blocks on b's frontier. */
static void compute_df(struct bb *bb, int nbb, int **df, int *ndf)
{
    for (int b = 0; b < nbb; b++) {
        if (bb[b].npred < 2) continue;
        for (int k = 0; k < bb[b].npred; k++) {
            int r = bb[b].pred[k];
            while (r >= 0 && r != bb[b].idom) {
                int dup = 0;
                for (int j = 0; j < ndf[r]; j++) if (df[r][j] == b) dup = 1;
                if (!dup) { df[r] = xrealloc(df[r], (size_t)(ndf[r]+1)*sizeof(int));
                            df[r][ndf[r]++] = b; }
                r = bb[r].idom;
            }
        }
    }
}

/* A full-width plain access (no truncation/extension mismatch between a store
 * and a load) — the same soundness gate mem2reg and store-forwarding share. */
static int m2r_plain(int size, int sign, int w)
{
    return size == 8 || (size == 4 && !(sign && w == 8));
}

/* Emit the phi copies for edge (pred p -> block s): read every incoming value
 * into a fresh temp, then write each phi result — read-all-then-write-all, so a
 * self-referential loop phi or a swap is realised correctly. */
static void emit_edge_copies(struct ibuf *nb, struct bb *bb, int s, int p,
                             struct ir_func *fn)
{
    struct bb *S = &bb[s];
    if (S->nphi == 0) return;
    int pi = -1;
    for (int k = 0; k < S->npred; k++) if (S->pred[k] == p) { pi = k; break; }
    if (pi < 0) return;
    /* A phi copy runs on the EDGE from p, so it belongs to whatever ends p
     * -- the branch or the fall-through's last instruction (R3). Going out
     * of SSA is the compiler's own bookkeeping, but the copy still executes
     * at a place the programmer wrote, and a line table with a hole here is
     * a debugger stepping into nowhere. */
    int eline = 0, ecol = 0;
    if (bb[p].end > bb[p].start) {
        eline = fn->ins[bb[p].end - 1].line;
        ecol = fn->ins[bb[p].end - 1].col;
    }
    /* A conflict — an incoming value that is another phi result of this block
     * (a swap, a self-referential loop phi) — needs read-all-then-write-all
     * through temps. The common case has none: emit direct copies, no temps. */
    int conflict = 0;
    for (int k = 0; k < S->nphi && !conflict; k++)
        for (int j = 0; j < S->nphi; j++)
            if (S->phi_inc[pi][k] == S->phi_res[j]) { conflict = 1; break; }
    if (!conflict) {
        for (int k = 0; k < S->nphi; k++) {
            struct ir_ins *mv = ib_push(nb);
            mv->op = IR_MOV; mv->dst = S->phi_res[k]; mv->a = S->phi_inc[pi][k];
            mv->w = fn->locals[S->phi_local[k]].size == 8 ? 8 : 4;
            mv->line = eline; mv->col = ecol; mv->synth = !eline;
        }
        return;
    }
    int *tmp = xmalloc((size_t)S->nphi * sizeof *tmp);
    for (int k = 0; k < S->nphi; k++) {
        tmp[k] = fn->nvregs++;
        struct ir_ins *mv = ib_push(nb);
        mv->op = IR_MOV; mv->dst = tmp[k]; mv->a = S->phi_inc[pi][k];
        mv->w = fn->locals[S->phi_local[k]].size == 8 ? 8 : 4;
        mv->line = eline; mv->col = ecol; mv->synth = !eline;
    }
    for (int k = 0; k < S->nphi; k++) {
        struct ir_ins *mv = ib_push(nb);
        mv->op = IR_MOV; mv->dst = S->phi_res[k]; mv->a = tmp[k];
        mv->w = fn->locals[S->phi_local[k]].size == 8 ? 8 : 4;
        mv->line = eline; mv->col = ecol; mv->synth = !eline;
    }
    free(tmp);
}

static void mem2reg_free(struct bb *bb, int nbb, int **df, int *ndf, int *l2b,
                         int *order)
{
    for (int i = 0; i < nbb; i++) {
        free(bb[i].pred);
        free(bb[i].phi_local); free(bb[i].phi_res);
        if (bb[i].phi_inc) {
            for (int k = 0; k < bb[i].npred; k++) free(bb[i].phi_inc[k]);
            free(bb[i].phi_inc);
        }
        free(df[i]);
    }
    free(bb); free(df); free(ndf); free(l2b); free(order);
}

/* The name a local was written with. irgen records these unconditionally
 * ("harmless when -g is off"), so a remark can name the variable the
 * programmer knows rather than a slot number. */
static const struct ir_dbgvar *local_var(const struct ir_func *fn, int L)
{
    for (int i = 0; i < fn->ndbgvars; i++)
        if (fn->dbgvars[i].vreg == L && !fn->dbgvars[i].is_param)
            return &fn->dbgvars[i];
    return NULL;
}

static int pass_mem2reg(struct ir_func *fn)
{
    int nvars = fn->nvars;
    if (nvars == 0 || fn->nins == 0)
        return 0;

    /* 1. Promotable locals: a scalar int/ptr of 4 or 8 bytes, never
     * address-taken, every load full-width plain.
     *
     * Each rejection keeps its OWN reason (R2): "why is this variable still
     * on the stack" is the question this pass answers, and five different
     * causes used to leave the same zero behind. */
    int nparams = fn->nparams;
    char *ok = xmalloc((size_t)nvars);
    const char **why = xcalloc((size_t)nvars, sizeof *why);
    for (int L = 0; L < nvars; L++) {
        const struct ir_local *Li = &fn->locals[L];
        /* Params are excluded: their value is live on entry (no defining IR
         * instruction), so SSA has no version to seed a read with. Only true
         * locals, always assigned before use, are promoted. */
        ok[L] = 1;
        /* A float or double promotes too. The temp it becomes is still
         * read by SSE instructions, which take their operand from a
         * slot, and the register allocator marks anything a float op
         * touches as ineligible -- so it does not end up in a register
         * and nothing about codegen changes. What IS gained is that
         * folding, value numbering and copy propagation can finally see
         * through it: a double loaded twice becomes one load, and a
         * value carried across a branch stops being a store and a
         * reload. 188 locals in lib/libc and lib/libcxx were refused
         * here for being neither an integer nor a pointer. */
        int promotable = Li->is_scalar_int_or_ptr || Li->is_scalar_float;
        if (L < nparams)                        { ok[L] = 0; why[L] = "is-a-parameter"; }
        else if (!Li->size)                     { ok[L] = 0; why[L] = "type-unknown"; }
        else if (!promotable && (Li->size == 4 || Li->size == 8))
                                                { ok[L] = 0; why[L] = "not-a-scalar-integer-pointer-or-float"; }
        else if (!promotable)                   { ok[L] = 0; why[L] = "not-4-or-8-bytes"; }
    }
    for (int L = 0; L < nvars; L++)
        if (ok[L] && fn->locals[L].is_volatile) {
            ok[L] = 0;                          /* volatile: every access must stay */
            why[L] = "declared-volatile";
        }
    for (int i = 0; i < fn->nins; i++) {
        struct ir_ins *in = &fn->ins[i];
        if (in->op == IR_ADDR && in->a >= 0 && in->a < nvars && ok[in->a]) {
            ok[in->a] = 0; why[in->a] = "address-is-taken";
        }
        if (in->op == IR_LDVAR && in->a >= 0 && in->a < nvars && ok[in->a] &&
            (in->vol || !m2r_plain(in->size, in->sign, in->w))) {
            ok[in->a] = 0;
            why[in->a] = in->vol ? "read-is-volatile"
                                 : "read-is-partial-or-extending";
        }
        if (in->op == IR_STVAR && in->dst >= 0 && in->dst < nvars &&
            in->vol && ok[in->dst]) {
            ok[in->dst] = 0; why[in->dst] = "write-is-volatile";
        }
    }
    if (remarks_on())
        for (int L = nparams; L < nvars; L++) {
            const struct ir_dbgvar *v = local_var(fn, L);
            if (!v || !v->name || v->name[0] == '<')  /* a compiler-invented name */
                continue;
            const char *file = fn->src ? fn->file : NULL;
            int line = v->line ? v->line : (fn->src ? fn->line : 0);
            if (ok[L])
                remark_add("mem2reg", "promoted-to-register", v->name,
                           "scalar-and-never-addressed", file, line, NULL);
            else
                remark_add("mem2reg", "kept-in-memory", v->name,
                           why[L] ? why[L] : "unknown", file, line, NULL);
        }
    free(why);
    int nprom = 0;
    int *prom = xmalloc((size_t)nvars * sizeof *prom);     /* local -> prom idx */
    int *ploc = xmalloc((size_t)nvars * sizeof *ploc);     /* prom idx -> local */
    for (int L = 0; L < nvars; L++)
        prom[L] = ok[L] ? (ploc[nprom] = L, nprom++) : -1;
    free(ok);
    if (nprom == 0) { free(prom); free(ploc); return 0; }

    /* 2. CFG + dominance. */
    int nbb, *l2b;
    struct bb *bb = build_cfg(fn, &nbb, &l2b);
    int *order = xmalloc((size_t)nbb * sizeof *order), norder;
    compute_rpo(bb, nbb, order, &norder);
    if (norder != nbb) {   /* unreachable blocks: bail rather than mis-dominate */
        free(order); free(l2b);
        for (int i = 0; i < nbb; i++) free(bb[i].pred);
        free(bb); free(prom); free(ploc); return 0;
    }
    compute_idom(bb, order, norder);
    int **df = xcalloc((size_t)nbb, sizeof *df);
    int *ndf = xcalloc((size_t)nbb, sizeof *ndf);
    compute_df(bb, nbb, df, ndf);

    /* 3. Phi insertion at the iterated dominance frontier of each var's defs. */
    char *hasphi = xcalloc((size_t)nbb * (size_t)nprom, 1);
    int *work = xmalloc((size_t)nbb * sizeof *work);
    for (int pidx = 0; pidx < nprom; pidx++) {
        int L = ploc[pidx], nw = 0;
        char *ondef = xcalloc((size_t)nbb, 1);
        for (int bI = 0; bI < nbb; bI++)
            for (int i = bb[bI].start; i < bb[bI].end; i++)
                if (fn->ins[i].op == IR_STVAR && fn->ins[i].dst == L) {
                    if (!ondef[bI]) { ondef[bI] = 1; work[nw++] = bI; }
                    break;
                }
        while (nw) {
            int x = work[--nw];
            for (int j = 0; j < ndf[x]; j++) {
                int d = df[x][j];
                if (hasphi[d * nprom + pidx]) continue;
                hasphi[d * nprom + pidx] = 1;
                bb[d].phi_local = xrealloc(bb[d].phi_local, (size_t)(bb[d].nphi+1)*sizeof(int));
                bb[d].phi_res   = xrealloc(bb[d].phi_res,   (size_t)(bb[d].nphi+1)*sizeof(int));
                bb[d].phi_local[bb[d].nphi] = L;
                bb[d].phi_res[bb[d].nphi] = fn->nvregs++;
                bb[d].nphi++;
                if (!ondef[d]) { ondef[d] = 1; work[nw++] = d; }
            }
        }
        free(ondef);
    }
    for (int b = 0; b < nbb; b++) if (bb[b].nphi) {
        bb[b].phi_inc = xcalloc((size_t)bb[b].npred, sizeof *bb[b].phi_inc);
        for (int k = 0; k < bb[b].npred; k++)
            bb[b].phi_inc[k] = xmalloc((size_t)bb[b].nphi * sizeof(int));
    }

    /* 4. Rename down the dominator tree. Each prom has a version stack; an entry
     * "undef" temp (0) gives an uninitialised read a defined value. */
    int *undef = xmalloc((size_t)nprom * sizeof *undef);
    int **stk = xmalloc((size_t)nprom * sizeof *stk);
    int *sp = xcalloc((size_t)nprom, sizeof *sp);
    int *scap = xcalloc((size_t)nprom, sizeof *scap);
    for (int p = 0; p < nprom; p++) {
        undef[p] = fn->nvregs++;
        stk[p] = xmalloc(sizeof(int) * 8); scap[p] = 8;
        stk[p][sp[p]++] = undef[p];
    }
    /* explicit dominator-tree DFS (children = blocks whose idom is this block) */
    int *dstk = xmalloc((size_t)nbb * sizeof *dstk);
    int *dpushed = xcalloc((size_t)nbb * nprom, sizeof *dpushed); /* per (block,prom) */
    char *entered = xcalloc((size_t)nbb, 1);
    int dsp = 0; dstk[dsp++] = 0;
    while (dsp) {
        int b = dstk[dsp - 1];
        if (!entered[b]) {
            entered[b] = 1;
            /* phi defs become the current version */
            for (int k = 0; k < bb[b].nphi; k++) {
                int pidx = prom[bb[b].phi_local[k]];
                if (sp[pidx] == scap[pidx]) { scap[pidx]*=2; stk[pidx]=xrealloc(stk[pidx],(size_t)scap[pidx]*sizeof(int)); }
                stk[pidx][sp[pidx]++] = bb[b].phi_res[k];
                dpushed[b * nprom + pidx]++;
            }
            for (int i = bb[b].start; i < bb[b].end; i++) {
                struct ir_ins *in = &fn->ins[i];
                if (in->op == IR_LDVAR && in->a >= 0 && in->a < nvars && prom[in->a] >= 0) {
                    int pidx = prom[in->a];
                    int fw = in->size == 8 ? 8 : 4;
                    int was_float = in->flt;
                    in->op = IR_MOV; in->a = stk[pidx][sp[pidx]-1]; in->b = -1;
                    if (was_float) {
                        /* A copy of the BITS, at the variable's width.
                         * Left as a float move it would go through the
                         * SSE path, which loads from a slot the temp no
                         * longer has. */
                        in->flt = 0; in->sign = 0; in->size = 0; in->w = fw;
                    }
                } else if (in->op == IR_STVAR && in->dst >= 0 && in->dst < nvars && prom[in->dst] >= 0) {
                    int pidx = prom[in->dst];
                    if (sp[pidx] == scap[pidx]) { scap[pidx]*=2; stk[pidx]=xrealloc(stk[pidx],(size_t)scap[pidx]*sizeof(int)); }
                    stk[pidx][sp[pidx]++] = in->a;   /* the stored temp is the new version */
                    dpushed[b * nprom + pidx]++;
                    in->op = IR_MOV; in->dst = -1; in->a = -1;  /* mark: drop in rebuild */
                }
            }
            /* fill successors' phi incoming from this block */
            for (int s = 0; s < bb[b].nsucc; s++) {
                int sb = bb[b].succ[s];
                if (!bb[sb].nphi) continue;
                int pk = -1;
                for (int k = 0; k < bb[sb].npred; k++) if (bb[sb].pred[k]==b){pk=k;break;}
                for (int k = 0; k < bb[sb].nphi; k++) {
                    int pidx = prom[bb[sb].phi_local[k]];
                    bb[sb].phi_inc[pk][k] = stk[pidx][sp[pidx]-1];
                }
            }
            /* push dom-tree children */
            for (int c = 0; c < nbb; c++)
                if (c != 0 && bb[c].idom == b && !entered[c]) dstk[dsp++] = c;
        } else {
            /* leaving b: pop its versions */
            for (int p = 0; p < nprom; p++) sp[p] -= dpushed[b * nprom + p];
            dsp--;
        }
    }

    /* 5. Rebuild the linear IR out of SSA. */
    struct ibuf nb = { 0, 0, 0 };
    for (int p = 0; p < nprom; p++) {   /* entry undef defs */
        struct ir_ins *c = ib_push(&nb);
        c->op = IR_CONST; c->dst = undef[p]; c->imm = 0;
        c->w = fn->locals[ploc[p]].size == 8 ? 8 : 4;
        /* The seed for a variable read before it is written: it stands for
         * a value the program never produced, so it corresponds to no
         * source construct at all. The §9.1 exception, marked so the
         * verifier can tell it from a location a pass forgot to copy. */
        c->synth = 1;
    }
    struct { int lbl, from, edge_pred; } *tramp = NULL; int ntramp = 0, ctramp = 0;
    for (int b = 0; b < nbb; b++) {
        int hasterm = bb[b].end > bb[b].start;
        enum ir_op top = hasterm ? fn->ins[bb[b].end - 1].op : IR_UD2;
        int isterm = top == IR_JMP || top == IR_BRZ || top == IR_BRNZ ||
                     top == IR_RET || top == IR_UD2;
        int body_end = (hasterm && isterm) ? bb[b].end - 1 : bb[b].end;
        for (int i = bb[b].start; i < body_end; i++)
            if (!(fn->ins[i].op == IR_MOV && fn->ins[i].dst < 0))   /* dropped store */
                *ib_push(&nb) = fn->ins[i];
        if (top == IR_RET || top == IR_UD2) {
            if (isterm) *ib_push(&nb) = fn->ins[bb[b].end - 1];
        } else if (top == IR_JMP && isterm) {
            emit_edge_copies(&nb, bb, bb[b].succ[0], b, fn);
            *ib_push(&nb) = fn->ins[bb[b].end - 1];
        } else if ((top == IR_BRZ || top == IR_BRNZ) && isterm) {
            struct ir_ins br = fn->ins[bb[b].end - 1];   /* branch-taken = succ[0] */
            int taken = bb[b].succ[0];
            if (bb[taken].nphi) {                        /* trampoline the taken edge */
                int Lt = fn->nlabels++;
                if (ntramp == ctramp) { ctramp = ctramp?ctramp*2:8;
                    tramp = xrealloc(tramp, (size_t)ctramp*sizeof *tramp); }
                tramp[ntramp].lbl = Lt; tramp[ntramp].from = b;
                tramp[ntramp].edge_pred = taken; ntramp++;
                br.label = Lt;
            }
            *ib_push(&nb) = br;
            if (bb[b].nsucc > 1)                         /* fall-through copies (inline) */
                emit_edge_copies(&nb, bb, bb[b].succ[1], b, fn);
        } else {   /* falls through to the next block */
            if (bb[b].nsucc > 0)
                emit_edge_copies(&nb, bb, bb[b].succ[0], b, fn);
        }
    }
    /* The last real block may fall off the end — an implicit return that the
     * original linear IR carries no explicit RET for (codegen returns at the
     * function's physical end). Trampoline blocks are appended next, so a
     * fall-through last block would run straight into one. Cap it with a void
     * RET. Only a block that does NOT end in an unconditional jump/ret can
     * reach the next physical instruction, so only those need the cap. */
    if (ntramp > 0 && nbb > 0) {
        enum ir_op lt = bb[nbb - 1].end > bb[nbb - 1].start
                        ? fn->ins[bb[nbb - 1].end - 1].op : IR_UD2;
        if (lt != IR_JMP && lt != IR_RET && lt != IR_UD2) {
            struct ir_ins *r = ib_push(&nb);
            r->op = IR_RET; r->a = -1;
            /* A cap so the trampolines below cannot be fallen into: it
             * stands for no `return` the programmer wrote (R3, §9.1). */
            r->synth = 1;
        }
    }
    /* A trampoline block exists because SSA had to be undone on one edge.
     * Its label and its jump are the compiler's own; the copies between
     * them take the edge's location in emit_edge_copies. */
    for (int t = 0; t < ntramp; t++) {   /* trampoline blocks: label; copies; jmp */
        struct ir_ins *lb = ib_push(&nb);
        lb->op = IR_LABEL; lb->label = tramp[t].lbl;
        lb->synth = 1;
        emit_edge_copies(&nb, bb, tramp[t].edge_pred, tramp[t].from, fn);
        struct ir_ins *jp = ib_push(&nb);
        int origlbl = -1;
        for (int i = bb[tramp[t].edge_pred].start; i < bb[tramp[t].edge_pred].end; i++)
            if (fn->ins[i].op == IR_LABEL) { origlbl = fn->ins[i].label; break; }
        jp->op = IR_JMP; jp->label = origlbl;
        jp->synth = 1;
    }
    free(tramp);

    free(fn->ins);
    fn->ins = nb.p; fn->nins = nb.n; fn->cap = nb.cap;

    /* The rebuild renumbered every instruction, so the local scope ranges (which
     * are instruction indices, used by codegen to coalesce disjoint-lifetime
     * locals) are now stale. Drop them: codegen then gives each surviving local
     * its own slot — correct, if a touch larger. Promoted locals are dead. */
    if (fn->var_scope_lo) {
        free(fn->var_scope_lo); free(fn->var_scope_hi);
        fn->var_scope_lo = fn->var_scope_hi = NULL;
    }

    for (int p = 0; p < nprom; p++) free(stk[p]);
    free(undef); free(stk); free(sp); free(scap);
    free(dstk); free(dpushed); free(entered);
    free(hasphi); free(work); free(prom); free(ploc);
    mem2reg_free(bb, nbb, df, ndf, l2b, order);
    return 1;
}

/* ---- global common-subexpression elimination (dominator-scoped VN) ----
 *
 * pass_lvn reuses an identical computation only within a block. Global CSE
 * carries a value across the dominator tree: a value computed in a block is
 * available to every block that block dominates. Sound because the operand
 * temps are single-assignment (equal temps => equal value) and the producing
 * temp, defined in a dominator, is live on every path to the reuse. Only
 * position-independent, non-memory pure ops are numbered — arithmetic, compares,
 * extends, address computations, constants; a memory read (LDVAR/LOAD) depends
 * on a store history that crosses blocks, so pass_lvn keeps those local. */
static int gcse_numberable(enum ir_op op)
{
    switch (op) {
    /* Only genuinely COMPUTED values. A cheap single-instruction
     * materialization (CONST, a lea for &local/&global/string/func, a
     * sign/zero-extend, a bswap) costs less to recompute than to keep live
     * across the dominated region — global-CSEing those only lengthens a live
     * range (forcing a spill or a callee-saved reg) for no win. Redundant
     * arithmetic/compares are the profitable case. Memory reads (LDVAR/LOAD)
     * stay with the memory-versioned pass_lvn. */
    case IR_ADD: case IR_SUB: case IR_MUL:
    case IR_DIV: case IR_MOD: case IR_AND: case IR_OR: case IR_XOR:
    case IR_SHL: case IR_SHR: case IR_CMP: case IR_NEG: case IR_BNOT:
        return 1;
    default:
        return 0;
    }
}

static int pass_gcse(struct ir_func *fn)
{
    if (fn->nins == 0)
        return 0;
    int nbb, *l2b;
    struct bb *bb = build_cfg(fn, &nbb, &l2b);
    int *order = xmalloc((size_t)nbb * sizeof *order), norder;
    compute_rpo(bb, nbb, order, &norder);
    if (norder != nbb) {   /* unreachable blocks: dominance is not total, bail */
        free(order); free(l2b);
        for (int i = 0; i < nbb; i++) free(bb[i].pred);
        free(bb); return 0;
    }
    compute_idom(bb, order, norder);
    struct defs dfs;
    compute_defs(fn, &dfs);

    /* An active table holding the current block's and its dominators' values,
     * pushed on enter and truncated back on leave — an explicit dom-tree DFS so
     * siblings never see each other's values (they do not dominate each other). */
    struct vn *tab = NULL; int ntab = 0, captab = 0, changed = 0;
    int *dstk = xmalloc((size_t)nbb * sizeof *dstk);
    int *mark = xmalloc((size_t)nbb * sizeof *mark);
    char *entered = xcalloc((size_t)nbb, 1);
    int dsp = 0; dstk[dsp++] = 0;
    while (dsp) {
        int b = dstk[dsp - 1];
        if (!entered[b]) {
            entered[b] = 1;
            mark[b] = ntab;
            for (int n = bb[b].start; n < bb[b].end; n++) {
                struct ir_ins *i = &fn->ins[n];
                struct vn k;
                /* vn_stable: a key made of vreg numbers names a value
                 * only where a vreg names one. Dominance is not enough
                 * on its own -- the guard of a rotated loop dominates
                 * its latch, and the induction variable is a different
                 * value at each. */
                if (i->dst < 0 || !gcse_numberable(i->op) ||
                    !vn_stable(fn, &dfs, i) || !vn_key(i, 0, &k))
                    continue;
                int hit = -1;
                for (int t = 0; t < ntab; t++)
                    if (vn_eq(&tab[t], &k)) { hit = tab[t].result; break; }
                if (hit >= 0 && hit != i->dst) {
                    to_mov(i, hit); changed = 1; g_did.gcse++;
                } else if (hit < 0) {
                    if (ntab == captab) { captab = captab ? captab * 2 : 64;
                        tab = xrealloc(tab, (size_t)captab * sizeof *tab); }
                    k.result = i->dst; tab[ntab++] = k;
                }
            }
            for (int c = 0; c < nbb; c++)
                if (c != 0 && bb[c].idom == b && !entered[c]) dstk[dsp++] = c;
        } else {
            ntab = mark[b];    /* leaving b: drop its (and its subtree's) values */
            dsp--;
        }
    }
    free(tab); free(dstk); free(mark); free(entered);
    free_defs(&dfs);
    free(order); free(l2b);
    for (int i = 0; i < nbb; i++) free(bb[i].pred);
    free(bb);
    return changed;
}


/* ==== alias analysis ======================================================== *
 *
 * "Can these two memory references be the same bytes?" Every pass that
 * moves, reuses or deletes a memory operation needs the answer, and
 * until now there was none: pass_loadcse's kill model is "a store to a
 * non-address-taken local kills that local's LDVARs; anything else
 * kills every LOAD and every address-taken local's LDVAR". One store
 * through one pointer invalidated every cached load in the function.
 *
 * What makes a cheap answer possible is C's object model. A pointer
 * derived from one object does not point into another, so if two
 * references are based on DIFFERENT objects they cannot overlap --
 * whatever indexing happened in between. So the question becomes "what
 * object is this address based on", which is a walk back through the
 * address arithmetic:
 *
 *      %5 = gaddr @arr          <- the object
 *      %9 = shl %i, #2
 *      %10 = add %5, %9         <- still @arr
 *      load [%10]
 *
 * Three answers are possible: a named global, a numbered frame slot, or
 * unknown. The useful part is the last rule below -- an unknown pointer
 * cannot reach a slot whose address was never taken, because there is
 * no way for the program to have obtained one.
 *
 * What this deliberately does NOT do: type-based aliasing (EmbIR does
 * not carry the type of a load), `restrict`, and field sensitivity.
 * Those want metadata on the reference, which is the next step rather
 * than this one. */

enum mem_kind { MEM_UNKNOWN, MEM_GLOBAL, MEM_SLOT };

struct memref {
    enum mem_kind kind;
    int id;                  /* MEM_GLOBAL: the symbol; MEM_SLOT: the var */
};

/* The object an address is based on, or MEM_UNKNOWN. Walks back through
 * the arithmetic irgen builds for `a[i]` and `p->f`, which by C's rules
 * cannot leave the object it started from. */
static struct memref mem_base(struct ir_func *fn, struct defs *d, int addr)
{
    struct memref r = { MEM_UNKNOWN, -1 };
    for (int hop = 0; hop < 8; hop++) {         /* a chain, not a cycle */
        if (addr < 0 || addr >= fn->nvregs || d->cnt[addr] != 1)
            return r;
        int n = d->ins[addr];
        if (n < 0)
            return r;                           /* a parameter: unknown */
        struct ir_ins *i = &fn->ins[n];
        switch (i->op) {
        case IR_GADDR:
            r.kind = MEM_GLOBAL; r.id = i->glob_sym; return r;
        case IR_ADDR:
            r.kind = MEM_SLOT; r.id = i->a; return r;
        case IR_MOV:
            addr = i->a; continue;
        case IR_ADD: case IR_SUB: {
            /* base + offset, either way round. Only one side can be a
             * base; if both resolve, the answer is not decidable here. */
            struct memref A = { MEM_UNKNOWN, -1 };
            if (i->a >= 0) {
                int an = i->a < fn->nvregs && d->cnt[i->a] == 1 ? d->ins[i->a] : -1;
                if (an >= 0 && (fn->ins[an].op == IR_GADDR ||
                                fn->ins[an].op == IR_ADDR ||
                                fn->ins[an].op == IR_MOV ||
                                fn->ins[an].op == IR_ADD))
                    A = mem_base(fn, d, i->a);
            }
            if (A.kind != MEM_UNKNOWN)
                return A;
            if (i->op == IR_SUB || i->imm_b)
                return r;                       /* offset - base is not a base */
            addr = i->b; continue;
        }
        default:
            return r;
        }
    }
    return r;
}

/* Can a reference based on `a` and one based on `b` be the same bytes? */
static int may_alias(struct memref a, struct memref b, const char *taken,
                     int nvars)
{
    if (a.kind == MEM_GLOBAL && b.kind == MEM_GLOBAL)
        return a.id == b.id;            /* two globals do not overlap */
    if (a.kind == MEM_SLOT && b.kind == MEM_SLOT)
        return a.id == b.id;            /* nor two frame slots */
    if ((a.kind == MEM_GLOBAL && b.kind == MEM_SLOT) ||
        (a.kind == MEM_SLOT && b.kind == MEM_GLOBAL))
        return 0;                       /* nor a global and a slot */
    /* One of them is unknown. It can be anything the program could have
     * taken the address of -- which a slot whose address was never
     * taken is not. */
    struct memref k = a.kind == MEM_UNKNOWN ? b : a;
    if (k.kind == MEM_SLOT && k.id >= 0 && k.id < nvars && !taken[k.id])
        return 0;
    return 1;
}



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

/* Replace 32-bit `x / C` and `x % C` with a multiply and shifts. */
static int pass_divmagic(struct ir_func *fn)
{
    if (fn->nins == 0)
        return 0;
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
        if ((src->op != IR_DIV && src->op != IR_MOD) || src->flt ||
            src->w != 4 || src->dst < 0 || !const_b(fn, &d, src, &D) ||
            D == 0 || D == 1 || D == -1) {
            *ib_push(&nb) = *src;
            continue;
        }
        int is_mod = src->op == IR_MOD, sg = src->sign, x = src->a;
        /* An unsigned power of two is already a shift by the time this
         * runs; a signed one is not, and its bias is cheaper than a
         * multiply, so take it here. */
        int q;
        if (sg && log2_pow2_l((unsigned long)(D < 0 ? -D : D)) >= 0) {
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
            int t3 = dm_op(&nb, fn, IR_SHR, hi,
                           dm_const(&nb, fn, 32, 8, src), 8, 1, 0, src);
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
            int t3 = dm_op(&nb, fn, IR_SHR, hi,
                           dm_const(&nb, fn, 32, 8, src), 8, 0, 0, src);
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
    if (n < 0)
        return 8;                       /* a parameter, full width */
    int w = fn->ins[n].w;
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
    for (int i = 0; i < nbb; i++) free(bb[i].pred);
    free(bb);
    return done;
}

static int pass_ifconv(struct ir_func *fn)
{
    int changed = 0, guard = 0;
    while (guard++ < 64 && ifconv_one(fn))
        changed = 1;
    return changed;
}


/* ---- CFG cleanup: jump threading and block merging -----------------
 *
 * Three shapes that irgen and the loop passes leave behind, none of
 * which any existing pass removes:
 *
 *   A jump to a jump. `goto L1` where L1 holds only `goto L2` should go
 *   straight to L2. Rotation and if-conversion both create these when
 *   they delete the block in between.
 *
 *   A branch whose two arms are the same label. After SCCP folds one
 *   side of a condition the two edges can converge, and the branch is
 *   then a jump -- but the condition is still computed and tested.
 *
 *   A jump to the very next instruction, which is nothing at all.
 *
 * They are worth removing for their own sake and because every pass
 * that reasons about blocks gets a smaller graph: a label with no
 * remaining jumps to it stops splitting a block, so the block-local
 * passes see more at once. */
static int pass_cfgclean(struct ir_func *fn)
{
    if (fn->nins == 0 || fn->nlabels == 0)
        return 0;
    int changed = 0;

    /* Where each label sits, and whether the block it opens is nothing
     * but an unconditional jump elsewhere. */
    int *at = xmalloc((size_t)fn->nlabels * sizeof *at);
    int *fwd = xmalloc((size_t)fn->nlabels * sizeof *fwd);
    for (int l = 0; l < fn->nlabels; l++) { at[l] = -1; fwd[l] = -1; }
    for (int n = 0; n < fn->nins; n++)
        if (fn->ins[n].op == IR_LABEL && fn->ins[n].label >= 0 &&
            fn->ins[n].label < fn->nlabels)
            at[fn->ins[n].label] = n;
    for (int l = 0; l < fn->nlabels; l++) {
        int n = at[l];
        if (n < 0) continue;
        /* skip any further labels on the same spot */
        while (n + 1 < fn->nins && fn->ins[n + 1].op == IR_LABEL) n++;
        if (n + 1 < fn->nins && fn->ins[n + 1].op == IR_JMP)
            fwd[l] = fn->ins[n + 1].label;
    }
    /* Follow the chains, with a bound: `L: goto L` is a real loop and
     * must not be collapsed into itself. */
    for (int l = 0; l < fn->nlabels; l++) {
        int t = fwd[l], hops = 0;
        while (t >= 0 && t < fn->nlabels && fwd[t] >= 0 && fwd[t] != t &&
               hops++ < 16)
            t = fwd[t];
        fwd[l] = t;
    }
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        if (i->op != IR_JMP && i->op != IR_BRZ && i->op != IR_BRNZ)
            continue;
        int l = i->label;
        if (l < 0 || l >= fn->nlabels || fwd[l] < 0 || fwd[l] == l)
            continue;
        i->label = fwd[l];
        changed = 1;
    }

    /* A conditional branch whose taken target is the fall-through is a
     * jump; so is one whose two arms are the same label. */
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        if (i->op != IR_BRZ && i->op != IR_BRNZ)
            continue;
        int m = n + 1;
        while (m < fn->nins && fn->ins[m].op == IR_LABEL) {
            if (fn->ins[m].label == i->label) {
                i->op = IR_JMP; i->a = -1; changed = 1;
                break;
            }
            m++;
        }
    }

    /* Drop a jump to the label that immediately follows it, and any
     * instruction that can never be reached: after an unconditional
     * transfer, until the next label. */
    char *dead = xcalloc((size_t)fn->nins, 1);
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        if (i->op == IR_JMP) {
            int m = n + 1;
            while (m < fn->nins && fn->ins[m].op == IR_LABEL) {
                if (fn->ins[m].label == i->label) { dead[n] = 1; break; }
                m++;
            }
        }
        if ((i->op == IR_JMP && !dead[n]) || i->op == IR_RET ||
            i->op == IR_UD2) {
            for (int m = n + 1; m < fn->nins; m++) {
                if (fn->ins[m].op == IR_LABEL)
                    break;
                dead[m] = 1;
            }
        }
    }
    int ndead = 0;
    for (int n = 0; n < fn->nins; n++) ndead += dead[n];
    if (ndead) {
        int *newpos = fn->var_scope_lo
            ? xmalloc((size_t)(fn->nins + 1) * sizeof *newpos) : NULL;
        int j = 0;
        for (int n = 0; n < fn->nins; n++) {
            if (newpos) newpos[n] = j;
            if (dead[n]) continue;
            if (j != n) fn->ins[j] = fn->ins[n];
            j++;
        }
        if (newpos) {
            newpos[fn->nins] = j;
            for (int v = 0; v < fn->nvars; v++) {
                int lo = fn->var_scope_lo[v], hi = fn->var_scope_hi[v];
                if (lo >= 0 && lo <= fn->nins) fn->var_scope_lo[v] = newpos[lo];
                if (hi >= 0 && hi <= fn->nins) fn->var_scope_hi[v] = newpos[hi];
            }
            free(newpos);
        }
        fn->nins = j;
        changed = 1;
    }
    g_did.cfgclean += changed;
    free(dead); free(at); free(fwd);
    return changed;
}


/* ---- tail recursion into a loop -------------------------------------
 *
 * `return f(args)` where f is the function itself does not need a call
 * at all: assign the parameters and jump back to the top. Sibling calls
 * already stop the stack growing for this shape on x86-64, but a jump
 * is cheaper still -- no argument marshalling through the ABI
 * registers, no return address, and the body becomes a real loop that
 * LICM, rotation and strength reduction can then work on. That last
 * part is most of the value: a recursive function is opaque to every
 * loop pass, and a loop is not.
 *
 * The rewrite is the obvious one, and the ORDER inside it is the whole
 * correctness question:
 *
 *      f(a, b):                  f(a, b):
 *        ...                   L: ...
 *        return f(x, y)          t0 = x; t1 = y      <- read all
 *                                a = t0; b = t1      <- then write all
 *                                goto L
 *
 * Read-all-then-write-all, because `return f(b, a)` swaps its arguments
 * and assigning them one at a time would give f(b, b).
 *
 * Refused when a parameter's address escapes: the recursive call gets a
 * fresh frame and this does not, so anything holding a pointer to a
 * parameter would see it change underneath. */
/* Is the call at `n` a tail call to this function, and if so how much
 * of what follows belongs to it?
 *
 * The result rarely flows straight into the `ret`. irgen forwards it
 * through the temp the expression was assigned to, and the `ret` itself
 * usually sits after a LABEL that the other arm of the conditional also
 * jumps to:
 *
 *      %15 = call @self(...)
 *      %2  = mov %15          <- ours to delete
 *   L1:                       <- shared: the other arm jumps here
 *      ret %2                 <- shared: must stay
 *
 * So the forwarding moves before the next label are ours, and anything
 * from the label on is not. Fills *ndel with how many instructions
 * after the call to drop. */
static int tailrec_at(struct ir_func *fn, struct func *f, int n, int np,
                      int *ndel)
{
    struct ir_ins *c = &fn->ins[n];
    if (c->op != IR_CALL || c->indirect || c->callee != f ||
        c->nargs != np || c->retsize || c->call_varargs || c->dst < 0)
        return 0;
    for (int k = 0; k < np; k++)
        if (c->argv[k].is_struct || c->argv[k].on_stack ||
            c->argv[k].is_float || c->argv[k].is_int128 || c->argv[k].byref)
            return 0;
    int cur = c->dst, j = n + 1, del = 0;
    while (j < fn->nins && fn->ins[j].op == IR_MOV &&
           fn->ins[j].a == cur && fn->ins[j].dst >= 0 && !fn->ins[j].vol) {
        cur = fn->ins[j].dst; j++; del++;
    }
    int k = j;
    while (k < fn->nins && fn->ins[k].op == IR_LABEL)
        k++;
    if (k >= fn->nins || fn->ins[k].op != IR_RET || fn->ins[k].a != cur)
        return 0;
    /* Nothing may read the forwarded value except that return -- if the
     * other arm's path also reads it, it reads ITS own value, and the
     * label between us means we cannot tell the two apart. */
    for (int m = 0; m < fn->nins; m++) {
        if (m > n && m <= k)
            continue;
        if (ins_reads(&fn->ins[m], cur))
            return 0;
    }
    *ndel = del;
    return 1;
}

static int pass_tailrec(struct ir_func *fn)
{
    struct func *f = fn->src;
    if (!f || fn->nins == 0 || fn->is_varargs || fn->has_alloca || fn->neh)
        return 0;
    int np = fn->nparams;
    if (np > MAX_PARAMS)
        return 0;

    /* A parameter whose address is taken cannot be reassigned here. */
    for (int n = 0; n < fn->nins; n++)
        if (fn->ins[n].op == IR_ADDR && fn->ins[n].a >= 0 &&
            fn->ins[n].a < np)
            return 0;

    int found = 0, junk;
    for (int n = 0; n < fn->nins; n++)
        if (tailrec_at(fn, f, n, np, &junk)) { found = 1; break; }
    if (!found)
        return 0;

    int Ltop = fn->nlabels++;
    struct ibuf nb = { 0, 0, 0 };
    int *newpos = fn->var_scope_lo
        ? xmalloc((size_t)(fn->nins + 1) * sizeof *newpos) : NULL;
    int changed = 0;
    {   /* the loop's head, before anything the body does */
        struct ir_ins *l = ib_push(&nb);
        l->op = IR_LABEL; l->label = Ltop; l->synth = 1;
        l->line = fn->line;
    }
    for (int n = 0; n < fn->nins; n++) {
        if (newpos) newpos[n] = nb.n;
        struct ir_ins *c = &fn->ins[n];
        int ndel = 0;
        if (tailrec_at(fn, f, n, np, &ndel)) {
            int argv[MAX_PARAMS], tmp[MAX_PARAMS];
            for (int k = 0; k < np; k++)
                argv[k] = c->argv[k].vreg;
            for (int k = 0; k < np; k++) {          /* read all */
                tmp[k] = fn->nvregs++;
                struct ir_ins *m = ib_push(&nb);
                m->op = IR_MOV; m->dst = tmp[k]; m->a = argv[k];
                m->w = fn->locals[k].size == 8 ? 8 : 4;
                m->line = c->line; m->col = c->col; m->synth = 1;
            }
            for (int k = 0; k < np; k++) {          /* then write all */
                struct ir_ins *st = ib_push(&nb);
                st->op = IR_STVAR; st->dst = k; st->a = tmp[k];
                st->size = fn->locals[k].size;
                st->line = c->line; st->col = c->col; st->synth = 1;
            }
            struct ir_ins *j = ib_push(&nb);
            j->op = IR_JMP; j->label = Ltop;
            j->line = c->line; j->col = c->col; j->synth = 1;
            n += ndel;                  /* the forwarding moves go too */
            changed = 1;
            continue;
        }
        *ib_push(&nb) = *c;
    }
    if (!changed) { free(nb.p); free(newpos); fn->nlabels--; return 0; }
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
    g_did.tailrec++;
    return 1;
}

/* ---- dead store elimination ----------------------------------------
 *
 * The inverse of store forwarding: a store whose value nothing reads
 * because a later store to the same bytes comes first.
 *
 *      p->x = 1;        <- dead
 *      p->x = 2;
 *
 * DCE already drops a store to a local that is never read AT ALL. This
 * is the other case, and it needs the alias analysis above: between the
 * two stores there must be no read that could see the first one, and
 * "could see" is exactly may_alias.
 *
 * Block-local and backward. Block-local because a store dead on one
 * path out of a block is not dead on another, and proving otherwise
 * wants the available-expressions machinery load elimination has --
 * this is the cheap half. Backward because "is there a later store"
 * is the question, and walking backward makes it "have I already seen
 * one".
 *
 * Two stores kill each other only when they are to the SAME address
 * temp at the same width. That is must-alias, not may-alias: a wrong
 * answer here deletes a write the program made. */
static int pass_dse(struct ir_func *fn)
{
    if (fn->nins == 0)
        return 0;
    int nvars = fn->nvars;
    struct defs d;
    compute_defs(fn, &d);
    int nbb, *l2b;
    struct bb *bb = build_cfg(fn, &nbb, &l2b);

    char *taken = xcalloc((size_t)(nvars ? nvars : 1), 1);
    for (int i = 0; i < fn->nins; i++)
        if (fn->ins[i].op == IR_ADDR && fn->ins[i].a >= 0 &&
            fn->ins[i].a < nvars)
            taken[fn->ins[i].a] = 1;

    char *dead = xcalloc((size_t)fn->nins, 1);
    /* Stores seen later in this block, as (address temp, width). A slot
     * store records its var with a negative marker so the two kinds
     * share one list. */
    int *sa = xmalloc((size_t)fn->nins * sizeof *sa);
    int *ssz = xmalloc((size_t)fn->nins * sizeof *ssz);
    int changed = 0;

    for (int b = 0; b < nbb; b++) {
        int ns = 0;
        for (int i = bb[b].end - 1; i >= bb[b].start; i--) {
            struct ir_ins *ins = &fn->ins[i];
            if (ins->op == IR_STORE && !ins->vol && ins->a >= 0 &&
                ins->a < fn->nvregs && d.cnt[ins->a] == 1) {
                int killed = 0;
                for (int k = 0; k < ns; k++)
                    if (sa[k] == ins->a && ssz[k] == ins->size) { killed = 1; break; }
                if (killed) { dead[i] = 1; changed = 1; continue; }
                sa[ns] = ins->a; ssz[ns] = ins->size; ns++;
                continue;
            }
            if (ins->op == IR_STVAR && !ins->vol && ins->dst >= 0 &&
                ins->dst < nvars && !taken[ins->dst]) {
                int killed = 0;
                for (int k = 0; k < ns; k++)
                    if (sa[k] == -1 - ins->dst && ssz[k] == ins->size) { killed = 1; break; }
                if (killed) { dead[i] = 1; changed = 1; continue; }
                sa[ns] = -1 - ins->dst; ssz[ns] = ins->size; ns++;
                continue;
            }
            /* A read that could see one of them un-kills it. A call,
             * inline asm, an atomic or a fence could see anything. */
            struct memref r = { MEM_UNKNOWN, -1 };
            int reads = 0, everything = 0;
            switch (ins->op) {
            case IR_LOAD:
                r = mem_base(fn, &d, ins->a); reads = 1; break;
            case IR_LDVAR:
                r.kind = MEM_SLOT; r.id = ins->a; reads = 1; break;
            case IR_MEMCPY:
                r = mem_base(fn, &d, ins->b); reads = 1; break;
            case IR_STORE: case IR_STVAR:
                reads = 0; everything = 1; break;   /* volatile or unkeyed */
            case IR_CALL: case IR_ASM: case IR_VA_START: case IR_FENCE:
            case IR_XCHG: case IR_XADD: case IR_CMPXCHG: case IR_ARMW:
            case IR_CAS: case IR_CAS16: case IR_MEMZERO: case IR_ALLOCA:
                everything = 1; break;
            default:
                break;
            }
            if (everything) { ns = 0; continue; }
            if (!reads)
                continue;
            int j = 0;
            for (int k = 0; k < ns; k++) {
                struct memref w;
                if (sa[k] < 0) { w.kind = MEM_SLOT; w.id = -1 - sa[k]; }
                else w = mem_base(fn, &d, sa[k]);
                if (!may_alias(w, r, taken, nvars)) { sa[j] = sa[k]; ssz[j] = ssz[k]; j++; }
            }
            ns = j;
        }
    }

    if (changed) {
        int *newpos = fn->var_scope_lo
            ? xmalloc((size_t)(fn->nins + 1) * sizeof *newpos) : NULL;
        int j = 0;
        for (int n = 0; n < fn->nins; n++) {
            if (newpos) newpos[n] = j;
            if (dead[n]) continue;
            if (j != n) fn->ins[j] = fn->ins[n];
            j++;
        }
        if (newpos) {
            newpos[fn->nins] = j;
            for (int v = 0; v < fn->nvars; v++) {
                int lo = fn->var_scope_lo[v], hi = fn->var_scope_hi[v];
                if (lo >= 0 && lo <= fn->nins) fn->var_scope_lo[v] = newpos[lo];
                if (hi >= 0 && hi <= fn->nins) fn->var_scope_hi[v] = newpos[hi];
            }
            free(newpos);
        }
        g_did.dse += fn->nins - j;
        fn->nins = j;
    }

    free(dead); free(sa); free(ssz); free(taken); free(l2b);
    for (int i = 0; i < nbb; i++) free(bb[i].pred);
    free(bb); free_defs(&d);
    return changed;
}

/* ---- global redundant-load elimination (available-expressions) ------------ *
 *
 * pass_gcse leaves memory reads (LDVAR/LOAD) to the block-local pass_lvn: their
 * value depends on a store history that crosses blocks, which dominance alone
 * cannot reason about (a store on a non-dominator-tree path still kills a load).
 * This pass does the real thing — an available-expressions dataflow. A load is
 * redundant at a point if an identical earlier load reaches it on EVERY path
 * with no intervening write that could alias it; the reload becomes a copy.
 *
 * Soundness rests on three things:
 *   - the meet is "same representative TEMP from all predecessors", so the reused
 *     value is one dominating definition (loads produce single-def temps), never
 *     a per-path phi;
 *   - a LOAD is keyed only when its address temp is single-def, so the address
 *     cannot change between the two loads;
 *   - the kill model separates a store to a non-address-taken local (kills only
 *     that local's LDVARs) from a real memory write / call / asm (kills every
 *     LOAD and every address-taken local's LDVAR — no finer alias analysis).
 */
struct lkey { enum ir_op op; int a, size, sign, w; };

static int lcse_kills_mem(enum ir_op op)
{
    switch (op) {
    case IR_STORE: case IR_CALL: case IR_MEMCPY: case IR_MEMZERO:
    case IR_XCHG: case IR_XADD: case IR_CMPXCHG: case IR_ARMW: case IR_CAS:
    case IR_CAS16: case IR_ASM: case IR_VA_START:
    case IR_FENCE:           /* a barrier: see writes_memory */
        return 1;
    default:
        return 0;
    }
}

/* Drop the cached loads a write could reach. A store names an object,
 * so only the keys that may alias it go; a call, inline asm, an atomic
 * or a fence names nothing, so everything reachable goes. */
static void lcse_kill(int *s, int nk, const char *is_mem,
                      const struct memref *kbase, struct ir_ins *ins,
                      struct ir_func *fn, struct defs *d, const char *taken,
                      int nvars)
{
    struct memref w = { MEM_UNKNOWN, -1 };
    int named = 0;
    if (ins->op == IR_STORE || ins->op == IR_MEMCPY ||
        ins->op == IR_MEMZERO) {
        w = mem_base(fn, d, ins->a);
        named = w.kind != MEM_UNKNOWN;
    }
    for (int k = 0; k < nk; k++) {
        if (!is_mem[k])
            continue;
        if (named && !may_alias(w, kbase[k], taken, nvars))
            continue;
        s[k] = -1;
    }
}

static int pass_loadcse(struct ir_func *fn)
{
    int nvars = fn->nvars;
    if (fn->nins == 0)
        return 0;
    struct defs d;
    compute_defs(fn, &d);
    int nbb, *l2b;
    struct bb *bb = build_cfg(fn, &nbb, &l2b);
    int *order = xmalloc((size_t)nbb * sizeof *order), norder;
    compute_rpo(bb, nbb, order, &norder);
    if (norder != nbb) {
        free(order); free(l2b);
        for (int i = 0; i < nbb; i++) free(bb[i].pred);
        free(bb); free_defs(&d); return 0;
    }

    char *taken = xcalloc((size_t)(nvars ? nvars : 1), 1);
    for (int i = 0; i < fn->nins; i++)
        if (fn->ins[i].op == IR_ADDR && fn->ins[i].a >= 0 && fn->ins[i].a < nvars)
            taken[fn->ins[i].a] = 1;

    /* Enumerate distinct load keys; keyidx[i] maps a load instruction to one. */
    struct lkey *keys = NULL; int nk = 0, capk = 0;
    int *keyidx = xmalloc((size_t)fn->nins * sizeof *keyidx);
    for (int i = 0; i < fn->nins; i++) {
        keyidx[i] = -1;
        struct ir_ins *in = &fn->ins[i];
        struct lkey k;
        if (in->op == IR_LDVAR && !in->vol && in->a >= 0 && in->a < nvars) {
            k = (struct lkey){ IR_LDVAR, in->a, in->size, in->sign, in->w };
        } else if (in->op == IR_LOAD && !in->vol && in->a >= 0 &&
                   in->a < fn->nvregs && d.cnt[in->a] == 1) {
            k = (struct lkey){ IR_LOAD, in->a, in->size, in->sign, in->w };
        } else {
            continue;
        }
        int found = -1;
        for (int j = 0; j < nk; j++)
            if (keys[j].op == k.op && keys[j].a == k.a && keys[j].size == k.size &&
                keys[j].sign == k.sign && keys[j].w == k.w) { found = j; break; }
        if (found < 0) {
            if (nk == capk) { capk = capk ? capk * 2 : 32;
                keys = xrealloc(keys, (size_t)capk * sizeof *keys); }
            keys[nk] = k; found = nk++;
        }
        keyidx[i] = found;
    }
    if (nk == 0) {
        free(order); free(l2b); free(taken); free(keyidx); free(keys);
        for (int i = 0; i < nbb; i++) free(bb[i].pred);
        free(bb); free_defs(&d); return 0;
    }
    /* is_mem[k]: a LOAD or an address-taken local's LDVAR (killed by any write).
     * A non-address-taken local's LDVAR is killed only by a store to that local. */
    char *is_mem = xmalloc((size_t)nk);
    for (int k = 0; k < nk; k++)
        is_mem[k] = keys[k].op == IR_LOAD ||
                    (keys[k].op == IR_LDVAR && taken[keys[k].a]);

    /* What object each cached load reads, so a write can kill only the
     * ones it could actually reach. Without this a single store through
     * a pointer dropped every cached load in the function. */
    struct memref *kbase = xmalloc((size_t)(nk ? nk : 1) * sizeof *kbase);
    for (int k = 0; k < nk; k++) {
        if (keys[k].op == IR_LDVAR) {
            kbase[k].kind = MEM_SLOT; kbase[k].id = keys[k].a;
        } else {
            kbase[k] = mem_base(fn, &d, keys[k].a);
        }
    }

    /* Dataflow. avail[b][k]: -2 top (init), -1 not available, >=0 the temp. */
    int *aout = xmalloc((size_t)nbb * (size_t)nk * sizeof *aout);
    int *ain  = xmalloc((size_t)nbb * (size_t)nk * sizeof *ain);
    for (int i = 0; i < nbb * nk; i++) aout[i] = -2;
    int *s = xmalloc((size_t)nk * sizeof *s);

    for (int iter = 0, changed = 1; changed && iter < nbb + 2; iter++) {
        changed = 0;
        for (int oi = 0; oi < nbb; oi++) {
            int b = order[oi];
            int *in = ain + (size_t)b * nk;
            if (b == 0) {
                for (int k = 0; k < nk; k++) in[k] = -1;   /* entry: empty */
            } else {
                for (int k = 0; k < nk; k++) in[k] = -2;    /* top */
                for (int p = 0; p < bb[b].npred; p++) {
                    int *po = aout + (size_t)bb[b].pred[p] * nk;
                    for (int k = 0; k < nk; k++) {
                        int m = in[k], v = po[k];        /* three-valued meet */
                        in[k] = (m == -1 || v == -1) ? -1
                              : (m == -2) ? v : (v == -2) ? m
                              : (m == v) ? m : -1;
                    }
                }
                for (int k = 0; k < nk; k++) if (in[k] == -2) in[k] = -1;
            }
            for (int k = 0; k < nk; k++) s[k] = in[k];
            for (int i = bb[b].start; i < bb[b].end; i++) {
                struct ir_ins *ins = &fn->ins[i];
                if (ins->op == IR_STVAR) {
                    for (int k = 0; k < nk; k++)
                        if ((keys[k].op == IR_LDVAR && keys[k].a == ins->dst) ||
                            (taken[ins->dst] && is_mem[k]))
                            s[k] = -1;
                } else if (lcse_kills_mem(ins->op)) {
                    lcse_kill(s, nk, is_mem, kbase, ins, fn, &d, taken, nvars);
                }
                int k = keyidx[i];
                if (k >= 0 && s[k] < 0) s[k] = ins->dst;   /* first def of the value */
            }
            int *out = aout + (size_t)b * nk;
            for (int k = 0; k < nk; k++)
                if (out[k] != s[k]) { out[k] = s[k]; changed = 1; }
        }
    }

    /* Replacement: replay each block from its (now stable) avail_in. */
    int changed = 0;
    for (int b = 0; b < nbb; b++) {
        int *in = ain + (size_t)b * nk;
        for (int k = 0; k < nk; k++) s[k] = in[k];
        for (int i = bb[b].start; i < bb[b].end; i++) {
            struct ir_ins *ins = &fn->ins[i];
            if (ins->op == IR_STVAR) {
                for (int k = 0; k < nk; k++)
                    if ((keys[k].op == IR_LDVAR && keys[k].a == ins->dst) ||
                        (taken[ins->dst] && is_mem[k]))
                        s[k] = -1;
            } else if (lcse_kills_mem(ins->op)) {
                lcse_kill(s, nk, is_mem, kbase, ins, fn, &d, taken, nvars);
            }
            int k = keyidx[i];
            if (k < 0) continue;
            if (s[k] >= 0 && s[k] != ins->dst) {
                to_mov(ins, s[k]); changed = 1; g_did.loadcse++;  /* redundant reload */
            } else if (s[k] < 0) {
                s[k] = ins->dst;
            }
        }
    }

    free(order); free(l2b); free(taken); free(keyidx); free(keys);
    free(is_mem); free(kbase); free(aout); free(ain); free(s);
    for (int i = 0; i < nbb; i++) free(bb[i].pred);
    free(bb); free_defs(&d);
    return changed;
}

/* ---- block-local copy propagation ----
 *
 * pass_copyprop is global, so it needs its source to be
 * single-assignment. The values that matter most in a loop are exactly
 * the ones that are not: mem2reg destructs a phi into copies, so an
 * induction variable and an accumulator are each assigned on every
 * incoming edge, and every read of one goes through a fresh `mov` that
 * the global pass may not touch. A loop counter therefore gets copied
 * three times per iteration -- once for the test, once for the body,
 * once for the increment -- and all three copies survive to codegen.
 *
 * Inside ONE block the question is easy. `%9 = mov %27` makes %9 and
 * %27 the same value until something redefines %27, and a block is
 * straight-line code, so "until" is just a position. Walking the block
 * with a table of current copies, cleared on every def, rewrites those
 * reads with no dominance reasoning at all. The table is dropped at each
 * block boundary, so a value carried around a back edge is never assumed
 * to survive one.
 *
 * The table is indexed by vreg and a frame slot is a vreg (see
 * each_read), which is safe here for the same reason it is safe there:
 * an entry is only ever made for a MOV's dst, and a MOV's dst is always
 * a temp, so a slot's entry stays empty and IR_LDVAR's slot operand is
 * never rewritten. */
struct lcopy { const int *cp; int nv, n; };
static void lcopy_cb(int *p, void *ctx)
{
    struct lcopy *c = ctx;
    if (*p >= 0 && *p < c->nv && c->cp[*p] >= 0) { *p = c->cp[*p]; c->n++; }
}

static int pass_copyprop_local(struct ir_func *fn)
{
    if (fn->nins == 0 || fn->nvregs == 0)
        return 0;
    int nbb, *l2b;
    struct bb *bb = build_cfg(fn, &nbb, &l2b);
    struct defs d;
    compute_defs(fn, &d);
    int *cp = xmalloc((size_t)fn->nvregs * sizeof *cp);
    int changed = 0;
    for (int b = 0; b < nbb; b++) {
        for (int v = 0; v < fn->nvregs; v++)
            cp[v] = -1;
        for (int n = bb[b].start; n < bb[b].end; n++) {
            struct ir_ins *i = &fn->ins[n];
            /* Inline asm names its OUTPUT temps through each_read, and a
             * landing pad writes two temps that def_target cannot both
             * report. Neither is worth modelling for a copy table: drop
             * it and carry on from here. */
            if (i->op == IR_ASM || i->op == IR_LANDING) {
                for (int v = 0; v < fn->nvregs; v++)
                    cp[v] = -1;
                continue;
            }
            struct lcopy c = { cp, fn->nvregs, 0 };
            each_read(i, lcopy_cb, &c);
            if (c.n) {
                changed = 1;
                g_did.copy += c.n;
            }
            int t = def_target(i);
            if (t >= 0 && t < fn->nvregs) {
                cp[t] = -1;                  /* it is a new value now */
                for (int v = 0; v < fn->nvregs; v++)
                    if (cp[v] == t)          /* and so is anything copied
                                              * from what it just replaced */
                        cp[v] = -1;
            }
            /* Only a single-assignment dst becomes a copy. The reverse --
             * rewriting reads of a PHI temp to the value moved into it --
             * is legal and costs more than it saves: it keeps the source
             * live past the copy that ends it, so the two interfere and
             * the allocator can no longer give them one register. That
             * turns `i = i + 1` into a move, an add and a move back. A
             * phi temp is still a fine copy SOURCE, which is the case
             * worth having. */
            if (i->op == IR_MOV && !i->vol && i->dst >= 0 && i->a >= 0 &&
                i->dst != i->a && i->dst < fn->nvregs && i->a < fn->nvregs &&
                d.cnt[i->dst] == 1)
                cp[i->dst] = cp[i->a] >= 0 ? cp[i->a] : i->a;
        }
    }
    free_defs(&d);
    free(cp); free(l2b);
    for (int i = 0; i < nbb; i++) free(bb[i].pred);
    free(bb);
    return changed;
}

/* ==== loop-invariant code motion (-O2) ====================================== *
 *
 * An expression inside a loop whose operands all come from outside it
 * computes the same value every iteration, so it belongs before the
 * loop. `s += a * b + a` in a loop over i runs the multiply n times and
 * needs to run it once.
 *
 * The shape is the textbook one -- natural loops from back edges, a
 * preheader, an invariance fixpoint -- with the pieces already here:
 * build_cfg, compute_rpo and compute_idom were built for mem2reg, and a
 * back edge is exactly an edge whose target dominates its source.
 *
 * ---- the preheader, without a jump ---------------------------------------
 *
 * Hoisted code has to land somewhere that runs once on the way in and is
 * skipped by the back edge. The usual construction is a new block that
 * jumps to the header; this one puts the preheader PHYSICALLY where it
 * has to be instead, immediately before the header's label:
 *
 *      Lpre:                  <- new label, out-of-loop entries retargeted
 *          <hoisted>
 *      Lh:                    <- the header, unchanged
 *          ...
 *          brnz Lh            <- the back edge still lands past the hoist
 *
 * so the preheader falls through into the header and no jump is emitted
 * at all. Entries from outside the loop are retargeted from Lh to Lpre;
 * the back edges are left alone, which is what makes the hoisted code
 * run once. A block that FALLS INTO the header would fall into the
 * preheader instead, which is right when it is outside the loop and
 * wrong when it is inside -- so an in-loop block that ends where the
 * header starts is refused rather than handled.
 *
 * ---- what may move -------------------------------------------------------
 *
 * is_pure() already means "no side effect and cannot fault", which is
 * most of the condition: the preheader runs even when the loop body runs
 * zero times, so anything hoisted is speculated, and a trapping
 * instruction (DIV, MOD) or a LOAD may not be. The rest is that every
 * operand is defined outside the loop, or by something already being
 * hoisted -- a fixpoint, because `t = a * b` hoisting makes `t + a`
 * hoistable in the next round.
 *
 * IR_LDVAR is the one that needs more than purity. It reads a frame
 * slot, which is invariant only if nothing in the loop writes it: no
 * IR_STVAR to that slot, and the slot's address never taken anywhere in
 * the function (an address that escaped could be stored through).
 *
 * One loop is transformed per call and the caller rounds again, because
 * moving instructions invalidates every block boundary. Loops are taken
 * innermost first (highest RPO header), so an inner hoist has already
 * happened when the outer loop is considered and the value can move the
 * whole way out in successive rounds. */

/* Does block `s` dominate block `b`? The idom chain is already built;
 * the entry is its own idom, which is where the walk stops. */
static int bb_dominates(struct bb *bb, int s, int b)
{
    for (int x = b;;) {
        if (x == s)
            return 1;
        if (x == 0 || bb[x].idom < 0 || bb[x].idom == x)
            return 0;
        x = bb[x].idom;
    }
}

/* The natural loop of the back edge tail -> h: h, plus every block that
 * reaches tail without passing through h. */
static void loop_body(struct bb *bb, int nbb, int h, int tail, char *in)
{
    int *stk = xmalloc((size_t)nbb * sizeof *stk), sp = 0;
    in[h] = 1;
    if (!in[tail]) { in[tail] = 1; stk[sp++] = tail; }
    while (sp) {
        int b = stk[--sp];
        for (int k = 0; k < bb[b].npred; k++) {
            int p = bb[b].pred[k];
            if (!in[p]) { in[p] = 1; stk[sp++] = p; }
        }
    }
    free(stk);
}

/* Hoist out of one loop, or report that there was nothing to do. */
static int licm_one(struct ir_func *fn)
{
    if (fn->nins == 0)
        return 0;
    int nbb, *l2b;
    struct bb *bb = build_cfg(fn, &nbb, &l2b);
    int *order = xmalloc((size_t)nbb * sizeof *order), norder;
    compute_rpo(bb, nbb, order, &norder);
    if (norder != nbb) {       /* unreachable blocks: dominance is partial */
        free(order); free(l2b);
        for (int i = 0; i < nbb; i++) free(bb[i].pred);
        free(bb); return 0;
    }
    compute_idom(bb, order, norder);

    /* A slot whose address is taken ANYWHERE could be written through a
     * pointer, so a load of it is never loop-invariant. */
    char *addr_taken = xcalloc((size_t)(fn->nvars ? fn->nvars : 1), 1);
    for (int n = 0; n < fn->nins; n++)
        if (fn->ins[n].op == IR_ADDR && fn->ins[n].a >= 0 &&
            fn->ins[n].a < fn->nvars)
            addr_taken[fn->ins[n].a] = 1;

    struct defs d;
    compute_defs(fn, &d);
    char *in = xmalloc((size_t)nbb);
    char *inl_ins = xmalloc((size_t)fn->nins);
    char *inv = xmalloc((size_t)fn->nins);
    char *stored = xmalloc((size_t)(fn->nvars ? fn->nvars : 1));
    int *hoist = xmalloc((size_t)fn->nins * sizeof *hoist);
    int done = 0;

    /* Headers innermost first: a higher RPO number is deeper in. */
    for (int oi = norder - 1; oi >= 0 && !done; oi--) {
        int h = order[oi];
        if (h == 0)                      /* the entry has no preheader slot */
            continue;
        if (bb[h].end <= bb[h].start || fn->ins[bb[h].start].op != IR_LABEL)
            continue;                    /* not a branch target: no back edge */
        int Lh = fn->ins[bb[h].start].label;

        memset(in, 0, (size_t)nbb);
        int any_back = 0;
        for (int p = 0; p < nbb; p++)
            for (int k = 0; k < bb[p].nsucc; k++)
                if (bb[p].succ[k] == h && bb_dominates(bb, h, p)) {
                    loop_body(bb, nbb, h, p, in);
                    any_back = 1;
                }
        if (!any_back)
            continue;

        /* Every way into the loop must be through the header, or the
         * preheader would not dominate what was hoisted into it. */
        int ok = 1, nentry = 0;
        for (int b = 0; b < nbb && ok; b++) {
            if (!in[b])
                continue;
            for (int k = 0; k < bb[b].npred; k++) {
                int p = bb[b].pred[k];
                if (in[p])
                    continue;
                if (b != h) ok = 0;      /* a second entry: not a natural loop */
                else nentry++;
            }
        }
        if (!ok || nentry == 0)
            continue;
        /* And no block INSIDE the loop may fall into the header, since
         * it would fall into the preheader instead. A block that ends
         * where the header starts falls into it unless its last
         * instruction leaves unconditionally. */
        for (int p = 0; p < nbb && ok; p++) {
            if (!in[p] || bb[p].end != bb[h].start || bb[p].end <= bb[p].start)
                continue;
            enum ir_op t = fn->ins[bb[p].end - 1].op;
            if (t != IR_JMP && t != IR_RET && t != IR_UD2)
                ok = 0;
        }
        if (!ok)
            continue;

        /* What the loop does to memory, which decides whether a load of
         * a frame slot can move. */
        memset(inl_ins, 0, (size_t)fn->nins);
        memset(stored, 0, (size_t)(fn->nvars ? fn->nvars : 1));
        for (int b = 0; b < nbb; b++) {
            if (!in[b])
                continue;
            for (int n = bb[b].start; n < bb[b].end; n++) {
                inl_ins[n] = 1;
                if (fn->ins[n].op == IR_STVAR && fn->ins[n].dst >= 0 &&
                    fn->ins[n].dst < fn->nvars)
                    stored[fn->ins[n].dst] = 1;
            }
        }

        /* The invariance fixpoint. */
        memset(inv, 0, (size_t)fn->nins);
        int grew = 1;
        while (grew) {
            grew = 0;
            for (int n = 0; n < fn->nins; n++) {
                if (!inl_ins[n] || inv[n])
                    continue;
                struct ir_ins *i = &fn->ins[n];
                if (!is_pure(i->op) || i->vol)
                    continue;
                int t = def_target(i);
                /* Only a temp, and only one the function defines once:
                 * a slot is not SSA and a repeated def is not a value. */
                if (t < fn->nvars || t >= fn->nvregs || d.cnt[t] != 1)
                    continue;
                if (i->op == IR_LDVAR) {
                    if (i->a < 0 || i->a >= fn->nvars ||
                        stored[i->a] || addr_taken[i->a])
                        continue;
                }
                struct opnds o;
                value_opnds(i, &o);
                if (o.over)
                    continue;
                int all = 1;
                for (int k = 0; k < o.n && all; k++) {
                    int v = o.v[k];
                    if (v < 0 || v >= fn->nvregs) { all = 0; break; }
                    if (d.cnt[v] != 1) { all = 0; break; }
                    int def = d.ins[v];
                    if (def < 0)            /* a parameter, bound at entry */
                        continue;
                    if (inl_ins[def] && !inv[def])
                        all = 0;
                }
                if (all) { inv[n] = 1; grew = 1; }
            }
        }

        int nh = 0;
        for (int n = 0; n < fn->nins; n++)
            if (inv[n])
                hoist[nh++] = n;
        if (nh == 0)
            continue;
        /* Emitted in their original order, which must already put each
         * definition before its uses. It does, for a single-assignment
         * temp whose def dominates its uses -- but a hoist that broke
         * that would be a miscompile, so check rather than assume. */
        for (int k = 0; k < nh && ok; k++) {
            struct opnds o;
            value_opnds(&fn->ins[hoist[k]], &o);
            for (int q = 0; q < o.n && ok; q++) {
                int def = d.ins[o.v[q]];
                if (def < 0 || !inv[def])
                    continue;
                int seen = 0;
                for (int j = 0; j < k; j++)
                    if (hoist[j] == def) { seen = 1; break; }
                if (!seen) ok = 0;
            }
        }
        if (!ok)
            continue;

        /* Retarget every entry from outside the loop to the preheader.
         * A fall-through entry needs nothing: the preheader is placed
         * immediately before the header, so it lands there already. */
        int Lpre = fn->nlabels++;
        for (int p = 0; p < nbb; p++) {
            if (in[p] || bb[p].end <= bb[p].start)
                continue;
            int hits = 0;
            for (int k = 0; k < bb[p].nsucc; k++)
                if (bb[p].succ[k] == h) hits = 1;
            if (!hits)
                continue;
            struct ir_ins *t = &fn->ins[bb[p].end - 1];
            if ((t->op == IR_JMP || t->op == IR_BRZ || t->op == IR_BRNZ) &&
                t->label == Lh)
                t->label = Lpre;
        }

        /* Rebuild: the preheader label and the hoisted instructions go in
         * before the header's label, and the originals come out. */
        char *moved = xcalloc((size_t)fn->nins, 1);
        for (int k = 0; k < nh; k++)
            moved[hoist[k]] = 1;
        int *newpos = fn->var_scope_lo
            ? xmalloc((size_t)(fn->nins + 1) * sizeof *newpos) : NULL;
        struct ibuf nb = { 0, 0, 0 };
        for (int n = 0; n < fn->nins; n++) {
            if (n == bb[h].start) {
                struct ir_ins *l = ib_push(&nb);
                l->op = IR_LABEL;
                l->label = Lpre;
                l->line = fn->ins[n].line;
                l->col = fn->ins[n].col;
                l->synth = 1;            /* no source construct: it is a seam */
                for (int k = 0; k < nh; k++)
                    *ib_push(&nb) = fn->ins[hoist[k]];
            }
            if (newpos) newpos[n] = nb.n;
            if (moved[n])
                continue;
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
        free(moved);
        if (remarks_on() && fn->src)
            remark_add("opt", "hoisted", fn->name, "licm/loop-invariant",
                       fn->file, fn->line,
                       "%d instruction%s out of a loop", nh, nh == 1 ? "" : "s");
        done = 1;
    }

    free(hoist); free(stored); free(inv); free(inl_ins); free(in);
    free_defs(&d); free(addr_taken);
    free(order); free(l2b);
    for (int i = 0; i < nbb; i++) free(bb[i].pred);
    free(bb);
    return done;
}

static int pass_licm(struct ir_func *fn)
{
    int changed = 0, guard = 0;
    while (guard++ < 64 && licm_one(fn))
        changed = 1;
    return changed;
}

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
        for (int i = 0; i < nbb; i++) free(bb[i].pred);
        free(bb); return 0;
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
        for (int p = 0; p < nbb; p++)
            for (int k = 0; k < bb[p].nsucc; k++)
                if (bb[p].succ[k] == h && bb_dominates(bb, h, p)) {
                    latch = p; nback++;
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
         * so each must be safe to run twice and must define a temp read
         * only here. */
        int ok = 1;
        for (int n = bb[h].start + 1; n < bb[h].end - 1 && ok; n++) {
            struct ir_ins *i = &fn->ins[n];
            if (!is_pure(i->op) || i->vol) { ok = 0; break; }
            int t = def_target(i);
            if (t < fn->nvars || t >= fn->nvregs) { ok = 0; break; }
            for (int m = 0; m < fn->nins && ok; m++) {
                if (m >= bb[h].start && m < bb[h].end)
                    continue;
                if (ins_reads(&fn->ins[m], t))
                    ok = 0;
            }
        }
        if (!ok) { free(in); continue; }

        /* Fresh temps for the copy, so the guard's values stand. */
        int ncopy = bb[h].end - 1 - (bb[h].start + 1);
        int *map = xmalloc((size_t)(ncopy ? ncopy : 1) * sizeof *map);
        for (int k = 0; k < ncopy; k++)
            map[k] = fn->nvregs++;
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
                    if (old >= 0 && old < fn->nvregs)
                        tbl[old] = map[q];
                }
                for (int k = 0; k < ncopy; k++) {
                    struct ir_ins *c = ib_push(&nb);
                    *c = fn->ins[bb[h].start + 1 + k];
                    struct lcopy lc = { tbl, fn->nvregs, 0 };
                    each_read(c, lcopy_cb, &lc);
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
    for (int i = 0; i < nbb; i++) free(bb[i].pred);
    free(bb);
    return done;
}

static int pass_rotate(struct ir_func *fn)
{
    int changed = 0, guard = 0;
    while (guard++ < 64 && rotate_one(fn))
        changed = 1;
    return changed;
}

/* ==== automatic vectorization (-O2) ========================================= *
 *
 * `for (i = 0; i < 4096; i++) a[i] = a[i] * 3 + 1;` does one element at
 * a time where the machine will do four. Both targets have 128-bit SIMD
 * in their base instruction set -- SSE2 is part of x86-64, Advanced SIMD
 * part of aarch64 -- so four ints an iteration needs no feature test.
 *
 * ---- the shape of the transform ------------------------------------------
 *
 * The usual construction is a vector loop over floor(n/VF)*VF followed by
 * a scalar loop for the remainder, which is two loops, a new block, and
 * an induction variable that has to start where the other stopped. This
 * does none of that. It only vectorizes a loop whose trip count is a
 * CONSTANT multiple of the vector factor, and then the whole transform
 * is local to the existing loop:
 *
 *   - the induction step becomes VF instead of 1
 *   - each scalar op becomes its lane-wise equivalent
 *
 * and nothing else moves. The address is already `base + i * esize`, so
 * stepping i by 4 steps the address by 16 on its own; the existing test
 * `i < 4096` already stops after 1024 iterations of four. No remainder,
 * no second loop, no new blocks, and the guard that decides whether the
 * loop runs at all is untouched.
 *
 * That is a real restriction -- `i < n` for a runtime n is not covered --
 * but it is the whole of the risk budget spent on the part that pays:
 * fixed-size array loops are common, and a vectorizer that quietly gets
 * the remainder wrong is a wrong answer in the most ordinary code there
 * is. The general trip count is the next step, not this one.
 *
 * ---- what may be vectorized ----------------------------------------------
 *
 * Every memory reference must be `global + i * esize` at the same i and
 * the same scale, so lane k of every reference is element i+k of its
 * array: there is no cross-lane dependence to get wrong, and two
 * distinct globals cannot overlap. A base that came from a pointer could
 * alias something and is refused. Every value is then either loaded,
 * loop-invariant (broadcast once, before the loop), or computed from
 * those by an op SSE2 and NEON both have lane-wise.
 *
 * Multiplication is the interesting refusal. A packed 32-bit multiply is
 * SSE4.1, not SSE2, so a multiply by a constant is decomposed into
 * shifts and adds -- `x * 3` is `(x << 1) + x` -- and a multiply by
 * anything else is simply not vectorized. gcc does the same
 * decomposition here for the same reason. */

#define VEC_BYTES 16

/* One loop's worth of what the recognizer found. */
struct vecloop {
    int iv;            /* the induction variable's phi temp */
    int step_ins;      /* `next = iv + 1` */
    int copy_ins;      /* `iv = mov next` */
    int cmp_ins;       /* `c = cmp lt iv, #bound` */
    long bound;        /* the trip count, when bound_reg < 0 */
    int bound_reg;     /* or the temp holding it, for a runtime count */
    int esize, vf;
    /* A sum reduction (`s += a[i]`), or red_acc == -1. The accumulator
     * becomes VF partial sums, folded to one after the loop. */
    int red_acc;       /* the accumulator's phi temp */
    int red_next;      /* what the loop body adds into it */
    int red_add;       /* `next = acc + <a vector>` */
    int red_copy;      /* `acc = mov next`, the phi copy at the latch */
    int red_vec;       /* the vector operand being summed */
    int red_ext;       /* a widening sum: the IR_EXT between load and add,
                        * and then red_vec is the NARROW vector it reads */
    int lo, hi;        /* the loop's instruction range, [lo, hi) */
    int header;        /* its first block */
};

/* Is `v` defined outside [lo, hi)? A loop-invariant operand, which a
 * vector op reaches by broadcasting it once before the loop. */
static int defined_outside(struct ir_func *fn, struct defs *d, int v,
                           int lo, int hi)
{
    if (v < 0 || v >= fn->nvregs || d->cnt[v] != 1)
        return 0;
    int def = d->ins[v];
    return def < 0 || def < lo || def >= hi;   /* < 0: a parameter */
}

/* Does the address in temp `p` read `base + iv * scale`, with base a
 * global's address defined outside the loop? Returns the base temp, or
 * -1. The chain irgen builds is add(gaddr, shl(ext(iv), log2 scale)),
 * with the ext absent when the index is already 64-bit. */
static int addr_of_iv(struct ir_func *fn, struct defs *d, struct vecloop *L,
                      int p, int scale)
{
    if (p < 0 || p >= fn->nvregs || d->cnt[p] != 1)
        return -1;
    int an = d->ins[p];
    if (an < 0 || fn->ins[an].op != IR_ADD)
        return -1;
    struct ir_ins *add = &fn->ins[an];
    /* Either operand may be the base; the other is the scaled index. */
    for (int side = 0; side < 2; side++) {
        int base = side ? add->b : add->a;
        int idx  = side ? add->a : add->b;
        if (base < 0 || base >= fn->nvregs || d->cnt[base] != 1)
            continue;
        /* Loop-invariant is all that is asked HERE; whether the bases
         * can alias each other is decided once, over all of them, by
         * the caller. A global's address qualifies wherever the
         * instruction sits, since it is the same every iteration. */
        int bd = d->ins[base];
        if (bd >= 0 && (bd >= L->lo && bd < L->hi) &&
            fn->ins[bd].op != IR_GADDR)
            continue;               /* computed inside the loop */
        if (idx < 0 || idx >= fn->nvregs || d->cnt[idx] != 1)
            continue;
        int sn = d->ins[idx];
        long sh;
        if (sn < 0 || fn->ins[sn].op != IR_SHL ||
            !const_b(fn, d, &fn->ins[sn], &sh) || sh < 0 || sh > 6)
            continue;
        if ((1L << sh) != scale)
            continue;
        int x = fn->ins[sn].a;
        if (x >= 0 && x < fn->nvregs && d->cnt[x] == 1) {
            int xn = d->ins[x];
            if (xn >= 0 && fn->ins[xn].op == IR_EXT)
                x = fn->ins[xn].a;   /* the widened index */
        }
        if (x == L->iv)
            return base;
    }
    return -1;
}

/* The lane-wise opcode character for a scalar op, or 0 for one SSE2 and
 * NEON do not both have. */
static int vec_op_char(enum ir_op op)
{
    switch (op) {
    case IR_ADD: return '+';
    case IR_SUB: return '-';
    case IR_AND: return '&';
    case IR_OR:  return '|';
    case IR_XOR: return '^';
    case IR_SHL: return '<';
    case IR_SHR: return '>';
    default:     return 0;
    }
}

#define VDBG(...) do { if (getenv("EMBCC_VECDEBUG")) \
        fprintf(stderr, "vec: " __VA_ARGS__); } while (0)


/* Remap the vregs an instruction READS, for the copied vector body. */
struct vremap { const int *map; int n; };
static void vremap_cb(int *p, void *ctx)
{
    struct vremap *r = ctx;
    if (*p >= 0 && *p < r->n && r->map[*p] >= 0)
        *p = r->map[*p];
}

/* Build one lane-wise instruction from a scalar one, into `v`.
 * `a`/`b` are the operands already remapped; `splat` is the broadcast of
 * b when b was a constant, or -1. Returns 0 if the op has no lane-wise
 * form, which the recognizer should already have refused. */
static int vec_build(struct ir_ins *v, const struct ir_ins *src, int dst,
                     int a, int b, int splat, long kb, int hask, int esize)
{
    memset(v, 0, sizeof *v);
    v->op = IR_VBIN; v->dst = dst; v->a = a;
    v->size = esize; v->w = 8;
    v->line = src->line; v->col = src->col;
    int c = vec_op_char(src->op);
    if (!c)
        return 0;
    v->imm = c;
    if ((c == '<' || c == '>') && hask) {
        v->b = -1; v->c = (int)kb; v->sign = src->sign;
    } else if (hask) {
        v->b = splat;
    } else {
        v->b = b;
    }
    return 1;
}

/* Vectorize a loop whose trip count is only known at run time.
 *
 * The constant-count path above rewrites the loop in place, which it can
 * because the count is known to divide evenly. Here it does not, so the
 * loop is preceded by a VECTOR COPY of itself that runs over the whole
 * vectors, and the original is left to finish the remainder:
 *
 *       guard: if (0 >= n) goto done          <- already there
 *       nvec = n & ~(VF-1)                    <- n >= 1 by the guard
 *       vi = 0
 *       goto vcond
 *   vbody:  <the body, lane-wise, indexed by vi>
 *       vi += VF
 *   vcond:  if (vi < nvec) goto vbody
 *       <fold the reduction, if any>
 *       i = nvec
 *       if (i >= n) goto skip                 <- no remainder
 *       <the original loop, unchanged>
 *   skip:
 *
 * The guard means n >= 1 here, which is what makes `n & ~(VF-1)`
 * non-negative without a clamp -- for a negative n it would be negative,
 * the vector loop would be skipped, and the remainder would then start
 * at a negative index. The second test is needed because the first one
 * has already been passed: entering the original loop's body with
 * i == n would run it once too often.
 *
 * A reduction folds BETWEEN the two loops, so the remainder carries on
 * adding into an accumulator that already holds the vector part. */
static int vec_rewrite_runtime(struct ir_func *fn, struct defs *d,
                               struct vecloop *L, char *vec)
{
    int nv0 = fn->nvregs;
    int *map = xmalloc((size_t)nv0 * sizeof *map);
    for (int v = 0; v < nv0; v++)
        map[v] = -1;

    int vi = fn->nvregs++, vin = fn->nvregs++, nvec = fn->nvregs++;
    int kmask = fn->nvregs++, kzero = fn->nvregs++, kstep = fn->nvregs++;
    /* The guard's compare and the latch's compare are DIFFERENT values
     * of the same expression, one before the loop and one at the end of
     * each iteration, so they need different temps. Sharing one made it
     * doubly assigned, and every later pass that asks "where was this
     * defined" -- strength reduction among them -- gave up on the loop. */
    int tg = fn->nvregs++, tv = fn->nvregs++, t3 = fn->nvregs++;
    int vacc = -1, vzero = -1, redtmp = -1;
    if (L->red_acc >= 0) {
        vzero = fn->nvregs++; vacc = fn->nvregs++; redtmp = fn->nvregs++;
    }
    int Lvb = fn->nlabels++, Lvc = fn->nlabels++, Lskip = fn->nlabels++;
    int iw = fn->ins[L->cmp_ins].w, isign = fn->ins[L->cmp_ins].sign;

    map[L->iv] = vi;
    for (int n = L->lo; n < L->hi; n++) {
        if (n == L->step_ins || n == L->copy_ins || n == L->cmp_ins ||
            n == L->red_copy || n == L->red_add)
            continue;
        struct ir_ins *i = &fn->ins[n];
        if (i->op == IR_LABEL || i->op == IR_JMP ||
            i->op == IR_BRZ || i->op == IR_BRNZ)
            continue;
        int t = def_target(i);
        if (t >= 0 && t < nv0 && map[t] < 0)
            map[t] = fn->nvregs++;
    }

    struct ibuf nb = { 0, 0, 0 };
    int *newpos = fn->var_scope_lo
        ? xmalloc((size_t)(fn->nins + 1) * sizeof *newpos) : NULL;
#define EM(V) struct ir_ins *V = ib_push(&nb); \
              V->line = fn->ins[L->cmp_ins].line; \
              V->col = fn->ins[L->cmp_ins].col; V->synth = 1
    for (int n = 0; n < fn->nins; n++) {
        if (n == L->lo) {
            { EM(k); k->op = IR_CONST; k->dst = kzero; k->w = iw; }
            { EM(k); k->op = IR_CONST; k->dst = kstep; k->w = iw;
              k->imm = L->vf; }
            { EM(k); k->op = IR_CONST; k->dst = kmask; k->w = iw;
              k->imm = -(long)L->vf; }        /* ~(VF-1), VF a power of two */
            /* broadcasts of the constants the body adds lane-wise */
            for (int m = L->lo; m < L->hi; m++) {
                struct ir_ins *src = &fn->ins[m];
                int t = def_target(src);
                long kb;
                if (t < 0 || t >= nv0 || !vec[t] || m == L->red_add)
                    continue;
                if (src->op == IR_SHL || src->op == IR_SHR ||
                    src->op == IR_MUL || !const_b(fn, d, src, &kb))
                    continue;
                { EM(k); k->op = IR_CONST; k->dst = fn->nvregs;
                  k->w = L->esize; k->imm = kb; }
                { EM(sp); sp->op = IR_VSPLAT; sp->dst = fn->nvregs + 1;
                  sp->a = fn->nvregs; sp->size = L->esize; sp->w = 8; }
                src->c = fn->nvregs + 1;
                fn->nvregs += 2;
            }
            if (L->red_acc >= 0) {
                { EM(z); z->op = IR_CONST; z->dst = vzero; z->w = L->esize; }
                { EM(sp); sp->op = IR_VSPLAT; sp->dst = vacc; sp->a = vzero;
                  sp->size = L->esize; sp->w = 8; }
            }
            { EM(k); k->op = IR_AND; k->dst = nvec; k->a = L->bound_reg;
              k->b = kmask; k->w = iw; k->sign = isign; }
            { EM(k); k->op = IR_MOV; k->dst = vi; k->a = kzero; k->w = iw; }
            /* Guarded and bottom-tested, not top-tested with a jump in.
             * One branch an iteration rather than two, for the reason
             * rotation exists -- and, less obviously, it is what makes
             * this a NATURAL loop: entering at the test would mean the
             * header does not dominate the latch, and every later pass
             * that reasons about loops (strength reduction above all)
             * would decline to touch it. */
            { EM(k); k->op = IR_CMP; k->dst = tg; k->a = vi; k->b = nvec;
              k->w = iw; k->sign = isign; k->pred = B_LT; }
            { EM(k); k->op = IR_BRZ; k->a = tg; k->label = Lvc;
              k->w = fn->ins[L->hi - 1].w; }
            { EM(k); k->op = IR_LABEL; k->label = Lvb; }

            /* ---- the body, lane-wise, indexed by vi ---- */
            for (int m = L->lo; m < L->hi; m++) {
                struct ir_ins *src = &fn->ins[m];
                if (m == L->step_ins || m == L->copy_ins || m == L->cmp_ins ||
                    m == L->red_copy || src->op == IR_LABEL ||
                    src->op == IR_JMP || src->op == IR_BRZ ||
                    src->op == IR_BRNZ)
                    continue;
                int t = def_target(src);
                int nt = t >= 0 && t < nv0 && map[t] >= 0 ? map[t] : t;
                int ra = src->a >= 0 && src->a < nv0 && map[src->a] >= 0
                         ? map[src->a] : src->a;
                int rb = src->b >= 0 && src->b < nv0 && map[src->b] >= 0
                         ? map[src->b] : src->b;
                if (m == L->red_add) {
                    int rv = L->red_vec < nv0 && map[L->red_vec] >= 0
                             ? map[L->red_vec] : L->red_vec;
                    EM(v); v->op = IR_VBIN; v->dst = vacc; v->a = vacc;
                    v->b = rv; v->imm = '+'; v->size = L->esize; v->w = 8;
                    v->line = src->line; v->col = src->col; v->synth = 0;
                    continue;
                }
                if (src->op == IR_LOAD && t >= 0 && t < nv0 && vec[t]) {
                    EM(v); v->op = IR_VLOAD; v->dst = nt; v->a = ra;
                    v->size = L->esize; v->w = 8;
                    v->line = src->line; v->col = src->col; v->synth = 0;
                    continue;
                }
                if (src->op == IR_STORE) {
                    EM(v); v->op = IR_VSTORE; v->dst = -1; v->a = ra;
                    v->b = rb; v->size = L->esize; v->w = 8;
                    v->line = src->line; v->col = src->col; v->synth = 0;
                    continue;
                }
                if (t >= 0 && t < nv0 && vec[t]) {
                    long kb = 0;
                    int hask = const_b(fn, d, src, &kb);
                    if (src->op == IR_MUL) {
                        int sh = 0;
                        long mlt = kb;
                        int p2 = (mlt & (mlt - 1)) == 0;
                        while ((1L << sh) < (p2 ? mlt : mlt - 1)) sh++;
                        if (p2) {
                            EM(v); v->op = IR_VBIN; v->dst = nt; v->a = ra;
                            v->b = -1; v->imm = '<'; v->c = sh;
                            v->size = L->esize; v->w = 8;
                            v->line = src->line; v->col = src->col;
                            v->synth = 0;
                        } else {
                            int tmp = fn->nvregs++;
                            { EM(v); v->op = IR_VBIN; v->dst = tmp; v->a = ra;
                              v->b = -1; v->imm = '<'; v->c = sh;
                              v->size = L->esize; v->w = 8;
                              v->line = src->line; v->col = src->col;
                              v->synth = 0; }
                            { EM(w2); w2->op = IR_VBIN; w2->dst = nt;
                              w2->a = tmp; w2->b = ra; w2->imm = '+';
                              w2->size = L->esize; w2->w = 8;
                              w2->line = src->line; w2->col = src->col;
                              w2->synth = 0; }
                        }
                        continue;
                    }
                    EM(v);
                    if (!vec_build(v, src, nt, ra, rb, src->c, kb, hask,
                                   L->esize)) {
                        free(map); free(nb.p); free(newpos);
                        return 0;       /* the recognizer let one through */
                    }
                    continue;
                }
                /* the address chain and anything else scalar: copied
                 * with its operands pointed at the vector index */
                EM(v); *v = *src; v->synth = 0;
                struct vremap r = { map, nv0 };
                each_read(v, vremap_cb, &r);
                if (t >= 0)
                    v->dst = nt;
            }
            { EM(k); k->op = IR_ADD; k->dst = vin; k->a = vi; k->b = kstep;
              k->w = iw; k->sign = isign; }
            { EM(k); k->op = IR_MOV; k->dst = vi; k->a = vin; k->w = iw; }
            { EM(k); k->op = IR_CMP; k->dst = tv; k->a = vi; k->b = nvec;
              k->w = iw; k->sign = isign; k->pred = B_LT; }
            { EM(k); k->op = IR_BRNZ; k->a = tv; k->label = Lvb;
              k->w = fn->ins[L->hi - 1].w; }
            { EM(k); k->op = IR_LABEL; k->label = Lvc; }   /* past the vectors */
            if (L->red_acc >= 0) {
                { EM(r); r->op = IR_VREDADD; r->dst = redtmp; r->a = vacc;
                  r->size = L->esize; r->w = fn->ins[L->red_add].w; }
                { EM(a2); a2->op = IR_ADD; a2->dst = L->red_acc;
                  a2->a = L->red_acc; a2->b = redtmp;
                  a2->w = fn->ins[L->red_add].w;
                  a2->sign = fn->ins[L->red_add].sign; }
            }
            { EM(k); k->op = IR_MOV; k->dst = L->iv; k->a = nvec; k->w = iw; }
            { EM(k); k->op = IR_CMP; k->dst = t3; k->a = L->iv;
              k->b = L->bound_reg; k->w = iw; k->sign = isign;
              k->pred = B_LT; }
            { EM(k); k->op = IR_BRZ; k->a = t3; k->label = Lskip;
              k->w = fn->ins[L->hi - 1].w; }
        }
        if (newpos) newpos[n] = nb.n;
        *ib_push(&nb) = fn->ins[n];         /* the scalar loop, unchanged */
        if (n == L->hi - 1) {
            EM(k); k->op = IR_LABEL; k->label = Lskip;
        }
    }
#undef EM
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
    free(map);
    return 1;
}

static int vectorize_one(struct ir_func *fn)
{
    if (fn->nins == 0)
        return 0;
    VDBG("considering %s\n", fn->name);
    int nbb, *l2b;
    struct bb *bb = build_cfg(fn, &nbb, &l2b);
    int *order = xmalloc((size_t)nbb * sizeof *order), norder;
    compute_rpo(bb, nbb, order, &norder);
    if (norder != nbb) {
        free(order); free(l2b);
        for (int i = 0; i < nbb; i++) free(bb[i].pred);
        free(bb); return 0;
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

        /* One back edge, from a latch that branches to the header --
         * the rotated shape, which is the only one whose test is at the
         * bottom where the step is. */
        int latch = -1, nback = 0;
        for (int p = 0; p < nbb; p++)
            for (int k = 0; k < bb[p].nsucc; k++)
                if (bb[p].succ[k] == h && bb_dominates(bb, h, p)) {
                    latch = p; nback++;
                }
        if (nback != 1 || bb[latch].end <= bb[latch].start)
            continue;
        struct ir_ins *br = &fn->ins[bb[latch].end - 1];
        if (br->op != IR_BRNZ || br->label != Lh)
            { VDBG("h=%d not a rotated latch\n", h); continue; }

        memset(in, 0, (size_t)nbb);
        loop_body(bb, nbb, h, latch, in);
        /* The loop must be one contiguous run of instructions, so the
         * body can be read and rewritten as a straight line. */
        struct vecloop L;
        L.lo = bb[h].start; L.hi = bb[latch].end; L.header = h;
        int ok = 1;
        for (int b = 0; b < nbb && ok; b++)
            if (in[b] != (bb[b].start >= L.lo && bb[b].end <= L.hi))
                ok = 0;
        if (!ok)
            { VDBG("h=%d loop not contiguous\n", h); continue; }

        /* The test: `c = cmp lt iv, #bound`, feeding the back branch. */
        int cn = (br->a >= 0 && br->a < fn->nvregs && d.cnt[br->a] == 1)
                 ? d.ins[br->a] : -1;
        if (cn < L.lo || cn >= L.hi)
            continue;
        struct ir_ins *cmp = &fn->ins[cn];
        if (cmp->op != IR_CMP || cmp->pred != B_LT || !cmp->sign)
            { VDBG("h=%d test is not `cmp lt iv, ...`\n", h); continue; }
        L.cmp_ins = cn;
        L.iv = cmp->a;
        L.bound = 0; L.bound_reg = -1;
        if (!const_b(fn, &d, cmp, &L.bound)) {
            /* A runtime trip count. The bound itself must not change
             * inside the loop, or "how many whole vectors fit" is not a
             * question with one answer. */
            L.bound_reg = cmp->b;
            if (!defined_outside(fn, &d, L.bound_reg, L.lo, L.hi))
                { VDBG("h=%d the bound is not invariant\n", h); continue; }
        } else if (L.bound <= 0) {
            VDBG("h=%d bad bound\n", h); continue;
        }
        if (L.iv < 0 || L.iv >= fn->nvregs)
            { VDBG("h=%d bad iv\n", h); continue; }

        /* `iv = mov next` and `next = iv + 1`, both in the loop. */
        L.copy_ins = L.step_ins = -1;
        for (int n = L.lo; n < L.hi; n++) {
            struct ir_ins *i = &fn->ins[n];
            if (i->op == IR_MOV && i->dst == L.iv)
                L.copy_ins = n;
        }
        if (L.copy_ins < 0)
            { VDBG("h=%d no phi copy for iv %d\n", h, L.iv); continue; }
        int next = fn->ins[L.copy_ins].a;
        if (next < 0 || next >= fn->nvregs || d.cnt[next] != 1)
            continue;
        L.step_ins = d.ins[next];
        if (L.step_ins < L.lo || L.step_ins >= L.hi)
            { VDBG("h=%d step outside loop\n", h); continue; }
        struct ir_ins *st = &fn->ins[L.step_ins];
        long stepk;
        if (st->op != IR_ADD || st->a != L.iv ||
            !const_b(fn, &d, st, &stepk) || stepk != 1)
            { VDBG("h=%d step is not +1\n", h); continue; }

        /* It has to start at zero, or lane k is not element i+k. The
         * other definition of the phi temp is the one before the loop. */
        int init_ok = 0;
        for (int n = 0; n < fn->nins; n++) {
            if (n >= L.lo && n < L.hi)
                continue;
            struct ir_ins *i = &fn->ins[n];
            if (def_target(i) != L.iv)
                continue;
            if (i->op == IR_MOV && i->a >= 0 && i->a < fn->nvregs &&
                d.cnt[i->a] == 1 && d.ins[i->a] >= 0 &&
                fn->ins[d.ins[i->a]].op == IR_CONST &&
                fn->ins[d.ins[i->a]].imm == 0)
                init_ok = 1;
            else if (i->op == IR_CONST && i->imm == 0)
                init_ok = 1;
            else
                { init_ok = 0; break; }
        }
        if (!init_ok)
            { VDBG("h=%d iv does not start at 0\n", h); continue; }

        /* ---- classify the body -------------------------------------
         *
         * vec[v] marks a temp that becomes a vector. Loads seed it and
         * arithmetic spreads it; anything else reading a vector temp,
         * or any op that is not lane-wise, refuses the loop. */
        char *vec = xcalloc((size_t)fn->nvregs, 1);
        L.esize = 0;
        int nmem = 0, nbase = 0, bases[8];
        for (int n = L.lo; n < L.hi && ok; n++) {
            struct ir_ins *i = &fn->ins[n];
            if (i->op != IR_LOAD && i->op != IR_STORE)
                continue;
            if (i->vol || (i->size != 4 && i->size != 8)) { ok = 0; break; }
            if (L.esize && i->size != L.esize) { ok = 0; break; }
            L.esize = i->size;
            int base = addr_of_iv(fn, &d, &L, i->a, i->size);
            if (base < 0) { ok = 0; break; }
            int seen = 0;
            for (int k = 0; k < nbase; k++)
                if (bases[k] == base) seen = 1;
            if (!seen) {
                if (nbase == (int)(sizeof bases / sizeof bases[0]))
                    { ok = 0; break; }
                bases[nbase++] = base;
            }
            if (i->op == IR_LOAD)
                vec[i->dst] = 1;
            nmem++;
        }
        /* ---- can the bases alias each other? ------------------------
         *
         * Two distinct globals cannot overlap, so any number of those is
         * fine. A pointer could point anywhere, so more than one of them
         * -- or one of them beside a global -- would need a run-time
         * check that the ranges are disjoint, which is not built. But
         * ONE base, whatever it is, cannot alias itself: every reference
         * is then the same address at the same index, so lane k is
         * element i+k of the one array, read and written by the same
         * lane. That single case is most of what real code does
         * (`for (i...) p[i] = p[i] * 2`), and it is safe with no check
         * at all. */
        if (ok && nbase > 0) {
            int all_global = 1;
            for (int k = 0; k < nbase; k++) {
                int bd = d.ins[bases[k]];
                if (bd < 0 || fn->ins[bd].op != IR_GADDR)
                    all_global = 0;
            }
            if (!all_global && nbase > 1) {
                VDBG("h=%d %d bases, not all globals: they may alias\n",
                     h, nbase);
                ok = 0;
            }
        }
        if (!ok || !L.esize || nmem == 0) {
            VDBG("h=%d memory refs: ok=%d esize=%d n=%d\n", h, ok, L.esize, nmem);
            free(vec); continue; }
        L.vf = VEC_BYTES / L.esize;
        if (L.bound_reg < 0 && L.bound % L.vf != 0) {
            VDBG("h=%d trip %ld not a multiple of %d\n", h, L.bound, L.vf);
            free(vec); continue; }

        /* Spread vectorness to a fixpoint, and refuse anything that
         * cannot carry it. */
        for (int again = 1; again && ok; ) {
            again = 0;
            for (int n = L.lo; n < L.hi && ok; n++) {
                struct ir_ins *i = &fn->ins[n];
                if (i->op == IR_LOAD || i->op == IR_STORE ||
                    n == L.red_add || n == L.red_copy ||
                    n == L.step_ins || n == L.copy_ins || n == L.cmp_ins ||
                    i->op == IR_LABEL || i->op == IR_JMP ||
                    i->op == IR_BRNZ || i->op == IR_BRZ)
                    continue;
                /* A move into a multiply-assigned temp is a phi copy --
                 * mem2reg's way of carrying a value round the loop. It
                 * has no lane-wise form and refusing it here would
                 * refuse every reduction before the detector below has
                 * had a chance to claim it. Left alone: if the detector
                 * does not claim it, the escape check refuses it, which
                 * is the same answer arrived at properly. */
                if (i->op == IR_MOV && i->dst >= 0 && i->dst < fn->nvregs &&
                    d.cnt[i->dst] == 2)
                    continue;
                /* Likewise a widening of a vector: it has no single
                 * lane-wise form -- four narrow lanes become two wide
                 * ones, so one of these is TWO vectors -- and the
                 * reduction detector is what knows whether that is
                 * wanted. Refusing it here refused every `long s +=
                 * a[i]` over an int array before anything looked. */
                if (i->op == IR_EXT)
                    continue;
                struct opnds o;
                value_opnds(i, &o);
                int any = 0;
                for (int k = 0; k < o.n; k++)
                    if (o.v[k] >= 0 && o.v[k] < fn->nvregs && vec[o.v[k]])
                        any = 1;
                if (!any)
                    continue;               /* wholly scalar: it may stay */
                int t = def_target(i);
                /* A multiply by a constant becomes shifts and adds; any
                 * other multiply, and anything not lane-wise, stops us. */
                int c = vec_op_char(i->op);
                long kb;
                int hask = const_b(fn, &d, i, &kb);
                if (i->op == IR_MUL) {
                    /* only powers of two and (2^k)+1, which is one shift
                     * and one add -- the rest is not worth a chain, and
                     * a packed 32-bit multiply is not SSE2 anyway */
                    if (!hask || kb <= 0) { ok = 0; break; }
                    int p2 = (kb & (kb - 1)) == 0;
                    int p2p1 = ((kb - 1) & (kb - 2)) == 0 && kb >= 3;
                    if (!p2 && !p2p1) { ok = 0; break; }
                } else if (!c) {
                    ok = 0; break;
                } else if ((i->op == IR_SHL || i->op == IR_SHR) && !hask) {
                    ok = 0; break;          /* a per-lane shift count */
                } else if (i->op == IR_SHR && i->sign && L.esize == 8) {
                    /* SSE2 has no psraq. Refused HERE rather than at
                     * emission: the backend's refusal is a loud internal
                     * error, and a loop we simply do not vectorize is
                     * not an error at all. */
                    ok = 0; break;
                } else if (!hask && i->b >= 0 && i->b < fn->nvregs &&
                           !vec[i->b] &&
                           !defined_outside(fn, &d, i->b, L.lo, L.hi)) {
                    ok = 0; break;          /* a scalar that varies per lane */
                }
                if (t < 0 || t >= fn->nvregs) { ok = 0; break; }
                if (i->w != L.esize && i->op != IR_SHL && i->op != IR_SHR)
                    { ok = 0; break; }      /* a widening op changes lanes */
                if (!vec[t]) { vec[t] = 1; again = 1; }
            }
        }
        if (!ok) { VDBG("h=%d body not lane-wise\n", h); free(vec); continue; }

        /* ---- a sum reduction ----------------------------------------
         *
         * `s += a[i]` carries a value ACROSS iterations, which nothing
         * else here is allowed to do. It is vectorizable anyway because
         * addition is associative: keep VF running sums, one per lane,
         * and add them together once the loop is done. The lanes are
         * summed in a different order than the source says, which for
         * integers changes nothing (it would for floats, which is why
         * this is integer-only and why gcc needs -ffast-math for them).
         *
         * The shape is the induction variable's, one value over: a phi
         * temp copied at the latch from something the body computed.
         *
         * This runs AFTER the vectorness fixpoint, not before: the value
         * being added is often computed rather than loaded -- `s += a[i]
         * * 2` adds a shift of a load -- and before the fixpoint only a
         * bare load is marked, so a reduction over anything at all was
         * missed and the whole loop refused as "not lane-wise". */
        L.red_acc = L.red_next = L.red_add = L.red_copy = L.red_vec = -1;
        L.red_ext = -1;
        for (int n = L.lo; n < L.hi && ok; n++) {
            struct ir_ins *i = &fn->ins[n];
            if (i->op != IR_MOV || i->dst == L.iv || i->dst < 0)
                continue;
            int acc = i->dst, nxt = i->a;
            /* Not `vec[nxt]`: in a WIDENING sum neither the extension
             * nor the add is marked (the fixpoint skips the extension
             * on purpose), so the condition that matters is on the
             * operand being summed, checked below. */
            if (nxt < 0 || nxt >= fn->nvregs || d.cnt[nxt] != 1)
                continue;
            int an = d.ins[nxt];
            if (an < L.lo || an >= L.hi)
                continue;
            struct ir_ins *ad = &fn->ins[an];
            long junk;
            if (ad->op != IR_ADD || const_b(fn, &d, ad, &junk))
                continue;
            int other = ad->a == acc ? ad->b : ad->b == acc ? ad->a : -1;
            if (other < 0 || other >= fn->nvregs)
                continue;
            int widen_ext = -1, rvec = other;
            if (ad->flt)
                continue;
            if (ad->w == L.esize) {
                if (!vec[other])
                    continue;           /* summing something that is not a
                                         * vector at all */
            } else {
                /* A widening sum: `long s += a[i]` over an int array.
                 * The operand is an EXT of the loaded vector, and the
                 * accumulator is twice the element width. */
                if (ad->w != 2 * L.esize || L.esize != 4)
                    continue;
                if (other < 0 || other >= fn->nvregs || d.cnt[other] != 1)
                    continue;
                int en = d.ins[other];
                if (en < L.lo || en >= L.hi || fn->ins[en].op != IR_EXT)
                    continue;
                if (fn->ins[en].size != L.esize || fn->ins[en].w != ad->w)
                    continue;
                int src = fn->ins[en].a;
                if (src < 0 || src >= fn->nvregs || !vec[src])
                    continue;
                widen_ext = en; rvec = src;
            }
            if (L.red_acc >= 0) { ok = 0; break; }   /* one is enough */
            L.red_acc = acc; L.red_next = nxt; L.red_add = an;
            L.red_copy = n;  L.red_vec = rvec; L.red_ext = widen_ext;
            /* The running total is NOT a vector value -- it is a scalar
             * carried across iterations that happens to be computed
             * from one. Unmarking it is what stops the escape check
             * below from refusing the phi copy that carries it. */
            vec[nxt] = 0;
        }
        if (!ok) { VDBG("h=%d more than one reduction\n", h); free(vec); continue; }
        if (L.red_acc >= 0) {
            /* The accumulator may be read after the loop -- that is the
             * point of it -- but inside, only by its own sum. Anything
             * else reading a partial total would see one lane's share. */
            for (int n = L.lo; n < L.hi && ok; n++) {
                if (n == L.red_add || n == L.red_copy)
                    continue;
                if (ins_reads(&fn->ins[n], L.red_acc) ||
                    ins_reads(&fn->ins[n], L.red_next))
                    ok = 0;
            }
            if (L.red_ext >= 0) {
                int w = def_target(&fn->ins[L.red_ext]);
                for (int n = 0; n < fn->nins && ok; n++)
                    if (n != L.red_add && ins_reads(&fn->ins[n], w))
                        ok = 0;     /* the widened value feeds only the sum */
            }
            for (int n = 0; n < fn->nins && ok; n++)
                if ((n < L.lo || n >= L.hi) && ins_reads(&fn->ins[n], L.red_next))
                    ok = 0;             /* the partial sum must not escape */
            if (!ok) {
                VDBG("h=%d the accumulator is read elsewhere\n", h);
                free(vec); continue;
            }
        }


        /* Every store's VALUE must be a vector too.
         *
         * Without this, `a[i] = i` became a vstore of the induction
         * variable's eight-byte slot read as sixteen, and the array
         * filled with garbage -- the loop after it then computed
         * 0 * 3 + 1 for every element and the answer was 64 where it
         * should have been 6112. A scalar stored here is one of two
         * things and only one of them is safe: a loop-invariant value,
         * which every lane could share, or something that varies with i
         * like the index itself, which no single lane value can stand
         * for. Telling them apart is worth doing; until it is, neither
         * is vectorized. */
        for (int n = L.lo; n < L.hi && ok; n++) {
            struct ir_ins *i = &fn->ins[n];
            if (i->op != IR_STORE)
                continue;
            if (i->b < 0 || i->b >= fn->nvregs || !vec[i->b])
                ok = 0;
        }
        if (!ok) {
            VDBG("h=%d a store's value is not lane-wise\n", h);
            free(vec); continue;
        }
        /* ---- nothing may happen a quarter as often ------------------
         *
         * The vector loop runs the body once per VF elements, so ANY
         * instruction in it that is not part of the lane-wise work
         * happens a quarter as many times. A call is the obvious one --
         * `for (...) { a[i]++; g(); }` vectorized and called g() 256
         * times instead of 1024, which is a wrong answer with nothing
         * wrong-looking about it -- but so is a volatile access, a
         * fence, an atomic, or a store to anything but the arrays being
         * walked.
         *
         * The rule is therefore positive rather than a list of banned
         * opcodes: every instruction here is either the loop's own
         * control, one of the memory references already checked to be
         * base + i*esize, the reduction, or something PURE. is_pure is
         * exactly "no side effect and cannot fault", which is the
         * property needed -- a pure value computed a quarter as often
         * is only a wasted computation, not a changed program. */
        for (int n = L.lo; n < L.hi && ok; n++) {
            struct ir_ins *i = &fn->ins[n];
            if (n == L.step_ins || n == L.copy_ins || n == L.cmp_ins ||
                n == L.red_add || n == L.red_copy || n == L.hi - 1)
                continue;
            if (i->op == IR_LABEL || i->op == IR_LOAD || i->op == IR_STORE)
                continue;               /* the references, already checked */
            if (!is_pure(i->op) || i->vol)
                ok = 0;
        }
        if (!ok) {
            VDBG("h=%d the body does something a quarter as often\n", h);
            free(vec); continue;
        }

        /* ---- and nothing may carry a value out of it ----------------
         *
         * A pure computation is safe to run fewer times only if nobody
         * reads the result afterwards. `m = i` inside the loop leaves m
         * at the last VECTOR index, not the last one -- and in the
         * runtime form, where the remainder loop may not run at all, it
         * leaves the original temp never written. The induction
         * variable is the exception: it ends at the trip count either
         * way. The accumulator is the other, and it is folded by hand. */
        for (int n = L.lo; n < L.hi && ok; n++) {
            int t = def_target(&fn->ins[n]);
            if (t < 0 || t >= fn->nvregs || t == L.iv || t == L.red_acc)
                continue;
            for (int m = 0; m < fn->nins && ok; m++)
                if ((m < L.lo || m >= L.hi) && ins_reads(&fn->ins[m], t)) {
                    VDBG("h=%d temp %%%d (def at %d) read at %d; iv=%%%d"
                         " acc=%%%d\n", h, t, n, m, L.iv, L.red_acc);
                    ok = 0;
                }
        }
        if (!ok) {
            VDBG("h=%d a value escapes the loop\n", h);
            free(vec); continue;
        }

        /* A value that becomes a vector may only be read by something
         * that also becomes one: a lane-wise op inside the loop, or the
         * store that consumes it. Anything else -- a use after the loop,
         * a scalar op inside it -- would read sixteen bytes as eight. */
        for (int n = 0; n < fn->nins && ok; n++) {
            struct ir_ins *i = &fn->ins[n];
            int inside = n >= L.lo && n < L.hi;
            int t = def_target(i);
            int consumer = inside && (i->op == IR_STORE || n == L.red_add ||
                                      n == L.red_ext ||
                                      (t >= 0 && t < fn->nvregs && vec[t]));
            if (consumer)
                continue;
            for (int v = 0; v < fn->nvregs && ok; v++)
                if (vec[v] && ins_reads(i, v))
                    ok = 0;
        }
        if (!ok) { VDBG("h=%d a vector value escapes\n", h); free(vec); continue; }

        /* ---- rewrite ------------------------------------------------ */
        if (L.bound_reg >= 0 && L.red_ext >= 0) {
            VDBG("h=%d a widening sum with a runtime count\n", h);
            free(vec); continue;        /* the copy path does not widen */
        }
        if (L.bound_reg >= 0) {
            /* A runtime count needs a remainder, so the loop is copied
             * rather than rewritten in place. */
            if (!vec_rewrite_runtime(fn, &d, &L, vec)) {
                VDBG("h=%d the runtime rewrite refused\n", h);
                free(vec); continue;
            }
            free(vec);
            if (remarks_on() && fn->src)
                remark_add("opt", "vectorized", fn->name,
                           "vec/runtime-trip-count", fn->file, fn->line,
                           "%d lanes of %d bytes, with a scalar remainder%s",
                           L.vf, L.esize,
                           L.red_acc >= 0 ? " (a sum reduction)" : "");
            done = 1;
            continue;
        }
        struct ibuf nb = { 0, 0, 0 };
        int *newpos = fn->var_scope_lo
            ? xmalloc((size_t)(fn->nins + 1) * sizeof *newpos) : NULL;
        int vfk = fn->nvregs++;      /* the new induction step, VF */
        int vacc = -1, vzero = -1, redtmp = -1, vacc2 = -1, wlo = -1, whi = -1;
        int wsize = L.esize;            /* the accumulator's lane width */
        if (L.red_acc >= 0) {
            vzero = fn->nvregs++; vacc = fn->nvregs++; redtmp = fn->nvregs++;
            if (L.red_ext >= 0) {
                /* A widening sum needs TWO accumulators: one load's four
                 * narrow lanes become two wide ones twice over. */
                vacc2 = fn->nvregs++; wlo = fn->nvregs++; whi = fn->nvregs++;
                wsize = L.esize * 2;
            }
        }
        for (int n = 0; n < fn->nins; n++) {
            if (n == L.lo) {
                struct ir_ins *k = ib_push(&nb);
                k->op = IR_CONST; k->dst = vfk;
                k->w = fn->ins[L.step_ins].w; k->imm = L.vf;
                k->line = fn->ins[L.step_ins].line;
                k->col = fn->ins[L.step_ins].col;
                if (L.red_acc >= 0) {
                    /* Every lane starts at zero, HERE and not before the
                     * guard: the preheader runs exactly when the loop is
                     * entered, so a loop that runs zero times leaves the
                     * scalar accumulator alone -- and an inner loop is
                     * re-zeroed on every pass of the outer one. */
                    struct ir_ins *z = ib_push(&nb);
                    z->op = IR_CONST; z->dst = vzero;
                    z->w = wsize; z->imm = 0;
                    z->line = fn->ins[L.red_add].line;
                    z->col = fn->ins[L.red_add].col;
                    struct ir_ins *sp = ib_push(&nb);
                    sp->op = IR_VSPLAT; sp->dst = vacc; sp->a = vzero;
                    sp->size = wsize; sp->w = 8;
                    sp->line = z->line; sp->col = z->col;
                    if (vacc2 >= 0) {
                        struct ir_ins *s2 = ib_push(&nb);
                        s2->op = IR_VSPLAT; s2->dst = vacc2; s2->a = vzero;
                        s2->size = wsize; s2->w = 8;
                        s2->line = z->line; s2->col = z->col;
                    }
                }
                /* Before the header label: broadcast every constant the
                 * body needs, once. The back edge jumps past this, the
                 * same way it jumps past a hoisted expression. */
                for (int m = L.lo; m < L.hi; m++) {
                    struct ir_ins *s = &fn->ins[m];
                    int t = def_target(s);
                    long kb;
                    if (t < 0 || t >= fn->nvregs || !vec[t])
                        continue;
                    if (s->op == IR_SHL || s->op == IR_SHR || s->op == IR_MUL)
                        continue;           /* folded into the lane op */
                    if (!const_b(fn, &d, s, &kb))
                        continue;           /* both operands are vectors */
                    struct ir_ins *k = ib_push(&nb);
                    k->op = IR_CONST; k->dst = fn->nvregs;
                    k->w = L.esize; k->imm = kb;
                    k->line = s->line; k->col = s->col;
                    struct ir_ins *sp = ib_push(&nb);
                    sp->op = IR_VSPLAT; sp->dst = fn->nvregs + 1;
                    sp->a = fn->nvregs; sp->size = L.esize;
                    sp->line = s->line; sp->col = s->col;
                    s->c = fn->nvregs + 1;  /* remember the splat for below */
                    fn->nvregs += 2;
                }
            }
            if (newpos) newpos[n] = nb.n;
            struct ir_ins *i = &fn->ins[n];
            if (n < L.lo || n >= L.hi) { *ib_push(&nb) = *i; continue; }

            if (n == L.red_copy)
                continue;               /* the scalar carry is gone */
            if (n == L.red_ext)
                continue;   /* emitted with the add, so each half goes
                             * straight from xmm0 into its accumulator
                             * instead of out to a slot and back */
            if (n == L.red_add) {       /* vacc += the loaded vector */
                if (vacc2 >= 0) {
                    /* widen a half, add it, then the other: each value
                     * is consumed by the instruction after it. */
                    for (int half = 0; half < 2; half++) {
                        struct ir_ins *w2 = ib_push(&nb);
                        memset(w2, 0, sizeof *w2);
                        w2->op = IR_VWIDEN; w2->dst = half ? whi : wlo;
                        w2->a = L.red_vec; w2->c = half;
                        w2->size = L.esize;
                        w2->sign = fn->ins[L.red_ext].sign; w2->w = 8;
                        w2->line = i->line; w2->col = i->col;
                        struct ir_ins *v2 = ib_push(&nb);
                        memset(v2, 0, sizeof *v2);
                        v2->op = IR_VBIN;
                        v2->dst = half ? vacc2 : vacc;
                        v2->a = half ? vacc2 : vacc;
                        v2->b = half ? whi : wlo;
                        v2->imm = '+'; v2->size = wsize; v2->w = 8;
                        v2->line = i->line; v2->col = i->col;
                    }
                    continue;
                }
                struct ir_ins *v = ib_push(&nb);
                memset(v, 0, sizeof *v);
                v->op = IR_VBIN; v->dst = vacc; v->a = vacc;
                v->b = L.red_vec;
                v->imm = '+'; v->size = wsize; v->w = 8;
                v->line = i->line; v->col = i->col;
                continue;
            }
            if (n == L.step_ins) {          /* i += VF, not i += 1 */
                struct ir_ins *s = ib_push(&nb);
                *s = *i;
                /* Through a CONST temp, not imm_b: pass_immfold runs
                 * after this and the passes before it are documented
                 * never to reason about the folded form. */
                s->imm_b = 0; s->imm = 0; s->b = vfk;
                continue;
            }
            int t = def_target(i);
            if (i->op == IR_LOAD && t >= 0 && vec[t]) {
                struct ir_ins *v = ib_push(&nb);
                memset(v, 0, sizeof *v);
                v->op = IR_VLOAD; v->dst = t; v->a = i->a;
                v->size = L.esize; v->w = 8;
                v->line = i->line; v->col = i->col;
                continue;
            }
            if (i->op == IR_STORE) {
                struct ir_ins *v = ib_push(&nb);
                memset(v, 0, sizeof *v);
                v->op = IR_VSTORE; v->a = i->a; v->b = i->b; v->dst = -1;
                v->size = L.esize; v->w = 8;
                v->line = i->line; v->col = i->col;
                continue;
            }
            if (t >= 0 && t < fn->nvregs && vec[t]) {
                int c = vec_op_char(i->op);
                if (i->op == IR_MUL) {
                    long m = 0;
                    (void)const_b(fn, &d, i, &m);
                    if ((m & (m - 1)) == 0) {          /* a power of two */
                        int sh = 0;
                        while ((1L << sh) < m) sh++;
                        struct ir_ins *v = ib_push(&nb);
                        memset(v, 0, sizeof *v);
                        v->op = IR_VBIN; v->dst = t; v->a = i->a;
                        v->b = -1; v->imm = '<'; v->c = sh;
                        v->size = L.esize; v->w = 8;
                        v->line = i->line; v->col = i->col;
                    } else {                            /* (1 << k) + 1 */
                        int sh = 0;
                        while ((1L << sh) < m - 1) sh++;
                        int tmp = fn->nvregs++;
                        struct ir_ins *v = ib_push(&nb);
                        memset(v, 0, sizeof *v);
                        v->op = IR_VBIN; v->dst = tmp; v->a = i->a;
                        v->b = -1; v->imm = '<'; v->c = sh;
                        v->size = L.esize; v->w = 8;
                        v->line = i->line; v->col = i->col;
                        struct ir_ins *w = ib_push(&nb);
                        memset(w, 0, sizeof *w);
                        w->op = IR_VBIN; w->dst = t; w->a = tmp; w->b = i->a;
                        w->imm = '+'; w->size = L.esize; w->w = 8;
                        w->line = i->line; w->col = i->col;
                    }
                    continue;
                }
                struct ir_ins *v = ib_push(&nb);
                memset(v, 0, sizeof *v);
                v->op = IR_VBIN; v->dst = t; v->a = i->a;
                v->size = L.esize; v->w = 8;
                v->imm = c;
                v->line = i->line; v->col = i->col;
                long kb;
                int hask = const_b(fn, &d, i, &kb);
                if ((c == '<' || c == '>') && hask) {
                    v->b = -1; v->c = (int)kb; v->sign = i->sign;
                } else if (hask) {
                    v->b = i->c;              /* the splat made above */
                } else {
                    v->b = i->b;
                }
                continue;
            }
            *ib_push(&nb) = *i;
            if (L.red_acc >= 0 && n == L.hi - 1) {
                /* Right AFTER the back branch and before whatever label
                 * follows, so it runs on the way out of the loop and is
                 * skipped by the guard's jump straight to the exit --
                 * the preheader trick, mirrored. */
                if (vacc2 >= 0) {       /* the two halves, added lane-wise */
                    struct ir_ins *c2 = ib_push(&nb);
                    memset(c2, 0, sizeof *c2);
                    c2->op = IR_VBIN; c2->dst = vacc; c2->a = vacc;
                    c2->b = vacc2; c2->imm = '+'; c2->size = wsize; c2->w = 8;
                    c2->line = fn->ins[L.red_add].line;
                    c2->col = fn->ins[L.red_add].col;
                }
                struct ir_ins *r = ib_push(&nb);
                memset(r, 0, sizeof *r);
                r->op = IR_VREDADD; r->dst = redtmp; r->a = vacc;
                r->size = wsize; r->w = fn->ins[L.red_add].w;
                r->line = fn->ins[L.red_add].line;
                r->col = fn->ins[L.red_add].col;
                struct ir_ins *ad = ib_push(&nb);
                memset(ad, 0, sizeof *ad);
                ad->op = IR_ADD; ad->dst = L.red_acc; ad->a = L.red_acc;
                ad->b = redtmp; ad->w = fn->ins[L.red_add].w;
                ad->sign = fn->ins[L.red_add].sign;
                ad->line = fn->ins[L.red_add].line;
                ad->col = fn->ins[L.red_add].col;
            }
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
        free(vec);
        if (remarks_on() && fn->src)
            remark_add("opt", "vectorized", fn->name, "vec/constant-trip-count",
                       fn->file, fn->line,
                       "%d lanes of %d bytes, %ld iterations -> %ld%s",
                       L.vf, L.esize, L.bound, L.bound / L.vf,
                       L.red_acc >= 0 ? " (a sum reduction)" : "");
        done = 1;
    }

    free(in);
    free_defs(&d);
    free(order); free(l2b);
    for (int i = 0; i < nbb; i++) free(bb[i].pred);
    free(bb);
    return done;
}

static int pass_vectorize(struct ir_func *fn)
{
    int changed = 0, guard = 0;
    while (guard++ < 32 && vectorize_one(fn))
        changed = 1;
    return changed;
}


/* ==== induction-variable strength reduction (-O2) =========================== *
 *
 * `a[i]` costs three instructions an iteration that have nothing to do
 * with a[i]: widen i, shift it by the element size, add the base. The
 * value they compute is base + i*scale, which changes by scale*step
 * every iteration -- so it can be an induction variable of its own,
 * walked by one add, instead of being rebuilt from i each time.
 *
 *      ext  t1, i           p = base                 (preheader)
 *      shl  t2, t1, #2      ...
 *      add  t3, base, t2        load [p]
 *      load [t3]            p = p + 4                (latch)
 *
 * The chain then has no users and DCE takes it, so three instructions
 * become one. It is the oldest loop optimization there is and it
 * applies to every array loop, vectorized or not: in a vectorized loop
 * the index steps by VF, so the pointer steps by 16 and the saving is
 * the same.
 *
 * ---- why this runs last ----
 *
 * It DESTROYS the shape the vectorizer matches on. addr_of_iv looks for
 * exactly `add(base, shl(ext(i), k))`, and a loop whose addressing has
 * already been reduced to a walking pointer does not have it. So this
 * runs once, after the loop passes have converged, and never inside
 * their round -- otherwise it would race the vectorizer for the same
 * loops and win, and the lanes would be lost to save an add.
 *
 * ---- what may be reduced ----
 *
 * The value must be linear in the induction variable with a
 * loop-invariant base, and -- the part that is easy to forget -- it must
 * not be read after the loop. The new pointer ends one step PAST where
 * the old chain last computed, because it is incremented at the latch
 * like the induction variable itself, so a use outside the loop would
 * see base + n*scale where the chain would have given base + (n-1)*scale.
 */

/* Is `v` the value base + iv*scale, with base invariant? Fills base and
 * scale. The chain is irgen's: an optional widening of the index, a
 * shift by the log of the element size, and an add. */
static int linear_in_iv(struct ir_func *fn, struct defs *d, int lo, int hi,
                        int v, int iv, int *base_out, long *scale_out)
{
    if (v < 0 || v >= fn->nvregs || d->cnt[v] != 1)
        return 0;
    int an = d->ins[v];
    if (an < lo || an >= hi || fn->ins[an].op != IR_ADD)
        return 0;
    struct ir_ins *add = &fn->ins[an];
    for (int side = 0; side < 2; side++) {
        int base = side ? add->b : add->a;
        int idx  = side ? add->a : add->b;
        if (base < 0 || base >= fn->nvregs || d->cnt[base] != 1)
            continue;
        int bd = d->ins[base];
        if (bd >= lo && bd < hi)
            continue;                    /* the base must not move */
        if (idx < 0 || idx >= fn->nvregs || d->cnt[idx] != 1)
            continue;
        int sn = d->ins[idx];
        if (sn < lo || sn >= hi || fn->ins[sn].op != IR_SHL)
            continue;
        long sh;
        if (!const_b(fn, d, &fn->ins[sn], &sh) || sh < 0 || sh > 60)
            continue;
        int x = fn->ins[sn].a;
        if (x >= 0 && x < fn->nvregs && d->cnt[x] == 1) {
            int xn = d->ins[x];
            if (xn >= lo && xn < hi && fn->ins[xn].op == IR_EXT)
                x = fn->ins[xn].a;
        }
        if (x != iv)
            continue;
        *base_out = base;
        *scale_out = 1L << sh;
        return 1;
    }
    return 0;
}

static int ivsr_one(struct ir_func *fn)
{
    if (fn->nins == 0)
        return 0;
    int nbb, *l2b;
    struct bb *bb = build_cfg(fn, &nbb, &l2b);
    int *order = xmalloc((size_t)nbb * sizeof *order), norder;
    compute_rpo(bb, nbb, order, &norder);
    if (norder != nbb) {
        free(order); free(l2b);
        for (int i = 0; i < nbb; i++) free(bb[i].pred);
        free(bb); return 0;
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
        for (int p = 0; p < nbb; p++)
            for (int k = 0; k < bb[p].nsucc; k++)
                if (bb[p].succ[k] == h && bb_dominates(bb, h, p)) {
                    latch = p; nback++;
                }
        if (nback != 1 || bb[latch].end <= bb[latch].start)
            continue;
        struct ir_ins *br = &fn->ins[bb[latch].end - 1];
        if (br->op != IR_BRNZ || br->label != Lh)
            continue;
        memset(in, 0, (size_t)nbb);
        loop_body(bb, nbb, h, latch, in);
        int lo = bb[h].start, hi = bb[latch].end, ok = 1;
        for (int b = 0; b < nbb && ok; b++)
            if (in[b] != (bb[b].start >= lo && bb[b].end <= hi))
                ok = 0;
        if (!ok)
            continue;

        /* the induction variable and its step, as the vectorizer finds them */
        int copy_ins = -1;
        for (int n = lo; n < hi; n++)
            if (fn->ins[n].op == IR_MOV && fn->ins[n].dst >= 0 &&
                d.cnt[fn->ins[n].dst] == 2) {
                int nxt = fn->ins[n].a;
                if (nxt < 0 || nxt >= fn->nvregs || d.cnt[nxt] != 1)
                    continue;
                int sn = d.ins[nxt];
                if (sn < lo || sn >= hi || fn->ins[sn].op != IR_ADD)
                    continue;
                long st;
                if (fn->ins[sn].a != fn->ins[n].dst ||
                    !const_b(fn, &d, &fn->ins[sn], &st))
                    continue;
                copy_ins = n;
                break;
            }
        if (copy_ins < 0)
            continue;
        int iv = fn->ins[copy_ins].dst;
        int step_ins = d.ins[fn->ins[copy_ins].a];
        long step = 0;
        (void)const_b(fn, &d, &fn->ins[step_ins], &step);
        if (step <= 0)
            continue;

        /* Where the walk happens: immediately before the compare that
         * feeds the back branch, which is AFTER every phi copy in the
         * latch.
         *
         * Putting it next to `i += 1` looked natural and was wrong. The
         * phi copies sit after the step -- `p = mov <the address>` among
         * them -- so the pointer had already moved on by the time one
         * was taken, and `p = &a[i]` came out as &a[i+1]. The answer
         * differed at -O2 and nowhere else. */
        int inc_at = -1;
        if (br->a >= 0 && br->a < fn->nvregs && d.cnt[br->a] == 1)
            inc_at = d.ins[br->a];
        if (inc_at <= lo || inc_at >= hi) {
            VDBG("ivsr h=%d no compare to insert before (br->a=%d cnt=%d"
                 " inc_at=%d lo=%d hi=%d)\n", h, br->a,
                 br->a >= 0 && br->a < fn->nvregs ? d.cnt[br->a] : -1,
                 inc_at, lo, hi);
            continue;
        }

        /* the induction variable must start at a known place, because
         * the new pointer has to be initialised to match it */
        int init_zero = 1;
        for (int n = 0; n < fn->nins && init_zero; n++) {
            if (n >= lo && n < hi)
                continue;
            struct ir_ins *i = &fn->ins[n];
            if (def_target(i) != iv)
                continue;
            if (!(i->op == IR_MOV && i->a >= 0 && i->a < fn->nvregs &&
                  d.cnt[i->a] == 1 && d.ins[i->a] >= 0 &&
                  fn->ins[d.ins[i->a]].op == IR_CONST &&
                  fn->ins[d.ins[i->a]].imm == 0))
                init_zero = 0;
        }
        if (!init_zero)
            continue;

        /* every candidate: an address computed from the index, whose
         * value nothing outside the loop reads */
        int cand[16], cbase[16], nc = 0;
        long cscale[16];
        for (int n = lo; n < hi && nc < 16; n++) {
            int t = def_target(&fn->ins[n]);
            int base; long scale;
            if (t < 0 || fn->ins[n].op != IR_ADD)
                continue;
            if (!linear_in_iv(fn, &d, lo, hi, t, iv, &base, &scale))
                continue;
            int escapes = 0;
            for (int m = 0; m < fn->nins && !escapes; m++)
                if ((m < lo || m >= hi) && ins_reads(&fn->ins[m], t))
                    escapes = 1;
            if (escapes)
                continue;
            int dup = 0;
            for (int k = 0; k < nc; k++)
                if (cand[k] == t) dup = 1;
            if (dup)
                continue;
            cand[nc] = t; cbase[nc] = base; cscale[nc] = scale; nc++;
        }
        if (nc == 0)
            continue;

        /* ---- rewrite: a pointer per candidate ---- */
        int ptr[16], delta[16];
        for (int k = 0; k < nc; k++) {
            ptr[k] = fn->nvregs++;
            delta[k] = fn->nvregs++;
        }
        struct ibuf nb = { 0, 0, 0 };
        int *newpos = fn->var_scope_lo
            ? xmalloc((size_t)(fn->nins + 1) * sizeof *newpos) : NULL;
        for (int n = 0; n < fn->nins; n++) {
            if (n == lo) {
                for (int k = 0; k < nc; k++) {
                    struct ir_ins *c = ib_push(&nb);
                    c->op = IR_CONST; c->dst = delta[k];
                    c->w = 8; c->imm = cscale[k] * step;
                    c->line = fn->ins[cand[k] >= 0 ? d.ins[cand[k]] : n].line;
                    c->synth = 1;
                    struct ir_ins *m = ib_push(&nb);
                    m->op = IR_MOV; m->dst = ptr[k]; m->a = cbase[k];
                    m->w = 8;
                    m->line = c->line; m->synth = 1;
                }
            }
            if (n == inc_at) {          /* walk each pointer, after the copies */
                for (int k = 0; k < nc; k++) {
                    struct ir_ins *a2 = ib_push(&nb);
                    a2->op = IR_ADD; a2->dst = ptr[k]; a2->a = ptr[k];
                    a2->b = delta[k]; a2->w = 8;
                    a2->line = fn->ins[n].line; a2->synth = 1;
                }
            }
            if (newpos) newpos[n] = nb.n;
            /* the chain's add disappears; its users read the pointer */
            int skip = 0;
            for (int k = 0; k < nc; k++)
                if (def_target(&fn->ins[n]) == cand[k] && n >= lo && n < hi)
                    skip = 1;
            if (skip)
                continue;
            struct ir_ins *o = ib_push(&nb);
            *o = fn->ins[n];
            for (int k = 0; k < nc; k++) {
                struct lcopy lc;
                int *tbl = xmalloc((size_t)fn->nvregs * sizeof *tbl);
                for (int v = 0; v < fn->nvregs; v++) tbl[v] = -1;
                tbl[cand[k]] = ptr[k];
                lc.cp = tbl; lc.nv = fn->nvregs; lc.n = 0;
                each_read(o, lcopy_cb, &lc);
                free(tbl);
            }

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
        if (remarks_on() && fn->src)
            remark_add("opt", "strength-reduced", fn->name, "ivsr/address",
                       fn->file, fn->line,
                       "%d address%s walked instead of recomputed",
                       nc, nc == 1 ? "" : "es");
        done = 1;
    }

    free(in); free_defs(&d);
    free(order); free(l2b);
    for (int i = 0; i < nbb; i++) free(bb[i].pred);
    free(bb);
    return done;
}

static int pass_ivsr(struct ir_func *fn)
{
    int changed = 0, guard = 0;
    while (guard++ < 32 && ivsr_one(fn))
        changed = 1;
    return changed;
}

/* ---- conditional constant propagation (the reachability half of SCCP) ----
 *
 * A conditional branch whose condition is a known constant has one live edge.
 * Resolve it (BRZ/BRNZ -> unconditional jump, or fall-through), then drop every
 * block no longer reachable over the live edges. The constant conditions come
 * from inlining a call with a constant argument, mem2reg + folding collapsing a
 * flag, config constants — code the earlier passes leave as `test; jz` over a
 * value they have already proven constant, plus the now-dead arm behind it.
 *
 * One rebuild handles both: emit each reachable block, replacing a resolved
 * branch with a jump to its live successor (or nothing when that successor is
 * the fall-through), and skip unreachable blocks entirely. */
static int pass_sccp(struct ir_func *fn)
{
    if (fn->nins == 0)
        return 0;
    struct defs d;
    compute_defs(fn, &d);
    int nbb, *l2b;
    struct bb *bb = build_cfg(fn, &nbb, &l2b);

    /* live_only[b] = index into bb[b].succ of the sole live edge, or -1 = all.
     * succ[0] is the branch-taken target, succ[1] the fall-through. */
    int *live_only = xmalloc((size_t)nbb * sizeof *live_only);
    for (int b = 0; b < nbb; b++) {
        live_only[b] = -1;
        if (bb[b].end <= bb[b].start)
            continue;
        struct ir_ins *t = &fn->ins[bb[b].end - 1];
        if ((t->op != IR_BRZ && t->op != IR_BRNZ) || t->a < 0 ||
            d.cnt[t->a] != 1 || d.ins[t->a] < 0 ||
            fn->ins[d.ins[t->a]].op != IR_CONST)
            continue;
        long v = fn->ins[d.ins[t->a]].imm;
        int taken = (t->op == IR_BRZ) ? (v == 0) : (v != 0);
        int want = taken ? 0 : 1;
        if (want < bb[b].nsucc) {     /* only if that edge actually exists */
            live_only[b] = want;
            /* A branch whose condition is a constant always goes the same
             * way, which is worth saying out loud: it is as often a bug in
             * the program as an optimization in the compiler.
             *
             * `taken` means the BRANCH jumps, not that the source condition
             * was true -- for `if (c)` irgen emits `BRZ c -> else`, so a
             * taken branch is a FALSE condition. Rather than guess at the
             * source's polarity from the lowering, the remark states the IR
             * fact, which is the one that is certainly true. */
            remark_add("sccp",
                       taken ? "branch-always-jumps" : "branch-never-jumps",
                       fn->src ? fn->name : NULL,
                       "condition-is-a-constant",
                       fn->src ? fn->file : NULL, t->line,
                       "the condition folded to %ld, so one arm is "
                       "unreachable", v);
        }
    }

    /* Reachability over the live edges only. */
    char *reach = xcalloc((size_t)nbb, 1);
    int *wl = xmalloc((size_t)nbb * sizeof *wl), nwl = 0;
    reach[0] = 1; wl[nwl++] = 0;
    while (nwl) {
        int b = wl[--nwl];
        for (int s = 0; s < bb[b].nsucc; s++) {
            if (live_only[b] >= 0 && s != live_only[b])
                continue;
            int sb = bb[b].succ[s];
            if (!reach[sb]) { reach[sb] = 1; wl[nwl++] = sb; }
        }
    }

    int work = 0;
    for (int b = 0; b < nbb; b++)
        if (!reach[b] || live_only[b] >= 0) { work = 1; break; }
    if (!work) {
        free(live_only); free(reach); free(wl); free(l2b);
        for (int i = 0; i < nbb; i++) free(bb[i].pred);
        free(bb); free_defs(&d);
        return 0;
    }

    struct ibuf nb = { 0, 0, 0 };
    for (int b = 0; b < nbb; b++) {
        if (!reach[b])
            continue;                       /* unreachable: drop the whole block */
        if (live_only[b] >= 0) {
            for (int n = bb[b].start; n < bb[b].end - 1; n++)  /* body, not branch */
                *ib_push(&nb) = fn->ins[n];
            int ls = bb[b].succ[live_only[b]];
            /* Jump to the live successor by label; if it has none it is the
             * fall-through (b+1, reachable, emitted next) — just fall in. */
            if (bb[ls].end > bb[ls].start && fn->ins[bb[ls].start].op == IR_LABEL) {
                struct ir_ins *j = ib_push(&nb);
                j->op = IR_JMP; j->dst = -1; j->a = -1; j->b = -1;
                j->label = fn->ins[bb[ls].start].label;
                /* It replaces the branch that ended this block, so it is
                 * that branch's `if` as far as the programmer is concerned
                 * (R3) -- not a jump from nowhere. */
                j->line = fn->ins[bb[b].end - 1].line;
                j->col = fn->ins[bb[b].end - 1].col;
                j->synth = fn->ins[bb[b].end - 1].synth;
            }
        } else {
            for (int n = bb[b].start; n < bb[b].end; n++)
                *ib_push(&nb) = fn->ins[n];
        }
    }
    free(fn->ins);
    fn->ins = nb.p; fn->nins = nb.n; fn->cap = nb.cap;

    /* The rebuild renumbered every instruction, so the local scope ranges (used
     * by codegen to coalesce disjoint-lifetime locals) are stale — drop them,
     * as mem2reg does; each surviving local then takes its own slot. */
    if (fn->var_scope_lo) {
        free(fn->var_scope_lo); free(fn->var_scope_hi);
        fn->var_scope_lo = fn->var_scope_hi = NULL;
    }

    free(live_only); free(reach); free(wl); free(l2b);
    for (int i = 0; i < nbb; i++) free(bb[i].pred);
    free(bb); free_defs(&d);
    return 1;
}

/* ---- local store-forwarding (a lightweight mem2reg) ----
 *
 * EmbIR keeps locals in memory (STVAR/LDVAR). Within an extended basic block a
 * store `STVAR L, v` makes every later `LDVAR L` yield v — until the next store
 * to L or a control-flow join. Forwarding v turns the reload into a copy that
 * copyprop/DCE then erase. Sound only for a local that is never address-taken
 * (so no aliased write can change it) and a full-width plain load (size 4 or 8,
 * no narrowing/extension between the store and the load). This is what lets an
 * inlined body's parameter plumbing (STVAR param, arg; LDVAR param) collapse to
 * the argument, so folding flows through the inline. */
static int sf_plain(int size, int sign, int w)
{
    return size == 8 || (size == 4 && !(sign && w == 8));
}

static int pass_storefwd(struct ir_func *fn)
{
    int nvars = fn->nvars;
    if (nvars == 0)
        return 0;
    char *taken = xcalloc((size_t)fn->nvregs, 1);
    for (int i = 0; i < fn->nins; i++)
        if (fn->ins[i].op == IR_ADDR && fn->ins[i].a >= 0 &&
            fn->ins[i].a < fn->nvregs)
            taken[fn->ins[i].a] = 1;
    int *cur = xmalloc((size_t)nvars * sizeof *cur);
    for (int v = 0; v < nvars; v++) cur[v] = -1;
    int changed = 0;
    for (int i = 0; i < fn->nins; i++) {
        struct ir_ins *in = &fn->ins[i];
        if (in->op == IR_LABEL) {                 /* a join: values may differ */
            for (int v = 0; v < nvars; v++) cur[v] = -1;
        } else if (in->op == IR_STVAR) {
            int L = in->dst;
            if (L >= 0 && L < nvars)
                cur[L] = (!taken[L] && sf_plain(in->size, 0, in->size))
                             ? in->a : -1;
        } else if (in->op == IR_LDVAR) {
            int L = in->a;
            if (L >= 0 && L < nvars && !taken[L] && cur[L] >= 0 &&
                sf_plain(in->size, in->sign, in->w)) {
                in->op = IR_MOV;                  /* LDVAR L -> MOV of the stored temp */
                in->a = cur[L];
                in->b = -1;
                changed = 1;
            }
        }
    }
    free(taken); free(cur);
    return changed;
}

/* ---- immediate-operand folding ---- */

/* x86 ALU/compare immediates are imm32 (sign-extended to 64). A value outside
 * that range must stay in a register. */
static int fits_imm32(long v)
{
    return v >= -2147483647L - 1 && v <= 2147483647L;
}

/* The predicate when a comparison's operands are swapped: `a < b` becomes
 * `b > a`, so folding a constant `a` into `cmp b, imm` flips the direction. */
static enum binop swap_pred(enum binop p)
{
    switch (p) {
    case B_LT: return B_GT; case B_GT: return B_LT;
    case B_LE: return B_GE; case B_GE: return B_LE;
    default:   return p;   /* EQ/NE are symmetric */
    }
}

/* Fold a constant operand of an integer ALU/compare op into an immediate, so
 * the value need not be materialised in a register. Run once AFTER the main
 * fixpoint (fold/lvn/copyprop never see the imm_b form) and followed by DCE,
 * which drops the CONSTs that folding left unreferenced. */
static int pass_immfold(struct ir_func *fn)
{
    struct defs d;
    compute_defs(fn, &d);
    int changed = 0;
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        if (i->flt || i->imm_b || i->w == 16)
            continue;           /* see pass_fold on the 128-bit width */
        long A, B;
        int commutative;
        /* Shifts fold only their count (b), and only a valid small one — the
         * value being shifted (a) is not an immediate operand. */
        if (i->op == IR_SHL || i->op == IR_SHR) {
            if (get_const(fn, &d, i->b, &B) && B >= 0 && B <= 63) {
                i->imm = B; i->imm_b = 1; i->b = -1;
                changed = 1;
            }
            continue;
        }
        switch (i->op) {
        case IR_ADD: case IR_MUL: case IR_AND: case IR_OR: case IR_XOR:
            commutative = 1; break;
        case IR_SUB: case IR_CMP:
            commutative = 0; break;
        default:
            continue;
        }
        if (get_const(fn, &d, i->b, &B) && fits_imm32(B)) {
            i->imm = B; i->imm_b = 1; i->b = -1;    /* op a, imm */
            changed = 1;
        } else if ((commutative || i->op == IR_CMP) &&
                   get_const(fn, &d, i->a, &A) && fits_imm32(A)) {
            /* Constant in the first operand: move it to the immediate, keeping
             * a valid instruction — commutative ops just swap, a compare swaps
             * and flips its predicate. */
            i->a = i->b; i->b = -1; i->imm = A; i->imm_b = 1;
            if (i->op == IR_CMP)
                i->pred = swap_pred(i->pred);
            changed = 1;
        }
    }
    free_defs(&d);
    return changed;
}

/* ---- function inlining (inter-procedural, -O2) --------------------------- *
 *
 * A call to a small, defined function is replaced by the function's body. The
 * callee's params and locals become caller LOCALS (the memory model EmbIR uses
 * for addressable, possibly-reassigned variables — treating them as temps would
 * break codegen's single-assignment / no-alias assumptions), so the caller's
 * temps renumber UP to open a contiguous local range for them, and its
 * var_tys/var_aligns/scope metadata extend to match. Params are initialised by
 * an STVAR of each argument; every RET becomes `MOV result` + a jump to one
 * shared label after the inlined body. Gated to -O2, so -O0/-O1 are untouched. */

#define INLINE_MAX_CALLEE 24     /* instruction budget for an inline candidate */
#define INLINE_MAX_CALLER 800    /* stop expanding a caller past this many ins */
#define INLINE_MAX_PER_FUNC 64   /* and cap inlines per caller, for termination */

/* Vreg remap over one instruction. kind 0 = caller shift (a temp >= p1 moves up
 * by p2); kind 1 = callee map (a callee local < p3 -> p1+x, a temp -> p2+x). */
struct rmp { int kind, p1, p2, p3, lbase; };

static int vmap(const struct rmp *r, int x)
{
    if (x < 0) return x;
    if (r->kind == 0) return x < r->p1 ? x : x + r->p2;
    return x < r->p3 ? r->p1 + x : r->p2 + x;
}
static void rmp_cb(int *p, void *ctx) { *p = vmap(ctx, *p); }

static void remap_ins(struct ir_ins *in, struct rmp *r)
{
    each_read(in, rmp_cb, r);
    /* IR_LANDING's SECOND destination, for the same reason compute_defs
     * has to name it: def_target reports one, and a landing pad left with
     * the callee's numbering for its selector reads a vreg that does not
     * exist in the caller. Inlining a `noexcept` function is enough to
     * reach this -- its pad comes along with it. */
    if (in->op == IR_LANDING) {
        in->dst = vmap(r, in->dst);
        in->b = vmap(r, in->b);
    } else if (def_target(in) >= 0) {
        in->dst = vmap(r, in->dst);
    }
    if (in->op == IR_JMP || in->op == IR_BRZ || in->op == IR_BRNZ ||
        in->op == IR_LABEL)
        in->label += r->lbase;
}

/* The ir_func for a callee, or NULL if not defined in this unit. */
static struct ir_func *func_ir(struct ir_unit *iu, struct func *callee)
{
    for (int i = 0; i < iu->nfuncs; i++)
        if (iu->funcs[i].src == callee)
            return &iu->funcs[i];
    return NULL;
}

/* Conservative eligibility: a real, small body; scalar-integer params and
 * return only (no varargs / struct / float); no inline asm, va_start, or a
 * struct-returning call in the body. */
/* Each refusal has its OWN reason code (R2). This function used to return 0
 * from a dozen places and every one of them meant something different; by
 * the time anyone asked why a function was not inlined, the twelve answers
 * had collapsed into one. `*why` is the stable code, `*detail` the fact. */
static int inlinable(struct ir_func *cf, int force, const char **why,
                     char *detail, size_t dcap)
{
    struct func *c = cf->src;
    *why = NULL;
    if (detail && dcap)
        detail[0] = 0;

    if (c->is_varargs)       { *why = "callee-is-varargs";  return 0; }
    if (cf->nins == 0)       { *why = "callee-not-defined-here"; return 0; }
    /* __attribute__((always_inline)) overrides the SIZE budget and
     * nothing else. Every other test below is a thing this inliner
     * cannot do rather than a thing it decided against -- forcing one
     * would not inline the call, it would emit a wrong one. The remark
     * still names whichever test refused, so a function marked
     * always_inline that was not inlined says why. */
    if (!force && cf->nins > INLINE_MAX_CALLEE) {
        *why = "callee-too-large";
        if (detail)
            snprintf(detail, dcap, "%d instructions, budget %d",
                     cf->nins, INLINE_MAX_CALLEE);
        return 0;
    }
    if (cf->neh)             { *why = "callee-has-exception-regions"; return 0; }
    if (c->ret_ty->kind == TY_STRUCT) { *why = "returns-a-struct"; return 0; }
    if (ty_is_float(c->ret_ty))       { *why = "returns-floating-point"; return 0; }
    for (int k = 0; k < cf->nvars; k++)
        if (c->var_tys[k] &&
            (c->var_tys[k]->kind == TY_STRUCT || ty_is_float(c->var_tys[k]))) {
            *why = "callee-has-a-struct-or-float-local";
            return 0;
        }
    for (int i = 0; i < cf->nins; i++) {
        const struct ir_ins *in = &cf->ins[i];
        if (in->op == IR_ASM)      { *why = "callee-has-inline-asm"; return 0; }
        if (in->op == IR_VA_START) { *why = "callee-uses-va_start"; return 0; }
        if (in->flt)               { *why = "callee-computes-in-floating-point";
                                     return 0; }
        /* a VLA's allocation is released by the callee's own epilogue;
         * inlined into a loop it would never be */
        if (in->op == IR_ALLOCA)   { *why = "callee-has-a-vla"; return 0; }
        /* Computed goto: a label address / indirect jump can't be inlined —
         * the callee's label ids would need remapping into the caller, and the
         * caller then can't be optimized either (opt_func bails on it). */
        if (in->op == IR_LABELADDR || in->op == IR_IGOTO) {
            *why = "callee-uses-a-computed-goto";
            return 0;
        }
        if (in->op == IR_CALL && in->retsize) {
            *why = "callee-calls-a-struct-returning-function";
            return 0;
        }
    }
    return 1;
}

/* Splice the body of cf in place of the call at fn->ins[ci]. */
static void inline_call(struct ir_func *fn, int ci, struct ir_func *cf)
{
    int V = fn->nvars, N = fn->nvregs, L = fn->nlabels;
    int v = cf->nvars, n = cf->nvregs, nparams = cf->nparams;

    /* 1. Open room: shift the caller's temps up by v (locals stay put). */
    struct rmp shift = { 0, V, v, 0, 0 };
    for (int i = 0; i < fn->nins; i++)
        remap_ins(&fn->ins[i], &shift);

    struct ir_ins call = fn->ins[ci];    /* the (now-shifted) call */
    int dst = call.dst;
    int after = L + cf->nlabels;

    /* 2. Build param stores + the remapped body + one exit label. */
    struct ir_ins *buf = xmalloc((size_t)(nparams + 2 * cf->nins + 1) * sizeof *buf);
    int m = 0;
    /* Every instruction the inliner INVENTS still has a source location:
     * the call it replaces (R3). A pass that builds an instruction from
     * scratch and leaves line 0 puts a hole in the line table, and nothing
     * downstream notices -- which is exactly what the verifier's location
     * check now catches, and what it caught here. */
    for (int k = 0; k < nparams; k++) {
        struct ir_ins *s = &buf[m++];
        memset(s, 0, sizeof *s);
        s->op = IR_STVAR;
        s->dst = V + k;                  /* callee param -> caller local */
        s->a = call.argv[k].vreg;
        s->size = cf->locals[k].size;
        s->line = call.line;             /* the argument was written there */
        s->col = call.col;
    }
    struct rmp cm = { 1, V, N, v, L };
    for (int i = 0; i < cf->nins; i++) {
        struct ir_ins in = cf->ins[i];
        remap_ins(&in, &cm);
        if (in.op == IR_RET) {
            if (in.a >= 0 && dst >= 0) {
                struct ir_ins *mv = &buf[m++];
                memset(mv, 0, sizeof *mv);
                mv->op = IR_MOV; mv->dst = dst; mv->a = in.a;
                mv->line = in.line;      /* the callee's `return` */
                mv->col = in.col;
            }
            if (i != cf->nins - 1) {     /* the last RET falls into `after` */
                struct ir_ins *jp = &buf[m++];
                memset(jp, 0, sizeof *jp);
                jp->op = IR_JMP; jp->label = after;
                jp->line = in.line;
                jp->col = in.col;
            }
        } else {
            buf[m++] = in;
        }
    }
    struct ir_ins *lb = &buf[m++];
    memset(lb, 0, sizeof *lb);
    lb->op = IR_LABEL; lb->label = after;
    /* The join label belongs to no source construct: it exists only because
     * the body was spliced in. This is the §9.1 exception, marked rather
     * than left as an absent location the verifier cannot tell from a bug. */
    lb->synth = 1;

    /* 3. Splice buf over the call. */
    int newn = fn->nins - 1 + m;
    struct ir_ins *ni = xmalloc((size_t)newn * sizeof *ni);
    memcpy(ni, fn->ins, (size_t)ci * sizeof *ni);
    memcpy(ni + ci, buf, (size_t)m * sizeof *ni);
    memcpy(ni + ci + m, fn->ins + ci + 1,
           (size_t)(fn->nins - ci - 1) * sizeof *ni);
    free(fn->ins); free(buf);
    fn->ins = ni; fn->nins = newn; fn->cap = newn;

    /* 4. Grow vreg/label space and the caller's var metadata. */
    fn->nvregs = N + n;
    fn->nlabels = L + cf->nlabels + 1;
    int nv = V + v;
    struct type **vt = xmalloc((size_t)(nv ? nv : 1) * sizeof *vt);
    int *va = xmalloc((size_t)(nv ? nv : 1) * sizeof *va);
    for (int k = 0; k < V; k++) { vt[k] = fn->src->var_tys[k]; va[k] = fn->src->var_aligns[k]; }
    for (int k = 0; k < v; k++) { vt[V + k] = cf->src->var_tys[k]; va[V + k] = cf->src->var_aligns[k]; }
    fn->src->var_tys = vt;
    fn->src->var_aligns = va;
    /* Both representations grow together: the IR's per-slot descriptors are
     * rebuilt from the types the inliner just extended. Letting them drift
     * is exactly the split brain that miscompiled same-scope. The new count
     * is `nv`; fn->nvars is not updated until the end of this function, and
     * fn->src->nvars is stale by design. */
    ir_locals_fill(fn, fn->src, nv);

    /* Scope ranges are instruction indices; the splice inserted (m-1) net at ci.
     * Shift every existing endpoint past ci, and scope the new callee locals to
     * the inlined region. Kept precise so local-slot coalescing still works. */
    if (fn->var_scope_lo) {
        int *lo = xmalloc((size_t)nv * sizeof *lo);
        int *hi = xmalloc((size_t)nv * sizeof *hi);
        int d = m - 1;
        for (int k = 0; k < V; k++) {
            int a = fn->var_scope_lo[k], b = fn->var_scope_hi[k];
            lo[k] = a <= ci ? a : a + d;
            hi[k] = b <= ci ? b : b + d;
        }
        for (int k = 0; k < v; k++) { lo[V + k] = ci; hi[V + k] = ci + m; }
        free(fn->var_scope_lo); free(fn->var_scope_hi);
        fn->var_scope_lo = lo; fn->var_scope_hi = hi;
    }
    fn->nvars = nv;
}

/* Inline eligible calls across the unit (a bounded fixpoint per caller). */
static void inline_unit(struct ir_unit *iu)
{
    for (int f = 0; f < iu->nfuncs; f++) {
        struct ir_func *fn = &iu->funcs[f];
        int done = 0;
        for (;;) {
            if (done >= INLINE_MAX_PER_FUNC || fn->nins > INLINE_MAX_CALLER ||
                fn->neh || fn->has_i128)
                break;
            int ci = -1;
            struct ir_func *cf = NULL;
            for (int i = 0; i < fn->nins; i++) {
                struct ir_ins *in = &fn->ins[i];
                if (in->op != IR_CALL || in->indirect || !in->callee ||
                    in->retsize)
                    continue;
                struct ir_func *c = func_ir(iu, in->callee);
                const char *why = NULL;
                char detail[160] = "";
                int ok = 0;
                if (!c)
                    why = "callee-not-defined-here";
                else if (c == fn)
                    why = "would-be-recursive";
                else if (c->has_i128)
                    why = "callee-computes-in-__int128";
                else if (in->callee->attr_noinline)
                    why = "callee-is-noinline";
                else
                    ok = inlinable(c, in->callee->attr_always_inline,
                                   &why, detail, sizeof detail);
                if (!ok) {
                    remark_add("inline", "not-inlined", in->callee->name, why,
                               fn->src ? fn->file : NULL, in->line,
                               detail[0] ? "%s" : NULL, detail);
                    continue;
                }
                ci = i;
                cf = c;
                break;
            }
            if (ci < 0)
                break;
            remark_add("inline", "inlined", cf->name,
                       fn->ins[ci].callee->attr_always_inline
                           ? "always_inline" : "small-enough",
                       fn->src ? fn->file : NULL, fn->ins[ci].line,
                       "%d instructions into %s, budget %d", cf->nins,
                       fn->src ? fn->name : "?", INLINE_MAX_CALLEE);
            inline_call(fn, ci, cf);
            done++;
        }
    }
}

/* ---- the CFG, as a report (opt.h, vision §18) ----
 *
 * Built by build_cfg, the same function mem2reg, global CSE and SCCP use,
 * so what this prints is the graph the passes reason about. Dominators are
 * computed too, because "which block dominates which" is the question a
 * mem2reg or a CSE bug always turns into.
 */
void opt_cfg_dump(struct outbuf *b, struct ir_func *fn)
{
    if (fn->nins == 0) {
        ob_fmt(b, "function %s: no instructions\n", fn->name);
        return;
    }
    int nbb, *l2b;
    struct bb *bb = build_cfg(fn, &nbb, &l2b);
    int *order = xmalloc((size_t)(nbb ? nbb : 1) * sizeof *order), norder;
    compute_rpo(bb, nbb, order, &norder);
    int reachable = norder == nbb;
    if (reachable)
        compute_idom(bb, order, norder);

    ob_fmt(b, "function %s: %d blocks, %d instructions\n",
           fn->name, nbb, fn->nins);
    if (!reachable)
        ob_fmt(b, "  (%d block%s unreachable: dominators not computed)\n",
               nbb - norder, nbb - norder == 1 ? "" : "s");
    for (int i = 0; i < nbb; i++) {
        ob_fmt(b, "  B%-3d ins [%d,%d)", i, bb[i].start, bb[i].end);
        /* The label a block carries, when it has one: it is what the
         * instruction dump calls it, so the two can be read together. */
        if (bb[i].end > bb[i].start && fn->ins[bb[i].start].op == IR_LABEL)
            ob_fmt(b, " L%d", fn->ins[bb[i].start].label);
        if (bb[i].end > bb[i].start) {
            const struct ir_ins *t = &fn->ins[bb[i].end - 1];
            ob_fmt(b, "  ends %s", ir_opname(t->op));
            if (t->line)
                ob_fmt(b, " (line %d)", t->line);
        }
        ob_str(b, "\n");
        if (bb[i].npred) {
            ob_str(b, "       from");
            for (int k = 0; k < bb[i].npred; k++)
                ob_fmt(b, " B%d", bb[i].pred[k]);
            ob_str(b, "\n");
        }
        if (bb[i].nsucc) {
            ob_str(b, "       to  ");
            for (int k = 0; k < bb[i].nsucc; k++)
                ob_fmt(b, " B%d", bb[i].succ[k]);
            ob_str(b, "\n");
        } else {
            ob_str(b, "       to   (exit)\n");
        }
        if (reachable && bb[i].idom >= 0 && bb[i].idom != i)
            ob_fmt(b, "       idom B%d\n", bb[i].idom);
        /* A back edge is a successor that dominates this block: that is
         * what makes it a loop, and it is worth naming. */
        for (int k = 0; reachable && k < bb[i].nsucc; k++) {
            int sdom = bb[i].succ[k], q = i;
            while (q >= 0 && q != sdom)
                q = bb[q].idom == q ? -1 : bb[q].idom;
            if (q == sdom)
                ob_fmt(b, "       back edge to B%d (a loop)\n", sdom);
        }
    }
    free(order);
    free(l2b);
    for (int i = 0; i < nbb; i++)
        free(bb[i].pred);
    free(bb);
}

/* ---- driver ---- */

/* ---- the passes, by name -------------------------------------------
 *
 * One table so that -f<name>/-fno-<name>, the -O levels and --help all
 * read the same list, and a pass added without a name here is visibly
 * missing rather than silently unnameable. `forced` records that a flag
 * spoke, so the level does not overwrite what it asked for. */
struct passflag { const char *name; int on; int forced; };
static struct passflag g_pass[] = {
    { "mem2reg",   0, 0 },   /* promote locals to SSA before the fixpoint */
    { "gcse",      0, 0 },   /* dominator-scoped global CSE */
    { "load-cse",  0, 0 },   /* global redundant-load elimination */
    { "sccp",      0, 0 },   /* const-branch resolution + dead-block drop */
    { "licm",      0, 0 },   /* loop invariants, rotation, strength reduction */
    { "vectorize", 0, 0 },   /* lane-wise loops, where the backend has them */
    { "inline",    0, 0 },   /* splice a small callee into its caller */
    { "dse",       0, 0 },   /* drop a store a later one overwrites */
    { "div-magic", 0, 0 },   /* divide by a constant without dividing */
    { "if-convert",0, 0 },   /* a two-way choice without a branch */
    { "cfg-clean", 0, 0 },   /* thread jumps, drop unreachable code */
    { "tail-recursion", 0, 0 }, /* a self tail call becomes a loop */
};
#define NPASS ((int)(sizeof g_pass / sizeof g_pass[0]))
#define P_MEM2REG 0
#define P_GCSE    1
#define P_LOADCSE 2
#define P_SCCP    3
#define P_LICM    4
#define P_VEC     5
#define P_INLINE  6
#define P_DSE     7
#define P_DIVMAGIC 8
#define P_IFCONV   9
#define P_CFGCLEAN 10
#define P_TAILREC  11


int opt_set_pass(const char *name, int on)
{
    for (int i = 0; i < NPASS; i++)
        if (!strcmp(g_pass[i].name, name)) {
            g_pass[i].on = on;
            g_pass[i].forced = 1;
            return 1;
        }
    return 0;
}

int opt_pass_names(const char *const **names)
{
    static const char *n[NPASS];
    for (int i = 0; i < NPASS; i++)
        n[i] = g_pass[i].name;
    *names = n;
    return NPASS;
}

/* Set by the level, unless a flag already spoke for this pass. */
static void pass_default(int idx, int on)
{
    if (!g_pass[idx].forced)
        g_pass[idx].on = on;
}

#define g_mem2reg (g_pass[P_MEM2REG].on)
#define g_gcse    (g_pass[P_GCSE].on)
#define g_loadcse (g_pass[P_LOADCSE].on)
#define g_sccp    (g_pass[P_SCCP].on)
#define g_licm    (g_pass[P_LICM].on)
#define g_vec     (g_pass[P_VEC].on)
#define g_dse     (g_pass[P_DSE].on)
#define g_divmagic (g_pass[P_DIVMAGIC].on)
#define g_ifconv   (g_pass[P_IFCONV].on)
#define g_cfgclean (g_pass[P_CFGCLEAN].on)
#define g_tailrec  (g_pass[P_TAILREC].on)

/* ---- IR verifier (opt-in via EMBCC_VERIFY) --------------------------------
 * A cheap post-optimization sanity net for the two invariants a silent
 * miscompile breaks, and which the 99-test suite did NOT catch when var_scope
 * went stale: (1) every TEMP a surviving instruction reads still has a
 * definition — a pass that drops a live value trips this; (2) var_scope_lo/hi,
 * when present, index into the CURRENT instruction stream — a pass that
 * renumbers instructions without remapping (the DCE bug) trips this. Off by
 * default so normal builds pay nothing; the test suite runs with it set.
 * Aborts loudly (THE RULE) rather than let wrong code through. */
struct vrfy { struct defs *d; int np, nv; struct ir_func *fn; const char *tag; };
static void vrfy_read_cb(int *p, void *ctx)
{
    struct vrfy *v = ctx;
    int r = *p;
    if (r < 0 || r < v->nv)          /* param or local: a local may be read uninit'd */
        return;
    if (r < v->fn->nvregs && v->d->cnt[r] > 0)   /* a temp with a definition: fine */
        return;
    diag_fatal(v->fn->file, 0,
        "internal: %s reads temp %%%d with no definition (after %s) — an optimizer "
        "pass dropped a value that is still used", v->fn->name, r, v->tag);
}
static void verify_func(struct ir_func *fn, const char *tag)
{
    struct defs d;
    compute_defs(fn, &d);
    struct vrfy v = { &d, fn->nparams, fn->nvars, fn, tag };
    for (int n = 0; n < fn->nins; n++)
        each_read(&fn->ins[n], vrfy_read_cb, &v);
    if (fn->var_scope_lo)
        for (int i = 0; i < fn->nvars; i++) {
            int lo = fn->var_scope_lo[i], hi = fn->var_scope_hi[i];
            if (lo < 0 || lo > fn->nins || hi < lo || hi > fn->nins)
                diag_fatal(fn->file, 0,
                    "internal: %s local %d has out-of-range scope [%d,%d] for nins=%d "
                    "(after %s) — a pass renumbered instructions without remapping "
                    "var_scope", fn->name, i, lo, hi, fn->nins, tag);
        }

    /* R3 / §9.1: "Every instruction carries a debug location. The verifier
     * rejects instructions without one, except where explicitly marked
     * compiler-synthesized."
     *
     * This is what stops provenance rotting quietly. A pass that builds a
     * replacement instruction from scratch, instead of copying the one it
     * replaces, drops the location -- and nothing downstream complains,
     * because a line table with a hole still links. The verifier is the only
     * place that can notice, and it runs after every optimizing compile
     * under EMBCC_VERIFY (which the whole test suite sets).
     */
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        if (i->line || i->synth)
            continue;
        diag_fatal(fn->file, 0,
            "internal: %s instruction %d (%s) has no source location after %s "
            "— a pass built it without copying the location of what it "
            "replaced; if it corresponds to no source construct, mark it "
            "synth", fn->name, n, ir_opname(i->op), tag);
    }
    free_defs(&d);
}

static void opt_func(struct ir_func *fn)
{
    /* ---- functions the CFG cannot be trusted for -------------------
     *
     * Two shapes leave this analysis short of the truth:
     *
     *   Computed goto. An indirect jump can reach any address-taken
     *   label, and build_cfg does not model that, so dominance and
     *   liveness are both wrong.
     *
     *   An exception region. A landing pad is entered from EVERY call
     *   in its region -- edges nothing here records -- so the pad looks
     *   unreachable and a value live only on the exception path looks
     *   dead.
     *
     * Both used to return outright, leaving the whole function at -O0
     * however the build was invoked. For exceptions that is most of a
     * C++ program: across lib/libcxx, 316 of 609 functions got no
     * optimization at all, including every one of the loop passes.
     *
     * They do not need to. The passes that reason about CONTROL FLOW
     * are the ones that cannot be trusted here; the ones that work
     * within a block -- folding, value numbering, copy propagation,
     * store forwarding, dead code -- reason only between labels, and a
     * missing edge cannot make them wrong. A landing pad starts with a
     * label, so it is its own block to all of them. So the function is
     * optimized LOCALLY rather than not at all. */
    /* Computed goto still bails OUTRIGHT, and the difference from
     * exceptions is worth stating. A landing pad is a block with an
     * edge nothing recorded; the local passes never needed that edge,
     * so they are right without it. An indirect jump is worse: it can
     * land on any address-taken label, so a value live across it must
     * stay in memory, and tests/exec/computed-goto.c says so in its
     * first paragraph -- codegen keeps these functions in the memory
     * model for that reason. Running even the local passes here
     * returned the wrong answer at -O1 and -O2, and no single one of
     * them was responsible, so the exclusion stays until the CFG can
     * model the edges rather than until a pass is blamed. */
    for (int n = 0; n < fn->nins; n++)
        if (fn->ins[n].op == IR_IGOTO || fn->ins[n].op == IR_LABELADDR)
            return;
    int cfg_ok = !fn->neh;
    /* Likewise exception regions: a landing pad is entered from every call
     * of its region, edges the passes do not see (and its code, reached by
     * no jump, would look dead).
     *
     * Only while a region EXISTS, though. mark_eh_calls() clears neh when
     * no call in any region can actually throw -- true of every `noexcept`
     * function that calls nothing, and of type_info::name() in our own C++
     * runtime -- and then the pad really is ordinary unreachable code that
     * the passes may optimize or drop. What used to make that unsafe was
     * not the pad but the MODEL: IR_LANDING writes two temps and
     * compute_defs only saw one, so DCE deleted the landing and kept the
     * stores reading what it produced. compute_defs knows both now, so
     * these functions are optimized again instead of being compiled at
     * -O0 however the build was invoked. */
    int verify = getenv("EMBCC_VERIFY") != NULL;
    if (verify) verify_func(fn, "irgen");
    int ins_before = fn->nins;
    memset(&g_did, 0, sizeof g_did);
    /* Before the fixpoint: what it emits -- a multiply, some shifts and
     * an add -- is ordinary arithmetic the other passes then fold,
     * value-number and strength-reduce like any other. */
    /* Before everything: the loop it makes is then an ordinary loop to
     * mem2reg, LICM, rotation and strength reduction, which is most of
     * why it is worth doing at all. */
    if (g_tailrec && cfg_ok)
        pass_tailrec(fn);
    if (g_divmagic)
        pass_divmagic(fn);
    if (g_mem2reg && cfg_ok)
        pass_mem2reg(fn);         /* global mem2reg (subsumes store-forwarding) */
    /* Global load CSE is the expensive pass (CFG + an available-expressions
     * dataflow), so it runs ONCE per outer round instead of on every inner
     * iteration. When it exposes copies, the inner fixpoint reconverges and we
     * round again — it settles in one or two rounds. */
    int outer = 1, oguard = 0;
    while (outer && oguard++ < 100) {
        outer = 0;
        int changed = 1, guard = 0;
        while (changed && guard++ < 1000) {
            changed = 0;
            changed |= pass_storefwd(fn); /* forward local stores to loads (mem2reg-lite) */
            changed |= pass_fold(fn);
            changed |= pass_lvn(fn);      /* CSE: reuse identical computations */
            if (g_gcse && cfg_ok)
                changed |= pass_gcse(fn); /* CSE across the dominator tree */
            /* SCCP is the one that must stay off without a trustworthy
             * CFG even though it builds its own: it DELETES blocks it
             * finds unreachable, and a landing pad is exactly that. */
            if (g_sccp && cfg_ok)
                changed |= pass_sccp(fn); /* resolve const branches, drop dead blocks */
            changed |= pass_copyprop(fn);
            changed |= pass_copyprop_local(fn);  /* the phi copies the global
                                                  * one cannot touch */
            changed |= pass_dce(fn);
            if (g_cfgclean)
                changed |= pass_cfgclean(fn);
            if (g_dse && cfg_ok)
                changed |= pass_dse(fn);
        }
        if (g_loadcse && cfg_ok && pass_loadcse(fn)) {   /* reuse loads redundant on every path */
            pass_copyprop(fn);
            pass_dce(fn);
            outer = 1;
        }
        /* Rotation first: it merges the header into the body, so the
         * block-local passes in the next round see one block where they
         * saw two. */
        if (g_licm && cfg_ok && pass_rotate(fn)) {
            pass_copyprop_local(fn);
            pass_dce(fn);
            outer = 1;
        }
        /* LICM belongs out here with the other CFG passes: it rebuilds the
         * instruction array, so every block boundary the inner fixpoint
         * might hold is gone. Rounding again matters -- a hoisted
         * expression is a new candidate for folding and CSE in the
         * preheader, and what those leave can expose the next hoist. */
        if (g_licm && cfg_ok && pass_licm(fn)) {
            pass_copyprop(fn);
            pass_dce(fn);
            outer = 1;
        }
        /* Vectorizing LAST of the loop passes, because it depends on
         * all of them: rotation for the single-block bottom-tested
         * shape, and LICM for the address base -- until the `gaddr` is
         * hoisted out, every memory reference looks like it is indexed
         * off something that changes, and nothing vectorizes. */
        if (g_vec && cfg_ok && pass_vectorize(fn)) {
            pass_dce(fn);
            outer = 1;
        }
    }
    /* Strength reduction runs ONCE, after the loop passes have settled,
     * because it destroys the addressing shape the vectorizer matches
     * on: a loop already walking a pointer no longer looks like
     * base + i*scale. Inside the round it would race the vectorizer for
     * the same loops and win, trading four lanes for one add. */
    /* If-conversion after the loop passes: the shape it matches is what
     * phi destruction leaves, and rotation/LICM must have finished
     * moving blocks around before the diamond is the real one. */
    if (g_ifconv && cfg_ok && pass_ifconv(fn)) {
        pass_copyprop_local(fn);
        pass_dce(fn);
    }
    if (g_licm && cfg_ok && pass_ivsr(fn)) {
        pass_copyprop_local(fn);
        pass_dce(fn);
    }
    /* After the fixpoint: fold constant operands into immediates, then DCE the
     * CONSTs that leaves unreferenced. Kept out of the fixpoint so the earlier
     * passes never reason about the imm_b form. */
    if (pass_immfold(fn))
        pass_dce(fn);
    if (verify) verify_func(fn, "opt");

    /* What the whole fixpoint came to, for this function. The per-pass
     * decisions above answer "why"; this answers "did anything happen", and
     * it is the number a person compares between two builds. */
    if (remarks_on() && fn->src) {
        remark_add("opt", "optimized", fn->name, "fixpoint-reached",
                   fn->file, fn->line,
                   "%d instructions -> %d", ins_before, fn->nins);
        /* The passes that had been silent. One line, only when they did
         * something, because most functions give every count as zero. */
        if (g_did.lvn || g_did.gcse || g_did.dce || g_did.copy ||
            g_did.loadcse || g_did.dse || g_did.ifconv)
            remark_add("opt", "rewrote", fn->name, "pass-counts",
                       fn->file, fn->line,
                       "%ld cse, %ld global cse, %ld load reuse, "
                       "%ld copies propagated, %ld dead, %ld dead stores, "
                       "%ld selects",
                       g_did.lvn, g_did.gcse, g_did.loadcse, g_did.copy,
                       g_did.dce, g_did.dse, g_did.ifconv);
    }
}

/* -Os is level 2 without vectorization, which is the one pass here that
 * reliably adds code: a vector loop beside the scalar one, plus the
 * remainder test.
 *
 * Inlining is NOT disabled, which is worth writing down because the
 * obvious guess was wrong. Turning it off made the benchmark's object
 * file grow from 3959 bytes to 7340 -- inlining a static function that
 * has one caller lets the original be deleted, so refusing to inline
 * keeps two copies of it. Until there is a cost model that can tell
 * that case from a body copied into twenty call sites (section 4's
 * work), inlining everything it already inlines is the smaller answer.
 *
 * Everything else -- folding, value numbering, dead code, loop
 * invariants, strength reduction -- makes code smaller as well as
 * faster, which is why -Os is level 2 and not level 1. */
void opt_run(struct ir_unit *iu, int level)
{
    int size = level == OPT_SIZE;
    if (size)
        level = 2;
    if (level < 1)
        return;
    pass_default(P_MEM2REG, level >= 2);
    pass_default(P_GCSE,    level >= 2);
    pass_default(P_LICM,    level >= 2);
    /* Vectorization is x86-64 for now: the aarch64 backend refuses the
     * vector opcodes loudly (diag_fatal) rather than emitting something
     * it has not been taught, so the pass must not hand it any. */
    pass_default(P_VEC,     level >= 2 && !size &&
                            target_get() == TARGET_X86_64);
    pass_default(P_LOADCSE, level >= 2);
    pass_default(P_SCCP,    level >= 2);
    pass_default(P_INLINE,  level >= 2);
    pass_default(P_DSE,     level >= 2);
    /* A multiply and two shifts in place of a divide is smaller than
     * the divide's setup on these targets as well as faster, so -Os
     * keeps it. */
    pass_default(P_DIVMAGIC, level >= 2);
    pass_default(P_IFCONV, level >= 2);   /* cmov on x86-64, csel on aarch64 */
    pass_default(P_CFGCLEAN, level >= 1);  /* smaller and simpler at any level */
    pass_default(P_TAILREC, level >= 2);
    if (g_pass[P_INLINE].on)      /* inline before the per-function passes clean up */
        inline_unit(iu);
    /* Every function, including one computing with __int128. The folds
     * that are 64-bit refuse a 128-bit width individually now (see
     * pass_fold), which is a great deal narrower than refusing the
     * function: everything else -- value numbering, copy propagation,
     * dead code, and all of the loop passes -- works on it unchanged. */
    for (int f = 0; f < iu->nfuncs; f++)
        opt_func(&iu->funcs[f]);
}
