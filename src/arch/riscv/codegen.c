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
    struct { int patch_off; struct func *target; } *call;
    int ncall, capcall;
    struct extcall *ext;   int next, capext;
    struct strsite *str;   int nstr, capstr;
    struct gsite *g;       int ng, capg;
    struct fsite *f;       int nf, capf;
};

struct rv_fn {
    struct ir_func *fn;
    /* Per vreg: the register the allocator gave it, or -1 for one that
     * stays in memory. NULL when the allocator did not run (-O0/-O1),
     * which is what makes every helper below fall back to the slot
     * path the backend had before it existed. */
    int *loc;
    int used_callee[RA_MAXPOOL];  /* the callee-saved ones it took */
    int nsave;
    struct code *t;
    struct rv_sites *st;
    int xlen;            /* 32 or 64 */
    int w;               /* a register in bytes: 4 or 8 */
    char *wide;          /* per vreg: needs a register pair (RV32 only) */
    long *slot;          /* per-vreg byte offset from sp, -1 for none */
    long frame;          /* total bytes sp moves down by */
    long scratch_at;     /* where fn->scratch_bytes begins */
    long byref_at;       /* where the by-reference argument copies go */
    long sret_slot;      /* where the hidden result pointer is kept, or -1 */
    long ra_slot;        /* where the return address is saved */
    long save_at;        /* ... and the allocator's callee-saved ones */
    long va_regsave;     /* a variadic function's a0-a7 spill area, or -1 */
    long va_first;       /* ... and the offset of the first UNNAMED one */
    int *label_off;      /* per label id, or -1 while unseen */
    struct { int at; int label; } *fix;
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

static const int *rv_pool_for(const struct ir_func *fn, int *n)
{
    if (fn->is_varargs) {
        *n = RV_NPOOL - 8;
        return RV_POOL_VA;
    }
    *n = RV_NPOOL;
    return RV_POOL;
}

/* s0-s11: x8, x9 and x18-x27. */
static int rv_callee_saved(int r)
{
    return r == RV_FP || r == RV_S1 || (r >= RV_S2 && r <= RV_S2 + 9);
}

/* Is `dst = load(local)` a plain move here -- no extension emitted?
 *
 * Only at the full register width. It is tempting to say that a
 * four-byte SIGNED read at RV64 is plain too, since the ABI's invariant
 * is that a register holds the sign-extension of its 32-bit value -- but
 * a local that lives in a register is written by IR_STVAR, and a
 * NARROWING store there zero-extends (it cannot know the local's
 * signedness). The two rules have to agree, and the pair that agrees is
 * "narrow stores zero-extend, narrow loads always extend explicitly".
 * The other pair would read an unsigned local as a negative number. */
static int rv_ldvar_plain(int size, int sign, int w)
{
    int wb = target_ptr_size();
    (void)sign;
    return size == wb && w == wb;
}

/* Which instructions become a CALL the IR does not show as one. A value
 * live across one of these may not sit in a caller-saved register.
 *
 * On this target that is nearly all of floating point: there is no F and
 * no D extension under -march=rv32im/rv64im, so every arithmetic
 * operation on a float or a double is a libgcc call. Answering this
 * wrong is invisible until a float program is optimized, which is
 * exactly where the parked ARMv7-M attempt went wrong. */
static int rv_op_calls_helper(const struct ir_ins *i)
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
    return target_xlen() == 32 && (i->op == IR_DIV || i->op == IR_MOD) &&
           i->w == 8;
}

static const struct ra_target RISCV_RA = {
    rv_pool_for,
    rv_callee_saved,
    rv_ldvar_plain,
    /* The three capability flags stay 0 for now: this backend still
     * reads a call's arguments, a returned value and a memcpy's
     * addresses out of their stack slots, so the allocator must leave
     * them there. Turning each on is a change to the code that reads
     * them, and they go on ONE AT A TIME -- the parked ARMv7-M attempt
     * turned all three on in the same commit as the allocator itself
     * and had four bugs interacting with no way to tell them apart. */
    0, 0, 0,
    rv_op_calls_helper,
    0,            /* RISC-V is three-operand: d = a op b needs no copy */
    NULL,         /* ABI hints: a later increment */
    NULL, NULL    /* no FP class -- soft float lives in the core registers */
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
        case IR_I2F: case IR_F2I: case IR_F2F:
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
    if (is_struct && size > 2 * wb) {
        p->byref = 1;
        words = 1;                       /* just the pointer */
    }
    if (variadic && !p->byref && words == 2 && align >= 2 * wb) {
        *narg = (*narg + 1) & ~1;
        *stk = (*stk + 2 * wb - 1) & ~(long)(2 * wb - 1);
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

static long fn_sret_bytes(int wb, const struct ir_func *fn)
{
    return fn->ret_abi.is_struct ? sret_bytes(wb, fn->ret_abi.size) : 0;
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
        if (sret_bytes(F->w, i->retsize))
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
            if (a->is_struct && a->size > 2 * F->w)
                need = ((need + 15) & ~15L) + a->size;
        }
        if (need > most)
            most = need;
    }
    return (most + STACK_ALIGN - 1) & ~(long)(STACK_ALIGN - 1);
}

static void layout(struct rv_fn *F)
{
    struct ir_func *fn = F->fn;
    long off = outgoing_area(F);

    F->byref_at = off;
    off += byref_area(F);

    F->slot = xmalloc((size_t)(fn->nvregs ? fn->nvregs : 1) * sizeof *F->slot);
    for (int v = 0; v < fn->nvregs; v++)
        F->slot[v] = -1;

    for (int v = 0; v < fn->nvars; v++) {
        int size = fn->locals[v].size ? fn->locals[v].size : F->w;
        int align = fn->locals[v].user_align ? fn->locals[v].user_align
                  : fn->locals[v].align ? fn->locals[v].align : F->w;
        if (align < F->w) align = F->w;
        off = (off + align - 1) & ~(long)(align - 1);
        F->slot[v] = off;
        off += size;
    }
    for (int v = fn->nvars; v < fn->nvregs; v++) {
        int size = F->wide[v] ? 8 : F->w;
        off = (off + size - 1) & ~(long)(size - 1);
        F->slot[v] = off;
        off += size;
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
        long need = off + F->w + (long)F->nsave * F->w
                  + (fn->is_varargs ? (long)RV_NARGREG * F->w : 0);
        F->frame = (need + STACK_ALIGN - 1) & ~(long)(STACK_ALIGN - 1);
        if (fn->is_varargs) {
            F->va_regsave = F->frame - (long)RV_NARGREG * F->w;
            F->ra_slot = F->va_regsave - F->w;
        } else {
            F->va_regsave = -1;
            F->ra_slot = F->frame - F->w;
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
    rv_alu(F->t, RV_ADD, FAR, RV_SP, FAR, 0);
    return FAR;
}

static void ld_sp(struct rv_fn *F, int reg, long off, int size, int sign)
{
    if (rv_fits(off, 12)) {
        rv_load(F->t, reg, RV_SP, (int)off, size, sign, F->xlen);
        return;
    }
    rv_load(F->t, reg, sp_addr(F, off), 0, size, sign, F->xlen);
}

static void st_sp(struct rv_fn *F, int reg, long off, int size)
{
    if (rv_fits(off, 12)) {
        rv_store(F->t, reg, RV_SP, (int)off, size, F->xlen);
        return;
    }
    rv_store(F->t, reg, sp_addr(F, off), 0, size, F->xlen);
}

/* sp + off, into `reg`. */
static void addr_sp(struct rv_fn *F, int reg, long off)
{
    if (rv_fits(off, 12)) {
        rv_alu_imm(F->t, RV_ADD, reg, RV_SP, (int)off, 0);
        return;
    }
    rv_li(F->t, reg, off, F->xlen);
    rv_alu(F->t, RV_ADD, reg, RV_SP, reg, 0);
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
static void rd(struct rv_fn *F, int v, int reg)
{
    if (in_reg(F, v)) {
        if (F->loc[v] != reg)
            rv_mv(F->t, reg, F->loc[v]);
        return;
    }
    ld_sp(F, reg, F->slot[v], F->w, 1);
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
    st_sp(F, reg, F->slot[v], F->w);
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
    ld_sp(F, scratch, F->slot[v], F->w, 1);
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
    st_sp(F, reg, F->slot[v], F->w);
}

/* A 64-bit value's two halves at RV32, little-endian: the low word at the
 * slot and the high word four bytes above it. */
static void rd64(struct rv_fn *F, int v, int lo, int hi)
{
    ld_sp(F, lo, F->slot[v], 4, 1);
    ld_sp(F, hi, F->slot[v] + 4, 4, 1);
}

static void wr64(struct rv_fn *F, int v, int lo, int hi)
{
    if (F->slot[v] < 0)
        return;
    st_sp(F, lo, F->slot[v], 4);
    st_sp(F, hi, F->slot[v] + 4, 4);
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
static void want_label(struct rv_fn *F, int at, int label)
{
    if (F->nfix == F->capfix) {
        F->capfix = F->capfix ? F->capfix * 2 : 16;
        F->fix = xrealloc(F->fix, (size_t)F->capfix * sizeof *F->fix);
    }
    F->fix[F->nfix].at = at;
    F->fix[F->nfix].label = label;
    F->nfix++;
}

static void jump_to(struct rv_fn *F, int label)
{
    want_label(F, rv_j_placeholder(F->t, RV_ZERO), label);
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
    int at = rv_b_placeholder(F->t, invert_branch(cond), rs1, rs2);
    rv_patch_b(F->t, at, at + 8);           /* over the jump below */
    jump_to(F, label);
}

/* ---- site lists --------------------------------------------------------- */

static void note_call(struct rv_sites *st, int at, struct func *target)
{
    if (st->ncall == st->capcall) {
        st->capcall = st->capcall ? st->capcall * 2 : 16;
        st->call = xrealloc(st->call, (size_t)st->capcall * sizeof *st->call);
    }
    st->call[st->ncall].patch_off = at;
    st->call[st->ncall].target = target;
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
static void set_args(struct rv_fn *F, const int *dstreg, const int *vreg,
                     int n)
{
    int pd[RA_MAXPOOL], ps[RA_MAXPOOL], npm = 0;

    for (int k = 0; k < n; k++)
        if (in_reg(F, vreg[k])) {
            pd[npm] = dstreg[k];
            ps[npm] = F->loc[vreg[k]];
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
        if (!in_reg(F, vreg[k]))
            ld_sp(F, dstreg[k], F->slot[vreg[k]], F->w, 1);
}

/* Both operands of a two-argument helper. A pair at RV32 is never
 * allocated, so that case keeps the straightforward loads. */
static void fp_args2(struct rv_fn *F, const struct ir_ins *i)
{
    if (i->w == 8 && F->xlen == 32) {
        rd64(F, i->a, RV_A0, RV_A1);
        rd64(F, i->b, RV_A2, RV_A3);
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
static void cmp64(struct rv_fn *F, const struct ir_ins *i, enum binop pred,
                  int sign)
{
    struct code *t = F->t;
    int hi_ne, done;

    rd64(F, i->a, A_LO, A_HI);
    operand_b64(F, i, B_LO, B_HI);

    if (pred == B_EQ || pred == B_NE) {
        rv_alu(t, RV_XOR, SCR, A_LO, B_LO, 0);
        rv_alu(t, RV_XOR, SCR2, A_HI, B_HI, 0);
        rv_alu(t, RV_OR, SCR, SCR, SCR2, 0);
        if (pred == B_EQ) rv_alu_imm(t, RV_SLTU, ACC, SCR, 1, 0);
        else              rv_alu(t, RV_SLTU, ACC, RV_ZERO, SCR, 0);
        return;
    }
    hi_ne = rv_b_placeholder(t, RV_BNE, A_HI, B_HI);
    cmp_to_reg(F, pred, 0, A_LO, B_LO, SCR);   /* equal highs: unsigned lows */
    done = rv_j_placeholder(t, RV_ZERO);
    rv_patch_b(t, hi_ne, t->len);
    cmp_to_reg(F, pred, sign, A_HI, B_HI, SCR);
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
    case IR_CONST:
        rv_li(t, A_LO, (long long)(i->imm & 0xffffffffL), 32);
        rv_li(t, A_HI, (long long)((i->imm >> 32) & 0xffffffffL), 32);
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    case IR_MOV:
        rd64(F, i->a, A_LO, A_HI);
        wr64(F, i->dst, A_LO, A_HI);
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
        int op = i->op == IR_AND ? RV_AND : i->op == IR_OR ? RV_OR : RV_XOR;
        rd64(F, i->a, A_LO, A_HI);
        operand_b64(F, i, B_LO, B_HI);
        rv_alu(t, op, A_LO, A_LO, B_LO, 0);
        rv_alu(t, op, A_HI, A_HI, B_HI, 0);
        wr64(F, i->dst, A_LO, A_HI);
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
    case IR_NEG:
        rd64(F, i->a, A_LO, A_HI);
        rv_alu(t, RV_SLTU, SCR, RV_ZERO, A_LO, 0);   /* borrow out of 0-lo */
        rv_alu(t, RV_SUB, A_LO, RV_ZERO, A_LO, 0);
        rv_alu(t, RV_SUB, A_HI, RV_ZERO, A_HI, 0);
        rv_alu(t, RV_SUB, A_HI, A_HI, SCR, 0);
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    case IR_BNOT:
        rd64(F, i->a, A_LO, A_HI);
        rv_alu_imm(t, RV_XOR, A_LO, A_LO, -1, 0);
        rv_alu_imm(t, RV_XOR, A_HI, A_HI, -1, 0);
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    case IR_SHL: case IR_SHR: {
        int op = i->op == IR_SHL ? RV_SLL : RV_SRL;
        int sign = i->op == IR_SHR && i->sign;
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
    case IR_LDVAR:
        ld_sp(F, A_LO, F->slot[i->a], 4, 1);
        ld_sp(F, A_HI, F->slot[i->a] + 4, 4, 1);
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    case IR_STVAR:
        rd64(F, i->a, A_LO, A_HI);
        st_sp(F, A_LO, F->slot[i->dst], 4);
        st_sp(F, A_HI, F->slot[i->dst] + 4, 4);
        return 1;
    case IR_LOAD:
        rd(F, i->a, ADDR);
        rv_load(t, A_LO, ADDR, 0, 4, 1, F->xlen);
        rv_load(t, A_HI, ADDR, 4, 4, 1, F->xlen);
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    case IR_STORE:
        rd(F, i->a, ADDR);
        rd64(F, i->b, A_LO, A_HI);
        rv_store(t, A_LO, ADDR, 0, 4, F->xlen);
        rv_store(t, A_HI, ADDR, 4, 4, F->xlen);
        return 1;
    case IR_SELECT: {
        int take_c, done;
        rd(F, i->a, SCR);
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

static void gen_call(struct rv_fn *F, int n)
{
    struct ir_func *fn = F->fn;
    struct ir_ins *i = &fn->ins[n];
    struct code *t = F->t;
    struct argplace pl[MAX_PARAMS];
    int narg = 0;
    long stk = 0, copy_at = F->byref_at;
    long sret = sret_bytes(F->w, i->retsize);

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
        rd(F, a->vreg, TMP);
        for (long b = 0; b < a->size; b++) {
            rv_load(t, SCR, TMP, (int)b, 1, 0, F->xlen);
            st_sp(F, SCR, copy_at + b, 1);
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
            st_sp(F, SCR, pl[k].stk, F->w);
        } else if (a->is_struct) {
            rd(F, a->vreg, ADDR);
            for (int q = 0; q < pl[k].nstk; q++) {
                long off = (long)(pl[k].nreg + q) * F->w;
                long left = a->size - off;
                if (left >= F->w) {
                    rv_load(t, SCR, ADDR, (int)off, F->w, 0, F->xlen);
                    st_sp(F, SCR, pl[k].stk + (long)q * F->w, F->w);
                } else {
                    /* The tail of an odd-sized struct, byte by byte: a
                     * whole-word load would read past the object. */
                    for (long b = 0; b < left; b++) {
                        rv_load(t, SCR, ADDR, (int)(off + b), 1, 0, F->xlen);
                        st_sp(F, SCR, pl[k].stk + (long)q * F->w + b, 1);
                    }
                }
            }
        } else if (a->size > F->w) {
            rd64(F, a->vreg, SCR, SCR2);
            if (pl[k].nreg == 1) {
                /* THE SPLIT. With exactly one register left, a 2*XLEN
                 * scalar puts its LOW half there and its HIGH half at the
                 * bottom of the stack area -- so only the high half is
                 * written here. Confirmed against clang. */
                st_sp(F, SCR2, pl[k].stk, F->w);
            } else {
                st_sp(F, SCR, pl[k].stk, F->w);
                st_sp(F, SCR2, pl[k].stk + F->w, F->w);
            }
        } else {
            rd(F, a->vreg, SCR);
            st_sp(F, SCR, pl[k].stk, F->w);
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
            if (pl[k].nreg == 2)
                rd64(F, a->vreg, argreg(pl[k].reg), argreg(pl[k].reg + 1));
            else
                ld_sp(F, argreg(pl[k].reg), F->slot[a->vreg], 4, 1); /* low */
        } else {
            rd(F, a->vreg, argreg(pl[k].reg));
        }
    }
    /* The hidden result pointer goes in LAST, so nothing above can have
     * used a0 as a scratch after it was set. */
    if (sret)
        addr_sp(F, RV_A0, F->scratch_at + i->scratch);

    if (i->indirect) {
        /* The target is read BEFORE nothing -- the arguments are already
         * in place, and SCR is not one of them. */
        rd(F, i->a, SCR);
        rv_jalr(t, RV_RA, SCR, 0);
    } else if (i->callee->has_defn) {
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
    } else if (F->xlen == 32 && F->wide[i->dst]) {
        wr64(F, i->dst, RV_A0, RV_A1);
    } else {
        wr(F, i->dst, RV_A0);
    }
}

/* ---- one instruction ------------------------------------------------------ */

static void gen_ins(struct rv_fn *F, int n)
{
    struct ir_func *fn = F->fn;
    struct ir_ins *i = &fn->ins[n];
    struct code *t = F->t;
    int wordop;

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

    if (i->w > 8)
        rv_refuse(F, i, "a 128-bit value");

    /* Does this instruction work on a value that needs a register pair?
     * NOT `i->w == 8` everywhere: the width field is the OPERATION's, and
     * several instructions do not set it at all. IR_STVAR and IR_STORE
     * carry a `size` and no `w`, so asking `w` says four and stores half
     * of a long long; IR_RET carries neither. The wide map, built from
     * each value's defining instruction, is what knows. */
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
                rd64(F, i->a, RV_A0, RV_A1);
                operand_b64(F, i, RV_A2, RV_A3);
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
    case IR_CONST:
        rv_li(t, ACC, imm_val(F, i), F->xlen);
        wr(F, i->dst, ACC);
        return;
    case IR_MOV:
        rd(F, i->a, ACC);
        wr(F, i->dst, ACC);
        return;

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
    case IR_DIV: case IR_MOD:
        rd(F, i->a, ACC);
        operand_b(F, i, TMP);
        rv_muldiv(t, i->op == IR_DIV ? (i->sign ? RV_DIV : RV_DIVU)
                                     : (i->sign ? RV_REM : RV_REMU),
                  ACC, ACC, TMP, wordop);
        wr(F, i->dst, ACC);
        return;
    case IR_SHL: case IR_SHR: {
        int op = i->op == IR_SHL ? RV_SLL : i->sign ? RV_SRA : RV_SRL;
        int bits = wordop ? 32 : F->xlen;
        rd(F, i->a, ACC);
        if (i->imm_b && i->imm >= 0 && i->imm < bits) {
            rv_shift_imm(t, op, ACC, ACC, (int)i->imm, wordop, F->xlen);
        } else {
            operand_b(F, i, TMP);
            rv_alu(t, op, ACC, ACC, TMP, wordop);
        }
        wr(F, i->dst, ACC);
        return;
    }
    case IR_NEG:
        rd(F, i->a, ACC);
        rv_alu(t, RV_SUB, ACC, RV_ZERO, ACC, wordop);
        wr(F, i->dst, ACC);
        return;
    case IR_BNOT:
        rd(F, i->a, ACC);
        rv_alu_imm(t, RV_XOR, ACC, ACC, -1, 0);
        wr(F, i->dst, ACC);
        return;

    case IR_CMP:
        if (i->w == 8 && F->xlen == 32) {
            cmp64(F, i, i->pred, i->sign);
            wr(F, i->dst, ACC);
            return;
        }
        rd(F, i->a, ACC);
        operand_b(F, i, TMP);
        cmp_to_reg(F, i->pred, i->sign, ACC, TMP, ACC);
        wr(F, i->dst, ACC);
        return;

    case IR_SELECT: {
        /* dst = a ? b : c. Both arms are already-computed VALUES in
         * slots, so this is two loads and a branch over one of them. */
        int take_c, done;
        rd(F, i->a, SCR);
        take_c = rv_b_placeholder(t, RV_BEQ, SCR, RV_ZERO);
        rd(F, i->b, ACC);
        done = rv_j_placeholder(t, RV_ZERO);
        rv_patch_b(t, take_c, t->len);
        rd(F, i->c, ACC);
        rv_patch_j(t, done, t->len);
        wr(F, i->dst, ACC);
        return;
    }

    case IR_BRZ: case IR_BRNZ:
        if (i->w == 8 && F->xlen == 32) {
            rd64(F, i->a, A_LO, A_HI);
            rv_alu(t, RV_OR, A_LO, A_LO, A_HI, 0);
        } else {
            rd(F, i->a, A_LO);
        }
        branch_if(F, i->op == IR_BRZ ? RV_BEQ : RV_BNE, A_LO, RV_ZERO,
                  i->label);
        return;

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
            ld_sp(F, d, F->slot[i->a], i->size, i->sign);
        }
        wrote(F, i->dst, d);
        return;
    }
    case IR_STVAR: {
        int src = rdr(F, i->a, ACC);
        if (in_reg(F, i->dst)) {
            /* A narrowing store ZERO-extends: the register now holds
             * the whole local, and only its low `size` bytes are the
             * value. rv_ldvar_plain is written to match. */
            if (i->size >= F->w) {
                if (F->loc[i->dst] != src) rv_mv(t, F->loc[i->dst], src);
            } else {
                ext_reg(F, F->loc[i->dst], src, i->size, 0);
            }
        } else {
            st_sp(F, src, F->slot[i->dst], i->size);
        }
        return;
    }
    case IR_LOAD:
        rd(F, i->a, ADDR);
        rv_load(t, ACC, ADDR, 0, i->size, i->sign, F->xlen);
        wr(F, i->dst, ACC);
        return;
    case IR_STORE:
        rd(F, i->a, ADDR);
        rd(F, i->b, ACC);
        rv_store(t, ACC, ADDR, 0, i->size, F->xlen);
        return;
    case IR_EXT:
        rd(F, i->a, ACC);
        ext_reg(F, ACC, ACC, i->size, i->sign);
        wr(F, i->dst, ACC);
        return;

    case IR_ADDR:
        addr_sp(F, ACC, F->slot[i->a]);
        wr(F, i->dst, ACC);
        return;
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
        note_str(F->st, t->len, i->label, RK_RISCV_PCREL_HI20);
        rv_auipc(t, ACC, 0);
        note_str(F->st, t->len, i->label, RK_RISCV_PCREL_LO12_I);
        rv_alu_imm(t, RV_ADD, ACC, ACC, 0, 0);
        wr(F, i->dst, ACC);
        return;
    case IR_GADDR:
        note_glob(F->st, t->len, i->glob, RK_RISCV_PCREL_HI20);
        rv_auipc(t, ACC, 0);
        note_glob(F->st, t->len, i->glob, RK_RISCV_PCREL_LO12_I);
        rv_alu_imm(t, RV_ADD, ACC, ACC, 0, 0);
        wr(F, i->dst, ACC);
        return;
    case IR_FADDR:
        note_fn(F->st, t->len, i->callee, RK_RISCV_PCREL_HI20);
        rv_auipc(t, ACC, 0);
        note_fn(F->st, t->len, i->callee, RK_RISCV_PCREL_LO12_I);
        rv_alu_imm(t, RV_ADD, ACC, ACC, 0, 0);
        wr(F, i->dst, ACC);
        return;

    case IR_MEMCPY: case IR_MEMZERO: {
        /* Straight-line, unrolled to registers where the size allows.
         * Small and obviously right; a tuned copy is a later question. */
        long size = i->size, k;
        int step = F->w;
        rd(F, i->a, ADDR);
        if (i->op == IR_MEMCPY) {
            rd(F, i->b, TMP);
            for (k = 0; k + step <= size; k += step) {
                rv_load(t, SCR, TMP, (int)k, step, 0, F->xlen);
                rv_store(t, SCR, ADDR, (int)k, step, F->xlen);
            }
            for (; k < size; k++) {
                rv_load(t, SCR, TMP, (int)k, 1, 0, F->xlen);
                rv_store(t, SCR, ADDR, (int)k, 1, F->xlen);
            }
        } else {
            for (k = 0; k + step <= size; k += step)
                rv_store(t, RV_ZERO, ADDR, (int)k, step, F->xlen);
            for (; k < size; k++)
                rv_store(t, RV_ZERO, ADDR, (int)k, 1, F->xlen);
        }
        return;
    }

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
                    rd(F, i->a, TMP);
                    ld_sp(F, ADDR, F->sret_slot, F->w, 1);
                    for (long k = 0; k < size; k++) {
                        rv_load(t, SCR, TMP, (int)k, 1, 0, F->xlen);
                        rv_store(t, SCR, ADDR, (int)k, 1, F->xlen);
                    }
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
            } else if (F->xlen == 32 && F->wide[i->a]) {
                rd64(F, i->a, RV_A0, RV_A1);
            } else {
                rd(F, i->a, RV_A0);
            }
        }
        jump_to(F, fn->nlabels);           /* the epilogue */
        return;

    case IR_UD2:
        /* The guaranteed-illegal instruction. A load from address zero is
         * NOT a substitute: on a board with memory or a trap handler
         * there it simply succeeds, and an unreachable path becomes a
         * silent fallthrough -- which is what happened on Cortex-M. */
        rv_unimp(t);
        return;
    case IR_FENCE:
        /* Nothing to order: one hart, no A extension, no cache the ISA
         * exposes. `fence` would be correct too; this says why it is not
         * needed rather than leaving a reader to wonder. */
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

        if (i->op == IR_I2F && src_w == 8 && i->a >= 0 && !F->wide[i->a]) {
            /* irgen converts an `unsigned int` by asking for a SIGNED
             * 64-bit conversion of it, on the grounds that "a 32-bit
             * operation zero-extends its result into the eight-byte
             * slot". That is true of a register write on x86-64 and
             * aarch64 and false here: at RV32 the slot is four bytes and
             * the next four are another temporary, and at RV64 a slot
             * load SIGN-extends. So the zero extension is done
             * explicitly, which is what that comment meant all along --
             * without it (float)(unsigned)k came back as a constant
             * 4.7e18 whatever k was. */
            rd(F, i->a, RV_A0);
            if (F->xlen == 32) rv_mv(t, RV_A1, RV_ZERO);
            else               ext_reg(F, RV_A0, RV_A0, 4, 0);
        } else if (src_w == 8 && F->xlen == 32) {
            rd64(F, i->a, RV_A0, RV_A1);
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

    case IR_ASM:
        rv_refuse(F, i, "inline assembly");
        return;
    case IR_XCHG: case IR_XADD: case IR_CMPXCHG: case IR_ARMW: case IR_CAS:
        rv_refuse(F, i, "an atomic operation (this configuration has no A "
                        "extension)");
        return;
    case IR_LABELADDR: case IR_IGOTO:
        rv_refuse(F, i, "a computed goto");
        return;
    default:
        rv_refuse(F, i, "this operation");
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

/* ---- one function --------------------------------------------------------- */

static void gen_func(struct ir_func *fn, struct code *t, struct rv_sites *st,
                     int xlen)
{
    struct func *f = fn->src;
    struct rv_fn F;
    int i;

    F.fn = fn; F.t = t; F.st = st;
    F.xlen = xlen; F.w = xlen / 8;
    F.fix = NULL; F.nfix = F.capfix = 0;
    F.wide = wide_map(fn);
    F.loc = NULL; F.nsave = 0;
    if (g_rv_regalloc) {
        /* `wide` excludes the values needing a REGISTER PAIR, which at
         * RV32 is every eight-byte one: the allocator hands out one
         * register and a pair is not one. At RV64 that map is empty and
         * everything is eligible -- which is why the win is larger
         * there, and RV64 is where the slot-based code was worst.
         *
         * fltmap is NULL, not cg_float_vregs: this target has no
         * floating-point register class, so a float lives in an
         * ordinary integer register and must stay ELIGIBLE for the
         * integer pool. Passing the map would exclude every float from
         * both classes and leave it with nowhere to live. */
        F.loc = ra_allocate(fn, &RISCV_RA, F.wide, NULL,
                            F.used_callee, &F.nsave);
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
    layout(&F);

    /* One more label than the IR has: the epilogue, which every IR_RET
     * jumps to so the frame size is written down once. */
    F.label_off = xmalloc((size_t)(fn->nlabels + 1) * sizeof *F.label_off);
    for (i = 0; i <= fn->nlabels; i++)
        F.label_off[i] = -1;

    while (t->len & 3)
        rv_unimp(t);      /* alignment padding that traps if ever reached */
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
    st_sp(&F, RV_RA, F.ra_slot, F.w);
    for (i = 0; i < F.nsave; i++)
        st_sp(&F, F.used_callee[i], F.save_at + (long)i * F.w, F.w);

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
                if (pl.nreg) rv_mv(t, ADDR, param_reg(&F, &pl, 0));
                else         ld_sp(&F, ADDR, base + pl.stk, F.w, 1);
                for (long b = 0; b < a->size; b++) {
                    rv_load(t, SCR2, ADDR, (int)b, 1, 0, xlen);
                    st_sp(&F, SCR2, F.slot[i] + b, 1);
                }
                continue;
            }
            /* A SCALAR occupies whole registers and a whole slot: store
             * the register. Only a COMPOSITE has a partial last word, and
             * only it may be written byte by byte -- doing that to a
             * scalar stores one byte of it and leaves the rest of the
             * slot holding whatever the frame had. */
            if (!a->is_struct) {
                if (a->size > F.w) {
                    /* RV32's register pair: two words, low first. A
                     * pair is never allocated (the `wide` map excludes
                     * it), so both halves go to the slot. */
                    for (int q = 0; q < pl.nreg; q++)
                        st_sp(&F, param_reg(&F, &pl, q),
                              F.slot[i] + (long)q * F.w, F.w);
                    for (int q = 0; q < pl.nstk; q++) {
                        ld_sp(&F, SCR, base + pl.stk + (long)q * F.w, F.w, 1);
                        st_sp(&F, SCR,
                              F.slot[i] + (long)(pl.nreg + q) * F.w, F.w);
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
                    st_sp(&F, param_reg(&F, &pl, 0), F.slot[i], F.w);
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
                    st_sp(&F, SCR, F.slot[i], F.w);
                }
                continue;
            }
            for (int q = 0; q < pl.nreg; q++) {
                long off = F.slot[i] + (long)q * F.w;
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
                long dst = F.slot[i] + (long)(pl.nreg + q) * F.w;
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

    for (i = 0; i < fn->nins; i++)
        gen_ins(&F, i);

    /* The epilogue. */
    F.label_off[fn->nlabels] = t->len;
    for (i = 0; i < F.nsave; i++)
        ld_sp(&F, F.used_callee[i], F.save_at + (long)i * F.w, F.w, 1);
    ld_sp(&F, RV_RA, F.ra_slot, F.w, 1);
    if (F.frame) {
        if (rv_fits(F.frame, 12)) {
            rv_alu_imm(t, RV_ADD, RV_SP, RV_SP, (int)F.frame, 0);
        } else {
            rv_li(t, RV_T0, F.frame, xlen);
            rv_alu(t, RV_ADD, RV_SP, RV_SP, RV_T0, 0);
        }
    }
    rv_ret(t);

    for (i = 0; i < F.nfix; i++) {
        int target = F.label_off[F.fix[i].label];
        if (target < 0)
            internal_error("riscv: label %d of %s was never placed",
                           F.fix[i].label, fn->name);
        rv_patch_j(t, F.fix[i].at, target);
    }

    f->code_len = t->len - f->code_off;
    f->stack_bytes = (int)F.frame;     /* what -fstack-usage reports */
    free(F.slot);
    free(F.label_off);
    free(F.fix);
    free(F.wide);
    free(F.loc);
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
    g_rv_regalloc = regalloc;
    if (want_debug) {
        fprintf(stderr, "embcc: error: -g is not supported for RISC-V yet "
                        "(the DWARF frame description would be a guess)\n");
        exit(1);
    }
    memset(&st, 0, sizeof st);

    for (int n = 0; n < iu->nfuncs; n++)
        gen_func(&iu->funcs[n], text, &st, xlen);

    /* Intra-unit calls, now that every function has a place. The auipc
     * and the jalr are patched together: the auipc adds the HI20 of the
     * displacement to pc and the jalr adds the sign-extended LO12, so the
     * HI20 has to be rounded up when bit 11 is set -- the same +0x800 as
     * everywhere else in this target. */
    for (int k = 0; k < st.ncall; k++) {
        int at = st.call[k].patch_off;
        long disp = st.call[k].target->code_off - at;
        long hi = ((disp + 0x800) >> 12) & 0xfffff;
        int lo = (int)(((disp & 0xfff) ^ 0x800) - 0x800);
        code_patch32(text, at, rv_enc_u(0x17, RV_RA, hi));
        code_patch32(text, at + 4, rv_enc_i(0x67, RV_RA, 0, RV_RA, lo));
    }
    free(st.call);

    /* A string site recorded the string's INDEX (that is what IR_STRADDR
     * carries); the driver wants its OFFSET in .rodata. Resolved here,
     * where the unit's string table exists, exactly as the other three
     * backends do -- leaving it out made every literal after the first
     * resolve to a few bytes into the one before it, and `puts_("sum")`
     * printed the tail of "hello". */
    for (int n = 0; n < st.nstr; n++)
        st.str[n].str_off = iu->strs[st.str[n].str_off].off;

    *ext = st.ext;   *next = st.next;
    *strs = st.str;  *nstrs = st.nstr;
    *gs = st.g;      *ngs = st.ng;
    *fs = st.f;      *nfs = st.nf;
}
