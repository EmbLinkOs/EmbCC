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

/* A third scratch: r10, alongside T_ACC (r12) and T_TMP (r11). All three
 * are saved in the prologue except r12, which the ABI already makes
 * caller-saved. */
#define T_ADDR 10

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
    struct code *t;
    struct t_sites *st;
    long *slot;          /* per-vreg byte offset from sp, -1 for none */
    long frame;          /* total bytes sp moves down by */
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
static long outgoing_area(const struct ir_func *fn)
{
    long most = 0;
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        long need;
        if (i->op != IR_CALL || i->nargs <= 4)
            continue;
        need = (long)(i->nargs - 4) * 4;
        if (need > most)
            most = need;
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
        off = (off + 3) & ~3L;
        F->slot[v] = off;
        off += 4;
    }
    off += fn->scratch_bytes;
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

/* ---- one instruction ------------------------------------------------ */

static void gen_ins(struct t_fn *F, int n)
{
    struct ir_func *fn = F->fn;
    struct ir_ins *i = &fn->ins[n];
    struct code *t = F->t;

    /* The refusals, checked once and by name. `w` of 8 or 16 is a value
     * wider than a register; `flt` is the missing soft-float lowering. */
    if (i->flt)
        t_refuse(fn, i, "floating point (ARMv7-M has no FPU: this needs the "
                        "__aeabi soft-float calls)");
    if (i->w > 4 && i->op != IR_LDVAR && i->op != IR_STVAR &&
        i->op != IR_LOAD && i->op != IR_STORE)
        t_refuse(fn, i, "a value wider than 32 bits (long long needs the "
                        "register-pair legalisation)");

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
        rd(F, i->a, T_ACC);
        t_cmp_imm(t, T_ACC, 0);
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
        for (int k = 0; k < i->nargs; k++) {
            struct ir_arg *a = &i->argv[k];
            if (a->is_struct)
                t_refuse(fn, i, "an aggregate passed by value");
            if (a->is_float)
                t_refuse(fn, i, "a floating-point argument");
            if (ncrn < 4) {
                rd(F, a->vreg, ncrn);
                ncrn++;
            } else {
                rd(F, a->vreg, T_ACC);
                t_ldst_imm(t, T_ACC, T_SP, stk, 4, 0, 1);
                stk += 4;
            }
        }
        if (i->retsize && i->retnclass == 0)
            t_refuse(fn, i, "a call returning an aggregate in memory");
        if (i->indirect) {
            rd(F, i->a, T_ACC);
            t_blx(t, T_ACC);
        } else if (i->callee->has_defn) {
            note_call(F->st, t_bl(t), i->callee);
        } else {
            note_ext(F->st, t_bl(t), i->callee);
        }
        if (i->dst >= 0)
            wr(F, i->dst, T_R0);
        return;
    }

    case IR_RET:
        if (i->a >= 0)
            rd(F, i->a, T_R0);
        /* Every return leaves through the epilogue at the end of the
         * function, so there is one place that knows the frame size --
         * except the LAST instruction, which the epilogue already
         * follows. */
        if (n + 1 < fn->nins)
            jump_to(F, fn->nlabels);
        return;

    case IR_UD2:
        /* `udf #0`, the permanently undefined instruction. */
        t_mov_imm(t, T_ACC, 0, 0);
        t_ldst_imm(t, T_ACC, T_ACC, 0, 4, 0, 0);
        return;
    case IR_FENCE:
        /* dmb sy — a full data barrier. */
        code_byte(t, 0xbf); code_byte(t, 0xf3);
        code_byte(t, 0x5f); code_byte(t, 0x8f);
        return;

    case IR_I2F: case IR_F2I: case IR_F2F: case IR_SQRT:
        t_refuse(fn, i, "a floating-point conversion");
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
    layout(&F);

    /* One more label than the IR has: the epilogue, which every IR_RET
     * jumps to so the frame size is written down once. */
    F.label_off = xmalloc((size_t)(fn->nlabels + 1) * sizeof *F.label_off);
    for (i = 0; i <= fn->nlabels; i++)
        F.label_off[i] = -1;

    /* A Thumb function must start on a halfword, and four keeps the
     * literal loads and the disassembly tidy. */
    code_align(t, 4, 0xbf);
    f->code_off = t->len;

    push_at = t_push(t, SAVE_MASK);
    if (F.frame)
        t_sp_adjust(t, F.frame, 1);

    /* The parameters arrive in r0-r3 and on the stack above the saved
     * registers; the prologue writes each to its slot, which is what
     * every later reference reads. */
    {
        int ncrn = 0;
        long caller = F.frame + SAVE_BYTES;
        for (i = 0; i < fn->nparams; i++) {
            struct ir_arg *a = &fn->param_abi[i];
            if (a->is_struct || a->is_float)
                t_refuse(fn, NULL, "an aggregate or floating-point parameter");
            if (ncrn < 4) {
                if (!t_ldst_imm(t, ncrn, T_SP, F.slot[i], 4, 0, 1)) {
                    t_add_sp(t, T_ADDR, F.slot[i]);
                    t_ldst_imm(t, ncrn, T_ADDR, 0, 4, 0, 1);
                }
                ncrn++;
            } else {
                t_ldst_imm(t, T_ACC, T_SP, caller, 4, 0, 0);
                caller += 4;
                if (!t_ldst_imm(t, T_ACC, T_SP, F.slot[i], 4, 0, 1)) {
                    t_add_sp(t, T_ADDR, F.slot[i]);
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
    (void)push_at;
    free(F.slot);
    free(F.label_off);
    free(F.fix);
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
