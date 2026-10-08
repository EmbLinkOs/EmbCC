/* 32-bit PowerPC code generation: the embedded EABI (SVR4), big-endian,
 * soft float (docs/internals/powerpc-plan.md).
 *
 * The shape is the MIPS and RV32 backends' (src/arch/mips/codegen.c):
 * every vreg has one home -- a register the shared allocator gave it, or a
 * frame slot -- and every operation reads its operands through rdr/rd and
 * writes its result through wreg/wrote, so the code is correct with the
 * allocator off and smaller with it on. A 64-bit value is a register pair
 * named by its first register, which holds the HIGH word (PHI) -- the ABI's
 * order for r3:r4, and memory's, the high word at the lower address. What
 * PowerPC changes, and where:
 *
 *   * A comparison sets a CR field and a branch tests one of its bits; a
 *     0/1 result is mfcr and an rlwinm of the bit (cmp_to_reg).
 *   * r0 reads as the number 0 as a load's base and as addi's source, so
 *     it is used here only as DATA (SCR): an operand, a moved value, a
 *     frame offset in an indexed access (ld_sp's far form). emit.c refuses
 *     it anywhere it would mean 0.
 *   * The convention passes every composite BY REFERENCE (the caller
 *     copies it into its own frame, the callee copies it into its slot),
 *     pairs in odd-numbered registers, and returns small composites
 *     right-justified in r3:r4 (place_arg, gen_call, IR_RET).
 *   * The frame keeps the back chain at 0(r1) and the LR save word at
 *     4(r1) for the callee: a non-leaf function saves LR at frame+4, in
 *     its caller's frame, and an alloca keeps the chain (IR_ALLOCA).
 *   * No delay slots, and misaligned accesses are the hardware's.
 *
 * Refused by name: atomics wider than a word,
 * __builtin_frame_address/return_address, a branch beyond +-32 MiB, and
 * __int128 (ILP32). Inline asm is assembled by ppc/asm.c and placed here
 * (IR_ASM). THE RULE.
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

struct ppc_fn;
static void copy_block(struct ppc_fn *F, int copy, long size);

/* The scratch registers, none of them in the allocator's pool: r11, r12
 * (the A pair of a 64-bit operation, and the accumulator and second
 * operand of a 32-bit one), r10, r9 (the B pair; r10 an address), and r0
 * (data only). */
#define A_LO 11
#define A_HI 12
#define B_LO 10
#define B_HI 9
#define ACC  11
#define TMP  12
#define ADDR 10
#define SCR  0
#define LRR  12          /* LR's way to and from its save word */

/* The pairs: named by their first register, which holds the high word. */
#define PHI(r) (r)
#define PLO(r) ((r) + 1)
#define WHI 0
#define WLO 4

struct ppc_sites {
    struct extcall *ext;   int next, capext;
    struct strsite *str;   int nstr, capstr;
    struct gsite *g;       int ng, capg;
    struct fsite *f;       int nf, capf;
};

struct ppc_fn {
    int *usecnt;
    int skip_next;
    int want_debug;
    struct ir_func *fn;
    int *loc;            /* per vreg: its register, -1 in memory; NULL at -O0 */
    int used_callee[RA_MAXPOOL + 2];
    int pair_used[16], npair;
    int nsave;
    struct code *t;
    struct ppc_sites *st;
    char *wide;
    char *nshr;
    long *slot;
    long frame;          /* bytes r1 moves down by */
    long out_bytes;      /* the header and the outgoing stack arguments */
    long argcopy_at;     /* the by-reference copies of struct arguments */
    long scratch_at;
    long sret_slot;
    long va_save, va_tag;
    int va_gpr;          /* GPRs the named parameters took */
    long va_stk;         /* bytes of stack the named parameters took */
    long save_at;
    int fb;              /* the frame base: r1, or r31 under alloca */
    int leaf;
    char *tail;
    int *label_off;
    struct { int at; int label; int kind; int base; } *fix;
    int nfix, capfix;
    const char *longb;
    int nlongb;
};

/* ---- the allocator's view of this machine ------------------------------
 *
 * Caller-saved first: r3-r8 (r9-r12 are the scratches), then r14-r31.
 * r31 is the frame base under alloca and then not handed out. */
#define PPC_NPOOL 24
static const int PPC_POOL[PPC_NPOOL] = {
    3, 4, 5, 6, 7, 8,
    14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31
};
static unsigned long g_ppc_taken;       /* registers the pair pass took */
static int g_ppc_pairs = 1;
static int g_ppc_pool[PPC_NPOOL];

static const int *ppc_pool_for(const struct ir_func *fn, int *n)
{
    int k = 0;
    for (int j = 0; j < PPC_NPOOL; j++) {
        if (fn->has_alloca && PPC_POOL[j] == 31)
            continue;
        if (g_ppc_taken >> PPC_POOL[j] & 1)
            continue;
        g_ppc_pool[k++] = PPC_POOL[j];
    }
    *n = k;
    return g_ppc_pool;
}

/* The PAIR pool, each named by its high register: the argument pairs
 * r3:r4, r5:r6, r7:r8 (where a 64-bit value is passed and returned, and a
 * helper's operands go), then r14:r15 .. r30:r31. */
#define PPC_NPAIRS 12
static const int PPC_PAIRS[PPC_NPAIRS] = {
    3, 5, 7, 14, 16, 18, 20, 22, 24, 26, 28, 30
};
static int g_ppc_pairpool[PPC_NPAIRS];
static const int *ppc_pair_pool_for(const struct ir_func *fn, int *n)
{
    int k = 0;
    for (int j = 0; j < PPC_NPAIRS; j++)
        if (!(fn->has_alloca && PPC_PAIRS[j] == 30))
            g_ppc_pairpool[k++] = PPC_PAIRS[j];
    *n = k;
    return g_ppc_pairpool;
}

static void ppc_pair_hints(const struct ir_func *fn, int *hint);

static int ppc_callee_saved(int r)
{
    return r >= 14 && r <= 31;
}

static int ppc_ldvar_plain(int size, int sign, int w)
{
    (void)sign;
    return size == 4 && w == 4;
}

/* Which instructions become a CALL the IR does not show as one: every
 * floating-point operation (soft float), and a 64-bit divide. */
int ppc_op_calls_helper(const struct ir_ins *i)
{
    if (i->flt)
        return i->op == IR_ADD || i->op == IR_SUB || i->op == IR_MUL ||
               i->op == IR_DIV || i->op == IR_CMP;
    if (i->op == IR_I2F || i->op == IR_F2I || i->op == IR_F2F)
        return 1;
    return (i->op == IR_DIV || i->op == IR_MOD) && i->w == 8;
}

static void ppc_abi_hints(const struct ir_func *fn, int *hint);

static const struct ra_target PPC_RATGT = {
    ppc_pool_for,
    ppc_callee_saved,
    ppc_ldvar_plain,
    1, 1, 1,
    ppc_op_calls_helper,
    0,              /* three-operand */
    ppc_abi_hints,
    NULL, NULL,     /* no FP class: soft float in the integer registers */
    1,
    NULL, NULL,
    1,              /* atomic_in_reg */
    0,
    1               /* asm_in_reg: see IR_ASM */
};

static int g_ppc_regalloc;

/* ---- refusal ------------------------------------------------------------ */

static void ppc_refuse(const struct ppc_fn *F, const struct ir_ins *i,
                       const char *what)
{
    char op[64];
    op[0] = 0;
    if (i)
        snprintf(op, sizeof op, " [%s w=%d size=%d]", ir_opname(i->op),
                 i->w, i->size);
    fprintf(stderr,
            "embcc: %s:%d: error: the PowerPC backend cannot lower %s yet "
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

/* ---- the SVR4 calling convention ---------------------------------------
 *
 * Read off clang for powerpc-none-eabi (docs/internals/powerpc-plan.md):
 * each argument takes the next of r3-r10; an 8-byte scalar an odd pair
 * (index rounded up to even), high word first; once one does not fit,
 * the register count is closed (8) and it and every later argument go on
 * the stack at 8(r1) upward, 4-aligned (8 for an 8-byte one). A composite
 * is one word: a pointer to a copy. */
struct argplace {
    int reg, nreg;       /* first argument register index (0 = r3), count */
    long stk;            /* offset in the stack area (from 8(r1)), or -1 */
};

static void place_arg(const struct ir_arg *a, int *gpr, long *stk,
                      struct argplace *p)
{
    int words = a->is_struct ? 1 : a->size > 4 ? 2 : 1;
    p->reg = -1;
    p->nreg = 0;
    p->stk = -1;
    if (words == 2) {
        *gpr = (*gpr + 1) & ~1;
        if (*gpr <= 6) {
            p->reg = *gpr;
            p->nreg = 2;
            *gpr += 2;
            return;
        }
        *gpr = 8;
        *stk = (*stk + 7) & ~7L;
        p->stk = *stk;
        *stk += 8;
        return;
    }
    if (*gpr < 8) {
        p->reg = (*gpr)++;
        p->nreg = 1;
        return;
    }
    p->stk = *stk;
    *stk += 4;
}

static int argreg(int n) { return PPC_R3 + n; }

/* A composite comes back in r3:r4 when it is 8 bytes or less, its bytes a
 * big-endian integer of its own size right-justified there; a larger one
 * through the hidden pointer in r3. */
static int fn_sret(const struct ir_func *fn)
{
    return fn->ret_abi.is_struct && fn->ret_abi.size > 8;
}

static int call_sret(const struct ir_ins *i)
{
    return i->retsize > 8;
}

static void ppc_abi_hints(const struct ir_func *fn, int *hint)
{
    int gpr = fn_sret(fn) ? 1 : 0;
    long stk = 0;
    struct argplace pl;
    for (int p = 0; fn->src && p < fn->nparams && p < fn->nvregs; p++) {
        const struct ir_arg *a = &fn->param_abi[p];
        place_arg(a, &gpr, &stk, &pl);
        if (pl.nreg == 1 && !a->is_struct && a->size <= 4)
            hint[p] = argreg(pl.reg);
    }
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        if (i->op == IR_RET && i->a >= 0 && i->a < fn->nvregs &&
            !fn->ret_abi.is_struct && fn->ret_abi.size <= 4)
            hint[i->a] = PPC_R3;
        if (i->op != IR_CALL && ppc_op_calls_helper(i) && i->w <= 4) {
            if (i->a >= 0 && i->a < fn->nvregs && hint[i->a] < 0)
                hint[i->a] = PPC_R3;
            if (!i->imm_b && i->b >= 0 && i->b < fn->nvregs &&
                hint[i->b] < 0)
                hint[i->b] = PPC_R4;
            if (i->dst >= 0 && i->dst < fn->nvregs && hint[i->dst] < 0)
                hint[i->dst] = PPC_R3;
        }
        if (i->op != IR_CALL)
            continue;
        if (!i->retsize && i->dst >= 0 && i->dst < fn->nvregs && i->w <= 4)
            hint[i->dst] = PPC_R3;
        gpr = call_sret(i) ? 1 : 0;
        stk = 0;
        for (int k = 0; k < i->nargs; k++) {
            const struct ir_arg *a = &i->argv[k];
            place_arg(a, &gpr, &stk, &pl);
            if (pl.nreg == 1 && !a->is_struct && a->size <= 4 &&
                a->vreg >= 0 && a->vreg < fn->nvregs)
                hint[a->vreg] = argreg(pl.reg);
        }
    }
}

/* ---- the frame -------------------------------------------------------------
 *
 * From r1 upward: the back chain and the callee's LR save word (8 bytes),
 * the outgoing stack arguments, the by-reference copies of struct
 * arguments, the shared temp slots, 64-bit temps without a pair, locals
 * (small ones first), the struct-return scratch, the sret pointer, a
 * variadic function's register save area and va_list record, and the
 * callee-saved registers. A multiple of 16. LR is saved at frame+4, in
 * the caller's frame. */
#define STACK_ALIGN 16

static void outgoing_area(const struct ppc_fn *F, long *out, long *copies)
{
    const struct ir_func *fn = F->fn;
    long most = 0, mostc = 0;
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        struct argplace pl;
        int gpr;
        long stk = 0, c = 0;
        if (i->op != IR_CALL)
            continue;
        gpr = call_sret(i) ? 1 : 0;
        for (int k = 0; k < i->nargs; k++) {
            place_arg(&i->argv[k], &gpr, &stk, &pl);
            if (i->argv[k].is_struct)
                c = ((c + 15) & ~15L) + i->argv[k].size;
        }
        if (stk > most) most = stk;
        if (c > mostc) mostc = c;
    }
    *out = 8 + most;
    *copies = mostc;
}

static int in_reg(const struct ppc_fn *F, int v);

static void layout(struct ppc_fn *F)
{
    struct ir_func *fn = F->fn;
    long out, copies, off;
    outgoing_area(F, &out, &copies);
    off = (out + 15) & ~15L;
    F->out_bytes = off;        /* alloca's blocks sit above it */
    F->argcopy_at = off;
    off += (copies + 15) & ~15L;

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
            struct ra_slots so = { loc2, NULL, g_ppc_regalloc, has_cgoto };
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
                if (align > STACK_ALIGN)
                    ppc_refuse(F, NULL, "a local aligned beyond the 16-byte "
                               "stack");
                if (align > 4 && size < 4 && fn->locals[v].is_int_or_ptr) {
                    /* a narrow integer's object is its home word's LAST
                     * bytes (obj_slot): that is what is aligned, and the
                     * word itself may be misaligned -- PowerPC reads one */
                    long pad = 4 - size;
                    off = ((off + pad + align - 1) & ~(long)(align - 1)) - pad;
                    F->slot[v] = off;
                    off += 4;
                    continue;
                }
                off = (off + align - 1) & ~(long)(align - 1);
                F->slot[v] = off;
                off += size;
            }
        free(lref);
    }
    F->scratch_at = (off + 15) & ~15L;
    off = F->scratch_at + fn->scratch_bytes;

    F->sret_slot = -1;
    if (fn_sret(fn)) {
        off = (off + 3) & ~3L;
        F->sret_slot = off;
        off += 4;
    }
    F->va_save = F->va_tag = -1;
    if (fn->is_varargs) {
        off = (off + 7) & ~7L;
        F->va_save = off;
        off += 32;
        F->va_tag = off;
        off += 12;
    }
    {
        long need = off + (long)F->nsave * 4;
        F->frame = (need + STACK_ALIGN - 1) & ~(long)(STACK_ALIGN - 1);
        /* a function with nothing in its frame and no call keeps none */
        if (F->leaf && off == F->out_bytes && copies == 0 && !F->nsave &&
            fn->scratch_bytes == 0 && off <= 16) {
            int any = 0;
            for (int v = 0; v < fn->nvregs; v++)
                any |= F->slot[v] >= 0;
            if (!any)
                F->frame = 0;
        }
        F->save_at = F->frame - (long)F->nsave * 4;
    }
}

/* ---- reading and writing a vreg ----------------------------------------- */

static int fits16(long off) { return off >= -32768 && off <= 32767; }

/* A frame access: a 16-bit displacement, or the offset in r0 and the
 * indexed form (r0 is a real register as rb). */
static void ld_at(struct ppc_fn *F, int reg, int base, long off, int size,
                  int sign)
{
    int bsign = sign && size == 1;
    if (fits16(off)) {
        ppc_load(F->t, reg, base, (int)off, size, sign && !bsign);
    } else {
        ppc_li(F->t, SCR, off);
        ppc_loadx(F->t, reg, base, SCR, size, sign && !bsign);
    }
    if (bsign)
        ppc_un(F->t, PPC_EXTSB, reg, reg);
}

static void st_at(struct ppc_fn *F, int reg, int base, long off, int size)
{
    if (fits16(off)) {
        ppc_store(F->t, reg, base, (int)off, size);
        return;
    }
    if (reg == SCR)
        internal_error("ppc: %s: r0 stored at a frame offset beyond 32 KiB",
                       F->fn->name);
    ppc_li(F->t, SCR, off);
    ppc_storex(F->t, reg, base, SCR, size);
}

static void ld_sp(struct ppc_fn *F, int reg, long off, int size, int sign)
{
    ld_at(F, reg, F->fb, off, size, sign);
}

static void st_sp(struct ppc_fn *F, int reg, long off, int size)
{
    st_at(F, reg, F->fb, off, size);
}

/* A store into the OUTGOING area, at the live r1. */
static void st_out(struct ppc_fn *F, int reg, long off, int size)
{
    st_at(F, reg, PPC_SP, 8 + off, size);
}

/* base + off, into `reg` (which may be r0: li and add take it as data) */
static void addr_at(struct ppc_fn *F, int reg, int base, long off)
{
    if (fits16(off) && reg != 0) {
        ppc_imm(F->t, PPC_ADDI, reg, base, off);
        return;
    }
    ppc_li(F->t, reg, off);
    ppc_alu(F->t, PPC_ADD, reg, base, reg);
}

static void addr_sp(struct ppc_fn *F, int reg, long off)
{
    addr_at(F, reg, F->fb, off);
}

static int in_reg(const struct ppc_fn *F, int v)
{
    return F->loc && v >= 0 && F->loc[v] >= 0;
}

static long sslot(const struct ppc_fn *F, int v)
{
    if (v < 0 || v >= F->fn->nvregs || F->slot[v] < 0)
        internal_error("ppc: %s: a path addresses vreg %d's slot, and it "
                       "has none", F->fn->name, v);
    return F->slot[v];
}

/* A 64-bit vreg read or written at 32 bits is its LOW word: PLO of its
 * pair, or the word at +4 of its slot. */
static int is_wide(const struct ppc_fn *F, int v)
{
    return F->wide && v >= 0 && v < F->fn->nvregs && F->wide[v];
}

static int reg_of(const struct ppc_fn *F, int v)
{
    return is_wide(F, v) ? PLO(F->loc[v]) : F->loc[v];
}

static long slot32(const struct ppc_fn *F, int v)
{
    return sslot(F, v) + (is_wide(F, v) ? WLO : 0);
}

/* A narrow integer VARIABLE's object is at the END of its four-byte home,
 * which is also read and written as a whole word: big-endian, its low
 * bytes are the word's last (mips/codegen.c, obj_slot). */
static long obj_slot(const struct ppc_fn *F, int v)
{
    if (v < F->fn->nvars) {
        const struct ir_local *L = &F->fn->locals[v];
        if (L->is_int_or_ptr && L->size > 0 && L->size < 4)
            return sslot(F, v) + 4 - L->size;
    }
    return sslot(F, v);
}

static long var_slot(const struct ppc_fn *F, int v, int size)
{
    long vs = v < F->fn->nvars ? F->fn->locals[v].size
            : is_wide(F, v) ? 8 : 4;
    return obj_slot(F, v) + (vs > size ? vs - size : 0);
}

static void rd(struct ppc_fn *F, int v, int reg)
{
    if (in_reg(F, v)) {
        if (reg_of(F, v) != reg)
            ppc_mr(F->t, reg, reg_of(F, v));
        return;
    }
    ld_sp(F, reg, slot32(F, v), 4, 0);
}

static int rdr(struct ppc_fn *F, int v, int scratch)
{
    if (in_reg(F, v))
        return reg_of(F, v);
    ld_sp(F, scratch, slot32(F, v), 4, 0);
    return scratch;
}

static int wreg(struct ppc_fn *F, int v, int scratch)
{
    return in_reg(F, v) ? reg_of(F, v) : scratch;
}

static void wrote(struct ppc_fn *F, int v, int reg)
{
    if (in_reg(F, v)) {
        if (reg_of(F, v) != reg)
            ppc_mr(F->t, reg_of(F, v), reg);
        return;
    }
    if (v < 0 || F->slot[v] < 0)
        return;
    st_sp(F, reg, slot32(F, v), 4);
}

static void wr(struct ppc_fn *F, int v, int reg)
{
    wrote(F, v, reg);
}

/* dl <- sl and dh <- sh as one parallel move. */
static void mv2(struct ppc_fn *F, int dl, int sl, int dh, int sh)
{
    if (dl == sh && dh == sl) {
        if (dl == sl) return;
        ppc_mr(F->t, SCR, sl);
        ppc_mr(F->t, dh, sh);
        ppc_mr(F->t, dl, SCR);
        return;
    }
    if (dl == sh) {
        if (dh != sh) ppc_mr(F->t, dh, sh);
        if (dl != sl) ppc_mr(F->t, dl, sl);
        return;
    }
    if (dl != sl) ppc_mr(F->t, dl, sl);
    if (dh != sh) ppc_mr(F->t, dh, sh);
}

static void rd64(struct ppc_fn *F, int v, int lo, int hi)
{
    if (in_reg(F, v)) {
        mv2(F, lo, PLO(F->loc[v]), hi, PHI(F->loc[v]));
        return;
    }
    ld_sp(F, lo, sslot(F, v) + WLO, 4, 0);
    ld_sp(F, hi, sslot(F, v) + WHI, 4, 0);
}

static void wr64(struct ppc_fn *F, int v, int lo, int hi)
{
    if (in_reg(F, v)) {
        mv2(F, PLO(F->loc[v]), lo, PHI(F->loc[v]), hi);
        return;
    }
    if (v < 0 || F->slot[v] < 0)
        return;
    st_sp(F, lo, sslot(F, v) + WLO, 4);
    st_sp(F, hi, sslot(F, v) + WHI, 4);
}

static long long imm_val(const struct ir_ins *i)
{
    return (long long)(int)(unsigned int)(unsigned long)i->imm;
}

static void operand_b(struct ppc_fn *F, const struct ir_ins *i, int reg)
{
    if (i->imm_b)
        ppc_li(F->t, reg, imm_val(i));
    else
        rd(F, i->b, reg);
}

static void operand_b64(struct ppc_fn *F, const struct ir_ins *i,
                        int lo, int hi)
{
    if (i->imm_b) {
        ppc_li(F->t, lo, (long long)(i->imm & 0xffffffffL));
        ppc_li(F->t, hi, (long long)((i->imm >> 32) & 0xffffffffL));
    } else {
        rd64(F, i->b, lo, hi);
    }
}

/* Sign- or zero-extend the low `size` bytes of rs. */
static void ext_reg(struct ppc_fn *F, int rdst, int rs, int size, int sign)
{
    if (size >= 4) {
        if (rdst != rs)
            ppc_mr(F->t, rdst, rs);
        return;
    }
    if (sign) {
        ppc_un(F->t, size == 1 ? PPC_EXTSB : PPC_EXTSH, rdst, rs);
        return;
    }
    ppc_rlwinm(F->t, rdst, rs, 0, size == 1 ? 24 : 16, 31);
}

/* A load or store at base + off, any size, misaligned or not (the
 * hardware's); a signed byte is lbz and extsb. */
static void ld_mem(struct ppc_fn *F, int rt, int base, long off, int size,
                   int sign)
{
    ld_at(F, rt, base, off, size, sign);
}

static void st_mem(struct ppc_fn *F, int rs, int base, long off, int size)
{
    st_at(F, rs, base, off, size);
}

/* ---- branches ------------------------------------------------------------
 *
 * A branch to a label is resolved when the function ends, with no
 * relocation: a bc reaches +-32 KiB, and one that does not is given the
 * long form -- the inverse bc over a `b`, which reaches 32 MiB -- and the
 * function generated again (gen_func). */
/* FX_ADDR: &&label, the two function-address sites from index `base`,
 * whose addend becomes the label's offset in the function */
enum { FX_B, FX_TAB, FX_ADDR };

static void want_label(struct ppc_fn *F, int at, int label, int kind)
{
    if (F->nfix == F->capfix) {
        F->capfix = F->capfix ? F->capfix * 2 : 16;
        F->fix = xrealloc(F->fix, (size_t)F->capfix * sizeof *F->fix);
    }
    F->fix[F->nfix].at = at;
    F->fix[F->nfix].label = label;
    F->fix[F->nfix].kind = kind;
    F->fix[F->nfix].base = 0;
    F->nfix++;
}

static int want_long(const struct ppc_fn *F)
{
    return F->longb && F->nfix < F->nlongb && F->longb[F->nfix];
}

/* Branch to `label` when cr0 says `cond` (-1: always). */
static void branch_to(struct ppc_fn *F, int cond, int label)
{
    if (cond < 0) {
        int at = ppc_b_placeholder(F->t);
        want_label(F, at, label, FX_B);
        return;
    }
    if (want_long(F)) {
        int at;
        ppc_w(F->t, ppc_enc_bc(ppc_cond_invert(cond), 0, 8));
        at = ppc_b_placeholder(F->t);
        want_label(F, at, label, FX_B);
        return;
    }
    {
        int at = ppc_bc_placeholder(F->t, cond, 0);
        want_label(F, at, label, FX_B);
    }
}

static void jump_to(struct ppc_fn *F, int label)
{
    branch_to(F, -1, label);
}

/* A branch within one lowering, patched to land here. */
static int br_place(struct ppc_fn *F, int cond)
{
    return cond < 0 ? ppc_b_placeholder(F->t)
                    : ppc_bc_placeholder(F->t, cond, 0);
}

static void br_land(struct ppc_fn *F, int at)
{
    if (!ppc_patch_branch(F->t, at, F->t->len))
        internal_error("ppc: %s: a branch inside one operation does not "
                       "reach", F->fn->name);
}

static void br_back(struct ppc_fn *F, int at, int target)
{
    if (!ppc_patch_branch(F->t, at, target))
        internal_error("ppc: %s: a loop inside one operation does not "
                       "reach", F->fn->name);
}

/* ---- site lists ----------------------------------------------------------- */

static void note_ext(struct ppc_sites *st, int at, struct func *callee,
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

static void note_str(struct ppc_sites *st, int at, int idx, enum reloc_kind k)
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

static void note_glob(struct ppc_sites *st, int at, struct global *g,
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

static void note_fn(struct ppc_sites *st, int at, struct func *target,
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

/* `lis rd, 0; addi rd, rd, 0`: an absolute address in two halves, the
 * relocations at the lis (HA) and at the addi (LO) -- each at the field,
 * the instruction's low half, +2. Returns the lis's offset. */
static int abs_pair(struct ppc_fn *F, int rd_)
{
    int at = F->t->len;
    ppc_lis(F->t, rd_, 0);
    ppc_imm(F->t, PPC_ADDI, rd_, rd_, 0);      /* refuses r0 */
    return at;
}

static void call_sym(struct ppc_fn *F, struct func *callee, int tail)
{
    int at = F->t->len;
    if (tail) ppc_b_rel(F->t);
    else      ppc_bl(F->t);
    note_ext(F->st, at, callee, tail);
}

static struct func **g_helpers;
static int g_nhelpers, g_caphelpers;

static void call_helper(struct ppc_fn *F, const char *name)
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

/* ---- soft float ------------------------------------------------------------ */

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

/* Put n vregs into the registers a call expects, all at once: the
 * register-to-register edges as one parallel move (SCR breaks a cycle),
 * then the loads, which only write. `half` (may be NULL) picks the low (0)
 * or high (1) word of a 64-bit value. */
static void set_args_half(struct ppc_fn *F, const int *dstreg,
                          const int *vreg, const int *half, int n)
{
    int pd[RA_MAXPOOL * 2], ps[RA_MAXPOOL * 2], npm = 0;

    for (int k = 0; k < n; k++)
        if (in_reg(F, vreg[k])) {
            pd[npm] = dstreg[k];
            ps[npm] = !half ? reg_of(F, vreg[k])
                    : !is_wide(F, vreg[k]) ? F->loc[vreg[k]]
                    : half[k] ? PHI(F->loc[vreg[k]]) : PLO(F->loc[vreg[k]]);
            npm++;
        }
    if (npm) {
        int od[RA_MAXPOOL * 4], os[RA_MAXPOOL * 4];
        int m = ra_parallel_move(pd, ps, npm, SCR, od, os,
                                 (int)(sizeof od / sizeof od[0]));
        if (m < 0)
            internal_error("ppc: an argument setup is not a well-formed "
                           "move");
        for (int k = 0; k < m; k++)
            ppc_mr(F->t, od[k], os[k]);
    }
    for (int k = 0; k < n; k++)
        if (!in_reg(F, vreg[k]))
            ld_sp(F, dstreg[k],
                  !half ? slot32(F, vreg[k])
                  : !is_wide(F, vreg[k]) ? sslot(F, vreg[k])
                  : sslot(F, vreg[k]) + (half[k] ? WHI : WLO), 4, 0);
}

static void set_args(struct ppc_fn *F, const int *dstreg, const int *vreg,
                     int n)
{
    set_args_half(F, dstreg, vreg, NULL, n);
}

/* Two 64-bit operands into r3:r4 and r5:r6 (vb < 0: only the first). */
static void args64x2(struct ppc_fn *F, int va, int vb)
{
    int d[4] = { PLO(3), PHI(3), PLO(5), PHI(5) };
    int v[4], h[4] = { 0, 1, 0, 1 };
    v[0] = v[1] = va;
    v[2] = v[3] = vb;
    set_args_half(F, d, v, h, vb >= 0 ? 4 : 2);
}

static void fp_args2(struct ppc_fn *F, const struct ir_ins *i)
{
    if (i->w == 8) {
        args64x2(F, i->a, i->b);
        return;
    }
    {
        int dstreg[2], vreg[2];
        dstreg[0] = PPC_R3; vreg[0] = i->a;
        dstreg[1] = PPC_R4; vreg[1] = i->b;
        set_args(F, dstreg, vreg, 2);
    }
}

static void fp_result(struct ppc_fn *F, int dst, int w)
{
    if (dst < 0)
        return;
    if (w == 8) wr64(F, dst, PLO(3), PHI(3));
    else        wr(F, dst, PPC_R3);
}

/* ---- comparisons -----------------------------------------------------------
 *
 * Every comparison sets cr0 and is read as one of six conditions. A 0/1
 * value is mfcr, then the bit rotated down -- LT is CR bit 0, GT 1, EQ 2 --
 * and an xori 1 for the inverse three. */
static int pred_cond(enum binop pred)
{
    switch (pred) {
    case B_EQ: return PPC_EQ;
    case B_NE: return PPC_NE;
    case B_LT: return PPC_LT;
    case B_GE: return PPC_GE;
    case B_GT: return PPC_GT;
    default:   return PPC_LE;
    }
}

static void cond_to_reg(struct ppc_fn *F, int cond, int dst)
{
    int bit = cond == PPC_LT || cond == PPC_GE ? 0
            : cond == PPC_GT || cond == PPC_LE ? 1 : 2;
    ppc_mfcr(F->t, dst);
    ppc_rlwinm(F->t, dst, dst, bit + 1, 31, 31);
    if (cond == PPC_GE || cond == PPC_LE || cond == PPC_NE)
        ppc_imm(F->t, PPC_XORI, dst, dst, 1);
}

/* cr0 = a compared with b (signed or not). */
static void cmp_regs(struct ppc_fn *F, int sign, int ra, int rb)
{
    ppc_cmp(F->t, 0, sign, ra, rb);
}

/* cr0 = a compared with constant k (sign-extended from 32 bits): cmpwi
 * takes a signed 16-bit one, cmplwi an unsigned; anything else is built
 * in `tmp`. */
static void cmp_imm(struct ppc_fn *F, int sign, int ra, long long k, int tmp)
{
    if (sign ? ppc_fits16(k, 1) : (k >= 0 && k <= 0xffff)) {
        ppc_cmpi(F->t, 0, sign, ra, k);
        return;
    }
    ppc_li(F->t, tmp, k);
    ppc_cmp(F->t, 0, sign, ra, tmp);
}

/* cr0 for IR_CMP i at 32 bits. */
static void cmp32(struct ppc_fn *F, const struct ir_ins *i)
{
    int ra_ = rdr(F, i->a, ACC);
    if (i->imm_b) {
        long long k = imm_val(i);
        if (!i->sign)
            k = (long long)(unsigned int)k;
        cmp_imm(F, i->sign, ra_, i->sign ? imm_val(i) : k, TMP);
        return;
    }
    cmp_regs(F, i->sign, ra_, rdr(F, i->b, TMP));
}

/* ---- 64-bit integers, in register pairs ----------------------------------- */

static void src64(struct ppc_fn *F, int v, int slo, int shi, int *lo, int *hi)
{
    if (in_reg(F, v)) {
        *lo = PLO(F->loc[v]);
        *hi = PHI(F->loc[v]);
        return;
    }
    rd64(F, v, slo, shi);
    *lo = slo;
    *hi = shi;
}

static void dst64(struct ppc_fn *F, int v, int *lo, int *hi)
{
    *lo = in_reg(F, v) ? PLO(F->loc[v]) : A_LO;
    *hi = in_reg(F, v) ? PHI(F->loc[v]) : A_HI;
}

/* cr0 for a 64-bit comparison: the high words decide unless they are
 * equal, and then the low words, compared unsigned -- into the same cr0,
 * so every predicate reads its bit off one result. */
static void cmp64(struct ppc_fn *F, const struct ir_ins *i, int sign)
{
    int al, ah, bl, bh, skip;
    src64(F, i->a, A_LO, A_HI, &al, &ah);
    if (i->imm_b) {
        operand_b64(F, i, B_LO, B_HI);
        bl = B_LO; bh = B_HI;
    } else {
        src64(F, i->b, B_LO, B_HI, &bl, &bh);
    }
    ppc_cmp(F->t, 0, sign, ah, bh);
    skip = br_place(F, PPC_NE);
    ppc_cmp(F->t, 0, 0, al, bl);
    br_land(F, skip);
}

/* d = s OP c for one 32-bit half (or a whole 32-bit value) of an
 * AND/OR/XOR with a constant: andi./andis. or an rlwinm mask for AND,
 * ori/oris (xori/xoris) -- both, for a constant with bits in both halves --
 * for OR and XOR, else c is built in r0. */
static int mask_bits(unsigned long m, int *mb, int *me)
{
    unsigned long n;
    int lo, hi;
    m &= 0xffffffffUL;
    if (m == 0)
        return 0;
    if ((m & 1) && (m & 0x80000000UL)) {
        n = ~m & 0xffffffffUL;          /* the run of zeros */
        if (n == 0) { *mb = 0; *me = 31; return 1; }
        for (lo = 0; !(n >> lo & 1); lo++) ;
        for (hi = lo; hi < 32 && (n >> hi & 1); hi++) ;
        if (hi < 32 && (n >> hi))
            return 0;
        /* zeros at bit positions lo..hi-1 (LSB numbering) */
        *mb = 31 - lo + 1;
        *me = 31 - hi;
        return 1;
    }
    for (lo = 0; !(m >> lo & 1); lo++) ;
    for (hi = lo; hi < 32 && (m >> hi & 1); hi++) ;
    if (hi < 32 && (m >> hi))
        return 0;
    *mb = 31 - (hi - 1);
    *me = 31 - lo;
    return 1;
}

static void logic_imm(struct ppc_fn *F, int op, int d, int s, unsigned long c)
{
    struct code *t = F->t;
    int mb, me;
    c &= 0xffffffffUL;
    if ((op == PPC_AND && c == 0xffffffffUL) || (op != PPC_AND && c == 0)) {
        if (d != s) ppc_mr(t, d, s);
        return;
    }
    if (op == PPC_AND && c == 0) {
        ppc_li(t, d, 0);
        return;
    }
    if (op == PPC_OR && c == 0xffffffffUL) {
        ppc_li(t, d, -1);
        return;
    }
    if (op == PPC_AND) {
        if (mask_bits(c, &mb, &me)) {
            ppc_rlwinm(t, d, s, 0, mb, me);
            return;
        }
        if (c <= 0xffff) {
            ppc_imm(t, PPC_ANDI_, d, s, (long long)c);
            return;
        }
        if ((c & 0xffff) == 0) {
            ppc_imm(t, PPC_ANDIS_, d, s, (long long)(c >> 16));
            return;
        }
        ppc_li(t, SCR, (long long)(int)(unsigned int)c);
        ppc_alu(t, PPC_AND, d, s, SCR);
        return;
    }
    /* OR, XOR: the low half and the high half, each when it has bits */
    if (c & 0xffff) {
        ppc_imm(t, op == PPC_OR ? PPC_ORI : PPC_XORI, d, s,
                (long long)(c & 0xffff));
        s = d;
    }
    if (c >> 16)
        ppc_imm(t, op == PPC_OR ? PPC_ORIS : PPC_XORIS, d, s,
                (long long)(c >> 16));
}

/* d = s + v for any 32-bit v: addi, addis, or both (no scratch). s is
 * never r0 (no allocator home is, and the scratches given are not). */
static void add_imm(struct ppc_fn *F, int d, int s, long long v)
{
    long long hi, lo;
    v = (long long)(int)(unsigned int)(unsigned long long)v;
    if (v == 0) {
        if (d != s) ppc_mr(F->t, d, s);
        return;
    }
    if (ppc_fits16(v, 1)) {
        ppc_imm(F->t, PPC_ADDI, d, s, v);
        return;
    }
    lo = (long long)(short)(v & 0xffff);
    hi = ((v - lo) >> 16) & 0xffff;
    if (hi > 32767) hi -= 65536;
    ppc_imm(F->t, PPC_ADDIS, d, s, hi);
    if (lo)
        ppc_imm(F->t, PPC_ADDI, d, d, lo);
}

/* A shift by a constant from (al, ah) into (dl, dh): the same pair or one
 * sharing no register with it. */
static void shift64_imm_to(struct ppc_fn *F, int left, int sign, long n,
                           int al, int ah, int dl, int dh)
{
    struct code *t = F->t;
    n &= 63;
    if (n == 0) {
        if (dl != al) ppc_mr(t, dl, al);
        if (dh != ah) ppc_mr(t, dh, ah);
        return;
    }
    if (n >= 32) {
        int k = (int)(n - 32);
        if (left) {
            ppc_slwi(t, dh, al, k);
            ppc_li(t, dl, 0);
        } else {
            if (sign) ppc_srawi(t, dl, ah, k);
            else      ppc_srwi(t, dl, ah, k);
            if (sign) ppc_srawi(t, dh, ah, 31);
            else      ppc_li(t, dh, 0);
        }
        return;
    }
    if (left) {
        /* dh = ah << n | al >> (32 - n); dl = al << n */
        ppc_slwi(t, dh, ah, (int)n);
        ppc_rlwimi(t, dh, al, (int)n, 32 - (int)n, 31);
        ppc_slwi(t, dl, al, (int)n);
    } else {
        /* dl = al >> n | ah << (32 - n); dh = ah >> n */
        ppc_srwi(t, dl, al, (int)n);
        ppc_rlwimi(t, dl, ah, 32 - (int)n, 0, (int)n - 1);
        if (sign) ppc_srawi(t, dh, ah, (int)n);
        else      ppc_srwi(t, dh, ah, (int)n);
    }
}

/* A shift of A_LO:A_HI by the count in B_LO. slw and srw read six bits
 * of the count: one of 32..63 shifts everything out, so the left and the
 * logical right shifts need no branch (clang's sequence); sraw fills
 * with the sign instead, so the arithmetic one tests the count. */
static void shift64_var(struct ppc_fn *F, int left, int sign)
{
    struct code *t = F->t;
    ppc_rlwinm(t, B_LO, B_LO, 0, 26, 31);                /* & 63 */
    ppc_imm(t, PPC_SUBFIC, SCR, B_LO, 32);              /* 32 - n */
    if (left) {
        ppc_alu(t, PPC_SLW, A_HI, A_HI, B_LO);
        ppc_alu(t, PPC_SRW, SCR, A_LO, SCR);
        ppc_alu(t, PPC_OR, A_HI, A_HI, SCR);
        ppc_imm(t, PPC_ADDI, SCR, B_LO, -32);
        ppc_alu(t, PPC_SLW, SCR, A_LO, SCR);
        ppc_alu(t, PPC_OR, A_HI, A_HI, SCR);
        ppc_alu(t, PPC_SLW, A_LO, A_LO, B_LO);
        return;
    }
    ppc_alu(t, PPC_SRW, A_LO, A_LO, B_LO);
    ppc_alu(t, PPC_SLW, SCR, A_HI, SCR);
    ppc_alu(t, PPC_OR, A_LO, A_LO, SCR);
    if (!sign) {
        ppc_imm(t, PPC_ADDI, SCR, B_LO, -32);
        ppc_alu(t, PPC_SRW, SCR, A_HI, SCR);
        ppc_alu(t, PPC_OR, A_LO, A_LO, SCR);
        ppc_alu(t, PPC_SRW, A_HI, A_HI, B_LO);
        return;
    }
    {
        int small;
        ppc_cmpi(t, 0, 1, B_LO, 32);
        small = br_place(F, PPC_LT);
        ppc_imm(t, PPC_ADDI, SCR, B_LO, -32);
        ppc_alu(t, PPC_SRAW, A_LO, A_HI, SCR);
        br_land(F, small);
        ppc_alu(t, PPC_SRAW, A_HI, A_HI, B_LO);
    }
}

static int gen_ins64(struct ppc_fn *F, int n)
{
    struct ir_func *fn = F->fn;
    struct ir_ins *i = &fn->ins[n];
    struct code *t = F->t;

    switch (i->op) {
    case IR_CONST: {
        int lo, hi;
        dst64(F, i->dst, &lo, &hi);
        ppc_li(t, lo, (long long)(i->imm & 0xffffffffL));
        ppc_li(t, hi, (long long)((i->imm >> 32) & 0xffffffffL));
        wr64(F, i->dst, lo, hi);
        return 1;
    }
    case IR_BITCAST:
    case IR_MOV:
        if (in_reg(F, i->dst)) {
            rd64(F, i->a, PLO(F->loc[i->dst]), PHI(F->loc[i->dst]));
        } else if (in_reg(F, i->a)) {
            wr64(F, i->dst, PLO(F->loc[i->a]), PHI(F->loc[i->a]));
        } else {
            rd64(F, i->a, A_LO, A_HI);
            wr64(F, i->dst, A_LO, A_HI);
        }
        return 1;
    case IR_ADD:
    case IR_SUB:
        rd64(F, i->a, A_LO, A_HI);
        operand_b64(F, i, B_LO, B_HI);
        if (i->op == IR_ADD) {
            ppc_alu(t, PPC_ADDC, A_LO, A_LO, B_LO);
            ppc_alu(t, PPC_ADDE, A_HI, A_HI, B_HI);
        } else {
            ppc_alu(t, PPC_SUBC, A_LO, A_LO, B_LO);
            ppc_alu(t, PPC_SUBE, A_HI, A_HI, B_HI);
        }
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    case IR_AND: case IR_OR: case IR_XOR: {
        int op = i->op == IR_AND ? PPC_AND : i->op == IR_OR ? PPC_OR : PPC_XOR;
        int al, ah, bl, bh, dl, dh;
        src64(F, i->a, A_LO, A_HI, &al, &ah);
        if (i->imm_b) {
            dst64(F, i->dst, &dl, &dh);
            logic_imm(F, op, dl, al, (unsigned long)i->imm);
            logic_imm(F, op, dh, ah, (unsigned long)i->imm >> 32);
            wr64(F, i->dst, dl, dh);
            return 1;
        }
        src64(F, i->b, B_LO, B_HI, &bl, &bh);
        dst64(F, i->dst, &dl, &dh);
        ppc_alu(t, op, dl, al, bl);
        ppc_alu(t, op, dh, ah, bh);
        wr64(F, i->dst, dl, dh);
        return 1;
    }
    case IR_MUL:
        /* (ah:al) * (bh:bl) keeping 64 bits: al*bl whole, and the cross
         * terms reach only the high word */
        rd64(F, i->a, A_LO, A_HI);
        operand_b64(F, i, B_LO, B_HI);
        ppc_alu(t, PPC_MULLW, A_HI, A_HI, B_LO);
        ppc_alu(t, PPC_MULHWU, SCR, A_LO, B_LO);
        ppc_alu(t, PPC_ADD, A_HI, A_HI, SCR);
        ppc_alu(t, PPC_MULLW, SCR, A_LO, B_HI);
        ppc_alu(t, PPC_ADD, A_HI, A_HI, SCR);
        ppc_alu(t, PPC_MULLW, A_LO, A_LO, B_LO);
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    case IR_MULW: {
        /* mulhw(u) and mullw: the two words of a 32 x 32 product, the
         * second into a register the first did not overwrite an operand
         * in -- through r0 when the pair holds both operands */
        int ra_ = rdr(F, i->a, B_LO), rb_ = rdr(F, i->b, B_HI), dl, dh;
        int hop = i->sign ? PPC_MULHW : PPC_MULHWU;
        dst64(F, i->dst, &dl, &dh);
        if (dh != ra_ && dh != rb_) {
            ppc_alu(t, hop, dh, ra_, rb_);
            ppc_alu(t, PPC_MULLW, dl, ra_, rb_);
        } else if (dl != ra_ && dl != rb_) {
            ppc_alu(t, PPC_MULLW, dl, ra_, rb_);
            ppc_alu(t, hop, dh, ra_, rb_);
        } else {
            ppc_alu(t, hop, SCR, ra_, rb_);
            ppc_alu(t, PPC_MULLW, dl, ra_, rb_);
            ppc_mr(t, dh, SCR);
        }
        wr64(F, i->dst, dl, dh);
        return 1;
    }
    case IR_NEG: {
        int al, ah, dl, dh;
        src64(F, i->a, A_LO, A_HI, &al, &ah);
        dst64(F, i->dst, &dl, &dh);
        ppc_imm(t, PPC_SUBFIC, dl, al, 0);
        ppc_un(t, PPC_SUBFZE, dh, ah);
        wr64(F, i->dst, dl, dh);
        return 1;
    }
    case IR_BNOT: {
        int al, ah, dl, dh;
        src64(F, i->a, A_LO, A_HI, &al, &ah);
        dst64(F, i->dst, &dl, &dh);
        ppc_alu(t, PPC_NOR, dl, al, al);
        ppc_alu(t, PPC_NOR, dh, ah, ah);
        wr64(F, i->dst, dl, dh);
        return 1;
    }
    case IR_SHL: case IR_SHR: {
        int sign = i->op == IR_SHR && i->sign;
        if (i->imm_b) {
            int al, ah, dl, dh;
            /* in place, or between pairs that share no register: A is
             * no pool pair, and the pool's pairs are whole */
            src64(F, i->a, A_LO, A_HI, &al, &ah);
            dst64(F, i->dst, &dl, &dh);
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
        rd(F, i->a, A_LO);
        if (i->size < 4)
            ext_reg(F, A_LO, A_LO, i->size, i->sign);
        if (i->sign) ppc_srawi(t, A_HI, A_LO, 31);
        else         ppc_li(t, A_HI, 0);
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    case IR_LDVAR:
        if (i->size == 8) {
            rd64(F, i->a, A_LO, A_HI);
        } else {
            if (in_reg(F, i->a))
                ext_reg(F, A_LO, reg_of(F, i->a), i->size, i->sign);
            else
                ld_sp(F, A_LO, var_slot(F, i->a, i->size), i->size, i->sign);
            if (i->sign) ppc_srawi(t, A_HI, A_LO, 31);
            else         ppc_li(t, A_HI, 0);
        }
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    case IR_STVAR:
        rd64(F, i->a, A_LO, A_HI);
        if (i->size == 8)
            wr64(F, i->dst, A_LO, A_HI);
        else if (in_reg(F, i->dst))
            ext_reg(F, reg_of(F, i->dst), A_LO, i->size, 1);
        else if (F->slot[i->dst] >= 0)
            st_sp(F, A_LO, var_slot(F, i->dst, i->size), i->size);
        return 1;
    case IR_LOAD: {
        int addr = rdr(F, i->a, ADDR);
        if (i->size == 8) {
            if (addr == A_HI || addr == A_LO) {
                ppc_mr(t, ADDR, addr);
                addr = ADDR;
            }
            ld_mem(F, A_HI, addr, i->memoff + WHI, 4, 0);
            ld_mem(F, A_LO, addr, i->memoff + WLO, 4, 0);
        } else {
            ld_mem(F, A_LO, addr, i->memoff, i->size, i->sign);
            if (i->sign) ppc_srawi(t, A_HI, A_LO, 31);
            else         ppc_li(t, A_HI, 0);
        }
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    }
    case IR_STORE: {
        int addr = rdr(F, i->a, ADDR), lo, hi;
        src64(F, i->b, A_LO, A_HI, &lo, &hi);
        if (i->size == 8) {
            st_mem(F, hi, addr, i->memoff + WHI, 4);
            st_mem(F, lo, addr, i->memoff + WLO, 4);
        } else {
            st_mem(F, lo, addr, i->memoff, i->size);
        }
        return 1;
    }
    case IR_SELECT: {
        /* dst = a ? b : c: c into A, then b over it when the condition
         * (either half of a 64-bit one) is nonzero */
        int bl, bh, skip;
        if (i->size == 8) {
            int al, ah;
            src64(F, i->a, B_LO, B_HI, &al, &ah);
            ppc_alu(t, PPC_OR, SCR, al, ah);
            ppc_cmpi(t, 0, 1, SCR, 0);
        } else {
            ppc_cmpi(t, 0, 1, rdr(F, i->a, B_LO), 0);
        }
        rd64(F, i->c, A_LO, A_HI);
        skip = br_place(F, PPC_EQ);
        src64(F, i->b, B_LO, B_HI, &bl, &bh);
        ppc_mr(t, A_LO, bl);
        ppc_mr(t, A_HI, bh);
        br_land(F, skip);
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    }
    default:
        return 0;
    }
}

/* ---- one call ------------------------------------------------------------ */

/* Can the call at n be a TAIL call: the frame torn down, then a `b`? When
 * nothing of this frame can still be needed, the IR_RET after it returns
 * exactly what the call returned, and every argument is a scalar in a
 * register (a stack argument or a struct copy would be in this frame). */
static int ppc_tail_ok(const struct ppc_fn *F, int n)
{
    const struct ir_func *fn = F->fn;
    const struct ir_ins *i = &fn->ins[n], *r;
    struct argplace pl;
    int gpr = 0;
    long stk = 0;

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
                          i->ret_tybytes > 4))
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
    for (int k = 0; k < i->nargs; k++) {
        place_arg(&i->argv[k], &gpr, &stk, &pl);
        if (pl.stk >= 0 || i->argv[k].is_struct)
            return 0;
    }
    return 1;
}

/* The epilogue's restores, and with `ret` the return: the callee-saved
 * registers, LR (through r12), the frame. Touches nothing but those, r12
 * and r1, so r3:r4 survive it. */
static void ppc_restore(struct ppc_fn *F, int ret)
{
    struct code *t = F->t;
    for (int k = 0; k < F->nsave; k++)
        ld_sp(F, F->used_callee[k], F->save_at + (long)k * 4, 4, 0);
    if (!F->leaf) {
        ld_sp(F, LRR, F->frame + 4, 4, 0);
        ppc_mtlr(t, LRR);
    }
    if (F->frame) {
        if (fits16(F->frame))
            ppc_imm(t, PPC_ADDI, PPC_SP, PPC_SP, F->frame);
        else
            ppc_load(t, PPC_SP, PPC_SP, 0, 4, 0);     /* the back chain */
    }
    if (ret)
        ppc_blr(t);
}

/* The `n` (1..8) bytes at base+off as a big-endian integer, right-
 * justified: into r alone (n <= 4), or into rhi:rlo. The shape a small
 * composite has in r3:r4. */
static void pack_word(struct ppc_fn *F, int r, int base, long off, long n)
{
    if (n == 4) {
        ld_mem(F, r, base, off, 4, 0);
        return;
    }
    if (n == 2) {
        ld_mem(F, r, base, off, 2, 0);
        return;
    }
    ppc_li(F->t, r, 0);
    for (long b = 0; b < n; b++) {
        ppc_slwi(F->t, r, r, 8);
        ld_mem(F, SCR, base, off + b, 1, 0);
        ppc_alu(F->t, PPC_OR, r, r, SCR);
    }
}

static void pack_small(struct ppc_fn *F, int base, long size, int rhi,
                       int rlo)
{
    if (size <= 4) {
        pack_word(F, rhi, base, 0, size);
        return;
    }
    pack_word(F, rhi, base, 0, size - 4);
    pack_word(F, rlo, base, size - 4, 4);
}

/* The other way: r3 (and r4) right-justified holding a small composite,
 * written to the frame at `at` as its bytes. r3, r4 are stored as the
 * words they are -- 8 bytes, the low end of which is the composite's
 * tail -- and the composite's bytes moved down to `at`. */
static void unpack_small(struct ppc_fn *F, long at, long size)
{
    long pad;
    st_sp(F, PPC_R3, at, 4);
    if (size > 4)
        st_sp(F, PPC_R4, at + 4, 4);
    pad = (size > 4 ? 8 : 4) - size;
    for (long b = 0; pad && b < size; b++) {
        ld_sp(F, SCR, at + pad + b, 1, 0);
        st_sp(F, SCR, at + b, 1);
    }
}

static void gen_call(struct ppc_fn *F, int n)
{
    struct ir_func *fn = F->fn;
    struct ir_ins *i = &fn->ins[n];
    struct code *t = F->t;
    struct argplace pl[MAX_PARAMS];
    long copy_off[MAX_PARAMS];
    int gpr = 0;
    long stk = 0, c = 0;
    int sret = call_sret(i);

    if (sret)
        gpr = 1;                          /* r3 holds the result's address */
    for (int k = 0; k < i->nargs; k++) {
        place_arg(&i->argv[k], &gpr, &stk, &pl[k]);
        copy_off[k] = -1;
        if (i->argv[k].is_struct) {
            c = (c + 15) & ~15L;
            copy_off[k] = F->argcopy_at + c;
            c += i->argv[k].size;
        }
    }

    /* The struct copies first -- the callee may write its argument, so it
     * gets the caller's own copy -- and then the stack words. Both need
     * scratches, and once r3-r10 are loaded none is left. */
    for (int k = 0; k < i->nargs; k++) {
        struct ir_arg *a = &i->argv[k];
        if (!a->is_struct)
            continue;
        rd(F, a->vreg, TMP);
        addr_sp(F, ADDR, copy_off[k]);
        copy_block(F, 1, a->size);
        if (pl[k].stk >= 0) {
            addr_sp(F, ACC, copy_off[k]);
            st_out(F, ACC, pl[k].stk, 4);
        }
    }
    for (int k = 0; k < i->nargs; k++) {
        struct ir_arg *a = &i->argv[k];
        if (pl[k].stk < 0 || a->is_struct)
            continue;
        if (a->size > 4) {
            int lo, hi;
            src64(F, a->vreg, A_LO, A_HI, &lo, &hi);
            st_out(F, hi, pl[k].stk + WHI, 4);
            st_out(F, lo, pl[k].stk + WLO, 4);
        } else {
            st_out(F, rdr(F, a->vreg, ACC), pl[k].stk, 4);
        }
    }
    /* An indirect call's target, before the arguments take r3-r10. */
    if (i->indirect)
        rd(F, i->a, TMP);
    /* The scalar register arguments, all at once. A 64-bit one in a pair
     * is two edges of the same move: its high word into the first. */
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
                sh_[ns_] = a->size > 4 ? 1 - q : 0;
                ns_++;
            }
        }
        if (ns_)
            set_args_half(F, sd_, sv_, sh_, ns_);
    }
    for (int k = 0; k < i->nargs; k++)
        if (pl[k].nreg && i->argv[k].is_struct)
            addr_sp(F, argreg(pl[k].reg), copy_off[k]);
    if (sret)
        addr_sp(F, PPC_R3, F->scratch_at + i->scratch);

    if (F->tail && F->tail[n]) {
        ppc_restore(F, 0);
        call_sym(F, i->callee, 1);
        if (n + 1 < fn->nins)
            F->skip_next = 1;
        return;
    }
    /* No floating-point arguments in FPRs: CR bit 6 clear, as a variadic
     * callee reads it. */
    if (i->call_varargs)
        ppc_crxor(t, 6, 6, 6);
    if (i->indirect) {
        ppc_mtctr(t, TMP);
        ppc_bctrl(t);
    } else {
        call_sym(F, i->callee, 0);
    }

    if (i->dst < 0)
        return;
    if (i->retsize) {
        long at = F->scratch_at + i->scratch;
        if (!sret)
            unpack_small(F, at, i->retsize);
        addr_sp(F, ACC, at);
        wr(F, i->dst, ACC);
    } else if (F->wide[i->dst]) {
        if (i->ret_tybytes > 4 || i->ret_tybytes == 0)
            wr64(F, i->dst, PLO(3), PHI(3));
        else
            wr64(F, i->dst, PPC_R3, PPC_R4);      /* a 32-bit result */
    } else {
        wr(F, i->dst, PPC_R3);
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

static void need_word_atomic(struct ppc_fn *F, const struct ir_ins *i)
{
    if (i->size == 1 || i->size == 2)   /* sub_* below: the word around it */
        return;
    if (i->size != 4)
        ppc_refuse(F, i, "an atomic wider than a register");
}

/* ---- one- and two-byte atomics ---------------------------------------
 *
 * lwarx/stwcx. are word-sized (Book E has no lbarx/lharx), so a narrow
 * atomic works on the aligned word around it, as GCC's and LLVM's do: a
 * lwarx/stwcx. loop that rewrites only its lane,
 *
 *   retry: lwarx   old, 0, aligned
 *          new = f(old)  in the lane
 *          new = old ^ ((new ^ old) & mask)
 *          stwcx.  new, 0, aligned
 *          bne-    retry
 *
 * atomic against the neighbouring bytes too: a store to any of them
 * between the lwarx and the stwcx. loses the reservation. Big-endian, the
 * lane of address a is bits 8*((a & 3) ^ (4 - size)) up. The scratches are
 * five (r0, r9-r12) and a compare-and-swap needs six values in its loop,
 * so the desired value waits in CTR; the shift is not kept at all, but
 * worked out again from the address after the loop. */
#define SUB_AL  ADDR        /* r10: the aligned word's address */
#define SUB_MK  B_HI        /* r9:  the lane's mask */
#define SUB_OLD ACC         /* r11: the word lwarx saw */
#define SUB_VAL TMP         /* r12: the operand, moved into its lane */

/* sh = the lane's shift, from the address a (in place when sh == a) */
static void sub_shift(struct ppc_fn *F, int sh, int a, int size)
{
    ppc_rlwinm(F->t, sh, a, 3, 27, 28);             /* (a & 3) * 8 */
    if (target_big_endian())
        ppc_imm(F->t, PPC_XORI, sh, sh, (4 - size) * 8);
}

/* SUB_AL, SUB_MK, and the shift in `sh`, for the address a */
static void sub_lane(struct ppc_fn *F, int a, int size, int sh)
{
    struct code *t = F->t;
    sub_shift(F, sh, a, size);
    ppc_rlwinm(t, SUB_AL, a, 0, 0, 29);             /* a & ~3 */
    ppc_li(t, SUB_MK, size == 1 ? 0xff : 0xffff);
    ppc_alu(t, PPC_SLW, SUB_MK, SUB_MK, sh);
}

/* reg = (src << sh) & mask */
static void sub_in(struct ppc_fn *F, int reg, int src, int sh)
{
    ppc_alu(F->t, PPC_SLW, reg, src, sh);
    ppc_alu(F->t, PPC_AND, reg, reg, SUB_MK);
}

/* reg, its lane already masked, shifted down to bit 0 and extended as
 * `sign` says; the shift comes from vreg a's address again, through
 * `scr` */
static void sub_out(struct ppc_fn *F, int reg, int a, int scr, int size,
                    int sign)
{
    struct code *t = F->t;
    sub_shift(F, scr, rdr(F, a, scr), size);
    ppc_alu(t, PPC_SRW, reg, reg, scr);
    if (sign)
        ppc_un(t, size == 1 ? PPC_EXTSB : PPC_EXTSH, reg, reg);
}

static void gen_ins(struct ppc_fn *F, int n)
{
    struct ir_func *fn = F->fn;
    struct ir_ins *i = &fn->ins[n];
    struct code *t = F->t;

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

    if (i->flt && (i->op == IR_ADD || i->op == IR_SUB || i->op == IR_MUL ||
                   i->op == IR_DIV || i->op == IR_MOD || i->op == IR_NEG ||
                   i->op == IR_CMP || i->op == IR_SQRT)) {
        const char *name = fp_binop_name(i->op, i->w);
        if (i->w != 4 && i->w != 8)
            ppc_refuse(F, i, "a floating-point value of this width");
        if (name) {
            if (i->imm_b)
                ppc_refuse(F, i, "a folded floating-point immediate");
            fp_args2(F, i);
            call_helper(F, name);
            fp_result(F, i->dst, i->w);
            return;
        }
        if (i->op == IR_NEG) {
            /* the sign bit, flipped: right for -0.0 and a NaN as well */
            if (i->w == 8) {
                rd64(F, i->a, A_LO, A_HI);
                ppc_imm(t, PPC_XORIS, A_HI, A_HI, 0x8000);
                wr64(F, i->dst, A_LO, A_HI);
            } else {
                rd(F, i->a, ACC);
                ppc_imm(t, PPC_XORIS, ACC, ACC, 0x8000);
                wr(F, i->dst, ACC);
            }
            return;
        }
        if (i->op == IR_CMP) {
            /* __ltdf2 and the rest answer with an int whose relation to
             * zero is the predicate's; unordered makes it false */
            fp_args2(F, i);
            call_helper(F, fp_cmp_name(i->pred, i->w));
            ppc_cmpi(t, 0, 1, PPC_R3, 0);
            cond_to_reg(F, pred_cond(i->pred), ACC);
            wr(F, i->dst, ACC);
            return;
        }
        if (i->op == IR_SQRT)
            ppc_refuse(F, i, "__builtin_sqrt (a libm routine here, not an "
                             "instruction)");
        ppc_refuse(F, i, "this floating-point operation");
    }

    if (i->w > 8)
        ppc_refuse(F, i, "a 128-bit value");
    if (i->op == IR_XCHG || i->op == IR_XADD || i->op == IR_ARMW ||
        i->op == IR_CAS || i->op == IR_CMPXCHG)
        need_word_atomic(F, i);
    /* Level 0 only (irgen), as GCC and clang: the frame address is r1
     * once the prologue has made the frame -- the back chain's word --
     * and the return address is LR as the function was entered with it,
     * from the save word at frame+4 (a function that asks saves LR, as
     * one that calls does). */
    if (i->op == IR_FRAMEADDR) {
        int d = i->dst >= 0 ? wreg(F, i->dst, ACC) : ACC;
        if (i->imm == 2)
            ld_sp(F, d, F->frame + 4, 4, 0);
        else
            addr_sp(F, d, 0);
        if (i->dst >= 0)
            wrote(F, i->dst, d);
        return;
    }

    /* The high word of a 64-bit value, shifted: one register. */
    if (i->op == IR_SHR && F->nshr && i->dst >= 0 && F->nshr[i->dst]) {
        int k = (int)i->imm - 32, d = wreg(F, i->dst, A_LO), hi;
        if (in_reg(F, i->a)) {
            hi = PHI(F->loc[i->a]);
        } else {
            ld_sp(F, A_HI, sslot(F, i->a) + WHI, 4, 0);
            hi = A_HI;
        }
        if (k) {
            if (i->sign) ppc_srawi(t, d, hi, k);
            else         ppc_srwi(t, d, hi, k);
        } else if (d != hi) {
            ppc_mr(t, d, hi);
        }
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
                    operand_b64(F, i, PLO(5), PHI(5));
                } else {
                    args64x2(F, i->a, i->b);
                }
                call_helper(F, i->op == IR_DIV
                            ? (i->sign ? "__divdi3" : "__udivdi3")
                            : (i->sign ? "__moddi3" : "__umoddi3"));
                wr64(F, i->dst, PLO(3), PHI(3));
                return;
            }
            if (i->op == IR_BSWAP) {
                /* each word reversed, and the words exchanged */
                int al, ah;
                src64(F, i->a, A_LO, A_HI, &al, &ah);
                ppc_rlwinm(t, B_HI, al, 8, 0, 31);
                ppc_rlwimi(t, B_HI, al, 24, 0, 7);
                ppc_rlwimi(t, B_HI, al, 24, 16, 23);
                ppc_rlwinm(t, B_LO, ah, 8, 0, 31);
                ppc_rlwimi(t, B_LO, ah, 24, 0, 7);
                ppc_rlwimi(t, B_LO, ah, 24, 16, 23);
                wr64(F, i->dst, B_LO, B_HI);
                return;
            }
            if (gen_ins64(F, n))
                return;
            ppc_refuse(F, i, "this operation at 64 bits");
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
        ppc_li(t, d, imm_val(i));
        wrote(F, i->dst, d);
        return;
    }
    case IR_BITCAST:
    case IR_MOV: {
        int src = rdr(F, i->a, ACC);
        int d = wreg(F, i->dst, ACC);
        if (src != d)
            ppc_mr(t, d, src);
        wrote(F, i->dst, d);
        return;
    }

    case IR_ADD: case IR_SUB: case IR_MUL:
    case IR_AND: case IR_OR:  case IR_XOR: {
        int op = i->op == IR_ADD ? PPC_ADD
               : i->op == IR_SUB ? PPC_SUB
               : i->op == IR_AND ? PPC_AND
               : i->op == IR_OR  ? PPC_OR
               : i->op == IR_XOR ? PPC_XOR
               : PPC_MULLW;
        int ra_ = rdr(F, i->a, ACC);
        int rd_ = wreg(F, i->dst, ACC);
        if (i->imm_b) {
            long long v = imm_val(i);
            if (i->op == IR_ADD || i->op == IR_SUB) {
                add_imm(F, rd_, ra_, i->op == IR_SUB ? -v : v);
                wrote(F, i->dst, rd_);
                return;
            }
            if (i->op == IR_MUL) {
                if (ppc_fits16(v, 1)) {
                    ppc_imm(t, PPC_MULLI, rd_, ra_, v);
                    wrote(F, i->dst, rd_);
                    return;
                }
            } else {
                logic_imm(F, op, rd_, ra_, (unsigned long)v);
                wrote(F, i->dst, rd_);
                return;
            }
        }
        {
            int rb_ = (i->imm_b || !in_reg(F, i->b)) ? TMP : reg_of(F, i->b);
            if (rb_ == TMP) operand_b(F, i, TMP);
            ppc_alu(t, op, rd_, ra_, rb_);
        }
        wrote(F, i->dst, rd_);
        return;
    }
    case IR_MULH: {
        /* the high word of a 32 x 32 product */
        int ra_ = rdr(F, i->a, ACC), rb_ = rdr(F, i->b, TMP);
        int d = wreg(F, i->dst, ACC);
        ppc_alu(t, i->sign ? PPC_MULHW : PPC_MULHWU, d, ra_, rb_);
        wrote(F, i->dst, d);
        return;
    }
    case IR_DIV: case IR_MOD: {
        int ra_ = rdr(F, i->a, ACC);
        int rb_ = (i->imm_b || !in_reg(F, i->b)) ? TMP : reg_of(F, i->b);
        int d;
        if (rb_ == TMP) operand_b(F, i, TMP);
        d = wreg(F, i->dst, ACC);
        if (i->op == IR_DIV) {
            ppc_alu(t, i->sign ? PPC_DIVW : PPC_DIVWU, d, ra_, rb_);
        } else {
            /* a - (a / b) * b */
            ppc_alu(t, i->sign ? PPC_DIVW : PPC_DIVWU, SCR, ra_, rb_);
            ppc_alu(t, PPC_MULLW, SCR, SCR, rb_);
            ppc_alu(t, PPC_SUB, d, ra_, SCR);
        }
        wrote(F, i->dst, d);
        return;
    }
    case IR_SHL: case IR_SHR: {
        int ra_ = rdr(F, i->a, ACC);
        int d;
        if (i->imm_b && i->imm >= 0 && i->imm < 32) {
            int k = (int)i->imm;
            d = wreg(F, i->dst, ACC);
            if (i->op == IR_SHL)  ppc_slwi(t, d, ra_, k);
            else if (i->sign)     ppc_srawi(t, d, ra_, k);
            else                  ppc_srwi(t, d, ra_, k);
        } else {
            int rb_ = (i->imm_b || !in_reg(F, i->b)) ? TMP : reg_of(F, i->b);
            if (rb_ == TMP) operand_b(F, i, TMP);
            d = wreg(F, i->dst, ACC);
            ppc_alu(t, i->op == IR_SHL ? PPC_SLW : i->sign ? PPC_SRAW : PPC_SRW,
                    d, ra_, rb_);
        }
        wrote(F, i->dst, d);
        return;
    }
    case IR_NEG: {
        int ra_ = rdr(F, i->a, ACC);
        int d = wreg(F, i->dst, ACC);
        ppc_un(t, PPC_NEG, d, ra_);
        wrote(F, i->dst, d);
        return;
    }
    case IR_BNOT: {
        int ra_ = rdr(F, i->a, ACC);
        int d = wreg(F, i->dst, ACC);
        ppc_alu(t, PPC_NOR, d, ra_, ra_);
        wrote(F, i->dst, d);
        return;
    }

    case IR_CMP: {
        struct ir_ins *nx = n + 1 < fn->nins ? &fn->ins[n + 1]
                                             : (struct ir_ins *)0;
        int fuse = nx && (nx->op == IR_BRZ || nx->op == IR_BRNZ) &&
                   nx->a == i->dst && F->usecnt && F->usecnt[i->dst] == 1;
        int cond = pred_cond(i->pred);
        if (i->w == 8)
            cmp64(F, i, i->sign);
        else
            cmp32(F, i);
        if (fuse) {
            branch_to(F, nx->op == IR_BRZ ? ppc_cond_invert(cond) : cond,
                      nx->label);
            F->skip_next = 1;
            return;
        }
        {
            int d = wreg(F, i->dst, ACC);
            cond_to_reg(F, cond, d);
            wrote(F, i->dst, d);
        }
        return;
    }

    case IR_SELECT: {
        /* dst = a ? b : c -- tested at the condition's width, `size` */
        int rb_, rc_, d, skip;
        if (i->size == 8) {
            int al, ah;
            src64(F, i->a, A_LO, A_HI, &al, &ah);
            ppc_alu(t, PPC_OR, SCR, al, ah);
            ppc_cmpi(t, 0, 1, SCR, 0);
        } else {
            ppc_cmpi(t, 0, 1, rdr(F, i->a, ACC), 0);
        }
        rb_ = rdr(F, i->b, TMP);
        rc_ = rdr(F, i->c, ACC);
        d = wreg(F, i->dst, ACC);
        if (d == rb_ && d != rc_) {
            /* b is already where the result goes: c over it when zero */
            skip = br_place(F, PPC_NE);
            ppc_mr(t, d, rc_);
            br_land(F, skip);
        } else {
            if (d != rc_)
                ppc_mr(t, d, rc_);
            if (d != rb_) {
                skip = br_place(F, PPC_EQ);
                ppc_mr(t, d, rb_);
                br_land(F, skip);
            }
        }
        wrote(F, i->dst, d);
        return;
    }

    case IR_BRZ: case IR_BRNZ: {
        if (i->w == 8) {
            int al, ah;
            src64(F, i->a, A_LO, A_HI, &al, &ah);
            ppc_alu(t, PPC_OR, SCR, al, ah);
            ppc_cmpi(t, 0, 1, SCR, 0);
        } else {
            ppc_cmpi(t, 0, 1, rdr(F, i->a, A_LO), 0);
        }
        branch_to(F, i->op == IR_BRZ ? PPC_EQ : PPC_NE, i->label);
        return;
    }

    case IR_LDVAR: {
        int d = wreg(F, i->dst, ACC);
        if (in_reg(F, i->a)) {
            if (ppc_ldvar_plain(i->size, i->sign, i->w)) {
                if (d != reg_of(F, i->a)) ppc_mr(t, d, reg_of(F, i->a));
            } else {
                ext_reg(F, d, reg_of(F, i->a), i->size, i->sign);
            }
        } else {
            ld_sp(F, d, var_slot(F, i->a, i->size), i->size, i->sign);
        }
        wrote(F, i->dst, d);
        return;
    }
    case IR_STVAR: {
        int src = rdr(F, i->a, ACC);
        if (in_reg(F, i->dst)) {
            if (i->size >= 4) {
                if (reg_of(F, i->dst) != src) ppc_mr(t, reg_of(F, i->dst), src);
            } else {
                ext_reg(F, reg_of(F, i->dst), src, i->size, 1);
            }
        } else {
            st_sp(F, src, var_slot(F, i->dst, i->size), i->size);
        }
        return;
    }
    case IR_LOAD: {
        int addr = rdr(F, i->a, ADDR);
        int d = wreg(F, i->dst, ACC);
        ld_mem(F, d, addr, i->memoff, i->size, i->sign);
        wrote(F, i->dst, d);
        return;
    }
    case IR_STORE: {
        int addr = rdr(F, i->a, ADDR);
        int val = rdr(F, i->b, ACC);
        st_mem(F, val, addr, i->memoff, i->size);
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
        addr_sp(F, d, obj_slot(F, i->a));
        wrote(F, i->dst, d);
        return;
    }
    /* An address is lis + addi, R_PPC_ADDR16_HA then _LO, each at its
     * instruction's low half. */
    case IR_STRADDR: {
        int d = wreg(F, i->dst, ACC);
        int at = abs_pair(F, d);
        note_str(F->st, at + 2, i->label, RK_PPC_ADDR16_HA);
        note_str(F->st, at + 6, i->label, RK_PPC_ADDR16_LO);
        wrote(F, i->dst, d);
        return;
    }
    case IR_GADDR: {
        int d = wreg(F, i->dst, ACC);
        int at = abs_pair(F, d);
        note_glob(F->st, at + 2, i->glob, RK_PPC_ADDR16_HA);
        note_glob(F->st, at + 6, i->glob, RK_PPC_ADDR16_LO);
        wrote(F, i->dst, d);
        return;
    }
    case IR_FADDR: {
        int d = wreg(F, i->dst, ACC);
        int at = abs_pair(F, d);
        note_fn(F->st, at + 2, i->callee, RK_PPC_ADDR16_HA);
        note_fn(F->st, at + 6, i->callee, RK_PPC_ADDR16_LO);
        wrote(F, i->dst, d);
        return;
    }

    case IR_MEMCPY: case IR_MEMZERO:
        rd(F, i->a, ADDR);
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
                if (fn_sret(fn)) {
                    /* through the caller's buffer, whose address the
                     * prologue kept; r3 holds it back */
                    rd(F, i->a, TMP);
                    ld_sp(F, ADDR, F->sret_slot, 4, 0);
                    copy_block(F, 1, fn->ret_abi.size);
                    ld_sp(F, PPC_R3, F->sret_slot, 4, 0);
                } else {
                    rd(F, i->a, ADDR);
                    pack_small(F, ADDR, fn->ret_abi.size, PPC_R3, PPC_R4);
                }
            } else if (F->wide[i->a] && fn->ret_abi.size <= 4) {
                rd(F, i->a, PPC_R3);           /* its low word */
            } else if (F->wide[i->a]) {
                rd64(F, i->a, PLO(3), PHI(3));
            } else {
                rd(F, i->a, PPC_R3);
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
        ppc_trap(t);
        return;
    case IR_FENCE:
        ppc_sync(t);
        return;

    case IR_BSWAP: {
        int ra_ = rdr(F, i->a, ACC);
        int d = wreg(F, i->dst, ACC);
        int w = d == ra_ ? TMP : d;
        if (i->size == 2) {
            ppc_rlwinm(t, w, ra_, 24, 24, 31);
            ppc_rlwimi(t, w, ra_, 8, 16, 23);
        } else {
            ppc_rlwinm(t, w, ra_, 8, 0, 31);
            ppc_rlwimi(t, w, ra_, 24, 0, 7);
            ppc_rlwimi(t, w, ra_, 24, 16, 23);
        }
        if (w != d)
            ppc_mr(t, d, w);
        wrote(F, i->dst, d);
        return;
    }

    case IR_VA_START:
        /* The record: the GPRs and stack the named parameters took, the
         * overflow area (the caller's 8(r1) on), the save area. */
        if (F->va_tag < 0)
            internal_error("ppc: %s: va_start without a record", fn->name);
        ppc_li(t, SCR, F->va_gpr);
        st_sp(F, SCR, F->va_tag, 1);
        ppc_li(t, SCR, 0);
        st_sp(F, SCR, F->va_tag + 1, 1);
        st_sp(F, SCR, F->va_tag + 2, 2);
        addr_sp(F, ACC, F->frame + 8 + F->va_stk);
        st_sp(F, ACC, F->va_tag + 4, 4);
        addr_sp(F, ACC, F->va_save);
        st_sp(F, ACC, F->va_tag + 8, 4);
        rd(F, i->a, ADDR);
        addr_sp(F, ACC, F->va_tag);
        ppc_store(t, ACC, ADDR, 0, 4);
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
            ppc_refuse(F, i, "a conversion of a 128-bit value");
        if (i->op == IR_I2F && src_w == 8 && i->a >= 0 && !F->wide[i->a]) {
            /* a 32-bit value asked for as 64: zero-extended */
            rd(F, i->a, PLO(3));
            ppc_li(t, PHI(3), 0);
        } else if (src_w == 8) {
            args64x2(F, i->a, -1);
        } else {
            rd(F, i->a, PPC_R3);
        }
        call_helper(F, cvt_name(i));
        if (i->dst >= 0) {
            if (F->wide[i->dst] && i->w <= 4)
                wr64(F, i->dst, PPC_R3, PPC_R4);
            else if (F->wide[i->dst])
                wr64(F, i->dst, PLO(3), PHI(3));
            else
                wr(F, i->dst, PPC_R3);
        }
        return;
    }

    case IR_ASM: {
        /* Extended asm, assembled in irgen (ppc/irgen.c irg_asm_ppc)
         * against the vocabulary in ppc/asm.c. This only places the
         * operands and splices the bytes -- the SPARC and Xtensa
         * lowering. To the allocator (ra_target.asm_in_reg) a value live
         * across an asm keeps out of the registers it may change
         * (ir_asm.clob); a callee-saved one among those is saved by the
         * prologue (gen_func), and a template that links makes this
         * function save LR. Each way the operands move as ONE parallel
         * move. No operand is ever in r0, r1, r2, r9-r13 or r31: the
         * scratch, the stack pointer, the EABI anchors and the frame base
         * under alloca. */
        struct ir_asm *ia = i->asm_ir;
        int vreg_[16], vdst[16], nval = 0;
        if (!ia)
            return;
        if (ia->cont) {
            int k = n - 1;
            while (k >= 0 && fn->ins[k].op == IR_ASM && fn->ins[k].asm_ir &&
                   fn->ins[k].asm_ir->cont)
                k--;
            if (k < 0 || fn->ins[k].op != IR_ASM)
                internal_error("ppc: %s: an asm's further output is not "
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
        /* r31 is the frame base of a function that calls alloca: every
         * slot below is addressed through it */
        if (F->fb == 31 && (ia->clob >> 31 & 1))
            ppc_refuse(F, i, "an asm that changes r31, the frame base of a "
                             "function that calls alloca");
        for (int k = 0; k < ia->nin; k++)
            if (!((ia->in[k].reg >= 3 && ia->in[k].reg <= 8) ||
                  (ia->in[k].reg >= 14 && ia->in[k].reg <= 30)))
                internal_error("ppc: %s: an asm operand in r%d", fn->name,
                               ia->in[k].reg);
        for (int k = 0; k < ia->nout; k++)
            if (!ia->out[k].mem && ia->out[k].size > 4)
                ppc_refuse(F, i, "an asm output wider than a register");
        {
            int naddr = 0;
            for (int k = 0; k < ia->nout; k++)
                naddr += !ia->out[k].val && !ia->out[k].mem;
            if (ia->scr < 0 && naddr > 0)
                ppc_refuse(F, i, "an asm with no scratch register left "
                                 "around it");
        }
        {
            int pd[40], ps[40], npm = 0;
            for (int k = 0; k < ia->nin && npm < 40; k++)
                if (in_reg(F, ia->in[k].temp)) {
                    pd[npm] = ia->in[k].reg;
                    ps[npm++] = reg_of(F, ia->in[k].temp);
                }
            for (int k = 0; k < ia->nout && npm < 40; k++)
                if (!ia->out[k].val &&
                    (ia->out[k].mem || ia->out[k].inout) &&
                    in_reg(F, ia->out[k].temp)) {
                    pd[npm] = ia->out[k].reg;
                    ps[npm++] = reg_of(F, ia->out[k].temp);
                }
            if (npm) {
                int od[80], os[80];
                int m = ra_parallel_move(pd, ps, npm, SCR, od, os,
                                         (int)(sizeof od / sizeof od[0]));
                if (m < 0)
                    ppc_refuse(F, i, "an asm whose operands cannot be moved "
                                     "into place");
                for (int k = 0; k < m; k++)
                    ppc_mr(t, od[k], os[k]);
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
                    ppc_load(t, o->reg, o->reg, 0, o->size, 0);
            }
        }
        for (int k = 0; k < ia->codelen; k++)
            code_byte(t, ia->code[k]);
        for (int k = 0; k < ia->nout; k++) {
            const struct ir_asm_op *o = &ia->out[k];
            if (o->mem || o->val)
                continue;
            rd(F, o->temp, ia->scr);
            ppc_store(t, o->reg, ia->scr, 0, o->size);
        }
        {
            int pd[16], ps[16], npm = 0;
            for (int k = 0; k < nval; k++) {
                if (vdst[k] < 0)
                    continue;
                if (in_reg(F, vdst[k])) {
                    pd[npm] = reg_of(F, vdst[k]);
                    ps[npm++] = vreg_[k];
                } else {
                    wr(F, vdst[k], vreg_[k]);
                }
            }
            if (npm) {
                int od[32], os[32];
                int m = ra_parallel_move(pd, ps, npm, SCR, od, os,
                                         (int)(sizeof od / sizeof od[0]));
                if (m < 0)
                    ppc_refuse(F, i, "an asm whose outputs cannot be moved "
                                     "into place");
                for (int k = 0; k < m; k++)
                    ppc_mr(t, od[k], os[k]);
            }
        }
        return;
    }

    /* ---- atomics: lwarx/stwcx. loops, bracketed by sync --------------- */
    case IR_XCHG: case IR_XADD: case IR_ARMW: {
        int addr, val, dst, top, again;
        need_word_atomic(F, i);
        if (i->size == 1 || i->size == 2) {
            addr = rdr(F, i->a, ADDR);
            sub_lane(F, addr, i->size, SUB_OLD);
            sub_in(F, SUB_VAL, rdr(F, i->b, TMP), SUB_OLD);
            ppc_sync(t);
            top = t->len;
            ppc_lwarx(t, SUB_OLD, 0, SUB_AL);
            if (i->op == IR_XCHG) {
                ppc_mr(t, SCR, SUB_VAL);
            } else if (i->op == IR_XADD) {
                ppc_alu(t, PPC_ADD, SCR, SUB_OLD, SUB_VAL);
            } else {
                switch ((int)i->imm) {
                case '&': ppc_alu(t, PPC_AND, SCR, SUB_OLD, SUB_VAL); break;
                case '|': ppc_alu(t, PPC_OR, SCR, SUB_OLD, SUB_VAL); break;
                case '^': ppc_alu(t, PPC_XOR, SCR, SUB_OLD, SUB_VAL); break;
                default:  ppc_alu(t, PPC_NAND, SCR, SUB_OLD, SUB_VAL); break;
                }
            }
            /* only the lane changes: old ^ ((new ^ old) & mask) */
            ppc_alu(t, PPC_XOR, SCR, SCR, SUB_OLD);
            ppc_alu(t, PPC_AND, SCR, SCR, SUB_MK);
            ppc_alu(t, PPC_XOR, SCR, SCR, SUB_OLD);
            ppc_stwcx(t, SCR, 0, SUB_AL);
            again = br_place(F, PPC_NE);
            br_back(F, again, top);
            ppc_sync(t);
            ppc_alu(t, PPC_AND, SUB_OLD, SUB_OLD, SUB_MK);
            sub_out(F, SUB_OLD, i->a, SUB_VAL, i->size, i->sign);
            wrote(F, i->dst, SUB_OLD);
            return;
        }
        addr = rdr(F, i->a, ADDR);
        val = rdr(F, i->b, TMP);
        dst = wreg(F, i->dst, ACC);
        if (dst == addr || dst == val)
            dst = ACC;
        ppc_sync(t);
        top = t->len;
        ppc_lwarx(t, dst, 0, addr);
        if (i->op == IR_XCHG) {
            ppc_mr(t, SCR, val);
        } else if (i->op == IR_XADD) {
            ppc_alu(t, PPC_ADD, SCR, dst, val);
        } else {
            switch ((int)i->imm) {
            case '&': ppc_alu(t, PPC_AND, SCR, dst, val); break;
            case '|': ppc_alu(t, PPC_OR, SCR, dst, val); break;
            case '^': ppc_alu(t, PPC_XOR, SCR, dst, val); break;
            default:  ppc_alu(t, PPC_NAND, SCR, dst, val); break;
            }
        }
        ppc_stwcx(t, SCR, 0, addr);
        again = br_place(F, PPC_NE);
        br_back(F, again, top);
        ppc_sync(t);
        wrote(F, i->dst, dst);
        return;
    }
    case IR_CAS: case IR_CMPXCHG: {
        /*   sync
         *   retry: lwarx  seen, 0, addr
         *          cmpw   seen, expected
         *          bne    out
         *          stwcx. desired, 0, addr
         *          bne    retry
         *   out:   sync                                              */
        int addr, exp, des, seen = ACC, out_br, top, again;
        need_word_atomic(F, i);
        if (i->size == 1 || i->size == 2) {
            /*   sync
             *   retry: lwarx  w, 0, aligned
             *          xor    w, w, expected          (both in the lane)
             *          and    r0, w, mask ; cmpwi r0, 0 ; bne out
             *          mfctr  r0 ; xor w, w, r0       (old ^ exp ^ des)
             *          stwcx. w, 0, aligned ; bne- retry
             *          li     r0, 0
             *   out:   sync
             * r0 is then the seen lane xor the expected one, 0 on a match:
             * so the lane seen is r0 ^ expected either way. */
            addr = rdr(F, i->a, ADDR);
            sub_lane(F, addr, i->size, SUB_OLD);
            sub_in(F, SUB_VAL, rdr(F, i->c, TMP), SUB_OLD);
            ppc_mtctr(t, SUB_VAL);                  /* desired, in lane */
            if (i->op == IR_CAS) {
                sub_in(F, SUB_VAL, rdr(F, i->b, TMP), SUB_OLD);
            } else {
                int p = rdr(F, i->b, TMP);
                ppc_load(t, SUB_VAL, p, 0, i->size, 0);
                sub_in(F, SUB_VAL, SUB_VAL, SUB_OLD);
            }
            ppc_sync(t);
            top = t->len;
            ppc_lwarx(t, SUB_OLD, 0, SUB_AL);
            ppc_alu(t, PPC_XOR, SUB_OLD, SUB_OLD, SUB_VAL);
            ppc_alu(t, PPC_AND, SCR, SUB_OLD, SUB_MK);
            ppc_cmpi(t, 0, 1, SCR, 0);
            out_br = br_place(F, PPC_NE);
            ppc_mfspr(t, SCR, 9);                   /* mfctr */
            ppc_alu(t, PPC_XOR, SUB_OLD, SUB_OLD, SCR);
            ppc_stwcx(t, SUB_OLD, 0, SUB_AL);
            again = br_place(F, PPC_NE);
            br_back(F, again, top);
            ppc_li(t, SCR, 0);
            br_land(F, out_br);
            ppc_sync(t);
            if (i->op == IR_CMPXCHG) {
                ppc_cmpi(t, 0, 1, SCR, 0);
                cond_to_reg(F, PPC_EQ, SUB_MK);     /* the flag */
            }
            ppc_alu(t, PPC_XOR, SUB_OLD, SCR, SUB_VAL);
            sub_out(F, SUB_OLD, i->a, SUB_VAL, i->size,
                    i->op == IR_CAS && i->sign);
            if (i->op == IR_CAS) {
                wr(F, i->dst, SUB_OLD);
            } else {
                int p = rdr(F, i->b, ADDR);
                ppc_store(t, SUB_OLD, p, 0, i->size);
                wr(F, i->dst, SUB_MK);
            }
            return;
        }
        addr = rdr(F, i->a, ADDR);
        if (i->op == IR_CAS) {
            exp = rdr(F, i->b, TMP);
        } else {
            int p = rdr(F, i->b, TMP);
            ppc_load(t, SCR, p, 0, 4, 0);
            exp = SCR;
        }
        des = rdr(F, i->c, B_HI);
        ppc_sync(t);
        top = t->len;
        ppc_lwarx(t, seen, 0, addr);
        ppc_cmp(t, 0, 1, seen, exp);
        out_br = br_place(F, PPC_NE);
        ppc_stwcx(t, des, 0, addr);
        again = br_place(F, PPC_NE);
        br_back(F, again, top);
        br_land(F, out_br);
        ppc_sync(t);
        if (i->op == IR_CAS) {
            wr(F, i->dst, seen);
        } else {
            int p = rdr(F, i->b, TMP);
            ppc_store(t, seen, p, 0, 4);
            ppc_cmp(t, 0, 1, seen, exp);
            cond_to_reg(F, PPC_EQ, B_HI);
            wr(F, i->dst, B_HI);
        }
        return;
    }
    case IR_ALLOCA: {
        /* A fresh 16-aligned block: r1 moves down by the size rounded to
         * 16, the back chain going with it (stwux), and the block sits
         * above the header and outgoing area, which move down with r1.
         * The frame is addressed from r31. */
        int d = wreg(F, i->dst, ACC);
        rd(F, i->a, TMP);
        ppc_imm(t, PPC_ADDI, TMP, TMP, 15);
        ppc_rlwinm(t, TMP, TMP, 0, 0, 27);
        ppc_un(t, PPC_NEG, TMP, TMP);
        ppc_load(t, SCR, PPC_SP, 0, 4, 0);
        ppc_stwux(t, SCR, PPC_SP, TMP);
        addr_at(F, d, PPC_SP, F->out_bytes);
        wrote(F, i->dst, d);
        return;
    }
    case IR_SPSAVE: {
        int d = wreg(F, i->dst, ACC);
        ppc_mr(t, d, PPC_SP);
        wrote(F, i->dst, d);
        return;
    }
    case IR_SPRESTORE:
        /* r1 back, its back chain with it */
        rd(F, i->a, TMP);
        ppc_load(t, SCR, PPC_SP, 0, 4, 0);
        ppc_mr(t, PPC_SP, TMP);
        ppc_store(t, SCR, PPC_SP, 0, 4);
        return;
    case IR_SWITCH: {
        /* A jump table in .text after its dispatch, of 32-bit offsets
         * from the instruction `bcl` returns to -- no relocation, and the
         * same code wherever it is linked:
         *
         *     cmplwi rI, n ; bge default        (li + cmplw past 65535)
         *     bl 1f                            (LR, saved by the prologue)
         *  1: mflr r12 ; slwi r10, rI, 2 ; lwzx r10, r12, r10 (+ tab-1b)
         *     add r10, r10, r12 ; mtctr r10 ; bctr
         *   tab: .long L0-1b, L1-1b, ...
         *
         * A function with a switch is never a leaf, so its LR is saved. */
        int nc = fn->jt[i->jt].n;
        int ri = rdr(F, i->a, ACC);
        int anchor, tab;
        cmp_imm(F, 0, ri, nc, TMP);
        branch_to(F, PPC_GE, i->label);
        ppc_w(t, ppc_enc_b(4, 1));
        anchor = t->len;
        ppc_mflr(t, TMP);
        ppc_slwi(t, ADDR, ri, 2);
        ppc_alu(t, PPC_ADD, ADDR, ADDR, TMP);
        tab = anchor + 4 * 7;
        ppc_load(t, ADDR, ADDR, tab - anchor, 4, 0);
        ppc_alu(t, PPC_ADD, ADDR, ADDR, TMP);
        ppc_mtctr(t, ADDR);
        ppc_bctr(t);
        if (t->len != tab)
            internal_error("ppc: %s: the jump table is not where its load "
                           "says", fn->name);
        for (int k = 0; k < nc; k++) {
            want_label(F, t->len, fn->jt[i->jt].labels[k], FX_TAB);
            F->fix[F->nfix - 1].base = anchor;
            ppc_w(t, 0);
        }
        code_mark_data(t, tab, t->len);
        return;
    }
    case IR_LABELADDR: {
        if (cg_label_mark(i))       /* static data's marker: no code */
            return;
        /* &&label: the function's own address as IR_FADDR takes it,
         * lis/addi (@ha/@l), plus the label's offset in it -- the addend
         * set once the function is laid out. Absolute like every other
         * address here, and it needs no LR (a bcl would). */
        int d = wreg(F, i->dst, ACC);
        int at = abs_pair(F, d), s0 = F->st->nf;
        note_fn(F->st, at + 2, fn->src, RK_PPC_ADDR16_HA);
        note_fn(F->st, at + 6, fn->src, RK_PPC_ADDR16_LO);
        want_label(F, at, i->label, FX_ADDR);
        F->fix[F->nfix - 1].base = s0;
        wrote(F, i->dst, d);
        return;
    }
    case IR_IGOTO:
        ppc_mtctr(t, rdr(F, i->a, ACC));
        ppc_bctr(t);
        return;
    default:
        ppc_refuse(F, i, "this operation");
    }
}

/* Copy `size` bytes from [TMP] to [ADDR] (copy) or zero them (!copy), a
 * word at a time through r0 (misaligned words are the hardware's), then a
 * byte tail. Straight-line up to 128 bytes, a loop beyond (B_HI the end).
 * TMP and ADDR are scratch and may be moved. */
static void copy_block(struct ppc_fn *F, int copy, long size)
{
    struct code *t = F->t;
    long k, body = size & ~3L;
    if (!copy)
        ppc_li(t, SCR, 0);
    if (size > 128) {
        int top, again;
        ppc_li(t, B_HI, body);
        ppc_alu(t, PPC_ADD, B_HI, B_HI, ADDR);
        top = t->len;
        if (copy) {
            ppc_load(t, SCR, TMP, 0, 4, 0);
            ppc_imm(t, PPC_ADDI, TMP, TMP, 4);
        }
        ppc_store(t, SCR, ADDR, 0, 4);
        ppc_imm(t, PPC_ADDI, ADDR, ADDR, 4);
        ppc_cmp(t, 0, 0, ADDR, B_HI);
        again = br_place(F, PPC_NE);
        br_back(F, again, top);
        k = 0;
        size -= body;
    } else {
        for (k = 0; k + 4 <= size; k += 4) {
            if (copy)
                ppc_load(t, SCR, TMP, (int)k, 4, 0);
            ppc_store(t, SCR, ADDR, (int)k, 4);
        }
    }
    for (; k < size; k++) {
        if (copy) ppc_load(t, SCR, TMP, (int)k, 1, 0);
        ppc_store(t, SCR, ADDR, (int)k, 1);
    }
}

/* A by-reference parameter's copy into its slot, in the prologue, from
 * the pointer in `src` (an argument register, or r11): through r0 and r12
 * alone, so the other arguments in r3-r10 are untouched. */
static void param_copy(struct ppc_fn *F, int src, long dst_off, long size)
{
    struct code *t = F->t;
    long k, body = size & ~3L;
    if (dst_off + size <= 32767 && size <= 128) {
        for (k = 0; k + 4 <= size; k += 4) {
            ppc_load(t, SCR, src, (int)k, 4, 0);
            ppc_store(t, SCR, F->fb, (int)(dst_off + k), 4);
        }
        for (; k < size; k++) {
            ppc_load(t, SCR, src, (int)k, 1, 0);
            ppc_store(t, SCR, F->fb, (int)(dst_off + k), 1);
        }
        return;
    }
    /* a counted loop: r11 the source, r12 the destination, CTR the words */
    if (src != 11)
        ppc_mr(t, 11, src);
    addr_sp(F, 12, dst_off);
    if (body) {
        int top;
        ppc_li(t, SCR, body / 4);
        ppc_mtctr(t, SCR);
        top = t->len;
        ppc_load(t, SCR, 11, 0, 4, 0);
        ppc_store(t, SCR, 12, 0, 4);
        ppc_imm(t, PPC_ADDI, 11, 11, 4);
        ppc_imm(t, PPC_ADDI, 12, 12, 4);
        ppc_w(t, ppc_enc_bdnz(top - t->len));
    }
    for (k = 0; k < size - body; k++) {
        ppc_load(t, SCR, 11, (int)k, 1, 0);
        ppc_store(t, SCR, 12, (int)k, 1);
    }
}

/* ---- register pairs ------------------------------------------------------ */
static const struct ra_target PPC_PAIR_RA = {
    ppc_pair_pool_for, ppc_callee_saved, ppc_ldvar_plain,
    1, 1, 1,
    ppc_op_calls_helper,
    0,
    ppc_pair_hints,
    NULL, NULL,
    1,
    NULL, NULL,
    1,
    0,
    0
};

static void ppc_pair_hints(const struct ir_func *fn, int *hint)
{
    int gpr = fn_sret(fn) ? 1 : 0;
    long stk = 0;
    struct argplace pl;
    for (int p = 0; fn->src && p < fn->nparams && p < fn->nvregs; p++) {
        const struct ir_arg *a = &fn->param_abi[p];
        place_arg(a, &gpr, &stk, &pl);
        if (a->size == 8 && pl.nreg == 2 && !a->is_struct)
            hint[p] = argreg(pl.reg);
    }
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        if (i->op == IR_RET && i->a >= 0 && i->a < fn->nvregs &&
            !fn->ret_abi.is_struct && fn->ret_abi.size == 8)
            hint[i->a] = PPC_R3;
        if (i->op != IR_CALL && ppc_op_calls_helper(i) && i->w == 8) {
            if (i->a >= 0 && i->a < fn->nvregs && hint[i->a] < 0)
                hint[i->a] = PPC_R3;
            if (!i->imm_b && i->b >= 0 && i->b < fn->nvregs &&
                hint[i->b] < 0)
                hint[i->b] = PPC_R5;
            if (i->dst >= 0 && i->dst < fn->nvregs && hint[i->dst] < 0)
                hint[i->dst] = PPC_R3;
        }
        if (i->op != IR_CALL)
            continue;
        if (!i->retsize && i->dst >= 0 && i->dst < fn->nvregs && i->w == 8)
            hint[i->dst] = PPC_R3;
        gpr = call_sret(i) ? 1 : 0;
        stk = 0;
        for (int k = 0; k < i->nargs; k++) {
            const struct ir_arg *a = &i->argv[k];
            place_arg(a, &gpr, &stk, &pl);
            if (a->size == 8 && pl.nreg == 2 && !a->is_struct &&
                a->vreg >= 0 && a->vreg < fn->nvregs)
                hint[a->vreg] = argreg(pl.reg);
        }
    }
}

static struct ra_range *g_ppc_res;
static int g_ppc_nres, g_ppc_capres;
static void reserve_pairs(struct ir_func *fn, const int *loc)
{
    int nv = fn->nvregs;
    int *first = xmalloc((size_t)(nv ? nv : 1) * sizeof *first);
    int *last = xmalloc((size_t)(nv ? nv : 1) * sizeof *last);
    ra_live_ranges(fn, first, last);
    g_ppc_nres = 0;
    for (int v = 0; v < nv; v++) {
        if (loc[v] < 0 || first[v] < 0) continue;
        if (g_ppc_nres + 2 > g_ppc_capres) {
            g_ppc_capres = g_ppc_capres ? g_ppc_capres * 2 : 16;
            g_ppc_res = xrealloc(g_ppc_res,
                                 (size_t)g_ppc_capres * sizeof *g_ppc_res);
        }
        for (int h = 0; h < 2; h++) {
            g_ppc_res[g_ppc_nres].reg = loc[v] + h;
            g_ppc_res[g_ppc_nres].first = first[v];
            g_ppc_res[g_ppc_nres].last = last[v];
            g_ppc_res[g_ppc_nres].born = 0;
            g_ppc_nres++;
        }
    }
    ra_reserve(g_ppc_res, g_ppc_nres);
    free(first); free(last);
}

static int *pair_alloc(struct ir_func *fn, struct ppc_fn *F, const char *pin)
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
            int gpr = call_sret(i) ? 1 : 0;
            long stk = 0;
            struct argplace pl;
            for (int k = 0; k < i->nargs; k++) {
                const struct ir_arg *a = &i->argv[k];
                place_arg(a, &gpr, &stk, &pl);
                if (a->size > 4 && !a->is_struct && pl.nreg != 2 &&
                    a->vreg >= 0 && a->vreg < nv)
                    x[a->vreg] = 1;
            }
        }
    }
    F->npair = 0;
    if (!any) {
        free(x);
        return NULL;
    }
    loc = ra_allocate(fn, &PPC_PAIR_RA, NULL, x, used, &nused);
    free(x);
    reserve_pairs(fn, loc);
    for (int k = 0; k < nused && k < PPC_NPAIRS; k++)
        F->pair_used[F->npair++] = used[k];
    return loc;
}

/* ---- one function --------------------------------------------------------- */

static void gen_func(struct ir_func *fn, struct code *t, struct ppc_sites *st,
                     int want_debug)
{
    struct func *f = fn->src;
    struct ppc_fn F;
    int i;

    memset(&F, 0, sizeof F);
    F.fn = fn; F.t = t; F.st = st;
    F.want_debug = want_debug;
    F.wide = wide_map(fn);
    F.nshr = ra_narrow_hishift(fn);
    for (int v = 0; v < fn->nvregs; v++)
        if (F.nshr[v]) F.wide[v] = 0;
    F.fb = PPC_SP;
    if (g_ppc_regalloc) {
        char *pin = want_debug ? ra_debug_pin_vars(fn) : (char *)0;
        int *pair;
        g_ppc_taken = 0;
        pair = g_ppc_pairs ? pair_alloc(fn, &F, pin) : NULL;
        F.loc = ra_allocate(fn, &PPC_RATGT, F.wide, pin, F.used_callee,
                            &F.nsave);
        if (pair) {
            for (int v = 0; v < fn->nvregs; v++)
                if (pair[v] >= 0) F.loc[v] = pair[v];
            for (int k = 0; k < F.npair; k++) {
                int r = F.pair_used[k], seen0 = 0, seen1 = 0;
                for (int j = 0; j < F.nsave; j++) {
                    seen0 |= F.used_callee[j] == r;
                    seen1 |= F.used_callee[j] == r + 1;
                }
                if (!seen0 && ppc_callee_saved(r))
                    F.used_callee[F.nsave++] = r;
                if (!seen1 && ppc_callee_saved(r + 1))
                    F.used_callee[F.nsave++] = r + 1;
            }
            free(pair);
        }
        free(pin);
        if (fn->nvregs) {
            F.usecnt = xmalloc((size_t)fn->nvregs * sizeof *F.usecnt);
            ra_count_vreg_uses(fn, F.usecnt);
        }
        {
            const char *lim = getenv("EMBCC_PPC_RA_MAX");
            if (lim) {
                int nl = atoi(lim);
                for (int v = nl; v < fn->nvregs; v++)
                    F.loc[v] = -1;
            }
        }
    }
    if (fn->has_alloca) {
        int seen = 0;
        for (int j = 0; j < F.nsave; j++)
            seen |= F.used_callee[j] == 31;
        if (!seen)
            F.used_callee[F.nsave++] = 31;
    }
    /* A callee-saved register an asm changes -- one it names, clobbers or
     * puts an operand in -- is this function's to preserve, at every
     * optimisation level (the allocator only knows the ones it gave). */
    for (i = 0; i < fn->nins; i++) {
        const struct ir_asm *ia = fn->ins[i].op == IR_ASM ? fn->ins[i].asm_ir
                                                          : NULL;
        if (!ia || ia->cont)
            continue;
        for (int r = 14; r <= 31; r++) {
            int seen = 0;
            if (!(ia->clob >> r & 1))
                continue;
            for (int j = 0; j < F.nsave; j++)
                seen |= F.used_callee[j] == r;
            if (!seen)
                F.used_callee[F.nsave++] = r;
        }
    }
    F.tail = NULL;
    if (g_ppc_regalloc && !want_debug)
        for (i = 0; i < fn->nins; i++)
            if (ppc_tail_ok(&F, i)) {
                if (!F.tail)
                    F.tail = xcalloc((size_t)fn->nins, 1);
                F.tail[i] = 1;
            }
    F.leaf = 1;
    for (i = 0; i < fn->nins; i++)
        if ((fn->ins[i].op == IR_CALL && !(F.tail && F.tail[i])) ||
            fn->ins[i].op == IR_SWITCH ||
            ppc_op_calls_helper(&fn->ins[i]) ||
            /* __builtin_return_address reads LR's save word */
            (fn->ins[i].op == IR_FRAMEADDR && fn->ins[i].imm == 2) ||
            /* an asm that calls changes LR */
            (fn->ins[i].op == IR_ASM && fn->ins[i].asm_ir &&
             fn->ins[i].asm_ir->calls))
            F.leaf = 0;
    layout(&F);

    F.label_off = xmalloc((size_t)(fn->nlabels + 1) * sizeof *F.label_off);
    for (i = 0; i <= fn->nlabels; i++)
        F.label_off[i] = -1;

    f->code_align = 4;
    if (want_debug) {
        int nv = fn->nvars ? fn->nvars : 1;
        fn->var_off = xmalloc((size_t)nv * sizeof *fn->var_off);
        for (int v = 0; v < fn->nvars; v++)
            fn->var_off[v] = F.slot[v] < 0 ? (int)F.slot[v]
                                           : (int)obj_slot(&F, v);
    }
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
    F.fb = PPC_SP;
    F.longb = longb;
    F.nlongb = nlongb;
    f->code_off = t->len;

    /* The prologue: the frame and its back chain in one stwu, LR into the
     * caller's frame through r12, the callee-saved registers. */
    if (F.frame) {
        if (fits16(-F.frame)) {
            ppc_stwu(t, PPC_SP, PPC_SP, (int)-F.frame);
        } else {
            ppc_li(t, SCR, -F.frame);
            ppc_stwux(t, PPC_SP, PPC_SP, SCR);
        }
    }
    if (!F.leaf) {
        ppc_mflr(t, LRR);
        st_sp(&F, LRR, F.frame + 4, 4);
    }
    for (i = 0; i < F.nsave; i++)
        st_sp(&F, F.used_callee[i], F.save_at + (long)i * 4, 4);
    if (fn->has_alloca) {
        ppc_mr(t, 31, PPC_SP);
        F.fb = 31;
    }
    if (fn->is_varargs)
        for (int k = 0; k < PPC_NARGREG; k++)
            st_sp(&F, argreg(k), F.va_save + 4L * k, 4);

    /* The parameters. By-reference composites are copied into their
     * slots first, through r0 and r12 alone; then each register one is an
     * edge of a parallel move, and each stack one a load after it. */
    {
        struct argplace pl;
        int gpr = 0;
        long stk = 0;
        long base = F.frame + 8;      /* the caller's stack arguments */
        int pmv_dst[RA_MAXPOOL * 2], pmv_src[RA_MAXPOOL * 2], npmv = 0;
        int pstk_reg[RA_MAXPOOL * 2]; long pstk_off[RA_MAXPOOL * 2];
        int npstk = 0;
        if (F.sret_slot >= 0) {
            st_sp(&F, argreg(0), F.sret_slot, 4);
            gpr = 1;
        }
        for (i = 0; i < fn->nparams; i++) {
            struct ir_arg *a = &fn->param_abi[i];
            place_arg(a, &gpr, &stk, &pl);
            if (a->is_struct) {
                if (F.slot[i] < 0)
                    continue;
                if (pl.nreg) {
                    param_copy(&F, argreg(pl.reg), sslot(&F, i), a->size);
                } else {
                    ld_sp(&F, 11, base + pl.stk, 4, 0);
                    param_copy(&F, 11, sslot(&F, i), a->size);
                }
                continue;
            }
            if (a->size > 4) {
                if (in_reg(&F, i)) {
                    for (int q = 0; q < 2; q++) {
                        /* word q is the high one */
                        int dr = q == 0 ? PHI(F.loc[i]) : PLO(F.loc[i]);
                        if (pl.nreg) {
                            pmv_dst[npmv] = dr;
                            pmv_src[npmv] = argreg(pl.reg + q);
                            npmv++;
                        } else {
                            pstk_reg[npstk] = dr;
                            pstk_off[npstk] = base + pl.stk + 4L * q;
                            npstk++;
                        }
                    }
                } else if (F.slot[i] >= 0) {
                    if (pl.nreg) {
                        st_sp(&F, argreg(pl.reg), sslot(&F, i) + WHI, 4);
                        st_sp(&F, argreg(pl.reg + 1), sslot(&F, i) + WLO, 4);
                    } else {
                        ld_sp(&F, 12, base + pl.stk, 4, 0);
                        st_sp(&F, 12, sslot(&F, i), 4);
                        ld_sp(&F, 12, base + pl.stk + 4, 4, 0);
                        st_sp(&F, 12, sslot(&F, i) + 4, 4);
                    }
                }
                continue;
            }
            if (pl.nreg && in_reg(&F, i)) {
                pmv_dst[npmv] = reg_of(&F, i);
                pmv_src[npmv] = argreg(pl.reg);
                npmv++;
            } else if (pl.nreg) {
                if (F.slot[i] >= 0)
                    st_sp(&F, argreg(pl.reg), slot32(&F, i), 4);
            } else if (in_reg(&F, i)) {
                pstk_reg[npstk] = reg_of(&F, i);
                pstk_off[npstk] = base + pl.stk;
                npstk++;
            } else if (F.slot[i] >= 0) {
                ld_sp(&F, 12, base + pl.stk, 4, 0);
                st_sp(&F, 12, slot32(&F, i), 4);
            }
        }
        if (npmv) {
            int od[RA_MAXPOOL * 4], os[RA_MAXPOOL * 4];
            int m = ra_parallel_move(pmv_dst, pmv_src, npmv, SCR, od, os,
                                     (int)(sizeof od / sizeof od[0]));
            if (m < 0)
                internal_error("ppc: %s: the prologue's parameter "
                               "placement is not a well-formed move",
                               fn->name);
            for (int k = 0; k < m; k++)
                ppc_mr(t, od[k], os[k]);
        }
        for (int k = 0; k < npstk; k++)
            ld_sp(&F, pstk_reg[k], pstk_off[k], 4, 0);
        F.va_gpr = gpr;
        F.va_stk = stk;
    }

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
        F.label_off[fn->nlabels] = t->len;
        for (i = 0; tail_end && i < F.nfix; i++)
            if (F.fix[i].label == fn->nlabels)
                tail_end = 0;
        if (!tail_end) {
            if (fn->has_alloca) {
                /* every VLA at once: r1 back to the frame base; the
                 * restores then address from r1, since r31 is one */
                ppc_mr(t, PPC_SP, 31);
                F.fb = PPC_SP;
            }
            ppc_restore(&F, 1);
        }
    }

    for (i = 0; i < F.nfix; i++) {
        int target = F.label_off[F.fix[i].label];
        if (target < 0)
            internal_error("ppc: label %d of %s was never placed",
                           F.fix[i].label, fn->name);
        if (F.fix[i].kind == FX_TAB) {
            ppc_wrw(t, F.fix[i].at,
                    (unsigned long)(target - F.fix[i].base) & 0xffffffffUL);
            continue;
        }
        if (F.fix[i].kind == FX_ADDR) {
            F.st->f[F.fix[i].base].addend = target - f->code_off;
            F.st->f[F.fix[i].base + 1].addend = target - f->code_off;
            continue;
        }
        if (!ppc_patch_branch(t, F.fix[i].at, target)) {
            if ((ppc_rdw(t, F.fix[i].at) >> 26) == 18)
                ppc_refuse(&F, NULL, "a branch beyond +-32 MiB");
            if (nlongb < F.nfix) {
                longb = xrealloc(longb, (size_t)F.nfix);
                memset(longb + nlongb, 0, (size_t)(F.nfix - nlongb));
                nlongb = F.nfix;
            }
            if (longb[i])
                internal_error("ppc: %s: a long branch was patched as "
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
    cg_note_labels(fn, F.label_off);
    free(F.label_off);
    free(F.fix);
    free(F.wide);
    free(F.nshr);
    free(F.loc);
}

/* With the allocator on, a function is generated with the pair pass and
 * without it, and the shorter is kept (MIPS's and RV32's arrangement). */
static void gen_func_best(struct ir_func *fn, struct code *t,
                          struct ppc_sites *st, int want_debug)
{
    int at = t->len, next = st->next, nstr = st->nstr, ng = st->ng,
        nf = st->nf, with;

    if (g_ppc_regalloc && !want_debug && !getenv("EMBCC_NO_MEMOFF")) {
        char *w = wide_map(fn);
        ra_fold_memoff(fn, -32768, 32767 - 8, 4, 4, w, 0, 0);
        free(w);
    }
    g_ppc_pairs = 1;
    if (!g_ppc_regalloc || want_debug || getenv("EMBCC_PPC_PAIRS")) {
        if (getenv("EMBCC_PPC_PAIRS"))
            g_ppc_pairs = atoi(getenv("EMBCC_PPC_PAIRS"));
        gen_func(fn, t, st, want_debug);
        g_ppc_pairs = 1;
        return;
    }
    gen_func(fn, t, st, want_debug);
    with = t->len - at;
    t->len = at; st->next = next; st->nstr = nstr; st->ng = ng; st->nf = nf;
    g_ppc_pairs = 0;
    gen_func(fn, t, st, want_debug);
    if (t->len - at > with) {
        t->len = at; st->next = next; st->nstr = nstr; st->ng = ng;
        st->nf = nf;
        g_ppc_pairs = 1;
        gen_func(fn, t, st, want_debug);
    }
    g_ppc_pairs = 1;
}

void codegen_unit_ppc(struct ir_unit *iu, struct code *text,
                      struct extcall **ext, int *next,
                      struct strsite **strs, int *nstrs,
                      struct gsite **gs, int *ngs,
                      struct fsite **fs, int *nfs, int want_debug,
                      int optimize, int no_sse, int regalloc)
{
    struct ppc_sites st;

    (void)optimize; (void)no_sse;
    g_ppc_regalloc = regalloc;
    memset(&st, 0, sizeof st);
    for (int n = 0; n < iu->nfuncs; n++)
        gen_func_best(&iu->funcs[n], text, &st, want_debug);
    cg_resolve_strsites(iu, st.str, st.nstr);
    *ext = st.ext;   *next = st.next;
    *strs = st.str;  *nstrs = st.nstr;
    *gs = st.g;      *ngs = st.ng;
    *fs = st.f;      *nfs = st.nf;
}
