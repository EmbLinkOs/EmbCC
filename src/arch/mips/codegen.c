/* MIPS32r2 code generation, o32 in either byte order, soft float
 * (docs/internals/mips32-plan.md).
 *
 * The shape is the RV32 backend's (src/arch/riscv/codegen.c), because the
 * two machines are nearly the same machine: 32 registers with a hardwired
 * zero, three-operand arithmetic, no flags, loads and stores with a
 * signed immediate offset, a 64-bit value in a register pair, and soft
 * float through libgcc's helpers. Every vreg has one home -- a register
 * the shared allocator gave it, or a frame slot -- and every operation
 * reads its operands through rdr/rd and writes its result through
 * wreg/wrote, so the code is correct with the allocator off and smaller
 * with it on. What MIPS changes, and where:
 *
 *   * DELAY SLOTS. Every branch, jump, call and return executes the
 *     instruction after it before the transfer. All of them go through
 *     the helpers below that put a nop there, so the code is right by
 *     construction; filling the slots is an optimization for later.
 *   * The o32 convention lays the arguments out as a block in memory
 *     whose first 16 bytes ride in a0-a3 (place_arg), every caller
 *     reserves those 16 bytes as the callee's home area, and every
 *     struct comes back through a hidden pointer. Results are in v0:v1,
 *     not in the first argument register.
 *   * Branches compare two registers for == and != only; an ordered
 *     comparison is an slt into $at first (branch_if).
 *   * andi/ori/xori ZERO-extend their immediate where addiu and slti
 *     sign-extend it, and every immediate is 16 bits.
 *   * A misaligned word access traps. A load or store the front end
 *     cannot promise is aligned (a packed member) goes through
 *     lwl/lwr/swl/swr or bytes (ld_any/st_any).
 *   * Addresses are absolute lui/addiu pairs (R_MIPS_HI16/LO16) and calls
 *     are jal (R_MIPS_26), even within the unit: there is no PC-relative
 *     address and a jal's target is an absolute word index.
 *
 * Refused by name: atomics narrower than a word, a branch beyond
 * +-128 KiB, and __int128 and binary128 (neither exists on o32). THE
 * RULE.
 *
 * ---- MIPS64 (n64) --------------------------------------------------------
 *
 * The same file at 64 bits (g_m64, from target_xlen()), as one RISC-V
 * backend serves both widths (D-016): the delay slots, the branches, the
 * HI/LO unit, the misaligned paths and the relocation sites are the same
 * machine. What differs (docs/internals/mips64-plan.md):
 *
 *   * A register is eight bytes (W), and an operation at w == 8 is the
 *     doubleword instruction (daddu, dsll, dmult, ld). A 32-bit value in
 *     a register is kept SIGN-EXTENDED -- n64's invariant, and the 32-bit
 *     instructions' precondition -- the RV64 backend's sext map and rd32.
 *   * The "pair" machinery below, written for a 64-bit value in two
 *     32-bit registers, is at 64 bits the machinery for a 16-byte value
 *     (__int128, binary128 long double) in two doublewords: the same
 *     carries, shifts and comparisons at twice the width. Such a value
 *     lives in its slot, never in registers (the pair pass is off).
 *   * n64 places arguments in doubleword slots, eight in a0-a7 ($4-$11),
 *     the rest on the stack from the caller's sp with no home area; a
 *     16-byte-aligned argument starts at an even slot; a composite of any
 *     size goes by value. A composite of at most 16 bytes comes back in
 *     v0:v1. $8-$11 are argument registers here, so the scratch set moves
 *     to $12-$15, t8, t9 and $at.
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

struct mips_fn;
static void copy_block(struct mips_fn *F, int copy, long size, int aligned);

/* The scratch registers: t0-t6, none of them in the allocator's pool. A
 * 64-bit binary operation needs four (A and B pairs); SCR and SCR2 are
 * for the few places that need more; FAR holds a base plus a large frame
 * offset and nothing else. $at is the comparison a branch tests
 * (branch_if), and t9 an indirect call's target, as the ABI expects. */
/* MIPS64: g_m64. $8-$11 carry arguments there (a4-a7), so the scratch
 * set is $12-$15 (n64's t0-t3), t8, and t9 doubling as B_HI -- it is the
 * call register only between an indirect call's last argument and the
 * jalr, when no pair value is live -- and FAR is $at, which is CC: a far
 * slot is addressed only while loading or storing a value, never
 * between a comparison into $at and its branch. */
static int g_m64;
#define W (g_m64 ? 8 : 4)           /* a register's bytes */
#define A_LO (g_m64 ? 12 : MIPS_T0)
#define A_HI (g_m64 ? 13 : MIPS_T1)
#define B_LO (g_m64 ? 14 : MIPS_T2)
#define B_HI (g_m64 ? MIPS_T9 : MIPS_T3)
#define ACC  A_LO         /* the value being computed */
#define TMP  A_HI         /* the second operand */
#define ADDR B_LO         /* an address */
#define SCR  (g_m64 ? 15 : MIPS_T4)
#define SCR2 (g_m64 ? MIPS_T8 : MIPS_T5)
#define FAR  (g_m64 ? MIPS_AT : MIPS_T6)
#define CC   MIPS_AT
#define CALLREG MIPS_T9
/* Address arithmetic at the register's width: the 64-bit forms at MIPS64,
 * where a pointer is a doubleword. */
#define P_ADDU  (g_m64 ? MIPS_DADDU : MIPS_ADDU)
#define P_SUBU  (g_m64 ? MIPS_DSUBU : MIPS_SUBU)
#define P_ADDIU (g_m64 ? MIPS_DADDIU : MIPS_ADDIU)

struct mips_sites {
    struct extcall *ext;   int next, capext;
    struct strsite *str;   int nstr, capstr;
    struct gsite *g;       int ng, capg;
    struct fsite *f;       int nf, capf;
};

struct mips_fn {
    int *usecnt;         /* per vreg: how many reads (fusion), or NULL */
    int skip_next;       /* the instruction after this one is already out */
    int want_debug;
    struct ir_func *fn;
    int *loc;            /* per vreg: its register, -1 in memory; NULL at -O0 */
    int used_callee[RA_MAXPOOL];
    int pair_used[8], npair;
    int nsave;
    struct code *t;
    struct mips_sites *st;
    char *wide;          /* per vreg: a 64-bit value, a register pair */
    char *nshr;          /* per vreg: a narrow high-word shift (narrow_shr) */
    char *sx;            /* MIPS64, per vreg: already the sign extension of
                          * its low 32 bits (sext_map); NULL at MIPS32 */
    long *slot;          /* per vreg: byte offset from the frame base, -1 */
    long frame;          /* bytes sp moves down by */
    long scratch_at;     /* where fn->scratch_bytes begins */
    long sret_slot;      /* where the hidden result pointer is kept, or -1 */
    long ra_slot;
    long save_at;        /* the callee-saved registers the allocator took */
    int fb;              /* the frame base: sp, or fp under alloca */
    long out_bytes;      /* the outgoing argument block, home area included */
    int leaf;            /* no call at all, so ra stays where it is */
    char *tail;          /* per instruction: a tail call, or NULL */
    long va_first;       /* a variadic function's first unnamed word, or -1 */
    long va_base;        /* ...and where its argument register 0 is kept:
                          * the caller's home area (o32) or the top of this
                          * frame, below the incoming stack words (n64) */
    int *label_off;      /* per label id, or -1 while unseen */
    /* A branch or jump to a label: `kind` FX_B for the 16-bit form the
     * function patches itself, FX_J for a `j` relocated against .text
     * (the long form, branch_if's beyond 128 KiB). */
    struct { int at; int label; int kind; int base; } *fix;
    int nfix, capfix;
    /* Per branch, in emission order: take the long form. NULL on the
     * first attempt, which tries every one short (gen_func). */
    const char *longb;
    int nlongb;
    /* Delay-slot filling (take_slot): the lowest offset an instruction may
     * be moved from -- raised past every label, relocated instruction,
     * transfer, asm block and the prologue -- and whether this function
     * fills at all (not under -g, whose line rows name offsets). */
    int barrier;
    int fill;
    /* An interrupt handler (mips_isr_grow): struct func's is_isr, 0 for
     * an ordinary function. isr_x is the caller-saved registers it saves
     * (a mask), isr_hilo whether HI and LO are among them; with EPC and
     * Status they fill isr_bytes at the top of the frame, from isr_at. */
    int isr;
    unsigned long isr_x;
    int isr_hilo;
    long isr_at, isr_bytes;
};

/* ---- the allocator's view of this machine ------------------------------
 *
 * Caller-saved first, as regalloc.h asks: v0-v1, a0-a3 and the two
 * temporaries the scratches leave over (t7, t8); then s0-s7. fp ($30) is
 * the frame base under alloca and never handed out; gp, k0, k1 and at are
 * never touched. */
#define MIPS_NPOOL 16
static const int MIPS_POOL[MIPS_NPOOL] = {
    MIPS_V0, MIPS_V1, MIPS_A0, MIPS_A1, MIPS_A2, MIPS_A3, MIPS_T7, MIPS_T8,
    MIPS_S0, MIPS_S1, MIPS_S2, MIPS_S3, MIPS_S4, MIPS_S5, MIPS_S6, MIPS_S7
};
/* A variadic function spills a0-a3 into its home area at entry and
 * va_arg walks them there, so they are not the allocator's to give. */
static const int MIPS_POOL_VA[MIPS_NPOOL - 4] = {
    MIPS_V0, MIPS_V1, MIPS_T7, MIPS_T8,
    MIPS_S0, MIPS_S1, MIPS_S2, MIPS_S3, MIPS_S4, MIPS_S5, MIPS_S6, MIPS_S7
};
/* MIPS64: v0-v1 and the eight argument registers, then s0-s7; every
 * other register is a scratch (above). Variadic: no argument register. */
#define MIPS64_NPOOL 18
static const int MIPS64_POOL[MIPS64_NPOOL] = {
    MIPS_V0, MIPS_V1, MIPS_A0, MIPS_A1, MIPS_A2, MIPS_A3,
    MIPS_A4, MIPS_A5, MIPS_A6, MIPS_A7,
    MIPS_S0, MIPS_S1, MIPS_S2, MIPS_S3, MIPS_S4, MIPS_S5, MIPS_S6, MIPS_S7
};
static const int MIPS64_POOL_VA[MIPS64_NPOOL - 8] = {
    MIPS_V0, MIPS_V1,
    MIPS_S0, MIPS_S1, MIPS_S2, MIPS_S3, MIPS_S4, MIPS_S5, MIPS_S6, MIPS_S7
};
/* n64's argument registers, in order */
static const int MIPS64_ARGREG[8] = {
    MIPS_A0, MIPS_A1, MIPS_A2, MIPS_A3, MIPS_A4, MIPS_A5, MIPS_A6, MIPS_A7
};

/* BYTE ORDER (docs/internals/big-endian.md). In memory a 64-bit value's
 * low word is at +0 little-endian and at +4 big-endian. In registers o32
 * makes a pair MIRROR memory: the argument block's word at the lower
 * address travels in the lower-numbered register, so a long long in
 * a0:a1 has its HIGH word in a0 when big-endian, and a result in v0:v1
 * likewise (clang --target=mips-unknown-elf). The pairs here follow the
 * same rule, so a value already where the ABI wants it needs no swap: a
 * pair is named by its first register r, and holds the low word in PLO(r)
 * and the high one in PHI(r). WLO/WHI are the memory offsets. */
static int g_be;
#define PLO(r) ((r) + g_be)
#define PHI(r) ((r) + 1 - g_be)
#define WLO (g_be ? W : 0)
#define WHI (g_be ? 0 : W)

static unsigned long g_mips_taken;      /* registers the pair pass took */
static int g_mips_pairs = 1;            /* this attempt uses the pair pass */
static int g_mips_pool[MIPS64_NPOOL];

static const int *mips_pool_for(const struct ir_func *fn, int *n)
{
    const int *p = g_m64 ? (fn->is_varargs ? MIPS64_POOL_VA : MIPS64_POOL)
                         : (fn->is_varargs ? MIPS_POOL_VA : MIPS_POOL);
    int np = g_m64 ? (fn->is_varargs ? MIPS64_NPOOL - 8 : MIPS64_NPOOL)
                   : (fn->is_varargs ? MIPS_NPOOL - 4 : MIPS_NPOOL), k = 0;
    if (!g_mips_taken) {
        *n = np;
        return p;
    }
    for (int j = 0; j < np; j++)
        if (!(g_mips_taken >> p[j] & 1))
            g_mips_pool[k++] = p[j];
    *n = k;
    return g_mips_pool;
}

/* The PAIR pool, each pair named by its low register: the argument pairs
 * a0:a1 and a2:a3 (where a 64-bit value is passed, and every helper's
 * operands go), v0:v1 (where it comes back), then s0:s1 .. s6:s7 for one
 * that lives across a call. */
#define MIPS_NPAIRS 7
static const int MIPS_PAIRS[MIPS_NPAIRS] = {
    MIPS_A0, MIPS_A2, MIPS_V0, MIPS_S0, MIPS_S2, MIPS_S4, MIPS_S6
};
static const int *mips_pair_pool_for(const struct ir_func *fn, int *n)
{
    if (fn->is_varargs) {
        *n = MIPS_NPAIRS - 2;
        return MIPS_PAIRS + 2;
    }
    *n = MIPS_NPAIRS;
    return MIPS_PAIRS;
}

static void mips_pair_hints(const struct ir_func *fn, int *hint);

/* s0-s7 and fp. */
static int mips_callee_saved(int r)
{
    return (r >= MIPS_S0 && r <= MIPS_S7) || r == MIPS_FP;
}

/* `dst = load(local)` is a plain move at the full register width -- and
 * at MIPS64 a four-byte SIGNED read at four-byte width too, because
 * IR_STVAR sign-extends a narrowing store (LoongArch's and RV64's rule). */
static int mips_ldvar_plain(int size, int sign, int w)
{
    if (target_get() == TARGET_MIPS64)
        return (size == 8 && w == 8) || (size == 4 && sign && w == 4);
    (void)sign;
    return size == 4 && w == 4;
}

/* Which instructions become a CALL the IR does not show as one: every
 * floating-point operation (soft float), and a 64-bit divide. */
int mips_op_calls_helper(const struct ir_ins *i)
{
    if (i->flt)
        return i->op == IR_ADD || i->op == IR_SUB || i->op == IR_MUL ||
               i->op == IR_DIV || i->op == IR_CMP;
    if (i->op == IR_I2F || i->op == IR_F2I || i->op == IR_F2F)
        return 1;
    /* a divide of two registers' width: 64 bits at MIPS32, 128 at MIPS64 */
    return (i->op == IR_DIV || i->op == IR_MOD) &&
           i->w == (target_get() == TARGET_MIPS64 ? 16 : 8);
}

static void mips_abi_hints(const struct ir_func *fn, int *hint);

static const struct ra_target MIPS_RATGT = {
    mips_pool_for,
    mips_callee_saved,
    mips_ldvar_plain,
    1, 1, 1,        /* call args, returns and memcpy addresses from registers:
                     * the call setup is one parallel move (gen_call) */
    mips_op_calls_helper,
    0,              /* three-operand */
    mips_abi_hints,
    NULL, NULL,     /* no FP class: soft float in the integer registers */
    1,              /* ...allocated with them (float_in_gpr) */
    NULL, NULL,
    1,              /* atomic_in_reg: the ll/sc loops read through rdr */
    0,
    0               /* asm_in_reg: an asm's operands go through memory, as
                     * on x86-64, AArch64 and AVR (regalloc.h) */
};

static int g_mips_regalloc;

/* ---- refusal ------------------------------------------------------------ */

static void mips_refuse(const struct mips_fn *F, const struct ir_ins *i,
                        const char *what)
{
    char op[64];
    op[0] = 0;
    if (i)
        snprintf(op, sizeof op, " [%s w=%d size=%d]", ir_opname(i->op),
                 i->w, i->size);
    fprintf(stderr,
            "embcc: %s:%d: error: the %s backend cannot lower %s yet "
            "(function %s)%s\n",
            F->fn->file ? F->fn->file : "?",
            i ? i->line : F->fn->line, g_m64 ? "MIPS64" : "MIPS32", what, F->fn->name, op);
    exit(1);
}

/* ---- which values are two registers wide --------------------------------
 *
 * RV32's rule, for the same reasons (riscv/codegen.c wide_map): by the
 * width of the RESULT, a local by its declared size, and through copies
 * that do not say a register's width, to a fixed point. Two registers is
 * eight bytes at MIPS32 and sixteen (__int128, long double) at MIPS64. */
static char *wide_map(struct ir_func *fn)
{
    char *w = xcalloc((size_t)(fn->nvregs ? fn->nvregs : 1), 1);
    int pw = 2 * W;
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        if (i->w != pw || i->dst < 0 || i->dst >= fn->nvregs)
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
        if (fn->locals[v].size == pw &&
            (fn->locals[v].is_int_or_ptr || fn->locals[v].is_scalar_float ||
             fn->locals[v].is_ldouble || fn->locals[v].is_int128))
            w[v] = 1;
    for (int again = 1; again;) {
        again = 0;
        for (int n = 0; n < fn->nins; n++) {
            const struct ir_ins *i = &fn->ins[n];
            int src;
            if (i->dst < 0 || i->dst >= fn->nvregs || w[i->dst])
                continue;
            /* a copy that says one register or less narrows (at
             * MIPS32, one that says four bytes) */
            int wid = g_m64 ? i->w == 0 || i->w > 8 : i->w != 4;
            if (i->op == IR_MOV)
                src = wid &&
                      i->a >= 0 && i->a < fn->nvregs && w[i->a];
            else if (i->op == IR_SELECT)
                src = wid &&
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

/* ---- the o32 calling convention ------------------------------------------
 *
 * Read off clang for mipsel-unknown-elf (docs/internals/mips32-plan.md):
 *
 *   * The arguments are laid out as a block in memory, each at its offset
 *     rounded up to its alignment -- at least 4, at most 8 -- in whole
 *     words. The first 16 bytes travel in a0-a3 and the rest are on the
 *     stack at the same offset from the caller's sp; the caller reserves
 *     those first 16 bytes too, as the callee's home area. So a long long
 *     after an int skips a1 and lands in a2:a3, and a double at offset 16
 *     is at sp+16.
 *   * A composite of ANY size is passed by value in that layout, its
 *     bytes packed into the words and split across a3 and the stack when
 *     it straddles them. Nothing goes by reference.
 *   * A variadic argument follows exactly the same rule.
 *   * A struct comes back through a hidden pointer in a0, which moves the
 *     block's start to offset 4, and the callee hands the pointer back in
 *     v0 -- except a _Complex float (v0, v1) and a _Complex double
 *     (v0:v1, a0:a1), which clang returns in registers.
 */
/* n64 (MIPS64) is the same block at twice the width, read off clang for
 * mips64el-none-elf: doubleword slots, the first 64 bytes in a0-a7, an
 * argument aligned to 16 (a long double, an aligned struct -- but NOT an
 * __int128 scalar, which clang places at any slot) at an even one, a
 * composite of any size by value, packed as `ld` would read it. The rest
 * is on the stack from the caller's sp+0 -- there is no home area -- and
 * a variadic callee keeps a0-a7 at the top of its own frame. */
struct argplace {
    int reg, nreg;       /* first argument register (0-3, 0-7) and how many */
    int nstk;            /* words on the stack */
    long stk;            /* the first stack word's offset from the sp the
                          * callee is entered with */
};

static void place_arg(int size, int align, long *blk, struct argplace *p)
{
    int w = W, nar = g_m64 ? 8 : 4;
    int words = (size + w - 1) / w;
    long off;
    if (align < w) align = w;
    if (align > 2 * w) align = 2 * w;
    off = (*blk + align - 1) & ~(long)(align - 1);
    if (off < (long)nar * w) {
        p->reg = (int)(off / w);
        p->nreg = words < nar - p->reg ? words : nar - p->reg;
    } else {
        p->reg = nar;
        p->nreg = 0;
    }
    p->nstk = words - p->nreg;
    p->stk = off + (long)w * p->nreg - (g_m64 ? 64 : 0);
    *blk = off + (long)w * words;
}

static int argreg(int n) { return g_m64 ? MIPS64_ARGREG[n] : mips_argreg[n]; }

static int arg_align(const struct ir_arg *a)
{
    if (a->is_struct)
        return a->align ? a->align : W;
    if (g_m64)
        return a->size > 8 && a->is_float ? 16 : 8;
    return a->size > 4 ? 8 : 4;
}

/* The bytes of a stack word a scalar argument occupies: the whole word,
 * except a float at big-endian n64, which GCC pads UPWARD -- its four
 * bytes are the doubleword's first, where an integer's (extended to the
 * whole doubleword) are its last -- and clang places and reads it there.
 * A variadic float was promoted to double, so this is only a named one. */
static int stk_scalar_size(const struct ir_arg *a)
{
    return g_m64 && g_be && a->is_float && a->size == 4 ? 4 : W;
}

/* ...and a call's argument k, which at n64 may be VARIADIC: an unnamed
 * __int128 starts at an even slot, as clang's va_arg and GCC both read
 * it, where clang places a named one at any slot (and its own variadic
 * calls, which its va_arg then misreads, likewise). */
static int call_arg_align(const struct ir_ins *i, int k)
{
    const struct ir_arg *a = &i->argv[k];
    if (g_m64 && !a->is_struct && a->size > 8 && i->call_varargs &&
        k >= i->call_nfixed)
        return 16;
    return arg_align(a);
}

/* How many words of a returned composite come back in registers rather
 * than through the hidden pointer: a _Complex float's two (v0, v1) and a
 * _Complex double's four (v0, v1, a0, a1), as clang returns them; 0 for
 * every other struct. -1 for a complex of integers, whose convention has
 * not been checked against clang and so is refused. */
static int ret_reg_words(const struct type *t)
{
    if (!t || !t->is_complex || !t->celem)
        return 0;
    if (!ty_is_float(t->celem))
        return -1;
    return ty_size(t) == 8 ? 2 : ty_size(t) == 16 ? 4 : -1;
}

/* The register that word q of a register-returned composite is in. */
static int ret_word_reg(int q)
{
    return q < 2 ? MIPS_V0 + q : MIPS_A0 + (q - 2);
}

/* n64 soft float returns a binary128 -- a long double, or a structure of
 * one -- in v0 and A0, not v0:v1: its first doubleword in memory in v0
 * and its second in a0 (clang's RetCC_F128SoftFloat, which follows GCC).
 * The low and high halves' registers, in the target's order. */
#define TF_RET_LO (g_be ? MIPS_A0 : MIPS_V0)
#define TF_RET_HI (g_be ? MIPS_V0 : MIPS_A0)

/* n64's composite results (clang's MipsABIInfo for N32/N64): at most 16
 * bytes come back in v0 and v1, larger ones through a hidden pointer in
 * a0. ret_pieces gives each register's offset and size, and *fields says
 * how a piece sits in its register:
 *
 *   0  the composite's bytes as the doublewords `ld` would read from
 *      memory, the last one packed (left-justified big-endian);
 *   1  a structure of one or two floating-point fields, the first at
 *      offset 0: each FIELD in its own register, as `ld` at the field
 *      would read it -- so a float is the register's low half
 *      little-endian and its HIGH half big-endian;
 *   2  a structure of one long double: binary128's two doublewords in
 *      v0 and a0, as a long double result (TF_RET_*);
 *   3  a _Complex float or double: each part in its own register as a
 *      scalar of its type, a float sign-extended in the low half.
 *
 * 0 for a hidden pointer, -1 for a complex of integers, which is refused
 * as at o32. */
static int ret_pieces(const struct type *t, long size, int *off, int *sz,
                      int *fields)
{
    *fields = 0;
    if (!g_m64) {
        int rw = ret_reg_words(t);
        for (int q = 0; q < rw; q++) {
            off[q] = 4 * q;
            sz[q] = 4;
        }
        return rw;
    }
    if (!t || size > 16 || size <= 0)
        return 0;
    if (t->is_complex) {
        if (!t->celem || !ty_is_float(t->celem))
            return -1;
        off[0] = 0; sz[0] = (int)size / 2;
        off[1] = (int)size / 2; sz[1] = (int)size / 2;
        *fields = 3;
        return 2;
    }
    if (t->kind == TY_STRUCT && !t->is_union && t->nmembers == 1 &&
        !t->members[0].is_bitfield && t->members[0].ty &&
        t->members[0].ty->kind == TY_LDOUBLE) {
        /* one binary128 field: its doublewords in v0 and a0 */
        off[0] = 0; sz[0] = 8;
        off[1] = 8; sz[1] = 8;
        *fields = 2;
        return 2;
    }
    if (t->kind == TY_STRUCT && !t->is_union && t->nmembers >= 1 &&
        t->nmembers <= 2 && t->members[0].off == 0) {
        int k, ok = 1;
        for (k = 0; k < t->nmembers; k++) {
            const struct member *m = &t->members[k];
            if (m->is_bitfield || !m->ty ||
                (m->ty->kind != TY_FLOAT && m->ty->kind != TY_DOUBLE))
                ok = 0;
        }
        if (ok) {
            for (k = 0; k < t->nmembers; k++) {
                off[k] = t->members[k].off;
                sz[k] = (int)ty_size(t->members[k].ty);
            }
            *fields = 1;
            return t->nmembers;
        }
    }
    {
        int n = (int)((size + 7) / 8);
        for (int q = 0; q < n; q++) {
            off[q] = 8 * q;
            sz[q] = size - 8 * q < 8 ? (int)(size - 8 * q) : 8;
        }
        return n;
    }
}

static int ret_n(const struct type *t, long size)
{
    int off[4], sz[4], fl;
    return ret_pieces(t, size, off, sz, &fl);
}

static int fn_sret(const struct ir_func *fn)
{
    return fn->ret_abi.is_struct &&
           ret_n(fn->ret_abi.ty, fn->ret_abi.size) == 0;
}

static int call_sret(const struct ir_ins *i)
{
    return i->retsize && ret_n(i->rety, i->retsize) == 0;
}

/* Where the ABI would put each value: a parameter in the register it
 * arrives in, a call's arguments in theirs, a result in v0, and a soft
 * float helper's operands in a0 and a1. Hints only; the parallel moves
 * at the prologue and each call are what is correct regardless. */
static void mips_abi_hints(const struct ir_func *fn, int *hint)
{
    long blk = fn_sret(fn) ? W : 0;
    struct argplace pl;
    for (int p = 0; fn->src && p < fn->nparams && p < fn->nvregs; p++) {
        const struct ir_arg *a = &fn->param_abi[p];
        place_arg(a->size, arg_align(a), &blk, &pl);
        if (pl.nreg == 1 && !pl.nstk && !a->is_struct && a->size <= W)
            hint[p] = argreg(pl.reg);
    }
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        if (i->op == IR_RET && i->a >= 0 && i->a < fn->nvregs &&
            !fn->ret_abi.is_struct && fn->ret_abi.size <= W)
            hint[i->a] = MIPS_V0;
        if (i->op != IR_CALL && mips_op_calls_helper(i) && i->w <= W) {
            if (i->a >= 0 && i->a < fn->nvregs && hint[i->a] < 0)
                hint[i->a] = MIPS_A0;
            if (!i->imm_b && i->b >= 0 && i->b < fn->nvregs &&
                hint[i->b] < 0)
                hint[i->b] = MIPS_A1;
            if (i->dst >= 0 && i->dst < fn->nvregs && hint[i->dst] < 0)
                hint[i->dst] = MIPS_V0;
        }
        if (i->op != IR_CALL)
            continue;
        if (!i->retsize && i->dst >= 0 && i->dst < fn->nvregs && i->w <= W)
            hint[i->dst] = MIPS_V0;
        blk = call_sret(i) ? W : 0;
        for (int k = 0; k < i->nargs; k++) {
            const struct ir_arg *a = &i->argv[k];
            place_arg(a->size, call_arg_align(i, k), &blk, &pl);
            if (pl.nreg == 1 && !pl.nstk && !a->is_struct && a->size <= W &&
                a->vreg >= 0 && a->vreg < fn->nvregs)
                hint[a->vreg] = argreg(pl.reg);
        }
    }
}

/* ---- the frame -------------------------------------------------------------
 *
 * From sp upward: the outgoing argument block (at least the 16-byte home
 * area in any function that calls), the shared temp slots, 64-bit temps
 * without a pair, locals (small ones first), the struct-return scratch,
 * the sret pointer, the callee-saved registers and ra. A multiple of 8.
 * A variadic function's a0-a3 go in its CALLER's home area, at the top
 * of this frame plus 0..15, which makes the named and unnamed arguments
 * one block. */
/* n64: 16, and no home area -- the outgoing block is only what goes on
 * the stack, and a variadic function's a0-a7 are kept at the top of its
 * own frame (64 bytes, va_base), just below its incoming stack words. */
#define STACK_ALIGN (g_m64 ? 16 : 8)

static long outgoing_area(const struct mips_fn *F)
{
    const struct ir_func *fn = F->fn;
    long most = g_m64 ? 0 : 16;
    if (F->leaf)
        return 0;
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        struct argplace pl;
        long blk;
        if (i->op != IR_CALL)
            continue;
        blk = call_sret(i) ? W : 0;
        for (int k = 0; k < i->nargs; k++)
            place_arg(i->argv[k].size, call_arg_align(i, k), &blk, &pl);
        if (g_m64)
            blk -= 64;
        if (blk > most)
            most = blk;
    }
    return (most + STACK_ALIGN - 1) & ~(long)(STACK_ALIGN - 1);
}

static int in_reg(const struct mips_fn *F, int v);

static void layout(struct mips_fn *F)
{
    struct ir_func *fn = F->fn;
    long off = outgoing_area(F);
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
            struct ra_slots so = { loc2, NULL, g_mips_regalloc, has_cgoto };
            int *tslot = ra_coalesce_temps(fn, fn->nvars, &so, &npool);
            for (int v = fn->nvars; v < nv; v++) {
                int k = v - fn->nvars;
                if (loc2[v] >= 0 || !tslot || tslot[k] < 0)
                    continue;
                F->slot[v] = off + (long)tslot[k] * W;
            }
            off += (long)npool * W;
            free(tslot);
        }
        for (int v = fn->nvars; v < nv; v++) {
            if (!F->wide[v] || in_reg(F, v))
                continue;
            off = (off + 2 * W - 1) & ~(long)(2 * W - 1);
            F->slot[v] = off;
            off += 2 * W;
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
                if ((size > 2 * W) != pass)
                    continue;
                /* a variable is a whole register's slot at least: rd and
                 * wrote move it as one */
                if (align < W) align = W;
                if (size == 2 * W && align < 2 * W) align = 2 * W;
                if (size < W) size = W;
                off = (off + align - 1) & ~(long)(align - 1);
                F->slot[v] = off;
                off += size;
            }
        free(lref);
    }
    F->scratch_at = (off + 2 * W - 1) & ~(long)(2 * W - 1);
    off = F->scratch_at + ((fn->scratch_bytes + W - 1) & ~(long)(W - 1));

    F->sret_slot = -1;
    if (fn_sret(fn)) {
        off = (off + W - 1) & ~(long)(W - 1);
        F->sret_slot = off;
        off += W;
    }
    {
        /* an interrupt handler keeps ra with what it saves (isr_x) */
        int raw = F->leaf || F->isr ? 0 : W;
        long vsz = g_m64 && fn->is_varargs ? 64 : 0;
        long need = off + raw + (long)F->nsave * W + vsz;
        F->frame = (need + STACK_ALIGN - 1) & ~(long)(STACK_ALIGN - 1);
        F->ra_slot = F->frame - vsz - raw;
        F->save_at = F->ra_slot - (long)F->nsave * W;
        F->va_base = g_m64 ? F->frame - 64 : F->frame;
    }
    /* An interrupt handler's saves, at the very top, so nothing below
     * moves with how many there are: EPC and Status, the registers, and
     * HI and LO. */
    F->isr_at = F->frame;
    F->isr_bytes = 0;
    if (F->isr) {
        int n = 2 + (F->isr_hilo ? 2 : 0);
        for (int r = 0; r < 32; r++)
            n += (int)(F->isr_x >> r & 1);
        F->isr_bytes = ((long)n * 4 + STACK_ALIGN - 1) &
                       ~(long)(STACK_ALIGN - 1);
        F->frame += F->isr_bytes;
    }
    F->va_first = -1;
}

/* ---- reading and writing a vreg ----------------------------------------- */

static int fits16(long off) { return off >= -32768 && off <= 32767; }

/* A constant into a register: all 64 bits at MIPS64, the low 32 at
 * MIPS32. */
static void li(struct code *t, int rd, long long v)
{
    if (g_m64) mips_li64(t, rd, v);
    else       mips_li(t, rd, v);
}

/* A load from memory as the IR means it: a four-byte unsigned one at
 * MIPS64 ZERO-extends (lwu), as a 4-byte unsigned load does on every
 * other 64-bit target here, which the optimizer may rely on; lw
 * sign-extends. */
static void ld_mem(struct code *t, int rt, int base, int off, int size,
                   int sign)
{
    if (g_m64 && size == 4 && !sign)
        mips_lwu(t, rt, base, off);
    else
        mips_load(t, rt, base, off, size, sign);
}

/* base + a frame offset too large for a 16-bit field, in FAR. */
static int far_addr(struct mips_fn *F, int base, long off)
{
    mips_li(F->t, FAR, off);
    mips_alu(F->t, P_ADDU, FAR, base, FAR);
    return FAR;
}

static void ld_sp(struct mips_fn *F, int reg, long off, int size, int sign)
{
    if (fits16(off + 4)) {
        ld_mem(F->t, reg, F->fb, (int)off, size, sign);
        return;
    }
    ld_mem(F->t, reg, far_addr(F, F->fb, off), 0, size, sign);
}

static void st_sp(struct mips_fn *F, int reg, long off, int size)
{
    if (fits16(off + 4)) {
        mips_store(F->t, reg, F->fb, (int)off, size);
        return;
    }
    mips_store(F->t, reg, far_addr(F, F->fb, off), 0, size);
}

/* A store into the OUTGOING block, at the live sp: the callee finds its
 * stack arguments at its own entry sp, which after a VLA is not the
 * frame base. */
static void st_out(struct mips_fn *F, int reg, long off, int size)
{
    if (fits16(off + 4)) {
        mips_store(F->t, reg, MIPS_SP, (int)off, size);
        return;
    }
    mips_store(F->t, reg, far_addr(F, MIPS_SP, off), 0, size);
}

/* frame base + off, into `reg`. */
static void addr_sp(struct mips_fn *F, int reg, long off)
{
    if (fits16(off)) {
        mips_alu_imm(F->t, P_ADDIU, reg, F->fb, off);
        return;
    }
    mips_li(F->t, reg, off);
    mips_alu(F->t, P_ADDU, reg, F->fb, reg);
}

static int in_reg(const struct mips_fn *F, int v)
{
    return F->loc && v >= 0 && F->loc[v] >= 0;
}

/* A value's slot, for code that addresses it directly. One with no slot
 * -- in a register, or never stored -- reaching such a path would read
 * memory nothing wrote, so it is an internal error instead. */
static long sslot(const struct mips_fn *F, int v)
{
    if (v < 0 || v >= F->fn->nvregs || F->slot[v] < 0)
        internal_error("mips: %s: a path addresses vreg %d's slot, and it "
                       "has none", F->fn->name, v);
    return F->slot[v];
}

/* rd: v into exactly `reg`. rdr: where v IS (its register, or `scratch`
 * after a load). wreg: where to compute v. wrote: commit it if that was
 * a scratch. wr: v from `reg`. */
/* A 64-bit vreg read or written at 32 bits (copy propagation lets any
 * operation read one at its own width) is its LOW word: PLO of its pair,
 * or WLO into its slot. */
static int is_wide(const struct mips_fn *F, int v)
{
    return F->wide && v >= 0 && v < F->fn->nvregs && F->wide[v];
}

static int reg_of(const struct mips_fn *F, int v)
{
    return is_wide(F, v) ? PLO(F->loc[v]) : F->loc[v];
}

static long slot32(const struct mips_fn *F, int v)
{
    return sslot(F, v) + (is_wide(F, v) ? WLO : 0);
}

/* Where variable v's OBJECT is. A variable is also read and written as a
 * vreg -- a whole word at its slot (rd, wrote, a parameter's incoming
 * register) -- and its slot is a word even when it is a char or a short
 * (layout gives every local four bytes at least). Little-endian the
 * object is the word's first bytes, its low end; big-endian its low end
 * is the word's LAST bytes, so a narrow integer variable lives there, at
 * slot + 4 - size, and its address, its loads and its stores all say so. */
static long obj_slot(const struct mips_fn *F, int v)
{
    if (g_be && v < F->fn->nvars) {
        const struct ir_local *L = &F->fn->locals[v];
        /* ...unless it asked for an alignment its own size does not
         * give (`_Alignas(16) char`, or a typedef's aligned(8) on a
         * four-byte int): then at the slot's start, which is aligned.
         * Nothing writes such a local as a whole word -- a parameter
         * cannot carry an alignment specifier, and irgen gives one its
         * type's own alignment. */
        if ((L->is_int_or_ptr || L->is_scalar_float) && L->size > 0 &&
            L->size < W && L->user_align <= L->size && L->align <= L->size)
            return sslot(F, v) + W - L->size;
    }
    return sslot(F, v);
}

/* Where a `size`-byte access of variable v is. The IR only ever reads and
 * writes a variable at its own size (opt refuses to forward between
 * differing ones on a big-endian target), so this is the object itself;
 * were a narrower access of a wider variable to reach here, its VALUE's
 * low bytes are at the object's end when big-endian. */
static long var_slot(const struct mips_fn *F, int v, int size)
{
    long vs = v < F->fn->nvars ? F->fn->locals[v].size
            : is_wide(F, v) ? 2 * W : W;
    return obj_slot(F, v) + (g_be && vs > size ? vs - size : 0);
}

static void rd(struct mips_fn *F, int v, int reg)
{
    if (in_reg(F, v)) {
        if (reg_of(F, v) != reg)
            mips_mv(F->t, reg, reg_of(F, v));
        return;
    }
    ld_sp(F, reg, slot32(F, v), W, 1);
}

static int rdr(struct mips_fn *F, int v, int scratch)
{
    if (in_reg(F, v))
        return reg_of(F, v);
    ld_sp(F, scratch, slot32(F, v), W, 1);
    return scratch;
}

static int wreg(struct mips_fn *F, int v, int scratch)
{
    return in_reg(F, v) ? reg_of(F, v) : scratch;
}

static void wrote(struct mips_fn *F, int v, int reg)
{
    if (in_reg(F, v)) {
        if (reg_of(F, v) != reg)
            mips_mv(F->t, reg_of(F, v), reg);
        return;
    }
    if (v < 0 || F->slot[v] < 0)
        return;
    st_sp(F, reg, slot32(F, v), W);
}

static void wr(struct mips_fn *F, int v, int reg)
{
    wrote(F, v, reg);
}

/* dl <- sl and dh <- sh as one parallel move. */
static void mv2(struct mips_fn *F, int dl, int sl, int dh, int sh)
{
    if (dl == sh && dh == sl) {
        if (dl == sl) return;
        mips_mv(F->t, SCR, sl);
        mips_mv(F->t, dh, sh);
        mips_mv(F->t, dl, SCR);
        return;
    }
    if (dl == sh) {                    /* dh first, before sh is lost */
        if (dh != sh) mips_mv(F->t, dh, sh);
        if (dl != sl) mips_mv(F->t, dl, sl);
        return;
    }
    if (dl != sl) mips_mv(F->t, dl, sl);
    if (dh != sh) mips_mv(F->t, dh, sh);
}

/* A 64-bit value: its pair (low word in PLO(loc), high in PHI(loc)), or
 * its eight-aligned slot in the target's order (WLO, WHI). */
static void rd64(struct mips_fn *F, int v, int lo, int hi)
{
    if (in_reg(F, v)) {
        mv2(F, lo, PLO(F->loc[v]), hi, PHI(F->loc[v]));
        return;
    }
    ld_sp(F, lo, sslot(F, v) + WLO, W, 1);
    ld_sp(F, hi, sslot(F, v) + WHI, W, 1);
}

static void wr64(struct mips_fn *F, int v, int lo, int hi)
{
    if (in_reg(F, v)) {
        mv2(F, PLO(F->loc[v]), lo, PHI(F->loc[v]), hi);
        return;
    }
    if (v < 0 || F->slot[v] < 0)
        return;
    st_sp(F, lo, sslot(F, v) + WLO, W);
    st_sp(F, hi, sslot(F, v) + WHI, W);
}

/* A folded constant as the register holds it: its low 32 bits,
 * sign-extended -- and at MIPS64 for an operation on a whole doubleword,
 * all of it. */
static long long imm_val(const struct ir_ins *i)
{
    if (g_m64 && i->w > 4)
        return (long long)i->imm;
    return (long long)(int)(unsigned int)(unsigned long)i->imm;
}

/* An instruction's second operand into `reg`: the folded immediate, or b.
 * Every binary operation asks here; none reads i->b when imm_b is set. */
static void operand_b(struct mips_fn *F, const struct ir_ins *i, int reg)
{
    if (i->imm_b)
        li(F->t, reg, imm_val(i));
    else
        rd(F, i->b, reg);
}

static void operand_b64(struct mips_fn *F, const struct ir_ins *i,
                        int lo, int hi)
{
    if (i->imm_b && g_m64) {          /* sign-extended, as irgen made it */
        mips_li64(F->t, lo, (long long)i->imm);
        mips_li64(F->t, hi, i->imm < 0 ? -1 : 0);
    } else if (i->imm_b) {
        mips_li(F->t, lo, (long long)(i->imm & 0xffffffffL));
        mips_li(F->t, hi, (long long)((i->imm >> 32) & 0xffffffffL));
    } else {
        rd64(F, i->b, lo, hi);
    }
}

/* Sign- or zero-extend the low `size` bytes of rs: Release 2's seb/seh,
 * or an andi. */
static void ext_reg(struct mips_fn *F, int rdst, int rs, int size, int sign)
{
    if (size >= W) {
        if (rdst != rs)
            mips_mv(F->t, rdst, rs);
        return;
    }
    if (size == 4) {                  /* MIPS64: a word to a doubleword */
        if (sign) mips_shift_imm(F->t, MIPS_SLL, rdst, rs, 0);
        else      mips_dext(F->t, rdst, rs, 0, 32);
        return;
    }
    if (sign) {
        if (size == 1) mips_seb(F->t, rdst, rs);
        else           mips_seh(F->t, rdst, rs);
        return;
    }
    mips_alu_imm(F->t, MIPS_ANDI, rdst, rs, size == 1 ? 0xff : 0xffff);
}

/* ---- 32-bit values at MIPS64 --------------------------------------------
 *
 * n64's invariant, and the MIPS64 32-bit instructions' precondition: a
 * register holding a 32-bit value holds its SIGN-EXTENSION, unsigned or
 * not. addu, subu, mul, div, the shifts and lw keep it on the way out; an
 * addu of an operand that is not sign-extended is UNPREDICTABLE. But the
 * IR narrows for nothing -- `(int)some_long` is the same temp read at
 * width 4 -- and lwu, a zero-extending local read and a doubleword
 * operation break it. So the readers that need it ask (rd32), and this
 * map, the LoongArch and RV64 backends' sext_map, says which values have
 * it already. A value is sign-extended when EVERY definition leaves it
 * so; anything not listed is assumed not to be. */
static int sext_def(const struct mips_fn *F, const struct ir_ins *i,
                    const char *sx)
{
    struct ir_func *fn = F->fn;
    int nv = fn->nvregs;
#define SX(v) ((v) >= 0 && (v) < nv && sx[v])
    switch (i->op) {
    case IR_CONST: {
        long long v = imm_val(i);
        return i->w <= 8 && v == (long long)(int)v;
    }
    case IR_MOV:
        return SX(i->a);
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
    case IR_SHL: case IR_SHR: case IR_NEG:
        return !i->flt && i->w == 4;          /* addu, mul, div, sll... */
    case IR_AND: case IR_OR: case IR_XOR:
        if (i->op == IR_AND && !i->flt && i->imm_b && i->w <= 8 &&
            imm_val(i) >= 0 && imm_val(i) <= 0x7fffffffLL)
            return 1;
        if (i->flt || i->w > 8 || !SX(i->a))
            return 0;
        if (i->imm_b) {
            long long v = imm_val(i);
            return v == (long long)(int)v;
        }
        return SX(i->b);
    case IR_BNOT:                             /* nor rd, rs, $0 */
        return i->w <= 8 && SX(i->a);
    case IR_CMP:
        return 1;                             /* 0 or 1 */
    case IR_LOAD:
        return i->size < 4 || (i->size == 4 && i->sign);
    case IR_LDVAR:
        if (i->size < 4)
            return 1;
        if (i->size != 4 || !i->sign)
            return 0;
        return !in_reg(F, i->a) ||
               (i->a < fn->nvars && fn->locals[i->a].size == 4);
    case IR_EXT:
        return i->w <= 8 && (i->size < 4 || (i->size == 4 && i->sign));
    case IR_SELECT:
        return SX(i->b) && SX(i->c);
    case IR_CALL:                             /* the callee's, by the ABI */
        return !i->flt && !i->retsize && i->ret_tybytes > 0 &&
               i->ret_tybytes <= 4;
    case IR_F2I:                              /* __fix*si, by the ABI */
        return i->w == 4;
    default:
        return 0;
    }
#undef SX
}

static char *sext_map(const struct mips_fn *F)
{
    struct ir_func *fn = F->fn;
    int nv = fn->nvregs, changed = 1;
    char *sx = xmalloc((size_t)(nv ? nv : 1));
    /* Optimistic, then cut down to a fixed point: a loop's accumulator
     * is sign-extended if its entry value and its update both are. */
    for (int v = 0; v < nv; v++)
        sx[v] = v >= fn->nvars;               /* a local is not a value */
    while (changed) {
        changed = 0;
        for (int n = 0; n < fn->nins; n++) {
            const struct ir_ins *i = &fn->ins[n];
            if (i->op == IR_STVAR || i->dst < 0 || i->dst >= nv ||
                !sx[i->dst])
                continue;
            if (!sext_def(F, i, sx)) {
                sx[i->dst] = 0;
                changed = 1;
            }
        }
    }
    return sx;
}

/* `v`, already in register `r`, as a 32-bit value a 32-bit instruction
 * may read: `r` itself when it is sign-extended already, else `scratch`
 * holding the extension (sll 0). At MIPS32 there is nothing to do. */
static int sext32(struct mips_fn *F, int v, int r, int scratch)
{
    if (F->sx && (v < 0 || v >= F->fn->nvregs || !F->sx[v])) {
        mips_shift_imm(F->t, MIPS_SLL, scratch, r, 0);
        return scratch;
    }
    return r;
}

static int rd32(struct mips_fn *F, int v, int scratch)
{
    return sext32(F, v, rdr(F, v, scratch), scratch);
}

/* An operand of an operation at width w: rd32 for a 32-bit one at
 * MIPS64, the register as it is otherwise. */
static int rdw(struct mips_fn *F, const struct ir_ins *i, int v, int scratch)
{
    return g_m64 && i->w >= 1 && i->w <= 4 ? rd32(F, v, scratch)
                                           : rdr(F, v, scratch);
}

/* ---- loads and stores that may be misaligned -----------------------------
 *
 * A word access to an address that is not a multiple of its size traps
 * (Address Error). C promises alignment everywhere but a packed struct's
 * member, and irgen marks the accesses it can promise (ir_ins.natural);
 * the rest go through these. A word is lwl/lwr (swl/swr), the halves of
 * one unaligned word in either order; a halfword is two bytes. `rt` may
 * be `base` for a load: the value is built in SCR2 when they collide. */
static void ld_any(struct mips_fn *F, int rt, int base, int off, int size,
                   int sign, int aligned)
{
    struct code *t = F->t;
    if (aligned || size == 1) {
        ld_mem(t, rt, base, off, size, sign);
        return;
    }
    if (size == 8) {                  /* MIPS64: the doubleword halves */
        int r = rt == base ? SCR2 : rt;
        mips_ldl(t, r, base, g_be ? off : off + 7);
        mips_ldr(t, r, base, g_be ? off + 7 : off);
        if (r != rt)
            mips_mv(t, rt, r);
        return;
    }
    if (size == 4) {
        int r = rt == base ? SCR2 : rt;
        mips_lwl(t, r, base, g_be ? off : off + 3);
        mips_lwr(t, r, base, g_be ? off + 3 : off);
        /* MIPS64: lwr into a register lwl filled leaves the upper half
         * as lwl's sign extension, which the manual does not promise for
         * every implementation: made the word's extension here, as the
         * load asked for it */
        if (g_m64) {
            if (sign) mips_shift_imm(t, MIPS_SLL, rt, r, 0);
            else      mips_dext(t, rt, r, 0, 32);
            return;
        }
        if (r != rt)
            mips_mv(t, rt, r);
        return;
    }
    /* size 2: the high byte, extended as the value is, then the low --
     * at off + 1 and off little-endian, off and off + 1 big-endian */
    mips_load(t, SCR2, base, g_be ? off : off + 1, 1, sign);
    mips_load(t, rt, base, g_be ? off + 1 : off, 1, 0);
    mips_shift_imm(t, MIPS_SLL, SCR2, SCR2, 8);
    mips_alu(t, MIPS_OR, rt, rt, SCR2);
}

static void st_any(struct mips_fn *F, int rt, int base, int off, int size,
                   int aligned)
{
    struct code *t = F->t;
    if (aligned || size == 1) {
        mips_store(t, rt, base, off, size);
        return;
    }
    if (size == 8) {
        mips_sdl(t, rt, base, g_be ? off : off + 7);
        mips_sdr(t, rt, base, g_be ? off + 7 : off);
        return;
    }
    if (size == 4) {
        mips_swl(t, rt, base, g_be ? off : off + 3);
        mips_swr(t, rt, base, g_be ? off + 3 : off);
        return;
    }
    mips_store(t, rt, base, g_be ? off + 1 : off, 1);           /* low */
    mips_shift_imm(t, MIPS_SRL, SCR2, rt, 8);
    mips_store(t, SCR2, base, g_be ? off : off + 1, 1);          /* high */
}

/* ---- delay slots -----------------------------------------------------------
 *
 * A transfer's delay slot holds the instruction before it, when that is
 * safe, rather than a nop: the classic fill. The slot runs after the
 * transfer is decided and before its target, on both paths of a branch,
 * so moving X from just before a transfer B into B's slot keeps the
 * program when
 *
 *   - X is an ordinary computation, load or store (decoded below; a
 *     transfer, trap, sync, ll/sc, or anything not decoded stays),
 *   - B does not read what X writes (a branch's operands, jalr's target),
 *   - B links (jal, jalr) and X neither reads nor writes $ra, which B has
 *     already written when the slot runs,
 *   - nothing may jump to B itself: no label, no landing, no other
 *     transfer, prologue or asm block lies between X and B (F->barrier),
 *   - X is not a relocated field (its site would move), and is not a nop.
 *
 * A label AT X is fine: a jump there now runs B and then X, and B does
 * not depend on X. */

/* What a word reads and writes, as register bitmasks; 0 when it may not
 * be moved. */
static int slot_decode(unsigned long w, unsigned long *rd_mask,
                       unsigned long *wr_mask)
{
    int op = (int)(w >> 26), rs = (int)(w >> 21) & 31,
        rt = (int)(w >> 16) & 31, rd = (int)(w >> 11) & 31,
        fn = (int)(w & 63);
    unsigned long Rm = 0, Wm = 0;
    if (w == 0)
        return 0;                                   /* a nop: no gain */
    switch (op) {
    case 0x00:                                      /* SPECIAL */
        switch (fn) {
        case 0x00: case 0x02: case 0x03:            /* sll srl/rotr sra */
            Rm = 1UL << rt; Wm = 1UL << rd; break;
        case 0x04: case 0x06: case 0x07:            /* sllv srlv/rotrv srav */
            Rm = (1UL << rs) | (1UL << rt); Wm = 1UL << rd; break;
        case 0x0a: case 0x0b:                       /* movz movn */
            Rm = (1UL << rs) | (1UL << rt) | (1UL << rd); Wm = 1UL << rd; break;
        case 0x10: case 0x12:                       /* mfhi mflo */
            Wm = 1UL << rd; break;
        case 0x11: case 0x13:                       /* mthi mtlo */
            Rm = 1UL << rs; break;
        case 0x18: case 0x19: case 0x1a: case 0x1b: /* mult(u) div(u) */
            Rm = (1UL << rs) | (1UL << rt); break;
        case 0x21: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27:
        case 0x2a: case 0x2b:                       /* addu .. sltu */
        case 0x14: case 0x16: case 0x17:            /* dsllv dsrlv dsrav */
        case 0x2d: case 0x2f:                       /* daddu dsubu */
            Rm = (1UL << rs) | (1UL << rt); Wm = 1UL << rd; break;
        case 0x1c: case 0x1d: case 0x1e: case 0x1f: /* dmult(u) ddiv(u) */
            Rm = (1UL << rs) | (1UL << rt); break;
        case 0x38: case 0x3a: case 0x3b:            /* dsll dsrl dsra */
        case 0x3c: case 0x3e: case 0x3f:            /* ...and the *32 */
            Rm = 1UL << rt; Wm = 1UL << rd; break;
        default:
            return 0;
        }
        break;
    case 0x1c:                                      /* SPECIAL2 */
        if (fn == 0x02) { Rm = (1UL << rs) | (1UL << rt); Wm = 1UL << rd; }
        else if (fn == 0x20 || fn == 0x21 || fn == 0x24 || fn == 0x25)
            { Rm = 1UL << rs; Wm = 1UL << rd; }       /* (d)clz (d)clo */
        else return 0;
        break;
    case 0x1f:                                      /* SPECIAL3 */
        if (fn <= 0x03) { Rm = 1UL << rs; Wm = 1UL << rt; }   /* (d)ext* */
        else if (fn >= 0x04 && fn <= 0x07)                  /* (d)ins* */
            { Rm = (1UL << rs) | (1UL << rt); Wm = 1UL << rt; }
        else if (fn == 0x20 || fn == 0x24)          /* bshfl dbshfl */
            { Rm = 1UL << rt; Wm = 1UL << rd; }
        else return 0;
        break;
    case 0x09: case 0x0a: case 0x0b: case 0x0c: case 0x0d: case 0x0e:
    case 0x19:                                      /* daddiu */
        Rm = 1UL << rs; Wm = 1UL << rt; break;        /* addiu .. xori */
    case 0x0f:
        Wm = 1UL << rt; break;                       /* lui */
    case 0x20: case 0x21: case 0x23: case 0x24: case 0x25:
    case 0x27: case 0x37:                           /* lwu ld */
        Rm = 1UL << rs; Wm = 1UL << rt; break;        /* loads */
    case 0x22: case 0x26:                           /* lwl lwr: merge */
    case 0x1a: case 0x1b:                           /* ldl ldr */
        Rm = (1UL << rs) | (1UL << rt); Wm = 1UL << rt; break;
    case 0x28: case 0x29: case 0x2a: case 0x2b: case 0x2e:
    case 0x2c: case 0x2d: case 0x3f:                /* sdl sdr sd */
        Rm = (1UL << rs) | (1UL << rt); break;       /* stores */
    default:
        return 0;
    }
    *rd_mask = Rm & ~1UL;
    *wr_mask = Wm & ~1UL;
    return 1;
}

/* Is the unit's last relocation site, of any kind, at `off`? Sites are
 * noted in offset order, so the last of each list is the latest. */
static int site_at(const struct mips_fn *F, int off)
{
    const struct mips_sites *st = F->st;
    return (st->next && st->ext[st->next - 1].patch_off == off) ||
           (st->nstr && st->str[st->nstr - 1].patch_off == off) ||
           (st->ng && st->g[st->ng - 1].patch_off == off) ||
           (st->nf && st->f[st->nf - 1].patch_off == off) ||
           (F->nfix && F->fix[F->nfix - 1].at == off);
}

/* Before a transfer that reads `reads` (and writes $ra when `links`):
 * take the instruction just emitted out of the stream, if it may go in
 * the slot. Returns it, or -1 for a nop slot. */
static long take_slot(struct mips_fn *F, unsigned long reads, int links)
{
    struct code *t = F->t;
    int at = t->len - 4;
    unsigned long w, r, wr;
    if (!F->fill || at < F->barrier || site_at(F, at))
        return -1;
    w = mips_rdw(t, at);
    if (!slot_decode(w, &r, &wr))
        return -1;
    if (wr & reads)
        return -1;
    if (links && ((r | wr) & (1UL << MIPS_RA)))
        return -1;
    t->len = at;
    return (long)w;
}

/* The slot after a transfer: what take_slot took, or a nop; and nothing
 * before this point may be moved again. */
static void put_slot(struct mips_fn *F, long w)
{
    if (w >= 0)
        mips_w(F->t, (unsigned long)w);
    else
        mips_nop(F->t);
    F->barrier = F->t->len;
}

/* ---- branches, with their delay slots ------------------------------------
 *
 * Every transfer is followed by a nop in its delay slot, here and
 * nowhere else. A branch to a label is resolved when the function ends,
 * with no relocation: +-128 KiB from the delay slot, and a function whose
 * branches reach further is refused (the long form would need the label's
 * absolute address, a relocation against this object's own text). */
/* A branch whose target is patched later, with its nop. */
static int br_place(struct mips_fn *F, int cond, int rs, int rt)
{
    int at = mips_b_placeholder(F->t, cond, rs, rt);
    put_slot(F, -1);
    return at;
}

/* ...pointed here, for a branch within one lowering. */
static void br_land(struct mips_fn *F, int at)
{
    F->barrier = F->t->len;          /* a landing: a target */
    if (!mips_patch_b(F->t, at, F->t->len))
        internal_error("mips: %s: a branch inside one operation does not "
                       "reach", F->fn->name);
}

/* ...or pointed back at `target`, for a loop. */
static void br_back(struct mips_fn *F, int at, int target)
{
    if (!mips_patch_b(F->t, at, target))
        internal_error("mips: %s: a loop inside one operation does not "
                       "reach", F->fn->name);
}

/* FX_TAB: a jump table's word, the label's offset from `base`.
 * FX_ADDR: &&label, the function-address sites from index `base` on,
 * whose addend becomes the label's offset in the function. */
enum { FX_B, FX_J, FX_TAB, FX_ADDR };

static void want_label(struct mips_fn *F, int at, int label, int kind)
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

/* Does the next branch to a label take the long form? */
static int want_long(const struct mips_fn *F)
{
    return F->longb && F->nfix < F->nlongb && F->longb[F->nfix];
}

/* The long form of a branch: the inverse branch over a `j`, which reaches
 * anywhere in the 256 MiB region and is relocated against .text. */
static int inverse_mips_cond(int c)
{
    switch (c) {
    case MIPS_BEQ:  return MIPS_BNE;
    case MIPS_BNE:  return MIPS_BEQ;
    case MIPS_BLEZ: return MIPS_BGTZ;
    case MIPS_BGTZ: return MIPS_BLEZ;
    case MIPS_BLTZ: return MIPS_BGEZ;
    default:        return MIPS_BLTZ;      /* MIPS_BGEZ */
    }
}

static void branch_to(struct mips_fn *F, int cond, int rs, int rt, int label)
{
    if (want_long(F)) {
        int at;
        if (!(cond == MIPS_BEQ && rs == MIPS_ZERO && rt == MIPS_ZERO)) {
            /* over the j and its delay slot: 12 bytes from this slot */
            mips_w(F->t, mips_enc_branch(inverse_mips_cond(cond), rs, rt, 12));
            mips_nop(F->t);
        }
        at = F->t->len;
        mips_j(F->t);
        mips_nop(F->t);
        want_label(F, at, label, FX_J);
        return;
    }
    {
        unsigned long reads = (1UL << rs) | (1UL << rt);
        long slot = take_slot(F, reads, cond == MIPS_BAL);
        int at = mips_b_placeholder(F->t, cond, rs, rt);
        put_slot(F, slot);
        want_label(F, at, label, FX_B);
    }
}

static void jump_to(struct mips_fn *F, int label)
{
    branch_to(F, MIPS_BEQ, MIPS_ZERO, MIPS_ZERO, label);
}

/* The conditions a fused compare-and-branch asks for, as RISC-V's
 * branches spell them; GT and LE are LT and GE with the operands
 * swapped, by the caller. */
enum { C_EQ, C_NE, C_LT, C_GE, C_LTU, C_GEU };

static int invert_cond(int c)
{
    switch (c) {
    case C_EQ:  return C_NE;
    case C_NE:  return C_EQ;
    case C_LT:  return C_GE;
    case C_GE:  return C_LT;
    case C_LTU: return C_GEU;
    default:    return C_LTU;      /* C_GEU */
    }
}

/* Branch to `label` when `rs cond rt`. beq/bne compare two registers;
 * the signed tests against zero have their own branches (bltz, bgez,
 * bgtz, blez); anything else is slt/sltu into $at and a test of that. */
static void branch_if(struct mips_fn *F, int cond, int rs, int rt, int label)
{
    switch (cond) {
    case C_EQ:
        branch_to(F, MIPS_BEQ, rs, rt, label);
        return;
    case C_NE:
        branch_to(F, MIPS_BNE, rs, rt, label);
        return;
    case C_LT: case C_GE:
        if (rt == MIPS_ZERO) {
            branch_to(F, cond == C_LT ? MIPS_BLTZ : MIPS_BGEZ, rs, 0, label);
        } else if (rs == MIPS_ZERO) {        /* 0 < rt: rt > 0 */
            branch_to(F, cond == C_LT ? MIPS_BGTZ : MIPS_BLEZ, rt, 0, label);
        } else {
            mips_alu(F->t, MIPS_SLT, CC, rs, rt);
            branch_to(F, cond == C_LT ? MIPS_BNE : MIPS_BEQ, CC, MIPS_ZERO,
                      label);
        }
        return;
    default:                                /* C_LTU, C_GEU */
        if (rt == MIPS_ZERO) {
            if (cond == C_LTU)
                return;                     /* x <u 0: never */
            branch_to(F, MIPS_BEQ, MIPS_ZERO, MIPS_ZERO, label);  /* always */
        } else if (rs == MIPS_ZERO) {       /* 0 <u rt: rt != 0 */
            branch_to(F, cond == C_LTU ? MIPS_BNE : MIPS_BEQ, rt, MIPS_ZERO,
                      label);
        } else {
            mips_alu(F->t, MIPS_SLTU, CC, rs, rt);
            branch_to(F, cond == C_LTU ? MIPS_BNE : MIPS_BEQ, CC, MIPS_ZERO,
                      label);
        }
        return;
    }
}

/* ---- site lists ----------------------------------------------------------- */

static void note_ext(struct mips_sites *st, int at, struct func *callee,
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

static void note_str(struct mips_sites *st, int at, int idx, enum reloc_kind k)
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

static void note_glob(struct mips_sites *st, int at, struct global *g,
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

static void note_fn(struct mips_sites *st, int at, struct func *target,
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

/* `lui rd, 0; addiu rd, rd, 0`, an absolute address in two halves, the
 * HI16 site noted before its LO16 (the AHL rule needs them in that order
 * in the relocation table). Returns the lui's offset. */
static int abs_pair(struct mips_fn *F, int rd)
{
    int at = F->t->len;
    mips_lui(F->t, rd, 0);
    if (g_m64) {                     /* %highest, %higher, %hi, %lo */
        mips_alu_imm(F->t, MIPS_DADDIU, rd, rd, 0);
        mips_shift_imm(F->t, MIPS_DSLL, rd, rd, 16);
        mips_alu_imm(F->t, MIPS_DADDIU, rd, rd, 0);
        mips_shift_imm(F->t, MIPS_DSLL, rd, rd, 16);
        mips_alu_imm(F->t, MIPS_DADDIU, rd, rd, 0);
        return at;
    }
    mips_alu_imm(F->t, MIPS_ADDIU, rd, rd, 0);
    return at;
}

/* A call: jal with an R_MIPS_26, and the nop in its slot. Every call is
 * relocated, even to a function in this unit, because jal holds an
 * ABSOLUTE word index that only the linker knows. */
static void call_sym(struct mips_fn *F, struct func *callee, int tail)
{
    long slot = take_slot(F, 0, !tail);
    int at = F->t->len;
    if (tail) mips_j(F->t);
    else      mips_jal(F->t);
    note_ext(F->st, at, callee, tail);
    put_slot(F, slot);
}

/* The runtime helpers, interned by name. */
static struct func **g_helpers;
static int g_nhelpers, g_caphelpers;

static void call_helper(struct mips_fn *F, const char *name)
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

/* ---- soft float ------------------------------------------------------------
 *
 * No FPU under -msoft-float: every floating-point operation is a libgcc
 * call (lib/rt/softfp.c), a float in one integer register and a double
 * in a pair, results in v0 (v0:v1). */
/* (w 16 is binary128, MIPS64's long double: the tf helpers) */
static const char *fp_binop_name(enum ir_op op, int w)
{
    switch (op) {
    case IR_ADD: return w == 16 ? "__addtf3" : w == 8 ? "__adddf3" : "__addsf3";
    case IR_SUB: return w == 16 ? "__subtf3" : w == 8 ? "__subdf3" : "__subsf3";
    case IR_MUL: return w == 16 ? "__multf3" : w == 8 ? "__muldf3" : "__mulsf3";
    case IR_DIV: return w == 16 ? "__divtf3" : w == 8 ? "__divdf3" : "__divsf3";
    default:     return NULL;
    }
}

static const char *fp_cmp_name(enum binop pred, int w)
{
    switch (pred) {
    case B_EQ: return w == 16 ? "__eqtf2" : w == 8 ? "__eqdf2" : "__eqsf2";
    case B_NE: return w == 16 ? "__netf2" : w == 8 ? "__nedf2" : "__nesf2";
    case B_LT: return w == 16 ? "__lttf2" : w == 8 ? "__ltdf2" : "__ltsf2";
    case B_LE: return w == 16 ? "__letf2" : w == 8 ? "__ledf2" : "__lesf2";
    case B_GT: return w == 16 ? "__gttf2" : w == 8 ? "__gtdf2" : "__gtsf2";
    default:   return w == 16 ? "__getf2" : w == 8 ? "__gedf2" : "__gesf2";
    }
}

/* Put n vregs into the registers a helper (or a call) expects, all at
 * once: the register-to-register edges as one parallel move (SCR breaks
 * a cycle), then the loads, which only write. `half` (may be NULL) picks
 * the low (0) or high (1) word of a 64-bit value; without it a vreg is
 * read at 32 bits, which of a 64-bit one is its low word. */
static void set_args_half(struct mips_fn *F, const int *dstreg,
                          const int *vreg, const int *half, int n)
{
    int pd[RA_MAXPOOL], ps[RA_MAXPOOL], npm = 0;

    for (int k = 0; k < n; k++)
        if (in_reg(F, vreg[k])) {
            pd[npm] = dstreg[k];
            ps[npm] = !half ? reg_of(F, vreg[k])
                    : !is_wide(F, vreg[k]) ? F->loc[vreg[k]] + half[k]
                    : half[k] ? PHI(F->loc[vreg[k]]) : PLO(F->loc[vreg[k]]);
            /* (at MIPS64 a pair value is never in registers, so the
             * middle case does not arise there) */
            npm++;
        }
    if (npm) {
        int od[RA_MAXPOOL * 2], os[RA_MAXPOOL * 2];
        int m = ra_parallel_move(pd, ps, npm, SCR, od, os,
                                 (int)(sizeof od / sizeof od[0]));
        if (m < 0)
            internal_error("mips: an argument setup is not a well-formed "
                           "move");
        for (int k = 0; k < m; k++)
            mips_mv(F->t, od[k], os[k]);
    }
    for (int k = 0; k < n; k++)
        if (!in_reg(F, vreg[k]))
            ld_sp(F, dstreg[k],
                  !half ? slot32(F, vreg[k])
                  : !is_wide(F, vreg[k]) ? sslot(F, vreg[k]) + (long)W * half[k]
                  : sslot(F, vreg[k]) + (half[k] ? WHI : WLO), W, 1);
}

/* MIPS64: argument registers holding 32-bit values, as n64 passes them --
 * sign-extended -- where the value is not known to be already. */
static void sext_regs(struct mips_fn *F, const int *reg, const int *vreg,
                      const int *size, int n)
{
    for (int k = 0; F->sx && k < n; k++)
        if (size[k] <= 4)
            sext32(F, vreg[k], reg[k], reg[k]);
}

static void set_args(struct mips_fn *F, const int *dstreg, const int *vreg,
                     int n)
{
    set_args_half(F, dstreg, vreg, NULL, n);
}

/* Two 64-bit operands into a0:a1 and a2:a3 (vb < 0: only the first),
 * each pair as the ABI orders it (PLO, PHI). */
static void args64x2(struct mips_fn *F, int va, int vb)
{
    int d[4] = { PLO(MIPS_A0), PHI(MIPS_A0), PLO(MIPS_A2), PHI(MIPS_A2) };
    int v[4], h[4] = { 0, 1, 0, 1 };
    v[0] = v[1] = va;
    v[2] = v[3] = vb;
    set_args_half(F, d, v, h, vb >= 0 ? 4 : 2);
}

static void fp_args2(struct mips_fn *F, const struct ir_ins *i)
{
    if (i->w == 2 * W) {
        args64x2(F, i->a, i->b);
        return;
    }
    {
        int dstreg[2], vreg[2], size[2];
        dstreg[0] = MIPS_A0; vreg[0] = i->a;
        dstreg[1] = MIPS_A1; vreg[1] = i->b;
        set_args(F, dstreg, vreg, 2);
        size[0] = size[1] = i->w;
        sext_regs(F, dstreg, vreg, size, 2);
    }
}

static void fp_result(struct mips_fn *F, int dst, int w)
{
    if (dst < 0)
        return;
    if (w == 2 * W && g_m64) wr64(F, dst, TF_RET_LO, TF_RET_HI);
    else if (w == 2 * W)     wr64(F, dst, PLO(MIPS_V0), PHI(MIPS_V0));
    else                     wr(F, dst, MIPS_V0);
}

/* ---- comparisons -----------------------------------------------------------
 *
 * A 0 or 1 is slt/sltu, the only comparison computed into a register, an
 * operand swap to reverse it and xori 1 to invert it; == and != go
 * through xor first. */
static void cmp_to_reg(struct mips_fn *F, enum binop pred, int sign,
                       int ra, int rb, int dst)
{
    struct code *t = F->t;
    int slt = sign ? MIPS_SLT : MIPS_SLTU;
    switch (pred) {
    case B_EQ:
        mips_alu(t, MIPS_XOR, dst, ra, rb);
        mips_alu_imm(t, MIPS_SLTIU, dst, dst, 1);             /* seqz */
        return;
    case B_NE:
        mips_alu(t, MIPS_XOR, dst, ra, rb);
        mips_alu(t, MIPS_SLTU, dst, MIPS_ZERO, dst);          /* snez */
        return;
    case B_LT:
        mips_alu(t, slt, dst, ra, rb);
        return;
    case B_GT:
        mips_alu(t, slt, dst, rb, ra);
        return;
    case B_GE:
        mips_alu(t, slt, dst, ra, rb);
        mips_alu_imm(t, MIPS_XORI, dst, dst, 1);
        return;
    default: /* B_LE */
        mips_alu(t, slt, dst, rb, ra);
        mips_alu_imm(t, MIPS_XORI, dst, dst, 1);
        return;
    }
}

/* The same against a constant k (sign-extended from 32 bits): slti and
 * sltiu take a SIGNED 16-bit field, `x <= k` is `x < k + 1`, `x > k` its
 * inverse; == and != are an xori (k in 0..65535) or an addiu of -k, then
 * the zero test. Returns 0 where k does not fit, and the caller loads it. */
static int cmp_imm_to_reg(struct mips_fn *F, enum binop pred, int sign,
                          int ra, long long k, int dst, int w)
{
    struct code *t = F->t;
    int inv = pred == B_GE || pred == B_GT;
    if (pred == B_EQ || pred == B_NE) {
        if (k) {
            if (k > 0 && k <= 0xffff)
                mips_alu_imm(t, MIPS_XORI, dst, ra, k);
            else if (-k >= -32768 && -k <= 32767)
                mips_alu_imm(t, MIPS_ADDIU, dst, ra, -k);
            else
                return 0;
            ra = dst;
        }
        if (pred == B_EQ) mips_alu_imm(t, MIPS_SLTIU, dst, ra, 1);
        else              mips_alu(t, MIPS_SLTU, dst, MIPS_ZERO, ra);
        return 1;
    }
    if (pred == B_LE || pred == B_GT) {
        if (!sign && k == -1)
            return 0;               /* k + 1 wraps: x <=u max */
        if (sign && k == 0x7fffffffffffffffLL)
            return 0;               /* ...or x <= the largest doubleword */
        k++;
        /* a 32-bit unsigned bound past 2^31-1 is, in a register, its sign
         * extension (at MIPS64 a doubleword bound is what it says) */
        if (!sign && k == 0x80000000LL && w <= 4)
            k = -2147483647LL - 1;  /* the field is sign-extended */
    }
    if (k < -32768 || k > 32767)
        return 0;
    if (k == 0 && sign)
        mips_alu(t, MIPS_SLT, dst, ra, MIPS_ZERO);
    else
        mips_alu_imm(t, sign ? MIPS_SLTI : MIPS_SLTIU, dst, ra, k);
    if (inv)
        mips_alu_imm(t, MIPS_XORI, dst, dst, 1);
    return 1;
}

/* ---- 64-bit integers, in register pairs -----------------------------------
 *
 * No carry flag: after `sum = alo + blo`, `sltu c, sum, blo` is the carry,
 * as on RISC-V. The operations work on A_LO:A_HI and B_LO:B_HI unless a
 * case says otherwise.
 *
 * At MIPS64 the same code is the 128-bit arithmetic: each half a
 * doubleword (W), the shifts the doubleword ones over HB = 64 bits, the
 * values always in their slots (rd64/wr64 load and store the halves). */
#define HB (8 * W)              /* a register's bits: a half's */

/* The shift at the register's width: dsll and its kin at MIPS64. */
static int sh_w(int op)
{
    if (!g_m64)
        return op;
    return op == MIPS_SLL ? MIPS_DSLL : op == MIPS_SRL ? MIPS_DSRL
         : op == MIPS_SRA ? MIPS_DSRA : MIPS_DROTR;
}

static int shv_w(int op)
{
    if (!g_m64)
        return op;
    return op == MIPS_SLLV ? MIPS_DSLLV : op == MIPS_SRLV ? MIPS_DSRLV
                                                          : MIPS_DSRAV;
}

static void add64(struct mips_fn *F)
{
    struct code *t = F->t;
    mips_alu(t, P_ADDU, SCR, A_LO, B_LO);
    mips_alu(t, MIPS_SLTU, SCR2, SCR, B_LO);      /* did it wrap? */
    mips_mv(t, A_LO, SCR);
    mips_alu(t, P_ADDU, A_HI, A_HI, B_HI);
    mips_alu(t, P_ADDU, A_HI, A_HI, SCR2);
}

static void sub64(struct mips_fn *F)
{
    struct code *t = F->t;
    mips_alu(t, MIPS_SLTU, SCR2, A_LO, B_LO);     /* will it borrow? */
    mips_alu(t, P_SUBU, A_LO, A_LO, B_LO);
    mips_alu(t, P_SUBU, A_HI, A_HI, B_HI);
    mips_alu(t, P_SUBU, A_HI, A_HI, SCR2);
}

/* A shift by a constant from (al, ah) into (dl, dh): the same pair or
 * one sharing no register with it. `op` is MIPS_SLL or MIPS_SRL. */
static void shift64_imm_to(struct mips_fn *F, int op, int sign, long n,
                           int al, int ah, int dl, int dh)
{
    struct code *t = F->t;
    n &= 2 * HB - 1;
    if (n == 0) {
        if (dl != al) mips_mv(t, dl, al);
        if (dh != ah) mips_mv(t, dh, ah);
        return;
    }
    if (n >= HB) {
        int k = (int)(n - HB);
        if (op == MIPS_SLL) {
            if (k) mips_shift_imm(t, sh_w(MIPS_SLL), dh, al, k);
            else if (dh != al) mips_mv(t, dh, al);
            mips_mv(t, dl, MIPS_ZERO);
        } else {
            if (k) mips_shift_imm(t, sh_w(sign ? MIPS_SRA : MIPS_SRL), dl, ah,
                                  k);
            else if (dl != ah) mips_mv(t, dl, ah);
            if (sign) mips_shift_imm(t, sh_w(MIPS_SRA), dh, ah, HB - 1);
            else      mips_mv(t, dh, MIPS_ZERO);
        }
        return;
    }
    if (op == MIPS_SLL) {
        mips_shift_imm(t, sh_w(MIPS_SRL), SCR, al, (int)(HB - n));
        mips_shift_imm(t, sh_w(MIPS_SLL), dh, ah, (int)n);
        mips_alu(t, MIPS_OR, dh, dh, SCR);
        mips_shift_imm(t, sh_w(MIPS_SLL), dl, al, (int)n);
    } else {
        mips_shift_imm(t, sh_w(MIPS_SLL), SCR, ah, (int)(HB - n));
        mips_shift_imm(t, sh_w(MIPS_SRL), dl, al, (int)n);
        mips_alu(t, MIPS_OR, dl, dl, SCR);
        mips_shift_imm(t, sh_w(sign ? MIPS_SRA : MIPS_SRL), dh, ah, (int)n);
    }
}

/* A shift of A_LO:A_HI by the count in B_LO, in three arms. The variable
 * shifts take the count's low five bits, so the complementary shift by
 * 32 - n is a shift by 0 when n is 0 and the halves would mix: n == 0 is
 * an arm of its own, as on RISC-V. */
static void shift64_var(struct mips_fn *F, int left, int sign)
{
    struct code *t = F->t;
    int big, zero, done1, done2;

    mips_alu_imm(t, MIPS_ANDI, B_LO, B_LO, 2 * HB - 1);
    mips_alu_imm(t, MIPS_ANDI, SCR2, B_LO, HB);
    big = br_place(F, MIPS_BNE, SCR2, MIPS_ZERO);    /* count >= HB */
    zero = br_place(F, MIPS_BEQ, B_LO, MIPS_ZERO);
    /* 0 < count < HB: B_HI = -count, which the shifts read as HB - count */
    mips_alu(t, MIPS_SUBU, B_HI, MIPS_ZERO, B_LO);
    if (left) {
        mips_alu(t, shv_w(MIPS_SLLV), A_HI, A_HI, B_LO);
        mips_alu(t, shv_w(MIPS_SRLV), SCR, A_LO, B_HI);
        mips_alu(t, MIPS_OR, A_HI, A_HI, SCR);
        mips_alu(t, shv_w(MIPS_SLLV), A_LO, A_LO, B_LO);
    } else {
        mips_alu(t, shv_w(MIPS_SRLV), A_LO, A_LO, B_LO);
        mips_alu(t, shv_w(MIPS_SLLV), SCR, A_HI, B_HI);
        mips_alu(t, MIPS_OR, A_LO, A_LO, SCR);
        mips_alu(t, shv_w(sign ? MIPS_SRAV : MIPS_SRLV), A_HI, A_HI, B_LO);
    }
    done1 = br_place(F, MIPS_BEQ, MIPS_ZERO, MIPS_ZERO);
    br_land(F, big);
    /* count >= 32: the halves move wholesale, shifted by count - 32,
     * which is what the variable shifts read out of count's low bits */
    if (left) {
        mips_alu(t, shv_w(MIPS_SLLV), A_HI, A_LO, B_LO);
        mips_mv(t, A_LO, MIPS_ZERO);
    } else if (sign) {
        mips_alu(t, shv_w(MIPS_SRAV), A_LO, A_HI, B_LO);
        mips_shift_imm(t, sh_w(MIPS_SRA), A_HI, A_HI, HB - 1);
    } else {
        mips_alu(t, shv_w(MIPS_SRLV), A_LO, A_HI, B_LO);
        mips_mv(t, A_HI, MIPS_ZERO);
    }
    done2 = br_place(F, MIPS_BEQ, MIPS_ZERO, MIPS_ZERO);
    br_land(F, zero);                     /* count == 0: nothing to do */
    br_land(F, done1);
    br_land(F, done2);
}

/* d = s OP c for one 32-bit half of a 64-bit AND/OR/XOR with a constant:
 * the identity is a copy, AND with 0 a zero, OR with all ones an li, a
 * 16-bit c andi/ori/xori, an AND with a run of low bits an ext and with
 * all but low bits an ins of $0; else c is built in SCR. d may be s;
 * neither is SCR. */
static void logic_half(struct mips_fn *F, int op, int d, int s,
                       unsigned long c)
{
    struct code *t = F->t;
    unsigned long nc;
    c &= 0xffffffffUL;
    nc = ~c & 0xffffffffUL;
    if ((op == MIPS_AND && c == 0xffffffffUL) || (op != MIPS_AND && c == 0)) {
        if (d != s) mips_mv(t, d, s);
        return;
    }
    if (op == MIPS_AND && c == 0) {
        mips_mv(t, d, MIPS_ZERO);
        return;
    }
    if (op == MIPS_OR && c == 0xffffffffUL) {
        mips_li(t, d, -1);
        return;
    }
    if (c <= 0xffff) {
        mips_alu_imm(t, op == MIPS_AND ? MIPS_ANDI : op == MIPS_OR ? MIPS_ORI
                                                   : MIPS_XORI, d, s,
                     (long long)c);
        return;
    }
    if (op == MIPS_AND && (c & (c + 1)) == 0) {          /* low k bits */
        int k = 0;
        while (c >> k & 1) k++;
        mips_ext(t, d, s, 0, k);
        return;
    }
    if (op == MIPS_AND && (nc & (nc + 1)) == 0) {        /* all but low j */
        int j = 0;
        while (nc >> j & 1) j++;
        if (d != s) mips_mv(t, d, s);
        mips_ins(t, d, MIPS_ZERO, 0, j);
        return;
    }
    mips_li(t, SCR, (long long)(int)(unsigned int)c);
    mips_alu(t, op, d, s, SCR);
}

/* The same for a whole doubleword at MIPS64: a 16-bit c is andi/ori/xori,
 * an AND with a run of low bits dext and with all but low bits a dins of
 * $0; else c is built in SCR. */
static void logic_dw(struct mips_fn *F, int op, int d, int s,
                     unsigned long long c)
{
    struct code *t = F->t;
    unsigned long long nc = ~c;
    if ((op == MIPS_AND && c == ~0ULL) || (op != MIPS_AND && c == 0)) {
        if (d != s) mips_mv(t, d, s);
        return;
    }
    if (op == MIPS_AND && c == 0) {
        mips_mv(t, d, MIPS_ZERO);
        return;
    }
    if (op == MIPS_OR && c == ~0ULL) {
        mips_li64(t, d, -1);
        return;
    }
    if (c <= 0xffff) {
        mips_alu_imm(t, op == MIPS_AND ? MIPS_ANDI : op == MIPS_OR ? MIPS_ORI
                                                   : MIPS_XORI, d, s,
                     (long long)c);
        return;
    }
    if (op == MIPS_AND && (c & (c + 1)) == 0) {          /* low k bits */
        int k = 0;
        while (c >> k & 1) k++;
        mips_dext(t, d, s, 0, k);
        return;
    }
    if (op == MIPS_AND && (nc & (nc + 1)) == 0) {        /* all but low j */
        int j = 0;
        while (nc >> j & 1) j++;
        if (d != s) mips_mv(t, d, s);
        mips_dins(t, d, MIPS_ZERO, 0, j);
        return;
    }
    mips_li64(t, SCR, (long long)c);
    mips_alu(t, op, d, s, SCR);
}

/* Where a 64-bit operand's halves are: its pair, or the given scratches
 * after a load. And where to compute a 64-bit result: its pair, or A. */
static void src64(struct mips_fn *F, int v, int slo, int shi, int *lo, int *hi)
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

static void dst64(struct mips_fn *F, int v, int *lo, int *hi)
{
    *lo = in_reg(F, v) ? PLO(F->loc[v]) : A_LO;
    *hi = in_reg(F, v) ? PHI(F->loc[v]) : A_HI;
}

/* A 64-bit comparison into ACC: the high words decide unless they are
 * equal, and then the low words, compared UNSIGNED. */
static void cmp64(struct mips_fn *F, const struct ir_ins *i, enum binop pred,
                  int sign)
{
    struct code *t = F->t;
    int hi_ne, done, al, ah, bl, bh;

    src64(F, i->a, A_LO, A_HI, &al, &ah);
    if (i->imm_b && i->imm == 0) {
        if (pred == B_EQ || pred == B_NE) {
            mips_alu(t, MIPS_OR, SCR, al, ah);
            if (pred == B_EQ) mips_alu_imm(t, MIPS_SLTIU, ACC, SCR, 1);
            else              mips_alu(t, MIPS_SLTU, ACC, MIPS_ZERO, SCR);
            return;
        }
        if (sign && (pred == B_LT || pred == B_GE)) {
            mips_alu(t, MIPS_SLT, ACC, ah, MIPS_ZERO);
            if (pred == B_GE)
                mips_alu_imm(t, MIPS_XORI, ACC, ACC, 1);
            return;
        }
    }
    if (i->imm_b) {
        operand_b64(F, i, B_LO, B_HI);
        bl = B_LO; bh = B_HI;
    } else {
        src64(F, i->b, B_LO, B_HI, &bl, &bh);
    }
    if (pred == B_EQ || pred == B_NE) {
        mips_alu(t, MIPS_XOR, SCR, al, bl);
        mips_alu(t, MIPS_XOR, SCR2, ah, bh);
        mips_alu(t, MIPS_OR, SCR, SCR, SCR2);
        if (pred == B_EQ) mips_alu_imm(t, MIPS_SLTIU, ACC, SCR, 1);
        else              mips_alu(t, MIPS_SLTU, ACC, MIPS_ZERO, SCR);
        return;
    }
    hi_ne = br_place(F, MIPS_BNE, ah, bh);
    cmp_to_reg(F, pred, 0, al, bl, SCR);       /* equal highs: unsigned lows */
    done = br_place(F, MIPS_BEQ, MIPS_ZERO, MIPS_ZERO);
    br_land(F, hi_ne);
    cmp_to_reg(F, pred, sign, ah, bh, SCR);
    br_land(F, done);
    mips_mv(t, ACC, SCR);
}

/* Every 64-bit operation that is not a call. Returns 0 for one this does
 * not handle, which the caller refuses by name. */
static int gen_ins64(struct mips_fn *F, int n)
{
    struct ir_func *fn = F->fn;
    struct ir_ins *i = &fn->ins[n];
    struct code *t = F->t;

    switch (i->op) {
    case IR_CONST: {
        int lo, hi;
        dst64(F, i->dst, &lo, &hi);
        if (g_m64) {                  /* sign-extended, as irgen made it */
            mips_li64(t, lo, (long long)i->imm);
            mips_li64(t, hi, i->imm < 0 ? -1 : 0);
        } else {
            mips_li(t, lo, (long long)(i->imm & 0xffffffffL));
            mips_li(t, hi, (long long)((i->imm >> 32) & 0xffffffffL));
        }
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
        int op = i->op == IR_AND ? MIPS_AND : i->op == IR_OR ? MIPS_OR
                                                             : MIPS_XOR;
        int al, ah, bl, bh, dl, dh;
        src64(F, i->a, A_LO, A_HI, &al, &ah);
        if (i->imm_b && g_m64) {
            dst64(F, i->dst, &dl, &dh);
            logic_dw(F, op, dl, al, (unsigned long long)i->imm);
            logic_dw(F, op, dh, ah, i->imm < 0 ? ~0ULL : 0);
            wr64(F, i->dst, dl, dh);
            return 1;
        }
        if (i->imm_b) {
            dst64(F, i->dst, &dl, &dh);
            logic_half(F, op, dl, al, (unsigned long)i->imm);
            logic_half(F, op, dh, ah, (unsigned long)i->imm >> 32);
            wr64(F, i->dst, dl, dh);
            return 1;
        }
        src64(F, i->b, B_LO, B_HI, &bl, &bh);
        dst64(F, i->dst, &dl, &dh);
        mips_alu(t, op, dl, al, bl);
        mips_alu(t, op, dh, ah, bh);
        wr64(F, i->dst, dl, dh);
        return 1;
    }
    case IR_MUL:
        /* (ah:al) * (bh:bl) keeping 64 bits: multu gives al*bl whole,
         * and the cross terms reach only the high word. mul clobbers
         * HI/LO, so both are read first. */
        rd64(F, i->a, A_LO, A_HI);
        operand_b64(F, i, B_LO, B_HI);
        if (g_m64) {
            /* MIPS64r2 has no three-operand dmul: each product through
             * dmultu and LO (the low 64 bits are the same signed or not) */
            mips_muldiv(t, MIPS_DMULTU, A_LO, B_LO);
            mips_mflo(t, SCR);
            mips_mfhi(t, SCR2);
            mips_muldiv(t, MIPS_DMULTU, A_HI, B_LO);
            mips_mflo(t, A_HI);
            mips_alu(t, MIPS_DADDU, SCR2, SCR2, A_HI);
            mips_muldiv(t, MIPS_DMULTU, A_LO, B_HI);
            mips_mflo(t, A_HI);
            mips_alu(t, MIPS_DADDU, A_HI, SCR2, A_HI);
            mips_mv(t, A_LO, SCR);
            wr64(F, i->dst, A_LO, A_HI);
            return 1;
        }
        mips_muldiv(t, MIPS_MULTU, A_LO, B_LO);
        mips_mflo(t, SCR);
        mips_mfhi(t, SCR2);
        mips_alu(t, MIPS_MUL, A_HI, A_HI, B_LO);
        mips_alu(t, MIPS_ADDU, SCR2, SCR2, A_HI);
        mips_alu(t, MIPS_MUL, A_HI, A_LO, B_HI);
        mips_alu(t, MIPS_ADDU, A_HI, SCR2, A_HI);
        mips_mv(t, A_LO, SCR);
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    case IR_MULW: {
        /* mult/multu: the whole 64-bit product of two words in HI:LO,
         * read out a half at a time (MIPS32 only: target_has_mulh) */
        int ra_ = rdr(F, i->a, B_LO), rb_ = rdr(F, i->b, B_HI), dl, dh;
        if (g_m64)
            mips_refuse(F, i, "a 32-bit widening multiply at MIPS64");
        mips_muldiv(t, i->sign ? MIPS_MULT : MIPS_MULTU, ra_, rb_);
        dst64(F, i->dst, &dl, &dh);
        mips_mflo(t, dl);
        mips_mfhi(t, dh);
        wr64(F, i->dst, dl, dh);
        return 1;
    }
    case IR_NEG: {
        int al, ah, dl, dh;
        src64(F, i->a, A_LO, A_HI, &al, &ah);
        dst64(F, i->dst, &dl, &dh);
        mips_alu(t, MIPS_SLTU, SCR, MIPS_ZERO, al);     /* borrow out of 0-lo */
        mips_alu(t, P_SUBU, dl, MIPS_ZERO, al);
        mips_alu(t, P_SUBU, dh, MIPS_ZERO, ah);
        mips_alu(t, P_SUBU, dh, dh, SCR);
        wr64(F, i->dst, dl, dh);
        return 1;
    }
    case IR_BNOT: {
        int al, ah, dl, dh;
        src64(F, i->a, A_LO, A_HI, &al, &ah);
        dst64(F, i->dst, &dl, &dh);
        mips_alu(t, MIPS_NOR, dl, al, MIPS_ZERO);
        mips_alu(t, MIPS_NOR, dh, ah, MIPS_ZERO);
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
            shift64_imm_to(F, i->op == IR_SHL ? MIPS_SLL : MIPS_SRL, sign,
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
    case IR_EXT:
        /* to 64 bits: the low word is the value, the high its sign or 0 */
        rd(F, i->a, A_LO);
        if (i->size < W)
            ext_reg(F, A_LO, A_LO, i->size, i->sign);
        if (i->sign) mips_shift_imm(t, sh_w(MIPS_SRA), A_HI, A_LO, HB - 1);
        else         mips_mv(t, A_HI, MIPS_ZERO);
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    /* An access is `size` bytes whatever the value's width: a four-byte
     * store of an eight-byte value stores its low word, a narrower read
     * extends into the high one (riscv/codegen.c says why). */
    case IR_LDVAR:
        if (i->size == 2 * W) {
            rd64(F, i->a, A_LO, A_HI);
        } else {
            if (in_reg(F, i->a))
                ext_reg(F, A_LO, reg_of(F, i->a), i->size, i->sign);
            else
                ld_sp(F, A_LO, var_slot(F, i->a, i->size), i->size, i->sign);
            if (i->sign) mips_shift_imm(t, sh_w(MIPS_SRA), A_HI, A_LO, HB - 1);
            else         mips_mv(t, A_HI, MIPS_ZERO);
        }
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    case IR_STVAR:
        rd64(F, i->a, A_LO, A_HI);
        if (i->size == 2 * W)
            wr64(F, i->dst, A_LO, A_HI);
        else if (in_reg(F, i->dst))
            ext_reg(F, reg_of(F, i->dst), A_LO, i->size, 1);
        else if (F->slot[i->dst] >= 0)
            st_sp(F, A_LO, var_slot(F, i->dst, i->size), i->size);
        return 1;
    case IR_LOAD:
        rd(F, i->a, ADDR);
        if (i->size == 2 * W) {
            ld_any(F, A_LO, ADDR, WLO, W, 1, i->natural);
            ld_any(F, A_HI, ADDR, WHI, W, 1, i->natural);
        } else {
            ld_any(F, A_LO, ADDR, 0, i->size, i->sign, i->natural);
            if (i->sign) mips_shift_imm(t, sh_w(MIPS_SRA), A_HI, A_LO, HB - 1);
            else         mips_mv(t, A_HI, MIPS_ZERO);
        }
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    case IR_STORE:
        rd(F, i->a, ADDR);
        rd64(F, i->b, A_LO, A_HI);
        st_any(F, A_LO, ADDR, i->size == 2 * W ? WLO : 0,
               i->size == 2 * W ? W : i->size, i->natural);
        if (i->size == 2 * W)
            st_any(F, A_HI, ADDR, WHI, W, i->natural);
        return 1;
    case IR_SELECT: {
        /* dst = a ? b : c with movn: c into A, then b over it when the
         * condition (either half of a 64-bit one) is nonzero */
        int bl, bh;
        if (i->size == 2 * W) {
            int al, ah;
            src64(F, i->a, SCR, SCR2, &al, &ah);
            mips_alu(t, MIPS_OR, SCR, al, ah);
        } else if (g_m64 && i->size <= 4) {
            int c = rd32(F, i->a, SCR);
            if (c != SCR) mips_mv(t, SCR, c);
        } else {
            rd(F, i->a, SCR);
        }
        rd64(F, i->c, A_LO, A_HI);
        src64(F, i->b, B_LO, B_HI, &bl, &bh);
        mips_alu(t, MIPS_MOVN, A_LO, bl, SCR);
        mips_alu(t, MIPS_MOVN, A_HI, bh, SCR);
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    }
    default:
        return 0;
    }
}

/* ---- one call ------------------------------------------------------------ */

/* Can the call at n be a TAIL call: the frame torn down, then a `j`? Only
 * when nothing of this frame can still be needed and the IR_RET after it
 * returns exactly what the call returned -- RV32's conditions, plus every
 * argument in a0-a3 (a stack argument would be written into this frame's
 * outgoing block, which is gone). The callee uses the home area this
 * function's own caller reserved. */
static int mips_tail_ok(const struct mips_fn *F, int n)
{
    const struct ir_func *fn = F->fn;
    const struct ir_ins *i = &fn->ins[n], *r;
    struct argplace pl;
    long blk = 0;

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
                          i->ret_tybytes > W))
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
        place_arg(i->argv[k].size, call_arg_align(i, k), &blk, &pl);
        if (pl.nstk)
            return 0;
    }
    return 1;
}

/* The epilogue's restores: the callee-saved registers, ra, the frame.
 * Shared by the epilogue and a tail call. Touches nothing but the saved
 * registers, ra, sp and t0, so v0:v1 and a0-a3 survive it. */
/* The epilogue's restores, and with `ret` the return too: `jr ra` with
 * the frame's release in its delay slot -- the slot runs before the
 * jump lands, so the caller sees sp restored, and the slot is never a
 * nop when there is a frame to release. */
static void mips_restore(struct mips_fn *F, int ret)
{
    struct code *t = F->t;
    for (int k = 0; k < F->nsave; k++)
        ld_sp(F, F->used_callee[k], F->save_at + (long)k * W, W, 1);
    if (!F->leaf)
        ld_sp(F, MIPS_RA, F->ra_slot, W, 1);
    if (F->frame && fits16(F->frame)) {
        if (ret)
            mips_jr(t, MIPS_RA);
        mips_alu_imm(t, P_ADDIU, MIPS_SP, MIPS_SP, F->frame);
        return;
    }
    if (F->frame) {
        mips_li(t, A_LO, F->frame);
        mips_alu(t, P_ADDU, MIPS_SP, MIPS_SP, A_LO);
    }
    if (ret) {
        /* no frame (or one too big for addiu): the instruction before
         * may fill the slot, as before any transfer -- not one that
         * writes $ra, which jr reads */
        long slot = take_slot(F, 1UL << MIPS_RA, 0);
        mips_jr(t, MIPS_RA);
        put_slot(F, slot);
    }
}

/* ---- interrupt handlers ------------------------------------------------
 *
 * __attribute__((interrupt)) as GCC defines it for MIPS32 -- clang's is
 * the same -- with GCC's keep_interrupts_masked. The exception arrives
 * between two instructions of code with a value in every register, so
 * the handler leaves every register as it found it: the callee-saved
 * ones the ordinary way, and here
 *
 *   - each caller-saved register it WRITES -- at, v0-v1, a0-a3, t0-t9,
 *     ra -- and HI and LO if it writes either (a multiply or divide);
 *   - ALL of them, gp too, when it calls anything (soft float and the
 *     64-bit divisions are helper calls), which may use any of them.
 *
 * What it writes is decoded from the instructions it compiled to
 * (mips_scan_writes), and it is emitted again until it saves all of it,
 * as on RISC-V (riscv/codegen.c rv_isr_grow, which says why).
 *
 * It saves EPC and Status, because it runs with interrupts enabled again
 * and a nested one overwrites both: GCC's sequence, k0 and k1 being the
 * registers the ABI leaves to exception code --
 *
 *   mfc0 k0, Cause ; mfc0 k1, EPC ; addiu sp, sp, -frame ; sw k1, EPC
 *   mfc0 k1, Status ; ext k0, k0, 10, 6 ; sw k1, Status
 *   ins k1, k0, 10, 6        the RIPL as the new IPL ("eic", the default)
 *   ins k1, zero, 8, n+1     or IM0..IMn cleared ("vector=sw0".."hw5")
 *   ins k1, zero, 1, 4       KSU, ERL, EXL cleared: interrupts on
 *   mtc0 k1, Status ; the saves ... the restores
 *   di ; ehb ; EPC and Status back ; eret
 *
 * keep_interrupts_masked clears IE as well (ins k1, zero, 0, 5), reads
 * no Cause and needs no di. HI and LO go through k0, so they are saved
 * BEFORE the mtc0 that lets a nested interrupt in and restored AFTER the
 * di: a nested handler's own prologue writes k0. (clang stores them
 * after the mtc0 -- a nested interrupt between its mfhi k0 and sw k0
 * saves the wrong HI -- and stores EPC and Status before it moves sp,
 * into the interrupted code's stack; neither is copied here.)
 *
 * The plain form copies Cause.RIPL into Status.IPL, which is what an
 * External Interrupt Controller's priority level means; on a core
 * without one (malta's 24Kc) those bits are the pending lines, so a
 * handler there wants vector=hwN or keep_interrupts_masked -- as with
 * GCC. MIPS32r2 only: ext, ins, di and ehb are r2 instructions, and GCC
 * refuses the attribute below r2 too. */

/* The registers a call may clobber: at, v0-v1, a0-a3, t0-t9, ra. */
static int mips_isr_cand(int r)
{
    return (r >= MIPS_AT && r <= MIPS_T7) || r == MIPS_T8 || r == MIPS_T9 ||
           r == MIPS_RA;
}

/* What one word writes: a GPR mask, and whether HI or LO. A word not
 * recognised is taken to write its rt and rd fields. */
static void mips_writes(unsigned long w, unsigned long *xw, int *hilo)
{
    int op = (int)(w >> 26), rs = (int)(w >> 21) & 31,
        rt = (int)(w >> 16) & 31, rd = (int)(w >> 11) & 31,
        fn = (int)(w & 63);
    switch (op) {
    case 0x00:                                      /* SPECIAL */
        if (fn == 0x11 || fn == 0x13 || (fn >= 0x18 && fn <= 0x1f))
            *hilo = 1;                              /* mthi mtlo mult div */
        else if (fn != 0x08 && fn != 0x0c && fn != 0x0d && fn != 0x0f &&
                 (fn & 0x38) != 0x30)               /* jr syscall break sync
                                                     * traps: none */
            *xw |= 1UL << rd;
        break;
    case 0x1c:                                      /* SPECIAL2 */
        if (fn <= 0x05)                             /* madd(u) mul msub(u) */
            *hilo = 1;
        if (fn == 0x02 || fn >= 0x20)               /* mul clz clo */
            *xw |= 1UL << rd;
        break;
    case 0x01:                                      /* REGIMM */
        if (rt & 0x10)
            *xw |= 1UL << MIPS_RA;                  /* bltzal bgezal */
        break;
    case 0x03:                                      /* jal */
        *xw |= 1UL << MIPS_RA;
        break;
    case 0x02: case 0x04: case 0x05: case 0x06: case 0x07:
    case 0x14: case 0x15: case 0x16: case 0x17:     /* j, branches */
    case 0x28: case 0x29: case 0x2a: case 0x2b: case 0x2e:
    case 0x2c: case 0x2d: case 0x3f: case 0x2f: case 0x33:
    case 0x31: case 0x35: case 0x39: case 0x3d:     /* stores cache pref */
        break;
    case 0x10:                                      /* COP0 */
        if (rs == 0x00 || rs == 0x01 || rs == 0x0b)
            *xw |= 1UL << rt;                       /* mfc0 dmfc0 di ei */
        break;
    case 0x1f:                                      /* SPECIAL3 */
        if (fn <= 0x07 || fn == 0x3b)               /* (d)ext* (d)ins* rdhwr:
                                                     * rd is a bit position */
            *xw |= 1UL << rt;
        else if (fn == 0x20 || fn == 0x24)          /* seb seh wsbh */
            *xw |= 1UL << rd;
        else
            *xw |= 1UL << rt | 1UL << rd;
        break;
    default:                                        /* I-type ALU, loads,
                                                     * sc, and the rest */
        *xw |= 1UL << rt;
        if (op == 0x11 || op == 0x12)
            *xw |= 1UL << rd;
        break;
    }
}

static void mips_scan_writes(const struct mips_fn *F, int from, int to,
                             unsigned long *xw, int *hilo)
{
    for (int at = from; at + 4 <= to; at += 4) {
        int data = 0;
        for (int k = 0; k < F->nfix && !data; k++)
            data = F->fix[k].kind == FX_TAB && F->fix[k].at == at;
        if (!data)
            mips_writes(mips_rdw(F->t, at), xw, hilo);
    }
    *xw &= ~1UL;
}

/* After an attempt: what must the handler save? 1 when that is more than
 * this attempt saved, and gen_func goes again. */
static int mips_isr_grow(struct mips_fn *F)
{
    const struct ir_func *fn = F->fn;
    unsigned long xw = 0, x = 0;
    int hilo = 0, calls = 0;
    mips_scan_writes(F, fn->src->code_off, F->t->len, &xw, &hilo);
    /* A call: in the IR, to a helper, or in an asm template (it writes
     * ra). An asm is not a call by itself here, as it is for the leaf
     * test: what its template writes is in xw. */
    for (int i = 0; i < fn->nins; i++)
        if (fn->ins[i].op == IR_CALL || mips_op_calls_helper(&fn->ins[i]))
            calls = 1;
    calls |= (int)(xw >> MIPS_RA & 1);
    for (int r = 0; r < 32; r++)
        if (mips_isr_cand(r) && (calls || (xw >> r & 1)))
            x |= 1UL << r;
    if (calls) {
        x |= 1UL << MIPS_GP;
        hilo = 1;
    }
    if (!(x & ~F->isr_x) && (!hilo || F->isr_hilo))
        return 0;
    F->isr_x |= x;
    F->isr_hilo |= hilo;
    return 1;
}

/* The registers' saves or restores, below EPC and Status at the top of
 * the area at `base`: GPRs from the top down in register order. */
static void mips_isr_gprs(struct mips_fn *F, long base, int store)
{
    long off = base + F->isr_bytes - 8;
    for (int r = 1; r < 32; r++)
        if (F->isr_x >> r & 1) {
            off -= 4;
            if (store)
                mips_store(F->t, r, MIPS_SP, (int)off, 4);
            else
                mips_load(F->t, r, MIPS_SP, (int)off, 4, 1);
        }
}

/* HI and LO, at the bottom of the area, through k0. */
static void mips_isr_hilo(struct mips_fn *F, long base, int store)
{
    struct code *t = F->t;
    if (!F->isr_hilo)
        return;
    if (store) {
        mips_mfhi(t, MIPS_K0);
        mips_store(t, MIPS_K0, MIPS_SP, (int)base + 4, 4);
        mips_mflo(t, MIPS_K0);
        mips_store(t, MIPS_K0, MIPS_SP, (int)base, 4);
    } else {
        mips_load(t, MIPS_K0, MIPS_SP, (int)base + 4, 4, 1);
        mips_mthi(t, MIPS_K0);
        mips_load(t, MIPS_K0, MIPS_SP, (int)base, 4, 1);
        mips_mtlo(t, MIPS_K0);
    }
}

/* sp += d, through t0 past addiu's reach: only after the saves or
 * before the restores, so t0 is the handler's own. */
static void mips_isr_sp(struct mips_fn *F, long d)
{
    if (!d)
        return;
    if (fits16(d)) {
        mips_alu_imm(F->t, MIPS_ADDIU, MIPS_SP, MIPS_SP, d);
        return;
    }
    mips_li(F->t, A_LO, d);
    mips_alu(F->t, MIPS_ADDU, MIPS_SP, MIPS_SP, A_LO);
}

/* (the lowest of the area's words: LO, when HI and LO are saved) */
static long mips_isr_base(const struct mips_fn *F)
{
    return fits16(F->frame + 4) ? F->isr_at : 0;
}

static void mips_isr_prologue(struct mips_fn *F)
{
    struct code *t = F->t;
    int kind = ISR_KIND(F->isr), masked = (F->isr & ISR_MASKED) != 0;
    int eic = kind == ISR_INTERRUPT && !masked;
    long base = mips_isr_base(F), top = base + F->isr_bytes;
    if (eic)
        mips_mfc0(t, MIPS_K0, 13, 0);                 /* Cause */
    mips_mfc0(t, MIPS_K1, 14, 0);                     /* EPC */
    mips_isr_sp(F, base ? -F->frame : -F->isr_bytes);
    mips_store(t, MIPS_K1, MIPS_SP, (int)top - 4, 4);
    mips_mfc0(t, MIPS_K1, 12, 0);                     /* Status */
    if (eic)
        mips_ext(t, MIPS_K0, MIPS_K0, 10, 6);         /* RIPL */
    mips_store(t, MIPS_K1, MIPS_SP, (int)top - 8, 4);
    if (eic)
        mips_ins(t, MIPS_K1, MIPS_K0, 10, 6);         /* IPL = RIPL */
    else if (!masked)
        mips_ins(t, MIPS_K1, MIPS_ZERO, 8,
                 kind - ISR_MIPS_VECTOR + 1);         /* IM0..IMn */
    if (masked)
        mips_ins(t, MIPS_K1, MIPS_ZERO, 0, 5);        /* IE EXL ERL KSU */
    else
        mips_ins(t, MIPS_K1, MIPS_ZERO, 1, 4);        /* EXL ERL KSU */
    mips_isr_hilo(F, base, 1);           /* through k0: before the mtc0 */
    mips_mtc0(t, MIPS_K1, 12, 0);
    mips_isr_gprs(F, base, 1);
    if (!base)
        mips_isr_sp(F, -(F->frame - F->isr_bytes));
}

static void mips_isr_epilogue(struct mips_fn *F)
{
    struct code *t = F->t;
    int masked = (F->isr & ISR_MASKED) != 0;
    long base = mips_isr_base(F), top = base + F->isr_bytes;
    for (int k = 0; k < F->nsave; k++)
        ld_sp(F, F->used_callee[k], F->save_at + (long)k * W, W, 1);
    if (!base)
        mips_isr_sp(F, F->frame - F->isr_bytes);
    mips_isr_gprs(F, base, 0);
    if (!masked) {
        mips_di(t, MIPS_ZERO);
        mips_ehb(t);
    }
    mips_isr_hilo(F, base, 0);           /* through k0: after the di */
    mips_load(t, MIPS_K1, MIPS_SP, (int)top - 4, 4, 1);
    mips_mtc0(t, MIPS_K1, 14, 0);
    mips_load(t, MIPS_K1, MIPS_SP, (int)top - 8, 4, 1);
    mips_isr_sp(F, base ? F->frame : F->isr_bytes);
    mips_mtc0(t, MIPS_K1, 12, 0);
    mips_eret(t);
    F->barrier = t->len;
}

/* The last, partial word of a composite in a register: its bytes, packed
 * from the lowest address up, as clang packs them -- the first byte the
 * word's least significant little-endian, its MOST significant
 * big-endian (o32 left-justifies a short composite there). */
static void pack_tail(struct mips_fn *F, int r, int base, long off, long left)
{
    mips_mv(F->t, r, MIPS_ZERO);
    if (g_be) {
        for (long b = off; b < off + left; b++) {
            mips_shift_imm(F->t, sh_w(MIPS_SLL), r, r, 8);
            mips_load(F->t, SCR, base, (int)b, 1, 0);
            mips_alu(F->t, MIPS_OR, r, r, SCR);
        }
        mips_shift_imm(F->t, sh_w(MIPS_SLL), r, r, (int)(8 * (W - left)));
        return;
    }
    for (long b = off + left - 1; b >= off; b--) {
        mips_shift_imm(F->t, sh_w(MIPS_SLL), r, r, 8);
        mips_load(F->t, SCR, base, (int)b, 1, 0);
        mips_alu(F->t, MIPS_OR, r, r, SCR);
    }
}

/* `left` (1..W-1) bytes of a register to the frame at off, the first
 * bytes in memory order -- its low end little-endian, its high end
 * big-endian, where o32 and n64 left-justify a composite's last bytes.
 * r may be clobbered; SCR2 is used. */
static void store_tail(struct mips_fn *F, int r, long off, long left);

static void gen_call(struct mips_fn *F, int n)
{
    struct ir_func *fn = F->fn;
    struct ir_ins *i = &fn->ins[n];
    struct code *t = F->t;
    struct argplace pl[MAX_PARAMS];
    long blk = 0;
    int sret = call_sret(i);
    int roff[4], rsz[4], rfields = 0;
    int rw = i->retsize ? ret_pieces(i->rety, i->retsize, roff, rsz, &rfields)
                        : 0;

    if (rw < 0)
        mips_refuse(F, i, "a call returning a _Complex of integers");
    if (sret)
        blk = W;                          /* a0 holds the result's address */
    for (int k = 0; k < i->nargs; k++)
        place_arg(i->argv[k].size, call_arg_align(i, k), &blk, &pl[k]);

    /* The STACK words first: storing one needs a scratch, and once the
     * argument registers are loaded none is left that is not an argument.
     * A composite's words are read with unaligned-safe loads -- its
     * address may be a packed struct's member. */
    for (int k = 0; k < i->nargs; k++) {
        struct ir_arg *a = &i->argv[k];
        if (!pl[k].nstk)
            continue;
        if (a->is_struct) {
            rd(F, a->vreg, ADDR);
            for (int q = 0; q < pl[k].nstk; q++) {
                long off = (long)(pl[k].nreg + q) * W;
                long left = a->size - off;
                if (left >= W) {
                    ld_any(F, SCR, ADDR, (int)off, W, 0, 0);
                    st_out(F, SCR, pl[k].stk + (long)q * W, W);
                } else {
                    for (long b = 0; b < left; b++) {
                        mips_load(t, SCR, ADDR, (int)(off + b), 1, 0);
                        st_out(F, SCR, pl[k].stk + (long)q * W + b, 1);
                    }
                }
            }
        } else if (a->size > W) {
            /* a two-register scalar: its words in memory order, the high
             * one first big-endian; at o32 it is 8-aligned and never
             * split, at n64 an __int128 may start in a7 */
            rd64(F, a->vreg, SCR, SCR2);
            for (int h = 0; h < 2; h++) {         /* low half, then high */
                int q = g_be ? 1 - h : h;         /* its word in memory */
                if (q >= pl[k].nreg)
                    st_out(F, h ? SCR2 : SCR,
                           pl[k].stk + (long)W * (q - pl[k].nreg), W);
            }
        } else {
            int r;
            rd(F, a->vreg, SCR);
            r = a->size <= 4 ? sext32(F, a->vreg, SCR, SCR) : SCR;
            st_out(F, r, pl[k].stk, stk_scalar_size(a));
        }
    }
    /* The scalar register arguments, all at once: a value for a0 may be
     * in the register a2 is about to get. A two-register one in a pair is
     * two edges of the same move. Before the struct words below, which
     * also write argument registers; the allocator keeps every struct
     * argument's address in memory, so they cannot be a source here. */
    {
        int sd_[2 * MAX_PARAMS], sv_[2 * MAX_PARAMS], sh_[2 * MAX_PARAMS];
        int sz_[2 * MAX_PARAMS];
        int ns_ = 0;
        for (int k = 0; k < i->nargs; k++) {
            struct ir_arg *a = &i->argv[k];
            if (!pl[k].nreg || a->is_struct)
                continue;
            for (int q = 0; q < pl[k].nreg; q++) {
                sd_[ns_] = argreg(pl[k].reg + q);
                sv_[ns_] = a->vreg;
                /* word q: the high one first when big-endian */
                sh_[ns_] = a->size > W ? (g_be ? 1 - q : q) : 0;
                sz_[ns_] = a->size;
                ns_++;
            }
        }
        if (ns_) {
            set_args_half(F, sd_, sv_, sh_, ns_);
            /* n64: a 32-bit argument arrives sign-extended */
            sext_regs(F, sd_, sv_, sz_, ns_);
        }
    }
    for (int k = 0; k < i->nargs; k++) {
        struct ir_arg *a = &i->argv[k];
        if (!pl[k].nreg || !a->is_struct)
            continue;
        rd(F, a->vreg, ADDR);
        for (int q = 0; q < pl[k].nreg; q++) {
            int r = argreg(pl[k].reg + q);
            long off = (long)q * W;
            long left = a->size - off;
            if (left >= W)
                ld_any(F, r, ADDR, (int)off, W, 0, 0);
            else
                pack_tail(F, r, ADDR, off, left);
        }
    }
    /* The hidden result pointer last, so nothing above used a0 after it. */
    if (sret)
        addr_sp(F, MIPS_A0, F->scratch_at + i->scratch);

    if (F->tail && F->tail[n]) {
        mips_restore(F, 0);
        call_sym(F, i->callee, 1);
        if (n + 1 < fn->nins)
            F->skip_next = 1;         /* the IR_RET: not reached */
        return;
    }
    if (i->indirect) {
        /* t9, as an abicalls callee expects to find its own address; the
         * target is in memory (regalloc keeps it there), so reading it
         * now disturbs no argument */
        long slot;
        rd(F, i->a, CALLREG);
        slot = take_slot(F, 1UL << CALLREG, 1);
        mips_jalr(t, MIPS_RA, CALLREG);
        put_slot(F, slot);
    } else {
        call_sym(F, i->callee, 0);
    }

    if (i->dst < 0)
        return;
    if (i->retsize) {
        /* dst receives the scratch's ADDRESS, the contract irgen shares
         * with every backend. A composite that came back in registers is
         * stored there first -- each piece at its offset, a field at its
         * own size, a partial doubleword's bytes in memory order; one the
         * callee wrote through a0 is there already. */
        long at = F->scratch_at + i->scratch;
        for (int q = 0; q < rw; q++) {
            int r = rfields == 2 && q ? MIPS_A0 : ret_word_reg(q);
            if (rsz[q] == W || rfields == 3 || (rfields && !g_be))
                st_sp(F, r, at + roff[q], rsz[q]);
            else                    /* the register's first bytes */
                store_tail(F, r, at + roff[q], rsz[q]);
        }
        addr_sp(F, ACC, at);
        wr(F, i->dst, ACC);
    } else if (F->wide[i->dst] && g_m64 && i->flt && i->ret_tybytes == 16) {
        wr64(F, i->dst, TF_RET_LO, TF_RET_HI);     /* binary128: v0, a0 */
    } else if (F->wide[i->dst]) {
        /* a two-register result in v0:v1 as the ABI orders it; a narrower
         * one read into a wide value is v0, its low word, in either order */
        if (g_be && i->ret_tybytes > W)
            wr64(F, i->dst, MIPS_V1, MIPS_V0);
        else
            wr64(F, i->dst, MIPS_V0, MIPS_V1);
    } else {
        wr(F, i->dst, MIPS_V0);
    }
}

/* ---- one instruction ----------------------------------------------------- */

/* The conversion helpers' names, libgcc's. */
static const char *cvt_name(const struct ir_ins *i)
{
    int src_w = i->size, dst_w = i->w;
    if (g_m64 && (src_w == 16 || dst_w == 16)) {
        /* MIPS64's binary128 and __int128: the tf and ti helpers */
        if (i->op == IR_I2F)
            return src_w == 16
                 ? (dst_w == 16 ? (i->sign ? "__floattitf" : "__floatuntitf")
                    : dst_w == 8 ? (i->sign ? "__floattidf" : "__floatuntidf")
                                 : (i->sign ? "__floattisf" : "__floatuntisf"))
                 : src_w == 8 ? (i->sign ? "__floatditf" : "__floatunditf")
                              : (i->sign ? "__floatsitf" : "__floatunsitf");
        if (i->op == IR_F2I)
            return src_w == 16
                 ? (dst_w == 16 ? (i->sign ? "__fixtfti" : "__fixunstfti")
                    : dst_w == 8 ? (i->sign ? "__fixtfdi" : "__fixunstfdi")
                                 : (i->sign ? "__fixtfsi" : "__fixunstfsi"))
                 : src_w == 8 ? (i->sign ? "__fixdfti" : "__fixunsdfti")
                              : (i->sign ? "__fixsfti" : "__fixunssfti");
        if (dst_w == 16)
            return src_w == 8 ? "__extenddftf2" : "__extendsftf2";
        return dst_w == 8 ? "__trunctfdf2" : "__trunctfsf2";
    }
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

/* An ll/sc loop's atomic word access: every one is bracketed by `sync`,
 * which is what a seq_cst read-modify-write needs on a core that reorders
 * (irgen treats every atomic builtin as seq_cst). */
static void need_word_atomic(struct mips_fn *F, const struct ir_ins *i)
{
    if (g_m64 && i->size == 8)       /* lld/scd */
        return;
    if (i->size == 1 || i->size == 2)   /* sub_* below: the word around it */
        return;
    if (i->size != 4)
        mips_refuse(F, i, "an atomic wider than a register");
}

/* ---- one- and two-byte atomics ---------------------------------------
 *
 * ll/sc are word-sized, so a narrow atomic works on the aligned word
 * around it, as GCC's and LLVM's do: an ll/sc loop that rewrites only its
 * lane,
 *
 *   retry: ll    old, 0(aligned)
 *          new = f(old)  in the lane
 *          new = old ^ ((new ^ old) & mask)
 *          sc    new, 0(aligned)
 *          beq   new, $0, retry
 *
 * atomic against the neighbouring bytes too: a store to any of them
 * between the ll and the sc fails the sc. The lane of address a is bits
 * 8*(a & 3) up little-endian and 8*((a & 3) ^ (4 - size)) up big-endian.
 * Every operation is a 32-bit one on values ll and rd32 sign-extend, so
 * MIPS64 keeps them sign-extended, as its 32-bit forms require. */
#define SUB_SH  SCR
#define SUB_AL  SCR2
#define SUB_MK  B_HI
#define SUB_OLD ACC

static void sub_lane(struct mips_fn *F, int addr, int size)
{
    struct code *t = F->t;
    mips_alu_imm(t, MIPS_ANDI, SUB_SH, addr, 3);
    mips_alu(t, P_SUBU, SUB_AL, addr, SUB_SH);      /* aligned */
    if (target_big_endian())
        mips_alu_imm(t, MIPS_XORI, SUB_SH, SUB_SH, 4 - size);
    mips_shift_imm(t, MIPS_SLL, SUB_SH, SUB_SH, 3);
    mips_alu_imm(t, MIPS_ORI, SUB_MK, MIPS_ZERO, size == 1 ? 0xff : 0xffff);
    mips_alu(t, MIPS_SLLV, SUB_MK, SUB_MK, SUB_SH);
}

/* reg = (src << shift) & mask */
static void sub_in(struct mips_fn *F, int reg, int src)
{
    mips_alu(F->t, MIPS_SLLV, reg, src, SUB_SH);
    mips_alu(F->t, MIPS_AND, reg, reg, SUB_MK);
}

/* SUB_OLD = its lane at bit 0, extended as `sign` says */
static void sub_out(struct mips_fn *F, int size, int sign)
{
    struct code *t = F->t;
    mips_alu(t, MIPS_AND, SUB_OLD, SUB_OLD, SUB_MK);
    mips_alu(t, MIPS_SRLV, SUB_OLD, SUB_OLD, SUB_SH);
    if (sign) {
        mips_shift_imm(t, MIPS_SLL, SUB_OLD, SUB_OLD, 32 - 8 * size);
        mips_shift_imm(t, MIPS_SRA, SUB_OLD, SUB_OLD, 32 - 8 * size);
    }
}

/* FAR = old ^ ((new ^ old) & mask), sc'd, retried from `top` */
static void sub_commit(struct mips_fn *F, int new_reg, int top)
{
    struct code *t = F->t;
    mips_alu(t, MIPS_XOR, FAR, new_reg, SUB_OLD);
    mips_alu(t, MIPS_AND, FAR, FAR, SUB_MK);
    mips_alu(t, MIPS_XOR, FAR, FAR, SUB_OLD);
    mips_sc(t, FAR, SUB_AL, 0);
    int again = br_place(F, MIPS_BEQ, FAR, MIPS_ZERO);
    br_back(F, again, top);
}

static void gen_ins(struct mips_fn *F, int n)
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
        if (i->w != 4 && i->w != 8 && !(g_m64 && i->w == 16))
            mips_refuse(F, i, "a floating-point value of this width");
        if (name) {
            if (i->imm_b)
                mips_refuse(F, i, "a folded floating-point immediate");
            fp_args2(F, i);
            call_helper(F, name);
            fp_result(F, i->dst, i->w);
            return;
        }
        if (i->op == IR_NEG) {
            /* the sign bit, flipped: right for -0.0 and a NaN as well */
            if (i->w == 2 * W) {
                rd64(F, i->a, A_LO, A_HI);
                if (g_m64)                       /* bit 63 of the high half */
                    mips_li64(t, B_LO, (long long)(1ULL << 63));
                else
                    mips_lui(t, B_LO, 0x8000);
                mips_alu(t, MIPS_XOR, A_HI, A_HI, B_LO);
                wr64(F, i->dst, A_LO, A_HI);
            } else if (i->w == 8) {              /* MIPS64: a double */
                rd(F, i->a, ACC);
                mips_li64(t, TMP, (long long)(1ULL << 63));
                mips_alu(t, MIPS_XOR, ACC, ACC, TMP);
                wr(F, i->dst, ACC);
            } else {
                rd(F, i->a, ACC);
                mips_lui(t, TMP, 0x8000);
                mips_alu(t, MIPS_XOR, ACC, ACC, TMP);
                wr(F, i->dst, ACC);
            }
            return;
        }
        if (i->op == IR_CMP) {
            /* __ltdf2 and the rest answer with an int whose relation to
             * zero is the predicate's; unordered makes it false */
            fp_args2(F, i);
            call_helper(F, fp_cmp_name(i->pred, i->w));
            cmp_to_reg(F, i->pred, 1, MIPS_V0, MIPS_ZERO, ACC);
            wr(F, i->dst, ACC);
            return;
        }
        if (i->op == IR_SQRT)
            mips_refuse(F, i, "__builtin_sqrt (a libm routine here, not an "
                              "instruction)");
        mips_refuse(F, i, "this floating-point operation");
    }

    if (i->w > 2 * W)
        mips_refuse(F, i, g_m64 ? "a value wider than 128 bits"
                                : "a 128-bit value");
    /* (before the 64-bit dispatch, so a long long one is named as what
     * it is) */
    if (i->op == IR_XCHG || i->op == IR_XADD || i->op == IR_ARMW ||
        i->op == IR_CAS || i->op == IR_CMPXCHG)
        need_word_atomic(F, i);
    /* o32 and n64 code keep no frame-pointer chain, so only level 0
     * (irgen): the frame address is the stack pointer at entry (frame
     * base + frame), and the return address is ra as the function was
     * entered with it, from its slot -- a function that asks saves it,
     * as one that calls does. An interrupt handler was not called: it
     * returns to EPC, and ra is the interrupted code's. */
    if (i->op == IR_FRAMEADDR) {
        int d = i->dst >= 0 ? wreg(F, i->dst, ACC) : ACC;
        if (i->imm == 2 && F->isr)
            mips_refuse(F, i, "__builtin_return_address in an interrupt "
                              "handler (it was not called; EPC holds where "
                              "it returns)");
        if (i->imm == 2)
            ld_sp(F, d, F->ra_slot, W, 1);
        else
            addr_sp(F, d, F->frame);
        if (i->dst >= 0)
            wrote(F, i->dst, d);
        return;
    }
    if (i->op == IR_CAS16)
        mips_refuse(F, i, "a 16-byte atomic (MIPS64's lld/scd are a "
                          "doubleword; there is no 128-bit ll/sc)");

    /* The high word of a 64-bit value, shifted: one register. */
    if (i->op == IR_SHR && F->nshr && i->dst >= 0 && F->nshr[i->dst]) {
        /* (MIPS32 only: no narrow-shift map at MIPS64) */
        int k = (int)i->imm - 32, d = wreg(F, i->dst, A_LO), hi;
        if (in_reg(F, i->a)) {
            hi = PHI(F->loc[i->a]);
        } else {
            ld_sp(F, A_HI, sslot(F, i->a) + WHI, 4, 1);
            hi = A_HI;
        }
        if (k)
            mips_shift_imm(t, i->sign ? MIPS_SRA : MIPS_SRL, d, hi, k);
        else if (d != hi)
            mips_mv(t, d, hi);
        wrote(F, i->dst, d);
        return;
    }
    {
        /* Does this instruction work on a value that needs a register
         * pair? Not `w == 8` everywhere: STVAR and STORE carry a size and
         * no w, LDVAR and LOAD say it in the map, and a copy that says
         * four bytes copies four (riscv/codegen.c, the same rule). */
        int pw = 2 * W;
        int wide = i->w == pw;
        switch (i->op) {
        case IR_STVAR: wide = i->size == pw || (i->a >= 0 && F->wide[i->a]);
                       break;
        case IR_STORE: wide = i->size == pw || (i->b >= 0 && F->wide[i->b]);
                       break;
        case IR_LDVAR:
        case IR_LOAD:  wide = i->dst >= 0 && F->wide[i->dst]; break;
        case IR_MOV:
        case IR_SELECT:
            wide = (g_m64 ? i->w == 0 || i->w > 8 : i->w != 4) &&
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
                 * names, the operands in a0:a1 and a2:a3 */
                if (i->imm_b) {
                    args64x2(F, i->a, -1);
                    operand_b64(F, i, PLO(MIPS_A2), PHI(MIPS_A2));
                } else {
                    args64x2(F, i->a, i->b);
                }
                call_helper(F, g_m64
                            ? (i->op == IR_DIV
                               ? (i->sign ? "__divti3" : "__udivti3")
                               : (i->sign ? "__modti3" : "__umodti3"))
                            : i->op == IR_DIV
                            ? (i->sign ? "__divdi3" : "__udivdi3")
                            : (i->sign ? "__moddi3" : "__umoddi3"));
                wr64(F, i->dst, PLO(MIPS_V0), PHI(MIPS_V0));
                return;
            }
            if (i->op == IR_BSWAP && g_m64)
                mips_refuse(F, i, "a 128-bit byte swap");
            if (i->op == IR_BSWAP) {
                /* each word reversed (wsbh + rotr), and the words swapped */
                int al, ah;
                src64(F, i->a, A_LO, A_HI, &al, &ah);
                mips_wsbh(t, B_LO, ah);
                mips_shift_imm(t, MIPS_ROTR, B_LO, B_LO, 16);
                mips_wsbh(t, B_HI, al);
                mips_shift_imm(t, MIPS_ROTR, B_HI, B_HI, 16);
                wr64(F, i->dst, B_LO, B_HI);
                return;
            }
            if (gen_ins64(F, n))
                return;
            mips_refuse(F, i, g_m64 ? "this operation at 128 bits"
                                    : "this operation at 64 bits");
        }
        if (wide && (i->op == IR_CMP || i->op == IR_BRZ || i->op == IR_BRNZ))
            i->w = pw;        /* the cases below read `w` to pick the pair */
    }

    switch (i->op) {
    case IR_LABEL:
        F->label_off[i->label] = t->len;
        F->barrier = t->len;
        return;
    case IR_JMP:
        if (n + 1 < fn->nins && fn->ins[n + 1].op == IR_LABEL &&
            fn->ins[n + 1].label == i->label)
            return;
        jump_to(F, i->label);
        return;
    case IR_CONST: {
        int d = wreg(F, i->dst, ACC);
        li(t, d, imm_val(i));
        wrote(F, i->dst, d);
        return;
    }
    case IR_BITCAST:
    case IR_MOV: {
        int src = rdr(F, i->a, ACC);
        int d = wreg(F, i->dst, ACC);
        if (src != d)
            mips_mv(t, d, src);
        wrote(F, i->dst, d);
        return;
    }

    case IR_ADD: case IR_SUB: case IR_MUL:
    case IR_AND: case IR_OR:  case IR_XOR: {
        /* (IR_DIV and IR_MOD sit between IR_MUL and IR_AND in the enum, so
         * this is a switch and not a table.) */
        /* MIPS64: a doubleword operation (d64) is daddu/dsubu/dmult; a
         * 32-bit one reads its operands sign-extended (rdw) and the
         * 32-bit instruction leaves its result so */
        int d64 = g_m64 && i->w > 4;
        int op = i->op == IR_ADD ? (d64 ? MIPS_DADDU : MIPS_ADDU)
               : i->op == IR_SUB ? (d64 ? MIPS_DSUBU : MIPS_SUBU)
               : i->op == IR_AND ? MIPS_AND
               : i->op == IR_OR  ? MIPS_OR
               : i->op == IR_XOR ? MIPS_XOR
               : MIPS_MUL;
        int ra_ = rdw(F, i, i->a, ACC);
        int rd_ = wreg(F, i->dst, ACC);
        if (i->imm_b && i->op != IR_MUL) {
            long long v = imm_val(i);
            if (i->op == IR_ADD || i->op == IR_SUB) {
                /* addiu's field is SIGNED; a subtraction adds -v, and
                 * -(-32768) does not fit, which is why it is checked here */
                if (i->op == IR_SUB) v = -v;
                if (v >= -32768 && v <= 32767) {
                    mips_alu_imm(t, d64 ? MIPS_DADDIU : MIPS_ADDIU, rd_, ra_,
                                 v);
                    wrote(F, i->dst, rd_);
                    return;
                }
            } else if (d64) {
                logic_dw(F, op, rd_, ra_, (unsigned long long)v);
                wrote(F, i->dst, rd_);
                return;
            } else {
                /* andi/ori/xori ZERO-extend: a 16-bit unsigned field,
                 * and the masks ext and ins make in one instruction */
                logic_half(F, op, rd_, ra_, (unsigned long)v);
                wrote(F, i->dst, rd_);
                return;
            }
        }
        {
            /* the second operand may not land in the destination before
             * the first is read: a scratch unless it has a home */
            int rb_ = (i->imm_b || !in_reg(F, i->b)) ? TMP : reg_of(F, i->b);
            if (rb_ == TMP) operand_b(F, i, TMP);
            if (g_m64 && !d64 && !i->imm_b)
                rb_ = sext32(F, i->b, rb_, TMP);
            if (d64 && op == MIPS_MUL) {
                /* MIPS64r2 has no dmul: dmult, and LO */
                mips_muldiv(t, MIPS_DMULT, ra_, rb_);
                mips_mflo(t, rd_);
            } else {
                mips_alu(t, op, rd_, ra_, rb_);
            }
        }
        wrote(F, i->dst, rd_);
        return;
    }
    case IR_MULH: {
        /* The high word of a 32 x 32 product, from HI (MIPS32 only) */
        int ra_ = rdr(F, i->a, ACC), rb_ = rdr(F, i->b, TMP), d;
        if (g_m64)
            mips_refuse(F, i, "a 32-bit high multiply at MIPS64");
        mips_muldiv(t, i->sign ? MIPS_MULT : MIPS_MULTU, ra_, rb_);
        d = wreg(F, i->dst, ACC);
        mips_mfhi(t, d);
        wrote(F, i->dst, d);
        return;
    }
    case IR_DIV: case IR_MOD: {
        int d64 = g_m64 && i->w > 4;
        int ra_ = rdw(F, i, i->a, ACC);
        int rb_ = (i->imm_b || !in_reg(F, i->b)) ? TMP : reg_of(F, i->b);
        int d;
        if (rb_ == TMP) operand_b(F, i, TMP);
        if (g_m64 && !d64 && !i->imm_b)
            rb_ = sext32(F, i->b, rb_, TMP);
        mips_muldiv(t, d64 ? (i->sign ? MIPS_DDIV : MIPS_DDIVU)
                           : (i->sign ? MIPS_DIV : MIPS_DIVU), ra_, rb_);
        d = wreg(F, i->dst, ACC);
        if (i->op == IR_DIV) mips_mflo(t, d);
        else                 mips_mfhi(t, d);
        wrote(F, i->dst, d);
        return;
    }
    case IR_SHL: case IR_SHR: {
        /* at MIPS64 a 32-bit right shift reads its value sign-extended
         * (srl and sra are UNPREDICTABLE otherwise); sll reads only the
         * low word, which is why `sll r, r, 0` is the sign extension */
        int d64 = g_m64 && i->w > 4, bits = d64 ? 64 : 32;
        int ra_ = i->op == IR_SHR ? rdw(F, i, i->a, ACC) : rdr(F, i->a, ACC);
        int d;
        if (i->imm_b && i->imm >= 0 && i->imm < bits) {
            int op = i->op == IR_SHL ? MIPS_SLL : i->sign ? MIPS_SRA : MIPS_SRL;
            d = wreg(F, i->dst, ACC);
            mips_shift_imm(t, d64 ? sh_w(op) : op, d, ra_, (int)i->imm);
        } else {
            int op = i->op == IR_SHL ? MIPS_SLLV
                   : i->sign ? MIPS_SRAV : MIPS_SRLV;
            int rb_ = (i->imm_b || !in_reg(F, i->b)) ? TMP : reg_of(F, i->b);
            if (rb_ == TMP) operand_b(F, i, TMP);
            d = wreg(F, i->dst, ACC);
            mips_alu(t, d64 ? shv_w(op) : op, d, ra_, rb_);
        }
        wrote(F, i->dst, d);
        return;
    }
    case IR_NEG: {
        int ra_ = rdw(F, i, i->a, ACC);
        int d = wreg(F, i->dst, ACC);
        mips_alu(t, g_m64 && i->w > 4 ? MIPS_DSUBU : MIPS_SUBU, d, MIPS_ZERO,
                 ra_);
        wrote(F, i->dst, d);
        return;
    }
    case IR_BNOT: {
        int ra_ = rdr(F, i->a, ACC);
        int d = wreg(F, i->dst, ACC);
        mips_alu(t, MIPS_NOR, d, ra_, MIPS_ZERO);
        wrote(F, i->dst, d);
        return;
    }

    case IR_CMP: {
        struct ir_ins *nx = n + 1 < fn->nins ? &fn->ins[n + 1]
                                             : (struct ir_ins *)0;
        int fuse = nx && (nx->op == IR_BRZ || nx->op == IR_BRNZ) &&
                   nx->a == i->dst && F->usecnt && F->usecnt[i->dst] == 1;
        if (i->w == 2 * W) {
            if (fuse && i->imm_b && i->imm == 0 &&
                (i->pred == B_EQ || i->pred == B_NE ||
                 (i->sign && (i->pred == B_LT || i->pred == B_GE)))) {
                /* against zero: an or of the halves and a beq/bne, or the
                 * high word's sign and a bltz/bgez */
                int al, ah, cond, r;
                src64(F, i->a, A_LO, A_HI, &al, &ah);
                if (i->pred == B_EQ || i->pred == B_NE) {
                    mips_alu(t, MIPS_OR, SCR, al, ah);
                    r = SCR;
                    cond = i->pred == B_EQ ? C_EQ : C_NE;
                } else {
                    r = ah;
                    cond = i->pred == B_LT ? C_LT : C_GE;
                }
                if (nx->op == IR_BRZ)
                    cond = invert_cond(cond);
                branch_if(F, cond, r, MIPS_ZERO, nx->label);
                F->skip_next = 1;
                return;
            }
            cmp64(F, i, i->pred, i->sign);
            wr(F, i->dst, ACC);
            return;
        }
        if (fuse && !(nx->w == 2 * W)) {
            /* One branch where the unfused form is a compare, a store and
             * a test. GT and LE swap their operands. An ordered compare
             * with a 16-bit constant is slti/sltiu into $at and a test of
             * it; any other constant is loaded. At MIPS64 a 32-bit compare
             * reads both operands sign-extended. */
            int ra_ = rdw(F, i, i->a, ACC);
            int cond, sw = 0, rb_;
            if (i->imm_b && imm_val(i) != 0 && i->pred != B_EQ &&
                i->pred != B_NE &&
                cmp_imm_to_reg(F, i->pred, i->sign, ra_, imm_val(i), CC,
                               i->w)) {
                branch_if(F, nx->op == IR_BRZ ? C_EQ : C_NE, CC, MIPS_ZERO,
                          nx->label);
                F->skip_next = 1;
                return;
            }
            rb_ = (i->imm_b || !in_reg(F, i->b)) ? TMP : reg_of(F, i->b);
            if (i->imm_b && imm_val(i) == 0)
                rb_ = MIPS_ZERO;
            if (rb_ == TMP) operand_b(F, i, TMP);
            if (g_m64 && !i->imm_b && i->w >= 1 && i->w <= 4)
                rb_ = sext32(F, i->b, rb_, TMP);
            switch (i->pred) {
            case B_EQ: cond = C_EQ; break;
            case B_NE: cond = C_NE; break;
            case B_LT: cond = i->sign ? C_LT : C_LTU; break;
            case B_GE: cond = i->sign ? C_GE : C_GEU; break;
            case B_GT: cond = i->sign ? C_LT : C_LTU; sw = 1; break;
            case B_LE: cond = i->sign ? C_GE : C_GEU; sw = 1; break;
            default:   cond = -1; break;
            }
            if (cond >= 0) {
                int x = sw ? rb_ : ra_, y = sw ? ra_ : rb_;
                if (nx->op == IR_BRZ)
                    cond = invert_cond(cond);
                branch_if(F, cond, x, y, nx->label);
                F->skip_next = 1;
                return;
            }
        }
        {
            /* cmp_to_reg reads its operands only in its first instruction,
             * so d may be either of them */
            int ra_ = rdw(F, i, i->a, ACC);
            int rb_ = (i->imm_b || !in_reg(F, i->b)) ? TMP : reg_of(F, i->b);
            int d = wreg(F, i->dst, ACC);
            if (i->imm_b && cmp_imm_to_reg(F, i->pred, i->sign, ra_,
                                           imm_val(i), d, i->w)) {
                wrote(F, i->dst, d);
                return;
            }
            if (rb_ == TMP) operand_b(F, i, TMP);
            if (g_m64 && !i->imm_b && i->w >= 1 && i->w <= 4)
                rb_ = sext32(F, i->b, rb_, TMP);
            cmp_to_reg(F, i->pred, i->sign, ra_, rb_, d);
            wrote(F, i->dst, d);
        }
        return;
    }

    case IR_SELECT: {
        /* dst = a ? b : c, with movn: c, then b over it when the
         * condition -- tested at ITS width, `size` -- is nonzero */
        int cond, rb_, rc_, d;
        if (i->size == 2 * W) {
            int al, ah;
            src64(F, i->a, SCR, SCR2, &al, &ah);
            mips_alu(t, MIPS_OR, SCR, al, ah);
            cond = SCR;
        } else if (g_m64 && i->size >= 1 && i->size <= 4) {
            cond = rd32(F, i->a, SCR);
        } else {
            cond = rdr(F, i->a, SCR);
        }
        rb_ = rdr(F, i->b, TMP);
        rc_ = rdr(F, i->c, ACC);
        d = wreg(F, i->dst, ACC);
        if (d == cond || d == rb_)
            d = ACC;
        if (d != rc_)
            mips_mv(t, d, rc_);
        mips_alu(t, MIPS_MOVN, d, rb_, cond);
        wrote(F, i->dst, d);
        return;
    }

    case IR_BRZ: case IR_BRNZ: {
        int r;
        if (i->w == 2 * W) {
            int al, ah;
            src64(F, i->a, A_LO, A_HI, &al, &ah);
            mips_alu(t, MIPS_OR, SCR, al, ah);
            r = SCR;
        } else {
            r = rdw(F, i, i->a, A_LO);
        }
        branch_if(F, i->op == IR_BRZ ? C_EQ : C_NE, r, MIPS_ZERO, i->label);
        return;
    }

    /* A local may live in a register; these two are the only places that
     * name its slot directly, so they are the two that ask. */
    case IR_LDVAR: {
        int d = wreg(F, i->dst, ACC);
        if (in_reg(F, i->a)) {
            if (mips_ldvar_plain(i->size, i->sign, i->w)) {
                if (d != reg_of(F, i->a)) mips_mv(t, d, reg_of(F, i->a));
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
            /* a narrowing store sign-extends; an unsigned read extends
             * for itself (ldvar_plain is only the full word) */
            if (i->size >= W) {
                if (reg_of(F, i->dst) != src) mips_mv(t, reg_of(F, i->dst), src);
            } else {
                ext_reg(F, reg_of(F, i->dst), src, i->size, 1);
            }
        } else {
            st_sp(F, src, var_slot(F, i->dst, i->size), i->size);
        }
        return;
    }
    case IR_LOAD: {                    /* memoff: ra_fold_memoff's, or 0 */
        int addr = rdr(F, i->a, ADDR);
        int d = wreg(F, i->dst, ACC);
        ld_any(F, d, addr, i->memoff, i->size, i->sign, i->natural);
        wrote(F, i->dst, d);
        return;
    }
    case IR_STORE: {
        int addr = rdr(F, i->a, ADDR);
        int val = rdr(F, i->b, ACC);
        st_any(F, val, addr, i->memoff, i->size, i->natural);
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
    /* An address is lui + addiu, R_MIPS_HI16 then R_MIPS_LO16, against
     * the symbol (or .rodata for a string). Absolute, so the image's
     * link address decides it; o32 has no PC-relative form. */
    /* MIPS64: the four pieces, lui %highest, daddiu %higher, dsll 16,
     * daddiu %hi, dsll 16, daddiu %lo, as clang takes a 64-bit address
     * (abs_pair); the sites at +0, +4, +12 and +20. */
    case IR_STRADDR: {
        int d = wreg(F, i->dst, ACC);
        int at = abs_pair(F, d);
        if (g_m64) {
            note_str(F->st, at, i->label, RK_MIPS_HIGHEST);
            note_str(F->st, at + 4, i->label, RK_MIPS_HIGHER);
            note_str(F->st, at + 12, i->label, RK_MIPS_HI16);
            note_str(F->st, at + 20, i->label, RK_MIPS_LO16);
        } else {
            note_str(F->st, at, i->label, RK_MIPS_HI16);
            note_str(F->st, at + 4, i->label, RK_MIPS_LO16);
        }
        wrote(F, i->dst, d);
        return;
    }
    case IR_GADDR: {
        int d = wreg(F, i->dst, ACC);
        int at = abs_pair(F, d);
        if (g_m64) {
            note_glob(F->st, at, i->glob, RK_MIPS_HIGHEST);
            note_glob(F->st, at + 4, i->glob, RK_MIPS_HIGHER);
            note_glob(F->st, at + 12, i->glob, RK_MIPS_HI16);
            note_glob(F->st, at + 20, i->glob, RK_MIPS_LO16);
        } else {
            note_glob(F->st, at, i->glob, RK_MIPS_HI16);
            note_glob(F->st, at + 4, i->glob, RK_MIPS_LO16);
        }
        wrote(F, i->dst, d);
        return;
    }
    case IR_FADDR: {
        int d = wreg(F, i->dst, ACC);
        int at = abs_pair(F, d);
        if (g_m64) {
            note_fn(F->st, at, i->callee, RK_MIPS_HIGHEST);
            note_fn(F->st, at + 4, i->callee, RK_MIPS_HIGHER);
            note_fn(F->st, at + 12, i->callee, RK_MIPS_HI16);
            note_fn(F->st, at + 20, i->callee, RK_MIPS_LO16);
        } else {
            note_fn(F->st, at, i->callee, RK_MIPS_HI16);
            note_fn(F->st, at + 4, i->callee, RK_MIPS_LO16);
        }
        wrote(F, i->dst, d);
        return;
    }

    case IR_MEMCPY: case IR_MEMZERO:
        rd(F, i->a, ADDR);
        if (i->op == IR_MEMCPY)
            rd(F, i->b, TMP);
        copy_block(F, i->op == IR_MEMCPY, i->size, i->natural >= 4);
        return;

    case IR_CALL:
        gen_call(F, n);
        return;

    case IR_RET:
        if (i->a >= 0) {
            if (fn->ret_abi.is_struct) {
                int roff[4], rsz[4], rfields;
                int rw = ret_pieces(fn->ret_abi.ty, fn->ret_abi.size, roff,
                                    rsz, &rfields);
                if (rw < 0)
                    mips_refuse(F, i, "returning a _Complex of integers");
                if (rw > 0) {
                    /* o32: a complex's words into v0, v1 (a0, a1); n64:
                     * each piece into v0, v1 -- a field at its own size
                     * (a float sign-extended), else the doublewords `ld`
                     * reads, the last one packed */
                    rd(F, i->a, ADDR);
                    for (int q = 0; q < rw; q++) {
                        int r = rfields == 2 && q ? MIPS_A0 : ret_word_reg(q);
                        if (!g_m64) {
                            ld_any(F, r, ADDR, 4 * q, 4, 0, 0);
                        } else if (rfields || rsz[q] == 8) {
                            ld_any(F, r, ADDR, roff[q], rsz[q], 1, 0);
                            /* a float field big-endian: the high half */
                            if (rfields == 1 && rsz[q] == 4 && g_be)
                                mips_shift_imm(t, MIPS_DSLL, r, r, 32);
                        } else {
                            pack_tail(F, r, ADDR, roff[q], rsz[q]);
                        }
                    }
                } else {
                    /* through the caller's buffer, whose address the
                     * prologue kept; the pointer comes back in v0 */
                    rd(F, i->a, TMP);
                    ld_sp(F, ADDR, F->sret_slot, W, 1);
                    copy_block(F, 1, fn->ret_abi.size, 0);
                    ld_sp(F, MIPS_V0, F->sret_slot, W, 1);
                }
            } else if (F->wide[i->a] && g_be && fn->ret_abi.size <= W) {
                /* a 32-bit result of a 64-bit value: its low word, in v0 */
                rd(F, i->a, MIPS_V0);
            } else if (F->wide[i->a] && g_m64 && fn->ret_abi.is_float) {
                rd64(F, i->a, TF_RET_LO, TF_RET_HI);   /* binary128 */
            } else if (F->wide[i->a]) {
                rd64(F, i->a, PLO(MIPS_V0), PHI(MIPS_V0));
            } else if (g_m64 && fn->ret_abi.size <= 4) {
                /* n64: a 32-bit result is returned sign-extended */
                int r = rd32(F, i->a, MIPS_V0);
                if (r != MIPS_V0)
                    mips_mv(t, MIPS_V0, r);
            } else {
                rd(F, i->a, MIPS_V0);
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
        /* `break`: an exception, never a fall-through */
        mips_break(t, 0);
        return;
    case IR_FENCE:
        mips_sync(t, 0);
        return;

    case IR_BSWAP: {
        /* Release 2: wsbh swaps the bytes of each halfword, rotr 16 the
         * halfwords; a 16-bit value needs only the first and a mask */
        int ra_ = g_m64 && i->size == 4 ? rd32(F, i->a, ACC)
                                        : rdr(F, i->a, ACC);
        int d = wreg(F, i->dst, ACC);
        if (g_m64 && i->size == 8) {     /* MIPS64: dsbh then dshd */
            mips_dsbh(t, d, ra_);
            mips_dshd(t, d, d);
            wrote(F, i->dst, d);
            return;
        }
        mips_wsbh(t, d, ra_);
        if (i->size == 2)
            mips_alu_imm(t, MIPS_ANDI, d, d, 0xffff);
        else
            mips_shift_imm(t, MIPS_ROTR, d, d, 16);
        wrote(F, i->dst, d);
        return;
    }

    case IR_VA_START:
        /* va_list is a bare pointer at the first unnamed word: the
         * prologue stored a0-a3 into the home area, so one pointer walks
         * from the register words into the stack ones */
        rd(F, i->a, ADDR);
        addr_sp(F, ACC, F->va_first);
        mips_store(t, ACC, ADDR, 0, W);
        return;

    case IR_I2F: case IR_F2I: case IR_F2F: {
        int src_w = i->size, pw = 2 * W;
        if (i->op == IR_F2F && src_w == i->w) {
            if (src_w == pw) {
                rd64(F, i->a, A_LO, A_HI);
                wr64(F, i->dst, A_LO, A_HI);
            } else {
                rd(F, i->a, ACC);
                wr(F, i->dst, ACC);
            }
            return;
        }
        if (src_w > pw || i->w > pw)
            mips_refuse(F, i, g_m64 ? "a conversion of a value wider than "
                                      "128 bits"
                                    : "a conversion of a 128-bit value");
        if (i->op == IR_I2F && src_w == pw && i->a >= 0 && !F->wide[i->a]) {
            /* a one-register value asked for as two: zero-extended (only
             * an unsigned one is ever widened this way) */
            rd(F, i->a, PLO(MIPS_A0));
            mips_mv(t, PHI(MIPS_A0), MIPS_ZERO);
        } else if (src_w == pw) {
            args64x2(F, i->a, -1);
        } else if (g_m64 && src_w <= 4) {
            /* n64: a 32-bit argument sign-extended (a float's bits too) */
            int r = rd32(F, i->a, MIPS_A0);
            if (r != MIPS_A0)
                mips_mv(t, MIPS_A0, r);
        } else {
            rd(F, i->a, MIPS_A0);
        }
        call_helper(F, cvt_name(i));
        if (i->dst >= 0) {
            if (F->wide[i->dst] && g_be && i->w <= W)
                wr64(F, i->dst, MIPS_V0, MIPS_V1);     /* a 32-bit result */
            else if (F->wide[i->dst] && g_m64 && i->w == 16 &&
                     i->op != IR_F2I)
                wr64(F, i->dst, TF_RET_LO, TF_RET_HI); /* binary128 */
            else if (F->wide[i->dst])
                wr64(F, i->dst, PLO(MIPS_V0), PHI(MIPS_V0));
            else
                wr(F, i->dst, MIPS_V0);
        }
        return;
    }

    case IR_ASM: {
        /* Extended asm, assembled in irgen (mips/irgen.c) against the
         * vocabulary in mips/asm.c. Nothing is live in a register across
         * one -- the allocator excludes every vreg whose range spans an
         * IR_ASM -- so operands' registers may be loaded freely. */
        struct ir_asm *ia = i->asm_ir;
        int used[32] = { 0 };
        static const int scr_pool[] = {
            MIPS_T0, MIPS_T1, MIPS_T2, MIPS_T3, MIPS_T4, MIPS_T5,
            MIPS_V0, MIPS_V1, MIPS_A0, MIPS_A1, MIPS_A2, MIPS_A3
        };
        int scr = -1;
        for (int k = 0; k < ia->nin; k++) used[ia->in[k].reg] = 1;
        for (int k = 0; k < ia->nout; k++) used[ia->out[k].reg] = 1;
        for (unsigned k = 0; k < sizeof scr_pool / sizeof scr_pool[0]; k++)
            if (!used[scr_pool[k]]) { scr = scr_pool[k]; break; }
        if (scr < 0 && ia->nout > 0)
            mips_refuse(F, i, "an asm with no scratch register left around it");
        for (int k = 0; k < ia->nout; k++)
            if (!ia->out[k].mem && ia->out[k].size > W)
                mips_refuse(F, i, "an asm output wider than a register");
        for (int k = 0; k < ia->nout; k++) {
            if (!ia->out[k].inout || ia->out[k].mem)
                continue;
            rd(F, ia->out[k].temp, scr);
            mips_load(t, ia->out[k].reg, scr, 0, ia->out[k].size, 0);
        }
        for (int k = 0; k < ia->nin; k++)
            rd(F, ia->in[k].temp, ia->in[k].reg);
        for (int k = 0; k < ia->nout; k++)
            if (ia->out[k].mem)
                rd(F, ia->out[k].temp, ia->out[k].reg);
        for (int k = 0; k < ia->codelen; k++)
            code_byte(t, ia->code[k]);
        F->barrier = t->len;          /* the template's own: never moved */
        for (int k = 0; k < ia->nout; k++) {
            if (ia->out[k].mem)
                continue;
            rd(F, ia->out[k].temp, scr);
            mips_store(t, ia->out[k].reg, scr, 0, ia->out[k].size);
        }
        return;
    }

    /* ---- atomics: ll/sc loops, bracketed by sync --------------------- */
    case IR_XCHG: case IR_XADD: case IR_ARMW: {
        /* sc stores its rt and then overwrites it with the success flag,
         * so the new value is built in SCR on every trip. */
        int addr, val, dst, top, again, dw;
        need_word_atomic(F, i);
        if (i->size == 1 || i->size == 2) {
            addr = rdr(F, i->a, ADDR);
            val = g_m64 ? rd32(F, i->b, TMP) : rdr(F, i->b, TMP);
            sub_lane(F, addr, i->size);
            sub_in(F, TMP, val);                  /* the operand, in lane */
            int op = i->op == IR_ARMW ? (int)i->imm : 0;
            mips_sync(t, 0);
            top = t->len;
            mips_ll(t, SUB_OLD, SUB_AL, 0);
            if (i->op == IR_XCHG)
                mips_mv(t, ADDR, TMP);
            else if (i->op == IR_XADD)
                mips_alu(t, MIPS_ADDU, ADDR, SUB_OLD, TMP);
            else if (op == '&')
                mips_alu(t, MIPS_AND, ADDR, SUB_OLD, TMP);
            else if (op == '|')
                mips_alu(t, MIPS_OR, ADDR, SUB_OLD, TMP);
            else if (op == '^')
                mips_alu(t, MIPS_XOR, ADDR, SUB_OLD, TMP);
            else {                                  /* nand */
                mips_alu(t, MIPS_AND, ADDR, SUB_OLD, TMP);
                mips_alu(t, MIPS_NOR, ADDR, ADDR, MIPS_ZERO);
            }
            sub_commit(F, ADDR, top);
            mips_sync(t, 0);
            sub_out(F, i->size, i->sign);
            wrote(F, i->dst, SUB_OLD);
            return;
        }
        dw = i->size == 8;            /* MIPS64: lld/scd */
        addr = rdr(F, i->a, ADDR);
        val = g_m64 && !dw ? rd32(F, i->b, TMP) : rdr(F, i->b, TMP);
        dst = wreg(F, i->dst, ACC);
        if (dst == addr || dst == val)
            dst = ACC;
        mips_sync(t, 0);
        top = t->len;
        if (dw) mips_lld(t, dst, addr, 0);
        else    mips_ll(t, dst, addr, 0);
        if (i->op == IR_XCHG) {
            mips_mv(t, SCR, val);
        } else if (i->op == IR_XADD) {
            mips_alu(t, dw ? MIPS_DADDU : MIPS_ADDU, SCR, dst, val);
        } else {
            switch ((int)i->imm) {
            case '&': mips_alu(t, MIPS_AND, SCR, dst, val); break;
            case '|': mips_alu(t, MIPS_OR, SCR, dst, val); break;
            case '^': mips_alu(t, MIPS_XOR, SCR, dst, val); break;
            default:                                        /* nand */
                mips_alu(t, MIPS_AND, SCR, dst, val);
                mips_alu(t, MIPS_NOR, SCR, SCR, MIPS_ZERO);
                break;
            }
        }
        if (dw) mips_scd(t, SCR, addr, 0);
        else    mips_sc(t, SCR, addr, 0);
        again = br_place(F, MIPS_BEQ, SCR, MIPS_ZERO);
        br_back(F, again, top);
        mips_sync(t, 0);
        wrote(F, i->dst, dst);
        return;
    }
    case IR_CAS: case IR_CMPXCHG: {
        /*   sync
         *   retry: ll   seen, (addr)
         *          bne  seen, expected, out
         *          move FAR, desired
         *          sc   FAR, (addr)
         *          beq  FAR, $0, retry
         *   out:   sync
         * IR_CAS yields what was seen; IR_CMPXCHG writes it back through
         * the pointer in b and yields whether it was the expected one. */
        int addr, exp, des, seen = ACC, out_br, top, again;
        int dw;
        need_word_atomic(F, i);
        if (i->size == 1 || i->size == 2) {
            /* the lane only: expected and desired moved into it */
            addr = rdr(F, i->a, ADDR);
            sub_lane(F, addr, i->size);
            if (i->op == IR_CAS) {
                sub_in(F, TMP, g_m64 ? rd32(F, i->b, TMP) : rdr(F, i->b, TMP));
            } else {
                int p = rdr(F, i->b, TMP);
                mips_load(t, TMP, p, 0, i->size, 0);
                sub_in(F, TMP, TMP);
            }
            sub_in(F, ADDR, g_m64 ? rd32(F, i->c, ADDR) : rdr(F, i->c, ADDR));
            mips_sync(t, 0);
            top = t->len;
            mips_ll(t, SUB_OLD, SUB_AL, 0);
            mips_alu(t, MIPS_AND, FAR, SUB_OLD, SUB_MK);
            out_br = br_place(F, MIPS_BNE, FAR, TMP);
            sub_commit(F, ADDR, top);
            br_land(F, out_br);
            mips_sync(t, 0);
            if (i->op == IR_CAS) {
                sub_out(F, i->size, i->sign);
                wr(F, i->dst, SUB_OLD);
            } else {
                /* the flag from the lanes compared; then *b = seen */
                mips_alu(t, MIPS_AND, ADDR, SUB_OLD, SUB_MK);
                mips_alu(t, MIPS_XOR, ADDR, ADDR, TMP);
                mips_alu_imm(t, MIPS_SLTIU, ADDR, ADDR, 1);
                sub_out(F, i->size, 0);
                int p = rdr(F, i->b, TMP);
                mips_store(t, SUB_OLD, p, 0, i->size);
                wr(F, i->dst, ADDR);
            }
            return;
        }
        dw = i->size == 8;            /* MIPS64: lld/scd */
        addr = rdr(F, i->a, ADDR);
        if (i->op == IR_CAS) {
            /* compared with what ll sign-extended */
            exp = g_m64 && !dw ? rd32(F, i->b, TMP) : rdr(F, i->b, TMP);
        } else {
            int p = rdr(F, i->b, TMP);
            mips_load(t, SCR, p, 0, dw ? 8 : 4, 1);
            exp = SCR;
        }
        des = rdr(F, i->c, SCR2);
        mips_sync(t, 0);
        top = t->len;
        if (dw) mips_lld(t, seen, addr, 0);
        else    mips_ll(t, seen, addr, 0);
        out_br = br_place(F, MIPS_BNE, seen, exp);
        mips_mv(t, FAR, des);
        if (dw) mips_scd(t, FAR, addr, 0);
        else    mips_sc(t, FAR, addr, 0);
        again = br_place(F, MIPS_BEQ, FAR, MIPS_ZERO);
        br_back(F, again, top);
        br_land(F, out_br);
        mips_sync(t, 0);
        if (i->op == IR_CAS) {
            wr(F, i->dst, seen);
        } else {
            int p = rdr(F, i->b, TMP);
            mips_store(t, seen, p, 0, dw ? 8 : 4);
            mips_alu(t, MIPS_XOR, FAR, seen, exp);
            mips_alu_imm(t, MIPS_SLTIU, FAR, FAR, 1);
            wr(F, i->dst, FAR);
        }
        return;
    }
    case IR_ALLOCA: {
        /* A fresh 16-aligned block (the IR's promise, which o32's 8-byte
         * stack does not make by itself): sp moves down by the size
         * rounded to 16 and then to a multiple of 16. The block sits
         * above the outgoing area -- a multiple of 16 in such a function
         * -- which moves down with sp; the frame is addressed from fp. */
        int d = wreg(F, i->dst, SCR2);
        rd(F, i->a, SCR);
        mips_alu_imm(t, P_ADDIU, SCR, SCR, 15);
        if (g_m64) mips_dins(t, SCR, MIPS_ZERO, 0, 4);
        else       mips_ins(t, SCR, MIPS_ZERO, 0, 4);
        mips_alu(t, P_SUBU, SCR, MIPS_SP, SCR);
        if (g_m64) mips_dins(t, SCR, MIPS_ZERO, 0, 4);
        else       mips_ins(t, SCR, MIPS_ZERO, 0, 4);
        mips_mv(t, MIPS_SP, SCR);
        if (fits16(F->out_bytes)) {
            mips_alu_imm(t, P_ADDIU, d, MIPS_SP, F->out_bytes);
        } else {
            mips_li(t, d, F->out_bytes);
            mips_alu(t, P_ADDU, d, MIPS_SP, d);
        }
        wrote(F, i->dst, d);
        return;
    }
    case IR_SPSAVE: {
        int d = wreg(F, i->dst, SCR);
        mips_mv(t, d, MIPS_SP);
        wrote(F, i->dst, d);
        return;
    }
    case IR_SPRESTORE:
        mips_mv(t, MIPS_SP, rdr(F, i->a, SCR));
        return;
    case IR_SWITCH: {
        /* A jump table in .text right after its dispatch, of 32-bit
         * offsets from the instruction `bal` returns to -- so it needs no
         * relocation, and the code is the same wherever it is linked:
         *
         *     sltiu $at, rI, n ; beqz $at, default   (li + sltu past 32767)
         *     move  t6, $ra                          (a leaf: $ra is live)
         *     bal   1f ; sll t2, rI, 2               (in the slot)
         *  1: addu  t2, t2, $ra ; lw t2, tab-1b(t2) ; addu t2, t2, $ra
         *     move  $ra, t6
         *     jr    t2 ; nop
         *   tab: .word L0-1b, L1-1b, ...
         *
         * The index is the value less the lowest case (irgen), so one
         * unsigned compare sends both sides of the range to the default.
         * A function that calls has saved $ra and reloads it, so only a
         * leaf keeps it in t6 around the bal. */
        int n = fn->jt[i->jt].n;
        int ri = rdw(F, i, i->a, ACC);
        int leaf = F->leaf;
        int anchor, lw_at, tab;
        if (n <= 32767) {
            mips_alu_imm(t, MIPS_SLTIU, CC, ri, n);
        } else {
            mips_li(t, B_LO, n);
            mips_alu(t, MIPS_SLTU, CC, ri, B_LO);
        }
        branch_to(F, MIPS_BEQ, CC, MIPS_ZERO, i->label);
        if (leaf)
            mips_mv(t, FAR, MIPS_RA);
        mips_w(t, mips_enc_branch(MIPS_BAL, MIPS_ZERO, MIPS_ZERO, 4));
        mips_shift_imm(t, sh_w(MIPS_SLL), B_LO, ri, 2);
        anchor = t->len;
        mips_alu(t, P_ADDU, B_LO, B_LO, MIPS_RA);
        lw_at = t->len;
        tab = lw_at + 4 + 4 + (leaf ? 4 : 0) + 4 + 4;
        mips_load(t, B_LO, B_LO, tab - anchor, 4, 1);
        mips_alu(t, P_ADDU, B_LO, B_LO, MIPS_RA);
        if (leaf)
            mips_mv(t, MIPS_RA, FAR);
        mips_jr(t, B_LO);
        put_slot(F, -1);
        if (t->len != tab)
            internal_error("mips: %s: the jump table is not where its "
                           "load says", fn->name);
        for (int k = 0; k < n; k++) {
            want_label(F, t->len, fn->jt[i->jt].labels[k], FX_TAB);
            F->fix[F->nfix - 1].base = anchor;
            mips_w(t, 0);
        }
        code_mark_data(t, tab, t->len);
        F->barrier = t->len;
        return;
    }
    case IR_LABELADDR: {
        if (cg_label_mark(i))       /* static data's marker: no code */
            return;
        /* &&label: the function's own address, as IR_FADDR takes it,
         * plus the label's offset in it -- the addend set once the
         * function is laid out. o32 has no PC-relative address. */
        int d = wreg(F, i->dst, ACC);
        int at = abs_pair(F, d), s0 = F->st->nf;
        struct func *self = fn->src;
        if (g_m64) {
            note_fn(F->st, at, self, RK_MIPS_HIGHEST);
            note_fn(F->st, at + 4, self, RK_MIPS_HIGHER);
            note_fn(F->st, at + 12, self, RK_MIPS_HI16);
            note_fn(F->st, at + 20, self, RK_MIPS_LO16);
        } else {
            note_fn(F->st, at, self, RK_MIPS_HI16);
            note_fn(F->st, at + 4, self, RK_MIPS_LO16);
        }
        want_label(F, at, i->label, FX_ADDR);
        F->fix[F->nfix - 1].base = s0;
        wrote(F, i->dst, d);
        return;
    }
    case IR_IGOTO:
        mips_jr(t, rdr(F, i->a, ACC));
        put_slot(F, -1);
        return;
    default:
        mips_refuse(F, i, "this operation");
    }
}

/* Copy `size` bytes from [TMP] to [ADDR] (copy) or zero them (!copy).
 * Word by word when both ends are known to be word-aligned (`aligned`),
 * else lwl/lwr and swl/swr, then a byte tail. Straight-line up to 128
 * bytes, a loop beyond. TMP and ADDR are scratch and may be moved. */
static void copy_word(struct mips_fn *F, int copy, int off, int aligned)
{
    if (copy) {
        ld_any(F, SCR, TMP, off, 4, 0, aligned);
        st_any(F, SCR, ADDR, off, 4, aligned);
    } else {
        st_any(F, MIPS_ZERO, ADDR, off, 4, aligned);
    }
}

static void copy_block(struct mips_fn *F, int copy, long size, int aligned)
{
    struct code *t = F->t;
    long k, body = size & ~3L;
    if (size > 128) {
        int top, again;
        mips_li(t, SCR2, body);
        mips_alu(t, P_ADDU, SCR2, SCR2, ADDR);
        top = t->len;
        copy_word(F, copy, 0, aligned);
        if (copy)
            mips_alu_imm(t, P_ADDIU, TMP, TMP, 4);
        mips_alu_imm(t, P_ADDIU, ADDR, ADDR, 4);
        again = br_place(F, MIPS_BNE, ADDR, SCR2);
        br_back(F, again, top);
        k = 0;
        size -= body;
    } else {
        for (k = 0; k + 4 <= size; k += 4)
            copy_word(F, copy, (int)k, aligned);
    }
    for (; k < size; k++) {
        if (copy) mips_load(t, SCR, TMP, (int)k, 1, 0);
        mips_store(t, copy ? SCR : MIPS_ZERO, ADDR, (int)k, 1);
    }
}

/* The `left` (1..W-1) bytes of a composite's last, partial word in r, to
 * the frame at off: the word's first bytes in memory order -- its low end
 * little-endian, its high end big-endian, where o32 and n64 left-justify
 * them. r may be clobbered; SCR2 is used. */
static void store_tail(struct mips_fn *F, int r, long off, long left)
{
    for (long b = 0; b < left; b++) {
        if (g_be) {
            mips_shift_imm(F->t, sh_w(MIPS_SRL), SCR2, r,
                           (int)(HB - 8 - 8 * b));
            st_sp(F, SCR2, off + b, 1);
        } else {
            if (r != SCR) { mips_mv(F->t, SCR, r); r = SCR; }
            if (b) mips_shift_imm(F->t, sh_w(MIPS_SRL), r, r, 8);
            st_sp(F, r, off + b, 1);
        }
    }
}

/* A parameter's qth incoming word in a register ready to store: its
 * argument register, or in a variadic function the copy the prologue
 * spilled -- into the caller's home area (o32) or the top of this frame
 * (n64), va_base. */
static int param_reg(struct mips_fn *F, const struct argplace *pl, int q)
{
    if (!F->fn->is_varargs)
        return argreg(pl->reg + q);
    ld_sp(F, SCR, F->va_base + (long)W * (pl->reg + q), W, 1);
    return SCR;
}

/* ---- register pairs ------------------------------------------------------
 *
 * As at RV32: the shared allocator is run first for the 64-bit values
 * alone over a pool of PAIRS (each named by its low register), and the
 * ordinary pass then treats each pair's registers as taken over that
 * value's live range (ra_reserve). */
static const struct ra_target MIPS_PAIR_RA = {
    mips_pair_pool_for, mips_callee_saved, mips_ldvar_plain,
    1, 1, 1,
    mips_op_calls_helper,
    0,
    mips_pair_hints,
    NULL, NULL,
    1,
    NULL, NULL,
    1,
    0,
    0               /* asm_in_reg */
};

static void mips_pair_hints(const struct ir_func *fn, int *hint)
{
    long blk = fn_sret(fn) ? 4 : 0;
    struct argplace pl;
    for (int p = 0; fn->src && p < fn->nparams && p < fn->nvregs; p++) {
        const struct ir_arg *a = &fn->param_abi[p];
        place_arg(a->size, arg_align(a), &blk, &pl);
        if (a->size == 8 && pl.nreg == 2 && !a->is_struct)
            hint[p] = argreg(pl.reg);
    }
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        if (i->op == IR_RET && i->a >= 0 && i->a < fn->nvregs &&
            !fn->ret_abi.is_struct && fn->ret_abi.size == 8)
            hint[i->a] = MIPS_V0;
        if (i->op != IR_CALL && mips_op_calls_helper(i) && i->w == 8) {
            if (i->a >= 0 && i->a < fn->nvregs && hint[i->a] < 0)
                hint[i->a] = MIPS_A0;
            if (!i->imm_b && i->b >= 0 && i->b < fn->nvregs &&
                hint[i->b] < 0)
                hint[i->b] = MIPS_A2;
            if (i->dst >= 0 && i->dst < fn->nvregs && hint[i->dst] < 0)
                hint[i->dst] = MIPS_V0;
        }
        if (i->op != IR_CALL)
            continue;
        if (!i->retsize && i->dst >= 0 && i->dst < fn->nvregs && i->w == 8)
            hint[i->dst] = MIPS_V0;
        blk = call_sret(i) ? 4 : 0;
        for (int k = 0; k < i->nargs; k++) {
            const struct ir_arg *a = &i->argv[k];
            place_arg(a->size, arg_align(a), &blk, &pl);
            if (a->size == 8 && pl.nreg == 2 && !a->is_struct &&
                a->vreg >= 0 && a->vreg < fn->nvregs)
                hint[a->vreg] = argreg(pl.reg);
        }
    }
}

static struct ra_range *g_mips_res;
static int g_mips_nres, g_mips_capres;
static void reserve_pairs(struct ir_func *fn, const int *loc)
{
    int nv = fn->nvregs;
    int *first = xmalloc((size_t)(nv ? nv : 1) * sizeof *first);
    int *last = xmalloc((size_t)(nv ? nv : 1) * sizeof *last);
    ra_live_ranges(fn, first, last);
    g_mips_nres = 0;
    for (int v = 0; v < nv; v++) {
        if (loc[v] < 0 || first[v] < 0) continue;
        if (g_mips_nres + 2 > g_mips_capres) {
            g_mips_capres = g_mips_capres ? g_mips_capres * 2 : 16;
            g_mips_res = xrealloc(g_mips_res,
                                  (size_t)g_mips_capres * sizeof *g_mips_res);
        }
        for (int h = 0; h < 2; h++) {
            g_mips_res[g_mips_nres].reg = loc[v] + h;
            g_mips_res[g_mips_nres].first = first[v];
            g_mips_res[g_mips_nres].last = last[v];
            g_mips_res[g_mips_nres].born = 0;
            g_mips_nres++;
        }
    }
    ra_reserve(g_mips_res, g_mips_nres);
    free(first); free(last);
}

static int *pair_alloc(struct ir_func *fn, struct mips_fn *F, const char *pin)
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
            long blk = call_sret(i) ? 4 : 0;
            struct argplace pl;
            for (int k = 0; k < i->nargs; k++) {
                const struct ir_arg *a = &i->argv[k];
                place_arg(a->size, arg_align(a), &blk, &pl);
                if (a->size > 4 && pl.nreg != 2 && a->vreg >= 0 &&
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
    loc = ra_allocate(fn, &MIPS_PAIR_RA, NULL, x, used, &nused);
    free(x);
    reserve_pairs(fn, loc);
    for (int k = 0; k < nused && k < MIPS_NPAIRS; k++)
        F->pair_used[F->npair++] = used[k];
    return loc;
}

/* ---- one function --------------------------------------------------------- */

static void gen_func(struct ir_func *fn, struct code *t, struct mips_sites *st,
                     int want_debug)
{
    struct func *f = fn->src;
    struct mips_fn F;
    int i;

    memset(&F, 0, sizeof F);
    F.fn = fn; F.t = t; F.st = st;
    F.want_debug = want_debug;
    F.wide = wide_map(fn);
    /* (the high word of a register pair, shifted, is a MIPS32 idea: at
     * MIPS64 a pair value is never in registers) */
    F.nshr = g_m64 ? NULL : ra_narrow_hishift(fn);
    for (int v = 0; F.nshr && v < fn->nvregs; v++)
        if (F.nshr[v]) F.wide[v] = 0;
    F.fb = MIPS_SP;
    if (g_mips_regalloc) {
        /* Under -g a source variable stays in its frame slot, so its
         * DW_AT_location is true (regalloc.h). */
        char *pin = want_debug ? ra_debug_pin_vars(fn) : (char *)0;
        int *pair = g_mips_pairs && !g_m64 ? pair_alloc(fn, &F, pin) : NULL;
        F.loc = ra_allocate(fn, &MIPS_RATGT, F.wide, pin, F.used_callee,
                            &F.nsave);
        g_mips_taken = 0;
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
        /* EMBCC_MIPS_RA_MAX=N leaves only the first N vregs in registers
         * -- always correct -- so a miscompile that comes and goes with N
         * names the value whose allocation is wrong. */
        {
            const char *lim = getenv("EMBCC_MIPS_RA_MAX");
            if (lim) {
                int n = atoi(lim);
                for (int v = n; v < fn->nvregs; v++)
                    F.loc[v] = -1;
            }
        }
    }
    /* fp is the frame base under alloca: saved like any callee-saved one */
    if (fn->has_alloca)
        F.used_callee[F.nsave++] = MIPS_FP;
    F.tail = NULL;
    F.sx = g_m64 ? sext_map(&F) : NULL;
    /* an interrupt handler returns with eret: no tail call, whose callee
     * would return with jr ra */
    F.isr = f->is_isr;
    if (g_mips_regalloc && !want_debug && !F.isr)
        for (i = 0; i < fn->nins; i++)
            if (mips_tail_ok(&F, i)) {
                if (!F.tail)
                    F.tail = xcalloc((size_t)fn->nins, 1);
                F.tail[i] = 1;
            }
    F.leaf = 1;
    for (i = 0; i < fn->nins; i++)
        if ((fn->ins[i].op == IR_CALL && !(F.tail && F.tail[i])) ||
            fn->ins[i].op == IR_ASM || mips_op_calls_helper(&fn->ins[i]) ||
            /* __builtin_return_address reads ra's slot */
            (fn->ins[i].op == IR_FRAMEADDR && fn->ins[i].imm == 2))
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
    /* BRANCH RELAXATION, the other way round from RISC-V's: every branch
     * is tried in its short form, and one that does not reach is given the
     * long form and the function generated again. Code only grows, so
     * what reached on one attempt may stop reaching on the next, and the
     * loop runs until nothing new fails. */
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
    F.fb = MIPS_SP;
    F.longb = longb;
    F.nlongb = nlongb;
    f->code_off = t->len;
    F.barrier = t->len;
    F.fill = !want_debug && !getenv("EMBCC_MIPS_NO_FILL");

    /* The prologue. t0 builds a large frame's size: no argument has been
     * touched yet. */
    if (F.isr) {
        mips_isr_prologue(&F);
    } else if (F.frame) {
        if (fits16(-F.frame)) {
            mips_alu_imm(t, P_ADDIU, MIPS_SP, MIPS_SP, -F.frame);
        } else {
            mips_li(t, A_LO, -F.frame);
            mips_alu(t, P_ADDU, MIPS_SP, MIPS_SP, A_LO);
        }
    }
    if (!F.leaf && !F.isr)
        st_sp(&F, MIPS_RA, F.ra_slot, W);
    for (i = 0; i < F.nsave; i++)
        st_sp(&F, F.used_callee[i], F.save_at + (long)i * W, W);
    if (fn->has_alloca) {
        mips_mv(t, MIPS_FP, MIPS_SP);
        F.fb = MIPS_FP;
    }
    /* A variadic function stores its argument registers where they and
     * the stack words are one block: a0-a3 into the home area its caller
     * reserved (o32), a0-a7 into the top 64 bytes of its own frame, just
     * below the incoming stack words (n64). */
    if (fn->is_varargs)
        for (int k = 0; k < (g_m64 ? 8 : MIPS_NARGREG); k++)
            st_sp(&F, argreg(k), F.va_base + (long)W * k, W);

    /* The parameters: each register one an edge of a parallel move into
     * wherever the allocator put it (SCR breaks a cycle), each stack one
     * a load deferred until after it, so nothing overwrites an incoming
     * argument another parameter has not read. */
    {
        struct argplace pl;
        long blk = 0;
        long base = F.frame;          /* the caller's outgoing block */
        int pmv_dst[RA_MAXPOOL], pmv_src[RA_MAXPOOL], npmv = 0;
        int pstk_reg[RA_MAXPOOL], pstk_sz[RA_MAXPOOL];
        long pstk_off[RA_MAXPOOL];
        int npstk = 0;
        if (F.sret_slot >= 0) {
            st_sp(&F, argreg(0), F.sret_slot, W);
            blk = W;
        }
        for (i = 0; i < fn->nparams; i++) {
            struct ir_arg *a = &fn->param_abi[i];
            place_arg(a->size, arg_align(a), &blk, &pl);
            if (!a->is_struct) {
                if (a->size > W && in_reg(&F, i)) {
                    for (int q = 0; q < 2; q++) {
                        if (q < pl.nreg && !fn->is_varargs) {
                            /* word q is the high one big-endian */
                            pmv_dst[npmv] = (g_be ? 1 - q : q)
                                          ? PHI(F.loc[i]) : PLO(F.loc[i]);
                            pmv_src[npmv] = argreg(pl.reg + q);
                            npmv++;
                        } else {
                            pstk_reg[npstk] = (g_be ? 1 - q : q)
                                            ? PHI(F.loc[i]) : PLO(F.loc[i]);
                            pstk_off[npstk] = q < pl.nreg
                                ? F.va_base + (long)W * (pl.reg + q)
                                : base + pl.stk + (long)W * (q - pl.nreg);
                            pstk_sz[npstk] = W;
                            npstk++;
                        }
                    }
                } else if (a->size > W) {
                    for (int q = 0; q < pl.nreg; q++)
                        st_sp(&F, param_reg(&F, &pl, q),
                              sslot(&F, i) + (long)W * q, W);
                    for (int q = 0; q < pl.nstk; q++) {
                        ld_sp(&F, SCR, base + pl.stk + (long)W * q, W, 1);
                        st_sp(&F, SCR, sslot(&F, i) + (long)W * (pl.nreg + q),
                              W);
                    }
                } else if (pl.nreg && in_reg(&F, i) && !fn->is_varargs) {
                    pmv_dst[npmv] = reg_of(&F, i);
                    pmv_src[npmv] = argreg(pl.reg);
                    npmv++;
                } else if (pl.nreg && in_reg(&F, i)) {
                    /* variadic: from the home area, which the pool's
                     * registers (no argument register) cannot disturb */
                    pstk_reg[npstk] = reg_of(&F, i);
                    pstk_off[npstk] = F.va_base + (long)W * pl.reg;
                    pstk_sz[npstk] = W;
                    npstk++;
                } else if (pl.nreg) {
                    if (F.slot[i] >= 0)
                        /* the whole word, in either order: a promoted
                         * char is right-justified in it, which is where
                         * obj_slot puts a narrow variable big-endian */
                        st_sp(&F, param_reg(&F, &pl, 0), slot32(&F, i), W);
                } else if (in_reg(&F, i)) {
                    pstk_reg[npstk] = reg_of(&F, i);
                    pstk_off[npstk] = base + pl.stk;
                    pstk_sz[npstk] = stk_scalar_size(a);
                    npstk++;
                } else if (F.slot[i] >= 0) {
                    ld_sp(&F, SCR, base + pl.stk, stk_scalar_size(a), 1);
                    st_sp(&F, SCR, slot32(&F, i), W);
                }
                continue;
            }
            /* A composite's words to its slot; an odd-sized one's last
             * word only as far as the object goes. */
            for (int q = 0; q < pl.nreg; q++) {
                long off = sslot(&F, i) + (long)W * q;
                long left = a->size - (long)W * q;
                int r = param_reg(&F, &pl, q);
                if (left >= W)
                    st_sp(&F, r, off, W);
                else
                    store_tail(&F, r, off, left);
            }
            for (int q = 0; q < pl.nstk; q++) {
                long src = base + pl.stk + (long)W * q;
                long dst = sslot(&F, i) + (long)W * (pl.nreg + q);
                long left = a->size - (long)W * (pl.nreg + q);
                ld_sp(&F, SCR, src, W, 1);
                if (left >= W)
                    st_sp(&F, SCR, dst, W);
                else
                    store_tail(&F, SCR, dst, left);
            }
        }
        if (npmv) {
            int od[RA_MAXPOOL * 2], os[RA_MAXPOOL * 2];
            int m = ra_parallel_move(pmv_dst, pmv_src, npmv, SCR, od, os,
                                     (int)(sizeof od / sizeof od[0]));
            if (m < 0)
                internal_error("mips: %s: the prologue's parameter "
                               "placement is not a well-formed move",
                               fn->name);
            for (int k = 0; k < m; k++)
                mips_mv(t, od[k], os[k]);
        }
        for (int k = 0; k < npstk; k++)
            ld_sp(&F, pstk_reg[k], pstk_off[k], pstk_sz[k], 1);
        /* where the first unnamed argument is: where the named ones end */
        if (fn->is_varargs)
            F.va_first = F.va_base + blk;
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

        /* The epilogue -- unless the body ended in a tail call and no
         * IR_RET jumps here. */
        F.label_off[fn->nlabels] = t->len;
        /* every IR_RET's branch lands here -- a barrier for the slot of
         * the jr below, unless no branch does (they are all behind) */
        for (i = 0; i < F.nfix; i++)
            if (F.fix[i].label == fn->nlabels)
                F.barrier = t->len;
        for (i = 0; tail_end && i < F.nfix; i++)
            if (F.fix[i].label == fn->nlabels)
                tail_end = 0;
        if (!tail_end) {
            if (fn->has_alloca) {
                /* every VLA at once: sp back to the frame base; the
                 * restores then address from sp, since fp is one of them */
                mips_mv(t, MIPS_SP, MIPS_FP);
                F.fb = MIPS_SP;
            }
            if (F.isr)
                mips_isr_epilogue(&F);
            else
                mips_restore(&F, 1);
            F.barrier = t->len;
        }
    }

    for (i = 0; i < F.nfix; i++) {
        int target = F.label_off[F.fix[i].label];
        if (target < 0)
            internal_error("mips: label %d of %s was never placed",
                           F.fix[i].label, fn->name);
        if (F.fix[i].kind == FX_J) {
            note_str(F.st, F.fix[i].at, target, RK_MIPS_TEXT26);
            continue;
        }
        if (F.fix[i].kind == FX_TAB) {
            mips_wrw(t, F.fix[i].at,
                     (unsigned long)(target - F.fix[i].base) & 0xffffffffUL);
            continue;
        }
        if (F.fix[i].kind == FX_ADDR) {
            for (int k = 0; k < (g_m64 ? 4 : 2); k++)
                F.st->f[F.fix[i].base + k].addend = target - f->code_off;
            continue;
        }
        if (!mips_patch_b(t, F.fix[i].at, target)) {
            if (nlongb < F.nfix) {
                longb = xrealloc(longb, (size_t)F.nfix);
                memset(longb + nlongb, 0, (size_t)(F.nfix - nlongb));
                nlongb = F.nfix;
            }
            if (longb[i])
                internal_error("mips: %s: a long branch was patched as "
                               "a short one", fn->name);
            longb[i] = 1;
            nfail++;
        }
    }
    /* An interrupt handler goes again until it saves everything it
     * writes (mips_isr_grow), with the frame laid out for the saves. */
    if (!nfail && F.isr && mips_isr_grow(&F)) {
        free(F.slot);
        layout(&F);
        continue;
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
    free(F.sx);
    free(F.loc);
}

/* With the allocator on, a function is generated with the pair pass and
 * without it, and the shorter is kept (RV32's arrangement, for the same
 * reason: a pair withheld for the whole function can cost more than it
 * saves). A discarded attempt is undone by truncating what it appended. */
static void gen_func_best(struct ir_func *fn, struct code *t,
                          struct mips_sites *st, int want_debug)
{
    int at = t->len, next = st->next, nstr = st->nstr, ng = st->ng,
        nf = st->nf, with;

    /* A field's constant offset into its load or store, before
     * allocation; room is left for the +3 of an lwl. */
    if (g_mips_regalloc && !want_debug && !getenv("EMBCC_NO_MEMOFF")) {
        char *w = wide_map(fn);
        ra_fold_memoff(fn, -32768, 32767 - 8, 4, 4, w, 0, 0);
        free(w);
    }
    g_mips_pairs = 1;
    /* (MIPS64 has no pair pass: one attempt) */
    if (!g_mips_regalloc || want_debug || getenv("EMBCC_MIPS_PAIRS") ||
        g_m64) {
        if (getenv("EMBCC_MIPS_PAIRS"))
            g_mips_pairs = atoi(getenv("EMBCC_MIPS_PAIRS"));
        gen_func(fn, t, st, want_debug);
        g_mips_pairs = 1;
        return;
    }
    gen_func(fn, t, st, want_debug);
    with = t->len - at;
    t->len = at; st->next = next; st->nstr = nstr; st->ng = ng; st->nf = nf;
    g_mips_pairs = 0;
    gen_func(fn, t, st, want_debug);
    if (t->len - at > with) {
        t->len = at; st->next = next; st->nstr = nstr; st->ng = ng;
        st->nf = nf;
        g_mips_pairs = 1;
        gen_func(fn, t, st, want_debug);
    }
    g_mips_pairs = 1;
}

/* .MIPS.abiflags, as clang writes it for -mcpu=mips32r2 -msoft-float:
 * version 0, ISA level 32 release 2, 32-bit GPRs, no FPRs, FP ABI soft
 * (3), no extensions or ASEs, flags1 ODDSPREG. */
void mips_build_abiflags(unsigned char out[24])
{
    memset(out, 0, 24);
    int m64 = target_get() == TARGET_MIPS64;
    out[2] = m64 ? 64 : 32;     /* isa_level */
    out[3] = 2;                 /* isa_rev */
    out[4] = m64 ? 2 : 1;       /* gpr_size: AFL_REG_64 / AFL_REG_32 */
    out[7] = 3;                 /* fp_abi: Val_GNU_MIPS_ABI_FP_SOFT */
    out[target_big_endian() ? 19 : 16] = 1;   /* flags1, a word: ODDSPREG */
}

void codegen_unit_mips(struct ir_unit *iu, struct code *text,
                       struct extcall **ext, int *next,
                       struct strsite **strs, int *nstrs,
                       struct gsite **gs, int *ngs,
                       struct fsite **fs, int *nfs, int want_debug,
                       int optimize, int no_sse, int regalloc)
{
    struct mips_sites st;

    (void)optimize; (void)no_sse;
    g_mips_regalloc = regalloc;
    g_be = target_big_endian();
    mips_set_big_endian(g_be);
    g_m64 = target_get() == TARGET_MIPS64;
    mips_set_64(g_m64);
    memset(&st, 0, sizeof st);
    for (int n = 0; n < iu->nfuncs; n++)
        gen_func_best(&iu->funcs[n], text, &st, want_debug);
    cg_resolve_strsites(iu, st.str, st.nstr);
    *ext = st.ext;   *next = st.next;
    *strs = st.str;  *nstrs = st.nstr;
    *gs = st.g;      *ngs = st.ng;
    *fs = st.f;      *nfs = st.nf;
}
