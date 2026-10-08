/* SPARC V8 code generation for Gaisler's LEON3: big-endian, register
 * windows, soft float (docs/internals/sparc-plan.md).
 *
 * The shape is the MIPS backend's (src/arch/mips/codegen.c), itself
 * RV32's: every vreg has one home -- a register the shared allocator gave
 * it, or a frame slot -- and every operation reads its operands through
 * rdr/rd and writes its result through wreg/wrote, so the code is correct
 * with the allocator off and smaller with it on. What SPARC changes:
 *
 *   * REGISTER WINDOWS. Every function begins with `save %sp, -frame,
 *     %sp`, which gives it fresh locals and makes its caller's outs its
 *     ins; it returns with `ret; restore`. So the locals and the ins are
 *     preserved across every call at no cost, and nothing is ever saved
 *     in a prologue. Every slot is addressed from %fp, which never moves
 *     (alloca moves %sp only).
 *   * The convention passes arguments as unpadded WORDS, the first six in
 *     %o0-%o5 (the callee's %i0-%i5) and the rest at %sp+92; every
 *     aggregate goes by reference to a copy the caller makes; a struct
 *     comes back through the buffer whose address the caller stores at
 *     %sp+64, and the caller puts an `unimp` word after the call's delay
 *     slot, which the callee returns past.
 *   * CONDITION CODES: a comparison is `subcc` (cmp) and a branch on one
 *     of sixteen conditions, signed and unsigned; a 64-bit add or compare
 *     uses the carry (addcc/addxcc, subcc/subxcc).
 *   * DELAY SLOTS on every transfer, filled with an earlier instruction
 *     that may run there (take_slot), else a nop; `ret; restore` takes
 *     the return value's move into the restore, and the annulled `mov`
 *     idioms carry their own.
 *   * Immediates are signed 13 bits; addresses are sethi/or pairs
 *     (R_SPARC_HI22/LO10); a misaligned access traps, so one irgen cannot
 *     promise is aligned goes through bytes.
 *   * Multiply and divide are LEON3's umul/smul/udiv/sdiv through %y.
 *
 * Refused by name: atomics wider than a word, the frame and return
 * address builtins, a branch beyond +-8 MiB, and __int128. Inline asm is
 * assembled by sparc/asm.c and placed here (IR_ASM).
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

struct sparc_fn;
static void gen_ins(struct sparc_fn *F, int n);
static int copy_block(struct sparc_fn *F, int copy, long size, int align,
                      int sb, long so, int db, long dof);

/* The scratch registers, none of them in the allocator's pool: %g1-%g4
 * (caller-saved by the ABI, the application's to use), %l6 and %l7 (this
 * window's own), and %o7 (written by every call, so free between them).
 * A 64-bit binary operation needs four (A and B pairs); SCR and SCR2 are
 * for the few places that need more; FAR holds a large frame offset and
 * nothing else. */
#define A_LO SP_G1
#define A_HI SP_G2
#define B_LO SP_G3
#define B_HI SP_G4
#define ACC  A_LO        /* the value being computed */
#define TMP  A_HI        /* the second operand */
#define ADDR B_LO        /* an address */
#define SCR  SP_L7
#define SCR2 SP_L6
#define FAR  SP_O7
#define CALLREG SP_G1

struct sparc_sites {
    struct extcall *ext;   int next, capext;
    struct strsite *str;   int nstr, capstr;
    struct gsite *g;       int ng, capg;
    struct fsite *f;       int nf, capf;
};

struct sparc_fn {
    int *usecnt;         /* per vreg: how many reads (fusion), or NULL */
    int skip_next;       /* the instruction after this one is already out */
    int skip_to;         /* or every one before this index (cond_branch) */
    int want_debug;
    struct ir_func *fn;
    int *loc;            /* per vreg: its register, -1 in memory; NULL at -O0 */
    int used_callee[RA_MAXPOOL];
    int pair_used[16], npair;
    int nsave;
    struct code *t;
    struct sparc_sites *st;
    char *wide;          /* per vreg: a 64-bit value, a register pair */
    char *w16;           /* per vreg: a 16-byte long double, or NULL */
    char *nshr;          /* per vreg: a narrow high-word shift (narrow_shr) */
    int *last;           /* per vreg: its live range's last instruction, when
                          * there are long doubles (tf_operand); or NULL */
    char *remat;         /* per vreg: a constant made where it is an argument
                          * (const_remat), or NULL */
    long long *rematv;   /* ...its value */
    long *slot;          /* per vreg: byte offset from %sp after the save */
    long frame;          /* bytes the save moves %sp down by */
    long scratch_at;     /* where fn->scratch_bytes begins */
    long byref_at;       /* the by-reference argument copies */
    long tfa;            /* long double operand copies, 48 bytes, or -1 */
    long out_bytes;      /* the outgoing area: save area, sret, home, args */
    long va_first;       /* a variadic function's first unnamed word, or -1 */
    int *label_off;      /* per label id, or -1 while unseen */
    /* a branch to a label (base < 0), or a jump table's word holding the
     * label's offset from `base` */
    struct { int at; int label; int base; } *fix;
    int nfix, capfix;
    /* Delay-slot filling (take_slot): the lowest offset an instruction
     * may be taken from, and whether this function fills at all (not
     * under -g, whose line rows name offsets). */
    int barrier;
    int fill;
};

/* ---- the allocator's view of this machine ------------------------------
 *
 * Caller-saved first, as regalloc.h asks: %o0-%o5, which a call clobbers;
 * then the window's own %l0-%l5 and %i0-%i5, which survive every call at
 * no cost. %i6 (%fp), %i7 (the return address), %o6 (%sp) and %o7 are
 * never handed out, nor are %l6/%l7 (scratch). */
#define SPARC_NPOOL 18
static const int SPARC_POOL[SPARC_NPOOL] = {
    SP_O0, SP_O1, SP_O2, SP_O3, SP_O4, SP_O5,
    SP_L0, SP_L1, SP_L2, SP_L3, SP_L4, SP_L5,
    SP_I0, SP_I1, SP_I2, SP_I3, SP_I4, SP_I5
};
/* A variadic function stores %i0-%i5 into its caller's home area and
 * reads its named parameters back from there, so the ins are left out. */
static const int SPARC_POOL_VA[SPARC_NPOOL - 6] = {
    SP_O0, SP_O1, SP_O2, SP_O3, SP_O4, SP_O5,
    SP_L0, SP_L1, SP_L2, SP_L3, SP_L4, SP_L5
};

/* BYTE ORDER: big-endian, always. A 64-bit value's HIGH word is at +0 in
 * memory and in the lower-numbered register of a pair, as the ABI passes
 * a long long in %o0:%o1. A pair is named by its first register r. */
#define PHI(r) (r)
#define PLO(r) ((r) + 1)
#define WHI 0
#define WLO 4

static unsigned long g_sp_taken;        /* registers the pair pass took */
static int g_sp_pairs = 1;              /* this attempt uses the pair pass */
static int g_sp_leaf;                   /* this attempt is a leaf (leaf_fix) */
static int g_sp_pool[SPARC_NPOOL];

/* A LEAF attempt has the ins only: they become the outs (leaf_fix). */
static const int SPARC_POOL_LEAF[6] = {
    SP_I0, SP_I1, SP_I2, SP_I3, SP_I4, SP_I5
};

static const int *sparc_pool_for(const struct ir_func *fn, int *n)
{
    const int *p = g_sp_leaf ? SPARC_POOL_LEAF
                 : fn->is_varargs ? SPARC_POOL_VA : SPARC_POOL;
    int np = g_sp_leaf ? 6
           : fn->is_varargs ? SPARC_NPOOL - 6 : SPARC_NPOOL, k = 0;
    if (!g_sp_taken) {
        *n = np;
        return p;
    }
    for (int j = 0; j < np; j++)
        if (!(g_sp_taken >> p[j] & 1))
            g_sp_pool[k++] = p[j];
    *n = k;
    return g_sp_pool;
}

/* The PAIR pool, each pair named by its first register: %o0:%o1 (where a
 * 64-bit result comes back and every helper's first operand goes),
 * %o2:%o3, %o4:%o5, then the window's own for one that lives across a
 * call. */
#define SPARC_NPAIRS 9
static const int SPARC_PAIRS[SPARC_NPAIRS] = {
    SP_O0, SP_O2, SP_O4, SP_L0, SP_L2, SP_L4, SP_I0, SP_I2, SP_I4
};
static const int *sparc_pair_pool_for(const struct ir_func *fn, int *n)
{
    if (g_sp_leaf) {
        *n = 3;
        return SPARC_PAIRS + 6;                 /* %i0, %i2, %i4 */
    }
    *n = fn->is_varargs ? SPARC_NPAIRS - 3 : SPARC_NPAIRS;
    return SPARC_PAIRS;
}

static void sparc_pair_hints(const struct ir_func *fn, int *hint);

/* The locals and ins survive a call: the callee has its own window. */
static int sparc_callee_saved(int r)
{
    return (r >= SP_L0 && r <= SP_L5) || (r >= SP_I0 && r <= SP_I5);
}

static int sparc_ldvar_plain(int size, int sign, int w)
{
    (void)sign;
    return size == 4 && w == 4;
}

/* Which instructions become a CALL the IR does not show as one: every
 * floating-point operation (soft float), a 64-bit divide, and every
 * operation on a long double. */
int sparc_op_calls_helper(const struct ir_ins *i)
{
    if (i->w == 16 || ((i->op == IR_I2F || i->op == IR_F2I ||
                        i->op == IR_F2F) && i->size == 16))
        return i->flt || i->op == IR_I2F || i->op == IR_F2I ||
               i->op == IR_F2F;
    if (i->flt)
        return i->op == IR_ADD || i->op == IR_SUB || i->op == IR_MUL ||
               i->op == IR_DIV || i->op == IR_CMP;
    if (i->op == IR_I2F || i->op == IR_F2I || i->op == IR_F2F)
        return 1;
    return (i->op == IR_DIV || i->op == IR_MOD) && i->w == 8;
}

static void sparc_abi_hints(const struct ir_func *fn, int *hint);

static const struct ra_target SPARC_RATGT = {
    sparc_pool_for,
    sparc_callee_saved,
    sparc_ldvar_plain,
    1, 1, 2,        /* call args, returns, memcpy and every aggregate address
                     * from registers: the call setup is one parallel move
                     * (gen_call); a struct's address is read through rd */
    sparc_op_calls_helper,
    0,              /* three-operand */
    sparc_abi_hints,
    NULL, NULL,     /* no FP class: soft float in the integer registers */
    1,              /* ...allocated with them (float_in_gpr) */
    NULL, NULL,
    1,              /* atomic_in_reg: the casa/swap lowerings read through rdr */
    0,
    1               /* asm_in_reg: see IR_ASM */
};

static int g_sp_regalloc;

/* ---- refusal ------------------------------------------------------------ */

static void sparc_refuse(const struct sparc_fn *F, const struct ir_ins *i,
                         const char *what)
{
    char op[64];
    op[0] = 0;
    if (i)
        snprintf(op, sizeof op, " [%s w=%d size=%d]", ir_opname(i->op),
                 i->w, i->size);
    fprintf(stderr,
            "embcc: %s:%d: error: the SPARC backend cannot lower %s yet "
            "(function %s)%s\n",
            F->fn->file ? F->fn->file : "?",
            i ? i->line : F->fn->line, what, F->fn->name, op);
    exit(1);
}

/* ---- which values are eight bytes wide ----------------------------------
 *
 * RV32's and MIPS's rule: by the width of the RESULT, a local by its
 * declared size, and through copies that do not say four bytes. */
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

/* ---- the calling convention ------------------------------------------------
 *
 * Read off clang for sparc-none-elf (docs/internals/sparc-plan.md): the
 * arguments are a sequence of WORDS with no padding, the first six in
 * %o0-%o5 and the rest at %sp+92 on; an eight-byte scalar is two words,
 * high first, and may straddle %o5 and the stack; every aggregate and a
 * long double is one word, the address of the caller's copy. */
#define NARGREG 6
#define ARG_STK 92           /* the 7th word, from the caller's %sp */
#define HOME    68           /* the six-word home area */
#define SRET    64           /* the struct-return word */
#define MIN_FRAME 96

struct argplace {
    int reg, nreg;       /* first argument register index (0-5) and count */
    int nstk;            /* words on the stack */
    long stk;            /* %sp offset of the first stack word */
    int byref;           /* one word: the address of a copy */
    long copy;           /* gen_call: where that copy is */
};

static int arg_byref(const struct ir_arg *a)
{
    return a->is_struct || a->size > 8;
}

/* sret: this is the C++ indirect-result pointer (sret_first: the return
 * slot of a class that is not trivially copyable, which the C++ lowering
 * passes first). The SPARC V8 ABI passes it as it passes a C struct's
 * result buffer: in the struct-return word, %sp+64, taking no argument
 * word -- as clang++ and g++ do. */
static int fn_sret_first(const struct ir_func *fn)
{
    return fn->src && fn->src->sret_first;
}

static void place_arg(const struct ir_arg *a, int sret, int *word,
                      struct argplace *p)
{
    int byref = arg_byref(a);
    if (sret) {
        p->byref = 0;
        p->copy = -1;
        p->reg = NARGREG;
        p->nreg = 0;
        p->nstk = 1;
        p->stk = SRET;
        return;
    }
    int words = byref ? 1 : (a->size + 3) / 4;
    int w = *word;
    p->byref = byref;
    p->copy = -1;
    if (w < NARGREG) {
        p->reg = w;
        p->nreg = words < NARGREG - w ? words : NARGREG - w;
    } else {
        p->reg = NARGREG;
        p->nreg = 0;
    }
    p->nstk = words - p->nreg;
    p->stk = ARG_STK + 4L * (w + p->nreg - NARGREG);
    *word = w + words;
}

static int out_reg(int n) { return SP_O0 + n; }   /* the caller's view */
static int rd_caller(int r) { return r - SP_I0 + SP_O0; }  /* %iN as %oN */
static int in_reg_n(int n) { return SP_I0 + n; }  /* the callee's */

/* How many words of a returned composite come back in registers rather
 * than through the buffer: a _Complex of eight bytes (float, or int) in
 * %o0:%o1, one of sixteen (double) in %o0-%o3, as clang returns them; 0
 * for every other struct, and for a _Complex long double. -1 for a
 * complex whose convention has not been checked against clang. */
static int ret_reg_words(const struct type *t)
{
    if (!t || !t->is_complex || !t->celem)
        return 0;
    switch (ty_size(t)) {
    case 8:  return 2;
    case 16: return ty_is_float(t->celem) ? 4 : -1;
    case 32: return ty_is_float(t->celem) ? 0 : -1;
    default: return -1;
    }
}

/* Does this function return through the caller's buffer -- a struct, or
 * a long double? */
static int fn_sret(const struct ir_func *fn)
{
    if (fn_sret_first(fn))
        return 1;
    if (fn->ret_abi.is_struct)
        return ret_reg_words(fn->ret_abi.ty) == 0;
    return fn->ret_abi.size == 16;
}

static int call_sret(const struct ir_ins *i)
{
    if (i->retsize)
        return ret_reg_words(i->rety) == 0;
    return i->ret_tybytes == 16 && i->dst >= 0;
}

/* The size the caller's `unimp` after a struct-returning call holds. */
static long call_sret_size(const struct ir_ins *i)
{
    return i->retsize ? i->retsize : 16;
}

static void sparc_abi_hints(const struct ir_func *fn, int *hint)
{
    int word = 0;
    struct argplace pl;
    for (int p = 0; fn->src && p < fn->nparams && p < fn->nvregs; p++) {
        const struct ir_arg *a = &fn->param_abi[p];
        place_arg(a, p == 0 && fn_sret_first(fn), &word, &pl);
        if (pl.nreg == 1 && !pl.nstk && !pl.byref && a->size <= 4 &&
            !fn->is_varargs)
            hint[p] = in_reg_n(pl.reg);
    }
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        if (i->op == IR_RET && i->a >= 0 && i->a < fn->nvregs &&
            !fn->ret_abi.is_struct && fn->ret_abi.size <= 4 &&
            !fn->is_varargs)
            hint[i->a] = SP_I0;
        if (i->op != IR_CALL && sparc_op_calls_helper(i) && i->w <= 4 &&
            i->size <= 8) {
            if (i->a >= 0 && i->a < fn->nvregs && hint[i->a] < 0)
                hint[i->a] = SP_O0;
            if (!i->imm_b && i->b >= 0 && i->b < fn->nvregs &&
                hint[i->b] < 0)
                hint[i->b] = SP_O1;
            if (i->dst >= 0 && i->dst < fn->nvregs && hint[i->dst] < 0)
                hint[i->dst] = SP_O0;
        }
        if (i->op != IR_CALL)
            continue;
        if (!i->retsize && i->dst >= 0 && i->dst < fn->nvregs && i->w <= 4)
            hint[i->dst] = SP_O0;
        word = 0;
        for (int k = 0; k < i->nargs; k++) {
            const struct ir_arg *a = &i->argv[k];
            place_arg(a, k == 0 && i->sret_first, &word, &pl);
            /* not over a parameter's own register (or an earlier
             * call's): a value that lives across this call cannot take
             * an %o register, and the fallback was any free one */
            if (pl.nreg == 1 && !pl.nstk && !pl.byref && a->size <= 4 &&
                a->vreg >= 0 && a->vreg < fn->nvregs && hint[a->vreg] < 0)
                hint[a->vreg] = out_reg(pl.reg);
        }
    }
}

/* ---- the frame -------------------------------------------------------------
 *
 * From %sp upward: the 64-byte window save area, the struct-return word,
 * the six-word home area and the outgoing stack words (the outgoing area,
 * at least 96 bytes in every frame); then the by-reference copies, the
 * long double operand area, the shared temp slots, 64-bit temps without a
 * pair, locals (small ones first) and the struct-return scratch. A
 * multiple of 8 -- the window overflow handler stores with std. Every
 * slot is addressed from %fp, at slot - frame. */
#define STACK_ALIGN 8

static long outgoing_area(const struct sparc_fn *F)
{
    const struct ir_func *fn = F->fn;
    long most = MIN_FRAME;
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        struct argplace pl;
        int word = 0;
        long end;
        if (i->op != IR_CALL)
            continue;
        for (int k = 0; k < i->nargs; k++)
            place_arg(&i->argv[k], k == 0 && i->sret_first, &word, &pl);
        end = word > NARGREG ? ARG_STK + 4L * (word - NARGREG) : 0;
        if (end > most)
            most = end;
    }
    return (most + STACK_ALIGN - 1) & ~(long)(STACK_ALIGN - 1);
}

/* How many bytes of BY-REFERENCE COPIES the widest call needs. */
static long byref_area(const struct sparc_fn *F)
{
    const struct ir_func *fn = F->fn;
    long most = 0;
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        long need = 0;
        if (i->op != IR_CALL)
            continue;
        for (int k = 0; k < i->nargs; k++)
            if (arg_byref(&i->argv[k]))
                need = ((need + 7) & ~7L) + i->argv[k].size;
        if (need > most)
            most = need;
    }
    return (most + 7) & ~7L;
}

static int in_reg(const struct sparc_fn *F, int v);

static int is16(const struct sparc_fn *F, int v)
{
    return F->w16 && v >= 0 && v < F->fn->nvregs && F->w16[v];
}

static void layout(struct sparc_fn *F)
{
    struct ir_func *fn = F->fn;
    long off = outgoing_area(F);
    if (fn->has_alloca)
        off = (off + 15) & ~15L;     /* IR_ALLOCA's blocks sit above it */
    F->out_bytes = off;

    F->byref_at = off;
    off += byref_area(F);
    F->tfa = -1;
    if (F->w16) {
        F->tfa = off;
        off += 48;
    }

    F->slot = xmalloc((size_t)(fn->nvregs ? fn->nvregs : 1) * sizeof *F->slot);
    for (int v = 0; v < fn->nvregs; v++)
        F->slot[v] = -1;

    {
        int nv = fn->nvregs, npool = 0, has_cgoto = 0;
        int *loc2 = xmalloc((size_t)(nv ? nv : 1) * sizeof *loc2);
        for (int v = 0; v < nv; v++)
            loc2[v] = in_reg(F, v) || F->wide[v] || is16(F, v) ? 0 : -1;
        for (int n = 0; n < fn->nins; n++)
            if (fn->ins[n].op == IR_IGOTO || fn->ins[n].op == IR_LABELADDR)
                has_cgoto = 1;
        {
            struct ra_slots so = { loc2, NULL, g_sp_regalloc, has_cgoto };
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
            if (is16(F, v)) {
                off = (off + 7) & ~7L;
                F->slot[v] = off;
                off += 16;
                continue;
            }
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
                /* the frame is 8-aligned, and so is %fp: an aggregate
                 * asking for more is placed at 8 (no SPARC access needs
                 * more), as on MIPS; an over-aligned SCALAR is refused
                 * before here (the corpus's alignas-decl) */
                if (align > 8)
                    align = 8;
                off = (off + align - 1) & ~(long)(align - 1);
                F->slot[v] = off;
                off += size;
            }
        free(lref);
    }
    F->scratch_at = (off + 7) & ~7L;
    off = F->scratch_at + fn->scratch_bytes;
    F->frame = (off + STACK_ALIGN - 1) & ~(long)(STACK_ALIGN - 1);
    F->va_first = -1;
}

/* ---- reading and writing a vreg ----------------------------------------- */

static int fits13(long off) { return off >= -4096 && off <= 4095; }

/* A slot's offset from %fp, which never moves. */
static long fpo(const struct sparc_fn *F, long off)
{
    return off - F->frame;
}

static void ld_fp(struct sparc_fn *F, int reg, long o, int size, int sign)
{
    if (fits13(o)) {
        sparc_load(F->t, reg, SP_FP, (int)o, size, sign);
        return;
    }
    sparc_li(F->t, FAR, o);
    sparc_load_rr(F->t, reg, SP_FP, FAR, size, sign);
}

static void st_fp(struct sparc_fn *F, int reg, long o, int size)
{
    if (fits13(o)) {
        sparc_store(F->t, reg, SP_FP, (int)o, size);
        return;
    }
    sparc_li(F->t, FAR, o);
    sparc_store_rr(F->t, reg, SP_FP, FAR, size);
}

static void ld_sp(struct sparc_fn *F, int reg, long off, int size, int sign)
{
    ld_fp(F, reg, fpo(F, off), size, sign);
}

static void st_sp(struct sparc_fn *F, int reg, long off, int size)
{
    st_fp(F, reg, fpo(F, off), size);
}

/* A store into the OUTGOING area, at the live %sp: the callee finds its
 * stack arguments at its own %fp, which after a VLA is not this frame's
 * %sp at entry. */
static void st_out(struct sparc_fn *F, int reg, long off, int size)
{
    if (fits13(off)) {
        sparc_store(F->t, reg, SP_SP, (int)off, size);
        return;
    }
    sparc_li(F->t, FAR, off);
    sparc_store_rr(F->t, reg, SP_SP, FAR, size);
}

/* %fp + o, into `reg`. */
static void addr_fp(struct sparc_fn *F, int reg, long o)
{
    if (fits13(o)) {
        sparc_alu_imm(F->t, SP_ADD, reg, SP_FP, o);
        return;
    }
    sparc_li(F->t, reg, o);
    sparc_alu(F->t, SP_ADD, reg, SP_FP, reg);
}

static void addr_sp(struct sparc_fn *F, int reg, long off)
{
    addr_fp(F, reg, fpo(F, off));
}

/* reg = base + o (base may be reg; FAR builds a large o) */
static void addr_off_into(struct sparc_fn *F, int reg, int base, long o)
{
    if (o == 0) {
        if (reg != base)
            sparc_mov(F->t, reg, base);
    } else if (fits13(o)) {
        sparc_alu_imm(F->t, SP_ADD, reg, base, o);
    } else {
        sparc_li(F->t, FAR, o);
        sparc_alu(F->t, SP_ADD, reg, base, FAR);
    }
}

static int in_reg(const struct sparc_fn *F, int v)
{
    return F->loc && v >= 0 && v < F->fn->nvregs && F->loc[v] >= 0;
}

static long sslot(const struct sparc_fn *F, int v)
{
    if (v < 0 || v >= F->fn->nvregs || F->slot[v] < 0)
        internal_error("sparc: %s: a path addresses vreg %d's slot, and it "
                       "has none", F->fn->name, v);
    return F->slot[v];
}

static int is_wide(const struct sparc_fn *F, int v)
{
    return F->wide && v >= 0 && v < F->fn->nvregs && F->wide[v];
}

/* A 64-bit vreg read or written at 32 bits is its LOW word: PLO of its
 * pair, or the slot's second word. */
static int reg_of(const struct sparc_fn *F, int v)
{
    return is_wide(F, v) ? PLO(F->loc[v]) : F->loc[v];
}

static long slot32(const struct sparc_fn *F, int v)
{
    return sslot(F, v) + (is_wide(F, v) ? WLO : 0);
}

/* Where variable v's OBJECT is. A variable is also read and written as a
 * whole word at its slot, and big-endian a narrow value's low bytes are
 * the word's LAST, so a char or short variable lives at slot + 4 - size
 * (the MIPS backend's rule for mips-none-elf). */
static long obj_slot(const struct sparc_fn *F, int v)
{
    if (v < F->fn->nvars) {
        const struct ir_local *L = &F->fn->locals[v];
        if (L->is_int_or_ptr && L->size > 0 && L->size < 4)
            return sslot(F, v) + 4 - L->size;
    }
    return sslot(F, v);
}

static long var_slot(const struct sparc_fn *F, int v, int size)
{
    long vs = v < F->fn->nvars ? F->fn->locals[v].size
            : is_wide(F, v) ? 8 : 4;
    return obj_slot(F, v) + (vs > size ? vs - size : 0);
}

static void rd(struct sparc_fn *F, int v, int reg)
{
    if (in_reg(F, v)) {
        if (reg_of(F, v) != reg)
            sparc_mov(F->t, reg, reg_of(F, v));
        return;
    }
    ld_sp(F, reg, slot32(F, v), 4, 0);
}

static int rdr(struct sparc_fn *F, int v, int scratch)
{
    if (in_reg(F, v))
        return reg_of(F, v);
    ld_sp(F, scratch, slot32(F, v), 4, 0);
    return scratch;
}

static int wreg(struct sparc_fn *F, int v, int scratch)
{
    return in_reg(F, v) ? reg_of(F, v) : scratch;
}

static void wrote(struct sparc_fn *F, int v, int reg)
{
    if (in_reg(F, v)) {
        if (reg_of(F, v) != reg)
            sparc_mov(F->t, reg_of(F, v), reg);
        return;
    }
    if (v < 0 || F->slot[v] < 0)
        return;
    st_sp(F, reg, slot32(F, v), 4);
}

static void wr(struct sparc_fn *F, int v, int reg)
{
    wrote(F, v, reg);
}

/* dl <- sl and dh <- sh as one parallel move. */
static void mv2(struct sparc_fn *F, int dl, int sl, int dh, int sh)
{
    if (dl == sh && dh == sl) {
        if (dl == sl) return;
        sparc_mov(F->t, SCR, sl);
        sparc_mov(F->t, dh, sh);
        sparc_mov(F->t, dl, SCR);
        return;
    }
    if (dl == sh) {                    /* dh first, before sh is lost */
        if (dh != sh) sparc_mov(F->t, dh, sh);
        if (dl != sl) sparc_mov(F->t, dl, sl);
        return;
    }
    if (dl != sl) sparc_mov(F->t, dl, sl);
    if (dh != sh) sparc_mov(F->t, dh, sh);
}

static void rd64(struct sparc_fn *F, int v, int lo, int hi)
{
    if (in_reg(F, v)) {
        mv2(F, lo, PLO(F->loc[v]), hi, PHI(F->loc[v]));
        return;
    }
    ld_sp(F, lo, sslot(F, v) + WLO, 4, 0);
    ld_sp(F, hi, sslot(F, v) + WHI, 4, 0);
}

static void wr64(struct sparc_fn *F, int v, int lo, int hi)
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

static void operand_b(struct sparc_fn *F, const struct ir_ins *i, int reg)
{
    if (i->imm_b)
        sparc_li(F->t, reg, imm_val(i));
    else
        rd(F, i->b, reg);
}

static void operand_b64(struct sparc_fn *F, const struct ir_ins *i,
                        int lo, int hi)
{
    if (i->imm_b) {
        sparc_li(F->t, lo, (long long)(i->imm & 0xffffffffL));
        sparc_li(F->t, hi, (long long)((i->imm >> 32) & 0xffffffffL));
    } else {
        rd64(F, i->b, lo, hi);
    }
}

/* Sign- or zero-extend the low `size` bytes of rs: shifts up and down, or
 * an `and` for a byte. */
static void ext_reg(struct sparc_fn *F, int rdst, int rs, int size, int sign)
{
    if (size >= 4) {
        if (rdst != rs)
            sparc_mov(F->t, rdst, rs);
        return;
    }
    if (!sign && size == 1) {
        sparc_alu_imm(F->t, SP_AND, rdst, rs, 0xff);
        return;
    }
    sparc_alu_imm(F->t, SP_SLL, rdst, rs, 32 - 8 * size);
    sparc_alu_imm(F->t, sign ? SP_SRA : SP_SRL, rdst, rdst, 32 - 8 * size);
}

/* ---- loads and stores that may be misaligned -----------------------------
 *
 * A halfword or word access to an address that is not a multiple of its
 * size traps. C promises alignment everywhere but a packed structure's
 * member, and irgen marks the accesses it can promise (ir_ins.natural);
 * the rest are built from bytes, the first the most significant. */
static void ld_any(struct sparc_fn *F, int rt, int base, int off, int size,
                   int sign, int aligned)
{
    struct code *t = F->t;
    int r;
    if (aligned || size == 1) {
        sparc_load(t, rt, base, off, size, sign);
        return;
    }
    r = rt == base ? SCR2 : rt;
    sparc_load(t, r, base, off, 1, size == 2 && sign);
    for (int b = 1; b < size; b++) {
        sparc_load(t, SCR, base, off + b, 1, 0);
        sparc_alu_imm(t, SP_SLL, r, r, 8);
        sparc_alu(t, SP_OR, r, r, SCR);
    }
    if (size == 4 || !sign) {
        if (r != rt)
            sparc_mov(t, rt, r);
        return;
    }
    if (r != rt)
        sparc_mov(t, rt, r);
}

static void st_any(struct sparc_fn *F, int rt, int base, int off, int size,
                   int aligned)
{
    struct code *t = F->t;
    if (aligned || size == 1) {
        sparc_store(t, rt, base, off, size);
        return;
    }
    for (int b = 0; b < size; b++) {
        int sh = 8 * (size - 1 - b);
        if (sh) {
            sparc_alu_imm(t, SP_SRL, SCR2, rt, sh);
            sparc_store(t, SCR2, base, off + b, 1);
        } else {
            sparc_store(t, rt, base, off + b, 1);
        }
    }
}

/* ---- delay slots -----------------------------------------------------------
 *
 * A transfer's delay slot holds an instruction from just before it, when
 * that is safe, rather than a nop (the MIPS backend's fill, extended to
 * look a few instructions back). The slot runs after the transfer is
 * decided and before its target, on both paths of a branch, so taking X
 * out of the stream
 *
 *     X ; C1 .. Ck ; B ; nop        ->        C1 .. Ck ; B ; X
 *
 * keeps the program when
 *
 *   - X and every Ci are ordinary computations, loads or stores
 *     (slot_decode; a transfer, save/restore, a trap, %y's write, an
 *     atomic, stbar or anything not decoded is a wall the search stops
 *     at), and X is not a nop;
 *   - X commutes with every Ci: neither writes what the other reads or
 *     writes -- registers, the integer condition codes and %y alike --
 *     and they are not two memory accesses one of which is a store;
 *   - B does not read what X writes: a conditional branch reads the
 *     condition codes, so a cmp/subcc/orcc never moves into its slot and
 *     no addx/subx moves across one; jmpl reads its target register;
 *   - B links (call, jmpl into %o7) and X neither reads nor writes %o7,
 *     which B has already written when the slot runs; X never writes
 *     %sp, %fp or %i7;
 *   - X is not in a delay slot itself (the word before it is no transfer);
 *   - nothing may jump into the window: no label, landing, loop top,
 *     other transfer, prologue or data lies after X (F->barrier), and no
 *     relocation site is at X or a Ci (each would move).
 *
 * A label AT X is fine: a jump there now runs C1..Ck, B and then X, and
 * none of them depends on X. */

/* The two pseudo-registers past %i7: the integer condition codes and %y. */
#define R_ICC (1ULL << 32)
#define R_Y   (1ULL << 33)
#define R_O7  (1ULL << SP_O7)

struct sdec {
    unsigned long long rd, wr;    /* registers read and written */
    int mem, store;               /* a memory access; a store */
};

/* What a word reads and writes; 0 when it may not move (or be moved
 * across). */
static int slot_decode(unsigned long w, struct sdec *d)
{
    int op = (int)(w >> 30) & 3, rd = (int)(w >> 25) & 31,
        op3 = (int)(w >> 19) & 63, rs1 = (int)(w >> 14) & 31,
        imm = (int)(w >> 13) & 1, rs2 = (int)w & 31;
    unsigned long long R = 0, W = 0;
    d->mem = d->store = 0;
    if (op == 0) {                                  /* format 2 */
        if (((w >> 22) & 7) != 4 || rd == 0)        /* only sethi, not nop */
            return 0;
        W = 1ULL << rd;
    } else if (op == 2) {
        R = (1ULL << rs1) | (imm ? 0 : 1ULL << rs2);
        W = 1ULL << rd;
        switch (op3) {
        case SP_ADD: case SP_AND: case SP_OR: case SP_XOR: case SP_SUB:
        case SP_ANDN: case SP_ORN: case SP_XNOR:
        case SP_SLL: case SP_SRL: case SP_SRA:
            break;
        case SP_ADDX: case SP_SUBX:
            R |= R_ICC; break;
        case SP_UMUL: case SP_SMUL:
            W |= R_Y; break;
        case SP_UDIV: case SP_SDIV:
            R |= R_Y; break;
        case SP_ADDCC: case SP_ANDCC: case SP_ORCC: case SP_XORCC:
        case SP_SUBCC: case SP_ANDNCC: case SP_ORNCC: case SP_XNORCC:
            W |= R_ICC; break;
        case SP_ADDXCC: case SP_SUBXCC:
            R |= R_ICC; W |= R_ICC; break;
        case SP_UMULCC: case SP_SMULCC:
            W |= R_Y | R_ICC; break;
        case SP_UDIVCC: case SP_SDIVCC:
            R |= R_Y; W |= R_ICC; break;
        case 0x28:                                  /* rd %y (not stbar) */
            if (rs1 != 0)
                return 0;
            R = R_Y; break;
        default:
            return 0;
        }
    } else if (op == 3) {
        R = (1ULL << rs1) | (imm ? 0 : 1ULL << rs2);
        d->mem = 1;
        switch (op3) {
        case 0x00: case 0x01: case 0x02: case 0x09: case 0x0a:   /* loads */
            W = 1ULL << rd; break;
        case 0x03:                                               /* ldd */
            W = 3ULL << rd; break;
        case 0x04: case 0x05: case 0x06:                         /* stores */
            R |= 1ULL << rd; d->store = 1; break;
        case 0x07:                                               /* std */
            R |= 3ULL << rd; d->store = 1; break;
        default:
            return 0;
        }
    } else {
        return 0;                                   /* call */
    }
    d->rd = R & ~1ULL;
    d->wr = W & ~1ULL;
    return 1;
}

/* A delayed transfer: a Bicc, call or jmpl (rett too) -- the word after
 * it is its slot. */
static int is_dcti(unsigned long w)
{
    int op = (int)(w >> 30) & 3, op3 = (int)(w >> 19) & 63;
    return (op == 0 && ((w >> 22) & 7) == 2) || op == 1 ||
           (op == 2 && (op3 == 0x38 || op3 == 0x39));
}

/* The highest offset any relocation site or label fix-up names, or -1:
 * every list is appended in offset order, so the last of each. */
static int last_site(const struct sparc_fn *F)
{
    const struct sparc_sites *st = F->st;
    int m = -1;
    if (st->next && st->ext[st->next - 1].patch_off > m)
        m = st->ext[st->next - 1].patch_off;
    if (st->nstr && st->str[st->nstr - 1].patch_off > m)
        m = st->str[st->nstr - 1].patch_off;
    if (st->ng && st->g[st->ng - 1].patch_off > m)
        m = st->g[st->ng - 1].patch_off;
    if (st->nf && st->f[st->nf - 1].patch_off > m)
        m = st->f[st->nf - 1].patch_off;
    if (F->nfix && F->fix[F->nfix - 1].at > m)
        m = F->fix[F->nfix - 1].at;
    return m;
}

/* The offset here, as a target something will branch to: nothing before
 * it may be taken into a later slot past it. */
static int mark_here(struct sparc_fn *F)
{
    F->barrier = F->t->len;
    return F->t->len;
}

#define SLOT_LOOK 4             /* X and up to three instructions after it */

/* Before a transfer that reads `reads` (and writes %o7 when `links`):
 * take an instruction out of the stream if one may go in the slot.
 * Returns it, or -1 for a nop slot. */
static long take_slot(struct sparc_fn *F, unsigned long long reads,
                      int links)
{
    struct code *t = F->t;
    struct sdec c[SLOT_LOOK];
    int nc = 0, lim = last_site(F);
    if (!F->fill)
        return -1;
    for (int at = t->len - 4; at >= F->barrier && at > lim &&
                              nc < SLOT_LOOK; at -= 4) {
        unsigned long w = sparc_rdw(t, at);
        struct sdec x;
        int ok;
        if (!slot_decode(w, &x))
            return -1;                      /* a wall */
        ok = !(x.wr & reads) &&
             !(links && ((x.rd | x.wr) & R_O7)) &&
             !(x.wr & ((1ULL << SP_SP) | (1ULL << SP_FP) |
                       (1ULL << SP_I7))) &&
             at >= 4 && !is_dcti(sparc_rdw(t, at - 4));
        for (int k = 0; ok && k < nc; k++)
            ok = !(x.wr & (c[k].rd | c[k].wr)) && !(x.rd & c[k].wr) &&
                 !(x.mem && c[k].mem && (x.store || c[k].store));
        if (ok) {
            memmove(t->p + at, t->p + at + 4, (size_t)(t->len - at - 4));
            t->len -= 4;
            return (long)w;
        }
        c[nc++] = x;
    }
    return -1;
}

/* The slot after a transfer: what take_slot took, or a nop; and nothing
 * before this point may be moved again. */
static void put_slot(struct sparc_fn *F, long w)
{
    if (w >= 0)
        sparc_w(F->t, (unsigned long)w);
    else
        sparc_nop(F->t);
    F->barrier = F->t->len;
}

/* The condition codes a branch on `cond` reads: none for ba and bn. */
static unsigned long long cond_reads(int cond)
{
    return cond == SP_BA || cond == SP_BN ? 0 : R_ICC;
}

/* ---- branches, with their delay slots ------------------------------------
 *
 * A branch to a label is resolved when the function ends, with no
 * relocation: +-8 MiB from the branch, and a function whose branches
 * reach further is refused. */
static int br_place(struct sparc_fn *F, int cond)
{
    long slot = take_slot(F, cond_reads(cond), 0);
    int at = sparc_b_placeholder(F->t, cond, 0);
    put_slot(F, slot);
    return at;
}

static void br_land(struct sparc_fn *F, int at)
{
    F->barrier = F->t->len;          /* a landing: a target */
    if (!sparc_patch_b(F->t, at, F->t->len))
        internal_error("sparc: %s: a branch inside one operation does not "
                       "reach", F->fn->name);
}

static void br_back(struct sparc_fn *F, int at, int target)
{
    if (!sparc_patch_b(F->t, at, target))
        internal_error("sparc: %s: a loop inside one operation does not "
                       "reach", F->fn->name);
}

static void want_label(struct sparc_fn *F, int at, int label)
{
    if (F->nfix == F->capfix) {
        F->capfix = F->capfix ? F->capfix * 2 : 16;
        F->fix = xrealloc(F->fix, (size_t)F->capfix * sizeof *F->fix);
    }
    F->fix[F->nfix].at = at;
    F->fix[F->nfix].label = label;
    F->fix[F->nfix].base = -1;
    F->nfix++;
}

/* Is any relocation site of the unit at `off`? The lists are in offset
 * order, so each is read back from its end. */
static int site_at(const struct sparc_fn *F, int off)
{
    const struct sparc_sites *st = F->st;
    for (int k = st->next - 1; k >= 0 && st->ext[k].patch_off >= off; k--)
        if (st->ext[k].patch_off == off) return 1;
    for (int k = st->nstr - 1; k >= 0 && st->str[k].patch_off >= off; k--)
        if (st->str[k].patch_off == off) return 1;
    for (int k = st->ng - 1; k >= 0 && st->g[k].patch_off >= off; k--)
        if (st->g[k].patch_off == off) return 1;
    for (int k = st->nf - 1; k >= 0 && st->f[k].patch_off >= off; k--)
        if (st->f[k].patch_off == off) return 1;
    for (int k = 0; k < F->nfix; k++)
        if (F->fix[k].at == off) return 1;
    return 0;
}

/* A branch BACK to a label already placed, whose slot found nothing to
 * take: the slot gets a copy of the label's first instruction and the
 * branch goes to the one after it -- the classic target fill, annulled
 * when conditional so the copy runs only when it is taken. Either way
 * the copy runs exactly where the label's own would have, before the
 * rest of the target; it needs only to be an ordinary instruction (not a
 * transfer's slot, not a relocation site) with one after it. As short
 * as a nop, and one instruction less per trip round a loop. */
static int target_fill(struct sparc_fn *F, int cond, int label)
{
    struct code *t = F->t;
    int L = F->label_off[label], at;
    struct sdec d;
    unsigned long w;
    if (!F->fill || L < 0 || L + 8 > t->len || L < F->fn->src->code_off + 4)
        return 0;
    w = sparc_rdw(t, L);
    if (!slot_decode(w, &d) || is_dcti(sparc_rdw(t, L - 4)) || site_at(F, L))
        return 0;
    at = sparc_b_placeholder(t, cond, cond != SP_BA);
    sparc_w(t, w);
    F->barrier = t->len;
    if (!sparc_patch_b(t, at, L + 4))
        sparc_refuse(F, NULL, "a branch beyond +-8 MiB (the function is too "
                              "large)");
    return 1;
}

static void branch_to(struct sparc_fn *F, int cond, int label)
{
    long slot = take_slot(F, cond_reads(cond), 0);
    int at;
    if (slot < 0 && target_fill(F, cond, label))
        return;
    at = sparc_b_placeholder(F->t, cond, 0);
    put_slot(F, slot);
    want_label(F, at, label);
}

static void jump_to(struct sparc_fn *F, int label)
{
    branch_to(F, SP_BA, label);
}

/* `ret; restore`, the restore taking the instruction before it when that
 * is an add (or a mov, `or %g0`) into %i0 or %i1: restore adds its
 * operands in this window and writes its rd in the caller's, where %i0
 * is %o0 -- so `mov x, %i0; ret; restore` is `ret; restore x, %g0, %o0`.
 * The same rules as a slot: nothing jumps between it and the ret, it is
 * not in a slot itself, not a relocation site. restore_fusable returns
 * the fused restore, or -1. */
static long restore_fusable(const struct sparc_fn *F)
{
    const struct code *t = F->t;
    int at = t->len - 4, rd, op3, rs1, imm, rs2;
    unsigned long w;
    if (!F->fill || at < F->barrier || at <= last_site(F) || at < 4 ||
        is_dcti(sparc_rdw(t, at - 4)))
        return -1;
    w = sparc_rdw(t, at);
    rd = (int)(w >> 25) & 31; op3 = (int)(w >> 19) & 63;
    rs1 = (int)(w >> 14) & 31; imm = (int)(w >> 13) & 1;
    rs2 = (int)w & 31;
    if ((w >> 30) != 2 || (rd != SP_I0 && rd != SP_I1) ||
        !(op3 == SP_ADD ||
          (op3 == SP_OR && (rs1 == SP_G0 || (!imm && rs2 == SP_G0)))))
        return -1;
    return (long)((w & ~((31UL << 25) | (63UL << 19))) |
                  ((unsigned long)rd_caller(rd) << 25) | (0x3dUL << 19));
}

static void ret_restore(struct sparc_fn *F)
{
    struct code *t = F->t;
    long r;
    if (g_sp_leaf) {
        /* `retl` once %i7 is %o7 (leaf_fix), with a filled slot */
        long slot = take_slot(F, 1ULL << SP_I7, 0);
        sparc_jmpl(t, SP_G0, SP_I7, 8);
        put_slot(F, slot);
        return;
    }
    r = restore_fusable(F);
    if (r >= 0)
        t->len -= 4;
    sparc_jmpl(t, SP_G0, SP_I7, fn_sret(F->fn) ? 12 : 8);
    if (r >= 0)
        sparc_w(t, (unsigned long)r);
    else
        sparc_restore(t, SP_G0, SP_G0, SP_G0);
    F->barrier = t->len;
}

/* A return from inside the body: `ret; restore` in place when the
 * restore takes the value's move, else a branch to the epilogue whose
 * slot may take something, else `ret; restore` anyway -- as short as
 * `ba; nop`, and one transfer instead of two. */
static void ret_or_jump(struct sparc_fn *F)
{
    long slot = -1;
    if (!g_sp_leaf && restore_fusable(F) < 0)
        slot = take_slot(F, 0, 0);
    if (slot >= 0) {
        int at = sparc_b_placeholder(F->t, SP_BA, 0);
        put_slot(F, slot);
        want_label(F, at, F->fn->nlabels);
        return;
    }
    ret_restore(F);
}

/* The last instruction emitted, at `at`, when it may be rewritten in
 * place: no label, landing or transfer after it (F->barrier), not a
 * relocation site, and not in a transfer's delay slot (where it might
 * not run). -1 otherwise. */
static int last_rewritable(const struct sparc_fn *F)
{
    const struct code *t = F->t;
    int at = t->len - 4;
    if (at < F->barrier || at <= last_site(F) || at < 4 ||
        is_dcti(sparc_rdw(t, at - 4)))
        return -1;
    return at;
}

/* Set Z from r == 0 for a be/bne: `tst r` (orcc r, %g0, %g0) -- unless
 * the instruction just emitted computed r by an add, sub or a logical
 * operation, which then becomes its cc form and sets Z from the same
 * value (the other codes differ, so only for Z). */
static void test_zero(struct sparc_fn *F, int r)
{
    struct code *t = F->t;
    int at = F->fill ? last_rewritable(F) : -1;
    if (at >= 0 && r != SP_G0) {
        unsigned long w = sparc_rdw(t, at);
        int op3 = (int)(w >> 19) & 63;
        /* add and or xor sub andn orn xnor: op3 0..7, cc forms 0x10.. */
        if ((w >> 30) == 2 && (int)(w >> 25 & 31) == r && op3 <= SP_XNOR) {
            sparc_wrw(t, at, w | (0x10UL << 19));
            return;
        }
    }
    sparc_alu(t, SP_ORCC, SP_G0, r, SP_G0);
}

/* A narrow load just emitted into r, extended into d: the load itself
 * with the extension asked for, into d. Only when r is dead after (the
 * caller checks the loaded value has no other reader). */
static int load_ext(struct sparc_fn *F, int r, int d, int size, int sign)
{
    struct code *t = F->t;
    int at = F->fill ? last_rewritable(F) : -1, op3;
    unsigned long w;
    if (at < 0)
        return 0;
    w = sparc_rdw(t, at);
    op3 = (int)(w >> 19) & 63;
    if ((w >> 30) != 3 || (int)(w >> 25 & 31) != r ||
        !(size == 1 ? op3 == 0x01 || op3 == 0x09
                    : op3 == 0x02 || op3 == 0x0a))
        return 0;
    op3 = size == 1 ? (sign ? 0x09 : 0x01) : (sign ? 0x0a : 0x02);
    sparc_wrw(t, at, (w & ~((31UL << 25) | (63UL << 19))) |
                     ((unsigned long)d << 25) | ((unsigned long)op3 << 19));
    return 1;
}

/* cmp a, b (or an immediate): subcc into %g0. */
static void cmp_rr(struct sparc_fn *F, int a, int b)
{
    sparc_alu(F->t, SP_SUBCC, SP_G0, a, b);
}

/* The Bicc condition for an IR predicate. */
static int pred_cond(enum binop pred, int sign)
{
    switch (pred) {
    case B_EQ: return SP_BE;
    case B_NE: return SP_BNE;
    case B_LT: return sign ? SP_BL : SP_BCS;
    case B_GE: return sign ? SP_BGE : SP_BCC;
    case B_GT: return sign ? SP_BG : SP_BGU;
    default:   return sign ? SP_BLE : SP_BLEU;           /* B_LE */
    }
}

/* d = the condition codes' `cond`, 0 or 1: `mov 0, d` and an annulled
 * branch over nothing whose slot -- run only when it is taken -- is `mov
 * 1, d`. The mov after the compare leaves the codes alone, so d may be
 * either compared register. */
static void cc_to_reg(struct sparc_fn *F, int cond, int d)
{
    struct code *t = F->t;
    if (cond == SP_BCS) {                        /* the carry itself */
        sparc_alu_imm(t, SP_ADDX, d, SP_G0, 0);
        return;
    }
    if (cond == SP_BCC) {                        /* 1 - carry */
        sparc_alu_imm(t, SP_SUBX, d, SP_G0, -1);
        return;
    }
    sparc_mov(t, d, SP_G0);
    sparc_w(t, sparc_enc_branch(cond, 1, 8));
    sparc_alu_imm(t, SP_OR, d, SP_G0, 1);
}

/* ---- site lists ----------------------------------------------------------- */

static void note_ext(struct sparc_sites *st, int at, struct func *callee)
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

static void note_str(struct sparc_sites *st, int at, int idx, enum reloc_kind k)
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

static void note_glob(struct sparc_sites *st, int at, struct global *g,
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

static void note_fn(struct sparc_sites *st, int at, struct func *target,
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

/* `sethi %hi(sym), rd; or rd, %lo(sym), rd`: an absolute address in two
 * halves (R_SPARC_HI22 then R_SPARC_LO10). Returns the sethi's offset. */
static int abs_pair(struct sparc_fn *F, int rd)
{
    int at = F->t->len;
    sparc_sethi(F->t, rd, 0);
    sparc_alu_imm(F->t, SP_OR, rd, rd, 0);
    return at;
}

/* A call: `call` with an R_SPARC_WDISP30 and its slot filled. Every
 * call is relocated, even to a function in this unit. */
static void call_sym(struct sparc_fn *F, struct func *callee)
{
    long slot = take_slot(F, 0, 1);
    int at = F->t->len;
    sparc_call(F->t);
    note_ext(F->st, at, callee);
    put_slot(F, slot);
}

/* The runtime helpers, interned by name. */
static struct func **g_helpers;
static int g_nhelpers, g_caphelpers;

static struct func *helper_fn(const char *name)
{
    struct func *h = NULL;
    for (int k = 0; k < g_nhelpers; k++)
        if (strcmp(g_helpers[k]->name, name) == 0)
            return g_helpers[k];
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
    return h;
}

static void call_helper(struct sparc_fn *F, const char *name)
{
    call_sym(F, helper_fn(name));
}

/* ---- soft float ------------------------------------------------------------
 *
 * Every floating-point operation is a libgcc call (lib/rt/softfp.c), a
 * float in one integer register and a double in a pair, high word first,
 * results in %o0 (%o0:%o1). */
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

/* ---- constants made where they are passed --------------------------------
 *
 * A constant whose one reader passes it -- a call's register argument,
 * a soft-float or 64-bit-divide helper's operand -- is not made where the
 * IR defines it but in its argument register, after the argument setup's
 * parallel move: the register it would have held between the two is
 * free, and the move does not have to shuffle it (fdlibm's coefficients
 * went %o0 -> %l7 -> %o2 around each helper). One whose reader stores it
 * is made at the store, and a zero is %g0 there. Only a temp with exactly
 * one definition, an IR_CONST, and exactly one read, at such a place. */
static int is_remat(const struct sparc_fn *F, int v)
{
    return F->remat && v >= 0 && v < F->fn->nvregs && F->remat[v];
}

static void const_remat(struct sparc_fn *F)
{
    struct ir_func *fn = F->fn;
    int nv = fn->nvregs;
    int *ndef, *site;
    if (!F->usecnt || !nv)
        return;
    ndef = xcalloc((size_t)nv, sizeof *ndef);
    site = xcalloc((size_t)nv, sizeof *site);
    F->remat = xcalloc((size_t)nv, 1);
    F->rematv = xcalloc((size_t)nv, sizeof *F->rematv);
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        int d = ra_ins_def(i);
        if (d >= 0 && d < nv)
            ndef[d]++;
        if (i->op == IR_CALL) {
            int word = 0;
            struct argplace pl;
            for (int k = 0; k < i->nargs; k++) {
                const struct ir_arg *a = &i->argv[k];
                place_arg(a, k == 0 && i->sret_first, &word, &pl);
                if (!pl.byref && !pl.nstk && a->vreg >= 0 && a->vreg < nv)
                    site[a->vreg]++;
            }
        } else if (i->op == IR_STORE && i->size <= 8 && i->b >= 0 &&
                   i->b < nv) {
            site[i->b]++;
        } else if (i->op == IR_STVAR && i->size <= 4 && i->a >= 0 &&
                   i->a < nv && !is_wide(F, i->a)) {
            site[i->a]++;
        } else if (i->op != IR_CALL && sparc_op_calls_helper(i) &&
                   !i->imm_b && i->w != 16 && i->size != 16 &&
                   (i->op == IR_ADD || i->op == IR_SUB || i->op == IR_MUL ||
                    i->op == IR_DIV || i->op == IR_MOD || i->op == IR_CMP)) {
            if (i->a >= 0 && i->a < nv) site[i->a]++;
            if (i->b >= 0 && i->b < nv) site[i->b]++;
        }
    }
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        int v = i->dst;
        if (i->op != IR_CONST || v < fn->nvars || v >= nv || ndef[v] != 1 ||
            F->usecnt[v] != 1 || site[v] != 1 || is16(F, v))
            continue;
        F->remat[v] = 1;
        F->rematv[v] = is_wide(F, v) ? (long long)i->imm : imm_val(i);
    }
    free(ndef);
    free(site);
}

/* The register holding word `hi` (1: the high one of a 64-bit constant)
 * of rematerialized constant v: %g0 for a zero, else `scratch` loaded. */
static int remat_reg(struct sparc_fn *F, int v, int scratch, int hi)
{
    long long c = F->rematv[v];
    unsigned long w = (unsigned long)((hi && is_wide(F, v) ? c >> 32 : c) &
                                      0xffffffffL);
    if (!w)
        return SP_G0;
    sparc_li(F->t, scratch, (long long)w);
    return scratch;
}

/* Put n vregs into the registers a helper (or a call) expects, all at
 * once: the register-to-register edges as one parallel move (SCR breaks
 * a cycle), then the loads, which only write. `half` (may be NULL) picks
 * the low (0) or high (1) word of a 64-bit value; without it a vreg is
 * read at 32 bits, which of a 64-bit one is its low word. */
static void set_args_half(struct sparc_fn *F, const int *dstreg,
                          const int *vreg, const int *half, int n)
{
    int pd[RA_MAXPOOL], ps[RA_MAXPOOL], npm = 0;

    for (int k = 0; k < n; k++)
        if (in_reg(F, vreg[k]) && !is_remat(F, vreg[k])) {
            pd[npm] = dstreg[k];
            ps[npm] = !half ? reg_of(F, vreg[k])
                    : !is_wide(F, vreg[k]) ? F->loc[vreg[k]]
                    : half[k] ? PHI(F->loc[vreg[k]]) : PLO(F->loc[vreg[k]]);
            npm++;
        }
    if (npm) {
        int od[RA_MAXPOOL * 2], os[RA_MAXPOOL * 2];
        int m = ra_parallel_move(pd, ps, npm, SCR, od, os,
                                 (int)(sizeof od / sizeof od[0]));
        if (m < 0)
            internal_error("sparc: an argument setup is not a well-formed "
                           "move");
        for (int k = 0; k < m; k++)
            sparc_mov(F->t, od[k], os[k]);
    }
    for (int k = 0; k < n; k++)
        if (!in_reg(F, vreg[k]) && !is_remat(F, vreg[k]))
            ld_sp(F, dstreg[k],
                  !half ? slot32(F, vreg[k])
                  : !is_wide(F, vreg[k]) ? slot32(F, vreg[k])
                  : sslot(F, vreg[k]) + (half[k] ? WHI : WLO), 4, 0);
    for (int k = 0; k < n; k++)
        if (is_remat(F, vreg[k])) {
            long long c = F->rematv[vreg[k]];
            sparc_li(F->t, dstreg[k],
                     half && half[k] && is_wide(F, vreg[k])
                         ? (long long)((c >> 32) & 0xffffffffL)
                         : (long long)(c & 0xffffffffL));
        }
}

static void set_args(struct sparc_fn *F, const int *dstreg, const int *vreg,
                     int n)
{
    set_args_half(F, dstreg, vreg, NULL, n);
}

/* Two 64-bit operands into %o0:%o1 and %o2:%o3 (vb < 0: only the first),
 * the high word first. A 32-bit vreg read as 64 here is a value whose
 * high word is not asked for: it is never one (the callers check). */
static void args64x2(struct sparc_fn *F, int va, int vb)
{
    int d[4] = { PLO(SP_O0), PHI(SP_O0), PLO(SP_O2), PHI(SP_O2) };
    int v[4], h[4] = { 0, 1, 0, 1 };
    v[0] = v[1] = va;
    v[2] = v[3] = vb;
    set_args_half(F, d, v, h, vb >= 0 ? 4 : 2);
}

static void fp_args2(struct sparc_fn *F, const struct ir_ins *i)
{
    if (i->w == 8) {
        args64x2(F, i->a, i->b);
        return;
    }
    {
        int dstreg[2], vreg[2];
        dstreg[0] = SP_O0; vreg[0] = i->a;
        dstreg[1] = SP_O1; vreg[1] = i->b;
        set_args(F, dstreg, vreg, 2);
    }
}

static void fp_result(struct sparc_fn *F, int dst, int w)
{
    if (dst < 0)
        return;
    if (w == 8) wr64(F, dst, PLO(SP_O0), PHI(SP_O0));
    else        wr(F, dst, SP_O0);
}

/* ---- comparisons into a register -------------------------------------------
 *
 * == and != are an xor and the carry of `0 - x` (set when x is nonzero);
 * the unsigned orders are the carry of the compare; the signed ones the
 * annulled-branch idiom (cc_to_reg). */
static void cmp_to_reg(struct sparc_fn *F, enum binop pred, int sign,
                       int ra, int rb, int dst)
{
    struct code *t = F->t;
    if (pred == B_EQ || pred == B_NE) {
        int x = ra;
        if (rb != SP_G0) {
            sparc_alu(t, SP_XOR, dst, ra, rb);
            x = dst;
        }
        sparc_alu(t, SP_SUBCC, SP_G0, SP_G0, x);
        if (pred == B_NE) sparc_alu_imm(t, SP_ADDX, dst, SP_G0, 0);
        else              sparc_alu_imm(t, SP_SUBX, dst, SP_G0, -1);
        return;
    }
    if (!sign && (pred == B_GT || pred == B_LE)) {
        /* swapped, so the carry answers: a > b is b < a */
        cmp_rr(F, rb, ra);
        cc_to_reg(F, pred == B_GT ? SP_BCS : SP_BCC, dst);
        return;
    }
    cmp_rr(F, ra, rb);
    cc_to_reg(F, pred_cond(pred, sign), dst);
}

/* The same against a constant k: subcc takes a signed 13-bit field.
 * Returns 0 where k does not fit, and the caller loads it. */
static int cmp_imm_to_reg(struct sparc_fn *F, enum binop pred, int sign,
                          int ra, long long k, int dst)
{
    struct code *t = F->t;
    if (!sparc_simm13_ok(k))
        return 0;
    if (pred == B_EQ || pred == B_NE) {
        int x = ra;
        if (k) {
            sparc_alu_imm(t, SP_XOR, dst, ra, k);
            x = dst;
        }
        sparc_alu(t, SP_SUBCC, SP_G0, SP_G0, x);
        if (pred == B_NE) sparc_alu_imm(t, SP_ADDX, dst, SP_G0, 0);
        else              sparc_alu_imm(t, SP_SUBX, dst, SP_G0, -1);
        return 1;
    }
    sparc_alu_imm(t, SP_SUBCC, SP_G0, ra, k);
    cc_to_reg(F, pred_cond(pred, sign), dst);
    return 1;
}

/* ---- 64-bit integers, in register pairs ----------------------------------- */

static void shift64_imm_to(struct sparc_fn *F, int op, int sign, long n,
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
        if (op == SP_SLL) {
            if (k) sparc_alu_imm(t, SP_SLL, dh, al, k);
            else if (dh != al) sparc_mov(t, dh, al);
            sparc_mov(t, dl, SP_G0);
        } else {
            if (k) sparc_alu_imm(t, sign ? SP_SRA : SP_SRL, dl, ah, k);
            else if (dl != ah) sparc_mov(t, dl, ah);
            if (sign) sparc_alu_imm(t, SP_SRA, dh, ah, 31);
            else      sparc_mov(t, dh, SP_G0);
        }
        return;
    }
    if (op == SP_SLL) {
        sparc_alu_imm(t, SP_SRL, SCR, al, (int)(32 - n));
        sparc_alu_imm(t, SP_SLL, dh, ah, (int)n);
        sparc_alu(t, SP_OR, dh, dh, SCR);
        sparc_alu_imm(t, SP_SLL, dl, al, (int)n);
    } else {
        sparc_alu_imm(t, SP_SLL, SCR, ah, (int)(32 - n));
        sparc_alu_imm(t, SP_SRL, dl, al, (int)n);
        sparc_alu(t, SP_OR, dl, dl, SCR);
        sparc_alu_imm(t, sign ? SP_SRA : SP_SRL, dh, ah, (int)n);
    }
}

/* A shift of A_LO:A_HI by the count in B_LO, in three arms (the register
 * shifts take the count's low five bits, so n == 0 is its own arm). */
static void shift64_var(struct sparc_fn *F, int left, int sign)
{
    struct code *t = F->t;
    int big, zero, done1, done2;

    sparc_alu_imm(t, SP_AND, B_LO, B_LO, 63);
    sparc_alu_imm(t, SP_ANDCC, SP_G0, B_LO, 32);
    big = br_place(F, SP_BNE);                    /* count >= 32 */
    sparc_alu(t, SP_ORCC, SP_G0, B_LO, SP_G0);
    zero = br_place(F, SP_BE);
    /* 0 < count < 32: B_HI = -count, which the shifts read as 32 - count */
    sparc_alu(t, SP_SUB, B_HI, SP_G0, B_LO);
    if (left) {
        sparc_alu(t, SP_SLL, A_HI, A_HI, B_LO);
        sparc_alu(t, SP_SRL, SCR, A_LO, B_HI);
        sparc_alu(t, SP_OR, A_HI, A_HI, SCR);
        sparc_alu(t, SP_SLL, A_LO, A_LO, B_LO);
    } else {
        sparc_alu(t, SP_SRL, A_LO, A_LO, B_LO);
        sparc_alu(t, SP_SLL, SCR, A_HI, B_HI);
        sparc_alu(t, SP_OR, A_LO, A_LO, SCR);
        sparc_alu(t, sign ? SP_SRA : SP_SRL, A_HI, A_HI, B_LO);
    }
    done1 = br_place(F, SP_BA);
    br_land(F, big);
    if (left) {
        sparc_alu(t, SP_SLL, A_HI, A_LO, B_LO);
        sparc_mov(t, A_LO, SP_G0);
    } else if (sign) {
        sparc_alu(t, SP_SRA, A_LO, A_HI, B_LO);
        sparc_alu_imm(t, SP_SRA, A_HI, A_HI, 31);
    } else {
        sparc_alu(t, SP_SRL, A_LO, A_HI, B_LO);
        sparc_mov(t, A_HI, SP_G0);
    }
    done2 = br_place(F, SP_BA);
    br_land(F, zero);
    br_land(F, done1);
    br_land(F, done2);
}

/* d = s OP c for one half of a 64-bit AND/OR/XOR with a constant. */
static void logic_half(struct sparc_fn *F, int op, int d, int s,
                       unsigned long c)
{
    struct code *t = F->t;
    long long sc;
    c &= 0xffffffffUL;
    sc = (long long)(int)(unsigned int)c;
    if ((op == SP_AND && c == 0xffffffffUL) || (op != SP_AND && c == 0)) {
        if (d != s) sparc_mov(t, d, s);
        return;
    }
    if (op == SP_AND && c == 0) {
        sparc_mov(t, d, SP_G0);
        return;
    }
    if (sparc_simm13_ok(sc)) {
        sparc_alu_imm(t, op, d, s, sc);
        return;
    }
    sparc_li(t, SCR, sc);
    sparc_alu(t, op, d, s, SCR);
}

static void src64(struct sparc_fn *F, int v, int slo, int shi, int *lo,
                  int *hi)
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

static void dst64(struct sparc_fn *F, int v, int *lo, int *hi)
{
    *lo = in_reg(F, v) ? PLO(F->loc[v]) : A_LO;
    *hi = in_reg(F, v) ? PHI(F->loc[v]) : A_HI;
}

/* A 64-bit comparison's condition codes, and the condition that tests
 * them: == and != as an orcc of the halves' xors (Z), the orders as
 * subcc/subxcc (C for unsigned, N^V signed), GT and LE by swapping the
 * operands. */
static int cmp64_cc(struct sparc_fn *F, const struct ir_ins *i,
                    enum binop pred, int sign)
{
    struct code *t = F->t;
    int al, ah, bl, bh;

    src64(F, i->a, A_LO, A_HI, &al, &ah);
    if (i->imm_b && i->imm == 0 && (pred == B_EQ || pred == B_NE)) {
        sparc_alu(t, SP_ORCC, SP_G0, al, ah);
        return pred == B_EQ ? SP_BE : SP_BNE;
    }
    if (i->imm_b) {
        operand_b64(F, i, B_LO, B_HI);
        bl = B_LO; bh = B_HI;
    } else {
        src64(F, i->b, B_LO, B_HI, &bl, &bh);
    }
    if (pred == B_EQ || pred == B_NE) {
        sparc_alu(t, SP_XOR, SCR, al, bl);
        sparc_alu(t, SP_XOR, SCR2, ah, bh);
        sparc_alu(t, SP_ORCC, SP_G0, SCR, SCR2);
        return pred == B_EQ ? SP_BE : SP_BNE;
    }
    if (pred == B_GT || pred == B_LE) {
        int x;
        x = al; al = bl; bl = x;
        x = ah; ah = bh; bh = x;
        pred = pred == B_GT ? B_LT : B_GE;
    }
    sparc_alu(t, SP_SUBCC, SP_G0, al, bl);
    sparc_alu(t, SP_SUBXCC, SP_G0, ah, bh);
    return pred_cond(pred, sign);
}

static int gen_ins64(struct sparc_fn *F, int n)
{
    struct ir_func *fn = F->fn;
    struct ir_ins *i = &fn->ins[n];
    struct code *t = F->t;

    switch (i->op) {
    case IR_CONST: {
        int lo, hi;
        if (is_remat(F, i->dst))
            return 1;                    /* made where it is passed */
        dst64(F, i->dst, &lo, &hi);
        sparc_li(t, lo, (long long)(i->imm & 0xffffffffL));
        sparc_li(t, hi, (long long)((i->imm >> 32) & 0xffffffffL));
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
    case IR_ADD: case IR_SUB: {
        int al, ah, dl, dh;
        int add = i->op == IR_ADD;
        src64(F, i->a, A_LO, A_HI, &al, &ah);
        if (i->imm_b || !in_reg(F, i->b)) {
            operand_b64(F, i, B_LO, B_HI);
            dst64(F, i->dst, &dl, &dh);
            sparc_alu(t, add ? SP_ADDCC : SP_SUBCC, dl, al, B_LO);
            sparc_alu(t, add ? SP_ADDXCC : SP_SUBXCC, dh, ah, B_HI);
        } else {
            int bl = PLO(F->loc[i->b]), bh = PHI(F->loc[i->b]);
            dst64(F, i->dst, &dl, &dh);
            if (dl == bh || dl == ah) {          /* the low result would
                                                  * clobber a high input */
                dl = A_LO;
                dh = A_HI;
            }
            sparc_alu(t, add ? SP_ADDCC : SP_SUBCC, dl, al, bl);
            sparc_alu(t, add ? SP_ADDXCC : SP_SUBXCC, dh, ah, bh);
        }
        wr64(F, i->dst, dl, dh);
        return 1;
    }
    case IR_AND: case IR_OR: case IR_XOR: {
        int op = i->op == IR_AND ? SP_AND : i->op == IR_OR ? SP_OR : SP_XOR;
        int al, ah, bl, bh, dl, dh;
        src64(F, i->a, A_LO, A_HI, &al, &ah);
        if (i->imm_b) {
            dst64(F, i->dst, &dl, &dh);
            if (dl == ah) { dl = A_LO; dh = A_HI; }
            logic_half(F, op, dl, al, (unsigned long)i->imm);
            logic_half(F, op, dh, ah, (unsigned long)i->imm >> 32);
            wr64(F, i->dst, dl, dh);
            return 1;
        }
        src64(F, i->b, B_LO, B_HI, &bl, &bh);
        dst64(F, i->dst, &dl, &dh);
        if (dl == ah || dl == bh) { dl = A_LO; dh = A_HI; }
        sparc_alu(t, op, dl, al, bl);
        sparc_alu(t, op, dh, ah, bh);
        wr64(F, i->dst, dl, dh);
        return 1;
    }
    case IR_MUL:
        /* (ah:al) * (bh:bl) keeping 64 bits: umul gives al*bl whole (its
         * high word in %y), and the cross terms reach only the high word */
        rd64(F, i->a, A_LO, A_HI);
        operand_b64(F, i, B_LO, B_HI);
        sparc_alu(t, SP_SMUL, A_HI, A_HI, B_LO);
        sparc_alu(t, SP_SMUL, B_HI, B_HI, A_LO);
        sparc_alu(t, SP_ADD, A_HI, A_HI, B_HI);
        sparc_alu(t, SP_UMUL, A_LO, A_LO, B_LO);
        sparc_rdy(t, SCR);
        sparc_alu(t, SP_ADD, A_HI, A_HI, SCR);
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    case IR_MULW: {
        /* umul/smul: the low word to a register and the high one to %y,
         * where `rd %y` finds it -- the operands are read before either
         * is written, so the pair may hold them */
        int ra_ = rdr(F, i->a, B_LO), rb_ = rdr(F, i->b, B_HI), dl, dh;
        dst64(F, i->dst, &dl, &dh);
        sparc_alu(t, i->sign ? SP_SMUL : SP_UMUL, dl, ra_, rb_);
        sparc_rdy(t, dh);
        wr64(F, i->dst, dl, dh);
        return 1;
    }
    case IR_NEG: {
        int al, ah, dl, dh;
        src64(F, i->a, A_LO, A_HI, &al, &ah);
        dst64(F, i->dst, &dl, &dh);
        if (dl == ah) { dl = A_LO; dh = A_HI; }
        sparc_alu(t, SP_SUBCC, dl, SP_G0, al);
        sparc_alu(t, SP_SUBX, dh, SP_G0, ah);
        wr64(F, i->dst, dl, dh);
        return 1;
    }
    case IR_BNOT: {
        int al, ah, dl, dh;
        src64(F, i->a, A_LO, A_HI, &al, &ah);
        dst64(F, i->dst, &dl, &dh);
        sparc_alu(t, SP_XNOR, dl, al, SP_G0);
        sparc_alu(t, SP_XNOR, dh, ah, SP_G0);
        wr64(F, i->dst, dl, dh);
        return 1;
    }
    case IR_SHL: case IR_SHR: {
        int sign = i->op == IR_SHR && i->sign;
        if (i->imm_b) {
            int al, ah, dl, dh;
            src64(F, i->a, A_LO, A_HI, &al, &ah);
            dst64(F, i->dst, &dl, &dh);
            if (dl != al && (dl == ah || dh == al || dh == ah)) {
                rd64(F, i->a, A_LO, A_HI);
                al = A_LO; ah = A_HI;
            }
            if (dl == al && dh != ah) {         /* never: pairs are whole */
                dl = A_LO; dh = A_HI;
            }
            shift64_imm_to(F, i->op == IR_SHL ? SP_SLL : SP_SRL, sign,
                           (long)i->imm, al, ah, dl, dh);
            wr64(F, i->dst, dl, dh);
            return 1;
        }
        rd64(F, i->a, A_LO, A_HI);
        rd(F, i->b, B_LO);
        shift64_var(F, i->op == IR_SHL, sign);
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    }
    case IR_EXT: {
        /* straight into the pair: the source read once, before either
         * half is written; the low word first, the high made from it */
        int dl, dh, s;
        dst64(F, i->dst, &dl, &dh);
        s = rdr(F, i->a, dl);
        if (i->size < 4)
            ext_reg(F, dl, s, i->size, i->sign);
        else if (s != dl)
            sparc_mov(t, dl, s);
        if (i->sign) sparc_alu_imm(t, SP_SRA, dh, dl, 31);
        else         sparc_mov(t, dh, SP_G0);
        wr64(F, i->dst, dl, dh);
        return 1;
    }
    case IR_LDVAR:
        if (i->size == 8) {
            rd64(F, i->a, A_LO, A_HI);
        } else {
            if (in_reg(F, i->a))
                ext_reg(F, A_LO, reg_of(F, i->a), i->size, i->sign);
            else
                ld_sp(F, A_LO, var_slot(F, i->a, i->size), i->size, i->sign);
            if (i->sign) sparc_alu_imm(t, SP_SRA, A_HI, A_LO, 31);
            else         sparc_mov(t, A_HI, SP_G0);
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
        /* into the pair itself; the word whose register is the base
         * last */
        int addr = rdr(F, i->a, ADDR), dl, dh;
        dst64(F, i->dst, &dl, &dh);
        if (i->size == 8) {
            if (addr == dh) {
                ld_any(F, dl, addr, (int)i->memoff + WLO, 4, 0, i->natural);
                ld_any(F, dh, addr, (int)i->memoff + WHI, 4, 0, i->natural);
            } else {
                ld_any(F, dh, addr, (int)i->memoff + WHI, 4, 0, i->natural);
                ld_any(F, dl, addr, (int)i->memoff + WLO, 4, 0, i->natural);
            }
        } else {
            ld_any(F, dl, addr, (int)i->memoff, i->size, i->sign,
                   i->natural);
            if (i->sign) sparc_alu_imm(t, SP_SRA, dh, dl, 31);
            else         sparc_mov(t, dh, SP_G0);
        }
        wr64(F, i->dst, dl, dh);
        return 1;
    }
    case IR_STORE: {
        int addr = rdr(F, i->a, ADDR), lo, hi;
        if (is_remat(F, i->b)) {
            lo = remat_reg(F, i->b, A_LO, 0);
            hi = remat_reg(F, i->b, A_HI, 1);
        } else {
            src64(F, i->b, A_LO, A_HI, &lo, &hi);
        }
        if (i->size == 8) {
            st_any(F, hi, addr, (int)i->memoff + WHI, 4, i->natural);
            st_any(F, lo, addr, (int)i->memoff + WLO, 4, i->natural);
        } else {
            st_any(F, lo, addr, (int)i->memoff, i->size, i->natural);
        }
        return 1;
    }
    case IR_SELECT: {
        /* dst = a ? b : c: c into A, then b over it unless the condition
         * (either half of a 64-bit one) is zero */
        int skip;
        if (i->size == 8) {
            int al, ah;
            src64(F, i->a, SCR, SCR2, &al, &ah);
            sparc_alu(t, SP_OR, SCR, al, ah);
        } else {
            rd(F, i->a, SCR);
        }
        rd64(F, i->c, A_LO, A_HI);
        sparc_alu(t, SP_ORCC, SP_G0, SCR, SP_G0);
        skip = br_place(F, SP_BE);
        rd64(F, i->b, A_LO, A_HI);
        br_land(F, skip);
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    }
    default:
        return 0;
    }
}

/* A soft-float comparison's answer, the helper's %o0 against 0 (signed,
 * as cc_to_reg reads it), branched on at once when its only reader is
 * the BRZ/BRNZ right after it: no 0/1 made and tested again. */
static void cond_branch(struct sparc_fn *F, int m, int cond, int label);

static int fcmp_branch(struct sparc_fn *F, int n)
{
    struct ir_func *fn = F->fn;
    const struct ir_ins *i = &fn->ins[n], *nx;
    int cond;
    if (n + 1 >= fn->nins || !F->usecnt || i->dst < 0 ||
        i->dst >= fn->nvregs || F->usecnt[i->dst] != 1)
        return 0;
    nx = &fn->ins[n + 1];
    if ((nx->op != IR_BRZ && nx->op != IR_BRNZ) || nx->a != i->dst ||
        nx->w > 4)
        return 0;
    sparc_alu(F->t, SP_SUBCC, SP_G0, SP_O0, SP_G0);
    cond = pred_cond(i->pred, 1);
    if (nx->op == IR_BRZ)
        cond = sparc_cond_invert(cond);
    cond_branch(F, n + 2, cond, nx->label);
    F->skip_next = 1;
    return 1;
}

/* ---- long double: binary128, by reference ----------------------------------
 *
 * A long double lives in its sixteen-byte slot (cg_wide_vregs) and never in
 * a register. The helpers take their operands by reference, as any SPARC
 * function taking a long double does, and return one through the
 * struct-return word and an `unimp 16` -- their C definitions in
 * lib/rt/softtf.c, compiled by EmbCC, receive and return them exactly so.
 * The operands are COPIED into the tf area first: the callee owns a
 * by-reference argument and may write it. */
static void copy16(struct sparc_fn *F, long to, long from)
{
    if (to == from)
        return;
    if (!(to & 7) && !(from & 7)) {
        /* both 8-aligned from the 8-aligned %sp: doublewords through the
         * even pair %l6:%l7 */
        for (int q = 0; q < 16; q += 8) {
            ld_sp(F, SCR2, from + q, 8, 0);
            st_sp(F, SCR2, to + q, 8);
        }
        return;
    }
    for (int q = 0; q < 16; q += 4) {
        ld_sp(F, A_LO, from + q, 4, 0);
        st_sp(F, A_LO, to + q, 4);
    }
}

static void need16(const struct sparc_fn *F, int v)
{
    if (!is16(F, v) || F->slot[v] < 0)
        internal_error("sparc: %s: long double vreg %d has no 16-byte slot",
                       F->fn->name, v);
}

static long tf_result(struct sparc_fn *F, int v)
{
    if (v < 0 || F->slot[v] < 0)
        return F->tfa + 32;
    need16(F, v);
    return F->slot[v];
}

/* Does temp v's value die at instruction n -- no read after it, on any
 * path (the live range is dataflow's, so one live round a loop's back
 * edge ends past it)? Then its slot may be handed to a callee as a
 * by-reference copy, which the callee owns and may write. */
static int dies_at(const struct sparc_fn *F, int v, int n)
{
    return F->last && v >= F->fn->nvars && v < F->fn->nvregs &&
           F->last[v] == n;
}

/* The operand's address in `reg`: its own slot when it dies here and is
 * neither the result's slot nor `other` (the other operand, which a
 * callee writing one could change), else a copy in the tf area at `at`. */
static void tf_operand(struct sparc_fn *F, const struct ir_ins *i, int v,
                       long at, int reg, int other)
{
    int n = (int)(i - F->fn->ins);
    need16(F, v);
    if (dies_at(F, v, n) && v != other &&
        (i->dst < 0 || F->slot[i->dst] != F->slot[v])) {
        addr_sp(F, reg, sslot(F, v));
        return;
    }
    copy16(F, F->tfa + at, sslot(F, v));
    addr_sp(F, reg, F->tfa + at);
}

/* A helper returning a long double: the result's address in the
 * struct-return word, the call, and the `unimp 16` it returns past. */
static void call_tf_sret(struct sparc_fn *F, const char *name, long res)
{
    addr_sp(F, SCR, res);
    sparc_store(F->t, SCR, SP_SP, SRET, 4);
    call_helper(F, name);
    sparc_unimp(F->t, 16);
}

static const char *tf_cmp_name(enum binop pred)
{
    switch (pred) {
    case B_EQ: return "__eqtf2";
    case B_NE: return "__netf2";
    case B_LT: return "__lttf2";
    case B_LE: return "__letf2";
    case B_GT: return "__gttf2";
    default:   return "__getf2";
    }
}

static int ins128(const struct sparc_fn *F, const struct ir_ins *i)
{
    switch (i->op) {
    case IR_CONST: case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV:
    case IR_MOD: case IR_AND: case IR_OR: case IR_XOR: case IR_SHL:
    case IR_SHR: case IR_NEG: case IR_BNOT: case IR_CMP: case IR_EXT:
        return i->w == 16;
    case IR_LOAD: case IR_LDVAR: case IR_STORE: case IR_STVAR:
        return i->size == 16;
    case IR_I2F: case IR_F2I: case IR_F2F:
        return i->w == 16 || i->size == 16;
    case IR_MOV:
        return is16(F, i->dst) && is16(F, i->a) && (i->w == 16 || i->w == 0);
    case IR_SELECT:
        return i->w == 16;
    case IR_BRZ: case IR_BRNZ:
        return i->w == 16 && is16(F, i->a);
    default:
        return 0;
    }
}

static void gen_ld(struct sparc_fn *F, struct ir_ins *i)
{
    struct code *t = F->t;
    const char *name;

    if (i->imm_b)
        sparc_refuse(F, i, "a folded operand on a long double");
    switch (i->op) {
    case IR_LDVAR: case IR_STVAR: case IR_MOV:
        need16(F, i->a);
        need16(F, i->dst);
        copy16(F, F->slot[i->dst], sslot(F, i->a));
        return;
    case IR_LOAD: {
        int ra_ = rdr(F, i->a, ADDR);
        long d = tf_result(F, i->dst);
        for (int q = 0; q < 16; q += 4) {
            ld_any(F, A_LO, ra_, (int)i->memoff + q, 4, 0, i->natural);
            st_sp(F, A_LO, d + q, 4);
        }
        return;
    }
    case IR_STORE: {
        long s;
        int ra_;
        need16(F, i->b);
        s = sslot(F, i->b);
        ra_ = rdr(F, i->a, ADDR);
        for (int q = 0; q < 16; q += 4) {
            ld_sp(F, A_LO, s + q, 4, 0);
            st_any(F, A_LO, ra_, (int)i->memoff + q, 4, i->natural);
        }
        return;
    }
    case IR_SELECT: {
        int skip;
        long d = tf_result(F, i->dst);
        if (i->size == 8) {
            int al, ah;
            src64(F, i->a, SCR, SCR2, &al, &ah);
            sparc_alu(t, SP_OR, SCR, al, ah);
        } else {
            rd(F, i->a, SCR);
        }
        need16(F, i->b);
        need16(F, i->c);
        sparc_alu(t, SP_ORCC, SP_G0, SCR, SP_G0);
        {
            int isz = br_place(F, SP_BE);
            int done;
            copy16(F, d, F->slot[i->b]);
            done = br_place(F, SP_BA);
            br_land(F, isz);
            copy16(F, d, F->slot[i->c]);
            br_land(F, done);
        }
        (void)skip;
        return;
    }
    case IR_BRZ: case IR_BRNZ:
        need16(F, i->a);
        sparc_mov(t, SCR, SP_G0);
        for (int q = 0; q < 16; q += 4) {
            ld_sp(F, A_LO, F->slot[i->a] + q, 4, 0);
            sparc_alu(t, SP_OR, SCR, SCR, A_LO);
        }
        sparc_alu(t, SP_ORCC, SP_G0, SCR, SP_G0);
        branch_to(F, i->op == IR_BRZ ? SP_BE : SP_BNE, i->label);
        return;
    case IR_CONST: {                    /* sign-extended, as irgen made it */
        long d = tf_result(F, i->dst);
        long long v = (long long)i->imm;
        for (int q = 0; q < 4; q++) {
            /* word q at d + 4q: the most significant first */
            long long word = q >= 2 ? (long long)(int)(v >> (32 * (3 - q)))
                                    : (v < 0 ? -1 : 0);
            sparc_li(t, A_LO, word);
            st_sp(F, A_LO, d + 4 * q, 4);
        }
        return;
    }
    default:
        break;
    }

    if (i->flt && i->op == IR_NEG) {    /* bit 127: right for -0.0, NaN */
        long d = tf_result(F, i->dst);
        need16(F, i->a);
        copy16(F, d, sslot(F, i->a));
        ld_sp(F, A_LO, d, 4, 0);
        sparc_sethi(t, A_HI, 0x80000000UL >> 10);
        sparc_alu(t, SP_XOR, A_LO, A_LO, A_HI);
        st_sp(F, A_LO, d, 4);
        return;
    }
    if (i->flt && (i->op == IR_ADD || i->op == IR_SUB || i->op == IR_MUL ||
                   i->op == IR_DIV)) {
        tf_operand(F, i, i->a, 0, SP_O0, i->b);
        tf_operand(F, i, i->b, 16, SP_O1, i->a);
        call_tf_sret(F, i->op == IR_ADD ? "__addtf3"
                        : i->op == IR_SUB ? "__subtf3"
                        : i->op == IR_MUL ? "__multf3" : "__divtf3",
                     tf_result(F, i->dst));
        return;
    }
    if (i->flt && i->op == IR_CMP) {
        tf_operand(F, i, i->a, 0, SP_O0, i->b);
        tf_operand(F, i, i->b, 16, SP_O1, i->a);
        call_helper(F, tf_cmp_name(i->pred));
        if (fcmp_branch(F, (int)(i - F->fn->ins)))
            return;
        {
            int d = wreg(F, i->dst, ACC);
            cmp_to_reg(F, i->pred, 1, SP_O0, SP_G0, d);
            wrote(F, i->dst, d);
        }
        return;
    }
    if (i->op == IR_I2F || i->op == IR_F2F || i->op == IR_F2I) {
        int sw = i->size, dw = i->w;
        if (dw == 16 && sw <= 8) {
            if (sw == 8 && F->wide[i->a]) {
                args64x2(F, i->a, -1);
            } else if (sw == 8 && i->op == IR_I2F) {
                rd(F, i->a, PLO(SP_O0));    /* a narrow source asked as 64 */
                sparc_mov(t, PHI(SP_O0), SP_G0);
            } else {
                rd(F, i->a, SP_O0);
            }
            if (i->op == IR_I2F)
                name = sw == 8 ? (i->sign ? "__floatditf" : "__floatunditf")
                               : (i->sign ? "__floatsitf" : "__floatunsitf");
            else
                name = sw == 8 ? "__extenddftf2" : "__extendsftf2";
            call_tf_sret(F, name, tf_result(F, i->dst));
            return;
        }
        if (sw == 16 && dw == 16) {
            need16(F, i->a);
            copy16(F, tf_result(F, i->dst), sslot(F, i->a));
            return;
        }
        if (sw == 16) {
            tf_operand(F, i, i->a, 0, SP_O0, -1);
            if (i->op == IR_F2F)
                name = dw == 8 ? "__trunctfdf2" : "__trunctfsf2";
            else
                name = dw == 8 ? (i->sign ? "__fixtfdi" : "__fixunstfdi")
                               : (i->sign ? "__fixtfsi" : "__fixunstfsi");
            call_helper(F, name);
            if (i->dst >= 0) {
                if (F->wide[i->dst] && dw == 8)
                    wr64(F, i->dst, PLO(SP_O0), PHI(SP_O0));
                else if (F->wide[i->dst])
                    wr64(F, i->dst, SP_O0, SP_G0);
                else
                    wr(F, i->dst, SP_O0);
            }
            return;
        }
    }
    sparc_refuse(F, i, "this operation on a long double");
}

/* ---- one call ------------------------------------------------------------ */

static void gen_call(struct sparc_fn *F, int n)
{
    struct ir_func *fn = F->fn;
    struct ir_ins *i = &fn->ins[n];
    struct code *t = F->t;
    struct argplace pl[MAX_PARAMS];
    int word = 0;
    int sret = call_sret(i);
    int rw = i->retsize ? ret_reg_words(i->rety) : 0;
    long copy_at = F->byref_at;

    if (rw < 0)
        sparc_refuse(F, i, "a call returning a _Complex of this type");
    for (int k = 0; k < i->nargs; k++)
        place_arg(&i->argv[k], k == 0 && i->sret_first, &word, &pl[k]);

    /* The by-reference COPIES first: the caller owns them, because the
     * callee may write its parameter. */
    for (int k = 0; k < i->nargs; k++) {
        struct ir_arg *a = &i->argv[k];
        if (!pl[k].byref)
            continue;
        copy_at = (copy_at + 7) & ~7L;
        pl[k].copy = copy_at;
        if (a->is_struct) {
            copy_block(F, 1, a->size, a->natural ? a->align : 1,
                       rdr(F, a->vreg, TMP), 0,  /* its address */
                       SP_FP, fpo(F, copy_at));
        } else {
            int dup = 0;
            need16(F, a->vreg);                  /* a long double */
            for (int q = 0; q < i->nargs; q++)
                dup |= q != k && i->argv[q].vreg == a->vreg;
            if (dies_at(F, a->vreg, n) && !dup &&
                (i->dst < 0 || F->slot[i->dst] != F->slot[a->vreg])) {
                pl[k].copy = sslot(F, a->vreg);  /* its own: it dies */
                continue;
            }
            copy16(F, copy_at, sslot(F, a->vreg));
        }
        copy_at += a->size;
    }

    /* The STACK words next: storing one needs a scratch, and once
     * %o0-%o5 are loaded none is left that is not an argument. */
    for (int k = 0; k < i->nargs; k++) {
        struct ir_arg *a = &i->argv[k];
        if (!pl[k].nstk)
            continue;
        if (pl[k].byref) {
            addr_sp(F, SCR, pl[k].copy);
            st_out(F, SCR, pl[k].stk, 4);
        } else if (a->size > 4) {
            /* the low word (and the high, when it did not fit %o5) */
            int lo, hi;
            src64(F, a->vreg, SCR, SCR2, &lo, &hi);
            if (pl[k].nreg == 0) {
                st_out(F, hi, pl[k].stk, 4);
                st_out(F, lo, pl[k].stk + 4, 4);
            } else {
                st_out(F, lo, pl[k].stk, 4);
            }
        } else {
            st_out(F, rdr(F, a->vreg, SCR), pl[k].stk, 4);
        }
    }
    /* The scalar register words, all at once: a value for %o0 may be in
     * the register %o2 is about to get. Word q of a pair is the HIGH one
     * first. */
    {
        int sd_[2 * MAX_PARAMS], sv_[2 * MAX_PARAMS], sh_[2 * MAX_PARAMS];
        int ns_ = 0;
        for (int k = 0; k < i->nargs; k++) {
            struct ir_arg *a = &i->argv[k];
            if (!pl[k].nreg || pl[k].byref)
                continue;
            for (int q = 0; q < pl[k].nreg; q++) {
                sd_[ns_] = out_reg(pl[k].reg + q);
                sv_[ns_] = a->vreg;
                sh_[ns_] = a->size > 4 ? 1 - q : 0;
                ns_++;
            }
        }
        if (ns_) {
            /* a 32-bit vreg passed as 64 bits never reaches here: irgen
             * extends it first, so `half` reads a pair */
            set_args_half(F, sd_, sv_, sh_, ns_);
        }
    }
    for (int k = 0; k < i->nargs; k++)
        if (pl[k].nreg && pl[k].byref)
            addr_sp(F, out_reg(pl[k].reg), pl[k].copy);
    /* The result buffer's address in the struct-return word. */
    if (sret) {
        long res = i->retsize ? F->scratch_at + i->scratch
                              : tf_result(F, i->dst);
        addr_sp(F, SCR, res);
        sparc_store(t, SCR, SP_SP, SRET, 4);
    }

    if (i->indirect) {
        /* the target last: it may be in a register an argument used */
        long slot;
        rd(F, i->a, CALLREG);
        slot = take_slot(F, 1ULL << CALLREG, 1);
        sparc_jmpl(t, SP_O7, CALLREG, 0);
        put_slot(F, slot);
    } else {
        call_sym(F, i->callee);
    }
    if (sret)
        sparc_unimp(t, (unsigned long)call_sret_size(i) & 0xfff);
    else if (i->sret_first)
        /* the C++ return slot's call: the callee returns past this, as
         * a struct-returning one does; the size is the class's */
        sparc_unimp(t, (unsigned long)i->sret_size & 0xfff);

    if (i->dst < 0)
        return;
    if (i->retsize) {
        /* dst receives the scratch's ADDRESS, the contract irgen shares
         * with every backend. A complex result came back in registers and
         * is stored there first; a struct the callee wrote itself. */
        long at = F->scratch_at + i->scratch;
        for (int q = 0; q < rw; q++)
            st_sp(F, out_reg(q), at + 4L * q, 4);
        addr_sp(F, ACC, at);
        wr(F, i->dst, ACC);
    } else if (is16(F, i->dst)) {
        /* written through the buffer: nothing to move */
    } else if (F->wide[i->dst]) {
        /* a 64-bit result in %o0:%o1, high first; a 32-bit one read into a
         * 64-bit value is %o0, its low word */
        if (i->ret_tybytes > 4)
            wr64(F, i->dst, SP_O1, SP_O0);
        else
            wr64(F, i->dst, SP_O0, SP_G0);
    } else {
        wr(F, i->dst, SP_O0);
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

static void need_word_atomic(struct sparc_fn *F, const struct ir_ins *i)
{
    if (i->size == 1 || i->size == 2)   /* sub_* below: the word around it */
        return;
    if (i->size != 4)
        sparc_refuse(F, i, "an atomic wider than a register");
}

/* The ASI LEON3's casa is given: 10, user data, as clang writes it. */
#define CASA_ASI 10

/* ---- one- and two-byte atomics ---------------------------------------
 *
 * casa and swap are word-sized, so a narrow atomic works on the aligned
 * word around it, as GCC's and LLVM's do: a casa loop that rewrites only
 * its lane,
 *
 *        ld    [aligned], old
 *   1:   new = f(old)  in the lane
 *        new = old ^ ((new ^ old) & mask)
 *        casa  [aligned], old, new        (new = what was there)
 *        cmp   new, old ; bne,a 1b ; mov new, old
 *
 * atomic against the neighbouring bytes too: a store to any of them
 * between the load and the casa makes the casa fail, and the loop goes
 * round with the word it saw. SPARC is big-endian: the lane of address a
 * is bits 8*((a & 3) ^ (4 - size)) up. (ldstub is a byte, but it only
 * ever stores 0xff, so test-and-set, which stores 1, is the loop too.) */
#define SUB_AL  B_LO        /* %g3: the aligned word's address */
#define SUB_MK  B_HI        /* %g4: the lane's mask */
#define SUB_SH  A_LO        /* %g1: the lane's shift (then the CAS's x) */
#define SUB_VAL A_HI        /* %g2: the operand, moved into its lane */
#define SUB_OLD SCR         /* %l7: the word seen */
#define SUB_NEW SCR2        /* %l6: the word to store */

/* sh = the lane's shift, from the address a (in place when sh == a) */
static void sub_shift(struct sparc_fn *F, int sh, int a, int size)
{
    sparc_alu_imm(F->t, SP_AND, sh, a, 3);
    if (target_big_endian())
        sparc_alu_imm(F->t, SP_XOR, sh, sh, 4 - size);
    sparc_alu_imm(F->t, SP_SLL, sh, sh, 3);
}

/* SUB_AL, SUB_MK and SUB_SH for the address a */
static void sub_lane(struct sparc_fn *F, int a, int size)
{
    struct code *t = F->t;
    sub_shift(F, SUB_SH, a, size);
    sparc_alu_imm(t, SP_ANDN, SUB_AL, a, 3);
    sparc_li(t, SUB_MK, size == 1 ? 0xff : 0xffff);
    sparc_alu(t, SP_SLL, SUB_MK, SUB_MK, SUB_SH);
}

/* reg = (src << SUB_SH) & mask */
static void sub_in(struct sparc_fn *F, int reg, int src)
{
    sparc_alu(F->t, SP_SLL, reg, src, SUB_SH);
    sparc_alu(F->t, SP_AND, reg, reg, SUB_MK);
}

/* reg, its lane already masked, shifted down to bit 0 and extended as
 * `sign` says; the shift is in `sh`, or worked out again from vreg a's
 * address through it when `again` */
static void sub_out(struct sparc_fn *F, int reg, int a, int sh, int again,
                    int size, int sign)
{
    struct code *t = F->t;
    if (again)
        sub_shift(F, sh, rdr(F, a, sh), size);
    sparc_alu(t, SP_SRL, reg, reg, sh);
    if (sign) {
        sparc_alu_imm(t, SP_SLL, reg, reg, 32 - 8 * size);
        sparc_alu_imm(t, SP_SRA, reg, reg, 32 - 8 * size);
    }
}

/* swap, fetch-and-add and the bitwise ones, on a byte or a halfword */
static void sub_rmw(struct sparc_fn *F, const struct ir_ins *i)
{
    struct code *t = F->t;
    int top, again;
    sub_lane(F, rdr(F, i->a, ADDR), i->size);
    sub_in(F, SUB_VAL, rdr(F, i->b, TMP));
    sparc_stbar(t);
    sparc_load(t, SUB_OLD, SUB_AL, 0, 4, 0);
    top = mark_here(F);
    if (i->op == IR_XCHG) {
        sparc_mov(t, SUB_NEW, SUB_VAL);
    } else if (i->op == IR_XADD) {
        sparc_alu(t, SP_ADD, SUB_NEW, SUB_OLD, SUB_VAL);
    } else {
        switch ((int)i->imm) {
        case '&': sparc_alu(t, SP_AND, SUB_NEW, SUB_OLD, SUB_VAL); break;
        case '|': sparc_alu(t, SP_OR, SUB_NEW, SUB_OLD, SUB_VAL); break;
        case '^': sparc_alu(t, SP_XOR, SUB_NEW, SUB_OLD, SUB_VAL); break;
        default:                                        /* nand */
            sparc_alu(t, SP_AND, SUB_NEW, SUB_OLD, SUB_VAL);
            sparc_alu(t, SP_XNOR, SUB_NEW, SUB_NEW, SP_G0);
            break;
        }
    }
    /* only the lane changes: old ^ ((new ^ old) & mask) */
    sparc_alu(t, SP_XOR, SUB_NEW, SUB_NEW, SUB_OLD);
    sparc_alu(t, SP_AND, SUB_NEW, SUB_NEW, SUB_MK);
    sparc_alu(t, SP_XOR, SUB_NEW, SUB_NEW, SUB_OLD);
    sparc_casa(t, SUB_AL, CASA_ASI, SUB_OLD, SUB_NEW);
    cmp_rr(F, SUB_NEW, SUB_OLD);
    again = mark_here(F);
    sparc_w(t, sparc_enc_branch(SP_BNE, 1, 0));
    sparc_mov(t, SUB_OLD, SUB_NEW);
    br_back(F, again, top);
    sparc_alu(t, SP_AND, SUB_OLD, SUB_OLD, SUB_MK);
    sub_out(F, SUB_OLD, i->a, SUB_SH, 0, i->size, i->sign);
    wr(F, i->dst, SUB_OLD);
}

/* compare-and-swap on a byte or a halfword:
 *
 *        ld    [aligned], old
 *   1:   and   old, mask, new ; cmp new, expected ; bne 2f
 *        xor   old, x, new                (x = expected ^ desired)
 *        casa  [aligned], old, new
 *        cmp   new, old ; bne,a 1b ; mov new, old
 *   2:
 * old is then the word seen, whichever way the loop ended; the lane is
 * compared, not the word, so a neighbour's change only goes round. */
static void sub_cas(struct sparc_fn *F, const struct ir_ins *i)
{
    struct code *t = F->t;
    int top, again, out_br;
    sub_lane(F, rdr(F, i->a, ADDR), i->size);
    sub_in(F, SUB_NEW, rdr(F, i->c, SCR2));            /* desired */
    if (i->op == IR_CAS) {
        sub_in(F, SUB_VAL, rdr(F, i->b, TMP));         /* expected */
    } else {
        int p = rdr(F, i->b, TMP);
        sparc_load(t, SUB_VAL, p, 0, i->size, 0);
        sub_in(F, SUB_VAL, SUB_VAL);
    }
    sparc_alu(t, SP_XOR, SUB_SH, SUB_NEW, SUB_VAL);    /* x; no shift now */
    sparc_stbar(t);
    sparc_load(t, SUB_OLD, SUB_AL, 0, 4, 0);
    top = mark_here(F);
    sparc_alu(t, SP_AND, SUB_NEW, SUB_OLD, SUB_MK);
    cmp_rr(F, SUB_NEW, SUB_VAL);
    out_br = br_place(F, SP_BNE);
    sparc_alu(t, SP_XOR, SUB_NEW, SUB_OLD, SUB_SH);
    sparc_casa(t, SUB_AL, CASA_ASI, SUB_OLD, SUB_NEW);
    cmp_rr(F, SUB_NEW, SUB_OLD);
    again = mark_here(F);
    sparc_w(t, sparc_enc_branch(SP_BNE, 1, 0));
    sparc_mov(t, SUB_OLD, SUB_NEW);
    br_back(F, again, top);
    br_land(F, out_br);
    sparc_alu(t, SP_AND, SUB_OLD, SUB_OLD, SUB_MK);
    if (i->op == IR_CAS) {
        sub_out(F, SUB_OLD, i->a, SUB_SH, 1, i->size, i->sign);
        wr(F, i->dst, SUB_OLD);
    } else {
        /* the flag from the lanes compared; then *b = the lane seen */
        int p;
        cmp_rr(F, SUB_OLD, SUB_VAL);
        cc_to_reg(F, SP_BE, SUB_NEW);
        sub_out(F, SUB_OLD, i->a, SUB_SH, 1, i->size, 0);
        p = rdr(F, i->b, TMP);
        sparc_store(t, SUB_OLD, p, 0, i->size);
        wr(F, i->dst, SUB_NEW);
    }
}

/* A conditional branch to `label` whose fall-through is ONE machine
 * instruction X (from the IR at m, up to three instructions that make one
 * word between them, as an add and the copy that coalesces away), a jump
 * to L2, and `label` itself -- an if/else arm, a loop's step:
 *
 *     b<cond> label ; nop ; X ; ba L2 ; nop ; label:
 *
 * becomes the inverse branch to L2, annulled, with X in its slot. X then
 * runs exactly when the branch is taken, which is when the old code ran
 * it, and the jump is gone:
 *
 *     b<!cond>,a L2 ; X ; label:
 *
 * X is generated after the placeholder and kept only when it came out as
 * one ordinary word (slot_decode) that noted no relocation site or
 * branch; otherwise everything is undone and the branch is the plain one. */
static int cond_x_op(enum ir_op op)
{
    switch (op) {
    case IR_CONST: case IR_MOV: case IR_BITCAST: case IR_ADD: case IR_SUB:
    case IR_AND: case IR_OR: case IR_XOR: case IR_SHL: case IR_SHR:
    case IR_NEG: case IR_BNOT: case IR_LDVAR: case IR_STVAR: case IR_LOAD:
    case IR_STORE: case IR_EXT: case IR_ADDR:
        return 1;
    default:
        return 0;
    }
}

static void cond_branch(struct sparc_fn *F, int m, int cond, int label)
{
    struct ir_func *fn = F->fn;
    struct code *t = F->t;
    struct sparc_sites *st = F->st;
    int k = 0, j;
    while (k < 3 && m + k < fn->nins && cond_x_op(fn->ins[m + k].op) &&
           !fn->ins[m + k].flt && !sparc_op_calls_helper(&fn->ins[m + k]))
        k++;
    j = m + k;                               /* the jump */
    if (F->fill && cond != SP_BA && k > 0 && j + 1 < fn->nins &&
        fn->ins[j].op == IR_JMP && fn->ins[j].label != label &&
        fn->ins[j + 1].op == IR_LABEL && fn->ins[j + 1].label == label) {
        int len = t->len, nx = st->next, ns = st->nstr, ng = st->ng,
            nf = st->nf, nfix = F->nfix, bar = F->barrier, ok = 1;
        int at = sparc_b_placeholder(t, sparc_cond_invert(cond), 1);
        struct sdec d;
        want_label(F, at, fn->ins[j].label);
        F->barrier = t->len;
        for (int q = m; ok && q < j; q++) {
            gen_ins(F, q);
            ok = !F->skip_next && !F->skip_to && t->len <= at + 8;
        }
        if (ok && t->len == at + 8 &&
            st->next == nx && st->nstr == ns && st->ng == ng &&
            st->nf == nf && F->nfix == nfix + 1 &&
            slot_decode(sparc_rdw(t, at + 4), &d)) {
            F->barrier = t->len;
            F->skip_to = j + 1;              /* X and the jump are out */
            return;
        }
        t->len = len;
        st->next = nx; st->nstr = ns; st->ng = ng; st->nf = nf;
        F->nfix = nfix;
        F->barrier = bar;
        F->skip_next = 0;
        F->skip_to = 0;
    }
    branch_to(F, cond, label);
}

static void gen_ins(struct sparc_fn *F, int n)
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

    if (F->w16 && ins128(F, i)) {
        if (!i->flt && i->op != IR_LOAD && i->op != IR_STORE &&
            i->op != IR_LDVAR && i->op != IR_STVAR && i->op != IR_MOV &&
            i->op != IR_I2F && i->op != IR_F2I && i->op != IR_F2F &&
            i->op != IR_SELECT && i->op != IR_BRZ && i->op != IR_BRNZ &&
            i->op != IR_CONST)
            sparc_refuse(F, i, "a 128-bit integer operation");
        gen_ld(F, i);
        return;
    }

    if (i->flt && (i->op == IR_ADD || i->op == IR_SUB || i->op == IR_MUL ||
                   i->op == IR_DIV || i->op == IR_MOD || i->op == IR_NEG ||
                   i->op == IR_CMP || i->op == IR_SQRT)) {
        const char *name = fp_binop_name(i->op, i->w);
        if (i->w != 4 && i->w != 8)
            sparc_refuse(F, i, "a floating-point value of this width");
        if (name) {
            if (i->imm_b)
                sparc_refuse(F, i, "a folded floating-point immediate");
            fp_args2(F, i);
            call_helper(F, name);
            fp_result(F, i->dst, i->w);
            return;
        }
        if (i->op == IR_NEG) {
            if (i->w == 8) {
                rd64(F, i->a, A_LO, A_HI);
                sparc_sethi(t, B_LO, 0x80000000UL >> 10);
                sparc_alu(t, SP_XOR, A_HI, A_HI, B_LO);
                wr64(F, i->dst, A_LO, A_HI);
            } else {
                rd(F, i->a, ACC);
                sparc_sethi(t, TMP, 0x80000000UL >> 10);
                sparc_alu(t, SP_XOR, ACC, ACC, TMP);
                wr(F, i->dst, ACC);
            }
            return;
        }
        if (i->op == IR_CMP) {
            fp_args2(F, i);
            call_helper(F, fp_cmp_name(i->pred, i->w));
            if (fcmp_branch(F, n))
                return;
            {
                int d = wreg(F, i->dst, ACC);
                sparc_alu(t, SP_SUBCC, SP_G0, SP_O0, SP_G0);
                cc_to_reg(F, pred_cond(i->pred, 1), d);
                wrote(F, i->dst, d);
            }
            return;
        }
        if (i->op == IR_SQRT)
            sparc_refuse(F, i, "__builtin_sqrt (a libm routine here, not an "
                               "instruction)");
        sparc_refuse(F, i, "this floating-point operation");
    }

    if (i->w > 8 && i->op != IR_RET && i->op != IR_CALL)
        sparc_refuse(F, i, "a 128-bit value");
    if (i->op == IR_XCHG || i->op == IR_XADD || i->op == IR_ARMW ||
        i->op == IR_CAS || i->op == IR_CMPXCHG)
        need_word_atomic(F, i);
    /* Level 0 only (irgen). A function that asks always has its own
     * register window (leaf_candidate), so, as clang: the frame address
     * is %fp -- the caller's %sp, the stack pointer at entry -- and the
     * return address is %i7, the address of the call that entered it (the
     * return goes to %i7 + 8). */
    if (i->op == IR_FRAMEADDR) {
        if (i->dst >= 0) {
            int d = wreg(F, i->dst, ACC);
            sparc_mov(F->t, d, i->imm == 2 ? SP_I7 : SP_FP);
            wrote(F, i->dst, d);
        }
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
        if (k)
            sparc_alu_imm(t, i->sign ? SP_SRA : SP_SRL, d, hi, k);
        else if (d != hi)
            sparc_mov(t, d, hi);
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
                    operand_b64(F, i, PLO(SP_O2), PHI(SP_O2));
                } else {
                    args64x2(F, i->a, i->b);
                }
                call_helper(F, i->op == IR_DIV
                            ? (i->sign ? "__divdi3" : "__udivdi3")
                            : (i->sign ? "__moddi3" : "__umoddi3"));
                wr64(F, i->dst, PLO(SP_O0), PHI(SP_O0));
                return;
            }
            if (gen_ins64(F, n))
                return;
            sparc_refuse(F, i, "this operation at 64 bits");
        }
        if (wide && (i->op == IR_CMP || i->op == IR_BRZ || i->op == IR_BRNZ))
            i->w = 8;
    }

    switch (i->op) {
    case IR_LABEL:
        F->label_off[i->label] = mark_here(F);
        return;
    case IR_JMP:
        if (n + 1 < fn->nins && fn->ins[n + 1].op == IR_LABEL &&
            fn->ins[n + 1].label == i->label)
            return;
        jump_to(F, i->label);
        return;
    case IR_CONST: {
        int d = wreg(F, i->dst, ACC);
        if (is_remat(F, i->dst))
            return;                      /* made where it is passed */
        sparc_li(t, d, imm_val(i));
        wrote(F, i->dst, d);
        return;
    }
    case IR_BITCAST:
    case IR_MOV: {
        /* a slot into its register, or a register into its slot, at once */
        int d = wreg(F, i->dst, ACC);
        int src = rdr(F, i->a, d);
        if (!in_reg(F, i->dst)) {
            wrote(F, i->dst, src);
            return;
        }
        if (src != d)
            sparc_mov(t, d, src);
        wrote(F, i->dst, d);
        return;
    }

    case IR_ADD: case IR_SUB: case IR_MUL:
    case IR_AND: case IR_OR:  case IR_XOR: {
        int op = i->op == IR_ADD ? SP_ADD
               : i->op == IR_SUB ? SP_SUB
               : i->op == IR_AND ? SP_AND
               : i->op == IR_OR  ? SP_OR
               : i->op == IR_XOR ? SP_XOR
               : SP_SMUL;
        int ra_ = rdr(F, i->a, ACC);
        int rd_ = wreg(F, i->dst, ACC);
        if (i->imm_b && sparc_simm13_ok(imm_val(i))) {
            sparc_alu_imm(t, op, rd_, ra_, imm_val(i));
            wrote(F, i->dst, rd_);
            return;
        }
        {
            int rb_ = (i->imm_b || !in_reg(F, i->b)) ? TMP : reg_of(F, i->b);
            if (rb_ == TMP) operand_b(F, i, TMP);
            sparc_alu(t, op, rd_, ra_, rb_);
        }
        wrote(F, i->dst, rd_);
        return;
    }
    case IR_MULH: {
        /* the high word of a 32 x 32 product is %y after umul/smul; the
         * low word goes to the destination and is replaced */
        int ra_ = rdr(F, i->a, ACC), rb_ = rdr(F, i->b, TMP);
        int d = wreg(F, i->dst, ACC);
        sparc_alu(t, i->sign ? SP_SMUL : SP_UMUL, d, ra_, rb_);
        sparc_rdy(t, d);
        wrote(F, i->dst, d);
        return;
    }
    case IR_DIV: case IR_MOD: {
        /* %y:a / b: %y the dividend's sign word (signed) or 0 */
        int ra_ = rdr(F, i->a, ACC);
        int rb_ = (i->imm_b || !in_reg(F, i->b)) ? TMP : reg_of(F, i->b);
        int d;
        if (rb_ == TMP) operand_b(F, i, TMP);
        if (i->sign) {
            sparc_alu_imm(t, SP_SRA, SCR, ra_, 31);
            sparc_wry(t, SP_G0, SCR);
        } else {
            sparc_wry(t, SP_G0, SP_G0);
        }
        d = wreg(F, i->dst, ACC);
        if (i->op == IR_DIV) {
            sparc_alu(t, i->sign ? SP_SDIV : SP_UDIV, d, ra_, rb_);
        } else {
            sparc_alu(t, i->sign ? SP_SDIV : SP_UDIV, SCR2, ra_, rb_);
            sparc_alu(t, SP_SMUL, SCR2, SCR2, rb_);
            sparc_alu(t, SP_SUB, d, ra_, SCR2);
        }
        wrote(F, i->dst, d);
        return;
    }
    case IR_SHL: case IR_SHR: {
        int ra_ = rdr(F, i->a, ACC);
        int d;
        if (i->imm_b && i->imm >= 0 && i->imm < 32) {
            d = wreg(F, i->dst, ACC);
            sparc_alu_imm(t, i->op == IR_SHL ? SP_SLL
                             : i->sign ? SP_SRA : SP_SRL,
                          d, ra_, (int)i->imm);
        } else {
            int rb_ = (i->imm_b || !in_reg(F, i->b)) ? TMP : reg_of(F, i->b);
            if (rb_ == TMP) operand_b(F, i, TMP);
            d = wreg(F, i->dst, ACC);
            sparc_alu(t, i->op == IR_SHL ? SP_SLL
                         : i->sign ? SP_SRA : SP_SRL, d, ra_, rb_);
        }
        wrote(F, i->dst, d);
        return;
    }
    case IR_NEG: {
        int ra_ = rdr(F, i->a, ACC);
        int d = wreg(F, i->dst, ACC);
        sparc_alu(t, SP_SUB, d, SP_G0, ra_);
        wrote(F, i->dst, d);
        return;
    }
    case IR_BNOT: {
        int ra_ = rdr(F, i->a, ACC);
        int d = wreg(F, i->dst, ACC);
        sparc_alu(t, SP_XNOR, d, ra_, SP_G0);
        wrote(F, i->dst, d);
        return;
    }

    case IR_CMP: {
        struct ir_ins *nx = n + 1 < fn->nins ? &fn->ins[n + 1]
                                             : (struct ir_ins *)0;
        int fuse = nx && (nx->op == IR_BRZ || nx->op == IR_BRNZ) &&
                   nx->a == i->dst && F->usecnt && F->usecnt[i->dst] == 1;
        if (i->w == 8) {
            int cond = cmp64_cc(F, i, i->pred, i->sign);
            if (fuse) {
                if (nx->op == IR_BRZ)
                    cond = sparc_cond_invert(cond);
                cond_branch(F, n + 2, cond, nx->label);
                F->skip_next = 1;
                return;
            }
            {
                int d = wreg(F, i->dst, ACC);
                cc_to_reg(F, cond, d);
                wrote(F, i->dst, d);
            }
            return;
        }
        if (fuse && !(nx->w == 8)) {
            /* one compare and one branch */
            int ra_ = rdr(F, i->a, ACC);
            int cond = pred_cond(i->pred, i->sign);
            if (i->imm_b && imm_val(i) == 0 &&
                (i->pred == B_EQ || i->pred == B_NE)) {
                test_zero(F, ra_);
            } else if (i->imm_b && sparc_simm13_ok(imm_val(i))) {
                sparc_alu_imm(t, SP_SUBCC, SP_G0, ra_, imm_val(i));
            } else {
                int rb_ = (i->imm_b || !in_reg(F, i->b)) ? TMP
                                                         : reg_of(F, i->b);
                if (rb_ == TMP) operand_b(F, i, TMP);
                cmp_rr(F, ra_, rb_);
            }
            if (nx->op == IR_BRZ)
                cond = sparc_cond_invert(cond);
            cond_branch(F, n + 2, cond, nx->label);
            F->skip_next = 1;
            return;
        }
        {
            int ra_ = rdr(F, i->a, ACC);
            int rb_ = (i->imm_b || !in_reg(F, i->b)) ? TMP : reg_of(F, i->b);
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
        /* dst = a ? b : c: the condition tested, c moved in, and b over
         * it in an annulled slot that runs only when it is nonzero */
        int cond, rb_, rc_, d;
        if (i->size == 8) {
            int al, ah;
            src64(F, i->a, SCR, SCR2, &al, &ah);
            sparc_alu(t, SP_OR, SCR, al, ah);
            cond = SCR;
        } else {
            cond = rdr(F, i->a, SCR);
        }
        rb_ = rdr(F, i->b, TMP);
        rc_ = rdr(F, i->c, ACC);
        d = wreg(F, i->dst, ACC);
        if (d == rb_)
            d = ACC;
        sparc_alu(t, SP_ORCC, SP_G0, cond, SP_G0);
        if (d != rc_)
            sparc_mov(t, d, rc_);
        sparc_w(t, sparc_enc_branch(SP_BNE, 1, 8));
        sparc_mov(t, d, rb_);
        wrote(F, i->dst, d);
        return;
    }

    case IR_BRZ: case IR_BRNZ: {
        int r;
        if (i->w == 8) {
            int al, ah;
            src64(F, i->a, A_LO, A_HI, &al, &ah);
            sparc_alu(t, SP_ORCC, SP_G0, al, ah);
        } else {
            r = rdr(F, i->a, A_LO);
            test_zero(F, r);
        }
        cond_branch(F, n + 1, i->op == IR_BRZ ? SP_BE : SP_BNE, i->label);
        return;
    }

    case IR_LDVAR: {
        int d = wreg(F, i->dst, ACC);
        if (in_reg(F, i->a)) {
            if (sparc_ldvar_plain(i->size, i->sign, i->w)) {
                if (d != reg_of(F, i->a)) sparc_mov(t, d, reg_of(F, i->a));
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
        int src;
        if (is_remat(F, i->a) && in_reg(F, i->dst)) {
            /* the constant, as the narrow store's extension leaves it */
            int k = 32 - 8 * (i->size < 4 ? i->size : 4);
            long long c = F->rematv[i->a];
            c = k ? (long long)((int)((unsigned)c << k) >> k) : c;
            sparc_li(t, reg_of(F, i->dst), c);
            return;
        }
        src = is_remat(F, i->a) ? remat_reg(F, i->a, ACC, 0)
                                : rdr(F, i->a, ACC);
        if (in_reg(F, i->dst)) {
            if (i->size >= 4) {
                if (reg_of(F, i->dst) != src)
                    sparc_mov(t, reg_of(F, i->dst), src);
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
        ld_any(F, d, addr, (int)i->memoff, i->size, i->sign, i->natural);
        wrote(F, i->dst, d);
        return;
    }
    case IR_STORE: {
        int addr = rdr(F, i->a, ADDR);
        int val = is_remat(F, i->b) ? remat_reg(F, i->b, ACC, 0)
                                    : rdr(F, i->b, ACC);
        st_any(F, val, addr, (int)i->memoff, i->size, i->natural);
        return;
    }
    case IR_EXT: {
        int ra_ = rdr(F, i->a, ACC);
        int d = wreg(F, i->dst, ACC);
        if (!(i->size < 4 && F->usecnt && i->a >= 0 && i->a < fn->nvregs &&
              F->usecnt[i->a] == 1 && in_reg(F, i->a) &&
              load_ext(F, ra_, d, i->size, i->sign)))
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
    case IR_STRADDR: {
        int d = wreg(F, i->dst, ACC);
        int at = abs_pair(F, d);
        note_str(F->st, at, i->label, RK_SPARC_HI22);
        note_str(F->st, at + 4, i->label, RK_SPARC_LO10);
        wrote(F, i->dst, d);
        return;
    }
    case IR_GADDR: {
        int d = wreg(F, i->dst, ACC);
        int at = abs_pair(F, d);
        note_glob(F->st, at, i->glob, RK_SPARC_HI22);
        note_glob(F->st, at + 4, i->glob, RK_SPARC_LO10);
        wrote(F, i->dst, d);
        return;
    }
    case IR_FADDR: {
        int d = wreg(F, i->dst, ACC);
        int at = abs_pair(F, d);
        note_fn(F->st, at, i->callee, RK_SPARC_HI22);
        note_fn(F->st, at + 4, i->callee, RK_SPARC_LO10);
        wrote(F, i->dst, d);
        return;
    }

    case IR_MEMCPY: case IR_MEMZERO:
    {
        int db = rdr(F, i->a, ADDR);
        int sb = i->op == IR_MEMCPY ? rdr(F, i->b, TMP) : TMP;
        copy_block(F, i->op == IR_MEMCPY, i->size,
                   i->natural >= 8 ? 8 : i->natural >= 4 ? 4 : 1,
                   sb, 0, db, 0);
    }
        return;

    case IR_CALL:
        gen_call(F, n);
        return;

    case IR_RET:
        if (i->a >= 0) {
            if (fn->ret_abi.is_struct) {
                int rw = ret_reg_words(fn->ret_abi.ty);
                if (rw < 0)
                    sparc_refuse(F, i, "returning a _Complex of this type");
                if (rw > 0) {
                    rd(F, i->a, ADDR);
                    for (int q = 0; q < rw; q++)
                        ld_any(F, in_reg_n(q), ADDR, 4 * q, 4, 0,
                               i->natural);
                } else {
                    /* through the caller's buffer, whose address is the
                     * struct-return word; it comes back in %i0 */
                    int sb = rdr(F, i->a, TMP);
                    ld_fp(F, ADDR, SRET, 4, 0);
                    if (copy_block(F, 1, fn->ret_abi.size,
                                   i->natural ? fn->ret_abi.align : 1,
                                   sb, 0, ADDR, 0))
                        sparc_mov(t, SP_I0, ADDR);
                    else
                        ld_fp(F, SP_I0, SRET, 4, 0);
                }
            } else if (fn->ret_abi.size == 16) {
                /* into the caller's long double: 8-aligned (the ABI's
                 * alignment of the type), as the slot is */
                need16(F, i->a);
                ld_fp(F, ADDR, SRET, 4, 0);
                for (int q = 0; q < 16; q += 8) {
                    ld_sp(F, SCR2, sslot(F, i->a) + q, 8, 0);
                    sparc_store(t, SCR2, ADDR, q, 8);
                }
                sparc_mov(t, SP_I0, ADDR);
            } else if (F->wide[i->a] && fn->ret_abi.size <= 4) {
                rd(F, i->a, SP_I0);
            } else if (F->wide[i->a]) {
                rd64(F, i->a, PLO(SP_I0), PHI(SP_I0));
            } else {
                rd(F, i->a, SP_I0);
            }
        }
        {
            int m = n + 1;
            while (m < fn->nins && fn->ins[m].op == IR_LABEL)
                m++;
            if (m < fn->nins)
                ret_or_jump(F);
        }
        return;

    case IR_UD2:
        /* an illegal instruction: a trap, never a fall-through */
        sparc_unimp(t, 0);
        return;
    case IR_FENCE:
        /* clang's __sync_synchronize for LEON3: stbar, then an atomic
         * load-store below the stack, which orders the loads as well */
        sparc_stbar(t);
        sparc_ldstub(t, SP_G0, SP_SP, -1);
        return;

    case IR_VA_START:
        rd(F, i->a, ADDR);
        addr_sp(F, ACC, F->va_first);
        sparc_store(t, ACC, ADDR, 0, 4);
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
            sparc_refuse(F, i, "a conversion of a 128-bit value");
        if (i->op == IR_I2F && src_w == 8 && i->a >= 0 && !F->wide[i->a]) {
            /* a 32-bit value asked for as 64: zero-extended */
            rd(F, i->a, PLO(SP_O0));
            sparc_mov(t, PHI(SP_O0), SP_G0);
        } else if (src_w == 8) {
            args64x2(F, i->a, -1);
        } else {
            rd(F, i->a, SP_O0);
        }
        call_helper(F, cvt_name(i));
        if (i->dst >= 0) {
            if (F->wide[i->dst] && i->w <= 4)
                wr64(F, i->dst, SP_O0, SP_G0);       /* a 32-bit result */
            else if (F->wide[i->dst])
                wr64(F, i->dst, PLO(SP_O0), PHI(SP_O0));
            else
                wr(F, i->dst, SP_O0);
        }
        return;
    }

    case IR_ASM: {
        /* Extended asm, assembled in irgen (sparc/irgen.c irg_asm_sparc)
         * against the vocabulary in sparc/asm.c. This only places the
         * operands and splices the bytes -- Xtensa's lowering with the
         * register windows' roles.
         *
         * To the allocator (ra_target.asm_in_reg) a value live across an
         * asm keeps out of the registers it may change, which irgen
         * recorded (ir_asm.clob): its operands', its clobbers', the
         * template's, a call's, and its scratch. The operands are values
         * like any other, moved into and out of their registers here,
         * each way as ONE parallel move. No operand is ever in a global,
         * %o6/%o7, %l6/%l7 or %i6/%i7: the scratch the moves and the
         * frame accesses below use, the stack and frame pointers and the
         * return address. The template's bytes are a barrier: no later
         * transfer takes one of its instructions into a delay slot. */
        struct ir_asm *ia = i->asm_ir;
        int vreg_[16], vdst[16], nval = 0;
        if (!ia)
            return;
        /* A continuation's value was written by the asm before it, which
         * must be right there: nothing may run between an asm and the
         * moment its registers are read. */
        if (ia->cont) {
            int k = n - 1;
            while (k >= 0 && fn->ins[k].op == IR_ASM && fn->ins[k].asm_ir &&
                   fn->ins[k].asm_ir->cont)
                k--;
            if (k < 0 || fn->ins[k].op != IR_ASM)
                internal_error("sparc: %s: an asm's further output is not "
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
            if (!(ia->in[k].reg >= SP_O0 && ia->in[k].reg <= SP_O5) &&
                !(ia->in[k].reg >= SP_L0 && ia->in[k].reg <= SP_L5) &&
                !(ia->in[k].reg >= SP_I0 && ia->in[k].reg <= SP_I5))
                internal_error("sparc: %s: an asm operand in %%%s", fn->name,
                               sparc_reg_name(ia->in[k].reg));
        for (int k = 0; k < ia->nout; k++)
            if (!ia->out[k].mem && ia->out[k].size > 4)
                sparc_refuse(F, i, "an asm output wider than a register");
        {
            int naddr = 0;
            for (int k = 0; k < ia->nout; k++)
                naddr += !ia->out[k].val && !ia->out[k].mem;
            if (ia->scr < 0 && naddr > 0)
                sparc_refuse(F, i, "an asm with no scratch register left "
                                   "around it");
        }
        /* In: an input's value, an "m" output's address, and a "+"
         * output's address (its current value is loaded through it
         * below) -- the register-resident ones as one parallel move
         * (SCR breaks a cycle), then the rest from their slots. */
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
                    sparc_refuse(F, i, "an asm whose operands cannot be "
                                       "moved into place");
                for (int k = 0; k < m; k++)
                    sparc_mov(t, od[k], os[k]);
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
                    sparc_load(t, o->reg, o->reg, 0, o->size, 0);
            }
        }
        for (int k = 0; k < ia->codelen; k++)
            code_byte(t, ia->code[k]);
        F->barrier = t->len;
        /* Out, through an address: the address is live across the asm
         * (regalloc.c counts it so), so it is still there. An "m" output
         * was written BY the template through the address its register
         * holds; storing over it would destroy what it wrote. */
        for (int k = 0; k < ia->nout; k++) {
            const struct ir_asm_op *o = &ia->out[k];
            if (o->mem || o->val)
                continue;
            rd(F, o->temp, ia->scr);
            sparc_store(t, o->reg, ia->scr, 0, o->size);
        }
        /* Out, as values: each to its home -- those in memory first,
         * while every operand register still holds what the asm left,
         * then the register-resident ones as one parallel move. */
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
                    sparc_refuse(F, i, "an asm whose outputs cannot be "
                                       "moved into place");
                for (int k = 0; k < m; k++)
                    sparc_mov(t, od[k], os[k]);
            }
        }
        return;
    }

    /* ---- atomics: swap and casa, after a stbar --------------------- */
    case IR_XCHG: {
        int addr, d;
        if (i->size == 1 || i->size == 2) {
            sub_rmw(F, i);
            return;
        }
        addr = rdr(F, i->a, ADDR);
        d = wreg(F, i->dst, ACC);
        rd(F, i->b, SCR);
        sparc_stbar(t);
        sparc_swap(t, SCR, addr, 0);
        if (d != SCR)
            sparc_mov(t, d, SCR);
        wrote(F, i->dst, d);
        return;
    }
    case IR_XADD: case IR_ARMW: {
        /*   stbar
         *   ld    [addr], seen
         * 1: op   seen, val, new
         *   casa  [addr], seen, new      (new = what was there)
         *   cmp   new, seen ; bne,a 1b ; mov new, seen
         * The annulled slot carries the value just seen into the retry. */
        int addr, val, top, again;
        if (i->size == 1 || i->size == 2) {
            sub_rmw(F, i);
            return;
        }
        addr = rdr(F, i->a, ADDR);
        val = rdr(F, i->b, TMP);
        sparc_stbar(t);
        sparc_load(t, SCR, addr, 0, 4, 0);
        top = mark_here(F);
        if (i->op == IR_XADD) {
            sparc_alu(t, SP_ADD, SCR2, SCR, val);
        } else {
            switch ((int)i->imm) {
            case '&': sparc_alu(t, SP_AND, SCR2, SCR, val); break;
            case '|': sparc_alu(t, SP_OR, SCR2, SCR, val); break;
            case '^': sparc_alu(t, SP_XOR, SCR2, SCR, val); break;
            default:                                        /* nand */
                sparc_alu(t, SP_AND, SCR2, SCR, val);
                sparc_alu(t, SP_XNOR, SCR2, SCR2, SP_G0);
                break;
            }
        }
        sparc_casa(t, addr, CASA_ASI, SCR, SCR2);
        cmp_rr(F, SCR2, SCR);
        again = mark_here(F);
        sparc_w(t, sparc_enc_branch(SP_BNE, 1, 0));
        sparc_mov(t, SCR, SCR2);
        br_back(F, again, top);
        wr(F, i->dst, SCR);
        return;
    }
    case IR_CAS: case IR_CMPXCHG: {
        /* casa [addr] asi, expected, desired: desired becomes what was
         * seen. IR_CAS yields it; IR_CMPXCHG writes it back through the
         * pointer in b and yields whether it was the expected one. */
        int addr, exp;
        if (i->size == 1 || i->size == 2) {
            sub_cas(F, i);
            return;
        }
        addr = rdr(F, i->a, ADDR);
        if (i->op == IR_CAS) {
            exp = rdr(F, i->b, TMP);
        } else {
            int p = rdr(F, i->b, TMP);
            sparc_load(t, SCR, p, 0, 4, 0);
            exp = SCR;
        }
        rd(F, i->c, SCR2);
        sparc_stbar(t);
        sparc_casa(t, addr, CASA_ASI, exp, SCR2);
        if (i->op == IR_CAS) {
            wr(F, i->dst, SCR2);
        } else {
            int p = rdr(F, i->b, TMP);
            int d;
            sparc_store(t, SCR2, p, 0, 4);
            d = wreg(F, i->dst, ACC);
            cmp_rr(F, SCR2, exp);
            cc_to_reg(F, SP_BE, d);
            wrote(F, i->dst, d);
        }
        return;
    }
    case IR_ALLOCA: {
        /* A fresh 16-aligned block: %sp moves down by the size rounded to
         * 16 and is itself kept a multiple of 16; the block sits above the
         * outgoing area (a multiple of 16 in such a function), which moves
         * down with %sp. One write of %sp, so the window save area below
         * it is valid at every instruction. */
        int d = wreg(F, i->dst, SCR2);
        rd(F, i->a, SCR);
        sparc_alu_imm(t, SP_ADD, SCR, SCR, 15);
        sparc_alu_imm(t, SP_ANDN, SCR, SCR, 15);
        sparc_alu(t, SP_SUB, SCR, SP_SP, SCR);
        sparc_alu_imm(t, SP_ANDN, SP_SP, SCR, 15);
        if (fits13(F->out_bytes)) {
            sparc_alu_imm(t, SP_ADD, d, SP_SP, F->out_bytes);
        } else {
            sparc_li(t, d, F->out_bytes);
            sparc_alu(t, SP_ADD, d, SP_SP, d);
        }
        wrote(F, i->dst, d);
        return;
    }
    case IR_SPSAVE: {
        int d = wreg(F, i->dst, SCR);
        sparc_mov(t, d, SP_SP);
        wrote(F, i->dst, d);
        return;
    }
    case IR_SPRESTORE:
        sparc_mov(t, SP_SP, rdr(F, i->a, SCR));
        return;
    case IR_SWITCH: {
        /* A jump table in .text right after its dispatch, of 32-bit
         * offsets from the `call .+8` that finds its own address -- so it
         * needs no relocation:
         *
         *     cmp   rI, n ; bgeu default ; nop
         *     call  1f    ; sll rI, 2, %g3        (in the slot)
         *  1: add   %g3, %o7, %g3 ; ld [%g3 + tab-1b], %g3
         *     add   %g3, %o7, %g3 ; jmp %g3 ; nop
         *   tab: .word L0-1b, L1-1b, ...
         *
         * The index is the value less the lowest case (irgen), so one
         * unsigned compare sends both sides of the range to the default.
         * %o7 is free here: nothing lives in it between calls. */
        int nl = fn->jt[i->jt].n;
        int ri = rdr(F, i->a, ACC);
        int anchor, tab;
        if (sparc_simm13_ok(nl)) {
            sparc_alu_imm(t, SP_SUBCC, SP_G0, ri, nl);
        } else {
            sparc_li(t, B_LO, nl);
            cmp_rr(F, ri, B_LO);
        }
        branch_to(F, SP_BCC, i->label);
        anchor = t->len;
        sparc_w(t, sparc_enc_call(8));
        sparc_alu_imm(t, SP_SLL, B_LO, ri, 2);
        sparc_alu(t, SP_ADD, B_LO, B_LO, SP_O7);
        tab = anchor + 28;
        sparc_load(t, B_LO, B_LO, tab - anchor, 4, 0);
        sparc_alu(t, SP_ADD, B_LO, B_LO, SP_O7);
        sparc_jmpl(t, SP_G0, B_LO, 0);
        sparc_nop(t);
        if (t->len != tab)
            internal_error("sparc: %s: the jump table is not where its "
                           "load says", fn->name);
        for (int k = 0; k < nl; k++) {
            want_label(F, t->len, fn->jt[i->jt].labels[k]);
            F->fix[F->nfix - 1].base = anchor;
            sparc_w(t, 0);
        }
        code_mark_data(t, tab, t->len);
        F->barrier = t->len;
        return;
    }
    case IR_LABELADDR: {
        if (cg_label_mark(i))       /* static data's marker: no code */
            return;
        /* &&label: the function's own address as IR_FADDR takes it,
         * sethi/or (HI22/LO10), plus the label's offset in it -- the
         * addend set once the function is laid out. The fix's base is
         * -2 - the first site's index (a table entry's is >= 0, a
         * branch's -1). */
        int d = wreg(F, i->dst, ACC);
        int at = abs_pair(F, d), s0 = F->st->nf;
        note_fn(F->st, at, fn->src, RK_SPARC_HI22);
        note_fn(F->st, at + 4, fn->src, RK_SPARC_LO10);
        want_label(F, at, i->label);
        F->fix[F->nfix - 1].base = -2 - s0;
        wrote(F, i->dst, d);
        return;
    }
    case IR_IGOTO: {
        int r = rdr(F, i->a, ACC);
        long slot = take_slot(F, 1ULL << r, 0);
        sparc_jmpl(t, SP_G0, r, 0);
        put_slot(F, slot);
        return;
    }
    default:
        sparc_refuse(F, i, "this operation");
    }
}

/* Copy `size` bytes from [sb + so] to [db + dof] (copy) or zero them
 * (!copy; sb unused), both ends `align`-aligned (1 when not known): in
 * doublewords through the even pair %l6:%l7 when both are 8-aligned,
 * words when 4-aligned, else bytes. Straight-line up to 128 bytes, a loop
 * beyond, which first moves the addresses into TMP and ADDR (scratch,
 * then advanced). Returns 1 when sb and db are left as they were. */
static int copy_block(struct sparc_fn *F, int copy, long size, int align,
                      int sb, long so, int db, long dof)
{
    struct code *t = F->t;
    int step = align >= 8 && copy ? 8 : align >= 4 ? 4 : 1;
    int reg = step == 8 ? SCR2 : SCR, kept = 1;
    long k, body = size & ~(long)(step - 1);
    if (size > 128 || !fits13(so) || !fits13(so + size) || !fits13(dof) ||
        !fits13(dof + size)) {
        /* the one that is not a base of the other first */
        if (copy && sb == ADDR) {
            if (db == TMP)
                internal_error("sparc: a block copy's bases are crossed");
            addr_off_into(F, TMP, sb, so);
            addr_off_into(F, ADDR, db, dof);
        } else {
            addr_off_into(F, ADDR, db, dof);
            if (copy)
                addr_off_into(F, TMP, sb, so);
        }
        kept = (sb == TMP || !copy) && db == ADDR && so == 0 && dof == 0 &&
               size <= 128;
        sb = TMP; db = ADDR; so = dof = 0;
    }
    if (size > 128) {
        int top, again;
        sparc_li(t, FAR, body);
        sparc_alu(t, SP_ADD, FAR, FAR, ADDR);
        top = mark_here(F);
        if (copy) {
            sparc_load(t, reg, TMP, 0, step, 0);
            sparc_alu_imm(t, SP_ADD, TMP, TMP, step);
        }
        sparc_store(t, copy ? reg : SP_G0, ADDR, 0, step);
        sparc_alu_imm(t, SP_ADD, ADDR, ADDR, step);
        cmp_rr(F, ADDR, FAR);
        again = br_place(F, SP_BNE);
        br_back(F, again, top);
        k = 0;
        size -= body;
        kept = 0;
    } else {
        for (k = 0; k + step <= size; k += step) {
            if (copy) sparc_load(t, reg, sb, (int)(so + k), step, 0);
            sparc_store(t, copy ? reg : SP_G0, db, (int)(dof + k), step);
        }
    }
    for (; k + 4 <= size && step >= 4; k += 4) {
        if (copy) sparc_load(t, SCR, sb, (int)(so + k), 4, 0);
        sparc_store(t, copy ? SCR : SP_G0, db, (int)(dof + k), 4);
    }
    for (; k < size; k++) {
        if (copy) sparc_load(t, SCR, sb, (int)(so + k), 1, 0);
        sparc_store(t, copy ? SCR : SP_G0, db, (int)(dof + k), 1);
    }
    return kept;
}

/* ---- register pairs ------------------------------------------------------ */

static const struct ra_target SPARC_PAIR_RA = {
    sparc_pair_pool_for, sparc_callee_saved, sparc_ldvar_plain,
    1, 1, 1,
    sparc_op_calls_helper,
    0,
    sparc_pair_hints,
    NULL, NULL,
    1,
    NULL, NULL,
    1,
    0,
    0
};

static void sparc_pair_hints(const struct ir_func *fn, int *hint)
{
    int word = 0;
    struct argplace pl;
    for (int p = 0; fn->src && p < fn->nparams && p < fn->nvregs; p++) {
        const struct ir_arg *a = &fn->param_abi[p];
        place_arg(a, p == 0 && fn_sret_first(fn), &word, &pl);
        if (a->size == 8 && pl.nreg == 2 && !pl.byref && !(pl.reg & 1) &&
            !fn->is_varargs)
            hint[p] = in_reg_n(pl.reg);
    }
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        if (i->op == IR_RET && i->a >= 0 && i->a < fn->nvregs &&
            !fn->ret_abi.is_struct && fn->ret_abi.size == 8 &&
            !fn->is_varargs)
            hint[i->a] = SP_I0;
        if (i->op != IR_CALL && sparc_op_calls_helper(i) && i->w == 8) {
            if (i->a >= 0 && i->a < fn->nvregs && hint[i->a] < 0)
                hint[i->a] = SP_O0;
            if (!i->imm_b && i->b >= 0 && i->b < fn->nvregs &&
                hint[i->b] < 0)
                hint[i->b] = SP_O2;
            if (i->dst >= 0 && i->dst < fn->nvregs && hint[i->dst] < 0)
                hint[i->dst] = SP_O0;
        }
        if (i->op != IR_CALL)
            continue;
        if (!i->retsize && i->dst >= 0 && i->dst < fn->nvregs && i->w == 8)
            hint[i->dst] = SP_O0;
        word = 0;
        for (int k = 0; k < i->nargs; k++) {
            const struct ir_arg *a = &i->argv[k];
            place_arg(a, k == 0 && i->sret_first, &word, &pl);
            if (a->size == 8 && pl.nreg == 2 && !pl.byref && !(pl.reg & 1) &&
                a->vreg >= 0 && a->vreg < fn->nvregs && hint[a->vreg] < 0)
                hint[a->vreg] = out_reg(pl.reg);
        }
    }
}

static struct ra_range *g_sp_res;
static int g_sp_nres, g_sp_capres;
static void reserve_pairs(struct ir_func *fn, const int *loc)
{
    int nv = fn->nvregs;
    int *first = xmalloc((size_t)(nv ? nv : 1) * sizeof *first);
    int *last = xmalloc((size_t)(nv ? nv : 1) * sizeof *last);
    ra_live_ranges(fn, first, last);
    g_sp_nres = 0;
    for (int v = 0; v < nv; v++) {
        if (loc[v] < 0 || first[v] < 0) continue;
        if (g_sp_nres + 2 > g_sp_capres) {
            g_sp_capres = g_sp_capres ? g_sp_capres * 2 : 16;
            g_sp_res = xrealloc(g_sp_res,
                                (size_t)g_sp_capres * sizeof *g_sp_res);
        }
        for (int h = 0; h < 2; h++) {
            g_sp_res[g_sp_nres].reg = loc[v] + h;
            g_sp_res[g_sp_nres].first = first[v];
            g_sp_res[g_sp_nres].last = last[v];
            g_sp_res[g_sp_nres].born = 0;
            g_sp_nres++;
        }
    }
    ra_reserve(g_sp_res, g_sp_nres);
    free(first); free(last);
}

static int *pair_alloc(struct ir_func *fn, struct sparc_fn *F, const char *pin)
{
    int nv = fn->nvregs, any = 0;
    char *x = xcalloc((size_t)(nv ? nv : 1), 1);
    int used[RA_MAXPOOL], nused = 0;
    int *loc;

    for (int v = 0; v < nv; v++) {
        x[v] = !F->wide[v] || (pin && pin[v]);
        any |= !x[v];
    }
    /* Kept in memory: a local read or written narrower than itself. */
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
    loc = ra_allocate(fn, &SPARC_PAIR_RA, NULL, x, used, &nused);
    free(x);
    reserve_pairs(fn, loc);
    for (int k = 0; k < nused && k < SPARC_NPAIRS; k++)
        F->pair_used[F->npair++] = used[k];
    return loc;
}

/* ---- one function --------------------------------------------------------- */

/* A parameter's word q: its %i register, or in a variadic function the
 * copy the prologue stored into the home area. */
static int param_word(struct sparc_fn *F, const struct argplace *pl, int q,
                      int scratch)
{
    int w = pl->reg + q;
    if (q < pl->nreg && !F->fn->is_varargs)
        return in_reg_n(w);
    if (q < pl->nreg)
        ld_fp(F, scratch, HOME + 4L * w, 4, 0);
    else
        ld_fp(F, scratch, pl->stk + 4L * (q - pl->nreg), 4, 0);
    return scratch;
}

static void gen_func(struct ir_func *fn, struct code *t,
                     struct sparc_sites *st, int want_debug)
{
    struct func *f = fn->src;
    struct sparc_fn F;
    int i;

    memset(&F, 0, sizeof F);
    F.fn = fn; F.t = t; F.st = st;
    F.want_debug = want_debug;
    F.wide = wide_map(fn);
    F.w16 = cg_wide_vregs(fn);
    if (F.w16)
        for (int v = 0; v < fn->nvregs; v++)
            if (F.w16[v]) F.wide[v] = 0;
    F.nshr = ra_narrow_hishift(fn);
    if (F.w16 && !want_debug && fn->nvregs) {
        int *first = xmalloc((size_t)fn->nvregs * sizeof *first);
        F.last = xmalloc((size_t)fn->nvregs * sizeof *F.last);
        ra_live_ranges(fn, first, F.last);
        free(first);
    }
    for (int v = 0; v < fn->nvregs; v++)
        if (F.nshr[v]) F.wide[v] = 0;
    if (g_sp_regalloc) {
        char *pin = want_debug ? ra_debug_pin_vars(fn) : (char *)0;
        char *excl = NULL;
        int *pair;
        if (F.w16) {
            /* a long double never takes a register */
            excl = xcalloc((size_t)(fn->nvregs ? fn->nvregs : 1), 1);
            for (int v = 0; v < fn->nvregs; v++)
                excl[v] = F.w16[v] || (pin && pin[v]);
        }
        pair = g_sp_pairs ? pair_alloc(fn, &F, excl ? excl : pin) : NULL;
        {
            char *wx = xcalloc((size_t)(fn->nvregs ? fn->nvregs : 1), 1);
            for (int v = 0; v < fn->nvregs; v++)
                wx[v] = F.wide[v] || (F.w16 && F.w16[v]);
            F.loc = ra_allocate(fn, &SPARC_RATGT, wx, excl ? excl : pin,
                                F.used_callee, &F.nsave);
            free(wx);
        }
        g_sp_taken = 0;
        if (pair) {
            for (int v = 0; v < fn->nvregs; v++)
                if (pair[v] >= 0) F.loc[v] = pair[v];
            free(pair);
        }
        free(pin);
        free(excl);
        if (fn->nvregs) {
            F.usecnt = xmalloc((size_t)fn->nvregs * sizeof *F.usecnt);
            ra_count_vreg_uses(fn, F.usecnt);
        }
        if (!want_debug && !getenv("EMBCC_SPARC_NO_REMAT"))
            const_remat(&F);
        {
            const char *lim = getenv("EMBCC_SPARC_RA_MAX");
            if (lim) {
                int nlim = atoi(lim);
                for (int v = nlim; v < fn->nvregs; v++)
                    F.loc[v] = -1;
            }
        }
    }
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
                                           : (int)fpo(&F, obj_slot(&F, v));
    }
    f->code_off = t->len;

    /* The prologue: a new window and the frame. A frame past simm13 is
     * built in %g1, which no argument is in. */
    if (g_sp_leaf) {
        /* no window: leaf_fix checks nothing touches the frame */
    } else if (fits13(-F.frame)) {
        sparc_save(t, SP_SP, SP_SP, -F.frame);
    } else {
        sparc_li(t, SP_G1, -F.frame);
        sparc_save_rr(t, SP_SP, SP_SP, SP_G1);
    }
    /* A variadic function stores %i0-%i5 into the home area its caller
     * reserved, so the named and unnamed words are one block. */
    if (fn->is_varargs)
        for (int k = 0; k < NARGREG; k++)
            sparc_store(t, in_reg_n(k), SP_FP, HOME + 4 * k, 4);
    F.barrier = t->len;       /* the window: nothing moves above it */
    F.fill = !want_debug && !getenv("EMBCC_SPARC_NO_FILL");

    /* The parameters: each register one an edge of a parallel move into
     * wherever the allocator put it (SCR breaks a cycle), each stack one
     * a load deferred until after it. */
    {
        struct argplace pl;
        int word = 0;
        int pmv_dst[RA_MAXPOOL], pmv_src[RA_MAXPOOL], npmv = 0;
        int pstk_reg[RA_MAXPOOL]; long pstk_off[RA_MAXPOOL];
        int npstk = 0;
        for (i = 0; i < fn->nparams; i++) {
            struct ir_arg *a = &fn->param_abi[i];
            place_arg(a, i == 0 && fn_sret_first(fn), &word, &pl);
            if (pl.byref) {
                /* the word is the address of the caller's copy; the body
                 * expects the object in the parameter's own slot */
                int src = param_word(&F, &pl, 0, TMP);
                if (F.slot[i] < 0)
                    continue;
                if (a->is_struct) {
                    copy_block(&F, 1, a->size, a->align, src, 0, SP_FP,
                               fpo(&F, sslot(&F, i)));
                } else if (a->size == 16 && a->align >= 8 &&
                           !(sslot(&F, i) & 7)) {
                    /* a long double: the caller's copy is one, 8-aligned
                     * by the ABI, as the slot is */
                    for (int q = 0; q < 16; q += 8) {
                        sparc_load(t, SCR2, src, q, 8, 0);
                        st_sp(&F, SCR2, sslot(&F, i) + q, 8);
                    }
                } else {
                    for (int q = 0; q < a->size; q += 4) {
                        sparc_load(t, A_LO, src, q, 4, 0);
                        st_sp(&F, A_LO, sslot(&F, i) + q, 4);
                    }
                }
                continue;
            }
            if (a->size > 4 && in_reg(&F, i)) {
                for (int q = 0; q < 2; q++) {
                    int dreg = q == 0 ? PHI(F.loc[i]) : PLO(F.loc[i]);
                    if (q < pl.nreg && !fn->is_varargs) {
                        pmv_dst[npmv] = dreg;
                        pmv_src[npmv] = in_reg_n(pl.reg + q);
                        npmv++;
                    } else {
                        pstk_reg[npstk] = dreg;
                        pstk_off[npstk] = q < pl.nreg
                            ? HOME + 4L * (pl.reg + q)
                            : pl.stk + 4L * (q - pl.nreg);
                        npstk++;
                    }
                }
            } else if (a->size > 4) {
                if (F.slot[i] < 0)
                    continue;
                for (int q = 0; q < 2; q++) {
                    int r = param_word(&F, &pl, q, SCR);
                    st_sp(&F, r, sslot(&F, i) + 4L * q, 4);
                }
            } else if (in_reg(&F, i)) {
                if (pl.nreg && !fn->is_varargs) {
                    pmv_dst[npmv] = reg_of(&F, i);
                    pmv_src[npmv] = in_reg_n(pl.reg);
                    npmv++;
                } else {
                    pstk_reg[npstk] = reg_of(&F, i);
                    pstk_off[npstk] = pl.nreg ? HOME + 4L * pl.reg : pl.stk;
                    npstk++;
                }
            } else if (F.slot[i] >= 0) {
                /* the whole word: a promoted char is right-justified in
                 * it, which is where obj_slot puts a narrow variable */
                int r = param_word(&F, &pl, 0, SCR);
                st_sp(&F, r, slot32(&F, i), 4);
            }
        }
        if (npmv) {
            int od[RA_MAXPOOL * 2], os[RA_MAXPOOL * 2];
            int m = ra_parallel_move(pmv_dst, pmv_src, npmv, SCR, od, os,
                                     (int)(sizeof od / sizeof od[0]));
            if (m < 0)
                internal_error("sparc: %s: the prologue's parameter "
                               "placement is not a well-formed move",
                               fn->name);
            for (int k = 0; k < m; k++)
                sparc_mov(t, od[k], os[k]);
        }
        for (int k = 0; k < npstk; k++)
            ld_fp(&F, pstk_reg[k], pstk_off[k], 4, 0);
        /* where the first unnamed word is: where the named ones end,
         * as an offset from %sp so addr_sp turns it back into %fp + */
        if (fn->is_varargs)
            F.va_first = F.frame + HOME + 4L * word;
    }

    for (i = 0; i < fn->nins; i++) {
        gen_ins(&F, i);
        if (F.skip_to) {
            i = F.skip_to - 1;
            F.skip_to = 0;
            F.skip_next = 0;
        } else if (F.skip_next) {
            F.skip_next = 0;
            i++;
        }
    }

    /* The epilogue: every IR_RET's branch lands here. `restore` puts %sp
     * back to %fp, so a VLA needs nothing; a function returning through
     * the caller's buffer returns past the caller's `unimp`. */
    for (i = 0; i < F.nfix; i++)
        if (F.fix[i].label == fn->nlabels)
            F.barrier = t->len;          /* a branch lands here */
    F.label_off[fn->nlabels] = t->len;
    ret_restore(&F);

    for (i = 0; i < F.nfix; i++) {
        int target = F.label_off[F.fix[i].label];
        if (target < 0)
            internal_error("sparc: label %d of %s was never placed",
                           F.fix[i].label, fn->name);
        if (F.fix[i].base >= 0) {
            sparc_wrw(t, F.fix[i].at, (unsigned long)(target - F.fix[i].base)
                                      & 0xffffffffUL);
            continue;
        }
        if (F.fix[i].base <= -2) {                    /* &&label */
            int s0 = -2 - F.fix[i].base;
            F.st->f[s0].addend = target - f->code_off;
            F.st->f[s0 + 1].addend = target - f->code_off;
            continue;
        }
        if (!sparc_patch_b(t, F.fix[i].at, target))
            sparc_refuse(&F, NULL, "a branch beyond +-8 MiB (the function "
                                   "is too large)");
    }

    f->code_len = t->len - f->code_off;
    f->stack_bytes = g_sp_leaf ? 0 : (int)F.frame;
    free(F.usecnt);
    free(F.slot);
    cg_note_labels(fn, F.label_off);
    free(F.label_off);
    free(F.fix);
    free(F.wide);
    free(F.w16);
    free(F.nshr);
    free(F.last);
    free(F.remat);
    free(F.rematv);
    free(F.loc);
}

/* ---- leaf functions --------------------------------------------------------
 *
 * A function that calls nothing and needs no stack can run in its
 * caller's window, as clang's do: no `save`, its arguments in %o0-%o5,
 * its result back in %o0, `retl` (jmpl %o7 + 8). It is generated as an
 * ordinary function whose allocation pool is the ins alone, without the
 * save, and then CHECKED word by word (leaf_fix): every register any
 * instruction names must be %g0-%g4 or %i0-%i5 -- nothing of the
 * caller's window (%l*, %o*), no %sp or %fp, so no slot -- the only
 * transfers branches and the return through %i7, and no call, save or
 * restore. Then the ins are renamed the outs. Anything else, and the
 * attempt is thrown away. */
static int leaf_candidate(const struct ir_func *fn)
{
    int word = 0;
    struct argplace pl;
    if (fn->is_varargs || fn->has_alloca || fn_sret(fn) || !fn->src)
        return 0;
    for (int p = 0; p < fn->nparams; p++) {
        place_arg(&fn->param_abi[p], p == 0 && fn_sret_first(fn), &word, &pl);
        if (pl.byref || pl.nstk)
            return 0;
    }
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        switch (i->op) {
        case IR_CALL: case IR_SWITCH: case IR_LABELADDR: case IR_IGOTO:
        case IR_FENCE: case IR_ALLOCA: case IR_SPSAVE: case IR_SPRESTORE:
        case IR_VA_START: case IR_ASM: case IR_FRAMEADDR:
            return 0;
        default:
            break;
        }
        if (sparc_op_calls_helper(i))
            return 0;
    }
    return 1;
}

/* One register field: allowed in a leaf, and renamed %iN -> %oN. */
static int leaf_reg(int r, int *out)
{
    if (r <= SP_G4) {
        *out = r;
        return 1;
    }
    if (r >= SP_I0 && r <= SP_I5) {
        *out = r - SP_I0 + SP_O0;
        return 1;
    }
    return 0;
}

static int leaf_fix(struct code *t, int from)
{
    for (int pass = 0; pass < 2; pass++)
        for (int at = from; at < t->len; at += 4) {
            unsigned long w = sparc_rdw(t, at);
            int op = (int)(w >> 30) & 3, rd = (int)(w >> 25) & 31,
                op3 = (int)(w >> 19) & 63, rs1 = (int)(w >> 14) & 31,
                imm = (int)(w >> 13) & 1, rs2 = (int)w & 31;
            int nrd = rd, nrs1 = rs1, nrs2 = rs2, nrd2;
            if (op == 1)
                return 0;                               /* a call */
            if (op == 0) {
                int op2 = (int)(w >> 22) & 7;
                if (op2 == 2 || (op2 == 0 && rd == 0))
                    continue;                   /* a branch; unimp */
                if (op2 != 4 || !leaf_reg(rd, &nrd))
                    return 0;
                if (pass)
                    sparc_wrw(t, at, (w & ~(31UL << 25)) |
                                     ((unsigned long)nrd << 25));
                continue;
            }
            if (op == 2 && op3 == 0x38) {               /* jmpl */
                if (w != sparc_enc_ri(2, SP_G0, 0x38, SP_I7, 8))
                    return 0;
                if (pass)
                    sparc_wrw(t, at, sparc_enc_ri(2, SP_G0, 0x38, SP_O7, 8));
                continue;
            }
            if (op == 2 && !((op3 <= 0x1f) || (op3 >= 0x25 && op3 <= 0x28) ||
                             op3 == 0x30))
                return 0;              /* save, restore, rett, ticc, ... */
            if (op == 2 && op3 == 0x28 && rs1 != 0 && rs1 != 15)
                return 0;                       /* rd of a state register */
            if (!leaf_reg(rd, &nrd) || !leaf_reg(rs1, &nrs1) ||
                (!imm && !leaf_reg(rs2, &nrs2)))
                return 0;
            if (op == 3 && (op3 == 0x03 || op3 == 0x07) &&
                !leaf_reg(rd + 1, &nrd2))
                return 0;                       /* ldd/std's second word */
            if (pass) {
                w = (w & ~((31UL << 25) | (31UL << 14))) |
                    ((unsigned long)nrd << 25) | ((unsigned long)nrs1 << 14);
                if (!imm)
                    w = (w & ~31UL) | (unsigned long)nrs2;
                sparc_wrw(t, at, w);
            }
        }
    return 1;
}

/* With the allocator on, a function is generated with the pair pass and
 * without it, and the shorter is kept (RV32's and MIPS's arrangement). */
static void gen_func_best(struct ir_func *fn, struct code *t,
                          struct sparc_sites *st, int want_debug)
{
    int at = t->len, next = st->next, nstr = st->nstr, ng = st->ng,
        nf = st->nf, with;

    if (g_sp_regalloc && !want_debug && !getenv("EMBCC_NO_MEMOFF")) {
        char *w = wide_map(fn);
        ra_fold_memoff(fn, -4096, 4095 - 16, 4, 4, w, 0, 0);
        free(w);
    }
    g_sp_pairs = 1;
    if (!g_sp_regalloc || want_debug || getenv("EMBCC_SPARC_PAIRS")) {
        if (getenv("EMBCC_SPARC_PAIRS"))
            g_sp_pairs = atoi(getenv("EMBCC_SPARC_PAIRS"));
        gen_func(fn, t, st, want_debug);
        g_sp_pairs = 1;
        return;
    }
    gen_func(fn, t, st, want_debug);
    with = t->len - at;
    t->len = at; st->next = next; st->nstr = nstr; st->ng = ng; st->nf = nf;
    g_sp_pairs = 0;
    gen_func(fn, t, st, want_debug);
    if (t->len - at > with) {
        t->len = at; st->next = next; st->nstr = nstr; st->ng = ng;
        st->nf = nf;
        g_sp_pairs = 1;
        gen_func(fn, t, st, want_debug);
    }
    with = t->len - at;
    /* the leaf attempt, kept when it checks out and is shorter: tried
     * after the others, so the one kept is regenerated as it was */
    if (leaf_candidate(fn) && !getenv("EMBCC_SPARC_NO_LEAF")) {
        int pairs = g_sp_pairs, len = t->len, leaf;
        unsigned char *keep = xmalloc((size_t)(len - at) + 1);
        memcpy(keep, t->p + at, (size_t)(len - at));
        {
            int knext = st->next, knstr = st->nstr, kng = st->ng,
                knf = st->nf;
            t->len = at; st->next = next; st->nstr = nstr; st->ng = ng;
            st->nf = nf;
            g_sp_leaf = 1;
            g_sp_pairs = 1;
            gen_func(fn, t, st, want_debug);
            g_sp_leaf = 0;
            leaf = leaf_fix(t, fn->src->code_off) && t->len - at < with;
            if (!leaf) {
                /* the other one back, byte for byte: its sites are the
                 * entries past the counts, which a leaf (no call)
                 * attempt may have overwritten -- so it is generated
                 * again instead, as it was */
                t->len = at; st->next = next; st->nstr = nstr; st->ng = ng;
                st->nf = nf;
                g_sp_pairs = pairs;
                gen_func(fn, t, st, want_debug);
                if (t->len != len || memcmp(t->p + at, keep,
                                            (size_t)(len - at)) ||
                    st->next != knext || st->nstr != knstr ||
                    st->ng != kng || st->nf != knf)
                    internal_error("sparc: %s: generating it again gave "
                                   "other code", fn->name);
            }
        }
        free(keep);
    }
    g_sp_pairs = 1;
}

void codegen_unit_sparc(struct ir_unit *iu, struct code *text,
                        struct extcall **ext, int *next,
                        struct strsite **strs, int *nstrs,
                        struct gsite **gs, int *ngs,
                        struct fsite **fs, int *nfs, int want_debug,
                        int optimize, int no_sse, int regalloc)
{
    struct sparc_sites st;

    (void)optimize; (void)no_sse;
    g_sp_regalloc = regalloc;
    memset(&st, 0, sizeof st);
    for (int n = 0; n < iu->nfuncs; n++)
        gen_func_best(&iu->funcs[n], text, &st, want_debug);
    cg_resolve_strsites(iu, st.str, st.nstr);
    *ext = st.ext;   *next = st.next;
    *strs = st.str;  *nstrs = st.nstr;
    *gs = st.g;      *ngs = st.ng;
    *fs = st.f;      *nfs = st.nf;
}
