/* ARMv7-M (Thumb-2) code generation (D-015).
 *
 * This backend starts where the other two started: every vreg in a stack
 * slot, every operation through a scratch register. D-005's "prove it
 * first" applies to a third backend as much as it did to the second, and
 * a naive lowering that is RIGHT is worth more than a clever one that is
 * nearly right on a machine nobody here has run code on before.
 *
 * ---- what it does NOT do yet, and says so ---------------------------
 *
 * THE RULE: every one of these refuses by name rather than emitting
 * something plausible.
 *
 *   64-bit integers.  The IR hands them over as single vregs at w == 8,
 *   and a 32-bit machine needs a REGISTER PAIR and a carry chain. The
 *   answer is a legalisation pass that splits w == 8 into two w == 4
 *   operations before this file sees them, not a backend that quietly
 *   truncates. Until that exists, `long long` and packed bitfields (which
 *   irgen assembles in a 64-bit accumulator) are refused.
 *
 *   Floating point.  ARMv7-M's base profile has no FPU, so every float
 *   operation is a call to __aeabi_fadd and its family. That is a
 *   lowering, not an instruction selection, and it belongs with the
 *   legalisation above.
 *
 *   Aggregates by value, varargs, atomics, inline asm, VLAs, computed
 *   goto and exceptions.  Each needs ABI or runtime work of its own.
 *
 * What it DOES do is the 32-bit scalar language: int and pointer
 * arithmetic, comparisons and control flow, loads and stores of every
 * width, calls with up to four arguments in registers and the rest on
 * the stack, and taking the address of a local, a global, a string or a
 * function.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emit.h"
#include "a32.h"
#include "cg.h"
#include "../backend.h"
#include "../regalloc.h"
#include "../target.h"
#include "../../driver/util.h"

/* Four scratch registers, which is what a 64-bit binary operation needs:
 * both halves of each operand at once. r12 is the ABI's own scratch and
 * needs no saving; r9, r10 and r11 are callee-saved and the prologue
 * pays for them.
 *
 * The naming is by ROLE rather than by number because the 64-bit
 * lowerings read as pairs: A_LO/A_HI hold the left operand and
 * B_LO/B_HI the right, and the 32-bit paths keep using T_ACC, T_TMP and
 * T_ADDR for the same registers. */
/* r9, r10 and r11 are the backend's fixed scratch registers, and all
 * three are CALLEE-SAVED: a function that touches one must push it. Every
 * use goes through t_scr(), which records it, so the prologue can save
 * only what the body used (see the pass loop in the function generator).
 * Recording a register that is merely compared against is harmless -- it
 * only saves one more -- and nothing may name r9-r11 by number instead. */
static unsigned g_t_scr_used;
/* ...by default. When r9-r11 are in the allocator's pool as well
 * (g_t_ext, one of gen_func_best's attempts), each instruction maps the
 * three roles onto whichever of r9-r11 hold nothing live there
 * (t_roles), and a role that has none is -1. Using it fails the attempt
 * (g_t_role_fail) -- the code it made is thrown away and the function
 * made again with the fixed roles -- so a scratch never lands on a value,
 * and a function that never runs short keeps three more registers for its
 * values: on a Cortex-M those are three more that survive a call. */
static int g_t_ext;
static int g_r_scr = 9, g_r_addr = 10, g_r_tmp = 11;
static int g_t_role_fail;
/* What gen_func's body emitted inside loops, weighted by how much more
 * often it runs (t_depth_weight - 1): with the function's bytes, the
 * -O2 measure gen_func_best compares attempts by. 0 at -Os and from
 * v6_gen_func, where the bytes alone decide. */
static long g_t_loop_bytes;
static int t_scr(int r)
{
    if (r < 0) {
        g_t_role_fail = 1;
        return 9;              /* what it emits is discarded */
    }
    g_t_scr_used |= 1u << r;
    return r;
}
#define T_SCR  t_scr(g_r_scr)
#define T_ADDR t_scr(g_r_addr)
#undef T_TMP
#define T_TMP  t_scr(g_r_tmp)
#define T_SCR_ALL ((1u << 9) | (1u << 10) | (1u << 11))
static void ldst_must(struct code *c, int rt, int rn, long off, int size,
                      int sign, int store);
static void t_copy_block(struct code *t, int dst, int src, int copy, long size);
/* The numbers, for COMPARING a register against one: evaluating the
 * marker would record a use that is not one. -1 when the role has no
 * register at this instruction (g_t_ext). */
#define R_SCR  g_r_scr
#define R_ADDR g_r_addr
#define R_TMP  g_r_tmp
/* A parallel move is given R_SCR as the register that breaks a cycle,
 * and only a move that USES it touches r9. */
static int pm_reg(int r) { return r == R_SCR ? t_scr(r) : r; }

#define A_LO T_ACC      /* r12 */
#define A_HI T_TMP      /* r11 */
#define B_LO T_ADDR     /* r10 */
#define B_HI T_SCR      /* r9  */

/* The registers the prologue saves. Four, not three, because AAPCS32
 * wants sp eight-byte aligned and `push` of an odd count would break it
 * — and because r9 then costs nothing and is a fourth scratch if this
 * file ever wants one. */
#define SAVE_MASK (T_SCR_ALL | (1u << T_LR))

/* AAPCS32 requires sp eight-byte aligned at every public interface, so
 * the push must move it by a multiple of eight -- an EVEN number of
 * registers. SAVE_MASK is four for that reason, but the allocator's
 * callee-saved set is pushed by the same instruction and its parity was
 * never counted: an odd F.nsave made the push 4 mod 8 and put every
 * eight-byte object below it four bytes out.
 *
 * r12 is the pad. It is the ABI's own scratch, so saving and restoring
 * it is harmless, and it is never in the allocator's pool -- unlike
 * r4-r8, which may all be taken, leaving nothing else to add.
 *
 * The symptom was three calls away from the cause: a variadic callee
 * read a `long long` stack argument as zero because its CALLER had an
 * odd prologue. */
/* `scr` is which of r9-r11 the body uses (T_SCR_ALL when not known).
 * The pad is r3: an argument register, so nothing is lost by saving it,
 * and never a return register, so nothing is lost by restoring it -- and
 * unlike r12 it keeps an all-low list in the 16-bit push/pop. GCC pads
 * the same way. */
static unsigned save_mask_for(int nsave, const int *used, unsigned scr)
{
    unsigned m = (scr & T_SCR_ALL) | (1u << T_LR);
    int n = 0;
    for (int k = 0; k < nsave; k++)
        m |= 1u << used[k];
    for (unsigned b = m; b; b &= b - 1)
        n++;
    if (n & 1)
        m |= 1u << 3;          /* r3: the pad */
    return m;
}

/* The bytes that mask moves sp by -- what the stack-parameter offsets
 * are measured from, so it must be the same answer. */
static long save_bytes_for(int nsave, const int *used, unsigned scr)
{
    unsigned m = save_mask_for(nsave, used, scr);
    long n = 0;
    for (int r = 0; r < 16; r++)
        if (m & (1u << r)) n++;
    return n * 4;
}

/* struct t_sites and struct t_fn, the per-function state: see cg.h, which
 * v6m.c (the ARMv6-M instruction selection) shares. */

/* ---- the register allocator's view of this machine ---------------------
 *
 * Nine registers, and that is the whole of AAPCS32's generosity: r0-r3
 * are the argument file and r4-r8 the callee-saved part this file does
 * not already need. r9-r12 are kept as scratch, because the slot paths
 * still have to land a value somewhere and a 64-bit value needs FOUR of
 * them at once (A_LO/A_HI/B_LO/B_HI).
 *
 * Compare RISC-V, which had seven registers spare after the same
 * reservations. Here the parallel move's cycle-breaking scratch comes
 * out of a file that is already fully committed, which is why the
 * prologue's push mask is patched after the body rather than decided
 * before it.
 *
 * Caller-saved first, as regalloc.h asks: a short-lived value takes r0-r3
 * and the prologue never grows for it.
 */
#define T_NPOOL 9
static const int T_POOL[T_NPOOL] = { 0, 1, 2, 3, 4, 5, 6, 7, 8 };
/* Without the argument file, for a variadic function: its prologue
 * pushes r0-r3 and `va_arg` walks them, so those four are not the
 * allocator's to give. */
static const int T_POOL_VA[5] = { 4, 5, 6, 7, 8 };

/* The same pool without r7, for a function with a variable-length array:
 * r7 holds its frame base (see IR_ALLOCA). */
static const int T_POOL_FB[] = { 0, 1, 2, 3, 4, 5, 6, 8 };
static const int T_POOL_VA_FB[] = { 4, 5, 6, 8 };

/* With r9-r11 as well (g_t_ext): three more callee-saved registers,
 * last, so a value takes one only once r4-r8 are gone. */
#define T_NPOOL_EXT 12
static const int T_POOL_EXT[T_NPOOL_EXT] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11
};
static const int T_POOL_VA_EXT[8] = { 4, 5, 6, 7, 8, 9, 10, 11 };
static const int T_POOL_FB_EXT[] = { 0, 1, 2, 3, 4, 5, 6, 8, 9, 10, 11 };
static const int T_POOL_VA_FB_EXT[] = { 4, 5, 6, 8, 9, 10, 11 };

/* Registers the pair pass (t_pair_alloc) took for the whole function,
 * withheld from the ordinary pool; bit r for rr. */
static unsigned g_t_taken;
static int g_t_pool[T_NPOOL_EXT];

/* ARMv6-M (v6m.c): r0-r5. Only r0-r7 compute there, and r6/r7 are that
 * lowering's two scratch registers; r5 is the frame base of a function with
 * a variable-length array, where r7 is on ARMv7-M. */
static const int T6_POOL[6] = { 0, 1, 2, 3, 4, 5 };
static const int T6_POOL_VA[2] = { 4, 5 };
static const int T6_POOL_FB[5] = { 0, 1, 2, 3, 4 };
static const int T6_POOL_VA_FB[1] = { 4 };

static const int *t_pool_base(const struct ir_func *fn, int *n)
{
    if (target_thumb_arch() == 6) {
        if (fn->has_alloca) {
            *n = fn->is_varargs ? 1 : 5;
            return fn->is_varargs ? T6_POOL_VA_FB : T6_POOL_FB;
        }
        *n = fn->is_varargs ? 2 : 6;
        return fn->is_varargs ? T6_POOL_VA : T6_POOL;
    }
    if (g_t_ext) {
        if (fn->has_alloca) {
            *n = fn->is_varargs ? 7 : 11;
            return fn->is_varargs ? T_POOL_VA_FB_EXT : T_POOL_FB_EXT;
        }
        *n = fn->is_varargs ? 8 : T_NPOOL_EXT;
        return fn->is_varargs ? T_POOL_VA_EXT : T_POOL_EXT;
    }
    if (fn->has_alloca) {
        *n = fn->is_varargs ? 4 : 8;
        return fn->is_varargs ? T_POOL_VA_FB : T_POOL_FB;
    }
    if (fn->is_varargs) {
        *n = 5;
        return T_POOL_VA;
    }
    *n = T_NPOOL;
    return T_POOL;
}

/* The callee-saved registers an asm operand of fn is pinned to: the x86
 * letters S and D, which select r6 and r7 (register variables are held to
 * r0-r3 and r12 by sema). The asm's moves write them, so no value of fn
 * may live there and the prologue saves them. Bit r for rr. */
static unsigned t_asm_saved_regs(const struct ir_func *fn)
{
    unsigned m = 0;
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_asm *a = fn->ins[n].op == IR_ASM ? fn->ins[n].asm_ir
                                                         : NULL;
        for (int k = 0; a && k < a->nin; k++)
            if (a->in[k].reg >= 4 && a->in[k].reg <= 11)
                m |= 1u << a->in[k].reg;
        for (int k = 0; a && k < a->nout; k++)
            if (a->out[k].reg >= 4 && a->out[k].reg <= 11)
                m |= 1u << a->out[k].reg;
    }
    return m;
}

static const int *t_pool_for(const struct ir_func *fn, int *n)
{
    int np, k = 0;
    const int *p = t_pool_base(fn, &np);
    unsigned out = g_t_taken | t_asm_saved_regs(fn);
    if (!out) {
        *n = np;
        return p;
    }
    for (int j = 0; j < np; j++)
        if (!(out >> p[j] & 1))
            g_t_pool[k++] = p[j];
    *n = k;
    return g_t_pool;
}

/* Per register, how many instructions name its values (t_lowregs). */
struct t_lowreg_w {
    const int *loc;
    long w[16];
    long f;                    /* this instruction's loop weight */
};

/* Each instruction's loop depth, by back edge: a branch to a label at
 * or above it closes a loop over everything in between. fn->nins + 1
 * entries, the last 0. */
static int *t_loop_depth(const struct ir_func *fn)
{
    int *depth = xcalloc((size_t)fn->nins + 1, sizeof *depth);
    int nl = 0;
    for (int n = 0; n < fn->nins; n++)
        if (fn->ins[n].op == IR_LABEL && fn->ins[n].label >= nl)
            nl = fn->ins[n].label + 1;
    int *lpos = xmalloc((size_t)(nl ? nl : 1) * sizeof *lpos);
    for (int k = 0; k < nl; k++)
        lpos[k] = -1;
    for (int n = 0; n < fn->nins; n++)
        if (fn->ins[n].op == IR_LABEL && fn->ins[n].label >= 0)
            lpos[fn->ins[n].label] = n;
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        if ((i->op == IR_JMP || i->op == IR_BRZ || i->op == IR_BRNZ) &&
            i->label >= 0 && i->label < nl && lpos[i->label] >= 0 &&
            lpos[i->label] <= n) {
            depth[lpos[i->label]]++;
            depth[n + 1]--;
        }
    }
    free(lpos);
    for (int n = 1; n <= fn->nins; n++)
        depth[n] += depth[n - 1];
    return depth;
}

/* How often code at a loop depth runs, relative to straight-line code:
 * a guess, and the same one the low-register weighting makes. */
static long t_depth_weight(int d)
{
    return d <= 0 ? 1 : d == 1 ? 8 : 64;
}

static void t_lowreg_count(int v, void *ctx)
{
    struct t_lowreg_w *c = ctx;
    if (v >= 0 && c->loc[v] >= 0 && c->loc[v] < 16)
        c->w[c->loc[v]] += c->f;
}

/* ---- LOW REGISTERS FOR THE BUSIEST VALUES ----------------------------
 *
 * Thumb-2's 16-bit encodings reach r0-r7 only: `ldr r0, [r4, #8]` is two
 * bytes and `ldr.w r0, [r8, #8]` four, and the same holds for str, adds,
 * subs, cmp, mov and most of the rest. The colourer hands the callee-saved
 * registers out in pool order to whichever value it reaches first, which
 * says nothing about how often each is used: in FreeRTOS's
 * xTaskGenericNotifyFromISR the task pointer, the base of nearly every
 * access in the function, got r8, and every one of those accesses was a
 * four-byte instruction.
 *
 * The callee-saved registers are interchangeable: the prologue saves a
 * set, and nothing in the ABI names one of them. So once allocation is
 * done, the ones it used are renamed among themselves: the register whose
 * values appear most where a low register shortens the instruction --
 * loads and stores twice, moves, calls and returns not at all, a loop
 * body's counting eight times per level -- becomes the lowest, the next
 * the one after. The set, and with it the push and the pop, is
 * unchanged.
 *
 * Left alone: a register a 64-bit value uses (its two halves are rN and
 * rN+1, a shape a rename would break), and the asm-pinned ones, which are
 * not in the pool. r9 and above never come from the pool. */
static void t_lowregs(const struct ir_func *fn, int *loc, const char *wide)
{
    int np;
    const int *pool = t_pool_base(fn, &np);
    unsigned cand = 0, fixed = t_asm_saved_regs(fn);
    for (int j = 0; j < np; j++)
        if (pool[j] >= 4 && pool[j] <= 8)    /* the callee-saved ones */
            cand |= 1u << pool[j];
    unsigned seen = 0;
    for (int v = 0; v < fn->nvregs; v++) {
        int r = loc[v];
        if (r < 0)
            continue;
        if (wide && wide[v]) {
            fixed |= 1u << r | 1u << (r + 1);
            continue;
        }
        seen |= 1u << r;
    }
    cand &= seen & ~fixed;
    if (!cand || !(cand & (cand - 1)))
        return;                         /* nothing to choose between */

    int *depth = t_loop_depth(fn);
    struct t_lowreg_w c;
    c.loc = loc;
    for (int r = 0; r < 16; r++)
        c.w[r] = 0;
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        /* What a low register buys here: a load or store's base and
         * value are what the 16-bit forms are mostly about; a move is
         * two bytes from any register to any, and a call's arguments
         * and a return value arrive by moves. */
        int k = i->op == IR_LOAD || i->op == IR_STORE ? 2 :
                i->op == IR_MOV || i->op == IR_CALL || i->op == IR_RET ? 0 : 1;
        c.f = (long)k * t_depth_weight(depth[n]);
        int def = ra_ins_def(i);
        if (def >= 0 && def < fn->nvregs && loc[def] >= 0 && loc[def] < 16)
            c.w[loc[def]] += c.f;
        ra_each_use(i, t_lowreg_count, &c);
    }
    free(depth);

    /* Heaviest first onto the lowest; ties keep their order. */
    int from[16], to[16], n = 0;
    for (int r = 0; r < 16; r++)
        if (cand >> r & 1)
            to[n] = from[n] = r, n++;
    for (int a = 1; a < n; a++)
        for (int b = a; b > 0 && c.w[from[b]] > c.w[from[b - 1]]; b--) {
            int t = from[b]; from[b] = from[b - 1]; from[b - 1] = t;
        }
    int map[16];
    for (int r = 0; r < 16; r++)
        map[r] = r;
    for (int k = 0; k < n; k++)
        map[from[k]] = to[k];
    for (int v = 0; v < fn->nvregs; v++)
        if (loc[v] >= 0 && loc[v] < 16 && (cand >> loc[v] & 1) &&
            !(wide && wide[v]))
            loc[v] = map[loc[v]];
}

/* The PAIR pool for 64-bit values, each named by its low register: the
 * argument pairs r0:r1 and r2:r3, where AAPCS32 passes a double or a long
 * long and every helper takes one, then r4:r5 and r6:r7 for one that
 * lives across a call. Without r6:r7 when r7 is a VLA's frame base, and
 * without the argument file in a variadic function, whose prologue owns
 * it. Even-aligned, as AAPCS32 wants a 64-bit value passed. */
static const int T_PAIRS[4] = { 0, 2, 4, 6 };
/* With r9-r11 in the pool (g_t_ext), r8:r9 and r10:r11 as well: two more
 * pairs that live across a call. strtoull's accumulators, which a call
 * per digit separates, had r4:r5 and r6:r7 to share with every 32-bit
 * value that crossed one, and went to the stack. */
static const int T_PAIRS_EXT[6] = { 0, 2, 4, 6, 8, 10 };
static const int T_PAIRS_EXT_FB[5] = { 0, 2, 4, 8, 10 };
static const int *t_pair_pool_for(const struct ir_func *fn, int *n)
{
    if (g_t_ext && target_thumb_arch() != 6) {
        const int *p = fn->has_alloca ? T_PAIRS_EXT_FB : T_PAIRS_EXT;
        int np = fn->has_alloca ? 5 : 6, lo = fn->is_varargs ? 2 : 0;
        *n = np - lo;
        return p + lo;
    }
    int lo = fn->is_varargs ? 2 : 0, hi = fn->has_alloca ? 3 : 4;
    /* ARMv6-M: r0:r1, r2:r3 and r4:r5 -- not r6:r7, the scratch, and not
     * r4:r5 either where r5 is a VLA's frame base. */
    if (target_thumb_arch() == 6)
        hi = fn->has_alloca ? 2 : 3;
    if (hi < lo)
        hi = lo;
    *n = hi - lo;
    return T_PAIRS + lo;
}

static int t_callee_saved(int reg) { return reg >= 4 && reg <= 11; }

/* Is `dst = load(local)` a plain move here? Only at the full width: a
 * narrower load sign- or zero-extends, which is an operation and not a
 * copy, so the two values cannot share a register. */
static int t_ldvar_plain(int size, int sign, int w)
{
    (void)sign;
    return size == 4 && w == 4;
}

/* Is this floating-point instruction executed on the FPU, rather than by
 * a runtime helper? FPv4-SP-D16 and FPv5-SP-D16 are SINGLE precision: a
 * double, a 64-bit integer conversion and fmodf are still calls.
 * FPv5-D16, the Cortex-M7's, computes in double precision too, so there
 * a double's arithmetic, comparisons, square root, its 32-bit
 * conversions and the conversions between the two widths are all
 * instructions -- and the 64-bit integer conversions are still calls,
 * because VFP has no instruction for them at either width. */
static int fp_on_vfp(const struct ir_ins *i)
{
    int dp;
    if (!target_thumb_fpu())
        return 0;
    dp = target_thumb_fpu_dp();
    switch (i->op) {
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_NEG:
    case IR_CMP: case IR_SQRT:
        return i->flt && (i->w == 4 || (dp && i->w == 8));
    case IR_I2F:                       /* int32 -> float (or double) */
        return (i->w == 4 || (dp && i->w == 8)) && i->size != 8;
    case IR_F2I:                       /* float (or double) -> int32 */
        return (i->size == 4 || (dp && i->size == 8)) && i->w != 8;
    case IR_F2F:                       /* float <-> double, both ways */
        return dp && (i->size == 4 || i->size == 8) &&
               (i->w == 4 || i->w == 8);
    default:
        return 0;
    }
}

/* Which instructions become a CALL the IR does not show as one.
 * Everything floating point -- ARMv7-M's base profile has no FPU -- and
 * the 64-bit divides. A value live across one of these may not sit in a
 * caller-saved register. */
int t_op_calls_helper(const struct ir_ins *i)
{
    /* ARMv6-M has no divide, no 64-bit multiply and no exclusives: those
     * are calls there (v6m.c says which). */
    if (target_thumb_arch() == 6 && v6_op_calls_helper(i))
        return 1;
    /* With the FPU, the single-precision arithmetic, comparisons and
     * 32-bit conversions are instructions. This must say exactly what
     * fp_on_vfp() says, since it is the same question: a value live
     * across a call it does not know about is in a clobbered register. */
    if (fp_on_vfp(i))
        return 0;
    if (i->flt)
        return i->op == IR_ADD || i->op == IR_SUB || i->op == IR_MUL ||
               i->op == IR_DIV || i->op == IR_CMP;
    if (i->op == IR_I2F || i->op == IR_F2I || i->op == IR_F2F)
        return 1;
    /* ARMv7-A has no divide in ARM state: __aeabi_idiv and family */
    if (t_isa_a32 && (i->op == IR_DIV || i->op == IR_MOD))
        return 1;
    return (i->op == IR_DIV || i->op == IR_MOD) && i->w == 8;
}

/* The FP class: s16-s31, the CALLEE-SAVED half of the VFP file, and only
 * that half. Everything else in it is spoken for -- s0-s15 carry
 * hard-float arguments and results, and s0/s1 are the scratch pair every
 * VFP instruction here computes in -- so a float in s16 survives a call,
 * a helper, an argument setup and an incoming parameter without a
 * parallel move to get right. The cost is a vpush/vpop of what is used.
 *
 * Empty without an FPU: then a float is bits in a core register. */
static const int T_FPOOL[16] = { 16, 17, 18, 19, 20, 21, 22, 23,
                                 24, 25, 26, 27, 28, 29, 30, 31 };
static const int *t_fp_pool_for(const struct ir_func *fn, int *n)
{
    (void)fn;
    *n = target_thumb_fpu() ? 16 : 0;
    return T_FPOOL;
}
static int t_fp_callee_saved(int reg) { return reg >= 16; }

/* The DOUBLE class, on a unit that computes with doubles (FPv5-D16):
 * d8-d15, the same callee-saved half of the file, each named by the
 * single register that is its low word -- d8 is s16:s17 -- so that one
 * per-vreg map (t_fn.floc) says where every FP value lives, and every
 * check that a value has no slot because it has an FP home covers
 * doubles too. The D number is the S number halved (t_dreg).
 *
 * The allocator hands out one register per value, so doubles get a pass
 * of their own over this pool, before the floats, exactly as 64-bit
 * integers get one over core PAIRS (t_pair_alloc): the floats' pass is
 * then told which s16-s31 each double holds, over that double's live
 * range only (ra_reserve). d0-d7 are not in it, for the reason s0-s15 are
 * not in the float pool: they carry the hard-float arguments and results,
 * and d0/d1 are this file's scratch for every double operation. */
static const int T_DPOOL[8] = { 16, 18, 20, 22, 24, 26, 28, 30 };
static const int *t_dp_pool_for(const struct ir_func *fn, int *n)
{
    (void)fn;
    *n = target_thumb_fpu_dp() ? 8 : 0;
    return T_DPOOL;
}
/* A copy of a double local into a temporary is a plain move in the D
 * class: both are the whole eight bytes. */
static int t_dldvar_plain(int size, int sign, int w)
{
    (void)sign;
    return size == 8 && w == 8;
}

/* Where AAPCS32 would put each value if it had the choice. Defined
 * below place_arg, whose answer it uses rather than restating. */
static void t_abi_hints(const struct ir_func *fn, int *hint);

static const struct ra_target THUMB_RA = {
    t_pool_for,
    t_callee_saved,
    t_ldvar_plain,
    /* Two of the three are on now.
     *
     * A scalar call ARGUMENT is moved into its argument register by the
     * parallel move in the IR_CALL lowering -- one at a time would
     * overwrite a register another argument is still to be read from.
     * The allocator keeps a struct argument and an indirect target in
     * memory regardless, and a 64-bit value is never in a register
     * here, so only scalars reach that move.
     *
     * A scalar RETURN goes out through `rd`, which has been
     * register-aware all along; the flag was the only thing forcing the
     * value into a slot, so every function used to end with `str` to a
     * slot and `ldr` back into r0.
     *
     * A memcpy's addresses are still read from their slots, so those
     * values must stay there. */
    1, 1, 0,
    t_op_calls_helper,
    0,            /* Thumb-2's wide forms are three-operand */
    t_abi_hints,
    t_fp_pool_for,
    t_fp_callee_saved,
    1,            /* float_in_gpr: a soft float is allocated with the core
                   * registers; with an FPU every float belongs to the FP
                   * pass, which the integer pass excludes (excl) */
    NULL, NULL,
    1,            /* atomic_in_reg: thumb_atomic reads through rdr and
                   * writes through wr/wreg */
    0,            /* fp_reads_gpr */
    1             /* asm_in_reg: see IR_ASM */
};

/* The D class's view of the same machine. Only the FP fields matter to
 * ra_allocate_fp; the rest say what THUMB_RA says. No ABI hints: the
 * ones that exist name core registers, and a d register home for a
 * double argument is moved into d0-d7 at the call regardless. */
static const struct ra_target THUMB_DRA = {
    t_pool_for,
    t_callee_saved,
    t_dldvar_plain,
    1, 1, 0,
    t_op_calls_helper,
    0,
    NULL,         /* abi_hints */
    t_dp_pool_for,
    t_fp_callee_saved,
    1,            /* float_in_gpr: as THUMB_RA, so a double argument or
                   * result is not made opaque for being one */
    NULL, NULL,
    1,            /* atomic_in_reg */
    0,            /* fp_reads_gpr */
    1             /* asm_in_reg: the FP class keeps out of asm regardless */
};

/* -O2 and -Os: the allocator is on. */
static int g_t_regalloc;
/* -O0: the allocator runs, for the temporaries of each expression only.
 * Every source variable keeps its stack slot, pinned there as under -g,
 * so a debugger sees each one at every statement; and nothing the -O1
 * code generator does beyond that -- tail calls, folded offsets, the
 * second pair-allocation attempt -- happens. Before, every temporary
 * was stored to a slot and loaded back, and FreeRTOS at -O0 was 5.7
 * times clang's -O0. */
static int g_t_o0;

/* ---- refusal -------------------------------------------------------- */

/* Refusals name the IR OPERATION as well as the reason. The x86 backend
 * learned this the expensive way with its dead-slot guard: "something is
 * unsupported here" sends the reader back through the whole lowering,
 * where "ret at width 8" says which line of which pass to look at. */
static void t_refuse(const struct ir_func *fn, const struct ir_ins *i,
                     const char *what)
{
    char op[64];
    op[0] = 0;
    if (i)
        snprintf(op, sizeof op, " [%s w=%d size=%d]", ir_opname(i->op),
                 i->w, i->size);
    fprintf(stderr,
            "embcc: %s:%d: error: the %s backend cannot lower %s yet "
            "(function %s)%s\n",
            fn->file ? fn->file : "?", i ? i->line : fn->line,
            t_isa_a32 ? "ARMv7-A" : "ARMv7-M", what, fn->name, op);
    exit(1);
}

/* Which vregs hold a 64-bit integer.
 *
 * By the WIDTH OF THE RESULT, which is `i->w` for the value-producing
 * operations and four for the rest however wide their operands are:
 * IR_CMP at w == 8 compares two 64-bit values and yields a 0 or a 1,
 * and IR_ADDR yields a pointer whatever it points at. Getting that
 * backwards gives the result an eight-byte slot and reads four bytes of
 * neighbouring temp as its high half. */
static char *wide64_map(struct ir_func *fn)
{
    char *w = xcalloc((size_t)(fn->nvregs ? fn->nvregs : 1), 1);
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        /* `flt` is NOT a reason to skip: a double is eight bytes and a
         * register pair exactly as a long long is, and the slot it
         * needs is the same size. Skipping them here (from when floats
         * were refused outright) gave every double-returning call a
         * four-byte slot, and the next temporary landed on its high
         * word — which is how __addsf3 came to add the wrong numbers
         * while every routine it called was exact. */
        if (i->w != 8 || i->dst < 0 || i->dst >= fn->nvregs)
            continue;
        switch (i->op) {
        case IR_CONST: case IR_MOV:
        case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
        case IR_AND: case IR_OR: case IR_XOR: case IR_SHL: case IR_SHR:
        case IR_NEG: case IR_BNOT: case IR_BSWAP:
        case IR_LDVAR: case IR_LOAD: case IR_EXT: case IR_CALL:
        case IR_SELECT:
        case IR_MULW:             /* two words in, a 64-bit product out */
        /* The conversions' `w` is their RESULT's width too: a double
         * out of I2F, and the 64-bit intermediate F2I goes through so
         * that an unsigned int lands right. */
        case IR_I2F: case IR_F2I: case IR_F2F: case IR_BITCAST:
            w[i->dst] = 1;
            break;
        default:
            break;
        }
    }
    /* A local declared eight bytes wide is one too, whether or not any
     * instruction has been seen to define it yet: the prologue writes a
     * parameter into its slot before the body runs. */
    for (int v = 0; v < fn->nvars && v < fn->nvregs; v++)
        if (fn->locals[v].size == 8 &&
            (fn->locals[v].is_int_or_ptr || fn->locals[v].is_scalar_float))
            w[v] = 1;

    /* Then propagate through COPIES, to a fixpoint.
     *
     * A MOV is not required to carry a width and often does not: the
     * merge of a `?:`'s two arms is emitted with an operand and a
     * destination and nothing else, which cost nothing while every
     * register was 64 bits wide. Reading `w` there says four, and
     * `neg ? -q : q` returned half of a long long -- with the other
     * half being whatever the destination's neighbour held, which is
     * how __divdi3 came back carrying its own dividend's high word.
     *
     * So the width of a copy is the width of what it copies, and a
     * chain of them (or one around a loop) settles here rather than
     * being asked instruction by instruction. */
    for (int again = 1; again; ) {
        again = 0;
        for (int n = 0; n < fn->nins; n++) {
            const struct ir_ins *i = &fn->ins[n];
            int src;
            if (i->dst < 0 || i->dst >= fn->nvregs || w[i->dst])
                continue;
            /* ...but only a copy that does not SAY four bytes. A
             * narrowing `mov.4s` from an eight-byte value takes its low
             * word, and marking the destination wide for it makes the
             * other arm of the same `?:` -- a four-byte value that may
             * now hold a REGISTER -- get read as a pair out of a slot
             * nothing wrote. Harmless while everything is in memory;
             * a miscompile once the allocator runs. */
            /* ...but only a copy that does not SAY four bytes. A
             * narrowing `mov.4s` from an eight-byte value takes its low
             * word, and marking the destination wide for it makes the
             * other arm of the same `?:` -- a four-byte value that may
             * now hold a REGISTER -- read as a pair out of a slot
             * nothing wrote. `fits(d) ? (int)d : 0` is exactly that
             * shape. Harmless while everything is in memory; a
             * miscompile once the allocator runs. A width-less MOV still
             * propagates: w == 0 is "unknown", not "four". */
            if (i->op == IR_MOV)
                src = i->w != 4 &&
                      i->a >= 0 && i->a < fn->nvregs && w[i->a];
            else if (i->op == IR_SELECT)
                src = i->w != 4 &&
                      ((i->b >= 0 && i->b < fn->nvregs && w[i->b]) ||
                       (i->c >= 0 && i->c < fn->nvregs && w[i->c]));
            else
                continue;
            if (src) {
                w[i->dst] = 1;
                again = 1;
            }
        }
    }
    return w;
}

/* ---- frame ---------------------------------------------------------- */

/* One slot per vreg: the locals in declaration order at their own sizes,
 * then four bytes for each temp. Above them sits the outgoing-argument
 * area, which is addressed from sp at offset 0 so a call's stack
 * arguments are written where the callee will look for them. */
/* How many bytes of OUTGOING argument area this function needs.
 *
 * Computed here rather than read from fn->outgoing_bytes, which irgen
 * fills from the SysV classification: SysV has six integer argument
 * registers and AAPCS32 has four, so a six-argument call reserves
 * nothing there and needs eight bytes here. Believing the IR's number
 * would put the fifth and sixth arguments on top of this function's
 * first local — which compiles, links, and returns the wrong answer. */
/* AAPCS32 argument placement, shared by a call's arguments and a
 * function's own parameters so the two cannot disagree.
 *
 * Fills `p` with where the argument goes: `nreg` words starting at
 * register `reg`, then `nstk` words at offset `stk` in the outgoing
 * area. A COMPOSITE may be both at once — with three words already
 * placed and a two-word struct to pass, r3 takes its first word and the
 * stack its second. Checked against clang for this triple, which is
 * where the splitting was confirmed rather than assumed.
 *
 * `align` is the type's, and only 8 matters: it rounds the register
 * number up to even, and the stack offset with it. A `long long` is
 * therefore never split, because rounding leaves two registers or none. */
/* Where one argument goes: `nreg` core registers from r`reg`, then
 * `nstk` words at `stk` in the outgoing area -- or, under the hard-float
 * convention, `nvfp` SINGLE registers from s`vfp` (a double takes two,
 * and `vdbl` says the elements are doubles, so d(vfp/2) is the name). */
/* struct argplace: see cg.h */

static void place_arg(int size, int align, int *ncrn, long *stk,
                      struct argplace *p)
{
    int words = (size + 3) / 4;

    if (align >= 8) {
        *ncrn = (*ncrn + 1) & ~1;
        *stk = (*stk + 7) & ~7L;
    }
    p->vfp = -1;
    p->nvfp = p->vdbl = 0;
    p->reg = *ncrn;
    p->nreg = *ncrn < 4 ? (words < 4 - *ncrn ? words : 4 - *ncrn) : 0;
    /* C.5: an argument is SPLIT between the last core registers and the
     * stack only while nothing is on the stack yet. The base standard
     * alone never gets here with a stack argument and a free register;
     * the hard-float one does, once a float has overflowed s0-s15, and
     * then the whole of the next composite goes to memory. */
    if (p->nreg && p->nreg < words && *stk) {
        p->nreg = 0;
        *ncrn = 4;
    }
    p->nstk = words - p->nreg;
    p->stk = *stk;
    *ncrn += p->nreg;
    if (p->nstk) {
        *ncrn = 4;                     /* nothing may back-fill past a split */
        *stk += (long)p->nstk * 4;
    }
}

/* Does a call return its result through a hidden pointer? AAPCS32
 * returns a COMPOSITE of four bytes or fewer in r0 and a larger one in
 * memory, with the caller's buffer address passed as an implicit FIRST
 * argument in r0 — so the real arguments start at r1.
 *
 * Only a composite. A `long long` is eight bytes and comes back in
 * r0:r1 like any other scalar; asking about size alone made every
 * 64-bit-returning function treat r0 as a buffer address and read its
 * first parameter out of r1. */
static int sret_bytes(int retsize) { return retsize > 4 ? retsize : 0; }

/* ---- the hard-float calling convention (AAPCS §6.1.2, "VFP") --------
 *
 * -mfloat-abi=hard changes WHERE floating point travels, and nothing
 * else. A "co-processor register candidate" -- a float, a double, or a
 * homogeneous aggregate of one to four of either (which is what a
 * `_Complex float` is here too) -- goes in s0-s15 / d0-d7, allocated
 * lowest-first WITH BACK-FILL: f(float, double, float) passes s0, d1 and
 * then s1, the hole the double's alignment left. Everything else walks
 * r0-r3 and the stack exactly as before, independently.
 *
 * When a candidate does not fit, it goes to the stack and EVERY VFP
 * register still free becomes unusable (C.3), so a later float cannot
 * slip back into a hole.
 *
 * A VARIADIC function uses the base standard for all of its arguments,
 * named ones included, and for its result: the callee cannot know which
 * register file an unnamed argument came in. */

/* How many elements, and of what size, if `a` is a candidate. */
static int vfp_cprc(const struct ir_arg *a, int *esz)
{
    if (!a->is_struct && a->is_float && (a->size == 4 || a->size == 8)) {
        *esz = a->size;
        return 1;
    }
    if (a->is_struct && a->hfa_n >= 1 && a->hfa_n <= 4 &&
        (a->hfa_size == 4 || a->hfa_size == 8)) {
        *esz = a->hfa_size;
        return a->hfa_n;
    }
    return 0;
}

/* One walk over an argument list: the core registers, the stack and the
 * VFP registers, with the state all three share. */
/* struct abi_walk: see cg.h */

static void walk_init(struct abi_walk *w, int sret, int varargs, int pcs)
{
    w->ncrn = sret ? 1 : 0;
    w->stk = 0;
    w->vfree = 0xffffu;            /* s0-s15 */
    w->vfp = target_pcs_vfp(pcs, varargs);
}

static int arg_align(const struct ir_arg *a);

static void place_one(struct abi_walk *w, const struct ir_arg *a,
                      struct argplace *p)
{
    int esz, n = w->vfp ? vfp_cprc(a, &esz) : 0;
    if (n) {
        int step = esz / 4, need = n * step;
        unsigned m = (1u << need) - 1;
        p->reg = p->nreg = p->nstk = 0;
        p->stk = 0;
        p->vdbl = esz == 8;
        for (int s = 0; s + need <= 16; s += step)
            if ((w->vfree & (m << s)) == (m << s)) {
                w->vfree &= ~(m << s);
                p->vfp = s;
                p->nvfp = need;
                return;
            }
        /* C.3: to memory, and no VFP register is used after this. The
         * core registers are not touched: a later int still gets r0. */
        w->vfree = 0;
        p->vfp = -1;
        p->nvfp = 0;
        if (esz == 8)
            w->stk = (w->stk + 7) & ~7L;
        p->stk = w->stk;
        p->nstk = (a->size + 3) / 4;
        w->stk += (long)p->nstk * 4;
        return;
    }
    place_arg(a->size, arg_align(a), &w->ncrn, &w->stk, p);
}

/* Is a result returned in VFP registers? A float or double in s0/d0; a
 * homogeneous aggregate in s0-s3 / d0-d3, WHATEVER its size -- so an
 * eight-byte `struct { float x, y; }` that the base standard returns
 * through a hidden pointer comes back in s0 and s1 instead. */
static int ret_vfp(int varargs, int pcs, int is_struct, int is_float, int size,
                   int hfa_n, int hfa_size)
{
    struct ir_arg a;
    int esz;
    if (!target_pcs_vfp(pcs, varargs))
        return 0;
    memset(&a, 0, sizeof a);
    a.is_struct = is_struct;
    a.is_float = is_float;
    a.size = size;
    a.hfa_n = hfa_n;
    a.hfa_size = hfa_size;
    return vfp_cprc(&a, &esz);
}

static int call_ret_vfp(const struct ir_ins *i, int *esz)
{
    int n = ret_vfp(i->call_varargs, i->call_pcs, i->retsize > 0, i->flt,
                    i->retsize ? i->retsize : i->w, i->ret_hfa_n,
                    i->ret_hfa_size);
    *esz = i->retsize ? i->ret_hfa_size : i->w;
    return i->dst >= 0 || i->retsize ? n : 0;
}

static int fn_ret_vfp(const struct ir_func *fn, int *esz)
{
    const struct ir_arg *r = &fn->ret_abi;
    *esz = r->is_struct ? r->hfa_size : r->size;
    return ret_vfp(fn->is_varargs, fn->pcs, r->is_struct, r->is_float, r->size,
                   r->hfa_n, r->hfa_size);
}

static int fn_sret_bytes(const struct ir_func *fn)
{
    int esz;
    if (fn_ret_vfp(fn, &esz))
        return 0;
    return fn->ret_abi.is_struct ? sret_bytes(fn->ret_abi.size) : 0;
}

static int call_sret_bytes(const struct ir_ins *i);

/* Can the call at n be a TAIL call -- the frame torn down, then `b.w` to
 * the callee, which returns straight to this function's caller? Only when
 * nothing of this frame can still be needed: every argument in r0-r3, no
 * struct result, no local whose address could have escaped into the
 * callee, and the IR_RET after it returning exactly the call's result --
 * or nothing, at the end of a function that returns nothing. Not through
 * a pointer, not variadic, and no floats: an s0 result is left alone. */
static int t_tail_ok(const struct ir_func *fn, int n)
{
    const struct ir_ins *i = &fn->ins[n];
    struct argplace pl;
    struct abi_walk w;
    int esz;

    if (i->op != IR_CALL || i->indirect || i->call_varargs || i->retsize ||
        i->flt || call_sret_bytes(i) || call_ret_vfp(i, &esz) ||
        getenv("EMBCC_NO_TAILCALL"))
        return 0;
    /* a cmse_nonsecure_entry function leaves through its own BXNS */
    if (fn->ret_abi.is_struct || fn->ret_abi.is_float || fn->is_varargs ||
        fn->has_alloca || fn->neh || fn->cmse_entry)
        return 0;
    if (n + 1 >= fn->nins) {
        if (fn->ret_abi.size)
            return 0;
    } else {
        const struct ir_ins *r = &fn->ins[n + 1];
        if (r->op != IR_RET)
            return 0;
        if (r->a >= 0 && (r->a != i->dst ||
                          i->ret_tybytes != fn->ret_abi.size ||
                          i->ret_tybytes > 4))
            return 0;
    }
    for (int k = 0; k < fn->nins; k++)
        if (fn->ins[k].op == IR_ADDR || fn->ins[k].op == IR_VA_START)
            return 0;
    walk_init(&w, 0, 0, i->call_pcs);
    for (int k = 0; k < i->nargs; k++) {
        place_one(&w, &i->argv[k], &pl);
        if (pl.nstk)
            return 0;
    }
    return 1;
}

static int call_sret_bytes(const struct ir_ins *i)
{
    int esz;
    if (call_ret_vfp(i, &esz))
        return 0;
    return sret_bytes(i->retsize);
}

/* What an argument's alignment is for placement purposes. A composite
 * carries its own -- its NATURAL alignment, the members' (type.h): a
 * struct declared aligned(8) around one int goes in the next register,
 * as gcc and clang pass it, where EmbCC rounded to an even one. A
 * SCALAR carries none, and an eight-byte one is
 * eight-aligned — which is what rounds the register number up to even.
 * Asking only composites (an earlier shape of this) put `long long` in
 * whichever register came next, so f(int, long long, ...) passed it in
 * r1:r2 where every other toolchain passes it in r2:r3. */
static int arg_align(const struct ir_arg *a)
{
    if (a->is_struct)
        return a->nat_align ? a->nat_align : a->align ? a->align : 4;
    return a->size > 4 ? 8 : 4;
}

/* Where AAPCS32 would put each value if it had the choice: a parameter
 * in the register it arrives in, and a scalar return in r0. Each of
 * those is a `mov` that disappears when the home IS that register.
 *
 * Placement comes from place_arg, the same function the prologue and
 * every call site use, so no second copy of AAPCS32 is stated here --
 * a hint that disagreed with the placement would quietly cost the move
 * it was meant to save.
 *
 * Only single-register scalars are hinted. A 64-bit value needs a pair
 * and is not eligible for a register at all here; a struct is placed by
 * a rule this one register cannot express.
 *
 * A call's ARGUMENTS are hinted too, now that the call lowering moves
 * them in parallel out of wherever they live. Without this the
 * allocator's choice and the ABI's disagreed and the move stayed:
 * `g(a+1, b+2)` came out swapping r0 and r1 on the way in and swapping
 * them back at the call. A value that is an argument to two calls at
 * different positions takes the later hint -- right at one of the two
 * beats right at neither. */
static void t_abi_hints(const struct ir_func *fn, int *hint)
{
    struct func *f = fn->src;
    /* A returned composite takes r0 for the hidden pointer, which is
     * what shifts every declared parameter along one. */
    struct abi_walk w;
    walk_init(&w, fn_sret_bytes(fn) != 0, fn->is_varargs, fn->pcs);
    for (int p = 0; f && p < fn->nparams && p < fn->nvregs; p++) {
        struct ir_arg *a = &fn->param_abi[p];
        struct argplace pl;
        place_one(&w, a, &pl);
        if (pl.nreg == 1 && !pl.nstk && !a->is_struct && a->size <= 4)
            hint[p] = pl.reg;
    }
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        /* r0 for a scalar return -- the one boundary the allocator has
         * been told this backend can read from a register. */
        if (i->op == IR_RET && i->a >= 0 && i->a < fn->nvregs &&
            !fn->ret_abi.is_struct && !i->flt)
            hint[i->a] = 0;
        /* ...and r0 for a scalar result coming back from a call. */
        else if (i->op == IR_CALL && !i->retsize && i->dst >= 0 &&
                 i->dst < fn->nvregs)
            hint[i->dst] = 0;
        /* A soft-float helper the lowering calls: operands in r0 and r1,
         * the result back in r0 -- so a chain of float operations hands
         * each result straight on. Not over a hint already given. */
        else if (i->op != IR_CALL && t_op_calls_helper(i) && i->w <= 4) {
            if (i->a >= 0 && i->a < fn->nvregs && hint[i->a] < 0)
                hint[i->a] = 0;
            if (!i->imm_b && i->b >= 0 && i->b < fn->nvregs &&
                hint[i->b] < 0)
                hint[i->b] = 1;
            if (i->dst >= 0 && i->dst < fn->nvregs && hint[i->dst] < 0)
                hint[i->dst] = 0;
        }
    }
    /* A call's arguments, placed by the same place_arg the call site
     * itself uses -- so no second copy of AAPCS32 is stated here. */
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        struct abi_walk cw;
        if (i->op != IR_CALL)
            continue;
        walk_init(&cw, call_sret_bytes(i) != 0, i->call_varargs, i->call_pcs);
        for (int k = 0; k < i->nargs; k++) {
            const struct ir_arg *a = &i->argv[k];
            struct argplace pl;
            place_one(&cw, a, &pl);
            if (pl.nreg == 1 && !pl.nstk && !a->is_struct && a->size <= 4 &&
                a->vreg >= 0 && a->vreg < fn->nvregs)
                hint[a->vreg] = pl.reg;
        }
    }
}

static long outgoing_area(const struct ir_func *fn)
{
    long most = 0;
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        struct argplace pl;
        struct abi_walk w;
        if (i->op != IR_CALL)
            continue;
        /* r0 holds the result's address when there is one */
        walk_init(&w, call_sret_bytes(i) != 0, i->call_varargs, i->call_pcs);
        for (int k = 0; k < i->nargs; k++)
            place_one(&w, &i->argv[k], &pl);
        if (w.stk > most)
            most = w.stk;
    }
    return (most + 7) & ~7L;
}

static int in_freg(const struct t_fn *F, int v);

/* Which vregs belong to the FP class: the single-precision floats the
 * FPU computes with -- operands and results of what fp_on_vfp() runs,
 * floats passed to and returned from calls, and float locals. Nothing
 * here is needed for CORRECTNESS: a vreg in this map that some integer
 * path also touches is reached through rd/wr, which cross with vmov. It
 * decides only what is worth an S register.
 *
 * A double is not here: FPv4-SP-D16 cannot compute with one, so it is a
 * pair of core words handed to the runtime -- and on FPv5-D16, which
 * can, it is t_double_map's. Under -g a source variable stays in its
 * slot, as the integer class keeps them. */
static char *t_float_map(const struct ir_func *fn, const char *wide,
                         int debug)
{
    int nv = fn->nvregs;
    char *m;
    if (!target_thumb_fpu() || nv <= 0)
        return NULL;
    m = xcalloc((size_t)nv, 1);
#define MARK(v) do { int v_ = (v); \
        if (v_ >= 0 && v_ < nv && !wide[v_]) m[v_] = 1; } while (0)
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        if (fp_on_vfp(i)) {
            switch (i->op) {
            case IR_I2F: MARK(i->dst); break;
            case IR_F2I: MARK(i->a); break;
            case IR_CMP: MARK(i->a); if (!i->imm_b) MARK(i->b); break;
            /* a conversion between the widths: its float side (MARK
             * passes over the double, which is t_double_map's) */
            case IR_F2F: MARK(i->dst); MARK(i->a); break;
            default:
                MARK(i->dst); MARK(i->a);
                if (!i->imm_b && i->op != IR_NEG && i->op != IR_SQRT)
                    MARK(i->b);
                break;
            }
        } else if (i->op == IR_CALL) {
            if (i->flt && i->w == 4 && !i->retsize)
                MARK(i->dst);
            for (int k = 0; k < i->nargs; k++)
                if (!i->argv[k].is_struct && i->argv[k].is_float &&
                    i->argv[k].size == 4)
                    MARK(i->argv[k].vreg);
        } else if (i->op == IR_RET && i->a >= 0 &&
                   fn->ret_abi.is_float && !fn->ret_abi.is_struct &&
                   fn->ret_abi.size == 4) {
            MARK(i->a);
        }
    }
#undef MARK
    for (int v = 0; v < fn->nvars && v < nv; v++)
        if (debug || !fn->locals[v].is_scalar_float ||
            fn->locals[v].size != 4)
            m[v] = 0;
    return m;
}

/* A 64-bit AND that clears, or XOR that flips, the sign bit and nothing
 * else: what fabs and negation of a double are by the time they reach
 * this backend (irgen's fb_* builtins, and the optimizer's -x). On a d
 * register they are vabs.f64 and vneg.f64, which are exactly those bit
 * operations -- no rounding, no NaN quieted, no exception raised. */
static int t_sign_mask_op(const struct ir_ins *i)
{
    return i->w == 8 && i->imm_b &&
           ((i->op == IR_AND && i->imm == (long)0x7fffffffffffffffL) ||
            (i->op == IR_XOR && i->imm == (long)0x8000000000000000UL));
}

/* The doubles worth a d register, on a unit that computes with them: the
 * same question t_float_map asks, of the 64-bit values. Operands and
 * results of what fp_on_vfp() runs at eight bytes, doubles passed to and
 * returned from calls (in d0-d7 or r0-r3, one vmov from a d register
 * either way), and double locals -- but not a local any instruction reads
 * or writes narrower than itself, whose bytes are then addressed one word
 * at a time. As with floats, nothing here is needed for correctness:
 * every 64-bit path reaches a d register through rd64/wr64, which cross
 * with one vmov. NULL without the double-precision unit. */
static char *t_double_map(const struct ir_func *fn, const char *wide,
                          int debug)
{
    int nv = fn->nvregs;
    char *m;
    if (!target_thumb_fpu_dp() || nv <= 0)
        return NULL;
    m = xcalloc((size_t)nv, 1);
#define DMARK(v) do { int v_ = (v); \
        if (v_ >= 0 && v_ < nv && wide[v_]) m[v_] = 1; } while (0)
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        if (fp_on_vfp(i)) {
            switch (i->op) {
            case IR_I2F: DMARK(i->dst); break;
            case IR_F2I: DMARK(i->a); break;
            case IR_CMP: DMARK(i->a); if (!i->imm_b) DMARK(i->b); break;
            case IR_F2F: DMARK(i->dst); DMARK(i->a); break;
            default:
                DMARK(i->dst); DMARK(i->a);
                if (!i->imm_b && i->op != IR_NEG && i->op != IR_SQRT)
                    DMARK(i->b);
                break;
            }
        } else if (i->op == IR_CALL) {
            if (i->flt && i->w == 8 && !i->retsize)
                DMARK(i->dst);
            for (int k = 0; k < i->nargs; k++)
                if (!i->argv[k].is_struct && i->argv[k].is_float &&
                    i->argv[k].size == 8)
                    DMARK(i->argv[k].vreg);
        } else if (i->op == IR_RET && i->a >= 0 &&
                   fn->ret_abi.is_float && !fn->ret_abi.is_struct &&
                   fn->ret_abi.size == 8) {
            DMARK(i->a);
        } else if (t_sign_mask_op(i)) {
            DMARK(i->dst); DMARK(i->a);
        }
    }
    /* ...and the bit casts that join a double to those: fabs is
     * bitcast-and-bitcast, and leaving the integer in the middle in a slot
     * would cost a store and a load each side of the one vabs. */
    for (int again = 1; again; ) {
        again = 0;
        for (int n = 0; n < fn->nins; n++) {
            const struct ir_ins *i = &fn->ins[n];
            int d = i->dst, a = i->a;
            if (i->op != IR_BITCAST || i->w != 8 || d < 0 || d >= nv ||
                a < 0 || a >= nv || !wide[d] || !wide[a] || m[d] == m[a])
                continue;
            m[d] = m[a] = 1;
            again = 1;
        }
    }
#undef DMARK
    for (int v = 0; v < fn->nvars && v < nv; v++)
        if (debug || !fn->locals[v].is_scalar_float ||
            fn->locals[v].size != 8)
            m[v] = 0;
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        if (i->op == IR_LDVAR && i->a >= 0 && i->a < nv && i->size != 8)
            m[i->a] = 0;
        if (i->op == IR_STVAR && i->dst >= 0 && i->dst < nv && i->size != 8)
            m[i->dst] = 0;
    }
    return m;
}

/* Does any instruction name local v -- read it, write it or take its
 * address? A parameter nothing names after optimisation (`(void)
 * pvParameters;` in every RTOS task) needs neither a slot nor the
 * prologue's store into one. */
struct t_named { int v, hit; };
static void t_named_cb(int u, void *ctx)
{
    struct t_named *c = ctx;
    if (u == c->v)
        c->hit = 1;
}
static int t_var_named(const struct ir_func *fn, int v)
{
    struct t_named c = { v, 0 };
    for (int n = 0; n < fn->nins && !c.hit; n++) {
        const struct ir_ins *in = &fn->ins[n];
        /* after mem2reg a parameter is read as an operand like any vreg
         * (`mov %t, %0`), not only through LDVAR */
        if (((in->op == IR_LDVAR || in->op == IR_ADDR) && in->a == v) ||
            (in->op == IR_STVAR && in->dst == v) || ra_ins_def(in) == v)
            return 1;
        ra_each_use(in, t_named_cb, &c);
    }
    return c.hit;
}

static void layout(struct t_fn *F)
{
    struct ir_func *fn = F->fn;
    long off = outgoing_area(fn);
    F->out_bytes = off;

    F->slot = xmalloc((size_t)(fn->nvregs ? fn->nvregs : 1) * sizeof *F->slot);
    for (int v = 0; v < fn->nvregs; v++)
        F->slot[v] = -1;

    /* ORDER matters as much as size here. Thumb-2's 16-bit `ldr/str rt,
     * [sp, #imm]` reaches 1020 bytes (and only r0-r7); past that every
     * access is the 32-bit form, and a function with one large array used
     * to push every temporary out of reach. So the busiest slots go
     * nearest sp: the temporaries, then the eight-byte ones and the small
     * locals, and the large locals -- arrays, structs -- last. */
    {
        /* Temporaries share a pool of four-byte slots: two whose live
         * ranges do not overlap take the same one (ra_coalesce_temps, the
         * pass x86-64 and aarch64 use). A 64-bit temp keeps a slot of its
         * own, eight-aligned, outside the pool -- the pool's slots are
         * four -- so it is shown to the coalescer as if it had a register
         * and placed below. */
        int nv = fn->nvregs, npool = 0, has_cgoto = 0;
        int *loc2 = xmalloc((size_t)(nv ? nv : 1) * sizeof *loc2);
        for (int v = 0; v < nv; v++)
            loc2[v] = (F->loc && F->loc[v] >= 0) || F->wide[v] ||
                      (F->selimm && F->selimm[v]) ? 0 : -1;
        for (int n = 0; n < fn->nins; n++)
            if (fn->ins[n].op == IR_IGOTO || fn->ins[n].op == IR_LABELADDR)
                has_cgoto = 1;
        {
            struct ra_slots so = { loc2, F->floc, g_t_regalloc, has_cgoto };
            int *tslot = ra_coalesce_temps(fn, fn->nvars, &so, &npool);
            long base = off;
            for (int v = fn->nvars; v < nv; v++) {
                int k = v - fn->nvars;
                if (loc2[v] >= 0 || in_freg(F, v) || !tslot || tslot[k] < 0)
                    continue;
                F->slot[v] = base + (long)tslot[k] * 4;
            }
            off = base + (long)npool * 4;
            free(tslot);
        }
        /* The 64-bit temporaries the same way, in a pool of their own
         * eight-byte slots: two whose live ranges do not overlap share
         * one, and one never live gets none. Each used to have a slot of
         * its own, so strtol's conv, a function of `unsigned long long`
         * temporaries, reserved 96 bytes of frame and used 16 of them.
         * None for one in a d register (t_double_map): it touches
         * memory no more than a pair does. Eight-aligned as well as eight
         * wide: AAPCS32 aligns `long long` to 8, and ldrd would need it. */
        {
            int nw = 0;
            for (int v = 0; v < nv; v++)
                loc2[v] = !F->wide[v] || (F->loc && F->loc[v] >= 0) ||
                          in_freg(F, v) ? 0 : -1;
            struct ra_slots sw = { loc2, F->floc, g_t_regalloc, has_cgoto };
            int *wslot = ra_coalesce_temps(fn, fn->nvars, &sw, &nw);
            long base = (off + 7) & ~7L;
            for (int v = fn->nvars; v < nv; v++) {
                int k = v - fn->nvars;
                if (loc2[v] >= 0 || !wslot || wslot[k] < 0)
                    continue;
                F->slot[v] = base + (long)wslot[k] * 8;
            }
            if (nw)
                off = base + (long)nw * 8;
            free(wslot);
        }
        free(loc2);
    }
    {
        /* Locals: those nothing names need no slot (SROA leaves whole
         * aggregates behind that way), nor one the allocator put in a
         * register (ra_slot_dead; under -g every local keeps its slot,
         * which is what DW_AT_location describes). Small ones first. */
        char *lref = ra_locals_referenced(fn, F->want_debug);
        for (int pass = 0; pass < 2; pass++)
            for (int v = 0; v < fn->nvars; v++) {
                int size, align;
                if ((F->loc && F->loc[v] >= 0) || in_freg(F, v))
                    continue;
                if (!lref[v] ||
                    ra_slot_dead(fn, F->loc, F->floc, v, F->want_debug))
                    continue;
                /* (ARMv6-M's prologue, in v6m.c, stores every one) */
                if (v < fn->nparams && !F->want_debug && !fn->is_varargs &&
                    !fn->has_alloca && target_thumb_arch() != 6 &&
                    !t_var_named(fn, v))
                    continue;
                size = fn->locals[v].size ? fn->locals[v].size : 4;
                if ((size > 8) != pass)
                    continue;
                align = fn->locals[v].user_align ? fn->locals[v].user_align
                      : fn->locals[v].align ? fn->locals[v].align : 4;
                if (align < 4) align = 4;
                off = (off + align - 1) & ~(long)(align - 1);
                F->slot[v] = off;
                off += size;
            }
        free(lref);
    }
    F->scratch_at = (off + 7) & ~7L;
    off = F->scratch_at + fn->scratch_bytes;
    /* A function that returns a composite in memory is handed the
     * address to write it to in r0, and must still have it at the
     * return — which may be many calls later, and r0 survives none of
     * them. It lives on the frame. */
    F->sret_slot = -1;
    if (fn_sret_bytes(fn)) {
        off = (off + 3) & ~3L;
        F->sret_slot = off;
        off += 4;
    }
    /* Eight, not four: AAPCS32 requires sp to be eight-byte aligned at
     * every public interface. */
    F->frame = (off + 7) & ~7L;
}

/* ---- reading and writing a vreg ------------------------------------- */

/* Load vreg v into `reg`. Every value lives in memory in this backend,
 * so this is always a load — which is the naive part, and the part a
 * register allocator replaces. */
/* Does the allocator have this vreg in a register? */
static int in_reg(const struct t_fn *F, int v)
{
    return F->loc && v >= 0 && F->loc[v] >= 0;
}

/* Get vreg v into exactly `reg` -- a load without the allocator, a MOVE
 * with it, nothing at all when it is already there. Keeping that
 * contract is what leaves every existing call site correct; the ones
 * that decide code size use the trio below and skip the move. */
static void fb_addr(struct t_fn *F, int rd, long off);

static int in_freg(const struct t_fn *F, int v)
{
    return F->floc && v >= 0 && F->floc[v] >= 0;
}

/* The d register a double with an FP home lives in: floc names its low
 * single (T_DPOOL), and d<n> is s<2n>:s<2n+1>. */
static int t_dreg(const struct t_fn *F, int v)
{
    return F->floc[v] >> 1;
}

/* A slot, for code that addresses one directly. A value with an S
 * register home has none, and a path that reached here with one would
 * read memory nothing wrote -- the miscompile the first FP class made. It
 * is a refusal instead. */
static long slot_of(const struct t_fn *F, int v)
{
    if (in_freg(F, v))
        internal_error("thumb: %s: a path reads vreg %d's slot, and it "
                       "lives in s%d", F->fn->name, v, F->floc[v]);
    if (in_reg(F, v))
        internal_error("thumb: %s: a path reads vreg %d's slot, and it "
                       "lives in r%d", F->fn->name, v, F->loc[v]);
    return F->slot[v];
}

static int faddr(const struct t_fn *F, int v, long *off);
static unsigned lo_free_at(const struct t_fn *F, int n);

static void rd(struct t_fn *F, int v, int reg)
{
    if (in_freg(F, v)) {
        t_vmov_core(F->t, F->floc[v], reg, 0);
        return;
    }
    if (in_reg(F, v)) {
        if (F->loc[v] != reg)
            t_mov_reg(F->t, reg, F->loc[v]);
        return;
    }
    long fo;
    if (faddr(F, v, &fo)) {
        fb_addr(F, reg, fo);
        return;
    }
    if (!t_ldst_imm(F->t, reg, F->fb, F->slot[v], 4, 0, 0)) {
        t_mov_imm(F->t, reg, F->slot[v], 0);
        t_ldst_reg(F->t, reg, F->fb, reg, 0, 4, 0, 0);
    }
}

/* A free low register for this instruction, taken so the next ask gets
 * another (see lo_free). */
static int lo_take(struct t_fn *F)
{
    int r = 0;
    while (!(F->lofree & (1u << r)))
        r++;
    F->lofree &= ~(1u << r);
    return r;
}
/* A scratch: a free low register when this instruction has one, else
 * the high one named. A macro for the reason rdr is one below. */
#define LO(F, scratch) ((F)->lofree ? lo_take(F) : (scratch))

/* `rdr` says where a value already IS; `wreg` where to compute a result;
 * `wrote` commits it only if that was a scratch. */
static int rdr_(struct t_fn *F, int v, int scratch)
{
    if (in_reg(F, v))
        return F->loc[v];
    rd(F, v, scratch);
    return scratch;
}

/* Macros, so the SCRATCH argument is evaluated only when it is used.
 * T_ADDR, T_TMP and T_SCR are t_scr() calls that mark r9-r11 as needing
 * a save, and as function arguments they were evaluated for every value
 * already in a register -- so a leaf that never touched r10 still pushed
 * and popped it, and lost its `bx lr`. */
#define rdr(F, v, scratch) \
    (in_reg((F), (v)) ? (F)->loc[(v)] : rdr_((F), (v), LO((F), (scratch))))
#define wreg(F, v, scratch) \
    (in_reg((F), (v)) ? (F)->loc[(v)] : LO((F), (scratch)))

static void wr(struct t_fn *F, int v, int reg)
{
    if (in_freg(F, v)) {
        t_vmov_core(F->t, F->floc[v], reg, 1);
        return;
    }
    if (in_reg(F, v)) {
        if (F->loc[v] != reg)
            t_mov_reg(F->t, F->loc[v], reg);
        return;
    }
    long fo;
    if (F->slot[v] < 0 || faddr(F, v, &fo))
        return;
    if (!t_ldst_imm(F->t, reg, F->fb, F->slot[v], 4, 0, 1)) {
        /* The offset does not reach: compute the address in a scratch
         * that is not the value being stored. */
        int a = reg == R_ADDR ? T_TMP : T_ADDR;
        t_mov_imm(F->t, a, F->slot[v], 0);
        t_alu_reg(F->t, T_OP_ADD, a, F->fb, a, 0);
        ldst_must(F->t, reg, a, 0, 4, 0, 1);
    }
}

static void wrote(struct t_fn *F, int v, int reg)
{
    wr(F, v, reg);      /* wr already does the right thing either way */
}

/* A 64-bit value's two halves, little-endian: the low word at the slot
 * and the high word four bytes above it. */
/* dl <- sl and dh <- sh as one parallel move. */
static void mv2(struct t_fn *F, int dl, int sl, int dh, int sh)
{
    if (dl == sh && dh == sl) {
        if (dl == sl) return;
        t_mov_reg(F->t, T_SCR, sl);
        t_mov_reg(F->t, dh, sh);
        t_mov_reg(F->t, dl, T_SCR);
        return;
    }
    if (dl == sh) {
        if (dh != sh) t_mov_reg(F->t, dh, sh);
        if (dl != sl) t_mov_reg(F->t, dl, sl);
        return;
    }
    if (dl != sl) t_mov_reg(F->t, dl, sl);
    if (dh != sh) t_mov_reg(F->t, dh, sh);
}

/* A 64-bit value in a register PAIR (t_pair_alloc) has its low word in
 * F->loc[v] and its high word in the next register. */
static void rd64(struct t_fn *F, int v, int lo, int hi)
{
    if (in_reg(F, v)) {
        mv2(F, lo, F->loc[v], hi, F->loc[v] + 1);
        return;
    }
    /* A double in a d register: both words in one vmov, low word first,
     * which is how d<n> is laid over s<2n> and s<2n+1>. */
    if (in_freg(F, v)) {
        t_vmov_core_pair(F->t, t_dreg(F, v), lo, hi, 0);
        return;
    }
    (void)slot_of(F, v);
    /* a frame slot is word-aligned, which ldrd needs */
    if (t_ldst_pair(F->t, lo, hi, F->fb, F->slot[v], 0))
        return;
    if (!t_ldst_imm(F->t, lo, F->fb, F->slot[v], 4, 0, 0) ||
        !t_ldst_imm(F->t, hi, F->fb, F->slot[v] + 4, 4, 0, 0)) {
        fb_addr(F, hi, F->slot[v]);
        ldst_must(F->t, lo, hi, 0, 4, 0, 0);
        ldst_must(F->t, hi, hi, 4, 4, 0, 0);
    }
}

static void wr64(struct t_fn *F, int v, int lo, int hi)
{
    if (in_reg(F, v)) {
        mv2(F, F->loc[v], lo, F->loc[v] + 1, hi);
        return;
    }
    if (in_freg(F, v)) {
        t_vmov_core_pair(F->t, t_dreg(F, v), lo, hi, 1);
        return;
    }
    (void)slot_of(F, v);
    if (F->slot[v] < 0)
        return;
    if (t_ldst_pair(F->t, lo, hi, F->fb, F->slot[v], 1))
        return;
    if (!t_ldst_imm(F->t, lo, F->fb, F->slot[v], 4, 0, 1) ||
        !t_ldst_imm(F->t, hi, F->fb, F->slot[v] + 4, 4, 0, 1)) {
        int a = (lo == R_SCR || hi == R_SCR) ? T_ADDR : T_SCR;
        fb_addr(F, a, F->slot[v]);
        ldst_must(F->t, lo, a, 0, 4, 0, 1);
        ldst_must(F->t, hi, a, 4, 4, 0, 1);
    }
}

/* The second operand of a 64-bit binary operation, immediate or not.
 * Every caller materialises it before its own first flag-setting
 * instruction, and no flag survives from the IR instruction before, so
 * a small half in a low register is a two-byte movs. */
static void operand_b64(struct t_fn *F, const struct ir_ins *i, int lo, int hi)
{
    if (i->imm_b) {
        t_mov_imm_dead_flags(F->t, lo, (long)(i->imm & 0xffffffffL));
        t_mov_imm_dead_flags(F->t, hi, (long)((i->imm >> 32) & 0xffffffffL));
    } else {
        rd64(F, i->b, lo, hi);
    }
}

/* An instruction's SECOND operand, into `reg`.
 *
 * `b` is not always a vreg: the optimizer's immediate-fold pass moves a
 * constant into `imm` and sets `imm_b`, after which `b` holds nothing
 * and reading it as a vreg loads whatever happens to occupy that slot.
 * The IR header lists that as an ADD/SUB/AND/OR/XOR/CMP flag; it is set
 * on SHL, SHR and MUL too, which is how `t += p[i]` came out as
 * 101255427 at -O1 — the index shift had folded its `#2` and the
 * backend shifted by a stale word instead. So every binary operation
 * asks HERE, and none of them reads i->b directly. */
static void operand_b(struct t_fn *F, const struct ir_ins *i, int reg)
{
    if (i->imm_b)     /* flags dead: see operand_b64 */
        t_mov_imm_dead_flags(F->t, reg, (long)i->imm);
    else
        rd(F, i->b, reg);
}

/* The address of a local's slot, into `reg`. */
static void addr_of_slot(struct t_fn *F, int v, int reg)
{
    fb_addr(F, reg, slot_of(F, v));
}

/* Is vreg v a frame address that is recomputed rather than kept (see
 * t_fn.fvar)? Then *off is its offset from the frame base. A value the
 * allocator put in a register is just that register. */
static int faddr(const struct t_fn *F, int v, long *off)
{
    if (!F->fvar || v < 0 || v >= F->fn->nvregs || in_reg(F, v) ||
        in_freg(F, v))
        return 0;
    if (F->fvar[v] >= 0) {
        *off = slot_of(F, F->fvar[v]);
        return 1;
    }
    if (F->fscr[v] >= 0) {
        *off = F->scratch_at + F->fscr[v];
        return 1;
    }
    return 0;
}

/* rd <- base + off, where base may be the frame base with a frame
 * offset (which can exceed addw's reach) */
static void base_plus(struct t_fn *F, int rd, int base, long off)
{
    if (base == F->fb)
        fb_addr(F, rd, off);
    else
        t_addw(F->t, rd, base, off);
}

/* Which temps hold a frame address and nothing else: one definition,
 * an `addr` of a local or a struct-returning call (whose value is its
 * result scratch's address), and a four-byte value. Only temps -- a
 * variable is read and written through its own slot by ldvar and stvar.
 * Fills F->fvar and F->fscr. */
static void frame_addr_map(struct t_fn *F)
{
    const struct ir_func *fn = F->fn;
    int nv = fn->nvregs;
    F->fvar = NULL;
    F->fscr = NULL;
    if (!nv)
        return;
    int *fv = xmalloc((size_t)nv * sizeof *fv);
    long *fs = xmalloc((size_t)nv * sizeof *fs);
    int *nd = xcalloc((size_t)nv, sizeof *nd);
    for (int v = 0; v < nv; v++) {
        fv[v] = -1;
        fs[v] = -1;
    }
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        int d = ra_ins_def(i);
        if (d < 0 || d >= nv)
            continue;
        nd[d]++;
        if (i->op == IR_ADDR && i->a >= 0 && i->a < nv)
            fv[d] = i->a;
        else if (i->op == IR_CALL && i->retsize > 0 && i->scratch >= 0)
            fs[d] = i->scratch;
    }
    for (int v = 0; v < nv; v++)
        if (nd[v] != 1 || v < fn->nvars || (F->wide && F->wide[v]))
            fv[v] = -1, fs[v] = -1;
    free(nd);
    F->fvar = fv;
    F->fscr = fs;
}

/* frame base + off, into `rd`: t_add_sp where the frame base is sp. */
static void fb_addr(struct t_fn *F, int rd, long off)
{
    if (F->fb == T_SP) {
        t_add_sp(F->t, rd, off);
        return;
    }
    if (off >= 0 && off <= 4095) {
        t_addw(F->t, rd, F->fb, off);
        return;
    }
    t_mov_imm(F->t, rd, off, 0);
    t_alu_reg(F->t, T_OP_ADD, rd, F->fb, rd, 0);
}

/* ---- accesses that must be encodable ----------------------------------
 *
 * t_ldst_imm answers 0 when the offset is out of its forms' reach, and
 * that answer was ignored at forty-eight call sites -- so an access that
 * did not fit was not emitted at all. An 8000-byte struct assignment
 * copied its first 4096 bytes and returned. Every site that does not
 * handle the failure itself now goes through this, which refuses the
 * function by name instead (THE RULE); the copies that can be large are
 * loops (t_copy_block) and never get here with a big offset. */
static void ldst_must(struct code *c, int rt, int rn, long off, int size,
                      int sign, int store)
{
    if (!t_ldst_imm(c, rt, rn, off, size, sign, store))
        internal_error("thumb: a %d-byte %s at offset %ld from r%d is out of "
                       "reach", size, store ? "store" : "load", off, rn);
}

/* A load from the frame at any offset: the immediate form when it
 * reaches, else the address built in the destination itself. A frame
 * past 4 KB is ordinary at -O0 -- every temp has a slot -- and the
 * incoming stack arguments and the struct-return slot sit above all of
 * it. */
static void fb_ld(struct t_fn *F, int rt, long off, int size, int sign)
{
    if (t_ldst_imm(F->t, rt, F->fb, off, size, sign, 0))
        return;
    fb_addr(F, rt, off);
    ldst_must(F->t, rt, rt, 0, size, sign, 0);
}

/* The most a block copy or clear does in straight-line code; past it, a
 * loop (t_copy_block). It was "while every offset fits an immediate":
 * 4092 bytes, and a 2 KB table cleared in a loop -- tools/bench's hash --
 * became 512 four-byte stores, two kilobytes of code where clang has a
 * loop of 32. A word is a load and a store, so 128 bytes is 64
 * instructions at most; 64 bytes at -Os. */
static long t_block_straight(void)
{
    return target_opt_size() ? 64 : 128;
}

/* Copy `size` bytes from [src] to [dst] (copy) or zero them (!copy).
 * Straight-line up to t_block_straight; past that a loop of eight words
 * a trip (four at -Os) that walks both pointers, with the end in r9 --
 * so src and dst are scratch and are moved -- and what is left over
 * after it straight-line. T_ACC carries the data. */
static void t_copy_block(struct code *t, int dst, int src, int copy, long size)
{
    long k;
    if (!copy)
        t_mov_imm(t, T_ACC, 0, 0);
    if (size <= t_block_straight()) {
        for (k = 0; k + 4 <= size; k += 4) {
            if (copy) ldst_must(t, T_ACC, src, k, 4, 0, 0);
            ldst_must(t, T_ACC, dst, k, 4, 0, 1);
        }
        for (; k < size; k++) {
            if (copy) ldst_must(t, T_ACC, src, k, 1, 0, 0);
            ldst_must(t, T_ACC, dst, k, 1, 0, 1);
        }
        return;
    }
    long step = target_opt_size() ? 16 : 32;
    long body = size / step * step;
    t_mov_imm(t, T_SCR, body, 0);
    t_alu_reg(t, T_OP_ADD, T_SCR, T_SCR, dst, 0);
    int top = t->len;
    for (k = 0; k < step; k += 4) {
        if (copy) ldst_must(t, T_ACC, src, k, 4, 0, 0);
        ldst_must(t, T_ACC, dst, k, 4, 0, 1);
    }
    if (copy)
        t_addw(t, src, src, step);
    t_addw(t, dst, dst, step);
    t_cmp_reg(t, dst, T_SCR);
    t_patch_bcond(t, t_bcond(t, T_NE), top);
    for (k = 0; k + 4 <= size - body; k += 4) {
        if (copy) ldst_must(t, T_ACC, src, k, 4, 0, 0);
        ldst_must(t, T_ACC, dst, k, 4, 0, 1);
    }
    for (; k < size - body; k++) {
        if (copy) ldst_must(t, T_ACC, src, k, 1, 0, 0);
        ldst_must(t, T_ACC, dst, k, 1, 0, 1);
    }
}

/* ---- branches ------------------------------------------------------- */

#define T_CBZ 100              /* a fix's cond for cbz; cbnz is T_CBZ + 1 */
#define T_TAB 99               /* a jump table's entry: cz_at holds the table,
                                * the word becomes (target | 1) - table */
#define T_TBH 98               /* a tbh table's halfword: cz_at holds the pc
                                * it is relative to, the halfword becomes
                                * (target - pc) / 2 */
#define T_LADDR 97             /* &&label: a movw/movt pair into register
                                * `ins`, added to pc at cz_at */

static void want_label(struct t_fn *F, int at, int label, int cond)
{
    if (F->nfix == F->capfix) {
        F->capfix = F->capfix ? F->capfix * 2 : 16;
        F->fix = xrealloc(F->fix, (size_t)F->capfix * sizeof *F->fix);
    }
    F->fix[F->nfix].at = at;
    F->fix[F->nfix].label = label;
    F->fix[F->nfix].cond = cond;
    F->fix[F->nfix].sz = F->t->len - at;
    F->fix[F->nfix].cz_at = -1;
    F->fix[F->nfix].ins = -1;
    F->nfix++;
}

/* A branch to a label, 16-bit when the first pass measured that it fits
 * (cond < 0 is unconditional). */
static int emit_branch(struct t_fn *F, int cond)
{
    int k = F->nfix;
    int sh = F->shortb && k < F->nshortb && F->shortb[k];
    if (cond < 0)
        return sh ? t_b16(F->t) : t_b(F->t);
    return sh ? t_bcond16(F->t, cond) : t_bcond(F->t, cond);
}

static void jump_to(struct t_fn *F, int label)
{
    want_label(F, emit_branch(F, -1), label, -1);
}

static void jump_if(struct t_fn *F, int cond, int label)
{
    if (F->far_mode) {
        int skip = t_bcond16(F->t, cond ^ 1);   /* ARM inverts by bit 0 */
        int at = emit_branch(F, -1);
        if (!t_patch_bcond16(F->t, skip, F->t->len))
            internal_error("thumb: a far branch's skip does not reach");
        want_label(F, at, label, -1);
        F->bc_end = -1;                          /* nothing to invert */
        return;
    }
    want_label(F, emit_branch(F, cond), label, cond);
    F->bc_end = F->t->len;
    F->bc_fix = F->nfix - 1;
}

/* About to jump to `label` from IR instruction n. If the code so far ends
 * in a conditional branch around THIS jump --
 *
 *      b<c>  L1                 b<!c> label
 *      b     label      ==>
 *   L1:                      L1:
 *
 * -- turn it into one branch on the opposite condition. The IR often
 * has a move between the two that the allocator made free (a `?:` arm
 * whose value is already in the merge's register), and a return's jump
 * to the epilogue is the same shape, so this is caught here, after
 * allocation, and not only in the optimizer.
 *
 * Only when NOTHING was emitted since the branch and no label was placed
 * (nothing can arrive between the two), and L1 is where control goes
 * next. ARM inverts a condition by its low bit; the float conditions
 * were chosen so that is exact for an unordered result too. */
static int invert_last_bcond(struct t_fn *F, int n, int label)
{
    struct ir_func *fn = F->fn;
    int hit = 0, cond, at;
    if (F->bc_end != F->t->len || F->bc_fix != F->nfix - 1)
        return 0;
    for (int m = n + 1; m < fn->nins && fn->ins[m].op == IR_LABEL; m++)
        if (fn->ins[m].label == F->fix[F->bc_fix].label) { hit = 1; break; }
    if (!hit)
        return 0;
    cond = F->fix[F->bc_fix].cond ^ 1;
    F->t->len -= F->fix[F->bc_fix].sz;
    /* The same ordinal, so the same size decision: the first pass made
     * this inversion too, and measured the branch it produced. */
    F->nfix--;
    at = emit_branch(F, cond);
    F->nfix++;
    F->fix[F->bc_fix].at = at;
    F->fix[F->bc_fix].label = label;
    F->fix[F->bc_fix].cond = cond;
    /* No longer a `cbz` candidate: the second pass decides cbz at the
     * BRZ, before it can know this jump follows, and a cbz cannot be
     * inverted into it -- it would emit cbz AND this jump, one branch
     * more than the first pass counted, and every later branch would
     * take its neighbour's short-or-long decision -- lib/libc's
     * mbrtowc at -O2 stopped with `a relaxed branch no longer reaches
     * its label`. */
    F->fix[F->bc_fix].cz_at = -1;
    F->bc_end = -1;
    return 1;
}

/* ---- calls ---------------------------------------------------------- */

static void note_call(struct t_sites *st, int at, struct func *target)
{
    if (st->ncall == st->capcall) {
        st->capcall = st->capcall ? st->capcall * 2 : 16;
        st->call = xrealloc(st->call, (size_t)st->capcall * sizeof *st->call);
    }
    st->call[st->ncall].patch_off = at;
    st->call[st->ncall].target = target;
    st->call[st->ncall].tail = 0;
    st->ncall++;
}

static void note_ext(struct t_sites *st, int at, struct func *callee)
{
    if (st->next == st->capext) {
        st->capext = st->capext ? st->capext * 2 : 16;
        st->ext = xrealloc(st->ext, (size_t)st->capext * sizeof *st->ext);
    }
    st->ext[st->next].patch_off = at;
    st->ext[st->next].callee = callee;
    st->ext[st->next].tail = 0;
    st->next++;
}

static void note_str(struct t_sites *st, int at, int idx, enum reloc_kind k)
{
    if (st->nstr == st->capstr) {
        st->capstr = st->capstr ? st->capstr * 2 : 16;
        st->str = xrealloc(st->str, (size_t)st->capstr * sizeof *st->str);
    }
    st->str[st->nstr].patch_off = at;
    st->str[st->nstr].str_off = idx;
    st->str[st->nstr].kind = k;
    st->nstr++;
}

static void note_glob(struct t_sites *st, int at, struct global *g,
                      enum reloc_kind k)
{
    if (st->ng == st->capg) {
        st->capg = st->capg ? st->capg * 2 : 16;
        st->g = xrealloc(st->g, (size_t)st->capg * sizeof *st->g);
    }
    st->g[st->ng].patch_off = at;
    st->g[st->ng].glob = g;
    st->g[st->ng].kind = k;
    st->ng++;
}

static void note_fn(struct t_sites *st, int at, struct func *target,
                    enum reloc_kind k)
{
    if (st->nf == st->capf) {
        st->capf = st->capf ? st->capf * 2 : 16;
        st->f = xrealloc(st->f, (size_t)st->capf * sizeof *st->f);
    }
    st->f[st->nf].patch_off = at;
    st->f[st->nf].target = target;
    st->f[st->nf].kind = k;
    st->f[st->nf].addend = 0;
    st->nf++;
}

/* A call to a runtime routine the IR does not show as one: the 64-bit
 * divides, and every floating-point operation. The callee is interned
 * so the driver emits one UNDEF symbol and a relocation per name,
 * exactly as for any other external call.
 *
 * Interned by NAME rather than from a fixed table, because there are
 * forty of these once soft float is counted and a table would be a
 * second place to keep the list. */
static struct func **g_helpers;
static int g_nhelpers, g_caphelpers;

static void call_helper(struct t_fn *F, const char *name)
{
    struct func *h = NULL;
    for (int k = 0; k < g_nhelpers; k++)
        if (strcmp(g_helpers[k]->name, name) == 0) {
            h = g_helpers[k];
            break;
        }
    if (!h) {
        h = xcalloc(1, sizeof *h);
        h->name = name;
        h->declared = 1;
        h->used = 1;
        if (g_nhelpers == g_caphelpers) {
            g_caphelpers = g_caphelpers ? g_caphelpers * 2 : 16;
            g_helpers = xrealloc(g_helpers,
                                 (size_t)g_caphelpers * sizeof *g_helpers);
        }
        g_helpers[g_nhelpers++] = h;
    }
    note_ext(F->st, t_bl(F->t), h);
}

/* ---- floating point, which this machine has none of -----------------
 *
 * ARMv7-M's base profile has no FPU: every operation is a call, and
 * AAPCS's soft-float variant passes the operands in the CORE registers
 * — a float in one and a double in a pair — so the value never needs to
 * be anything but bits, and the integer paths above already carry it.
 *
 * The names are libgcc's. clang and gcc emit the __aeabi_* spellings
 * for this target, which are the same functions under other names; a
 * program that links a real libgcc gets both.
 */
static const char *fp_binop_name(enum ir_op op, int w)
{
    switch (op) {
    case IR_ADD: return w == 8 ? "__adddf3" : "__addsf3";
    case IR_SUB: return w == 8 ? "__subdf3" : "__subsf3";
    case IR_MUL: return w == 8 ? "__muldf3" : "__mulsf3";
    case IR_DIV: return w == 8 ? "__divdf3" : "__divsf3";
    default:     return NULL;
    }
}

/* The comparison helpers return an INT whose sign answers the question:
 * __ltdf2 is negative when a < b, __gtdf2 positive when a > b, and
 * __eqdf2 zero when they are equal. Unordered makes each of them answer
 * the way that renders the predicate false, which is what NaN must do —
 * except for `!=`, where __nedf2's nonzero is the right answer. */
static const char *fp_cmp_name(enum binop pred, int w)
{
    switch (pred) {
    case B_EQ: return w == 8 ? "__eqdf2" : "__eqsf2";
    case B_NE: return w == 8 ? "__nedf2" : "__nesf2";
    case B_LT: return w == 8 ? "__ltdf2" : "__ltsf2";
    case B_LE: return w == 8 ? "__ledf2" : "__lesf2";
    case B_GT: return w == 8 ? "__gtdf2" : "__gtsf2";
    default:   return w == 8 ? "__gedf2" : "__gesf2";   /* B_GE */
    }
}

/* Read a floating operand into the argument registers starting at
 * `reg`, and say how many it took. */
static int fp_arg(struct t_fn *F, int v, int w, int reg)
{
    if (w == 8) {
        rd64(F, v, reg, reg + 1);
        return 2;
    }
    rd(F, v, reg);
    return 1;
}

/* Both operands of a two-argument helper, as a PARALLEL MOVE.
 *
 * A soft-float helper is not an IR_CALL, so nothing marks its operands
 * and the allocator is free to put them in registers -- and then
 * `__ltdf2(a, b)` with a in r1 and b in r0 does `mov r0, r1` and loses b
 * before reading it. An eight-byte operand is a PAIR and never
 * allocated, so that case keeps its loads.
 *
 * T_SCR breaks a cycle: r9 is scratch and holds nothing of its own. */
/* Two 64-bit operands into r0:r1 and r2:r3 as ONE parallel move: a pair
 * may live in the argument registers (t_pair_alloc), and loading r0:r1
 * first loses a b that lives there. vb < 0: only the first. */
static void args64x2(struct t_fn *F, int va, int vb)
{
    int pd[4], ps[4], npm = 0;
    int v[2], nv = vb >= 0 ? 2 : 1;
    v[0] = va; v[1] = vb;
    for (int k = 0; k < nv; k++)
        if (in_reg(F, v[k]))
            for (int q = 0; q < 2; q++) {
                pd[npm] = 2 * k + q;
                ps[npm] = F->loc[v[k]] + q;
                npm++;
            }
    if (npm) {
        int od[8], os[8];
        int m = ra_parallel_move(pd, ps, npm, R_SCR, od, os, 8);
        if (m < 0)
            internal_error("thumb: a 64-bit helper's argument setup is not "
                           "a well-formed move");
        for (int k = 0; k < m; k++)
            t_mov_reg(F->t, pm_reg(od[k]), pm_reg(os[k]));
    }
    for (int k = 0; k < nv; k++)
        if (!in_reg(F, v[k]))
            rd64(F, v[k], 2 * k, 2 * k + 1);
}

static void fp_args2(struct t_fn *F, const struct ir_ins *i)
{
    if (i->w == 8) {
        args64x2(F, i->a, i->b);
        return;
    }
    {
        int pd[2], ps[2], npm = 0;
        if (in_reg(F, i->a)) { pd[npm] = T_R0; ps[npm] = F->loc[i->a]; npm++; }
        if (in_reg(F, i->b)) { pd[npm] = T_R1; ps[npm] = F->loc[i->b]; npm++; }
        if (npm) {
            int od[8], os[8];
            int m = ra_parallel_move(pd, ps, npm, R_SCR, od, os, 8);
            if (m < 0)
                internal_error("thumb: a helper's argument setup is not a "
                               "well-formed move");
            for (int k = 0; k < m; k++)
                t_mov_reg(F->t, pm_reg(od[k]), pm_reg(os[k]));
        }
        /* The loads come after: they only WRITE argument registers, so
         * nothing still needs the old contents of one. */
        if (!in_reg(F, i->a)) rd(F, i->a, T_R0);
        if (!in_reg(F, i->b)) rd(F, i->b, T_R1);
    }
}

static void fp_result(struct t_fn *F, int dst, int w)
{
    if (dst < 0)
        return;
    if (w == 8) wr64(F, dst, T_R0, T_R1);
    else        wr(F, dst, T_R0);
}

/* ---- comparisons ---------------------------------------------------- */

static int cond_for(enum binop pred, int sign)
{
    switch (pred) {
    case B_EQ: return T_EQ;
    case B_NE: return T_NE;
    case B_LT: return sign ? T_LT : T_CC;
    case B_LE: return sign ? T_LE : T_LS;
    case B_GT: return sign ? T_GT : T_HI;
    case B_GE: return sign ? T_GE : T_CS;
    default:   return T_AL;
    }
}

/* ---- 64-bit integers ------------------------------------------------
 *
 * A 32-bit machine carries one in a REGISTER PAIR and an eight-byte
 * slot, low word first. Everything below works in A_LO/A_HI and
 * B_LO/B_HI and writes its result back through wr64.
 *
 * Done here rather than as a legalisation pass over the IR because the
 * IR has no carry: expressing `adds`/`adcs` in EmbIR would take a
 * compare and a branch per addition, and adding carry-carrying opcodes
 * would put two operations into the shared operand switches that only
 * one target ever emits -- which is precisely how an opcode rots.
 */

/* A shift of a 64-bit value by a variable amount, branching on whether
 * the count reaches into the high word. The branchless form ARM code
 * usually uses needs two more registers than this backend has spare;
 * this one needs none, and a shift is not the hot path on a Cortex-M.
 *
 * Counts of 32 and above fall out of the second arm, and a count of
 * zero out of the first: ARM's register shifts take the low byte of the
 * count and produce zero (or the sign, for ASR) at 32 and above, so
 * `alo >> (32 - 0)` contributes nothing exactly as it should. */
static void shift64_var(struct t_fn *F, int op, int sign)
{
    struct code *t = F->t;
    int big, done;
    t_cmp_imm(t, B_LO, 32);
    big = t_bcond(t, T_GE);
    if (op == T_SH_LSL) {
        t_shift_reg(t, T_SH_LSL, A_HI, A_HI, B_LO, 0);
        t_alu_imm(t, T_OP_RSB, B_HI, B_LO, 32, 0);
        t_shift_reg(t, T_SH_LSR, B_HI, A_LO, B_HI, 0);
        t_alu_reg(t, T_OP_ORR, A_HI, A_HI, B_HI, 0);
        t_shift_reg(t, T_SH_LSL, A_LO, A_LO, B_LO, 0);
    } else {
        /* The LOW word always shifts LOGICALLY, whatever the shift is:
         * only the high word carries the sign. An arithmetic shift here
         * smears bit 31 of the low word across the bits the high word
         * is about to supply -- 0x1234567890abcdef >> 8 came back as
         * 0x00123456ff90abcd. */
        t_shift_reg(t, T_SH_LSR, A_LO, A_LO, B_LO, 0);
        t_alu_imm(t, T_OP_RSB, B_HI, B_LO, 32, 0);
        t_shift_reg(t, T_SH_LSL, B_HI, A_HI, B_HI, 0);
        t_alu_reg(t, T_OP_ORR, A_LO, A_LO, B_HI, 0);
        t_shift_reg(t, op, A_HI, A_HI, B_LO, 0);
    }
    done = t_b(t);
    t_patch_bcond(t, big, t->len);
    t_alu_imm(t, T_OP_SUB, B_HI, B_LO, 32, 0);
    if (op == T_SH_LSL) {
        t_shift_reg(t, T_SH_LSL, A_HI, A_LO, B_HI, 0);
        t_mov_imm(t, A_LO, 0, 0);
    } else {
        t_shift_reg(t, op, A_LO, A_HI, B_HI, 0);
        if (sign)
            t_shift_imm(t, T_SH_ASR, A_HI, A_HI, 31, 0);
        else
            t_mov_imm(t, A_HI, 0, 0);
    }
    t_patch_b(t, done, t->len);
}

/* The same by a constant, where which arm applies is already known. */
static void shift64_imm(struct t_fn *F, int op, int sign, long n)
{
    struct code *t = F->t;
    if (n <= 0)
        return;
    if (n >= 64)
        n = op == T_SH_ASR ? 63 : 64;
    if (op == T_SH_LSL) {
        if (n >= 32) {
            if (n > 32) t_shift_imm(t, T_SH_LSL, A_LO, A_LO, (int)(n - 32), 0);
            t_mov_reg(t, A_HI, A_LO);
            t_mov_imm(t, A_LO, 0, 0);
        } else {
            t_shift_imm(t, T_SH_LSL, A_HI, A_HI, (int)n, 0);
            t_shift_imm(t, T_SH_LSR, B_HI, A_LO, (int)(32 - n), 0);
            t_alu_reg(t, T_OP_ORR, A_HI, A_HI, B_HI, 0);
            t_shift_imm(t, T_SH_LSL, A_LO, A_LO, (int)n, 0);
        }
        return;
    }
    if (n >= 32) {
        if (n > 32) t_shift_imm(t, op, A_HI, A_HI, (int)(n - 32), 0);
        t_mov_reg(t, A_LO, A_HI);
        if (sign)
            t_shift_imm(t, T_SH_ASR, A_HI, A_HI, 31, 0);
        else
            t_mov_imm(t, A_HI, 0, 0);
        return;
    }
    t_shift_imm(t, T_SH_LSR, A_LO, A_LO, (int)n, 0);   /* always logical */
    t_shift_imm(t, T_SH_LSL, B_HI, A_HI, (int)(32 - n), 0);
    t_alu_reg(t, T_OP_ORR, A_LO, A_LO, B_HI, 0);
    t_shift_imm(t, op, A_HI, A_HI, (int)n, 0);
}

/* ---- a 64-bit operation with a constant, half by half ----------------
 *
 * Each half of `x & 0x000fffffffffffff` is its own question: the low word
 * is ANDed with all ones, which is a copy, and the high one with 0xfffff,
 * which is a ubfx. Building both halves in r9/r10 and ANDing each was four
 * instructions and two pushed registers where one does -- and fdlibm and
 * every soft-float routine are made of exactly these masks on the two
 * words of a double. EMBCC_T_NOWIDEIMM=1 goes back to building them. */
static int g_t_nowideimm = -1;
static int t_wide_imm(void)
{
    if (g_t_nowideimm < 0)
        g_t_nowideimm = getenv("EMBCC_T_NOWIDEIMM") != NULL;
    return !g_t_nowideimm;
}

/* d = s OP c for one 32-bit half, d and s possibly the same register. */
static void logic_half(struct t_fn *F, int op, int d, int s, unsigned long c)
{
    struct code *t = F->t;
    unsigned long nc;
    c &= 0xffffffffUL;
    nc = ~c & 0xffffffffUL;
    if ((op == T_OP_AND && c == 0xffffffffUL) || (op != T_OP_AND && c == 0)) {
        if (d != s)
            t_mov_reg(t, d, s);
        return;
    }
    if (op == T_OP_AND && c == 0) {
        t_mov_imm_dead_flags(t, d, 0);
        return;
    }
    if (op == T_OP_ORR && c == 0xffffffffUL) {
        t_mov_imm_dead_flags(t, d, -1);
        return;
    }
    if (op == T_OP_EOR && c == 0xffffffffUL) {
        t_mvn_reg(t, d, s, 0);
        return;
    }
    if (t_imm_ok((long)c) && t_alu_imm(t, op, d, s, (long)c, 0))
        return;
    if (op == T_OP_AND && t_imm_ok((long)nc) &&
        t_alu_imm(t, T_OP_BIC, d, s, (long)nc, 0))
        return;
    if (op == T_OP_ORR && t_imm_ok((long)nc) &&
        t_alu_imm(t, T_OP_ORN, d, s, (long)nc, 0))
        return;
    if (op == T_OP_AND && (c & (c + 1)) == 0) {      /* the low k bits */
        int k = 0;
        while (c >> k & 1)
            k++;
        t_bfx(t, d, s, 0, k, 0);
        return;
    }
    {
        int tmp = B_LO;
        t_mov_imm_dead_flags(t, tmp, (long)c);
        t_alu_reg(t, op, d, s, tmp, 0);
    }
}

/* A 64-bit shift by a constant from the pair (al, ah) into (dl, dh),
 * which is either the same pair or one sharing no register with it.
 * Each half is written after the last read of the source half it would
 * overwrite, so the in-place case needs nothing in between -- and the
 * bits crossing from one word to the other are the shifted operand of
 * an orr, one instruction where a shift, an orr and r9 were three. */
static void shift64_imm_to(struct t_fn *F, int op, int sign, long n,
                           int al, int ah, int dl, int dh)
{
    struct code *t = F->t;
    if (n <= 0) {
        if (dl != al) t_mov_reg(t, dl, al);
        if (dh != ah) t_mov_reg(t, dh, ah);
        return;
    }
    if (n >= 64)
        n = op == T_SH_ASR ? 63 : 64;
    if (op == T_SH_LSL) {
        if (n >= 32) {
            if (n == 64)     t_mov_imm_dead_flags(t, dh, 0);
            else if (n > 32) t_shift_imm(t, T_SH_LSL, dh, al, (int)(n - 32), 0);
            else if (dh != al) t_mov_reg(t, dh, al);
            t_mov_imm_dead_flags(t, dl, 0);
        } else {
            t_shift_imm(t, T_SH_LSL, dh, ah, (int)n, 0);
            t_alu_reg_shift(t, T_OP_ORR, dh, dh, al, T_SH_LSR, (int)(32 - n), 0);
            t_shift_imm(t, T_SH_LSL, dl, al, (int)n, 0);
        }
        return;
    }
    if (n >= 32) {
        if (n == 64)     t_mov_imm_dead_flags(t, dl, 0);
        else if (n > 32) t_shift_imm(t, op, dl, ah, (int)(n - 32), 0);
        else if (dl != ah) t_mov_reg(t, dl, ah);
        if (sign) t_shift_imm(t, T_SH_ASR, dh, ah, 31, 0);
        else      t_mov_imm_dead_flags(t, dh, 0);
        return;
    }
    t_shift_imm(t, T_SH_LSR, dl, al, (int)n, 0);       /* always logical */
    t_alu_reg_shift(t, T_OP_ORR, dl, dl, ah, T_SH_LSL, (int)(32 - n), 0);
    t_shift_imm(t, op, dh, ah, (int)n, 0);
}

/* Lower one 64-bit instruction. Returns 0 for one this does not handle,
 * which the caller then refuses by name. */
/* Where a 64-bit operand's halves ARE -- its pair, or the given scratch
 * registers after a load -- and where a 64-bit result belongs: its pair,
 * or A. Pairs are whole and never partly overlap (t_pair_alloc), so an
 * operation that reads each half before writing the same half is safe
 * with its result in an operand's pair. */
/* slo and shi are register NUMBERS (R_TMP, not T_TMP): they are recorded
 * as used only when the value is loaded into them. Passed as the marking
 * macros, r9-r11 counted as used by every 64-bit operation whether its
 * operands were in registers or not, and every 64-bit leaf function
 * pushed and popped all three for nothing. */
/* The base register for a 64-bit access through vreg `a`, with *off
 * (memoff on entry) adjusted: the frame base when `a` is a recomputed
 * frame address, else wherever `a` is (B_LO when nothing holds it).
 * Every word of the access is within one ldr's reach of the result. */
static int mem64_base(struct t_fn *F, int a, long *off)
{
    long fo;
    if (faddr(F, a, &fo)) {
        *off += fo;
        if (*off >= -255 && *off <= 4091)
            return F->fb;
        fb_addr(F, B_LO, *off);
        *off = 0;
        return B_LO;
    }
    if (*off < -255 || *off > 4091) {
        /* not reached today: a wide access gets no memoff */
        rd(F, a, B_LO);
        t_mov_imm(F->t, B_HI, *off, 0);
        t_alu_reg(F->t, T_OP_ADD, B_LO, B_LO, B_HI, 0);
        *off = 0;
        return B_LO;
    }
    return rdr(F, a, B_LO);
}

static void src64(struct t_fn *F, int v, int slo, int shi, int *lo, int *hi)
{
    if (in_reg(F, v)) {
        *lo = F->loc[v];
        *hi = F->loc[v] + 1;
        return;
    }
    /* (a role with no register fails the attempt; one that has one is
     * recorded, as the marking macros would) */
    if (slo < 0 || (slo >= 9 && slo <= 11)) slo = t_scr(slo);
    if (shi < 0 || (shi >= 9 && shi <= 11)) shi = t_scr(shi);
    rd64(F, v, slo, shi);
    *lo = slo;
    *hi = shi;
}
static void dst64(struct t_fn *F, int v, int *lo, int *hi)
{
    *lo = in_reg(F, v) ? F->loc[v] : A_LO;
    *hi = in_reg(F, v) ? F->loc[v] + 1 : A_HI;
}
/* The second operand likewise, or an immediate into B. */
static void srcb64(struct t_fn *F, const struct ir_ins *i, int *lo, int *hi)
{
    if (i->imm_b) {
        operand_b64(F, i, B_LO, B_HI);
        *lo = B_LO;
        *hi = B_HI;
        return;
    }
    src64(F, i->b, R_ADDR, R_SCR, lo, hi);
}

/* A double between a vreg and a d register, defined with the FPU below;
 * gen_ins64 asks them whenever one end of a copy is a d register. */
static void vfp_load_d(struct t_fn *F, int v, int d);
static void vfp_store_d(struct t_fn *F, int v, int d);
static void vfp_const_d(struct t_fn *F, int d, unsigned long long bits);
static int vfp_src_d(struct t_fn *F, int v, int scratch);
static int vfp_dst_d(struct t_fn *F, int v, int scratch);
static void vfp_done_d(struct t_fn *F, int v, int d);
/* d0 and d1: the double-precision scratch pair (see vfp_src_d). */
#define T_FD0 0
#define T_FD1 1

/* vldr/vstr of d register `d` at [base, #off]: 0 when the offset is not
 * one the word-scaled eight-bit field holds, for the caller's core path. */
static int vfp_mem_d(struct t_fn *F, int d, int base, long off, int store)
{
    if (off < -1020 || off > 1020 || (off & 3))
        return 0;
    t_vldst(F->t, d, base, (int)off, 1, store);
    return 1;
}

/* A copy whose one end is a double in a d register (t_double_map): a
 * vmov.f64, or the other end loaded into or stored from it -- one vldr or
 * vstr where that end is a slot. Without this every such copy crossed
 * through two core registers both ways. 0 when neither end is one. */
static int copy64_d(struct t_fn *F, int dst, int src)
{
    if (in_freg(F, dst)) {
        vfp_load_d(F, src, t_dreg(F, dst));
        return 1;
    }
    if (in_freg(F, src)) {
        vfp_store_d(F, dst, t_dreg(F, src));
        return 1;
    }
    return 0;
}

/* ---- THE 64-BIT CONSTANT POOL ------------------------------------------
 *
 * A double or a long long constant built in registers is two movw/movt
 * pairs, sixteen bytes, where clang loads it from a literal pool:
 * `ldrd r2, r3, [pc, #n]`, four bytes, and eight of data shared by every
 * load of the same value in the function. Over lib/libc that was 548
 * constants and 5748 bytes, 1500 of them saved by the pool -- fdlibm is
 * made of such constants.
 *
 * Only where it is shorter: 1.0 is `movs r0, #0; movs r1, #0; movt r1,
 * #0x3ff0`, eight bytes, and a value used once must cost more than twelve
 * in registers to be worth its pool slot (t_lit64_plan). The pool sits
 * after the function's last instruction, word-aligned and marked as data;
 * LDRD (literal) reaches 1020 bytes forward of Align(pc, 4). Reach is
 * measured on the first pass, whose code is the longest -- later passes
 * only shrink what lies between a load and the pool -- and a constant
 * that does not reach is built in registers on a first pass made again
 * (which can only push others out of reach in turn, so that repeats until
 * none is). ARM state keeps movw/movt. */
static unsigned t_mov_imm_len(int rd, long v)
{
    struct code c;
    unsigned n;
    memset(&c, 0, sizeof c);
    t_mov_imm_dead_flags(&c, rd, v);
    n = (unsigned)c.len;
    free(c.p);
    free(c.drange);
    return n;
}

static void t_lit64_plan(struct t_fn *F)
{
    const struct ir_func *fn = F->fn;
    int nins = fn->nins, any = 0;
    F->lp_use = NULL;
    if (t_isa_a32 || nins == 0 || getenv("EMBCC_T_NOLIT64"))
        return;
    char *cand = xcalloc((size_t)nins, 1);
    unsigned *cost = xcalloc((size_t)nins, sizeof *cost);
    for (int n = 0; n < nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        if (i->op != IR_CONST || i->w != 8 || i->dst < 0 ||
            i->dst >= fn->nvregs || !F->wide[i->dst] || in_freg(F, i->dst))
            continue;
        int lo = in_reg(F, i->dst) ? F->loc[i->dst] : 12;
        int hi = in_reg(F, i->dst) ? F->loc[i->dst] + 1 : 11;
        cand[n] = 1;
        cost[n] = t_mov_imm_len(lo, (long)(i->imm & 0xffffffffL)) +
                  t_mov_imm_len(hi, (long)((i->imm >> 32) & 0xffffffffL));
    }
    /* by value: the pool when its loads and its slot are shorter than
     * building every one of them */
    for (int n = 0; n < nins; n++) {
        if (cand[n] != 1)
            continue;
        long long v = fn->ins[n].imm;
        unsigned built = 0, cnt = 0;
        for (int m = n; m < nins; m++)
            if (cand[m] == 1 && fn->ins[m].imm == v) {
                built += cost[m];
                cnt++;
            }
        int pool = 4 * cnt + 8 < built;
        for (int m = n; m < nins; m++)
            if (cand[m] == 1 && fn->ins[m].imm == v)
                cand[m] = pool ? 2 : 3;
        any |= pool;
    }
    if (any) {
        F->lp_use = xcalloc((size_t)nins, 1);
        for (int n = 0; n < nins; n++)
            F->lp_use[n] = cand[n] == 2;
    }
    free(cand);
    free(cost);
}

/* `ldrd lo, hi, [pc, #?]` for the constant v: its slot in this pass's
 * pool, and the site t_lit64_flush patches. */
static void t_lit64_load(struct t_fn *F, int n, int lo, int hi,
                         unsigned long long v)
{
    int k;
    for (k = 0; k < F->lp_n; k++)
        if (F->lp_val[k] == v)
            break;
    if (k == F->lp_n && F->lp_planned)
        internal_error("thumb: %s: a constant not in the planned pool",
                       F->fn->name);
    if (k == F->lp_n) {
        if (F->lp_n == F->lp_cap) {
            F->lp_cap = F->lp_cap ? F->lp_cap * 2 : 8;
            F->lp_val = xrealloc(F->lp_val,
                                 (size_t)F->lp_cap * sizeof *F->lp_val);
        }
        F->lp_val[F->lp_n++] = v;
    }
    if (F->lp_nsite == F->lp_capsite) {
        F->lp_capsite = F->lp_capsite ? F->lp_capsite * 2 : 8;
        F->lp_site = xrealloc(F->lp_site,
                              (size_t)F->lp_capsite * sizeof *F->lp_site);
    }
    struct t_lsite *s = &F->lp_site[F->lp_nsite++];
    s->at = F->t->len;
    s->ins = n;
    s->idx = k;
    s->rt = lo;
    s->rt2 = hi;
    if (!t_ldst_pair(F->t, lo, hi, T_PC, 0, 0))
        internal_error("thumb: %s: a literal ldrd into r%d:r%d",
                       F->fn->name, lo, hi);
}

/* What fits, when not everything did: walking back from the last load,
 * keep a constant when its load reaches its slot -- the code after the
 * load, plus twelve bytes for each later constant already given up (a
 * movw/movt pair for each of its words where the ldrd was four), plus its
 * slot -- and give it up otherwise. Slots go in that order, so the loads
 * nearest the end get the nearest slots, and one value keeps one slot.
 * An estimate from this pass's positions, which are the longest any pass
 * has (and twelve is the most a give-up grows by); the pass made again
 * measures exactly. */
static void t_lit64_fit(struct t_fn *F, int pool)
{
    int ns = F->lp_nsite, nv = 0;
    unsigned long long *val = xmalloc((size_t)(ns ? ns : 1) * sizeof *val);
    long extra = 0;
    for (int k = ns - 1; k >= 0; k--) {
        const struct t_lsite *s = &F->lp_site[k];
        unsigned long long v = F->lp_val[s->idx];
        int slot;
        for (slot = 0; slot < nv; slot++)
            if (val[slot] == v)
                break;
        long d = (long)pool - ((s->at + 4) & ~3L) + extra + 8L * slot;
        if (d <= 1020 - 8) {
            if (slot == nv)
                val[nv++] = v;
        } else {
            F->lp_use[s->ins] = 0;
            extra += 12;
        }
    }
    for (int k = 0; k < nv; k++)
        F->lp_val[k] = val[k];
    F->lp_n = nv;
    F->lp_planned = 1;
    free(val);
}

/* Drop the planned slots no load uses any more (a later first pass gave
 * one up), keeping the order: a slot only moves nearer. */
static void t_lit64_compact(struct t_fn *F)
{
    int nv = 0;
    for (int k = 0; k < F->lp_n; k++) {
        int used = 0;
        for (int m = 0; m < F->lp_nsite && !used; m++)
            used = F->lp_site[m].idx == k && F->lp_use[F->lp_site[m].ins];
        if (used)
            F->lp_val[nv++] = F->lp_val[k];
    }
    F->lp_n = nv;
}

/* The pool after the function's code, and every load patched to reach
 * it. 0 when one does not: then t_lit64_fit (or, once planned, the load
 * alone) gives constants up to be built in registers, and the caller
 * makes the first pass again. */
static int t_lit64_flush(struct t_fn *F, int pass)
{
    struct code *t = F->t;
    int ok = 1;
    if (!F->lp_n)
        return 1;
    while (t->len & 3)
        t_nop(t);
    int pool = t->len;
    for (int k = 0; k < F->lp_n; k++) {
        code_u32(t, (unsigned long)(F->lp_val[k] & 0xffffffffULL));
        code_u32(t, (unsigned long)(F->lp_val[k] >> 32));
    }
    code_mark_data(t, pool, t->len);
    for (int k = 0; k < F->lp_nsite; k++) {
        const struct t_lsite *s = &F->lp_site[k];
        long off = (long)pool + 8L * s->idx - ((s->at + 4) & ~3L);
        struct code c;
        memset(&c, 0, sizeof c);
        if (off >= 0 && t_ldst_pair(&c, s->rt, s->rt2, T_PC, off, 0)) {
            memcpy(t->p + s->at, c.p, 4);
        } else if (pass == 0) {
            if (F->lp_planned)
                F->lp_use[s->ins] = 0;
            ok = 0;
        } else {
            internal_error("thumb: %s: a literal ldrd no longer reaches its "
                           "pool", F->fn->name);
        }
        free(c.p);
        free(c.drange);
    }
    if (!ok) {
        if (F->lp_planned)
            t_lit64_compact(F);
        else
            t_lit64_fit(F, pool);
    }
    return ok;
}

static int gen_ins64(struct t_fn *F, int n)
{
    struct ir_func *fn = F->fn;
    struct ir_ins *i = &fn->ins[n];
    struct code *t = F->t;

    switch (i->op) {
    case IR_CONST:
        /* A double constant in a d register: vmov.f64 #imm when it is one
         * of VFP's, else its words through a core register. */
        if (in_freg(F, i->dst)) {
            vfp_const_d(F, t_dreg(F, i->dst), (unsigned long long)i->imm);
            return 1;
        }
    {
        /* Built where it lives when that is a pair. */
        int lo = in_reg(F, i->dst) ? F->loc[i->dst] : A_LO;
        int hi = in_reg(F, i->dst) ? F->loc[i->dst] + 1 : A_HI;
        if (F->lp_use && F->lp_use[n]) {
            t_lit64_load(F, n, lo, hi, (unsigned long long)i->imm);
            wr64(F, i->dst, lo, hi);
            return 1;
        }
        /* no flag survives from one IR instruction to the next (the
         * 32-bit IR_CONST says why), so a half that fits eight bits in a
         * low register is a two-byte movs */
        t_mov_imm_dead_flags(t, lo, (long)(i->imm & 0xffffffffL));
        t_mov_imm_dead_flags(t, hi, (long)((i->imm >> 32) & 0xffffffffL));
        wr64(F, i->dst, lo, hi);
        return 1;
    }
    /* This target has no floating-point register file: a double
     * already lives in a general register pair, so reinterpreting
     * its bits is a copy and nothing else. */
    case IR_BITCAST:
    case IR_MOV:
        if (copy64_d(F, i->dst, i->a))
            return 1;
        /* Straight between the two homes; within one pair, nothing. */
        if (in_reg(F, i->dst)) {
            rd64(F, i->a, F->loc[i->dst], F->loc[i->dst] + 1);
        } else if (in_reg(F, i->a)) {
            wr64(F, i->dst, F->loc[i->a], F->loc[i->a] + 1);
        } else {
            rd64(F, i->a, A_LO, A_HI);
            wr64(F, i->dst, A_LO, A_HI);
        }
        return 1;

    case IR_ADD: case IR_SUB: {
        /* The carry must survive from one instruction to the next, so
         * nothing may come between them -- which is why both operands
         * are fully in registers before either is emitted. In place: the
         * low result never lands on a high operand, pairs being whole. */
        int al, ah, bl, bh, dl, dh;
        src64(F, i->a, A_LO, R_TMP, &al, &ah);
        /* A constant whose two words both encode as immediates is used
         * as one -- adds/adc, subs/sbc #imm -- instead of being built in
         * r9/r10 first, which also saves pushing them. Both are checked
         * before either is emitted: the carry links the pair. */
        if (i->imm_b) {
            unsigned long lo = (unsigned long)i->imm & 0xffffffffUL;
            unsigned long hi = ((unsigned long)i->imm >> 32) & 0xffffffffUL;
            if (t_imm_ok((long)lo) && t_imm_ok((long)hi)) {
                int add = i->op == IR_ADD;
                dst64(F, i->dst, &dl, &dh);
                if (!t_alu_imm(t, add ? T_OP_ADD : T_OP_SUB, dl, al,
                               (long)lo, 1) ||
                    !t_alu_imm(t, add ? T_OP_ADC : T_OP_SBC, dh, ah,
                               (long)hi, 1))
                    internal_error("thumb: a 64-bit %s immediate that "
                                   "encodes did not", add ? "add" : "sub");
                wr64(F, i->dst, dl, dh);
                return 1;
            }
        }
        srcb64(F, i, &bl, &bh);
        dst64(F, i->dst, &dl, &dh);
        if (i->op == IR_ADD) {
            t_alu_reg(t, T_OP_ADD, dl, al, bl, 1);
            t_alu_reg(t, T_OP_ADC, dh, ah, bh, 1);
        } else {
            t_alu_reg(t, T_OP_SUB, dl, al, bl, 1);
            t_alu_reg(t, T_OP_SBC, dh, ah, bh, 1);
        }
        wr64(F, i->dst, dl, dh);
        return 1;
    }

    case IR_AND: case IR_OR: case IR_XOR: {
        int op = i->op == IR_AND ? T_OP_AND
               : i->op == IR_OR  ? T_OP_ORR : T_OP_EOR;
        int al, ah, bl, bh, dl, dh;
        /* fabs and negation of a double in a d register: one VFP
         * instruction where the core path is two vmovs around a bic or
         * an eor (t_sign_mask_op says why it is the same thing). */
        if (t_sign_mask_op(i) && (in_freg(F, i->dst) || in_freg(F, i->a))) {
            int a = vfp_src_d(F, i->a, T_FD0);
            int d = vfp_dst_d(F, i->dst, T_FD0);
            if (i->op == IR_AND) t_vabs(t, d, a, 1);
            else                 t_vneg(t, d, a, 1);
            vfp_done_d(F, i->dst, d);
            return 1;
        }
        src64(F, i->a, A_LO, R_TMP, &al, &ah);
        if (i->imm_b && t_wide_imm()) {
            dst64(F, i->dst, &dl, &dh);
            logic_half(F, op, dl, al, (unsigned long)i->imm);
            logic_half(F, op, dh, ah, (unsigned long)i->imm >> 32);
            wr64(F, i->dst, dl, dh);
            return 1;
        }
        srcb64(F, i, &bl, &bh);
        dst64(F, i->dst, &dl, &dh);
        t_alu_reg(t, op, dl, al, bl, 0);
        t_alu_reg(t, op, dh, ah, bh, 0);
        wr64(F, i->dst, dl, dh);
        return 1;
    }

    case IR_BNOT: {
        int al, ah, dl, dh;
        src64(F, i->a, A_LO, R_TMP, &al, &ah);
        dst64(F, i->dst, &dl, &dh);
        t_mvn_reg(t, dl, al, 0);
        t_mvn_reg(t, dh, ah, 0);
        wr64(F, i->dst, dl, dh);
        return 1;
    }

    case IR_BSWAP: {
        /* Each word reversed, and the two words exchanged. The low word's
         * reversal goes through B first: the result may share the
         * operand's registers, and its low word would overwrite the
         * operand's before it was read. */
        int al, ah, dl, dh, tmp;
        src64(F, i->a, A_LO, R_TMP, &al, &ah);
        dst64(F, i->dst, &dl, &dh);
        /* r12 when neither pair is in A (it needs no saving); B else */
        tmp = al != A_LO && ah != A_LO && dl != A_LO && dh != A_LO
            ? A_LO : B_LO;
        t_rev(t, tmp, al);
        t_rev(t, dl, ah);
        t_mov_reg(t, dh, tmp);
        wr64(F, i->dst, dl, dh);
        return 1;
    }

    case IR_NEG: {
        /* 0 - a. `rsbs` leaves C clear exactly when the low word
         * borrowed, and `sbc` from zero is the high half. */
        int al, ah, dl, dh;
        src64(F, i->a, A_LO, R_TMP, &al, &ah);
        dst64(F, i->dst, &dl, &dh);
        t_mov_imm(t, B_LO, 0, 0);
        t_alu_imm(t, T_OP_RSB, dl, al, 0, 1);
        t_alu_reg(t, T_OP_SBC, dh, B_LO, ah, 0);
        wr64(F, i->dst, dl, dh);
        return 1;
    }

    case IR_MULW: {
        /* smull/umull: the 64-bit product of two words, one instruction
         * that reads both before writing either half, so the result may
         * land on its operands. The operands are single words wherever
         * they live; B's registers take them, A is the result's. */
        int sa = rdr(F, i->a, B_LO), sb = rdr(F, i->b, B_HI), dl, dh;
        /* With the 64-bit add that is its one reader right after it,
         * smlal/umlal: acc += a * b in one, where the add was four
         * instructions of its own -- a long dot product, a fixed-point
         * filter with a 64-bit accumulator. The accumulator goes into
         * the result's pair first, which must then hold neither
         * operand. */
        if (F->usecnt && i->dst >= 0 && F->usecnt[i->dst] == 1 &&
            n + 1 < fn->nins && !getenv("EMBCC_NO_MLAL")) {
            const struct ir_ins *nx = &fn->ins[n + 1];
            int c = -1;
            if (nx->op == IR_ADD && !nx->imm_b && !nx->flt && nx->w == 8 &&
                nx->dst >= 0 && F->wide[nx->dst]) {
                if (nx->b == i->dst && nx->a != i->dst)
                    c = nx->a;
                else if (nx->a == i->dst && nx->b != i->dst)
                    c = nx->b;
            }
            if (c >= 0 && F->wide[c] && !in_freg(F, c) &&
                !in_freg(F, nx->dst)) {
                dst64(F, nx->dst, &dl, &dh);
                int same = in_reg(F, c) && F->loc[c] == dl;
                if (same || (dl != sa && dl != sb && dh != sa && dh != sb)) {
                    if (!same)
                        rd64(F, c, dl, dh);
                    t_mlal(t, dl, dh, sa, sb, i->sign);
                    wr64(F, nx->dst, dl, dh);
                    F->skip_next = 1;
                    return 1;
                }
            }
        }
        dst64(F, i->dst, &dl, &dh);
        t_mull(t, dl, dh, sa, sb, i->sign);
        wr64(F, i->dst, dl, dh);
        return 1;
    }

    case IR_MUL:
        /* (a_hi:a_lo) * (b_hi:b_lo), keeping 64 bits: the two cross
         * products contribute only to the high word, and the low
         * product's carry comes out of umull's own high half. */
        rd64(F, i->a, A_LO, A_HI);
        operand_b64(F, i, B_LO, B_HI);
        t_mul(t, B_HI, A_LO, B_HI);              /* a_lo * b_hi */
        t_mla(t, B_HI, A_HI, B_LO, B_HI);        /* += a_hi * b_lo */
        t_mull(t, A_LO, A_HI, A_LO, B_LO, 0);    /* a_lo * b_lo */
        t_alu_reg(t, T_OP_ADD, A_HI, A_HI, B_HI, 0);
        wr64(F, i->dst, A_LO, A_HI);
        return 1;

    case IR_SHL: case IR_SHR: {
        int op = i->op == IR_SHL ? T_SH_LSL
               : i->sign ? T_SH_ASR : T_SH_LSR;
        if (i->imm_b && t_wide_imm()) {
            int al, ah, dl, dh;
            src64(F, i->a, A_LO, R_TMP, &al, &ah);
            dst64(F, i->dst, &dl, &dh);
            shift64_imm_to(F, op, i->sign, (long)i->imm, al, ah, dl, dh);
            wr64(F, i->dst, dl, dh);
            return 1;
        }
        rd64(F, i->a, A_LO, A_HI);
        if (i->imm_b)
            shift64_imm(F, op, i->sign, (long)i->imm);
        else {
            rd(F, i->b, B_LO);
            shift64_var(F, op, i->sign);
        }
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    }

    case IR_EXT:
        /* Widening to 64 bits: the low word is the source, extended to
         * 32 first if it was narrower, and the high word is zero or the
         * sign. */
        rd(F, i->a, A_LO);
        if (i->size < 4)
            t_ext(t, A_LO, A_LO, i->size, i->sign);
        if (i->sign)
            t_shift_imm(t, T_SH_ASR, A_HI, A_LO, 31, 0);
        else
            t_mov_imm(t, A_HI, 0, 0);
        wr64(F, i->dst, A_LO, A_HI);
        return 1;

    /* A 64-bit RESULT from a narrower access is a load plus an
     * extension: `long long x = *(int *)p` reads four bytes and fills
     * the high word from the sign. rd64 on a four-byte slot would read
     * the neighbouring temp as the high half. */
    case IR_LDVAR:
        if (i->size == 8 && copy64_d(F, i->dst, i->a))
            return 1;
        if (i->size == 8) {
            rd64(F, i->a, A_LO, A_HI);
        } else {
            /* A narrow local widened: from its register when it has one,
             * as the 32-bit IR_LDVAR does. This read its slot regardless,
             * which a local in a register does not have. */
            if (in_reg(F, i->a)) {
                if (i->size >= 4) t_mov_reg(t, A_LO, F->loc[i->a]);
                else              t_ext(t, A_LO, F->loc[i->a], i->size, i->sign);
            } else if (!t_ldst_imm(t, A_LO, F->fb, slot_of(F, i->a), i->size,
                                   i->sign, 0)) {
                fb_addr(F, B_LO, F->slot[i->a]);
                ldst_must(t, A_LO, B_LO, 0, i->size, i->sign, 0);
            }
            if (i->sign) t_shift_imm(t, T_SH_ASR, A_HI, A_LO, 31, 0);
            else         t_mov_imm(t, A_HI, 0, 0);
        }
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    case IR_STVAR:
        if (i->size == 8 && copy64_d(F, i->dst, i->a))
            return 1;
        rd64(F, i->a, A_LO, A_HI);
        if (i->size == 8) {
            wr64(F, i->dst, A_LO, A_HI);
        } else if (in_reg(F, i->dst)) {  /* a truncating store, to a reg */
            if (i->size >= 4) t_mov_reg(t, F->loc[i->dst], A_LO);
            else              t_ext(t, F->loc[i->dst], A_LO, i->size, 0);
        } else {                       /* a truncating store */
            if (!t_ldst_imm(t, A_LO, F->fb, slot_of(F, i->dst), i->size, 0, 1)) {
                fb_addr(F, B_LO, F->slot[i->dst]);
                ldst_must(t, A_LO, B_LO, 0, i->size, 0, 1);
            }
        }
        return 1;
    case IR_LOAD: {
        /* Straight into the destination's pair, and through a local's
         * address as the frame base plus its offset. One ldrd where the
         * address is known aligned (a packed member is not, and ldrd
         * faults on one); else two loads, ordered so the base is read
         * before a destination register that is the base overwrites it. */
        int base, dl, dh;
        long off = i->memoff;
        base = mem64_base(F, i->a, &off);
        /* Into a d register with one vldr -- only where the address is
         * KNOWN aligned: vldr faults on a misaligned one where ldr would
         * not, as for a float in an S register. */
        if (i->size == 8 && in_freg(F, i->dst) && i->natural &&
            vfp_mem_d(F, t_dreg(F, i->dst), base, off, 0))
            return 1;
        dst64(F, i->dst, &dl, &dh);
        if (i->size == 8) {
            if (!(i->natural && t_ldst_pair(t, dl, dh, base, off, 0))) {
                int first = dl == base ? dh : dl;
                ldst_must(t, first, base, first == dl ? off : off + 4,
                          4, 0, 0);
                if (first == dl) ldst_must(t, dh, base, off + 4, 4, 0, 0);
                else             ldst_must(t, dl, base, off, 4, 0, 0);
            }
        } else {
            ldst_must(t, dl, base, off, i->size, i->sign, 0);
            if (i->sign) t_shift_imm(t, T_SH_ASR, dh, dl, 31, 0);
            else         t_mov_imm(t, dh, 0, 0);
        }
        wr64(F, i->dst, dl, dh);
        return 1;
    }
    case IR_STORE: {
        int base, sl, sh;
        long off = i->memoff;
        base = mem64_base(F, i->a, &off);
        if (i->size == 8 && in_freg(F, i->b) && i->natural &&
            vfp_mem_d(F, t_dreg(F, i->b), base, off, 1))
            return 1;                    /* see IR_LOAD */
        src64(F, i->b, A_LO, A_HI, &sl, &sh);
        if (i->size == 8) {
            if (!(i->natural && t_ldst_pair(t, sl, sh, base, off, 1))) {
                ldst_must(t, sl, base, off, 4, 0, 1);
                ldst_must(t, sh, base, off + 4, 4, 0, 1);
            }
        } else {
            ldst_must(t, sl, base, off, i->size, 0, 1);
        }
        return 1;
    }

    case IR_SELECT:
        /* A double either of whose arms, or whose result, is in a d
         * register: the arm is moved straight into the result's d
         * register (or d0, then stored). Nothing after the test sets the
         * flags -- vmov.f64, vldr and the core loads of rd64 do not. */
        if (in_freg(F, i->dst) || in_freg(F, i->b) || in_freg(F, i->c)) {
            int d = vfp_dst_d(F, i->dst, T_FD0);
            if (i->size == 8) {
                rd64(F, i->a, A_LO, A_HI);
                t_alu_reg(t, T_OP_ORR, A_LO, A_LO, A_HI, 0);
                t_cmp_imm(t, A_LO, 0);
            } else {
                t_cmp_imm(t, rdr(F, i->a, B_LO), 0);
            }
            {
                int take_c = t_bcond(t, T_EQ);
                vfp_load_d(F, i->b, d);
                {
                    int done = t_b(t);
                    t_patch_bcond(t, take_c, t->len);
                    vfp_load_d(F, i->c, d);
                    t_patch_b(t, done, t->len);
                }
            }
            vfp_done_d(F, i->dst, d);
            return 1;
        }
        if (i->size == 8) {            /* a 64-bit condition: either half */
            rd64(F, i->a, A_LO, A_HI);
            t_alu_reg(t, T_OP_ORR, A_LO, A_LO, A_HI, 0);
            t_cmp_imm(t, A_LO, 0);
        } else {
            rd(F, i->a, B_LO);
            t_cmp_imm(t, B_LO, 0);
        }
        {
            int take_c = t_bcond(t, T_EQ);
            rd64(F, i->b, A_LO, A_HI);
            {
                int done = t_b(t);
                t_patch_bcond(t, take_c, t->len);
                rd64(F, i->c, A_LO, A_HI);
                t_patch_b(t, done, t->len);
            }
        }
        wr64(F, i->dst, A_LO, A_HI);
        return 1;

    default:
        return 0;
    }
}

/* A 64-bit comparison, leaving the flags for `cond`. Returns the
 * condition to branch on, which is not always the one the predicate
 * names: there is no way to read "greater than" out of a subtraction's
 * flags directly, so the operands are swapped and the mirrored
 * predicate used instead. */
static int cmp64(struct t_fn *F, const struct ir_ins *i, enum binop pred,
                 int sign)
{
    struct code *t = F->t;
    int swap = pred == B_GT || pred == B_LE;
    int al, ah, bl, bh;
    /* Against zero, the common case: signed `< 0` / `>= 0` is the high
     * word's sign alone (cmp with 0 clears V, so LT is N), and `== 0` /
     * `!= 0` is an OR of the halves -- no second operand, no subtract. */
    if (i->imm_b && i->imm == 0 &&
        (pred == B_EQ || pred == B_NE ||
         (sign && (pred == B_LT || pred == B_GE)))) {
        src64(F, i->a, A_LO, R_TMP, &al, &ah);
        if (pred == B_EQ || pred == B_NE) {
            t_alu_reg(t, T_OP_ORR, T_ACC, al, ah, 1);
            return pred == B_EQ ? T_EQ : T_NE;
        }
        t_cmp_imm(t, ah, 0);
        return pred == B_LT ? T_LT : T_GE;
    }
    /* Against any other constant K, K's halves are immediates when
     * thumb_cmp64_imm says they encode, and nothing is swapped into the
     * scratch registers: the operand stays where it lives and only r12
     * is written. strtol's range checks (`v > LONG_MAX`) moved the
     * constant into r2:r3, it and the value into r9-r12, and pushed
     * r9-r11 to do it: 170 bytes where clang has 76. */
    if (i->imm_b) {
        int p, form;
        long lo, hi;
        form = thumb_cmp64_imm(pred, sign, i->imm, &p, &lo, &hi);
        if (form) {
            src64(F, i->a, A_LO, R_TMP, &al, &ah);
            if (form == 3) {
                /* Z only when both halves are equal */
                t_cmp_imm(t, al, lo);
                t_it(t, T_EQ, "");
                t_cmp_imm(t, ah, hi);
                return p == B_EQ ? T_EQ : T_NE;
            }
            if (form == 1) {
                t_alu_imm(t, T_OP_SUB, A_LO, al, lo, 1);
                t_alu_imm(t, T_OP_SBC, A_LO, ah, hi, 1);
            } else {
                /* K - a: the low half, then ~ah + K's high half + C,
                 * which is SBC's AddWithCarry with its operands in the
                 * other order. The mvn sets no flags. */
                t_alu_imm(t, T_OP_RSB, A_LO, al, lo, 1);
                t_mvn_reg(t, A_LO, ah, 0);
                t_alu_imm(t, T_OP_ADC, A_LO, A_LO, hi, 1);
            }
            if (p == B_LT) return sign ? T_LT : T_CC;
            return sign ? T_GE : T_CS;
        }
    }
    /* Swapped, the operands are read where they live as well: copying
     * both into r9-r12 first pushed three callee-saved registers in a
     * leaf function whose operands were already in r0-r3. */
    src64(F, i->a, A_LO, R_TMP, &al, &ah);
    srcb64(F, i, &bl, &bh);
    if (swap) {
        int x = al, y = ah;
        al = bl; ah = bh; bl = x; bh = y;
        pred = pred == B_GT ? B_LT : B_GE;
    }
    if (pred == B_EQ || pred == B_NE) {
        /* Equality needs both halves, and `sbcs` only reports Z for the
         * high one: `cmp lo; it eq; cmpeq hi` sets Z only when both
         * are equal. (An eor of each half and an orr wanted r11 for the
         * high one, and pushed it.) */
        t_cmp_reg(t, al, bl);
        t_it(t, T_EQ, "");
        t_cmp_reg(t, ah, bh);
        return pred == B_EQ ? T_EQ : T_NE;
    }
    /* Only the flags are wanted: both differences go to r12, leaving the
     * operands where they live. (The high one went to r11, which is
     * callee-saved, so every function comparing two 64-bit values pushed
     * it. The low difference is dead before the high one is written, and
     * neither ah nor bh can be r12.) */
    t_alu_reg(t, T_OP_SUB, A_LO, al, bl, 1);
    t_alu_reg(t, T_OP_SBC, A_LO, ah, bh, 1);
    if (pred == B_LT) return sign ? T_LT : T_CC;
    return sign ? T_GE : T_CS;             /* B_GE */
}


/* ---- the FPU, for single precision -------------------------------------
 *
 * S0 and S1 are the scratch pair, chosen the way T_ACC and T_TMP are:
 * caller-saved in AAPCS-VFP (s0-s15 are), so using them costs no
 * prologue. Nothing else in this backend touches an FP register yet,
 * which is why two are enough.
 */
#define T_FS0 0
#define T_FS1 1

/* A single-precision value from its slot into an FP register, and back.
 * The offset field is in WORDS, so it reaches 1020 bytes -- further than
 * the integer immediate forms -- and a slot beyond that needs the
 * address computed first. */
static void vfp_load(struct t_fn *F, int v, int sreg)
{
    if (in_freg(F, v)) {
        if (F->floc[v] != sreg)
            t_vmov_reg(F->t, sreg, F->floc[v], 0);
        return;
    }
    if (in_reg(F, v)) {
        /* The value is in a CORE register: one move, no memory. */
        t_vmov_core(F->t, sreg, F->loc[v], 1);
        return;
    }
    if (F->slot[v] >= 0 && F->slot[v] <= 1020 && !(F->slot[v] & 3)) {
        t_vldst(F->t, sreg, F->fb, (int)F->slot[v], 0, 0);
        return;
    }
    rd(F, v, T_ACC);                    /* the general path, via a core reg */
    t_vmov_core(F->t, sreg, T_ACC, 1);
}

static void vfp_store(struct t_fn *F, int v, int sreg)
{
    if (v < 0)
        return;
    if (in_freg(F, v)) {
        if (F->floc[v] != sreg)
            t_vmov_reg(F->t, F->floc[v], sreg, 0);
        return;
    }
    if (in_reg(F, v)) {
        t_vmov_core(F->t, sreg, F->loc[v], 0);
        return;
    }
    if (F->slot[v] >= 0 && F->slot[v] <= 1020 && !(F->slot[v] & 3)) {
        t_vldst(F->t, sreg, F->fb, (int)F->slot[v], 0, 1);
        return;
    }
    t_vmov_core(F->t, sreg, T_ACC, 0);
    wr(F, v, T_ACC);
}

/* A DOUBLE between its vreg and d register `d`. Without a double-precision
 * unit the vreg is a pair of words on the frame; FPv4-SP-D16 cannot
 * compute in double precision, but it can hold, load, store and move one,
 * which is all the hard-float convention asks of it at a call. With
 * FPv5-D16 it may live in a d register of its own (t_double_map), and
 * then this is a vmov.f64 or nothing.
 *
 * A pair in core registers is reached through rd64/wr64, which know it;
 * vfp_slot_ok() is asked only of a value that has neither kind of home. */
static int vfp_slot_ok(const struct t_fn *F, int v)
{
    return !in_reg(F, v) &&
           F->slot[v] >= 0 && F->slot[v] <= 1020 && !(F->slot[v] & 3);
}

static void vfp_load_d(struct t_fn *F, int v, int d)
{
    if (in_freg(F, v)) {
        if (t_dreg(F, v) != d)
            t_vmov_reg(F->t, d, t_dreg(F, v), 1);
        return;
    }
    if (vfp_slot_ok(F, v)) {
        t_vldst(F->t, d, F->fb, (int)F->slot[v], 1, 0);
        return;
    }
    rd64(F, v, T_ACC, T_TMP);
    t_vmov_core_pair(F->t, d, T_ACC, T_TMP, 1);
}

static void vfp_store_d(struct t_fn *F, int v, int d)
{
    if (v < 0)
        return;
    if (in_freg(F, v)) {
        if (t_dreg(F, v) != d)
            t_vmov_reg(F->t, t_dreg(F, v), d, 1);
        return;
    }
    if (!in_reg(F, v) && F->slot[v] < 0)
        return;
    if (vfp_slot_ok(F, v)) {
        t_vldst(F->t, d, F->fb, (int)F->slot[v], 1, 1);
        return;
    }
    t_vmov_core_pair(F->t, d, T_ACC, T_TMP, 0);
    wr64(F, v, T_ACC, T_TMP);
}

/* A homogeneous aggregate between memory at [base, #0...] and the VFP
 * registers from s`s0` -- element by element, doubles as d registers. */
static void vfp_aggregate(struct t_fn *F, int base, int s0, int n, int dbl,
                          int store)
{
    for (int e = 0; e < n; e++)
        t_vldst(F->t, dbl ? s0 / 2 + e : s0 + e, base, e * (dbl ? 8 : 4),
                dbl, store);
}

/* Returns 1 when this instruction was emitted on the FPU, 0 to fall
 * through to the soft-float helper. Saying 0 rather than refusing is
 * deliberate: an operation the FPU cannot do is not an error, it is a
 * call -- `double` on an SP-only part is the whole reason. */
/* An operand's S register: its home when it has one, else `scratch`
 * loaded with it. And a result's: its home, else `scratch`, which
 * vfp_done then stores. Computing straight into and out of s16-s31 is
 * the point of the register class -- vadd s17, s16, s18, one
 * instruction where the slots needed four. */
static int vfp_src(struct t_fn *F, int v, int scratch)
{
    if (in_freg(F, v))
        return F->floc[v];
    vfp_load(F, v, scratch);
    return scratch;
}
static int vfp_dst(struct t_fn *F, int v, int scratch)
{
    return in_freg(F, v) ? F->floc[v] : scratch;
}
static void vfp_done(struct t_fn *F, int v, int sreg)
{
    if (!in_freg(F, v))
        vfp_store(F, v, sreg);
}

/* The second operand into s register `sreg`: a vreg, or a folded
 * constant, which for a float operation is its bit pattern. */
static int vfp_operand_b(struct t_fn *F, const struct ir_ins *i, int sreg)
{
    if (i->imm_b) {
        t_mov_imm(F->t, T_ACC, (long)i->imm, 0);
        t_vmov_core(F->t, sreg, T_ACC, 1);
        return sreg;
    }
    return vfp_src(F, i->b, sreg);
}

/* A float comparison's condition, read after vmrs. Each is chosen so an
 * UNORDERED result (a NaN operand sets C and V) makes it false, as C
 * requires of everything but !=: MI rather than LT, LS rather than LE.
 * Its inverse (the low bit flipped) is then exactly "not this", NaN
 * included, which is what a fused branch on the false side needs. */
static int fp_cond_for(enum binop pred)
{
    switch (pred) {
    case B_EQ: return T_EQ;
    case B_NE: return T_NE;
    case B_LT: return T_MI;
    case B_LE: return T_LS;
    case B_GT: return T_GT;
    default:   return T_GE;          /* B_GE */
    }
}

/* ---- the FPU, for double precision (FPv5-D16) ---------------------------
 *
 * The same shape as the single-precision trio above, on d registers: an
 * operand's home when it has one, else d0 or d1 loaded with it, and the
 * result computed straight into its home or into d0 and stored. d0 and
 * d1 are s0-s3, caller-saved like the singles' scratch pair and holding
 * nothing between instructions (T_FD0 and T_FD1, defined with the 64-bit
 * paths, which reach them too).
 */
static int vfp_src_d(struct t_fn *F, int v, int scratch)
{
    if (in_freg(F, v))
        return t_dreg(F, v);
    vfp_load_d(F, v, scratch);
    return scratch;
}
static int vfp_dst_d(struct t_fn *F, int v, int scratch)
{
    return in_freg(F, v) ? t_dreg(F, v) : scratch;
}
static void vfp_done_d(struct t_fn *F, int v, int d)
{
    if (!in_freg(F, v))
        vfp_store_d(F, v, d);
}

/* A double constant into d register `d`: one vmov.f64 when VFPExpandImm
 * reaches it (1.0, 0.5, 10.0...), else its words built in core registers
 * and moved across -- one register for both when they are the same, which
 * 0.0 is. */
static void vfp_const_d(struct t_fn *F, int d, unsigned long long bits)
{
    int k = t_vfp_imm8(bits, 1);
    long lo = (long)(bits & 0xffffffffULL), hi = (long)(bits >> 32);
    if (k >= 0) {
        t_vmov_imm(F->t, d, k, 1);
        return;
    }
    t_mov_imm(F->t, T_ACC, lo, 0);
    if (hi == lo) {
        t_vmov_core_pair(F->t, d, T_ACC, T_ACC, 1);
        return;
    }
    t_mov_imm(F->t, T_TMP, hi, 0);
    t_vmov_core_pair(F->t, d, T_ACC, T_TMP, 1);
}

/* The second operand into d register `d`: a vreg, or a folded constant,
 * which for a double operation is its bit pattern. */
static int vfp_operand_b_d(struct t_fn *F, const struct ir_ins *i, int d)
{
    if (i->imm_b) {
        vfp_const_d(F, d, (unsigned long long)i->imm);
        return d;
    }
    return vfp_src_d(F, i->b, d);
}

static void fp_vfp_cmp(struct t_fn *F, const struct ir_ins *i)
{
    int dbl = i->w == 8;
    int a = dbl ? vfp_src_d(F, i->a, T_FD0) : vfp_src(F, i->a, T_FS0);
    int b = dbl ? vfp_operand_b_d(F, i, T_FD1) : vfp_operand_b(F, i, T_FS1);
    if (i->pred == B_EQ || i->pred == B_NE)
        t_vcmp(F->t, a, b, dbl);
    else
        t_vcmpe(F->t, a, b, dbl);
    t_vmrs_apsr(F->t);
}

static int fp_vfp_arith(struct t_fn *F, const struct ir_ins *i)
{
    struct code *t = F->t;
    /* A double here only on FPv5-D16, which fp_on_vfp() checked. */
    int dbl = i->w == 8;
    switch (i->op) {
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: {
        int a = dbl ? vfp_src_d(F, i->a, T_FD0) : vfp_src(F, i->a, T_FS0);
        int b = dbl ? vfp_operand_b_d(F, i, T_FD1)
                    : vfp_operand_b(F, i, T_FS1);
        int d = dbl ? vfp_dst_d(F, i->dst, T_FD0)
                    : vfp_dst(F, i->dst, T_FS0);
        if (i->op == IR_ADD)      t_vadd(t, d, a, b, dbl);
        else if (i->op == IR_SUB) t_vsub(t, d, a, b, dbl);
        else if (i->op == IR_MUL) t_vmul(t, d, a, b, dbl);
        else                      t_vdiv(t, d, a, b, dbl);
        if (dbl) vfp_done_d(F, i->dst, d);
        else     vfp_done(F, i->dst, d);
        return 1;
    }
    case IR_NEG: case IR_SQRT: {
        int a = dbl ? vfp_src_d(F, i->a, T_FD0) : vfp_src(F, i->a, T_FS0);
        int d = dbl ? vfp_dst_d(F, i->dst, T_FD0)
                    : vfp_dst(F, i->dst, T_FS0);
        if (i->op == IR_NEG) t_vneg(t, d, a, dbl);
        else                 t_vsqrt(t, d, a, dbl);
        if (dbl) vfp_done_d(F, i->dst, d);
        else     vfp_done(F, i->dst, d);
        return 1;
    }
    /* IR_CMP is deliberately NOT here yet. vcmp writes FPSCR and only
     * vmrs moves that to APSR, so the comparison is two instructions and
     * then the 0/1 result still has to be materialised from the flags --
     * a different shape from the helper, which returns it in r0. Emitting
     * the vcmp here and returning 0 would emit the helper call as well,
     * which is how the first draft of this was wrong. It goes in with
     * the flag-reading path, not before it. */
    default:
        return 0;
    }
}


/* ---- atomics ---------------------------------------------------------
 *
 * ARMv7-M and ARMv8-M Mainline have exclusive loads and stores at one, two
 * and four bytes, and every atomic here is a retry loop around a pair:
 *
 *      dmb
 *   1: ldrex{b,h}  old, [addr]
 *      <new from old>
 *      strex{b,h}  lr, new, [addr]     -- 0 in lr when it took
 *      cmp  lr, #0
 *      bne  1b
 *      dmb
 *
 * which is what GCC and clang emit for a sequentially consistent one on a
 * Cortex-M. The barriers are always full: the IR does not carry the memory
 * order, and the strongest one is right for every weaker request.
 *
 * Five values are live in the loop -- address, operand, old, new and the
 * store's status -- and the backend's scratch set is four, so the status
 * goes in lr: every prologue saves it, and nothing in the loop calls.
 *
 * The byte and halfword forms zero-extend, so a compare-and-swap compares
 * against its expected value zero-extended to the same width, and a signed
 * result is sign-extended on the way out. Eight bytes has no exclusive
 * pair on ARMv7-M and is refused. */
static void set_cc(struct t_fn *F, int dst, int cond);
static void cmse_entry_return(struct t_fn *F);
static void cmse_call(struct t_fn *F, int ncrn);

static void thumb_atomic(struct t_fn *F, const struct ir_ins *i)
{
    struct code *t = F->t;
    int sz = i->size, addr, top, br;
    if (sz != 1 && sz != 2 && sz != 4)
        t_refuse(F->fn, i, t_isa_a32
                 ? "an atomic wider than four bytes (ldrexd/strexd are not "
                   "used yet)"
                 : "an atomic wider than four bytes (ARMv7-M has no "
                   "doubleword exclusive; GCC calls libatomic for these)");
#define LDX(rt) (sz == 4 ? t_ldrex(t, (rt), addr, 0) \
                         : t_ldrexbh(t, (rt), addr, sz))
#define STX(rt) (sz == 4 ? t_strex(t, T_LR, (rt), addr, 0) \
                         : t_strexbh(t, T_LR, (rt), addr, sz))
    addr = rdr(F, i->a, T_ADDR);
    t_barrier(t, T_BAR_DMB);
    if (i->op == IR_XCHG || i->op == IR_XADD || i->op == IR_ARMW) {
        int val = rdr(F, i->b, T_TMP), nw;
        top = t->len;
        LDX(T_ACC);
        if (i->op == IR_XCHG) {
            nw = val;
        } else {
            nw = T_SCR;
            if (i->op == IR_XADD)
                t_alu_reg(t, T_OP_ADD, nw, T_ACC, val, 0);
            else if (i->imm == '&' || i->imm == 'n')
                t_alu_reg(t, T_OP_AND, nw, T_ACC, val, 0);
            else if (i->imm == '|')
                t_alu_reg(t, T_OP_ORR, nw, T_ACC, val, 0);
            else if (i->imm == '^')
                t_alu_reg(t, T_OP_EOR, nw, T_ACC, val, 0);
            else
                t_refuse(F->fn, i, "an atomic read-modify-write of this "
                                   "operation");
            if (i->imm == 'n')
                t_mvn_reg(t, nw, nw, 0);
        }
        STX(nw);
        t_cmp_imm(t, T_LR, 0);
        br = t_bcond16(t, T_NE);
        if (!t_patch_bcond16(t, br, top))
            internal_error("thumb: an atomic's retry loop is out of reach");
    } else {
        /* compare-and-swap: IR_CAS by value, IR_CMPXCHG with the expected
         * value at *b and the value seen written back there */
        int exp = T_TMP, des, fail, done;
        if (i->op == IR_CAS && sz == 4) {
            exp = rdr(F, i->b, T_TMP);
        } else if (i->op == IR_CAS) {
            t_ext(t, T_TMP, rdr(F, i->b, T_TMP), sz, 0);
        } else {
            int p = rdr(F, i->b, T_TMP);
            ldst_must(t, T_TMP, p, 0, sz, 0, 0);    /* zero-extended */
        }
        des = rdr(F, i->c, T_SCR);
        top = t->len;
        LDX(T_ACC);
        t_cmp_reg(t, T_ACC, exp);
        fail = t_bcond16(t, T_NE);
        STX(des);
        t_cmp_imm(t, T_LR, 0);
        br = t_bcond16(t, T_NE);
        done = t_b16(t);
        if (!t_patch_bcond16(t, br, top) ||
            !t_patch_bcond16(t, fail, t->len))
            internal_error("thumb: a compare-and-swap loop is out of reach");
        t_clrex(t);                  /* the failed path holds a reservation */
        if (!t_patch_b16(t, done, t->len))
            internal_error("thumb: a compare-and-swap loop is out of reach");
        if (i->op == IR_CMPXCHG) {
            /* *b = the value seen; the result is whether it matched --
             * set_cc's IT block when it has a low register. */
            int p = rdr(F, i->b, T_SCR);
            ldst_must(t, T_ACC, p, 0, sz, 0, 1);
            t_barrier(t, T_BAR_DMB);
            t_cmp_reg(t, T_ACC, exp);
            set_cc(F, i->dst, T_EQ);
            return;
        }
    }
    t_barrier(t, T_BAR_DMB);
    if (i->dst >= 0) {
        if (sz < 4 && i->sign)
            t_ext(t, T_ACC, T_ACC, sz, 1);
        wr(F, i->dst, T_ACC);
    }
#undef LDX
#undef STX
}

/* ---- one instruction ------------------------------------------------ */


/* dst = the condition's truth, 0 or 1, after a compare that set the flags.
 * Into a low register: `ite cond; mov d, #1; mov d, #0`, six bytes --
 * inside an IT block the 16-bit mov sets no flags. Elsewhere: set r12,
 * skip the clear on the condition, clear -- ten bytes, and no IT. */
static void set_cc(struct t_fn *F, int dst, int cond)
{
    int d = wreg(F, dst, T_ACC);
    /* (ARM state: two conditional movs, into any register) */
    if (d < 8 || t_isa_a32) {
        t_setcc_low(F->t, cond, d);
        wrote(F, dst, d);
        return;
    }
    t_mov_imm(F->t, T_ACC, 1, 0);
    {
        int over = t_bcond16(F->t, cond);
        t_mov_imm(F->t, T_ACC, 0, 0);
        t_patch_bcond16(F->t, over, F->t->len);
    }
    wr(F, dst, T_ACC);
}

/* A select the IT block takes: a 32-bit value, its condition 32 bits. */
static int t_select_it_ok(const struct t_fn *F, const struct ir_ins *i)
{
    return i->op == IR_SELECT && !i->flt && i->w <= 4 && i->size == 4 &&
           i->dst >= 0 && !F->wide[i->dst] && !getenv("EMBCC_T_NOITSEL");
}

/* The select operands an IT block takes as immediates: a temp defined
 * once, by a constant one instruction moves (t_it_imm_ok), read by
 * nothing but the select. clang's `it eq; moveq r6, #0x7e`: the
 * constant costs neither its own instruction nor a register. */
static void t_select_imms(struct t_fn *F)
{
    struct ir_func *fn = F->fn;
    int nv = fn->nvregs, *ndef, *at;
    if (!F->usecnt || !nv)
        return;
    ndef = xcalloc((size_t)nv, sizeof *ndef);
    at = xmalloc((size_t)nv * sizeof *at);
    for (int n = 0; n < fn->nins; n++) {
        int d = ra_ins_def(&fn->ins[n]);
        if (d >= 0 && d < nv) {
            ndef[d]++;
            at[d] = n;
        }
    }
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        if (!t_select_it_ok(F, i))
            continue;
        int ops[2] = { i->b, i->c };
        for (int k = 0; k < 2; k++) {
            int v = ops[k];
            if (v < fn->nvars || v >= nv || v == i->a || ndef[v] != 1 ||
                F->usecnt[v] != 1)
                continue;
            const struct ir_ins *c = &fn->ins[at[v]];
            if (c->op != IR_CONST || c->flt || F->wide[v] ||
                !t_it_imm_ok((long)c->imm))
                continue;
            if (!F->selimm) {
                F->selimm = xcalloc((size_t)nv, 1);
                F->selimm_v = xcalloc((size_t)nv, sizeof *F->selimm_v);
            }
            F->selimm[v] = 1;
            F->selimm_v[v] = (long)c->imm;
        }
    }
    free(ndef);
    free(at);
}

static int t_sel_imm(const struct t_fn *F, int v)
{
    return F->selimm && v >= 0 && F->selimm[v];
}

/* Keep only the callee-saved registers some value still lives in, after
 * values have been taken out of them. */
static void t_drop_unused_saves(struct t_fn *F)
{
    struct ir_func *fn = F->fn;
    int keep = 0;
    for (int k = 0; k < F->nsave; k++) {
        int used = 0;
        for (int v = 0; v < fn->nvregs; v++)
            if (F->loc[v] == F->used_callee[k] ||
                (F->loc[v] >= 0 && F->wide[v] &&
                 F->loc[v] + 1 == F->used_callee[k])) {
                used = 1;
                break;
            }
        if (used) F->used_callee[keep++] = F->used_callee[k];
    }
    F->nsave = keep;
}

/* One of an IT block's moves: from a register, or (rs < 0) the
 * operand's immediate. */
static void t_sel_mov(struct t_fn *F, int d, int rs, int v)
{
    if (rs < 0)
        t_mov_imm_it(F->t, d, F->selimm_v[v]);
    else
        t_mov_reg(F->t, d, rs);
}

/* dst = cond ? b : c, the flags already set and b and c in rb and rc:
 * one IT block -- `ite cond; mov d, b; mov d, c`, or one move when d
 * already holds one of them. A Cortex-M pays one to three cycles for
 * every taken branch, and the if-converted diamonds (a state machine's
 * `st = len ? 2 : 0`, a saturation) were branches. The moves are the
 * 16-bit `mov` (T1), which sets no flags inside or outside an IT block;
 * t_mov_reg emits nothing for a move to itself, so those are never asked
 * of it inside one. Nothing that sets flags may come between the compare
 * and this. Reading a value from its slot sets none today (rd: ldr, an
 * add from sp or the frame base, movw), but nothing promises that, so the
 * callers do not lean on it: IR_SELECT reads b and c before its own
 * compare, and a fused compare hands over only operands already in
 * registers -- the case the fusion is worth having for anyway. */
static void t_select_cc(struct t_fn *F, const struct ir_ins *i, int cond,
                        int rb, int rc)
{
    struct code *t = F->t;
    int d = wreg(F, i->dst, T_ACC);
    if (rb == rc && rb >= 0) {
        t_mov_reg(t, d, rb);
    } else if (d == rb) {
        t_it(t, cond ^ 1, "");
        t_sel_mov(F, d, rc, i->c);
    } else if (d == rc) {
        t_it(t, cond, "");
        t_sel_mov(F, d, rb, i->b);
    } else {
        t_it(t, cond, "e");
        t_sel_mov(F, d, rb, i->b);
        t_sel_mov(F, d, rc, i->c);
    }
    wrote(F, i->dst, d);
}

/* Where an operand of an IT-block select is: its register, or -1 for an
 * immediate (t_select_imms). */
#define SEL_OPR(F, v, scratch) (t_sel_imm((F), (v)) ? -1 : rdr((F), (v), scratch))

static void gen_ins(struct t_fn *F, int n)
{
    struct ir_func *fn = F->fn;
    struct ir_ins *i = &fn->ins[n];
    struct code *t = F->t;

    /* -g: a line-table row wherever the source line changes, as the
     * x86-64 and aarch64 backends record them. t->len is where this
     * instruction's code begins. */
    if (F->want_debug && i->line) {
        struct ir_line *last = fn->nlines ? &fn->lines[fn->nlines - 1]
                                          : (struct ir_line *)0;
        if (last && last->off == t->len) {
            last->line = i->line;
        } else if (!last || last->line != i->line) {
            if (fn->nlines == fn->linecap) {
                fn->linecap = fn->linecap ? fn->linecap * 2 : 8;
                fn->lines = xrealloc(fn->lines, (size_t)fn->linecap *
                                     sizeof *fn->lines);
            }
            fn->lines[fn->nlines].off = t->len;
            fn->lines[fn->nlines].line = i->line;
            fn->nlines++;
        }
    }

    /* Floating point is a CALL on this machine, not an instruction.
     * Only the arithmetic is flagged: the IR already carries a float
     * value as plain bits of its own width, so loads, stores, moves and
     * constants go through the integer paths below untouched. */
    if (i->flt && (i->op == IR_ADD || i->op == IR_SUB || i->op == IR_MUL ||
                   i->op == IR_DIV || i->op == IR_MOD || i->op == IR_NEG ||
                   i->op == IR_CMP || i->op == IR_SQRT)) {
        /* Only the ARITHMETIC. `flt` is set on a return, a move and a
         * call too, and those carry the value as the bits it already
         * is — the integer paths below move exactly the right number of
         * them. */
        const char *name = fp_binop_name(i->op, i->w);
        if (i->w != 4 && i->w != 8)
            t_refuse(fn, i, "a long double (ARMv7-M has no 16-byte float)");
        /* THE FPU, where there is one. FPv4-SP-D16 computes SINGLE
         * precision only, so this is `float` and nothing else -- a
         * double still goes to __adddf3 below, on the same part. On the
         * Cortex-M7's FPv5-D16 it is the double too (fp_on_vfp).
         *
         * The values go through their slots rather than staying in FP
         * registers: there is no floating-point register class on this
         * target yet, so `vldr` both operands, one instruction, `vstr`
         * the result. That is already four to six instructions where the
         * helper call was a dozen plus the call itself, and it is
         * CORRECT before it is fast -- the register class is the next
         * step and does not change what is computed. */
        if (fp_on_vfp(i) && i->op == IR_CMP) {
            int cond = fp_cond_for(i->pred);
            struct ir_ins *nx = n + 1 < fn->nins ? &fn->ins[n + 1]
                                                 : (struct ir_ins *)0;
            fp_vfp_cmp(F, i);
            /* The same fusion as an integer compare: a branch that is the
             * only reader of the 0/1 takes the flags directly. */
            if (nx && (nx->op == IR_BRZ || nx->op == IR_BRNZ) &&
                nx->a == i->dst && nx->w != 8 &&
                F->usecnt && F->usecnt[i->dst] == 1) {
                jump_if(F, nx->op == IR_BRNZ ? cond : (cond ^ 1), nx->label);
                F->skip_next = 1;
                return;
            }
            set_cc(F, i->dst, cond);
            return;
        }
        if (fp_on_vfp(i) && fp_vfp_arith(F, i))
            return;
        if (name) {
            if (i->imm_b)
                t_refuse(fn, i, "a folded floating-point immediate");
            fp_args2(F, i);
            call_helper(F, name);
            fp_result(F, i->dst, i->w);
            return;
        }
        if (i->op == IR_NEG) {
            /* The sign bit, flipped. A call would be correct and this
             * is two instructions -- and unlike a subtraction from zero
             * it is right for -0.0 and for a NaN. */
            if (i->w == 8) {
                rd64(F, i->a, A_LO, A_HI);
                t_mov_imm(t, B_LO, 0x80000000L, 0);
                t_alu_reg(t, T_OP_EOR, A_HI, A_HI, B_LO, 0);
                wr64(F, i->dst, A_LO, A_HI);
            } else {
                rd(F, i->a, T_ACC);
                t_mov_imm(t, T_TMP, 0x80000000L, 0);
                t_alu_reg(t, T_OP_EOR, T_ACC, T_ACC, T_TMP, 0);
                wr(F, i->dst, T_ACC);
            }
            return;
        }
        if (i->op == IR_CMP) {
            int cond;
            /* A branch on the answer and nothing else reading it takes
             * the helper's flags straight, as the integer compare does
             * (IR_CMP below): the helpers answer an unordered pair the
             * way that makes the predicate false, so the inverse
             * condition is exactly "not (a pred b)" and BRZ may use it.
             * The 0/1 never exists -- `ite; movs #1; movs #0; cbz` was
             * eight bytes after every soft-float compare. */
            struct ir_ins *nx = n + 1 < fn->nins ? &fn->ins[n + 1]
                                                 : (struct ir_ins *)0;
            int fuse = nx && (nx->op == IR_BRZ || nx->op == IR_BRNZ) &&
                       nx->a == i->dst && nx->w != 8 && F->usecnt &&
                       F->usecnt[i->dst] == 1;
            fp_args2(F, i);
            call_helper(F, fp_cmp_name(i->pred, i->w));
            t_cmp_imm(t, T_R0, 0);
            cond = cond_for(i->pred, 1);      /* the helper's signed answer */
            if (fuse) {
                jump_if(F, nx->op == IR_BRNZ ? cond : (cond ^ 1), nx->label);
                F->skip_next = 1;
                return;
            }
            set_cc(F, i->dst, cond);
            return;
        }
        if (i->op == IR_SQRT)
            t_refuse(fn, i, "__builtin_sqrt (it is a libm routine here, not "
                            "an instruction)");
        t_refuse(fn, i, "this floating-point operation");
    }

    if (i->w > 8)
        t_refuse(fn, i, "a 128-bit value");
    /* Whether this instruction works on a 64-bit value.
     *
     * NOT `i->w == 8` everywhere: the width field is the OPERATION's,
     * and several instructions do not set it at all. IR_STVAR and
     * IR_STORE carry a `size` and no `w`, so asking `w` about them
     * says four and stores half of a `long long`; IR_RET carries
     * neither and would send one home in r0 alone. The wide map,
     * which is built from each value's defining instruction, is what
     * knows -- so each op asks about the value it actually touches. */
    /* The high word of a 64-bit value, shifted: one register
     * (ra_narrow_hishift). */
    if (i->op == IR_SHR && F->nshr && i->dst >= 0 && F->nshr[i->dst]) {
        int k = (int)i->imm - 32, d, hi, fw = 0;
        struct ir_ins *nx = n + 1 < fn->nins ? &fn->ins[n + 1] : NULL;
        /* ...and when its one reader is the mask after it, a field of
         * that word: `(int)(x >> 52) & 0x7ff`, a double's exponent, is
         * one ubfx (see the 32-bit shift below). */
        if (nx && t_wide_imm() && F->usecnt && F->usecnt[i->dst] == 1 &&
            nx->op == IR_AND && nx->imm_b && nx->a == i->dst && !nx->flt &&
            nx->w == 4 && nx->dst >= 0 && !F->wide[nx->dst] &&
            !in_freg(F, nx->dst) && nx->imm > 0 && nx->imm <= 0xffffffffL &&
            ((nx->imm + 1) & nx->imm) == 0) {
            while (fw < 32 && (nx->imm >> fw & 1))
                fw++;
            if (k + fw > 32)
                fw = 0;
        }
        d = wreg(F, fw ? nx->dst : i->dst, T_ACC);
        if (in_reg(F, i->a)) {
            hi = F->loc[i->a] + 1;             /* the pair's high register */
        } else if (in_freg(F, i->a)) {
            /* A double in a d register: its high word is the high single,
             * one vmov straight into the result -- fdlibm's GET_HIGH_WORD,
             * which otherwise moved both words out to keep one. */
            hi = d;
            t_vmov_core(t, F->floc[i->a] + 1, d, 0);
        } else {
            rd64(F, i->a, T_ACC, T_TMP);
            hi = T_TMP;
        }
        if (fw) {
            t_bfx(t, d, hi, k, fw, 0);
            wrote(F, nx->dst, d);
            F->skip_next = 1;
            return;
        }
        if (k)
            t_shift_imm(t, i->sign ? T_SH_ASR : T_SH_LSR, d, hi, k, 0);
        else if (d != hi)
            t_mov_reg(t, d, hi);
        wrote(F, i->dst, d);
        return;
    }
    {
        int wide = i->w == 8;
        switch (i->op) {
        /* A float value being MOVED is an integer of its own width. */
        case IR_STVAR: wide = i->size == 8 || F->wide[i->a]; break;
        case IR_STORE: wide = i->size == 8 || F->wide[i->b]; break;
        case IR_LDVAR:
        case IR_LOAD:  wide = i->dst >= 0 && F->wide[i->dst]; break;
        /* A copy is as wide as what it copies, and IR_MOV is the one
         * instruction that routinely carries no width at all. */
        case IR_MOV:
        case IR_SELECT:
            /* The same rule as the map's propagation, and it has to BE
             * the same rule: a copy that says four bytes copies four,
             * whatever the width of what it reads. */
            wide = i->w != 4 &&
                   ((i->dst >= 0 && F->wide[i->dst]) ||
                    (i->op == IR_MOV && i->a >= 0 && F->wide[i->a]));
            break;
        /* Refused below by what they are, not as "at 64 bits": the
         * frame address carries w = 8 on every target, and an 8-byte
         * atomic is thumb_atomic's to refuse. */
        case IR_FRAMEADDR:
        case IR_XCHG: case IR_XADD: case IR_ARMW: case IR_CAS:
        case IR_CMPXCHG:
            wide = 0;
            break;
        default: break;
        }
        if (wide && i->op != IR_CMP && i->op != IR_BRZ &&
            i->op != IR_BRNZ && i->op != IR_CALL && i->op != IR_RET &&
            /* The conversions are calls with their own cases, and their
             * operand and result widths differ — gen_ins64 would read
             * the wrong one. */
            i->op != IR_I2F && i->op != IR_F2I && i->op != IR_F2F) {
            if (i->op == IR_DIV || i->op == IR_MOD) {
                /* No instruction divides 64 by 64 here, so it is a call
                 * — lib/rt/int64.c, under libgcc's names so an object
                 * of ours links beside one of theirs. Both operands are
                 * eight bytes, which AAPCS32 puts in r0:r1 and r2:r3,
                 * and the result comes back in r0:r1. */
                if (i->imm_b) {
                    args64x2(F, i->a, -1);
                    operand_b64(F, i, T_R2, T_R3);
                } else {
                    args64x2(F, i->a, i->b);
                }
                call_helper(F, i->op == IR_DIV
                            ? (i->sign ? "__divdi3" : "__udivdi3")
                            : (i->sign ? "__moddi3" : "__umoddi3"));
                wr64(F, i->dst, T_R0, T_R1);
                return;
            }
            if (gen_ins64(F, n))
                return;
            t_refuse(fn, i, "this operation at 64 bits");
        }
        if (wide && (i->op == IR_CMP || i->op == IR_BRZ ||
                     i->op == IR_BRNZ))
            i->w = 8;      /* the cases below read `w` to pick the pair */
    }

    switch (i->op) {
    case IR_LABEL:
        F->label_off[i->label] = t->len;
        F->bc_end = -1;          /* something may branch here */
        F->fl_end = -1;          /* ...with other flags */
        return;
    case IR_JMP:
        /* A jump to the label that follows it is not an instruction.
         * Four bytes each and the IR is full of them, because every
         * `if` without an `else` ends in one. */
        if (n + 1 < fn->nins && fn->ins[n + 1].op == IR_LABEL &&
            fn->ins[n + 1].label == i->label)
            return;
        if (!invert_last_bcond(F, n, i->label))
            jump_to(F, i->label);
        return;
    case IR_CONST: {
        if (t_sel_imm(F, i->dst))
            return;             /* the select moves it (t_select_imms) */
        /* Build the constant in the destination's OWN register when it
         * has one. Going through T_ACC and copying cost two extra
         * instructions on the commonest operation there is, and the
         * copy out of a high scratch cannot use a 16-bit encoding. */
        int d = wreg(F, i->dst, T_ACC);
        t_mov_imm_dead_flags(t, d, (long)i->imm);
        wrote(F, i->dst, d);
        return;
    }
    /* Soft float -- see the 64-bit arm. */
    case IR_BITCAST:
    case IR_MOV: {
        /* Source register to destination register, with no detour. This
         * was `rd(a, ACC); wr(dst, ACC)`, which emitted
         *     mov r12, r2
         *     mov r2, r12
         * for a copy between two allocated registers -- two
         * instructions that together do nothing, on every move in the
         * program. */
        int srcr;
        if (in_freg(F, i->dst)) {        /* into an S register: vmov */
            vfp_load(F, i->a, F->floc[i->dst]);
            return;
        }
        srcr = rdr(F, i->a, T_ACC);
        wr(F, i->dst, srcr);
        return;
    }

    case IR_ADD: case IR_SUB: case IR_MUL:
    case IR_AND: case IR_OR:  case IR_XOR: {
        /* A switch and not a table indexed by (i->op - IR_ADD): IR_DIV
         * and IR_MOD sit between IR_MUL and IR_AND, so a six-entry table
         * turns `and` into `eor` and reads past its end for `or` and
         * `xor`. It compiled, it ran, and `v & 1` came back as v & ~1. */
        /* A product whose one reader is the add or subtract right after
         * it: `mla d, a, b, c` is c + a*b and `mls d, a, b, c` is c - a*b,
         * one instruction for the two, which every multiply-accumulate
         * loop is -- a dot product, a filter tap, a matrix row. Both read
         * all three sources before writing, so d may be any of them.
         * `a*b - c` has no such instruction and stays two. */
        if (i->op == IR_MUL && !i->imm_b && !i->flt && i->w == 4 &&
            i->dst >= 0 && F->usecnt && F->usecnt[i->dst] == 1 &&
            n + 1 < fn->nins && !getenv("EMBCC_NO_MLA")) {
            const struct ir_ins *nx = &fn->ins[n + 1];
            int c = -1;
            if ((nx->op == IR_ADD || nx->op == IR_SUB) && !nx->imm_b &&
                !nx->flt && nx->w == 4 && nx->dst >= 0) {
                if (nx->b == i->dst && nx->a != i->dst)
                    c = nx->a;                  /* c + p, c - p */
                else if (nx->op == IR_ADD && nx->a == i->dst &&
                         nx->b != i->dst)
                    c = nx->b;                  /* p + c */
            }
            if (c >= 0) {
                int ra_ = rdr(F, i->a, T_ACC);
                int rb_ = rdr(F, i->b, T_TMP);
                int rc_ = rdr(F, c, T_ADDR);
                int d = wreg(F, nx->dst, T_ACC);
                if (nx->op == IR_ADD) t_mla(t, d, ra_, rb_, rc_);
                else                  t_mls(t, d, ra_, rb_, rc_);
                wrote(F, nx->dst, d);
                F->skip_next = 1;
                return;
            }
        }
        /* A mask whose only reader is a branch: `tst a, #k; bne` sets the
         * flags from a & k and keeps nothing, where `and r, a, #k; cmp r,
         * #0; bne` was three instructions and a register -- every `if (x
         * & FLAG)` and `(i & 31) == 0`. Copies may come between the two
         * (phi destruction puts the join's there): a register move, a
         * load or a store of one sets no flags, so they are made as
         * usual and the branch takes the flags after them. */
        if (i->op == IR_AND && !i->flt && i->w == 4 && i->dst >= 0 &&
            !F->wide[i->dst] && F->usecnt && F->usecnt[i->dst] == 1 &&
            n + 1 < fn->nins && !F->wide[i->a] &&
            (i->imm_b || !F->wide[i->b]) && !getenv("EMBCC_T_NOTST")) {
            int k = n + 1;
            while (k < fn->nins && k - n <= 4 && fn->ins[k].op == IR_MOV &&
                   !fn->ins[k].flt && fn->ins[k].dst != i->dst &&
                   fn->ins[k].dst >= 0 && fn->ins[k].a >= 0 &&
                   !F->wide[fn->ins[k].dst] && !F->wide[fn->ins[k].a] &&
                   !in_freg(F, fn->ins[k].dst) && !in_freg(F, fn->ins[k].a))
                k++;
            const struct ir_ins *bx = k < fn->nins ? &fn->ins[k] : i;
            if ((bx->op == IR_BRZ || bx->op == IR_BRNZ) &&
                bx->a == i->dst && bx->w == 4) {
                int ra_ = rdr(F, i->a, T_ACC);
                if (!i->imm_b) {
                    t_tst_reg(t, ra_, rdr(F, i->b, T_TMP));
                } else if (!t_tst_imm(t, ra_, i->imm)) {
                    int sb = LO(F, T_TMP);
                    operand_b(F, i, sb);
                    t_tst_reg(t, ra_, sb);
                }
                if (k == n + 1) {
                    jump_if(F, bx->op == IR_BRZ ? T_EQ : T_NE, bx->label);
                    F->skip_next = 1;
                } else {
                    F->tst_br = k + 1;
                }
                return;
            }
        }
        int op = i->op == IR_ADD ? T_OP_ADD
               : i->op == IR_SUB ? T_OP_SUB
               : i->op == IR_AND ? T_OP_AND
               : i->op == IR_OR  ? T_OP_ORR
               : i->op == IR_XOR ? T_OP_EOR
               : 0;                          /* IR_MUL: not an ALU op */
        /* An add of two registers whose only reader is the next access's
         * ADDRESS is that access's register offset: `ldrb rt, [rn, rm]`,
         * two bytes in low registers, where the add and `ldrb rt, [rd]`
         * were two instructions. `p[i]` on bytes and every pointer plus
         * offset is this shape; the shifted form above covers the wider
         * elements. Not when the access carries an immediate offset
         * (ra_fold_memoff's): the register form has no field for it. */
        if (i->op == IR_ADD && !i->imm_b && !i->flt && i->w == 4 &&
            i->dst >= 0 && !F->wide[i->dst] && F->usecnt &&
            F->usecnt[i->dst] == 1 && n + 1 < fn->nins &&
            !F->wide[i->a] && !F->wide[i->b] &&
            !in_freg(F, i->a) && !in_freg(F, i->b)) {
            const struct ir_ins *ax = &fn->ins[n + 1];
            int isld = ax->op == IR_LOAD && ax->a == i->dst &&
                       ax->memoff == 0 && ax->w <= 4 && !ax->flt &&
                       ax->dst >= 0 && !F->wide[ax->dst] &&
                       !in_freg(F, ax->dst);
            int isst = ax->op == IR_STORE && ax->a == i->dst &&
                       ax->memoff == 0 && ax->size <= 4 && !ax->flt &&
                       ax->b >= 0 && ax->b != i->dst &&
                       !F->wide[ax->b] && !in_freg(F, ax->b);
            if ((isld || isst) && !getenv("EMBCC_T_NOREGOFF")) {
                F->lofree = 0;      /* the pool was computed for this
                                     * instruction alone */
                int rn = rdr(F, i->a, T_ADDR);
                int rm = rdr(F, i->b, T_TMP);
                if (isld) {
                    int d = wreg(F, ax->dst, T_ACC);
                    t_ldst_reg(t, d, rn, rm, 0, ax->size, ax->sign, 0);
                    wrote(F, ax->dst, d);
                } else {
                    int v = rdr(F, ax->b, T_ACC);
                    t_ldst_reg(t, v, rn, rm, 0, ax->size, 0, 1);
                }
                F->skip_next = 1;
                return;
            }
        }
        int ra_ = rdr(F, i->a, T_ACC);
        int d = wreg(F, i->dst, T_ACC);
        /* The flags are dead here -- no flag value survives from one IR
         * instruction to the next (t_mov_imm_dead_flags says why) -- so
         * the flag-setting forms are allowed, and for r0-r7 those are the
         * 16-bit ones: `adds r0, #1` is two bytes where `addw` is four. */
        if (i->imm_b && i->op != IR_MUL) {
            int lo = d < 8 && ra_ < 8;
            if ((i->op == IR_ADD || i->op == IR_SUB) && lo &&
                i->imm >= 0 && (i->imm <= 7 || (d == ra_ && i->imm <= 255))) {
                t_alu_imm(t, op, d, ra_, i->imm, 1);
            /* addw/subw reach any 0..4095 where the modified immediate
             * reaches only what it can rotate into place, and almost
             * every constant folded here is a small offset. */
            } else if ((i->op == IR_ADD || i->op == IR_SUB) &&
                i->imm >= -4095 && i->imm <= 4095) {
                /* A negative one is the other operation: x + -5 is a
                 * subw of 5. */
                int add = (i->op == IR_ADD) == (i->imm >= 0);
                long mag = i->imm < 0 ? -i->imm : i->imm;
                if (add) t_addw(t, d, ra_, mag);
                else     t_subw(t, d, ra_, mag);
            } else if (i->op == IR_AND && i->imm > 0 &&
                       ((i->imm + 1) & i->imm) == 0 && !t_imm_ok(i->imm)) {
                /* a mask of the low n bits the modified immediate cannot
                 * hold: the bitfield extract, one instruction */
                int nb = 0;
                while ((1L << nb) - 1 < i->imm) nb++;
                t_bfx(t, d, ra_, 0, nb, 0);
            } else if (!t_alu_imm(t, op, d, ra_, i->imm, 0)) {
                int rb_ = !in_reg(F, i->b) ? LO(F, T_TMP) : F->loc[i->b];
                if (!in_reg(F, i->b)) operand_b(F, i, rb_);
                t_alu_reg(t, op, d, ra_, rb_, 0);
            }
            wrote(F, i->dst, d);
            return;
        }
        if (i->op == IR_MUL && i->imm_b && i->w == 4) {
            /* x * ((2^k +- 1) << j): the shifted-operand add or rsb, then
             * the shift -- `add.w d, a, a, lsl #1` for 3, `rsb d, a, a,
             * lsl #3` for 7, `lsls` after for 6, 10, 12, 20. One or two
             * instructions where the constant's mov and the mul were two,
             * and no smaller. */
            int k, neg, j;
            if (target_mul_shift_add(i->imm, &k, &neg, &j)) {
                t_alu_reg_shift(t, neg ? T_OP_RSB : T_OP_ADD, d, ra_, ra_,
                                T_SH_LSL, k, 0);
                if (j)
                    t_shift_imm(t, T_SH_LSL, d, d, j, 1);   /* flags dead */
                wrote(F, i->dst, d);
                return;
            }
        }
        {
            /* The second operand is pinned to a scratch unless it is
             * already in a register, so a load of it cannot land in the
             * destination before the operation reads it. */
            int bin = !i->imm_b && in_reg(F, i->b);
            int rb_ = bin ? F->loc[i->b] : LO(F, T_TMP);
            if (!bin) operand_b(F, i, rb_);
            if (i->op == IR_MUL)
                t_mul(t, d, ra_, rb_);
            else if (d == rb_ && ra_ != rb_ && i->op != IR_SUB)
                /* The 16-bit two-operand forms need the destination to
                 * be the FIRST operand; these commute. */
                t_alu_reg(t, op, d, rb_, ra_, 1);
            else
                t_alu_reg(t, op, d, ra_, rb_, 1);   /* flags dead: above */
        }
        wrote(F, i->dst, d);
        return;
    }
    case IR_MULH: {
        /* The high word of a 32 x 32 product, which division by a
         * constant multiplies by: the long multiply with its low word
         * thrown away -- or smmul in ARM state, where every ARMv7-A has
         * it. Not on ARMv7E-M, though the DSP set has it there too: the
         * Cortex-M levels select the same instructions for v7-M and
         * v7E-M (target_thumb_em), and a v7E-M image is run on a
         * Cortex-M3 by the firmware tests, where smmul is undefined. */
        int sa = rdr(F, i->a, T_ACC), sb = rdr(F, i->b, T_TMP);
        int d = wreg(F, i->dst, T_ACC);
        if (i->sign && t_isa_a32 && !getenv("EMBCC_NO_SMMUL")) {
            t_smmul(t, d, sa, sb);
        } else {
            int lo = d != T_ACC ? LO(F, T_ACC) : LO(F, T_ADDR);
            t_mull(t, lo, d, sa, sb, i->sign);
        }
        wrote(F, i->dst, d);
        return;
    }
    case IR_DIV: case IR_MOD: {
        /* ARMv7-A in ARM state has no sdiv/udiv (an A7 or A15 has, base
         * v7-A does not): the RTABI's routines, as clang calls them --
         * the quotient in r0, and from the divmod pair the remainder in
         * r1. lib/rt provides them for this target. */
        if (t_isa_a32) {
            if (i->imm_b) {
                rd(F, i->a, T_R0);
                t_mov_imm(t, T_R1, (long)i->imm, 0);
            } else {
                fp_args2(F, i);
            }
            call_helper(F, i->op == IR_DIV
                        ? (i->sign ? "__aeabi_idiv" : "__aeabi_uidiv")
                        : (i->sign ? "__aeabi_idivmod" : "__aeabi_uidivmod"));
            wr(F, i->dst, i->op == IR_DIV ? T_R0 : T_R1);
            return;
        }
        /* The operands where they live and the result where it goes:
         * sdiv, udiv and mls take any registers and read all of them
         * before writing. Copying both into r12 and r11 first and the
         * quotient through r10 was three moves a division, and r10 and
         * r11 are callee-saved -- pushed for it. */
        int sa = rdr(F, i->a, T_ACC), sb, d;
        if (i->imm_b) {
            sb = LO(F, T_TMP);
            t_mov_imm(t, sb, (long)i->imm, 0);
        } else {
            sb = rdr(F, i->b, T_TMP);
        }
        d = wreg(F, i->dst, T_ACC);
        if (i->op == IR_DIV) {
            t_div(t, d, sa, sb, i->sign);
        } else {
            /* There is no remainder instruction: r = a - (a / b) * b,
             * which `mls` does in one. The quotient in d itself when d
             * is neither operand, else in r12 unless an operand is. */
            int q = d != sa && d != sb ? d
                  : sa != T_ACC && sb != T_ACC ? T_ACC : T_ADDR;
            t_div(t, q, sa, sb, i->sign);
            t_mls(t, d, q, sb, sa);
        }
        wrote(F, i->dst, d);
        return;
    }
    case IR_SHL: case IR_SHR: {
        int sh = i->op == IR_SHL ? T_SH_LSL : i->sign ? T_SH_ASR : T_SH_LSR;
        /* `(x >> s) & (2^k - 1)` with s + k <= 32 is the bitfield
         * x[s, s+k): one ubfx where the shift and the mask were two. The
         * bits taken are x's own, so the shift's kind does not matter.
         * Every exponent field in soft-float code is this. */
        if (i->op == IR_SHR && i->imm_b && i->imm >= 1 && i->imm <= 31 &&
            i->w == 4 && i->dst >= 0 && !F->wide[i->dst] && F->usecnt &&
            F->usecnt[i->dst] == 1 && n + 1 < fn->nins && t_wide_imm()) {
            struct ir_ins *nx = &fn->ins[n + 1];
            long m = nx->imm;
            if (nx->op == IR_AND && nx->imm_b && nx->a == i->dst && !nx->flt &&
                nx->w == 4 && nx->dst >= 0 && !F->wide[nx->dst] &&
                !in_freg(F, nx->dst) && m > 0 && m <= 0xffffffffL &&
                ((m + 1) & m) == 0) {
                int k = 0;
                while (k < 32 && (m >> k & 1))
                    k++;
                if (i->imm + k <= 32) {
                    int ra_ = rdr(F, i->a, T_ACC);
                    int d = wreg(F, nx->dst, T_ACC);
                    t_bfx(t, d, ra_, (int)i->imm, k, 0);
                    wrote(F, nx->dst, d);
                    F->skip_next = 1;
                    return;
                }
            }
        }
        /* A shift by a constant whose only reader is the next instruction's
         * operand is that instruction's SHIFTED OPERAND: `add.w rd, rn, rm,
         * lsl #k` is one instruction where the shift and the add were two
         * -- the same size as the two 16-bit ones, and half the count. And
         * when that add is itself only the address of the access after it,
         * `ldr rt, [rn, rm, lsl #k]` does all three. aarch64 has had both
         * since its own array indexing came out three instructions long. */
        if (i->imm_b && i->imm >= 1 && i->imm <= 31 && i->w == 4 &&
            i->dst >= 0 && !F->wide[i->dst] && F->usecnt &&
            F->usecnt[i->dst] == 1 && n + 1 < fn->nins) {
            struct ir_ins *nx = &fn->ins[n + 1];
            int comm = nx->op == IR_ADD || nx->op == IR_AND ||
                       nx->op == IR_OR || nx->op == IR_XOR;
            int op = nx->op == IR_ADD ? T_OP_ADD : nx->op == IR_SUB ? T_OP_SUB
                   : nx->op == IR_AND ? T_OP_AND : nx->op == IR_OR ? T_OP_ORR
                   : nx->op == IR_XOR ? T_OP_EOR : -1;
            int other = -1;
            if (op >= 0 && !nx->flt && !nx->imm_b && nx->w == 4 &&
                nx->dst >= 0 && !F->wide[nx->dst]) {
                if (nx->b == i->dst && nx->a != i->dst)
                    other = nx->a;
                else if (nx->a == i->dst && nx->b != i->dst && comm)
                    other = nx->b;
                else if (nx->a == i->dst && nx->b != i->dst && op == T_OP_SUB)
                    other = nx->b, op = T_OP_RSB;   /* (a << k) - b */
            }
            if (other >= 0 && !F->wide[other] && !in_freg(F, other)) {
                F->lofree = 0;      /* the pool was computed for this
                                     * instruction alone */
                if (op == T_OP_ADD && sh == T_SH_LSL && i->imm <= 3 &&
                    F->usecnt[nx->dst] == 1 && n + 2 < fn->nins) {
                    struct ir_ins *ax = &fn->ins[n + 2];
                    int isld = ax->op == IR_LOAD && ax->a == nx->dst &&
                               ax->memoff == 0 && ax->w <= 4 &&
                               ax->dst >= 0 && !F->wide[ax->dst] &&
                               !in_freg(F, ax->dst);
                    int isst = ax->op == IR_STORE && ax->a == nx->dst &&
                               ax->memoff == 0 && ax->size <= 4 &&
                               ax->b >= 0 && ax->b != nx->dst &&
                               !F->wide[ax->b] && !in_freg(F, ax->b);
                    if ((isld || isst) &&
                        t_ldst_reg_ok((int)i->imm, ax->size,
                                      isld && ax->sign, isst)) {
                        int rm = rdr(F, i->a, T_TMP);
                        int rn = rdr(F, other, T_ADDR);
                        if (isld) {
                            int d = wreg(F, ax->dst, T_ACC);
                            t_ldst_reg(t, d, rn, rm, (int)i->imm, ax->size,
                                       ax->sign, 0);
                            wrote(F, ax->dst, d);
                        } else {
                            int v = rdr(F, ax->b, T_ACC);
                            t_ldst_reg(t, v, rn, rm, (int)i->imm, ax->size,
                                       0, 1);
                        }
                        F->skip_next = 2;
                        return;
                    }
                }
                {
                    int rm = rdr(F, i->a, T_TMP);
                    int rn = rdr(F, other, T_ACC);
                    int d = wreg(F, nx->dst, T_ACC);
                    t_alu_reg_shift(t, op, d, rn, rm, sh, (int)i->imm, 0);
                    wrote(F, nx->dst, d);
                    F->skip_next = 1;
                    return;
                }
            }
        }
        int sa = rdr(F, i->a, T_ACC);
        int d = wreg(F, i->dst, T_ACC);
        if (i->imm_b && i->imm >= 0 && i->imm < 32) {
            t_shift_imm(t, sh, d, sa, (int)i->imm, 1);   /* flags dead */
        } else {
            int sb = LO(F, T_TMP);
            operand_b(F, i, sb);
            t_shift_reg(t, sh, d, sa, sb, 1);
        }
        wrote(F, i->dst, d);
        return;
    }
    case IR_NEG: {
        int sa = rdr(F, i->a, T_ACC);
        int d = wreg(F, i->dst, T_ACC);
        t_alu_imm(t, T_OP_RSB, d, sa, 0, 0);
        wrote(F, i->dst, d);
        return;
    }
    case IR_BNOT: {
        int sa = rdr(F, i->a, T_ACC);
        int d = wreg(F, i->dst, T_ACC);
        t_mvn_reg(t, d, sa, 0);
        wrote(F, i->dst, d);
        return;
    }
    case IR_BSWAP: {
        /* One `rev`; a halfword is `rev16` and the zero-extension the
         * result's type wants. irgen splits a 64-bit swap into two of
         * these. It was shifts and masks: 24 bytes for a bswap32 where
         * clang's is 4. */
        int sa = rdr(F, i->a, T_ACC);
        int d = wreg(F, i->dst, T_ACC);
        if (i->size == 2) {
            t_rev16(t, d, sa);
            t_ext(t, d, d, 2, 0);
        } else {
            t_rev(t, d, sa);
        }
        wrote(F, i->dst, d);
        return;
    }

    case IR_CMP: {
        int cond = cond_for(i->pred, i->sign);
        /* Does the NEXT instruction branch on this result, and does
         * nothing else read it? Then the 0/1 never has to exist.
         *
         * IR_BRNZ takes the branch when the predicate HELD, so it is
         * the condition itself; IR_BRZ when it failed, which is the
         * inverse -- and an ARM condition inverts by flipping its low
         * bit (EQ/NE, CS/CC, ...), which is why this is `^ 1` and not
         * a table. */
        struct ir_ins *nx = n + 1 < fn->nins ? &fn->ins[n + 1] : (struct ir_ins *)0;
        int fuse = nx && (nx->op == IR_BRZ || nx->op == IR_BRNZ) &&
                   nx->a == i->dst && nx->w != 8 &&
                   F->usecnt && F->usecnt[i->dst] == 1;
        /* ...or selects on it: the select reads the compare's flags */
        int selfuse = nx && t_select_it_ok(F, nx) && nx->a == i->dst &&
                      (in_reg(F, nx->b) || t_sel_imm(F, nx->b)) &&
                      (in_reg(F, nx->c) || t_sel_imm(F, nx->c)) &&
                      F->usecnt && F->usecnt[i->dst] == 1;
        if (i->w == 8) {
            cond = cmp64(F, i, i->pred, i->sign);
            if (fuse) {
                jump_if(F, nx->op == IR_BRNZ ? cond : (cond ^ 1), nx->label);
                F->skip_next = 1;
                return;
            }
            if (selfuse) {
                t_select_cc(F, nx, cond, SEL_OPR(F, nx->b, T_TMP),
                            SEL_OPR(F, nx->c, T_ADDR));
                F->skip_next = 1;
                return;
            }
            set_cc(F, i->dst, cond);
            return;
        }
        if (fuse && i->imm_b && in_reg(F, i->a) && F->fl_end == t->len &&
            F->fl_reg == F->loc[i->a] && F->fl_imm == i->imm) {
            /* the flags of the compare just made (fl_end) */
        } else {
        /* The comparison reads its left operand where it already is;
         * only the 0/1 result needs a register of its own. */
        int sa = rdr(F, i->a, T_ACC);
        if (i->imm_b && ((i->imm >= 0 && i->imm <= 255) || t_imm_ok(i->imm))) {
            t_cmp_imm(t, sa, i->imm);
        } else if (!i->imm_b) {
            /* ...and its right one too: `cmp r2, r4`, not a copy of r4
             * into r11 first. */
            t_cmp_reg(t, sa, rdr(F, i->b, T_TMP));
        } else {
            int sb = LO(F, T_TMP);
            operand_b(F, i, sb);
            t_cmp_reg(t, sa, sb);
        }
        }
        if (selfuse) {
            t_select_cc(F, nx, cond, SEL_OPR(F, nx->b, T_TMP),
                        SEL_OPR(F, nx->c, T_ADDR));
            F->skip_next = 1;
            return;
        }
        if (fuse) {
            jump_if(F, nx->op == IR_BRNZ ? cond : (cond ^ 1), nx->label);
            F->skip_next = 1;
            F->fl_end = -1;
            if (i->imm_b && in_reg(F, i->a)) {
                F->fl_end = t->len;
                F->fl_reg = F->loc[i->a];
                F->fl_imm = i->imm;
            }
            return;
        }
        /* 0 or 1, without an IT block: set it, then jump over the
         * clear. Two instructions either way, and no flag-liveness
         * question to get wrong. */
        set_cc(F, i->dst, cond);
        return;
    }
    case IR_SELECT: {
        /* dst = a ? b : c, 32 bits: `cmp a, #0` and an IT block
         * (t_select_cc), with b and c read before the compare. */
        if (t_select_it_ok(F, i)) {
            int rb = SEL_OPR(F, i->b, T_TMP);
            int rc = SEL_OPR(F, i->c, T_ADDR);
            int ra = rdr(F, i->a, T_ACC);
            t_cmp_imm(t, ra, 0);
            t_select_cc(F, i, T_NE, rb, rc);
            return;
        }
        /* Otherwise two loads and a branch over one of them. The
         * condition is tested at its own width, `size`. */
        if (i->size == 8 && t_wide_imm()) {
            int al, ah;
            src64(F, i->a, A_LO, R_TMP, &al, &ah);
            t_alu_reg(t, T_OP_ORR, T_ACC, al, ah, 1);
        } else if (i->size == 8) {
            rd64(F, i->a, A_LO, A_HI);
            t_alu_reg(t, T_OP_ORR, A_LO, A_LO, A_HI, 0);
            t_cmp_imm(t, A_LO, 0);
        } else {
            rd(F, i->a, T_ACC);
            t_cmp_imm(t, T_ACC, 0);
        }
        {
            int take_c = t_bcond(t, T_EQ);
            rd(F, i->b, T_ACC);
            {
                int done = t_b(t);
                t_patch_bcond(t, take_c, t->len);
                rd(F, i->c, T_ACC);
                t_patch_b(t, done, t->len);
            }
        }
        wr(F, i->dst, T_ACC);
        return;
    }
    case IR_BRZ: case IR_BRNZ:
        if (F->tst_br == n + 1) {       /* the mask's tst, made above */
            F->tst_br = 0;
            jump_if(F, i->op == IR_BRZ ? T_EQ : T_NE, i->label);
            return;
        }
        if (i->w == 8 && t_wide_imm()) {
            /* where the halves are, and one orrs: Z is "both zero" */
            int al, ah;
            src64(F, i->a, A_LO, R_TMP, &al, &ah);
            t_alu_reg(t, T_OP_ORR, T_ACC, al, ah, 1);
        } else if (i->w == 8) {
            rd64(F, i->a, A_LO, A_HI);
            t_alu_reg(t, T_OP_ORR, A_LO, A_LO, A_HI, 0);
            t_cmp_imm(t, A_LO, 0);
        } else {
            /* In place: a low register takes the 16-bit `cmp rN, #0`,
             * where a copy into r12 took a mov and a cmp.w -- and when the
             * first pass measured the target forward within 126 bytes,
             * `cbz`/`cbnz`: the compare and the branch in two bytes. */
            int r = rdr(F, i->a, T_ACC), k = F->nfix, cmp_at;
            if (r < 8 && F->shortb && k < F->nshortb && F->shortb[k] == 2) {
                int at = t_cbz(t, i->op == IR_BRNZ, r);
                want_label(F, at, i->label, T_CBZ + (i->op == IR_BRNZ));
                F->bc_end = -1;        /* no condition to invert */
                return;
            }
            cmp_at = t->len;
            t_cmp_imm(t, r, 0);
            jump_if(F, i->op == IR_BRZ ? T_EQ : T_NE, i->label);
            if (r < 8)
                F->fix[F->nfix - 1].cz_at = cmp_at;
            return;
        }
        jump_if(F, i->op == IR_BRZ ? T_EQ : T_NE, i->label);
        return;

    /* A LOCAL may live in a register too, and these are the only two
     * places that name its slot directly -- so they are the two that
     * have to ask. Reading the slot of an allocated local reads whatever
     * the frame happened to hold. */
    case IR_LDVAR: {
        int d;
        if (i->w > 4) t_refuse(fn, i, "a 64-bit local");
        if (in_freg(F, i->a) || in_freg(F, i->dst)) {
            /* A float local in s16-s31, or read into one: the value is
             * all four bytes of it, so this is a move. */
            if (i->size != 4)
                internal_error("thumb: %s: a %d-byte read of a float in "
                               "an S register", fn->name, i->size);
            if (in_freg(F, i->a) && in_freg(F, i->dst))
                t_vmov_reg(t, F->floc[i->dst], F->floc[i->a], 0);
            else if (in_freg(F, i->a))
                wr(F, i->dst, rdr(F, i->a, T_ACC));
            else
                vfp_store(F, i->dst, (vfp_load(F, i->a, T_FS0), T_FS0));
            return;
        }
        d = wreg(F, i->dst, T_ACC);
        if (in_reg(F, i->a)) {
            if (t_ldvar_plain(i->size, i->sign, i->w)) {
                if (d != F->loc[i->a]) t_mov_reg(t, d, F->loc[i->a]);
            } else {
                t_ext(t, d, F->loc[i->a], i->size, i->sign);
            }
        } else if (!t_ldst_imm(t, d, F->fb, slot_of(F, i->a), i->size,
                               i->sign, 0)) {
            fb_addr(F, T_ADDR, F->slot[i->a]);
            ldst_must(t, d, T_ADDR, 0, i->size, i->sign, 0);
        }
        wrote(F, i->dst, d);
        return;
    }
    case IR_STVAR: {
        int src;
        if (i->w > 4) t_refuse(fn, i, "a 64-bit local");
        if (in_freg(F, i->dst)) {
            if (i->size != 4)
                internal_error("thumb: %s: a %d-byte store to a float in "
                               "an S register", fn->name, i->size);
            vfp_load(F, i->a, F->floc[i->dst]);
            return;
        }
        src = rdr(F, i->a, T_ACC);
        if (in_reg(F, i->dst)) {
            /* A narrowing store extends, because the register now holds
             * the whole local and only its low `size` bytes are the
             * value. Zero-extending is what t_ldvar_plain is written to
             * match: it calls a narrow read an operation, so every read
             * extends for itself and is right either way. */
            if (i->size >= 4) {
                if (F->loc[i->dst] != src) t_mov_reg(t, F->loc[i->dst], src);
            } else {
                t_ext(t, F->loc[i->dst], src, i->size, 0);
            }
        } else if (!t_ldst_imm(t, src, F->fb, slot_of(F, i->dst), i->size, 0, 1)) {
            fb_addr(F, T_ADDR, F->slot[i->dst]);
            ldst_must(t, src, T_ADDR, 0, i->size, 0, 1);
        }
        return;
    }
    case IR_LOAD: {
        /* `ldr rd, [rn]` with rd == rn is legal, so the destination
         * may share the address's register; nothing has to be kept
         * apart here. An address that is a local's is the frame base
         * plus an offset, used as that. */
        long fo = 0;
        int fa = faddr(F, i->a, &fo);
        int an = fa ? F->fb : rdr(F, i->a, T_ADDR);
        int d;
        long off = i->memoff + fo;
        /* A float with an S-register home, read with vldr -- only where
         * the address is KNOWN aligned: vldr faults on a misaligned one
         * where ldr would not, and a packed struct's float member is
         * exactly that. */
        if (in_freg(F, i->dst) && i->size == 4 && i->natural) {
            if (off) {
                base_plus(F, T_ADDR, an, off);
                an = T_ADDR;
            }
            t_vldst(t, F->floc[i->dst], an, 0, 0, 0);
            return;
        }
        d = wreg(F, i->dst, T_ACC);
        if (i->w > 4) t_refuse(fn, i, "a 64-bit load");
        if (!t_ldst_imm(t, d, an, off, i->size, i->sign, 0)) {
            base_plus(F, T_ADDR, an, off);
            ldst_must(t, d, T_ADDR, 0, i->size, i->sign, 0);
        }
        wrote(F, i->dst, d);
        return;
    }
    case IR_STORE: {
        int an, vr;
        long off;
        if (i->w > 4) t_refuse(fn, i, "a 64-bit store");
        {
            long fo = 0;
            int fa = faddr(F, i->a, &fo);
            an = fa ? F->fb : rdr(F, i->a, T_ADDR);
            off = i->memoff + fo;
        }
        if (in_freg(F, i->b) && i->size == 4 && i->natural) {
            if (off) {
                base_plus(F, T_ADDR, an, off);
                an = T_ADDR;
            }
            t_vldst(t, F->floc[i->b], an, 0, 0, 1);   /* see IR_LOAD */
            return;
        }
        /* The value must not land in the register the address is in
         * when that register is the scratch -- rdr would overwrite it. */
        vr = rdr(F, i->b, an == T_ACC ? T_TMP : T_ACC);
        if (!t_ldst_imm(t, vr, an, off, i->size, 0, 1)) {
            base_plus(F, T_ADDR, an, off);
            ldst_must(t, vr, T_ADDR, 0, i->size, 0, 1);
        }
        return;
    }
    case IR_EXT: {
        int sa = rdr(F, i->a, T_ACC);
        int d = wreg(F, i->dst, T_ACC);
        if (i->size < 4)
            t_ext(t, d, sa, i->size, i->sign);
        else if (d != sa)
            t_mov_reg(t, d, sa);
        wrote(F, i->dst, d);
        return;
    }

    case IR_ADDR: {
        long fo;
        if (faddr(F, i->dst, &fo))
            return;                    /* recomputed where it is read */
        int d = wreg(F, i->dst, T_ACC);
        addr_of_slot(F, i->a, d);
        wrote(F, i->dst, d);
        return;
    }
    case IR_STRADDR: {
        int d = wreg(F, i->dst, T_ACC);   /* straight into its home */
        note_str(F->st, t_mov_addr(t, d, 0), i->label, RK_THM_MOVW);
        note_str(F->st, t->len - 4, i->label, RK_THM_MOVT);
        wrote(F, i->dst, d);
        return;
    }
    case IR_GADDR: {
        int d = wreg(F, i->dst, T_ACC);   /* straight into its home */
        note_glob(F->st, t_mov_addr(t, d, 0), i->glob, RK_THM_MOVW);
        note_glob(F->st, t->len - 4, i->glob, RK_THM_MOVT);
        wrote(F, i->dst, d);
        return;
    }
    case IR_FADDR: {
        int d = wreg(F, i->dst, T_ACC);   /* straight into its home */
        note_fn(F->st, t_mov_addr(t, d, 0), i->callee, RK_THM_MOVW);
        note_fn(F->st, t->len - 4, i->callee, RK_THM_MOVT);
        wrote(F, i->dst, d);
        return;
    }

    case IR_MEMCPY: case IR_MEMZERO: {
        /* Straight-line up to t_block_straight bytes: each side through
         * the frame base when it is a local's address (no register
         * for it at all), the words through a free low register (the
         * two-byte ldr/str), and eight bytes at a time with ldrd/strd
         * where both sides are frame slots at word offsets -- which
         * are aligned, as ldrd needs. Else the word loop
         * (t_copy_block). */
        int copy = i->op == IR_MEMCPY;
        long doff = 0, soff = 0;
        int dfa = faddr(F, i->a, &doff);
        int sfa = copy && faddr(F, i->b, &soff);
        long size = i->size, k = 0;
        int reach = size <= t_block_straight() &&
                    (!dfa || (doff >= 0 && doff + size <= 1020)) &&
                    (!sfa || (soff >= 0 && soff + size <= 1020));
        if (!reach) {
            if (dfa) fb_addr(F, T_ADDR, doff); else rd(F, i->a, T_ADDR);
            if (copy) {
                if (sfa) fb_addr(F, T_TMP, soff); else rd(F, i->b, T_TMP);
            }
            t_copy_block(t, T_ADDR, T_TMP, copy, size);
            return;
        }
        {
            int dst = dfa ? F->fb : rdr(F, i->a, T_ADDR);
            int src = !copy ? -1 : sfa ? F->fb : rdr(F, i->b, T_TMP);
            int d0 = LO(F, T_ACC), d1 = -1;
            /* (not in ARM state, where ldrd takes an even register and
             * the next, and these two are whatever LO answers) */
            int pairs = dfa && (sfa || !copy) && doff % 4 == 0 &&
                        (!copy || soff % 4 == 0) && size >= 8 &&
                        !t_isa_a32;
            if (pairs)
                d1 = LO(F, T_SCR);
            if (!copy) {
                t_mov_imm_dead_flags(t, d0, 0);
                if (pairs)
                    t_mov_imm_dead_flags(t, d1, 0);
            }
            for (; pairs && k + 8 <= size; k += 8) {
                if (copy && !t_ldst_pair(t, d0, d1, src, soff + k, 0))
                    internal_error("thumb: an ldrd that reaches did not");
                if (!t_ldst_pair(t, d0, d1, dst, doff + k, 1))
                    internal_error("thumb: an strd that reaches did not");
            }
            for (; k + 4 <= size; k += 4) {
                if (copy) ldst_must(t, d0, src, soff + k, 4, 0, 0);
                ldst_must(t, d0, dst, doff + k, 4, 0, 1);
            }
            for (; k < size; k++) {
                if (copy) ldst_must(t, d0, src, soff + k, 1, 0, 0);
                ldst_must(t, d0, dst, doff + k, 1, 0, 1);
            }
        }
        return;
    }

    case IR_CALL: {
        /* AAPCS32, the scalar half: r0-r3 in order, then four-byte stack
         * slots from sp. An eight-byte argument would round the register
         * number up to even and take two — refused above with everything
         * else 64-bit, so the placement here stays the simple one. */
        long sret = call_sret_bytes(i);
        int resz, rvfp = call_ret_vfp(i, &resz);
        /* The STACK words first, then the registers: writing a stack
         * argument needs a scratch, and by the time r0-r3 are loaded
         * there is none left that is not already an argument. */
        struct argplace pl[MAX_PARAMS];
        struct abi_walk w;
        walk_init(&w, sret != 0, i->call_varargs, i->call_pcs);
        /* Under the base standard a struct of floats is a composite like
         * any other; under the hard-float one it is a candidate for the
         * VFP registers. place_one knows which convention is in force. */
        for (int k = 0; k < i->nargs; k++)
            place_one(&w, &i->argv[k], &pl[k]);
        /* The stack words go through low registers where the call has
         * any free (lo_free_at): `ldr r2, [sp, #n]; str r2, [sp]` is
         * four bytes where the same through r12 is eight, and a value
         * already in a register is stored from it -- it was moved to
         * r12 first, six bytes for every stack argument. Nothing below
         * writes r0-r3 until the stack words are all down. */
        unsigned cfree = lo_free_at(F, n);
        int cs0 = -1, cs1 = -1;
        for (int r = 0; r < 8; r++)
            if (cfree >> r & 1) {
                if (cs0 < 0) cs0 = r;
                else if (cs1 < 0) cs1 = r;
            }
        for (int k = 0; k < i->nargs; k++) {
            struct ir_arg *a = &i->argv[k];
            if (!pl[k].nstk)
                continue;
            /* A composite's vreg holds its ADDRESS; a scalar's holds
             * the value, and a scalar never splits. */
            if (a->is_struct) {
                /* A local's address is the frame base and an offset:
                 * no register for it at all. */
                long fo = 0;
                int fa = faddr(F, a->vreg, &fo);
                int base = fa ? F->fb : cs1 >= 0 ? cs1 : T_ADDR;
                int dr = cs0 >= 0 ? cs0 : T_ACC;
                if (!fa)
                    rd(F, a->vreg, base);
                for (int q = 0; q < pl[k].nstk; q++) {
                    long off = (long)(pl[k].nreg + q) * 4;
                    int last = off + 4 > a->size;
                    /* The tail of an odd-sized struct is copied byte by
                     * byte: reading a whole word past the end of the
                     * object would be a load nothing put there. */
                    if (last && (a->size & 3)) {
                        for (long b = off; b < a->size; b++) {
                            if (fa) fb_ld(F, dr, fo + b, 1, 0);
                            else ldst_must(t, dr, base, b, 1, 0, 0);
                            /* the outgoing area is at the live sp, which
                             * after a VLA is not the frame base */
                            ldst_must(t, dr, T_SP,
                                       pl[k].stk + (long)q * 4 + (b - off),
                                       1, 0, 1);
                        }
                    } else {
                        if (fa) fb_ld(F, dr, fo + off, 4, 0);
                        else ldst_must(t, dr, base, off, 4, 0, 0);
                        ldst_must(t, dr, T_SP, pl[k].stk + (long)q * 4,
                                   4, 0, 1);
                    }
                }
            } else if (a->size > 4) {
                if (in_reg(F, a->vreg)) {
                    int r = F->loc[a->vreg];
                    if (!t_ldst_pair(t, r, r + 1, T_SP, pl[k].stk, 1)) {
                        ldst_must(t, r, T_SP, pl[k].stk, 4, 0, 1);
                        ldst_must(t, r + 1, T_SP, pl[k].stk + 4, 4, 0, 1);
                    }
                } else {
                    int lo = cs0 >= 0 && cs1 >= 0 ? cs0 : T_ACC;
                    int hi = cs0 >= 0 && cs1 >= 0 ? cs1 : T_TMP;
                    rd64(F, a->vreg, lo, hi);
                    ldst_must(t, lo, T_SP, pl[k].stk, 4, 0, 1);
                    ldst_must(t, hi, T_SP, pl[k].stk + 4, 4, 0, 1);
                }
            } else {
                int r = in_reg(F, a->vreg) ? F->loc[a->vreg]
                                           : cs0 >= 0 ? cs0 : T_ACC;
                rd(F, a->vreg, r);
                ldst_must(t, r, T_SP, pl[k].stk, 4, 0, 1);
            }
        }
        /* The VFP arguments next, while every vreg is still where the
         * allocator put it: the core moves below overwrite r0-r3, which
         * may be exactly where a float argument lives. Nothing after
         * this touches s0-s15 before the call. */
        for (int k = 0; k < i->nargs; k++) {
            struct ir_arg *a = &i->argv[k];
            if (!pl[k].nvfp)
                continue;
            if (a->is_struct) {
                rd(F, a->vreg, T_ADDR);
                vfp_aggregate(F, T_ADDR, pl[k].vfp,
                              pl[k].vdbl ? pl[k].nvfp / 2 : pl[k].nvfp,
                              pl[k].vdbl, 0);
            } else if (pl[k].vdbl) {
                vfp_load_d(F, a->vreg, pl[k].vfp / 2);
            } else {
                vfp_load(F, a->vreg, pl[k].vfp);
            }
        }
        /* The register-resident scalars move in PARALLEL. Loading them
         * one at a time would overwrite a register another argument is
         * still to be read from -- `g(a+1, b+2)` with the sum of a in
         * r1 and of b in r0 is enough. This is the same shape as the
         * soft-float helper setup above and the prologue's parameter
         * placement, and T_SCR breaks a cycle because it is reserved
         * scratch and not in the allocator's pool.
         *
         * Only scalars appear here: the allocator keeps a struct
         * argument and an indirect call's target in memory whatever
         * this backend can do, and a 64-bit value is never in a
         * register on this target at all. */
        {
            int pd[8], ps[8], npm = 0;
            for (int k = 0; k < i->nargs && npm < 8; k++) {
                struct ir_arg *a = &i->argv[k];
                if (!pl[k].nreg || a->is_struct || a->size > 8)
                    continue;
                if (!in_reg(F, a->vreg))
                    continue;
                /* A 64-bit value in a pair is two edges of the move. */
                for (int q = 0; q < (a->size > 4 ? 2 : 1) && npm < 8; q++) {
                    pd[npm] = pl[k].reg + q;
                    ps[npm] = F->loc[a->vreg] + q;
                    npm++;
                }
            }
            if (npm) {
                int od[16], os[16];
                int m = ra_parallel_move(pd, ps, npm, R_SCR, od, os,
                                         (int)(sizeof od / sizeof od[0]));
                if (m < 0)
                    internal_error("thumb: a call's argument setup is not "
                                   "a well-formed move");
                for (int k = 0; k < m; k++)
                    t_mov_reg(t, pm_reg(od[k]), pm_reg(os[k]));
            }
        }
        for (int k = 0; k < i->nargs; k++) {
            struct ir_arg *a = &i->argv[k];
            if (!pl[k].nreg)
                continue;
            /* Already placed by the parallel move above. */
            if (!a->is_struct && a->size <= 8 && in_reg(F, a->vreg))
                continue;
            if (a->is_struct) {
                rd(F, a->vreg, T_ADDR);
                for (int q = 0; q < pl[k].nreg; q++) {
                    long off = (long)q * 4;
                    if (off + 4 > a->size && (a->size & 3)) {
                        /* The last, partial word: assembled byte by
                         * byte into its register. */
                        t_mov_imm(t, pl[k].reg + q, 0, 0);
                        for (long b = a->size - 1; b >= off; b--) {
                            t_shift_imm(t, T_SH_LSL, pl[k].reg + q,
                                        pl[k].reg + q, 8, 0);
                            ldst_must(t, T_ACC, T_ADDR, b, 1, 0, 0);
                            t_alu_reg(t, T_OP_ORR, pl[k].reg + q,
                                      pl[k].reg + q, T_ACC, 0);
                        }
                    } else {
                        ldst_must(t, pl[k].reg + q, T_ADDR, off, 4, 0, 0);
                    }
                }
            } else if (a->size > 4) {
                rd64(F, a->vreg, pl[k].reg, pl[k].reg + 1);
            } else {
                rd(F, a->vreg, pl[k].reg);
            }
        }
        /* The hidden result pointer goes in LAST, so nothing above can
         * have used r0 as a scratch after it was set. */
        if (sret)
            fb_addr(F, T_R0, F->scratch_at + i->scratch);
        if (F->tail && F->tail[n] && F->nopush) {
            /* A BRANCH: the callee returns to this function's caller with
             * lr as it came in. Only in a function that pushes nothing,
             * which the tail calls themselves can make it -- they do not
             * count as calls for the leaf test. Anywhere else the pop
             * would have to put lr back, and `pop.w {..., lr}; b.w` is
             * two bytes more than `bl; pop {..., pc}`; and since every
             * push saves lr, the ordinary call there is sound. */
            if (cg_call_local(F->fn->src, i->callee)) {
                note_call(F->st, t_b(t), i->callee);
                F->st->call[F->st->ncall - 1].tail = 1;
            } else {
                note_ext(F->st, t_b(t), i->callee);
                F->st->ext[F->st->next - 1].tail = 1;
            }
            if (n + 1 < fn->nins)
                F->skip_next = 1;     /* the IR_RET: not reached */
            return;
        }
        if (i->indirect && i->call_cmse) {
            tcg_cmse_check_call(fn, i, &w, sret);
            rd(F, i->a, T_ACC);
            cmse_call(F, w.ncrn);
            /* the Non-secure callee is not trusted to have extended a
             * narrow result as the AAPCS asks */
            if (i->dst >= 0 && !i->retsize &&
                (i->ret_tybytes == 1 || i->ret_tybytes == 2))
                t_ext(t, T_R0, T_R0, i->ret_tybytes, i->ret_tysign);
        } else if (i->indirect) {
            rd(F, i->a, T_ACC);
            t_blx(t, T_ACC);
        } else if (cg_call_local(F->fn->src, i->callee)) {
            note_call(F->st, t_bl(t), i->callee);
        } else {
            note_ext(F->st, t_bl(t), i->callee);
        }
        if (i->dst >= 0 && rvfp) {
            /* Back in s0 / d0, or s0-s3 / d0-d3 for an aggregate. */
            if (i->retsize) {
                fb_addr(F, T_ADDR, F->scratch_at + i->scratch);
                vfp_aggregate(F, T_ADDR, 0, rvfp, resz == 8, 1);
                wr(F, i->dst, T_ADDR);
            } else if (resz == 8) {
                vfp_store_d(F, i->dst, 0);
            } else {
                vfp_store(F, i->dst, 0);
            }
        } else if (i->dst >= 0) {
            if (i->retsize) {
                /* dst receives the scratch's ADDRESS, which is the
                 * contract irgen shares with the other backends. A
                 * four-byte composite came back in r0 and has to be
                 * stored there first; a larger one the callee already
                 * wrote through the pointer. */
                if (!sret) {
                    fb_addr(F, T_ADDR, F->scratch_at + i->scratch);
                    ldst_must(t, T_R0, T_ADDR, 0, i->retsize, 0, 1);
                }
                long fo;
                if (!faddr(F, i->dst, &fo)) {   /* else recomputed */
                    fb_addr(F, T_ACC, F->scratch_at + i->scratch);
                    wr(F, i->dst, T_ACC);
                }
            } else if (F->wide[i->dst]) {
                wr64(F, i->dst, T_R0, T_R1);
            } else {
                wr(F, i->dst, T_R0);
            }
        }
        return;
    }

    case IR_RET: {
        int resz, rvfp = i->a >= 0 ? fn_ret_vfp(fn, &resz) : 0;
        if (rvfp) {
            /* Home in s0 / d0, or s0-s3 / d0-d3 for an aggregate whose
             * ADDRESS `a` holds. The epilogue only pops core registers,
             * so nothing disturbs them on the way out. */
            if (fn->ret_abi.is_struct) {
                rd(F, i->a, T_ADDR);
                vfp_aggregate(F, T_ADDR, 0, rvfp, resz == 8, 0);
            } else if (resz == 8) {
                vfp_load_d(F, i->a, 0);
            } else {
                vfp_load(F, i->a, 0);
            }
            goto ret_epilogue;
        }
        if (i->a >= 0 && fn->ret_abi.size && fn->ret_abi.is_struct) {
            /* `a` holds the ADDRESS of the composite being returned.
             * Four bytes or fewer come back in r0; anything larger is
             * copied to the buffer the caller named, whose address is
             * also what r0 must hold at the return. */
            long n = fn->ret_abi.size;
            if (F->sret_slot >= 0 && n <= t_block_straight()) {
                /* Nothing is live past a return, so r0-r3 are this
                 * copy's: the buffer's address straight into r0, where
                 * it has to be anyway, the source in r1 unless it is a
                 * local (the frame base and an offset), and the words
                 * through r2 -- every access a two-byte one where r10,
                 * r11 and r12 made each of them four, and no reload of
                 * r0 after. */
                long fo = 0, k = 0;
                int fa = faddr(F, i->a, &fo);
                if (!fa)
                    rd(F, i->a, T_R1);
                fb_ld(F, T_R0, F->sret_slot, 4, 0);
                for (; k < n; ) {
                    int sz = k + 4 <= n ? 4 : 1;
                    if (fa) fb_ld(F, T_R2, fo + k, sz, 0);
                    else ldst_must(t, T_R2, T_R1, k, sz, 0, 0);
                    ldst_must(t, T_R2, T_R0, k, sz, 0, 1);
                    k += sz;
                }
                goto ret_epilogue;
            }
            rd(F, i->a, T_ADDR);
            if (F->sret_slot >= 0) {
                fb_ld(F, T_TMP, F->sret_slot, 4, 0);
                t_copy_block(t, T_TMP, T_ADDR, 1, n);
                /* the loop form moves the pointer: r0 from the slot */
                fb_ld(F, T_R0, F->sret_slot, 4, 0);
            } else {
                /* Four bytes or fewer, in r0. A three-byte composite is
                 * read as a word: it is at least four-byte aligned and
                 * the high byte is padding the caller ignores. */
                ldst_must(t, T_R0, T_ADDR, 0, n == 3 ? 4 : (int)n, 0, 0);
            }
            goto ret_epilogue;
        }
        if (i->a >= 0) {
            /* From the VALUE's width, not the instruction's: IR_RET
             * carries no `w` at all, so asking it returns zero and a
             * `long long` goes home in r0 with its high half left
             * behind. The wide map is the one place that knows. */
            if (F->wide[i->a]) rd64(F, i->a, T_R0, T_R1);
            else               rd(F, i->a, T_R0);
        }
        /* Every return leaves through the epilogue at the end of the
         * function, so there is one place that knows the frame size --
         * except the LAST instruction, which the epilogue already
         * follows. */
    ret_epilogue:
        if (n + 1 < fn->nins && !invert_last_bcond(F, n, fn->nlabels))
            jump_to(F, fn->nlabels);
        return;
    }

    case IR_UD2:
        /* `udf #0` (0xde00): PERMANENTLY UNDEFINED, which is what this
         * op means. A load from address zero was standing in for it and
         * is not the same thing at all — on a Cortex-M address zero is
         * the vector table and the load succeeds, so a
         * __builtin_unreachable() that was reached carried on. (ARM
         * state: its own `udf #0`, a word.) */
        if (t_isa_a32) {
            a32_udf(t, 0);
            return;
        }
        code_byte(t, 0x00);
        code_byte(t, 0xde);
        return;
    case IR_FENCE:
        /* dmb sy — a full data barrier. */
        t_barrier(t, T_BAR_DMB);
        return;

    /* The conversions, which carry no `flt` of their own: `size` is the
     * source's width and `w` the destination's. */
    case IR_I2F: {
        /* size/sign describe the integer source, w the float result.
         *
         * irgen USED TO convert an `unsigned int` by asking for a SIGNED
         * 64-bit conversion of it, on the grounds that "a 32-bit
         * operation zero-extends its result into the eight-byte slot".
         * That is true of a register write on x86-64 and aarch64 and
         * false of a four-byte stack slot here, where the next four
         * bytes are another temporary — which is what the explicit zero
         * extension below cost.
         *
         * It no longer does: target_widen_unsigned_fp_cvt() is false for
         * this target, so an unsigned 32-bit source arrives as size 4 with
         * sign 0 and __floatunsisf is called by name. The widening path
         * stays because `size == 8` with a narrow source vreg is still a
         * representable shape, and zero-extending it is still right. */
        if (fp_on_vfp(i)) {
            /* vcvt reads its integer from an S register -- s0 for a
             * double result too, which may be d0 itself: the conversion
             * reads its source before it writes. */
            int dbl = i->w == 8;
            int d = dbl ? vfp_dst_d(F, i->dst, T_FD0)
                        : vfp_dst(F, i->dst, T_FS0);
            t_vmov_core(t, T_FS0, rdr(F, i->a, T_ACC), 1);
            t_vcvt_f_from_i(t, d, T_FS0, i->sign, dbl);
            if (dbl) vfp_done_d(F, i->dst, d);
            else     vfp_done(F, i->dst, d);
            return;
        }
        if (i->size == 8) {
            if (F->wide[i->a]) {
                rd64(F, i->a, T_R0, T_R1);
            } else {
                rd(F, i->a, T_R0);
                t_mov_imm(t, T_R1, 0, 0);
            }
        } else {
            rd(F, i->a, T_R0);
        }
        call_helper(F, i->size == 8
                    ? (i->sign ? (i->w == 8 ? "__floatdidf" : "__floatdisf")
                               : (i->w == 8 ? "__floatundidf" : "__floatundisf"))
                    : (i->sign ? (i->w == 8 ? "__floatsidf" : "__floatsisf")
                               : (i->w == 8 ? "__floatunsidf" : "__floatunsisf")));
        fp_result(F, i->dst, i->w);
        return;
    }
    case IR_F2I: {
        /* size is the float source's width, w/sign the integer result. */
        if (fp_on_vfp(i)) {
            /* Round toward zero, which is C's conversion and what
             * t_vcvt_i_from_f encodes. The integer lands in s0 whichever
             * width the source is. */
            int dbl = i->size == 8;
            int r = wreg(F, i->dst, T_ACC);
            t_vcvt_i_from_f(t, T_FS0, dbl ? vfp_src_d(F, i->a, T_FD0)
                                          : vfp_src(F, i->a, T_FS0),
                            i->sign, dbl);
            t_vmov_core(t, T_FS0, r, 0);
            wrote(F, i->dst, r);
            return;
        }
        fp_arg(F, i->a, i->size, T_R0);
        call_helper(F, i->size == 8
                    ? (i->w == 8 ? (i->sign ? "__fixdfdi" : "__fixunsdfdi")
                                 : (i->sign ? "__fixdfsi" : "__fixunsdfsi"))
                    : (i->w == 8 ? (i->sign ? "__fixsfdi" : "__fixunssfdi")
                                 : (i->sign ? "__fixsfsi" : "__fixunssfsi")));
        if (i->w == 8) wr64(F, i->dst, T_R0, T_R1);
        else           wr(F, i->dst, T_R0);
        return;
    }
    case IR_F2F:
        if (i->size == i->w) {          /* nothing to convert */
            if (i->w == 8 && (in_freg(F, i->a) || in_freg(F, i->dst)))
                vfp_store_d(F, i->dst, vfp_src_d(F, i->a, T_FD0));
            else if (i->w == 8) { rd64(F, i->a, A_LO, A_HI);
                                  wr64(F, i->dst, A_LO, A_HI); }
            else           { rd(F, i->a, T_ACC); wr(F, i->dst, T_ACC); }
            return;
        }
        if (fp_on_vfp(i)) {
            /* One vcvt: widening is exact, and narrowing rounds as
             * FPSCR says -- to nearest, ties to even, from reset, which
             * is what __truncdfsf2 does and C's default mode. */
            if (i->w == 8) {
                int d = vfp_dst_d(F, i->dst, T_FD0);
                t_vcvt_f_f(t, d, vfp_src(F, i->a, T_FS0), 1);
                vfp_done_d(F, i->dst, d);
            } else {
                int d = vfp_dst(F, i->dst, T_FS0);
                t_vcvt_f_f(t, d, vfp_src_d(F, i->a, T_FD0), 0);
                vfp_done(F, i->dst, d);
            }
            return;
        }
        fp_arg(F, i->a, i->size, T_R0);
        call_helper(F, i->size == 4 ? "__extendsfdf2" : "__truncdfsf2");
        fp_result(F, i->dst, i->w);
        return;
    case IR_SQRT:
        t_refuse(fn, i, "__builtin_sqrt (a libm routine here, not an "
                        "instruction)");
        return;
    case IR_ASM: {
        /* Extended asm, assembled in irgen (thumb/irgen.c irg_asm_thumb)
         * against the vocabulary in thumb/asm.c. This only places the
         * operands and splices the bytes.
         *
         * To the allocator (ra_target.asm_in_reg) a value live across an
         * asm keeps out of the registers the asm may change, which irgen
         * recorded (ir_asm.clob): its operands', its clobbers', the
         * template's and its scratch, and r0-r3, r12 and lr if it calls.
         * irgen refuses r4-r11 in a template or clobber list. The
         * operands are values like any other, so they are moved into and
         * out of their registers here, each way as ONE parallel move: one
         * at a time would overwrite a register a later operand is still
         * to be read from. */
        struct ir_asm *ia = i->asm_ir;
        /* A continuation's value was written by the asm before it, which
         * must be right there: nothing may run between an asm and the
         * moment its registers are read. */
        if (ia->cont) {
            int k = n - 1;
            while (k >= 0 && fn->ins[k].op == IR_ASM && fn->ins[k].asm_ir &&
                   fn->ins[k].asm_ir->cont)
                k--;
            if (k < 0 || fn->ins[k].op != IR_ASM)
                internal_error("thumb: %s: an asm's further output is not "
                               "right after the asm", fn->name);
            return;
        }
        /* this asm's value outputs: its own, then its continuations' */
        int vreg_[16], vdst[16], nval = 0;
        for (int k = 0; k < ia->nout; k++)
            if (ia->out[k].val) {
                vreg_[nval] = ia->out[k].reg;
                vdst[nval++] = i->dst;
            }
        for (int q = n + 1; q < fn->nins && fn->ins[q].op == IR_ASM &&
                            fn->ins[q].asm_ir && fn->ins[q].asm_ir->cont &&
                            nval < 16; q++) {
            vreg_[nval] = fn->ins[q].asm_ir->out[0].reg;
            vdst[nval++] = fn->ins[q].dst;
        }
        int used[16] = { 0 };
        /* The address scratch. r12 is the ABI's own and the only
         * register that is neither an argument nor callee-saved, so it
         * is tried first; r0-r3 after it, when an operand has taken it. */
        static const int scr_pool[] = { 12, 0, 1, 2, 3 };
        int scr = -1;
        for (int k = 0; k < ia->nin; k++) used[ia->in[k].reg] = 1;
        for (int k = 0; k < ia->nout; k++) used[ia->out[k].reg] = 1;
        for (int k = 0; k < nval; k++) used[vreg_[k]] = 1;
        /* irgen chose it, and told the allocator it is changed here */
        if (ia->clob)
            scr = ia->scr;
        else
            for (unsigned k = 0; k < sizeof scr_pool / sizeof scr_pool[0];
                 k++)
                if (!used[scr_pool[k]]) { scr = scr_pool[k]; break; }
        int naddr = 0;
        for (int k = 0; k < ia->nout; k++)
            naddr += !ia->out[k].val && !ia->out[k].mem;
        if (scr < 0 && naddr > 0)
            t_refuse(fn, i, "an asm with no scratch register left around it");
        for (int k = 0; k < ia->nout; k++)
            if (!ia->out[k].mem && ia->out[k].size > 4)
                t_refuse(fn, i, "an asm output wider than a register");
        /* In: an input's value, an "m" output's address, and a "+"
         * output's address (its current value is loaded through it
         * below) -- the register-resident ones as one parallel move,
         * then the rest from wherever they are. */
        {
            int pd[24], ps[24], npm = 0;
            for (int k = 0; k < ia->nin && npm < 24; k++)
                if (in_reg(F, ia->in[k].temp)) {
                    pd[npm] = ia->in[k].reg;
                    ps[npm++] = F->loc[ia->in[k].temp];
                }
            for (int k = 0; k < ia->nout && npm < 24; k++)
                if (!ia->out[k].val &&
                    (ia->out[k].mem || ia->out[k].inout) &&
                    in_reg(F, ia->out[k].temp)) {
                    pd[npm] = ia->out[k].reg;
                    ps[npm++] = F->loc[ia->out[k].temp];
                }
            if (npm) {
                int od[48], os[48];
                int m = ra_parallel_move(pd, ps, npm, R_SCR, od, os,
                                         (int)(sizeof od / sizeof od[0]));
                if (m < 0)
                    internal_error("thumb: an asm's operands are not a "
                                   "well-formed move");
                for (int k = 0; k < m; k++)
                    t_mov_reg(t, pm_reg(od[k]), pm_reg(os[k]));
            }
            for (int k = 0; k < ia->nin; k++)
                if (!in_reg(F, ia->in[k].temp))
                    rd(F, ia->in[k].temp, ia->in[k].reg);
            for (int k = 0; k < ia->nout; k++) {
                const struct ir_asm_op *o = &ia->out[k];
                if (o->val || !(o->mem || o->inout))
                    continue;
                if (!in_reg(F, o->temp))
                    rd(F, o->temp, o->reg);
                /* A "+" output starts with the lvalue's CURRENT value. */
                if (o->inout && !o->mem)
                    ldst_must(t, o->reg, o->reg, 0, o->size, 0, 0);
            }
        }
        for (int k = 0; k < ia->codelen; k++)
            code_byte(t, ia->code[k]);
        /* Out, through an address: the address is live across the asm
         * (regalloc.c counts it as crossing), so it is still there. An
         * "m" output was written BY the template through the address its
         * register holds; storing over it would destroy what it wrote. */
        for (int k = 0; k < ia->nout; k++) {
            const struct ir_asm_op *o = &ia->out[k];
            if (o->mem || o->val)
                continue;
            rd(F, o->temp, scr);
            ldst_must(t, o->reg, scr, 0, o->size, 0, 1);
        }
        /* Out, as values: each to its home. Those in memory first, while
         * every operand register still holds what the asm left; then
         * the register-resident ones as one parallel move. */
        {
            int pd[16], ps[16], npm = 0;
            for (int k = 0; k < nval; k++) {
                if (vdst[k] < 0)
                    continue;
                if (in_reg(F, vdst[k])) {
                    pd[npm] = F->loc[vdst[k]];
                    ps[npm++] = vreg_[k];
                } else {
                    wr(F, vdst[k], vreg_[k]);
                }
            }
            if (npm) {
                int od[32], os[32];
                int m = ra_parallel_move(pd, ps, npm, R_SCR, od, os,
                                         (int)(sizeof od / sizeof od[0]));
                if (m < 0)
                    internal_error("thumb: an asm's outputs are not a "
                                   "well-formed move");
                for (int k = 0; k < m; k++)
                    t_mov_reg(t, pm_reg(od[k]), pm_reg(os[k]));
            }
        }
        return;
    }
    case IR_VA_START:
        /* `a` holds the ADDRESS of the va_list, which on this ABI is a
         * bare pointer at the next argument. */
        rd(F, i->a, T_ADDR);
        fb_addr(F, T_ACC, F->va_first);
        ldst_must(t, T_ACC, T_ADDR, 0, 4, 0, 1);
        return;
    case IR_ALLOCA: {
        /* A variable-length array: sp -= round8(size), AAPCS32's stack
         * alignment. The block starts ABOVE the outgoing-argument area,
         * which stays at the bottom where a callee looks for its stack
         * arguments -- so the area moves down with sp and the block sits
         * on top of it. The frame is addressed from r7 in such a
         * function. */
        int d = wreg(F, i->dst, T_ADDR);
        rd(F, i->a, T_ACC);
        t_addw(t, T_ACC, T_ACC, 7);
        t_alu_imm(t, T_OP_BIC, T_ACC, T_ACC, 7, 0);
        t_alu_reg(t, T_OP_SUB, T_SP, T_SP, T_ACC, 0);
        t_add_sp(t, d, F->out_bytes);
        wrote(F, i->dst, d);
        return;
    }
    case IR_SPSAVE: {
        int d = wreg(F, i->dst, T_ACC);
        t_mov_reg(t, d, T_SP);
        wrote(F, i->dst, d);
        return;
    }
    case IR_SPRESTORE:
        t_mov_reg(t, T_SP, rdr(F, i->a, T_ACC));
        return;
    case IR_LANDING:
        t_refuse(fn, i, "an exception landing pad");
        return;
    case IR_SWITCH: {
        /* A jump table in .text right after its dispatch, of 32-bit
         * offsets from the table's own start (plus the Thumb bit, so the
         * sum is ready for bx):
         *     cmp rI, #n ; bhs default
         *     adr.w rT, table ; ldr.w rE, [rT, rI, lsl #2]
         *     add rE, rT ; bx rE
         * rT is r11 and rE r12, the scratch pair; a terminator keeps
         * nothing in them. adr.w reads Align(pc, 4) and is patched once
         * the table's place is known. */
        int n_ins = (int)(i - fn->ins);   /* `n` is the entry count below */
        int n = fn->jt[i->jt].n;
        int ri = rdr(F, i->a, T_ACC);
        if (t_imm_ok(n)) {
            t_cmp_imm(t, ri, n);
        } else {
            t_mov_imm(t, T_TMP, n, 0);
            t_cmp_reg(t, ri, T_TMP);
        }
        jump_if(F, T_CS, i->label);                  /* bhs: unsigned >= n */
        /* ARM state: `add pc, pc, rI, lsl #2` over a table of branches.
         * pc reads as the add's address + 8, which is where the table
         * starts, one word on -- so the word between is never run. The
         * entries are instructions, not data: no $d, and each reaches
         * its case like any other branch. */
        if (t_isa_a32) {
            t_alu_reg_shift(t, T_OP_ADD, T_PC, T_PC, ri, T_SH_LSL, 2, 0);
            t_nop(t);
            for (int k = 0; k < n; k++)
                want_label(F, t_b(t), fn->jt[i->jt].labels[k], -1);
            F->bc_end = -1;
            return;
        }
        /* When every target still lies ahead, the one-instruction form:
         * `tbh [pc, rI, lsl #1]` adds twice the halfword the index picks
         * from the table right after it. Its offsets are unsigned, so a
         * target already placed (a case whose block the CFG cleanup
         * forwarded to a label before the switch) rules it out, and the
         * general form below takes over. */
        int ahead = !(F->no_tbh && F->no_tbh[n_ins] == 1);
        for (int k = 0; k < n; k++)
            if (F->label_off[fn->jt[i->jt].labels[k]] >= 0)
                ahead = 0;
        if (ahead) {
            int at = t_tbh(t, ri);
            for (int k = 0; k < n; k++) {
                want_label(F, t->len, fn->jt[i->jt].labels[k], T_TBH);
                F->fix[F->nfix - 1].cz_at = at + 4;
                F->fix[F->nfix - 1].ins = n_ins;
                code_u16(t, 0);
            }
            code_mark_data(t, at + 4, t->len);
            return;
        }
        int adr_at = t_adr_w(t, T_TMP, 0);
        t_ldst_reg(t, T_ACC, T_TMP, ri, 2, 4, 0, 0);
        t_alu_reg(t, T_OP_ADD, T_ACC, T_ACC, T_TMP, 0);
        t_bx(t, T_ACC);
        if (t->len % 4)
            t_nop(t);
        int tab = t->len, disp = tab - ((adr_at + 4) & ~3);
        if (disp < 0 || disp > 4095)
            internal_error("thumb: %s: the jump table is out of adr.w's "
                           "reach", fn->name);
        t_patch_adr_w(t, adr_at, T_TMP, disp);
        for (int k = 0; k < n; k++) {
            want_label(F, t->len, fn->jt[i->jt].labels[k], T_TAB);
            F->fix[F->nfix - 1].cz_at = tab;
            code_u32(t, 0);
        }
        code_mark_data(t, tab, t->len);
        return;
    }
    case IR_LABELADDR: {
        if (cg_label_mark(i))       /* static data's marker: no code */
            return;
        /* &&label, PC-relative and with no relocation: the label's
         * distance from the pc the `add` reads, built by movw/movt and
         * patched once the label is placed:
         *     movw rD, #lo ; movt rD, #hi ; add rD, pc
         * In Thumb state pc reads as the add + 4 and the distance carries
         * the Thumb bit, so the address is ready for bx; in ARM state it
         * is `add rD, pc, rD`, pc reading as the add + 8, bit 0 clear.
         * adr.w would be one instruction but reaches only 4 KiB. */
        int d = wreg(F, i->dst, T_ACC);
        int at = t_mov_addr(t, d, 0), add_at = t->len;
        if (t_isa_a32)
            a32_alu_reg(t, T_OP_ADD, d, T_PC, d, 0);
        else
            t1_add_hi(t, d, T_PC);
        want_label(F, at, i->label, T_LADDR);
        F->fix[F->nfix - 1].cz_at = add_at;
        F->fix[F->nfix - 1].ins = d;
        wrote(F, i->dst, d);
        return;
    }
    case IR_IGOTO:
        t_bx(t, rdr(F, i->a, T_ACC));
        return;
    case IR_XCHG: case IR_XADD: case IR_ARMW:
    case IR_CAS: case IR_CMPXCHG:
        thumb_atomic(F, i);
        return;
    case IR_FRAMEADDR:
        t_refuse(fn, i, "__builtin_frame_address or __builtin_return_address "
                        "(this backend keeps no frame-pointer chain)");
        return;
    case IR_CAS16:
        t_refuse(fn, i, "a 16-byte atomic (ARMv7-M has no doubleword "
                        "exclusive, let alone a quadword one)");
        return;
    default:
        t_refuse(fn, i, "this operation");
        return;
    }
}

/* ---- one function --------------------------------------------------- */

/* ---- register pairs for 64-bit values -----------------------------------
 *
 * The allocator hands out one register per value, so a double or a long
 * long -- two registers -- was always in a stack slot, and fdlibm on
 * soft-float Cortex-M ran at 2.2x clang's size. As on RV32 (and AVR's
 * quads), the allocator runs first over PAIRS (T_PAIRS) for the 64-bit
 * values alone, and the ordinary pass gets the registers no pair took
 * (g_t_taken). rd64/wr64 are pair-aware, and every setup that loads two
 * 64-bit operands, or a call's register arguments, is one parallel move
 * over the halves.
 *
 * Not with an FPU: the hard-float paths pass doubles in d registers
 * through vfp_load_d/vfp_store_d, which were written for slots -- they
 * reach a pair through rd64/wr64 now, as they reach a d register, but
 * the pair pass has not been run beside the FP passes, and on the
 * double-precision unit the doubles have d registers of their own. And
 * not for a variadic function's 64-bit parameters, a wide local read or
 * written narrower than itself, or anything pinned for -g. slot_of()
 * refuses a path this misses. */
static int g_t_pairs = 1;
/* t_lowregs on or off for this attempt (gen_func_best). */
static int g_t_lowregs = 1;

static void t_pair_hints(const struct ir_func *fn, int *hint)
{
    struct abi_walk w;
    struct argplace pl;
    walk_init(&w, fn_sret_bytes(fn) != 0, fn->is_varargs, fn->pcs);
    for (int p = 0; fn->src && p < fn->nparams && p < fn->nvregs; p++) {
        struct ir_arg *a = &fn->param_abi[p];
        place_one(&w, a, &pl);
        if (a->size == 8 && !a->is_struct && pl.nreg == 2)
            hint[p] = pl.reg;
    }
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        if (i->op == IR_RET && i->a >= 0 && i->a < fn->nvregs &&
            !fn->ret_abi.is_struct && fn->ret_abi.size == 8)
            hint[i->a] = 0;
        if (i->op != IR_CALL && t_op_calls_helper(i) && i->w == 8) {
            if (i->a >= 0 && i->a < fn->nvregs && hint[i->a] < 0)
                hint[i->a] = 0;
            if (!i->imm_b && i->b >= 0 && i->b < fn->nvregs &&
                hint[i->b] < 0)
                hint[i->b] = 2;
            if (i->dst >= 0 && i->dst < fn->nvregs && hint[i->dst] < 0)
                hint[i->dst] = 0;
        }
        if (i->op != IR_CALL)
            continue;
        if (!i->retsize && i->dst >= 0 && i->dst < fn->nvregs && i->w == 8)
            hint[i->dst] = 0;
        {
            struct abi_walk cw;
            walk_init(&cw, call_sret_bytes(i) != 0, i->call_varargs,
                      i->call_pcs);
            for (int k = 0; k < i->nargs; k++) {
                const struct ir_arg *a = &i->argv[k];
                place_one(&cw, a, &pl);
                if (a->size == 8 && !a->is_struct && pl.nreg == 2 &&
                    a->vreg >= 0 && a->vreg < fn->nvregs)
                    hint[a->vreg] = pl.reg;
            }
        }
    }
}

static const struct ra_target THUMB_PAIR_RA = {
    t_pair_pool_for, t_callee_saved, t_ldvar_plain,
    1, 1, 0,
    t_op_calls_helper,
    0,
    t_pair_hints,
    NULL, NULL,
    1,
    NULL, NULL,
    0, /* atomic_in_reg */
    0, /* fp_reads_gpr */
    0  /* asm_in_reg: a pair live across an asm stays in memory */
};

/* The pair pass: a vreg -> low register map, or NULL for none. Fills
 * g_t_taken with every register a pair holds, and `used` with the
 * callee-saved ones to push. */

/* The pair pass's registers, each over its value's live range only, for
 * the integer pass that follows (ra_reserve). */
static struct ra_range *g_t_res;
static int g_t_nres, g_t_capres;
static void g_t_reserve_pairs(struct ir_func *fn, const int *loc)
{
    int nv = fn->nvregs;
    int *first = xmalloc((size_t)(nv ? nv : 1) * sizeof *first);
    int *last = xmalloc((size_t)(nv ? nv : 1) * sizeof *last);
    ra_live_ranges(fn, first, last);
    g_t_nres = 0;
    for (int v = 0; v < nv; v++) {
        int born = 0;
        if (loc[v] < 0 || first[v] < 0) continue;
        if (g_t_nres + 2 > g_t_capres) {
            g_t_capres = g_t_capres ? g_t_capres * 2 : 16;
            g_t_res = xrealloc(g_t_res, (size_t)g_t_capres * sizeof *g_t_res);
        }
        /* A pair dying at a 64-bit compare leaves its registers to the
         * 0 or 1 born there: cmp64 reads both operands and writes only
         * r12, and set_cc writes the result after. Reserved through the
         * compare, `return a < b` built the result in r4, pushed it,
         * and moved it to r0. A compare falls through to the next
         * instruction, so a value still live after it would be live
         * there too: last[v] at the compare is a value that dies there. */
        if (last[v] < fn->nins) {
            const struct ir_ins *c = &fn->ins[last[v]];
            born = c->op == IR_CMP && c->w == 8 && !c->flt &&
                   (c->a == v || (!c->imm_b && c->b == v));
        }
        for (int h = 0; h < 2; h++) {
            g_t_res[g_t_nres].reg = loc[v] + h;
            g_t_res[g_t_nres].first = first[v];
            g_t_res[g_t_nres].last = last[v];
            g_t_res[g_t_nres].born = born;
            g_t_nres++;
        }
    }
    ra_reserve(g_t_res, g_t_nres);
    free(first); free(last);
}
static int *t_pair_alloc(struct ir_func *fn, const char *wide,
                         const char *excl, int *used, int *nused)
{
    int nv = fn->nvregs, any = 0;
    char *x;
    int *loc;

    *nused = 0;
    if (target_thumb_fpu() || !wide)
        return NULL;
    x = xcalloc((size_t)(nv ? nv : 1), 1);
    for (int v = 0; v < nv; v++) {
        x[v] = !wide[v] || (excl && excl[v]) ||
               (v < fn->nparams && fn->is_varargs);
        any |= !x[v];
    }
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        if (i->op == IR_LDVAR && i->a >= 0 && i->a < nv && i->size != 8)
            x[i->a] |= (char)wide[i->a];
        if (i->op == IR_STVAR && i->dst >= 0 && i->dst < nv && i->size != 8)
            x[i->dst] |= (char)wide[i->dst];
    }
    if (!any) {
        free(x);
        return NULL;
    }
    loc = ra_allocate(fn, &THUMB_PAIR_RA, NULL, x, used, nused);
    free(x);
    /* Each pair's registers are the integer pass's to use outside the
     * pair's live range: reserved by range, not withheld whole. */
    g_t_reserve_pairs(fn, loc);
    return loc;
}

/* ---- LOW SCRATCH -----------------------------------------------------
 *
 * r9-r12 are this file's scratch registers, and all four are high: a
 * value that lives in a slot came back through `ldr.w r12, [sp, #n]`,
 * four bytes where `ldr r2, [sp, #n]` is two, and whatever worked on it
 * after took a 32-bit form too -- a quarter of the instructions in the
 * non-math corpus named one of them.
 *
 * Most instructions leave some of r0-r7 holding nothing. A register is
 * free across instruction n when no value live into or out of it, and
 * none it reads or writes, has its home there; n + 1 counts as well,
 * because a comparison emits the branch after it. r0-r3 are free for
 * the asking, r4-r7 only once the prologue saves them anyway, and r7
 * never while it is the frame base.
 *
 * Only for the instructions below, whose lowering names no low register
 * of its own: a call, a helper, inline asm, an atomic and every 64-bit
 * path put arguments and pairs in fixed ones, and a scratch there could
 * be the register an argument is about to arrive in. */
static int lo_op_ok(const struct t_fn *F, const struct ir_ins *i)
{
    int nv = F->fn->nvregs, v[3], k;
    switch (i->op) {
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
    case IR_AND: case IR_OR: case IR_XOR: case IR_SHL: case IR_SHR:
    case IR_NEG: case IR_BNOT: case IR_CMP: case IR_MULH:
        if (i->flt)
            return 0;
        break;
    case IR_CONST: case IR_MOV: case IR_BITCAST:
    case IR_LDVAR: case IR_STVAR: case IR_LOAD: case IR_STORE: case IR_EXT:
    case IR_ADDR: case IR_STRADDR: case IR_GADDR: case IR_FADDR:
    case IR_BRZ: case IR_BRNZ:
        break;
    case IR_MEMCPY: case IR_MEMZERO:
        return 1;              /* its own registers are r9-r12; `size` is
                                * a byte count, not a width */
    default:
        return 0;
    }
    if (t_op_calls_helper(i) || i->w > 4 || i->size > 4)
        return 0;
    v[0] = i->a; v[1] = i->b; v[2] = i->dst;
    for (k = 0; k < 3; k++)
        if (v[k] >= 0 && v[k] < nv && F->wide[v[k]])
            return 0;
    return 1;
}

struct lo_busy { const struct t_fn *F; unsigned busy; };

static void lo_mark(int v, void *ctx)
{
    struct lo_busy *b = ctx;
    const struct t_fn *F = b->F;
    if (v < 0 || v >= F->fn->nvregs || !F->loc || F->loc[v] < 0)
        return;
    b->busy |= 1u << F->loc[v];
    if (F->wide[v])
        b->busy |= 2u << F->loc[v];     /* the pair's high register */
}

/* lo_free's answer for instruction n whatever its op, over n alone. For
 * a lowering that writes its scratch before it touches any register of
 * its own: a call's stack arguments are stored before the argument
 * registers are loaded, so a register holding nothing live into or out
 * of the call, and none of its operands, is free for them. */
static unsigned lo_free_at(const struct t_fn *F, int n)
{
    const struct ir_func *fn = F->fn;
    struct lo_busy b;
    unsigned avail = 0xfu;
    if (!F->lv_busy)
        return 0;
    for (int k = 0; k < F->nsave; k++)
        if (F->used_callee[k] < 8)
            avail |= 1u << F->used_callee[k];
    if (F->fb == 7 || fn->has_alloca)
        avail &= ~(1u << 7);
    b.F = F;
    b.busy = F->lv_busy[n];
    ra_each_use(&fn->ins[n], lo_mark, &b);
    lo_mark(fn->ins[n].dst, &b);
    return avail & ~b.busy;
}

static unsigned lo_free(const struct t_fn *F, int n)
{
    const struct ir_func *fn = F->fn;
    struct lo_busy b;
    unsigned avail = 0xfu;
    int m, k;
    if (!F->lv_busy || !lo_op_ok(F, &fn->ins[n]))
        return 0;
    for (k = 0; k < F->nsave; k++)
        if (F->used_callee[k] < 8)
            avail |= 1u << F->used_callee[k];
    if (F->fb == 7 || fn->has_alloca)
        avail &= ~(1u << 7);
    b.F = F;
    b.busy = 0;
    for (m = n; m < fn->nins && m <= n + 1; m++) {
        b.busy |= F->lv_busy[m];
        ra_each_use(&fn->ins[m], lo_mark, &b);
        lo_mark(fn->ins[m].dst, &b);
    }
    return avail & ~b.busy;
}

/* ---- SCRATCH ROLES WHERE r9-r11 ARE ALSO HOMES (g_t_ext) ---------------
 *
 * The registers holding something across instructions n..n+span: every
 * value live into or out of one of them, and every one they read or
 * write -- lo_free's question, asked of all sixteen. span 2 covers the
 * furthest an instruction's lowering emits ahead (skip_next). */
static unsigned t_busy(const struct t_fn *F, int n, int span)
{
    const struct ir_func *fn = F->fn;
    struct lo_busy b;
    b.F = F;
    b.busy = 0;
    if (!F->loc)
        return 0;
    for (int m = n; m < fn->nins && m <= n + span; m++) {
        if (F->lv_busy)
            b.busy |= F->lv_busy[m];
        ra_each_use(&fn->ins[m], lo_mark, &b);
        lo_mark(fn->ins[m].dst, &b);
    }
    return b.busy;
}

/* The roles from what `busy` leaves of r9-r11: TMP first, then ADDR,
 * then SCR, each its usual register when that is free; -1 for a role
 * with none left. Without g_t_ext, the fixed r11, r10 and r9. */
static void t_roles_from(unsigned busy)
{
    int fr[3], nf = 0;
    if (!g_t_ext) {
        g_r_tmp = 11; g_r_addr = 10; g_r_scr = 9;
        return;
    }
    for (int r = 11; r >= 9; r--)
        if (!(busy >> r & 1))
            fr[nf++] = r;
    g_r_tmp = nf > 0 ? fr[0] : -1;
    g_r_addr = nf > 1 ? fr[1] : -1;
    g_r_scr = nf > 2 ? fr[2] : -1;
}

/* lo_free's liveness, as registers: for each instruction, lo_mark of
 * every value live into it or out of it. It read those values out of
 * the per-instruction live sets, one bit per vreg, at every instruction
 * -- most of a 4000-statement Cortex-M compile once the optimizer was
 * linear. A walk back through each block (ra_lset_step) keeps a count
 * of the live values in each register instead, so an instruction costs
 * what changes at it. */
struct lo_cnt { const struct t_fn *F; int cnt[32]; unsigned mask; };
static void lo_cnt_chg(int v, int added, void *ctx)
{
    struct lo_cnt *c = ctx;
    struct lo_busy b;
    b.F = c->F;
    b.busy = 0;
    lo_mark(v, &b);
    for (int r = 0; r < 32; r++) {
        if (!(b.busy >> r & 1))
            continue;
        if (added) {
            if (c->cnt[r]++ == 0) c->mask |= 1u << r;
        } else {
            if (--c->cnt[r] == 0) c->mask &= ~(1u << r);
        }
    }
}

static unsigned *lo_busy_map(const struct t_fn *F)
{
    const struct ir_func *fn = F->fn;
    int nv = fn->nvregs;
    int *lf = xmalloc((size_t)nv * sizeof *lf);
    int *ll = xmalloc((size_t)nv * sizeof *ll);
    struct ra_live *lv = ra_live_compute(fn, lf, ll);
    unsigned *busy = xcalloc((size_t)fn->nins, sizeof *busy);
    struct lo_cnt c;
    struct ra_lset s;
    memset(&c, 0, sizeof c);
    c.F = F;
    ra_lset_init(&s, nv);
    s.chg = lo_cnt_chg;
    s.chg_ctx = &c;
    for (int b = ra_live_nblocks(lv) - 1; b >= 0; b--) {
        ra_lset_out(&s, lv, b);
        for (int i = ra_live_block_start(lv, b + 1) - 1;
             i >= ra_live_block_start(lv, b); i--) {
            unsigned out = c.mask;
            ra_lset_step(&s, &fn->ins[i]);
            busy[i] = out | c.mask;
        }
    }
    ra_lset_free(&s);
    ra_live_free(lv);
    free(lf); free(ll);
    return busy;
}

/* The callee-saved VFP registers a function uses, s16 up, `n` of them
 * (always even). On a double-precision unit by their D names, d8 up, as
 * clang and GCC write it for a Cortex-M7: the same words saved to the
 * same places, and a disassembly that says d8 where a double lives. */
static void t_vsave(struct code *t, int n, int pop)
{
    if (target_thumb_fpu_dp())
        t_vpush_d(t, 8, n / 2, pop);
    else
        t_vpush_s(t, 16, n, pop);
}

static void gen_func(struct ir_func *fn, struct code *t, struct t_sites *st,
                     int want_debug)
{
    struct func *f = fn->src;
    struct t_fn F;
    int push_at, i;

    /* Zeroed first: the struct is a local and several fields -- usecnt
     * and skip_next among them -- are only set on some paths, so
     * reading them uninitialised on the others is exactly the
     * segfault this caused at -O0, where the allocator does not run. */
    memset(&F, 0, sizeof F);
    F.fn = fn; F.t = t; F.st = st;
    F.want_debug = want_debug;
    F.fix = NULL; F.nfix = F.capfix = 0;
    F.wide = wide64_map(fn);
    frame_addr_map(&F);
    F.nshr = ra_narrow_hishift(fn);
    for (int v = 0; v < fn->nvregs; v++)
        if (F.nshr[v]) F.wide[v] = 0;
    F.va_regsave = F.va_first = -1;
    F.loc = NULL; F.nsave = 0; F.save_at = 0;
    F.floc = NULL; F.nfsave = 0;
    F.bc_end = F.bc_fix = -1;
    F.fl_end = -1;
    F.shortb = NULL; F.nshortb = 0;
    F.scr_save = T_SCR_ALL;
    F.fb = T_SP;
    F.leaf = F.nopush = 0;
    if (g_t_regalloc) {
        /* nsave is what the allocator REPORTS it took, and the prologue
         * pushes exactly that -- so the two must be computed together.
         * The EMBCC_T_RA_MAX gate below therefore has to clear nsave as
         * well as loc, or the pushes stay and every stack parameter is
         * read from the wrong offset with nothing in a register to show
         * for it. */
        /* `wide` here DOES mean "never eligible": this is a 32-bit
         * machine throughout, so an eight-byte value needs a register
         * pair and the allocator hands out one. (RISC-V had to pass NULL
         * at RV64, where the same map means the opposite thing.)
         *
         * fltmap NULL: ARMv7-M's base profile has no FPU, so a float
         * lives in a core register and must stay eligible for this pool. */
        /* Under -g every source variable stays in its frame slot, so
         * the DW_AT_location naming that slot is true. A variable in a
         * register needs a location list to describe, which is the
         * larger feature; this is exact. */
        char *pin = want_debug || g_t_o0 ? ra_debug_pin_vars(fn)
                                         : (char *)0;
        char *flt = t_float_map(fn, F.wide, want_debug || g_t_o0);
        char *dbl = t_double_map(fn, F.wide, want_debug || g_t_o0);
        /* The integer pass must not give a GPR to a value the FP pass
         * owns, and the -g pins are the same kind of "not here" -- so it
         * takes the union of the two. */
        char *excl = pin;
        if (flt) {
            excl = xcalloc((size_t)fn->nvregs, 1);
            for (int v = 0; v < fn->nvregs; v++)
                excl[v] = (char)(flt[v] || (dbl && dbl[v]) ||
                                 (pin && pin[v]));
        }
        int pused[RA_MAXPOOL], npused = 0;
        int *pair = g_t_pairs ? t_pair_alloc(fn, F.wide, excl, pused, &npused)
                              : NULL;
        F.loc = ra_allocate(fn, &THUMB_RA, F.wide, excl,
                            F.used_callee, &F.nsave);
        g_t_taken = 0;
        if (pair) {
            for (int v = 0; v < fn->nvregs; v++)
                if (pair[v] >= 0) F.loc[v] = pair[v];
            for (int k = 0; k < npused; k++) {
                F.used_callee[F.nsave++] = pused[k];
                F.used_callee[F.nsave++] = pused[k] + 1;
            }
            free(pair);
        }
        if (g_t_lowregs)
            t_lowregs(fn, F.loc, F.wide);
        if (flt) {
            int fused[32], nfused = 0, top = 15;
            int *dloc = NULL;
            /* The doubles first, on d8-d15 (T_DPOOL), and then the floats
             * on whatever of s16-s31 those leave free over each float's
             * own life: g_t_reserve_pairs hands the float pass both halves
             * of every d register a double took, over that double's live
             * range, which is the same thing the core pairs tell the
             * integer pass. `wide` is NULL here because a double is
             * exactly what this pass is for. */
            if (dbl) {
                int dused[16], ndused = 0;
                dloc = ra_allocate_fp(fn, &THUMB_DRA, NULL, dbl,
                                      dused, &ndused);
                for (int k = 0; k < ndused; k++)
                    if (dused[k] + 1 > top)
                        top = dused[k] + 1;
                if (dloc)
                    g_t_reserve_pairs(fn, dloc);
            }
            F.floc = ra_allocate_fp(fn, &THUMB_RA, F.wide, flt,
                                    fused, &nfused);
            for (int k = 0; k < nfused; k++)
                if (fused[k] > top)
                    top = fused[k];
            if (dloc) {
                if (!F.floc) {
                    F.floc = xmalloc((size_t)fn->nvregs * sizeof *F.floc);
                    for (int v = 0; v < fn->nvregs; v++)
                        F.floc[v] = -1;
                }
                for (int v = 0; v < fn->nvregs; v++)
                    if (dloc[v] >= 0)
                        F.floc[v] = dloc[v];
                free(dloc);
            }
            /* vpush takes a RANGE, so s16 up to the highest one used,
             * rounded to an even count to keep sp eight-aligned. */
            F.nfsave = (top - 15 + 1) & ~1;
            free(excl);
            free(flt);
        }
        free(dbl);
        free(pin);
        /* Read counts for comparison/branch fusion. Only with the
         * allocator on: without it every value round-trips through a
         * slot and the branch reads the slot, so nothing is saved and
         * the "only reader" claim would not hold. */
        if (fn->nvregs) {
            F.usecnt = xmalloc((size_t)fn->nvregs * sizeof *F.usecnt);
            ra_count_vreg_uses(fn, F.usecnt);
            t_select_imms(&F);
            if (F.selimm) {
                /* an immediate needs no register (nor a slot: layout) */
                for (int v = 0; v < fn->nvregs; v++)
                    if (F.selimm[v])
                        F.loc[v] = -1;
                t_drop_unused_saves(&F);
            }
        }
        {
            const char *lim = getenv("EMBCC_T_RA_MAX");
            if (lim) {
                int n = atoi(lim);
                for (int v = n; v < fn->nvregs; v++)
                    F.loc[v] = -1;
                /* so the gate really is "allocate less" and not "push
                 * registers for nothing" */
                t_drop_unused_saves(&F);
            }
        }
    }
    /* A variable-length array moves sp at run time, so the frame is
     * addressed from r7, which the prologue sets once the frame is in
     * place: callee-saved, out of the pool for such a function
     * (t_pool_for), and so saved like any register the allocator took. */
    if (fn->has_alloca)
        F.used_callee[F.nsave++] = 7;
    {
        unsigned am = t_asm_saved_regs(fn);
        if (fn->has_alloca && (am >> 7 & 1))
            t_refuse(fn, NULL, "an asm operand in r7 (the letter D) in a "
                               "function whose frame r7 addresses");
        for (int r = 4; r <= 8; r++) {
            int have = 0;
            if (!(am >> r & 1))
                continue;
            for (int k = 0; k < F.nsave; k++)
                have |= F.used_callee[k] == r;
            if (!have)
                F.used_callee[F.nsave++] = r;
        }
    }
    /* A leaf: no call in the IR and none the lowering makes -- the
     * t_op_calls_helper the allocator already trusts -- and no inline asm,
     * which might call anything. Such a function never overwrites lr. */
    /* A tail call leaves lr alone -- it is the caller's, and the callee
     * returns with it -- so it does not make this function a non-leaf. */
    F.tail = NULL;
    if (fn->cmse_entry)
        tcg_cmse_check_entry(fn);
    if (g_t_regalloc && !want_debug && !g_t_o0)
        for (i = 0; i < fn->nins; i++)
            if (t_tail_ok(fn, i)) {
                if (!F.tail)
                    F.tail = xcalloc((size_t)fn->nins, 1);
                F.tail[i] = 1;
            }
    /* ...and an atomic writes lr too: its strex reports into it
     * (thumb_atomic), which was harmless only while an atomic's operands
     * lived in memory and so gave every such function a frame. */
    /* An asm writes lr when its template calls or names it, which irgen
     * recorded (ir_asm.clob); one whose clobbers are unknown might. */
    F.leaf = 1;
    for (i = 0; i < fn->nins; i++)
        if ((fn->ins[i].op == IR_CALL && !(F.tail && F.tail[i])) ||
            (fn->ins[i].op == IR_ASM && fn->ins[i].asm_ir &&
             !fn->ins[i].asm_ir->cont &&
             (!fn->ins[i].asm_ir->clob ||
              (fn->ins[i].asm_ir->clob >> 14 & 1))) ||
            t_op_calls_helper(&fn->ins[i]) ||
            fn->ins[i].op == IR_XCHG || fn->ins[i].op == IR_XADD ||
            fn->ins[i].op == IR_ARMW || fn->ins[i].op == IR_CAS ||
            fn->ins[i].op == IR_CMPXCHG)
            F.leaf = 0;
    layout(&F);
    if (F.loc && fn->nins && fn->nvregs && !getenv("EMBCC_T_NOLO"))
        F.lv_busy = lo_busy_map(&F);
    t_lit64_plan(&F);

    /* One more label than the IR has: the epilogue, which every IR_RET
     * jumps to so the frame size is written down once. */
    F.label_off = xmalloc((size_t)(fn->nlabels + 1) * sizeof *F.label_off);
    for (i = 0; i <= fn->nlabels; i++)
        F.label_off[i] = -1;

    /* BRANCH RELAXATION. The function is emitted twice: the first pass
     * uses the 32-bit branch forms everywhere and records which ones
     * would have reached their label in 16 bits, and the second emits
     * those short. Everything else it emits is decided by the IR and the
     * frame, never by code addresses, so the second pass makes the same
     * branches in the same order and the ordinal is enough to match
     * them. Optimising builds only: -O0's output stays as it was. */
    {
    int len0 = t->len, nl0 = fn->nlines, nd0 = t->ndrange;
    int sc0 = F.st->ncall, se0 = F.st->next, ss0 = F.st->nstr,
        sg0 = F.st->ng, sf0 = F.st->nf;
    char *shortb = NULL;
    int nshortb = 0;
    F.no_tbh = xcalloc((size_t)fn->nins + 1, 1);
    int restarted = 0;           /* a first pass made again: reset as for a later one */
    for (int pass = 0; pass < 3; pass++) {
    int redo0 = 0;
    if (!F.lp_planned)
        F.lp_n = 0;              /* else the planned order (t_lit64_fit) */
    F.lp_nsite = 0;
    g_t_scr_used = 0;
    t_roles_from(0);            /* r9-r11, or all three free */
    F.fb = T_SP;                 /* until the prologue sets r7 */
    if (pass || restarted) {
        t->len = len0;
        /* and the jump tables an abandoned pass marked as data: left in,
         * their $d mapping symbols fell on this pass's instructions */
        t->ndrange = nd0;
        fn->nlines = nl0;
        F.st->ncall = sc0; F.st->next = se0; F.st->nstr = ss0;
        F.st->ng = sg0; F.st->nf = sf0;
        F.nfix = 0;
        for (i = 0; i <= fn->nlabels; i++)
            F.label_off[i] = -1;
        F.skip_next = 0;
        F.bc_end = F.bc_fix = -1;
        F.fl_end = -1;
        F.va_regsave = F.va_first = -1;
        F.shortb = shortb;
        F.nshortb = nshortb;
        if (want_debug) {
            free(fn->var_off);
            fn->var_off = NULL;
        }
    }
    /* A Thumb function must start on a halfword, and that is all: this
     * backend has no literal pool whose pc-relative loads would care
     * where it sits (constants and addresses are movw/movt), and clang
     * lays functions out at two as well. Rounding each up to four was
     * a nop after every other one. A function with inline asm keeps the
     * four, since its template may address relative to pc. */
    /* Pad with halfword NOPs (bf00), not with a repeated 0xbf: that
     * byte pairs into 0xbfbf, which is an `itttt` — harmless, since
     * nothing branches there, but it makes every disassembly of the gap
     * between two functions look like a condition block. */
    /* An A32 function is word-aligned, as every A32 instruction is. */
    {
        int al = t_isa_a32 ? 3 : 1;
        for (int k = 0; k < fn->nins; k++)
            if (fn->ins[k].op == IR_ASM)
                al = 3;
        while (t->len & al)
            t_nop(t);
        f->code_align = al + 1;
    }
    /* -g: each source variable's slot, which IS its offset from the
     * DWARF frame base -- sp, because this backend keeps no frame
     * pointer (see src/debug/dwarf.c). */
    if (want_debug) {
        int nv = fn->nvars ? fn->nvars : 1;
        fn->var_off = xmalloc((size_t)nv * sizeof *fn->var_off);
        for (int v = 0; v < fn->nvars; v++)
            fn->var_off[v] = (int)F.slot[v];
    }
    f->code_off = t->len;
    /* The prologue's scratch: not where a parameter arrives at or is
     * moved to, nor anything live into the body. */
    if (g_t_ext) {
        unsigned busy = t_busy(&F, 0, 0);
        for (int v = 0; v < fn->nparams && v < fn->nvregs; v++)
            if (F.loc && F.loc[v] >= 0) {
                busy |= 1u << F.loc[v];
                if (F.wide[v])
                    busy |= 2u << F.loc[v];
            }
        t_roles_from(busy);
    }

    /* The register save area goes down FIRST, so it lands immediately
     * below the caller's stack arguments and a single pointer walks
     * from r0's copy straight into them. AAPCS32 needs no more than
     * that: a variadic argument is placed exactly like a named one. */
    if (fn->is_varargs)
        t_push(t, (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3));
    /* The mask is decided HERE, not patched at the end: its register
     * COUNT sets how far sp moves, which every stack-parameter offset
     * below is measured from. F.nsave is already known -- ra_allocate
     * ran before layout() -- so there is nothing left to discover. */
    /* A leaf with nothing to save: not even lr, which no call overwrites.
     * Only once a pass has shown the body touches none of r9-r11 (the
     * first pass saves all three), and only with no frame, no FPU saves,
     * no variadic save area and no VLA frame base to set up. */
    F.nopush = F.leaf && !F.nsave && !F.nfsave && !F.frame &&
               !fn->is_varargs && !fn->has_alloca &&
               !(F.scr_save & T_SCR_ALL);
    /* No IR_RET and no tail call is not enough: a void function's body
     * falls off its end into the epilogue, so its last instruction must
     * be a jump or a trap too. */
    F.noret = !F.nopush && !want_debug && !fn->is_varargs && fn->nins &&
              (fn->ins[fn->nins - 1].op == IR_JMP ||
               fn->ins[fn->nins - 1].op == IR_UD2);
    for (i = 0; F.noret && i < fn->nins; i++)
        if (fn->ins[i].op == IR_RET || (F.tail && F.tail[i]))
            F.noret = 0;
    push_at = F.nopush || F.noret ? -1
            : t_push(t, save_mask_for(F.nsave, F.used_callee, F.scr_save));
    if (F.nfsave && !F.noret)
        t_vsave(t, F.nfsave, 0);
    /* The mask is PATCHED at the end with whatever callee-saved
     * registers the allocator turned out to take: a `push` encodes them
     * as a bitmask, so growing the set costs no extra instruction, and
     * emitting the push before the body is what lets the frame layout be
     * decided first. This is what t_patch_push exists for. */
    if (F.frame)
        t_sp_adjust(t, F.frame, 1);
    if (fn->has_alloca) {
        t_mov_reg(t, 7, T_SP);             /* the frame base, from here on */
        F.fb = 7;
    }

    /* The parameters arrive in r0-r3 and on the stack above the saved
     * registers; the prologue writes each to its slot, which is what
     * every later reference reads. */
    {
        struct argplace pl;
        struct abi_walk w;
        /* float parameters the allocator put in a core register, and the
         * s register each arrives in: moved after the core parameters */
        int pvf_reg[RA_MAXPOOL], pvf_s[RA_MAXPOOL], npvf = 0;
        /* The caller's outgoing area, above everything this prologue
         * pushed. save_bytes_for is the ONE answer for how far that
         * push moves sp: the four fixed registers, the allocator's
         * callee-saved ones (pushed by the same instruction, since a
         * mask costs no extra push), and the r12 pad when the count
         * would otherwise be odd. Leaving any of them out of this sum
         * reads every stack parameter four bytes too low per register,
         * which is a miscompile in any function with more arguments
         * than the register file holds -- and the pad is exactly the
         * term that was missing. */
        long base = F.nopush ? 0 : F.noret ? F.frame
                  : F.frame + save_bytes_for(F.nsave, F.used_callee,
                                             F.scr_save) +
                    (long)F.nfsave * 4;
        int pmv_dst[RA_MAXPOOL], pmv_src[RA_MAXPOOL], npmv = 0;
        int pstk_reg[RA_MAXPOOL]; long pstk_off[RA_MAXPOOL];
        int npstk = 0;
        if (fn->is_varargs) {
            F.va_regsave = base;            /* r0-r3, four words */
            base += 16;                     /* ... then the stack ones */
        }
        if (F.sret_slot >= 0 &&
            !t_ldst_imm(t, T_R0, F.fb, F.sret_slot, 4, 0, 1)) {
            fb_addr(&F, T_ADDR, F.sret_slot);
            ldst_must(t, T_R0, T_ADDR, 0, 4, 0, 1);
        }
        walk_init(&w, F.sret_slot >= 0, fn->is_varargs, fn->pcs);
        for (i = 0; i < fn->nparams; i++) {
            struct ir_arg *a = &fn->param_abi[i];
            place_one(&w, a, &pl);
            /* A double with a d register home goes straight there, from
             * d0-d7, from a core pair (softfp, or a variadic function's
             * named one), or from its two stack words -- as a float with
             * an S register home does, below, and for the same reason. A
             * double is eight-aligned, so AAPCS32 never splits one between
             * r3 and the stack. */
            /* nothing names it: no slot (layout) and nothing to store */
            if (F.slot[i] < 0 && !in_reg(&F, i) && !in_freg(&F, i))
                continue;
            if (in_freg(&F, i) && F.wide[i]) {
                int dd = t_dreg(&F, i);
                if (pl.nvfp == 2 && pl.vdbl) {
                    t_vmov_reg(t, dd, pl.vfp / 2, 1);
                } else if (pl.nreg == 2 && !pl.nstk) {
                    t_vmov_core_pair(t, dd, pl.reg, pl.reg + 1, 1);
                } else if (!pl.nreg && pl.nstk == 2 && !pl.nvfp) {
                    long off = base + pl.stk;
                    if (!vfp_mem_d(&F, dd, F.fb, off, 0)) {
                        fb_addr(&F, T_ADDR, off);
                        t_vldst(t, dd, T_ADDR, 0, 1, 0);
                    }
                } else {
                    internal_error("thumb: %s: double parameter %d arrives "
                                   "in a shape a D register cannot take",
                                   fn->name, i);
                }
                continue;
            }
            /* A float parameter with an S-register home goes straight
             * there, from wherever it arrived. Nothing here writes a core
             * or an argument VFP register, so it cannot disturb another
             * parameter, and it has no slot to write. */
            if (in_freg(&F, i)) {
                int s = F.floc[i];
                if (pl.nvfp == 1) {
                    t_vmov_reg(t, s, pl.vfp, 0);
                } else if (pl.nreg == 1 && !pl.nstk) {
                    t_vmov_core(t, s, pl.reg, 1);
                } else if (!pl.nreg && pl.nstk == 1 && !pl.nvfp) {
                    long off = base + pl.stk;
                    if (off <= 1020) {
                        t_vldst(t, s, F.fb, (int)off, 0, 0);
                    } else {
                        fb_addr(&F, T_ADDR, off);
                        t_vldst(t, s, T_ADDR, 0, 0, 0);
                    }
                } else {
                    internal_error("thumb: %s: float parameter %d arrives "
                                   "in a shape an S register cannot take",
                                   fn->name, i);
                }
                continue;
            }
            /* Arrived in VFP registers (-mfloat-abi=hard). Reading them
             * disturbs no core register, so everything except a float
             * headed for a core register is done here; that one waits
             * for the core parameters to leave the register it may be
             * about to take. */
            if (pl.nvfp) {
                if (a->is_struct) {
                    fb_addr(&F, T_ADDR, F.slot[i]);
                    vfp_aggregate(&F, T_ADDR, pl.vfp,
                                  pl.vdbl ? pl.nvfp / 2 : pl.nvfp,
                                  pl.vdbl, 1);
                } else if (pl.vdbl) {
                    vfp_store_d(&F, i, pl.vfp / 2);
                } else if (in_reg(&F, i) && npvf < RA_MAXPOOL) {
                    pvf_reg[npvf] = F.loc[i];
                    pvf_s[npvf] = pl.vfp;
                    npvf++;
                } else {
                    vfp_store(&F, i, pl.vfp);
                }
                continue;
            }
            /* A parameter the allocator put in a REGISTER is one edge of
             * a PARALLEL MOVE, deferred until every parameter has been
             * placed: writing it here would destroy an incoming argument
             * another parameter has not read yet. This is the second of
             * the three sites regalloc.h names, and leaving it out is
             * what made vreg 0 alone enough to break the suite -- the
             * prologue stored the parameter to a slot nothing read.
             *
             * Only a SCALAR in one register: a pair is never allocated,
             * and a composite's slot IS the composite. */
            if (pl.nreg == 1 && pl.nstk == 0 && !a->is_struct &&
                a->size <= 4 && in_reg(&F, i)) {
                pmv_dst[npmv] = F.loc[i];
                pmv_src[npmv] = pl.reg;
                npmv++;
                continue;
            }
            /* A 64-bit parameter in a PAIR: each half an edge of the same
             * move, or a deferred load for a half that came on the stack
             * (t_pair_alloc keeps pairs out of variadic functions' r0-r3,
             * and their pl.reg is not an incoming register anyway). */
            if (a->size > 4 && !a->is_struct && in_reg(&F, i) &&
                pl.nreg + pl.nstk == 2 && !fn->is_varargs) {
                for (int q = 0; q < 2; q++) {
                    if (q < pl.nreg) {
                        pmv_dst[npmv] = F.loc[i] + q;
                        pmv_src[npmv] = pl.reg + q;
                        npmv++;
                    } else {
                        pstk_reg[npstk] = F.loc[i] + q;
                        pstk_off[npstk] = base + pl.stk + (long)(q - pl.nreg) * 4;
                        npstk++;
                    }
                }
                continue;
            }
            /* A scalar arriving ON THE STACK that the allocator put in a
             * register: a LOAD into it, deferred with the moves because
             * it writes a register another parameter may still be read
             * from.
             *
             * Easy to miss, and missing it is a miscompile rather than a
             * pessimisation: udivmod64(u64 a, u64 b, u64 *q, u64 *r) has
             * two eight-byte arguments, so r0-r3 are spent and `q`
             * arrives on the stack. The slot got written, every read went
             * to the register, and the register held b's high word. */
            if (pl.nreg == 0 && pl.nstk == 1 && !a->is_struct &&
                a->size <= 4 && in_reg(&F, i)) {
                pstk_reg[npstk] = F.loc[i];
                pstk_off[npstk] = base + pl.stk;
                npstk++;
                continue;
            }
            /* Every other parameter lands in its own local's slot, which
             * is where the body reads it. A composite's slot IS the
             * composite, so the words go straight into it. */
            for (int q = 0; q < pl.nreg; q++) {
                long off = F.slot[i] + (long)q * 4;
                int wid = (long)(q + 1) * 4 > a->size ? (a->size & 3) : 4;
                if (wid == 3) wid = 4;       /* a three-byte tail: store 4 */
                if (!t_ldst_imm(t, pl.reg + q, F.fb, off, wid == 4 ? 4 : wid,
                                0, 1)) {
                    fb_addr(&F, T_ADDR, off);
                    ldst_must(t, pl.reg + q, T_ADDR, 0, wid == 4 ? 4 : wid,
                               0, 1);
                }
            }
            for (int q = 0; q < pl.nstk; q++) {
                long src = base + pl.stk + (long)q * 4;
                long dst = F.slot[i] + (long)(pl.nreg + q) * 4;
                /* Through a low register the prologue has just pushed
                 * when there is one: nothing has been put in it yet --
                 * the parameters bound for r4-r7 move after this loop --
                 * and `ldr r4, [sp, #n]; str r4, [sp, #m]` is four
                 * bytes where the same through r12 is eight. */
                int ds = T_ACC;
                for (int k = 0; k < F.nsave && ds == T_ACC; k++)
                    if (F.used_callee[k] >= 4 && F.used_callee[k] < 8 &&
                        !(F.used_callee[k] == 7 &&
                          (F.fb == 7 || fn->has_alloca)))
                        ds = F.used_callee[k];
                fb_ld(&F, ds, src, 4, 0);
                if (!t_ldst_imm(t, ds, F.fb, dst, 4, 0, 1)) {
                    fb_addr(&F, T_ADDR, dst);
                    ldst_must(t, ds, T_ADDR, 0, 4, 0, 1);
                }
            }
        }
        /* The parallel move, now that every parameter has been placed.
         * T_SCR (r9) breaks a cycle: it is scratch, the prologue has
         * already saved it, and it holds nothing of its own yet. */
        if (npmv) {
            int od[RA_MAXPOOL * 2], os[RA_MAXPOOL * 2];
            int n = ra_parallel_move(pmv_dst, pmv_src, npmv, R_SCR, od, os,
                                     (int)(sizeof od / sizeof od[0]));
            if (n < 0)
                internal_error("thumb: %s: the prologue's parameter "
                               "placement is not a well-formed move",
                               fn->name);
            for (int k = 0; k < n; k++)
                t_mov_reg(t, pm_reg(od[k]), pm_reg(os[k]));
        }
        /* Then the loads: they only WRITE, so by now nothing still needs
         * the old contents of an argument register. */
        for (int k = 0; k < npstk; k++)
            if (!t_ldst_imm(t, pstk_reg[k], F.fb, pstk_off[k], 4, 0, 0)) {
                fb_addr(&F, T_ADDR, pstk_off[k]);
                ldst_must(t, pstk_reg[k], T_ADDR, 0, 4, 0, 0);
            }
        /* And the floats that arrived in s registers, into theirs. */
        for (int k = 0; k < npvf; k++)
            t_vmov_core(t, pvf_s[k], pvf_reg[k], 0);

        /* Where the first UNNAMED argument sits — which is simply where
         * the named ones stopped. The save area and the caller's stack
         * arguments are contiguous, so one expression covers both
         * cases: below four named words it is inside the save area, and
         * at four it is exactly its end, which is the stack. */
        if (fn->is_varargs)
            F.va_first = F.va_regsave + (long)w.ncrn * 4 + w.stk;
    }

    int tail_end = 0;        /* the body's last act is a tail call */
    int *ldepth = target_opt_size() ? (int *)0 : t_loop_depth(fn);
    g_t_loop_bytes = 0;
    for (i = 0; i < fn->nins; i++) {
        int was_tail = F.tail && F.tail[i] && F.nopush;
        int len0 = t->len, i0 = i;
        if (g_t_ext) {
            if (!F.lv_busy)
                g_t_role_fail = 1;     /* no liveness: nothing is known
                                        * free, so this attempt cannot be */
            t_roles_from(t_busy(&F, i, 2));
        }
        F.lofree = lo_free(&F, i);
        gen_ins(&F, i);
        F.lofree = 0;
        if (F.skip_next) {      /* the comparison emitted its branch too,
                                 * or a shift its consumer (and its load) */
            i += F.skip_next;
            F.skip_next = 0;
        }
        tail_end = i == fn->nins - 1 && was_tail;
        if (ldepth)
            g_t_loop_bytes += (long)(t->len - len0) *
                              (t_depth_weight(ldepth[i0]) - 1);
    }
    free(ldepth);

    t_roles_from(0);            /* nothing is live past the body */
    /* The epilogue -- its code only if something reaches it: not when the
     * body ended in a tail call and no IR_RET jumps here. The push mask
     * is patched either way. */
    F.label_off[fn->nlabels] = t->len;
    for (i = 0; tail_end && i < F.nfix; i++)
        if (F.fix[i].label == fn->nlabels)
            tail_end = 0;
    if (F.noret) {
        /* nothing reaches an epilogue, and nothing was pushed */
    } else if (tail_end) {
        if (!F.nopush)
            t_patch_push(t, push_at, save_mask_for(F.nsave, F.used_callee,
                                                   F.scr_save));
    } else {
    if (fn->has_alloca) {
        /* Release every VLA at once: sp back to the frame base, which
         * the `add sp` below then unwinds as usual. */
        t_mov_reg(t, T_SP, 7);
    }
    if (F.frame)
        t_sp_adjust(t, F.frame, 0);
    if (F.nfsave)
        t_vsave(t, F.nfsave, 1);
    {
        /* The same set the prologue pushed: SAVE_MASK plus whatever
         * callee-saved registers the allocator took. Built here and
         * patched into the push below, so the two cannot disagree. */
        unsigned mask = save_mask_for(F.nsave, F.used_callee, F.scr_save);
        if (F.nopush && !fn->cmse_entry)
            t_bx(t, T_LR);
        else if (!F.nopush)
            t_patch_push(t, push_at, mask);
        if (fn->cmse_entry) {
            /* lr back, then the clearing and BXNS */
            if (!F.nopush)
                t_pop(t, mask);
            cmse_entry_return(&F);
        } else if (F.nopush) {
            /* returned above */
        } else if (fn->is_varargs) {
            /* Return through lr rather than popping into pc: the four
             * words of register save area sit above the saved registers
             * and have to come off too, and `pop {..., pc}` would jump
             * before that. */
            t_pop(t, mask);
            t_sp_adjust(t, 16, 0);
            t_bx(t, T_LR);
        } else {
            t_pop(t, (mask & ~(1u << T_LR)) | (1u << T_PC));
        }
    }
    }                                   /* the epilogue */

    if (!t_lit64_flush(&F, pass))
        redo0 = 1;                  /* a constant out of reach: again */
    for (i = 0; i < F.nfix; i++) {
        int target = F.label_off[F.fix[i].label];
        if (target < 0) {
            fprintf(stderr, "embcc: internal: thumb: label %d of %s was "
                            "never placed\n", F.fix[i].label, fn->name);
            exit(1);
        }
        if (F.fix[i].cond == T_TAB) {
            code_patch32(t, F.fix[i].at, (unsigned long)(unsigned int)
                         ((target | 1) - F.fix[i].cz_at));
        } else if (F.fix[i].cond == T_LADDR) {
            /* the pair again, by the same encoder, over the placeholder */
            long v = t_isa_a32 ? (long)target - (F.fix[i].cz_at + 8)
                               : (long)(target | 1) - (F.fix[i].cz_at + 4);
            struct code pair;
            memset(&pair, 0, sizeof pair);
            t_mov_addr(&pair, F.fix[i].ins, (unsigned long)v & 0xffffffffUL);
            if (pair.len != F.fix[i].cz_at - F.fix[i].at)
                internal_error("thumb: %s: a label address changed size",
                               fn->name);
            memcpy(t->p + F.fix[i].at, pair.p, (size_t)pair.len);
            free(pair.p);
            free(pair.drange);
        } else if (F.fix[i].cond == T_TBH) {
            long d = (long)target - F.fix[i].cz_at;
            if (d < 0 || d > 2L * 65535 || (d & 1)) {
                /* A tbh entry is a halfword count: 128 KB forward and no
                 * more, which a large function at -O0 passes. On the
                 * first pass that switch is marked for the word table and
                 * the pass is made again -- the table is longer, so every
                 * other branch has to be measured over it. Later passes
                 * only shrink the code, so they cannot meet this. */
                /* 2: marked by this pass, whose other entries for the
                 * same table land here too. 1, marked by an earlier pass,
                 * would be a switch that ignored it: refused. */
                if (pass == 0 && F.fix[i].ins >= 0 &&
                    F.no_tbh[F.fix[i].ins] != 1) {
                    F.no_tbh[F.fix[i].ins] = 2;
                    redo0 = 1;
                    continue;
                }
                internal_error("thumb: %s: a tbh entry cannot reach its "
                               "label", fn->name);
            }
            t_patch_hw16(t, F.fix[i].at, (unsigned)(d / 2));
        } else if (F.fix[i].cond >= T_CBZ) {
            if (!t_patch_cbz(t, F.fix[i].at, target))
                internal_error("thumb: %s: a cbz no longer reaches its "
                               "label", fn->name);
        } else if (F.fix[i].sz == 2) {
            /* Measured to fit on the first pass, and nothing between
             * here and the target can have grown since. Checked anyway:
             * an offset that did not fit would be a jump elsewhere. */
            if (!(F.fix[i].cond < 0
                  ? t_patch_b16(t, F.fix[i].at, target)
                  : t_patch_bcond16(t, F.fix[i].at, target)))
                internal_error("thumb: %s: a relaxed branch no longer "
                               "reaches its label", fn->name);
        } else if (F.fix[i].cond < 0) {
            t_patch_b(t, F.fix[i].at, target);
        } else {
            long d = (long)target - F.fix[i].at - 4;
            /* past B<c>.W's reach: again, in far mode -- and skip the
             * rest that do not reach either, in this pass that asked */
            if ((d < -1048576L || d > 1048574L) && pass == 0 &&
                (!F.far_mode || redo0)) {
                F.far_mode = 1;
                redo0 = 1;
                continue;
            }
            t_patch_bcond(t, F.fix[i].at, target);
        }
    }

    /* After the first pass: which branches would the 16-bit form reach?
     * Measured from this pass's positions, where every branch is still
     * the wide form. On the second pass code between a branch and its
     * target can only get SHORTER, so what reached still reaches. */
    if (redo0) {                     /* a tbh became a word table: again */
        for (i = 0; i < fn->nins; i++)
            if (F.no_tbh[i] == 2)
                F.no_tbh[i] = 1;
        restarted = 1;
        pass = -1;
        continue;
    }
    if (pass == 0) {
        int any = 0;
        nshortb = F.nfix;
        shortb = xcalloc((size_t)(nshortb ? nshortb : 1), 1);
        for (i = 0; i < F.nfix; i++) {
            long d = (long)F.label_off[F.fix[i].label] - F.fix[i].at - 4;
            shortb[i] = F.fix[i].cond < 0 ? (d >= -2048 && d <= 2046)
                                          : (d >= -256 && d <= 254);
            /* ARM state: one branch form, and no cbz */
            if (t_isa_a32) {
                shortb[i] = 0;
                continue;
            }
            /* cbz: forward 0..126 from where the cmp stands, since the
             * two become the one instruction there -- and not to the
             * instruction right after the branch. Measured from the cmp,
             * a label just past a 4-byte bcond.w is 2 ahead; the cmp and
             * the branch then shrink to a 2-byte cbz, and the label is
             * the next instruction, -2 from the pc, which cbz cannot
             * encode. Code between the two only shrinks, never to
             * nothing, so one instruction there now is one later. */
            if (F.fix[i].cz_at >= 0) {
                long dz = (long)F.label_off[F.fix[i].label] -
                          F.fix[i].cz_at - 4;
                long gap = (long)F.label_off[F.fix[i].label] -
                           (F.fix[i].at + F.fix[i].sz);
                if (dz >= 0 && dz <= 126 && gap >= 2)
                    shortb[i] = 2;
            }
            any |= shortb[i];
            any |= shortb[i];
        }
        /* ...and which scratch registers it touched: only those need
         * saving. An asm cannot touch them: irgen refuses a template or
         * clobber naming r4-r11, and what its lowering uses goes through
         * t_scr like everything else. */
        {
            unsigned used = g_t_scr_used & T_SCR_ALL;
            if (!g_t_regalloc || (!any && used == T_SCR_ALL))
                break;
            F.scr_save = used;
        }
    } else if (pass == 1) {
        /* The second pass saved only what the first used. It emitted
         * the same body, so it should have used the same -- checked,
         * and a third pass with everything saved if it did not, rather
         * than return with a callee-saved register clobbered. */
        if (!(g_t_scr_used & T_SCR_ALL & ~F.scr_save))
            break;
        F.scr_save = T_SCR_ALL;
    }
    }                                   /* the passes */
    free(shortb);
    free(F.no_tbh);
    F.no_tbh = NULL;
    }

    f->code_len = t->len - f->code_off;
    /* What -fstack-usage reports: the registers the prologue pushed
     * plus everything sub sp reserved -- and a variadic function's
     * register save area, r0-r3, which it pushes first and apart from
     * the rest. Leaving those 16 bytes out made embrt's bound for a
     * program calling a variadic function 16 bytes short of what ran
     * (tests/golden/embrt.sh, floats2). */
    f->stack_bytes = (F.nopush ? 0 : F.noret ? (int)F.frame : (int)(F.frame + save_bytes_for(F.nsave, F.used_callee,
                                                    F.scr_save) +
                           (long)F.nfsave * 4)) +
                     (fn->is_varargs ? 16 : 0);
    free(F.usecnt);
    free(F.selimm);
    free(F.selimm_v);
    free(F.tail);
    free(F.slot);
    cg_note_labels(fn, F.label_off);
    free(F.label_off);
    free(F.fix);
    free(F.wide);
    free(F.nshr);
    free(F.loc);
    free(F.fvar);
    free(F.fscr);
    free(F.floc);
    free(F.lv_busy);
    free(F.lp_use);
    free(F.lp_val);
    free(F.lp_site);
}

/* With the allocator on and no FPU, a function is generated with the pair
 * pass and without it and the shorter kept: a pair withheld for the whole
 * function can cost the integer values more than it saves. A discarded
 * attempt is undone by truncating the code and the five site lists.
 * EMBCC_T_PAIRS=0/1 forces the choice, EMBCC_T_PAIRS_ONLY=fn limits the
 * pairs to one function -- so a pair path wrong only where it loses can
 * still be tested and bisected.
 *
 * Each of those is tried with the callee-saved registers renamed for the
 * 16-bit encodings (t_lowregs) and without. The rename's weights are an
 * estimate made before any code exists, and a few functions came out
 * larger by it (strtod's parse_hex by 36 bytes); trying both makes it a
 * choice the bytes decide. EMBCC_T_LOWREGS=0/1 forces it. */
static void gen_func_best(struct ir_func *fn, struct code *t,
                          struct t_sites *st, int want_debug)
{
    int at = t->len, ncall = st->ncall, next = st->next, nstr = st->nstr,
        ng = st->ng, nf = st->nf, nd = t->ndrange, with;
    const char *knob = getenv("EMBCC_T_PAIRS");
    const char *only = getenv("EMBCC_T_PAIRS_ONLY");

    /* A field's constant offset into its load or store (ldr r, [rn, #k])
     * -- once, before any attempt, and before allocation since the base's
     * live range grows.
     *
     * With FPv5-D16, the eight-byte accesses too: a double's vldr and
     * vstr take a word-scaled offset of up to 1020, and an array of
     * doubles walked in an unrolled loop was an addw before every one of
     * them. gen_ins64's LOAD and STORE read memoff (mem64_base), and one
     * whose offset vldr cannot hold goes the core way, ldrd or two ldr --
     * which reach the 4095 - 8 this allows. Only there, where the d
     * registers are what gains: elsewhere a 64-bit access keeps its
     * add, as it always has. */
    void (*gen)(struct ir_func *, struct code *, struct t_sites *, int) =
        target_thumb_arch() == 6 ? v6_gen_func : gen_func;
    if (g_t_regalloc && !want_debug && !g_t_o0 &&
        !getenv("EMBCC_NO_MEMOFF")) {
        int dp = target_thumb_fpu_dp();
        char *w = dp ? (char *)0 : wide64_map(fn);
        /* ARMv6-M's immediate offsets are five bits of the access size:
         * 124 for a word is the most any of them reaches. */
        ra_fold_memoff(fn, 0, target_thumb_arch() == 6 ? 124 : 4095, 4,
                       dp ? 8 : 4, w, 1, 31);
        free(w);
    }
    const char *lr = getenv("EMBCC_T_LOWREGS");
    int lr_forced = lr && *lr;
    g_t_pairs = 1;
    /* the rename is for the 16-bit encodings, which ARM state has none of */
    g_t_lowregs = lr_forced ? atoi(lr) != 0 : !t_isa_a32;
    if (!g_t_regalloc || want_debug || g_t_o0) {
        gen(fn, t, st, want_debug);
        g_t_lowregs = 1;
        return;
    }
    /* The attempts: pairs on and off (only without an FPU, and unless a
     * knob fixes them), each with the rename on and off (unless
     * EMBCC_T_LOWREGS fixes it). The smallest wins, the first of equal
     * ones -- at -Os by bytes, and otherwise by bytes plus the loop
     * bytes' extra weight (g_t_loop_bytes): a reload in a loop is two
     * bytes and runs every trip, while the push that saves r9-r11 for
     * it runs once, and choosing by bytes alone kept the loop's
     * constants in the frame where clang keeps them in r8 and r9.
     *
     * Before those, on ARMv7-M and ARMv8-M, the same pair choices with
     * r9-r11 in the pool too (g_t_ext; EMBCC_T_EXT=0/1 forces it), the
     * rename on. An attempt where some instruction needed a scratch role
     * that every one of r9-r11 was holding a value at (g_t_role_fail) is
     * not a candidate; the attempts without r9-r11 never fail that way.
     *
     * ARMv6-M has no rename to try: every register it computes in is a
     * low one already, so there is no 16-bit form to win (v6m.c does not
     * call t_lowregs), and a second attempt would make the same bytes. */
    int fixed_pairs = target_thumb_fpu() || (knob && *knob) || (only && *only);
    int pv[2], np = 0, lv[2], nl = 0;
    if (fixed_pairs) {
        pv[np++] = !(knob && *knob) || atoi(knob);
        if (only && *only)
            pv[0] = strcmp(only, fn->name) == 0;
    } else {
        pv[np++] = 1;
        pv[np++] = 0;
    }
    lv[nl++] = g_t_lowregs;
    if (!lr_forced && target_thumb_arch() != 6 && !t_isa_a32)
        lv[nl++] = 0;
    const char *ek = getenv("EMBCC_T_EXT");
    int ext_ok = target_thumb_arch() != 6 && !(ek && *ek && atoi(ek) == 0);
    /* EMBCC_T_EXT=1: an attempt with r9-r11 wins whenever one succeeds,
     * whatever its size, so tests can drive the path. */
    int ext_pref = ext_ok && ek && *ek && atoi(ek) != 0;
    /* (pairs, rename, r9-r11) for each attempt */
    int tp[12], tl[12], te[12], na = 0;
    if (ext_ok)
        for (int a = 0; a < np; a++) {
            tp[na] = pv[a]; tl[na] = lv[0]; te[na] = 1; na++;
        }
    for (int a = 0; a < np * nl; a++) {
        tp[na] = pv[a / nl]; tl[na] = lv[a % nl]; te[na] = 0; na++;
    }
    long best_score = 0;
    int best = -1, last = -1;
    for (int a = 0; a < na; a++) {
        if (a) {
            t->len = at; t->ndrange = nd; st->ncall = ncall; st->next = next;
            st->nstr = nstr; st->ng = ng; st->nf = nf;
        }
        g_t_pairs = tp[a];
        g_t_lowregs = tl[a];
        g_t_ext = te[a];
        g_t_role_fail = 0;
        g_t_loop_bytes = 0;
        gen(fn, t, st, want_debug);
        with = t->len - at;
        long score = with + g_t_loop_bytes;
        last = a;
        if (g_t_role_fail)
            continue;
        if (ext_pref && !te[a] && best >= 0 && te[best])
            continue;
        if (best < 0 || score < best_score ||
            (ext_pref && te[a] && !te[best])) {
            best = a;
            best_score = score;
        }
    }
    if (best != last || g_t_role_fail) {
        t->len = at; t->ndrange = nd; st->ncall = ncall; st->next = next;
        st->nstr = nstr; st->ng = ng; st->nf = nf;
        g_t_pairs = tp[best];
        g_t_lowregs = tl[best];
        g_t_ext = te[best];
        g_t_role_fail = 0;
        gen(fn, t, st, want_debug);
    }
    g_t_pairs = 1;
    g_t_lowregs = 1;
    g_t_ext = 0;
    t_roles_from(0);
}

void codegen_unit_thumb(struct ir_unit *iu, struct code *text,
                        struct extcall **ext, int *next,
                        struct strsite **strs, int *nstrs,
                        struct gsite **gs, int *ngs,
                        struct fsite **fs, int *nfs, int want_debug,
                        int optimize, int no_sse, int regalloc)
{
    (void)no_sse;
    g_t_regalloc = regalloc;
    g_t_o0 = !optimize;
    /* EMBCC_T_FPU=1: emit VFP for single-precision arithmetic.
     *
     * An environment variable and not -mfpu=, because -mfpu= is a
     * PROMISE about the object -- it implies the hard-float ABI, the
     * register class and Tag_ABI_VFP_args, none of which is finished.
     * Accepting the flag now would mean accepting it and emitting
     * something else, which is the failure this whole area is being
     * fixed for. The variable lets the arithmetic be exercised and
     * tested while the flag stays refused by name; it goes away when
     * -mfpu= can be honoured in full. */
    {
        /* Only when SET: unconditionally, this overwrote whatever the
         * driver had decided from -mfpu= -- to off. */
        const char *e = getenv("EMBCC_T_FPU");
        if (e)
            target_set_thumb_fpu(*e && *e != '0');
    }

    struct t_sites st;
    st.call = NULL; st.ncall = st.capcall = 0;
    st.ext = NULL;  st.next = st.capext = 0;
    st.str = NULL;  st.nstr = st.capstr = 0;
    st.g = NULL;    st.ng = st.capg = 0;
    st.f = NULL;    st.nf = st.capf = 0;

    for (int n = 0; n < iu->nfuncs; n++) {
        int ra = g_t_regalloc;
        if (g_t_o0 && ra_o0_too_big(&iu->funcs[n]))
            g_t_regalloc = 0;          /* see ra_o0_too_big */
        gen_func_best(&iu->funcs[n], text, &st, want_debug);
        g_t_regalloc = ra;
    }

    for (int n = 0; n < st.ncall; n++) {
        if (st.call[n].tail)          /* b.w, not bl: see t_tail_ok */
            t_patch_b(text, st.call[n].patch_off, st.call[n].target->code_off);
        else
            t_patch_bl(text, st.call[n].patch_off, st.call[n].target->code_off);
    }
    free(st.call);

    cg_resolve_strsites(iu, st.str, st.nstr);

    *ext = st.ext;   *next = st.next;
    *strs = st.str;  *nstrs = st.nstr;
    *gs = st.g;      *ngs = st.ng;
    *fs = st.f;      *nfs = st.nf;
}

/* ---- for v6m.c (cg.h) ------------------------------------------------------
 *
 * The ARMv6-M lowering shares AAPCS32, the frame layout, the site lists and
 * the allocator setup with this file. These hand them over unchanged, so
 * there is one copy of each. */
char *tcg_wide64_map(struct ir_func *fn) { return wide64_map(fn); }
void tcg_frame_addr_map(struct t_fn *F) { frame_addr_map(F); }
void tcg_layout(struct t_fn *F) { layout(F); }
void tcg_walk_init(struct abi_walk *w, int sret, int varargs, int pcs)
{
    walk_init(w, sret, varargs, pcs);
}
void tcg_place_one(struct abi_walk *w, const struct ir_arg *a,
                   struct argplace *p)
{
    place_one(w, a, p);
}
int tcg_call_sret_bytes(const struct ir_ins *i) { return call_sret_bytes(i); }
int tcg_fn_sret_bytes(const struct ir_func *fn) { return fn_sret_bytes(fn); }

/* ---- ARMv8-M's security extension: ACLE's CMSE, under -mcmse ------------
 *
 * Both lowerings share these: ARMv8-M Mainline here (cmse_entry_return,
 * cmse_call), ARMv8-M Baseline in v6m.c, the same shape in Thumb-1.
 *
 * A cmse_nonsecure_entry function returns to the Non-secure state with
 * BXNS lr. Before it, every register the AAPCS lets a callee leave
 * changed and that does not hold the result -- r0-r3 above it, and r12 --
 * is overwritten with lr (the return address, which the caller knows
 * already), and so are the flags, with an MSR of lr into APSR: a secret
 * the body computed must not be readable there. r4-r11 hold the caller's
 * values again after the epilogue's pop. This is clang's sequence for a
 * soft-float build. The floating-point state is not cleared: -mcmse is
 * refused with an FPU (src/driver), so Secure code built here never puts
 * anything in it.
 *
 * A call through a cmse_nonsecure_call pointer saves r4-r11, clears the
 * target's bit 0 (a Non-secure address: BLXNS to an odd one would stay
 * Secure), overwrites every register that is not an argument with the
 * target, and the flags, and branches with BLXNS; the callee's result
 * comes back in r0 (r0:r1), and r4-r11 are restored -- the Non-secure
 * callee is not trusted to preserve them. Mainline also saves and clears
 * the floating-point context around the call with VLSTM/VLLDM, as clang
 * does, which is a no-op where no Secure FP context is active.
 *
 * What CMSE forbids is refused by name, as clang refuses it: an entry
 * function with arguments on the stack or a result returned through
 * memory (the stack is the Non-secure caller's), and the same for a call
 * through a Non-secure pointer. */
void tcg_cmse_fail(const struct ir_func *fn, int line, const char *what)
{
    diag_fatal(fn->file, line ? line : fn->line, "%s '%s' %s",
               "cmse_nonsecure_entry function", fn->name, what);
}

/* The core registers the result occupies, r0 up: 0, 1 or 2. */
int tcg_cmse_ret_regs(const struct ir_func *fn)
{
    long sz = fn->ret_abi.size;
    if (!sz)
        return 0;
    return fn->ret_abi.is_struct ? 1 : (int)((sz + 3) / 4);
}

void tcg_cmse_check_entry(const struct ir_func *fn)
{
    struct abi_walk w;
    struct argplace pl;
    if (fn_sret_bytes(fn))
        tcg_cmse_fail(fn, 0, "would return its value through memory the "
                      "Non-secure caller owns (CMSE allows a result in r0-r3 "
                      "only)");
    walk_init(&w, 0, fn->is_varargs, fn->pcs);
    for (int k = 0; k < fn->nparams; k++)
        place_one(&w, &fn->param_abi[k], &pl);
    if (w.stk)
        tcg_cmse_fail(fn, 0, "requires arguments on the stack, which is the "
                      "Non-secure caller's (CMSE allows r0-r3 only)");
}

void tcg_cmse_check_call(const struct ir_func *fn, const struct ir_ins *i,
                         const struct abi_walk *w, long sret)
{
    if (sret || w->stk)
        diag_fatal(fn->file, i->line ? i->line : fn->line,
                   "a call through a cmse_nonsecure_call pointer in '%s' "
                   "%s, which is not supported: CMSE passes r0-r3 only",
                   fn->name, sret ? "returns its value through memory"
                                  : "passes arguments on the stack");
}

/* ARMv8-M Mainline: an entry function's way out, after the pop that put
 * the caller's registers and lr back (or none, in a function that pushed
 * nothing). */
static void cmse_entry_return(struct t_fn *F)
{
    struct code *t = F->t;
    for (int r = tcg_cmse_ret_regs(F->fn); r < 4; r++)
        t_mov_reg(t, r, T_LR);
    t_mov_reg(t, 12, T_LR);
    /* APSR_nzcvqg where the part has the DSP extension's GE bits, which
     * a SIMD instruction in the body could have set -- clang's choice
     * for the Cortex-M33 */
    t_msr_apsr(t, T_LR, target_thumb_em());
    t_bxns(t, T_LR, 0);
}

/* ARMv8-M Mainline: the target in T_ACC, the arguments in r0..r(ncrn-1). */
static void cmse_call(struct t_fn *F, int ncrn)
{
    struct code *t = F->t;
    t_push(t, 0x0ff0u);                            /* r4-r11 */
    if (!t_alu_imm(t, T_OP_BIC, T_ACC, T_ACC, 1, 0))
        internal_error("thumb: bic #1 does not encode");
    t_sp_adjust(t, 136, 1);
    t_vlstm(t, T_SP, 0);
    for (int r = ncrn; r < 12; r++)
        t_mov_reg(t, r, T_ACC);
    t_msr_apsr(t, T_ACC, target_thumb_em());
    t_bxns(t, T_ACC, 1);
    t_vlstm(t, T_SP, 1);
    t_sp_adjust(t, 136, 0);
    t_pop(t, 0x0ff0u);
}
long tcg_slot_of(const struct t_fn *F, int v) { return slot_of(F, v); }
int tcg_faddr(const struct t_fn *F, int v, long *off) { return faddr(F, v, off); }
void tcg_want_label(struct t_fn *F, int at, int label, int cond)
{
    want_label(F, at, label, cond);
}
void tcg_note_call(struct t_sites *st, int at, struct func *target)
{
    note_call(st, at, target);
}
void tcg_note_ext(struct t_sites *st, int at, struct func *callee)
{
    note_ext(st, at, callee);
}
void tcg_note_str(struct t_sites *st, int at, int idx, enum reloc_kind k)
{
    note_str(st, at, idx, k);
}
void tcg_note_glob(struct t_sites *st, int at, struct global *g,
                   enum reloc_kind k)
{
    note_glob(st, at, g, k);
}
void tcg_note_fn(struct t_sites *st, int at, struct func *target,
                 enum reloc_kind k)
{
    note_fn(st, at, target, k);
}
void tcg_call_helper(struct t_fn *F, const char *name) { call_helper(F, name); }
const char *tcg_fp_binop_name(enum ir_op op, int w) { return fp_binop_name(op, w); }
const char *tcg_fp_cmp_name(enum binop pred, int w) { return fp_cmp_name(pred, w); }
int tcg_cond_for(enum binop pred, int sign) { return cond_for(pred, sign); }
int *tcg_pair_alloc(struct ir_func *fn, const char *wide, const char *excl,
                    int *used, int *nused)
{
    return g_t_pairs ? t_pair_alloc(fn, wide, excl, used, nused) : NULL;
}
const struct ra_target *tcg_ra(void) { return &THUMB_RA; }
int tcg_regalloc(void) { return g_t_regalloc; }
int tcg_o0(void) { return g_t_o0; }
int tcg_pairs(void) { return g_t_pairs; }
void tcg_reset_taken(void) { g_t_taken = 0; }
