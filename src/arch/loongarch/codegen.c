/* LoongArch64 code generation, LP64S (soft float).
 *
 * This file began as a COPY of RV64's code generator (src/arch/riscv/
 * codegen.c), because LoongArch's base integer ISA and its LP64 calling
 * convention are RISC-V's rule for rule -- the argument registers and the
 * pair rules, the packed small aggregates and the by-reference large
 * ones, the sign-extended 32-bit values, the soft-float helpers. What
 * differs is selection, and docs/internals/loongarch64-plan.md lists it:
 * immediates that are UNSIGNED on andi/ori/xori, constants built by
 * lu12i.w/ori/lu32i.d/lu52i.d, 32-bit divides that demand sign-extended
 * operands, branches that reach 32 times further, `bl` for every call,
 * pcalau12i/addi.d for every address, and sc writing 1 on success. It is
 * a copy and not a shared file so that nothing here can move RISC-V.
 *
 * ---- the shape of the lowering ---------------------------------------
 *
 * Every vreg has ONE home: the register the shared allocator gave it
 * (-O0 included, for each expression's temporaries) or a frame slot.
 * Each operation reads its operands where they are (rdr), computes into
 * the destination's register or a scratch (wreg) and commits it
 * (wrote). A 32-bit operation leaves its result sign-extended, the
 * psABI's invariant, and a reader that needs all 64 bits asks rd32.
 *
 * ---- what this file refuses -------------------------------------------
 *
 * By name, with the IR operation printed: inline assembly, computed goto,
 * one- and two-byte atomics. THE RULE -- an object full of plausible
 * instructions that implement something else is worse than no object.
 */
#include "emit.h"

#include "../backend.h"
#include "../regalloc.h"
#include "../target.h"
#include "../../driver/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct la_fn;
static void copy_block(struct la_fn *F, int copy, long size, int step);
static void copy_block_at(struct la_fn *F, int copy, long size, int step,
                          int sreg, long soff, int dreg, long doff);

/* The scratch registers, never a value's home: t0, t1, t2, t4, t5, t6.
 * A_LO/A_HI and B_LO with W_HI are a 128-bit value's two words. */
#define A_LO LA_T0
#define A_HI LA_T1
#define B_LO LA_T2
#define ACC  LA_T0      /* the value being computed */
#define TMP  LA_T1      /* the second operand */
#define ADDR LA_T2      /* an address */
#define SCR  LA_T4      /* a fourth, for when the other three are taken */
#define SCR2 LA_T5
/* Reserved for ONE job: holding sp + a large offset, and nothing else.
 * It has to be a register no value ever lands in -- the RISC-V backend
 * once picked "whichever scratch is free" and destroyed a value it had
 * just read in a function with a frame deeper than 2047 bytes. */
#define FAR  LA_T6

struct la_sites {
    struct { int patch_off; struct func *target; int tail; } *call;
    int ncall, capcall;
    struct extcall *ext;   int next, capext;
    struct strsite *str;   int nstr, capstr;
    struct gsite *g;       int ng, capg;
    struct fsite *f;       int nf, capf;
};

struct la_fn {
    /* Comparison/branch fusion: read counts per vreg, so a comparison
     * whose only reader is the branch after it becomes ONE branch
     * instruction (LoongArch branches compare two registers, as RISC-V's
     * do). skip_next tells the dispatch loop the branch is already out. */
    int *usecnt;
    int skip_next;
    int want_debug;
    struct ir_func *fn;
    /* Per vreg: the register the allocator gave it, or -1 for one that
     * stays in memory. NULL when the allocator did not run. */
    int *loc;
    int used_callee[RA_MAXPOOL];  /* the callee-saved ones it took */
    int nsave;
    struct code *t;
    struct la_sites *st;
    int w;               /* a register in bytes: 8 */
    char *wide;          /* per vreg: an eight-byte value (wide_map) */
    char *w16;           /* per vreg: an __int128 or a long double, in a
                          * sixteen-byte slot (la_w16_map) */
    char *sx;            /* per vreg: already the sign-extension of its low
                          * 32 bits (sext_map) */
    long *slot;          /* per-vreg byte offset from sp, -1 for none */
    long frame;          /* total bytes sp moves down by */
    long scratch_at;     /* where fn->scratch_bytes begins */
    long byref_at;       /* where the by-reference argument copies go */
    long sret_slot;      /* where the hidden result pointer is kept, or -1 */
    long ra_slot;        /* where the return address is saved */
    /* The register every frame slot is addressed from: sp, except in a
     * function with a variable-length array, where sp moves at run time
     * and fp holds the frame base (see IR_ALLOCA). */
    int fb;
    long out_bytes;      /* the outgoing-argument area, at the live sp */
    /* Makes no call -- none in the IR and none to a runtime helper -- so
     * ra is never overwritten and needs no slot, save or restore. */
    int leaf;
    /* Per instruction: an IR_CALL made as a TAIL call (la_tail_ok). */
    char *tail;
    long save_at;        /* ... and the allocator's callee-saved ones */
    long va_regsave;     /* a variadic function's a0-a7 spill area, or -1 */
    long va_first;       /* ... and the offset of the first UNNAMED one */
    int *label_off;      /* per label id, or -1 while unseen */
    /* A jump or branch to a label. `kind` is its form (FX_*); for the
     * long form `bat` is where its branch-over begins. */
    struct { int at; int label; int kind; int cond, rs1, rs2; int bat; } *fix;
    /* Per jump or branch, in emission order, the form to emit: NULL on the
     * first pass, which tries every one short (FX_B); a pass after a
     * branch that did not reach makes that one FX_LONG. */
    signed char *relax;
    int nrelax;
    int nfix, capfix;
};

/* ---- the register allocator's view of this machine ---------------------
 *
 * a0-a7 and the temporaries the scratch set leaves (t3, t7, t8), then
 * the callee-saved fp and s0-s8: 21, as on RV64. Caller-saved FIRST in
 * the preference order, which is what regalloc.h asks for: a short-lived
 * value takes one and the prologue never has to save it. r21 is the
 * psABI's and appears nowhere.
 */
#define LA_NPOOL 21
static const int LA_POOL[LA_NPOOL] = {
    LA_A0, LA_A1, LA_A2, LA_A3, LA_A4, LA_A5, LA_A6, LA_A7,
    LA_T3, LA_T7, LA_T8,
    LA_FP, LA_S0, LA_S1, LA_S2, LA_S3, LA_S4, LA_S5, LA_S6, LA_S7, LA_S8
};
/* The same list with the argument file removed, for a variadic function:
 * its prologue spills a0-a7 into the register save area and `va_arg`
 * walks them, so those eight are not the allocator's to give. */
static const int LA_POOL_VA[LA_NPOOL - 8] = {
    LA_T3, LA_T7, LA_T8,
    LA_FP, LA_S0, LA_S1, LA_S2, LA_S3, LA_S4, LA_S5, LA_S6, LA_S7, LA_S8
};
static int g_la_pool[LA_NPOOL];

static const int *la_pool_for(const struct ir_func *fn, int *n)
{
    const int *p = fn->is_varargs ? LA_POOL_VA : LA_POOL;
    int np = fn->is_varargs ? LA_NPOOL - 8 : LA_NPOOL, k = 0;
    /* A VLA's function addresses its frame from fp (IR_ALLOCA). */
    if (!fn->has_alloca) {
        *n = np;
        return p;
    }
    for (int j = 0; j < np; j++)
        if (p[j] != LA_FP)
            g_la_pool[k++] = p[j];
    *n = k;
    return g_la_pool;
}

/* fp (s9) and s0-s8: r22-r31. */
static int la_callee_saved(int r)
{
    return r >= LA_FP && r <= LA_S8;
}

/* Is `dst = load(local)` a plain move here -- no extension emitted?
 *
 * At the full register width, always; and a four-byte SIGNED read at
 * four-byte width, which is most of what integer code does. That second
 * one is only sound because IR_STVAR below SIGN-extends a four-byte
 * store -- the two rules agree about what a register holding a narrow
 * local contains. An unsigned read still extends for itself, so the
 * choice decides only which reads are free, never which are correct. */
static int la_ldvar_plain(int size, int sign, int w)
{
    if (size == 8 && w == 8)
        return 1;
    return size == 4 && sign && w == 4;
}

/* Which instructions become a CALL the IR does not show as one. A value
 * live across one of these may not sit in a caller-saved register.
 *
 * Under LP64S that is all of floating-point arithmetic, every conversion
 * involving a float, and the 128-bit divides and variable shifts (lib/rt,
 * libgcc's names). A 64-bit divide is one instruction. */
int la_op_calls_helper(const struct ir_ins *i)
{
    if (i->flt)
        return i->op == IR_ADD || i->op == IR_SUB || i->op == IR_MUL ||
               i->op == IR_DIV || i->op == IR_CMP;
    if (i->op == IR_I2F || i->op == IR_F2I || i->op == IR_F2F)
        return 1;
    /* gen_ins128 does a constant shift count inline, which this does not
     * look for: answering yes where no call is made costs a register,
     * answering no where one is made costs a value. */
    return i->w == 16 && !i->flt &&
           (i->op == IR_DIV || i->op == IR_MOD || i->op == IR_SHL ||
            i->op == IR_SHR);
}

/* Where the psABI would put each value (below place_arg, whose answer
 * it uses). */
static void la_abi_hints(const struct ir_func *fn, int *hint);

static const struct ra_target LOONGARCH_RA = {
    la_pool_for,
    la_callee_saved,
    la_ldvar_plain,
    /* A scalar call argument, a returned value and a memcpy's addresses
     * may come from a register: gen_call's setup is a parallel move. */
    1, 1, 1,
    la_op_calls_helper,
    0,            /* three-operand: d = a op b needs no copy */
    la_abi_hints,
    NULL, NULL,   /* no FP class -- soft float lives in the core registers */
    1,            /* ...and so is allocated with them (float_in_gpr) */
    NULL, NULL,
    1,            /* atomic_in_reg: every atomic reads its address and
                   * values through rdr and writes through wreg/wr */
    0,            /* fp_reads_gpr: floats are already general (above) */
    0             /* asm_in_reg: inline asm is refused (IR_ASM) */
};

/* -O1 and up: the allocator is on. */
static int g_la_regalloc;
/* -O0: the allocator runs for the temporaries of each expression only,
 * every source variable pinned to its slot as under -g. */
static int g_la_o0;

/* ---- refusal ---------------------------------------------------------- */

static void la_refuse(const struct la_fn *F, const struct ir_ins *i,
                      const char *what)
{
    char op[64];
    op[0] = 0;
    if (i)
        snprintf(op, sizeof op, " [%s w=%d size=%d]", ir_opname(i->op),
                 i->w, i->size);
    fprintf(stderr,
            "embcc: %s:%d: error: the LoongArch64 backend cannot lower %s "
            "yet (function %s)%s\n",
            F->fn->file ? F->fn->file : "?",
            i ? i->line : F->fn->line, what, F->fn->name, op);
    exit(1);
}

/* ---- which values are eight bytes wide ---------------------------------
 *
 * One register here, and the map is read only by the conversions --
 * which have to tell a genuinely 64-bit source from a 32-bit one irgen
 * asked to be widened, and cannot do that from the instruction alone.
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

/* The vregs holding a sixteen-byte value --  an __int128 or a long
 * double: cg_wide_vregs, the map the x86-64 and AArch64 backends share,
 * less any LOCAL that is not itself sixteen bytes. That map closes over
 * ldvar and stvar in both directions, so `long y = (long)x;` marks y --
 * whose slot is laid out from its declared size, eight bytes, which a
 * sixteen-byte copy would overrun. A narrowing stvar stores its `size`
 * and needs no mark; the temps such a local is read into stay marked and
 * only waste eight bytes of slot. */
static char *la_w16_map(struct ir_func *fn)
{
    char *w = cg_wide_vregs(fn);
    if (!w)
        return NULL;
    for (int v = 0; v < fn->nvars && v < fn->nvregs; v++)
        if (fn->locals[v].size != 16)
            w[v] = 0;
    return w;
}

static int is16(const struct la_fn *F, int v)
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
    if (*narg >= LA_NARGREG) {
        int a = p->byref || align < wb ? wb : align > 16 ? 16 : align;
        *stk = (*stk + a - 1) & ~(long)(a - 1);
    }
    p->reg = *narg;
    p->nreg = *narg < LA_NARGREG
            ? (words < LA_NARGREG - *narg ? words : LA_NARGREG - *narg) : 0;
    p->nstk = words - p->nreg;
    p->stk = *stk;
    *narg += p->nreg;
    if (p->nstk) {
        *narg = LA_NARGREG;              /* nothing back-fills past a split */
        *stk += (long)p->nstk * wb;
    }
}

/* place_arg numbers the argument registers 0..7; a0 is x10. On ARM the
 * two coincided (r0 is register 0) and the Thumb backend could use the
 * index directly -- here that would load a1 from x1, which is `ra`, and
 * the first thing a function did was read its own return address as its
 * second parameter. */
static int argreg(int n) { return la_argreg[n]; }

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
static void la_abi_hints(const struct ir_func *fn, int *hint)
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
            hint[i->a] = LA_A0;
        /* A soft-float helper the lowering calls (fp_args2/fp_result):
         * its operands go in a0 and a1 and its result comes back in a0.
         * Hinted there, a chain of float operations passes each result
         * straight on as the next one's argument; without, every link
         * was `mv a1,a0; mv a0,a1`. Not over a hint already given. */
        if (i->op != IR_CALL && la_op_calls_helper(i) && i->w <= wb) {
            if (i->a >= 0 && i->a < fn->nvregs && hint[i->a] < 0)
                hint[i->a] = LA_A0;
            if (!i->imm_b && i->b >= 0 && i->b < fn->nvregs &&
                hint[i->b] < 0)
                hint[i->b] = LA_A1;
            if (i->dst >= 0 && i->dst < fn->nvregs && hint[i->dst] < 0)
                hint[i->dst] = LA_A0;
        }
        if (i->op != IR_CALL)
            continue;
        if (!i->retsize && i->dst >= 0 && i->dst < fn->nvregs && i->w <= wb)
            hint[i->dst] = LA_A0;
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

static long outgoing_area(const struct la_fn *F)
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
static long byref_area(const struct la_fn *F)
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

static int in_reg(const struct la_fn *F, int v);

static void layout(struct la_fn *F)
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
     * sp: a load or store reaches a 12-bit offset, and past 2047 every
     * access is built with lu12i.w/ori/add.d first.
     *
     * Temporaries share a pool (ra_coalesce_temps, as the other backends
     * use): two whose live ranges do not overlap take one slot; a 128-bit
     * one keeps a sixteen-byte slot of its own. Then locals, small ones
     * first; one nothing names needs none (ra_locals_referenced), nor one
     * in a register (ra_slot_dead; under -g every local keeps its slot). */
    {
        int nv = fn->nvregs, npool = 0, has_cgoto = 0;
        int *loc2 = xmalloc((size_t)(nv ? nv : 1) * sizeof *loc2);
        for (int v = 0; v < nv; v++)
            loc2[v] = in_reg(F, v) || is16(F, v) ? 0 : -1;
        for (int n = 0; n < fn->nins; n++)
            if (fn->ins[n].op == IR_IGOTO || fn->ins[n].op == IR_LABELADDR)
                has_cgoto = 1;
        {
            struct ra_slots so = { loc2, NULL, g_la_regalloc, has_cgoto };
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
                  + (fn->is_varargs ? (long)LA_NARGREG * F->w : 0);
        F->frame = (need + STACK_ALIGN - 1) & ~(long)(STACK_ALIGN - 1);
        if (fn->is_varargs) {
            F->va_regsave = F->frame - (long)LA_NARGREG * F->w;
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
static int sp_addr(struct la_fn *F, long off)
{
    la_li(F->t, FAR, off);
    la_alu(F->t, LA_ADD, FAR, F->fb, FAR, 0);
    return FAR;
}

static void ld_sp(struct la_fn *F, int reg, long off, int size, int sign)
{
    if (la_fits(off, 12)) {
        la_load(F->t, reg, F->fb, (int)off, size, sign);
        return;
    }
    la_load(F->t, reg, sp_addr(F, off), 0, size, sign);
}

static void st_sp(struct la_fn *F, int reg, long off, int size)
{
    if (la_fits(off, 12)) {
        la_store(F->t, reg, F->fb, (int)off, size);
        return;
    }
    la_store(F->t, reg, sp_addr(F, off), 0, size);
}

/* A store into the OUTGOING argument area, which is always at the live
 * sp -- the callee finds its stack arguments at its own entry sp, and
 * after a VLA that is not the frame base. */
static void st_out(struct la_fn *F, int reg, long off, int size)
{
    if (la_fits(off, 12)) {
        la_store(F->t, reg, LA_SP, (int)off, size);
        return;
    }
    la_li(F->t, FAR, off);
    la_alu(F->t, LA_ADD, FAR, LA_SP, FAR, 0);
    la_store(F->t, reg, FAR, 0, size);
}

/* sp + off, into `reg`. */
static void addr_sp(struct la_fn *F, int reg, long off)
{
    if (la_fits(off, 12)) {
        la_alu_imm(F->t, LA_ADD, reg, F->fb, (int)off, 0);
        return;
    }
    la_li(F->t, reg, off);
    la_alu(F->t, LA_ADD, reg, F->fb, reg, 0);
}

/* Does the allocator have this vreg in a register? */
static int in_reg(const struct la_fn *F, int v)
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
static long sslot(const struct la_fn *F, int v)
{
    if (v < 0 || v >= F->fn->nvregs || F->slot[v] < 0)
        internal_error("loongarch: %s: a path addresses vreg %d's slot, and it "
                       "has none", F->fn->name, v);
    return F->slot[v];
}

static void rd(struct la_fn *F, int v, int reg)
{
    if (in_reg(F, v)) {
        if (F->loc[v] != reg)
            la_mv(F->t, reg, F->loc[v]);
        return;
    }
    ld_sp(F, reg, sslot(F, v), F->w, 1);
}

static void wr(struct la_fn *F, int v, int reg)
{
    if (in_reg(F, v)) {
        if (F->loc[v] != reg)
            la_mv(F->t, F->loc[v], reg);
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
static int rdr(struct la_fn *F, int v, int scratch)
{
    if (in_reg(F, v))
        return F->loc[v];
    ld_sp(F, scratch, sslot(F, v), F->w, 1);
    return scratch;
}

static int wreg(struct la_fn *F, int v, int scratch)
{
    return in_reg(F, v) ? F->loc[v] : scratch;
}

static void wrote(struct la_fn *F, int v, int reg)
{
    if (in_reg(F, v)) {
        if (F->loc[v] != reg)
            la_mv(F->t, F->loc[v], reg);
        return;
    }
    if (F->slot[v] < 0)
        return;
    st_sp(F, reg, sslot(F, v), F->w);
}

/* A sixteen-byte value's two doublewords (gen_ins128). */
static void ld128(struct la_fn *F, int v, int lo, int hi);
static void st128(struct la_fn *F, int v, int lo, int hi);
static void need16(const struct la_fn *F, int v);

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
 * an operation narrower than a register wants only its low half: narrowed
 * to 32 bits and sign-extended, which is exactly the invariant a 32-bit
 * value obeys in a 64-bit register. */
static long long imm_val(const struct la_fn *F, const struct ir_ins *i)
{
    (void)F;
    if (i->w == 4)
        return (long long)(int)(unsigned int)(unsigned long)i->imm;
    return (long long)i->imm;
}

/* ---- 32-bit values in 64-bit registers ---------------------------------------------
 *
 * The psABI's invariant is that a register holding a 32-bit value holds
 * its SIGN-EXTENSION, signed or not: 0xffffffffu is all ones. The `.w`
 * instructions keep it and `ld.w` establishes it, so arithmetic is free --
 * but the IR narrows for nothing (`(int)some_long` is the same temp, read
 * at width 4), `ld.wu` and a zero-extending local read break it, and
 * div.w/mod.w are undefined without it, and the
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
static int sext_def(const struct la_fn *F, const struct ir_ins *i,
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
    case IR_BNOT:                             /* nor rd, rj, zero */
        return SX(i->a);
    case IR_CMP:
        return 1;                             /* 0 or 1 */
    case IR_LOAD:
        return i->size < 4 || (i->size == 4 && i->sign);
    case IR_LDVAR:
        /* A four-byte local in a register is read with a plain move
         * (la_ldvar_plain): it holds what STVAR's sign extension, or the
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

static char *sext_map(const struct la_fn *F)
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
static int sext32(struct la_fn *F, int v, int r, int scratch)
{
    if (F->sx && v >= 0 && v < F->fn->nvregs && !F->sx[v]) {
        la_alu_imm(F->t, LA_ADD, scratch, r, 0, 1);       /* sext.w */
        return scratch;
    }
    return r;
}

static int rd32(struct la_fn *F, int v, int scratch)
{
    return sext32(F, v, rdr(F, v, scratch), scratch);
}

static void operand_b(struct la_fn *F, const struct ir_ins *i, int reg)
{
    if (i->imm_b)
        la_li(F->t, reg, imm_val(F, i));
    else
        rd(F, i->b, reg);
}

/* Sign- or zero-extend the low `size` bytes of `rs` to the full register:
 * one instruction each, andi 255 / bstrpick.d 15,0 / bstrpick.d 31,0 for
 * the unsigned, ext.w.b / ext.w.h / addi.w 0 for the signed. */
static void ext_reg(struct la_fn *F, int rdst, int rs, int size, int sign)
{
    if (size >= F->w) {
        if (rdst != rs)
            la_mv(F->t, rdst, rs);
        return;
    }
    if (sign && size == 4)
        la_alu_imm(F->t, LA_ADD, rdst, rs, 0, 1);         /* sext.w */
    else if (sign)
        la_ext(F->t, rdst, rs, size);
    else if (size == 1)
        la_alu_imm(F->t, LA_AND, rdst, rs, 255, 0);
    else
        la_bstrpick(F->t, rdst, rs, size * 8 - 1, 0, 1);
}

/* ---- branches ------------------------------------------------------------
 *
 * No condition codes: a conditional branch compares its two registers
 * itself (beq/bne/blt/bge/bltu/bgeu rj, rd: +-128 KiB), or one register
 * against zero (beqz/bnez: +-4 MiB); `b` reaches +-128 MiB.
 *
 * A function is generated with every branch in its SHORT form first. One
 * that does not reach its label is then made the LONG form -- the inverse
 * branch over a `b` -- and the function generated again, until every one
 * fits (gen_func). The jumps and branches come out in the same order on
 * every pass, so the ordinal names each one.
 */
enum { FX_J,       /* b label */
       FX_B,       /* a direct conditional branch */
       FX_LONG,    /* the inverse branch at bat, over a b at `at` */
       FX_TAB };   /* a jump table's entry: bat is the pcaddi that finds
                    * the table, the word becomes target - pcaddi */

static void want_label(struct la_fn *F, int at, int label, int kind,
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

/* The form an earlier pass chose for the next jump or branch, or -1. */
static int relaxed_form(const struct la_fn *F)
{
    return F->relax && F->nfix < F->nrelax ? F->relax[F->nfix] : -1;
}

static void jump_to(struct la_fn *F, int label)
{
    want_label(F, la_j_placeholder(F->t, 0), label, FX_J, 0, 0, 0, 0);
}

static int invert_branch(int cond)
{
    switch (cond) {
    case LA_BEQ:  return LA_BNE;
    case LA_BNE:  return LA_BEQ;
    case LA_BLT:  return LA_BGE;
    case LA_BGE:  return LA_BLT;
    case LA_BLTU: return LA_BGEU;
    case LA_BGEU: return LA_BLTU;
    case LA_BEQZ: return LA_BNEZ;
    default:      return LA_BEQZ;      /* LA_BNEZ */
    }
}

/* A branch with its two operands, the form for a test against zero
 * being beqz/bnez, which reach further. */
static int la_branch_place(struct la_fn *F, int cond, int rs1, int rs2)
{
    if ((cond == LA_BEQ || cond == LA_BNE) && rs2 == LA_ZERO) {
        cond = cond == LA_BEQ ? LA_BEQZ : LA_BNEZ;
    } else if ((cond == LA_BEQ || cond == LA_BNE) && rs1 == LA_ZERO) {
        cond = cond == LA_BEQ ? LA_BEQZ : LA_BNEZ;
        rs1 = rs2;
    }
    if (cond == LA_BEQZ || cond == LA_BNEZ)
        rs2 = LA_ZERO;
    return la_b_placeholder(F->t, cond, rs1, rs2);
}

static void branch_if(struct la_fn *F, int cond, int rs1, int rs2, int label)
{
    int at, jat;
    if (relaxed_form(F) != FX_LONG) {
        want_label(F, la_branch_place(F, cond, rs1, rs2), label, FX_B,
                   cond, rs1, rs2, 0);
        return;
    }
    /* The long form, which reaches anywhere: the opposite branch over a
     * jump. */
    at = la_branch_place(F, invert_branch(cond), rs1, rs2);
    jat = la_j_placeholder(F->t, 0);
    want_label(F, jat, label, FX_LONG, cond, rs1, rs2, at);
    if (!la_patch_b(F->t, at, F->t->len))   /* over the jump just emitted */
        internal_error("loongarch: a branch over one instruction does not "
                       "reach");
}

/* ---- site lists --------------------------------------------------------- */

/* A call to a function defined in this unit is a `bl`, patched here once
 * every function has a place (codegen_unit_loongarch); one to anything
 * else is a `bl` the linker relocates (R_LARCH_B26). */
static void note_call(struct la_sites *st, int at, struct func *target)
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

static void note_ext(struct la_sites *st, int at, struct func *callee)
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

static void note_str(struct la_sites *st, int at, int idx, enum reloc_kind k)
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

static void note_glob(struct la_sites *st, int at, struct global *g,
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

static void note_fn(struct la_sites *st, int at, struct func *target,
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

/* The runtime helpers: every floating-point operation, the 128-bit
 * divides and shifts. Interned by NAME rather than from a fixed
 * table, because there are forty once soft float is counted and a table
 * would be a second place to keep the list. */
static struct func **g_helpers;
static int g_nhelpers, g_caphelpers;

static void call_helper(struct la_fn *F, const char *name)
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
    note_ext(F->st, la_j_placeholder(F->t, 1), h);
}

/* pcalau12i rd, 0 ; addi.d rd, rd, 0 -- a symbol's address in two halves,
 * both immediates left for relocation. Returns the pcalau12i's offset; the
 * addi.d is four bytes after it. */
static int la_pcala_pair(struct code *t, int rd)
{
    int at = t->len;
    la_pcrel(t, LA_PCALAU12I, rd, 0);
    la_alu_imm(t, LA_ADD, rd, rd, 0, 0);
    return at;
}

/* ---- floating point, which this configuration has none of ----------------
 *
 * LP64S with -mfpu=none: every floating-point operation is a call and the
 * soft-float ABI passes the operands in the INTEGER argument registers, a
 * float or a double in one. The value is therefore never anything but
 * bits, and the integer paths already carry exactly the right number of
 * them. The names are libgcc's, which is what clang calls here too.
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
static void set_args(struct la_fn *F, const int *dstreg, const int *vreg,
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
            internal_error("loongarch: a helper's argument setup is not a "
                           "well-formed move");
        for (int k = 0; k < m; k++)
            la_mv(F->t, od[k], os[k]);
    }
    /* The loads come after: they only WRITE argument registers, so by
     * now nothing still needs the old contents of one. */
    for (int k = 0; k < n; k++)
        if (!in_reg(F, vreg[k]))
            ld_sp(F, dstreg[k], sslot(F, vreg[k]), F->w, 1);
}

/* Both operands of a two-argument helper. */
static void fp_args2(struct la_fn *F, const struct ir_ins *i)
{
    int dstreg[2], vreg[2];
    dstreg[0] = LA_A0; vreg[0] = i->a;
    dstreg[1] = LA_A1; vreg[1] = i->b;
    set_args(F, dstreg, vreg, 2);
}

static void fp_result(struct la_fn *F, int dst)
{
    if (dst >= 0)
        wr(F, dst, LA_A0);
}

/* ---- comparisons ---------------------------------------------------------
 *
 * There is no `setcc`. A 0-or-1 is built from `slt`, the only comparison
 * the machine computes into a register, plus `xori 1` to invert and an
 * operand swap to reverse. `==` and `!=` go through `xor` first, because
 * slt cannot express them.
 */
static void cmp_to_reg(struct la_fn *F, enum binop pred, int sign,
                       int ra, int rb, int dst)
{
    struct code *t = F->t;
    switch (pred) {
    case B_EQ:
        la_alu(t, LA_XOR, dst, ra, rb, 0);
        la_alu_imm(t, LA_SLTU, dst, dst, 1, 0);           /* seqz */
        return;
    case B_NE:
        la_alu(t, LA_XOR, dst, ra, rb, 0);
        la_alu(t, LA_SLTU, dst, LA_ZERO, dst, 0);         /* snez */
        return;
    case B_LT:
        la_alu(t, sign ? LA_SLT : LA_SLTU, dst, ra, rb, 0);
        return;
    case B_GT:
        la_alu(t, sign ? LA_SLT : LA_SLTU, dst, rb, ra, 0);
        return;
    case B_GE:
        la_alu(t, sign ? LA_SLT : LA_SLTU, dst, ra, rb, 0);
        la_alu_imm(t, LA_XOR, dst, dst, 1, 0);
        return;
    default: /* B_LE */
        la_alu(t, sign ? LA_SLT : LA_SLTU, dst, rb, ra, 0);
        la_alu_imm(t, LA_XOR, dst, dst, 1, 0);
        return;
    }
}

/* The same against a constant k, when it fits an immediate: slti and
 * sltui take a SIGNED 12-bit k as it is (sign-extended to 64 bits, which
 * is the form imm_val gives and rd32 reads), `x <= k` is `x < k + 1` and
 * `x > k` its inverse; `==` is an xori (k in 0..4095, the field being
 * unsigned) or an addi.d of -k, and a seqz -- against zero the seqz, snez
 * or slt alone. Where k does not fit, 0 and the caller loads it. */
static int cmp_imm_to_reg(struct la_fn *F, enum binop pred, int sign,
                          int ra, long long k, int dst)
{
    struct code *t = F->t;
    int inv = pred == B_GE || pred == B_GT;
    if (pred == B_LE || pred == B_GT) {
        if (!sign && k == -1)
            return 0;               /* k + 1 wraps: x <=u max */
        k++;
    }
    switch (pred) {
    case B_EQ: case B_NE:
        if (k > 0 && k <= 4095) {
            la_alu_imm(t, LA_XOR, dst, ra, k, 0);
            ra = dst;
        } else if (k) {
            if (!la_fits(-k, 12))
                return 0;
            la_alu_imm(t, LA_ADD, dst, ra, -k, 0);
            ra = dst;
        }
        if (pred == B_EQ) la_alu_imm(t, LA_SLTU, dst, ra, 1, 0);  /* seqz */
        else              la_alu(t, LA_SLTU, dst, LA_ZERO, ra, 0); /* snez */
        return 1;
    default:
        if (k < -2048 || k > 2047)
            return 0;
        if (k == 0 && sign)
            la_alu(t, LA_SLT, dst, ra, LA_ZERO, 0);                /* sltz */
        else
            la_alu_imm(t, sign ? LA_SLT : LA_SLTU, dst, ra, k, 0);
        if (inv)
            la_alu_imm(t, LA_XOR, dst, dst, 1, 0);
        return 1;
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
static int la_tail_ok(const struct la_fn *F, int n)
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
static void la_restore(struct la_fn *F)
{
    struct code *t = F->t;
    for (int k = 0; k < F->nsave; k++)
        ld_sp(F, F->used_callee[k], F->save_at + (long)k * F->w, F->w, 1);
    if (!F->leaf)
        ld_sp(F, LA_RA, F->ra_slot, F->w, 1);
    if (F->frame) {
        if (la_fits(F->frame, 12)) {
            la_alu_imm(t, LA_ADD, LA_SP, LA_SP, (int)F->frame, 0);
        } else {
            la_li(t, LA_T0, F->frame);
            la_alu(t, LA_ADD, LA_SP, LA_SP, LA_T0, 0);
        }
    }
}

static void gen_call(struct la_fn *F, int n)
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
            int s = rdr(F, a->vreg, TMP);    /* its address */
            copy_block_at(F, 1, a->size, byref_step(F->w, a), s, 0,
                          F->fb, copy_at);
        } else {
            need16(F, a->vreg);
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
                    la_load(t, SCR, ADDR, (int)off, F->w, 0);
                    st_out(F, SCR, pl[k].stk + (long)q * F->w, F->w);
                } else {
                    /* The tail of an odd-sized struct, byte by byte: a
                     * whole-word load would read past the object. */
                    for (long b = 0; b < left; b++) {
                        la_load(t, SCR, ADDR, (int)(off + b), 1, 0);
                        st_out(F, SCR, pl[k].stk + (long)q * F->w + b, 1);
                    }
                }
            }
        } else if (a->size > F->w) {
            ld128(F, a->vreg, SCR, SCR2);
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
        int sd_[MAX_PARAMS], sv_[MAX_PARAMS];
        int ns_ = 0;
        for (int k = 0; k < i->nargs; k++) {
            struct ir_arg *a = &i->argv[k];
            if (!pl[k].nreg || pl[k].byref || a->is_struct || a->size > F->w)
                continue;
            sd_[ns_] = argreg(pl[k].reg);
            sv_[ns_] = a->vreg;
            ns_++;
        }
        if (ns_)
            set_args(F, sd_, sv_, ns_);
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
                    la_load(t, r, ADDR, (int)off, F->w, 0);
                } else {
                    /* The last, partial word, assembled byte by byte into
                     * its register. An aggregate travels PACKED -- these
                     * are the object's bytes, not its fields. */
                    la_mv(t, r, LA_ZERO);
                    for (long b = off + left - 1; b >= off; b--) {
                        la_shift_imm(t, LA_SLL, r, r, 8, 0);
                        la_load(t, SCR, ADDR, (int)b, 1, 0);
                        la_alu(t, LA_OR, r, r, SCR, 0);
                    }
                }
            }
        } else if (a->size > F->w) {
            /* A sixteen-byte value is in its slot, so loading it now can
             * overwrite nothing the move above still had to read. */
            if (pl[k].nreg == 2)
                ld128(F, a->vreg, argreg(pl[k].reg), argreg(pl[k].reg + 1));
            else
                ld_sp(F, argreg(pl[k].reg), sslot(F, a->vreg), F->w,
                      1);                        /* the low word */
        }
        /* a plain scalar: already placed by the parallel move above */
    }
    /* The hidden result pointer goes in LAST, so nothing above can have
     * used a0 as a scratch after it was set. */
    if (sret && i->retsize) {
        addr_sp(F, LA_A0, F->scratch_at + i->scratch);
    } else if (sret) {
        la_refuse(F, i, "a scalar result wider than two registers");
    }

    if (F->tail && F->tail[n]) {
        /* The frame down, then a JUMP: the callee returns straight to
         * this function's caller, with ra as it came in. */
        la_restore(F);
        if (cg_call_local(fn->src, i->callee)) {
            note_call(F->st, la_j_placeholder(t, 0), i->callee);
            F->st->call[F->st->ncall - 1].tail = 1;
        } else {
            note_ext(F->st, la_j_placeholder(t, 0), i->callee);
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
        la_jirl(t, LA_RA, SCR, 0);
    } else if (cg_call_local(fn->src, i->callee)) {
        note_call(F->st, la_j_placeholder(t, 1), i->callee);
    } else {
        note_ext(F->st, la_j_placeholder(t, 1), i->callee);
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
                    st_sp(F, LA_A0 + q, at + off, F->w);
                } else {
                    for (long b = 0; b < left; b++) {
                        if (b) la_shift_imm(t, LA_SRL, LA_A0 + q, LA_A0 + q,
                                            8, 0);
                        st_sp(F, LA_A0 + q, at + off + b, 1);
                    }
                }
            }
        }
        addr_sp(F, ACC, F->scratch_at + i->scratch);
        wr(F, i->dst, ACC);
    } else if (sret) {
        /* the callee wrote it through a0 */
    } else if (i->w == 16 || is16(F, i->dst)) {
        st128(F, i->dst, LA_A0, LA_A1);
    } else {
        wr(F, i->dst, LA_A0);
    }
}

/* ---- 128 bits -----------------------------------------------------
 *
 * __int128 and long double (IEEE binary128) are two 64-bit words. Each
 * such value lives in a sixteen-byte slot of its own and never in a
 * register: la_w16_map marks them, and the allocator is handed that map
 * as `wide` (ineligible) -- the AArch64 backend's arrangement. An
 * operation loads the words it needs into t0-t2, t4 and t5, the low word
 * at the slot and the high one eight bytes above, computes, and stores
 * both back; t6 stays the far-offset register. None of those six is ever
 * allocated, so an inline operation disturbs no value.
 *
 * Division, remainder, a shift by a count not known here, and every
 * binary128 operation and conversion are calls into lib/rt (int128.c,
 * fp128.c, softtf.c) under libgcc's names, a 128-bit operand in a0:a1
 * and a second in a2:a3 -- which la_op_calls_helper tells the allocator. */

#define W_HI SCR2               /* t5: the second operand's high word */

/* Only a value la_w16_map marked HAS sixteen bytes of slot: one it
 * missed has eight, or a register, and reading sixteen there takes the
 * neighbour's bytes for the high word. */
static void need16(const struct la_fn *F, int v)
{
    if (!is16(F, v))
        internal_error("loongarch: %s: vreg %d is read or written as sixteen "
                       "bytes and has no sixteen-byte slot", F->fn->name, v);
}

static void ld128(struct la_fn *F, int v, int lo, int hi)
{
    need16(F, v);
    long s = sslot(F, v);
    ld_sp(F, lo, s, 8, 1);
    ld_sp(F, hi, s + 8, 8, 1);
}

static void st128(struct la_fn *F, int v, int lo, int hi)
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
static int la_const_of(const struct la_fn *F, int v, long *out)
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
static int la_ins128(const struct la_fn *F, const struct ir_ins *i)
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
static void shift128_imm(struct la_fn *F, const struct ir_ins *i, int k)
{
    struct code *t = F->t;
    int left = i->op == IR_SHL, ar = !left && i->sign;
    if (k == 0)
        return;
    if (k >= 64) {
        /* One word crosses into the other, and the vacated word is zero
         * -- or, arithmetically, the sign. */
        if (left) {
            la_shift_imm(t, LA_SLL, A_HI, A_LO, k - 64, 0);
            la_mv(t, A_LO, LA_ZERO);
        } else {
            la_shift_imm(t, ar ? LA_SRA : LA_SRL, A_LO, A_HI, k - 64, 0);
            if (ar) la_shift_imm(t, LA_SRA, A_HI, A_HI, 63, 0);
            else    la_mv(t, A_HI, LA_ZERO);
        }
        return;
    }
    /* 1..63: each word shifts, and the bits leaving one enter the other */
    if (left) {
        la_shift_imm(t, LA_SRL, B_LO, A_LO, 64 - k, 0);
        la_shift_imm(t, LA_SLL, A_HI, A_HI, k, 0);
        la_alu(t, LA_OR, A_HI, A_HI, B_LO, 0);
        la_shift_imm(t, LA_SLL, A_LO, A_LO, k, 0);
    } else {
        la_shift_imm(t, LA_SLL, B_LO, A_HI, 64 - k, 0);
        la_shift_imm(t, LA_SRL, A_LO, A_LO, k, 0);
        la_alu(t, LA_OR, A_LO, A_LO, B_LO, 0);
        la_shift_imm(t, ar ? LA_SRA : LA_SRL, A_HI, A_HI, k, 0);
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

static void gen_ins128(struct la_fn *F, struct ir_ins *i)
{
    struct code *t = F->t;
    long k;

    if (i->imm_b)          /* pass_immfold leaves width 16 alone */
        la_refuse(F, i, "a folded immediate on a 128-bit operation");

    switch (i->op) {
    case IR_CONST:                      /* sign-extended, as irgen made it */
        la_li(t, A_LO, i->imm);
        la_li(t, A_HI, i->imm < 0 ? -1 : 0);
        st128(F, i->dst, A_LO, A_HI);
        return;

    case IR_LDVAR: case IR_STVAR: case IR_MOV:
        /* slot to slot: the local is `a` of an ldvar and `dst` of an
         * stvar, and one of sixteen bytes is never in a register */
        if (!is16(F, i->a) || !is16(F, i->dst))
            la_refuse(F, i, "a sixteen-byte copy of a narrower value");
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
            la_refuse(F, i, "a sixteen-byte load into a narrower value");
        la_load(t, A_LO, ra_, 0, 8, 0);
        la_load(t, A_HI, ra_, 8, 8, 0);
        st128(F, i->dst, A_LO, A_HI);
        return;
    }
    case IR_STORE: {
        if (!is16(F, i->b) || i->memoff)
            la_refuse(F, i, "a sixteen-byte store of a narrower value");
        ld128(F, i->b, A_LO, A_HI);
        int ra_ = rdr(F, i->a, ADDR);
        la_store(t, A_LO, ra_, 0, 8);
        la_store(t, A_HI, ra_, 8, 8);
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
            if (i->sign) la_shift_imm(t, LA_SRA, A_HI, A_LO, 63, 0);
            else         la_mv(t, A_HI, LA_ZERO);
        }
        st128(F, i->dst, A_LO, A_HI);
        return;

    case IR_SELECT:
        la_refuse(F, i, "a 128-bit select");
        return;

    case IR_BRZ: case IR_BRNZ:
        ld128(F, i->a, A_LO, A_HI);
        la_alu(t, LA_OR, A_LO, A_LO, A_HI, 0);
        branch_if(F, i->op == IR_BRZ ? LA_BEQ : LA_BNE, A_LO, LA_ZERO,
                  i->label);
        return;

    default:
        break;
    }

    if (i->flt) {
        switch (i->op) {
        case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV:
            ld128(F, i->a, LA_A0, LA_A1);
            ld128(F, i->b, LA_A2, LA_A3);
            call_helper(F, i->op == IR_ADD ? "__addtf3"
                           : i->op == IR_SUB ? "__subtf3"
                           : i->op == IR_MUL ? "__multf3" : "__divtf3");
            st128(F, i->dst, LA_A0, LA_A1);
            return;
        case IR_NEG:                    /* bit 127: right for -0.0, NaN */
            ld128(F, i->a, A_LO, A_HI);
            la_li(t, B_LO, (long long)(-9223372036854775807LL - 1));
            la_alu(t, LA_XOR, A_HI, A_HI, B_LO, 0);
            st128(F, i->dst, A_LO, A_HI);
            return;
        case IR_CMP: {
            ld128(F, i->a, LA_A0, LA_A1);
            ld128(F, i->b, LA_A2, LA_A3);
            call_helper(F, tf_cmp_name(i->pred));
            int d = wreg(F, i->dst, ACC);
            cmp_to_reg(F, i->pred, 1, LA_A0, LA_ZERO, d);
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
            ld128(F, i->a, LA_A0, LA_A1);
        } else if (i->op == IR_I2F && sw <= 4) {
            /* an int or unsigned argument: sign-extended either way, the
             * psABI's rule for every 32-bit value (rd32) */
            int r = rd32(F, i->a, LA_A0);
            if (r != LA_A0) la_mv(t, LA_A0, r);
        } else {
            rd(F, i->a, LA_A0);
        }
        call_helper(F, name);
        if (dw == 16)
            st128(F, i->dst, LA_A0, LA_A1);
        else if (i->dst >= 0)
            wr(F, i->dst, LA_A0);
        return;
    }

    case IR_ADD: case IR_SUB:
        /* No carry flag: after lo = a + b, `sltu lo, b` is the carry,
         * and before a - b, `sltu a, b` is the borrow. */
        ld128(F, i->a, A_LO, A_HI);
        ld128(F, i->b, B_LO, W_HI);
        if (i->op == IR_ADD) {
            la_alu(t, LA_ADD, A_LO, A_LO, B_LO, 0);
            la_alu(t, LA_SLTU, SCR, A_LO, B_LO, 0);
            la_alu(t, LA_ADD, A_HI, A_HI, W_HI, 0);
            la_alu(t, LA_ADD, A_HI, A_HI, SCR, 0);
        } else {
            la_alu(t, LA_SLTU, SCR, A_LO, B_LO, 0);
            la_alu(t, LA_SUB, A_LO, A_LO, B_LO, 0);
            la_alu(t, LA_SUB, A_HI, A_HI, W_HI, 0);
            la_alu(t, LA_SUB, A_HI, A_HI, SCR, 0);
        }
        st128(F, i->dst, A_LO, A_HI);
        return;

    case IR_AND: case IR_OR: case IR_XOR: {
        int op = i->op == IR_AND ? LA_AND : i->op == IR_OR ? LA_OR : LA_XOR;
        ld128(F, i->a, A_LO, A_HI);
        ld128(F, i->b, B_LO, W_HI);
        la_alu(t, op, A_LO, A_LO, B_LO, 0);
        la_alu(t, op, A_HI, A_HI, W_HI, 0);
        st128(F, i->dst, A_LO, A_HI);
        return;
    }

    case IR_BNOT:
        ld128(F, i->a, A_LO, A_HI);
        la_alu(t, LA_NOR, A_LO, A_LO, LA_ZERO, 0);
        la_alu(t, LA_NOR, A_HI, A_HI, LA_ZERO, 0);
        st128(F, i->dst, A_LO, A_HI);
        return;

    case IR_NEG:                        /* 0 - a, the borrow from lo */
        ld128(F, i->a, A_LO, A_HI);
        la_alu(t, LA_SLTU, SCR, LA_ZERO, A_LO, 0);          /* snez */
        la_alu(t, LA_SUB, A_LO, LA_ZERO, A_LO, 0);
        la_alu(t, LA_SUB, A_HI, LA_ZERO, A_HI, 0);
        la_alu(t, LA_SUB, A_HI, A_HI, SCR, 0);
        st128(F, i->dst, A_LO, A_HI);
        return;

    case IR_MUL:
        /* The low 128 bits of the product, which is the same for signed
         * and unsigned: lo*lo in full (mul, mulhu) plus each cross term's
         * low word in the high half. */
        ld128(F, i->a, A_LO, A_HI);
        ld128(F, i->b, B_LO, W_HI);
        la_alu(t, LA_MULHU, SCR, A_LO, B_LO, 0);
        la_alu(t, LA_MUL, A_HI, A_HI, B_LO, 0);
        la_alu(t, LA_MUL, W_HI, A_LO, W_HI, 0);
        la_alu(t, LA_MUL, A_LO, A_LO, B_LO, 0);
        la_alu(t, LA_ADD, SCR, SCR, A_HI, 0);
        la_alu(t, LA_ADD, A_HI, SCR, W_HI, 0);
        st128(F, i->dst, A_LO, A_HI);
        return;

    case IR_DIV: case IR_MOD:
        ld128(F, i->a, LA_A0, LA_A1);
        ld128(F, i->b, LA_A2, LA_A3);
        call_helper(F, i->op == IR_DIV
                       ? (i->sign ? "__divti3" : "__udivti3")
                       : (i->sign ? "__modti3" : "__umodti3"));
        st128(F, i->dst, LA_A0, LA_A1);
        return;

    case IR_SHL: case IR_SHR:
        if (la_const_of(F, i->b, &k) && k >= 0 && k < 128) {
            ld128(F, i->a, A_LO, A_HI);
            shift128_imm(F, i, (int)k);
            st128(F, i->dst, A_LO, A_HI);
            return;
        }
        {
            /* the count first: it may be in a0 or a1, which the value's
             * words are about to take */
            int r = rd32(F, i->b, LA_A2);
            if (r != LA_A2) la_mv(t, LA_A2, r);
        }
        ld128(F, i->a, LA_A0, LA_A1);
        call_helper(F, i->op == IR_SHL ? "__ashlti3"
                       : i->sign ? "__ashrti3" : "__lshrti3");
        st128(F, i->dst, LA_A0, LA_A1);
        return;

    case IR_CMP: {
        int d = wreg(F, i->dst, ACC);
        if (i->pred == B_EQ || i->pred == B_NE) {
            ld128(F, i->a, A_LO, A_HI);
            ld128(F, i->b, B_LO, W_HI);
            la_alu(t, LA_XOR, A_LO, A_LO, B_LO, 0);
            la_alu(t, LA_XOR, A_HI, A_HI, W_HI, 0);
            la_alu(t, LA_OR, A_LO, A_LO, A_HI, 0);
            cmp_to_reg(F, i->pred, 0, A_LO, LA_ZERO, d);
        } else {
            /* x < y: the high words decide unless they are equal, and
             * then the low words do, unsigned. > and <= swap the two. */
            int swap = i->pred == B_GT || i->pred == B_LE;
            ld128(F, swap ? i->b : i->a, A_LO, A_HI);
            ld128(F, swap ? i->a : i->b, B_LO, W_HI);
            la_alu(t, i->sign ? LA_SLT : LA_SLTU, SCR, A_HI, W_HI, 0);
            la_alu(t, LA_XOR, A_HI, A_HI, W_HI, 0);
            la_alu_imm(t, LA_SLTU, A_HI, A_HI, 1, 0);       /* seqz */
            la_alu(t, LA_SLTU, A_LO, A_LO, B_LO, 0);
            la_alu(t, LA_AND, A_LO, A_LO, A_HI, 0);
            la_alu(t, LA_OR, d, A_LO, SCR, 0);
            if (i->pred == B_GE || i->pred == B_LE)
                la_alu_imm(t, LA_XOR, d, d, 1, 0);
        }
        wrote(F, i->dst, d);
        return;
    }

    default:
        la_refuse(F, i, "this operation on a 128-bit value");
    }
}

/* ---- one instruction ------------------------------------------------------ */

static void gen_ins(struct la_fn *F, int n)
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

    if (F->w16 && la_ins128(F, i)) {
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
            la_refuse(F, i, "a long double (no binary128 arithmetic yet)");
        if (name) {
            if (i->imm_b)
                la_refuse(F, i, "a folded floating-point immediate");
            fp_args2(F, i);
            call_helper(F, name);
            fp_result(F, i->dst);
            return;
        }
        if (i->op == IR_NEG) {
            /* The sign bit, flipped. A call would be correct and this is
             * two instructions -- and unlike a subtraction from zero it
             * is right for -0.0 and for a NaN. */
            rd(F, i->a, ACC);
            la_li(t, TMP, i->w == 8 ? (long long)(-9223372036854775807LL - 1)
                                    : 0x80000000LL);
            la_alu(t, LA_XOR, ACC, ACC, TMP, 0);
            wr(F, i->dst, ACC);
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
                    int cond, r1 = LA_A0, r2 = LA_ZERO;
                    switch (i->pred) {
                    case B_EQ: cond = LA_BEQ; break;
                    case B_NE: cond = LA_BNE; break;
                    case B_LT: cond = LA_BLT; break;
                    case B_GE: cond = LA_BGE; break;
                    case B_GT: cond = LA_BLT; r1 = LA_ZERO; r2 = LA_A0; break;
                    default:   cond = LA_BGE; r1 = LA_ZERO; r2 = LA_A0; break;
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
                cmp_to_reg(F, i->pred, 1, LA_A0, LA_ZERO, d);
                wrote(F, i->dst, d);
            }
            return;
        }
        if (i->op == IR_SQRT)
            la_refuse(F, i, "__builtin_sqrt (a libm routine here, not an "
                            "instruction)");
        la_refuse(F, i, "this floating-point operation");
    }

    /* (A call or a return of one is gen_call's and IR_RET's.) */
    if (i->w > 8 &&
        !(F->w16 && (i->op == IR_CALL || i->op == IR_RET)))
        la_refuse(F, i, "a 128-bit value");

    /* A 32-bit operation must leave a SIGN-EXTENDED result: the psABI's
     * invariant is that a register holds the sign-extension of its 32-bit
     * value, and `ld.w` maintains it on the way in. The `.w` instruction
     * forms maintain it on the way out. and/or/xor need none -- the
     * operation of two sign-extended values already is one -- which is
     * why this is a flag and not a second opcode table. */
    wordop = i->w == 4;

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
        la_li(t, d, imm_val(F, i));
        wrote(F, i->dst, d);
        return;
    }
    /* Soft float -- see the 64-bit arm. */
    case IR_BITCAST:
    case IR_MOV: {
        int src = rdr(F, i->a, ACC);
        int d = wreg(F, i->dst, ACC);
        if (src != d)
            la_mv(t, d, src);
        wrote(F, i->dst, d);
        return;
    }

    case IR_ADD: case IR_SUB: case IR_MUL:
    case IR_AND: case IR_OR:  case IR_XOR: {
        /* A switch and not a table indexed by (i->op - IR_ADD): IR_DIV
         * and IR_MOD sit between IR_MUL and IR_AND, so a six-entry table
         * turns `and` into something else and reads past its end for
         * `or` and `xor`. It compiled, ran, and returned v & ~v. */
        int op = i->op == IR_ADD ? LA_ADD
               : i->op == IR_SUB ? LA_SUB
               : i->op == IR_AND ? LA_AND
               : i->op == IR_OR  ? LA_OR
               : i->op == IR_XOR ? LA_XOR
               : -1;                           /* IR_MUL: not an ALU op */
        int logical = i->op == IR_AND || i->op == IR_OR || i->op == IR_XOR;
        int ra_ = rdr(F, i->a, ACC);
        int rd_ = wreg(F, i->dst, ACC);
        if (i->imm_b && i->op != IR_MUL) {
            /* addi takes a SIGNED 12-bit value, and `sub` has none -- a
             * folded subtraction adds the negative, and -(-2048) does not
             * fit, which is why the range is checked here rather than
             * assumed from the original constant's. andi, ori and xori
             * take an UNSIGNED one, 0..4095: `x & -16` is not an andi. */
            long long v = imm_val(F, i);
            if (i->op == IR_SUB) v = -v;
            if (logical ? la_ufits(v, 12) : la_fits(v, 12)) {
                la_alu_imm(t, i->op == IR_SUB ? LA_ADD : op, rd_, ra_,
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
                la_alu(t, LA_MUL, rd_, ra_, rb_, wordop);
            else
                la_alu(t, op, rd_, ra_, rb_, logical ? 0 : wordop);
        }
        wrote(F, i->dst, rd_);
        return;
    }
    case IR_DIV: case IR_MOD: {
        /* div.w, mod.w, div.wu and mod.wu are UNDEFINED unless both
         * operands are the sign extension of their low 32 bits -- clang
         * puts an addi.w in front of each that is not -- so a 32-bit one
         * reads them through rd32. (A folded constant is built
         * sign-extended by imm_val.) */
        int ra_ = wordop ? rd32(F, i->a, ACC) : rdr(F, i->a, ACC);
        int rb_ = (i->imm_b || !in_reg(F, i->b)) ? TMP : F->loc[i->b];
        int d;
        if (rb_ == TMP) operand_b(F, i, TMP);
        if (wordop && !i->imm_b)
            rb_ = sext32(F, i->b, rb_, TMP);
        d = wreg(F, i->dst, ACC);
        la_alu(t, i->op == IR_DIV ? (i->sign ? LA_DIV : LA_DIVU)
                                     : (i->sign ? LA_MOD : LA_MODU),
                  d, ra_, rb_, wordop);
        wrote(F, i->dst, d);
        return;
    }
    case IR_SHL: case IR_SHR: {
        int op = i->op == IR_SHL ? LA_SLL : i->sign ? LA_SRA : LA_SRL;
        int bits = wordop ? 32 : 64;
        int ra_ = rdr(F, i->a, ACC);
        int d;
        if (i->imm_b && i->imm >= 0 && i->imm < bits) {
            d = wreg(F, i->dst, ACC);
            la_shift_imm(t, op, d, ra_, (int)i->imm, wordop);
        } else {
            int rb_ = (i->imm_b || !in_reg(F, i->b)) ? TMP : F->loc[i->b];
            if (rb_ == TMP) operand_b(F, i, TMP);
            d = wreg(F, i->dst, ACC);
            la_alu(t, op, d, ra_, rb_, wordop);
        }
        wrote(F, i->dst, d);
        return;
    }
    case IR_NEG: {
        int ra_ = rdr(F, i->a, ACC);
        int d = wreg(F, i->dst, ACC);
        la_alu(t, LA_SUB, d, LA_ZERO, ra_, wordop);
        wrote(F, i->dst, d);
        return;
    }
    case IR_BNOT: {
        int ra_ = rdr(F, i->a, ACC);
        int d = wreg(F, i->dst, ACC);
        la_alu(t, LA_NOR, d, ra_, LA_ZERO, 0);           /* not */
        wrote(F, i->dst, d);
        return;
    }

    case IR_CMP:
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
                nx->a == i->dst &&
                F->usecnt && F->usecnt[i->dst] == 1) {
                int ra_ = wordop ? rd32(F, i->a, ACC) : rdr(F, i->a, ACC);
                int rb_ = (i->imm_b || !in_reg(F, i->b)) ? TMP : F->loc[i->b];
                int cond, sw = 0;
                /* against zero: r0 is zero, and beqz/bnez reach
                 * furthest */
                if (i->imm_b && imm_val(F, i) == 0)
                    rb_ = LA_ZERO;
                if (rb_ == TMP) operand_b(F, i, TMP);
                if (wordop && !i->imm_b)
                    rb_ = sext32(F, i->b, rb_, TMP);
                switch (i->pred) {
                case B_EQ: cond = LA_BEQ; break;
                case B_NE: cond = LA_BNE; break;
                case B_LT: cond = i->sign ? LA_BLT : LA_BLTU; break;
                case B_GE: cond = i->sign ? LA_BGE : LA_BGEU; break;
                case B_GT: cond = i->sign ? LA_BLT : LA_BLTU; sw = 1; break;
                case B_LE: cond = i->sign ? LA_BGE : LA_BGEU; sw = 1; break;
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
            if (i->imm_b) {
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
        /* dst = a ? b : c, without a branch: maskeqz keeps b where the
         * condition is nonzero, masknez keeps c where it is zero, and the
         * two are or'd -- clang's sequence. The condition is tested at
         * ITS width, `size`, which is not the arms' `w`: if-convert
         * records the branch's, and a 32-bit one that is zero may have
         * bits above 31 (rd32). Every operand is read before the `or`
         * writes the destination, so it may be any of them. */
        int cond = i->size == 4 ? rd32(F, i->a, SCR) : rdr(F, i->a, SCR);
        int rb_ = rdr(F, i->b, TMP);
        int rc_ = rdr(F, i->c, SCR2);
        int d = wreg(F, i->dst, ACC);
        la_alu(t, LA_MASKEQZ, ADDR, rb_, cond, 0);
        la_alu(t, LA_MASKNEZ, TMP, rc_, cond, 0);
        la_alu(t, LA_OR, d, ADDR, TMP, 0);
        wrote(F, i->dst, d);
        return;
    }

    case IR_BRZ: case IR_BRNZ: {
        int r = wordop ? rd32(F, i->a, A_LO) : rdr(F, i->a, A_LO);
        branch_if(F, i->op == IR_BRZ ? LA_BEQ : LA_BNE, r, LA_ZERO,
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
            if (la_ldvar_plain(i->size, i->sign, i->w)) {
                if (d != F->loc[i->a]) la_mv(t, d, F->loc[i->a]);
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
            /* A narrowing store SIGN-extends, which la_ldvar_plain is
             * written to match: it is what makes a signed four-byte
             * read free at RV64, and it is the ABI's own invariant for
             * a 32-bit value in a 64-bit register. Only the low `size`
             * bytes carry the value, so an unsigned read still extends
             * for itself and is right regardless. */
            if (i->size >= F->w) {
                if (F->loc[i->dst] != src) la_mv(t, F->loc[i->dst], src);
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
        la_load(t, d, addr, i->memoff, i->size, i->sign);
        wrote(F, i->dst, d);
        return;
    }
    case IR_STORE: {
        int addr = rdr(F, i->a, ADDR);
        int val = rdr(F, i->b, ACC);
        la_store(t, val, addr, i->memoff, i->size);
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
     * address. `pcalau12i` yields the 4 KiB page of the symbol (relative
     * to the instruction's own page) and `addi.d` adds its SIGN-EXTENDED
     * low 12 bits, which is why the linker rounds the page by 0x800 --
     * and both relocations name the SYMBOL (la_pcala_pair). */
    case IR_STRADDR:
        {
        int d = wreg(F, i->dst, ACC);     /* straight into its home */
        int at = la_pcala_pair(t, d);
        note_str(F->st, at, i->label, RK_LA_PCALA_HI20);
        note_str(F->st, at + 4, i->label, RK_LA_PCALA_LO12);
        wrote(F, i->dst, d);
        }
        return;
    case IR_GADDR:
        {
        int d = wreg(F, i->dst, ACC);     /* straight into its home */
        int at = la_pcala_pair(t, d);
        note_glob(F->st, at, i->glob, RK_LA_PCALA_HI20);
        note_glob(F->st, at + 4, i->glob, RK_LA_PCALA_LO12);
        wrote(F, i->dst, d);
        }
        return;
    case IR_FADDR:
        {
        int d = wreg(F, i->dst, ACC);     /* straight into its home */
        int at = la_pcala_pair(t, d);
        note_fn(F->st, at, i->callee, RK_LA_PCALA_HI20);
        note_fn(F->st, at + 4, i->callee, RK_LA_PCALA_LO12);
        wrote(F, i->dst, d);
        }
        return;

    case IR_MEMCPY: case IR_MEMZERO: {
        int d = rdr(F, i->a, ADDR);
        int s = i->op == IR_MEMCPY ? rdr(F, i->b, TMP) : LA_ZERO;
        copy_block_at(F, i->op == IR_MEMCPY, i->size, F->w, s, 0, d, 0);
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
                    ld_sp(F, LA_A0, F->sret_slot, F->w, 1);
                } else {
                    /* Small enough for a0:a1, PACKED -- the object's
                     * bytes, not its fields. */
                    rd(F, i->a, ADDR);
                    for (int q = 0; (long)q * F->w < size; q++) {
                        long off = (long)q * F->w;
                        long left = size - off;
                        if (left >= F->w) {
                            la_load(t, LA_A0 + q, ADDR, (int)off, F->w, 0);
                        } else {
                            la_mv(t, LA_A0 + q, LA_ZERO);
                            for (long b = off + left - 1; b >= off; b--) {
                                la_shift_imm(t, LA_SLL, LA_A0 + q, LA_A0 + q,
                                             8, 0);
                                la_load(t, SCR, ADDR, (int)b, 1, 0);
                                la_alu(t, LA_OR, LA_A0 + q, LA_A0 + q, SCR, 0);
                            }
                        }
                    }
                }
            } else if (fn_sret_bytes(F->w, fn)) {
                la_refuse(F, i, "a scalar result wider than two registers");
            } else if (fn->ret_abi.size == 16) {
                ld128(F, i->a, LA_A0, LA_A1);
            } else if (F->sx && !fn->ret_abi.is_float &&
                       fn->ret_abi.size == 4) {
                /* the caller is owed the sign extension (rd32) */
                int r = rd32(F, i->a, LA_A0);
                if (r != LA_A0) la_mv(t, LA_A0, r);
            } else {
                rd(F, i->a, LA_A0);
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
        la_break(t, 0);
        return;
    case IR_FENCE:
        /* dbar 0: the full barrier, every access before it ordered
         * before every access after it -- what a seq_cst fence and the
         * barrier around a seq_cst load or store ask for. */
        la_dbar(t, 0);
        return;

    case IR_BSWAP: {
        /* revb.2h / revb.2w reverse the bytes of each halfword / word,
         * revb.d of the whole register; the narrow results are then
         * zero-extended, as the shift-and-or they replace left them. */
        int ra_ = rdr(F, i->a, ACC);
        int d = wreg(F, i->dst, ACC);
        if (i->size == 8) {
            la_revb(t, LA_REVB_D, d, ra_);
        } else if (i->size == 4 || i->size == 2) {
            la_revb(t, i->size == 4 ? LA_REVB_2W : LA_REVB_2H, d, ra_);
            la_bstrpick(t, d, d, i->size * 8 - 1, 0, 1);
        } else {
            la_refuse(F, i, "a byte swap of this width");
        }
        wrote(F, i->dst, d);
        return;
    }

    case IR_VA_START:
        /* va_list is a bare POINTER here, as it is on AAPCS32: it points
         * at the first unnamed argument and walks up. The prologue has
         * already spilled a0-a7 immediately below the caller's stack
         * arguments, so one pointer covers both halves. */
        rd(F, i->a, ADDR);
        addr_sp(F, ACC, F->va_first);
        la_store(t, ACC, ADDR, 0, F->w);
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
            /* A 64-bit conversion of a narrow source: zero-extended, as
             * the RISC-V backend's comment explains (irgen once widened an
             * `unsigned` this way, and a slot load sign-extends). */
            rd(F, i->a, LA_A0);
            ext_reg(F, LA_A0, LA_A0, 4, 0);
        } else if (i->op == IR_I2F && src_w == 4) {
            /* __floatsidf's argument is an int: sign-extended (rd32) */
            int r = rd32(F, i->a, LA_A0);
            if (r != LA_A0) la_mv(t, LA_A0, r);
        } else {
            rd(F, i->a, LA_A0);
        }
        call_helper(F, name);
        if (i->dst >= 0)
            wr(F, i->dst, LA_A0);
        return;
    }

    case IR_ASM: {
        /* Extended asm, assembled in irgen against the target's
         * vocabulary. This only places the operands and splices the
         * bytes. (irgen refuses inline asm on LoongArch today; this is the
         * RISC-V lowering with LoongArch's register roles, ready for it.)
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
                internal_error("loongarch: %s: an asm's further output is not "
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
            LA_T0, LA_T1, LA_T2, LA_T3, LA_T4, LA_T5, LA_T7, LA_T8,
            LA_A0, LA_A1, LA_A2, LA_A3, LA_A4, LA_A5, LA_A6, LA_A7
        };
        int scr = -1;
        for (int k = 0; k < ia->nin; k++) used[ia->in[k].reg] = 1;
        for (int k = 0; k < ia->nout; k++) used[ia->out[k].reg] = 1;
        for (int k = 0; k < nval; k++) used[vreg_[k]] = 1;
        for (int r = 0; r < 32; r++)
            if (used[r] && la_callee_saved(r))
                la_refuse(F, i, "an asm operand in a callee-saved register");
        if (ia->clob)
            scr = ia->scr;      /* irgen chose it, and the allocator knows */
        else
            for (unsigned k = 0; k < sizeof scr_pool / sizeof scr_pool[0];
                 k++)
                if (!used[scr_pool[k]]) { scr = scr_pool[k]; break; }
        /* The parallel moves' cycle breaker: a backend scratch no operand
         * uses, which is never a value's home either. */
        static const int pm_pool[] = { LA_T4, LA_T2, LA_T1, LA_T0, LA_T5 };
        int pmscr = -1;
        for (unsigned k = 0; k < sizeof pm_pool / sizeof pm_pool[0]; k++)
            if (!used[pm_pool[k]]) { pmscr = pm_pool[k]; break; }
        int naddr = 0;
        for (int k = 0; k < ia->nout; k++)
            naddr += !ia->out[k].val && !ia->out[k].mem;
        if (scr < 0 && naddr > 0)
            la_refuse(F, i, "an asm with no scratch register left around it");
        for (int k = 0; k < ia->nout; k++)
            if (!ia->out[k].mem && ia->out[k].size > F->w)
                la_refuse(F, i, "an asm output wider than a register");
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
                    la_refuse(F, i, "an asm whose operands cannot be moved "
                                    "into place");
                for (int k = 0; k < m; k++)
                    la_mv(t, od[k], os[k]);
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
                    la_load(t, o->reg, o->reg, 0, o->size, 0);
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
            la_store(t, o->reg, scr, 0, o->size);
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
                    la_refuse(F, i, "an asm whose outputs cannot be moved "
                                    "into place");
                for (int k = 0; k < m; k++)
                    la_mv(t, od[k], os[k]);
            }
        }
        return;
    }
    /* ---- atomics ------------------------------------------------------
     *
     * amswap_db, amadd_db, amand_db, amor_db and amxor_db at .w and .d:
     * one instruction each, returning the old value, and the _db forms
     * are full barriers -- what a seq_cst operation asks for. NAND and the
     * compare-and-swaps are ll/sc loops, bracketed by `dbar 0` on both
     * sides (clang leans on ll's implicit ordering and fences only the
     * failure path; a full barrier is never wrong). sc writes 1 when the
     * store happened -- the opposite of RISC-V's sc.
     *
     * Only at four and eight bytes: the base ISA has no byte or halfword
     * am* or ll/sc, so a one- or two-byte atomic is refused rather than
     * turned into a read-modify-write of the word around it, which would
     * not be atomic with respect to a neighbouring byte. */
    case IR_XCHG: case IR_XADD: case IR_ARMW: {
        int aw = i->size;
        int addr, val, dst;
        if (aw != 4 && aw != 8)
            la_refuse(F, i, aw < 4 ? "an atomic narrower than four bytes "
                                     "(the base ISA has no such am* or "
                                     "ll/sc, and a read-modify-write of the "
                                     "containing word is not atomic against "
                                     "its neighbours)"
                                   : "an atomic wider than a register");
        addr = rdr(F, i->a, ADDR);
        val = rdr(F, i->b, TMP);
        dst = wreg(F, i->dst, ACC);
        /* An am* instruction's rd may be neither its address nor its
         * value (it is undefined then), and the NAND loop writes dst
         * before its sc reads addr and val again: such a result is made
         * in ACC, which is neither, and `wrote` moves it home. */
        if (dst == addr || dst == val)
            dst = ACC;
        if (i->op == IR_XCHG)
            la_am(t, LA_AMSWAP, dst, val, addr, aw == 8);
        else if (i->op == IR_XADD)
            la_am(t, LA_AMADD, dst, val, addr, aw == 8);
        else {
            /* IR_ARMW's operation is a character in `imm`. */
            int op;
            switch ((int)i->imm) {
            case '&': op = LA_AMAND; break;
            case '|': op = LA_AMOR;  break;
            case '^': op = LA_AMXOR; break;
            default: {
                /* nand: dst = *a; *a = ~(dst & b), until the sc holds */
                int top, br;
                la_dbar(t, 0);
                top = t->len;
                la_ll(t, dst, addr, 0, aw == 8);
                la_alu(t, LA_AND, SCR, dst, val, 0);
                la_alu(t, LA_NOR, SCR, SCR, LA_ZERO, 0);
                la_sc(t, SCR, addr, 0, aw == 8);
                br = la_b_placeholder(t, LA_BEQZ, SCR, LA_ZERO);
                if (!la_patch_b(t, br, top))
                    internal_error("loongarch: an ll/sc loop's branch");
                la_dbar(t, 0);
                wrote(F, i->dst, dst);
                return;
            }
            }
            la_am(t, op, dst, val, addr, aw == 8);
        }
        wrote(F, i->dst, dst);
        return;
    }

    case IR_CAS: case IR_CMPXCHG: {
        /* A compare-and-swap is an ll/sc loop:
         *
         *   retry: ll.w   seen, addr, 0
         *          bne    seen, expected, out      -- someone else's value
         *          or     tmp, desired, zero
         *          sc.w   tmp, addr, 0
         *          beqz   tmp, retry               -- the reservation broke
         *   out:
         *
         * IR_CAS yields the value SEEN, whether or not the swap happened --
         * the __sync_val_compare_and_swap shape. IR_CMPXCHG yields a 0/1 and
         * writes the seen value back through the pointer in `b` -- the
         * __atomic_compare_exchange one. The two differ only in what is
         * stored afterwards, so they share the loop. */
        int aw = i->size;
        int addr, exp, des, seen, out_br, top, sc_br;
        if (aw != 4 && aw != 8)
            la_refuse(F, i, aw < 4 ? "an atomic compare-and-swap narrower "
                                     "than four bytes"
                                   : "an atomic wider than a register");
        addr = rdr(F, i->a, ADDR);
        if (i->op == IR_CAS) {
            /* compared with what ll.w sign-extended (rd32) */
            exp = aw == 4 ? rd32(F, i->b, TMP) : rdr(F, i->b, TMP);
        } else {
            /* IR_CMPXCHG's expected value is at *b, not in b. */
            int p = rdr(F, i->b, TMP);
            la_load(t, SCR, p, 0, aw, 1);
            exp = SCR;
        }
        des = rdr(F, i->c, SCR2);
        seen = ACC;
        la_dbar(t, 0);
        top = t->len;
        la_ll(t, seen, addr, 0, aw == 8);
        out_br = la_b_placeholder(t, LA_BNE, seen, exp);
        la_mv(t, FAR, des);
        la_sc(t, FAR, addr, 0, aw == 8);
        sc_br = la_b_placeholder(t, LA_BEQZ, FAR, LA_ZERO);
        if (!la_patch_b(t, sc_br, top) || !la_patch_b(t, out_br, t->len))
            internal_error("loongarch: an ll/sc loop's branch");
        la_dbar(t, 0);
        if (i->op == IR_CAS) {
            wr(F, i->dst, seen);
        } else {
            /* the bool: did the value seen equal the expected one? */
            int p = rdr(F, i->b, TMP);
            la_store(t, seen, p, 0, aw);   /* *b = what was seen */
            la_alu(t, LA_XOR, FAR, seen, exp, 0);
            la_alu_imm(t, LA_SLTU, FAR, FAR, 1, 0);    /* == 0 -> 1 */
            wr(F, i->dst, FAR);
        }
        return;
    }
    case IR_ALLOCA: {
        /* A variable-length array: sp -= round16(size). The block starts
         * ABOVE the outgoing-argument area, which stays at the bottom
         * of the stack where a callee looks for its arguments -- so the
         * area moves down with sp and the block sits on top of it. The
         * frame itself is addressed from fp in such a function. (andi's
         * field is unsigned, so the rounding down is two shifts.) */
        int d = wreg(F, i->dst, SCR2);
        rd(F, i->a, SCR);
        la_alu_imm(t, LA_ADD, SCR, SCR, 15, 0);
        la_shift_imm(t, LA_SRL, SCR, SCR, 4, 0);
        la_shift_imm(t, LA_SLL, SCR, SCR, 4, 0);
        la_alu(t, LA_SUB, LA_SP, LA_SP, SCR, 0);
        if (la_fits(F->out_bytes, 12)) {
            la_alu_imm(t, LA_ADD, d, LA_SP, (int)F->out_bytes, 0);
        } else {
            la_li(t, d, F->out_bytes);
            la_alu(t, LA_ADD, d, LA_SP, d, 0);
        }
        wrote(F, i->dst, d);
        return;
    }
    case IR_SPSAVE: {
        int d = wreg(F, i->dst, SCR);
        la_mv(t, d, LA_SP);
        wrote(F, i->dst, d);
        return;
    }
    case IR_SPRESTORE:
        la_mv(t, LA_SP, rdr(F, i->a, SCR));
        return;
    case IR_SWITCH: {
        /* A jump table in .text right after its dispatch, of 32-bit
         * offsets from the PCADDI that finds it (so the table's own
         * distance folds into the load's immediate):
         *     li t2, n ; bgeu rI, t2, default
         *     pcaddi t1, 0 ; alsl.d t2, rI, t1, 2
         *     ld.w t2, t2, table - pcaddi ; add.d t2, t2, t1 ; jr t2
         * so the table sits exactly 20 bytes past the pcaddi. At width 4
         * the index is sign-extended, so a negative one is a huge
         * unsigned and takes the default like any other value outside
         * the range. */
        int n = fn->jt[i->jt].n;
        int ri = wordop ? rd32(F, i->a, ACC) : rdr(F, i->a, ACC);
        la_li(t, LA_T2, n);
        branch_if(F, LA_BGEU, ri, LA_T2, i->label);
        int at = t->len;
        int tab = at + 20;
        la_pcrel(t, LA_PCADDI, TMP, 0);
        la_alsl(t, LA_T2, ri, TMP, 2, 1);
        la_load(t, LA_T2, LA_T2, tab - at, 4, 1);
        la_alu(t, LA_ADD, LA_T2, LA_T2, TMP, 0);
        la_jirl(t, LA_ZERO, LA_T2, 0);
        if (t->len != tab)
            internal_error("loongarch: %s: the jump table is not where its "
                           "pcaddi says", fn->name);
        for (int k = 0; k < n; k++) {
            want_label(F, t->len, fn->jt[i->jt].labels[k], FX_TAB,
                       0, 0, 0, at);
            code_u32(t, 0);
        }
        code_mark_data(t, tab, t->len);
        return;
    }
    case IR_LABELADDR: case IR_IGOTO:
        la_refuse(F, i, "a computed goto");
        return;
    default:
        la_refuse(F, i, "this operation");
    }
}

/* Copy `size` bytes from [TMP] to [ADDR] (copy) or zero them (!copy), in
 * accesses of `step` bytes and a byte tail. Straight-line while every
 * offset fits a load's or store's 12-bit immediate; past that, a loop
 * that walks both pointers with the end in SCR2 -- `long long a[300] =
 * {0}` and a 2403-byte struct returned by value were internal errors at
 * every -O level. TMP and ADDR are scratch and may be moved. */
static void copy_block(struct la_fn *F, int copy, long size, int step)
{
    struct code *t = F->t;
    long k;
    if (size <= 2040) {
        for (k = 0; k + step <= size; k += step) {
            if (copy) la_load(t, SCR, TMP, (int)k, step, 0);
            la_store(t, copy ? SCR : LA_ZERO, ADDR, (int)k, step);
        }
        for (; k < size; k++) {
            if (copy) la_load(t, SCR, TMP, (int)k, 1, 0);
            la_store(t, copy ? SCR : LA_ZERO, ADDR, (int)k, 1);
        }
        return;
    }
    long body = size / step * step;
    la_li(t, SCR2, body);
    la_alu(t, LA_ADD, SCR2, SCR2, ADDR, 0);
    int top = t->len;
    if (copy) {
        la_load(t, SCR, TMP, 0, step, 0);
        la_store(t, SCR, ADDR, 0, step);
        la_alu_imm(t, LA_ADD, TMP, TMP, step, 0);
    } else {
        la_store(t, LA_ZERO, ADDR, 0, step);
    }
    la_alu_imm(t, LA_ADD, ADDR, ADDR, step, 0);
    if (!la_patch_b(t, la_b_placeholder(t, LA_BNE, ADDR, SCR2), top))
        internal_error("loongarch: a block copy's loop branch");
    for (k = 0; k < size - body; k++) {
        if (copy) la_load(t, SCR, TMP, (int)k, 1, 0);
        la_store(t, copy ? SCR : LA_ZERO, ADDR, (int)k, 1);
    }
}

/* copy_block from [sreg + soff] to [dreg + doff] with those registers as
 * the bases, when every offset fits a load's or store's immediate: a
 * struct or long double whose address is already in a register, or whose
 * home is a frame slot, was first moved into TMP and ADDR -- a by-value
 * parameter's copy into its slot was `mv t1, a1; addi t2, sp, 48` before
 * its first word. Otherwise the addresses go to TMP and ADDR, and
 * copy_block does the rest. sreg and dreg are left as they were. */
static void copy_block_at(struct la_fn *F, int copy, long size, int step,
                          int sreg, long soff, int dreg, long doff)
{
    struct code *t = F->t;
    if (size <= 2040 && la_fits(doff, 12) && la_fits(doff + size, 12) &&
        (!copy || (la_fits(soff, 12) && la_fits(soff + size, 12)))) {
        /* the data register: SCR, unless a base is (param_reg's is) */
        int dr = sreg == SCR || dreg == SCR ? SCR2 : SCR;
        long k;
        for (k = 0; k + step <= size; k += step) {
            if (copy) la_load(t, dr, sreg, (int)(soff + k), step, 0);
            la_store(t, copy ? dr : LA_ZERO, dreg, (int)(doff + k), step);
        }
        for (; k < size; k++) {
            if (copy) la_load(t, dr, sreg, (int)(soff + k), 1, 0);
            la_store(t, copy ? dr : LA_ZERO, dreg, (int)(doff + k), 1);
        }
        return;
    }
    if (copy) {
        if (la_fits(soff, 12)) {
            la_alu_imm(t, LA_ADD, TMP, sreg, (int)soff, 0);
        } else {
            la_li(t, TMP, soff);
            la_alu(t, LA_ADD, TMP, sreg, TMP, 0);
        }
    }
    if (la_fits(doff, 12)) {
        la_alu_imm(t, LA_ADD, ADDR, dreg, (int)doff, 0);
    } else {
        la_li(t, ADDR, doff);
        la_alu(t, LA_ADD, ADDR, dreg, ADDR, 0);
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
static int param_reg(struct la_fn *F, const struct argplace *pl, int q)
{
    if (!F->fn->is_varargs)
        return argreg(pl->reg + q);
    ld_sp(F, SCR, F->va_regsave + (long)(pl->reg + q) * F->w, F->w, 1);
    return SCR;
}

/* ---- one function --------------------------------------------------------- */

static void gen_func(struct ir_func *fn, struct code *t, struct la_sites *st,
                     int want_debug)
{
    struct func *f = fn->src;
    struct la_fn F;
    int i;

    /* Zeroed first: usecnt and skip_next are only set when the
     * allocator runs, and reading them uninitialised on the other
     * paths is a segfault at -O0. */
    memset(&F, 0, sizeof F);
    F.fn = fn; F.t = t; F.st = st;
    F.want_debug = want_debug;
    F.w = 8;
    F.fix = NULL; F.nfix = F.capfix = 0;
    F.relax = NULL; F.nrelax = 0;
    F.wide = wide_map(fn);
    F.w16 = la_w16_map(fn);
    F.loc = NULL; F.nsave = 0;
    F.fb = LA_SP;
    if (g_la_regalloc) {
        /* `wide` here means "an eight-byte value", which fits a
         * register; to ra_allocate `wide` means "never eligible", and
         * only the sixteen-byte values are that (F.w16) -- handing it
         * the eight-byte map made every pointer and `long` ineligible on
         * RV64 once. fltmap is NULL: soft float lives in the integer
         * registers and must stay eligible for them. Under -g (and at
         * -O0) a source variable stays in its frame slot, so the
         * DW_AT_location naming that slot is true (see regalloc.h). */
        char *pin = want_debug || g_la_o0 ? ra_debug_pin_vars(fn)
                                          : (char *)0;
        F.loc = ra_allocate(fn, &LOONGARCH_RA, F.w16, pin,
                            F.used_callee, &F.nsave);
        free(pin);
        /* Read counts for comparison/branch fusion, with the allocator
         * on: without it every value goes through a slot and the
         * branch reads the slot, so "the only reader" would not hold. */
        if (fn->nvregs) {
            F.usecnt = xmalloc((size_t)fn->nvregs * sizeof *F.usecnt);
            ra_count_vreg_uses(fn, F.usecnt);
        }
        /* A BISECTION HANDLE. EMBCC_LA_RA_MAX=N leaves only the first N
         * vregs in registers and sends the rest back to memory, which
         * is always a correct thing to do -- so a miscompile that
         * survives at N and vanishes at N-1 names the value whose
         * allocation is wrong. */
        {
            const char *lim = getenv("EMBCC_LA_RA_MAX");
            if (lim) {
                int n = atoi(lim);
                for (int v = n; v < fn->nvregs; v++)
                    F.loc[v] = -1;
            }
        }
    }
    /* A variable-length array moves sp at run time, so the frame is
     * addressed from fp instead, which the prologue sets once the frame
     * is in place. fp is callee-saved and, in such a function, out of the
     * allocator's pool (la_pool_for), so it only has to be saved like any
     * other callee-saved register. */
    if (fn->has_alloca)
        F.used_callee[F.nsave++] = LA_FP;
    /* A leaf: no call in the IR and none the lowering makes -- the same
     * la_op_calls_helper the allocator trusts for which values survive a
     * call, so the two cannot disagree. Inline asm might call anything,
     * so it keeps ra saved. */
    /* A tail call leaves ra alone -- it is the caller's, and the callee
     * returns with it -- so it does not make this function a non-leaf. */
    F.tail = NULL;
    if (g_la_regalloc && !want_debug && !g_la_o0)
        for (i = 0; i < fn->nins; i++)
            if (la_tail_ok(&F, i)) {
                if (!F.tail)
                    F.tail = xcalloc((size_t)fn->nins, 1);
                F.tail[i] = 1;
            }
    F.sx = sext_map(&F);
    F.leaf = 1;
    for (i = 0; i < fn->nins; i++)
        if ((fn->ins[i].op == IR_CALL && !(F.tail && F.tail[i])) ||
            /* an asm writes ra when its template calls or names it,
             * which irgen recorded; one whose clobbers are unknown might */
            (fn->ins[i].op == IR_ASM && fn->ins[i].asm_ir &&
             !fn->ins[i].asm_ir->cont &&
             (!fn->ins[i].asm_ir->clob ||
              (fn->ins[i].asm_ir->clob >> 1 & 1))) ||
            la_op_calls_helper(&fn->ins[i]))
            F.leaf = 0;
    layout(&F);

    /* One more label than the IR has: the epilogue, which every IR_RET
     * jumps to so the frame size is written down once. */
    F.label_off = xmalloc((size_t)(fn->nlabels + 1) * sizeof *F.label_off);
    for (i = 0; i <= fn->nlabels; i++)
        F.label_off[i] = -1;

    /* BRANCH RELAXATION: the function is emitted with every branch in
     * its short form; any that does not reach is made the long form and
     * the function emitted again (see the branches section). Nothing else
     * emitted depends on a code address, so every pass makes the same
     * jumps and branches in the same order and the ordinal matches them.
     * Each pass only lengthens, so it ends. */
    {
    int len0 = t->len, nl0 = fn->nlines;
    int sc0 = F.st->ncall, se0 = F.st->next, ss0 = F.st->nstr,
        sg0 = F.st->ng, sf0 = F.st->nf;
    signed char *relax = NULL;
    int nrelax = 0;
    for (int pass = 0; ; pass++) {
    F.fb = LA_SP;
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
    /* Every instruction is four bytes and so is a jump table's entry, so
     * every function starts four-aligned. */
    if (t->len & 3)
        internal_error("loongarch: %s starts at an offset not a multiple "
                       "of four", fn->name);
    f->code_align = 4;
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
        if (la_fits(-F.frame, 12)) {
            la_alu_imm(t, LA_ADD, LA_SP, LA_SP, (int)-F.frame, 0);
        } else {
            la_li(t, LA_T0, -F.frame);
            la_alu(t, LA_ADD, LA_SP, LA_SP, LA_T0, 0);
        }
    }
    if (!F.leaf)
        st_sp(&F, LA_RA, F.ra_slot, F.w);
    for (i = 0; i < F.nsave; i++)
        st_sp(&F, F.used_callee[i], F.save_at + (long)i * F.w, F.w);
    if (fn->has_alloca) {
        la_mv(t, LA_FP, LA_SP);        /* the frame base, from here on */
        F.fb = LA_FP;
    }

    /* A variadic function spills EVERY argument register, named ones
     * included: the named ones are read out of the spill below, and the
     * unnamed ones have to be there for va_arg to walk into. */
    if (fn->is_varargs)
        for (int k = 0; k < LA_NARGREG; k++)
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
                    internal_error("loongarch: %s: a sixteen-byte parameter "
                                   "in a register", fn->name);
                } else if (a->size > F.w) {
                    /* A sixteen-byte scalar: two words, low first, to its
                     * slot (one may have come on the stack). */
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
                     * because la_pool_for hands a variadic function no
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
                        if (b) la_shift_imm(t, LA_SRL, r, r, 8, 0);
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
                        if (b) la_shift_imm(t, LA_SRL, SCR, SCR, 8, 0);
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
                internal_error("loongarch: %s: the prologue's parameter "
                               "placement is not a well-formed move",
                               fn->name);
            for (int k = 0; k < n; k++)
                la_mv(t, od[k], os[k]);
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
         * restores below then address from sp, because fp is one of the
         * registers they restore. */
        la_mv(t, LA_SP, LA_FP);
        F.fb = LA_SP;
    }
    la_restore(&F);
    la_ret(t);
    }

    {
    int again = 0;
    for (i = 0; i < F.nfix; i++) {
        int target = F.label_off[F.fix[i].label], ok = 1;
        if (target < 0)
            internal_error("loongarch: label %d of %s was never placed",
                           F.fix[i].label, fn->name);
        switch (F.fix[i].kind) {
        case FX_J: case FX_LONG:
            if (!la_patch_j(t, F.fix[i].at, target))
                internal_error("loongarch: %s: a jump does not reach its "
                               "label (128 MiB)", fn->name);
            break;
        case FX_B:
            ok = la_patch_b(t, F.fix[i].at, target);
            break;
        default:                                    /* FX_TAB */
            code_patch32(t, F.fix[i].at,
                         (unsigned long)(unsigned int)(target - F.fix[i].bat));
            break;
        }
        /* A short branch that does not reach: long on the next pass. */
        if (!ok) {
            if (!relax) {
                nrelax = F.nfix;
                relax = xcalloc((size_t)(nrelax ? nrelax : 1), 1);
                for (int k = 0; k < F.nfix; k++)
                    relax[k] = (signed char)F.fix[k].kind;
            }
            if (i >= nrelax)
                internal_error("loongarch: %s: the passes made different "
                               "branches", fn->name);
            relax[i] = FX_LONG;
            again = 1;
        }
    }
    if (!again)
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
    free(F.loc);
}

/* A field's constant offset into its load or store (ld.w r, rn, k) --
 * once, before allocation -- and then the function. */
static void gen_func_best(struct ir_func *fn, struct code *t,
                          struct la_sites *st, int want_debug)
{
    if (g_la_regalloc && !want_debug && !g_la_o0 &&
        !getenv("EMBCC_NO_MEMOFF"))
        ra_fold_memoff(fn, -2048, 2047, 8, 8, NULL, 0, 0);
    gen_func(fn, t, st, want_debug);
}

void codegen_unit_loongarch(struct ir_unit *iu, struct code *text,
                            struct extcall **ext, int *next,
                            struct strsite **strs, int *nstrs,
                            struct gsite **gs, int *ngs,
                            struct fsite **fs, int *nfs, int want_debug,
                            int optimize, int no_sse, int regalloc)
{
    struct la_sites st;

    (void)no_sse;
    g_la_o0 = !optimize;
    g_la_regalloc = regalloc;
    memset(&st, 0, sizeof st);

    for (int n = 0; n < iu->nfuncs; n++) {
        int ra = g_la_regalloc;
        if (g_la_o0 && ra_o0_too_big(&iu->funcs[n]))
            g_la_regalloc = 0;     /* see ra_o0_too_big */
        gen_func_best(&iu->funcs[n], text, &st, want_debug);
        g_la_regalloc = ra;
    }

    /* Intra-unit calls, now that every function has a place: `bl` (or
     * `b` for a tail call), +-128 MiB, which a unit's own text would have
     * to exceed. */
    for (int k = 0; k < st.ncall; k++) {
        int at = st.call[k].patch_off;
        if (!la_patch_j(text, at, (int)st.call[k].target->code_off))
            internal_error("loongarch: a call within the unit does not "
                           "reach %s (128 MiB)", st.call[k].target->name);
    }
    free(st.call);

    cg_resolve_strsites(iu, st.str, st.nstr);

    *ext = st.ext;   *next = st.next;
    *strs = st.str;  *nstrs = st.nstr;
    *gs = st.g;      *ngs = st.ng;
    *fs = st.f;      *nfs = st.nf;
}
