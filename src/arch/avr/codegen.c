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
struct a_fix { int at; int label; };

struct a_fn {
    struct ir_func *fn;
    struct code *t;
    struct a_sites *st;
    long *slot;          /* per vreg: its offset from Y, or -1 */
    long frame;          /* bytes between Y and the caller's saved Y */
    int *label_off;
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
    int want_debug;
};

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

static void place_arg(int size, int *cursor, long *stk, struct argplace *p)
{
    int even = (size + 1) & ~1;

    p->reg = 0; p->nreg = 0; p->stk = 0; p->nstk = 0;
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

/* The widest outgoing-argument area any call in this function needs. The
 * IR's own outgoing_bytes is the System V answer and does not apply. */
static long outgoing_area(const struct ir_func *fn)
{
    long most = 0;
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        struct argplace pl;
        int cursor = ARG_TOP;
        long stk = 0;
        if (i->op != IR_CALL)
            continue;
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
            if (last[v] < def[v]) {
                /* Either never read, or read before it is written. The
                 * first needs one instruction's worth of slot and the
                 * second needs the whole function; both are covered by
                 * refusing to share this one. */
                F->slot[v] = off;
                off += VW;
                continue;
            }
            for (k = 0; k < nslots; k++)
                if (free_from[k] <= def[v])
                    break;
            if (k == nslots) {
                slot_at[k] = off;
                off += VW;
                nslots++;
            }
            free_from[k] = last[v] + 1;
            F->slot[v] = slot_at[k];
        }
        free(def); free(last); free(free_from); free(slot_at); free(labpos);
    }
    off += fn->scratch_bytes;
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

/* A vreg, whole: four bytes in and out of its slot. */
static void rd4(struct a_fn *F, int v, int r) { ld_slot(F, r, F->slot[v], VW); }

static void wr4(struct a_fn *F, int v, int r)
{
    if (v >= 0 && F->slot[v] >= 0)
        st_slot(F, F->slot[v], r, VW);
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

static void want_label(struct a_fn *F, int at, int label)
{
    if (F->nfix == F->capfix) {
        F->capfix = F->capfix ? F->capfix * 2 : 16;
        F->fix = xrealloc(F->fix, (size_t)F->capfix * sizeof *F->fix);
    }
    F->fix[F->nfix].at = at;
    F->fix[F->nfix].label = label;
    F->nfix++;
}

static void jump_to(struct a_fn *F, int label)
{
    want_label(F, avr_rjmp(F->t, 0), label);
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
    avr_br(F->t, (enum avr_cond)(cond ^ 1), 1);
    jump_to(F, label);
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
    int top, back;
    avr_rr(F->t, AVR_MOV, R_TMP, cnt);
    top = F->t->len;
    avr_r1(F->t, AVR_DEC, R_TMP);
    {
        int br = avr_br(F->t, AVR_BR_MI, 0);
        shift1(F, r, op, sign);
        back = avr_rjmp(F->t, 0);
        avr_patch_rjmp(F->t, back, (top - (back + 2)) / 2);
        avr_patch_br(F->t, br, (F->t->len - (br + 2)) / 2);
    }
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
    if (i->w == 8)
        a_refuse(fn, i,
                 "a 64-bit integer: it needs eight consecutive registers "
                 "and a carry chain twice as long as the four-byte one, "
                 "which belongs in a legalisation pass");
    if (i->w == 16)
        a_refuse(fn, i, "a 128-bit value");
    /* A memory access's width is `size`, not `w`, and the four that use it
     * size a REGISTER RUN from it: RA is r18, so size 8 would write
     * r18..r25 and take B for the top half of the value. The w == 8
     * refusal above catches the ordinary `long long`, but nothing states
     * that size cannot exceed w -- and every construct that would reach
     * here is refused for some other reason today, so no test would
     * notice if one stopped being. Say it instead of relying on that. */
    if (i->size > VW &&
        (i->op == IR_LDVAR || i->op == IR_STVAR ||
         i->op == IR_LOAD  || i->op == IR_STORE || i->op == IR_EXT))
        a_refuse(fn, i,
                 "a memory access wider than four bytes: the value would "
                 "need eight consecutive registers and this backend's "
                 "second scratch bank is already one of them");

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
        if (i->a >= 0) {
            int size = fn->ret_abi.size ? fn->ret_abi.size : 2;
            if (size > VW)
                a_refuse(fn, i, "returning a value wider than four bytes");
            ld_slot(F, ret_reg(size), F->slot[i->a], size);
        }
        jump_to(F, fn->nlabels);             /* the epilogue */
        return;

    case IR_CALL: {
        struct argplace pl;
        int cursor = ARG_TOP;
        long stk = 0;
        int k;

        if (i->sret_first || i->rety)
            a_refuse(fn, i, "a call returning a struct by value");
        if (i->call_varargs)
            a_refuse(fn, i, "a variadic call");
        for (k = 0; k < i->nargs; k++)
            if (i->argv[k].is_struct)
                a_refuse(fn, i, "passing a struct by value");

        /* Stack arguments FIRST, while the argument registers are still
         * free to carry them: each is read out of its slot into A and
         * written into the outgoing area, which sits at the bottom of
         * this frame so that it lands directly above the return address
         * the `call` is about to push. */
        for (k = 0; k < i->nargs; k++) {
            place_arg(i->argv[k].size, &cursor, &stk, &pl);
            if (!pl.nstk)
                continue;
            rd4(F, i->argv[k].vreg, RA);
            st_slot(F, 1 + pl.stk, RA, pl.nstk);
        }
        /* Then the register ones. In any order: `ldd rN, Y+q` touches rN
         * and Y alone, so no argument can tread on another and there is
         * no parallel move here at all. */
        cursor = ARG_TOP; stk = 0;
        for (k = 0; k < i->nargs; k++) {
            place_arg(i->argv[k].size, &cursor, &stk, &pl);
            if (pl.nreg)
                ld_slot(F, pl.reg, F->slot[i->argv[k].vreg], pl.nreg);
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

        if (i->dst >= 0 && i->ret_tybytes) {
            int size = i->ret_tybytes;
            if (size > VW)
                a_refuse(fn, i, "a call returning a value wider than "
                                "four bytes");
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
    if (fn->is_varargs)
        a_refuse(fn, NULL, "a variadic function");
    if (fn->has_alloca)
        a_refuse(fn, NULL, "a variable-length array");
    if (fn->neh)
        a_refuse(fn, NULL, "an exception region");
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
        int cursor = ARG_TOP;
        long stk = 0;
        for (i = 0; i < fn->nparams; i++) {
            struct ir_arg *a = &fn->param_abi[i];
            if (a->is_struct)
                a_refuse(fn, NULL, "a struct parameter passed by value");
            place_arg(a->size, &cursor, &stk, &pl);
            if (pl.nreg) {
                st_slot(&F, F.slot[a->vreg], pl.reg, pl.nreg);
            } else {
                ld_slot(&F, RA, F.frame + INCOMING_AT + pl.stk, pl.nstk);
                st_slot(&F, F.slot[a->vreg], RA, pl.nstk);
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
        avr_patch_rjmp(t, at, (to - (at + 2)) / 2);
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
