/* IR → AArch64, AAPCS64 (ARCHITECTURE §4; myos/docs/ARM64.md for the OS
 * side of the contract).
 *
 * Deliberately naive, exactly as the x86-64 backend was at the same stage:
 * every vreg lives in a stack slot and every operation goes through x9
 * (with x10 for a second operand). Correct-and-slow first — the residency
 * cache, slot coalescing and the register allocator on the x86 side are all
 * later work, and none of them changes what this file must get right.
 *
 * Two things differ from the x86 backend by necessity rather than taste:
 *
 *  - Slots are addressed [sp, #off] with a NON-NEGATIVE off, not [rbp-N].
 *    The scaled 12-bit unsigned-offset load reaches 32 KiB from sp where
 *    the signed form reaches only ±256 from x29, so sp-relative addressing
 *    is what keeps ordinary frames to one instruction per access.
 *
 *  - Argument placement is recomputed here to AAPCS64 rather than read
 *    from ir_arg's on_stack/stk_off, which irgen fills in with the SysV
 *    classification. The two ABIs disagree (8 integer argument registers
 *    against 6, composites by value on the stack rather than by MEMORY
 *    class), and honouring the wrong one is a silent miscompile.
 *
 * Anything not lowered fails loudly (THE RULE): an IR op this file has no
 * case for is a diag_fatal naming it, never a quiet miscompile.
 */
#include "../backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emit.h"
#include "../../driver/util.h"

/* -mgeneral-regs-only / -mno-sse: the FP registers are off limits. */
static int g_no_fp;

/* The register frame slots are addressed from: sp, except in a function
 * whose sp moves at run time (a VLA's IR_ALLOCA), where the prologue pins
 * the post-prologue sp in callee-saved x19 and every slot goes through it.
 * Only outgoing stack arguments are always at the live sp. */
#define A64_FBREG 19
static int g_fb = A64_SP;
#define FB g_fb

/* long double (binary128): the vregs holding one (codegen.h
 * cg_wide_vregs) — each gets a 16-aligned 16-byte slot. */
static char *g_a64_wide;

/* ---- site accumulation ---------------------------------------------- */

struct a64_callsite {
    int patch_off;          /* offset of the bl word in .text */
    struct func *target;
};

struct a64_sites {
    struct a64_callsite *call;
    int ncall, capcall;
    struct extcall *ext;
    int next, capext;
    struct strsite *str;
    int nstr, capstr;
    struct gsite *g;
    int ng, capg;
    struct fsite *f;
    int nf, capf;
};

#define PUSH(arr, n, cap, item)                                          \
    do {                                                                 \
        if ((n) == (cap)) {                                              \
            (cap) = (cap) ? (cap) * 2 : 16;                              \
            (arr) = xrealloc((arr), (size_t)(cap) * sizeof *(arr));      \
        }                                                                \
        (arr)[(n)++] = (item);                                           \
    } while (0)

/* ---- AAPCS64 argument placement -------------------------------------- */

/* AAPCS64 §5.9.5: a Homogeneous Floating-point Aggregate is a struct, union
 * or array whose members are, recursively, all the same floating type — one
 * to four of them, with no padding. It travels in v registers, one member per
 * register, which no SysV class can describe. Returns the member count (0:
 * not an HFA) and the member size in *esz (4 float, 8 double). */
/* AAPCS64's homogeneous-float-aggregate test and the by-reference rule now
 * live with the types (src/sema/type.c), because irgen has to ask the same
 * questions while the types still exist -- an IR that carries the ANSWER
 * instead of the type is a step toward a self-contained EmbIR (§9.1). */
#define a64_hfa(t, esz)  ty_hfa((t), (esz))
#define a64_byref(t)     ty_aapcs64_byref(t)

/* Where one argument goes. The same classifier places a call's arguments and
 * a function's parameters, so the two sides cannot disagree. */
enum a64_where { AP_X, AP_V, AP_STACK };
struct a64_argplan {
    enum a64_where where;
    int reg, nreg;          /* AP_X / AP_V: first register, count */
    int esz;                /* AP_V: bytes per register (a scalar, or an HFA
                             * member: 4 in s, 8 in d) */
    int byref;              /* B.3: `where` places a pointer to a copy */
    int is_struct;          /* the value is an aggregate (by its address) */
    long stk_off;           /* AP_STACK: offset in the argument area */
    long copy_off;          /* byref, caller side: the copy's offset in the
                             * frame's byref area */
    int size;               /* the value's size (the aggregate's, if byref) */
};

/* The registers and the stack walked so far (AAPCS64's NGRN, NSRN, NSAA). */
struct a64_cursor {
    int ngrn, nsrn;
    long nsaa;
    long byref_bytes;       /* caller side: copy space this call needs */
};

static void to_stack(struct a64_cursor *cu, struct a64_argplan *p, long size,
                     int align)
{
    long al = align > 8 ? 16 : 8;
    cu->nsaa = (cu->nsaa + al - 1) & ~(al - 1);
    p->where = AP_STACK;
    p->stk_off = cu->nsaa;
    cu->nsaa += (size + 7) & ~7L;           /* every stack slot is >= 8 */
}

/* AAPCS64 §6.8.2 stages B and C, for the types EmbCC has. No back-filling:
 * once a register file is declared spent (C.3 for v registers, C.11 for x
 * registers), later small arguments go to the stack too. */
/* The placer works from what irgen recorded about the type, not from the
 * type (§9.1) -- so the IR carries the ABI facts and the AST is not
 * consulted during code generation. */
static void a64_place_info(const struct ir_arg *a, struct a64_cursor *cu,
                           struct a64_argplan *p)
{
    memset(p, 0, sizeof *p);
    p->size = a->size;
    p->is_struct = a->is_struct;

    if (a->is_float) {                                  /* C.1 / C.5 */
        if (cu->nsrn < 8) {
            p->where = AP_V; p->reg = cu->nsrn++; p->nreg = 1; p->esz = p->size;
        } else if (p->size == 16) {
            to_stack(cu, p, 16, 16);                    /* a long double */
        } else {
            to_stack(cu, p, 8, 8);
        }
        return;
    }
    int esz = a->hfa_size, n = a->hfa_n;
    if (n) {                                            /* C.2 - C.4 */
        if (cu->nsrn + n <= 8) {
            p->where = AP_V; p->reg = cu->nsrn; p->nreg = n; p->esz = esz;
            cu->nsrn += n;
        } else {
            cu->nsrn = 8;
            to_stack(cu, p, p->size, a->align);
        }
        return;
    }
    if (a->byref) {                                     /* B.3: a pointer */
        p->byref = 1;
        cu->byref_bytes = (cu->byref_bytes + 15) & ~15L;
        p->copy_off = cu->byref_bytes;
        cu->byref_bytes += p->size;
        if (cu->ngrn < 8) {
            p->where = AP_X; p->reg = cu->ngrn++; p->nreg = 1;
        } else {
            to_stack(cu, p, 8, 8);
        }
        return;
    }
    if (a->is_int128) {                                 /* C.8, C.9 */
        /* 16-aligned: an even-numbered pair of x registers */
        cu->ngrn = (cu->ngrn + 1) & ~1;
        if (cu->ngrn + 2 <= 8) {
            p->where = AP_X; p->reg = cu->ngrn; p->nreg = 2;
            cu->ngrn += 2;
        } else {
            cu->ngrn = 8;
            to_stack(cu, p, 16, 16);
        }
        return;
    }
    int nslot = p->is_struct ? (p->size + 7) / 8 : 1;   /* C.9 / C.10 */
    if (cu->ngrn + nslot <= 8) {
        p->where = AP_X; p->reg = cu->ngrn; p->nreg = nslot;
        cu->ngrn += nslot;
        return;
    }
    cu->ngrn = 8;                                       /* C.11 */
    to_stack(cu, p, p->is_struct ? p->size : 8, a->align);
}

/* Argument k of a call or function whose argument 0 may be the indirect-
 * result pointer (sret_first: the C++ return slot of a class that is not
 * trivially copyable), which AAPCS64 passes in x8 whatever the result's
 * size — taking none of x0..x7. */
static void a64_place_arg(const struct ir_arg *a, int k, int sret_first,
                          struct a64_cursor *cu, struct a64_argplan *p)
{
    if (sret_first && k == 0) {
        memset(p, 0, sizeof *p);
        p->size = 8;
        p->where = AP_X;
        p->reg = 8;
        p->nreg = 1;
        return;
    }
    a64_place_info(a, cu, p);
}

/* ---- frame layout ---------------------------------------------------- */

/* Byte offsets from sp after the prologue, low to high: the outgoing
 * argument area; the byref copy area (reused by each call); the
 * struct-return scratch; the hidden return pointer; for a variadic function
 * its register save areas and va_list record; the variables; the
 * temporaries. */
struct a64_frame {
    int size;               /* 16-aligned */
    long outgoing, byref, scratch;
    long sret;              /* -1, or the slot that keeps x8 */
    long gr_save, vr_save, va_tag;   /* -1 unless variadic (vr_save also -1
                                      * under -mgeneral-regs-only) */
    long fb_save;           /* -1, or where the caller's x19 is kept */
};

static long *layout_frame(struct ir_func *fn, struct a64_frame *fr)
{
    struct func *f = fn->src;
    long *disp = xmalloc((size_t)(fn->nvregs ? fn->nvregs : 1) * sizeof *disp);

    /* The widest stack-argument list and byref-copy need of any call,
     * measured with AAPCS64 — irgen's outgoing_bytes is SysV's. */
    long outgoing = 0, byref = 0;
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        if (i->op != IR_CALL)
            continue;
        struct a64_cursor cu = { 0, 0, 0, 0 };
        struct a64_argplan pl;
        for (int k = 0; k < i->nargs; k++)
            a64_place_arg(&i->argv[k], k, i->sret_first, &cu, &pl);
        if (cu.nsaa > outgoing) outgoing = cu.nsaa;
        if (cu.byref_bytes > byref) byref = cu.byref_bytes;
    }
    long running = (outgoing + 15) & ~15L;
    fr->outgoing = running;
    fr->byref = running;
    running += (byref + 15) & ~15L;

    fr->scratch = running;
    running += fn->scratch_bytes;

    fr->sret = -1;
    if (a64_byref(f->ret_ty)) {
        running = (running + 7) & ~7L;
        fr->sret = running;
        running += 8;
    }

    fr->fb_save = -1;
    if (fn->has_alloca) {
        running = (running + 7) & ~7L;
        fr->fb_save = running;
        running += 8;
    }

    fr->gr_save = fr->vr_save = fr->va_tag = -1;
    if (f->is_varargs) {
        running = (running + 15) & ~15L;
        fr->gr_save = running;              /* x0..x7 */
        running += 64;
        if (!g_no_fp) {
            fr->vr_save = running;          /* q0..q7, 16-aligned */
            running += 128;
        }
        fr->va_tag = running;               /* the 32-byte va_list record */
        running += 32;
    }

    for (int v = 0; v < fn->nvars; v++) {
        int al = f->var_aligns ? f->var_aligns[v] : 0;
        int tal = ty_align(f->var_tys[v]);
        if (tal > al) al = tal;
        if (al > 16)
            diag_fatal(f->file, f->line,
                       "a local in '%s' needs %d-byte alignment, exceeding "
                       "the 16-byte stack alignment EmbCC can guarantee",
                       f->name, al);
        if (al > 1)
            running = (running + al - 1) & ~((long)al - 1);
        disp[v] = running;
        running += (ty_size(f->var_tys[v]) + 7) & ~7;
    }

    running = (running + 7) & ~7L;
    for (int t = fn->nvars; t < fn->nvregs; t++) {
        if (g_a64_wide && g_a64_wide[t]) {
            running = (running + 15) & ~15L;
            disp[t] = running;
            running += 16;
            continue;
        }
        disp[t] = running;
        running += 8;
    }

    fr->size = (int)((running + 15) & ~15L);
    return disp;
}

/* ---- helpers --------------------------------------------------------- */

static void align16(struct code *t)
{
    while (t->len & 15)
        a64_word(t, 0xD503201FUL);       /* nop */
}

/* Load vreg's slot into `reg`, extending per size/sign into a w-wide value. */
static void ld_slot(struct code *t, const long *sd, int vreg, int reg,
                    int size, int sign, int w)
{
    a64_ldr(t, reg, FB, sd[vreg], size, sign, w);
}

/* Store `reg`'s low `size` bytes into vreg's slot. */
static void st_slot(struct code *t, const long *sd, int vreg, int reg,
                    int size)
{
    a64_str(t, reg, FB, sd[vreg], size);
}

/* Materialise operand b into A64_TMP, whether it is a vreg or a folded
 * immediate (the optimizer's imm_b). */
static void operand_b(struct code *t, const long *sd, struct ir_ins *i)
{
    if (i->imm_b)
        a64_mov_imm(t, A64_TMP, i->imm, i->w);
    else
        ld_slot(t, sd, i->b, A64_TMP, 8, 0, 8);
}

/* dst = src + off, where src may be sp. */
static void addr_of(struct code *t, int dst, int base, long off)
{
    if (!a64_add_imm(t, dst, base, off, 8)) {
        a64_mov_imm(t, A64_SCR, off, 8);
        if (base == A64_SP)
            a64_word(t, 0x8B2063E0UL | ((unsigned long)A64_SCR << 16) |
                        (unsigned long)dst);      /* add dst, sp, scr */
        else
            a64_alu_reg(t, '+', dst, base, A64_SCR, 8);
    }
}

/* Copy `size` bytes from [src] to [dst]. Unrolled: struct copies in real
 * code are small, and an unrolled copy needs no spare register for a
 * counter. A very large aggregate therefore costs a long instruction run
 * — a bulk-copy loop is a later optimisation, not a correctness gap. */
static void emit_copy(struct code *t, int dst, int src, int size)
{
    int off = 0;
    while (off < size) {
        int chunk = size - off >= 8 ? 8 : size - off >= 4 ? 4
                  : size - off >= 2 ? 2 : 1;
        a64_ldr(t, A64_ACC, src, off, chunk, 0, 8);
        a64_str(t, A64_ACC, dst, off, chunk);
        off += chunk;
    }
}

static void emit_zero(struct code *t, int dst, int size)
{
    int off = 0;
    while (off < size) {
        int chunk = size - off >= 8 ? 8 : size - off >= 4 ? 4
                  : size - off >= 2 ? 2 : 1;
        a64_str(t, A64_ZR, dst, off, chunk);
        off += chunk;
    }
}

/* A floating-point binary operation: both operands come from their slots
 * into the FP scratch pair, and the result goes back to the destination
 * slot at the operation's own width. */
static void fbin(struct code *t, const long *sd, struct ir_ins *i, int op)
{
    a64_fldr(t, A64_FACC, FB, sd[i->a], i->w);
    a64_fldr(t, A64_FTMP, FB, sd[i->b], i->w);
    a64_falu(t, op, A64_FACC, A64_FACC, A64_FTMP, i->w);
    a64_fstr(t, A64_FACC, FB, sd[i->dst], i->w);
}

/* The condition code for an IR comparison predicate. */
static int cond_for(enum binop pred, int sign)
{
    switch (pred) {
    case B_EQ: return A64_EQ;
    case B_NE: return A64_NE;
    case B_LT: return sign ? A64_LT : A64_CC;   /* cc == lo */
    case B_LE: return sign ? A64_LE : A64_LS;
    case B_GT: return sign ? A64_GT : A64_HI;
    case B_GE: return sign ? A64_GE : A64_CS;   /* cs == hs */
    default:   return A64_AL;
    }
}

/* Branch fixups within one function. */
/* kind: 26-bit branch, 19-bit conditional branch, or a 21-bit adr. */
enum a64_fixkind { FIX_B26, FIX_B19, FIX_ADR };
struct a64_fix { int at; int label; enum a64_fixkind kind; };

/* ---- one function ---------------------------------------------------- */

/* ---- long double: IEEE binary128 through libgcc ----
 *
 * AArch64 has no quad-precision arithmetic, so (like gcc) every operation
 * is a call to libgcc's soft-float routines, with values passed and
 * returned in q registers. A call mid-instruction is safe in this backend:
 * nothing lives in a register across IR instructions (every value is in its
 * slot), x30 was saved by the prologue, and the frame base (sp or x19)
 * is preserved by the callee. Moves are plain 16-byte copies. */

/* One undefined symbol per helper, shared by every call site in the unit. */
static struct func *a64_helper(const char *name)
{
    static struct func *made[32];
    static int nmade;
    for (int k = 0; k < nmade; k++)
        if (strcmp(made[k]->name, name) == 0)
            return made[k];
    if (nmade == (int)(sizeof made / sizeof made[0]))
        diag_fatal(NULL, 0, "internal: too many libgcc helpers");
    struct func *h = xcalloc(1, sizeof *h);
    h->name = name;
    made[nmade++] = h;
    return h;
}

static void call_helper(struct code *t, struct a64_sites *st, const char *name)
{
    struct extcall ec;
    ec.patch_off = a64_bl(t);
    ec.callee = a64_helper(name);
    PUSH(st->ext, st->next, st->capext, ec);
}

static int a64_ld_ins(const struct ir_ins *i)
{
    switch (i->op) {
    case IR_LOAD: case IR_LDVAR: case IR_STORE: case IR_STVAR:
        return i->size == 16;
    case IR_MOV:
        return g_a64_wide && i->a >= 0 && g_a64_wide[i->a];
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_NEG:
    case IR_CMP:
        return i->flt && i->w == 16;
    case IR_I2F:
        return i->w == 16;
    case IR_F2I:
        return i->size == 16;
    case IR_F2F:
        return i->size == 16 || i->w == 16;
    default:
        return 0;
    }
}

static void gen_a64_ld(struct code *t, const long *sd, struct ir_ins *i,
                       struct a64_sites *st)
{
    switch (i->op) {
    case IR_LOAD:
        ld_slot(t, sd, i->a, A64_TMP, 8, 0, 8);          /* the address */
        addr_of(t, A64_ADDR, FB, sd[i->dst]);
        emit_copy(t, A64_ADDR, A64_TMP, 16);
        break;
    case IR_STORE:
        ld_slot(t, sd, i->a, A64_ADDR, 8, 0, 8);
        addr_of(t, A64_TMP, FB, sd[i->b]);
        emit_copy(t, A64_ADDR, A64_TMP, 16);
        break;
    case IR_LDVAR: case IR_STVAR: case IR_MOV:
        addr_of(t, A64_ADDR, FB, sd[i->dst]);
        addr_of(t, A64_TMP, FB, sd[i->a]);
        emit_copy(t, A64_ADDR, A64_TMP, 16);
        break;
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV:
        a64_fldr(t, 0, FB, sd[i->a], 16);
        a64_fldr(t, 1, FB, sd[i->b], 16);
        call_helper(t, st, i->op == IR_ADD ? "__addtf3" : i->op == IR_SUB
                               ? "__subtf3" : i->op == IR_MUL ? "__multf3"
                               : "__divtf3");
        a64_fstr(t, 0, FB, sd[i->dst], 16);
        break;
    case IR_NEG:              /* flip bit 127: exact for -0.0 and NaN */
        ld_slot(t, sd, i->a, A64_ACC, 8, 0, 8);
        st_slot(t, sd, i->dst, A64_ACC, 8);
        a64_ldr(t, A64_ACC, FB, sd[i->a] + 8, 8, 0, 8);
        a64_mov_imm(t, A64_TMP, (long)(1UL << 63), 8);
        a64_alu_reg(t, '^', A64_ACC, A64_ACC, A64_TMP, 8);
        a64_str(t, A64_ACC, FB, sd[i->dst] + 8, 8);
        break;
    case IR_CMP: {
        /* each helper's int result stands in the same relation to 0 as a
         * to b (and makes an unordered pair false): __eqtf2 == 0,
         * __netf2 != 0, __lttf2 < 0, __letf2 <= 0, __gttf2 > 0,
         * __getf2 >= 0 */
        const char *h = i->pred == B_EQ ? "__eqtf2" : i->pred == B_NE
                        ? "__netf2" : i->pred == B_LT ? "__lttf2"
                        : i->pred == B_LE ? "__letf2" : i->pred == B_GT
                        ? "__gttf2" : "__getf2";
        a64_fldr(t, 0, FB, sd[i->a], 16);
        a64_fldr(t, 1, FB, sd[i->b], 16);
        call_helper(t, st, h);
        a64_cmp_reg(t, 0, A64_ZR, 4);
        a64_cset(t, A64_ACC, cond_for(i->pred, 1));
        st_slot(t, sd, i->dst, A64_ACC, 8);
        break;
    }
    case IR_I2F:              /* any integer, sign-extended to 64 bits */
        ld_slot(t, sd, i->a, 0, i->size, 1, 8);
        call_helper(t, st, "__floatditf");
        a64_fstr(t, 0, FB, sd[i->dst], 16);
        break;
    case IR_F2I:              /* truncates toward zero, to 64 bits */
        a64_fldr(t, 0, FB, sd[i->a], 16);
        call_helper(t, st, "__fixtfdi");
        st_slot(t, sd, i->dst, 0, 8);
        break;
    case IR_F2F:
        if (i->w == 16) {     /* float/double -> long double: exact */
            a64_fldr(t, 0, FB, sd[i->a], i->size);
            call_helper(t, st, i->size == 4 ? "__extendsftf2"
                                            : "__extenddftf2");
            a64_fstr(t, 0, FB, sd[i->dst], 16);
        } else {              /* long double -> float/double: rounds */
            a64_fldr(t, 0, FB, sd[i->a], 16);
            call_helper(t, st, i->w == 4 ? "__trunctfsf2" : "__trunctfdf2");
            a64_fstr(t, 0, FB, sd[i->dst], i->w);
        }
        break;
    default:
        break;
    }
}

/* ---- __int128: two eightbytes in a 16-byte slot ----
 * Add, subtract, the bitwise ops, negation and comparison are inline (adds/
 * adc, subs/sbcs); multiplication, division, shifts and the float
 * conversions are libgcc's, the 128-bit values in x0:x1 and x2:x3. */
static int a64_i128_ins(const struct ir_ins *i)
{
    if (i->flt)
        return 0;
    switch (i->op) {
    case IR_CONST: case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV:
    case IR_MOD: case IR_AND: case IR_OR: case IR_XOR: case IR_SHL:
    case IR_SHR: case IR_NEG: case IR_BNOT: case IR_CMP:
        return i->w == 16;
    case IR_EXT:
        return i->w == 16 || (g_a64_wide && i->a >= 0 && g_a64_wide[i->a]);
    case IR_I2F:
        return i->size == 16;
    case IR_F2I:
        return i->w == 16;
    case IR_CAS16:
        return 1;
    default:
        return 0;
    }
}

/* op Xd, Xn, Xm with a register-register encoding base */
static void a64_rrr(struct code *t, unsigned long base, int d, int n, int m)
{
    a64_word(t, base | ((unsigned long)m << 16) | ((unsigned long)n << 5) |
                (unsigned long)d);
}

#define A64_ADDS 0xAB000000UL
#define A64_ADC  0x9A000000UL
#define A64_SUBS 0xEB000000UL
#define A64_SBC  0xDA000000UL
#define A64_SBCS 0xFA000000UL

static void gen_a64_i128(struct code *t, const long *sd, struct ir_ins *i,
                         struct a64_sites *st)
{
    long d = i->dst >= 0 ? sd[i->dst] : 0;
    int X = A64_ACC, Y = A64_TMP, Z = 13, W = 14;   /* x9 x10 x13 x14 */
    switch (i->op) {
    case IR_CAS16: {
        /* An exclusive pair: ldxp, and stxp of the desired value if the
         * pair read is the expected one — or of the pair read itself if
         * not, as only a successful stxp makes the ldxp's 16 bytes one
         * single-copy-atomic read; retried until a stxp goes through.
         * x0:x1 expected, x2:x3 desired, x4:x5 seen, w6 the status. */
        a64_ldr(t, A64_ADDR, FB, sd[i->a], 8, 0, 8);
        a64_ldr(t, 0, FB, sd[i->b], 8, 0, 8);
        a64_ldr(t, 1, FB, sd[i->b] + 8, 8, 0, 8);
        a64_ldr(t, 2, FB, sd[i->c], 8, 0, 8);
        a64_ldr(t, 3, FB, sd[i->c] + 8, 8, 0, 8);
        a64_dmb_ish(t);
        int loop = t->len;
        a64_word(t, 0xC87F0000UL | (5UL << 10) |       /* ldxp x4, x5, */
                    ((unsigned long)A64_ADDR << 5) | 4UL);   /* [addr] */
        a64_alu_reg(t, '^', 7, 4, 0, 8);
        a64_alu_reg(t, '^', Z, 5, 1, 8);
        a64_alu_reg(t, '|', 7, 7, Z, 8);
        int miss = a64_cbz(t, 7, 1, 8);                 /* not expected */
        a64_word(t, 0xC8200000UL | (6UL << 16) | (3UL << 10) |
                    ((unsigned long)A64_ADDR << 5) | 2UL); /* stxp w6, x2, x3 */
        int done = a64_b(t);
        a64_patch_b19(t, miss, t->len);
        a64_word(t, 0xC8200000UL | (6UL << 16) | (5UL << 10) |
                    ((unsigned long)A64_ADDR << 5) | 4UL); /* stxp w6, x4, x5 */
        a64_patch_b26(t, done, t->len);
        a64_patch_b19(t, a64_cbz(t, 6, 1, 4), loop);    /* lost it: again */
        a64_dmb_ish(t);
        a64_str(t, 4, FB, d, 8);
        a64_str(t, 5, FB, d + 8, 8);
        break;
    }
    case IR_CONST:
        a64_mov_imm(t, X, i->imm, 8);
        a64_str(t, X, FB, d, 8);
        a64_mov_imm(t, X, i->imm < 0 ? -1 : 0, 8);
        a64_str(t, X, FB, d + 8, 8);
        break;
    case IR_ADD: case IR_SUB:
        a64_ldr(t, X, FB, sd[i->a], 8, 0, 8);
        a64_ldr(t, Y, FB, sd[i->a] + 8, 8, 0, 8);
        a64_ldr(t, Z, FB, sd[i->b], 8, 0, 8);
        a64_ldr(t, W, FB, sd[i->b] + 8, 8, 0, 8);
        a64_rrr(t, i->op == IR_ADD ? A64_ADDS : A64_SUBS, X, X, Z);
        a64_rrr(t, i->op == IR_ADD ? A64_ADC : A64_SBC, Y, Y, W);
        a64_str(t, X, FB, d, 8);
        a64_str(t, Y, FB, d + 8, 8);
        break;
    case IR_AND: case IR_OR: case IR_XOR: {
        int op = i->op == IR_AND ? '&' : i->op == IR_OR ? '|' : '^';
        for (int q = 0; q < 16; q += 8) {
            a64_ldr(t, X, FB, sd[i->a] + q, 8, 0, 8);
            a64_ldr(t, Y, FB, sd[i->b] + q, 8, 0, 8);
            a64_alu_reg(t, op, X, X, Y, 8);
            a64_str(t, X, FB, d + q, 8);
        }
        break;
    }
    case IR_NEG:                   /* negs lo; ngc hi */
        a64_ldr(t, X, FB, sd[i->a], 8, 0, 8);
        a64_ldr(t, Y, FB, sd[i->a] + 8, 8, 0, 8);
        a64_rrr(t, A64_SUBS, X, A64_ZR, X);
        a64_rrr(t, A64_SBC, Y, A64_ZR, Y);
        a64_str(t, X, FB, d, 8);
        a64_str(t, Y, FB, d + 8, 8);
        break;
    case IR_BNOT:
        for (int q = 0; q < 16; q += 8) {
            a64_ldr(t, X, FB, sd[i->a] + q, 8, 0, 8);
            a64_mvn(t, X, X, 8);
            a64_str(t, X, FB, d + q, 8);
        }
        break;
    case IR_MUL: case IR_DIV: case IR_MOD:
        a64_ldr(t, 0, FB, sd[i->a], 8, 0, 8);
        a64_ldr(t, 1, FB, sd[i->a] + 8, 8, 0, 8);
        a64_ldr(t, 2, FB, sd[i->b], 8, 0, 8);
        a64_ldr(t, 3, FB, sd[i->b] + 8, 8, 0, 8);
        call_helper(t, st, i->op == IR_MUL ? "__multi3"
                           : i->op == IR_DIV
                           ? (i->sign ? "__divti3" : "__udivti3")
                           : (i->sign ? "__modti3" : "__umodti3"));
        a64_str(t, 0, FB, d, 8);
        a64_str(t, 1, FB, d + 8, 8);
        break;
    case IR_SHL: case IR_SHR:
        if (g_a64_wide && g_a64_wide[i->b])
            a64_ldr(t, 2, FB, sd[i->b], 8, 0, 8);
        else
            ld_slot(t, sd, i->b, 2, 8, 0, 8);
        a64_ldr(t, 0, FB, sd[i->a], 8, 0, 8);
        a64_ldr(t, 1, FB, sd[i->a] + 8, 8, 0, 8);
        call_helper(t, st, i->op == IR_SHL ? "__ashlti3"
                           : i->sign ? "__ashrti3" : "__lshrti3");
        a64_str(t, 0, FB, d, 8);
        a64_str(t, 1, FB, d + 8, 8);
        break;
    case IR_CMP:
        if (i->pred == B_EQ || i->pred == B_NE) {
            a64_ldr(t, X, FB, sd[i->a], 8, 0, 8);
            a64_ldr(t, Y, FB, sd[i->b], 8, 0, 8);
            a64_alu_reg(t, '^', X, X, Y, 8);
            a64_ldr(t, Z, FB, sd[i->a] + 8, 8, 0, 8);
            a64_ldr(t, W, FB, sd[i->b] + 8, 8, 0, 8);
            a64_alu_reg(t, '^', Z, Z, W, 8);
            a64_alu_reg(t, '|', X, X, Z, 8);
            a64_cmp_reg(t, X, A64_ZR, 8);
            a64_cset(t, X, cond_for(i->pred, 0));
        } else {
            /* cmp lo; sbcs hi: the flags of the whole difference — <,
             * >= from a - b, >, <= from b - a */
            int swap = i->pred == B_GT || i->pred == B_LE;
            int x = swap ? i->b : i->a, y = swap ? i->a : i->b;
            a64_ldr(t, X, FB, sd[x], 8, 0, 8);
            a64_ldr(t, Y, FB, sd[x] + 8, 8, 0, 8);
            a64_ldr(t, Z, FB, sd[y], 8, 0, 8);
            a64_ldr(t, W, FB, sd[y] + 8, 8, 0, 8);
            a64_rrr(t, A64_SUBS, A64_ZR, X, Z);
            a64_rrr(t, A64_SBCS, A64_ZR, Y, W);
            a64_cset(t, X, cond_for(i->pred == B_LT || i->pred == B_GT
                                    ? B_LT : B_GE, i->sign));
        }
        st_slot(t, sd, i->dst, X, 8);
        break;
    case IR_EXT:
        if (i->w == 16) {
            if (g_a64_wide && g_a64_wide[i->a])
                a64_ldr(t, X, FB, sd[i->a], 8, 0, 8);
            else
                ld_slot(t, sd, i->a, X, i->size, i->sign, 8);
            a64_str(t, X, FB, d, 8);
            if (i->sign)                  /* asr y, x, #63 */
                a64_word(t, 0x937FFC00UL | ((unsigned long)X << 5) |
                            (unsigned long)Y);
            else
                a64_mov_imm(t, Y, 0, 8);
            a64_str(t, Y, FB, d + 8, 8);
        } else {
            /* the low bytes, re-extended as the target type */
            a64_ldr(t, X, FB, sd[i->a], i->size, i->sign, i->w);
            st_slot(t, sd, i->dst, X, 8);
        }
        break;
    case IR_I2F:
        a64_ldr(t, 0, FB, sd[i->a], 8, 0, 8);
        a64_ldr(t, 1, FB, sd[i->a] + 8, 8, 0, 8);
        call_helper(t, st, i->w == 4 ? (i->sign ? "__floattisf"
                                                : "__floatuntisf")
                           : i->w == 8 ? (i->sign ? "__floattidf"
                                                  : "__floatuntidf")
                           : (i->sign ? "__floattitf" : "__floatuntitf"));
        a64_fstr(t, 0, FB, d, i->w);
        break;
    case IR_F2I:
        a64_fldr(t, 0, FB, sd[i->a], i->size);
        call_helper(t, st, i->size == 4 ? (i->sign ? "__fixsfti"
                                                   : "__fixunssfti")
                           : i->size == 8 ? (i->sign ? "__fixdfti"
                                                     : "__fixunsdfti")
                           : (i->sign ? "__fixtfti" : "__fixunstfti"));
        a64_str(t, 0, FB, d, 8);
        a64_str(t, 1, FB, d + 8, 8);
        break;
    default:
        break;
    }
}

static void gen_func(struct ir_func *fn, struct code *t, struct a64_sites *st,
                     int want_debug)
{
    struct func *f = fn->src;

    struct a64_frame fr;
    g_a64_wide = cg_wide_vregs(fn);
    long *sd = layout_frame(fn, &fr);

    /* -g: each source variable's slot relative to the DWARF frame base,
     * x29. The prologue leaves sp (and x19, which pins it in a function
     * with a VLA) exactly fr.size below x29, and slots are sp-relative. */
    if (want_debug) {
        int nv = fn->nvars ? fn->nvars : 1;
        fn->var_off = xmalloc((size_t)nv * sizeof *fn->var_off);
        for (int v = 0; v < fn->nvars; v++)
            fn->var_off[v] = (int)(sd[v] - fr.size);
    }

    align16(t);
    f->code_off = t->len;

    a64_prologue(t, fr.size);
    /* for the unwind tables: stp x29, x30 ends at +4, mov x29, sp at +8 */
    f->cfi_push = 4;
    f->cfi_frame = 8;
    f->cfi_nsaved = 0;
    if (fn->has_alloca) {
        a64_str(t, A64_FBREG, A64_SP, fr.fb_save, 8);
        f->cfi_nsaved = 1;
        f->cfi_reg[0] = 19;
        f->cfi_off[0] = fr.fb_save - fr.size - 16;  /* the CFA is x29+16 */
        f->cfi_saved_at = t->len - f->code_off;
        a64_add_imm(t, A64_FBREG, A64_SP, 0, 8);     /* mov x19, sp */
        g_fb = A64_FBREG;
    }

    /* A variadic function saves every argument register first, raw, before
     * anything can disturb them: va_arg walks these areas later. Under
     * -mgeneral-regs-only (the kernel) the q registers are never touched —
     * gcc makes the same choice, and __vr_offs then says "none". */
    if (f->is_varargs) {
        for (int r = 0; r < 8; r++)
            a64_str(t, r, FB, fr.gr_save + r * 8, 8);
        if (fr.vr_save >= 0)
            for (int r = 0; r < 8; r++)
                a64_str_q(t, r, FB, fr.vr_save + r * 16);
    }

    /* Incoming parameters, placed by the same a64_place a call uses.
     * Arguments on the caller's stack sit above this frame: x29 points at
     * the saved x29/x30 pair, so the caller's area begins at x29+16. */
    {
        struct a64_cursor cu = { 0, 0, 0, 0 };
        if (fr.sret >= 0)
            a64_str(t, A64_SRET, FB, fr.sret, 8);
        for (int p = 0; p < f->nparams; p++) {
            struct a64_argplan pl;
            a64_place_arg(&fn->param_abi[p], p, f->sret_first, &cu, &pl);
            if (pl.where == AP_V) {
                for (int q = 0; q < pl.nreg; q++)
                    a64_fstr(t, pl.reg + q, FB, sd[p] + q * pl.esz, pl.esz);
            } else if (pl.byref) {
                /* A pointer to the caller's copy: take our own, so the
                 * parameter's address is an ordinary local. */
                int src = pl.reg;
                if (pl.where == AP_STACK) {
                    a64_ldr(t, A64_TMP, A64_FP, 16 + pl.stk_off, 8, 0, 8);
                    src = A64_TMP;
                }
                addr_of(t, A64_ADDR, FB, sd[p]);
                emit_copy(t, A64_ADDR, src, pl.size);
            } else if (pl.where == AP_X) {
                if (pl.is_struct || pl.nreg == 2)   /* (or an __int128) */
                    for (int q = 0; q < pl.nreg; q++)
                        a64_str(t, pl.reg + q, FB, sd[p] + q * 8, 8);
                else
                    a64_str(t, pl.reg, FB, sd[p], pl.size > 8 ? 8 : pl.size);
            } else if (pl.is_struct || pl.size == 16) {  /* or a long double */
                addr_of(t, A64_ADDR, FB, sd[p]);
                addr_of(t, A64_TMP, A64_FP, 16 + pl.stk_off);
                emit_copy(t, A64_ADDR, A64_TMP, pl.size);
            } else {
                a64_ldr(t, A64_ACC, A64_FP, 16 + pl.stk_off, 8, 0, 8);
                st_slot(t, sd, p, A64_ACC, pl.size > 8 ? 8 : pl.size);
            }
        }
    }

    /* Label offsets and the branches waiting on them. */
    int *loff = xmalloc((size_t)(fn->nlabels ? fn->nlabels : 1) * sizeof *loff);
    for (int l = 0; l < fn->nlabels; l++)
        loff[l] = -1;
    struct a64_fix *fix = NULL;
    int nfix = 0, capfix = 0;
    /* Every `return` jumps to the single epilogue at the end. */
    int *retfix = NULL;
    int nret = 0, capret = 0;

    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        int ins_start = t->len;

        /* -g: a line-table row wherever the source line changes, exactly
         * as the x86 backend records them (t->len is where this
         * instruction's code starts). */
        if (want_debug && i->line) {
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

        /* -mgeneral-regs-only (the kernel's mode) means the FP registers
         * may not be touched at all: on aarch64 they trap until
         * CPACR_EL1.FPEN is set, exactly as SSE does before CR4.OSFXSR. */
        if (g_no_fp && (i->flt || i->op == IR_I2F || i->op == IR_F2I ||
                        i->op == IR_F2F))
            diag_fatal(f->file, i->line ? i->line : f->line,
                       "floating point used under -mgeneral-regs-only "
                       "(in '%s')", f->name);

        if (a64_i128_ins(i)) {        /* __int128 (before: I2F, F2I) */
            gen_a64_i128(t, sd, i, st);
            continue;
        }
        if (a64_ld_ins(i)) {          /* long double: 16 bytes, libgcc */
            gen_a64_ld(t, sd, i, st);
            continue;
        }

        switch (i->op) {
        case IR_CONST:
            a64_mov_imm(t, A64_ACC, i->imm, i->w);
            st_slot(t, sd, i->dst, A64_ACC, 8);
            break;

        case IR_MOV:
            ld_slot(t, sd, i->a, A64_ACC, 8, 0, 8);
            st_slot(t, sd, i->dst, A64_ACC, 8);
            break;

        case IR_ADD: case IR_SUB: case IR_AND: case IR_OR: case IR_XOR: {
            if (i->flt) {
                /* Only +, - and * ever reach here as floats; the bitwise
                 * operators have no floating-point form in C. */
                fbin(t, sd, i, i->op == IR_ADD ? '+' : '-');
                break;
            }
            int op = i->op == IR_ADD ? '+' : i->op == IR_SUB ? '-'
                   : i->op == IR_AND ? '&' : i->op == IR_OR  ? '|' : '^';
            ld_slot(t, sd, i->a, A64_ACC, 8, 0, 8);
            operand_b(t, sd, i);
            a64_alu_reg(t, op, A64_ACC, A64_ACC, A64_TMP, i->w);
            st_slot(t, sd, i->dst, A64_ACC, 8);
            break;
        }

        case IR_MUL:
            if (i->flt) { fbin(t, sd, i, '*'); break; }
            ld_slot(t, sd, i->a, A64_ACC, 8, 0, 8);
            operand_b(t, sd, i);
            a64_mul(t, A64_ACC, A64_ACC, A64_TMP, i->w);
            st_slot(t, sd, i->dst, A64_ACC, 8);
            break;

        case IR_DIV:
            if (i->flt) { fbin(t, sd, i, '/'); break; }
            ld_slot(t, sd, i->a, A64_ACC, 8, 0, 8);
            operand_b(t, sd, i);
            a64_div(t, A64_ACC, A64_ACC, A64_TMP, i->sign, i->w);
            st_slot(t, sd, i->dst, A64_ACC, 8);
            break;

        case IR_MOD:
            /* q = a / b ; r = a - q*b. msub does the second half. */
            ld_slot(t, sd, i->a, A64_ACC, 8, 0, 8);
            operand_b(t, sd, i);
            a64_div(t, A64_ADDR, A64_ACC, A64_TMP, i->sign, i->w);
            a64_msub(t, A64_ACC, A64_ADDR, A64_TMP, A64_ACC, i->w);
            st_slot(t, sd, i->dst, A64_ACC, 8);
            break;

        case IR_SHL: case IR_SHR:
            ld_slot(t, sd, i->a, A64_ACC, 8, 0, 8);
            operand_b(t, sd, i);
            a64_shift_reg(t, i->op == IR_SHL ? '<' : (i->sign ? '>' : 'u'),
                          A64_ACC, A64_ACC, A64_TMP, i->w);
            st_slot(t, sd, i->dst, A64_ACC, 8);
            break;

        case IR_SQRT:
            a64_fldr(t, A64_FACC, FB, sd[i->a], i->w);
            a64_fsqrt(t, A64_FACC, A64_FACC, i->w);
            a64_fstr(t, A64_FACC, FB, sd[i->dst], i->w);
            break;

        case IR_NEG:
            if (i->flt) {
                a64_fldr(t, A64_FACC, FB, sd[i->a], i->w);
                a64_fneg(t, A64_FACC, A64_FACC, i->w);
                a64_fstr(t, A64_FACC, FB, sd[i->dst], i->w);
                break;
            }
            ld_slot(t, sd, i->a, A64_ACC, 8, 0, 8);
            a64_neg(t, A64_ACC, A64_ACC, i->w);
            st_slot(t, sd, i->dst, A64_ACC, 8);
            break;

        case IR_BNOT:
            ld_slot(t, sd, i->a, A64_ACC, 8, 0, 8);
            a64_mvn(t, A64_ACC, A64_ACC, i->w);
            st_slot(t, sd, i->dst, A64_ACC, 8);
            break;

        case IR_CMP:
            if (i->flt) {
                /* FCMP leaves NZCV "unordered" (N=0 Z=0 C=1 V=1) when either
                 * operand is NaN, which is why LT and LE need mi/ls rather
                 * than lt/le: lt tests N!=V and would read TRUE on a NaN,
                 * where C requires every ordered comparison against NaN to
                 * be false. eq/ne/gt/ge already fall out correctly. */
                a64_fldr(t, A64_FACC, FB, sd[i->a], i->w);
                a64_fldr(t, A64_FTMP, FB, sd[i->b], i->w);
                a64_fcmp(t, A64_FACC, A64_FTMP, i->w);
                int cc = i->pred == B_EQ ? A64_EQ : i->pred == B_NE ? A64_NE
                       : i->pred == B_LT ? A64_MI : i->pred == B_LE ? A64_LS
                       : i->pred == B_GT ? A64_GT : A64_GE;
                a64_cset(t, A64_ACC, cc);
                st_slot(t, sd, i->dst, A64_ACC, 8);
                break;
            }
            ld_slot(t, sd, i->a, A64_ACC, 8, 0, 8);
            operand_b(t, sd, i);
            a64_cmp_reg(t, A64_ACC, A64_TMP, i->w);
            a64_cset(t, A64_ACC, cond_for(i->pred, i->sign));
            st_slot(t, sd, i->dst, A64_ACC, 8);
            break;

        case IR_LDVAR:
            ld_slot(t, sd, i->a, A64_ACC, i->size, i->sign, i->w);
            st_slot(t, sd, i->dst, A64_ACC, 8);
            break;

        case IR_STVAR:
            ld_slot(t, sd, i->a, A64_ACC, 8, 0, 8);
            st_slot(t, sd, i->dst, A64_ACC, i->size);
            break;

        case IR_ADDR:
            addr_of(t, A64_ACC, FB, sd[i->a]);
            st_slot(t, sd, i->dst, A64_ACC, 8);
            break;

        case IR_STRADDR: {
            struct strsite hi, lo;
            hi.patch_off = a64_adrp(t, A64_ACC);
            hi.str_off = i->label;          /* resolved to an offset below */
            hi.kind = RK_ADR_HI21;
            PUSH(st->str, st->nstr, st->capstr, hi);
            lo.patch_off = a64_add_lo12(t, A64_ACC, A64_ACC);
            lo.str_off = i->label;
            lo.kind = RK_ADD_LO12;
            PUSH(st->str, st->nstr, st->capstr, lo);
            st_slot(t, sd, i->dst, A64_ACC, 8);
            break;
        }

        case IR_GADDR: {
            /* (a weak reference through its GOT slot: adrp/add would give
             * the page of the pc, not 0, for an undefined one) */
            int got = i->glob->is_weak && !i->glob->defined;
            struct gsite hi, lo;
            hi.patch_off = a64_adrp(t, A64_ACC);
            hi.glob = i->glob;
            hi.kind = got ? RK_GOT_PAGE : RK_ADR_HI21;
            PUSH(st->g, st->ng, st->capg, hi);
            if (got) {
                lo.patch_off = t->len;
                a64_ldr(t, A64_ACC, A64_ACC, 0, 8, 0, 8);
            } else {
                lo.patch_off = a64_add_lo12(t, A64_ACC, A64_ACC);
            }
            lo.glob = i->glob;
            lo.kind = got ? RK_GOT_LO12 : RK_ADD_LO12;
            PUSH(st->g, st->ng, st->capg, lo);
            st_slot(t, sd, i->dst, A64_ACC, 8);
            break;
        }

        case IR_FADDR: {
            int got = i->callee->is_weak && !i->callee->has_defn;
            struct fsite hi, lo;
            hi.patch_off = a64_adrp(t, A64_ACC);
            hi.target = i->callee;
            hi.kind = got ? RK_GOT_PAGE : RK_ADR_HI21;
            PUSH(st->f, st->nf, st->capf, hi);
            if (got) {
                lo.patch_off = t->len;
                a64_ldr(t, A64_ACC, A64_ACC, 0, 8, 0, 8);
            } else {
                lo.patch_off = a64_add_lo12(t, A64_ACC, A64_ACC);
            }
            lo.target = i->callee;
            lo.kind = got ? RK_GOT_LO12 : RK_ADD_LO12;
            PUSH(st->f, st->nf, st->capf, lo);
            st_slot(t, sd, i->dst, A64_ACC, 8);
            break;
        }

        case IR_LOAD:
            ld_slot(t, sd, i->a, A64_ADDR, 8, 0, 8);
            a64_ldr(t, A64_ACC, A64_ADDR, 0, i->size, i->sign, i->w);
            st_slot(t, sd, i->dst, A64_ACC, 8);
            break;

        case IR_STORE:
            ld_slot(t, sd, i->a, A64_ADDR, 8, 0, 8);
            ld_slot(t, sd, i->b, A64_ACC, 8, 0, 8);
            a64_str(t, A64_ACC, A64_ADDR, 0, i->size);
            break;

        case IR_EXT:
            ld_slot(t, sd, i->a, A64_ACC, 8, 0, 8);
            a64_extend(t, A64_ACC, A64_ACC, i->size, i->sign, i->w);
            st_slot(t, sd, i->dst, A64_ACC, 8);
            break;

        case IR_BSWAP:
            ld_slot(t, sd, i->a, A64_ACC, 8, 0, 8);
            a64_rev(t, A64_ACC, A64_ACC, i->size);
            if (i->size == 2)                 /* rev16 leaves the top bits */
                a64_extend(t, A64_ACC, A64_ACC, 2, 0, 8);
            st_slot(t, sd, i->dst, A64_ACC, 8);
            break;

        case IR_MEMCPY:
            ld_slot(t, sd, i->a, A64_ADDR, 8, 0, 8);
            ld_slot(t, sd, i->b, A64_SCR, 8, 0, 8);
            emit_copy(t, A64_ADDR, A64_SCR, i->size);
            break;

        case IR_MEMZERO:
            ld_slot(t, sd, i->a, A64_ADDR, 8, 0, 8);
            emit_zero(t, A64_ADDR, i->size);
            break;

        case IR_FENCE:
            a64_dmb_ish(t);
            break;

        case IR_UD2:
            a64_udf(t);
            break;

        case IR_LABEL:
            loff[i->label] = t->len;
            break;

        case IR_JMP: {
            struct a64_fix fx;
            fx.at = a64_b(t); fx.label = i->label; fx.kind = FIX_B26;
            PUSH(fix, nfix, capfix, fx);
            break;
        }

        case IR_BRZ: case IR_BRNZ: {
            ld_slot(t, sd, i->a, A64_ACC, 8, 0, 8);
            struct a64_fix fx;
            fx.at = a64_cbz(t, A64_ACC, i->op == IR_BRNZ, i->w);
            fx.label = i->label; fx.kind = FIX_B19;
            PUSH(fix, nfix, capfix, fx);
            break;
        }

        case IR_RET: {
            if (i->a >= 0) {
                struct type *rt = f->ret_ty;
                int esz, nh = a64_hfa(rt, &esz);
                if (i->flt) {
                    a64_fldr(t, 0, FB, sd[i->a], i->w);
                } else if (nh) {
                    /* an HFA comes back one member per v register */
                    ld_slot(t, sd, i->a, A64_ADDR, 8, 0, 8);
                    for (int q = 0; q < nh; q++)
                        a64_fldr(t, q, A64_ADDR, q * esz, esz);
                } else if (rt->kind == TY_STRUCT) {
                    int size = ty_size(rt);
                    /* The value's ADDRESS is in the operand slot. Small
                     * composites return in x0/x1; a large one is copied
                     * through the hidden pointer the caller supplied. */
                    ld_slot(t, sd, i->a, A64_ADDR, 8, 0, 8);
                    if (size <= 16) {
                        for (int q = 0; q * 8 < size; q++)
                            a64_ldr(t, q, A64_ADDR, q * 8, 8, 0, 8);
                    } else {
                        a64_ldr(t, A64_SCR, FB, fr.sret, 8, 0, 8);
                        emit_copy(t, A64_SCR, A64_ADDR, size);
                        a64_ldr(t, 0, FB, fr.sret, 8, 0, 8);
                    }
                } else if (rt->kind == TY_INT128) {
                    a64_ldr(t, 0, FB, sd[i->a], 8, 0, 8);
                    a64_ldr(t, 1, FB, sd[i->a] + 8, 8, 0, 8);
                } else {
                    ld_slot(t, sd, i->a, 0, 8, 0, 8);
                }
            }
            struct a64_fix fx;
            fx.at = a64_b(t); fx.label = -1; fx.kind = FIX_B26;
            PUSH(retfix, nret, capret, fx.at);
            break;
        }

        case IR_CALL: {
            struct a64_argplan pl[MAX_PARAMS];
            struct a64_cursor cu = { 0, 0, 0, 0 };
            for (int k = 0; k < i->nargs; k++)
                a64_place_arg(&i->argv[k], k, i->sret_first, &cu, &pl[k]);

            /* 1. The copies B.3 passes by reference: the callee may write
             * its parameter, so it gets a copy, never the caller's object.
             * Destination first — addr_of may borrow A64_SCR, which would
             * clobber a source address already parked there. */
            for (int k = 0; k < i->nargs; k++) {
                if (!pl[k].byref)
                    continue;
                addr_of(t, A64_ADDR, FB, fr.byref + pl[k].copy_off);
                ld_slot(t, sd, i->argv[k].vreg, A64_SCR, 8, 0, 8);
                emit_copy(t, A64_ADDR, A64_SCR, pl[k].size);
            }
            /* 2. Stack arguments, which use the scratch registers, before
             * any argument register is loaded. */
            for (int k = 0; k < i->nargs; k++) {
                if (pl[k].where != AP_STACK)
                    continue;
                int v = i->argv[k].vreg;
                if (pl[k].byref) {
                    addr_of(t, A64_ACC, FB, fr.byref + pl[k].copy_off);
                    a64_str(t, A64_ACC, A64_SP, pl[k].stk_off, 8);
                } else if (pl[k].is_struct) {
                    addr_of(t, A64_ADDR, A64_SP, pl[k].stk_off);
                    ld_slot(t, sd, v, A64_SCR, 8, 0, 8);
                    emit_copy(t, A64_ADDR, A64_SCR, pl[k].size);
                } else if (pl[k].size == 16) {   /* a long double's bytes */
                    addr_of(t, A64_ADDR, A64_SP, pl[k].stk_off);
                    addr_of(t, A64_TMP, FB, sd[v]);
                    emit_copy(t, A64_ADDR, A64_TMP, 16);
                } else {
                    ld_slot(t, sd, v, A64_ACC, 8, 0, 8);
                    a64_str(t, A64_ACC, A64_SP, pl[k].stk_off, 8);
                }
            }
            /* 3. Register arguments, in order; each writes only its own
             * register, so no shuffle is needed. */
            for (int k = 0; k < i->nargs; k++) {
                int v = i->argv[k].vreg;
                if (pl[k].where == AP_V) {
                    if (pl[k].is_struct) {
                        ld_slot(t, sd, v, A64_ADDR, 8, 0, 8);
                        for (int q = 0; q < pl[k].nreg; q++)
                            a64_fldr(t, pl[k].reg + q, A64_ADDR,
                                     q * pl[k].esz, pl[k].esz);
                    } else {
                        a64_fldr(t, pl[k].reg, FB, sd[v], pl[k].esz);
                    }
                } else if (pl[k].where == AP_X) {
                    if (pl[k].byref) {
                        addr_of(t, pl[k].reg, FB, fr.byref + pl[k].copy_off);
                    } else if (pl[k].is_struct) {
                        ld_slot(t, sd, v, A64_ADDR, 8, 0, 8);
                        for (int q = 0; q < pl[k].nreg; q++)
                            a64_ldr(t, pl[k].reg + q, A64_ADDR, q * 8, 8, 0, 8);
                    } else if (pl[k].nreg == 2) {         /* an __int128 */
                        a64_ldr(t, pl[k].reg, FB, sd[v], 8, 0, 8);
                        a64_ldr(t, pl[k].reg + 1, FB, sd[v] + 8, 8, 0, 8);
                    } else {
                        ld_slot(t, sd, v, pl[k].reg, 8, 0, 8);
                    }
                }
            }
            /* A large composite return: the callee writes it to our scratch
             * through x8. */
            /* Precomputed by irgen (§9.1): the IR carries the ABI answer,
             * not the type it was derived from. */
            int resz = i->ret_hfa_size, resn = i->retsize ? i->ret_hfa_n : 0;
            if (i->retsize && i->ret_byref)
                addr_of(t, A64_SRET, FB, fr.scratch + i->scratch);

            if (i->indirect) {
                ld_slot(t, sd, i->a, A64_ADDR, 8, 0, 8);
                a64_blr(t, A64_ADDR);
            } else if (i->callee->has_defn) {
                struct a64_callsite cs;
                cs.patch_off = a64_bl(t);
                cs.target = i->callee;
                PUSH(st->call, st->ncall, st->capcall, cs);
            } else {
                struct extcall ec;
                ec.patch_off = a64_bl(t);
                ec.callee = i->callee;
                PUSH(st->ext, st->next, st->capext, ec);
            }

            if (i->dst < 0)
                break;
            if (i->retsize) {
                /* dst receives the scratch's ADDRESS — the contract irgen
                 * and the x86 backend share. An HFA arrives in v0.., a small
                 * composite in x0/x1; a large one is already there. */
                addr_of(t, A64_ADDR, FB, fr.scratch + i->scratch);
                if (resn) {
                    for (int q = 0; q < resn; q++)
                        a64_fstr(t, q, A64_ADDR, q * resz, resz);
                } else if (!i->ret_byref) {
                    for (int q = 0; q * 8 < i->retsize; q++)
                        a64_str(t, q, A64_ADDR, q * 8, 8);
                }
                st_slot(t, sd, i->dst, A64_ADDR, 8);
            } else if (i->flt) {
                a64_fstr(t, 0, FB, sd[i->dst], i->w);
            } else if (i->w == 16) {                      /* x0:x1 */
                a64_str(t, 0, FB, sd[i->dst], 8);
                a64_str(t, 1, FB, sd[i->dst] + 8, 8);
            } else {
                st_slot(t, sd, i->dst, 0, 8);
            }
            break;
        }

        case IR_I2F: {
            /* size/sign describe the integer SOURCE, w the float result. */
            int iw = i->size >= 8 ? 8 : 4;
            ld_slot(t, sd, i->a, A64_ACC, i->size, i->sign, iw);
            a64_cvt_i2f(t, A64_FACC, A64_ACC, i->sign, iw, i->w);
            a64_fstr(t, A64_FACC, FB, sd[i->dst], i->w);
            break;
        }
        case IR_F2I: {
            /* size is the float source; w/sign the integer result. Unlike
             * SSE, aarch64 has a native unsigned form, so the u64 case needs
             * no fixup sequence. */
            int iw = i->w >= 8 ? 8 : 4;
            a64_fldr(t, A64_FACC, FB, sd[i->a], i->size);
            a64_cvt_f2i(t, A64_ACC, A64_FACC, i->sign, iw, i->size);
            st_slot(t, sd, i->dst, A64_ACC, 8);
            break;
        }
        case IR_F2F:
            a64_fldr(t, A64_FACC, FB, sd[i->a], i->size);
            a64_fcvt(t, A64_FACC, A64_FACC, i->size, i->w);
            a64_fstr(t, A64_FACC, FB, sd[i->dst], i->w);
            break;
        case IR_VA_START: {
            /* AAPCS64's va_list record, 32 bytes:
             *   +0  __stack    the next variadic argument on the stack
             *   +8  __gr_top   the end of the saved x0..x7
             *   +16 __vr_top   the end of the saved q0..q7
             *   +24 __gr_offs  (int) -(x registers still unread) * 8
             *   +28 __vr_offs  (int) -(v registers still unread) * 16
             * The named parameters decide the starting point; walking them
             * with a64_place gives exactly the registers and stack they
             * took. `va_list` itself is a char * pointing at this record —
             * gcc's aarch64 va_list is a struct larger than 16 bytes, which
             * B.3 passes as a POINTER, so vfprintf and friends take ours
             * unchanged. */
            struct a64_cursor cu = { 0, 0, 0, 0 };
            struct a64_argplan pl;
            for (int p = 0; p < f->nparams; p++)
                a64_place_arg(&fn->param_abi[p], p, f->sret_first, &cu, &pl);
            long tag = fr.va_tag;
            addr_of(t, A64_ACC, A64_FP, 16 + ((cu.nsaa + 7) & ~7L));
            a64_str(t, A64_ACC, FB, tag + 0, 8);
            addr_of(t, A64_ACC, FB, fr.gr_save + 64);
            a64_str(t, A64_ACC, FB, tag + 8, 8);
            if (fr.vr_save >= 0)
                addr_of(t, A64_ACC, FB, fr.vr_save + 128);
            else
                a64_mov_imm(t, A64_ACC, 0, 8);
            a64_str(t, A64_ACC, FB, tag + 16, 8);
            a64_mov_imm(t, A64_ACC, -(8 - (cu.ngrn < 8 ? cu.ngrn : 8)) * 8, 4);
            a64_str(t, A64_ACC, FB, tag + 24, 4);
            a64_mov_imm(t, A64_ACC, fr.vr_save >= 0
                        ? -(8 - (cu.nsrn < 8 ? cu.nsrn : 8)) * 16 : 0, 4);
            a64_str(t, A64_ACC, FB, tag + 28, 4);
            /* *ap = &record  (i->a holds the address of the va_list) */
            ld_slot(t, sd, i->a, A64_ADDR, 8, 0, 8);
            addr_of(t, A64_ACC, FB, tag);
            a64_str(t, A64_ACC, A64_ADDR, 0, 8);
            break;
        }
        case IR_ASM: {
            /* Extended asm, assembled in irgen (aarch64/irgen.c irg_asm_arm64). Every value
             * lives in a stack slot, so nothing is live in a register across
             * the asm and clobbers need no saving: load the inputs into the
             * registers their constraints chose, splice the bytes, store the
             * outputs back through their lvalue addresses. */
            struct ir_asm *ia = i->asm_ir;
            int used[32] = { 0 };
            for (int k = 0; k < ia->nin; k++) used[ia->in[k].reg] = 1;
            for (int k = 0; k < ia->nout; k++) used[ia->out[k].reg] = 1;
            /* The address scratch: no operand's register, and never x12,
             * which a far slot access borrows internally. */
            static const int scr_pool[] = { 9, 10, 11, 13, 14, 15,
                                             0, 1, 2, 3, 4, 5, 6, 7, 8 };
            int scr = -1;
            for (unsigned k = 0; k < sizeof scr_pool / sizeof scr_pool[0]; k++)
                if (!used[scr_pool[k]]) { scr = scr_pool[k]; break; }
            if (scr < 0 && (ia->nout > 0))
                diag_fatal(f->file, i->line ? i->line : f->line,
                           "no scratch register left around an asm in '%s'",
                           f->name);
            /* A "+" output starts with the lvalue's current value. */
            for (int k = 0; k < ia->nout; k++) {
                if (!ia->out[k].inout)
                    continue;
                ld_slot(t, sd, ia->out[k].temp, scr, 8, 0, 8);
                a64_ldr(t, ia->out[k].reg, scr, 0, ia->out[k].size, 0,
                        ia->out[k].size > 4 ? 8 : 4);
            }
            for (int k = 0; k < ia->nin; k++)
                ld_slot(t, sd, ia->in[k].temp, ia->in[k].reg, 8, 0, 8);
            for (int k = 0; k < ia->codelen; k++)
                code_byte(t, ia->code[k]);
            for (int k = 0; k < ia->nout; k++) {
                ld_slot(t, sd, ia->out[k].temp, scr, 8, 0, 8);
                a64_str(t, ia->out[k].reg, scr, 0, ia->out[k].size);
            }
            break;
        }
        case IR_XCHG: case IR_XADD: case IR_ARMW: {
            /* A read-modify-write is an exclusive-access retry loop: load
             * exclusive, compute, store exclusive, and go again if another
             * observer touched the location in between (stxr's status is
             * nonzero). Bracketed by full barriers: the fence-based seq_cst
             * mapping, the same one irgen's atomic loads and stores use, so
             * the two never mix with the acquire/release-flavoured one. */
            int w = i->size == 8 ? 8 : 4;
            ld_slot(t, sd, i->a, A64_ADDR, 8, 0, 8);          /* address */
            ld_slot(t, sd, i->b, A64_TMP, 8, 0, 8);           /* operand */
            a64_dmb_ish(t);
            int loop = t->len;
            a64_ldxr(t, A64_ACC, A64_ADDR, i->size);          /* old value */
            int nv = A64_TMP;                                  /* XCHG stores b */
            if (i->op == IR_XADD) {
                a64_alu_reg(t, '+', A64_T13, A64_ACC, A64_TMP, w);
                nv = A64_T13;
            } else if (i->op == IR_ARMW) {
                int op = (int)i->imm;
                a64_alu_reg(t, op == 'n' ? '&' : op, A64_T13, A64_ACC, A64_TMP, w);
                if (op == 'n')
                    a64_mvn(t, A64_T13, A64_T13, w);
                nv = A64_T13;
            }
            a64_stxr(t, A64_SCR, nv, A64_ADDR, i->size);
            a64_patch_b19(t, a64_cbz(t, A64_SCR, 1, 4), loop);  /* lost it: retry */
            a64_dmb_ish(t);
            st_slot(t, sd, i->dst, A64_ACC, 8);
            break;
        }
        case IR_CAS: {
            /* By value: the result is the value seen, swapped or not. */
            int w = i->size == 8 ? 8 : 4;
            ld_slot(t, sd, i->a, A64_ADDR, 8, 0, 8);
            ld_slot(t, sd, i->b, A64_TMP, i->size, 0, w);     /* expected */
            ld_slot(t, sd, i->c, A64_T13, 8, 0, 8);           /* desired */
            a64_dmb_ish(t);
            int loop = t->len;
            a64_ldxr(t, A64_ACC, A64_ADDR, i->size);
            a64_cmp_reg(t, A64_ACC, A64_TMP, w);
            int miss = a64_bcond(t, A64_NE);
            a64_stxr(t, A64_SCR, A64_T13, A64_ADDR, i->size);
            a64_patch_b19(t, a64_cbz(t, A64_SCR, 1, 4), loop);
            a64_patch_b19(t, miss, t->len);
            a64_dmb_ish(t);
            st_slot(t, sd, i->dst, A64_ACC, 8);
            break;
        }
        case IR_CMPXCHG: {
            /* __atomic_compare_exchange: expected is passed by ADDRESS; on a
             * miss the value seen is written back through it, and the result
             * is whether the swap happened. */
            int w = i->size == 8 ? 8 : 4;
            ld_slot(t, sd, i->a, A64_ADDR, 8, 0, 8);          /* object */
            ld_slot(t, sd, i->b, A64_T14, 8, 0, 8);           /* &expected */
            ld_slot(t, sd, i->c, A64_T13, 8, 0, 8);           /* desired */
            a64_ldr(t, A64_TMP, A64_T14, 0, i->size, 0, w);   /* expected */
            a64_dmb_ish(t);
            int loop = t->len;
            a64_ldxr(t, A64_ACC, A64_ADDR, i->size);
            a64_cmp_reg(t, A64_ACC, A64_TMP, w);
            int miss = a64_bcond(t, A64_NE);
            a64_stxr(t, A64_SCR, A64_T13, A64_ADDR, i->size);
            a64_patch_b19(t, a64_cbz(t, A64_SCR, 1, 4), loop);
            a64_mov_imm(t, A64_ACC, 1, 4);                    /* swapped */
            int done = a64_b(t);
            a64_patch_b19(t, miss, t->len);
            a64_str(t, A64_ACC, A64_T14, 0, i->size);         /* *expected = seen */
            a64_mov_imm(t, A64_ACC, 0, 4);
            a64_patch_b26(t, done, t->len);
            a64_dmb_ish(t);
            st_slot(t, sd, i->dst, A64_ACC, 8);
            break;
        }
        case IR_FRAMEADDR:
            a64_mov_reg(t, A64_ACC, A64_FP, 8);
            st_slot(t, sd, i->dst, A64_ACC, 8);
            break;
        case IR_ALLOCA:
            /* sp -= round16(size); the block starts above the outgoing
             * area (already a multiple of 16), which moves down with sp */
            ld_slot(t, sd, i->a, A64_ACC, 8, 0, 8);
            addr_of(t, A64_ACC, A64_ACC, 15);
            a64_mov_imm(t, A64_TMP, -16, 8);
            a64_alu_reg(t, '&', A64_ACC, A64_ACC, A64_TMP, 8);
            /* sub sp, sp, x9  (extended-register form, LSL #0) */
            a64_word(t, 0xCB2063FFUL | ((unsigned long)A64_ACC << 16));
            addr_of(t, A64_ACC, A64_SP, fr.outgoing);
            st_slot(t, sd, i->dst, A64_ACC, 8);
            break;
        case IR_SPSAVE:
            a64_add_imm(t, A64_ACC, A64_SP, 0, 8);    /* mov x9, sp */
            st_slot(t, sd, i->dst, A64_ACC, 8);
            break;
        case IR_SPRESTORE:
            ld_slot(t, sd, i->a, A64_ACC, 8, 0, 8);
            a64_add_imm(t, A64_SP, A64_ACC, 0, 8);    /* mov sp, x9 */
            break;
        case IR_LABELADDR: {
            /* &&label. adr gives the label's RUN-TIME address directly,
             * and its ±1 MiB reach covers any function EmbCC will emit. */
            struct a64_fix fx;
            fx.at = a64_adr(t, A64_ACC);
            fx.label = i->label;
            fx.kind = FIX_ADR;
            PUSH(fix, nfix, capfix, fx);
            st_slot(t, sd, i->dst, A64_ACC, 8);
            break;
        }
        case IR_IGOTO:
            ld_slot(t, sd, i->a, A64_ADDR, 8, 0, 8);
            a64_br(t, A64_ADDR);
            break;
        case IR_LANDING:
            /* the unwinder left the exception in x0, the selector in x1 */
            st_slot(t, sd, i->dst, 0, 8);
            st_slot(t, sd, i->b, 1, 8);
            break;
        default:
            diag_fatal(f->file, i->line ? i->line : f->line,
                       "aarch64 codegen: unhandled IR op %d in '%s'",
                       (int)i->op, f->name);
            break;
        }
        if (i->op == IR_CALL && fn->neh)
            ir_add_csite(fn, ins_start - f->code_off, t->len - f->code_off,
                         i->eh_region - 1);
    }

    /* The single epilogue every `return` branches to. With a VLA, sp is
     * wherever the last allocation left it: x29 knows where the frame
     * record is, and x19 is restored from the pinned frame first. */
    int epi = t->len;
    if (fn->has_alloca) {
        a64_ldr(t, A64_FBREG, A64_FBREG, fr.fb_save, 8, 0, 8);
        a64_word(t, 0x910003BFUL);                   /* mov sp, x29 */
        a64_epilogue(t, 0);
    } else {
        a64_epilogue(t, fr.size);
    }
    g_fb = A64_SP;
    free(g_a64_wide);
    g_a64_wide = NULL;

    for (int k = 0; k < nret; k++)
        a64_patch_b26(t, retfix[k], epi);
    for (int k = 0; k < nfix; k++) {
        int target = loff[fix[k].label];
        if (target < 0)
            diag_fatal(f->file, f->line,
                       "aarch64 codegen: branch to undefined label %d in '%s'",
                       fix[k].label, f->name);
        switch (fix[k].kind) {
        case FIX_B26: a64_patch_b26(t, fix[k].at, target); break;
        case FIX_B19: a64_patch_b19(t, fix[k].at, target); break;
        case FIX_ADR: a64_patch_adr(t, fix[k].at, target); break;
        }
    }

    for (int r = 0; r < fn->neh; r++)      /* where each landing pad is */
        fn->eh[r].lp_off = loff[fn->eh[r].lp_label] - f->code_off;
    f->code_len = t->len - f->code_off;
    free(loff); free(fix); free(retfix); free(sd);
}

/* ---- the unit -------------------------------------------------------- */

void codegen_unit_arm64(struct ir_unit *iu, struct code *text,
                        struct extcall **ext, int *next,
                        struct strsite **strs, int *nstrs,
                        struct gsite **gs, int *ngs,
                        struct fsite **fs, int *nfs, int want_debug,
                        int optimize, int no_sse, int regalloc)
{
    (void)optimize;   /* the IR arrives already optimized; this backend has
                       * no level-dependent output of its own yet */
    g_no_fp = no_sse;
    (void)regalloc;

    struct a64_sites st;
    st.call = NULL; st.ncall = st.capcall = 0;
    st.ext = NULL;  st.next = st.capext = 0;
    st.str = NULL;  st.nstr = st.capstr = 0;
    st.g = NULL;    st.ng = st.capg = 0;
    st.f = NULL;    st.nf = st.capf = 0;

    for (int n = 0; n < iu->nfuncs; n++)
        gen_func(&iu->funcs[n], text, &st, want_debug);

    /* Intra-unit calls resolve here, now that every function is placed. */
    for (int n = 0; n < st.ncall; n++)
        a64_patch_b26(text, st.call[n].patch_off,
                      st.call[n].target->code_off);
    free(st.call);

    /* String sites carried the literal's INDEX; turn it into its .rodata
     * offset now that the pool is final. */
    for (int n = 0; n < st.nstr; n++)
        st.str[n].str_off = iu->strs[st.str[n].str_off].off;

    *ext = st.ext;   *next = st.next;
    *strs = st.str;  *nstrs = st.nstr;
    *gs = st.g;      *ngs = st.ng;
    *fs = st.f;      *nfs = st.nf;
}
