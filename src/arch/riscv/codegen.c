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
 * computed goto, __int128, long double arithmetic, -g. THE RULE -- an
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
#define RV_NPOOL 20
static const int RV_POOL[RV_NPOOL] = {
    /* caller-saved: a0-a7, then the one spare temporary */
    RV_A0, RV_A1, RV_A2, RV_A3, RV_A4, RV_A5, RV_A6, RV_A7, RV_T3,
    /* callee-saved: s1, s2-s11. s0 is left out -- it is the frame
     * pointer by convention and DWARF names it as the frame base, and
     * a register the debugger believes in is not one to hand out. */
    RV_S1, RV_S2, RV_S2 + 1, RV_S2 + 2, RV_S2 + 3, RV_S2 + 4, RV_S2 + 5,
    RV_S2 + 6, RV_S2 + 7, RV_S2 + 8, RV_S2 + 9
};
/* The same list with the argument file removed, for a variadic
 * function: its prologue spills a0-a7 into the register save area and
 * `va_arg` walks them, so those eight are not the allocator's to give.
 */
static const int RV_POOL_VA[RV_NPOOL - 8] = {
    RV_T3,
    RV_S1, RV_S2, RV_S2 + 1, RV_S2 + 2, RV_S2 + 3, RV_S2 + 4, RV_S2 + 5,
    RV_S2 + 6, RV_S2 + 7, RV_S2 + 8, RV_S2 + 9
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
int rv_op_calls_helper(const struct ir_ins *i)
{
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
    NULL, NULL,   /* no FP class -- soft float lives in the core registers */
    1,            /* ...and so is allocated with them: every float lowering
                   * here goes through rd/wr/set_args (float_in_gpr) */
    NULL, NULL,
    1,            /* atomic_in_reg: every atomic reads its address and
                   * values through rdr and writes through wreg/wr */
    0,            /* fp_reads_gpr: floats are already general (above) */
    1             /* asm_in_reg: see IR_ASM */
};

/* -O2 and -Os: the allocator is on. */
static int g_rv_regalloc;

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
};

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
static long fn_sret_bytes(int wb, const struct ir_func *fn)
{
    return sret_bytes(wb, fn->ret_abi.size);
}

static long call_sret_bytes(int wb, const struct ir_ins *i)
{
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
    int narg = fn_sret_bytes(wb, fn) ? 1 : 0;
    long stk = 0;
    struct argplace pl;
    for (int p = 0; fn->src && p < fn->nparams && p < fn->nvregs; p++) {
        const struct ir_arg *a = &fn->param_abi[p];
        place_arg(wb, a->size, arg_align(wb, a), a->is_struct, 0,
                  &narg, &stk, &pl);
        if (pl.nreg == 1 && !pl.nstk && !pl.byref && !a->is_struct &&
            a->size <= wb)
            hint[p] = argreg(pl.reg);
    }
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        if (i->op == IR_RET && i->a >= 0 && i->a < fn->nvregs &&
            !fn->ret_abi.is_struct && fn->ret_abi.size <= wb)
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
        if (!i->retsize && i->dst >= 0 && i->dst < fn->nvregs && i->w <= wb)
            hint[i->dst] = RV_A0;
        narg = call_sret_bytes(wb, i) ? 1 : 0;
        stk = 0;
        for (int k = 0; k < i->nargs; k++) {
            const struct ir_arg *a = &i->argv[k];
            place_arg(wb, a->size, arg_align(wb, a), a->is_struct,
                      i->call_varargs && k >= i->call_nfixed,
                      &narg, &stk, &pl);
            if (pl.nreg == 1 && !pl.nstk && !pl.byref && !a->is_struct &&
                a->size <= wb && a->vreg >= 0 && a->vreg < fn->nvregs)
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
        int narg = 0;
        long stk = 0;
        if (i->op != IR_CALL)
            continue;
        if (call_sret_bytes(F->w, i))
            narg = 1;
        for (int k = 0; k < i->nargs; k++)
            place_arg(F->w, i->argv[k].size, arg_align(F->w, &i->argv[k]),
                      i->argv[k].is_struct,
                      i->call_varargs && k >= i->call_nfixed,
                      &narg, &stk, &pl);
        if (stk > most)
            most = stk;
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
        if (i->op != IR_CALL)
            continue;
        for (int k = 0; k < i->nargs; k++) {
            const struct ir_arg *a = &i->argv[k];
            if (a->size > 2 * F->w)
                need = ((need + 15) & ~15L) + a->size;
        }
        if (need > most)
            most = need;
    }
    return (most + STACK_ALIGN - 1) & ~(long)(STACK_ALIGN - 1);
}

static int in_reg(const struct rv_fn *F, int v);

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
     * to the coalescer as if it had a register. Then locals, small ones
     * first; one nothing names needs none (ra_locals_referenced), nor one
     * in a register (ra_slot_dead; under -g every local keeps its slot). */
    {
        int nv = fn->nvregs, npool = 0, has_cgoto = 0;
        int *loc2 = xmalloc((size_t)(nv ? nv : 1) * sizeof *loc2);
        for (int v = 0; v < nv; v++)
            loc2[v] = in_reg(F, v) || F->wide[v] || is16(F, v) ? 0 : -1;
        for (int n = 0; n < fn->nins; n++)
            if (fn->ins[n].op == IR_IGOTO || fn->ins[n].op == IR_LABELADDR)
                has_cgoto = 1;
        {
            struct ra_slots so = { loc2, NULL, g_rv_regalloc, has_cgoto };
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
            if (!F->wide[v] || in_reg(F, v) || is16(F, v))
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
                if (in_reg(F, v) || !lref[v] ||
                    ra_slot_dead(fn, F->loc, NULL, v, F->want_debug))
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
        int raw = F->leaf ? 0 : F->w;
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

static void rd(struct rv_fn *F, int v, int reg)
{
    if (in_reg(F, v)) {
        if (F->loc[v] != reg)
            rv_mv(F->t, reg, F->loc[v]);
        return;
    }
    ld_sp(F, reg, sslot(F, v), F->w, 1);
}

static void wr(struct rv_fn *F, int v, int reg)
{
    if (in_reg(F, v)) {
        if (F->loc[v] != reg)
            rv_mv(F->t, F->loc[v], reg);
        return;
    }
    if (F->slot[v] < 0)
        return;
    st_sp(F, reg, sslot(F, v), F->w);
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
    ld_sp(F, scratch, sslot(F, v), F->w, 1);
    return scratch;
}

static int wreg(struct rv_fn *F, int v, int scratch)
{
    return in_reg(F, v) ? F->loc[v] : scratch;
}

static void wrote(struct rv_fn *F, int v, int reg)
{
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
    if (in_reg(F, v)) {
        mv2(F, lo, F->loc[v], hi, F->loc[v] + 1);
        return;
    }
    ld_sp(F, lo, sslot(F, v), 4, 1);
    ld_sp(F, hi, sslot(F, v) + 4, 4, 1);
}

static void wr64(struct rv_fn *F, int v, int lo, int hi)
{
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
       FX_TAB };   /* a jump table's entry: bat is the auipc that finds
                    * the table, the word becomes target - auipc */

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
     * now nothing still needs the old contents of one. */
    for (int k = 0; k < n; k++)
        if (!in_reg(F, vreg[k])) {
            if (half)
                ld_sp(F, dstreg[k], sslot(F, vreg[k]) + 4L * half[k], 4, 1);
            else
                ld_sp(F, dstreg[k], sslot(F, vreg[k]), F->w, 1);
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
static void add64(struct rv_fn *F)
{
    struct code *t = F->t;
    rv_alu(t, RV_ADD, SCR, A_LO, B_LO, 0);
    rv_alu(t, RV_SLTU, SCR2, SCR, B_LO, 0);      /* did it wrap? */
    rv_mv(t, A_LO, SCR);
    rv_alu(t, RV_ADD, A_HI, A_HI, B_HI, 0);
    rv_alu(t, RV_ADD, A_HI, A_HI, SCR2, 0);
}

static void sub64(struct rv_fn *F)
{
    struct code *t = F->t;
    rv_alu(t, RV_SLTU, SCR2, A_LO, B_LO, 0);     /* will it borrow? */
    rv_alu(t, RV_SUB, A_LO, A_LO, B_LO, 0);
    rv_alu(t, RV_SUB, A_HI, A_HI, B_HI, 0);
    rv_alu(t, RV_SUB, A_HI, A_HI, SCR2, 0);
}

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
    case IR_ADD:
        rd64(F, i->a, A_LO, A_HI);
        operand_b64(F, i, B_LO, B_HI);
        add64(F);
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    case IR_SUB:
        rd64(F, i->a, A_LO, A_HI);
        operand_b64(F, i, B_LO, B_HI);
        sub64(F);
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
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
        rd(F, i->a, A_LO);
        if (i->size < 4)
            ext_reg(F, A_LO, A_LO, i->size, i->sign);
        if (i->sign) rv_shift_imm(t, RV_SRA, A_HI, A_LO, 31, 0, 32);
        else         rv_mv(t, A_HI, RV_ZERO);
        wr64(F, i->dst, A_LO, A_HI);
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
        rd64(F, i->a, A_LO, A_HI);
        if (i->size == 8)
            wr64(F, i->dst, A_LO, A_HI);
        else if (in_reg(F, i->dst))          /* sign-extends, as at 32 bits */
            ext_reg(F, F->loc[i->dst], A_LO, i->size, 1);
        else if (F->slot[i->dst] >= 0)
            st_sp(F, A_LO, sslot(F, i->dst), i->size);
        return 1;
    case IR_LOAD:
        rd(F, i->a, ADDR);
        if (i->size == 8) {
            rv_load(t, A_LO, ADDR, 0, 4, 1, F->xlen);
            rv_load(t, A_HI, ADDR, 4, 4, 1, F->xlen);
        } else {
            rv_load(t, A_LO, ADDR, 0, i->size, i->sign, F->xlen);
            if (i->sign) rv_shift_imm(t, RV_SRA, A_HI, A_LO, 31, 0, 32);
            else         rv_mv(t, A_HI, RV_ZERO);
        }
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    case IR_STORE:
        rd(F, i->a, ADDR);
        rd64(F, i->b, A_LO, A_HI);
        rv_store(t, A_LO, ADDR, 0, i->size == 8 ? 4 : i->size, F->xlen);
        if (i->size == 8)
            rv_store(t, A_HI, ADDR, 4, 4, F->xlen);
        return 1;
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
    int narg = 0;
    long stk = 0;

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
    for (int k = 0; k < i->nargs; k++) {
        place_arg(F->w, i->argv[k].size, arg_align(F->w, &i->argv[k]),
                  i->argv[k].is_struct, 0, &narg, &stk, &pl);
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

static void gen_call(struct rv_fn *F, int n)
{
    struct ir_func *fn = F->fn;
    struct ir_ins *i = &fn->ins[n];
    struct code *t = F->t;
    struct argplace pl[MAX_PARAMS];
    int narg = 0;
    long stk = 0, copy_at = F->byref_at;
    long sret = call_sret_bytes(F->w, i);

    if (sret)
        narg = 1;                          /* a0 holds the result's address */
    for (int k = 0; k < i->nargs; k++)
        place_arg(F->w, i->argv[k].size, arg_align(F->w, &i->argv[k]),
                  i->argv[k].is_struct,
                  i->call_varargs && k >= i->call_nfixed,
                  &narg, &stk, &pl[k]);

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
            rd(F, a->vreg, TMP);         /* its address */
        } else {
            need16(F, a->vreg);          /* RV32's long double: its slot */
            addr_sp(F, TMP, sslot(F, a->vreg));
        }
        addr_sp(F, ADDR, copy_at);
        copy_block(F, 1, a->size, byref_step(F->w, a));
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
            if (sw == 8 && F->wide[i->a]) {
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
                if (dw == 8 && F->wide[i->dst])
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

static void gen_ins(struct rv_fn *F, int n)
{
    struct ir_func *fn = F->fn;
    struct ir_ins *i = &fn->ins[n];
    struct code *t = F->t;

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
            rv_mv(t, TMP, RV_A0);
            cmp_to_reg(F, i->pred, 1, TMP, RV_ZERO, ACC);
            wr(F, i->dst, ACC);
            return;
        }
        if (i->op == IR_SQRT)
            rv_refuse(F, i, "__builtin_sqrt (a libm routine here, not an "
                            "instruction)");
        rv_refuse(F, i, "this floating-point operation");
    }

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

    case IR_MEMCPY: case IR_MEMZERO:
        rd(F, i->a, ADDR);
        if (i->op == IR_MEMCPY)
            rd(F, i->b, TMP);
        copy_block(F, i->op == IR_MEMCPY, i->size, F->w);
        return;

    case IR_CALL:
        gen_call(F, n);
        return;

    case IR_RET:
        if (i->a >= 0) {
            if (fn->ret_abi.is_struct) {
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
        if (i->op == IR_I2F && src_w == 8 && i->a >= 0 && !F->wide[i->a] &&
            !is16(F, i->a)) {
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
            if (F->xlen == 32 && F->wide[i->dst])
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
        if (aw != F->w && !(aw == 4 && F->xlen == 64))
            rv_refuse(F, i, aw < 4 ? "an atomic narrower than four bytes "
                                     "(the A extension has no such form, and "
                                     "a read-modify-write of the containing "
                                     "word is not atomic against its "
                                     "neighbours)"
                                   : "an atomic wider than a register");
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
        if (aw != F->w && !(aw == 4 && F->xlen == 64))
            rv_refuse(F, i, aw < 4 ? "an atomic compare-and-swap narrower "
                                     "than four bytes"
                                   : "an atomic wider than a register");
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
    case IR_LABELADDR: case IR_IGOTO:
        rv_refuse(F, i, "a computed goto");
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
    int wb = 4, narg = fn_sret_bytes(wb, fn) ? 1 : 0;
    long stk = 0;
    struct argplace pl;
    for (int p = 0; fn->src && p < fn->nparams && p < fn->nvregs; p++) {
        const struct ir_arg *a = &fn->param_abi[p];
        place_arg(wb, a->size, arg_align(wb, a), a->is_struct, 0,
                  &narg, &stk, &pl);
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
        narg = call_sret_bytes(wb, i) ? 1 : 0;
        stk = 0;
        for (int k = 0; k < i->nargs; k++) {
            const struct ir_arg *a = &i->argv[k];
            place_arg(wb, a->size, arg_align(wb, a), a->is_struct,
                      i->call_varargs && k >= i->call_nfixed,
                      &narg, &stk, &pl);
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
    unsigned long *li = NULL, *lo;
    int *dv = NULL, wds = 0;
    lo = ra_live_intervals(fn, first, last, &li, &dv, &wds);
    free(lo); free(li); free(dv);
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
            int narg = call_sret_bytes(wb, i) ? 1 : 0;
            long stk = 0;
            struct argplace pl;
            for (int k = 0; k < i->nargs; k++) {
                const struct ir_arg *a = &i->argv[k];
                place_arg(wb, a->size, arg_align(wb, a), a->is_struct,
                          i->call_varargs && k >= i->call_nfixed,
                          &narg, &stk, &pl);
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
        char *pin = want_debug ? ra_debug_pin_vars(fn) : (char *)0;
        int *pair = xlen == 32 && g_rv_pairs ? rv_pair_alloc(fn, &F, pin)
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
            F.loc = ra_allocate(fn, &RISCV_RA, ineligible, pin,
                                F.used_callee, &F.nsave);
            if (ineligible != F.wide && ineligible != F.w16)
                free(ineligible);
        }
        g_rv_taken = 0;
        if (pair) {
            for (int v = 0; v < fn->nvregs; v++)
                if (pair[v] >= 0) F.loc[v] = pair[v];
            for (int k = 0; k < F.npair; k++) {
                F.used_callee[F.nsave++] = F.pair_used[k];
                F.used_callee[F.nsave++] = F.pair_used[k] + 1;
            }
            free(pair);
        }
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
                for (int v = n; v < fn->nvregs; v++)
                    F.loc[v] = -1;
            }
        }
    }
    /* A variable-length array moves sp at run time, so the frame is
     * addressed from s0 instead, which the prologue sets once the frame
     * is in place. s0 is callee-saved and never in the allocator's pool,
     * so it only has to be saved like any other callee-saved register. */
    if (fn->has_alloca)
        F.used_callee[F.nsave++] = RV_FP;
    /* A leaf: no call in the IR and none the lowering makes -- the same
     * rv_op_calls_helper the allocator trusts for which values survive a
     * call, so the two cannot disagree. Inline asm might call anything,
     * so it keeps ra saved. */
    /* A tail call leaves ra alone -- it is the caller's, and the callee
     * returns with it -- so it does not make this function a non-leaf. */
    F.tail = NULL;
    if (g_rv_regalloc && !want_debug)
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
    if (!rv_compress_enabled()) {
        if (t->len & 3)
            rv_cunimp(t);
        while (t->len & 3)
            rv_unimp(t);
    }
    f->code_align = rv_compress_enabled() ? 2 : 4;
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
    if (F.frame) {
        if (rv_fits(-F.frame, 12)) {
            rv_alu_imm(t, RV_ADD, RV_SP, RV_SP, (int)-F.frame, 0);
        } else {
            rv_li(t, RV_T0, -F.frame, xlen);
            rv_alu(t, RV_ADD, RV_SP, RV_SP, RV_T0, 0);
        }
    }
    if (!F.leaf)
        st_sp(&F, RV_RA, F.ra_slot, F.w);
    for (i = 0; i < F.nsave; i++)
        st_sp(&F, F.used_callee[i], F.save_at + (long)i * F.w, F.w);
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
        int narg = 0;
        long stk = 0;
        long base = F.frame;       /* the caller's outgoing area */
        int pmv_dst[RA_MAXPOOL], pmv_src[RA_MAXPOOL], npmv = 0;
        int pstk_reg[RA_MAXPOOL]; long pstk_off[RA_MAXPOOL];
        int npstk = 0;
        if (F.sret_slot >= 0) {
            st_sp(&F, argreg(0), F.sret_slot, F.w);
            narg = 1;
        }
        for (i = 0; i < fn->nparams; i++) {
            struct ir_arg *a = &fn->param_abi[i];
            place_arg(F.w, a->size, arg_align(F.w, a), a->is_struct, 0,
                      &narg, &stk, &pl);
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
                if (pl.nreg) rv_mv(t, TMP, param_reg(&F, &pl, 0));
                else         ld_sp(&F, TMP, base + pl.stk, F.w, 1);
                addr_sp(&F, ADDR, sslot(&F, i));
                copy_block(&F, 1, a->size, byref_step(F.w, a));
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

        /* Where the first UNNAMED argument sits -- simply where the named
         * ones stopped. The save area and the caller's stack arguments
         * are contiguous, so one expression covers both cases: below
         * eight named words it is inside the save area, and at eight it
         * is exactly its end, which is the stack. */
        if (fn->is_varargs)
            F.va_first = F.va_regsave + (long)narg * F.w + stk;
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
    rv_restore(&F);
    rv_ret(t);
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
            if (F.fix[i].kind == FX_TAB) {             /* not a branch */
                relax[i] = FX_TAB;
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

    f->code_len = t->len - f->code_off;
    f->stack_bytes = (int)F.frame;     /* what -fstack-usage reports */
    free(F.usecnt);
    free(F.tail);
    free(F.slot);
    free(F.label_off);
    free(F.fix);
    free(F.wide);
    free(F.w16);
    free(F.sx);
    free(F.nshr);
    free(F.loc);
}

/* At RV32 with the allocator on, a function is generated with the pair
 * pass and without it, and the shorter is kept. A pair the pass takes is
 * withheld from the ordinary pool for the whole function, which costs a
 * function whose integer values wanted those registers more than its
 * doubles did: over tests/ and lib/libc, eight files came out larger by
 * up to 60 bytes while the total fell 12.8%. A discarded attempt is
 * undone by truncating what it appended -- the code and the five site
 * lists, which only ever grow. */
static void gen_func_best(struct ir_func *fn, struct code *t,
                          struct rv_sites *st, int xlen, int want_debug)
{
    int at = t->len, ncall = st->ncall, next = st->next, nstr = st->nstr,
        ng = st->ng, nf = st->nf, with;
    const char *knob = getenv("EMBCC_RV_PAIRS");

    const char *only = getenv("EMBCC_RV_PAIRS_ONLY");

    /* A field's constant offset into its load or store (lw r, k(rn)) --
     * once, before any attempt, and before allocation. */
    if (g_rv_regalloc && !want_debug && !getenv("EMBCC_NO_MEMOFF")) {
        char *w = xlen == 32 ? wide_map(fn) : NULL;
        ra_fold_memoff(fn, -2048, 2047, xlen / 8, xlen / 8, w);
        free(w);
    }
    g_rv_pairs = 1;
    if (xlen != 32 || !g_rv_regalloc || want_debug || (knob && *knob) ||
        (only && *only)) {
        if (knob && *knob) g_rv_pairs = atoi(knob);
        if (only && *only) g_rv_pairs = strcmp(only, fn->name) == 0;
        gen_func(fn, t, st, xlen, want_debug);
        g_rv_pairs = 1;
        return;
    }
    gen_func(fn, t, st, xlen, want_debug);
    with = t->len - at;
    t->len = at; st->ncall = ncall; st->next = next; st->nstr = nstr;
    st->ng = ng; st->nf = nf;
    g_rv_pairs = 0;
    gen_func(fn, t, st, xlen, want_debug);
    if (t->len - at > with) {
        t->len = at; st->ncall = ncall; st->next = next; st->nstr = nstr;
        st->ng = ng; st->nf = nf;
        g_rv_pairs = 1;
        gen_func(fn, t, st, xlen, want_debug);
    }
    g_rv_pairs = 1;
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
    /* I, M and A -- mul/div and the lr/sc atomics are emitted -- plus C
     * when target_riscv_rvc says so. No F or D: floating point is soft
     * (e_flags' float ABI bits are 0 to match). */
    const char *arch = target_xlen() == 64
        ? (target_riscv_rvc() ? "rv64i2p1_m2p0_a2p1_c2p0" : "rv64i2p1_m2p0_a2p1")
        : (target_riscv_rvc() ? "rv32i2p1_m2p0_a2p1_c2p0" : "rv32i2p1_m2p0_a2p1");
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

    (void)optimize; (void)no_sse;
    /* The C extension: target_riscv_rvc, which the object's e_flags
     * read as well. */
    rv_set_compress(target_riscv_rvc(), xlen);
    g_rv_regalloc = regalloc;
    memset(&st, 0, sizeof st);

    int text0 = text->len;
    /* EMBCC_RV_JAL_RANGE shrinks jal's reach, so the fallback -- which
     * real code meets only past 1 MB of text -- can be tested. */
    long reach = getenv("EMBCC_RV_JAL_RANGE") ? atol(getenv("EMBCC_RV_JAL_RANGE"))
                                              : 1L << 20;
    g_rv_short_calls = !getenv("EMBCC_RV_LONG_CALLS");
    for (;;) {
        int far = 0;
        for (int n = 0; n < iu->nfuncs; n++)
            gen_func_best(&iu->funcs[n], text, &st, xlen, want_debug);
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
