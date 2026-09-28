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
#define T_ADDR 10
#define T_SCR   9

#define A_LO T_ACC      /* r12 */
#define A_HI T_TMP      /* r11 */
#define B_LO T_ADDR     /* r10 */
#define B_HI T_SCR      /* r9  */

/* The registers the prologue saves. Four, not three, because AAPCS32
 * wants sp eight-byte aligned and `push` of an odd count would break it
 * — and because r9 then costs nothing and is a fourth scratch if this
 * file ever wants one. */
#define SAVE_MASK ((1u << 9) | (1u << T_ADDR) | (1u << T_TMP) | (1u << T_LR))

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
static unsigned save_mask_for(int nsave, const int *used)
{
    unsigned m = SAVE_MASK;
    int n = 4;
    for (int k = 0; k < nsave; k++) {
        m |= 1u << used[k];
        n++;
    }
    if (n & 1)
        m |= 1u << T_ACC;      /* r12: the pad */
    return m;
}

/* The bytes that mask moves sp by -- what the stack-parameter offsets
 * are measured from, so it must be the same answer. */
static long save_bytes_for(int nsave, const int *used)
{
    unsigned m = save_mask_for(nsave, used);
    long n = 0;
    for (int r = 0; r < 16; r++)
        if (m & (1u << r)) n++;
    return n * 4;
}

struct t_sites {
    struct { int patch_off; struct func *target; } *call;
    int ncall, capcall;
    struct extcall *ext;   int next, capext;
    struct strsite *str;   int nstr, capstr;
    struct gsite *g;       int ng, capg;
    struct fsite *f;       int nf, capf;
};

/* Per-function state. A struct rather than a pile of globals so that the
 * one thing a reader has to know about a function's lowering — where its
 * slots are and what is still unpatched — is in one place. */
struct t_fn {
    struct ir_func *fn;
    /* Comparison/branch fusion: how many times each vreg is READ, so a
     * comparison whose only reader is the branch after it can become
     * one `cmp` and one conditional branch instead of materialising 0
     * or 1 and testing that. `skip_next` tells the dispatch loop the
     * branch has already been emitted. The x86-64 backend has done
     * this from the start (usecnt there); ARMv7-M paid seven
     * instructions for every `if` without it. */
    int *usecnt;
    int skip_next;
    int want_debug;
    /* Per vreg: 1 when it holds a 64-bit integer, which on a 32-bit
     * machine is an eight-byte slot and a REGISTER PAIR. Built from the
     * width of each value's DEFINING instruction, which is not the same
     * as i->w everywhere -- a compare of two 64-bit values has w == 8
     * and produces a one-or-zero that is four bytes wide. */
    char *wide;
    struct code *t;
    struct t_sites *st;
    long *slot;          /* per-vreg byte offset from sp, -1 for none */
    long frame;          /* total bytes sp moves down by */
    long scratch_at;     /* where fn->scratch_bytes begins */
    long sret_slot;      /* where the hidden result pointer is kept, or -1 */
    /* A variadic function's REGISTER SAVE AREA: where the prologue
     * spilled r0-r3 so that one pointer walks from them into the
     * caller's stack arguments. -1 when the function is not variadic. */
    long va_regsave;
    long va_first;       /* ... and the offset of the first UNNAMED one */
    int *label_off;      /* per label id, or -1 while unseen */
    struct { int at; int label; int cond; } *fix;
    int nfix, capfix;
    /* Per vreg: the register the allocator gave it, or -1 for one that
     * stays in memory. NULL when it did not run (-O0/-O1). */
    int *loc;
    int used_callee[RA_MAXPOOL];
    int nsave;           /* how many of those it took */
    long save_at;        /* where the prologue spilled them */
};

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

static const int *t_pool_for(const struct ir_func *fn, int *n)
{
    if (fn->is_varargs) {
        *n = 5;
        return T_POOL_VA;
    }
    *n = T_NPOOL;
    return T_POOL;
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

/* Which instructions become a CALL the IR does not show as one.
 * Everything floating point -- ARMv7-M's base profile has no FPU -- and
 * the 64-bit divides. A value live across one of these may not sit in a
 * caller-saved register. */
static int t_op_calls_helper(const struct ir_ins *i)
{
    if (i->flt)
        return i->op == IR_ADD || i->op == IR_SUB || i->op == IR_MUL ||
               i->op == IR_DIV || i->op == IR_CMP;
    if (i->op == IR_I2F || i->op == IR_F2I || i->op == IR_F2F)
        return 1;
    return (i->op == IR_DIV || i->op == IR_MOD) && i->w == 8;
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
    NULL, NULL    /* no FP class -- soft float, in the core registers */
};

/* -O2 and -Os: the allocator is on. */
static int g_t_regalloc;

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
            "embcc: %s:%d: error: the ARMv7-M backend cannot lower %s yet "
            "(function %s)%s\n",
            fn->file ? fn->file : "?", i ? i->line : fn->line, what,
            fn->name, op);
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
        case IR_NEG: case IR_BNOT:
        case IR_LDVAR: case IR_LOAD: case IR_EXT: case IR_CALL:
        case IR_SELECT:
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
struct argplace { int reg, nreg, nstk; long stk; };

static void place_arg(int size, int align, int *ncrn, long *stk,
                      struct argplace *p)
{
    int words = (size + 3) / 4;

    if (align >= 8) {
        *ncrn = (*ncrn + 1) & ~1;
        *stk = (*stk + 7) & ~7L;
    }
    p->reg = *ncrn;
    p->nreg = *ncrn < 4 ? (words < 4 - *ncrn ? words : 4 - *ncrn) : 0;
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

static int fn_sret_bytes(const struct ir_func *fn)
{
    return fn->ret_abi.is_struct ? sret_bytes(fn->ret_abi.size) : 0;
}

/* What an argument's alignment is for placement purposes. A composite
 * carries its own; a SCALAR does not, and an eight-byte one is
 * eight-aligned — which is what rounds the register number up to even.
 * Asking only composites (an earlier shape of this) put `long long` in
 * whichever register came next, so f(int, long long, ...) passed it in
 * r1:r2 where every other toolchain passes it in r2:r3. */
static int arg_align(const struct ir_arg *a)
{
    if (a->is_struct)
        return a->align ? a->align : 4;
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
    int ncrn = 0;
    long stk = 0;
    /* A returned composite takes r0 for the hidden pointer, which is
     * what shifts every declared parameter along one. */
    if (fn->ret_abi.is_struct && fn->ret_abi.size > 4)
        ncrn = 1;
    for (int p = 0; f && p < fn->nparams && p < fn->nvregs; p++) {
        struct ir_arg *a = &fn->param_abi[p];
        struct argplace pl;
        place_arg(a->size, arg_align(a), &ncrn, &stk, &pl);
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
    }
    /* A call's arguments, placed by the same place_arg the call site
     * itself uses -- so no second copy of AAPCS32 is stated here. */
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        int cn = 0;
        long cstk = 0;
        if (i->op != IR_CALL)
            continue;
        if (sret_bytes(i->retsize))
            cn = 1;
        for (int k = 0; k < i->nargs; k++) {
            const struct ir_arg *a = &i->argv[k];
            struct argplace pl;
            place_arg(a->size, arg_align(a), &cn, &cstk, &pl);
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
        int ncrn = 0;
        long stk = 0;
        if (i->op != IR_CALL)
            continue;
        if (sret_bytes(i->retsize))
            ncrn = 1;                  /* r0 holds the result's address */
        for (int k = 0; k < i->nargs; k++)
            place_arg(i->argv[k].size, arg_align(&i->argv[k]),
                      &ncrn, &stk, &pl);
        if (stk > most)
            most = stk;
    }
    return (most + 7) & ~7L;
}

static void layout(struct t_fn *F)
{
    struct ir_func *fn = F->fn;
    long off = outgoing_area(fn);

    F->slot = xmalloc((size_t)(fn->nvregs ? fn->nvregs : 1) * sizeof *F->slot);
    for (int v = 0; v < fn->nvregs; v++)
        F->slot[v] = -1;

    for (int v = 0; v < fn->nvars; v++) {
        int size, align;
        /* A LOCAL in a register needs no slot either. The allocator
         * only ever gives one to a local whose address is never taken
         * (anything else is opaque to it), and under -g every local is
         * pinned to its slot so this cannot fire -- which is what keeps
         * DW_AT_location true. */
        if (F->loc && F->loc[v] >= 0)
            continue;
        size = fn->locals[v].size ? fn->locals[v].size : 4;
        align = fn->locals[v].user_align ? fn->locals[v].user_align
              : fn->locals[v].align ? fn->locals[v].align : 4;
        if (align < 4) align = 4;
        off = (off + align - 1) & ~(long)(align - 1);
        F->slot[v] = off;
        off += size;
    }
    for (int v = fn->nvars; v < fn->nvregs; v++) {
        /* A TEMPORARY the allocator put in a register needs no slot.
         * layout() runs after ra_allocate for exactly this reason, and
         * every helper that would write one (wr, wrote, wr64) already
         * asks in_reg first and returns without touching the slot -- so
         * the slot was reserved, aligned and paid for in `sub sp` and
         * then never read or written.
         *
         * It is worth real bytes: `int f(int a,int b){return a+b;}` had
         * a 24-byte frame for two values that were both in registers,
         * and the sub/add pair around it. Only temporaries, though:
         * a LOCAL keeps its slot, because its address can be taken and
         * because -g describes it by that slot.
         *
         * `F->slot[v]` stays -1 for these, which wr() already treats as
         * "nowhere to store" and skips. */
        int size;
        if (F->loc && F->loc[v] >= 0)
            continue;
        /* Eight-byte values are eight-ALIGNED as well as eight wide:
         * AAPCS32 aligns `long long` to 8, and a pair straddling that
         * boundary would be legal but slower and would break `ldrd` if
         * this ever emits one. */
        size = F->wide[v] ? 8 : 4;
        off = (off + size - 1) & ~(long)(size - 1);
        F->slot[v] = off;
        off += size;
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
static void rd(struct t_fn *F, int v, int reg)
{
    if (in_reg(F, v)) {
        if (F->loc[v] != reg)
            t_mov_reg(F->t, reg, F->loc[v]);
        return;
    }
    if (!t_ldst_imm(F->t, reg, T_SP, F->slot[v], 4, 0, 0)) {
        t_mov_imm(F->t, reg, F->slot[v], 0);
        t_ldst_reg(F->t, reg, T_SP, reg, 0, 4, 0, 0);
    }
}

/* `rdr` says where a value already IS; `wreg` where to compute a result;
 * `wrote` commits it only if that was a scratch. */
static int rdr(struct t_fn *F, int v, int scratch)
{
    if (in_reg(F, v))
        return F->loc[v];
    rd(F, v, scratch);
    return scratch;
}

static int wreg(struct t_fn *F, int v, int scratch)
{
    return in_reg(F, v) ? F->loc[v] : scratch;
}

static void wr(struct t_fn *F, int v, int reg)
{
    if (in_reg(F, v)) {
        if (F->loc[v] != reg)
            t_mov_reg(F->t, F->loc[v], reg);
        return;
    }
    if (F->slot[v] < 0)
        return;
    if (!t_ldst_imm(F->t, reg, T_SP, F->slot[v], 4, 0, 1)) {
        /* The offset does not reach: compute the address in a scratch
         * that is not the value being stored. */
        int a = reg == T_ADDR ? T_TMP : T_ADDR;
        t_mov_imm(F->t, a, F->slot[v], 0);
        t_alu_reg(F->t, T_OP_ADD, a, T_SP, a, 0);
        t_ldst_imm(F->t, reg, a, 0, 4, 0, 1);
    }
}

static void wrote(struct t_fn *F, int v, int reg)
{
    wr(F, v, reg);      /* wr already does the right thing either way */
}

/* A 64-bit value's two halves, little-endian: the low word at the slot
 * and the high word four bytes above it. */
static void rd64(struct t_fn *F, int v, int lo, int hi)
{
    if (!t_ldst_imm(F->t, lo, T_SP, F->slot[v], 4, 0, 0) ||
        !t_ldst_imm(F->t, hi, T_SP, F->slot[v] + 4, 4, 0, 0)) {
        t_add_sp(F->t, hi, F->slot[v]);
        t_ldst_imm(F->t, lo, hi, 0, 4, 0, 0);
        t_ldst_imm(F->t, hi, hi, 4, 4, 0, 0);
    }
}

static void wr64(struct t_fn *F, int v, int lo, int hi)
{
    if (F->slot[v] < 0)
        return;
    if (!t_ldst_imm(F->t, lo, T_SP, F->slot[v], 4, 0, 1) ||
        !t_ldst_imm(F->t, hi, T_SP, F->slot[v] + 4, 4, 0, 1)) {
        int a = (lo == T_SCR || hi == T_SCR) ? T_ADDR : T_SCR;
        t_add_sp(F->t, a, F->slot[v]);
        t_ldst_imm(F->t, lo, a, 0, 4, 0, 1);
        t_ldst_imm(F->t, hi, a, 4, 4, 0, 1);
    }
}

/* The second operand of a 64-bit binary operation, immediate or not. */
static void operand_b64(struct t_fn *F, const struct ir_ins *i, int lo, int hi)
{
    if (i->imm_b) {
        t_mov_imm(F->t, lo, (long)(i->imm & 0xffffffffL), 0);
        t_mov_imm(F->t, hi, (long)((i->imm >> 32) & 0xffffffffL), 0);
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
    if (i->imm_b)
        t_mov_imm(F->t, reg, (long)i->imm, 0);
    else
        rd(F, i->b, reg);
}

/* The address of a local's slot, into `reg`. */
static void addr_of_slot(struct t_fn *F, int v, int reg)
{
    t_add_sp(F->t, reg, F->slot[v]);
}

/* ---- branches ------------------------------------------------------- */

static void want_label(struct t_fn *F, int at, int label, int cond)
{
    if (F->nfix == F->capfix) {
        F->capfix = F->capfix ? F->capfix * 2 : 16;
        F->fix = xrealloc(F->fix, (size_t)F->capfix * sizeof *F->fix);
    }
    F->fix[F->nfix].at = at;
    F->fix[F->nfix].label = label;
    F->fix[F->nfix].cond = cond;
    F->nfix++;
}

static void jump_to(struct t_fn *F, int label)
{
    want_label(F, t_b(F->t), label, -1);
}

static void jump_if(struct t_fn *F, int cond, int label)
{
    want_label(F, t_bcond(F->t, cond), label, cond);
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
static void fp_args2(struct t_fn *F, const struct ir_ins *i)
{
    if (i->w == 8) {
        rd64(F, i->a, T_R0, T_R1);
        rd64(F, i->b, T_R2, T_R3);
        return;
    }
    {
        int pd[2], ps[2], npm = 0;
        if (in_reg(F, i->a)) { pd[npm] = T_R0; ps[npm] = F->loc[i->a]; npm++; }
        if (in_reg(F, i->b)) { pd[npm] = T_R1; ps[npm] = F->loc[i->b]; npm++; }
        if (npm) {
            int od[8], os[8];
            int m = ra_parallel_move(pd, ps, npm, T_SCR, od, os, 8);
            if (m < 0)
                internal_error("thumb: a helper's argument setup is not a "
                               "well-formed move");
            for (int k = 0; k < m; k++)
                t_mov_reg(F->t, od[k], os[k]);
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

/* Lower one 64-bit instruction. Returns 0 for one this does not handle,
 * which the caller then refuses by name. */
static int gen_ins64(struct t_fn *F, int n)
{
    struct ir_func *fn = F->fn;
    struct ir_ins *i = &fn->ins[n];
    struct code *t = F->t;

    switch (i->op) {
    case IR_CONST:
        t_mov_imm(t, A_LO, (long)(i->imm & 0xffffffffL), 0);
        t_mov_imm(t, A_HI, (long)((i->imm >> 32) & 0xffffffffL), 0);
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    /* This target has no floating-point register file: a double
     * already lives in a general register pair, so reinterpreting
     * its bits is a copy and nothing else. */
    case IR_BITCAST:
    case IR_MOV:
        rd64(F, i->a, A_LO, A_HI);
        wr64(F, i->dst, A_LO, A_HI);
        return 1;

    case IR_ADD: case IR_SUB:
        rd64(F, i->a, A_LO, A_HI);
        operand_b64(F, i, B_LO, B_HI);
        /* The carry must survive from one instruction to the next, so
         * nothing may come between them -- which is why both operands
         * are fully in registers before either is emitted. */
        if (i->op == IR_ADD) {
            t_alu_reg(t, T_OP_ADD, A_LO, A_LO, B_LO, 1);
            t_alu_reg(t, T_OP_ADC, A_HI, A_HI, B_HI, 1);
        } else {
            t_alu_reg(t, T_OP_SUB, A_LO, A_LO, B_LO, 1);
            t_alu_reg(t, T_OP_SBC, A_HI, A_HI, B_HI, 1);
        }
        wr64(F, i->dst, A_LO, A_HI);
        return 1;

    case IR_AND: case IR_OR: case IR_XOR: {
        int op = i->op == IR_AND ? T_OP_AND
               : i->op == IR_OR  ? T_OP_ORR : T_OP_EOR;
        rd64(F, i->a, A_LO, A_HI);
        operand_b64(F, i, B_LO, B_HI);
        t_alu_reg(t, op, A_LO, A_LO, B_LO, 0);
        t_alu_reg(t, op, A_HI, A_HI, B_HI, 0);
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    }

    case IR_BNOT:
        rd64(F, i->a, A_LO, A_HI);
        t_mvn_reg(t, A_LO, A_LO, 0);
        t_mvn_reg(t, A_HI, A_HI, 0);
        wr64(F, i->dst, A_LO, A_HI);
        return 1;

    case IR_NEG:
        /* 0 - a. `rsbs` leaves C clear exactly when the low word
         * borrowed, and `sbc` from zero is the high half. */
        rd64(F, i->a, A_LO, A_HI);
        t_mov_imm(t, B_LO, 0, 0);
        t_alu_imm(t, T_OP_RSB, A_LO, A_LO, 0, 1);
        t_alu_reg(t, T_OP_SBC, A_HI, B_LO, A_HI, 0);
        wr64(F, i->dst, A_LO, A_HI);
        return 1;

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
        if (i->size == 8) {
            rd64(F, i->a, A_LO, A_HI);
        } else {
            if (!t_ldst_imm(t, A_LO, T_SP, F->slot[i->a], i->size, i->sign,
                            0)) {
                t_add_sp(t, B_LO, F->slot[i->a]);
                t_ldst_imm(t, A_LO, B_LO, 0, i->size, i->sign, 0);
            }
            if (i->sign) t_shift_imm(t, T_SH_ASR, A_HI, A_LO, 31, 0);
            else         t_mov_imm(t, A_HI, 0, 0);
        }
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    case IR_STVAR:
        rd64(F, i->a, A_LO, A_HI);
        if (i->size == 8) {
            wr64(F, i->dst, A_LO, A_HI);
        } else {                       /* a truncating store */
            if (!t_ldst_imm(t, A_LO, T_SP, F->slot[i->dst], i->size, 0, 1)) {
                t_add_sp(t, B_LO, F->slot[i->dst]);
                t_ldst_imm(t, A_LO, B_LO, 0, i->size, 0, 1);
            }
        }
        return 1;
    case IR_LOAD:
        rd(F, i->a, B_LO);
        if (i->size == 8) {
            t_ldst_imm(t, A_LO, B_LO, 0, 4, 0, 0);
            t_ldst_imm(t, A_HI, B_LO, 4, 4, 0, 0);
        } else {
            t_ldst_imm(t, A_LO, B_LO, 0, i->size, i->sign, 0);
            if (i->sign) t_shift_imm(t, T_SH_ASR, A_HI, A_LO, 31, 0);
            else         t_mov_imm(t, A_HI, 0, 0);
        }
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    case IR_STORE:
        rd(F, i->a, B_LO);
        rd64(F, i->b, A_LO, A_HI);
        t_ldst_imm(t, A_LO, B_LO, 0, i->size == 8 ? 4 : i->size, 0, 1);
        if (i->size == 8)
            t_ldst_imm(t, A_HI, B_LO, 4, 4, 0, 1);
        return 1;

    case IR_SELECT:
        rd(F, i->a, B_LO);
        t_cmp_imm(t, B_LO, 0);
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
    if (swap) {
        operand_b64(F, i, A_LO, A_HI);
        rd64(F, i->a, B_LO, B_HI);
        pred = pred == B_GT ? B_LT : B_GE;
    } else {
        rd64(F, i->a, A_LO, A_HI);
        operand_b64(F, i, B_LO, B_HI);
    }
    if (pred == B_EQ || pred == B_NE) {
        /* Equality needs both halves, and `sbcs` only reports Z for the
         * high one -- so the difference of each half is folded together
         * and tested against zero. */
        t_alu_reg(t, T_OP_EOR, A_LO, A_LO, B_LO, 0);
        t_alu_reg(t, T_OP_EOR, A_HI, A_HI, B_HI, 0);
        t_alu_reg(t, T_OP_ORR, A_LO, A_LO, A_HI, 0);
        t_cmp_imm(t, A_LO, 0);
        return pred == B_EQ ? T_EQ : T_NE;
    }
    t_alu_reg(t, T_OP_SUB, A_LO, A_LO, B_LO, 1);
    t_alu_reg(t, T_OP_SBC, A_HI, A_HI, B_HI, 1);
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
    if (in_reg(F, v)) {
        /* The value is in a CORE register: one move, no memory. */
        t_vmov_core(F->t, sreg, F->loc[v], 1);
        return;
    }
    if (F->slot[v] >= 0 && F->slot[v] <= 1020 && !(F->slot[v] & 3)) {
        t_vldst(F->t, sreg, T_SP, (int)F->slot[v], 0, 0);
        return;
    }
    rd(F, v, T_ACC);                    /* the general path, via a core reg */
    t_vmov_core(F->t, sreg, T_ACC, 1);
}

static void vfp_store(struct t_fn *F, int v, int sreg)
{
    if (v < 0)
        return;
    if (in_reg(F, v)) {
        t_vmov_core(F->t, sreg, F->loc[v], 0);
        return;
    }
    if (F->slot[v] >= 0 && F->slot[v] <= 1020 && !(F->slot[v] & 3)) {
        t_vldst(F->t, sreg, T_SP, (int)F->slot[v], 0, 1);
        return;
    }
    t_vmov_core(F->t, sreg, T_ACC, 0);
    wr(F, v, T_ACC);
}

/* Returns 1 when this instruction was emitted on the FPU, 0 to fall
 * through to the soft-float helper. Saying 0 rather than refusing is
 * deliberate: an operation the FPU cannot do is not an error, it is a
 * call -- `double` on an SP-only part is the whole reason. */
static int fp_vfp_arith(struct t_fn *F, const struct ir_ins *i)
{
    struct code *t = F->t;
    if (i->imm_b)
        return 0;                       /* the helper path folds it */
    switch (i->op) {
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV:
        vfp_load(F, i->a, T_FS0);
        vfp_load(F, i->b, T_FS1);
        if (i->op == IR_ADD)      t_vadd(t, T_FS0, T_FS0, T_FS1, 0);
        else if (i->op == IR_SUB) t_vsub(t, T_FS0, T_FS0, T_FS1, 0);
        else if (i->op == IR_MUL) t_vmul(t, T_FS0, T_FS0, T_FS1, 0);
        else                      t_vdiv(t, T_FS0, T_FS0, T_FS1, 0);
        vfp_store(F, i->dst, T_FS0);
        return 1;
    case IR_NEG:
        vfp_load(F, i->a, T_FS0);
        t_vneg(t, T_FS0, T_FS0, 0);
        vfp_store(F, i->dst, T_FS0);
        return 1;
    case IR_SQRT:
        vfp_load(F, i->a, T_FS0);
        t_vsqrt(t, T_FS0, T_FS0, 0);
        vfp_store(F, i->dst, T_FS0);
        return 1;
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


/* ---- one instruction ------------------------------------------------ */

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
         * double still goes to __adddf3 below, on the same part.
         *
         * The values go through their slots rather than staying in FP
         * registers: there is no floating-point register class on this
         * target yet, so `vldr` both operands, one instruction, `vstr`
         * the result. That is already four to six instructions where the
         * helper call was a dozen plus the call itself, and it is
         * CORRECT before it is fast -- the register class is the next
         * step and does not change what is computed. */
        if (target_thumb_fpu() && i->w == 4 && fp_vfp_arith(F, i))
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
            fp_args2(F, i);
            call_helper(F, fp_cmp_name(i->pred, i->w));
            t_cmp_imm(t, T_R0, 0);
            cond = cond_for(i->pred, 1);      /* the helper's signed answer */
            t_mov_imm(t, T_ACC, 1, 0);
            {
                int over = t_bcond(t, cond);
                t_mov_imm(t, T_ACC, 0, 0);
                t_patch_bcond(t, over, t->len);
            }
            wr(F, i->dst, T_ACC);
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
                rd64(F, i->a, T_R0, T_R1);
                operand_b64(F, i, T_R2, T_R3);
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
        return;
    case IR_JMP:
        /* A jump to the label that follows it is not an instruction.
         * Four bytes each and the IR is full of them, because every
         * `if` without an `else` ends in one. */
        if (n + 1 < fn->nins && fn->ins[n + 1].op == IR_LABEL &&
            fn->ins[n + 1].label == i->label)
            return;
        jump_to(F, i->label);
        return;
    case IR_CONST: {
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
        int srcr = rdr(F, i->a, T_ACC);
        wr(F, i->dst, srcr);
        return;
    }

    case IR_ADD: case IR_SUB: case IR_MUL:
    case IR_AND: case IR_OR:  case IR_XOR: {
        /* A switch and not a table indexed by (i->op - IR_ADD): IR_DIV
         * and IR_MOD sit between IR_MUL and IR_AND, so a six-entry table
         * turns `and` into `eor` and reads past its end for `or` and
         * `xor`. It compiled, it ran, and `v & 1` came back as v & ~1. */
        int op = i->op == IR_ADD ? T_OP_ADD
               : i->op == IR_SUB ? T_OP_SUB
               : i->op == IR_AND ? T_OP_AND
               : i->op == IR_OR  ? T_OP_ORR
               : i->op == IR_XOR ? T_OP_EOR
               : 0;                          /* IR_MUL: not an ALU op */
        int ra_ = rdr(F, i->a, T_ACC);
        int d = wreg(F, i->dst, T_ACC);
        if (i->imm_b && i->op != IR_MUL) {
            /* addw/subw reach any 0..4095 where the modified immediate
             * reaches only what it can rotate into place, and almost
             * every constant folded here is a small offset. */
            if ((i->op == IR_ADD || i->op == IR_SUB) &&
                i->imm >= 0 && i->imm <= 4095) {
                if (i->op == IR_ADD) t_addw(t, d, ra_, i->imm);
                else                 t_subw(t, d, ra_, i->imm);
            } else if (!t_alu_imm(t, op, d, ra_, i->imm, 0)) {
                int rb_ = (!in_reg(F, i->b)) ? T_TMP : F->loc[i->b];
                if (rb_ == T_TMP) operand_b(F, i, T_TMP);
                t_alu_reg(t, op, d, ra_, rb_, 0);
            }
            wrote(F, i->dst, d);
            return;
        }
        {
            /* The second operand is pinned to a scratch unless it is
             * already in a register, so a load of it cannot land in the
             * destination before the operation reads it. */
            int rb_ = (i->imm_b || !in_reg(F, i->b)) ? T_TMP : F->loc[i->b];
            if (rb_ == T_TMP) operand_b(F, i, T_TMP);
            if (i->op == IR_MUL)
                t_mul(t, d, ra_, rb_);
            else
                t_alu_reg(t, op, d, ra_, rb_, 0);
        }
        wrote(F, i->dst, d);
        return;
    }
    case IR_DIV: case IR_MOD:
        rd(F, i->a, T_ACC);
        operand_b(F, i, T_TMP);
        t_div(t, T_ADDR, T_ACC, T_TMP, i->sign);
        if (i->op == IR_MOD) {
            /* There is no remainder instruction: r = a - (a / b) * b,
             * which `mls` does in one. */
            t_mls(t, T_ACC, T_ADDR, T_TMP, T_ACC);
            wr(F, i->dst, T_ACC);
        } else {
            wr(F, i->dst, T_ADDR);
        }
        return;
    case IR_SHL: case IR_SHR: {
        int sh = i->op == IR_SHL ? T_SH_LSL : i->sign ? T_SH_ASR : T_SH_LSR;
        int sa = rdr(F, i->a, T_ACC);
        int d = wreg(F, i->dst, T_ACC);
        if (i->imm_b && i->imm >= 0 && i->imm < 32) {
            t_shift_imm(t, sh, d, sa, (int)i->imm, 0);
        } else {
            operand_b(F, i, T_TMP);
            t_shift_reg(t, sh, d, sa, T_TMP, 0);
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
        if (i->w == 8) {
            cond = cmp64(F, i, i->pred, i->sign);
            if (fuse) {
                jump_if(F, nx->op == IR_BRNZ ? cond : (cond ^ 1), nx->label);
                F->skip_next = 1;
                return;
            }
            t_mov_imm(t, T_ACC, 1, 0);
            {
                int over = t_bcond(t, cond);
                t_mov_imm(t, T_ACC, 0, 0);
                t_patch_bcond(t, over, t->len);
            }
            wr(F, i->dst, T_ACC);
            return;
        }
        {
        /* The comparison reads its left operand where it already is;
         * only the 0/1 result needs a register of its own. */
        int sa = rdr(F, i->a, T_ACC);
        if (i->imm_b && ((i->imm >= 0 && i->imm <= 255) || t_imm_ok(i->imm))) {
            t_cmp_imm(t, sa, i->imm);
        } else {
            operand_b(F, i, T_TMP);
            t_cmp_reg(t, sa, T_TMP);
        }
        }
        if (fuse) {
            jump_if(F, nx->op == IR_BRNZ ? cond : (cond ^ 1), nx->label);
            F->skip_next = 1;
            return;
        }
        /* 0 or 1, without an IT block: set it, then jump over the
         * clear. Two instructions either way, and no flag-liveness
         * question to get wrong. */
        t_mov_imm(t, T_ACC, 1, 0);
        {
            int over = t_bcond(t, cond);
            t_mov_imm(t, T_ACC, 0, 0);
            t_patch_bcond(t, over, t->len);
        }
        wr(F, i->dst, T_ACC);
        return;
    }
    case IR_SELECT: {
        /* dst = a ? b : c. Thumb has conditional execution through an IT
         * block, but both arms here are already-computed VALUES sitting
         * in slots, so this is two loads and a branch over one of them —
         * which needs no flag-liveness reasoning and is the same size. */
        rd(F, i->a, T_ACC);
        t_cmp_imm(t, T_ACC, 0);
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
        if (i->w == 8) {
            rd64(F, i->a, A_LO, A_HI);
            t_alu_reg(t, T_OP_ORR, A_LO, A_LO, A_HI, 0);
            t_cmp_imm(t, A_LO, 0);
        } else {
            rd(F, i->a, T_ACC);
            t_cmp_imm(t, T_ACC, 0);
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
        d = wreg(F, i->dst, T_ACC);
        if (in_reg(F, i->a)) {
            if (t_ldvar_plain(i->size, i->sign, i->w)) {
                if (d != F->loc[i->a]) t_mov_reg(t, d, F->loc[i->a]);
            } else {
                t_ext(t, d, F->loc[i->a], i->size, i->sign);
            }
        } else if (!t_ldst_imm(t, d, T_SP, F->slot[i->a], i->size,
                               i->sign, 0)) {
            t_add_sp(t, T_ADDR, F->slot[i->a]);
            t_ldst_imm(t, d, T_ADDR, 0, i->size, i->sign, 0);
        }
        wrote(F, i->dst, d);
        return;
    }
    case IR_STVAR: {
        int src;
        if (i->w > 4) t_refuse(fn, i, "a 64-bit local");
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
        } else if (!t_ldst_imm(t, src, T_SP, F->slot[i->dst], i->size, 0, 1)) {
            t_add_sp(t, T_ADDR, F->slot[i->dst]);
            t_ldst_imm(t, src, T_ADDR, 0, i->size, 0, 1);
        }
        return;
    }
    case IR_LOAD: {
        /* `ldr rd, [rn]` with rd == rn is legal, so the destination
         * may share the address's register; nothing has to be kept
         * apart here. */
        int an = rdr(F, i->a, T_ADDR);
        int d = wreg(F, i->dst, T_ACC);
        if (i->w > 4) t_refuse(fn, i, "a 64-bit load");
        t_ldst_imm(t, d, an, 0, i->size, i->sign, 0);
        wrote(F, i->dst, d);
        return;
    }
    case IR_STORE: {
        int an, vr;
        if (i->w > 4) t_refuse(fn, i, "a 64-bit store");
        an = rdr(F, i->a, T_ADDR);
        /* The value must not land in the register the address is in
         * when that register is the scratch -- rdr would overwrite it. */
        vr = rdr(F, i->b, an == T_ACC ? T_TMP : T_ACC);
        t_ldst_imm(t, vr, an, 0, i->size, 0, 1);
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
        int d = wreg(F, i->dst, T_ACC);
        addr_of_slot(F, i->a, d);
        wrote(F, i->dst, d);
        return;
    }
    case IR_STRADDR:
        note_str(F->st, t_mov_addr(t, T_ACC, 0), i->label, RK_THM_MOVW);
        note_str(F->st, t->len - 4, i->label, RK_THM_MOVT);
        wr(F, i->dst, T_ACC);
        return;
    case IR_GADDR:
        note_glob(F->st, t_mov_addr(t, T_ACC, 0), i->glob, RK_THM_MOVW);
        note_glob(F->st, t->len - 4, i->glob, RK_THM_MOVT);
        wr(F, i->dst, T_ACC);
        return;
    case IR_FADDR:
        note_fn(F->st, t_mov_addr(t, T_ACC, 0), i->callee, RK_THM_MOVW);
        note_fn(F->st, t->len - 4, i->callee, RK_THM_MOVT);
        wr(F, i->dst, T_ACC);
        return;

    case IR_MEMCPY: case IR_MEMZERO: {
        /* A byte loop, unrolled to words where the size allows. Small
         * and obviously right; a tuned copy is a later question. */
        long size = i->size, k;
        rd(F, i->a, T_ADDR);
        if (i->op == IR_MEMCPY) {
            rd(F, i->b, T_TMP);
            for (k = 0; k + 4 <= size; k += 4) {
                t_ldst_imm(t, T_ACC, T_TMP, k, 4, 0, 0);
                t_ldst_imm(t, T_ACC, T_ADDR, k, 4, 0, 1);
            }
            for (; k < size; k++) {
                t_ldst_imm(t, T_ACC, T_TMP, k, 1, 0, 0);
                t_ldst_imm(t, T_ACC, T_ADDR, k, 1, 0, 1);
            }
        } else {
            t_mov_imm(t, T_ACC, 0, 0);
            for (k = 0; k + 4 <= size; k += 4)
                t_ldst_imm(t, T_ACC, T_ADDR, k, 4, 0, 1);
            for (; k < size; k++)
                t_ldst_imm(t, T_ACC, T_ADDR, k, 1, 0, 1);
        }
        return;
    }

    case IR_CALL: {
        /* AAPCS32, the scalar half: r0-r3 in order, then four-byte stack
         * slots from sp. An eight-byte argument would round the register
         * number up to even and take two — refused above with everything
         * else 64-bit, so the placement here stays the simple one. */
        int ncrn = 0;
        long stk = 0;
        long sret = sret_bytes(i->retsize);
        /* The STACK words first, then the registers: writing a stack
         * argument needs a scratch, and by the time r0-r3 are loaded
         * there is none left that is not already an argument. */
        struct argplace pl[MAX_PARAMS];
        if (sret)
            ncrn = 1;
        for (int k = 0; k < i->nargs; k++) {
            struct ir_arg *a = &i->argv[k];
            /* A struct of floats needs no special case either: the
             * base AAPCS standard has no homogeneous-aggregate rule —
             * that is the VFP variant's — so it travels in core
             * registers like any other composite. */
            place_arg(a->size, arg_align(a), &ncrn, &stk, &pl[k]);
        }
        for (int k = 0; k < i->nargs; k++) {
            struct ir_arg *a = &i->argv[k];
            if (!pl[k].nstk)
                continue;
            /* A composite's vreg holds its ADDRESS; a scalar's holds
             * the value, and a scalar never splits. */
            if (a->is_struct) {
                rd(F, a->vreg, T_ADDR);
                for (int q = 0; q < pl[k].nstk; q++) {
                    long off = (long)(pl[k].nreg + q) * 4;
                    int last = off + 4 > a->size;
                    /* The tail of an odd-sized struct is copied byte by
                     * byte: reading a whole word past the end of the
                     * object would be a load nothing put there. */
                    if (last && (a->size & 3)) {
                        for (long b = off; b < a->size; b++) {
                            t_ldst_imm(t, T_ACC, T_ADDR, b, 1, 0, 0);
                            t_ldst_imm(t, T_ACC, T_SP,
                                       pl[k].stk + (long)q * 4 + (b - off),
                                       1, 0, 1);
                        }
                    } else {
                        t_ldst_imm(t, T_ACC, T_ADDR, off, 4, 0, 0);
                        t_ldst_imm(t, T_ACC, T_SP, pl[k].stk + (long)q * 4,
                                   4, 0, 1);
                    }
                }
            } else if (a->size > 4) {
                rd64(F, a->vreg, T_ACC, T_TMP);
                t_ldst_imm(t, T_ACC, T_SP, pl[k].stk, 4, 0, 1);
                t_ldst_imm(t, T_TMP, T_SP, pl[k].stk + 4, 4, 0, 1);
            } else {
                rd(F, a->vreg, T_ACC);
                t_ldst_imm(t, T_ACC, T_SP, pl[k].stk, 4, 0, 1);
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
                if (!pl[k].nreg || a->is_struct || a->size > 4)
                    continue;
                if (!in_reg(F, a->vreg))
                    continue;
                pd[npm] = pl[k].reg;
                ps[npm] = F->loc[a->vreg];
                npm++;
            }
            if (npm) {
                int od[16], os[16];
                int m = ra_parallel_move(pd, ps, npm, T_SCR, od, os,
                                         (int)(sizeof od / sizeof od[0]));
                if (m < 0)
                    internal_error("thumb: a call's argument setup is not "
                                   "a well-formed move");
                for (int k = 0; k < m; k++)
                    t_mov_reg(t, od[k], os[k]);
            }
        }
        for (int k = 0; k < i->nargs; k++) {
            struct ir_arg *a = &i->argv[k];
            if (!pl[k].nreg)
                continue;
            /* Already placed by the parallel move above. */
            if (!a->is_struct && a->size <= 4 && in_reg(F, a->vreg))
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
                            t_ldst_imm(t, T_ACC, T_ADDR, b, 1, 0, 0);
                            t_alu_reg(t, T_OP_ORR, pl[k].reg + q,
                                      pl[k].reg + q, T_ACC, 0);
                        }
                    } else {
                        t_ldst_imm(t, pl[k].reg + q, T_ADDR, off, 4, 0, 0);
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
            t_add_sp(t, T_R0, F->scratch_at + i->scratch);
        if (i->indirect) {
            rd(F, i->a, T_ACC);
            t_blx(t, T_ACC);
        } else if (i->callee->has_defn) {
            note_call(F->st, t_bl(t), i->callee);
        } else {
            note_ext(F->st, t_bl(t), i->callee);
        }
        if (i->dst >= 0) {
            if (i->retsize) {
                /* dst receives the scratch's ADDRESS, which is the
                 * contract irgen shares with the other backends. A
                 * four-byte composite came back in r0 and has to be
                 * stored there first; a larger one the callee already
                 * wrote through the pointer. */
                if (!sret_bytes(i->retsize)) {
                    t_add_sp(t, T_ADDR, F->scratch_at + i->scratch);
                    t_ldst_imm(t, T_R0, T_ADDR, 0, i->retsize, 0, 1);
                }
                t_add_sp(t, T_ACC, F->scratch_at + i->scratch);
                wr(F, i->dst, T_ACC);
            } else if (F->wide[i->dst]) {
                wr64(F, i->dst, T_R0, T_R1);
            } else {
                wr(F, i->dst, T_R0);
            }
        }
        return;
    }

    case IR_RET:
        if (i->a >= 0 && fn->ret_abi.size && fn->ret_abi.is_struct) {
            /* `a` holds the ADDRESS of the composite being returned.
             * Four bytes or fewer come back in r0; anything larger is
             * copied to the buffer the caller named, whose address is
             * also what r0 must hold at the return. */
            long n = fn->ret_abi.size;
            rd(F, i->a, T_ADDR);
            if (F->sret_slot >= 0) {
                long k;
                t_ldst_imm(t, T_TMP, T_SP, F->sret_slot, 4, 0, 0);
                for (k = 0; k + 4 <= n; k += 4) {
                    t_ldst_imm(t, T_ACC, T_ADDR, k, 4, 0, 0);
                    t_ldst_imm(t, T_ACC, T_TMP, k, 4, 0, 1);
                }
                for (; k < n; k++) {
                    t_ldst_imm(t, T_ACC, T_ADDR, k, 1, 0, 0);
                    t_ldst_imm(t, T_ACC, T_TMP, k, 1, 0, 1);
                }
                t_mov_reg(t, T_R0, T_TMP);
            } else {
                /* Four bytes or fewer, in r0. A three-byte composite is
                 * read as a word: it is at least four-byte aligned and
                 * the high byte is padding the caller ignores. */
                t_ldst_imm(t, T_R0, T_ADDR, 0, n == 3 ? 4 : (int)n, 0, 0);
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
        if (n + 1 < fn->nins)
            jump_to(F, fn->nlabels);
        return;

    case IR_UD2:
        /* `udf #0` (0xde00): PERMANENTLY UNDEFINED, which is what this
         * op means. A load from address zero was standing in for it and
         * is not the same thing at all — on a Cortex-M address zero is
         * the vector table and the load succeeds, so a
         * __builtin_unreachable() that was reached carried on. */
        code_byte(t, 0x00);
        code_byte(t, 0xde);
        return;
    case IR_FENCE:
        /* dmb sy — a full data barrier. */
        code_byte(t, 0xbf); code_byte(t, 0xf3);
        code_byte(t, 0x5f); code_byte(t, 0x8f);
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
            if (i->w == 8) { rd64(F, i->a, A_LO, A_HI);
                             wr64(F, i->dst, A_LO, A_HI); }
            else           { rd(F, i->a, T_ACC); wr(F, i->dst, T_ACC); }
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
         * Nothing is live in a REGISTER across an asm, and that is not
         * an assumption: the shared allocator excludes every vreg whose
         * range spans an IR_ASM, because the clobber set is not visible
         * to it. So the operands' registers may be loaded freely. */
        struct ir_asm *ia = i->asm_ir;
        int used[16] = { 0 };
        /* The address scratch. r12 is the ABI's own and the only
         * register that is neither an argument nor callee-saved, so it
         * is tried first; r0-r3 after it, when an operand has taken it. */
        static const int scr_pool[] = { 12, 0, 1, 2, 3 };
        int scr = -1;
        for (int k = 0; k < ia->nin; k++) used[ia->in[k].reg] = 1;
        for (int k = 0; k < ia->nout; k++) used[ia->out[k].reg] = 1;
        for (unsigned k = 0; k < sizeof scr_pool / sizeof scr_pool[0]; k++)
            if (!used[scr_pool[k]]) { scr = scr_pool[k]; break; }
        if (scr < 0 && ia->nout > 0)
            t_refuse(fn, i, "an asm with no scratch register left around it");
        for (int k = 0; k < ia->nout; k++)
            if (!ia->out[k].mem && ia->out[k].size > 4)
                t_refuse(fn, i, "an asm output wider than a register");
        /* A "+" output starts with the lvalue's CURRENT value. */
        for (int k = 0; k < ia->nout; k++) {
            if (!ia->out[k].inout || ia->out[k].mem)
                continue;
            rd(F, ia->out[k].temp, scr);
            t_ldst_imm(t, ia->out[k].reg, scr, 0, ia->out[k].size, 0, 0);
        }
        for (int k = 0; k < ia->nin; k++)
            rd(F, ia->in[k].temp, ia->in[k].reg);
        for (int k = 0; k < ia->codelen; k++)
            code_byte(t, ia->code[k]);
        for (int k = 0; k < ia->nout; k++) {
            /* An "m" output was written BY the template through the
             * address this register holds; storing over it would destroy
             * what the asm produced. */
            if (ia->out[k].mem)
                continue;
            rd(F, ia->out[k].temp, scr);
            t_ldst_imm(t, ia->out[k].reg, scr, 0, ia->out[k].size, 0, 1);
        }
        return;
    }
    case IR_VA_START:
        /* `a` holds the ADDRESS of the va_list, which on this ABI is a
         * bare pointer at the next argument. */
        rd(F, i->a, T_ADDR);
        t_add_sp(t, T_ACC, F->va_first);
        t_ldst_imm(t, T_ACC, T_ADDR, 0, 4, 0, 1);
        return;
    case IR_ALLOCA: case IR_SPSAVE: case IR_SPRESTORE:
        t_refuse(fn, i, "a variable-length array");
        return;
    case IR_LANDING:
        t_refuse(fn, i, "an exception landing pad");
        return;
    case IR_IGOTO: case IR_LABELADDR:
        t_refuse(fn, i, "a computed goto");
        return;
    case IR_XCHG: case IR_XADD: case IR_CMPXCHG: case IR_ARMW:
    case IR_CAS: case IR_CAS16:
        t_refuse(fn, i, "an atomic operation");
        return;
    default:
        t_refuse(fn, i, "this operation");
        return;
    }
}

/* ---- one function --------------------------------------------------- */

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
    F.va_regsave = F.va_first = -1;
    F.loc = NULL; F.nsave = 0; F.save_at = 0;
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
        char *pin = want_debug ? ra_debug_pin_vars(fn) : (char *)0;
        F.loc = ra_allocate(fn, &THUMB_RA, F.wide, pin,
                            F.used_callee, &F.nsave);
        free(pin);
        /* Read counts for comparison/branch fusion. Only with the
         * allocator on: without it every value round-trips through a
         * slot and the branch reads the slot, so nothing is saved and
         * the "only reader" claim would not hold. */
        if (fn->nvregs) {
            F.usecnt = xmalloc((size_t)fn->nvregs * sizeof *F.usecnt);
            ra_count_vreg_uses(fn, F.usecnt);
        }
        {
            const char *lim = getenv("EMBCC_T_RA_MAX");
            if (lim) {
                int n = atoi(lim);
                int keep = 0;
                for (int v = n; v < fn->nvregs; v++)
                    F.loc[v] = -1;
                /* Keep only the saved registers still in use, so the
                 * gate really is "allocate less" and not "push registers
                 * for nothing". */
                for (int k = 0; k < F.nsave; k++) {
                    int used = 0;
                    for (int v = 0; v < fn->nvregs; v++)
                        if (F.loc[v] == F.used_callee[k]) { used = 1; break; }
                    if (used) F.used_callee[keep++] = F.used_callee[k];
                }
                F.nsave = keep;
            }
        }
    }
    layout(&F);

    /* One more label than the IR has: the epilogue, which every IR_RET
     * jumps to so the frame size is written down once. */
    F.label_off = xmalloc((size_t)(fn->nlabels + 1) * sizeof *F.label_off);
    for (i = 0; i <= fn->nlabels; i++)
        F.label_off[i] = -1;

    /* A Thumb function must start on a halfword, and four keeps the
     * literal loads and the disassembly tidy. */
    /* Pad with halfword NOPs (bf00), not with a repeated 0xbf: that
     * byte pairs into 0xbfbf, which is an `itttt` — harmless, since
     * nothing branches there, but it makes every disassembly of the gap
     * between two functions look like a condition block. */
    while (t->len & 3)
        t_nop(t);
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
    push_at = t_push(t, save_mask_for(F.nsave, F.used_callee));
    /* The mask is PATCHED at the end with whatever callee-saved
     * registers the allocator turned out to take: a `push` encodes them
     * as a bitmask, so growing the set costs no extra instruction, and
     * emitting the push before the body is what lets the frame layout be
     * decided first. This is what t_patch_push exists for. */
    if (F.frame)
        t_sp_adjust(t, F.frame, 1);

    /* The parameters arrive in r0-r3 and on the stack above the saved
     * registers; the prologue writes each to its slot, which is what
     * every later reference reads. */
    {
        struct argplace pl;
        int ncrn = 0;
        long stk = 0;
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
        long base = F.frame + save_bytes_for(F.nsave, F.used_callee);
        int pmv_dst[RA_MAXPOOL], pmv_src[RA_MAXPOOL], npmv = 0;
        int pstk_reg[RA_MAXPOOL]; long pstk_off[RA_MAXPOOL];
        int npstk = 0;
        if (fn->is_varargs) {
            F.va_regsave = base;            /* r0-r3, four words */
            base += 16;                     /* ... then the stack ones */
        }
        if (F.sret_slot >= 0) {
            t_ldst_imm(t, T_R0, T_SP, F.sret_slot, 4, 0, 1);
            ncrn = 1;
        }
        for (i = 0; i < fn->nparams; i++) {
            struct ir_arg *a = &fn->param_abi[i];
            place_arg(a->size, arg_align(a), &ncrn, &stk, &pl);
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
                if (!t_ldst_imm(t, pl.reg + q, T_SP, off, wid == 4 ? 4 : wid,
                                0, 1)) {
                    t_add_sp(t, T_ADDR, off);
                    t_ldst_imm(t, pl.reg + q, T_ADDR, 0, wid == 4 ? 4 : wid,
                               0, 1);
                }
            }
            for (int q = 0; q < pl.nstk; q++) {
                long src = base + pl.stk + (long)q * 4;
                long dst = F.slot[i] + (long)(pl.nreg + q) * 4;
                t_ldst_imm(t, T_ACC, T_SP, src, 4, 0, 0);
                if (!t_ldst_imm(t, T_ACC, T_SP, dst, 4, 0, 1)) {
                    t_add_sp(t, T_ADDR, dst);
                    t_ldst_imm(t, T_ACC, T_ADDR, 0, 4, 0, 1);
                }
            }
        }
        /* The parallel move, now that every parameter has been placed.
         * T_SCR (r9) breaks a cycle: it is scratch, the prologue has
         * already saved it, and it holds nothing of its own yet. */
        if (npmv) {
            int od[RA_MAXPOOL * 2], os[RA_MAXPOOL * 2];
            int n = ra_parallel_move(pmv_dst, pmv_src, npmv, T_SCR, od, os,
                                     (int)(sizeof od / sizeof od[0]));
            if (n < 0)
                internal_error("thumb: %s: the prologue's parameter "
                               "placement is not a well-formed move",
                               fn->name);
            for (int k = 0; k < n; k++)
                t_mov_reg(t, od[k], os[k]);
        }
        /* Then the loads: they only WRITE, so by now nothing still needs
         * the old contents of an argument register. */
        for (int k = 0; k < npstk; k++)
            if (!t_ldst_imm(t, pstk_reg[k], T_SP, pstk_off[k], 4, 0, 0)) {
                t_add_sp(t, T_ADDR, pstk_off[k]);
                t_ldst_imm(t, pstk_reg[k], T_ADDR, 0, 4, 0, 0);
            }

        /* Where the first UNNAMED argument sits — which is simply where
         * the named ones stopped. The save area and the caller's stack
         * arguments are contiguous, so one expression covers both
         * cases: below four named words it is inside the save area, and
         * at four it is exactly its end, which is the stack. */
        if (fn->is_varargs)
            F.va_first = F.va_regsave + (long)ncrn * 4 + stk;
    }

    for (i = 0; i < fn->nins; i++) {
        gen_ins(&F, i);
        if (F.skip_next) {      /* the comparison emitted its branch too */
            F.skip_next = 0;
            i++;
        }
    }

    /* The epilogue. */
    F.label_off[fn->nlabels] = t->len;
    if (F.frame)
        t_sp_adjust(t, F.frame, 0);
    {
        /* The same set the prologue pushed: SAVE_MASK plus whatever
         * callee-saved registers the allocator took. Built here and
         * patched into the push below, so the two cannot disagree. */
        unsigned mask = save_mask_for(F.nsave, F.used_callee);
        t_patch_push(t, push_at, mask);
        if (fn->is_varargs) {
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

    for (i = 0; i < F.nfix; i++) {
        int target = F.label_off[F.fix[i].label];
        if (target < 0) {
            fprintf(stderr, "embcc: internal: thumb: label %d of %s was "
                            "never placed\n", F.fix[i].label, fn->name);
            exit(1);
        }
        if (F.fix[i].cond < 0)
            t_patch_b(t, F.fix[i].at, target);
        else
            t_patch_bcond(t, F.fix[i].at, target);
    }

    f->code_len = t->len - f->code_off;
    /* What -fstack-usage reports: the registers the prologue pushed
     * plus everything sub sp reserved. */
    f->stack_bytes = (int)(F.frame + save_bytes_for(F.nsave, F.used_callee));
    free(F.usecnt);
    free(F.slot);
    free(F.label_off);
    free(F.fix);
    free(F.wide);
    free(F.loc);
}

void codegen_unit_thumb(struct ir_unit *iu, struct code *text,
                        struct extcall **ext, int *next,
                        struct strsite **strs, int *nstrs,
                        struct gsite **gs, int *ngs,
                        struct fsite **fs, int *nfs, int want_debug,
                        int optimize, int no_sse, int regalloc)
{
    (void)optimize; (void)no_sse;
    g_t_regalloc = regalloc;
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
        const char *e = getenv("EMBCC_T_FPU");
        target_set_thumb_fpu(e && *e && *e != '0');
    }

    struct t_sites st;
    st.call = NULL; st.ncall = st.capcall = 0;
    st.ext = NULL;  st.next = st.capext = 0;
    st.str = NULL;  st.nstr = st.capstr = 0;
    st.g = NULL;    st.ng = st.capg = 0;
    st.f = NULL;    st.nf = st.capf = 0;

    for (int n = 0; n < iu->nfuncs; n++)
        gen_func(&iu->funcs[n], text, &st, want_debug);

    for (int n = 0; n < st.ncall; n++)
        t_patch_bl(text, st.call[n].patch_off, st.call[n].target->code_off);
    free(st.call);

    cg_resolve_strsites(iu, st.str, st.nstr);

    *ext = st.ext;   *next = st.next;
    *strs = st.str;  *nstrs = st.nstr;
    *gs = st.g;      *ngs = st.ng;
    *fs = st.f;      *nfs = st.nf;
}
