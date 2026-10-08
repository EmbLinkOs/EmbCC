/* RISC-V code generation for RV32IM and RV64IM (D-016).
 *
 * ONE backend for both widths, parameterised by target_xlen(). The
 * instruction set is the same at both -- `add` and `addw` differ by a
 * bit, the register file and the calling convention are the same shape --
 * and two copies of this file would drift. What differs is carried in
 * `F->xlen` and `F->w` (a register's size in bytes), and it means more
 * than a different mnemonic in exactly three places:
 *
 *   * a `long long` is a REGISTER PAIR at RV32 and a register at RV64,
 *     so the wide-value machinery below runs only at RV32;
 *   * a 32-bit operation at RV64 must leave its result SIGN-EXTENDED
 *     into the upper half -- the ABI's invariant for the whole register
 *     file -- which is what the `w` instruction forms are for;
 *   * a variadic 2*XLEN argument is aligned to an even register pair and
 *     a fixed one is not, which matters only at RV32.
 *
 * ---- the shape of the lowering ---------------------------------------
 *
 * Slot-based, like the Thumb backend and for the same reason (D-005,
 * prove it first): every vreg lives in a stack slot, every operation
 * loads its operands into scratch registers and stores its result back.
 * That is several times the instructions a register allocator would emit,
 * and it is obviously correct, which is what a new backend needs first.
 *
 * RISC-V makes the naive version cheaper than it was on ARM. t0-t6 are
 * seven caller-saved temporaries that are not argument registers, so this
 * file never borrows a callee-saved register and never pays for one in
 * the prologue -- where the Thumb backend had exactly one spare register
 * (r12) and had to buy its second.
 *
 * ---- what this file refuses -------------------------------------------
 *
 * By name, with the IR operation printed: inline assembly, atomics,
 * __int128, long double arithmetic, -g. THE RULE -- an
 * object full of plausible instructions that implement something else is
 * worse than no object.
 */
#include "emit.h"

#include "../backend.h"
#include "../regalloc.h"
#include "../target.h"
#include "../../driver/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct rv_fn;
static void copy_block(struct rv_fn *F, int copy, long size, int step);
static void copy_block_at(struct rv_fn *F, int copy, long size, int step,
                          int sreg, long soff, int dreg, long doff);

/* The scratch registers. Four named ones, because a 64-bit value at RV32
 * is a pair and a binary operation on two of them needs four; t5 and t6
 * stay spare for the few places that want a fifth. */
#define A_LO RV_T0
#define A_HI RV_T1
#define B_LO RV_T2
#define B_HI RV_T3
#define ACC  RV_T0      /* the value being computed */
#define TMP  RV_T1      /* the second operand */
#define ADDR RV_T2      /* an address */
#define SCR  RV_T4      /* a fourth, for when the other three are taken */
#define SCR2 RV_T5
/* Reserved for ONE job: holding sp + a large offset, and nothing else.
 *
 * It has to be a register no value ever lands in. An earlier version
 * picked "whichever of the scratches is not the one being moved", and
 * for a frame deeper than 2047 bytes that chose B_LO while loading B_HI
 * -- so reading the high half of a register pair destroyed the low half
 * it had just read. It only appeared in functions with enough locals to
 * push a slot past the reach of an sp-relative offset, which is why the
 * small tests all passed. */
#define FAR  RV_T6

/* ---- one- and two-byte atomics --------------------------------------
 *
 * The A extension has no byte or halfword forms, so a narrow atomic works
 * on the aligned word around it, as GCC's and LLVM's do: an AMO with the
 * other lanes neutral for AND, OR and XOR, and otherwise an LR/SC loop
 * that rewrites only this lane --
 *
 *   retry: lr.w   old, (aligned)
 *          new  = f(old)                   in this lane
 *          merged = old ^ ((new ^ old) & mask)
 *          sc.w   fail, merged, (aligned)
 *          bnez   fail, retry
 *
 * which is atomic against the neighbouring bytes too: a write to any of
 * them between the lr and the sc breaks the reservation, and the loop
 * starts again with their new values. Little-endian, so the lane of
 * address a is bits 8*(a & 3) up. The registers: SUB_OLD the word read,
 * SUB_SH the lane's shift, SUB_AL the aligned address, SUB_MK the lane's
 * mask, and FAR for what each step computes -- no slot is touched inside
 * the loop, so FAR's own job does not arise until it is over. */
#define SUB_OLD RV_T0
#define SUB_V   RV_T1
#define SUB_V2  RV_T2
#define SUB_SH  RV_T3
#define SUB_AL  RV_T4
#define SUB_MK  RV_T5

/* SUB_SH, SUB_AL and SUB_MK for an aw-byte lane at `addr` */
static void sub_lane(struct code *t, int addr, int aw, int xlen)
{
    rv_alu_imm(t, RV_AND, SUB_SH, addr, 3, 0);
    rv_shift_imm(t, RV_SLL, SUB_SH, SUB_SH, 3, 0, xlen);
    rv_alu_imm(t, RV_AND, SUB_AL, addr, -4, 0);
    rv_li(t, SUB_MK, aw == 1 ? 0xff : 0xffff, xlen);
    rv_alu(t, RV_SLL, SUB_MK, SUB_MK, SUB_SH, 0);
}

/* reg = (src << shift) & mask: a value moved into the lane */
static void sub_in(struct code *t, int reg, int src)
{
    rv_alu(t, RV_SLL, reg, src, SUB_SH, 0);
    rv_alu(t, RV_AND, reg, reg, SUB_MK, 0);
}

/* SUB_OLD = the lane of SUB_OLD, at bit 0, extended as `sign` says */
static void sub_out(struct code *t, int aw, int sign, int xlen)
{
    rv_alu(t, RV_AND, SUB_OLD, SUB_OLD, SUB_MK, 0);
    rv_alu(t, RV_SRL, SUB_OLD, SUB_OLD, SUB_SH, 0);
    if (sign) {
        int k = xlen - 8 * aw;
        rv_shift_imm(t, RV_SLL, SUB_OLD, SUB_OLD, k, 0, xlen);
        rv_shift_imm(t, RV_SRA, SUB_OLD, SUB_OLD, k, 0, xlen);
    }
}

/* merged = old ^ ((new ^ old) & mask), then sc and retry from `top` */
static void sub_commit(struct code *t, int new_reg, int top)
{
    rv_alu(t, RV_XOR, FAR, new_reg, SUB_OLD, 0);
    rv_alu(t, RV_AND, FAR, FAR, SUB_MK, 0);
    rv_alu(t, RV_XOR, FAR, FAR, SUB_OLD, 0);
    rv_amo(t, RV_SC, FAR, SUB_AL, FAR, RV_ORD_RL, 0);
    int br = rv_b_placeholder(t, RV_BNE, FAR, RV_ZERO);
    rv_patch_b(t, br, top);
}

struct rv_sites {
    struct { int patch_off; struct func *target; int jal, tail; } *call;
    int ncall, capcall;
    struct extcall *ext;   int next, capext;
    struct strsite *str;   int nstr, capstr;
    struct gsite *g;       int ng, capg;
    struct fsite *f;       int nf, capf;
};

struct rv_fn {
    /* Comparison/branch fusion: read counts per vreg, so a comparison
     * whose only reader is the branch after it becomes ONE branch
     * instruction. RISC-V branches compare two registers directly, so
     * `if (a < b)` is a single `blt` -- materialising 0 or 1 and then
     * testing it against zero was three instructions and a register.
     * skip_next tells the dispatch loop the branch is already out. */
    int *usecnt;
    int skip_next;
    int want_debug;
    struct ir_func *fn;
    /* Per vreg: the register the allocator gave it, or -1 for one that
     * stays in memory. NULL when the allocator did not run (-O0/-O1),
     * which is what makes every helper below fall back to the slot
     * path the backend had before it existed. */
    int *loc;
    /* Per vreg: the FLOATING-POINT register the FP pass gave it (f0-f31),
     * or -1; NULL without an FPU or the allocator. A value has one home:
     * loc, floc or a slot. `fw` says how wide a value with an FP home is
     * -- 4 a float, 8 a double -- which is what every crossing between
     * the register files reads. */
    int *floc;
    char *fw;
    int fused[32], nfsave;        /* the callee-saved FP registers it took */
    long fsave_at;                /* ...and where the prologue saves them */
    /* RV32 with D: eight bytes of frame a double crosses between an f
     * register and an integer pair through (fsd, two lw), there being no
     * 64-bit fmv at RV32 -- clang does the same. -1 when unneeded. */
    long fx;
    /* fx is first left out (fx_lazy) and the function emitted without
     * it: most functions that might cross a double never do, and the
     * slot cost them a frame. A crossing that finds none sets fx_missed,
     * and gen_func emits the function again with it. */
    int fx_lazy, fx_missed;
    /* Staging for a value in an fa register an x register must receive
     * at a call or in the prologue: the f registers' parallel move may
     * overwrite it before the x registers' runs, so it is stored here
     * first (fstage, eight bytes each) and loaded from here after. The
     * values staged at the call being set up: stg_v[k] at stg_off[k]. */
    long fstage;
    int stg_v[MAX_PARAMS], nstg;
    long stg_off[MAX_PARAMS];
    int used_callee[RA_MAXPOOL];  /* the callee-saved ones it took */
    int pair_used[9], npair;      /* callee-saved pairs rv_pair_alloc took */
    int nsave;
    struct code *t;
    struct rv_sites *st;
    int xlen;            /* 32 or 64 */
    int w;               /* a register in bytes: 4 or 8 */
    char *wide;          /* per vreg: needs a register pair (RV32 only) */
    char *w16;           /* per vreg: an __int128 or a long double, in a
                          * sixteen-byte slot (rv_w16_map) */
    long tfa;            /* RV32: 48 bytes for a long double helper's
                          * by-reference operands (gen_ld32), or -1 */
    char *nshr;          /* per vreg: a narrow high-word shift (narrow_shr) */
    char *sx;            /* per vreg, RV64 only: already the sign-extension
                          * of its low 32 bits (sext_map) */
    char *f4;            /* per vreg, RV64: a float whose slot fdone writes
                          * four bytes of (slot_bytes) */
    long *slot;          /* per-vreg byte offset from sp, -1 for none */
    long frame;          /* total bytes sp moves down by */
    long scratch_at;     /* where fn->scratch_bytes begins */
    long byref_at;       /* where the by-reference argument copies go */
    long sret_slot;      /* where the hidden result pointer is kept, or -1 */
    long ra_slot;        /* where the return address is saved */
    /* The register every frame slot is addressed from: sp, except in a
     * function with a variable-length array, where sp moves at run time
     * and s0 holds the frame base (see IR_ALLOCA). */
    int fb;
    long out_bytes;      /* the outgoing-argument area, at the live sp */
    /* Makes no call -- none in the IR and none to a runtime helper -- so
     * ra is never overwritten and needs no slot, save or restore. With
     * nothing else in the frame, the function touches sp not at all. */
    int leaf;
    /* Per instruction: an IR_CALL made as a TAIL call (rv_tail_ok) --
     * the epilogue's restores, then a jump, with the IR_RET after it
     * never reached. NULL when there are none. */
    char *tail;
    long save_at;        /* ... and the allocator's callee-saved ones */
    long va_regsave;     /* a variadic function's a0-a7 spill area, or -1 */
    long va_first;       /* ... and the offset of the first UNNAMED one */
    int *label_off;      /* per label id, or -1 while unseen */
    /* A jump or branch to a label. `kind` is its form (FX_*); for the
     * long form `bat` is where its branch-over begins, which is where a
     * direct branch would sit. */
    struct { int at; int label; int kind; int cond, rs1, rs2; int bat; } *fix;
    /* Branch relaxation: per jump or branch, in emission order, the form
     * the first pass measured would reach. NULL on the first pass, which
     * emits every one in its longest form. */
    const signed char *relax;
    int nrelax;
    int nfix, capfix;
    /* An interrupt handler (rv_isr_grow): ISR_INTERRUPT or ISR_SUPERVISOR,
     * 0 for an ordinary function. isr_x and isr_f are the caller-saved
     * registers it saves, as bit masks -- every one it writes, or all of
     * them when it calls -- in isr_bytes at the top of the frame, from
     * isr_at. */
    int isr;
    unsigned long isr_x, isr_f;
    long isr_at, isr_bytes;
};

/* ---- the register allocator's view of this machine ---------------------
 *
 * RISC-V hands the allocator more registers than either of the other
 * embedded targets could dream of: eight argument registers, seven
 * temporaries and twelve saved ones. Six are held back as scratch
 * because the slot paths still need somewhere to land a value, and t3
 * is the only temporary left over.
 *
 * Caller-saved FIRST in the preference order, which is what regalloc.h
 * asks for: a short-lived value takes one and the prologue never has to
 * save it.
 */
#define RV_NPOOL 21
static const int RV_POOL[RV_NPOOL] = {
    /* caller-saved: a0-a7, then the one spare temporary */
    RV_A0, RV_A1, RV_A2, RV_A3, RV_A4, RV_A5, RV_A6, RV_A7, RV_T3,
    /* callee-saved: s0, s1, s2-s11. s0 and s1 first: they are x8 and
     * x9, the only callee-saved registers the compressed loads, stores
     * and ALU forms reach.
     *
     * s0 was left out as "the frame pointer", which this backend does not
     * keep: every slot is addressed from sp, and DWARF's frame base is sp
     * too (src/debug/dwarf.c), except in a function with a variable-
     * length array, where s0 holds the frame base and rv_pool_for takes it
     * out. Everywhere else it sat unused -- not one access in lib/libc
     * went through it -- while clang hands it out like s1. */
    RV_FP, RV_S1, RV_S2, RV_S2 + 1, RV_S2 + 2, RV_S2 + 3, RV_S2 + 4,
    RV_S2 + 5, RV_S2 + 6, RV_S2 + 7, RV_S2 + 8, RV_S2 + 9
};
/* The same list with the argument file removed, for a variadic
 * function: its prologue spills a0-a7 into the register save area and
 * `va_arg` walks them, so those eight are not the allocator's to give.
 */
static const int RV_POOL_VA[RV_NPOOL - 8] = {
    RV_T3,
    RV_FP, RV_S1, RV_S2, RV_S2 + 1, RV_S2 + 2, RV_S2 + 3, RV_S2 + 4,
    RV_S2 + 5, RV_S2 + 6, RV_S2 + 7, RV_S2 + 8, RV_S2 + 9
};

/* Registers the RV32 pair pass (rv_pair_alloc) took for the whole
 * function, withheld from the ordinary pool; bit r for xr. */
static unsigned long g_rv_taken;
/* Whether this attempt at a function uses the pair pass (gen_func_best). */
static int g_rv_pairs = 1;
static int g_rv_pool[RV_NPOOL];

static const int *rv_pool_for(const struct ir_func *fn, int *n)
{
    const int *p = fn->is_varargs ? RV_POOL_VA : RV_POOL;
    int np = fn->is_varargs ? RV_NPOOL - 8 : RV_NPOOL, k = 0;
    unsigned long out = g_rv_taken;
    /* t3 is also B_HI, the scratch every RV32 64-bit lowering loads its
     * second operand's high word into -- so it is no home in a function
     * that has one. It was, and a pointer held in t3 across a 64-bit
     * compare came back as the compare's zero (softfp.c's unpack, once
     * the pair pass raised the pressure enough to hand t3 out). */
    if (target_xlen() == 32)
        for (int m = 0; m < fn->nins; m++)
            if (fn->ins[m].w == 8) {
                out |= 1UL << RV_T3;
                break;
            }
    /* A VLA's function addresses its frame from s0 (IR_ALLOCA). */
    if (fn->has_alloca)
        out |= 1UL << RV_FP;
    if (!out) {
        *n = np;
        return p;
    }
    for (int j = 0; j < np; j++)
        if (!(out >> p[j] & 1))
            g_rv_pool[k++] = p[j];
    *n = k;
    return g_rv_pool;
}

/* The PAIR pool at RV32, each pair named by its low register: the
 * argument pairs first, where a double is passed, returned and handed to
 * every helper, then s2:s3 up to s10:s11 for one that lives across a call
 * (the allocator keeps those off caller-saved registers). A variadic
 * function's prologue owns a0-a7, so it gets the callee-saved ones only. */
#define RV_NPAIRS 9
static const int RV_PAIRS[RV_NPAIRS] = {
    RV_A0, RV_A0 + 2, RV_A0 + 4, RV_A0 + 6,
    RV_S2, RV_S2 + 2, RV_S2 + 4, RV_S2 + 6, RV_S2 + 8
};
static const int *rv_pair_pool_for(const struct ir_func *fn, int *n)
{
    if (fn->is_varargs) {
        *n = RV_NPAIRS - 4;
        return RV_PAIRS + 4;
    }
    *n = RV_NPAIRS;
    return RV_PAIRS;
}

/* Where the psABI puts each 64-bit value, for the pair pass: a
 * parameter in its argument pair, a result and a returned value in a0:a1,
 * a helper's operands in a0:a1 and a2:a3. */
static void rv_pair_hints(const struct ir_func *fn, int *hint);

/* s0-s11: x8, x9 and x18-x27. */
static int rv_callee_saved(int r)
{
    return r == RV_FP || r == RV_S1 || (r >= RV_S2 && r <= RV_S2 + 9);
}

/* Is `dst = load(local)` a plain move here -- no extension emitted?
 *
 * At the full register width, always. And at RV64, a four-byte SIGNED
 * read at four-byte width, which is most of what integer code does.
 *
 * That second one is only sound because IR_STVAR below SIGN-extends a
 * four-byte store rather than zero-extending it -- the two rules have
 * to agree about what a register holding a narrow local contains, and
 * this is the pair that makes the common case free on both sides.
 *
 * What makes the choice safe either way is that only the low `size`
 * bytes carry the value: an UNSIGNED read still emits its own
 * slli/srli and gets the right answer whatever the upper bits were.
 * The extension form on the store decides only which reads are free,
 * never which are correct. Storing sign-extended is also the ABI's own
 * invariant for a 32-bit value in a 64-bit register, so a parameter
 * arriving in a0 already satisfies it. */
static int rv_ldvar_plain(int size, int sign, int w)
{
    int wb = target_ptr_size();
    if (size == wb && w == wb)
        return 1;
    return target_xlen() == 64 && size == 4 && sign && w == 4;
}

/* Which instructions become a CALL the IR does not show as one. A value
 * live across one of these may not sit in a caller-saved register.
 *
 * On this target that is nearly all of floating point: there is no F and
 * no D extension under -march=rv32im/rv64im, so every arithmetic
 * operation on a float or a double is a libgcc call. Answering this
 * wrong is invisible until a float program is optimized, which is
 * exactly where the parked ARMv7-M attempt went wrong. */
/* Does this instruction run on the FPU? With F, the single-precision
 * arithmetic, comparisons, square root and the conversions between a float
 * and an integer the machine has a register for (RV32's 64-bit integers
 * are still __floatdisf and __fixsfdi); with D the same for double and the
 * conversions between the two. Everything else that touches floating
 * point stays a call -- double under F alone, long double always.
 *
 * rv_op_calls_helper asks this first, so the allocator's idea of which
 * instructions are calls is exactly the lowering's. */
static int rv_fp_width_hw(int w)
{
    int flen = target_riscv_flen();
    return (w == 4 && flen >= 32) || (w == 8 && flen == 64);
}

static int rv_fp_hw(const struct ir_ins *i)
{
    if (!target_riscv_flen())
        return 0;
    switch (i->op) {
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_NEG:
    case IR_CMP: case IR_SQRT:
        return i->flt && rv_fp_width_hw(i->w);
    case IR_I2F:
        return rv_fp_width_hw(i->w) &&
               (i->size <= 4 || (i->size == 8 && target_xlen() == 64));
    case IR_F2I:
        return rv_fp_width_hw(i->size) &&
               (i->w <= 4 || (i->w == 8 && target_xlen() == 64));
    case IR_F2F:
        return target_riscv_flen() == 64 &&
               ((i->size == 4 && i->w == 8) || (i->size == 8 && i->w == 4));
    default:
        return 0;
    }
}

int rv_op_calls_helper(const struct ir_ins *i)
{
    if (rv_fp_hw(i))
        return 0;
    if (i->flt)
        return i->op == IR_ADD || i->op == IR_SUB || i->op == IR_MUL ||
               i->op == IR_DIV || i->op == IR_CMP;
    if (i->op == IR_I2F || i->op == IR_F2I || i->op == IR_F2F)
        return 1;
    /* A 64-bit divide is __divdi3 at RV32 and a single instruction at
     * RV64 -- the same IR operation, a call on one width and not the
     * other, which is the sort of thing one backend for two machines
     * has to keep asking rather than deciding once. */
    if (target_xlen() == 32 && (i->op == IR_DIV || i->op == IR_MOD) &&
        i->w == 8)
        return 1;
    /* ...and at RV64 a 128-bit divide is __divti3, and a shift may be
     * __ashlti3 (gen_ins128 does a constant count inline, which this does
     * not look for: answering yes where no call is made costs a register,
     * answering no where one is made costs a value). */
    return target_xlen() == 64 && i->w == 16 && !i->flt &&
           (i->op == IR_DIV || i->op == IR_MOD || i->op == IR_SHL ||
            i->op == IR_SHR);
}

/* Where the psABI would put each value (below place_arg, whose answer
 * it uses). */
static void rv_abi_hints(const struct ir_func *fn, int *hint);

/* ---- the floating-point register class (F and D) --------------------------
 *
 * ft0-ft2 are the scratch every FP lowering computes in, as t0-t2 are for
 * the integer one. The class is ft3-ft6 and fa0-fa7, caller-saved and
 * first, then fs0-fs11 -- twenty-four, the allocator's most (RA_MAXPOOL);
 * ft7-ft11 are left over. fa0-fa7 are in it so that a float argument can
 * live where it arrives and a result be computed where it is returned
 * (rv_fp_hints): a call's argument setup and a prologue's parameter
 * placement are then PARALLEL MOVES among the f registers, as they are
 * among the x ones.
 *
 * The fs registers survive a call only under a hardware-float ABI, and
 * only as wide as that ABI says: under ilp32/lp64 no FP register is
 * preserved (a soft-float caller knows nothing of them), and under
 * ilp32f/lp64f with D only the low 32 bits are -- so there, with values of
 * both widths in one class, none is treated as callee-saved. clang
 * spills across the call in both cases too. */
#define RV_NFPOOL 24
static const int RV_FPOOL[RV_NFPOOL] = {
    3, 4, 5, 6,                                  /* ft3-ft6 */
    10, 11, 12, 13, 14, 15, 16, 17,              /* fa0-fa7 */
    8, 9, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27  /* fs0, fs1, fs2-fs11 */
};
static int is_fa(int r) { return r >= RV_FA0 && r < RV_FA0 + 8; }
static const int *rv_fp_pool_for(const struct ir_func *fn, int *n)
{
    (void)fn;
    *n = target_riscv_flen() ? RV_NFPOOL : 0;
    /* EMBCC_RA_MAXPOOL squeezes this class too (regalloc.c squeezes the
     * integer one), so the exec goldens reach the paths that read an FP
     * value from its slot beside ones in f registers. */
    if (*n && getenv("EMBCC_RA_MAXPOOL")) {
        int m = atoi(getenv("EMBCC_RA_MAXPOOL"));
        if (m >= 0 && m < *n)
            *n = m;
    }
    return RV_FPOOL;
}
static int rv_fp_callee_saved(int r)
{
    int abi = target_riscv_abi_flen();
    if (!abi || abi < target_riscv_flen())
        return 0;
    return r == 8 || r == 9 || (r >= 18 && r <= 27);
}

static const struct ra_target RISCV_RA = {
    rv_pool_for,
    rv_callee_saved,
    rv_ldvar_plain,
    /* A scalar call argument may come from a register (see gen_call's
     * set_args: the setup is a PARALLEL MOVE, and the allocator keeps
     * struct arguments in memory regardless, so only the scalars need
     * ordering). A returned value and a memcpy's addresses may too --
     * each is a single destination, or two that are non-argument
     * scratches, so neither can destroy the other's source.
     *
     * They went on one at a time, each with the full matrix. The parked
     * ARMv7-M attempt turned all three on in the same commit as the
     * allocator itself and had four bugs interacting with no way to
     * tell them apart. */
    1, 1, 1,
    rv_op_calls_helper,
    0,            /* RISC-V is three-operand: d = a op b needs no copy */
    rv_abi_hints,
    /* The FP class, with F or D; empty without, when a float is bits in
     * a core register and allocated with them (float_in_gpr). Every
     * float lowering reaches a value wherever it lives -- an f register,
     * an x register or a slot -- through rd/wr and fsrc/fdone. */
    rv_fp_pool_for, rv_fp_callee_saved,
    1,
    NULL, NULL,
    1,            /* atomic_in_reg: every atomic reads its address and
                   * values through rdr and writes through wreg/wr */
    0,            /* fp_reads_gpr: floats are already general (above) */
    1             /* asm_in_reg: see IR_ASM */
};

/* -O2 and -Os: the allocator is on. */
static int g_rv_regalloc;
/* -O0: the allocator runs for the temporaries of each expression only,
 * every source variable pinned to its slot as under -g; see thumb's
 * g_t_o0. */
static int g_rv_o0;

/* ---- refusal ---------------------------------------------------------- */

static void rv_refuse(const struct rv_fn *F, const struct ir_ins *i,
                      const char *what)
{
    char op[64];
    op[0] = 0;
    if (i)
        snprintf(op, sizeof op, " [%s w=%d size=%d]", ir_opname(i->op),
                 i->w, i->size);
    fprintf(stderr,
            "embcc: %s:%d: error: the RV%d backend cannot lower %s yet "
            "(function %s)%s\n",
            F->fn->file ? F->fn->file : "?",
            i ? i->line : F->fn->line, F->xlen, what, F->fn->name, op);
    exit(1);
}

/* ---- which values are eight bytes wide ---------------------------------
 *
 * At RV32 that means a REGISTER PAIR and an eight-byte slot; at RV64 it
 * is one register, and the map is read only by the conversions -- which
 * have to tell a genuinely 64-bit source from a 32-bit one irgen asked
 * to be widened, and cannot do that from the instruction alone.
 *
 * By the WIDTH OF THE RESULT -- `i->w` for the value-producing operations
 * and the register width for everything else however wide its operands
 * are. IR_CMP at w == 8 compares two 64-bit values and yields a 0 or a 1,
 * and IR_ADDR yields a pointer whatever it points at; treating either as
 * wide gives it an eight-byte slot and reads its neighbour as a high
 * word.
 */
static char *wide_map(struct ir_func *fn)
{
    char *w = xcalloc((size_t)(fn->nvregs ? fn->nvregs : 1), 1);
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        /* `flt` is NOT a reason to skip: a double is eight bytes and a
         * register pair exactly as a long long is. Skipping them gave
         * every double-returning call a four-byte slot on the Thumb
         * backend, and the next temporary landed on its high word. */
        if (i->w != 8 || i->dst < 0 || i->dst >= fn->nvregs)
            continue;
        switch (i->op) {
        case IR_CONST: case IR_MOV:
        case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
        case IR_AND: case IR_OR: case IR_XOR: case IR_SHL: case IR_SHR:
        case IR_NEG: case IR_BNOT:
        case IR_LDVAR: case IR_LOAD: case IR_EXT: case IR_CALL:
        case IR_SELECT: case IR_BSWAP:
        case IR_MULW:             /* two 32-bit operands, a 64-bit result */
        /* The conversions' `w` is their RESULT's width too. */
        case IR_I2F: case IR_F2I: case IR_F2F: case IR_BITCAST:
            w[i->dst] = 1;
            break;
        default:
            break;
        }
    }
    /* A local declared eight bytes wide is one whether or not an
     * instruction has been seen to define it: the prologue writes a
     * parameter into its slot before the body runs. */
    for (int v = 0; v < fn->nvars && v < fn->nvregs; v++)
        if (fn->locals[v].size == 8 &&
            (fn->locals[v].is_int_or_ptr || fn->locals[v].is_scalar_float))
            w[v] = 1;

    /* Then through COPIES, to a fixed point. A MOV is not required to
     * carry a width and often does not -- the merge of a `?:`'s two arms
     * is emitted with an operand and a destination and nothing else,
     * which cost nothing while every register was 64 bits. Reading `w`
     * there says four, and `neg ? -q : q` returns half of a long long,
     * the other half being whatever its neighbour held. */
    for (int again = 1; again;) {
        again = 0;
        for (int n = 0; n < fn->nins; n++) {
            const struct ir_ins *i = &fn->ins[n];
            int src;
            if (i->dst < 0 || i->dst >= fn->nvregs || w[i->dst])
                continue;
            /* ...but only a copy that does not SAY four bytes.
             *
             * `%d = mov.4s %s` with an eight-byte %s is a narrowing
             * copy -- it takes the low word -- and marking %d wide for
             * it makes the other arm of the same `?:` an eight-byte
             * read of a four-byte value. That is harmless while
             * everything lives in memory and the high word is merely
             * garbage nobody reads; it is a miscompile the moment the
             * four-byte arm gets a REGISTER, because then the slot the
             * pair is read from was never written at all.
             *
             * `fits(d) ? (int)d : 0` is exactly that shape, and it is
             * what this cost to find. A width-less MOV still
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

/* The vregs holding a sixteen-byte value at RV64 -- an __int128 or a long
 * double: cg_wide_vregs, the map the x86-64 and AArch64 backends share,
 * less any LOCAL that is not itself sixteen bytes. That map closes over
 * ldvar and stvar in both directions, so `long y = (long)x;` marks y --
 * whose slot is laid out from its declared size, eight bytes, which a
 * sixteen-byte copy would overrun. A narrowing stvar stores its `size`
 * and needs no mark; the temps such a local is read into stay marked and
 * only waste eight bytes of slot. */
static char *rv_w16_map(struct ir_func *fn)
{
    char *w = cg_wide_vregs(fn);
    if (!w)
        return NULL;
    for (int v = 0; v < fn->nvars && v < fn->nvregs; v++)
        if (fn->locals[v].size != 16)
            w[v] = 0;
    return w;
}

static int is16(const struct rv_fn *F, int v)
{
    return F->w16 && v >= 0 && v < F->fn->nvregs && F->w16[v];
}

/* ---- the calling convention --------------------------------------------
 *
 * Every rule here was read off clang for the triple rather than reasoned
 * out, and three of them are NOT what AAPCS32 taught:
 *
 *   * a fixed 2*XLEN scalar is NOT aligned to an even register pair. On
 *     RV32, f(int, long long) passes the long long in a1:a2, odd-first.
 *   * a VARIADIC one IS: v(0, 1, 2, x) skips a3 and lands in a4:a5, and
 *     the callee's va_arg rounds its walking pointer up to 8 first. The
 *     same type has two rules in one function depending on which side of
 *     the `...` it is on.
 *   * an aggregate of at most 2*XLEN bytes travels PACKED IN REGISTERS,
 *     by bytes and not field by field, and splits across the register and
 *     stack boundary like anything else. A LARGER one goes by reference --
 *     always, never conditionally on registers being free -- and the
 *     CALLER owns the copy, because the callee may write to its
 *     parameter.
 */
struct argplace {
    int reg, nreg, nstk;
    long stk;            /* offset in the outgoing area */
    int byref;
    long copy;           /* where the caller's private copy lives */
    /* The hardware floating-point convention (place_one): `hf` set means
     * none of the above applies -- nreg and nstk are 0 -- and the value
     * travels as `nfld` fields, each an f register (fa0 + reg) or, for the
     * integer half of a float-and-integer struct, an x register (a0 +
     * reg), read from `off` bytes into the object, `size` bytes wide. */
    int hf, nfld;
    struct { int fp, reg, size; long off; } fld[2];
};

/* ---- the hardware floating-point calling convention ----------------------
 *
 * ilp32f/lp64f and ilp32d/lp64d (the psABI's "Hardware Floating-point
 * Calling Convention"), with ABI_FLEN 32 or 64 -- target_riscv_abi_flen:
 *
 *   * a float no wider than ABI_FLEN goes in the next of fa0-fa7, and once
 *     those are gone by the integer rules, as under ilp32/lp64;
 *   * a struct that FLATTENS -- its fields and its arrays' elements,
 *     nested structs opened up -- to one or two floats no wider than
 *     ABI_FLEN, or to one such float and one integer no wider than XLEN in
 *     either order, goes field by field: floats in fa registers, the
 *     integer in an a register, when enough of each are left; otherwise
 *     whole, by the integer rules. A complex counts as two floats. A union
 *     never flattens, nor does anything with a pointer in it, and a
 *     zero-width bit-field is ignored in a lone float's struct but ends the
 *     two-field ones -- clang's reading, which is what was checked;
 *   * a VARIADIC argument always takes the integer rules;
 *   * a result comes back the same way, in fa0/fa1 and a0 -- so a struct
 *     of two doubles at RV32, sixteen bytes, is returned in fa0 and fa1
 *     and not through a hidden pointer, and is passed in them rather than
 *     by reference.
 *
 * Read off clang (clang/lib/CodeGen/Targets/RISCV.cpp) and checked across
 * the call against it: tests/golden/riscv-hardfloat-abi.sh. */
struct rv_flat { int n; int fp[2]; int size[2]; long off[2]; };

static int flat_add(struct rv_flat *o, int fp, int size, long off)
{
    if (o->n == 2)
        return 0;
    o->fp[o->n] = fp;
    o->size[o->n] = size;
    o->off[o->n] = off;
    o->n++;
    return 1;
}

static int flat_walk(const struct type *t, long off, struct rv_flat *o)
{
    int flen = target_riscv_abi_flen() / 8, xlen = target_xlen() / 8;
    t = ty_unqual((struct type *)t);
    if (t->kind == TY_STRUCT && t->is_complex) {
        int esz = t->celem ? ty_size(t->celem) : 0;
        if (!t->celem || !ty_is_float(t->celem) || esz > flen || o->n)
            return 0;
        flat_add(o, 1, esz, off);
        return flat_add(o, 1, esz, off + esz);
    }
    if (ty_is_float(t)) {
        if (ty_size(t) > flen)
            return 0;
        return flat_add(o, 1, ty_size(t), off);
    }
    if (ty_is_integer(t)) {
        if (ty_size(t) > xlen || (o->n && !o->fp[0]))
            return 0;                       /* two integers: the integer CC */
        return flat_add(o, 0, ty_size(t), off);
    }
    if (t->kind == TY_ARRAY) {
        int esz;
        if (!t->pointee || t->count < 0)
            return 0;
        esz = ty_size(t->pointee);
        for (int k = 0; k < t->count; k++)
            if (!flat_walk(t->pointee, off + (long)k * esz, o))
                return 0;
        return 1;
    }
    if (t->kind == TY_STRUCT) {
        int zw = 0;
        if (!t->complete)
            return 0;
        if (!t->nmembers)
            return 1;                       /* empty: nothing */
        if (t->is_union)
            return 0;
        for (int k = 0; k < t->nmembers; k++) {
            const struct member *m = &t->members[k];
            if (m->is_bitfield) {
                /* an integer of the field's own type, at the byte its
                 * first bit is in -- or of XLEN bits, for a field of a
                 * wider type that fits in them */
                int bsz = ty_size(m->ty);
                if (m->bit_width == 0) {
                    zw++;
                    continue;
                }
                if (bsz > xlen) {
                    if (m->bit_width > 8 * xlen)
                        return 0;
                    bsz = xlen;
                }
                if (m->bf_bytes || (o->n && !o->fp[0]) ||
                    !flat_add(o, 0, bsz, off + m->off + m->bit_off / 8))
                    return 0;
            } else if (!flat_walk(m->ty, off + m->off, o)) {
                return 0;
            }
            if (o->n == 2 && zw)
                return 0;
        }
        return 1;
    }
    return 0;                               /* a pointer, among others */
}

/* Does an aggregate of type `t` flatten for the hardware-float CC? Only
 * the shape: whether the registers are there is the caller's. */
static int rv_flatten(const struct type *t, struct rv_flat *o)
{
    o->n = 0;
    if (!target_riscv_abi_flen() || !t || t->kind != TY_STRUCT)
        return 0;
    if (!flat_walk(t, 0, o) || !o->n)
        return 0;
    return o->n == 2 || o->fp[0];          /* not a lone integer */
}

/* How a result comes back under the hardware-float CC, or 0 for the
 * integer rules: a scalar float in fa0, a flattening struct in fa0/fa1
 * and a0. */
static int rv_ret_hf(int is_struct, int is_float, int size,
                     const struct type *ty, struct rv_flat *o)
{
    int abi = target_riscv_abi_flen();
    o->n = 0;
    if (!abi)
        return 0;
    if (!is_struct) {
        if (!is_float || size * 8 > abi)
            return 0;
        flat_add(o, 1, size, 0);
        return 1;
    }
    return rv_flatten(ty, o);
}

static void place_arg(int wb, int size, int align, int is_struct,
                      int variadic, int *narg, long *stk, struct argplace *p)
{
    int words = (size + wb - 1) / wb;

    p->byref = 0;
    p->copy = 0;
    /* (Not only an aggregate: RV32's long double is a scalar of four
     * words, and the psABI passes every argument wider than two
     * registers by reference.) */
    (void)is_struct;
    if (size > 2 * wb) {
        p->byref = 1;
        words = 1;                       /* just the pointer */
    }
    if (variadic && !p->byref && words == 2 && align >= 2 * wb) {
        *narg = (*narg + 1) & ~1;
        *stk = (*stk + 2 * wb - 1) & ~(long)(2 * wb - 1);
    }
    /* Wholly on the stack, an argument is aligned to its type, and to
     * XLEN at least, never past the stack's 16 (psABI). This rounded to
     * XLEN only, so a double or long long after one stack word went at
     * sp+4 on RV32 where clang and gcc put it at sp+8 -- and a call
     * between EmbCC code and theirs read the wrong half. (A split
     * argument starts the stack area, at offset 0, already aligned.) */
    if (*narg >= RV_NARGREG) {
        int a = p->byref || align < wb ? wb : align > 16 ? 16 : align;
        *stk = (*stk + a - 1) & ~(long)(a - 1);
    }
    p->reg = *narg;
    p->nreg = *narg < RV_NARGREG
            ? (words < RV_NARGREG - *narg ? words : RV_NARGREG - *narg) : 0;
    p->nstk = words - p->nreg;
    p->stk = *stk;
    *narg += p->nreg;
    if (p->nstk) {
        *narg = RV_NARGREG;              /* nothing back-fills past a split */
        *stk += (long)p->nstk * wb;
    }
}

/* Does a runtime helper take or return a floating-point value of `w`
 * bytes in an f register? Under the hardware-float CC lib/rt's helpers are
 * ordinary functions of it -- __extendsfdf2's float arrives in fa0 and
 * __truncdfsf2's comes back there -- exactly as clang calls them. */
static int rv_hfw(int w)
{
    int abi = target_riscv_abi_flen();
    return abi && w * 8 <= abi;
}

/* Where each argument goes, in order: the integer rules (place_arg),
 * and before them the hardware-float ones. The walk counts the a and the
 * fa registers apart -- a float in fa0 leaves a0 for the next integer. */
struct rv_walk { int narg, nfarg; long stk; };

static void walk_init(struct rv_walk *w, int sret)
{
    w->narg = sret ? 1 : 0;
    w->nfarg = 0;
    w->stk = 0;
}

static int arg_align(int wb, const struct ir_arg *a);

static void place_one(int wb, struct rv_walk *w, const struct ir_arg *a,
                      int variadic, struct argplace *p)
{
    int abi = target_riscv_abi_flen();
    struct rv_flat fl;
    p->hf = 0;
    p->nfld = 0;
    if (abi && !variadic) {
        int ok = 0;
        if (!a->is_struct && a->is_float && a->size * 8 <= abi) {
            fl.n = 1;
            fl.fp[0] = 1;
            fl.size[0] = a->size;
            fl.off[0] = 0;
            ok = 1;
        } else if (a->is_struct && rv_flatten(a->ty, &fl)) {
            ok = 1;
        }
        if (ok) {
            int nf = 0, ng;
            for (int k = 0; k < fl.n; k++)
                nf += fl.fp[k];
            ng = fl.n - nf;
            if (w->nfarg + nf <= RV_NARGREG && w->narg + ng <= RV_NARGREG) {
                p->hf = 1;
                p->nfld = fl.n;
                p->reg = p->nreg = p->nstk = 0;
                p->stk = 0;
                p->byref = 0;
                p->copy = 0;
                for (int k = 0; k < fl.n; k++) {
                    p->fld[k].fp = fl.fp[k];
                    p->fld[k].reg = fl.fp[k] ? w->nfarg++ : w->narg++;
                    p->fld[k].size = fl.size[k];
                    p->fld[k].off = fl.off[k];
                }
                return;
            }
        }
    }
    place_arg(wb, a->size, arg_align(wb, a), a->is_struct, variadic,
              &w->narg, &w->stk, p);
}

/* place_arg numbers the argument registers 0..7; a0 is x10. On ARM the
 * two coincided (r0 is register 0) and the Thumb backend could use the
 * index directly -- here that would load a1 from x1, which is `ra`, and
 * the first thing a function did was read its own return address as its
 * second parameter. */
static int argreg(int n) { return rv_argreg[n]; }

/* A by-reference copy's step: both ends are objects of the argument's
 * type -- the caller's object or slot and its copy, the copy and the
 * callee's local -- so it moves a word at a time where the type's
 * alignment allows, and a byte at a time only for a packed one. It went
 * a byte at a time always: a long double at RV32 was 32 instructions. */
static int byref_step(int wb, const struct ir_arg *a)
{
    int al = a->align ? a->align : a->is_struct ? 1 : a->size;
    return al >= wb ? wb : al >= 4 ? 4 : al >= 2 ? 2 : 1;
}

static int arg_align(int wb, const struct ir_arg *a)
{
    if (a->is_struct)
        return a->align ? a->align : wb;
    return a->size > wb ? 2 * wb : wb;
}

/* Does a call return through a hidden pointer? A composite LARGER than
 * two registers does; one that fits comes back packed in a0:a1. Only a
 * composite -- a `long long` at RV32 is eight bytes and comes back in
 * a0:a1 like any other scalar, and asking about size alone made every
 * 64-bit-returning function on the Thumb backend read its first parameter
 * out of the wrong register. */
static long sret_bytes(int wb, int retsize)
{
    return retsize > 2 * wb ? retsize : 0;
}

/* ...and so does a SCALAR wider than two registers, of which there is
 * one: RV32's long double, binary128 in four words. The psABI returns it
 * the way it would pass it as a first argument -- by reference -- so the
 * caller hands over the address in a0 as for a large struct. At RV64 it
 * is two registers and comes back in a0:a1. (`long long` at RV32 is two,
 * not more, and stays out.) A struct-returning call says retsize; any
 * other says the C type's size in ret_tybytes. */
/* A struct the hardware-float CC returns in registers needs no pointer,
 * however large (two doubles at RV32 are sixteen bytes). */
static long fn_sret_bytes(int wb, const struct ir_func *fn)
{
    struct rv_flat fl;
    if (rv_ret_hf(fn->ret_abi.is_struct, fn->ret_abi.is_float,
                  fn->ret_abi.size, fn->ret_abi.ty, &fl))
        return 0;
    return sret_bytes(wb, fn->ret_abi.size);
}

static long call_sret_bytes(int wb, const struct ir_ins *i)
{
    struct rv_flat fl;
    if (rv_ret_hf(i->retsize != 0, i->flt && !i->retsize,
                  i->retsize ? i->retsize : i->ret_tybytes, i->rety, &fl))
        return 0;
    return sret_bytes(wb, i->retsize ? i->retsize : i->ret_tybytes);
}

/* Where the psABI would put each value if it had the choice: a parameter
 * in the register it arrives in, a call's arguments in theirs, a call's
 * result and a returned value in a0. Each is a move that disappears when
 * the home IS that register -- without them the allocator put `a` of
 * `int add(int a, int b)` in a1 and b in a0 and the function opened by
 * swapping them through t4.
 *
 * Placement comes from place_arg, the same function the prologue and
 * every call use, so the psABI is not restated here. Only single-register
 * scalars: a pair or an aggregate is placed by a rule one register cannot
 * say, and a hint is only ever a preference -- the parallel moves at the
 * prologue and at each call are what is correct whatever is chosen. */
static void rv_abi_hints(const struct ir_func *fn, int *hint)
{
    int wb = target_ptr_size();
    struct rv_walk wk;
    struct argplace pl;
    struct rv_flat fl;
    int ret_f = rv_ret_hf(fn->ret_abi.is_struct, fn->ret_abi.is_float,
                          fn->ret_abi.size, fn->ret_abi.ty, &fl);
    walk_init(&wk, fn_sret_bytes(wb, fn) != 0);
    for (int p = 0; fn->src && p < fn->nparams && p < fn->nvregs; p++) {
        const struct ir_arg *a = &fn->param_abi[p];
        place_one(wb, &wk, a, 0, &pl);
        if (!pl.hf && pl.nreg == 1 && !pl.nstk && !pl.byref &&
            !a->is_struct && a->size <= wb)
            hint[p] = argreg(pl.reg);
    }
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        if (i->op == IR_RET && i->a >= 0 && i->a < fn->nvregs &&
            !fn->ret_abi.is_struct && fn->ret_abi.size <= wb && !ret_f)
            hint[i->a] = RV_A0;
        /* A soft-float helper the lowering calls (fp_args2/fp_result):
         * its operands go in a0 and a1 and its result comes back in a0.
         * Hinted there, a chain of float operations passes each result
         * straight on as the next one's argument; without, every link
         * was `mv a1,a0; mv a0,a1`. Not over a hint already given. */
        if (i->op != IR_CALL && rv_op_calls_helper(i) && i->w <= wb) {
            if (i->a >= 0 && i->a < fn->nvregs && hint[i->a] < 0)
                hint[i->a] = RV_A0;
            if (!i->imm_b && i->b >= 0 && i->b < fn->nvregs &&
                hint[i->b] < 0)
                hint[i->b] = RV_A1;
            if (i->dst >= 0 && i->dst < fn->nvregs && hint[i->dst] < 0)
                hint[i->dst] = RV_A0;
        }
        if (i->op != IR_CALL)
            continue;
        if (!i->retsize && i->dst >= 0 && i->dst < fn->nvregs && i->w <= wb &&
            !rv_ret_hf(0, i->flt, i->ret_tybytes, NULL, &fl))
            hint[i->dst] = RV_A0;
        walk_init(&wk, call_sret_bytes(wb, i) != 0);
        for (int k = 0; k < i->nargs; k++) {
            const struct ir_arg *a = &i->argv[k];
            place_one(wb, &wk, a, i->call_varargs && k >= i->call_nfixed,
                      &pl);
            if (!pl.hf && pl.nreg == 1 && !pl.nstk && !pl.byref &&
                !a->is_struct && a->size <= wb && a->vreg >= 0 &&
                a->vreg < fn->nvregs)
                hint[a->vreg] = argreg(pl.reg);
        }
    }
}

/* ---- the frame ---------------------------------------------------------- */

#define STACK_ALIGN 16          /* the psABI, at both widths */

static long outgoing_area(const struct rv_fn *F)
{
    const struct ir_func *fn = F->fn;
    long most = 0;
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        struct argplace pl;
        struct rv_walk wk;
        if (i->op != IR_CALL)
            continue;
        walk_init(&wk, call_sret_bytes(F->w, i) != 0);
        for (int k = 0; k < i->nargs; k++)
            place_one(F->w, &wk, &i->argv[k],
                      i->call_varargs && k >= i->call_nfixed, &pl);
        if (wk.stk > most)
            most = wk.stk;
    }
    return (most + STACK_ALIGN - 1) & ~(long)(STACK_ALIGN - 1);
}

/* How many bytes of BY-REFERENCE COPIES the widest call needs. */
static long byref_area(const struct rv_fn *F)
{
    const struct ir_func *fn = F->fn;
    long most = 0;
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        long need = 0;
        struct rv_walk wk;
        struct argplace pl;
        if (i->op != IR_CALL)
            continue;
        walk_init(&wk, call_sret_bytes(F->w, i) != 0);
        for (int k = 0; k < i->nargs; k++) {
            const struct ir_arg *a = &i->argv[k];
            place_one(F->w, &wk, a, i->call_varargs && k >= i->call_nfixed,
                      &pl);
            if (pl.byref)
                need = ((need + 15) & ~15L) + a->size;
        }
        if (need > most)
            most = need;
    }
    return (most + STACK_ALIGN - 1) & ~(long)(STACK_ALIGN - 1);
}

static int in_reg(const struct rv_fn *F, int v);
static int in_freg(const struct rv_fn *F, int v);
static int rv_needs_fx(const struct ir_func *fn);

static void layout(struct rv_fn *F)
{
    struct ir_func *fn = F->fn;
    long off = outgoing_area(F);
    F->out_bytes = off;

    F->byref_at = off;
    off += byref_area(F);

    F->slot = xmalloc((size_t)(fn->nvregs ? fn->nvregs : 1) * sizeof *F->slot);
    for (int v = 0; v < fn->nvregs; v++)
        F->slot[v] = -1;

    /* Only values that need memory get a slot, and the busiest go nearest
     * sp: a load or store reaches a 12-bit offset, the compressed
     * c.lwsp/c.swsp only 0..252, and past 2047 every access is built with
     * lui/addi/add first. Every value used to get a slot here -- including
     * the ones the allocator had put in registers -- so a function with a
     * few arrays addressed every temporary that way.
     *
     * Temporaries share a pool (ra_coalesce_temps, as the other backends
     * use): two whose live ranges do not overlap take one slot. A 64-bit
     * temp at RV32 keeps a slot of its own, eight-aligned, and so is shown
     * to the coalescer as if it had a register. At RV64 a 64-bit temp is
     * one register and shares the pool like any other: `wide` marks it
     * there too (wide_map), and reading the map without asking the XLEN
     * gave each one a slot of its own -- xTaskIncrementTick's -O0 frame
     * was 2384 bytes at RV64 against 128 at RV32, and FreeRTOS's timer
     * task overflowed a 2 KB stack. Then locals, small ones
     * first; one nothing names needs none (ra_locals_referenced), nor one
     * in a register (ra_slot_dead; under -g every local keeps its slot). */
    {
        int nv = fn->nvregs, npool = 0, has_cgoto = 0;
        int *loc2 = xmalloc((size_t)(nv ? nv : 1) * sizeof *loc2);
        for (int v = 0; v < nv; v++)
            loc2[v] = in_reg(F, v) || in_freg(F, v) ||
                      (F->xlen == 32 && F->wide[v]) || is16(F, v) ? 0 : -1;
        for (int n = 0; n < fn->nins; n++)
            if (fn->ins[n].op == IR_IGOTO || fn->ins[n].op == IR_LABELADDR)
                has_cgoto = 1;
        {
            struct ra_slots so = { loc2, F->floc, g_rv_regalloc, has_cgoto };
            int *tslot = ra_coalesce_temps(fn, fn->nvars, &so, &npool);
            off = (off + F->w - 1) & ~(long)(F->w - 1);
            for (int v = fn->nvars; v < nv; v++) {
                int k = v - fn->nvars;
                if (loc2[v] >= 0 || !tslot || tslot[k] < 0)
                    continue;
                F->slot[v] = off + (long)tslot[k] * F->w;
            }
            off += (long)npool * F->w;
            free(tslot);
        }
        for (int v = fn->nvars; v < nv; v++) {
            if (!is16(F, v))
                continue;
            off = (off + 15) & ~15L;
            F->slot[v] = off;
            off += 16;
        }
        for (int v = fn->nvars; v < nv; v++) {
            if (F->xlen != 32 || !F->wide[v] || in_reg(F, v) ||
                in_freg(F, v) || is16(F, v))
                continue;
            off = (off + 7) & ~7L;
            F->slot[v] = off;
            off += 8;
        }
        free(loc2);
    }
    {
        char *lref = ra_locals_referenced(fn, F->want_debug);
        for (int pass = 0; pass < 2; pass++)
            for (int v = 0; v < fn->nvars; v++) {
                int size = fn->locals[v].size ? fn->locals[v].size : F->w;
                int align = fn->locals[v].user_align ? fn->locals[v].user_align
                          : fn->locals[v].align ? fn->locals[v].align : F->w;
                if (in_reg(F, v) || in_freg(F, v) || !lref[v] ||
                    ra_slot_dead(fn, F->loc, F->floc, v, F->want_debug))
                    continue;
                if ((size > 2 * F->w) != pass)
                    continue;
                if (align < F->w) align = F->w;
                off = (off + align - 1) & ~(long)(align - 1);
                F->slot[v] = off;
                off += size;
            }
        free(lref);
    }
    /* RV32 with D: a double's crossing between the register files */
    F->fx = -1;
    if (rv_needs_fx(fn) && !F->fx_lazy) {
        off = (off + 7) & ~7L;
        F->fx = off;
        off += 8;
    }
    /* fa registers staged for x registers (F->fstage): the most any
     * call or the prologue needs */
    F->fstage = -1;
    {
        int most = 0, k;
        struct rv_walk wk;
        struct argplace pl;
        walk_init(&wk, fn_sret_bytes(F->w, fn) != 0);
        k = 0;
        for (int p = 0; p < fn->nparams; p++) {
            place_one(F->w, &wk, &fn->param_abi[p], 0, &pl);
            if (pl.hf && !fn->param_abi[p].is_struct && in_reg(F, p))
                k++;
        }
        most = k;
        for (int n = 0; n < fn->nins; n++) {
            const struct ir_ins *i = &fn->ins[n];
            if (i->op != IR_CALL)
                continue;
            walk_init(&wk, call_sret_bytes(F->w, i) != 0);
            k = 0;
            for (int a = 0; a < i->nargs; a++) {
                int v = i->argv[a].vreg;
                place_one(F->w, &wk, &i->argv[a],
                          i->call_varargs && a >= i->call_nfixed, &pl);
                if (!pl.hf && !pl.byref && !i->argv[a].is_struct &&
                    pl.nreg && in_freg(F, v) && is_fa(F->floc[v]))
                    k++;
            }
            if (k > most)
                most = k;
        }
        if (most) {
            off = (off + 7) & ~7L;
            F->fstage = off;
            off += 8L * most;
        }
    }
    /* the callee-saved f registers the FP pass took, eight bytes each */
    if (F->nfsave) {
        off = (off + 7) & ~7L;
        F->fsave_at = off;
        off += (long)F->nfsave * 8;
    }
    /* RV32's long double helpers' by-reference operands (gen_ld32) */
    F->tfa = -1;
    if (F->xlen == 32 && F->w16) {
        off = (off + 15) & ~15L;
        F->tfa = off;
        off += 48;
    }
    F->scratch_at = (off + 15) & ~15L;
    off = F->scratch_at + fn->scratch_bytes;

    F->sret_slot = -1;
    if (fn_sret_bytes(F->w, fn)) {
        /* A function returning a composite in memory is handed the
         * address to write to in a0 and must still have it at the return,
         * which may be many calls later -- and a0 survives none of them.
         * It lives on the frame. */
        off = (off + F->w - 1) & ~(long)(F->w - 1);
        F->sret_slot = off;
        off += F->w;
    }

    /* The saved return address and, for a variadic function, the REGISTER
     * SAVE AREA go at the TOP of the frame -- the save area flush against
     * it, so a0's copy sits immediately below the caller's stack
     * arguments and ONE pointer walks from the register ones into the
     * stack ones. Whatever padding the alignment needs lands below them,
     * where nothing depends on it. */
    {
        /* An interrupt handler keeps ra with the other caller-saved
         * registers it saves (isr_x), not in a slot of its own. */
        int raw = F->leaf || F->isr ? 0 : F->w;
        long need = off + raw + (long)F->nsave * F->w
                  + (fn->is_varargs ? (long)RV_NARGREG * F->w : 0);
        F->frame = (need + STACK_ALIGN - 1) & ~(long)(STACK_ALIGN - 1);
        if (fn->is_varargs) {
            F->va_regsave = F->frame - (long)RV_NARGREG * F->w;
            F->ra_slot = F->va_regsave - raw;
        } else {
            F->va_regsave = -1;
            F->ra_slot = F->frame - raw;
        }
        /* The callee-saved registers the allocator took, just below the
         * return address. Only the ones it REPORTS: a function that
         * needed none pays for none, which is what makes the
         * caller-saved-first preference order worth having. */
        F->save_at = F->ra_slot - (long)F->nsave * F->w;
    }
    /* An interrupt handler's saves, at the very top: sixteen-byte
     * aligned, so nothing below moves with how many there are, and
     * the first thing stored -- before anything is written. */
    F->isr_at = F->frame;
    F->isr_bytes = 0;
    if (F->isr) {
        int nx = 0, nf = 0;
        for (int r = 0; r < 32; r++) {
            nx += (int)(F->isr_x >> r & 1);
            nf += (int)(F->isr_f >> r & 1);
        }
        F->isr_bytes = ((long)nx * F->w + (long)nf * (target_riscv_flen() / 8)
                        + STACK_ALIGN - 1) & ~(long)(STACK_ALIGN - 1);
        F->frame += F->isr_bytes;
    }
    F->va_first = -1;
}

/* ---- reading and writing a vreg ----------------------------------------- */

/* sp-relative addressing reaches a signed 12-bit offset. Past that the
 * address is built in FAR, which exists for this and for nothing else. */
static int sp_addr(struct rv_fn *F, long off)
{
    rv_li(F->t, FAR, off, F->xlen);
    rv_alu(F->t, RV_ADD, FAR, F->fb, FAR, 0);
    return FAR;
}

static void ld_sp(struct rv_fn *F, int reg, long off, int size, int sign)
{
    if (rv_fits(off, 12)) {
        rv_load(F->t, reg, F->fb, (int)off, size, sign, F->xlen);
        return;
    }
    rv_load(F->t, reg, sp_addr(F, off), 0, size, sign, F->xlen);
}

static void st_sp(struct rv_fn *F, int reg, long off, int size)
{
    if (rv_fits(off, 12)) {
        rv_store(F->t, reg, F->fb, (int)off, size, F->xlen);
        return;
    }
    rv_store(F->t, reg, sp_addr(F, off), 0, size, F->xlen);
}

/* A store into the OUTGOING argument area, which is always at the live
 * sp -- the callee finds its stack arguments at its own entry sp, and
 * after a VLA that is not the frame base. */
static void st_out(struct rv_fn *F, int reg, long off, int size)
{
    if (rv_fits(off, 12)) {
        rv_store(F->t, reg, RV_SP, (int)off, size, F->xlen);
        return;
    }
    rv_li(F->t, FAR, off, F->xlen);
    rv_alu(F->t, RV_ADD, FAR, RV_SP, FAR, 0);
    rv_store(F->t, reg, FAR, 0, size, F->xlen);
}

/* sp + off, into `reg`. */
static void addr_sp(struct rv_fn *F, int reg, long off)
{
    if (rv_fits(off, 12)) {
        rv_alu_imm(F->t, RV_ADD, reg, F->fb, (int)off, 0);
        return;
    }
    rv_li(F->t, reg, off, F->xlen);
    rv_alu(F->t, RV_ADD, reg, F->fb, reg, 0);
}

/* Does the allocator have this vreg in a register? */
static int in_reg(const struct rv_fn *F, int v)
{
    return F->loc && v >= 0 && F->loc[v] >= 0;
}

/* Get vreg v into `reg`, whatever it takes.
 *
 * Without the allocator every value lives in memory and this is always
 * a load -- the naive part this whole file was built on. With it, a
 * value already in a register is a MOVE, and one already in the
 * register asked for is nothing at all.
 *
 * Keeping the "ends up in exactly `reg`" contract is what let the
 * allocator land without rewriting a hundred call sites at once: every
 * one of them stays correct, and the ones that decide code size are
 * converted to rdr/wreg below, which skip the move entirely. */
/* A slot, for code that addresses one directly. A value without one --
 * in a register, or never stored -- reaching such a path would read memory
 * nothing wrote; it is a refusal instead. */
static long sslot(const struct rv_fn *F, int v)
{
    if (v < 0 || v >= F->fn->nvregs || F->slot[v] < 0)
        internal_error("riscv: %s: a path addresses vreg %d's slot, and it "
                       "has none", F->fn->name, v);
    return F->slot[v];
}

/* ---- the floating-point registers, and crossing between the files -------
 *
 * A value has ONE home -- an x register (loc), an f register (floc) or a
 * slot -- and an operation that wants it in the other file moves it
 * across: fmv.x.w / fmv.w.x for a float, fmv.x.d / fmv.d.x for a double at
 * RV64, and at RV32 a double between an f register and an x PAIR through
 * the eight bytes at F->fx (fsd and two lw, or two sw and fld), there
 * being no 64-bit move at RV32. So every integer path that reads a float's
 * bits -- fabs, signbit, a bit cast -- is correct whatever the allocator
 * chose, and the FP paths are correct for a value it left in an x
 * register or a slot. */
static int in_freg(const struct rv_fn *F, int v)
{
    return F->floc && v >= 0 && v < F->fn->nvregs && F->floc[v] >= 0;
}

/* A slot's bytes into or out of an f register, as ld_sp/st_sp. */
static void fld_sp(struct rv_fn *F, int freg, long off, int dbl)
{
    if (rv_fits(off, 12)) {
        rv_fload(F->t, freg, F->fb, (int)off, dbl);
        return;
    }
    rv_fload(F->t, freg, sp_addr(F, off), 0, dbl);
}

static void fst_sp(struct rv_fn *F, int freg, long off, int dbl)
{
    if (rv_fits(off, 12)) {
        rv_fstore(F->t, freg, F->fb, (int)off, dbl);
        return;
    }
    rv_fstore(F->t, freg, sp_addr(F, off), 0, dbl);
}

static void need_fx(struct rv_fn *F)
{
    if (F->fx < 0 && F->fx_lazy) {
        F->fx_missed = 1;           /* emitted again, with it (gen_func) */
        return;
    }
    if (F->fx < 0)
        internal_error("riscv: %s: a double crosses between the register "
                       "files at RV32 with no transfer slot", F->fn->name);
}

/* The instruction being lowered, for the internal errors below. */
static const struct ir_ins *g_rv_cur;

/* x register (or pair) <- f register, and the other way */
static void x_from_f(struct rv_fn *F, int reg, int freg, int w)
{
    if (w == 8 && F->xlen == 32)
        internal_error("riscv: %s: a double read into one x register at "
                       "RV32 (%s)", F->fn->name,
                       g_rv_cur ? ir_opname(g_rv_cur->op) : "?");
    rv_fmv_to_x(F->t, reg, freg, w == 8);
}
static void f_from_x(struct rv_fn *F, int freg, int reg, int w)
{
    if (w == 8 && F->xlen == 32)
        internal_error("riscv: %s: a double written from one x register at "
                       "RV32 (%s)", F->fn->name,
                       g_rv_cur ? ir_opname(g_rv_cur->op) : "?");
    rv_fmv_from_x(F->t, freg, reg, w == 8);
}
static void pair_from_f(struct rv_fn *F, int lo, int hi, int freg)
{
    need_fx(F);
    fst_sp(F, freg, F->fx, 1);
    ld_sp(F, lo, F->fx, 4, 1);
    ld_sp(F, hi, F->fx + 4, 4, 1);
}
static void f_from_pair(struct rv_fn *F, int freg, int lo, int hi)
{
    need_fx(F);
    st_sp(F, lo, F->fx, 4);
    st_sp(F, hi, F->fx + 4, 4);
    fld_sp(F, freg, F->fx, 1);
}

/* At RV64 a slot holds eight bytes, sign-extended from a 32-bit value's
 * (wr's sd) -- except a float's, which fdone writes with fsw: four bytes,
 * so it is read back with lw, which is the same sign extension. */
static int slot_bytes(const struct rv_fn *F, int v)
{
    return F->xlen == 64 && F->f4 && v >= 0 && v < F->fn->nvregs &&
           F->f4[v] ? 4 : F->w;
}

static void rd(struct rv_fn *F, int v, int reg)
{
    if (in_freg(F, v)) {
        /* A narrow read of a double at RV32 -- after copy propagation any
         * operation may read a wide value at its own width -- is its LOW
         * word, as of a pair or a slot. */
        if (F->fw[v] == 8 && F->xlen == 32) {
            need_fx(F);
            fst_sp(F, F->floc[v], F->fx, 1);
            ld_sp(F, reg, F->fx, 4, 1);
            return;
        }
        x_from_f(F, reg, F->floc[v], F->fw[v]);
        return;
    }
    if (in_reg(F, v)) {
        if (F->loc[v] != reg)
            rv_mv(F->t, reg, F->loc[v]);
        return;
    }
    ld_sp(F, reg, sslot(F, v), slot_bytes(F, v), 1);
}

static void wr(struct rv_fn *F, int v, int reg)
{
    if (in_freg(F, v)) {
        f_from_x(F, F->floc[v], reg, F->fw[v]);
        return;
    }
    if (in_reg(F, v)) {
        if (F->loc[v] != reg)
            rv_mv(F->t, F->loc[v], reg);
        return;
    }
    if (F->slot[v] < 0)
        return;
    st_sp(F, reg, sslot(F, v), F->w);
}

/* An FP operand into exactly `freg`, wherever it lives -- `dbl` its width. */
static void fload_v(struct rv_fn *F, int v, int freg, int dbl)
{
    if (in_freg(F, v)) {
        if (F->floc[v] != freg)
            rv_fmv(F->t, freg, F->floc[v], dbl);
        return;
    }
    if (in_reg(F, v)) {
        if (dbl && F->xlen == 32)
            f_from_pair(F, freg, F->loc[v], F->loc[v] + 1);
        else
            f_from_x(F, freg, F->loc[v], dbl ? 8 : 4);
        return;
    }
    fld_sp(F, freg, sslot(F, v), dbl);
}

/* ...where an FP operand IS: its f register, or `scratch` loaded */
static int fsrc(struct rv_fn *F, int v, int scratch, int dbl)
{
    if (in_freg(F, v))
        return F->floc[v];
    fload_v(F, v, scratch, dbl);
    return scratch;
}

/* ...where to compute an FP result, and committing it from there */
static int fdst(const struct rv_fn *F, int v, int scratch)
{
    return in_freg(F, v) ? F->floc[v] : scratch;
}

static void fdone(struct rv_fn *F, int v, int freg, int dbl)
{
    if (v < 0)
        return;
    if (in_freg(F, v)) {
        if (F->floc[v] != freg)
            rv_fmv(F->t, F->floc[v], freg, dbl);
        return;
    }
    if (in_reg(F, v)) {
        if (dbl && F->xlen == 32)
            pair_from_f(F, F->loc[v], F->loc[v] + 1, freg);
        else
            x_from_f(F, F->loc[v], freg, dbl ? 8 : 4);
        return;
    }
    if (F->slot[v] < 0)
        return;
    if (!dbl && F->xlen == 64 && slot_bytes(F, v) == 8) {
        /* a slot the integer paths read eight bytes of: sign-extended */
        rv_fmv_to_x(F->t, SCR2, freg, 0);
        st_sp(F, SCR2, sslot(F, v), 8);
        return;
    }
    fst_sp(F, freg, sslot(F, v), dbl);
}

/* The three that skip the move.
 *
 * `rdr` says where a value IS -- its own register, or `scratch` after a
 * load -- so an operation reads it in place. `wreg` says where to
 * compute a result, and `wrote` commits it if that was a scratch. For
 * an allocated a, b and dst the trio turns four instructions into one:
 *
 *   ld t0,(a); ld t1,(b); add t0,t0,t1; sd t0,(d)  ->  add rD, rA, rB
 *
 * which is the whole point of an allocator on this target. */
static int rdr(struct rv_fn *F, int v, int scratch)
{
    if (in_reg(F, v))
        return F->loc[v];
    rd(F, v, scratch);
    return scratch;
}

static int wreg(struct rv_fn *F, int v, int scratch)
{
    return in_reg(F, v) ? F->loc[v] : scratch;
}

static void wrote(struct rv_fn *F, int v, int reg)
{
    if (in_freg(F, v)) {
        f_from_x(F, F->floc[v], reg, F->fw[v]);
        return;
    }
    if (in_reg(F, v)) {
        if (F->loc[v] != reg)
            rv_mv(F->t, F->loc[v], reg);
        return;
    }
    if (F->slot[v] < 0)
        return;
    st_sp(F, reg, sslot(F, v), F->w);
}

/* A 64-bit value's two halves at RV32, little-endian: the low word at the
 * slot and the high word four bytes above it. */
/* Two moves that are one parallel move: dl <- sl and dh <- sh. The
 * order matters when a destination is the other move's source, and a
 * swap needs the scratch. */
static void mv2(struct rv_fn *F, int dl, int sl, int dh, int sh)
{
    if (dl == sh && dh == sl) {
        if (dl == sl) return;
        rv_mv(F->t, SCR, sl);
        rv_mv(F->t, dh, sh);
        rv_mv(F->t, dl, SCR);
        return;
    }
    if (dl == sh) {                    /* dh first, before sh is lost */
        if (dh != sh) rv_mv(F->t, dh, sh);
        if (dl != sl) rv_mv(F->t, dl, sl);
        return;
    }
    if (dl != sl) rv_mv(F->t, dl, sl);
    if (dh != sh) rv_mv(F->t, dh, sh);
}

/* A 64-bit value in a register PAIR (rv_pair_alloc) has its low word in
 * F->loc[v] and its high word in the next register. */
/* A sixteen-byte value's two doublewords at RV64 (gen_ins128). */
static void ld128(struct rv_fn *F, int v, int lo, int hi);
static void st128(struct rv_fn *F, int v, int lo, int hi);
static void need16(const struct rv_fn *F, int v);

static void rd64(struct rv_fn *F, int v, int lo, int hi)
{
    if (in_freg(F, v)) {
        pair_from_f(F, lo, hi, F->floc[v]);
        return;
    }
    if (in_reg(F, v)) {
        mv2(F, lo, F->loc[v], hi, F->loc[v] + 1);
        return;
    }
    ld_sp(F, lo, sslot(F, v), 4, 1);
    ld_sp(F, hi, sslot(F, v) + 4, 4, 1);
}

static void wr64(struct rv_fn *F, int v, int lo, int hi)
{
    if (in_freg(F, v)) {
        f_from_pair(F, F->floc[v], lo, hi);
        return;
    }
    if (in_reg(F, v)) {
        mv2(F, F->loc[v], lo, F->loc[v] + 1, hi);
        return;
    }
    if (F->slot[v] < 0)
        return;
    st_sp(F, lo, sslot(F, v), 4);
    st_sp(F, hi, sslot(F, v) + 4, 4);
}

/* An instruction's SECOND operand, into `reg`.
 *
 * `b` is not always a vreg: the optimizer's immediate-fold pass moves a
 * constant into `imm` and sets `imm_b`, after which `b` holds nothing and
 * reading it as a vreg loads whatever occupies that slot. The IR header
 * lists that as an ADD/SUB/AND/OR/XOR/CMP flag; it is set on SHL, SHR and
 * MUL too, which is how `t += p[i]` came out as 101255427 on the Thumb
 * backend. Every binary operation asks HERE and none reads i->b. */
/* A folded constant as the REGISTER should hold it.
 *
 * `imm` is a `long` and carries the value sign-extended to 64 bits, but
 * an operation narrower than a register wants only its low half -- and at
 * RV32 the wider value does not fit in a register at all, which is how a
 * mask the optimizer folded to 0xffffffff00001fff reached rv_li and was
 * refused. Narrowed to 32 bits and sign-extended, which is also exactly
 * the invariant a 32-bit value obeys in a 64-bit register. */
static long long imm_val(const struct rv_fn *F, const struct ir_ins *i)
{
    if (F->xlen == 32 || i->w == 4)
        return (long long)(int)(unsigned int)(unsigned long)i->imm;
    return (long long)i->imm;
}

/* ---- 32-bit values at RV64 ------------------------------------------------
 *
 * The psABI's invariant is that a register holding a 32-bit value holds
 * its SIGN-EXTENSION, signed or not: 0xffffffffu is all ones. The `w`
 * instructions keep it and `lw` establishes it, so arithmetic is free --
 * but the IR narrows for nothing (`(int)some_long` is the same temp, read
 * at width 4), `lwu` and a zero-extending local read break it, and the
 * instructions that read all 64 bits -- a compare, a branch, a jump-table
 * bound -- then see a value the 32-bit one is not. So does whoever
 * receives it: a caller comparing a returned `unsigned` against
 * 0xffffffff, or a callee taking an argument.
 *
 * (unsigned)4294967295.75 came back from __fixunsdfsi in its zero-extended
 * form, which the caller's `li -1` did not equal.
 *
 * So the readers that need the invariant ask for it (rd32), and this map
 * says which values have it already, so that most of them cost nothing.
 * A value is sign-extended when EVERY definition leaves it so; anything
 * not listed here is assumed not to. */
static int sext_def(const struct rv_fn *F, const struct ir_ins *i,
                    const char *sx)
{
    struct ir_func *fn = F->fn;
    int nv = fn->nvregs;
#define SX(v) ((v) >= 0 && (v) < nv && sx[v])
    switch (i->op) {
    case IR_CONST: {
        long long v = imm_val(F, i);
        return v == (long long)(int)v;
    }
    case IR_MOV:
        return SX(i->a);
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
    case IR_SHL: case IR_SHR: case IR_NEG:
        return !i->flt && i->w == 4;          /* addw, mulw, divw, sllw... */
    case IR_AND: case IR_OR: case IR_XOR:
        /* x & k with 0 <= k < 2^31 is below 2^31 whatever x was:
         * pass_signtest narrows `if (m & 0xff0)` to an and.4 on a wide
         * m, which is then read by a 64-bit branch. */
        if (i->op == IR_AND && !i->flt && i->imm_b &&
            imm_val(F, i) >= 0 && imm_val(F, i) <= 0x7fffffffLL)
            return 1;
        if (i->flt || !SX(i->a))
            return 0;
        if (i->imm_b) {
            long long v = imm_val(F, i);
            return v == (long long)(int)v;
        }
        return SX(i->b);
    case IR_BNOT:                             /* xori -1 */
        return SX(i->a);
    case IR_CMP:
        return 1;                             /* 0 or 1 */
    case IR_LOAD:
        return i->size < 4 || (i->size == 4 && i->sign);
    case IR_LDVAR:
        /* A four-byte local in a register is read with a plain move
         * (rv_ldvar_plain): it holds what STVAR's sign extension, or the
         * caller, left there. A wider one read at four does not. */
        if (i->size < 4)
            return 1;
        if (i->size != 4 || !i->sign)
            return 0;
        return !in_reg(F, i->a) ||
               (i->a < fn->nvars && fn->locals[i->a].size == 4);
    case IR_EXT:
        return i->w != 16 && (i->size < 4 || (i->size == 4 && i->sign));
    case IR_SELECT:
        return SX(i->b) && SX(i->c);
    case IR_CALL:                             /* the callee's, by the ABI */
        return !i->flt && !i->retsize && i->ret_tybytes > 0 &&
               i->ret_tybytes <= 4;
    case IR_F2I:                              /* __fix*si, by the ABI */
        return i->w == 4;
    default:
        return 0;
    }
#undef SX
}

static char *sext_map(const struct rv_fn *F)
{
    struct ir_func *fn = F->fn;
    int nv = fn->nvregs, changed = 1;
    char *sx = xmalloc((size_t)(nv ? nv : 1));
    /* Optimistic, then cut down to a fixed point: a loop's accumulator
     * is sign-extended if its entry value and its update both are. */
    for (int v = 0; v < nv; v++)
        sx[v] = v >= fn->nvars;               /* a local is not a value */
    while (changed) {
        changed = 0;
        for (int n = 0; n < fn->nins; n++) {
            const struct ir_ins *i = &fn->ins[n];
            if (i->op == IR_STVAR || i->dst < 0 || i->dst >= nv ||
                !sx[i->dst])
                continue;
            if (!sext_def(F, i, sx)) {
                sx[i->dst] = 0;
                changed = 1;
            }
        }
    }
    return sx;
}

/* `v`, already in register `r`, as a 32-bit value a 64-bit instruction
 * may read: `r` itself when it is sign-extended already, else `scratch`
 * holding the extension. At RV32 every register is 32 bits and there is
 * nothing to do. */
static int sext32(struct rv_fn *F, int v, int r, int scratch)
{
    if (F->sx && v >= 0 && v < F->fn->nvregs && !F->sx[v]) {
        rv_alu_imm(F->t, RV_ADD, scratch, r, 0, 1);       /* sext.w */
        return scratch;
    }
    return r;
}

static int rd32(struct rv_fn *F, int v, int scratch)
{
    return sext32(F, v, rdr(F, v, scratch), scratch);
}

static void operand_b(struct rv_fn *F, const struct ir_ins *i, int reg)
{
    if (i->imm_b)
        rv_li(F->t, reg, imm_val(F, i), F->xlen);
    else
        rd(F, i->b, reg);
}

static void operand_b64(struct rv_fn *F, const struct ir_ins *i,
                        int lo, int hi)
{
    if (i->imm_b) {
        rv_li(F->t, lo, (long long)(i->imm & 0xffffffffL), 32);
        rv_li(F->t, hi, (long long)((i->imm >> 32) & 0xffffffffL), 32);
    } else {
        rd64(F, i->b, lo, hi);
    }
}

/* Sign- or zero-extend the low `size` bytes of `rs` to the full register. */
static void ext_reg(struct rv_fn *F, int rdst, int rs, int size, int sign)
{
    if (size >= F->w) {
        if (rdst != rs)
            rv_mv(F->t, rdst, rs);
        return;
    }
    if (size == 1 && !sign) {
        rv_alu_imm(F->t, RV_AND, rdst, rs, 255, 0);       /* zext.b */
        return;
    }
    if (size == 4 && sign && F->xlen == 64) {
        rv_alu_imm(F->t, RV_ADD, rdst, rs, 0, 1);         /* sext.w */
        return;
    }
    rv_shift_imm(F->t, RV_SLL, rdst, rs, F->xlen - size * 8, 0, F->xlen);
    rv_shift_imm(F->t, sign ? RV_SRA : RV_SRL, rdst, rdst,
                 F->xlen - size * 8, 0, F->xlen);
}

/* ---- branches ------------------------------------------------------------
 *
 * RISC-V has no condition codes: a conditional branch takes its two
 * operands and compares them itself. That removes every flag-liveness
 * question the Thumb backend had to answer and adds one of its own -- a
 * B-type displacement reaches only +-4KiB where a J-type reaches +-1MiB.
 *
 * So a conditional branch to a label is the INVERSE branch over an
 * unconditional jump: eight bytes, always in range, and independent of
 * how large the function turns out to be. The extra four bytes are the
 * same trade this whole file makes -- correct before small.
 */
/* The forms a jump or branch to a label can take. The first pass uses J
 * and LONG, which reach anywhere in a function; the second, whichever
 * shorter one the first measured in reach -- CJ (c.j, +-2 KiB), B (a
 * direct branch, +-4 KiB), CB (c.beqz/c.bnez, +-256 B, against zero from
 * x8-x15). Code between a branch and its target only shrinks on the
 * second pass, so what reached still reaches; the patch checks anyway. */
enum { FX_J, FX_CJ, FX_LONG, FX_B, FX_CB,
       FX_TAB,     /* a jump table's entry: bat is the auipc that finds
                    * the table, the word becomes target - auipc */
       FX_ADDR };  /* &&label: an auipc/addi pair at `at` */

static void want_label(struct rv_fn *F, int at, int label, int kind,
                       int cond, int rs1, int rs2, int bat)
{
    if (F->nfix == F->capfix) {
        F->capfix = F->capfix ? F->capfix * 2 : 16;
        F->fix = xrealloc(F->fix, (size_t)F->capfix * sizeof *F->fix);
    }
    F->fix[F->nfix].at = at;
    F->fix[F->nfix].label = label;
    F->fix[F->nfix].kind = kind;
    F->fix[F->nfix].cond = cond;
    F->fix[F->nfix].rs1 = rs1;
    F->fix[F->nfix].rs2 = rs2;
    F->fix[F->nfix].bat = bat;
    F->nfix++;
}

/* The form the first pass chose for the next jump or branch, or -1. */
static int relaxed_form(const struct rv_fn *F)
{
    return F->relax && F->nfix < F->nrelax ? F->relax[F->nfix] : -1;
}

static void jump_to(struct rv_fn *F, int label)
{
    if (relaxed_form(F) == FX_CJ)
        want_label(F, rv_c_placeholder(F->t), label, FX_CJ, 0, 0, 0, 0);
    else
        want_label(F, rv_j_placeholder(F->t, RV_ZERO), label, FX_J,
                   0, 0, 0, 0);
}

static int invert_branch(int cond)
{
    switch (cond) {
    case RV_BEQ:  return RV_BNE;
    case RV_BNE:  return RV_BEQ;
    case RV_BLT:  return RV_BGE;
    case RV_BGE:  return RV_BLT;
    case RV_BLTU: return RV_BGEU;
    default:      return RV_BLTU;      /* RV_BGEU */
    }
}

static void branch_if(struct rv_fn *F, int cond, int rs1, int rs2, int label)
{
    int form = relaxed_form(F), at, jat;
    if (form == FX_CB) {
        want_label(F, rv_c_placeholder(F->t), label, FX_CB, cond, rs1, rs2,
                   0);
        return;
    }
    if (form == FX_B) {
        want_label(F, rv_b_placeholder(F->t, cond, rs1, rs2), label, FX_B,
                   cond, rs1, rs2, 0);
        return;
    }
    /* The long form, which reaches anywhere: the opposite branch over a
     * jump. Patched to wherever the jump ENDED, not to `at + 8`. */
    at = rv_b_placeholder(F->t, invert_branch(cond), rs1, rs2);
    jat = rv_j_placeholder(F->t, RV_ZERO);
    want_label(F, jat, label, FX_LONG, cond, rs1, rs2, at);
    rv_patch_b(F->t, at, F->t->len);        /* over the jump just emitted */
}

/* ---- site lists --------------------------------------------------------- */

/* Calls to a function defined in this unit are `jal ra` -- four bytes
 * where auipc+jalr is eight -- while this is set. jal reaches +-1 MB,
 * which the unit's own text has to exceed before it matters; if a patch
 * finds it did, the unit is generated again with it clear. The linker
 * cannot do this: branches inside a function are resolved here, with no
 * relocation to move, so deleting bytes at link time would break them. */
static int g_rv_short_calls = 1;

static void note_call(struct rv_sites *st, int at, struct func *target)
{
    if (st->ncall == st->capcall) {
        st->capcall = st->capcall ? st->capcall * 2 : 16;
        st->call = xrealloc(st->call, (size_t)st->capcall * sizeof *st->call);
    }
    st->call[st->ncall].patch_off = at;
    st->call[st->ncall].target = target;
    st->call[st->ncall].jal = 0;
    st->call[st->ncall].tail = 0;
    st->ncall++;
}

static void note_ext(struct rv_sites *st, int at, struct func *callee)
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

static void note_str(struct rv_sites *st, int at, int idx, enum reloc_kind k)
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

static void note_glob(struct rv_sites *st, int at, struct global *g,
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

static void note_fn(struct rv_sites *st, int at, struct func *target,
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

/* The runtime helpers: 64-bit divides at RV32, and every floating-point
 * operation at both widths. Interned by NAME rather than from a fixed
 * table, because there are forty once soft float is counted and a table
 * would be a second place to keep the list. */
static struct func **g_helpers;
static int g_nhelpers, g_caphelpers;

static void call_helper(struct rv_fn *F, const char *name)
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
    note_ext(F->st, rv_call_placeholder(F->t), h);
}

/* ---- floating point, which this configuration has none of ----------------
 *
 * -march=rv32im/rv64im: no F and no D, so every floating-point operation
 * is a call and the soft-float ABI passes the operands in the INTEGER
 * argument registers -- a float in one, a double in one at RV64 and a
 * pair at RV32. The value is therefore never anything but bits, and the
 * integer paths already carry exactly the right number of them.
 *
 * The names are libgcc's, which is what the RISC-V toolchains use too.
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
 * __ltdf2 is negative when a < b, __gtdf2 positive when a > b, __eqdf2
 * zero when equal. Unordered makes each answer the way that renders the
 * predicate false, which is what a NaN must do -- except `!=`, where
 * __nedf2's nonzero is the right answer. */
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

/* Put `n` vregs into the argument registers a HELPER expects, all at
 * once.
 *
 * This is the first of the three sites regalloc.h names, and on this
 * target it bites where the IR cannot see it. `call_int_arg_in_reg` is
 * 0, so the allocator leaves an IR_CALL's arguments in memory and that
 * setup is a sequence of loads with no ordering problem. A SOFT-FLOAT
 * HELPER is not an IR_CALL: nothing marks its operands, so they are
 * ordinary values the allocator is free to put in registers -- and then
 * `__ltdf2(a, b)` with a in a1 and b in a0 does `mv a0, a1` and loses b
 * before reading it.
 *
 * That is what `fits(d)` compiled to at -O2, and it is why the parked
 * ARMv7-M attempt failed thumb-float and nothing else. */
/* `half` (may be NULL) picks a WORD of a 64-bit value at RV32: 0 its low
 * word, 1 its high one -- in a register pair the next register, in a slot
 * the next four bytes. A pair lives in the argument registers when that is
 * where it is passed (rv_pair_alloc), so the halves of one operand and the
 * words of another are edges of the SAME move: loading a0:a1 first and
 * then a2:a3 would overwrite a b that lives in a0:a1 before it was read. */
static int stg_find(const struct rv_fn *F, int v)
{
    for (int k = 0; k < F->nstg; k++)
        if (F->stg_v[k] == v)
            return k;
    return -1;
}

static void set_args_half(struct rv_fn *F, const int *dstreg,
                          const int *vreg, const int *half, int n)
{
    int pd[RA_MAXPOOL], ps[RA_MAXPOOL], npm = 0;

    for (int k = 0; k < n; k++)
        if (in_reg(F, vreg[k])) {
            pd[npm] = dstreg[k];
            ps[npm] = F->loc[vreg[k]] + (half ? half[k] : 0);
            npm++;
        }
    if (npm) {
        int od[RA_MAXPOOL * 2], os[RA_MAXPOOL * 2];
        int m = ra_parallel_move(pd, ps, npm, SCR, od, os,
                                 (int)(sizeof od / sizeof od[0]));
        if (m < 0)
            internal_error("riscv: a helper's argument setup is not a "
                           "well-formed move");
        for (int k = 0; k < m; k++)
            rv_mv(F->t, od[k], os[k]);
    }
    /* The loads come after: they only WRITE argument registers, so by
     * now nothing still needs the old contents of one. A value in an f
     * register crosses (fmv.x.w; a double's word at RV32 through fx). */
    for (int k = 0; k < n; k++)
        if (in_freg(F, vreg[k]) && stg_find(F, vreg[k]) >= 0) {
            /* staged out of its fa register before the f registers moved */
            long off = F->stg_off[stg_find(F, vreg[k])];
            int w8 = F->fw[vreg[k]] == 8;
            if (half)
                ld_sp(F, dstreg[k], off + 4L * half[k], 4, 1);
            else
                ld_sp(F, dstreg[k], off, w8 ? F->w : 4, 1);
        } else if (in_freg(F, vreg[k])) {
            if (half && F->fw[vreg[k]] == 8 && F->xlen == 32) {
                if (!half[k]) {
                    need_fx(F);
                    fst_sp(F, F->floc[vreg[k]], F->fx, 1);
                }
                ld_sp(F, dstreg[k], F->fx + 4L * half[k], 4, 1);
            } else {
                rd(F, vreg[k], dstreg[k]);
            }
        } else if (!in_reg(F, vreg[k])) {
            if (half)
                ld_sp(F, dstreg[k], sslot(F, vreg[k]) + 4L * half[k], 4, 1);
            else
                ld_sp(F, dstreg[k], sslot(F, vreg[k]),
                      slot_bytes(F, vreg[k]), 1);
        }
}

static void set_args(struct rv_fn *F, const int *dstreg, const int *vreg,
                     int n)
{
    set_args_half(F, dstreg, vreg, NULL, n);
}

/* Two 64-bit operands into a0:a1 and a2:a3, as one parallel move. */
static void args64x2(struct rv_fn *F, int va, int vb)
{
    int d[4] = { RV_A0, RV_A1, RV_A2, RV_A3 };
    int v[4], h[4] = { 0, 1, 0, 1 };
    v[0] = v[1] = va;
    v[2] = v[3] = vb;
    set_args_half(F, d, v, h, vb >= 0 ? 4 : 2);
}

/* Both operands of a two-argument helper. */
static void fp_args2(struct rv_fn *F, const struct ir_ins *i)
{
    if (i->w == 8 && F->xlen == 32) {
        args64x2(F, i->a, i->b);
        return;
    }
    {
        int dstreg[2], vreg[2];
        dstreg[0] = RV_A0; vreg[0] = i->a;
        dstreg[1] = RV_A1; vreg[1] = i->b;
        set_args(F, dstreg, vreg, 2);
    }
}

static void fp_result(struct rv_fn *F, int dst, int w)
{
    if (dst < 0)
        return;
    if (w == 8 && F->xlen == 32) wr64(F, dst, RV_A0, RV_A1);
    else                         wr(F, dst, RV_A0);
}

/* ---- comparisons ---------------------------------------------------------
 *
 * There is no `setcc`. A 0-or-1 is built from `slt`, the only comparison
 * the machine computes into a register, plus `xori 1` to invert and an
 * operand swap to reverse. `==` and `!=` go through `xor` first, because
 * slt cannot express them.
 */
static void cmp_to_reg(struct rv_fn *F, enum binop pred, int sign,
                       int ra, int rb, int dst)
{
    struct code *t = F->t;
    switch (pred) {
    case B_EQ:
        rv_alu(t, RV_XOR, dst, ra, rb, 0);
        rv_alu_imm(t, RV_SLTU, dst, dst, 1, 0);           /* seqz */
        return;
    case B_NE:
        rv_alu(t, RV_XOR, dst, ra, rb, 0);
        rv_alu(t, RV_SLTU, dst, RV_ZERO, dst, 0);         /* snez */
        return;
    case B_LT:
        rv_alu(t, sign ? RV_SLT : RV_SLTU, dst, ra, rb, 0);
        return;
    case B_GT:
        rv_alu(t, sign ? RV_SLT : RV_SLTU, dst, rb, ra, 0);
        return;
    case B_GE:
        rv_alu(t, sign ? RV_SLT : RV_SLTU, dst, ra, rb, 0);
        rv_alu_imm(t, RV_XOR, dst, dst, 1, 0);
        return;
    default: /* B_LE */
        rv_alu(t, sign ? RV_SLT : RV_SLTU, dst, rb, ra, 0);
        rv_alu_imm(t, RV_XOR, dst, dst, 1, 0);
        return;
    }
}

/* The same against a constant k, when it fits an I-type immediate: slti
 * and sltiu take it as it is (sign-extended to XLEN, which is the form
 * imm_val gives and rd32 reads), `x <= k` is `x < k + 1` and `x > k` its
 * inverse, `==` an xori and a seqz -- against zero the seqz, snez or slt
 * alone. Where k does not fit, 0 and the caller loads it. */
static int cmp_imm_to_reg(struct rv_fn *F, enum binop pred, int sign,
                          int ra, long long k, int dst)
{
    struct code *t = F->t;
    int inv = pred == B_GE || pred == B_GT;
    if (pred == B_LE || pred == B_GT) {
        if (!sign && k == -1)
            return 0;               /* k + 1 wraps: x <=u max */
        k++;
    }
    if (k < -2048 || k > 2047)
        return 0;
    switch (pred) {
    case B_EQ: case B_NE:
        if (k) {
            rv_alu_imm(t, RV_XOR, dst, ra, k, 0);
            ra = dst;
        }
        if (pred == B_EQ) rv_alu_imm(t, RV_SLTU, dst, ra, 1, 0);  /* seqz */
        else              rv_alu(t, RV_SLTU, dst, RV_ZERO, ra, 0); /* snez */
        return 1;
    default:
        if (k == 0 && sign)
            rv_alu(t, RV_SLT, dst, ra, RV_ZERO, 0);                /* sltz */
        else
            rv_alu_imm(t, sign ? RV_SLT : RV_SLTU, dst, ra, k, 0);
        if (inv)
            rv_alu_imm(t, RV_XOR, dst, dst, 1, 0);
        return 1;
    }
}

/* ---- 64-bit integers at RV32 ---------------------------------------------
 *
 * A 32-bit machine carries one in a REGISTER PAIR and an eight-byte slot,
 * low word first. Done here rather than as a legalisation pass over the
 * IR because the IR has no carry, and adding carry-carrying opcodes would
 * put two operations into the shared operand switches that only one
 * target ever emits -- which is how an opcode rots.
 *
 * RISC-V has no carry FLAG either, so the carry is computed rather than
 * read: after `sum = alo + blo`, `sltu sum, blo` is 1 exactly when the
 * addition wrapped. That is the whole trick, and it is why these
 * sequences are a little longer than ARM's adds/adcs.
 */
/* A shift of a 64-bit value by a CONSTANT amount. */
static void shift64_imm(struct rv_fn *F, int op, int sign, long n)
{
    struct code *t = F->t;
    n &= 63;
    if (n == 0)
        return;
    if (n >= 32) {
        int k = (int)(n - 32);
        if (op == RV_SLL) {
            if (k) rv_shift_imm(t, RV_SLL, A_HI, A_LO, k, 0, 32);
            else   rv_mv(t, A_HI, A_LO);
            rv_mv(t, A_LO, RV_ZERO);
        } else if (sign) {
            if (k) rv_shift_imm(t, RV_SRA, A_LO, A_HI, k, 0, 32);
            else   rv_mv(t, A_LO, A_HI);
            rv_shift_imm(t, RV_SRA, A_HI, A_HI, 31, 0, 32);
        } else {
            if (k) rv_shift_imm(t, RV_SRL, A_LO, A_HI, k, 0, 32);
            else   rv_mv(t, A_LO, A_HI);
            rv_mv(t, A_HI, RV_ZERO);
        }
        return;
    }
    if (op == RV_SLL) {
        rv_shift_imm(t, RV_SLL, A_HI, A_HI, (int)n, 0, 32);
        rv_shift_imm(t, RV_SRL, SCR, A_LO, (int)(32 - n), 0, 32);
        rv_alu(t, RV_OR, A_HI, A_HI, SCR, 0);
        rv_shift_imm(t, RV_SLL, A_LO, A_LO, (int)n, 0, 32);
    } else {
        rv_shift_imm(t, RV_SRL, A_LO, A_LO, (int)n, 0, 32);
        rv_shift_imm(t, RV_SLL, SCR, A_HI, (int)(32 - n), 0, 32);
        rv_alu(t, RV_OR, A_LO, A_LO, SCR, 0);
        rv_shift_imm(t, sign ? RV_SRA : RV_SRL, A_HI, A_HI, (int)n, 0, 32);
    }
}

/* A shift by a VARIABLE amount, in B_LO. Branching, in three arms.
 *
 * The branchless form every RISC-V compiler emits needs the complementary
 * shift `x << (32 - n)` to produce ZERO when n is 0. It does not: RISC-V
 * takes the low five bits of the count, so a shift by 32 is a shift by 0
 * and the two halves mix. ARM's register shifts DO produce zero at 32,
 * which is why the Thumb backend could write this in two arms and this
 * cannot. The third arm is n == 0, and leaving it out is a miscompile
 * that only shows up for a shift whose count happens to be zero. */
static void shift64_var(struct rv_fn *F, int op, int sign)
{
    struct code *t = F->t;
    int big, zero, done1, done2;

    rv_alu_imm(t, RV_AND, B_LO, B_LO, 63, 0);
    rv_li(t, SCR2, 32, 32);
    big = rv_b_placeholder(t, RV_BGEU, B_LO, SCR2);   /* count >= 32 */
    zero = rv_b_placeholder(t, RV_BEQ, B_LO, RV_ZERO);
    {
        /* 0 < count < 32 */
        rv_alu(t, RV_SUB, B_HI, SCR2, B_LO, 0);       /* 32 - count */
        if (op == RV_SLL) {
            rv_alu(t, RV_SLL, A_HI, A_HI, B_LO, 0);
            rv_alu(t, RV_SRL, SCR, A_LO, B_HI, 0);
            rv_alu(t, RV_OR, A_HI, A_HI, SCR, 0);
            rv_alu(t, RV_SLL, A_LO, A_LO, B_LO, 0);
        } else {
            rv_alu(t, RV_SRL, A_LO, A_LO, B_LO, 0);
            rv_alu(t, RV_SLL, SCR, A_HI, B_HI, 0);
            rv_alu(t, RV_OR, A_LO, A_LO, SCR, 0);
            rv_alu(t, sign ? RV_SRA : RV_SRL, A_HI, A_HI, B_LO, 0);
        }
    }
    done1 = rv_j_placeholder(t, RV_ZERO);
    rv_patch_b(t, big, t->len);
    {
        /* count >= 32: the halves move wholesale */
        rv_alu_imm(t, RV_ADD, B_HI, B_LO, -32, 0);
        if (op == RV_SLL) {
            rv_alu(t, RV_SLL, A_HI, A_LO, B_HI, 0);
            rv_mv(t, A_LO, RV_ZERO);
        } else if (sign) {
            rv_alu(t, RV_SRA, A_LO, A_HI, B_HI, 0);
            rv_shift_imm(t, RV_SRA, A_HI, A_HI, 31, 0, 32);
        } else {
            rv_alu(t, RV_SRL, A_LO, A_HI, B_HI, 0);
            rv_mv(t, A_HI, RV_ZERO);
        }
    }
    done2 = rv_j_placeholder(t, RV_ZERO);
    rv_patch_b(t, zero, t->len);          /* count == 0: nothing to do */
    rv_patch_j(t, done1, t->len);
    rv_patch_j(t, done2, t->len);
}

/* A 64-bit comparison at RV32, into ACC as a 0 or a 1. The high words
 * decide unless they are equal, in which case the low words do -- and the
 * low comparison is always UNSIGNED however the value itself is signed. */
/* ---- a 64-bit operation with a constant, half by half (RV32) ---------
 *
 * Each half of `x & 0x000fffffffffffff` is its own question: the low word
 * ANDed with all ones is a copy, the high one with 0xfffff two shifts --
 * where building both words and ANDing each was five instructions. Every
 * soft-float routine is these masks on the two words of a double.
 * EMBCC_RV_NOWIDEIMM=1 goes back to building them. */
static int g_rv_nowideimm = -1;
static int rv_wide_imm(void)
{
    if (g_rv_nowideimm < 0)
        g_rv_nowideimm = getenv("EMBCC_RV_NOWIDEIMM") != NULL;
    return !g_rv_nowideimm;
}

/* d = s OP c for one 32-bit half; d and s may be the same register,
 * neither is SCR. */
static void logic_half(struct rv_fn *F, int op, int d, int s, unsigned long c)
{
    struct code *t = F->t;
    unsigned long nc;
    long sc;
    c &= 0xffffffffUL;
    nc = ~c & 0xffffffffUL;
    sc = (long)(int)(unsigned int)c;
    if ((op == RV_AND && c == 0xffffffffUL) || (op != RV_AND && c == 0)) {
        if (d != s) rv_mv(t, d, s);
        return;
    }
    if (op == RV_AND && c == 0) {
        rv_mv(t, d, RV_ZERO);
        return;
    }
    if (op == RV_OR && c == 0xffffffffUL) {
        rv_li(t, d, -1, 32);
        return;
    }
    if (sc >= -2048 && sc <= 2047) {            /* andi, ori, xori (not) */
        rv_alu_imm(t, op, d, s, (int)sc, 0);
        return;
    }
    if (op == RV_AND && (c & (c + 1)) == 0) {   /* the low k bits */
        int k = 0;
        while (c >> k & 1) k++;
        rv_shift_imm(t, RV_SLL, d, s, 32 - k, 0, 32);
        rv_shift_imm(t, RV_SRL, d, d, 32 - k, 0, 32);
        return;
    }
    if (op == RV_AND && (nc & (nc + 1)) == 0) { /* all but the low j bits */
        int j = 0;
        while (nc >> j & 1) j++;
        rv_shift_imm(t, RV_SRL, d, s, j, 0, 32);
        rv_shift_imm(t, RV_SLL, d, d, j, 0, 32);
        return;
    }
    rv_li(t, SCR, sc, 32);
    rv_alu(t, op, d, s, SCR, 0);
}

/* A 64-bit shift by a constant from the pair (al, ah) into (dl, dh): the
 * same pair, or one sharing no register with it. Each half is written
 * after the last read of the source half it overwrites, so in place needs
 * nothing between, and nothing goes through A first. */
static void shift64_imm_to(struct rv_fn *F, int op, int sign, long n,
                           int al, int ah, int dl, int dh)
{
    struct code *t = F->t;
    n &= 63;
    if (n == 0) {
        if (dl != al) rv_mv(t, dl, al);
        if (dh != ah) rv_mv(t, dh, ah);
        return;
    }
    if (n >= 32) {
        int k = (int)(n - 32);
        if (op == RV_SLL) {
            if (k) rv_shift_imm(t, RV_SLL, dh, al, k, 0, 32);
            else if (dh != al) rv_mv(t, dh, al);
            rv_mv(t, dl, RV_ZERO);
        } else {
            if (k) rv_shift_imm(t, sign ? RV_SRA : RV_SRL, dl, ah, k, 0, 32);
            else if (dl != ah) rv_mv(t, dl, ah);
            if (sign) rv_shift_imm(t, RV_SRA, dh, ah, 31, 0, 32);
            else      rv_mv(t, dh, RV_ZERO);
        }
        return;
    }
    if (op == RV_SLL) {
        rv_shift_imm(t, RV_SRL, SCR, al, (int)(32 - n), 0, 32);
        rv_shift_imm(t, RV_SLL, dh, ah, (int)n, 0, 32);
        rv_alu(t, RV_OR, dh, dh, SCR, 0);
        rv_shift_imm(t, RV_SLL, dl, al, (int)n, 0, 32);
    } else {
        rv_shift_imm(t, RV_SLL, SCR, ah, (int)(32 - n), 0, 32);
        rv_shift_imm(t, RV_SRL, dl, al, (int)n, 0, 32);
        rv_alu(t, RV_OR, dl, dl, SCR, 0);
        rv_shift_imm(t, sign ? RV_SRA : RV_SRL, dh, ah, (int)n, 0, 32);
    }
}

/* Where a 64-bit operand's halves ARE: its pair, or the given scratch
 * registers after a load -- so an operation reads it in place. And where
 * to compute a 64-bit result: its pair, or A. Pairs never partly overlap
 * (rv_pair_alloc hands out whole aligned pairs), so an operation that
 * reads a half before writing the same half is safe with the result in
 * an operand's pair. */
static void src64(struct rv_fn *F, int v, int slo, int shi, int *lo, int *hi)
{
    if (in_reg(F, v)) {
        *lo = F->loc[v];
        *hi = F->loc[v] + 1;
        return;
    }
    rd64(F, v, slo, shi);
    *lo = slo;
    *hi = shi;
}
static void dst64(struct rv_fn *F, int v, int *lo, int *hi)
{
    *lo = in_reg(F, v) ? F->loc[v] : A_LO;
    *hi = in_reg(F, v) ? F->loc[v] + 1 : A_HI;
}

static void cmp64(struct rv_fn *F, const struct ir_ins *i, enum binop pred,
                  int sign)
{
    struct code *t = F->t;
    int hi_ne, done, al, ah, bl, bh;

    /* The operand where it lives. */
    src64(F, i->a, A_LO, A_HI, &al, &ah);
    /* Against zero -- `x < 0`, `x == 0`, most 64-bit compares there are --
     * the answer is in the high word's sign, or in whether either half
     * is set: no second operand, no branch. */
    if (i->imm_b && i->imm == 0) {
        if (pred == B_EQ || pred == B_NE) {
            rv_alu(t, RV_OR, SCR, al, ah, 0);
            if (pred == B_EQ) rv_alu_imm(t, RV_SLTU, ACC, SCR, 1, 0);
            else              rv_alu(t, RV_SLTU, ACC, RV_ZERO, SCR, 0);
            return;
        }
        if (sign && (pred == B_LT || pred == B_GE)) {
            rv_alu(t, RV_SLT, ACC, ah, RV_ZERO, 0);
            if (pred == B_GE)
                rv_alu_imm(t, RV_XOR, ACC, ACC, 1, 0);
            return;
        }
    }
    if (i->imm_b) {
        operand_b64(F, i, B_LO, B_HI);
        bl = B_LO; bh = B_HI;
    } else {
        src64(F, i->b, B_LO, B_HI, &bl, &bh);
    }

    if (pred == B_EQ || pred == B_NE) {
        rv_alu(t, RV_XOR, SCR, al, bl, 0);
        rv_alu(t, RV_XOR, SCR2, ah, bh, 0);
        rv_alu(t, RV_OR, SCR, SCR, SCR2, 0);
        if (pred == B_EQ) rv_alu_imm(t, RV_SLTU, ACC, SCR, 1, 0);
        else              rv_alu(t, RV_SLTU, ACC, RV_ZERO, SCR, 0);
        return;
    }
    hi_ne = rv_b_placeholder(t, RV_BNE, ah, bh);
    cmp_to_reg(F, pred, 0, al, bl, SCR);       /* equal highs: unsigned lows */
    done = rv_j_placeholder(t, RV_ZERO);
    rv_patch_b(t, hi_ne, t->len);
    cmp_to_reg(F, pred, sign, ah, bh, SCR);
    rv_patch_j(t, done, t->len);
    rv_mv(t, ACC, SCR);
}

/* Every 64-bit operation at RV32 that is not a call. Returns 0 for one
 * this does not handle, which the caller turns into a refusal naming it. */
static int gen_ins64(struct rv_fn *F, int n)
{
    struct ir_func *fn = F->fn;
    struct ir_ins *i = &fn->ins[n];
    struct code *t = F->t;

    switch (i->op) {
    case IR_CONST: {
        /* Built where it lives when that is a pair. */
        int lo = in_reg(F, i->dst) ? F->loc[i->dst] : A_LO;
        int hi = in_reg(F, i->dst) ? F->loc[i->dst] + 1 : A_HI;
        rv_li(t, lo, (long long)(i->imm & 0xffffffffL), 32);
        rv_li(t, hi, (long long)((i->imm >> 32) & 0xffffffffL), 32);
        wr64(F, i->dst, lo, hi);
        return 1;
    }
    /* This target has no floating-point register file: a double
     * already lives in a general register pair, so reinterpreting
     * its bits is a copy and nothing else. */
    case IR_BITCAST:
    case IR_MOV:
        /* Straight between the two homes; a copy within one pair is
         * nothing (mv2). Through A was four moves for a pair-to-pair. */
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
        /* Operands where they live and the result where it lives, as
         * the logic operations below: through A and B a pair-to-pair add
         * was eight moves around five instructions. The carry (borrow)
         * is computed from the low words before the low result can
         * overwrite one of them; the low result waits in SCR only when
         * its register is still to be read. */
        int al, ah, bl, bh, dl, dh;
        src64(F, i->a, A_LO, A_HI, &al, &ah);
        if (i->imm_b) {
            operand_b64(F, i, B_LO, B_HI);
            bl = B_LO; bh = B_HI;
        } else {
            src64(F, i->b, B_LO, B_HI, &bl, &bh);
        }
        dst64(F, i->dst, &dl, &dh);
        int lo = dl == ah || dl == bh || (i->op == IR_ADD && dl == bl)
                     ? SCR : dl;
        if (i->op == IR_ADD) {
            rv_alu(t, RV_ADD, lo, al, bl, 0);
            rv_alu(t, RV_SLTU, SCR2, lo, bl, 0);     /* did it wrap? */
            rv_alu(t, RV_ADD, dh, ah, bh, 0);
            rv_alu(t, RV_ADD, dh, dh, SCR2, 0);
        } else {
            rv_alu(t, RV_SLTU, SCR2, al, bl, 0);     /* will it borrow? */
            rv_alu(t, RV_SUB, lo, al, bl, 0);
            rv_alu(t, RV_SUB, dh, ah, bh, 0);
            rv_alu(t, RV_SUB, dh, dh, SCR2, 0);
        }
        if (lo != dl)
            rv_mv(t, dl, lo);
        wr64(F, i->dst, dl, dh);
        return 1;
    }
    case IR_AND: case IR_OR: case IR_XOR: {
        /* Each half on its own: operands where they live, result where it
         * lives. */
        int op = i->op == IR_AND ? RV_AND : i->op == IR_OR ? RV_OR : RV_XOR;
        int al, ah, bl, bh, dl, dh;
        src64(F, i->a, A_LO, A_HI, &al, &ah);
        if (i->imm_b && rv_wide_imm()) {
            dst64(F, i->dst, &dl, &dh);
            logic_half(F, op, dl, al, (unsigned long)i->imm);
            logic_half(F, op, dh, ah, (unsigned long)i->imm >> 32);
            wr64(F, i->dst, dl, dh);
            return 1;
        }
        if (i->imm_b) {
            operand_b64(F, i, B_LO, B_HI);
            bl = B_LO; bh = B_HI;
        } else {
            src64(F, i->b, B_LO, B_HI, &bl, &bh);
        }
        dst64(F, i->dst, &dl, &dh);
        rv_alu(t, op, dl, al, bl, 0);
        rv_alu(t, op, dh, ah, bh, 0);
        wr64(F, i->dst, dl, dh);
        return 1;
    }
    case IR_MUL:
        /* (ahi:alo) * (bhi:blo) keeping 64 bits: the cross terms reach
         * only the high word, and `mulhu` supplies the carry out of the
         * low one. */
        rd64(F, i->a, A_LO, A_HI);
        operand_b64(F, i, B_LO, B_HI);
        rv_muldiv(t, RV_MULHU, SCR2, A_LO, B_LO, 0);
        rv_muldiv(t, RV_MUL, SCR, A_LO, B_HI, 0);
        rv_alu(t, RV_ADD, SCR2, SCR2, SCR, 0);
        rv_muldiv(t, RV_MUL, SCR, A_HI, B_LO, 0);
        rv_alu(t, RV_ADD, SCR2, SCR2, SCR, 0);
        rv_muldiv(t, RV_MUL, A_LO, A_LO, B_LO, 0);
        rv_mv(t, A_HI, SCR2);
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    case IR_MULW: {
        /* 32 x 32 -> 64: mulh(u) for the high word and mul for the low,
         * the pair the ISA manual suggests fusing -- high first, into a
         * register neither operand is in, so the second still reads
         * both. The operands are single words, wherever they live. */
        int ra_ = rdr(F, i->a, B_LO), rb_ = rdr(F, i->b, B_HI), dl, dh;
        int hop = i->sign ? RV_MULH : RV_MULHU;
        dst64(F, i->dst, &dl, &dh);
        if (dh != ra_ && dh != rb_) {
            rv_muldiv(t, hop, dh, ra_, rb_, 0);
            rv_muldiv(t, RV_MUL, dl, ra_, rb_, 0);
        } else if (dl != ra_ && dl != rb_) {
            rv_muldiv(t, RV_MUL, dl, ra_, rb_, 0);
            rv_muldiv(t, hop, dh, ra_, rb_, 0);
        } else {
            rv_muldiv(t, hop, SCR, ra_, rb_, 0);
            rv_muldiv(t, RV_MUL, dl, ra_, rb_, 0);
            rv_mv(t, dh, SCR);
        }
        wr64(F, i->dst, dl, dh);
        return 1;
    }
    case IR_NEG: {
        int al, ah, dl, dh;
        src64(F, i->a, A_LO, A_HI, &al, &ah);
        dst64(F, i->dst, &dl, &dh);
        rv_alu(t, RV_SLTU, SCR, RV_ZERO, al, 0);     /* borrow out of 0-lo */
        rv_alu(t, RV_SUB, dl, RV_ZERO, al, 0);
        rv_alu(t, RV_SUB, dh, RV_ZERO, ah, 0);
        rv_alu(t, RV_SUB, dh, dh, SCR, 0);
        wr64(F, i->dst, dl, dh);
        return 1;
    }
    case IR_BNOT:
        rd64(F, i->a, A_LO, A_HI);
        rv_alu_imm(t, RV_XOR, A_LO, A_LO, -1, 0);
        rv_alu_imm(t, RV_XOR, A_HI, A_HI, -1, 0);
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    case IR_SHL: case IR_SHR: {
        int op = i->op == IR_SHL ? RV_SLL : RV_SRL;
        int sign = i->op == IR_SHR && i->sign;
        if (i->imm_b && rv_wide_imm()) {
            int al, ah, dl, dh;
            src64(F, i->a, A_LO, A_HI, &al, &ah);
            dst64(F, i->dst, &dl, &dh);
            shift64_imm_to(F, op, sign, (long)i->imm, al, ah, dl, dh);
            wr64(F, i->dst, dl, dh);
            return 1;
        }
        rd64(F, i->a, A_LO, A_HI);
        if (i->imm_b) {
            shift64_imm(F, op, sign, (long)i->imm);
        } else {
            rd(F, i->b, B_LO);
            shift64_var(F, op, sign);
        }
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    }
    case IR_EXT:
        /* Widening TO 64 bits: the low word is the value, the high word
         * is its sign or zero. */
        {
            /* From where it lives into the destination's own pair. */
            int s = rdr(F, i->a, A_LO), dl, dh;
            dst64(F, i->dst, &dl, &dh);
            if (i->size < 4)
                ext_reg(F, dl, s, i->size, i->sign);
            else if (dl != s)
                rv_mv(t, dl, s);
            if (i->sign) rv_shift_imm(t, RV_SRA, dh, dl, 31, 0, 32);
            else         rv_mv(t, dh, RV_ZERO);
            wr64(F, i->dst, dl, dh);
        }
        return 1;
    /* These four reach here when EITHER side is 64 bits, and only one
     * of them has to be: `*(unsigned *)p = (unsigned)(v >> i)` is a
     * four-byte store of an eight-byte value, and `long long x = y` of a
     * four-byte local an eight-byte read of it. The access is `size`
     * bytes whatever the value's width -- each of them wrote or read all
     * eight, and the four past a four-byte object are someone else's:
     * the next local, or past the frame's top, the caller's frame. As
     * Thumb's 64-bit path has always done, a narrower access moves the
     * low word, and a narrower read extends into the high one. */
    case IR_LDVAR:
        if (i->size == 8) {
            /* Straight between the two homes, as IR_MOV does: through
             * A, a pair-to-pair read was four moves. */
            if (in_reg(F, i->dst)) {
                rd64(F, i->a, F->loc[i->dst], F->loc[i->dst] + 1);
                return 1;
            }
            if (in_reg(F, i->a)) {
                wr64(F, i->dst, F->loc[i->a], F->loc[i->a] + 1);
                return 1;
            }
            rd64(F, i->a, A_LO, A_HI);       /* the local, slot or pair */
        } else {
            if (in_reg(F, i->a))
                ext_reg(F, A_LO, F->loc[i->a], i->size, i->sign);
            else
                ld_sp(F, A_LO, sslot(F, i->a), i->size, i->sign);
            if (i->sign) rv_shift_imm(t, RV_SRA, A_HI, A_LO, 31, 0, 32);
            else         rv_mv(t, A_HI, RV_ZERO);
        }
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    case IR_STVAR:
        if (i->size == 8 && in_reg(F, i->dst)) {     /* as IR_LDVAR */
            rd64(F, i->a, F->loc[i->dst], F->loc[i->dst] + 1);
            return 1;
        }
        if (i->size == 8 && in_reg(F, i->a)) {
            wr64(F, i->dst, F->loc[i->a], F->loc[i->a] + 1);
            return 1;
        }
        rd64(F, i->a, A_LO, A_HI);
        if (i->size == 8)
            wr64(F, i->dst, A_LO, A_HI);
        else if (in_reg(F, i->dst))          /* sign-extends, as at 32 bits */
            ext_reg(F, F->loc[i->dst], A_LO, i->size, 1);
        else if (F->slot[i->dst] >= 0)
            st_sp(F, A_LO, sslot(F, i->dst), i->size);
        return 1;
    case IR_LOAD: {
        /* The address where it lives, and the words straight into the
         * destination's pair: through ADDR and A, a 64-bit load from a
         * pointer in a register was two moves and the loads and two
         * moves more. A destination whose low register IS the address
         * takes the high word first. */
        int ra_ = rdr(F, i->a, ADDR), dl, dh;
        dst64(F, i->dst, &dl, &dh);
        if (i->size == 8) {
            if (dl == ra_) {
                rv_load(t, dh, ra_, 4, 4, 1, F->xlen);
                rv_load(t, dl, ra_, 0, 4, 1, F->xlen);
            } else {
                rv_load(t, dl, ra_, 0, 4, 1, F->xlen);
                rv_load(t, dh, ra_, 4, 4, 1, F->xlen);
            }
        } else {
            rv_load(t, dl, ra_, 0, i->size, i->sign, F->xlen);
            if (i->sign) rv_shift_imm(t, RV_SRA, dh, dl, 31, 0, 32);
            else         rv_mv(t, dh, RV_ZERO);
        }
        wr64(F, i->dst, dl, dh);
        return 1;
    }
    case IR_STORE: {
        int ra_ = rdr(F, i->a, ADDR), vl, vh;
        src64(F, i->b, A_LO, A_HI, &vl, &vh);
        rv_store(t, vl, ra_, 0, i->size == 8 ? 4 : i->size, F->xlen);
        if (i->size == 8)
            rv_store(t, vh, ra_, 4, 4, F->xlen);
        return 1;
    }
    case IR_SELECT: {
        int take_c, done;
        if (i->size == 8 && rv_wide_imm()) {   /* either half, in place */
            int al, ah;
            src64(F, i->a, SCR, SCR2, &al, &ah);
            rv_alu(t, RV_OR, SCR, al, ah, 0);
        } else if (i->size == 8) {     /* a 64-bit condition: either half */
            rd64(F, i->a, SCR, SCR2);
            rv_alu(t, RV_OR, SCR, SCR, SCR2, 0);
        } else {
            rd(F, i->a, SCR);
        }
        take_c = rv_b_placeholder(t, RV_BEQ, SCR, RV_ZERO);
        rd64(F, i->b, A_LO, A_HI);
        done = rv_j_placeholder(t, RV_ZERO);
        rv_patch_b(t, take_c, t->len);
        rd64(F, i->c, A_LO, A_HI);
        rv_patch_j(t, done, t->len);
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    }
    default:
        return 0;
    }
}

/* `fd[k] <- fs[k]` among the f registers, as one parallel move
 * (ra_parallel_move; ft0 breaks a cycle). With D every move is fmv.d,
 * which copies a NaN-boxed float as exactly as a double. */
static void fp_parallel_move(struct rv_fn *F, const int *fd, const int *fs,
                             int n)
{
    int od[RA_MAXPOOL * 2], os[RA_MAXPOOL * 2], m;
    if (!n)
        return;
    m = ra_parallel_move(fd, fs, n, RV_FT0, od, os,
                         (int)(sizeof od / sizeof od[0]));
    if (m < 0)
        internal_error("riscv: %s: the f registers' parallel move is not "
                       "well formed", F->fn->name);
    for (int k = 0; k < m; k++)
        rv_fmv(F->t, od[k], os[k], target_riscv_flen() == 64);
}

/* ---- one call ------------------------------------------------------------ */

/* Can the call at n be a TAIL call: the frame torn down first and the
 * callee jumped to, returning straight to this function's caller? Only
 * when nothing of this frame can still be needed -- no argument on the
 * stack or passed by reference (the copy is in this frame), no local
 * whose address could have escaped into the callee, no struct result --
 * and the IR_RET right after returns exactly what the call returned, at
 * the same width and in the same register class. */
static int rv_tail_ok(const struct rv_fn *F, int n)
{
    const struct ir_func *fn = F->fn;
    const struct ir_ins *i = &fn->ins[n], *r;
    struct argplace pl;
    struct rv_walk wk;

    if (i->op != IR_CALL || i->indirect || i->call_varargs || i->retsize ||
        i->flt || getenv("EMBCC_NO_TAILCALL"))
        return 0;
    if (fn->ret_abi.is_struct || fn->ret_abi.is_float)
        return 0;
    if (n + 1 >= fn->nins) {
        /* the last instruction of a function that returns nothing */
        if (fn->ret_abi.size)
            return 0;
    } else {
        r = &fn->ins[n + 1];
        if (r->op != IR_RET)
            return 0;
        if (r->a >= 0 && (r->a != i->dst ||
                          i->ret_tybytes != fn->ret_abi.size ||
                          i->ret_tybytes > F->w))
            return 0;
    }
    {
        int nret = 0;
        for (int k = 0; k < fn->nins; k++) {
            enum ir_op op = fn->ins[k].op;
            if (op == IR_ADDR || op == IR_VA_START)
                return 0;
            nret += op == IR_RET;
        }
        /* ...and the function's ONLY return: elsewhere the restores
         * here would sit beside the epilogue's, which a tail call
         * pays for with its copy. Measured over the libc corpus, that
         * rule is the smaller of the two. */
        if (nret > 1)
            return 0;
    }
    if (fn->has_alloca || fn->is_varargs || fn->neh)
        return 0;
    walk_init(&wk, 0);
    for (int k = 0; k < i->nargs; k++) {
        place_one(F->w, &wk, &i->argv[k], 0, &pl);
        if (pl.nstk || pl.byref)
            return 0;
    }
    return 1;
}

/* The epilogue's restores: the callee-saved registers, ra when it was
 * saved, and the frame. Shared by the epilogue and a tail call, which
 * must leave exactly the state the epilogue's `ret` would. */
static void rv_restore(struct rv_fn *F)
{
    struct code *t = F->t;
    for (int k = 0; k < F->nsave; k++)
        ld_sp(F, F->used_callee[k], F->save_at + (long)k * F->w, F->w, 1);
    for (int k = 0; k < F->nfsave; k++)
        fld_sp(F, F->fused[k], F->fsave_at + (long)k * 8,
               target_riscv_abi_flen() == 64);
    if (!F->leaf)
        ld_sp(F, RV_RA, F->ra_slot, F->w, 1);
    if (F->frame) {
        if (rv_fits(F->frame, 12)) {
            rv_alu_imm(t, RV_ADD, RV_SP, RV_SP, (int)F->frame, 0);
        } else {
            rv_li(t, RV_T0, F->frame, F->xlen);
            rv_alu(t, RV_ADD, RV_SP, RV_SP, RV_T0, 0);
        }
    }
}

/* ---- interrupt handlers ------------------------------------------------
 *
 * __attribute__((interrupt)) / interrupt("machine") / ("supervisor"), as
 * GCC and clang define them. The trap arrives between two instructions
 * of code that had values in every register, so the handler must leave
 * every register as it found it -- not just the callee-saved ones an
 * ordinary function keeps. The callee-saved ones are kept the ordinary
 * way; the rest are saved and restored here:
 *
 *   - each caller-saved integer register the handler WRITES (ra, t0-t6,
 *     a0-a7), and each floating-point register a call may clobber, any
 *     part of (rv_isr_fcand);
 *   - ALL of them, integer and floating point, when it calls anything:
 *     the callee may use any of them and saves none. A runtime helper
 *     (soft-float, a multiply on a part without M) is a call too.
 *
 * clang's rule, register for register (tests/golden/riscv-isr.sh checks
 * the set against it), and it returns as clang does: mret, or sret for
 * a supervisor-mode handler. fcsr is not saved, as by neither compiler.
 *
 * What a handler WRITES is read off the bytes it compiled to, not
 * predicted: the scratch registers, the far-slot base, a large frame's
 * size, an asm template's own registers are all written by code the
 * allocator never sees, and a rule that listed them would be the
 * thing that goes stale. gen_func emits the handler, decodes what its
 * instructions write (rv_scan_writes), and emits it again saving those,
 * until a pass writes nothing outside the set -- which the next pass
 * changes nothing about, because the saves sit at the top of the frame
 * and every slot the body addresses is below them.
 *
 * The handler is four-byte aligned whatever the C extension allows:
 * mtvec and stvec take an address whose low two bits are the mode, so a
 * two-aligned handler would be entered two bytes early. */

/* The integer registers a call may clobber: ra, t0-t2, a0-a7, t3-t6. */
static int rv_isr_xcand(int r)
{
    return r == RV_RA || (r >= RV_T0 && r <= RV_T2) ||
           (r >= RV_A0 && r <= RV_A7) || r >= RV_T3;
}

/* The f registers a call may clobber any part of: ft0-ft11 and fa0-fa7,
 * and all 32 when the ABI keeps fewer bits of fs0-fs11 than the
 * registers hold (ilp32f on a D part, or a soft-float ABI with an FPU) --
 * rv_fp_callee_saved's own rule. */
static int rv_isr_fcand(int r)
{
    return !rv_fp_callee_saved(r);
}

/* The registers one instruction writes, or might: an encoding not
 * recognised is taken to write its rd field in both files. */
static void rv_writes32(unsigned long w, unsigned long *xw, unsigned long *fw)
{
    int rd = (int)(w >> 7) & 31;
    switch (w & 0x7f) {
    case 0x37: case 0x17: case 0x6f: case 0x67:     /* lui auipc jal jalr */
    case 0x13: case 0x1b: case 0x33: case 0x3b:     /* OP(-IMM)(-32) */
    case 0x03: case 0x2f:                           /* loads, AMOs */
        *xw |= 1UL << rd;
        break;
    case 0x73:                                      /* csrr* (not *ret) */
        if ((w >> 12 & 7) != 0)
            *xw |= 1UL << rd;
        break;
    case 0x07: case 0x43: case 0x47: case 0x4b: case 0x4f:
        *fw |= 1UL << rd;                           /* flw/fld, fmadd.. */
        break;
    case 0x53: {
        int f5 = (int)(w >> 27) & 31;
        /* compares, fcvt to an integer, fmv.x/fclass: an x register */
        if (f5 == 0x14 || f5 == 0x18 || f5 == 0x1c)
            *xw |= 1UL << rd;
        else
            *fw |= 1UL << rd;
        break;
    }
    case 0x23: case 0x27: case 0x63: case 0x0f:     /* stores, branches */
        break;
    default:
        *xw |= 1UL << rd;
        *fw |= 1UL << rd;
        break;
    }
}

static void rv_writes16(unsigned h, int xlen, unsigned long *xw,
                        unsigned long *fw)
{
    int f3 = (int)(h >> 13) & 7, rd = (int)(h >> 7) & 31;
    int rdp = 8 + (int)((h >> 2) & 7), rdp9 = 8 + (int)((h >> 7) & 7);
    switch (h & 3) {
    case 0:
        if (f3 == 0 || f3 == 2 || (f3 == 3 && xlen == 64))
            *xw |= 1UL << rdp;                 /* addi4spn lw ld */
        else if (f3 == 1 || f3 == 3)
            *fw |= 1UL << rdp;                 /* fld flw */
        else if (f3 == 4) {
            *xw |= 1UL << rdp;
            *fw |= 1UL << rdp;
        }
        break;                                 /* 5-7: stores */
    case 1:
        if (f3 == 1 && xlen == 32)
            *xw |= 1UL << RV_RA;               /* c.jal */
        else if (f3 <= 3)
            *xw |= 1UL << rd;                  /* addi addiw li lui */
        else if (f3 == 4)
            *xw |= 1UL << rdp9;                /* the ALU group */
        break;                                 /* 5-7: j beqz bnez */
    default:
        if (f3 == 0 || f3 == 2 || (f3 == 3 && xlen == 64))
            *xw |= 1UL << rd;                  /* slli lwsp ldsp */
        else if (f3 == 1 || f3 == 3)
            *fw |= 1UL << rd;                  /* fldsp flwsp */
        else if (f3 == 4) {
            int rs2 = (int)(h >> 2) & 31;
            if (rs2)
                *xw |= 1UL << rd;              /* mv add */
            else if ((h >> 12 & 1) && rd)
                *xw |= 1UL << RV_RA;           /* jalr (jr writes none) */
        }
        break;                                 /* 5-7: stores to sp */
    }
}

/* What the instructions in [from, to) of F's code write. A jump table's
 * words are data and skipped. */
static void rv_scan_writes(const struct rv_fn *F, int from, int to,
                           unsigned long *xw, unsigned long *fw)
{
    const unsigned char *p = F->t->p;
    int at = from;
    while (at < to) {
        int data = 0;
        for (int k = 0; k < F->nfix && !data; k++)
            data = F->fix[k].kind == FX_TAB && F->fix[k].at == at;
        if (data) {
            at += 4;
            continue;
        }
        unsigned h = (unsigned)p[at] | (unsigned)p[at + 1] << 8;
        if ((h & 3) != 3) {
            rv_writes16(h, F->xlen, xw, fw);
            at += 2;
            continue;
        }
        rv_writes32((unsigned long)h |
                    ((unsigned long)p[at + 2] | (unsigned long)p[at + 3] << 8)
                    << 16, xw, fw);
        at += 4;
    }
    *xw &= ~1UL;
}

/* After a pass: what must the handler save, from what it wrote? Returns
 * 1 when that is more than this pass saved, and gen_func goes again. */
static int rv_isr_grow(struct rv_fn *F)
{
    unsigned long xw = 0, fw = 0, x = 0, f = 0;
    rv_scan_writes(F, F->fn->src->code_off, F->t->len, &xw, &fw);
    /* A call -- in the IR, to a helper, or in an asm template, which
     * writes ra -- clobbers whatever the callee likes. */
    int calls = !F->leaf || (xw >> RV_RA & 1);
    for (int r = 0; r < 32; r++) {
        if (r && rv_isr_xcand(r) && (calls || (xw >> r & 1)))
            x |= 1UL << r;
        if (target_riscv_flen() && rv_isr_fcand(r) &&
            (calls || (fw >> r & 1)))
            f |= 1UL << r;
    }
    if (!(x & ~F->isr_x) && !(f & ~F->isr_f))
        return 0;
    F->isr_x |= x;
    F->isr_f |= f;
    return 1;
}

/* The saves (store) or the restores, at base + their offsets from sp:
 * integer registers from the top down in register order, as clang lays
 * them out, then the f registers at the full FLEN. */
static void rv_isr_regs(struct rv_fn *F, long base, int store)
{
    struct code *t = F->t;
    long off = base + F->isr_bytes;
    int fl = target_riscv_flen();
    for (int r = 1; r < 32; r++)
        if (F->isr_x >> r & 1) {
            off -= F->w;
            if (store)
                rv_store(t, r, RV_SP, (int)off, F->w, F->xlen);
            else
                rv_load(t, r, RV_SP, (int)off, F->w, 1, F->xlen);
        }
    for (int r = 0; r < 32; r++)
        if (F->isr_f >> r & 1) {
            off -= fl / 8;
            if (store)
                rv_fstore(t, r, RV_SP, (int)off, fl == 64);
            else
                rv_fload(t, r, RV_SP, (int)off, fl == 64);
        }
}

/* sp += d, through t0 when d is past addi's reach: only ever after the
 * saves, or before the restores, so t0 is the handler's to use. */
static void rv_isr_sp(struct rv_fn *F, long d)
{
    if (!d)
        return;
    if (rv_fits(d, 12)) {
        rv_alu_imm(F->t, RV_ADD, RV_SP, RV_SP, (int)d, 0);
        return;
    }
    rv_li(F->t, RV_T0, d, F->xlen);
    rv_alu(F->t, RV_ADD, RV_SP, RV_SP, RV_T0, 0);
}

/* The prologue's first act: the saves, before anything is written. A
 * frame addi reaches is one adjustment with the saves at its top; a
 * larger one is made in two, the saves first, so the li that builds its
 * size writes a t0 already saved. */
static void rv_isr_prologue(struct rv_fn *F)
{
    if (rv_fits(-F->frame, 12)) {
        rv_isr_sp(F, -F->frame);
        rv_isr_regs(F, F->isr_at, 1);
        return;
    }
    rv_isr_sp(F, -F->isr_bytes);
    rv_isr_regs(F, 0, 1);
    rv_isr_sp(F, -(F->frame - F->isr_bytes));
}

/* ...and the epilogue: the callee-saved restores, the frame, the saves
 * and mret or sret. */
static void rv_isr_epilogue(struct rv_fn *F)
{
    for (int k = 0; k < F->nsave; k++)
        ld_sp(F, F->used_callee[k], F->save_at + (long)k * F->w, F->w, 1);
    for (int k = 0; k < F->nfsave; k++)
        fld_sp(F, F->fused[k], F->fsave_at + (long)k * 8,
               target_riscv_abi_flen() == 64);
    if (rv_fits(F->frame, 12)) {
        rv_isr_regs(F, F->isr_at, 0);
        rv_isr_sp(F, F->frame);
    } else {
        rv_isr_sp(F, F->frame - F->isr_bytes);
        rv_isr_regs(F, 0, 0);
        rv_isr_sp(F, F->isr_bytes);
    }
    rv_xret(F->t, F->isr == ISR_SUPERVISOR);
}

static void gen_call(struct rv_fn *F, int n)
{
    struct ir_func *fn = F->fn;
    struct ir_ins *i = &fn->ins[n];
    struct code *t = F->t;
    struct argplace pl[MAX_PARAMS];
    struct rv_walk wk;
    long copy_at = F->byref_at;
    long sret = call_sret_bytes(F->w, i);
    struct rv_flat rfl;
    int ret_hf = rv_ret_hf(i->retsize != 0, i->flt && !i->retsize,
                           i->retsize ? i->retsize : i->ret_tybytes, i->rety,
                           &rfl);

    walk_init(&wk, sret != 0);             /* a0 holds the result's address */
    for (int k = 0; k < i->nargs; k++)
        place_one(F->w, &wk, &i->argv[k],
                  i->call_varargs && k >= i->call_nfixed, &pl[k]);

    /* The by-reference COPIES first: the psABI makes the CALLER own them,
     * because the callee may write to its parameter -- so passing the
     * original object's address would let a callee modify its caller's
     * variable. */
    for (int k = 0; k < i->nargs; k++) {
        struct ir_arg *a = &i->argv[k];
        if (!pl[k].byref)
            continue;
        copy_at = (copy_at + 15) & ~15L;
        pl[k].copy = copy_at;
        if (a->is_struct) {
            int s = rdr(F, a->vreg, TMP);    /* its address */
            copy_block_at(F, 1, a->size, byref_step(F->w, a), s, 0,
                          F->fb, copy_at);
        } else {
            need16(F, a->vreg);          /* RV32's long double: its slot */
            copy_block_at(F, 1, a->size, byref_step(F->w, a), F->fb,
                          sslot(F, a->vreg), F->fb, copy_at);
        }
        copy_at += a->size;
    }

    /* The STACK words next, then the registers: writing a stack argument
     * needs a scratch, and by the time a0-a7 are loaded there is none
     * left that is not already an argument. */
    for (int k = 0; k < i->nargs; k++) {
        struct ir_arg *a = &i->argv[k];
        if (!pl[k].nstk)
            continue;
        if (pl[k].byref) {
            addr_sp(F, SCR, pl[k].copy);
            st_out(F, SCR, pl[k].stk, F->w);
        } else if (a->is_struct) {
            rd(F, a->vreg, ADDR);
            for (int q = 0; q < pl[k].nstk; q++) {
                long off = (long)(pl[k].nreg + q) * F->w;
                long left = a->size - off;
                if (left >= F->w) {
                    rv_load(t, SCR, ADDR, (int)off, F->w, 0, F->xlen);
                    st_out(F, SCR, pl[k].stk + (long)q * F->w, F->w);
                } else {
                    /* The tail of an odd-sized struct, byte by byte: a
                     * whole-word load would read past the object. */
                    for (long b = 0; b < left; b++) {
                        rv_load(t, SCR, ADDR, (int)(off + b), 1, 0, F->xlen);
                        st_out(F, SCR, pl[k].stk + (long)q * F->w + b, 1);
                    }
                }
            }
        } else if (a->size > F->w) {
            if (F->xlen == 64)
                ld128(F, a->vreg, SCR, SCR2);
            else
                rd64(F, a->vreg, SCR, SCR2);
            if (pl[k].nreg == 1) {
                /* THE SPLIT. With exactly one register left, a 2*XLEN
                 * scalar puts its LOW half there and its HIGH half at the
                 * bottom of the stack area -- so only the high half is
                 * written here. Confirmed against clang. */
                st_out(F, SCR2, pl[k].stk, F->w);
            } else {
                st_out(F, SCR, pl[k].stk, F->w);
                st_out(F, SCR2, pl[k].stk + F->w, F->w);
            }
        } else {
            rd(F, a->vreg, SCR);
            if (a->size == 4 && !a->is_float)
                sext32(F, a->vreg, SCR, SCR);     /* an int: see rd32 */
            st_out(F, SCR, pl[k].stk, F->w);
        }
    }
    /* The hardware-float arguments' f registers, BEFORE the integer
     * ones -- a float whose home is an x register is read here, before
     * the x registers' parallel move below can overwrite it.
     *
     * First, a value in an fa register that an x register must receive (a
     * variadic double, a float past fa7) is STAGED to the frame: the f
     * registers' parallel move may write over it. Then that move, every
     * float argument already in an f register into its fa one; then the
     * rest, which only write fa registers nothing still reads -- a float
     * from an x register or a slot, a flattened struct's fields. The
     * integer half of a float-and-integer struct waits for the struct
     * arguments. */
    F->nstg = 0;
    for (int k = 0; k < i->nargs; k++) {
        int v = i->argv[k].vreg;
        if (pl[k].hf || pl[k].byref || i->argv[k].is_struct ||
            !pl[k].nreg || !in_freg(F, v) || !is_fa(F->floc[v]) ||
            stg_find(F, v) >= 0)
            continue;
        F->stg_v[F->nstg] = v;
        F->stg_off[F->nstg] = F->fstage + 8L * F->nstg;
        fst_sp(F, F->floc[v], F->stg_off[F->nstg], F->fw[v] == 8);
        F->nstg++;
    }
    {
        int fd[MAX_PARAMS], fs[MAX_PARAMS], nf = 0;
        for (int k = 0; k < i->nargs; k++) {
            int v = i->argv[k].vreg;
            if (pl[k].hf && !i->argv[k].is_struct && in_freg(F, v)) {
                fd[nf] = RV_FA0 + pl[k].fld[0].reg;
                fs[nf] = F->floc[v];
                nf++;
            }
        }
        fp_parallel_move(F, fd, fs, nf);
    }
    for (int k = 0; k < i->nargs; k++) {
        struct ir_arg *a = &i->argv[k];
        if (!pl[k].hf)
            continue;
        if (!a->is_struct) {
            if (!in_freg(F, a->vreg))
                fload_v(F, a->vreg, RV_FA0 + pl[k].fld[0].reg,
                        pl[k].fld[0].size == 8);
            continue;
        }
        rd(F, a->vreg, ADDR);
        for (int q = 0; q < pl[k].nfld; q++)
            if (pl[k].fld[q].fp)
                rv_fload(t, RV_FA0 + pl[k].fld[q].reg, ADDR,
                         (int)pl[k].fld[q].off, pl[k].fld[q].size == 8);
    }
    /* The SCALAR register arguments, all at once. This is the third of
     * the three sites regalloc.h names: the value for a0 may be sitting
     * in the register a2 is about to be given, and placing them in
     * order loses it.
     *
     * It runs BEFORE the struct arguments below, which also write
     * argument registers -- a struct's words landing in a2 would
     * destroy a scalar's source before the move had read it. The other
     * direction cannot happen: the allocator keeps every struct
     * argument's address in memory whatever this flag says. */
    {
        /* A 64-bit argument passed in a register pair is two edges of
         * the same move, since at RV32 a pair may live in the argument
         * registers themselves (rv_pair_alloc). */
        int sd_[2 * MAX_PARAMS], sv_[2 * MAX_PARAMS], sh_[2 * MAX_PARAMS];
        int ns_ = 0;
        for (int k = 0; k < i->nargs; k++) {
            struct ir_arg *a = &i->argv[k];
            if (!pl[k].nreg || pl[k].byref || a->is_struct)
                continue;
            if (a->size > F->w) {
                if (pl[k].nreg != 2 || F->xlen != 32)
                    continue;
                for (int q = 0; q < 2; q++) {
                    sd_[ns_] = argreg(pl[k].reg + q);
                    sv_[ns_] = a->vreg;
                    sh_[ns_] = q;
                    ns_++;
                }
                continue;
            }
            sd_[ns_] = argreg(pl[k].reg);
            sv_[ns_] = a->vreg;
            sh_[ns_] = 0;
            ns_++;
        }
        if (ns_)
            set_args_half(F, sd_, sv_, F->xlen == 32 ? sh_ : NULL, ns_);
        /* ...and an int argument is owed its sign extension (rd32),
         * made in place now that every register holds its own value. */
        for (int k = 0; k < i->nargs; k++) {
            struct ir_arg *a = &i->argv[k];
            if (pl[k].nreg && !pl[k].byref && !a->is_struct &&
                a->size == 4 && !a->is_float)
                sext32(F, a->vreg, argreg(pl[k].reg), argreg(pl[k].reg));
        }
    }
    for (int k = 0; k < i->nargs; k++) {
        struct ir_arg *a = &i->argv[k];
        if (pl[k].hf) {
            /* a float-and-integer struct's integer, from the struct */
            for (int q = 0; q < pl[k].nfld; q++) {
                if (pl[k].fld[q].fp)
                    continue;
                rd(F, a->vreg, ADDR);
                rv_load(t, argreg(pl[k].fld[q].reg), ADDR,
                        (int)pl[k].fld[q].off, pl[k].fld[q].size, 1,
                        F->xlen);
            }
            continue;
        }
        if (!pl[k].nreg)
            continue;
        if (pl[k].byref) {
            addr_sp(F, argreg(pl[k].reg), pl[k].copy);
        } else if (a->is_struct) {
            rd(F, a->vreg, ADDR);
            for (int q = 0; q < pl[k].nreg; q++) {
                int r = argreg(pl[k].reg + q);
                long off = (long)q * F->w;
                long left = a->size - off;
                if (left >= F->w) {
                    rv_load(t, r, ADDR, (int)off, F->w, 0, F->xlen);
                } else {
                    /* The last, partial word, assembled byte by byte into
                     * its register. An aggregate travels PACKED -- these
                     * are the object's bytes, not its fields. */
                    rv_mv(t, r, RV_ZERO);
                    for (long b = off + left - 1; b >= off; b--) {
                        rv_shift_imm(t, RV_SLL, r, r, 8, 0, F->xlen);
                        rv_load(t, SCR, ADDR, (int)b, 1, 0, F->xlen);
                        rv_alu(t, RV_OR, r, r, SCR, 0);
                    }
                }
            }
        } else if (a->size > F->w) {
            /* At RV32 a pair was placed by the parallel move above; at
             * RV64 the value is in its slot, so loading it now can
             * overwrite nothing the move still had to read. */
            if (pl[k].nreg == 2 && F->xlen != 32)
                ld128(F, a->vreg, argreg(pl[k].reg), argreg(pl[k].reg + 1));
            else if (pl[k].nreg != 2 && stg_find(F, a->vreg) >= 0)
                ld_sp(F, argreg(pl[k].reg),             /* the low word */
                      F->stg_off[stg_find(F, a->vreg)], 4, 1);
            else if (pl[k].nreg != 2 && in_freg(F, a->vreg))
                rd64(F, a->vreg, argreg(pl[k].reg), SCR);  /* the low word */
            else if (pl[k].nreg != 2)
                ld_sp(F, argreg(pl[k].reg), sslot(F, a->vreg), F->w,
                      1);                        /* the low word */
        }
        /* a plain scalar: already placed by the parallel move above */
    }
    /* The hidden result pointer goes in LAST, so nothing above can have
     * used a0 as a scratch after it was set. */
    if (sret && i->retsize) {
        addr_sp(F, RV_A0, F->scratch_at + i->scratch);
    } else if (sret) {
        /* RV32's long double: straight into the result's slot, or the tf
         * area's spare sixteen bytes when nothing reads it */
        if (i->dst >= 0)
            need16(F, i->dst);
        addr_sp(F, RV_A0, i->dst >= 0 && F->slot[i->dst] >= 0
                          ? F->slot[i->dst] : F->tfa + 32);
    }

    F->nstg = 0;
    if (F->tail && F->tail[n]) {
        /* The frame down, then a JUMP: the callee returns straight to
         * this function's caller, with ra as it came in. t1 carries the
         * far form's address, and nothing is live in it by now. */
        rv_restore(F);
        if (cg_call_local(fn->src, i->callee) && g_rv_short_calls) {
            note_call(F->st, t->len, i->callee);
            F->st->call[F->st->ncall - 1].jal = 1;
            F->st->call[F->st->ncall - 1].tail = 1;
            code_u32(t, rv_enc_j(0x6f, RV_ZERO, 0));
        } else if (cg_call_local(fn->src, i->callee)) {
            note_call(F->st, rv_tail_placeholder(t), i->callee);
            F->st->call[F->st->ncall - 1].tail = 1;
        } else {
            note_ext(F->st, rv_tail_placeholder(t), i->callee);
            F->st->ext[F->st->next - 1].tail = 1;
        }
        if (n + 1 < fn->nins)
            F->skip_next = 1;         /* the IR_RET: not reached */
        return;
    }
    if (i->indirect) {
        /* The target is read BEFORE nothing -- the arguments are already
         * in place, and SCR is not one of them. */
        rd(F, i->a, SCR);
        rv_jalr(t, RV_RA, SCR, 0);
    } else if (cg_call_local(fn->src, i->callee) && g_rv_short_calls) {
        note_call(F->st, t->len, i->callee);
        F->st->call[F->st->ncall - 1].jal = 1;
        code_u32(t, rv_enc_j(0x6f, RV_RA, 0));  /* raw: a fixed patch site */
    } else if (cg_call_local(fn->src, i->callee)) {
        note_call(F->st, rv_call_placeholder(t), i->callee);
    } else {
        note_ext(F->st, rv_call_placeholder(t), i->callee);
    }

    if (i->dst < 0)
        return;
    if (ret_hf && !i->retsize) {
        /* a float result, in fa0 */
        fdone(F, i->dst, RV_FA0, rfl.size[0] == 8);
        return;
    }
    if (ret_hf) {
        /* a flattened struct, in fa0/fa1 and a0, stored field by field
         * into the scratch whose address dst receives */
        long at = F->scratch_at + i->scratch;
        int nf = 0, ng = 0;
        for (int q = 0; q < rfl.n; q++) {
            if (rfl.fp[q])
                fst_sp(F, RV_FA0 + nf++, at + rfl.off[q], rfl.size[q] == 8);
            else
                st_sp(F, RV_A0 + ng++, at + rfl.off[q], rfl.size[q]);
        }
        addr_sp(F, ACC, at);
        wr(F, i->dst, ACC);
        return;
    }
    if (i->retsize) {
        /* dst receives the scratch's ADDRESS, the contract irgen shares
         * with the other backends. A composite that fits in registers
         * came back in a0:a1 and has to be stored there first; a larger
         * one the callee already wrote through the pointer. */
        if (!sret) {
            long at = F->scratch_at + i->scratch;
            for (int q = 0; (long)q * F->w < i->retsize; q++) {
                long off = (long)q * F->w;
                long left = i->retsize - off;
                if (left >= F->w) {
                    st_sp(F, RV_A0 + q, at + off, F->w);
                } else {
                    for (long b = 0; b < left; b++) {
                        if (b) rv_shift_imm(t, RV_SRL, RV_A0 + q, RV_A0 + q,
                                            8, 0, F->xlen);
                        st_sp(F, RV_A0 + q, at + off + b, 1);
                    }
                }
            }
        }
        addr_sp(F, ACC, F->scratch_at + i->scratch);
        wr(F, i->dst, ACC);
    } else if (sret) {
        /* the callee wrote it through a0 */
    } else if (F->xlen == 32 && F->wide[i->dst]) {
        wr64(F, i->dst, RV_A0, RV_A1);
    } else if (F->xlen == 64 && (i->w == 16 || is16(F, i->dst))) {
        st128(F, i->dst, RV_A0, RV_A1);
    } else {
        wr(F, i->dst, RV_A0);
    }
}

/* ---- 128 bits at RV64 -----------------------------------------------------
 *
 * __int128 and long double (IEEE binary128) are two XLEN words. At RV64
 * each such value lives in a sixteen-byte slot of its own and never in a
 * register: rv_w16_map marks them, and the allocator is handed that map
 * as `wide` (ineligible) -- the AArch64 backend's arrangement. An
 * operation loads the words it needs into t0-t2, t4 and t5, the low word
 * at the slot and the high one eight bytes above, computes, and stores
 * both back; t6 stays the far-offset register. Not t3: it is B_HI to the
 * RV32 pair code, but at RV64 the allocator may give it a value -- a
 * stack parameter in t3 came back as the high word of an xor. The other
 * five are never allocated, so an inline operation disturbs no value.
 *
 * Division, remainder, a shift by a count not known here, and every
 * binary128 operation and conversion are calls into lib/rt (int128.c,
 * fp128.c, softtf.c) under libgcc's names, a 128-bit operand in a0:a1
 * and a second in a2:a3 -- which rv_op_calls_helper tells the allocator.
 *
 * Not RV32: there binary128 is four words, which the psABI passes by
 * reference, and __int128 does not exist. */

#define W_HI SCR2               /* t5: the second operand's high word */

/* Only a value rv_w16_map marked HAS sixteen bytes of slot: one it
 * missed has eight, or a register, and reading sixteen there takes the
 * neighbour's bytes for the high word. */
static void need16(const struct rv_fn *F, int v)
{
    if (!is16(F, v))
        internal_error("riscv: %s: vreg %d is read or written as sixteen "
                       "bytes and has no sixteen-byte slot", F->fn->name, v);
}

static void ld128(struct rv_fn *F, int v, int lo, int hi)
{
    need16(F, v);
    long s = sslot(F, v);
    ld_sp(F, lo, s, 8, 1);
    ld_sp(F, hi, s + 8, 8, 1);
}

static void st128(struct rv_fn *F, int v, int lo, int hi)
{
    if (v < 0)
        return;
    need16(F, v);
    if (F->slot[v] < 0)
        return;                         /* a result nothing reads */
    st_sp(F, lo, F->slot[v], 8);
    st_sp(F, hi, F->slot[v] + 8, 8);
}

/* `v`'s value when its ONLY definition is a constant -- a shift count,
 * which a shift by a known amount does inline. Every definition is
 * counted, because a merge temp is written once per arm; a local is
 * never one. */
static int rv_const_of(const struct rv_fn *F, int v, long *out)
{
    const struct ir_func *fn = F->fn;
    const struct ir_ins *def = NULL;
    if (v < fn->nvars || v >= fn->nvregs)
        return 0;
    for (int n = 0; n < fn->nins; n++)
        if (fn->ins[n].dst == v && fn->ins[n].op != IR_STVAR &&
            fn->ins[n].op != IR_STORE) {
            if (def)
                return 0;
            def = &fn->ins[n];
        }
    if (!def || def->op != IR_CONST)
        return 0;
    *out = def->imm;
    return 1;
}

/* Does this instruction read or write a sixteen-byte value? The rest of
 * gen_ins handles everything else -- including a NARROW read of one
 * (`(long)x`, `(int)x` are the same vreg at a smaller width), which
 * loads the low word from the slot like any slot read. */
static int rv_ins128(const struct rv_fn *F, const struct ir_ins *i)
{
    switch (i->op) {
    case IR_CONST: case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV:
    case IR_MOD: case IR_AND: case IR_OR: case IR_XOR: case IR_SHL:
    case IR_SHR: case IR_NEG: case IR_BNOT: case IR_CMP: case IR_EXT:
        return i->w == 16;
    case IR_LOAD: case IR_LDVAR: case IR_STORE: case IR_STVAR:
        return i->size == 16;
    case IR_I2F: case IR_F2I: case IR_F2F:
        return i->w == 16 || i->size == 16;
    /* A copy or a select by its OWN width, not by the map: a temp the
     * optimizer reuses can hold a four-byte value on one path and be in
     * the sixteen-byte map through a copy on another, and `select.4` of
     * it was refused as "a 128-bit select". Such a narrow copy moves its
     * width like any other; the slot is sixteen bytes either way. A
     * width-less mov still copies the whole value. */
    case IR_MOV:
        return is16(F, i->dst) && is16(F, i->a) && (i->w == 16 || i->w == 0);
    case IR_SELECT:
        return i->w == 16;
    case IR_BRZ: case IR_BRNZ:
        return i->w == 16 && is16(F, i->a);
    default:
        return 0;
    }
}

/* A shift by a constant count, 0..127, on the words in A_LO:A_HI. */
static void shift128_imm(struct rv_fn *F, const struct ir_ins *i, int k)
{
    struct code *t = F->t;
    int left = i->op == IR_SHL, ar = !left && i->sign;
    if (k == 0)
        return;
    if (k >= 64) {
        /* One word crosses into the other, and the vacated word is zero
         * -- or, arithmetically, the sign. */
        if (left) {
            rv_shift_imm(t, RV_SLL, A_HI, A_LO, k - 64, 0, 64);
            rv_mv(t, A_LO, RV_ZERO);
        } else {
            rv_shift_imm(t, ar ? RV_SRA : RV_SRL, A_LO, A_HI, k - 64, 0, 64);
            if (ar) rv_shift_imm(t, RV_SRA, A_HI, A_HI, 63, 0, 64);
            else    rv_mv(t, A_HI, RV_ZERO);
        }
        return;
    }
    /* 1..63: each word shifts, and the bits leaving one enter the other */
    if (left) {
        rv_shift_imm(t, RV_SRL, B_LO, A_LO, 64 - k, 0, 64);
        rv_shift_imm(t, RV_SLL, A_HI, A_HI, k, 0, 64);
        rv_alu(t, RV_OR, A_HI, A_HI, B_LO, 0);
        rv_shift_imm(t, RV_SLL, A_LO, A_LO, k, 0, 64);
    } else {
        rv_shift_imm(t, RV_SLL, B_LO, A_HI, 64 - k, 0, 64);
        rv_shift_imm(t, RV_SRL, A_LO, A_LO, k, 0, 64);
        rv_alu(t, RV_OR, A_LO, A_LO, B_LO, 0);
        rv_shift_imm(t, ar ? RV_SRA : RV_SRL, A_HI, A_HI, k, 0, 64);
    }
}

/* The binary128 helpers' names. A comparison helper's int stands in the
 * same relation to 0 as a to b, and unordered makes the relation false
 * -- except __netf2's, whose nonzero is the right answer for a NaN. */
static const char *tf_cmp_name(enum binop pred)
{
    switch (pred) {
    case B_EQ: return "__eqtf2";
    case B_NE: return "__netf2";
    case B_LT: return "__lttf2";
    case B_LE: return "__letf2";
    case B_GT: return "__gttf2";
    default:   return "__getf2";       /* B_GE */
    }
}

/* ---- long double at RV32 ---------------------------------------------------
 *
 * binary128 is four words at RV32, and the psABI passes it BY REFERENCE
 * and returns it through a hidden pointer in a0 -- to the runtime's
 * helpers as to any function: `__addtf3(&r, &a, &b)` is what clang
 * emits. A value lives in its sixteen-byte slot (rv_w16_map, as at
 * RV64); a copy is four words through t0 and t1; an operation hands the
 * helper the address of its result's own slot, and of a COPY of each
 * operand in this function's tf area (F->tfa). A copy, because the
 * callee owns a by-reference argument and may write it, as the psABI
 * allows. There is no __int128 at RV32, so nothing here is integer
 * arithmetic. */

static void copy16(struct rv_fn *F, long to, long from)
{
    if (to == from)
        return;
    for (int q = 0; q < 16; q += 8) {
        ld_sp(F, A_LO, from + q, 4, 1);
        ld_sp(F, A_HI, from + q + 4, 4, 1);
        st_sp(F, A_LO, to + q, 4);
        st_sp(F, A_HI, to + q + 4, 4);
    }
}

/* Where a long double result goes: its slot, or the tf area's spare
 * sixteen bytes when nothing reads it (the helper writes it regardless). */
static long tf_result(struct rv_fn *F, int v)
{
    if (v < 0)
        return F->tfa + 32;
    need16(F, v);
    return F->slot[v] >= 0 ? F->slot[v] : F->tfa + 32;
}

/* An operand into the tf area at `at`, and a1/a2 pointing there. */
static void tf_operand(struct rv_fn *F, int v, long at, int reg)
{
    need16(F, v);
    copy16(F, F->tfa + at, sslot(F, v));
    addr_sp(F, reg, F->tfa + at);
}

static void gen_ld32(struct rv_fn *F, struct ir_ins *i)
{
    struct code *t = F->t;
    const char *name;

    if (i->imm_b || i->memoff)
        rv_refuse(F, i, "a folded operand on a long double at RV32");
    switch (i->op) {
    case IR_LDVAR: case IR_STVAR: case IR_MOV:
        need16(F, i->a);
        need16(F, i->dst);
        if (F->slot[i->dst] >= 0)
            copy16(F, F->slot[i->dst], sslot(F, i->a));
        return;
    case IR_LOAD: {
        int ra_ = rdr(F, i->a, ADDR);
        long d = tf_result(F, i->dst);
        for (int q = 0; q < 16; q += 4) {
            rv_load(t, A_LO, ra_, q, 4, 0, 32);
            st_sp(F, A_LO, d + q, 4);
        }
        return;
    }
    case IR_STORE: {
        long s;
        need16(F, i->b);
        s = sslot(F, i->b);
        int ra_ = rdr(F, i->a, ADDR);
        for (int q = 0; q < 16; q += 4) {
            ld_sp(F, A_LO, s + q, 4, 1);
            rv_store(t, A_LO, ra_, q, 4, 32);
        }
        return;
    }
    case IR_CONST: {                    /* sign-extended, as irgen made it */
        long d = tf_result(F, i->dst);
        long long v = (long long)i->imm;
        for (int q = 0; q < 4; q++) {
            long long word = q < 2 ? (long long)(int)(v >> (32 * q))
                                   : (v < 0 ? -1 : 0);
            rv_li(t, A_LO, word, 32);
            st_sp(F, A_LO, d + 4 * q, 4);
        }
        return;
    }
    default:
        break;
    }

    if (i->flt && i->op == IR_NEG) {    /* bit 127: right for -0.0, NaN */
        long d = tf_result(F, i->dst);
        need16(F, i->a);
        copy16(F, d, sslot(F, i->a));
        ld_sp(F, A_LO, d + 12, 4, 1);
        rv_li(t, A_HI, (long long)0x80000000LL, 32);
        rv_alu(t, RV_XOR, A_LO, A_LO, A_HI, 0);
        st_sp(F, A_LO, d + 12, 4);
        return;
    }
    if (i->flt && (i->op == IR_ADD || i->op == IR_SUB || i->op == IR_MUL ||
                   i->op == IR_DIV)) {
        tf_operand(F, i->a, 0, RV_A1);
        tf_operand(F, i->b, 16, RV_A2);
        addr_sp(F, RV_A0, tf_result(F, i->dst));
        call_helper(F, i->op == IR_ADD ? "__addtf3"
                       : i->op == IR_SUB ? "__subtf3"
                       : i->op == IR_MUL ? "__multf3" : "__divtf3");
        return;
    }
    if (i->flt && i->op == IR_CMP) {
        tf_operand(F, i->a, 0, RV_A0);
        tf_operand(F, i->b, 16, RV_A1);
        call_helper(F, tf_cmp_name(i->pred));
        {
            int d = wreg(F, i->dst, ACC);
            cmp_to_reg(F, i->pred, 1, RV_A0, RV_ZERO, d);
            wrote(F, i->dst, d);
        }
        return;
    }
    if (i->op == IR_I2F || i->op == IR_F2F || i->op == IR_F2I) {
        int sw = i->size, dw = i->w;
        if (dw == 16) {
            /* to long double: the source into a1 (a pair into a1:a2, as
             * one parallel move -- it may be in a0:a1), then the result's
             * address into a0, which nothing is still reading */
            if (i->op == IR_F2F && rv_hfw(sw)) {
                fload_v(F, i->a, RV_FA0, sw == 8);   /* a float argument */
            } else if (sw == 8 && F->wide[i->a]) {
                int dr[2] = { RV_A1, RV_A2 }, vr[2], hf[2] = { 0, 1 };
                vr[0] = vr[1] = i->a;
                set_args_half(F, dr, vr, hf, 2);
            } else if (sw == 8 && i->op == IR_I2F) {
                rd(F, i->a, RV_A1);     /* a narrow source asked as 64 */
                rv_mv(t, RV_A2, RV_ZERO);
            } else {
                rd(F, i->a, RV_A1);
            }
            if (i->op == IR_I2F)
                name = sw == 8 ? (i->sign ? "__floatditf" : "__floatunditf")
                               : (i->sign ? "__floatsitf" : "__floatunsitf");
            else
                name = sw == 8 ? "__extenddftf2" : "__extendsftf2";
            addr_sp(F, RV_A0, tf_result(F, i->dst));
            call_helper(F, name);
            return;
        }
        if (sw == 16) {
            /* from long double: the operand by reference in a0, the
             * result in a0 (a pair at eight bytes) */
            tf_operand(F, i->a, 0, RV_A0);
            if (i->op == IR_F2F)
                name = dw == 8 ? "__trunctfdf2" : "__trunctfsf2";
            else
                name = dw == 8 ? (i->sign ? "__fixtfdi" : "__fixunstfdi")
                               : (i->sign ? "__fixtfsi" : "__fixunstfsi");
            call_helper(F, name);
            if (i->dst >= 0) {
                if (i->op == IR_F2F && rv_hfw(dw))
                    fdone(F, i->dst, RV_FA0, dw == 8);   /* a float result */
                else if (dw == 8 && F->wide[i->dst])
                    wr64(F, i->dst, RV_A0, RV_A1);
                else
                    wr(F, i->dst, RV_A0);
            }
            return;
        }
    }
    rv_refuse(F, i, "this operation on a long double at RV32");
}

static void gen_ins128(struct rv_fn *F, struct ir_ins *i)
{
    if (F->xlen == 32) {
        gen_ld32(F, i);
        return;
    }
    struct code *t = F->t;
    long k;

    if (i->imm_b)          /* pass_immfold leaves width 16 alone */
        rv_refuse(F, i, "a folded immediate on a 128-bit operation");

    switch (i->op) {
    case IR_CONST:                      /* sign-extended, as irgen made it */
        rv_li(t, A_LO, i->imm, 64);
        rv_li(t, A_HI, i->imm < 0 ? -1 : 0, 64);
        st128(F, i->dst, A_LO, A_HI);
        return;

    case IR_LDVAR: case IR_STVAR: case IR_MOV:
        /* slot to slot: the local is `a` of an ldvar and `dst` of an
         * stvar, and one of sixteen bytes is never in a register */
        if (!is16(F, i->a) || !is16(F, i->dst))
            rv_refuse(F, i, "a sixteen-byte copy of a narrower value");
        if (F->slot[i->dst] < 0 || F->slot[i->dst] == F->slot[i->a])
            return;
        ld128(F, i->a, A_LO, A_HI);
        st128(F, i->dst, A_LO, A_HI);
        return;


    case IR_LOAD: {
        /* Two doublewords. C has the address 16-aligned (a long double
         * constant included: irgen places it so); a packed member is
         * read the way the eight-byte path reads one. */
        int ra_ = rdr(F, i->a, ADDR);
        if (!is16(F, i->dst) || i->memoff)
            rv_refuse(F, i, "a sixteen-byte load into a narrower value");
        rv_load(t, A_LO, ra_, 0, 8, 0, 64);
        rv_load(t, A_HI, ra_, 8, 8, 0, 64);
        st128(F, i->dst, A_LO, A_HI);
        return;
    }
    case IR_STORE: {
        if (!is16(F, i->b) || i->memoff)
            rv_refuse(F, i, "a sixteen-byte store of a narrower value");
        ld128(F, i->b, A_LO, A_HI);
        int ra_ = rdr(F, i->a, ADDR);
        rv_store(t, A_LO, ra_, 0, 8, 64);
        rv_store(t, A_HI, ra_, 8, 8, 64);
        return;
    }

    case IR_EXT:
        /* to 128 bits: the low word extended to 64 as the source asks,
         * and the high word its sign or zero (a narrowing EXT is an
         * ordinary slot read, below gen_ins128) */
        if (is16(F, i->a) && i->size == 16) {
            ld128(F, i->a, A_LO, A_HI);
        } else {
            int r = rdr(F, i->a, A_LO);
            ext_reg(F, A_LO, r, i->size, i->sign);
            if (i->sign) rv_shift_imm(t, RV_SRA, A_HI, A_LO, 63, 0, 64);
            else         rv_mv(t, A_HI, RV_ZERO);
        }
        st128(F, i->dst, A_LO, A_HI);
        return;

    case IR_SELECT:
        rv_refuse(F, i, "a 128-bit select");
        return;

    case IR_BRZ: case IR_BRNZ:
        ld128(F, i->a, A_LO, A_HI);
        rv_alu(t, RV_OR, A_LO, A_LO, A_HI, 0);
        branch_if(F, i->op == IR_BRZ ? RV_BEQ : RV_BNE, A_LO, RV_ZERO,
                  i->label);
        return;

    default:
        break;
    }

    if (i->flt) {
        switch (i->op) {
        case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV:
            ld128(F, i->a, RV_A0, RV_A1);
            ld128(F, i->b, RV_A2, RV_A3);
            call_helper(F, i->op == IR_ADD ? "__addtf3"
                           : i->op == IR_SUB ? "__subtf3"
                           : i->op == IR_MUL ? "__multf3" : "__divtf3");
            st128(F, i->dst, RV_A0, RV_A1);
            return;
        case IR_NEG:                    /* bit 127: right for -0.0, NaN */
            ld128(F, i->a, A_LO, A_HI);
            rv_li(t, B_LO, (long long)(-9223372036854775807LL - 1), 64);
            rv_alu(t, RV_XOR, A_HI, A_HI, B_LO, 0);
            st128(F, i->dst, A_LO, A_HI);
            return;
        case IR_CMP: {
            ld128(F, i->a, RV_A0, RV_A1);
            ld128(F, i->b, RV_A2, RV_A3);
            call_helper(F, tf_cmp_name(i->pred));
            int d = wreg(F, i->dst, ACC);
            cmp_to_reg(F, i->pred, 1, RV_A0, RV_ZERO, d);
            wrote(F, i->dst, d);
            return;
        }
        default:
            break;
        }
    }

    switch (i->op) {
    case IR_I2F: case IR_F2I: case IR_F2F: {
        const char *name;
        int sw = i->size, dw = i->w;
        if (i->op == IR_I2F)
            name = sw == 16
                 ? (dw == 16 ? (i->sign ? "__floattitf" : "__floatuntitf")
                    : dw == 8 ? (i->sign ? "__floattidf" : "__floatuntidf")
                    : (i->sign ? "__floattisf" : "__floatuntisf"))
                 : sw == 8 ? (i->sign ? "__floatditf" : "__floatunditf")
                 : (i->sign ? "__floatsitf" : "__floatunsitf");
        else if (i->op == IR_F2I)
            name = sw == 16
                 ? (dw == 16 ? (i->sign ? "__fixtfti" : "__fixunstfti")
                    : dw == 8 ? (i->sign ? "__fixtfdi" : "__fixunstfdi")
                    : (i->sign ? "__fixtfsi" : "__fixunstfsi"))
                 : sw == 8 ? (i->sign ? "__fixdfti" : "__fixunsdfti")
                 : (i->sign ? "__fixsfti" : "__fixunssfti");
        else
            name = dw == 16 ? (sw == 8 ? "__extenddftf2" : "__extendsftf2")
                 : dw == 8 ? "__trunctfdf2" : "__trunctfsf2";
        if (sw == 16) {
            ld128(F, i->a, RV_A0, RV_A1);
        } else if (i->op != IR_I2F && rv_hfw(sw)) {
            fload_v(F, i->a, RV_FA0, sw == 8);       /* a float argument */
        } else if (i->op == IR_I2F && sw <= 4) {
            /* an int or unsigned argument: sign-extended either way, the
             * psABI's rule for every 32-bit value (rd32) */
            int r = rd32(F, i->a, RV_A0);
            if (r != RV_A0) rv_mv(t, RV_A0, r);
        } else {
            rd(F, i->a, RV_A0);
        }
        call_helper(F, name);
        if (dw == 16)
            st128(F, i->dst, RV_A0, RV_A1);
        else if (i->dst >= 0 && i->op != IR_F2I && rv_hfw(dw))
            fdone(F, i->dst, RV_FA0, dw == 8);       /* a float result */
        else if (i->dst >= 0)
            wr(F, i->dst, RV_A0);
        return;
    }

    case IR_ADD: case IR_SUB:
        /* No carry flag: after lo = a + b, `sltu lo, b` is the carry,
         * and before a - b, `sltu a, b` is the borrow. */
        ld128(F, i->a, A_LO, A_HI);
        ld128(F, i->b, B_LO, W_HI);
        if (i->op == IR_ADD) {
            rv_alu(t, RV_ADD, A_LO, A_LO, B_LO, 0);
            rv_alu(t, RV_SLTU, SCR, A_LO, B_LO, 0);
            rv_alu(t, RV_ADD, A_HI, A_HI, W_HI, 0);
            rv_alu(t, RV_ADD, A_HI, A_HI, SCR, 0);
        } else {
            rv_alu(t, RV_SLTU, SCR, A_LO, B_LO, 0);
            rv_alu(t, RV_SUB, A_LO, A_LO, B_LO, 0);
            rv_alu(t, RV_SUB, A_HI, A_HI, W_HI, 0);
            rv_alu(t, RV_SUB, A_HI, A_HI, SCR, 0);
        }
        st128(F, i->dst, A_LO, A_HI);
        return;

    case IR_AND: case IR_OR: case IR_XOR: {
        int op = i->op == IR_AND ? RV_AND : i->op == IR_OR ? RV_OR : RV_XOR;
        ld128(F, i->a, A_LO, A_HI);
        ld128(F, i->b, B_LO, W_HI);
        rv_alu(t, op, A_LO, A_LO, B_LO, 0);
        rv_alu(t, op, A_HI, A_HI, W_HI, 0);
        st128(F, i->dst, A_LO, A_HI);
        return;
    }

    case IR_BNOT:
        ld128(F, i->a, A_LO, A_HI);
        rv_alu_imm(t, RV_XOR, A_LO, A_LO, -1, 0);
        rv_alu_imm(t, RV_XOR, A_HI, A_HI, -1, 0);
        st128(F, i->dst, A_LO, A_HI);
        return;

    case IR_NEG:                        /* 0 - a, the borrow from lo */
        ld128(F, i->a, A_LO, A_HI);
        rv_alu(t, RV_SLTU, SCR, RV_ZERO, A_LO, 0);          /* snez */
        rv_alu(t, RV_SUB, A_LO, RV_ZERO, A_LO, 0);
        rv_alu(t, RV_SUB, A_HI, RV_ZERO, A_HI, 0);
        rv_alu(t, RV_SUB, A_HI, A_HI, SCR, 0);
        st128(F, i->dst, A_LO, A_HI);
        return;

    case IR_MUL:
        /* The low 128 bits of the product, which is the same for signed
         * and unsigned: lo*lo in full (mul, mulhu) plus each cross term's
         * low word in the high half. */
        ld128(F, i->a, A_LO, A_HI);
        ld128(F, i->b, B_LO, W_HI);
        rv_muldiv(t, RV_MULHU, SCR, A_LO, B_LO, 0);
        rv_muldiv(t, RV_MUL, A_HI, A_HI, B_LO, 0);
        rv_muldiv(t, RV_MUL, W_HI, A_LO, W_HI, 0);
        rv_muldiv(t, RV_MUL, A_LO, A_LO, B_LO, 0);
        rv_alu(t, RV_ADD, SCR, SCR, A_HI, 0);
        rv_alu(t, RV_ADD, A_HI, SCR, W_HI, 0);
        st128(F, i->dst, A_LO, A_HI);
        return;

    case IR_DIV: case IR_MOD:
        ld128(F, i->a, RV_A0, RV_A1);
        ld128(F, i->b, RV_A2, RV_A3);
        call_helper(F, i->op == IR_DIV
                       ? (i->sign ? "__divti3" : "__udivti3")
                       : (i->sign ? "__modti3" : "__umodti3"));
        st128(F, i->dst, RV_A0, RV_A1);
        return;

    case IR_SHL: case IR_SHR:
        if (rv_const_of(F, i->b, &k) && k >= 0 && k < 128) {
            ld128(F, i->a, A_LO, A_HI);
            shift128_imm(F, i, (int)k);
            st128(F, i->dst, A_LO, A_HI);
            return;
        }
        {
            /* the count first: it may be in a0 or a1, which the value's
             * words are about to take */
            int r = rd32(F, i->b, RV_A2);
            if (r != RV_A2) rv_mv(t, RV_A2, r);
        }
        ld128(F, i->a, RV_A0, RV_A1);
        call_helper(F, i->op == IR_SHL ? "__ashlti3"
                       : i->sign ? "__ashrti3" : "__lshrti3");
        st128(F, i->dst, RV_A0, RV_A1);
        return;

    case IR_CMP: {
        int d = wreg(F, i->dst, ACC);
        if (i->pred == B_EQ || i->pred == B_NE) {
            ld128(F, i->a, A_LO, A_HI);
            ld128(F, i->b, B_LO, W_HI);
            rv_alu(t, RV_XOR, A_LO, A_LO, B_LO, 0);
            rv_alu(t, RV_XOR, A_HI, A_HI, W_HI, 0);
            rv_alu(t, RV_OR, A_LO, A_LO, A_HI, 0);
            cmp_to_reg(F, i->pred, 0, A_LO, RV_ZERO, d);
        } else {
            /* x < y: the high words decide unless they are equal, and
             * then the low words do, unsigned. > and <= swap the two. */
            int swap = i->pred == B_GT || i->pred == B_LE;
            ld128(F, swap ? i->b : i->a, A_LO, A_HI);
            ld128(F, swap ? i->a : i->b, B_LO, W_HI);
            rv_alu(t, i->sign ? RV_SLT : RV_SLTU, SCR, A_HI, W_HI, 0);
            rv_alu(t, RV_XOR, A_HI, A_HI, W_HI, 0);
            rv_alu_imm(t, RV_SLTU, A_HI, A_HI, 1, 0);       /* seqz */
            rv_alu(t, RV_SLTU, A_LO, A_LO, B_LO, 0);
            rv_alu(t, RV_AND, A_LO, A_LO, A_HI, 0);
            rv_alu(t, RV_OR, d, A_LO, SCR, 0);
            if (i->pred == B_GE || i->pred == B_LE)
                rv_alu_imm(t, RV_XOR, d, d, 1, 0);
        }
        wrote(F, i->dst, d);
        return;
    }

    default:
        rv_refuse(F, i, "this operation on a 128-bit value");
    }
}

/* ---- one instruction ------------------------------------------------------ */

/* ---- floating point on the FPU (F, D) -------------------------------------
 *
 * Each operation reads its operands where they live (fsrc), computes into
 * the result's f register or ft0 (fdst), and commits (fdone): with the
 * operands and the result in f registers, `fadd.s fs1, fs0, ft3` and
 * nothing else. ft0-ft2 are the scratch, never allocated.
 *
 * NO fused multiply-add: `a * b + c` is two roundings in C unless
 * contraction is allowed, and EmbCC does not contract (clang does by
 * default within an expression, -ffp-contract=on). */
#define FT0 RV_FT0
#define FT1 RV_FT1
#define FT2 RV_FT2

static void gen_fp(struct rv_fn *F, int n)
{
    struct ir_ins *i = &F->fn->ins[n];
    struct code *t = F->t;
    int dbl, a, b, d;

    switch (i->op) {
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV:
        dbl = i->w == 8;
        if (i->imm_b)
            rv_refuse(F, i, "a folded floating-point immediate");
        a = fsrc(F, i->a, FT0, dbl);
        b = fsrc(F, i->b, FT1, dbl);
        d = fdst(F, i->dst, FT0);
        rv_farith(t, i->op == IR_ADD ? RV_FADD : i->op == IR_SUB ? RV_FSUB
                   : i->op == IR_MUL ? RV_FMUL : RV_FDIV, d, a, b, dbl);
        fdone(F, i->dst, d, dbl);
        return;
    case IR_NEG:                 /* the sign flipped: right for -0.0, NaN */
        dbl = i->w == 8;
        a = fsrc(F, i->a, FT0, dbl);
        d = fdst(F, i->dst, FT0);
        rv_fsgnj(t, RV_FSGNJN, d, a, a, dbl);
        fdone(F, i->dst, d, dbl);
        return;
    case IR_SQRT:
        dbl = i->w == 8;
        a = fsrc(F, i->a, FT0, dbl);
        d = fdst(F, i->dst, FT0);
        rv_fsqrt(t, d, a, dbl);
        fdone(F, i->dst, d, dbl);
        return;
    case IR_CMP: {
        /* feq, flt, fle -- each FALSE for an unordered pair, which is C's
         * answer for every relation but `!=`: that one is feq inverted,
         * true for a NaN. > and >= are < and <= with the operands
         * swapped, as on the integer side. */
        int kind, sw = 0, inv = 0, r;
        struct ir_ins *nx = n + 1 < F->fn->nins ? &F->fn->ins[n + 1]
                                                : (struct ir_ins *)0;
        dbl = i->w == 8;
        if (i->imm_b)
            rv_refuse(F, i, "a folded floating-point immediate");
        switch (i->pred) {
        case B_EQ: kind = RV_FEQ; break;
        case B_NE: kind = RV_FEQ; inv = 1; break;
        case B_LT: kind = RV_FLT; break;
        case B_LE: kind = RV_FLE; break;
        case B_GT: kind = RV_FLT; sw = 1; break;
        default:   kind = RV_FLE; sw = 1; break;      /* B_GE */
        }
        a = fsrc(F, i->a, FT0, dbl);
        b = fsrc(F, i->b, FT1, dbl);
        /* Read only by the branch after it: the 0 or 1 is the branch's
         * operand, and `!=` is the inverted branch rather than an xori. */
        if (nx && (nx->op == IR_BRZ || nx->op == IR_BRNZ) &&
            nx->a == i->dst && F->usecnt && i->dst >= 0 &&
            F->usecnt[i->dst] == 1) {
            int cond = nx->op == IR_BRNZ ? RV_BNE : RV_BEQ;
            rv_fcmp(t, kind, SCR, sw ? b : a, sw ? a : b, dbl);
            if (inv)
                cond = invert_branch(cond);
            branch_if(F, cond, SCR, RV_ZERO, nx->label);
            F->skip_next = 1;
            return;
        }
        r = wreg(F, i->dst, ACC);
        rv_fcmp(t, kind, r, sw ? b : a, sw ? a : b, dbl);
        if (inv)
            rv_alu_imm(t, RV_XOR, r, r, 1, 0);
        wrote(F, i->dst, r);
        return;
    }
    case IR_I2F: {
        /* fcvt.s.w reads the low 32 bits of its source and nothing else,
         * so an int needs no extension at RV64; a narrower one is
         * extended to 32 first, and a 32-bit source of a 64-bit
         * conversion (wide_map's narrow shape) zero-extended to 64. */
        int ity, r;
        dbl = i->w == 8;
        if (i->size == 8) {
            ity = i->sign ? RV_CVT_L : RV_CVT_LU;
            r = rdr(F, i->a, ACC);
            if (i->a >= 0 && !F->wide[i->a] && !is16(F, i->a)) {
                ext_reg(F, ACC, r, 4, 0);
                r = ACC;
                ity = RV_CVT_L;
            }
        } else {
            ity = i->sign ? RV_CVT_W : RV_CVT_WU;
            r = rdr(F, i->a, ACC);
            if (i->size < 4) {
                ext_reg(F, ACC, r, i->size, i->sign);
                r = ACC;
                ity = RV_CVT_W;
            }
        }
        d = fdst(F, i->dst, FT0);
        rv_fcvt_from_int(t, d, r, ity, dbl);
        fdone(F, i->dst, d, dbl);
        return;
    }
    case IR_F2I: {
        /* Toward zero, as C converts. At RV64 the word forms leave the
         * 32-bit result sign-extended -- fcvt.wu.s too -- which is the
         * invariant a 32-bit value keeps there (sext_map counts on it). */
        int ity = i->w == 8 ? (i->sign ? RV_CVT_L : RV_CVT_LU)
                            : (i->sign || i->w < 4 ? RV_CVT_W : RV_CVT_WU);
        dbl = i->size == 8;
        a = fsrc(F, i->a, FT0, dbl);
        d = wreg(F, i->dst, ACC);
        rv_fcvt_to_int(t, d, a, ity, dbl);
        wrote(F, i->dst, d);
        return;
    }
    case IR_F2F:
        a = fsrc(F, i->a, FT0, i->size == 8);
        d = fdst(F, i->dst, FT0);
        rv_fcvt_fp(t, d, a, i->w == 8);
        fdone(F, i->dst, d, i->w == 8);
        return;
    default:
        rv_refuse(F, i, "this floating-point operation");
    }
}

/* The unit being compiled: a floating-point constant goes into its
 * .rodata (ir_intern_aligned), which the driver lays out after code
 * generation. */
static struct ir_unit *g_rv_iu;

/* A floating-point constant into f register `fd`: from .rodata --
 * `auipc` and `flw`/`fld`, the pair one R_RISCV_PCREL_HI20/LO12_I
 * relocates, as clang does -- unless it is one `lui` away (a float whose
 * low twelve bits are clear), when that and an fmv are as short and need
 * no load. A double at RV64 was up to eight instructions of li. */
static void fconst(struct rv_fn *F, int fd, long long bits, int w)
{
    struct code *t = F->t;
    if (w == 4 && rv_li_len(bits, F->xlen) <= 4) {
        rv_li(t, ACC, bits, F->xlen);
        rv_fmv_from_x(t, fd, ACC, 0);
        return;
    }
    if (!g_rv_iu)
        internal_error("riscv: a floating-point constant with no unit");
    {
        unsigned char *b = xmalloc(8);
        int idx, at, rvc = rv_compress_enabled();
        for (int k = 0; k < w; k++)
            b[k] = (unsigned char)((unsigned long long)bits >> (8 * k));
        idx = ir_intern_aligned(g_rv_iu, (const char *)b, w, w);
        if (g_rv_iu->strs[idx].bytes != (const char *)b)
            free(b);
        /* the pair is patched as one: neither half may change size */
        rv_set_compress(0, F->xlen);
        at = t->len;
        rv_auipc(t, ACC, 0);
        rv_fload(t, fd, ACC, 0, w == 8);
        rv_set_compress(rvc, F->xlen);
        note_str(F->st, at, idx, RK_RISCV_PCREL_HI20);
        note_str(F->st, at + 4, idx, RK_RISCV_PCREL_LO12_I);
    }
}

/* The copies, loads, stores and constants of a value with an f-register
 * home, straight between the homes: flw into it, fsw out of it, fmv
 * between two. Returns 0 for anything else, which the integer paths
 * lower -- correctly, since rd and wr cross between the files. */
static int gen_fp_move(struct rv_fn *F, int n)
{
    struct ir_ins *i = &F->fn->ins[n];
    struct code *t = F->t;
    int d = i->dst, a = i->a;

    if (!F->floc)
        return 0;
    switch (i->op) {
    case IR_MOV: case IR_BITCAST: {
        int w;
        if (!in_freg(F, d) && !in_freg(F, a))
            return 0;
        w = in_freg(F, d) ? F->fw[d] : F->fw[a];
        if (in_freg(F, d) && in_freg(F, a) && F->fw[d] != F->fw[a])
            internal_error("riscv: %s: a copy between a float and a double "
                           "in f registers", F->fn->name);
        /* a narrowing copy of a double reads its low word: the integer
         * paths do that, crossing */
        if (i->w && i->w != w)
            return 0;
        if (in_freg(F, d))
            fload_v(F, a, F->floc[d], w == 8);
        else
            fdone(F, d, F->floc[a], w == 8);
        return 1;
    }
    case IR_LDVAR:
        if (in_freg(F, a)) {
            if (i->size != F->fw[a])
                internal_error("riscv: %s: a %d-byte read of a %d-byte "
                               "floating-point local", F->fn->name, i->size,
                               F->fw[a]);
            fdone(F, d, F->floc[a], i->size == 8);
            return 1;
        }
        if (in_freg(F, d) && i->size == F->fw[d]) {
            fload_v(F, a, F->floc[d], i->size == 8);
            return 1;
        }
        return 0;
    case IR_STVAR:
        if (in_freg(F, d)) {
            if (i->size != F->fw[d])
                internal_error("riscv: %s: a %d-byte write of a %d-byte "
                               "floating-point local", F->fn->name, i->size,
                               F->fw[d]);
            fload_v(F, a, F->floc[d], i->size == 8);
            return 1;
        }
        if (in_freg(F, a) && i->size == F->fw[a]) {
            fdone(F, d, F->floc[a], i->size == 8);
            return 1;
        }
        return 0;
    case IR_LOAD:
        if (in_freg(F, d) && i->size == F->fw[d]) {
            int ra_ = rdr(F, a, ADDR);
            rv_fload(t, F->floc[d], ra_, i->memoff, i->size == 8);
            return 1;
        }
        return 0;
    case IR_STORE:
        if (in_freg(F, i->b) && i->size == F->fw[i->b]) {
            int ra_ = rdr(F, a, ADDR);
            rv_fstore(t, F->floc[i->b], ra_, i->memoff, i->size == 8);
            return 1;
        }
        return 0;
    case IR_CONST: {
        int fd;
        long long bits = i->imm;
        if (!in_freg(F, d))
            return 0;
        fd = F->floc[d];
        if (F->fw[d] == 4)
            bits = (long long)(int)(bits & 0xffffffffLL);
        if (bits == 0) {
            /* +0.0: from x0 -- at RV32 a double through fcvt.d.w */
            if (F->fw[d] == 8 && F->xlen == 32)
                rv_fcvt_from_int(t, fd, RV_ZERO, RV_CVT_W, 1);
            else
                rv_fmv_from_x(t, fd, RV_ZERO, F->fw[d] == 8);
            return 1;
        }
        fconst(F, fd, bits, F->fw[d]);
        return 1;
    }
    case IR_SELECT: {
        /* dst = a ? b : c, each arm loaded straight into dst's register */
        int take_c, done, cond, dbl;
        if (!in_freg(F, d))
            return 0;
        dbl = F->fw[d] == 8;
        if (F->xlen == 32 && i->size == 8) {
            rd64(F, a, A_LO, A_HI);
            rv_alu(t, RV_OR, SCR, A_LO, A_HI, 0);
            cond = SCR;
        } else {
            cond = i->size == 4 ? rd32(F, a, SCR) : rdr(F, a, SCR);
        }
        take_c = rv_b_placeholder(t, RV_BEQ, cond, RV_ZERO);
        fload_v(F, i->b, F->floc[d], dbl);
        done = rv_j_placeholder(t, RV_ZERO);
        rv_patch_b(t, take_c, t->len);
        fload_v(F, i->c, F->floc[d], dbl);
        rv_patch_j(t, done, t->len);
        return 1;
    }
    default:
        return 0;
    }
}

static void gen_ins(struct rv_fn *F, int n)
{
    struct ir_func *fn = F->fn;
    struct ir_ins *i = &fn->ins[n];
    struct code *t = F->t;

    g_rv_cur = i;

    /* -g: a line-table row wherever the source line changes, as the
     * other backends record them. t->len is where this instruction's
     * code begins. */
    if (F->want_debug && F->fn->ins[n].line) {
        struct ir_func *dfn = F->fn;
        long line = dfn->ins[n].line;
        struct ir_line *last = dfn->nlines ? &dfn->lines[dfn->nlines - 1]
                                           : (struct ir_line *)0;
        if (last && last->off == t->len) {
            last->line = line;
        } else if (!last || last->line != line) {
            if (dfn->nlines == dfn->linecap) {
                dfn->linecap = dfn->linecap ? dfn->linecap * 2 : 8;
                dfn->lines = xrealloc(dfn->lines, (size_t)dfn->linecap *
                                      sizeof *dfn->lines);
            }
            dfn->lines[dfn->nlines].off = t->len;
            dfn->lines[dfn->nlines].line = line;
            dfn->nlines++;
        }
    }
    int wordop;

    if (rv_fp_hw(i)) {
        gen_fp(F, n);
        return;
    }
    if (gen_fp_move(F, n))
        return;

    if (F->w16 && rv_ins128(F, i)) {
        gen_ins128(F, i);
        return;
    }

    /* Floating point is a CALL here, not an instruction. Only the
     * ARITHMETIC is flagged: `flt` is set on a return, a move and a call
     * too, and those carry the value as the bits it already is, which the
     * integer paths below move exactly the right number of. */
    if (i->flt && (i->op == IR_ADD || i->op == IR_SUB || i->op == IR_MUL ||
                   i->op == IR_DIV || i->op == IR_MOD || i->op == IR_NEG ||
                   i->op == IR_CMP || i->op == IR_SQRT)) {
        const char *name = fp_binop_name(i->op, i->w);
        if (i->w != 4 && i->w != 8)
            rv_refuse(F, i, "a long double (no binary128 arithmetic yet)");
        if (name) {
            if (i->imm_b)
                rv_refuse(F, i, "a folded floating-point immediate");
            fp_args2(F, i);
            call_helper(F, name);
            fp_result(F, i->dst, i->w);
            return;
        }
        if (i->op == IR_NEG) {
            /* The sign bit, flipped. A call would be correct and this is
             * two instructions -- and unlike a subtraction from zero it
             * is right for -0.0 and for a NaN. */
            if (i->w == 8 && F->xlen == 32) {
                rd64(F, i->a, A_LO, A_HI);
                rv_li(t, B_LO, 0x80000000LL, 32);
                rv_alu(t, RV_XOR, A_HI, A_HI, B_LO, 0);
                wr64(F, i->dst, A_LO, A_HI);
            } else {
                rd(F, i->a, ACC);
                rv_li(t, TMP, i->w == 8 ? (long long)(-9223372036854775807LL - 1)
                                        : 0x80000000LL,
                      i->w == 8 ? 64 : F->xlen);
                rv_alu(t, RV_XOR, ACC, ACC, TMP, 0);
                wr(F, i->dst, ACC);
            }
            return;
        }
        if (i->op == IR_CMP) {
            fp_args2(F, i);
            call_helper(F, fp_cmp_name(i->pred, i->w));
            /* The helper's int is the answer's sign (fp_cmp_name). Read
             * only by the branch after it, it IS the branch: `bltz a0`
             * where the 0 or 1 was built in t0, moved home and tested. */
            if (n + 1 < F->fn->nins && F->usecnt && i->dst >= 0 &&
                F->usecnt[i->dst] == 1) {
                struct ir_ins *nx = &F->fn->ins[n + 1];
                if ((nx->op == IR_BRZ || nx->op == IR_BRNZ) &&
                    nx->a == i->dst) {
                    int cond, r1 = RV_A0, r2 = RV_ZERO;
                    switch (i->pred) {
                    case B_EQ: cond = RV_BEQ; break;
                    case B_NE: cond = RV_BNE; break;
                    case B_LT: cond = RV_BLT; break;
                    case B_GE: cond = RV_BGE; break;
                    case B_GT: cond = RV_BLT; r1 = RV_ZERO; r2 = RV_A0; break;
                    default:   cond = RV_BGE; r1 = RV_ZERO; r2 = RV_A0; break;
                    }
                    if (nx->op == IR_BRZ)
                        cond = invert_branch(cond);
                    branch_if(F, cond, r1, r2, nx->label);
                    F->skip_next = 1;
                    return;
                }
            }
            /* Otherwise from a0 straight into the destination: each
             * cmp_to_reg form reads its operands in its first
             * instruction, so the destination may be a0 itself. */
            {
                int d = wreg(F, i->dst, ACC);
                cmp_to_reg(F, i->pred, 1, RV_A0, RV_ZERO, d);
                wrote(F, i->dst, d);
            }
            return;
        }
        if (i->op == IR_SQRT)
            rv_refuse(F, i, "__builtin_sqrt (a libm routine here, not an "
                            "instruction)");
        rv_refuse(F, i, "this floating-point operation");
    }

    /* named here, before the pair test below would call it "this
     * operation at 64 bits": its w is a host pointer's */
    if (i->op == IR_FRAMEADDR)
        rv_refuse(F, i, "__builtin_frame_address or __builtin_return_address "
                        "(RISC-V code keeps no frame-pointer chain)");

    /* (At RV64 a call or a return of one is gen_call's and IR_RET's.) */
    if (i->w > 8 &&
        !(F->w16 && (i->op == IR_CALL || i->op == IR_RET)))
        rv_refuse(F, i, "a 128-bit value");

    /* Does this instruction work on a value that needs a register pair?
     * NOT `i->w == 8` everywhere: the width field is the OPERATION's, and
     * several instructions do not set it at all. IR_STVAR and IR_STORE
     * carry a `size` and no `w`, so asking `w` says four and stores half
     * of a long long; IR_RET carries neither. The wide map, built from
     * each value's defining instruction, is what knows. */
    /* The high word of a 64-bit value, shifted: one register (narrow_shr). */
    if (F->xlen == 32 && i->op == IR_SHR && F->nshr && i->dst >= 0 &&
        F->nshr[i->dst]) {
        int k = (int)i->imm - 32, d = wreg(F, i->dst, A_LO), hi;
        if (in_reg(F, i->a)) {
            hi = F->loc[i->a] + 1;             /* the pair's high register */
        } else if (in_freg(F, i->a)) {         /* a double: its high word */
            rd64(F, i->a, A_LO, A_HI);
            hi = A_HI;
        } else {
            ld_sp(F, A_HI, sslot(F, i->a) + 4, 4, 1);
            hi = A_HI;
        }
        if (k)
            rv_shift_imm(t, i->sign ? RV_SRA : RV_SRL, d, hi, k, 0, 32);
        else if (d != hi)
            rv_mv(t, d, hi);
        wrote(F, i->dst, d);
        return;
    }
    if (F->xlen == 32) {
        int wide = i->w == 8;
        switch (i->op) {
        case IR_STVAR: wide = i->size == 8 || (i->a >= 0 && F->wide[i->a]);
                       break;
        case IR_STORE: wide = i->size == 8 || (i->b >= 0 && F->wide[i->b]);
                       break;
        case IR_LDVAR:
        case IR_LOAD:  wide = i->dst >= 0 && F->wide[i->dst]; break;
        case IR_MOV:
        case IR_SELECT:
            /* The same rule as the map's propagation, and it has to be
             * the same rule: a copy that says four bytes copies four,
             * whatever the width of what it reads. */
            wide = i->w != 4 &&
                   ((i->dst >= 0 && F->wide[i->dst]) ||
                    (i->op == IR_MOV && i->a >= 0 && F->wide[i->a]));
            break;
        default: break;
        }
        if (wide && i->op != IR_CMP && i->op != IR_BRZ && i->op != IR_BRNZ &&
            i->op != IR_CALL && i->op != IR_RET &&
            /* The conversions are calls with their own case, and their
             * operand and result widths differ -- gen_ins64 would read
             * the wrong one. */
            i->op != IR_I2F && i->op != IR_F2I && i->op != IR_F2F) {
            if (i->op == IR_DIV || i->op == IR_MOD) {
                /* Nothing divides 64 by 64 at RV32, so it is a call into
                 * lib/rt/int64.c under libgcc's names. Both operands are
                 * eight bytes, which the psABI puts in a0:a1 and a2:a3
                 * -- odd-first is fine here, there is no even-pair rule
                 * for a fixed argument -- and the result comes back in
                 * a0:a1. */
                if (i->imm_b) {        /* a0:a1 first; a2:a3 are free */
                    args64x2(F, i->a, -1);
                    operand_b64(F, i, RV_A2, RV_A3);
                } else {
                    args64x2(F, i->a, i->b);
                }
                call_helper(F, i->op == IR_DIV
                            ? (i->sign ? "__divdi3" : "__udivdi3")
                            : (i->sign ? "__moddi3" : "__umoddi3"));
                wr64(F, i->dst, RV_A0, RV_A1);
                return;
            }
            if (gen_ins64(F, n))
                return;
            rv_refuse(F, i, "this operation at 64 bits");
        }
        if (wide && (i->op == IR_CMP || i->op == IR_BRZ || i->op == IR_BRNZ))
            i->w = 8;         /* the cases below read `w` to pick the pair */
    }

    /* At RV64 a 32-bit operation must leave a SIGN-EXTENDED result: the
     * ABI's invariant is that a register holds the sign-extension of its
     * 32-bit value, and `lw` maintains it on the way in. The `w`
     * instruction forms maintain it on the way out. and/or/xor need none
     * -- the operation of two sign-extended values already is one --
     * which is why this is a flag and not a second opcode table. */
    wordop = F->xlen == 64 && i->w == 4;

    switch (i->op) {
    case IR_LABEL:
        F->label_off[i->label] = t->len;
        return;
    case IR_JMP:
        /* A jump to the label that follows it is not an instruction, and
         * the IR is full of them: every `if` without an `else` ends in
         * one. */
        if (n + 1 < fn->nins && fn->ins[n + 1].op == IR_LABEL &&
            fn->ins[n + 1].label == i->label)
            return;
        jump_to(F, i->label);
        return;
    case IR_CONST: {
        int d = wreg(F, i->dst, ACC);
        rv_li(t, d, imm_val(F, i), F->xlen);
        wrote(F, i->dst, d);
        return;
    }
    /* Soft float -- see the 64-bit arm. */
    case IR_BITCAST:
    case IR_MOV: {
        int src = rdr(F, i->a, ACC);
        int d = wreg(F, i->dst, ACC);
        if (src != d)
            rv_mv(t, d, src);
        wrote(F, i->dst, d);
        return;
    }

    case IR_ADD: case IR_SUB: case IR_MUL:
    case IR_AND: case IR_OR:  case IR_XOR: {
        /* A switch and not a table indexed by (i->op - IR_ADD): IR_DIV
         * and IR_MOD sit between IR_MUL and IR_AND, so a six-entry table
         * turns `and` into something else and reads past its end for
         * `or` and `xor`. It compiled, ran, and returned v & ~v. */
        int op = i->op == IR_ADD ? RV_ADD
               : i->op == IR_SUB ? RV_SUB
               : i->op == IR_AND ? RV_AND
               : i->op == IR_OR  ? RV_OR
               : i->op == IR_XOR ? RV_XOR
               : -1;                           /* IR_MUL: not an ALU op */
        int logical = i->op == IR_AND || i->op == IR_OR || i->op == IR_XOR;
        int ra_ = rdr(F, i->a, ACC);
        int rd_ = wreg(F, i->dst, ACC);
        if (i->imm_b && i->op != IR_MUL) {
            /* The immediate forms take a SIGNED 12-bit value, and `sub`
             * has none -- a folded subtraction adds the negative. -(-2048)
             * does not fit, which is why the range is checked here rather
             * than assumed from the original constant's. */
            long long v = imm_val(F, i);
            if (i->op == IR_SUB) v = -v;
            if (rv_fits(v, 12)) {
                rv_alu_imm(t, i->op == IR_SUB ? RV_ADD : op, rd_, ra_,
                           (int)v, logical ? 0 : wordop);
                wrote(F, i->dst, rd_);
                return;
            }
        }
        {
            /* The second operand may not land in the destination: a
             * three-operand machine reads both before it writes, but
             * only within ONE instruction, and `rd_` may be the
             * register `rb_` was about to be loaded into. */
            int rb_ = (i->imm_b || !in_reg(F, i->b)) ? TMP : F->loc[i->b];
            if (rb_ == TMP) operand_b(F, i, TMP);
            if (i->op == IR_MUL)
                rv_muldiv(t, RV_MUL, rd_, ra_, rb_, wordop);
            else
                rv_alu(t, op, rd_, ra_, rb_, logical ? 0 : wordop);
        }
        wrote(F, i->dst, rd_);
        return;
    }
    case IR_MULH: {
        /* The high word of a 32 x 32 product (division by a constant) --
         * RV32 only; target_has_mulh keeps it from RV64. */
        int ra_ = rdr(F, i->a, ACC), rb_ = rdr(F, i->b, TMP);
        int d = wreg(F, i->dst, ACC);
        if (F->xlen != 32)
            rv_refuse(F, i, "a 32-bit high multiply at RV64");
        rv_muldiv(t, i->sign ? RV_MULH : RV_MULHU, d, ra_, rb_, 0);
        wrote(F, i->dst, d);
        return;
    }
    case IR_DIV: case IR_MOD: {
        int ra_ = rdr(F, i->a, ACC);
        int rb_ = (i->imm_b || !in_reg(F, i->b)) ? TMP : F->loc[i->b];
        int d;
        if (rb_ == TMP) operand_b(F, i, TMP);
        d = wreg(F, i->dst, ACC);
        rv_muldiv(t, i->op == IR_DIV ? (i->sign ? RV_DIV : RV_DIVU)
                                     : (i->sign ? RV_REM : RV_REMU),
                  d, ra_, rb_, wordop);
        wrote(F, i->dst, d);
        return;
    }
    case IR_SHL: case IR_SHR: {
        int op = i->op == IR_SHL ? RV_SLL : i->sign ? RV_SRA : RV_SRL;
        int bits = wordop ? 32 : F->xlen;
        int ra_ = rdr(F, i->a, ACC);
        int d;
        if (i->imm_b && i->imm >= 0 && i->imm < bits) {
            d = wreg(F, i->dst, ACC);
            rv_shift_imm(t, op, d, ra_, (int)i->imm, wordop, F->xlen);
        } else {
            int rb_ = (i->imm_b || !in_reg(F, i->b)) ? TMP : F->loc[i->b];
            if (rb_ == TMP) operand_b(F, i, TMP);
            d = wreg(F, i->dst, ACC);
            rv_alu(t, op, d, ra_, rb_, wordop);
        }
        wrote(F, i->dst, d);
        return;
    }
    case IR_NEG: {
        int ra_ = rdr(F, i->a, ACC);
        int d = wreg(F, i->dst, ACC);
        rv_alu(t, RV_SUB, d, RV_ZERO, ra_, wordop);
        wrote(F, i->dst, d);
        return;
    }
    case IR_BNOT: {
        int ra_ = rdr(F, i->a, ACC);
        int d = wreg(F, i->dst, ACC);
        rv_alu_imm(t, RV_XOR, d, ra_, -1, 0);
        wrote(F, i->dst, d);
        return;
    }

    case IR_CMP:
        if (i->w == 8 && F->xlen == 32 && rv_wide_imm() && i->imm_b &&
            i->imm == 0 && n + 1 < F->fn->nins && F->usecnt &&
            F->usecnt[i->dst] == 1 &&
            (i->pred == B_EQ || i->pred == B_NE ||
             (i->sign && (i->pred == B_LT || i->pred == B_GE)))) {
            /* A 64-bit value against zero, read only by the branch after
             * it: `== 0` is an or of the halves and a beqz, signed `< 0`
             * the high word's sign and a bltz -- where the compare made
             * its 0 or 1 first and the branch tested that. */
            struct ir_ins *nx = &F->fn->ins[n + 1];
            if ((nx->op == IR_BRZ || nx->op == IR_BRNZ) && nx->a == i->dst) {
                int al, ah, cond, r;
                src64(F, i->a, A_LO, A_HI, &al, &ah);
                if (i->pred == B_EQ || i->pred == B_NE) {
                    rv_alu(t, RV_OR, SCR, al, ah, 0);
                    r = SCR;
                    cond = i->pred == B_EQ ? RV_BEQ : RV_BNE;
                } else {
                    r = ah;
                    cond = i->pred == B_LT ? RV_BLT : RV_BGE;
                }
                if (nx->op == IR_BRZ)
                    cond = invert_branch(cond);
                branch_if(F, cond, r, RV_ZERO, nx->label);
                F->skip_next = 1;
                return;
            }
        }
        if (i->w == 8 && F->xlen == 32) {
            cmp64(F, i, i->pred, i->sign);
            wr(F, i->dst, ACC);
            return;
        }
        {
            /* Fuse with the branch that follows, when nothing else
             * reads the result. RISC-V has no flags: a branch names
             * its two registers and its condition, so the fused form
             * is one instruction where the unfused one is a compare
             * sequence, a store and a test.
             *
             * B_GT and B_LE have no branch of their own -- the ISA
             * provides lt/ge and expects the operands swapped, which
             * is what the mapping below does. */
            struct ir_ins *nx = n + 1 < F->fn->nins ? &F->fn->ins[n + 1]
                                                    : (struct ir_ins *)0;
            if (nx && (nx->op == IR_BRZ || nx->op == IR_BRNZ) &&
                nx->a == i->dst && !(nx->w == 8 && F->xlen == 32) &&
                F->usecnt && F->usecnt[i->dst] == 1) {
                int ra_ = wordop ? rd32(F, i->a, ACC) : rdr(F, i->a, ACC);
                int rb_ = (i->imm_b || !in_reg(F, i->b)) ? TMP : F->loc[i->b];
                int cond, sw = 0;
                /* against zero: x0 is zero, and only a branch against
                 * x0 has a compressed form (c.beqz/c.bnez) */
                if (i->imm_b && imm_val(F, i) == 0)
                    rb_ = RV_ZERO;
                if (rb_ == TMP) operand_b(F, i, TMP);
                if (wordop && !i->imm_b)
                    rb_ = sext32(F, i->b, rb_, TMP);
                switch (i->pred) {
                case B_EQ: cond = RV_BEQ; break;
                case B_NE: cond = RV_BNE; break;
                case B_LT: cond = i->sign ? RV_BLT : RV_BLTU; break;
                case B_GE: cond = i->sign ? RV_BGE : RV_BGEU; break;
                case B_GT: cond = i->sign ? RV_BLT : RV_BLTU; sw = 1; break;
                case B_LE: cond = i->sign ? RV_BGE : RV_BGEU; sw = 1; break;
                default:   cond = -1; break;
                }
                if (cond >= 0) {
                    int x = sw ? rb_ : ra_, y = sw ? ra_ : rb_;
                    if (nx->op == IR_BRZ)
                        cond = invert_branch(cond);
                    branch_if(F, cond, x, y, nx->label);
                    F->skip_next = 1;
                    return;
                }
            }
        }
        {
            /* cmp_to_reg writes its destination before it has finished
             * reading -- `xor d, a, b` then `sltu d, d, 1` -- but only
             * the FIRST instruction reads a and b, so d may safely be
             * either of them. */
            int ra_ = wordop ? rd32(F, i->a, ACC) : rdr(F, i->a, ACC);
            int rb_ = (i->imm_b || !in_reg(F, i->b)) ? TMP : F->loc[i->b];
            int d;
            if (i->imm_b && !getenv("EMBCC_RV_NOCMPIMM")) {
                d = wreg(F, i->dst, ACC);
                if (cmp_imm_to_reg(F, i->pred, i->sign, ra_, imm_val(F, i),
                                   d)) {
                    wrote(F, i->dst, d);
                    return;
                }
            }
            if (rb_ == TMP) operand_b(F, i, TMP);
            if (wordop && !i->imm_b)
                rb_ = sext32(F, i->b, rb_, TMP);
            d = wreg(F, i->dst, ACC);
            cmp_to_reg(F, i->pred, i->sign, ra_, rb_, d);
            wrote(F, i->dst, d);
        }
        return;

    case IR_SELECT: {
        /* dst = a ? b : c. Both arms are already-computed VALUES in
         * slots, so this is two loads and a branch over one of them. */
        /* The condition is tested at ITS width, `size`, which is not the
         * arms' `w`: if-convert records the branch's. A 32-bit one that
         * is zero may have bits above 31 (rd32); at RV32 a 64-bit one is
         * a pair, and zero only if both halves are. */
        int take_c, done, cond;
        if (F->xlen == 32 && i->size == 8) {
            rd64(F, i->a, A_LO, A_HI);
            rv_alu(t, RV_OR, SCR, A_LO, A_HI, 0);
            cond = SCR;
        } else {
            cond = i->size == 4 ? rd32(F, i->a, SCR) : rdr(F, i->a, SCR);
        }
        int d = wreg(F, i->dst, ACC);
        take_c = rv_b_placeholder(t, RV_BEQ, cond, RV_ZERO);
        rd(F, i->b, d);
        done = rv_j_placeholder(t, RV_ZERO);
        rv_patch_b(t, take_c, t->len);
        rd(F, i->c, d);
        rv_patch_j(t, done, t->len);
        wrote(F, i->dst, d);
        return;
    }

    case IR_BRZ: case IR_BRNZ: {
        int r;
        if (i->w == 8 && F->xlen == 32 && rv_wide_imm()) {
            int al, ah;
            src64(F, i->a, A_LO, A_HI, &al, &ah);
            rv_alu(t, RV_OR, SCR, al, ah, 0);
            r = SCR;
        } else if (i->w == 8 && F->xlen == 32) {
            rd64(F, i->a, A_LO, A_HI);
            rv_alu(t, RV_OR, A_LO, A_LO, A_HI, 0);
            r = A_LO;
        } else {
            r = wordop ? rd32(F, i->a, A_LO) : rdr(F, i->a, A_LO);
        }
        branch_if(F, i->op == IR_BRZ ? RV_BEQ : RV_BNE, r, RV_ZERO,
                  i->label);
        return;
    }

    /* A LOCAL may live in a register too, and these two are the only
     * places that name its slot directly -- so they are the two that
     * have to ask. Reading the slot of an allocated local is reading
     * whatever the frame happened to hold: it is what turned a switch
     * returning 100/200/300 into one returning 200 every time. */
    case IR_LDVAR: {
        int d = wreg(F, i->dst, ACC);
        if (in_reg(F, i->a)) {
            if (rv_ldvar_plain(i->size, i->sign, i->w)) {
                if (d != F->loc[i->a]) rv_mv(t, d, F->loc[i->a]);
            } else {
                ext_reg(F, d, F->loc[i->a], i->size, i->sign);
            }
        } else {
            ld_sp(F, d, sslot(F, i->a), i->size, i->sign);
        }
        wrote(F, i->dst, d);
        return;
    }
    case IR_STVAR: {
        int src = rdr(F, i->a, ACC);
        if (in_reg(F, i->dst)) {
            /* A narrowing store SIGN-extends, which rv_ldvar_plain is
             * written to match: it is what makes a signed four-byte
             * read free at RV64, and it is the ABI's own invariant for
             * a 32-bit value in a 64-bit register. Only the low `size`
             * bytes carry the value, so an unsigned read still extends
             * for itself and is right regardless. */
            if (i->size >= F->w) {
                if (F->loc[i->dst] != src) rv_mv(t, F->loc[i->dst], src);
            } else {
                ext_reg(F, F->loc[i->dst], src, i->size, 1);
            }
        } else {
            st_sp(F, src, sslot(F, i->dst), i->size);
        }
        return;
    }
    case IR_LOAD: {                    /* memoff: ra_fold_memoff's, or 0 */
        int addr = rdr(F, i->a, ADDR);
        int d = wreg(F, i->dst, ACC);
        rv_load(t, d, addr, i->memoff, i->size, i->sign, F->xlen);
        wrote(F, i->dst, d);
        return;
    }
    case IR_STORE: {
        int addr = rdr(F, i->a, ADDR);
        int val = rdr(F, i->b, ACC);
        rv_store(t, val, addr, i->memoff, i->size, F->xlen);
        return;
    }
    case IR_EXT: {
        int ra_ = rdr(F, i->a, ACC);
        int d = wreg(F, i->dst, ACC);
        ext_reg(F, d, ra_, i->size, i->sign);
        wrote(F, i->dst, d);
        return;
    }

    case IR_ADDR: {
        int d = wreg(F, i->dst, ACC);
        addr_sp(F, d, sslot(F, i->a));
        wrote(F, i->dst, d);
        return;
    }
    /* A symbol's address takes TWO instructions and two relocations, as
     * on aarch64 and for the same reason: no instruction carries a whole
     * address. `auipc` supplies bits 31:12 of a PC-relative displacement
     * and `addi` a SIGN-EXTENDED low 12, which is why the linker rounds
     * the high half up by 0x800 -- the note on hi20_of() in emit.c.
     *
     * auipc and not lui, at BOTH widths. `lui` sign-extends bit 31, so
     * the absolute pair cannot name an RV64 address between 0x80000000
     * and 0xffffffff7fffffff -- and that is exactly where a firmware
     * image lives. Every global's address came out sign-extended and the
     * first store through one faulted. */
    case IR_STRADDR:
        {
        int d = wreg(F, i->dst, ACC);     /* straight into its home */
        int at = rv_pcrel_pair(t, d);
        note_str(F->st, at, i->label, RK_RISCV_PCREL_HI20);
        note_str(F->st, at + 4, i->label, RK_RISCV_PCREL_LO12_I);
        wrote(F, i->dst, d);
        }
        return;
    case IR_GADDR:
        {
        int d = wreg(F, i->dst, ACC);     /* straight into its home */
        int at = rv_pcrel_pair(t, d);
        note_glob(F->st, at, i->glob, RK_RISCV_PCREL_HI20);
        note_glob(F->st, at + 4, i->glob, RK_RISCV_PCREL_LO12_I);
        wrote(F, i->dst, d);
        }
        return;
    case IR_FADDR:
        {
        int d = wreg(F, i->dst, ACC);     /* straight into its home */
        int at = rv_pcrel_pair(t, d);
        note_fn(F->st, at, i->callee, RK_RISCV_PCREL_HI20);
        note_fn(F->st, at + 4, i->callee, RK_RISCV_PCREL_LO12_I);
        wrote(F, i->dst, d);
        }
        return;

    case IR_MEMCPY: case IR_MEMZERO: {
        int d = rdr(F, i->a, ADDR);
        int s = i->op == IR_MEMCPY ? rdr(F, i->b, TMP) : RV_ZERO;
        copy_block_at(F, i->op == IR_MEMCPY, i->size, F->w, s, 0, d, 0);
        return;
    }

    case IR_CALL:
        gen_call(F, n);
        return;

    case IR_RET:
        if (i->a >= 0) {
            struct rv_flat rfl;
            if (rv_ret_hf(fn->ret_abi.is_struct, fn->ret_abi.is_float,
                          fn->ret_abi.size, fn->ret_abi.ty, &rfl)) {
                /* the hardware-float CC: a float in fa0, a flattened
                 * struct's fields in fa0/fa1 and a0 */
                if (!fn->ret_abi.is_struct) {
                    fload_v(F, i->a, RV_FA0, rfl.size[0] == 8);
                } else {
                    int nf = 0, ng = 0;
                    rd(F, i->a, ADDR);
                    for (int q = 0; q < rfl.n; q++) {
                        if (rfl.fp[q])
                            rv_fload(t, RV_FA0 + nf++, ADDR, (int)rfl.off[q],
                                     rfl.size[q] == 8);
                        else
                            rv_load(t, RV_A0 + ng++, ADDR, (int)rfl.off[q],
                                    rfl.size[q], 1, F->xlen);
                    }
                }
            } else if (fn->ret_abi.is_struct) {
                long size = fn->ret_abi.size;
                if (sret_bytes(F->w, (int)size)) {
                    /* Through the caller's buffer, whose address the
                     * prologue put on the frame because a0 does not
                     * survive the calls in between. */
                    /* In the widest access the type's alignment allows:
                     * both ends are objects of this type, so both are
                     * aligned to it. Byte by byte, lldiv's 16 bytes were
                     * 32 instructions; a packed struct (align 1) still
                     * goes a byte at a time. */
                    int al = fn->ret_abi.align;
                    int step = al >= F->w ? F->w : al >= 4 ? 4
                             : al >= 2 ? 2 : 1;
                    rd(F, i->a, TMP);
                    ld_sp(F, ADDR, F->sret_slot, F->w, 1);
                    copy_block(F, 1, size, step);
                    ld_sp(F, RV_A0, F->sret_slot, F->w, 1);
                } else {
                    /* Small enough for a0:a1, PACKED -- the object's
                     * bytes, not its fields. */
                    rd(F, i->a, ADDR);
                    for (int q = 0; (long)q * F->w < size; q++) {
                        long off = (long)q * F->w;
                        long left = size - off;
                        if (left >= F->w) {
                            rv_load(t, RV_A0 + q, ADDR, (int)off, F->w, 0, F->xlen);
                        } else {
                            rv_mv(t, RV_A0 + q, RV_ZERO);
                            for (long b = off + left - 1; b >= off; b--) {
                                rv_shift_imm(t, RV_SLL, RV_A0 + q, RV_A0 + q,
                                             8, 0, F->xlen);
                                rv_load(t, SCR, ADDR, (int)b, 1, 0, F->xlen);
                                rv_alu(t, RV_OR, RV_A0 + q, RV_A0 + q, SCR, 0);
                            }
                        }
                    }
                }
            } else if (fn_sret_bytes(F->w, fn)) {
                /* RV32's long double, through the caller's buffer: the
                 * prologue kept its address on the frame */
                long s;
                need16(F, i->a);
                s = sslot(F, i->a);
                ld_sp(F, ADDR, F->sret_slot, 4, 1);
                for (int q = 0; q < 16; q += 4) {
                    ld_sp(F, A_LO, s + q, 4, 1);
                    rv_store(t, A_LO, ADDR, q, 4, 32);
                }
                rv_mv(t, RV_A0, ADDR);
            } else if (F->xlen == 32 && F->wide[i->a]) {
                rd64(F, i->a, RV_A0, RV_A1);
            } else if (F->xlen == 64 && fn->ret_abi.size == 16) {
                ld128(F, i->a, RV_A0, RV_A1);
            } else if (F->sx && !fn->ret_abi.is_float &&
                       fn->ret_abi.size == 4) {
                /* the caller is owed the sign extension (rd32) */
                int r = rd32(F, i->a, RV_A0);
                if (r != RV_A0) rv_mv(t, RV_A0, r);
            } else {
                rd(F, i->a, RV_A0);
            }
        }
        /* To the epilogue -- unless it is what comes next: only labels
         * between here and the end of the function emit no code, and a
         * jump to the next instruction is four bytes of nothing. */
        {
            int m = n + 1;
            while (m < fn->nins && fn->ins[m].op == IR_LABEL)
                m++;
            if (m < fn->nins)
                jump_to(F, fn->nlabels);
        }
        return;

    case IR_UD2:
        /* The guaranteed-illegal instruction. A load from address zero is
         * NOT a substitute: on a board with memory or a trap handler
         * there it simply succeeds, and an unreachable path becomes a
         * silent fallthrough -- which is what happened on Cortex-M. */
        rv_unimp(t);
        return;
    case IR_FENCE:
        /* fence rw, rw: memory against memory, both ways -- what a
         * seq_cst fence and the barrier around a seq_cst load or store
         * ask for. It was nothing, on the reasoning of one hart; but the
         * A extension's AMOs are emitted, so harts are expected, and on
         * a second one a store could be seen out of order. */
        rv_fence(t, RV_FENCE_R | RV_FENCE_W, RV_FENCE_R | RV_FENCE_W);
        return;

    case IR_BSWAP: {
        int nbytes = i->size;
        rd(F, i->a, ACC);
        rv_mv(t, TMP, RV_ZERO);
        for (int b = 0; b < nbytes; b++) {
            rv_shift_imm(t, RV_SLL, TMP, TMP, 8, 0, F->xlen);
            rv_alu_imm(t, RV_AND, SCR, ACC, 255, 0);
            rv_alu(t, RV_OR, TMP, TMP, SCR, 0);
            rv_shift_imm(t, RV_SRL, ACC, ACC, 8, 0, F->xlen);
        }
        wr(F, i->dst, TMP);
        return;
    }

    case IR_VA_START:
        /* va_list is a bare POINTER here, as it is on AAPCS32: it points
         * at the first unnamed argument and walks up. The prologue has
         * already spilled a0-a7 immediately below the caller's stack
         * arguments, so one pointer covers both halves. */
        rd(F, i->a, ADDR);
        addr_sp(F, ACC, F->va_first);
        rv_store(t, ACC, ADDR, 0, F->w, F->xlen);
        return;

    case IR_I2F: case IR_F2I: case IR_F2F: {
        /* Conversions are calls, and their OPERAND and RESULT widths
         * differ -- which is why they are not in the wide block above,
         * whose test reads one width for both ends. */
        const char *name;
        int src_w = i->size, dst_w = i->w;
        if (i->op == IR_F2F && src_w == dst_w) {
            rd(F, i->a, ACC);
            wr(F, i->dst, ACC);
            return;
        }
        if (i->op == IR_I2F)
            name = src_w <= 4
                 ? (dst_w == 8 ? (i->sign ? "__floatsidf" : "__floatunsidf")
                               : (i->sign ? "__floatsisf" : "__floatunsisf"))
                 : (dst_w == 8 ? (i->sign ? "__floatdidf" : "__floatundidf")
                               : (i->sign ? "__floatdisf" : "__floatundisf"));
        else if (i->op == IR_F2I)
            name = dst_w <= 4
                 ? (src_w == 8 ? (i->sign ? "__fixdfsi" : "__fixunsdfsi")
                               : (i->sign ? "__fixsfsi" : "__fixunssfsi"))
                 : (src_w == 8 ? (i->sign ? "__fixdfdi" : "__fixunsdfdi")
                               : (i->sign ? "__fixsfdi" : "__fixunssfdi"));
        else
            name = dst_w == 8 ? "__extendsfdf2" : "__truncdfsf2";

        /* "Narrow" means 32 bits. A sixteen-byte source is not in the
         * eight-byte map either, and `(float)(long)x` of an __int128 --
         * the narrowing is no instruction once copies are propagated --
         * converted its low 32 bits, zero-extended: -2 came out 2^32. */
        if (i->op != IR_I2F && rv_hfw(src_w)) {
            fload_v(F, i->a, RV_FA0, src_w == 8);  /* a float argument */
        } else if (i->op == IR_I2F && src_w == 8 && i->a >= 0 &&
                   !F->wide[i->a] && !is16(F, i->a)) {
            /* irgen USED TO convert an `unsigned int` by asking for a
             * SIGNED 64-bit conversion of it, on the grounds that "a
             * 32-bit operation zero-extends its result into the
             * eight-byte slot". That is true of a register write on
             * x86-64 and aarch64 and false here: at RV32 the slot is four
             * bytes and the next four are another temporary, and at RV64 a
             * slot load SIGN-extends. Without the explicit zero extension
             * below, (float)(unsigned)k came back as a constant 4.7e18
             * whatever k was.
             *
             * It no longer does: target_widen_unsigned_fp_cvt() is false
             * here, so an unsigned 32-bit source arrives as size 4 with
             * sign 0 and __floatunsisf is called by name. This path stays
             * because `src_w == 8` with a narrow source vreg is still a
             * representable shape and the zero extension is still right. */
            rd(F, i->a, RV_A0);
            if (F->xlen == 32) rv_mv(t, RV_A1, RV_ZERO);
            else               ext_reg(F, RV_A0, RV_A0, 4, 0);
        } else if (src_w == 8 && F->xlen == 32) {
            rd64(F, i->a, RV_A0, RV_A1);
        } else if (i->op == IR_I2F && src_w == 4) {
            /* __floatsidf's argument is an int: sign-extended (rd32) */
            int r = rd32(F, i->a, RV_A0);
            if (r != RV_A0) rv_mv(t, RV_A0, r);
        } else {
            rd(F, i->a, RV_A0);
        }
        call_helper(F, name);
        if (i->dst >= 0) {
            if (i->op != IR_F2I && rv_hfw(dst_w))
                fdone(F, i->dst, RV_FA0, dst_w == 8);   /* a float result */
            else if (F->xlen == 32 && F->wide[i->dst])
                wr64(F, i->dst, RV_A0, RV_A1);
            else
                wr(F, i->dst, RV_A0);
        }
        return;
    }

    case IR_ASM: {
        /* Extended asm, assembled in irgen (riscv/irgen.c irg_asm_riscv)
         * against the vocabulary in riscv/asm.c. This only places the
         * operands and splices the bytes.
         *
         * To the allocator (ra_target.asm_in_reg) a value live across an
         * asm keeps out of the registers the asm may change, which irgen
         * recorded (ir_asm.clob): its operands', its clobbers', the
         * template's and its scratch, and every caller-saved register if
         * it calls. irgen refuses s0-s11 in a template or clobber list.
         * The operands are values like any other, moved into and out of
         * their registers here, each way as ONE parallel move -- one at a
         * time would overwrite a register a later operand is still to be
         * read from. thumb/codegen.c's IR_ASM is the same. */
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
                internal_error("riscv: %s: an asm's further output is not "
                               "right after the asm", fn->name);
            return;
        }
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
        int used[32] = { 0 };
        /* An address scratch that is no operand's register. t6 is left
         * out because a far slot access borrows it internally. */
        static const int scr_pool[] = {
            RV_T0, RV_T1, RV_T2, RV_T3, RV_T4, RV_T5,
            RV_A0, RV_A1, RV_A2, RV_A3, RV_A4, RV_A5, RV_A6, RV_A7
        };
        int scr = -1;
        for (int k = 0; k < ia->nin; k++) used[ia->in[k].reg] = 1;
        for (int k = 0; k < ia->nout; k++) used[ia->out[k].reg] = 1;
        for (int k = 0; k < nval; k++) used[vreg_[k]] = 1;
        for (int r = 0; r < 32; r++)
            if (used[r] && (r == 8 || r == 9 || (r >= 18 && r <= 27)))
                rv_refuse(F, i, "an asm operand in a callee-saved register");
        if (ia->clob)
            scr = ia->scr;      /* irgen chose it, and the allocator knows */
        else
            for (unsigned k = 0; k < sizeof scr_pool / sizeof scr_pool[0];
                 k++)
                if (!used[scr_pool[k]]) { scr = scr_pool[k]; break; }
        /* The parallel moves' cycle breaker: a backend scratch no operand
         * uses, which is never a value's home either. */
        static const int pm_pool[] = { RV_T4, RV_T2, RV_T1, RV_T0, RV_T5 };
        int pmscr = -1;
        for (unsigned k = 0; k < sizeof pm_pool / sizeof pm_pool[0]; k++)
            if (!used[pm_pool[k]]) { pmscr = pm_pool[k]; break; }
        int naddr = 0;
        for (int k = 0; k < ia->nout; k++)
            naddr += !ia->out[k].val && !ia->out[k].mem;
        if (scr < 0 && naddr > 0)
            rv_refuse(F, i, "an asm with no scratch register left around it");
        for (int k = 0; k < ia->nout; k++)
            if (!ia->out[k].mem && ia->out[k].size > F->w)
                rv_refuse(F, i, "an asm output wider than a register");
        /* In: an input's value, an "m" output's address, and a "+"
         * output's address (its current value is loaded through it
         * below) -- the register-resident ones as one parallel move,
         * then the rest from their slots. */
        {
            int pd[40], ps[40], npm = 0;
            for (int k = 0; k < ia->nin && npm < 40; k++)
                if (in_reg(F, ia->in[k].temp)) {
                    pd[npm] = ia->in[k].reg;
                    ps[npm++] = F->loc[ia->in[k].temp];
                }
            for (int k = 0; k < ia->nout && npm < 40; k++)
                if (!ia->out[k].val &&
                    (ia->out[k].mem || ia->out[k].inout) &&
                    in_reg(F, ia->out[k].temp)) {
                    pd[npm] = ia->out[k].reg;
                    ps[npm++] = F->loc[ia->out[k].temp];
                }
            if (npm) {
                int od[80], os[80];
                int m = ra_parallel_move(pd, ps, npm, pmscr, od, os,
                                         (int)(sizeof od / sizeof od[0]));
                if (m < 0)
                    rv_refuse(F, i, "an asm whose operands cannot be moved "
                                    "into place");
                for (int k = 0; k < m; k++)
                    rv_mv(t, od[k], os[k]);
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
                    rv_load(t, o->reg, o->reg, 0, o->size, 0, F->xlen);
            }
        }
        for (int k = 0; k < ia->codelen; k++)
            code_byte(t, ia->code[k]);
        /* Out, through an address: the address is live across the asm
         * (regalloc.c counts it so), so it is still there. An "m" output
         * was written BY the template through the address its register
         * holds; storing over it would destroy what it wrote. */
        for (int k = 0; k < ia->nout; k++) {
            const struct ir_asm_op *o = &ia->out[k];
            if (o->mem || o->val)
                continue;
            rd(F, o->temp, scr);
            rv_store(t, o->reg, scr, 0, o->size, F->xlen);
        }
        /* Out, as values: each to its home -- those in memory first,
         * while every operand register still holds what the asm left,
         * then the register-resident ones as one parallel move. */
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
                int m = ra_parallel_move(pd, ps, npm, pmscr, od, os,
                                         (int)(sizeof od / sizeof od[0]));
                if (m < 0)
                    rv_refuse(F, i, "an asm whose outputs cannot be moved "
                                    "into place");
                for (int k = 0; k < m; k++)
                    rv_mv(t, od[k], os[k]);
            }
        }
        return;
    }
    /* ---- the A extension ---------------------------------------------
     *
     * Hazard3, the RTOS requirements' fourth target, is RV32IMAC, and a
     * kernel cannot be written without these: a lock is a compare-and-swap.
     *
     * Every one is AQRL -- acquire AND release ordering -- rather than
     * relaxed. A C11 atomic defaults to seq_cst, and an RTOS lock that is
     * merely relaxed is a lock that does not work on a core that reorders.
     * The cost of getting this wrong is invisible on Hazard3, which is
     * in-order, and appears on the first core that is not.
     *
     * Only at the register's own width. The A extension has .w and (at
     * RV64) .d and nothing narrower, so a one- or two-byte atomic is
     * refused rather than turned into a read-modify-write of the word
     * around it -- which is what it would have to be, and which is not
     * atomic with respect to a neighbouring byte. */
    case IR_XCHG: case IR_XADD: case IR_ARMW: {
        int aw = i->size;
        int addr, val, dst;
        if (aw == 1 || aw == 2) {
            /* the word around it: see sub_lane */
            addr = rdr(F, i->a, ADDR);
            val = rdr(F, i->b, TMP);
            sub_lane(t, addr, aw, F->xlen);
            sub_in(t, SUB_V, val);                 /* the operand, in lane */
            int op = i->op == IR_ARMW ? (int)i->imm : 0;
            if (op == '|' || op == '^') {
                /* the other lanes of the operand are 0: unchanged */
                rv_amo(t, op == '|' ? RV_AMOOR : RV_AMOXOR, SUB_OLD, SUB_AL,
                       SUB_V, RV_ORD_AQRL, 0);
            } else if (op == '&') {
                /* ...and for AND, 1 */
                rv_alu_imm(t, RV_XOR, SUB_V2, SUB_MK, -1, 0);
                rv_alu(t, RV_OR, SUB_V, SUB_V, SUB_V2, 0);
                rv_amo(t, RV_AMOAND, SUB_OLD, SUB_AL, SUB_V, RV_ORD_AQRL, 0);
            } else {
                int top = t->len;
                rv_amo(t, RV_LR, SUB_OLD, SUB_AL, RV_ZERO, RV_ORD_AQ, 0);
                if (i->op == IR_XCHG)
                    rv_mv(t, SUB_V2, SUB_V);
                else if (i->op == IR_XADD)
                    rv_alu(t, RV_ADD, SUB_V2, SUB_OLD, SUB_V, 0);
                else {                              /* nand */
                    rv_alu(t, RV_AND, SUB_V2, SUB_OLD, SUB_V, 0);
                    rv_alu_imm(t, RV_XOR, SUB_V2, SUB_V2, -1, 0);
                }
                sub_commit(t, SUB_V2, top);
            }
            sub_out(t, aw, i->sign, F->xlen);
            wrote(F, i->dst, SUB_OLD);
            return;
        }
        if (aw != F->w && !(aw == 4 && F->xlen == 64))
            rv_refuse(F, i, "an atomic wider than a register");
        addr = rdr(F, i->a, ADDR);
        val = rdr(F, i->b, TMP);
        dst = wreg(F, i->dst, ACC);
        /* With the operands in their homes (atomic_in_reg) the result's
         * home may be one of theirs -- an operand that dies here -- and
         * the NAND loop below writes dst before its store-conditional
         * reads addr and val again. So the result is made in ACC then;
         * `wrote` moves it home. */
        if (dst == addr || dst == val)
            dst = ACC;
        if (i->op == IR_XCHG)
            rv_amo(t, RV_AMOSWAP, dst, addr, val, RV_ORD_AQRL, aw == 8);
        else if (i->op == IR_XADD)
            rv_amo(t, RV_AMOADD, dst, addr, val, RV_ORD_AQRL, aw == 8);
        else {
            /* IR_ARMW's operation is a character in `imm`. Three of the four
             * are single instructions; NAND is not -- there is no amonand --
             * so it becomes the load-reserved loop below. */
            enum rv_amo op;
            switch ((int)i->imm) {
            case '&': op = RV_AMOAND; break;
            case '|': op = RV_AMOOR;  break;
            case '^': op = RV_AMOXOR; break;
            default:
                /* nand: dst = *a; *a = ~(dst & b). An lr/sc retry loop,
                 * which is also the shape every CAS below has. */
                {
                    int top = t->len;
                    rv_amo(t, RV_LR, dst, addr, RV_ZERO, RV_ORD_AQ, aw == 8);
                    rv_alu(t, RV_AND, SCR, dst, val, 0);
                    rv_alu_imm(t, RV_XOR, SCR, SCR, -1, 0);    /* xori -1 = ~ */
                    rv_amo(t, RV_SC, SCR2, addr, SCR, RV_ORD_RL, aw == 8);
                    /* sc writes 0 on success; retry while non-zero. */
                    {
                        int br = rv_b_placeholder(t, RV_BNE, SCR2, RV_ZERO);
                        rv_patch_b(t, br, top);
                    }
                }
                wrote(F, i->dst, dst);
                return;
            }
            rv_amo(t, op, dst, addr, val, RV_ORD_AQRL, aw == 8);
        }
        wrote(F, i->dst, dst);
        return;
    }

    case IR_CAS: case IR_CMPXCHG: {
        /* A compare-and-swap is a load-reserved/store-conditional loop: the
         * A extension has no single instruction for it.
         *
         *   retry: lr.w   seen, (addr)
         *          bne    seen, expected, out      -- someone else's value
         *          sc.w   failed, desired, (addr)
         *          bnez   failed, retry            -- the reservation broke
         *   out:
         *
         * IR_CAS yields the value SEEN, whether or not the swap happened --
         * the __sync_val_compare_and_swap shape. IR_CMPXCHG yields a 0/1 and
         * writes the seen value back through the pointer in `b` -- the
         * __atomic_compare_exchange one. The two differ only in what is
         * stored afterwards, so they share the loop. */
        int aw = i->size;
        int addr, exp, des, seen, out_br, top, sc_br;
        if (aw == 1 || aw == 2) {
            /* the word around it (sub_lane): compare this lane only */
            addr = rdr(F, i->a, ADDR);
            sub_lane(t, addr, aw, F->xlen);
            if (i->op == IR_CAS) {
                sub_in(t, SUB_V, rdr(F, i->b, TMP));
            } else {
                int p = rdr(F, i->b, TMP);
                rv_load(t, SUB_V, p, 0, aw, 0, F->xlen);
                sub_in(t, SUB_V, SUB_V);
            }
            sub_in(t, SUB_V2, rdr(F, i->c, SUB_V2));
            top = t->len;
            rv_amo(t, RV_LR, SUB_OLD, SUB_AL, RV_ZERO, RV_ORD_AQ, 0);
            rv_alu(t, RV_AND, FAR, SUB_OLD, SUB_MK, 0);
            out_br = rv_b_placeholder(t, RV_BNE, FAR, SUB_V);
            sub_commit(t, SUB_V2, top);
            rv_patch_b(t, out_br, t->len);
            if (i->op == IR_CAS) {
                sub_out(t, aw, i->sign, F->xlen);
                wr(F, i->dst, SUB_OLD);
            } else {
                /* the bool, from the lanes compared; then *b = seen */
                rv_alu(t, RV_AND, SUB_V2, SUB_OLD, SUB_MK, 0);
                rv_alu(t, RV_XOR, SUB_V2, SUB_V2, SUB_V, 0);
                rv_alu_imm(t, RV_SLTU, SUB_V2, SUB_V2, 1, 0);
                sub_out(t, aw, 0, F->xlen);
                int p = rdr(F, i->b, TMP);
                rv_store(t, SUB_OLD, p, 0, aw, F->xlen);
                wr(F, i->dst, SUB_V2);
            }
            return;
        }
        if (aw != F->w && !(aw == 4 && F->xlen == 64))
            rv_refuse(F, i, "an atomic wider than a register");
        addr = rdr(F, i->a, ADDR);
        if (i->op == IR_CAS) {
            /* compared with what lr.w sign-extended (rd32) */
            exp = aw == 4 ? rd32(F, i->b, TMP) : rdr(F, i->b, TMP);
        } else {
            /* IR_CMPXCHG's expected value is at *b, not in b. */
            int p = rdr(F, i->b, TMP);
            rv_load(t, SCR, p, 0, aw, 1, F->xlen);
            exp = SCR;
        }
        des = rdr(F, i->c, SCR2);
        seen = ACC;
        top = t->len;
        rv_amo(t, RV_LR, seen, addr, RV_ZERO, RV_ORD_AQ, aw == 8);
        out_br = rv_b_placeholder(t, RV_BNE, seen, exp);
        rv_amo(t, RV_SC, FAR, addr, des, RV_ORD_RL, aw == 8);
        sc_br = rv_b_placeholder(t, RV_BNE, FAR, RV_ZERO);
        rv_patch_b(t, sc_br, top);
        rv_patch_b(t, out_br, t->len);
        if (i->op == IR_CAS) {
            wr(F, i->dst, seen);
        } else {
            /* the bool: did the value seen equal the expected one? */
            int p = rdr(F, i->b, TMP);
            rv_store(t, seen, p, 0, aw, F->xlen);   /* *b = what was seen */
            rv_alu(t, RV_XOR, FAR, seen, exp, 0);
            rv_alu_imm(t, RV_SLTU, FAR, FAR, 1, 0);    /* sltiu 1: == 0 -> 1 */
            wr(F, i->dst, FAR);
        }
        return;
    }
    case IR_ALLOCA: {
        /* A variable-length array: sp -= round16(size). The block starts
         * ABOVE the outgoing-argument area, which stays at the bottom
         * of the stack where a callee looks for its arguments -- so the
         * area moves down with sp and the block sits on top of it. The
         * frame itself is addressed from s0 in such a function. */
        int d = wreg(F, i->dst, SCR2);
        rd(F, i->a, SCR);
        rv_alu_imm(t, RV_ADD, SCR, SCR, 15, 0);
        rv_alu_imm(t, RV_AND, SCR, SCR, -16, 0);
        rv_alu(t, RV_SUB, RV_SP, RV_SP, SCR, 0);
        if (rv_fits(F->out_bytes, 12)) {
            rv_alu_imm(t, RV_ADD, d, RV_SP, (int)F->out_bytes, 0);
        } else {
            rv_li(t, d, F->out_bytes, F->xlen);
            rv_alu(t, RV_ADD, d, RV_SP, d, 0);
        }
        wrote(F, i->dst, d);
        return;
    }
    case IR_SPSAVE: {
        int d = wreg(F, i->dst, SCR);
        rv_mv(t, d, RV_SP);
        wrote(F, i->dst, d);
        return;
    }
    case IR_SPRESTORE:
        rv_mv(t, RV_SP, rdr(F, i->a, SCR));
        return;
    case IR_SWITCH: {
        /* A jump table in .text right after its dispatch, of 32-bit
         * offsets from the AUIPC that finds it (so the table's own
         * distance folds into the load's immediate and no addi is
         * needed):
         *     li t2, n ; bgeu rI, t2, default
         *     auipc t1, 0 ; slli t2, rI, 2 ; add t2, t2, t1
         *     lw t2, table - auipc(t2) ; add t2, t2, t1 ; jr t2
         * The six words after the branch are emitted with compression
         * off, so the table sits a known 24 bytes past the auipc (plus
         * alignment). At width 4 on RV64 the index is sign-extended, so
         * a negative one is a huge unsigned and takes the default like
         * any other value outside the range. */
        int n = fn->jt[i->jt].n;
        int ri = wordop ? rd32(F, i->a, ACC) : rdr(F, i->a, ACC);
        rv_li(t, RV_T2, n, F->xlen);
        branch_if(F, RV_BGEU, ri, RV_T2, i->label);
        int on = rv_compress_enabled();
        rv_set_compress(0, F->xlen);
        int at = t->len;
        int tab = (at + 24 + 3) & ~3;
        rv_auipc(t, TMP, 0);
        rv_shift_imm(t, RV_SLL, RV_T2, ri, 2, 0, F->xlen);
        rv_alu(t, RV_ADD, RV_T2, RV_T2, TMP, 0);
        rv_load(t, RV_T2, RV_T2, tab - at, 4, 1, F->xlen);
        rv_alu(t, RV_ADD, RV_T2, RV_T2, TMP, 0);
        rv_jalr(t, RV_ZERO, RV_T2, 0);
        while (t->len < tab)
            code_u16(t, 0x0001);                      /* c.nop, never run */
        rv_set_compress(on, F->xlen);
        if (t->len != tab)
            internal_error("riscv: %s: the jump table is not where its "
                           "auipc says", fn->name);
        for (int k = 0; k < n; k++) {
            want_label(F, t->len, fn->jt[i->jt].labels[k], FX_TAB,
                       0, 0, 0, at);
            code_u32(t, 0);
        }
        return;
    }
    case IR_LABELADDR: {
        if (cg_label_mark(i))       /* static data's marker: no code */
            return;
        /* &&label: `auipc d, 0; addi d, d, 0`, never compressed, patched
         * with the label's distance once it is placed -- no relocation,
         * the label being in this same function. */
        int d = wreg(F, i->dst, ACC);
        want_label(F, rv_pcrel_pair(t, d), i->label, FX_ADDR, 0, 0, 0, 0);
        wrote(F, i->dst, d);
        return;
    }
    case IR_IGOTO:
        rv_jalr(t, RV_ZERO, rdr(F, i->a, ACC), 0);
        return;
    default:
        rv_refuse(F, i, "this operation");
    }
}

/* Copy `size` bytes from [TMP] to [ADDR] (copy) or zero them (!copy), in
 * accesses of `step` bytes and a byte tail. Straight-line while every
 * offset fits a load's or store's 12-bit immediate; past that, a loop
 * that walks both pointers with the end in SCR2 -- `long long a[300] =
 * {0}` and a 2403-byte struct returned by value were internal errors at
 * every -O level. TMP and ADDR are scratch and may be moved. */
static void copy_block(struct rv_fn *F, int copy, long size, int step)
{
    struct code *t = F->t;
    long k;
    if (size <= 2040) {
        for (k = 0; k + step <= size; k += step) {
            if (copy) rv_load(t, SCR, TMP, (int)k, step, 0, F->xlen);
            rv_store(t, copy ? SCR : RV_ZERO, ADDR, (int)k, step, F->xlen);
        }
        for (; k < size; k++) {
            if (copy) rv_load(t, SCR, TMP, (int)k, 1, 0, F->xlen);
            rv_store(t, copy ? SCR : RV_ZERO, ADDR, (int)k, 1, F->xlen);
        }
        return;
    }
    long body = size / step * step;
    rv_li(t, SCR2, body, F->xlen);
    rv_alu(t, RV_ADD, SCR2, SCR2, ADDR, 0);
    int top = t->len;
    if (copy) {
        rv_load(t, SCR, TMP, 0, step, 0, F->xlen);
        rv_store(t, SCR, ADDR, 0, step, F->xlen);
        rv_alu_imm(t, RV_ADD, TMP, TMP, step, 0);
    } else {
        rv_store(t, RV_ZERO, ADDR, 0, step, F->xlen);
    }
    rv_alu_imm(t, RV_ADD, ADDR, ADDR, step, 0);
    rv_patch_b(t, rv_b_placeholder(t, RV_BNE, ADDR, SCR2), top);
    for (k = 0; k < size - body; k++) {
        if (copy) rv_load(t, SCR, TMP, (int)k, 1, 0, F->xlen);
        rv_store(t, copy ? SCR : RV_ZERO, ADDR, (int)k, 1, F->xlen);
    }
}

/* copy_block from [sreg + soff] to [dreg + doff] with those registers as
 * the bases, when every offset fits a load's or store's immediate: a
 * struct or long double whose address is already in a register, or whose
 * home is a frame slot, was first moved into TMP and ADDR -- a by-value
 * parameter's copy into its slot was `mv t1, a1; addi t2, sp, 48` before
 * its first word. Otherwise the addresses go to TMP and ADDR, and
 * copy_block does the rest. sreg and dreg are left as they were. */
static void copy_block_at(struct rv_fn *F, int copy, long size, int step,
                          int sreg, long soff, int dreg, long doff)
{
    struct code *t = F->t;
    if (size <= 2040 && rv_fits(doff, 12) && rv_fits(doff + size, 12) &&
        (!copy || (rv_fits(soff, 12) && rv_fits(soff + size, 12)))) {
        /* the data register: SCR, unless a base is (param_reg's is) */
        int dr = sreg == SCR || dreg == SCR ? SCR2 : SCR;
        long k;
        for (k = 0; k + step <= size; k += step) {
            if (copy) rv_load(t, dr, sreg, (int)(soff + k), step, 0, F->xlen);
            rv_store(t, copy ? dr : RV_ZERO, dreg, (int)(doff + k), step,
                     F->xlen);
        }
        for (; k < size; k++) {
            if (copy) rv_load(t, dr, sreg, (int)(soff + k), 1, 0, F->xlen);
            rv_store(t, copy ? dr : RV_ZERO, dreg, (int)(doff + k), 1,
                     F->xlen);
        }
        return;
    }
    if (copy) {
        if (rv_fits(soff, 12)) {
            rv_alu_imm(t, RV_ADD, TMP, sreg, (int)soff, 0);
        } else {
            rv_li(t, TMP, soff, F->xlen);
            rv_alu(t, RV_ADD, TMP, sreg, TMP, 0);
        }
    }
    if (rv_fits(doff, 12)) {
        rv_alu_imm(t, RV_ADD, ADDR, dreg, (int)doff, 0);
    } else {
        rv_li(t, ADDR, doff, F->xlen);
        rv_alu(t, RV_ADD, ADDR, dreg, ADDR, 0);
    }
    copy_block(F, copy, size, step);
}

/* A parameter's qth incoming word, in a register ready to store.
 *
 * Normally that is the argument register itself. In a VARIADIC function
 * the prologue has already spilled all eight, and the named parameters
 * are read back OUT of the spill rather than out of the registers -- not
 * because the registers are wrong at that point, but because it keeps one
 * rule: a0-a7 are written to the save area once, and everything
 * afterwards addresses memory. */
static int param_reg(struct rv_fn *F, const struct argplace *pl, int q)
{
    if (!F->fn->is_varargs)
        return argreg(pl->reg + q);
    ld_sp(F, SCR, F->va_regsave + (long)(pl->reg + q) * F->w, F->w, 1);
    return SCR;
}

/* ---- register pairs at RV32 ----------------------------------------------

 * A 64-bit value -- a double on this soft-float target, or a long long --
 * needs two registers, and the shared allocator hands out one. So it is
 * run twice, as the AVR backend does: first for the 64-bit values alone
 * over a pool of PAIRS (RV_PAIRS, s2:s3 up to s10:s11, each named by its
 * low register), then for everything else over the ordinary pool less
 * every register a pair took (g_rv_taken). Withholding a pair for the
 * whole function is coarser than one graph with both classes in it, and
 * sound without teaching the colourer about overlapping registers.
 *
 * Every double used to live in a stack slot: an operation was four loads,
 * the call and two stores, and libc's fdlibm ran at 2.3x clang's size.
 *
 * rd64/wr64 are the pair-aware accessors, and every 64-bit lowering goes
 * through them. The few that address a slot directly are kept out of
 * this pass: a call argument split between a register and the stack, and
 * a local read or written narrower than itself. sslot() refuses anything
 * this misses. */
static const struct ra_target RV_PAIR_RA = {
    rv_pair_pool_for, rv_callee_saved, rv_ldvar_plain,
    1, 1, 1,
    rv_op_calls_helper,
    0,
    rv_pair_hints,
    NULL, NULL,
    1,
    NULL, NULL,
    1,
    0,
    0
};

static void rv_pair_hints(const struct ir_func *fn, int *hint)
{
    int wb = 4;
    struct rv_walk wk;
    struct argplace pl;
    walk_init(&wk, fn_sret_bytes(wb, fn) != 0);
    for (int p = 0; fn->src && p < fn->nparams && p < fn->nvregs; p++) {
        const struct ir_arg *a = &fn->param_abi[p];
        place_one(wb, &wk, a, 0, &pl);
        if (a->size == 8 && pl.nreg == 2 && !a->is_struct)
            hint[p] = argreg(pl.reg);
    }
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        if (i->op == IR_RET && i->a >= 0 && i->a < fn->nvregs &&
            !fn->ret_abi.is_struct && fn->ret_abi.size == 8)
            hint[i->a] = RV_A0;
        if (i->op != IR_CALL && rv_op_calls_helper(i) && i->w == 8) {
            if (i->a >= 0 && i->a < fn->nvregs && hint[i->a] < 0)
                hint[i->a] = RV_A0;
            if (!i->imm_b && i->b >= 0 && i->b < fn->nvregs &&
                hint[i->b] < 0)
                hint[i->b] = RV_A0 + 2;
            if (i->dst >= 0 && i->dst < fn->nvregs && hint[i->dst] < 0)
                hint[i->dst] = RV_A0;
        }
        if (i->op != IR_CALL)
            continue;
        if (!i->retsize && i->dst >= 0 && i->dst < fn->nvregs && i->w == 8)
            hint[i->dst] = RV_A0;
        walk_init(&wk, call_sret_bytes(wb, i) != 0);
        for (int k = 0; k < i->nargs; k++) {
            const struct ir_arg *a = &i->argv[k];
            place_one(wb, &wk, a, i->call_varargs && k >= i->call_nfixed,
                      &pl);
            if (a->size == 8 && pl.nreg == 2 && !a->is_struct &&
                a->vreg >= 0 && a->vreg < fn->nvregs)
                hint[a->vreg] = argreg(pl.reg);
        }
    }
}


/* The pair pass's registers, each over its value's live range only, for
 * the integer pass that follows (ra_reserve). */
static struct ra_range *g_rv_res;
static int g_rv_nres, g_rv_capres;
static void g_rv_reserve_pairs(struct ir_func *fn, const int *loc)
{
    int nv = fn->nvregs;
    int *first = xmalloc((size_t)(nv ? nv : 1) * sizeof *first);
    int *last = xmalloc((size_t)(nv ? nv : 1) * sizeof *last);
    ra_live_ranges(fn, first, last);
    g_rv_nres = 0;
    for (int v = 0; v < nv; v++) {
        if (loc[v] < 0 || first[v] < 0) continue;
        if (g_rv_nres + 2 > g_rv_capres) {
            g_rv_capres = g_rv_capres ? g_rv_capres * 2 : 16;
            g_rv_res = xrealloc(g_rv_res, (size_t)g_rv_capres * sizeof *g_rv_res);
        }
        for (int h = 0; h < 2; h++) {
            g_rv_res[g_rv_nres].reg = loc[v] + h;
            g_rv_res[g_rv_nres].first = first[v];
            g_rv_res[g_rv_nres].last = last[v];
            g_rv_res[g_rv_nres].born = 0;
            g_rv_nres++;
        }
    }
    ra_reserve(g_rv_res, g_rv_nres);
    free(first); free(last);
}
static int *rv_pair_alloc(struct ir_func *fn, struct rv_fn *F,
                          const char *pin)
{
    int nv = fn->nvregs, wb = 4, any = 0;
    char *x = xcalloc((size_t)(nv ? nv : 1), 1);
    int used[RA_MAXPOOL], nused = 0;
    int *loc;

    for (int v = 0; v < nv; v++) {
        x[v] = !F->wide[v] || (pin && pin[v]);
        any |= !x[v];
    }
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        if ((i->op == IR_LDVAR && i->a >= 0 && i->a < nv &&
             F->wide[i->a] && i->w != 8) ||
            (i->op == IR_STVAR && i->dst >= 0 && i->dst < nv &&
             F->wide[i->dst] && i->w != 8)) {
            x[i->op == IR_LDVAR ? i->a : i->dst] = 1;
        }
        if (i->op == IR_CALL) {
            struct rv_walk wk;
            struct argplace pl;
            walk_init(&wk, call_sret_bytes(wb, i) != 0);
            for (int k = 0; k < i->nargs; k++) {
                const struct ir_arg *a = &i->argv[k];
                place_one(wb, &wk, a, i->call_varargs && k >= i->call_nfixed,
                          &pl);
                if (a->size > wb && pl.nreg != 2 && a->vreg >= 0 &&
                    a->vreg < nv)
                    x[a->vreg] = 1;
            }
        }
    }
    F->npair = 0;
    if (!any) {
        free(x);
        return NULL;
    }
    loc = ra_allocate(fn, &RV_PAIR_RA, NULL, x, used, &nused);
    free(x);
    /* Each pair's registers are the ordinary pass's to use outside the
     * pair's live range: reserved by range (ra_reserve), not withheld
     * from the whole function. `used` lists only the callee-saved ones,
     * for the prologue to save. */
    g_rv_reserve_pairs(fn, loc);
    for (int k = 0; k < nused && k < RV_NPAIRS; k++)
        F->pair_used[F->npair++] = used[k];
    return loc;
}

/* ---- one function --------------------------------------------------------- */

/* The callee-saved renaming (defined with gen_func_best), and whether this
 * attempt at a function makes it. */
static void rv_lowregs(const struct ir_func *fn, int *loc, unsigned long fixed);

/* ---- the FP class's members -----------------------------------------------
 *
 * Which vregs are worth an f register, and how wide each is: the operands
 * and results of what rv_fp_hw() runs, floats and doubles passed to and
 * returned from calls and returned by this function, and its
 * floating-point locals -- but not a local any instruction reads or
 * writes narrower than itself (its bytes are taken a word at a time), nor
 * one under -g or -O0, which keep every source variable in its slot. None
 * of it is needed for correctness: a value outside the class is reached
 * through rd/wr and fsrc/fdone wherever it lives. A vreg asked for at two
 * widths is left out. */
/* Which vregs an INTEGER operation reads -- not an FP one, nor a copy, a
 * load's or store's value, a select's arm, a return or a float argument,
 * which take a value wherever it lives. */
struct rv_cnt { int *m; int nv; };

static void rv_int_use_cb(int v, void *ctx)
{
    struct rv_cnt *c = ctx;
    if (v >= 0 && v < c->nv)
        c->m[v]++;
}

/* ...counted, per vreg */
static int *rv_int_uses(const struct ir_func *fn)
{
    struct rv_cnt c;
    c.nv = fn->nvregs;
    c.m = xcalloc((size_t)(c.nv ? c.nv : 1), sizeof *c.m);
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        if (rv_fp_hw(i))
            continue;
        switch (i->op) {
        case IR_MOV: case IR_BITCAST: case IR_RET: case IR_LDVAR:
        case IR_STVAR:
            continue;
        case IR_LOAD: case IR_STORE: case IR_SELECT:
            rv_int_use_cb(i->a, &c);
            continue;
        case IR_CALL:
            if (i->indirect)
                rv_int_use_cb(i->a, &c);
            for (int k = 0; k < i->nargs; k++)
                if (i->argv[k].is_struct || !i->argv[k].is_float)
                    rv_int_use_cb(i->argv[k].vreg, &c);
            continue;
        default:
            ra_each_use(i, rv_int_use_cb, &c);
        }
    }
    return c.m;
}

static char *rv_float_map(const struct rv_fn *F, char **fw_out, int debug)
{
    const struct ir_func *fn = F->fn;
    int nv = fn->nvregs, any = 0;
    char *m, *fw;
    *fw_out = NULL;
    if (!target_riscv_flen() || nv <= 0)
        return NULL;
    m = xcalloc((size_t)nv, 1);
    fw = xcalloc((size_t)nv, 1);
#define FMARK(v, w) do { int v_ = (v), w_ = (w); \
        if (v_ >= 0 && v_ < nv && rv_fp_width_hw(w_) && !is16(F, v_)) { \
            if (fw[v_] && fw[v_] != w_) m[v_] = 2; \
            else if (m[v_] != 2) { m[v_] = 1; fw[v_] = (char)w_; } \
        } } while (0)
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        if (rv_fp_hw(i)) {
            switch (i->op) {
            case IR_I2F: FMARK(i->dst, i->w); break;
            case IR_F2I: FMARK(i->a, i->size); break;
            case IR_F2F: FMARK(i->dst, i->w); FMARK(i->a, i->size); break;
            case IR_CMP:
                FMARK(i->a, i->w);
                if (!i->imm_b) FMARK(i->b, i->w);
                break;
            default:
                FMARK(i->dst, i->w); FMARK(i->a, i->w);
                if (!i->imm_b && i->op != IR_NEG && i->op != IR_SQRT)
                    FMARK(i->b, i->w);
                break;
            }
        } else if (i->op == IR_CALL) {
            if (i->flt && !i->retsize)
                FMARK(i->dst, i->w);
            for (int k = 0; k < i->nargs; k++)
                if (!i->argv[k].is_struct && i->argv[k].is_float)
                    FMARK(i->argv[k].vreg, i->argv[k].size);
        } else if (i->op == IR_RET && i->a >= 0 && fn->ret_abi.is_float &&
                   !fn->ret_abi.is_struct) {
            FMARK(i->a, fn->ret_abi.size);
        }
    }
#undef FMARK
    for (int v = 0; v < fn->nvars && v < nv; v++)
        if (debug || !fn->locals[v].is_scalar_float ||
            fn->locals[v].size != fw[v])
            m[v] = 0;
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        int v = i->op == IR_LDVAR ? i->a : i->op == IR_STVAR ? i->dst : -1;
        if (v >= 0 && v < nv && m[v] && i->size != fw[v])
            m[v] = 0;
    }
    /* A constant integer code reads too -- the optimizer gives 0 and 0.0f
     * one vreg -- keeps its x register: in an f register every integer
     * use would cross with an fmv, and the float uses cross the other
     * way only where it is used as a float. */
    /* And by cost, the way aarch64 places a value both kinds of
     * operation touch (cg_float_vregs_by_cost): in an f register each
     * integer read crosses with an fmv, in an x register each
     * floating-point read and write does. fdlibm's doubles are read word
     * by word (GET_HIGH_WORD) as often as they are computed with. */
    {
        int *iu = rv_int_uses(fn);
        int *fu = xcalloc((size_t)nv, sizeof *fu);
#define FUSE(v) do { int v_ = (v); if (v_ >= 0 && v_ < nv) fu[v_]++; } while (0)
        for (int n = 0; n < fn->nins; n++) {
            const struct ir_ins *i = &fn->ins[n];
            if (rv_fp_hw(i)) {
                if (i->op != IR_I2F) FUSE(i->a);
                if (!i->imm_b && i->op != IR_I2F && i->op != IR_F2I &&
                    i->op != IR_F2F && i->op != IR_NEG && i->op != IR_SQRT)
                    FUSE(i->b);
                if (i->op != IR_CMP && i->op != IR_F2I) FUSE(i->dst);
            } else if (i->op == IR_CALL) {
                if (i->flt && !i->retsize) FUSE(i->dst);
                for (int k = 0; k < i->nargs; k++)
                    if (!i->argv[k].is_struct && i->argv[k].is_float)
                        FUSE(i->argv[k].vreg);
            } else if (i->op == IR_RET && fn->ret_abi.is_float) {
                FUSE(i->a);
            }
        }
#undef FUSE
        for (int p = 0; p < fn->nparams && p < nv; p++)
            if (fn->param_abi[p].is_float && !fn->param_abi[p].is_struct)
                fu[p]++;                    /* arriving in fa0-fa7 */
        for (int n = 0; n < fn->nins; n++) {
            const struct ir_ins *i = &fn->ins[n];
            if (i->op == IR_CONST && i->dst >= 0 && i->dst < nv &&
                iu[i->dst])
                m[i->dst] = 0;
        }
        for (int v = 0; v < nv; v++)
            if (m[v] && iu[v] > fu[v])
                m[v] = 0;
        free(fu);
        free(iu);
    }
    for (int v = 0; v < nv; v++) {
        if (m[v] != 1)
            m[v] = 0;
        /* at RV32 a float is never a pair, nor a double anything else */
        if (m[v] && F->xlen == 32 && F->wide[v] != (fw[v] == 8))
            m[v] = 0;
        if (!m[v])
            fw[v] = 0;
        any |= m[v];
    }
    if (!any) {
        free(m);
        free(fw);
        return NULL;
    }
    *fw_out = fw;
    return m;
}

/* RV64: the vregs fdone may write a float's four bytes of into a slot --
 * so rd reads them back with lw (slot_bytes). A slot that is not here
 * gets the eight-byte, sign-extended form instead. */
static char *rv_f4_map(const struct rv_fn *F)
{
    const struct ir_func *fn = F->fn;
    int nv = fn->nvregs;
    char *m;
    if (F->xlen != 64 || !target_riscv_flen() || nv <= 0)
        return NULL;
    m = xcalloc((size_t)nv, 1);
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        int d = i->dst;
        if (d < 0 || d >= nv)
            continue;
        if ((rv_fp_hw(i) && i->w == 4 && i->op != IR_CMP && i->op != IR_F2I) ||
            (i->op == IR_CALL && i->flt && !i->retsize && i->w == 4))
            m[d] = 1;
    }
    for (int v = 0; v < fn->nvars && v < nv; v++)
        if (fn->locals[v].is_scalar_float && fn->locals[v].size == 4)
            m[v] = 1;
    return m;
}

/* RV32 with D: does anything here put a double in an f register? Then
 * the frame has F->fx, the eight bytes it crosses to an x pair through. */
static int rv_needs_fx(const struct ir_func *fn)
{
    if (target_xlen() != 32 || target_riscv_flen() != 64)
        return 0;
    if ((fn->ret_abi.is_float && fn->ret_abi.size == 8) ||
        fn->ret_abi.is_struct)
        return 1;
    for (int p = 0; p < fn->nparams; p++)
        if (fn->param_abi[p].is_struct ||
            (fn->param_abi[p].is_float && fn->param_abi[p].size == 8))
            return 1;
    for (int v = 0; v < fn->nvars; v++)
        if (fn->locals[v].is_scalar_float && fn->locals[v].size == 8)
            return 1;
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        /* a conversion's double may be a helper's, in fa0 */
        if ((i->flt && i->w == 8) ||
            ((i->op == IR_I2F || i->op == IR_F2I || i->op == IR_F2F) &&
             (i->w == 8 || i->size == 8)))
            return 1;
        if (i->op == IR_CALL)
            for (int k = 0; k < i->nargs; k++)
                if (i->argv[k].is_struct ||
                    (i->argv[k].is_float && i->argv[k].size == 8))
                    return 1;
    }
    return 0;
}

/* Where the hardware-float convention would put each FP value: a
 * parameter in the fa register it arrives in, a call's float arguments in
 * theirs, a float result -- of this function, of a call, of a runtime
 * helper -- in fa0, and a helper's float operand there too. Only
 * preferences, as rv_abi_hints' are for the x registers: the parallel
 * moves at the prologue and each call are what is correct. */
static void rv_fp_hints(const struct ir_func *fn, int *hint)
{
    int wb = target_ptr_size();
    struct rv_walk wk;
    struct argplace pl;
    struct rv_flat fl;
    if (!target_riscv_abi_flen())
        return;
    walk_init(&wk, fn_sret_bytes(wb, fn) != 0);
    for (int p = 0; fn->src && p < fn->nparams && p < fn->nvregs; p++) {
        const struct ir_arg *a = &fn->param_abi[p];
        place_one(wb, &wk, a, 0, &pl);
        if (pl.hf && !a->is_struct)
            hint[p] = RV_FA0 + pl.fld[0].reg;
    }
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        if (i->op == IR_RET && i->a >= 0 && i->a < fn->nvregs &&
            rv_ret_hf(fn->ret_abi.is_struct, fn->ret_abi.is_float,
                      fn->ret_abi.size, fn->ret_abi.ty, &fl) &&
            !fn->ret_abi.is_struct && hint[i->a] < 0)
            hint[i->a] = RV_FA0;
        if ((i->op == IR_I2F || i->op == IR_F2I || i->op == IR_F2F) &&
            !rv_fp_hw(i)) {
            if (i->op != IR_I2F && rv_hfw(i->size) && i->a >= 0 &&
                i->a < fn->nvregs && hint[i->a] < 0)
                hint[i->a] = RV_FA0;
            if (i->op != IR_F2I && rv_hfw(i->w) && i->dst >= 0 &&
                i->dst < fn->nvregs && hint[i->dst] < 0)
                hint[i->dst] = RV_FA0;
        }
        if (i->op != IR_CALL)
            continue;
        if (!i->retsize && i->dst >= 0 && i->dst < fn->nvregs &&
            rv_ret_hf(0, i->flt, i->ret_tybytes, NULL, &fl))
            hint[i->dst] = RV_FA0;
        walk_init(&wk, call_sret_bytes(wb, i) != 0);
        for (int k = 0; k < i->nargs; k++) {
            const struct ir_arg *a = &i->argv[k];
            place_one(wb, &wk, a, i->call_varargs && k >= i->call_nfixed,
                      &pl);
            if (pl.hf && !a->is_struct && a->vreg >= 0 &&
                a->vreg < fn->nvregs)
                hint[a->vreg] = RV_FA0 + pl.fld[0].reg;
        }
    }
}

/* The FP pass's view: RISCV_RA's, with the f-register hints. */
static const struct ra_target RISCV_FRA = {
    rv_pool_for, rv_callee_saved, rv_ldvar_plain,
    1, 1, 1,
    rv_op_calls_helper,
    0,
    rv_fp_hints,
    rv_fp_pool_for, rv_fp_callee_saved,
    1,
    NULL, NULL,
    1,
    0,
    1
};
static int g_rv_lowregs = 1;

static void gen_func(struct ir_func *fn, struct code *t, struct rv_sites *st,
                     int xlen, int want_debug)
{
    struct func *f = fn->src;
    struct rv_fn F;
    int i;

    /* Zeroed first: usecnt and skip_next are only set when the
     * allocator runs, and reading them uninitialised on the other
     * paths is a segfault at -O0 -- which is exactly what the ARMv7-M
     * version of this change did before the memset went in. */
    memset(&F, 0, sizeof F);
    F.fn = fn; F.t = t; F.st = st;
    F.want_debug = want_debug;
    F.xlen = xlen; F.w = xlen / 8;
    F.fix = NULL; F.nfix = F.capfix = 0;
    F.relax = NULL; F.nrelax = 0;
    F.wide = wide_map(fn);
    F.w16 = rv_w16_map(fn);
    if (xlen == 32) {
        F.nshr = ra_narrow_hishift(fn);
        for (int v = 0; v < fn->nvregs; v++)
            if (F.nshr[v]) F.wide[v] = 0;
    }
    F.loc = NULL; F.nsave = 0;
    F.floc = NULL; F.fw = NULL; F.nfsave = 0;
    F.f4 = rv_f4_map(&F);
    F.fx = -1;
    F.fb = RV_SP;
    if (g_rv_regalloc) {
        /* `wide` means two different things and they must not be
         * confused, which they were:
         *
         *   to this FILE it means "needs a register PAIR", which at
         *   RV32 is every eight-byte value and at RV64 is nothing;
         *
         *   to ra_allocate it means "too large for a register, never
         *   eligible".
         *
         * The map is built at both widths because the conversions need
         * to tell a genuinely 64-bit source from a widened 32-bit one.
         * Handing that same map to the allocator at RV64 marked every
         * pointer and every `long` ineligible, and the backend emitted
         * 294 memory operations where RV32 emitted 41 -- the whole
         * reason RV64 stayed at 3.5x clang while RV32 reached 1.7x. An
         * eight-byte value fits an eight-byte register: at RV64 only a
         * sixteen-byte one is ineligible on width grounds (F.w16).
         *
         * fltmap is NULL, not cg_float_vregs: this target has no
         * floating-point register class, so a float lives in an
         * ordinary integer register and must stay ELIGIBLE for the
         * integer pool. Passing the map would exclude every float from
         * both classes and leave it with nowhere to live. */
        /* Under -g a source variable stays in its frame slot, so the
         * DW_AT_location naming that slot is true (see regalloc.h). */
        char *pin = want_debug || g_rv_o0 ? ra_debug_pin_vars(fn)
                                          : (char *)0;
        /* With an FPU, the FP class's members are kept from both integer
         * passes (the pairs and the single registers) -- one value, one
         * home -- and handed to the FP pass after them. */
        char *fwm = NULL;
        char *flt = rv_float_map(&F, &fwm, want_debug || g_rv_o0);
        char *excl = pin;
        if (flt) {
            excl = xcalloc((size_t)fn->nvregs, 1);
            for (int v = 0; v < fn->nvregs; v++)
                excl[v] = (char)(flt[v] || (pin && pin[v]));
        }
        int *pair = xlen == 32 && g_rv_pairs ? rv_pair_alloc(fn, &F, excl)
                                             : NULL;
        {
            /* At RV32 the eight-byte map (pairs, rv_pair_alloc) and the
             * sixteen-byte one are both "not for a single register". */
            char *ineligible = F.w16;
            if (xlen == 32) {
                ineligible = F.wide;
                if (F.w16) {
                    ineligible = xmalloc((size_t)(fn->nvregs ? fn->nvregs : 1));
                    for (int v = 0; v < fn->nvregs; v++)
                        ineligible[v] = F.wide[v] | F.w16[v];
                }
            }
            F.loc = ra_allocate(fn, &RISCV_RA, ineligible, excl,
                                F.used_callee, &F.nsave);
            if (ineligible != F.wide && ineligible != F.w16)
                free(ineligible);
        }
        g_rv_taken = 0;
        unsigned long pair_regs = 0;
        if (pair) {
            for (int v = 0; v < fn->nvregs; v++)
                if (pair[v] >= 0) {
                    F.loc[v] = pair[v];
                    pair_regs |= 3UL << pair[v];
                }
            for (int k = 0; k < F.npair; k++) {
                F.used_callee[F.nsave++] = F.pair_used[k];
                F.used_callee[F.nsave++] = F.pair_used[k] + 1;
            }
            free(pair);
        }
        if (g_rv_lowregs && F.loc && rv_compress_enabled())
            rv_lowregs(fn, F.loc, pair_regs);
        if (flt) {
            F.floc = ra_allocate_fp(fn, &RISCV_FRA, F.w16, flt, F.fused,
                                    &F.nfsave);
            F.fw = fwm;
            fwm = NULL;
            free(excl);
            free(flt);
        }
        free(fwm);
        free(pin);
        /* Read counts for comparison/branch fusion, with the allocator
         * on: without it every value goes through a slot and the
         * branch reads the slot, so "the only reader" would not hold. */
        if (fn->nvregs) {
            F.usecnt = xmalloc((size_t)fn->nvregs * sizeof *F.usecnt);
            ra_count_vreg_uses(fn, F.usecnt);
        }
        /* A BISECTION HANDLE. EMBCC_RV_RA_MAX=N leaves only the first N
         * vregs in registers and sends the rest back to memory, which
         * is always a correct thing to do -- so a miscompile that
         * survives at N and vanishes at N-1 names the value whose
         * allocation is wrong.
         *
         * It is here rather than in a scratch patch because finding the
         * one bad value in a function with two hundred of them is the
         * recurring cost of this work, and the alternative is
         * re-deriving the trick each time. */
        {
            const char *lim = getenv("EMBCC_RV_RA_MAX");
            if (lim) {
                int n = atoi(lim);
                for (int v = n; v < fn->nvregs; v++) {
                    F.loc[v] = -1;
                    if (F.floc)
                        F.floc[v] = -1;
                }
            }
        }
    }
    /* A variable-length array moves sp at run time, so the frame is
     * addressed from s0 instead, which the prologue sets once the frame
     * is in place. s0 is callee-saved and, in such a function, out of the
     * allocator's pool (rv_pool_for), so it only has to be saved like any
     * other callee-saved register. */
    if (fn->has_alloca)
        F.used_callee[F.nsave++] = RV_FP;
    /* A leaf: no call in the IR and none the lowering makes -- the same
     * rv_op_calls_helper the allocator trusts for which values survive a
     * call, so the two cannot disagree. Inline asm might call anything,
     * so it keeps ra saved. */
    /* A tail call leaves ra alone -- it is the caller's, and the callee
     * returns with it -- so it does not make this function a non-leaf. */
    F.tail = NULL;
    /* An interrupt handler returns with mret or sret, so it makes no
     * tail call: the callee would return with ret. */
    F.isr = ISR_KIND(f->is_isr);
    if (g_rv_regalloc && !want_debug && !g_rv_o0 && !F.isr)
        for (i = 0; i < fn->nins; i++)
            if (rv_tail_ok(&F, i)) {
                if (!F.tail)
                    F.tail = xcalloc((size_t)fn->nins, 1);
                F.tail[i] = 1;
            }
    F.sx = xlen == 64 ? sext_map(&F) : NULL;
    F.leaf = 1;
    for (i = 0; i < fn->nins; i++)
        if ((fn->ins[i].op == IR_CALL && !(F.tail && F.tail[i])) ||
            /* an asm writes ra when its template calls or names it,
             * which irgen recorded; one whose clobbers are unknown might */
            (fn->ins[i].op == IR_ASM && fn->ins[i].asm_ir &&
             !fn->ins[i].asm_ir->cont &&
             (!fn->ins[i].asm_ir->clob ||
              (fn->ins[i].asm_ir->clob >> 1 & 1))) ||
            rv_op_calls_helper(&fn->ins[i]))
            F.leaf = 0;
    /* The first attempt leaves fx out where it might not be needed
     * (F.fx_lazy); one that needs it after all is emitted again. */
    int fx_len0 = t->len, fx_nl0 = fn->nlines;
    int fx_sc0 = F.st->ncall, fx_se0 = F.st->next, fx_ss0 = F.st->nstr,
        fx_sg0 = F.st->ng, fx_sf0 = F.st->nf;
    F.fx_lazy = rv_needs_fx(fn);
    F.fx_missed = 0;
  fx_again:
    layout(&F);

    /* One more label than the IR has: the epilogue, which every IR_RET
     * jumps to so the frame size is written down once. */
    F.label_off = xmalloc((size_t)(fn->nlabels + 1) * sizeof *F.label_off);
    for (i = 0; i <= fn->nlabels; i++)
        F.label_off[i] = -1;

    /* BRANCH RELAXATION, as on Thumb: the function is emitted twice, the
     * first pass with every jump and branch in its longest form, the
     * second with each in the shortest form the first measured it reaches
     * in. Nothing else emitted depends on a code address, so the second
     * pass makes the same jumps and branches in the same order and the
     * ordinal matches them. Optimising builds only. */
    {
    int len0 = t->len, nl0 = fn->nlines;
    int sc0 = F.st->ncall, se0 = F.st->next, ss0 = F.st->nstr,
        sg0 = F.st->ng, sf0 = F.st->nf;
    signed char *relax = NULL;
    int nrelax = 0;
    for (int pass = 0; pass < 2; pass++) {
    F.fb = RV_SP;
    if (pass) {
        t->len = len0;
        fn->nlines = nl0;
        F.st->ncall = sc0; F.st->next = se0; F.st->nstr = ss0;
        F.st->ng = sg0; F.st->nf = sf0;
        F.nfix = 0;
        for (i = 0; i <= fn->nlabels; i++)
            F.label_off[i] = -1;
        F.skip_next = 0;
        F.va_first = -1;
        F.relax = relax;
        F.nrelax = nrelax;
        if (want_debug) {
            free(fn->var_off);
            fn->var_off = NULL;
        }
    }
    /* Align the function to four, with padding that traps if it is ever
     * reached. The loop that used to be here added FOUR bytes at a time,
     * which never terminates once the C extension can leave t->len at
     * two mod four -- it was an out-of-memory on the second function of
     * any unit. Every instruction is two or four bytes, so at most one
     * halfword is ever needed, and c.unimp (the all-zero encoding, a
     * defined illegal instruction) is exactly two. */
    /* Only without the C extension. With it every instruction is
     * two-aligned and so is every function, which is what clang emits: the
     * c.unimp that rounded each function up to four was two bytes of
     * nothing after about one function in two -- 236 bytes across lib/libc's
     * non-math code. */
    /* An interrupt handler is four-aligned with it too: mtvec's low
     * two bits are its mode, not its address (see rv_isr_grow). */
    if (!rv_compress_enabled() || F.isr) {
        if (t->len & 3)
            rv_cunimp(t);
        while (t->len & 3)
            rv_unimp(t);
    }
    f->code_align = rv_compress_enabled() && !F.isr ? 2 : 4;
    /* -g: each source variable's slot, which IS its offset from the
     * DWARF frame base -- sp, because this backend keeps no frame
     * pointer (src/debug/dwarf.c). */
    if (want_debug) {
        int nv = fn->nvars ? fn->nvars : 1;
        fn->var_off = xmalloc((size_t)nv * sizeof *fn->var_off);
        for (int v = 0; v < fn->nvars; v++)
            fn->var_off[v] = (int)F.slot[v];
    }
    f->code_off = t->len;

    /* The prologue. `addi sp, sp, -frame` reaches 2047 bytes; a larger
     * frame builds the constant first, and t0 is free to do it in because
     * no argument has been touched yet. */
    if (F.isr) {
        rv_isr_prologue(&F);
    } else if (F.frame) {
        if (rv_fits(-F.frame, 12)) {
            rv_alu_imm(t, RV_ADD, RV_SP, RV_SP, (int)-F.frame, 0);
        } else {
            rv_li(t, RV_T0, -F.frame, xlen);
            rv_alu(t, RV_ADD, RV_SP, RV_SP, RV_T0, 0);
        }
    }
    if (!F.leaf && !F.isr)
        st_sp(&F, RV_RA, F.ra_slot, F.w);
    for (i = 0; i < F.nsave; i++)
        st_sp(&F, F.used_callee[i], F.save_at + (long)i * F.w, F.w);
    /* the callee-saved f registers, as wide as the ABI preserves them */
    for (i = 0; i < F.nfsave; i++)
        fst_sp(&F, F.fused[i], F.fsave_at + (long)i * 8,
               target_riscv_abi_flen() == 64);
    if (fn->has_alloca) {
        rv_mv(t, RV_FP, RV_SP);        /* the frame base, from here on */
        F.fb = RV_FP;
    }

    /* A variadic function spills EVERY argument register, named ones
     * included: the named ones are read out of the spill below, and the
     * unnamed ones have to be there for va_arg to walk into. */
    if (fn->is_varargs)
        for (int k = 0; k < RV_NARGREG; k++)
            st_sp(&F, argreg(k), F.va_regsave + (long)k * F.w, F.w);

    /* The parameters arrive in a0-a7 and on the stack above the frame;
     * the prologue writes each to its slot, which is what every later
     * reference reads. */
    {
        struct argplace pl;
        struct rv_walk wk;
        long base = F.frame;       /* the caller's outgoing area */
        int pmv_dst[RA_MAXPOOL], pmv_src[RA_MAXPOOL], npmv = 0;
        int pstk_reg[RA_MAXPOOL]; long pstk_off[RA_MAXPOOL];
        int npstk = 0;
        /* fa registers into f-register homes: one parallel move */
        int pfm_d[MAX_PARAMS], pfm_s[MAX_PARAMS], npfm = 0;
        /* fa registers into x-register homes, staged through the frame
         * (F.fstage) and loaded after the x registers' parallel move */
        int pfx_v[MAX_PARAMS], pfx_d[MAX_PARAMS], npfx = 0;
        /* integer-rules parameters into f-register homes, after the f
         * registers' parallel move (a home may be an fa register still
         * to be read) and before the x registers' */
        int plate_v[MAX_PARAMS], nplate = 0;
        struct argplace plate_pl[MAX_PARAMS];
        walk_init(&wk, F.sret_slot >= 0);
        if (F.sret_slot >= 0)
            st_sp(&F, argreg(0), F.sret_slot, F.w);
        for (i = 0; i < fn->nparams; i++) {
            struct ir_arg *a = &fn->param_abi[i];
            place_one(F.w, &wk, a, 0, &pl);
            if (pl.hf) {
                /* The hardware-float CC: a float in fa0-fa7 straight to
                 * its home -- an f register (never an fa one) or a slot --
                 * except an x register, which may be an argument register
                 * the parallel move below has still to read. A flattened
                 * struct's fields into its slot. */
                if (!a->is_struct) {
                    int fr = RV_FA0 + pl.fld[0].reg, dbl = pl.fld[0].size == 8;
                    if (in_freg(&F, i)) {
                        pfm_d[npfm] = F.floc[i];
                        pfm_s[npfm] = fr;
                        npfm++;
                    } else if (in_reg(&F, i)) {
                        fst_sp(&F, fr, F.fstage + 8L * npfx, dbl);
                        pfx_v[npfx] = i; pfx_d[npfx] = dbl;
                        npfx++;
                    } else {
                        fdone(&F, i, fr, dbl);
                    }
                    continue;
                }
                for (int q = 0; q < pl.nfld; q++) {
                    long off = sslot(&F, i) + pl.fld[q].off;
                    if (pl.fld[q].fp) {
                        fst_sp(&F, RV_FA0 + pl.fld[q].reg, off,
                               pl.fld[q].size == 8);
                    } else {
                        struct argplace ip = pl;
                        ip.reg = pl.fld[q].reg;
                        st_sp(&F, param_reg(&F, &ip, 0), off, pl.fld[q].size);
                    }
                }
                continue;
            }
            if (!a->is_struct && in_freg(&F, i)) {
                plate_v[nplate] = i;
                plate_pl[nplate] = pl;
                nplate++;
                continue;
            }
            if (pl.byref) {
                /* What arrived is a POINTER to the caller's private
                 * copy, and the body expects the OBJECT in the local's
                 * slot -- every ldvar in it is an offset from there. So
                 * the prologue copies it in.
                 *
                 * That is a second copy on top of the caller's, and it
                 * is the price of the IR modelling a by-value parameter
                 * as an ordinary local. Storing the pointer instead
                 * would make a struct parameter's slot sometimes hold an
                 * object and sometimes an address, which is how u20()
                 * came to print a stack address where it meant 190. */
                {
                    int src = TMP;
                    if (pl.nreg) src = param_reg(&F, &pl, 0);
                    else         ld_sp(&F, TMP, base + pl.stk, F.w, 1);
                    copy_block_at(&F, 1, a->size, byref_step(F.w, a),
                                  src, 0, F.fb, sslot(&F, i));
                }
                continue;
            }
            /* A SCALAR occupies whole registers and a whole slot: store
             * the register. Only a COMPOSITE has a partial last word, and
             * only it may be written byte by byte -- doing that to a
             * scalar stores one byte of it and leaves the rest of the
             * slot holding whatever the frame had. */
            if (!a->is_struct) {
                if (a->size > F.w && in_reg(&F, i)) {
                    /* RV32's register pair, into the pair the allocator
                     * gave it -- which may be ARGUMENT registers
                     * (rv_pair_alloc), so each half is an edge of the
                     * parallel move below like any register parameter:
                     * written here, `mv a0,a1` destroyed the int that
                     * arrived in a0 before it was read. A half that came
                     * on the stack is a deferred load. A variadic
                     * function's pairs are callee-saved and its param_reg
                     * reads the save area, so there a pair is loaded at
                     * once, which nothing else can be reading. */
                    for (int q = 0; q < 2; q++) {
                        if (q < pl.nreg && !fn->is_varargs) {
                            pmv_dst[npmv] = F.loc[i] + q;
                            pmv_src[npmv] = argreg(pl.reg + q);
                            npmv++;
                        } else if (q < pl.nreg) {
                            rv_mv(t, F.loc[i] + q, param_reg(&F, &pl, q));
                        } else {
                            pstk_reg[npstk] = F.loc[i] + q;
                            pstk_off[npstk] = base + pl.stk +
                                              (long)(q - pl.nreg) * F.w;
                            npstk++;
                        }
                    }
                } else if (a->size > F.w) {
                    /* RV32's register pair: two words, low first, to its
                     * slot. */
                    for (int q = 0; q < pl.nreg; q++)
                        st_sp(&F, param_reg(&F, &pl, q),
                              sslot(&F, i) + (long)q * F.w, F.w);
                    for (int q = 0; q < pl.nstk; q++) {
                        ld_sp(&F, SCR, base + pl.stk + (long)q * F.w, F.w, 1);
                        st_sp(&F, SCR,
                              sslot(&F, i) + (long)(pl.nreg + q) * F.w, F.w);
                    }
                } else if (pl.nreg && in_reg(&F, i) && !fn->is_varargs) {
                    /* ALLOCATED, and arriving in a register: this is
                     * one edge of a PARALLEL MOVE, deferred until every
                     * parameter has been placed. Writing it here would
                     * destroy an incoming argument another parameter
                     * has not read yet -- which is the second of the
                     * three sites regalloc.h names, and the second bug
                     * the parked ARMv7-M attempt got from open-coding
                     * the ordering. */
                    pmv_dst[npmv] = F.loc[i];
                    pmv_src[npmv] = argreg(pl.reg);
                    npmv++;
                } else if (pl.nreg && in_reg(&F, i)) {
                    /* A VARIADIC function's named parameters do not
                     * come from their argument registers: the prologue
                     * has already spilled all eight, and param_reg
                     * reads them back out of the save area. So this is
                     * a LOAD, not a move -- feeding param_reg's scratch
                     * into the parallel move as a source gave every
                     * parameter the same register and read `d` as 10.
                     *
                     * Loading into the allocated register is safe here
                     * because rv_pool_for hands a variadic function no
                     * argument register at all, so nothing being loaded
                     * can land on a source still to be read. */
                    pstk_reg[npstk] = F.loc[i];
                    pstk_off[npstk] = F.va_regsave + (long)pl.reg * F.w;
                    npstk++;
                } else if (pl.nreg) {
                    st_sp(&F, param_reg(&F, &pl, 0), sslot(&F, i), F.w);
                } else if (in_reg(&F, i)) {
                    /* On the stack, and allocated: a load straight into
                     * its register. Loads address off sp and so cannot
                     * disturb an incoming argument register -- but it
                     * could WRITE one another parameter still needs, so
                     * it waits for the parallel move too. */
                    pstk_reg[npstk] = F.loc[i];
                    pstk_off[npstk] = base + pl.stk;
                    npstk++;
                } else {
                    ld_sp(&F, SCR, base + pl.stk, F.w, 1);
                    st_sp(&F, SCR, sslot(&F, i), F.w);
                }
                continue;
            }
            for (int q = 0; q < pl.nreg; q++) {
                long off = sslot(&F, i) + (long)q * F.w;
                long left = a->size - (long)q * F.w;
                int r = param_reg(&F, &pl, q);
                if (left >= F.w) {
                    st_sp(&F, r, off, F.w);
                } else {
                    /* An odd-sized composite's tail: store only the bytes
                     * the object has, lowest first. */
                    for (long b = 0; b < left; b++) {
                        if (b) rv_shift_imm(t, RV_SRL, r, r, 8, 0, xlen);
                        st_sp(&F, r, off + b, 1);
                    }
                }
            }
            for (int q = 0; q < pl.nstk; q++) {
                long src = base + pl.stk + (long)q * F.w;
                long dst = sslot(&F, i) + (long)(pl.nreg + q) * F.w;
                long left = a->size - (long)(pl.nreg + q) * F.w;
                ld_sp(&F, SCR, src, F.w, 1);
                if (left >= F.w) {
                    st_sp(&F, SCR, dst, F.w);
                } else {
                    for (long b = 0; b < left; b++) {
                        if (b) rv_shift_imm(t, RV_SRL, SCR, SCR, 8, 0, xlen);
                        st_sp(&F, SCR, dst + b, 1);
                    }
                }
            }
        }
        /* The f registers' parallel move, then the parameters that came
         * by the integer rules into f-register homes -- a float under
         * ilp32/lp64, a double under ilp32f, one past fa7: read while the
         * argument registers still hold what arrived, written where
         * nothing else still reads. At RV32 a double's words go through
         * fx. */
        fp_parallel_move(&F, pfm_d, pfm_s, npfm);
        for (int k = 0; k < nplate; k++) {
            int p = plate_v[k], fr = F.floc[p], dbl = F.fw[p] == 8;
            struct argplace *pp = &plate_pl[k];
            if (dbl && xlen == 32) {
                need_fx(&F);
                for (int q = 0; q < 2; q++) {
                    int r = q < pp->nreg ? param_reg(&F, pp, q) : SCR;
                    if (q >= pp->nreg)
                        ld_sp(&F, SCR, base + pp->stk +
                              (long)(q - pp->nreg) * F.w, 4, 1);
                    st_sp(&F, r, F.fx + 4L * q, 4);
                }
                fld_sp(&F, fr, F.fx, 1);
            } else if (pp->nreg) {
                f_from_x(&F, fr, param_reg(&F, pp, 0), dbl ? 8 : 4);
            } else {
                fld_sp(&F, fr, base + pp->stk, dbl);
            }
        }
        /* The parallel move, now that every parameter has been placed:
         * the register-to-register edges first, in an order that
         * destroys nothing, and then the loads -- which only WRITE
         * argument registers, so by then no incoming one is still
         * wanted. SCR breaks a cycle and holds nothing of its own. */
        if (npmv) {
            int od[RA_MAXPOOL * 2], os[RA_MAXPOOL * 2];
            int n = ra_parallel_move(pmv_dst, pmv_src, npmv, SCR, od, os,
                                     (int)(sizeof od / sizeof od[0]));
            if (n < 0)
                internal_error("riscv: %s: the prologue's parameter "
                               "placement is not a well-formed move",
                               fn->name);
            for (int k = 0; k < n; k++)
                rv_mv(t, od[k], os[k]);
        }
        for (int k = 0; k < npstk; k++)
            ld_sp(&F, pstk_reg[k], pstk_off[k], F.w, 1);
        for (int k = 0; k < npfx; k++) {
            int p = pfx_v[k];
            long off = F.fstage + 8L * k;
            if (pfx_d[k] && xlen == 32) {
                ld_sp(&F, F.loc[p], off, 4, 1);
                ld_sp(&F, F.loc[p] + 1, off + 4, 4, 1);
            } else {
                ld_sp(&F, F.loc[p], off, pfx_d[k] ? 8 : 4, 1);
            }
        }

        /* Where the first UNNAMED argument sits -- simply where the named
         * ones stopped. The save area and the caller's stack arguments
         * are contiguous, so one expression covers both cases: below
         * eight named words it is inside the save area, and at eight it
         * is exactly its end, which is the stack. (A float in an fa
         * register took no a register.) */
        if (fn->is_varargs)
            F.va_first = F.va_regsave + (long)wk.narg * F.w + wk.stk;
    }

    int tail_end = 0;        /* the body's last act is a tail call */
    for (i = 0; i < fn->nins; i++) {
        int was_tail = F.tail && F.tail[i];
        gen_ins(&F, i);
        if (F.skip_next) {          /* the comparison emitted its branch */
            F.skip_next = 0;
            i++;
        }
        tail_end = i == fn->nins - 1 && was_tail;
    }

    /* The epilogue -- unless nothing reaches it: the body ended in a tail
     * call and no IR_RET jumps here. */
    F.label_off[fn->nlabels] = t->len;
    for (i = 0; tail_end && i < F.nfix; i++)
        if (F.fix[i].label == fn->nlabels)
            tail_end = 0;
    if (!tail_end) {
    if (fn->has_alloca) {
        /* Release every VLA at once: sp back to the frame base. The
         * restores below then address from sp, because s0 is one of the
         * registers they restore. */
        rv_mv(t, RV_SP, RV_FP);
        F.fb = RV_SP;
    }
    if (F.isr) {
        rv_isr_epilogue(&F);
    } else {
        rv_restore(&F);
        rv_ret(t);
    }
    }

    for (i = 0; i < F.nfix; i++) {
        int target = F.label_off[F.fix[i].label], ok = 1;
        if (target < 0)
            internal_error("riscv: label %d of %s was never placed",
                           F.fix[i].label, fn->name);
        switch (F.fix[i].kind) {
        case FX_J: case FX_LONG:
            rv_patch_j(t, F.fix[i].at, target);
            break;
        case FX_CJ:
            ok = rv_patch_cj(t, F.fix[i].at, target);
            break;
        case FX_B:
            ok = rv_patch_b_checked(t, F.fix[i].at, target);
            break;
        case FX_TAB:
            code_patch32(t, F.fix[i].at,
                         (unsigned long)(unsigned int)(target - F.fix[i].bat));
            break;
        case FX_ADDR:
            rv_patch_pcrel_pair(t, F.fix[i].at, target);
            break;
        default:                                    /* FX_CB */
            ok = rv_patch_cb(t, F.fix[i].at, F.fix[i].cond == RV_BNE,
                             F.fix[i].rs1, target);
            break;
        }
        /* Measured to fit on the first pass, and only shorter since. An
         * offset that did not fit would be a jump somewhere else. */
        if (!ok)
            internal_error("riscv: %s: a relaxed branch no longer reaches "
                           "its label", fn->name);
    }

    /* After the first pass: the shortest form each jump and branch
     * reaches in, measured from this pass's positions. */
    if (pass == 0) {
        int any = 0;
        nrelax = F.nfix;
        relax = xcalloc((size_t)(nrelax ? nrelax : 1), 1);
        for (i = 0; i < F.nfix; i++) {
            long tgt = F.label_off[F.fix[i].label];
            if (F.fix[i].kind == FX_TAB || F.fix[i].kind == FX_ADDR) {
                relax[i] = (signed char)F.fix[i].kind;  /* not a branch */
                continue;
            }
            if (F.fix[i].kind == FX_J) {
                long d = tgt - F.fix[i].at;
                relax[i] = d >= -2048 && d <= 2046 ? FX_CJ : FX_J;
            } else {
                long d = tgt - F.fix[i].bat;
                int c = F.fix[i].cond, r1 = F.fix[i].rs1;
                if ((c == RV_BEQ || c == RV_BNE) && F.fix[i].rs2 == RV_ZERO &&
                    r1 >= 8 && r1 <= 15 && d >= -256 && d <= 254)
                    relax[i] = FX_CB;
                else if (d >= -4096 && d <= 4094)
                    relax[i] = FX_B;
                else
                    relax[i] = FX_LONG;
            }
            any |= relax[i] != FX_J && relax[i] != FX_LONG;
        }
        if (!any || !g_rv_regalloc)
            break;
    }
    }                                   /* the passes */
    free(relax);
    }
    /* An interrupt handler is emitted again until it saves everything it
     * writes (rv_isr_grow) -- once more than it would be otherwise, for
     * any handler that writes a register. */
    int isr_grew = F.isr && rv_isr_grow(&F);
    if ((F.fx_lazy && F.fx_missed) || isr_grew) {
        t->len = fx_len0;
        fn->nlines = fx_nl0;
        F.st->ncall = fx_sc0; F.st->next = fx_se0; F.st->nstr = fx_ss0;
        F.st->ng = fx_sg0; F.st->nf = fx_sf0;
        F.nfix = 0;
        F.skip_next = 0;
        F.va_first = -1;
        F.relax = NULL;
        F.nrelax = 0;
        free(F.slot);
        F.slot = NULL;
        free(F.label_off);
        F.label_off = NULL;
        if (want_debug) {
            free(fn->var_off);
            fn->var_off = NULL;
        }
        if (F.fx_missed) {
            F.fx_lazy = 0;
            F.fx_missed = 0;
        }
        goto fx_again;
    }

    f->code_len = t->len - f->code_off;
    f->stack_bytes = (int)F.frame;     /* what -fstack-usage reports */
    free(F.usecnt);
    free(F.tail);
    free(F.slot);
    cg_note_labels(fn, F.label_off);
    free(F.label_off);
    free(F.fix);
    free(F.wide);
    free(F.w16);
    free(F.sx);
    free(F.nshr);
    free(F.loc);
    free(F.floc);
    free(F.fw);
    free(F.f4);
}

/* ---- s0 AND s1 FOR THE BUSIEST VALUES ----------------------------------
 *
 * The compressed forms reach x8-x15 only for most of what they do --
 * c.lw and c.sw (base and value), c.and/or/xor/sub, c.andi, c.srli,
 * c.srai, c.beqz/c.bnez -- and of the callee-saved registers just s0 and
 * s1 are in that range. The colourer gives callee-saved registers out in
 * pool order to whichever value it reaches first, so a pointer every load
 * in a loop goes through could land in s4 and make each of them four
 * bytes instead of two.
 *
 * The callee-saved registers are interchangeable (the prologue saves a
 * set), so once allocation is done the ones the function used are renamed
 * among themselves, the busiest by that measure first into s0, then s1,
 * then on in pool order: loads and stores count twice, a move, call or
 * return not at all (c.mv and c.add take any register), a loop body
 * eight times per level. The set, and the saves, are unchanged. A
 * register a 64-bit pair uses is left alone. As on Cortex-M, the weights
 * are an estimate, so gen_func_best tries the function both ways. */
struct rv_lowreg_w {
    const int *loc;
    long w[32];
    long f;                    /* this instruction's weight */
};

static void rv_lowreg_count(int v, void *ctx)
{
    struct rv_lowreg_w *c = ctx;
    if (v >= 0 && c->loc[v] >= 0 && c->loc[v] < 32)
        c->w[c->loc[v]] += c->f;
}

static void rv_lowregs(const struct ir_func *fn, int *loc, unsigned long fixed)
{
    int np;
    const int *pool = rv_pool_for(fn, &np);
    unsigned long cand = 0, seen = 0;
    for (int j = 0; j < np; j++)
        if (rv_callee_saved(pool[j]))
            cand |= 1UL << pool[j];
    for (int v = 0; v < fn->nvregs; v++)
        if (loc[v] >= 0 && loc[v] < 32)
            seen |= 1UL << loc[v];
    cand &= seen & ~fixed;
    if (!cand || !(cand & (cand - 1)))
        return;

    /* Loop depth by back edge, as t_lowregs does. */
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
    struct rv_lowreg_w c;
    c.loc = loc;
    for (int r = 0; r < 32; r++)
        c.w[r] = 0;
    int d = 0;
    for (int n = 0; n < fn->nins; n++) {
        d += depth[n];
        const struct ir_ins *i = &fn->ins[n];
        int k = i->op == IR_LOAD || i->op == IR_STORE ? 2 :
                i->op == IR_MOV || i->op == IR_CALL || i->op == IR_RET ? 0 : 1;
        c.f = (long)k * (d <= 0 ? 1 : d == 1 ? 8 : 64);
        int def = ra_ins_def(i);
        if (def >= 0 && def < fn->nvregs && loc[def] >= 0 && loc[def] < 32)
            c.w[loc[def]] += c.f;
        ra_each_use(i, rv_lowreg_count, &c);
    }
    free(depth);
    free(lpos);

    /* Heaviest first onto the earliest in pool order (s0, s1, s2, ...). */
    int from[32], to[32], n = 0;
    for (int j = 0; j < np; j++)
        if (cand >> pool[j] & 1)
            to[n] = from[n] = pool[j], n++;
    for (int a = 1; a < n; a++)
        for (int b = a; b > 0 && c.w[from[b]] > c.w[from[b - 1]]; b--) {
            int t = from[b]; from[b] = from[b - 1]; from[b - 1] = t;
        }
    int map[32];
    for (int r = 0; r < 32; r++)
        map[r] = r;
    for (int k = 0; k < n; k++)
        map[from[k]] = to[k];
    for (int v = 0; v < fn->nvregs; v++)
        if (loc[v] >= 0 && loc[v] < 32 && (cand >> loc[v] & 1))
            loc[v] = map[loc[v]];
}

/* At RV32 with the allocator on, a function is generated with the pair
 * pass and without it, and the shorter is kept. A pair the pass takes is
 * withheld from the ordinary pool for the whole function, which costs a
 * function whose integer values wanted those registers more than its
 * doubles did: over tests/ and lib/libc, eight files came out larger by
 * up to 60 bytes while the total fell 12.8%. A discarded attempt is
 * undone by truncating what it appended -- the code and the five site
 * lists, which only ever grow.
 *
 * Each attempt is also made with the callee-saved registers renamed for
 * the compressed forms (rv_lowregs) and without, at RV32 and RV64 alike,
 * and the shortest kept; the first of equal sizes wins.
 * EMBCC_RV_LOWREGS=0/1 forces that choice. */
static void gen_func_best(struct ir_func *fn, struct code *t,
                          struct rv_sites *st, int xlen, int want_debug)
{
    int at = t->len, ncall = st->ncall, next = st->next, nstr = st->nstr,
        ng = st->ng, nf = st->nf, with;
    const char *knob = getenv("EMBCC_RV_PAIRS");

    const char *only = getenv("EMBCC_RV_PAIRS_ONLY");

    /* A field's constant offset into its load or store (lw r, k(rn)) --
     * once, before any attempt, and before allocation. */
    if (g_rv_regalloc && !want_debug && !g_rv_o0 &&
        !getenv("EMBCC_NO_MEMOFF")) {
        char *w = xlen == 32 ? wide_map(fn) : NULL;
        ra_fold_memoff(fn, -2048, 2047, xlen / 8, xlen / 8, w, 0, 0);
        free(w);
    }
    const char *lr = getenv("EMBCC_RV_LOWREGS");
    int lr_forced = lr && *lr;
    g_rv_pairs = 1;
    g_rv_lowregs = lr_forced ? atoi(lr) != 0 : 1;
    if (!g_rv_regalloc || want_debug || g_rv_o0) {
        gen_func(fn, t, st, xlen, want_debug);
        g_rv_lowregs = 1;
        return;
    }
    /* The attempts: pairs on and off (RV32 only, unless a knob fixes
     * them), each with the rename on and off (unless EMBCC_RV_LOWREGS
     * fixes it). */
    int pv[2], np = 0, lv[2], nl = 0;
    if (xlen != 32 || (knob && *knob) || (only && *only)) {
        pv[np++] = !(knob && *knob) || atoi(knob);
        if (only && *only)
            pv[0] = strcmp(only, fn->name) == 0;
    } else {
        pv[np++] = 1;
        pv[np++] = 0;
    }
    lv[nl++] = g_rv_lowregs;
    if (!lr_forced && rv_compress_enabled())
        lv[nl++] = 0;
    int best = -1, bestlen = 0, last = -1;
    for (int a = 0; a < np * nl; a++) {
        if (a) {
            t->len = at; st->ncall = ncall; st->next = next; st->nstr = nstr;
            st->ng = ng; st->nf = nf;
        }
        g_rv_pairs = pv[a / nl];
        g_rv_lowregs = lv[a % nl];
        gen_func(fn, t, st, xlen, want_debug);
        with = t->len - at;
        last = a;
        if (best < 0 || with < bestlen) {
            best = a;
            bestlen = with;
        }
    }
    if (best != last) {
        t->len = at; st->ncall = ncall; st->next = next; st->nstr = nstr;
        st->ng = ng; st->nf = nf;
        g_rv_pairs = pv[best / nl];
        g_rv_lowregs = lv[best % nl];
        gen_func(fn, t, st, xlen, want_debug);
    }
    g_rv_pairs = 1;
    g_rv_lowregs = 1;
}

/* ---- .riscv.attributes ------------------------------------------------
 *
 * The RISC-V psABI's build attributes, in the same container as ARM's:
 * format 'A', a vendor subsection ("riscv") and a File sub-subsection of
 * tag/value pairs, both lengths counting themselves. Read back off
 * clang's object for the same triple rather than transcribed. */
enum { Tag_RISCV_stack_align = 4, Tag_RISCV_arch = 5 };

static void ab_put(unsigned char **p, size_t *n, size_t *cap, unsigned v)
{
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 64;
        *p = xrealloc(*p, *cap);
    }
    (*p)[(*n)++] = (unsigned char)v;
}

static void ab_u32(unsigned char **p, size_t *n, size_t *cap, unsigned long v)
{
    for (int k = 0; k < 4; k++)
        ab_put(p, n, cap, (unsigned)(v >> (8 * k)) & 0xff);
}

static void ab_str(unsigned char **p, size_t *n, size_t *cap, const char *s)
{
    while (*s)
        ab_put(p, n, cap, (unsigned char)*s++);
    ab_put(p, n, cap, 0);
}

unsigned char *riscv_build_attributes(size_t *len)
{
    /* I, M and A -- mul/div and the lr/sc atomics are emitted -- plus F
     * and D as -march= says (with the Zicsr they imply), C when
     * target_riscv_rvc says so, and Zifencei when -march= named it: in
     * clang's order. A disassembler reads this to know which
     * instructions to decode. */
    char arch[96];
    int flen = target_riscv_flen();
    snprintf(arch, sizeof arch, "rv%di2p1_m2p0_a2p1%s%s%s%s%s",
             target_xlen(), flen ? "_f2p2" : "", flen == 64 ? "_d2p2" : "",
             target_riscv_rvc() ? "_c2p0" : "", flen ? "_zicsr2p0" : "",
             target_riscv_zifencei() ? "_zifencei2p0" : "");
    unsigned char *a = NULL, *o = NULL;
    size_t na = 0, ca = 0, no = 0, co = 0;
    ab_put(&a, &na, &ca, Tag_RISCV_stack_align);
    ab_put(&a, &na, &ca, 16);                   /* uleb128 16 */
    ab_put(&a, &na, &ca, Tag_RISCV_arch);
    ab_str(&a, &na, &ca, arch);

    ab_put(&o, &no, &co, 'A');                  /* format version */
    ab_u32(&o, &no, &co, (unsigned long)(4 + sizeof "riscv" + 1 + 4 + na));
    ab_str(&o, &no, &co, "riscv");
    ab_put(&o, &no, &co, 1);                    /* Tag_File */
    ab_u32(&o, &no, &co, (unsigned long)(1 + 4 + na));
    for (size_t k = 0; k < na; k++)
        ab_put(&o, &no, &co, a[k]);
    free(a);
    *len = no;
    return o;
}

void codegen_unit_riscv(struct ir_unit *iu, struct code *text,
                        struct extcall **ext, int *next,
                        struct strsite **strs, int *nstrs,
                        struct gsite **gs, int *ngs,
                        struct fsite **fs, int *nfs, int want_debug,
                        int optimize, int no_sse, int regalloc)
{
    struct rv_sites st;
    int xlen = target_xlen();

    (void)no_sse;
    g_rv_o0 = !optimize;
    /* The C extension: target_riscv_rvc, which the object's e_flags
     * read as well. */
    rv_set_compress(target_riscv_rvc(), xlen);
    g_rv_regalloc = regalloc;
    g_rv_iu = iu;
    memset(&st, 0, sizeof st);

    int text0 = text->len;
    /* EMBCC_RV_JAL_RANGE shrinks jal's reach, so the fallback -- which
     * real code meets only past 1 MB of text -- can be tested. */
    long reach = getenv("EMBCC_RV_JAL_RANGE") ? atol(getenv("EMBCC_RV_JAL_RANGE"))
                                              : 1L << 20;
    g_rv_short_calls = !getenv("EMBCC_RV_LONG_CALLS");
    for (;;) {
        int far = 0;
        for (int n = 0; n < iu->nfuncs; n++) {
            int ra = g_rv_regalloc;
            if (g_rv_o0 && ra_o0_too_big(&iu->funcs[n]))
                g_rv_regalloc = 0;     /* see ra_o0_too_big */
            gen_func_best(&iu->funcs[n], text, &st, xlen, want_debug);
            g_rv_regalloc = ra;
        }
        for (int k = 0; k < st.ncall; k++) {
            long disp = st.call[k].target->code_off - st.call[k].patch_off;
            if (st.call[k].jal && (disp < -reach || disp >= reach))
                far = 1;
        }
        if (!far || !g_rv_short_calls)
            break;
        /* A jal that does not reach: everything again, the long way. */
        g_rv_short_calls = 0;
        text->len = text0;
        free(st.call); free(st.ext); free(st.str); free(st.g); free(st.f);
        memset(&st, 0, sizeof st);
    }

    /* Intra-unit calls, now that every function has a place. The auipc
     * and the jalr are patched together: the auipc adds the HI20 of the
     * displacement to pc and the jalr adds the sign-extended LO12, so the
     * HI20 has to be rounded up when bit 11 is set -- the same +0x800 as
     * everywhere else in this target. */
    for (int k = 0; k < st.ncall; k++) {
        int at = st.call[k].patch_off;
        long disp = st.call[k].target->code_off - at;
        /* A tail call keeps its own registers: jal x0, and auipc t1
         * with jalr x0 through it. */
        int lr = st.call[k].tail ? RV_ZERO : RV_RA;
        int ar = st.call[k].tail ? RV_T1 : RV_RA;
        if (st.call[k].jal) {
            code_patch32(text, at, rv_enc_j(0x6f, lr, (int)disp));
            continue;
        }
        long hi = ((disp + 0x800) >> 12) & 0xfffff;
        int lo = (int)(((disp & 0xfff) ^ 0x800) - 0x800);
        code_patch32(text, at, rv_enc_u(0x17, ar, hi));
        code_patch32(text, at + 4, rv_enc_i(0x67, lr, 0, ar, lo));
    }
    free(st.call);

    cg_resolve_strsites(iu, st.str, st.nstr);

    *ext = st.ext;   *next = st.next;
    *strs = st.str;  *nstrs = st.nstr;
    *gs = st.g;      *ngs = st.ng;
    *fs = st.f;      *nfs = st.nf;
}
