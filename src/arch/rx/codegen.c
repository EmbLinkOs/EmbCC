/* Renesas RX (RXv1) code generation, little-endian, GCC's rx-elf ABI with
 * 32-bit doubles and no FPU (docs/internals/rx-plan.md).
 *
 * The shape is the MIPS32 backend's (src/arch/mips/codegen.c), itself
 * RV32's: every vreg has one home -- a register the shared allocator gave
 * it, or a frame slot -- every operation reads its operands through
 * rdr/rd and writes its result through wreg/wrote, and a 64-bit value
 * lives in a register pair found by a pass of its own. What RX changes:
 *
 *   * Two-operand arithmetic with flags, a CISC's memory operands and
 *     immediates of any width. `d = a op b` is `mov a, d; op b, d`, or one
 *     of the three-operand forms (add/sub/and/or/mul rs, rs2, rd), and b
 *     may stay in its frame slot: `add 8[r0].l, r1` reads it there. A
 *     64-bit add or subtract is add/adc or sub/sbb, the second operand's
 *     halves read from wherever they are.
 *   * A compare sets the flags and a branch or scCND reads them. After
 *     `cmp b, a` the carry means a >= b UNSIGNED (it is not a borrow).
 *   * Branches are 1 to 4 bytes with different reaches; each starts in
 *     its 2-byte form and one that does not reach is lengthened, and the
 *     function generated again until nothing new fails.
 *   * The call pushes its return address (bsr, jsr): a callee's incoming
 *     stack arguments start at its entry sp + 4. The frame is
 *     `pushm r6-rN; add #-frame, r0` and one `rtsd` undoes all of it.
 *   * The convention (place_arg): r1-r4 by whole words, an argument in
 *     registers only when ALL of it fits, the byte count advancing past a
 *     stacked one too; stacked arguments naturally aligned and only their
 *     own size; the last named argument of a variadic call on the stack;
 *     the hidden result pointer in r15.
 *   * Soft float through lib/rt, with double the same binary32 as float.
 *
 * Scratch registers, outside the allocator's pool: r14 (ACC, the low
 * word of the A pair), r15 (TMP, its high word) and r5 (SCR). r15 also
 * carries a call's hidden result pointer, set last.
 *
 * Inline assembly is assembled in irgen (rx/irgen.c, against rx/asm.c);
 * IR_ASM here moves its operands in and out and splices its bytes.
 *
 * Refused by name: a jump table (target_jump_tables keeps a dense
 * switch a decision tree), __int128,
 * __builtin_frame_address, and a frame beyond what the encodings reach.
 * THE RULE.
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

#define ACC  RX_R14
#define TMP  RX_R15
#define SCR  RX_R5
#define A_LO RX_R14
#define A_HI RX_R15
#define FBREG RX_R13        /* the frame base under alloca */
#define CR_PSW 0

struct rx_sites {
    struct extcall *ext;   int next, capext;
    struct strsite *str;   int nstr, capstr;
    struct gsite *g;       int ng, capg;
    struct fsite *f;       int nf, capf;
};

struct rx_fn {
    int *usecnt;
    int skip_next;
    int keep_vars;
    struct ir_func *fn;
    int *loc;
    int used_callee[RA_MAXPOOL];
    int pair_used[8], npair;
    int nsave;           /* registers the allocator says it used */
    int save_hi;         /* pushm r6-r<save_hi>, or 0 for none */
    struct code *t;
    struct rx_sites *st;
    char *wide;
    char *nshr;
    long *slot;
    long frame;          /* bytes below the pushed registers */
    long in_base;        /* the first incoming stack argument, from sp */
    long scratch_at;
    long sret_slot;
    int fb;              /* r0, or r13 under alloca */
    long out_bytes;
    long va_first;
    int *label_off;
    struct { int at; int label; int level; } *fix;
    int nfix, capfix;
    const char *level;   /* per branch in emission order: 0 short .. 2 */
    int nlevel;
};

/* ---- the allocator's view ------------------------------------------------
 * The argument registers first (caller-saved), then r6-r13. */
#define RX_NPOOL 12
static const int RX_POOL[RX_NPOOL] = {
    RX_R1, RX_R2, RX_R3, RX_R4,
    RX_R6, RX_R7, RX_R8, RX_R9, RX_R10, RX_R11, RX_R12, RX_R13
};

static const int *rx_pool_for(const struct ir_func *fn, int *n)
{
    *n = fn->has_alloca ? RX_NPOOL - 1 : RX_NPOOL;   /* r13: the frame base */
    return RX_POOL;
}

#define RX_NPAIRS 6
static const int RX_PAIRS[RX_NPAIRS] = {
    RX_R1, RX_R3, RX_R6, RX_R8, RX_R10, RX_R12
};

static const int *rx_pair_pool_for(const struct ir_func *fn, int *n)
{
    *n = fn->has_alloca ? RX_NPAIRS - 1 : RX_NPAIRS;
    return RX_PAIRS;
}

static int rx_callee_saved(int r)
{
    return r >= RX_R6 && r <= RX_R13;
}

static int rx_ldvar_plain(int size, int sign, int w)
{
    (void)sign;
    return size == 4 && w == 4;
}

/* Every floating-point operation is a call (soft float), and so are the
 * conversions and a 64-bit divide. A conversion between float and double
 * is not: they are the same binary32 here. */
int rx_op_calls_helper(const struct ir_ins *i)
{
    if (i->flt)
        return i->op == IR_ADD || i->op == IR_SUB || i->op == IR_MUL ||
               i->op == IR_DIV || i->op == IR_CMP;
    if (i->op == IR_I2F || i->op == IR_F2I)
        return 1;
    if (i->op == IR_F2F)
        return i->size != i->w;
    return (i->op == IR_DIV || i->op == IR_MOD) && i->w == 8;
}

static void rx_abi_hints(const struct ir_func *fn, int *hint);
static void rx_pair_hints(const struct ir_func *fn, int *hint);

static const struct ra_target RX_RATGT = {
    rx_pool_for,
    rx_callee_saved,
    rx_ldvar_plain,
    1, 1, 1,
    rx_op_calls_helper,
    1,              /* two-operand, mostly */
    rx_abi_hints,
    NULL, NULL,
    1,              /* float_in_gpr */
    NULL, NULL,
    1,              /* atomic_in_reg */
    0,
    1               /* asm_in_reg: see IR_ASM */
};

static const struct ra_target RX_PAIR_RA = {
    rx_pair_pool_for, rx_callee_saved, rx_ldvar_plain,
    1, 1, 1,
    rx_op_calls_helper,
    1,
    rx_pair_hints,
    NULL, NULL,
    1,
    NULL, NULL,
    1,
    0,
    0
};

static int g_rx_regalloc;
static int g_rx_pairs = 1;

/* ---- refusal ------------------------------------------------------------ */

static EMBCC_NORETURN void rx_refuse(const struct rx_fn *F,
                                     const struct ir_ins *i, const char *what)
{
    char op[64];
    op[0] = 0;
    if (i)
        snprintf(op, sizeof op, " [%s w=%d size=%d]", ir_opname(i->op),
                 i->w, i->size);
    fprintf(stderr,
            "embcc: %s:%d: error: the RX backend cannot lower %s yet "
            "(function %s)%s\n",
            F->fn->file ? F->fn->file : "?",
            i ? i->line : F->fn->line, what, F->fn->name, op);
    exit(1);
}

/* ---- which values are eight bytes wide (MIPS's rule) ------------------- */
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
        case IR_MULW:             /* two words in, a 64-bit product out */
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

/* ---- the calling convention -----------------------------------------------
 *
 * GCC's rx_function_arg, exactly: the count `cum` is in bytes, each
 * argument rounded up to whole words. An argument goes in r1 + cum/4 when
 * all of it fits below 16 bytes, it is named, and -- for an aggregate --
 * its size is a whole number of words; otherwise it is stacked. `cum`
 * advances either way. A stacked argument sits at the next offset aligned
 * for its type and takes its own size. */
struct argplace {
    int reg, nreg;       /* first argument register index (0..3), count */
    int onstk;
    long stk;            /* its offset in the stack block */
    int sret;            /* the C++ return slot: in r15 (place_arg) */
};

static int aggregate(const struct ir_arg *a)
{
    return a->is_struct && !(a->ty && a->ty->is_complex);
}

/* sret: this is the C++ indirect-result pointer (sret_first: the return
 * slot of a class that is not trivially copyable, which the C++ lowering
 * passes first). GCC passes it as it passes a struct's result buffer: in
 * r15, taking no argument register or stack word. */
static void place_arg(const struct ir_arg *a, int named, int sret,
                      long *cum, long *stk, struct argplace *p)
{
    long size = a->size, words = (size + 3) / 4;
    p->nreg = 0;
    p->reg = 0;
    p->onstk = 0;
    p->stk = 0;
    p->sret = sret;
    if (sret)
        return;
    if (size >= 1 && named && *cum + words * 4 <= 16 &&
        !(aggregate(a) && size % 4)) {
        p->reg = (int)(*cum / 4);
        p->nreg = (int)words;
    } else {
        /* its alignment, capped at the stack's 4 (GCC's
         * MAX_SUPPORTED_STACK_ALIGNMENT: an aligned(16) struct is at a
         * multiple of 4) */
        long al = a->align > 0 ? a->align : 1;
        if (al > 4)
            al = 4;
        *stk = (*stk + al - 1) & ~(al - 1);
        p->stk = *stk;
        p->onstk = 1;
        *stk += size;
    }
    *cum += words * 4;
}

/* Is argument k of a call named? The last named argument of a variadic
 * call travels with the unnamed ones (GCC has no setup_incoming_varargs
 * for RX, so it treats that one as unnamed). */
static int call_named(const struct ir_ins *i, int k)
{
    if (!i->call_varargs)
        return 1;
    return k < i->call_nfixed - 1;
}

static int param_named(const struct ir_func *fn, int p)
{
    return !(fn->is_varargs && p == fn->nparams - 1);
}

static int argreg(int q) { return RX_R1 + q; }

/* Does a composite of this type come back in registers (r1..r4)? GCC's
 * rx_return_in_memory: an aggregate of 1..16 bytes whose size is a whole
 * number of words; a _Complex (not an aggregate to GCC) always. */
static int ret_in_regs(const struct type *t, long size)
{
    if (t && t->is_complex)
        return size <= 16;
    return size >= 1 && size <= 16 && size % 4 == 0;
}

static int fn_sret_first(const struct ir_func *fn)
{
    return fn->src && fn->src->sret_first;
}

static int fn_sret(const struct ir_func *fn)
{
    return fn->ret_abi.is_struct &&
           !ret_in_regs(fn->ret_abi.ty, fn->ret_abi.size);
}

static int call_sret(const struct ir_ins *i)
{
    return i->retsize && !ret_in_regs(i->rety, i->retsize);
}

static void rx_abi_hints(const struct ir_func *fn, int *hint)
{
    long cum = 0, stk = 0;
    struct argplace pl;
    for (int p = 0; fn->src && p < fn->nparams && p < fn->nvregs; p++) {
        const struct ir_arg *a = &fn->param_abi[p];
        place_arg(a, param_named(fn, p), p == 0 && fn_sret_first(fn), &cum, &stk, &pl);
        if (pl.nreg == 1 && !a->is_struct && a->size <= 4)
            hint[p] = argreg(pl.reg);
    }
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        if (i->op == IR_RET && i->a >= 0 && i->a < fn->nvregs &&
            !fn->ret_abi.is_struct && fn->ret_abi.size <= 4)
            hint[i->a] = RX_R1;
        if (i->op != IR_CALL && rx_op_calls_helper(i) && i->w <= 4) {
            if (i->a >= 0 && i->a < fn->nvregs && hint[i->a] < 0)
                hint[i->a] = RX_R1;
            if (!i->imm_b && i->b >= 0 && i->b < fn->nvregs &&
                hint[i->b] < 0)
                hint[i->b] = RX_R2;
            if (i->dst >= 0 && i->dst < fn->nvregs && hint[i->dst] < 0)
                hint[i->dst] = RX_R1;
        }
        if (i->op != IR_CALL)
            continue;
        if (!i->retsize && i->dst >= 0 && i->dst < fn->nvregs && i->w <= 4)
            hint[i->dst] = RX_R1;
        cum = 0; stk = 0;
        for (int k = 0; k < i->nargs; k++) {
            const struct ir_arg *a = &i->argv[k];
            place_arg(a, call_named(i, k), k == 0 && i->sret_first, &cum, &stk, &pl);
            if (pl.nreg == 1 && !a->is_struct && a->size <= 4 &&
                a->vreg >= 0 && a->vreg < fn->nvregs)
                hint[a->vreg] = argreg(pl.reg);
        }
    }
}

static void rx_pair_hints(const struct ir_func *fn, int *hint)
{
    long cum = 0, stk = 0;
    struct argplace pl;
    for (int p = 0; fn->src && p < fn->nparams && p < fn->nvregs; p++) {
        const struct ir_arg *a = &fn->param_abi[p];
        place_arg(a, param_named(fn, p), p == 0 && fn_sret_first(fn), &cum, &stk, &pl);
        if (a->size == 8 && pl.nreg == 2 && !a->is_struct &&
            (pl.reg == 0 || pl.reg == 2))
            hint[p] = argreg(pl.reg);
    }
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        if (i->op == IR_RET && i->a >= 0 && i->a < fn->nvregs &&
            !fn->ret_abi.is_struct && fn->ret_abi.size == 8)
            hint[i->a] = RX_R1;
        if (i->op != IR_CALL && rx_op_calls_helper(i) && i->w == 8) {
            if (i->a >= 0 && i->a < fn->nvregs && hint[i->a] < 0)
                hint[i->a] = RX_R1;
            if (!i->imm_b && i->b >= 0 && i->b < fn->nvregs &&
                hint[i->b] < 0)
                hint[i->b] = RX_R3;
            if (i->dst >= 0 && i->dst < fn->nvregs && hint[i->dst] < 0)
                hint[i->dst] = RX_R1;
        }
        if (i->op != IR_CALL)
            continue;
        if (!i->retsize && i->dst >= 0 && i->dst < fn->nvregs && i->w == 8)
            hint[i->dst] = RX_R1;
        cum = 0; stk = 0;
        for (int k = 0; k < i->nargs; k++) {
            const struct ir_arg *a = &i->argv[k];
            place_arg(a, call_named(i, k), k == 0 && i->sret_first, &cum, &stk, &pl);
            if (a->size == 8 && pl.nreg == 2 && !a->is_struct &&
                (pl.reg == 0 || pl.reg == 2) &&
                a->vreg >= 0 && a->vreg < fn->nvregs)
                hint[a->vreg] = argreg(pl.reg);
        }
    }
}

/* ---- the frame -----------------------------------------------------------
 *
 * From sp upward: the outgoing stack arguments, the shared temp slots,
 * 64-bit temps without a pair, locals, the struct-return scratch, the
 * sret pointer; then the registers pushm saved (r6 lowest), the return
 * address, and the caller's stack arguments. 4-aligned throughout. */
static int in_reg(const struct rx_fn *F, int v)
{
    return F->loc && v >= 0 && F->loc[v] >= 0;
}

static long outgoing_area(const struct rx_fn *F)
{
    const struct ir_func *fn = F->fn;
    long most = 0;
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        struct argplace pl;
        long cum = 0, stk = 0;
        if (i->op != IR_CALL)
            continue;
        for (int k = 0; k < i->nargs; k++)
            place_arg(&i->argv[k], call_named(i, k), k == 0 && i->sret_first, &cum, &stk, &pl);
        if (stk > most)
            most = stk;
    }
    return (most + 3) & ~3L;
}

static void layout(struct rx_fn *F)
{
    struct ir_func *fn = F->fn;
    long off = outgoing_area(F);
    if (fn->has_alloca)
        off = (off + 15) & ~15L;
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
            struct ra_slots so = { loc2, NULL, g_rx_regalloc, has_cgoto };
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
            F->slot[v] = off;
            off += 8;
        }
        free(loc2);
    }
    {
        char *lref = ra_locals_referenced(fn, F->keep_vars);
        for (int pass = 0; pass < 2; pass++)
            for (int v = 0; v < fn->nvars; v++) {
                int size = fn->locals[v].size ? fn->locals[v].size : 4;
                int align = fn->locals[v].user_align ? fn->locals[v].user_align
                          : fn->locals[v].align ? fn->locals[v].align : 4;
                if (in_reg(F, v) || !lref[v] ||
                    ra_slot_dead(fn, F->loc, NULL, v, F->keep_vars))
                    continue;
                if ((size > 8) != pass)
                    continue;
                if (align < 4) align = 4;
                off = (off + align - 1) & ~(long)(align - 1);
                F->slot[v] = off;
                off += size;
            }
        free(lref);
    }
    F->scratch_at = (off + 7) & ~7L;
    off = F->scratch_at + fn->scratch_bytes;
    F->sret_slot = -1;
    if (fn_sret(fn) || fn_sret_first(fn)) {
        off = (off + 3) & ~3L;
        F->sret_slot = off;
        off += 4;
    }
    F->frame = (off + 3) & ~3L;
    F->save_hi = 0;
    for (int k = 0; k < F->nsave; k++)
        if (F->used_callee[k] > F->save_hi)
            F->save_hi = F->used_callee[k];
    F->in_base = F->frame + (F->save_hi ? 4L * (F->save_hi - RX_R6 + 1) : 0)
                 + 4;
    F->va_first = -1;
}

/* ---- memory through a base register ------------------------------------- */

static void ld_base(struct rx_fn *F, int reg, int base, long off, int size,
                    int sign)
{
    int sz = size == 1 ? RX_B : size == 2 ? RX_W : RX_L;
    if (rx_load_ok(sz, sign, off)) {
        rx_load(F->t, sz, sign, off, base, reg);
        return;
    }
    if (reg == RX_SP)
        internal_error("rx: a load into the stack pointer");
    rx_add3(F->t, off, base, reg);
    rx_load(F->t, sz, sign, 0, reg, reg);
}

/* A store whose displacement does not encode: the address in a register
 * pushed for the purpose, so nothing live is disturbed. */
static void st_base(struct rx_fn *F, int val, int base, long off, int size)
{
    int sz = size == 1 ? RX_B : size == 2 ? RX_W : RX_L;
    int x;
    if (rx_dsp_ok(sz, off)) {
        rx_store(F->t, sz, val, off, base);
        return;
    }
    x = val != ACC && base != ACC ? ACC : val != TMP && base != TMP ? TMP
                                                                     : SCR;
    rx_push(F->t, x);
    rx_add3(F->t, base == RX_SP ? off + 4 : off, base, x);
    rx_store(F->t, sz, val, 0, x);
    rx_pop(F->t, x);
}

static void ld_sp(struct rx_fn *F, int reg, long off, int size, int sign)
{
    ld_base(F, reg, F->fb, off, size, sign);
}

static void st_sp(struct rx_fn *F, int reg, long off, int size)
{
    st_base(F, reg, F->fb, off, size);
}

static void st_out(struct rx_fn *F, int reg, long off, int size)
{
    st_base(F, reg, RX_SP, off, size);
}

static void addr_sp(struct rx_fn *F, int reg, long off)
{
    rx_add3(F->t, off, F->fb, reg);
}

static void mv(struct rx_fn *F, int d, int s)
{
    if (d != s)
        rx_rr(F->t, RX_MOV, s, d);
}

static long sslot(const struct rx_fn *F, int v)
{
    if (v < 0 || v >= F->fn->nvregs || F->slot[v] < 0)
        internal_error("rx: %s: a path addresses vreg %d's slot, and it "
                       "has none", F->fn->name, v);
    return F->slot[v];
}

static void rd(struct rx_fn *F, int v, int reg)
{
    if (in_reg(F, v)) {
        mv(F, reg, F->loc[v]);
        return;
    }
    ld_sp(F, reg, sslot(F, v), 4, 1);
}

static int rdr(struct rx_fn *F, int v, int scratch)
{
    if (in_reg(F, v))
        return F->loc[v];
    ld_sp(F, scratch, sslot(F, v), 4, 1);
    return scratch;
}

static int wreg(struct rx_fn *F, int v, int scratch)
{
    return in_reg(F, v) ? F->loc[v] : scratch;
}

static void wrote(struct rx_fn *F, int v, int reg)
{
    if (in_reg(F, v)) {
        mv(F, F->loc[v], reg);
        return;
    }
    if (v < 0 || F->slot[v] < 0)
        return;
    st_sp(F, reg, sslot(F, v), 4);
}

static void wr(struct rx_fn *F, int v, int reg) { wrote(F, v, reg); }

/* dl <- sl and dh <- sh as one parallel move. */
static void mv2(struct rx_fn *F, int dl, int sl, int dh, int sh)
{
    if (dl == sh && dh == sl) {
        if (dl == sl) return;
        rx_rr(F->t, RX_XCHG, sl, sh);
        return;
    }
    if (dl == sh) {
        mv(F, dh, sh);
        mv(F, dl, sl);
        return;
    }
    mv(F, dl, sl);
    mv(F, dh, sh);
}

static void rd64(struct rx_fn *F, int v, int lo, int hi)
{
    if (in_reg(F, v)) {
        mv2(F, lo, F->loc[v], hi, F->loc[v] + 1);
        return;
    }
    ld_sp(F, lo, sslot(F, v), 4, 1);
    ld_sp(F, hi, sslot(F, v) + 4, 4, 1);
}

static void wr64(struct rx_fn *F, int v, int lo, int hi)
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

static long imm_val(const struct ir_ins *i)
{
    return (long)(int)(unsigned int)(unsigned long)i->imm;
}

/* rd OP= v for a vreg v, from its register or its slot (a memory
 * operand where the operation has one). `scr` is a free scratch, not rd. */
static void op_v(struct rx_fn *F, int op, int v, int rdst, int scr)
{
    if (in_reg(F, v)) {
        rx_rr(F->t, op, F->loc[v], rdst);
        return;
    }
    if (F->fb == RX_SP || F->fb == FBREG) {
        long off = sslot(F, v);
        if (rx_rm_ok(op, RX_L, 1, off)) {
            rx_rm(F->t, op, RX_L, 1, off, F->fb, rdst);
            return;
        }
    }
    ld_sp(F, scr, sslot(F, v), 4, 1);
    rx_rr(F->t, op, scr, rdst);
}

/* rd OP= the instruction's second operand (immediate or vreg). */
static void op_b(struct rx_fn *F, int op, const struct ir_ins *i, int rdst,
                 int scr)
{
    if (i->imm_b) {
        rx_ri(F->t, op, imm_val(i), rdst);
        return;
    }
    op_v(F, op, i->b, rdst, scr);
}

static int other_scr(int r)
{
    return r == TMP ? ACC : TMP;
}

/* Sign- or zero-extend the low `size` bytes of rs into rdst. */
static void ext_reg(struct rx_fn *F, int rdst, int rs, int size, int sign)
{
    if (size >= 4) {
        mv(F, rdst, rs);
        return;
    }
    rx_ext(F->t, size == 1 ? RX_B : RX_W, sign, rs, rdst);
}

/* ---- branches --------------------------------------------------------------
 *
 * A branch to a label is placed at the level its fix asks (0 on the first
 * attempt): 0 the two-byte bCND.b / bra.b, 1 the three-byte beq.w / bne.w
 * / bra.w (other conditions: the inverse bCND.b over a bra.w), 2 a bra.a
 * (the inverse over one for a condition). One that does not reach goes up
 * a level and the function is generated again. */
static void want_label(struct rx_fn *F, int at, int label, int level)
{
    if (F->nfix == F->capfix) {
        F->capfix = F->capfix ? F->capfix * 2 : 16;
        F->fix = xrealloc(F->fix, (size_t)F->capfix * sizeof *F->fix);
    }
    F->fix[F->nfix].at = at;
    F->fix[F->nfix].label = label;
    F->fix[F->nfix].level = level;
    F->nfix++;
}

static void branch_to(struct rx_fn *F, int cond, int label)
{
    struct code *t = F->t;
    int level = F->level && F->nfix < F->nlevel ? F->level[F->nfix] : 0;
    int at;
    if (cond == RX_ALWAYS) {
        at = rx_branch(t, level == 0 ? RX_BR_B : level == 1 ? RX_BR_W
                                                             : RX_BR_A,
                       RX_ALWAYS);
    } else if (level == 0) {
        at = rx_branch(t, RX_BR_B, cond);
    } else if (level == 1 && (cond == RX_EQ || cond == RX_NE)) {
        at = rx_branch(t, RX_BR_W, cond);
    } else {
        int kind = level == 1 ? RX_BR_W : RX_BR_A;
        rx_branch_d(t, RX_BR_B, rx_cond_invert(cond),
                    2 + rx_branch_len(kind));
        at = rx_branch(t, kind, RX_ALWAYS);
    }
    want_label(F, at, label, level);
}

static void jump_to(struct rx_fn *F, int label)
{
    branch_to(F, RX_ALWAYS, label);
}

/* A branch inside one lowering, forward to br_land: always short. */
static int br_place(struct rx_fn *F, int cond)
{
    return rx_branch(F->t, RX_BR_B, cond);
}

static void br_land(struct rx_fn *F, int at)
{
    if (!rx_patch_branch(F->t, at, F->t->len))
        internal_error("rx: %s: a branch inside one operation does not "
                       "reach", F->fn->name);
}

static void br_back(struct rx_fn *F, int at, int target)
{
    if (!rx_patch_branch(F->t, at, target))
        internal_error("rx: %s: a loop inside one operation does not "
                       "reach", F->fn->name);
}

static int cond_of(enum binop pred, int sign)
{
    switch (pred) {
    case B_EQ: return RX_EQ;
    case B_NE: return RX_NE;
    case B_LT: return sign ? RX_LT : RX_LTU;
    case B_GE: return sign ? RX_GE : RX_GEU;
    case B_GT: return sign ? RX_GT : RX_GTU;
    default:   return sign ? RX_LE : RX_LEU;     /* B_LE */
    }
}

/* The condition that holds for (b, a) when `cond` holds for (a, b). */
static int cond_swap(int cond)
{
    switch (cond) {
    case RX_LT:  return RX_GT;
    case RX_GT:  return RX_LT;
    case RX_LE:  return RX_GE;
    case RX_GE:  return RX_LE;
    case RX_LTU: return RX_GTU;
    case RX_GTU: return RX_LTU;
    case RX_LEU: return RX_GEU;
    case RX_GEU: return RX_LEU;
    default:     return cond;
    }
}

/* ---- site lists ----------------------------------------------------------- */

static void note_ext(struct rx_sites *st, int at, struct func *callee,
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

static void note_str(struct rx_sites *st, int at, int idx, enum reloc_kind k)
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

static void note_glob(struct rx_sites *st, int at, struct global *g,
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

static void note_fn(struct rx_sites *st, int at, struct func *target,
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

/* A call: bsr.a with an R_RX_DIR24S_PCREL on its 24-bit field (+1), even
 * to a function of this unit: the displacement is the linker's. */
static void call_sym(struct rx_fn *F, struct func *callee)
{
    int at = rx_bsr_a(F->t);
    note_ext(F->st, at + 1, callee, 0);
}

static struct func **g_helpers;
static int g_nhelpers, g_caphelpers;

static void call_helper(struct rx_fn *F, const char *name)
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
    call_sym(F, h);
}

/* Put n vregs (word `half`) into the registers a helper or call expects,
 * at once: the register edges as one parallel move (`cyc` breaks a
 * cycle), then the loads. */
static void set_args_half(struct rx_fn *F, const int *dstreg,
                          const int *vreg, const int *half, int n, int cyc)
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
        int m = ra_parallel_move(pd, ps, npm, cyc, od, os,
                                 (int)(sizeof od / sizeof od[0]));
        if (m < 0)
            internal_error("rx: an argument setup is not a well-formed move");
        for (int k = 0; k < m; k++)
            mv(F, od[k], os[k]);
    }
    for (int k = 0; k < n; k++)
        if (!in_reg(F, vreg[k]))
            ld_sp(F, dstreg[k],
                  sslot(F, vreg[k]) + (half ? 4L * half[k] : 0), 4, 1);
}

static void args64x2(struct rx_fn *F, int va, int vb)
{
    int d[4] = { RX_R1, RX_R2, RX_R3, RX_R4 };
    int v[4], h[4] = { 0, 1, 0, 1 };
    v[0] = v[1] = va;
    v[2] = v[3] = vb;
    set_args_half(F, d, v, h, vb >= 0 ? 4 : 2, SCR);
}

static void args2(struct rx_fn *F, int va, int vb)
{
    int d[2] = { RX_R1, RX_R2 }, v[2];
    v[0] = va; v[1] = vb;
    set_args_half(F, d, v, NULL, vb >= 0 ? 2 : 1, SCR);
}

/* ---- 64-bit integers ---------------------------------------------------- */

/* The A pair's half `h` OP= half h of the instruction's second operand,
 * which is an immediate, a pair, or a slot (a memory operand). */
static void op64_b(struct rx_fn *F, int op, const struct ir_ins *i, int h,
                   int rdst)
{
    if (i->imm_b) {
        long v = h ? (long)(i->imm >> 32) : (long)i->imm;
        rx_ri(F->t, op, v, rdst);
        return;
    }
    if (in_reg(F, i->b)) {
        rx_rr(F->t, op, F->loc[i->b] + h, rdst);
        return;
    }
    {
        long off = sslot(F, i->b) + 4L * h;
        if (rx_rm_ok(op, RX_L, 1, off)) {
            rx_rm(F->t, op, RX_L, 1, off, F->fb, rdst);
            return;
        }
        ld_sp(F, SCR, off, 4, 1);
        rx_rr(F->t, op, SCR, rdst);
    }
}

/* A 64-bit comparison's flags: (lo, hi) minus the instruction's second
 * operand, by sub and sbb, so the carry, sign and overflow are the whole
 * subtraction's (Z is not: only the high word's). With `swap`, b minus a
 * instead. Uses the A pair and SCR. */
static void cmp64_flags(struct rx_fn *F, const struct ir_ins *i, int swap)
{
    struct code *t = F->t;
    if (!swap) {
        rd64(F, i->a, A_LO, A_HI);
        if (i->imm_b) {
            rx_ri(t, RX_MOV, (long)i->imm, SCR);
            rx_rr(t, RX_SUB, SCR, A_LO);
            rx_ri(t, RX_MOV, (long)(i->imm >> 32), SCR);
            rx_rr(t, RX_SBB, SCR, A_HI);
        } else {
            op64_b(F, RX_SUB, i, 0, A_LO);
            op64_b(F, RX_SBB, i, 1, A_HI);
        }
        return;
    }
    if (i->imm_b) {
        rx_ri(t, RX_MOV, (long)i->imm, A_LO);
        rx_ri(t, RX_MOV, (long)(i->imm >> 32), A_HI);
    } else {
        rd64(F, i->b, A_LO, A_HI);
    }
    if (in_reg(F, i->a)) {
        rx_rr(t, RX_SUB, F->loc[i->a], A_LO);
        rx_rr(t, RX_SBB, F->loc[i->a] + 1, A_HI);
    } else {
        long off = sslot(F, i->a);
        if (rx_rm_ok(RX_SBB, RX_L, 1, off + 4)) {
            rx_rm(t, RX_SUB, RX_L, 1, off, F->fb, A_LO);
            rx_rm(t, RX_SBB, RX_L, 1, off + 4, F->fb, A_HI);
        } else {
            ld_sp(F, SCR, off, 4, 1);
            rx_rr(t, RX_SUB, SCR, A_LO);
            ld_sp(F, SCR, off + 4, 4, 1);
            rx_rr(t, RX_SBB, SCR, A_HI);
        }
    }
}

/* The flags of a 64-bit comparison and the condition to test: EQ/NE by
 * xor-ing the halves and or-ing them; the ordered ones by a full
 * subtraction, GT/LE (and their unsigned twins) with the operands swapped
 * so that the Z the subtraction cannot give is not needed. */
static int cmp64_cond(struct rx_fn *F, const struct ir_ins *i)
{
    int cond = cond_of(i->pred, i->sign);
    if (cond == RX_EQ || cond == RX_NE) {
        rd64(F, i->a, A_LO, A_HI);
        op64_b(F, RX_XOR, i, 0, A_LO);
        op64_b(F, RX_XOR, i, 1, A_HI);
        rx_rr(F->t, RX_OR, A_HI, A_LO);
        return cond;
    }
    if (cond == RX_GT || cond == RX_LE || cond == RX_GTU || cond == RX_LEU) {
        cmp64_flags(F, i, 1);
        return cond_swap(cond);
    }
    cmp64_flags(F, i, 0);
    return cond;
}

static void shift64_imm(struct rx_fn *F, int left, int sign, long n)
{
    struct code *t = F->t;
    n &= 63;
    if (n == 0)
        return;
    if (n >= 32) {
        int k = (int)(n - 32);
        if (left) {
            rx_shift_i(t, RX_SHLL, k, A_LO, A_HI);
            rx_ri(t, RX_MOV, 0, A_LO);
        } else {
            rx_shift_i(t, sign ? RX_SHAR : RX_SHLR, k, A_HI, A_LO);
            if (sign) rx_shift_i(t, RX_SHAR, 31, A_HI, A_HI);
            else      rx_ri(t, RX_MOV, 0, A_HI);
        }
        return;
    }
    if (left) {
        rx_shift_i(t, RX_SHLR, (int)(32 - n), A_LO, SCR);
        rx_shift_i(t, RX_SHLL, (int)n, A_HI, A_HI);
        rx_rr(t, RX_OR, SCR, A_HI);
        rx_shift_i(t, RX_SHLL, (int)n, A_LO, A_LO);
    } else {
        rx_shift_i(t, RX_SHLL, (int)(32 - n), A_HI, SCR);
        rx_shift_i(t, RX_SHLR, (int)n, A_LO, A_LO);
        rx_rr(t, RX_OR, SCR, A_LO);
        rx_shift_i(t, sign ? RX_SHAR : RX_SHLR, (int)n, A_HI, A_HI);
    }
}

/* The A pair shifted by the count in SCR (its low six bits), one bit a
 * trip through the carry: shll/rolc, shlr|shar/rorc. */
static void shift64_var(struct rx_fn *F, int left, int sign)
{
    struct code *t = F->t;
    int done, top, again;
    rx_ri(t, RX_AND, 63, SCR);
    done = br_place(F, RX_EQ);
    top = t->len;
    if (left) {
        rx_shift_i(t, RX_SHLL, 1, A_LO, A_LO);
        rx_rolc(t, A_HI);
    } else {
        rx_shift_i(t, sign ? RX_SHAR : RX_SHLR, 1, A_HI, A_HI);
        rx_rorc(t, A_LO);
    }
    rx_ri(t, RX_SUB, 1, SCR);
    again = br_place(F, RX_NE);
    br_back(F, again, top);
    br_land(F, done);
}

static int gen_ins64(struct rx_fn *F, int n)
{
    struct ir_func *fn = F->fn;
    struct ir_ins *i = &fn->ins[n];
    struct code *t = F->t;

    switch (i->op) {
    case IR_CONST:
        rx_ri(t, RX_MOV, (long)i->imm, A_LO);
        rx_ri(t, RX_MOV, (long)(i->imm >> 32), A_HI);
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
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
        op64_b(F, RX_ADD, i, 0, A_LO);
        op64_b(F, RX_ADC, i, 1, A_HI);
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    case IR_SUB:
        rd64(F, i->a, A_LO, A_HI);
        if (i->imm_b) {
            /* a - k is a + (-k), carries and all: there is no sbb #imm */
            unsigned long long nk = 0ULL - (unsigned long long)i->imm;
            rx_ri(t, RX_ADD, (long)(nk & 0xffffffffULL), A_LO);
            rx_ri(t, RX_ADC, (long)(nk >> 32), A_HI);
        } else {
            op64_b(F, RX_SUB, i, 0, A_LO);
            op64_b(F, RX_SBB, i, 1, A_HI);
        }
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    case IR_AND: case IR_OR: case IR_XOR: {
        int op = i->op == IR_AND ? RX_AND : i->op == IR_OR ? RX_OR : RX_XOR;
        rd64(F, i->a, A_LO, A_HI);
        op64_b(F, op, i, 0, A_LO);
        op64_b(F, op, i, 1, A_HI);
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    }
    case IR_MUL:
        /* (ah:al)*(bh:bl) mod 2^64: emulu gives al*bl whole, the cross
         * terms only reach the high word */
        if (in_reg(F, i->a)) {
            mv(F, SCR, F->loc[i->a] + 1);
        } else {
            ld_sp(F, SCR, sslot(F, i->a) + 4, 4, 1);
        }
        op64_b(F, RX_MUL, i, 0, SCR);                 /* ah * bl */
        rd64(F, i->a, A_LO, A_HI);                    /* A_HI scratch */
        op64_b(F, RX_MUL, i, 1, A_LO);                /* al * bh */
        rx_rr(t, RX_ADD, A_LO, SCR);
        rd64(F, i->a, A_LO, A_HI);
        op64_b(F, RX_EMULU, i, 0, A_LO);              /* al * bl */
        rx_rr(t, RX_ADD, SCR, A_HI);
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    case IR_MULW: {
        /* emul/emulu: rd:rd+1 = rd * src, the whole product of two words.
         * Into the result's own pair when it has one; b first, into SCR
         * when the pair is where it lives, since a is copied over it. */
        int dl = in_reg(F, i->dst) ? F->loc[i->dst] : A_LO, rb_;
        if (in_reg(F, i->b) && F->loc[i->b] != dl && F->loc[i->b] != dl + 1) {
            rb_ = F->loc[i->b];
        } else {
            rd(F, i->b, SCR);
            rb_ = SCR;
        }
        rd(F, i->a, dl);
        rx_rr(t, i->sign ? RX_EMUL : RX_EMULU, rb_, dl);
        wr64(F, i->dst, dl, dl + 1);
        return 1;
    }
    case IR_NEG:
        rx_ri(t, RX_MOV, 0, A_LO);
        rx_ri(t, RX_MOV, 0, A_HI);
        if (in_reg(F, i->a)) {
            rx_rr(t, RX_SUB, F->loc[i->a], A_LO);
            rx_rr(t, RX_SBB, F->loc[i->a] + 1, A_HI);
        } else {
            ld_sp(F, SCR, sslot(F, i->a), 4, 1);
            rx_rr(t, RX_SUB, SCR, A_LO);
            ld_sp(F, SCR, sslot(F, i->a) + 4, 4, 1);
            rx_rr(t, RX_SBB, SCR, A_HI);
        }
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    case IR_BNOT:
        rd64(F, i->a, A_LO, A_HI);
        rx_r(t, RX_NOT, A_LO);
        rx_r(t, RX_NOT, A_HI);
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    case IR_SHL: case IR_SHR: {
        int sign = i->op == IR_SHR && i->sign;
        if (!i->imm_b)
            rd(F, i->b, SCR);
        rd64(F, i->a, A_LO, A_HI);
        if (i->imm_b)
            shift64_imm(F, i->op == IR_SHL, sign, (long)i->imm);
        else
            shift64_var(F, i->op == IR_SHL, sign);
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    }
    case IR_EXT:
        rd(F, i->a, A_LO);
        if (i->size < 4)
            ext_reg(F, A_LO, A_LO, i->size, i->sign);
        if (i->sign) rx_shift_i(t, RX_SHAR, 31, A_LO, A_HI);
        else         rx_ri(t, RX_MOV, 0, A_HI);
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    case IR_LDVAR:
        if (i->size == 8) {
            rd64(F, i->a, A_LO, A_HI);
        } else {
            if (in_reg(F, i->a))
                ext_reg(F, A_LO, F->loc[i->a], i->size, i->sign);
            else
                ld_sp(F, A_LO, sslot(F, i->a), i->size, i->sign);
            if (i->sign) rx_shift_i(t, RX_SHAR, 31, A_LO, A_HI);
            else         rx_ri(t, RX_MOV, 0, A_HI);
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
        rd(F, i->a, SCR);
        if (i->size == 8) {
            ld_base(F, A_LO, SCR, i->memoff, 4, 1);
            ld_base(F, A_HI, SCR, i->memoff + 4L, 4, 1);
        } else {
            ld_base(F, A_LO, SCR, i->memoff, i->size, i->sign);
            if (i->sign) rx_shift_i(t, RX_SHAR, 31, A_LO, A_HI);
            else         rx_ri(t, RX_MOV, 0, A_HI);
        }
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    case IR_STORE:
        rd(F, i->a, SCR);
        rd64(F, i->b, A_LO, A_HI);
        st_base(F, A_LO, SCR, i->memoff, i->size == 8 ? 4 : i->size);
        if (i->size == 8)
            st_base(F, A_HI, SCR, i->memoff + 4L, 4);
        return 1;
    case IR_SELECT: {
        /* c, then b over it when the condition is nonzero */
        int skip;
        if (i->size == 8) {
            rd64(F, i->a, A_LO, A_HI);
            rx_rr(t, RX_OR, A_HI, A_LO);
        } else {
            rd(F, i->a, A_LO);
            rx_ri(t, RX_CMP, 0, A_LO);
        }
        rx_scc(t, RX_NE, SCR);
        rd64(F, i->c, A_LO, A_HI);
        rx_ri(t, RX_CMP, 0, SCR);
        skip = br_place(F, RX_EQ);
        rd64(F, i->b, A_LO, A_HI);
        br_land(F, skip);
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    }
    default:
        return 0;
    }
}

/* ---- the epilogue ----------------------------------------------------------
 *
 * One rtsd releases the frame, pops r6..rN and returns, when the frame
 * and the saved registers fit its byte; else the frame goes first. */
static void rx_restore(struct rx_fn *F)
{
    struct code *t = F->t;
    long nb = F->save_hi ? 4L * (F->save_hi - RX_R6 + 1) : 0;
    long fr = F->frame;
    if (fr + nb > 1020) {
        rx_add3(t, fr, RX_SP, RX_SP);
        fr = 0;
    }
    if (nb)
        rx_rtsd_m(t, fr + nb, RX_R6, F->save_hi);
    else if (fr)
        rx_rtsd(t, fr);
    else
        rx_rts(t);
}

/* ---- calls ---------------------------------------------------------------- */

static void gen_call(struct rx_fn *F, int n)
{
    struct ir_func *fn = F->fn;
    struct ir_ins *i = &fn->ins[n];
    struct code *t = F->t;
    struct argplace pl[MAX_PARAMS];
    long cum = 0, stk = 0;
    int sret = call_sret(i);

    for (int k = 0; k < i->nargs; k++)
        place_arg(&i->argv[k], call_named(i, k), k == 0 && i->sret_first, &cum, &stk, &pl[k]);

    /* The stacked arguments first, through the scratches. */
    for (int k = 0; k < i->nargs; k++) {
        struct ir_arg *a = &i->argv[k];
        if (!pl[k].onstk)
            continue;
        if (a->is_struct) {
            long off = 0;
            rd(F, a->vreg, TMP);
            for (; off + 4 <= a->size; off += 4) {
                ld_base(F, SCR, TMP, off, 4, 1);
                st_out(F, SCR, pl[k].stk + off, 4);
            }
            for (; off < a->size; off++) {
                ld_base(F, SCR, TMP, off, 1, 0);
                st_out(F, SCR, pl[k].stk + off, 1);
            }
        } else if (a->size > 4) {
            rd64(F, a->vreg, A_LO, A_HI);
            st_out(F, A_LO, pl[k].stk, 4);
            st_out(F, A_HI, pl[k].stk + 4, 4);
        } else {
            int r = rdr(F, a->vreg, SCR);
            st_out(F, r, pl[k].stk, a->size >= 4 ? 4 : a->size);
        }
    }
    /* An indirect callee's address into SCR before the argument registers
     * change: it may live in one of them. */
    if (i->indirect)
        rd(F, i->a, SCR);
    /* The scalar register arguments, one parallel move (TMP breaks a
     * cycle; SCR may hold the callee). */
    {
        int sd_[2 * MAX_PARAMS], sv_[2 * MAX_PARAMS], sh_[2 * MAX_PARAMS];
        int ns_ = 0;
        for (int k = 0; k < i->nargs; k++) {
            struct ir_arg *a = &i->argv[k];
            if (!pl[k].nreg || a->is_struct)
                continue;
            for (int q = 0; q < pl[k].nreg; q++) {
                sd_[ns_] = argreg(pl[k].reg + q);
                sv_[ns_] = a->vreg;
                sh_[ns_] = a->size > 4 ? q : 0;
                ns_++;
            }
        }
        if (ns_)
            set_args_half(F, sd_, sv_, sh_, ns_, TMP);
    }
    /* The composite register arguments, word by word from their address
     * (which the allocator keeps out of registers). */
    for (int k = 0; k < i->nargs; k++) {
        struct ir_arg *a = &i->argv[k];
        if (!pl[k].nreg || !a->is_struct)
            continue;
        rd(F, a->vreg, ACC);
        for (int q = 0; q < pl[k].nreg; q++) {
            int r = argreg(pl[k].reg + q);
            long off = (long)q * 4, left = a->size - off;
            if (left >= 4) {
                ld_base(F, r, ACC, off, 4, 1);
            } else {
                rx_ri(t, RX_MOV, 0, r);
                for (long b = off + left - 1; b >= off; b--) {
                    rx_shift_i(t, RX_SHLL, 8, r, r);
                    ld_base(F, TMP, ACC, b, 1, 0);
                    rx_rr(t, RX_OR, TMP, r);
                }
            }
        }
    }
    if (sret)
        addr_sp(F, RX_R15, F->scratch_at + i->scratch);
    else if (i->sret_first && i->nargs > 0)
        rd(F, i->argv[0].vreg, RX_R15);       /* the C++ return slot */

    if (i->indirect)
        rx_jsr(t, SCR);
    else
        call_sym(F, i->callee);

    if (i->dst < 0)
        return;
    if (i->retsize) {
        long at = F->scratch_at + i->scratch;
        if (!sret) {
            int nw = (int)((i->retsize + 3) / 4);
            for (int q = 0; q < nw; q++) {
                long left = i->retsize - 4L * q;
                if (left >= 4) {
                    st_sp(F, argreg(q), at + 4L * q, 4);
                } else {
                    for (long b = 0; b < left; b++) {
                        if (b) rx_shift_i(t, RX_SHLR, 8, argreg(q), argreg(q));
                        st_sp(F, argreg(q), at + 4L * q + b, 1);
                    }
                }
            }
        }
        addr_sp(F, ACC, at);
        wr(F, i->dst, ACC);
    } else if (F->wide[i->dst]) {
        wr64(F, i->dst, RX_R1, RX_R2);
    } else {
        wr(F, i->dst, RX_R1);
    }
}

/* ---- block copies --------------------------------------------------------
 * [TMP] to [ACC] (or zeroes at [ACC]), `size` bytes. Straight-line words
 * up to 32 bytes; beyond, the string instructions with r1-r3 saved around
 * them (smovf copies r3 bytes from [r2] to [r1]; sstr.b stores r2's byte). */
static void copy_block(struct rx_fn *F, int copy, long size)
{
    struct code *t = F->t;
    long k = 0;
    if (size > 32) {
        rx_pushm(t, RX_R1, RX_R3);
        mv(F, RX_R1, ACC);
        if (copy) mv(F, RX_R2, TMP);
        else      rx_ri(t, RX_MOV, 0, RX_R2);
        rx_ri(t, RX_MOV, size, RX_R3);
        if (copy) rx_smovf(t);
        else      rx_sstr_b(t);
        rx_popm(t, RX_R1, RX_R3);
        return;
    }
    if (!copy)
        rx_ri(t, RX_MOV, 0, SCR);
    for (; k + 4 <= size; k += 4) {
        if (copy) ld_base(F, SCR, TMP, k, 4, 1);
        st_base(F, SCR, ACC, k, 4);
    }
    for (; k + 2 <= size; k += 2) {
        if (copy) ld_base(F, SCR, TMP, k, 2, 0);
        st_base(F, SCR, ACC, k, 2);
    }
    for (; k < size; k++) {
        if (copy) ld_base(F, SCR, TMP, k, 1, 0);
        st_base(F, SCR, ACC, k, 1);
    }
}

/* ---- one instruction ------------------------------------------------------ */

static const char *fp_binop_name(enum ir_op op)
{
    switch (op) {
    case IR_ADD: return "__addsf3";
    case IR_SUB: return "__subsf3";
    case IR_MUL: return "__mulsf3";
    case IR_DIV: return "__divsf3";
    default:     return NULL;
    }
}

static const char *fp_cmp_name(enum binop pred)
{
    switch (pred) {
    case B_EQ: return "__eqsf2";
    case B_NE: return "__nesf2";
    case B_LT: return "__ltsf2";
    case B_LE: return "__lesf2";
    case B_GT: return "__gtsf2";
    default:   return "__gesf2";
    }
}

static const char *cvt_name(const struct ir_ins *i)
{
    if (i->op == IR_I2F)
        return i->size <= 4 ? (i->sign ? "__floatsisf" : "__floatunsisf")
                            : (i->sign ? "__floatdisf" : "__floatundisf");
    return i->w <= 4 ? (i->sign ? "__fixsfsi" : "__fixunssfsi")
                     : (i->sign ? "__fixsfdi" : "__fixunssfdi");
}

/* An atomic read-modify-write: the interrupts masked around it, the PSW
 * saved on the stack (pushc/popc), as GCC's RX port does on a single
 * core. The access is `size` bytes at [TMP]. */
static int atomic_sz(struct rx_fn *F, const struct ir_ins *i)
{
    if (i->size != 1 && i->size != 2 && i->size != 4)
        rx_refuse(F, i, "an atomic wider than a register");
    return i->size == 1 ? RX_B : i->size == 2 ? RX_W : RX_L;
}

static void gen_ins(struct rx_fn *F, int n)
{
    struct ir_func *fn = F->fn;
    struct ir_ins *i = &fn->ins[n];
    struct code *t = F->t;

    if (target_debug_info() && fn->ins[n].line) {
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

    /* Floating point: binary32 everywhere, every operation a call. */
    if (i->flt && (i->op == IR_ADD || i->op == IR_SUB || i->op == IR_MUL ||
                   i->op == IR_DIV || i->op == IR_MOD || i->op == IR_NEG ||
                   i->op == IR_CMP || i->op == IR_SQRT)) {
        const char *name = fp_binop_name(i->op);
        if (i->w != 4)
            rx_refuse(F, i, "a floating-point value wider than binary32");
        if (name) {
            if (i->imm_b)
                rx_refuse(F, i, "a folded floating-point immediate");
            args2(F, i->a, i->b);
            call_helper(F, name);
            if (i->dst >= 0)
                wr(F, i->dst, RX_R1);
            return;
        }
        if (i->op == IR_NEG) {
            int d = wreg(F, i->dst, ACC);
            rd(F, i->a, d);
            rx_bit_i(t, RX_BNOT, 31, d);
            wrote(F, i->dst, d);
            return;
        }
        if (i->op == IR_CMP) {
            int d;
            if (i->imm_b)
                rx_refuse(F, i, "a folded floating-point immediate");
            args2(F, i->a, i->b);
            call_helper(F, fp_cmp_name(i->pred));
            rx_ri(t, RX_CMP, 0, RX_R1);
            d = wreg(F, i->dst, ACC);
            rx_scc(t, cond_of(i->pred, 1), d);
            wrote(F, i->dst, d);
            return;
        }
        if (i->op == IR_SQRT)
            rx_refuse(F, i, "__builtin_sqrt (a libm routine here, not an "
                            "instruction)");
        rx_refuse(F, i, "this floating-point operation");
    }

    if (i->w > 8)
        rx_refuse(F, i, "a 128-bit value");
    /* (before the 64-bit dispatch, so a long long one is named as what
     * it is) */
    if (i->op == IR_XCHG || i->op == IR_XADD || i->op == IR_ARMW ||
        i->op == IR_CAS || i->op == IR_CMPXCHG)
        (void)atomic_sz(F, i);
    /* Level 0 only (irgen): RX code keeps no frame-pointer chain. The
     * call pushed the return address, so the stack pointer at entry
     * points at it: that is the frame address (frame base + in_base - 4,
     * above the frame and the registers pushm saved), and the word there
     * is the return address. */
    if (i->op == IR_FRAMEADDR) {
        int d = i->dst >= 0 ? wreg(F, i->dst, ACC) : ACC;
        if (i->imm == 2)
            ld_sp(F, d, F->in_base - 4, 4, 0);
        else
            addr_sp(F, d, F->in_base - 4);
        if (i->dst >= 0)
            wrote(F, i->dst, d);
        return;
    }

    if (i->op == IR_SHR && F->nshr && i->dst >= 0 && F->nshr[i->dst]) {
        int k = (int)i->imm - 32, d = wreg(F, i->dst, ACC), hi;
        if (in_reg(F, i->a)) {
            hi = F->loc[i->a] + 1;
        } else {
            ld_sp(F, ACC, sslot(F, i->a) + 4, 4, 1);
            hi = ACC;
        }
        rx_shift_i(t, i->sign ? RX_SHAR : RX_SHLR, k, hi, d);
        wrote(F, i->dst, d);
        return;
    }
    {
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
                if (i->imm_b) {
                    args64x2(F, i->a, -1);
                    rx_ri(t, RX_MOV, (long)i->imm, RX_R3);
                    rx_ri(t, RX_MOV, (long)(i->imm >> 32), RX_R4);
                } else {
                    args64x2(F, i->a, i->b);
                }
                call_helper(F, i->op == IR_DIV
                            ? (i->sign ? "__divdi3" : "__udivdi3")
                            : (i->sign ? "__moddi3" : "__umoddi3"));
                wr64(F, i->dst, RX_R1, RX_R2);
                return;
            }
            if (i->op == IR_BSWAP) {
                rd64(F, i->a, A_LO, A_HI);
                rx_rr(t, RX_REVL, A_LO, SCR);
                rx_rr(t, RX_REVL, A_HI, A_LO);
                mv(F, A_HI, SCR);
                wr64(F, i->dst, A_LO, A_HI);
                return;
            }
            if (gen_ins64(F, n))
                return;
            rx_refuse(F, i, "this operation at 64 bits");
        }
        if (wide && (i->op == IR_CMP || i->op == IR_BRZ || i->op == IR_BRNZ))
            i->w = 8;
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
        rx_ri(t, RX_MOV, imm_val(i), d);
        wrote(F, i->dst, d);
        return;
    }
    case IR_BITCAST:
    case IR_MOV: {
        if (in_reg(F, i->dst)) {
            rd(F, i->a, F->loc[i->dst]);
        } else {
            int src = rdr(F, i->a, ACC);
            wrote(F, i->dst, src);
        }
        return;
    }

    case IR_ADD: case IR_SUB: case IR_MUL:
    case IR_AND: case IR_OR:  case IR_XOR: {
        int op = i->op == IR_ADD ? RX_ADD : i->op == IR_SUB ? RX_SUB
               : i->op == IR_AND ? RX_AND : i->op == IR_OR ? RX_OR
               : i->op == IR_XOR ? RX_XOR : RX_MUL;
        int three = op != RX_XOR;
        int d = wreg(F, i->dst, ACC);
        int ra_ = in_reg(F, i->a) ? F->loc[i->a] : -1;
        int rb_ = !i->imm_b && in_reg(F, i->b) ? F->loc[i->b] : -1;
        if (i->imm_b) {
            long v = imm_val(i);
            if ((op == RX_ADD || op == RX_SUB) && ra_ >= 0) {
                rx_add3(t, op == RX_SUB ? -v : v, ra_, d);
            } else {
                rd(F, i->a, d);
                rx_ri(t, op, v, d);
            }
        } else if (rb_ >= 0 && d == rb_ && d != ra_) {
            if (op == RX_SUB) {
                int a_ = ra_ >= 0 ? ra_ : rdr(F, i->a, other_scr(d));
                rx_rrr(t, RX_SUB, rb_, a_, d);         /* d = a - b */
            } else {
                op_v(F, op, i->a, d, other_scr(d));    /* commutative */
            }
        } else if (three && ra_ >= 0 && rb_ >= 0 && d != ra_) {
            rx_rrr(t, op, rb_, ra_, d);
        } else {
            rd(F, i->a, d);
            op_b(F, op, i, d, other_scr(d));
        }
        wrote(F, i->dst, d);
        return;
    }
    case IR_MULH: {
        /* the high word of a 32 x 32 product: emul(u) into r14:r15, and
         * r15 -- neither ever holds a value across an instruction */
        int rb_ = in_reg(F, i->b) ? F->loc[i->b] : SCR;
        if (rb_ == SCR)
            rd(F, i->b, SCR);
        rd(F, i->a, A_LO);
        rx_rr(t, i->sign ? RX_EMUL : RX_EMULU, rb_, A_LO);
        wrote(F, i->dst, A_HI);
        return;
    }
    case IR_DIV: case IR_MOD: {
        int op = i->sign ? RX_DIV : RX_DIVU;
        int rb_ = !i->imm_b && in_reg(F, i->b) ? F->loc[i->b] : -1;
        int d = wreg(F, i->dst, ACC);
        if (i->op == IR_DIV) {
            int q = d == rb_ ? ACC : d;
            rd(F, i->a, q);
            op_b(F, op, i, q, other_scr(q));
            mv(F, d, q);
        } else {
            int ra_;
            rd(F, i->a, ACC);
            op_b(F, op, i, ACC, TMP);                 /* q */
            op_b(F, RX_MUL, i, ACC, TMP);             /* q * b */
            ra_ = rdr(F, i->a, TMP);
            rx_rrr(t, RX_SUB, ACC, ra_, d);           /* a - q*b */
        }
        wrote(F, i->dst, d);
        return;
    }
    case IR_SHL: case IR_SHR: {
        int op = i->op == IR_SHL ? RX_SHLL : i->sign ? RX_SHAR : RX_SHLR;
        int d = wreg(F, i->dst, ACC);
        if (i->imm_b) {
            int ra_ = rdr(F, i->a, d);
            rx_shift_i(t, op, (int)(i->imm & 31), ra_, d);
        } else {
            int rc = rdr(F, i->b, TMP);
            int dd = d == rc ? ACC : d;
            if (dd == rc)
                dd = SCR;
            rd(F, i->a, dd);
            rx_rr(t, op, rc, dd);
            mv(F, d, dd);
        }
        wrote(F, i->dst, d);
        return;
    }
    case IR_NEG: case IR_BNOT: {
        int op = i->op == IR_NEG ? RX_NEG : RX_NOT;
        int d = wreg(F, i->dst, ACC);
        if (in_reg(F, i->a)) {
            if (F->loc[i->a] == d) rx_r(t, op, d);
            else                   rx_rr(t, op, F->loc[i->a], d);
        } else {
            rd(F, i->a, d);
            rx_r(t, op, d);
        }
        wrote(F, i->dst, d);
        return;
    }

    case IR_CMP: {
        struct ir_ins *nx = n + 1 < fn->nins ? &fn->ins[n + 1]
                                             : (struct ir_ins *)0;
        int fuse = nx && (nx->op == IR_BRZ || nx->op == IR_BRNZ) &&
                   nx->a == i->dst && F->usecnt && F->usecnt[i->dst] == 1;
        int cond;
        if (i->w == 8) {
            cond = cmp64_cond(F, i);
        } else {
            int ra_ = rdr(F, i->a, ACC);
            op_b(F, RX_CMP, i, ra_, other_scr(ra_));
            cond = cond_of(i->pred, i->sign);
        }
        if (fuse) {
            branch_to(F, nx->op == IR_BRZ ? rx_cond_invert(cond) : cond,
                      nx->label);
            F->skip_next = 1;
            return;
        }
        {
            int d = wreg(F, i->dst, ACC);
            rx_scc(t, cond, d);
            wrote(F, i->dst, d);
        }
        return;
    }

    case IR_SELECT: {
        /* dst = a ? b : c; the moves keep the flags */
        int d = wreg(F, i->dst, ACC), skip;
        int rb_ = in_reg(F, i->b) ? F->loc[i->b] : -1;
        int rc_ = in_reg(F, i->c) ? F->loc[i->c] : -1;
        if (i->size == 8) {
            rd64(F, i->a, A_LO, A_HI);
            rx_rr(t, RX_OR, A_HI, A_LO);
        } else {
            int ca = rdr(F, i->a, SCR);
            rx_ri(t, RX_CMP, 0, ca);
        }
        if (d == rb_ && d != rc_) {
            skip = br_place(F, RX_NE);
            rd(F, i->c, d);
            br_land(F, skip);
        } else {
            if (d == ACC && (rb_ < 0 || rc_ < 0) && i->size == 8)
                d = SCR;                     /* A_LO holds the condition */
            rd(F, i->c, d);
            skip = br_place(F, RX_EQ);
            rd(F, i->b, d);
            br_land(F, skip);
        }
        wrote(F, i->dst, d);
        return;
    }

    case IR_BRZ: case IR_BRNZ: {
        if (i->w == 8) {
            rd64(F, i->a, A_LO, A_HI);
            rx_rr(t, RX_OR, A_HI, A_LO);
        } else {
            int r = rdr(F, i->a, ACC);
            rx_ri(t, RX_CMP, 0, r);
        }
        branch_to(F, i->op == IR_BRZ ? RX_EQ : RX_NE, i->label);
        return;
    }

    case IR_LDVAR: {
        int d = wreg(F, i->dst, ACC);
        if (in_reg(F, i->a)) {
            if (rx_ldvar_plain(i->size, i->sign, i->w))
                mv(F, d, F->loc[i->a]);
            else
                ext_reg(F, d, F->loc[i->a], i->size, i->sign);
        } else {
            ld_sp(F, d, sslot(F, i->a), i->size, i->sign);
        }
        wrote(F, i->dst, d);
        return;
    }
    case IR_STVAR: {
        int src = rdr(F, i->a, ACC);
        if (in_reg(F, i->dst)) {
            if (i->size >= 4)
                mv(F, F->loc[i->dst], src);
            else
                ext_reg(F, F->loc[i->dst], src, i->size, 1);
        } else {
            st_sp(F, src, sslot(F, i->dst), i->size);
        }
        return;
    }
    case IR_LOAD: {
        int addr = rdr(F, i->a, TMP);
        int d = wreg(F, i->dst, ACC);
        ld_base(F, d, addr, i->memoff, i->size, i->sign);
        wrote(F, i->dst, d);
        return;
    }
    case IR_STORE: {
        int addr = rdr(F, i->a, TMP);
        int val = rdr(F, i->b, ACC);
        st_base(F, val, addr, i->memoff, i->size);
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
    /* An address is `mov.l #sym, rd`, its four-byte field relocated
     * R_RX_DIR32. */
    case IR_STRADDR: {
        int d = wreg(F, i->dst, ACC);
        int at = rx_mov_abs(t, d, 0);
        note_str(F->st, at, i->label, RK_ABS32);
        wrote(F, i->dst, d);
        return;
    }
    case IR_GADDR: {
        int d = wreg(F, i->dst, ACC);
        int at = rx_mov_abs(t, d, 0);
        note_glob(F->st, at, i->glob, RK_ABS32);
        wrote(F, i->dst, d);
        return;
    }
    case IR_FADDR: {
        int d = wreg(F, i->dst, ACC);
        int at = rx_mov_abs(t, d, 0);
        note_fn(F->st, at, i->callee, RK_ABS32);
        wrote(F, i->dst, d);
        return;
    }

    case IR_MEMCPY: case IR_MEMZERO:
        rd(F, i->a, ACC);
        if (i->op == IR_MEMCPY)
            rd(F, i->b, TMP);
        copy_block(F, i->op == IR_MEMCPY, i->size);
        return;

    case IR_CALL:
        gen_call(F, n);
        return;

    case IR_RET:
        if (i->a >= 0) {
            if (fn->ret_abi.is_struct) {
                if (!fn_sret(fn)) {
                    long size = fn->ret_abi.size;
                    int nw = (int)((size + 3) / 4);
                    rd(F, i->a, ACC);
                    for (int q = 0; q < nw; q++) {
                        long left = size - 4L * q;
                        int r = argreg(q);
                        if (left >= 4) {
                            ld_base(F, r, ACC, 4L * q, 4, 1);
                        } else {
                            rx_ri(t, RX_MOV, 0, r);
                            for (long b = 4L * q + left - 1; b >= 4L * q; b--) {
                                rx_shift_i(t, RX_SHLL, 8, r, r);
                                ld_base(F, TMP, ACC, b, 1, 0);
                                rx_rr(t, RX_OR, TMP, r);
                            }
                        }
                    }
                } else {
                    rd(F, i->a, TMP);
                    ld_sp(F, ACC, F->sret_slot, 4, 1);
                    copy_block(F, 1, fn->ret_abi.size);
                    ld_sp(F, RX_R1, F->sret_slot, 4, 1);
                }
            } else if (F->wide[i->a]) {
                rd64(F, i->a, RX_R1, RX_R2);
            } else {
                rd(F, i->a, RX_R1);
                /* a narrow result is extended by the callee (GCC's
                 * rx_function_value promotes it) */
                if (fn->ret_abi.size == 1 || fn->ret_abi.size == 2) {
                    const struct type *rt = fn->ret_abi.ty;
                    int uns = rt && (rt->is_unsigned || rt->kind == TY_BOOL);
                    ext_reg(F, RX_R1, RX_R1, fn->ret_abi.size, !uns);
                }
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
        rx_brk(t);
        return;
    case IR_FENCE:
        return;          /* one core, in order: nothing to order */

    case IR_BSWAP: {
        int ra_ = rdr(F, i->a, ACC);
        int d = wreg(F, i->dst, ACC);
        if (i->size == 2) {
            rx_rr(t, RX_REVW, ra_, d);
            rx_ext(t, RX_W, 0, d, d);
        } else {
            rx_rr(t, RX_REVL, ra_, d);
        }
        wrote(F, i->dst, d);
        return;
    }

    case IR_VA_START:
        rd(F, i->a, TMP);
        addr_sp(F, ACC, F->va_first);
        rx_store(t, RX_L, ACC, 0, TMP);
        return;

    case IR_I2F: case IR_F2I: case IR_F2F: {
        if (i->op == IR_F2F) {
            if (i->size != i->w)
                rx_refuse(F, i, "a conversion to or from a floating type "
                                "wider than binary32");
            rd(F, i->a, ACC);
            wr(F, i->dst, ACC);
            return;
        }
        if ((i->op == IR_I2F && i->w != 4) || (i->op == IR_F2I && i->size != 4))
            rx_refuse(F, i, "a floating-point value wider than binary32");
        if (i->op == IR_I2F && i->size == 8 && i->a >= 0 && !F->wide[i->a]) {
            rd(F, i->a, RX_R1);
            rx_ri(t, RX_MOV, 0, RX_R2);
        } else if (i->op == IR_I2F && i->size == 8) {
            args64x2(F, i->a, -1);
        } else {
            rd(F, i->a, RX_R1);
        }
        call_helper(F, cvt_name(i));
        if (i->dst >= 0) {
            if (F->wide[i->dst])
                wr64(F, i->dst, RX_R1, RX_R2);
            else
                wr(F, i->dst, RX_R1);
        }
        return;
    }

    case IR_ASM: {
        /* Extended asm, assembled in irgen (rx/irgen.c irg_asm_rx) against
         * the vocabulary in rx/asm.c. This only places the operands and
         * splices the bytes -- Xtensa's lowering with RX's registers.
         *
         * To the allocator (ra_target.asm_in_reg) a value live across an
         * asm keeps out of the registers it may change, which irgen
         * recorded (ir_asm.clob); a callee-saved one among them the
         * prologue saves. The operands are values like any other, moved
         * into and out of their registers here, each way as ONE parallel
         * move (ACC breaks a cycle). No operand is ever in r0 (sp), r5,
         * r14 or r15 (the scratch these moves and the frame accesses use)
         * or r13 (the frame base under alloca). */
        struct ir_asm *ia = i->asm_ir;
        int vreg_[16], vdst[16], nval = 0;
        /* A continuation's value was written by the asm before it, which
         * must be right there. */
        if (ia->cont) {
            int k = n - 1;
            while (k >= 0 && fn->ins[k].op == IR_ASM && fn->ins[k].asm_ir &&
                   fn->ins[k].asm_ir->cont)
                k--;
            if (k < 0 || fn->ins[k].op != IR_ASM)
                internal_error("rx: %s: an asm's further output is not "
                               "right after the asm", fn->name);
            return;
        }
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
        if (F->fb == FBREG && (ia->clob >> FBREG & 1))
            rx_refuse(F, i, "an asm that changes r13, the frame base of a "
                            "function that calls alloca");
        for (int k = 0; k < ia->nin; k++)
            if (ia->in[k].reg < RX_R1 || ia->in[k].reg == SCR ||
                ia->in[k].reg >= FBREG)
                internal_error("rx: %s: an asm operand in r%d", fn->name,
                               ia->in[k].reg);
        for (int k = 0; k < ia->nout; k++)
            if (!ia->out[k].mem && ia->out[k].size > 4)
                rx_refuse(F, i, "an asm output wider than a register");
        {
            int naddr = 0;
            for (int k = 0; k < ia->nout; k++)
                naddr += !ia->out[k].val && !ia->out[k].mem;
            if (ia->scr < 0 && naddr > 0)
                rx_refuse(F, i, "an asm with no scratch register left around "
                                "it");
        }
        /* In: an input's value, an "m" output's address, and a "+"
         * output's address (its current value is loaded through it
         * below) -- the register-resident ones as one parallel move, then
         * the rest from their slots. */
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
                int m = ra_parallel_move(pd, ps, npm, ACC, od, os,
                                         (int)(sizeof od / sizeof od[0]));
                if (m < 0)
                    rx_refuse(F, i, "an asm whose operands cannot be moved "
                                    "into place");
                for (int k = 0; k < m; k++)
                    mv(F, od[k], os[k]);
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
                    ld_base(F, o->reg, o->reg, 0, o->size, 1);
            }
        }
        for (int k = 0; k < ia->codelen; k++)
            code_byte(t, ia->code[k]);
        /* Out, through an address: the address is live across the asm
         * (regalloc.c counts it so), so it is still there. An "m" output
         * was written BY the template through the address its register
         * holds. */
        for (int k = 0; k < ia->nout; k++) {
            const struct ir_asm_op *o = &ia->out[k];
            if (o->mem || o->val)
                continue;
            rd(F, o->temp, ia->scr);
            st_base(F, o->reg, ia->scr, 0, o->size);
        }
        /* Out, as values: each to its home -- those in memory first,
         * while every operand register still holds what the asm left,
         * then the register-resident ones as one parallel move. */
        {
            int pd[16], ps[16], npm = 0;
            for (int k = 0; k < nval; k++) {
                /* a value nothing reads may share its home with one that
                 * is read: it is not moved at all */
                if (vdst[k] < 0 || (F->usecnt && F->usecnt[vdst[k]] == 0))
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
                int m = ra_parallel_move(pd, ps, npm, ACC, od, os,
                                         (int)(sizeof od / sizeof od[0]));
                if (m < 0)
                    rx_refuse(F, i, "an asm whose outputs cannot be moved "
                                    "into place");
                for (int k = 0; k < m; k++)
                    mv(F, od[k], os[k]);
            }
        }
        return;
    }

    /* ---- atomics, with the interrupts masked -------------------------- */
    case IR_XCHG: case IR_XADD: case IR_ARMW: {
        int sz = atomic_sz(F, i);
        rd(F, i->a, TMP);
        rd(F, i->b, ACC);
        rx_pushc(t, CR_PSW);
        rx_clrpsw(t, 8);
        rx_load(t, sz, 0, 0, TMP, SCR);               /* the old value */
        if (i->op == IR_XADD) {
            rx_rr(t, RX_ADD, SCR, ACC);
        } else if (i->op == IR_ARMW) {
            switch ((int)i->imm) {
            case '&': rx_rr(t, RX_AND, SCR, ACC); break;
            case '|': rx_rr(t, RX_OR, SCR, ACC); break;
            case '^': rx_rr(t, RX_XOR, SCR, ACC); break;
            default:  rx_rr(t, RX_AND, SCR, ACC); rx_r(t, RX_NOT, ACC); break;
            }
        }
        rx_store(t, sz, ACC, 0, TMP);
        rx_popc(t, CR_PSW);
        if (i->size < 4 && i->sign)
            ext_reg(F, SCR, SCR, i->size, 1);
        wr(F, i->dst, SCR);
        return;
    }
    case IR_CAS: case IR_CMPXCHG: {
        /* push r1 (a fourth register); TMP the address, ACC the expected
         * value, SCR the desired one, r1 what was there */
        int sz = atomic_sz(F, i), skip;
        rd(F, i->c, SCR);
        if (i->op == IR_CAS) {
            rd(F, i->b, ACC);
        } else {
            rd(F, i->b, TMP);
            rx_load(t, sz, 0, 0, TMP, ACC);
        }
        if (i->size < 4)
            ext_reg(F, ACC, ACC, i->size, 0);
        rd(F, i->a, TMP);
        rx_push(t, RX_R1);
        rx_pushc(t, CR_PSW);
        rx_clrpsw(t, 8);
        rx_load(t, sz, 0, 0, TMP, RX_R1);
        rx_rr(t, RX_CMP, ACC, RX_R1);
        rx_scc(t, RX_EQ, ACC);
        skip = br_place(F, RX_NE);
        rx_store(t, sz, SCR, 0, TMP);
        br_land(F, skip);
        rx_popc(t, CR_PSW);
        if (i->op == IR_CAS) {
            mv(F, ACC, RX_R1);
            if (i->size < 4 && i->sign)
                ext_reg(F, ACC, ACC, i->size, 1);
            rx_pop(t, RX_R1);
            wr(F, i->dst, ACC);
        } else {
            /* a failure writes what was seen back through b (not part of
             * the atomic access) */
            rx_ri(t, RX_CMP, 0, ACC);
            skip = br_place(F, RX_NE);
            mv(F, SCR, RX_R1);
            rx_pop(t, RX_R1);
            rd(F, i->b, TMP);
            rx_store(t, sz, SCR, 0, TMP);
            rx_ri(t, RX_MOV, 0, ACC);
            {
                int done = br_place(F, RX_ALWAYS);
                br_land(F, skip);
                rx_pop(t, RX_R1);
                br_land(F, done);
            }
            wr(F, i->dst, ACC);
        }
        return;
    }
    case IR_ALLOCA: {
        /* sp down by the size rounded to 16, then to a multiple of 16;
         * the block is above the outgoing area. */
        int d = wreg(F, i->dst, SCR);
        rd(F, i->a, ACC);
        rx_ri(t, RX_ADD, 15, ACC);
        rx_ri(t, RX_AND, -16, ACC);
        mv(F, TMP, RX_SP);
        rx_rr(t, RX_SUB, ACC, TMP);
        rx_ri(t, RX_AND, -16, TMP);
        mv(F, RX_SP, TMP);
        rx_add3(t, F->out_bytes, RX_SP, d);
        wrote(F, i->dst, d);
        return;
    }
    case IR_SPSAVE: {
        int d = wreg(F, i->dst, ACC);
        mv(F, d, RX_SP);
        wrote(F, i->dst, d);
        return;
    }
    case IR_SPRESTORE:
        mv(F, RX_SP, rdr(F, i->a, ACC));
        return;
    case IR_SWITCH:
        rx_refuse(F, i, "a jump table");
    case IR_LABELADDR: {
        if (cg_label_mark(i))       /* static data's marker: no code */
            return;
        /* &&label: the function's own address as IR_FADDR takes it, a
         * mov.l #imm32 (ABS32), plus the label's offset in it -- the
         * addend set once the function is laid out. The fix is no
         * branch: level -1, `at` the site's index. */
        int d = wreg(F, i->dst, ACC);
        int at = rx_mov_abs(t, d, 0), s0 = F->st->nf;
        note_fn(F->st, at, fn->src, RK_ABS32);
        want_label(F, s0, i->label, -1);
        wrote(F, i->dst, d);
        return;
    }
    case IR_IGOTO:
        rx_jmp(t, rdr(F, i->a, ACC));
        return;
    default:
        rx_refuse(F, i, "this operation");
    }
}

/* ---- register pairs (MIPS's arrangement) ------------------------------- */

static struct ra_range *g_rx_res;
static int g_rx_nres, g_rx_capres;

static void reserve_pairs(struct ir_func *fn, const int *loc)
{
    int nv = fn->nvregs;
    int *first = xmalloc((size_t)(nv ? nv : 1) * sizeof *first);
    int *last = xmalloc((size_t)(nv ? nv : 1) * sizeof *last);
    ra_live_ranges(fn, first, last);
    g_rx_nres = 0;
    for (int v = 0; v < nv; v++) {
        if (loc[v] < 0 || first[v] < 0) continue;
        if (g_rx_nres + 2 > g_rx_capres) {
            g_rx_capres = g_rx_capres ? g_rx_capres * 2 : 16;
            g_rx_res = xrealloc(g_rx_res,
                                (size_t)g_rx_capres * sizeof *g_rx_res);
        }
        for (int h = 0; h < 2; h++) {
            g_rx_res[g_rx_nres].reg = loc[v] + h;
            g_rx_res[g_rx_nres].first = first[v];
            g_rx_res[g_rx_nres].last = last[v];
            g_rx_res[g_rx_nres].born = 0;
            g_rx_nres++;
        }
    }
    ra_reserve(g_rx_res, g_rx_nres);
    free(first); free(last);
}

static int *pair_alloc(struct ir_func *fn, struct rx_fn *F, const char *pin)
{
    int nv = fn->nvregs, any = 0;
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
             F->wide[i->dst] && i->w != 8))
            x[i->op == IR_LDVAR ? i->a : i->dst] = 1;
    }
    F->npair = 0;
    if (!any) {
        free(x);
        return NULL;
    }
    loc = ra_allocate(fn, &RX_PAIR_RA, NULL, x, used, &nused);
    free(x);
    reserve_pairs(fn, loc);
    for (int k = 0; k < nused && k < RX_NPAIRS; k++)
        F->pair_used[F->npair++] = used[k];
    return loc;
}

/* ---- one function --------------------------------------------------------- */

static void gen_func(struct ir_func *fn, struct code *t, struct rx_sites *st,
                     int keep_vars)
{
    struct func *f = fn->src;
    struct rx_fn F;
    int i;

    memset(&F, 0, sizeof F);
    F.fn = fn; F.t = t; F.st = st;
    F.keep_vars = keep_vars;
    fn->nlines = 0;                 /* -g: this attempt's rows only */
    F.wide = wide_map(fn);
    F.nshr = ra_narrow_hishift(fn);
    for (int v = 0; v < fn->nvregs; v++)
        if (F.nshr[v]) F.wide[v] = 0;
    F.fb = RX_SP;
    if (g_rx_regalloc) {
        char *pin = keep_vars ? ra_debug_pin_vars(fn) : (char *)0;
        int *pair = g_rx_pairs ? pair_alloc(fn, &F, pin) : NULL;
        F.loc = ra_allocate(fn, &RX_RATGT, F.wide, pin, F.used_callee,
                            &F.nsave);
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
        if (fn->nvregs) {
            F.usecnt = xmalloc((size_t)fn->nvregs * sizeof *F.usecnt);
            ra_count_vreg_uses(fn, F.usecnt);
        }
        {
            const char *lim = getenv("EMBCC_RX_RA_MAX");
            if (lim) {
                int nl = atoi(lim);
                for (int v = nl; v < fn->nvregs; v++)
                    F.loc[v] = -1;
            }
        }
    }
    /* A callee-saved register an asm changes (its clobbers, the registers
     * its template names, its operands') is saved by the prologue, as GCC
     * saves it: the frame's pushm covers r6 up to the highest. */
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_asm *ia = fn->ins[n].op == IR_ASM ? fn->ins[n].asm_ir
                                                          : NULL;
        if (!ia || ia->cont)
            continue;
        for (int r = RX_R6; r <= RX_R13; r++) {
            int have = 0;
            if (!(ia->clob >> r & 1))
                continue;
            for (int k = 0; k < F.nsave; k++)
                have |= F.used_callee[k] == r;
            if (!have && F.nsave < RA_MAXPOOL)
                F.used_callee[F.nsave++] = r;
        }
    }
    /* only the callee-saved ones are pushed */
    {
        int k2 = 0;
        for (int k = 0; k < F.nsave; k++)
            if (rx_callee_saved(F.used_callee[k]))
                F.used_callee[k2++] = F.used_callee[k];
        F.nsave = k2;
    }
    if (fn->has_alloca)
        F.used_callee[F.nsave++] = FBREG;
    layout(&F);

    F.label_off = xmalloc((size_t)(fn->nlabels + 1) * sizeof *F.label_off);
    f->code_align = 1;
    if (target_debug_info()) {
        int nv = fn->nvars ? fn->nvars : 1;
        fn->var_off = xmalloc((size_t)nv * sizeof *fn->var_off);
        for (int v = 0; v < fn->nvars; v++)
            fn->var_off[v] = ra_var_home(fn, v, F.slot[v] >= 0, F.slot[v]);
    }
    {
    int len0 = t->len, nl0 = fn->nlines;
    int se0 = F.st->next, ss0 = F.st->nstr, sg0 = F.st->ng, sf0 = F.st->nf;
    char *level = NULL;
    int nlevel = 0;
    for (;;) {
    int nfail = 0;
    t->len = len0;
    fn->nlines = nl0;
    F.st->next = se0; F.st->nstr = ss0; F.st->ng = sg0; F.st->nf = sf0;
    F.nfix = 0;
    for (i = 0; i <= fn->nlabels; i++)
        F.label_off[i] = -1;
    F.skip_next = 0;
    F.fb = RX_SP;
    F.level = level;
    F.nlevel = nlevel;
    f->code_off = t->len;

    /* The prologue: push the callee-saved range, then the frame. */
    if (F.save_hi == RX_R6)
        rx_push(t, RX_R6);
    else if (F.save_hi)
        rx_pushm(t, RX_R6, F.save_hi);
    if (F.frame)
        rx_add3(t, -F.frame, RX_SP, RX_SP);
    if (fn->has_alloca) {
        mv(&F, FBREG, RX_SP);
        F.fb = FBREG;
    }

    /* The parameters: register ones as one parallel move into their homes
     * (SCR breaks a cycle), stack ones loaded after it. */
    {
        struct argplace pl;
        long cum = 0, stk = 0;
        int pmv_dst[RA_MAXPOOL], pmv_src[RA_MAXPOOL], npmv = 0;
        int pstk_reg[RA_MAXPOOL]; long pstk_off[RA_MAXPOOL];
        int pstk_size[RA_MAXPOOL], npstk = 0;
        long base = F.in_base;
        if (F.sret_slot >= 0)
            st_sp(&F, RX_R15, F.sret_slot, 4);
        for (i = 0; i < fn->nparams; i++) {
            struct ir_arg *a = &fn->param_abi[i];
            place_arg(a, param_named(fn, i), i == 0 && fn_sret_first(fn), &cum, &stk, &pl);
            if (pl.sret) {
                /* the C++ return slot: r15, kept in the sret slot */
                if (in_reg(&F, i)) {
                    pstk_reg[npstk] = F.loc[i];
                    pstk_off[npstk] = F.sret_slot;
                    pstk_size[npstk] = 4;
                    npstk++;
                } else if (F.slot[i] >= 0) {
                    ld_sp(&F, SCR, F.sret_slot, 4, 1);
                    st_sp(&F, SCR, sslot(&F, i), 4);
                }
                continue;
            }
            if (!a->is_struct) {
                int sz = a->size >= 4 ? 4 : a->size;
                if (a->size > 4 && in_reg(&F, i)) {
                    for (int q = 0; q < 2; q++) {
                        if (pl.nreg) {
                            pmv_dst[npmv] = F.loc[i] + q;
                            pmv_src[npmv] = argreg(pl.reg + q);
                            npmv++;
                        } else {
                            pstk_reg[npstk] = F.loc[i] + q;
                            pstk_off[npstk] = base + pl.stk + 4L * q;
                            pstk_size[npstk] = 4;
                            npstk++;
                        }
                    }
                } else if (a->size > 4) {
                    for (int q = 0; q < 2; q++) {
                        if (pl.nreg) {
                            st_sp(&F, argreg(pl.reg + q), sslot(&F, i) + 4L * q,
                                  4);
                        } else if (F.slot[i] >= 0) {
                            ld_sp(&F, SCR, base + pl.stk + 4L * q, 4, 1);
                            st_sp(&F, SCR, sslot(&F, i) + 4L * q, 4);
                        }
                    }
                } else if (pl.nreg && in_reg(&F, i)) {
                    pmv_dst[npmv] = F.loc[i];
                    pmv_src[npmv] = argreg(pl.reg);
                    npmv++;
                } else if (pl.nreg) {
                    if (F.slot[i] >= 0)
                        st_sp(&F, argreg(pl.reg), sslot(&F, i), sz);
                } else if (in_reg(&F, i)) {
                    pstk_reg[npstk] = F.loc[i];
                    pstk_off[npstk] = base + pl.stk;
                    pstk_size[npstk] = sz;
                    npstk++;
                } else if (F.slot[i] >= 0) {
                    ld_sp(&F, SCR, base + pl.stk, sz, 1);
                    st_sp(&F, SCR, sslot(&F, i), sz);
                }
                continue;
            }
            /* A composite: its words (or bytes) into its slot. */
            if (F.slot[i] < 0)
                continue;
            if (pl.nreg) {
                for (int q = 0; q < pl.nreg; q++) {
                    long off = sslot(&F, i) + 4L * q;
                    long left = a->size - 4L * q;
                    int r = argreg(pl.reg + q);
                    if (left >= 4) {
                        st_sp(&F, r, off, 4);
                    } else {
                        mv(&F, SCR, r);
                        for (long b = 0; b < left; b++) {
                            if (b) rx_shift_i(t, RX_SHLR, 8, SCR, SCR);
                            st_sp(&F, SCR, off + b, 1);
                        }
                    }
                }
            } else {
                long k = 0;
                for (; k + 4 <= a->size; k += 4) {
                    ld_sp(&F, SCR, base + pl.stk + k, 4, 1);
                    st_sp(&F, SCR, sslot(&F, i) + k, 4);
                }
                for (; k < a->size; k++) {
                    ld_sp(&F, SCR, base + pl.stk + k, 1, 0);
                    st_sp(&F, SCR, sslot(&F, i) + k, 1);
                }
            }
        }
        if (npmv) {
            int od[RA_MAXPOOL * 2], os[RA_MAXPOOL * 2];
            int m = ra_parallel_move(pmv_dst, pmv_src, npmv, SCR, od, os,
                                     (int)(sizeof od / sizeof od[0]));
            if (m < 0)
                internal_error("rx: %s: the prologue's parameter placement "
                               "is not a well-formed move", fn->name);
            for (int k = 0; k < m; k++)
                mv(&F, od[k], os[k]);
        }
        for (int k = 0; k < npstk; k++)
            ld_sp(&F, pstk_reg[k], pstk_off[k], pstk_size[k], 1);
        if (fn->is_varargs)
            F.va_first = base + stk;
    }

    for (i = 0; i < fn->nins; i++) {
        gen_ins(&F, i);
        if (F.skip_next) {
            F.skip_next = 0;
            i++;
        }
    }
    F.label_off[fn->nlabels] = t->len;
    if (fn->has_alloca) {
        mv(&F, RX_SP, FBREG);
        F.fb = RX_SP;
    }
    rx_restore(&F);

    for (i = 0; i < F.nfix; i++) {
        int target = F.label_off[F.fix[i].label];
        if (target < 0)
            internal_error("rx: label %d of %s was never placed",
                           F.fix[i].label, fn->name);
        if (F.fix[i].level < 0) {                     /* &&label */
            F.st->f[F.fix[i].at].addend = target - f->code_off;
            continue;
        }
        if (!rx_patch_branch(t, F.fix[i].at, target)) {
            if (nlevel < F.nfix) {
                level = xrealloc(level, (size_t)F.nfix);
                memset(level + nlevel, 0, (size_t)(F.nfix - nlevel));
                nlevel = F.nfix;
            }
            if (level[i] >= 2)
                internal_error("rx: %s: a branch does not reach even as "
                               "bra.a", fn->name);
            level[i] = (char)(F.fix[i].level + 1);
            nfail++;
        }
    }
    if (!nfail)
        break;
    }
    free(level);
    }

    f->code_len = t->len - f->code_off;
    f->stack_bytes = (int)(F.in_base - 4);
    free(F.usecnt);
    free(F.slot);
    cg_note_labels(fn, F.label_off);
    free(F.label_off);
    free(F.fix);
    free(F.wide);
    free(F.nshr);
    free(F.loc);
}

static void gen_func_best(struct ir_func *fn, struct code *t,
                          struct rx_sites *st, int keep_vars)
{
    int at = t->len, next = st->next, nstr = st->nstr, ng = st->ng,
        nf = st->nf, with;

    if (g_rx_regalloc && !keep_vars && !getenv("EMBCC_NO_MEMOFF")) {
        char *w = wide_map(fn);
        ra_fold_memoff(fn, 0, 32767, 4, 4, w, 0, 0);
        free(w);
    }
    g_rx_pairs = 1;
    if (!g_rx_regalloc || keep_vars || getenv("EMBCC_RX_PAIRS")) {
        if (getenv("EMBCC_RX_PAIRS"))
            g_rx_pairs = atoi(getenv("EMBCC_RX_PAIRS"));
        gen_func(fn, t, st, keep_vars);
        g_rx_pairs = 1;
        return;
    }
    gen_func(fn, t, st, keep_vars);
    with = t->len - at;
    t->len = at; st->next = next; st->nstr = nstr; st->ng = ng; st->nf = nf;
    g_rx_pairs = 0;
    gen_func(fn, t, st, keep_vars);
    if (t->len - at > with) {
        t->len = at; st->next = next; st->nstr = nstr; st->ng = ng;
        st->nf = nf;
        g_rx_pairs = 1;
        gen_func(fn, t, st, keep_vars);
    }
    g_rx_pairs = 1;
}

void codegen_unit_rx(struct ir_unit *iu, struct code *text,
                     struct extcall **ext, int *next,
                     struct strsite **strs, int *nstrs,
                     struct gsite **gs, int *ngs,
                     struct fsite **fs, int *nfs, int keep_vars,
                     int optimize, int no_sse, int regalloc)
{
    struct rx_sites st;

    (void)optimize; (void)no_sse;
    g_rx_regalloc = regalloc;
    memset(&st, 0, sizeof st);
    for (int n = 0; n < iu->nfuncs; n++)
        gen_func_best(&iu->funcs[n], text, &st, keep_vars);
    cg_resolve_strsites(iu, st.str, st.nstr);
    *ext = st.ext;   *next = st.next;
    *strs = st.str;  *nstrs = st.nstr;
    *gs = st.g;      *ngs = st.ng;
    *fs = st.f;      *nfs = st.nf;
}
