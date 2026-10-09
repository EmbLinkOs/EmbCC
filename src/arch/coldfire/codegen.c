/* ColdFire code generation: ISA_A with the hardware divide, the m68k SVR4
 * convention as GCC's m68k-elf implements it, big-endian, soft float
 * (docs/internals/coldfire-plan.md).
 *
 * Every vreg has one home -- a register the shared allocator gave it, or
 * a frame slot -- and every operation reads its operands through vea/rdr
 * and writes its result through wreg/wrote, so the code is correct with
 * the allocator off and smaller with it on (the MIPS and PowerPC backends'
 * shape). What the m68k changes, and where:
 *
 *   * An operand is an EFFECTIVE ADDRESS, not a register: `add.l
 *     -8(%fp),%d2` reads a frame slot directly, so a value in memory costs
 *     no load (vea). The ALU writes a data register only, and ColdFire's
 *     immediates go into a data register only.
 *   * Two register files. The allocator's data class is d2-d7; its
 *     address class, a2-a5, holds values used as pointers. d0, d1, a0
 *     and a1 are the scratch, the results and the helpers' clobbers.
 *   * Every argument is on the stack. The callee finds its parameters in
 *     the caller's argument words and uses them as their homes: a
 *     parameter's slot is 8(%fp) upward, a char's byte the word's last.
 *     The caller stores its arguments into an area at the bottom of its
 *     frame, the widest call's, rather than pushing at each call.
 *   * Every function links a6, so the frame is addressed from it whatever
 *     an alloca does to the stack pointer, and the callee-saved registers
 *     are saved with one movem a6-relative.
 *   * A comparison sets the condition codes; a 0/1 value is scc, extb.l,
 *     neg.l. There are no rotates, no byte or word arithmetic, no 64-bit
 *     product: a 64-bit multiply, divide and variable shift are calls.
 *   * The atomics mask interrupts around a plain read-modify-write (ISA_A
 *     has no compare-and-swap), which needs supervisor mode.
 *
 * Inline assembly is assembled in irgen (coldfire/irgen.c, against
 * coldfire/asm.c); IR_ASM here moves its operands and splices its bytes.
 *
 * Refused by name: a frame larger than 32 KiB, a branch beyond 32 KiB,
 * a _Complex double result, atomics wider than four bytes, __int128
 * (ILP32), and C++. THE RULE.
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

/* The scratch registers, none of them in the allocator's pools: d0 and d1
 * (the result pair, d0 the high word of a 64-bit one), a0 and a1. */
#define D0 CF_D0
#define D1 CF_D1
#define A0 CF_A0
#define A1 CF_A1

#define NOSLOT 0x7fffffffL

struct cf_sites {
    struct extcall *ext;   int next, capext;
    struct strsite *str;   int nstr, capstr;
    struct gsite *g;       int ng, capg;
    struct fsite *f;       int nf, capf;
};

enum { FX_B, FX_TAB, FX_LONG };

struct cf_fn {
    struct ir_func *fn;
    struct code *t;
    struct cf_sites *st;
    int keep_vars;
    int *loc;            /* per vreg: its register (0-15), -1 in memory */
    int *usecnt;
    int used_callee[2 * RA_MAXPOOL];
    int nsave;
    char *wide;
    char *nshr;
    long *slot;          /* per vreg: its a6-relative home, or NOSLOT */
    long frame;          /* link's displacement, negated */
    long out_bytes;      /* the argument area at the bottom of the frame */
    long save_at;        /* the callee-saved registers, a6-relative */
    long scratch_at;     /* returned structures */
    long sret_slot;      /* the caller's buffer address, from a1 */
    long tmp_slot;       /* 8 bytes: bswap, the atomics' saved sr, a divisor */
    long va_named;       /* the named parameters' argument bytes */
    int skip_next;
    int *label_off;
    struct { int at; int label; int kind; int base; } *fix;
    int nfix, capfix;
    const char *form;    /* per fix: 0 bcc.w, 1 bcc.b, 2 the long form */
    int nform;
};

static void copy_block(struct cf_fn *F, int copy, long size);

/* ---- refusal ------------------------------------------------------------ */

static void cf_refuse(const struct cf_fn *F, const struct ir_ins *i,
                      const char *what)
{
    char op[64];
    op[0] = 0;
    if (i)
        snprintf(op, sizeof op, " [%s w=%d size=%d]", ir_opname(i->op),
                 i->w, i->size);
    fprintf(stderr,
            "embcc: %s:%d: error: the ColdFire backend cannot lower %s yet "
            "(function %s)%s\n",
            F->fn->file ? F->fn->file : "?",
            i ? i->line : F->fn->line, what, F->fn->name, op);
    exit(1);
}

/* ---- the allocator's view of this machine ------------------------------
 *
 * The data class: d2-d7, all callee-saved (GCC's m68k convention keeps
 * only d0/d1 and a0/a1 for the caller to lose, and those are this
 * backend's scratch). The address class: a2-a5 (a6 is the frame pointer). */
#define CF_NPOOL 6
static const int CF_POOL[CF_NPOOL] = { 2, 3, 4, 5, 6, 7 };
#define CF_NAPOOL 4
static const int CF_APOOL[CF_NAPOOL] = { 10, 11, 12, 13 };
static int g_cf_pool[CF_NPOOL];
static int g_cf_apool[CF_NAPOOL];
static int g_cf_regalloc;
static int g_cf_maxd = CF_NPOOL, g_cf_maxa = CF_NAPOOL;

static int uses_d2d3_result(const struct ir_func *fn);

static const int *cf_pool_for(const struct ir_func *fn, int *n)
{
    int k = 0, skip23 = uses_d2d3_result(fn);
    for (int j = 0; j < CF_NPOOL && k < g_cf_maxd; j++)
        if (!(skip23 && (CF_POOL[j] == 2 || CF_POOL[j] == 3)))
            g_cf_pool[k++] = CF_POOL[j];
    *n = k;
    return g_cf_pool;
}

static const int *cf_apool_for(const struct ir_func *fn, int *n)
{
    int k = 0;
    (void)fn;
    for (int j = 0; j < CF_NAPOOL && k < g_cf_maxa; j++)
        g_cf_apool[k++] = CF_APOOL[j];
    *n = k;
    return g_cf_apool;
}

static int cf_callee_saved(int r)
{
    return (r >= 2 && r <= 7) || (r >= 10 && r <= 14);
}

static int cf_ldvar_plain(int size, int sign, int w)
{
    (void)sign;
    return size == 4 && w == 4;
}

/* Which instructions become a CALL the IR does not show as one: every
 * floating-point operation (soft float), and a 64-bit multiply, divide
 * and variable shift. Every pool register is callee-saved, so this only
 * keeps the optimizer's view of the calls true. */
int cf_op_calls_helper(const struct ir_ins *i)
{
    if (i->flt)
        return i->op == IR_ADD || i->op == IR_SUB || i->op == IR_MUL ||
               i->op == IR_DIV || i->op == IR_CMP;
    if (i->op == IR_I2F || i->op == IR_F2I || i->op == IR_F2F)
        return 1;
    if (i->w == 8 && (i->op == IR_DIV || i->op == IR_MOD || i->op == IR_MUL))
        return 1;
    /* a 64-bit shift: inline by 0, by 32 or more, and left by 1 (gen_ins64);
     * every other count is __ashldi3 and friends. The frame's argument
     * area is sized by this answer, so it must be gen_ins64's exactly. */
    if (i->w == 8 && (i->op == IR_SHL || i->op == IR_SHR)) {
        long k = i->imm & 63;
        if (!i->imm_b)
            return 1;
        return !(k == 0 || k >= 32 || (k == 1 && i->op == IR_SHL));
    }
    return 0;
}

static const struct ra_target CF_RATGT = {
    cf_pool_for,
    cf_callee_saved,
    cf_ldvar_plain,
    1, 1, 1,
    cf_op_calls_helper,
    1,              /* two-operand: d = a; d op= b */
    NULL,           /* every argument is on the stack: nothing to hint */
    cf_apool_for,   /* the second class: the address registers */
    cf_callee_saved,
    1,              /* soft float: a float is bits in a data register */
    NULL, NULL,
    1,              /* atomic_in_reg */
    0,
    1,              /* asm_in_reg: see IR_ASM (the address class keeps the
                     * old rule: nothing of it lives across an asm) */
    NULL  /* remat_ok */
};

/* ---- which values are eight bytes wide --------------------------------- */

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

/* ---- the argument words --------------------------------------------------
 *
 * Each argument at the next offset in whole words: a scalar of four bytes
 * or fewer one word, a 64-bit scalar two (high first), a composite its
 * size rounded up to a word. A composite smaller than a word is
 * right-justified in it (pad_of). */
static long arg_words(const struct ir_arg *a)
{
    if (a->is_struct)
        return ((long)a->size + 3) & ~3L;
    return a->size > 4 ? 8 : 4;
}

static long pad_of(const struct ir_arg *a)
{
    return a->is_struct && a->size < 4 ? 4 - a->size : 0;
}

/* A _Complex float or double comes back in registers: to GCC it is not an
 * aggregate, so DEFAULT_PCC_STRUCT_RETURN does not send it to memory, and
 * m68k_function_value gives its mode d0 on -- d0 (real) and d1 (imaginary)
 * for a float, d0:d1 (real) and d2:d3 (imaginary) for a double. How many
 * registers, or 0 for memory. */
static int cplx_regs(const struct type *t, long size)
{
    if (!t || t->kind != TY_STRUCT || !t->is_complex)
        return 0;
    return size == 8 ? 2 : size == 16 ? 4 : 0;
}

/* The C++ indirect-result pointer (sret_first: the return slot of a
 * class that is not trivially copyable, which the C++ lowering passes
 * first) travels as a struct's result buffer does: in a1, taking no
 * argument word. */
static int fn_sret_first(const struct ir_func *fn)
{
    return fn->src && fn->src->sret_first;
}

static int fn_sret(const struct ir_func *fn)
{
    return fn->ret_abi.is_struct && !cplx_regs(fn->ret_abi.ty,
                                               fn->ret_abi.size);
}

static int call_sret(const struct ir_ins *i)
{
    return i->retsize && !cplx_regs(i->rety, i->retsize);
}

/* Does fn return, or call something that returns, a _Complex double? Its
 * imaginary part is in d2:d3, which then are not callee-saved values but
 * results: the function keeps them out of its pool, so it never saves,
 * restores or keeps a value in them across that call. */
static int uses_d2d3_result(const struct ir_func *fn)
{
    if (cplx_regs(fn->ret_abi.ty, fn->ret_abi.size) == 4)
        return 1;
    for (int n = 0; n < fn->nins; n++)
        if (fn->ins[n].op == IR_CALL &&
            cplx_regs(fn->ins[n].rety, fn->ins[n].retsize) == 4)
            return 1;
    return 0;
}

/* ---- the frame -------------------------------------------------------------
 *
 * a6-relative: the parameters at 8(%fp) upward (the caller's words), the
 * return address at 4, the caller's a6 at 0; below, the temps' shared
 * slots, 64-bit temps, the locals, the returned-structure scratch, the
 * sret pointer, the 8-byte tmp slot, then the callee-saved registers; and
 * at the stack pointer the argument area of the widest call. */
static int in_reg(const struct cf_fn *F, int v)
{
    return F->loc && v >= 0 && v < F->fn->nvregs && F->loc[v] >= 0;
}

static long out_area(const struct cf_fn *F)
{
    const struct ir_func *fn = F->fn;
    long most = 0;
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        long stk = 0;
        if (i->op == IR_CALL) {
            for (int k = i->sret_first ? 1 : 0; k < i->nargs; k++)
                stk += arg_words(&i->argv[k]);
        } else if (cf_op_calls_helper(i)) {
            stk = 16;            /* two doubles, the most any helper takes */
        }
        if (stk > most)
            most = stk;
    }
    return most;
}

static int needs_tmp(const struct cf_fn *F)
{
    const struct ir_func *fn = F->fn;
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        switch (i->op) {
        case IR_BSWAP: case IR_XCHG: case IR_XADD: case IR_ARMW:
        case IR_CAS: case IR_CMPXCHG:
            return 1;
        case IR_DIV: case IR_MOD:
            /* an immediate divisor, and a remainder's divisor in an
             * address register, go through the slot */
            if (i->imm_b || (i->op == IR_MOD && in_reg(F, i->b) &&
                             CF_IS_A(F->loc[i->b])))
                return 1;
            break;
        default:
            break;
        }
    }
    return 0;
}

static void layout(struct cf_fn *F)
{
    struct ir_func *fn = F->fn;
    long off = 0;                   /* grows down from a6 */
    long pstk = 8;

    F->slot = xmalloc((size_t)(fn->nvregs ? fn->nvregs : 1) * sizeof *F->slot);
    for (int v = 0; v < fn->nvregs; v++)
        F->slot[v] = NOSLOT;

    /* The parameters live where the caller put them. */
    for (int p = 0; p < fn->nparams && p < fn->nvregs; p++) {
        const struct ir_arg *a = &fn->param_abi[p];
        if (p == 0 && fn_sret_first(fn))
            continue;            /* in a1: the sret slot, below */
        F->slot[p] = pstk + pad_of(a);
        pstk += arg_words(a);
    }
    F->va_named = pstk - 8;

    {
        int nv = fn->nvregs, npool = 0, has_cgoto = 0;
        int *loc2 = xmalloc((size_t)(nv ? nv : 1) * sizeof *loc2);
        for (int v = 0; v < nv; v++)
            loc2[v] = in_reg(F, v) || F->wide[v] ? 0 : -1;
        for (int n = 0; n < fn->nins; n++)
            if (fn->ins[n].op == IR_IGOTO || fn->ins[n].op == IR_LABELADDR)
                has_cgoto = 1;
        {
            struct ra_slots so = { loc2, NULL, g_cf_regalloc, has_cgoto };
            int *tslot = ra_coalesce_temps(fn, fn->nvars, &so, &npool);
            off -= (long)npool * 4;
            for (int v = fn->nvars; v < nv; v++) {
                int k = v - fn->nvars;
                if (loc2[v] >= 0 || !tslot || tslot[k] < 0)
                    continue;
                F->slot[v] = off + (long)tslot[k] * 4;
            }
            free(tslot);
        }
        for (int v = fn->nvars; v < nv; v++) {
            if (!F->wide[v] || in_reg(F, v))
                continue;
            off -= 8;
            F->slot[v] = off;
        }
        free(loc2);
    }
    {
        char *lref = ra_locals_referenced(fn, F->keep_vars);
        for (int v = fn->nparams; v < fn->nvars; v++) {
            int size = fn->locals[v].size ? fn->locals[v].size : 4;
            int align = fn->locals[v].user_align ? fn->locals[v].user_align
                      : fn->locals[v].align ? fn->locals[v].align : 2;
            if (in_reg(F, v) || !lref[v] ||
                ra_slot_dead(fn, F->loc, NULL, v, F->keep_vars))
                continue;
            if (size < 4 && fn->locals[v].is_int_or_ptr)
                size = 4;           /* a whole word: obj_slot */
            if (align < 4 && size >= 4)
                align = 4;          /* the m68k needs 2; 4 is faster */
            if (align > 4)
                cf_refuse(F, NULL, "a local aligned beyond the 4-byte stack");
            off -= size;
            off &= ~(long)(align - 1);
            F->slot[v] = off;
        }
        free(lref);
    }
    off &= ~3L;
    off -= ((long)fn->scratch_bytes + 3) & ~3L;
    F->scratch_at = off;
    F->sret_slot = NOSLOT;
    if (fn_sret(fn) || fn_sret_first(fn)) {
        off -= 4;
        F->sret_slot = off;
        if (fn_sret_first(fn) && fn->nparams > 0 && fn->nvregs > 0)
            F->slot[0] = off;    /* the return slot's own home */
    }
    F->tmp_slot = NOSLOT;
    if (needs_tmp(F)) {
        off -= 8;
        F->tmp_slot = off;
    }
    off -= 4L * F->nsave;
    F->save_at = off;
    F->out_bytes = out_area(F);
    F->frame = -off + F->out_bytes;
    if (F->frame > 32767)
        cf_refuse(F, NULL, "a stack frame larger than 32 KiB (link.w and "
                           "every frame access take a 16-bit displacement)");
}

/* ---- operands ----------------------------------------------------------- */

static int is_wide(const struct cf_fn *F, int v)
{
    return F->wide && v >= 0 && v < F->fn->nvregs && F->wide[v];
}

static long sslot(const struct cf_fn *F, int v)
{
    if (v < 0 || v >= F->fn->nvregs || F->slot[v] == NOSLOT)
        internal_error("coldfire: %s: a path addresses vreg %d's slot, and "
                       "it has none", F->fn->name, v);
    return F->slot[v];
}

static struct cf_ea fp_at(long off)
{
    return cf_disp16(CF_FP, off);
}

/* A 64-bit vreg read or written at 32 bits is its LOW word: big-endian,
 * the word at +4 of its slot. */
static long slot32(const struct cf_fn *F, int v)
{
    return sslot(F, v) + (is_wide(F, v) ? 4 : 0);
}

/* A narrow integer VARIABLE's object is at the END of its four-byte home,
 * which is also read and written as a whole word: big-endian, its low
 * bytes are the word's last (a char parameter's byte is 11(%fp)). */
static long obj_slot(const struct cf_fn *F, int v)
{
    if (v < F->fn->nvars) {
        const struct ir_local *L = &F->fn->locals[v];
        if (L->is_int_or_ptr && L->size > 0 && L->size < 4)
            return sslot(F, v) + 4 - L->size;
    }
    return sslot(F, v);
}

static long var_slot(const struct cf_fn *F, int v, int size)
{
    long vs = v < F->fn->nvars ? F->fn->locals[v].size
            : is_wide(F, v) ? 8 : 4;
    return obj_slot(F, v) + (vs > size ? vs - size : 0);
}

static struct cf_ea reg_ea(int r)
{
    return CF_IS_A(r) ? cf_areg(r) : cf_dreg(r);
}

/* The 32-bit home of v as an operand. */
static struct cf_ea vea(const struct cf_fn *F, int v)
{
    if (in_reg(F, v))
        return reg_ea(F->loc[v]);
    return fp_at(slot32(F, v));
}

/* ...and its low `size` bytes. */
static struct cf_ea vea_n(const struct cf_fn *F, int v, int size)
{
    if (in_reg(F, v))
        return reg_ea(F->loc[v]);
    return fp_at(slot32(F, v) + 4 - size);
}

static int dreg_of(const struct cf_fn *F, int v)
{
    return in_reg(F, v) && !CF_IS_A(F->loc[v]) ? F->loc[v] : -1;
}

static int ea_is_dreg(const struct cf_ea *e, int r)
{
    return e->mode == CFM_D && e->reg == r;
}

/* Is e register r (a data or an address register)? */
static int ea_is_reg(const struct cf_ea *e, int r)
{
    return (e->mode == CFM_D || e->mode == CFM_A) && e->reg == r;
}

static int ea_same(const struct cf_ea *a, const struct cf_ea *b)
{
    if (a->mode != b->mode)
        return 0;
    switch (a->mode) {
    case CFM_D: case CFM_A: case CFM_IND:
        return a->reg == b->reg;
    case CFM_DISP:
        return a->reg == b->reg && a->disp == b->disp;
    default:
        return 0;
    }
}

/* move.<size> src,dst, through data register `scr` where ColdFire has no
 * one-instruction form (two long effective addresses, a byte of an
 * address register). */
static void mv(struct cf_fn *F, int size, struct cf_ea src, struct cf_ea dst,
               int scr)
{
    if (ea_same(&src, &dst))
        return;
    if (cf_move_ok(size, &src, &dst)) {
        cf_move(F->t, size, src, dst);
        return;
    }
    if (src.mode == CFM_A && size < 4) {
        cf_move(F->t, 4, src, cf_dreg(scr));
        cf_move(F->t, size, cf_dreg(scr), dst);
        return;
    }
    cf_move(F->t, size, src, cf_dreg(scr));
    cf_move(F->t, size, cf_dreg(scr), dst);
}

/* A 32-bit constant into an operand. */
static void ldi(struct cf_fn *F, long v, struct cf_ea dst, int scr)
{
    v = (long)(int)(v & 0xffffffffL);
    if (dst.mode == CFM_D) {
        if (v >= -128 && v <= 127)
            cf_moveq(F->t, v, dst.reg);
        else
            cf_move(F->t, 4, cf_imm(v), dst);
        return;
    }
    if (dst.mode == CFM_A) {
        if (v == 0)
            cf_alua(F->t, CF_SUB, dst, dst.reg);
        else if (v >= -32768 && v <= 32767)
            cf_move(F->t, 2, cf_imm(v), dst);       /* movea.w: extended */
        else
            cf_move(F->t, 4, cf_imm(v), dst);
        return;
    }
    if (v == 0) {
        cf_clr(F->t, 4, dst);
        return;
    }
    {
        struct cf_ea im = cf_imm(v);
        if (cf_move_ok(4, &im, &dst)) {
            cf_move(F->t, 4, im, dst);
            return;
        }
    }
    ldi(F, v, cf_dreg(scr), scr);
    cf_move(F->t, 4, cf_dreg(scr), dst);
}

/* v's value in a data register: its own, or loaded into `scr`. */
static int rdr(struct cf_fn *F, int v, int scr)
{
    int r = dreg_of(F, v);
    if (r >= 0)
        return r;
    cf_move(F->t, 4, vea(F, v), cf_dreg(scr));
    return scr;
}

static void rd(struct cf_fn *F, int v, int r)
{
    struct cf_ea e = vea(F, v);
    if (!ea_is_reg(&e, r))
        cf_move(F->t, 4, e, reg_ea(r));
}

/* The data register to compute v's new value in. */
static int wreg(struct cf_fn *F, int v, int scr)
{
    int r = dreg_of(F, v);
    return r >= 0 ? r : scr;
}

/* v's value is in register r (data or address): put it home. */
static void wrote(struct cf_fn *F, int v, int r)
{
    if (v < 0)
        return;
    if (in_reg(F, v)) {
        if (F->loc[v] != r)
            cf_move(F->t, 4, reg_ea(r), reg_ea(F->loc[v]));
        return;
    }
    if (F->slot[v] == NOSLOT)
        return;
    cf_move(F->t, 4, reg_ea(r), fp_at(slot32(F, v)));
}

/* An address register holding v (an address): its own, or `scr` loaded. */
static int areg(struct cf_fn *F, int v, int scr)
{
    if (in_reg(F, v) && CF_IS_A(F->loc[v]))
        return F->loc[v];
    cf_move(F->t, 4, vea(F, v), cf_areg(scr));
    return scr;
}

/* ---- 64-bit halves -------------------------------------------------------- */

static struct cf_ea hi_ea(const struct cf_fn *F, int v)
{
    if (!is_wide(F, v))
        internal_error("coldfire: %s: vreg %d is read as 64 bits and is not "
                       "a 64-bit value", F->fn->name, v);
    return fp_at(sslot(F, v));
}

static struct cf_ea lo_ea(const struct cf_fn *F, int v)
{
    if (!is_wide(F, v))
        internal_error("coldfire: %s: vreg %d is read as 64 bits and is not "
                       "a 64-bit value", F->fn->name, v);
    return fp_at(sslot(F, v) + 4);
}

static void rd64(struct cf_fn *F, int v, int hi, int lo)
{
    cf_move(F->t, 4, hi_ea(F, v), cf_dreg(hi));
    cf_move(F->t, 4, lo_ea(F, v), cf_dreg(lo));
}

static void wr64(struct cf_fn *F, int v, int hi, int lo)
{
    if (v < 0 || F->slot[v] == NOSLOT)
        return;
    cf_move(F->t, 4, cf_dreg(hi), hi_ea(F, v));
    cf_move(F->t, 4, cf_dreg(lo), lo_ea(F, v));
}

static long long imm_hi(const struct ir_ins *i)
{
    return (long long)(int)(unsigned int)((unsigned long)i->imm >> 32);
}

static long long imm_lo(const struct ir_ins *i)
{
    return (long long)(int)(unsigned int)(unsigned long)i->imm;
}

static long imm_val(const struct ir_ins *i)
{
    return (long)(int)(unsigned int)(unsigned long)i->imm;
}

/* ---- branches ------------------------------------------------------------
 *
 * A branch to a label is a bcc.w resolved when the function ends. One that
 * does not reach 32 KiB is given the long form and the function generated
 * again, until nothing new fails; then once more with every branch that
 * reached within 8 bits in its 2-byte form (gen_func). Shrinking only
 * brings code closer, so a branch that fit still does.
 *
 * The long form is position-independent, with no relocation: the inverse
 * condition's bcc.b over
 *
 *     lea (0,%pc),%a0 ; adda.l #(target - that word),%a0 ; jmp (%a0)
 *
 * (bra's has no bcc.b). a0 is free between instructions: the epilogue
 * sets a returned pointer's copy in a0 itself, after the branch to it. */
static void want_label(struct cf_fn *F, int at, int label, int kind, int base)
{
    if (F->nfix == F->capfix) {
        F->capfix = F->capfix ? F->capfix * 2 : 16;
        F->fix = xrealloc(F->fix, (size_t)F->capfix * sizeof *F->fix);
    }
    F->fix[F->nfix].at = at;
    F->fix[F->nfix].label = label;
    F->fix[F->nfix].kind = kind;
    F->fix[F->nfix].base = base;
    F->nfix++;
}

static void branch_to(struct cf_fn *F, int cond, int label)
{
    int at = F->t->len;
    int form = F->form && F->nfix < F->nform ? F->form[F->nfix] : 0;
    if (form == 2) {
        int ext, imm;
        if (cond != CF_T)
            cf_bcc_b(F->t, cf_cond_invert(cond), 12);
        ext = F->t->len + 2;
        cf_lea(F->t, cf_pcdisp(0), A0);
        imm = F->t->len + 2;
        cf_alua(F->t, CF_ADD, cf_imm(0), A0);
        cf_jmp(F->t, cf_ind(A0));
        want_label(F, imm, label, FX_LONG, ext);
        return;
    }
    if (form == 1)
        cf_w(F->t, 0x6000u | (unsigned)((cond & 15) << 8) | 0x7fu);
    else
        cf_bcc_w(F->t, cond, 0);
    want_label(F, at, label, FX_B, 0);
}

/* A branch inside one lowering, patched to land here. */
static int br_place(struct cf_fn *F, int cond)
{
    return cf_bcc_placeholder(F->t, cond);
}

static void br_land(struct cf_fn *F, int at)
{
    if (!cf_patch_bcc(F->t, at, F->t->len))
        internal_error("coldfire: %s: a branch inside one operation does "
                       "not reach", F->fn->name);
}

static void br_back(struct cf_fn *F, int cond, int target)
{
    long d = (long)target - (F->t->len + 2);
    if (d >= -128 && d <= 127)
        cf_bcc_b(F->t, cond, d);
    else
        cf_bcc_w(F->t, cond, d);
}

/* ---- site lists ----------------------------------------------------------- */

static void note_ext(struct cf_sites *st, int at, struct func *callee,
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

static void note_str(struct cf_sites *st, int at, int idx)
{
    if (st->nstr == st->capstr) {
        st->capstr = st->capstr ? st->capstr * 2 : 16;
        st->str = xrealloc(st->str, (size_t)st->capstr * sizeof *st->str);
    }
    st->str[st->nstr].patch_off = at;
    st->str[st->nstr].str_off = idx;
    st->str[st->nstr].kind = RK_ABS32;
    st->nstr++;
}

static void note_glob(struct cf_sites *st, int at, struct global *g)
{
    if (st->ng == st->capg) {
        st->capg = st->capg ? st->capg * 2 : 16;
        st->g = xrealloc(st->g, (size_t)st->capg * sizeof *st->g);
    }
    st->g[st->ng].patch_off = at;
    st->g[st->ng].glob = g;
    st->g[st->ng].kind = RK_ABS32;
    st->ng++;
}

static void note_fn(struct cf_sites *st, int at, struct func *target)
{
    if (st->nf == st->capf) {
        st->capf = st->capf ? st->capf * 2 : 16;
        st->f = xrealloc(st->f, (size_t)st->capf * sizeof *st->f);
    }
    st->f[st->nf].patch_off = at;
    st->f[st->nf].target = target;
    st->f[st->nf].kind = RK_ABS32;
    st->f[st->nf].addend = 0;
    st->nf++;
}

/* jsr to a symbol: the absolute address after the operation word. */
static void call_sym(struct cf_fn *F, struct func *callee)
{
    int at = F->t->len;
    cf_jsr(F->t, cf_absl(0));
    note_ext(F->st, at + 2, callee, 0);
}

static struct func **g_helpers;
static int g_nhelpers, g_caphelpers;

static void call_helper(struct cf_fn *F, const char *name)
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

/* ---- helper arguments: words at the stack pointer ----------------------- */

static struct cf_ea out_at(long off)
{
    return cf_disp(CF_SP, off);
}

/* An argument word at the stack pointer must lie inside the argument
 * area the frame reserved (out_area): past it are the callee-saved
 * registers. A lowering that calls a helper its op_calls_helper answer
 * did not admit would write there; this stops the compile instead. */
static void need_out(const struct cf_fn *F, long end)
{
    if (end > F->out_bytes)
        internal_error("coldfire: %s: a call's arguments need %ld bytes and "
                       "the frame reserved %ld", F->fn->name, end,
                       F->out_bytes);
}

static void harg32(struct cf_fn *F, long off, int v)
{
    need_out(F, off + 4);
    mv(F, 4, vea(F, v), out_at(off), D0);
}

static void harg64(struct cf_fn *F, long off, int v)
{
    need_out(F, off + 8);
    mv(F, 4, hi_ea(F, v), out_at(off), D0);
    mv(F, 4, lo_ea(F, v), out_at(off + 4), D0);
}

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
    default:   return w == 8 ? "__gedf2" : "__gesf2";
    }
}

/* ---- comparisons ------------------------------------------------------- */

/* The condition `cmp.l b,a` leaves for a pred b. */
static int pred_cond(enum binop pred, int sign)
{
    switch (pred) {
    case B_EQ: return CF_EQ;
    case B_NE: return CF_NE;
    case B_LT: return sign ? CF_LT : CF_CS;
    case B_LE: return sign ? CF_LE : CF_LS;
    case B_GT: return sign ? CF_GT : CF_HI;
    default:   return sign ? CF_GE : CF_CC;
    }
}

/* 0 or 1 from a condition, into data register r. */
static void cond_to_reg(struct cf_fn *F, int cond, int r)
{
    cf_scc(F->t, cond, r);
    cf_unary(F->t, CF_EXTBL, r);
    cf_unary(F->t, CF_NEG, r);
}

/* The flags for v != 0 at 32 bits: tst, or a move to d1 for an address
 * register (tst takes none on ISA_A). */
static void test32(struct cf_fn *F, int v)
{
    struct cf_ea e = vea(F, v);
    if (e.mode == CFM_A)
        cf_move(F->t, 4, e, cf_dreg(D1));
    else
        cf_tst(F->t, 4, e);
}

static void test64(struct cf_fn *F, int v)
{
    cf_move(F->t, 4, hi_ea(F, v), cf_dreg(D1));
    cf_alu(F->t, CF_OR, lo_ea(F, v), D1);
}

/* The flags for IR_CMP i at 32 bits: cmp.l b,a. */
static void cmp32(struct cf_fn *F, const struct ir_ins *i)
{
    struct cf_ea a = vea(F, i->a);
    if (a.mode == CFM_A) {
        if (i->imm_b)
            cf_alua(F->t, CF_CMP, cf_imm(imm_val(i)), a.reg);
        else
            cf_alua(F->t, CF_CMP, vea(F, i->b), a.reg);
        return;
    }
    if (i->imm_b && imm_val(i) == 0 && a.mode != CFM_IMM) {
        cf_tst(F->t, 4, a);
        return;
    }
    {
        int ra_ = rdr(F, i->a, D0);
        if (i->imm_b)
            cf_alu_imm(F->t, CF_CMP, imm_val(i), ra_);
        else
            cf_alu(F->t, CF_CMP, vea(F, i->b), ra_);
    }
}

/* A 64-bit comparison as branches: to `t` when (a pred b) holds and to
 * `f` when it does not, each an IR label or -1 for "fall through". The
 * high words decide unless they are equal (signed or not with the
 * comparison); then the low words, unsigned. Returns the placeholder of
 * a local branch to patch where the caller lands the fall-through, or -1. */
struct br64 { int at[4]; int n; };

static void cmp64_br(struct cf_fn *F, const struct ir_ins *i, int t, int f,
                     struct br64 *tl, struct br64 *fl)
{
    int sign = i->sign, p = i->pred;
    int c_hi_true, c_hi_false, c_lo;
    tl->n = fl->n = 0;
    cf_move(F->t, 4, hi_ea(F, i->a), cf_dreg(D0));
    if (i->imm_b)
        cf_alu_imm(F->t, CF_CMP, (long)imm_hi(i), D0);
    else
        cf_alu(F->t, CF_CMP, hi_ea(F, i->b), D0);
    switch (p) {
    case B_EQ: c_hi_true = -1; c_hi_false = CF_NE; c_lo = CF_EQ; break;
    case B_NE: c_hi_true = CF_NE; c_hi_false = -1; c_lo = CF_NE; break;
    case B_LT: case B_LE:
        c_hi_true = sign ? CF_LT : CF_CS;
        c_hi_false = sign ? CF_GT : CF_HI;
        c_lo = p == B_LT ? CF_CS : CF_LS;
        break;
    default:     /* GT, GE */
        c_hi_true = sign ? CF_GT : CF_HI;
        c_hi_false = sign ? CF_LT : CF_CS;
        c_lo = p == B_GT ? CF_HI : CF_CC;
        break;
    }
    if (c_hi_true >= 0) {
        if (t >= 0) branch_to(F, c_hi_true, t);
        else        tl->at[tl->n++] = br_place(F, c_hi_true);
    }
    if (c_hi_false >= 0) {
        if (f >= 0) branch_to(F, c_hi_false, f);
        else        fl->at[fl->n++] = br_place(F, c_hi_false);
    }
    cf_move(F->t, 4, lo_ea(F, i->a), cf_dreg(D0));
    if (i->imm_b)
        cf_alu_imm(F->t, CF_CMP, (long)imm_lo(i), D0);
    else
        cf_alu(F->t, CF_CMP, lo_ea(F, i->b), D0);
    if (t >= 0) {
        branch_to(F, c_lo, t);
        if (f >= 0)
            branch_to(F, CF_T, f);
    } else {
        tl->at[tl->n++] = br_place(F, c_lo);
        if (f >= 0)
            branch_to(F, CF_T, f);
    }
}

/* ---- ALU into a data register -------------------------------------------- */

/* R op= src (32 bits), src an operand: a register, a slot or #imm. */
static void op_into(struct cf_fn *F, enum ir_op op, struct cf_ea src, int R,
                    int scr)
{
    struct code *t = F->t;
    if (src.mode == CFM_IMM) {
        long v = (long)(int)(src.imm & 0xffffffffL);
        switch (op) {
        case IR_ADD: case IR_SUB: {
            int sub = op == IR_SUB;
            if (v == 0)
                return;
            if (v < 0 && v >= -8) {
                v = -v;
                sub = !sub;
            }
            if (v >= 1 && v <= 8)
                cf_addq(t, sub, (int)v, cf_dreg(R));
            else
                cf_alu_imm(t, sub ? CF_SUB : CF_ADD, v, R);
            return;
        }
        case IR_AND:
            if (v == -1)
                return;
            if (v == 0) { cf_moveq(t, 0, R); return; }
            cf_alu_imm(t, CF_AND, v, R);
            return;
        case IR_OR:
            if (v == 0)
                return;
            cf_alu_imm(t, CF_OR, v, R);
            return;
        case IR_XOR:
            if (v == 0)
                return;
            if (v == -1) { cf_unary(t, CF_NOT, R); return; }
            cf_alu_imm(t, CF_EOR, v, R);
            return;
        case IR_MUL:
            ldi(F, v, cf_dreg(scr), scr);
            cf_mul(t, 1, 4, cf_dreg(scr), R);
            return;
        default:
            internal_error("coldfire: op_into %d with an immediate", op);
        }
    }
    switch (op) {
    case IR_ADD: cf_alu(t, CF_ADD, src, R); return;
    case IR_SUB: cf_alu(t, CF_SUB, src, R); return;
    case IR_AND: case IR_OR:
        if (src.mode == CFM_A) {
            cf_move(t, 4, src, cf_dreg(scr));
            src = cf_dreg(scr);
        }
        cf_alu(t, op == IR_AND ? CF_AND : CF_OR, src, R);
        return;
    case IR_XOR:
        if (src.mode != CFM_D) {
            cf_move(t, 4, src, cf_dreg(scr));
            src = cf_dreg(scr);
        }
        cf_alu_mem(t, CF_EOR, src.reg, cf_dreg(R));
        return;
    case IR_MUL:
        if (src.mode == CFM_A) {
            cf_move(t, 4, src, cf_dreg(scr));
            src = cf_dreg(scr);
        }
        cf_mul(t, 1, 4, src, R);
        return;
    default:
        internal_error("coldfire: op_into %d", op);
    }
}

/* Sign- or zero-extend the low `size` bytes of data register r. */
static void ext_reg(struct cf_fn *F, int r, int size, int sign)
{
    if (size >= 4)
        return;
    if (sign)
        cf_unary(F->t, size == 1 ? CF_EXTBL : CF_EXTL, r);
    else
        cf_alu_imm(F->t, CF_AND, size == 1 ? 0xff : 0xffff, r);
}

/* R = the `size`-byte value at `src`, extended. */
static void ld_ext(struct cf_fn *F, int R, struct cf_ea src, int size,
                   int sign)
{
    if (size >= 4) {
        cf_move(F->t, 4, src, cf_dreg(R));
        return;
    }
    if (!sign) {
        cf_moveq(F->t, 0, R);
        cf_move(F->t, size, src, cf_dreg(R));
        return;
    }
    cf_move(F->t, size, src, cf_dreg(R));
    cf_unary(F->t, size == 1 ? CF_EXTBL : CF_EXTL, R);
}

/* ---- the soft-float and 64-bit helpers' results --------------------------- */

static void res32_to64(struct cf_fn *F, int dst, int sign)
{
    /* a 32-bit result in d0 into a 64-bit value */
    cf_move(F->t, 4, cf_dreg(D0), cf_dreg(D1));
    if (sign) {
        cf_scc(F->t, CF_MI, D0);
        cf_unary(F->t, CF_EXTBL, D0);
    } else {
        cf_moveq(F->t, 0, D0);
    }
    wr64(F, dst, D0, D1);
}

/* ---- 64-bit operations, in frame slots ------------------------------------ */

static int gen_ins64(struct cf_fn *F, int n)
{
    struct ir_func *fn = F->fn;
    struct ir_ins *i = &fn->ins[n];
    struct code *t = F->t;

    switch (i->op) {
    case IR_CONST:
        ldi(F, (long)((unsigned long)i->imm >> 32), hi_ea(F, i->dst), D0);
        ldi(F, (long)i->imm, lo_ea(F, i->dst), D0);
        return 1;
    case IR_BITCAST:
    case IR_MOV:
        mv(F, 4, hi_ea(F, i->a), hi_ea(F, i->dst), D0);
        mv(F, 4, lo_ea(F, i->a), lo_ea(F, i->dst), D0);
        return 1;
    case IR_ADD: case IR_SUB: {
        int sub = i->op == IR_SUB;
        cf_move(t, 4, hi_ea(F, i->a), cf_dreg(D0));
        if (i->imm_b) {
            if (imm_hi(i))
                cf_alu_imm(t, sub ? CF_SUB : CF_ADD, (long)imm_hi(i), D0);
        } else {
            cf_alu(t, sub ? CF_SUB : CF_ADD, hi_ea(F, i->b), D0);
        }
        cf_move(t, 4, lo_ea(F, i->a), cf_dreg(D1));
        if (i->imm_b)
            cf_alu_imm(t, sub ? CF_SUB : CF_ADD, (long)imm_lo(i), D1);
        else
            cf_alu(t, sub ? CF_SUB : CF_ADD, lo_ea(F, i->b), D1);
        /* the low word's carry (borrow) into the high one */
        cf_bcc_b(t, CF_CC, 2);
        cf_addq(t, sub, 1, cf_dreg(D0));
        wr64(F, i->dst, D0, D1);
        return 1;
    }
    case IR_AND: case IR_OR: {
        enum ir_op op = i->op;
        cf_move(t, 4, hi_ea(F, i->a), cf_dreg(D0));
        cf_move(t, 4, lo_ea(F, i->a), cf_dreg(D1));
        if (i->imm_b) {
            op_into(F, op, cf_imm((long)imm_hi(i)), D0, D0);
            op_into(F, op, cf_imm((long)imm_lo(i)), D1, D1);
        } else {
            op_into(F, op, hi_ea(F, i->b), D0, D0);
            op_into(F, op, lo_ea(F, i->b), D1, D1);
        }
        wr64(F, i->dst, D0, D1);
        return 1;
    }
    case IR_XOR:
        if (i->imm_b) {
            cf_move(t, 4, hi_ea(F, i->a), cf_dreg(D0));
            cf_move(t, 4, lo_ea(F, i->a), cf_dreg(D1));
            op_into(F, IR_XOR, cf_imm((long)imm_hi(i)), D0, D0);
            op_into(F, IR_XOR, cf_imm((long)imm_lo(i)), D1, D1);
            wr64(F, i->dst, D0, D1);
            return 1;
        }
        /* eor's source is a data register: the high words through d0
         * and d1, stored before the low words take them (dst may share a
         * slot with a or b, whose high words are read by then) */
        cf_move(t, 4, hi_ea(F, i->a), cf_dreg(D0));
        cf_move(t, 4, hi_ea(F, i->b), cf_dreg(D1));
        cf_alu_mem(t, CF_EOR, D1, cf_dreg(D0));
        cf_move(t, 4, lo_ea(F, i->a), cf_dreg(D1));
        if (F->slot[i->dst] != NOSLOT)
            cf_move(t, 4, cf_dreg(D0), hi_ea(F, i->dst));
        cf_move(t, 4, lo_ea(F, i->b), cf_dreg(D0));
        cf_alu_mem(t, CF_EOR, D0, cf_dreg(D1));
        if (F->slot[i->dst] != NOSLOT)
            cf_move(t, 4, cf_dreg(D1), lo_ea(F, i->dst));
        return 1;
    case IR_NEG:
        rd64(F, i->a, D0, D1);
        cf_unary(t, CF_NEG, D1);
        cf_unary(t, CF_NEGX, D0);
        wr64(F, i->dst, D0, D1);
        return 1;
    case IR_BNOT:
        rd64(F, i->a, D0, D1);
        cf_unary(t, CF_NOT, D0);
        cf_unary(t, CF_NOT, D1);
        wr64(F, i->dst, D0, D1);
        return 1;
    case IR_MUL: case IR_DIV: case IR_MOD:
        harg64(F, 0, i->a);
        if (i->imm_b) {
            need_out(F, 16);
            ldi(F, (long)imm_hi(i), out_at(8), D0);
            ldi(F, (long)imm_lo(i), out_at(12), D0);
        } else {
            harg64(F, 8, i->b);
        }
        call_helper(F, i->op == IR_MUL ? "__muldi3"
                     : i->op == IR_DIV ? (i->sign ? "__divdi3" : "__udivdi3")
                     : (i->sign ? "__moddi3" : "__umoddi3"));
        wr64(F, i->dst, D0, D1);
        return 1;
    case IR_SHL: case IR_SHR: {
        int left = i->op == IR_SHL, sign = !left && i->sign;
        if (i->imm_b) {
            long k = i->imm & 63;
            rd64(F, i->a, D0, D1);
            if (k == 0) {
                /* nothing */
            } else if (k >= 32) {
                /* one word moves to the other and shifts on */
                k -= 32;
                if (left) {
                    cf_move(t, 4, cf_dreg(D1), cf_dreg(D0));
                    cf_moveq(t, 0, D1);
                } else {
                    cf_move(t, 4, cf_dreg(D0), cf_dreg(D1));
                    if (sign) {
                        cf_tst(t, 4, cf_dreg(D0));
                        cf_scc(t, CF_MI, D0);
                        cf_unary(t, CF_EXTBL, D0);
                    } else {
                        cf_moveq(t, 0, D0);
                    }
                }
                while (k > 0) {
                    int s = k > 8 ? 8 : (int)k;
                    cf_shift_imm(t, left ? CF_LSL : sign ? CF_ASR : CF_LSR,
                                 s, left ? D0 : D1);
                    k -= s;
                }
            } else {
                /* one bit at a time through the X flag is too slow for
                 * a large count; the helper takes the rest */
                if (k == 1 && left) {
                    cf_alu(t, CF_ADD, cf_dreg(D1), D1);
                    cf_addx(t, 0, D0, D0);
                } else {
                    need_out(F, 12);
                    cf_move(t, 4, cf_dreg(D0), out_at(0));
                    cf_move(t, 4, cf_dreg(D1), out_at(4));
                    ldi(F, k, out_at(8), D0);
                    call_helper(F, left ? "__ashldi3"
                                 : sign ? "__ashrdi3" : "__lshrdi3");
                }
            }
            wr64(F, i->dst, D0, D1);
            return 1;
        }
        harg64(F, 0, i->a);
        harg32(F, 8, i->b);
        call_helper(F, left ? "__ashldi3" : sign ? "__ashrdi3" : "__lshrdi3");
        wr64(F, i->dst, D0, D1);
        return 1;
    }
    case IR_EXT:
        cf_move(t, 4, vea(F, i->a), cf_dreg(D1));
        ext_reg(F, D1, i->size, i->sign);
        if (i->sign) {
            cf_tst(t, 4, cf_dreg(D1));
            cf_scc(t, CF_MI, D0);
            cf_unary(t, CF_EXTBL, D0);
        } else {
            cf_moveq(t, 0, D0);
        }
        wr64(F, i->dst, D0, D1);
        return 1;
    case IR_LDVAR:
        if (i->size == 8) {
            mv(F, 4, fp_at(var_slot(F, i->a, 8)), hi_ea(F, i->dst), D0);
            mv(F, 4, fp_at(var_slot(F, i->a, 8) + 4), lo_ea(F, i->dst), D0);
            return 1;
        }
        if (in_reg(F, i->a)) {
            cf_move(t, 4, vea(F, i->a), cf_dreg(D1));
            ext_reg(F, D1, i->size, i->sign);
        } else {
            ld_ext(F, D1, fp_at(var_slot(F, i->a, i->size)), i->size,
                   i->sign);
        }
        if (i->sign) {
            cf_tst(t, 4, cf_dreg(D1));
            cf_scc(t, CF_MI, D0);
            cf_unary(t, CF_EXTBL, D0);
        } else {
            cf_moveq(t, 0, D0);
        }
        wr64(F, i->dst, D0, D1);
        return 1;
    case IR_STVAR:
        if (i->size == 8) {
            mv(F, 4, hi_ea(F, i->a), fp_at(var_slot(F, i->dst, 8)), D0);
            mv(F, 4, lo_ea(F, i->a), fp_at(var_slot(F, i->dst, 8) + 4), D0);
        } else if (in_reg(F, i->dst)) {
            int R = F->loc[i->dst];
            if (CF_IS_A(R)) {
                cf_move(t, 4, lo_ea(F, i->a), cf_areg(R));
            } else {
                cf_move(t, 4, lo_ea(F, i->a), cf_dreg(R));
                ext_reg(F, R, i->size, 1);
            }
        } else if (F->slot[i->dst] != NOSLOT) {
            mv(F, i->size, fp_at(sslot(F, i->a) + 8 - i->size),
               fp_at(var_slot(F, i->dst, i->size)), D0);
        }
        return 1;
    case IR_LOAD: {
        int base = areg(F, i->a, A0);
        if (i->size == 8) {
            cf_move(t, 4, cf_disp(base, i->memoff), cf_dreg(D0));
            cf_move(t, 4, cf_disp(base, i->memoff + 4), cf_dreg(D1));
        } else {
            ld_ext(F, D1, cf_disp(base, i->memoff), i->size, i->sign);
            if (i->sign) {
                cf_tst(t, 4, cf_dreg(D1));
                cf_scc(t, CF_MI, D0);
                cf_unary(t, CF_EXTBL, D0);
            } else {
                cf_moveq(t, 0, D0);
            }
        }
        wr64(F, i->dst, D0, D1);
        return 1;
    }
    case IR_STORE: {
        int base = areg(F, i->a, A0);
        if (i->size == 8) {
            mv(F, 4, hi_ea(F, i->b), cf_disp(base, i->memoff), D0);
            mv(F, 4, lo_ea(F, i->b), cf_disp(base, i->memoff + 4), D0);
        } else {
            mv(F, i->size, fp_at(sslot(F, i->b) + 8 - i->size),
               cf_disp(base, i->memoff), D0);
        }
        return 1;
    }
    case IR_SELECT: {
        int skip;
        if (i->size == 8) test64(F, i->a);
        else              test32(F, i->a);
        skip = br_place(F, CF_EQ);
        cf_move(t, 4, hi_ea(F, i->b), cf_dreg(D0));
        cf_move(t, 4, lo_ea(F, i->b), cf_dreg(D1));
        {
            int done = br_place(F, CF_T);
            br_land(F, skip);
            cf_move(t, 4, hi_ea(F, i->c), cf_dreg(D0));
            cf_move(t, 4, lo_ea(F, i->c), cf_dreg(D1));
            br_land(F, done);
        }
        wr64(F, i->dst, D0, D1);
        return 1;
    }
    default:
        return 0;
    }
}

/* ---- one call ------------------------------------------------------------ */

static void gen_call(struct cf_fn *F, int n)
{
    struct ir_func *fn = F->fn;
    struct ir_ins *i = &fn->ins[n];
    struct code *t = F->t;
    long off = 0;

    /* each argument into its words at the stack pointer (the C++
     * return slot, sret_first, goes in a1 below) */
    for (int k = i->sret_first ? 1 : 0; k < i->nargs; k++) {
        struct ir_arg *a = &i->argv[k];
        need_out(F, off + arg_words(a));
        if (a->is_struct) {
            cf_move(t, 4, vea(F, a->vreg), cf_areg(A1));
            cf_lea(t, out_at(off + pad_of(a)), A0);
            copy_block(F, 1, a->size);
        } else if (a->size > 4) {
            if (is_wide(F, a->vreg)) {
                harg64(F, off, a->vreg);
            } else {
                /* a 32-bit value where 64 bits are wanted: never made by
                 * irgen, which extends first */
                internal_error("coldfire: %s: a 32-bit argument in 64-bit "
                               "words", fn->name);
            }
        } else {
            harg32(F, off, a->vreg);
        }
        off += arg_words(a);
    }
    if (call_sret(i))
        cf_lea(t, fp_at(F->scratch_at + i->scratch), A1);
    else if (i->sret_first && i->nargs > 0)
        cf_move(t, 4, vea(F, i->argv[0].vreg), cf_areg(A1));
    if (i->indirect) {
        int r = areg(F, i->a, A0);
        cf_jsr(t, cf_ind(r));
    } else {
        call_sym(F, i->callee);
    }

    if (i->dst < 0)
        return;
    if (i->retsize) {
        long at = F->scratch_at + i->scratch;
        if (!call_sret(i))      /* a _Complex, in d0 on */
            for (int r = 0; r < cplx_regs(i->rety, i->retsize); r++)
                cf_move(t, 4, cf_dreg(r), fp_at(at + 4L * r));
        cf_lea(t, fp_at(at), A0);
        wrote(F, i->dst, A0);
    } else if (is_wide(F, i->dst)) {
        if (i->ret_tybytes > 4 || i->ret_tybytes == 0)
            wr64(F, i->dst, D0, D1);
        else
            res32_to64(F, i->dst, i->ret_tysign);
    } else {
        /* a narrow result is extended here: the callee need not have */
        if (i->ret_tybytes == 1 || i->ret_tybytes == 2)
            ext_reg(F, D0, i->ret_tybytes, i->ret_tysign);
        wrote(F, i->dst, D0);
    }
}

/* ---- one instruction ----------------------------------------------------- */

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

/* Interrupts off for an atomic read-modify-write: sr saved in the tmp
 * slot, the mask raised to 7. ISA_A has no compare-and-swap, and on a
 * single core this is what makes the sequence atomic -- in supervisor
 * mode, where a bare-metal ColdFire program runs; in user mode the move
 * to sr traps rather than run unprotected. */
static void irq_off(struct cf_fn *F)
{
    cf_move_from_sr(F->t, D0);
    cf_move(F->t, 2, cf_dreg(D0), fp_at(F->tmp_slot));
    cf_alu_imm(F->t, CF_OR, 0x0700, D0);
    cf_move_to_sr(F->t, D0);
}

static void irq_restore(struct cf_fn *F, int scr)
{
    cf_move(F->t, 2, fp_at(F->tmp_slot), cf_dreg(scr));
    cf_move_to_sr(F->t, scr);
}

static void need_atomic_size(struct cf_fn *F, const struct ir_ins *i)
{
    if (i->size != 1 && i->size != 2 && i->size != 4)
        cf_refuse(F, i, "an atomic wider than four bytes");
}

static void gen_ins(struct cf_fn *F, int n)
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

    /* ---- soft float ---- */
    if (i->flt && (i->op == IR_ADD || i->op == IR_SUB || i->op == IR_MUL ||
                   i->op == IR_DIV || i->op == IR_MOD || i->op == IR_NEG ||
                   i->op == IR_CMP || i->op == IR_SQRT)) {
        const char *name = fp_binop_name(i->op, i->w);
        if (i->w != 4 && i->w != 8)
            cf_refuse(F, i, "a floating-point value of this width");
        if (i->imm_b)
            cf_refuse(F, i, "a folded floating-point immediate");
        if (name || i->op == IR_CMP) {
            if (i->w == 8) {
                harg64(F, 0, i->a);
                harg64(F, 8, i->b);
            } else {
                harg32(F, 0, i->a);
                harg32(F, 4, i->b);
            }
        }
        if (name) {
            call_helper(F, name);
            if (i->w == 8) wr64(F, i->dst, D0, D1);
            else           wrote(F, i->dst, D0);
            return;
        }
        if (i->op == IR_NEG) {
            /* the sign bit flipped: right for -0.0 and a NaN as well */
            if (i->w == 8) {
                rd64(F, i->a, D0, D1);
                cf_alu_imm(t, CF_EOR, (long)0x80000000UL, D0);
                wr64(F, i->dst, D0, D1);
            } else {
                int R = wreg(F, i->dst, D0);
                rd(F, i->a, R);
                cf_alu_imm(t, CF_EOR, (long)0x80000000UL, R);
                wrote(F, i->dst, R);
            }
            return;
        }
        if (i->op == IR_CMP) {
            /* the helper's int relates to zero as the operands do;
             * unordered makes the predicate false */
            struct ir_ins *nx = n + 1 < fn->nins ? &fn->ins[n + 1] : NULL;
            int cond = pred_cond(i->pred, 1);
            call_helper(F, fp_cmp_name(i->pred, i->w));
            cf_tst(t, 4, cf_dreg(D0));
            if (nx && (nx->op == IR_BRZ || nx->op == IR_BRNZ) &&
                nx->a == i->dst && F->usecnt && F->usecnt[i->dst] == 1) {
                branch_to(F, nx->op == IR_BRZ ? cf_cond_invert(cond) : cond,
                          nx->label);
                F->skip_next = 1;
                return;
            }
            {
                int R = wreg(F, i->dst, D0);
                cond_to_reg(F, cond, R);
                wrote(F, i->dst, R);
            }
            return;
        }
        if (i->op == IR_SQRT)
            cf_refuse(F, i, "__builtin_sqrt (a libm routine here, not an "
                            "instruction)");
        cf_refuse(F, i, "this floating-point operation");
    }

    if (i->w > 8)
        cf_refuse(F, i, "a 128-bit value");

    /* The high word of a 64-bit value, shifted: one register. */
    if (i->op == IR_SHR && F->nshr && i->dst >= 0 && F->nshr[i->dst]) {
        int k = (int)i->imm - 32, R = wreg(F, i->dst, D0);
        cf_move(t, 4, hi_ea(F, i->a), cf_dreg(R));
        while (k > 0) {
            int s = k > 8 ? 8 : k;
            cf_shift_imm(t, i->sign ? CF_ASR : CF_LSR, s, R);
            k -= s;
        }
        wrote(F, i->dst, R);
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
        case IR_CMP: case IR_BRZ: case IR_BRNZ:
            break;
        default:
            /* an address (IR_ADDR, IR_FRAMEADDR, ...) is w 8 whatever the
             * pointer's width: what is 64 bits is what wide_map says is */
            wide = wide && (i->dst < 0 || F->wide[i->dst]);
            break;
        }
        if (wide && i->op != IR_CMP && i->op != IR_BRZ && i->op != IR_BRNZ &&
            i->op != IR_CALL && i->op != IR_RET &&
            i->op != IR_I2F && i->op != IR_F2I && i->op != IR_F2F &&
            i->op != IR_BSWAP) {
            if (gen_ins64(F, n))
                return;
            cf_refuse(F, i, "this operation at 64 bits");
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
        branch_to(F, CF_T, i->label);
        return;
    case IR_CONST:
        if (in_reg(F, i->dst))
            ldi(F, imm_val(i), vea(F, i->dst), D0);
        else if (i->dst >= 0 && F->slot[i->dst] != NOSLOT)
            ldi(F, imm_val(i), vea(F, i->dst), D0);
        return;
    case IR_BITCAST:
    case IR_MOV:
        if (i->dst < 0 || (!in_reg(F, i->dst) && F->slot[i->dst] == NOSLOT))
            return;
        mv(F, 4, vea(F, i->a), vea(F, i->dst), D0);
        return;

    case IR_ADD: case IR_SUB: case IR_MUL:
    case IR_AND: case IR_OR:  case IR_XOR: {
        int R = wreg(F, i->dst, D0);
        int a = i->a, b = i->b;
        int comm = i->op != IR_SUB;
        if (!i->imm_b && dreg_of(F, b) == R && dreg_of(F, a) != R) {
            if (comm) {
                int x = a; a = b; b = x;
            } else {
                /* R = a - R */
                cf_unary(t, CF_NEG, R);
                cf_alu(t, CF_ADD, vea(F, a), R);
                wrote(F, i->dst, R);
                return;
            }
        }
        /* an address register can add in place: adda/suba/lea */
        if (in_reg(F, i->dst) && CF_IS_A(F->loc[i->dst]) &&
            (i->op == IR_ADD || i->op == IR_SUB)) {
            int Ra = F->loc[i->dst];
            struct cf_ea ea_a = vea(F, i->a);
            if (i->imm_b) {
                long v = imm_val(i);
                if (i->op == IR_SUB) v = -v;
                if (ea_a.mode == CFM_A && v >= -32768 && v <= 32767) {
                    if (ea_a.reg != Ra || v)
                        cf_lea(t, cf_disp(ea_a.reg, v), Ra);
                    return;
                }
                rd(F, i->a, Ra);
                if (v >= 1 && v <= 8)
                    cf_addq(t, 0, (int)v, cf_areg(Ra));
                else if (v <= -1 && v >= -8)
                    cf_addq(t, 1, (int)-v, cf_areg(Ra));
                else if (v)
                    cf_alua(t, CF_ADD, cf_imm(v), Ra);
                return;
            }
            if (!(in_reg(F, i->b) && F->loc[i->b] == Ra)) {
                rd(F, i->a, Ra);
                cf_alua(t, i->op == IR_ADD ? CF_ADD : CF_SUB, vea(F, i->b),
                        Ra);
                return;
            }
            if (i->op == IR_ADD) {
                cf_alua(t, CF_ADD, vea(F, i->a), Ra);
                return;
            }
            /* Ra = a - Ra, through d0 */
            cf_move(t, 4, vea(F, i->a), cf_dreg(D0));
            cf_alu(t, CF_SUB, cf_areg(Ra), D0);
            cf_move(t, 4, cf_dreg(D0), cf_areg(Ra));
            return;
        }
        if (dreg_of(F, a) != R)
            cf_move(t, 4, vea(F, a), cf_dreg(R));
        op_into(F, i->op, i->imm_b ? cf_imm(imm_val(i)) : vea(F, b), R, D1);
        wrote(F, i->dst, R);
        return;
    }
    case IR_DIV: case IR_MOD: {
        /* divs.l/divu.l <ea>,Dq and rems.l/remu.l <ea>,Dr:Dq take the
         * divisor from a data register or a slot, never an immediate */
        struct cf_ea div;
        int R;
        if (i->imm_b) {
            ldi(F, imm_val(i), fp_at(F->tmp_slot), D1);
            div = fp_at(F->tmp_slot);
        } else {
            div = vea(F, i->b);
            if (div.mode == CFM_A && i->op == IR_MOD) {
                cf_move(t, 4, div, fp_at(F->tmp_slot));
                div = fp_at(F->tmp_slot);
            } else if (div.mode == CFM_A) {
                cf_move(t, 4, div, cf_dreg(D1));
                div = cf_dreg(D1);
            }
        }
        if (i->op == IR_DIV) {
            /* the quotient over the dividend: in R unless R is the
             * divisor's register, then in d0 */
            int Q;
            R = wreg(F, i->dst, D0);
            Q = ea_is_dreg(&div, R) ? D0 : R;
            if (dreg_of(F, i->a) != Q)
                cf_move(t, 4, vea(F, i->a), cf_dreg(Q));
            cf_div(t, i->sign, div, Q);
            if (Q != R)
                cf_move(t, 4, cf_dreg(Q), cf_dreg(R));
            wrote(F, i->dst, R);
        } else {
            /* the dividend in d0, the remainder in R unless R is the
             * divisor's register (or there is none), then in d1 -- which
             * the divisor is only for an immediate, in the tmp slot */
            int Rr;
            R = wreg(F, i->dst, D1);
            Rr = ea_is_dreg(&div, R) ? D1 : R;
            if (ea_is_dreg(&div, Rr))
                internal_error("coldfire: %s: the remainder's register is "
                               "the divisor's", fn->name);
            cf_move(t, 4, vea(F, i->a), cf_dreg(D0));
            cf_rem(t, i->sign, div, Rr, D0);
            if (Rr != R)
                cf_move(t, 4, cf_dreg(Rr), cf_dreg(R));
            wrote(F, i->dst, R);
        }
        return;
    }
    case IR_SHL: case IR_SHR: {
        enum cf_sh op = i->op == IR_SHL ? CF_LSL : i->sign ? CF_ASR : CF_LSR;
        int R = wreg(F, i->dst, D0);
        if (i->imm_b) {
            int k = (int)(i->imm & 31);
            if (dreg_of(F, i->a) != R)
                cf_move(t, 4, vea(F, i->a), cf_dreg(R));
            if (k > 16) {
                cf_moveq(t, k, D1);
                cf_shift_reg(t, op, D1, R);
            } else {
                while (k > 0) {
                    int s = k > 8 ? 8 : k;
                    cf_shift_imm(t, op, s, R);
                    k -= s;
                }
            }
        } else {
            int c = dreg_of(F, i->b);
            if (c < 0 || c == R) {
                cf_move(t, 4, vea(F, i->b), cf_dreg(D1));
                c = D1;
            }
            if (dreg_of(F, i->a) != R)
                cf_move(t, 4, vea(F, i->a), cf_dreg(R));
            cf_shift_reg(t, op, c, R);
        }
        wrote(F, i->dst, R);
        return;
    }
    case IR_NEG: case IR_BNOT: {
        int R = wreg(F, i->dst, D0);
        if (dreg_of(F, i->a) != R)
            cf_move(t, 4, vea(F, i->a), cf_dreg(R));
        cf_unary(t, i->op == IR_NEG ? CF_NEG : CF_NOT, R);
        wrote(F, i->dst, R);
        return;
    }

    case IR_CMP: {
        struct ir_ins *nx = n + 1 < fn->nins ? &fn->ins[n + 1]
                                             : (struct ir_ins *)0;
        int fuse = nx && (nx->op == IR_BRZ || nx->op == IR_BRNZ) &&
                   nx->a == i->dst && F->usecnt && F->usecnt[i->dst] == 1;
        if (i->w == 8) {
            struct br64 tl, fl;
            if (fuse) {
                if (nx->op == IR_BRNZ) cmp64_br(F, i, nx->label, -1, &tl, &fl);
                else                   cmp64_br(F, i, -1, nx->label, &tl, &fl);
                for (int k = 0; k < tl.n; k++) br_land(F, tl.at[k]);
                for (int k = 0; k < fl.n; k++) br_land(F, fl.at[k]);
                F->skip_next = 1;
                return;
            }
            {
                int R = wreg(F, i->dst, D1), done;
                cmp64_br(F, i, -1, -1, &tl, &fl);
                for (int k = 0; k < fl.n; k++) br_land(F, fl.at[k]);
                cf_moveq(t, 0, R);
                done = br_place(F, CF_T);
                for (int k = 0; k < tl.n; k++) br_land(F, tl.at[k]);
                cf_moveq(t, 1, R);
                br_land(F, done);
                wrote(F, i->dst, R);
            }
            return;
        }
        cmp32(F, i);
        {
            int cond = pred_cond(i->pred, i->sign);
            if (fuse) {
                branch_to(F, nx->op == IR_BRZ ? cf_cond_invert(cond) : cond,
                          nx->label);
                F->skip_next = 1;
                return;
            }
            {
                int R = wreg(F, i->dst, D0);
                cond_to_reg(F, cond, R);
                wrote(F, i->dst, R);
            }
        }
        return;
    }

    case IR_SELECT: {
        /* dst = a ? b : c, tested at the condition's width, `size` */
        int R = wreg(F, i->dst, D0), skip;
        int ra_ = in_reg(F, i->a) ? F->loc[i->a] : -1;
        int rb_ = in_reg(F, i->b) ? F->loc[i->b] : -1;
        if (R == rb_) {
            if (i->size == 8) test64(F, i->a);
            else              test32(F, i->a);
            skip = br_place(F, CF_NE);
            cf_move(t, 4, vea(F, i->c), cf_dreg(R));
            br_land(F, skip);
        } else {
            int W = R == ra_ ? D0 : R;
            if (!(in_reg(F, i->c) && F->loc[i->c] == W))
                cf_move(t, 4, vea(F, i->c), cf_dreg(W));
            if (i->size == 8) test64(F, i->a);
            else              test32(F, i->a);
            skip = br_place(F, CF_EQ);
            cf_move(t, 4, vea(F, i->b), cf_dreg(W));
            br_land(F, skip);
            if (W != R)
                cf_move(t, 4, cf_dreg(W), cf_dreg(R));
        }
        wrote(F, i->dst, R);
        return;
    }

    case IR_BRZ: case IR_BRNZ:
        if (i->w == 8) test64(F, i->a);
        else           test32(F, i->a);
        branch_to(F, i->op == IR_BRZ ? CF_EQ : CF_NE, i->label);
        return;

    case IR_LDVAR: {
        int R;
        if (in_reg(F, i->dst) && CF_IS_A(F->loc[i->dst]) &&
            cf_ldvar_plain(i->size, i->sign, i->w)) {
            cf_move(t, 4, in_reg(F, i->a) ? vea(F, i->a)
                                          : fp_at(var_slot(F, i->a, 4)),
                    cf_areg(F->loc[i->dst]));
            return;
        }
        R = wreg(F, i->dst, D0);
        if (in_reg(F, i->a)) {
            if (!(F->loc[i->a] == R))
                cf_move(t, 4, vea(F, i->a), cf_dreg(R));
            if (!cf_ldvar_plain(i->size, i->sign, i->w))
                ext_reg(F, R, i->size, i->sign);
        } else {
            ld_ext(F, R, fp_at(var_slot(F, i->a, i->size)), i->size, i->sign);
        }
        wrote(F, i->dst, R);
        return;
    }
    case IR_STVAR:
        if (in_reg(F, i->dst)) {
            int R = F->loc[i->dst];
            struct cf_ea src = vea(F, i->a);
            if (!ea_is_reg(&src, R))
                cf_move(t, 4, src, reg_ea(R));
            if (i->size < 4 && !CF_IS_A(R))
                ext_reg(F, R, i->size, 1);
        } else if (F->slot[i->dst] != NOSLOT) {
            mv(F, i->size, vea_n(F, i->a, i->size),
               fp_at(var_slot(F, i->dst, i->size)), D0);
        }
        return;
    case IR_LOAD: {
        int base = areg(F, i->a, A0);
        struct cf_ea src = cf_disp(base, i->memoff);
        if (in_reg(F, i->dst) && CF_IS_A(F->loc[i->dst]) && i->size == 4) {
            cf_move(t, 4, src, cf_areg(F->loc[i->dst]));
            return;
        }
        {
            int R = wreg(F, i->dst, D0);
            ld_ext(F, R, src, i->size, i->sign);
            wrote(F, i->dst, R);
        }
        return;
    }
    case IR_STORE: {
        int base = areg(F, i->a, A0);
        mv(F, i->size, vea_n(F, i->b, i->size), cf_disp(base, i->memoff), D0);
        return;
    }
    case IR_EXT: {
        int R = wreg(F, i->dst, D0);
        if (dreg_of(F, i->a) != R)
            cf_move(t, 4, vea(F, i->a), cf_dreg(R));
        ext_reg(F, R, i->size, i->sign);
        wrote(F, i->dst, R);
        return;
    }

    case IR_ADDR:
        if (in_reg(F, i->dst) && CF_IS_A(F->loc[i->dst])) {
            cf_lea(t, fp_at(obj_slot(F, i->a)), F->loc[i->dst]);
            return;
        }
        cf_lea(t, fp_at(obj_slot(F, i->a)), A0);
        wrote(F, i->dst, A0);
        return;
    /* An address is a 32-bit absolute operand, R_68K_32 at the extension
     * words: move.l #sym into a data register, lea sym into an address
     * register. */
    case IR_STRADDR: case IR_GADDR: case IR_FADDR: {
        int R = in_reg(F, i->dst) ? F->loc[i->dst] : A0, at = t->len;
        if (CF_IS_A(R))
            cf_lea(t, cf_absl(0), R);
        else
            cf_move(t, 4, cf_imm(0), cf_dreg(R));
        if (i->op == IR_STRADDR)     note_str(F->st, at + 2, i->label);
        else if (i->op == IR_GADDR)  note_glob(F->st, at + 2, i->glob);
        else                         note_fn(F->st, at + 2, i->callee);
        wrote(F, i->dst, R);
        return;
    }

    case IR_MEMCPY: case IR_MEMZERO:
        cf_move(t, 4, vea(F, i->a), cf_areg(A0));
        if (i->op == IR_MEMCPY)
            cf_move(t, 4, vea(F, i->b), cf_areg(A1));
        copy_block(F, i->op == IR_MEMCPY, i->size);
        return;

    case IR_CALL:
        gen_call(F, n);
        return;

    case IR_RET:
        if (i->a >= 0) {
            if (fn->ret_abi.is_struct) {
                if (!fn_sret(fn)) {
                    /* a _Complex: its words in d0 on (the pool keeps
                     * d2 and d3 free for a double's: uses_d2d3_result) */
                    int r = areg(F, i->a, A0);
                    for (int k = 0; k < cplx_regs(fn->ret_abi.ty,
                                                   fn->ret_abi.size); k++)
                        cf_move(t, 4, cf_disp(r, 4L * k), cf_dreg(k));
                } else {
                    /* into the caller's buffer, whose address comes back
                     * in d0 and a0 */
                    cf_move(t, 4, vea(F, i->a), cf_areg(A1));
                    cf_move(t, 4, fp_at(F->sret_slot), cf_areg(A0));
                    copy_block(F, 1, fn->ret_abi.size);
                    cf_move(t, 4, fp_at(F->sret_slot), cf_dreg(D0));
                }
            } else if (is_wide(F, i->a) && fn->ret_abi.size <= 4) {
                cf_move(t, 4, lo_ea(F, i->a), cf_dreg(D0));
            } else if (is_wide(F, i->a)) {
                rd64(F, i->a, D0, D1);
            } else {
                rd(F, i->a, D0);    /* and a0 for a pointer: the epilogue */
            }
        }
        {
            int m = n + 1;
            while (m < fn->nins && fn->ins[m].op == IR_LABEL)
                m++;
            if (m < fn->nins)
                branch_to(F, CF_T, fn->nlabels);
        }
        return;

    case IR_UD2:
        cf_illegal(t);
        return;
    case IR_FENCE:
        /* a single in-order core: nop synchronises the pipeline */
        cf_nop(t);
        return;

    case IR_BSWAP: {
        /* through the tmp slot: stored whole, read back a byte at a time
         * from the other end (ColdFire has no rotate) */
        long tmp = F->tmp_slot;
        if (i->size == 8) {
            mv(F, 4, hi_ea(F, i->a), fp_at(tmp), D0);
            mv(F, 4, lo_ea(F, i->a), fp_at(tmp + 4), D0);
            for (int h = 0; h < 2; h++) {
                int R = h ? D1 : D0;
                long base = h ? tmp : tmp + 4;
                for (int k = 3; k >= 0; k--) {
                    cf_move(t, 1, fp_at(base + k), cf_dreg(R));
                    if (k)
                        cf_shift_imm(t, CF_LSL, 8, R);
                }
            }
            wr64(F, i->dst, D0, D1);
            return;
        }
        {
            int R = wreg(F, i->dst, D0);
            mv(F, 4, vea(F, i->a), fp_at(tmp), D1);
            if (i->size == 2) {
                cf_moveq(t, 0, R);
                cf_move(t, 1, fp_at(tmp + 3), cf_dreg(R));
                cf_shift_imm(t, CF_LSL, 8, R);
                cf_move(t, 1, fp_at(tmp + 2), cf_dreg(R));
            } else {
                for (int k = 3; k >= 0; k--) {
                    cf_move(t, 1, fp_at(tmp + k), cf_dreg(R));
                    if (k)
                        cf_shift_imm(t, CF_LSL, 8, R);
                }
            }
            wrote(F, i->dst, R);
        }
        return;
    }

    case IR_VA_START:
        /* the first unnamed argument's word, after the named ones */
        cf_lea(t, fp_at(8 + F->va_named), A0);
        cf_move(t, 4, vea(F, i->a), cf_areg(A1));
        cf_move(t, 4, cf_areg(A0), cf_ind(A1));
        return;

    case IR_I2F: case IR_F2I: case IR_F2F: {
        int src_w = i->size;
        if (i->op == IR_F2F && src_w == i->w) {
            if (src_w == 8) {
                mv(F, 4, hi_ea(F, i->a), hi_ea(F, i->dst), D0);
                mv(F, 4, lo_ea(F, i->a), lo_ea(F, i->dst), D0);
            } else {
                mv(F, 4, vea(F, i->a), vea(F, i->dst), D0);
            }
            return;
        }
        if (src_w > 8 || i->w > 8)
            cf_refuse(F, i, "a conversion of a 128-bit value");
        if (i->op == IR_I2F && src_w == 8 && !is_wide(F, i->a)) {
            /* a 32-bit value asked for as 64: zero-extended */
            need_out(F, 8);
            cf_clr(t, 4, out_at(0));
            harg32(F, 4, i->a);
        } else if (src_w == 8) {
            harg64(F, 0, i->a);
        } else {
            harg32(F, 0, i->a);
        }
        call_helper(F, cvt_name(i));
        if (i->dst >= 0) {
            if (is_wide(F, i->dst) && i->w <= 4)
                res32_to64(F, i->dst, i->op == IR_F2I && i->sign);
            else if (is_wide(F, i->dst))
                wr64(F, i->dst, D0, D1);
            else
                wrote(F, i->dst, D0);
        }
        return;
    }

    case IR_ASM: {
        /* Extended asm, assembled in irgen (coldfire/irgen.c
         * irg_asm_coldfire) against the vocabulary in coldfire/asm.c. This
         * only places the operands and splices the bytes -- RX's and
         * Xtensa's lowering with the m68k's two register files. The moves
         * in and out are each ONE parallel move (a1, which no operand is
         * given, breaks a cycle and carries a "+" output's address); no
         * operand is ever in a1, a6 or a7. */
        struct ir_asm *ia = i->asm_ir;
        int vreg_[16], vdst[16], nval = 0;
        if (ia->cont) {
            int k = n - 1;
            while (k >= 0 && fn->ins[k].op == IR_ASM && fn->ins[k].asm_ir &&
                   fn->ins[k].asm_ir->cont)
                k--;
            if (k < 0 || fn->ins[k].op != IR_ASM)
                internal_error("coldfire: %s: an asm's further output is not "
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
        for (int k = 0; k < ia->nin; k++)
            if (ia->in[k].reg < 0 || ia->in[k].reg == A1 ||
                ia->in[k].reg >= CF_FP)
                internal_error("coldfire: %s: an asm operand in %s", fn->name,
                               cf_reg_name(ia->in[k].reg));
        for (int k = 0; k < ia->nout; k++)
            if (!ia->out[k].mem && ia->out[k].size > 4)
                cf_refuse(F, i, "an asm output wider than a register");
        {
            int naddr = 0;
            for (int k = 0; k < ia->nout; k++)
                naddr += !ia->out[k].val && !ia->out[k].mem;
            if (ia->scr < 0 && naddr > 0)
                cf_refuse(F, i, "an asm with no address register left around "
                                "it");
        }
        /* In: an input's value, an "m" output's address, a "+" output's
         * address -- the register-resident ones as one parallel move, then
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
                int m = ra_parallel_move(pd, ps, npm, A1, od, os,
                                         (int)(sizeof od / sizeof od[0]));
                if (m < 0)
                    cf_refuse(F, i, "an asm whose operands cannot be moved "
                                    "into place");
                for (int k = 0; k < m; k++)
                    cf_move(t, 4, reg_ea(os[k]), reg_ea(od[k]));
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
                /* A "+" output starts with the lvalue's CURRENT value,
                 * loaded through its address (in a1 when the operand is a
                 * data register, which cannot hold one as a base). */
                if (o->inout && !o->mem) {
                    int base = o->reg;
                    if (!CF_IS_A(base)) {
                        cf_move(t, 4, reg_ea(o->reg), cf_areg(A1));
                        base = A1;
                    }
                    cf_move(t, o->size, cf_ind(base), reg_ea(o->reg));
                }
            }
        }
        for (int k = 0; k < ia->codelen; k++)
            code_byte(t, ia->code[k]);
        /* Out, through an address (live across the asm, so still there). */
        for (int k = 0; k < ia->nout; k++) {
            const struct ir_asm_op *o = &ia->out[k];
            if (o->mem || o->val)
                continue;
            rd(F, o->temp, ia->scr);
            cf_move(t, o->size, reg_ea(o->reg), cf_ind(ia->scr));
        }
        /* Out, as values: those in memory first, then the register-
         * resident ones as one parallel move. */
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
                    wrote(F, vdst[k], vreg_[k]);
                }
            }
            if (npm) {
                int od[32], os[32];
                int m = ra_parallel_move(pd, ps, npm, A1, od, os,
                                         (int)(sizeof od / sizeof od[0]));
                if (m < 0)
                    cf_refuse(F, i, "an asm whose outputs cannot be moved "
                                    "into place");
                for (int k = 0; k < m; k++)
                    cf_move(t, 4, reg_ea(os[k]), reg_ea(od[k]));
            }
        }
        return;
    }

    /* ---- atomics: a plain read-modify-write with interrupts masked ---- */
    case IR_XCHG: case IR_XADD: case IR_ARMW: {
        int sz = i->size;
        need_atomic_size(F, i);
        cf_move(t, 4, vea(F, i->a), cf_areg(A0));
        irq_off(F);
        if (sz < 4)
            cf_moveq(t, 0, D0);
        cf_move(t, sz, cf_ind(A0), cf_dreg(D0));
        if (i->op == IR_XCHG) {
            mv(F, sz, vea_n(F, i->b, sz), cf_ind(A0), D1);
        } else {
            cf_move(t, 4, vea(F, i->b), cf_dreg(D1));
            if (i->op == IR_XADD) {
                cf_alu(t, CF_ADD, cf_dreg(D0), D1);
            } else {
                switch ((int)i->imm) {
                case '&': cf_alu(t, CF_AND, cf_dreg(D0), D1); break;
                case '|': cf_alu(t, CF_OR, cf_dreg(D0), D1); break;
                case '^': cf_alu_mem(t, CF_EOR, D0, cf_dreg(D1)); break;
                default:
                    cf_alu(t, CF_AND, cf_dreg(D0), D1);
                    cf_unary(t, CF_NOT, D1);
                    break;
                }
            }
            cf_move(t, sz, cf_dreg(D1), cf_ind(A0));
        }
        irq_restore(F, D1);
        wrote(F, i->dst, D0);
        return;
    }
    case IR_CAS: {
        /* dst = *a; if (dst == b) *a = c -- the low `size` bytes */
        int sz = i->size, skip;
        need_atomic_size(F, i);
        cf_move(t, 4, vea(F, i->a), cf_areg(A0));
        irq_off(F);
        if (sz < 4)
            cf_moveq(t, 0, D0);
        cf_move(t, sz, cf_ind(A0), cf_dreg(D0));
        cf_move(t, 4, vea(F, i->b), cf_dreg(D1));
        ext_reg(F, D1, sz, 0);
        cf_alu(t, CF_CMP, cf_dreg(D1), D0);
        skip = br_place(F, CF_NE);
        mv(F, sz, vea_n(F, i->c, sz), cf_ind(A0), D1);
        br_land(F, skip);
        irq_restore(F, D1);
        wrote(F, i->dst, D0);
        return;
    }
    case IR_CMPXCHG: {
        /* *a against *b; c on a match; dst = matched, *b = what was seen */
        int sz = i->size, skip;
        need_atomic_size(F, i);
        cf_move(t, 4, vea(F, i->a), cf_areg(A0));
        cf_move(t, 4, vea(F, i->b), cf_areg(A1));
        irq_off(F);
        if (sz < 4) {
            cf_moveq(t, 0, D0);
            cf_moveq(t, 0, D1);
        }
        cf_move(t, sz, cf_ind(A0), cf_dreg(D0));
        cf_move(t, sz, cf_ind(A1), cf_dreg(D1));
        cf_alu(t, CF_CMP, cf_dreg(D1), D0);
        cf_scc(t, CF_EQ, D1);
        skip = br_place(F, CF_NE);
        {
            /* c may be in memory; d1 holds the answer, so through a1's
             * neighbour -- c is moved directly where ColdFire allows */
            struct cf_ea c = vea_n(F, i->c, sz), to = cf_ind(A0);
            if (!cf_move_ok(sz, &c, &to)) {
                cf_move(t, 4, vea(F, i->c), cf_dreg(D0));
                cf_move(t, sz, cf_dreg(D0), cf_ind(A0));
                cf_move(t, sz, cf_ind(A0), cf_dreg(D0));
            } else {
                cf_move(t, sz, c, cf_ind(A0));
            }
        }
        br_land(F, skip);
        cf_move(t, sz, cf_dreg(D0), cf_ind(A1));
        irq_restore(F, D0);
        cf_unary(t, CF_EXTBL, D1);
        cf_unary(t, CF_NEG, D1);
        wrote(F, i->dst, D1);
        return;
    }

    case IR_FRAMEADDR:
        /* a6 points at the caller's a6, the return address above it */
        if (i->dst >= 0)
            mv(F, 4, cf_areg(CF_FP), vea(F, i->dst), D0);
        return;
    case IR_ALLOCA:
        /* A fresh 16-aligned block (the IR's promise; the stack itself is
         * only 4-aligned): the stack pointer down by the size plus 15,
         * rounded to a word, and the block the first 16-aligned address
         * above the argument area, which moves down with it. */
        cf_move(t, 4, vea(F, i->a), cf_dreg(D0));
        cf_alu_imm(t, CF_ADD, 18, D0);
        cf_alu_imm(t, CF_AND, -4, D0);
        cf_alua(t, CF_SUB, cf_dreg(D0), CF_SP);
        cf_lea(t, out_at(F->out_bytes + 15), A0);
        cf_move(t, 4, cf_areg(A0), cf_dreg(D0));
        cf_alu_imm(t, CF_AND, -16, D0);
        wrote(F, i->dst, D0);
        return;
    case IR_SPSAVE:
        if (i->dst >= 0)
            mv(F, 4, cf_areg(CF_SP), vea(F, i->dst), D0);
        return;
    case IR_SPRESTORE:
        cf_move(t, 4, vea(F, i->a), cf_areg(CF_SP));
        return;

    case IR_SWITCH: {
        /* A table of 32-bit offsets from itself, after the dispatch:
         *
         *     cmpi.l #n,dI ; bcc.w default         (unsigned >= n)
         *     lea (10,pc),a0                       (the table)
         *     move.l (0,a0,dI.l*4),d1
         *     jmp (0,a0,d1.l)
         *   tab: .long L0-tab, L1-tab, ...                          */
        int nc = fn->jt[i->jt].n;
        int ri = rdr(F, i->a, D0);
        int tab;
        cf_alu_imm(t, CF_CMP, nc, ri);
        branch_to(F, CF_CC, i->label);
        cf_lea(t, cf_pcdisp(10), A0);
        cf_move(t, 4, cf_idx(A0, 0, ri, 4), cf_dreg(D1));
        cf_jmp(t, cf_idx(A0, 0, D1, 1));
        tab = t->len;
        for (int k = 0; k < nc; k++) {
            want_label(F, t->len, fn->jt[i->jt].labels[k], FX_TAB, tab);
            cf_l(t, 0);
        }
        code_mark_data(t, tab, t->len);
        return;
    }
    case IR_LABELADDR: {
        if (cg_label_mark(i))       /* static data's marker: no code */
            return;
        /* lea (0,pc),a0 ; adda.l #label-., a0 -- the long branch's way
         * to an address, which reaches anywhere. `lea (label,pc)` alone
         * reached 32 KiB, and a threaded interpreter's -O0 body passes
         * that (tests/exec/computed-goto-more.c's far()). */
        int ext = t->len + 2, imm;
        cf_lea(t, cf_pcdisp(0), A0);
        imm = t->len + 2;
        cf_alua(t, CF_ADD, cf_imm(0), A0);
        want_label(F, imm, i->label, FX_LONG, ext);
        wrote(F, i->dst, A0);
        return;
    }
    case IR_IGOTO:
        cf_jmp(t, cf_ind(areg(F, i->a, A0)));
        return;
    case IR_LANDING:
        cf_refuse(F, i, "an exception landing pad (C++)");
        return;
    default:
        cf_refuse(F, i, "this operation");
    }
}

/* Copy `size` bytes from (a1) to (a0) (copy) or zero them (!copy),
 * post-incrementing: whole words, then a halfword and a byte. Straight
 * line up to 64 bytes, a counted loop in d0 beyond. a0, a1 and d0 are
 * scratch. */
static void copy_block(struct cf_fn *F, int copy, long size)
{
    struct code *t = F->t;
    long words = size / 4;
    if (words > 16) {
        int top;
        ldi(F, words, cf_dreg(D0), D0);
        top = t->len;
        if (copy) cf_move(t, 4, cf_post(A1), cf_post(A0));
        else      cf_clr(t, 4, cf_post(A0));
        cf_addq(t, 1, 1, cf_dreg(D0));
        br_back(F, CF_NE, top);
    } else {
        for (long k = 0; k < words; k++) {
            if (copy) cf_move(t, 4, cf_post(A1), cf_post(A0));
            else      cf_clr(t, 4, cf_post(A0));
        }
    }
    size -= words * 4;
    if (size >= 2) {
        if (copy) cf_move(t, 2, cf_post(A1), cf_post(A0));
        else      cf_clr(t, 2, cf_post(A0));
        size -= 2;
    }
    if (size) {
        if (copy) cf_move(t, 1, cf_post(A1), cf_post(A0));
        else      cf_clr(t, 1, cf_post(A0));
    }
}

/* ---- the address class -----------------------------------------------------
 *
 * A vreg may live in a2-a5 when everything done to it an address
 * register can do: it is defined by a copy, an address, a 32-bit load or
 * an add or subtract, and read as a load or store's base, a copy, an
 * add, subtract or compare operand, a call's argument or a return value.
 * Then a dereference uses it as the base directly. Anything else -- a
 * shift, a multiply, a byte of it -- keeps it in the data class. */
static char *addr_map(const struct ir_func *fn, const char *wide)
{
    int nv = fn->nvregs;
    char *ok = xcalloc((size_t)(nv ? nv : 1), 1);
    char *base = xcalloc((size_t)(nv ? nv : 1), 1);
    for (int v = 0; v < nv; v++)
        ok[v] = !(wide && wide[v]);
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        int d = ra_ins_def(i);
        /* the definitions an address register can take */
        if (d >= 0 && d < nv) {
            int good = 0;
            switch (i->op) {
            case IR_MOV: case IR_ADDR: case IR_GADDR: case IR_STRADDR:
            case IR_FADDR: case IR_ALLOCA: case IR_SPSAVE: case IR_FRAMEADDR:
                good = i->w <= 4 || i->op != IR_MOV;
                break;
            case IR_LOAD:
                good = i->size == 4;
                break;
            case IR_LDVAR:
                good = i->size == 4 && i->w == 4;
                break;
            case IR_STVAR:
                good = i->size == 4;
                break;
            case IR_ADD: case IR_SUB:
                good = i->w == 4 && !i->flt;
                break;
            default:
                break;
            }
            if (!good)
                ok[d] = 0;
            if (d < fn->nvars && i->op == IR_STVAR && i->size != 4)
                ok[d] = 0;
        }
        /* the uses */
        switch (i->op) {
        case IR_LOAD:
            if (i->a >= 0 && i->a < nv) base[i->a] = 1;
            break;
        case IR_STORE:
            if (i->a >= 0 && i->a < nv) base[i->a] = 1;
            if (i->b >= 0 && i->b < nv && i->size != 4) ok[i->b] = 0;
            break;
        case IR_MOV: case IR_RET:
            break;
        case IR_STVAR:
            if (i->a >= 0 && i->a < nv && i->size != 4) ok[i->a] = 0;
            break;
        case IR_ADD: case IR_SUB:
            if (i->flt || i->w != 4) {
                if (i->a >= 0 && i->a < nv) ok[i->a] = 0;
                if (!i->imm_b && i->b >= 0 && i->b < nv) ok[i->b] = 0;
            }
            break;
        case IR_CMP:
            if (i->w != 4 || i->flt) {
                if (i->a >= 0 && i->a < nv) ok[i->a] = 0;
                if (!i->imm_b && i->b >= 0 && i->b < nv) ok[i->b] = 0;
            }
            break;
        case IR_BRZ: case IR_BRNZ:
            if (i->w != 4 && i->a >= 0 && i->a < nv) ok[i->a] = 0;
            break;
        case IR_CALL:
            for (int k = 0; k < i->nargs; k++) {
                int v = i->argv[k].vreg;
                if (v >= 0 && v < nv && i->argv[k].size != 4 &&
                    !i->argv[k].is_struct)
                    ok[v] = 0;
            }
            break;
        case IR_MEMCPY: case IR_MEMZERO: case IR_IGOTO: case IR_SPRESTORE:
        case IR_VA_START:
            break;
        default: {
            /* any other reader wants a data register */
            if (i->a >= 0 && i->a < nv) ok[i->a] = 0;
            if (!i->imm_b && i->b >= 0 && i->b < nv) ok[i->b] = 0;
            if (i->c >= 0 && i->c < nv && i->op == IR_SELECT) ok[i->c] = 0;
            if (i->op == IR_CAS || i->op == IR_CMPXCHG) {
                if (i->c >= 0 && i->c < nv) ok[i->c] = 0;
            }
            break;
        }
        }
        if ((i->op == IR_XCHG || i->op == IR_XADD || i->op == IR_ARMW ||
             i->op == IR_CAS || i->op == IR_CMPXCHG) &&
            i->a >= 0 && i->a < nv)
            base[i->a] = 1;
    }
    /* worth a register of its own only when it is a base somewhere */
    for (int v = 0; v < nv; v++)
        ok[v] = ok[v] && base[v];
    free(base);
    return ok;
}

/* ---- one function --------------------------------------------------------- */

static void gen_func(struct ir_func *fn, struct code *t, struct cf_sites *st,
                     int keep_vars)
{
    struct func *f = fn->src;
    struct cf_fn F;
    int i;

    memset(&F, 0, sizeof F);
    F.fn = fn; F.t = t; F.st = st;
    F.keep_vars = keep_vars;
    fn->nlines = 0;                 /* -g: this attempt's rows only */
    F.wide = wide_map(fn);
    F.nshr = ra_narrow_hishift(fn);
    for (int v = 0; v < fn->nvregs; v++)
        if (F.nshr[v]) F.wide[v] = 0;
    if (g_cf_regalloc) {
        char *pin = keep_vars ? ra_debug_pin_vars(fn) : (char *)0;
        int nv = fn->nvregs;
        char *amap = getenv("EMBCC_CF_NOAREG") ? NULL : addr_map(fn, F.wide);
        char *excl = xcalloc((size_t)(nv ? nv : 1), 1);
        int *aloc = NULL, aused[RA_MAXPOOL], naused = 0;
        for (int v = 0; v < nv; v++)
            excl[v] = (amap && amap[v]) || (pin && pin[v]);
        F.loc = ra_allocate(fn, &CF_RATGT, F.wide, excl, F.used_callee,
                            &F.nsave);
        if (amap) {
            for (int v = 0; v < nv; v++)
                if (pin && pin[v]) amap[v] = 0;
            aloc = ra_allocate_fp(fn, &CF_RATGT, F.wide, amap, aused,
                                  &naused);
            for (int v = 0; aloc && v < nv; v++)
                if (amap[v] && aloc[v] >= 0)
                    F.loc[v] = aloc[v];
            for (int k = 0; k < naused; k++)
                F.used_callee[F.nsave++] = aused[k];
            free(aloc);
            free(amap);
        }
        free(excl);
        free(pin);
        if (fn->nvregs) {
            F.usecnt = xmalloc((size_t)fn->nvregs * sizeof *F.usecnt);
            ra_count_vreg_uses(fn, F.usecnt);
        }
        {
            const char *lim = getenv("EMBCC_CF_RA_MAX");
            if (lim) {
                int nl = atoi(lim);
                for (int v = nl; v < fn->nvregs; v++)
                    F.loc[v] = -1;
            }
        }
    }
    /* A callee-saved register an asm changes -- its clobbers, the
     * registers its template names, its operands' -- is saved by the
     * prologue's movem, as GCC saves it. */
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_asm *ia = fn->ins[n].op == IR_ASM ? fn->ins[n].asm_ir
                                                          : NULL;
        if (!ia || ia->cont)
            continue;
        for (int r = 2; r <= 13; r++) {
            int have = 0;
            if (!cf_callee_saved(r) || !(ia->clob >> r & 1))
                continue;
            for (int k = 0; k < F.nsave; k++)
                have |= F.used_callee[k] == r;
            if (!have && F.nsave < 2 * RA_MAXPOOL)
                F.used_callee[F.nsave++] = r;
        }
    }
    layout(&F);

    F.label_off = xmalloc((size_t)(fn->nlabels + 1) * sizeof *F.label_off);
    f->code_align = 2;
    if (target_debug_info()) {
        int nv = fn->nvars ? fn->nvars : 1;
        fn->var_off = xmalloc((size_t)nv * sizeof *fn->var_off);
        for (int v = 0; v < fn->nvars; v++)
            fn->var_off[v] = F.slot[v] == NOSLOT ? IR_VAR_NO_LOC
                           : ra_var_home(fn, v, 1, obj_slot(&F, v));
    }
    {
    int len0 = t->len, nl0 = fn->nlines;
    int se0 = F.st->next, ss0 = F.st->nstr, sg0 = F.st->ng, sf0 = F.st->nf;
    char *form = NULL;
    int nform = 0, shrunk = 0;
    for (;;) {
    int grew = 0;
    t->len = len0;
    fn->nlines = nl0;
    F.st->next = se0; F.st->nstr = ss0; F.st->ng = sg0; F.st->nf = sf0;
    F.nfix = 0;
    for (i = 0; i <= fn->nlabels; i++)
        F.label_off[i] = -1;
    F.skip_next = 0;
    F.form = form;
    F.nform = nform;
    f->code_off = t->len;

    /* The prologue: the frame, the callee-saved registers, the sret
     * pointer, and the parameters the allocator put in registers. */
    cf_link(t, CF_FP, -F.frame);
    if (F.nsave == 1) {
        cf_move(t, 4, reg_ea(F.used_callee[0]), fp_at(F.save_at));
    } else if (F.nsave) {
        unsigned mask = 0;
        for (int k = 0; k < F.nsave; k++)
            mask |= 1u << F.used_callee[k];
        cf_movem_store(t, mask, fp_at(F.save_at));
    }
    if (F.sret_slot != NOSLOT)
        cf_move(t, 4, cf_areg(A1), fp_at(F.sret_slot));
    for (int p = 0; p < fn->nparams && p < fn->nvregs; p++)
        if (in_reg(&F, p))
            cf_move(t, 4, fp_at(F.slot[p]), reg_ea(F.loc[p]));

    for (i = 0; i < fn->nins; i++) {
        gen_ins(&F, i);
        if (F.skip_next) {
            F.skip_next = 0;
            i++;
        }
    }
    F.label_off[fn->nlabels] = t->len;
    /* a returned pointer, and an sret buffer's address, in a0 as well
     * (the SVR4 rule); here rather than at each return, whose branch
     * here may take a0 */
    if (fn_sret(fn) || (fn->ret_abi.ty && fn->ret_abi.ty->kind == TY_PTR))
        cf_move(t, 4, cf_dreg(D0), cf_areg(A0));
    if (F.nsave == 1) {
        cf_move(t, 4, fp_at(F.save_at), reg_ea(F.used_callee[0]));
    } else if (F.nsave) {
        unsigned mask = 0;
        for (int k = 0; k < F.nsave; k++)
            mask |= 1u << F.used_callee[k];
        cf_movem_load(t, fp_at(F.save_at), mask);
    }
    cf_unlk(t, CF_FP);
    cf_rts(t);

    if (!form) {
        form = xcalloc((size_t)(F.nfix ? F.nfix : 1), 1);
        nform = F.nfix;
    }
    if (nform != F.nfix)
        internal_error("coldfire: %s: %d branches, then %d", fn->name, nform,
                       F.nfix);
    for (i = 0; i < F.nfix; i++) {
        int target = F.label_off[F.fix[i].label];
        int at = F.fix[i].at;
        if (target < 0)
            internal_error("coldfire: label %d of %s was never placed",
                           F.fix[i].label, fn->name);
        if (F.fix[i].kind == FX_TAB) {
            cf_wrl(t, at, (unsigned long)(target - F.fix[i].base) &
                          0xffffffffUL);
            continue;
        }
        if (F.fix[i].kind == FX_LONG) {
            cf_wrl(t, at, (unsigned long)((long)target - F.fix[i].base) &
                          0xffffffffUL);
            continue;
        }
        if (form[i] == 1) {
            long d = (long)target - (at + 2);
            if (d == 0) {
                cf_wrw(t, at, 0x4e71u);      /* to the next word: a nop */
            } else {
                if (d < -128 || d > 127)
                    internal_error("coldfire: %s: a short branch no longer "
                                   "reaches", fn->name);
                cf_wrw(t, at, (cf_rdw(t, at) & 0xff00u) |
                              ((unsigned)d & 0xff));
            }
            continue;
        }
        if (!cf_patch_bcc(t, at, target)) {
            if (shrunk)
                internal_error("coldfire: %s: a branch stopped reaching "
                               "when the code shrank", fn->name);
            form[i] = 2;
            grew = 1;
        }
    }
    if (grew)
        continue;                       /* again, with the long forms */
    if (shrunk)
        break;
    /* every branch reaches: those within 8 bits take the short form */
    for (i = 0; i < F.nfix; i++) {
        long d = (long)F.label_off[F.fix[i].label] - (F.fix[i].at + 2);
        if (F.fix[i].kind == FX_B && form[i] == 0 && d >= -128 && d <= 127) {
            form[i] = 1;
            shrunk = 1;
        }
    }
    if (!shrunk)
        break;
    }                                   /* the attempts */
    free(form);
    }

    f->code_len = t->len - f->code_off;
    f->stack_bytes = (int)F.frame;
    free(F.usecnt);
    free(F.slot);
    cg_note_labels(fn, F.label_off);
    free(F.label_off);
    free(F.fix);
    free(F.wide);
    free(F.nshr);
    free(F.loc);
}

void codegen_unit_coldfire(struct ir_unit *iu, struct code *text,
                           struct extcall **ext, int *next,
                           struct strsite **strs, int *nstrs,
                           struct gsite **gs, int *ngs,
                           struct fsite **fs, int *nfs, int keep_vars,
                           int optimize, int no_sse, int regalloc)
{
    struct cf_sites st;
    (void)optimize; (void)no_sse;
    g_cf_regalloc = regalloc;
    {
        const char *m = getenv("EMBCC_RA_MAXPOOL");
        if (m && atoi(m) > 0) {
            g_cf_maxd = atoi(m) < CF_NPOOL ? atoi(m) : CF_NPOOL;
            g_cf_maxa = atoi(m) < CF_NAPOOL ? atoi(m) : CF_NAPOOL;
        }
    }
    memset(&st, 0, sizeof st);
    for (int n = 0; n < iu->nfuncs; n++) {
        if (regalloc && !keep_vars && !getenv("EMBCC_NO_MEMOFF")) {
            char *w = wide_map(&iu->funcs[n]);
            ra_fold_memoff(&iu->funcs[n], -32768, 32767 - 8, 4, 4, w, 0, 0);
            free(w);
        }
        gen_func(&iu->funcs[n], text, &st, keep_vars);
    }
    cg_resolve_strsites(iu, st.str, st.nstr);
    *ext = st.ext;   *next = st.next;
    *strs = st.str;  *nstrs = st.nstr;
    *gs = st.g;      *ngs = st.ng;
    *fs = st.f;      *nfs = st.nf;
}
