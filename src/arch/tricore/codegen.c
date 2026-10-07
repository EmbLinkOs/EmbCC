/* TriCore 1.6.1 code generation, little-endian, soft float
 * (docs/internals/tricore-plan.md).
 *
 * The shape is the MIPS backend's (src/arch/mips/codegen.c), itself the
 * RV32 one's: every vreg has one home -- a register the shared allocator
 * gave it, or a frame slot -- and every operation reads its operands
 * through rdr/rd and writes its result through wreg/wrote, so the code is
 * correct with the allocator off and smaller with it on. A 64-bit value
 * lives in a register pair (the allocator's pair pass) or in an
 * eight-byte slot. What TriCore changes, and where:
 *
 *   * TWO REGISTER FILES. Every vreg's home is a DATA register (D) or a
 *     slot; the ADDRESS registers (A) are scratch, filled just before a
 *     load or store (mov.a, or ld.a straight from the slot) and for the
 *     pointer arguments and results of a call, which the TriCore EABI
 *     passes in A4-A7 and A2. Which values are pointers there is decided
 *     by the C types irgen still holds (place_args).
 *   * THE HARDWARE SAVES REGISTERS. CALL stores the upper context --
 *     D8-D15, A10-A15 -- and RET restores it, the stack pointer included.
 *     So nothing is saved or restored in a prologue or epilogue: the
 *     prologue moves A10 down and the epilogue is RET.
 *   * Branches compare two registers, or one with a 4-bit constant, and
 *     reach +-32 KiB; one that does not reach becomes the inverse branch
 *     over a J (+-16 MiB), and the function is generated again until
 *     nothing new fails.
 *   * SH and SHA shift left by a positive count and right by a negative
 *     one. Comparisons produce 0 or 1 directly. DIV gives quotient and
 *     remainder at once in a register pair, and the 64-bit add and
 *     subtract go through the PSW carry.
 *   * A word access to an address that is not halfword-aligned traps, so
 *     a load or store irgen cannot promise is aligned (a packed member)
 *     goes byte by byte.
 *
 * Refused by name: atomics wider than a word, computed
 * goto, jump tables
 * (target_jump_tables keeps a dense switch a decision tree), the frame
 * and return address, __int128 and binary128. THE RULE.
 */
#include "emit.h"

#include "../backend.h"
#include "../regalloc.h"
#include "../target.h"
#include "../../driver/util.h"
#include "../../sema/type.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- registers -------------------------------------------------------------
 *
 * Data scratches: D0:D1 (E0) and D2:D3 (E2) are the A and B operand
 * pairs of a 64-bit operation and ACC/TMP of a 32-bit one -- D2:D3 is
 * also where a result comes back -- and D15 is the one spare. None is in
 * the allocator's pool. Address scratches: A12 the address of an access,
 * A13 a second address (a copy's source, a far frame offset), A14 the
 * frame base under alloca, A15 an indirect call's target. A12-A15 are in
 * the upper context, so using them costs nothing. A2 and A3 are left
 * alone except as A2, the pointer result. */
#define A_LO 0
#define A_HI 1
#define B_LO 2
#define B_HI 3
#define ACC  0            /* the value being computed */
#define TMP  1            /* the second operand */
#define SCR  15
#define AD   12
#define AD2  13
#define AFB  14
#define ACALL 15

/* In a parallel move, and nowhere else, an address register n is 16 + n,
 * so one move can carry both files (mvx). */
#define AREG(n) (16 + (n))

struct tc_sites {
    struct extcall *ext;   int next, capext;
    struct strsite *str;   int nstr, capstr;
    struct gsite *g;       int ng, capg;
    struct fsite *f;       int nf, capf;
};

struct tc_fn {
    int *usecnt;         /* per vreg: how many reads (fusion), or NULL */
    int skip_next;       /* the instruction after this one is already out */
    int want_debug;
    struct ir_func *fn;
    int *loc;            /* per vreg: its D register, -1 in memory; NULL at -O0 */
    int used_callee[RA_MAXPOOL];
    int pair_used[8], npair;
    int nsave;
    struct code *t;
    struct tc_sites *st;
    char *wide;          /* per vreg: a 64-bit value, a register pair */
    char *nshr;          /* per vreg: a narrow high-word shift (narrow_shr) */
    long *slot;          /* per vreg: byte offset from the frame base, -1 */
    long frame;          /* bytes A10 moves down by */
    long scratch_at;     /* where fn->scratch_bytes begins */
    long copy_at;        /* where a call's by-reference argument copies go */
    long sret_slot;      /* where the hidden result pointer is kept, or -1 */
    int fb;              /* the frame base, an A register: A10, or A14 */
    long out_bytes;      /* the outgoing stack-argument area */
    char *tail;          /* per instruction: a tail call, or NULL */
    long va_first;       /* a variadic function's first unnamed word, or -1 */
    int *label_off;      /* per label id, or -1 while unseen */
    /* A branch or jump to a label: FX_B a conditional branch, FX_J a J,
     * both patched when the function ends. */
    struct { int at; int label; int kind; } *fix;
    int nfix, capfix;
    /* Per branch, in emission order: take the long form. NULL on the
     * first attempt, which tries every one short (gen_func). */
    const char *longb;
    int nlongb;
};

/* ---- the allocator's view of this machine ---------------------------------
 *
 * The argument registers D4-D7 first, then D8-D14 -- which the hardware
 * saves at every call, so they survive one and cost nothing to use. D15
 * and D0-D3 are scratch. */
#define TC_NPOOL 11
static const int TC_POOL[TC_NPOOL] = {
    4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14
};

static unsigned long g_tc_taken;        /* registers the pair pass took */
static int g_tc_pairs = 1;              /* this attempt uses the pair pass */
static int g_tc_pool[TC_NPOOL];

static const int *tc_pool_for(const struct ir_func *fn, int *n)
{
    int k = 0;
    (void)fn;
    if (!g_tc_taken) {
        *n = TC_NPOOL;
        return TC_POOL;
    }
    for (int j = 0; j < TC_NPOOL; j++)
        if (!(g_tc_taken >> TC_POOL[j] & 1))
            g_tc_pool[k++] = TC_POOL[j];
    *n = k;
    return g_tc_pool;
}

/* The PAIR pool, each pair named by its low (even) register: E4 and E6,
 * where 64-bit arguments travel, then E8-E12 for values that live across
 * a call. E14 holds D15, a scratch. */
#define TC_NPAIRS 5
static const int TC_PAIRS[TC_NPAIRS] = { 4, 6, 8, 10, 12 };
static const int *tc_pair_pool_for(const struct ir_func *fn, int *n)
{
    (void)fn;
    *n = TC_NPAIRS;
    return TC_PAIRS;
}

static void tc_pair_hints(const struct ir_func *fn, int *hint);

/* D8-D15: the upper context, which CALL saves and RET restores. */
static int tc_callee_saved(int r)
{
    return r >= 8 && r <= 15;
}

/* `dst = load(local)` is a plain move at the full register width. */
static int tc_ldvar_plain(int size, int sign, int w)
{
    (void)sign;
    return size == 4 && w == 4;
}

/* Which instructions become a CALL the IR does not show as one: every
 * floating-point operation (soft float), and a 64-bit divide. */
int tc_op_calls_helper(const struct ir_ins *i)
{
    if (i->flt)
        return i->op == IR_ADD || i->op == IR_SUB || i->op == IR_MUL ||
               i->op == IR_DIV || i->op == IR_CMP;
    if (i->op == IR_I2F || i->op == IR_F2I || i->op == IR_F2F)
        return 1;
    return (i->op == IR_DIV || i->op == IR_MOD) && i->w == 8;
}

static void tc_abi_hints(const struct ir_func *fn, int *hint);

static const struct ra_target TC_RATGT = {
    tc_pool_for,
    tc_callee_saved,
    tc_ldvar_plain,
    1, 1, 1,        /* call args, returns and memcpy addresses from registers:
                     * the call setup is one parallel move (gen_call) */
    tc_op_calls_helper,
    0,              /* three-operand */
    tc_abi_hints,
    NULL, NULL,     /* no FP class: soft float in the data registers */
    1,              /* ...allocated with them (float_in_gpr) */
    NULL, NULL,
    0,              /* atomic_in_reg: the atomics read their operands
                     * through rd/rda, but are left in memory for now */
    0,
    0               /* asm_in_reg: inline asm is refused */
};

static int g_tc_regalloc;

/* ---- refusal ---------------------------------------------------------------- */

static void tc_refuse(const struct tc_fn *F, const struct ir_ins *i,
                      const char *what)
{
    char op[64];
    op[0] = 0;
    if (i)
        snprintf(op, sizeof op, " [%s w=%d size=%d]", ir_opname(i->op),
                 i->w, i->size);
    fprintf(stderr,
            "embcc: %s:%d: error: the TriCore backend cannot lower %s yet "
            "(function %s)%s\n",
            F->fn->file ? F->fn->file : "?",
            i ? i->line : F->fn->line, what, F->fn->name, op);
    exit(1);
}

/* ---- which values are eight bytes wide ----------------------------------
 *
 * RV32's rule, for the same reasons (riscv/codegen.c wide_map): by the
 * width of the RESULT, a local by its declared size, and through copies
 * that do not say four bytes, to a fixed point. */
static char *wide_map(struct ir_func *fn)
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
        case IR_I2F: case IR_F2I: case IR_F2F: case IR_BITCAST:
            w[i->dst] = 1;
            break;
        default:
            break;
        }
    }
    for (int v = 0; v < fn->nvars && v < fn->nvregs; v++)
        if (fn->locals[v].size == 8 &&
            (fn->locals[v].is_int_or_ptr || fn->locals[v].is_scalar_float))
            w[v] = 1;
    for (int again = 1; again;) {
        again = 0;
        for (int n = 0; n < fn->nins; n++) {
            const struct ir_ins *i = &fn->ins[n];
            int src;
            if (i->dst < 0 || i->dst >= fn->nvregs || w[i->dst])
                continue;
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

/* ---- the TriCore EABI calling convention --------------------------------
 *
 * As remembered and unverified (docs/internals/tricore-plan.md):
 *
 *   * A pointer goes in the next free of A4-A7; any other scalar of 32
 *     bits or fewer in the lowest free of D4-D7; a 64-bit one in the
 *     first free even pair, E4 or E6 -- a register skipped on the way is
 *     filled by a later 32-bit argument.
 *   * A struct or union of 8 bytes or fewer travels as an integer of its
 *     size (D, or an E pair); a larger one BY REFERENCE, the caller
 *     passing the address of its own copy as a pointer argument.
 *   * What finds no register goes on the stack, in whole words from the
 *     caller's SP, 4-aligned whatever the size. Every unnamed argument of
 *     a variadic call goes there.
 *   * A pointer result is in A2; any other of 32 bits or fewer in D2, of
 *     64 bits (or a struct of 5-8 bytes) in E2; a larger struct comes
 *     back through a hidden pointer the caller passes in A4.
 */
enum { PL_NONE, PL_D, PL_E, PL_A, PL_STK };
struct argplace {
    int kind;
    int reg;             /* PL_D, PL_E (the even register), PL_A */
    long stk;            /* PL_STK: offset from the caller's SP */
    int words;           /* PL_STK: how many words */
    int byref;           /* a struct passed as the address of a copy */
};

static int ty_is_ptr(const struct type *t)
{
    return t && (t->kind == TY_PTR || t->kind == TY_ARRAY ||
                 t->kind == TY_FUNC);
}

static int arg_byref(const struct ir_arg *a)
{
    return a->is_struct && a->size > 8;
}

static int arg_is_ptr(const struct ir_arg *a)
{
    return arg_byref(a) || (!a->is_struct && ty_is_ptr(a->ty));
}

/* A struct result of more than 8 bytes comes back through a hidden
 * pointer. */
static int fn_sret(const struct ir_func *fn)
{
    return fn->ret_abi.is_struct && fn->ret_abi.size > 8;
}

static int call_sret(const struct ir_ins *i)
{
    return i->retsize > 8;
}

/* Place n arguments. `nfixed` is how many are named when the call (or
 * the function) is variadic, else -1; `sret` reserves A4. Returns the
 * stack bytes the arguments take. */
static long place_args(const struct ir_arg *av, int n, int sret, int nfixed,
                       struct argplace *pl)
{
    unsigned dmask = 0, amask = sret ? 1u : 0u;
    long stk = 0;
    for (int k = 0; k < n; k++) {
        const struct ir_arg *a = &av[k];
        struct argplace *p = &pl[k];
        int size = a->size > 0 ? a->size : 4;
        memset(p, 0, sizeof *p);
        p->byref = arg_byref(a);
        if (p->byref)
            size = 4;
        if (nfixed < 0 || k < nfixed) {
            if (arg_is_ptr(a)) {
                for (int r = 0; r < 4; r++)
                    if (!(amask >> r & 1)) {
                        amask |= 1u << r;
                        p->kind = PL_A;
                        p->reg = 4 + r;
                        break;
                    }
                if (p->kind == PL_A)
                    continue;
            } else if (size > 4) {
                for (int r = 0; r < 4; r += 2)
                    if (!(dmask >> r & 3)) {
                        dmask |= 3u << r;
                        p->kind = PL_E;
                        p->reg = 4 + r;
                        break;
                    }
                if (p->kind == PL_E)
                    continue;
            } else {
                for (int r = 0; r < 4; r++)
                    if (!(dmask >> r & 1)) {
                        dmask |= 1u << r;
                        p->kind = PL_D;
                        p->reg = 4 + r;
                        break;
                    }
                if (p->kind == PL_D)
                    continue;
            }
        }
        p->kind = PL_STK;
        p->stk = stk;
        p->words = (size + 3) / 4;
        stk += 4L * p->words;
    }
    return stk;
}

static int call_nfixed(const struct ir_ins *i)
{
    return i->call_varargs ? i->call_nfixed : -1;
}

/* Where the ABI would put each value: a parameter in the register it
 * arrives in, a call's arguments in theirs, a result in D2, a soft-float
 * helper's operands in D4 and D5. Hints only; the parallel moves at the
 * prologue and each call are what is correct regardless. */
static void tc_abi_hints(const struct ir_func *fn, int *hint)
{
    struct argplace pl[MAX_PARAMS];
    if (fn->src && fn->nparams) {
        place_args(fn->param_abi, fn->nparams, fn_sret(fn),
                   fn->is_varargs ? fn->nparams : -1, pl);
        for (int p = 0; p < fn->nparams && p < fn->nvregs; p++)
            if (pl[p].kind == PL_D && !fn->param_abi[p].is_struct)
                hint[p] = pl[p].reg;
    }
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        if (i->op == IR_RET && i->a >= 0 && i->a < fn->nvregs &&
            !fn->ret_abi.is_struct && fn->ret_abi.size <= 4 &&
            !ty_is_ptr(fn->ret_abi.ty))
            hint[i->a] = 2;
        if (i->op != IR_CALL && tc_op_calls_helper(i) && i->w <= 4) {
            if (i->a >= 0 && i->a < fn->nvregs && hint[i->a] < 0)
                hint[i->a] = 4;
            if (!i->imm_b && i->b >= 0 && i->b < fn->nvregs &&
                hint[i->b] < 0)
                hint[i->b] = 5;
        }
        if (i->op != IR_CALL)
            continue;
        place_args(i->argv, i->nargs, call_sret(i), call_nfixed(i), pl);
        for (int k = 0; k < i->nargs; k++) {
            const struct ir_arg *a = &i->argv[k];
            if (pl[k].kind == PL_D && !a->is_struct && a->vreg >= 0 &&
                a->vreg < fn->nvregs)
                hint[a->vreg] = pl[k].reg;
        }
    }
}

/* ---- the frame -------------------------------------------------------------
 *
 * From A10 upward: the outgoing stack arguments, the shared temp slots,
 * 64-bit temps without a pair, locals (small ones first), the
 * struct-return scratch, the by-reference argument copies, the sret
 * pointer. A multiple of 8. The caller's stack arguments are above, at
 * the frame base + frame. Nothing is saved: the hardware does it. */
#define STACK_ALIGN 8

static long outgoing_area(const struct tc_fn *F, long *copies)
{
    const struct ir_func *fn = F->fn;
    long most = 0;
    *copies = 0;
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        struct argplace pl[MAX_PARAMS];
        long blk, cp = 0;
        if (i->op != IR_CALL)
            continue;
        blk = place_args(i->argv, i->nargs, call_sret(i), call_nfixed(i), pl);
        if (blk > most)
            most = blk;
        for (int k = 0; k < i->nargs; k++)
            if (pl[k].byref)
                cp += (i->argv[k].size + 7) & ~7L;
        if (cp > *copies)
            *copies = cp;
    }
    return (most + STACK_ALIGN - 1) & ~(long)(STACK_ALIGN - 1);
}

static int in_reg(const struct tc_fn *F, int v);

static void layout(struct tc_fn *F)
{
    struct ir_func *fn = F->fn;
    long copies;
    long off = outgoing_area(F, &copies);
    if (fn->has_alloca)
        off = (off + 15) & ~15L;     /* IR_ALLOCA's blocks sit above it */
    F->out_bytes = off;

    F->slot = xmalloc((size_t)(fn->nvregs ? fn->nvregs : 1) * sizeof *F->slot);
    for (int v = 0; v < fn->nvregs; v++)
        F->slot[v] = -1;

    {
        int nv = fn->nvregs, npool = 0, has_cgoto = 0;
        int *loc2 = xmalloc((size_t)(nv ? nv : 1) * sizeof *loc2);
        for (int v = 0; v < nv; v++)
            loc2[v] = in_reg(F, v) || F->wide[v] ? 0 : -1;
        for (int n = 0; n < fn->nins; n++)
            if (fn->ins[n].op == IR_IGOTO || fn->ins[n].op == IR_LABELADDR)
                has_cgoto = 1;
        {
            struct ra_slots so = { loc2, NULL, g_tc_regalloc, has_cgoto };
            int *tslot = ra_coalesce_temps(fn, fn->nvars, &so, &npool);
            for (int v = fn->nvars; v < nv; v++) {
                int k = v - fn->nvars;
                if (loc2[v] >= 0 || !tslot || tslot[k] < 0)
                    continue;
                F->slot[v] = off + (long)tslot[k] * 4;
            }
            off += (long)npool * 4;
            free(tslot);
        }
        for (int v = fn->nvars; v < nv; v++) {
            if (!F->wide[v] || in_reg(F, v))
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
                int size = fn->locals[v].size ? fn->locals[v].size : 4;
                int align = fn->locals[v].user_align ? fn->locals[v].user_align
                          : fn->locals[v].align ? fn->locals[v].align : 4;
                if (in_reg(F, v) || !lref[v] ||
                    ra_slot_dead(fn, F->loc, NULL, v, F->want_debug))
                    continue;
                if ((size > 8) != pass)
                    continue;
                if (align < 4) align = 4;
                if (size == 8 && align < 8) align = 8;
                off = (off + align - 1) & ~(long)(align - 1);
                F->slot[v] = off;
                off += size;
            }
        free(lref);
    }
    F->scratch_at = (off + 7) & ~7L;
    off = F->scratch_at + fn->scratch_bytes;
    F->copy_at = (off + 7) & ~7L;
    off = F->copy_at + copies;

    F->sret_slot = -1;
    if (fn_sret(fn)) {
        off = (off + 3) & ~3L;
        F->sret_slot = off;
        off += 4;
    }
    F->frame = (off + STACK_ALIGN - 1) & ~(long)(STACK_ALIGN - 1);
    F->va_first = -1;
}

/* ---- reading and writing a vreg ----------------------------------------- */

static int fits16(long off) { return off >= -32768 && off <= 32767; }

/* The address register and offset that reach base + off: base itself
 * when off fits a 16-bit field, else AD2 = base + the high part. */
static int far_base(struct tc_fn *F, int base, long *off)
{
    long o = *off;
    if (fits16(o))
        return base;
    tc_addih_a(F->t, AD2, base, tc_hi_adj((unsigned long)o));
    *off = (long)(short)(unsigned short)((unsigned long)o & 0xffff);
    return AD2;
}

static void ld_sp(struct tc_fn *F, int reg, long off, int size, int sign)
{
    int b = far_base(F, F->fb, &off);
    tc_load(F->t, reg, b, off, size, sign);
}

static void st_sp(struct tc_fn *F, int reg, long off, int size)
{
    int b = far_base(F, F->fb, &off);
    tc_store(F->t, reg, b, off, size);
}

/* An address register from a frame slot. */
static void lda_sp(struct tc_fn *F, int areg, long off)
{
    int b = far_base(F, F->fb, &off);
    tc_ld_a(F->t, areg, b, off);
}

static void sta_sp(struct tc_fn *F, int areg, long off)
{
    int b = far_base(F, F->fb, &off);
    tc_st_a(F->t, areg, b, off);
}

/* A store into the OUTGOING area, at the live A10: the callee finds its
 * stack arguments at its own entry A10, which after a VLA is not the
 * frame base. */
static void st_out(struct tc_fn *F, int reg, long off, int size)
{
    int b = far_base(F, TC_SP, &off);
    tc_store(F->t, reg, b, off, size);
}

static void sta_out(struct tc_fn *F, int areg, long off)
{
    int b = far_base(F, TC_SP, &off);
    tc_st_a(F->t, areg, b, off);
}

/* base + off into the address register `areg`. */
static void lea_off(struct tc_fn *F, int areg, int base, long off)
{
    if (fits16(off)) {
        if (off || areg != base)
            tc_lea(F->t, areg, base, off);
        return;
    }
    tc_addih_a(F->t, areg, base, tc_hi_adj((unsigned long)off));
    if ((short)(unsigned short)((unsigned long)off & 0xffff))
        tc_lea(F->t, areg, areg,
               (long)(short)(unsigned short)((unsigned long)off & 0xffff));
}

/* frame base + off, into the DATA register `reg`. */
static void addr_sp(struct tc_fn *F, int reg, long off)
{
    lea_off(F, AD2, F->fb, off);
    tc_mov_d(F->t, reg, AD2);
}

static int in_reg(const struct tc_fn *F, int v)
{
    return F->loc && v >= 0 && F->loc[v] >= 0;
}

/* A value's slot, for code that addresses it directly. One with no slot
 * -- in a register, or never stored -- reaching such a path would read
 * memory nothing wrote, so it is an internal error instead. */
static long sslot(const struct tc_fn *F, int v)
{
    if (v < 0 || v >= F->fn->nvregs || F->slot[v] < 0)
        internal_error("tricore: %s: a path addresses vreg %d's slot, and "
                       "it has none", F->fn->name, v);
    return F->slot[v];
}

/* rd: v into exactly `reg`. rdr: where v IS (its register, or `scratch`
 * after a load). wreg: where to compute v. wrote: commit it if that was
 * a scratch. wr: v from `reg`. */
static void rd(struct tc_fn *F, int v, int reg)
{
    if (in_reg(F, v)) {
        if (F->loc[v] != reg)
            tc_mov(F->t, reg, F->loc[v]);
        return;
    }
    ld_sp(F, reg, sslot(F, v), 4, 1);
}

static int rdr(struct tc_fn *F, int v, int scratch)
{
    if (in_reg(F, v))
        return F->loc[v];
    ld_sp(F, scratch, sslot(F, v), 4, 1);
    return scratch;
}

static int wreg(struct tc_fn *F, int v, int scratch)
{
    return in_reg(F, v) ? F->loc[v] : scratch;
}

static void wrote(struct tc_fn *F, int v, int reg)
{
    if (in_reg(F, v)) {
        if (F->loc[v] != reg)
            tc_mov(F->t, F->loc[v], reg);
        return;
    }
    if (v < 0 || F->slot[v] < 0)
        return;
    st_sp(F, reg, sslot(F, v), 4);
}

static void wr(struct tc_fn *F, int v, int reg)
{
    wrote(F, v, reg);
}

/* The value of v -- an address -- into the address register `areg`: a
 * mov.a from its register, or ld.a straight from its slot. */
static void rda(struct tc_fn *F, int v, int areg)
{
    if (in_reg(F, v)) {
        tc_mov_a(F->t, areg, F->loc[v]);
        return;
    }
    lda_sp(F, areg, sslot(F, v));
}

/* One move of the unified namespace (AREG): D to D, A to D, D to A. */
static void mvx(struct tc_fn *F, int dst, int src)
{
    if (dst == src)
        return;
    if (dst < 16 && src < 16)
        tc_mov(F->t, dst, src);
    else if (dst < 16)
        tc_mov_d(F->t, dst, src - 16);
    else if (src < 16)
        tc_mov_a(F->t, dst - 16, src);
    else
        tc_mov_aa(F->t, dst - 16, src - 16);
}

/* dl <- sl and dh <- sh as one parallel move. */
static void mv2(struct tc_fn *F, int dl, int sl, int dh, int sh)
{
    if (dl == sh && dh == sl) {
        if (dl == sl) return;
        tc_mov(F->t, SCR, sl);
        tc_mov(F->t, dh, sh);
        tc_mov(F->t, dl, SCR);
        return;
    }
    if (dl == sh) {                    /* dh first, before sh is lost */
        if (dh != sh) tc_mov(F->t, dh, sh);
        if (dl != sl) tc_mov(F->t, dl, sl);
        return;
    }
    if (dl != sl) tc_mov(F->t, dl, sl);
    if (dh != sh) tc_mov(F->t, dh, sh);
}

/* A 64-bit value: its pair (low word in loc, high in loc + 1), or its
 * eight-byte slot, low word first. */
static void rd64(struct tc_fn *F, int v, int lo, int hi)
{
    if (in_reg(F, v)) {
        mv2(F, lo, F->loc[v], hi, F->loc[v] + 1);
        return;
    }
    ld_sp(F, lo, sslot(F, v), 4, 1);
    ld_sp(F, hi, sslot(F, v) + 4, 4, 1);
}

static void wr64(struct tc_fn *F, int v, int lo, int hi)
{
    if (in_reg(F, v)) {
        mv2(F, F->loc[v], lo, F->loc[v] + 1, hi);
        return;
    }
    if (v < 0 || F->slot[v] < 0)
        return;
    st_sp(F, lo, sslot(F, v), 4);
    st_sp(F, hi, sslot(F, v) + 4, 4);
}

/* A folded constant as the register holds it: its low 32 bits,
 * sign-extended. */
static long long imm_val(const struct ir_ins *i)
{
    return (long long)(int)(unsigned int)(unsigned long)i->imm;
}

/* An instruction's second operand into `reg`: the folded immediate, or b.
 * Every binary operation asks here; none reads i->b when imm_b is set. */
static void operand_b(struct tc_fn *F, const struct ir_ins *i, int reg)
{
    if (i->imm_b)
        tc_li(F->t, reg, imm_val(i));
    else
        rd(F, i->b, reg);
}

static void operand_b64(struct tc_fn *F, const struct ir_ins *i,
                        int lo, int hi)
{
    if (i->imm_b) {
        tc_li(F->t, lo, (long long)(i->imm & 0xffffffffL));
        tc_li(F->t, hi, (long long)((i->imm >> 32) & 0xffffffffL));
    } else {
        rd64(F, i->b, lo, hi);
    }
}

/* Sign- or zero-extend the low `size` bytes of rs. */
static void ext_reg(struct tc_fn *F, int rdst, int rs, int size, int sign)
{
    if (size >= 4) {
        if (rdst != rs)
            tc_mov(F->t, rdst, rs);
        return;
    }
    if (!sign && size == 1) {
        tc_alu_imm(F->t, TC_AND, rdst, rs, 0xff);
        return;
    }
    tc_extr(F->t, rdst, rs, 0, size * 8, sign);
}

/* ---- loads and stores that may be misaligned ----------------------------
 *
 * A halfword or word access to an odd address traps (ALN). C promises
 * alignment everywhere but a packed struct's member, and irgen marks the
 * accesses it can promise (ir_ins.natural); the rest go a byte at a
 * time. The base is an address register; `rt` a data register, never
 * SCR. */
static void ld_any(struct tc_fn *F, int rt, int base, long off, int size,
                   int sign, int aligned)
{
    struct code *t = F->t;
    if (aligned || size == 1) {
        tc_load(t, rt, base, off, size, sign);
        return;
    }
    if (size == 4) {
        tc_load(t, rt, base, off, 1, 0);
        for (int b = 1; b < 4; b++) {
            tc_load(t, SCR, base, off + b, 1, 0);
            tc_insert(t, rt, rt, SCR, 8 * b, 8);
        }
        return;
    }
    /* size 2: the high byte, extended as the value is, then the low */
    tc_load(t, SCR, base, off + 1, 1, sign);
    tc_load(t, rt, base, off, 1, 0);
    tc_alu_imm(t, TC_SH, SCR, SCR, 8);
    tc_alu(t, TC_OR, rt, rt, SCR);
}

static void st_any(struct tc_fn *F, int rt, int base, long off, int size,
                   int aligned)
{
    struct code *t = F->t;
    if (aligned || size == 1) {
        tc_store(t, rt, base, off, size);
        return;
    }
    tc_store(t, rt, base, off, 1);
    for (int b = 1; b < size; b++) {
        tc_alu_imm(t, TC_SH, SCR, rt, -8 * b);
        tc_store(t, SCR, base, off + b, 1);
    }
}

/* ---- branches ----------------------------------------------------------------
 *
 * A branch to a label is resolved when the function ends, with no
 * relocation: +-32 KiB for a conditional one, +-16 MiB for a J. One that
 * does not reach takes the long form on the next attempt: the inverse
 * condition over a J. */
enum { FX_B, FX_J };

static void want_label(struct tc_fn *F, int at, int label, int kind)
{
    if (F->nfix == F->capfix) {
        F->capfix = F->capfix ? F->capfix * 2 : 16;
        F->fix = xrealloc(F->fix, (size_t)F->capfix * sizeof *F->fix);
    }
    F->fix[F->nfix].at = at;
    F->fix[F->nfix].label = label;
    F->fix[F->nfix].kind = kind;
    F->nfix++;
}

/* Does the next branch to a label take the long form? */
static int want_long(const struct tc_fn *F)
{
    return F->longb && F->nfix < F->nlongb && F->longb[F->nfix];
}

static int inverse_cond(int c)
{
    switch (c) {
    case TC_JEQ:   return TC_JNE;
    case TC_JNE:   return TC_JEQ;
    case TC_JLT:   return TC_JGE;
    case TC_JGE:   return TC_JLT;
    case TC_JLTU:  return TC_JGEU;
    case TC_JGEU:  return TC_JLTU;
    case TC_JEQ_A: return TC_JNE_A;
    case TC_JNE_A: return TC_JEQ_A;
    case TC_JZ_A:  return TC_JNZ_A;
    default:       return TC_JZ_A;      /* TC_JNZ_A */
    }
}

static void jump_to(struct tc_fn *F, int label)
{
    int at = tc_j_placeholder(F->t);
    want_label(F, at, label, FX_J);
}

/* Branch to `label` when `s1 cond s2` (registers). */
static void branch_rr(struct tc_fn *F, int cond, int s1, int s2, int label)
{
    if (want_long(F)) {
        /* the inverse branch over the J, then the J; the J is the fixup
         * this branch's index names, so the list stays in step */
        tc_w(F->t, tc_enc_jcc(inverse_cond(cond), s1, s2, 8));
        jump_to(F, label);
        return;
    }
    want_label(F, tc_jcc_placeholder(F->t, cond, s1, s2), label, FX_B);
}

/* ...or `s1 cond k` with a constant; one that does not fit the 4-bit
 * field is built in TMP first (s1 is never TMP when k may not fit). */
static void branch_ri(struct tc_fn *F, int cond, int s1, long long k,
                      int label)
{
    if (!tc_jcci_ok(cond, k)) {
        tc_li(F->t, TMP == s1 ? SCR : TMP, k);
        branch_rr(F, cond, s1, TMP == s1 ? SCR : TMP, label);
        return;
    }
    if (want_long(F)) {
        tc_w(F->t, tc_enc_jcci(inverse_cond(cond), s1, k, 8));
        jump_to(F, label);
        return;
    }
    want_label(F, tc_jcci_placeholder(F->t, cond, s1, k), label, FX_B);
}

/* A branch within one lowering, short and patched right away. */
static int br_placei(struct tc_fn *F, int cond, int s1, long long k)
{
    return tc_jcci_placeholder(F->t, cond, s1, k);
}

static void br_land(struct tc_fn *F, int at)
{
    if (!tc_patch(F->t, at, F->t->len))
        internal_error("tricore: %s: a branch inside one operation does "
                       "not reach", F->fn->name);
}

static void br_back(struct tc_fn *F, int at, int target)
{
    if (!tc_patch(F->t, at, target))
        internal_error("tricore: %s: a loop inside one operation does not "
                       "reach", F->fn->name);
}

/* ---- site lists ------------------------------------------------------------- */

static void note_ext(struct tc_sites *st, int at, struct func *callee,
                     int tail)
{
    if (st->next == st->capext) {
        st->capext = st->capext ? st->capext * 2 : 16;
        st->ext = xrealloc(st->ext, (size_t)st->capext * sizeof *st->ext);
    }
    st->ext[st->next].patch_off = at;
    st->ext[st->next].callee = callee;
    st->ext[st->next].tail = tail;
    st->next++;
}

static void note_str(struct tc_sites *st, int at, int idx, enum reloc_kind k)
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

static void note_glob(struct tc_sites *st, int at, struct global *g,
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

static void note_fn(struct tc_sites *st, int at, struct func *target,
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

/* `movh rd, 0; addi rd, rd, 0`, an absolute address in two halves, the
 * HI site noted before its LO. Returns the movh's offset. */
static int abs_pair(struct tc_fn *F, int rd_)
{
    int at = F->t->len;
    tc_movh(F->t, rd_, 0);
    tc_addi(F->t, rd_, rd_, 0);
    return at;
}

/* A call: CALL with an R_TRICORE_24REL, even to a function in this unit
 * (the linker resolves it); a tail call is a J. */
static void call_sym(struct tc_fn *F, struct func *callee, int tail)
{
    int at = F->t->len;
    if (tail) tc_j0(F->t);
    else      tc_call0(F->t);
    note_ext(F->st, at, callee, tail);
}

/* The runtime helpers, interned by name. */
static struct func **g_helpers;
static int g_nhelpers, g_caphelpers;

static void call_helper(struct tc_fn *F, const char *name)
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
    call_sym(F, h, 0);
}

/* ---- soft float -------------------------------------------------------------
 *
 * Every floating-point operation is a libgcc call (lib/rt/softfp.c), a
 * float in one data register and a double in a pair: operands in D4, D5
 * (E4, E6), the result in D2 (E2). */
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

/* Put n vregs into the data registers a helper (or a call) expects, all
 * at once: the register-to-register edges as one parallel move (SCR
 * breaks a cycle), then the loads, which only write. `half` (may be
 * NULL) picks word 0 or 1 of a 64-bit value. */
static void set_args_half(struct tc_fn *F, const int *dstreg,
                          const int *vreg, const int *half, int n)
{
    int pd[RA_MAXPOOL * 2], ps[RA_MAXPOOL * 2], npm = 0;

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
            internal_error("tricore: an argument setup is not a well-formed "
                           "move");
        for (int k = 0; k < m; k++)
            tc_mov(F->t, od[k], os[k]);
    }
    for (int k = 0; k < n; k++)
        if (!in_reg(F, vreg[k]))
            ld_sp(F, dstreg[k],
                  sslot(F, vreg[k]) + (half ? 4L * half[k] : 0), 4, 1);
}

static void set_args(struct tc_fn *F, const int *dstreg, const int *vreg,
                     int n)
{
    set_args_half(F, dstreg, vreg, NULL, n);
}

/* Two 64-bit operands into E4 and E6 (vb < 0: only the first). */
static void args64x2(struct tc_fn *F, int va, int vb)
{
    int d[4] = { 4, 5, 6, 7 };
    int v[4], h[4] = { 0, 1, 0, 1 };
    v[0] = v[1] = va;
    v[2] = v[3] = vb;
    set_args_half(F, d, v, h, vb >= 0 ? 4 : 2);
}

static void fp_args2(struct tc_fn *F, const struct ir_ins *i)
{
    if (i->w == 8) {
        args64x2(F, i->a, i->b);
        return;
    }
    {
        int dstreg[2], vreg[2];
        dstreg[0] = 4; vreg[0] = i->a;
        dstreg[1] = 5; vreg[1] = i->b;
        set_args(F, dstreg, vreg, 2);
    }
}

static void fp_result(struct tc_fn *F, int dst, int w)
{
    if (dst < 0)
        return;
    if (w == 8) wr64(F, dst, 2, 3);
    else        wr(F, dst, 2);
}

/* ---- comparisons -------------------------------------------------------------
 *
 * EQ, NE, LT, LT.U, GE and GE.U leave 0 or 1; GT and LE are LT and GE
 * with the operands swapped. */
static void cmp_to_reg(struct tc_fn *F, enum binop pred, int sign,
                       int ra_, int rb_, int dst)
{
    struct code *t = F->t;
    switch (pred) {
    case B_EQ: tc_alu(t, TC_EQ, dst, ra_, rb_); return;
    case B_NE: tc_alu(t, TC_NE, dst, ra_, rb_); return;
    case B_LT: tc_alu(t, sign ? TC_LT : TC_LTU, dst, ra_, rb_); return;
    case B_GT: tc_alu(t, sign ? TC_LT : TC_LTU, dst, rb_, ra_); return;
    case B_GE: tc_alu(t, sign ? TC_GE : TC_GEU, dst, ra_, rb_); return;
    default:   tc_alu(t, sign ? TC_GE : TC_GEU, dst, rb_, ra_); return;
    }
}

/* The same against a constant k (sign-extended from 32 bits), with the
 * RC form's 9-bit field: `x > k` is `x >= k + 1` and `x <= k` is
 * `x < k + 1`. Returns 0 where k does not fit, and the caller loads it. */
static int cmp_imm_to_reg(struct tc_fn *F, enum binop pred, int sign,
                          int ra_, long long k, int dst)
{
    int op;
    switch (pred) {
    case B_EQ: op = TC_EQ; break;
    case B_NE: op = TC_NE; break;
    case B_LT: op = sign ? TC_LT : TC_LTU; break;
    case B_GE: op = sign ? TC_GE : TC_GEU; break;
    case B_GT: op = sign ? TC_GE : TC_GEU; break;
    default:   op = sign ? TC_LT : TC_LTU; break;        /* B_LE */
    }
    if (pred == B_GT || pred == B_LE) {
        if (sign ? k == 0x7fffffffLL : (k & 0xffffffffLL) == 0xffffffffLL)
            return 0;               /* k + 1 wraps */
        k++;
    }
    if (!sign)
        k &= 0xffffffffLL;
    if (!tc_alu_imm_ok(op, k))
        return 0;
    tc_alu_imm(F->t, op, dst, ra_, k);
    return 1;
}

/* ---- 64-bit integers, in register pairs -------------------------------------
 *
 * The operations work on A_LO:A_HI and B_LO:B_HI unless a case says
 * otherwise. Add and subtract carry through PSW.C. */
static void add64(struct tc_fn *F)
{
    tc_alu(F->t, TC_ADDX, A_LO, A_LO, B_LO);
    tc_alu(F->t, TC_ADDC, A_HI, A_HI, B_HI);
}

static void sub64(struct tc_fn *F)
{
    tc_alu(F->t, TC_SUBX, A_LO, A_LO, B_LO);
    tc_alu(F->t, TC_SUBC, A_HI, A_HI, B_HI);
}

/* A shift by a constant from (al, ah) into (dl, dh): the same pair or
 * one sharing no register with it. Left (`left`) or right, arithmetic
 * when `sign`. DEXTR is the funnel: the high word of {hi, lo} << n. */
static void shift64_imm_to(struct tc_fn *F, int left, int sign, long n,
                           int al, int ah, int dl, int dh)
{
    struct code *t = F->t;
    n &= 63;
    if (n == 0) {
        mv2(F, dl, al, dh, ah);
        return;
    }
    if (n >= 32) {
        int k = (int)(n - 32);
        if (left) {
            if (k) tc_alu_imm(t, TC_SH, dh, al, k);
            else if (dh != al) tc_mov(t, dh, al);
            tc_mov_imm(t, dl, 0);
        } else {
            if (k) tc_alu_imm(t, sign ? TC_SHA : TC_SH, dl, ah, -k);
            else if (dl != ah) tc_mov(t, dl, ah);
            if (sign) tc_alu_imm(t, TC_SHA, dh, ah, -31);
            else      tc_mov_imm(t, dh, 0);
        }
        return;
    }
    if (left) {
        tc_dextr(t, dh, ah, al, (int)n);
        tc_alu_imm(t, TC_SH, dl, al, (int)n);
    } else {
        tc_dextr(t, dl, ah, al, (int)(32 - n));
        tc_alu_imm(t, sign ? TC_SHA : TC_SH, dh, ah, (int)-n);
    }
}

/* A shift of A_LO:A_HI by the count in B_LO, in two arms: a count below
 * 32 mixes the halves, one of 32 or more moves a half wholesale. The
 * register shifts read their count's low six bits SIGNED, so every count
 * here is first made 0..31, and a right shift is a shift by its
 * negation. */
static void shift64_var(struct tc_fn *F, int left, int sign)
{
    struct code *t = F->t;
    int big, done;

    tc_alu_imm(t, TC_AND, B_LO, B_LO, 63);
    tc_alu_imm(t, TC_AND, B_HI, B_LO, 31);              /* the count mod 32 */
    /* (a 4-bit constant cannot say 32: test the bit instead) */
    tc_alu_imm(t, TC_AND, SCR, B_LO, 32);
    big = br_placei(F, TC_JNE, SCR, 0);
    if (left) {
        tc_dextr_r(t, A_HI, A_HI, A_LO, B_HI);
        tc_alu(t, TC_SH, A_LO, A_LO, B_HI);
    } else {
        /* lo = lo >> n | hi << (32 - n), where a count of 32 is the
         * SH's -32: a right shift by 32, zero -- right for n == 0 */
        tc_alu_imm(t, TC_RSUB, SCR, B_HI, 0);           /* -n */
        tc_alu(t, TC_SH, A_LO, A_LO, SCR);
        tc_alu_imm(t, TC_RSUB, B_LO, B_HI, 32);         /* 32 - n */
        tc_alu(t, TC_SH, B_LO, A_HI, B_LO);
        tc_alu(t, TC_OR, A_LO, A_LO, B_LO);
        tc_alu(t, sign ? TC_SHA : TC_SH, A_HI, A_HI, SCR);
    }
    done = tc_j_placeholder(t);
    br_land(F, big);
    if (left) {
        tc_alu(t, TC_SH, A_HI, A_LO, B_HI);
        tc_mov_imm(t, A_LO, 0);
    } else {
        tc_alu_imm(t, TC_RSUB, SCR, B_HI, 0);
        tc_alu(t, sign ? TC_SHA : TC_SH, A_LO, A_HI, SCR);
        if (sign) tc_alu_imm(t, TC_SHA, A_HI, A_HI, -31);
        else      tc_mov_imm(t, A_HI, 0);
    }
    if (!tc_patch(t, done, t->len))
        internal_error("tricore: a 64-bit shift's jump does not reach");
}

/* d = s OP c for one 32-bit half of a 64-bit AND/OR/XOR with a constant:
 * the identity is a copy, AND with 0 a zero, OR with all ones -1, a
 * 9-bit c the RC form, else c is built in SCR. d may be s; neither is
 * SCR. */
static void logic_half(struct tc_fn *F, int op, int d, int s,
                       unsigned long c)
{
    struct code *t = F->t;
    c &= 0xffffffffUL;
    if ((op == TC_AND && c == 0xffffffffUL) || (op != TC_AND && c == 0)) {
        if (d != s) tc_mov(t, d, s);
        return;
    }
    if (op == TC_AND && c == 0) {
        tc_mov_imm(t, d, 0);
        return;
    }
    if (op == TC_OR && c == 0xffffffffUL) {
        tc_mov_imm(t, d, -1);
        return;
    }
    if (c <= 511) {
        tc_alu_imm(t, op, d, s, (long long)c);
        return;
    }
    if (op == TC_AND && (~c & 0xffffffffUL) <= 511) {
        tc_alu_imm(t, TC_ANDN, d, s, (long long)(~c & 0xffffffffUL));
        return;
    }
    if (op == TC_AND && (c & (c + 1)) == 0) {          /* low k bits */
        int k = 0;
        while (c >> k & 1) k++;
        tc_extr(t, d, s, 0, k, 0);
        return;
    }
    tc_li(t, SCR, (long long)(int)(unsigned int)c);
    tc_alu(t, op, d, s, SCR);
}

/* Where a 64-bit operand's halves are: its pair, or the given scratches
 * after a load. And where to compute a 64-bit result: its pair, or A. */
static void src64(struct tc_fn *F, int v, int slo, int shi, int *lo, int *hi)
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

static void dst64(struct tc_fn *F, int v, int *lo, int *hi)
{
    *lo = in_reg(F, v) ? F->loc[v] : A_LO;
    *hi = in_reg(F, v) ? F->loc[v] + 1 : A_HI;
}

/* A 64-bit comparison into ACC: the high words decide unless they are
 * equal, and then the low words, compared UNSIGNED. A and B are loaded
 * as copies; ACC is A_LO, so the result is built in SCR first. */
static void cmp64(struct tc_fn *F, const struct ir_ins *i, enum binop pred,
                  int sign)
{
    struct code *t = F->t;
    int sw = pred == B_GT || pred == B_LE;
    rd64(F, i->a, A_LO, A_HI);
    operand_b64(F, i, B_LO, B_HI);
    if (pred == B_EQ || pred == B_NE) {
        tc_alu(t, TC_XOR, A_LO, A_LO, B_LO);
        tc_alu(t, TC_XOR, A_HI, A_HI, B_HI);
        tc_alu(t, TC_OR, A_LO, A_LO, A_HI);
        tc_alu_imm(t, pred == B_EQ ? TC_EQ : TC_NE, ACC, A_LO, 0);
        return;
    }
    {
        /* x < y: the high words less, or equal and the low words less
         * unsigned; >= is its inverse, and GT/LE swap the operands */
        int xl = sw ? B_LO : A_LO, xh = sw ? B_HI : A_HI;
        int yl = sw ? A_LO : B_LO, yh = sw ? A_HI : B_HI;
        int ge = pred == B_GE || pred == B_LE;
        tc_alu(t, TC_LTU, SCR, xl, yl);                 /* low less */
        tc_alu(t, TC_EQ, xl, xh, yh);                   /* highs equal */
        tc_alu(t, TC_AND, SCR, SCR, xl);
        tc_alu(t, sign ? TC_LT : TC_LTU, xl, xh, yh);   /* high less */
        tc_alu(t, TC_OR, SCR, SCR, xl);
        if (ge)
            tc_alu_imm(t, TC_XOR, ACC, SCR, 1);
        else
            tc_mov(t, ACC, SCR);
    }
}

/* Every 64-bit operation that is not a call. Returns 0 for one this does
 * not handle, which the caller refuses by name. */
static int gen_ins64(struct tc_fn *F, int n)
{
    struct ir_func *fn = F->fn;
    struct ir_ins *i = &fn->ins[n];
    struct code *t = F->t;

    switch (i->op) {
    case IR_CONST: {
        int lo, hi;
        dst64(F, i->dst, &lo, &hi);
        tc_li(t, lo, (long long)(i->imm & 0xffffffffL));
        tc_li(t, hi, (long long)((i->imm >> 32) & 0xffffffffL));
        wr64(F, i->dst, lo, hi);
        return 1;
    }
    case IR_BITCAST:
    case IR_MOV:
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
        int op = i->op == IR_AND ? TC_AND : i->op == IR_OR ? TC_OR : TC_XOR;
        int al, ah, bl, bh, dl, dh;
        src64(F, i->a, A_LO, A_HI, &al, &ah);
        if (i->imm_b) {
            dst64(F, i->dst, &dl, &dh);
            logic_half(F, op, dl, al, (unsigned long)i->imm);
            logic_half(F, op, dh, ah, (unsigned long)i->imm >> 32);
            wr64(F, i->dst, dl, dh);
            return 1;
        }
        src64(F, i->b, B_LO, B_HI, &bl, &bh);
        dst64(F, i->dst, &dl, &dh);
        tc_alu(t, op, dl, al, bl);
        tc_alu(t, op, dh, ah, bh);
        wr64(F, i->dst, dl, dh);
        return 1;
    }
    case IR_MUL:
        /* (ah:al) * (bh:bl) keeping 64 bits: the cross terms reach only
         * the high word, so they are summed into A_HI first, and then
         * MUL.U makes al * bl whole in E2 over the B pair it reads */
        rd64(F, i->a, A_LO, A_HI);
        operand_b64(F, i, B_LO, B_HI);
        tc_alu(t, TC_MUL, A_HI, A_HI, B_LO);
        tc_madd(t, A_HI, A_HI, A_LO, B_HI);
        tc_mul64(t, B_LO, A_LO, B_LO, 0);
        tc_alu(t, TC_ADD, B_HI, B_HI, A_HI);
        wr64(F, i->dst, B_LO, B_HI);
        return 1;
    case IR_NEG:
        tc_mov_imm(t, A_LO, 0);
        tc_mov_imm(t, A_HI, 0);
        rd64(F, i->a, B_LO, B_HI);
        sub64(F);
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    case IR_BNOT: {
        int al, ah, dl, dh;
        src64(F, i->a, A_LO, A_HI, &al, &ah);
        dst64(F, i->dst, &dl, &dh);
        tc_alu_imm(t, TC_NOR, dl, al, 0);
        tc_alu_imm(t, TC_NOR, dh, ah, 0);
        wr64(F, i->dst, dl, dh);
        return 1;
    }
    case IR_SHL: case IR_SHR: {
        int sign = i->op == IR_SHR && i->sign;
        if (i->imm_b) {
            int al, ah, dl, dh;
            src64(F, i->a, A_LO, A_HI, &al, &ah);
            dst64(F, i->dst, &dl, &dh);
            /* in place, or into A from somewhere else: never a partial
             * overlap, because pairs are whole and aligned */
            if (dl != al && (dl == ah || dh == al)) {
                rd64(F, i->a, A_LO, A_HI);
                al = A_LO; ah = A_HI;
            }
            shift64_imm_to(F, i->op == IR_SHL, sign, (long)i->imm,
                           al, ah, dl, dh);
            wr64(F, i->dst, dl, dh);
            return 1;
        }
        rd64(F, i->a, A_LO, A_HI);
        rd(F, i->b, B_LO);
        shift64_var(F, i->op == IR_SHL, sign);
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    }
    case IR_EXT:
        /* to 64 bits: the low word is the value, the high its sign or 0 */
        rd(F, i->a, A_LO);
        if (i->size < 4)
            ext_reg(F, A_LO, A_LO, i->size, i->sign);
        if (i->sign) tc_alu_imm(t, TC_SHA, A_HI, A_LO, -31);
        else         tc_mov_imm(t, A_HI, 0);
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    /* An access is `size` bytes whatever the value's width: a four-byte
     * store of an eight-byte value stores its low word, a narrower read
     * extends into the high one (riscv/codegen.c says why). */
    case IR_LDVAR:
        if (i->size == 8) {
            rd64(F, i->a, A_LO, A_HI);
        } else {
            if (in_reg(F, i->a))
                ext_reg(F, A_LO, F->loc[i->a], i->size, i->sign);
            else
                ld_sp(F, A_LO, sslot(F, i->a), i->size, i->sign);
            if (i->sign) tc_alu_imm(t, TC_SHA, A_HI, A_LO, -31);
            else         tc_mov_imm(t, A_HI, 0);
        }
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    case IR_STVAR:
        rd64(F, i->a, A_LO, A_HI);
        if (i->size == 8)
            wr64(F, i->dst, A_LO, A_HI);
        else if (in_reg(F, i->dst))
            ext_reg(F, F->loc[i->dst], A_LO, i->size, 1);
        else if (F->slot[i->dst] >= 0)
            st_sp(F, A_LO, sslot(F, i->dst), i->size);
        return 1;
    case IR_LOAD:
        rda(F, i->a, AD);
        if (i->size == 8) {
            ld_any(F, A_LO, AD, i->memoff, 4, 1, i->natural);
            ld_any(F, A_HI, AD, i->memoff + 4, 4, 1, i->natural);
        } else {
            ld_any(F, A_LO, AD, i->memoff, i->size, i->sign, i->natural);
            if (i->sign) tc_alu_imm(t, TC_SHA, A_HI, A_LO, -31);
            else         tc_mov_imm(t, A_HI, 0);
        }
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    case IR_STORE:
        rda(F, i->a, AD);
        rd64(F, i->b, A_LO, A_HI);
        st_any(F, A_LO, AD, i->memoff, i->size == 8 ? 4 : i->size,
               i->natural);
        if (i->size == 8)
            st_any(F, A_HI, AD, i->memoff + 4, 4, i->natural);
        return 1;
    case IR_SELECT: {
        /* dst = a ? b : c, half by half with SEL on the condition (both
         * halves of a 64-bit one ORed) */
        int bl, bh;
        if (i->size == 8) {
            int al, ah;
            src64(F, i->a, A_LO, A_HI, &al, &ah);
            tc_alu(t, TC_OR, SCR, al, ah);
        } else {
            rd(F, i->a, SCR);
        }
        rd64(F, i->c, A_LO, A_HI);
        src64(F, i->b, B_LO, B_HI, &bl, &bh);
        tc_sel(t, A_LO, SCR, bl, A_LO);
        tc_sel(t, A_HI, SCR, bh, A_HI);
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    }
    default:
        return 0;
    }
}

/* ---- one call ------------------------------------------------------------ */

/* Can the call at n be a TAIL call -- a J to the callee, which then
 * returns to this function's caller? TriCore's RET pops the context the
 * CALL into THIS function pushed, so a J leaves the callee's RET to do
 * exactly that, and this function's frame is gone with it. Only when
 * nothing of the frame can still be needed and the IR_RET after it
 * returns what the call returned, in the same register: no stack
 * arguments (they would be in this frame), no struct results, nothing
 * address-taken. The J is taken with A10 restored to its entry value. */
static int tc_tail_ok(const struct tc_fn *F, int n)
{
    const struct ir_func *fn = F->fn;
    const struct ir_ins *i = &fn->ins[n], *r;
    struct argplace pl[MAX_PARAMS];

    if (i->op != IR_CALL || i->indirect || i->call_varargs || i->retsize ||
        i->flt || getenv("EMBCC_NO_TAILCALL"))
        return 0;
    if (fn->ret_abi.is_struct || fn->ret_abi.is_float)
        return 0;
    if (n + 1 >= fn->nins) {
        if (fn->ret_abi.size)
            return 0;
    } else {
        r = &fn->ins[n + 1];
        if (r->op != IR_RET)
            return 0;
        if (r->a >= 0 && (r->a != i->dst ||
                          i->ret_tybytes != fn->ret_abi.size ||
                          i->ret_tybytes > 4 ||
                          i->ret_ptr != ty_is_ptr(fn->ret_abi.ty)))
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
        if (nret > 1)
            return 0;
    }
    if (fn->has_alloca || fn->is_varargs || fn->neh)
        return 0;
    place_args(i->argv, i->nargs, 0, -1, pl);
    for (int k = 0; k < i->nargs; k++)
        if (pl[k].kind == PL_STK || pl[k].byref || i->argv[k].is_struct)
            return 0;
    return 1;
}

/* A struct's bytes from the address in `base` + off into the data
 * register r: `left` bytes (1-4), packed from the lowest address up. */
static void pack_word(struct tc_fn *F, int r, int base, long off, long left)
{
    if (left >= 4) {
        ld_any(F, r, base, off, 4, 0, 0);
        return;
    }
    tc_load(F->t, r, base, off, 1, 0);
    for (long b = 1; b < left; b++) {
        tc_load(F->t, SCR, base, off + b, 1, 0);
        tc_insert(F->t, r, r, SCR, (int)(8 * b), 8);
    }
}

static void copy_block(struct tc_fn *F, int copy, long size, int aligned);

static void gen_call(struct tc_fn *F, int n)
{
    struct ir_func *fn = F->fn;
    struct ir_ins *i = &fn->ins[n];
    struct code *t = F->t;
    struct argplace pl[MAX_PARAMS];
    int sret = call_sret(i);
    long copy_off = F->copy_at;

    place_args(i->argv, i->nargs, sret, call_nfixed(i), pl);

    /* An indirect call's target first, into A15, which no argument uses
     * and which the setup below does not disturb. */
    if (i->indirect && !(F->tail && F->tail[n]))
        rda(F, i->a, ACALL);

    /* The by-reference copies, and the stack words: storing needs
     * scratches, and once the argument registers are loaded none is left
     * that is not an argument. A composite's words are read with
     * unaligned-safe loads -- its address may be a packed member's. */
    for (int k = 0; k < i->nargs; k++) {
        struct ir_arg *a = &i->argv[k];
        struct argplace *p = &pl[k];
        if (p->byref) {
            /* the copy, in this frame's copy area; the argument is its
             * address, like a pointer's */
            rda(F, a->vreg, AD2);
            lea_off(F, AD, F->fb, copy_off);
            copy_block(F, 1, a->size, 0);
            a->copy_off = (int)copy_off;
            copy_off += (a->size + 7) & ~7L;
            if (p->kind == PL_STK) {
                lea_off(F, AD, F->fb, a->copy_off);
                sta_out(F, AD, p->stk);
            }
            continue;
        }
        if (p->kind != PL_STK)
            continue;
        if (a->is_struct) {
            rda(F, a->vreg, AD);
            for (int q = 0; q < p->words; q++) {
                long off = 4L * q, left = a->size - off;
                if (left >= 4) {
                    ld_any(F, ACC, AD, off, 4, 0, 0);
                    st_out(F, ACC, p->stk + off, 4);
                } else {
                    for (long b = 0; b < left; b++) {
                        tc_load(t, ACC, AD, off + b, 1, 0);
                        st_out(F, ACC, p->stk + off + b, 1);
                    }
                }
            }
        } else if (a->size > 4) {
            rd64(F, a->vreg, A_LO, A_HI);
            st_out(F, A_LO, p->stk, 4);
            st_out(F, A_HI, p->stk + 4, 4);
        } else {
            rd(F, a->vreg, ACC);
            st_out(F, ACC, p->stk, 4);
        }
    }
    /* The pointer arguments into A4-A7, from their homes: before the data
     * registers move, since a home may be one of D4-D7. */
    for (int k = 0; k < i->nargs; k++) {
        struct ir_arg *a = &i->argv[k];
        if (pl[k].kind != PL_A)
            continue;
        if (pl[k].byref)
            lea_off(F, pl[k].reg, F->fb, a->copy_off);
        else
            rda(F, a->vreg, pl[k].reg);
    }
    /* The scalar data-register arguments, all at once: a value for D4 may
     * be in the register D6 is about to get. A 64-bit one in a pair is two
     * edges of the same move. Before the struct words below, which also
     * write argument registers; the allocator keeps every struct
     * argument's address in memory, so they cannot be a source here. */
    {
        int sd_[2 * MAX_PARAMS], sv_[2 * MAX_PARAMS], sh_[2 * MAX_PARAMS];
        int ns_ = 0;
        for (int k = 0; k < i->nargs; k++) {
            struct ir_arg *a = &i->argv[k];
            if ((pl[k].kind != PL_D && pl[k].kind != PL_E) || a->is_struct)
                continue;
            for (int q = 0; q < (pl[k].kind == PL_E ? 2 : 1); q++) {
                sd_[ns_] = pl[k].reg + q;
                sv_[ns_] = a->vreg;
                sh_[ns_] = pl[k].kind == PL_E ? q : 0;
                ns_++;
            }
        }
        if (ns_)
            set_args_half(F, sd_, sv_, sh_, ns_);
    }
    for (int k = 0; k < i->nargs; k++) {
        struct ir_arg *a = &i->argv[k];
        if ((pl[k].kind != PL_D && pl[k].kind != PL_E) || !a->is_struct)
            continue;
        rda(F, a->vreg, AD);
        pack_word(F, pl[k].reg, AD, 0, a->size < 4 ? a->size : 4);
        if (pl[k].kind == PL_E)
            pack_word(F, pl[k].reg + 1, AD, 4, a->size - 4);
    }
    /* The hidden result pointer last, so nothing above used A4 after it. */
    if (sret)
        lea_off(F, 4, F->fb, F->scratch_at + i->scratch);

    if (F->tail && F->tail[n]) {
        /* the frame released, then a J: the callee's RET returns to this
         * function's caller (tc_tail_ok) */
        if (F->frame)
            lea_off(F, TC_SP, TC_SP, F->frame);
        call_sym(F, i->callee, 1);
        if (n + 1 < fn->nins)
            F->skip_next = 1;         /* the IR_RET: not reached */
        return;
    }
    if (i->indirect)
        tc_calli(t, ACALL);
    else
        call_sym(F, i->callee, 0);

    if (i->dst < 0)
        return;
    if (i->retsize) {
        /* dst receives the scratch's ADDRESS, the contract irgen shares
         * with every backend. A small struct came back in D2 or E2 and
         * is stored there first; a large one the callee wrote through A4. */
        long at = F->scratch_at + i->scratch;
        if (!sret) {
            if (i->retsize > 4) {
                st_sp(F, 2, at, 4);
                st_sp(F, 3, at + 4, 4);
            } else {
                st_sp(F, 2, at, 4);
            }
        }
        addr_sp(F, ACC, at);
        wr(F, i->dst, ACC);
    } else if (F->wide[i->dst]) {
        wr64(F, i->dst, 2, 3);
    } else if (i->ret_ptr) {
        tc_mov_d(t, ACC, 2);
        wr(F, i->dst, ACC);
    } else {
        wr(F, i->dst, 2);
    }
}

/* ---- one instruction ----------------------------------------------------- */

/* The conversion helpers' names, libgcc's. */
static const char *cvt_name(const struct ir_ins *i)
{
    int src_w = i->size, dst_w = i->w;
    if (i->op == IR_I2F)
        return src_w <= 4
             ? (dst_w == 8 ? (i->sign ? "__floatsidf" : "__floatunsidf")
                           : (i->sign ? "__floatsisf" : "__floatunsisf"))
             : (dst_w == 8 ? (i->sign ? "__floatdidf" : "__floatundidf")
                           : (i->sign ? "__floatdisf" : "__floatundisf"));
    if (i->op == IR_F2I)
        return dst_w <= 4
             ? (src_w == 8 ? (i->sign ? "__fixdfsi" : "__fixunsdfsi")
                           : (i->sign ? "__fixsfsi" : "__fixunssfsi"))
             : (src_w == 8 ? (i->sign ? "__fixdfdi" : "__fixunsdfdi")
                           : (i->sign ? "__fixsfdi" : "__fixunssfdi"));
    return dst_w == 8 ? "__extendsfdf2" : "__truncdfsf2";
}

/* The branch for `ra_ cond rb_`, with GT and LE swapped into LT and GE. */
static int pred_cond(enum binop pred, int sign, int *swap)
{
    *swap = 0;
    switch (pred) {
    case B_EQ: return TC_JEQ;
    case B_NE: return TC_JNE;
    case B_LT: return sign ? TC_JLT : TC_JLTU;
    case B_GE: return sign ? TC_JGE : TC_JGEU;
    case B_GT: *swap = 1; return sign ? TC_JLT : TC_JLTU;
    default:   *swap = 1; return sign ? TC_JGE : TC_JGEU;   /* B_LE */
    }
}

/* ---- one- and two-byte atomics ---------------------------------------
 *
 * SWAP.W and CMPSWAP.W are word-sized, so a narrow atomic works on the
 * aligned word around it, as GCC's and LLVM's do elsewhere: a CMPSWAP.W
 * loop that rewrites only its lane,
 *
 *   retry: ld.w      d1, [a12]               (a12 = the aligned word)
 *          d0 = f(d1)  in the lane
 *          d0 = d1 ^ ((d0 ^ d1) & mask)
 *          cmpswap.w [a12], e0               (stored if still d1; d0 = seen)
 *          jne       d0, d1, retry
 *
 * atomic against the neighbouring bytes too: a store to any of them
 * between the load and the CMPSWAP.W makes it fail, and the loop goes
 * round. TriCore is little-endian: the lane of address a is bits
 * 8*(a & 3) up. SH shifts left by a positive count and right by a
 * negative one. The data scratches are five, D0-D3 and D15. */
#define SUB_MK 2            /* the lane's mask */
#define SUB_SH 3            /* the lane's shift */
#define SUB_VAL 15          /* the operand, moved into its lane */

/* sh = the lane's shift, from the address a (in place when sh == a) */
static void sub_shift(struct tc_fn *F, int sh, int a)
{
    tc_alu_imm(F->t, TC_AND, sh, a, 3);
    tc_alu_imm(F->t, TC_SH, sh, sh, 3);
}

/* AD = the aligned word, SUB_MK and SUB_SH, for the address in vreg a */
static void sub_lane(struct tc_fn *F, int av, int size)
{
    struct code *t = F->t;
    int a = rdr(F, av, SUB_VAL);
    sub_shift(F, SUB_SH, a);
    tc_alu_imm(t, TC_ANDN, SUB_MK, a, 3);
    tc_mov_a(t, AD, SUB_MK);
    tc_li(t, SUB_MK, size == 1 ? 0xff : 0xffff);
    tc_alu(t, TC_SH, SUB_MK, SUB_MK, SUB_SH);
}

/* reg = (src << SUB_SH) & mask */
static void sub_in(struct tc_fn *F, int reg, int src)
{
    tc_alu(F->t, TC_SH, reg, src, SUB_SH);
    tc_alu(F->t, TC_AND, reg, reg, SUB_MK);
}

/* reg, its lane already masked, shifted down to bit 0 and extended as
 * `sign` says; the shift is SUB_SH's, or worked out again from vreg a's
 * address when `again` */
static void sub_out(struct tc_fn *F, int reg, int a, int again, int size,
                    int sign)
{
    struct code *t = F->t;
    if (again)
        sub_shift(F, SUB_SH, rdr(F, a, SUB_SH));
    tc_alu_imm(t, TC_RSUB, SUB_SH, SUB_SH, 0);      /* right: negative */
    tc_alu(t, TC_SH, reg, reg, SUB_SH);
    if (sign)
        tc_extr(t, reg, reg, 0, 8 * size, 1);
}

/* swap, fetch-and-add and the bitwise ones, on a byte or a halfword */
static void sub_rmw(struct tc_fn *F, const struct ir_ins *i)
{
    struct code *t = F->t;
    int top, again;
    sub_lane(F, i->a, i->size);
    sub_in(F, SUB_VAL, rdr(F, i->b, SUB_VAL));
    tc_dsync(t);
    top = t->len;
    tc_load(t, 1, AD, 0, 4, 0);
    if (i->op == IR_XCHG) {
        tc_mov(t, 0, SUB_VAL);
    } else if (i->op == IR_XADD) {
        tc_alu(t, TC_ADD, 0, 1, SUB_VAL);
    } else {
        switch ((int)i->imm) {
        case '&': tc_alu(t, TC_AND, 0, 1, SUB_VAL); break;
        case '|': tc_alu(t, TC_OR, 0, 1, SUB_VAL); break;
        case '^': tc_alu(t, TC_XOR, 0, 1, SUB_VAL); break;
        default:  tc_alu(t, TC_NAND, 0, 1, SUB_VAL); break;    /* nand */
        }
    }
    /* only the lane changes: old ^ ((new ^ old) & mask) */
    tc_alu(t, TC_XOR, 0, 0, 1);
    tc_alu(t, TC_AND, 0, 0, SUB_MK);
    tc_alu(t, TC_XOR, 0, 0, 1);
    tc_cmpswap_w(t, 0, AD, 0);
    again = tc_jcc_placeholder(t, TC_JNE, 0, 1);
    br_back(F, again, top);
    tc_dsync(t);
    tc_alu(t, TC_AND, 1, 1, SUB_MK);
    sub_out(F, 1, i->a, 0, i->size, i->sign);
    wr(F, i->dst, 1);
}

/* compare-and-swap on a byte or a halfword. CMPSWAP.W compares the whole
 * word, so the word it is given to compare is the one last seen with the
 * expected value in the lane, and when it fails the loop looks at why:
 *
 *          ld.w  d1, [a12] ; d1 = (d1 & ~mask) | expected
 *   retry: d0 = d1 ^ x                       (x = expected ^ desired)
 *          cmpswap.w [a12], e0               (d0 = the word seen)
 *          jeq   d0, d1, out                 (swapped)
 *          ((d0 ^ d1) & ~mask) == 0: out     (the lane differs: failed)
 *          d1 = (d0 & ~mask) | expected ; j retry   (a neighbour moved)
 *   out:
 * d0 is the word seen either way. */
static void sub_cas(struct tc_fn *F, const struct ir_ins *i)
{
    struct code *t = F->t;
    int top, again, done, failed;
    sub_lane(F, i->a, i->size);
    sub_in(F, 0, rdr(F, i->c, 0));                     /* desired */
    if (i->op == IR_CAS) {
        sub_in(F, SUB_VAL, rdr(F, i->b, SUB_VAL));     /* expected */
    } else {
        rda(F, i->b, AD2);
        tc_load(t, SUB_VAL, AD2, 0, i->size, 0);
        sub_in(F, SUB_VAL, SUB_VAL);
    }
    tc_alu(t, TC_XOR, SUB_SH, 0, SUB_VAL);            /* x; no shift now */
    tc_dsync(t);
    tc_load(t, 1, AD, 0, 4, 0);
    tc_alu(t, TC_ANDN, 1, 1, SUB_MK);
    tc_alu(t, TC_OR, 1, 1, SUB_VAL);
    top = t->len;
    tc_alu(t, TC_XOR, 0, 1, SUB_SH);
    tc_cmpswap_w(t, 0, AD, 0);
    done = tc_jcc_placeholder(t, TC_JEQ, 0, 1);
    tc_alu(t, TC_XOR, 1, 1, 0);
    tc_alu(t, TC_ANDN, 1, 1, SUB_MK);
    failed = tc_jcci_placeholder(t, TC_JEQ, 1, 0);
    tc_alu(t, TC_ANDN, 1, 0, SUB_MK);
    tc_alu(t, TC_OR, 1, 1, SUB_VAL);
    again = tc_j_placeholder(t);
    br_back(F, again, top);
    br_land(F, done);
    br_land(F, failed);
    tc_dsync(t);
    tc_alu(t, TC_AND, 0, 0, SUB_MK);
    if (i->op == IR_CAS) {
        sub_out(F, 0, i->a, 1, i->size, i->sign);
        wr(F, i->dst, 0);
    } else {
        /* the flag from the lanes compared; then *b = the lane seen */
        tc_alu(t, TC_EQ, 1, 0, SUB_VAL);
        sub_out(F, 0, i->a, 1, i->size, 0);
        rda(F, i->b, AD2);
        tc_store(t, 0, AD2, 0, i->size);
        wr(F, i->dst, 1);
    }
}

static void gen_ins(struct tc_fn *F, int n)
{
    struct ir_func *fn = F->fn;
    struct ir_ins *i = &fn->ins[n];
    struct code *t = F->t;

    /* -g: a line-table row wherever the source line changes. */
    if (F->want_debug && fn->ins[n].line) {
        long line = fn->ins[n].line;
        struct ir_line *last = fn->nlines ? &fn->lines[fn->nlines - 1]
                                          : (struct ir_line *)0;
        if (last && last->off == t->len) {
            last->line = line;
        } else if (!last || last->line != line) {
            if (fn->nlines == fn->linecap) {
                fn->linecap = fn->linecap ? fn->linecap * 2 : 8;
                fn->lines = xrealloc(fn->lines, (size_t)fn->linecap *
                                     sizeof *fn->lines);
            }
            fn->lines[fn->nlines].off = t->len;
            fn->lines[fn->nlines].line = line;
            fn->nlines++;
        }
    }

    /* Floating point is a call, not an instruction. Only the ARITHMETIC
     * is flagged here: a move, a return or a call of a float carries its
     * bits through the integer paths below. */
    if (i->flt && (i->op == IR_ADD || i->op == IR_SUB || i->op == IR_MUL ||
                   i->op == IR_DIV || i->op == IR_MOD || i->op == IR_NEG ||
                   i->op == IR_CMP || i->op == IR_SQRT)) {
        const char *name = fp_binop_name(i->op, i->w);
        if (i->w != 4 && i->w != 8)
            tc_refuse(F, i, "a floating-point value of this width");
        if (name) {
            if (i->imm_b)
                tc_refuse(F, i, "a folded floating-point immediate");
            fp_args2(F, i);
            call_helper(F, name);
            fp_result(F, i->dst, i->w);
            return;
        }
        if (i->op == IR_NEG) {
            /* the sign bit, flipped: right for -0.0 and a NaN as well */
            if (i->w == 8) {
                rd64(F, i->a, A_LO, A_HI);
                tc_movh(t, B_LO, 0x8000);
                tc_alu(t, TC_XOR, A_HI, A_HI, B_LO);
                wr64(F, i->dst, A_LO, A_HI);
            } else {
                rd(F, i->a, ACC);
                tc_movh(t, TMP, 0x8000);
                tc_alu(t, TC_XOR, ACC, ACC, TMP);
                wr(F, i->dst, ACC);
            }
            return;
        }
        if (i->op == IR_CMP) {
            /* __ltdf2 and the rest answer with an int whose relation to
             * zero is the predicate's; unordered makes it false */
            fp_args2(F, i);
            call_helper(F, fp_cmp_name(i->pred, i->w));
            if (!cmp_imm_to_reg(F, i->pred, 1, 2, 0, ACC))
                internal_error("tricore: a compare with zero");
            wr(F, i->dst, ACC);
            return;
        }
        if (i->op == IR_SQRT)
            tc_refuse(F, i, "__builtin_sqrt (a libm routine here, not an "
                            "instruction)");
        tc_refuse(F, i, "this floating-point operation");
    }

    if (i->w > 8)
        tc_refuse(F, i, "a 128-bit value");
    /* (one and two bytes are sub_rmw/sub_cas: the word around them) */
    if ((i->op == IR_XCHG || i->op == IR_XADD || i->op == IR_ARMW ||
         i->op == IR_CAS || i->op == IR_CMPXCHG) && i->size > 4)
        tc_refuse(F, i, "an atomic wider than a register");
    if (i->op == IR_CAS16)
        tc_refuse(F, i, "a 16-byte atomic");
    if (i->op == IR_FRAMEADDR)
        tc_refuse(F, i, "__builtin_frame_address or "
                        "__builtin_return_address (TriCore code keeps no "
                        "frame-pointer chain; the return address is in the "
                        "context-save area)");

    /* The high word of a 64-bit value, shifted: one register. */
    if (i->op == IR_SHR && F->nshr && i->dst >= 0 && F->nshr[i->dst]) {
        int k = (int)i->imm - 32, d = wreg(F, i->dst, A_LO), hi;
        if (in_reg(F, i->a)) {
            hi = F->loc[i->a] + 1;
        } else {
            ld_sp(F, A_HI, sslot(F, i->a) + 4, 4, 1);
            hi = A_HI;
        }
        if (k)
            tc_alu_imm(t, i->sign ? TC_SHA : TC_SH, d, hi, -k);
        else if (d != hi)
            tc_mov(t, d, hi);
        wrote(F, i->dst, d);
        return;
    }
    {
        /* Does this instruction work on a value that needs a register
         * pair? Not `w == 8` everywhere: STVAR and STORE carry a size and
         * no w, LDVAR and LOAD say it in the map, and a copy that says
         * four bytes copies four (riscv/codegen.c, the same rule). */
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
            wide = i->w != 4 &&
                   ((i->dst >= 0 && F->wide[i->dst]) ||
                    (i->op == IR_MOV && i->a >= 0 && F->wide[i->a]));
            break;
        default: break;
        }
        if (wide && i->op != IR_CMP && i->op != IR_BRZ && i->op != IR_BRNZ &&
            i->op != IR_CALL && i->op != IR_RET &&
            i->op != IR_I2F && i->op != IR_F2I && i->op != IR_F2F) {
            if (i->op == IR_DIV || i->op == IR_MOD) {
                /* no 64-by-64 divide: lib/rt/int64.c, under libgcc's
                 * names, the operands in E4 and E6 */
                if (i->imm_b) {
                    args64x2(F, i->a, -1);
                    operand_b64(F, i, 6, 7);
                } else {
                    args64x2(F, i->a, i->b);
                }
                call_helper(F, i->op == IR_DIV
                            ? (i->sign ? "__divdi3" : "__udivdi3")
                            : (i->sign ? "__moddi3" : "__umoddi3"));
                wr64(F, i->dst, 2, 3);
                return;
            }
            if (i->op == IR_BSWAP) {
                /* each word reversed, and the words swapped */
                int al, ah;
                src64(F, i->a, A_LO, A_HI, &al, &ah);
                tc_alu_imm(t, TC_SH, B_LO, ah, 24);
                tc_extr(t, SCR, ah, 8, 8, 0);
                tc_insert(t, B_LO, B_LO, SCR, 16, 8);
                tc_extr(t, SCR, ah, 16, 8, 0);
                tc_insert(t, B_LO, B_LO, SCR, 8, 8);
                tc_extr(t, SCR, ah, 24, 8, 0);
                tc_insert(t, B_LO, B_LO, SCR, 0, 8);
                tc_alu_imm(t, TC_SH, B_HI, al, 24);
                tc_extr(t, SCR, al, 8, 8, 0);
                tc_insert(t, B_HI, B_HI, SCR, 16, 8);
                tc_extr(t, SCR, al, 16, 8, 0);
                tc_insert(t, B_HI, B_HI, SCR, 8, 8);
                tc_extr(t, SCR, al, 24, 8, 0);
                tc_insert(t, B_HI, B_HI, SCR, 0, 8);
                wr64(F, i->dst, B_LO, B_HI);
                return;
            }
            if (gen_ins64(F, n))
                return;
            tc_refuse(F, i, "this operation at 64 bits");
        }
        if (wide && (i->op == IR_CMP || i->op == IR_BRZ || i->op == IR_BRNZ))
            i->w = 8;         /* the cases below read `w` to pick the pair */
    }

    switch (i->op) {
    case IR_LABEL:
        F->label_off[i->label] = t->len;
        return;
    case IR_JMP:
        if (n + 1 < fn->nins && fn->ins[n + 1].op == IR_LABEL &&
            fn->ins[n + 1].label == i->label)
            return;
        jump_to(F, i->label);
        return;
    case IR_CONST: {
        int d = wreg(F, i->dst, ACC);
        tc_li(t, d, imm_val(i));
        wrote(F, i->dst, d);
        return;
    }
    case IR_BITCAST:
    case IR_MOV: {
        int src = rdr(F, i->a, ACC);
        int d = wreg(F, i->dst, ACC);
        if (src != d)
            tc_mov(t, d, src);
        wrote(F, i->dst, d);
        return;
    }

    case IR_ADD: case IR_SUB: case IR_MUL:
    case IR_AND: case IR_OR:  case IR_XOR: {
        /* (IR_DIV and IR_MOD sit between IR_MUL and IR_AND in the enum, so
         * this is a switch and not a table.) */
        int op = i->op == IR_ADD ? TC_ADD
               : i->op == IR_SUB ? TC_SUB
               : i->op == IR_AND ? TC_AND
               : i->op == IR_OR  ? TC_OR
               : i->op == IR_XOR ? TC_XOR
               : TC_MUL;
        int ra_ = rdr(F, i->a, ACC);
        int rd_ = wreg(F, i->dst, ACC);
        if (i->imm_b) {
            long long v = imm_val(i);
            if (i->op == IR_ADD || i->op == IR_SUB) {
                /* ADDI's field is SIGNED 16 bits; a subtraction adds -v */
                if (i->op == IR_SUB) v = -v;
                if (v >= -32768 && v <= 32767) {
                    tc_addi(t, rd_, ra_, v);
                    wrote(F, i->dst, rd_);
                    return;
                }
                if (!(v & 0xffff)) {
                    tc_addih(t, rd_, ra_,
                             (unsigned)((unsigned long long)v >> 16) & 0xffffu);
                    wrote(F, i->dst, rd_);
                    return;
                }
            } else if (i->op == IR_MUL) {
                if (tc_alu_imm_ok(TC_MUL, v)) {
                    tc_alu_imm(t, TC_MUL, rd_, ra_, v);
                    wrote(F, i->dst, rd_);
                    return;
                }
            } else {
                logic_half(F, op, rd_, ra_, (unsigned long)v);
                wrote(F, i->dst, rd_);
                return;
            }
        }
        {
            /* the second operand may not land in the destination before
             * the first is read: a scratch unless it has a home */
            int rb_ = (i->imm_b || !in_reg(F, i->b)) ? TMP : F->loc[i->b];
            if (rb_ == TMP) operand_b(F, i, TMP);
            tc_alu(t, op, rd_, ra_, rb_);
        }
        wrote(F, i->dst, rd_);
        return;
    }
    case IR_DIV: case IR_MOD: {
        /* DIV: the quotient in D0 and the remainder in D1, at once */
        int ra_ = rdr(F, i->a, B_LO);
        int rb_ = (i->imm_b || !in_reg(F, i->b)) ? B_HI : F->loc[i->b];
        if (rb_ == B_HI) operand_b(F, i, B_HI);
        tc_div(t, 0, ra_, rb_, i->sign);
        wrote(F, i->dst, i->op == IR_DIV ? 0 : 1);
        return;
    }
    case IR_SHL: case IR_SHR: {
        int ra_ = rdr(F, i->a, ACC);
        int d;
        int op = i->op == IR_SHL || !i->sign ? TC_SH : TC_SHA;
        if (i->imm_b && i->imm >= 0 && i->imm < 32) {
            d = wreg(F, i->dst, ACC);
            if (i->imm == 0) {
                if (d != ra_) tc_mov(t, d, ra_);
            } else {
                tc_alu_imm(t, op, d, ra_,
                           i->op == IR_SHL ? i->imm : -i->imm);
            }
        } else {
            int rb_ = (i->imm_b || !in_reg(F, i->b)) ? TMP : F->loc[i->b];
            if (rb_ == TMP) operand_b(F, i, TMP);
            d = wreg(F, i->dst, ACC);
            if (i->op == IR_SHR) {
                tc_alu_imm(t, TC_RSUB, SCR, rb_, 0);    /* right: -count */
                rb_ = SCR;
            }
            tc_alu(t, op, d, ra_, rb_);
        }
        wrote(F, i->dst, d);
        return;
    }
    case IR_NEG: {
        int ra_ = rdr(F, i->a, ACC);
        int d = wreg(F, i->dst, ACC);
        tc_alu_imm(t, TC_RSUB, d, ra_, 0);
        wrote(F, i->dst, d);
        return;
    }
    case IR_BNOT: {
        int ra_ = rdr(F, i->a, ACC);
        int d = wreg(F, i->dst, ACC);
        tc_alu_imm(t, TC_NOR, d, ra_, 0);
        wrote(F, i->dst, d);
        return;
    }

    case IR_CMP: {
        struct ir_ins *nx = n + 1 < fn->nins ? &fn->ins[n + 1]
                                             : (struct ir_ins *)0;
        int fuse = nx && (nx->op == IR_BRZ || nx->op == IR_BRNZ) &&
                   nx->a == i->dst && F->usecnt && F->usecnt[i->dst] == 1;
        if (i->w == 8) {
            cmp64(F, i, i->pred, i->sign);
            if (fuse) {
                branch_ri(F, nx->op == IR_BRZ ? TC_JEQ : TC_JNE, ACC, 0,
                          nx->label);
                F->skip_next = 1;
                return;
            }
            wr(F, i->dst, ACC);
            return;
        }
        if (fuse && !(nx->w == 8)) {
            /* One branch where the unfused form is a compare and a test.
             * GT and LE swap their operands, or against a constant are
             * GE and LT of the next one. */
            int ra_ = rdr(F, i->a, ACC);
            int sw, cond = pred_cond(i->pred, i->sign, &sw);
            if (i->imm_b) {
                long long k = imm_val(i);
                int ok = 1;
                if (sw) {
                    /* x > k is x >= k + 1, x <= k is x < k + 1 */
                    if (i->sign ? k == 0x7fffffffLL
                                : (k & 0xffffffffLL) == 0xffffffffLL)
                        ok = 0;
                    else
                        k++;
                    cond = cond == TC_JLT ? TC_JGE : cond == TC_JLTU ? TC_JGEU
                         : cond == TC_JGE ? TC_JLT : TC_JLTU;
                }
                if (!i->sign)
                    k &= 0xffffffffLL;
                if (ok && tc_jcci_ok(cond, k)) {
                    if (nx->op == IR_BRZ)
                        cond = inverse_cond(cond);
                    branch_ri(F, cond, ra_, k, nx->label);
                    F->skip_next = 1;
                    return;
                }
            }
            {
                int rb_ = (i->imm_b || !in_reg(F, i->b)) ? TMP : F->loc[i->b];
                int x, y;
                cond = pred_cond(i->pred, i->sign, &sw);
                if (rb_ == TMP) operand_b(F, i, TMP);
                x = sw ? rb_ : ra_;
                y = sw ? ra_ : rb_;
                if (nx->op == IR_BRZ)
                    cond = inverse_cond(cond);
                branch_rr(F, cond, x, y, nx->label);
                F->skip_next = 1;
                return;
            }
        }
        {
            int ra_ = rdr(F, i->a, ACC);
            int rb_ = (i->imm_b || !in_reg(F, i->b)) ? TMP : F->loc[i->b];
            int d = wreg(F, i->dst, ACC);
            if (i->imm_b && cmp_imm_to_reg(F, i->pred, i->sign, ra_,
                                           imm_val(i), d)) {
                wrote(F, i->dst, d);
                return;
            }
            if (rb_ == TMP) operand_b(F, i, TMP);
            cmp_to_reg(F, i->pred, i->sign, ra_, rb_, d);
            wrote(F, i->dst, d);
        }
        return;
    }

    case IR_SELECT: {
        /* dst = a ? b : c, with SEL on the condition -- tested at ITS
         * width, `size` */
        int cond, rb_, rc_, d;
        if (i->size == 8) {
            int al, ah;
            src64(F, i->a, A_LO, A_HI, &al, &ah);
            tc_alu(t, TC_OR, SCR, al, ah);
            cond = SCR;
        } else {
            cond = rdr(F, i->a, SCR);
        }
        rb_ = rdr(F, i->b, TMP);
        rc_ = rdr(F, i->c, ACC);
        d = wreg(F, i->dst, ACC);
        tc_sel(t, d, cond, rb_, rc_);
        wrote(F, i->dst, d);
        return;
    }

    case IR_BRZ: case IR_BRNZ: {
        int r;
        if (i->w == 8) {
            int al, ah;
            src64(F, i->a, A_LO, A_HI, &al, &ah);
            tc_alu(t, TC_OR, SCR, al, ah);
            r = SCR;
        } else {
            r = rdr(F, i->a, A_LO);
        }
        branch_ri(F, i->op == IR_BRZ ? TC_JEQ : TC_JNE, r, 0, i->label);
        return;
    }

    /* A local may live in a register; these two are the only places that
     * name its slot directly, so they are the two that ask. */
    case IR_LDVAR: {
        int d = wreg(F, i->dst, ACC);
        if (in_reg(F, i->a)) {
            if (tc_ldvar_plain(i->size, i->sign, i->w)) {
                if (d != F->loc[i->a]) tc_mov(t, d, F->loc[i->a]);
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
            /* a narrowing store sign-extends; an unsigned read extends
             * for itself (ldvar_plain is only the full word) */
            if (i->size >= 4) {
                if (F->loc[i->dst] != src) tc_mov(t, F->loc[i->dst], src);
            } else {
                ext_reg(F, F->loc[i->dst], src, i->size, 1);
            }
        } else {
            st_sp(F, src, sslot(F, i->dst), i->size);
        }
        return;
    }
    case IR_LOAD: {                    /* memoff: ra_fold_memoff's, or 0 */
        int d = wreg(F, i->dst, ACC);
        rda(F, i->a, AD);
        ld_any(F, d, AD, i->memoff, i->size, i->sign, i->natural);
        wrote(F, i->dst, d);
        return;
    }
    case IR_STORE: {
        int val;
        rda(F, i->a, AD);
        val = rdr(F, i->b, ACC);
        st_any(F, val, AD, i->memoff, i->size, i->natural);
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
    /* An address is MOVH + ADDI, R_TRICORE_HIADJ then R_TRICORE_LO,
     * against the symbol (or .rodata for a string). Absolute. */
    case IR_STRADDR: {
        int d = wreg(F, i->dst, ACC);
        int at = abs_pair(F, d);
        note_str(F->st, at, i->label, RK_TRICORE_HI);
        note_str(F->st, at + 4, i->label, RK_TRICORE_LO);
        wrote(F, i->dst, d);
        return;
    }
    case IR_GADDR: {
        int d = wreg(F, i->dst, ACC);
        int at = abs_pair(F, d);
        note_glob(F->st, at, i->glob, RK_TRICORE_HI);
        note_glob(F->st, at + 4, i->glob, RK_TRICORE_LO);
        wrote(F, i->dst, d);
        return;
    }
    case IR_FADDR: {
        int d = wreg(F, i->dst, ACC);
        int at = abs_pair(F, d);
        note_fn(F->st, at, i->callee, RK_TRICORE_HI);
        note_fn(F->st, at + 4, i->callee, RK_TRICORE_LO);
        wrote(F, i->dst, d);
        return;
    }

    case IR_MEMCPY: case IR_MEMZERO:
        rda(F, i->a, AD);
        if (i->op == IR_MEMCPY)
            rda(F, i->b, AD2);
        copy_block(F, i->op == IR_MEMCPY, i->size, i->natural >= 4);
        return;

    case IR_CALL:
        gen_call(F, n);
        return;

    case IR_RET:
        if (i->a >= 0) {
            if (fn->ret_abi.is_struct) {
                if (fn_sret(fn)) {
                    /* through the caller's buffer, whose address the
                     * prologue kept; the address comes back in A2 */
                    lda_sp(F, AD, F->sret_slot);
                    rda(F, i->a, AD2);
                    copy_block(F, 1, fn->ret_abi.size, 0);
                    lda_sp(F, 2, F->sret_slot);
                } else {
                    /* a small one in D2, or E2 */
                    rda(F, i->a, AD);
                    pack_word(F, 2, AD, 0, fn->ret_abi.size < 4
                                           ? fn->ret_abi.size : 4);
                    if (fn->ret_abi.size > 4)
                        pack_word(F, 3, AD, 4, fn->ret_abi.size - 4);
                }
            } else if (F->wide[i->a]) {
                rd64(F, i->a, 2, 3);
            } else if (ty_is_ptr(fn->ret_abi.ty)) {
                rda(F, i->a, 2);
            } else {
                rd(F, i->a, 2);
            }
        }
        {
            int m = n + 1;
            while (m < fn->nins && fn->ins[m].op == IR_LABEL)
                m++;
            if (m < fn->nins)
                jump_to(F, fn->nlabels);
        }
        return;

    case IR_UD2:
        /* an illegal instruction: the IOPC trap, never a fall-through */
        tc_illegal(t);
        return;
    case IR_FENCE:
        tc_dsync(t);
        return;

    case IR_BSWAP: {
        /* the bytes moved one at a time: no byte reverse in TriCore 1.6.1 */
        int ra_ = rdr(F, i->a, ACC);
        int d = TMP;
        if (i->size == 2) {
            tc_extr(t, d, ra_, 8, 8, 0);
            tc_extr(t, SCR, ra_, 0, 8, 0);
            tc_insert(t, d, d, SCR, 8, 8);
        } else {
            tc_alu_imm(t, TC_SH, d, ra_, 24);
            tc_extr(t, SCR, ra_, 8, 8, 0);
            tc_insert(t, d, d, SCR, 16, 8);
            tc_extr(t, SCR, ra_, 16, 8, 0);
            tc_insert(t, d, d, SCR, 8, 8);
            tc_extr(t, SCR, ra_, 24, 8, 0);
            tc_insert(t, d, d, SCR, 0, 8);
        }
        wr(F, i->dst, d);
        return;
    }

    case IR_VA_START:
        /* va_list is a bare pointer at the first unnamed word, above the
         * named stack arguments in the caller's outgoing area */
        rda(F, i->a, AD);
        addr_sp(F, ACC, F->va_first);
        tc_store(t, ACC, AD, 0, 4);
        return;

    case IR_I2F: case IR_F2I: case IR_F2F: {
        int src_w = i->size;
        if (i->op == IR_F2F && src_w == i->w) {
            if (src_w == 8) {
                rd64(F, i->a, A_LO, A_HI);
                wr64(F, i->dst, A_LO, A_HI);
            } else {
                rd(F, i->a, ACC);
                wr(F, i->dst, ACC);
            }
            return;
        }
        if (src_w > 8 || i->w > 8)
            tc_refuse(F, i, "a conversion of a 128-bit value");
        if (i->op == IR_I2F && src_w == 8 && i->a >= 0 && !F->wide[i->a]) {
            /* a 32-bit value asked for as 64: zero-extended (only an
             * unsigned one is ever widened this way) */
            rd(F, i->a, 4);
            tc_mov_imm(t, 5, 0);
        } else if (src_w == 8) {
            args64x2(F, i->a, -1);
        } else {
            rd(F, i->a, 4);
        }
        call_helper(F, cvt_name(i));
        if (i->dst >= 0) {
            if (F->wide[i->dst])
                wr64(F, i->dst, 2, 3);
            else
                wr(F, i->dst, 2);
        }
        return;
    }

    case IR_ASM: {
        /* Extended asm, assembled in irgen (tricore/irgen.c) against the
         * vocabulary in tricore/asm.c. Nothing is live in a register
         * across one -- the allocator excludes every vreg whose range
         * spans an IR_ASM -- so operands' registers may be loaded freely.
         * An operand register is a data register 0-15 or an address
         * register 16 + n; an output's lvalue address goes through an
         * address register no operand uses. */
        struct ir_asm *ia = i->asm_ir;
        int used[32] = { 0 };
        static const int scr_pool[] = { 2, 3, 4, 5, 6, 7 };
        int scr = -1;
        for (int k = 0; k < ia->nin; k++) used[ia->in[k].reg] = 1;
        for (int k = 0; k < ia->nout; k++) used[ia->out[k].reg] = 1;
        for (unsigned k = 0; k < sizeof scr_pool / sizeof scr_pool[0]; k++)
            if (!used[16 + scr_pool[k]]) { scr = scr_pool[k]; break; }
        if (scr < 0 && ia->nout > 0)
            tc_refuse(F, i, "an asm with no address register left around it");
        for (int k = 0; k < ia->nout; k++)
            if (!ia->out[k].mem && (ia->out[k].size > 4 ||
                                    (ia->out[k].reg >= 16 &&
                                     ia->out[k].size != 4)))
                tc_refuse(F, i, "an asm output wider than a register, or a "
                                "narrow one in an address register");
        for (int k = 0; k < ia->nout; k++) {
            int r = ia->out[k].reg;
            if (!ia->out[k].inout || ia->out[k].mem)
                continue;
            rda(F, ia->out[k].temp, scr);
            if (r >= 16) tc_ld_a(t, r - 16, scr, 0);
            else         tc_load(t, r, scr, 0, ia->out[k].size, 0);
        }
        for (int k = 0; k < ia->nin; k++) {
            int r = ia->in[k].reg;
            if (r >= 16) rda(F, ia->in[k].temp, r - 16);
            else         rd(F, ia->in[k].temp, r);
        }
        for (int k = 0; k < ia->nout; k++)
            if (ia->out[k].mem) {
                if (ia->out[k].reg < 16)
                    tc_refuse(F, i, "an \"m\" asm operand in a data register");
                rda(F, ia->out[k].temp, ia->out[k].reg - 16);
            }
        for (int k = 0; k < ia->codelen; k++)
            code_byte(t, ia->code[k]);
        for (int k = 0; k < ia->nout; k++) {
            int r = ia->out[k].reg;
            if (ia->out[k].mem)
                continue;
            rda(F, ia->out[k].temp, scr);
            if (r >= 16) tc_st_a(t, r - 16, scr, 0);
            else         tc_store(t, r, scr, 0, ia->out[k].size);
        }
        return;
    }

    /* ---- atomics: SWAP.W, and CMPSWAP.W loops, bracketed by dsync ---- */
    case IR_XCHG:
        if (i->size == 1 || i->size == 2) {
            sub_rmw(F, i);
            return;
        }
        rda(F, i->a, AD);
        rd(F, i->b, ACC);
        tc_dsync(t);
        tc_swap_w(t, ACC, AD, 0);
        tc_dsync(t);
        wr(F, i->dst, ACC);
        return;
    case IR_XADD: case IR_ARMW: {
        /*   retry: ld.w   d1, [a12]        (the value seen)
         *          d0 = d1 OP val
         *          cmpswap.w [a12], e0     (stored if still d1; d0 = seen)
         *          jne    d0, d1, retry
         * and the old value is d1. */
        int top, again;
        if (i->size == 1 || i->size == 2) {
            sub_rmw(F, i);
            return;
        }
        rda(F, i->a, AD);
        rd(F, i->b, B_LO);
        tc_dsync(t);
        top = t->len;
        tc_load(t, 1, AD, 0, 4, 0);
        if (i->op == IR_XADD) {
            tc_alu(t, TC_ADD, 0, 1, B_LO);
        } else {
            switch ((int)i->imm) {
            case '&': tc_alu(t, TC_AND, 0, 1, B_LO); break;
            case '|': tc_alu(t, TC_OR, 0, 1, B_LO); break;
            case '^': tc_alu(t, TC_XOR, 0, 1, B_LO); break;
            default:  tc_alu(t, TC_NAND, 0, 1, B_LO); break;   /* nand */
            }
        }
        tc_cmpswap_w(t, 0, AD, 0);
        again = tc_jcc_placeholder(t, TC_JNE, 0, 1);
        br_back(F, again, top);
        tc_dsync(t);
        wr(F, i->dst, 1);
        return;
    }
    case IR_CAS:
        /* dst = the value seen, whether or not the swap happened */
        if (i->size == 1 || i->size == 2) {
            sub_cas(F, i);
            return;
        }
        rd(F, i->c, 0);
        rd(F, i->b, 1);
        rda(F, i->a, AD);
        tc_dsync(t);
        tc_cmpswap_w(t, 0, AD, 0);
        tc_dsync(t);
        wr(F, i->dst, 0);
        return;
    case IR_CMPXCHG:
        /* the expected value through the pointer in b, written back with
         * what was seen; dst = whether it was the expected one */
        if (i->size == 1 || i->size == 2) {
            sub_cas(F, i);
            return;
        }
        rd(F, i->c, 0);
        rda(F, i->a, AD);
        rda(F, i->b, AD2);
        tc_load(t, 1, AD2, 0, 4, 0);
        tc_dsync(t);
        tc_cmpswap_w(t, 0, AD, 0);
        tc_dsync(t);
        tc_store(t, 0, AD2, 0, 4);
        tc_alu(t, TC_EQ, B_LO, 0, 1);
        wr(F, i->dst, B_LO);
        return;

    case IR_ALLOCA: {
        /* A fresh 16-aligned block: A10 moves down by the size rounded to
         * 16 and then to a multiple of 16. The block sits above the
         * outgoing area -- a multiple of 16 in such a function -- which
         * moves down with A10; the frame is addressed from A14. */
        int d;
        rd(F, i->a, ACC);
        tc_addi(t, ACC, ACC, 15);
        tc_insert_imm(t, ACC, ACC, 0, 0, 4);
        tc_mov_d(t, TMP, TC_SP);
        tc_alu(t, TC_SUB, TMP, TMP, ACC);
        tc_insert_imm(t, TMP, TMP, 0, 0, 4);
        tc_mov_a(t, TC_SP, TMP);
        lea_off(F, AD, TC_SP, F->out_bytes);
        d = wreg(F, i->dst, ACC);
        tc_mov_d(t, d, AD);
        wrote(F, i->dst, d);
        return;
    }
    case IR_SPSAVE: {
        int d = wreg(F, i->dst, ACC);
        tc_mov_d(t, d, TC_SP);
        wrote(F, i->dst, d);
        return;
    }
    case IR_SPRESTORE:
        tc_mov_a(t, TC_SP, rdr(F, i->a, ACC));
        return;
    case IR_SWITCH:
        tc_refuse(F, i, "a jump table");
        return;
    case IR_LABELADDR: case IR_IGOTO:
        tc_refuse(F, i, "a computed goto");
        return;
    default:
        tc_refuse(F, i, "this operation");
    }
}

/* Copy `size` bytes from [AD2] to [AD] (copy) or zero them (!copy).
 * Word by word when both ends are known to be word-aligned (`aligned`),
 * else a byte at a time. Straight-line up to 64 bytes, a loop beyond (the
 * pointers stepped with LEA, the count in D3). AD and AD2 are scratch and
 * may be moved; so are ACC and B_HI. */
static void copy_block(struct tc_fn *F, int copy, long size, int aligned)
{
    struct code *t = F->t;
    int step = aligned ? 4 : 1;
    long k, body = size / step * step;
    if (!copy)
        tc_mov_imm(t, ACC, 0);
    if (size > 64 && body / step > 1) {
        int top, again;
        tc_li(t, B_HI, body / step);
        top = t->len;
        if (copy)
            tc_load(t, ACC, AD2, 0, step, 0);
        tc_store(t, ACC, AD, 0, step);
        if (copy)
            tc_lea(t, AD2, AD2, step);
        tc_lea(t, AD, AD, step);
        tc_addi(t, B_HI, B_HI, -1);
        again = br_placei(F, TC_JNE, B_HI, 0);
        br_back(F, again, top);
        k = 0;
        size -= body;
    } else {
        for (k = 0; k + step <= size; k += step) {
            if (copy)
                tc_load(t, ACC, AD2, k, step, 0);
            tc_store(t, ACC, AD, k, step);
        }
    }
    for (; k < size; k++) {
        if (copy)
            tc_load(t, ACC, AD2, k, 1, 0);
        tc_store(t, ACC, AD, k, 1);
    }
}

/* ---- register pairs ------------------------------------------------------
 *
 * As at RV32 and MIPS: the shared allocator is run first for the 64-bit
 * values alone over a pool of PAIRS (each named by its low register), and
 * the ordinary pass then treats each pair's registers as taken over that
 * value's live range (ra_reserve). */
static const struct ra_target TC_PAIR_RA = {
    tc_pair_pool_for, tc_callee_saved, tc_ldvar_plain,
    1, 1, 1,
    tc_op_calls_helper,
    0,
    tc_pair_hints,
    NULL, NULL,
    1,
    NULL, NULL,
    0,
    0,
    0               /* asm_in_reg */
};

static void tc_pair_hints(const struct ir_func *fn, int *hint)
{
    struct argplace pl[MAX_PARAMS];
    if (fn->src && fn->nparams) {
        place_args(fn->param_abi, fn->nparams, fn_sret(fn),
                   fn->is_varargs ? fn->nparams : -1, pl);
        for (int p = 0; p < fn->nparams && p < fn->nvregs; p++)
            if (pl[p].kind == PL_E && !fn->param_abi[p].is_struct)
                hint[p] = pl[p].reg;
    }
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        if (i->op != IR_CALL && tc_op_calls_helper(i) && i->w == 8) {
            if (i->a >= 0 && i->a < fn->nvregs && hint[i->a] < 0)
                hint[i->a] = 4;
            if (!i->imm_b && i->b >= 0 && i->b < fn->nvregs &&
                hint[i->b] < 0)
                hint[i->b] = 6;
        }
        if (i->op != IR_CALL)
            continue;
        place_args(i->argv, i->nargs, call_sret(i), call_nfixed(i), pl);
        for (int k = 0; k < i->nargs; k++) {
            const struct ir_arg *a = &i->argv[k];
            if (pl[k].kind == PL_E && !a->is_struct && a->vreg >= 0 &&
                a->vreg < fn->nvregs)
                hint[a->vreg] = pl[k].reg;
        }
    }
}

static struct ra_range *g_tc_res;
static int g_tc_nres, g_tc_capres;
static void reserve_pairs(struct ir_func *fn, const int *loc)
{
    int nv = fn->nvregs;
    int *first = xmalloc((size_t)(nv ? nv : 1) * sizeof *first);
    int *last = xmalloc((size_t)(nv ? nv : 1) * sizeof *last);
    ra_live_ranges(fn, first, last);
    g_tc_nres = 0;
    for (int v = 0; v < nv; v++) {
        if (loc[v] < 0 || first[v] < 0) continue;
        if (g_tc_nres + 2 > g_tc_capres) {
            g_tc_capres = g_tc_capres ? g_tc_capres * 2 : 16;
            g_tc_res = xrealloc(g_tc_res,
                                (size_t)g_tc_capres * sizeof *g_tc_res);
        }
        for (int h = 0; h < 2; h++) {
            g_tc_res[g_tc_nres].reg = loc[v] + h;
            g_tc_res[g_tc_nres].first = first[v];
            g_tc_res[g_tc_nres].last = last[v];
            g_tc_res[g_tc_nres].born = 0;
            g_tc_nres++;
        }
    }
    ra_reserve(g_tc_res, g_tc_nres);
    free(first); free(last);
}

static int *pair_alloc(struct ir_func *fn, struct tc_fn *F, const char *pin)
{
    int nv = fn->nvregs, any = 0;
    char *x = xcalloc((size_t)(nv ? nv : 1), 1);
    int used[RA_MAXPOOL], nused = 0;
    int *loc;

    for (int v = 0; v < nv; v++) {
        x[v] = !F->wide[v] || (pin && pin[v]);
        any |= !x[v];
    }
    /* Kept in memory: a local read or written narrower than itself, and
     * an argument that is not wholly in a register pair. */
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        if ((i->op == IR_LDVAR && i->a >= 0 && i->a < nv &&
             F->wide[i->a] && i->w != 8) ||
            (i->op == IR_STVAR && i->dst >= 0 && i->dst < nv &&
             F->wide[i->dst] && i->w != 8))
            x[i->op == IR_LDVAR ? i->a : i->dst] = 1;
        if (i->op == IR_CALL) {
            struct argplace pl[MAX_PARAMS];
            place_args(i->argv, i->nargs, call_sret(i), call_nfixed(i), pl);
            for (int k = 0; k < i->nargs; k++) {
                const struct ir_arg *a = &i->argv[k];
                if (a->size > 4 && pl[k].kind != PL_E && a->vreg >= 0 &&
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
    loc = ra_allocate(fn, &TC_PAIR_RA, NULL, x, used, &nused);
    free(x);
    reserve_pairs(fn, loc);
    for (int k = 0; k < nused && k < TC_NPAIRS; k++)
        F->pair_used[F->npair++] = used[k];
    return loc;
}

/* ---- the prologue's parameters -------------------------------------------
 *
 * Each register parameter is an edge of one parallel move into wherever
 * the allocator put it (D15 breaks a cycle; an address register is
 * AREG(n) there, so a pointer's mov.d is an edge like any other); each
 * stack parameter a load deferred until after it, so nothing overwrites
 * an incoming argument another parameter has not read. A struct
 * parameter goes to its slot: its words from D or E registers, its bytes
 * from the stack, or copied from the caller's copy when it came by
 * reference. */
static void gen_params(struct tc_fn *F)
{
    struct ir_func *fn = F->fn;
    struct code *t = F->t;
    struct argplace pl[MAX_PARAMS];
    long base = F->frame;             /* the caller's outgoing area */
    int pmv_dst[RA_MAXPOOL * 2], pmv_src[RA_MAXPOOL * 2], npmv = 0;
    int pstk_reg[RA_MAXPOOL * 2]; long pstk_off[RA_MAXPOOL * 2];
    int npstk = 0;
    long blk;

    if (F->sret_slot >= 0)
        sta_sp(F, 4, F->sret_slot);
    blk = place_args(fn->param_abi, fn->nparams, fn_sret(fn),
                     fn->is_varargs ? fn->nparams : -1, pl);
    for (int i = 0; i < fn->nparams; i++) {
        struct ir_arg *a = &fn->param_abi[i];
        struct argplace *p = &pl[i];
        if (p->byref) {
            /* the caller's copy, into this parameter's slot */
            if (p->kind == PL_A)
                tc_mov_aa(t, AD2, p->reg);
            else
                lda_sp(F, AD2, base + p->stk);
            lea_off(F, AD, F->fb, sslot(F, i));
            copy_block(F, 1, a->size, 0);
            continue;
        }
        if (a->is_struct) {
            if (p->kind == PL_D || p->kind == PL_E) {
                int nw = p->kind == PL_E ? 2 : 1;
                for (int q = 0; q < nw; q++) {
                    long left = a->size - 4L * q;
                    long off = sslot(F, i) + 4L * q;
                    int r = p->reg + q;
                    if (left >= 4) {
                        st_sp(F, r, off, 4);
                    } else {
                        tc_mov(t, SCR, r);
                        for (long b = 0; b < left; b++) {
                            if (b) tc_alu_imm(t, TC_SH, SCR, SCR, -8);
                            st_sp(F, SCR, off + b, 1);
                        }
                    }
                }
            } else {
                lea_off(F, AD2, F->fb, base + p->stk);
                lea_off(F, AD, F->fb, sslot(F, i));
                copy_block(F, 1, a->size, 0);
            }
            continue;
        }
        if (p->kind == PL_A) {
            if (in_reg(F, i)) {
                pmv_dst[npmv] = F->loc[i];
                pmv_src[npmv] = AREG(p->reg);
                npmv++;
            } else if (F->slot[i] >= 0) {
                sta_sp(F, p->reg, sslot(F, i));
            }
            continue;
        }
        if (p->kind == PL_E) {
            if (in_reg(F, i)) {
                for (int q = 0; q < 2; q++) {
                    pmv_dst[npmv] = F->loc[i] + q;
                    pmv_src[npmv] = p->reg + q;
                    npmv++;
                }
            } else if (F->slot[i] >= 0) {
                st_sp(F, p->reg, sslot(F, i), 4);
                st_sp(F, p->reg + 1, sslot(F, i) + 4, 4);
            }
            continue;
        }
        if (p->kind == PL_D) {
            if (in_reg(F, i)) {
                pmv_dst[npmv] = F->loc[i];
                pmv_src[npmv] = p->reg;
                npmv++;
            } else if (F->slot[i] >= 0) {
                st_sp(F, p->reg, sslot(F, i), 4);
            }
            continue;
        }
        /* on the stack */
        for (int q = 0; q < p->words && q < 2; q++) {
            if (in_reg(F, i)) {
                pstk_reg[npstk] = F->loc[i] + q;
                pstk_off[npstk] = base + p->stk + 4L * q;
                npstk++;
            } else if (F->slot[i] >= 0) {
                ld_sp(F, SCR, base + p->stk + 4L * q, 4, 1);
                st_sp(F, SCR, sslot(F, i) + 4L * q, 4);
            }
        }
    }
    if (npmv) {
        int od[RA_MAXPOOL * 4], os[RA_MAXPOOL * 4];
        int m = ra_parallel_move(pmv_dst, pmv_src, npmv, SCR, od, os,
                                 (int)(sizeof od / sizeof od[0]));
        if (m < 0)
            internal_error("tricore: %s: the prologue's parameter "
                           "placement is not a well-formed move", fn->name);
        for (int k = 0; k < m; k++)
            mvx(F, od[k], os[k]);
    }
    for (int k = 0; k < npstk; k++)
        ld_sp(F, pstk_reg[k], pstk_off[k], 4, 1);
    /* where the first unnamed argument is: after the named stack words */
    if (fn->is_varargs)
        F->va_first = F->frame + blk;
}

/* ---- one function --------------------------------------------------------- */

static void gen_func(struct ir_func *fn, struct code *t, struct tc_sites *st,
                     int want_debug)
{
    struct func *f = fn->src;
    struct tc_fn F;
    int i;

    memset(&F, 0, sizeof F);
    F.fn = fn; F.t = t; F.st = st;
    F.want_debug = want_debug;
    F.wide = wide_map(fn);
    F.nshr = ra_narrow_hishift(fn);
    for (int v = 0; v < fn->nvregs; v++)
        if (F.nshr[v]) F.wide[v] = 0;
    F.fb = TC_SP;
    if (g_tc_regalloc) {
        /* Under -g a source variable stays in its frame slot, so its
         * DW_AT_location is true (regalloc.h). */
        char *pin = want_debug ? ra_debug_pin_vars(fn) : (char *)0;
        int *pair = g_tc_pairs ? pair_alloc(fn, &F, pin) : NULL;
        F.loc = ra_allocate(fn, &TC_RATGT, F.wide, pin, F.used_callee,
                            &F.nsave);
        g_tc_taken = 0;
        if (pair) {
            for (int v = 0; v < fn->nvregs; v++)
                if (pair[v] >= 0) F.loc[v] = pair[v];
            free(pair);
        }
        free(pin);
        if (fn->nvregs) {
            F.usecnt = xmalloc((size_t)fn->nvregs * sizeof *F.usecnt);
            ra_count_vreg_uses(fn, F.usecnt);
        }
        /* EMBCC_TC_RA_MAX=N leaves only the first N vregs in registers
         * -- always correct -- so a miscompile that comes and goes with N
         * names the value whose allocation is wrong. */
        {
            const char *lim = getenv("EMBCC_TC_RA_MAX");
            if (lim) {
                int n = atoi(lim);
                for (int v = n; v < fn->nvregs; v++)
                    F.loc[v] = -1;
            }
        }
    }
    F.tail = NULL;
    if (g_tc_regalloc && !want_debug)
        for (i = 0; i < fn->nins; i++)
            if (tc_tail_ok(&F, i)) {
                if (!F.tail)
                    F.tail = xcalloc((size_t)fn->nins, 1);
                F.tail[i] = 1;
            }
    layout(&F);

    F.label_off = xmalloc((size_t)(fn->nlabels + 1) * sizeof *F.label_off);
    for (i = 0; i <= fn->nlabels; i++)
        F.label_off[i] = -1;

    f->code_align = 2;
    if (want_debug) {
        int nv = fn->nvars ? fn->nvars : 1;
        fn->var_off = xmalloc((size_t)nv * sizeof *fn->var_off);
        for (int v = 0; v < fn->nvars; v++)
            fn->var_off[v] = (int)F.slot[v];
    }
    /* BRANCH RELAXATION: every branch is tried in its short form, and one
     * that does not reach is given the long form and the function
     * generated again. Code only grows, so what reached on one attempt
     * may stop reaching on the next, and the loop runs until nothing new
     * fails. */
    {
    int len0 = t->len, nl0 = fn->nlines;
    int se0 = F.st->next, ss0 = F.st->nstr, sg0 = F.st->ng, sf0 = F.st->nf;
    char *longb = NULL;
    int nlongb = 0;
    for (;;) {
    int nfail = 0;
    t->len = len0;
    fn->nlines = nl0;
    F.st->next = se0; F.st->nstr = ss0; F.st->ng = sg0; F.st->nf = sf0;
    F.nfix = 0;
    for (i = 0; i <= fn->nlabels; i++)
        F.label_off[i] = -1;
    F.skip_next = 0;
    F.fb = TC_SP;
    F.longb = longb;
    F.nlongb = nlongb;
    f->code_off = t->len;

    /* The prologue: A10 down by the frame. No register is saved -- CALL
     * saved the upper context -- and the return address stays in A11. */
    if (F.frame)
        lea_off(&F, TC_SP, TC_SP, -F.frame);
    if (fn->has_alloca) {
        tc_mov_aa(t, AFB, TC_SP);
        F.fb = AFB;
    }
    gen_params(&F);

    {
        int tail_end = 0;
        for (i = 0; i < fn->nins; i++) {
            int was_tail = F.tail && F.tail[i];
            gen_ins(&F, i);
            if (F.skip_next) {
                F.skip_next = 0;
                i++;
            }
            tail_end = i == fn->nins - 1 && was_tail;
        }

        /* The epilogue: RET restores A10 with the rest of the upper
         * context -- unless the body ended in a tail call and no IR_RET
         * jumps here. */
        F.label_off[fn->nlabels] = t->len;
        for (i = 0; tail_end && i < F.nfix; i++)
            if (F.fix[i].label == fn->nlabels)
                tail_end = 0;
        if (!tail_end)
            tc_ret(t);
    }

    for (i = 0; i < F.nfix; i++) {
        int target = F.label_off[F.fix[i].label];
        if (target < 0)
            internal_error("tricore: label %d of %s was never placed",
                           F.fix[i].label, fn->name);
        if (!tc_patch(t, F.fix[i].at, target)) {
            if (F.fix[i].kind == FX_J)
                tc_refuse(&F, NULL, "a jump beyond 16 MiB");
            if (nlongb < F.nfix) {
                longb = xrealloc(longb, (size_t)F.nfix);
                memset(longb + nlongb, 0, (size_t)(F.nfix - nlongb));
                nlongb = F.nfix;
            }
            if (longb[i])
                internal_error("tricore: %s: a long branch was patched as "
                               "a short one", fn->name);
            longb[i] = 1;
            nfail++;
        }
    }
    if (!nfail)
        break;
    }                                   /* the attempts */
    free(longb);
    }

    f->code_len = t->len - f->code_off;
    f->stack_bytes = (int)F.frame;
    free(F.usecnt);
    free(F.tail);
    free(F.slot);
    free(F.label_off);
    free(F.fix);
    free(F.wide);
    free(F.nshr);
    free(F.loc);
}

/* With the allocator on, a function is generated with the pair pass and
 * without it, and the shorter is kept (RV32's arrangement, for the same
 * reason: a pair withheld for the whole function can cost more than it
 * saves). A discarded attempt is undone by truncating what it appended. */
static void gen_func_best(struct ir_func *fn, struct code *t,
                          struct tc_sites *st, int want_debug)
{
    int at = t->len, next = st->next, nstr = st->nstr, ng = st->ng,
        nf = st->nf, with;

    /* A field's constant offset into its load or store, before
     * allocation; room is left for the +7 of a byte-wise access. */
    if (g_tc_regalloc && !want_debug && !getenv("EMBCC_NO_MEMOFF")) {
        char *w = wide_map(fn);
        ra_fold_memoff(fn, -32768, 32767 - 8, 4, 4, w, 0, 0);
        free(w);
    }
    g_tc_pairs = 1;
    if (!g_tc_regalloc || want_debug || getenv("EMBCC_TC_PAIRS")) {
        if (getenv("EMBCC_TC_PAIRS"))
            g_tc_pairs = atoi(getenv("EMBCC_TC_PAIRS"));
        gen_func(fn, t, st, want_debug);
        g_tc_pairs = 1;
        return;
    }
    gen_func(fn, t, st, want_debug);
    with = t->len - at;
    t->len = at; st->next = next; st->nstr = nstr; st->ng = ng; st->nf = nf;
    g_tc_pairs = 0;
    gen_func(fn, t, st, want_debug);
    if (t->len - at > with) {
        t->len = at; st->next = next; st->nstr = nstr; st->ng = ng;
        st->nf = nf;
        g_tc_pairs = 1;
        gen_func(fn, t, st, want_debug);
    }
    g_tc_pairs = 1;
}

void codegen_unit_tricore(struct ir_unit *iu, struct code *text,
                          struct extcall **ext, int *next,
                          struct strsite **strs, int *nstrs,
                          struct gsite **gs, int *ngs,
                          struct fsite **fs, int *nfs, int want_debug,
                          int optimize, int no_sse, int regalloc)
{
    struct tc_sites st;

    (void)optimize; (void)no_sse;
    g_tc_regalloc = regalloc;
    memset(&st, 0, sizeof st);
    /* the 16-bit forms wherever one says the same (emit.h), and back off
     * for whatever encodes after this -- EmbLD's fixed-size entry stub,
     * in the same process when embcc links */
    tc_set_short(!getenv("EMBCC_TC_NOSHORT"));
    for (int n = 0; n < iu->nfuncs; n++)
        gen_func_best(&iu->funcs[n], text, &st, want_debug);
    tc_set_short(0);
    cg_resolve_strsites(iu, st.str, st.nstr);
    *ext = st.ext;   *next = st.next;
    *strs = st.str;  *nstrs = st.nstr;
    *gs = st.g;      *ngs = st.ng;
    *fs = st.f;      *nfs = st.nf;
}
