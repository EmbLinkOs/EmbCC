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
#define SAVE_BYTES 16

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
    int *label_off;      /* per label id, or -1 while unseen */
    struct { int at; int label; int cond; } *fix;
    int nfix, capfix;
};

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
        case IR_I2F: case IR_F2I: case IR_F2F:
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
            if (i->op == IR_MOV)
                src = i->a >= 0 && i->a < fn->nvregs && w[i->a];
            else if (i->op == IR_SELECT)
                src = (i->b >= 0 && i->b < fn->nvregs && w[i->b]) ||
                      (i->c >= 0 && i->c < fn->nvregs && w[i->c]);
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
        int size = fn->locals[v].size ? fn->locals[v].size : 4;
        int align = fn->locals[v].user_align ? fn->locals[v].user_align
                  : fn->locals[v].align ? fn->locals[v].align : 4;
        if (align < 4) align = 4;
        off = (off + align - 1) & ~(long)(align - 1);
        F->slot[v] = off;
        off += size;
    }
    for (int v = fn->nvars; v < fn->nvregs; v++) {
        /* Eight-byte values are eight-ALIGNED as well as eight wide:
         * AAPCS32 aligns `long long` to 8, and a pair straddling that
         * boundary would be legal but slower and would break `ldrd` if
         * this ever emits one. */
        int size = F->wide[v] ? 8 : 4;
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
     * every public interface, and the push above already moved it by a
     * multiple of eight. */
    F->frame = (off + 7) & ~7L;
}

/* ---- reading and writing a vreg ------------------------------------- */

/* Load vreg v into `reg`. Every value lives in memory in this backend,
 * so this is always a load — which is the naive part, and the part a
 * register allocator replaces. */
static void rd(struct t_fn *F, int v, int reg)
{
    if (!t_ldst_imm(F->t, reg, T_SP, F->slot[v], 4, 0, 0)) {
        t_mov_imm(F->t, reg, F->slot[v], 0);
        t_ldst_reg(F->t, reg, T_SP, reg, 0, 4, 0, 0);
    }
}

static void wr(struct t_fn *F, int v, int reg)
{
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

/* ---- one instruction ------------------------------------------------ */

static void gen_ins(struct t_fn *F, int n)
{
    struct ir_func *fn = F->fn;
    struct ir_ins *i = &fn->ins[n];
    struct code *t = F->t;

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
        if (name) {
            int n = fp_arg(F, i->a, i->w, T_R0);
            if (i->imm_b)
                t_refuse(fn, i, "a folded floating-point immediate");
            fp_arg(F, i->b, i->w, n);
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
            int n = fp_arg(F, i->a, i->w, T_R0);
            int cond;
            fp_arg(F, i->b, i->w, n);
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
            wide = (i->dst >= 0 && F->wide[i->dst]) ||
                   (i->op == IR_MOV && i->a >= 0 && F->wide[i->a]);
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
    case IR_CONST:
        t_mov_imm(t, T_ACC, (long)i->imm, 0);
        wr(F, i->dst, T_ACC);
        return;
    case IR_MOV:
        rd(F, i->a, T_ACC);
        wr(F, i->dst, T_ACC);
        return;

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
        rd(F, i->a, T_ACC);
        if (i->imm_b && i->op != IR_MUL) {
            /* addw/subw reach any 0..4095 where the modified immediate
             * reaches only what it can rotate into place, and almost
             * every constant folded here is a small offset. */
            if ((i->op == IR_ADD || i->op == IR_SUB) &&
                i->imm >= 0 && i->imm <= 4095) {
                if (i->op == IR_ADD) t_addw(t, T_ACC, T_ACC, i->imm);
                else                 t_subw(t, T_ACC, T_ACC, i->imm);
            } else if (!t_alu_imm(t, op, T_ACC, T_ACC, i->imm, 0)) {
                operand_b(F, i, T_TMP);
                t_alu_reg(t, op, T_ACC, T_ACC, T_TMP, 0);
            }
            wr(F, i->dst, T_ACC);
            return;
        }
        operand_b(F, i, T_TMP);
        if (i->op == IR_MUL)
            t_mul(t, T_ACC, T_ACC, T_TMP);
        else
            t_alu_reg(t, op, T_ACC, T_ACC, T_TMP, 0);
        wr(F, i->dst, T_ACC);
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
        rd(F, i->a, T_ACC);
        if (i->imm_b && i->imm >= 0 && i->imm < 32) {
            t_shift_imm(t, sh, T_ACC, T_ACC, (int)i->imm, 0);
        } else {
            operand_b(F, i, T_TMP);
            t_shift_reg(t, sh, T_ACC, T_ACC, T_TMP, 0);
        }
        wr(F, i->dst, T_ACC);
        return;
    }
    case IR_NEG:
        rd(F, i->a, T_ACC);
        t_alu_imm(t, T_OP_RSB, T_ACC, T_ACC, 0, 0);
        wr(F, i->dst, T_ACC);
        return;
    case IR_BNOT:
        rd(F, i->a, T_ACC);
        t_mvn_reg(t, T_ACC, T_ACC, 0);
        wr(F, i->dst, T_ACC);
        return;

    case IR_CMP: {
        int cond = cond_for(i->pred, i->sign);
        if (i->w == 8) {
            cond = cmp64(F, i, i->pred, i->sign);
            t_mov_imm(t, T_ACC, 1, 0);
            {
                int over = t_bcond(t, cond);
                t_mov_imm(t, T_ACC, 0, 0);
                t_patch_bcond(t, over, t->len);
            }
            wr(F, i->dst, T_ACC);
            return;
        }
        rd(F, i->a, T_ACC);
        if (i->imm_b && ((i->imm >= 0 && i->imm <= 255) || t_imm_ok(i->imm))) {
            t_cmp_imm(t, T_ACC, i->imm);
        } else {
            operand_b(F, i, T_TMP);
            t_cmp_reg(t, T_ACC, T_TMP);
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

    case IR_LDVAR:
        if (i->w > 4) t_refuse(fn, i, "a 64-bit local");
        if (!t_ldst_imm(t, T_ACC, T_SP, F->slot[i->a], i->size, i->sign, 0)) {
            t_add_sp(t, T_ADDR, F->slot[i->a]);
            t_ldst_imm(t, T_ACC, T_ADDR, 0, i->size, i->sign, 0);
        }
        wr(F, i->dst, T_ACC);
        return;
    case IR_STVAR:
        if (i->w > 4) t_refuse(fn, i, "a 64-bit local");
        rd(F, i->a, T_ACC);
        if (!t_ldst_imm(t, T_ACC, T_SP, F->slot[i->dst], i->size, 0, 1)) {
            t_add_sp(t, T_ADDR, F->slot[i->dst]);
            t_ldst_imm(t, T_ACC, T_ADDR, 0, i->size, 0, 1);
        }
        return;
    case IR_LOAD:
        if (i->w > 4) t_refuse(fn, i, "a 64-bit load");
        rd(F, i->a, T_ADDR);
        t_ldst_imm(t, T_ACC, T_ADDR, 0, i->size, i->sign, 0);
        wr(F, i->dst, T_ACC);
        return;
    case IR_STORE:
        if (i->w > 4) t_refuse(fn, i, "a 64-bit store");
        rd(F, i->a, T_ADDR);
        rd(F, i->b, T_ACC);
        t_ldst_imm(t, T_ACC, T_ADDR, 0, i->size, 0, 1);
        return;
    case IR_EXT:
        rd(F, i->a, T_ACC);
        if (i->size < 4)
            t_ext(t, T_ACC, T_ACC, i->size, i->sign);
        wr(F, i->dst, T_ACC);
        return;

    case IR_ADDR:
        addr_of_slot(F, i->a, T_ACC);
        wr(F, i->dst, T_ACC);
        return;
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
        for (int k = 0; k < i->nargs; k++) {
            struct ir_arg *a = &i->argv[k];
            if (!pl[k].nreg)
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
         * irgen converts an `unsigned int` by asking for a SIGNED
         * 64-bit conversion of it, on the grounds that "a 32-bit
         * operation zero-extends its result into the eight-byte slot".
         * That is true of a register write on both other targets and
         * false of a four-byte stack slot here, where the next four
         * bytes are another temporary — so the zero extension is done
         * explicitly, which is what that comment meant all along. */
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
    case IR_ASM:
        t_refuse(fn, i, "inline assembly");
        return;
    case IR_VA_START:
        t_refuse(fn, i, "a variadic function");
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

static void gen_func(struct ir_func *fn, struct code *t, struct t_sites *st)
{
    struct func *f = fn->src;
    struct t_fn F;
    int push_at, i;

    if (fn->is_varargs)
        t_refuse(fn, NULL, "a variadic function");

    F.fn = fn; F.t = t; F.st = st;
    F.fix = NULL; F.nfix = F.capfix = 0;
    F.wide = wide64_map(fn);
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
    f->code_off = t->len;

    push_at = t_push(t, SAVE_MASK);
    if (F.frame)
        t_sp_adjust(t, F.frame, 1);

    /* The parameters arrive in r0-r3 and on the stack above the saved
     * registers; the prologue writes each to its slot, which is what
     * every later reference reads. */
    {
        struct argplace pl;
        int ncrn = 0;
        long stk = 0;
        long base = F.frame + SAVE_BYTES;   /* the caller's outgoing area */
        if (F.sret_slot >= 0) {
            t_ldst_imm(t, T_R0, T_SP, F.sret_slot, 4, 0, 1);
            ncrn = 1;
        }
        for (i = 0; i < fn->nparams; i++) {
            struct ir_arg *a = &fn->param_abi[i];
            place_arg(a->size, arg_align(a), &ncrn, &stk, &pl);
            /* Every parameter lands in its own local's slot, which is
             * where the body reads it. A composite's slot IS the
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
    }

    for (i = 0; i < fn->nins; i++)
        gen_ins(&F, i);

    /* The epilogue. */
    F.label_off[fn->nlabels] = t->len;
    if (F.frame)
        t_sp_adjust(t, F.frame, 0);
    t_pop(t, (SAVE_MASK & ~(1u << T_LR)) | (1u << T_PC));

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
    f->stack_bytes = (int)(F.frame + SAVE_BYTES);
    (void)push_at;
    free(F.slot);
    free(F.label_off);
    free(F.fix);
    free(F.wide);
}

void codegen_unit_thumb(struct ir_unit *iu, struct code *text,
                        struct extcall **ext, int *next,
                        struct strsite **strs, int *nstrs,
                        struct gsite **gs, int *ngs,
                        struct fsite **fs, int *nfs, int want_debug,
                        int optimize, int no_sse, int regalloc)
{
    (void)optimize; (void)no_sse; (void)regalloc;
    if (want_debug) {
        fprintf(stderr, "embcc: error: -g is not supported for ARMv7-M yet "
                        "(the DWARF frame description would be a guess)\n");
        exit(1);
    }

    struct t_sites st;
    st.call = NULL; st.ncall = st.capcall = 0;
    st.ext = NULL;  st.next = st.capext = 0;
    st.str = NULL;  st.nstr = st.capstr = 0;
    st.g = NULL;    st.ng = st.capg = 0;
    st.f = NULL;    st.nf = st.capf = 0;

    for (int n = 0; n < iu->nfuncs; n++)
        gen_func(&iu->funcs[n], text, &st);

    for (int n = 0; n < st.ncall; n++)
        t_patch_bl(text, st.call[n].patch_off, st.call[n].target->code_off);
    free(st.call);

    for (int n = 0; n < st.nstr; n++)
        st.str[n].str_off = iu->strs[st.str[n].str_off].off;

    *ext = st.ext;   *next = st.next;
    *strs = st.str;  *nstrs = st.nstr;
    *gs = st.g;      *ngs = st.ng;
    *fs = st.f;      *nfs = st.nf;
}
