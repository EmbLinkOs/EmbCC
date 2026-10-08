/* ---- sinking a constant to the use that wants it ----------------------
 *
 * A literal has no operands and no side effects, so where it is
 * MATERIALISED is free to choose -- and the choice costs a register for
 * however long the value is live. irgen and the loop passes leave
 * plenty of them a long way from the one instruction that reads them:
 * 464 of the 2242 constants across lib/libc and lib/libcxx have a
 * single use more than one instruction later, 116 of them more than
 * eight. 186 of the 828 values the x86-64 allocator spills are
 * constants, which is a slot and a reload for something a `mov $imm`
 * reproduces in one instruction wherever it is wanted.
 *
 * So one of these with exactly one use moves to just before it. Always
 * sound, in the strong sense: nothing it depends on can have changed,
 * because it depends on nothing -- the same holds for the address of a
 * local, a global, a string or a function, so those move too (75 and 63
 * of the spilled values are an `addr` and a `straddr`). It cannot move to a place the use does
 * not reach, because that place is where the use is.
 *
 * Only forward. A use EARLIER in the instruction stream is the far side
 * of a back edge, and moving the definition after it would leave the
 * first iteration reading nothing.
 *
 * And never INTO a loop. A constant before a loop whose one use is
 * inside it is very often there because LICM put it there, and sinking
 * it undoes the hoist: the value is rebuilt on every trip. That is one
 * instruction on x86-64, but two on aarch64 for a 32-bit multiplier
 * (mov + movk) and three for a global's address (adrp + add + mov) --
 * an FNV hash rebuilt 16777619 for every byte it read. A loop here is
 * the span of a backward branch, which is what the IR has for one.
 */

#include "opt_int.h"

/* Does this constant take more than one instruction to build on the
 * target? Only those are worth a register held across a loop: x86-64
 * builds anything in one (`mov $imm`, `lea sym(%rip)`), and there sinking
 * into a loop frees a register for one instruction a trip -- keeping them
 * all hoisted made matrix and sort slower and lib/libc bigger. A global's
 * address is two (adrp+add, movw+movt, auipc+addi); a literal is two when
 * it is outside the one-instruction range (movz/movn or a bitmask on
 * aarch64; a 16-bit movw or a modified immediate on Thumb; 12 bits on
 * RISC-V). AVR is left as it was: every multi-byte value there is several
 * instructions, and its register file is what runs out first. */
int t_imm_ok(long imm);                 /* arch/thumb/emit.c */
int a64_bitmask_ok(long imm, int w);    /* arch/aarch64/emit.c */
static int const_is_expensive(const struct ir_ins *i)
{
    enum target_arch ta = target_get();
    /* RX: any constant or address is one `mov.l #imm, rd` */
    if (ta == TARGET_X86_64 || ta == TARGET_AVR || ta == TARGET_RX)
        return 0;
    switch (i->op) {
    case IR_GADDR: case IR_STRADDR: case IR_FADDR:
        return 1;
    case IR_CONST: {
        long v = i->imm;
        if (i->w == 4) v = (long)(int)v;
        if (ta == TARGET_AARCH64) {
            unsigned long u = (unsigned long)v, n = ~u;
            int w = i->w == 4 ? 4 : 8;
            if (w == 4) { u &= 0xffffffffUL; n &= 0xffffffffUL; }
            for (int s = 0; s < 8 * w; s += 16) {
                if ((u & ~(0xffffUL << s)) == 0) return 0;     /* movz */
                if ((n & ~(0xffffUL << s)) == 0) return 0;     /* movn */
            }
            return !a64_bitmask_ok(w == 4 ? (long)(unsigned)u : v, w);
        }
        if (ta == TARGET_THUMB)
            return !(t_imm_ok(v) || (v >= 0 && v <= 0xffff));
        if (ta == TARGET_MIPS32 || ta == TARGET_MIPS64)  /* addiu, ori from $0 */
            return !((v >= -32768 && v <= 32767) || (v >= 0 && v <= 0xffff));
        if (ta == TARGET_LOONGARCH64)  /* ori/addi.w from r0, or a lu12i.w */
            return !((v >= -2048 && v <= 4095) ||
                     ((v & 0xfff) == 0 && v == (long)(int)v));
        if (ta == TARGET_TRICORE)               /* mov, mov.u or movh */
            return !((v >= -32768 && v <= 32767) || (v >= 0 && v <= 0xffff) ||
                     !(v & 0xffff));
        if (ta == TARGET_XTENSA)                /* movi; else a literal */
            return !(v >= -2048 && v <= 2047);
        if (ta == TARGET_PPC32)                 /* li, or lis */
            return !((v >= -32768 && v <= 32767) || (v & 0xffff) == 0);
        if (ta == TARGET_SPARC32)               /* or from %g0, or sethi */
            return !((v >= -4096 && v <= 4095) || (v & 0x3ff) == 0);
        if (ta == TARGET_COLDFIRE)              /* moveq */
            return !(v >= -128 && v <= 127);
        return !(v >= -2048 && v <= 2047);                     /* RISC-V */
    }
    default:
        return 0;
    }
}

static int *sink_loop_depth(const struct ir_func *fn)
{
    int N = fn->nins;
    int *depth = xcalloc((size_t)(N ? N : 1), sizeof *depth);
    int *lab = xmalloc((size_t)(fn->nlabels ? fn->nlabels : 1) * sizeof *lab);
    for (int l = 0; l < fn->nlabels; l++) lab[l] = -1;
    for (int i = 0; i < N; i++)
        if (fn->ins[i].op == IR_LABEL && fn->ins[i].label >= 0 &&
            fn->ins[i].label < fn->nlabels)
            lab[fn->ins[i].label] = i;
    for (int i = 0; i < N; i++) {
        const struct ir_ins *in = &fn->ins[i];
        if ((in->op == IR_JMP || in->op == IR_BRZ || in->op == IR_BRNZ) &&
            in->label >= 0 && in->label < fn->nlabels &&
            lab[in->label] >= 0 && lab[in->label] <= i)
            for (int k = lab[in->label]; k <= i; k++)
                depth[k]++;
    }
    free(lab);
    return depth;
}
/* each_read's callback for "which instruction reads this vreg": the
 * FIRST one wins, which is the only one when the use count is 1. */
struct sink_at_ctx { int *at; int nv; int n; };
static void sink_at_cb(int *p, void *ctx)
{
    struct sink_at_ctx *s = ctx;
    if (*p >= 0 && *p < s->nv && s->at[*p] < 0) s->at[*p] = s->n;
}

int pass_sinkconst(struct ir_func *fn)
{
    int nv = fn->nvregs;
    if (nv == 0 || fn->nins == 0)
        return 0;
    int *use = xcalloc((size_t)nv, sizeof *use);
    struct ucount uc = { use, nv };
    for (int n = 0; n < fn->nins; n++)
        each_read(&fn->ins[n], count_cb, &uc);

    struct defs d;
    compute_defs(fn, &d);

    /* The one instruction that reads each vreg, found in a single pass:
     * `use[] == 1` above already says there is exactly one. */
    int *at = xmalloc((size_t)nv * sizeof *at);
    for (int v = 0; v < nv; v++) at[v] = -1;
    struct sink_at_ctx sa = { at, nv, 0 };
    for (int n = 0; n < fn->nins; n++) {
        sa.n = n;
        each_read(&fn->ins[n], sink_at_cb, &sa);
    }

    /* where each sinkable constant wants to go */
    int *to = xmalloc((size_t)fn->nins * sizeof *to);
    for (int n = 0; n < fn->nins; n++) to[n] = -1;
    int *depth = sink_loop_depth(fn);
    int any = 0;
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        /* Every op here materialises a value out of nothing: a
         * literal, or the address of a local, a global, a string or a
         * function. None reads a vreg, so none can be invalidated by
         * what it moves past. */
        switch (i->op) {
        case IR_CONST: case IR_ADDR: case IR_GADDR:
        case IR_STRADDR: case IR_FADDR:
            break;
        default:
            continue;
        }
        if (i->dst < 0 || i->dst >= nv)
            continue;
        if (use[i->dst] != 1 || d.cnt[i->dst] != 1)
            continue;
        /* A RISC-V branch compares two registers: a loop's bound is a
         * `li` every trip once it sits beside the compare, where x86 and
         * Arm have taken it as an immediate before this runs. It used to
         * stay out by accident -- the guard in front of the loop read it
         * too -- until the guard could be decided at compile time. */
        /* (MIPS's beq/bne compare two registers too.) */
        int rv_cmp = (target_get() == TARGET_RISCV32 ||
                      target_get() == TARGET_RISCV64 ||
                      target_is_mips() ||
                      target_get() == TARGET_LOONGARCH64 ||
                      target_get() == TARGET_XTENSA) &&
                     i->op == IR_CONST && i->imm != 0 && at[i->dst] >= 0 &&
                     fn->ins[at[i->dst]].op == IR_CMP;
        /* A select's value stops above the compare that makes its
         * condition: between the two it would part the flags from the
         * IT block or cmov that reads them (ifconv_any put it there). */
        int u = at[i->dst];
        if (u > 0 && fn->ins[u].op == IR_SELECT && target_cheap_select() &&
            fn->ins[u - 1].op == IR_CMP && fn->ins[u - 1].dst == fn->ins[u].a)
            u--;
        if (u > n + 1 &&
            (depth[u] <= depth[n] || g_opt_size ||
             (!const_is_expensive(i) && !rv_cmp))) {
            to[n] = u;
            any = 1;
        }
    }
    free(at);
    free(depth);
    if (!any) { free(use); free(to); free_defs(&d); return 0; }

    /* Rebuild in one pass: `head[m]` chains the constants that want to
     * land just before instruction m, in their original order. */
    int *head = xmalloc((size_t)fn->nins * sizeof *head);
    int *next = xmalloc((size_t)fn->nins * sizeof *next);
    for (int n = 0; n < fn->nins; n++) { head[n] = -1; next[n] = -1; }
    for (int n = fn->nins - 1; n >= 0; n--)
        if (to[n] >= 0) { next[n] = head[to[n]]; head[to[n]] = n; }
    struct ibuf nb = { 0, 0, 0 };
    for (int n = 0; n < fn->nins; n++) {
        for (int k = head[n]; k >= 0; k = next[k])
            *ib_push(&nb) = fn->ins[k];
        if (to[n] < 0) *ib_push(&nb) = fn->ins[n];
    }
    free(head); free(next);
    free(fn->ins);
    fn->ins = nb.p; fn->nins = nb.n; fn->cap = nb.cap;
    free(use); free(to); free_defs(&d);
    return 1;
}

/* ---- immediate-operand folding ---- */

/* x86 ALU/compare immediates are imm32 (sign-extended to 64). A value outside
 * that range must stay in a register. */
static int fits_imm32(long v)
{
    return v >= -2147483647L - 1 && v <= 2147483647L;
}

/* The predicate when a comparison's operands are swapped: `a < b` becomes
 * `b > a`, so folding a constant `a` into `cmp b, imm` flips the direction. */
enum binop swap_pred(enum binop p)
{
    switch (p) {
    case B_LT: return B_GT; case B_GT: return B_LT;
    case B_LE: return B_GE; case B_GE: return B_LE;
    default:   return p;   /* EQ/NE are symmetric */
    }
}

/* Fold a constant operand of an integer ALU/compare op into an immediate, so
 * the value need not be materialised in a register. Run once AFTER the main
 * fixpoint (fold/lvn/copyprop never see the imm_b form) and followed by DCE,
 * which drops the CONSTs that folding left unreferenced. */
/* May constant `imm` become op's immediate operand? Yes, except where the
 * target's instructions cannot hold it: there a folded constant is rebuilt
 * at every use -- a movw+movt pair each time on Thumb -- where one left in
 * a register is built once and can be hoisted out of a loop. */
static int target_imm_foldable(int op, long imm, int w)
{
    if (target_get() == TARGET_THUMB && w == 8 &&
        (op == IR_AND || op == IR_OR || op == IR_XOR) && !getenv("EMBCC_T_NOWIDEIMM"))
        return thumb_imm_foldable64(op, imm);
    if (target_get() == TARGET_RISCV32 && w == 8 &&
        (op == IR_AND || op == IR_OR || op == IR_XOR) && !getenv("EMBCC_RV_NOWIDEIMM"))
        return riscv_imm_foldable64(op, imm);
    if (target_get() == TARGET_MIPS32 && w == 8 &&
        (op == IR_AND || op == IR_OR || op == IR_XOR))
        return mips_imm_foldable64(op, imm);
    if (target_get() == TARGET_MIPS32)
        return mips_imm_foldable(op, imm);
    /* MIPS64: one register to 64 bits; a 128-bit operation takes none */
    if (target_get() == TARGET_MIPS64)
        return w <= 8 && mips_imm_foldable(op, imm);
    /* TriCore: a 64-bit AND/OR/XOR is done half by half with any constant
     * (codegen.c's logic_half); every other 64-bit operation builds it */
    if (target_get() == TARGET_TRICORE)
        return w == 8 ? op == IR_AND || op == IR_OR || op == IR_XOR
                      : tc_imm_foldable(op, imm);
    if (target_get() == TARGET_XTENSA)
        return w == 4 && xtensa_imm_foldable(op, imm);
    if (target_get() == TARGET_PPC32 && w == 8 &&
        (op == IR_AND || op == IR_OR || op == IR_XOR))
        return ppc_imm_foldable64(op, imm);
    if (target_get() == TARGET_PPC32)
        return ppc_imm_foldable(op, imm);
    if (target_get() == TARGET_SPARC32 && w == 8 &&
        (op == IR_AND || op == IR_OR || op == IR_XOR))
        return sparc_imm_foldable64(op, imm);
    if (target_get() == TARGET_SPARC32)
        return sparc_imm_foldable(op, imm);
    if (target_get() == TARGET_COLDFIRE)
        return cf_imm_foldable(op, imm, w);
    if (target_get() == TARGET_THUMB)
        return thumb_imm_foldable(op, imm);
    if (target_get() == TARGET_RISCV32 || target_get() == TARGET_RISCV64)
        return riscv_imm_foldable(op, imm);
    if (target_get() == TARGET_LOONGARCH64)
        return la_imm_foldable(op, imm);
    if (target_get() == TARGET_AARCH64)
        return a64_imm_foldable(op, imm, w);
    return 1;
}

/* `if (x >> 63)` for a 64-bit x is `if (x < 0)`: the shift's 0 or 1 is the
 * sign bit, which a signed compare with zero reads straight off the high
 * word. On a 32-bit target the shift is two instructions (the bit moved
 * down, the high word zeroed) and the branch an orrs of both halves; the
 * compare is one `cmp hi, #0` fused into the branch. Only for a shift by
 * the width less one, unsigned, whose one reader is a branch -- which
 * then tests the compare's 0 or 1 at four bytes, a compare's result being
 * an int. After immfold, so the zero can be an immediate. */
int pass_signtest(struct ir_func *fn)
{
    int nv = fn->nvregs, changed = 0;
    if (fn->nins == 0 || nv == 0 || getenv("EMBCC_NO_SIGNTEST"))
        return 0;
    int *use = xcalloc((size_t)nv, sizeof *use);
    struct ucount uc = { use, nv };
    for (int n = 0; n < fn->nins; n++)
        each_read(&fn->ins[n], count_cb, &uc);
    for (int n = 0; n + 1 < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n], *br = &fn->ins[n + 1];
        /* ...and the same AND read by `== 0` or `!= 0`, which is how C
         * spells `while (!(m >> 52 & 1))`: both at four bytes. */
        if (i->op == IR_AND && i->imm_b && !i->flt && i->w == 8 &&
            i->imm >= 0 && i->imm <= 0xffffffffL && i->dst >= 0 &&
            i->dst < nv && use[i->dst] == 1 && br->op == IR_CMP &&
            br->a == i->dst && br->imm_b && br->imm == 0 && br->w == 8 &&
            (br->pred == B_EQ || br->pred == B_NE) &&
            !getenv("EMBCC_NO_SIGNTEST")) {
            i->w = 4;
            i->sign = 0;
            br->w = 4;
            changed = 1;
            continue;
        }
        if ((br->op != IR_BRZ && br->op != IR_BRNZ) || br->a != i->dst ||
            i->flt || !i->imm_b || i->w != 8 || i->dst < 0 ||
            i->dst >= nv || use[i->dst] != 1)
            continue;
        /* `if (x & K)` with K below 2^32 tests only bits of the low word:
         * the AND and the branch at four bytes, which on a 32-bit target is
         * one and and one branch instead of a pair and an or -- and leaves
         * whatever x is read narrow, `(m >> 52) & 1` a shift of the high
         * word alone. */
        if (i->op == IR_AND && i->imm >= 0 && i->imm <= 0xffffffffL &&
            !getenv("EMBCC_NO_SIGNTEST")) {
            i->w = 4;
            i->sign = 0;
            br->w = 4;
            changed = 1;
            continue;
        }
        if (i->op != IR_SHR || i->sign || i->imm != 63)
            continue;
        i->op = IR_CMP;
        i->pred = B_LT;
        i->sign = 1;
        i->imm = 0;                     /* x <s 0, still imm_b */
        br->w = 4;
        changed = 1;
    }
    free(use);
    return changed;
}

/* A store of N bytes writes the low N bytes of its value, and an
 * extension from S >= N bytes leaves those bytes as they were -- so the
 * store may take the extension's SOURCE, and the extension, read by
 * nothing else, goes with the DCE after. `k[n] = (char)(v + 'a')` was
 * ext.4:1s and store:1: a movsbl before every byte store on x86-64, a
 * shift pair on RV32. 45 of the 555 stores across lib/libc.
 *
 * Both values must be defined exactly once: a merge temp is written on
 * each path into it, and a source written again between
 * the extension and the store would be read at its new value. */
int pass_storenarrow(struct ir_func *fn)
{
    struct defs d;
    int changed = 0;
    compute_defs(fn, &d);
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        const struct ir_ins *e;
        int v = i->b, x;
        if (i->op != IR_STORE || i->flt || v < 0 || v >= fn->nvregs ||
            i->size <= 0 || d.cnt[v] != 1 || d.ins[v] < 0)
            continue;
        e = &fn->ins[d.ins[v]];
        if (e->op != IR_EXT || e->flt || e->size < i->size || e->size > 8)
            continue;
        x = e->a;
        if (x < 0 || x >= fn->nvregs || d.cnt[x] != 1)
            continue;
        i->b = x;
        changed = 1;
    }
    free_defs(&d);
    return changed;
}

/* Thumb's mla and mls take the product's operands in registers -- the
 * backend fuses `p = a * b; d = c +- p` into one when p's only reader is
 * the instruction right after it. A constant folded into the multiply
 * turns that into a shifted add and an add, two or three instructions,
 * where `mla d, a, rK, c` is one and rK is built once (and outside the
 * loop, often). So such a multiply keeps its constant. `use` is each
 * vreg's read count. */
static int mla_keeps_reg(const struct ir_func *fn, int n, const int *use)
{
    const struct ir_ins *i = &fn->ins[n];
    if (target_get() != TARGET_THUMB || i->op != IR_MUL || i->w != 4 ||
        i->dst < 0 || i->dst >= fn->nvregs || use[i->dst] != 1 ||
        n + 1 >= fn->nins || getenv("EMBCC_NO_MLA") ||
        getenv("EMBCC_NO_MLAKEEP"))
        return 0;
    const struct ir_ins *nx = &fn->ins[n + 1];
    if ((nx->op != IR_ADD && nx->op != IR_SUB) || nx->flt || nx->w != 4 ||
        nx->dst < 0)
        return 0;
    return (nx->b == i->dst && nx->a != i->dst) ||
           (nx->op == IR_ADD && nx->a == i->dst && nx->b != i->dst);
}

int pass_immfold(struct ir_func *fn)
{
    struct defs d;
    compute_defs(fn, &d);
    int changed = 0;
    int *use = xcalloc((size_t)(fn->nvregs ? fn->nvregs : 1), sizeof *use);
    struct ucount uc = { use, fn->nvregs };
    for (int n = 0; n < fn->nins; n++)
        each_read(&fn->ins[n], count_cb, &uc);
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        if (i->flt || i->imm_b || i->w == 16)
            continue;           /* see pass_fold on the 128-bit width */
        if (mla_keeps_reg(fn, n, use))
            continue;
        long A, B;
        int commutative;
        /* Shifts fold only their count (b), and only a valid small one — the
         * value being shifted (a) is not an immediate operand. */
        if (i->op == IR_SHL || i->op == IR_SHR) {
            if (get_const(fn, &d, i->b, &B) && B >= 0 && B <= 63) {
                i->imm = B; i->imm_b = 1; i->b = -1;
                changed = 1;
            }
            continue;
        }
        switch (i->op) {
        case IR_ADD: case IR_MUL: case IR_AND: case IR_OR: case IR_XOR:
            commutative = 1; break;
        case IR_SUB: case IR_CMP:
            commutative = 0; break;
        default:
            continue;
        }
        /* Thumb and RV32 take a 64-bit AND/OR/XOR constant half by half,
         * so its width is not x86's imm32 question (*_imm_foldable64). */
        int wide_ok = (target_get() == TARGET_THUMB ||
                       target_get() == TARGET_RISCV32 ||
                       target_get() == TARGET_MIPS32 ||
                       target_get() == TARGET_TRICORE ||
                       target_get() == TARGET_PPC32 ||
                       target_get() == TARGET_RX ||
                       target_get() == TARGET_SPARC32 ||
                       target_get() == TARGET_COLDFIRE) && i->w == 8 &&
                      (i->op == IR_AND || i->op == IR_OR || i->op == IR_XOR);
        /* ...and a 64-bit compare with any constant whose halves its
         * subs/sbcs or cmp/cmpeq take (thumb_cmp64_imm): strtol's
         * `v > LONG_MAX` kept 0x7fffffff in a register pair. Not on
         * ARMv6-M, which builds a constant from a literal pool. */
        int cmp64 = target_get() == TARGET_THUMB && i->op == IR_CMP &&
                    i->w == 8 && target_thumb_arch() >= 7 &&
                    !getenv("EMBCC_T_NOCMP64IMM");
        int p64;
        long lo64, hi64;
        if (cmp64 ? get_const(fn, &d, i->b, &B) &&
                    thumb_cmp64_imm(i->pred, i->sign, B, &p64, &lo64, &hi64)
                  : get_const(fn, &d, i->b, &B) && (fits_imm32(B) || wide_ok) &&
                    target_imm_foldable(i->op, B, i->w)) {
            i->imm = B; i->imm_b = 1; i->b = -1;    /* op a, imm */
            changed = 1;
        } else if (cmp64 ? get_const(fn, &d, i->a, &A) &&
                           thumb_cmp64_imm(swap_pred(i->pred), i->sign, A,
                                           &p64, &lo64, &hi64)
                         : (commutative || i->op == IR_CMP) &&
                           get_const(fn, &d, i->a, &A) &&
                           (fits_imm32(A) || wide_ok) &&
                           target_imm_foldable(i->op, A, i->w)) {
            /* Constant in the first operand: move it to the immediate, keeping
             * a valid instruction — commutative ops just swap, a compare swaps
             * and flips its predicate. */
            i->a = i->b; i->b = -1; i->imm = A; i->imm_b = 1;
            if (i->op == IR_CMP)
                i->pred = swap_pred(i->pred);
            changed = 1;
        }
    }
    free(use);
    free_defs(&d);
    return changed;
}
