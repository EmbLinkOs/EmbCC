/* AVR code generation (ATmega328P and its family).
 *
 * The fifth backend, and the first one that is not a 32- or 64-bit
 * machine. That difference is the whole character of this file: on the
 * other four a scalar fits in a register and the awkward case was the
 * one that did not (a `long long` on ARMv7-M). Here NOTHING fits. An
 * `int` is two registers, a `long` is four, and every arithmetic
 * operation is a carry chain over bytes, low byte first.
 *
 * Three facts about the machine shape everything below.
 *
 *   There is no sp-relative addressing. None. `ld`/`st` reach memory
 *   only through X, Y or Z, and only Y and Z take a displacement. A
 *   frame pointer is therefore not an optimisation here -- it is the
 *   only way to name a local -- and the displacement is SIX BITS, so a
 *   frame past 63 bytes needs a computed pointer for its upper reaches.
 *
 *   r1 is zero. Not by convention this file could opt out of: every AVR
 *   object in the program assumes it, avr-libc's startup clears it, and
 *   `mul` is the one instruction that destroys it. That is a real reason
 *   this version emits no multiply, and not merely a missing lowering.
 *
 *   Program space is a SEPARATE address space, addressed in words. A
 *   function pointer holds half a byte address. Getting that wrong does
 *   not fault -- it calls twice as far into flash and finds a real
 *   instruction there -- which is why `&f` goes through the _GS
 *   relocations and never the data ones.
 *
 * Like the other three non-x86 backends this one starts with every value
 * in a frame slot and a fixed set of scratch registers (D-005). A
 * register allocator here has to hand out RUNS of consecutive registers,
 * even-aligned for movw and adiw, out of a pool whose halves are not
 * interchangeable -- the immediate instructions reach only r16-r31.
 * Teaching the shared Chaitin-Briggs allocator that is a change to code
 * five working targets depend on, and it is not the first thing to do on
 * a machine that has never run a single instruction from this compiler.
 *
 * ---- what it does NOT do yet, and says so ---------------------------
 *
 * THE RULE: every one of these refuses by name rather than emitting
 * something plausible.
 *
 *   Multiply, divide and modulo. AVR's `mul` is 8x8 -> 16 and destroys
 *   r1; divide has no instruction at all. Both are runtime helpers on
 *   every AVR toolchain (libgcc's __mulsi3, __udivmodsi4), so they need
 *   lib/rt work and a helper-call convention, not an instruction
 *   selection.
 *
 *   Floating point. `float` and `double` are both four-byte IEEE single
 *   here, and every operation on them is a call to a soft-float routine
 *   this target has none of yet.
 *
 *   64-bit integers. `long long` arrives as one vreg at w == 8 and needs
 *   eight consecutive registers; A and B together are eight, so this is
 *   reachable, but it wants the legalisation pass rather than a second
 *   set of hand-written carry chains.
 *
 *   Aggregates by value, varargs, atomics, inline asm, VLAs, computed
 *   goto, exceptions and vectors. Each is ABI or runtime work of its own.
 *
 *   __flash / PROGMEM. Literals and `const` data go to RAM, initialised
 *   from flash by the startup code, which is what avr-gcc does by
 *   default and why `const char *s = "hi"` works there unannotated.
 *   Keeping them in flash is a RAM-saving optimisation on top, and an
 *   address-space qualifier the front end does not have.
 *
 * What it DOES do is the scalar integer language at one, two and four
 * bytes: arithmetic, bitwise operations, shifts by a constant or a
 * variable, comparisons and control flow, loads and stores of every
 * width, calls with arguments in registers and on the stack, and the
 * address of a local, a global, a string or a function.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emit.h"
#include "../backend.h"
#include "../target.h"
#include "../../driver/util.h"

/* ---- the register plan ----------------------------------------------
 *
 *   r0        the ABI's scratch: SREG saves, and byte shuffling.
 *   r1        the zero register. Read, never written.
 *   r18-r21   A, the accumulator: four bytes, LOW BYTE FIRST.
 *   r22-r25   B, the second operand: the same shape.
 *   r26:r27   X and r30:r31 Z: addresses. No value ever lives in either.
 *   r28:r29   Y, the frame pointer.
 *   r2-r17    never touched, and therefore never saved.
 *
 * A and B both sit inside r16-r31, which is the half the immediate
 * instructions (ldi, subi, sbci, andi, ori, cpi) can reach. That is not
 * an accident to be lost later: moving either down would put an extra
 * register in the path of every constant.
 *
 * A and B are also exactly the first four argument registers, which
 * costs nothing because an argument is loaded STRAIGHT from its slot
 * into its own ABI register -- `ldd rN, Y+q` touches rN and Y and
 * nothing else, so the arguments cannot tread on one another and there
 * is no parallel move to solve. That is the one place where keeping
 * every value in memory makes the code SIMPLER rather than slower.
 */
#define R_TMP    0
#define R_ZERO   1
#define RA      18
#define RB      22
#define VW       4       /* a temporary's slot: four bytes, always */

/* The I/O addresses the prologue needs. These three are the same on
 * every AVR with 32 or fewer I/O registers below them, which is every
 * part this backend targets; a machine with SPH elsewhere would need
 * them from the device description. */
#define IO_SPL  0x3d
#define IO_SPH  0x3e
#define IO_SREG 0x3f

/* The argument registers run from r25 down to r8. Below r8 an argument
 * goes on the stack. */
#define ARG_TOP 26
#define ARG_BOT 8

struct a_sites {
    struct extcall *ext;   int next, capext;
    struct strsite *str;   int nstr, capstr;
    struct gsite *g;       int ng, capg;
    struct fsite *f;       int nf, capf;
};

/* A jump whose target label was not placed yet. Every branch to a label
 * goes on this list and is patched when the function ends, so forward
 * and backward jumps take one path. */
/* A jump whose target label was not placed yet.
 *
 * `wide` says the site is a 32-bit `jmp` rather than a 16-bit `rjmp`. AVR has
 * no PC-relative long jump: rjmp reaches +-4 KB and `jmp` carries an
 * ABSOLUTE word address, which a relocatable object cannot know -- so a far
 * jump is a `jmp` with a relocation against this .text plus the label's
 * offset. A FORWARD jump is always the wide form, because its distance is not
 * known when it is emitted and the two sizes differ; a BACKWARD one takes
 * the short form when it fits, which is the common case and every loop. */
struct a_fix { int at; int label; int wide; };

struct a_fn {
    struct ir_func *fn;
    struct code *t;
    struct a_sites *st;
    long *slot;          /* per vreg: its offset from Y, or -1 */
    long frame;          /* bytes between Y and the caller's saved Y */
    int *label_off;
    long scratch_at;     /* base of the struct-return temporaries */
    long sret_slot;      /* where the hidden result pointer is kept, or -1 */
    struct a_fix *fix;
    int nfix, capfix;
    /* Which vregs are known constants, and what they hold.
     *
     * `imm_b` is the OPTIMIZER's answer and is absent at -O0, where a
     * multiply by eight -- which is what an index into an array of
     * eight-byte structs is -- arrives as a multiply by a vreg that an
     * IR_CONST two instructions earlier filled in. Refusing that refuses
     * `tab[1].x` at -O0, and turning every constant shift into a
     * count-down loop makes -O0 code several times larger than it needs
     * to be. So this backend works the constants out for itself rather
     * than depending on a pass that may not have run.
     *
     * Only temporaries: a LOCAL is memory and can be written again. A
     * temporary written twice is marked unknown, so the map never claims
     * more than the IR guarantees. */
    long *cval;
    char *cknown;
    /* Which vregs hold an EIGHT-byte value. By the width of the RESULT --
     * i->w for the value-producing operations and four for the rest however
     * wide their operands are: an IR_CMP at w == 8 compares two 64-bit
     * values and yields a 0 or a 1. Getting that backwards gives the result
     * an eight-byte slot and reads four bytes of a neighbouring temporary as
     * its high half. */
    char *wide;
    int want_debug;
};

/* A value's width in bytes: eight when the map says so, four otherwise. */
static int vw(const struct a_fn *F, int v)
{
    return (v >= 0 && F->wide && F->wide[v]) ? 8 : VW;
}

/* Does this INSTRUCTION operate on eight bytes?
 *
 * `i->w` answers for the arithmetic, but not for the memory operations: an
 * IR_STVAR carries the width of the value it stores in `size` and leaves `w`
 * at four, so asking `w == 8` sends an eight-byte store down the four-byte
 * path -- which silently drops its top half, and did, inside __muldi3. The
 * operand's own map entry is the answer there. */
static int wide_ins(const struct a_fn *F, const struct ir_ins *i)
{
    switch (i->op) {
    case IR_STVAR: return i->size == 8 || vw(F, i->a) == 8;
    case IR_STORE: return i->size == 8 || vw(F, i->b) == 8;
    case IR_LDVAR: case IR_LOAD: case IR_EXT:
        return i->size == 8 || (i->dst >= 0 && vw(F, i->dst) == 8);
    default:
        /* `w` is the first answer, and the DESTINATION's own width is the
         * second -- because a MOV is not required to carry a width and often
         * does not. The merge of a `?:`'s two arms is emitted with an operand
         * and a destination and nothing else, so asking `w == 8` alone made
         * an eight-byte merge copy four bytes: __divdi3's
         * `neg ? -(s64_)q : (s64_)q` returned its low half with the high half
         * of whatever the slot's previous tenant left there. */
        return i->w == 8 || (i->dst >= 0 && vw(F, i->dst) == 8);
    }
}

/* Which vregs are eight bytes wide. */
static char *avr_wide_map(struct ir_func *fn)
{
    char *w = xcalloc((size_t)(fn->nvregs ? fn->nvregs : 1), 1);
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        if (i->w != 8 || i->dst < 0 || i->dst >= fn->nvregs)
            continue;
        switch (i->op) {
        case IR_CONST: case IR_MOV:
        case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
        case IR_AND: case IR_OR: case IR_XOR: case IR_SHL: case IR_SHR:
        case IR_NEG: case IR_BNOT:
        case IR_LDVAR: case IR_LOAD: case IR_EXT: case IR_CALL:
        case IR_SELECT: case IR_BSWAP:
            w[i->dst] = 1;
            break;
        default:
            break;
        }
    }
    /* A local declared eight bytes wide is one whether or not any
     * instruction has been seen to define it: the prologue writes a
     * parameter into its slot before the body runs. */
    for (int v = 0; v < fn->nvars && v < fn->nvregs; v++)
        if (fn->locals[v].size == 8 && fn->locals[v].is_int_or_ptr)
            w[v] = 1;
    /* Then through COPIES, to a fixpoint: a MOV is not required to carry a
     * width and often does not -- the merge of a `?:`'s two arms is emitted
     * with an operand and a destination and nothing else. */
    for (int again = 1; again; ) {
        again = 0;
        for (int n = 0; n < fn->nins; n++) {
            const struct ir_ins *i = &fn->ins[n];
            if (i->dst < 0 || i->dst >= fn->nvregs || w[i->dst])
                continue;
            if (i->op == IR_MOV && i->w != 4 &&
                i->a >= 0 && i->a < fn->nvregs && w[i->a]) {
                w[i->dst] = 1;
                again = 1;
            }
        }
    }
    return w;
}

/* Build that map. One pass, and the second write to a vreg withdraws it. */
static void const_map(struct a_fn *F)
{
    struct ir_func *fn = F->fn;
    int n = fn->nvregs ? fn->nvregs : 1;
    F->cval = xcalloc((size_t)n, sizeof *F->cval);
    F->cknown = xcalloc((size_t)n, 1);
    for (int k = 0; k < fn->nins; k++) {
        const struct ir_ins *i = &fn->ins[k];
        int d = i->dst;
        if (d < fn->nvars || d < 0 || d >= fn->nvregs)
            continue;
        if (F->cknown[d]) {            /* written twice: withdraw it */
            F->cknown[d] = 2;
            continue;
        }
        if (i->op == IR_CONST) {
            F->cval[d] = i->imm;
            F->cknown[d] = 1;
        }
    }
    for (int v = 0; v < n; v++)
        if (F->cknown[v] == 2)
            F->cknown[v] = 0;
}

/* The constant operand b holds, from `imm_b` when the optimizer folded it
 * and from the map otherwise. */
static int const_b(const struct a_fn *F, const struct ir_ins *i, long *out)
{
    if (i->imm_b) { *out = i->imm; return 1; }
    if (i->b >= 0 && i->b < F->fn->nvregs && F->cknown[i->b]) {
        *out = F->cval[i->b];
        return 1;
    }
    return 0;
}

/* ---- refusal --------------------------------------------------------- */

static void a_refuse(const struct ir_func *fn, const struct ir_ins *i,
                     const char *what)
{
    char op[80];
    op[0] = 0;
    if (i)
        snprintf(op, sizeof op, " [%s w=%d size=%d]", ir_opname(i->op),
                 i->w, i->size);
    fprintf(stderr,
            "embcc: %s:%d: error: the AVR backend cannot lower %s yet "
            "(function %s)%s\n",
            fn->file ? fn->file : "?", i ? i->line : fn->line, what,
            fn->name, op);
    exit(1);
}

/* ---- the ABI --------------------------------------------------------- */

/* Where one argument goes.
 *
 * AVR's rule is a single cursor starting just above r25 and walking
 * DOWN, with each argument's size rounded up to EVEN so that every
 * argument begins on an even register -- which is what lets a pointer or
 * an int be moved with `movw`. Below r8 there are no argument registers
 * left and the argument goes on the stack.
 *
 * There is NO back-fill. Once one argument has overflowed, every later
 * one is on the stack even if it would have fitted: measured from clang,
 * where f(long,long,long,long,long,char) puts the char on the stack with
 * r8:r9 sitting free. Getting that wrong is invisible in any call whose
 * arguments happen to be the same size.
 *
 * On the stack the arguments are packed at their NATURAL size, with no
 * even rounding and no padding -- the same measurement: the char above
 * takes one byte, not two.
 */
struct argplace {
    int reg;             /* the low register, or 0 when on the stack */
    int nreg;            /* bytes in registers (the odd pad is not one) */
    long stk;            /* offset in the outgoing area */
    int nstk;            /* bytes on the stack */
};

/* `cursor` of -1 means a VARIADIC call: every argument goes on the stack,
 * the named ones included. Measured from clang -- sum(3, 10, 20, 30) writes
 * all four words to the outgoing area and puts nothing in a register -- and
 * it is why a va_list here is a bare pointer with no register-save area
 * behind it. */
static void place_arg(int size, int *cursor, long *stk, struct argplace *p)
{
    int even = (size + 1) & ~1;

    p->reg = 0; p->nreg = 0; p->stk = 0; p->nstk = 0;
    if (*cursor < 0) {
        p->stk = *stk;
        p->nstk = size;
        *stk += size;
        return;
    }
    if (*cursor - even >= ARG_BOT) {
        p->reg = *cursor - even;
        p->nreg = size;
        *cursor -= even;
    } else {
        *cursor = ARG_BOT;         /* nothing back-fills past an overflow */
        p->stk = *stk;
        p->nstk = size;
        *stk += size;
    }
}

/* The return value follows the same rule applied to one value: r24 for a
 * byte, r25:r24 for two, r25:r22 for four, r25:r18 for eight. Which is
 * what libgcc's AVR routines use, so `26 - even` is the whole table. */
static int ret_reg(int size)
{
    return ARG_TOP - ((size + 1) & ~1);
}

/* Does a composite of this size come back through a hidden POINTER?
 *
 * Eight bytes is the largest that fits: r25:r18 is the lowest run the cursor
 * can give, so nine bytes has nowhere to go. Measured from clang -- a
 * 12-byte struct is written through an address the caller passed in r24:r25,
 * and an 8-byte one comes back in r18-r25.
 *
 * Only a COMPOSITE. A `long long` is eight bytes and comes back in
 * r18-r25 like any other scalar; asking about size alone would make every
 * 64-bit-returning function treat r24 as a buffer address. */
static int sret_bytes(int retsize) { return retsize > 8 ? retsize : 0; }

static int fn_sret_bytes(const struct ir_func *fn)
{
    return fn->ret_abi.is_struct ? sret_bytes(fn->ret_abi.size) : 0;
}

/* The widest outgoing-argument area any call in this function needs. The
 * IR's own outgoing_bytes is the System V answer and does not apply. */
static long outgoing_area(const struct ir_func *fn)
{
    long most = 0;
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        struct argplace pl;
        int cursor;
        long stk = 0;
        if (i->op != IR_CALL)
            continue;
        cursor = i->call_varargs ? -1 : ARG_TOP;
        for (int k = 0; k < i->nargs; k++)
            place_arg(i->argv[k].size, &cursor, &stk, &pl);
        if (stk > most)
            most = stk;
    }
    return most;
}

/* ---- the frame ------------------------------------------------------
 *
 * Y points one byte BELOW the frame, because that is where AVR's stack
 * pointer points: a push stores and then decrements, so the lowest byte
 * in use is at SP+1. The frame therefore occupies Y+1 .. Y+frame, and
 * going UP from there:
 *
 *      Y+frame+1   the caller's r29, pushed by this prologue
 *      Y+frame+2   the caller's r28
 *      Y+frame+3   the return address, high byte  (the call pushed it)
 *      Y+frame+4   the return address, low byte
 *      Y+frame+5   the first incoming stack argument
 *
 * Which is why r28/r29 are pushed LAST of anything this prologue pushes:
 * the distance from Y to the incoming arguments must not depend on how
 * many other registers were saved. Nothing else is saved here yet, so the
 * five is exact -- and when something is, it goes below this pair.
 */
#define INCOMING_AT 5

static void layout(struct a_fn *F)
{
    struct ir_func *fn = F->fn;
    long off = 1 + outgoing_area(fn);

    F->slot = xmalloc((size_t)(fn->nvregs ? fn->nvregs : 1) * sizeof *F->slot);
    for (int v = 0; v < fn->nvregs; v++)
        F->slot[v] = -1;

    for (int v = 0; v < fn->nvars; v++) {
        int size = fn->locals[v].size ? fn->locals[v].size : VW;
        if (F->wide[v] && size < 8)
            size = 8;
        /* No alignment. AVR's stack pointer is whatever the caller left
         * it at and nothing aligns it, so a slot cannot be promised an
         * alignment in absolute terms -- which is what an
         * __attribute__((aligned)) asks for. avr-gcc has the same
         * limitation and quietly ignores it; this refuses instead. */
        if (fn->locals[v].user_align > 1)
            a_refuse(fn, NULL,
                     "a local with __attribute__((aligned)): AVR's stack "
                     "pointer has no known alignment, so a frame slot "
                     "cannot be given one");
        F->slot[v] = off;
        off += size;
    }
    /* Every temporary is four bytes, because the IR's width class is only
     * ever 4, 8 or 16 -- there is no w == 2 -- so a two-byte `int` arrives
     * here as a four-byte value already extended to four by whatever
     * produced it. Computing at four bytes is the CORRECT reading, and
     * narrowing it would take a width analysis over the whole function.
     *
     * But four bytes each, one slot each, does not FIT. This part has 2 KB
     * of SRAM in total; the test program's `run` wanted 1910 bytes for its
     * temporaries alone once the inliner had been through it, and the
     * symptom was not a diagnostic -- it was a program that printed
     * nothing, because its frame ran off the bottom of RAM into the
     * register file.
     *
     * So temporaries SHARE slots when their live ranges do not overlap,
     * which at -O0 is almost all of them: each subexpression gets its own
     * vreg and nearly every one dies at the next instruction. The interval
     * is [definition, last use] in instruction order, and two intervals
     * that do not overlap cannot be live together whatever the control
     * flow does -- with one guard: a temporary whose last use comes
     * BEFORE its definition in the listing would be a value live around a
     * back edge, and that one is given a slot of its own rather than
     * reasoned about.
     *
     * Measured on tests/golden/avr-exec.sh's program: 1296 bytes down to
     * 304 at -O0, and 1910 down to 418 at -O2. A local keeps its own slot
     * regardless -- its address can be taken, and -g describes it by that
     * slot. */
    {
        int nv = fn->nvregs;
        int *def = xmalloc((size_t)(nv ? nv : 1) * sizeof *def);
        int *last = xmalloc((size_t)(nv ? nv : 1) * sizeof *last);
        long *free_from = xmalloc((size_t)(nv ? nv : 1) * sizeof *free_from);
        long *slot_at = xmalloc((size_t)(nv ? nv : 1) * sizeof *slot_at);
        int *slot_sz = xmalloc((size_t)(nv ? nv : 1) * sizeof *slot_sz);
        int nslots = 0;

        int *labpos = xmalloc((size_t)(fn->nlabels ? fn->nlabels : 1) *
                              sizeof *labpos);

        for (int v = 0; v < nv; v++) { def[v] = -1; last[v] = -1; }
        for (int k = 0; k < fn->nlabels; k++) labpos[k] = -1;
        for (int n = 0; n < fn->nins; n++)
            if (fn->ins[n].op == IR_LABEL && fn->ins[n].label >= 0 &&
                fn->ins[n].label < fn->nlabels)
                labpos[fn->ins[n].label] = n;
        for (int n = 0; n < fn->nins; n++) {
            const struct ir_ins *i = &fn->ins[n];
            int reads[3 + MAX_PARAMS], nr = 0;
            reads[nr++] = i->a;
            reads[nr++] = i->b;
            reads[nr++] = i->c;
            if (i->op == IR_CALL)
                for (int k = 0; k < i->nargs; k++)
                    reads[nr++] = i->argv[k].vreg;
            for (int k = 0; k < nr; k++) {
                int v = reads[k];
                if (v >= fn->nvars && v < nv && n > last[v])
                    last[v] = n;
            }
            if (i->dst >= fn->nvars && i->dst < nv && def[i->dst] < 0)
                def[i->dst] = n;
        }
        /* A linear interval is NOT sound on its own, and this is where
         * slot sharing first went wrong.
         *
         * Consider a value defined inside a loop and used after it,
         * sharing a slot with one defined EARLIER in the same loop: the
         * two intervals do not overlap in the listing, but the back edge
         * re-executes the earlier definition and overwrites the later
         * value before it is read. At -O2 that is not a corner case --
         * mem2reg turns every promotable local into a vreg, so `int d`
         * inside a loop body IS such a value. The symptom was every
         * number printing as "12345C789": `d` and the loop counter shared
         * a slot, so each digit printed the position rather than the count.
         *
         * So every interval that touches a loop is stretched over the
         * WHOLE loop, which is what makes "disjoint" mean "cannot be live
         * together" whatever the control flow does. To a fixpoint, because
         * stretching one interval can make it touch an outer loop. */
        for (int again = 1; again; ) {
            again = 0;
            for (int n = 0; n < fn->nins; n++) {
                const struct ir_ins *i = &fn->ins[n];
                int lp;
                if (i->op != IR_JMP && i->op != IR_BRZ && i->op != IR_BRNZ)
                    continue;
                if (i->label < 0 || i->label >= fn->nlabels)
                    continue;
                lp = labpos[i->label];
                if (lp < 0 || lp >= n)
                    continue;          /* forward: not a back edge */
                for (int v = fn->nvars; v < nv; v++) {
                    if (def[v] < 0 || last[v] < def[v])
                        continue;
                    if (last[v] < lp || def[v] > n)
                        continue;      /* wholly outside this loop */
                    if (def[v] > lp)  { def[v] = lp;  again = 1; }
                    if (last[v] < n)  { last[v] = n;  again = 1; }
                }
            }
        }
        for (int v = fn->nvars; v < nv; v++) {
            int k;
            if (def[v] < 0)
                continue;              /* never defined: no slot at all */
            /* A slot is reused only by a value of the SAME width, which is
             * why the sweep below tracks one size per slot. Mixing them
             * would let a four-byte value land on the high half of an
             * eight-byte one. */
            int need = vw(F, v);
            if (last[v] < def[v]) {
                /* Either never read, or read before it is written. The
                 * first needs one instruction's worth of slot and the
                 * second needs the whole function; both are covered by
                 * refusing to share this one. */
                F->slot[v] = off;
                off += need;
                continue;
            }
            for (k = 0; k < nslots; k++)
                if (free_from[k] <= def[v] && slot_sz[k] == need)
                    break;
            if (k == nslots) {
                slot_at[k] = off;
                slot_sz[k] = need;
                off += need;
                nslots++;
            }
            free_from[k] = last[v] + 1;
            F->slot[v] = slot_at[k];
        }
        free(def); free(last); free(free_from); free(slot_at);
        free(slot_sz); free(labpos);
    }
    F->scratch_at = off;
    off += fn->scratch_bytes;
    /* A function returning a composite in memory is handed the address to
     * write it to, and must still have it at the return -- which may be many
     * calls later, and r24:r25 survives none of them. It lives on the
     * frame. */
    F->sret_slot = -1;
    if (fn_sret_bytes(fn)) {
        F->sret_slot = off;
        off += 2;
    }
    F->frame = off - 1;
}

/* ---- addressing a slot ----------------------------------------------- */

/* Add a 16-bit constant to a pointer pair. adiw reaches 63 and only the
 * four high pairs; past that it is the subi/sbci pair against the
 * NEGATED constant, which is the idiom every AVR toolchain uses because
 * there is no addi. */
static void add_const16(struct a_fn *F, int p, long k)
{
    unsigned neg;
    if (k == 0)
        return;
    if (k > 0 && k <= 63 && (p == 24 || p == 26 || p == 28 || p == 30)) {
        avr_adiw(F->t, p, (int)k);
        return;
    }
    neg = (unsigned)(-k) & 0xffffu;
    avr_ri(F->t, AVR_SUBI, p,     (int)(neg & 0xffu));
    avr_ri(F->t, AVR_SBCI, p + 1, (int)((neg >> 8) & 0xffu));
}

/* A pointer to walk a slot with, chosen so it cannot be the value being
 * moved. Z unless the destination run covers it, then X; values never
 * live in either, so one of the two is always free. */
static int walk_ptr(int r, int n)
{
    return (r <= AVR_Z + 1 && r + n > AVR_Z) ? AVR_X : AVR_Z;
}

/* n bytes of the slot at `off` into r .. r+n-1.
 *
 * Within ldd's six-bit reach this is one instruction per byte off Y.
 * Past it, a walker pointer is built and the bytes come out with
 * post-increment, which needs no displacement at all -- and is why the
 * far path is three instructions plus the loop rather than a
 * displacement recomputed per byte.
 *
 * The walker's setup uses subi/sbci and so CLOBBERS SREG. Nothing here
 * may sit between a compare and its branch; gen_ins is written so that
 * nothing does.
 */
static void ld_slot(struct a_fn *F, int r, long off, int n)
{
    int p, k;
    if (off >= 0 && off + n <= 64) {
        for (k = 0; k < n; k++)
            avr_ldd(F->t, r + k, AVR_Y, (int)off + k);
        return;
    }
    p = walk_ptr(r, n);
    avr_movw(F->t, p, AVR_Y);
    add_const16(F, p, off);
    for (k = 0; k < n; k++)
        avr_ld(F->t, r + k, p, AVR_PTR_POST_INC);
}

static void st_slot(struct a_fn *F, long off, int r, int n)
{
    int p, k;
    if (off >= 0 && off + n <= 64) {
        for (k = 0; k < n; k++)
            avr_std(F->t, AVR_Y, (int)off + k, r + k);
        return;
    }
    p = walk_ptr(r, n);
    avr_movw(F->t, p, AVR_Y);
    add_const16(F, p, off);
    for (k = 0; k < n; k++)
        avr_st(F->t, p, r + k, AVR_PTR_POST_INC);
}

/* A vreg, whole. Four bytes for an ordinary value; an eight-byte one does
 * NOT come through here -- both scratch banks together are eight registers,
 * so an eight-byte binary operation could not hold its two operands. Those
 * go through the byte-at-a-time chain below instead. */
/* A slot access that PRESERVES the flags.
 *
 * The far path computes its walker with subi/sbci, which clobber SREG -- and
 * a carry chain has memory traffic between the instruction that sets the
 * carry and the one that consumes it. So a chain must use these, or its
 * carry is destroyed the first time the frame grows past ldd's six-bit
 * reach. That is not hypothetical: it made __muldi3's `y >>= 1` produce
 * garbage, so the loop never terminated, and only in functions whose frame
 * was large enough -- which is every function that calls it.
 *
 * SREG goes through r0, which the chains never use for a value. The two
 * instructions are paid only on the far path; a near access is a bare ldd
 * and touches nothing. */
static int slot_is_far(long off, int n)
{
    return !(off >= 0 && off + n <= 64);
}

static void ld_slot_cc(struct a_fn *F, int r, long off, int n)
{
    int far = slot_is_far(off, n);
    if (far) { avr_in(F->t, R_TMP, IO_SREG); }
    ld_slot(F, r, off, n);
    if (far) { avr_out(F->t, IO_SREG, R_TMP); }
}

static void st_slot_cc(struct a_fn *F, long off, int r, int n)
{
    int far = slot_is_far(off, n);
    if (far) { avr_in(F->t, R_TMP, IO_SREG); }
    st_slot(F, off, r, n);
    if (far) { avr_out(F->t, IO_SREG, R_TMP); }
}

static void rd4(struct a_fn *F, int v, int r) { ld_slot(F, r, F->slot[v], VW); }

static void wr4(struct a_fn *F, int v, int r)
{
    if (v >= 0 && F->slot[v] >= 0)
        st_slot(F, F->slot[v], r, VW);
}

static void ldi4(struct a_fn *F, int r, unsigned long v, int n);

/* ---- values wider than the scratch banks ------------------------------
 *
 * An eight-byte value cannot be held in registers here: A and B are four
 * each, and a binary operation needs both operands at once. So it is
 * processed a BYTE AT A TIME, straight out of one slot and into another.
 *
 * That works because `ldd` and `std` do NOT affect SREG on this machine.
 * The carry from byte k survives the two loads and the store that byte k+1
 * needs, so an eight-byte add is eight `adc`s with memory traffic between
 * them and no spill. It is the same reason an eight-byte shift can be eight
 * `rol`s: the carry is the only state that has to live across the loads.
 *
 * Slower than a register-resident chain and correct at any width, which is
 * the right trade on a machine with 32 registers and none to spare.
 */
static void wide_bin(struct a_fn *F, const struct ir_ins *i, int n,
                     enum avr_rr first, enum avr_rr rest)
{
    long sa = F->slot[i->a], sd = F->slot[i->dst];
    long sb = i->imm_b ? -1 : F->slot[i->b];
    for (int k = 0; k < n; k++) {
        ld_slot_cc(F, RA, sa + k, 1);
        if (i->imm_b)
            ldi4(F, RB, (unsigned long)((unsigned long)i->imm >> (8 * k)), 1);
        else
            ld_slot_cc(F, RB, sb + k, 1);
        avr_rr(F->t, k ? rest : first, RA, RB);
        if (sd >= 0)
            st_slot_cc(F, sd + k, RA, 1);
    }
}

/* The same shape for a one-operand chain: com, or a shift step. */
static void wide_un(struct a_fn *F, long sa, long sd, int n,
                    enum avr_r1 op, int down)
{
    for (int j = 0; j < n; j++) {
        int k = down ? n - 1 - j : j;
        ld_slot_cc(F, RA, sa + k, 1);
        avr_r1(F->t, op, RA);
        if (sd >= 0)
            st_slot_cc(F, sd + k, RA, 1);
    }
}

/* ---- extension ------------------------------------------------------
 *
 * Every value in a slot is valid to its full four bytes, so a narrow
 * value read from memory must be widened before it is arithmetic. Zero
 * extension is a move from the zero register -- which is the whole
 * reason r1's invariant is worth keeping. Sign extension is AVR's own
 * idiom: shift the top byte left so its sign lands in carry, then
 * `sbc rX, rX` turns that carry into 0x00 or 0xff.
 */
static void extend(struct a_fn *F, int r, int from, int sign, int to)
{
    int k;
    if (from >= to)
        return;
    if (!sign) {
        for (k = from; k < to; k++)
            avr_rr(F->t, AVR_MOV, r + k, R_ZERO);
        return;
    }
    /* The sign byte is built in the FIRST byte above the value and then
     * copied up, so the source byte is not disturbed: `lsl` would
     * destroy it, which is why this shifts the copy. */
    avr_rr(F->t, AVR_MOV, r + from, r + from - 1);
    avr_rr(F->t, AVR_ADD, r + from, r + from);       /* lsl: MSB -> carry */
    avr_rr(F->t, AVR_SBC, r + from, r + from);       /* 0 - carry */
    for (k = from + 1; k < to; k++)
        avr_rr(F->t, AVR_MOV, r + k, r + from);
}

/* ---- constants ------------------------------------------------------- */

static void ldi4(struct a_fn *F, int r, unsigned long v, int n)
{
    for (int k = 0; k < n; k++) {
        int b = (int)((v >> (8 * k)) & 0xffu);
        /* `ldi rX, 0` and `mov rX, r1` are the same size and the same
         * speed; the move is used so that a zero byte does not depend on
         * the destination being in r16-r31. Every register this file
         * loads a constant into is, but the helper is also the one the
         * shift paths use for their fill bytes. */
        if (b == 0)
            avr_rr(F->t, AVR_MOV, r + k, R_ZERO);
        else
            avr_ri(F->t, AVR_LDI, r + k, b);
    }
}

/* ---- labels and branches --------------------------------------------- */

static void want_label(struct a_fn *F, int at, int label, int wide)
{
    if (F->nfix == F->capfix) {
        F->capfix = F->capfix ? F->capfix * 2 : 16;
        F->fix = xrealloc(F->fix, (size_t)F->capfix * sizeof *F->fix);
    }
    F->fix[F->nfix].at = at;
    F->fix[F->nfix].label = label;
    F->fix[F->nfix].wide = wide;
    F->nfix++;
}

/* An unconditional jump to a label.
 *
 * Backward and in reach: `rjmp`, two bytes. Otherwise a 32-bit `jmp` whose
 * absolute address the linker fills in. The choice has to be made HERE and
 * not at patch time, because the two are different sizes -- and a forward
 * jump's distance is not known yet, so it takes the long form. At -O0 a
 * single 64-bit statement is hundreds of instructions, so a loop's back edge
 * really does leave rjmp's +-4 KB; before this, that displacement was masked
 * to twelve bits and a back edge 2300 words behind became a forward jump
 * 1866 words ahead, into empty flash. */
static void jump_to(struct a_fn *F, int label)
{
    long here = F->t->len;
    int known = label <= F->fn->nlabels && F->label_off[label] >= 0;
    if (known) {
        long d = (F->label_off[label] - (here + 2)) / 2;
        if (d >= -2048 && d <= 2047) {
            want_label(F, avr_rjmp(F->t, 0), label, 0);
            return;
        }
    }
    want_label(F, F->t->len, label, 1);
    avr_jmp(F->t, 0);
}

/* A conditional branch to a label is always an INVERTED skip over an
 * rjmp, never a br to the label itself.
 *
 * A br reaches +-63 words, which is 126 bytes -- less than one loop body
 * on a machine where an int add is four instructions. The distance is not
 * known when the branch is emitted, so choosing per site would mean
 * either a relaxation pass or a refusal that fires on ordinary code.
 * Two words always works, and `cond ^ 1` is the inversion because the
 * condition enum pairs each sense with its opposite in bit 0.
 */
static void jump_if(struct a_fn *F, enum avr_cond cond, int label)
{
    /* The inverted branch skips the jump, whose size depends on how far the
     * target is -- so the skip is over one word or two. */
    long at = F->t->len;
    int before;
    avr_br(F->t, (enum avr_cond)(cond ^ 1), 1);
    before = F->t->len;
    jump_to(F, label);
    if (F->t->len - before != 2)
        avr_patch_br(F->t, (int)at, (int)((F->t->len - (at + 2)) / 2));
}

/* ---- call sites ------------------------------------------------------ */

/* EVERY call on this target is a relocation, including one to a function
 * defined in the same unit.
 *
 * The other four backends resolve an intra-unit call themselves, because
 * theirs is PC-relative and a displacement inside one .text is known once
 * the functions are placed. AVR's `call` carries an ABSOLUTE word
 * address, which nothing in a relocatable object knows; its only
 * PC-relative form, `rcall`, reaches +-4 KB, which a single kernel object
 * can exceed. So the linker resolves them all, through the same list the
 * external ones use -- a defined callee already has a symbol, so this
 * costs one relocation and no new plumbing.
 */
static void note_call(struct a_sites *st, int at, struct func *callee)
{
    if (st->next == st->capext) {
        st->capext = st->capext ? st->capext * 2 : 16;
        st->ext = xrealloc(st->ext, (size_t)st->capext * sizeof *st->ext);
    }
    st->ext[st->next].patch_off = at;
    st->ext[st->next].callee = callee;
    st->next++;
}

static void note_str(struct a_sites *st, int at, int idx, enum reloc_kind k)
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

static void note_glob(struct a_sites *st, int at, struct global *g,
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

static void note_fn(struct a_sites *st, int at, struct func *target,
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

/* ---- runtime helpers --------------------------------------------------
 *
 * Multiply, divide and remainder are calls. AVR's `mul` is 8x8 into r1:r0
 * and destroys r1, the zero register every other lowering here reads; there
 * is no divide instruction at all. lib/rt/avr.c implements them, under the
 * constraint rt.h states -- a routine may not use the operation it
 * implements -- so they are shifts and adds inside.
 *
 * A helper has no declaration in the translation unit, so one is minted
 * and kept: the driver turns each site into a relocation against an UNDEF
 * symbol of that name, exactly as it does for any other external call.
 * The list is static because a helper is shared across every function in
 * the unit and the same `struct func` must be handed to every site, or
 * each would get a symbol of its own.
 */
static struct func **g_helpers;
static int g_nhelpers, g_caphelpers;

static void call_helper(struct a_fn *F, const char *name)
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
    note_call(F->st, F->t->len, h);
    avr_call(F->t, 0);
}

/* The helper for one operation. Names are libgcc's and single-result.
 *
 * avr-gcc calls __divmodsi4, which returns the quotient in r18-r21 AND the
 * remainder in r22-r25 -- two results in a layout no C function can
 * express, so matching it needs assembly this compiler cannot yet emit for
 * AVR. These are extra symbols in EmbCC's own runtime rather than
 * redefinitions of avr-libgcc's, so both can be linked into one program;
 * the cost is that `a / b` and `a % b` together are two calls here and one
 * there.
 *
 * ONE width, because the backend computes everything at four bytes: a
 * 32-bit divide of two sign-extended 16-bit values has the right low 16
 * bits. That is also why `int` arithmetic is slower here than avr-gcc's,
 * which has __divmodhi4 and an inline 16-bit multiply.
 */
static const char *helper_for(enum ir_op op, int sign)
{
    switch (op) {
    /* Signedness does not enter a multiply: the low 32 bits of a
     * two's-complement product are the same either way, which is why
     * libgcc has one __mulsi3 and not two. */
    case IR_MUL: return "__mulsi3";
    case IR_DIV: return sign ? "__divsi3" : "__udivsi3";
    case IR_MOD: return sign ? "__modsi3" : "__umodsi3";
    default:     return NULL;
    }
}

/* A 16-bit address into a register pair: two `ldi`s, two relocations,
 * one per byte. The relocation sits on the INSTRUCTION, and the
 * immediate's own bits are split across it -- which the linker knows and
 * this file does not have to. */
static int ldi_addr_pair(struct a_fn *F, int r)
{
    int at = F->t->len;
    avr_ri(F->t, AVR_LDI, r,     0);
    avr_ri(F->t, AVR_LDI, r + 1, 0);
    return at;
}

/* ---- comparisons ----------------------------------------------------- */

/* The four-byte compare: cp then three cpc, which leaves carry and zero
 * describing the whole 32-bit subtraction. Then AVR's own conditions --
 * brlo for unsigned below (it is brcs), brlt for signed, each with the
 * opposite sense one bit away. */
static void cmp4(struct a_fn *F, int a, int b)
{
    avr_rr(F->t, AVR_CP, a, b);
    for (int k = 1; k < VW; k++)
        avr_rr(F->t, AVR_CPC, a + k, b + k);
}

/* Which condition makes the comparison true, and whether the operands
 * must be exchanged to get it. AVR has no "greater than": a > b is
 * b < a, which costs nothing here because both operands are already in
 * registers this file chose. */
static enum avr_cond cond_for(enum binop pred, int sign, int *swap)
{
    *swap = 0;
    switch (pred) {
    case B_EQ: return AVR_BR_EQ;
    case B_NE: return AVR_BR_NE;
    case B_LT: return sign ? AVR_BR_LT : AVR_BR_CS;
    case B_GE: return sign ? AVR_BR_GE : AVR_BR_CC;
    case B_GT: *swap = 1; return sign ? AVR_BR_LT : AVR_BR_CS;
    case B_LE: *swap = 1; return sign ? AVR_BR_GE : AVR_BR_CC;
    default:   return AVR_BR_EQ;
    }
}

/* ---- shifts ---------------------------------------------------------
 *
 * There is no barrel shifter. A shift by one is one instruction per byte
 * and a shift by n is n of those, so a constant count is split: the
 * whole bytes are moved (free -- `mov` is one cycle where eight shifts
 * would be thirty-two) and only the remainder is shifted. That is what
 * avr-gcc does and what clang's `a << 9` shows: three bit-shifts and
 * three moves, not nine of anything.
 */
static void shift1(struct a_fn *F, int r, int op, int sign)
{
    if (op == IR_SHL) {
        avr_rr(F->t, AVR_ADD, r, r);                  /* lsl r */
        for (int k = 1; k < VW; k++)
            avr_rr(F->t, AVR_ADC, r + k, r + k);      /* rol r+k */
    } else {
        avr_r1(F->t, sign ? AVR_ASR : AVR_LSR, r + VW - 1);
        for (int k = VW - 2; k >= 0; k--)
            avr_r1(F->t, AVR_ROR, r + k);
    }
}

static void shift_imm(struct a_fn *F, int r, int op, int sign, long n)
{
    int bytes, k;

    if (n <= 0)
        return;
    /* A count at or past the width is undefined in C. Producing all
     * zeroes (or all sign) is the honest answer and keeps the emitted
     * sequence bounded; the alternative is 32 shift instructions for a
     * program that was already wrong. */
    if (n >= 8 * VW)
        n = 8 * VW - (op == IR_SHL || !sign ? 0 : 1);
    bytes = (int)(n / 8);
    n -= 8 * bytes;

    if (bytes > 0) {
        if (op == IR_SHL) {
            for (k = VW - 1; k >= bytes; k--)
                avr_rr(F->t, AVR_MOV, r + k, r + k - bytes);
            for (k = 0; k < bytes && k < VW; k++)
                avr_rr(F->t, AVR_MOV, r + k, R_ZERO);
        } else {
            /* The fill byte is built BEFORE the moves, from the top byte
             * that is about to be overwritten. */
            int fill = R_ZERO;
            if (sign) {
                avr_rr(F->t, AVR_MOV, R_TMP, r + VW - 1);
                avr_rr(F->t, AVR_ADD, R_TMP, R_TMP);
                avr_rr(F->t, AVR_SBC, R_TMP, R_TMP);
                fill = R_TMP;
            }
            for (k = 0; k + bytes < VW; k++)
                avr_rr(F->t, AVR_MOV, r + k, r + k + bytes);
            for (k = VW - bytes; k < VW; k++)
                if (k >= 0)
                    avr_rr(F->t, AVR_MOV, r + k, fill);
        }
    }
    for (k = 0; k < (int)n; k++)
        shift1(F, r, op, sign);
}

/* A variable count: the count-down loop clang emits, and the only shape
 * available without a barrel shifter.
 *
 *      L: dec  count
 *         brmi done
 *         <one bit>
 *         rjmp L
 *      done:
 *
 * `dec` then `brmi` tests count-1 < 0, so a count of zero exits before
 * shifting anything -- which is what makes the pre-test unnecessary.
 */
static void shift_var(struct a_fn *F, int r, int cnt, int op, int sign)
{
    int top, back, exitj;
    /* The count is decremented WHERE IT IS, not copied into r0: r0 is where a
     * far slot access saves SREG, so a counter there does not survive a chain
     * that touches memory. Nothing needs the original count afterwards. */
    top = F->t->len;
    avr_r1(F->t, AVR_DEC, cnt);
    /* Inverted, over an rjmp: a conditional branch reaches +-128 bytes and
     * nothing should depend on a loop body staying inside that. The wide
     * version of this loop really did outgrow it. */
    avr_br(F->t, AVR_BR_PL, 1);
    exitj = avr_rjmp(F->t, 0);
    shift1(F, r, op, sign);
    back = avr_rjmp(F->t, 0);
    avr_patch_rjmp(F->t, back, (top - (back + 2)) / 2);
    avr_patch_rjmp(F->t, exitj, (F->t->len - (exitj + 2)) / 2);
}

/* ---- the instruction dispatch ---------------------------------------- */

static void gen_ins(struct a_fn *F, int n)
{
    struct ir_func *fn = F->fn;
    struct code *t = F->t;
    const struct ir_ins *i = &fn->ins[n];

    /* Everything this backend has not learned, named before anything is
     * emitted for it. The width test comes first because it applies to
     * every arithmetic op at once. */
    if (i->flt)
        a_refuse(fn, i,
                 "floating point: on AVR a float and a double are both "
                 "four-byte IEEE single and every operation on them is a "
                 "soft-float call this target has no runtime for");
    /* Eight-byte values are handled per operation below, through the
     * byte-at-a-time chain rather than in registers. The ones that have no
     * such path say so where they are. */
    if (i->w == 16)
        a_refuse(fn, i, "a 128-bit value");
    /* A memory access's width is `size`, not `w`, and the four that use it
     * size a REGISTER RUN from it: RA is r18, so size 8 would write
     * r18..r25 and take B for the top half of the value. The w == 8
     * refusal above catches the ordinary `long long`, but nothing states
     * that size cannot exceed w -- and every construct that would reach
     * here is refused for some other reason today, so no test would
     * notice if one stopped being. Say it instead of relying on that.
     *
     * w == 8 is exempt: those go through the byte-at-a-time chain, which
     * never puts more than one byte in a register at a time. */
    if (i->size > VW && !wide_ins(F, i) &&
        (i->op == IR_LDVAR || i->op == IR_STVAR ||
         i->op == IR_LOAD  || i->op == IR_STORE || i->op == IR_EXT))
        a_refuse(fn, i,
                 "a memory access wider than four bytes: the value would "
                 "need eight consecutive registers and this backend's "
                 "second scratch bank is already one of them");

    /* ---- eight bytes: byte at a time, through memory ------------------ */
    if (wide_ins(F, i)) {
        int n = 8;
        switch (i->op) {
        case IR_CONST:
            for (int k = 0; k < n; k++) {
                ldi4(F, RA, (unsigned long)((unsigned long)i->imm >> (8 * k)), 1);
                st_slot_cc(F, F->slot[i->dst] + k, RA, 1);
            }
            return;
        case IR_MOV:
            for (int k = 0; k < n; k++) {
                ld_slot_cc(F, RA, F->slot[i->a] + k, 1);
                st_slot_cc(F, F->slot[i->dst] + k, RA, 1);
            }
            return;
        case IR_ADD: wide_bin(F, i, n, AVR_ADD, AVR_ADC); return;
        case IR_SUB: wide_bin(F, i, n, AVR_SUB, AVR_SBC); return;
        case IR_AND: wide_bin(F, i, n, AVR_AND, AVR_AND); return;
        case IR_OR:  wide_bin(F, i, n, AVR_OR,  AVR_OR);  return;
        case IR_XOR: wide_bin(F, i, n, AVR_EOR, AVR_EOR); return;
        case IR_BNOT:
            wide_un(F, F->slot[i->a], F->slot[i->dst], n, AVR_COM, 0);
            return;
        case IR_NEG:
            /* 0 - a, the same answer as at four bytes: the zero is an
             * immediate per byte, so this is one chain and not two. */
            for (int k = 0; k < n; k++) {
                avr_rr(t, AVR_MOV, RA, R_ZERO);
                ld_slot_cc(F, RB, F->slot[i->a] + k, 1);
                avr_rr(t, k ? AVR_SBC : AVR_SUB, RA, RB);
                st_slot_cc(F, F->slot[i->dst] + k, RA, 1);
            }
            return;
        case IR_EXT: {
            /* Widen the low `size` bytes into eight. */
            int from = i->size < 1 ? 1 : i->size;
            if (from > n) from = n;
            for (int k = 0; k < from; k++) {
                ld_slot_cc(F, RA, F->slot[i->a] + k, 1);
                st_slot_cc(F, F->slot[i->dst] + k, RA, 1);
            }
            if (from < n) {
                /* The fill byte: zero, or the sign of the top valid byte. */
                if (i->sign) {
                    ld_slot_cc(F, RA, F->slot[i->a] + from - 1, 1);
                    avr_rr(t, AVR_ADD, RA, RA);      /* lsl: MSB -> carry */
                    avr_rr(t, AVR_SBC, RA, RA);      /* 0 - carry */
                } else {
                    avr_rr(t, AVR_MOV, RA, R_ZERO);
                }
                for (int k = from; k < n; k++)
                    st_slot_cc(F, F->slot[i->dst] + k, RA, 1);
            }
            return;
        }
        case IR_LDVAR: {
            int from = i->size < 1 ? 1 : i->size;
            if (from > n) from = n;
            for (int k = 0; k < from; k++) {
                ld_slot_cc(F, RA, F->slot[i->a] + k, 1);
                st_slot_cc(F, F->slot[i->dst] + k, RA, 1);
            }
            if (from < n) {
                if (i->sign) {
                    ld_slot_cc(F, RA, F->slot[i->a] + from - 1, 1);
                    avr_rr(t, AVR_ADD, RA, RA);
                    avr_rr(t, AVR_SBC, RA, RA);
                } else {
                    avr_rr(t, AVR_MOV, RA, R_ZERO);
                }
                for (int k = from; k < n; k++)
                    st_slot_cc(F, F->slot[i->dst] + k, RA, 1);
            }
            return;
        }
        case IR_STVAR: {
            int to = i->size < 1 ? 1 : i->size;
            if (to > n) to = n;
            for (int k = 0; k < to; k++) {
                ld_slot_cc(F, RA, F->slot[i->a] + k, 1);
                st_slot_cc(F, F->slot[i->dst] + k, RA, 1);
            }
            return;
        }
        /* The address lives in X, and that is the whole point.
         *
         * A slot the frame pointer cannot reach builds a WALKER, and
         * walk_ptr's answer for a byte in the A bank is Z -- so an address
         * held in Z is destroyed by the very next far slot access. With an
         * eight-byte value the first three bytes came from near slots and
         * worked, and the fourth built its walker in Z and wrote the
         * remaining five through a clobbered pointer: __umoddi3 returned
         * 0x24ffff0697000000 where 4 was wanted.
         *
         * X is never a walker for these. It has no DISPLACED form either,
         * which costs nothing: the bytes are consecutive, so the run walks it
         * with post-increment. */
        case IR_LOAD: {
            int from = i->size < 1 ? 1 : i->size;
            if (from > n) from = n;
            ld_slot(F, RB, F->slot[i->a], 2);
            avr_movw(t, AVR_X, RB);
            for (int k = 0; k < from; k++) {
                avr_ld(t, RA, AVR_X, AVR_PTR_POST_INC);
                st_slot_cc(F, F->slot[i->dst] + k, RA, 1);
            }
            if (from < n) {
                if (i->sign) {
                    /* X has walked past the value, so the sign byte is read
                     * back from the slot just written. */
                    ld_slot(F, RA, F->slot[i->dst] + from - 1, 1);
                    avr_rr(t, AVR_ADD, RA, RA);
                    avr_rr(t, AVR_SBC, RA, RA);
                } else {
                    avr_rr(t, AVR_MOV, RA, R_ZERO);
                }
                for (int k = from; k < n; k++)
                    st_slot_cc(F, F->slot[i->dst] + k, RA, 1);
            }
            return;
        }
        case IR_STORE: {
            int to = i->size < 1 ? 1 : i->size;
            if (to > n) to = n;
            ld_slot(F, RB, F->slot[i->a], 2);
            avr_movw(t, AVR_X, RB);
            for (int k = 0; k < to; k++) {
                ld_slot(F, RA, F->slot[i->b] + k, 1);
                avr_st(t, AVR_X, RA, AVR_PTR_POST_INC);
            }
            return;
        }
        case IR_SHL: case IR_SHR: {
            /* One bit at a time across all eight bytes, and the carry is
             * what carries the bit between them -- which survives the load
             * and store of the next byte because neither touches SREG.
             *
             * A constant count runs the chain that many times; a variable
             * one wraps it in the same count-down loop the four-byte path
             * uses. Left shifts walk UP from the low byte and right shifts
             * DOWN from the high one. */
            long k;
            long sd = F->slot[i->dst], sa = F->slot[i->a];
            int var = !const_b(F, i, &k);
            if (sa != sd)
                for (int b = 0; b < n; b++) {
                    ld_slot_cc(F, RA, sa + b, 1);
                    st_slot_cc(F, sd + b, RA, 1);
                }
            if (!var && k >= 8 * n)
                k = i->op == IR_SHR && i->sign ? 8 * n - 1 : 8 * n;
            if (var) {
                int top, exitj;
                /* The counter stays in RB, NOT in r0. r0 is where a far slot
                 * access saves SREG, and the chain below is full of them --
                 * so a counter there is destroyed on the first iteration
                 * whose frame needs the far path. It shifted by seven bytes
                 * whatever the count said. RB is untouched by the chain,
                 * which only ever uses RA. */
                rd4(F, i->b, RB);
                top = t->len;
                avr_r1(t, AVR_DEC, RB);
                /* An INVERTED branch over an rjmp, not a branch to the exit.
                 * A conditional branch reaches +-128 bytes and the chain
                 * below is eight bytes' worth of load/shift/store -- more
                 * than that as soon as a slot needs the far path. Written the
                 * direct way, the exit branch's displacement overflowed its
                 * seven-bit field and (before avr_patch_br checked) wrapped
                 * into a jump inside the loop's own body. */
                avr_br(t, AVR_BR_PL, 1);
                exitj = avr_rjmp(t, 0);
                /* The first byte sets the carry from nothing: lsl for a left
                 * shift, asr/lsr for a right one, then rol/ror for the rest. */
                if (i->op == IR_SHL) {
                    ld_slot_cc(F, RA, sd, 1);
                    avr_rr(t, AVR_ADD, RA, RA);
                    st_slot_cc(F, sd, RA, 1);
                    for (int b = 1; b < n; b++) {
                        ld_slot_cc(F, RA, sd + b, 1);
                        avr_rr(t, AVR_ADC, RA, RA);
                        st_slot_cc(F, sd + b, RA, 1);
                    }
                } else {
                    ld_slot_cc(F, RA, sd + n - 1, 1);
                    avr_r1(t, i->sign ? AVR_ASR : AVR_LSR, RA);
                    st_slot_cc(F, sd + n - 1, RA, 1);
                    for (int b = n - 2; b >= 0; b--) {
                        ld_slot_cc(F, RA, sd + b, 1);
                        avr_r1(t, AVR_ROR, RA);
                        st_slot_cc(F, sd + b, RA, 1);
                    }
                }
                {
                    int back = avr_rjmp(t, 0);
                    avr_patch_rjmp(t, back, (top - (back + 2)) / 2);
                    avr_patch_rjmp(t, exitj,
                                   (int)((t->len - (exitj + 2)) / 2));
                }
                return;
            }
            for (long q = 0; q < k; q++) {
                if (i->op == IR_SHL) {
                    ld_slot_cc(F, RA, sd, 1);
                    avr_rr(t, AVR_ADD, RA, RA);
                    st_slot_cc(F, sd, RA, 1);
                    for (int b = 1; b < n; b++) {
                        ld_slot_cc(F, RA, sd + b, 1);
                        avr_rr(t, AVR_ADC, RA, RA);
                        st_slot_cc(F, sd + b, RA, 1);
                    }
                } else {
                    ld_slot_cc(F, RA, sd + n - 1, 1);
                    avr_r1(t, i->sign ? AVR_ASR : AVR_LSR, RA);
                    st_slot_cc(F, sd + n - 1, RA, 1);
                    for (int b = n - 2; b >= 0; b--) {
                        ld_slot_cc(F, RA, sd + b, 1);
                        avr_r1(t, AVR_ROR, RA);
                        st_slot_cc(F, sd + b, RA, 1);
                    }
                }
            }
            return;
        }
        case IR_MUL: case IR_DIV: case IR_MOD: {
            /* Eight-byte multiply and divide are runtime calls, as the
             * four-byte ones are. libgcc's names for this width. */
            const char *nm = i->op == IR_MUL ? "__muldi3"
                           : i->op == IR_DIV ? (i->sign ? "__divdi3"
                                                        : "__udivdi3")
                           :                   (i->sign ? "__moddi3"
                                                        : "__umoddi3");
            struct argplace pl;
            int cursor = ARG_TOP;
            long stk = 0;
            int a_reg, b_reg;
            place_arg(8, &cursor, &stk, &pl); a_reg = pl.reg;
            place_arg(8, &cursor, &stk, &pl); b_reg = pl.reg;
            if (!pl.nreg)
                a_refuse(fn, i,
                         "a 64-bit multiply or divide: its two arguments do "
                         "not both fit in the argument registers, so the "
                         "helper would need a stack argument this path does "
                         "not place");
            ld_slot(F, a_reg, F->slot[i->a], 8);
            if (i->imm_b)
                for (int k = 0; k < 8; k++)
                    ldi4(F, b_reg + k,
                         (unsigned long)((unsigned long)i->imm >> (8 * k)), 1);
            else
                ld_slot(F, b_reg, F->slot[i->b], 8);
            call_helper(F, nm);
            /* The result comes back in the first argument's registers. */
            st_slot(F, F->slot[i->dst], ret_reg(8), 8);
            return;
        }
        case IR_CMP: {
            /* cp then seven cpc, through memory: the carry and zero flags
             * survive the loads, so the whole 64-bit subtraction's result is
             * in SREG by the last byte. */
            int swap;
            enum avr_cond c = cond_for(i->pred, i->sign, &swap);
            long sl = swap ? (i->imm_b ? -1 : F->slot[i->b]) : F->slot[i->a];
            long sr = swap ? F->slot[i->a] : (i->imm_b ? -1 : F->slot[i->b]);
            for (int k = 0; k < n; k++) {
                if (sl < 0)
                    ldi4(F, RA, (unsigned long)((unsigned long)i->imm >> (8 * k)), 1);
                else
                    ld_slot_cc(F, RA, sl + k, 1);
                if (sr < 0)
                    ldi4(F, RB, (unsigned long)((unsigned long)i->imm >> (8 * k)), 1);
                else
                    ld_slot_cc(F, RB, sr + k, 1);
                avr_rr(t, k ? AVR_CPC : AVR_CP, RA, RB);
            }
            /* The result is four bytes wide even though the comparison was
             * eight -- an IR_CMP yields a 0 or a 1. */
            avr_ri(t, AVR_LDI, RA, 1);
            avr_br(t, c, 1);
            avr_rr(t, AVR_MOV, RA, R_ZERO);
            for (int k = 1; k < VW; k++) avr_rr(t, AVR_MOV, RA + k, R_ZERO);
            wr4(F, i->dst, RA);
            return;
        }
        case IR_BRZ: case IR_BRNZ:
            /* PLAIN ld_slot here, not the flag-preserving one, for two
             * reasons that point the same way. It is not needed: this chain
             * BUILDS the flags rather than carrying them, and the last thing
             * before the branch is an `or`. And it is harmful: the
             * flag-preserving path saves SREG through r0, which is this
             * chain's accumulator -- so on a frame large enough for the far
             * path it destroyed the accumulated value, and `while (y)` in
             * __muldi3 read garbage. */
            avr_rr(t, AVR_MOV, R_TMP, R_ZERO);
            for (int k = 0; k < n; k++) {
                ld_slot(F, RA, F->slot[i->a] + k, 1);
                avr_rr(t, AVR_OR, R_TMP, RA);
            }
            jump_if(F, i->op == IR_BRZ ? AVR_BR_EQ : AVR_BR_NE, i->label);
            return;

        /* These do not care how wide the value is, or handle it themselves:
         * a call places its arguments and takes its result by SIZE, an
         * address is two bytes whatever it points at, and control flow has
         * no operand width at all. They fall through to the ordinary
         * switch. */
        case IR_CALL: case IR_RET: case IR_LABEL: case IR_JMP:
        case IR_MEMCPY: case IR_MEMZERO: case IR_FENCE: case IR_UD2:
        case IR_ASM: case IR_ADDR: case IR_STRADDR: case IR_GADDR:
        case IR_FADDR:
            break;

        default:
            /* Anything else at eight bytes has no path yet, and says so. */
            a_refuse(fn, i, "this operation at 64 bits");
        }
    }

    switch (i->op) {
    case IR_CONST:
        ldi4(F, RA, (unsigned long)i->imm, VW);
        wr4(F, i->dst, RA);
        return;

    case IR_MOV:
        rd4(F, i->a, RA);
        wr4(F, i->dst, RA);
        return;

    case IR_ADD: case IR_SUB: case IR_AND: case IR_OR: case IR_XOR: {
        /* An immediate operand is materialised into B rather than folded
         * into subi/andi. The folded forms exist and reach r16-r31, which
         * B is -- but `add` has no immediate at all (it is subi of the
         * negation, with sbci for the carry bytes) and `xor` has none
         * either, so half the cases would take the register path anyway.
         * One path is worth more here than three instructions. */
        rd4(F, i->a, RA);
        if (i->imm_b)
            ldi4(F, RB, (unsigned long)i->imm, VW);
        else
            rd4(F, i->b, RB);
        switch (i->op) {
        case IR_ADD:
            avr_rr(t, AVR_ADD, RA, RB);
            for (int k = 1; k < VW; k++) avr_rr(t, AVR_ADC, RA + k, RB + k);
            break;
        case IR_SUB:
            avr_rr(t, AVR_SUB, RA, RB);
            for (int k = 1; k < VW; k++) avr_rr(t, AVR_SBC, RA + k, RB + k);
            break;
        default: {
            enum avr_rr op = i->op == IR_AND ? AVR_AND
                           : i->op == IR_OR  ? AVR_OR : AVR_EOR;
            for (int k = 0; k < VW; k++) avr_rr(t, op, RA + k, RB + k);
            break;
        }
        }
        wr4(F, i->dst, RA);
        return;
    }

    case IR_MUL: {
        /* A multiply by a CONSTANT is shifts and adds. A general multiply
         * is not, and is refused.
         *
         * AVR's `mul` is 8x8 -> 16 into r1:r0, so a 32-bit product is ten
         * partial products with a carry chain threaded through them --
         * about seventy instructions -- and it DESTROYS r1, the machine's
         * zero register that every other lowering in this file reads. At
         * 140 bytes a site on a part with 32 KB of flash, inlining that is
         * not the answer; every AVR toolchain calls a helper (libgcc's
         * __mulsi3) and so will this one, once lib/rt has an AVR half.
         *
         * The constant case cannot wait for that, because at -O0 an array
         * index scales by the element size with an IR_MUL -- so refusing
         * it would refuse `tab[1].x`. Double-and-add from the top bit
         * down: one shift for a power of two, a few adds for an ordinary
         * struct stride.
         */
        static const int ACC[VW] = { 26, 27, 30, 31 };
        long k;
        int bit, hi, neg;

        if (!const_b(F, i, &k))
            goto helper;               /* __mulsi3 */
        neg = k < 0;
        if (neg) k = -k;
        rd4(F, i->a, RA);
        if (k == 0) {
            ldi4(F, RA, 0, VW);
            wr4(F, i->dst, RA);
            return;
        }
        for (hi = 8 * VW - 1; hi > 0 && !((k >> hi) & 1); hi--)
            ;
        if (hi > 16)
            goto helper;               /* wider than the shifts are worth */
        if (k == (1L << hi)) {
            shift_imm(F, RA, IR_SHL, 0, hi);    /* a power of two */
        } else {
            for (int q = 0; q < VW; q++)
                avr_rr(t, AVR_MOV, ACC[q], R_ZERO);
            for (bit = hi; bit >= 0; bit--) {
                /* acc <<= 1. The accumulator's registers are not
                 * consecutive -- X and Z, the two pairs no value uses --
                 * which lsl/rol do not care about. */
                avr_rr(t, AVR_ADD, ACC[0], ACC[0]);
                for (int q = 1; q < VW; q++)
                    avr_rr(t, AVR_ADC, ACC[q], ACC[q]);
                if ((k >> bit) & 1) {
                    avr_rr(t, AVR_ADD, ACC[0], RA);
                    for (int q = 1; q < VW; q++)
                        avr_rr(t, AVR_ADC, ACC[q], RA + q);
                }
            }
            for (int q = 0; q < VW; q++)
                avr_rr(t, AVR_MOV, RA + q, ACC[q]);
        }
        if (neg) {
            /* 0 - acc, through B as IR_NEG does. */
            for (int q = 0; q < VW; q++)
                avr_rr(t, AVR_MOV, RB + q, RA + q);
            ldi4(F, RA, 0, VW);
            avr_rr(t, AVR_SUB, RA, RB);
            for (int q = 1; q < VW; q++) avr_rr(t, AVR_SBC, RA + q, RB + q);
        }
        wr4(F, i->dst, RA);
        return;
    }

    case IR_DIV: case IR_MOD:
    helper: {
        /* The operands go straight into the helper's argument registers,
         * which for a two-long call are r22-r25 and r18-r21 -- the same
         * places A and B already are, so the loads land where they belong
         * with nothing to move. The result comes back in r22-r25 and is
         * copied to A, because everything after this reads A. */
        const char *name = helper_for(i->op, i->sign);
        /* Two four-byte arguments: the cursor starts above r25 and steps
         * down by four, so they are r22-r25 and r18-r21 -- which is where
         * B and A already sit, so the operands load straight into place.
         * The result returns in r22-r25 and is copied to A, because every
         * lowering after this reads A. */
        struct argplace pl;
        int cursor = ARG_TOP;
        long stk = 0;
        int a_reg, b_reg;
        place_arg(VW, &cursor, &stk, &pl); a_reg = pl.reg;
        place_arg(VW, &cursor, &stk, &pl); b_reg = pl.reg;

        ld_slot(F, a_reg, F->slot[i->a], VW);
        if (i->imm_b)
            ldi4(F, b_reg, (unsigned long)i->imm, VW);
        else
            ld_slot(F, b_reg, F->slot[i->b], VW);
        call_helper(F, name);
        for (int q = 0; q < VW; q++)
            avr_rr(t, AVR_MOV, RA + q, a_reg + q);
        wr4(F, i->dst, RA);
        return;
    }

    case IR_NEG:
        /* 0 - a, which is what clang emits: the com/neg/sbci chain is
         * shorter to write down and longer to get right, and this reuses
         * the subtract that is already here. */
        rd4(F, i->a, RB);
        ldi4(F, RA, 0, VW);
        avr_rr(t, AVR_SUB, RA, RB);
        for (int k = 1; k < VW; k++) avr_rr(t, AVR_SBC, RA + k, RB + k);
        wr4(F, i->dst, RA);
        return;

    case IR_BNOT:
        rd4(F, i->a, RA);
        for (int k = 0; k < VW; k++) avr_r1(t, AVR_COM, RA + k);
        wr4(F, i->dst, RA);
        return;

    case IR_SHL: case IR_SHR: {
        long k;
        rd4(F, i->a, RA);
        if (const_b(F, i, &k)) {
            shift_imm(F, RA, (int)i->op, i->sign, k);
        } else {
            rd4(F, i->b, RB);
            shift_var(F, RA, RB, (int)i->op, i->sign);
        }
        wr4(F, i->dst, RA);
        return;
    }

    case IR_CMP: {
        int swap;
        enum avr_cond c = cond_for(i->pred, i->sign, &swap);
        rd4(F, i->a, RA);
        if (i->imm_b)
            ldi4(F, RB, (unsigned long)i->imm, VW);
        else
            rd4(F, i->b, RB);
        cmp4(F, swap ? RB : RA, swap ? RA : RB);
        /* ldi does not touch SREG, so the 1 may be loaded between the
         * compare and the branch; `mov` does not either. */
        avr_ri(t, AVR_LDI, RA, 1);
        avr_br(t, c, 1);                        /* skip the clear */
        avr_rr(t, AVR_MOV, RA, R_ZERO);
        for (int k = 1; k < VW; k++) avr_rr(t, AVR_MOV, RA + k, R_ZERO);
        wr4(F, i->dst, RA);
        return;
    }

    case IR_BRZ: case IR_BRNZ:
        /* r0 is the accumulator, so every access here must be one that does
         * NOT save SREG through it -- rd4 is, and the wide path above says
         * why that matters. */
        rd4(F, i->a, RA);
        avr_rr(t, AVR_MOV, R_TMP, RA);
        for (int k = 1; k < VW; k++) avr_rr(t, AVR_OR, R_TMP, RA + k);
        jump_if(F, i->op == IR_BRZ ? AVR_BR_EQ : AVR_BR_NE, i->label);
        return;

    case IR_LDVAR:
        ld_slot(F, RA, F->slot[i->a], i->size);
        extend(F, RA, i->size, i->sign, VW);
        wr4(F, i->dst, RA);
        return;

    case IR_STVAR:
        rd4(F, i->a, RA);
        st_slot(F, F->slot[i->dst], RA, i->size);
        return;

    case IR_LOAD:
        /* The pointer is two bytes: an AVR address IS two bytes, whatever
         * the IR's width class says about the vreg holding it. */
        ld_slot(F, AVR_Z, F->slot[i->a], 2);
        if (i->size == 1) {
            avr_ld(t, RA, AVR_Z, AVR_PTR_NONE);
        } else {
            for (int k = 0; k < i->size; k++)
                avr_ld(t, RA + k, AVR_Z, AVR_PTR_POST_INC);
        }
        extend(F, RA, i->size, i->sign, VW);
        wr4(F, i->dst, RA);
        return;

    case IR_STORE:
        /* The value FIRST: its far path may walk through Z, which the
         * address load is about to own. */
        rd4(F, i->b, RA);
        ld_slot(F, AVR_Z, F->slot[i->a], 2);
        if (i->size == 1) {
            avr_st(t, AVR_Z, RA, AVR_PTR_NONE);
        } else {
            for (int k = 0; k < i->size; k++)
                avr_st(t, AVR_Z, RA + k, AVR_PTR_POST_INC);
        }
        return;

    case IR_EXT:
        rd4(F, i->a, RA);
        extend(F, RA, i->size, i->sign, VW);
        wr4(F, i->dst, RA);
        return;

    case IR_ADDR:
        avr_movw(t, RA, AVR_Y);
        add_const16(F, RA, F->slot[i->a]);
        extend(F, RA, 2, 0, VW);
        wr4(F, i->dst, RA);
        return;

    case IR_STRADDR:
        note_str(F->st, ldi_addr_pair(F, RA), i->label, RK_AVR_LO8_LDI);
        note_str(F->st, F->t->len - 2, i->label, RK_AVR_HI8_LDI);
        extend(F, RA, 2, 0, VW);
        wr4(F, i->dst, RA);
        return;

    case IR_GADDR:
        note_glob(F->st, ldi_addr_pair(F, RA), i->glob, RK_AVR_LO8_LDI);
        note_glob(F->st, F->t->len - 2, i->glob, RK_AVR_HI8_LDI);
        extend(F, RA, 2, 0, VW);
        wr4(F, i->dst, RA);
        return;

    case IR_FADDR:
        /* _GS, not the data forms: a function pointer on AVR holds the
         * WORD address, and the linker may route it through a stub. */
        note_fn(F->st, ldi_addr_pair(F, RA), i->callee, RK_AVR_LO8_LDI_GS);
        note_fn(F->st, F->t->len - 2, i->callee, RK_AVR_HI8_LDI_GS);
        extend(F, RA, 2, 0, VW);
        wr4(F, i->dst, RA);
        return;

    case IR_LABEL:
        F->label_off[i->label] = t->len;
        return;

    case IR_JMP:
        jump_to(F, i->label);
        return;

    case IR_RET:
        if (i->a >= 0 && fn->ret_abi.is_struct && fn->ret_abi.size) {
            /* `a` holds the ADDRESS of the composite being returned. Eight
             * bytes or fewer come back in registers; anything larger is
             * copied to the buffer the caller named, whose address must
             * also be in r24:r25 at the return. */
            long n = fn->ret_abi.size;
            /* Both addresses are read into the scratch banks FIRST and moved
             * across with movw. Loading one straight into Z and the other
             * into X does not work: each load's far path needs a walker, and
             * walk_ptr hands out the very pair the other is sitting in. */
            ld_slot(F, RA, F->slot[i->a], 2);
            if (F->sret_slot >= 0) {
                ld_slot(F, RB, F->sret_slot, 2);
                avr_movw(t, AVR_Z, RA);
                avr_movw(t, AVR_X, RB);
                for (long b = 0; b < n; b++) {
                    avr_ldd(t, R_TMP, AVR_Z, (int)b);
                    avr_st(t, AVR_X, R_TMP, AVR_PTR_POST_INC);
                }
                /* and the pointer itself is the return value */
                ld_slot(F, ret_reg(2), F->sret_slot, 2);
            } else {
                avr_movw(t, AVR_Z, RA);
                for (long b = 0; b < n; b++)
                    avr_ldd(t, ret_reg((int)n) + (int)b, AVR_Z, (int)b);
            }
            jump_to(F, fn->nlabels);
            return;
        }
        if (i->a >= 0) {
            int size = fn->ret_abi.size ? fn->ret_abi.size : 2;
            /* Eight bytes is r25:r18 -- the cursor's own answer for a value
             * of that width, and what libgcc's 64-bit helpers return in. */
            if (size > 8)
                a_refuse(fn, i, "returning a value wider than eight bytes");
            ld_slot(F, ret_reg(size), F->slot[i->a], size);
        }
        jump_to(F, fn->nlabels);             /* the epilogue */
        return;

    case IR_CALL: {
        struct argplace pl;
        int cursor = i->call_varargs ? -1 : ARG_TOP;
        long stk = 0;
        int k;




        /* Stack arguments FIRST, while the argument registers are still
         * free to carry them: each is read out of its slot into A and
         * written into the outgoing area, which sits at the bottom of
         * this frame so that it lands directly above the return address
         * the `call` is about to push. */
        for (k = 0; k < i->nargs; k++) {
            place_arg(i->argv[k].size, &cursor, &stk, &pl);
            if (!pl.nstk)
                continue;
            if (i->argv[k].is_struct) {
                /* The vreg holds the struct's ADDRESS; the bytes are what
                 * travels. Copied one at a time through r0 so that no
                 * argument register is disturbed. */
                ld_slot(F, AVR_Z, F->slot[i->argv[k].vreg], 2);
                for (int b = 0; b < pl.nstk; b++) {
                    avr_ldd(t, R_TMP, AVR_Z, b);
                    st_slot(F, 1 + pl.stk + b, R_TMP, 1);
                }
            } else {
                int n = vw(F, i->argv[k].vreg);
                ld_slot(F, RA, F->slot[i->argv[k].vreg],
                        pl.nstk < n ? pl.nstk : n);
                st_slot(F, 1 + pl.stk, RA, pl.nstk);
            }
        }
        /* Then the register ones. In any order: `ldd rN, Y+q` touches rN
         * and Y alone, so no argument can tread on another and there is
         * no parallel move here at all. */
        cursor = i->call_varargs ? -1 : ARG_TOP; stk = 0;
        for (k = 0; k < i->nargs; k++) {
            place_arg(i->argv[k].size, &cursor, &stk, &pl);
            if (!pl.nreg)
                continue;
            if (i->argv[k].is_struct) {
                /* A struct in registers is just its bytes, one per
                 * register -- no partial-word assembly, because a register
                 * here IS a byte. */
                ld_slot(F, AVR_Z, F->slot[i->argv[k].vreg], 2);
                for (int b = 0; b < pl.nreg; b++)
                    avr_ldd(t, pl.reg + b, AVR_Z, b);
            } else {
                ld_slot(F, pl.reg, F->slot[i->argv[k].vreg], pl.nreg);
            }
        }

        /* The hidden result pointer goes in LAST, so nothing above can have
         * used its register as a scratch after it was set. */
        if (sret_bytes(i->retsize)) {
            avr_movw(t, ret_reg(2), AVR_Y);
            add_const16(F, ret_reg(2), F->scratch_at + i->scratch);
        }
        if (i->indirect) {
            /* Z holds a WORD address here, which is what icall wants and
             * what IR_FADDR put in the pointer. */
            ld_slot(F, AVR_Z, F->slot[i->a], 2);
            avr_icall(t);
        } else {
            note_call(F->st, t->len, i->callee);
            avr_call(t, 0);
        }

        if (i->dst >= 0 && i->retsize) {
            /* dst receives the scratch's ADDRESS, which is the contract
             * irgen shares with every backend -- not the composite itself.
             * A composite of eight bytes or fewer came back in registers and
             * has to be stored there first; a larger one the callee already
             * wrote through the pointer handed to it above. */
            if (!sret_bytes(i->retsize))
                st_slot(F, F->scratch_at + i->scratch, ret_reg(i->retsize),
                        i->retsize);
            avr_movw(t, RA, AVR_Y);
            add_const16(F, RA, F->scratch_at + i->scratch);
            extend(F, RA, 2, 0, VW);
            wr4(F, i->dst, RA);
            return;
        }
        if (i->dst >= 0 && i->ret_tybytes) {
            int size = i->ret_tybytes;
            if (size > 8)
                a_refuse(fn, i, "a call returning a value wider than "
                                "eight bytes");
            if (size > VW) {
                /* Eight bytes: straight from the return registers into the
                 * slot, with no extension to do -- the value fills it. */
                st_slot(F, F->slot[i->dst], ret_reg(size), size);
                return;
            }
            /* The CALLER extends. The ABI leaves everything above the
             * return value's own bytes undefined -- r25 after a
             * char-returning call is not zero and not the sign -- and the
             * use site asks for a four-byte value. ret_tybytes/ret_tysign
             * exist in the IR for exactly this (see ir.h). */
            if (ret_reg(size) != RA)
                for (int b = 0; b < size; b++)
                    avr_rr(t, AVR_MOV, RA + b, ret_reg(size) + b);
            extend(F, RA, size, i->ret_tysign, VW);
            wr4(F, i->dst, RA);
        }
        return;
    }

    case IR_MEMCPY: case IR_MEMZERO: {
        /* Z is the destination and X the source, both walked with
         * post-increment. The two pointers are loaded into A and B first
         * and moved across with movw, because loading one of them
         * DIRECTLY would need the other as its walker for a far slot and
         * overwrite the one already there. Three instructions to avoid a
         * bug that would only appear in a function with a frame past 63
         * bytes.
         *
         * A byte loop, not an unrolled copy: `size` is a struct's size
         * and can be hundreds of bytes on a part with 2 KB of RAM. The
         * counter is one byte when it fits, because `dec` sets Z on its
         * own result and a two-byte countdown does not -- sbci's Z
         * describes its own byte only, so the wide form needs an explicit
         * cp/cpc against zero to test the whole count. */
        int top, br;
        int cnt = RA;                 /* free: the pointers are in Z and X */
        if (i->size <= 0)
            return;
        ld_slot(F, RA, F->slot[i->a], 2);
        if (i->op == IR_MEMCPY)
            ld_slot(F, RB, F->slot[i->b], 2);
        avr_movw(t, AVR_Z, RA);
        if (i->op == IR_MEMCPY)
            avr_movw(t, AVR_X, RB);
        if (i->size <= 255) {
            avr_ri(t, AVR_LDI, cnt, i->size);
            top = t->len;
            if (i->op == IR_MEMCPY) {
                avr_ld(t, R_TMP, AVR_X, AVR_PTR_POST_INC);
                avr_st(t, AVR_Z, R_TMP, AVR_PTR_POST_INC);
            } else {
                avr_st(t, AVR_Z, R_ZERO, AVR_PTR_POST_INC);
            }
            avr_r1(t, AVR_DEC, cnt);
        } else {
            ldi4(F, cnt, (unsigned long)i->size, 2);
            top = t->len;
            if (i->op == IR_MEMCPY) {
                avr_ld(t, R_TMP, AVR_X, AVR_PTR_POST_INC);
                avr_st(t, AVR_Z, R_TMP, AVR_PTR_POST_INC);
            } else {
                avr_st(t, AVR_Z, R_ZERO, AVR_PTR_POST_INC);
            }
            avr_ri(t, AVR_SUBI, cnt, 1);
            avr_ri(t, AVR_SBCI, cnt + 1, 0);
            avr_rr(t, AVR_CP,  cnt,     R_ZERO);
            avr_rr(t, AVR_CPC, cnt + 1, R_ZERO);
        }
        br = avr_br(t, AVR_BR_NE, 0);
        avr_patch_br(t, br, (top - (br + 2)) / 2);
        return;
    }

    case IR_ASM: {
        /* Extended asm, assembled in irgen (avr/irgen.c irg_asm_avr) against
         * the vocabulary in avr/asm.c. This only has to place the operands
         * and splice the bytes in.
         *
         * Nothing is live in a register across an asm here, and that needs no
         * argument: every value lives in a frame slot in this backend, so the
         * only registers holding anything across the asm are Y (the frame
         * pointer) and r1 (the zero register) -- and irgen refuses any
         * template, operand or clobber that names either.
         *
         * Z is the address scratch for a slot that ldd cannot reach. An
         * operand pinned to Z is therefore loaded LAST, after every other
         * operand has been placed, so the far-slot path cannot overwrite it.
         */
        struct ir_asm *ia = i->asm_ir;
        int pass;

        for (int k = 0; k < ia->nout; k++)
            if (!ia->out[k].mem && ia->out[k].size > VW)
                a_refuse(fn, i, "an asm output wider than four bytes");
        /* A "+" output starts with the lvalue's CURRENT value; an "m" one is
         * an address the template writes THROUGH. Two passes so that an
         * operand in Z or X goes after the ones whose slots may need Z or X
         * to reach. */
        for (pass = 0; pass < 2; pass++) {
            int late;
            for (int k = 0; k < ia->nout; k++) {
                struct ir_asm_op *o = &ia->out[k];
                late = o->reg >= AVR_X;
                if (late != pass || !o->inout || o->mem)
                    continue;
                /* The address is in the slot; the VALUE is what it points at. */
                ld_slot(F, AVR_Z, F->slot[o->temp], 2);
                for (int b = 0; b < o->size; b++)
                    avr_ldd(t, o->reg + b, AVR_Z, b);
            }
            for (int k = 0; k < ia->nin; k++) {
                struct ir_asm_op *o = &ia->in[k];
                late = o->reg >= AVR_X;
                if (late != pass)
                    continue;
                ld_slot(F, o->reg, F->slot[o->temp], o->size);
            }
        }
        for (int k = 0; k < ia->codelen; k++)
            code_byte(t, ia->code[k]);
        for (int k = 0; k < ia->nout; k++) {
            struct ir_asm_op *o = &ia->out[k];
            /* An "m" output was written BY the template, through the address
             * this register holds; storing the register over it would destroy
             * what the asm produced. */
            if (o->mem)
                continue;
            /* The output value is in o->reg..+size; o->temp holds the
             * ADDRESS to write it to. X is used for the address so that an
             * output sitting in Z is not the thing overwritten. */
            ld_slot(F, AVR_X, F->slot[o->temp], 2);
            for (int b = 0; b < o->size; b++)
                avr_st(t, AVR_X, o->reg + b, AVR_PTR_POST_INC);
        }
        return;
    }

    case IR_VA_START: {
        /* The list is a two-byte pointer at the first argument past the
         * named ones. Everything a variadic call passes is on the stack and
         * packed, so that address is the incoming area plus the named
         * parameters' own bytes -- there is no register-save area to skip. */
        struct argplace pl;
        int cursor = -1;
        long stk = 0;
        for (int k = 0; k < fn->nparams; k++)
            place_arg(fn->param_abi[k].size, &cursor, &stk, &pl);
        avr_movw(t, RA, AVR_Y);
        add_const16(F, RA, F->frame + INCOMING_AT + stk);
        /* `a` holds the ADDRESS of the va_list object. */
        ld_slot(F, AVR_Z, F->slot[i->a], 2);
        avr_std(t, AVR_Z, 0, RA);
        avr_std(t, AVR_Z, 1, RA + 1);
        return;
    }

    case IR_FENCE:
        /* Nothing. An ATmega has one core, no store buffer and no cache:
         * every access is already ordered with respect to every other, so
         * a barrier has nothing to emit. Saying so here rather than
         * refusing, because __sync_synchronize() in portable kernel code
         * is CORRECT on this machine and emitting nothing is the right
         * lowering, not a gap. */
        return;

    case IR_UD2:
        /* A self-loop. AVR has no trapping undefined instruction (`break`
         * halts only under a debug probe), and __builtin_unreachable()
         * promises this is never executed -- so the choice is only about
         * what happens when that promise is broken. A tight loop stops
         * the program where the bug is, which a reset or a fall-through
         * into the next function would not. */
        avr_patch_rjmp(t, avr_rjmp(t, 0), -1);
        return;

    default:
        break;
    }
    a_refuse(fn, i, ir_opname(i->op));

}

/* ---- a function ------------------------------------------------------ */

/* SP is written through two I/O registers, and an interrupt between them
 * would run on a half-updated stack pointer. So the write is bracketed by
 * cli, with SREG saved in r0 and restored BETWEEN the two halves -- which
 * is safe because AVR executes the instruction after an interrupt-enable
 * before servicing anything, and is the order avr-gcc and clang both use.
 */
static void set_sp_from_y(struct code *t)
{
    avr_in(t, R_TMP, IO_SREG);
    avr_bclr(t, AVR_SREG_I);                 /* cli */
    avr_out(t, IO_SPH, 29);
    avr_out(t, IO_SREG, R_TMP);
    avr_out(t, IO_SPL, 28);
}

/* ---- interrupt handlers ----------------------------------------------
 *
 * An AVR interrupt handler is a different function from an ordinary one, in
 * three ways the hardware imposes:
 *
 *   It returns with `reti`, which re-enables interrupts. `ret` leaves them
 *   masked for the rest of the program's life -- which looks like a hang
 *   and not like a miscompile, and is why this attribute is refused rather
 *   than ignored on the targets that do not implement it.
 *
 *   It saves SREG. The interrupt arrived between two instructions of code
 *   that was mid-comparison, and every flag belongs to that code.
 *
 *   It cannot assume r1 is zero. The machine's zero register is only zero
 *   by convention, and `mul` clobbers it -- so an interrupt that lands
 *   between a `mul` and its `clr r1` sees a dirty r1. The handler saves it,
 *   clears it for its own body (and for anything it calls), and restores
 *   it. avr-gcc does exactly this, and it is the reason an ISR is bigger
 *   than a function that does the same work.
 *
 * Everything this backend touches is saved, because a handler must leave
 * the interrupted code exactly as it found it and nothing here knows what
 * that code was using. That is r0, r1, SREG, the A and B scratch banks,
 * X, Z and the frame pointer -- seventeen pushes. A register allocator
 * would narrow it to what the body really uses; until then it is correct
 * and expensive, which is the right way round.
 *
 * The order matters at one point only: SREG is read with `in` AFTER r0 is
 * safe to use and BEFORE anything sets a flag.
 */
static const int ISR_SAVE[] = {
    0, 1,                                  /* scratch and the zero register */
    18, 19, 20, 21, 22, 23, 24, 25,        /* A and B */
    26, 27,                                /* X */
    30, 31,                                /* Z */
    28, 29                                 /* Y, the frame pointer */
};

static void isr_prologue(struct code *t, int kind)
{
    unsigned k;
    avr_push(t, 0);
    avr_in(t, R_TMP, IO_SREG);
    avr_push(t, R_TMP);                    /* SREG, through r0 */
    for (k = 1; k < sizeof ISR_SAVE / sizeof ISR_SAVE[0]; k++)
        avr_push(t, ISR_SAVE[k]);
    /* The body and everything it calls read r1 as zero. */
    avr_rr(t, AVR_EOR, R_ZERO, R_ZERO);
    /* `interrupt`, unlike `signal`, runs with interrupts enabled. */
    if (kind == 2)
        avr_bset(t, AVR_SREG_I);
}

static void isr_epilogue(struct code *t)
{
    int k;
    for (k = (int)(sizeof ISR_SAVE / sizeof ISR_SAVE[0]) - 1; k >= 1; k--)
        avr_pop(t, ISR_SAVE[k]);
    avr_pop(t, R_TMP);
    avr_out(t, IO_SREG, R_TMP);
    avr_pop(t, 0);
    avr_reti(t);
}

static void gen_func(struct ir_func *fn, struct code *t, struct a_sites *st,
                     int want_debug)
{
    struct func *f = fn->src;
    struct a_fn F;
    int i;

    memset(&F, 0, sizeof F);
    F.fn = fn; F.t = t; F.st = st;
    F.want_debug = want_debug;

    if (fn->has_alloca)
        a_refuse(fn, NULL, "a variable-length array");
    if (fn->neh)
        a_refuse(fn, NULL, "an exception region");
    /* The width map BEFORE layout: a slot's size depends on it. */
    F.wide = avr_wide_map(fn);
    layout(&F);
    const_map(&F);

    F.label_off = xmalloc((size_t)(fn->nlabels + 1) * sizeof *F.label_off);
    for (i = 0; i <= fn->nlabels; i++)
        F.label_off[i] = -1;

    f->code_off = t->len;
    if (want_debug) {
        int nv = fn->nvars ? fn->nvars : 1;
        fn->var_off = xmalloc((size_t)nv * sizeof *fn->var_off);
        for (int v = 0; v < fn->nvars; v++)
            fn->var_off[v] = (int)F.slot[v];
    }

    /* ---- prologue ---- */
    if (f->is_isr) {
        /* An interrupt handler takes no arguments and returns nothing --
         * the hardware calls it, so there is no caller to agree with. A
         * handler with a signature would silently read its parameters out
         * of registers the interrupted code was using. */
        if (fn->nparams)
            a_refuse(fn, NULL,
                     "an interrupt handler with parameters: the hardware "
                     "calls it, so there is no caller to pass them and they "
                     "would be read out of whatever the interrupted code "
                     "left in those registers");
        if (fn->ret_abi.size)
            a_refuse(fn, NULL,
                     "an interrupt handler that returns a value: `reti` goes "
                     "back to the interrupted instruction, and nothing is "
                     "there to receive it");
        isr_prologue(t, f->is_isr);
    }
    /* Y is pushed here for an ordinary function and was pushed by
     * isr_prologue for a handler, so the frame arithmetic below is the
     * same either way -- and the distance from Y to the incoming stack
     * arguments stays exact, because a handler has none. */
    if (!f->is_isr) {
        avr_push(t, 28);
        avr_push(t, 29);
    }
    avr_in(t, 28, IO_SPL);
    avr_in(t, 29, IO_SPH);
    if (F.frame) {
        add_const16(&F, AVR_Y, -F.frame);
        set_sp_from_y(t);
    }

    /* The parameters arrive in their ABI registers and, past r8, above
     * the return address. Each is written to its slot, which is what
     * every later reference reads. Nothing in the prologue above
     * clobbered an argument register: it used r28, r29 and r0, and the
     * arguments are r8-r25. */
    {
        struct argplace pl;
        /* A variadic function's parameters are ALL on the stack, the named
         * ones too -- so the placement it must undo is the variadic one. */
        int cursor = fn->is_varargs ? -1 : ARG_TOP;
        long stk = 0;
        /* A function returning a composite in memory receives the buffer's
         * address as an implicit FIRST argument, so the real ones start one
         * slot along. */
        if (F.sret_slot >= 0) {
            struct argplace p0;
            place_arg(2, &cursor, &stk, &p0);
            if (p0.nreg)
                st_slot(&F, F.sret_slot, p0.reg, 2);
            else
                a_refuse(fn, NULL,
                         "a composite result whose hidden pointer did not "
                         "fit in a register");
        }
        for (i = 0; i < fn->nparams; i++) {
            struct ir_arg *a = &fn->param_abi[i];
            place_arg(a->size, &cursor, &stk, &pl);
            if (pl.nreg) {
                st_slot(&F, F.slot[a->vreg], pl.reg, pl.nreg);
            } else if (pl.nstk <= VW) {
                ld_slot(&F, RA, F.frame + INCOMING_AT + pl.stk, pl.nstk);
                st_slot(&F, F.slot[a->vreg], RA, pl.nstk);
            } else {
                /* Wider than a scratch bank -- a struct, or a long long:
                 * byte at a time, which needs no run of registers. */
                for (int b = 0; b < pl.nstk; b++) {
                    ld_slot(&F, R_TMP, F.frame + INCOMING_AT + pl.stk + b, 1);
                    st_slot(&F, F.slot[a->vreg] + b, R_TMP, 1);
                }
            }
        }
    }

    for (i = 0; i < fn->nins; i++)
        gen_ins(&F, i);

    /* ---- epilogue ---- */
    F.label_off[fn->nlabels] = t->len;
    if (F.frame) {
        add_const16(&F, AVR_Y, F.frame);
        set_sp_from_y(t);
    }
    if (f->is_isr) {
        isr_epilogue(t);
    } else {
        avr_pop(t, 29);
        avr_pop(t, 28);
        avr_ret(t);
    }

    for (i = 0; i < F.nfix; i++) {
        int at = F.fix[i].at;
        int to = F.label_off[F.fix[i].label];
        if (to < 0)
            a_refuse(fn, NULL, "a jump to a label that was never placed");
        if (F.fix[i].wide) {
            /* A 32-bit `jmp`, whose operand is an ABSOLUTE word address that
             * only the linker knows: the site is relocated against this
             * .text with the label's offset as the addend. */
            note_str(F.st, at, to, RK_AVR_TEXT_CALL);
        } else {
            avr_patch_rjmp(t, at, (to - (at + 2)) / 2);
        }
    }

    /* -fstack-usage. Worth more on this target than on any other: the
     * part has 2 KB of SRAM total, every temporary here takes four bytes
     * of it, and a frame that does not fit shows up as a program that
     * produces no output at all. */
    f->stack_bytes = (int)F.frame + 2 /* the pushed Y */ + 2 /* the return
                                       * address the call pushed */;
    f->code_len = t->len - f->code_off;
    free(F.slot);
    free(F.label_off);
    free(F.fix);
    free(F.cval);
    free(F.cknown);
    free(F.wide);
}

void codegen_unit_avr(struct ir_unit *iu, struct code *text,
                      struct extcall **ext, int *next,
                      struct strsite **strs, int *nstrs,
                      struct gsite **gs, int *ngs,
                      struct fsite **fs, int *nfs, int want_debug,
                      int optimize, int no_sse, int regalloc)
{
    struct a_sites st;
    memset(&st, 0, sizeof st);
    (void)optimize; (void)no_sse; (void)regalloc;

    for (int n = 0; n < iu->nfuncs; n++)
        gen_func(&iu->funcs[n], text, &st, want_debug);

    /* The sites still carry string INDICES; the driver's relocations want
     * .rodata offsets. */
    cg_resolve_strsites(iu, st.str, st.nstr);

    *ext = st.ext;   *next = st.next;
    *strs = st.str;  *nstrs = st.nstr;
    *gs = st.g;      *ngs = st.ng;
    *fs = st.f;      *nfs = st.nf;
}
