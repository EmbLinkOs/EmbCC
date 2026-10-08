/* Xtensa code generation: the windowed ABI of the ESP32 (LX6) and
 * ESP32-S3 (LX7), little-endian, soft float (docs/internals/xtensa-plan.md).
 *
 * The shape is the MIPS32 backend's (src/arch/mips/codegen.c), which is
 * RV32's: every vreg has one home -- a register the shared allocator gave
 * it, or a frame slot -- and every operation reads its operands through
 * rdr/rd and writes its result through wreg/wrote, so the code is correct
 * with the allocator off and smaller with it on; a 64-bit value is a
 * register pair from a pair pass. What Xtensa changes, and where:
 *
 *   * THE REGISTER WINDOW. A call8 rotates the window by eight, so the
 *     caller's a0-a7 survive every call without a save and a8-a15 do not;
 *     the hardware's window exceptions spill and refill frames. So a2-a7
 *     are the allocator's "callee-saved" registers, and cost nothing: no
 *     prologue saves anything and the epilogue is one `retw`, which also
 *     restores sp. Arguments go out in a10-a15 and arrive in a2-a7; a
 *     result comes back in a10-a13 and is returned in a2-a5.
 *   * ENTRY. The first instruction is `entry a1, frame`, which allocates
 *     the frame and moves the window; the top 32 bytes of every frame
 *     belong to the window-spill areas. A larger frame and every alloca
 *     move sp with movsp (the Alloca exception's handler keeps the spill
 *     area right).
 *   * NO ZERO REGISTER AND NO SET-LESS-THAN. A comparison's 0 or 1 is a
 *     branch over a movi (emit_bool), and branches compare two registers,
 *     a register with zero, or a register with one of sixteen constants.
 *   * SHORT BRANCHES. A two-register branch reaches +-128 bytes, a
 *     zero-test +-2 KiB, j +-128 KiB. Every branch is tried short; one
 *     that does not reach becomes the inverse branch over a j, and the
 *     function is generated again until nothing new fails.
 *   * LITERALS. A constant beyond what movi (and a shift or an addi) can
 *     build, and every address, is an l32r from the function's literal
 *     pool, which sits right before the function because l32r reaches
 *     only backwards. The pool's size is reserved before the body is
 *     generated and grown by another attempt if the body wants more.
 *   * MISALIGNED ACCESS raises an exception, as on the ESP32; a load or
 *     store irgen cannot promise is aligned goes byte by byte.
 *   * VOLATILE accesses are preceded by memw, as GCC's default
 *     -mserialize-volatile has them.
 *
 * Refused by name: a jump table (target_jump_tables keeps a dense switch
 * a decision tree), atomics wider than a word,
 * the frame and return address, a function too large for its branches or
 * its literal pool, and __int128 (which does not exist on ILP32). THE RULE.
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

/* The scratch registers: four of a8-a15, none in the allocator's pool and
 * all of them clobbered by any call anyway. A 64-bit binary operation
 * holds its two operands in A_LO:A_HI and B_LO:B_HI; the 32-bit lowerings
 * compute in ACC with a second operand in TMP and an address in ADDR. */
#define A_LO XT_A8
#define A_HI XT_A9
#define B_LO XT_A14
#define B_HI XT_A15
#define ACC  XT_A8
#define TMP  XT_A9
#define SCR  XT_A14
#define ADDR XT_A15
/* The frame base in a function that calls alloca: sp at entry. GCC's
 * frame pointer is a7 too. */
#define FBREG XT_A7
/* Not a register: "compare with zero" in a branch test. */
#define ZERO (-1)

/* The six argument words: a2-a7 in the callee, a10-a15 in the caller. */
#define NARGW 6
#define IN_ARG(k)  (XT_A2 + (k))
#define OUT_ARG(k) (XT_A10 + (k))

struct xt_sites {
    struct extcall *ext;   int next, capext;
    struct strsite *str;   int nstr, capstr;
    struct gsite *g;       int ng, capg;
    struct fsite *f;       int nf, capf;
};

/* A literal-pool entry: a constant, or an address the linker fills in. */
enum { LIT_CONST, LIT_STR, LIT_GLOB, LIT_FUNC, LIT_LABEL };
struct xt_lit {
    int kind;
    unsigned long v;       /* LIT_CONST: the word; LIT_STR: the string
                            * index; LIT_LABEL: the label */
    void *p;               /* LIT_GLOB: struct global *; LIT_FUNC: struct func * */
};

struct xt_fn {
    int *usecnt;         /* per vreg: how many reads (fusion), or NULL */
    int skip_next;       /* the instruction after this one is already out */
    int want_debug;
    struct ir_func *fn;
    int *loc;            /* per vreg: its register, -1 in memory; NULL at -O0 */
    int used_callee[RA_MAXPOOL];
    int nsave;           /* what the allocator reports; nothing is saved */
    int pair_used[8], npair;
    struct code *t;
    struct xt_sites *st;
    char *wide;          /* per vreg: a 64-bit value, a register pair */
    char *nshr;          /* per vreg: a narrow high-word shift */
    long *slot;          /* per vreg: byte offset from the frame base, -1 */
    long frame;          /* the frame `entry` (and movsp) allocate */
    long scratch_at;     /* where fn->scratch_bytes begins */
    long sret_slot;      /* where the hidden result pointer is kept, or -1 */
    long va_save;        /* a variadic function's a2-a7, or -1 */
    int va_words;        /* ...and how many words the named arguments took */
    int fb;              /* the frame base: sp, or FBREG under alloca */
    long out_bytes;      /* the outgoing stack-argument area */
    int *label_off;      /* per label id, or -1 while unseen */
    struct { int at; int label; int kind; } *fix;
    int nfix, capfix;
    /* Per branch, in emission order: 1 take the long form (the inverse
     * branch over a j), 2 the far one (over an l32r of the label's
     * address and a jx). */
    const char *longb;
    int nlongb;
    /* The literal pool: npool words reserved at pool_at, before the
     * function's entry; the body asked for nlit of them. */
    struct xt_lit *lit;
    int nlit, caplit;
    int pool_at, npool;
    /* Literals placed in the code itself, jumped over, for an l32r more
     * than 256 KiB past the pool: their offsets, and what each holds. */
    struct xt_lit *isl;
    int *isl_at;
    int nisl, capisl;
};

/* ---- the allocator's view of this machine --------------------------------
 *
 * a10-a13 first: caller-saved, the registers a short-lived value takes
 * and where a call's arguments and result are. Then a2-a7, which survive
 * every call for free. a7 is the frame base under alloca. */
#define XT_NPOOL 10
static const int XT_POOL[XT_NPOOL] = {
    XT_A10, XT_A11, XT_A12, XT_A13, XT_A2, XT_A3, XT_A4, XT_A5, XT_A6, XT_A7
};

static unsigned long g_xt_taken;      /* registers the pair pass took */
static int g_xt_pairs = 1;            /* this attempt uses the pair pass */
static int g_xt_pool[XT_NPOOL];

static const int *xt_pool_for(const struct ir_func *fn, int *n)
{
    int k = 0;
    for (int j = 0; j < XT_NPOOL; j++) {
        if (fn->has_alloca && XT_POOL[j] == FBREG)
            continue;
        if (g_xt_taken >> XT_POOL[j] & 1)
            continue;
        g_xt_pool[k++] = XT_POOL[j];
    }
    *n = k;
    return g_xt_pool;
}

/* The PAIR pool, each pair named by its low register: a10:a11 and
 * a12:a13 (where a 64-bit argument and result travel), then a2:a3 ..
 * a6:a7 for one that lives across a call. */
#define XT_NPAIRS 5
static const int XT_PAIRS[XT_NPAIRS] = { XT_A10, XT_A12, XT_A2, XT_A4, XT_A6 };
static int g_xt_pairpool[XT_NPAIRS];
static const int *xt_pair_pool_for(const struct ir_func *fn, int *n)
{
    int k = 0;
    for (int j = 0; j < XT_NPAIRS; j++)
        if (!(fn->has_alloca && XT_PAIRS[j] == XT_A6))
            g_xt_pairpool[k++] = XT_PAIRS[j];
    *n = k;
    return g_xt_pairpool;
}

static void xt_pair_hints(const struct ir_func *fn, int *hint);

/* a2-a7: the window keeps them across a call8. */
static int xt_callee_saved(int r)
{
    return r >= XT_A2 && r <= XT_A7;
}

/* `dst = load(local)` is a plain move at the full register width. */
static int xt_ldvar_plain(int size, int sign, int w)
{
    (void)sign;
    return size == 4 && w == 4;
}

/* Which instructions become a CALL the IR does not show as one: every
 * floating-point operation (soft float), a 64-bit multiply, divide or
 * remainder. */
int xtensa_op_calls_helper(const struct ir_ins *i)
{
    if (i->flt)
        return i->op == IR_ADD || i->op == IR_SUB || i->op == IR_MUL ||
               i->op == IR_DIV || i->op == IR_CMP;
    if (i->op == IR_I2F || i->op == IR_F2I || i->op == IR_F2F)
        return 1;
    return (i->op == IR_DIV || i->op == IR_MOD || i->op == IR_MUL) &&
           i->w == 8;
}

static void xt_abi_hints(const struct ir_func *fn, int *hint);

static const struct ra_target XT_RATGT = {
    xt_pool_for,
    xt_callee_saved,
    xt_ldvar_plain,
    1, 1, 1,        /* call args, returns and memcpy addresses from registers */
    xtensa_op_calls_helper,
    0,              /* three-operand */
    xt_abi_hints,
    NULL, NULL,     /* no FP class: soft float in the address registers */
    1,              /* float_in_gpr */
    NULL, NULL,
    1,              /* atomic_in_reg: the s32c1i loops read through rdr */
    0,
    1               /* asm_in_reg: see IR_ASM */
};

static int g_xt_regalloc;

/* ---- refusal ---------------------------------------------------------------- */

static void xt_refuse(const struct xt_fn *F, const struct ir_ins *i,
                      const char *what)
{
    char op[64];
    op[0] = 0;
    if (i)
        snprintf(op, sizeof op, " [%s w=%d size=%d]", ir_opname(i->op),
                 i->w, i->size);
    fprintf(stderr,
            "embcc: %s:%d: error: the Xtensa backend cannot lower %s yet "
            "(function %s)%s\n",
            F->fn->file ? F->fn->file : "?",
            i ? i->line : F->fn->line, what, F->fn->name, op);
    exit(1);
}

/* ---- which values are eight bytes wide --------------------------------------
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

/* ---- the windowed calling convention ---------------------------------------
 *
 * GCC's rules (xtensa.cc, xtensa_function_arg_1 and _advance), checked
 * against Espressif's GCC (tests/golden/xtensa-abi.sh):
 *
 *   * Arguments are counted in words, six of them in registers. A type
 *     aligned beyond 4 rounds the word count up to its alignment in words
 *     (at most 16 bytes) -- an even register for a long long.
 *   * One that does not fit wholly in the registers left goes on the
 *     stack, and the count jumps to six, so every argument after it does
 *     too. Stack arguments are laid out from the caller's sp, each at its
 *     own alignment (4 to 16), in whole words.
 *   * A struct is passed by value whatever its size; a _Complex is split
 *     into its two parts, each an argument of its own.
 *   * Up to 16 bytes of result come back in a2-a5; a larger struct
 *     through a hidden pointer that is the first argument.
 */
struct xpiece {
    int nw;              /* words */
    int reg;             /* the first argument word (0-5), or -1: stack */
    long stk;            /* the stack offset, when reg < 0 */
};
struct xplace {
    int np;              /* 1, or 2 for a split _Complex */
    int esz;             /* bytes per piece (the element of a complex) */
    struct xpiece p[2];
};

static void place_piece(int size, int align, int *words, long *stk,
                        struct xpiece *pc)
{
    int nw = (size + 3) / 4;
    if (align > 4) {
        int a = (align > 16 ? 16 : align) / 4;
        *words = (*words + a - 1) & -a;
    }
    pc->nw = nw;
    if (*words + nw <= NARGW) {
        pc->reg = *words;
        pc->stk = -1;
        *words += nw;
        return;
    }
    if (*words < NARGW)
        *words = NARGW;
    *words += nw;
    {
        long al = align < 4 ? 4 : align > 16 ? 16 : align;
        *stk = (*stk + al - 1) & ~(al - 1);
        pc->reg = -1;
        pc->stk = *stk;
        *stk += 4L * nw;
    }
}

/* Is this argument a _Complex, passed as its two parts? */
static int arg_is_complex(const struct ir_arg *a)
{
    return a->is_struct && a->ty && a->ty->is_complex && a->ty->celem;
}

static void place_arg(const struct ir_arg *a, int *words, long *stk,
                      struct xplace *pl)
{
    if (arg_is_complex(a)) {
        int esz = a->size / 2;
        pl->np = 2;
        pl->esz = esz;
        place_piece(esz, esz, words, stk, &pl->p[0]);
        place_piece(esz, esz, words, stk, &pl->p[1]);
        return;
    }
    pl->np = 1;
    pl->esz = a->size;
    place_piece(a->size, a->is_struct ? (a->align ? a->align : 4)
                                      : (a->size > 4 ? 8 : 4),
                words, stk, &pl->p[0]);
}

/* How many words of a result come back in registers: 1 or 2 for a
 * scalar, up to 4 for a composite of 16 bytes or less, 0 for one that
 * goes through the hidden pointer. */
static int ret_words(int is_struct, long size)
{
    if (!is_struct)
        return size > 4 ? 2 : 1;
    return size <= 16 ? (int)((size + 3) / 4) : 0;
}

static int fn_sret(const struct ir_func *fn)
{
    return fn->ret_abi.is_struct && ret_words(1, fn->ret_abi.size) == 0;
}

static int call_sret(const struct ir_ins *i)
{
    return i->retsize && ret_words(1, i->retsize) == 0;
}

/* Where the ABI would put each value: a parameter in the register it
 * arrives in, a call's arguments in theirs, a result in a10 (a2 in a
 * return), a soft-float helper's operands in a10 and a11. Hints only. */
static void xt_abi_hints(const struct ir_func *fn, int *hint)
{
    int words = fn_sret(fn) ? 1 : 0;
    long stk = 0;
    struct xplace pl;
    for (int p = 0; fn->src && p < fn->nparams && p < fn->nvregs; p++) {
        const struct ir_arg *a = &fn->param_abi[p];
        place_arg(a, &words, &stk, &pl);
        if (pl.np == 1 && pl.p[0].nw == 1 && pl.p[0].reg >= 0 &&
            !a->is_struct && !fn->is_varargs)
            hint[p] = IN_ARG(pl.p[0].reg);
    }
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        if (i->op == IR_RET && i->a >= 0 && i->a < fn->nvregs &&
            !fn->ret_abi.is_struct && fn->ret_abi.size <= 4)
            hint[i->a] = XT_A2;
        if (i->op != IR_CALL && xtensa_op_calls_helper(i) && i->w <= 4 &&
            i->size <= 4) {
            if (i->a >= 0 && i->a < fn->nvregs && hint[i->a] < 0)
                hint[i->a] = XT_A10;
            if (!i->imm_b && i->b >= 0 && i->b < fn->nvregs &&
                hint[i->b] < 0)
                hint[i->b] = XT_A11;
            if (i->dst >= 0 && i->dst < fn->nvregs && hint[i->dst] < 0)
                hint[i->dst] = XT_A10;
        }
        if (i->op != IR_CALL)
            continue;
        if (!i->retsize && i->dst >= 0 && i->dst < fn->nvregs && i->w <= 4)
            hint[i->dst] = XT_A10;
        words = call_sret(i) ? 1 : 0;
        stk = 0;
        for (int k = 0; k < i->nargs; k++) {
            const struct ir_arg *a = &i->argv[k];
            place_arg(a, &words, &stk, &pl);
            if (pl.np == 1 && pl.p[0].nw == 1 && pl.p[0].reg >= 0 &&
                !a->is_struct && a->vreg >= 0 && a->vreg < fn->nvregs)
                hint[a->vreg] = OUT_ARG(pl.p[0].reg);
        }
    }
}

/* ---- the frame ---------------------------------------------------------------
 *
 * From sp upward: the outgoing stack arguments, the shared temp slots,
 * 64-bit temps without a pair, locals (small ones first), the
 * struct-return scratch, the sret pointer, a variadic function's register
 * save area; then the 32 bytes the window-spill handlers own. A multiple
 * of 16, at least 32 (GCC's compute_frame_size). */
static long outgoing_area(const struct xt_fn *F)
{
    const struct ir_func *fn = F->fn;
    long most = 0;
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        struct xplace pl;
        int words;
        long stk = 0;
        if (i->op != IR_CALL)
            continue;
        words = call_sret(i) ? 1 : 0;
        for (int k = 0; k < i->nargs; k++)
            place_arg(&i->argv[k], &words, &stk, &pl);
        if (stk > most)
            most = stk;
    }
    return (most + 15) & ~15L;
}

static int in_reg(const struct xt_fn *F, int v);

static void layout(struct xt_fn *F)
{
    struct ir_func *fn = F->fn;
    long off = outgoing_area(F);
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
            struct ra_slots so = { loc2, NULL, g_xt_regalloc, has_cgoto };
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
                if (align > 16)
                    xt_refuse(F, NULL, "a local aligned beyond the 16-byte "
                                       "stack");
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

    F->sret_slot = -1;
    if (fn_sret(fn)) {
        off = (off + 3) & ~3L;
        F->sret_slot = off;
        off += 4;
    }
    F->va_save = -1;
    if (fn->is_varargs) {
        off = (off + 3) & ~3L;
        F->va_save = off;
        off += 4L * NARGW;
    }
    F->frame = (off + 32 + 15) & ~15L;
}

/* ---- literals -------------------------------------------------------------- */

static int lit_index(struct xt_fn *F, int kind, unsigned long v, void *p)
{
    for (int k = 0; k < F->nlit; k++)
        if (F->lit[k].kind == kind && F->lit[k].v == v && F->lit[k].p == p)
            return k;
    if (F->nlit == F->caplit) {
        F->caplit = F->caplit ? F->caplit * 2 : 16;
        F->lit = xrealloc(F->lit, (size_t)F->caplit * sizeof *F->lit);
    }
    F->lit[F->nlit].kind = kind;
    F->lit[F->nlit].v = v;
    F->lit[F->nlit].p = p;
    return F->nlit++;
}

/* l32r reg from literal k. A literal beyond the reserved pool is still
 * counted (the attempt is repeated with a pool that large) and the
 * instruction is a placeholder of the same length. */
static void lit_load(struct xt_fn *F, int reg, int kind, unsigned long v,
                     void *p)
{
    int k = lit_index(F, kind, v, p);
    long at = F->t->len, lit = F->pool_at + 4L * k;
    if (k >= F->npool) {
        xt_nop(F->t);
        return;
    }
    if (!xt_l32r_reaches(at, lit)) {
        /* Past the pool's reach (a function over 256 KiB): an island of
         * one word in the code, jumped over, and the l32r just after it. */
        int jat = at, wat = (int)((at + 3 + 3) & ~3L);
        xt_w(F->t, xt_enc_j((long)(wat + 4) - (jat + 4)));
        while (F->t->len < wat)
            code_byte(F->t, 0);
        code_u32(F->t, 0);
        code_mark_data(F->t, wat, wat + 4);
        if (F->nisl == F->capisl) {
            F->capisl = F->capisl ? F->capisl * 2 : 16;
            F->isl = xrealloc(F->isl, (size_t)F->capisl * sizeof *F->isl);
            F->isl_at = xrealloc(F->isl_at,
                                 (size_t)F->capisl * sizeof *F->isl_at);
        }
        F->isl[F->nisl].kind = kind;
        F->isl[F->nisl].v = v;
        F->isl[F->nisl].p = p;
        F->isl_at[F->nisl++] = wat;
        xt_l32r(F->t, reg, wat);
        return;
    }
    xt_l32r(F->t, reg, lit);
}

/* reg = v, the low 32 bits of it: movi, or movi and a shift, or a
 * literal. */
static void li(struct xt_fn *F, int reg, long long v)
{
    long long w = (long long)(int)(unsigned int)(unsigned long long)v;
    int n = xt_li_inline_len(w);
    if (n && n <= 6) {
        xt_li_inline(F->t, reg, w);
        return;
    }
    lit_load(F, reg, LIT_CONST, (unsigned long)w & 0xffffffffUL, NULL);
}

static void addr_off(struct xt_fn *F, int reg, int base, long off);

/* ---- the frame, addressed --------------------------------------------------- */

/* base + off into `tmp` such that what is left fits a `size`-byte access:
 * returns the residual offset. addmi takes the 256-byte multiple, or a
 * literal and an add a larger one. */
static long far_base(struct xt_fn *F, int tmp, int base, long off, int size)
{
    long lo = off & 255, hi = off - lo;
    if (xt_mem_ok(off, size))
        return off;
    if (lo % size) {
        /* the base is not aligned the way the access is: the whole sum
         * goes into tmp */
        addr_off(F, tmp, base, off);
        return 0;
    }
    if (hi >= -32768 && hi <= 32512) {
        xt_addmi(F->t, tmp, base, hi);
    } else {
        li(F, tmp, hi);
        xt_alu(F->t, XT_ADD, tmp, base, tmp);
    }
    return lo;
}

/* A load of `size` bytes at base + off into reg (which may serve as the
 * address scratch, since it is written anyway). */
static void ld_off(struct xt_fn *F, int reg, int base, long off, int size,
                   int sign)
{
    if (!xt_mem_ok(off, size)) {
        off = far_base(F, reg, base, off, size);
        base = reg;
    }
    xt_load(F->t, reg, base, off, size, sign && size == 2);
    if (sign && size == 1)
        xt_sext(F->t, reg, reg, 7);
}

/* A store of reg at base + off, `tmp` the address scratch if one is
 * needed (not reg). */
static void st_off(struct xt_fn *F, int reg, int base, long off, int size,
                   int tmp)
{
    if (!xt_mem_ok(off, size)) {
        if (tmp == reg || tmp == base)
            internal_error("xtensa: %s: a far store with no scratch",
                           F->fn->name);
        off = far_base(F, tmp, base, off, size);
        base = tmp;
    }
    xt_store(F->t, reg, base, off, size);
}

/* The address scratch for a store of `reg` to the frame. */
static int st_tmp(int reg)
{
    return reg == ADDR ? SCR : ADDR;
}

static void ld_sp(struct xt_fn *F, int reg, long off, int size, int sign)
{
    ld_off(F, reg, F->fb, off, size, sign);
}

static void st_sp(struct xt_fn *F, int reg, long off, int size)
{
    st_off(F, reg, F->fb, off, size, st_tmp(reg));
}

/* A store into the OUTGOING area, at the live sp: the callee finds its
 * stack arguments at its own entry sp, which after an alloca is not the
 * frame base. `tmp` is a scratch the caller is not using. */
static void st_out(struct xt_fn *F, int reg, long off, int size, int tmp)
{
    st_off(F, reg, XT_SP, off, size, tmp);
}

/* reg = frame base + off. */
static void addr_off(struct xt_fn *F, int reg, int base, long off)
{
    if (xt_addi_any(F->t, reg, base, off))
        return;
    li(F, reg, off);
    xt_alu(F->t, XT_ADD, reg, base, reg);
}

static void addr_sp(struct xt_fn *F, int reg, long off)
{
    addr_off(F, reg, F->fb, off);
}

static int in_reg(const struct xt_fn *F, int v)
{
    return F->loc && v >= 0 && F->loc[v] >= 0;
}

static long sslot(const struct xt_fn *F, int v)
{
    if (v < 0 || v >= F->fn->nvregs || F->slot[v] < 0)
        internal_error("xtensa: %s: a path addresses vreg %d's slot, and it "
                       "has none", F->fn->name, v);
    return F->slot[v];
}

/* rd: v into exactly `reg`. rdr: where v IS (its register, or `scratch`
 * after a load). wreg: where to compute v. wrote: commit it. */
static void rd(struct xt_fn *F, int v, int reg)
{
    if (in_reg(F, v)) {
        if (F->loc[v] != reg)
            xt_mov(F->t, reg, F->loc[v]);
        return;
    }
    ld_sp(F, reg, sslot(F, v), 4, 0);
}

static int rdr(struct xt_fn *F, int v, int scratch)
{
    if (in_reg(F, v))
        return F->loc[v];
    ld_sp(F, scratch, sslot(F, v), 4, 0);
    return scratch;
}

static int wreg(struct xt_fn *F, int v, int scratch)
{
    return in_reg(F, v) ? F->loc[v] : scratch;
}

static void wrote(struct xt_fn *F, int v, int reg)
{
    if (in_reg(F, v)) {
        if (F->loc[v] != reg)
            xt_mov(F->t, F->loc[v], reg);
        return;
    }
    if (v < 0 || F->slot[v] < 0)
        return;
    st_sp(F, reg, sslot(F, v), 4);
}

static void wr(struct xt_fn *F, int v, int reg)
{
    wrote(F, v, reg);
}

/* dl <- sl and dh <- sh as one parallel move (`via` breaks a swap). */
static void mv2(struct xt_fn *F, int dl, int sl, int dh, int sh, int via)
{
    if (dl == sh && dh == sl) {
        if (dl == sl) return;
        xt_mov(F->t, via, sl);
        xt_mov(F->t, dh, sh);
        xt_mov(F->t, dl, via);
        return;
    }
    if (dl == sh) {
        if (dh != sh) xt_mov(F->t, dh, sh);
        if (dl != sl) xt_mov(F->t, dl, sl);
        return;
    }
    if (dl != sl) xt_mov(F->t, dl, sl);
    if (dh != sh) xt_mov(F->t, dh, sh);
}

/* A 64-bit value: its pair (low word in loc, high in loc + 1), or its
 * slot, low word first. */
static void rd64(struct xt_fn *F, int v, int lo, int hi)
{
    if (in_reg(F, v)) {
        mv2(F, lo, F->loc[v], hi, F->loc[v] + 1, SCR == lo || SCR == hi
                                                  ? ADDR : SCR);
        return;
    }
    ld_sp(F, lo, sslot(F, v), 4, 0);
    ld_sp(F, hi, sslot(F, v) + 4, 4, 0);
}

static void wr64(struct xt_fn *F, int v, int lo, int hi)
{
    if (in_reg(F, v)) {
        int via = SCR;
        while (via == lo || via == hi || via == F->loc[v] ||
               via == F->loc[v] + 1)
            via = via == SCR ? ADDR : via == ADDR ? TMP : ACC;
        mv2(F, F->loc[v], lo, F->loc[v] + 1, hi, via);
        return;
    }
    if (v < 0 || F->slot[v] < 0)
        return;
    {
        int tmp = ADDR;
        while (tmp == lo || tmp == hi)
            tmp = tmp == ADDR ? SCR : tmp == SCR ? TMP : ACC;
        st_off(F, lo, F->fb, sslot(F, v), 4, tmp);
        st_off(F, hi, F->fb, sslot(F, v) + 4, 4, tmp);
    }
}

/* A folded constant as the register holds it: its low 32 bits,
 * sign-extended. */
static long long imm_val(const struct ir_ins *i)
{
    return (long long)(int)(unsigned int)(unsigned long)i->imm;
}

static void operand_b(struct xt_fn *F, const struct ir_ins *i, int reg)
{
    if (i->imm_b)
        li(F, reg, imm_val(i));
    else
        rd(F, i->b, reg);
}

static void operand_b64(struct xt_fn *F, const struct ir_ins *i,
                        int lo, int hi)
{
    if (i->imm_b) {
        li(F, lo, (long long)(i->imm & 0xffffffffL));
        li(F, hi, (long long)((i->imm >> 32) & 0xffffffffL));
    } else {
        rd64(F, i->b, lo, hi);
    }
}

/* Sign- or zero-extend the low `size` bytes of rs into rdst. */
static void ext_reg(struct xt_fn *F, int rdst, int rs, int size, int sign)
{
    if (size >= 4) {
        if (rdst != rs)
            xt_mov(F->t, rdst, rs);
        return;
    }
    if (sign)
        xt_sext(F->t, rdst, rs, size * 8 - 1);
    else
        xt_extui(F->t, rdst, rs, 0, size * 8);
}

/* ---- loads and stores that may be misaligned ------------------------------
 *
 * A halfword or word access to an address that is not a multiple of its
 * size raises the LoadStoreAlignment exception. C promises alignment
 * everywhere but a packed struct's member, and irgen marks the accesses it
 * can promise (ir_ins.natural); the rest go a byte at a time, assembled
 * in `tmp` (which must be neither rt nor base). */
static void ld_any(struct xt_fn *F, int rt, int base, long off, int size,
                   int sign, int aligned, int tmp, int tmp2)
{
    struct code *t = F->t;
    if (aligned || size == 1) {
        if (!xt_mem_ok(off, size)) {
            int r = rt == base ? tmp : rt;
            off = far_base(F, r, base, off, size);
            base = r;
        }
        xt_load(t, rt, base, off, size, sign && size == 2);
        if (sign && size == 1)
            xt_sext(t, rt, rt, 7);
        return;
    }
    /* Offsets here are a folded memoff (at most 248) plus at most 7, so
     * every byte is within l8ui's reach. The value is built from the
     * highest byte down in `acc`, a byte at a time through `by`. */
    if (off < 0 || !xt_mem_ok(off + size - 1, 1))
        internal_error("xtensa: %s: an unaligned access at offset %ld",
                       F->fn->name, off);
    {
        int acc = rt == base ? tmp : rt, by = rt == base ? tmp2 : tmp;
        if (acc == base || by == base || acc == by)
            internal_error("xtensa: %s: an unaligned load with no scratch",
                           F->fn->name);
        xt_load(t, acc, base, off + size - 1, 1, 0);
        if (sign)
            xt_sext(t, acc, acc, 7);
        for (long b = size - 2; b >= 0; b--) {
            xt_load(t, by, base, off + b, 1, 0);
            xt_slli(t, acc, acc, 8);
            xt_alu(t, XT_OR, acc, acc, by);
        }
        if (acc != rt)
            xt_mov(t, rt, acc);
    }
}

static void st_any(struct xt_fn *F, int rt, int base, long off, int size,
                   int aligned, int tmp)
{
    struct code *t = F->t;
    if (aligned || size == 1) {
        if (!xt_mem_ok(off, size)) {
            if (tmp == rt || tmp == base)
                internal_error("xtensa: %s: a far store with no scratch",
                               F->fn->name);
            off = far_base(F, tmp, base, off, size);
            base = tmp;
        }
        xt_store(t, rt, base, off, size);
        return;
    }
    if (off < 0 || !xt_mem_ok(off + size - 1, 1))
        internal_error("xtensa: %s: an unaligned access at offset %ld",
                       F->fn->name, off);
    if (tmp == rt || tmp == base)
        internal_error("xtensa: %s: an unaligned store with no scratch",
                       F->fn->name);
    xt_store(t, rt, base, off, 1);
    for (long b = 1; b < size; b++) {
        if (8 * b < 16)
            xt_srli(t, tmp, rt, (int)(8 * b));
        else
            xt_extui(t, tmp, rt, (int)(8 * b), 8);
        xt_store(t, tmp, base, off + b, 1);
    }
}

/* ---- branches -----------------------------------------------------------------
 *
 * A test is one of the four branch shapes: two registers (RR), one with
 * zero (Z), one with a b4const/b4constu constant (I), or one bit (BIT). */
enum { T_RR, T_Z, T_I, T_BIT, T_TRUE, T_FALSE };
struct xtest {
    int kind;
    int cond;            /* xt_cond, xt_zcond or xt_icond; T_BIT: set */
    int s, t;            /* registers (t unused but for T_RR) */
    long long k;         /* T_I's constant, T_BIT's bit */
};

static struct xtest test_rr(int cond, int s, int t)
{
    struct xtest x;
    x.kind = T_RR; x.cond = cond; x.s = s; x.t = t; x.k = 0;
    return x;
}

static struct xtest test_z(int cond, int s)
{
    struct xtest x;
    x.kind = T_Z; x.cond = cond; x.s = s; x.t = -1; x.k = 0;
    return x;
}

static struct xtest test_i(int cond, int s, long long k)
{
    struct xtest x;
    x.kind = T_I; x.cond = cond; x.s = s; x.t = -1; x.k = k;
    return x;
}

static struct xtest test_bit(int set, int s, int bit)
{
    struct xtest x;
    x.kind = T_BIT; x.cond = set; x.s = s; x.t = -1; x.k = bit;
    return x;
}

static struct xtest test_const(int truth)
{
    struct xtest x;
    x.kind = truth ? T_TRUE : T_FALSE; x.cond = 0; x.s = x.t = -1; x.k = 0;
    return x;
}

static struct xtest invert(struct xtest x)
{
    switch (x.kind) {
    case T_RR:
        switch (x.cond) {
        case XT_BEQ:   x.cond = XT_BNE; break;
        case XT_BNE:   x.cond = XT_BEQ; break;
        case XT_BLT:   x.cond = XT_BGE; break;
        case XT_BGE:   x.cond = XT_BLT; break;
        case XT_BLTU:  x.cond = XT_BGEU; break;
        case XT_BGEU:  x.cond = XT_BLTU; break;
        case XT_BANY:  x.cond = XT_BNONE; break;
        case XT_BNONE: x.cond = XT_BANY; break;
        case XT_BALL:  x.cond = XT_BNALL; break;
        case XT_BNALL: x.cond = XT_BALL; break;
        case XT_BBC:   x.cond = XT_BBS; break;
        default:       x.cond = XT_BBC; break;     /* XT_BBS */
        }
        return x;
    case T_Z:
        x.cond = x.cond == XT_BEQZ ? XT_BNEZ : x.cond == XT_BNEZ ? XT_BEQZ
               : x.cond == XT_BLTZ ? XT_BGEZ : XT_BLTZ;
        return x;
    case T_I:
        x.cond = x.cond == XT_BEQI ? XT_BNEI : x.cond == XT_BNEI ? XT_BEQI
               : x.cond == XT_BLTI ? XT_BGEI : x.cond == XT_BGEI ? XT_BLTI
               : x.cond == XT_BLTUI ? XT_BGEUI : XT_BLTUI;
        return x;
    case T_BIT:
        x.cond = !x.cond;
        return x;
    case T_TRUE:
        x.kind = T_FALSE;
        return x;
    default:
        x.kind = T_TRUE;
        return x;
    }
}

/* Does the test read register r? */
static int test_reads(const struct xtest *x, int r)
{
    return (x->kind != T_TRUE && x->kind != T_FALSE && x->s == r) ||
           (x->kind == T_RR && x->t == r);
}

/* The branch word for a test, `off` bytes from the next instruction. */
static unsigned long enc_test(const struct xtest *x, long off)
{
    switch (x->kind) {
    case T_RR:  return xt_enc_b(x->cond, x->s, x->t, off);
    case T_Z:   return xt_enc_bz(x->cond, x->s, off);
    case T_I:   return xt_enc_bi(x->cond, x->s, x->k, off);
    case T_BIT: return xt_enc_bbi(x->cond, x->s, (int)x->k, off);
    default:
        internal_error("xtensa: encoding a constant test");
        return 0;
    }
}

/* The test `a pred b` of two registers (b may be ZERO). */
static struct xtest pred_test(enum binop pred, int sign, int a, int b)
{
    if (b == ZERO) {
        switch (pred) {
        case B_EQ: return test_z(XT_BEQZ, a);
        case B_NE: return test_z(XT_BNEZ, a);
        case B_LT: return sign ? test_z(XT_BLTZ, a) : test_const(0);
        case B_GE: return sign ? test_z(XT_BGEZ, a) : test_const(1);
        case B_GT: return sign ? test_i(XT_BGEI, a, 1) : test_z(XT_BNEZ, a);
        default:   return sign ? test_i(XT_BLTI, a, 1) : test_z(XT_BEQZ, a);
        }
    }
    switch (pred) {
    case B_EQ: return test_rr(XT_BEQ, a, b);
    case B_NE: return test_rr(XT_BNE, a, b);
    case B_LT: return test_rr(sign ? XT_BLT : XT_BLTU, a, b);
    case B_GE: return test_rr(sign ? XT_BGE : XT_BGEU, a, b);
    case B_GT: return test_rr(sign ? XT_BLT : XT_BLTU, b, a);
    default:   return test_rr(sign ? XT_BGE : XT_BGEU, b, a);   /* B_LE */
    }
}

/* The test `a pred k` against a constant (as the register holds it:
 * sign-extended 32 bits), or 0 when no single branch can test it. */
static int pred_test_imm(enum binop pred, int sign, int a, long long k,
                         struct xtest *out)
{
    unsigned long long u = (unsigned long long)k & 0xffffffffULL;
    if (k == 0) {
        *out = pred_test(pred, sign, a, ZERO);
        return 1;
    }
    switch (pred) {
    case B_EQ: case B_NE:
        if (!xt_bi_ok(XT_BEQI, k))
            return 0;
        *out = test_i(pred == B_EQ ? XT_BEQI : XT_BNEI, a, k);
        return 1;
    case B_LE: case B_GT:
        /* x <= k is x < k + 1, x > k is x >= k + 1 */
        if (sign ? k == 2147483647LL : u == 0xffffffffULL) {
            *out = test_const(pred == B_LE);
            return 1;
        }
        k = sign ? k + 1 : (long long)(u + 1);
        u = (unsigned long long)k & 0xffffffffULL;
        pred = pred == B_LE ? B_LT : B_GE;
        break;
    default:
        break;
    }
    if (sign) {
        if (!xt_bi_ok(XT_BLTI, k))
            return 0;
        *out = test_i(pred == B_LT ? XT_BLTI : XT_BGEI, a, k);
        return 1;
    }
    if (!xt_bi_ok(XT_BLTUI, (long long)u))
        return 0;
    *out = test_i(pred == B_LT ? XT_BLTUI : XT_BGEUI, a, (long long)u);
    return 1;
}

/* A branch inside one lowering, patched later by br_land/br_back:
 * returns its offset. The target must be within the short reach. */
static int br_place(struct xt_fn *F, struct xtest x)
{
    int at = F->t->len;
    if (x.kind == T_TRUE) {
        xt_w(F->t, xt_enc_j(0));
        return at;
    }
    if (x.kind == T_FALSE)
        internal_error("xtensa: placing a branch that is never taken");
    xt_w(F->t, enc_test(&x, 0));
    return at;
}

static void br_land(struct xt_fn *F, int at)
{
    if (!xt_patch_branch(F->t, at, F->t->len))
        internal_error("xtensa: %s: a branch inside one operation does not "
                       "reach", F->fn->name);
}

static void br_back(struct xt_fn *F, int at, int target)
{
    if (!xt_patch_branch(F->t, at, target))
        internal_error("xtensa: %s: a loop inside one operation does not "
                       "reach", F->fn->name);
}

enum { FX_B, FX_J, FX_FAR };

static void want_label(struct xt_fn *F, int at, int label, int kind)
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

static int want_long(const struct xt_fn *F)
{
    return F->longb && F->nfix < F->nlongb ? F->longb[F->nfix] : 0;
}

static void lit_load(struct xt_fn *F, int reg, int kind, unsigned long v,
                     void *p);

/* A jump to a label beyond j's 128 KiB: its address from the pool, then
 * jx. ADDR holds nothing live across a branch (a test that read it has
 * already been evaluated). */
static void far_jump(struct xt_fn *F, int label)
{
    int at = F->t->len;
    lit_load(F, ADDR, LIT_LABEL, (unsigned long)label, NULL);
    xt_jx(F->t, ADDR);
    want_label(F, at, label, FX_FAR);
}

/* Branch to `label` when the test holds: the short form, or the inverse
 * test over a `j` when an earlier attempt found the short one does not
 * reach. */
static void branch_to(struct xt_fn *F, struct xtest x, int label)
{
    int at, lv = want_long(F);
    if (x.kind == T_FALSE)
        return;
    if (x.kind == T_TRUE) {
        if (lv == 2) {
            far_jump(F, label);
            return;
        }
        at = F->t->len;
        xt_w(F->t, xt_enc_j(0));
        want_label(F, at, label, FX_J);
        return;
    }
    if (lv == 2) {
        struct xtest inv = invert(x);
        /* over the l32r and the jx: 9 bytes on, 5 past the fourth */
        xt_w(F->t, enc_test(&inv, 5));
        far_jump(F, label);
        return;
    }
    if (lv) {
        struct xtest inv = invert(x);
        /* over the j: the branch is 3 bytes, the j 3, so 2 from the
         * instruction after the branch's own fourth byte */
        xt_w(F->t, enc_test(&inv, 2));
        at = F->t->len;
        xt_w(F->t, xt_enc_j(0));
        want_label(F, at, label, FX_J);
        return;
    }
    at = F->t->len;
    xt_w(F->t, enc_test(&x, 0));
    want_label(F, at, label, FX_B);
}

static void jump_to(struct xt_fn *F, int label)
{
    branch_to(F, test_const(1), label);
}

/* dst = the test's truth, 0 or 1. The branch reads its registers before
 * dst is written, unless dst is one of them: then the value is built in a
 * scratch the test does not read and moved. */
static void emit_bool(struct xt_fn *F, struct xtest x, int dst)
{
    struct code *t = F->t;
    int d = dst;
    if (x.kind == T_TRUE || x.kind == T_FALSE) {
        xt_movi(t, dst, x.kind == T_TRUE);
        return;
    }
    if (test_reads(&x, d)) {
        static const int cand[] = { ACC, TMP, SCR, ADDR };
        for (int k = 0; k < 4; k++)
            if (!test_reads(&x, cand[k])) {
                d = cand[k];
                break;
            }
    }
    xt_movi(t, d, 1);
    xt_w(t, enc_test(&x, 2));       /* over the movi d, 0 */
    xt_movi(t, d, 0);
    if (d != dst)
        xt_mov(t, dst, d);
}

/* ---- site lists --------------------------------------------------------------- */

static void note_ext(struct xt_sites *st, int at, struct func *callee)
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

static void note_str(struct xt_sites *st, int at, int idx, enum reloc_kind k)
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

static void note_glob(struct xt_sites *st, int at, struct global *g,
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

static void note_fn(struct xt_sites *st, int at, struct func *target,
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

/* A call: call8 with an R_XTENSA_SLOT0_OP against the callee, even within
 * the unit (the linker knows where both land). */
static void call_sym(struct xt_fn *F, struct func *callee)
{
    int at = F->t->len;
    xt_call(F->t, 2);
    note_ext(F->st, at, callee);
}

static struct func **g_helpers;
static int g_nhelpers, g_caphelpers;

static void call_helper(struct xt_fn *F, const char *name)
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

/* ---- soft float ----------------------------------------------------------------
 *
 * Every floating-point operation is a libgcc call (lib/rt/softfp.c), a
 * float in one register and a double in a pair; the operands go out in
 * a10.. and the result comes back in a10 (a10:a11). */
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

/* Put n vregs into the registers a helper (or a call) expects, all at
 * once: the register-to-register edges as one parallel move (ACC breaks
 * a cycle), then the loads, which only write. `half` (may be NULL) picks
 * word 0 or 1 of a 64-bit value. */
static void set_args_half(struct xt_fn *F, const int *dstreg,
                          const int *vreg, const int *half, int n)
{
    int pd[2 * MAX_PARAMS + 8], ps[2 * MAX_PARAMS + 8], npm = 0;

    for (int k = 0; k < n; k++)
        if (in_reg(F, vreg[k])) {
            pd[npm] = dstreg[k];
            ps[npm] = F->loc[vreg[k]] + (half ? half[k] : 0);
            npm++;
        }
    if (npm) {
        int od[2 * (2 * MAX_PARAMS + 8)], os[2 * (2 * MAX_PARAMS + 8)];
        int m = ra_parallel_move(pd, ps, npm, ACC, od, os,
                                 (int)(sizeof od / sizeof od[0]));
        if (m < 0)
            internal_error("xtensa: an argument setup is not a well-formed "
                           "move");
        for (int k = 0; k < m; k++)
            xt_mov(F->t, od[k], os[k]);
    }
    for (int k = 0; k < n; k++)
        if (!in_reg(F, vreg[k]))
            ld_sp(F, dstreg[k],
                  sslot(F, vreg[k]) + (half ? 4L * half[k] : 0), 4, 0);
}

static void set_args(struct xt_fn *F, const int *dstreg, const int *vreg,
                     int n)
{
    set_args_half(F, dstreg, vreg, NULL, n);
}

/* Two 64-bit operands into a10:a11 and a12:a13 (vb < 0: only the first). */
static void args64x2(struct xt_fn *F, int va, int vb)
{
    int d[4] = { XT_A10, XT_A11, XT_A12, XT_A13 };
    int v[4], h[4] = { 0, 1, 0, 1 };
    v[0] = v[1] = va;
    v[2] = v[3] = vb;
    set_args_half(F, d, v, h, vb >= 0 ? 4 : 2);
}

static void fp_args2(struct xt_fn *F, const struct ir_ins *i)
{
    if (i->w == 8) {
        args64x2(F, i->a, i->b);
        return;
    }
    {
        int dstreg[2], vreg[2];
        dstreg[0] = XT_A10; vreg[0] = i->a;
        dstreg[1] = XT_A11; vreg[1] = i->b;
        set_args(F, dstreg, vreg, 2);
    }
}

static void fp_result(struct xt_fn *F, int dst, int w)
{
    if (dst < 0)
        return;
    if (w == 8) wr64(F, dst, XT_A10, XT_A11);
    else        wr(F, dst, XT_A10);
}

/* ---- 64-bit integers, in register pairs ------------------------------------- */

/* Where a 64-bit operand's halves are: its pair, or the given scratches
 * after a load. And where to compute a 64-bit result: its pair, or A. */
static void src64(struct xt_fn *F, int v, int slo, int shi, int *lo, int *hi)
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

/* A 64-bit comparison into `dst`: the high words decide unless they are
 * equal, and then the low words, compared UNSIGNED. The branches read
 * every operand before dst is written, so dst may be any of them. */
static void cmp64(struct xt_fn *F, const struct ir_ins *i, int dst)
{
    struct code *t = F->t;
    int al, ah, bl, bh, hi_ne, to_true, to_true2, to_false, done;
    enum binop pred = i->pred;
    int sign = i->sign;
    src64(F, i->a, A_LO, A_HI, &al, &ah);
    if (i->imm_b) {
        operand_b64(F, i, B_LO, B_HI);
        bl = B_LO; bh = B_HI;
    } else {
        src64(F, i->b, B_LO, B_HI, &bl, &bh);
    }
    if (pred == B_EQ || pred == B_NE) {
        /* equal iff both halves are */
        int ne1 = br_place(F, test_rr(XT_BNE, al, bl));
        int ne2 = br_place(F, test_rr(XT_BNE, ah, bh));
        xt_movi(t, dst, pred == B_EQ);
        done = br_place(F, test_const(1));
        br_land(F, ne1);
        br_land(F, ne2);
        xt_movi(t, dst, pred != B_EQ);
        br_land(F, done);
        return;
    }
    hi_ne = br_place(F, test_rr(XT_BNE, ah, bh));
    to_true = br_place(F, pred_test(pred, 0, al, bl));     /* low: unsigned */
    to_false = br_place(F, test_const(1));
    br_land(F, hi_ne);
    to_true2 = br_place(F, pred_test(pred, sign, ah, bh));
    br_land(F, to_false);
    xt_movi(t, dst, 0);
    done = br_place(F, test_const(1));
    br_land(F, to_true);
    br_land(F, to_true2);
    xt_movi(t, dst, 1);
    br_land(F, done);
}

/* A shift by a constant from (al, ah) into (dl, dh) -- the same pair as
 * the source, or one sharing no register with it. */
static void shift64_imm_to(struct xt_fn *F, int left, int sign, long n,
                           int al, int ah, int dl, int dh)
{
    struct code *t = F->t;
    n &= 63;
    if (n == 0) {
        if (dl != al) xt_mov(t, dl, al);
        if (dh != ah) xt_mov(t, dh, ah);
        return;
    }
    if (n >= 32) {
        int k = (int)(n - 32);
        if (left) {
            if (k) xt_slli(t, dh, al, k);
            else if (dh != al) xt_mov(t, dh, al);
            xt_movi(t, dl, 0);
        } else {
            if (sign) {
                xt_srai(t, dl, ah, k);
                xt_srai(t, dh, ah, 31);
            } else {
                if (k) { xt_ssai(t, k); xt_srl(t, dl, ah); }
                else if (dl != ah) xt_mov(t, dl, ah);
                xt_movi(t, dh, 0);
            }
        }
        return;
    }
    if (left) {
        /* SAR = 32 - n: src gives (hi:lo) >> (32 - n) = hi << n | lo >> (32-n) */
        xt_ssai(t, (int)(32 - n));
        xt_alu(t, XT_SRC, dh, ah, al);
        xt_slli(t, dl, al, (int)n);
    } else {
        xt_ssai(t, (int)n);
        xt_alu(t, XT_SRC, dl, ah, al);
        if (sign) xt_sra(t, dh, ah);
        else      xt_srl(t, dh, ah);
    }
}

/* A shift of A_LO:A_HI by the count in B_LO. ssl/ssr take the count's
 * low five bits; bit 5 picks the arm where the halves move wholesale. */
static void shift64_var(struct xt_fn *F, int left, int sign)
{
    struct code *t = F->t;
    int big, done;
    if (left) xt_ssl(t, B_LO);
    else      xt_ssr(t, B_LO);
    big = br_place(F, test_bit(1, B_LO, 5));
    if (left) {
        xt_alu(t, XT_SRC, A_HI, A_HI, A_LO);
        xt_sll(t, A_LO, A_LO);
    } else {
        xt_alu(t, XT_SRC, A_LO, A_HI, A_LO);
        if (sign) xt_sra(t, A_HI, A_HI);
        else      xt_srl(t, A_HI, A_HI);
    }
    done = br_place(F, test_const(1));
    br_land(F, big);
    if (left) {
        xt_sll(t, A_HI, A_LO);
        xt_movi(t, A_LO, 0);
    } else if (sign) {
        xt_sra(t, A_LO, A_HI);
        xt_srai(t, A_HI, A_HI, 31);
    } else {
        xt_srl(t, A_LO, A_HI);
        xt_movi(t, A_HI, 0);
    }
    br_land(F, done);
}

/* d = s & c for a 32-bit constant: extui for a run of low bits, else the
 * constant in `tmp`. d may be s. */
static void and_imm(struct xt_fn *F, int d, int s, unsigned long c, int tmp)
{
    c &= 0xffffffffUL;
    if (c == 0xffffffffUL) {
        if (d != s) xt_mov(F->t, d, s);
        return;
    }
    if (c == 0) {
        xt_movi(F->t, d, 0);
        return;
    }
    if ((c & (c + 1)) == 0) {                /* the low k bits */
        int k = 0;
        while (c >> k & 1) k++;
        if (k <= 16) {
            xt_extui(F->t, d, s, 0, k);
            return;
        }
    }
    li(F, tmp, (long long)(int)(unsigned int)c);
    xt_alu(F->t, XT_AND, d, s, tmp);
}

/* d = s OP c for one half of a 64-bit logic op with a constant. */
static void logic_half(struct xt_fn *F, int op, int d, int s, unsigned long c,
                       int tmp)
{
    c &= 0xffffffffUL;
    if (op == XT_AND) {
        and_imm(F, d, s, c, tmp);
        return;
    }
    if (c == 0) {
        if (d != s) xt_mov(F->t, d, s);
        return;
    }
    li(F, tmp, (long long)(int)(unsigned int)c);
    xt_alu(F->t, op, d, s, tmp);
}

/* Every 64-bit operation that is not a call. Returns 0 for one this does
 * not handle, which the caller refuses by name. */
static int gen_ins64(struct xt_fn *F, int n)
{
    struct ir_func *fn = F->fn;
    struct ir_ins *i = &fn->ins[n];
    struct code *t = F->t;

    switch (i->op) {
    case IR_CONST:
        li(F, A_LO, (long long)(i->imm & 0xffffffffL));
        li(F, A_HI, (long long)((i->imm >> 32) & 0xffffffffL));
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
    case IR_ADD: case IR_SUB: {
        /* No carry flag: the carry out of the low words is the sum being
         * below an addend (unsigned), the borrow the minuend being below
         * the subtrahend -- each a branch over the +-1. */
        int skip;
        rd64(F, i->a, A_LO, A_HI);
        operand_b64(F, i, B_LO, B_HI);
        if (i->op == IR_ADD) {
            xt_alu(t, XT_ADD, A_LO, A_LO, B_LO);
            xt_alu(t, XT_ADD, A_HI, A_HI, B_HI);
            skip = br_place(F, test_rr(XT_BGEU, A_LO, B_LO));
            xt_addi(t, A_HI, A_HI, 1);
        } else {
            xt_alu(t, XT_SUB, A_HI, A_HI, B_HI);
            skip = br_place(F, test_rr(XT_BGEU, A_LO, B_LO));
            xt_addi(t, A_HI, A_HI, -1);
            br_land(F, skip);
            xt_alu(t, XT_SUB, A_LO, A_LO, B_LO);
            wr64(F, i->dst, A_LO, A_HI);
            return 1;
        }
        br_land(F, skip);
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    }
    case IR_AND: case IR_OR: case IR_XOR: {
        int op = i->op == IR_AND ? XT_AND : i->op == IR_OR ? XT_OR : XT_XOR;
        int al, ah, bl, bh;
        src64(F, i->a, A_LO, A_HI, &al, &ah);
        if (i->imm_b) {
            logic_half(F, op, A_LO, al, (unsigned long)i->imm, B_LO);
            logic_half(F, op, A_HI, ah, (unsigned long)i->imm >> 32, B_LO);
            wr64(F, i->dst, A_LO, A_HI);
            return 1;
        }
        src64(F, i->b, B_LO, B_HI, &bl, &bh);
        xt_alu(t, op, A_LO, al, bl);
        xt_alu(t, op, A_HI, ah, bh);
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    }
    case IR_NEG: {
        /* 0 - x: the low word negated, the high word negated less a
         * borrow when the low word was not 0 */
        int al, ah, skip;
        src64(F, i->a, A_LO, A_HI, &al, &ah);
        xt_neg(t, A_HI, ah);
        xt_neg(t, A_LO, al);
        skip = br_place(F, test_z(XT_BEQZ, A_LO));
        xt_addi(t, A_HI, A_HI, -1);
        br_land(F, skip);
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    }
    case IR_BNOT: {
        /* ~x = -x - 1, a word at a time */
        int al, ah;
        src64(F, i->a, A_LO, A_HI, &al, &ah);
        xt_neg(t, A_LO, al);
        xt_addi(t, A_LO, A_LO, -1);
        xt_neg(t, A_HI, ah);
        xt_addi(t, A_HI, A_HI, -1);
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    }
    case IR_SHL: case IR_SHR: {
        int sign = i->op == IR_SHR && i->sign;
        if (i->imm_b) {
            int al, ah;
            src64(F, i->a, A_LO, A_HI, &al, &ah);
            if (al != A_LO && (al == A_HI || ah == A_LO)) {
                rd64(F, i->a, A_LO, A_HI);
                al = A_LO; ah = A_HI;
            }
            shift64_imm_to(F, i->op == IR_SHL, sign, (long)i->imm,
                           al, ah, A_LO, A_HI);
            wr64(F, i->dst, A_LO, A_HI);
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
        if (i->sign) xt_srai(t, A_HI, A_LO, 31);
        else         xt_movi(t, A_HI, 0);
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
            if (i->sign) xt_srai(t, A_HI, A_LO, 31);
            else         xt_movi(t, A_HI, 0);
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
            st_off(F, A_LO, F->fb, sslot(F, i->dst), i->size, ADDR);
        return 1;
    case IR_LOAD: {
        int addr = rdr(F, i->a, ADDR);
        if (i->vol)
            xt_memw(t);
        if (i->size == 8) {
            ld_any(F, A_LO, addr, i->memoff, 4, 0, i->natural, SCR, -1);
            ld_any(F, A_HI, addr, i->memoff + 4, 4, 0, i->natural, SCR, -1);
        } else {
            ld_any(F, A_LO, addr, i->memoff, i->size, i->sign, i->natural,
                   SCR, -1);
            if (i->sign) xt_srai(t, A_HI, A_LO, 31);
            else         xt_movi(t, A_HI, 0);
        }
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    }
    case IR_STORE: {
        int addr = rdr(F, i->a, ADDR);
        rd64(F, i->b, A_LO, A_HI);
        if (i->vol)
            xt_memw(t);
        st_any(F, A_LO, addr, i->memoff, i->size == 8 ? 4 : i->size,
               i->natural, SCR);
        if (i->size == 8)
            st_any(F, A_HI, addr, i->memoff + 4, 4, i->natural, SCR);
        return 1;
    }
    case IR_SELECT: {
        /* dst = a ? b : c: c into A, then b over it with movnez when the
         * condition (either half of a 64-bit one) is nonzero */
        int bl, bh, cond;
        if (i->size == 8) {
            int al, ah;
            src64(F, i->a, B_LO, B_HI, &al, &ah);
            xt_alu(t, XT_OR, SCR, al, ah);
            cond = SCR;
        } else {
            cond = rdr(F, i->a, SCR);
        }
        rd64(F, i->c, A_LO, A_HI);
        if (in_reg(F, i->b)) {
            bl = F->loc[i->b];
            bh = bl + 1;
        } else {
            /* B_LO is SCR: keep the condition out of it */
            if (cond == SCR) {
                xt_mov(t, ADDR, SCR);
                cond = ADDR;
            }
            if (cond == ADDR) {
                ld_sp(F, B_LO, sslot(F, i->b), 4, 0);
                xt_alu(t, XT_MOVNEZ, A_LO, B_LO, cond);
                ld_sp(F, B_LO, sslot(F, i->b) + 4, 4, 0);
                xt_alu(t, XT_MOVNEZ, A_HI, B_LO, cond);
                wr64(F, i->dst, A_LO, A_HI);
                return 1;
            }
            rd64(F, i->b, B_LO, B_HI);
            bl = B_LO; bh = B_HI;
        }
        xt_alu(t, XT_MOVNEZ, A_LO, bl, cond);
        xt_alu(t, XT_MOVNEZ, A_HI, bh, cond);
        wr64(F, i->dst, A_LO, A_HI);
        return 1;
    }
    default:
        return 0;
    }
}

/* ---- one call ------------------------------------------------------------------- */

/* Word q of a composite at [base] into register r: one l32i when the
 * composite is 4-aligned at that address (irgen's ir_arg.natural, or the
 * IR_RET's natural: a packed structure's member need not be), else its
 * bytes from the highest down, through
 * the scratch `by`. `base` is the caller's scratch, and is moved on (by
 * *adv bytes so far) when an offset passes the fields' reach. */
static void comp_word(struct xt_fn *F, int r, int base, long size, int align,
                      int q, int by, long *adv)
{
    struct code *t = F->t;
    long off = 4L * q - *adv, left = size - 4L * q;
    if (left > 4)
        left = 4;
    if (off + 3 > 255) {
        addr_off(F, base, base, off);
        *adv += off;
        off = 0;
    }
    if (left == 4 && align >= 4) {
        xt_load(t, r, base, off, 4, 0);
        return;
    }
    xt_load(t, r, base, off + left - 1, 1, 0);
    for (long b = left - 2; b >= 0; b--) {
        xt_load(t, by, base, off + b, 1, 0);
        xt_slli(t, r, r, 8);
        xt_alu(t, XT_OR, r, r, by);
    }
}

static void gen_call(struct xt_fn *F, int n)
{
    struct ir_func *fn = F->fn;
    struct ir_ins *i = &fn->ins[n];
    struct code *t = F->t;
    struct xplace pl[MAX_PARAMS];
    int words = 0;
    long stk = 0;
    int sret = call_sret(i);
    int rw = i->retsize ? ret_words(1, i->retsize) : 0;

    if (sret)
        words = 1;                         /* a10 holds the result's address */
    for (int k = 0; k < i->nargs; k++)
        place_arg(&i->argv[k], &words, &stk, &pl[k]);

    /* The STACK words first, through SCR and ADDR (a far outgoing offset
     * uses TMP): the argument registers are written after. */
    for (int k = 0; k < i->nargs; k++) {
        struct ir_arg *a = &i->argv[k];
        for (int p = 0; p < pl[k].np; p++) {
            struct xpiece *pc = &pl[k].p[p];
            if (pc->reg >= 0)
                continue;
            if (a->is_struct) {
                long base_w = (long)p * pl[k].esz / 4, adv = 0;
                rd(F, a->vreg, ADDR);
                for (int q = 0; q < pc->nw; q++) {
                    comp_word(F, SCR, ADDR, a->size,
                              a->natural && a->align ? a->align : 1,
                              (int)(base_w + q), ACC,
                              &adv);
                    st_out(F, SCR, pc->stk + 4L * q, 4, TMP);
                }
            } else if (a->size > 4) {
                rd64(F, a->vreg, SCR, ADDR);
                st_out(F, SCR, pc->stk, 4, TMP);
                st_out(F, ADDR, pc->stk + 4, 4, TMP);
            } else {
                rd(F, a->vreg, SCR);
                if (a->size < 4)
                    ext_reg(F, SCR, SCR, a->size,
                            a->ty ? ty_signed_int(a->ty) : 1);
                st_out(F, SCR, pc->stk, 4, TMP);
            }
        }
    }
    /* The scalar register arguments, all at once: one parallel move (a
     * 64-bit value in a pair is two edges of it). */
    {
        int sd_[2 * MAX_PARAMS], sv_[2 * MAX_PARAMS], sh_[2 * MAX_PARAMS];
        int ns_ = 0;
        for (int k = 0; k < i->nargs; k++) {
            struct ir_arg *a = &i->argv[k];
            struct xpiece *pc = &pl[k].p[0];
            if (a->is_struct || pc->reg < 0)
                continue;
            for (int q = 0; q < pc->nw; q++) {
                sd_[ns_] = OUT_ARG(pc->reg + q);
                sv_[ns_] = a->vreg;
                sh_[ns_] = a->size > 4 ? q : 0;
                ns_++;
            }
        }
        if (ns_)
            set_args_half(F, sd_, sv_, sh_, ns_);
        /* narrower than a word: extended, as GCC's callers do */
        for (int k = 0; k < i->nargs; k++) {
            struct ir_arg *a = &i->argv[k];
            struct xpiece *pc = &pl[k].p[0];
            if (a->is_struct || pc->reg < 0 || a->size >= 4)
                continue;
            ext_reg(F, OUT_ARG(pc->reg), OUT_ARG(pc->reg), a->size,
                    a->ty ? ty_signed_int(a->ty) : 1);
        }
    }
    /* The composites' register words, from their addresses (in memory:
     * the allocator keeps a struct argument's address there), through ACC
     * as the address and TMP as the byte scratch -- neither is an
     * argument register. */
    for (int k = 0; k < i->nargs; k++) {
        struct ir_arg *a = &i->argv[k];
        if (!a->is_struct)
            continue;
        for (int p = 0; p < pl[k].np; p++) {
            struct xpiece *pc = &pl[k].p[p];
            long base_w = (long)p * pl[k].esz / 4, adv = 0;
            if (pc->reg < 0)
                continue;
            rd(F, a->vreg, ACC);
            for (int q = 0; q < pc->nw; q++)
                comp_word(F, OUT_ARG(pc->reg + q), ACC, a->size,
                          a->natural && a->align ? a->align : 1,
                              (int)(base_w + q), TMP,
                          &adv);
        }
    }
    if (sret)
        addr_sp(F, XT_A10, F->scratch_at + i->scratch);

    if (i->indirect) {
        /* the target is in memory (regalloc keeps it there), so reading it
         * now disturbs no argument */
        rd(F, i->a, ACC);
        xt_callx(t, 2, ACC);
    } else {
        call_sym(F, i->callee);
    }

    if (i->dst < 0)
        return;
    if (i->retsize) {
        /* dst receives the scratch's ADDRESS. A composite that came back
         * in a10-a13 is stored there first; a larger one the callee wrote
         * through the hidden pointer. */
        long at = F->scratch_at + i->scratch;
        for (int q = 0; q < rw; q++) {
            long left = i->retsize - 4L * q;
            if (left >= 4) {
                st_sp(F, OUT_ARG(q), at + 4L * q, 4);
            } else {
                /* the last partial word, a byte at a time */
                for (long b = 0; b < left; b++) {
                    if (b) xt_srli(t, OUT_ARG(q), OUT_ARG(q), 8);
                    st_sp(F, OUT_ARG(q), at + 4L * q + b, 1);
                }
            }
        }
        addr_sp(F, ACC, at);
        wr(F, i->dst, ACC);
    } else if (F->wide[i->dst]) {
        wr64(F, i->dst, XT_A10, XT_A11);
    } else {
        wr(F, i->dst, XT_A10);
    }
}

/* ---- one instruction ------------------------------------------------------------ */

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

static void need_word_atomic(struct xt_fn *F, const struct ir_ins *i)
{
    if (i->size == 1 || i->size == 2)   /* sub_* below: the word around it */
        return;
    if (i->size != 4)
        xt_refuse(F, i, "an atomic wider than a register");
}

/* ---- one- and two-byte atomics ---------------------------------------
 *
 * s32c1i is word-sized, so a narrow atomic works on the aligned word
 * around it, as GCC's and LLVM's do: an s32c1i loop that rewrites only its
 * lane,
 *
 *   retry: l32i   old, aligned, 0
 *          wsr    old, scompare1
 *          new = f(old)  in the lane
 *          new = old ^ ((new ^ old) & mask)
 *          s32c1i new, aligned, 0          (new = what memory held)
 *          bne    new, old, retry
 *
 * atomic against the neighbouring bytes too: a store to any of them
 * between the load and the s32c1i makes it fail, and the loop goes round.
 * Xtensa is little-endian here: the lane of address a is bits 8*(a & 3)
 * up. The scratches are four, one short of what such a loop holds, so the
 * lane's shift is kept in SAR (ssl: sll then shifts left by it) and the
 * operands are read from their homes and shifted into the lane on every
 * trip; the merge masks them, so they need no mask of their own. */
#define SUB_AL  ADDR        /* a15: the aligned word's address */
#define SUB_MK  SCR         /* a14: the lane's mask */
#define SUB_OLD ACC         /* a8:  the word seen */
#define SUB_NEW TMP         /* a9:  the operand, then the word to store */

/* SUB_AL, SUB_MK, and SAR for sll into the lane, for the address in a */
static void sub_lane(struct xt_fn *F, int av, int size)
{
    struct code *t = F->t;
    int a = rdr(F, av, SUB_AL);
    xt_extui(t, SUB_NEW, a, 0, 2);
    xt_slli(t, SUB_NEW, SUB_NEW, 3);
    xt_ssl(t, SUB_NEW);
    xt_srli(t, SUB_AL, a, 2);
    xt_slli(t, SUB_AL, SUB_AL, 2);
    xt_movi(t, SUB_MK, -1);
    xt_extui(t, SUB_MK, SUB_MK, 0, 8 * size);
    xt_sll(t, SUB_MK, SUB_MK);
}

/* SUB_NEW = vreg v shifted into the lane (SAR as sub_lane left it) */
static void sub_in(struct xt_fn *F, int v)
{
    xt_sll(F->t, SUB_NEW, rdr(F, v, SUB_NEW));
}

/* SUB_NEW = the byte or halfword at the pointer in vreg p, in the lane */
static void sub_in_mem(struct xt_fn *F, int p, int size)
{
    xt_load(F->t, SUB_NEW, rdr(F, p, SUB_NEW), 0, size, 0);
    xt_sll(F->t, SUB_NEW, SUB_NEW);
}

/* SUB_NEW = old ^ ((SUB_NEW ^ old) & mask) */
static void sub_merge(struct xt_fn *F)
{
    xt_alu(F->t, XT_XOR, SUB_NEW, SUB_NEW, SUB_OLD);
    xt_alu(F->t, XT_AND, SUB_NEW, SUB_NEW, SUB_MK);
    xt_alu(F->t, XT_XOR, SUB_NEW, SUB_NEW, SUB_OLD);
}

/* SUB_OLD's lane down to bit 0, extended as `sign` says: the shift from
 * vreg a's address again (ssa8l: srl then shifts right by 8 * (a & 3)) */
static void sub_out(struct xt_fn *F, int av, int size, int sign)
{
    struct code *t = F->t;
    xt_alu(t, XT_AND, SUB_OLD, SUB_OLD, SUB_MK);
    xt_ssa8l(t, rdr(F, av, SUB_NEW));
    xt_srl(t, SUB_OLD, SUB_OLD);
    if (sign)
        xt_sext(t, SUB_OLD, SUB_OLD, 8 * size - 1);
}

/* swap, fetch-and-add and the bitwise ones, on a byte or a halfword */
static void sub_rmw(struct xt_fn *F, const struct ir_ins *i)
{
    struct code *t = F->t;
    int top, again;
    sub_lane(F, i->a, i->size);
    xt_memw(t);
    top = t->len;
    xt_load(t, SUB_OLD, SUB_AL, 0, 4, 0);
    xt_wsr(t, SUB_OLD, XT_SR_SCOMPARE1);
    sub_in(F, i->b);
    if (i->op == IR_XADD) {
        xt_alu(t, XT_ADD, SUB_NEW, SUB_OLD, SUB_NEW);
    } else if (i->op == IR_ARMW) {
        switch ((int)i->imm) {
        case '&': xt_alu(t, XT_AND, SUB_NEW, SUB_OLD, SUB_NEW); break;
        case '|': xt_alu(t, XT_OR, SUB_NEW, SUB_OLD, SUB_NEW); break;
        case '^': xt_alu(t, XT_XOR, SUB_NEW, SUB_OLD, SUB_NEW); break;
        default:                                        /* nand */
            xt_alu(t, XT_AND, SUB_NEW, SUB_OLD, SUB_NEW);
            xt_neg(t, SUB_NEW, SUB_NEW);
            xt_addi(t, SUB_NEW, SUB_NEW, -1);
            break;
        }
    }                                       /* (IR_XCHG: the operand) */
    sub_merge(F);
    xt_s32c1i(t, SUB_NEW, SUB_AL, 0);
    again = br_place(F, test_rr(XT_BNE, SUB_NEW, SUB_OLD));
    br_back(F, again, top);
    xt_memw(t);
    sub_out(F, i->a, i->size, i->sign);
    wrote(F, i->dst, SUB_OLD);
}

/* compare-and-swap on a byte or a halfword: the lane is compared, not the
 * word, so a neighbour's change only goes round:
 *
 *          l32i   old, aligned, 0
 *   retry: (expected, in the lane) ^ old ; bany it, mask, out
 *          wsr    old, scompare1
 *          new = old ^ (((desired, in the lane) ^ old) & mask)
 *          s32c1i new, aligned, 0
 *          beq    new, old, out
 *          mov    old, new ; j retry
 *   out:
 * old is then the word seen, whichever way the loop ended. */
static void sub_cas(struct xt_fn *F, const struct ir_ins *i)
{
    struct code *t = F->t;
    int top, again, failed, done;
    sub_lane(F, i->a, i->size);
    xt_memw(t);
    xt_load(t, SUB_OLD, SUB_AL, 0, 4, 0);
    top = t->len;
    if (i->op == IR_CAS)
        sub_in(F, i->b);
    else
        sub_in_mem(F, i->b, i->size);
    xt_alu(t, XT_XOR, SUB_NEW, SUB_NEW, SUB_OLD);
    failed = br_place(F, test_rr(XT_BANY, SUB_NEW, SUB_MK));
    xt_wsr(t, SUB_OLD, XT_SR_SCOMPARE1);
    sub_in(F, i->c);
    sub_merge(F);
    xt_s32c1i(t, SUB_NEW, SUB_AL, 0);
    done = br_place(F, test_rr(XT_BEQ, SUB_NEW, SUB_OLD));
    xt_mov(t, SUB_OLD, SUB_NEW);
    again = br_place(F, test_const(1));
    br_back(F, again, top);
    br_land(F, failed);
    br_land(F, done);
    xt_memw(t);
    if (i->op == IR_CAS) {
        sub_out(F, i->a, i->size, i->sign);
        wr(F, i->dst, SUB_OLD);
    } else {
        /* the flag from the lanes compared (SAR still shifts left); then
         * *b = the lane seen */
        sub_in_mem(F, i->b, i->size);
        xt_alu(t, XT_XOR, SUB_NEW, SUB_NEW, SUB_OLD);
        xt_alu(t, XT_AND, SUB_NEW, SUB_NEW, SUB_MK);
        emit_bool(F, test_z(XT_BEQZ, SUB_NEW), SUB_AL);
        sub_out(F, i->a, i->size, 0);
        xt_store(t, SUB_OLD, rdr(F, i->b, SUB_NEW), 0, i->size);
        wr(F, i->dst, SUB_AL);
    }
}

/* Copy `size` bytes from [TMP] to [ADDR] (copy) or zero them (!copy):
 * words when both ends are known to be word-aligned, else bytes;
 * straight-line up to 128 bytes, a loop beyond. TMP and ADDR are scratch
 * and may be moved; SCR carries the data, ACC the loop's end. */
static void copy_block(struct xt_fn *F, int copy, long size, int aligned)
{
    struct code *t = F->t;
    int unit = aligned ? 4 : 1;
    long body = size & ~(long)(unit - 1), k;
    if (!copy)
        xt_movi(t, SCR, 0);
    if (size > 128) {
        int top, again;
        addr_off(F, ACC, ADDR, body);
        top = t->len;
        if (copy)
            xt_load(t, SCR, TMP, 0, unit, 0);
        xt_store(t, SCR, ADDR, 0, unit);
        if (copy)
            xt_addi(t, TMP, TMP, unit);
        xt_addi(t, ADDR, ADDR, unit);
        again = br_place(F, test_rr(XT_BNE, ADDR, ACC));
        br_back(F, again, top);
        k = 0;
        size -= body;
    } else {
        for (k = 0; k + unit <= size; k += unit) {
            if (k > (unit == 4 ? 1020 : 255)) {
                /* past the field's reach: move both pointers on */
                if (copy)
                    xt_addi_any(t, TMP, TMP, k);
                xt_addi_any(t, ADDR, ADDR, k);
                size -= k;
                k = 0;
            }
            if (copy)
                xt_load(t, SCR, TMP, k, unit, 0);
            xt_store(t, SCR, ADDR, k, unit);
        }
    }
    for (; k < size; k++) {
        if (copy) xt_load(t, SCR, TMP, k, 1, 0);
        xt_store(t, SCR, ADDR, k, 1);
    }
}

/* The 0x80000000 sign bit, for a float's negation. */
static void sign_bit(struct xt_fn *F, int reg)
{
    xt_movi(F->t, reg, 1);
    xt_slli(F->t, reg, reg, 31);
}

static void gen_ins(struct xt_fn *F, int n)
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

    /* Floating point is a call, not an instruction. */
    if (i->flt && (i->op == IR_ADD || i->op == IR_SUB || i->op == IR_MUL ||
                   i->op == IR_DIV || i->op == IR_MOD || i->op == IR_NEG ||
                   i->op == IR_CMP || i->op == IR_SQRT)) {
        const char *name = fp_binop_name(i->op, i->w);
        if (i->w != 4 && i->w != 8)
            xt_refuse(F, i, "a floating-point value of this width");
        if (name) {
            if (i->imm_b)
                xt_refuse(F, i, "a folded floating-point immediate");
            fp_args2(F, i);
            call_helper(F, name);
            fp_result(F, i->dst, i->w);
            return;
        }
        if (i->op == IR_NEG) {
            /* the sign bit, flipped: right for -0.0 and a NaN as well */
            if (i->w == 8) {
                rd64(F, i->a, A_LO, A_HI);
                sign_bit(F, B_LO);
                xt_alu(t, XT_XOR, A_HI, A_HI, B_LO);
                wr64(F, i->dst, A_LO, A_HI);
            } else {
                rd(F, i->a, ACC);
                sign_bit(F, TMP);
                xt_alu(t, XT_XOR, ACC, ACC, TMP);
                wr(F, i->dst, ACC);
            }
            return;
        }
        if (i->op == IR_CMP) {
            /* __ltdf2 and the rest answer with an int whose relation to
             * zero is the predicate's; unordered makes it false */
            int d;
            fp_args2(F, i);
            call_helper(F, fp_cmp_name(i->pred, i->w));
            d = wreg(F, i->dst, ACC);
            emit_bool(F, pred_test(i->pred, 1, XT_A10, ZERO), d);
            wrote(F, i->dst, d);
            return;
        }
        if (i->op == IR_SQRT)
            xt_refuse(F, i, "__builtin_sqrt (a libm routine here, not an "
                            "instruction)");
        xt_refuse(F, i, "this floating-point operation");
    }

    if (i->w > 8)
        xt_refuse(F, i, "a 128-bit value");
    if (i->op == IR_XCHG || i->op == IR_XADD || i->op == IR_ARMW ||
        i->op == IR_CAS || i->op == IR_CMPXCHG)
        need_word_atomic(F, i);
    /* Level 0 only (irgen): the windowed ABI keeps a caller's frame in
     * its register window, not in a chain. The frame address is a1 as
     * the function was entered (frame base + frame: entry, and movsp for
     * a large frame, take exactly `frame` off it). The return address is
     * a0, which the windowed code never reuses -- but its top two bits
     * are the caller's window increment, not address bits; as GCC does,
     * they are replaced by those of this function's own address, the
     * 1 GiB region the call came from (an ESP32's IRAM is 0x4008xxxx, so
     * they are not zero). */
    if (i->op == IR_FRAMEADDR) {
        int d = i->dst >= 0 ? wreg(F, i->dst, ACC) : ACC;
        if (i->imm == 2) {
            xt_slli(F->t, d, XT_A0, 2);          /* a0's low 30 bits */
            xt_srli(F->t, d, d, 2);
            lit_load(F, TMP, LIT_FUNC, 0, F->fn->src);
            xt_extui(F->t, TMP, TMP, 30, 2);    /* the region's two */
            xt_slli(F->t, TMP, TMP, 30);
            xt_alu(F->t, XT_OR, d, d, TMP);
        } else {
            addr_sp(F, d, F->frame);
        }
        if (i->dst >= 0)
            wrote(F, i->dst, d);
        return;
    }

    /* The high word of a 64-bit value, shifted: one register. */
    if (i->op == IR_SHR && F->nshr && i->dst >= 0 && F->nshr[i->dst]) {
        int k = (int)i->imm - 32, d = wreg(F, i->dst, A_LO), hi;
        if (in_reg(F, i->a)) {
            hi = F->loc[i->a] + 1;
        } else {
            ld_sp(F, A_HI, sslot(F, i->a) + 4, 4, 0);
            hi = A_HI;
        }
        if (i->sign)
            xt_srai(t, d, hi, k);
        else if (k == 0) {
            if (d != hi) xt_mov(t, d, hi);
        } else if (k < 16)
            xt_srli(t, d, hi, k);
        else
            xt_extui(t, d, hi, k, 32 - k);
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
            if (i->op == IR_DIV || i->op == IR_MOD || i->op == IR_MUL) {
                /* lib/rt, under libgcc's names: the operands in a10:a11
                 * and a12:a13, the result in a10:a11 */
                if (i->imm_b) {
                    args64x2(F, i->a, -1);
                    operand_b64(F, i, XT_A12, XT_A13);
                } else {
                    args64x2(F, i->a, i->b);
                }
                call_helper(F, i->op == IR_MUL ? "__muldi3"
                            : i->op == IR_DIV
                            ? (i->sign ? "__divdi3" : "__udivdi3")
                            : (i->sign ? "__moddi3" : "__umoddi3"));
                wr64(F, i->dst, XT_A10, XT_A11);
                return;
            }
            if (gen_ins64(F, n))
                return;
            xt_refuse(F, i, "this operation at 64 bits");
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
        li(F, d, imm_val(i));
        wrote(F, i->dst, d);
        return;
    }
    case IR_BITCAST:
    case IR_MOV: {
        int src = rdr(F, i->a, ACC);
        int d = wreg(F, i->dst, ACC);
        if (src != d)
            xt_mov(t, d, src);
        wrote(F, i->dst, d);
        return;
    }

    case IR_ADD: case IR_SUB: case IR_MUL:
    case IR_AND: case IR_OR:  case IR_XOR: {
        int op = i->op == IR_ADD ? XT_ADD
               : i->op == IR_SUB ? XT_SUB
               : i->op == IR_AND ? XT_AND
               : i->op == IR_OR  ? XT_OR
               : i->op == IR_XOR ? XT_XOR
               : XT_MULL;
        int ra_ = rdr(F, i->a, ACC);
        int rd_ = wreg(F, i->dst, ACC);
        if (i->imm_b) {
            long long v = imm_val(i);
            if (i->op == IR_ADD || i->op == IR_SUB) {
                if (i->op == IR_SUB) v = -v;
                if (xt_addi_any_len(v) && xt_addi_any_len(v) <= 6) {
                    xt_addi_any(t, rd_, ra_, v);
                    wrote(F, i->dst, rd_);
                    return;
                }
                li(F, TMP, v);
                xt_alu(t, XT_ADD, rd_, ra_, TMP);
                wrote(F, i->dst, rd_);
                return;
            }
            if (i->op == IR_AND) {
                and_imm(F, rd_, ra_, (unsigned long)v, TMP);
                wrote(F, i->dst, rd_);
                return;
            }
        }
        {
            int rb_ = (i->imm_b || !in_reg(F, i->b)) ? TMP : F->loc[i->b];
            if (rb_ == TMP) operand_b(F, i, TMP);
            xt_alu(t, op, rd_, ra_, rb_);
        }
        wrote(F, i->dst, rd_);
        return;
    }
    case IR_DIV: case IR_MOD: {
        /* quos/quou/rems/remu: DIV32. A zero divisor raises the
         * IntegerDivideByZero exception, which C leaves undefined. */
        int ra_ = rdr(F, i->a, ACC);
        int rb_ = (i->imm_b || !in_reg(F, i->b)) ? TMP : F->loc[i->b];
        int d = wreg(F, i->dst, ACC);
        if (rb_ == TMP) operand_b(F, i, TMP);
        xt_alu(t, i->op == IR_DIV ? (i->sign ? XT_QUOS : XT_QUOU)
                                  : (i->sign ? XT_REMS : XT_REMU),
               d, ra_, rb_);
        wrote(F, i->dst, d);
        return;
    }
    case IR_SHL: case IR_SHR: {
        int ra_ = rdr(F, i->a, ACC);
        int d = wreg(F, i->dst, ACC);
        if (i->imm_b) {
            int k = (int)(i->imm & 31);
            if (i->op == IR_SHL) {
                if (k) xt_slli(t, d, ra_, k);
                else if (d != ra_) xt_mov(t, d, ra_);
            } else if (i->sign) {
                xt_srai(t, d, ra_, k);
            } else if (k < 16) {
                xt_srli(t, d, ra_, k);
            } else {
                xt_extui(t, d, ra_, k, 32 - k);
            }
        } else {
            int rb_ = rdr(F, i->b, TMP);
            if (i->op == IR_SHL) {
                xt_ssl(t, rb_);
                xt_sll(t, d, ra_);
            } else {
                xt_ssr(t, rb_);
                if (i->sign) xt_sra(t, d, ra_);
                else         xt_srl(t, d, ra_);
            }
        }
        wrote(F, i->dst, d);
        return;
    }
    case IR_NEG: {
        int ra_ = rdr(F, i->a, ACC);
        int d = wreg(F, i->dst, ACC);
        xt_neg(t, d, ra_);
        wrote(F, i->dst, d);
        return;
    }
    case IR_BNOT: {
        /* ~x is -x - 1 */
        int ra_ = rdr(F, i->a, ACC);
        int d = wreg(F, i->dst, ACC);
        xt_neg(t, d, ra_);
        xt_addi(t, d, d, -1);
        wrote(F, i->dst, d);
        return;
    }

    case IR_CMP: {
        struct ir_ins *nx = n + 1 < fn->nins ? &fn->ins[n + 1]
                                             : (struct ir_ins *)0;
        int fuse = nx && (nx->op == IR_BRZ || nx->op == IR_BRNZ) &&
                   nx->a == i->dst && F->usecnt && F->usecnt[i->dst] == 1;
        if (i->w == 8) {
            int d;
            if (fuse && i->imm_b && i->imm == 0 &&
                (i->pred == B_EQ || i->pred == B_NE ||
                 (i->sign && (i->pred == B_LT || i->pred == B_GE)))) {
                /* against zero: an or of the halves, or the high word's
                 * sign */
                int al, ah;
                struct xtest x;
                src64(F, i->a, A_LO, A_HI, &al, &ah);
                if (i->pred == B_EQ || i->pred == B_NE) {
                    xt_alu(t, XT_OR, SCR, al, ah);
                    x = test_z(i->pred == B_EQ ? XT_BEQZ : XT_BNEZ, SCR);
                } else {
                    x = test_z(i->pred == B_LT ? XT_BLTZ : XT_BGEZ, ah);
                }
                if (nx->op == IR_BRZ)
                    x = invert(x);
                branch_to(F, x, nx->label);
                F->skip_next = 1;
                return;
            }
            d = wreg(F, i->dst, ACC);
            cmp64(F, i, d);
            wrote(F, i->dst, d);
            return;
        }
        {
            int ra_ = rdr(F, i->a, ACC);
            struct xtest x;
            if (!i->imm_b || !pred_test_imm(i->pred, i->sign, ra_,
                                            imm_val(i), &x)) {
                int rb_ = (i->imm_b || !in_reg(F, i->b)) ? TMP
                                                          : F->loc[i->b];
                if (rb_ == TMP) operand_b(F, i, TMP);
                x = pred_test(i->pred, i->sign, ra_, rb_);
            }
            if (fuse && nx->w != 8) {
                if (nx->op == IR_BRZ)
                    x = invert(x);
                branch_to(F, x, nx->label);
                F->skip_next = 1;
                return;
            }
            {
                int d = wreg(F, i->dst, ACC);
                emit_bool(F, x, d);
                wrote(F, i->dst, d);
            }
        }
        return;
    }

    case IR_SELECT: {
        /* dst = a ? b : c, with movnez: c, then b over it when the
         * condition -- tested at ITS width, `size` -- is nonzero */
        int cond, rb_, rc_, d;
        if (i->size == 8) {
            int al, ah;
            src64(F, i->a, SCR, ADDR, &al, &ah);
            xt_alu(t, XT_OR, SCR, al, ah);
            cond = SCR;
        } else {
            cond = rdr(F, i->a, SCR);
        }
        rb_ = rdr(F, i->b, TMP);
        rc_ = rdr(F, i->c, ACC);
        d = wreg(F, i->dst, ACC);
        if (d == cond || d == rb_)
            d = ACC;
        if (d != rc_)
            xt_mov(t, d, rc_);
        xt_alu(t, XT_MOVNEZ, d, rb_, cond);
        wrote(F, i->dst, d);
        return;
    }

    case IR_BRZ: case IR_BRNZ: {
        int r;
        if (i->w == 8) {
            int al, ah;
            src64(F, i->a, A_LO, A_HI, &al, &ah);
            xt_alu(t, XT_OR, SCR, al, ah);
            r = SCR;
        } else {
            r = rdr(F, i->a, A_LO);
        }
        branch_to(F, test_z(i->op == IR_BRZ ? XT_BEQZ : XT_BNEZ, r),
                  i->label);
        return;
    }

    case IR_LDVAR: {
        int d = wreg(F, i->dst, ACC);
        if (in_reg(F, i->a)) {
            if (xt_ldvar_plain(i->size, i->sign, i->w)) {
                if (d != F->loc[i->a]) xt_mov(t, d, F->loc[i->a]);
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
            if (i->size >= 4) {
                if (F->loc[i->dst] != src) xt_mov(t, F->loc[i->dst], src);
            } else {
                ext_reg(F, F->loc[i->dst], src, i->size, 1);
            }
        } else {
            st_sp(F, src, sslot(F, i->dst), i->size);
        }
        return;
    }
    case IR_LOAD: {
        int addr = rdr(F, i->a, ADDR);
        int d = wreg(F, i->dst, ACC);
        if (i->vol)
            xt_memw(t);
        ld_any(F, d, addr, i->memoff, i->size, i->sign, i->natural,
               SCR, TMP);
        wrote(F, i->dst, d);
        return;
    }
    case IR_STORE: {
        int addr = rdr(F, i->a, ADDR);
        int val = rdr(F, i->b, ACC);
        if (i->vol)
            xt_memw(t);
        st_any(F, val, addr, i->memoff, i->size, i->natural, SCR);
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
    /* An address is an l32r of a literal the linker fills in
     * (R_XTENSA_32): there is no PC-relative address form. */
    case IR_STRADDR: {
        int d = wreg(F, i->dst, ACC);
        lit_load(F, d, LIT_STR, (unsigned long)i->label, NULL);
        wrote(F, i->dst, d);
        return;
    }
    case IR_GADDR: {
        int d = wreg(F, i->dst, ACC);
        lit_load(F, d, LIT_GLOB, 0, i->glob);
        wrote(F, i->dst, d);
        return;
    }
    case IR_FADDR: {
        int d = wreg(F, i->dst, ACC);
        lit_load(F, d, LIT_FUNC, 0, i->callee);
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
                int rw = ret_words(1, fn->ret_abi.size);
                if (rw > 0) {
                    /* its words into a2-a5 */
                    long adv = 0;
                    rd(F, i->a, ACC);
                    for (int q = 0; q < rw; q++)
                        comp_word(F, XT_A2 + q, ACC, fn->ret_abi.size,
                                  i->natural && fn->ret_abi.align
                                      ? fn->ret_abi.align : 1,
                                  q, TMP, &adv);
                } else {
                    /* through the caller's buffer, whose address the
                     * prologue kept; the pointer goes back in a2 */
                    rd(F, i->a, TMP);
                    ld_sp(F, ADDR, F->sret_slot, 4, 0);
                    copy_block(F, 1, fn->ret_abi.size, 0);
                    ld_sp(F, XT_A2, F->sret_slot, 4, 0);
                }
            } else if (F->wide[i->a]) {
                rd64(F, i->a, XT_A2, XT_A3);
            } else {
                rd(F, i->a, XT_A2);
            }
        } else if (fn_sret(fn)) {
            ld_sp(F, XT_A2, F->sret_slot, 4, 0);
        }
        /* retw undoes the window rotation, and with it sp: there is
         * nothing else to restore, so every return is just this */
        xt_retw(t);
        return;

    case IR_UD2:
        xt_ill(t);
        return;
    case IR_FENCE:
        xt_memw(t);
        return;

    case IR_VA_START: {
        /* GCC's record: __va_stk = the incoming sp - 32, __va_reg = the
         * register save area, __va_ndx = the named words * 4 (plus 8
         * when they already reached six, the gap between the register
         * words and __va_stk[32]). */
        int ndx = F->va_words >= NARGW ? (F->va_words + 2) * 4
                                       : F->va_words * 4;
        rd(F, i->a, ADDR);
        addr_sp(F, ACC, F->frame - 32);
        xt_store(t, ACC, ADDR, 0, 4);
        addr_sp(F, ACC, F->va_save);
        xt_store(t, ACC, ADDR, 4, 4);
        li(F, ACC, ndx);
        xt_store(t, ACC, ADDR, 8, 4);
        return;
    }

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
            xt_refuse(F, i, "a conversion of a 128-bit value");
        if (i->op == IR_I2F && src_w == 8 && i->a >= 0 && !F->wide[i->a]) {
            /* a 32-bit value asked for as 64: zero-extended */
            rd(F, i->a, XT_A10);
            xt_movi(t, XT_A11, 0);
        } else if (src_w == 8) {
            args64x2(F, i->a, -1);
        } else {
            rd(F, i->a, XT_A10);
        }
        call_helper(F, cvt_name(i));
        if (i->dst >= 0) {
            if (F->wide[i->dst])
                wr64(F, i->dst, XT_A10, XT_A11);
            else
                wr(F, i->dst, XT_A10);
        }
        return;
    }

    case IR_ASM: {
        /* Extended asm, assembled in irgen (xtensa/irgen.c irg_asm_xtensa)
         * against the vocabulary in xtensa/asm.c. This only places the
         * operands and splices the bytes -- LoongArch's lowering with the
         * windowed registers' roles.
         *
         * To the allocator (ra_target.asm_in_reg) a value live across an
         * asm keeps out of the registers it may change, which irgen
         * recorded (ir_asm.clob): its operands', its clobbers', the
         * template's, a windowed call's, and its scratch. The operands are
         * values like any other, moved into and out of their registers
         * here, each way as ONE parallel move -- one at a time would
         * overwrite a register a later operand is still to be read from.
         * No operand is ever in a0/a1 (the return address and sp), a7
         * (the frame base under alloca) or a14/a15, the scratch the moves
         * and the frame accesses below use. */
        struct ir_asm *ia = i->asm_ir;
        /* A continuation's value was written by the asm before it, which
         * must be right there: nothing may run between an asm and the
         * moment its registers are read. */
        if (ia->cont) {
            int k = n - 1;
            while (k >= 0 && fn->ins[k].op == IR_ASM && fn->ins[k].asm_ir &&
                   fn->ins[k].asm_ir->cont)
                k--;
            if (k < 0 || fn->ins[k].op != IR_ASM)
                internal_error("xtensa: %s: an asm's further output is not "
                               "right after the asm", fn->name);
            return;
        }
        int vreg_[16], vdst[16], nval = 0;
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
        /* a7 is the frame base of a function that calls alloca: every
         * slot below is addressed through it */
        if (F->fb == FBREG && (ia->clob >> FBREG & 1))
            xt_refuse(F, i, "an asm that changes a7, the frame base of a "
                            "function that calls alloca");
        for (int k = 0; k < ia->nin; k++)
            if (ia->in[k].reg < XT_A2 || ia->in[k].reg == FBREG ||
                ia->in[k].reg >= SCR)
                internal_error("xtensa: %s: an asm operand in %s", fn->name,
                               xt_reg_name(ia->in[k].reg));
        for (int k = 0; k < ia->nout; k++)
            if (!ia->out[k].mem && ia->out[k].size > 4)
                xt_refuse(F, i, "an asm output wider than a register");
        int naddr = 0;
        for (int k = 0; k < ia->nout; k++)
            naddr += !ia->out[k].val && !ia->out[k].mem;
        if (ia->scr < 0 && naddr > 0)
            xt_refuse(F, i, "an asm with no scratch register left around it");
        /* In: an input's value, an "m" output's address, and a "+"
         * output's address (its current value is loaded through it
         * below) -- the register-resident ones as one parallel move
         * (a14 breaks a cycle), then the rest from their slots. */
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
                int m = ra_parallel_move(pd, ps, npm, SCR, od, os,
                                         (int)(sizeof od / sizeof od[0]));
                if (m < 0)
                    xt_refuse(F, i, "an asm whose operands cannot be moved "
                                    "into place");
                for (int k = 0; k < m; k++)
                    xt_mov(t, od[k], os[k]);
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
                    xt_load(t, o->reg, o->reg, 0, o->size, 0);
            }
        }
        for (int k = 0; k < ia->codelen; k++)
            code_byte(t, ia->code[k]);
        /* Out, through an address: the address is live across the asm
         * (regalloc.c counts it so), so it is still there. An "m" output
         * was written BY the template through the address its register
         * holds; storing over it would destroy what it wrote. */
        for (int k = 0; k < ia->nout; k++) {
            const struct ir_asm_op *o = &ia->out[k];
            if (o->mem || o->val)
                continue;
            rd(F, o->temp, ia->scr);
            xt_store(t, o->reg, ia->scr, 0, o->size);
        }
        /* Out, as values: each to its home -- those in memory first,
         * while every operand register still holds what the asm left,
         * then the register-resident ones as one parallel move. */
        {
            int pd[16], ps[16], npm = 0;
            for (int k = 0; k < nval; k++) {
                if (vdst[k] < 0)
                    continue;
                /* An output nothing reads has no home to fill: the
                 * allocator may give two such dead values one register,
                 * and two writes to it are no parallel move. */
                if (F->usecnt && F->usecnt[vdst[k]] == 0)
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
                int m = ra_parallel_move(pd, ps, npm, SCR, od, os,
                                         (int)(sizeof od / sizeof od[0]));
                if (m < 0)
                    xt_refuse(F, i, "an asm whose outputs cannot be moved "
                                    "into place");
                for (int k = 0; k < m; k++)
                    xt_mov(t, od[k], os[k]);
            }
        }
        return;
    }

    /* ---- atomics: s32c1i, bracketed by memw ------------------------- */
    case IR_XCHG: case IR_XADD: case IR_ARMW: {
        /*   memw
         *   retry: l32i  old, addr
         *          wsr   old, scompare1
         *          (new = old OP val, or val)
         *          s32c1i new, addr      -- new <- what memory held
         *          bne   new, old, retry
         *   memw                         -- dst = old */
        int addr, val, top, again;
        need_word_atomic(F, i);
        if (i->size == 1 || i->size == 2) {
            sub_rmw(F, i);
            return;
        }
        addr = rdr(F, i->a, ADDR);
        val = rdr(F, i->b, TMP);
        xt_memw(t);
        top = t->len;
        xt_load(t, ACC, addr, 0, 4, 0);
        xt_wsr(t, ACC, XT_SR_SCOMPARE1);
        if (i->op == IR_XCHG) {
            xt_mov(t, SCR, val);
        } else if (i->op == IR_XADD) {
            xt_alu(t, XT_ADD, SCR, ACC, val);
        } else {
            switch ((int)i->imm) {
            case '&': xt_alu(t, XT_AND, SCR, ACC, val); break;
            case '|': xt_alu(t, XT_OR, SCR, ACC, val); break;
            case '^': xt_alu(t, XT_XOR, SCR, ACC, val); break;
            default:                                        /* nand */
                xt_alu(t, XT_AND, SCR, ACC, val);
                xt_neg(t, SCR, SCR);
                xt_addi(t, SCR, SCR, -1);
                break;
            }
        }
        xt_s32c1i(t, SCR, addr, 0);
        again = br_place(F, test_rr(XT_BNE, SCR, ACC));
        br_back(F, again, top);
        xt_memw(t);
        wrote(F, i->dst, ACC);
        return;
    }
    case IR_CAS: case IR_CMPXCHG: {
        /* one s32c1i is the whole compare-and-swap: it stores the desired
         * value when memory holds SCOMPARE1, and returns what it held */
        int addr, exp, des;
        need_word_atomic(F, i);
        if (i->size == 1 || i->size == 2) {
            sub_cas(F, i);
            return;
        }
        addr = rdr(F, i->a, ADDR);
        if (i->op == IR_CAS) {
            exp = rdr(F, i->b, TMP);
        } else {
            int p = rdr(F, i->b, TMP);
            xt_load(t, ACC, p, 0, 4, 0);
            exp = ACC;
        }
        xt_wsr(t, exp, XT_SR_SCOMPARE1);
        des = rdr(F, i->c, SCR);
        if (des != SCR)
            xt_mov(t, SCR, des);
        xt_memw(t);
        xt_s32c1i(t, SCR, addr, 0);
        xt_memw(t);
        if (i->op == IR_CAS) {
            wr(F, i->dst, SCR);
        } else {
            int p = rdr(F, i->b, TMP);
            int d;
            xt_rsr(t, ACC, XT_SR_SCOMPARE1);
            xt_store(t, SCR, p, 0, 4);
            d = wreg(F, i->dst, ADDR);
            emit_bool(F, test_rr(XT_BEQ, SCR, ACC), d);
            wrote(F, i->dst, d);
        }
        return;
    }
    case IR_ALLOCA: {
        /* A fresh 16-aligned block: sp moves down by the size rounded to
         * 16, with movsp (the Alloca exception's handler moves the
         * caller's spilled registers when they are below the old sp).
         * The block sits above the outgoing area, a multiple of 16. */
        int d = wreg(F, i->dst, SCR);
        rd(F, i->a, TMP);
        xt_addi(t, TMP, TMP, 15);
        xt_srli(t, TMP, TMP, 4);
        xt_slli(t, TMP, TMP, 4);
        xt_alu(t, XT_SUB, TMP, XT_SP, TMP);
        xt_movsp(t, XT_SP, TMP);
        addr_off(F, d, XT_SP, F->out_bytes);
        wrote(F, i->dst, d);
        return;
    }
    case IR_SPSAVE: {
        int d = wreg(F, i->dst, SCR);
        xt_mov(t, d, XT_SP);
        wrote(F, i->dst, d);
        return;
    }
    case IR_SPRESTORE:
        xt_movsp(t, XT_SP, rdr(F, i->a, SCR));
        return;
    case IR_SWITCH:
        xt_refuse(F, i, "a jump table");
        return;
    case IR_BSWAP:
        xt_refuse(F, i, "a byte swap as one operation");
        return;
    case IR_LABELADDR: {
        if (cg_label_mark(i))       /* static data's marker: no code */
            return;
        /* &&label: l32r of a pool word holding the label's address --
         * the far jump's literal, R_XTENSA_32 against .text plus the
         * label's offset (RK_XTENSA_TEXT32). */
        int d = wreg(F, i->dst, ACC);
        lit_load(F, d, LIT_LABEL, (unsigned long)i->label, NULL);
        wrote(F, i->dst, d);
        return;
    }
    case IR_IGOTO:
        xt_jx(t, rdr(F, i->a, ACC));
        return;
    default:
        xt_refuse(F, i, "this operation");
    }
}

/* ---- register pairs ----------------------------------------------------------- */

static const struct ra_target XT_PAIR_RA = {
    xt_pair_pool_for, xt_callee_saved, xt_ldvar_plain,
    1, 1, 1,
    xtensa_op_calls_helper,
    0,
    xt_pair_hints,
    NULL, NULL,
    1,
    NULL, NULL,
    1,
    0,
    0
};

static void xt_pair_hints(const struct ir_func *fn, int *hint)
{
    int words = fn_sret(fn) ? 1 : 0;
    long stk = 0;
    struct xplace pl;
    for (int p = 0; fn->src && p < fn->nparams && p < fn->nvregs; p++) {
        const struct ir_arg *a = &fn->param_abi[p];
        place_arg(a, &words, &stk, &pl);
        if (a->size == 8 && !a->is_struct && pl.p[0].reg >= 0 &&
            !fn->is_varargs)
            hint[p] = IN_ARG(pl.p[0].reg);
    }
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        if (i->op == IR_RET && i->a >= 0 && i->a < fn->nvregs &&
            !fn->ret_abi.is_struct && fn->ret_abi.size == 8)
            hint[i->a] = XT_A2;
        if (i->op != IR_CALL && xtensa_op_calls_helper(i) && i->w == 8) {
            if (i->a >= 0 && i->a < fn->nvregs && hint[i->a] < 0)
                hint[i->a] = XT_A10;
            if (!i->imm_b && i->b >= 0 && i->b < fn->nvregs &&
                hint[i->b] < 0)
                hint[i->b] = XT_A12;
            if (i->dst >= 0 && i->dst < fn->nvregs && hint[i->dst] < 0)
                hint[i->dst] = XT_A10;
        }
        if (i->op != IR_CALL)
            continue;
        if (!i->retsize && i->dst >= 0 && i->dst < fn->nvregs && i->w == 8)
            hint[i->dst] = XT_A10;
        words = call_sret(i) ? 1 : 0;
        stk = 0;
        for (int k = 0; k < i->nargs; k++) {
            const struct ir_arg *a = &i->argv[k];
            place_arg(a, &words, &stk, &pl);
            if (a->size == 8 && !a->is_struct && pl.p[0].reg >= 0 &&
                pl.p[0].reg + 1 < NARGW - 1 &&
                a->vreg >= 0 && a->vreg < fn->nvregs)
                hint[a->vreg] = OUT_ARG(pl.p[0].reg);
        }
    }
}

static struct ra_range *g_xt_res;
static int g_xt_nres, g_xt_capres;
static void reserve_pairs(struct ir_func *fn, const int *loc)
{
    int nv = fn->nvregs;
    int *first = xmalloc((size_t)(nv ? nv : 1) * sizeof *first);
    int *last = xmalloc((size_t)(nv ? nv : 1) * sizeof *last);
    ra_live_ranges(fn, first, last);
    g_xt_nres = 0;
    for (int v = 0; v < nv; v++) {
        if (loc[v] < 0 || first[v] < 0) continue;
        if (g_xt_nres + 2 > g_xt_capres) {
            g_xt_capres = g_xt_capres ? g_xt_capres * 2 : 16;
            g_xt_res = xrealloc(g_xt_res,
                                (size_t)g_xt_capres * sizeof *g_xt_res);
        }
        for (int h = 0; h < 2; h++) {
            g_xt_res[g_xt_nres].reg = loc[v] + h;
            g_xt_res[g_xt_nres].first = first[v];
            g_xt_res[g_xt_nres].last = last[v];
            g_xt_res[g_xt_nres].born = 0;
            g_xt_nres++;
        }
    }
    ra_reserve(g_xt_res, g_xt_nres);
    free(first); free(last);
}

static int *pair_alloc(struct ir_func *fn, struct xt_fn *F, const char *pin)
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
    }
    F->npair = 0;
    if (!any) {
        free(x);
        return NULL;
    }
    loc = ra_allocate(fn, &XT_PAIR_RA, NULL, x, used, &nused);
    free(x);
    reserve_pairs(fn, loc);
    for (int k = 0; k < nused && k < XT_NPAIRS; k++)
        F->pair_used[F->npair++] = used[k];
    return loc;
}

/* ---- one function ----------------------------------------------------------------- */

/* A parameter's word q (of the argument words) in a register ready to
 * store: its incoming register, or in a variadic function the copy the
 * prologue saved. */
static int param_word(struct xt_fn *F, int w, int scratch)
{
    if (!F->fn->is_varargs)
        return IN_ARG(w);
    ld_sp(F, scratch, F->va_save + 4L * w, 4, 0);
    return scratch;
}

/* The prologue's parameter placement. */
static void place_params(struct xt_fn *F)
{
    struct ir_func *fn = F->fn;
    struct code *t = F->t;
    int words = 0;
    long stk = 0;
    long base = F->frame;         /* the caller's outgoing area, from fb */
    int pmv_dst[RA_MAXPOOL], pmv_src[RA_MAXPOOL], npmv = 0;
    int pstk_reg[RA_MAXPOOL]; long pstk_off[RA_MAXPOOL];
    int npstk = 0;
    struct xplace pl;

    if (F->sret_slot >= 0) {
        st_sp(F, param_word(F, 0, ACC), F->sret_slot, 4);
        words = 1;
    }
    for (int i = 0; i < fn->nparams; i++) {
        struct ir_arg *a = &fn->param_abi[i];
        place_arg(a, &words, &stk, &pl);
        if (!a->is_struct) {
            struct xpiece *pc = &pl.p[0];
            for (int q = 0; q < pc->nw; q++) {
                long where = pc->reg >= 0 ? -1 : base + pc->stk + 4L * q;
                int dreg = in_reg(F, i) ? F->loc[i] + q : -1;
                if (dreg >= 0 && pc->reg >= 0 && !fn->is_varargs) {
                    pmv_dst[npmv] = dreg;
                    pmv_src[npmv] = IN_ARG(pc->reg + q);
                    npmv++;
                } else if (dreg >= 0) {
                    pstk_reg[npstk] = dreg;
                    pstk_off[npstk] = pc->reg >= 0
                        ? F->va_save + 4L * (pc->reg + q) : where;
                    npstk++;
                } else if (F->slot[i] >= 0) {
                    int r;
                    if (pc->reg >= 0) {
                        r = param_word(F, pc->reg + q, ACC);
                    } else {
                        ld_sp(F, ACC, where, 4, 0);
                        r = ACC;
                    }
                    if (a->size < 4)
                        st_sp(F, r, sslot(F, i), a->size);
                    else
                        st_sp(F, r, sslot(F, i) + 4L * q, 4);
                }
            }
            continue;
        }
        /* A composite's words (a _Complex's two parts) into its slot; an
         * odd-sized one's last word only as far as the object goes. */
        for (int p = 0; p < pl.np; p++) {
            struct xpiece *pc = &pl.p[p];
            long first = (long)p * pl.esz;          /* byte offset of piece */
            for (int q = 0; q < pc->nw; q++) {
                long off = first + 4L * q;
                long left = (p + 1 == pl.np ? a->size : first + pl.esz) - off;
                int r;
                if (left <= 0)
                    break;
                if (pc->reg >= 0) {
                    r = param_word(F, pc->reg + q, ACC);
                } else {
                    ld_sp(F, ACC, base + pc->stk + 4L * q, 4, 0);
                    r = ACC;
                }
                if (left >= 4 && !((sslot(F, i) + off) & 3)) {
                    st_sp(F, r, sslot(F, i) + off, 4);
                } else {
                    if (r != ACC) { xt_mov(t, ACC, r); r = ACC; }
                    for (long b = 0; b < left && b < 4; b++) {
                        if (b) xt_srli(t, r, r, 8);
                        st_sp(F, r, sslot(F, i) + off + b, 1);
                    }
                }
            }
        }
    }
    if (npmv) {
        int od[RA_MAXPOOL * 2], os[RA_MAXPOOL * 2];
        int m = ra_parallel_move(pmv_dst, pmv_src, npmv, ACC, od, os,
                                 (int)(sizeof od / sizeof od[0]));
        if (m < 0)
            internal_error("xtensa: %s: the prologue's parameter placement "
                           "is not a well-formed move", fn->name);
        for (int k = 0; k < m; k++)
            xt_mov(t, od[k], os[k]);
    }
    for (int k = 0; k < npstk; k++)
        ld_sp(F, pstk_reg[k], pstk_off[k], 4, 0);
    F->va_words = words;
}

/* The pool's words (and the islands'): constants written now, addresses
 * left to the linker. */
static void fill_pool(struct xt_fn *F)
{
    for (int k = 0; k < F->nlit + F->nisl; k++) {
        int at = k < F->nlit ? F->pool_at + 4 * k : F->isl_at[k - F->nlit];
        struct xt_lit *l = k < F->nlit ? &F->lit[k] : &F->isl[k - F->nlit];
        switch (l->kind) {
        case LIT_CONST:
            code_patch32(F->t, at, l->v);
            break;
        case LIT_STR:
            code_patch32(F->t, at, 0);
            note_str(F->st, at, (int)l->v, RK_ABS32);
            break;
        case LIT_GLOB:
            code_patch32(F->t, at, 0);
            note_glob(F->st, at, (struct global *)l->p, RK_ABS32);
            break;
        case LIT_LABEL:
            code_patch32(F->t, at, 0);
            note_str(F->st, at, F->label_off[l->v], RK_XTENSA_TEXT32);
            break;
        default:
            code_patch32(F->t, at, 0);
            note_fn(F->st, at, (struct func *)l->p, RK_ABS32);
            break;
        }
    }
}

static void gen_func(struct ir_func *fn, struct code *t, struct xt_sites *st,
                     int want_debug)
{
    struct func *f = fn->src;
    struct xt_fn F;
    int i;

    memset(&F, 0, sizeof F);
    F.fn = fn; F.t = t; F.st = st;
    F.want_debug = want_debug;
    F.wide = wide_map(fn);
    F.nshr = ra_narrow_hishift(fn);
    for (int v = 0; v < fn->nvregs; v++)
        if (F.nshr[v]) F.wide[v] = 0;
    F.fb = XT_SP;
    if (g_xt_regalloc) {
        char *pin = want_debug ? ra_debug_pin_vars(fn) : (char *)0;
        int *pair = g_xt_pairs ? pair_alloc(fn, &F, pin) : NULL;
        F.loc = ra_allocate(fn, &XT_RATGT, F.wide, pin, F.used_callee,
                            &F.nsave);
        g_xt_taken = 0;
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
        /* EMBCC_XT_RA_MAX=N leaves only the first N vregs in registers --
         * always correct -- so a miscompile that comes and goes with N
         * names the value whose allocation is wrong. */
        {
            const char *lim = getenv("EMBCC_XT_RA_MAX");
            if (lim) {
                int n = atoi(lim);
                for (int v = n; v < fn->nvregs; v++)
                    F.loc[v] = -1;
            }
        }
    }
    layout(&F);

    F.label_off = xmalloc((size_t)(fn->nlabels + 1) * sizeof *F.label_off);
    f->code_align = 4;
    if (want_debug) {
        int nv = fn->nvars ? fn->nvars : 1;
        fn->var_off = xmalloc((size_t)nv * sizeof *fn->var_off);
        for (int v = 0; v < fn->nvars; v++)
            fn->var_off[v] = (int)F.slot[v];
    }
    /* The attempts: every branch tried short, then the ones that did not
     * reach long, until nothing new fails; and the literal pool reserved
     * at the size the last attempt wanted. */
    {
    int len0, nl0 = fn->nlines;
    int se0 = F.st->next, ss0 = F.st->nstr, sg0 = F.st->ng, sf0 = F.st->nf;
    char *longb = NULL;
    int nlongb = 0;
    code_align(t, 4, 0);
    len0 = t->len;
    for (;;) {
    int nfail = 0;
    t->len = len0;
    fn->nlines = nl0;
    F.st->next = se0; F.st->nstr = ss0; F.st->ng = sg0; F.st->nf = sf0;
    F.nfix = 0;
    F.nlit = 0;
    F.nisl = 0;
    for (i = 0; i <= fn->nlabels; i++)
        F.label_off[i] = -1;
    F.skip_next = 0;
    F.fb = XT_SP;
    F.longb = longb;
    F.nlongb = nlongb;
    F.pool_at = t->len;
    for (i = 0; i < 4 * F.npool; i++)
        code_byte(t, 0);
    f->code_off = F.pool_at;
    f->code_entry = 4 * F.npool;

    /* The prologue: entry, and movsp for a frame entry cannot take (a8
     * is free: no argument arrives there). */
    if (F.frame <= 32760) {
        xt_entry(t, XT_SP, F.frame);
    } else {
        xt_entry(t, XT_SP, 32);
        li(&F, XT_A8, -(F.frame - 32));
        xt_alu(t, XT_ADD, XT_A8, XT_SP, XT_A8);
        xt_movsp(t, XT_SP, XT_A8);
    }
    /* A variadic function keeps every argument register in its save area,
     * where va_arg finds the unnamed ones and the prologue the named. */
    if (fn->is_varargs)
        for (int k = 0; k < NARGW; k++)
            st_sp(&F, IN_ARG(k), F.va_save + 4L * k, 4);
    place_params(&F);
    if (fn->has_alloca) {
        xt_mov(t, FBREG, XT_SP);
        F.fb = FBREG;
    }

    for (i = 0; i < fn->nins; i++) {
        gen_ins(&F, i);
        if (F.skip_next) {
            F.skip_next = 0;
            i++;
        }
    }
    /* Falling off the end of a void function returns. */
    if (!fn->nins || fn->ins[fn->nins - 1].op != IR_RET) {
        if (fn_sret(fn))
            ld_sp(&F, XT_A2, F.sret_slot, 4, 0);
        xt_retw(t);
    }

    for (i = 0; i < F.nfix; i++) {
        int target = F.label_off[F.fix[i].label];
        if (target < 0)
            internal_error("xtensa: label %d of %s was never placed",
                           F.fix[i].label, fn->name);
        if (F.fix[i].kind == FX_FAR)
            continue;               /* its literal is the label's address */
        if (!xt_patch_branch(t, F.fix[i].at, target)) {
            if (nlongb < F.nfix) {
                longb = xrealloc(longb, (size_t)F.nfix);
                memset(longb + nlongb, 0, (size_t)(F.nfix - nlongb));
                nlongb = F.nfix;
            }
            /* a short branch becomes the inverse over a j, a j that does
             * not reach the far form */
            if (longb[i] >= (F.fix[i].kind == FX_J ? 2 : 1))
                internal_error("xtensa: %s: a long branch was patched as "
                               "a short one", fn->name);
            longb[i] = F.fix[i].kind == FX_J ? 2 : 1;
            nfail++;
        }
    }
    if (F.nlit > F.npool) {
        F.npool = F.nlit;
        nfail++;
    }
    if (!nfail)
        break;
    }                                   /* the attempts */
    free(longb);
    }
    fill_pool(&F);
    code_mark_data(t, F.pool_at, F.pool_at + 4 * F.npool);

    f->code_len = t->len - f->code_off;
    f->stack_bytes = (int)F.frame;
    free(F.usecnt);
    free(F.slot);
    cg_note_labels(fn, F.label_off);
    free(F.label_off);
    free(F.fix);
    free(F.lit);
    free(F.isl);
    free(F.isl_at);
    free(F.wide);
    free(F.nshr);
    free(F.loc);
}

/* With the allocator on, a function is generated with the pair pass and
 * without it, and the shorter is kept (the RV32 and MIPS arrangement). */
static void gen_func_best(struct ir_func *fn, struct code *t,
                          struct xt_sites *st, int want_debug)
{
    int at = t->len, next = st->next, nstr = st->nstr, ng = st->ng,
        nf = st->nf, with;

    /* A field's constant offset into its load or store, before
     * allocation: l8ui reaches 255 and the word forms more, so 0..255
     * serves every size (a misaligned residue is moved into the base). */
    if (g_xt_regalloc && !want_debug && !getenv("EMBCC_NO_MEMOFF")) {
        char *w = wide_map(fn);
        ra_fold_memoff(fn, 0, 248, 4, 4, w, 0, 0);
        free(w);
    }
    g_xt_pairs = 1;
    if (!g_xt_regalloc || want_debug || getenv("EMBCC_XT_PAIRS")) {
        if (getenv("EMBCC_XT_PAIRS"))
            g_xt_pairs = atoi(getenv("EMBCC_XT_PAIRS"));
        gen_func(fn, t, st, want_debug);
        g_xt_pairs = 1;
        return;
    }
    gen_func(fn, t, st, want_debug);
    with = t->len - at;
    t->len = at; st->next = next; st->nstr = nstr; st->ng = ng; st->nf = nf;
    g_xt_pairs = 0;
    gen_func(fn, t, st, want_debug);
    if (t->len - at > with) {
        t->len = at; st->next = next; st->nstr = nstr; st->ng = ng;
        st->nf = nf;
        g_xt_pairs = 1;
        gen_func(fn, t, st, want_debug);
    }
    g_xt_pairs = 1;
}

void codegen_unit_xtensa(struct ir_unit *iu, struct code *text,
                         struct extcall **ext, int *next,
                         struct strsite **strs, int *nstrs,
                         struct gsite **gs, int *ngs,
                         struct fsite **fs, int *nfs, int want_debug,
                         int optimize, int no_sse, int regalloc)
{
    struct xt_sites st;

    (void)optimize; (void)no_sse;
    g_xt_regalloc = regalloc;
    memset(&st, 0, sizeof st);
    for (int n = 0; n < iu->nfuncs; n++)
        gen_func_best(&iu->funcs[n], text, &st, want_debug);
    cg_resolve_strsites(iu, st.str, st.nstr);
    *ext = st.ext;   *next = st.next;
    *strs = st.str;  *nstrs = st.nstr;
    *gs = st.g;      *ngs = st.ng;
    *fs = st.f;      *nfs = st.nf;
}
