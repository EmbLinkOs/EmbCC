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
#include "../arch/regalloc.h"

/* The width of a POINTER as an IR operation's `w`. Strength reduction
 * rewrites an indexed access into a walking pointer and increments it
 * every iteration; those increments are pointer arithmetic, and saying 8
 * made them 64-bit operations on a machine with 32-bit registers. The
 * other 8s in this file are 16-byte vector lanes and mean something
 * else. */
#define PTRW (target_ptr_size())
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
                ifconv, cfgclean, tailrec, idiom, sroa, pre, latch; } g_did;

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
    case IR_I2F: case IR_F2I: case IR_F2F: case IR_BITCAST:
    case IR_CALL: case IR_XCHG:
    case IR_XADD: case IR_CMPXCHG: case IR_ARMW: case IR_CAS: case IR_CAS16:
    case IR_FRAMEADDR: case IR_ALLOCA: case IR_SPSAVE:
    case IR_SELECT:
    /* `dst = &&label` defines dst. It was missing here for as long as
     * the optimizer skipped every function that used one, which meant
     * nothing counted it as a definition -- so the verifier reported
     * the jump that read it as reading a temp nothing writes, and was
     * right. */
    case IR_LABELADDR:
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
    case IR_I2F: case IR_F2I: case IR_F2F: case IR_BITCAST:
    case IR_SELECT:        /* both arms are values; it cannot trap */
    case IR_LABELADDR:     /* the address of a label is a constant */
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
    /* An asm's value output (ir_asm_op.val), as a call's result. Not in
     * writes_temp: two asms are never the same computation, however
     * alike they look, so nothing may value-number one. */
    if (i->op == IR_ASM)
        return i->dst;
    return -1;
}

/* Visit &slot for every LABEL an instruction names: a branch's target, a
 * label address, a switch's default and each entry of its table. The one
 * place that knows where they all are, so a pass that retargets or counts
 * them cannot miss the table -- which lives in the function, not the
 * instruction, and is why this takes fn. */
static void each_label(struct ir_func *fn, struct ir_ins *i,
                       void (*cb)(int *, void *), void *ctx)
{
    switch (i->op) {
    case IR_JMP: case IR_BRZ: case IR_BRNZ: case IR_LABELADDR:
        cb(&i->label, ctx);
        break;
    case IR_SWITCH:
        cb(&i->label, ctx);
        for (int k = 0; k < fn->jt[i->jt].n; k++)
            cb(&fn->jt[i->jt].labels[k], ctx);
        break;
    default:
        break;
    }
}
struct retarget { int from, to; };
static void retarget_cb(int *p, void *ctx)
{
    struct retarget *r = ctx;
    if (*p == r->from) *p = r->to;
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
    case IR_I2F: case IR_F2I: case IR_F2F: case IR_BITCAST:
    case IR_EXT: case IR_BSWAP: case IR_SQRT:
    case IR_LDVAR: case IR_ADDR: case IR_LOAD:
    case IR_MEMZERO: case IR_VA_START: case IR_STVAR:
    case IR_ALLOCA: case IR_SPRESTORE:
    /* `goto *p` reads p. It was missing here, and could not be noticed
     * while every function using one was skipped by the optimizer
     * whole: dead-code elimination saw no reader, dropped the load of
     * the label address, and left the jump reading a temp nothing
     * writes. Every visitor built on each_read -- the use counts, the
     * inliner's remap, the verifier -- was blind to it the same way. */
    case IR_IGOTO:
    case IR_SWITCH:               /* the table index */
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
        /* an output's lvalue ADDRESS is read; a `val` one is the dst */
        for (int k = 0; k < i->asm_ir->nout; k++)
            if (!i->asm_ir->out[k].val)
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
    /* Every definition of each vreg, when defs_lists has built them:
     * first[v] is the first instruction defining v and next[n] the one
     * after instruction n, -1 at the end. NULL until asked for. */
    int *first, *next;
};

static void compute_defs(struct ir_func *fn, struct defs *d)
{
    d->cnt = xcalloc((size_t)fn->nvregs, sizeof *d->cnt);
    d->ins = xmalloc((size_t)fn->nvregs * sizeof *d->ins);
    d->first = d->next = NULL;
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
    free(d->first);
    free(d->next);
}

/* The definition lists (struct defs' first/next), by def_target. A
 * landing pad's two destinations are not among them, which is why a
 * reader counts what it walks against cnt. */
static void defs_lists(struct ir_func *fn, struct defs *d)
{
    if (d->first)
        return;
    d->first = xmalloc((size_t)(fn->nvregs ? fn->nvregs : 1) * sizeof *d->first);
    d->next = xmalloc((size_t)(fn->nins ? fn->nins : 1) * sizeof *d->next);
    for (int v = 0; v < fn->nvregs; v++)
        d->first[v] = -1;
    for (int n = fn->nins - 1; n >= 0; n--) {
        int t = def_target(&fn->ins[n]);
        d->next[n] = -1;
        if (t >= 0 && t < fn->nvregs && fn->ins[n].op != IR_LANDING) {
            d->next[n] = d->first[t];
            d->first[t] = n;
        }
    }
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

/* Combine `(x INNER c1) OUTER c2` into one `x OP c` -- the arithmetic of
 * reassociation, with no IR in it so the cases can be read at once.
 * Everything is computed unsigned and normalised to the width, because
 * a signed overflow here is undefined and the machine's answer is the
 * one the program will see either way. */
static int reassoc_fold(enum ir_op outer, enum ir_op inner, long c1, long c2,
                        int w, enum ir_op *op_out, long *c_out);

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

/* A conversion of a constant, done now: `(double)1000000` was a call to
 * __floatsidf at run time on every soft-float target, and `float s = 0`
 * a vcvt on the FPU ones. `size` is the source's width and `w` the
 * result's, as the backends read them; a float constant is its bit
 * pattern in an IR_CONST, as irgen's emit_fconst makes it.
 *
 * The host computes it, with its IEEE round-to-nearest -- what every
 * target does by default. Declined where the answer is not that simple:
 * a float that is not finite, or out of the integer type's range (C
 * leaves that undefined and targets really do differ -- ARM saturates,
 * x86 gives the "integer indefinite"), a NaN through a float/double
 * conversion (a soft-float runtime need not keep its payload as the host
 * does), and anything 16 bytes wide. */
static double fc_bits_to(long bits, int size)
{
    if (size == 8) {
        double d;
        memcpy(&d, &bits, 8);
        return d;
    }
    {
        unsigned int u = (unsigned int)bits;
        float f;
        memcpy(&f, &u, 4);
        return (double)f;
    }
}
static long fc_to_bits(double d, int w)
{
    long bits = 0;
    if (w == 8) {
        memcpy(&bits, &d, 8);
    } else {
        float f = (float)d;
        unsigned int u;
        memcpy(&u, &f, 4);
        bits = (long)u;
    }
    return bits;
}
/* A float or double operation on constants, as the machine does it: in
 * the operation's own format, one rounding, the default rounding mode
 * (EmbCC does not honour FENV_ACCESS, and keeps no exception flags).
 * Constants arrive as bit patterns, the form irgen gives them.
 *
 * Not when an operand or the result is a NaN: WHICH NaN an invalid
 * operation produces is the machine's choice (x86's default NaN is
 * negative, ARM's positive) and so is how a payload propagates, so the
 * folded bits could differ from the ones the program would compute.
 * Long double (width 16) is never here. A comparison folds to 0 or 1;
 * with NaN excluded every predicate is the plain ordered one. Integer
 * division is not folded (a divide by zero must happen at run time);
 * a floating one by zero is infinity, which is what the machine gives.
 *
 * Every float constant expression used to be computed at run time:
 * `(struct color){ 251/255.0f, ... }` was a divss per component, and
 * EmbLinkOs's ui/theme/theme.c was five times gcc's size for it. */
static int fold_fp(const struct ir_ins *i, long A, long B, long *out)
{
    if (i->w == 4) {
        unsigned int ua = (unsigned int)A, ub = (unsigned int)B, ur;
        float a, b, r;
        memcpy(&a, &ua, 4);
        memcpy(&b, &ub, 4);
        if (a != a || (i->op != IR_NEG && b != b))
            return 0;
        switch (i->op) {
        case IR_ADD: r = a + b; break;
        case IR_SUB: r = a - b; break;
        case IR_MUL: r = a * b; break;
        case IR_DIV: r = a / b; break;
        case IR_NEG: r = -a; break;
        case IR_CMP:
            switch (i->pred) {
            case B_EQ: *out = a == b; return 1;
            case B_NE: *out = a != b; return 1;
            case B_LT: *out = a < b;  return 1;
            case B_LE: *out = a <= b; return 1;
            case B_GT: *out = a > b;  return 1;
            case B_GE: *out = a >= b; return 1;
            default: return 0;
            }
        default: return 0;
        }
        if (r != r)
            return 0;
        memcpy(&ur, &r, 4);
        *out = (long)ur;
        return 1;
    }
    if (i->w == 8) {
        double a, b, r;
        memcpy(&a, &A, 8);
        memcpy(&b, &B, 8);
        if (a != a || (i->op != IR_NEG && b != b))
            return 0;
        switch (i->op) {
        case IR_ADD: r = a + b; break;
        case IR_SUB: r = a - b; break;
        case IR_MUL: r = a * b; break;
        case IR_DIV: r = a / b; break;
        case IR_NEG: r = -a; break;
        case IR_CMP:
            switch (i->pred) {
            case B_EQ: *out = a == b; return 1;
            case B_NE: *out = a != b; return 1;
            case B_LT: *out = a < b;  return 1;
            case B_LE: *out = a <= b; return 1;
            case B_GT: *out = a > b;  return 1;
            case B_GE: *out = a >= b; return 1;
            default: return 0;
            }
        default: return 0;
        }
        if (r != r)
            return 0;
        memcpy(out, &r, 8);
        return 1;
    }
    return 0;
}

static int fold_cvt(const struct ir_ins *i, long A, long *out)
{
    if (i->w == 16 || i->size == 16 || i->w <= 0 || i->size <= 0)
        return 0;
    if (i->op == IR_I2F) {
        long v;
        /* Where an unsigned 32-bit source is widened to a 64-bit signed
         * conversion (target_widen_unsigned_fp_cvt), irgen relies on the
         * MACHINE having zero-extended it -- there is no extension in the
         * IR to fold, and the constant's own normalisation need not match.
         * Only a value that reads the same either way is folded. */
        if (i->size == 8 && target_widen_unsigned_fp_cvt() &&
            (A < 0 || A > 0x7fffffffL))
            return 0;
        v = fold_ext(A, i->size, i->sign, 8);
        double d;
        float f;
        if (i->w != 4 && i->w != 8)
            return 0;
        if (i->w == 4) {
            /* ONE rounding, straight to float: through double first
             * would round twice for a 64-bit value */
            f = i->sign ? (float)v : (float)(unsigned long)v;
            *out = fc_to_bits((double)f, 4);
            return 1;
        }
        d = i->sign ? (double)v : (double)(unsigned long)v;
        *out = fc_to_bits(d, 8);
        return 1;
    }
    if (i->size != 4 && i->size != 8)
        return 0;
    {
        double d = fc_bits_to(A, i->size);
        if (d != d)                            /* NaN */
            return 0;
        if (i->op == IR_F2F) {
            if (i->w != 4 && i->w != 8)
                return 0;
            if (i->w == 4 && i->size == 8) {
                /* the rounding of double to float, which may overflow
                 * to infinity -- as the conversion does at run time */
                float f = (float)d;
                *out = fc_to_bits((double)f, 4);
            } else {
                *out = fc_to_bits(d, i->w);
            }
            return 1;
        }
        /* IR_F2I: truncate toward zero, only when the result fits. C's
         * own conversion truncates, so the host does it -- after the
         * range check, which is what makes the conversion defined. (No
         * math builtins here: EmbCC compiles this file too.) */
        {
            int bits = i->w * 8;
            double two63 = (double)(1ULL << 63);
            if (bits > 64 || bits <= 0)
                return 0;
            if (i->sign) {
                double hi = bits == 64 ? two63 : (double)(1ULL << (bits - 1));
                if (!(d > -hi - 1.0 && d < hi))
                    return 0;
                *out = norm((long)(long long)d, i->w);
            } else {
                double hi = bits == 64 ? two63 * 2.0 : (double)(1ULL << bits);
                if (!(d > -1.0 && d < hi))
                    return 0;
                *out = norm((long)(unsigned long long)d, i->w);
            }
            return 1;
        }
    }
}

/* The bits of vreg v that are zero in EVERY value it can take, at width
 * w: a shift leaves the bits it shifted over, a multiply by a multiple
 * of 2^k the low k, a mask the bits it clears, a zero-extension
 * everything above the source, and an or/xor only what both sides
 * leave. Bits above the width are reported zero, which is what the
 * caller's own width mask discards. Conservative: a bit not proven
 * zero is not claimed, and the walk stops a few definitions up. */
static unsigned long known_zero_ins(struct ir_func *fn, struct defs *d,
                                    const struct ir_ins *i, int w, int depth);

static unsigned long known_zero(struct ir_func *fn, struct defs *d, int v,
                                int w, int depth)
{
    unsigned long wm = w == 8 ? ~0UL : 0xffffffffUL, hi = ~wm;
    if (v < 0 || v >= fn->nvregs || depth > 4)
        return hi;
    /* A temp with several definitions -- a join's, after phi
     * destruction: `b = (u8)x` on one path and `b = 0x7e` on the other
     * -- has the zeros all of them have. Only with the lists built, only
     * a temp (a variable has its parameter binding too), and only when
     * the lists hold every definition (not a landing pad's). */
    if (d->cnt[v] > 1) {
        if (!d->first || v < fn->nvars || depth > 2)
            return hi;
        unsigned long kz = ~0UL;
        int seen = 0;
        for (int n = d->first[v]; n >= 0; n = d->next[n], seen++)
            kz &= known_zero_ins(fn, d, &fn->ins[n], w, depth + 1);
        return seen == d->cnt[v] ? kz | hi : hi;
    }
    if (d->cnt[v] != 1 || d->ins[v] < 0)
        return hi;
    return known_zero_ins(fn, d, &fn->ins[d->ins[v]], w, depth);
}

/* known_zero of what one instruction writes. */
static unsigned long known_zero_ins(struct ir_func *fn, struct defs *d,
                                    const struct ir_ins *i, int w, int depth)
{
    unsigned long wm = w == 8 ? ~0UL : 0xffffffffUL, hi = ~wm;
    if (i->flt || i->w == 16 || (i->w != w && i->op != IR_EXT))
        return hi;
    long B = 0;
    int kb = i->imm_b ? (B = i->imm, 1) : get_const(fn, d, i->b, &B);
    unsigned long ub = (unsigned long)B & wm;
    switch (i->op) {
    case IR_CONST:
        return hi | (~(unsigned long)i->imm & wm);
    case IR_MOV:
        return hi | known_zero(fn, d, i->a, w, depth + 1);
    case IR_SHL:
        if (!kb || B < 0 || B >= 8 * w)
            return hi;
        return hi | ((known_zero(fn, d, i->a, w, depth + 1) << B) & wm) |
               ((1UL << B) - 1);
    case IR_SHR:
        if (!kb || B < 0 || B >= 8 * w || i->sign)
            return hi;                   /* an arithmetic shift copies the sign */
        return hi | ((known_zero(fn, d, i->a, w, depth + 1) & wm) >> B) |
               (~(wm >> B) & wm);
    case IR_AND:
        return hi | known_zero(fn, d, i->a, w, depth + 1) |
               (kb ? (~ub & wm) : known_zero(fn, d, i->b, w, depth + 1));
    case IR_OR: case IR_XOR:
        return hi | (known_zero(fn, d, i->a, w, depth + 1) &
                     (kb ? (~ub & wm) : known_zero(fn, d, i->b, w, depth + 1)));
    case IR_MUL: {
        if (!kb || ub == 0)
            return hi;
        int tz = 0;
        while (!(ub & 1)) { ub >>= 1; tz++; }
        return hi | ((1UL << tz) - 1);   /* a multiple of 2^tz ends in tz zeros */
    }
    case IR_EXT:
        if (i->sign || i->size <= 0 || i->size >= w)
            return hi;
        return hi | (~((1UL << (8 * i->size)) - 1) & wm);
    case IR_LOAD:               /* `load.4:1` zero-extends the byte it reads */
        if (i->sign || i->size <= 0 || i->size >= w)
            return hi;
        return hi | (~((1UL << (8 * i->size)) - 1) & wm);
    default:
        return hi;
    }
}

/* Is vreg v defined earlier in the same basic block as instruction n --
 * no label between its definition and n? */
static int defined_in_block(struct ir_func *fn, struct defs *d, int v, int n)
{
    if (v < 0 || d->cnt[v] != 1 || d->ins[v] < 0 || d->ins[v] >= n)
        return 0;
    for (int m = d->ins[v] + 1; m < n; m++)
        if (fn->ins[m].op == IR_LABEL)
            return 0;
    return 1;
}

/* ---- block-local constants ----
 *
 * get_const knows a temp only when it has ONE definition, and the temps a
 * loop carries have one per incoming edge: phi destruction assigns an
 * induction variable before the loop and again at the latch. Inside one
 * block that does not matter. After `i = const 0`, and until something
 * else writes i, i is 0. That is exactly where a rotated loop's guard
 * sits -- `i = 0; if (!(i < 24)) skip the loop` -- and the guard stayed
 * a compare and a branch, run on every entry to every counted loop.
 *
 * lk_gen[v] == gen says v is known to hold lk_val[v] at width lk_w[v].
 * A new generation starts at every label and after every control
 * transfer, so nothing is believed across a block boundary; a definition
 * of v by anything but a constant, or a copy of a known value, forgets
 * it; and an instruction that writes temps def_target does not report
 * (inline asm, a landing pad) starts a new generation too. */
struct lkconst { int *gen_of; long *val; int *w; int gen, nv; };

static void lk_note(struct lkconst *k, const struct ir_ins *i)
{
    switch (i->op) {
    case IR_LABEL: case IR_JMP: case IR_BRZ: case IR_BRNZ: case IR_SWITCH:
    case IR_RET: case IR_IGOTO: case IR_UD2: case IR_ASM: case IR_LANDING:
        k->gen++;
        return;
    default:
        break;
    }
    int t = def_target(i);
    if (t < 0 || t >= k->nv)
        return;
    if (i->op == IR_CONST && !i->flt && (i->w == 4 || i->w == 8)) {
        k->gen_of[t] = k->gen; k->val[t] = i->imm; k->w[t] = i->w;
    } else if (i->op == IR_MOV && !i->vol && !i->flt && i->a >= 0 &&
               i->a < k->nv && i->a != t && k->gen_of[i->a] == k->gen &&
               k->w[i->a] == i->w) {
        k->gen_of[t] = k->gen; k->val[t] = k->val[i->a]; k->w[t] = i->w;
    } else {
        k->gen_of[t] = 0;
    }
}

/* Is v known here, at width w? */
static int lk_get(const struct lkconst *k, int v, int w, long *out)
{
    if (v < 0 || v >= k->nv || k->gen_of[v] != k->gen || k->w[v] != w)
        return 0;
    *out = k->val[v];
    return 1;
}

/* May `ext.8 x` become `mov.8 x` when x is known zero above the
 * extension's size? Where a register is 64 bits, yes: a 32-bit value sits
 * in it extended, as every 64-bit backend keeps it. Where an eight-byte
 * value is a register PAIR -- every target with pointers narrower than 8
 * -- only if x itself is eight bytes wide wherever it is defined: a
 * four-byte x has no high word, and the copy read whatever the pair's
 * other register held. That was fuzz seed 927: `l0 = (u64)a0` became an
 * eight-byte copy of a four-byte byte-extension, wrong on SPARC, RV32,
 * Cortex-M and MIPS at -O1 and up, right on x86-64 and AArch64. */
static int kz_copy_wide_ok(struct ir_func *fn, struct defs *d,
                           const struct ir_ins *i)
{
    if (i->w != 8 || target_ptr_size() >= 8)
        return 1;
    if (i->a < fn->nparams)
        return 0;                       /* its width is the ABI's question */
    int any = 0;
    for (int n = d->first[i->a]; n >= 0; n = d->next[n]) {
        const struct ir_ins *in = &fn->ins[n];
        if (in->op == IR_STVAR ? in->size != 8 : in->w != 8)
            return 0;
        any = 1;
    }
    return any;
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
    struct lkconst lk = {
        xcalloc((size_t)fn->nvregs, sizeof(int)),
        xmalloc((size_t)fn->nvregs * sizeof(long)),
        xmalloc((size_t)fn->nvregs * sizeof(int)), 1, fn->nvregs
    };
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        /* What the previous instruction left, in its final form: one
         * this loop has just folded to a constant is a constant. */
        if (n > 0)
            lk_note(&lk, &fn->ins[n - 1]);
        if (i->op == IR_LABEL)
            lk.gen++;
        if (i->flt) {
            /* never as an integer -- as a float, from bit patterns */
            long A, B = 0, r;
            if ((i->op == IR_ADD || i->op == IR_SUB || i->op == IR_MUL ||
                 i->op == IR_DIV || i->op == IR_NEG || i->op == IR_CMP) &&
                get_const(fn, &d, i->a, &A) &&
                (i->op == IR_NEG || get_const(fn, &d, i->b, &B)) &&
                fold_fp(i, A, B, &r)) {
                to_const(i, r);
                i->flt = 0;             /* a bit pattern, as irgen's are */
                changed = 1;
            }
            continue;
        }
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
        if (i->op == IR_MOV && ka && !i->flt && (i->w == 4 || i->w == 8) &&
            target_get() != TARGET_AVR && !defined_in_block(fn, &d, i->a, n)) {
            /* A copy of a constant IS that constant. The copies are what
             * phi destruction leaves at a loop's entry and latch, and a
             * constant they all read stayed live across the whole loop
             * for their sake -- in a callee-saved register, or a slot:
             * `bounds` in tests/bench kept its zero on the stack and
             * reloaded it every outer iteration. As a constant of its own
             * each copy is one instruction and no live range. Only across
             * a block boundary, though: within one block value numbering
             * merges equal constants into exactly this copy, and the two
             * rules undid each other forever -- src/arch/x86_64/codegen.c
             * never finished compiling. Not on AVR:
             * a constant there is an ldi per byte (four `mov r,r1` for a
             * zero) where the copy was a movw per pair, and lib/libc's
             * math grew by 256 bytes. */
            to_const(i, norm(A, i->w));
            changed = 1;
            continue;
        }
        if ((i->op == IR_I2F || i->op == IR_F2I || i->op == IR_F2F) && ka) {
            long r;
            if (fold_cvt(i, A, &r)) {
                to_const(i, r);
                changed = 1;
            }
            continue;
        }
        if (i->op == IR_EXT) {
            if (ka) {
                to_const(i, fold_ext(A, i->size, i->sign, i->w));
                changed = 1;
            } else if (i->size >= i->w) {
                /* An extension to a width the value already fills does
                 * nothing: `ext.4:4s` asks for the low four bytes
                 * sign-extended to four bytes, which is what it was
                 * handed. irgen emits these for every `int` operand of
                 * an `int` operation, so they are not rare -- four in a
                 * two-parameter comparison -- and each one costs an
                 * instruction on aarch64 (`asr w, w, #0`) that the x86
                 * backend was already folding into its addressing. */
                to_mov(i, i->a);
                changed = 1;
            } else if (i->a >= 0 && i->a < fn->nvregs && d.cnt[i->a] == 1 &&
                       d.ins[i->a] >= 0 &&
                       fn->ins[d.ins[i->a]].op == IR_EXT &&
                       !fn->ins[d.ins[i->a]].flt &&
                       fn->ins[d.ins[i->a]].w >= i->size) {
                /* An extension of an extension, which promoting a narrow
                 * local makes of every read after a store: `x = ext.4:2
                 * (ext.4:2 y)`. Wider than the inner width, or the same
                 * width and kind, the outer changes nothing -- the bits
                 * it would write already hold that extension -- so it is
                 * a copy. At the same width with the other kind, or
                 * narrower, it only needs the inner's SOURCE, provided
                 * that has one definition and so cannot have changed. */
                const struct ir_ins *in = &fn->ins[d.ins[i->a]];
                int src_ok = in->a >= 0 && in->a < fn->nvregs &&
                             d.cnt[in->a] == 1 && i->a != in->a;
                /* Wider than the inner, the outer changes nothing when the
                 * inner ZERO-extended (the bits above are 0, which either
                 * kind of extension keeps) or both sign-extend -- but a
                 * zero-extension of a sign-extended value clears what the
                 * inner set: (unsigned short)(signed char)-1 is 65535, and
                 * calling that a copy returned -1 at -O2. */
                int keeps = (i->size > in->size && (i->sign || !in->sign)) ||
                            (i->size == in->size && i->sign == in->sign);
                if (keeps && i->w == in->w) {
                    to_mov(i, i->a);
                    changed = 1;
                } else if (keeps && src_ok) {
                    /* WIDER than the inner result (`ext.8:4` of an
                     * `ext.4:2`): not a copy -- the high bytes are the
                     * outer's to make -- but the inner's extension taken
                     * straight to the outer width. As a copy it handed a
                     * four-byte value to an eight-byte add. */
                    i->a = in->a;
                    i->size = in->size;
                    i->sign = in->sign;
                    changed = 1;
                } else if (!keeps && src_ok && i->size <= in->size) {
                    /* Narrower, or the same width and the other kind: the
                     * outer reads only bytes the inner left as they were,
                     * so it may read them from the inner's source. Wider,
                     * it would read bytes the inner made. */
                    i->a = in->a;
                    changed = 1;
                }
            } else if (i->a >= 0 && i->a < fn->nvregs && d.cnt[i->a] == 1 &&
                       d.ins[i->a] >= 0 &&
                       fn->ins[d.ins[i->a]].op == IR_LOAD &&
                       !fn->ins[d.ins[i->a]].flt &&
                       fn->ins[d.ins[i->a]].w == i->w) {
                /* An extension of a narrow LOAD, which already extended
                 * what it read -- `load.4:1` is a byte zero-extended to
                 * four. The same rule as above decides whether the outer
                 * one is a copy; 31 byte loads in the Thumb corpus were
                 * followed by an `ext.4:1` of their own result. */
                const struct ir_ins *in = &fn->ins[d.ins[i->a]];
                if ((i->size > in->size && (i->sign || !in->sign)) ||
                    (i->size == in->size && i->sign == in->sign)) {
                    to_mov(i, i->a);
                    changed = 1;
                }
            } else if (i->a >= 0 && i->a < fn->nvregs && i->size > 0 &&
                       (i->w == 4 || i->w == 8) &&
                       !getenv("EMBCC_NO_KZEXT")) {
                /* An extension of a value whose high bits are already
                 * zero (known_zero) -- `(u8)(x >> 24)`, `(u8)(x & 0x7f)`,
                 * and a join of such values, `b = c ? (u8)x : 0x7e` --
                 * is a copy: zero-extending changes nothing above the
                 * size, and sign-extending copies a sign bit that is 0.
                 * The state machine of tools/bench extended its input
                 * byte once where it was made and again in every case
                 * that read it. */
                defs_lists(fn, &d);
                unsigned long wm = i->w == 8 ? ~0UL : 0xffffffffUL;
                unsigned long keep =
                    (1UL << (8 * i->size - (i->sign ? 1 : 0))) - 1;
                if ((wm & ~keep & ~known_zero(fn, &d, i->a, i->w, 0)) == 0 &&
                    kz_copy_wide_ok(fn, &d, i)) {
                    to_mov(i, i->a);
                    changed = 1;
                }
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
        /* A loop-carried temp read where the block has just assigned it
         * a constant: see lk_note. Only at the width the constant was
         * written at. A compare at first (a rotated loop's guard); then
         * the arithmetic too, because a FULLY unrolled loop is this shape
         * from end to end -- `b = 0`, then `b*8`, `(b+1)*8`, ... in one
         * block, with the latch's dead `b = b + 4` keeping b two
         * definitions -- and FNV's byte loop shifted by a register four
         * times where clang has uxtb, ubfx and lsr #24. Not a division:
         * see fold_bin. */
        if ((i->op == IR_CMP || i->op == IR_ADD || i->op == IR_SUB ||
             i->op == IR_MUL || i->op == IR_AND || i->op == IR_OR ||
             i->op == IR_XOR || i->op == IR_SHL || i->op == IR_SHR) &&
            !i->imm_b && (i->w == 4 || i->w == 8) &&
            !(ka && kb) && !getenv("EMBCC_NO_LKCONST")) {
            long la, lb;
            int ja = ka || lk_get(&lk, i->a, i->w, &la);
            int jb = kb || lk_get(&lk, i->b, i->w, &lb);
            if (ja && jb) {
                if (!ka) A = la;
                if (!kb) B = lb;
                if (fold_bin(i->op, A, B, i->w, i->sign, i->pred, &r)) {
                    to_const(i, r);
                    changed = 1;
                    continue;
                }
            }
        }
        if (ka && kb) {
            if (fold_bin(i->op, A, B, i->w, i->sign, i->pred, &r)) {
                to_const(i, r);
                changed = 1;
            }
            continue;
        }
        /* `(i * 2) & 1` is zero whatever i is: an AND whose constant
         * reads only bits the other operand can never set (known_zero)
         * is a constant zero, and the branch on it, and the arm behind
         * the branch, go with it -- tests/bench's dead_branch spent
         * eleven instructions an iteration on x86-64 where five do. Only
         * the all-zero answer is taken; trimming a mask would change
         * nothing that runs. */
        if (i->op == IR_AND && kb && !ka && i->a >= 0) {
            unsigned long wm = i->w == 8 ? ~0UL : 0xffffffffUL;
            if (((unsigned long)B & wm & ~known_zero(fn, &d, i->a, i->w, 0)) == 0) {
                to_const(i, 0);
                changed = 1;
                continue;
            }
        }
        /* A COMPARISON OF A COMPARISON. `!!x` is `(x == 0) == 0`, and
         * `(a == b) && (b == c)` re-tests its own result twice more --
         * and each of those retests is a `sete`, a `movzbl` and a
         * `test` on x86 before the one that matters. The inner result is
         * 0 or 1 by construction, so comparing it with zero is the inner
         * comparison itself, negated (`== 0`) or as it stands (`!= 0`).
         *
         * The outer width and signedness fall away with the outer
         * comparison: a 0/1 value compares the same at every width, and
         * the inner comparison keeps its own. 687 sites across lib/libc
         * and lib/libcxx, 417 of them the `sete movzbl test sete` that
         * `!!` and `&&` leave behind. */
        if (i->op == IR_CMP && !i->flt && kb && B == 0 &&
            (i->pred == B_EQ || i->pred == B_NE) && i->a >= 0 &&
            d.cnt[i->a] == 1 && d.ins[i->a] >= 0) {
            struct ir_ins *in = &fn->ins[d.ins[i->a]];
            if (in->op == IR_CMP && in->w != 16) {
                if (i->pred == B_NE) {
                    /* Any comparison's result is already 0 or 1, so
                     * `!= 0` is that value -- true of a FLOAT compare
                     * too, which is why this branch has no `flt` test. */
                    to_mov(i, i->a);
                    changed = 1;
                    continue;
                }
                /* Negating the sense is the part that is not always
                 * sound: over floats `!(a < b)` is NOT `a >= b`, because
                 * an unordered pair makes both false. tests/golden/rt.sh
                 * and complex.c in regalloc-O2.sh both said so. */
                if (!in->flt) {
                    static const enum binop neg[] = {
                        [B_EQ] = B_NE, [B_NE] = B_EQ, [B_LT] = B_GE,
                        [B_LE] = B_GT, [B_GT] = B_LE, [B_GE] = B_LT,
                    };
                    /* `i` IS fn->ins[n], so everything worth keeping
                     * comes off it BEFORE the copy overwrites it. The
                     * inner comparison's OWN width and signedness come
                     * with it: `w` on an IR_CMP is what its operands are
                     * compared at, not what its 0/1 result is read at,
                     * and carrying the outer's 4 into a compare of two
                     * longs truncated both of them. */
                    int dst = i->dst;
                    int line = i->line, col = i->col;
                    enum binop np = neg[in->pred];
                    *i = *in;                   /* the inner comparison... */
                    i->pred = np;               /* ...with the sense flipped */
                    i->dst = dst;
                    i->line = line; i->col = col;
                    changed = 1;
                    continue;
                }
            }
        }
        /* Both operands the SAME value. None of these needs to know
         * anything about what the value is, which is what makes them
         * safe at any width and either signedness -- and `i->flt` is
         * already excluded above, so no NaN can make `x == x` false. */
        if (!i->imm_b && i->a >= 0 && i->a == i->b) {
            switch (i->op) {
            case IR_SUB: case IR_XOR:
                to_const(i, 0); changed = 1; continue;
            case IR_AND: case IR_OR:
                to_mov(i, i->a); changed = 1; continue;
            case IR_CMP:
                to_const(i, i->pred == B_EQ || i->pred == B_LE ||
                            i->pred == B_GE);
                changed = 1; continue;
            default:
                break;
            }
        }
        /* The constant goes on the RIGHT, and otherwise the lower vreg
         * does. Value numbering keys an operation on its operands in
         * order, so `1 + x` and `x + 1` were two different values of the
         * same expression and neither ever matched the other. */
        if (!i->imm_b && i->a >= 0 && i->b >= 0 &&
            (i->op == IR_ADD || i->op == IR_MUL || i->op == IR_AND ||
             i->op == IR_OR  || i->op == IR_XOR) &&
            ((ka && !kb) || (ka == kb && i->a > i->b))) {
            int t = i->a; i->a = i->b; i->b = t;
            t = ka; ka = kb; kb = t;
            long v = A; A = B; B = v;
            changed = 1;
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
    free(lk.gen_of); free(lk.val); free(lk.w);
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
 * block compute the same value. Loads carry `memver`, which a write that
 * names no bytes (a call, asm, a fence, any volatile store) bumps so that
 * every load before it has a different key; a plain store instead removes
 * the entries it may overlap (lvn_mem_kill). */
struct vn {
    enum ir_op op;
    int a, b, w, sign, size;
    int flt;                   /* a float or double operation */
    enum binop pred;
    long imm;
    void *ptr;                 /* GADDR glob / FADDR callee */
    int label;                 /* STRADDR string index */
    int memver;                /* LDVAR / LOAD only */
    /* An operand that is a LITERAL, keyed by its value rather than by
     * the temp holding it (gcse_key_consts). `has` distinguishes "the
     * constant -1" from "no operand", both of which leave a/b at -1. */
    long ca, cb;
    char has_ca, has_cb;
    int result;                /* the temp holding this value */
};

static int vn_eq(const struct vn *x, const struct vn *y)
{
    return x->op == y->op && x->a == y->a && x->b == y->b && x->w == y->w &&
           x->flt == y->flt &&
           x->sign == y->sign && x->size == y->size && x->pred == y->pred &&
           x->imm == y->imm && x->ptr == y->ptr && x->label == y->label &&
           x->memver == y->memver &&
           x->has_ca == y->has_ca && x->has_cb == y->has_cb &&
           (!x->has_ca || x->ca == y->ca) && (!x->has_cb || x->cb == y->cb);
}

/* Build the value key for a CSE-able instruction; returns 0 if it is not one
 * (VOLATILE loads/ldvars, calls, stores — anything with an effect or that we
 * don't number).
 *
 * A float or double operation is numbered like an integer one. EmbCC
 * compiles every program as if FENV_ACCESS were off (the rounding mode is
 * the default one, and no flag is ever tested), and then the same operation
 * on the same operands gives the same bits every time -- a NaN included,
 * since the machine that makes it is the same -- so computing it once is
 * computing it. `(x*y)/(x*y)` in fdlibm was two multiplies, two calls on a
 * Cortex-M3, with x and y kept live across the first. `flt` is part of the
 * key: an integer and a floating add of the same temps are different
 * values. Long double is not numbered (w == 16, below). */
static int vn_key(struct ir_ins *i, int memver, struct vn *k)
{
    memset(k, 0, sizeof *k);
    k->op = i->op; k->a = -1; k->b = -1;
    k->flt = i->flt;
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

/* A numbered instruction becomes a copy of the earlier result. The copy
 * moves the RESULT, so it takes the result's form rather than the
 * operation's: a float comparison's result is an int 0/1 (its `w` and
 * `flt` describe the operands), and a float value is copied the way
 * irgen's own merges copy one, with `flt` clear. (A comparison's copy
 * has not been seen to survive: its destination is written once, so
 * copy propagation removes it. This keeps the form right if it does.) */
static void vn_to_mov(struct ir_ins *i, int src)
{
    if (i->flt) {
        if (i->op == IR_CMP) {
            i->w = 4;
            i->sign = 0;
        }
        i->flt = 0;
    }
    to_mov(i, src);
}

/* Replace a computation that reproduces an earlier one in the same block with a
 * copy of that earlier result; fold/copyprop/dce then remove the redundancy.
 * The block is the run between labels; a store removes the loads whose bytes it
 * may overlap, and any other memory write bumps `memver` (part of a load's
 * key), so no load is reused across a write that could have changed it. Sound: temps are
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

/* The value table, hashed.
 *
 * Both value-numbering passes looked an instruction's key up by walking
 * every entry, and LVN also forgot what a definition replaced by walking
 * every entry again: on one block of 8000 plain statements that was
 * O(n^2) twice over, and vn_eq and vn_kill were the second and third
 * hottest functions of the -O2 compile. Now a key is found through its
 * hash, and the entries that name a vreg (as an operand or as the result)
 * are filed under it, so a definition reaches exactly the entries it
 * makes stale.
 *
 * What makes this give the same answers is that a key is in the table
 * at most once: an entry is only ever added after a lookup of its key
 * found nothing. So "the first matching entry", which is what the walk
 * returned, is "the matching entry", and the order of the entries --
 * which the hash does not keep -- never mattered. */
struct vnent {
    struct vn k;
    unsigned h;                /* its bucket */
    int prev, next;            /* the bucket's chain; -1 ends it */
    char live;
};
struct vnref { int ent, next; };
struct vntab {
    struct vnent *e;
    int ne, cape;
    int *head;
    unsigned mask;
    /* LVN only (byv != NULL): per vreg, the entries naming it, for
     * vn_kill; and the loads keyed at the current memory version, the
     * only entries a store can make stale (lvn_mem_kill). */
    struct vnref *r;
    int nr, capr;
    int *byv, nv;
    int *ld;
    int nld, capld;
};

static unsigned vn_mix(unsigned h, unsigned long x)
{
    h ^= (unsigned)x;
    h *= 0x9E3779B1u;
    h ^= (unsigned)(x >> 16 >> 16);
    h *= 0x85EBCA77u;
    return h ^ (h >> 15);
}

/* A hash of exactly the fields vn_eq compares, so equal keys always
 * meet in one bucket. */
static unsigned vn_hash(const struct vn *k)
{
    unsigned char pb[sizeof k->ptr];
    unsigned long pv = 0;
    memcpy(pb, &k->ptr, sizeof pb);
    for (size_t j = 0; j < sizeof pb; j++)
        pv = pv * 257 + pb[j];
    unsigned h = vn_mix(0, (unsigned long)k->op);
    h = vn_mix(h, (unsigned long)(long)k->a);
    h = vn_mix(h, (unsigned long)(long)k->b);
    h = vn_mix(h, (unsigned long)(long)k->w * 64 + (unsigned long)(long)k->size);
    h = vn_mix(h, (unsigned long)(long)k->sign * 4 + (unsigned long)(long)k->flt);
    h = vn_mix(h, (unsigned long)k->pred);
    h = vn_mix(h, (unsigned long)k->imm);
    h = vn_mix(h, pv);
    h = vn_mix(h, (unsigned long)(long)k->label);
    h = vn_mix(h, (unsigned long)(long)k->memver);
    h = vn_mix(h, (unsigned long)(k->has_ca * 2 + k->has_cb));
    if (k->has_ca) h = vn_mix(h, (unsigned long)k->ca);
    if (k->has_cb) h = vn_mix(h, (unsigned long)k->cb);
    return h;
}

/* `nins` bounds how many entries one walk can make; the buckets are
 * twice that, so a chain is short. */
static void vntab_init(struct vntab *t, int nins, int nv, int index)
{
    memset(t, 0, sizeof *t);
    unsigned nb = 16;
    while (nb < (unsigned)nins * 2u && nb < (1u << 30))
        nb *= 2;
    t->mask = nb - 1;
    t->head = xmalloc((size_t)nb * sizeof *t->head);
    for (unsigned b = 0; b < nb; b++)
        t->head[b] = -1;
    if (index) {
        t->nv = nv;
        t->byv = xmalloc((size_t)(nv ? nv : 1) * sizeof *t->byv);
        for (int v = 0; v < nv; v++)
            t->byv[v] = -1;
    }
}

static void vntab_free(struct vntab *t)
{
    free(t->e); free(t->head); free(t->r); free(t->byv); free(t->ld);
}

/* The result of the entry whose key equals k, or -1. */
static int vntab_find(const struct vntab *t, const struct vn *k)
{
    for (int x = t->head[vn_hash(k) & t->mask]; x >= 0; x = t->e[x].next)
        if (vn_eq(&t->e[x].k, k))
            return t->e[x].k.result;
    return -1;
}

static void vntab_file(struct vntab *t, int v, int ent)
{
    if (v < 0 || v >= t->nv)
        return;
    if (t->nr == t->capr) {
        t->capr = t->capr ? t->capr * 2 : 64;
        t->r = xrealloc(t->r, (size_t)t->capr * sizeof *t->r);
    }
    t->r[t->nr].ent = ent;
    t->r[t->nr].next = t->byv[v];
    t->byv[v] = t->nr++;
}

static void vntab_add(struct vntab *t, const struct vn *k)
{
    if (t->ne == t->cape) {
        t->cape = t->cape ? t->cape * 2 : 64;
        t->e = xrealloc(t->e, (size_t)t->cape * sizeof *t->e);
    }
    int x = t->ne++;
    struct vnent *e = &t->e[x];
    e->k = *k;
    e->h = vn_hash(k) & t->mask;
    e->prev = -1;
    e->next = t->head[e->h];
    if (e->next >= 0)
        t->e[e->next].prev = x;
    t->head[e->h] = x;
    e->live = 1;
    if (t->byv) {
        vntab_file(t, k->a, x);
        vntab_file(t, k->b, x);
        vntab_file(t, k->result, x);
        if (k->op == IR_LOAD || k->op == IR_LDVAR) {
            if (t->nld == t->capld) {
                t->capld = t->capld ? t->capld * 2 : 64;
                t->ld = xrealloc(t->ld, (size_t)t->capld * sizeof *t->ld);
            }
            t->ld[t->nld++] = x;
        }
    }
}

static void vntab_unlink(struct vntab *t, int x)
{
    struct vnent *e = &t->e[x];
    if (!e->live)
        return;
    if (e->prev >= 0) t->e[e->prev].next = e->next;
    else              t->head[e->h] = e->next;
    if (e->next >= 0) t->e[e->next].prev = e->prev;
    e->live = 0;
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
static void vn_kill(struct vntab *t, int v)
{
    if (v < 0 || v >= t->nv) {           /* not filed: look at them all */
        for (int x = 0; x < t->ne; x++)
            if (t->e[x].k.a == v || t->e[x].k.b == v || t->e[x].k.result == v)
                vntab_unlink(t, x);
        return;
    }
    for (int r = t->byv[v]; r >= 0; r = t->r[r].next)
        vntab_unlink(t, t->r[r].ent);
    t->byv[v] = -1;
}

/* Forget everything: a block boundary. Only what was filed is undone,
 * so a function of many small blocks pays for its entries, not for its
 * vregs at every label. */
static void vntab_clear(struct vntab *t)
{
    for (int x = 0; x < t->ne; x++) {
        struct vnent *e = &t->e[x];
        t->head[e->h] = -1;
        if (t->byv) {
            if (e->k.a >= 0 && e->k.a < t->nv) t->byv[e->k.a] = -1;
            if (e->k.b >= 0 && e->k.b < t->nv) t->byv[e->k.b] = -1;
            if (e->k.result >= 0 && e->k.result < t->nv)
                t->byv[e->k.result] = -1;
        }
    }
    t->ne = t->nr = t->nld = 0;
}

/* Defined with the alias analysis further down. */
static char *slots_taken(struct ir_func *fn);
static int lvn_mem_kill(struct ir_func *fn, struct defs *d, const char *taken,
                        struct vntab *tb, const struct ir_ins *ins);

static int pass_lvn(struct ir_func *fn)
{
    int changed = 0, memver = 0;
    struct vntab tb;
    vntab_init(&tb, fn->nins, fn->nvregs, 1);
    /* What a store can reach, so it forgets only those loads
     * (lvn_mem_kill). The definitions are counted once, before the walk
     * turns any instruction into a copy: a copy keeps its destination,
     * so the counts stay true, and the walk to a base follows the copy
     * to the same value. */
    struct defs d;
    compute_defs(fn, &d);
    char *taken = slots_taken(fn);
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        enum ir_op op0 = i->op;
        if (op0 == IR_LABEL) { vntab_clear(&tb); continue; } /* block boundary */
        struct vn k;
        if (i->dst >= 0 && vn_key(i, memver, &k)) {
            int hit = vntab_find(&tb, &k);
            if (hit >= 0 && hit != i->dst) {
                vn_to_mov(i, hit);
                changed = 1;
                g_did.lvn++;
                op0 = IR_MOV;      /* what it is NOW, for the kill below */
            } else if (hit < 0) {
                vn_kill(&tb, i->dst);
                k.result = i->dst;
                vntab_add(&tb, &k);
                if (writes_memory(op0) &&
                    !lvn_mem_kill(fn, &d, taken, &tb, i)) {
                    memver++;
                    tb.nld = 0;    /* every load keyed so far is stale */
                }
                continue;
            }
        }
        /* Anything else that assigns a vreg -- a call's result, a store
         * to a slot, an instruction with no key at all -- invalidates
         * what named it. Inline asm and a landing pad write temps that
         * def_target cannot report, so they clear the table outright. */
        if (op0 == IR_ASM || op0 == IR_LANDING) {
            vntab_clear(&tb);
        } else {
            int t = def_target(i);
            if (t >= 0)
                vn_kill(&tb, t);
        }
        /* A store forgets only the reads it could reach; everything
         * else that writes memory forgets them all. */
        if (writes_memory(op0) &&
            !lvn_mem_kill(fn, &d, taken, &tb, i)) {
            memver++;
            tb.nld = 0;
        }
    }
    vntab_free(&tb);
    free(taken);
    free_defs(&d);
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

/* The rule, one move at a time: walking the function in order, each
 * eligible `dst = mov a` rewrites every read of dst, everywhere, to
 * whatever its `a` holds at that moment. That is a walk over the whole
 * function per move, and a function of 8000 statements spent most of
 * its -O2 compile there (each_read and repl_cb on top of the profile,
 * 30 s for the thumb compile).
 *
 * pass_copyprop computes the same rewrite in one walk. It is used when
 * the eligible moves make no cycle (`x = mov y` and `y = mov x`, each
 * the only definition: values nothing ever computes), which is every
 * function in practice; a cycle falls back to this, whose answer then
 * depends on the order in a way not worth reproducing. */
static int copyprop_by_move(struct ir_func *fn, struct defs *d)
{
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
        if (d->cnt[i->a] != 1 || d->cnt[i->dst] != 1)
            continue;
        struct repl r = { i->dst, i->a, 0 };
        for (int m = 0; m < fn->nins; m++)
            each_read(&fn->ins[m], repl_cb, &r);
        if (r.n) {
            changed = 1;   /* the MOV is now dead; DCE removes it */
            g_did.copy += r.n;
        }
    }
    return changed;
}

/* What the one-at-a-time rule comes to. Each eligible move is an edge
 * dst -> a, and a dst has at most one (it has one definition), so the
 * edges form chains, and every read of a vreg on a chain ends up naming
 * the chain's last vreg, its root -- in whatever order the moves are
 * met, because a move met later than the one that copied FROM it
 * rewrites that copy's readers again. Which is also why the order shows
 * in one place: how many rewrites a read took, which the remark counts.
 * A read of u is rewritten when u's move is met, and lands on the
 * vreg the move's own operand holds by then -- so it takes one rewrite
 * for every step down the chain where the next move comes LATER in the
 * function than the one before it, with the root counting as last of
 * all. nrw[] is that number. */
struct cpmap { const int *root, *nrw; int nv; long n; };
static void cpmap_cb(int *p, void *ctx)
{
    struct cpmap *c = ctx;
    int v = *p;
    if (v >= 0 && v < c->nv && c->root[v] >= 0) {
        *p = c->root[v];
        c->n += c->nrw[v];
    }
}

static int pass_copyprop(struct ir_func *fn)
{
    struct defs d;
    compute_defs(fn, &d);
    int nv = fn->nvregs;
    /* src[v]/pos[v]: v's eligible move, by the same test as
     * copyprop_by_move's, and where it is; -1 for none. */
    int *src = xmalloc((size_t)(nv ? nv : 1) * sizeof *src);
    int *pos = xmalloc((size_t)(nv ? nv : 1) * sizeof *pos);
    int nmov = 0;
    for (int v = 0; v < nv; v++)
        src[v] = -1;
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        if (i->op != IR_MOV || i->a < 0 || i->dst < 0 || i->a == i->dst)
            continue;
        if (d.cnt[i->a] != 1 || d.cnt[i->dst] != 1)
            continue;
        src[i->dst] = i->a;
        pos[i->dst] = n;
        nmov++;
    }
    if (nmov == 0) {
        free(src); free(pos); free_defs(&d);
        return 0;
    }
    /* root[v]: where a read of v ends up (-1: v has no move). state: 0
     * not yet seen, 1 on the chain being walked, 2 resolved. */
    int *root = xmalloc((size_t)nv * sizeof *root);
    int *nrw = xmalloc((size_t)nv * sizeof *nrw);
    int *stk = xmalloc((size_t)nv * sizeof *stk);
    char *state = xcalloc((size_t)nv, 1);
    int cycle = 0;
    for (int v = 0; v < nv && !cycle; v++) {
        if (src[v] < 0 || state[v] == 2)
            continue;
        int sp = 0, u = v;
        while (src[u] >= 0 && state[u] == 0) {
            state[u] = 1;
            stk[sp++] = u;
            u = src[u];
        }
        if (src[u] >= 0 && state[u] == 1) {
            cycle = 1;
            break;
        }
        /* u is a root, or a vreg already resolved */
        int r = src[u] < 0 ? u : root[u];
        while (sp > 0) {
            int w = stk[--sp];
            int s = src[w];
            int later = src[s] < 0 || pos[s] > pos[w];
            root[w] = r;
            nrw[w] = (src[s] < 0 ? 0 : nrw[s]) + later;
            state[w] = 2;
        }
    }
    int changed;
    if (cycle) {
        changed = copyprop_by_move(fn, &d);
    } else {
        for (int v = 0; v < nv; v++)
            if (src[v] < 0)
                root[v] = -1;
        struct cpmap c = { root, nrw, nv, 0 };
        for (int n = 0; n < fn->nins; n++)
            each_read(&fn->ins[n], cpmap_cb, &c);
        changed = c.n > 0;   /* the MOVs are now dead; DCE removes them */
        g_did.copy += c.n;
    }
    free(src); free(pos); free(root); free(nrw); free(stk); free(state);
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

/* Mark a temp live, and queue the instructions that define it for the
 * same treatment. */
struct mark { char *live; int n, nins; const int *dhead, *dnext;
              char *live_ins; int *work, nwork; };
static void mark_cb(int *p, void *ctx)
{
    struct mark *m = ctx;
    int v = *p;
    if (v < 0 || v >= m->n || m->live[v])
        return;
    m->live[v] = 1;
    for (int k = m->dhead[v]; k >= 0; k = m->dnext[k]) {
        int x = k < m->nins ? k : k - m->nins;
        if (!m->live_ins[x]) {
            m->live_ins[x] = 1;
            m->work[m->nwork++] = x;
        }
    }
}

/* ---- dead code, by marking what is live rather than counting uses ----
 *
 * Counting uses cannot remove a CYCLE. `i = i + 1` feeding `i = mov
 * next` feeding the add again is two instructions that each have a use
 * -- each other -- so a use count never reaches zero for either, and a
 * loop counter nothing reads survives for the life of the function.
 * That is the ordinary shape left behind whenever a loop's test stops
 * naming its own induction variable, which is exactly what the test
 * replacement in `ivsr_one` does.
 *
 * So liveness is computed the other way round: an instruction is live
 * when its EFFECT is not its result -- a store, a branch, a label, a
 * return, a call that can be observed -- or when something live reads
 * what it defines. Everything else is dead, cycles included, because a
 * cycle no live instruction reaches is never marked.
 *
 * IR_STVAR's "result" is a frame slot, and a slot is read by an LDVAR
 * or an ADDR, both of which each_read reports -- so the same rule
 * covers dead stores to a local with no remaining readers. A volatile
 * one is live regardless: the access itself is the effect.
 *
 * The marking is a worklist: a temp found live queues the instructions
 * that define it, found through a list per temp. It used to sweep the
 * function until a sweep changed nothing, and a sweep carries liveness
 * one step backwards through code that runs forwards -- a chain of 4000
 * joins, each copy read by the next, took 4000 sweeps. Both compute the
 * least set closed under the two rules, so they mark the same set. */
static int pass_dce(struct ir_func *fn)
{
    int nins = fn->nins, nvr = fn->nvregs;
    if (nins == 0)
        return 0;
    char *live_ins = xcalloc((size_t)nins, 1);
    char *live_t = xcalloc((size_t)(nvr ? nvr : 1), 1);
    /* the instructions defining each temp: def_target, and a landing
     * pad's second temp in `b` */
    int *dhead = xmalloc((size_t)(nvr ? nvr : 1) * sizeof *dhead);
    int *dnext = xmalloc((size_t)nins * 2 * sizeof *dnext);
    int *work = xmalloc((size_t)nins * sizeof *work);
    for (int v = 0; v < nvr; v++)
        dhead[v] = -1;
    for (int n = nins - 1; n >= 0; n--) {
        struct ir_ins *i = &fn->ins[n];
        int t = def_target(i);
        dnext[n] = dnext[nins + n] = -1;
        if (t >= 0 && t < nvr) {
            dnext[n] = dhead[t];
            dhead[t] = n;
        }
        int second = i->op == IR_LANDING ? i->b : -1;
        if (second >= 0 && second < nvr && second != t) {
            /* filed as nins + n, which names instruction n again */
            dnext[nins + n] = dhead[second];
            dhead[second] = nins + n;
        }
    }
    struct mark m = { live_t, nvr, nins, dhead, dnext, live_ins, work, 0 };
    for (int n = 0; n < nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        if (i->op == IR_STVAR) {
            if (i->vol)
                live_ins[n] = 1;
            continue;                 /* otherwise its slot decides */
        }
        /* A call that touches no memory and cannot throw does nothing
         * but produce a value, so it lives or dies with that value. */
        if (i->op == IR_CALL && !i->indirect && i->callee &&
            i->callee->inf_no_read && i->callee->inf_no_write &&
            i->callee->is_nothrow && def_target(i) >= 0)
            continue;
        /* a volatile local's read happens even when nothing uses it:
         * `(void)v;` is a read the program asked for */
        if (is_pure(i->op) && def_target(i) >= 0 && !i->vol)
            continue;
        live_ins[n] = 1;
    }
    for (int n = 0; n < nins; n++)
        if (live_ins[n])
            work[m.nwork++] = n;
    while (m.nwork > 0) {
        int n = work[--m.nwork];
        each_read(&fn->ins[n], mark_cb, &m);
    }
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
        if (!live_ins[n]) {
            changed = 1;
            g_did.dce++;
            continue;   /* nothing live reaches what it computes */
        }
        if (j != n)
            fn->ins[j] = fn->ins[n];
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
    free(live_ins); free(live_t);
    free(dhead); free(dnext); free(work);
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
    /* Successors. Two would do for every branch this IR has -- except
     * the indirect one: `goto *p` reaches any label whose address was
     * taken, which is as many as the function has. */
    int *succ, nsucc, capsucc;
    int *pred, npred;
    int idom, rpo;                  /* immediate dominator; reverse-postorder # */
    int dpre, dpost;                /* dominator-tree DFS numbers (bb_dominates);
                                     * -1 off the tree */
    int *phi_local, *phi_res, nphi; /* phi(local) -> result temp, per block */
    int **phi_inc;                  /* phi_inc[p][k] = value on edge from pred p */
};

/* A fresh instruction with no operands: what irgen's emit() starts from.
 * The vreg and label fields are -1, not 0 -- slot 0 is a real local, the
 * first parameter, and a field left at 0 names it. A branch built here
 * with dst 0 told AVR's width test the branch WROTE that parameter, so
 * where it was eight bytes a four-byte zero test was lowered over eight
 * registers: past r31 the encoder refused it, short of r31 four bytes of
 * an unrelated value decided the branch. */
static void ins_blank(struct ir_ins *i)
{
    memset(i, 0, sizeof *i);
    i->dst = i->a = i->b = -1;
    i->label = -1;
}

/* Growable instruction buffer, for rebuilding fn->ins out of SSA. */
struct ibuf { struct ir_ins *p; int n, cap; };
/* EMBCC_IBUF_MOVE=1: every push MOVES the buffer and scribbles over the
 * old one. A pointer an earlier push returned is valid only until the
 * next, and holding one across it reads freed memory -- but only when
 * that push happens to grow the buffer, which is how such reads survive
 * ordinary testing. With this every one of them reads garbage at once
 * (tests/golden/ibuf-move.sh). Each push copies the whole buffer, so a
 * compile is many times slower: for testing only. */
static int g_ib_move = -1;

static struct ir_ins *ib_push(struct ibuf *b)
{
    if (g_ib_move < 0)
        g_ib_move = getenv("EMBCC_IBUF_MOVE") != NULL;
    if (g_ib_move) {
        struct ir_ins *np = xmalloc((size_t)(b->n + 1) * sizeof *np);
        if (b->n)
            memcpy(np, b->p, (size_t)b->n * sizeof *np);
        if (b->p) {
            memset(b->p, 0xa5, (size_t)b->cap * sizeof *b->p);
            free(b->p);
        }
        b->p = np;
        b->cap = b->n + 1;
    } else if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 64;
        b->p = xrealloc(b->p, (size_t)b->cap * sizeof *b->p);
    }
    struct ir_ins *i = &b->p[b->n++];
    ins_blank(i);
    return i;
}

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
static int as_op_const(struct ir_func *fn, struct defs *d, struct ir_ins *i,
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

static int pass_reassoc(struct ir_func *fn)
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
static int pass_idxoff(struct ir_func *fn)
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

static void free_cfg(struct bb *bb, int nbb)
{
    for (int i = 0; i < nbb; i++) { free(bb[i].pred); free(bb[i].succ); }
    free(bb);
}

/* Add t to block i's successors, once. build_cfg adds every successor
 * of block i before moving on to i + 1, so whether t is already there is
 * a stamp to ask, not a list to walk -- a switch of 4000 cases has 4000
 * successors. */
static void cfg_succ(struct bb *bb, int *seen, int i, int t)
{
    if (seen[t] == i + 1)
        return;                     /* an edge once is an edge */
    seen[t] = i + 1;
    struct bb *b = &bb[i];
    if (b->nsucc == b->capsucc) {
        b->capsucc = b->capsucc ? b->capsucc * 2 : 2;
        b->succ = xrealloc(b->succ, (size_t)b->capsucc * sizeof *b->succ);
    }
    b->succ[b->nsucc++] = t;
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
             op == IR_RET || op == IR_UD2 || op == IR_IGOTO ||
             op == IR_SWITCH) && i + 1 < N)
            lead[i + 1] = 1;
    }
    int nbb = 0;
    for (int i = 0; i < N; i++) if (lead[i]) nbb++;
    if (nbb == 0) nbb = 1;
    struct bb *bb = xcalloc((size_t)nbb, sizeof *bb);
    int b = 0, prev = 0;
    for (int i = 1; i <= N; i++)
        if (i == N || lead[i]) { bb[b].start = prev; bb[b].end = i; prev = i; b++; }
    for (int i = 0; i < nbb; i++) {
        bb[i].idom = -1; bb[i].rpo = -1;
        bb[i].dpre = bb[i].dpost = -1;
    }

    int *l2b = xmalloc((size_t)(fn->nlabels ? fn->nlabels : 1) * sizeof *l2b);
    for (int l = 0; l < fn->nlabels; l++) l2b[l] = -1;
    for (int i = 0; i < nbb; i++)
        if (bb[i].end > bb[i].start && fn->ins[bb[i].start].op == IR_LABEL)
            l2b[fn->ins[bb[i].start].label] = i;

    /* Which labels a computed goto could land on: the ones something
     * takes the address of. Nothing else can be the target of `goto *p`
     * -- the value has to have come from a `&&label` somewhere in this
     * function, because that is the only thing that produces one. */
    char *taken_lbl = NULL;
    for (int i = 0; i < N; i++)
        if (fn->ins[i].op == IR_LABELADDR && fn->ins[i].label >= 0 &&
            fn->ins[i].label < fn->nlabels) {
            if (!taken_lbl)
                taken_lbl = xcalloc((size_t)(fn->nlabels ? fn->nlabels : 1), 1);
            taken_lbl[fn->ins[i].label] = 1;
        }

    /* seen[t] == i + 1: t is already a successor of i (cfg_succ) */
    int *seen = xcalloc((size_t)nbb, sizeof *seen);
    for (int i = 0; i < nbb; i++) {
        enum ir_op op = bb[i].end > bb[i].start ? fn->ins[bb[i].end - 1].op : IR_UD2;
        int L = bb[i].end > bb[i].start ? fn->ins[bb[i].end - 1].label : -1;
        if (op == IR_RET || op == IR_UD2) {
            /* no successors */
        } else if (op == IR_JMP) {
            if (l2b[L] >= 0) cfg_succ(bb, seen, i, l2b[L]);
        } else if (op == IR_BRZ || op == IR_BRNZ) {
            if (l2b[L] >= 0) cfg_succ(bb, seen, i, l2b[L]);
            if (i + 1 < nbb) cfg_succ(bb, seen, i, i + 1);
        } else if (op == IR_SWITCH) {
            /* the default, and every entry of the table */
            const struct ir_ins *sw = &fn->ins[bb[i].end - 1];
            if (L >= 0 && L < fn->nlabels && l2b[L] >= 0)
                cfg_succ(bb, seen, i, l2b[L]);
            for (int k = 0; k < fn->jt[sw->jt].n; k++) {
                int tl = fn->jt[sw->jt].labels[k];
                if (tl >= 0 && tl < fn->nlabels && l2b[tl] >= 0)
                    cfg_succ(bb, seen, i, l2b[tl]);
            }
        } else if (op == IR_IGOTO) {
            /* An edge to every address-taken label. Without these the
             * blocks they open look unreachable, and the passes that
             * DELETE unreachable code delete the program. */
            for (int l = 0; taken_lbl && l < fn->nlabels; l++)
                if (taken_lbl[l] && l2b[l] >= 0)
                    cfg_succ(bb, seen, i, l2b[l]);
        } else if (i + 1 < nbb) {
            cfg_succ(bb, seen, i, i + 1);
        }
    }
    free(taken_lbl);
    free(seen);
    /* Predecessors by source block, then by successor. A block's
     * successors are distinct, so none is a duplicate, and each list is
     * allocated once at its size: added one at a time, each checked every
     * earlier one and grew the array by one, and for the block after a
     * 4000-case switch that was the hottest thing in the -O2 compile. */
    for (int i = 0; i < nbb; i++)
        for (int k = 0; k < bb[i].nsucc; k++)
            bb[bb[i].succ[k]].npred++;
    for (int i = 0; i < nbb; i++) {
        if (bb[i].npred)
            bb[i].pred = xmalloc((size_t)bb[i].npred * sizeof *bb[i].pred);
        bb[i].npred = 0;
    }
    for (int i = 0; i < nbb; i++)
        for (int k = 0; k < bb[i].nsucc; k++) {
            struct bb *t = &bb[bb[i].succ[k]];
            t->pred[t->npred++] = i;
        }
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
    /* Number the dominator tree depth-first, so that "does s dominate b"
     * is two comparisons (bb_dominates) rather than a walk up b's
     * dominators -- which on a chain of 4000 else-ifs is a walk of
     * thousands, asked of every edge by every loop pass. */
    int nbb = 0;
    for (int i = 0; i < norder; i++)
        if (order[i] + 1 > nbb) nbb = order[i] + 1;
    int *kid = xcalloc((size_t)nbb + 1, sizeof *kid);
    int *kids = xmalloc((size_t)(norder ? norder : 1) * sizeof *kids);
    int *stk = xmalloc((size_t)(norder ? norder : 1) * sizeof *stk);
    int *it = xmalloc((size_t)(norder ? norder : 1) * sizeof *it);
    for (int i = 0; i < norder; i++) {
        int b = order[i];
        bb[b].dpre = bb[b].dpost = -1;
        if (b != 0 && bb[b].idom >= 0 && bb[b].idom < nbb) kid[bb[b].idom + 1]++;
    }
    for (int b = 0; b < nbb; b++) kid[b + 1] += kid[b];
    {
        int *fill = xmalloc((size_t)(nbb ? nbb : 1) * sizeof *fill);
        for (int b = 0; b < nbb; b++) fill[b] = kid[b];
        for (int i = 0; i < norder; i++) {
            int b = order[i];
            if (b != 0 && bb[b].idom >= 0 && bb[b].idom < nbb)
                kids[fill[bb[b].idom]++] = b;
        }
        free(fill);
    }
    int pre = 0, post = 0, sp = 0;
    if (norder > 0) { stk[sp] = 0; it[sp] = kid[0]; sp++; bb[0].dpre = pre++; }
    while (sp > 0) {
        int u = stk[sp - 1];
        if (it[sp - 1] < kid[u + 1]) {
            int c = kids[it[sp - 1]++];
            if (bb[c].dpre >= 0) continue;
            bb[c].dpre = pre++;
            stk[sp] = c; it[sp] = kid[c]; sp++;
        } else {
            bb[u].dpost = post++;
            sp--;
        }
    }
    free(kid); free(kids); free(stk); free(it);
}

/* Dominance frontiers. df[b] holds the blocks on b's frontier. */
static void compute_df(struct bb *bb, int nbb, int **df, int *ndf)
{
    /* b only ever joins a frontier while b is the block being looked at,
     * so a repeat of b can only be the last entry: that is the whole
     * duplicate test, where a scan of the frontier so far was one per
     * step -- and each list grows by doubling, not by one. */
    int *cap = xcalloc((size_t)(nbb ? nbb : 1), sizeof *cap);
    for (int b = 0; b < nbb; b++) {
        if (bb[b].npred < 2) continue;
        for (int k = 0; k < bb[b].npred; k++) {
            int r = bb[b].pred[k];
            while (r >= 0 && r != bb[b].idom) {
                if (!(ndf[r] > 0 && df[r][ndf[r] - 1] == b)) {
                    if (ndf[r] == cap[r]) {
                        cap[r] = cap[r] ? cap[r] * 2 : 4;
                        df[r] = xrealloc(df[r], (size_t)cap[r] * sizeof(int));
                    }
                    df[r][ndf[r]++] = b;
                }
                r = bb[r].idom;
            }
        }
    }
    free(cap);
}

/* Where p is in b's predecessor list, or -1. build_cfg lists them in
 * increasing block order, once each, so a binary search finds the one
 * index a scan from the front would: a join after a 4000-case switch has
 * 4000 predecessors, and mem2reg asked this once per incoming edge. */
static int bb_pred_index(const struct bb *b, int p)
{
    int lo = 0, hi = b->npred - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (b->pred[mid] == p) return mid;
        if (b->pred[mid] < p) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
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
    int pi = bb_pred_index(S, p);
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
        free(bb[i].pred); free(bb[i].succ);
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
        /* A PARAMETER promotes too. It used to be excluded for having
         * no defining instruction to seed its first read with -- but it
         * does have one, outside the IR: the prologue, which writes the
         * incoming value into the parameter's own home. Slots and temps
         * share one numbering (vreg L IS local L), so that home is a
         * vreg like any other and the seed is simply L itself. Every
         * read of a parameter that is never assigned then becomes a
         * direct use of the incoming value and the read disappears --
         * 1692 of the 2584 ldvars across lib/libc and lib/libcxx, with
         * only 173 stvars naming a parameter at all. What is really
         * gained is downstream: value numbering, PRE, LICM and strength
         * reduction all used to stop at a parameter read. */
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
        /* ...and a one- or two-byte integer or pointer: every pointer
         * and `int` on AVR, and a char or short anywhere. Each read of
         * it becomes an EXTENSION of the current value from the local's
         * width, which is exactly what the narrow load did; each store
         * keeps the whole value, since only the low bytes are ever read
         * back. Refusing them left every AVR local in memory, where the
         * value numbering, LICM and strength reduction below cannot see. */
        int narrow = Li->is_int_or_ptr && !Li->is_int128 &&
                     (Li->size == 1 || Li->size == 2);
        int promotable = Li->is_scalar_int_or_ptr || Li->is_scalar_float ||
                         narrow;
        if (!Li->size)                          { ok[L] = 0; why[L] = "type-unknown"; }
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
            (in->vol ||
             (fn->locals[in->a].size < 4
                  ? in->size != fn->locals[in->a].size
                  : !m2r_plain(in->size, in->sign, in->w)) ||
             /* a narrower read of a wider local is its FIRST bytes,
              * which are the value's low end only little-endian */
             (target_big_endian() &&
              in->size != fn->locals[in->a].size))) {
            ok[in->a] = 0;
            why[in->a] = in->vol ? "read-is-volatile"
                                 : "read-is-partial-or-extending";
        }
        if (in->op == IR_STVAR && in->dst >= 0 && in->dst < nvars &&
            in->vol && ok[in->dst]) {
            ok[in->dst] = 0; why[in->dst] = "write-is-volatile";
        }
        /* A narrow local is promoted only if every write covers all of
         * it: a partial store leaves bytes the new value does not carry. */
        if (in->op == IR_STVAR && in->dst >= 0 && in->dst < nvars &&
            ok[in->dst] && fn->locals[in->dst].size < 4 &&
            in->size != fn->locals[in->dst].size) {
            ok[in->dst] = 0; why[in->dst] = "write-is-partial";
        }
    }
    if (remarks_on())
        for (int L = 0; L < nvars; L++) {
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
    /* -g: a promoted variable that is ASSIGNED leaves its slot behind.
     * For a local nothing writes the slot any more, which a backend sees
     * for itself; a parameter's slot is still written once, by the
     * prologue, and only this can say the value there goes stale. */
    for (int i = 0; i < fn->nins; i++) {
        const struct ir_ins *in = &fn->ins[i];
        if (in->op != IR_STVAR || in->dst < 0 || in->dst >= nvars ||
            prom[in->dst] < 0)
            continue;
        for (int d = 0; d < fn->ndbgvars; d++)
            if (fn->dbgvars[d].vreg == in->dst)
                fn->dbgvars[d].moved = 1;
    }

    /* The ENTRY BLOCK must not be a join. It is one whenever the
     * function's first instruction is a label something branches back
     * to -- a `while` at the top of a function, and every tail call
     * pass_tailrec turned into a loop. Such a block has exactly ONE
     * predecessor, the back edge, because the function entry is not a
     * block; compute_df skips it for having npred < 2, no phi is placed,
     * and the value assigned round the loop is silently dropped. For a
     * local that is unreachable in a defined program (nothing could have
     * initialised it before a loop that starts at instruction zero), and
     * for a PARAMETER it is the argument the caller passed:
     * `sum_to(n, acc)` came out as a branch around an empty body, three
     * million tail calls that never returned.
     *
     * One explicit jump gives the entry a block of its own, and the
     * header two predecessors and a phi. pass_cfgclean takes the jump
     * out again afterwards -- it goes to the very next instruction. */
    if (fn->nins > 0 && fn->ins[0].op == IR_LABEL) {
        int L0 = fn->ins[0].label, reached = 0;
        for (int i = 1; i < fn->nins && !reached; i++) {
            enum ir_op op = fn->ins[i].op;
            if ((op == IR_JMP || op == IR_BRZ || op == IR_BRNZ) &&
                fn->ins[i].label == L0)
                reached = 1;
        }
        if (reached) {
            struct ibuf eb = { 0, 0, 0 };
            struct ir_ins *j = ib_push(&eb);
            j->op = IR_JMP; j->label = L0;
            j->dst = -1; j->a = -1; j->b = -1;
            j->line = fn->ins[0].line; j->col = fn->ins[0].col;
            j->synth = 1;
            for (int i = 0; i < fn->nins; i++)
                *ib_push(&eb) = fn->ins[i];
            free(fn->ins);
            fn->ins = eb.p; fn->nins = eb.n; fn->cap = eb.cap;
        }
    }

    /* 2. CFG + dominance. */
    int nbb, *l2b;
    struct bb *bb = build_cfg(fn, &nbb, &l2b);
    int *order = xmalloc((size_t)nbb * sizeof *order), norder;
    compute_rpo(bb, nbb, order, &norder);
    if (norder != nbb) {   /* unreachable blocks: bail rather than mis-dominate */
        free(order); free(l2b);
        free_cfg(bb, nbb); free(prom); free(ploc); return 0;
    }
    compute_idom(bb, order, norder);

    int **df = xcalloc((size_t)nbb, sizeof *df);
    int *ndf = xcalloc((size_t)nbb, sizeof *ndf);
    compute_df(bb, nbb, df, ndf);

    /* 3. Phi insertion at the iterated dominance frontier of each var's defs.
     *
     * The blocks that store each variable are found in one pass, in block
     * order, rather than by a pass over the function per variable; and
     * "this block has its phi / is on the list" is a stamp of the
     * variable's number per block rather than a flag per (block,
     * variable) -- 4000 locals in a function were 4000 passes. */
    int *dcnt = xcalloc((size_t)nprom + 1, sizeof *dcnt), *work0 = NULL;
    int *dlast = xcalloc((size_t)(nprom ? nprom : 1), sizeof *dlast);
    for (int pass = 0; pass < 2; pass++) {
        for (int pidx = 0; pidx < nprom; pidx++) dlast[pidx] = 0;
        for (int bI = 0; bI < nbb; bI++)
            for (int i = bb[bI].start; i < bb[bI].end; i++) {
                const struct ir_ins *in = &fn->ins[i];
                if (in->op != IR_STVAR || in->dst < 0 || in->dst >= nvars ||
                    prom[in->dst] < 0 || dlast[prom[in->dst]] == bI + 1)
                    continue;
                int pidx = prom[in->dst];
                dlast[pidx] = bI + 1;
                if (pass) work0[dcnt[pidx]++] = bI;
                else dcnt[pidx + 1]++;
            }
        if (!pass) {
            for (int pidx = 0; pidx < nprom; pidx++) dcnt[pidx + 1] += dcnt[pidx];
            work0 = xmalloc((size_t)(dcnt[nprom] ? dcnt[nprom] : 1) * sizeof *work0);
        }
    }
    /* the second pass filled each list from its start, so dcnt[pidx] is
     * now the end of pidx's list, and it starts where the one before ends */
    int *hasphi = xcalloc((size_t)(nbb ? nbb : 1), sizeof *hasphi);
    int *ondef = xcalloc((size_t)(nbb ? nbb : 1), sizeof *ondef);
    int *work = xmalloc((size_t)nbb * sizeof *work);
    for (int pidx = 0; pidx < nprom; pidx++) {
        int L = ploc[pidx], nw = 0;
        for (int k = pidx ? dcnt[pidx - 1] : 0; k < dcnt[pidx]; k++) {
            int bI = work0[k];
            ondef[bI] = pidx + 1;
            work[nw++] = bI;
        }
        while (nw) {
            int x = work[--nw];
            for (int j = 0; j < ndf[x]; j++) {
                int d = df[x][j];
                if (hasphi[d] == pidx + 1) continue;
                hasphi[d] = pidx + 1;
                bb[d].phi_local = xrealloc(bb[d].phi_local, (size_t)(bb[d].nphi+1)*sizeof(int));
                bb[d].phi_res   = xrealloc(bb[d].phi_res,   (size_t)(bb[d].nphi+1)*sizeof(int));
                bb[d].phi_local[bb[d].nphi] = L;
                bb[d].phi_res[bb[d].nphi] = fn->nvregs++;
                bb[d].nphi++;
                if (ondef[d] != pidx + 1) { ondef[d] = pidx + 1; work[nw++] = d; }
            }
        }
    }
    free(ondef); free(dcnt); free(dlast); free(work0);
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
        /* A parameter's entry version is its own vreg -- the prologue
         * put the incoming value there. Everything else starts at a
         * fresh temp defined to zero below, standing for a read of a
         * variable the program never wrote. */
        undef[p] = ploc[p] < nparams ? ploc[p] : fn->nvregs++;
        stk[p] = xmalloc(sizeof(int) * 8); scap[p] = 8;
        stk[p][sp[p]++] = undef[p];
    }
    /* explicit dominator-tree DFS (children = blocks whose idom is this block).
     * Each block's children are listed once, in block order, and pushed in
     * that order; what a block pushes onto the version stacks is logged,
     * and leaving it pops back to where the log stood on entry. Both used
     * to cost blocks x blocks and blocks x variables. */
    int *dstk = xmalloc((size_t)nbb * sizeof *dstk);
    int *mark = xmalloc((size_t)nbb * sizeof *mark);
    int *plog = NULL, nplog = 0, cplog = 0;     /* pidx of each push */
    char *entered = xcalloc((size_t)nbb, 1);
    int *kid = xcalloc((size_t)nbb + 1, sizeof *kid);
    int *kids = xmalloc((size_t)(nbb ? nbb : 1) * sizeof *kids);
    for (int c = 1; c < nbb; c++)
        if (bb[c].idom >= 0 && bb[c].idom < nbb) kid[bb[c].idom + 1]++;
    for (int c = 0; c < nbb; c++) kid[c + 1] += kid[c];
    {
        int *fill = xmalloc((size_t)(nbb ? nbb : 1) * sizeof *fill);
        for (int c = 0; c < nbb; c++) fill[c] = kid[c];
        for (int c = 1; c < nbb; c++)
            if (bb[c].idom >= 0 && bb[c].idom < nbb)
                kids[fill[bb[c].idom]++] = c;
        free(fill);
    }
    int dsp = 0; dstk[dsp++] = 0;
    while (dsp) {
        int b = dstk[dsp - 1];
        if (!entered[b]) {
            entered[b] = 1;
            mark[b] = nplog;
            /* phi defs become the current version */
            for (int k = 0; k < bb[b].nphi; k++) {
                int pidx = prom[bb[b].phi_local[k]];
                if (sp[pidx] == scap[pidx]) { scap[pidx]*=2; stk[pidx]=xrealloc(stk[pidx],(size_t)scap[pidx]*sizeof(int)); }
                stk[pidx][sp[pidx]++] = bb[b].phi_res[k];
                if (nplog == cplog) { cplog = cplog ? cplog * 2 : 64;
                    plog = xrealloc(plog, (size_t)cplog * sizeof *plog); }
                plog[nplog++] = pidx;
            }
            for (int i = bb[b].start; i < bb[b].end; i++) {
                struct ir_ins *in = &fn->ins[i];
                if (in->op == IR_LDVAR && in->a >= 0 && in->a < nvars && prom[in->a] >= 0) {
                    int pidx = prom[in->a];
                    int fw = in->size == 8 ? 8 : 4;
                    int was_float = in->flt;
                    if (fn->locals[ploc[pidx]].size < 4) {
                        /* A narrow local: the read extends the value's low
                         * `size` bytes, as the load did -- size, sign and
                         * width are the load's own. */
                        in->op = IR_EXT; in->a = stk[pidx][sp[pidx]-1];
                        in->b = -1;
                        continue;
                    }
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
                    if (nplog == cplog) { cplog = cplog ? cplog * 2 : 64;
                        plog = xrealloc(plog, (size_t)cplog * sizeof *plog); }
                    plog[nplog++] = pidx;
                    in->op = IR_MOV; in->dst = -1; in->a = -1;  /* mark: drop in rebuild */
                }
            }
            /* fill successors' phi incoming from this block */
            for (int s = 0; s < bb[b].nsucc; s++) {
                int sb = bb[b].succ[s];
                if (!bb[sb].nphi) continue;
                int pk = bb_pred_index(&bb[sb], b);
                for (int k = 0; k < bb[sb].nphi; k++) {
                    int pidx = prom[bb[sb].phi_local[k]];
                    bb[sb].phi_inc[pk][k] = stk[pidx][sp[pidx]-1];
                }
            }
            /* push dom-tree children */
            for (int q = kid[b]; q < kid[b + 1]; q++)
                if (!entered[kids[q]]) dstk[dsp++] = kids[q];
        } else {
            /* leaving b: pop its versions */
            while (nplog > mark[b])
                sp[plog[--nplog]]--;
            dsp--;
        }
    }
    free(mark); free(plog); free(kid); free(kids);

    /* 5. Rebuild the linear IR out of SSA. */
    struct ibuf nb = { 0, 0, 0 };
    for (int p = 0; p < nprom; p++) {   /* entry undef defs */
        if (ploc[p] < nparams)
            continue;                   /* the prologue defined it */
        struct ir_ins *c = ib_push(&nb);
        c->op = IR_CONST; c->dst = undef[p]; c->imm = 0;
        c->w = fn->locals[ploc[p]].size == 8 ? 8 : 4;
        /* The seed for a variable read before it is written: it stands for
         * a value the program never produced, so it corresponds to no
         * source construct at all. The §9.1 exception, marked so the
         * verifier can tell it from a location a pass forgot to copy. */
        c->synth = 1;
    }
    /* A phi in the ENTRY block has no edge to take the function's
     * incoming value from: the entry is not a block, so the only
     * predecessor such a phi has is the back edge that made block 0 a
     * loop header -- which is every function whose first instruction is
     * a loop's label, and every tail call pass_tailrec turned into one.
     *
     * The value is supplied HERE instead, as the copy it is, ahead of
     * block 0's own label so the back edge jumps past it. For a
     * parameter that value is the parameter; for anything else it is the
     * undef seed above. Without it `sum_to(n, acc)` came out as a branch
     * around an empty body: three million tail calls that never
     * returned. */
    for (int k = 0; k < bb[0].nphi; k++) {
        int pidx = prom[bb[0].phi_local[k]];
        struct ir_ins *c = ib_push(&nb);
        c->op = IR_MOV; c->dst = bb[0].phi_res[k];
        c->a = undef[pidx]; c->b = -1;
        c->w = fn->locals[ploc[pidx]].size == 8 ? 8 : 4;
        c->synth = 1;
    }
    struct { int lbl, from, edge_pred; } *tramp = NULL; int ntramp = 0, ctramp = 0;
    for (int b = 0; b < nbb; b++) {
        int hasterm = bb[b].end > bb[b].start;
        enum ir_op top = hasterm ? fn->ins[bb[b].end - 1].op : IR_UD2;
        int isterm = top == IR_JMP || top == IR_BRZ || top == IR_BRNZ ||
                     top == IR_RET || top == IR_UD2 || top == IR_SWITCH;
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
        } else if (top == IR_SWITCH && isterm) {
            /* Every edge out of a switch is a taken edge: a successor
             * with phis gets a trampoline, and every entry (and the
             * default) that named it names the trampoline instead. A
             * block may open with several labels; any of them may be
             * what the table says. */
            struct ir_ins sw = fn->ins[bb[b].end - 1];
            for (int s = 0; s < bb[b].nsucc; s++) {
                int sb = bb[b].succ[s];
                if (!bb[sb].nphi) continue;
                int Lt = fn->nlabels++;
                if (ntramp == ctramp) { ctramp = ctramp?ctramp*2:8;
                    tramp = xrealloc(tramp, (size_t)ctramp*sizeof *tramp); }
                tramp[ntramp].lbl = Lt; tramp[ntramp].from = b;
                tramp[ntramp].edge_pred = sb; ntramp++;
                for (int q = bb[sb].start;
                     q < bb[sb].end && fn->ins[q].op == IR_LABEL; q++) {
                    struct retarget rt = { fn->ins[q].label, Lt };
                    each_label(fn, &sw, retarget_cb, &rt);
                }
            }
            *ib_push(&nb) = sw;
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
        if (lt != IR_JMP && lt != IR_RET && lt != IR_UD2 && lt != IR_SWITCH) {
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
    free(dstk); free(entered);
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
     * materialization (CONST, a lea for &local, a sign/zero-extend, a
     * bswap) costs less to recompute than to keep live across the
     * dominated region — global-CSEing those only lengthens a live range
     * (forcing a spill or a callee-saved reg) for no win. Redundant
     * arithmetic/compares are the profitable case. Memory reads
     * (LDVAR/LOAD) stay with the memory-versioned pass_lvn. */
    case IR_ADD: case IR_SUB: case IR_MUL:
    case IR_DIV: case IR_MOD: case IR_AND: case IR_OR: case IR_XOR:
    case IR_SHL: case IR_SHR: case IR_CMP: case IR_NEG: case IR_BNOT:
        return 1;
    /* A global's or a string's address is not that cheap. It is two
     * instructions on Thumb (movw/movt), RISC-V (auipc/addi) and aarch64
     * (adrp/add), and a seven-byte lea on x86-64. A function that names
     * the same global in three blocks rebuilt it in each: the hash
     * table's insert, `hused` three times. Measured 2026-10-04: the hash
     * kernel ran 2.4-2.6% fewer instructions on RV32, M4 and aarch64 and
     * 1.2% fewer on x86-64, nothing else moved, and code over lib/libc
     * and the workload shrank 0.2-0.6% on all four. AVR, whose few
     * registers make a long live range dearer, came out even and is left
     * as it was. EMBCC_NO_GCSE_ADDR=1 turns it off, for bisecting. */
    case IR_GADDR: case IR_STRADDR:
        return target_get() != TARGET_AVR && !getenv("EMBCC_NO_GCSE_ADDR");
    default:
        return 0;
    }
}

/* Key a constant operand by its VALUE.
 *
 * IR_CONST is deliberately not numbered by this pass -- rematerialising
 * a literal costs less than keeping one live across a dominated region
 * -- so two blocks that each need `-2` hold it in two different temps.
 * Keyed by temp, `x & -2` in one block and `x & -2` in another were two
 * different values and never matched, which was very nearly ALL of what
 * this pass was missing: over lib/libc and lib/libcxx it found 3
 * redundant expressions, and 97 once an operand could be a literal.
 *
 * What gets kept live is still only the RESULT -- the arithmetic the
 * policy above calls the profitable case. The constant temp is left
 * where it was and dies with the instruction that read it. */
static void gcse_key_consts(struct ir_func *fn, struct defs *d, struct vn *k)
{
    long v;
    if (k->a >= 0 && get_const(fn, d, k->a, &v))
        { k->a = -1; k->ca = v; k->has_ca = 1; }
    if (k->b >= 0 && get_const(fn, d, k->b, &v))
        { k->b = -1; k->cb = v; k->has_cb = 1; }
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
        free_cfg(bb, nbb); return 0;
    }
    compute_idom(bb, order, norder);
    struct defs dfs;
    compute_defs(fn, &dfs);

    /* An active table holding the current block's and its dominators' values,
     * pushed on enter and truncated back on leave — an explicit dom-tree DFS so
     * siblings never see each other's values (they do not dominate each other).
     * A key is entered only when it is not there, so the table holds each
     * once and is hashed (vntab); leaving a block unlinks what it entered,
     * newest first. */
    struct vntab tb;
    vntab_init(&tb, fn->nins, fn->nvregs, 0);
    int changed = 0;
    int *dstk = xmalloc((size_t)nbb * sizeof *dstk);
    int *mark = xmalloc((size_t)nbb * sizeof *mark);
    char *entered = xcalloc((size_t)nbb, 1);
    /* Each block's dominator-tree children, in block order: finding them
     * by asking every block for its idom was blocks x blocks, and a
     * switch of 4000 cases is 8000 blocks. Pushed in that order, so the
     * walk visits them in the order it always has. */
    int *kid = xmalloc((size_t)(nbb + 1) * sizeof *kid);
    int *kids = xmalloc((size_t)(nbb ? nbb : 1) * sizeof *kids);
    for (int b = 0; b <= nbb; b++) kid[b] = 0;
    for (int c = 1; c < nbb; c++)
        if (bb[c].idom >= 0 && bb[c].idom < nbb) kid[bb[c].idom + 1]++;
    for (int b = 0; b < nbb; b++) kid[b + 1] += kid[b];
    {
        int *fill = xmalloc((size_t)(nbb ? nbb : 1) * sizeof *fill);
        for (int b = 0; b < nbb; b++) fill[b] = kid[b];
        for (int c = 1; c < nbb; c++)
            if (bb[c].idom >= 0 && bb[c].idom < nbb)
                kids[fill[bb[c].idom]++] = c;
        free(fill);
    }
    int dsp = 0; dstk[dsp++] = 0;
    while (dsp) {
        int b = dstk[dsp - 1];
        if (!entered[b]) {
            entered[b] = 1;
            mark[b] = tb.ne;
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
                gcse_key_consts(fn, &dfs, &k);
                int hit = vntab_find(&tb, &k);
                if (hit >= 0 && hit != i->dst) {
                    vn_to_mov(i, hit); changed = 1; g_did.gcse++;
                } else if (hit < 0) {
                    k.result = i->dst;
                    vntab_add(&tb, &k);
                }
            }
            for (int q = kid[b]; q < kid[b + 1]; q++)
                if (!entered[kids[q]]) dstk[dsp++] = kids[q];
        } else {
            /* leaving b: drop its (and its subtree's) values */
            while (tb.ne > mark[b])
                vntab_unlink(&tb, --tb.ne);
            dsp--;
        }
    }
    vntab_free(&tb); free(kid); free(kids);
    free(dstk); free(mark); free(entered);
    free_defs(&dfs);
    free(order); free(l2b);
    free_cfg(bb, nbb);
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

/* ==== which bytes of the object ============================================
 *
 * may_alias answers by OBJECT, and one object is where most of the
 * traffic is. A function handed `Queue_t *q` reads and writes q's fields
 * through that one pointer, so every store is to the same unknown object
 * as every load, and each store to one field made the optimizer forget
 * every other field and load it again: FreeRTOS's xQueueGenericReset
 * read pxQueue->pcHead a second time after storing pxQueue->pcWriteTo,
 * which clang does not.
 *
 * The finer question is which BYTES. An address that is a base value
 * plus a constant names a range of bytes from that value:
 *
 *      %8 = add %0, #60          (%0, 60, 4 bytes)
 *      %9 = load.4 [%8]
 *      %3 = add %0, #4           (%0, 4, 4 bytes)
 *      store:4 [%3], %15
 *
 * and two ranges from the SAME value that do not overlap are different
 * bytes, whatever the value is. Nothing else is assumed. Not the type of
 * either access: EmbCC does not do type-based aliasing, because the
 * kernels it compiles pun through casts. And not that pointer arithmetic
 * stays inside its object: the offsets are kept modulo the address width,
 * exactly as the machine adds them, so a negative offset, or one that
 * wraps, is compared as the bytes it really reaches (-fwrapv).
 *
 * "The same value" is what needs care, because a vreg is a name, not a
 * value. The base must be a temp written once, or a parameter never
 * reassigned -- and such a temp's definition dominates its uses, which
 * makes the name good enough: on any path, the address temps of a load
 * and of a later store were both computed after the base's LAST
 * definition, so they were computed from the same value of it. A load
 * whose address predates that definition is not remembered there at all:
 * load CSE's meet forgets it at the top of the block that defines the
 * base (the path from the entry into that block never loaded it), and
 * value numbering never carries anything across a block.
 *
 * The address of a global or of a frame slot is the same value wherever
 * it is computed, so the walk keys `gaddr @g` by the symbol and `addr v3`
 * by the slot, whichever temps hold them. Anything the walk cannot see
 * through -- a load, a call's result, an add of a variable -- is a base of
 * its own. Two different bases say nothing about each other, and the
 * question goes back to may_alias. */

enum { BASE_NONE, BASE_VREG, BASE_GLOBAL, BASE_SLOT };

struct maccess {
    struct memref obj;       /* the object, for may_alias */
    int bkind, bid;          /* the base value; BASE_NONE: not known */
    unsigned long off;       /* bytes past the base, modulo addr_mask */
    int size;                /* bytes accessed; 0: not known */
};

/* The modulus addresses are computed in: the target's pointer width,
 * or the host's unsigned long when that is narrower. Narrower is still
 * sound for disjointness -- two byte addresses equal modulo 2^64 are
 * equal modulo 2^32 too, so ranges disjoint at 32 bits are disjoint at
 * 64 -- but not for containment, which pass_dse asks only when the
 * modulus is exact. */
static unsigned long addr_mask(int *exact)
{
    int bits = 8 * PTRW;
    if (bits <= 0 || bits >= (int)(8 * sizeof(unsigned long))) {
        *exact = bits == (int)(8 * sizeof(unsigned long));
        return ~0UL;
    }
    *exact = 1;
    return (1UL << bits) - 1;
}

/* A vreg that names one value for the whole function: a temp written
 * once, or a parameter nothing reassigns. A local's slot is not one --
 * its vreg number is read through IR_LDVAR, not as a value. */
static int one_value(const struct ir_func *fn, const struct defs *d, int v)
{
    return v >= 0 && v < fn->nvregs && d->cnt[v] == 1 &&
           (v < fn->nparams || v >= fn->nvars);
}

/* The value of `v` when it is a constant of width `w`, the width of the
 * add that uses it: a constant of another width leaves the bits the add
 * reads above it to the backend, and they are not guessed here. */
static int addr_const(const struct ir_func *fn, const struct defs *d, int v,
                      int w, long *out)
{
    if (!one_value(fn, d, v) || d->ins[v] < 0)
        return 0;
    const struct ir_ins *c = &fn->ins[d->ins[v]];
    if (c->op != IR_CONST || c->flt || c->w != w)
        return 0;
    *out = c->imm;
    return 1;
}

/* Base, offset and extent of the `size` bytes at address temp `addr`.
 * The walk goes back through copies and through adds and subtracts of a
 * constant at the address width, while each step is one value; where it
 * stops is the base. The walk from any temp is the same walk, so two
 * addresses built on one base -- at whatever depth -- end at that base
 * with their offsets from it, unless the hop limit cuts one short, which
 * only makes the bases differ and the answer conservative. */
static struct maccess mem_access(struct ir_func *fn, struct defs *d,
                                 int addr, int size)
{
    struct maccess m;
    m.obj = mem_base(fn, d, addr);
    m.bkind = BASE_NONE; m.bid = -1; m.off = 0;
    m.size = size > 0 ? size : 0;
    int exact;
    unsigned long mask = addr_mask(&exact), off = 0;
    if (!one_value(fn, d, addr))
        return m;
    for (int hop = 0; hop < 16; hop++) {
        int n = d->ins[addr], next = -1;
        if (n >= 0) {
            const struct ir_ins *i = &fn->ins[n];
            long c = 0;
            if (i->op == IR_GADDR && i->glob_sym >= 0) {
                m.bkind = BASE_GLOBAL; m.bid = i->glob_sym;
                m.off = off & mask;
                return m;
            }
            if (i->op == IR_ADDR) {
                m.bkind = BASE_SLOT; m.bid = i->a;
                m.off = off & mask;
                return m;
            }
            if (i->op == IR_MOV) {
                next = i->a;
            } else if ((i->op == IR_ADD || i->op == IR_SUB) && !i->flt &&
                       i->w == PTRW) {
                int other = -1;
                if (i->imm_b) {
                    c = i->imm; other = i->a;
                } else if (addr_const(fn, d, i->b, i->w, &c)) {
                    other = i->a;
                } else if (i->op == IR_ADD &&
                           addr_const(fn, d, i->a, i->w, &c)) {
                    other = i->b;
                }
                if (other >= 0 && one_value(fn, d, other)) {
                    off = i->op == IR_SUB ? off - (unsigned long)c
                                          : off + (unsigned long)c;
                    next = other;
                }
            }
        }
        if (next < 0 || !one_value(fn, d, next))
            break;                      /* `addr` is the base */
        addr = next;
    }
    m.bkind = BASE_VREG; m.bid = addr;
    m.off = off & mask;
    return m;
}

/* A frame slot as a whole, as IR_LDVAR and IR_STVAR reach it: the object
 * is known, the bytes are not described. */
static struct maccess slot_access(int var)
{
    struct maccess m;
    m.obj.kind = MEM_SLOT; m.obj.id = var;
    m.bkind = BASE_NONE; m.bid = -1; m.off = 0; m.size = 0;
    return m;
}

/* The object alone, for an access whose bytes are not described: a
 * memcpy, a memzero, a volatile store (whose existing rules stay as
 * they were). */
static struct maccess obj_access(struct ir_func *fn, struct defs *d, int addr)
{
    struct maccess m;
    m.obj = mem_base(fn, d, addr);
    m.bkind = BASE_NONE; m.bid = -1; m.off = 0; m.size = 0;
    return m;
}

static int same_base(struct maccess a, struct maccess b)
{
    return a.bkind != BASE_NONE && a.bkind == b.bkind && a.bid == b.bid &&
           a.size > 0 && b.size > 0;
}

/* Can these two accesses touch a common byte? From one base: only when
 * the ranges overlap on the ring of addresses -- b starts fewer than
 * a.size bytes past a, or a fewer than b.size bytes past b, modulo the
 * address width. Otherwise by object. */
static int acc_overlap(struct maccess a, struct maccess b, const char *taken,
                       int nvars)
{
    if (same_base(a, b)) {
        int exact;
        unsigned long mask = addr_mask(&exact);
        unsigned long ab = (b.off - a.off) & mask;   /* b past a */
        unsigned long ba = (a.off - b.off) & mask;   /* a past b */
        return ab < (unsigned long)a.size || ba < (unsigned long)b.size;
    }
    return may_alias(a.obj, b.obj, taken, nvars);
}

/* Does `later` write every byte `early` does? Only from one base, and
 * only when the modulus is the machine's own: this is the must-alias
 * answer dead-store elimination deletes a write on. */
static int acc_covers(struct maccess later, struct maccess early)
{
    if (!same_base(later, early))
        return 0;
    int exact;
    unsigned long mask = addr_mask(&exact);
    if (!exact)
        return 0;
    unsigned long d = (early.off - later.off) & mask;  /* early past later */
    return d < (unsigned long)later.size &&
           (unsigned long)later.size - d >= (unsigned long)early.size;
}

/* The slots whose address is taken anywhere in the function: the only
 * locals a pointer can reach (may_alias). Freed by the caller. */
static char *slots_taken(struct ir_func *fn)
{
    int nvars = fn->nvars;
    char *taken = xcalloc((size_t)(nvars ? nvars : 1), 1);
    for (int i = 0; i < fn->nins; i++)
        if (fn->ins[i].op == IR_ADDR && fn->ins[i].a >= 0 &&
            fn->ins[i].a < nvars)
            taken[fn->ins[i].a] = 1;
    return taken;
}

/* pass_lvn's memory kill. A store need not forget every remembered
 * read, only those it could reach: drop exactly those from the table and
 * return 1. Anything else that writes memory -- a call, a memcpy, an
 * atomic, a fence, asm, a vector store, any volatile write -- returns 0,
 * and the caller forgets every read as before (memver). Entries made
 * under an older memver can never match again and are dropped here
 * too. */
static int lvn_mem_kill(struct ir_func *fn, struct defs *d, const char *taken,
                        struct vntab *tb, const struct ir_ins *ins)
{
    struct maccess w;
    if (ins->vol)
        return 0;
    if (ins->op == IR_STORE)
        w = mem_access(fn, d, ins->a, ins->size);
    else if (ins->op == IR_STVAR && ins->dst >= 0 && ins->dst < fn->nvars)
        w = slot_access(ins->dst);
    else
        return 0;
    /* Only the loads keyed at the current version can be reached: an
     * older one's key will never be asked for again. */
    int j = 0;
    for (int x = 0; x < tb->nld; x++) {
        int ei = tb->ld[x];
        struct vnent *e = &tb->e[ei];
        if (!e->live)
            continue;
        struct maccess r = e->k.op == IR_LOAD
            ? mem_access(fn, d, e->k.a, e->k.size) : slot_access(e->k.a);
        if (acc_overlap(w, r, taken, fn->nvars)) {
            vntab_unlink(tb, ei);
            continue;
        }
        tb->ld[j++] = ei;
    }
    tb->nld = j;
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

static int g_opt_size;                  /* -Os, set by opt_run (below) */

/* Replace 32-bit `x / C` and `x % C` with a multiply and shifts. */
static int pass_divmagic(struct ir_func *fn)
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
    int pow2_only = target_ptr_size() < 8;
    if (pow2_only && g_opt_size) {
        struct ir_ins probe;
        memset(&probe, 0, sizeof probe);
        probe.op = IR_DIV; probe.w = 4; probe.sign = 1;
        if (!target_op_calls_helper(&probe))
            return 0;
    }
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
static int pass_divtest(struct ir_func *fn)
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
static int target_cheap_select(void)
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

static int pass_ifconv(struct ir_func *fn)
{
    int changed = 0, guard = 0;
    while (guard++ < 64 && (ifconv_one(fn) || ifconv_any(fn)))
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
/* Jump threading on a merged boolean: what `a && b` and `a || b` leave in
 * a condition.
 *
 *       %t = mov %c        ; the comparison, on one arm
 *       jmp L
 *       ...
 *       %t = const 0       ; the short-circuit, on the other
 *       jmp L
 *   L:  brz %t -> Lx       ; %t's only reader
 *       <Lafter>
 *
 * Each arm already knows where it is going: the constant one straight to
 * Lx or Lafter, the copying one on %c itself. So an arm becomes a jump
 * there, and the 0/1 is never built, merged and tested again -- 128 of
 * these in lib/libc, and on AVR each 0/1 was four bytes wide.
 *
 * Lafter is the label right after the branch, or the target of a jmp
 * there; when it is neither, one is inserted, and only then. The skipped
 * `jmp L` is left dead for cfgclean. A copying arm that FALLS into L is
 * left alone, since a branch there would leave %t unset on the way
 * through. */
/* Move fn->var_scope_lo/hi with a renumbering: newpos[n] is where old
 * instruction n now is, newpos[oldn] the new end (DCE's idiom). */
static void remap_scopes(struct ir_func *fn, const int *newpos, int oldn)
{
    if (!fn->var_scope_lo || !newpos)
        return;
    for (int v = 0; v < fn->nvars; v++) {
        int lo = fn->var_scope_lo[v], hi = fn->var_scope_hi[v];
        if (lo >= 0 && lo <= oldn) fn->var_scope_lo[v] = newpos[lo];
        if (hi >= 0 && hi <= oldn) fn->var_scope_hi[v] = newpos[hi];
    }
}

static int thread_arm(const struct ir_func *fn, int k, int t, int L,
                      const struct defs *d)
{
    const struct ir_ins *def = &fn->ins[k], *go = &fn->ins[k + 1];
    int jumps = go->op == IR_JMP && go->label == L;
    int falls = k + 1 < fn->nins && go->op == IR_LABEL && go->label == L;
    if (def->dst != t || def->flt || (!falls && !jumps))
        return 0;
    if (def->op == IR_CONST)
        return 1;
    if (def->op != IR_MOV || def->a < 0 || def->a >= fn->nvregs ||
        d->cnt[def->a] != 1 || d->ins[def->a] < 0)
        return 0;
    /* a copy of a constant is a constant arm (the merge temp is written
     * twice, so copy propagation leaves `%t = mov %k` where %k = const) */
    if (fn->ins[d->ins[def->a]].op == IR_CONST && !fn->ins[d->ins[def->a]].flt)
        return 1;
    return jumps && fn->ins[d->ins[def->a]].op == IR_CMP;
}

/* The constant an arm sets, when thread_arm accepted it as one. */
static int thread_arm_const(const struct ir_func *fn, const struct ir_ins *def,
                            const struct defs *d, long *v)
{
    if (def->op == IR_CONST) {
        *v = def->imm;
        return 1;
    }
    if (def->op == IR_MOV && def->a >= 0 && def->a < fn->nvregs &&
        d->cnt[def->a] == 1 && d->ins[def->a] >= 0 &&
        fn->ins[d->ins[def->a]].op == IR_CONST) {
        *v = fn->ins[d->ins[def->a]].imm;
        return 1;
    }
    return 0;
}

static int thread_site(const struct ir_func *fn, int n, const int *use)
{
    const struct ir_ins *lab = &fn->ins[n], *br = &fn->ins[n + 1];
    int t = br->a;
    return n + 2 < fn->nins && lab->op == IR_LABEL &&
           (br->op == IR_BRZ || br->op == IR_BRNZ) &&
           t >= fn->nvars && t < fn->nvregs && use[t] == 1 && br->w <= 8;
}

/* A branch on `%u = cmp ne %t, 0` is a branch on %t, and one on
 * `cmp eq %t, 0` the opposite branch on %t, when nothing else reads %u.
 * C writes the first wherever a truth value is compared with pdFALSE or
 * 0 -- FreeRTOS's `listLIST_IS_EMPTY(l) == pdFALSE` is a 1/0 merged from
 * two arms and then tested -- and the compare in between hid the merge
 * from the threading below. The compare is left for DCE.
 *
 * Unsigned, `%t < 1` and `%t <= 0` are `%t == 0`, and `%t >= 1` and
 * `%t > 0` are `%t != 0`: FreeRTOS asserts `uxIndexToNotify <
 * configTASK_NOTIFICATION_ARRAY_ENTRIES`, which is 1 by default, and the
 * compare-and-branch it made was a `cmp #1; blo` where a test of the
 * value is one cbz. */
static int branch_on_cmp0(struct ir_func *fn, const int *use,
                          const struct defs *d)
{
    int changed = 0;
    for (int n = 0; n + 1 < fn->nins; n++) {
        const struct ir_ins *c = &fn->ins[n];
        struct ir_ins *br = &fn->ins[n + 1];
        long k;
        int eq;
        if (c->op != IR_CMP || c->flt || c->w > 8 ||
            (br->op != IR_BRZ && br->op != IR_BRNZ) || br->a != c->dst ||
            c->dst < 0 || c->dst >= fn->nvregs || use[c->dst] != 1 ||
            c->a < 0 || c->a >= fn->nvregs)
            continue;
        if (c->imm_b)
            k = c->imm;
        else if (c->b >= 0 && c->b < fn->nvregs && d->cnt[c->b] == 1 &&
                 d->ins[c->b] >= 0 && fn->ins[d->ins[c->b]].op == IR_CONST)
            k = fn->ins[d->ins[c->b]].imm;
        else
            continue;
        if (k == 0 && (c->pred == B_EQ || c->pred == B_NE))
            eq = c->pred == B_EQ;
        else if (!c->sign && k == 1 && (c->pred == B_LT || c->pred == B_GE))
            eq = c->pred == B_LT;
        else if (!c->sign && k == 0 && (c->pred == B_LE || c->pred == B_GT))
            eq = c->pred == B_LE;
        else
            continue;
        br->a = c->a;
        br->w = c->w;
        if (eq)
            br->op = br->op == IR_BRZ ? IR_BRNZ : IR_BRZ;
        changed = 1;
    }
    return changed;
}

static int pass_thread(struct ir_func *fn)
{
    int changed = 0, nv = fn->nvregs;
    int *use = xcalloc((size_t)(nv ? nv : 1), sizeof *use);
    struct ucount uc = { use, nv };
    struct defs d;

    for (int n = 0; n < fn->nins; n++)
        each_read(&fn->ins[n], count_cb, &uc);
    compute_defs(fn, &d);
    /* the branch now reads %t, and the uses counted are stale: the next
     * round, after DCE, threads it */
    if (branch_on_cmp0(fn, use, &d)) {
        free(use);
        free(d.cnt); free(d.ins);
        return 1;
    }
    /* A label after each branch that will be threaded and has none. */
    {
        int need = 0;
        char *mark = xcalloc((size_t)fn->nins + 1, 1);
        for (int n = 0; n + 2 < fn->nins; n++) {
            const struct ir_ins *nx = &fn->ins[n + 2];
            if (!thread_site(fn, n, use) || nx->op == IR_LABEL ||
                nx->op == IR_JMP)
                continue;
            for (int k = 0; k + 1 < fn->nins; k++)
                if (thread_arm(fn, k, fn->ins[n + 1].a, fn->ins[n].label, &d)) {
                    mark[n + 1] = 1;
                    need = 1;
                    break;
                }
        }
        if (need) {
            struct ibuf nb = { 0, 0, 0 };
            int *newpos = xmalloc((size_t)(fn->nins + 1) * sizeof *newpos);
            for (int n = 0; n < fn->nins; n++) {
                newpos[n] = nb.n;
                *ib_push(&nb) = fn->ins[n];
                if (mark[n]) {
                    struct ir_ins *l = ib_push(&nb);
                    memset(l, 0, sizeof *l);
                    l->op = IR_LABEL;
                    l->label = fn->nlabels++;
                    l->dst = l->a = l->b = -1;
                    l->line = fn->ins[n].line; l->col = fn->ins[n].col;
                    l->synth = 1;
                }
            }
            newpos[fn->nins] = nb.n;
            remap_scopes(fn, newpos, fn->nins);
            free(newpos);
            free(fn->ins);
            fn->ins = nb.p; fn->nins = nb.n; fn->cap = nb.cap;
            free(d.cnt); free(d.ins);
            compute_defs(fn, &d);
        }
        free(mark);
    }
    for (int n = 0; n + 2 < fn->nins; n++) {
        const struct ir_ins *lab = &fn->ins[n], *br = &fn->ins[n + 1];
        const struct ir_ins *nx = &fn->ins[n + 2];
        int t = br->a, L = lab->label, Lx = br->label, Lafter;
        if (!thread_site(fn, n, use))
            continue;
        if (nx->op == IR_LABEL)     Lafter = nx->label;
        else if (nx->op == IR_JMP)  Lafter = nx->label;
        else                        continue;
        for (int k = 0; k + 1 < fn->nins; k++) {
            struct ir_ins *def = &fn->ins[k], *go = &fn->ins[k + 1];
            long kv;
            if (!thread_arm(fn, k, t, L, &d))
                continue;
            if (thread_arm_const(fn, def, &d, &kv)) {
                unsigned long mask = br->w >= 8 ? ~0UL
                                   : (1UL << (8 * (br->w > 0 ? br->w : 4))) - 1;
                int zero = ((unsigned long)kv & mask) == 0;
                int taken = br->op == IR_BRZ ? zero : !zero;
                int line = def->line, col = def->col;
                memset(def, 0, sizeof *def);
                def->op = IR_JMP;
                def->label = taken ? Lx : Lafter;
                def->dst = def->a = def->b = -1;
                def->line = line; def->col = col;
                changed = 1;
            } else {                               /* a copied comparison */
                int c = def->a;
                def->op = br->op;
                def->a = c;
                def->b = -1;
                def->dst = -1;
                def->label = Lx;
                def->w = br->w;
                def->sign = br->sign;
                def->imm_b = 0;
                go->label = Lafter;
                changed = 1;
            }
        }
    }
    free(use);
    free(d.cnt); free(d.ins);
    return changed;
}

struct fwdctx { const int *fwd; int n, changed; };
static void fwd_cb(int *p, void *ctx)
{
    struct fwdctx *c = ctx;
    int l = *p;
    if (l < 0 || l >= c->n || c->fwd[l] < 0 || c->fwd[l] == l)
        return;
    *p = c->fwd[l];
    c->changed = 1;
}
struct reachctx { char *reached; int n; };
static void reach_cb(int *p, void *ctx)
{
    struct reachctx *c = ctx;
    if (*p >= 0 && *p < c->n) c->reached[*p] = 1;
}

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
    {
        struct fwdctx fc = { fwd, fn->nlabels, 0 };
        for (int n = 0; n < fn->nins; n++) {
            struct ir_ins *i = &fn->ins[n];
            if (i->op != IR_JMP && i->op != IR_BRZ && i->op != IR_BRNZ &&
                i->op != IR_SWITCH)
                continue;
            each_label(fn, i, fwd_cb, &fc);
        }
        if (fc.changed)
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

    /* A second branch on a condition the one above it already decided.
     * Control reaches it only by falling out of the first, so the
     * condition is known there: the same test cannot fire twice, and
     * the opposite test cannot fail. Nothing can jump between them --
     * there is no label -- which is the whole of the argument.
     *
     * Unrolling makes these on purpose: its entry test repeats the
     * loop's own guard, because the block it opens is the target of a
     * back edge and the guard runs once. Threading a jump makes them
     * too. */
    char *dead = xcalloc((size_t)fn->nins, 1);
    /* A branch on a temp the same block has just set to a constant.
     * pass_fold decides a rotated loop's guard, `i = 0; c = i < 24`, from
     * the block's own `i = 0` (lk_note), and value numbering then gives
     * the constant it made the name it already had in the block -- the
     * loop-carried i -- so the branch reads a temp with two definitions
     * and nothing else here can decide it. The same table decides it
     * here. */
    if (!getenv("EMBCC_NO_LKCONST")) {
        struct lkconst lk = {
            xcalloc((size_t)fn->nvregs, sizeof(int)),
            xmalloc((size_t)fn->nvregs * sizeof(long)),
            xmalloc((size_t)fn->nvregs * sizeof(int)), 1, fn->nvregs
        };
        /* Only for a name with more than one definition. One defined
         * once by a constant is SCCP's to decide, and SCCP says so in a
         * remark (`if (DEBUG)` with `const int DEBUG = 0`: the branch
         * always goes one way, which is as often a bug as an
         * optimization). Deciding it here first took that remark away. */
        struct defs dd;
        compute_defs(fn, &dd);
        for (int n = 0; n < fn->nins; n++) {
            if (n > 0)
                lk_note(&lk, &fn->ins[n - 1]);
            struct ir_ins *i = &fn->ins[n];
            long v;
            if ((i->op != IR_BRZ && i->op != IR_BRNZ) ||
                !lk_get(&lk, i->a, i->w, &v))
                continue;
            if (i->a >= 0 && i->a < fn->nvregs && dd.cnt[i->a] == 1 &&
                dd.ins[i->a] >= 0 && fn->ins[dd.ins[i->a]].op == IR_CONST)
                continue;
            if ((i->op == IR_BRZ) == (norm(v, i->w) == 0)) {
                i->op = IR_JMP; i->a = -1;      /* it is always taken */
            } else {
                dead[n] = 1;                    /* it is never taken */
            }
            changed = 1;
        }
        free(lk.gen_of); free(lk.val); free(lk.w);
        free_defs(&dd);
    }
    {
        int last_cond = -1;
        enum ir_op last_op = IR_JMP;
        for (int n = 0; n < fn->nins; n++) {
            struct ir_ins *i = &fn->ins[n];
            if (i->op == IR_LABEL) { last_cond = -1; continue; }
            if (dead[n])
                continue;                       /* decided above */
            int t = def_target(i);
            if (t >= 0 && t == last_cond)
                last_cond = -1;                 /* a different value now */
            if (i->op != IR_BRZ && i->op != IR_BRNZ)
                continue;
            if (last_cond >= 0 && i->a == last_cond) {
                if (i->op == last_op) {
                    dead[n] = 1;                /* it cannot fire */
                    changed = 1;
                    continue;                   /* and decides nothing */
                }
                i->op = IR_JMP; i->a = -1;      /* it cannot fail */
                changed = 1;
                last_cond = -1;
                continue;
            }
            last_cond = i->a; last_op = i->op;
        }
    }

    /* A label nothing can jump to is not a block boundary, it is only
     * pretending to be one -- and every block-local pass stops at it:
     * value numbering, copy propagation, store forwarding and dead-
     * store elimination all reason between labels, so one that no
     * branch names splits a straight line into two for nothing. The
     * loop passes leave these behind by the handful (rotation's old
     * header, a latch that was once its own block), and threading a
     * jump above makes more of them on the spot.
     *
     * A landing pad's label IS named, by the exception region rather
     * than by a branch, and the unwinder is what jumps to it. */
    /* A conditional branch AROUND an unconditional one:
     *
     *      brz  c, L1              brnz c, L2
     *      jmp  L2         ==>
     *   L1:                     L1:
     *
     * is one branch on the opposite sense. irgen writes the first shape
     * for every `?:` and `if` whose arm ends in a jump, and on a machine
     * with conditional branches it cost a branch per test. The inversion
     * is exact on the IR's 0/1 value -- brz and brnz are each other's
     * complement whatever produced it, NaN compares included. The jump
     * is left aimed at L1, where the rule below drops it. */
    for (int n = 0; n + 2 < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n], *j = &fn->ins[n + 1];
        /* Not a branch the rule above already condemned: it is only
         * MARKED dead, and removed at the end -- inverted first, the
         * removal took the jump's target with it. `brnz c; brnz c -> L9;
         * jmp L4; L9:` lost its way to L4 (a switch's case 0, at -Os,
         * where switches are compare trees) and SCCP then deleted the
         * case as unreachable. */
        if (dead[n] || dead[n + 1])
            continue;
        if ((i->op != IR_BRZ && i->op != IR_BRNZ) || j->op != IR_JMP ||
            j->label == i->label)
            continue;
        int m = n + 2, hit = 0;
        while (m < fn->nins && fn->ins[m].op == IR_LABEL)
            if (fn->ins[m++].label == i->label) { hit = 1; break; }
        if (!hit)
            continue;
        {
            int l2 = j->label;       /* where the branch goes now */
            i->op = i->op == IR_BRZ ? IR_BRNZ : IR_BRZ;
            j->label = i->label;     /* a jump to the fall-through */
            i->label = l2;
        }
        changed = 1;
    }

    char *reached = xcalloc((size_t)(fn->nlabels ? fn->nlabels : 1), 1);
    {
        struct reachctx rc = { reached, fn->nlabels };
        for (int n = 0; n < fn->nins; n++)
            each_label(fn, &fn->ins[n], reach_cb, &rc);
    }
    for (int e = 0; e < fn->neh; e++)
        if (fn->eh[e].lp_label >= 0 && fn->eh[e].lp_label < fn->nlabels)
            reached[fn->eh[e].lp_label] = 1;

    /* Drop a jump to the label that immediately follows it, and any
     * instruction that can never be reached: after an unconditional
     * transfer, until the next label. */
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        if (i->op == IR_LABEL && i->label >= 0 && i->label < fn->nlabels &&
            !reached[i->label]) {
            dead[n] = 1;
            continue;
        }
        if (i->op == IR_JMP) {
            int m = n + 1;
            while (m < fn->nins && fn->ins[m].op == IR_LABEL) {
                if (fn->ins[m].label == i->label) { dead[n] = 1; break; }
                m++;
            }
        }
        if ((i->op == IR_JMP && !dead[n]) || i->op == IR_RET ||
            i->op == IR_UD2 || i->op == IR_SWITCH) {
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
    free(dead); free(reached); free(at); free(fwd);
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


/* ==== attribute inference =================================================== *
 *
 * A call is the most pessimistic thing in the IR. Load elimination
 * drops every cached value at one, dead-store elimination cannot look
 * past one, and the alias analysis has to answer "could be anything".
 * That is right for a call that might write through a pointer it was
 * handed, and wrong for `abs`, `strlen`, or any of the small helpers a
 * program is mostly made of.
 *
 * So: work out from the body what a function actually does. Two
 * questions, both answered conservatively -- "not known to be" rather
 * than "known not to be":
 *
 *   Does it write memory the caller can see? A store through a pointer
 *   it was given, a store to a global, a call to something that does.
 *   Writing its OWN locals does not count: they die with the frame.
 *
 *   Does it read any? Same set, for loads.
 *
 * A function that does neither is `const` in gcc's sense -- its result
 * depends only on its arguments. One that only reads is `pure`.
 *
 * Recursion and indirect calls are where this has to be careful. The
 * fixpoint starts by assuming every function is clean and retreats;
 * starting from "clean" and only ever removing the property is what
 * makes a recursive cycle converge to the truth rather than to an
 * optimistic lie, because a cycle that does something dirty has that
 * fact introduced by whichever member does it. An indirect call could
 * go anywhere, so it is dirty outright. */
static void infer_attrs(struct ir_unit *iu)
{
    int nf = iu->nfuncs;
    if (nf <= 0)
        return;
    /* Start clean and retreat. Not a weak function: the body here is a
     * default the link may replace with one that does anything. */
    for (int k = 0; k < nf; k++) {
        struct func *f = iu->funcs[k].src;
        if (!f) continue;
        f->inf_no_write = f->inf_no_read = f->has_defn && !f->absorbed &&
                                           !f->is_weak;
    }
    for (int round = 0, changed = 1; changed && round < 32; round++) {
        changed = 0;
        for (int k = 0; k < nf; k++) {
            struct ir_func *fn = &iu->funcs[k];
            struct func *f = fn->src;
            if (!f || !f->inf_no_write) {
                if (f && f->inf_no_read) { f->inf_no_read = 0; changed = 1; }
                if (!f || !f->inf_no_read) continue;
            }
            int w = 1, r = 1;
            for (int n = 0; n < fn->nins && (w || r); n++) {
                struct ir_ins *i = &fn->ins[n];
                switch (i->op) {
                case IR_STORE: case IR_MEMCPY: case IR_MEMZERO:
                case IR_XCHG: case IR_XADD: case IR_CMPXCHG: case IR_ARMW:
                case IR_CAS: case IR_CAS16: case IR_ASM: case IR_VA_START:
                case IR_FENCE: case IR_VSTORE:
                    w = r = 0;          /* reaches memory the caller shares */
                    break;
                case IR_LOAD: case IR_VLOAD:
                    r = 0;
                    break;
                case IR_STVAR:
                    /* a local, unless its address escaped -- and if it
                     * did, IR_ADDR below has already said so */
                    break;
                case IR_LDVAR:
                    break;
                case IR_ADDR:
                    /* the address of a local leaving the function is the
                     * caller being handed a way in */
                    break;
                case IR_GADDR:
                    /* a global's address alone is not an access */
                    break;
                case IR_CALL: {
                    struct func *t = i->indirect ? NULL : i->callee;
                    if (!t || !t->has_defn) { w = r = 0; break; }
                    if (!t->inf_no_write) w = 0;
                    if (!t->inf_no_read)  r = 0;
                    break;
                }
                default:
                    break;
                }
            }
            if (!w && f->inf_no_write) { f->inf_no_write = 0; changed = 1; }
            if (!r && f->inf_no_read)  { f->inf_no_read = 0;  changed = 1; }
        }
    }
    if (!remarks_on())
        return;
    for (int k = 0; k < nf; k++) {
        struct func *f = iu->funcs[k].src;
        if (!f || !f->has_defn || f->absorbed)
            continue;
        if (f->inf_no_read)
            remark_add("opt", "inferred", f->name, "attr/const",
                       f->file, f->line, "reads and writes no memory");
        else if (f->inf_no_write)
            remark_add("opt", "inferred", f->name, "attr/pure",
                       f->file, f->line, "writes no memory");
    }
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
 * Two stores kill each other only when the later one writes every byte
 * of the earlier: the SAME address temp at the same width, or the same
 * base value with the earlier's range inside the later's (acc_covers).
 * That is must-alias, not may-alias: a wrong answer here deletes a write
 * the program made. A read in between keeps the earlier store when it
 * may overlap it (acc_overlap), so a load of p->y does not keep alive a
 * store to p->x that p->x = 2 overwrites. */
static int pass_dse(struct ir_func *fn)
{
    if (fn->nins == 0)
        return 0;
    int nvars = fn->nvars;
    struct defs d;
    compute_defs(fn, &d);
    int nbb, *l2b;
    struct bb *bb = build_cfg(fn, &nbb, &l2b);

    char *taken = slots_taken(fn);

    char *dead = xcalloc((size_t)fn->nins, 1);
    /* Stores seen later in this block, as (address temp, width) and the
     * bytes they write. A slot store records its var with a negative
     * marker so the two kinds share one list. */
    int *sa = xmalloc((size_t)fn->nins * sizeof *sa);
    int *ssz = xmalloc((size_t)fn->nins * sizeof *ssz);
    struct maccess *sacc = xmalloc((size_t)fn->nins * sizeof *sacc);
    int changed = 0;

    for (int b = 0; b < nbb; b++) {
        int ns = 0;
        for (int i = bb[b].end - 1; i >= bb[b].start; i--) {
            struct ir_ins *ins = &fn->ins[i];
            if (ins->op == IR_STORE && !ins->vol && ins->a >= 0 &&
                ins->a < fn->nvregs && d.cnt[ins->a] == 1) {
                struct maccess w = mem_access(fn, &d, ins->a, ins->size);
                int killed = 0;
                for (int k = 0; k < ns; k++)
                    if ((sa[k] == ins->a && ssz[k] == ins->size) ||
                        acc_covers(sacc[k], w)) { killed = 1; break; }
                if (killed) { dead[i] = 1; changed = 1; continue; }
                sa[ns] = ins->a; ssz[ns] = ins->size; sacc[ns] = w; ns++;
                continue;
            }
            if (ins->op == IR_STVAR && !ins->vol && ins->dst >= 0 &&
                ins->dst < nvars && !taken[ins->dst]) {
                int killed = 0;
                for (int k = 0; k < ns; k++)
                    if (sa[k] == -1 - ins->dst && ssz[k] == ins->size) { killed = 1; break; }
                if (killed) { dead[i] = 1; changed = 1; continue; }
                sa[ns] = -1 - ins->dst; ssz[ns] = ins->size;
                sacc[ns] = slot_access(ins->dst); ns++;
                continue;
            }
            /* A read that could see one of them un-kills it. A call,
             * inline asm, an atomic or a fence could see anything. */
            struct maccess r = slot_access(-1);
            int reads = 0, everything = 0;
            switch (ins->op) {
            case IR_LOAD:
                r = mem_access(fn, &d, ins->a, ins->size); reads = 1; break;
            case IR_LDVAR:
                r = slot_access(ins->a); reads = 1; break;
            case IR_MEMCPY:
                r = obj_access(fn, &d, ins->b); reads = 1; break;
            case IR_STORE: case IR_STVAR:
                reads = 0; everything = 1; break;   /* volatile or unkeyed */
            case IR_CALL:
                /* A call that reads no memory the caller can see cannot
                 * observe a store, and one that writes none cannot be
                 * the reason to keep it. */
                if (!ins->indirect && ins->callee &&
                    ins->callee->inf_no_read && ins->callee->inf_no_write)
                    break;
                everything = 1; break;
            case IR_ASM: case IR_VA_START: case IR_FENCE:
            case IR_XCHG: case IR_XADD: case IR_CMPXCHG: case IR_ARMW:
            case IR_CAS: case IR_CAS16: case IR_MEMZERO: case IR_ALLOCA:
            case IR_VLOAD:          /* sixteen bytes nothing here describes */
                everything = 1; break;
            default:
                break;
            }
            if (everything) { ns = 0; continue; }
            if (!reads)
                continue;
            int j = 0;
            for (int k = 0; k < ns; k++)
                if (!acc_overlap(sacc[k], r, taken, nvars)) {
                    sa[j] = sa[k]; ssz[j] = ssz[k]; sacc[j] = sacc[k]; j++;
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

    free(dead); free(sa); free(ssz); free(sacc); free(taken); free(l2b);
    free_cfg(bb, nbb); free_defs(&d);
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
 *     cannot change between the two loads -- and it is keyed by where that
 *     address points, base value and offset (mem_access), so `p->len` read
 *     through two different `add %p, #16` temps is one key. That is what -O1,
 *     which has no global CSE to merge the two adds, needs;
 *   - the kill model separates a store to a non-address-taken local (kills only
 *     that local's LDVARs) from a real memory write. A plain store kills the
 *     keys whose bytes it may overlap (acc_overlap); a volatile store, a memcpy
 *     or a memzero the keys on an object it may alias; a call, asm, an atomic,
 *     a fence or a vector store every LOAD and every address-taken local's
 *     LDVAR.
 */
struct lkey {
    enum ir_op op;
    int a, size, sign, w;
    /* A LOAD whose address has a base (mem_access) is keyed by the base
     * and the offset, and `a` is -1; `rep` is one address temp of it,
     * for the object it reads. */
    int bkind, bid, rep;
    unsigned long off;
};

static int lkey_eq(const struct lkey *x, const struct lkey *y)
{
    return x->op == y->op && x->a == y->a && x->size == y->size &&
           x->sign == y->sign && x->w == y->w && x->bkind == y->bkind &&
           x->bid == y->bid && x->off == y->off;
}

static int lcse_kills_mem(enum ir_op op)
{
    switch (op) {
    case IR_STORE: case IR_CALL: case IR_MEMCPY: case IR_MEMZERO:
    case IR_XCHG: case IR_XADD: case IR_CMPXCHG: case IR_ARMW: case IR_CAS:
    case IR_CAS16: case IR_ASM: case IR_VA_START:
    case IR_FENCE:           /* a barrier: see writes_memory */
    /* A vectorized loop's stores. Missing here, a scalar load from
     * before such a loop was reused after it: `x = a[0]; for (...)
     * a[i] = b[i] + 1; return x + a[0];` returned 2 * x at -O2 on
     * x86-64, where the loop vectorizes. */
    case IR_VSTORE:
        return 1;
    default:
        return 0;
    }
}

/* Drop the cached loads a write could reach. A plain store names its
 * bytes, so only the keys that may overlap them go (acc_overlap): a
 * store to q->wr keeps q->len. A volatile store, a memcpy and a memzero
 * keep their older rule and name only an object. A call, inline asm, an
 * atomic, a fence or a vector store names nothing, so everything
 * reachable goes. */
static void lcse_kill(int *s, int nk, const char *is_mem,
                      const struct maccess *kacc, struct ir_ins *ins,
                      struct ir_func *fn, struct defs *d, const char *taken,
                      int nvars)
{
    struct maccess w = slot_access(-1);
    int named = 0;
    if (ins->op == IR_STORE && !ins->vol) {
        w = mem_access(fn, d, ins->a, ins->size);
        named = 1;      /* an unknown object still has its base */
    } else if (ins->op == IR_STORE || ins->op == IR_MEMCPY ||
               ins->op == IR_MEMZERO) {
        w = obj_access(fn, d, ins->a);
        named = w.obj.kind != MEM_UNKNOWN;
    } else if (ins->op == IR_CALL && !ins->indirect && ins->callee &&
               ins->callee->inf_no_write) {
        return;         /* it writes nothing the caller can see */
    }
    for (int k = 0; k < nk; k++) {
        if (!is_mem[k])
            continue;
        if (named && !acc_overlap(w, kacc[k], taken, nvars))
            continue;
        s[k] = -1;
    }
}

/* A store of v makes a later load of the same address read v: the key
 * becomes available with v as its value, as a load would have made it.
 * Only when nothing can change v meanwhile -- one definition: a temp
 * written once, or a parameter never reassigned -- and when the load
 * reads exactly what was stored: the same 4 or 8 bytes, no extension.
 * (A float stored and its bits loaded as an integer is the same value:
 * the forwarded copy ties the two to one register class, which is what
 * every backend's class analysis already does with a copy.) */
static void lcse_store_gen(int *s, int nk, const struct lkey *keys,
                           const struct ir_ins *ins,
                           struct ir_func *fn, struct defs *d)
{
    int v = ins->b;
    if (ins->vol || ins->imm_b || v < 0 || v >= fn->nvregs ||
        d->cnt[v] != 1 || (v >= fn->nparams && v < fn->nvars) ||
        (ins->size != 4 && ins->size != 8))
        return;
    struct maccess m = mem_access(fn, d, ins->a, ins->size);
    for (int k = 0; k < nk; k++) {
        if (keys[k].op != IR_LOAD || keys[k].size != ins->size ||
            keys[k].w != ins->size || keys[k].bkind != m.bkind)
            continue;
        if (m.bkind == BASE_NONE ? keys[k].a == ins->a
                                 : keys[k].bid == m.bid && keys[k].off == m.off)
            s[k] = v;
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
        free_cfg(bb, nbb); free_defs(&d); return 0;
    }

    char *taken = slots_taken(fn);

    /* Enumerate distinct load keys; keyidx[i] maps a load instruction to one. */
    struct lkey *keys = NULL; int nk = 0, capk = 0;
    int *keyidx = xmalloc((size_t)fn->nins * sizeof *keyidx);
    for (int i = 0; i < fn->nins; i++) {
        keyidx[i] = -1;
        struct ir_ins *in = &fn->ins[i];
        struct lkey k;
        memset(&k, 0, sizeof k);
        k.bkind = BASE_NONE; k.bid = -1;
        if (in->op == IR_LDVAR && !in->vol && in->a >= 0 && in->a < nvars) {
            k.op = IR_LDVAR;
        } else if (in->op == IR_LOAD && !in->vol && in->a >= 0 &&
                   in->a < fn->nvregs && d.cnt[in->a] == 1) {
            k.op = IR_LOAD;
        } else {
            continue;
        }
        k.a = k.rep = in->a; k.size = in->size; k.sign = in->sign; k.w = in->w;
        if (k.op == IR_LOAD) {
            struct maccess m = mem_access(fn, &d, in->a, in->size);
            if (m.bkind != BASE_NONE) {
                k.a = -1; k.bkind = m.bkind; k.bid = m.bid; k.off = m.off;
            }
        }
        int found = -1;
        for (int j = 0; j < nk; j++)
            if (lkey_eq(&keys[j], &k)) { found = j; break; }
        if (found < 0) {
            if (nk == capk) { capk = capk ? capk * 2 : 32;
                keys = xrealloc(keys, (size_t)capk * sizeof *keys); }
            keys[nk] = k; found = nk++;
        }
        keyidx[i] = found;
    }
    if (nk == 0) {
        free(order); free(l2b); free(taken); free(keyidx); free(keys);
        free_cfg(bb, nbb); free_defs(&d); return 0;
    }
    /* is_mem[k]: a LOAD or an address-taken local's LDVAR (killed by any write).
     * A non-address-taken local's LDVAR is killed only by a store to that local. */
    char *is_mem = xmalloc((size_t)nk);
    for (int k = 0; k < nk; k++)
        is_mem[k] = keys[k].op == IR_LOAD ||
                    (keys[k].op == IR_LDVAR && taken[keys[k].a]);

    /* What bytes each cached load reads, so a write can kill only the
     * ones it could actually reach. Without this a single store through
     * a pointer dropped every cached load in the function. */
    struct maccess *kacc = xmalloc((size_t)(nk ? nk : 1) * sizeof *kacc);
    for (int k = 0; k < nk; k++) {
        if (keys[k].op == IR_LDVAR)
            kacc[k] = slot_access(keys[k].a);
        else
            kacc[k] = mem_access(fn, &d, keys[k].rep, keys[k].size);
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
                    lcse_kill(s, nk, is_mem, kacc, ins, fn, &d, taken, nvars);
                    if (ins->op == IR_STORE)
                        lcse_store_gen(s, nk, keys, ins, fn, &d);
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
                lcse_kill(s, nk, is_mem, kacc, ins, fn, &d, taken, nvars);
                if (ins->op == IR_STORE)
                    lcse_store_gen(s, nk, keys, ins, fn, &d);
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
    free(is_mem); free(kacc); free(aout); free(ain); free(s);
    free_cfg(bb, nbb); free_defs(&d);
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

/* The table is cleared at every block and every definition clears the
 * entries copied FROM what it defines, and both used to be a sweep over
 * every vreg: blocks x vregs plus definitions x vregs, which on one
 * function of 4000 plain statements was most of an -O2 compile once
 * pass_copyprop stopped being. So each entry is also filed under its
 * source (`by`), and a block's entries are remembered in the order they
 * were made, to be undone one by one. A filing goes stale when its entry
 * is cleared or replaced; it is checked against cp[] when used, not
 * removed. */
struct lcfile { int v, src, next; };
struct lctab { int *cp, *by; struct lcfile *f; int nf, capf; };

static void lctab_reset(struct lctab *t)
{
    for (int k = 0; k < t->nf; k++) {
        t->cp[t->f[k].v] = -1;
        t->by[t->f[k].src] = -1;
    }
    t->nf = 0;
}

static void lctab_set(struct lctab *t, int v, int src)
{
    if (t->nf == t->capf) {
        t->capf = t->capf ? t->capf * 2 : 64;
        t->f = xrealloc(t->f, (size_t)t->capf * sizeof *t->f);
    }
    t->f[t->nf].v = v;
    t->f[t->nf].src = src;
    t->f[t->nf].next = t->by[src];
    t->by[src] = t->nf++;
    t->cp[v] = src;
}

static int pass_copyprop_local(struct ir_func *fn)
{
    if (fn->nins == 0 || fn->nvregs == 0)
        return 0;
    int nbb, *l2b;
    struct bb *bb = build_cfg(fn, &nbb, &l2b);
    struct defs d;
    compute_defs(fn, &d);
    struct lctab tb = { NULL, NULL, NULL, 0, 0 };
    tb.cp = xmalloc((size_t)fn->nvregs * sizeof *tb.cp);
    tb.by = xmalloc((size_t)fn->nvregs * sizeof *tb.by);
    for (int v = 0; v < fn->nvregs; v++)
        tb.cp[v] = tb.by[v] = -1;
    int *cp = tb.cp;
    int changed = 0;
    for (int b = 0; b < nbb; b++) {
        lctab_reset(&tb);
        for (int n = bb[b].start; n < bb[b].end; n++) {
            struct ir_ins *i = &fn->ins[n];
            /* Inline asm names its OUTPUT temps through each_read, and a
             * landing pad writes two temps that def_target cannot both
             * report. Neither is worth modelling for a copy table: drop
             * it and carry on from here. */
            if (i->op == IR_ASM || i->op == IR_LANDING) {
                lctab_reset(&tb);
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
                for (int k = tb.by[t]; k >= 0; k = tb.f[k].next)
                    if (cp[tb.f[k].v] == t)  /* and so is anything copied
                                              * from what it just replaced */
                        cp[tb.f[k].v] = -1;
                tb.by[t] = -1;
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
                lctab_set(&tb, i->dst, cp[i->a] >= 0 ? cp[i->a] : i->a);
        }
    }
    free_defs(&d);
    free(tb.cp); free(tb.by); free(tb.f); free(l2b);
    free_cfg(bb, nbb);
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
    if (b == s)
        return 1;
    /* s is an ancestor of b in the dominator tree (compute_idom's
     * numbering). Off the tree -- unreachable, or no dominators
     * computed -- nothing dominates but the block itself. */
    if (bb[b].dpre < 0 || bb[s].dpre < 0)
        return 0;
    return bb[s].dpre <= bb[b].dpre && bb[b].dpost <= bb[s].dpost;
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

/* ==== partial redundancy elimination (-O2) ================================= *
 *
 * Local value numbering reuses a value computed earlier in the same
 * block. Global CSE reuses one computed in a block that DOMINATES this
 * one. Between them they answer "was this computed on EVERY path to
 * here", and there is a third case neither can see:
 *
 *      if (c) sink(a * b);       <- computed on one path
 *      return a * b;             <- and again here, on both
 *
 * The second multiply is redundant when `c` was true and new when it was
 * false, so it is PARTIALLY redundant: no single earlier computation
 * dominates it, and both earlier passes decline. Compute it on the path
 * that lacked it and the partial redundancy becomes a full one; the
 * bottom copy is then a move.
 *
 * ---- why this is not speculation ------------------------------------
 *
 * Inserting a computation on a path that would not have performed it is
 * work invented, and the only thing that stops that is WHERE the
 * insertion goes: at the end of a predecessor whose sole successor is
 * the redundant block. Every execution reaching that point reaches the
 * block, so the expression runs exactly as often after as before -- once
 * per path instead of once on some and twice on others. The count never
 * goes up on any path, including a path through a loop, which is why
 * this needs no loop reasoning at all.
 *
 * A predecessor with two successors gives no such point: anything put
 * before its branch runs for the other successor too. That edge is
 * CRITICAL, and it is also the shape the example above has -- `if (c)`
 * branches straight to the merge. Refusing it would mean refusing very
 * nearly every real opportunity, so instead the edge is SPLIT: the
 * branch is retargeted to a new block that holds the insertion and jumps
 * on to the original. The new block is on exactly one edge, so the
 * invariant above is restored rather than bent.
 *
 * ---- and the half that pays for itself -------------------------------
 *
 * The result of an insertion is a vreg written in more than one
 * predecessor -- a phi in all but name, and the same shape irgen
 * already emits for a `?:`. Copy propagation declines to touch a
 * multiply-assigned vreg (it checks), so those moves reach the register
 * allocator to be coalesced.
 *
 * It also means the value is now held under ONE name on every path
 * while being dominated by no single definition -- so global CSE, which
 * asks about dominance, cannot see it, and the insertion would buy
 * nothing past the block it was made for. The fix is to ask the weaker
 * question the analysis here already answers: has every path to this
 * point passed a definition, under one name? Availability implies
 * dominance for a value with one definition, so this strictly contains
 * what global CSE does, and it is what makes the merged name usable
 * downstream.
 *
 * The two halves are one mechanism, not two features: the reuse exists
 * to make an insertion worth making, and on its own it can only find
 * what global CSE already finds. "What is worth inserting, and what
 * this is worth" below has the measurement, including the part where
 * that turns out to be nothing at all on lib/libc.
 *
 * ---- what may move ---------------------------------------------------
 *
 * The expressions global CSE numbers, minus divide and remainder, whose
 * fault on a zero divisor is a path-dependent EFFECT and not a value.
 * Every operand must be single-assignment (`vn_stable`, for the reason
 * it guards CSE), so a value never stops being its value and
 * availability has nothing to kill -- a plain forward must-analysis.
 *
 * An operand also has to exist where the copy goes:
 *
 *   - a literal is rematerialised there, one instruction, no question;
 *   - a read of a slot that is never written and never addressed has
 *     one value for the whole function, so it too is rematerialised.
 *     This case is not a detail: parameters stay in their slots here
 *     (mem2reg skips them -- their value is live on entry, so SSA has
 *     no version to seed a read with), so `a * b` on two paths is two
 *     multiplies of two DIFFERENT pairs of temps. Without recognising
 *     that both pairs read the same unwritten slots, the example at the
 *     top of this comment does not match itself;
 *   - anything else must be defined in a block that dominates the
 *     predecessor, which is what makes reading it there legal.
 */

/* Where a new instruction goes at the end of block b: before its
 * terminator, which has to stay last. */
static int pre_tail(struct ir_func *fn, struct bb *bb, int b)
{
    int e = bb[b].end;
    if (e > bb[b].start) {
        enum ir_op op = fn->ins[e - 1].op;
        if (op == IR_JMP || op == IR_BRZ || op == IR_BRNZ ||
            op == IR_RET || op == IR_UD2 || op == IR_IGOTO ||
            op == IR_SWITCH)
            return e - 1;
    }
    return e;
}

/* Slots whose contents cannot change: never written, never addressed,
 * not volatile, every read plain and full width. A parameter that is
 * only ever read is the overwhelmingly common case. */
static char *pre_invariant_slots(struct ir_func *fn)
{
    int n = fn->nvars;
    char *inv = xmalloc((size_t)(n ? n : 1));
    for (int L = 0; L < n; L++)
        inv[L] = !fn->locals[L].is_volatile;
    for (int i = 0; i < fn->nins; i++) {
        struct ir_ins *in = &fn->ins[i];
        if ((in->op == IR_ADDR || in->op == IR_LDVAR) &&
            in->a >= 0 && in->a < n) {
            if (in->op == IR_ADDR || in->vol)
                inv[in->a] = 0;
        }
        if (in->op == IR_STVAR && in->dst >= 0 && in->dst < n)
            inv[in->dst] = 0;
    }
    return inv;
}

/* The instruction defining v, when v has exactly one definition and it
 * is a rematerialisable read of an unwritten slot. */
static struct ir_ins *pre_slot_read(struct ir_func *fn, struct defs *d,
                                    const char *inv, int v)
{
    if (v < 0 || v >= fn->nvregs || d->cnt[v] != 1)
        return NULL;
    int def = d->ins[v];
    if (def < 0)
        return NULL;
    struct ir_ins *in = &fn->ins[def];
    if (in->op != IR_LDVAR || in->vol ||
        in->a < 0 || in->a >= fn->nvars || !inv[in->a])
        return NULL;
    return in;
}

/* Is vreg v readable at the end of block p? A parameter's vreg is live
 * throughout; anything else needs its defining block to dominate p, and
 * if that block IS p, the definition must precede the insertion. */
static int pre_have(struct ir_func *fn, struct defs *d, struct bb *bb,
                    int nbb, const int *i2b, int v, int p, int at)
{
    if (v < 0)
        return 1;                       /* absent operand */
    if (v >= fn->nvregs || d->cnt[v] != 1)
        return 0;
    int def = d->ins[v];
    if (def < 0)
        return 1;                       /* live on entry */
    int db = i2b[def];
    if (db < 0 || db >= nbb)
        return 0;
    if (db == p)
        return def < at;
    return bb_dominates(bb, db, p);
}

/* ---- what is worth inserting, and what this is worth --------------
 *
 * Reusing a value costs nothing: an instruction becomes a move, and
 * copy propagation usually deletes even that. INSERTING is not free. It
 * costs a move in every predecessor that already held the value, plus
 * the one at the use, and buys back the whole expression on the paths
 * that recomputed it.
 *
 * Measured over the 84 files of lib/libc and lib/libcxx at -O2, .text
 * summed per target, against this same compiler with `-fno-pre`
 * (x86-64 247583, aarch64 444044):
 *
 *     insert nothing, reuse only        247583   444044
 *     insert multiplies  (what ships)   247583   444044
 *     ... or shifts and compares too    247613   444056
 *     insert every numberable op        247658   444204
 *
 * Read the first two rows carefully, because they say this pass does
 * not change one byte of that corpus, and the reason is worth more than
 * the result:
 *
 *   - no multiply insertion fires anywhere in it. The shape at the top
 *     of this comment is real, but it is not what libc is made of;
 *   - and with no insertion, the reuse half cannot beat global CSE.
 *     That is not luck, it is a theorem: with retreating edges dropped,
 *     an available name that has ONE definition is available only
 *     because every path passed that definition, which is what it means
 *     for it to dominate. Availability beats dominance exactly when the
 *     name has SEVERAL definitions, and the only thing that makes one
 *     is an insertion.
 *
 * So the 59 computations it removes there are 59 that global CSE would
 * have taken on the next round -- the final IR is instruction-for-
 * instruction identical either way, which is how this was confirmed
 * rather than assumed. Compile time over the same corpus is 1.30s
 * against 1.31s: free.
 *
 * It is kept, on at -O2, for the case it was built for and the tests
 * hold it to. It is restricted to multiplies so that where it does not
 * apply it cannot cost anything -- the two rows below the fold are what
 * a wider rule does, on both targets. A shift or a compare is one
 * instruction, so trading it for a move is a wash the longer live range
 * then loses.
 */
static int pre_worth_it(const struct ir_ins *i)
{
    return i->op == IR_MUL;
}

/* Which temp holds each key as a block is walked, and the inverse, so
 * that a write can cheaply un-name whatever the vreg used to hold.
 *
 * The inverse is not a convenience. A vreg written on two paths -- the
 * shape irgen emits for `?:` and for the va_arg result -- can carry a
 * key down one path and something else down the other, and a table that
 * only ever RECORDS names goes on believing the first long after the
 * second overwrote it. That is a wrong answer, not a missed one.
 *
 * `stamp` dates each entry so the table costs nothing to clear between
 * blocks: an entry from an older generation simply does not exist. */
struct pretab { int *st, *own, *stamp; int gen, nk, nv; };

static void pre_name(struct pretab *t, int k, int v)
{
    t->st[k] = v;
    if (v >= 0 && v < t->nv) { t->own[v] = k; t->stamp[v] = t->gen; }
}

/* Start a block from the availability computed for its entry. */
static void pre_enter(struct pretab *t, const int *in)
{
    t->gen++;
    for (int k = 0; k < t->nk; k++) { t->st[k] = -1; }
    for (int k = 0; k < t->nk; k++) if (in[k] >= 0) pre_name(t, k, in[k]);
}

/* One instruction's effect. Whatever it writes stops holding what it
 * held; a numbered instruction then names its key if nothing already
 * held it, and a MOV renames the key it copies -- that one overwrites,
 * because the new name is the one the other paths will agree on. */
static void pre_step(struct ir_func *fn, const int *keyidx, int i,
                     struct pretab *t)
{
    struct ir_ins *in = &fn->ins[i];
    int dst = in->dst, src = -1;
    if (dst >= 0 && dst < t->nv && t->stamp[dst] == t->gen) {
        int k0 = t->own[dst];
        if (k0 >= 0 && t->st[k0] == dst) t->st[k0] = -1;
        t->stamp[dst] = 0;                      /* names nothing now */
    }
    int k = keyidx[i];
    if (k >= 0) {
        if (t->st[k] < 0) pre_name(t, k, dst);
        return;
    }
    if (in->op == IR_MOV && dst >= 0 && (src = in->a) >= 0 && src < t->nv &&
        t->stamp[src] == t->gen && t->own[src] >= 0)
        pre_name(t, t->own[src], dst);
}

#define PRE_MAX_KEYS 400      /* the dataflow is blocks x keys */

static int pre_one(struct ir_func *fn)
{
    if (fn->nins == 0 || fn->nlabels == 0)
        return 0;
    int nbb, *l2b;
    struct bb *bb = build_cfg(fn, &nbb, &l2b);
    int *order = xmalloc((size_t)(nbb ? nbb : 1) * sizeof *order), norder;
    compute_rpo(bb, nbb, order, &norder);
    if (norder != nbb) {        /* unreachable blocks: dominance is partial */
        free(order); free(l2b); free_cfg(bb, nbb); return 0;
    }
    compute_idom(bb, order, norder);
    struct defs d;
    compute_defs(fn, &d);
    char *inv = pre_invariant_slots(fn);

    int *i2b = xmalloc((size_t)fn->nins * sizeof *i2b);
    for (int b = 0; b < nbb; b++)
        for (int i = bb[b].start; i < bb[b].end; i++)
            i2b[i] = b;

    /* An operand's identity. A read of an unwritten slot is keyed by the
     * SLOT, so the same read in two blocks -- two temps -- is one value. */
    int *pcan = xmalloc((size_t)(fn->nvregs ? fn->nvregs : 1) * sizeof *pcan);
    for (int v = 0; v < fn->nvregs; v++) {
        struct ir_ins *r = pre_slot_read(fn, &d, inv, v);
        pcan[v] = r ? -(1 << 20) - r->a : v;
    }

    /* The expressions worth numbering, and which one each instruction
     * computes. */
    struct vn *keys = NULL;
    int nk = 0, capk = 0;
    int *keyidx = xmalloc((size_t)fn->nins * sizeof *keyidx);
    for (int i = 0; i < fn->nins; i++) {
        keyidx[i] = -1;
        struct ir_ins *in = &fn->ins[i];
        struct vn k;
        /* Not a float operation. It would be sound (nothing here
         * speculates, and FENV_ACCESS is off), but allowing them changed
         * no function in lib/libc for Cortex-M, so the merge temps stay
         * as they were: written by integer operations and moves only. */
        if (in->dst < 0 || !gcse_numberable(in->op) || in->flt ||
            in->op == IR_DIV || in->op == IR_MOD ||
            !vn_stable(fn, &d, in) || !vn_key(in, 0, &k))
            continue;
        gcse_key_consts(fn, &d, &k);
        if (k.a >= 0) k.a = pcan[k.a];
        if (k.b >= 0) k.b = pcan[k.b];
        int found = -1;
        for (int j = 0; j < nk; j++)
            if (vn_eq(&keys[j], &k)) { found = j; break; }
        if (found < 0) {
            if (nk == PRE_MAX_KEYS)
                continue;
            if (nk == capk) {
                capk = capk ? capk * 2 : 32;
                keys = xrealloc(keys, (size_t)capk * sizeof *keys);
            }
            keys[nk] = k; found = nk++;
        }
        keyidx[i] = found;
    }
    if (nk == 0) {
        free(order); free(l2b); free(i2b); free(keyidx); free(keys);
        free(pcan); free(inv);
        free_cfg(bb, nbb); free_defs(&d); return 0;
    }

    /* Availability. Nothing kills an entry: every operand is
     * single-assignment, so a value computed stays that value. */
    int *aout = xmalloc((size_t)nbb * (size_t)nk * sizeof *aout);
    int *ain  = xmalloc((size_t)nbb * (size_t)nk * sizeof *ain);
    for (int i = 0; i < nbb * nk; i++) { aout[i] = -2; ain[i] = -1; }
    /* A RETREATING edge carries nothing.
     *
     * This is the whole soundness argument, so it is worth stating.
     * "Available in vreg v" has to mean v holds the value THIS execution
     * would compute, and around a cycle it does not: a loop body that
     * leaves its result in one name hands the next iteration the
     * PREVIOUS value, computed from operands that have since moved on.
     * An earlier draft of this pass let that through and turned a hash
     * loop into `%63 = mov %63`.
     *
     * Dropping every retreating edge in the meet fixes it, and the
     * argument is short: after the last retreating edge an execution
     * takes, the rest of its path is acyclic, so no definition site on
     * it runs twice and every name still holds what this pass through
     * put there. The target of that edge starts from nothing, so
     * nothing older can leak in.
     *
     * What it costs is availability at a loop header, where a value
     * from before the loop would otherwise be reusable inside it. That
     * case needs the definition to dominate the use, which is the
     * question global CSE already answers, so nothing is actually lost.
     *
     * It also makes this a DAG problem: one pass in reverse post-order
     * is the fixpoint, with no optimistic initialization to have to
     * distrust. */
    int nv = fn->nvregs ? fn->nvregs : 1;
    int *st = xmalloc((size_t)nk * sizeof *st);
    int *own = xmalloc((size_t)nv * sizeof *own);
    int *stamp = xcalloc((size_t)nv, sizeof *stamp);
    struct pretab tab = { st, own, stamp, 0, nk, fn->nvregs };
    for (int oi = 0; oi < nbb; oi++) {
        int b = order[oi];
        int *in = ain + (size_t)b * nk;
        for (int k = 0; k < nk; k++) in[k] = -2;
        int nfwd = 0;
        for (int p = 0; p < bb[b].npred; p++) {
            int pb = bb[b].pred[p];
            if (bb[pb].rpo >= bb[b].rpo)
                continue;                       /* retreating: carries nothing */
            nfwd++;
            int *po = aout + (size_t)pb * nk;
            for (int k = 0; k < nk; k++) {
                int m = in[k], v = po[k];       /* three-valued meet */
                in[k] = (m == -1 || v == -1) ? -1
                      : (m == -2) ? v : (v == -2) ? m
                      : (m == v) ? m : -1;
            }
        }
        for (int k = 0; k < nk; k++)
            if (in[k] == -2 || nfwd != bb[b].npred) in[k] = -1;
        pre_enter(&tab, in);
        for (int i = bb[b].start; i < bb[b].end; i++)
            pre_step(fn, keyidx, i, &tab);
        int *out = aout + (size_t)b * nk;
        for (int k = 0; k < nk; k++) out[k] = st[k];
    }

    /* ---- reuse what is already available ---------------------------- *
     *
     * Global CSE reuses a value whose definition DOMINATES the use.
     * Availability is the weaker condition and so catches strictly more:
     * it is enough that every path here has passed a definition, under
     * one name. That is exactly the state an insertion below leaves --
     * a value arriving from several predecessors, dominated by none of
     * them -- so without this step the insertions would buy nothing
     * past the one block they were made for. */
    int nreuse = 0;
    for (int b = 0; b < nbb; b++) {
        pre_enter(&tab, ain + (size_t)b * nk);
        for (int i = bb[b].start; i < bb[b].end; i++) {
            int k = keyidx[i];
            if (k >= 0 && st[k] >= 0 && st[k] != fn->ins[i].dst) {
                to_mov(&fn->ins[i], st[k]);
                keyidx[i] = -1;         /* now a rename, not a definition */
                nreuse++;
            }
            pre_step(fn, keyidx, i, &tab);
        }
    }
    if (nreuse) {
        g_did.pre += nreuse;
        free(order); free(l2b); free(i2b); free(keyidx); free(keys);
        free(aout); free(ain); free(st); free(own); free(stamp); free(inv); free(pcan);
        free_cfg(bb, nbb); free_defs(&d);
        return 1;
    }

    /* One candidate: an expression computed here whose value some -- but
     * not all -- predecessors already hold. */
    int cand = -1, cb = -1;
    for (int oi = 0; oi < norder && cand < 0; oi++) {
        int b = order[oi];
        if (bb[b].npred < 2 || bb[b].end == bb[b].start ||
            fn->ins[bb[b].start].op != IR_LABEL)
            continue;           /* a merge point always opens with a label */
        int *in = ain + (size_t)b * nk;
        for (int i = bb[b].start; i < bb[b].end && cand < 0; i++) {
            int k = keyidx[i];
            if (k < 0 || in[k] >= 0)
                continue;               /* not numbered, or already reused */
            struct ir_ins *e = &fn->ins[i];
            if (!pre_worth_it(e))
                continue;
            /* only the first computation in this block; a second one is
             * local value numbering's to make */
            int first = 1;
            for (int j = bb[b].start; j < i; j++)
                if (keyidx[j] == k) { first = 0; break; }
            if (!first)
                continue;
            int have = 0, ok = 1;
            for (int p = 0; p < bb[b].npred && ok; p++) {
                int pb = bb[b].pred[p];
                if (bb[pb].rpo >= bb[b].rpo) { ok = 0; break; }
                if (aout[(size_t)pb * nk + k] >= 0) { have++; continue; }
                if (bb[pb].end == bb[pb].start) { ok = 0; break; }
                /* a critical edge is split rather than refused, but only
                 * a conditional branch gives somewhere to put the split */
                if (bb[pb].nsucc != 1) {
                    enum ir_op t = fn->ins[bb[pb].end - 1].op;
                    if (t != IR_BRZ && t != IR_BRNZ) { ok = 0; break; }
                }
                int at = pre_tail(fn, bb, pb);
                long cv;
                for (int w = 0; w < 2 && ok; w++) {
                    int v = w ? e->b : e->a;
                    if (v < 0) continue;
                    if (get_const(fn, &d, v, &cv)) continue;
                    if (pre_slot_read(fn, &d, inv, v)) continue;
                    if (!pre_have(fn, &d, bb, nbb, i2b, v, pb, at)) ok = 0;
                }
            }
            if (ok && have > 0 && have < bb[b].npred) { cand = i; cb = b; }
        }
    }
    if (cand < 0) {
        free(order); free(l2b); free(i2b); free(keyidx); free(keys);
        free(aout); free(ain); free(st); free(own); free(stamp); free(inv); free(pcan);
        free_cfg(bb, nbb); free_defs(&d);
        return 0;
    }

    /* ---- rewrite ---------------------------------------------------- */
    int key = keyidx[cand];
    int t = fn->nvregs++;
    struct ir_ins proto = fn->ins[cand];
    int line = proto.line, col = proto.col;
    int cblab = fn->ins[bb[cb].start].label;

    /* What each predecessor contributes. `psrc >= 0`: it holds the value
     * already, so a move names it. `pins`: where the contribution goes.
     * `pnew >= 0`: the edge was critical, so the contribution goes in a
     * new block with that label and `pjmp` says how the branch reaches
     * it. */
    int *pins = xmalloc((size_t)nbb * sizeof *pins);
    int *psrc = xmalloc((size_t)nbb * sizeof *psrc);
    int *pnew = xmalloc((size_t)nbb * sizeof *pnew);
    char *pjmp = xcalloc((size_t)nbb, 1);   /* 1: append a jmp after the branch */
    for (int b = 0; b < nbb; b++) { pins[b] = -1; psrc[b] = -1; pnew[b] = -1; }
    for (int p = 0; p < bb[cb].npred; p++) {
        int pb = bb[cb].pred[p];
        int src = aout[(size_t)pb * nk + key];
        psrc[pb] = src;
        if (src < 0 && bb[pb].nsucc != 1) {
            pnew[pb] = fn->nlabels++;
            /* retarget the branch when cb is its target; otherwise cb is
             * the fall-through, and a jmp after the branch takes it */
            struct ir_ins *term = &fn->ins[bb[pb].end - 1];
            /* a switch never falls through: cb is in its table */
            if (term->op == IR_SWITCH || term->label == cblab)
                pins[pb] = bb[pb].end - 1;                    /* retarget */
            else { pins[pb] = bb[pb].end; pjmp[pb] = 1; }
        } else {
            pins[pb] = pre_tail(fn, bb, pb);
        }
    }

    /* Emit the contribution of predecessor pb into `nb`. */
    struct ibuf nb = { 0, 0, 0 };
    int *newpos = fn->var_scope_lo
        ? xmalloc((size_t)(fn->nins + 1) * sizeof *newpos) : NULL;

#define PRE_EMIT(pb)                                                       \
    do {                                                                   \
        if (psrc[pb] >= 0) {                                               \
            struct ir_ins *m = ib_push(&nb);                               \
            memset(m, 0, sizeof *m);                                       \
            m->op = IR_MOV; m->dst = t; m->a = psrc[pb]; m->b = -1;        \
            m->w = proto.w; m->line = line; m->col = col;                  \
            m->synth = line ? 0 : 1;                                       \
        } else {                                                           \
            struct ir_ins e = proto;                                       \
            for (int w = 0; w < 2; w++) {                                  \
                int v = w ? proto.b : proto.a;                             \
                long cv;                                                   \
                struct ir_ins *r;                                          \
                if (v < 0) continue;                                       \
                if (get_const(fn, &d, v, &cv)) {                           \
                    int c = fn->nvregs++;                                  \
                    struct ir_ins *ci = ib_push(&nb);                      \
                    memset(ci, 0, sizeof *ci);                             \
                    ci->op = IR_CONST; ci->dst = c; ci->imm = cv;          \
                    ci->a = -1; ci->b = -1;                                \
                    ci->w = proto.w; ci->line = line; ci->col = col;       \
                    ci->synth = line ? 0 : 1;                              \
                    if (w) e.b = c; else e.a = c;                          \
                } else if ((r = pre_slot_read(fn, &d, inv, v)) != NULL) {  \
                    int c = fn->nvregs++;                                  \
                    struct ir_ins *ci = ib_push(&nb);                      \
                    *ci = *r; ci->dst = c;                                 \
                    if (w) e.b = c; else e.a = c;                          \
                }                                                          \
            }                                                              \
            e.dst = t;                                                     \
            *ib_push(&nb) = e;                                             \
        }                                                                  \
    } while (0)

    for (int n = 0; n < fn->nins; n++) {
        if (newpos) newpos[n] = nb.n;
        for (int b = 0; b < nbb; b++) {
            if (pins[b] != n || pnew[b] >= 0)
                continue;
            PRE_EMIT(b);
        }
        struct ir_ins *out;
        if (n == cand) {
            out = ib_push(&nb); *out = fn->ins[n]; to_mov(out, t);
        } else {
            out = ib_push(&nb); *out = fn->ins[n];
        }
        /* By index from here: a jump pushed below may move the buffer,
         * and `out` would then point into freed memory. */
        int out_at = nb.n - 1;
        /* the two forms a split edge takes */
        for (int b = 0; b < nbb; b++) {
            if (pnew[b] < 0 || pins[b] != n) continue;
            if (pjmp[b]) {
                struct ir_ins *j = ib_push(&nb);
                memset(j, 0, sizeof *j);
                j->op = IR_JMP; j->dst = -1; j->a = -1; j->b = -1;
                j->label = pnew[b]; j->line = line; j->col = col;
                j->synth = line ? 0 : 1;
            } else if (nb.p[out_at].op == IR_SWITCH) {
                struct retarget rt = { cblab, pnew[b] };
                each_label(fn, &nb.p[out_at], retarget_cb, &rt);  /* every entry that was cb */
            } else {
                nb.p[out_at].label = pnew[b];   /* branch now enters the split */
            }
        }
    }
    /* The split blocks themselves, past the end: each on exactly one
     * edge, each falling out to where the branch used to go. */
    for (int b = 0; b < nbb; b++) {
        if (pnew[b] < 0) continue;
        struct ir_ins *l = ib_push(&nb);
        memset(l, 0, sizeof *l);
        l->op = IR_LABEL; l->dst = -1; l->a = -1; l->b = -1;
        l->label = pnew[b]; l->line = line; l->col = col; l->synth = 1;
        PRE_EMIT(b);
        struct ir_ins *j = ib_push(&nb);
        memset(j, 0, sizeof *j);
        j->op = IR_JMP; j->dst = -1; j->a = -1; j->b = -1;
        j->label = cblab; j->line = line; j->col = col; j->synth = 1;
    }
#undef PRE_EMIT

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
    g_did.pre++;
    free(pins); free(psrc); free(pnew); free(pjmp);
    free(order); free(l2b); free(i2b); free(keyidx); free(keys);
    free(aout); free(ain); free(st); free(own); free(stamp); free(inv); free(pcan);
    free_cfg(bb, nbb); free_defs(&d);
    return 1;
}

static int pass_pre(struct ir_func *fn)
{
    int changed = 0, guard = 0;
    while (guard++ < 64 && pre_one(fn))
        changed = 1;
    return changed;
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
        free_cfg(bb, nbb); return 0;
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
    char *wrin = xmalloc((size_t)(fn->nvregs ? fn->nvregs : 1));
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
        for (int q = 0; q < bb[h].npred; q++) {   /* back edges into h */
            int p = bb[h].pred[q];
            if (bb_dominates(bb, h, p)) {
                loop_body(bb, nbb, h, p, in);
                any_back = 1;
            }
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
            if (t != IR_JMP && t != IR_RET && t != IR_UD2 && t != IR_SWITCH)
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
        /* Which temps the loop writes at all, by ANY definition. A temp
         * written more than once -- an enclosing loop's counter, defined
         * at its entry and again at its latch -- is still invariant here
         * when none of those writes is inside this loop: the loop is
         * entered only through its preheader, and nothing in it changes
         * the value it found there. Refusing every multiply-defined
         * operand refused the row address of every 2-D array walk:
         * `ma[i][k]` recomputed i*48 + ma on each trip of the k loop. */
        memset(wrin, 0, (size_t)(fn->nvregs ? fn->nvregs : 1));
        for (int n = 0; n < fn->nins; n++) {
            if (!inl_ins[n])
                continue;
            int t = def_target(&fn->ins[n]);
            if (t >= 0 && t < fn->nvregs) wrin[t] = 1;
            if (fn->ins[n].op == IR_LANDING && fn->ins[n].b >= 0 &&
                fn->ins[n].b < fn->nvregs)
                wrin[fn->ins[n].b] = 1;
            if (fn->ins[n].op == IR_ASM)
                memset(wrin, 1, (size_t)(fn->nvregs ? fn->nvregs : 1));
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
                    if (d.cnt[v] != 1) {
                        /* several writes: invariant iff none in the loop
                         * (a local's slot has its own rules, above) */
                        if (v < fn->nvars || wrin[v]) all = 0;
                        continue;
                    }
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
                if (d.cnt[o.v[q]] != 1)
                    continue;                /* written only outside the loop */
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

    free(hoist); free(stored); free(inv); free(inl_ins); free(wrin); free(in);
    free_defs(&d); free(addr_taken);
    free(order); free(l2b);
    free_cfg(bb, nbb);
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
static int g_opt_size;                  /* -Os, set by opt_run (below) */
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
        free_cfg(bb, nbb); return 0;
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
        for (int q = 0; q < bb[h].npred; q++) {   /* back edges into h */
            int p = bb[h].pred[q];
            if (bb_dominates(bb, h, p)) {
                latch = p; nback++;
            }
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
         * so each must define a temp read only here -- and must be one
         * that may appear twice.
         *
         * A LOAD may. It is not `is_pure` (it can fault, so nothing may
         * SPECULATE one), but rotation speculates nothing: the header's
         * computation runs once in the guard and once per latch, which
         * for N iterations is the N+1 times the header itself ran, in
         * the same order and at the same points. Refusing it refused
         * `while (*p) p++;` -- every string walk, every list walk --
         * which then paid two branches an iteration for the life of the
         * program. Volatile is still refused: the ACCESS is the effect
         * there, and moving one is not a thing to do on this argument. */
        /* A value the header computes and the body (or the code after
         * the loop) also reads -- `while (*s) h ^= *s++;` once value
         * numbering has given the body the header's load -- is written
         * by the copy under its OWN name: the body is entered from the
         * guard or from the copy, and either way that name holds this
         * iteration's value; so does every path to the exit. It is
         * assigned twice then, which is the price; refusing it cost
         * every such loop a jump an iteration. One read only in the
         * header gets a fresh temp, so the guard's stands untouched. */
        int ok = 1;
        int ncopy = bb[h].end - 1 - (bb[h].start + 1);
        char *outside = xcalloc((size_t)(ncopy ? ncopy : 1), 1);
        for (int n = bb[h].start + 1; n < bb[h].end - 1 && ok; n++) {
            struct ir_ins *i = &fn->ins[n];
            /* A STORE may appear twice too, by the argument the load
             * makes: it runs once in the guard and once per latch, the
             * N+1 times the header ran, in the same order against the
             * body. `while ((d[n] = s[n]) != 0) n++;` -- strcpy, and
             * the hash table's key copy -- has its test after the
             * store, and stayed a jump and an index re-extended every
             * iteration. It defines nothing, so nothing is renamed. Not
             * at -Os: the guard is a second copy of the store, and the
             * jump it saves is the same two bytes. */
            if (i->op == IR_STORE && !i->vol && !g_opt_size &&
                !getenv("EMBCC_NO_ROTSTORE"))
                continue;
            if (!(is_pure(i->op) || i->op == IR_LOAD) || i->vol)
                { ok = 0; break; }
            int t = def_target(i);
            if (t < fn->nvars || t >= fn->nvregs) { ok = 0; break; }
            for (int m = 0; m < fn->nins; m++) {
                if (m >= bb[h].start && m < bb[h].end)
                    continue;
                if (ins_reads(&fn->ins[m], t)) {
                    outside[n - (bb[h].start + 1)] = 1;
                    break;
                }
            }
        }
        if (!ok) { free(in); free(outside); continue; }

        /* A value with no operands -- a constant, an address -- is the
         * same on every iteration: the copy reads the guard's, which
         * dominates the latch, rather than writing it again. Written
         * twice it would stop being a known constant, and folding,
         * LICM and unrolling all ask for exactly one definition (the
         * loop bound `j < 24` shared by value numbering with `i * 24`
         * left the workload's matrix checksum un-unrolled). */
        int *map = xmalloc((size_t)(ncopy ? ncopy : 1) * sizeof *map);
        for (int k = 0; k < ncopy; k++) {
            enum ir_op op = fn->ins[bb[h].start + 1 + k].op;
            int fixed = op == IR_CONST || op == IR_GADDR || op == IR_ADDR ||
                        op == IR_STRADDR || op == IR_FADDR;
            map[k] = op == IR_STORE ? -2     /* copied, and names nothing */
                   : fixed ? -1
                   : outside[k] ? def_target(&fn->ins[bb[h].start + 1 + k])
                   : fn->nvregs++;
        }
        free(outside);
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
                    if (old >= 0 && old < fn->nvregs && map[q] >= 0 &&
                        map[q] != old)
                        tbl[old] = map[q];
                }
                for (int k = 0; k < ncopy; k++) {
                    if (map[k] == -1)
                        continue;            /* the guard's value stands */
                    struct ir_ins *c = ib_push(&nb);
                    *c = fn->ins[bb[h].start + 1 + k];
                    struct lcopy lc = { tbl, fn->nvregs, 0 };
                    each_read(c, lcopy_cb, &lc);
                    if (map[k] >= 0)
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
    free_cfg(bb, nbb);
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

static int pass_idiom(struct ir_func *fn)
{
    int changed = 0, guard = 0;
    while (guard++ < 16 && idiom_one(fn))
        changed = 1;
    return changed;
}

/* Is `i` (defining t) a reduction's add -- `next = acc + v`, with a phi
 * copy `acc = mov next` inside the loop closing the circle? */
static int vec_self_accum(struct ir_func *fn, struct defs *d, int lo, int hi,
                          const struct ir_ins *i, int t)
{
    if (i->op != IR_ADD || t < 0 || t >= fn->nvregs)
        return 0;
    for (int n = lo; n < hi; n++) {
        const struct ir_ins *m = &fn->ins[n];
        if (m->op != IR_MOV || m->a != t)
            continue;
        if (m->dst < 0 || m->dst >= fn->nvregs || d->cnt[m->dst] != 2)
            continue;
        if (m->dst == i->a || m->dst == i->b)
            return 1;
    }
    return 0;
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

        /* One back edge, from a latch that branches to the header --
         * the rotated shape, which is the only one whose test is at the
         * bottom where the step is. */
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
                /* And a self-accumulating add -- `next = acc + v` with
                 * `acc = mov next` closing the circle -- is a REDUCTION,
                 * which the detector below is what judges. It has no
                 * lane-wise form of its own: the accumulator is a scalar
                 * carried round the loop, not sixteen bytes.
                 *
                 * Which side the accumulator lands on is not fixed.
                 * `s += v` and `s = v + s` are one expression, and the
                 * canonical operand order may put either first -- so
                 * this asks about the SHAPE rather than about operand b,
                 * which is what the test below used to stand in for and
                 * what stopped working the day the order was made
                 * canonical. */
                if (vec_self_accum(fn, &d, L.lo, L.hi, i, def_target(i)))
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
                /* Floating-point arithmetic has no lane form here: the
                 * lane operations are INTEGER ones (paddd, psll...), and
                 * `a[i] = b[i] + c[i]` over floats was added as integers
                 * -- wrong answers at -O2 on x86-64. The reduction below
                 * already refused a float sum; a copy, being only loads
                 * and stores, is still vectorized. */
                if (i->flt) { ok = 0; break; }
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
        /* A frame slot written in the body -- a local kept in memory
         * because its address is taken -- is one place written on every
         * iteration. Neither rewrite keeps that: the copying one renames
         * every definition, slots included, and the in-place one would
         * write it once per vector of iterations. */
        {
            int slot_write = 0;
            for (int q = L.lo; q < L.hi; q++)
                slot_write |= fn->ins[q].op == IR_STVAR;
            if (slot_write) {
                VDBG("h=%d writes a local kept in memory\n", h);
                free(vec); continue;
            }
        }
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
                    /* (the location by value: each ib_push may move the
                     * buffer the previous one returned a pointer into) */
                    int zl = fn->ins[L.red_add].line, zc = fn->ins[L.red_add].col;
                    struct ir_ins *z = ib_push(&nb);
                    z->op = IR_CONST; z->dst = vzero;
                    z->w = wsize; z->imm = 0;
                    z->line = zl;
                    z->col = zc;
                    struct ir_ins *sp = ib_push(&nb);
                    sp->op = IR_VSPLAT; sp->dst = vacc; sp->a = vzero;
                    sp->size = wsize; sp->w = 8;
                    sp->line = zl; sp->col = zc;
                    if (vacc2 >= 0) {
                        struct ir_ins *s2 = ib_push(&nb);
                        s2->op = IR_VSPLAT; s2->dst = vacc2; s2->a = vzero;
                        s2->size = wsize; s2->w = 8;
                        s2->line = zl; s2->col = zc;
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
    free_cfg(bb, nbb);
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

/* Is `x` the induction variable scaled by a constant -- iv, a widening of
 * it, a shift of that by a constant, or a multiply by one? Fills the
 * scale. A multiply is how a ROW of a 2-D array is indexed (`m[k][j]`
 * is base + k*48 + j*2), and the shift is how an element is. */
static int scaled_iv(struct ir_func *fn, struct defs *d, int lo, int hi,
                     int x, int iv, long *scale_out)
{
    long sc = 1;
    if (x >= 0 && x < fn->nvregs && d->cnt[x] == 1) {
        int xn = d->ins[x];
        if (xn >= lo && xn < hi &&
            (fn->ins[xn].op == IR_SHL || fn->ins[xn].op == IR_MUL)) {
            struct ir_ins *s = &fn->ins[xn];
            long c;
            if (!const_b(fn, d, s, &c))
                return 0;
            if (s->op == IR_SHL) {
                if (c < 0 || c > 30) return 0;
                sc = 1L << c;
            } else {
                if (c <= 0 || c > (1L << 20)) return 0;
                sc = c;
            }
            x = s->a;
        }
    }
    if (x >= 0 && x < fn->nvregs && d->cnt[x] == 1) {
        int xn = d->ins[x];
        if (xn >= lo && xn < hi && fn->ins[xn].op == IR_EXT)
            x = fn->ins[xn].a;
    }
    if (x != iv)
        return 0;
    *scale_out = sc;
    return 1;
}

/* Defined once, and outside [lo, hi): a value the loop cannot change. */
static int invariant_vreg(struct ir_func *fn, struct defs *d, int lo, int hi, int v)
{
    if (v < 0 || v >= fn->nvregs || d->cnt[v] != 1)
        return 0;
    int bd = d->ins[v];
    return !(bd >= lo && bd < hi);
}
static int is_const_vreg(struct ir_func *fn, struct defs *d, int v)
{
    return v >= 0 && v < fn->nvregs && d->cnt[v] == 1 && d->ins[v] >= 0 &&
           fn->ins[d->ins[v]].op == IR_CONST;
}

/* Is `v` the value base + iv*scale (+ base2), every base invariant? The
 * second base is the column of a row walk: (m + k*48) + j*2, where j*2
 * is fixed while k moves. base2 is -1 when there is none. */
static int linear_in_iv2(struct ir_func *fn, struct defs *d, int lo, int hi,
                         int v, int iv, int *base_out, int *base2_out,
                         long *scale_out)
{
    if (v < 0 || v >= fn->nvregs || d->cnt[v] != 1)
        return 0;
    int an = d->ins[v];
    if (an < lo || an >= hi || fn->ins[an].op != IR_ADD || fn->ins[an].imm_b)
        return 0;
    const struct ir_ins *add = &fn->ins[an];
    for (int side = 0; side < 2; side++) {
        int p = side ? add->b : add->a, q = side ? add->a : add->b;
        if (!invariant_vreg(fn, d, lo, hi, p))
            continue;
        /* base + scaled iv -- where the base is something to walk from:
         * a CONSTANT base is counter arithmetic, `i + 1` with its one
         * hoisted into a vreg, and taking that for an address turned
         * every counted loop's increment into a second induction
         * variable, which the unroller then no longer recognised (the
         * `unroll` kernel ran 1.6 times slower). */
        if (!is_const_vreg(fn, d, p) &&
            scaled_iv(fn, d, lo, hi, q, iv, scale_out)) {
            *base_out = p; *base2_out = -1;
            return 1;
        }
        /* (base + scaled iv) + base2, the inner sum computed only here */
        if (q >= 0 && q < fn->nvregs && d->cnt[q] == 1) {
            int qn = d->ins[q];
            if (qn < lo || qn >= hi || fn->ins[qn].op != IR_ADD || fn->ins[qn].imm_b)
                continue;
            const struct ir_ins *in2 = &fn->ins[qn];
            for (int s2 = 0; s2 < 2; s2++) {
                int b1 = s2 ? in2->b : in2->a, r = s2 ? in2->a : in2->b;
                if (invariant_vreg(fn, d, lo, hi, b1) &&
                    !(is_const_vreg(fn, d, b1) && is_const_vreg(fn, d, p)) &&
                    scaled_iv(fn, d, lo, hi, r, iv, scale_out)) {
                    *base_out = b1; *base2_out = p;
                    return 1;
                }
            }
        }
    }
    return 0;
}

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
         * the new pointer has to be initialised to match it: its one
         * other definition is OUTSIDE the loop, and is a zero.
         *
         * "Outside" is the part that was missing. With both definitions
         * inside, this loop had nothing to check and said yes -- which is
         * what an INNER loop's counter looks like from the loop around
         * it: `for (j..) for (i = k; i < 16; i++) s += a[i]` walked its
         * pointer once per j, from a, and the inner loop read a[j]
         * sixteen times. Only an inner counter starting at zero escaped,
         * because that inner loop was rewritten first and its counter
         * was gone before the outer one looked. */
        int init_zero = 1, ninit = 0;
        for (int n = 0; n < fn->nins && init_zero; n++) {
            if (n >= lo && n < hi)
                continue;
            struct ir_ins *i = &fn->ins[n];
            if (def_target(i) != iv)
                continue;
            ninit++;
            if (!((i->op == IR_MOV && i->a >= 0 && i->a < fn->nvregs &&
                   d.cnt[i->a] == 1 && d.ins[i->a] >= 0 &&
                   fn->ins[d.ins[i->a]].op == IR_CONST &&
                   fn->ins[d.ins[i->a]].imm == 0) ||
                  (i->op == IR_CONST && i->imm == 0)))
                init_zero = 0;
            /* A copy of a temp written more than once -- value numbering
             * shares one `const 0` between `s = 0` and `k = 0`, and s is
             * then the loop's accumulator too. What the copy reads is the
             * nearest write of that temp before it in the same block: if
             * that is a zero, so is the counter. */
            if (!init_zero && i->op == IR_MOV && i->a >= 0 &&
                i->a < fn->nvregs && d.cnt[i->a] > 1) {
                for (int m = n - 1; m >= 0; m--) {
                    const struct ir_ins *w = &fn->ins[m];
                    if (w->op == IR_LABEL || w->op == IR_ASM ||
                        w->op == IR_LANDING)
                        break;
                    if (def_target(w) != i->a)
                        continue;
                    if (w->op == IR_CONST && w->imm == 0 && !w->flt)
                        init_zero = 1;
                    break;
                }
            }
        }
        if (!init_zero || ninit != 1)
            continue;

        /* every candidate: an address computed from the index, whose
         * value nothing outside the loop reads */
        int cand[16], cbase[16], cbase2[16], nc = 0;
        long cscale[16];
        for (int n = lo; n < hi && nc < 16; n++) {
            int t = def_target(&fn->ins[n]);
            int base, base2 = -1; long scale;
            /* At POINTER width, and only there: the walk is an add of that
             * width, so it reproduces the chain exactly modulo 2^PTRW and
             * nothing else. A 64-bit integer on a 32-bit target --
             * `m[i][k] = i * 1000000000LL + k` -- is linear in k too, and
             * walking it as a pointer kept its low word and dropped the
             * high one. */
            if (t < 0 || fn->ins[n].op != IR_ADD || fn->ins[n].w != PTRW ||
                fn->ins[n].flt)
                continue;
            /* Computed AFTER the induction variable's own update, it is
             * the address for the NEXT value of the index -- and the
             * pointer moves only at inc_at, further on, so it would
             * still hold this iteration's. Rotation puts exactly such an
             * address in the latch: the copy of the header's test goes
             * after `i = mov i1`, and `while (s[n]) n++;` read s[n - 1]
             * there on every trip -- strlen by index was one short of
             * its answer at -O2 and -Os on every target. */
            if (n > copy_ins)
                continue;
            if (!linear_in_iv(fn, &d, lo, hi, t, iv, &base, &scale) &&
                !linear_in_iv2(fn, &d, lo, hi, t, iv, &base, &base2, &scale))
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
            cand[nc] = t; cbase[nc] = base; cbase2[nc] = base2;
            cscale[nc] = scale; nc++;
        }
        if (nc == 0)
            continue;

        /* ---- and then the loop can stop counting ----------------------
         *
         * The test `i < bound` and the pointer `p = base + i*scale` are
         * about the same iterations, so the test can be made about the
         * POINTER instead: p walks from base in steps of scale*step and
         * reaches base + T*scale*step exactly when i reaches T*step,
         * where T = ceil(bound/step) is the trip count. `!=` rather than
         * `<` because p hits that value exactly -- it steps by a fixed
         * amount from a fixed start -- which also keeps the question
         * away from whether a pointer compare is signed.
         *
         * What it buys is the counter: once nothing reads i, `i += step`
         * and the copy that carries it are a cycle that no live
         * instruction reaches, and the marking dead-code pass takes
         * both. Two instructions an iteration, and one live value fewer
         * across the whole loop. Measured on the vectorized copy loop of
         * tests/bench/kernels.c memory_stream: ten instructions an
         * iteration down to eight, which is gcc's nine.
         *
         * Only for a CONSTANT bound, because T is otherwise a division
         * by step in the preheader, and only when the index is dead
         * outside the loop, because otherwise the counter stays and the
         * two extra instructions here are all that happens. */
        int lftr_lim = -1, lftr_k = -1, lftr_iv = -1;
        long lftr_off = 0;
        {
            struct ir_ins *cmpi = &fn->ins[inc_at];
            long bound = 0;
            long scale = cscale[0], dv = scale * step;
            if (cmpi->op == IR_CMP && cmpi->pred == B_LT &&
                cmpi->sign && !cmpi->flt && cmpi->a == iv &&
                const_b(fn, &d, cmpi, &bound) &&
                bound > 0 && bound <= (1L << 40) &&
                scale > 0 && scale <= (1L << 20) && step <= (1L << 20)) {
                long T = (bound + step - 1) / step;
                if (T > 0 && dv > 0 && T <= (1L << 40) / dv) {
                    lftr_off = T * dv;
                    lftr_k = fn->nvregs++;
                    lftr_lim = fn->nvregs++;
                    /* The index gets a NAME OF ITS OWN inside the loop.
                     * It has two definitions -- the one before the loop
                     * and the copy at the latch -- and the loop's GUARD
                     * reads it, which is a use no amount of rewriting
                     * inside the loop removes. One vreg with a live read
                     * keeps every definition of it, so the latch copy
                     * would survive and the counter with it. Split in
                     * two and the guard keeps the value it was always
                     * reading (the one from before the loop) while the
                     * loop's copy becomes a cycle nothing outside can
                     * reach. */
                    lftr_iv = fn->nvregs++;
                }
            }
        }

        /* ---- rewrite: a pointer per candidate ---- */
        int ptr[16], delta[16], bsum[16];
        for (int k = 0; k < nc; k++) {
            ptr[k] = fn->nvregs++;
            delta[k] = fn->nvregs++;
            bsum[k] = cbase2[k] >= 0 ? fn->nvregs++ : -1;
        }
        struct ibuf nb = { 0, 0, 0 };
        int *newpos = fn->var_scope_lo
            ? xmalloc((size_t)(fn->nins + 1) * sizeof *newpos) : NULL;
        for (int n = 0; n < fn->nins; n++) {
            if (n == lo) {
                /* a two-part base, summed once before the loop */
                for (int k = 0; k < nc; k++) {
                    if (bsum[k] < 0)
                        continue;
                    struct ir_ins *a = ib_push(&nb);
                    a->op = IR_ADD; a->dst = bsum[k]; a->a = cbase[k];
                    a->b = cbase2[k]; a->w = PTRW;
                    a->line = fn->ins[d.ins[cand[k]]].line; a->synth = 1;
                    cbase[k] = bsum[k];
                }
                for (int k = 0; k < nc; k++) {
                    /* the line by value: `c` is not valid after the next
                     * ib_push, which may move the buffer -- reading it
                     * there faulted once the push landed on a growth */
                    int ln = fn->ins[cand[k] >= 0 ? d.ins[cand[k]] : n].line;
                    struct ir_ins *c = ib_push(&nb);
                    c->op = IR_CONST; c->dst = delta[k];
                    c->w = PTRW; c->imm = cscale[k] * step;
                    c->line = ln;
                    c->synth = 1;
                    struct ir_ins *m = ib_push(&nb);
                    m->op = IR_MOV; m->dst = ptr[k]; m->a = cbase[k];
                    m->w = PTRW;
                    m->line = ln; m->synth = 1;
                }
                if (lftr_lim >= 0) {
                    struct ir_ins *c = ib_push(&nb);
                    c->op = IR_CONST; c->dst = lftr_k; c->w = PTRW;
                    c->imm = lftr_off;
                    c->line = fn->ins[inc_at].line; c->synth = 1;
                    struct ir_ins *a3 = ib_push(&nb);
                    a3->op = IR_ADD; a3->dst = lftr_lim; a3->a = cbase[0];
                    a3->b = lftr_k; a3->w = PTRW;
                    a3->line = fn->ins[inc_at].line; a3->synth = 1;
                    struct ir_ins *m2 = ib_push(&nb);
                    m2->op = IR_MOV; m2->dst = lftr_iv; m2->a = iv;
                    m2->w = fn->ins[copy_ins].w ? fn->ins[copy_ins].w : PTRW;
                    m2->line = fn->ins[inc_at].line; m2->synth = 1;
                }
            }
            if (n == inc_at) {          /* walk each pointer, after the copies */
                for (int k = 0; k < nc; k++) {
                    struct ir_ins *a2 = ib_push(&nb);
                    a2->op = IR_ADD; a2->dst = ptr[k]; a2->a = ptr[k];
                    a2->b = delta[k]; a2->w = PTRW;
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
            if (lftr_lim >= 0 && n == inc_at) {
                struct ir_ins *o = ib_push(&nb);
                *o = fn->ins[n];
                o->pred = B_NE; o->a = ptr[0]; o->b = lftr_lim;
                o->w = PTRW; o->sign = 0; o->imm_b = 0; o->imm = 0;
                continue;
            }
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
            /* Inside the loop, the index goes by its own name. */
            if (lftr_iv >= 0 && n >= lo && n < hi) {
                struct lcopy lc;
                int *tbl = xmalloc((size_t)fn->nvregs * sizeof *tbl);
                for (int v = 0; v < fn->nvregs; v++) tbl[v] = -1;
                tbl[iv] = lftr_iv;
                lc.cp = tbl; lc.nv = fn->nvregs; lc.n = 0;
                each_read(o, lcopy_cb, &lc);
                free(tbl);
                if (def_target(o) == iv)
                    o->dst = lftr_iv;
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
    free_cfg(bb, nbb);
    return done;
}

static int pass_ivsr(struct ir_func *fn)
{
    int changed = 0, guard = 0;
    while (guard++ < 32 && ivsr_one(fn))
        changed = 1;
    return changed;
}

/* ==== loop unrolling (-O2, not -Os) ======================================= *
 *
 * `for (i = 0; i < n; i++) s += i & 7;` spends two of its five
 * instructions on being a loop -- the increment and the test -- and one
 * more on the branch back. Running the body four times per test spends
 * them once instead of four times, and gives the block-local passes four
 * copies in one block to fold, value-number and schedule together.
 *
 * ---- the shape, and why there is no remainder loop -----------------------
 *
 * A runtime trip count is the whole difficulty: `n` is not known, so the
 * body cannot simply be copied U times. The usual answer is a second,
 * scalar loop for the leftover iterations. This does not build one,
 * because it already has one -- THE ORIGINAL LOOP. The unrolled copies
 * are inserted in FRONT of it and fall into it when fewer than U
 * iterations remain:
 *
 *   LU:  if (!(i < n)) goto exit          <- the loop may be over
 *        if ((unsigned)(n - i) < U) goto L  <- fewer than U left
 *        body; i++    (U times, no test between)
 *        goto LU
 *   L:   body; i++; if (i < n) goto L     <- the original loop, untouched
 *   exit:
 *
 * So the transform ADDS code and changes nothing that was there: the
 * original loop is still correct on its own, still the target of its own
 * back edge, and still what runs if anything jumps straight to it.
 *
 * The subtraction is the part worth stating. `n - i` computed as SIGNED
 * can overflow -- `for (i = LONG_MIN; i < LONG_MAX; i++)` is a legal
 * loop -- so the count of remaining iterations is taken in UNSIGNED,
 * where the difference of two values with i < n is exactly n - i and
 * cannot wrap. The `i < n` test above it is what makes that true, which
 * is why it is there and not left to the loop's own guard: this block is
 * the target of a back edge, and the guard runs once.
 *
 * ---- what may be unrolled ------------------------------------------------
 *
 * A rotated, bottom-tested loop -- rotation has already run -- whose only
 * control transfer is its own back edge, whose induction variable steps
 * by one and is compared `< invariant` with a signed compare, and whose
 * body defines nothing that is read after the loop except through the
 * values the loop carries (which keep their numbers, so the last copy's
 * write is the one that escapes). Anything that moves the stack or is
 * its own control flow -- alloca, inline asm, a landing pad, a computed
 * goto -- is refused rather than duplicated.
 */

#define UNROLL_MAX_BODY   20  /* instructions in the body worth copying */
#define UNROLL_BUDGET     96  /* and a ceiling on U * body */
#define UNROLL_MAX_COPIES  8
#define UNROLL_FULL_TRIP  32  /* a constant trip count copied whole: at most */
#define UNROLL_FULL_BODY 200  /* ...and at most this many instructions in all */

/* One loop's worth of what the recognizer found. */
struct unrloop {
    int lo, hi;        /* the loop's instruction range, [lo, hi) */
    int header;        /* bb index of the header; Lh is its label */
    int Lh;
    int iv;            /* the induction variable's phi temp */
    int bound;         /* the vreg it is compared against (loop-invariant) */
    long step;         /* what the iv advances by each iteration: 1, or a
                        * pointer walk's element size */
    int cmp_ins;       /* `c = cmp.w lt iv, bound`, or `cmp.w ne iv, bound`
                        * once strength reduction has made the loop walk a
                        * pointer -- not copied */
    int w;             /* 4 or 8: the compare's width */
    int body_lo;       /* [body_lo, cmp_ins): what a copy consists of */
};

/* Is this instruction safe to appear twice? Duplication does not change
 * how many times anything RUNS -- the copies stand in for iterations
 * that would have happened anyway -- so the question is only whether the
 * instruction can be re-emitted at all. */
static int unr_copyable(const struct ir_ins *i)
{
    switch (i->op) {
    case IR_ALLOCA: case IR_SPSAVE: case IR_SPRESTORE:
    case IR_ASM: case IR_VA_START: case IR_LANDING:
    case IR_IGOTO: case IR_LABELADDR: case IR_SWITCH:
    case IR_JMP: case IR_BRZ: case IR_BRNZ: case IR_RET: case IR_UD2:
        return 0;
    case IR_CALL:
        return !i->eh_region;   /* a call in an exception region has an edge */
    default:
        return 1;
    }
}

/* One fresh instruction in the buffer. A FUNCTION and not a macro: its
 * arguments are fully evaluated before ib_push runs, so there is no way
 * for a nested push to reallocate the buffer under a pointer this
 * already returned -- which is how three separate bugs got written. */
static struct ir_ins *unr_emit(struct ibuf *nb, enum ir_op op,
                               int line, int col)
{
    struct ir_ins *p = ib_push(nb);
    p->op = op;
    p->line = line; p->col = col; p->synth = 1;
    return p;
}

/* Find an unrollable loop, or return 0. */
static int unr_find(struct ir_func *fn, struct bb *bb, int nbb, int *order,
                    int norder, struct defs *d, char *in, struct unrloop *L,
                    const char *seen, int nseen)
{
    for (int oi = norder - 1; oi >= 0; oi--) {
        int h = order[oi];
        if (h == 0 || bb[h].end <= bb[h].start ||
            fn->ins[bb[h].start].op != IR_LABEL)
            continue;
        int Lh = fn->ins[bb[h].start].label;
        /* A loop this pass already unrolled still matches, and unrolling
         * it again would only copy the remainder loop into another
         * remainder loop. One pass over each. */
        if (Lh >= 0 && Lh < nseen && seen[Lh])
            continue;
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
        /* The loop's blocks must be exactly the contiguous instruction
         * range [lo, hi): the same test the vectorizer makes, and what
         * lets a copy be a memcpy of a slice. */
        memset(in, 0, (size_t)nbb);
        loop_body(bb, nbb, h, latch, in);
        L->lo = bb[h].start; L->hi = bb[latch].end;
        int ok = 1;
        for (int b = 0; b < nbb && ok; b++)
            if (in[b] != (bb[b].start >= L->lo && bb[b].end <= L->hi))
                ok = 0;
        if (!ok)
            continue;
        /* There must be a label after the loop to send a finished
         * unrolled run to. One is created by the caller when the loop
         * is taken, so only the range has to be inside the function. */
        if (L->hi > fn->nins)
            continue;

        /* `c = cmp.w lt iv, bound`, signed, bound a loop-invariant vreg --
         * or `cmp.w ne iv, bound`, which is what induction-variable
         * strength reduction leaves: the loop walks a pointer up to its
         * end. For `ne` the iterations left are the UNSIGNED, modular
         * (bound - iv) / step exactly when the loop terminates at all,
         * whatever the direction the values were in, so the counted
         * tests below are right for it with no signed guard. */
        int cn = (br->a >= 0 && br->a < fn->nvregs && d->cnt[br->a] == 1)
                 ? d->ins[br->a] : -1;
        if (cn < L->lo || cn >= L->hi || cn != L->hi - 2)
            continue;                    /* the test must be the latch's last */
        struct ir_ins *cmp = &fn->ins[cn];
        if (cmp->op != IR_CMP || cmp->flt || cmp->imm_b ||
            cmp->b < 0 || cmp->b >= fn->nvregs ||
            !((cmp->pred == B_LT && cmp->sign) || cmp->pred == B_NE))
            continue;
        if (cmp->w != 4 && cmp->w != 8)
            continue;
        L->cmp_ins = cn; L->iv = cmp->a; L->bound = cmp->b; L->w = cmp->w;
        L->header = h; L->Lh = Lh;
        if (L->iv < 0 || L->iv >= fn->nvregs)
            continue;
        /* The bound is loop-invariant, and live on entry to the header
         * (the compare reads it), so it is available anywhere on the
         * edge into the header -- which is where the copies go. */
        int bn = d->cnt[L->bound] == 1 ? d->ins[L->bound] : -2;
        if (bn == -2 || (bn >= L->lo && bn < L->hi))
            continue;

        /* `iv = mov next`, `next = iv + 1`, both inside the loop -- or the
         * one instruction `iv = add iv, step`, which is how strength
         * reduction advances the pointer it walks. Either way the iv must
         * be written exactly once inside the loop, so that the renaming
         * of the copies carries it from each to the next. */
        int copy_ins = -1, step_ins = -1, ndef = 0;
        for (int n = L->lo; n < L->hi; n++) {
            const struct ir_ins *q = &fn->ins[n];
            if (def_target(q) != L->iv)
                continue;
            ndef++;
            if (q->op == IR_MOV) copy_ins = n;
            else if (q->op == IR_ADD && q->a == L->iv) step_ins = n;
        }
        if (ndef != 1)
            continue;
        if (copy_ins >= 0) {
            int nxt = fn->ins[copy_ins].a;
            if (nxt < 0 || nxt >= fn->nvregs || d->cnt[nxt] != 1)
                continue;
            step_ins = d->ins[nxt];
            if (step_ins < L->lo || step_ins >= L->hi ||
                fn->ins[step_ins].op != IR_ADD ||
                fn->ins[step_ins].a != L->iv)
                continue;
        } else if (step_ins < 0) {
            continue;
        }
        long step;
        if (!const_b(fn, d, &fn->ins[step_ins], &step) || step < 1 ||
            (cmp->pred == B_LT && step != 1) || step > 4096)
            continue;
        L->step = step;

        /* Everything from just after the header's label up to the test is
         * what a copy consists of. */
        L->body_lo = L->lo + 1;
        int nbody = 0;
        ok = 1;
        for (int n = L->body_lo; n < L->cmp_ins && ok; n++) {
            struct ir_ins *i = &fn->ins[n];
            if (i->op == IR_LABEL) {
                /* An empty label left behind by rotation is fine to drop
                 * from the copies; one anything can still reach is not. */
                for (int m = 0; m < fn->nins && ok; m++) {
                    enum ir_op o = fn->ins[m].op;
                    if ((o == IR_JMP || o == IR_BRZ || o == IR_BRNZ) &&
                        fn->ins[m].label == i->label)
                        ok = 0;
                }
                continue;
            }
            if (!unr_copyable(i)) { ok = 0; break; }
            /* A loop whose body CALLS something gets nothing out of
             * this. What unrolling saves is the test and the branch,
             * which next to a call -- its argument setup, its frame, its
             * return -- is a rounding error, and what it costs is U
             * copies of the call site and U times the pressure across
             * it. Measured: four copies of `s = f(s, i)` ran 63% SLOWER
             * than the loop it replaced. */
            if (i->op == IR_CALL) { ok = 0; break; }
            nbody++;
            int t = def_target(i);
            if (t < 0 || t >= fn->nvregs || d->cnt[t] != 1)
                continue;
            /* A value the body computes and something AFTER the loop
             * reads would, once copied, be the wrong copy's. Values the
             * loop CARRIES are multiply-assigned, and the block ends by
             * copying the last version back into them, so those are
             * fine; a singly-assigned one must not leave the loop. */
            for (int m = 0; m < fn->nins && ok; m++)
                if ((m < L->lo || m >= L->hi) && ins_reads(&fn->ins[m], t))
                    ok = 0;
            /* And it must not be READ before it is written inside the
             * loop either: that is a value carried across the back edge
             * without a copy to carry it, which the renaming below would
             * silently turn into this iteration's. */
            for (int m = L->lo; m < n && ok; m++)
                if (ins_reads(&fn->ins[m], t))
                    ok = 0;
        }
        if (!ok || nbody == 0 || nbody > UNROLL_MAX_BODY)
            continue;
        return nbody;
    }
    return 0;
}

/* each_label's callback for "does anything name this label". */
struct unr_lblctx { int label, hit; };
static void unr_lbl_cb(int *p, void *ctx)
{
    struct unr_lblctx *c = ctx;
    if (*p == c->label) c->hit = 1;
}

/* The loop's trip count when it is a constant, else 0.
 *
 * Constant means the induction variable's ONE definition outside the
 * loop sits in the block that falls into the header, nothing but the
 * back edge jumps to the header -- so every entry passes that definition
 * -- and it and the bound are a pair whose distance is known:
 *
 *   iv = const c0;  ...  iv < const B       B - c0 iterations (step 1)
 *   iv = X;         ...  iv != X + C        C / step  (strength reduction's
 *   iv = X + c1;    ...  iv != X + c2       (c2 - c1) / step   pointer walk)
 *
 * A rotated loop runs its body before its first test, so the count is
 * exactly how many times the body runs on every entry. */
static long unr_trip(struct ir_func *fn, struct defs *d, const struct unrloop *L)
{
    for (int m = 0; m < fn->nins; m++) {
        if (m == L->hi - 1)
            continue;                       /* the back edge itself */
        struct unr_lblctx lc = { L->Lh, 0 };
        if (fn->ins[m].op == IR_LABELADDR && fn->ins[m].label == L->Lh)
            return 0;
        each_label(fn, &fn->ins[m], unr_lbl_cb, &lc);
        if (lc.hit)
            return 0;
    }
    int od = -1, nout = 0;
    for (int m = 0; m < fn->nins; m++)
        if ((m < L->lo || m >= L->hi) && def_target(&fn->ins[m]) == L->iv) {
            od = m;
            nout++;
        }
    if (nout != 1 || od < 0 || od >= L->lo)
        return 0;
    /* od falls into the header: no label between them, and nothing that
     * leaves except a branch that skips the loop altogether */
    for (int m = od + 1; m < L->lo; m++) {
        enum ir_op o = fn->ins[m].op;
        if (o == IR_LABEL || o == IR_JMP || o == IR_SWITCH || o == IR_RET ||
            o == IR_IGOTO || o == IR_UD2 || o == IR_ASM || o == IR_LANDING)
            return 0;
    }
    const struct ir_ins *e = &fn->ins[od];
    int bn = d->cnt[L->bound] == 1 ? d->ins[L->bound] : -1;
    if (bn < 0 || e->w != L->w)
        return 0;
    const struct ir_ins *b = &fn->ins[bn];
    const struct ir_ins *cmp = &fn->ins[L->cmp_ins];
    long t = 0;
    if (cmp->pred == B_LT) {
        long c0, B;
        if (e->op == IR_CONST && !e->flt) c0 = e->imm;
        else if (!(e->op == IR_MOV && get_const(fn, d, e->a, &c0))) return 0;
        if (!get_const(fn, d, L->bound, &B))
            return 0;
        if (L->w == 4) { c0 = (long)(int)c0; B = (long)(int)B; }
        t = B - c0;                          /* step is 1 for `<` */
    } else {
        /* the start and the bound off one base X */
        int x0; long c1 = 0, c2;
        if (e->op == IR_MOV) x0 = e->a;
        else if (!(e->op == IR_ADD && as_op_const(fn, d, (struct ir_ins *)e, &x0, &c1)))
            return 0;
        int x1;
        if (b->op != IR_ADD || b->w != L->w ||
            !as_op_const(fn, d, (struct ir_ins *)b, &x1, &c2))
            return 0;
        /* Either side may name its base through one more constant
         * add -- the preheader's `iv = mov X` with `X = a + 8` against a
         * bound of `a + 40` -- so each is followed back a step. */
        for (int k = 0; k < 2 && x0 != x1; k++) {
            int *xs = k ? &x1 : &x0;
            long *cs = k ? &c2 : &c1;
            int y; long cy;
            if (*xs < 0 || *xs >= fn->nvregs || d->cnt[*xs] != 1 ||
                d->ins[*xs] < 0)
                continue;
            struct ir_ins *a = &fn->ins[d->ins[*xs]];
            if (a->op == IR_ADD && a->w == L->w &&
                as_op_const(fn, d, a, &y, &cy)) {
                *xs = y;
                *cs += cy;
            }
        }
        if (x0 < 0 || x0 != x1 || x0 >= fn->nvregs || d->cnt[x0] != 1)
            return 0;
        long dist = c2 - c1;
        if (L->w == 4) dist = (long)(int)dist;
        if (dist <= 0 || dist % L->step)
            return 0;
        t = dist / L->step;
    }
    return t > 0 ? t : 0;
}

/* unr_local_cb: a read of v at instruction `at`. v stays the body's own
 * only while every read of it is inside the body and after its def. */
struct unr_local { char *local; const struct defs *d; int at, end, nv; };
static void unr_local_cb(int *p, void *ctx)
{
    struct unr_local *c = ctx;
    int v = *p;
    if (v < 0 || v >= c->nv || !c->local[v])
        return;
    if (!(c->at > c->d->ins[v] && c->at < c->end))
        c->local[v] = 0;
}

/* The U copies of the body, appended to nb. cur[v] is the name v
 * currently goes by inside this block, or -1 for "still itself". One
 * running map across every copy, so copy c reads what copy c-1 wrote --
 * and the LAST copy writes the original names, so the block leaves every
 * value exactly where the loop's own body would have, with no restoring
 * moves at the end. */
static void unr_copies(struct ir_func *fn, struct ibuf *nb,
                       const struct unrloop *L, long U, const char *local,
                       int nvr)
{
    int *cur = xmalloc((size_t)(nvr ? nvr : 1) * sizeof *cur);
    for (int v = 0; v < nvr; v++)
        cur[v] = -1;
    for (long c = 0; c < U; c++)
        for (int n2 = L->body_lo; n2 < L->cmp_ins; n2++) {
            if (fn->ins[n2].op == IR_LABEL)
                continue;             /* unreachable; see unr_find */
            int t = def_target(&fn->ins[n2]);
            struct ir_ins *q = ib_push(nb);
            *q = fn->ins[n2];
            struct lcopy lc = { cur, nvr, 0 };
            each_read(q, lcopy_cb, &lc);
            /* A TEMP is renamed. A frame slot -- the dst of an stvar, a
             * local that stays in memory because its address is taken --
             * is not a value to rename but a place: every copy has to
             * write that one place, or a read of it through its address
             * sees none of the copies' stores. Renamed, `stvar v2` became
             * `stvar v98`, a slot that does not exist, and loading `*ps`
             * where ps = &s gave the value from before the loop. */
            if (t >= fn->nvars && t < nvr) {
                if (c == U - 1 && !local[t]) { q->dst = t; cur[t] = -1; }
                else { q->dst = fn->nvregs++; cur[t] = q->dst; }
            }
        }
    free(cur);
}

static int unroll_one(struct ir_func *fn, char *seen, int nseen)
{
    if (fn->nins == 0)
        return 0;
    int nbb, *l2b;
    struct bb *bb = build_cfg(fn, &nbb, &l2b);
    int *order = xmalloc((size_t)(nbb ? nbb : 1) * sizeof *order), norder;
    compute_rpo(bb, nbb, order, &norder);
    if (norder != nbb) {           /* unreachable blocks: do not mis-dominate */
        free(order); free(l2b);
        free_cfg(bb, nbb);
        return 0;
    }
    compute_idom(bb, order, norder);
    struct defs d;
    compute_defs(fn, &d);
    char *in = xmalloc((size_t)(nbb ? nbb : 1));
    struct unrloop L;
    int nbody = unr_find(fn, bb, nbb, order, norder, &d, in, &L,
                         seen, nseen);
    free(in);
    if (!nbody) {
        free_defs(&d); free(order); free(l2b);
        free_cfg(bb, nbb);
        return 0;
    }

    /* A constant trip count small enough to copy whole: the loop goes,
     * its test, its branch and the remainder machinery with it (see
     * unr_trip). The compare must have no reader past the loop, since it
     * goes too. */
    long T = getenv("EMBCC_NO_FULLUNROLL") ? 0 : unr_trip(fn, &d, &L);
    if (T < 2 || T > UNROLL_FULL_TRIP || T * nbody > UNROLL_FULL_BODY)
        T = 0;
    for (int m = 0; T && m < fn->nins; m++)
        if ((m < L.lo || m >= L.hi) &&
            ins_reads(&fn->ins[m], fn->ins[L.cmp_ins].dst))
            T = 0;
    /* Nor a body that calls the runtime -- a soft-float `double`, a
     * 64-bit divide -- which unr_find's IR_CALL test does not see. The
     * test and the branch saved are nothing beside the calls, and
     * lgamma's eight Lanczos terms grew by 288 bytes on Thumb for it. */
    for (int m = L.body_lo; T && m < L.cmp_ins; m++)
        if (target_op_calls_helper(&fn->ins[m]))
            T = 0;
    /* How many copies: the most, a power of two up to eight, that keep
     * U * body within the budget. A short body pays most of its cost on
     * the test and the branch, so it gets more of them; a long one
     * already amortises them and would only grow the function.
     *
     * The budget was 32 (a table: 8 copies to 4 instructions, 4 to 8, 2
     * to 16), which gave a CRC's ten-instruction body two copies and a
     * test every other byte. At 96 it gets eight: the workload's crc ran
     * 8% (RV32) to 17% (M4, x86-64) fewer instructions, the matrix 7-8%,
     * nothing ran more, and -O2 code grew 2.4-3.9%. -Os does not unroll.
     * More than eight copies is worse, not better: the leftover
     * iterations, up to U - 1 of them, run in the original loop, and
     * sixteen copies of the matrix's 24-trip loop left a third of the
     * work there (+7% to +14%). */
    int U = 1;
    if (T)
        U = (int)T;
    else
        while (U * 2 <= UNROLL_MAX_COPIES && U * 2 * nbody <= UNROLL_BUDGET)
            U *= 2;
    if (U < 2) {
        free_defs(&d); free(order); free(l2b);
        free_cfg(bb, nbb);
        return 0;
    }

    /* Every value the body computes is renamed, copy by copy, so the
     * copies are straight-line SSA that folding and value numbering can
     * see through. Without it each copy would end by writing the loop's
     * carried temp and the next would read it back -- a dependency chain
     * that exists only because two copies share a name. */
    int nvr = fn->nvregs;
    /* The last copy writes the original names, so that the block leaves
     * every value where the loop's own body would -- but only the values
     * that outlive the body need that: the ones the loop carries, tests,
     * or reads after it. A temp that dies inside the body gets a fresh
     * name in the last copy as in every other. Otherwise its original
     * name is written twice, by that copy and by the remainder loop, and
     * the backends' single-use fusions (`ldr [xn, xm, lsl #2]` is a
     * shift, an add and a load with one use each) refuse it in both:
     * `bounds` in tests/bench ran 5% MORE instructions unrolled. */
    char *local = xmalloc((size_t)(nvr ? nvr : 1));
    for (int v = 0; v < nvr; v++)
        local[v] = d.cnt[v] == 1 && d.ins[v] >= L.body_lo &&
                   d.ins[v] < L.cmp_ins;
    for (int m = 0; m < fn->nins; m++) {
        struct unr_local ul = { local, &d, m, L.cmp_ins, nvr };
        each_read(&fn->ins[m], unr_local_cb, &ul);
    }
    int Lunroll = fn->nlabels++;
    int Lexit = fn->nlabels++;
    int lineh = fn->ins[L.lo].line, colh = fn->ins[L.lo].col;
    int brw = fn->ins[L.hi - 1].w, brs = fn->ins[L.hi - 1].sign;

    struct ibuf nb = { 0, 0, 0 };
    int *newpos = fn->var_scope_lo
        ? xmalloc((size_t)(fn->nins + 1) * sizeof *newpos) : NULL;
    int cstart = 0, cend = 0;           /* where a full unroll's copies are */
    for (int n = 0; n < fn->nins; n++) {
        /* Recorded BEFORE the insertions: a local whose scope began at
         * the header must cover the copies too, or coalesce_locals is
         * free to give its slot to something that overlaps them. */
        if (newpos) newpos[n] = nb.n;
        if (T) {
            /* the header's label, then the copies in place of the loop:
             * its body, its compare and its back edge */
            if (n > L.lo && n < L.hi)
                continue;
            *ib_push(&nb) = fn->ins[n];
            if (n == L.lo) {
                cstart = nb.n;
                unr_copies(fn, &nb, &L, U, local, nvr);
                cend = nb.n;
                free(local);
            }
            continue;
        }
        if (n == L.lo) {
            struct ir_ins *p;
            int kU = fn->nvregs++;
            /* ---- once, on the way in: is there a full run to do? ----
             *
             * lim = bound - U*step, and the copies run while iv <= lim:
             * then iv + U*step <= bound, so every one of the U
             * iterations is one the loop would have run. That holds only
             * if the subtraction did not wrap, which `lim < bound` tests
             * (U*step is positive), at the loop's own signedness -- a
             * signed `iv < bound` loop, or unsigned for `iv != bound`,
             * whose walk is upward to its end. Both tests are paid once;
             * each run of copies then re-tests with one compare, where
             * re-deriving the count (`bound - iv >= U*step`) took a
             * subtract and a compare -- four instructions with x86's
             * two-operand subtract. A loop that fails either test, an
             * `iv != bound` walk that wraps included, runs as the
             * ORIGINAL loop, not the exit: the loop is a rotated one, a
             * do-while, and what it does when entered is what it always
             * did. The subtraction is the machine's, wrapping: that is
             * what the `lim < bound` test reads, and why nothing here may
             * assume a signed difference cannot overflow. */
            int ls = fn->ins[L.cmp_ins].pred == B_LT
                     ? fn->ins[L.cmp_ins].sign : 0;
            int lim = fn->nvregs++, e0 = fn->nvregs++, e1 = fn->nvregs++;
            p = unr_emit(&nb, IR_CONST, lineh, colh);
            p->dst = kU; p->imm = U * L.step; p->w = L.w;   /* U iterations' worth */
            p = unr_emit(&nb, IR_SUB, lineh, colh);
            p->dst = lim; p->a = L.bound; p->b = kU; p->w = L.w; p->sign = ls;
            p = unr_emit(&nb, IR_CMP, lineh, colh);
            p->dst = e0; p->a = lim; p->b = L.bound;
            p->pred = B_LT; p->w = L.w; p->sign = ls;
            p = unr_emit(&nb, IR_BRZ, lineh, colh);
            p->a = e0; p->label = L.Lh; p->w = brw; p->sign = brs;
            p = unr_emit(&nb, IR_CMP, lineh, colh);
            p->dst = e1; p->a = L.iv; p->b = lim;
            p->pred = B_LE; p->w = L.w; p->sign = ls;
            p = unr_emit(&nb, IR_BRZ, lineh, colh);
            p->a = e1; p->label = L.Lh; p->w = brw; p->sign = brs;

            /* ---- the copies ---- */
            p = unr_emit(&nb, IR_LABEL, lineh, colh);
            p->label = Lunroll;
            unr_copies(fn, &nb, &L, U, local, nvr);
            free(local);

            /* ---- the back edge: a compare and a branch per U iterations ----
             *
             * iv only grew by U*step from a value <= lim, so it is <=
             * bound and the same compare stays exact. */
            int e2 = fn->nvregs++;
            p = unr_emit(&nb, IR_CMP, lineh, colh);
            p->dst = e2; p->a = L.iv; p->b = lim;
            p->pred = B_LE; p->w = L.w; p->sign = ls;
            p = unr_emit(&nb, IR_BRNZ, lineh, colh);
            p->a = e2; p->label = Lunroll; p->w = brw; p->sign = brs;

            /* Out of full runs. iv <= bound, so anything left is a
             * partial one -- which is what the original loop below is. */
            int c2 = fn->nvregs++;
            p = ib_push(&nb);
            *p = fn->ins[L.cmp_ins];
            p->dst = c2;
            p = unr_emit(&nb, IR_BRNZ, lineh, colh);
            p->a = c2; p->label = L.Lh; p->w = brw; p->sign = brs;
            p = unr_emit(&nb, IR_JMP, lineh, colh);
            p->label = Lexit;
        }
        if (n == L.hi) {
            struct ir_ins *p = ib_push(&nb);
            p->op = IR_LABEL; p->label = Lexit;
            p->line = lineh; p->col = colh; p->synth = 1;
        }
        *ib_push(&nb) = fn->ins[n];
    }
    if (!T && L.hi == fn->nins) {      /* the loop ends the function */
        struct ir_ins *p = ib_push(&nb);
        p->op = IR_LABEL; p->label = Lexit;
        p->line = lineh; p->col = colh; p->synth = 1;
    }
    if (newpos) {
        newpos[fn->nins] = nb.n;
        for (int v = 0; v < fn->nvars; v++) {
            int lo = fn->var_scope_lo[v], hi = fn->var_scope_hi[v];
            /* A full unroll deleted the loop's own instructions, and a
             * scope that began or ended among them now covers all of the
             * copies that replaced them: from the first to past the
             * last. */
            if (T && lo > L.lo && lo < L.hi) fn->var_scope_lo[v] = cstart;
            else if (lo >= 0 && lo <= fn->nins) fn->var_scope_lo[v] = newpos[lo];
            if (T && hi > L.lo && hi < L.hi) fn->var_scope_hi[v] = cend;
            else if (hi >= 0 && hi <= fn->nins) fn->var_scope_hi[v] = newpos[hi];
        }
        free(newpos);
    }
    free(fn->ins);
    fn->ins = nb.p; fn->nins = nb.n; fn->cap = nb.cap;
    free_defs(&d); free(order); free(l2b);
    free_cfg(bb, nbb);
    if (L.Lh >= 0 && L.Lh < nseen)
        seen[L.Lh] = 1;
    if (remarks_on() && fn->src && T)
        remark_add("unroll", "unrolled", fn->name, "constant-trip-count",
                   fn->file, lineh ? lineh : fn->line,
                   "all %d iterations of a %d-instruction body, no loop "
                   "left", U, nbody);
    else if (remarks_on() && fn->src)
        remark_add("unroll", "unrolled", fn->name, "counted-loop",
                   fn->file, lineh ? lineh : fn->line,
                   "%d copies of a %d-instruction body, the original kept "
                   "for the remainder", U, nbody);
    return 1;
}

static int pass_unroll(struct ir_func *fn)
{
    /* One pass over each loop. The unrolled block is a loop too -- it
     * ends in a jump back to its own label -- but its latch is a JUMP
     * and the recognizer wants a conditional back edge, so it does not
     * match; the ORIGINAL loop still does, and `seen` is what stops it
     * being unrolled into its own remainder over and over. */
    int cap = fn->nlabels + 64;
    char *seen = xcalloc((size_t)cap, 1);
    int changed = 0, guard = 0;
    while (guard++ < 16 && fn->nlabels + 2 <= cap &&
           unroll_one(fn, seen, cap))
        changed = 1;
    free(seen);
    return changed;
}

/* ==== switch threading =====================================================
 *
 * A state machine is a loop around `switch (state)` whose arms set the
 * state to a constant: CoreMark's core_state, a hand-written lexer, a
 * protocol parser. Every arm knows which case the next iteration will
 * take, and the loop forgets it at the latch, so each character pays for
 * the whole dispatch again -- a bounds check, a table load and an
 * indirect jump.
 *
 * This keeps what the arm knew. The path from the latch to the switch --
 * the increment, the loop's own exit tests, anything else between them --
 * is copied once for each case the state can arrive at, and the copy ends
 * in a jump straight to that case instead of in the switch. An arm that
 * leaves the state alone counts as well: the case edge it was entered by
 * says what the state is. Every copy keeps the loop's exits, and writes
 * the state as the original did, for whatever reads it after the loop.
 * An arrival nothing is proven about still takes the original path, which
 * stays.
 *
 * What is known where is a forward dataflow over the few temps the
 * switch's operand is computed from -- copies, constants and arithmetic
 * with a constant, at the switch's own width -- to which a switch's case
 * edge adds the operand's value.
 *
 * Arms seldom end at the latch itself: an if/else inside one ends at a
 * join, which is where `state = x` meets `state = y`, and that block knows
 * nothing. So a block with one way out (a "latch" here) is copied too, and
 * its predecessors asked instead, up to SWT_FEED_DEPTH deep. Each block is
 * copied once per CASE, not once per way of reaching it: the copy of a
 * latch for case c jumps on to the copy of the next block for case c, and
 * all of them to one copy of the path for c. Sharing is sound because
 * every way into a copy for c is an arrival proven to reach c. */

#define SWT_MAX_TRACK  32    /* temps followed back from the operand */
#define SWT_MAX_PATH   24    /* instructions on the path to the switch */
#define SWT_MAX_FEED   8     /* in one latch block copied in front of it */
#define SWT_FEED_DEPTH 6     /* how many latches deep */
#define SWT_MAX_COPIES 48    /* copies of blocks, per switch */
#define SWT_MAX_ADDED  400   /* instructions, per function, over every round */

/* 0: nothing known yet (unreached), 1: the constant k, 2: varies. */
struct swv { int st; long k; };

static struct swv swt_val(const int *tix, const struct swv *s, int v)
{
    struct swv r = { 2, 0 };
    if (v >= 0 && tix[v] >= 0)
        r = s[tix[v]];
    return r;
}

/* The value instruction i leaves in its destination, given what is known
 * before it. Only arithmetic at the switch's own width is followed, so
 * the low w bytes -- all the switch reads -- are exact whatever a backend
 * keeps above them; everything is normalised the way pass_fold does it. */
static struct swv swt_eval(struct ir_func *fn, struct defs *d,
                           const struct ir_ins *i, const int *tix,
                           const struct swv *s, int w)
{
    struct swv r = { 2, 0 }, a, b;
    long B;
    if (i->flt || i->w != w)
        return r;
    switch (i->op) {
    case IR_CONST:
        r.st = 1;
        r.k = norm(i->imm, w);
        return r;
    case IR_MOV:
        a = swt_val(tix, s, i->a);
        if (a.st == 1)
            a.k = norm(a.k, w);
        return a;
    case IR_EXT:
        a = swt_val(tix, s, i->a);
        if (a.st == 1)
            a.k = fold_ext(a.k, i->size, i->sign, w);
        return a;
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_AND: case IR_OR:
    case IR_XOR: case IR_SHL: case IR_SHR:
        a = swt_val(tix, s, i->a);
        if (a.st != 1)
            return a;
        if (i->imm_b) {
            b.st = 1; b.k = i->imm;
        } else if (get_const(fn, d, i->b, &B)) {
            b.st = 1; b.k = B;
        } else {
            b = swt_val(tix, s, i->b);
        }
        if (b.st != 1)
            return b;
        if (fold_bin(i->op, a.k, b.k, w, i->sign, B_EQ, &r.k))
            r.st = 1;
        return r;
    default:
        return r;
    }
}

/* Run instructions [lo, hi) over the state s. */
static void swt_run(struct ir_func *fn, struct defs *d, int lo, int hi,
                    const int *tix, struct swv *s, int w)
{
    for (int n = lo; n < hi; n++) {
        int t = def_target(&fn->ins[n]);
        if (t >= 0 && tix[t] >= 0)
            s[tix[t]] = swt_eval(fn, d, &fn->ins[n], tix, s, w);
    }
}

/* What the edge from block p to block t adds to s: a switch's case edge
 * names its operand's value when that case is the only way along it --
 * and, when the operand is `v - c` computed in that block, v's too. */
static void swt_edge(struct ir_func *fn, struct defs *d, const struct bb *bb,
                     int p, int t, const int *l2b, const int *tix,
                     struct swv *s, int w)
{
    if (bb[p].end <= bb[p].start)
        return;
    const struct ir_ins *sw = &fn->ins[bb[p].end - 1];
    if (sw->op != IR_SWITCH || sw->w != w || sw->a < 0 || tix[sw->a] < 0)
        return;
    if (sw->label >= 0 && sw->label < fn->nlabels && l2b[sw->label] == t)
        return;                                   /* the default goes there too */
    const struct ir_jt *jt = &fn->jt[sw->jt];
    int idx = -1, hits = 0;
    for (int k = 0; k < jt->n; k++)
        if (jt->labels[k] >= 0 && jt->labels[k] < fn->nlabels &&
            l2b[jt->labels[k]] == t) {
            idx = k;
            hits++;
        }
    if (hits != 1)
        return;
    s[tix[sw->a]].st = 1;
    s[tix[sw->a]].k = norm(idx, w);
    int x = sw->a, at = d->ins[x];
    if (d->cnt[x] != 1 || at < bb[p].start || at >= bb[p].end - 1)
        return;
    const struct ir_ins *sub = &fn->ins[at];
    long c, v;
    if (sub->op != IR_SUB || sub->flt || sub->w != w || sub->a < 0 ||
        tix[sub->a] < 0 || !const_b(fn, d, (struct ir_ins *)sub, &c))
        return;
    for (int n = at + 1; n < bb[p].end - 1; n++)
        if (def_target(&fn->ins[n]) == sub->a)
            return;
    if (fold_bin(IR_ADD, idx, c, w, 0, B_EQ, &v)) {
        s[tix[sub->a]].st = 1;
        s[tix[sub->a]].k = v;
    }
}

static int swt_meet(struct swv *x, struct swv y)
{
    if (y.st == 0 || x->st == 2)
        return 0;
    if (x->st == 0) { *x = y; return 1; }
    if (y.st == 2 || y.k != x->k) { x->st = 2; x->k = 0; return 1; }
    return 0;
}

/* Can this block be copied: nothing that is not plain code. */
static int swt_copyable(struct ir_func *fn, const struct bb *b, int *count)
{
    for (int n = b->start; n < b->end; n++) {
        switch (fn->ins[n].op) {
        case IR_LABEL:
            continue;
        case IR_ASM: case IR_ALLOCA: case IR_SPSAVE: case IR_SPRESTORE:
        case IR_LANDING: case IR_VA_START: case IR_LABELADDR: case IR_IGOTO:
        case IR_RET: case IR_UD2:
            return 0;
        case IR_SWITCH:
            if (n != b->end - 1)
                return 0;
            break;
        default:
            break;
        }
        (*count)++;
    }
    return 1;
}

static int swt_term(struct ir_func *fn, const struct bb *b)
{
    return b->end > b->start ? (int)fn->ins[b->end - 1].op : -1;
}

/* Does block b fall through into the block after it? */
static int swt_falls(struct ir_func *fn, const struct bb *b)
{
    int op = swt_term(fn, b);
    return op != IR_JMP && op != IR_RET && op != IR_UD2 &&
           op != IR_SWITCH && op != IR_IGOTO;
}

/* An arrival: block p reaches block h (the path's head, or a latch) and
 * is sent to copy g instead. A copy: block blk (-1: the whole path to the
 * switch) for the case whose label is target. */
struct swt_arr { int p, h, g; };
struct swt_grp { int blk, target, label, host, emitted; };

/* Everything the arrival search reads, and what it collects. */
struct swt_ctx {
    struct ir_func *fn;
    struct defs *d;
    const struct bb *bb;
    const int *l2b, *tix;
    int nt, w;
    const struct swv *out;              /* per block, nt each */
    const int *chain;
    int nch, head, sb;
    const struct ir_ins *sw;
    const int *bsize;                   /* a block's instructions, -1: not copyable */
    struct swt_grp grp[SWT_MAX_COPIES];
    int ngrp;
    struct swt_arr arr[128];
    int narr, npath;
    int *budget;
};

/* May block p be sent somewhere else: reachable, off the path, and
 * ending in something that names its targets one by one. */
static int swt_from(const struct swt_ctx *c, int p)
{
    for (int k = 0; k < c->nch; k++)
        if (c->chain[k] == p)
            return 0;
    int op = swt_term(c->fn, &c->bb[p]);
    return c->bb[p].rpo >= 0 && op != IR_SWITCH && op != IR_IGOTO;
}

static int swt_find(const struct swt_ctx *c, int blk, int target)
{
    for (int g = 0; g < c->ngrp; g++)
        if (c->grp[g].blk == blk && c->grp[g].target == target)
            return g;
    return -1;
}

/* The copy a latch's copy leads on to. */
static int swt_next(const struct swt_ctx *c, int g)
{
    int nb2 = c->bb[c->grp[g].blk].succ[0];
    return swt_find(c, nb2 == c->head ? -1 : nb2, c->grp[g].target);
}

/* Block p reaching block h: if what p knows at its end, carried through
 * the latches from h (none when h is the head) and the path, decides the
 * switch, record the arrival, and the copies it runs through. */
static int swt_arrive(struct swt_ctx *c, int p, int h)
{
    struct ir_func *fn = c->fn;
    const struct bb *bb = c->bb;
    struct swv s[SWT_MAX_TRACK];
    memcpy(s, &c->out[(size_t)p * c->nt], (size_t)c->nt * sizeof *s);
    swt_edge(fn, c->d, bb, p, h, c->l2b, c->tix, s, c->w);
    for (int b = h; b != c->head; b = bb[b].succ[0])
        swt_run(fn, c->d, bb[b].start, bb[b].end, c->tix, s, c->w);
    for (int k = 0; k < c->nch; k++)
        swt_run(fn, c->d, bb[c->chain[k]].start,
                k == c->nch - 1 ? bb[c->sb].end - 1 : bb[c->chain[k]].end,
                c->tix, s, c->w);
    struct swv xv = s[c->tix[c->sw->a]];
    if (xv.st != 1)
        return 0;
    const struct ir_jt *jt = &fn->jt[c->sw->jt];
    unsigned long idx = c->w == 8 ? (unsigned long)xv.k
                                  : (unsigned long)(unsigned int)xv.k;
    int target = idx < (unsigned long)jt->n ? jt->labels[idx] : c->sw->label;
    if (target < 0 || target >= fn->nlabels || c->l2b[target] < 0 ||
        c->narr == 128)
        return 0;
    /* the copies this needs that do not exist yet, and what they cost */
    int need = 0, cost = 0;
    for (int b = h; ; b = bb[b].succ[0]) {
        int blk = b == c->head ? -1 : b;
        if (swt_find(c, blk, target) < 0) {
            need++;
            cost += 1 + (blk < 0 ? c->npath : c->bsize[blk]);
        }
        if (blk < 0)
            break;
    }
    if (c->ngrp + need > SWT_MAX_COPIES || cost > *c->budget)
        return 0;
    *c->budget -= cost;
    for (int b = h; ; b = bb[b].succ[0]) {
        int blk = b == c->head ? -1 : b;
        if (swt_find(c, blk, target) < 0) {
            struct swt_grp *g = &c->grp[c->ngrp++];
            g->blk = blk;
            g->target = target;
            g->label = fn->nlabels++;
            g->host = -1;
            g->emitted = 0;
        }
        if (blk < 0)
            break;
    }
    c->arr[c->narr].p = p;
    c->arr[c->narr].h = h;
    c->arr[c->narr].g = swt_find(c, h == c->head ? -1 : h, target);
    c->narr++;
    return 1;
}

/* swt_local_cb: a read of v at position `pos` of the copied run. */
struct swt_lc { char *local; const int *defpos; int pos, nv; };
static void swt_local_cb(int *p, void *ctx)
{
    struct swt_lc *c = ctx;
    int v = *p;
    if (v < 0 || v >= c->nv || !c->local[v])
        return;
    if (c->pos < 0 || c->pos <= c->defpos[v])
        c->local[v] = 0;
}

/* Emit copy g, and after it every copy it leads on to that is not out
 * yet, so that each falls into the next. A temp is renamed when the
 * copy's own run of blocks is the only place it is defined and read;
 * one that crosses into the next copy keeps its name, because that copy
 * may be entered from others too. */
struct swt_emit {
    struct swt_ctx *c;
    struct ibuf *nb;
    const int *zlabel, *blabel;
    int nv;
    int *pos, *defpos, *ren;
    char *local;
};
static void swt_emit_copy(struct swt_emit *e, int g)
{
    struct swt_ctx *c = e->c;
    struct ir_func *fn = c->fn;
    const struct bb *bb = c->bb;
    while (g >= 0 && !c->grp[g].emitted) {
        struct swt_grp *G = &c->grp[g];
        G->emitted = 1;
        int run[64], nrun = 0;
        if (G->blk >= 0) {
            run[nrun++] = G->blk;
        } else {
            for (int k = 0; k < c->nch; k++)
                run[nrun++] = c->chain[k];
        }
        for (int n = 0; n < fn->nins; n++)
            e->pos[n] = -1;
        int ps = 0;
        for (int k = 0; k < nrun; k++)
            for (int n = bb[run[k]].start; n < bb[run[k]].end; n++)
                e->pos[n] = ps++;
        for (int v = 0; v < e->nv; v++) {
            e->defpos[v] = c->d->cnt[v] == 1 && c->d->ins[v] >= 0
                           ? e->pos[c->d->ins[v]] : -1;
            e->local[v] = v >= fn->nvars && e->defpos[v] >= 0;
            e->ren[v] = -1;
        }
        for (int n = 0; n < fn->nins; n++) {
            struct swt_lc lc = { e->local, e->defpos, e->pos[n], e->nv };
            each_read(&fn->ins[n], swt_local_cb, &lc);
        }
        const struct ir_ins *first = &fn->ins[bb[run[0]].start];
        struct ir_ins *l = ib_push(e->nb);
        l->op = IR_LABEL;
        l->label = G->label;
        l->line = first->line; l->col = first->col; l->synth = 1;
        int last_line = first->line, last_col = first->col;
        for (int k = 0; k < nrun; k++) {
            int b = run[k], lastblk = k == nrun - 1;
            int next = lastblk ? -1 : run[k + 1];
            for (int n = bb[b].start; n < bb[b].end; n++) {
                const struct ir_ins *o = &fn->ins[n];
                int term = n == bb[b].end - 1;
                if (o->op == IR_LABEL)
                    continue;
                last_line = o->line; last_col = o->col;
                if (term && (o->op == IR_SWITCH || o->op == IR_JMP))
                    break;          /* the jump on is emitted below */
                int inv = 0;
                if (term && (o->op == IR_BRZ || o->op == IR_BRNZ)) {
                    if (lastblk)
                        break;      /* a latch: both ways lead on */
                    if (c->l2b[o->label] == next) {
                        if (b + 1 == next)
                            break;  /* both ways lead on */
                        inv = 1;    /* on along the taken edge */
                    }
                }
                struct ir_ins *q = ib_push(e->nb);
                *q = *o;
                struct lcopy lcp = { e->ren, e->nv, 0 };
                each_read(q, lcopy_cb, &lcp);
                int t = def_target(o);
                if (t >= 0 && t < e->nv) {
                    if (e->local[t]) {
                        q->dst = fn->nvregs++;
                        e->ren[t] = q->dst;
                    } else {
                        e->ren[t] = -1;
                    }
                }
                if (inv) {
                    q->op = o->op == IR_BRZ ? IR_BRNZ : IR_BRZ;
                    q->label = e->blabel[b + 1] >= 0 ? e->blabel[b + 1]
                                                     : e->zlabel[b + 1];
                }
            }
        }
        /* on: to the case, or to the copy of the next block for it */
        int next_g = G->blk >= 0 ? swt_next(c, g) : -1;
        struct ir_ins *j = ib_push(e->nb);
        j->op = IR_JMP;
        j->label = next_g >= 0 ? c->grp[next_g].label : G->target;
        j->line = last_line; j->col = last_col; j->synth = 1;
        g = next_g;
    }
}

static int swt_one(struct ir_func *fn, int *budget)
{
    if (fn->nins == 0 || fn->nvregs == 0)
        return 0;
    int nsw = 0;
    for (int n = 0; n < fn->nins; n++) {
        if (fn->ins[n].op == IR_ASM)
            return 0;               /* its outputs are not def_target's */
        nsw += fn->ins[n].op == IR_SWITCH;
    }
    if (!nsw)
        return 0;
    int nbb, *l2b;
    struct bb *bb = build_cfg(fn, &nbb, &l2b);
    int *order = xmalloc((size_t)(nbb ? nbb : 1) * sizeof *order), norder;
    compute_rpo(bb, nbb, order, &norder);
    struct defs d;
    compute_defs(fn, &d);
    int nv = fn->nvregs, done = 0;
    int *tix = xmalloc((size_t)nv * sizeof *tix);
    struct swv *out = NULL;
    struct swt_ctx *c = xcalloc(1, sizeof *c);
    int *bsize = xmalloc((size_t)nbb * sizeof *bsize);
    for (int b = 0; b < nbb; b++) {
        bsize[b] = 0;
        if (!swt_copyable(fn, &bb[b], &bsize[b]))
            bsize[b] = -1;
    }

    for (int sb = 0; sb < nbb && !done; sb++) {
        if (bb[sb].rpo < 0 || swt_term(fn, &bb[sb]) != IR_SWITCH ||
            bsize[sb] < 0)
            continue;
        const struct ir_ins *sw = &fn->ins[bb[sb].end - 1];
        int w = sw->w, x = sw->a, swline = sw->line;
        if ((w != 4 && w != 8) || x < fn->nvars || x >= nv)
            continue;

        /* ---- the path: back from the switch while there is one way in */
        int chain[64], nch = 0, npath = bsize[sb];
        int cur = sb;
        chain[nch++] = sb;
        while (bb[cur].npred == 1 && nch < 64) {
            int p = bb[cur].pred[0], seen = 0;
            for (int k = 0; k < nch; k++)
                seen |= chain[k] == p;
            int op = swt_term(fn, &bb[p]);
            if (seen || bb[p].rpo < 0 || op == IR_SWITCH || op == IR_IGOTO ||
                bb[p].nsucc > 2 || bsize[p] < 0)
                break;
            npath += bsize[p];
            memmove(chain + 1, chain, (size_t)nch * sizeof *chain);
            chain[0] = p;
            nch++;
            cur = p;
        }
        if (npath > SWT_MAX_PATH)
            continue;
        int head = chain[0];

        /* ---- the temps the operand is computed from ---- */
        int nt = 0;
        for (int v = 0; v < nv; v++)
            tix[v] = -1;
        tix[x] = nt++;
        for (int grew = 1; grew; ) {
            grew = 0;
            for (int n = 0; n < fn->nins; n++) {
                const struct ir_ins *i = &fn->ins[n];
                int t = def_target(i), src[2] = { -1, -1 };
                if (t < 0 || tix[t] < 0 || i->flt || i->w != w)
                    continue;
                switch (i->op) {
                case IR_MOV: case IR_EXT:
                    src[0] = i->a;
                    break;
                case IR_ADD: case IR_SUB: case IR_MUL: case IR_AND:
                case IR_OR: case IR_XOR: case IR_SHL: case IR_SHR:
                    src[0] = i->a;
                    if (!i->imm_b) src[1] = i->b;
                    break;
                default:
                    break;
                }
                for (int k = 0; k < 2; k++) {
                    int v = src[k];
                    if (v >= fn->nvars && v < nv && tix[v] < 0 &&
                        nt < SWT_MAX_TRACK) {
                        tix[v] = nt++;
                        grew = 1;
                    }
                }
            }
        }

        /* ---- what is known at the end of every block ---- */
        free(out);
        out = xcalloc((size_t)nbb * (size_t)nt, sizeof *out);
        struct swv tmp[SWT_MAX_TRACK], nin[SWT_MAX_TRACK];
        for (int again = 1, guard = 0; again && guard++ < 64; ) {
            again = 0;
            for (int oi = 0; oi < norder; oi++) {
                int b = order[oi];
                for (int k = 0; k < nt; k++) {
                    nin[k].st = b == 0 ? 2 : 0;   /* entry: unknown, not unseen */
                    nin[k].k = 0;
                }
                for (int q = 0; b != 0 && q < bb[b].npred; q++) {
                    int p = bb[b].pred[q];
                    if (bb[p].rpo < 0)
                        continue;
                    memcpy(tmp, &out[(size_t)p * nt], (size_t)nt * sizeof *tmp);
                    swt_edge(fn, &d, bb, p, b, l2b, tix, tmp, w);
                    for (int k = 0; k < nt; k++)
                        swt_meet(&nin[k], tmp[k]);
                }
                swt_run(fn, &d, bb[b].start, bb[b].end, tix, nin, w);
                struct swv *o = &out[(size_t)b * nt];
                for (int k = 0; k < nt; k++)
                    if (o[k].st != nin[k].st ||
                        (nin[k].st == 1 && o[k].k != nin[k].k)) {
                        o[k] = nin[k];
                        again = 1;
                    }
            }
        }

        /* ---- the arrivals the analysis proves ----
         *
         * A predecessor of the path's head may know the operand at its
         * end. One that does not may be a latch -- a block with one way
         * out -- whose own predecessors do, so those are asked in turn. */
        memset(c, 0, sizeof *c);
        c->fn = fn; c->d = &d; c->bb = bb; c->l2b = l2b; c->tix = tix;
        c->nt = nt; c->w = w; c->out = out; c->chain = chain; c->nch = nch;
        c->head = head; c->sb = sb; c->sw = sw; c->bsize = bsize;
        c->npath = npath; c->budget = budget;
        int *fdepth = xmalloc((size_t)nbb * sizeof *fdepth);
        for (int b = 0; b < nbb; b++)
            fdepth[b] = -1;
        int wl[64], nwl = 0;
        for (int q = 0; q < bb[head].npred; q++) {
            int Q = bb[head].pred[q];
            if (!swt_from(c, Q) || swt_arrive(c, Q, head))
                continue;
            if (Q != 0 && bb[Q].nsucc == 1 && bb[Q].npred > 0 &&
                bsize[Q] >= 0 && bsize[Q] <= SWT_MAX_FEED &&
                fdepth[Q] < 0 && nwl < 64) {
                fdepth[Q] = 1;
                wl[nwl++] = Q;
            }
        }
        for (int at = 0; at < nwl; at++) {
            int F = wl[at];
            for (int r = 0; r < bb[F].npred; r++) {
                int P = bb[F].pred[r];
                if (!swt_from(c, P) || fdepth[P] >= 0 || P == F ||
                    swt_arrive(c, P, F))
                    continue;
                if (fdepth[F] < SWT_FEED_DEPTH && P != 0 &&
                    bb[P].nsucc == 1 && bb[P].npred > 0 &&
                    bsize[P] >= 0 && bsize[P] <= SWT_MAX_FEED && nwl < 64) {
                    fdepth[P] = fdepth[F] + 1;
                    wl[nwl++] = P;
                }
            }
        }
        free(fdepth);
        if (!c->narr)
            continue;

        /* ---- where each copy goes: after an arrival that can fall into
         * it, the last such in the layout; the copies it leads on to
         * follow it. What has no such arrival goes at the end. ---- */
        char *hosting = xcalloc((size_t)nbb, 1);
        for (int a = 0; a < c->narr; a++) {
            int P = c->arr[a].p, H = c->arr[a].h;
            struct swt_grp *G = &c->grp[c->arr[a].g];
            int canhost = (swt_falls(fn, &bb[P]) && P + 1 == H) ||
                          (swt_term(fn, &bb[P]) == IR_JMP &&
                           bb[P].nsucc == 1 && bb[P].succ[0] == H);
            if (canhost && !hosting[P] && (G->host < 0 || P > G->host)) {
                if (G->host >= 0)
                    hosting[G->host] = 0;
                G->host = P;
                hosting[P] = 1;
            }
        }
        int lastop = fn->ins[fn->nins - 1].op;
        if (lastop != IR_JMP && lastop != IR_RET && lastop != IR_UD2 &&
            lastop != IR_SWITCH && lastop != IR_IGOTO) {
            /* A function that falls off its end: nothing can go after
             * it, so every copy must be placed by a host -- its own, or
             * one leading on to it. */
            int all = 1;
            for (int g = 0; g < c->ngrp; g++) {
                int placed = c->grp[g].host >= 0;
                for (int h = 0; h < c->ngrp && !placed; h++)
                    placed = c->grp[h].blk >= 0 && swt_next(c, h) == g;
                all &= placed;
            }
            if (!all) {
                free(hosting);
                continue;
            }
        }

        /* A block a copy branches to by name, where the original fell
         * into it (a path block that continues along its branch's taken
         * edge, so the copy branches the other way). */
        int *zlabel = xmalloc((size_t)nbb * sizeof *zlabel);
        int *blabel = xmalloc((size_t)nbb * sizeof *blabel);
        for (int b = 0; b < nbb; b++) {
            zlabel[b] = -1;
            blabel[b] = bb[b].end > bb[b].start &&
                        fn->ins[bb[b].start].op == IR_LABEL
                        ? fn->ins[bb[b].start].label : -1;
        }
        for (int k = 0; k + 1 < nch; k++) {
            int cb = chain[k], op = swt_term(fn, &bb[cb]);
            if ((op == IR_BRZ || op == IR_BRNZ) &&
                l2b[fn->ins[bb[cb].end - 1].label] == chain[k + 1] &&
                cb + 1 != chain[k + 1] && blabel[cb + 1] < 0 &&
                zlabel[cb + 1] < 0)
                zlabel[cb + 1] = fn->nlabels++;
        }

        struct ibuf nb = { 0, 0, 0 };
        struct swt_emit e;
        e.c = c; e.nb = &nb; e.zlabel = zlabel; e.blabel = blabel;
        e.nv = nv;
        e.pos = xmalloc((size_t)(fn->nins ? fn->nins : 1) * sizeof *e.pos);
        e.defpos = xmalloc((size_t)nv * sizeof *e.defpos);
        e.ren = xmalloc((size_t)nv * sizeof *e.ren);
        e.local = xmalloc((size_t)nv);
        int *newpos = xmalloc((size_t)(fn->nins + 1) * sizeof *newpos);
        for (int b = 0; b < nbb; b++) {
            if (zlabel[b] >= 0) {
                struct ir_ins *l = ib_push(&nb);
                l->op = IR_LABEL;
                l->label = zlabel[b];
                l->line = fn->ins[bb[b].start].line;
                l->col = fn->ins[bb[b].start].col;
                l->synth = 1;
            }
            for (int n = bb[b].start; n < bb[b].end; n++) {
                newpos[n] = nb.n;
                struct ir_ins *q = ib_push(&nb);
                *q = fn->ins[n];
                if (n != bb[b].end - 1 ||
                    (q->op != IR_JMP && q->op != IR_BRZ && q->op != IR_BRNZ))
                    continue;
                for (int a = 0; a < c->narr; a++)
                    if (c->arr[a].p == b && q->label >= 0 &&
                        q->label < fn->nlabels && l2b[q->label] == c->arr[a].h)
                        q->label = c->grp[c->arr[a].g].label;
            }
            /* an arrival that fell into its block: into the copy now */
            int falls_to = swt_falls(fn, &bb[b]) && b + 1 < nbb ? b + 1 : -1;
            int hosted = -1;
            for (int g = 0; g < c->ngrp; g++)
                if (c->grp[g].host == b)
                    hosted = g;
            for (int a = 0; a < c->narr; a++)
                if (c->arr[a].p == b && c->arr[a].h == falls_to &&
                    c->arr[a].g != hosted) {
                    struct ir_ins *j = ib_push(&nb);
                    j->op = IR_JMP;
                    j->label = c->grp[c->arr[a].g].label;
                    j->line = fn->ins[bb[b].end - 1].line;
                    j->col = fn->ins[bb[b].end - 1].col;
                    j->synth = 1;
                    break;
                }
            if (hosted >= 0)
                swt_emit_copy(&e, hosted);
        }
        for (int g = 0; g < c->ngrp; g++)
            if (!c->grp[g].emitted)
                swt_emit_copy(&e, g);
        newpos[fn->nins] = nb.n;
        remap_scopes(fn, newpos, fn->nins);
        free(newpos);
        free(fn->ins);
        fn->ins = nb.p; fn->nins = nb.n; fn->cap = nb.cap;
        free(e.pos); free(e.defpos); free(e.ren); free(e.local);
        free(zlabel); free(blabel); free(hosting);
        if (remarks_on() && fn->src)
            remark_add("switch-thread", "threaded", fn->name, "state-known",
                       fn->file, swline,
                       "%d arrivals at this switch go straight to their "
                       "case, through %d copied blocks", c->narr, c->ngrp);
        done = 1;
    }
    free(out); free(tix); free(c); free(bsize);
    free_defs(&d); free(order); free(l2b);
    free_cfg(bb, nbb);
    return done;
}

static int pass_swthread(struct ir_func *fn)
{
    int budget = SWT_MAX_ADDED, changed = 0, guard = 0;
    while (guard++ < 8 && budget > 0 && swt_one(fn, &budget))
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
static int sccp_core(struct ir_func *fn, int fold_branches)
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
        if (!fold_branches ||
            (t->op != IR_BRZ && t->op != IR_BRNZ) || t->a < 0 ||
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
        free_cfg(bb, nbb); free_defs(&d);
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
    free_cfg(bb, nbb); free_defs(&d);
    return 1;
}

static int pass_sccp(struct ir_func *fn)
{
    return sccp_core(fn, 1);
}

/* Only the second half: drop the blocks nothing reaches. mem2reg needs
 * it -- its dominators are wrong over a block no path enters, so it
 * refused any function with one -- and irgen leaves them wherever a
 * statement follows a jump: the `break` after a `return` or `continue`,
 * the code after a call to a noreturn function, the end of a loop that
 * never exits. Across lib/libc and EmbLinkOs that was 149 functions --
 * sin, cos, pow and most of fdlibm among them -- whose every local
 * stayed in memory through the whole optimizer. */
static int drop_unreachable(struct ir_func *fn)
{
    return sccp_core(fn, 0);
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

static int pass_divmod(struct ir_func *fn)
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
            /* Never a volatile local: each read of one must happen, and
             * read what is there. `volatile int v = 5; return v + v;`
             * returned 10 with no load at all. */
            /* (big-endian: only an access at the local's own size, as
             * mem2reg asks -- a narrower one is the stored value's HIGH
             * end there, not the low bits the MOV would carry) */
            if (L >= 0 && L < nvars)
                cur[L] = (!taken[L] && !in->vol &&
                          !(fn->locals && fn->locals[L].is_volatile) &&
                          sf_plain(in->size, 0, in->size) &&
                          !(target_big_endian() && fn->locals &&
                            in->size != fn->locals[L].size))
                             ? in->a : -1;
        } else if (in->op == IR_LDVAR) {
            int L = in->a;
            if (L >= 0 && L < nvars && !taken[L] && cur[L] >= 0 && !in->vol &&
                sf_plain(in->size, in->sign, in->w) &&
                !(target_big_endian() && fn->locals &&
                  in->size != fn->locals[L].size)) {
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

/* ---- reading a global nothing writes ------------------------------------
 *
 * `static const double pi = 3.14159...;` and fdlibm's two hundred other
 * constants were each an address and two loads at every use, where a
 * constant is two instructions. The type system here records no `const`,
 * and it would not be enough anyway (a cast can write through it), so
 * this PROVES it instead: a static global of this unit is read-only when
 * every use of its address, in every function, is a load address --
 * directly or at a constant offset -- no initializer anywhere points at
 * it, and no inline or top-level assembly could touch it. Its bytes are
 * then its initializer's, forever, and a load of them is that constant.
 * A float's constant is its bit pattern, which is what every backend
 * takes a float constant to be. Bytes a relocation fills (a pointer in
 * an initializer) are left alone. */
static const struct global **g_ro;
static int g_nro;

static const struct global *ro_find(const struct global *g)
{
    for (int k = 0; g && k < g_nro; k++)
        if (g_ro[k] == g || (g_ro[k]->name && g->name &&
                             strcmp(g_ro[k]->name, g->name) == 0))
            return g_ro[k];
    return NULL;
}

static void ro_globals(struct ir_unit *iu)
{
    struct unit *u = iu->src;
    g_nro = 0;
    free(g_ro);
    g_ro = NULL;
    if (!u || u->topasm)
        return;
    int cap = 0;
    for (struct global *g = u->globals; g; g = g->next) {
        if (!g->is_static || !g->defined || g->absorbed || g->is_tls ||
            g->is_weak || g->section || !g->ty || !g->ty->kind ||
            g->ty->is_volatile)
            continue;
        if (g_nro == cap) {
            cap = cap ? cap * 2 : 16;
            g_ro = xrealloc(g_ro, (size_t)cap * sizeof *g_ro);
        }
        g_ro[g_nro++] = g;
    }
    /* withdraw every one that something could write or see */
    char *out = xcalloc((size_t)(g_nro ? g_nro : 1), 1);
    for (struct global *g = u->globals; g; g = g->next)
        for (int r = 0; r < g->nrelocs; r++)
            for (int k = 0; k < g_nro; k++)
                if (g->relocs[r].gtarget &&
                    (g->relocs[r].gtarget == g_ro[k] ||
                     (g->relocs[r].gtarget->name &&
                      strcmp(g->relocs[r].gtarget->name, g_ro[k]->name) == 0)))
                    out[k] = 1;
    for (int f = 0; f < iu->nfuncs; f++) {
        struct ir_func *fn = &iu->funcs[f];
        int nv = fn->nvregs;
        if (!nv) continue;
        int *gof = xmalloc((size_t)nv * sizeof *gof);   /* vreg -> ro index */
        struct defs d;
        compute_defs(fn, &d);
        for (int v = 0; v < nv; v++) gof[v] = -1;
        for (int n = 0; n < fn->nins; n++) {
            const struct ir_ins *i = &fn->ins[n];
            if (i->op == IR_ASM) {
                memset(out, 1, (size_t)(g_nro ? g_nro : 1));
                break;
            }
            if (i->op == IR_GADDR && i->glob) {
                const struct global *r = ro_find(i->glob);
                if (!r) continue;
                int k;
                for (k = 0; k < g_nro && g_ro[k] != r; k++) ;
                if (i->dst >= 0 && i->dst < nv && d.cnt[i->dst] == 1)
                    gof[i->dst] = k;
                else
                    out[k] = 1;
            } else if (i->op == IR_ADD && i->a >= 0 && i->a < nv &&
                       gof[i->a] >= 0 && i->dst >= 0 && i->dst < nv &&
                       d.cnt[i->dst] == 1) {
                /* Any offset, constant or not: this runs on the IR as
                 * irgen left it, where `table[1]` is still `mul 1, 4`,
                 * and a read at a computed offset is still only a read.
                 * pass_roload folds only the constant ones. */
                gof[i->dst] = gof[i->a];
            }
        }
        for (int n = 0; n < fn->nins; n++) {
            struct ir_ins *i = &fn->ins[n];
            int *ops[3], nops = 0;
            if (i->a >= 0 && i->a < nv && gof[i->a] >= 0) ops[nops++] = &i->a;
            if (!i->imm_b && i->b >= 0 && i->b < nv && gof[i->b] >= 0)
                ops[nops++] = &i->b;
            if (i->c >= 0 && i->c < nv && gof[i->c] >= 0) ops[nops++] = &i->c;
            for (int k = 0; k < nops; k++) {
                int ok = ops[k] == &i->a &&
                         ((i->op == IR_LOAD && !i->vol) ||
                          (i->op == IR_ADD && i->dst >= 0 && gof[i->dst] >= 0));
                if (!ok) out[gof[*ops[k]]] = 1;
            }
            if (i->op == IR_CALL)
                for (int k = 0; k < i->nargs; k++) {
                    int v = i->argv[k].vreg;
                    if (v >= 0 && v < nv && gof[v] >= 0) out[gof[v]] = 1;
                }
            if (i->op == IR_RET && i->a >= 0 && i->a < nv && gof[i->a] >= 0)
                out[gof[i->a]] = 1;
        }
        free(gof); free(d.cnt); free(d.ins);
    }
    int k = 0;
    for (int j = 0; j < g_nro; j++)
        if (!out[j]) g_ro[k++] = g_ro[j];
    g_nro = k;
    free(out);
}

/* The `size` bytes at `off` of read-only global g, or 0 if a relocation
 * fills any of them or they are outside it. */
static int ro_bytes(const struct global *g, long off, int size,
                    unsigned long *out)
{
    int gs = global_size(g);
    unsigned long v = 0;
    if (off < 0 || off + size > gs || size < 1 || size > 8)
        return 0;
    for (int r = 0; r < g->nrelocs; r++) {
        long ro = g->relocs[r].off;
        if (ro < off + size && off < ro + 8)
            return 0;
    }
    /* The bytes are the object's image, in the target's order: the most
     * significant first is the LAST of them little-endian, the first
     * big-endian. A scalar without an image is its value, stored the same
     * way. */
    for (int k = 0; k < size; k++) {
        int b = target_big_endian() ? k : size - 1 - k;
        long at = off + b;
        unsigned char byte;
        if (g->init_bytes)
            byte = at < g->init_len ? (unsigned char)g->init_bytes[at] : 0;
        else
            byte = (unsigned char)(((unsigned long)g->init >>
                                    (8 * (target_big_endian() ? gs - 1 - at
                                                              : at)))
                                   & 0xff);
        v = v << 8 | byte;
    }
    *out = v;
    return 1;
}

static int pass_roload(struct ir_func *fn)
{
    int nv = fn->nvregs, changed = 0;
    if (!g_nro || !nv)
        return 0;
    struct defs d;
    compute_defs(fn, &d);
    const struct global **gv = xcalloc((size_t)nv, sizeof *gv);
    long *goff = xcalloc((size_t)nv, sizeof *goff);
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        long c;
        if (i->dst < 0 || i->dst >= nv || d.cnt[i->dst] != 1)
            continue;
        if (i->op == IR_GADDR && i->glob)
            gv[i->dst] = ro_find(i->glob);
        else if (i->op == IR_ADD && i->a >= 0 && i->a < nv && gv[i->a] &&
                 (i->imm_b ? (c = i->imm, 1)
                           : get_const(fn, &d, i->b, &c))) {
            gv[i->dst] = gv[i->a];
            goff[i->dst] = goff[i->a] + c;
        }
    }
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        unsigned long v;
        if (i->op != IR_LOAD || i->vol || i->a < 0 || i->a >= nv ||
            !gv[i->a] || i->w > 8)
            continue;
        if (!ro_bytes(gv[i->a], goff[i->a], i->size, &v))
            continue;
        to_const(i, fold_ext((long)v, i->size, i->sign, i->w));
        i->flt = 0;
        changed = 1;
    }
    free(gv); free(goff); free(d.cnt); free(d.ins);
    return changed;
}

/* ---- forwarding through a punning union --------------------------------
 *
 * fdlibm reads a double's words through a union -- EXTRACT_WORDS,
 * GET_HIGH_WORD, GET_LOW_WORD -- and every such union stayed a stack
 * object: the double stored, one word loaded back, in nearly every
 * function of the math library. mem2reg cannot promote a union (it is
 * not a scalar) and storefwd will not touch a local whose address is
 * taken.
 *
 * Here a local is PRIVATE when every use of its address is a load or
 * store address, or a memzero's (`union { ... } v = { x }` zeroes v before
 * storing x), directly or at a constant offset -- nothing can see it but
 * these accesses. In a block, a load that one earlier store covers
 * becomes that store's value: a copy or a bit-reinterpret at the same
 * width and offset, and at half the width the low or high word
 * (reinterpret, shift by 32, truncate). A private local left with no load
 * loses its stores, and DCE the addresses.
 *
 * Not on AVR, whose 64-bit shift is a byte loop and whose double is four
 * bytes anyway. */
struct pun_rec { int off, width, val, flt; };

static int pass_punfwd(struct ir_func *fn)
{
    int nv = fn->nvregs, nvars = fn->nvars, changed = 0;
    if (!nvars || !nv || target_get() == TARGET_AVR)
        return 0;
    for (int n = 0; n < fn->nins; n++)
        if (fn->ins[n].op == IR_ASM)
            return 0;
    struct defs d;
    compute_defs(fn, &d);
    int *base = xmalloc((size_t)nv * sizeof *base);
    long *boff = xcalloc((size_t)nv, sizeof *boff);
    char *bad = xcalloc((size_t)nvars, 1);
    for (int v = 0; v < nv; v++) base[v] = -1;
    /* the address of a local, and constant offsets from it */
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        long c;
        if (i->dst < 0 || i->dst >= nv || d.cnt[i->dst] != 1)
            continue;
        if (i->op == IR_ADDR && i->a >= 0 && i->a < nvars) {
            base[i->dst] = i->a;
            boff[i->dst] = 0;
        } else if (i->op == IR_ADD && i->a >= 0 && i->a < nv &&
                   base[i->a] >= 0 &&
                   (i->imm_b ? (c = i->imm, 1)
                             : get_const(fn, &d, i->b, &c))) {
            base[i->dst] = base[i->a];
            boff[i->dst] = boff[i->a] + c;
        }
    }
    /* private: every read of such an address is a load/store address or
     * another offset; and no whole-local access */
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        if ((i->op == IR_LDVAR && i->a >= 0 && i->a < nvars) ||
            (i->op == IR_STVAR && i->dst >= 0 && i->dst < nvars)) {
            bad[i->op == IR_LDVAR ? i->a : i->dst] = 1;
            /* ...and an address STORED in a variable has escaped: it can
             * be read back past a join, where nothing here follows it,
             * and the local was declared unread and its stores dropped
             * (`p = &s; if (c) ...; return p->x;` read garbage at -O1). */
            if (i->op == IR_STVAR && i->a >= 0 && i->a < nv &&
                base[i->a] >= 0)
                bad[base[i->a]] = 1;
            continue;
        }
        int *ops[4], nops = 0;
        if (i->a >= 0 && i->a < nv && base[i->a] >= 0) ops[nops++] = &i->a;
        if (!i->imm_b && i->b >= 0 && i->b < nv && base[i->b] >= 0)
            ops[nops++] = &i->b;
        if (i->c >= 0 && i->c < nv && base[i->c] >= 0) ops[nops++] = &i->c;
        for (int k = 0; k < nops; k++) {
            int ok = ops[k] == &i->a &&
                     (i->op == IR_LOAD || i->op == IR_STORE ||
                      i->op == IR_MEMZERO ||
                      (i->op == IR_ADD && i->dst >= 0 && base[i->dst] >= 0));
            if (!ok)
                bad[base[*ops[k]]] = 1;
        }
        if (i->op == IR_CALL)
            for (int k = 0; k < i->nargs; k++) {
                int v = i->argv[k].vreg;
                if (v >= 0 && v < nv && base[v] >= 0) bad[base[v]] = 1;
            }
        if (i->op == IR_RET && i->a >= 0 && i->a < nv && base[i->a] >= 0)
            bad[base[i->a]] = 1;
    }
    /* forward, a block at a time */
    struct pun_rec (*rec)[4] = xcalloc((size_t)nvars, sizeof *rec);
    int *nrec = xcalloc((size_t)nvars, sizeof *nrec);
    int *plan = xmalloc((size_t)fn->nins * sizeof *plan);  /* rec index */
    struct pun_rec *pr = xmalloc((size_t)fn->nins * sizeof *pr);
    int any = 0;
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        plan[n] = 0;
        if (i->op == IR_LABEL || i->op == IR_CALL) {
            /* a join; and a call cannot touch a private local, but a
             * longjmp-style return into the middle is not worth reasoning
             * about -- start again either way */
            memset(nrec, 0, (size_t)nvars * sizeof *nrec);
            continue;
        }
        if (i->op == IR_MEMZERO && i->a >= 0 && i->a < nv &&
            base[i->a] >= 0 && !bad[base[i->a]]) {
            /* zeroes: whatever it overlaps no longer holds a value */
            int L = base[i->a], off = (int)boff[i->a], wid = i->size, k = 0;
            for (int j = 0; j < nrec[L]; j++) {
                struct pun_rec *r = &rec[L][j];
                if (r->off + r->width <= off || off + wid <= r->off)
                    rec[L][k++] = *r;
            }
            nrec[L] = k;
            continue;
        }
        if ((i->op != IR_LOAD && i->op != IR_STORE) || i->a < 0 ||
            i->a >= nv || base[i->a] < 0 || bad[base[i->a]] || i->vol)
            continue;
        int L = base[i->a], off = (int)boff[i->a], wid = i->size;
        if (i->op == IR_STORE) {
            int k = 0;
            for (int j = 0; j < nrec[L]; j++) {      /* drop what it overlaps */
                struct pun_rec *r = &rec[L][j];
                if (r->off + r->width <= off || off + wid <= r->off)
                    rec[L][k++] = *r;
            }
            nrec[L] = k;
            if (k < 4 && (wid == 4 || wid == 8) && i->b >= 0) {
                rec[L][k].off = off; rec[L][k].width = wid;
                rec[L][k].val = i->b; rec[L][k].flt = i->flt;
                nrec[L] = k + 1;
            }
            continue;
        }
        for (int j = 0; j < nrec[L]; j++) {          /* a LOAD */
            struct pun_rec *r = &rec[L][j];
            if (i->flt && r->width != wid)
                continue;      /* half a double as a float: not handled */
            if ((r->width == wid && r->off == off) ||
                (r->width == 8 && wid == 4 &&
                 (off == r->off || off == r->off + 4))) {
                plan[n] = 1;
                pr[n] = *r;
                any = 1;
                break;
            }
        }
    }
    if (any) {
        struct ibuf nb = { 0, 0, 0 };
        int *newpos = xmalloc((size_t)(fn->nins + 1) * sizeof *newpos);
        for (int n = 0; n < fn->nins; n++) {
            struct ir_ins in = fn->ins[n];
            newpos[n] = nb.n;
            if (!plan[n]) {
                *ib_push(&nb) = in;
                continue;
            }
            const struct pun_rec *r = &pr[n];
            /* the word at +4 of an 8-byte store is its high half
             * little-endian, its low half big-endian */
            int x = r->val,
                hi = ((int)boff[in.a] != r->off) != target_big_endian();
            if (r->width == in.size) {
                struct ir_ins *m = ib_push(&nb);
                *m = in;
                m->op = r->flt == in.flt ? IR_MOV : IR_BITCAST;
                m->a = x; m->b = -1; m->c = -1;
                m->w = in.size; m->size = in.size;
                m->sign = in.flt ? 0 : 1;
                m->vol = 0; m->natural = 0;
                changed = 1;
                continue;
            }
            if (r->flt) {                   /* the double's bits */
                struct ir_ins *b = ib_push(&nb);
                *b = in;
                b->op = IR_BITCAST; b->a = x; b->b = -1; b->c = -1;
                b->w = 8; b->size = 8; b->sign = 1; b->flt = 0;
                b->vol = 0; b->natural = 0;
                b->dst = x = fn->nvregs++;
            }
            if (hi) {
                struct ir_ins *k = ib_push(&nb);
                memset(k, 0, sizeof *k);
                k->op = IR_CONST; k->imm = 32; k->w = 8;
                k->a = k->b = k->c = -1;
                k->line = in.line; k->col = in.col;
                int kdst = k->dst = fn->nvregs++;   /* (k dangles after the
                                                       * next ib_push) */
                struct ir_ins *sh = ib_push(&nb);
                memset(sh, 0, sizeof *sh);
                sh->op = IR_SHR; sh->a = x; sh->b = kdst; sh->w = 8;
                sh->sign = 0; sh->c = -1;
                sh->line = in.line; sh->col = in.col;
                sh->dst = x = fn->nvregs++;
            }
            struct ir_ins *e = ib_push(&nb);
            *e = in;
            e->op = IR_EXT; e->a = x; e->b = -1; e->c = -1;
            e->size = 4; e->flt = 0; e->vol = 0; e->natural = 0;
            changed = 1;
        }
        newpos[fn->nins] = nb.n;
        remap_scopes(fn, newpos, fn->nins);
        free(newpos);
        free(fn->ins);
        fn->ins = nb.p; fn->nins = nb.n; fn->cap = nb.cap;
    }
    /* a private local nothing loads from any more: its stores are dead */
    {
        char *loaded = xcalloc((size_t)nvars, 1);
        for (int n = 0; n < fn->nins; n++) {
            const struct ir_ins *i = &fn->ins[n];
            if ((i->op == IR_LOAD || i->op == IR_MEMCPY) && i->a >= 0 &&
                i->a < nv && base[i->a] >= 0)
                loaded[base[i->a]] = 1;
            if (i->op == IR_MEMCPY && i->b >= 0 && i->b < nv &&
                base[i->b] >= 0)
                loaded[base[i->b]] = 1;
        }
        int k = 0;
        int *newpos = xmalloc((size_t)(fn->nins + 1) * sizeof *newpos);
        for (int n = 0; n < fn->nins; n++) {
            const struct ir_ins *i = &fn->ins[n];
            newpos[n] = k;
            if ((i->op == IR_STORE || i->op == IR_MEMZERO) && i->a >= 0 &&
                i->a < nv && base[i->a] >= 0 && !bad[base[i->a]] &&
                !loaded[base[i->a]] && !i->vol) {
                changed = 1;
                continue;
            }
            fn->ins[k++] = fn->ins[n];
        }
        newpos[fn->nins] = k;
        remap_scopes(fn, newpos, fn->nins);
        free(newpos);
        fn->nins = k;
        free(loaded);
    }
    free(base); free(boff); free(bad); free(rec); free(nrec);
    free(plan); free(pr);
    free(d.cnt); free(d.ins);
    return changed;
}

/* ---- sinking a constant to the use that wants it ----------------------
 *
 * A literal has no operands and no side effects, so where it is
 * MATERIALISED is free to choose -- and the choice costs a register for
 * however long the value is live. irgen and the loop passes leave
 * plenty of them a long way from the one instruction that reads them:
 * 464 of the 2242 constants across lib/libc and lib/libcxx have a
 * single use more than one instruction later, 116 of them more than
 * eight. 186 of the 828 values the x86-64 allocator spills are
 * constants, which is a slot and a reload for something a `mov $imm`
 * reproduces in one instruction wherever it is wanted.
 *
 * So one of these with exactly one use moves to just before it. Always
 * sound, in the strong sense: nothing it depends on can have changed,
 * because it depends on nothing -- the same holds for the address of a
 * local, a global, a string or a function, so those move too (75 and 63
 * of the spilled values are an `addr` and a `straddr`). It cannot move to a place the use does
 * not reach, because that place is where the use is.
 *
 * Only forward. A use EARLIER in the instruction stream is the far side
 * of a back edge, and moving the definition after it would leave the
 * first iteration reading nothing.
 *
 * And never INTO a loop. A constant before a loop whose one use is
 * inside it is very often there because LICM put it there, and sinking
 * it undoes the hoist: the value is rebuilt on every trip. That is one
 * instruction on x86-64, but two on aarch64 for a 32-bit multiplier
 * (mov + movk) and three for a global's address (adrp + add + mov) --
 * an FNV hash rebuilt 16777619 for every byte it read. A loop here is
 * the span of a backward branch, which is what the IR has for one.
 */
/* Does this constant take more than one instruction to build on the
 * target? Only those are worth a register held across a loop: x86-64
 * builds anything in one (`mov $imm`, `lea sym(%rip)`), and there sinking
 * into a loop frees a register for one instruction a trip -- keeping them
 * all hoisted made matrix and sort slower and lib/libc bigger. A global's
 * address is two (adrp+add, movw+movt, auipc+addi); a literal is two when
 * it is outside the one-instruction range (movz/movn or a bitmask on
 * aarch64; a 16-bit movw or a modified immediate on Thumb; 12 bits on
 * RISC-V). AVR is left as it was: every multi-byte value there is several
 * instructions, and its register file is what runs out first. */
static int g_opt_size;                  /* -Os, set by opt_run (defined below) */
int t_imm_ok(long imm);                 /* arch/thumb/emit.c */
int a64_bitmask_ok(long imm, int w);    /* arch/aarch64/emit.c */
static int const_is_expensive(const struct ir_ins *i)
{
    enum target_arch ta = target_get();
    /* RX: any constant or address is one `mov.l #imm, rd` */
    if (ta == TARGET_X86_64 || ta == TARGET_AVR || ta == TARGET_RX)
        return 0;
    switch (i->op) {
    case IR_GADDR: case IR_STRADDR: case IR_FADDR:
        return 1;
    case IR_CONST: {
        long v = i->imm;
        if (i->w == 4) v = (long)(int)v;
        if (ta == TARGET_AARCH64) {
            unsigned long u = (unsigned long)v, n = ~u;
            int w = i->w == 4 ? 4 : 8;
            if (w == 4) { u &= 0xffffffffUL; n &= 0xffffffffUL; }
            for (int s = 0; s < 8 * w; s += 16) {
                if ((u & ~(0xffffUL << s)) == 0) return 0;     /* movz */
                if ((n & ~(0xffffUL << s)) == 0) return 0;     /* movn */
            }
            return !a64_bitmask_ok(w == 4 ? (long)(unsigned)u : v, w);
        }
        if (ta == TARGET_THUMB)
            return !(t_imm_ok(v) || (v >= 0 && v <= 0xffff));
        if (ta == TARGET_MIPS32 || ta == TARGET_MIPS64)  /* addiu, ori from $0 */
            return !((v >= -32768 && v <= 32767) || (v >= 0 && v <= 0xffff));
        if (ta == TARGET_LOONGARCH64)  /* ori/addi.w from r0, or a lu12i.w */
            return !((v >= -2048 && v <= 4095) ||
                     ((v & 0xfff) == 0 && v == (long)(int)v));
        if (ta == TARGET_TRICORE)               /* mov, mov.u or movh */
            return !((v >= -32768 && v <= 32767) || (v >= 0 && v <= 0xffff) ||
                     !(v & 0xffff));
        if (ta == TARGET_XTENSA)                /* movi; else a literal */
            return !(v >= -2048 && v <= 2047);
        if (ta == TARGET_PPC32)                 /* li, or lis */
            return !((v >= -32768 && v <= 32767) || (v & 0xffff) == 0);
        if (ta == TARGET_SPARC32)               /* or from %g0, or sethi */
            return !((v >= -4096 && v <= 4095) || (v & 0x3ff) == 0);
        if (ta == TARGET_COLDFIRE)              /* moveq */
            return !(v >= -128 && v <= 127);
        return !(v >= -2048 && v <= 2047);                     /* RISC-V */
    }
    default:
        return 0;
    }
}

static int *sink_loop_depth(const struct ir_func *fn)
{
    int N = fn->nins;
    int *depth = xcalloc((size_t)(N ? N : 1), sizeof *depth);
    int *lab = xmalloc((size_t)(fn->nlabels ? fn->nlabels : 1) * sizeof *lab);
    for (int l = 0; l < fn->nlabels; l++) lab[l] = -1;
    for (int i = 0; i < N; i++)
        if (fn->ins[i].op == IR_LABEL && fn->ins[i].label >= 0 &&
            fn->ins[i].label < fn->nlabels)
            lab[fn->ins[i].label] = i;
    for (int i = 0; i < N; i++) {
        const struct ir_ins *in = &fn->ins[i];
        if ((in->op == IR_JMP || in->op == IR_BRZ || in->op == IR_BRNZ) &&
            in->label >= 0 && in->label < fn->nlabels &&
            lab[in->label] >= 0 && lab[in->label] <= i)
            for (int k = lab[in->label]; k <= i; k++)
                depth[k]++;
    }
    free(lab);
    return depth;
}
/* each_read's callback for "which instruction reads this vreg": the
 * FIRST one wins, which is the only one when the use count is 1. */
struct sink_at_ctx { int *at; int nv; int n; };
static void sink_at_cb(int *p, void *ctx)
{
    struct sink_at_ctx *s = ctx;
    if (*p >= 0 && *p < s->nv && s->at[*p] < 0) s->at[*p] = s->n;
}

static int pass_sinkconst(struct ir_func *fn)
{
    int nv = fn->nvregs;
    if (nv == 0 || fn->nins == 0)
        return 0;
    int *use = xcalloc((size_t)nv, sizeof *use);
    struct ucount uc = { use, nv };
    for (int n = 0; n < fn->nins; n++)
        each_read(&fn->ins[n], count_cb, &uc);

    struct defs d;
    compute_defs(fn, &d);

    /* The one instruction that reads each vreg, found in a single pass:
     * `use[] == 1` above already says there is exactly one. */
    int *at = xmalloc((size_t)nv * sizeof *at);
    for (int v = 0; v < nv; v++) at[v] = -1;
    struct sink_at_ctx sa = { at, nv, 0 };
    for (int n = 0; n < fn->nins; n++) {
        sa.n = n;
        each_read(&fn->ins[n], sink_at_cb, &sa);
    }

    /* where each sinkable constant wants to go */
    int *to = xmalloc((size_t)fn->nins * sizeof *to);
    for (int n = 0; n < fn->nins; n++) to[n] = -1;
    int *depth = sink_loop_depth(fn);
    int any = 0;
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        /* Every op here materialises a value out of nothing: a
         * literal, or the address of a local, a global, a string or a
         * function. None reads a vreg, so none can be invalidated by
         * what it moves past. */
        switch (i->op) {
        case IR_CONST: case IR_ADDR: case IR_GADDR:
        case IR_STRADDR: case IR_FADDR:
            break;
        default:
            continue;
        }
        if (i->dst < 0 || i->dst >= nv)
            continue;
        if (use[i->dst] != 1 || d.cnt[i->dst] != 1)
            continue;
        /* A RISC-V branch compares two registers: a loop's bound is a
         * `li` every trip once it sits beside the compare, where x86 and
         * Arm have taken it as an immediate before this runs. It used to
         * stay out by accident -- the guard in front of the loop read it
         * too -- until the guard could be decided at compile time. */
        /* (MIPS's beq/bne compare two registers too.) */
        int rv_cmp = (target_get() == TARGET_RISCV32 ||
                      target_get() == TARGET_RISCV64 ||
                      target_is_mips() ||
                      target_get() == TARGET_LOONGARCH64 ||
                      target_get() == TARGET_XTENSA) &&
                     i->op == IR_CONST && i->imm != 0 && at[i->dst] >= 0 &&
                     fn->ins[at[i->dst]].op == IR_CMP;
        /* A select's value stops above the compare that makes its
         * condition: between the two it would part the flags from the
         * IT block or cmov that reads them (ifconv_any put it there). */
        int u = at[i->dst];
        if (u > 0 && fn->ins[u].op == IR_SELECT && target_cheap_select() &&
            fn->ins[u - 1].op == IR_CMP && fn->ins[u - 1].dst == fn->ins[u].a)
            u--;
        if (u > n + 1 &&
            (depth[u] <= depth[n] || g_opt_size ||
             (!const_is_expensive(i) && !rv_cmp))) {
            to[n] = u;
            any = 1;
        }
    }
    free(at);
    free(depth);
    if (!any) { free(use); free(to); free_defs(&d); return 0; }

    /* Rebuild in one pass: `head[m]` chains the constants that want to
     * land just before instruction m, in their original order. */
    int *head = xmalloc((size_t)fn->nins * sizeof *head);
    int *next = xmalloc((size_t)fn->nins * sizeof *next);
    for (int n = 0; n < fn->nins; n++) { head[n] = -1; next[n] = -1; }
    for (int n = fn->nins - 1; n >= 0; n--)
        if (to[n] >= 0) { next[n] = head[to[n]]; head[to[n]] = n; }
    struct ibuf nb = { 0, 0, 0 };
    for (int n = 0; n < fn->nins; n++) {
        for (int k = head[n]; k >= 0; k = next[k])
            *ib_push(&nb) = fn->ins[k];
        if (to[n] < 0) *ib_push(&nb) = fn->ins[n];
    }
    free(head); free(next);
    free(fn->ins);
    fn->ins = nb.p; fn->nins = nb.n; fn->cap = nb.cap;
    free(use); free(to); free_defs(&d);
    return 1;
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
/* May constant `imm` become op's immediate operand? Yes, except where the
 * target's instructions cannot hold it: there a folded constant is rebuilt
 * at every use -- a movw+movt pair each time on Thumb -- where one left in
 * a register is built once and can be hoisted out of a loop. */
static int target_imm_foldable(int op, long imm, int w)
{
    if (target_get() == TARGET_THUMB && w == 8 &&
        (op == IR_AND || op == IR_OR || op == IR_XOR) && !getenv("EMBCC_T_NOWIDEIMM"))
        return thumb_imm_foldable64(op, imm);
    if (target_get() == TARGET_RISCV32 && w == 8 &&
        (op == IR_AND || op == IR_OR || op == IR_XOR) && !getenv("EMBCC_RV_NOWIDEIMM"))
        return riscv_imm_foldable64(op, imm);
    if (target_get() == TARGET_MIPS32 && w == 8 &&
        (op == IR_AND || op == IR_OR || op == IR_XOR))
        return mips_imm_foldable64(op, imm);
    if (target_get() == TARGET_MIPS32)
        return mips_imm_foldable(op, imm);
    /* MIPS64: one register to 64 bits; a 128-bit operation takes none */
    if (target_get() == TARGET_MIPS64)
        return w <= 8 && mips_imm_foldable(op, imm);
    /* TriCore: a 64-bit AND/OR/XOR is done half by half with any constant
     * (codegen.c's logic_half); every other 64-bit operation builds it */
    if (target_get() == TARGET_TRICORE)
        return w == 8 ? op == IR_AND || op == IR_OR || op == IR_XOR
                      : tc_imm_foldable(op, imm);
    if (target_get() == TARGET_XTENSA)
        return w == 4 && xtensa_imm_foldable(op, imm);
    if (target_get() == TARGET_PPC32 && w == 8 &&
        (op == IR_AND || op == IR_OR || op == IR_XOR))
        return ppc_imm_foldable64(op, imm);
    if (target_get() == TARGET_PPC32)
        return ppc_imm_foldable(op, imm);
    if (target_get() == TARGET_SPARC32 && w == 8 &&
        (op == IR_AND || op == IR_OR || op == IR_XOR))
        return sparc_imm_foldable64(op, imm);
    if (target_get() == TARGET_SPARC32)
        return sparc_imm_foldable(op, imm);
    if (target_get() == TARGET_COLDFIRE)
        return cf_imm_foldable(op, imm, w);
    if (target_get() == TARGET_THUMB)
        return thumb_imm_foldable(op, imm);
    if (target_get() == TARGET_RISCV32 || target_get() == TARGET_RISCV64)
        return riscv_imm_foldable(op, imm);
    if (target_get() == TARGET_LOONGARCH64)
        return la_imm_foldable(op, imm);
    if (target_get() == TARGET_AARCH64)
        return a64_imm_foldable(op, imm, w);
    return 1;
}

/* `if (x >> 63)` for a 64-bit x is `if (x < 0)`: the shift's 0 or 1 is the
 * sign bit, which a signed compare with zero reads straight off the high
 * word. On a 32-bit target the shift is two instructions (the bit moved
 * down, the high word zeroed) and the branch an orrs of both halves; the
 * compare is one `cmp hi, #0` fused into the branch. Only for a shift by
 * the width less one, unsigned, whose one reader is a branch -- which
 * then tests the compare's 0 or 1 at four bytes, a compare's result being
 * an int. After immfold, so the zero can be an immediate. */
static int pass_signtest(struct ir_func *fn)
{
    int nv = fn->nvregs, changed = 0;
    if (fn->nins == 0 || nv == 0 || getenv("EMBCC_NO_SIGNTEST"))
        return 0;
    int *use = xcalloc((size_t)nv, sizeof *use);
    struct ucount uc = { use, nv };
    for (int n = 0; n < fn->nins; n++)
        each_read(&fn->ins[n], count_cb, &uc);
    for (int n = 0; n + 1 < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n], *br = &fn->ins[n + 1];
        /* ...and the same AND read by `== 0` or `!= 0`, which is how C
         * spells `while (!(m >> 52 & 1))`: both at four bytes. */
        if (i->op == IR_AND && i->imm_b && !i->flt && i->w == 8 &&
            i->imm >= 0 && i->imm <= 0xffffffffL && i->dst >= 0 &&
            i->dst < nv && use[i->dst] == 1 && br->op == IR_CMP &&
            br->a == i->dst && br->imm_b && br->imm == 0 && br->w == 8 &&
            (br->pred == B_EQ || br->pred == B_NE) &&
            !getenv("EMBCC_NO_SIGNTEST")) {
            i->w = 4;
            i->sign = 0;
            br->w = 4;
            changed = 1;
            continue;
        }
        if ((br->op != IR_BRZ && br->op != IR_BRNZ) || br->a != i->dst ||
            i->flt || !i->imm_b || i->w != 8 || i->dst < 0 ||
            i->dst >= nv || use[i->dst] != 1)
            continue;
        /* `if (x & K)` with K below 2^32 tests only bits of the low word:
         * the AND and the branch at four bytes, which on a 32-bit target is
         * one and and one branch instead of a pair and an or -- and leaves
         * whatever x is read narrow, `(m >> 52) & 1` a shift of the high
         * word alone. */
        if (i->op == IR_AND && i->imm >= 0 && i->imm <= 0xffffffffL &&
            !getenv("EMBCC_NO_SIGNTEST")) {
            i->w = 4;
            i->sign = 0;
            br->w = 4;
            changed = 1;
            continue;
        }
        if (i->op != IR_SHR || i->sign || i->imm != 63)
            continue;
        i->op = IR_CMP;
        i->pred = B_LT;
        i->sign = 1;
        i->imm = 0;                     /* x <s 0, still imm_b */
        br->w = 4;
        changed = 1;
    }
    free(use);
    return changed;
}

/* A store of N bytes writes the low N bytes of its value, and an
 * extension from S >= N bytes leaves those bytes as they were -- so the
 * store may take the extension's SOURCE, and the extension, read by
 * nothing else, goes with the DCE after. `k[n] = (char)(v + 'a')` was
 * ext.4:1s and store:1: a movsbl before every byte store on x86-64, a
 * shift pair on RV32. 45 of the 555 stores across lib/libc.
 *
 * Both values must be defined exactly once: a merge temp is written on
 * each path into it, and a source written again between
 * the extension and the store would be read at its new value. */
static int pass_storenarrow(struct ir_func *fn)
{
    struct defs d;
    int changed = 0;
    compute_defs(fn, &d);
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        const struct ir_ins *e;
        int v = i->b, x;
        if (i->op != IR_STORE || i->flt || v < 0 || v >= fn->nvregs ||
            i->size <= 0 || d.cnt[v] != 1 || d.ins[v] < 0)
            continue;
        e = &fn->ins[d.ins[v]];
        if (e->op != IR_EXT || e->flt || e->size < i->size || e->size > 8)
            continue;
        x = e->a;
        if (x < 0 || x >= fn->nvregs || d.cnt[x] != 1)
            continue;
        i->b = x;
        changed = 1;
    }
    free_defs(&d);
    return changed;
}

/* Thumb's mla and mls take the product's operands in registers -- the
 * backend fuses `p = a * b; d = c +- p` into one when p's only reader is
 * the instruction right after it. A constant folded into the multiply
 * turns that into a shifted add and an add, two or three instructions,
 * where `mla d, a, rK, c` is one and rK is built once (and outside the
 * loop, often). So such a multiply keeps its constant. `use` is each
 * vreg's read count. */
static int mla_keeps_reg(const struct ir_func *fn, int n, const int *use)
{
    const struct ir_ins *i = &fn->ins[n];
    if (target_get() != TARGET_THUMB || i->op != IR_MUL || i->w != 4 ||
        i->dst < 0 || i->dst >= fn->nvregs || use[i->dst] != 1 ||
        n + 1 >= fn->nins || getenv("EMBCC_NO_MLA") ||
        getenv("EMBCC_NO_MLAKEEP"))
        return 0;
    const struct ir_ins *nx = &fn->ins[n + 1];
    if ((nx->op != IR_ADD && nx->op != IR_SUB) || nx->flt || nx->w != 4 ||
        nx->dst < 0)
        return 0;
    return (nx->b == i->dst && nx->a != i->dst) ||
           (nx->op == IR_ADD && nx->a == i->dst && nx->b != i->dst);
}

static int pass_immfold(struct ir_func *fn)
{
    struct defs d;
    compute_defs(fn, &d);
    int changed = 0;
    int *use = xcalloc((size_t)(fn->nvregs ? fn->nvregs : 1), sizeof *use);
    struct ucount uc = { use, fn->nvregs };
    for (int n = 0; n < fn->nins; n++)
        each_read(&fn->ins[n], count_cb, &uc);
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        if (i->flt || i->imm_b || i->w == 16)
            continue;           /* see pass_fold on the 128-bit width */
        if (mla_keeps_reg(fn, n, use))
            continue;
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
        /* Thumb and RV32 take a 64-bit AND/OR/XOR constant half by half,
         * so its width is not x86's imm32 question (*_imm_foldable64). */
        int wide_ok = (target_get() == TARGET_THUMB ||
                       target_get() == TARGET_RISCV32 ||
                       target_get() == TARGET_MIPS32 ||
                       target_get() == TARGET_TRICORE ||
                       target_get() == TARGET_PPC32 ||
                       target_get() == TARGET_RX ||
                       target_get() == TARGET_SPARC32 ||
                       target_get() == TARGET_COLDFIRE) && i->w == 8 &&
                      (i->op == IR_AND || i->op == IR_OR || i->op == IR_XOR);
        /* ...and a 64-bit compare with any constant whose halves its
         * subs/sbcs or cmp/cmpeq take (thumb_cmp64_imm): strtol's
         * `v > LONG_MAX` kept 0x7fffffff in a register pair. Not on
         * ARMv6-M, which builds a constant from a literal pool. */
        int cmp64 = target_get() == TARGET_THUMB && i->op == IR_CMP &&
                    i->w == 8 && target_thumb_arch() >= 7 &&
                    !getenv("EMBCC_T_NOCMP64IMM");
        int p64;
        long lo64, hi64;
        if (cmp64 ? get_const(fn, &d, i->b, &B) &&
                    thumb_cmp64_imm(i->pred, i->sign, B, &p64, &lo64, &hi64)
                  : get_const(fn, &d, i->b, &B) && (fits_imm32(B) || wide_ok) &&
                    target_imm_foldable(i->op, B, i->w)) {
            i->imm = B; i->imm_b = 1; i->b = -1;    /* op a, imm */
            changed = 1;
        } else if (cmp64 ? get_const(fn, &d, i->a, &A) &&
                           thumb_cmp64_imm(swap_pred(i->pred), i->sign, A,
                                           &p64, &lo64, &hi64)
                         : (commutative || i->op == IR_CMP) &&
                           get_const(fn, &d, i->a, &A) &&
                           (fits_imm32(A) || wide_ok) &&
                           target_imm_foldable(i->op, A, i->w)) {
            /* Constant in the first operand: move it to the immediate, keeping
             * a valid instruction — commutative ops just swap, a compare swaps
             * and flips its predicate. */
            i->a = i->b; i->b = -1; i->imm = A; i->imm_b = 1;
            if (i->op == IR_CMP)
                i->pred = swap_pred(i->pred);
            changed = 1;
        }
    }
    free(use);
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

/* ---- the budget is flat, and that was measured -----------------------
 *
 * A flat instruction budget looks like the wrong question. What inlining
 * trades is the callee's body against the CALL -- the argument moves,
 * the call, the return -- and that is not a constant, so a cost model
 * ought to beat a number. Three were tried, each a fact already in the
 * IR rather than a guess, and all three made the code BIGGER:
 *
 *   Credit for the call removed (2 + one per argument), and credit for
 *   a CONSTANT argument the callee will fold around (4 each):
 *     lib/libc + lib/libcxx objects, x86-64  472912 -> 474096
 *                                   aarch64  661208 -> 664816
 *   The constant credit alone changed x86-64 by nothing at all and
 *   aarch64 by +384: an argument that is a literal rarely decides a
 *   budget, because the functions it would let in are not near it.
 *
 *   A budget of 200 for the SOLE caller of a static function nothing
 *   else can reach -- where the body is moved rather than copied, so
 *   dead-function elimination takes the original and the unit should
 *   get smaller however big it was:
 *     objects, x86-64  472912 -> 474864
 *              aarch64 661208 -> 667824
 *   and tests/bench/kernels.c memory_stream 0.22 -> 0.287, because that
 *   kernel IS a static function with one caller: its loop nest moved
 *   into main, and a bigger function allocates worse. The size the
 *   original would have freed is smaller than what the caller then
 *   spends on spills.
 *
 * Re-measured 2026-10-02, after the allocator had changed under it: a
 * sole-caller budget of 2000 made lib/libc SMALLER at -O2 and -Os on all
 * five targets (x86-64 -84 bytes, aarch64 -156, RV32 -58, Thumb -22, AVR
 * -54), left tests/bench/kernels.c unchanged on four boards, memory_stream
 * included, and made the workload's state machine 4-11% faster (its
 * per-token function, 316 instructions, now moves into its one caller).
 * So the sole-caller number is 2000; the copied-callee one stays at 24 --
 * at 64 the hash table's probe and key builder were copied into their
 * callers and ran 1-4% SLOWER on every board.
 *
 * One count between the two did pay, measured 2026-10-04: a static
 * function called from exactly TWO places, its address never taken, may
 * be 200 instructions. Inlining both calls removes the original, so the
 * unit pays one extra copy, not one per caller. That was the hash
 * table's probe, key builder and insert again, but after the allocator
 * stopped spilling a loop's counters around calls (ra-callee-k): against
 * 24, the text kernel ran 5.9% (RV32), 8.6% (x86-64) and 10.0% (aarch64)
 * fewer instructions, M4's +0.4%, and hash 9.6-20.9% fewer on all four.
 * -O2 code over lib/libc and the workload grew 1.0-1.6%; -Os is
 * unchanged. The insert is 167 instructions, so a two-caller budget of
 * 64 or 100 bought text and a fraction of hash (0.6-5.4%) for 0.2-1.1%.
 * A flat budget of 64 bought text's speed for 6-9% more code.
 *
 * So the number stays, and this is what it is doing there. A cost model
 * that beats it wants something these three did not have -- how HOT the
 * call is (section 4's profile work), or a real estimate of what the
 * caller's register pressure will do -- not more arithmetic on facts
 * already available here. */
#define INLINE_MAX_CALLEE 24     /* instruction budget for an inline candidate */
#define INLINE_SOLE_CALLEE 2000  /* ...and for a body that MOVES (sole_static_caller) */
#define INLINE_TWO_CALLEE  200   /* ...and for one of two calls to a static (-O2) */
#define INLINE_MAX_CALLER 800    /* stop expanding a caller past this many ins */
#define INLINE_MAX_PER_FUNC 64   /* and cap inlines per caller, for termination */

/* -Os: a much smaller budget, and WHY it needed one.
 *
 * The comment above opt_run used to say that inlining everything -O2 inlines
 * is the smaller answer at -Os too, because a callee with one caller is
 * DELETED after it moves, so refusing to inline it keeps two copies. That
 * half is still true, and INLINE_SOLE_CALLEE below is how it stays true.
 *
 * The other half was wrong, and it was wrong because 24 is a count of IR
 * instructions being used as a proxy for BYTES. An IR op costs one or two
 * instructions on x86-64 and six to ten on AVR, where an int add is four and
 * every value lives in a frame slot. So the same budget that admits a genuine
 * one-liner on a 64-bit machine admits a 450-byte function on an 8-bit one,
 * and lib/rt/avrfp.c is what that looks like from the outside: -Os produced
 * 41392 bytes of text against -O1's 32856, on a part with 32768 of flash.
 * `pack` and `mul24` had been copied into every caller and deleted, and
 * addsub grew 6600 -> 9850 for it.
 *
 * So at -Os a body is inlined only when it MOVES -- the sole-caller case,
 * where there is no duplication to pay for -- or when it is small enough that
 * the copy is plausibly smaller than the call sequence it replaces, on the
 * most expansive target rather than the least. Nothing changes at -O2, so no
 * speed measurement moves; -Os is the level that asked for size.
 */
#define INLINE_SIZE_CALLEE 6

/* Whether opt_run was asked for size. A global in the style of g_pass: the
 * inliner's decision is three call levels below opt_run. */
static int g_opt_size;

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
        in->op == IR_LABEL || in->op == IR_SWITCH)
        in->label += r->lbase;      /* a switch's table: see inline_call */
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
static int inlinable(struct ir_func *cf, int force, int sole, const char **why,
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
    int budget = g_opt_size ? INLINE_SIZE_CALLEE : INLINE_MAX_CALLEE;
    if (!force && cf->nins > budget &&
        !(sole == 1 && cf->nins <= INLINE_SOLE_CALLEE) &&
        !(sole == 2 && !g_opt_size && cf->nins <= INLINE_TWO_CALLEE)) {
        *why = "callee-too-large";
        /* Name the budget that applied: a sole or one-of-two caller's
         * is larger than the copied one. */
        if (sole == 1)
            budget = INLINE_SOLE_CALLEE;
        else if (sole == 2 && !g_opt_size)
            budget = INLINE_TWO_CALLEE;
        if (detail)
            snprintf(detail, dcap, "%d instructions, budget %d%s",
                     cf->nins, budget, g_opt_size ? " (-Os)" : "");
        return 0;
    }
    if (cf->neh)             { *why = "callee-has-exception-regions"; return 0; }
    if (c->ret_ty->kind == TY_STRUCT) { *why = "returns-a-struct"; return 0; }
    /* FLOATS used to be refused here too -- a float return and a float
     * local, 82 and 117 call sites across lib/libc and lib/libcxx
     * against the 218 that were inlined. The reason was that a float
     * had no register to live in, so the inliner's parameter STVARs and
     * its result MOV were memory traffic the call had not been paying;
     * with the float register class and a float pool those are ordinary
     * copies that mem2reg and the allocator see through like any other.
     *
     * lib/rt/complex.c is what this is for: __muldc3 called is_nan_d,
     * is_inf_d and copysign_d thirty-four times, and every product was
     * live across one, so every product was in memory. gcc's __muldc3
     * touches the stack not once. */
    /* A PARAMETER is bound by `stvar local, argvreg`, which is only the
     * value when the argument vreg IS the value. For anything an ABI
     * passes some other way -- a struct or a _Complex (the vreg is its
     * ADDRESS), a long double (the x87 stack), an __int128 (a register
     * pair) -- that store copies the wrong bytes from the wrong place.
     * `clog` is what says so: its callee took a `_Complex double`, the
     * caller had `%3 = addr v0`, and the splice produced
     * `stvar:16 v1, %3` -- sixteen bytes read out of an eight-byte
     * pointer's slot.
     *
     * So a parameter has to be a scalar that fits one vreg. A non-
     * parameter local has no such constraint: the metadata copy below
     * carries its type and alignment, ir_locals_fill rebuilds the
     * descriptors, and SROA looks at it in the caller exactly as it
     * would have in the callee -- which is what lets is_inf_d's
     * `union { double d; u64 u; }` come across. */
    for (int k = 0; k < cf->nparams && k < cf->nvars; k++) {
        struct type *pt = c->var_tys[k];
        if (!pt || ty_size(pt) > 8 || ty_is_complex(pt) ||
            !(ty_is_integer(pt) || pt->kind == TY_PTR || ty_is_float(pt))) {
            *why = "parameter-is-not-a-simple-scalar";
            return 0;
        }
    }
    if (ty_is_complex(c->ret_ty) || c->ret_ty->kind == TY_LDOUBLE ||
        c->ret_ty->kind == TY_INT128) {
        *why = "returns-a-value-wider-than-a-vreg";
        return 0;
    }
    for (int i = 0; i < cf->nins; i++) {
        const struct ir_ins *in = &cf->ins[i];
        /* Inline asm used to be refused here, which left every
         * always_inline CMSIS intrinsic and RTOS critical section a call.
         * An asm comes across with its operands renamed (inline_call);
         * its registers were chosen when it was assembled, and the
         * caller's allocator treats it exactly as the callee's did. */
        if (in->op == IR_VA_START) { *why = "callee-uses-va_start"; return 0; }
        /* `in->flt` used to refuse the callee outright. Floating-point
         * arithmetic is ordinary arithmetic to this pass -- it renames
         * vregs and splices instructions -- and the values it produces
         * now have a register class and a pool to live in. */
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
        /* An asm's operands are renamed in place through each_read, and
         * the callee keeps its own: the copy gets operand arrays of its
         * own (the assembled bytes are never written, and are shared). */
        if (in.op == IR_ASM && in.asm_ir) {
            struct ir_asm *ca = xmalloc(sizeof *ca);
            *ca = *in.asm_ir;
            ca->in = xmalloc((size_t)(ca->nin ? ca->nin : 1) * sizeof *ca->in);
            ca->out = xmalloc((size_t)(ca->nout ? ca->nout : 1) *
                              sizeof *ca->out);
            memcpy(ca->in, in.asm_ir->in, (size_t)ca->nin * sizeof *ca->in);
            memcpy(ca->out, in.asm_ir->out, (size_t)ca->nout * sizeof *ca->out);
            in.asm_ir = ca;
        }
        remap_ins(&in, &cm);
        if (in.op == IR_SWITCH)          /* its table, renumbered like its default */
            in.jt = ir_jt_clone(fn, cf, in.jt, L);
        if (in.op == IR_RET) {
            if (in.a >= 0 && dst >= 0) {
                struct ir_ins *mv = &buf[m++];
                ins_blank(mv);
                mv->op = IR_MOV; mv->dst = dst; mv->a = in.a;
                mv->line = in.line;      /* the callee's `return` */
                mv->col = in.col;
            }
            if (i != cf->nins - 1) {     /* the last RET falls into `after` */
                struct ir_ins *jp = &buf[m++];
                ins_blank(jp);
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
    /* ...and the stack-argument area: the callee's calls are the
     * caller's now. x86-64 sizes the frame from it, and a caller that
     * passed nothing on the stack itself had none -- so a printf of four
     * long doubles, inlined into main, wrote its arguments over main's
     * own slots (embedded-libc.c at -O1: the fourth printed as the
     * first). */
    if (cf->outgoing_bytes > fn->outgoing_bytes)
        fn->outgoing_bytes = cf->outgoing_bytes;
    if (cf->scratch_bytes > fn->scratch_bytes)
        fn->scratch_bytes = cf->scratch_bytes;
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

/* How many calls in the whole unit name this function, and does its
 * address escape? A STATIC function nothing takes the address of and
 * exactly one call names is a body that will be MOVED rather than
 * copied: once the call is gone, dead-function elimination takes the
 * original, so the unit cannot grow by more than the call it removed.
 * The size budget is the wrong question for that one. Exactly two calls
 * is the next case (INLINE_TWO_CALLEE): both inlined, one copy extra.
 * Returns the count, 1 or 2, and 0 for any other count, a non-static
 * function, or one whose address is taken. */
static int sole_static_caller(struct ir_unit *iu, struct func *c)
{
    if (!c || !c->is_static)
        return 0;
    int calls = 0;
    for (int f = 0; f < iu->nfuncs; f++) {
        struct ir_func *fn = &iu->funcs[f];
        for (int i = 0; i < fn->nins; i++) {
            struct ir_ins *in = &fn->ins[i];
            if (in->op == IR_FADDR && in->callee == c)
                return 0;                 /* the address escapes */
            if (in->op == IR_CALL && !in->indirect && in->callee == c &&
                ++calls > 2)
                return 0;
        }
    }
    return calls;      /* 1: the sole caller; 2: one of two; 0: neither */
}

/* -O1: inline only what makes the code smaller or was asked for -- an
 * always_inline callee, and a static function with a single caller,
 * whose original is then deleted. gcc's -O1 does the same (always_inline
 * at every level, -finline-functions-called-once). */
static int g_inline_o1;

/* -fno-inline-functions: only a function its author declared `inline`
 * (or always_inline) is a candidate, at any level -- the sole-caller and
 * small-function cases included, which GCC's flag leaves to two others.
 * One flag that means "what I did not mark stays a call" is the one a
 * build can rely on: for a breakpoint, a stack-usage figure, a symbol
 * in the map file. */
static int g_inline_declared_only;
void opt_set_inline_declared_only(int on) { g_inline_declared_only = on; }

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
            int ci = -1, csole = 0;
            struct ir_func *cf = NULL;
            for (int i = 0; i < fn->nins; i++) {
                struct ir_ins *in = &fn->ins[i];
                if (in->op != IR_CALL || in->indirect || !in->callee ||
                    in->retsize)
                    continue;
                struct ir_func *c = func_ir(iu, in->callee);
                const char *why = NULL;
                char detail[160] = "";
                int ok = 0, sole = 0;
                if (!c)
                    why = "callee-not-defined-here";
                else if (c == fn)
                    why = "would-be-recursive";
                else if (c->has_i128)
                    why = "callee-computes-in-__int128";
                else if (in->callee->attr_noinline)
                    why = "callee-is-noinline";
                else if (in->callee->is_weak)
                    why = "callee-is-weak";   /* the link may replace it */
                else if (g_inline_declared_only &&
                         !in->callee->attr_always_inline &&
                         !in->callee->any_inline)
                    why = "not-declared-inline";  /* -fno-inline-functions */
                else {
                    sole = sole_static_caller(iu, in->callee);
                    if (g_inline_o1 && !in->callee->attr_always_inline &&
                        sole != 1)
                        why = "not-a-sole-callee-at-O1";
                    else
                        ok = inlinable(c, in->callee->attr_always_inline,
                                       sole, &why, detail, sizeof detail);
                }
                if (!ok) {
                    remark_add("inline", "not-inlined", in->callee->name, why,
                               fn->src ? fn->file : NULL, in->line,
                               detail[0] ? "%s" : NULL, detail);
                    continue;
                }
                ci = i;
                cf = c;
                csole = sole;
                break;
            }
            if (ci < 0)
                break;
            {
                /* The budget that admitted it, not always the copied
                 * one: a sole caller's or one of two calls' is larger,
                 * and -Os's is smaller. */
                int bud = g_opt_size ? INLINE_SIZE_CALLEE : INLINE_MAX_CALLEE;
                if (cf->nins > bud && csole == 1)
                    bud = INLINE_SOLE_CALLEE;
                else if (cf->nins > bud && csole == 2 && !g_opt_size)
                    bud = INLINE_TWO_CALLEE;
                remark_add("inline", "inlined", cf->name,
                           fn->ins[ci].callee->attr_always_inline
                               ? "always_inline" : "small-enough",
                           fn->src ? fn->file : NULL, fn->ins[ci].line,
                           "%d instructions into %s, budget %d", cf->nins,
                           fn->src ? fn->name : "?", bud);
            }
            inline_call(fn, ci, cf);
            done++;
        }
    }
}

/* ---- SROA: scalar replacement of aggregates (-O2) ------------------------
 *
 * mem2reg promotes a local out of memory only when nothing takes its
 * address -- and reading a struct field IS taking its address. `s.b = 1`
 * lowers to `addr s`, `add #4`, `store`, so every struct local, however
 * private, stays on the stack and every field access is a real memory
 * reference: invisible to value numbering, to store forwarding, and to
 * the register allocator alike. Across lib/libc and lib/libcxx that is
 * where most of the remaining stack traffic at -O2 came from.
 *
 * It does not have to be. When a local's address never leaves the
 * `addr` / `add constant` / `load` / `store` shape, the bytes it is
 * accessed at are independent scalars -- nothing in the program can
 * observe that they share an object. So the local is SPLIT: one fresh
 * scalar local per distinct (offset, size), and each access becomes an
 * ordinary LDVAR/STVAR of it. This pass only renames memory. What makes
 * the code faster is mem2reg, which now finds variables where it used to
 * find a struct, and everything downstream of that.
 *
 * Every condition is a form of "the address does not escape, and the
 * pieces do not overlap":
 *
 *   - each use of `addr L` is a load or a store, directly or through an
 *     add of a CONSTANT. A variable index could name any field, so no;
 *     and the address reaching a call, a store, a compare or anything
 *     else at all means the object is still one object.
 *   - no access is volatile, and neither is the local.
 *   - each (offset, size) lies inside the object, and two distinct ones
 *     never overlap. Two accesses at the same offset with different
 *     widths -- a union punned, a byte pulled out of an int -- are one
 *     object and not two, and the local is refused rather than split
 *     into pieces that would each hold half the truth.
 *
 * The new locals are appended after the existing ones, so every TEMP
 * renumbers UP by however many were added: slots and temps share one
 * numbering space with the slots first (each_read's comment). That is
 * the same shift the inliner performs, through the same remap.
 */

#define SROA_MAX_FIELDS 16    /* distinct pieces one local may split into */
#define SROA_MAX_SIZE  128    /* bytes; past this it is a buffer, not a record */

struct sroa_fld { int off, size, nl; };

/* The scalar member type covering [off, off+size) in `t`, or NULL when
 * those bytes are not exactly one scalar. The STORAGE would be right
 * either way -- a slot of `size` bytes written and read at `size` bytes
 * holds any field -- but naming a double a double is what lets mem2reg's
 * float path see it, and what makes the remark readable. */
static struct type *sroa_field_ty(struct type *t, int off, int size)
{
    for (int hop = 0; t && hop < 8; hop++) {
        if (off == 0 && ty_size(t) == size &&
            (ty_is_integer(t) || t->kind == TY_PTR || ty_is_float(t)))
            return t;
        if (t->kind == TY_ARRAY) {
            int es = ty_size(t->pointee);
            if (es <= 0 || off % es != 0)
                return NULL;
            off %= es;
            t = t->pointee;
            continue;
        }
        if (t->kind != TY_STRUCT || !t->complete)
            return NULL;
        struct member *m = NULL;
        for (int k = 0; k < t->nmembers; k++) {
            struct member *c = &t->members[k];
            int cs = c->ty ? ty_size(c->ty) : 0;
            if (c->is_bitfield || cs <= 0)
                continue;
            if (off >= c->off && off + size <= c->off + cs) { m = c; break; }
        }
        if (!m)
            return NULL;
        off -= m->off;
        t = m->ty;
    }
    return NULL;
}

/* A vreg's value, when it is a constant, by EVALUATING the chain rather
 * than waiting for the folder.
 *
 * This pass runs before the fixpoint, so nothing has been folded yet and
 * `a[2]` is still four instructions -- `const 2`, `ext`, `mul 4`, `add`.
 * Accepting only a literal would refuse every array subscript in the
 * program, which is most of what there is to split. */
static int sroa_const(struct ir_func *fn, struct defs *d, int v, long *out,
                      int hop)
{
    if (hop > 6 || v < 0 || v >= fn->nvregs || d->cnt[v] != 1)
        return 0;
    int n = d->ins[v];
    if (n < 0)
        return 0;                       /* a parameter: not a constant */
    struct ir_ins *i = &fn->ins[n];
    long a, b;
    switch (i->op) {
    case IR_CONST:
        *out = i->imm;
        return 1;
    case IR_MOV:
        return sroa_const(fn, d, i->a, out, hop + 1);
    case IR_EXT:
        if (i->flt || !sroa_const(fn, d, i->a, &a, hop + 1))
            return 0;
        *out = fold_ext(a, i->size, i->sign, i->w);
        return 1;
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_SHL:
        if (i->flt || !sroa_const(fn, d, i->a, &a, hop + 1))
            return 0;
        if (i->imm_b)
            b = i->imm;
        else if (!sroa_const(fn, d, i->b, &b, hop + 1))
            return 0;
        /* In unsigned, because a signed overflow here is undefined and
         * an offset that wraps is one the caller will reject anyway. */
        {
            unsigned long A = (unsigned long)a, B = (unsigned long)b, r;
            if (i->op == IR_ADD)      r = A + B;
            else if (i->op == IR_SUB) r = A - B;
            else if (i->op == IR_MUL) r = A * B;
            else if (b < 0 || b > 63) return 0;
            else                      r = A << B;
            *out = norm((long)r, i->w);
        }
        return 1;
    default:
        return 0;
    }
}

/* A read of a tracked address anywhere this pass cannot account for: the
 * object escapes, so it stays whole. */
struct sroa_ctx { char *cand; const char **why; const int *abase; int nvr; };
static void sroa_esc_cb(int *p, void *ctx)
{
    struct sroa_ctx *c = ctx;
    int v = *p;
    if (v >= 0 && v < c->nvr && c->abase[v] >= 0 && c->cand[c->abase[v]]) {
        c->cand[c->abase[v]] = 0;
        c->why[c->abase[v]] = "address-escapes";
    }
}

/* One line per local this pass had an opinion about, named as the
 * programmer named it. `nfld` is read only for a local that split, so
 * the early "nothing here was eligible" exit may pass NULL. */
static void sroa_report(struct ir_func *fn, int nparams, int nvars,
                        const char *cand, const int *nfld,
                        const char **why, int report_refusals)
{
    if (!remarks_on() || !fn->src)
        return;
    for (int L = nparams; L < nvars; L++) {
        const struct ir_dbgvar *v = local_var(fn, L);
        if (!v || !v->name || v->name[0] == '<')  /* a compiler-invented name */
            continue;
        int line = v->line ? v->line : fn->line;
        if (cand[L])
            remark_add("sroa", "split-into-scalars", v->name,
                       "address-never-escapes", fn->file, line,
                       "%d bytes -> %d variable%s", fn->locals[L].size,
                       nfld[L], nfld[L] == 1 ? "" : "s");
        else if (why[L] && report_refusals)
            remark_add("sroa", "kept-whole", v->name, why[L],
                       fn->file, line, NULL);
    }
}

/* `report_refusals` belongs to the LAST look this function gets. The
 * pass runs twice -- once before mem2reg and once after -- and a
 * refusal from the first is provisional: the whole point of the second
 * is that some of them stop being true. Only the last one has anything
 * worth telling a person. */
/* ---- a compare-exchange's `expected`, by value --------------------------
 *
 * __atomic_compare_exchange_n(obj, &expected, desired, ...) takes
 * `expected` by ADDRESS, and the address is all that keeps it in memory.
 * A lock's fast path is exactly this --
 *
 *     int c = 0;
 *     if (__atomic_compare_exchange_n(&l->v, &c, 1, 0, ACQ, RLX)) return;
 *
 * -- and mem2reg refuses `c` for having its address taken, so the lock
 * stored a zero to the frame, took its address, loaded it back for the
 * compare-exchange and stored the value seen through it again, where a
 * register would have done (lib/libc's __lock, mtx_lock, call_once).
 *
 * When every address of a scalar local feeds compare-exchanges as their
 * `expected` and nothing else, the local is private: nobody else can see
 * it, so reading it before and writing the value seen after is the same
 * program. That is IR_CAS, the by-value form the __sync builtins use:
 *
 *     e = ldvar v ; s = cas [obj], e, desired ; stvar v, s ; r = (s == e)
 *
 * (s re-extended first when the object is narrower than four bytes, as
 * irgen does for __sync_bool_compare_and_swap). Then nothing takes v's
 * address and mem2reg promotes it. Writing v on a match as well stores
 * the value it already holds, which a private local cannot show. */
struct cx_read { struct ir_ins *i; const int *of; int nv; signed char *st; };
static void cx_read_cb(int *p, void *ctx)
{
    struct cx_read *r = ctx;
    int t = *p;
    if (t < 0 || t >= r->nv || r->of[t] < 0)
        return;
    /* the expected operand of a compare-exchange as wide as the local,
     * and not also its object or its desired value */
    if (r->i->op == IR_CMPXCHG && p == &r->i->b && r->i->a != t &&
        r->i->c != t)
        return;
    r->st[r->of[t]] = -1;
}

static int pass_cxlocal(struct ir_func *fn)
{
    int nv = fn->nvregs, nvars = fn->nvars;
    if (nvars == 0 || fn->nins == 0 || fn->neh)
        return 0;
    struct defs d;
    compute_defs(fn, &d);
    /* per local: 0 untouched, 1 a candidate, -1 refused */
    signed char *st = xcalloc((size_t)nvars, 1);
    int *of = xmalloc((size_t)nv * sizeof *of);   /* temp -> its local */
    for (int v = 0; v < nv; v++)
        of[v] = -1;
    int any = 0;
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        if (i->op != IR_ADDR || i->a < 0 || i->a >= nvars)
            continue;
        if (i->dst < nvars || i->dst >= nv || d.cnt[i->dst] != 1) {
            st[i->a] = -1;
            continue;
        }
        of[i->dst] = i->a;
        if (st[i->a] == 0)
            st[i->a] = 1;
    }
    for (int L = 0; L < nvars; L++) {
        const struct ir_local *Li = &fn->locals[L];
        if (st[L] == 1 &&
            (!Li->is_int_or_ptr || Li->is_int128 || Li->is_volatile ||
             (Li->size != 1 && Li->size != 2 && Li->size != 4 &&
              Li->size != 8)))
            st[L] = -1;
    }
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        struct cx_read r = { i, of, nv, st };
        if (i->op == IR_ADDR)
            continue;                     /* its `a` is the slot itself */
        each_read(i, cx_read_cb, &r);
        if (i->op == IR_CMPXCHG && i->b >= 0 && i->b < nv && of[i->b] >= 0 &&
            (i->size != fn->locals[of[i->b]].size || i->flt))
            st[of[i->b]] = -1;
    }
    for (int L = 0; L < nvars; L++)
        if (st[L] == 1)
            any = 1;
    if (!any) {
        free(st); free(of); free_defs(&d);
        return 0;
    }

    struct ibuf nb = { 0, 0, 0 };
    int *newpos = fn->var_scope_lo
        ? xmalloc((size_t)(fn->nins + 1) * sizeof *newpos) : NULL;
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins o = fn->ins[n];
        if (newpos) newpos[n] = nb.n;
        if (o.op == IR_ADDR && o.a >= 0 && o.a < nvars && st[o.a] == 1)
            continue;                     /* read by nothing now */
        if (o.op != IR_CMPXCHG || o.b < 0 || o.b >= nv || of[o.b] < 0 ||
            st[of[o.b]] != 1) {
            *ib_push(&nb) = o;
            continue;
        }
        int L = of[o.b];
        int e = fn->nvregs++, sv = fn->nvregs++, x = sv;
        struct ir_ins *p = ib_push(&nb);
        p->op = IR_LDVAR; p->a = L; p->dst = e;
        p->size = o.size; p->sign = o.sign; p->w = o.w;
        p->line = o.line; p->col = o.col; p->synth = o.synth;
        p = ib_push(&nb);
        p->op = IR_CAS; p->a = o.a; p->b = e; p->c = o.c; p->dst = sv;
        p->size = o.size; p->sign = o.sign; p->w = o.w;
        p->line = o.line; p->col = o.col; p->synth = o.synth;
        if (o.size < 4) {
            x = fn->nvregs++;
            p = ib_push(&nb);
            p->op = IR_EXT; p->a = sv; p->dst = x;
            p->size = o.size; p->sign = o.sign; p->w = o.w;
            p->line = o.line; p->col = o.col; p->synth = o.synth;
        }
        p = ib_push(&nb);
        p->op = IR_STVAR; p->dst = L; p->a = x; p->size = o.size;
        p->line = o.line; p->col = o.col; p->synth = o.synth;
        p = ib_push(&nb);
        p->op = IR_CMP; p->pred = B_EQ; p->a = x; p->b = e; p->dst = o.dst;
        p->w = o.w; p->sign = o.sign;
        p->line = o.line; p->col = o.col; p->synth = o.synth;
    }
    if (newpos) {
        newpos[fn->nins] = nb.n;
        for (int v = 0; v < nvars; v++) {
            int lo = fn->var_scope_lo[v], hi = fn->var_scope_hi[v];
            if (lo >= 0 && lo <= fn->nins) fn->var_scope_lo[v] = newpos[lo];
            if (hi >= 0 && hi <= fn->nins) fn->var_scope_hi[v] = newpos[hi];
        }
        free(newpos);
    }
    free(fn->ins);
    fn->ins = nb.p; fn->nins = nb.n; fn->cap = nb.cap;
    free(st); free(of); free_defs(&d);
    return 1;
}

static int pass_sroa(struct ir_func *fn, int report_refusals)
{
    int nvars = fn->nvars, nparams = fn->nparams, nvr = fn->nvregs;
    if (nvars <= nparams || fn->nins == 0 || !fn->src || !fn->src->var_tys)
        return 0;

    struct defs d;
    compute_defs(fn, &d);

    /* 1. Which locals are worth asking about. Aggregates are the point,
     * but a SCALAR belongs here too: mem2reg refuses one whose address
     * is taken, and `int x; int *p = &x; ... *p` is that -- a one-field
     * object, split into one variable, which mem2reg can then promote.
     * A local nothing takes the address of falls out with no fields and
     * costs a scan. Parameters stay out: a struct one is written to its
     * slot by the PROLOGUE, which is not IR this pass can see. */
    char *cand = xcalloc((size_t)nvars, 1);
    const char **why = xcalloc((size_t)nvars, sizeof *why);
    int ncand = 0;
    /* Only a local something takes the ADDRESS of is this pass's
     * business. Without that there is nothing to split and nothing to
     * explain -- mem2reg already answers for it -- and asking anyway
     * would put a line in the remarks for every variable in the
     * program. */
    for (int n = 0; n < fn->nins; n++)
        if (fn->ins[n].op == IR_ADDR && fn->ins[n].a >= nparams &&
            fn->ins[n].a < nvars)
            cand[fn->ins[n].a] = 1;
    for (int L = nparams; L < nvars; L++) {
        const struct ir_local *Li = &fn->locals[L];
        struct type *t = fn->src->var_tys[L];
        if (!cand[L])                                continue;
        cand[L] = 0;
        if (!t || Li->size <= 0)                     continue;
        if (Li->is_volatile)      { why[L] = "declared-volatile";   continue; }
        if (Li->is_int128 || Li->is_ldouble)
                                  { why[L] = "is-a-wide-scalar";    continue; }
        if (Li->size > SROA_MAX_SIZE)
                                  { why[L] = "too-large-to-split";  continue; }
        if (t->kind == TY_ARRAY && !t->count)
                                  { why[L] = "is-a-variable-length-array"; continue; }
        cand[L] = 1; ncand++;
    }
    if (!ncand) {
        /* Not nothing to say: "declared volatile" is a refusal, and the
         * only local in its function can be the one that was refused. */
        sroa_report(fn, nparams, nvars, cand, NULL, why, report_refusals);
        free(cand); free(why); free_defs(&d);
        return 0;
    }

    /* 2. Follow `&L + constant` forward. Two rounds, because a use can
     * textually precede its definition across a back edge; the chains
     * themselves are two or three instructions long. */
    int *abase = xmalloc((size_t)(nvr ? nvr : 1) * sizeof *abase);
    long *aoff = xmalloc((size_t)(nvr ? nvr : 1) * sizeof *aoff);
    for (int v = 0; v < nvr; v++) { abase[v] = -1; aoff[v] = 0; }
    for (int round = 0; round < 2; round++)
        for (int n = 0; n < fn->nins; n++) {
            struct ir_ins *in = &fn->ins[n];
            int dst = def_target(in);
            int single = dst >= 0 && dst < nvr && d.cnt[dst] == 1;
            if (in->op == IR_ADDR) {
                int L = in->a;
                if (L < 0 || L >= nvars || !cand[L])
                    continue;
                if (!single) {          /* the address lands where we cannot follow it */
                    cand[L] = 0; why[L] = "address-held-in-a-reassigned-temp";
                    continue;
                }
                abase[dst] = L; aoff[dst] = 0;
            } else if (in->op == IR_MOV) {
                if (in->a >= 0 && in->a < nvr && abase[in->a] >= 0 && single) {
                    abase[dst] = abase[in->a];
                    aoff[dst] = aoff[in->a];
                }
            } else if (in->op == IR_ADD && in->w == 8) {
                int bs = -1; long k = 0; int have = 0;
                if (in->a >= 0 && in->a < nvr && abase[in->a] >= 0) {
                    bs = in->a;
                    have = in->imm_b ? (k = in->imm, 1)
                                     : sroa_const(fn, &d, in->b, &k, 0);
                } else if (!in->imm_b && in->b >= 0 && in->b < nvr &&
                           abase[in->b] >= 0) {
                    bs = in->b;
                    have = sroa_const(fn, &d, in->a, &k, 0);
                }
                if (bs >= 0 && have && single) {
                    abase[dst] = abase[bs];
                    aoff[dst] = aoff[bs] + k;
                }
            }
        }

    /* 3. Check every use, and collect the pieces. */
    struct sroa_fld *fld =
        xcalloc((size_t)nvars * SROA_MAX_FIELDS, sizeof *fld);
    int *nfld = xcalloc((size_t)nvars, sizeof *nfld);
    struct sroa_ctx sc = { cand, why, abase, nvr };
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *in = &fn->ins[n];
        switch (in->op) {
        case IR_ADDR:
            break;                  /* the root; its `a` is a slot, not a read */
        case IR_MOV: case IR_ADD:
            /* Accounted for when the result was tracked above. Otherwise
             * the address flowed into arithmetic this pass cannot follow. */
            if (in->dst >= 0 && in->dst < nvr && abase[in->dst] >= 0)
                break;
            each_read(in, sroa_esc_cb, &sc);
            break;
        case IR_LDVAR: case IR_LOAD: case IR_STORE: case IR_STVAR: {
            /* A store whose VALUE is the address is the address escaping. */
            if (in->op == IR_STORE && in->b >= 0 && in->b < nvr &&
                abase[in->b] >= 0 && cand[abase[in->b]]) {
                cand[abase[in->b]] = 0; why[abase[in->b]] = "address-escapes";
            }
            if (in->op == IR_STVAR)
                each_read(in, sroa_esc_cb, &sc);   /* the value it writes */
            /* A whole-object read or write is an access like any other:
             * at offset 0, of the object's own width. For an aggregate
             * that width is not 1, 2, 4 or 8 and the local is refused
             * just below; for a SCALAR whose address was taken it is
             * exactly the one piece, so `int x = a; int *p = &x; *p = 2`
             * splits instead of being refused for mentioning x by name. */
            int L, sz = in->size;
            long off;
            if (in->op == IR_LDVAR || in->op == IR_STVAR) {
                L = in->op == IR_LDVAR ? in->a : in->dst;
                if (L < 0 || L >= nvars || !cand[L])
                    break;
                off = 0;
            } else {
                int ad = in->a;
                if (ad < 0 || ad >= nvr || abase[ad] < 0)
                    break;
                L = abase[ad];
                if (!cand[L])
                    break;
                off = aoff[ad];
            }
            if (in->vol) {
                cand[L] = 0; why[L] = "access-is-volatile"; break;
            }
            if (sz != 1 && sz != 2 && sz != 4 && sz != 8) {
                cand[L] = 0; why[L] = "access-is-not-1-2-4-or-8-bytes"; break;
            }
            if (off < 0 || off + sz > fn->locals[L].size) {
                cand[L] = 0; why[L] = "access-runs-outside-the-object"; break;
            }
            struct sroa_fld *F = &fld[(size_t)L * SROA_MAX_FIELDS];
            int k = 0;
            for (; k < nfld[L]; k++) {
                if (F[k].off == off && F[k].size == sz)
                    break;                        /* a piece already named */
                if (off < F[k].off + F[k].size && F[k].off < off + sz) {
                    cand[L] = 0;
                    why[L] = "two-accesses-overlap-at-different-widths";
                    break;
                }
            }
            if (!cand[L] || k < nfld[L])
                break;
            if (nfld[L] == SROA_MAX_FIELDS) {
                cand[L] = 0; why[L] = "too-many-pieces"; break;
            }
            F[nfld[L]].off = (int)off;
            F[nfld[L]].size = sz;
            nfld[L]++;
            break;
        }
        default:
            each_read(in, sroa_esc_cb, &sc);
            break;
        }
    }

    /* 4. Number the new locals. A candidate nothing ever reached through
     * the chain has nothing to split. */
    int nnew = 0;
    for (int L = nparams; L < nvars; L++) {
        if (cand[L] && nfld[L] == 0)
            cand[L] = 0;          /* nothing ever reached it through the chain */
        if (cand[L])
            nnew += nfld[L];
    }
    sroa_report(fn, nparams, nvars, cand, nfld, why, report_refusals);
    if (!nnew) {
        free(cand); free(why); free(abase); free(aoff);
        free(fld); free(nfld); free_defs(&d);
        return 0;
    }
    int next = nvars;
    for (int L = nparams; L < nvars; L++)
        if (cand[L])
            for (int k = 0; k < nfld[L]; k++)
                fld[(size_t)L * SROA_MAX_FIELDS + k].nl = next++;

    /* 5. Decide each rewrite BEFORE renumbering, because abase/aoff are
     * indexed by the old vreg numbers and the new locals occupy exactly
     * the range the temps are about to move out of. */
    int *rw = xcalloc((size_t)(fn->nins ? fn->nins : 1), sizeof *rw);
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *in = &fn->ins[n];
        int L, off;
        if (in->op == IR_LDVAR || in->op == IR_STVAR) {
            L = in->op == IR_LDVAR ? in->a : in->dst;
            off = 0;
            if (L < 0 || L >= nvars || !cand[L])
                continue;
        } else if (in->op == IR_LOAD || in->op == IR_STORE) {
            int ad = in->a;
            if (ad < 0 || ad >= nvr || abase[ad] < 0 || !cand[abase[ad]])
                continue;
            L = abase[ad];
            off = (int)aoff[ad];
        } else {
            continue;
        }
        struct sroa_fld *F = &fld[(size_t)L * SROA_MAX_FIELDS];
        for (int k = 0; k < nfld[L]; k++)
            if (F[k].off == off && F[k].size == in->size) {
                rw[n] = F[k].nl + 1;
                break;
            }
    }

    /* 6. Grow the slot metadata -- both representations together, which is
     * the invariant ir.h warns about -- and shift the temps up. */
    int nv = nvars + nnew;
    struct type **vt = xmalloc((size_t)nv * sizeof *vt);
    int *va = xmalloc((size_t)nv * sizeof *va);
    for (int k = 0; k < nvars; k++) {
        vt[k] = fn->src->var_tys[k];
        va[k] = fn->src->var_aligns ? fn->src->var_aligns[k] : 0;
    }
    for (int L = nparams; L < nvars; L++) {
        if (!cand[L])
            continue;
        struct sroa_fld *F = &fld[(size_t)L * SROA_MAX_FIELDS];
        for (int k = 0; k < nfld[L]; k++) {
            struct type *mt =
                sroa_field_ty(fn->src->var_tys[L], F[k].off, F[k].size);
            if (!mt || mt->is_volatile || mt->kind == TY_LDOUBLE ||
                mt->kind == TY_INT128)
                mt = ty_base(F[k].size == 8 ? TY_LONG :
                             F[k].size == 4 ? TY_INT :
                             F[k].size == 2 ? TY_SHORT : TY_CHAR, 0);
            vt[F[k].nl] = mt;
            va[F[k].nl] = 0;        /* a piece nothing addresses needs no more
                                     * than its type's natural alignment */
        }
    }
    fn->src->var_tys = vt;
    fn->src->var_aligns = va;

    if (fn->var_scope_lo) {
        /* Each piece lives exactly as long as the object it came out of. */
        int *lo = xmalloc((size_t)nv * sizeof *lo);
        int *hi = xmalloc((size_t)nv * sizeof *hi);
        memcpy(lo, fn->var_scope_lo, (size_t)nvars * sizeof *lo);
        memcpy(hi, fn->var_scope_hi, (size_t)nvars * sizeof *hi);
        for (int L = nparams; L < nvars; L++) {
            if (!cand[L])
                continue;
            struct sroa_fld *F = &fld[(size_t)L * SROA_MAX_FIELDS];
            for (int k = 0; k < nfld[L]; k++) {
                lo[F[k].nl] = lo[L];
                hi[F[k].nl] = hi[L];
            }
        }
        free(fn->var_scope_lo); free(fn->var_scope_hi);
        fn->var_scope_lo = lo; fn->var_scope_hi = hi;
    }

    struct rmp shift = { 0, nvars, nnew, 0, 0 };
    for (int n = 0; n < fn->nins; n++)
        remap_ins(&fn->ins[n], &shift);
    fn->nvregs = nvr + nnew;
    fn->nvars = nv;
    ir_locals_fill(fn, fn->src, nv);

    /* 7. And the accesses become ordinary variable reads and writes. The
     * size/sign/width of each one is carried over untouched: an LDVAR
     * extends on the way in exactly as the LOAD it replaces did. */
    int nrw = 0;
    for (int n = 0; n < fn->nins; n++) {
        if (!rw[n])
            continue;
        struct ir_ins *in = &fn->ins[n];
        int NL = rw[n] - 1;
        switch (in->op) {
        case IR_LOAD:
            in->op = IR_LDVAR;
            in->a = NL;
            break;
        case IR_STORE:
            in->op = IR_STVAR;
            in->a = in->b;          /* IR_STORE's value operand */
            in->b = -1;
            in->dst = NL;
            break;
        case IR_LDVAR:
            in->a = NL;             /* already a variable read: just retarget */
            break;
        default:                    /* IR_STVAR */
            in->dst = NL;
            break;
        }
        nrw++;
    }
    g_did.sroa += nrw;

    free(rw); free(cand); free(why); free(abase); free(aoff);
    free(fld); free(nfld); free_defs(&d);
    return 1;
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
    free_cfg(bb, nbb);
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
    { "idiom",     0, 0 },   /* a copy or clear loop becomes memcpy/memzero */
    { "sroa",      0, 0 },   /* split a private aggregate into scalars */
    { "unroll",    0, 0 },   /* copies of a body, one test between them */
    { "pre",       0, 0 },   /* compute it on the path that lacked it */
    { "switch-thread", 0, 0 }, /* a known state jumps straight to its case */
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
#define P_IDIOM    12
#define P_SROA     13
#define P_UNROLL   14
#define P_PRE      15
#define P_SWTHREAD 16


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
#define g_idiom    (g_pass[P_IDIOM].on)
#define g_sroa     (g_pass[P_SROA].on)
#define g_unroll   (g_pass[P_UNROLL].on)
#define g_pre      (g_pass[P_PRE].on)
#define g_swthread (g_pass[P_SWTHREAD].on)

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
    /* (3) every target a switch names is a label this function places:
     * a table entry left behind by a pass that renumbered or dropped
     * labels is a jump into nowhere. */
    {
        char *placed = xcalloc((size_t)(fn->nlabels ? fn->nlabels : 1), 1);
        for (int n = 0; n < fn->nins; n++)
            if (fn->ins[n].op == IR_LABEL && fn->ins[n].label >= 0 &&
                fn->ins[n].label < fn->nlabels)
                placed[fn->ins[n].label] = 1;
        for (int n = 0; n < fn->nins; n++) {
            const struct ir_ins *i = &fn->ins[n];
            if (i->op != IR_SWITCH)
                continue;
            if (i->jt < 0 || i->jt >= fn->njt || fn->jt[i->jt].n < 1)
                diag_fatal(fn->file, 0, "internal: %s has a switch with no "
                           "table (after %s)", fn->name, tag);
            for (int k = -1; k < fn->jt[i->jt].n; k++) {
                int l = k < 0 ? i->label : fn->jt[i->jt].labels[k];
                if (l < 0 || l >= fn->nlabels || !placed[l])
                    diag_fatal(fn->file, 0, "internal: %s: a switch names "
                               "label L%d, which nothing places (after %s)",
                               fn->name, l, tag);
            }
        }
        free(placed);
    }
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

#define OPT_MAX_ROUNDS 64   /* rounds of the block-local passes per outer round */
static void opt_no_fixpoint(struct ir_func *fn, int rounds)
{
    if (getenv("EMBCC_VERIFY"))
        diag_fatal(fn->file, fn->line,
                   "internal: the optimizer did not converge on '%s' after %d "
                   "rounds -- two passes are undoing each other's work",
                   fn->name, rounds);
    fprintf(stderr, "embcc: warning: the optimizer stopped after %d rounds on "
                    "'%s' without converging (the code is correct; this is a "
                    "compiler performance bug worth reporting)\n",
            rounds, fn->name);
}

/* ==== splitting a live range around a loop ================================
 *
 * A value that crosses a call takes a callee-saved register, and keeps
 * it everywhere it lives. When the call it crosses is OUTSIDE a loop and
 * the loop has none, the loop pays on every iteration: the loop's own
 * call hands the value over in an argument register and back in the
 * return register, and each trip copies it into the callee-saved one
 * and out again (kernels_main's `s = opaque_add(s, i)`: nine
 * instructions an iteration where seven do). The allocator cannot see
 * this -- one vreg, one register -- so the IR gives it two: inside the
 * loop the value goes by a new name, copied in on the entry edge and
 * back on every exit edge, and the new name crosses nothing.
 *
 * Shapes taken: a natural loop with one entry, from the block that falls
 * into its header; no switch, no indirect jump, no inline asm or landing
 * pad inside; exits that fall out, jump out, or branch out through a
 * trampoline that does the copy. Values taken: temps live into the
 * header, read or written inside, live across an IR_CALL in a block
 * outside the loop and across NONE inside it -- a call inside is fine
 * when the value is what it passes and returns (`s = opaque_add(s, i)`
 * hands s over in r0 and gets it back there), which is exactly the case
 * that pays. A call is an IR_CALL or an op the backend lowers to a
 * runtime helper (target_op_calls_helper: a soft-float divide after the
 * loop is what the Cortex-M4 bench crosses); the allocator judges by the
 * same predicate. Not at -Os: the copies are bytes. This runs last,
 * after every copy propagation that would fold the copies back in, one
 * loop per call. */
struct splitrd { int from, to; };
static void split_rd_cb(int *p, void *ctx)
{
    struct splitrd *c = ctx;
    if (*p == c->from) *p = c->to;
}
struct splituse { unsigned long *use, *def; int nv; };
static void split_use_cb(int *p, void *ctx)
{
    struct splituse *c = ctx;
    int v = *p;
    if (v < 0 || v >= c->nv) return;
    if (!(c->def[v >> 6] & (1UL << (v & 63))))
        c->use[v >> 6] |= 1UL << (v & 63);
}
#define BIT(set, v) ((set)[(v) >> 6] & (1UL << ((v) & 63)))

static int pass_splitloops(struct ir_func *fn)
{
    if (fn->nins == 0 || fn->nvregs == 0)
        return 0;
    int nbb, *l2b;
    struct bb *bb = build_cfg(fn, &nbb, &l2b);
    int *order = xmalloc((size_t)(nbb ? nbb : 1) * sizeof *order), norder;
    compute_rpo(bb, nbb, order, &norder);
    int changed = 0;
    if (norder != nbb)
        goto out;
    compute_idom(bb, order, norder);
    /* Nothing to split without a loop, and the liveness below is blocks x
     * vregs bits: a 4000-case switch, which has no loop at all, built and
     * swept 200 MB of it here. The headers looked for are the ones the
     * walk below would take. */
    {
        int any_loop = 0;
        for (int h = 1; h < nbb && !any_loop; h++) {
            if (bb[h].end <= bb[h].start || fn->ins[bb[h].start].op != IR_LABEL)
                continue;
            for (int q = 0; q < bb[h].npred && !any_loop; q++)
                if (bb_dominates(bb, h, bb[h].pred[q]))
                    any_loop = 1;
        }
        if (!any_loop)
            goto out;
    }
    int nv = fn->nvregs, words = (nv + 63) / 64;

    /* The width and class of each temp, from any definition. */
    int *vw = xcalloc((size_t)nv, sizeof *vw);
    char *vflt = xcalloc((size_t)nv, 1);
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        int t = i->op == IR_STVAR ? -1 : def_target(i);
        if (t < 0 || t >= nv || vw[t]) continue;
        vw[t] = i->op == IR_CALL ? (i->w ? i->w : 8) : i->w;
        vflt[t] = (char)i->flt;
    }

    /* Block liveness. */
    unsigned long *use = xcalloc((size_t)nbb * words, sizeof *use);
    unsigned long *def = xcalloc((size_t)nbb * words, sizeof *def);
    unsigned long *lin = xcalloc((size_t)nbb * words, sizeof *lin);
    unsigned long *lout = xcalloc((size_t)nbb * words, sizeof *lout);
    for (int b = 0; b < nbb; b++) {
        struct splituse su = { use + (size_t)b * words, def + (size_t)b * words, nv };
        for (int n = bb[b].start; n < bb[b].end; n++) {
            each_read(&fn->ins[n], split_use_cb, &su);
            int t = def_target(&fn->ins[n]);
            if (t >= 0 && t < nv) su.def[t >> 6] |= 1UL << (t & 63);
        }
    }
    for (int again = 1; again; ) {
        again = 0;
        for (int b = nbb - 1; b >= 0; b--) {
            unsigned long *o = lout + (size_t)b * words;
            for (int w = 0; w < words; w++) o[w] = 0;
            for (int k = 0; k < bb[b].nsucc; k++) {
                unsigned long *si = lin + (size_t)bb[b].succ[k] * words;
                for (int w = 0; w < words; w++) o[w] |= si[w];
            }
            unsigned long *ii = lin + (size_t)b * words;
            for (int w = 0; w < words; w++) {
                unsigned long nvl = use[(size_t)b * words + w] |
                                    (o[w] & ~def[(size_t)b * words + w]);
                if (nvl != ii[w]) { ii[w] = nvl; again = 1; }
            }
        }
    }
    /* Per block: the temps live across a call in it. */
    unsigned long *xcall = xcalloc((size_t)nbb * words, sizeof *xcall);
    {
        unsigned long *live = xmalloc((size_t)words * sizeof *live);
        for (int b = 0; b < nbb; b++) {
            for (int w = 0; w < words; w++) live[w] = lout[(size_t)b * words + w];
            for (int n = bb[b].end - 1; n >= bb[b].start; n--) {
                const struct ir_ins *i = &fn->ins[n];
                int t = def_target(i);
                if (i->op == IR_CALL || target_op_calls_helper(i))
                    for (int w = 0; w < words; w++) {
                        unsigned long m = live[w];
                        if (t >= 0 && (t >> 6) == w) m &= ~(1UL << (t & 63));
                        xcall[(size_t)b * words + w] |= m;
                    }
                if (t >= 0 && t < nv) live[t >> 6] &= ~(1UL << (t & 63));
                struct splituse su = { live, live, nv };   /* def = live: always set */
                /* a read is live before the instruction whatever follows */
                su.def = xcalloc((size_t)words, sizeof *su.def);
                each_read(&fn->ins[n], split_use_cb, &su);
                free(su.def);
            }
        }
        free(live);
    }

    char *in = xmalloc((size_t)(nbb ? nbb : 1));
    unsigned long *xout = xmalloc((size_t)words * sizeof *xout);
    unsigned long *inside = xmalloc((size_t)words * sizeof *inside);
    /* Headers innermost first. */
    for (int oi = norder - 1; oi >= 0 && !changed; oi--) {
        int h = order[oi];
        if (h == 0 || bb[h].end <= bb[h].start ||
            fn->ins[bb[h].start].op != IR_LABEL)
            continue;
        int Lh = fn->ins[bb[h].start].label;
        memset(in, 0, (size_t)nbb);
        int any_back = 0;
        for (int q = 0; q < bb[h].npred; q++) {   /* back edges into h */
            int p = bb[h].pred[q];
            if (bb_dominates(bb, h, p)) {
                loop_body(bb, nbb, h, p, in);
                any_back = 1;
            }
        }
        if (!any_back)
            continue;
        /* One entry, from the block that falls into the header. */
        int ok = 1, pe = -1;
        for (int b = 0; b < nbb && ok; b++) {
            if (!in[b]) continue;
            for (int k = 0; k < bb[b].npred; k++) {
                int p = bb[b].pred[k];
                if (in[p]) continue;
                if (b != h || pe >= 0) ok = 0;
                else pe = p;
            }
        }
        if (!ok || pe != h - 1 || bb[pe].end != bb[h].start || bb[pe].end <= bb[pe].start)
            continue;
        {
            const struct ir_ins *lt = &fn->ins[bb[pe].end - 1];
            if (lt->op == IR_JMP || lt->op == IR_RET || lt->op == IR_UD2 ||
                lt->op == IR_IGOTO || lt->op == IR_SWITCH)
                continue;                       /* no fall-through edge */
            if ((lt->op == IR_BRZ || lt->op == IR_BRNZ) && lt->label == Lh)
                continue;                       /* the taken edge skips the copy */
        }
        /* Nothing inside the loop a copy could not be placed around. */
        for (int w = 0; w < words; w++) inside[w] = 0;
        for (int b = 0; b < nbb && ok; b++) {
            if (!in[b]) continue;
            for (int n = bb[b].start; n < bb[b].end; n++) {
                const struct ir_ins *i = &fn->ins[n];
                if (i->op == IR_SWITCH || i->op == IR_IGOTO ||
                    i->op == IR_ASM || i->op == IR_LANDING || i->op == IR_LABELADDR)
                    { ok = 0; break; }
                int t = def_target(i);
                if (t >= 0 && t < nv) inside[t >> 6] |= 1UL << (t & 63);
            }
            for (int w = 0; w < words; w++) inside[w] |= use[(size_t)b * words + w];
        }
        if (!ok)
            continue;
        /* xout: live across a call outside the loop; xin: across one inside */
        unsigned long *xin = inside;      /* reused below, after the candidate test */
        unsigned long *xinb = xmalloc((size_t)words * sizeof *xinb);
        for (int w = 0; w < words; w++) { xout[w] = 0; xinb[w] = 0; }
        for (int b = 0; b < nbb; b++)
            for (int w = 0; w < words; w++)
                (in[b] ? xinb : xout)[w] |= xcall[(size_t)b * words + w];
        (void)xin;
        /* The candidates: live into the header, touched inside, live
         * across a call outside and across none inside. */
        int v = -1;
        for (int c = fn->nvars; c < nv; c++)
            if (BIT(lin + (size_t)h * words, c) && BIT(inside, c) && BIT(xout, c) &&
                !BIT(xinb, c) && (vw[c] == 4 || vw[c] == 8)) { v = c; break; }
        free(xinb);
        if (v < 0)
            continue;
        /* ---- rewrite ---- */
        int v2 = fn->nvregs++;
        int lineh = fn->ins[bb[h].start].line, colh = fn->ins[bb[h].start].col;
        struct splitrd rd = { v, v2 };
        for (int b = 0; b < nbb; b++) {
            if (!in[b]) continue;
            for (int n = bb[b].start; n < bb[b].end; n++) {
                struct ir_ins *i = &fn->ins[n];
                each_read(i, split_rd_cb, &rd);
                if (i->op != IR_STVAR && def_target(i) == v)
                    i->dst = v2;
            }
        }
        /* Where the copies go: `at` is an instruction index, kind 0 = before
         * it, 1 = after it (at == nins: the end). Trampolines are appended. */
        struct cp { int at, kind, dst, src; } *cps = NULL; int ncps = 0, ccps = 0;
        struct tr { int lbl, dst, src, to; } *trs = NULL; int ntrs = 0, ctrs = 0;
#define ADDCP(a, k, d, s) do { if (ncps == ccps) { ccps = ccps ? ccps * 2 : 8; \
            cps = xrealloc(cps, (size_t)ccps * sizeof *cps); } \
            cps[ncps].at = (a); cps[ncps].kind = (k); cps[ncps].dst = (d); \
            cps[ncps].src = (s); ncps++; } while (0)
        ADDCP(bb[h].start, 0, v2, v);                 /* the entry edge */
        for (int b = 0; b < nbb; b++) {
            if (!in[b]) continue;
            struct ir_ins *lt = &fn->ins[bb[b].end - 1];
            for (int k = 0; k < bb[b].nsucc; k++) {
                int s = bb[b].succ[k];
                if (in[s]) continue;
                if (!BIT(lin + (size_t)s * words, v))
                    continue;                     /* dead past this exit */
                int Ls = bb[s].end > bb[s].start && fn->ins[bb[s].start].op == IR_LABEL
                         ? fn->ins[bb[s].start].label : -1;
                int falls = bb[b].end == bb[s].start &&
                            lt->op != IR_JMP && lt->op != IR_RET && lt->op != IR_UD2;
                if (lt->op == IR_JMP && lt->label == Ls) {
                    ADDCP(bb[b].end - 1, 0, v, v2);  /* before the jump */
                } else if ((lt->op == IR_BRZ || lt->op == IR_BRNZ) && lt->label == Ls &&
                           Ls >= 0) {
                    if (ntrs == ctrs) { ctrs = ctrs ? ctrs * 2 : 4;
                        trs = xrealloc(trs, (size_t)ctrs * sizeof *trs); }
                    trs[ntrs].lbl = fn->nlabels++; trs[ntrs].dst = v;
                    trs[ntrs].src = v2; trs[ntrs].to = Ls;
                    lt->label = trs[ntrs].lbl;
                    ntrs++;
                    if (falls && bb[b].end == bb[s].start)
                        ADDCP(bb[b].end - 1, 1, v, v2);  /* both arms leave */
                } else if (falls) {
                    ADDCP(bb[b].end - 1, 1, v, v2);      /* after the block */
                } else {
                    ok = 0;                            /* an edge of no known shape */
                }
            }
        }
        if (!ok) {
            /* undo the rename: the shapes above are the only ones, so this
             * cannot happen, but a split half done is a miscompile */
            diag_fatal(fn->file, lineh, "internal: %s: a loop exit of a shape "
                       "the live-range split does not know", fn->name);
        }
        /* ---- rebuild ---- */
        struct ibuf nb = { 0, 0, 0 };
        int *newpos = fn->var_scope_lo
            ? xmalloc((size_t)(fn->nins + 1) * sizeof *newpos) : NULL;
        for (int n = 0; n <= fn->nins; n++) {
            if (newpos) newpos[n] = nb.n;
            for (int c = 0; c < ncps; c++)
                if (cps[c].at == n && cps[c].kind == 0) {
                    struct ir_ins *m = ib_push(&nb);
                    memset(m, 0, sizeof *m);
                    m->op = IR_MOV; m->dst = cps[c].dst; m->a = cps[c].src; m->b = -1;
                    m->w = vw[v]; m->flt = vflt[v]; m->line = lineh; m->col = colh;
                    m->synth = 1;
                }
            if (n == fn->nins) break;
            *ib_push(&nb) = fn->ins[n];
            for (int c = 0; c < ncps; c++)
                if (cps[c].at == n && cps[c].kind == 1) {
                    struct ir_ins *m = ib_push(&nb);
                    memset(m, 0, sizeof *m);
                    m->op = IR_MOV; m->dst = cps[c].dst; m->a = cps[c].src; m->b = -1;
                    m->w = vw[v]; m->flt = vflt[v]; m->line = lineh; m->col = colh;
                    m->synth = 1;
                }
        }
        /* The trampolines go after the last instruction -- so if that
         * one can fall through, it would fall INTO the first of them: a
         * void function's implicit return ran the exit copy and jumped
         * back to the code after the loop, and lib/libcxx's rb-tree
         * erase never returned. Cap it with the return it stands for,
         * as mem2reg's trampolines do. */
        if (ntrs > 0) {
            enum ir_op lt = nb.n ? nb.p[nb.n - 1].op : IR_UD2;
            if (lt != IR_JMP && lt != IR_RET && lt != IR_UD2 &&
                lt != IR_IGOTO && lt != IR_SWITCH) {
                struct ir_ins *r = ib_push(&nb);
                memset(r, 0, sizeof *r);
                r->op = IR_RET; r->dst = r->a = r->b = -1;
                r->synth = 1;
            }
        }
        for (int t = 0; t < ntrs; t++) {
            struct ir_ins *l = ib_push(&nb);
            memset(l, 0, sizeof *l);
            l->op = IR_LABEL; l->label = trs[t].lbl; l->dst = l->a = l->b = -1;
            l->line = lineh; l->col = colh; l->synth = 1;
            struct ir_ins *m = ib_push(&nb);
            memset(m, 0, sizeof *m);
            m->op = IR_MOV; m->dst = trs[t].dst; m->a = trs[t].src; m->b = -1;
            m->w = vw[v]; m->flt = vflt[v]; m->line = lineh; m->col = colh; m->synth = 1;
            struct ir_ins *j = ib_push(&nb);
            memset(j, 0, sizeof *j);
            j->op = IR_JMP; j->label = trs[t].to; j->dst = j->a = j->b = -1;
            j->line = lineh; j->col = colh; j->synth = 1;
        }
        if (newpos) {
            remap_scopes(fn, newpos, fn->nins);
            free(newpos);
        }
        free(fn->ins);
        fn->ins = nb.p; fn->nins = nb.n; fn->cap = nb.cap;
        free(cps); free(trs);
#undef ADDCP
        if (remarks_on() && fn->src)
            remark_add("regalloc", "split", fn->name, "loop-live-range",
                       fn->file, lineh ? lineh : fn->line,
                       "a value live across a call outside a loop goes by "
                       "another name inside it");
        changed = 1;
    }
    free(in); free(xout); free(inside);
    free(use); free(def); free(lin); free(lout); free(xcall);
    free(vw); free(vflt);
out:
    free(order); free(l2b);
    free_cfg(bb, nbb);
    return changed;
}
#undef BIT

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

static int pass_rangecheck(struct ir_func *fn)
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

/* ==== a join's copies, coalesced ==========================================
 *
 * Leaving SSA puts a copy on every edge into a join, and nested choices
 * make chains of them: `st = c ? (d ? 1 : 2) : 3` is
 *
 *      %197 = const 1 ... jmp L21      L21: %198 = mov %197
 *      %198 = const 2 ... jmp L19      L19: %199 = mov %198
 *                                      L14: %188 = mov %199; jmp L5
 *
 * Every backend's allocator gives the four names one register and the
 * copies vanish -- leaving L21, L19 and L14 as blocks that do nothing but
 * fall or jump to the next, so each arm pays a jump to a jump. CoreMark's
 * state machine took one per character. Done here, in the IR, the copies
 * go and the blocks are empty before cfgclean threads the jumps.
 *
 * `%b = mov %a` takes %a's name away -- every definition of %a writes %b
 * instead, and the copy goes -- when %a is a temp the copy is the one
 * reader of, of the same width and class as %b, and the two never hold
 * different live values at once: %b is dead right after each definition
 * of %a (its old value is not wanted), and %a is dead right after each
 * other definition of %b (no %a value is in flight to the copy when %b
 * is written). That is the allocator's own interference test, made here
 * where the CFG still knows which blocks become empty. Not when a
 * definition of %a reads %b: that is an update in place, which needs
 * nothing from here. Liveness is per block, updated by union as names
 * merge; each question about one point is a walk to the end of its
 * block. Functions with inline asm (whose outputs are not definitions to
 * the rest of this file) or exception edges are left alone. */
/* Block liveness as a list per vreg of the blocks it is live out of,
 * ascending -- the least solution of the usual equations over build_cfg's
 * blocks, found one vreg at a time from the blocks that read it before
 * writing it, back through predecessors to the blocks that write it.
 * `slots` says whether an LDVAR's or ADDR's slot operand counts as a read.
 *
 * What it replaces was a bit set per block of every vreg, iterated to a
 * fixpoint: blocks x vregs, which for a function of 4000 if statements
 * is 12000 blocks by 36000 vregs, 54 MB a set and four sets. */
struct vblk { int **b, *n, nv; };

struct vblk_ue { int *ust, *dst, b, nv; int *pv, *pb, np, cap; };
static void vblk_pair(struct vblk_ue *u, int v, int b)
{
    if (u->np == u->cap) {
        u->cap = u->cap ? u->cap * 2 : 256;
        u->pv = xrealloc(u->pv, (size_t)u->cap * sizeof *u->pv);
        u->pb = xrealloc(u->pb, (size_t)u->cap * sizeof *u->pb);
    }
    u->pv[u->np] = v;
    u->pb[u->np] = b;
    u->np++;
}
static void vblk_ue_cb(int *p, void *ctx)
{
    struct vblk_ue *u = ctx;
    int v = *p;
    if (v < 0 || v >= u->nv || u->dst[v] == u->b + 1 || u->ust[v] == u->b + 1)
        return;
    u->ust[v] = u->b + 1;
    vblk_pair(u, v, u->b);
}

/* (v, b) pairs -> per key, the other side, stably */
static void vblk_csr(const int *key, const int *val, int np, int nkey,
                     int **off_out, int **val_out)
{
    int *off = xcalloc((size_t)nkey + 1, sizeof *off);
    int *out = xmalloc((size_t)(np ? np : 1) * sizeof *out);
    for (int j = 0; j < np; j++) off[key[j] + 1]++;
    for (int k = 0; k < nkey; k++) off[k + 1] += off[k];
    int *fill = xmalloc((size_t)(nkey ? nkey : 1) * sizeof *fill);
    for (int k = 0; k < nkey; k++) fill[k] = off[k];
    for (int j = 0; j < np; j++) out[fill[key[j]]++] = val[j];
    free(fill);
    *off_out = off;
    *val_out = out;
}

static void vblk_build(struct ir_func *fn, const struct bb *bb, int nbb,
                       int slots, struct vblk *lv)
{
    int nv = fn->nvregs;
    lv->nv = nv;
    lv->b = xcalloc((size_t)(nv ? nv : 1), sizeof *lv->b);
    lv->n = xcalloc((size_t)(nv ? nv : 1), sizeof *lv->n);
    struct vblk_ue ue = { NULL, NULL, 0, nv, NULL, NULL, 0, 0 };
    struct vblk_ue df = { NULL, NULL, 0, nv, NULL, NULL, 0, 0 };
    ue.ust = xcalloc((size_t)(nv ? nv : 1), sizeof *ue.ust);
    ue.dst = xcalloc((size_t)(nv ? nv : 1), sizeof *ue.dst);
    for (int b = 0; b < nbb; b++) {
        ue.b = b;
        for (int n = bb[b].start; n < bb[b].end; n++) {
            struct ir_ins *i = &fn->ins[n];
            if (slots || (i->op != IR_LDVAR && i->op != IR_ADDR))
                each_read(i, vblk_ue_cb, &ue);
            int t = def_target(i);
            if (t >= 0 && t < nv && ue.dst[t] != b + 1) {
                ue.dst[t] = b + 1;
                vblk_pair(&df, t, b);
            }
        }
    }
    free(ue.ust); free(ue.dst);
    int *ueoff, *ueb, *dfoff, *dfb;
    vblk_csr(ue.pv, ue.pb, ue.np, nv, &ueoff, &ueb);
    vblk_csr(df.pv, df.pb, df.np, nv, &dfoff, &dfb);
    free(ue.pv); free(ue.pb); free(df.pv); free(df.pb);
    int *inmk = xcalloc((size_t)(nbb ? nbb : 1), sizeof *inmk);
    int *outmk = xcalloc((size_t)(nbb ? nbb : 1), sizeof *outmk);
    int *defmk = xcalloc((size_t)(nbb ? nbb : 1), sizeof *defmk);
    int *work = xmalloc((size_t)(nbb ? nbb : 1) * sizeof *work);
    int *tmp = xmalloc((size_t)(nbb ? nbb : 1) * sizeof *tmp);
    for (int v = 0; v < nv; v++) {
        if (ueoff[v] == ueoff[v + 1])
            continue;
        for (int k = dfoff[v]; k < dfoff[v + 1]; k++) defmk[dfb[k]] = v + 1;
        int nw = 0, nt = 0;
        for (int k = ueoff[v]; k < ueoff[v + 1]; k++) {
            inmk[ueb[k]] = v + 1;
            work[nw++] = ueb[k];
        }
        while (nw > 0) {
            int b = work[--nw];
            for (int k = 0; k < bb[b].npred; k++) {
                int p = bb[b].pred[k];
                if (outmk[p] != v + 1) {
                    outmk[p] = v + 1;
                    tmp[nt++] = p;
                }
                if (defmk[p] != v + 1 && inmk[p] != v + 1) {
                    inmk[p] = v + 1;
                    work[nw++] = p;
                }
            }
        }
        if (nt == 0)
            continue;
        /* ascending: an insertion sort is fine for the short lists, and a
         * marked pass over the blocks for a long one */
        int *l = xmalloc((size_t)nt * sizeof *l);
        if (nt <= 32) {
            for (int k = 0; k < nt; k++) {
                int x = tmp[k], j = k;
                while (j > 0 && l[j - 1] > x) { l[j] = l[j - 1]; j--; }
                l[j] = x;
            }
        } else {
            int m = 0;
            for (int b = 0; b < nbb; b++)
                if (outmk[b] == v + 1) l[m++] = b;
        }
        lv->b[v] = l;
        lv->n[v] = nt;
    }
    free(inmk); free(outmk); free(defmk); free(work); free(tmp);
    free(ueoff); free(ueb); free(dfoff); free(dfb);
}

static int vblk_has(const struct vblk *lv, int v, int b)
{
    if (v < 0 || v >= lv->nv)
        return 0;
    const int *l = lv->b[v];
    int lo = 0, hi = lv->n[v] - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (l[mid] == b) return 1;
        if (l[mid] < b) lo = mid + 1;
        else hi = mid - 1;
    }
    return 0;
}

/* b is now live wherever a was, and a nowhere */
static void vblk_merge(struct vblk *lv, int a, int b)
{
    int na = lv->n[a], nb2 = lv->n[b], m = 0, i = 0, j = 0;
    if (na == 0)
        return;
    int *l = xmalloc((size_t)(na + nb2) * sizeof *l);
    const int *x = lv->b[a], *y = lv->b[b];
    while (i < na || j < nb2) {
        int c;
        if (j >= nb2 || (i < na && x[i] < y[j])) c = x[i++];
        else if (i >= na || y[j] < x[i]) c = y[j++];
        else { c = x[i]; i++; j++; }
        l[m++] = c;
    }
    free(lv->b[a]); free(lv->b[b]);
    lv->b[a] = NULL; lv->n[a] = 0;
    lv->b[b] = l; lv->n[b] = m;
}

static void vblk_free(struct vblk *lv)
{
    for (int v = 0; v < lv->nv; v++) free(lv->b[v]);
    free(lv->b); free(lv->n);
}

static int jc_live_after(struct ir_func *fn, const char *gone, int n, int end,
                         int v, const struct vblk *lout, int blk)
{
    for (int k = n + 1; k < end; k++) {
        if (gone[k])
            continue;
        if (ins_reads(&fn->ins[k], v))
            return 1;
        if (def_target(&fn->ins[k]) == v)
            return 0;
    }
    return vblk_has(lout, v, blk);
}
struct jc_cnt { int *use; int nv; };
static void jc_cnt_cb(int *p, void *ctx)
{
    struct jc_cnt *c = ctx;
    if (*p >= 0 && *p < c->nv) c->use[*p]++;
}
static int pass_joincopies(struct ir_func *fn)
{
    if (fn->nins == 0 || fn->nvregs == 0 || fn->neh)
        return 0;
    int nv = fn->nvregs, nins = fn->nins;
    for (int n = 0; n < nins; n++)
        if (fn->ins[n].op == IR_ASM)
            return 0;
    int *nuse = xcalloc((size_t)nv, sizeof *nuse);
    int *vw = xcalloc((size_t)nv, sizeof *vw);
    char *vflt = xcalloc((size_t)nv, 1), *mixed = xcalloc((size_t)nv, 1);
    for (int n = 0; n < nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        struct jc_cnt c = { nuse, nv };
        if (i->op != IR_LDVAR && i->op != IR_ADDR)
            each_read(i, jc_cnt_cb, &c);
        int t = i->op == IR_STVAR ? -1 : def_target(i);
        if (t < 0 || t >= nv)
            continue;
        int w = i->op == IR_CALL ? (i->w ? i->w : 8) : i->w;
        if (!vw[t]) { vw[t] = w; vflt[t] = (char)i->flt; }
        else if (vw[t] != w || vflt[t] != (char)i->flt) mixed[t] = 1;
    }
    int any = 0;
    for (int n = 0; n < nins && !any; n++) {
        const struct ir_ins *i = &fn->ins[n];
        if (i->op == IR_MOV && i->a >= fn->nvars && i->a < nv &&
            i->dst >= fn->nvars && i->dst < nv && i->a != i->dst &&
            nuse[i->a] == 1)
            any = 1;
    }
    int changed = 0, nbb, *l2b;
    struct bb *bb = NULL;
    if (!any)
        goto out0;
    bb = build_cfg(fn, &nbb, &l2b);
    int *blk = xmalloc((size_t)nins * sizeof *blk);
    for (int n = 0; n < nins; n++) blk[n] = -1;
    for (int b = 0; b < nbb; b++)
        for (int n = bb[b].start; n < bb[b].end; n++) blk[n] = b;
    struct vblk lout;
    vblk_build(fn, bb, nbb, 0, &lout);
    char *gone = xcalloc((size_t)nins, 1);
    /* The instructions defining each vreg, in a list per vreg, kept true
     * as names merge: the questions below are about the definitions of
     * two vregs, and finding them by asking every instruction was
     * instructions x copies -- on a chain of 4000 joins, most of the
     * -O2 compile once the passes before it were linear. */
    int *dfirst = xmalloc((size_t)nv * sizeof *dfirst);
    int *dnext = xmalloc((size_t)nins * sizeof *dnext);
    for (int v = 0; v < nv; v++) dfirst[v] = -1;
    for (int n = nins - 1; n >= 0; n--) {
        int t = def_target(&fn->ins[n]);
        dnext[n] = -1;
        if (t >= 0 && t < nv) { dnext[n] = dfirst[t]; dfirst[t] = n; }
    }
    for (int m = 0; m < nins; m++) {
        struct ir_ins *i = &fn->ins[m];
        if (gone[m] || i->op != IR_MOV || blk[m] < 0)
            continue;
        int a = i->a, b = i->dst;
        if (a < fn->nvars || a >= nv || b < fn->nvars || b >= nv || a == b ||
            nuse[a] != 1 || mixed[a] || mixed[b] || vw[a] != vw[b] ||
            vw[a] != i->w || vflt[a] != vflt[b] || vflt[a] != (char)i->flt)
            continue;
        int ok = 1, nd = 0;
        for (int pass = 0; pass < 2 && ok; pass++)
        for (int d = dfirst[pass ? b : a]; d >= 0 && ok; d = dnext[d]) {
            if (gone[d] || d == m || blk[d] < 0)
                continue;
            int t = def_target(&fn->ins[d]);
            if (t == a) {
                nd++;
                /* `%a = add %b, 1; %b = mov %a` is a loop's own update:
                 * every allocator already gives the two one register,
                 * and writing it in place only moves its heuristics --
                 * RV32's spilled a pointer in the next loop of the
                 * workload's `text`, 5% more instructions. */
                if (ins_reads(&fn->ins[d], b))
                    ok = 0;
                if (jc_live_after(fn, gone, d, bb[blk[d]].end, b,
                                  &lout, blk[d]))
                    ok = 0;
            } else if (t == b) {
                if (jc_live_after(fn, gone, d, bb[blk[d]].end, a,
                                  &lout, blk[d]))
                    ok = 0;
            }
        }
        if (!ok || nd == 0)
            continue;
        /* a's definitions become b's, and move to b's list (in order,
         * though nothing asks for one) */
        int keep = -1, *kt = &keep;
        for (int d = dfirst[a], nx; d >= 0; d = nx) {
            nx = dnext[d];
            if (!gone[d] && d != m && fn->ins[d].op != IR_STVAR) {
                fn->ins[d].dst = b;
                dnext[d] = dfirst[b];
                dfirst[b] = d;
            } else {
                *kt = d;
                kt = &dnext[d];
            }
        }
        *kt = -1;
        dfirst[a] = keep;
        gone[m] = 1;
        nuse[a] = 0;
        vblk_merge(&lout, a, b);
        changed = 1;
    }
    if (changed) {
        struct ibuf nb = { 0, 0, 0 };
        int *newpos = fn->var_scope_lo
            ? xmalloc((size_t)(nins + 1) * sizeof *newpos) : NULL;
        for (int n = 0; n < nins; n++) {
            if (newpos) newpos[n] = nb.n;
            if (!gone[n])
                *ib_push(&nb) = fn->ins[n];
        }
        if (newpos) {
            newpos[nins] = nb.n;
            remap_scopes(fn, newpos, nins);
            free(newpos);
        }
        free(fn->ins);
        fn->ins = nb.p; fn->nins = nb.n; fn->cap = nb.cap;
    }
    free(gone); free(blk);
    vblk_free(&lout);
    free(dfirst); free(dnext);
    free(l2b);
    free_cfg(bb, nbb);
out0:
    free(nuse); free(vw); free(vflt); free(mixed);
    return changed;
}

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
static int pass_sinkupd(struct ir_func *fn)
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
static int pass_sinkaddr(struct ir_func *fn)
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
    /* Computed goto used to bail OUTRIGHT, and the reason was recorded
     * as a condition on when it could stop: "the exclusion stays until
     * the CFG can model the edges rather than until a pass is blamed".
     * build_cfg models them now -- an indirect jump gets an edge to
     * every label whose address is taken, which is the complete set of
     * places it can land, because `&&label` is the only thing that
     * produces such a value.
     *
     * What was actually going wrong is worth naming, because it was not
     * the local passes. Without those edges the blocks those labels
     * open have no predecessor, so they are UNREACHABLE -- and the two
     * passes that delete unreachable code deleted the program. With the
     * edges they are reachable, dominance is right, and the function is
     * optimized like any other. Codegen still keeps it in the memory
     * model (no register allocation), because a value live across an
     * indirect jump is a separate question and that is where it is
     * answered. */
    int cfg_ok = !fn->neh;
    /* What still has to stand aside is every pass that puts an
     * instruction ON AN EDGE. `goto *p` goes to the label and there is
     * no block between them to intercept, so an edge out of one cannot
     * be split -- and a preheader written just after the jump is not on
     * any path at all, which is exactly what happened: LICM hoisted
     * four instructions into one, and the pass that drops unreachable
     * code dropped it, uses and all.
     *
     * So: mem2reg (phi copies go on edges), the loop passes (a
     * preheader is an edge), if-conversion and unrolling (both open new
     * blocks). What is left -- folding, value numbering local and
     * global, copy propagation, store forwarding, load elimination,
     * dead stores, constant branches, dead code, SROA, magic division,
     * CFG cleanup -- reasons about the blocks that are there and needs
     * no new ones. That is most of the optimizer, where before it was
     * none of it.
     *
     * It is also the right answer for the code that uses this: a
     * threaded-code dispatcher keeps its state where every arm can
     * reach it, and tests/exec/computed-goto.c says so in its first
     * paragraph -- a value live across the jump stays in memory. */
    int has_igoto = 0;
    for (int n = 0; n < fn->nins; n++)
        if (fn->ins[n].op == IR_IGOTO) { has_igoto = 1; break; }
    int edge_ok = cfg_ok && !has_igoto;   /* may a pass split an edge? */
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
    if (g_tailrec && edge_ok)
        pass_tailrec(fn);
    if (g_divmagic) {
        pass_divtest(fn);               /* before divmagic takes the % */
        pass_divmagic(fn);
    }
    /* Before mem2reg, and needing no CFG of its own: it only renames
     * memory, and what it renames is what mem2reg then finds. */
    int sroa_twice = g_sroa && g_mem2reg && cfg_ok && !has_igoto;
    if (g_mem2reg && cfg_ok && !has_igoto)
        pass_cxlocal(fn);         /* a private `expected`: by value */
    if (g_sroa)
        pass_sroa(fn, !sroa_twice);
    if (g_mem2reg && cfg_ok && !has_igoto) {
        drop_unreachable(fn);     /* or mem2reg refuses the function */
        pass_mem2reg(fn);         /* global mem2reg (subsumes store-forwarding) */
    }
    /* And a second look at the aggregates, now that mem2reg has run.
     * `int *p = &s.x; ... *p` hides the object behind a pointer
     * VARIABLE, which is memory like any other, so the first look sees
     * the address escape into it and stops. Once the pointer is a temp
     * the chain is plain again and the object turns out to have been
     * private all along -- so whatever that splits, mem2reg is run once
     * more to promote. */
    if (sroa_twice && pass_sroa(fn, 1))
        pass_mem2reg(fn);
    /* Global load CSE is the expensive pass (CFG + an available-expressions
     * dataflow), so it runs ONCE per outer round instead of on every inner
     * iteration. When it exposes copies, the inner fixpoint reconverges and we
     * round again — it settles in one or two rounds. */
    /* The rounds run to a fixpoint, and the guards are how a fixpoint
     * that never comes is noticed rather than waited for. A pair of
     * passes that undo each other -- a constant-copy rule against value
     * numbering, once -- is a compiler that never finishes on a function
     * big enough to make each round slow: an hour on
     * src/arch/x86_64/codegen.c before anyone looked. So the cap is low
     * enough to hit in seconds (a real fixpoint takes a handful of
     * rounds), and hitting it is reported: fatal under EMBCC_VERIFY,
     * where the test suite runs, and a warning otherwise, because every
     * round leaves the function correct and the user's build should not
     * fail for a slow convergence. */
    int outer = 1, oguard = 0;
    while (outer && oguard++ < 100) {
        outer = 0;
        int changed = 1, guard = 0;
        while (changed && guard++ < OPT_MAX_ROUNDS) {
            if (guard == OPT_MAX_ROUNDS)
                opt_no_fixpoint(fn, OPT_MAX_ROUNDS);
            changed = 0;
            changed |= pass_storefwd(fn); /* forward local stores to loads (mem2reg-lite) */
            changed |= pass_roload(fn);   /* a global nothing writes */
            changed |= pass_fold(fn);
            /* ...and the divisors folding has just made constants:
             * `x / (1 << k)`, a const local, an inlined parameter. The
             * run before the rounds sees only literals. */
            if (g_divmagic)
                changed |= pass_divmagic(fn);
            /* After folding, so the constants it just exposed are the
             * ones this moves, and before value numbering, so what it
             * leaves is what CSE sees. */
            changed |= pass_reassoc(fn);
            changed |= pass_lvn(fn);      /* CSE: reuse identical computations */
            /* After value numbering, whose merges are what make an
             * index's use count true. */
            changed |= pass_idxoff(fn);   /* a[i - 1]: the -1 into the address */
            changed |= pass_divmod(fn);   /* a % b from the a / b beside it */
            if (g_divmagic)               /* and `x % C == 0` folding found */
                changed |= pass_divtest(fn);
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
            if (g_cfgclean) {
                changed |= pass_thread(fn);
                changed |= pass_cfgclean(fn);
            }
            if (g_dse && cfg_ok)
                changed |= pass_dse(fn);
            changed |= pass_rangecheck(fn);
        }
        if (pass_punfwd(fn)) {         /* a union's words, without the union */
            pass_copyprop(fn);
            pass_dce(fn);
            outer = 1;
        }
        if (g_loadcse && cfg_ok && pass_loadcse(fn)) {   /* reuse loads redundant on every path */
            pass_copyprop(fn);
            pass_dce(fn);
            outer = 1;
        }
        /* PRE after both CSEs have had their go: what it is looking for
         * is what they LEFT -- an expression redundant on some paths and
         * new on the rest. Running it first would have it insert copies
         * for expressions the cheaper passes were about to remove
         * outright. It rebuilds the array, so it belongs out here. */
        if (g_pre && edge_ok && pass_pre(fn)) {
            pass_copyprop(fn);
            pass_dce(fn);
            outer = 1;
        }
        /* Rotation first: it merges the header into the body, so the
         * block-local passes in the next round see one block where they
         * saw two. */
        if (g_licm && edge_ok && pass_rotate(fn)) {
            pass_copyprop_local(fn);
            pass_dce(fn);
            outer = 1;
        }
        /* LICM belongs out here with the other CFG passes: it rebuilds the
         * instruction array, so every block boundary the inner fixpoint
         * might hold is gone. Rounding again matters -- a hoisted
         * expression is a new candidate for folding and CSE in the
         * preheader, and what those leave can expose the next hoist. */
        if (g_licm && edge_ok && pass_licm(fn)) {
            pass_copyprop(fn);
            pass_dce(fn);
            outer = 1;
        }
        /* Vectorizing LAST of the loop passes, because it depends on
         * all of them: rotation for the single-block bottom-tested
         * shape, and LICM for the address base -- until the `gaddr` is
         * hoisted out, every memory reference looks like it is indexed
         * off something that changes, and nothing vectorizes. */
        /* Before vectorizing: a copy or clear loop turned into one
         * operation beats four lanes of the same loop. */
        if (g_idiom && edge_ok && pass_idiom(fn)) {
            pass_dce(fn);
            outer = 1;
        }
        if (g_vec && edge_ok && pass_vectorize(fn)) {
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
    if (g_ifconv && edge_ok && pass_ifconv(fn)) {
        pass_copyprop_local(fn);
        pass_dce(fn);
    }
    if (g_licm && edge_ok && pass_ivsr(fn)) {
        pass_copyprop_local(fn);
        pass_dce(fn);
    }
    /* Unrolling LAST of the loop passes, and for the same reason
     * strength reduction runs late: it leaves copies of the body that no
     * longer look like a loop to anything that wanted to match one.
     * What follows it is the block-local work -- folding, value
     * numbering, copy propagation -- which is most of what the copies
     * are for. */
    /* Switch threading with it: what it copies is a path, not a loop,
     * and the same block-local work cleans up after both. */
    int copied = 0;
    if (g_unroll && edge_ok && pass_unroll(fn))
        copied = 1;
    if (g_swthread && edge_ok && pass_swthread(fn))
        copied = 1;
    if (copied) {
        int changed = 1, g2 = 0;
        while (changed && g2++ < 100) {
            changed = 0;
            changed |= pass_fold(fn);
            /* The copies are a chain of `i+1` on `i+1` on `i+1`, which
             * is the shape this turns into four independent adds. */
            changed |= pass_reassoc(fn);
            changed |= pass_lvn(fn);
            changed |= pass_copyprop(fn);
            changed |= pass_copyprop_local(fn);
            changed |= pass_dce(fn);
            /* The unrolled block opens with a test the loop's own guard
             * has just made -- it has to, because that block is the
             * target of a back edge and the guard runs once -- so on
             * the way IN it is a branch whose answer is already known.
             * This is what notices. */
            if (g_cfgclean)
                changed |= pass_cfgclean(fn);
            /* And a guard that folding has just decided: the copies'
             * value numbering sees `end != base + 2048` with end ==
             * base + 2048 and makes it a constant, which nothing in
             * this loop resolved -- CRC's inner loop entered through
             * `mov $1; test; je` on every outer trip. */
            if (g_sccp && cfg_ok)
                changed |= pass_sccp(fn);
        }
    }
    /* After the fixpoint: fold constant operands into immediates, then DCE the
     * CONSTs that leaves unreferenced. Kept out of the fixpoint so the earlier
     * passes never reason about the imm_b form. */
    if (pass_immfold(fn))
        pass_dce(fn);
    pass_signtest(fn);       /* `if (x >> 63)` is `if (x < 0)` */
    if (pass_storenarrow(fn))
        pass_dce(fn);
    /* ...and only now put each surviving literal where it is wanted.
     *
     * AFTER the fixpoint, not inside it, for the same reason immfold is:
     * this decides where a value is MATERIALISED, not what it is, so
     * every pass that reasons about the instruction order should have
     * finished first. Inside the round it also never settled -- folding
     * and value numbering kept producing literals for it to move and it
     * kept reporting a change, which put format.c at six minutes. */
    /* The joins' copies, before anything decides where literals go: a
     * constant written straight into the merged name is where it was. */
    if (cfg_ok && pass_joincopies(fn) && g_cfgclean)
        pass_cfgclean(fn);
    pass_sinkconst(fn);
    /* Last: the copies it adds must not be propagated away again.
     * EMBCC_NO_SPLITLOOPS=1 turns it off, for bisecting a difference. */
    if (g_licm && edge_ok && !g_opt_size && !getenv("EMBCC_NO_SPLITLOOPS")) {
        int guard = 0;
        while (guard++ < 64 && pass_splitloops(fn))
            ;
    }
    pass_sinkaddr(fn);       /* last: nothing may separate them again */
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
            g_did.loadcse || g_did.dse || g_did.ifconv || g_did.pre)
            remark_add("opt", "rewrote", fn->name, "pass-counts",
                       fn->file, fn->line,
                       "%ld cse, %ld global cse, %ld load reuse, "
                       "%ld copies propagated, %ld dead, %ld dead stores, "
                       "%ld selects, %ld partial redundancies",
                       g_did.lvn, g_did.gcse, g_did.loadcse, g_did.copy,
                       g_did.dce, g_did.dse, g_did.ifconv, g_did.pre);
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
 * keeps two copies of it -- INLINE_SOLE_CALLEE keeps that case inlined
 * at every level. The body copied into twenty call sites is the other
 * case, and -Os now tells them apart by budget: INLINE_SIZE_CALLEE.
 *
 * Everything else -- folding, value numbering, dead code, loop
 * invariants, strength reduction -- makes code smaller as well as
 * faster, which is why -Os is level 2 and not level 1. */
/* ---- a loop's back-edge copies, before the branch ----------------------
 *
 * Out of SSA, the values a loop carries are copied on its back edge, and
 * a bottom-tested loop gets them as a block of its own:
 *
 *        brnz %c -> L5        ...falls through to the exit
 *     L5: %62 = mov %52
 *        %64 = mov %11
 *        jmp L0              the header
 *
 * -- one jump on every iteration (417 such blocks in a sample of
 * EmbLinkOs). When nothing on the exit path reads what the copies write,
 * they can run before the branch whichever way it goes, and the branch
 * can go to the header itself: the copies execute once more on the way
 * out and write values nobody reads. Only for a copy block nothing else
 * enters -- one branch to its label, no fall-through into it -- and
 * never when a copy writes the branch's own condition.
 *
 * The copies go above the COMPARE that feeds the branch, not between the
 * two: every backend fuses a compare into the branch right after it, and
 * copies in between turned `cmp; jl` into `setl; test; jne` -- a loss on
 * every loop of the x86 workload. A copy that writes the compare's own
 * operand cannot go above it, and that loop is left alone. */
struct lc_ref { int *n; };
static void lc_ref_cb(int *p, void *ctx)
{
    struct lc_ref *r = ctx;
    if (*p >= 0)
        r->n[*p]++;
}

static int lc_falls_through(enum ir_op op)
{
    return !(op == IR_JMP || op == IR_RET || op == IR_UD2 ||
             op == IR_IGOTO || op == IR_SWITCH);
}

static int pass_latch_copies(struct ir_func *fn)
{
    int N = fn->nins, nl = fn->nlabels, nv = fn->nvregs;
    if (N < 4 || !nl || !nv)
        return 0;
    int *refs = xcalloc((size_t)nl, sizeof *refs);
    int *lpos = xmalloc((size_t)nl * sizeof *lpos);
    struct lc_ref r = { refs };
    for (int k = 0; k < nl; k++)
        lpos[k] = -1;
    for (int n = 0; n < N; n++) {
        each_label(fn, &fn->ins[n], lc_ref_cb, &r);
        if (fn->ins[n].op == IR_LABEL && fn->ins[n].label >= 0 &&
            fn->ins[n].label < nl)
            lpos[fn->ins[n].label] = n;
    }
    /* candidates: br[p] -> copy block [b0, b1) ending in jmp at b1 */
    int *cand_br = xmalloc((size_t)N * sizeof *cand_br);
    int ncand = 0;
    char *in_cand = xcalloc((size_t)N, 1);
    for (int p = 0; p + 1 < N; p++) {
        const struct ir_ins *br = &fn->ins[p];
        if ((br->op != IR_BRZ && br->op != IR_BRNZ) || br->label < 0 ||
            br->label >= nl || refs[br->label] != 1)
            continue;
        int b = lpos[br->label];
        if (b <= 0 || b == p + 1 || lc_falls_through(fn->ins[b - 1].op))
            continue;
        int e = b + 1;
        while (e < N && fn->ins[e].op == IR_MOV && !fn->ins[e].vol &&
               fn->ins[e].dst >= fn->nvars && fn->ins[e].dst < nv &&
               fn->ins[e].dst != br->a)
            e++;
        if (e == b + 1 || e >= N || fn->ins[e].op != IR_JMP)
            continue;
        if (in_cand[p] || in_cand[b])
            continue;
        cand_br[ncand++] = p;
        in_cand[p] = 1;
        for (int k = b; k <= e; k++)
            in_cand[k] = 1;
    }
    int changed = 0;
    if (ncand) {
        int *first = xmalloc((size_t)nv * sizeof *first);
        int *last = xmalloc((size_t)nv * sizeof *last);
        struct ra_live *lv = ra_live_compute(fn, first, last);
        int any_live = fn->nins > 0 && nv > 0;
        char *take = xcalloc((size_t)N, 1);    /* br index: rewrite it */
        char *above = xcalloc((size_t)N, 1);   /* ...with copies above p-1 */
        char *drop = xcalloc((size_t)N, 1);    /* the copy block's ins */
        for (int c = 0; c < ncand && any_live; c++) {
            int p = cand_br[c], b = lpos[fn->ins[p].label], e = b + 1;
            const struct ir_ins *cmp = p > 0 ? &fn->ins[p - 1] : NULL;
            int fused = cmp && cmp->op == IR_CMP &&
                        cmp->dst == fn->ins[p].a && !in_cand[p - 1];
            int ok = 1;
            while (fn->ins[e].op == IR_MOV) {
                int d = fn->ins[e].dst;
                if (ra_live_in_at(lv, fn, p + 1, d))
                    ok = 0;
                if (fused && (d == cmp->a || (!cmp->imm_b && d == cmp->b)))
                    ok = 0;
                e++;
            }
            if (!ok)
                continue;
            take[p] = 1;
            above[p] = (char)fused;
            for (int k = b; k <= e; k++)
                drop[k] = 1;
        }
        struct ibuf nb = { 0, 0, 0 };
        int *newpos = xmalloc((size_t)(N + 1) * sizeof *newpos);
        for (int n = 0; n < N; n++) {
            newpos[n] = nb.n;
            if (drop[n])
                continue;
            /* the compare of a branch whose copies go above it: they
             * first, then the compare, then (next n) the branch */
            int brn = n + 1 < N && take[n + 1] && above[n + 1] ? n + 1 :
                      take[n] && !above[n] ? n : -1;
            if (brn >= 0) {
                int k = lpos[fn->ins[brn].label] + 1;
                for (; fn->ins[k].op == IR_MOV; k++)
                    *ib_push(&nb) = fn->ins[k];
                changed = 1;
                g_did.latch++;
            }
            *ib_push(&nb) = fn->ins[n];
            if (take[n]) {
                int k = lpos[fn->ins[n].label] + 1;
                while (fn->ins[k].op == IR_MOV)
                    k++;
                nb.p[nb.n - 1].label = fn->ins[k].label;   /* the header */
            }
        }
        newpos[N] = nb.n;
        if (changed) {
            remap_scopes(fn, newpos, N);
            free(fn->ins);
            fn->ins = nb.p; fn->nins = nb.n; fn->cap = nb.cap;
        } else {
            free(nb.p);
        }
        free(newpos); free(take); free(above); free(drop);
        free(first); free(last); ra_live_free(lv);
    }
    free(refs); free(lpos); free(cand_br); free(in_cand);
    return changed;
}

/* ---- a jump to a block of copies takes the copies with it --------------
 *
 * A loop whose body ends in several places -- a switch's cases, an
 * interpreter's opcodes -- sends each end to one block that copies the
 * loop's carried values and jumps to the header: two jumps per trip
 * where one would do. A `jmp` to such a block (copies and a jump,
 * nothing else) can do the copies itself and jump on to the block's
 * target: the same instructions on the same path, so nothing about
 * liveness changes. The block goes once nothing reaches it. Not at
 * -Os, where it trades a jump executed for copies stored once per
 * predecessor. */
#define THREAD_MAX_COPIES 4

static int pass_thread_copies(struct ir_func *fn)
{
    int N = fn->nins, nl = fn->nlabels;
    if (g_opt_size || N < 3 || !nl)
        return 0;
    int *lpos = xmalloc((size_t)nl * sizeof *lpos);
    int *refs = xcalloc((size_t)nl, sizeof *refs);
    struct lc_ref r = { refs };
    for (int k = 0; k < nl; k++)
        lpos[k] = -1;
    for (int n = 0; n < N; n++) {
        each_label(fn, &fn->ins[n], lc_ref_cb, &r);
        if (fn->ins[n].op == IR_LABEL && fn->ins[n].label >= 0 &&
            fn->ins[n].label < nl)
            lpos[fn->ins[n].label] = n;
    }
    /* copies[l]: the number of copies in label l's block when it is a
     * copy block ending in a jump elsewhere, else -1 */
    int *copies = xmalloc((size_t)nl * sizeof *copies);
    for (int l = 0; l < nl; l++) {
        copies[l] = -1;
        int b = lpos[l];
        if (b < 0)
            continue;
        int e = b + 1;
        while (e < N && fn->ins[e].op == IR_MOV && !fn->ins[e].vol)
            e++;
        if (e - b - 1 <= THREAD_MAX_COPIES && e < N &&
            fn->ins[e].op == IR_JMP && fn->ins[e].label != l &&
            e > b + 1)
            copies[l] = e - b - 1;
    }
    int changed = 0, *threaded = xcalloc((size_t)nl, sizeof *threaded);
    struct ibuf nb = { 0, 0, 0 };
    int *newpos = xmalloc((size_t)(N + 1) * sizeof *newpos);
    for (int n = 0; n < N; n++) {
        const struct ir_ins *in = &fn->ins[n];
        newpos[n] = nb.n;
        if (in->op == IR_JMP && in->label >= 0 && in->label < nl &&
            copies[in->label] > 0 && lpos[in->label] != n + 1) {
            int b = lpos[in->label];
            for (int k = 1; k <= copies[in->label]; k++)
                *ib_push(&nb) = fn->ins[b + k];
            struct ir_ins *j = ib_push(&nb);
            *j = *in;
            j->label = fn->ins[b + 1 + copies[in->label]].label;
            threaded[in->label]++;
            changed = 1;
            continue;
        }
        *ib_push(&nb) = *in;
    }
    newpos[N] = nb.n;
    if (changed) {
        remap_scopes(fn, newpos, N);
        free(fn->ins);
        fn->ins = nb.p; fn->nins = nb.n; fn->cap = nb.cap;
        /* a copy block every reference to which went: unreachable now
         * unless something falls into it, and DCE of blocks takes it */
    } else {
        free(nb.p);
    }
    free(newpos); free(threaded); free(copies); free(lpos); free(refs);
    return changed;
}

/* ---- x86-64: a load moved down to the operation it feeds ---------------
 *
 * x86 can take one operand of an add, sub, and, or or xor straight from
 * memory -- `xor (%r10,%rsi,4), %eax` is the table step of every CRC --
 * but only when the load sits next to its use, and only into a register
 * that already holds the other operand. irgen puts the load where the
 * source reads it, often several instructions earlier, and the register
 * allocator has decided everything by the time codegen could notice.
 *
 * So, before allocation: a load whose value is read once, by such an
 * operation later in the same block, is moved down to just before it,
 * together with the address add only it uses -- when nothing between can
 * write memory (it would change what the load reads) or redefine the
 * address. A commutative operation gets the loaded value as its SECOND
 * operand, which is the one x86 takes from memory, and the allocator's
 * two-address bias then puts the result in the other operand's register.
 * Codegen fuses what it finds adjacent. */
struct lop_cnt { int *n; int nv; };
static void lop_cnt_cb(int *p, void *ctx)
{
    struct lop_cnt *c = ctx;
    if (*p >= 0 && *p < c->nv)
        c->n[*p]++;
}
struct lop_find { int v, hit; };
static void lop_find_cb(int *p, void *ctx)
{
    struct lop_find *f = ctx;
    if (*p == f->v)
        f->hit = 1;
}

static int lop_user_ok(const struct ir_ins *u, int v, int size)
{
    if (u->flt || u->imm_b || u->w != size || u->a == u->b)
        return 0;
    switch (u->op) {
    case IR_ADD: case IR_AND: case IR_OR: case IR_XOR:
        return u->a == v || u->b == v;
    case IR_SUB:
        return u->b == v;          /* a - [mem]; [mem] - b gains nothing */
    default:
        return 0;
    }
}

/* A compare takes its memory operand second (`cmp reg, [mem]`), or first
 * against an immediate (`cmp [mem], imm`). */
static int lop_cmp_ok(const struct ir_ins *u, int v, int size)
{
    if (u->op != IR_CMP || u->flt || u->w != size || u->a == u->b)
        return 0;
    return u->imm_b ? u->a == v : u->a == v || u->b == v;
}

static int pass_x86_loadop(struct ir_func *fn)
{
    int nv = fn->nvregs, changed = 0;
    if (target_get() != TARGET_X86_64 || fn->nins < 2 || !nv)
        return 0;
    struct defs d;
    compute_defs(fn, &d);
    int *nuse = xcalloc((size_t)nv, sizeof *nuse);
    struct lop_cnt c = { nuse, nv };
    for (int n = 0; n < fn->nins; n++)
        if (fn->ins[n].op != IR_LDVAR && fn->ins[n].op != IR_ADDR)
            each_read(&fn->ins[n], lop_cnt_cb, &c);
    for (int p = 0; p < fn->nins; p++) {
        struct ir_ins *L = &fn->ins[p];
        if (L->op != IR_LOAD || L->vol || L->flt || L->memoff ||
            L->dst < fn->nvars || L->dst >= nv || nuse[L->dst] != 1 ||
            d.cnt[L->dst] != 1 || (L->size != 4 && L->size != 8) ||
            L->w != L->size)
            continue;
        int a0 = p;                    /* first instruction that moves */
        if (p > 0) {
            const struct ir_ins *ad = &fn->ins[p - 1];
            if (ad->op == IR_ADD && !ad->flt && ad->dst == L->a &&
                L->a >= fn->nvars && L->a < nv && nuse[L->a] == 1 &&
                d.cnt[L->a] == 1) {
                a0 = p - 1;
                /* ...and the shift scaling its index, which codegen folds
                 * into the address only when it is adjacent too */
                const struct ir_ins *sh = p > 1 ? &fn->ins[p - 2] : NULL;
                if (sh && sh->op == IR_SHL && sh->imm_b && !ad->imm_b &&
                    sh->dst == ad->b && sh->dst >= fn->nvars &&
                    sh->dst < nv && nuse[sh->dst] == 1 &&
                    d.cnt[sh->dst] == 1)
                    a0 = p - 2;
            }
        }
        int q = -1;
        for (int k = p + 1; k < fn->nins; k++) {
            const struct ir_ins *x = &fn->ins[k];
            enum ir_op op = x->op;
            struct lop_find f = { L->dst, 0 };
            if (op != IR_LDVAR && op != IR_ADDR)
                each_read((struct ir_ins *)x, lop_find_cb, &f);
            if (f.hit) {
                if (lop_user_ok(x, L->dst, L->size) ||
                    lop_cmp_ok(x, L->dst, L->size))
                    q = k;
                break;
            }
            if (op == IR_LABEL || op == IR_JMP || op == IR_BRZ ||
                op == IR_BRNZ || op == IR_RET || op == IR_UD2 ||
                op == IR_IGOTO || op == IR_SWITCH || writes_memory(op))
                break;
            int t = def_target(x), clash = t >= 0 && t == L->a;
            for (int m = a0; m < p && t >= 0 && !clash; m++)
                clash = t == fn->ins[m].a ||
                        (!fn->ins[m].imm_b && t == fn->ins[m].b);
            if (clash)
                break;
        }
        if (q < 0)
            continue;
        struct ir_ins *U = &fn->ins[q];
        if (U->a == L->dst && !U->imm_b) {   /* memory goes second */
            int t = U->a; U->a = U->b; U->b = t;
            if (U->op == IR_CMP)
                U->pred = swap_pred(U->pred);
        }
        if (q > p + 1) {
            int nm = p - a0 + 1, N = fn->nins;
            struct ir_ins mv[3];
            for (int k = 0; k < nm; k++)
                mv[k] = fn->ins[a0 + k];
            memmove(&fn->ins[a0], &fn->ins[p + 1],
                    (size_t)(q - p - 1) * sizeof *fn->ins);
            for (int k = 0; k < nm; k++)
                fn->ins[q - nm + k] = mv[k];
            if (fn->var_scope_lo) {
                int *newpos = xmalloc((size_t)(N + 1) * sizeof *newpos);
                for (int k = 0; k <= N; k++)
                    newpos[k] = k < a0 || k >= q ? k
                              : k <= p ? q - 1 - (p - k)
                              : k - nm;
                remap_scopes(fn, newpos, N);
                free(newpos);
            }
        }
        changed = 1;
        p = q;
    }
    free(nuse);
    free_defs(&d);
    return changed;
}

void opt_run(struct ir_unit *iu, int level)
{
    int size = level == OPT_SIZE;
    g_opt_size = size;
    if (size)
        level = 2;
    if (level < 1)
        return;
    /* -O1 is gcc's -O1: every value that can be is a register, and the
     * passes that only remove work run -- constants, dead stores, loop
     * invariants, branches made selects -- while the ones that trade
     * size or compile time for speed (inlining beyond a sole callee,
     * global CSE, PRE, unrolling, vectorizing) wait for -O2. It used to
     * be folding and copies over values that all lived in memory, and
     * with the sole-callee inlining that made frames LARGER than -O0's:
     * FreeRTOS's timer task overflowed its stack at -O1 alone. */
    pass_default(P_MEM2REG, level >= 1);
    pass_default(P_GCSE,    level >= 2);
    pass_default(P_LICM,    level >= 1);
    /* Vectorization is x86-64 for now: the aarch64 backend refuses the
     * vector opcodes loudly (diag_fatal) rather than emitting something
     * it has not been taught, so the pass must not hand it any. */
    pass_default(P_VEC,     level >= 2 && !size &&
                            target_get() == TARGET_X86_64);
    pass_default(P_LOADCSE, level >= 1);
    pass_default(P_SCCP,    level >= 1);
    pass_default(P_INLINE,  level >= 1);   /* -O1: see g_inline_o1 */
    g_inline_o1 = level == 1;
    pass_default(P_DSE,     level >= 1);
    /* A multiply and two shifts in place of a divide is smaller than
     * the divide's setup on these targets as well as faster, so -Os
     * keeps it. */
    pass_default(P_DIVMAGIC, level >= 1);
    pass_default(P_IFCONV, level >= 1);   /* cmov on x86-64, csel on aarch64 */
    pass_default(P_CFGCLEAN, level >= 1);  /* smaller and simpler at any level */
    pass_default(P_TAILREC, level >= 2);
    pass_default(P_IDIOM, level >= 2);
    /* Splitting a struct into the scalars it is made of makes the code
     * smaller as well as faster -- a field in a register is not loaded --
     * so -Os keeps it too. */
    pass_default(P_SROA, level >= 1);
    /* Unrolling is the one pass besides vectorization that reliably adds
     * code -- U copies of a body, plus the original kept whole for the
     * remainder -- so -Os leaves it off. */
    pass_default(P_UNROLL, level >= 2 && !size);
    pass_default(P_PRE, level >= 2);
    /* It copies the path to a switch once per state, so -Os leaves it
     * off with unrolling. */
    pass_default(P_SWTHREAD, level >= 2 && !size);
    if (g_pass[P_INLINE].on)      /* inline before the per-function passes clean up */
        inline_unit(iu);
    /* After inlining, so the bodies are the ones that will be compiled,
     * and before the per-function passes, which consult the result at
     * every call site. */
    if (level >= 1)
        infer_attrs(iu);
    /* After inlining too: it is every function's final body that has to
     * leave the global alone. */
    if (level >= 1)
        ro_globals(iu);
    else
        g_nro = 0;
    /* Every function, including one computing with __int128. The folds
     * that are 64-bit refuse a 128-bit width individually now (see
     * pass_fold), which is a great deal narrower than refusing the
     * function: everything else -- value numbering, copy propagation,
     * dead code, and all of the loop passes -- works on it unchanged. */
    for (int f = 0; f < iu->nfuncs; f++) {
        opt_func(&iu->funcs[f]);
        pass_latch_copies(&iu->funcs[f]);
        if (pass_thread_copies(&iu->funcs[f]))
            pass_cfgclean(&iu->funcs[f]);   /* the copy blocks left behind */
        /* After both: they are what put the back-edge copies into the
         * block whose update this moves next to them. */
        if (level >= 1)
            pass_sinkupd(&iu->funcs[f]);
        pass_x86_loadop(&iu->funcs[f]);     /* last: nothing reorders after */
    }
}
