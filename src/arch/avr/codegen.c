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
 *   PROGMEM. Literals and `const` data go to RAM, initialised from flash
 *   by the startup code, which is what avr-gcc does by default and why
 *   `const char *s = "hi"` works there unannotated. `__flash` keeps an
 *   object in flash instead: its loads are IR_LOADs marked `flash`, read
 *   here with LPM (gen_ins, wide_ins). avr-libc's PROGMEM attribute is
 *   not one EmbCC takes.
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
#include "../regalloc.h"
#include "../../driver/util.h"

/* ---- the register plan ----------------------------------------------
 *
 *   r0        the ABI's scratch: SREG saves, and byte shuffling.
 *   r1        the zero register. Read, never written.
 *   r18-r21   A, the accumulator: four bytes, LOW BYTE FIRST.
 *   r22-r25   B, the second operand: the same shape -- and HOMES, r24:r25
 *             and r22:r23 or all four, for values the allocator gives them
 *             (see "the proof", below).
 *   r26:r27   X: a scratch pointer, or a home.
 *   r30:r31   Z: the address scratch. No value lives in it.
 *   r28:r29   Y, the frame pointer.
 *   r2-r17    homes, each pushed and popped by the function using it.
 *
 * A and B both sit inside r16-r31, which is the half the immediate
 * instructions (ldi, subi, sbci, andi, ori, cpi) can reach. That is not
 * an accident to be lost later: moving either down would put an extra
 * register in the path of every constant.
 *
 * A and B are also exactly the first four argument registers, and B is
 * where a two- or four-byte result comes back. A value that lives THERE
 * costs no push, and no copy at the call, the return or the parameter it
 * arrived as -- which is where clang keeps nearly everything, and why a
 * leaf function here saved four pairs where clang saved none. So B is
 * both: a home for whatever the allocator puts in it, and a scratch bank
 * for an instruction that needs one. Those cannot both hold at once, and
 * what keeps them apart is a proof rather than a convention: gen_func
 * decodes every instruction it emitted and checks that nothing still
 * needed was written (a_verify). Arguments are therefore a PARALLEL MOVE
 * now (see IR_CALL), since one may sit where another goes.
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
/* wide: 0 an rjmp, 1 a 32-bit jmp (relocated), 2 a conditional br, 3 no
 * jump at all -- &&label, whose `at` is its first function-address site's
 * index. site: the jump site it belongs to (avr_relax), or -1. */
struct a_fix { int at; int label; int wide; int site; };

/* Branch relaxation, by regeneration. A forward branch's distance is not
 * known when it is emitted, so every site starts in its long form -- an
 * inverted br over a 4-byte jmp for a conditional one, a jmp for the rest
 * -- and gen_func is run again with each site the last layout showed
 * would reach in a short one: a direct br (+-64 words) or an rjmp
 * (+-2048). Shortening a site only moves every other target closer, so
 * this converges, and a short site that does not reach after all is
 * caught at patch time (`bad`) and pinned long. The site numbering is
 * the order jump_to/jump_if are called in, which no hint changes. */
struct avr_relax {
    unsigned char *hint;   /* per site, in: 0 long, 1 rjmp, 2 br, 3 none:
                            * an unconditional jump to where it stands */
    unsigned char *fits;   /* per site, out: the shortest that reaches */
    int nsite, cap;
    int bad;               /* out: a short site that did not reach, or -1 */
};
struct a_jsite { long at, end; int label; int cond; };

struct a_fn {
    struct ir_func *fn;
    struct code *t;
    struct a_sites *st;
    long *slot;          /* per vreg: its offset from Y, or -1 */
    long frame;          /* bytes between Y and the caller's saved Y */
    int *label_off;
    long scratch_at;     /* base of the struct-return temporaries */
    /* Bytes of each temporary anything reads (avr_demand). Slots stay four
     * bytes; only the traffic narrows, so no write can overrun one. */
    unsigned char *need;
    long sret_slot;      /* where the hidden result pointer is kept, or -1 */
    struct a_fix *fix;
    int nfix, capfix;
    struct avr_relax *rx;
    struct a_jsite *js;  /* every jump site, in order (avr_relax) */
    int njs, capjs;
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
    /* Per vreg: a four-byte-or-narrower constant with one definition that
     * is REBUILT where it is read (vld, vld_cc) rather than kept anywhere:
     * no home, no slot, and its IR_CONST emits nothing. */
    char *remat;
    /* The last store into a home (vst), and where the code stood after it:
     * a vld of the same value into the same registers with nothing emitted
     * since -- and no label placed -- has nothing to do. */
    struct { int v, r, n; long at; } last_st;
    /* What Z holds: the temporary last loaded into it as an address, and
     * where the code stood then. The next access through the same pointer
     * loads nothing if nothing since has written Z -- avr_insn_writes over
     * what was emitted in between says -- and the temporary has not been
     * redefined (gen_func) or a label reached. Temporaries only: a local
     * can change through a pointer to it. -1 when nothing. */
    int zv;
    long zat;
    long zscan;          /* z_holds has seen [zat, zscan) leave Z alone */
    /* Per instruction: an IR_CALL that may be a TAIL call (a_tail_ok) --
     * made one when, too, no argument register is one the epilogue pops.
     * NULL when there are none. */
    char *tail;
    int tail_made;            /* the last instruction was one */
    /* Which vregs hold an EIGHT-byte value. By the width of the RESULT --
     * i->w for the value-producing operations and four for the rest however
     * wide their operands are: an IR_CMP at w == 8 compares two 64-bit
     * values and yields a 0 or a 1. Getting that backwards gives the result
     * an eight-byte slot and reads four bytes of a neighbouring temporary as
     * its high half. */
    char *wide;
    int keep_vars;
    /* Per vreg: the low register of the PAIR the allocator gave it (r2,
     * r4, ... r16) -- or of the QUAD, two adjacent pairs (r2, r6, r10,
     * r14), for a four-byte value -- or -1 for its slot. hw[v] is how
     * many bytes that home holds. A value with one has no slot traffic:
     * vld/vst move its bytes with mov/movw, and every direct slot access
     * goes through sslot(), which refuses one. NULL at -O0 and -O1. */
    int *loc;
    unsigned char *hw;
    int used_callee[RA_MAXPOOL];
    int nsave;           /* how many pairs the allocator took */
    long in_at;          /* Y to the incoming stack arguments, less the frame */
    /* Per temporary: how many low bytes carry its value, the rest being
     * their zero- (xs 0) or sign- (xs 1) extension; 0 when not known.
     * From a narrow load, an extension, a comparison's 0/1, and copies
     * of those (ext_info). */
    unsigned char *xw, *xs;
    int *usecnt;         /* reads per vreg, for compare/branch fusion */
    int skip_next;
    int use_y;           /* the frame pointer is set up (see the prologue) */       /* the compare emitted the branch that follows */
    /* The body's TRANSIENT pushes (t_push): a byte or a run of bytes
     * pushed and popped again inside one instruction's code -- ldi4's
     * borrowed r31, an atomic's operands. tdepth is how far the current
     * instruction's are above the frame, tpeak the most any reached:
     * -fstack-usage adds it, since sp really goes that far below. */
    int tdepth, tpeak;
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
         * of whatever the slot's previous tenant left there.
         *
         * The destination the instruction WRITES, by its opcode -- not the
         * `dst` field, which a branch, a jump or a store has no use for and
         * may leave at 0: that is local 0, and where it was an eight-byte
         * parameter every four-byte branch on the optimizer's own compares
         * came here and read eight registers. */
        {
            int d = ra_ins_def(i);
            return i->w == 8 || (d >= 0 && vw(F, d) == 8);
        }
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
        /* an atomic's old value; not IR_CMPXCHG's, which is a flag */
        case IR_XCHG: case IR_XADD: case IR_ARMW: case IR_CAS:
        /* IR_F2I belongs here even though it is lowered ABOVE the eight-byte
         * dispatch, with its own helper call: what this map decides is how
         * many bytes the DESTINATION SLOT gets, and that lowering stores
         * eight of them for `(long long)f`. Left out, the store ran four
         * bytes past the end of a four-byte slot and over whichever slot the
         * sharing had put next to it. The symptom was a reset loop -- the
         * program printed its first marker over and over -- because what it
         * landed on was a saved return address. */
        case IR_F2I:
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
    /* A constant only if its ONE definition is an IR_CONST. Counting every
     * definition, not only the constant ones: a merge like `%4 = mov %15
     * ... %4 = const 0` wrote %4 first with a mov, which this used to miss,
     * so %4 was taken for the constant 0 -- folded as an immediate by
     * const_b and, once constants were rebuilt where read, rebuilt as 0
     * on the path that had moved %15 into it. */
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
        } else {
            F->cknown[d] = 2;          /* defined, and not as a constant */
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

static int sret_bytes(int retsize);

/* The bytes a call passes on the stack: its outgoing area's extent */
static long call_stack_bytes(const struct ir_ins *i)
{
    int cursor = i->call_varargs ? -1 : ARG_TOP;
    long stk = 0;
    struct argplace pl;
    if (sret_bytes(i->retsize))
        place_arg(2, &cursor, &stk, &pl);
    for (int k = 0; k < i->nargs; k++)
        place_arg(i->argv[k].size, &cursor, &stk, &pl);
    return stk;
}

/* Where a returned value's low byte is: r24 for one or two bytes, r22 for
 * up to four, r18 for up to eight.
 *
 * The size is padded to the next POWER OF TWO, not to the next even number,
 * and that is not the argument rule. avr-libc's FAQ says it in so many words
 * -- "When an argument is returned in registers, its size is padded to the
 * next power of 2" -- where an ARGUMENT is only rounded up to even. This was
 * `26 - even` and it agreed with the power-of-two rule for every size but
 * five and six, which it returned in r20 where avr-gcc returns them in r18: a
 * five-byte struct from any avr-gcc-built function came back two registers
 * off. Every scalar is a power of two already, so only composites move.
 *
 * It was measured from clang when it was written, and clang is not a safe
 * oracle for composites on this target: its struct ARGUMENT passing puts the
 * first field in the highest registers, against the same FAQ's "allocated
 * left to right" -- which Rust's AVR backend documents as clang's ABI not
 * being binary-compatible with avr-gcc. tests/golden/avr-abi.sh checks
 * every size against the documented rules directly. */
static int ret_reg(int size)
{
    int p = 1;
    while (p < size)
        p <<= 1;
    return ARG_TOP - (p < 2 ? 2 : p);
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

/* ---- where each slot goes ----------------------------------------------
 *
 * `ldd`/`std` reach 63 bytes above Y. Past that, a slot is addressed by
 * copying Y into Z and adding the offset -- movw, subi, sbci -- and then an
 * indirect ld/st per byte, with SREG saved and restored around it wherever a
 * carry has to survive. A byte that costs ONE instruction near costs four or
 * five far.
 *
 * Measured over this backend's own output (the embedded-*.c programs and
 * lib/rt/avr*.c, 54730 instructions): ld/st 19%, subi/sbci 18%, movw 9%,
 * in/out 13% -- the far path was roughly HALF OF ALL CODE, and the arithmetic
 * the program asked for barely registered.
 *
 * Offsets used to be handed out in discovery order: outgoing arguments, then
 * every local in declaration order, then temporaries first-come. So a loop
 * counter declared after a 64-byte buffer was out of reach for its whole
 * life. Now every object is weighed by the bytes of access the code makes to
 * it, and they are placed densest first -- accesses per byte of frame -- so
 * the near window holds what is touched most. A big array that is only ever
 * addressed goes last, where its size costs nothing.
 *
 * The weight is STATIC -- each access in the listing counts once, however
 * often a loop runs it -- because what is being minimised is code size, and
 * an access costs its bytes once whatever its trip count. */
struct fobj {
    int size;
    long weight;
    long at;
};

static void weigh_one(struct a_fn *F, struct fobj *obj, const int *obj_of,
                      int v, int bytes)
{
    struct ir_func *fn = F->fn;
    if (v < 0 || v >= fn->nvregs || obj_of[v] < 0)
        return;
    obj[obj_of[v]].weight += bytes > 0 ? bytes : 1;
}

static void weigh_objects(struct a_fn *F, struct fobj *obj, const int *obj_of)
{
    struct ir_func *fn = F->fn;
    /* Every parameter is stored into its slot once, in the prologue. */
    for (int v = 0; v < fn->nparams && v < fn->nvars; v++)
        weigh_one(F, obj, obj_of, v, fn->locals[v].size);
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        switch (i->op) {
        case IR_LDVAR:
            /* `a` is the LOCAL; the destination is a temporary. */
            weigh_one(F, obj, obj_of, i->a, i->size);
            weigh_one(F, obj, obj_of, i->dst, vw(F, i->dst));
            break;
        case IR_STVAR:
            weigh_one(F, obj, obj_of, i->dst, i->size);
            weigh_one(F, obj, obj_of, i->a, vw(F, i->a));
            break;
        case IR_ADDR:
            /* Taking a local's address is add_const16, which costs the same
             * near or far -- so it earns the local no place near Y. */
            weigh_one(F, obj, obj_of, i->dst, vw(F, i->dst));
            break;
        case IR_CALL:
            for (int k = 0; k < i->nargs; k++)
                weigh_one(F, obj, obj_of, i->argv[k].vreg,
                          vw(F, i->argv[k].vreg));
            weigh_one(F, obj, obj_of, i->dst, vw(F, i->dst));
            break;
        default:
            weigh_one(F, obj, obj_of, i->a, vw(F, i->a));
            weigh_one(F, obj, obj_of, i->b, vw(F, i->b));
            weigh_one(F, obj, obj_of, i->c, vw(F, i->c));
            weigh_one(F, obj, obj_of, i->dst, vw(F, i->dst));
            break;
        }
    }
}

/* Densest first; the original order breaks ties, so the layout is a pure
 * function of the IR and an unchanged function gets unchanged bytes. */
static int dense_first(const struct fobj *a, int ia, const struct fobj *b, int ib)
{
    /* a->weight/a->size > b->weight/b->size, without division */
    long l = a->weight * b->size, r = b->weight * a->size;
    if (l != r)
        return l > r;
    return ia < ib;
}

static long place_objects(struct fobj *obj, int nobj, long off)
{
    int *order = xmalloc((size_t)(nobj ? nobj : 1) * sizeof *order);
    for (int k = 0; k < nobj; k++)
        order[k] = k;
    /* Insertion sort: a frame has tens of objects, and it is stable. */
    for (int k = 1; k < nobj; k++) {
        int x = order[k], j = k - 1;
        while (j >= 0 && dense_first(&obj[x], x, &obj[order[j]], order[j])) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = x;
    }
    for (int k = 0; k < nobj; k++) {
        obj[order[k]].at = off;
        off += obj[order[k]].size;
    }
    free(order);
    return off;
}

/* ---- how many bytes of each value anything reads ----------------------
 *
 * need[v] is the number of low bytes of v that some use can observe: 0 if
 * none, else 1, 2 or 4. Backward, to a fixpoint, because a loop can carry a
 * use around to before the definition.
 *
 * It rests on one fact of two's-complement arithmetic: the low N bytes of a
 * sum, difference, product, bitwise and/or/xor, negation, complement or left
 * shift depend ONLY on the low N bytes of the operands. So when nothing reads
 * past byte 2 of a result, nothing about bytes 2 and 3 of its operands can
 * matter either -- and on this target, where an `int` is two bytes and the
 * IR computes it at four, that is most of the arithmetic there is.
 *
 * Every use not named below needs ALL of its operands: a compare, a right
 * shift, a divide, a branch on zero, an intrinsic this was never told about.
 * The default is the safe answer, never the convenient one. Eight-byte values
 * are left alone; they have their own path. */
/* ---- narrow values ----------------------------------------------------
 *
 * The IR has no two-byte width class: an `unsigned` loaded from a local
 * is a four-byte value zero-extended from two, and `i < n` compares four
 * bytes of each. When both sides of a comparison are extensions of the
 * same kind from at most two bytes -- or one is a constant that such an
 * extension could hold -- comparing just those bytes gives the same
 * answer: extension preserves order, signed and unsigned. Only a
 * TEMPORARY defined exactly once is described; anything else is unknown. */
static void ext_info(struct a_fn *F)
{
    const struct ir_func *fn = F->fn;
    int nv = fn->nvregs, again = 1;
    int *ndef = xcalloc((size_t)(nv ? nv : 1), sizeof *ndef);
    F->xw = xcalloc((size_t)(nv ? nv : 1), 1);
    F->xs = xcalloc((size_t)(nv ? nv : 1), 1);
    for (int n = 0; n < fn->nins; n++) {
        int d = ra_ins_def(&fn->ins[n]);
        if (d >= 0 && d < nv) ndef[d]++;
    }
    /* A temporary with SEVERAL definitions -- the merge of `a && b`'s two
     * arms, a 0/1 from either compare -- is known narrow when every one
     * of them is, the same way (the widest of their widths): mw/mk/mn
     * gather that per round, and it is settled when mn reaches ndef. One
     * whose definitions depend on itself never gets there, and stays
     * unknown, which is the safe answer. */
    unsigned char *mw = xcalloc((size_t)(nv ? nv : 1), 1);
    unsigned char *mk = xcalloc((size_t)(nv ? nv : 1), 1);
    int *mn = xcalloc((size_t)(nv ? nv : 1), sizeof *mn);
    for (int round = 0; again && round < 32; round++) {
        again = 0;
        memset(mn, 0, (size_t)(nv ? nv : 1) * sizeof *mn);
        for (int n = 0; n < fn->nins; n++) {
            const struct ir_ins *i = &fn->ins[n];
            int d = i->dst, w = 0, k = 0;
            if (d < fn->nvars || d >= nv || ndef[d] < 1 || F->wide[d])
                continue;
            switch (i->op) {
            case IR_LDVAR: case IR_LOAD:
                if (i->size > 0 && i->size < VW) { w = i->size; k = i->sign; }
                break;
            case IR_EXT:
                if (i->size > 0 && i->size < VW) { w = i->size; k = i->sign; }
                /* An extension of something already narrower composes:
                 * zero-extended from 1 then extended from 2 either way is
                 * still zero-extended from 1 (bit 15 is 0), sign from 1
                 * then sign from 2 is sign from 1, and sign from 1 then
                 * ZERO from 2 is zero-extended from 2. `unsigned char` to
                 * `int` to the IR's four bytes is the first of these, and
                 * it is what lets a char comparison use one byte. */
                if (w && i->a >= 0 && i->a < nv && F->xw[i->a] &&
                    F->xw[i->a] < w) {
                    int wa = F->xw[i->a], ka = F->xs[i->a];
                    if (!ka)            { w = wa; k = 0; }
                    else if (k)         { w = wa; k = 1; }
                    /* else: sign inside, zero outside -- (w, zero) */
                }
                break;
            case IR_CMP:
                w = 1; k = 0;                  /* a 0 or a 1 */
                break;
            /* A constant is as narrow as its value: a small non-negative
             * one zero-extended, a small negative one sign-extended. The
             * `1` in one arm of `a && b` is what the other arm's compare
             * is merged with. */
            case IR_CONST:
                if (!i->flt) {
                    long c = (long)i->imm;
                    if (c >= 0 && c <= 0xff)          { w = 1; k = 0; }
                    else if (c >= 0 && c <= 0xffff)   { w = 2; k = 0; }
                    else if (c < 0 && c >= -0x80)     { w = 1; k = 1; }
                    else if (c < 0 && c >= -0x8000)   { w = 2; k = 1; }
                }
                break;
            case IR_MOV:
                if (i->a >= 0 && i->a < nv && F->xw[i->a]) {
                    w = F->xw[i->a]; k = F->xs[i->a];
                }
                break;
            /* An address is two bytes, zero above: the machine has no
             * more, and every lowering of one fills the rest with r1. */
            case IR_ADDR: case IR_GADDR: case IR_STRADDR: case IR_FADDR:
            case IR_LABELADDR:
                w = 2; k = 0;
                break;
            /* A call's result is extended by the CALLER from its own
             * width, by its own signedness (see IR_CALL). */
            case IR_CALL:
                if (!i->retsize && i->ret_tybytes > 0 && i->ret_tybytes < VW) {
                    w = i->ret_tybytes; k = i->ret_tysign;
                }
                break;
            /* A mask whose upper bytes are zero leaves those bytes zero,
             * whatever it masks: `(f->flags & 2) != 0` tests one byte. And
             * an AND of a zero-extended value is no wider than it. */
            case IR_AND: {
                int wm = 0;
                if (i->imm_b) {
                    unsigned long m = (unsigned long)i->imm & 0xffffffffUL;
                    wm = m <= 0xff ? 1 : m <= 0xffff ? 2 : m <= 0xffffff ? 3 : 0;
                }
                if (i->a >= 0 && i->a < nv && F->xw[i->a] && !F->xs[i->a] &&
                    (!wm || F->xw[i->a] < wm))
                    wm = F->xw[i->a];
                if (!i->imm_b && i->b >= 0 && i->b < nv && F->xw[i->b] &&
                    !F->xs[i->b] && (!wm || F->xw[i->b] < wm))
                    wm = F->xw[i->b];
                if (wm == 3) wm = VW;          /* no three-byte forms */
                if (wm && wm < VW) { w = wm; k = 0; }
                break;
            }
            default:
                break;
            }
            if (ndef[d] > 1) {
                if (!w || (mn[d] && mk[d] != k)) {
                    mn[d] = -nv - 1;          /* one without, or a mix */
                } else if (mn[d] >= 0) {
                    if (!mn[d] || w > mw[d]) mw[d] = (unsigned char)w;
                    mk[d] = (unsigned char)k;
                    mn[d]++;
                }
                continue;
            }
            if (w && (F->xw[d] != w || F->xs[d] != k)) {
                F->xw[d] = (unsigned char)w;
                F->xs[d] = (unsigned char)k;
                again = 1;
            }
        }
        for (int d = fn->nvars; d < nv; d++)
            if (ndef[d] > 1 && mn[d] == ndef[d] &&
                (F->xw[d] != mw[d] || F->xs[d] != mk[d])) {
                F->xw[d] = mw[d];
                F->xs[d] = mk[d];
                again = 1;
            }
    }
    free(mw); free(mk); free(mn);
    free(ndef);
}

/* How many bytes an IR_CMP at w == 4 needs to compare, and whether they
 * compare signed: VW when nothing narrower is known. A zero-extended pair
 * is non-negative at four bytes, so it compares UNSIGNED at its width
 * whatever the IR said; a sign-extended one keeps the IR's signedness. */
static int cmp_width(const struct a_fn *F, const struct ir_ins *i, int *sign)
{
    int nv = F->fn->nvregs, wa = 0, ka, wb = 0;
    *sign = i->sign;
    if (F->xw && i->a >= 0 && i->a < nv)
        wa = F->xw[i->a];
    if (!wa)
        return VW;
    ka = F->xs[i->a];
    if (i->imm_b) {
        long v = (long)i->imm;
        long lim = 1L << (8 * wa);
        if (ka ? !(v >= -lim / 2 && v < lim / 2) : !(v >= 0 && v < lim))
            return VW;
        wb = wa;
    } else {
        if (i->b >= 0 && i->b < nv && F->xs[i->b] == ka)
            wb = F->xw[i->b];
        if (!wb)
            return VW;
    }
    if (!ka)
        *sign = 0;
    return wa > wb ? wa : wb;
}

static int need_of_use(const struct a_fn *F, const struct ir_ins *i, int opnd,
                       int nd)
{
    const struct ir_func *fn = F->fn;
    switch (i->op) {
    case IR_ADD: case IR_SUB: case IR_MUL:
    case IR_AND: case IR_OR: case IR_XOR:
    case IR_NEG: case IR_BNOT: case IR_MOV:
        return nd;
    case IR_SHL:
        return opnd == 0 ? nd : 1;          /* the count: its low byte */
    case IR_CMP:
        if (i->w != 8) {
            int sg;
            return cmp_width(F, i, &sg);    /* only the bytes compared */
        }
        return VW;
    case IR_BRZ: case IR_BRNZ:
        /* an extension is nonzero exactly when its low bytes are */
        if (F->xw && i->a >= 0 && i->a < F->fn->nvregs && F->xw[i->a])
            return F->xw[i->a];
        return VW;
    case IR_SELECT:                         /* the condition: all of it */
        return opnd == 0 ? (i->size == 8 ? 8 : VW) : nd;
    case IR_EXT:
        return nd < i->size ? nd : i->size;
    case IR_STVAR:
        return i->size;
    case IR_STORE:
        return opnd == 0 ? 2 : i->size;     /* an address is two bytes */
    case IR_LOAD:
        return 2;
    case IR_RET:
        /* A composite is returned by ADDRESS: the operand is a pointer to
         * it, all of which is needed however small the struct. The first
         * version of this rule gave it the struct's size -- one byte for a
         * one-byte struct -- so half the pointer was written, and
         * tests/golden/avr-abi.sh's callret1 came back 00 for 60. */
        if (fn->ret_abi.is_struct)
            return VW;
        return fn->ret_abi.size ? fn->ret_abi.size : VW;
    default:
        return VW;
    }
}

/* Uses the rules above do not describe are taken from ra_each_use -- the
 * register allocator's own list of every vreg an instruction reads, inline
 * asm operands included -- and demand all of the value. So an instruction
 * this analysis was never told about can only make a value WIDER, never
 * narrower; the failure it guards against is a sixth hand-kept operand list
 * that misses a field. */
struct dem_ctx { unsigned char *need; const struct a_fn *F; int again; };

static void dem_raise(struct dem_ctx *d, int v, int w)
{
    const struct ir_func *fn = d->F->fn;
    if (v < 0 || v >= fn->nvregs)
        return;
    if (vw(d->F, v) == 8)
        w = 8;
    else if (w > VW || w == 3)
        w = VW;
    if (d->need[v] < w) {
        d->need[v] = (unsigned char)w;
        d->again = 1;
    }
}

static void dem_full(int v, void *ctx) { dem_raise(ctx, v, VW); }

static int narrow_aware(int op)
{
    switch (op) {
    case IR_ADD: case IR_SUB: case IR_MUL:
    case IR_AND: case IR_OR: case IR_XOR:
    case IR_NEG: case IR_BNOT: case IR_MOV: case IR_SHL:
    case IR_SELECT: case IR_EXT: case IR_STVAR: case IR_STORE:
    case IR_LOAD: case IR_RET: case IR_CALL:
    /* The compare and the zero tests read only the bytes need_of_use
     * names -- cmp_width's for a compare, the extension's for a branch --
     * and they were written for that; without them here a compared value
     * was demanded whole, and loaded, extended and kept at four bytes to
     * have one byte tested. */
    case IR_CMP: case IR_BRZ: case IR_BRNZ:
        return 1;
    default:
        return 0;
    }
}

static unsigned char *avr_demand(const struct a_fn *F)
{
    const struct ir_func *fn = F->fn;
    struct dem_ctx d;
    d.need = xcalloc((size_t)(fn->nvregs ? fn->nvregs : 1), 1);
    d.F = F;
    for (d.again = 1; d.again; ) {
        d.again = 0;
        for (int n = fn->nins - 1; n >= 0; n--) {
            const struct ir_ins *i = &fn->ins[n];
            int nd = (i->dst >= 0 && i->dst < fn->nvregs) ? d.need[i->dst] : VW;
            if (!narrow_aware(i->op)) {
                ra_each_use(i, dem_full, &d);
                continue;
            }
            if (i->op == IR_CALL) {
                for (int k = 0; k < i->nargs; k++) {
                    int w = i->argv[k].size;
                    if (w <= 0 || i->argv[k].is_struct) w = VW;
                    dem_raise(&d, i->argv[k].vreg, w);
                }
                if (i->indirect)
                    dem_raise(&d, i->a, VW);
                continue;
            }
            dem_raise(&d, i->a, need_of_use(F, i, 0, nd));
            if (i->op != IR_NEG && i->op != IR_BNOT && i->op != IR_MOV &&
                i->op != IR_EXT && i->op != IR_STVAR && i->op != IR_LOAD &&
                i->op != IR_RET && !i->imm_b)
                dem_raise(&d, i->b, need_of_use(F, i, 1, nd));
            if (i->op == IR_SELECT)
                dem_raise(&d, i->c, need_of_use(F, i, 2, nd));
        }
    }
    return d.need;
}

/* The bytes of v that anything reads. A LOCAL (v < nvars) is always read
 * whole: its slot is its declared size, and -g describes it by that slot.
 * dw() is the same for an instruction's result, capped at a scratch bank. */
static int cw(const struct a_fn *F, int v)
{
    int n;
    if (v < 0 || v < F->fn->nvars || !F->need)
        return VW;
    if (vw(F, v) == 8)
        return 8;
    n = F->need[v];
    return n == 0 ? 0 : n == 1 ? 1 : n == 2 ? 2 : VW;
}

static int dw(const struct a_fn *F, const struct ir_ins *i)
{
    int n = cw(F, i->dst);
    return n > VW ? VW : n;
}

static int in_pair(const struct a_fn *F, int v);
static int g_a_regalloc;           /* -O1 and up, and -O0 (defined below) */
/* -O0: the allocator runs for the temporaries of expressions only, every
 * source variable pinned to its slot (ra_debug_pin_vars), with no tail
 * call and no folded offset -- as thumb's g_t_o0. */
static int g_a_o0;

static void layout(struct a_fn *F)
{
    struct ir_func *fn = F->fn;
    long off = 1 + outgoing_area(fn);

    F->slot = xmalloc((size_t)(fn->nvregs ? fn->nvregs : 1) * sizeof *F->slot);
    for (int v = 0; v < fn->nvregs; v++)
        F->slot[v] = -1;

    /* Offsets are NOT handed out as objects are found. Every local and every
     * shared temporary slot is first collected as an object with a size;
     * then each is weighted by how many bytes of code-visible access it gets,
     * and they are placed DENSEST FIRST. See place_objects() for why that
     * is the single largest code-size lever on this machine. */
    int nobj_cap = fn->nvars + fn->nvregs + 4;
    struct fobj *obj = xcalloc((size_t)nobj_cap, sizeof *obj);
    int nobj = 0;
    int *obj_of = xmalloc((size_t)(fn->nvregs ? fn->nvregs : 1) * sizeof *obj_of);
    for (int v = 0; v < fn->nvregs; v++)
        obj_of[v] = -1;

    char *lref = ra_locals_referenced(fn, F->keep_vars);
    for (int v = 0; v < fn->nvars; v++) {
        int size = fn->locals[v].size ? fn->locals[v].size : VW;
        if (in_pair(F, v))
            continue;                  /* in its register pair: no slot */
        /* Nothing names it -- mem2reg promoted every access away, which
         * it now does for this target's two-byte locals -- so it needs no
         * slot, and a function with none needs no frame at all. At -O0 and -Og
         * every local keeps one (ra_locals_referenced). */
        if (!lref[v] && v >= fn->nparams)
            continue;
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
        obj[nobj].size = size;
        obj_of[v] = nobj++;
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
        /* The four-byte temps share slots by LIVENESS (ra_coalesce_temps,
         * as the other backends do), not by the interval sweep below: two
         * values share a slot unless they are live at once, rather than
         * unless their [first, last] spans overlap once stretched over
         * every loop they touch. What that saves is frame, and on this
         * machine frame is more than memory: past Y+63 every access is
         * three instructions more (a Z walk off Y), and 1938 of them were
         * across lib/libc. Eight-byte temps keep the sweep. */
        int *tslot = NULL, npool = 0, *pool_obj = NULL;
        {
            int *loc2 = xmalloc((size_t)(nv ? nv : 1) * sizeof *loc2);
            int has_cgoto = 0;
            for (int v = 0; v < nv; v++)
                loc2[v] = (in_pair(F, v) || (F->remat && F->remat[v]) ||
                           vw(F, v) != VW) ? 0 : -1;
            for (int n = 0; n < fn->nins; n++)
                if (fn->ins[n].op == IR_IGOTO ||
                    fn->ins[n].op == IR_LABELADDR)
                    has_cgoto = 1;
            {
                struct ra_slots so = { loc2, NULL, g_a_regalloc, has_cgoto };
                tslot = ra_coalesce_temps(fn, fn->nvars, &so, &npool);
            }
            free(loc2);
            pool_obj = xmalloc((size_t)(npool ? npool : 1) * sizeof *pool_obj);
            for (int k = 0; k < npool; k++) pool_obj[k] = -1;
        }
        for (int v = fn->nvars; v < nv; v++) {
            int k;
            if (def[v] < 0 || in_pair(F, v) ||
                (F->remat && F->remat[v]))
                continue;      /* never defined, in a pair, or rebuilt */
            if (vw(F, v) == VW && tslot) {
                int ts = tslot[v - fn->nvars];
                if (ts < 0)
                    continue;              /* named by no instruction */
                if (pool_obj[ts] < 0) {
                    pool_obj[ts] = nobj;
                    obj[nobj++].size = VW;
                }
                obj_of[v] = pool_obj[ts];
                continue;
            }
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
                obj[nobj].size = need;
                obj_of[v] = nobj++;
                continue;
            }
            for (k = 0; k < nslots; k++)
                if (free_from[k] <= def[v] && slot_sz[k] == need)
                    break;
            if (k == nslots) {
                /* A new shared slot is a new object; slot_at holds its
                 * object index until offsets are assigned. */
                slot_at[k] = nobj;
                obj[nobj].size = need;
                nobj++;
                slot_sz[k] = need;
                nslots++;
            }
            free_from[k] = last[v] + 1;
            obj_of[v] = (int)slot_at[k];
        }
        free(def); free(last); free(free_from); free(slot_at);
        free(slot_sz); free(labpos);
        free(tslot); free(pool_obj);
    }
    int scratch_obj = -1, sret_obj = -1;
    if (fn->scratch_bytes > 0) {
        scratch_obj = nobj;
        obj[nobj++].size = fn->scratch_bytes;
    }
    /* A function returning a composite in memory is handed the address to
     * write it to, and must still have it at the return -- which may be many
     * calls later, and r24:r25 survives none of them. It lives on the
     * frame. */
    F->sret_slot = -1;
    if (fn_sret_bytes(fn)) {
        sret_obj = nobj;
        obj[nobj].size = 2;
        obj[nobj++].weight = 4;          /* stored once, read at each return */
    }

    weigh_objects(F, obj, obj_of);
    off = place_objects(obj, nobj, off);

    for (int v = 0; v < fn->nvregs; v++)
        if (obj_of[v] >= 0)
            F->slot[v] = obj[obj_of[v]].at;
    F->scratch_at = scratch_obj >= 0 ? obj[scratch_obj].at : off;
    if (sret_obj >= 0)
        F->sret_slot = obj[sret_obj].at;
    F->frame = off - 1;
    free(obj);
    free(obj_of);
    free(lref);
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
/* X (r26:r27) as a HOME: offered to the pair pass (g_a_xhome) and then
 * checked, since a few lowerings take X as a scratch pointer. Each such
 * use sets g_x_hit; an attempt that both gave X to a value (g_x_alloc)
 * and used it so is thrown away and generated without it. */
static int g_a_xhome, g_x_hit, g_x_alloc;

static int walk_ptr(int r, int n)
{
    if (r <= AVR_Z + 1 && r + n > AVR_Z) {
        g_x_hit = 1;
        return AVR_X;
    }
    return AVR_Z;
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
/* Y, the frame pointer, is set up only when the function has a frame
 * (F->use_y, see the prologue). Every reader of it goes through here, so
 * a path that needs it where it was not set up is a refusal instead of an
 * access relative to whatever the caller left in r28:r29. */
static void need_y(const struct a_fn *F)
{
    if (!F->use_y)
        internal_error("avr: %s: a frame access in a function whose frame "
                       "pointer was not set up", F->fn->name);
}

static void y_to(struct a_fn *F, int r)
{
    need_y(F);
    avr_movw(F->t, r, AVR_Y);
}

static void ld_slot(struct a_fn *F, int r, long off, int n)
{
    int p, k;
    need_y(F);
    if (off >= 0 && off + n <= 64) {
        for (k = 0; k < n; k++)
            avr_ldd(F->t, r + k, AVR_Y, (int)off + k);
        return;
    }
    p = walk_ptr(r, n);
    if (p == AVR_Z && off > 0 && off + n <= 127) {
        /* Within Y+127: `adiw` moves Z up to 63 in one instruction where
         * subi/sbci take two, and ldd reaches the rest -- Z has a
         * displacement form, X does not. adiw sets SREG as subi does. */
        int a = off > 63 ? 63 : (int)off;
        y_to(F, AVR_Z);
        avr_adiw(F->t, AVR_Z, a);
        for (k = 0; k < n; k++)
            avr_ldd(F->t, r + k, AVR_Z, (int)off - a + k);
        return;
    }
    y_to(F, p);
    add_const16(F, p, off);
    for (k = 0; k < n; k++)
        avr_ld(F->t, r + k, p, AVR_PTR_POST_INC);
}

static void st_slot(struct a_fn *F, long off, int r, int n)
{
    int p, k;
    need_y(F);
    if (off >= 0 && off + n <= 64) {
        for (k = 0; k < n; k++)
            avr_std(F->t, AVR_Y, (int)off + k, r + k);
        return;
    }
    p = walk_ptr(r, n);
    if (p == AVR_Z && off > 0 && off + n <= 127) {   /* as ld_slot */
        int a = off > 63 ? 63 : (int)off;
        y_to(F, AVR_Z);
        avr_adiw(F->t, AVR_Z, a);
        for (k = 0; k < n; k++)
            avr_std(F->t, AVR_Z, (int)off - a + k, r + k);
        return;
    }
    y_to(F, p);
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

/* Read and write only the bytes of a temporary that something reads -- see
 * avr_demand. What stays in the high registers after a narrow read is stale,
 * and nothing that uses it can tell: the only operations allowed a narrow
 * operand are the ones whose low bytes depend on nothing else. */
static int in_pair(const struct a_fn *F, int v)
{
    return F->loc && v >= 0 && F->loc[v] >= 0;
}

/* A slot, for code that addresses one directly. A value in a register pair
 * has none, and reaching such a path with one would read memory nothing
 * wrote: it is a refusal instead. */
static long sslot(const struct a_fn *F, int v)
{
    if (F->remat && v >= 0 && F->remat[v])
        internal_error("avr: %s: a path addresses vreg %d's slot, and it is "
                       "a constant rebuilt where it is read", F->fn->name, v);
    if (in_pair(F, v))
        internal_error("avr: %s: a path addresses vreg %d's slot, and it "
                       "lives in r%d..r%d", F->fn->name, v, F->loc[v],
                       F->loc[v] + F->hw[v] - 1);
    return F->slot[v];
}

/* n bytes of vreg v, from byte `off`, into registers from r -- out of its
 * pair or quad when it has one. That holds every byte anything reads
 * (avr_demand); a wider request leaves r's upper bytes as they were,
 * which is what a narrow slot read does too. mov and movw set no flags,
 * so this serves the flag-preserving _cc paths as well. */
static void ldi4(struct a_fn *F, int r, unsigned long v, int n);

static int is_remat(const struct a_fn *F, int v)
{
    return F->remat && v >= 0 && F->remat[v];
}

static void vld_raw(struct a_fn *F, int r, int v, long off, int n);

/* Does Z still hold v, nothing emitted since it was loaded having written
 * it? Each question decodes only what was emitted since the last one:
 * re-reading everything from the load made a pointer kept in Z across a
 * long function -- a global read at every statement -- quadratic, and an
 * -O2 function of 2000 statements spent most of its time here. What was
 * already read stays read: nothing rewrites an instruction in place but
 * to patch a branch's distance or an ldi's constant, which leaves the
 * register it writes as it was; and a rewind of the code below what was
 * read starts the reading over. */
static int z_holds(struct a_fn *F, int v)
{
    struct code *t = F->t;
    if (F->zv < 0 || F->zv != v)
        return 0;
    long p = F->zscan;
    if (p < F->zat || p > t->len)
        p = F->zat;
    while (p < t->len) {
        int len;
        if (avr_insn_writes(t->p + p, t->len - p, &len) & (3UL << AVR_Z)) {
            F->zv = -1;
            return 0;
        }
        p += len;
    }
    F->zscan = p;
    return 1;
}

/* Z += off before an LPM, which has no displaced form. */
static void z_add(struct code *t, long off)
{
    if (!off)
        return;
    if (off > 0 && off < 64) {
        avr_adiw(t, AVR_Z, (int)off);
        return;
    }
    long neg = -off;
    avr_ri(t, AVR_SUBI, AVR_Z, (int)(neg & 0xff));
    avr_ri(t, AVR_SBCI, AVR_Z + 1, (int)((neg >> 8) & 0xff));
}

static void vld(struct a_fn *F, int r, int v, long off, int n)
{
    int zc = r == AVR_Z && off == 0 && n == 2 && v >= F->fn->nvars;
    if (zc && z_holds(F, v))
        return;                        /* Z still holds this pointer */
    vld_raw(F, r, v, off, n);
    if (zc) {
        F->zv = v;
        F->zat = F->zscan = F->t->len;
    }
}

static void vld_raw(struct a_fn *F, int r, int v, long off, int n)
{
    if (is_remat(F, v)) {
        ldi4(F, r, (unsigned long)F->cval[v] >> (8 * off), n);
        return;
    }
    if (in_pair(F, v) && F->last_st.v == v && F->last_st.r == r &&
        off == 0 && n <= F->last_st.n && F->last_st.at == F->t->len)
        return;              /* just stored from exactly these registers */
    if (in_pair(F, v)) {
        int h = F->loc[v] + (int)off, k = 0;
        int have = F->hw[v] - (int)off;
        if (n > have) n = have;
        while (k < n) {
            if (k + 2 <= n && !((h + k) & 1) && !((r + k) & 1)) {
                if (h != r) avr_movw(F->t, r + k, h + k);
                k += 2;
            } else {
                if (h != r) avr_rr(F->t, AVR_MOV, r + k, h + k);
                k++;
            }
        }
        return;
    }
    ld_slot(F, r, F->slot[v] + off, n);
}
static void vld_cc(struct a_fn *F, int r, int v, long off, int n)
{
    /* ldi, mov and the r31 borrow ldi4 may use leave SREG alone */
    if (in_pair(F, v) || is_remat(F, v)) { vld(F, r, v, off, n); return; }
    ld_slot_cc(F, r, F->slot[v] + off, n);
}
static void vst(struct a_fn *F, int v, long off, int r, int n)
{
    if (in_pair(F, v)) {
        int h = F->loc[v] + (int)off, k = 0;
        int room = F->hw[v] - (int)off;
        if (n > room) n = room;
        while (k < n) {
            if (k + 2 <= n && !((h + k) & 1) && !((r + k) & 1)) {
                if (h != r) avr_movw(F->t, h + k, r + k);
                k += 2;
            } else {
                if (h != r) avr_rr(F->t, AVR_MOV, h + k, r + k);
                k++;
            }
        }
        if (off == 0 && h != r) {      /* r..r+n-1 still hold it (vld) */
            F->last_st.v = v; F->last_st.r = r; F->last_st.n = n;
            F->last_st.at = F->t->len;
        }
        return;
    }
    st_slot(F, F->slot[v] + off, r, n);
}
static void vst_cc(struct a_fn *F, int v, long off, int r, int n)
{
    if (in_pair(F, v)) { vst(F, v, off, r, n); return; }
    st_slot_cc(F, F->slot[v] + off, r, n);
}

/* Where n bytes of v can be READ in place: its home when it holds them,
 * else `scratch` after a load. For the operations that take any register
 * -- add/adc, sub/sbc, and/or/eor, cp/cpc -- so an operand in a home is
 * not first copied into a bank to be read once. */
static int rd_in(struct a_fn *F, int v, int n, int scratch)
{
    if (in_pair(F, v) && n <= F->hw[v])
        return F->loc[v];
    vld(F, scratch, v, 0, n);
    return scratch;
}

/* Where to compute a result of nb bytes: in the destination's home when
 * it holds them, so the value is never copied out of A afterwards; A
 * otherwise. dst_done() commits an A result, and does nothing for a home. */
static int dst_reg(const struct a_fn *F, const struct ir_ins *i, int nb)
{
    if (in_pair(F, i->dst) && nb <= F->hw[i->dst])
        return F->loc[i->dst];
    return RA;
}

static void wr4(struct a_fn *F, int v, int r);
static void dst_done(struct a_fn *F, const struct ir_ins *i, int r)
{
    if (r == RA)
        wr4(F, i->dst, RA);
}

static void rd4(struct a_fn *F, int v, int r)
{
    int n = cw(F, v);
    if (n > VW) n = VW;
    if (n > 0)
        vld(F, r, v, 0, n);
}

static void wr4(struct a_fn *F, int v, int r)
{
    int n = cw(F, v);
    if (n > VW) n = VW;
    if (v >= 0 && (in_pair(F, v) || F->slot[v] >= 0) && n > 0)
        vst(F, v, 0, r, n);
}

static void ldi4(struct a_fn *F, int r, unsigned long v, int n);

/* ---- values wider than the scratch banks ------------------------------
 *
 * An eight-byte value either has a HOME -- a run of eight registers, r18,
 * r10 or r2 up, which the allocator's first pass hands out -- or lives in
 * a slot. Either way it is processed a BYTE AT A TIME: A and B are four
 * bytes each, and an operation between two eight-byte values in scratch
 * would need sixteen.
 *
 * In memory that works because `ldd` and `std` do NOT affect SREG on this
 * machine: the carry from byte k survives the two loads and the store
 * that byte k+1 needs, so an eight-byte add is eight `adc`s with memory
 * traffic between them. In registers it is just the eight `adc`s, in the
 * destination's home -- a quarter of the instructions, and why the homes
 * exist.
 *
 * The byte scratch is two registers outside every home this instruction
 * names (w_scr): r18:r19 normally, r30:r31 when one of its values lives
 * in r18-r25. What it must not be is the destination's own home, which a
 * chain writing byte k and then loading byte k+1 into it would destroy --
 * the one hazard a_verify cannot see.
 */
static int w_home(const struct a_fn *F, int v, int r)
{
    return in_pair(F, v) && r + 1 >= F->loc[v] && r < F->loc[v] + F->hw[v];
}

static int w_scr(const struct a_fn *F, const struct ir_ins *i)
{
    int bad = w_home(F, i->dst, RA) || w_home(F, i->a, RA) ||
              (!i->imm_b && w_home(F, i->b, RA));
    return bad ? AVR_Z : RA;
}

/* Byte k of v, where it can be read: its home, or `scr` after a load
 * that keeps the flags (the chains read these between an add and its
 * adc). An immediate operand when v < 0. */
static int w_byte(struct a_fn *F, int v, long imm, int k, int scr)
{
    if (v >= 0 && is_remat(F, v)) {
        imm = F->cval[v];               /* a constant, rebuilt here */
        v = -1;
    }
    if (v < 0) {
        ldi4(F, scr, (unsigned long)imm >> (8 * k), 1);
        return scr;
    }
    if (in_pair(F, v))
        return F->loc[v] + k;
    ld_slot_cc(F, scr, sslot(F, v) + k, 1);
    return scr;
}

static void wide_bin(struct a_fn *F, const struct ir_ins *i, int n,
                     enum avr_rr first, enum avr_rr rest)
{
    struct code *t = F->t;
    int a = i->a, b = i->imm_b ? -1 : i->b;
    int sc = w_scr(F, i);
    long imm = i->imm;
    if (b >= 0 && is_remat(F, b)) {     /* a constant: the immediate forms */
        imm = F->cval[b];
        b = -1;
    }

    if (in_pair(F, i->dst)) {
        /* In the destination's home: a copied in, then b applied. When d
         * is where b lives, a commutative operation takes them the other
         * way round; a subtraction goes byte by byte below, which reads
         * byte k of b before writing byte k of d and never looks back. */
        int d = F->loc[i->dst];
        if (b >= 0 && in_pair(F, b) && F->loc[b] == d &&
            !(in_pair(F, a) && F->loc[a] == d)) {
            if (first == AVR_SUB)
                goto bytewise;
            int x = a; a = b; b = x;
        }
        vld(F, d, a, 0, n);
        if (b < 0 && d >= 16 && (first == AVR_ADD || first == AVR_SUB)) {
            /* subi/sbci of the constant -- or of its negation for an add,
             * which is the same sum mod 2^64 -- as the four-byte path. */
            unsigned long v = (unsigned long)imm;
            if (first == AVR_ADD) v = 0UL - v;
            for (int k = 0; k < n; k++)
                avr_ri(t, k ? AVR_SBCI : AVR_SUBI, d + k,
                       (int)((v >> (8 * k)) & 0xffu));
            return;
        }
        for (int k = 0; k < n; k++) {
            int c = (int)(((unsigned long)imm >> (8 * k)) & 0xffu);
            if (b < 0 && first == AVR_AND) {
                if (c == 0xff) continue;
                if (c == 0) { avr_rr(t, AVR_MOV, d + k, R_ZERO); continue; }
                if (d + k >= 16) { avr_ri(t, AVR_ANDI, d + k, c); continue; }
            } else if (b < 0 && first == AVR_OR) {
                if (c == 0) continue;
                if (d + k >= 16) { avr_ri(t, AVR_ORI, d + k, c); continue; }
            } else if (b < 0 && first == AVR_EOR) {
                if (c == 0) continue;
                if (c == 0xff) { avr_r1(t, AVR_COM, d + k); continue; }
            }
            avr_rr(t, k ? rest : first, d + k, w_byte(F, b, imm, k, sc + 1));
        }
        return;
    }
bytewise:
    /* A byte of a into the scratch, b applied, the result stored where
     * the destination lives. */
    for (int k = 0; k < n; k++) {
        vld_cc(F, sc, a, k, 1);
        avr_rr(t, k ? rest : first, sc, w_byte(F, b, imm, k, sc + 1));
        vst_cc(F, i->dst, k, sc, 1);
    }
}

/* ---- runs of bytes, two at a time where they pair --------------------
 *
 * `movw` moves an aligned pair to an aligned pair in one instruction,
 * reading both bytes before writing either -- so wherever a byte loop
 * reaches such a pair, doing its two moves at once cannot differ from
 * doing them in turn. These are the byte loops, with that taken.
 *
 * copy_run copies n bytes upward (d below s, or apart), copy_run_down
 * downward (d above s): the order each caller already relied on. */
static void copy_run(struct code *t, int d, int s, int n)
{
    for (int k = 0; k < n; ) {
        if (k + 1 < n && !((d + k) & 1) && !((s + k) & 1)) {
            avr_movw(t, d + k, s + k);
            k += 2;
        } else {
            avr_rr(t, AVR_MOV, d + k, s + k);
            k++;
        }
    }
}

static void copy_run_down(struct code *t, int d, int s, int n)
{
    for (int k = n - 1; k >= 0; ) {
        if (k >= 1 && !((d + k - 1) & 1) && !((s + k - 1) & 1)) {
            avr_movw(t, d + k - 1, s + k - 1);
            k -= 2;
        } else {
            avr_rr(t, AVR_MOV, d + k, s + k);
            k--;
        }
    }
}

/* Bytes r .. r+n-1 all set to the byte in register `src` -- the zero
 * register, or a sign byte. Once an aligned pair of them holds it, each
 * later aligned pair is a copy of that pair: eight bytes of zero are two
 * moves and three movw, not eight moves. `src` may be r-1 itself, which
 * then makes the pair it completes. */
static void fill_run(struct code *t, int r, int n, int src)
{
    int pair = -1;                  /* an even register whose pair is filled */
    for (int k = 0; k < n; ) {
        int q = r + k;
        if (pair >= 0 && !(q & 1) && k + 1 < n) {
            avr_movw(t, q, pair);
            k += 2;
            continue;
        }
        avr_rr(t, AVR_MOV, q, src);
        if ((q & 1) && (k >= 1 || src == q - 1))
            pair = q - 1;
        k++;
    }
}

/* One eight-byte value copied to another's place, whichever has a home. */
static void wide_copy(struct a_fn *F, int dst, int a, int n)
{
    if (in_pair(F, dst)) {
        vld(F, F->loc[dst], a, 0, n);
        return;
    }
    if (in_pair(F, a)) {
        vst(F, dst, 0, F->loc[a], n);
        return;
    }
    if (!is_remat(F, a) && sslot(F, dst) == sslot(F, a))
        return;
    for (int k = 0; k < n; k += 4) {
        int m = n - k < 4 ? n - k : 4;
        vld(F, RA, a, k, m);            /* a slot, or a constant rebuilt */
        st_slot(F, sslot(F, dst) + k, RA, m);
    }
}

/* The byte above `from` valid ones: zero, or the sign of the top one --
 * built in r0, which no home and no scratch here is. */
static int w_fill(struct a_fn *F, int top, int sign)
{
    if (!sign)
        return R_ZERO;
    avr_rr(F->t, AVR_MOV, R_TMP, top);
    avr_rr(F->t, AVR_ADD, R_TMP, R_TMP);        /* lsl: MSB -> carry */
    avr_rr(F->t, AVR_SBC, R_TMP, R_TMP);        /* 0 - carry */
    return R_TMP;
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
    if (from >= to)
        return;
    if (!sign) {
        fill_run(F->t, r + from, to - from, R_ZERO);
        return;
    }
    /* The sign byte is built in the FIRST byte above the value and then
     * copied up, so the source byte is not disturbed: `lsl` would
     * destroy it, which is why this shifts the copy. */
    avr_rr(F->t, AVR_MOV, r + from, r + from - 1);
    avr_rr(F->t, AVR_ADD, r + from, r + from);       /* lsl: MSB -> carry */
    avr_rr(F->t, AVR_SBC, r + from, r + from);       /* 0 - carry */
    fill_run(F->t, r + from + 1, to - from - 1, r + from);
}

/* ---- transient pushes ------------------------------------------------ */

/* A push the body pops again before its instruction's code ends, counted
 * for -fstack-usage (a_fn.tpeak): the frame's number is what the stack
 * must hold, and a byte pushed under it is a byte it must hold too. */
static void t_push(struct a_fn *F, int r)
{
    avr_push(F->t, r);
    if (++F->tdepth > F->tpeak)
        F->tpeak = F->tdepth;
}

static void t_pop(struct a_fn *F, int r)
{
    avr_pop(F->t, r);
    F->tdepth--;
}

/* ---- constants ------------------------------------------------------- */

static void ldi4(struct a_fn *F, int r, unsigned long v, int n)
{
    int borrowed = 0;
    for (int k = 0; k < n; k++) {
        int b = (int)((v >> (8 * k)) & 0xffu);
        /* `ldi rX, 0` and `mov rX, r1` are the same size and the same
         * speed; the move is used so that a zero byte does not depend on
         * the destination being in r16-r31. */
        if (b == 0) {
            avr_rr(F->t, AVR_MOV, r + k, R_ZERO);
        } else if (r + k >= 16) {
            avr_ri(F->t, AVR_LDI, r + k, b);
        } else {
            /* A NONZERO byte into r0-r15, which `ldi` cannot reach. This
             * comment used to say that never happens, because every
             * register this file loads a constant into is a high one --
             * and the 64-bit multiply/divide helpers take their second
             * argument in r17:r10, so at -Os, where the optimizer folds a
             * constant operand into imm_b, `x * 1000000007LL` asked for
             * `ldi r10`. The encoder's range check refused it; without
             * that check it would have encoded a load into r26.
             *
             * Borrowed through r31, saved around the run: push, ldi and
             * mov leave SREG alone, so this is safe inside a carry chain
             * too, and nothing live in Z is disturbed. */
            if (!borrowed) {
                t_push(F, 31);
                borrowed = 1;
            }
            avr_ri(F->t, AVR_LDI, 31, b);
            avr_rr(F->t, AVR_MOV, r + k, 31);
        }
    }
    if (borrowed)
        t_pop(F, 31);
}

/* ---- labels and branches --------------------------------------------- */

static void want_label_site(struct a_fn *F, int at, int label, int wide,
                            int site)
{
    if (F->nfix == F->capfix) {
        F->capfix = F->capfix ? F->capfix * 2 : 16;
        F->fix = xrealloc(F->fix, (size_t)F->capfix * sizeof *F->fix);
    }
    F->fix[F->nfix].at = at;
    F->fix[F->nfix].label = label;
    F->fix[F->nfix].wide = wide;
    F->fix[F->nfix].site = site;
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
/* A new jump site, and the form the relaxation hint allows it. `cond` is
 * the condition of a conditional one, -1 for an unconditional one. */
static int jump_site(struct a_fn *F, int label, int cond, int *hint)
{
    int k = F->njs;
    if (F->njs == F->capjs) {
        F->capjs = F->capjs ? F->capjs * 2 : 16;
        F->js = xrealloc(F->js, (size_t)F->capjs * sizeof *F->js);
    }
    F->js[k].at = F->t->len;
    F->js[k].label = label;
    F->js[k].cond = cond;
    F->njs++;
    *hint = F->rx && k < F->rx->nsite ? F->rx->hint[k] : 0;
    return k;
}

/* The jump itself, after any inverted skip: rjmp when the target is
 * behind and in reach or the hint says it will be, else a jmp. */
static void emit_jump(struct a_fn *F, int label, int site, int short_ok)
{
    long here = F->t->len;
    int known = label <= F->fn->nlabels && F->label_off[label] >= 0;
    if (known) {
        long d = (F->label_off[label] - (here + 2)) / 2;
        if (d >= -2048 && d <= 2047) {
            want_label_site(F, avr_rjmp(F->t, 0), label, 0, site);
            return;
        }
    }
    if (short_ok) {
        want_label_site(F, avr_rjmp(F->t, 0), label, 0, site);
        return;
    }
    want_label_site(F, F->t->len, label, 1, site);
    avr_jmp(F->t, 0);
}

/* An unconditional jump to a label.
 *
 * Backward and in reach: `rjmp`, two bytes. Forward, the long 32-bit
 * `jmp` whose absolute address the linker fills in -- unless relaxation
 * has shown an rjmp reaches (struct avr_relax). The choice has to be made
 * HERE and not at patch time, because the two are different sizes. At -O0
 * a single 64-bit statement is hundreds of instructions, so a loop's back
 * edge really does leave rjmp's +-4 KB; before the range check, that
 * displacement was masked to twelve bits and a back edge 2300 words
 * behind became a forward jump 1866 words ahead, into empty flash. */
static void jump_to(struct a_fn *F, int label)
{
    int h, k = jump_site(F, label, -1, &h);
    if (h != 3)                   /* 3: the target is the next instruction */
        emit_jump(F, label, k, h >= 1);
    F->js[k].end = F->t->len;
}

/* A conditional branch to a label: a direct `br` when the target is in
 * its +-64 words -- behind and measured, or ahead and shown to be by
 * relaxation -- and otherwise an INVERTED br skipping the jump, since
 * `cond ^ 1` is the inversion (the condition enum pairs each sense with
 * its opposite in bit 0).
 *
 * The inverted form was the only form until relaxation: a br reaches 126
 * bytes, less than one loop body on a machine where an int add is four
 * instructions, and the distance of a forward target is not known here. */
static void jump_if(struct a_fn *F, enum avr_cond cond, int label)
{
    int h, k = jump_site(F, label, (int)cond, &h);
    long at = F->t->len;
    F->js[k].end = -1;
    int known = label <= F->fn->nlabels && F->label_off[label] >= 0;
    int before;
    if (known) {
        long d = (F->label_off[label] - (at + 2)) / 2;
        if (d >= -64 && d <= 63) {
            avr_br(F->t, cond, (int)d);
            return;
        }
    } else if (h == 2) {
        want_label_site(F, avr_br(F->t, cond, 0), label, 2, k);
        return;
    }
    /* The inverted branch skips the jump, whose size depends on how far
     * the target is -- so the skip is over one word or two. */
    avr_br(F->t, (enum avr_cond)(cond ^ 1), 1);
    before = F->t->len;
    emit_jump(F, label, k, h >= 1);
    if (F->t->len - before != 2)
        avr_patch_br(F->t, (int)at, (int)((F->t->len - (at + 2)) / 2));
}

/* ---- a select, which on this machine is a branch --------------------- */

/* dst = cond ? b : c, at `n` bytes.
 *
 * There is no conditional move on AVR, so this is the branch the optimizer's
 * if-conversion pass just removed. That sounds like a reason to stop the pass
 * on this target, and it is not: the pass runs for every target and a backend
 * that has no IR_SELECT refuses ordinary code. `long pick(int c, long a, long
 * b) { return c ? a : b; }` compiled at -O0 and -O1 and REFUSED at -O2, which
 * is the worst shape a gap can have -- the same source, the same target, and
 * the optimisation level decides whether it builds.
 *
 * Two-byte values never showed it, because if-conversion asks for a 4- or
 * 8-byte arm and an `int` here is two. So the hole was open for exactly the
 * widths a program is most likely to write.
 *
 * A byte at a time through RA rather than a wide load: the arms can be eight
 * bytes and there is no eight-register run to spare, and no flag has to
 * survive a copy.
 */
static void copy_slot(struct a_fn *F, long dst, long src, int n)
{
    if (dst == src)
        return;
    for (int k = 0; k < n; k++) {
        ld_slot(F, RA, src + k, 1);
        st_slot(F, dst + k, RA, 1);
    }
}

static void gen_select(struct a_fn *F, const struct ir_ins *i, int n)
{
    struct code *t = F->t;
    long br_at, rj_at, join;

    /* The condition is a value, not flags: OR its bytes so Z answers
     * "was it zero". PLAIN ld_slot, as IR_BRZ uses, because r0 is the
     * accumulator and the flag-preserving path saves SREG through it. */
    if (i->size == 8) {
        /* A 64-bit condition -- its own width, `size`, not the arms'.
         * Four bytes were read whatever it was, so 2^32 was false. Byte
         * by byte, as the 64-bit IR_BRZ reads it. */
        avr_rr(t, AVR_MOV, R_TMP, R_ZERO);
        for (int k = 0; k < 8; k++) {
            int r = RA;
            if (in_pair(F, i->a))
                r = F->loc[i->a] + k;
            else
                vld(F, RA, i->a, k, 1);
            avr_rr(t, AVR_OR, R_TMP, r);
        }
    } else {
        rd4(F, i->a, RA);
        avr_rr(t, AVR_MOV, R_TMP, RA);
        for (int k = 1; k < VW; k++)
            avr_rr(t, AVR_OR, R_TMP, RA + k);
    }

    /* An INVERTED branch over an rjmp, not a branch over the arm: a br
     * reaches +-63 words and an eight-byte copy out of a far slot is more
     * than that. Written the direct way, this passed at -O0 and -O1 and
     * tripped its own range check at -O2, where the frame had grown enough
     * to put the arms past ldd's reach -- 65 words for a branch that can
     * carry 63. jump_if() above has the same shape for the same reason. */
    avr_br(t, AVR_BR_NE, 1);            /* nonzero -> fall into the true arm */
    br_at = t->len;
    avr_rjmp(t, 0);                     /* ...zero -> the false arm */
    copy_slot(F, sslot(F, i->dst), sslot(F, i->b), n);
    rj_at = t->len;
    avr_rjmp(t, 0);
    join = t->len;
    /* Both displacements are measured, not predicted: a slot past ldd's
     * six-bit reach costs extra instructions, so neither arm has a size
     * this code can know in advance. */
    avr_patch_rjmp(t, (int)br_at, (int)((join - (br_at + 2)) / 2));
    copy_slot(F, sslot(F, i->dst), sslot(F, i->c), n);
    avr_patch_rjmp(t, (int)rj_at, (int)((t->len - (rj_at + 2)) / 2));
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
    st->ext[st->next].tail = 0;
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
    st->f[st->nf].addend = 0;
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
/* The shifts work on `w` bytes, VW unless the caller knows fewer carry
 * the answer (the IR_SHL/IR_SHR lowering): a left shift's low bytes
 * depend only on the low bytes, and a right shift of an extension is the
 * extension of the shift, when the kinds agree. */
static void shift1w(struct a_fn *F, int r, int op, int sign, int w)
{
    if (op == IR_SHL) {
        avr_rr(F->t, AVR_ADD, r, r);                  /* lsl r */
        for (int k = 1; k < w; k++)
            avr_rr(F->t, AVR_ADC, r + k, r + k);      /* rol r+k */
    } else {
        avr_r1(F->t, sign ? AVR_ASR : AVR_LSR, r + w - 1);
        for (int k = w - 2; k >= 0; k--)
            avr_r1(F->t, AVR_ROR, r + k);
    }
}

static void shift_immw(struct a_fn *F, int r, int op, int sign, long n,
                       int VWn);

static void shift_immw(struct a_fn *F, int r, int op, int sign, long n,
                       int VWn)
{
    int bytes, k;

    if (n <= 0)
        return;
    /* A count at or past the width is undefined in C. Producing all
     * zeroes (or all sign) is the honest answer and keeps the emitted
     * sequence bounded; the alternative is 32 shift instructions for a
     * program that was already wrong. */
    if (n >= 8 * VWn)
        n = 8 * VWn - (op == IR_SHL || !sign ? 0 : 1);
    bytes = (int)(n / 8);
    n -= 8 * bytes;

    if (bytes > 0) {
        if (op == IR_SHL) {
            if (VWn > bytes)
                copy_run_down(F->t, r + bytes, r, VWn - bytes);
            fill_run(F->t, r, bytes < VWn ? bytes : VWn, R_ZERO);
        } else {
            /* The fill byte is built BEFORE the moves, from the top byte
             * that is about to be overwritten. */
            int fill = R_ZERO;
            if (sign) {
                avr_rr(F->t, AVR_MOV, R_TMP, r + VWn - 1);
                avr_rr(F->t, AVR_ADD, R_TMP, R_TMP);
                avr_rr(F->t, AVR_SBC, R_TMP, R_TMP);
                fill = R_TMP;
            }
            if (VWn > bytes)
                copy_run(F->t, r, r + bytes, VWn - bytes);
            k = VWn - bytes < 0 ? 0 : VWn - bytes;
            fill_run(F->t, r + k, VWn - k, fill);
        }
    }
    for (k = 0; k < (int)n; k++)
        shift1w(F, r, op, sign, VWn);
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
static void shift_varw(struct a_fn *F, int r, int cnt, int op, int sign,
                       int w)
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
    shift1w(F, r, op, sign, w);
    back = avr_rjmp(F->t, 0);
    avr_patch_rjmp(F->t, back, (top - (back + 2)) / 2);
    avr_patch_rjmp(F->t, exitj, (F->t->len - (exitj + 2)) / 2);
}

static int a_calls_helper(const struct ir_ins *i);
static const char *avr_knob(const char *name);
static int g_a_vol;
static char *g_a_novol, *g_a_nohome, *g_a_pend;
static int g_a_nvr;

/* ---- the proof -------------------------------------------------------
 *
 * B and X are homes AND scratch. Nothing about an instruction's lowering
 * says in advance which it will need -- whether `a + b` loads b into B
 * depends on where b ended up -- so instead of a rule, a check: after
 * each IR instruction, decode what was emitted for it (avr_insn_writes)
 * and see that no register it wrote held something still needed:
 *
 *  - a value live after the instruction, other than the one it defines;
 *  - an operand, except where it shares the destination's home (the
 *    two-address case, `p = p + 1` in one pair, which writes it by
 *    design). A call's operands are exempt too: its argument move writes
 *    registers after reading them, which is its whole job, and nothing
 *    call-clobbered is live across one.
 *
 * A value caught is kept out of that class of home on the next attempt
 * (avr_attempt) -- the call-clobbered ones if it was in one, else all --
 * and the function is generated again, until nothing is caught. Every
 * home is checked, the call-saved ones too, though only B and X are
 * expected to fail: a hit there is a lowering that writes a home it was
 * not given, and the retry is sound either way.
 *
 * What this cannot see is a lowering that uses the destination's own
 * home as scratch before writing the result into it; the lowerings that
 * compute in place (dst_reg) take their scratch from whichever bank the
 * destination is not, for that reason. */
static unsigned long home_mask(const struct a_fn *F, int v)
{
    if (!in_pair(F, v))
        return 0;
    return ((1UL << F->hw[v]) - 1) << F->loc[v];
}

static void a_conflict(struct a_fn *F, int v)
{
    int r = F->loc[v];
    if (!g_a_pend || v < 0 || v >= g_a_nvr)
        internal_error("avr: %s: vreg %d's home r%d was overwritten outside "
                       "an attempt that can be retried", F->fn->name, v, r);
    g_a_pend[v] |= (r >= 22 && r <= 27) ? 1 : 2;
    if (avr_knob("EMBCC_AVR_RA"))
        fprintf(stderr, "%s: %%%d overwritten in r%d\n", F->fn->name, v, r);
}

struct a_vuse { struct a_fn *F; unsigned long w, own; };

static void a_verify_use(int v, void *p)
{
    struct a_vuse *c = p;
    if (home_mask(c->F, v) & ~c->own & c->w)
        a_conflict(c->F, v);
}

/* What a_verify needs of liveness: per instruction, the values live
 * out of it that have a home -- the only ones a write can clobber --
 * ascending (hl_v[hl_off[k] .. hl_off[k + 1])). It used to be the whole
 * live-out bit set of every instruction; a walk back through each block
 * (ra_lset_step) keeps the homed ones aside as it goes, so building this
 * costs what changes at each instruction and what it holds. */
struct a_homed { const struct a_fn *F; struct ra_lset *h; };
static void a_homed_chg(int v, int added, void *ctx)
{
    struct a_homed *c = ctx;
    if (!home_mask(c->F, v))
        return;
    if (added) ra_lset_add(c->h, v);
    else       ra_lset_del(c->h, v);
}

static void a_homed_map(const struct a_fn *F, int **off_out, int **v_out)
{
    const struct ir_func *fn = F->fn;
    int nv = fn->nvregs, nins = fn->nins;
    int *lf = xmalloc((size_t)nv * sizeof *lf);
    int *ll = xmalloc((size_t)nv * sizeof *ll);
    struct ra_live *lv = ra_live_compute(fn, lf, ll);
    struct ra_lset s, h;
    struct a_homed c;
    ra_lset_init(&s, nv);
    ra_lset_init(&h, nv);
    c.F = F;
    c.h = &h;
    s.chg = a_homed_chg;
    s.chg_ctx = &c;
    /* first the counts, then the lists: two walks */
    int *off = xcalloc((size_t)nins + 1, sizeof *off);
    int *val = NULL;
    for (int pass = 0; pass < 2; pass++) {
        for (int b = ra_live_nblocks(lv) - 1; b >= 0; b--) {
            ra_lset_out(&s, lv, b);
            for (int i = ra_live_block_start(lv, b + 1) - 1;
                 i >= ra_live_block_start(lv, b); i--) {
                if (!pass) {
                    off[i + 1] = h.n;
                } else {
                    int *o = val + off[i];
                    for (int k = 0; k < h.n; k++) {   /* insertion, ascending */
                        int x = h.mem[k], j = k;
                        while (j > 0 && o[j - 1] > x) { o[j] = o[j - 1]; j--; }
                        o[j] = x;
                    }
                }
                ra_lset_step(&s, &fn->ins[i]);
            }
        }
        if (!pass) {
            for (int i = 0; i < nins; i++) off[i + 1] += off[i];
            val = xmalloc((size_t)(off[nins] ? off[nins] : 1) * sizeof *val);
        }
    }
    ra_lset_free(&s); ra_lset_free(&h);
    ra_live_free(lv);
    free(lf); free(ll);
    *off_out = off;
    *v_out = val;
}

/* IR instructions n..last were emitted from byte `at` on. */
static void a_verify(struct a_fn *F, int n, int last, long at,
                     const int *hl_off, const int *hl_v)
{
    struct code *t = F->t;
    unsigned long w = 0;
    for (long p = at; p < t->len; ) {
        int len;
        unsigned long m = avr_insn_writes(t->p + p, t->len - p, &len);
        /* `or r, r` -- the zero test a branch reads -- and `and r, r` and
         * `mov r, r` write r with what it already held: no value is lost. */
        if (len == 2) {
            unsigned op = (unsigned)t->p[p] | (unsigned)t->p[p + 1] << 8;
            unsigned d = (op >> 4) & 0x1f, r = (op & 0xf) | ((op >> 5) & 0x10);
            if (d == r && ((op & 0xfc00) == 0x2800 || (op & 0xfc00) == 0x2000 ||
                           (op & 0xfc00) == 0x2c00))
                m = 0;
        }
        w |= m;
        p += len;
    }
    if (!w)
        return;
    for (int k = n; k <= last; k++) {
        const struct ir_ins *i = &F->fn->ins[k];
        for (int q = hl_off[k]; q < hl_off[k + 1]; q++) {
            int v = hl_v[q];
            if (v != i->dst && (home_mask(F, v) & w))
                a_conflict(F, v);
        }
        /* The operands -- not a fused branch's, whose one operand is the
         * comparison it replaced and never existed. */
        if (k == n && i->op != IR_CALL && !a_calls_helper(i)) {
            struct a_vuse c;
            c.F = F; c.w = w;
            c.own = i->dst >= 0 ? home_mask(F, i->dst) : 0;
            ra_each_use(i, a_verify_use, &c);
        }
    }
}

/* Emit the moves ra_parallel_move ordered, a byte each -- or two at once
 * with movw where consecutive moves are an aligned pair to an aligned
 * pair. Doing those two together cannot differ from doing them in turn:
 * the first writes an even register and the second reads an odd one. */
static void emit_moves(struct code *t, const int *od, const int *os, int no)
{
    for (int q = 0; q < no; q++) {
        if (q + 1 < no && !(od[q] & 1) && !(os[q] & 1) &&
            od[q + 1] == od[q] + 1 && os[q + 1] == os[q] + 1) {
            avr_movw(t, od[q], os[q]);
            q++;
        } else {
            avr_rr(t, AVR_MOV, od[q], os[q]);
        }
    }
}

static void set_sp_from_y(struct code *t);
static void set_sp_from(struct code *t, int lo);

/* The registers the epilogue pops: the saved pairs, and Y when it was
 * set up. */
static unsigned long a_saved_mask(const struct a_fn *F)
{
    unsigned long m = F->use_y ? 3UL << 28 : 0;
    for (int k = 0; k < F->nsave; k++)
        m |= 3UL << F->used_callee[k];
    return m;
}

/* The epilogue's teardown, short of the `ret`: the frame, Y, the saved
 * pairs. Shared by the epilogue and a tail call, which must leave the
 * stack exactly as the `ret` would find it. */
static void a_teardown(struct a_fn *F)
{
    struct code *t = F->t;
    /* after an alloca sp is below the frame, so it is set from Y even
     * when there is no frame to add */
    if (F->frame)
        add_const16(F, AVR_Y, F->frame);
    if (F->frame || F->fn->has_alloca)
        set_sp_from_y(t);
    if (F->use_y) {
        avr_pop(t, 29);
        avr_pop(t, 28);
    }
    for (int k = F->nsave - 1; k >= 0; k--) {
        avr_pop(t, F->used_callee[k] + 1);
        avr_pop(t, F->used_callee[k]);
    }
}

/* Can the call at n be a TAIL call -- the frame and the saved pairs
 * popped, then `jmp` to the callee, which returns to this function's
 * caller through the address still on the stack? Only when nothing of
 * this frame can still be needed: every argument in registers, no struct
 * result, no local whose address could have escaped into the callee, and
 * the IR_RET after it returning exactly the call's result -- or nothing,
 * at the end of a function that returns nothing. Not through a pointer,
 * not variadic, not a float, and not from an interrupt handler, whose
 * epilogue is reti. */
static int a_tail_ok(const struct a_fn *F, int n)
{
    const struct ir_func *fn = F->fn;
    const struct ir_ins *i = &fn->ins[n];
    struct argplace pl;
    int cursor = ARG_TOP;
    long stk = 0;
    int nret = 0;

    if (i->op != IR_CALL || i->indirect || i->call_varargs || i->retsize ||
        i->flt || avr_knob("EMBCC_NO_TAILCALL"))
        return 0;
    if (fn->ret_abi.is_struct || fn->ret_abi.is_float || fn->is_varargs ||
        fn->src->is_isr || fn->has_alloca || fn->neh)
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
                          i->ret_tybytes > 8))
            return 0;
    }
    for (int k = 0; k < fn->nins; k++) {
        if (fn->ins[k].op == IR_ADDR || fn->ins[k].op == IR_VA_START)
            return 0;
        nret += fn->ins[k].op == IR_RET;
    }
    for (int k = 0; k < i->nargs; k++) {
        place_arg(i->argv[k].size, &cursor, &stk, &pl);
        if (pl.nstk)
            return 0;
    }
    /* 2 when the function returns anywhere else: then the tail call is
     * worth its copy of the teardown only if that copy is empty. */
    return nret > 1 ? 2 : 1;
}

/* Where an address is built: the destination's home when `ldi` reaches
 * it and it holds every byte read, else A. */
static int addr_reg(const struct a_fn *F, const struct ir_ins *i)
{
    int nb = dw(F, i);
    if (in_pair(F, i->dst) && F->loc[i->dst] >= 16 &&
        (nb < 2 ? 2 : nb) <= F->hw[i->dst])
        return F->loc[i->dst];
    return RA;
}

/* ---- -g --------------------------------------------------------------- */

/* A line-table row wherever the source line changes, at the offset the
 * instruction's code begins -- what the other backends record, and what
 * was missing here: an AVR object's .debug_line held one empty sequence,
 * so no address had a line and no line had an address. Two instructions
 * that emit nothing between them leave one row, the later line's. */
static void a_line_row(struct ir_func *fn, long off, int line)
{
    struct ir_line *last;
    if (!line)
        return;
    last = fn->nlines ? &fn->lines[fn->nlines - 1] : (struct ir_line *)0;
    if (last && last->off == off) {
        last->line = line;
        return;
    }
    if (last && last->line == line)
        return;
    if (fn->nlines == fn->linecap) {
        fn->linecap = fn->linecap ? fn->linecap * 2 : 8;
        fn->lines = xrealloc(fn->lines,
                             (size_t)fn->linecap * sizeof *fn->lines);
    }
    fn->lines[fn->nlines].off = off;
    fn->lines[fn->nlines].line = line;
    fn->nlines++;
}


/* ---- the instruction dispatch ---------------------------------------- */

/* ---- atomics -------------------------------------------------------
 *
 * No AVR instruction is an atomic read-modify-write, so each is done with
 * interrupts masked -- SREG saved in r0, cli, the access, SREG restored,
 * which puts the I flag back as it was -- as avr-libc's ATOMIC_BLOCK does
 * it: one core, so masking interrupts is all the atomicity there is to
 * have. The same goes for an atomic load or store wider than a byte,
 * which AVR's byte-wide memory accesses would otherwise split: irgen makes
 * a store an exchange and a load an IR_ARMW of 'L', which reads and
 * writes nothing back. No library call, at any size. */
static void gen_atomic8(struct a_fn *F, const struct ir_ins *i);

static void gen_atomic(struct a_fn *F, const struct ir_ins *i)
{
    struct code *t = F->t;

    /* An atomic read-modify-write. No AVR instruction is one, so it is
     * done with interrupts masked -- SREG saved in r0, cli, the
     * access, SREG restored -- as avr-libc's ATOMIC_BLOCK does it: one
     * core, so masking interrupts is all the atomicity there is to
     * have.
     *
     * Every operand goes through the STACK first. A and B are the
     * first argument registers, so an operand's home may be in the
     * very bank another is loaded into; pushed from wherever it is and
     * popped into place, none is read after any bank is written. And
     * inside the window nothing touches r0 (a far slot borrows it):
     * the compare's byte goes in r1, which no one sees until it is
     * cleared again. */
    int n = i->size, nb = dw(F, i), op = i->op;
    /* an atomic load (irgen's atomic_load): no operand, no store */
    int load = op == IR_ARMW && i->imm == 'L';
    if (n == 8) {
        gen_atomic8(F, i);
        return;
    }
    if (n != 1 && n != 2 && n != 4)
        a_refuse(F->fn, i, "an atomic of this size");
    /* push_v: v's n bytes, byte 0 first, so they pop high first */
#define PUSH_V(v, cnt) do { for (int k_ = 0; k_ < (cnt); k_++) { \
        vld(F, 26, (v), k_, 1); t_push(F, 26); } } while (0)
#define POP_TO(r, cnt) do { for (int k_ = (cnt) - 1; k_ >= 0; k_--) \
        t_pop(F, (r) + k_); } while (0)
    if (op == IR_CAS) {
        PUSH_V(i->b, n);                    /* expected, stays below */
        PUSH_V(i->a, 2);
        PUSH_V(i->c, n);
        POP_TO(RB, n);                      /* desired */
        POP_TO(AVR_Z, 2);
    } else if (op == IR_CMPXCHG) {
        PUSH_V(i->b, 2);                    /* where expected is */
        PUSH_V(i->a, 2);
        PUSH_V(i->c, n);
        POP_TO(RB, n);
        POP_TO(AVR_Z, 2);
        POP_TO(AVR_X, 2);
    } else if (load) {
        vld(F, AVR_Z, i->a, 0, 2);
    } else {
        PUSH_V(i->a, 2);
        PUSH_V(i->b, n);
        POP_TO(RB, n);                      /* the operand */
        POP_TO(AVR_Z, 2);
    }
#undef PUSH_V
#undef POP_TO
    F->zv = -1;
    avr_in(t, R_TMP, IO_SREG);
    avr_bclr(t, AVR_SREG_I);
    for (int k = 0; k < n; k++)
        avr_ldd(t, RA + k, AVR_Z, k);       /* the old value */
    if (op == IR_CAS || op == IR_CMPXCHG) {
        /* old == expected, byte by byte: cp then cpc, which only
         * ever clears Z */
        for (int j2 = 0; j2 < n; j2++) {
            /* the value form's expected bytes pop high first; *b's
             * are read low first through X */
            int k = op == IR_CAS ? n - 1 - j2 : j2;
            if (op == IR_CAS)
                t_pop(F, R_ZERO);
            else
                avr_ld(t, R_ZERO, AVR_X, AVR_PTR_POST_INC);
            avr_rr(t, j2 ? AVR_CPC : AVR_CP, RA + k, R_ZERO);
        }
        int br = avr_br(t, AVR_BR_NE, 0);
        for (int k = 0; k < n; k++)
            avr_std(t, AVR_Z, k, RB + k);
        if (op == IR_CMPXCHG)
            avr_ri(t, AVR_LDI, RB, 1);      /* swapped */
        int j = avr_rjmp(t, 0);
        avr_patch_br(t, br, (t->len - (br + 2)) / 2);
        if (op == IR_CMPXCHG)
            avr_ri(t, AVR_LDI, RB, 0);
        avr_patch_rjmp(t, j, (t->len - (j + 2)) / 2);
    } else if (!load) {
        for (int k = 0; k < n; k++) {
            switch (op == IR_ARMW ? (int)i->imm : op == IR_XADD ? '+' : 0) {
            case '+': avr_rr(t, k ? AVR_ADC : AVR_ADD, RB + k, RA + k); break;
            case '&': avr_rr(t, AVR_AND, RB + k, RA + k); break;
            case '|': avr_rr(t, AVR_OR, RB + k, RA + k); break;
            case '^': avr_rr(t, AVR_EOR, RB + k, RA + k); break;
            case 'n':
                avr_rr(t, AVR_AND, RB + k, RA + k);
                avr_r1(t, AVR_COM, RB + k);
                break;
            default: break;                 /* exchange */
            }
        }
        for (int k = 0; k < n; k++)
            avr_std(t, AVR_Z, k, RB + k);
    }
    /* r1 zero again BEFORE interrupts can come back; the eor's flags
     * are then overwritten by the SREG restored */
    if (op == IR_CAS || op == IR_CMPXCHG)
        avr_rr(t, AVR_EOR, R_ZERO, R_ZERO);
    avr_out(t, IO_SREG, R_TMP);
    if (op == IR_CMPXCHG) {
        /* *b = what was there; X walked n past it reading */
        avr_sbiw(t, AVR_X, n);
        for (int k = 0; k < n; k++)
            avr_st(t, AVR_X, RA + k, AVR_PTR_POST_INC);
        avr_rr(t, AVR_MOV, RA, RB);         /* the flag is the result */
        n = 1;
    }
    {
        int d = dst_reg(F, i, nb);
        int m = n < nb ? n : nb;
        for (int k = 0; k < m; k++)
            if (d + k != RA + k)
                avr_rr(t, AVR_MOV, d + k, RA + k);
        if (nb > n)
            extend(F, d, n, op == IR_CMPXCHG ? 0 : i->sign, nb);
        dst_done(F, i, d);
    }
    return;
}

/* An eight-byte atomic. The old value takes r18-r25, both scratch banks,
 * so the operand cannot be in registers too: it waits on the stack,
 * pushed high byte first, and is popped a byte at a time INSIDE the
 * window -- pop, mov, ldd and std leave SREG alone, so an add's carry
 * runs from byte to byte through them. r26 takes each popped byte and
 * r27 each new one; a CMPXCHG, whose X points at the expected value,
 * uses r1 instead and clears it before interrupts come back. The flag a
 * CMPXCHG returns is made in r30, once Z is done with. */
static void gen_atomic8(struct a_fn *F, const struct ir_ins *i)
{
    struct code *t = F->t;
    int op = i->op, n = 8;
    int load = op == IR_ARMW && i->imm == 'L';
    /* v's bytes pushed high first, so they pop low first */
#define PUSH_HI(v, cnt) do { for (int k_ = (cnt) - 1; k_ >= 0; k_--) { \
            vld(F, 26, (v), k_, 1); t_push(F, 26); } } while (0)
    if (op == IR_CAS) {
        PUSH_HI(i->c, n);                   /* desired, below */
        PUSH_HI(i->b, n);                   /* expected, on top */
    } else if (op == IR_CMPXCHG) {
        PUSH_HI(i->c, n);
        PUSH_HI(i->b, 2);                   /* where expected is */
    } else if (!load) {
        PUSH_HI(i->b, n);
    }
    PUSH_HI(i->a, 2);
#undef PUSH_HI
    t_pop(F, AVR_Z);
    t_pop(F, AVR_Z + 1);
    if (op == IR_CMPXCHG) {
        t_pop(F, AVR_X);
        t_pop(F, AVR_X + 1);
    }
    F->zv = -1;
    avr_in(t, R_TMP, IO_SREG);
    avr_bclr(t, AVR_SREG_I);
    for (int k = 0; k < n; k++)
        avr_ldd(t, RA + k, AVR_Z, k);           /* the old value */
    if (op == IR_CAS || op == IR_CMPXCHG) {
        /* old == expected: cp then cpc, which only ever clear Z */
        int e = op == IR_CAS ? 26 : R_ZERO;
        for (int k = 0; k < n; k++) {
            if (op == IR_CAS)
                t_pop(F, e);
            else
                avr_ld(t, e, AVR_X, AVR_PTR_POST_INC);
            avr_rr(t, k ? AVR_CPC : AVR_CP, RA + k, e);
        }
        int br = avr_br(t, AVR_BR_NE, 0);
        for (int k = 0; k < n; k++) {
            t_pop(F, e);
            avr_std(t, AVR_Z, k, e);
        }
        if (op == IR_CMPXCHG)
            avr_ri(t, AVR_LDI, AVR_Z, 1);       /* swapped */
        int j = avr_rjmp(t, 0);
        avr_patch_br(t, br, (t->len - (br + 2)) / 2);
        for (int k = 0; k < n; k++)
            t_pop(F, AVR_Z + 1);              /* desired, unused */
        if (op == IR_CMPXCHG)
            avr_ri(t, AVR_LDI, AVR_Z, 0);
        avr_patch_rjmp(t, j, (t->len - (j + 2)) / 2);
        if (op == IR_CMPXCHG)
            avr_rr(t, AVR_EOR, R_ZERO, R_ZERO);
    } else if (!load) {
        int c = op == IR_ARMW ? (int)i->imm : op == IR_XADD ? '+' : 0;
        for (int k = 0; k < n; k++) {
            t_pop(F, 26);
            if (c) {
                avr_rr(t, AVR_MOV, 27, RA + k);
                switch (c) {
                case '+': avr_rr(t, k ? AVR_ADC : AVR_ADD, 27, 26); break;
                case '&': avr_rr(t, AVR_AND, 27, 26); break;
                case '|': avr_rr(t, AVR_OR, 27, 26); break;
                case '^': avr_rr(t, AVR_EOR, 27, 26); break;
                default:                        /* nand */
                    avr_rr(t, AVR_AND, 27, 26);
                    avr_r1(t, AVR_COM, 27);
                    break;
                }
            }
            avr_std(t, AVR_Z, k, c ? 27 : 26);
        }
    }
    avr_out(t, IO_SREG, R_TMP);
    if (op == IR_CMPXCHG) {
        /* *b = what was there; X walked eight past it reading */
        avr_sbiw(t, AVR_X, n);
        for (int k = 0; k < n; k++)
            avr_st(t, AVR_X, RA + k, AVR_PTR_POST_INC);
        int nb = dw(F, i), d = dst_reg(F, i, nb);
        avr_rr(t, AVR_MOV, d, AVR_Z);
        extend(F, d, 1, 0, nb);
        dst_done(F, i, d);
        return;
    }
    if (in_pair(F, i->dst) || F->slot[i->dst] >= 0)
        vst(F, i->dst, 0, RA, n);
}

static void gen_ins(struct a_fn *F, int n)
{
    struct ir_func *fn = F->fn;
    struct code *t = F->t;
    const struct ir_ins *i = &fn->ins[n];

    /* Everything this backend has not learned, named before anything is
     * emitted for it. The width test comes first because it applies to
     * every arithmetic op at once. */

    /* Eight-byte values are handled per operation below, through the
     * byte-at-a-time chain rather than in registers. The ones that have no
     * such path say so where they are. */
    if (i->w == 16)
        a_refuse(fn, i, "a 128-bit value");
    /* Level 0 only (irgen): AVR code keeps no frame-pointer chain. The
     * call pushed the return address above the saved Y (see "the frame"),
     * a WORD address, high byte at the lower address -- which is what a
     * function pointer holds here -- and the stack pointer at entry is
     * the byte below it: the frame address. A function that asks always
     * sets up Y. An interrupt handler pushes more above Y, and what it
     * returns to was not a call. */
    if (i->op == IR_FRAMEADDR) {
        if (fn->src && fn->src->is_isr)
            a_refuse(fn, i, "__builtin_frame_address or "
                            "__builtin_return_address in an interrupt "
                            "handler");
        int nb = dw(F, i), d = addr_reg(F, i);
        long hi = F->frame + F->in_at - 2;   /* the return address's high */
        if (!nb)
            return;
        if (i->imm == 2) {
            ld_slot(F, d + 1, hi, 1);
            ld_slot(F, d, hi + 1, 1);
        } else {
            y_to(F, d);
            add_const16(F, d, hi - 1);
        }
        if (nb > 2)
            extend(F, d, 2, 0, nb);
        if (d == RA)
            wr4(F, i->dst, RA);
        return;
    }
    /* at every size, before the eight-byte dispatch takes the w == 8 ones */
    if (i->op == IR_XCHG || i->op == IR_XADD || i->op == IR_ARMW ||
        i->op == IR_CAS || i->op == IR_CMPXCHG) {
        gen_atomic(F, i);
        return;
    }
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

    /* ---- floating point: every operation is a call --------------------
     *
     * `float` AND `double` are both four-byte binary32 here (avr-gcc's
     * documented default, and what the data model in src/arch/target.c says),
     * so there is no `df` family to emit -- a double IS a float, and F2F
     * between them is a move. lib/rt/avrfp.c is what these call.
     *
     * A float is four bytes, so it travels exactly as a `long` does: the
     * cursor gives r25:r22 for the first argument and r21:r18 for the second,
     * which is where A and B already sit. */
    if (i->flt) {
        static const struct { enum ir_op op; const char *name; } fops[] = {
            { IR_ADD, "__addsf3" }, { IR_SUB, "__subsf3" },
            { IR_MUL, "__mulsf3" }, { IR_DIV, "__divsf3" }
        };
        int a_reg = ret_reg(4), b_reg = ret_reg(4) - 4;   /* r22 and r18 */

        switch (i->op) {
        case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: {
            const char *nm = NULL;
            for (unsigned k = 0; k < sizeof fops / sizeof fops[0]; k++)
                if (fops[k].op == i->op) nm = fops[k].name;
            /* b FIRST: a goes into r22-r25, which may be b's home, and
             * r18-r21 are nobody's. */
            if (i->imm_b)
                ldi4(F, b_reg, (unsigned long)i->imm, 4);
            else
                vld(F, b_reg, i->b, 0, 4);
            vld(F, a_reg, i->a, 0, 4);
            call_helper(F, nm);
            vst(F, i->dst, 0, ret_reg(4), 4);
            return;
        }
        case IR_MOD:
            a_refuse(fn, i, "the remainder of two floats (C has no such "
                            "operator; fmod is a library function)");
            return;
        case IR_NEG:
            /* The sign bit, not a call: negation is exact and this is what
             * __negsf2 does anyway. Byte 3 holds the sign. */
            rd4(F, i->a, RA);
            /* Adding 0x80 to the top byte flips bit 7 and nothing else
             * (mod 256), and needs no second register to hold the mask. */
            avr_ri(t, AVR_SUBI, RA + 3, 0x80);
            wr4(F, i->dst, RA);
            return;
        case IR_CMP: {
            /* The helper returns an `int` -- TWO bytes here -- whose sign
             * answers the question, and any comparison with a NaN answers
             * "not equal, not less, not greater". So the 0/1 this IR_CMP
             * yields comes from comparing that against zero. */
            static const struct { enum binop p; const char *nm; } fc[] = {
                { B_EQ, "__eqsf2" }, { B_NE, "__nesf2" },
                { B_LT, "__ltsf2" }, { B_LE, "__lesf2" },
                { B_GT, "__gtsf2" }, { B_GE, "__gesf2" }
            };
            const char *nm = "__gesf2";
            enum avr_cond c;
            int swap = 0;
            for (unsigned k = 0; k < sizeof fc / sizeof fc[0]; k++)
                if (fc[k].p == i->pred) nm = fc[k].nm;
            if (i->imm_b)                       /* b first, as above */
                ldi4(F, b_reg, (unsigned long)i->imm, 4);
            else
                vld(F, b_reg, i->b, 0, 4);
            vld(F, a_reg, i->a, 0, 4);
            call_helper(F, nm);
            /* The result is in r25:r24. Compare it against zero -- signed,
             * because its SIGN is the answer. For `> 0` and `<= 0` the
             * comparison is the other way round, since AVR has no "greater
             * than" and zero is the fixed operand. */
            switch (i->pred) {
            case B_EQ: c = AVR_BR_EQ; break;
            case B_NE: c = AVR_BR_NE; break;
            case B_LT: c = AVR_BR_LT; break;
            case B_GE: c = AVR_BR_GE; break;
            case B_GT: c = AVR_BR_LT; swap = 1; break;
            default:   c = AVR_BR_GE; swap = 1; break;   /* B_LE */
            }
            if (swap) {
                avr_rr(t, AVR_CP,  R_ZERO, ret_reg(2));
                avr_rr(t, AVR_CPC, R_ZERO, ret_reg(2) + 1);
            } else {
                avr_rr(t, AVR_CP,  ret_reg(2), R_ZERO);
                avr_rr(t, AVR_CPC, ret_reg(2) + 1, R_ZERO);
            }
            avr_ri(t, AVR_LDI, RA, 1);
            avr_br(t, c, 1);
            avr_rr(t, AVR_MOV, RA, R_ZERO);
            for (int k = 1; k < dw(F, i); k++) avr_rr(t, AVR_MOV, RA + k, R_ZERO);
            wr4(F, i->dst, RA);
            return;
        }
        default:
            /* Everything else that carries `flt` just MOVES four bytes -- a
             * return, a call, a load, a store, a copy, a branch on zero --
             * and the ordinary paths below already do that correctly, because
             * a binary32 is exactly the width they work in. Only arithmetic
             * needs a helper. */
            break;
        }
    }

    /* A conversion is a call too, and its `flt` is on the SOURCE or the
     * destination rather than on the operation -- so these are outside the
     * block above. */
    if (i->op == IR_I2F || i->op == IR_F2I || i->op == IR_F2F) {
        int a_reg = ret_reg(4);
        if (i->op == IR_F2F) {
            /* binary32 to binary32: a move. `float` and `double` are the same
             * type on this target, so there is nothing to convert -- and a
             * call to __truncdfsf2 would be a call to the identity. */
            rd4(F, i->a, RA);
            wr4(F, i->dst, RA);
            return;
        }
        if (i->op == IR_I2F) {
            int sz = i->size < 1 ? 4 : i->size;
            const char *nm = sz > 4 ? (i->sign ? "__floatdisf"
                                              : "__floatundisf")
                                    : (i->sign ? "__floatsisf"
                                              : "__floatunsisf");
            int reg = sz > 4 ? ret_reg(8) : ret_reg(4);
            vld(F, reg, i->a, 0, sz > 4 ? 8 : 4);
            call_helper(F, nm);
            vst(F, i->dst, 0, ret_reg(4), 4);
            return;
        }
        {
            int sz = i->w < 1 ? 4 : i->w;
            const char *nm = sz > 4 ? (i->sign ? "__fixsfdi" : "__fixunssfdi")
                                    : (i->sign ? "__fixsfsi" : "__fixunssfsi");
            vld(F, a_reg, i->a, 0, 4);
            call_helper(F, nm);
            vst(F, i->dst, 0, ret_reg(sz > 4 ? 8 : 4), sz > 4 ? 8 : 4);
            return;
        }
    }

    /* ---- eight bytes: byte at a time, in a home or through memory ----- */
    if (wide_ins(F, i)) {
        int n = 8;
        switch (i->op) {
        case IR_CONST:
            if (is_remat(F, i->dst))
                return;                   /* rebuilt where it is read */
            if (in_pair(F, i->dst)) {
                ldi4(F, F->loc[i->dst], (unsigned long)i->imm, n);
                return;
            }
            for (int k = 0; k < n; k++) {
                ldi4(F, RA, (unsigned long)((unsigned long)i->imm >> (8 * k)), 1);
                vst_cc(F, i->dst, k, RA, 1);
            }
            return;
        case IR_MOV:
            wide_copy(F, i->dst, i->a, n);
            return;
        case IR_ADD: wide_bin(F, i, n, AVR_ADD, AVR_ADC); return;
        case IR_SUB: wide_bin(F, i, n, AVR_SUB, AVR_SBC); return;
        case IR_AND: wide_bin(F, i, n, AVR_AND, AVR_AND); return;
        case IR_OR:  wide_bin(F, i, n, AVR_OR,  AVR_OR);  return;
        case IR_XOR: wide_bin(F, i, n, AVR_EOR, AVR_EOR); return;
        case IR_BSWAP: {
            /* In pairs, both bytes read before either is written: the
             * result may have been given the operand's own home. Eight
             * byte moves where 64-bit shifts made two kilobytes. */
            int sc = w_scr(F, i);
            for (int k = 0; k < n / 2; k++) {
                vld(F, sc, i->a, k, 1);
                vld(F, sc + 1, i->a, n - 1 - k, 1);
                vst(F, i->dst, k, sc + 1, 1);
                vst(F, i->dst, n - 1 - k, sc, 1);
            }
            return;
        }
        case IR_BNOT:
            if (in_pair(F, i->dst)) {
                int d = F->loc[i->dst];
                vld(F, d, i->a, 0, n);
                for (int k = 0; k < n; k++) avr_r1(t, AVR_COM, d + k);
                return;
            }
            {
                int sc = w_scr(F, i);
                for (int k = 0; k < n; k++) {
                    vld(F, sc, i->a, k, 1);
                    avr_r1(t, AVR_COM, sc);
                    vst(F, i->dst, k, sc, 1);
                }
            }
            return;
        case IR_NEG:
            /* 0 - a, the same answer as at four bytes: the zero is an
             * immediate per byte, so this is one chain and not two. */
            {
                int sc = w_scr(F, i);
                if (in_pair(F, i->dst)) {
                    int d = F->loc[i->dst];
                    vld(F, d, i->a, 0, n);
                    for (int k = 0; k < n; k++) {
                        avr_rr(t, AVR_MOV, sc, R_ZERO);
                        avr_rr(t, k ? AVR_SBC : AVR_SUB, sc, d + k);
                        avr_rr(t, AVR_MOV, d + k, sc);
                    }
                    return;
                }
                for (int k = 0; k < n; k++) {
                    avr_rr(t, AVR_MOV, sc, R_ZERO);
                    avr_rr(t, k ? AVR_SBC : AVR_SUB, sc,
                           w_byte(F, i->a, 0, k, sc + 1));
                    vst_cc(F, i->dst, k, sc, 1);
                }
            }
            return;
        case IR_EXT: {
            /* Widen the low `size` bytes into eight. */
            int from = i->size < 1 ? 1 : i->size;
            if (from > n) from = n;
            if (in_pair(F, i->dst)) {
                int d = F->loc[i->dst], fill;
                vld(F, d, i->a, 0, from);
                fill = w_fill(F, d + from - 1, i->sign);
                fill_run(t, d + from, n - from, fill);
                return;
            }
            for (int k = 0; k < from; k++) {
                vld_cc(F, RA, i->a, k, 1);
                vst_cc(F, i->dst, k, RA, 1);
            }
            if (from < n) {
                /* The fill byte: zero, or the sign of the top valid byte. */
                if (i->sign) {
                    vld_cc(F, RA, i->a, from - 1, 1);
                    avr_rr(t, AVR_ADD, RA, RA);      /* lsl: MSB -> carry */
                    avr_rr(t, AVR_SBC, RA, RA);      /* 0 - carry */
                } else {
                    avr_rr(t, AVR_MOV, RA, R_ZERO);
                }
                for (int k = from; k < n; k++)
                    vst_cc(F, i->dst, k, RA, 1);
            }
            return;
        }
        case IR_LDVAR: {
            int from = i->size < 1 ? 1 : i->size;
            if (from > n) from = n;
            if (in_pair(F, i->dst)) {
                int d = F->loc[i->dst], fill;
                vld(F, d, i->a, 0, from);
                fill = w_fill(F, d + from - 1, i->sign);
                fill_run(t, d + from, n - from, fill);
                return;
            }
            for (int k = 0; k < from; k++) {
                vld_cc(F, RA, i->a, k, 1);
                vst_cc(F, i->dst, k, RA, 1);
            }
            if (from < n) {
                if (i->sign) {
                    vld_cc(F, RA, i->a, from - 1, 1);
                    avr_rr(t, AVR_ADD, RA, RA);
                    avr_rr(t, AVR_SBC, RA, RA);
                } else {
                    avr_rr(t, AVR_MOV, RA, R_ZERO);
                }
                for (int k = from; k < n; k++)
                    vst_cc(F, i->dst, k, RA, 1);
            }
            return;
        }
        case IR_STVAR: {
            int to = i->size < 1 ? 1 : i->size;
            if (to > n) to = n;
            if (in_pair(F, i->dst) || in_pair(F, i->a)) {
                wide_copy(F, i->dst, i->a, to);
                return;
            }
            for (int k = 0; k < to; k++) {
                vld_cc(F, RA, i->a, k, 1);
                vst_cc(F, i->dst, k, RA, 1);
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
            if (i->flash) {
                /* __flash, eight bytes: LPM through Z, walking -- straight
                 * into the home when there is one, as the RAM path does:
                 * A's first byte is the home's first byte too, so a byte
                 * staged in A was overwritten by the next. */
                vld(F, AVR_Z, i->a, 0, 2);
                z_add(t, i->memoff);
                if (in_pair(F, i->dst)) {
                    int d = F->loc[i->dst], fill;
                    for (int k = 0; k < from; k++)
                        avr_lpm(t, d + k, 1);
                    F->zv = -1;
                    fill = w_fill(F, d + from - 1, i->sign);
                    fill_run(t, d + from, n - from, fill);
                    return;
                }
                for (int k = 0; k < from; k++) {
                    avr_lpm(t, RA, 1);
                    vst_cc(F, i->dst, k, RA, 1);
                }
                F->zv = -1;
                if (from < n) {
                    if (i->sign) {
                        vld(F, RA, i->dst, from - 1, 1);
                        avr_rr(t, AVR_ADD, RA, RA);
                        avr_rr(t, AVR_SBC, RA, RA);
                    } else {
                        avr_rr(t, AVR_MOV, RA, R_ZERO);
                    }
                    for (int k = from; k < n; k++)
                        vst_cc(F, i->dst, k, RA, 1);
                }
                return;
            }
            if (in_pair(F, i->dst)) {
                /* Into the home, through Z: nothing between reaches a
                 * slot, so nothing walks with Z. */
                int d = F->loc[i->dst], fill;
                vld(F, AVR_Z, i->a, 0, 2);
                for (int k = 0; k < from; k++)
                    avr_ldd(t, d + k, AVR_Z, k);
                fill = w_fill(F, d + from - 1, i->sign);
                fill_run(t, d + from, n - from, fill);
                return;
            }
            vld(F, RA + 2, i->a, 0, 2);   /* A's upper pair: B may be a home */
            g_x_hit = 1; avr_movw(t, AVR_X, RA + 2);
            for (int k = 0; k < from; k++) {
                avr_ld(t, RA, AVR_X, AVR_PTR_POST_INC);
                vst_cc(F, i->dst, k, RA, 1);
            }
            if (from < n) {
                if (i->sign) {
                    /* X has walked past the value, so the sign byte is read
                     * back from the slot just written. */
                    vld(F, RA, i->dst, from - 1, 1);
                    avr_rr(t, AVR_ADD, RA, RA);
                    avr_rr(t, AVR_SBC, RA, RA);
                } else {
                    avr_rr(t, AVR_MOV, RA, R_ZERO);
                }
                for (int k = from; k < n; k++)
                    vst_cc(F, i->dst, k, RA, 1);
            }
            return;
        }
        case IR_STORE: {
            int to = i->size < 1 ? 1 : i->size;
            if (to > n) to = n;
            if (in_pair(F, i->b)) {
                int h = F->loc[i->b];
                vld(F, AVR_Z, i->a, 0, 2);
                for (int k = to - 1; k >= 0; k--)   /* high first: IR_STORE */
                    avr_std(t, AVR_Z, k, h + k);
                return;
            }
            vld(F, RA + 2, i->a, 0, 2);
            g_x_hit = 1; avr_movw(t, AVR_X, RA + 2);
            /* High byte first (IR_STORE says why): X past the end, then
             * pre-decrement down. */
            avr_adiw(t, AVR_X, to);
            for (int k = to - 1; k >= 0; k--) {
                vld(F, RA, i->b, k, 1);
                avr_st(t, AVR_X, RA, AVR_PTR_PRE_DEC);
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
            int var = !const_b(F, i, &k);
            long sd;
            if (in_pair(F, i->dst)) {
                /* In the home: the four-byte shifts at eight bytes, whole
                 * bytes moved and the rest shifted, or the count-down loop
                 * with its count in r0 -- which the far slot path saves
                 * SREG through, and nothing here reaches a slot. */
                int d = F->loc[i->dst];
                if (var)
                    vld(F, R_TMP, i->b, 0, 1);
                vld(F, d, i->a, 0, n);
                if (var)
                    shift_varw(F, d, R_TMP, (int)i->op, i->sign, n);
                else
                    shift_immw(F, d, (int)i->op, i->sign, k, n);
                return;
            }
            wide_copy(F, i->dst, i->a, n);
            sd = sslot(F, i->dst);
            if (!var && k >= 8 * n)
                k = i->op == IR_SHR && i->sign ? 8 * n - 1 : 8 * n;
            if (var) {
                int top, exitj;
                /* The counter stays in r19, NOT in r0. r0 is where a far slot
                 * access saves SREG, and the chain below is full of them --
                 * so a counter there is destroyed on the first iteration
                 * whose frame needs the far path. It shifted by seven bytes
                 * whatever the count said. r19 is untouched by the chain,
                 * which only ever uses r18; and it is not B, which may be a
                 * home. */
                vld(F, RA + 1, i->b, 0, 1);
                top = t->len;
                avr_r1(t, AVR_DEC, RA + 1);
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
            /* Whole bytes first: a count of 8*B + r is B byte moves and
             * then r passes of the bit chain, where it was 8*B + r passes
             * -- forty of them, two kilobytes, for an -O0 `x >> 40`. A
             * left shift moves bytes up from the top down, a right shift
             * down from the bottom up, so each byte is read before it is
             * overwritten; an arithmetic one fills with the sign of the
             * top byte, taken (in r19, which the chain never touches)
             * before that byte moves. */
            if (k >= 8) {
                long bytes = k / 8;
                k %= 8;
                if (i->op == IR_SHL) {
                    for (long b = n - 1; b >= 0; b--) {
                        if (b >= bytes) {
                            ld_slot_cc(F, RA, sd + b - bytes, 1);
                            st_slot_cc(F, sd + b, RA, 1);
                        } else {
                            st_slot_cc(F, sd + b, R_ZERO, 1);
                        }
                    }
                } else {
                    int fill = R_ZERO;
                    if (i->sign) {
                        ld_slot_cc(F, RA + 1, sd + n - 1, 1);
                        avr_rr(t, AVR_ADD, RA + 1, RA + 1);
                        avr_rr(t, AVR_SBC, RA + 1, RA + 1);
                        fill = RA + 1;
                    }
                    for (long b = 0; b < n; b++) {
                        if (b + bytes < n) {
                            ld_slot_cc(F, RA, sd + b + bytes, 1);
                            st_slot_cc(F, sd + b, RA, 1);
                        } else {
                            st_slot_cc(F, sd + b, fill, 1);
                        }
                    }
                }
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
            /* The operands with homes first, as one parallel move -- one
             * may live where the other goes, r18-r25 and r10-r17 both
             * being homes -- then the ones in memory and the constant. */
            {
                int md[16], ms[16], od[24], os[24], nm2 = 0, no;
                for (int k = 0; in_pair(F, i->a) && k < F->hw[i->a]; k++) {
                    md[nm2] = a_reg + k; ms[nm2] = F->loc[i->a] + k; nm2++;
                }
                for (int k = 0; !i->imm_b && in_pair(F, i->b) &&
                                k < F->hw[i->b]; k++) {
                    md[nm2] = b_reg + k; ms[nm2] = F->loc[i->b] + k; nm2++;
                }
                no = ra_parallel_move(md, ms, nm2, R_TMP, od, os, 24);
                if (no < 0)
                    internal_error("avr: %s: a 64-bit helper's operands do "
                                   "not form a parallel move", fn->name);
                emit_moves(t, od, os, no);
            }
            if (!in_pair(F, i->a))
                vld(F, a_reg, i->a, 0, 8);
            if (i->imm_b)
                for (int k = 0; k < 8; k++)
                    ldi4(F, b_reg + k,
                         (unsigned long)((unsigned long)i->imm >> (8 * k)), 1);
            else if (!in_pair(F, i->b))
                vld(F, b_reg, i->b, 0, 8);
            call_helper(F, nm);
            /* The result comes back in the first argument's registers. */
            vst(F, i->dst, 0, ret_reg(8), 8);
            return;
        }
        case IR_CMP: {
            /* cp then seven cpc, through memory: the carry and zero flags
             * survive the loads, so the whole 64-bit subtraction's result is
             * in SREG by the last byte. */
            int swap, sc = w_scr(F, i);
            enum avr_cond c = cond_for(i->pred, i->sign, &swap);
            int vb = i->imm_b ? -1 : i->b;
            int vl = swap ? vb : i->a, vr = swap ? i->a : vb;
            for (int k = 0; k < n; k++) {
                int rl = w_byte(F, vl, i->imm, k, sc);
                int rr = w_byte(F, vr, i->imm, k, sc + 1);
                avr_rr(t, k ? AVR_CPC : AVR_CP, rl, rr);
            }
            /* The result is four bytes wide even though the comparison was
             * eight -- an IR_CMP yields a 0 or a 1. */
            avr_ri(t, AVR_LDI, RA, 1);
            avr_br(t, c, 1);
            avr_rr(t, AVR_MOV, RA, R_ZERO);
            for (int k = 1; k < dw(F, i); k++) avr_rr(t, AVR_MOV, RA + k, R_ZERO);
            wr4(F, i->dst, RA);
            return;
        }
        case IR_SELECT:
            gen_select(F, i, n);
            return;

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
                int r = RA;
                if (in_pair(F, i->a))
                    r = F->loc[i->a] + k;      /* read in its home */
                else
                    vld(F, RA, i->a, k, 1);
                avr_rr(t, AVR_OR, R_TMP, r);
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
        case IR_FADDR: case IR_LABELADDR: case IR_IGOTO:
            break;

        default:
            /* Anything else at eight bytes has no path yet, and says so. */
            a_refuse(fn, i, "this operation at 64 bits");
        }
    }

    switch (i->op) {
    case IR_CONST: {
        /* Only the bytes something reads -- see avr_demand. A result nothing
         * reads is not computed at all, which for these operations is safe:
         * none of them has an effect beyond its result. */
        int nb = dw(F, i);
        if (is_remat(F, i->dst))
            return;                   /* rebuilt where it is read */
        if (nb && in_pair(F, i->dst) && nb <= F->hw[i->dst]) {
            /* Straight into the home: `ldi` reaches it from r16 up, and
             * below that a zero is `mov r, r1` from the zero register.
             * Anything else below r16 goes through A as before. */
            int d = F->loc[i->dst];
            unsigned long m = nb >= 4 ? 0xffffffffUL : (1UL << (8 * nb)) - 1;
            if (d >= 16 || ((unsigned long)i->imm & m) == 0) {
                ldi4(F, d, (unsigned long)i->imm, nb);
                return;
            }
        }
        if (nb) {
            ldi4(F, RA, (unsigned long)i->imm, nb);
            wr4(F, i->dst, RA);
        }
        return;
    }

    case IR_MOV: {
        /* Straight into or out of a home when there is one, rather than
         * through A both ways: a copy between two values that share a
         * home is then nothing at all, which is what most of them are --
         * the colourer puts a copy's two ends together when it can. */
        int n = dw(F, i);
        if (n && in_pair(F, i->dst)) {
            vld(F, F->loc[i->dst], i->a, 0, n);
            return;
        }
        if (n && in_pair(F, i->a) && F->slot[i->dst] >= 0) {
            vst(F, i->dst, 0, F->loc[i->a], n);
            return;
        }
        rd4(F, i->a, RA);
        wr4(F, i->dst, RA);
        return;
    }

    case IR_ADD: case IR_SUB: case IR_AND: case IR_OR: case IR_XOR: {
        int nb = dw(F, i);
        int d, a = i->a, b = i->b;
        if (!nb)
            return;
        /* Computed IN the destination's home when it has one: `d = a op
         * b` is then a copy of a into d and the operation, where it was a
         * copy of a into A, the operation, and a copy of A into d. The
         * immediate forms reach r16 and up only, so a constant goes
         * through A unless the home is there. If d is where b lives,
         * copying a in first would lose b: a commutative operation takes
         * them the other way round, a subtraction takes A. */
        d = dst_reg(F, i, nb);
        if (i->imm_b && d < 16 && (i->op == IR_ADD || i->op == IR_SUB)) {
            /* A home below r16, which subi cannot reach -- but a constant
             * of one significant byte is `ldi` into A and an add or a
             * subtract of that with the zero register carrying: three
             * instructions in place of a copy into A, subi/sbci and a copy
             * back. `p + 1` on a pointer in r2 is the common case. */
            unsigned long m = nb >= 4 ? 0xffffffffUL : (1UL << (8 * nb)) - 1;
            unsigned long v = (unsigned long)i->imm & m;
            int sub = i->op == IR_SUB;
            if (v > 0xff && ((0UL - v) & m) <= 0xff) {
                v = (0UL - v) & m;      /* a small negative: the other op */
                sub = !sub;
            }
            if (v <= 0xff) {
                vld(F, d, a, 0, nb);
                if (v) {
                    avr_ri(t, AVR_LDI, RA, (int)v);
                    avr_rr(t, sub ? AVR_SUB : AVR_ADD, d, RA);
                    for (int k = 1; k < nb; k++)
                        avr_rr(t, sub ? AVR_SBC : AVR_ADC, d + k, R_ZERO);
                }
                return;
            }
        }
        if (i->imm_b && d < 16)
            d = RA;
        if (!i->imm_b && d != RA && in_pair(F, b) && F->loc[b] == d &&
            !(in_pair(F, a) && F->loc[a] == d)) {
            if (i->op == IR_SUB) {
                d = RA;
            } else {
                int x = a; a = b; b = x;
            }
        }
        if (i->imm_b) {
            vld(F, d, a, 0, nb);
            /* A constant folds into the instruction, since A is in
             * r16-r31 where the immediate forms reach: an add is a subi
             * of the negation with sbci carrying the borrow -- exactly
             * the sum, mod 2^(8nb) -- a subtract is subi/sbci of the
             * constant itself, and and/or/xor go a byte at a time,
             * skipping each byte the constant leaves alone. `x + 1` is
             * two instructions instead of four. */
            unsigned long v = (unsigned long)i->imm;
            if ((d == 24 || d == AVR_X) && nb == 2 &&
                (i->op == IR_ADD || i->op == IR_SUB)) {
                /* adiw/sbiw: one word where subi/sbci are two */
                long k = (long)(short)(v & 0xffffu);
                if (i->op == IR_SUB) k = -k;
                if (k >= 1 && k <= 63) { avr_adiw(t, d, (int)k); return; }
                if (k <= -1 && k >= -63) { avr_sbiw(t, d, (int)-k); return; }
            }
            if (i->op == IR_ADD) v = 0UL - v;
            for (int k = 0; k < nb; k++) {
                int b = (int)((v >> (8 * k)) & 0xffu);
                switch (i->op) {
                case IR_ADD: case IR_SUB:
                    avr_ri(t, k ? AVR_SBCI : AVR_SUBI, d + k, b);
                    break;
                case IR_AND:
                    if (b == 0) avr_rr(t, AVR_MOV, d + k, R_ZERO);
                    else if (b != 0xff) avr_ri(t, AVR_ANDI, d + k, b);
                    break;
                case IR_OR:
                    if (b) avr_ri(t, AVR_ORI, d + k, b);
                    break;
                default:
                    if (b == 0xff) avr_r1(t, AVR_COM, d + k);
                    else if (b) {
                        /* the mask in whichever bank d is not: d may be
                         * a home in B */
                        int sc = d == RA ? RB : RA;
                        avr_ri(t, AVR_LDI, sc + k, b);
                        avr_rr(t, AVR_EOR, d + k, sc + k);
                    }
                    break;
                }
            }
            dst_done(F, i, d);
            return;
        }
        {
        /* b where it lives, or loaded into the bank d is not: d may be
         * a home in B, which copying a into would then overwrite */
        int rb = rd_in(F, b, nb, d == RA ? RB : RA);
        vld(F, d, a, 0, nb);
        switch (i->op) {
        case IR_ADD:
            avr_rr(t, AVR_ADD, d, rb);
            for (int k = 1; k < nb; k++) avr_rr(t, AVR_ADC, d + k, rb + k);
            break;
        case IR_SUB:
            avr_rr(t, AVR_SUB, d, rb);
            for (int k = 1; k < nb; k++) avr_rr(t, AVR_SBC, d + k, rb + k);
            break;
        default: {
            enum avr_rr op = i->op == IR_AND ? AVR_AND
                           : i->op == IR_OR  ? AVR_OR : AVR_EOR;
            for (int k = 0; k < nb; k++) avr_rr(t, op, d + k, rb + k);
            break;
        }
        }
        }
        dst_done(F, i, d);
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
        long k;
        int hi, neg, nb = dw(F, i), d, ra;

        if (!const_b(F, i, &k))
            goto helper;               /* __mulsi3 */
        if (!nb)
            return;
        neg = k < 0;
        if (neg) k = -k;
        for (hi = 8 * VW - 1; hi > 0 && !((k >> hi) & 1); hi--)
            ;
        if (k && hi > 16)
            goto helper;               /* wider than the shifts are worth */
        /* At the width anything reads -- a product's low bytes depend on
         * the operands' low bytes alone -- and in the destination's home
         * when it has one. The accumulator STARTS as a, which is the top
         * bit's add done for nothing, then shifts once per bit below it
         * and adds a again for each one set: `i * 12` on an int is a copy
         * and four pairs, where it was a four-byte accumulator in X and Z
         * zeroed, shifted and added from bit 16 down, and copied out. */
        d = dst_reg(F, i, nb);
        if (k == 0) {
            ldi4(F, d, 0, nb);
            dst_done(F, i, d);
            return;
        }
        /* a, to add at each set bit: in place where it lives, unless that
         * is d; otherwise in the bank d is not -- d may be a home in B. */
        ra = -1;
        if (k != (1L << hi)) {
            if (in_pair(F, i->a) && nb <= F->hw[i->a] && F->loc[i->a] != d)
                ra = F->loc[i->a];
            else {
                ra = d == RA ? RB : RA;
                vld(F, ra, i->a, 0, nb);
            }
        }
        if (in_pair(F, i->a) && F->loc[i->a] == d) {
            ;                          /* d holds a already */
        } else if (ra >= 0 && !(in_pair(F, i->a) && F->loc[i->a] == ra)) {
            /* d from the copy just made, not from memory again */
            for (int q = 0; q < nb; q++) {
                if (q + 1 < nb && !((d + q) & 1) && !((ra + q) & 1)) {
                    avr_movw(t, d + q, ra + q);
                    q++;
                } else {
                    avr_rr(t, AVR_MOV, d + q, ra + q);
                }
            }
        } else {
            vld(F, d, i->a, 0, nb);
        }
        for (int bit = hi - 1; bit >= 0; bit--) {
            if (ra < 0) {
                shift_immw(F, d, IR_SHL, 0, hi, nb);   /* a power of two */
                break;
            }
            shift1w(F, d, IR_SHL, 0, nb);
            if ((k >> bit) & 1) {
                avr_rr(t, AVR_ADD, d, ra);
                for (int q = 1; q < nb; q++)
                    avr_rr(t, AVR_ADC, d + q, ra + q);
            }
        }
        if (neg && d >= 16) {
            /* -d = ~d + 1: com the upper bytes, neg the low one -- which
             * leaves carry set unless it was zero -- and sbci 0xff carries
             * the +1 up. avr-gcc's sequence, a byte shorter than 0 - d. */
            for (int q = nb - 1; q >= 1; q--)
                avr_r1(t, AVR_COM, d + q);
            avr_r1(t, AVR_NEG, d);
            for (int q = 1; q < nb; q++)
                avr_ri(t, AVR_SBCI, d + q, 0xff);
        } else if (neg) {
            /* 0 - d, in place, through a byte of the bank d is not */
            int sc = d == RA ? RB : RA;
            for (int q = 0; q < nb; q++) {
                avr_rr(t, AVR_MOV, sc, R_ZERO);
                avr_rr(t, q ? AVR_SBC : AVR_SUB, sc, d + q);
                avr_rr(t, AVR_MOV, d + q, sc);
            }
        }
        dst_done(F, i, d);
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

        /* b FIRST: a's registers r22-r25 may be b's home. */
        if (i->imm_b)
            ldi4(F, b_reg, (unsigned long)i->imm, VW);
        else
            vld(F, b_reg, i->b, 0, VW);
        vld(F, a_reg, i->a, 0, VW);
        call_helper(F, name);
        copy_run(t, RA, a_reg, VW);
        wr4(F, i->dst, RA);
        return;
    }

    case IR_NEG:
        /* 0 - a, which is what clang emits: the com/neg/sbci chain is
         * shorter to write down and longer to get right, and this reuses
         * the subtract that is already here. */
        {
            int nb = dw(F, i);
            if (!nb)
                return;
            int ra = rd_in(F, i->a, nb, RB);
            ldi4(F, RA, 0, nb);
            avr_rr(t, AVR_SUB, RA, ra);
            for (int k = 1; k < nb; k++) avr_rr(t, AVR_SBC, RA + k, ra + k);
            wr4(F, i->dst, RA);
        }
        return;

    case IR_BNOT: {
        int nb = dw(F, i);
        if (!nb)
            return;
        rd4(F, i->a, RA);
        for (int k = 0; k < nb; k++) avr_r1(t, AVR_COM, RA + k);
        wr4(F, i->dst, RA);
        return;
    }

    case IR_BSWAP: {
        /* A byte swap is a permutation of the registers the value is in:
         * byte k of the result is byte size-1-k of the operand, and a
         * halfword's upper bytes are zero. It was 32-bit shifts and
         * masks -- 142 bytes for a bswap32 where clang's is 32. */
        int sz = i->size;
        rd4(F, i->a, RA);
        for (int k = 0; k < sz / 2; k++) {
            avr_rr(t, AVR_MOV, R_TMP, RA + k);
            avr_rr(t, AVR_MOV, RA + k, RA + sz - 1 - k);
            avr_rr(t, AVR_MOV, RA + sz - 1 - k, R_TMP);
        }
        for (int k = sz; k < 4; k++)
            avr_rr(t, AVR_MOV, RA + k, R_ZERO);
        wr4(F, i->dst, RA);
        return;
    }

    case IR_SHL: case IR_SHR: {
        /* At the width that carries the answer: a left shift's nb low
         * bytes need only the operand's nb low bytes, and a right shift of
         * an extension is that extension of the shift when the kinds agree
         * -- logical of a zero-extended value, arithmetic of a sign-extended
         * one. An `int` here is two bytes and the IR shifts four. */
        long k;
        int nb = dw(F, i), w = VW, xk = -1;
        int a = i->a;
        if (!nb)
            return;
        if (i->op == IR_SHL) {
            w = nb;
        } else if (F->xw && a >= 0 && a < F->fn->nvregs && F->xw[a] &&
                   F->xw[a] < VW) {
            if (F->xs[a] == 0)           { w = F->xw[a]; xk = 0; }
            else if (i->sign)            { w = F->xw[a]; xk = 1; }
        }
        {
            int d = dst_reg(F, i, nb > w ? nb : w);   /* all w shifted */
            int cnt;
            if (d != RA && !const_b(F, i, &k) && in_pair(F, i->b) &&
                F->loc[i->b] == d)
                d = RA;                     /* the count lives there */
            /* the count's low byte, in the bank d is not: d may be a
             * home in B */
            int cr = d == RA ? RB : RA;
            if (!const_b(F, i, &k))
                vld(F, cr, i->b, 0, 1);
            /* A zero-extended operand shifts LOGICALLY at its own width
             * whatever the IR says: at four bytes its top bit is 0 and an
             * arithmetic shift is a logical one, but at one byte `asr`
             * would drag bit 7 back in -- `(uint8_t)b >> 4` gave 0xfe for
             * b = 0xea. */
            int sg = xk == 0 ? 0 : i->sign;
            vld(F, d, a, 0, w);
            if (const_b(F, i, &k)) {
                shift_immw(F, d, (int)i->op, sg, k, w);
            } else {
                cnt = cr;
                shift_varw(F, d, cnt, (int)i->op, sg, w);
            }
            if (w < nb)
                extend(F, d, w, xk == 1, nb);
            dst_done(F, i, d);
        }
        return;
    }

    case IR_CMP: {
        int swap, sg, nb = cmp_width(F, i, &sg);
        enum avr_cond c = cond_for(i->pred, sg, &swap);
        const struct ir_ins *nx = n + 1 < fn->nins ? &fn->ins[n + 1] : NULL;
        {
            /* Both operands where they live; a comparison with zero is
             * against r1, the zero register, byte for byte. */
            int ra = rd_in(F, i->a, nb, RA), rb[4], cpi = -1;
            /* b's scratch is A when a did not need it, so that B is
             * written only when both operands had to be loaded */
            int sc = ra == RA ? RB : RA;
            if (i->imm_b) {
                /* A constant byte by byte: a zero is r1, the zero register,
                 * and the low byte of `a < K` is `cpi` when a is from r16
                 * up; only the rest need loading. */
                for (int k = 0; k < nb; k++) {
                    int c = (int)(((unsigned long)i->imm >> (8 * k)) & 0xffu);
                    if (c == 0) {
                        rb[k] = R_ZERO;
                    } else if (k == 0 && !swap && ra >= 16) {
                        cpi = c;
                        rb[k] = -1;
                    } else {
                        avr_ri(t, AVR_LDI, sc + k, c);
                        rb[k] = sc + k;
                    }
                }
            } else {
                int r = rd_in(F, i->b, nb, sc);
                for (int k = 0; k < nb; k++) rb[k] = r + k;
            }
            for (int k = 0; k < nb; k++) {
                if (k == 0 && cpi >= 0)
                    avr_ri(t, AVR_CPI, ra, cpi);
                else if (swap)
                    avr_rr(t, k ? AVR_CPC : AVR_CP, rb[k], ra + k);
                else
                    avr_rr(t, k ? AVR_CPC : AVR_CP, ra + k, rb[k]);
            }
        }
        /* The branch that follows, when it is the 0/1's only reader: take
         * the flags as they stand, and the value never exists. */
        if (nx && (nx->op == IR_BRZ || nx->op == IR_BRNZ) &&
            nx->a == i->dst && F->usecnt && i->dst >= 0 &&
            F->usecnt[i->dst] == 1 && !(F->wide && F->wide[nx->a])) {
            jump_if(F, nx->op == IR_BRNZ ? c : (enum avr_cond)(c ^ 1),
                    nx->label);
            F->skip_next = 1;
            return;
        }
        /* ldi does not touch SREG, so the 1 may be loaded between the
         * compare and the branch; `mov` does not either. In the home when
         * `ldi` reaches it. */
        {
            int d = RA, nb2 = dw(F, i);
            if (in_pair(F, i->dst) && F->loc[i->dst] >= 16 &&
                nb2 <= F->hw[i->dst])
                d = F->loc[i->dst];
            avr_ri(t, AVR_LDI, d, 1);
            avr_br(t, c, 1);                    /* skip the clear */
            avr_rr(t, AVR_MOV, d, R_ZERO);
            /* the 0/1's upper bytes, as far as anything reads them */
            for (int k = 1; k < nb2; k++) avr_rr(t, AVR_MOV, d + k, R_ZERO);
            if (d == RA)
                wr4(F, i->dst, RA);
        }
        return;
    }

    case IR_SELECT:
        gen_select(F, i, VW);
        return;

    case IR_BRZ: case IR_BRNZ:
        /* r0 is the accumulator, so every access here must be one that does
         * NOT save SREG through it -- rd4 is, and the wide path above says
         * why that matters. */
        {
            /* only the bytes that can be nonzero (need_of_use) */
            int nb = (F->xw && F->xw[i->a]) ? F->xw[i->a] : VW;
            int r = RA;
            /* Tested where it is: a home in place, anything else once it
             * is loaded into A. One byte is `or r,r` (tst) -- a `mov`
             * sets no flag, and a branch after one alone read whatever
             * came before. More are compared with zero, low byte up: cpc
             * keeps Z only while every byte so far was zero. */
            if (in_pair(F, i->a) && nb <= F->hw[i->a])
                r = F->loc[i->a];
            else
                vld(F, RA, i->a, 0, nb);
            if (nb == 1) {
                avr_rr(t, AVR_OR, r, r);
            } else {
                avr_rr(t, AVR_CP, r, R_ZERO);
                for (int k = 1; k < nb; k++) avr_rr(t, AVR_CPC, r + k, R_ZERO);
            }
        }
        jump_if(F, i->op == IR_BRZ ? AVR_BR_EQ : AVR_BR_NE, i->label);
        return;

    case IR_LDVAR: {
        /* Load only what is read, and extend only past what was loaded.
         * A VOLATILE access is performed as written, whatever is used of
         * it: on this part a 16-bit timer or ADC register latches its high
         * byte when the low one is read, and the standard says the access
         * happens regardless. */
        int nb = dw(F, i);
        int ld = (i->vol || nb > i->size) ? i->size : nb;
        if (!ld)
            return;
        if (nb <= ld && in_pair(F, i->dst)) {  /* no extension: in place */
            vld(F, F->loc[i->dst], i->a, 0, ld);
            return;
        }
        vld(F, RA, i->a, 0, ld);
        if (nb > i->size)
            extend(F, RA, i->size, i->sign, nb);
        wr4(F, i->dst, RA);
        return;
    }

    case IR_STVAR:
        if (in_pair(F, i->a)) {                /* straight from its home */
            vst(F, i->dst, 0, F->loc[i->a],
                i->size < F->hw[i->a] ? i->size : F->hw[i->a]);
            return;
        }
        rd4(F, i->a, RA);
        vst(F, i->dst, 0, RA, i->size);
        return;

    case IR_LOAD:
        /* The pointer is two bytes: an AVR address IS two bytes, whatever
         * the IR's width class says about the vreg holding it. */
        {
            /* Narrowed as IR_LDVAR is, with the same exception for a
             * volatile access -- which here is the common case: this is how
             * a memory-mapped register is read. */
            int nb = dw(F, i);
            int ld = (i->vol || nb > i->size) ? i->size : nb;
            if (!ld)
                return;
            int d = dst_reg(F, i, nb);       /* straight into its home */
            vld(F, AVR_Z, i->a, 0, 2);
            if (i->flash) {
                /* __flash: program memory, which LPM alone reads, through
                 * Z, and with no displaced form -- so Z walks, and holds
                 * this pointer no longer. */
                z_add(F->t, i->memoff);
                for (int k = 0; k < ld; k++)
                    avr_lpm(t, d + k, 1);
                F->zv = -1;
                if (nb > i->size)
                    extend(F, d, i->size, i->sign, nb);
                dst_done(F, i, d);
                return;
            }
            /* Displaced off Z, low byte first -- the order a 16-bit I/O
             * register's latch needs for a read -- at the field offset
             * ra_fold_memoff folded in, and without moving Z, so the next
             * access through the same pointer finds it there. */
            for (int k = 0; k < ld; k++)
                avr_ldd(t, d + k, AVR_Z, i->memoff + k);
            if (nb > i->size)
                extend(F, d, i->size, i->sign, nb);
            dst_done(F, i, d);
        }
        return;

    case IR_STORE:
        /* The value FIRST: its far path may walk through Z, which the
         * address load is about to own. */
        {
        /* ...from its home when it has one, which touches no Z. */
        int src = rd_in(F, i->b, i->size, RA);
        vld(F, AVR_Z, i->a, 0, 2);
        /* The HIGH byte first. A 16-bit timer, compare or ADC register
         * is written through the part's TEMP latch: the high byte waits
         * there and the LOW byte's write commits both, so written low
         * first the register takes whatever TEMP last held as its top
         * half. `TCNT1 = 0x1234` read back 0 under QEMU, which models the
         * latch (tests/golden/avr-io16.sh). avr-gcc and clang both store
         * every multi-byte value high byte first; reads stay low first,
         * which is the order the same latch needs for them. */
        for (int k = i->size - 1; k >= 0; k--)
            avr_std(t, AVR_Z, i->memoff + k, src + k);
        }
        return;

    case IR_EXT: {
        /* When no more is read than the source had, there is nothing to
         * extend: the low bytes ARE the source's low bytes. */
        int nb = dw(F, i);
        if (!nb)
            return;
        if (nb <= i->size && in_pair(F, i->dst)) {   /* a copy, in place */
            vld(F, F->loc[i->dst], i->a, 0, nb);
            return;
        }
        if (nb <= i->size && in_pair(F, i->a) && F->slot[i->dst] >= 0) {
            vst(F, i->dst, 0, F->loc[i->a], nb);
            return;
        }
        {
        int d = dst_reg(F, i, nb);
        vld(F, d, i->a, 0, nb < i->size ? nb : i->size);
        if (nb > i->size)
            extend(F, d, i->size, i->sign, nb);
        dst_done(F, i, d);
        }
        return;
    }

    /* A variable-length array, alloca, or a local aligned beyond what
     * the stack promises (which is nothing here): sp -= size, and the
     * block is the bytes above the new sp -- AVR's sp points at the next
     * FREE byte, so the block starts at sp + 1. sp is moved with
     * interrupts masked, as the prologue does. The frame stays at Y, so
     * every slot keeps its address; a call's stack arguments, which a
     * callee finds just above its return address, are moved down to the
     * new sp at the call (IR_CALL). X is no home's register. */
    case IR_ALLOCA:
        vld(F, RA, i->a, 0, 2);
        avr_in(t, AVR_X, IO_SPL);
        avr_in(t, AVR_X + 1, IO_SPH);
        avr_rr(t, AVR_SUB, AVR_X, RA);
        avr_rr(t, AVR_SBC, AVR_X + 1, RA + 1);
        set_sp_from(t, AVR_X);
        avr_adiw(t, AVR_X, 1);
        avr_movw(t, RA, AVR_X);
        if (dw(F, i) > 2)
            extend(F, RA, 2, 0, dw(F, i));
        wr4(F, i->dst, RA);
        return;
    case IR_SPSAVE:
        avr_in(t, RA, IO_SPL);
        avr_in(t, RA + 1, IO_SPH);
        if (dw(F, i) > 2)
            extend(F, RA, 2, 0, dw(F, i));
        wr4(F, i->dst, RA);
        return;
    case IR_SPRESTORE:
        vld(F, AVR_X, i->a, 0, 2);
        set_sp_from(t, AVR_X);
        return;

    case IR_ADDR:
        y_to(F, RA);
        add_const16(F, RA, sslot(F, i->a));
        if (dw(F, i) > 2)
            extend(F, RA, 2, 0, dw(F, i));
        wr4(F, i->dst, RA);
        return;

    /* An address is two `ldi`s the linker fills in: into the home when
     * it is from r16 up, and extended past its two bytes only as far as
     * anything reads -- a pointer in a pair is read as two. */
    case IR_STRADDR: case IR_GADDR: case IR_FADDR: {
        int nb = dw(F, i), d = addr_reg(F, i);
        if (!nb)
            return;
        if (i->op == IR_STRADDR) {
            note_str(F->st, ldi_addr_pair(F, d), i->label, RK_AVR_LO8_LDI);
            note_str(F->st, F->t->len - 2, i->label, RK_AVR_HI8_LDI);
        } else if (i->op == IR_GADDR) {
            note_glob(F->st, ldi_addr_pair(F, d), i->glob, RK_AVR_LO8_LDI);
            note_glob(F->st, F->t->len - 2, i->glob, RK_AVR_HI8_LDI);
        } else {
            /* _GS, not the data forms: a function pointer on AVR holds the
             * WORD address, and the linker may route it through a stub. */
            note_fn(F->st, ldi_addr_pair(F, d), i->callee, RK_AVR_LO8_LDI_GS);
            note_fn(F->st, F->t->len - 2, i->callee, RK_AVR_HI8_LDI_GS);
        }
        if (nb > 2)
            extend(F, d, 2, 0, nb);
        if (d == RA)
            wr4(F, i->dst, RA);
        return;
    }

    /* GNU computed goto. &&label is a code address, so a WORD address
     * as a function pointer is: the function's own symbol through the _GS
     * forms, as IR_FADDR takes it, plus the label's byte offset as the
     * addend (the linker halves the sum), set once the function is laid
     * out -- a fix of `wide` 3 whose `at` is the first site's index.
     * goto *p puts that word address in Z and is ijmp. */
    case IR_LABELADDR: {
        if (cg_label_mark(i))       /* static data's marker: no code */
            return;
        int nb = dw(F, i), d = addr_reg(F, i), s0 = F->st->nf;
        if (!nb)
            return;
        note_fn(F->st, ldi_addr_pair(F, d), fn->src, RK_AVR_LO8_LDI_GS);
        note_fn(F->st, F->t->len - 2, fn->src, RK_AVR_HI8_LDI_GS);
        want_label_site(F, s0, i->label, 3, -1);
        if (nb > 2)
            extend(F, d, 2, 0, nb);
        if (d == RA)
            wr4(F, i->dst, RA);
        return;
    }
    case IR_IGOTO:
        vld(F, AVR_Z, i->a, 0, 2);
        avr_ijmp(F->t);
        return;

    case IR_LABEL:
        F->label_off[i->label] = t->len;
        F->last_st.v = -1;            /* a jump may arrive here */
        F->zv = -1;
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
            vld(F, RA, i->a, 0, 2);
            if (F->sret_slot >= 0) {
                ld_slot(F, RB, F->sret_slot, 2);
                avr_movw(t, AVR_Z, RA);
                g_x_hit = 1; avr_movw(t, AVR_X, RB);
                /* both walk: `ldd` reaches 63, and a struct of 64
                 * bytes or more stopped the build at the 65th */
                for (long b = 0; b < n; b++) {
                    avr_ld(t, R_TMP, AVR_Z, AVR_PTR_POST_INC);
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
            vld(F, ret_reg(size), i->a, 0, size);
        }
        jump_to(F, fn->nlabels);             /* the epilogue */
        return;

    case IR_CALL: {
        struct argplace pl, hid;
        int cursor = i->call_varargs ? -1 : ARG_TOP;
        long stk = 0;
        int k;
        unsigned long argregs = 0;     /* the registers the arguments use */
        /* A callee returning a composite of more than eight bytes takes the
         * buffer's address as an implicit FIRST argument, so the real ones
         * start one slot along -- r23:r22 for the first, not r25:r24.
         *
         * The callee's prologue always knew that. This side did not: it
         * placed the arguments from r25 down as though there were no hidden
         * pointer, then wrote the pointer into r25:r24 at the end, on top of
         * the first argument. `struct s20 m20(int k)` received whatever was
         * left in r23:r22 as `k`, and every AVR call to a function returning
         * a large struct with arguments silently lost its first one --
         * tests/golden/embedded-aggregate.c printed 40 for 190 and 5 for 205
         * the first time it ran on the part.
         *
         * For a VARIADIC callee every argument is on the stack, and so is the
         * pointer: it is the first stack word, below the named arguments.
         * That is what clang emits, and what place_arg gives from a cursor of
         * -1 without being told. */
        int sret = sret_bytes(i->retsize) != 0;

        /* Stack arguments FIRST, while the argument registers are still
         * free to carry them: each is read out of its slot into A and
         * written into the outgoing area, which sits at the bottom of
         * this frame so that it lands directly above the return address
         * the `call` is about to push. */
        if (sret) {
            place_arg(2, &cursor, &stk, &hid);
            if (hid.nstk) {
                y_to(F, RA);
                add_const16(F, RA, F->scratch_at + i->scratch);
                st_slot(F, 1 + hid.stk, RA, 2);
            }
        }
        for (k = 0; k < i->nargs; k++) {
            place_arg(i->argv[k].size, &cursor, &stk, &pl);
            if (!pl.nstk)
                continue;
            if (i->argv[k].is_struct && 1 + pl.stk + pl.nstk > 64) {
                /* Past std's reach the destination needs a pointer of
                 * its own, and the one st_slot builds is Z -- which held
                 * the SOURCE here: every byte after the first was read
                 * from the frame, not the struct (a sixth 16-byte struct
                 * argument came out as garbage). The source walks in X,
                 * the destination in Z. */
                g_x_hit = 1;
                ld_slot(F, AVR_X, sslot(F, i->argv[k].vreg), 2);
                y_to(F, AVR_Z);
                add_const16(F, AVR_Z, 1 + pl.stk);
                for (int b = 0; b < pl.nstk; b++) {
                    avr_ld(t, R_TMP, AVR_X, AVR_PTR_POST_INC);
                    avr_st(t, AVR_Z, R_TMP, AVR_PTR_POST_INC);
                }
            } else if (i->argv[k].is_struct) {
                /* The vreg holds the struct's ADDRESS; the bytes are what
                 * travels. Copied one at a time through r0 so that no
                 * argument register is disturbed. */
                ld_slot(F, AVR_Z, sslot(F, i->argv[k].vreg), 2);
                for (int b = 0; b < pl.nstk; b++) {
                    avr_ldd(t, R_TMP, AVR_Z, b);
                    st_slot(F, 1 + pl.stk + b, R_TMP, 1);
                }
            } else {
                int n = vw(F, i->argv[k].vreg), m = pl.nstk < n ? pl.nstk : n;
                /* r18 up is this store's scratch, and may be where another
                 * argument already lives -- in its home, before the
                 * parallel move below. -O0's `-5`, computed into r18-r25
                 * for the first argument, was overwritten so by the
                 * bytes of an eight-byte stack argument. Then the bytes
                 * go one at a time through r0, which nothing holds. */
                int busy = 0;
                for (int q = 0; q < i->nargs && !busy; q++) {
                    int v = i->argv[q].vreg;
                    if (q == k || i->argv[q].is_struct || !in_pair(F, v))
                        continue;
                    busy = F->loc[v] < RA + m && F->loc[v] + F->hw[v] > RA;
                }
                if (busy) {
                    for (int b = 0; b < pl.nstk; b++) {
                        if (b < m)
                            vld(F, R_TMP, i->argv[k].vreg, b, 1);
                        st_slot(F, 1 + pl.stk + b, b < m ? R_TMP : R_ZERO,
                                1);
                    }
                } else {
                    vld(F, RA, i->argv[k].vreg, 0, m);
                    st_slot(F, 1 + pl.stk, RA, pl.nstk);
                }
            }
        }
        /* Then the register ones: first every argument that lives in a
         * home, as ONE parallel move -- a home can be where another
         * argument goes, r22-r25 being homes as well as the first four
         * argument registers, so loading them in order would overwrite a
         * value before it was read. Then what comes from memory or is a
         * constant, which writes only its own registers and Z, none of
         * them a source by then. r0 breaks a cycle. */
        cursor = i->call_varargs ? -1 : ARG_TOP; stk = 0;
        if (sret)
            place_arg(2, &cursor, &stk, &hid);    /* the same slot again */
        {
            int md[48], ms[48], od[52], os[52], nm = 0, no;
            int zmoved = 0, zused = 0;
            if (sret && hid.nreg)
                argregs |= 3UL << hid.reg;
            for (k = 0; k < i->nargs; k++) {
                int v = i->argv[k].vreg;
                place_arg(i->argv[k].size, &cursor, &stk, &pl);
                if (!pl.nreg)
                    continue;
                argregs |= ((1UL << pl.nreg) - 1) << pl.reg;
                if (i->argv[k].is_struct) {
                    zused = 1;               /* its bytes come through Z */
                    continue;
                }
                if (!in_pair(F, v)) {
                    if (!is_remat(F, v) && slot_is_far(F->slot[v], pl.nreg))
                        zused = 1;           /* the far path walks with Z */
                    continue;
                }
                for (int b = 0; b < pl.nreg && b < F->hw[v]; b++) {
                    md[nm] = pl.reg + b;
                    ms[nm] = F->loc[v] + b;
                    nm++;
                }
            }
            /* An indirect callee's address, when an argument goes where it
             * lives: into Z in the same move, since afterwards it is gone.
             * Nothing after may then walk with Z -- an attempt that needs
             * it to is one a_verify's retry makes without that home. */
            if (i->indirect && in_pair(F, i->a) &&
                ((3UL << F->loc[i->a]) & argregs)) {
                md[nm] = AVR_Z;     ms[nm] = F->loc[i->a];     nm++;
                md[nm] = AVR_Z + 1; ms[nm] = F->loc[i->a] + 1; nm++;
                zmoved = 1;
                if (zused)
                    a_conflict(F, i->a);
            }
            no = ra_parallel_move(md, ms, nm, R_TMP, od, os, 52);
            if (no < 0)
                internal_error("avr: %s: a call's argument registers do not "
                               "form a parallel move", fn->name);
            emit_moves(t, od, os, no);
            cursor = i->call_varargs ? -1 : ARG_TOP; stk = 0;
            if (sret)
                place_arg(2, &cursor, &stk, &hid);
            for (k = 0; k < i->nargs; k++) {
                place_arg(i->argv[k].size, &cursor, &stk, &pl);
                if (!pl.nreg)
                    continue;
                if (i->argv[k].is_struct) {
                    /* A struct in registers is just its bytes, one per
                     * register -- no partial-word assembly, because a
                     * register here IS a byte. */
                    ld_slot(F, AVR_Z, sslot(F, i->argv[k].vreg), 2);
                    for (int b = 0; b < pl.nreg; b++)
                        avr_ldd(t, pl.reg + b, AVR_Z, b);
                } else if (!in_pair(F, i->argv[k].vreg)) {
                    vld(F, pl.reg, i->argv[k].vreg, 0, pl.nreg);
                }
            }
            /* The hidden result pointer goes in LAST, so nothing above can
             * have used its register as a scratch after it was set. (Its
             * stack form, for a variadic callee, went out with the other
             * stack words.) */
            if (sret && hid.nreg) {
                y_to(F, hid.reg);
                add_const16(F, hid.reg, F->scratch_at + i->scratch);
            }
            if (i->indirect && !zmoved)
                vld(F, AVR_Z, i->a, 0, 2);
        }
        /* After an alloca sp is below the frame, and the stack arguments
         * written to the outgoing area at Y+1 are not where the callee
         * looks, just above the return address. They are copied down:
         * sp -= n, then byte by byte from Y+1 through X, which no argument
         * travels in (they are r8-r25, and Z is an icall's target). sp
         * comes back up after the call. */
        long nout = fn->has_alloca ? call_stack_bytes(i) : 0;
        if (nout) {
            if (nout > 63)
                a_refuse(fn, i, "a call passing more than 63 bytes on the "
                                "stack in a function that uses alloca or a "
                                "variable-length array");
            avr_in(t, AVR_X, IO_SPL);
            avr_in(t, AVR_X + 1, IO_SPH);
            avr_sbiw(t, AVR_X, (int)nout);
            set_sp_from(t, AVR_X);
            avr_adiw(t, AVR_X, 1);
            for (long b = 0; b < nout; b++) {
                avr_ldd(t, R_TMP, AVR_Y, (int)(1 + b));
                avr_st(t, AVR_X, R_TMP, AVR_PTR_POST_INC);
            }
        }
        if (i->indirect) {
            /* Z holds a WORD address here, which is what icall wants and
             * what IR_FADDR put in the pointer. */
            avr_icall(t);
        } else if (!nout && F->tail && F->tail[n] &&
                   !(argregs & a_saved_mask(F)) &&
                   (F->tail[n] == 1 ||
                    (!F->frame && !F->use_y && !F->nsave))) {
            /* The epilogue's teardown, then a JUMP: the return address
             * this function was called with is on top of the stack again,
             * and the callee returns through it. Not when an argument
             * sits in a register the pops restore -- r8-r17 carry the
             * later arguments of a long call, and are saved exactly
             * because such a call writes them. And where the function
             * returns elsewhere too, only when the teardown is empty:
             * its copy here would sit beside the epilogue's, and the
             * pops and a `jmp` outweigh the `call` and `ret` they save. */
            a_teardown(F);
            note_call(F->st, t->len, i->callee);
            F->st->ext[F->st->next - 1].tail = 1;
            avr_jmp(t, 0);
            if (n + 1 < fn->nins)
                F->skip_next = 1;     /* the IR_RET: not reached */
            F->tail_made = 1;
            return;
        } else {
            note_call(F->st, t->len, i->callee);
            avr_call(t, 0);
        }
        if (nout) {
            /* X again: the result is in r18-r25 */
            avr_in(t, AVR_X, IO_SPL);
            avr_in(t, AVR_X + 1, IO_SPH);
            avr_adiw(t, AVR_X, (int)nout);
            set_sp_from(t, AVR_X);
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
            y_to(F, RA);
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
                vst(F, i->dst, 0, ret_reg(size), size);
                return;
            }
            /* The CALLER extends. The ABI leaves everything above the
             * return value's own bytes undefined -- r25 after a
             * char-returning call is not zero and not the sign -- and the
             * use site asks for a four-byte value. ret_tybytes/ret_tysign
             * exist in the IR for exactly this (see ir.h). */
            /* Only as far as something reads, and into its home: a
             * two-byte result read as two was moved byte by byte into A
             * and extended to four with two `mov r, r1`. */
            {
                int nb = dw(F, i), d, cp, rr = ret_reg(size);
                if (!nb)
                    return;
                d = dst_reg(F, i, nb);
                cp = size < nb ? size : nb;
                for (int b = 0; b < cp; ) {
                    if (b + 1 < cp && !((d + b) & 1) && !((rr + b) & 1)) {
                        if (d != rr) avr_movw(t, d + b, rr + b);
                        b += 2;
                    } else {
                        if (d != rr) avr_rr(t, AVR_MOV, d + b, rr + b);
                        b++;
                    }
                }
                if (nb > size)
                    extend(F, d, size, i->ret_tysign, nb);
                dst_done(F, i, d);
            }
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
        vld(F, RA, i->a, 0, 2);
        if (i->op == IR_MEMCPY)
            vld(F, RB, i->b, 0, 2);
        avr_movw(t, AVR_Z, RA);
        if (i->op == IR_MEMCPY) {
            g_x_hit = 1;
            avr_movw(t, AVR_X, RB);
        }
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
                vld(F, AVR_Z, o->temp, 0, 2);
                for (int b = 0; b < o->size; b++)
                    avr_ldd(t, o->reg + b, AVR_Z, b);
            }
            for (int k = 0; k < ia->nin; k++) {
                struct ir_asm_op *o = &ia->in[k];
                late = o->reg >= AVR_X;
                if (late != pass)
                    continue;
                vld(F, o->reg, o->temp, 0, o->size);
            }
            /* ...and an "m" output's register its ADDRESS, which nothing
             * loaded: the template wrote through whatever it held. */
            for (int k = 0; k < ia->nout; k++) {
                struct ir_asm_op *o = &ia->out[k];
                late = o->reg >= AVR_X;
                if (late != pass || !o->mem)
                    continue;
                vld(F, o->reg, o->temp, 0, 2);
            }
        }
        /* the bytes, padded at each alignment for where they land */
        code_put_asm(t, ia->code, ia->codelen, ia->drange, ia->ndrange,
                     ia->arange, ia->narange, CODE_FILL_ZERO);
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
            g_x_hit = 1; vld(F, AVR_X, o->temp, 0, 2);
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
        y_to(F, RA);
        add_const16(F, RA, F->frame + F->in_at + stk);
        /* `a` holds the ADDRESS of the va_list object. */
        vld(F, AVR_Z, i->a, 0, 2);
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
static void set_sp_from(struct code *t, int lo)
{
    avr_in(t, R_TMP, IO_SREG);
    avr_bclr(t, AVR_SREG_I);                 /* cli */
    avr_out(t, IO_SPH, lo + 1);
    avr_out(t, IO_SREG, R_TMP);
    avr_out(t, IO_SPL, lo);
}

static void set_sp_from_y(struct code *t)
{
    set_sp_from(t, AVR_Y);
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

/* ---- register allocation ---------------------------------------------
 *
 * The shared allocator hands out one register per value; here each "reg"
 * it is given is the LOW half of an even-aligned PAIR -- r2:r3 up to
 * r16:r17, the call-saved registers -- so a two-byte value moves with one
 * movw. Only values of which nothing reads more than two bytes are
 * candidates (avr_demand's answer; the IR has no two-byte width class, so
 * an `int` arrives as four bytes already extended). Wider ones, 64-bit
 * ones, floats, parameters and anything the lowerings touch through a
 * slot directly stay in memory: the allocator's own flags keep call
 * arguments and returned values there, and avr_excl() the rest.
 *
 * Call-saved, so nothing is lost across a call -- except that a call with
 * many arguments LOADS them into r8-r17, so each function's pool stops
 * below the lowest argument register any of its calls uses. */
static const int A_POOL[8] = { 2, 4, 6, 8, 10, 12, 14, 16 };
/* r24, r22 and X (g_a_vol, g_a_xhome), then the eight call-saved pairs */
static int g_a_pool[11];
static int g_a_npool;
static int g_a_regalloc;

/* The call-clobbered homes in B (r24:r25, r22:r23, or r22-r25 as one
 * quad) are offered when g_a_vol is set. Per vreg, for the attempt being
 * generated: g_a_novol keeps a value out of every call-clobbered home, X
 * included, and g_a_nohome out of every home -- each set because a_verify
 * caught the emitted code overwriting that value there. g_a_pend is what
 * this generation caught, merged into those by avr_attempt. */
static int g_a_vol = 1;
/* Homes for eight-byte values (the first allocation pass): g_a_oct. */
static int g_a_oct = 1;

static const int *a_pool_for(const struct ir_func *fn, int *n)
{
    (void)fn;
    *n = g_a_npool;
    return g_a_pool;
}
static int a_callee_saved(int r) { return r >= 2 && r <= 17; }
/* Is `dst = ldvar(local)` a plain copy here, so the two may share a home?
 * At the full width, always. Narrower too: an `int` is two bytes on this
 * machine and the IR computes at four, so nearly every load of a local is
 * `ldvar.4:2` -- and the extension that makes it more than a copy is only
 * emitted when more bytes are demanded than the local has, which a PAIR
 * cannot be asked for (it holds two). The only bytes an in-place extension
 * could write are above the local's own size, which nothing reads as the
 * local. Answering "no" kept every such pair of values apart, and a copy
 * between two homes at each use. */
static int a_ldvar_plain(int size, int sign, int w)
{
    (void)sign;
    return size == w || size <= 2;
}
/* Which operations become a helper call. Nothing needed this while every
 * home was call-saved; X is not, so a value in it must not live across
 * one. Conservative -- a multiply by a constant is shifts and adds, and
 * saying it calls only keeps X from a value across it. */
static int a_calls_helper(const struct ir_ins *i)
{
    /* A multiply by an immediate below 2^17 is shifts and adds in place
     * (IR_MUL), and calls nothing. */
    if (i->op == IR_MUL && i->imm_b && i->w <= VW && !i->flt &&
        i->imm > -(1L << 17) && i->imm < (1L << 17))
        return 0;
    return i->flt || i->op == IR_I2F || i->op == IR_F2I ||
           i->op == IR_F2F || i->op == IR_MUL || i->op == IR_DIV ||
           i->op == IR_MOD;
}

static const char *a_saved_only(const struct ir_func *fn)
{
    return g_a_novol && fn->nvregs <= g_a_nvr ? g_a_novol : NULL;
}

/* The register the ABI puts each boundary value in, as the allocator's
 * hint: a parameter where it arrives, a call's result and a returned
 * value where they come back, an argument where it goes. A value in
 * that register needs no copy at the boundary -- `return f(x)` was a
 * call, `movw r26, r24` and `movw r24, r26`. Only r24, r22 and the r22
 * quad are in the pool, so any other answer is simply not taken, and a
 * hint is dropped like any other when the register is not free. */
static void a_abi_hints(const struct ir_func *fn, int *hint)
{
    struct argplace pl;
    int cursor = ARG_TOP, nv = fn->nvregs;
    long stk = 0;

    if (!fn->is_varargs) {
        if (fn_sret_bytes(fn))
            place_arg(2, &cursor, &stk, &pl);
        for (int a = 0; a < fn->nparams; a++) {
            const struct ir_arg *pa = &fn->param_abi[a];
            place_arg(pa->size, &cursor, &stk, &pl);
            if (pl.nreg && !pl.nstk && !pa->is_struct &&
                pa->vreg >= 0 && pa->vreg < nv)
                hint[pa->vreg] = pl.reg;
        }
    }
    for (int k = 0; k < fn->nins; k++) {
        const struct ir_ins *i = &fn->ins[k];
        if (i->op == IR_RET && i->a >= 0 && i->a < nv &&
            !fn->ret_abi.is_struct)
            hint[i->a] = ret_reg(fn->ret_abi.size ? fn->ret_abi.size : 2);
        if (i->op != IR_CALL || i->call_varargs)
            continue;
        if (i->dst >= 0 && i->dst < nv && !i->retsize && i->ret_tybytes &&
            i->ret_tybytes <= VW)
            hint[i->dst] = ret_reg(i->ret_tybytes);
        cursor = ARG_TOP; stk = 0;
        if (sret_bytes(i->retsize))
            place_arg(2, &cursor, &stk, &pl);
        for (int a = 0; a < i->nargs; a++) {
            int v = i->argv[a].vreg;
            place_arg(i->argv[a].size, &cursor, &stk, &pl);
            if (pl.nreg && !pl.nstk && !i->argv[a].is_struct &&
                v >= 0 && v < nv && hint[v] < 0)
                hint[v] = pl.reg;
        }
    }
}

/* Which pass is allocating (avr_ra_pass): an `ext.4:2` is a copy in a
 * PAIR, which holds the two bytes it keeps, and not in a quad, whose
 * upper two it would write. */
static int g_a_quadpass;

static int a_ext_plain(const struct ir_ins *i)
{
    return !g_a_quadpass && i->size == 2;
}

static const struct ra_target AVR_RA = {
    a_pool_for, a_callee_saved, a_ldvar_plain,
    1, 1, 0,          /* call arguments and returns from a home; memcpy's
                       * addresses from their slots */
    a_calls_helper,   /* for the call-clobbered homes, B and X */
    0,
    a_abi_hints,
    NULL, NULL,
    1,                /* float_in_gpr: a float is four bytes in a quad,
                       * and every float lowering reads through vld/rd4 */
    a_saved_only,
    a_ext_plain,
    0, /* atomic_in_reg */
    0, /* fp_reads_gpr */
    0, /* asm_in_reg */
    NULL, /* remat_ok */
    NULL  /* call_target_in_reg */
};

/* The lowest register any call this function makes -- the IR's, and the
 * ones codegen makes that the IR does not show -- loads an argument into,
 * or 26 for none.
 *
 * The helpers matter: an eight-byte multiply or divide is __muldi3 and
 * friends, whose SECOND argument travels in r10-r17. Missing them, the
 * allocator gave a loop counter r14:r17 across `x * 6364136223846793005ULL`,
 * the argument load overwrote it, and tests/golden/bitops-width.c never
 * left its loop. The float helpers take theirs in r18-r25. */
static int avr_call_low(const struct ir_func *fn)
{
    int low = 26;
    for (int k = 0; k < fn->nins; k++) {
        const struct ir_ins *i = &fn->ins[k];
        int cursor;
        long stk = 0;
        struct argplace p;
        if ((i->op == IR_MUL || i->op == IR_DIV || i->op == IR_MOD) &&
            i->w == 8) {
            cursor = ARG_TOP;
            place_arg(8, &cursor, &stk, &p);
            place_arg(8, &cursor, &stk, &p);
            if (p.nreg && p.reg < low) low = p.reg;
            continue;
        }
        if (i->op != IR_CALL || i->call_varargs)
            continue;
        cursor = ARG_TOP;
        if (sret_bytes(i->retsize)) {
            place_arg(2, &cursor, &stk, &p);
            if (p.nreg && p.reg < low) low = p.reg;
        }
        for (int a = 0; a < i->nargs; a++) {
            place_arg(i->argv[a].size, &cursor, &stk, &p);
            if (p.nreg && p.reg < low) low = p.reg;
        }
    }
    return low;
}

/* Below which every home the allocator hands out must lie: avr_call_low,
 * so that loading one argument cannot overwrite a value another still has
 * to be read from, and the lowest register this function's own
 * parameters ARRIVE in, so that the prologue, storing each parameter to
 * its home, cannot land on one not yet stored. */
static int avr_arg_low(const struct ir_func *fn)
{
    int low = avr_call_low(fn);
    if (!fn->is_varargs) {
        int cursor = ARG_TOP;
        long stk = 0;
        struct argplace p;
        if (fn_sret_bytes(fn)) {
            place_arg(2, &cursor, &stk, &p);
            if (p.nreg && p.reg < low) low = p.reg;
        }
        for (int a = 0; a < fn->nparams; a++) {
            place_arg(fn->param_abi[a].size, &cursor, &stk, &p);
            if (p.nreg && p.reg < low) low = p.reg;
        }
    }
    return low;
}

/* Values the allocator must leave in memory. `quad` asks the other
 * question: which must stay out of the QUAD pass, which takes only the
 * values whose reads need three or four bytes. */
static char *avr_excl(const struct a_fn *F, int hw)
{
    const struct ir_func *fn = F->fn;
    int nv = fn->nvregs, quad = hw == 4;
    char *x = xcalloc((size_t)(nv ? nv : 1), 1);
    for (int v = 0; v < nv; v++) {
        if (hw == 8)
            x[v] = !F->wide[v];      /* the eight-byte pass takes only them */
        else if (F->wide[v])
            x[v] = 1;
        else if (v < fn->nvars)
            x[v] = quad ? fn->locals[v].size != 4 : fn->locals[v].size > 2;
        else
            x[v] = quad ? cw(F, v) < 3 || cw(F, v) > 4 : cw(F, v) > 2;
        if (F->remat && F->remat[v])
            x[v] = 1;                /* rebuilt where it is read */
        if (g_a_nohome && v < g_a_nvr && g_a_nohome[v])
            x[v] = 1;                /* overwritten where it was (a_verify) */
    }
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        /* copied slot to slot (gen_select) */
        if (i->op == IR_SELECT) {
            if (i->dst >= 0 && i->dst < nv) x[i->dst] = 1;
            if (i->b >= 0 && i->b < nv) x[i->b] = 1;
            if (i->c >= 0 && i->c < nv) x[i->c] = 1;
        }
    }
    return x;
}

/* Two passes of the shared allocator, because it hands out one register
 * per value and a four-byte value needs four. One pass colours the
 * four-byte values with QUADS -- two adjacent pairs, named by the low
 * register -- and the other the two-byte ones with PAIRS, each from what
 * the first left. A pair or quad the first pass took is withdrawn for the
 * whole function, not just where its value is live: coarser than one
 * graph with both classes in it, and sound without teaching the colourer
 * about overlapping registers.
 *
 * Which pass goes first decides who starves. Quads first shrank the
 * 32-bit kernels by a fifth, and grew tests/exec/byte-regs.c by a
 * quarter: its `int` temporaries that read four bytes took every quad,
 * and the two-byte values that had pairs lost them. And no estimate of
 * the traffic saved is the answer either: tests/exec/switch.c's main
 * grew with the better estimate, because the values that lost their
 * pairs pushed its frame past Y+63, where every access costs five more
 * instructions. So gen_func is run under each choice (AVR_RA_*) and the
 * shortest code is kept -- the pairs-only one is what the allocator did
 * before quads, so no function comes out longer than it did then. */
enum { AVR_RA_NONE, AVR_RA_QUADS_FIRST, AVR_RA_PAIRS_FIRST, AVR_RA_PAIRS_ONLY };

struct avr_ra {
    int *loc;
    unsigned char *hw;
    int used[RA_MAXPOOL], nsave;
};

/* How many PAIRS the allocator may take for this attempt, a quad being
 * two. Every pair it takes is two pushes and two pops, eight bytes, and
 * the allocator counts them as free -- so a function saved all sixteen
 * registers for values that were each read twice. gen_func_best tries a
 * few budgets and keeps what the size says. */
static int g_a_cap = 8;

/* The pass for values of `hw` bytes: 2 (pairs), 4 (quads) or 8. */
static void avr_ra_pass(struct a_fn *F, int hw, int low, int *taken,
                        const char *also, struct avr_ra *r)
{
    int quad = hw == 4;
    struct ir_func *fn = F->fn;
    int nv = fn->nvregs, used[RA_MAXPOOL], nused = 0;
    int *a = NULL;
    char *x;

    g_a_npool = 0;
    {
        /* The call-clobbered homes first: a value that crosses no call
         * takes one and costs no push. They are no argument register a
         * call's loads could overwrite first -- those are a parallel
         * move now (see IR_CALL) -- so `low` does not bound them, and
         * neither does the budget, which counts pushes. */
        int have = 0, left;
        for (int b = 2; b <= 17; b++)
            if (*taken & (1 << b)) have++;
        left = (g_a_cap - have / 2) / (hw / 2);
        if (hw == 8) {
            /* r18..r25 -- where libgcc's 64-bit helpers take their first
             * argument and return, so a value there is neither pushed nor
             * moved for one -- then r10..r17, where they take the second,
             * and r2..r9. */
            if (g_a_vol && !(*taken & (0xFF << 18)))
                g_a_pool[g_a_npool++] = 18;
            for (int b = 10; b >= 2 && left > 0; b -= 8)
                if (b + 7 < low && !(*taken & (0xFF << b))) {
                    g_a_pool[g_a_npool++] = b;
                    left--;
                }
        } else if (quad) {
            if (g_a_vol && !(*taken & (0xF << 22)))
                g_a_pool[g_a_npool++] = 22;
            /* r2..r5, r6..r9, r10..r13, r14..r17 */
            for (int b = 2; b + 3 < low && b <= 14 && left > 0; b += 4)
                if (!(*taken & (0xF << b))) {
                    g_a_pool[g_a_npool++] = b;
                    left--;
                }
        } else {
            if (g_a_vol && !(*taken & (3 << 24)))
                g_a_pool[g_a_npool++] = 24;
            if (g_a_vol && !(*taken & (3 << 22)))
                g_a_pool[g_a_npool++] = 22;
            /* X, call-clobbered as well, and no argument register */
            if (g_a_xhome && !(*taken & (3 << AVR_X)))
                g_a_pool[g_a_npool++] = AVR_X;
            for (int k = 0; k < 8 && left > 0; k++)
                if (A_POOL[k] + 1 < low && !(*taken & (3 << A_POOL[k]))) {
                    g_a_pool[g_a_npool++] = A_POOL[k];
                    left--;
                }
        }
    }
    if (!g_a_npool)
        return;
    x = avr_excl(F, hw);
    for (int v = 0; v < nv; v++)
        if (also[v]) x[v] = 1;
    /* -O0 and -Og (target_keep_vars): the source variables stay in
     * their slots, where a debugger reads them; the temporaries are
     * still allocated, as on the other targets. Turning allocation off
     * altogether, as -g once did, doubled the code. */
    if (g_a_o0 || target_keep_vars()) {
        char *pin = ra_debug_pin_vars(fn);
        for (int v = 0; pin && v < nv; v++)
            if (pin[v]) x[v] = 1;
        free(pin);
    }
    g_a_quadpass = hw != 2;
    /* `wide` tells the allocator which values no register can hold, and
     * for the eight-byte pass that is none of them: its registers are
     * runs of eight. The other passes leave them out through `x`. */
    a = ra_allocate(fn, &AVR_RA, hw == 8 ? NULL : F->wide, x, used, &nused);
    g_a_quadpass = 0;
    free(x);
    for (int k = 0; k < nused; k++)
        for (int q = 0; q < hw; q += 2)
            r->used[r->nsave++] = used[k] + q;
    /* Taken for the other pass by what was HANDED OUT, not by what is
     * saved: `used` lists the call-saved registers only, and a quad in
     * r22-r25 left out of `taken` would be given again as a pair. */
    for (int v = 0; v < nv; v++)
        if (a[v] >= 0) {
            r->loc[v] = a[v];
            r->hw[v] = (unsigned char)hw;
            *taken |= ((1 << hw) - 1) << a[v];
        }
    free(a);
}

static void avr_ra_try(struct a_fn *F, int mode, struct avr_ra *r)
{
    int quads_first = mode == AVR_RA_QUADS_FIRST;
    struct ir_func *fn = F->fn;
    int nv = fn->nvregs, low = avr_arg_low(fn), taken = 0;
    char *also = xcalloc((size_t)(nv ? nv : 1), 1);

    r->loc = xmalloc((size_t)(nv ? nv : 1) * sizeof *r->loc);
    r->hw = xcalloc((size_t)(nv ? nv : 1), 1);
    for (int v = 0; v < nv; v++) r->loc[v] = -1;
    r->nsave = 0;
    /* The eight-byte values first, in every mode: one of those in memory
     * costs four instructions a byte for every operation on it, and a
     * home is a run of eight registers that only this pass can find
     * free. */
    if (g_a_oct) {
        avr_ra_pass(F, 8, low, &taken, also, r);
        for (int v = 0; v < nv; v++) also[v] = r->loc[v] >= 0;
    }
    avr_ra_pass(F, quads_first ? 4 : 2, low, &taken, also, r);
    for (int v = 0; v < nv; v++) also[v] = r->loc[v] >= 0;
    if (mode != AVR_RA_PAIRS_ONLY)
        avr_ra_pass(F, quads_first ? 2 : 4, low, &taken, also, r);
    free(also);
}

/* An environment knob, with an EMPTY value read as unset: a script that
 * writes `EMBCC_AVR_RA_MODE=$mode cmd` with no mode must get the default,
 * not mode 0. */
static const char *avr_knob(const char *name)
{
    const char *v = getenv(name);
    return v && *v ? v : NULL;
}

/* Knobs for finding a miscompile in all this, each read at compile time:
 *
 *   EMBCC_AVR_RA=1         each function's homes and each mode's size
 *   EMBCC_AVR_RA_MODE=n    only mode n (AVR_RA_*), so a mode that never
 *                          wins is still testable -- the tests force each
 *   EMBCC_AVR_RA_ONLY=fn   allocate in that one function only
 *   EMBCC_AVR_RA_LIMIT=k   keep the first k homes and drop the rest
 *
 * Dropping homes is always sound, so bisecting k names the one value
 * whose home breaks the program; that is how the r10-r17 argument loads
 * below were found. */
static void avr_regalloc(struct a_fn *F, int mode)
{
    struct avr_ra r;

    avr_ra_try(F, mode, &r);
    for (int v = 0; v < F->fn->nvregs; v++)
        if (r.loc[v] == AVR_X) g_x_alloc = 1;
    if (avr_knob("EMBCC_AVR_RA_LIMIT")) {
        int lim = atoi(avr_knob("EMBCC_AVR_RA_LIMIT")), c = 0;
        for (int v = 0; v < F->fn->nvregs; v++)
            if (r.loc[v] >= 0 && c++ >= lim) r.loc[v] = -1;
    }
    if (avr_knob("EMBCC_AVR_RA")) {
        fprintf(stderr, "%s mode %d:", F->fn->name, mode);
        for (int v = 0; v < F->fn->nvregs; v++)
            if (r.loc[v] >= 0)
                fprintf(stderr, " %%%d=r%d/%d", v, r.loc[v], r.hw[v]);
        fprintf(stderr, " save:");
        for (int k = 0; k < r.nsave; k++) fprintf(stderr, " r%d", r.used[k]);
        fprintf(stderr, "\n");
    }
    /* Anything allocated at all -- not "anything saved": a value in X
     * needs no push, and testing nsave threw such an allocation away and
     * left the function a frame for it. */
    int any = 0;
    for (int v = 0; v < F->fn->nvregs; v++)
        if (r.loc[v] >= 0) any = 1;
    if (any) {
        F->loc = r.loc; F->hw = r.hw;
        F->nsave = r.nsave;
        for (int k = 0; k < r.nsave; k++) F->used_callee[k] = r.used[k];
    } else {
        free(r.loc); free(r.hw);
    }
}

static void gen_func(struct ir_func *fn, struct code *t, struct a_sites *st,
                     int keep_vars, int ra_mode, struct avr_relax *rx)
{
    struct func *f = fn->src;
    struct a_fn F;
    int i;

    memset(&F, 0, sizeof F);
    F.fn = fn; F.t = t; F.st = st;
    F.keep_vars = keep_vars;
    F.rx = rx;

    if (fn->neh)
        a_refuse(fn, NULL, "an exception region");
    /* The width map BEFORE layout: a slot's size depends on it. */
    F.wide = avr_wide_map(fn);
    ext_info(&F);
    F.need = avr_demand(&F);
    if (fn->nvregs) {
        F.usecnt = xmalloc((size_t)fn->nvregs * sizeof *F.usecnt);
        ra_count_vreg_uses(fn, F.usecnt);
    }
    const_map(&F);
    if (ra_mode != AVR_RA_NONE) {
        F.remat = xcalloc((size_t)(fn->nvregs ? fn->nvregs : 1), 1);
        for (int v = fn->nvars; v < fn->nvregs; v++)
            F.remat[v] = F.cknown[v] == 1;
        /* EMBCC_AVR_REMAT_MAX=k: only the first k are rebuilt, the rest
         * kept -- for bisecting one that is rebuilt wrong. */
        if (avr_knob("EMBCC_AVR_REMAT_MAX")) {
            int lim = atoi(avr_knob("EMBCC_AVR_REMAT_MAX")), c = 0;
            for (int v = 0; v < fn->nvregs; v++)
                if (F.remat[v] && c++ >= lim) F.remat[v] = 0;
        }
        /* ...but not an arm of a select, which gen_select copies slot to
         * slot (the same reason avr_excl keeps them out of homes). */
        for (int k = 0; k < fn->nins; k++)
            if (fn->ins[k].op == IR_SELECT) {
                const struct ir_ins *s = &fn->ins[k];
                if (s->b >= 0 && s->b < fn->nvregs) F.remat[s->b] = 0;
                if (s->c >= 0 && s->c < fn->nvregs) F.remat[s->c] = 0;
            }
    }
    F.last_st.v = -1;
    F.zv = -1;
    if (ra_mode != AVR_RA_NONE)
        avr_regalloc(&F, ra_mode);
    /* Loading a call's arguments WRITES r8-r17 once there are enough of
     * them, and those are call-saved: avr-gcc saves each one a function
     * writes, argument loads included, and so must this. Missing it was
     * invisible while no EmbCC function kept anything in r2-r17 -- and
     * wrong for every avr-gcc caller that did. With the allocator it was
     * EmbCC's own: __modsi3 loads &r into r16:r17 to call udivmod, and a
     * caller's constant in r14:r17 came back as an address. At every
     * level, since the loads are there at -O0 too. */
    for (int r = avr_call_low(fn) & ~1; r <= 16; r += 2) {
        int have = 0;
        if (r < 2)
            continue;
        for (i = 0; i < F.nsave; i++)
            if (F.used_callee[i] == r) have = 1;
        if (!have)
            F.used_callee[F.nsave++] = r;
    }
    layout(&F);

    F.label_off = xmalloc((size_t)(fn->nlabels + 1) * sizeof *F.label_off);
    for (i = 0; i <= fn->nlabels; i++)
        F.label_off[i] = -1;

    f->code_align = 2;                  /* a word of flash */
    /* An asm that aligns its own bytes (`.p2align 2`): the function starts
     * on that, so that in a section of its own the buffer's offsets are
     * the section's. The padding, nops, is never run. */
    for (i = 0; i < fn->nins; i++) {
        const struct ir_asm *ia = fn->ins[i].op == IR_ASM ? fn->ins[i].asm_ir
                                                          : NULL;
        int m = ia ? code_asm_align_max(ia->arange, ia->narange) : 0;
        if (m > f->code_align) {
            code_fill(t, CODE_FILL_ZERO, (m - t->len % m) % m);
            f->code_align = m;
        }
    }
    f->code_off = t->len;
    if (target_debug_info()) {
        int nv = fn->nvars ? fn->nvars : 1;
        free(fn->var_off);             /* this function generated again */
        fn->var_off = xmalloc((size_t)nv * sizeof *fn->var_off);
        for (int v = 0; v < fn->nvars; v++)
            fn->var_off[v] = ra_var_home(fn, v, F.slot[v] >= 0, F.slot[v]);
        /* ...and its rows recorded again. The row at the entry, with the
         * line the function was declared on, is the DWARF writer's to add
         * (src/debug/dwarf.c, emit_line_func), for every target. */
        fn->nlines = 0;
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
    /* The pairs the allocator took, BEFORE Y: then the distance from Y
     * to the incoming stack arguments grows by exactly what was pushed. */
    for (i = 0; i < F.nsave; i++) {
        avr_push(t, F.used_callee[i]);
        avr_push(t, F.used_callee[i] + 1);
    }
    F.in_at = INCOMING_AT + 2L * F.nsave;
    /* Y is the frame pointer, and a function with no frame -- no slot, no
     * outgoing area, nothing on the stack coming in -- never reads it:
     * then it is neither saved nor set, which is twelve bytes a function.
     * Every Y-relative access is to one of those, so F.frame is zero
     * without them; stack parameters and varargs are the two that are
     * above the frame instead of in it. */
    {
        struct argplace pl;
        int cursor = ARG_TOP;
        long stk = 0;
        F.use_y = f->is_isr || fn->is_varargs || F.frame != 0 ||
                  F.sret_slot >= 0 || fn->has_alloca;
        /* __builtin_frame_address / _return_address read above Y */
        for (i = 0; i < fn->nins && !F.use_y; i++)
            if (fn->ins[i].op == IR_FRAMEADDR)
                F.use_y = 1;
        if (fn_sret_bytes(fn)) place_arg(2, &cursor, &stk, &pl);
        for (i = 0; i < fn->nparams && !F.use_y; i++) {
            place_arg(fn->param_abi[i].size, &cursor, &stk, &pl);
            if (pl.nstk) F.use_y = 1;
        }
    }
    if (!f->is_isr && F.use_y) {
        avr_push(t, 28);
        avr_push(t, 29);
    }
    if (F.use_y) {
        avr_in(t, 28, IO_SPL);
        avr_in(t, 29, IO_SPH);
    }
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
            if (p0.nreg) {
                st_slot(&F, F.sret_slot, p0.reg, 2);
            } else {
                /* A variadic function's is on the stack, as its first word
                 * -- clang's layout, and what a caller's place_arg gives it.
                 * This used to be a refusal. */
                ld_slot(&F, RA, F.frame + F.in_at + p0.stk, 2);
                st_slot(&F, F.sret_slot, RA, 2);
            }
        }
        /* Those arriving in registers: the ones kept in memory stored
         * first, which reads the argument registers and writes none, then
         * the ones with a home as ONE parallel move, since a home may be
         * where another parameter arrived -- two ints swapped between
         * r24 and r22 is a cycle. The stack ones last, through A, which
         * no parameter arrives in by then and nothing lives in. */
        {
            int md[40], ms[40], od[48], os[48], nm = 0, no;
            int c2 = cursor;
            long s2 = stk;
            for (i = 0; i < fn->nparams; i++) {
                struct ir_arg *a = &fn->param_abi[i];
                place_arg(a->size, &c2, &s2, &pl);
                if (!pl.nreg)
                    continue;
                if (!in_pair(&F, a->vreg)) {
                    vst(&F, a->vreg, 0, pl.reg, pl.nreg);
                    continue;
                }
                /* A parameter nothing reads is live nowhere, so its home
                 * may be a live one's: moving it there would clobber that,
                 * and two moves into one register are no parallel move. */
                if (F.usecnt && !F.usecnt[a->vreg])
                    continue;
                for (int b = 0; b < pl.nreg && b < F.hw[a->vreg]; b++) {
                    md[nm] = F.loc[a->vreg] + b;
                    ms[nm] = pl.reg + b;
                    nm++;
                }
            }
            no = ra_parallel_move(md, ms, nm, R_TMP, od, os, 48);
            if (no < 0)
                internal_error("avr: %s: the parameters' homes do not form a "
                               "parallel move", fn->name);
            emit_moves(t, od, os, no);
            F.last_st.v = -1;
        }
        for (i = 0; i < fn->nparams; i++) {
            struct ir_arg *a = &fn->param_abi[i];
            place_arg(a->size, &cursor, &stk, &pl);
            if (pl.nreg) {
                continue;                           /* placed above */
            } else if (pl.nstk <= VW) {
                ld_slot(&F, RA, F.frame + F.in_at + pl.stk, pl.nstk);
                vst(&F, a->vreg, 0, RA, pl.nstk);
            } else if (in_pair(&F, a->vreg)) {
                /* A long long the allocator gave a home: straight into it,
                 * a byte at a time. It has no slot -- the copy below asked
                 * for one and was refused, for any function whose eighth
                 * argument byte onwards held a 64-bit parameter. Not when
                 * nothing reads it: as above, its home may be a live one's. */
                if (F.usecnt && !F.usecnt[a->vreg])
                    continue;
                for (int b = 0; b < pl.nstk && b < F.hw[a->vreg]; b++)
                    ld_slot(&F, F.loc[a->vreg] + b,
                            F.frame + F.in_at + pl.stk + b, 1);
            } else {
                /* Wider than a scratch bank -- a struct, or a long long:
                 * byte at a time, which needs no run of registers. */
                for (int b = 0; b < pl.nstk; b++) {
                    ld_slot(&F, R_TMP, F.frame + F.in_at + pl.stk + b, 1);
                    st_slot(&F, sslot(&F, a->vreg) + b, R_TMP, 1);
                }
            }
        }
    }

    int tail_end = 0;        /* the body's last act is a tail call */
    /* Which calls may be tail calls (a_tail_ok); whether each is made
     * waits for the argument registers it uses. */
    F.tail = NULL;
    if (F.loc && !g_a_o0)
        for (i = 0; i < fn->nins; i++)
            if (a_tail_ok(&F, i)) {
                if (!F.tail)
                    F.tail = xcalloc((size_t)fn->nins, 1);
                F.tail[i] = (char)a_tail_ok(&F, i);
            }
    {
        /* Liveness for a_verify, when anything has a home to check. */
        int *hl_off = NULL, *hl_v = NULL;
        if (F.loc && fn->nvregs && fn->nins)
            a_homed_map(&F, &hl_off, &hl_v);
        F.zv = -1;
        for (i = 0; i < fn->nins; i++) {
            long at = t->len;
            int first = i;
            F.tail_made = 0;
            if (target_debug_info())
                a_line_row(fn, t->len, fn->ins[i].line);
            F.tdepth = 0;
            gen_ins(&F, i);
            if (F.skip_next) {          /* the compare emitted its branch */
                F.skip_next = 0;
                i++;
            }
            if (fn->ins[first].dst >= 0 && fn->ins[first].dst == F.zv)
                F.zv = -1;              /* Z's pointer was redefined */
            if (hl_off)
                a_verify(&F, first, i, at, hl_off, hl_v);
            tail_end = i == fn->nins - 1 && F.tail_made;
        }
        free(hl_off); free(hl_v);
    }

    /* ---- epilogue ---- */
    /* ...unless nothing reaches it: the body ended in a tail call and no
     * IR_RET jumps here. */
    F.label_off[fn->nlabels] = t->len;
    for (i = 0; tail_end && i < F.nfix; i++)
        if (F.fix[i].label == fn->nlabels)
            tail_end = 0;
    for (i = 0; tail_end && i < F.njs; i++)
        if (F.js[i].label == fn->nlabels)
            tail_end = 0;
    if (tail_end) {
        ;
    } else if (f->is_isr) {
        if (F.frame)
            add_const16(&F, AVR_Y, F.frame);
        if (F.frame || fn->has_alloca)
            set_sp_from_y(t);
        /* The pairs the prologue pushed after isr_prologue's registers
         * (r8-r17 a call's arguments are loaded into): popped first.
         * Leaving them on the stack popped Y, Z, X and the rest from
         * the wrong bytes, and reti returned to one of them. */
        for (i = F.nsave - 1; i >= 0; i--) {
            avr_pop(t, F.used_callee[i] + 1);
            avr_pop(t, F.used_callee[i]);
        }
        isr_epilogue(t);
    } else {
        a_teardown(&F);
        avr_ret(t);
    }

    for (i = 0; i < F.nfix; i++) {
        int at = F.fix[i].at;
        int to = F.label_off[F.fix[i].label];
        if (to < 0)
            a_refuse(fn, NULL, "a jump to a label that was never placed");
        if (F.fix[i].wide == 3) {                     /* &&label */
            F.st->f[at].addend = to - f->code_off;
            F.st->f[at + 1].addend = to - f->code_off;
            continue;
        }
        if (F.fix[i].wide != 1) {
            /* A short form relaxation chose: it must reach, or this
             * attempt is thrown away and that site pinned long. One
             * emitted without a hint was measured, and cannot miss. */
            long d = (to - (at + 2)) / 2;
            int lim = F.fix[i].wide == 2 ? 64 : 2048;
            if (d < -lim || d > lim - 1) {
                if (F.rx && F.fix[i].site >= 0) {
                    F.rx->bad = F.fix[i].site;
                    continue;
                }
            }
            if (F.fix[i].wide == 2) {
                avr_patch_br(t, at, (int)d);
                continue;
            }
        }
        if (F.fix[i].wide == 1) {
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
    /* The frame, the pairs and Y the prologue pushed, the return address
     * the call (or the interrupt) pushed, and the most the body pushes
     * on top of all that for a moment (t_push). A handler's prologue also
     * pushes everything in ISR_SAVE but Y (counted above), and SREG. */
    f->stack_bytes = (int)F.frame + 2 * F.nsave + 2 * F.use_y /* Y */ +
                     2 /* the return address */ + F.tpeak;
    if (f->is_isr)
        f->stack_bytes += (int)(sizeof ISR_SAVE / sizeof ISR_SAVE[0]) - 2 + 1;
    f->code_len = t->len - f->code_off;
    if (F.rx) {
        struct avr_relax *rx = F.rx;
        if (F.njs > rx->cap) {
            rx->cap = F.njs;
            rx->hint = xrealloc(rx->hint, (size_t)rx->cap);
            rx->fits = xrealloc(rx->fits, (size_t)rx->cap);
        }
        for (i = rx->nsite; i < F.njs; i++) rx->hint[i] = 0;
        rx->nsite = F.njs;
        /* A jump left out must have had its target right where it stood. */
        for (i = 0; i < F.njs; i++)
            if (F.js[i].cond < 0 && F.js[i].end == F.js[i].at &&
                F.label_off[F.js[i].label] != F.js[i].at)
                rx->bad = i;
        for (i = 0; i < F.njs; i++) {
            long to = F.label_off[F.js[i].label], at = F.js[i].at;
            long d = (to - (at + 2)) / 2;
            rx->fits[i] = 0;
            if (to < 0)
                continue;
            if (F.js[i].cond >= 0 && d >= -64 && d <= 63)
                rx->fits[i] = 2;
            else if (F.js[i].cond >= 0) {
                d = (to - (at + 4)) / 2;       /* the rjmp after the skip */
                if (d >= -2048 && d <= 2047) rx->fits[i] = 1;
            } else if (to == F.js[i].end)
                rx->fits[i] = 3;
            else if (d >= -2048 && d <= 2047)
                rx->fits[i] = 1;
        }
    }
    free(F.js);
    free(F.slot);
    free(F.need);
    cg_note_labels(fn, F.label_off);
    free(F.label_off);
    free(F.fix);
    free(F.cval);
    free(F.cknown);
    free(F.remat);
    free(F.wide);
    free(F.loc);
    free(F.hw);
    free(F.usecnt);
    free(F.tail);
}

/* One allocation mode, relaxed (struct avr_relax): generated until no site
 * can shorten further. Ends with that mode's code in `t`, and returns its
 * length. The caller has marked where the function starts in `rb`. */
struct avr_rollback { int at, next, nstr, ng, nf; };

static void avr_rollback(struct code *t, struct a_sites *st,
                         const struct avr_rollback *rb)
{
    t->len = rb->at; st->next = rb->next; st->nstr = rb->nstr;
    st->ng = rb->ng; st->nf = rb->nf;
}

static int gen_relaxed(struct ir_func *fn, struct code *t, struct a_sites *st,
                       int keep_vars, int mode, const struct avr_rollback *rb)
{
    struct avr_relax rx;
    unsigned char *pin = NULL;
    int npin = 0, ok = 0;

    memset(&rx, 0, sizeof rx);
    for (int it = 0; it < 16; it++) {
        int changed = 0;
        avr_rollback(t, st, rb);
        rx.bad = -1;
        gen_func(fn, t, st, keep_vars, mode, &rx);
        if (npin < rx.nsite) {
            pin = xrealloc(pin, (size_t)rx.nsite);
            for (; npin < rx.nsite; npin++) pin[npin] = 0;
        }
        if (rx.bad >= 0) {                   /* thrown away: pin it long */
            pin[rx.bad] = 1;
            rx.hint[rx.bad] = 0;
            ok = 0;
            continue;
        }
        ok = 1;
        for (int k = 0; k < rx.nsite; k++) {
            int h = pin[k] ? 0 : rx.fits[k] > rx.hint[k] ? rx.fits[k]
                                                         : rx.hint[k];
            if (h != rx.hint[k]) { rx.hint[k] = (unsigned char)h; changed = 1; }
        }
        if (!changed)
            break;
    }
    if (!ok) {
        /* Never left with a thrown-away attempt in `t`: everything long. */
        avr_rollback(t, st, rb);
        gen_func(fn, t, st, keep_vars, mode, NULL);
    }
    free(rx.hint); free(rx.fits); free(pin);
    return t->len - rb->at;
}

/* One allocation choice, generated until a_verify catches nothing: each
 * value it caught is kept out of the home it was overwritten in, and the
 * function generated again. Each round excludes at least one more value
 * from at least one more class of home, so this ends; the bound is only
 * there to name a loop that did not. */
static int avr_attempt(struct ir_func *fn, struct code *t, struct a_sites *st,
                       int keep_vars, int mode, const struct avr_rollback *rb)
{
    int nv = fn->nvregs;
    for (int tries = 0; ; tries++) {
        int len, any = 0;
        g_x_hit = g_x_alloc = 0;
        if (nv)
            memset(g_a_pend, 0, (size_t)nv);
        len = gen_relaxed(fn, t, st, keep_vars, mode, rb);
        if (g_a_xhome && g_x_hit && g_x_alloc) {
            g_a_xhome = 0;               /* X was scratch after all */
            continue;
        }
        for (int v = 0; v < nv; v++) {
            if (g_a_pend[v] & 1) { g_a_novol[v] = 1; any = 1; }
            if (g_a_pend[v] & 2) { g_a_nohome[v] = 1; any = 1; }
        }
        if (!any)
            return len;
        if (tries > 2 * nv + 2)
            internal_error("avr: %s: values kept being overwritten in their "
                           "homes after %d attempts", fn->name, tries);
    }
}

/* Each allocation choice in turn, keeping the shortest (avr_regalloc).
 * A discarded attempt is undone by truncating what it appended: the code,
 * and the four site lists, which only ever grow. */
static void gen_func_best(struct ir_func *fn, struct code *t,
                          struct a_sites *st, int keep_vars)
{
    struct avr_rollback rb = { t->len, st->next, st->nstr, st->ng, st->nf };
    int best = AVR_RA_NONE, best_len = 0;
    int nv = fn->nvregs;

    if (!g_a_regalloc || fn->src->is_isr ||
        (avr_knob("EMBCC_AVR_RA_ONLY") &&
         strcmp(avr_knob("EMBCC_AVR_RA_ONLY"), fn->name) != 0)) {
        gen_relaxed(fn, t, st, keep_vars, AVR_RA_NONE, &rb);
        return;
    }
    /* A field's constant offset into its load or store -- `ldd r, Z+q`
     * reaches q = 63 -- once, before any attempt and before allocation,
     * since the base's live range grows. `f->flags` was the pointer into
     * X, subi/sbci, into Z and a load: six instructions for clang's
     * three. EMBCC_NO_MEMOFF turns it off, as on Thumb and RISC-V. */
    if (!avr_knob("EMBCC_NO_MEMOFF") && !g_a_o0) {
        char *w = avr_wide_map(fn);
        ra_fold_memoff(fn, 0, 64, 2, 4, w, 0, 0);
        free(w);
    }
    int m0 = AVR_RA_QUADS_FIRST, m1 = AVR_RA_PAIRS_ONLY;
    /* ...and each mode under a few budgets of pairs (g_a_cap). */
    static const int caps[3] = { 8, 3, 1 };
    int c0 = 0, c1 = 2, best_cap = 8, last_cap = 1, best_x = 0;
    int best_last = 0;
    char *best_novol, *best_nohome;
    if (avr_knob("EMBCC_AVR_RA_MODE"))
        m0 = m1 = atoi(avr_knob("EMBCC_AVR_RA_MODE"));
    if (avr_knob("EMBCC_AVR_RA_CAP")) {
        c0 = c1 = 0;
        last_cap = atoi(avr_knob("EMBCC_AVR_RA_CAP"));
    }
    /* EMBCC_AVR_NO_VOL: no homes in B, which is what the allocator did
     * before it had them -- for telling a miscompile of theirs apart. */
    g_a_vol = !avr_knob("EMBCC_AVR_NO_VOL");
    /* EMBCC_AVR_NO_OCT: every eight-byte value in memory, as before. */
    g_a_oct = !avr_knob("EMBCC_AVR_NO_OCT");
    g_a_nvr = nv;
    g_a_novol = xcalloc((size_t)(nv ? nv : 1), 1);
    g_a_nohome = xcalloc((size_t)(nv ? nv : 1), 1);
    g_a_pend = xcalloc((size_t)(nv ? nv : 1), 1);
    best_novol = xcalloc((size_t)(nv ? nv : 1), 1);
    best_nohome = xcalloc((size_t)(nv ? nv : 1), 1);
    for (int m = m0; m <= m1; m++)
        for (int c = c0; c <= c1; c++) {
            int len;
            g_a_cap = avr_knob("EMBCC_AVR_RA_CAP") ? last_cap : caps[c];
            /* with X offered, unless the attempt then used it as scratch */
            g_a_xhome = !avr_knob("EMBCC_AVR_NO_XHOME");
            /* each attempt learns its own exclusions */
            memset(g_a_novol, 0, (size_t)(nv ? nv : 1));
            memset(g_a_nohome, 0, (size_t)(nv ? nv : 1));
            len = avr_attempt(fn, t, st, keep_vars, m, &rb);
            if (avr_knob("EMBCC_AVR_RA"))
                fprintf(stderr, "%s: mode %d cap %d, %d bytes\n", fn->name,
                        m, g_a_cap, len);
            best_last = 0;
            if (best == AVR_RA_NONE || len < best_len) {
                best = m;
                best_cap = g_a_cap;
                best_x = g_a_xhome;
                best_len = len;
                best_last = 1;
                memcpy(best_novol, g_a_novol, (size_t)(nv ? nv : 1));
                memcpy(best_nohome, g_a_nohome, (size_t)(nv ? nv : 1));
            }
        }
    if (!best_last) {
        /* The best attempt again, exactly: its choices and what its own
         * retries learned, so the code is the code that was measured. */
        g_a_cap = best_cap;
        g_a_xhome = best_x;
        memcpy(g_a_novol, best_novol, (size_t)(nv ? nv : 1));
        memcpy(g_a_nohome, best_nohome, (size_t)(nv ? nv : 1));
        g_x_hit = g_x_alloc = 0;
        if (nv)
            memset(g_a_pend, 0, (size_t)nv);
        gen_relaxed(fn, t, st, keep_vars, best, &rb);
        if (g_x_hit && g_x_alloc)
            internal_error("avr: %s: X is a home and was used as scratch",
                           fn->name);
        for (int v = 0; v < nv; v++)
            if (g_a_pend[v])
                internal_error("avr: %s: vreg %d's home was overwritten in "
                               "the attempt chosen", fn->name, v);
    }
    free(g_a_novol); free(g_a_nohome); free(g_a_pend);
    free(best_novol); free(best_nohome);
    g_a_novol = g_a_nohome = g_a_pend = NULL;
    g_a_nvr = 0;
    g_a_cap = 8;
    g_a_xhome = 0;
    g_a_vol = 1;
    g_a_oct = 1;
}

void codegen_unit_avr(struct ir_unit *iu, struct code *text,
                      struct extcall **ext, int *next,
                      struct strsite **strs, int *nstrs,
                      struct gsite **gs, int *ngs,
                      struct fsite **fs, int *nfs, int keep_vars,
                      int optimize, int no_sse, int regalloc)
{
    struct a_sites st;
    memset(&st, 0, sizeof st);
    (void)no_sse;
    g_a_regalloc = regalloc;           /* -O1 and up, and -O0 */
    g_a_o0 = !optimize;

    for (int n = 0; n < iu->nfuncs; n++) {
        int ra = g_a_regalloc;
        if (g_a_o0 && ra_o0_too_big(&iu->funcs[n]))
            g_a_regalloc = 0;          /* see ra_o0_too_big */
        gen_func_best(&iu->funcs[n], text, &st, keep_vars);
        g_a_regalloc = ra;
    }

    /* The sites still carry string INDICES; the driver's relocations want
     * .rodata offsets. */
    cg_resolve_strsites(iu, st.str, st.nstr);

    *ext = st.ext;   *next = st.next;
    *strs = st.str;  *nstrs = st.nstr;
    *gs = st.g;      *ngs = st.ng;
    *fs = st.f;      *nfs = st.nf;
}
