/* IR → x86-64, System V AMD64 (ARCHITECTURE §4). Deliberately naive:
 * every vreg lives in a stack slot, every operation goes through eax.
 * Correct-and-slow first — register allocation is a post-M4 reason to
 * exist, not an M2 one (ARCHITECTURE §3).
 *
 * Slot discipline: temporaries are stored as full 8 bytes (32-bit
 * results arrive zero-extended, so the slot is always well-defined);
 * variables occupy their real size (arrays their full extent) so their
 * address points at exactly sizeof(type) meaningful bytes.
 */
#include "../backend.h"
#include "emit.h"
#include "../regalloc.h"
#include "../predef.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../driver/remark.h"
#include "../../driver/util.h"

/* -O0 and -Og (target_keep_vars): every source variable has a stack
 * location of its own, where a debugger reads it, so local-slot
 * coalescing is off. Never because of -g, which changes no code.
 * Defined here (used by coalesce_locals); codegen_unit sets it. */
static int g_keep_vars;

/* K13 — temporary stack-slot coalescing. At -O0 every temp (vreg >= nvars)
 * otherwise gets its own 8-byte slot, never reused, so a deep call chain's
 * frames overflow the kernel stack. This assigns each temp a 0-based index
 * into a SHARED pool, letting temps whose live ranges don't overlap reuse one
 * slot. *npool_out receives the pool size (distinct slots). Returns a malloc'd
 * per-temp index array (length nvregs-nvars), or NULL if there are no temps.
 *
 * Soundness. A temp is "coalescable" only when its whole live range lies within
 * ONE basic block (block ids below). For such temps a value defined at d and
 * last used at u is dead everywhere outside [d,u] within a straight-line run,
 * so two coalescable temps with disjoint [first,last] index ranges are never
 * simultaneously live — even across loop back-edges (each is reborn inside its
 * block every iteration). Temps that cross a block boundary (a `?:`/`&&`/`||`
 * result, say) are NOT coalesced: they keep a unique slot. The interval is the
 * span of EVERY appearance of the temp in ANY operand field — over-counting a
 * range only shrinks reuse, never makes it unsound, so a blind field scan (no
 * per-op operand table to get wrong) is deliberately used. Deterministic, which
 * the self-host fixed point requires. */
static int g_regalloc;          /* defined below; -O2 register allocation is on */
static const int *g_loc;        /* per-vreg physical register at -O2, or -1 */
static int g_opt_frames;        /* -O1+: dead temps take no stack slot (frame shrink) */
static int g_has_cgoto;         /* ---- -O2 register allocation ----
 *
 * Assign eligible vregs to callee-saved registers via linear scan over
 * [first,last] appearance intervals — a sound over-approximation of liveness
 * (two vregs interfere only if their intervals overlap; see coalesce_temps for
 * why the blind-scan interval is safe). A vreg is eligible only if it is a temp
 * or a scalar (int/long/pointer, size 4 or 8) local/param, and EVERY site it
 * appears at is register-aware: cg_load / cg_store / cg_load_rcx (the second
 * ALU operand), a register-aware STVAR store, and a scalar RET. Any appearance
 * at an "opaque" site — a float op, an address-of, a raw-slot atomic/memcpy/
 * store-address/va_start/call-arg, or inside inline asm — makes the vreg
 * INELIGIBLE (it stays in memory). Being conservative here is always correct;
 * a missed exclusion would silently read a stale slot, so the allocator errs
 * toward memory and the gcc-differential tests police the rest. */

/* The callee-saved GPRs the allocator may hand out (rbp/rsp excluded; none is
 * used as codegen scratch). NCALLEE is a literal so it can size arrays under
 * EmbCC's own subset (which won't fold sizeof/sizeof there). It bounds the
 * prologue-save set — a function saves at most these five. */
#define NCALLEE 5

/* A VARIADIC function's pool: the callee-saved five plus the two caller-saved
 * GPRs that are not part of the argument register file — r10, r11. r8/r9 are
 * held out because a variadic prologue saves the six integer arg registers
 * (rdi..r9) to the register-save area. The caller-saved pair come first so a
 * short-lived value prefers them and skips the prologue save. */
#define NVARIADIC 7
static const int VARIADIC_POOL[NVARIADIC] = { 10, 11, 3 /*rbx*/, 12, 13, 14, 15 };

/* Every non-variadic function's pool: the four caller-saved GPRs r8..r11 first
 * (preferred, no prologue save), then the callee-saved five. A caller-saved
 * register is only sound for a value that does NOT cross a call (a call clobbers
 * them) and, for r8/r9, is not itself a call argument (the sequential arg-setup
 * move would clobber a source still held there) — enforced by the `crosses` /
 * `is_arg` masks in colouring. A leaf function has neither constraint, so it
 * gets all nine freely. NLEAF sizes the allocator's per-colour arrays. */
#define NLEAF 10
static const int LEAF_POOL[NLEAF] = { 6 /*rsi*/, 8, 9, 10, 11,
                                      3 /*rbx*/, 12, 13, 14, 15 };
/* rdi is NOT here, and it was tried. It is argument register zero AND
 * the hidden pointer a struct return travels through, so it is written
 * at more call sites than any other -- and <format>, whose every result
 * is a std::string, failed every floating-point case with it in.
 * Emitting the sret `lea` after the parallel move (below) fixes one of
 * those writes and is kept for when the rest are found; it is not
 * enough on its own. */
/* ...without rsi, for a function with an ARMW (whose loop holds its
 * operand there when it has no register) or a CAS16 (gen_i128's). rdi
 * stays, because nothing outside the __int128 lowering uses it, and
 * x86_wants_rdi keeps it from a function that has one. */
/* ...and with rdx, for a function that neither divides nor has an
 * atomic in it. */
#define NLEAF_RDX 11
static const int LEAF_POOL_RDX[NLEAF_RDX] = { 6 /*rsi*/, 2 /*rdx*/,
                                              8, 9, 10, 11, 3 /*rbx*/,
                                              12, 13, 14, 15 };

#define NLEAF_AT 9
static const int LEAF_POOL_AT[NLEAF_AT] = { 8, 9, 10, 11,
                                            3 /*rbx*/, 12, 13, 14, 15 };

/* The FLOATING-POINT pool, which was xmm8-15 (see below for why it
 * moved). Win64 makes xmm6-15 callee-saved, and neither the pool's xmm6
 * nor the xmm7 scratch is saved under it: one of the gaps the
 * -Wwindows-abi warning names on every Windows compile. */
#define X86_FP_ALLOC 1    /* see the note below */
#define NX86_FPOOL 7
static const int X86_FPOOL[NX86_FPOOL] = { 0, 1, 2, 3, 4, 5, 6 };

/* The fixed FP registers, which are now the TOP of the file rather than
 * the bottom.
 *
 * The pool used to be xmm8-15 on the reasoning that the eight above the
 * argument file are nobody else's. That is true, and it is exactly
 * wrong: because they are nobody else's, a value in one is never where
 * the ABI wants it. Every float parameter arrived in xmm0-7 and was
 * copied up; every float result was copied back down. 1409 of the 1535
 * register-to-register movaps across lib/libc and lib/libcxx crossed
 * that boundary, and 1353 of them were xmm0 alone.
 *
 * So the pool is xmm0-7 -- the same eight registers, in the place the
 * ABI already puts things -- and the scratch roles move up out of the
 * way. Nothing here is callee-saved either way (System V preserves none
 * of the sixteen), so no prologue save or CFI rule changes. */
/* ...and the scratch is xmm7, not one of the high eight, because every
 * SSE instruction naming a register above seven carries a REX byte it
 * would not otherwise need. The scratch appears in 757 of the
 * register-to-register moves this backend emits, so putting it up there
 * gave back 757 bytes of the 2750 the swap had won. Seven pool
 * registers and a cheap scratch beat eight and an expensive one. */
#define X86_FSCR   7      /* the float scratch: every fld/fst path */
#define X86_FSCR2  15     /* a second, for the vector reduce only */
static int x86_fp_callee_saved(int reg) { (void)reg; return 0; }

/* What this function reserves, beyond what the machine does.
 *
 *   an atomic  -- each reads its operands where they live (atomic_in_reg),
 *                 and loads only one left in a slot: a compare-exchange's
 *                 desired value into rdx, ARMW's operand into rsi while
 *                 its loop builds the new value in rdx. An exchange or
 *                 fetch-add needs nothing beyond rax and rcx, which no
 *                 pool holds. CAS16 is gen_i128's, and keeps both out;
 *   a divide   -- idiv writes the rdx:rax pair, whatever the operands;
 *   variadic   -- the prologue spills the six integer argument
 *                 registers to the save area va_arg reads.
 *
 * Returns 2 when rsi and rdx are both taken, 1 for rdx alone (and sets
 * *div for a divide, which takes rdx too). rdx is in the pool for
 * everything else, which is most functions: it appears in 1.5% of the
 * instructions this backend emits. */
static int x86_reserves(const struct ir_func *fn, int *div)
{
    int at = 0;
    *div = 0;
    for (int n = 0; n < fn->nins; n++)
        switch (fn->ins[n].op) {
        case IR_ARMW: case IR_CAS16:
            at = 2; break;
        case IR_CAS: case IR_CMPXCHG:
            if (at < 1) at = 1;
            break;
        case IR_DIV: case IR_MOD:
            if (!fn->ins[n].flt) *div = 1;
            break;
        default:
            break;
        }
    return at;
}

/* May this function have rdi as well?
 *
 * The note below records rdi being tried twice and dropped twice. The
 * second time it was correct and worthless -- but that was before
 * x86_abi_hints existed, so the allocator had no reason to put
 * PARAMETER ZERO there and rdi was just a twelfth register for a
 * function that was short of five. With the hint it is the register the
 * commonest parameter in the corpus already arrives in.
 *
 * The condition is the same one: not variadic (the prologue spills the
 * argument file to the save area), not returning a struct and no call
 * returning one (the hidden sret pointer is rdi and travels outside the
 * argument sequence, which the allocator does not model), and no
 * __int128 (gen_i128 loads rdi raw). */
static int i128_ins(const struct ir_ins *i);

static int x86_wants_rdi(const struct ir_func *fn)
{
    if (fn->is_varargs || fn->ret_abi.is_struct)
        return 0;
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *in = &fn->ins[n];
        if (in->op == IR_CALL && in->retsize)
            return 0;
        if (i128_ins(in))
            return 0;
    }
    return 1;
}

/* The chosen table with rdi appended -- composed rather than tabulated,
 * because four base pools times two is eight tables to keep in step and
 * the allocator only looks at one at a time. */
static int x86_pool_buf[RA_MAXPOOL];

static const int *x86_pool_for(const struct ir_func *fn, int *n)
{
    int div, at = x86_reserves(fn, &div);
    const int *base;
    int nb;
    if (fn->is_varargs) { nb = NVARIADIC; base = VARIADIC_POOL; }
    else if (at == 2)   { nb = NLEAF_AT;  base = LEAF_POOL_AT; }
    else if (div || at) { nb = NLEAF;     base = LEAF_POOL; }
    else                { nb = NLEAF_RDX; base = LEAF_POOL_RDX; }
    if (!x86_wants_rdi(fn)) { *n = nb; return base; }
    /* After the caller-saved ones already there (it is caller-saved too,
     * so it needs no prologue save) and before the callee-saved five. */
    int k = 0, o = 0;
    while (k < nb && (base[k] == 6 || base[k] == 2))
        x86_pool_buf[o++] = base[k++];
    x86_pool_buf[o++] = REG_RDI;
    while (k < nb) x86_pool_buf[o++] = base[k++];
    *n = o;
    return x86_pool_buf;
}

/* rdi was tried AGAIN, under the narrowest condition that could be
 * stated -- not variadic, not returning a struct, no call returning
 * one, no __int128 -- so the struct-return pointer the note above
 * blames could never collide. It is correct and it is worthless: 177898
 * bytes of .text against 177849 without it, and 1081 spilled values
 * against 1072. A twelfth register does not help, because the functions
 * that spill are not short by one: strftime spills 45 of 303. Left out
 * rather than carried for nothing. */

static const int *x86_fp_pool_for(const struct ir_func *fn, int *n)
{
    (void)fn;
    *n = X86_FP_ALLOC ? NX86_FPOOL : 0;
    return X86_FPOOL;
}

/* Does a register need callee-save preservation (rbx, r12..r15)? r8..r11 are
 * caller-saved — free to clobber, so no prologue slot. */
/* The x86-64 side of the shared allocator (src/arch/regalloc.c). What
 * a machine has to say for itself is small: which registers may be
 * handed out and in what order, which survive a call, and whether a
 * narrow load is a plain move. */
static int ldvar_plain(int size, int sign, int w);
static int is_callee_saved(int reg);
static int i128_ins(const struct ir_ins *i);

/* Where System V would put each parameter if it had the choice.
 *
 * A parameter arrives in an argument register and the prologue's
 * parallel move takes it to its allocated home; that move is an
 * identity, and vanishes, when the home IS the register it arrived in.
 * Without this every function opened with `mov %rsi,%r10` and the rest
 * of its argument list.
 *
 * The walk has to track System V's two register files exactly as the
 * prologue does, or a hint lands on the wrong parameter. Rather than
 * restate the whole classification, it models the cases it is sure of
 * and STOPS at the first one it is not: a struct, whose eightbytes need
 * ty_classify, ends the walk. A hint is only a bias, so stopping early
 * costs a move rather than correctness -- but a hint derived from a
 * counter that has drifted would cost a move at every later parameter,
 * which is worse than none.
 *
 * Only System V. Win64 places by position with the files sharing one
 * counter, and its four argument registers include rcx, which is not in
 * any pool here. */
static void x86_abi_hints(const struct ir_func *fn, int *hint)
{
    struct func *f = fn->src;
    if (!f || target_win64_abi() || f->is_varargs)
        return;
    enum arg_class rcls[2];
    int ireg = 0, freg = 0;
    /* A hidden return pointer consumes rdi before any real parameter. */
    if (f->ret_ty->kind == TY_STRUCT && ty_classify(f->ret_ty, rcls) == 0 &&
        !ty_x87_ret(f->ret_ty))
        ireg++;
    for (int i = 0; i < f->nparams && i < fn->nvregs; i++) {
        struct type *pt = f->param_tys[i];
        if (pt->kind == TY_STRUCT)
            return;                      /* eightbytes: not modelled here */
        if (pt->kind == TY_LDOUBLE)
            continue;                    /* x87 class: always on the stack */
        if (pt->kind == TY_INT128) {
            if (ireg + 2 <= 6) ireg += 2;
            continue;                    /* a pair, and never allocated */
        }
        if (ty_is_float(pt)) {
            /* The FP pool IS xmm0-7 now, so a float parameter can stay
             * in the register it arrived in -- which is the whole point
             * of moving it there. The allocator reads one `hint` array
             * for both classes and a value belongs to exactly one, so a
             * 3 meaning xmm3 here cannot be mistaken for rbx. */
            if (freg < NX86_FPOOL)
                hint[i] = freg;
            freg++;
            continue;
        }
        if (ireg < 6)
            hint[i] = x86_argreg(ireg++);
    }
    /* A float RESULT, returned or received, is xmm0's. */
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *in = &fn->ins[n];
        if (in->op == IR_CALL && in->flt && in->w != 16 && !in->retsize &&
            in->dst >= 0 && in->dst < fn->nvregs)
            hint[in->dst] = 0;
        else if (in->op == IR_RET && in->flt && in->w != 16 &&
                 in->a >= 0 && in->a < fn->nvregs)
            hint[in->a] = 0;
    }


    /* ...and the other two boundaries, which are the same question asked
     * of a CALL. An integer argument is moved into its argument register
     * by the parallel move at the call site, and that move is an
     * identity when the value already lives there. A value that is an
     * argument to two calls at different positions gets the later
     * hint -- a hint is a bias, and being right at one of the two sites
     * is better than at neither.
     *
     * Nothing hints rax. A returned value and a call's result both want
     * it, and rax is the scratch every lowering path in this backend
     * uses; it is in no pool, so the hint would be dropped anyway. That
     * is the 227 `mov %rX,%rax` before a `ret` that remain. */
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *in = &fn->ins[n];
        if (in->op != IR_CALL)
            continue;
        int ireg2 = in->retsize ? 1 : 0;       /* sret consumes rdi */
        int freg2 = 0;
        for (int k = 0; k < in->nargs; k++) {
            const struct ir_arg *a = &in->argv[k];
            if (a->on_stack)
                continue;
            if (a->is_struct || a->nclass == 2) {
                for (int q = 0; q < a->nclass; q++)
                    if (a->cls[q] == CLASS_SSE) freg2++;
                    else if (a->cls[q] != CLASS_NONE) ireg2++;
                continue;
            }
            if (a->cls[0] == CLASS_SSE) {
                if (freg2 < NX86_FPOOL && a->vreg >= 0 &&
                    a->vreg < fn->nvregs)
                    hint[a->vreg] = freg2;
                freg2++;
                continue;
            }
            if (ireg2 < 6 && a->vreg >= 0 && a->vreg < fn->nvregs)
                hint[a->vreg] = x86_argreg(ireg2);
            ireg2++;
        }
    }
}



static const struct ra_target X86_RA = {
    x86_pool_for,
    is_callee_saved,
    ldvar_plain,
    1, 1, 1,       /* this backend reads call arguments, scalar returns
                    * and memcpy addresses straight out of a register */
    i128_ins,      /* __int128 multiply, divide, remainder, shift and
                    * the float conversions are libgcc's, emitted here
                    * with no IR_CALL for `crosses` to find. Saying so
                    * is what let the blunt refusal below go. */
    1,             /* two-operand ALU: `addsd d, b` means `d += b`, so d
                    * and a want one register -- worth coalescing. */
    x86_abi_hints  /* ...and where SysV would put each value if it had
                    * the choice. The old note below said the same three
                    * boundaries exist here as on aarch64 and were not
                    * yet described; they are, for parameters.
                    * here -- a parameter's register, rax for a call's
                    * result and for a return -- and none of rdi/rsi/rax
                    * is in this backend's pool, so a hint naming one
                    * would never match. That changes if the pool ever
                    * grows to them. */,
    x86_fp_pool_for, x86_fp_callee_saved,
    0,            /* float_in_gpr: floats have their own class (SSE) */
    NULL, NULL,
    1, /* atomic_in_reg: every atomic but CAS16 reads its address and
        * values where they live, and loads only one left in a slot
        * (x86_atomic_addr, x86_atomic_val, cg_load) */
    0, /* fp_reads_gpr */
    0, /* asm_in_reg: a template may name a callee-saved register */
    NULL, /* remat_ok */
    NULL  /* call_target_in_reg */
};

/* ---- long double: 16-byte values and the x87 unit ----
 *
 * A long double is x87 80-bit extended, stored in 16 bytes. Its temps are
 * the only values wider than a register: each gets a 16-byte slot of its
 * own (never the 8-byte coalesced pool, never a register), is moved as two
 * eightbytes, and is computed on the x87 stack — loaded, operated on, and
 * stored straight back, so the stack is empty between IR instructions and
 * at every call. g_wide marks the vregs that hold one: a long double local,
 * the dst of any op that produces one (w == 16), and a MOV of such a value
 * (to a fixpoint, since a ?: joins through MOVs). */
static char *g_wide;

/* The float/double vregs (cg_float_vregs) and where the allocator put
 * each: an xmm register, or -1 for its stack slot. */
static char *g_flt;
static int *g_floc;
static int in_freg(int v) { return g_floc && v >= 0 && g_floc[v] >= 0; }
static int is_flt(int v)  { return g_flt && v >= 0 && g_flt[v]; }

static int wide_def(const struct ir_ins *i)
{
    switch (i->op) {
    case IR_LOAD: case IR_LDVAR:
        return i->size == 16;
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_NEG:
    case IR_CALL:
        return i->w == 16;          /* (a long double, or an __int128) */
    case IR_I2F: case IR_F2F:
        return i->w == 16;
    /* __int128: its ops at width 16, an extension to it, a conversion */
    case IR_CONST: case IR_MOD: case IR_AND: case IR_OR: case IR_XOR:
    case IR_SHL: case IR_SHR: case IR_BNOT: case IR_EXT: case IR_F2I:
        return !i->flt && i->w == 16;
    case IR_CAS16:
        return 1;
    /* A vector result is 16 bytes, so it wants the same 16-aligned slot
     * a long double gets and the same exclusion from the integer
     * register allocator. `wide` is already exactly that map. */
    case IR_VLOAD: case IR_VBIN: case IR_VSPLAT: case IR_VWIDEN:
        return 1;
    default:
        return 0;
    }
}

char *cg_wide_vregs(struct ir_func *fn)
{
    int nv = fn->nvregs ? fn->nvregs : 1;
    char *w = xcalloc((size_t)nv, 1);
    int any = 0;
    for (int v = 0; v < fn->nvars; v++)
        if (fn->locals[v].is_ldouble || fn->locals[v].is_int128)
            w[v] = any = 1;
    for (int n = 0; n < fn->nins; n++)
        if (wide_def(&fn->ins[n]) && fn->ins[n].dst >= 0)
            w[fn->ins[n].dst] = any = 1;
    /* A sixteen-byte value carries its width through every COPY, not
     * just IR_MOV: `ldvar`/`stvar` of one are equally a copy, and
     * gen_x87 lowers all three the same way. Missing those left a vreg
     * that a 16-byte copy WRITES looking like an ordinary eight-byte
     * temp -- so the allocator was free to put it in a register and the
     * copy wrote sixteen bytes over whatever slot it had been given
     * instead. lib/libc/src/math/widths.c does exactly that. */
    for (int changed = any; changed; ) {
        changed = 0;
        for (int n = 0; n < fn->nins; n++) {
            struct ir_ins *i = &fn->ins[n];
            int a, d;
            switch (i->op) {
            case IR_MOV: case IR_LDVAR: case IR_STVAR:
                a = i->a; d = i->dst; break;
            default: continue;
            }
            if (a < 0 || d < 0 || a >= nv || d >= nv) continue;
            if (w[a] && !w[d]) { w[d] = 1; changed = 1; }
            if (w[d] && !w[a]) { w[a] = 1; changed = 1; }
        }
    }
    if (!any) { free(w); return NULL; }
    return w;
}

/* The vregs that hold a FLOATING-POINT value -- a float or a double, not
 * a long double, which is x87 or a 16-byte slot and is `wide` instead.
 *
 * Float-ness is not written on every instruction that carries one. An
 * op that COMPUTES in floating point says so (ir_ins::flt), and a
 * conversion says which side is which, but a `ldvar` of a double, a
 * `mov` joining the arms of a `?:`, and a `load` through a pointer all
 * look exactly like their integer selves. So this starts from the
 * places float-ness IS stated and closes over the copies, the same
 * fixpoint cg_wide_vregs does and for the same reason.
 *
 * A compare is the one op whose operands and result disagree: it reads
 * two floats and produces a 0/1 integer. */
/* String sites carry the string INDEX while a function is being lowered,
 * because that is what the IR's IR_STRADDR holds; the driver's relocations
 * want the OFFSET in .rodata. Every backend owes this conversion once its
 * unit is done.
 *
 * It lives here, shared, rather than as the same line in each backend.
 * Four of them had that line and the fifth did not, and the symptom was
 * three strings away from the cause: string index N resolved to
 * rodata + N, so `puts_("DONE\n")` printed "cXYZ" out of the middle of
 * the previous literal and a digit-summing loop over "0123456789" got 224.
 * Nothing faulted and every address was inside .rodata. */
void cg_resolve_strsites(struct ir_unit *iu, struct strsite *s, int n)
{
    for (int k = 0; k < n; k++) {
        /* RK_AVR_TEXT_CALL names a label in .text and its str_off is ALREADY
         * an offset -- a jump too far for AVR's 12-bit rjmp, relocated
         * against the section symbol. Everything else here is a string index
         * into the unit's pool. */
        if (s[k].kind == RK_AVR_TEXT_CALL || s[k].kind == RK_MIPS_TEXT26 ||
            s[k].kind == RK_XTENSA_TEXT32)
            continue;
        s[k].str_off = iu->strs[s[k].str_off].off;
    }
}

/* Is a call from `caller` to `callee` resolved here, as a displacement
 * within one section -- or left to the linker, as a relocation? Only a
 * callee defined in this unit AND placed in the same section can be:
 * a function with a section attribute is laid out apart from .text, and
 * the distance between two sections is the linker's to decide. Shared,
 * like cg_resolve_strsites (AVR relocates every call anyway).
 *
 * Never a WEAK one: the definition here is a default the link may
 * replace, and a call bound to it now still runs the default after the
 * program has supplied its own -- libc's bare-metal write() was, and a
 * program's printf went nowhere. */
int cg_call_local(const struct func *caller, const struct func *callee)
{
    if (!callee->has_defn || callee->is_weak)
        return 0;
    const char *a = caller && caller->section ? caller->section : "";
    const char *b = callee->section ? callee->section : "";
    return strcmp(a, b) == 0;
}

int cg_label_mark(const struct ir_ins *i)
{
    return i->op == IR_LABELADDR && i->vol;
}

void cg_note_labels(struct ir_func *fn, const int *label_off)
{
    struct func *f = fn->src;
    if (!f || !f->label_pos)
        return;
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        if (!cg_label_mark(i) || i->imm < 0 || i->imm >= f->nlabel_pos)
            continue;
        if (i->label < 0 || i->label >= fn->nlabels || label_off[i->label] < 0)
            internal_error("%s: a label static data takes the address of "
                           "was never placed", fn->name);
        f->label_pos[i->imm] =
            (long)label_off[i->label] - (f->code_off + f->code_entry);
    }
}

/* Can this load, store, ldvar or stvar move its value as a float or a
 * double? (cg_float_vregs) */
static int flt_width(const struct ir_ins *i)
{
    if (i->size != 4 && i->size != 8)
        return 0;
    if (i->op == IR_LOAD || i->op == IR_LDVAR)
        return !(i->sign && i->size < i->w);
    return 1;
}

struct flt_bad { char *bad; int *soft; int nv; };
static void flt_bad_cb(int v, void *ctx)
{
    struct flt_bad *b = ctx;
    if (v >= 0 && v < b->nv) {
        if (b->soft) b->soft[v]++;
        else b->bad[v] = 1;
    }
}

static int flt_find(int *uf, int x)
{
    while (uf[x] != x) { uf[x] = uf[uf[x]]; x = uf[x]; }
    return x;
}

static char *float_vregs(struct ir_func *fn, int by_cost);

char *cg_float_vregs(struct ir_func *fn)
{
    return float_vregs(fn, 0);
}

/* The same classes, decided by cost where a value is touched both ways:
 * for a backend whose integer lowering can also reach a value at home in
 * an FP register (one fmov) -- AArch64. An integer use that can only be
 * made from a general register (an address, a narrow access, a narrow
 * result) still decides for the integer class; one that an fmov serves
 * is a vote, and the floating-point uses are the other votes. fdlibm's
 * `x` is read by a dozen float operations and by the one shift that
 * takes its high word: it belongs in a d register, with one fmov out. */
char *cg_float_vregs_by_cost(struct ir_func *fn)
{
    return float_vregs(fn, 1);
}

static char *float_vregs(struct ir_func *fn, int by_cost)
{
    int nv = fn->nvregs ? fn->nvregs : 1;
    char *w = xcalloc((size_t)nv, 1);
    char *wide = cg_wide_vregs(fn);
    int any = 0;
    /* by_cost: each value's floating-point uses, the votes for its class */
    int *fcnt = by_cost ? xcalloc((size_t)nv, sizeof *fcnt) : NULL;
#define MARK(v) do { int _v=(v); if (_v>=0 && _v<nv) { if (fcnt) fcnt[_v]++; \
                     if (!w[_v]) { w[_v]=1; any=1; } } } while (0)
    for (int v = 0; v < fn->nvars; v++)
        if (fn->locals[v].is_scalar_float) MARK(v);
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        if (i->flt) {
            if (i->op == IR_CMP) { MARK(i->a); MARK(i->b); }
            else { MARK(i->dst); MARK(i->a); MARK(i->b); }
        }
        switch (i->op) {
        case IR_I2F:  MARK(i->dst); break;
        case IR_F2I:  MARK(i->a);   break;
        /* A bitcast has a float on exactly one side: `sign` says
         * which, so only that side joins the float class. */
        case IR_BITCAST:
            if (i->sign) MARK(i->a); else MARK(i->dst);
            break;
        case IR_F2F:  MARK(i->dst); MARK(i->a); break;
        case IR_SQRT: MARK(i->dst); MARK(i->a); break;
        default: break;
        }
    }
    for (int changed = any; changed; ) {
        changed = 0;
        for (int n = 0; n < fn->nins; n++) {
            struct ir_ins *i = &fn->ins[n];
            int a = -1, d = -1;
            switch (i->op) {
            case IR_MOV:   a = i->a; d = i->dst; break;
            case IR_LDVAR: a = i->a; d = i->dst; break;
            case IR_STVAR: a = i->a; d = i->dst; break;
            default: break;
            }
            if (a < 0 || d < 0 || a >= nv || d >= nv) continue;
            /* a copy carries float-ness both ways */
            if (w[a] && !w[d]) { w[d] = 1; changed = 1; }
            if (w[d] && !w[a]) { w[a] = 1; changed = 1; }
        }
    }
    /* ---- and now take back everything an INTEGER op touches --------
     *
     * Float-ness is not a property of a value here, it is a property of
     * each USE, and the two disagree more often than the arithmetic
     * suggests. A floating-point constant is an ordinary `const` of its
     * bit pattern -- `const.8s 4602678819172646912` is 0.5 -- and
     * negating a double is an integer XOR of the sign bit, not `fneg`.
     * A value reaching either of those has to be in a general register
     * when it gets there.
     *
     * So the mark above is only a candidate, and this is the whitelist:
     * a value stays in this class only if EVERY instruction that touches
     * it treats it as floating point. Anything else takes it back, and
     * the taking-back travels through the copies for the same reason the
     * marking did -- both ends of a `mov` are one value and cannot be in
     * two classes. */
    char *bad = xcalloc((size_t)nv, 1);
    /* by_cost: the integer uses an fmov can serve, counted, not decided */
    int *soft = by_cost ? xcalloc((size_t)nv, sizeof *soft) : NULL;
    struct flt_bad fb = { bad, soft, nv };
    void *bad_ctx = &fb;
#define BAD(v) do { int _v=(v); if (_v>=0 && _v<nv) bad[_v]=1; } while (0)
#define SOFT(v) do { int _v=(v); if (_v>=0 && _v<nv) { \
                     if (soft) soft[_v]++; else bad[_v]=1; } } while (0)
    /* A parameter arrives where its TYPE puts it: an integer one in a
     * general register, whatever its later uses. `double bits(long x) {
     * union { long l; double d; } u; u.l = x; return u.d; }` folds to
     * returning x as a double, which marked x float -- and an integer
     * parameter in the float class had no home the prologue could fill:
     * an internal error at -O1. In the integer class its float uses
     * move it with movq (x86_fld). */
    if (fn->src)
        for (int p = 0; p < fn->nparams && p < nv; p++) {
            const struct type *pt = fn->src->param_tys[p];
            if (pt && !ty_is_float(pt))
                BAD(p);
        }
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        if (i->flt) {
            if (i->op == IR_CMP) BAD(i->dst);     /* a 0/1 integer */
            continue;                             /* operands are float */
        }
        switch (i->op) {
        case IR_CONST:
            /* A constant is whatever its consumer needs it to be. The
             * bits of 0.5 are `const.8s 4602678819172646912` and nothing
             * says float, so the default below took it out of the class
             * -- and the taking-back travels through the copies, so one
             * literal put every value reachable from it in memory, in
             * BOTH classes. An op that wants it as an integer still
             * BADs it by its own rule, so the whitelist stays sound. */
            break;
        case IR_I2F:  SOFT(i->a);   break;        /* integer in */
        case IR_F2I:                              /* integer out */
            if (i->w == 8) SOFT(i->dst); else BAD(i->dst);
            break;
        case IR_BITCAST:
            if (i->sign) SOFT(i->dst); else SOFT(i->a);
            break;
        case IR_F2F:
            /* A conversion with a LONG DOUBLE on either side goes
             * through the x87 unit, which loads and stores memory and
             * nothing else -- so the float/double side of it has to be
             * in a slot for x87 to reach. tests/exec/long-double.c:
             * `(double)(one + tiny)`. */
            if (i->w == 16 || i->size == 16) { BAD(i->dst); BAD(i->a); }
            break;
        case IR_SQRT: break;
        case IR_MOV: break;                       /* a copy */
        /* A memory access moves a float or a double as movss/movsd (ldr
         * s/d): four or eight bytes. One of another width is an INTEGER
         * access -- `r->h = 0` where the same `const 0` is also the 0.0f
         * of `f * 0.0f` -- and from an FP register it stored eight bytes
         * into a two-byte field. A signed widening read is integer too:
         * movss fills the high half with zeros, not the sign. */
        case IR_LDVAR: case IR_STVAR:             /* copies, unless narrow */
            if (!flt_width(i)) { BAD(i->a); BAD(i->dst); }
            break;
        case IR_LOAD:
            BAD(i->a);                            /* the address */
            if (!flt_width(i)) BAD(i->dst);
            break;
        case IR_STORE:
            BAD(i->a);                            /* the address */
            if (!flt_width(i)) BAD(i->b);
            break;
        /* A floating-point return is `flt` and never reaches here; this
         * one hands its value back in rax (x0), so the value has to be
         * in a general register. */
        case IR_RET:   SOFT(i->a); break;
        case IR_CALL:
            if (i->indirect) BAD(i->a);
            for (int k = 0; k < i->nargs; k++)
                if (i->argv[k].cls[0] != CLASS_SSE) SOFT(i->argv[k].vreg);
            if (!i->flt) {
                if (i->w == 8 && !i->retsize) SOFT(i->dst); else BAD(i->dst);
            }
            break;
        default:
            /* every other op is integer in and integer out -- in the
             * operands it has, which ra_each_use knows. The fields
             * themselves do not say: a two-operand op's `c` is 0, not
             * -1, so `and.4 %x, #1` or an `ext` anywhere in a function
             * took vreg 0 -- the first parameter -- out of the float
             * class, and a double argument went through memory. */
            /* a result narrower than eight bytes is not something an
             * fmov puts in a d register whole */
            if (i->w == 8) SOFT(i->dst); else BAD(i->dst);
            ra_each_use(i, flt_bad_cb, bad_ctx);
            break;
        }
    }
#undef BAD
#undef SOFT
    if (by_cost) {
        /* A copy's two ends are one value: the votes are pooled over the
         * copies, and a hard integer use anywhere decides for all. */
        int *uf = xmalloc((size_t)nv * sizeof *uf);
        for (int v = 0; v < nv; v++) uf[v] = v;
        for (int n = 0; n < fn->nins; n++) {
            const struct ir_ins *i = &fn->ins[n];
            if ((i->op == IR_MOV || i->op == IR_LDVAR || i->op == IR_STVAR) &&
                i->a >= 0 && i->a < nv && i->dst >= 0 && i->dst < nv) {
                int x = flt_find(uf, i->a), y = flt_find(uf, i->dst);
                if (x != y) uf[x] = y;
            }
        }
        long *fs = xcalloc((size_t)nv, sizeof *fs);
        long *is = xcalloc((size_t)nv, sizeof *is);
        char *hard = xcalloc((size_t)nv, 1);
        for (int v = 0; v < nv; v++) {
            int r = flt_find(uf, v);
            fs[r] += fcnt[v];
            is[r] += soft[v];
            hard[r] |= bad[v];
        }
        for (int v = 0; v < nv; v++) {
            int r = flt_find(uf, v);
            bad[v] = hard[r] || (is[r] > 0 && fs[r] < is[r]);
        }
        free(uf); free(fs); free(is); free(hard);
        free(fcnt); free(soft);
    }
    for (int changed = 1; changed; ) {
        changed = 0;
        for (int n = 0; n < fn->nins; n++) {
            struct ir_ins *i = &fn->ins[n];
            int a, d;
            switch (i->op) {
            case IR_MOV: case IR_LDVAR: case IR_STVAR:
                a = i->a; d = i->dst; break;
            default: continue;
            }
            if (a < 0 || d < 0 || a >= nv || d >= nv) continue;
            if (bad[a] && !bad[d]) { bad[d] = 1; changed = 1; }
            if (bad[d] && !bad[a]) { bad[a] = 1; changed = 1; }
        }
    }
    any = 0;
    for (int v = 0; v < nv; v++) {
        if (bad[v]) w[v] = 0;
        if (w[v]) any = 1;
    }
    free(bad);

    /* A long double is not this class: it is sixteen bytes and has its
     * own slot. Whatever marked one, unmark it. */
    if (wide)
        for (int v = 0; v < nv; v++)
            if (wide[v]) w[v] = 0;
    free(wide);
#undef MARK
    if (!any) { free(w); return NULL; }
    return w;
}

/* Is this instruction a 16-byte (long double) one, lowered by gen_x87? */
static int x87_ins(const struct ir_ins *i)
{
    switch (i->op) {
    case IR_LOAD: case IR_LDVAR: case IR_STORE: case IR_STVAR:
        return i->size == 16;
    case IR_MOV:
        return g_wide && i->a >= 0 && g_wide[i->a];
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

static int is_callee_saved(int reg)
{
    return reg == 3 || (reg >= 12 && reg <= 15);
}


/* Is an IR_LDVAR (dst = extend(local a)) a PLAIN register move — no sign/zero
 * extension emitted — so dst and a may share a register (and the load vanish)?
 * True for a full 8-byte load, or a 4-byte load that isn't a signed widen to 64
 * (a narrow char/short load always movsx/movzx-extends, so never plain). */
static int ldvar_plain(int size, int sign, int w)
{
    return size == 8 || (size == 4 && !(sign && w == 8));
}


/* Coalesce local stack SLOTS by lexical scope: locals whose scope ranges are
 * disjoint never coexist, so they share a slot (gcc does the same — a stack
 * pointer used past its scope is UB). Returns a per-local slot id [0..*nslots),
 * assigned by interval-graph colouring in scope-start order (optimal for
 * intervals). Params and function-level locals span the whole function, so they
 * interfere with everything and never coalesce. -g disables it (so each source
 * variable keeps a distinct DWARF location). Length nvars; caller frees. */
static int *coalesce_locals(struct ir_func *fn, int *nslots_out)
{
    int n = fn->nvars;
    int *slot = xmalloc((size_t)(n ? n : 1) * sizeof *slot);
    if (n == 0 || g_keep_vars || g_has_cgoto || !fn->var_scope_lo) {
        for (int i = 0; i < n; i++) slot[i] = i;   /* one slot each */
        *nslots_out = n;
        return slot;
    }
    int nins = fn->nins;

    /* Per-local lifetime range [rlo, rhi): an ADDRESS-TAKEN local (its address
     * could reach a pointer we don't track) is bounded by its lexical SCOPE
     * (sound — a stack pointer past its scope is UB); a non-address-taken local,
     * accessed only by direct LDVAR/STVAR, uses its precise LIVENESS range,
     * which is tighter and lets two same-scope locals with disjoint lifetimes
     * share a slot (gcc does the same). */
    char *at = xcalloc((size_t)n, 1);
    for (int i = 0; i < nins; i++)
        if (fn->ins[i].op == IR_ADDR) {
            int v = fn->ins[i].a;
            if (v >= 0 && v < n) at[v] = 1;
        }
    int *lf = xmalloc((size_t)fn->nvregs * sizeof *lf);
    int *ll = xmalloc((size_t)fn->nvregs * sizeof *ll);
    ra_live_ranges(fn, lf, ll);

    int *rlo = xmalloc((size_t)n * sizeof *rlo);
    int *rhi = xmalloc((size_t)n * sizeof *rhi);
    for (int i = 0; i < n; i++) {
        if (at[i] || lf[i] < 0) {           /* scope-bounded (or never referenced) */
            rlo[i] = fn->var_scope_lo[i];
            rhi[i] = fn->var_scope_hi[i];
        } else {                             /* tighter: precise liveness */
            rlo[i] = lf[i];
            rhi[i] = ll[i] + 1;              /* half-open */
        }
        /* A parameter is written by the prologue, before instruction 0,
         * whether or not its incoming value is ever read -- so its slot is
         * in use from the entry. Liveness started a parameter that is only
         * assigned at its first assignment, and `u8 a3` was given the slot
         * of `u64 a1`: the prologue's store of a3 overwrote a1 before the
         * body read it (random programs, x86-64 -O0). */
        if (i < fn->nparams)
            rlo[i] = 0;
    }
    free(at); free(lf); free(ll);

    /* interval-graph colouring in range-start order (optimal for intervals):
     * reuse a slot once its occupant's range ends at or before this one starts. */
    int *head = xmalloc((size_t)(nins + 2) * sizeof *head);
    for (int i = 0; i <= nins + 1; i++) head[i] = -1;
    int *nxt = xmalloc((size_t)n * sizeof *nxt);
    for (int i = n - 1; i >= 0; i--) {
        int b = rlo[i]; if (b < 0) b = 0; if (b > nins + 1) b = nins + 1;
        nxt[i] = head[b]; head[b] = i;
    }
    int *slot_free = xmalloc((size_t)n * sizeof *slot_free);
    int ns = 0;
    for (int b = 0; b <= nins + 1; b++)
        for (int i = head[b]; i >= 0; i = nxt[i]) {
            int pick = -1;
            for (int s = 0; s < ns; s++)
                if (slot_free[s] <= rlo[i]) { pick = s; break; }
            if (pick < 0) { pick = ns++; }
            slot[i] = pick;
            slot_free[pick] = rhi[i];
        }
    *nslots_out = ns;
    free(head); free(nxt); free(slot_free); free(rlo); free(rhi);
    return slot;
}

/* Frame layout: variables first (their slots coalesced by scope), then a
 * coalesced pool of 8-byte temporary slots (K13). Returns the per-vreg
 * displacement table (caller frees). */

/* What a dead slot's displacement is set to. It is never addressed -- so
 * if it ever is, this makes that a fault at the first access instead of
 * a silent read of whatever the frame happens to hold there. THE RULE,
 * applied to an offset. */
#define DEAD_SLOT_OFF (-0x40000000)

/* Is parameter p a struct that System V passes in MEMORY? It arrives in
 * the caller's outgoing area, which belongs to this function for the
 * length of the call -- gcc and clang use it as the parameter's home, and
 * so does this: the prologue used to copy it into the frame eight bytes
 * at a time, 168 bytes for every EmbLinkOs widget call's EmProps. Not
 * at -O0 and -Og, where its DWARF home is a frame offset, nor on Win64, which
 * passes such an aggregate by reference. */
static int x86_param_home_incoming(const struct func *f, int p)
{
    enum arg_class cls[2];
    if (target_win64_abi() || g_keep_vars || !f ||
        p < 0 || p >= f->nparams || !f->param_tys[p])
        return 0;
    return f->param_tys[p]->kind == TY_STRUCT &&
           ty_classify(f->param_tys[p], cls) == 0;
}

/* ---- a struct argument built where the callee will read it ------------
 *
 * A struct passed by value in MEMORY travels in the outgoing area at
 * [rsp + stk_off], and the call copies it there from wherever it was
 * built. For a compound literal -- `f((Props){ .x = 1 })`, every widget
 * call in EmbLinkOs's UI -- that wherever is a temporary local made for
 * the purpose: zeroed, its fields stored, copied, and never read again.
 * gcc builds it in the outgoing area directly. So does this, for a local
 * whose one `addr` reaches nothing but its own loads, stores, clears and
 * copies and that one argument of that one call, all in the call's block
 * and before it, with nothing between its first write and the call that
 * could write the outgoing area -- another call, asm, an alloca, or an
 * operation lowered through a helper. Two such locals whose lives overlap
 * would share the area, so the later one keeps its own slot.
 *
 * The frame is fixed (no alloca), so rsp is rbp - frame at every call and
 * the local's slot is simply rbp - frame + stk_off. Its address then IS
 * the destination, and the call's copy, which compares them, is skipped.
 * Returns, per local, that stk_off, or -1; NULL when there is none. */
static int *x86_inplace_locals(struct ir_func *fn)
{
    struct func *f = fn->src;
    int nv = fn->nvregs, nvars = fn->nvars;
    if (!g_regalloc || g_keep_vars || fn->has_alloca || target_win64_abi() ||
        !f || nvars == 0 || nv == 0)
        return NULL;
    int *res = NULL;
    int *ndef = xcalloc((size_t)nv, sizeof *ndef);
    int *defn = xmalloc((size_t)nv * sizeof *defn);
    int *naddr = xcalloc((size_t)nvars, sizeof *naddr);
    for (int n = 0; n < fn->nins; n++) {
        const struct ir_ins *i = &fn->ins[n];
        int d = ra_ins_def(i);
        if (d >= 0 && d < nv) { ndef[d]++; defn[d] = n; }
        if (i->op == IR_ADDR && i->a >= 0 && i->a < nvars)
            naddr[i->a]++;
        if ((i->op == IR_LDVAR && i->a >= 0 && i->a < nvars) ||
            (i->op == IR_STVAR && i->dst >= 0 && i->dst < nvars))
            naddr[i->op == IR_LDVAR ? i->a : i->dst] += 2;   /* named directly */
    }
    char *der = xcalloc((size_t)nv, 1);
    /* The last call given an in-place argument, and the one before it.
     * Calls are visited in order, so a window for a LATER call must open
     * after the last one, and another window for the SAME call (its
     * arguments sit at different offsets) after the one before that. */
    int last_call = -1, prev_call = -1;
    for (int c = 0; c < fn->nins; c++) {
        const struct ir_ins *call = &fn->ins[c];
        if (call->op != IR_CALL)
            continue;
        for (int k = 0; k < call->nargs; k++) {
            const struct ir_arg *a = &call->argv[k];
            int p = a->vreg;
            if (!a->on_stack || !a->is_struct || p < nvars || p >= nv ||
                ndef[p] != 1 || fn->ins[defn[p]].op != IR_ADDR)
                continue;
            int X = fn->ins[defn[p]].a;
            if (X < 0 || X >= nvars || naddr[X] != 1 || !f->var_tys ||
                !f->var_tys[X] || ty_size(f->var_tys[X]) != a->size)
                continue;
            int al = f->var_aligns ? f->var_aligns[X] : 0;
            if (ty_align(f->var_tys[X]) > al) al = ty_align(f->var_tys[X]);
            if (al > 16 || (al > 0 && a->stk_off % al) || (res && res[X] >= 0))
                continue;
            /* the address and its constant offsets, and every use */
            memset(der, 0, (size_t)nv);
            der[p] = 1;
            /* `first` is the first access THROUGH it: the `addr` itself
             * touches nothing, and LICM hoists it to the entry block,
             * away from the clear and the stores in a loop body. */
            int ok = 1, first = -1;
            for (int n = 0; n < fn->nins && ok; n++) {
                const struct ir_ins *i = &fn->ins[n];
                int ra = i->a >= 0 && i->a < nv && der[i->a];
                int rb = !i->imm_b && i->b >= 0 && i->b < nv && der[i->b];
                int rc = i->c >= 0 && i->c < nv && der[i->c];
                if (n == defn[p])
                    continue;
                if (i->op == IR_ADD && i->imm_b && ra && i->dst >= 0 &&
                    i->dst < nv && ndef[i->dst] == 1) {
                    der[i->dst] = 1;
                } else if (i->op == IR_CALL) {
                    for (int q = 0; q < i->nargs; q++)
                        if (i->argv[q].vreg >= 0 && i->argv[q].vreg < nv &&
                            der[i->argv[q].vreg] && !(n == c && q == k))
                            ok = 0;
                    if (i->indirect && ra)
                        ok = 0;
                    continue;
                } else if ((i->op == IR_LOAD || i->op == IR_MEMZERO) && !rb && !rc) {
                    /* through it: fine; it is checked for place below */
                } else if (i->op == IR_STORE && !rb && !rc) {
                } else if (i->op == IR_MEMCPY && !rc) {
                } else {
                    if (ra || rb || rc)
                        ok = 0;
                    continue;
                }
                if (ra || rb) {
                    if (n > c)
                        ok = 0;                 /* used after the call */
                    else if (first < 0 || n < first)
                        first = n;
                }
            }
            if (first < 0)
                first = c;
            if (!ok || first <= (c == last_call ? prev_call : last_call))
                continue;
            /* between the first access and the call: one block, and
             * nothing that could write the outgoing area */
            for (int n = first; n < c && ok; n++) {
                const struct ir_ins *i = &fn->ins[n];
                switch (i->op) {
                case IR_LABEL: case IR_CALL: case IR_ASM: case IR_ALLOCA:
                case IR_SPSAVE: case IR_SPRESTORE: case IR_VA_START:
                case IR_LANDING: case IR_JMP: case IR_BRZ: case IR_BRNZ:
                case IR_SWITCH: case IR_IGOTO: case IR_RET: case IR_UD2:
                    ok = 0;
                    break;
                default:
                    if (i128_ins(i) || x87_ins(i))
                        ok = 0;
                    break;
                }
            }
            if (!ok)
                continue;
            if (!res) {
                res = xmalloc((size_t)nvars * sizeof *res);
                for (int v = 0; v < nvars; v++)
                    res[v] = -1;
            }
            res[X] = a->stk_off;
            if (c != last_call) {
                prev_call = last_call;
                last_call = c;
            }
        }
    }
    free(ndef); free(defn); free(naddr); free(der);
    return res;
}

static int *layout_frame(struct ir_func *fn, int *frame_out,
                         int *scratch_base_out, int *sret_slot_out,
                         int *va_save_out, int *va_tag_out,
                         int nsave, int *save_base_out, const int *loc,
                         const int *inplace)
{
    struct func *f = fn->src;
    int *disp = xmalloc((size_t)(fn->nvregs ? fn->nvregs : 1)
                        * sizeof *disp);
    int running = 0;
    enum arg_class rcls[2];

    /* -O2: a slot per callee-saved register the allocator uses, saved in the
     * prologue and restored before every epilogue. Slot k is at save_base+k*8. */
    *save_base_out = 0;
    if (nsave > 0) {
        running += nsave * 8;
        *save_base_out = -running;
    }

    /* A function returning a MEMORY-class struct is handed a hidden
     * pointer in rdi; it must survive until the return, so it gets a
     * slot of its own. */
    *sret_slot_out = 0;
    if (f->ret_ty->kind == TY_STRUCT && ty_classify(f->ret_ty, rcls) == 0 &&
        !ty_x87_ret(f->ret_ty)) {
        running += 8;
        *sret_slot_out = -running;
    }

    /* Locals share slots when their scopes are disjoint (coalesce_locals). Each
     * slot is sized to its largest occupant and aligned to the strictest one. */
    int nls = 0;
    int *lslot = coalesce_locals(fn, &nls);
    /* A local no instruction names needs no stack, whether or not it
     * could live in a register: the other half of slot_dead's question,
     * and what SROA leaves behind once a split aggregate is mentioned
     * nowhere (regalloc.h). */
    char *lref = ra_locals_referenced(fn, g_keep_vars);
    int *ssize = xcalloc((size_t)(nls ? nls : 1), sizeof *ssize);
    int *salign = xcalloc((size_t)(nls ? nls : 1), sizeof *salign);
    for (int i = 0; i < fn->nvars; i++) {
        int s = lslot[i];
        if (!lref[i] || ra_slot_dead(fn, loc, g_floc, i, g_keep_vars))
            continue;               /* in a register, or named nowhere at all */
        if (inplace && inplace[i] >= 0)
            continue;               /* in the outgoing area: placed below */
        if (i < fn->nparams && x86_param_home_incoming(f, i))
            continue;               /* in the caller's: the prologue says where */
        int sz = (ty_size(f->var_tys[i]) + 7) & ~7;
        if (sz > ssize[s]) ssize[s] = sz;
        /* Alignment of a local's stack slot: the greater of its type's natural
         * alignment (a struct with an aligned(16) member, e.g. struct thread's
         * fpu_state, is itself 16-aligned) and any __attribute__((aligned(N)))
         * on the declarator. */
        int al = f->var_aligns ? f->var_aligns[i] : 0;
        int tal = ty_align(f->var_tys[i]);
        if (tal > al) al = tal;
        if (al > salign[s]) salign[s] = al;
    }
    int *soff = xmalloc((size_t)(nls ? nls : 1) * sizeof *soff);
    for (int s = 0; s < nls; s++) {
        if (ssize[s] == 0) {        /* every local in it lives in a register */
            soff[s] = DEAD_SLOT_OFF;
            continue;
        }
        running += ssize[s];
        /* rbp is 16-aligned on entry, so rounding `running` up to N makes the
         * slot base rbp-running N-aligned for N <= 16; a larger request would
         * need dynamic realignment, so refuse loudly (THE RULE). */
        int al = salign[s];
        if (al > 1) {
            if (al > 16)
                diag_fatal(f->file, f->line,
                           "a local in '%s' needs %d-byte alignment, exceeding "
                           "the 16-byte stack alignment EmbCC can guarantee",
                           f->name, al);
            running = (running + al - 1) & ~(al - 1);
        }
        soff[s] = -running;
    }
    for (int i = 0; i < fn->nvars; i++)
        disp[i] = !lref[i] || ra_slot_dead(fn, loc, g_floc, i, g_keep_vars) ? DEAD_SLOT_OFF
                                                    : soff[lslot[i]];
    /* A value with an FP home has no slot either, and saying so out loud
     * is how the paths that do not know about the class get found. */
    for (int i = 0; i < fn->nvregs; i++)
        if (g_floc && g_floc[i] >= 0) disp[i] = DEAD_SLOT_OFF;
    free(lslot); free(lref); free(ssize); free(salign); free(soff);
    /* Temporaries share a coalesced pool of 8-byte slots (K13) instead of one
     * slot each — the temp region is `npool` slots wide, not (nvregs-nvars). */
    int npool = 0;
    struct ra_slots so = { g_regalloc ? g_loc : NULL, g_floc,
                           g_opt_frames, g_has_cgoto };
    int *tslot = ra_coalesce_temps(fn, fn->nvars, &so, &npool);
    int temp_base = running;
    for (int t = fn->nvars; t < fn->nvregs; t++) {
        int k = tslot[t - fn->nvars];
        /* No slot means no slot: giving every slotless temp the same
         * real address made a lowering path that still touched one look
         * harmless. It is not, and this is how it says so. */
        disp[t] = k < 0 ? DEAD_SLOT_OFF : -(temp_base + (k + 1) * 8);
    }
    running = temp_base + npool * 8;
    free(tslot);
    /* a long double temp: its own 16-aligned 16-byte slot, outside the pool */
    for (int t = fn->nvars; t < fn->nvregs; t++)
        if (g_wide && g_wide[t]) {
            running = (running + 16 + 15) & ~15;
            disp[t] = -running;
        }
    /* ...and then a COPY of one need not have a slot of its own at all.
     *
     * A 16-byte value never gets a register, so `%d = ldvar v` and
     * `%d = mov %s` are lowered as sixteen bytes moved from one slot to
     * another -- 433 such copies across lib/libc and lib/libcxx, two
     * instructions each. Where the SOURCE can never change again, the
     * two ends can simply BE the same slot and the copy is nothing at
     * all: copy16 sees equal addresses and emits none.
     *
     * "Can never change again" is the whole condition, and it is read
     * conservatively: the source must have exactly one definition in the
     * function (a parameter's is the prologue's, counted here) and its
     * address must never be taken, so no store through a pointer can
     * reach it either. The destination must likewise be written only by
     * this copy. Walking forward makes it transitive -- a copy of a copy
     * lands on the original's slot -- because a definition precedes
     * every use. */
    if (g_wide && !g_has_cgoto) {
        int nv = fn->nvregs;
        int *nwrite = xcalloc((size_t)(nv ? nv : 1), sizeof *nwrite);
        char *taken = xcalloc((size_t)(nv ? nv : 1), 1);
        for (int v = 0; v < fn->nparams && v < nv; v++)
            nwrite[v]++;                       /* the prologue writes it */
        for (int i = 0; i < fn->nins; i++) {
            struct ir_ins *in = &fn->ins[i];
            int d = in->op == IR_STVAR ? in->dst : ra_ins_def(in);
            if (d >= 0 && d < nv) nwrite[d]++;
            if (in->op == IR_ADDR && in->a >= 0 && in->a < nv)
                taken[in->a] = 1;
        }
        for (int i = 0; i < fn->nins; i++) {
            struct ir_ins *in = &fn->ins[i];
            if (in->op != IR_MOV && in->op != IR_LDVAR)
                continue;
            int d = in->dst, a = in->a;
            if (d < 0 || d >= nv || a < 0 || a >= nv) continue;
            if (!g_wide[d] || !g_wide[a]) continue;
            if (in->op == IR_LDVAR && in->size != 16) continue;
            if (nwrite[d] != 1 || nwrite[a] != 1) continue;
            if (taken[a] || taken[d]) continue;
            if (disp[a] == DEAD_SLOT_OFF || disp[d] == DEAD_SLOT_OFF)
                continue;
            /* ...and the source's SLOT must not change either. A local
             * shares its slot with any other whose life does not overlap
             * its own (coalesce_locals), and its own life ends at its last
             * read -- which may be this very copy. The copy reading that
             * slot then lives on in a slot the next local writes:
             * roundl's x went into `a`'s and `t`'s, and roundl(-2.5L)
             * returned +3. */
            if (a < fn->nvars) {
                int shared = 0;
                for (int b = 0; b < fn->nvars && !shared; b++)
                    shared = b != a && disp[b] == disp[a];
                if (shared) continue;
            }
            if (g_keep_vars && d < fn->nvars) continue;  /* its DWARF home */
            disp[d] = disp[a];
        }
        free(nwrite); free(taken);
    }
    /* struct-return temporaries sit above the outgoing area */
    running += fn->scratch_bytes;
    *scratch_base_out = -running;
    /* A variadic function reserves the SysV register save area (6 int
     * eightbytes + 8 SSE sixteen-bytes = 176) plus one __va_list_tag (24)
     * that va_start initializes. Named source uses a single va_list, so
     * one tag suffices (a second concurrent va_list is a future seam). */
    *va_save_out = 0;
    *va_tag_out = 0;
    if (f->is_varargs) {
        running += 176;
        *va_save_out = -running;
        running += 24;
        *va_tag_out = -running;
    }
    /* The outgoing stack-argument area is the BOTTOM of the frame, so
     * it starts exactly at rsp and a call can address it as [rsp+off]
     * without moving rsp — which also keeps the 16-byte alignment the
     * ABI requires at every call, since the frame is a multiple of 16. */
    running += fn->outgoing_bytes;
    *frame_out = (running + 15) & ~15;
    for (int i = 0; inplace && i < fn->nvars; i++)
        if (inplace[i] >= 0)
            disp[i] = -*frame_out + inplace[i];     /* rsp + stk_off */
    for (int i = 0; i < fn->nparams && i < fn->nvars; i++)
        if (x86_param_home_incoming(f, i))
            disp[i] = DEAD_SLOT_OFF;                /* until the prologue */
    return disp;
}

struct callsite {
    int patch_off;        /* offset of the rel32 field in text */
    struct func *target;
};

/* Growable site lists shared across the unit's functions. */
struct sites {
    struct callsite *call;
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

/* setcc opcode byte per predicate; pointers and unsigned integers use
 * the unsigned condition set (b/be/a/ae). */
/* The negated predicate — for fusing a comparison into the branch that consumes
 * it: `brz (a EQ b)` jumps exactly when `a NE b`. */
static enum binop negate_pred(enum binop p)
{
    switch (p) {
    case B_EQ: return B_NE; case B_NE: return B_EQ;
    case B_LT: return B_GE; case B_GE: return B_LT;
    case B_GT: return B_LE; case B_LE: return B_GT;
    default:   return p;
    }
}

/* Per-vreg use count over the whole function (a source operand appearing once
 * per instruction it is read in), mirroring the liveness USE enumeration. Used
 * to prove a comparison result feeds nothing but the branch that follows it, so
 * the two can fuse into a single `cmp; jcc`. cnt has fn->nvregs entries. */

static int cc_for(enum binop pred, int sign)
{
    switch (pred) {
    case B_EQ: return 0x94;               /* sete */
    case B_NE: return 0x95;               /* setne */
    case B_LT: return sign ? 0x9c : 0x92; /* setl / setb */
    case B_LE: return sign ? 0x9e : 0x96; /* setle / setbe */
    case B_GT: return sign ? 0x9f : 0x97; /* setg / seta */
    case B_GE: return sign ? 0x9d : 0x93; /* setge / setae */
    default:
        internal_error("bad cmp predicate %d", pred);
    }
}

/* A pending branch: where its rel32 displacement must be patched, and the
 * label it targets. File scope because EmbCC's own subset (which compiles
 * this file) does not permit block-scope struct definitions. */
struct brsite {
    int patch_off;
    int label;
    int size;       /* 1 or 4: the width of the displacement field */
    int ord;        /* this branch's number in the function, or -1 for
                     * something that is not a branch at all (the `lea`
                     * of a label's address, which is always 32-bit) */
    int tab;        /* a jump table's entry: the table's offset + 1, and
                     * the value is target - table; 0 for everything else */
};

/* ---- branch relaxation ----------------------------------------------
 *
 * `74 cb` reaches 127 bytes and costs two; `0f 84 cd` reaches anywhere
 * and costs six. Most branches in a function reach their target in a
 * byte, and emitting every one of them long cost about a tenth of all
 * the code EmbCC produced -- 10138 bytes of the 112796 in lib/libc,
 * counted from the objects.
 *
 * Which ones fit is not knowable while emitting, because it depends on
 * where everything after them lands, and that depends on which of THOSE
 * are short. So the function is emitted more than once. The first
 * attempt assumes every branch is short; any that turns out not to
 * reach is marked long and the function is emitted again. The marking
 * only ever goes short -> long, so the layout only grows and the loop
 * terminates -- at worst once per branch, in practice after one or two
 * rounds.
 *
 * Starting optimistic is what makes it tight. From all-long, a branch
 * is only ever shortened when it already fits at the LONG layout, and
 * shortening it does not bring its neighbours into range on the same
 * pass; from all-short, everything that can possibly fit does. */
static unsigned char *g_short;    /* per branch ordinal: emit the short form */
static int g_nshort;
static int g_brord;               /* the next ordinal, while emitting */
static int g_grew;                /* a short branch did not reach */

/* Take the next branch ordinal and say whether it may be short. */
static int br_short(void)
{
    int ord = g_brord++;
    return g_short && ord < g_nshort && g_short[ord];
}

/* g_keep_vars (-O0 and -Og) is declared near the top of the file — it is read
 * by coalesce_locals, which appears before this point. */

/* -mno-sse: never emit an SSE/xmm instruction. A kernel built before it turns
 * on CR4.OSFXSR needs this — any SSE op #UDs. Set by codegen_unit; the varargs
 * prologue skips its xmm spill, and a float operation is refused loudly rather
 * than silently emitting a faulting instruction (THE RULE). */
static int g_no_sse;

/* ---- local register (RAX) residency cache (the -O codegen step) ----
 *
 * Every vreg still owns a stack slot, but a value just computed into RAX
 * need not be reloaded from its slot to be used again. The cache records
 * which TEMP's value RAX currently holds and in which load shape, so an
 * identical reload is elided. Soundness rests on three facts:
 *   - only TEMPS are cached; their slots are never address-taken, so their
 *     memory is stable from the (single) store that defines them;
 *   - STORES are never elided, so a memory operand (a binary op's second
 *     source, read straight from a slot) is always the current value;
 *   - the cache is invalidated (cg_reset) wherever RAX is clobbered without
 *     a cg_load/cg_store fixing it, and at every basic-block boundary.
 * Off (g_regcache 0) the helpers are exactly the old direct calls, so -O0
 * output is byte-for-byte unchanged — which the self-host fixed point needs.
 */
static int g_regcache;        /* enabled only when optimizing */
static int rc_nvars;          /* vregs < this are locals/params (aliasable) */
static int rc_vreg = -1;      /* the temp whose value RAX holds, or -1 */
static int rc_size, rc_sign, rc_w;   /* the exact shape RAX holds it in */
/* -O2 relaxation: when rc_zx, RAX holds the value ZERO-extended above its low
 * rc_vw bytes, so any zero-extending read of at least rc_vw bytes reproduces it
 * (e.g. a 4-byte store then an 8-byte reload — the IR_MOV round-trip).
 *
 * That needs the VALUE to be rc_vw bytes wide, which a store knows (it wrote
 * RAX zero-extended, so the slot's upper bytes are zero) and a load does not:
 * `movl slot, %eax` of an eight-byte value holds its low half, and the full
 * value is still in the slot. So after a LOAD (rc_ld) only a zero-extending
 * read of exactly rc_vw bytes may reuse RAX. Allowing more truncated a 64-bit
 * loop variable copied right after `(unsigned)v == 0` read it -- `v = mov.8 v`
 * became `movq %rax` of the 32-bit load -- in a fuzzed program at -O2. */
static int rc_vw, rc_zx, rc_ld;

/* ---- register allocation (the -O2 codegen step) ----
 *
 * At -O2 a subset of vregs live in CALLEE-SAVED registers (rbx, r12..r15)
 * instead of memory, so their loads/stores vanish. Callee-saved is deliberate:
 * such a value survives a call untouched (the callee preserves it), so there is
 * no spill-around-call logic. g_loc[v] is the physical register a vreg lives in,
 * or -1 for "in its stack slot" (the default, and every vreg when regalloc is
 * off — which keeps -O0/-O1 byte-identical). Only vregs whose every use is at a
 * register-aware site are eligible (see regalloc); the rest stay in memory.
 * Narrow (4-byte) register values keep their upper half zero, mirroring the
 * slot invariant, so an unsigned widen is a plain 64-bit read. */
static int g_regalloc;        /* enabled only at -O2 */
/* Sibling calls: on at -O2, with the register allocator, because both
 * are about a frame being finished with and both are off at -O0 where
 * a debugger wants every frame to still be there. */
static int g_tailcalls;
static const int *g_loc;      /* per-vreg physical register, or -1; NULL when off */

/* Which VECTOR temp xmm0 currently holds, or -1. The same idea as the
 * RAX residency cache above, and it matters more: every vector op works
 * in xmm0 against 16-byte slots, so without it a four-instruction loop
 * body spends eight movdqa shuttling values it already had. */
static int vrc_vreg = -1;

/* Which vector xmm2/xmm3 hold the source and extension bits OF. Widening
 * takes one half at a time, so the same vector is widened twice in a
 * row; without this each half reloaded it and recomputed the sign bits,
 * which is four instructions of the eleven. */
static int vw_src = -1;

/* A shift left whose only purpose is to scale an index for the very
 * next address computation. x86 addressing already has the scale -- the
 * SIB byte holds 1, 2, 4 or 8 -- so the shift need not be an
 * instruction at all. It is recognised at the shift, which is emitted
 * as nothing, and consumed at the add below.
 *
 * Deferred rather than looked back at, because instructions are emitted
 * in order: by the time the add is reached the shift has already gone
 * out, and undoing that is not possible. */
static int fold_idx = -1;         /* the unshifted index, or -1 */
static int fold_scale = 1;

/* zx32[v]: temp v is written exactly once, by an integer operation at
 * width 4 whose every lowering here ends in a 32-bit write of the
 * register -- in place (`and $255, %esi`, `lea (..), %esi`, `imul`), or
 * through RAX and a 32-bit `mov` home. x86-64 zeroes a register's upper
 * half on every 32-bit write, so such a temp's register already holds
 * its zero extension and `(unsigned long)` of it is no instruction.
 *
 * NOT every width-4 value has that property, which is why the general
 * widening still emits `movl`: a copy, a local's read, or a truncating
 * store into a local sharing its source's register can leave the source's
 * upper half in place. Hence the single definition, by one of these
 * operations, of a temp (never a local). Shifts only by a count of 1-31:
 * the manual's pseudo-code for a shift by zero never assigns the
 * destination, so what it does to the upper half is not relied on. Loads
 * only unsigned: every one of those zero-fills, at any width. */
static char *g_zx32;
static int x86_def_zx32(const struct ir_ins *i)
{
    if (i->flt || i->w != 4)
        return 0;
    switch (i->op) {
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_AND: case IR_OR:
    case IR_XOR: case IR_NEG: case IR_BNOT:
        return 1;
    case IR_SHL: case IR_SHR:
        return i->imm_b && (i->imm & 31) != 0;
    case IR_LOAD:
        /* a zero-extending load fills the register whatever form it
         * takes -- movzbl, movzwl, movl, or a 64-bit movzx */
        return !i->sign;
    default:
        return 0;
    }
}

/* May an address computation feeding `mem` be folded into that access's
 * addressing mode?
 *
 * Both halves of the fusion have to agree, because they are two
 * instructions apart: `shl idx, #k` is emitted as NOTHING on the promise
 * that the `add` two instructions later will fold it into a SIB scale,
 * and if the add then decides not to fold, the shift is simply gone.
 *
 * That is exactly what happened. The shift asked only `!x87_ins(mem)`
 * while the add also refused a float-class load or store, so
 * `double *p; p[i]` -- the most ordinary indexed float access there is
 * -- dropped its `shlq $3` and read byte i instead of byte 8i. It
 * survived every test in the tree because the caller usually inlines the
 * function and the inlined copy takes a different path; it shows up the
 * moment the access is out of line.
 *
 * So the question is asked once, here, and both sites call it. */
static int is_flt(int v);
static int x87_ins(const struct ir_ins *i);
static int addr_fold_ok(const struct ir_ins *mem)
{
    if (x87_ins(mem))
        return 0;                       /* long double is gen_x87's */
    /* A float-class value's home is an xmm register, and the fused paths
     * write the slot (cg_store) instead. tests/exec/complex.c caught
     * that end of it. */
    if (mem->op == IR_LOAD)
        return !is_flt(mem->dst);
    if (mem->op == IR_STORE)
        return !is_flt(mem->b);
    return 1;
}

static void cg_reset(void) { rc_vreg = -1; rc_zx = 0; vrc_vreg = -1;
                             vw_src = -1; }

static const int *g_vacc;         /* per-vreg xmm register, or -1 */
static int vacc_of(int v) { return g_vacc && v >= 0 ? g_vacc[v] : -1; }

/* Does this instruction read vector temp `v` FROM XMM0 -- as opposed to
 * from its slot?
 *
 * The distinction is the whole correctness of the cache. A packed ALU op
 * takes its first operand in the register and its second as a MEMORY
 * operand, so a value read as operand b has to be in its slot however
 * recently it was computed. Answering "does it read v" instead of "does
 * it read v in the register" skipped the store for a value the very next
 * instruction then read from the slot, and the answer changed. */
static int vec_reads_xmm(const struct ir_ins *i, int v)
{
    switch (i->op) {
    case IR_VBIN:
        /* An accumulator update takes its OTHER operand from xmm0 when
         * it is sitting there, because the accumulator itself is in a
         * register and the add can be register-to-register. */
        if (i->a == i->dst && vacc_of(i->dst) >= 0)
            return i->b == v;
        return i->a == v && i->b != v;
    case IR_VSTORE:  return i->b == v;
    case IR_VREDADD: case IR_VWIDEN: return i->a == v;
    default:         return 0;
    }
}

/* ---- vector accumulators in registers -------------------------------
 *
 * Everything else here works in xmm0 against 16-byte slots, which is
 * fine for a chain of values that die immediately. An ACCUMULATOR does
 * not: it is written in the preheader and updated every iteration, so
 * it crosses the back edge, so it cannot stay in xmm0 -- and the update
 * costs a load, an add and a store where it should cost an add.
 *
 * A widening sum needs two of them at once, and paid so much for it
 * that vectorizing `long s += a[i]` came out SLOWER than the scalar
 * loop: 0.440 against 0.255. Eight movdqa an iteration shuttling values
 * the registers already held.
 *
 * So an accumulator gets a register of its own for the whole function.
 * It is recognisable without any analysis: every definition is either
 * the splat that zeroes it or an add INTO ITSELF, and every read is one
 * of those adds or the final fold. A temp shaped like that never needs
 * to be anywhere but its register. xmm0 and xmm1 stay scratch; there
 * are four accumulators' worth after them, which is more than the
 * vectorizer builds. */
/* xmm0/xmm1 are scratch, xmm2/xmm3 hold the widening cache below, and
 * accumulators take xmm4 upward. */
#define VACC_FIRST 8      /* the vector accumulators: xmm8-11 */
#define VACC_N     4
#define VW_SRC     12     /* the vector being widened */
#define VW_SIGN    13     /* and its lanes' extension bits */

static int *vacc_regs(struct ir_func *fn)
{
    int nv = fn->nvregs ? fn->nvregs : 1;
    int *reg = xmalloc((size_t)nv * sizeof *reg);
    char *bad = xcalloc((size_t)nv, 1);
    for (int v = 0; v < nv; v++)
        reg[v] = -1;
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        int t = i->dst;
        /* a definition that is not "zero it" or "add into itself" */
        if (t >= 0 && t < nv && i->op != IR_VSPLAT &&
            !(i->op == IR_VBIN && i->a == t))
            bad[t] = 1;
        /* a read that is not one of those adds or the fold */
        switch (i->op) {
        case IR_VBIN:
            /* Operand b is normally a memory operand, which would mean
             * the value had to be in its slot -- but a register-resident
             * one is emitted register-to-register instead, so being read
             * here does not disqualify it. Operand a does, unless it is
             * the accumulator updating itself. */
            if (i->a >= 0 && i->a < nv && i->a != t) bad[i->a] = 1;
            break;
        case IR_VREDADD:
            break;                       /* reading it to fold is fine */
        default:
            /* Anything else that so much as mentions it disqualifies it:
             * the register is the value's only home, so a use this does
             * not understand would read a stale slot. */
            if (i->a >= 0 && i->a < nv) bad[i->a] = 1;
            if (i->b >= 0 && i->b < nv) bad[i->b] = 1;
            if (i->c >= 0 && i->c < nv) bad[i->c] = 1;
            if (i->op == IR_CALL)
                for (int k = 0; k < i->nargs; k++)
                    if (i->argv[k].vreg >= 0 && i->argv[k].vreg < nv)
                        bad[i->argv[k].vreg] = 1;
            if (i->op == IR_ASM && i->asm_ir) {
                for (int k = 0; k < i->asm_ir->nin; k++)
                    if (i->asm_ir->in[k].temp >= 0 &&
                        i->asm_ir->in[k].temp < nv)
                        bad[i->asm_ir->in[k].temp] = 1;
                for (int k = 0; k < i->asm_ir->nout; k++)
                    if (i->asm_ir->out[k].temp >= 0 &&
                        i->asm_ir->out[k].temp < nv)
                        bad[i->asm_ir->out[k].temp] = 1;
            }
            break;
        }
    }
    int next = VACC_FIRST, any = 0;
    for (int n = 0; n < fn->nins && next < VACC_FIRST + VACC_N; n++) {
        struct ir_ins *i = &fn->ins[n];
        if (i->op != IR_VSPLAT || i->dst < 0 || i->dst >= nv)
            continue;
        if (bad[i->dst] || reg[i->dst] >= 0)
            continue;
        /* it must actually be accumulated into, or it is just a splat */
        int updated = 0;
        for (int m = 0; m < fn->nins; m++)
            if (fn->ins[m].op == IR_VBIN && fn->ins[m].dst == i->dst &&
                fn->ins[m].a == i->dst)
                updated = 1;
        if (!updated)
            continue;
        reg[i->dst] = next++;
        any = 1;
    }
    free(bad);
    if (!any) { free(reg); return NULL; }
    return reg;
}

/* May a vector result stay in xmm0 instead of going out to its slot?
 *
 * Two ways. The first is that the very next instruction is the single
 * use it has -- then nothing else can ever read the slot, and nothing
 * can come between.
 *
 * The second is a WIDENING source. A widen reads its operand twice, low
 * half then high half, so it has two uses and the rule above refuses
 * it; but the first widen takes the value straight out of xmm0 and puts
 * it in the xmm2/xmm3 cache, and the second reads it THERE. The slot is
 * written and never read -- a movdqa in the middle of every iteration
 * of a widening sum, for nothing. So: every reader is a widen of it,
 * the first is the next instruction, and nothing between them clears
 * the cache. */
static int vec_keep(struct ir_func *fn, int n, const int *usecnt, int dst)
{
    if (dst < 0 || !usecnt || n + 1 >= fn->nins)
        return 0;
    if (usecnt[dst] == 1)
        return vec_reads_xmm(&fn->ins[n + 1], dst);
    if (fn->ins[n + 1].op != IR_VWIDEN || fn->ins[n + 1].a != dst)
        return 0;
    int seen = 0;
    for (int m = n + 1; m < fn->nins && seen < usecnt[dst]; m++) {
        struct ir_ins *i = &fn->ins[m];
        if (i->op == IR_VWIDEN) {
            if (i->a != dst)
                return 0;               /* a different source evicts it */
            seen++;
            continue;
        }
        switch (i->op) {                /* these leave xmm2/xmm3 alone */
        case IR_VLOAD: case IR_VSTORE: case IR_VBIN:
        case IR_VSPLAT: case IR_VREDADD:
            break;
        default:
            return 0;                   /* anything else resets vw_src */
        }
        if (i->a == dst || i->b == dst || i->c == dst)
            return 0;                   /* a reader that is not a widen */
    }
    return seen == usecnt[dst];
}

static int in_reg(int vreg) { return g_regalloc && g_loc && g_loc[vreg] >= 0; }

/* Is vreg cacheable in RAX? Register-resident vregs and memory TEMPS are (their
 * value is never aliased through memory); a memory LOCAL is not (a store through
 * a pointer could change its slot behind RAX's back). */
static int cacheable(int vreg) { return in_reg(vreg) || vreg >= rc_nvars; }

/* Load vreg into RAX, eliding the load when RAX already holds it in this shape
 * (the residency cache — now covering register-resident vregs too, which is
 * where the store-then-reload round-trips came from). A register-resident vreg
 * is a reg-reg move with the right extension; a memory one a slot load. */
static void cg_load(struct code *text, const int *sd, int vreg,
                    int size, int sign, int w)
{
    if (g_regcache && rc_vreg == vreg) {
        if (rc_size == size && rc_sign == sign && rc_w == w)
            return;                               /* exact: RAX already holds it */
        /* -O2: RAX holds the value zero-extended above rc_vw bytes; a
         * zero-extending read of at least that many bytes reproduces it. */
        if (g_regalloc && rc_zx && sign == 0 &&
            (rc_ld ? size == rc_vw : size >= rc_vw))
            return;
    }
    if (in_reg(vreg)) {
        int R = g_loc[vreg];
        if (size == 1 || size == 2)
            x86_movx_rr(text, REG_RAX, R, size, sign, w); /* char/short widen */
        else if (size == 4 && sign && w == 8)
            x86_movsxd_rr(text, REG_RAX, R);      /* signed int -> 64 */
        else
            /* Four bytes read are a 32-BIT move, which zeroes the upper
             * half -- as cg_ext_into learned. Copying 64 bits trusted the
             * register's upper half to be zero already, and an integer a
             * call returns sits in the whole of rax with whatever the
             * callee left above bit 31: (double)(unsigned)f() converted
             * a negative int's sign bits as part of the value. */
            x86_mov_rr_w(text, REG_RAX, R, size == 8 ? 8 : 4);
    } else {
        x86_load_slot(text, sd[vreg], size, sign, w);
    }
    if (g_regcache && cacheable(vreg)) {
        rc_vreg = vreg; rc_size = size; rc_sign = sign; rc_w = w;
        rc_zx = (sign == 0); rc_vw = size;        /* zero-ext read: low `size` valid */
        rc_ld = 1;                                /* ...and only those bytes */
    } else {
        cg_reset();               /* a memory local (or cache off): don't cache */
    }
}

/* Store RAX to vreg. A register-resident vreg gets a reg-reg move sized to
 * valw (4-byte writes zero the upper half, keeping the narrow-value invariant);
 * a memory vreg is stored 8 bytes (a 32-bit result is zero-extended). Either
 * way RAX still holds the value, so record it for the residency cache. */
static void cg_store(struct code *text, const int *sd, int vreg, int valw)
{
    if (in_reg(vreg))
        x86_mov_rr_w(text, g_loc[vreg], REG_RAX, valw);
    else
        x86_store_slot(text, sd[vreg], 8);
    if (g_regcache && cacheable(vreg)) {
        rc_vreg = vreg; rc_size = valw; rc_sign = 0; rc_w = valw;
        rc_zx = 1; rc_vw = valw;   /* the result is zero-extended to 8 in RAX */
        rc_ld = 0;                 /* and it is the whole value: wider reads too */
    }
}

/* Load vreg into RCX (a binary op's second operand), reg-reg or from its slot. */
static void cg_load_rcx(struct code *text, const int *sd, int vreg, int w)
{
    if (in_reg(vreg))
        x86_mov_rr_w(text, REG_RCX, g_loc[vreg], w);
    else
        x86_mov_ecx_mem(text, sd[vreg], w);
}

/* Produce the size/sign/w-extended value of vreg `a` straight in register `dst`
 * (never RAX): the "extend into the home register" analogue of cg_load, used for
 * a register-resident LDVAR/EXT result whose load actually extends (movsx/movzx/
 * movsxd). Mirrors cg_load's extension choices exactly. Leaves RAX and its
 * residency cache untouched. */
static void cg_ext_into(struct code *text, const int *sd, int dst, int a,
                        int size, int sign, int w)
{
    if (in_reg(a)) {
        int R = g_loc[a];
        if (size == 1 || size == 2)
            x86_movx_rr(text, dst, R, size, sign, w);
        else if (size == 4 && sign && w == 8)
            x86_movsxd_rr(text, dst, R);
        else
            /* size 4 unsigned widening to 8 is a 32-BIT move: that is the
             * instruction that zeroes the upper half. A 64-bit move copied
             * whatever the register held there — and it can hold the rest
             * of a wider value, since a truncating store into a local that
             * shares the source's register emits no move at all — so
             * `(unsigned long)(unsigned int)x` came back as x at -O2. */
            x86_mov_rr_w(text, dst, R, size == 8 ? 8 : 4);
    } else {
        x86_load_reg_basedisp(text, dst, REG_RBP, sd[a], size, sign, w);
    }
}

/* A plain reg-to-reg copy dst<-a of `w` bytes when BOTH vregs are register-
 * resident: emit a single move (or nothing when they already share a register)
 * instead of routing the value through RAX (mov a,%rax; mov %rax,dst). RAX and
 * its residency cache are left untouched — the move never reads or writes RAX,
 * so a value cached there stays valid. Returns 1 if it handled the copy, 0 to
 * fall back to the cg_load/cg_store path. Inert unless -O2 (in_reg needs
 * regalloc), so -O0/-O1 output is byte-identical. */
static int cg_reg_move(struct code *text, int dst, int a, int w)
{
    if (!in_reg(a) || !in_reg(dst))
        return 0;
    if (g_loc[a] != g_loc[dst])
        x86_mov_rr_w(text, g_loc[dst], g_loc[a], w);
    return 1;
}

/* Emit the flag-setting form of an integer IR_CMP `i` (`cmp b,a` / `test a`),
 * leaving the 0/1 result UNMATERIALISED — the caller then either branches (jcc)
 * or setcc's it. The point is operand `a`: a comparison only reads its operands,
 * so when `a` is register-resident we compare straight from its register instead
 * of the old `mov a,%rax; cmp ...` staging move. `a` is staged through RAX only
 * when it isn't in a register, or when `b` sits in memory (there is no
 * register-vs-memory compare encoder, so the register operand must be RAX for
 * `cmp mem,%rax`). When regalloc is off in_reg() is always false, so this always
 * falls to the cg_load path and stays byte-identical to the pre-existing code.
 *
 * Cache: on the register-direct paths RAX is untouched, but the caller's
 * following setcc/jcc clobbers or resets it, so this leaves the residency cache
 * alone and relies on the caller (cg_store after setcc, cg_reset after jcc). */
/* A load deferred into the compare right after it (cmpmem_defer): that
 * compare reads its operand from memory instead. One instruction only --
 * the dispatch loop refuses a deferral nothing consumed. */
static const struct ir_ins *g_cmem_ins;
static int g_cmem_base, g_cmem_index, g_cmem_scale, g_cmem_disp;

static void cg_icmp_flags(struct code *text, const int *sd, struct ir_ins *i)
{
    if (g_cmem_ins == i) {
        g_cmem_ins = NULL;
        if (i->imm_b) {                   /* the load was `a`: cmp [mem], imm */
            x86_alu_mem_imm(text, 'c', g_cmem_base, g_cmem_disp, i->imm,
                            i->w);
            return;
        }
        int areg;
        if (in_reg(i->a)) {
            areg = g_loc[i->a];
        } else {
            cg_load(text, sd, i->a, i->w, 0, i->w);
            areg = REG_RAX;
        }
        if (g_cmem_index >= 0)
            x86_alu_reg_baseindex(text, 'c', areg, g_cmem_base, g_cmem_index,
                                  g_cmem_scale, i->w);
        else
            x86_alu_reg_basedisp(text, 'c', areg, g_cmem_base, g_cmem_disp,
                                 i->w);
        return;
    }
    int b_mem = !i->imm_b && !in_reg(i->b);
    int areg;
    if (in_reg(i->a) && !b_mem) {
        areg = g_loc[i->a];                       /* read a from its register */
    } else {
        cg_load(text, sd, i->a, i->w, 0, i->w);   /* stage a in RAX */
        areg = REG_RAX;
    }
    if (i->imm_b) {
        if (i->imm == 0) x86_test_reg(text, areg, i->w);
        else             x86_alu_reg_imm(text, 'c', areg, i->imm, i->w);
    } else if (in_reg(i->b)) {
        x86_cmp_rr(text, areg, g_loc[i->b], i->w);
    } else {
        x86_cmp_eax_mem(text, sd[i->b], i->w);    /* areg == RAX here */
    }
}

/* Emit a set of register-to-register moves that must take effect "in parallel":
 * every dest receives its src's ORIGINAL value even when a dest is another
 * move's src (a chain) or two moves swap (a cycle). All dests are distinct.
 * Emit any move whose dest no pending move still needs as a source; when only
 * cycles remain, break one by parking its dest in `scratch` (a register outside
 * every src and dest — rax at a call site) and pointing its readers there. Used
 * to shuffle call arguments among rdi..r9 when some already sit in r8/r9. */
/* The same permutation, in the FLOAT file.
 *
 * It did not exist while the FP pool was xmm8-15: sources (the argument
 * registers xmm0-7) and destinations (the pool) were disjoint sets, so
 * one move at a time was always safe. With the pool AT xmm0-7 they are
 * the same eight registers and `movaps xmm0,xmm3` followed by
 * `movaps xmm3,xmm1` delivers one argument twice and loses another.
 *
 * X86_FSCR breaks a cycle: it is in no pool, so parking a value there
 * costs nothing live. */
static void emit_fp_parallel_move(struct code *text, int *dest, int *src,
                                  int n)
{
    char done[16];
    int remaining = 0;
    for (int i = 0; i < n; i++) {
        done[i] = (dest[i] == src[i]);
        if (!done[i]) remaining++;
    }
    while (remaining > 0) {
        int progressed = 0;
        for (int i = 0; i < n; i++) {
            if (done[i]) continue;
            int blocked = 0;
            for (int j = 0; j < n; j++)
                if (!done[j] && j != i && src[j] == dest[i]) { blocked = 1; break; }
            if (blocked) continue;
            x86_movs_reg(text, dest[i], src[i]);
            done[i] = 1; remaining--; progressed = 1;
        }
        if (progressed)
            continue;
        int c = -1;
        for (int i = 0; i < n; i++) if (!done[i]) { c = i; break; }
        x86_movs_reg(text, X86_FSCR, dest[c]);
        for (int j = 0; j < n; j++)
            if (!done[j] && src[j] == dest[c]) src[j] = X86_FSCR;
    }
}

static void emit_reg_parallel_move(struct code *text, int *dest, int *src,
                                   int n, int scratch)
{
    char done[16];
    int remaining = 0;
    for (int i = 0; i < n; i++) {
        done[i] = (dest[i] == src[i]);       /* an identity move is a no-op */
        if (!done[i]) remaining++;
    }
    while (remaining > 0) {
        int progressed = 0;
        for (int i = 0; i < n; i++) {
            if (done[i]) continue;
            int blocked = 0;
            for (int j = 0; j < n; j++)
                if (!done[j] && j != i && src[j] == dest[i]) { blocked = 1; break; }
            if (blocked) continue;
            x86_mov_reg_reg(text, dest[i], src[i]);
            done[i] = 1; remaining--; progressed = 1;
        }
        if (progressed)
            continue;
        int c = -1;                          /* only cycles left: break one */
        for (int i = 0; i < n; i++) if (!done[i]) { c = i; break; }
        x86_mov_reg_reg(text, scratch, dest[c]);
        for (int j = 0; j < n; j++)
            if (!done[j] && src[j] == dest[c]) src[j] = scratch;
    }
}

/* Copy 16 bytes [sbase+soff] -> [dbase+doff].
 *
 * One xmm register carries all sixteen, so this is two instructions
 * rather than four and touches neither rax nor its residency cache.
 * The float scratch carries it -- the same register every other float
 * path here uses, which holds nothing across an IR instruction, and
 * this copy IS one. (It was xmm7 for a few hours, which is a VECTOR
 * ACCUMULATOR: live across a whole loop, so a struct copy in that loop
 * would have eaten it.)
 *
 * Under -mno-sse there is no such register -- a kernel built that way
 * must not touch the FPU -- so the two-eightbytes-through-rax form
 * stays as the fallback, and there neither base may be rax. */
#define COPY16_XMM X86_FSCR
static void copy16(struct code *text, int dbase, int doff, int sbase, int soff)
{
    if (dbase == sbase && doff == soff)
        return;                  /* the two ends share a slot: nothing to do */
    if (!g_no_sse) {
        x86_mov128_load(text, COPY16_XMM, sbase, soff);
        x86_mov128_store(text, dbase, doff, COPY16_XMM);
        return;
    }
    for (int q = 0; q < 16; q += 8) {
        x86_load_reg_mem(text, REG_RAX, sbase, soff + q, 8);
        x86_store_mem_reg(text, dbase, doff + q, REG_RAX, 8);
    }
}

/* The address held by vreg v, in a register other than rax: its own if
 * register-resident, else loaded into rcx. */
static int addr_reg(struct code *text, const int *sd, int v)
{
    if (in_reg(v))
        return g_loc[v];
    x86_load_slot(text, sd[v], 8, 0, 8);
    x86_mov_reg_reg(text, REG_RCX, REG_RAX);
    return REG_RCX;
}

/* A parameter's bytes, from where the caller left them into the slot at
 * [rbp+dst], through rax: 8, 4, 2 and 1 bytes at a time, because an
 * aggregate of 3, 5, 6 or 7 bytes has a tail no single move is (copying
 * it as `chunk` bytes stopped the build with "bad load size 3"). It
 * writes only rax and memory, so in the prologue loop it disturbs no
 * argument register the parallel move has yet to read. */
static void x86_param_copy(struct code *text, int dst, int base, int off,
                           int sz)
{
    for (int at = 0; at < sz;) {
        int w = sz - at >= 8 ? 8 : sz - at >= 4 ? 4 : sz - at >= 2 ? 2 : 1;
        x86_load_reg_mem(text, REG_RAX, base, off + at, w);
        x86_store_mem_reg(text, REG_RBP, dst + at, REG_RAX, w);
        at += w;
    }
}

/* The floating-point pair of cg_load/cg_store. An xmm home and a stack
 * slot are the same value and only one of them is current, so every site
 * that touches a float vreg's slot goes through these. */
/* A float may live in a GENERAL register: a union pun (`u.l = x; return
 * u.d;`) is folded to the integer itself, which the allocator gave an
 * integer home. Its bits move with movq/movd -- through its slot they
 * would be stale, and a frameless function has no slot to read: `double
 * bits(long x)` at -O1 was an internal error ("a frame access in a
 * function that has no frame pointer"). */
static void x86_fld(struct code *text, const int *sd, int v, int xmm, int w)
{
    if (in_freg(v)) {
        if (g_floc[v] != xmm) x86_movs_reg(text, xmm, g_floc[v]);
        return;
    }
    if (in_reg(v)) {
        x86_movq_xmm_gpr(text, xmm, g_loc[v], w);
        return;
    }
    x86_movs_load(text, xmm, sd[v], w);
}

static void x86_fst(struct code *text, const int *sd, int v, int xmm, int w)
{
    if (in_freg(v)) {
        if (g_floc[v] != xmm) x86_movs_reg(text, g_floc[v], xmm);
        return;
    }
    if (in_reg(v)) {
        x86_movq_gpr_xmm(text, g_loc[v], xmm, w);
        return;
    }
    x86_movs_store(text, xmm, sd[v], w);
}

/* ...and "operate where the value already is": which xmm a value can be
 * READ from, which one a result may be COMPUTED in, and the store back
 * when that was the scratch. */
static int x86_frd(struct code *text, const int *sd, int v, int scratch, int w)
{
    if (in_freg(v)) return g_floc[v];
    if (in_reg(v)) {
        x86_movq_xmm_gpr(text, scratch, g_loc[v], w);
        return scratch;
    }
    x86_movs_load(text, scratch, sd[v], w);
    return scratch;
}
static int x86_fwr(int v, int scratch) { return in_freg(v) ? g_floc[v] : scratch; }
static void x86_fwrote(struct code *text, const int *sd, int v, int xmm, int w)
{
    if (!in_freg(v)) x86_movs_store(text, xmm, sd[v], w);
}

/* A float value copied from one place to another. */
static void x86_fmove(struct code *text, const int *sd, int dst, int src, int w)
{
    int r = x86_frd(text, sd, src, X86_FSCR, w);
    if (in_freg(dst)) {
        if (g_floc[dst] != r) x86_movs_reg(text, g_floc[dst], r);
        return;
    }
    x86_movs_store(text, r, sd[dst], w);
}

/* ---- __int128: two eightbytes in a 16-byte slot ----
 * Add, subtract, the bitwise ops, negation and equality are inline;
 * multiplication, division, shifts, ordering and the float conversions are
 * libgcc's (__multi3, __divti3, __ashlti3, __cmpti2, __floattidf ...), whose
 * 128-bit arguments travel in rdi:rsi and rdx:rcx and come back in
 * rax:rdx. (A function computing with one is kept out of the register
 * allocator: these calls clobber the caller-saved registers.) */
static int i128_ins(const struct ir_ins *i)
{
    if (i->flt)
        return 0;
    switch (i->op) {
    case IR_CONST: case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV:
    case IR_MOD: case IR_AND: case IR_OR: case IR_XOR: case IR_SHL:
    case IR_SHR: case IR_NEG: case IR_BNOT: case IR_CMP:
        return i->w == 16;
    case IR_EXT:
        return i->w == 16 || (g_wide && i->a >= 0 && g_wide[i->a]);
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

static struct func *x86_helper(const char *name)
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

static void x86_call_helper(struct code *text, struct sites *st,
                            const char *name)
{
    struct extcall ec = { 0, NULL, 0 };
    ec.patch_off = x86_call_rel32(text);
    ec.callee = x86_helper(name);
    PUSH(st->ext, st->next, st->capext, ec);
}

static void ld8(struct code *text, int reg, int disp)
{
    x86_load_reg_mem(text, reg, REG_RBP, disp, 8);
}

static void st8(struct code *text, int disp, int reg)
{
    x86_store_mem_reg(text, REG_RBP, disp, reg, 8);
}

/* dst += src with the carry (adc), dst -= src with the borrow (sbb):
 * 64-bit, registers below r8 */
static void adc_sbb(struct code *text, int sbb, int dst, int src)
{
    code_byte(text, 0x48);
    code_byte(text, sbb ? 0x19 : 0x11);
    code_byte(text, 0xC0 | (src << 3) | dst);
}

static void gen_i128(struct code *text, const int *sd, struct ir_ins *i,
                     struct sites *st)
{
    cg_reset();
    int d = i->dst >= 0 ? sd[i->dst] : 0;
    switch (i->op) {
    case IR_CONST:
        x86_mov_reg_imm(text, REG_RAX, i->imm, 8);
        st8(text, d, REG_RAX);
        x86_mov_reg_imm(text, REG_RAX, i->imm < 0 ? -1 : 0, 8);
        st8(text, d + 8, REG_RAX);
        break;
    case IR_CAS16:
        /* lock cmpxchg16b [rsi]: rdx:rax expected, rcx:rbx desired; rdx:rax
         * after it the value seen, swapped or not. rbx is callee-saved,
         * and nothing here allocates it: kept on the stack meanwhile. */
        code_byte(text, 0x53);                          /* push rbx */
        ld8(text, REG_RSI, sd[i->a]);
        ld8(text, REG_RAX, sd[i->b]);
        ld8(text, REG_RDX, sd[i->b] + 8);
        ld8(text, 3, sd[i->c]);                         /* rbx */
        ld8(text, REG_RCX, sd[i->c] + 8);
        code_byte(text, 0xf0);                          /* lock */
        code_byte(text, 0x48);                          /* REX.W */
        code_byte(text, 0x0f);
        code_byte(text, 0xc7);
        code_byte(text, 0x0e);                          /* /1, [rsi] */
        code_byte(text, 0x5b);                          /* pop rbx */
        st8(text, d, REG_RAX);
        st8(text, d + 8, REG_RDX);
        break;
    case IR_ADD: case IR_SUB:
        ld8(text, REG_RAX, sd[i->a]);
        ld8(text, REG_RDX, sd[i->a] + 8);
        ld8(text, REG_RCX, sd[i->b]);
        ld8(text, REG_RSI, sd[i->b] + 8);
        x86_alu_rr(text, i->op == IR_ADD ? '+' : '-', REG_RAX, REG_RCX, 8);
        adc_sbb(text, i->op == IR_SUB, REG_RDX, REG_RSI);
        st8(text, d, REG_RAX);
        st8(text, d + 8, REG_RDX);
        break;
    case IR_AND: case IR_OR: case IR_XOR: {
        int op = i->op == IR_AND ? '&' : i->op == IR_OR ? '|' : '^';
        for (int q = 0; q < 16; q += 8) {
            ld8(text, REG_RAX, sd[i->a] + q);
            ld8(text, REG_RCX, sd[i->b] + q);
            x86_alu_rr(text, op, REG_RAX, REG_RCX, 8);
            st8(text, d + q, REG_RAX);
        }
        break;
    }
    case IR_NEG:                /* neg lo; adc hi, 0; neg hi */
        ld8(text, REG_RAX, sd[i->a]);
        ld8(text, REG_RDX, sd[i->a] + 8);
        x86_neg_reg(text, REG_RAX, 8);
        code_byte(text, 0x48);          /* adc rdx, 0 */
        code_byte(text, 0x83);
        code_byte(text, 0xD2);
        code_byte(text, 0x00);
        x86_neg_reg(text, REG_RDX, 8);
        st8(text, d, REG_RAX);
        st8(text, d + 8, REG_RDX);
        break;
    case IR_BNOT:
        for (int q = 0; q < 16; q += 8) {
            ld8(text, REG_RAX, sd[i->a] + q);
            x86_not_reg(text, REG_RAX, 8);
            st8(text, d + q, REG_RAX);
        }
        break;
    case IR_MUL: case IR_DIV: case IR_MOD:
        ld8(text, REG_RDI, sd[i->a]);
        ld8(text, REG_RSI, sd[i->a] + 8);
        ld8(text, REG_RDX, sd[i->b]);
        ld8(text, REG_RCX, sd[i->b] + 8);
        x86_call_helper(text, st, i->op == IR_MUL ? "__multi3"
                                  : i->op == IR_DIV
                                  ? (i->sign ? "__divti3" : "__udivti3")
                                  : (i->sign ? "__modti3" : "__umodti3"));
        st8(text, d, REG_RAX);
        st8(text, d + 8, REG_RDX);
        break;
    case IR_SHL: case IR_SHR:
        /* the count: its low bits, from a narrow value or a wide one */
        if (g_wide && g_wide[i->b])
            ld8(text, REG_RAX, sd[i->b]);
        else
            cg_load(text, sd, i->b, 8, 0, 8);
        x86_mov_reg_reg(text, REG_RDX, REG_RAX);
        ld8(text, REG_RDI, sd[i->a]);
        ld8(text, REG_RSI, sd[i->a] + 8);
        x86_call_helper(text, st, i->op == IR_SHL ? "__ashlti3"
                                  : i->sign ? "__ashrti3" : "__lshrti3");
        st8(text, d, REG_RAX);
        st8(text, d + 8, REG_RDX);
        break;
    case IR_CMP:
        if (i->pred == B_EQ || i->pred == B_NE) {
            ld8(text, REG_RAX, sd[i->a]);
            ld8(text, REG_RCX, sd[i->b]);
            x86_alu_rr(text, '^', REG_RAX, REG_RCX, 8);
            ld8(text, REG_RDX, sd[i->a] + 8);
            ld8(text, REG_RCX, sd[i->b] + 8);
            x86_alu_rr(text, '^', REG_RDX, REG_RCX, 8);
            x86_alu_rr(text, '|', REG_RAX, REG_RDX, 8);
            x86_setcc_eax(text, cc_for(i->pred, 0));
        } else {
            /* x - y as cmp lo; sbb hi: the flags of the whole difference
             * (CF below, SF/OF less) — <, >= from a - b, >, <= from b - a */
            int swap = i->pred == B_GT || i->pred == B_LE;
            int x = swap ? i->b : i->a, y = swap ? i->a : i->b;
            ld8(text, REG_RAX, sd[x]);
            ld8(text, REG_RDX, sd[x] + 8);
            ld8(text, REG_RCX, sd[y]);
            ld8(text, REG_RSI, sd[y] + 8);
            x86_cmp_rr(text, REG_RAX, REG_RCX, 8);
            adc_sbb(text, 1, REG_RDX, REG_RSI);
            x86_setcc_eax(text, cc_for(i->pred == B_LT || i->pred == B_GT
                                       ? B_LT : B_GE, i->sign));
        }
        cg_store(text, sd, i->dst, 4);
        break;
    case IR_EXT:
        if (i->w == 16) {
            /* to 128: the value as its class holds it, then its sign (or
             * zero) in the high eightbyte. A sixteen-byte source is read
             * at the extension's own width: `(int)x` of an __int128 is no
             * instruction once copies are propagated, so ext.16:4s reads
             * the 128-bit value itself, and taking all eight low bytes
             * made (i128)(int)x of 0x80000000 positive. */
            if (g_wide && g_wide[i->a]) {
                ld8(text, REG_RAX, sd[i->a]);
                if (i->size < 8) {
                    x86_shift_reg_imm(text, REG_RAX, '<', 64 - 8 * i->size, 8);
                    x86_shift_reg_imm(text, REG_RAX, i->sign ? '>' : 'u',
                                      64 - 8 * i->size, 8);
                }
            } else
                cg_load(text, sd, i->a, i->size, i->sign, 8);
            cg_reset();
            st8(text, d, REG_RAX);
            if (i->sign) {
                x86_mov_reg_reg(text, REG_RDX, REG_RAX);
                x86_shift_reg_imm(text, REG_RDX, '>', 63, 8);
            } else {
                x86_alu_rr(text, '^', REG_RDX, REG_RDX, 4);
            }
            st8(text, d + 8, REG_RDX);
        } else {
            /* from 128: the low bytes, re-extended as the target type */
            ld8(text, REG_RAX, sd[i->a]);
            if (i->size == 4 && i->w == 8) {
                if (i->sign) {
                    x86_movsxd_rr(text, REG_RAX, REG_RAX);
                } else {
                    code_byte(text, 0x89);              /* mov eax, eax */
                    code_byte(text, 0xc0);
                }
            } else if (i->size < 4) {  /* (x86_movx_rr: 8- and 16-bit) */
                x86_movx_rr(text, REG_RAX, REG_RAX, i->size, i->sign,
                            i->w);
            }
            cg_store(text, sd, i->dst, i->w);
        }
        break;
    case IR_I2F: {
        /* 128-bit integer -> float/double (xmm0) or long double (st0) */
        ld8(text, REG_RDI, sd[i->a]);
        ld8(text, REG_RSI, sd[i->a] + 8);
        const char *h = i->w == 4 ? (i->sign ? "__floattisf" : "__floatuntisf")
                      : i->w == 8 ? (i->sign ? "__floattidf" : "__floatuntidf")
                      : (i->sign ? "__floattixf" : "__floatuntixf");
        x86_call_helper(text, st, h);
        if (i->w == 16)
            x86_x87_mem(text, 0xDB, 7, REG_RBP, d);        /* fstp tword */
        else
            x86_fst(text, sd, i->dst, 0, i->w);   /* libgcc returns xmm0 */
        break;
    }
    case IR_F2I: {
        /* float/double (xmm0) or long double (on the stack) -> 128-bit */
        const char *h = i->size == 4 ? (i->sign ? "__fixsfti" : "__fixunssfti")
                      : i->size == 8 ? (i->sign ? "__fixdfti" : "__fixunsdfti")
                      : (i->sign ? "__fixxfti" : "__fixunsxfti");
        if (i->size == 16) {
            x86_alu_reg_imm(text, '-', REG_RSP, 16, 8);
            for (int q = 0; q < 16; q += 8) {
                ld8(text, REG_RAX, sd[i->a] + q);
                x86_store_mem_reg(text, REG_RSP, q, REG_RAX, 8);
            }
            x86_call_helper(text, st, h);
            x86_alu_reg_imm(text, '+', REG_RSP, 16, 8);
        } else {
            x86_fld(text, sd, i->a, 0, i->size);  /* libgcc reads xmm0 */
            x86_call_helper(text, st, h);
        }
        st8(text, d, REG_RAX);
        st8(text, d + 8, REG_RDX);
        break;
    }
    default:
        break;
    }
    cg_reset();
}

#define FLD_T(d)  x86_x87_mem(text, 0xDB, 5, REG_RBP, (d))   /* fld tword  */
#define FSTP_T(d) x86_x87_mem(text, 0xDB, 7, REG_RBP, (d))   /* fstp tword */

/* One long double instruction (x87_ins): every value is in a 16-byte slot;
 * the x87 stack is used within the instruction and left empty. */
static void gen_x87(struct code *text, const int *sd, struct ir_ins *i)
{
    cg_reset();                           /* rax/rcx are scratch below */
    switch (i->op) {
    case IR_LOAD:
        copy16(text, REG_RBP, sd[i->dst], addr_reg(text, sd, i->a), 0);
        break;
    case IR_STORE:
        copy16(text, addr_reg(text, sd, i->a), 0, REG_RBP, sd[i->b]);
        break;
    case IR_LDVAR: case IR_STVAR: case IR_MOV:
        copy16(text, REG_RBP, sd[i->dst], REG_RBP, sd[i->a]);
        break;
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV:
        /* st1 = a, st0 = b; the popping form leaves st1 OP st0 = a OP b */
        FLD_T(sd[i->a]);
        FLD_T(sd[i->b]);
        x86_op2(text, 0xDE, i->op == IR_ADD ? 0xC1 : i->op == IR_SUB ? 0xE9
                            : i->op == IR_MUL ? 0xC9 : 0xF9);
        FSTP_T(sd[i->dst]);
        break;
    case IR_NEG:
        FLD_T(sd[i->a]);
        x86_op2(text, 0xD9, 0xE0);        /* fchs */
        FSTP_T(sd[i->dst]);
        break;
    case IR_CMP: {
        /* fucomip sets ZF/PF/CF exactly as ucomisd does, so the flag
         * reading below is the float compare's own: <,<= swap the operands
         * and test above/above-equal, which also makes NaN false. */
        int swap = i->pred == B_LT || i->pred == B_LE;
        FLD_T(sd[swap ? i->a : i->b]);    /* st1 = the right-hand side */
        FLD_T(sd[swap ? i->b : i->a]);    /* st0 = the left-hand side  */
        x86_op2(text, 0xDF, 0xE9);        /* fucomip st0, st1 (pops) */
        x86_op2(text, 0xDD, 0xD8);        /* fstp st0 */
        if (i->pred == B_EQ || i->pred == B_NE)
            x86_set_float_eq(text, i->pred == B_NE);
        else
            x86_setcc_eax(text, cc_for(i->pred == B_LT ? B_GT :
                                       i->pred == B_LE ? B_GE : i->pred, 0));
        cg_store(text, sd, i->dst, 4);
        break;
    }
    case IR_I2F:
        /* the integer, sign-extended to 64 bits, through a stack slot:
         * fild takes memory only */
        cg_load(text, sd, i->a, i->size, 1, 8);
        cg_reset();
        x86_alu_reg_imm(text, '-', REG_RSP, 16, 8);
        x86_store_mem_reg(text, REG_RSP, 0, REG_RAX, 8);
        x86_x87_mem(text, 0xDF, 5, REG_RSP, 0);        /* fild qword [rsp] */
        x86_alu_reg_imm(text, '+', REG_RSP, 16, 8);
        FSTP_T(sd[i->dst]);
        break;
    case IR_F2I:
        /* truncate toward zero: fistp rounds by the control word, so set
         * its rounding field to chop for the one store (fisttp would need
         * SSE3, which an x86-64 need not have) */
        FLD_T(sd[i->a]);
        x86_alu_reg_imm(text, '-', REG_RSP, 16, 8);
        x86_x87_mem(text, 0xD9, 7, REG_RSP, 0);        /* fnstcw [rsp] */
        x86_op2(text, 0x0F, 0xB7);                     /* movzx eax, word [rsp] */
        x86_op2(text, 0x04, 0x24);
        x86_alu_reg_imm(text, '|', REG_RAX, 0x0C00, 4);
        x86_op2(text, 0x66, 0x89);                     /* mov [rsp+2], ax */
        x86_op2(text, 0x44, 0x24);
        code_byte(text, 0x02);
        x86_x87_mem(text, 0xD9, 5, REG_RSP, 2);        /* fldcw [rsp+2] */
        x86_x87_mem(text, 0xDF, 7, REG_RSP, 8);        /* fistp qword [rsp+8] */
        x86_x87_mem(text, 0xD9, 5, REG_RSP, 0);        /* fldcw [rsp] */
        x86_load_reg_mem(text, REG_RAX, REG_RSP, 8, 8);
        x86_alu_reg_imm(text, '+', REG_RSP, 16, 8);
        cg_store(text, sd, i->dst, i->w);
        break;
    case IR_F2F:
        if (i->w == 16) {                 /* float/double -> long double */
            x86_x87_mem(text, i->size == 4 ? 0xD9 : 0xDD, 0, REG_RBP, sd[i->a]);
            FSTP_T(sd[i->dst]);
        } else {                          /* long double -> float/double */
            FLD_T(sd[i->a]);
            x86_x87_mem(text, i->w == 4 ? 0xD9 : 0xDD, 3, REG_RBP, sd[i->dst]);
        }
        break;
    default:
        break;
    }
}

#undef FLD_T
#undef FSTP_T

/* ---- sibling calls -------------------------------------------------------
 *
 * `return f(args);` need not build a frame on top of this one. If this
 * frame is finished with -- nothing of it can still be referenced --
 * then tearing it down BEFORE the call and jumping rather than calling
 * leaves the callee returning straight to our caller. The stack stops
 * growing, which is what makes a tail-recursive function a loop instead
 * of an eventual overflow.
 *
 * The conditions are all about that "nothing of it can still be
 * referenced", and each one below is a way for the frame to outlive the
 * jump:
 *
 *   - an argument on the STACK would be written into the very frame
 *     being torn down (the outgoing area overlaps it);
 *   - ANY address-of in the function may have handed a local's address
 *     to somebody, and after `leave` that address points at dead stack.
 *     Conservative -- it refuses functions that pass an address
 *     somewhere harmless -- and conservative is the only safe direction
 *     here, because the failure is a callee reading a live-looking
 *     pointer into a frame that no longer exists;
 *   - a VLA moved the stack pointer, so `leave` does not restore it to
 *     what the epilogue expects;
 *   - a variadic caller has its register save area IN the frame;
 *   - a landing pad is entered on an edge that has no frame at all by
 *     then;
 *   - a struct return goes through a hidden pointer into the caller's
 *     buffer, which is a second thing to get right and is excluded
 *     until it is;
 *   - Win64 reserves shadow space in the caller's frame, which is the
 *     same overlap problem in a different ABI.
 *
 * What is NOT a condition: the return TYPE. The callee leaves its value
 * exactly where this function would have left it -- rax, xmm0, st0 --
 * because they return the same type. That is the whole point of the
 * transform, and it is why the IR_RET that follows is skipped rather
 * than emitted. */
static int tail_call_ok(struct ir_func *fn, int n)
{
    struct ir_ins *c = &fn->ins[n];
    int k;

    if (c->op != IR_CALL || c->indirect || !c->callee)
        return 0;
    if (c->retsize || fn->ret_abi.is_struct)
        return 0;
    if (fn->is_varargs || fn->has_alloca || fn->neh)
        return 0;
    if (target_win64_abi())
        return 0;
    for (k = 0; k < c->nargs; k++)
        if (c->argv[k].on_stack || c->argv[k].byref)
            return 0;
    for (k = 0; k < fn->nins; k++)
        if (fn->ins[k].op == IR_ADDR || fn->ins[k].op == IR_ALLOCA ||
            fn->ins[k].op == IR_IGOTO || fn->ins[k].op == IR_LABELADDR)
            return 0;
    /* Follow the call's value forward to a RET of it.
     *
     * A `mov` that merely forwards the value is transparent, and so is
     * a LABEL: the other path that joins there keeps its own frame and
     * reaches that return the ordinary way, because only THIS path
     * becomes a jump. The instructions between the call and the return
     * simply become unreachable, which is what a jump does to whatever
     * follows it.
     *
     * A `jmp` is where this stops. The return it reaches is somewhere
     * else, and following a branch to decide whether a frame may be
     * destroyed is a dataflow question rather than a peephole one. */
    {
        int val = c->dst;
        int k;
        for (k = n + 1; k < fn->nins; k++) {
            struct ir_ins *r = &fn->ins[k];
            if (r->op == IR_LABEL)
                continue;
            if (r->op == IR_MOV && r->a == val && r->dst >= 0) {
                val = r->dst;
                continue;
            }
            if (r->op == IR_RET)
                return val >= 0 ? r->a == val : r->a < 0;
            return 0;
        }
    }
    return 0;
}

/* Folding a LOCAL's address into the access that uses it.
 *
 * `&s.field` lowers to `addr s`, `add #off`, then a load or a store
 * through the result -- three instructions where the machine has an
 * addressing mode that does it in one: the frame base is a register
 * already and the offset is a constant already. 256 of the 570 `addr`
 * results across lib/libc and lib/libcxx never reach anything BUT a
 * load or a store (600 accesses between them), and every one of those
 * is a `lea` and usually an `add` for nothing.
 *
 * So each address vreg gets a displacement, computed once per function
 * from the slot table, and the accesses use rbp+disp directly. The
 * `addr` and the `add` then emit nothing at all -- which is only safe
 * because the analysis demands that NOTHING ELSE reads them. The set of
 * uses it allows is exactly the set of lowering paths below that know
 * about the fold; anything else, including an address that reaches a
 * call, a comparison or a 16-byte access with its own lowering, marks
 * the root unfoldable and everything derived from it stays as it was.
 *
 * Returns NULL when nothing is foldable. `disp[v]` is meaningful only
 * where `ok[v]`. */
struct afold { char *ok; int *disp; };
static struct afold g_afold;
static int afolded(int v) { return g_afold.ok && v >= 0 && g_afold.ok[v]; }

/* A load whose one reader is the integer compare right after it, as its
 * second operand (pass_x86_loadop put it there) or as its first against
 * an immediate: emit nothing now and let cg_icmp_flags read memory. The
 * base and index must not be RAX, which the compare may stage its other
 * operand in. index < 0: [base + disp]. */
static int cmpmem_defer(struct ir_func *fn, int ln, const int *usecnt,
                        int base, int index, int scale, int disp)
{
    const struct ir_ins *L = &fn->ins[ln], *U;
    if (!usecnt || ln + 1 >= fn->nins)
        return 0;
    U = &fn->ins[ln + 1];
    if (L->op != IR_LOAD || L->vol || L->flt || L->memoff ||
        L->size != L->w || (L->size != 4 && L->size != 8) ||
        L->dst < 0 || usecnt[L->dst] != 1 || U->op != IR_CMP || U->flt ||
        U->w != L->size || U->a == U->b || base == REG_RAX ||
        index == REG_RAX)
        return 0;
    if (U->imm_b) {
        if (U->a != L->dst || index >= 0)
            return 0;
        if (U->w == 8 ? U->imm < -2147483647L - 1 || U->imm > 2147483647L
                      : U->imm < -2147483647L - 1 || U->imm > 4294967295L)
            return 0;
    } else if (U->b != L->dst) {
        return 0;
    }
    g_cmem_ins = U;
    g_cmem_base = base;
    g_cmem_index = index;
    g_cmem_scale = scale;
    g_cmem_disp = disp;
    return 1;
}

/* A load at fn->ins[ln] whose one reader is the operation right after it,
 * taking it as its SECOND operand -- pass_x86_loadop arranged both: that
 * operation is emitted as `op dst, [mem]` and the loaded value never
 * exists in a register. Only with the destination and the first operand
 * in registers, a full-width load (no extension), and never when copying
 * the first operand into the destination would overwrite the address
 * before the operation reads it. index < 0: [base + disp].
 *
 * Not when read-modify-write fusion has claimed that operation (rmw_op,
 * or g_rmwf_op when the RMW's address folded too): it has already
 * dropped the load of the OTHER operand, to be done in memory at the
 * store, and an operation emitted here would read a register nothing
 * loaded. */
static int g_rmwf_ld = -1, g_rmwf_op = -1, g_rmwf_base, g_rmwf_index,
           g_rmwf_scale, g_rmwf_disp;

static int loadop_fuse(struct code *text, struct ir_func *fn, int ln,
                       const int *usecnt, int base, int index, int scale,
                       int disp, int rmw_op)
{
    const struct ir_ins *L = &fn->ins[ln], *U;
    if (!usecnt || ln + 1 >= fn->nins || ln + 1 == rmw_op ||
        ln + 1 == g_rmwf_op)
        return 0;
    U = &fn->ins[ln + 1];
    if (L->op != IR_LOAD || L->vol || L->flt || L->memoff ||
        L->size != L->w || (L->size != 4 && L->size != 8) ||
        L->dst < 0 || usecnt[L->dst] != 1 || U->imm_b || U->flt ||
        U->b != L->dst || U->a == L->dst || U->w != L->size)
        return 0;
    int aop = U->op == IR_ADD ? '+' : U->op == IR_SUB ? '-' :
              U->op == IR_AND ? '&' : U->op == IR_OR ? '|' :
              U->op == IR_XOR ? '^' : 0;
    if (!aop || !in_reg(U->dst) || !in_reg(U->a))
        return 0;
    int D = g_loc[U->dst], A = g_loc[U->a];
    if (D != A && (D == base || D == index))
        return 0;
    if (D != A)
        x86_mov_rr_w(text, D, A, U->w);
    if (index >= 0)
        x86_alu_reg_baseindex(text, aop, D, base, index, scale, U->w);
    else
        x86_alu_reg_basedisp(text, aop, D, base, disp, U->w);
    cg_reset();
    return 1;
}

static void afold_free(struct afold *a) { free(a->ok); free(a->disp); }

static struct afold afold_build(struct ir_func *fn, const int *sd)
{
    struct afold r = { NULL, NULL };
    int nv = fn->nvregs;
    if (nv == 0) return r;
    char *isb = xcalloc((size_t)nv, 1);     /* derived from some `addr` */
    int *root = xmalloc((size_t)nv * sizeof *root);
    int *disp = xmalloc((size_t)nv * sizeof *disp);
    for (int v = 0; v < nv; v++) root[v] = -1;
    /* A temp folded into its accesses stands for ONE frame address, so it
     * must have one definition. A name written on two paths -- `&s1` on
     * one, `&s2` on the other, once a join's copies are coalesced -- took
     * whichever definition came last, and `(k ? s1 : s2).a` read s1
     * either way (tests/exec/struct-rvalue-member.c at -O1). */
    int *ndef = xcalloc((size_t)nv, sizeof *ndef);
    for (int n = 0; n < fn->nins; n++) {
        int d = ra_ins_def(&fn->ins[n]);
        if (d >= 0 && d < nv) ndef[d]++;
    }
    int any = 0;
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        if (i->dst >= 0 && i->dst < nv && ndef[i->dst] != 1)
            continue;
        if (i->op == IR_ADDR && i->dst >= 0 && i->dst < nv &&
            i->a >= 0 && i->a < fn->nvars && sd[i->a] != DEAD_SLOT_OFF) {
            root[i->dst] = i->dst; disp[i->dst] = sd[i->a];
            isb[i->dst] = 1; any = 1;
        } else if (i->op == IR_ADD && i->imm_b && i->dst >= 0 && i->dst < nv &&
                   i->a >= 0 && i->a < nv && isb[i->a]) {
            root[i->dst] = root[i->a];
            disp[i->dst] = disp[i->a] + (int)i->imm;
            isb[i->dst] = 1;
        }
    }
    free(ndef);
    if (!any) { free(isb); free(root); free(disp); return r; }

    /* Every use must be one the lowering below folds. */
    char *bad = xcalloc((size_t)nv, 1);
#define UNFOLD(v) do { int _v=(v); if (_v>=0 && _v<nv && isb[_v]) \
                           bad[root[_v]] = 1; } while (0)
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        /* a 16-byte or vector access has its own lowering and is not
         * one of the four sites below */
        int plain = !i128_ins(i) && !x87_ins(i);
        if (plain && (i->op == IR_LOAD || i->op == IR_STORE)) {
            if (i->op == IR_STORE) UNFOLD(i->b);
            UNFOLD(i->c);
            continue;                        /* `a` is the folded address */
        }
        if (i->op == IR_ADD && i->imm_b && i->dst >= 0 && i->dst < nv &&
            isb[i->dst])
            continue;                        /* the offset chain itself */
        if (i->op == IR_ADDR)
            continue;
        /* A block copy or clear, and a struct argument copied into the
         * outgoing area, take their addresses as rbp+disp too: building a
         * compound literal is a memzero and a store per field, all through
         * an address that otherwise sat in a slot and was reloaded for
         * each one -- four instructions a field where one does. */
        if (i->op == IR_MEMZERO || i->op == IR_MEMCPY)
            continue;
        if (i->op == IR_CALL) {
            if (i->indirect)
                UNFOLD(i->a);                /* the target; a direct call's
                                              * `a` is not an operand */
            /* `byref` is AAPCS64's, computed for every target; here
             * only Win64 copies an aggregate by reference, through the
             * address's slot. */
            for (int k = 0; k < i->nargs; k++)
                if (!(i->argv[k].on_stack && i->argv[k].is_struct &&
                      !target_win64_abi()))
                    UNFOLD(i->argv[k].vreg);
            continue;
        }
        UNFOLD(i->a); UNFOLD(i->b); UNFOLD(i->c);
        /* An asm operand is named nowhere near `a`/`b`/`c`, and the
         * lowering loads every one of them from its slot. Missing these
         * is what crashed asm-clobber.c, asm-inout.c and asm-mem-alu.c
         * the first time this ran. */
        if (i->op == IR_ASM && i->asm_ir) {
            for (int k = 0; k < i->asm_ir->nin; k++)
                UNFOLD(i->asm_ir->in[k].temp);
            for (int k = 0; k < i->asm_ir->nout; k++)
                UNFOLD(i->asm_ir->out[k].temp);
        }
    }
#undef UNFOLD
    char *ok = xcalloc((size_t)nv, 1);
    int used = 0;
    for (int v = 0; v < nv; v++)
        if (isb[v] && !bad[root[v]]) { ok[v] = 1; used = 1; }
    free(isb); free(root); free(bad);
    if (!used) { free(ok); free(disp); return r; }
    r.ok = ok; r.disp = disp;
    return r;
}

/* Put the callee-saved registers back.
 *
 * They were PUSHED, in reverse slot order, so they are a contiguous run
 * ending at rbp -- point rsp at the bottom of it and pop them in slot
 * order. One `lea` plus a byte a register, against four or five a
 * register for the loads. Worth it from two registers up; for one, the
 * load is shorter than the `lea` that would set up the pop.
 *
 * After the pops rsp is rbp again, which is what the `leave` that
 * follows assumes anyway -- so the caller's epilogue is unchanged. */
/* A frame that is only its pushes (see gen_func): popped in the order
 * they sit, the alignment pad first -- into r11, which carries neither
 * an argument nor a result, since a tail call pops after its arguments
 * are in place. */
static int g_pushonly, g_pad;

static void restore_callee(struct code *text, const int *used_callee,
                           int nsave, int save_base)
{
    if (g_pushonly) {
        if (g_pad)
            x86_pop_reg(text, 11);
        for (int k = 0; k < nsave; k++)
            x86_pop_reg(text, used_callee[k]);
        return;
    }
    if (nsave >= 2) {
        x86_lea_reg_slot(text, REG_RSP, save_base);
        for (int k = 0; k < nsave; k++)
            x86_pop_reg(text, used_callee[k]);
        return;
    }
    for (int k = 0; k < nsave; k++)
        x86_load_reg_mem(text, used_callee[k], REG_RBP, save_base + k * 8, 8);
}

/* ---- read-modify-write ------------------------------------------------
 *
 * `t = load [A]`, later `u = t OP v`, then `store [A], u` -- a counter, an
 * accumulator in memory, `stack[sp - 1] += stack[sp]` -- is one x86
 * instruction, `OP v, (A)`, where it was three: the load into a register,
 * the op, and the store back. x86_rmw_find recognises it at the load and
 * returns the op's index; the load emits nothing and the op emits the
 * whole thing and consumes the store.
 *
 * Between the load and the op nothing may write memory (it could be A),
 * redefine A, or leave the block; t and u must have no other use, so
 * neither needs to exist in a register. Only a full-width access (4 or
 * 8 bytes, the op's own width), and only the ops with a memory
 * destination: + - & | ^, with t as the left operand of a subtraction. */
/* A block copy or clear longer than this is `rep movsq` / `rep stosq`
 * rather than two instructions per eight bytes (IR_MEMCPY). */
#define X86_REP_MIN 256

static int x86_rmw_writes_memory(enum ir_op op)
{
    switch (op) {
    case IR_STORE: case IR_STVAR: case IR_CALL: case IR_MEMCPY:
    case IR_MEMZERO: case IR_XCHG: case IR_XADD: case IR_ARMW: case IR_CAS:
    case IR_CMPXCHG: case IR_CAS16: case IR_VSTORE: case IR_ASM:
    case IR_VA_START: case IR_FENCE: case IR_ALLOCA: case IR_SPRESTORE:
    case IR_LANDING:
        return 1;
    default:
        return 0;
    }
}

struct rmw_rd { int v, found; };
static void rmw_rd_cb(int v, void *ctx)
{
    struct rmw_rd *r = ctx;
    if (v == r->v)
        r->found = 1;
}

static int x86_rmw_find(struct ir_func *fn, int n, const int *usecnt);

/* ---- a read-modify-write's address, folded --------------------------------
 *
 * An `add base, X` whose result is the ADDRESS of a read-modify-write
 * folds into the RMW instruction's addressing like any other access, and
 * the shift scaling its index with it: `stack[sp - 1] += stack[sp]` was
 * movslq, shl, add and the RMW, and is movslq and `add %x, (%b,%i,4)`.
 * The address has two uses, the load and the store, where the other
 * fusions want one; and the source reads the right-hand side BETWEEN
 * computing the address and loading through it. So the add emits nothing
 * and leaves the base and the index where they are, and the RMW is
 * emitted at its operation (g_rmwf) -- provided what runs in between is
 * one of a few operations that write no memory and touch no fixed
 * register (a variable shift takes rcx, a divide rax and rdx: the
 * allocator keeps LIVE values out of those, and to it the base and the
 * index died at the add), and that none of them is given the base's or
 * the index's register, which the allocator is free to hand on. The
 * shift's decision (fold_idx) asks this same question, so the two cannot
 * disagree about whether the index was ever computed. Returns the RMW's
 * load, or -1, and its operation through *opp. */
static int rmw_addr_load(struct ir_func *fn, int n, const int *usecnt,
                         int ireg, int *opp)
{
    const struct ir_ins *i = &fn->ins[n];
    int breg, ld = -1, op = -1;
    if (!g_regcache || !usecnt || i->op != IR_ADD || i->flt || i->dst < 0 ||
        usecnt[i->dst] != 2 || !in_reg(i->a) || g_rmwf_op >= 0 ||
        getenv("EMBCC_NO_RMW"))
        return -1;
    if (!i->imm_b && ireg < 0)
        return -1;
    breg = g_loc[i->a];
    for (int k = n + 1; k + 1 < fn->nins && (ld >= 0 || k <= n + 8); k++) {
        const struct ir_ins *o = &fn->ins[k];
        int d;
        if (k == op) {
            *opp = op;
            return ld;
        }
        if (ld < 0 && o->op == IR_LOAD && o->a == i->dst) {
            if (!addr_fold_ok(o) || (op = x86_rmw_find(fn, k, usecnt)) < 0)
                return -1;
            ld = k;
            continue;
        }
        switch (o->op) {
        case IR_LOAD: case IR_LDVAR: case IR_EXT: case IR_MOV:
        case IR_CONST: case IR_GADDR: case IR_ADDR:
        case IR_ADD: case IR_SUB: case IR_AND: case IR_OR: case IR_XOR:
            break;
        case IR_SHL: case IR_SHR:
            if (o->imm_b)
                break;
            return -1;
        default:
            return -1;
        }
        if (o->flt || o->vol)
            return -1;
        d = ra_ins_def(o);
        if (d >= 0 && in_reg(d) && (g_loc[d] == breg || g_loc[d] == ireg))
            return -1;
    }
    return -1;
}

static int x86_rmw_find(struct ir_func *fn, int n, const int *usecnt)
{
    const struct ir_ins *ld = &fn->ins[n];
    int t = ld->dst, A = ld->a;
    if (ld->op != IR_LOAD || ld->vol || ld->memoff || ld->flt ||
        (ld->size != 4 && ld->size != 8) || ld->w != ld->size ||
        t < fn->nvars || t >= fn->nvregs || usecnt[t] != 1 || is_flt(t) ||
        A < 0 || A >= fn->nvregs || (!in_reg(A) && !afolded(A)))
        return -1;
    for (int k = n + 1; k + 1 < fn->nins && k <= n + 16; k++) {
        const struct ir_ins *o = &fn->ins[k];
        switch (o->op) {
        case IR_LABEL: case IR_JMP: case IR_BRZ: case IR_BRNZ: case IR_RET:
        case IR_SWITCH: case IR_IGOTO: case IR_UD2:
            return -1;
        default:
            break;
        }
        if (x86_rmw_writes_memory(o->op) || ra_ins_def(o) == A)
            return -1;
        struct rmw_rd rd = { t, 0 };
        ra_each_use(o, rmw_rd_cb, &rd);
        if (!rd.found)
            continue;
        /* the one use of t */
        int u = o->dst, v;
        if ((o->op != IR_ADD && o->op != IR_SUB && o->op != IR_AND &&
             o->op != IR_OR && o->op != IR_XOR) || o->flt ||
            o->w != ld->w || u < fn->nvars || u >= fn->nvregs ||
            usecnt[u] != 1 || is_flt(u))
            return -1;
        if (o->a == t)
            v = o->imm_b ? -1 : o->b;
        else if (!o->imm_b && o->b == t && o->op != IR_SUB)
            v = o->a;
        else
            return -1;
        if (v == t || (v < 0 && !o->imm_b))
            return -1;
        if (o->imm_b && ld->w == 8 &&
            (o->imm < -2147483648L || o->imm > 2147483647L))
            return -1;
        if (v >= 0 && is_flt(v))
            return -1;
        const struct ir_ins *st = &fn->ins[k + 1];
        if (st->op != IR_STORE || st->a != A || st->b != u || st->vol ||
            st->memoff || st->size != ld->size)
            return -1;
        return k;
    }
    return -1;
}

/* An atomic's address operand, in a register other than rax: its own
 * when it has one, else loaded from its slot into rcx. */
static int x86_atomic_addr(struct code *text, const int *sd, int v)
{
    if (in_reg(v))
        return g_loc[v];
    x86_mov_rcx_slot(text, sd[v]);
    return REG_RCX;
}

/* An atomic's value operand, `size` bytes of which the instruction reads:
 * its own register, or `scratch` filled from its slot. Upper bits past
 * `size` are whatever they are -- the instructions read only the low
 * ones. */
static int x86_atomic_val(struct code *text, const int *sd, int v,
                          int scratch, int size)
{
    if (in_reg(v))
        return g_loc[v];
    x86_load_reg_basedisp(text, scratch, REG_RBP, sd[v], size, 0,
                          size == 8 ? 8 : 4);
    return scratch;
}

static void gen_func(struct ir_func *fn, struct code *text,
                     struct sites *st)
{
    struct func *f = fn->src;
    int frame;
    int scratch_base;
    int sret_slot;
    int va_save, va_tag;

    /* A computed goto's indirect jump makes the CFG imprecise (it can reach any
     * address-taken label), so the liveness the allocator and slot-coalescing
     * rely on is unsound here. Keep such functions in the plain memory model:
     * no register allocation, and every temp/local gets its own slot. */
    g_has_cgoto = 0;
    for (int n = 0; n < fn->nins; n++)
        if (fn->ins[n].op == IR_IGOTO || fn->ins[n].op == IR_LABELADDR)
            { g_has_cgoto = 1; break; }
    /* So does a landing pad, entered from any call of its region (a
     * control-flow edge the liveness below does not see). */
    if (fn->neh)
        g_has_cgoto = 1;
    /* Give such a function the plain memory model for its whole codegen: no
     * register allocation and no RAX residency cache. Both reason about values
     * across straight-line control flow, which an indirect jump violates (the
     * cache would elide the reload before `jmp *rax`, jumping through a stale
     * register). Restored at the single exit so other functions are unaffected. */
    int saved_regalloc = g_regalloc, saved_regcache = g_regcache;
    if (g_has_cgoto) { g_regalloc = 0; g_regcache = 0; }
    /* A function with an __int128 in it used to be kept out of the
     * allocator ENTIRELY, because gen_i128's helper calls clobber the
     * caller-saved registers and nothing told `crosses` about them.
     * op_calls_helper tells it now, so only the 128-bit values
     * themselves stay in memory -- they are `wide`, and were never
     * eligible -- and every ordinary value in the function gets a
     * register like any other. lib/rt/int128.c was 1445 instructions
     * against gcc's 394 with the refusal in place. */
    g_wide = cg_wide_vregs(fn);
    int *vacc = vacc_regs(fn);
    g_vacc = vacc;

    /* -O2: allocate eligible vregs to callee-saved registers first, so the
     * frame can reserve a save slot for each register the allocator uses.
     * When regalloc is off, loc is all -1 and nsave 0 — every path below is a
     * no-op, keeping -O0/-O1 byte-identical. g_loc is read by cg_load/cg_store/
     * cg_load_rcx via in_reg(). */
    int used_callee[NCALLEE], nsave = 0;
    int *loc = NULL;
    if (g_regalloc && !g_has_cgoto) {
        const struct ra_target *rt = &X86_RA;
        /* The float map FIRST: the integer allocation needs it to leave
         * those values alone. */
        g_flt = cg_float_vregs(fn);
        loc = ra_allocate(fn, rt, g_wide, g_flt, used_callee, &nsave);
        g_loc = loc;
        {
            int fsave[NX86_FPOOL], nfsave = 0;
            g_floc = ra_allocate_fp(fn, rt, g_wide, g_flt,
                                    fsave, &nfsave);
            (void)nfsave;
            /* ...and thrown away again until the slot question above is
             * answered. Everything downstream then sees what it saw
             * before: in_freg() is false for every vreg and each float
             * site falls back to its slot. */
            if (!X86_FP_ALLOC) {
                free(g_floc); g_floc = NULL;
                free(g_flt);  g_flt = NULL;
            }
        }
    } else {
        g_loc = NULL;
        g_flt = NULL;
        g_floc = NULL;
    }
    int save_base;
    int *inplace = x86_inplace_locals(fn);
    int *sd = layout_frame(fn, &frame, &scratch_base, &sret_slot,
                           &va_save, &va_tag, nsave, &save_base, loc,
                           inplace);
    free(inplace);
    /* Set by the parameter pass below, read by IR_VA_START: how many
     * named arguments the integer and SSE register files hold, and the
     * rbp offset of the first stack-passed argument (the overflow area). */
    int va_named_int = 0, va_named_sse = 0, va_overflow = 16;

    /* Branch targets and sites are function-local; both arrays are
     * resolved before this function returns. */
    int *label_off = xmalloc((size_t)(fn->nlabels ? fn->nlabels : 1)
                             * sizeof *label_off);
    for (int i = 0; i < fn->nlabels; i++)
        label_off[i] = -1;
    struct brsite *brs = NULL;
    int nbrs = 0, capbrs = 0;
    /* absolute jump-table entries awaiting their labels (IR_SWITCH) */
    struct { int off, label; } *jtabs = NULL;
    int njtabs = 0, capjtabs = 0;

    /* -g: expose each source variable's frame slot (rbp-relative) so the
     * DWARF emitter can write DW_OP_fbreg. sd is indexed by vreg; params and
     * locals are vregs [0, nvars), which is what dbgvars reference. */
    if (target_debug_info()) {
        int nv = fn->nvars ? fn->nvars : 1;
        fn->var_off = xmalloc((size_t)nv * sizeof *fn->var_off);
        for (int v = 0; v < fn->nvars; v++)
            fn->var_off[v] = sd[v] == DEAD_SLOT_OFF ? IR_VAR_NO_LOC
                           : ra_var_home(fn, v, 1, sd[v]);
    }

    /* ---- can this function do without a frame entirely? ----------------
     *
     * `push rbp; mov rsp,rbp; ...; leave` is four instructions a leaf
     * that touches no stack does not need, and a two-line function pays
     * them in full -- opaque_add was nine instructions where gcc emits
     * two. With the slot elision above, such a function's frame is 0, so
     * nothing is left to point at.
     *
     * Everything here is a way rbp could still be read. A call needs the
     * stack aligned (the entry rsp is 8 mod 16 without the push); alloca
     * and va_start move or read it; -g describes locals as fbreg
     * offsets; asm, __builtin_frame_address and the stack-save builtins
     * may name it outright. Parameters are the subtle one: a stack-passed
     * argument is read from [rbp+N], so the frame pointer is needed to
     * find it. Rather than re-derive which parameters those are -- the
     * ABI classifier already does that, and a second copy of it is a
     * second thing to get wrong -- the test is that they plainly fit in
     * registers: four or fewer, each an ordinary integer or pointer.
     * That is the SysV and the Win64 rule at once. */
    int frameless = g_regalloc && frame == 0 && nsave == 0 &&
                    !fn->has_alloca && !f->is_varargs && !g_keep_vars &&
                    sret_slot == 0 && f->nparams <= 4;
    /* A SIBLING call is not a call here: it leaves rsp exactly as it
     * found it and jumps, and the callee sees the stack our caller
     * aligned. A function whose only calls are tail calls needs no frame
     * record to tear down before them. */
    for (int n = 0; n < fn->nins && frameless; n++)
        switch (fn->ins[n].op) {
        case IR_CALL:
            if (!(g_tailcalls && tail_call_ok(fn, n)))
                frameless = 0;
            break;
        case IR_ASM: case IR_ALLOCA: case IR_VA_START:
        case IR_FRAMEADDR: case IR_SPSAVE: case IR_SPRESTORE:
            frameless = 0;
            break;
        default:
            break;
        }
    /* float and double arrive in xmm0-3 under both conventions when there
     * are four parameters or fewer -- System V has eight of them, and
     * Win64 gives each of the four positions an xmm register as well as
     * an integer one. long double is x87 memory in both. */
    for (int p = 0; p < f->nparams && frameless; p++) {
        struct type *pt = f->param_tys[p];
        if (!pt || pt->kind == TY_STRUCT || pt->kind == TY_ARRAY ||
            pt->kind == TY_INT128 || pt->kind == TY_LDOUBLE ||
            ty_size(pt) > 8)
            frameless = 0;
    }

    /* ---- or a frame without a frame POINTER ---------------------------
     *
     * A function that saves registers or makes calls but keeps nothing
     * in memory does not need rbp either: its frame is the pushes. clang
     * and gcc build it that way, and here it was `push rbp; mov rbp,rsp`
     * ... `lea -N(%rbp),%rsp` (or a load per register) ... `leave` --
     * nine bytes a function that point rbp at a frame nothing reads.
     *
     * The conditions are frameless's -- nothing that names rbp or the
     * stack arguments -- with the calls allowed and every slot dead: no
     * local, temp, scratch or outgoing area has an address. At a call
     * rsp must be 16-aligned; it is 8 off at entry, so an even number of
     * pushes takes one more as a pad. ELF only: Mach-O's compact unwind
     * describes every x86-64 function as an rbp frame, and Win64 has
     * unwind codes of its own. Not with exception regions, whose landing
     * pads are entered by the unwinder. */
    g_pushonly = g_pad = 0;
    x86_no_rbp = 0;              /* the prologue below may use rbp */
    if (!frameless && g_regalloc && !fn->has_alloca && !f->is_varargs &&
        !g_keep_vars && sret_slot == 0 && f->nparams <= 4 && !fn->neh &&
        !target_win64_abi() && target_fmt_get() == TGT_FMT_ELF &&
        fn->scratch_bytes == 0 && fn->outgoing_bytes == 0) {
        int calls = 0;
        g_pushonly = 1;
        for (int v = 0; v < fn->nvregs; v++)
            if (sd[v] != DEAD_SLOT_OFF)
                g_pushonly = 0;
        /* a parameter at home in the caller's area is found through rbp */
        for (int p = 0; p < f->nparams; p++)
            if (x86_param_home_incoming(f, p))
                g_pushonly = 0;
        for (int n = 0; n < fn->nins && g_pushonly; n++)
            switch (fn->ins[n].op) {
            case IR_CALL:
                if (!(g_tailcalls && tail_call_ok(fn, n)))
                    calls = 1;
                break;
            case IR_ASM: case IR_ALLOCA: case IR_VA_START:
            case IR_FRAMEADDR: case IR_SPSAVE: case IR_SPRESTORE:
            case IR_LANDING:
                g_pushonly = 0;
                break;
            default:
                if (i128_ins(&fn->ins[n]))
                    calls = 1;          /* a libgcc helper */
                break;
            }
        for (int p = 0; p < f->nparams && g_pushonly; p++) {
            struct type *pt = f->param_tys[p];
            if (!pt || pt->kind == TY_STRUCT || pt->kind == TY_ARRAY ||
                pt->kind == TY_INT128 || pt->kind == TY_LDOUBLE ||
                ty_size(pt) > 8)
                g_pushonly = 0;
        }
        g_pad = g_pushonly && calls && !(nsave & 1);
    }

    /* Sixteen at -O2, as clang does; none at -Os, as clang does there --
     * a one-byte function was followed by fifteen nops. But two in C++:
     * the Itanium ABI's pointer to member function says "virtual" with
     * the low bit of its function address, so a member function at an odd
     * address was called through its vtable instead (clang aligns member
     * functions to 2 for this; every function of a C++ unit is, here). */
    int falign = !target_opt_size() ? 16 : predef_is_cxx() ? 2 : 1;
    if (falign > 1)
        code_align(text, falign, 0x90);
    f->code_off = text->len;
    f->code_align = falign;

    /* The frame record, then the callee-saved registers, then the rest
     * of the frame. They go out as PUSHES: the save area is the top of
     * the frame (layout_frame puts it first, so slot k is at
     * rbp-(nsave-k)*8), which is exactly where pushing them in reverse
     * slot order lands them. One byte each, or two above r8, against
     * four or five for `mov %reg,disp(%rbp)` -- 627 of those across
     * lib/libc and lib/libcxx. The epilogue still reads the slots, and
     * every displacement in the frame is unchanged, because the pushes
     * and the smaller `sub` move rsp by exactly what the `sub` alone
     * moved it by before. */
    /* -fstack-usage: everything below the caller's rsp — the return
     * address, the saved rbp where there is one, and the frame. */
    f->stack_bytes = g_pushonly ? 8 + 8 * (nsave + g_pad)
                                : (int)(frame + 8 + (frameless ? 0 : 8));
    f->cfi_pushonly = g_pushonly;
    f->cfi_npush = 0;
    if (g_pushonly) {
        /* The same order as below, so the registers land where the
         * unwind table says: the last slot first, then the pad. */
        for (int k = nsave - 1; k >= 0; k--) {
            x86_push_reg(text, used_callee[k]);
            f->cfi_push_end[f->cfi_npush++] = text->len - f->code_off;
        }
        if (g_pad) {
            x86_push_reg(text, REG_RAX);
            f->cfi_push_end[f->cfi_npush++] = text->len - f->code_off;
        }
    } else {
        x86_prologue(text, frameless ? frame : 0, frameless);
    }
    /* for the unwind tables: push rbp ends at +1, mov rbp,rsp at +4 */
    f->cfi_frameless = frameless;
    f->cfi_push = 1;
    f->cfi_frame = 4;
    if (!frameless && !g_pushonly) {
        for (int k = nsave - 1; k >= 0; k--)
            x86_push_reg(text, used_callee[k]);
        x86_sub_rsp(text, frame - nsave * 8);
    }
    f->cfi_nsaved = nsave;
    for (int k = 0; k < nsave; k++) {
        /* DWARF numbers the registers rax rdx rcx rbx rsi rdi rbp rsp */
        static const int dw[8] = { 0, 2, 1, 3, 7, 6, 4, 5 };
        f->cfi_reg[k] = used_callee[k] < 8 ? dw[used_callee[k]]
                                           : used_callee[k];
        /* the CFA is rbp+16, or -- with no rbp -- the return address
         * plus the pushes: slot k was the (nsave-k)th of them */
        f->cfi_off[k] = g_pushonly ? -8 * (nsave + 1 - k)
                                   : save_base + k * 8 - 16;
    }
    f->cfi_saved_at = text->len - f->code_off;
    x86_no_rbp = frameless || g_pushonly;
    /* Variadic: spill the whole argument register file into the save area
     * FIRST, before the parameter pass below uses rcx/rax as scratch and
     * so clobbers the vararg registers. Storing a register does not alter
     * it, so the named-parameter loads that follow still see rdi..r9 and
     * xmm0..7 intact. The SSE slots are 16 apart (SysV) but only their low
     * 8 bytes — a double — are stored, which is all vfprintf reads. */
    if (f->is_varargs && target_win64_abi()) {
        /* Microsoft x64: the four argument registers go to the home area
         * the caller reserved above the return address, so that one
         * pointer walks from them into the stack arguments. A float in
         * the first four is in its integer register too -- the caller
         * duplicates it for a variadic call -- so this is all of them.
         * Before, the SysV save area and record were built here: va_arg
         * read "r9" twice where the fifth and sixth slots were. */
        for (int r = 0; r < 4; r++)
            x86_store_mem_reg(text, REG_RBP, 16 + r * 8, x86_argreg(r), 8);
    } else if (f->is_varargs) {
        for (int r = 0; r < 6; r++)
            x86_store_mem_reg(text, REG_RBP, va_save + r * 8,
                              x86_argreg(r), 8);
        /* The SSE half of the save area is skipped under -mno-sse (the xmm
         * spill would #UD before CR4.OSFXSR is set). Sound because a callee
         * built -mno-sse takes no floating varargs, so va_arg never reads it;
         * callers must likewise pass al=0 (they do — no float args exist). */
        if (!g_no_sse)
            for (int r = 0; r < 8; r++)
                x86_movs_store_base(text, REG_RBP, va_save + 48 + r * 16,
                                    r, 8);
    }
    /* -O2 (non-variadic, non-debug): a register-allocated scalar-integer param
     * that arrives in an arg register moves STRAIGHT into its allocated register
     * via one parallel move — no home-slot store + reload. Debug builds keep the
     * slots (DWARF fbreg reads them); variadic keeps the current handling (the
     * arg registers are already spilled to the save area). */
    int pmove = g_regalloc && g_loc && !g_keep_vars && !f->is_varargs;
    int pmv_src[MAX_PARAMS], pmv_dst[MAX_PARAMS], npmv = 0;
    /* A STACK-passed parameter whose home is a register is loaded after
     * the shuffle below, not during the loop that collects it.
     *
     * Its home may be an ARGUMENT register -- r8 and r9 are both in the
     * allocator's pool -- and until the shuffle runs, the argument
     * registers still hold the parameters that arrived in them. Loading
     * into r9 during the loop destroyed the sixth parameter before the
     * shuffle read it, in any function with a seventh that the allocator
     * homed there. The loop's own comment already relied on this: "params
     * reach their allocated registers only in the parallel move that runs
     * after this loop" was true of every other path and not of this one.
     *
     * These are not part of the permutation -- their source is memory --
     * so they need no cycle breaking, only to come after it. */
    int pstk_dst[MAX_PARAMS], pstk_off[MAX_PARAMS], pstk_sz[MAX_PARAMS];
    int npstk = 0;
    /* The float ones, whose home is an xmm register: the same rule,
     * against the FP permutation. Their slot does not exist -- before
     * these, a ninth double went by way of rax into it, and the
     * dead-slot guard refused any such function at -O2. */
    int fstk_dst[MAX_PARAMS], fstk_off[MAX_PARAMS], fstk_sz[MAX_PARAMS];
    int nfstk = 0;
    int fmv_dst[16], fmv_src[16], nfmv = 0;   /* the float half */
    char pmoved[MAX_PARAMS];
    for (int p = 0; p < MAX_PARAMS; p++) pmoved[p] = 0;
    {   /* The same two-file split, in reverse. A hidden return pointer
         * (sret) consumes rdi BEFORE any real parameter, and MEMORY
         * parameters arrive on the caller's stack at [rbp+16...]. */
        int ireg = 0, freg = 0;
        enum arg_class rcls[2];
        int ret_mem = f->ret_ty->kind == TY_STRUCT &&
                      ty_classify(f->ret_ty, rcls) == 0 &&
                      !ty_x87_ret(f->ret_ty);
        if (ret_mem) {
            x86_store_arg(text, ireg++, sret_slot);
        }
        /* System V puts the first stack argument straight above the
         * return address; Microsoft x64 leaves 32 bytes of shadow space
         * there for this function to spill its four register arguments
         * into, so the first one that did not fit is four words higher. */
        int incoming = x86_stack_arg_base();
        for (int i = 0; i < f->nparams; i++) {
            struct type *pt = f->param_tys[i];
            enum arg_class cls[2];
            int n = ty_classify(pt, cls);
            if (target_win64_abi()) {
                /* The caller placed this by POSITION, so the callee
                 * reads it by position: `ireg` is the one slot counter
                 * and the float registers share its numbering. Reading
                 * with a separate float counter is the same mistake the
                 * call site could make, and it looks correct until an
                 * argument list mixes the two classes. */
                int slot = ireg++;
                freg = ireg;
                /* An aggregate that travelled by reference arrives as a
                 * POINTER to the caller's copy. It is copied again into
                 * this function's own slot so that the parameter is an
                 * ordinary local with an ordinary address -- taking its
                 * address, or assigning to it, then behaves as the
                 * language says. The extra copy is redundant (the
                 * caller's is already private) and is the simple thing
                 * that cannot be subtly wrong. */
                /* The pointer goes in r11, never an argument register:
                 * this was rcx, and copying a second-place one there
                 * overwrote the first parameter before the shuffle read
                 * it -- f(long a, struct big s) used s's address as a. */
                if (fn->param_abi && fn->param_abi[i].byref) {
                    int src = x86_argreg(slot);
                    if (slot >= 4) {
                        x86_load_reg_mem(text, 11 /*r11*/, REG_RBP,
                                         incoming, 8);
                        incoming += 8;
                        src = 11;
                    }
                    x86_param_copy(text, sd[i], src, 0, ty_size(pt));
                    continue;
                }
                if (slot >= 4) {
                    if (ty_is_float(pt) && in_freg(i)) {
                        fstk_dst[nfstk] = g_floc[i];
                        fstk_off[nfstk] = incoming;
                        fstk_sz[nfstk] = ty_size(pt);
                        nfstk++;
                    } else if (pmove && g_loc[i] >= 0) {  /* as SysV, below */
                        pstk_dst[npstk] = g_loc[i];
                        pstk_off[npstk] = incoming;
                        pstk_sz[npstk] = ty_size(pt);
                        npstk++; pmoved[i] = 1;
                    } else {
                        x86_load_reg_mem(text, REG_RAX, REG_RBP, incoming, 8);
                        x86_store_slot(text, sd[i], 8);
                    }
                    incoming += 8;
                } else if (ty_is_float(pt) && in_freg(i)) {
                    /* into the FP permutation, as SysV does: moving it
                     * here wrote the register a LATER float arrives in,
                     * and f(double a, double b) returned a in place of b */
                    fmv_dst[nfmv] = g_floc[i];
                    fmv_src[nfmv] = slot;
                    nfmv++;
                } else if (ty_is_float(pt)) {
                    x86_fst(text, sd, i, slot, ty_size(pt));
                } else if (pmove && g_loc[i] >= 0) {
                    pmv_src[npmv] = x86_argreg(slot);
                    pmv_dst[npmv] = g_loc[i];
                    npmv++; pmoved[i] = 1;
                } else {
                    x86_store_arg(text, slot, sd[i]);
                }
                continue;
            }
            if (pt->kind == TY_INT128) {
                /* two integer registers, or 16 aligned bytes of the stack */
                if (ireg + 2 <= 6) {
                    x86_store_arg(text, ireg++, sd[i]);
                    x86_store_arg(text, ireg++, sd[i] + 8);
                } else {
                    incoming = (incoming + 15) & ~15;
                    for (int q = 0; q < 16; q += 8) {
                        x86_load_reg_mem(text, REG_RAX, REG_RBP,
                                         incoming + q, 8);
                        x86_store_slot(text, sd[i] + q, 8);
                    }
                    incoming += 16;
                }
                continue;
            }
            if (pt->kind == TY_LDOUBLE) {
                /* X87 class: always on the caller's stack, 16-aligned */
                incoming = (incoming + 15) & ~15;
                for (int q = 0; q < 16; q += 8) {
                    x86_load_reg_mem(text, REG_RAX, REG_RBP, incoming + q, 8);
                    x86_store_slot(text, sd[i] + q, 8);
                }
                incoming += 16;
                continue;
            }
            if (pt->kind != TY_STRUCT) {
                /* Register file exhausted -> the argument arrived on the
                 * caller's stack at [rbp+incoming] (mirrors the caller's
                 * on_stack decision in irgen). Copy the eightbyte into the
                 * local; without this the 7th+ integer parameter read a
                 * nonexistent register (argregs[6]). */
                int in_reg = ty_is_float(pt) ? (freg < 8) : (ireg < 6);
                if (!in_reg) {
                    /* Straight into its allocated register where it has
                     * one. Going by way of the home slot -- store here,
                     * reload in the materialisation loop below -- is two
                     * instructions for nothing, and it is the only
                     * reason a stack-passed parameter's slot has to
                     * exist at all. */
                    if (ty_is_float(pt) && in_freg(i)) {
                        fstk_dst[nfstk] = g_floc[i];
                        fstk_off[nfstk] = incoming;
                        fstk_sz[nfstk] = ty_size(pt);
                        nfstk++;
                    } else if (pmove && g_loc[i] >= 0) {
                        pstk_dst[npstk] = g_loc[i];
                        pstk_off[npstk] = incoming;
                        pstk_sz[npstk] = ty_size(pt);
                        npstk++; pmoved[i] = 1;
                    } else {
                        x86_load_reg_mem(text, REG_RAX, REG_RBP, incoming, 8);
                        x86_store_slot(text, sd[i], 8);
                    }
                    incoming += 8;
                } else if (ty_is_float(pt)) {
                    /* A float parameter whose home is an xmm register
                     * joins the FP permutation below: with the pool at
                     * xmm0-7 its source and some other parameter's
                     * destination are the same register file. One that
                     * lives in memory is stored here and now, which
                     * only READS an argument register and so cannot
                     * disturb the permutation. */
                    if (in_freg(i)) {
                        fmv_dst[nfmv] = g_floc[i];
                        fmv_src[nfmv] = freg++;
                        nfmv++;
                    } else {
                        x86_fst(text, sd, i, freg++, ty_size(pt));
                    }
                } else if (pmove && g_loc[i] >= 0) {
                    pmv_src[npmv] = x86_argreg(ireg++);   /* arg reg -> its own */
                    pmv_dst[npmv] = g_loc[i];             /* allocated register */
                    npmv++; pmoved[i] = 1;
                } else {
                    x86_store_arg(text, ireg++, sd[i]);
                }
                continue;
            }
            if (n == 0) {
                /* MEMORY: copy it out of the caller's frame into ours, so its
                 * address is a normal local. */
                int sz = ty_size(pt);
                if (ty_align(pt) > 8)       /* a 16-aligned stack slot */
                    incoming = (incoming + 15) & ~15;
                if (x86_param_home_incoming(f, i)) {
                    sd[i] = incoming;       /* its home: no copy at all */
                    incoming += (sz + 7) & ~7;
                    continue;
                }
                x86_param_copy(text, sd[i], REG_RBP, incoming, sz);
                incoming += (sz + 7) & ~7;
                continue;
            }
            /* Registers -- if there are enough left for EVERY eightbyte.
             * When there are not, the whole struct came on the caller's
             * stack and takes no register at all (SysV 3.2.3), which the
             * caller's on_stack decision in irgen already said; this read
             * the next "register" regardless, so a seventh-place
             * struct{short} came out of r9. */
            int need_i = 0, need_s = 0;
            for (int k = 0; k < n; k++) {
                if (cls[k] == CLASS_SSE)
                    need_s++;
                else if (cls[k] != CLASS_NONE)
                    need_i++;
            }
            if (ireg + need_i > 6 || freg + need_s > 8) {
                int sz = ty_size(pt);
                if (ty_align(pt) > 8)       /* a 16-aligned stack slot */
                    incoming = (incoming + 15) & ~15;
                x86_param_copy(text, sd[i], REG_RBP, incoming, sz);
                incoming += (sz + 7) & ~7;
                continue;
            }
            /* registers -> the parameter's own storage. The slot-address scratch
             * must NOT be an integer arg register: RCX (the 4th int arg) would be
             * clobbered here before a later scalar param arriving in it is stored
             * (`f(struct{long,long} s, long a, long b)` -> b lost). RAX is free at
             * prologue time and is never an argument register. */
            x86_lea_reg_slot(text, REG_RAX, sd[i]);
            for (int k = 0; k < n; k++) {
                if (cls[k] == CLASS_SSE)
                    x86_movs_store_base(text, REG_RAX, k * 8, freg++, 8);
                else if (cls[k] != CLASS_NONE)
                    x86_store_mem_reg(text, REG_RAX, k * 8,
                                      x86_argreg(ireg++), 8);
            }
        }
        /* Shuffle the collected arg registers into their allocated registers at
         * once (handles the r8/r9 overlap and any cycle via RAX, which is free
         * here and never an arg or allocated register). */
        if (npmv) emit_reg_parallel_move(text, pmv_dst, pmv_src, npmv, REG_RAX);
        if (nfmv) emit_fp_parallel_move(text, fmv_dst, fmv_src, nfmv);
        /* and only now the stack-passed ones, whose homes may be the
         * argument registers the shuffle has just finished reading */
        for (int k = 0; k < npstk; k++)
            x86_load_reg_mem(text, pstk_dst[k], REG_RBP, pstk_off[k],
                             pstk_sz[k]);
        for (int k = 0; k < nfstk; k++)
            x86_movs_load_base(text, fstk_dst[k], REG_RBP, fstk_off[k],
                               fstk_sz[k]);
        va_named_int = ireg;
        va_named_sse = freg;
        va_overflow = incoming;
    }

    /* -O2: params were stored to their slots above; move each register-resident
     * param's incoming value into its register. (Its low bits are the value; a
     * signed read re-extends, so no widening subtlety.) */
    if (g_regalloc && g_loc)
        for (int p = 0; p < f->nparams; p++) {
            if (g_loc[p] < 0 || pmoved[p]) continue;   /* pmoved: already in reg */
            struct type *pt = f->param_tys[p];
            int psz = ty_size(pt);
            /* Load the home slot straight into the param's register — no RAX
             * detour. Only the low psz bytes matter (a later read re-extends);
             * regalloc promotes only size-4/8 scalars, and x86_load_reg_mem
             * zero-extends a 4-byte load, which is the register narrow-value
             * invariant. Halves the per-param materialisation. */
            x86_load_reg_mem(text, g_loc[p], REG_RBP, sd[p], psz);
        }

    g_afold = afold_build(fn, sd);
    rc_nvars = fn->nvars;
    cg_reset();
    /* Use counts drive comparison/branch fusion below (a compare feeding only
     * the next branch). Built once; freed after the loop. */
    int *usecnt = fn->nvregs
        ? xmalloc((size_t)fn->nvregs * sizeof *usecnt) : (int *)0;
    if (usecnt) ra_count_vreg_uses(fn, usecnt);
    g_zx32 = NULL;
    if (g_regalloc && fn->nvregs) {
        int *nd = xcalloc((size_t)fn->nvregs, sizeof *nd);
        for (int t = 0; t < fn->nins; t++) {
            int v = ra_ins_def(&fn->ins[t]);
            if (v >= 0 && v < fn->nvregs) nd[v]++;
        }
        g_zx32 = xcalloc((size_t)fn->nvregs, 1);
        for (int t = 0; t < fn->nins; t++) {
            int v = ra_ins_def(&fn->ins[t]);
            if (v >= fn->nvars && v < fn->nvregs && nd[v] == 1 &&
                x86_def_zx32(&fn->ins[t]))
                g_zx32[v] = 1;
        }
        free(nd);
    }
    /* -O2: with two or more returns each inlining the full callee-restore
     * sequence, route them through ONE shared epilogue instead — each return
     * loads its value then `jmp`s to it. Worth the jmp only when there is more
     * than one return and something to restore; a single return stays inline. */
    int nret = 0;
    for (int t = 0; t < fn->nins; t++) if (fn->ins[t].op == IR_RET) nret++;
    int shared_epi = g_regalloc && nsave >= 1 && nret >= 2;
    int *epi_patch = NULL, nepi = 0, capepi = 0;
    /* UNREACHABLE code: what follows a return, a jump, a trap or a tail
     * call, up to the next label, which is the only way back in. A
     * sibling call's IR_RET was emitted after its `jmp` -- `mov %r8,%rax;
     * leave; ret` that nothing could reach, in every function ending in
     * one. Optimising builds only, so -O0/-O1 stay byte-identical. */
    int dead = 0, tail_made = 0, rmw_op = -1, rmw_ld = -1;
    g_cmem_ins = NULL;
    g_rmwf_ld = g_rmwf_op = -1;
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        if (dead) {
            if (i->op != IR_LABEL && i->op != IR_LANDING)
                continue;
            dead = 0;
        }
        /* What xmm0 held coming in. Cleared by default, so any op that
         * is not one of the vector cases below invalidates it simply by
         * not setting it again. */
        int vrc_in = vrc_vreg;
        int vw_in = vw_src;
        int fold_in = fold_idx;
        /* THE RULE: a load handed to the next instruction's compare and
         * not taken by it would vanish from the program. */
        if (g_cmem_ins && g_cmem_ins != i)
            internal_error("x86: %s: a load deferred into a compare was not "
                           "consumed", fn->name);
        vrc_vreg = -1;
        vw_src = -1;
        fold_idx = -1;      /* only the instruction right after may use it */
        int ins_start = text->len;
        x86_lowering_op = ir_opname(i->op);   /* for the dead-slot guard */
        /* -g: a row where the source line changes. text->len is the .text
         * offset this instruction's code begins at (the switch below emits
         * it). Multiple IR ops from one statement share a line and collapse
         * to a single row; ops with no line (0) inherit the last row. */
        if (target_debug_info() && i->line) {
            struct ir_line *last = fn->nlines ? &fn->lines[fn->nlines - 1]
                                              : (struct ir_line *)0;
            if (last && last->off == text->len) {
                last->line = i->line;   /* same PC: the latest line wins */
            } else if (!last || last->line != i->line) {
                if (fn->nlines == fn->linecap) {
                    fn->linecap = fn->linecap ? fn->linecap * 2 : 8;
                    fn->lines = xrealloc(fn->lines,
                                         (size_t)fn->linecap * sizeof *fn->lines);
                }
                fn->lines[fn->nlines].off = text->len;
                fn->lines[fn->nlines].line = i->line;
                fn->nlines++;
            }
        }
        /* -mno-sse: an operation that would touch an xmm register (float
         * math, an int<->float conversion) has no non-SSE lowering — refuse
         * it rather than emit a #UD. The kernel reaches this never (it has no
         * float math); if a caller does, the diagnostic names why. */
        if (i128_ins(i)) {           /* __int128 (before the x87's: I2F) */
            gen_i128(text, sd, i, st);
            continue;
        }
        /* long double: the x87 unit, which -mno-sse leaves available */
        if (x87_ins(i)) {
            gen_x87(text, sd, i);
            continue;
        }
        if (g_no_sse && (i->flt || i->op == IR_I2F || i->op == IR_F2I ||
                         i->op == IR_F2F))
            diag_fatal(fn->file, i->line,
                       "floating point needs SSE, which -mno-sse forbids");
        /* Pure address arithmetic on a local, folded into every access
         * that uses it: emit nothing at all. This has to happen BEFORE
         * the switch, because the ALU case would otherwise reach the
         * address-generation fusion below, which consumes the following
         * access itself and reads the base out of a register the fold
         * has just decided not to materialise. tests/exec/aapcs64.c
         * found that within the hour. */
        if ((i->op == IR_ADDR || (i->op == IR_ADD && i->imm_b)) &&
            afolded(i->dst))
            continue;
        /* A read-modify-write whose address folded (rmw_addr_load): the
         * load emits nothing, the operation is the RMW, with the store. */
        if (n == g_rmwf_ld)
            continue;
        if (n == g_rmwf_op) {
            const struct ir_ins *o = i;
            int aop = o->op == IR_ADD ? '+' : o->op == IR_SUB ? '-' :
                      o->op == IR_AND ? '&' : o->op == IR_OR ? '|' : '^';
            int t = fn->ins[g_rmwf_ld].dst;
            int v = o->a == t ? (o->imm_b ? -1 : o->b) : o->a;
            if (v < 0) {
                if (g_rmwf_index < 0)
                    x86_alu_mem_imm(text, aop, g_rmwf_base, g_rmwf_disp,
                                    o->imm, o->w);
                else
                    x86_alu_mem_imm_bi(text, aop, g_rmwf_base, g_rmwf_index,
                                       g_rmwf_scale, o->imm, o->w);
            } else {
                int V = REG_RAX;
                if (in_reg(v))
                    V = g_loc[v];
                else
                    cg_load(text, sd, v, 8, 0, 8);
                if (g_rmwf_index < 0)
                    x86_alu_mem_reg(text, aop, g_rmwf_base, g_rmwf_disp, V,
                                    o->w);
                else
                    x86_alu_mem_reg_bi(text, aop, g_rmwf_base, g_rmwf_index,
                                       g_rmwf_scale, V, o->w);
            }
            cg_reset();
            g_rmwf_ld = g_rmwf_op = -1;
            n++;                                /* ...and the store */
            continue;
        }
        /* THE RULE again: a read-modify-write whose address was left
         * uncomputed and whose operation was never reached would lose
         * the update. */
        if (g_rmwf_op >= 0 && n > g_rmwf_op)
            internal_error("x86: %s: a read-modify-write's folded address "
                           "was not consumed", fn->name);
        /* Read-modify-write (x86_rmw_find): the load emits nothing... */
        if (n == rmw_op) {
            const struct ir_ins *ld = &fn->ins[rmw_ld];
            int base = afolded(ld->a) ? REG_RBP : g_loc[ld->a];
            int disp = afolded(ld->a) ? g_afold.disp[ld->a] : 0;
            int aop = i->op == IR_ADD ? '+' : i->op == IR_SUB ? '-' :
                      i->op == IR_AND ? '&' : i->op == IR_OR ? '|' : '^';
            int v = i->a == ld->dst ? (i->imm_b ? -1 : i->b) : i->a;
            if (v < 0) {
                x86_alu_mem_imm(text, aop, base, disp, i->imm, i->w);
            } else {
                int V = REG_RAX;
                if (in_reg(v))
                    V = g_loc[v];
                else
                    cg_load(text, sd, v, 8, 0, 8);
                x86_alu_mem_reg(text, aop, base, disp, V, i->w);
            }
            cg_reset();
            rmw_op = -1;
            n++;                                /* ...and the store */
            continue;
        }
        if (rmw_op < 0 && i->op == IR_LOAD && g_regalloc && usecnt &&
            !getenv("EMBCC_NO_RMW")) {
            int m = x86_rmw_find(fn, n, usecnt);
            if (m >= 0) {
                rmw_op = m;
                rmw_ld = n;
                continue;
            }
        }
        switch (i->op) {
        /* ---- 128-bit vectors -------------------------------------------
         *
         * Every one works in xmm0 against 16-byte frame slots, which is
         * the shape the scalar float path already has. A vector temp is
         * `wide`, so it has a 16-aligned slot of its own and the integer
         * allocator never sees it -- which is what makes "operate in
         * xmm0, spill to the slot" correct without a second allocator. */
        case IR_VLOAD: {
            vw_src = vw_in;   /* xmm2/xmm3 untouched by this */
            int base;
            cg_reset();
            if (in_reg(i->a)) {
                base = g_loc[i->a];
            } else {
                cg_load(text, sd, i->a, 8, 0, 8);
                base = REG_RAX;
            }
            x86_vload_base(text, X86_FSCR, base, 0);
            rc_vreg = -1;
            if (!vec_keep(fn, n, usecnt, i->dst))
                x86_vstore_slot(text, sd[i->dst], X86_FSCR);
            vrc_vreg = i->dst;   /* still in xmm0, stored or not */
            break;
        }
        case IR_VSTORE: {
            vw_src = vw_in;   /* xmm2/xmm3 untouched by this */
            int base;
            /* The ADDRESS first when the value is already in xmm0, so
             * loading it cannot be what evicts the value. */
            if (vrc_in != i->b)
                x86_vload_slot(text, X86_FSCR, sd[i->b]);
            if (in_reg(i->a)) {
                base = g_loc[i->a];
            } else {
                cg_load(text, sd, i->a, 8, 0, 8);
                base = REG_RAX;
            }
            x86_vstore_base(text, base, 0, X86_FSCR);
            rc_vreg = -1;
            vrc_vreg = i->b;     /* the stored value is still in xmm0 */
            break;
        }
        case IR_VBIN:
            vw_src = vw_in;   /* xmm2/xmm3 untouched by this */
            if (vacc_of(i->dst) >= 0 && i->a == i->dst) {
                /* acc OP= b, in place: no load, no store. */
                int R = vacc_of(i->dst), Rb = vacc_of(i->b);
                if (i->imm == '<' || i->imm == '>')
                    x86_vshift_imm(text, R, i->imm == '<', i->sign, i->size,
                                   i->c);
                else if (Rb >= 0)
                    x86_vbin_rr(text, R, Rb, (int)i->imm, i->size);
                else if (vrc_in == i->b)
                    x86_vbin_rr(text, R, X86_FSCR, (int)i->imm, i->size);
                else
                    x86_vbin_slot(text, R, (int)i->imm, i->size, sd[i->b]);
                break;
            }
            if (vacc_of(i->a) >= 0)
                x86_vmov_rr(text, X86_FSCR, vacc_of(i->a));
            else if (vrc_in != i->a)
                x86_vload_slot(text, X86_FSCR, sd[i->a]);
            if (i->imm == '<' || i->imm == '>')
                x86_vshift_imm(text, X86_FSCR, i->imm == '<', i->sign, i->size,
                               i->c);
            else if (vacc_of(i->b) >= 0)
                x86_vbin_rr(text, X86_FSCR, vacc_of(i->b), (int)i->imm, i->size);
            else
                x86_vbin_slot(text, X86_FSCR, (int)i->imm, i->size, sd[i->b]);
            if (!vec_keep(fn, n, usecnt, i->dst))
                x86_vstore_slot(text, sd[i->dst], X86_FSCR);
            vrc_vreg = i->dst;
            break;
        case IR_VSPLAT:
            vw_src = vw_in;   /* xmm2/xmm3 untouched by this */
            cg_reset();
            cg_load(text, sd, i->a, i->size, 0, i->size == 8 ? 8 : 4);
            x86_vmov_xmm_reg(text, X86_FSCR, REG_RAX, i->size == 8 ? 8 : 4);
            /* 0x00 repeats lane 0 across all four; 0x44 repeats the low
             * QUADword, which is the same thing at eight bytes a lane. */
            x86_vshufd(text, X86_FSCR, X86_FSCR, i->size == 8 ? 0x44 : 0x00);
            rc_vreg = -1;
            if (vacc_of(i->dst) >= 0) {
                x86_vmov_rr(text, vacc_of(i->dst), X86_FSCR);
            } else {
                if (!vec_keep(fn, n, usecnt, i->dst))
                    x86_vstore_slot(text, sd[i->dst], X86_FSCR);
                vrc_vreg = i->dst;
            }
            break;
        case IR_SELECT: {
            /* else -> RAX, condition -> RCX, then cmov the `then` arm
             * over it. RAX is loaded FIRST because `mov` leaves the
             * flags alone while `test` sets them, and the cmov reads
             * what the test set. */
            cg_reset();
            int w = i->w ? i->w : 8;
            int cw = i->size == 8 ? 8 : 4;    /* the condition's width */
            cg_load(text, sd, i->c, w, i->sign, w);
            int cr = in_reg(i->a) ? g_loc[i->a] : REG_RCX;
            if (!in_reg(i->a))
                cg_load_rcx(text, sd, i->a, cw);
            x86_test_rr(text, cr, cr, cw);
            if (in_reg(i->b))
                x86_cmovne_rr(text, REG_RAX, g_loc[i->b], w);
            else
                x86_cmovne_slot(text, REG_RAX, sd[i->b], w);
            rc_vreg = -1;
            cg_store(text, sd, i->dst, w);
            break;
        }
        case IR_VWIDEN:
            /* One half of the lanes, each widened. The extension bits
             * go in xmm1 -- all sign bits for a signed source, all zero
             * for unsigned -- and the unpack interleaves them with the
             * data, so lane k becomes [value, extension] which IS the
             * wider value. The source is destroyed in xmm0, so the
             * other half reloads it from its slot; that is why a
             * widening load has two uses and never stays resident. */
            if (vw_in != i->a) {
                /* Fetch the source once and derive its extension bits
                 * once; the other half reuses both. */
                if (vacc_of(i->a) >= 0)
                    x86_vmov_rr(text, VW_SRC, vacc_of(i->a));
                else if (vrc_in == i->a)
                    x86_vmov_rr(text, VW_SRC, X86_FSCR);
                else
                    x86_vload_slot(text, VW_SRC, sd[i->a]);
                if (i->sign) {
                    x86_vmov_rr(text, VW_SIGN, VW_SRC);
                    x86_vshift_imm(text, VW_SIGN, 0, 1, i->size,
                                   i->size * 8 - 1);
                } else {
                    x86_vbin_rr(text, VW_SIGN, VW_SIGN, '^', i->size);
                }
            }
            x86_vmov_rr(text, X86_FSCR, VW_SRC);
            x86_vunpck(text, X86_FSCR, VW_SIGN, i->c, i->size);
            vw_src = i->a;
            rc_vreg = -1;
            if (!vec_keep(fn, n, usecnt, i->dst))
                x86_vstore_slot(text, sd[i->dst], X86_FSCR);
            vrc_vreg = i->dst;
            break;
        case IR_VREDADD:
            vw_src = vw_in;
            /* Fold the vector against a permuted copy of itself, halving
             * the live lanes each time, until lane 0 holds the sum. */
            cg_reset();
            if (vacc_of(i->a) >= 0)
                x86_vmov_rr(text, X86_FSCR, vacc_of(i->a));
            else if (vrc_in != i->a)
                x86_vload_slot(text, X86_FSCR, sd[i->a]);
            x86_vshufd(text, X86_FSCR2, X86_FSCR, 0x4e);          /* swap the 64-bit halves */
            x86_vbin_rr(text, X86_FSCR, X86_FSCR2, '+', i->size);
            if (i->size == 4) {
                x86_vshufd(text, X86_FSCR2, X86_FSCR, 0xb1);      /* and the 32-bit pairs */
                x86_vbin_rr(text, X86_FSCR, X86_FSCR2, '+', 4);
            }
            x86_vmov_reg_xmm(text, REG_RAX, X86_FSCR, i->size == 8 ? 8 : 4);
            cg_store(text, sd, i->dst, i->w);
            break;
        case IR_CONST:
            /* A constant whose one use is the store right after it, to a
             * folded frame address or one in a register: `mov $imm, mem`,
             * one instruction where materialising it and storing it were
             * two. Every field of a compound literal is one of these. An
             * eight-byte store takes it only when the value fits the
             * sign-extended 32-bit immediate; a float-class value has its
             * own home and keeps its own path. */
            if (g_regalloc && usecnt && i->dst >= 0 && usecnt[i->dst] == 1 &&
                n + 1 < fn->nins && !is_flt(i->dst) && !i->flt) {
                /* past the folded address arithmetic, which emits nothing */
                int m = n + 1;
                while (m + 1 < fn->nins &&
                       (fn->ins[m].op == IR_ADDR ||
                        (fn->ins[m].op == IR_ADD && fn->ins[m].imm_b)) &&
                       afolded(fn->ins[m].dst))
                    m++;
                /* ...or a field's `add base, #off` that only this store
                 * reads, which then becomes the displacement */
                int fbase = -1, fdisp = 0;
                if (m + 1 < fn->nins && fn->ins[m].op == IR_ADD &&
                    fn->ins[m].imm_b && fn->ins[m].dst >= 0 &&
                    usecnt[fn->ins[m].dst] == 1 && in_reg(fn->ins[m].a) &&
                    !afolded(fn->ins[m].dst) &&
                    fn->ins[m].imm >= -2147483647L - 1 &&
                    fn->ins[m].imm <= 2147483647L &&
                    fn->ins[m + 1].op == IR_STORE &&
                    fn->ins[m + 1].a == fn->ins[m].dst) {
                    fbase = g_loc[fn->ins[m].a];
                    fdisp = (int)fn->ins[m].imm;
                    m++;
                }
                struct ir_ins *st = &fn->ins[m];
                int sz = st->size;
                if (st->op == IR_STORE && st->b == i->dst && st->a != i->dst &&
                    !x87_ins(st) && !i128_ins(st) && addr_fold_ok(st) &&
                    (sz == 1 || sz == 2 || sz == 4 ||
                     (sz == 8 && i->w == 8 && i->imm >= -2147483647L - 1 &&
                      i->imm <= 2147483647L)) &&
                    (fbase >= 0 || afolded(st->a) || in_reg(st->a))) {
                    if (fbase >= 0)
                        x86_store_mem_imm(text, fbase, fdisp, i->imm, sz);
                    else if (afolded(st->a))
                        x86_store_mem_imm(text, REG_RBP, g_afold.disp[st->a],
                                          i->imm, sz);
                    else
                        x86_store_mem_imm(text, g_loc[st->a], 0, i->imm, sz);
                    n = m;                      /* consume the store */
                    break;
                }
            }
            /* A FLOAT constant is an ordinary integer const of the value's
             * bit pattern -- `0.5` is `const.8s 4602678819172646912`, and
             * nothing in the IR says float but the class its destination
             * belongs to. When that destination has an xmm home, put the
             * bits in a general register and move them across; the slot
             * is the traffic the class exists to remove. A float-classed
             * dest that was SPILLED still falls through to the integer
             * path below, which writes the same slot in one instruction
             * instead of three. */
            if (in_freg(i->dst)) {
                int D = g_floc[i->dst];
                cg_reset();
                x86_mov_eax_imm(text, i->imm, i->w);
                x86_movq_xmm_gpr(text, D, REG_RAX, i->w == 8 ? 8 : 4);
                break;
            }
            /* Register-resident dest: materialise the constant straight in its
             * register (`xor D,D` for zero, else `mov $imm,D`), no RAX detour and
             * no store. RAX is untouched, so a value cached there survives. */
            if (in_reg(i->dst)) {
                int D = g_loc[i->dst];
                if (i->imm == 0) x86_alu_rr(text, '^', D, D, 4);
                else             x86_mov_reg_imm(text, D, i->imm, i->w);
                break;
            }
            /* -O2: materialise zero with `xor eax,eax` (2 bytes, upper zeroed)
             * rather than a 7-byte `mov`. Gated to keep -O0/-O1 byte-identical;
             * safe because no comparison's flags are live across a CONST. */
            if (g_regalloc && i->imm == 0)
                x86_zero_eax(text);
            else
                x86_mov_eax_imm(text, i->imm, i->w);
            cg_store(text, sd, i->dst, i->w);
            break;
        case IR_MOV:
            /* A float value is somebody else's class: it moves between
             * xmm registers, or through one, never through RAX. Same for
             * the four below -- none of these ops carries ir_ins::flt,
             * because a copy of eight bytes is a copy of eight bytes. */
            if (is_flt(i->dst) || is_flt(i->a)) {
                cg_reset(); x86_fmove(text, sd, i->dst, i->a, 8); break;
            }
            /* Two register-resident vregs: a direct reg-reg move (or nothing
             * when coalesced onto the same register) — no RAX round-trip. */
            if (cg_reg_move(text, i->dst, i->a, 8))
                break;
            /* dst register-resident, source in memory: load straight into dst's
             * register instead of memory->RAX->dst. Regalloc never hands out
             * RAX, so g_loc[dst] != RAX and the RAX residency cache is left
             * intact. Halves the very common LDVAR-of-a-local-into-a-temp
             * sequence (`mov slot,%rax; mov %rax,%rN` -> `mov slot,%rN`). */
            if (in_reg(i->dst)) {
                x86_load_reg_mem(text, g_loc[i->dst], REG_RBP, sd[i->a], 8);
                break;
            }
            /* source register-resident, dest in memory: store straight to the
             * slot (`mov %rN,slot`) instead of routing through RAX (`mov %rN,%rax;
             * mov %rax,slot`). RAX and its residency cache are untouched — the
             * value already in RAX (if any) stays valid. */
            if (in_reg(i->a)) {
                x86_store_mem_reg(text, REG_RBP, sd[i->dst], g_loc[i->a], 8);
                break;
            }
            cg_load(text, sd, i->a, 8, 0, 8);
            cg_store(text, sd, i->dst, 8);
            break;
        case IR_ADD:
        case IR_SUB:
        case IR_MUL:
        case IR_AND:
        case IR_OR:
        case IR_XOR:
            if (i->flt) {
                int fop = i->op == IR_ADD ? '+' : i->op == IR_SUB ? '-' : '*';
                cg_reset();
                /* The destination is also the LEFT operand on x86, so
                 * it has to hold `a` before the op and the result after.
                 * The allocator is free to give the result the register
                 * `b` is living in -- both die here -- and then loading
                 * `a` into it would destroy `b` before it is read. When
                 * that happens the op runs in the scratch instead and
                 * the result is moved home afterwards. */
                int home = x86_fwr(i->dst, X86_FSCR), d = home;
                if (in_freg(i->b) && g_floc[i->b] == d && i->a != i->b)
                    d = X86_FSCR;
                x86_fld(text, sd, i->a, d, i->w);
                if (in_freg(i->b))
                    x86_sse_alu_reg(text, fop, d, g_floc[i->b], i->w);
                else
                    x86_sse_alu_mem(text, fop, d, sd[i->b], i->w);
                if (d != home) x86_movs_reg(text, home, d);
                x86_fwrote(text, sd, i->dst, home, i->w);
                break;
            }
            /* ...and the address of a read-modify-write (rmw_addr_load):
             * nothing here, the RMW at its operation. */
            {
                int ireg = i->imm_b ? -1 : in_reg(fold_in >= 0 ? fold_in
                                                               : i->b)
                           ? g_loc[fold_in >= 0 ? fold_in : i->b] : -1;
                int op = -1, L = rmw_addr_load(fn, n, usecnt, ireg, &op);
                if (L >= 0) {
                    g_rmwf_ld = L;
                    g_rmwf_op = op;
                    g_rmwf_base = g_loc[i->a];
                    g_rmwf_index = ireg;
                    g_rmwf_scale = fold_in >= 0 ? fold_scale : 1;
                    g_rmwf_disp = i->imm_b ? (int)i->imm : 0;
                    break;
                }
            }
            /* Address-generation fusion: an `ADD base, X` whose SOLE use is the
             * immediately-following memory access folds into that access's
             * addressing, dropping the address computation. X a constant ->
             * base+disp (`p->field`); X a register -> base+index (`p[i]`). The
             * ADD's result is never materialised (the fusion reads base/index,
             * not the sum), so no register is needed for it. */
            if (g_regcache && usecnt && i->op == IR_ADD &&
                in_reg(i->a) && n + 1 < fn->nins && usecnt[i->dst] == 1 &&
                (i->imm_b ? 1 : in_reg(i->b))) {
                struct ir_ins *nx = &fn->ins[n + 1];
                int base = g_loc[i->a], index = i->imm_b ? 0 : g_loc[i->b];
                int scale = 1;
                if (!i->imm_b && fold_in >= 0) {
                    index = g_loc[fold_in];     /* the shift we skipped */
                    scale = fold_scale;
                }
                /* a 16-byte (long double) access is gen_x87's, never
                 * fused -- and neither is one whose VALUE is in the
                 * float class, because this path writes the slot
                 * (cg_store) while that value's home is an xmm register.
                 * tests/exec/complex.c caught it: `scalef` loaded the
                 * imaginary part into eax and then multiplied whatever
                 * was still in xmm8, which was the real part's product. */
                if (!addr_fold_ok(nx)) {
                    /* fall through to materialise the address */
                } else if (nx->op == IR_LOAD && nx->a == i->dst &&
                           cmpmem_defer(fn, n + 1, usecnt, base,
                                        i->imm_b ? -1 : index, scale,
                                        i->imm_b ? (int)i->imm : 0)) {
                    n++;              /* the add and the load: the compare
                                       * next reads the memory itself */
                    break;
                } else if (nx->op == IR_LOAD && nx->a == i->dst &&
                           loadop_fuse(text, fn, n + 1, usecnt, base,
                                       i->imm_b ? -1 : index, scale,
                                       i->imm_b ? (int)i->imm : 0, rmw_op)) {
                    n += 2;           /* the add, the load and their user */
                    break;
                } else if (nx->op == IR_LOAD && nx->a == i->dst) {
                    /* Straight into the value's own register when it has
                     * one. Landing in RAX and moving from there was a
                     * second instruction for every fused load, and the
                     * fusion fires on nearly every field access. */
                    int D = in_reg(nx->dst) ? g_loc[nx->dst] : REG_RAX;
                    if (i->imm_b)
                        x86_load_reg_basedisp(text, D, base, (int)i->imm,
                                              nx->size, nx->sign, nx->w);
                    else
                        x86_load_reg_baseindex(text, D, base, index, scale,
                                               nx->size, nx->sign, nx->w);
                    if (D == REG_RAX) cg_store(text, sd, nx->dst, nx->w);
                    else              cg_reset();
                    n++;                           /* consume the fused load */
                    break;
                } else if (nx->op == IR_STORE && nx->a == i->dst) {
                    /* Straight out of the value's own register when it has
                     * one; otherwise through RAX, which never aliases the
                     * base or the index. */
                    int Sv;
                    if (in_reg(nx->b)) {
                        Sv = g_loc[nx->b];
                    } else {
                        cg_load(text, sd, nx->b, 8, 0, 8);
                        Sv = REG_RAX;
                    }
                    if (i->imm_b)
                        x86_store_basedisp_reg(text, base, (int)i->imm, Sv,
                                               nx->size);
                    else
                        x86_store_baseindex_reg(text, base, index, scale, Sv,
                                                nx->size);
                    n++;                           /* consume the fused store */
                    break;
                }
            }
            {
                int aop = i->op == IR_ADD ? '+' :
                          i->op == IR_SUB ? '-' :
                          i->op == IR_MUL ? '*' :
                          i->op == IR_AND ? '&' :
                          i->op == IR_OR ? '|' : '^';
                /* Operand b folded to an immediate (the optimizer's imm-fold):
                 * `OP $imm, dst` with no constant materialised in a register. In
                 * the dest register directly when both dst and a are resident
                 * (RAX untouched), else through RAX. MUL is never imm-folded. */
                if (i->imm_b) {
                    /* MUL by a constant is the three-operand imul: dst = a*imm
                     * directly, no copy and no rax detour even when dst != a. */
                    if (i->op == IR_MUL) {
                        if (in_reg(i->dst) && in_reg(i->a)) {
                            x86_imul_reg_imm(text, g_loc[i->dst], g_loc[i->a],
                                             i->imm, i->w);
                            break;
                        }
                        cg_load(text, sd, i->a, i->w, 0, i->w);
                        x86_imul_reg_imm(text, REG_RAX, REG_RAX, i->imm, i->w);
                        cg_store(text, sd, i->dst, i->w);
                        break;
                    }
                    if (in_reg(i->dst) && in_reg(i->a)) {
                        int D = g_loc[i->dst], A = g_loc[i->a];
                        /* `d = a + k` with d != a is one `lea`, not a
                         * copy and an add -- x86's only three-address
                         * arithmetic, and the commonest shape the
                         * coalescer fails to merge: 286 sites across
                         * lib/libc and lib/libcxx.
                         *
                         * At width 4 too. A `lea` with a 32-bit
                         * DESTINATION computes the address in 64 bits
                         * and truncates, zero-extending into the full
                         * register -- which is exactly what a 32-bit
                         * `add` does, so the narrow-value invariant
                         * holds either way. `sub` reaches it as an add
                         * of the negated constant, which is how the
                         * optimizer already writes most of them. */
                        long k = i->op == IR_SUB ? -i->imm : i->imm;
                        if (D != A && (i->op == IR_ADD || i->op == IR_SUB) &&
                            (i->w == 4 || i->w == 8) &&
                            k >= -2147483647L - 1 && k <= 2147483647L) {
                            x86_lea_reg_basedisp(text, D, A, (int)k, i->w);
                            break;
                        }
                        if (D != A)
                            x86_mov_rr_w(text, D, A, i->w);
                        x86_alu_reg_imm(text, aop, D, i->imm, i->w);
                        break;
                    }
                    cg_load(text, sd, i->a, i->w, 0, i->w);
                    x86_alu_reg_imm(text, aop, REG_RAX, i->imm, i->w);
                    cg_store(text, sd, i->dst, i->w);
                    break;
                }
                /* All three operands register-resident: compute in the dest
                 * register, no RAX detour. dst = a OP b becomes an in-place
                 * `OP b,dst` when dst already holds a (the common case after
                 * coalescing), else `mov a,dst; OP b,dst`. The one hazard is
                 * dst sharing b's register with a non-commutative SUB — fall
                 * back to RAX there. RAX (and its cache) is left untouched. */
                if (in_reg(i->dst) && in_reg(i->a) && in_reg(i->b)) {
                    int D = g_loc[i->dst], A = g_loc[i->a], B = g_loc[i->b];
                    int commut = i->op != IR_SUB;
                    if (D == A) {
                        x86_alu_rr(text, aop, D, B, i->w);
                        break;
                    } else if (D != B) {
                        /* the same three-address trick for `d = a + b` */
                        if (i->op == IR_ADD && (i->w == 4 || i->w == 8)) {
                            x86_lea_reg_baseindex(text, D, A, B, 1, i->w);
                            break;
                        }
                        x86_mov_rr_w(text, D, A, i->w);
                        x86_alu_rr(text, aop, D, B, i->w);
                        break;
                    } else if (commut) {              /* D holds b; a OP b == b OP a */
                        x86_alu_rr(text, aop, D, A, i->w);
                        break;
                    }
                    /* SUB with D == B != A: fall through to the RAX path. */
                }
                cg_load(text, sd, i->a, i->w, 0, i->w);
                /* a register-resident second operand is a reg-reg op; a memory
                 * one keeps the direct memory-operand form (no extra load). */
                if (in_reg(i->b))
                    x86_alu_rr(text, aop, REG_RAX, g_loc[i->b], i->w);
                else
                    x86_alu_eax_mem(text, aop, sd[i->b], i->w);
            }
            cg_store(text, sd, i->dst, i->w);
            break;
        case IR_SQRT:
            /* sqrtsd xmm0, [a] -- the hardware's result is correctly
             * rounded, which is why the C library calls this rather than
             * iterating. */
            cg_reset();
            {
                int d = x86_fwr(i->dst, X86_FSCR);
                if (in_freg(i->a))
                    x86_sse_alu_reg(text, 'q', d, g_floc[i->a], i->w);
                else
                    x86_sse_alu_mem(text, 'q', d, sd[i->a], i->w);
                x86_fwrote(text, sd, i->dst, d, i->w);
            }
            break;
        case IR_DIV:
        case IR_MOD:
            if (i->flt) { /* only DIV is ever float; MOD is integers */
                cg_reset();
                {
                    int home = x86_fwr(i->dst, X86_FSCR), d = home;
                    if (in_freg(i->b) && g_floc[i->b] == d && i->a != i->b)
                        d = X86_FSCR;             /* see IR_ADD above */
                    x86_fld(text, sd, i->a, d, i->w);
                    if (in_freg(i->b))
                        x86_sse_alu_reg(text, '/', d, g_floc[i->b], i->w);
                    else
                        x86_sse_alu_mem(text, '/', d, sd[i->b], i->w);
                    if (d != home) x86_movs_reg(text, home, d);
                    x86_fwrote(text, sd, i->dst, home, i->w);
                }
                break;
            }
            cg_load(text, sd, i->a, i->w, 0, i->w);
            if (i->sign)
                x86_cdq(text, i->w);
            else
                x86_zero_edx(text);
            if (in_reg(i->b))
                x86_div_rr(text, g_loc[i->b], i->sign, i->w);
            else
                x86_div_mem(text, sd[i->b], i->sign, i->w);
            if (i->op == IR_MOD)
                x86_mov_eax_edx(text, i->w); /* remainder lives in edx */
            cg_store(text, sd, i->dst, i->w);
            break;
        case IR_SHL:
            /* `shl idx, #k` feeding an `add base, .` that feeds a
             * memory access: the whole shift becomes the SIB scale. */
            if (g_regcache && usecnt && i->imm_b && !i->flt &&
                i->imm >= 1 && i->imm <= 3 && i->dst >= 0 &&
                usecnt[i->dst] == 1 && in_reg(i->a) &&
                n + 2 < fn->nins) {
                struct ir_ins *ad = &fn->ins[n + 1];
                struct ir_ins *mem = &fn->ins[n + 2];
                int rmw_dummy;
                if (ad->op == IR_ADD && !ad->flt && !ad->imm_b &&
                    ad->b == i->dst && in_reg(ad->a) && ad->dst >= 0 &&
                    ((usecnt[ad->dst] == 1 && addr_fold_ok(mem) &&
                      ((mem->op == IR_LOAD && mem->a == ad->dst) ||
                       (mem->op == IR_STORE && mem->a == ad->dst))) ||
                     rmw_addr_load(fn, n + 1, usecnt, g_loc[i->a],
                                   &rmw_dummy) >= 0)) {
                    fold_idx = i->a;
                    fold_scale = 1 << (int)i->imm;
                    break;              /* emitted as nothing */
                }
            }
            /* fall through to the ordinary shift */
        case IR_SHR: {
            int skind = i->op == IR_SHL ? '<' : i->sign ? '>' : 'u';
            /* Constant shift count folded to an immediate: `shift $k, dst` with
             * no count loaded into rcx — in the dest register when resident. */
            if (i->imm_b) {
                if (in_reg(i->dst) && in_reg(i->a)) {
                    int D = g_loc[i->dst], A = g_loc[i->a];
                    if (D != A)
                        x86_mov_rr_w(text, D, A, i->w);
                    x86_shift_reg_imm(text, D, skind, (int)i->imm, i->w);
                    break;
                }
                cg_load(text, sd, i->a, i->w, 0, i->w);
                x86_shift_reg_imm(text, REG_RAX, skind, (int)i->imm, i->w);
                cg_store(text, sd, i->dst, i->w);
                break;
            }
            cg_load(text, sd, i->a, i->w, 0, i->w);
            cg_load_rcx(text, sd, i->b, 4);  /* byte-identical to the old
                                              * x86_mov_ecx_mem when regalloc off */
            x86_shift_eax_cl(text, skind, i->w);
            cg_store(text, sd, i->dst, i->w);
            break;
        }
        case IR_NEG:
        case IR_BNOT:
            /* Both register-resident: negate/complement in the dest register
             * (in place when dst and a coalesced onto one), no RAX detour. */
            if (in_reg(i->dst) && in_reg(i->a)) {
                int D = g_loc[i->dst], A = g_loc[i->a];
                if (D != A)
                    x86_mov_rr_w(text, D, A, i->w);
                if (i->op == IR_NEG) x86_neg_reg(text, D, i->w);
                else                 x86_not_reg(text, D, i->w);
                break;
            }
            cg_load(text, sd, i->a, i->w, 0, i->w);
            if (i->op == IR_NEG)
                x86_neg_eax(text, i->w);
            else
                x86_not_eax(text, i->w);
            cg_store(text, sd, i->dst, i->w);
            break;
        case IR_CMP:
            if (i->flt) {
                /* ucomis sets the UNSIGNED flags, so >,>= use seta/setae
                 * directly and <,<= are the same test with the operands
                 * swapped — which is also what makes NaN compare false
                 * in every direction. */
                cg_reset();
                int swap = i->pred == B_LT || i->pred == B_LE;
                x86_fld(text, sd, swap ? i->b : i->a, X86_FSCR, i->w);
                {
                    int other = swap ? i->a : i->b;
                    if (in_freg(other))
                        x86_ucomis_reg(text, X86_FSCR, g_floc[other], i->w);
                    else
                        x86_ucomis_mem(text, X86_FSCR, sd[other], i->w);
                }
                /* ...and fuse it into the branch that solely consumes
                 * it, the way the integer compare below already is.
                 * `__divsc3` spent 99 of its 443 instructions on
                 * test/movzbl/sete materialising conditions it then
                 * immediately branched on; gcc spends none.
                 *
                 * Only the ORDERED predicates. ucomis sets CF on an
                 * unordered compare, so ja/jae are false for a NaN in
                 * either direction, which is what C asks for. Equality
                 * is the awkward one -- it has to consult PF as well,
                 * so `==` is two branches and a label -- and is left to
                 * the setcc path. */
                int fused = 0;
                if (g_regcache && usecnt && n + 1 < fn->nins &&
                    (fn->ins[n + 1].op == IR_BRZ ||
                     fn->ins[n + 1].op == IR_BRNZ) &&
                    fn->ins[n + 1].a == i->dst && usecnt[i->dst] == 1) {
                    struct ir_ins *br = &fn->ins[n + 1];
                    int eq = i->pred == B_EQ || i->pred == B_NE;
                    /* Equality has to consult PF as well, because
                     * ucomis sets ZF=PF=CF for an unordered pair. Two of
                     * the four cases come out as "unordered OR not
                     * equal", which is two jumps to the same label:
                     * `!=` taken, and `==` NOT taken. The other two want
                     * "ordered AND equal", which needs a label of its
                     * own to jump over -- those keep the setcc. */
                    int two = eq && ((i->pred == B_NE) ==
                                     (br->op == IR_BRNZ));
                    if (!eq || two) {
                        enum binop p = i->pred == B_LT ? B_GT
                                     : i->pred == B_LE ? B_GE : i->pred;
                        if (br->op == IR_BRZ && !eq) p = negate_pred(p);
                        int ord = g_brord, sh = br_short();
                        int ccs[2], ncc = 0;
                        if (two) { ccs[ncc++] = 0x9a;    /* jp  */
                                   ccs[ncc++] = 0x95; }  /* jne */
                        else ccs[ncc++] = cc_for(p, 0);
                        for (int q = 0; q < ncc; q++) {
                            int patch = sh ? x86_jcc_rel8(text, ccs[q])
                                           : x86_jcc_rel32(text, ccs[q]);
                            if (nbrs == capbrs) {
                                capbrs = capbrs ? capbrs * 2 : 16;
                                brs = xrealloc(brs, (size_t)capbrs *
                                                    sizeof *brs);
                            }
                            brs[nbrs].patch_off = patch;
                            brs[nbrs].label = br->label;
                            brs[nbrs].size = sh ? 1 : 4;
                            brs[nbrs].ord = ord;
                            brs[nbrs].tab = 0;
                            nbrs++;
                        }
                        cg_reset();
                        n++;                   /* consume the fused branch */
                        fused = 1;
                    }
                }
                if (!fused) {
                    if (i->pred == B_EQ || i->pred == B_NE)
                        x86_set_float_eq(text, i->pred == B_NE);
                    else
                        x86_setcc_eax(text,
                                      cc_for(i->pred == B_LT ? B_GT :
                                             i->pred == B_LE ? B_GE : i->pred,
                                             0));
                    cg_store(text, sd, i->dst, 4); /* the 0/1 result is an int */
                }
                break;
            }
            /* Fuse an integer comparison into the branch that solely consumes
             * it: `cmp; jcc` instead of setcc/movzx/store then load/test/jz.
             * Sound only when the next op is that branch on this result and the
             * result is used nowhere else. Gated to the optimizing path so -O0
             * stays byte-identical (the self-host fixed point). */
            if (g_regcache && usecnt && n + 1 < fn->nins &&
                (fn->ins[n + 1].op == IR_BRZ || fn->ins[n + 1].op == IR_BRNZ) &&
                fn->ins[n + 1].a == i->dst && usecnt[i->dst] == 1) {
                struct ir_ins *br = &fn->ins[n + 1];
                cg_icmp_flags(text, sd, i);
                /* BRNZ jumps when the comparison is true; BRZ when it is false. */
                enum binop jp = br->op == IR_BRNZ ? i->pred
                                                  : negate_pred(i->pred);
                int ord = g_brord;
                int sh = br_short();
                int patch = sh ? x86_jcc_rel8(text, cc_for(jp, i->sign))
                               : x86_jcc_rel32(text, cc_for(jp, i->sign));
                cg_reset();                       /* control splits here */
                if (nbrs == capbrs) {
                    capbrs = capbrs ? capbrs * 2 : 16;
                    brs = xrealloc(brs, (size_t)capbrs * sizeof *brs);
                }
                brs[nbrs].patch_off = patch;
                brs[nbrs].label = br->label;
                brs[nbrs].size = sh ? 1 : 4;
                brs[nbrs].ord = ord;
                brs[nbrs].tab = 0;
                nbrs++;
                n++;                              /* consume the fused branch */
                break;
            }
            cg_icmp_flags(text, sd, i);
            /* Register-resident dest: setcc + widen straight into it, no
             * result-carrying `mov %eax,%rN`. setcc_reg touches only the home
             * register, so a value cached in RAX (e.g. operand a, if it was
             * staged) survives. */
            if (in_reg(i->dst)) {
                x86_setcc_reg(text, cc_for(i->pred, i->sign), g_loc[i->dst]);
                break;
            }
            x86_setcc_eax(text, cc_for(i->pred, i->sign));
            cg_store(text, sd, i->dst, 4);        /* the 0/1 result is an int */
            break;
        case IR_I2F: {
            cg_reset();
            /* the SOURCE is an integer here, so only the result is in
             * the float class -- and it converts straight into that
             * value's own register when it has one */
            int fd = x86_fwr(i->dst, X86_FSCR);
            x86_cvtsi2s(text, fd, sd[i->a], i->size, i->w);
            x86_fwrote(text, sd, i->dst, fd, i->w);
            break;
        }
        case IR_F2I:
            cg_reset();
            if (in_freg(i->a))
                x86_cvtts2si_reg(text, g_floc[i->a], i->size, i->w);
            else
                x86_cvtts2si(text, sd[i->a], i->size, i->w);
            cg_store(text, sd, i->dst, i->w);
            break;
        case IR_BITCAST:
            /* movq/movd between the register files -- the ONE instruction
             * that is target-specific about fabs, copysign, signbit and
             * the isnan/isinf family. Everything else those builtins do
             * is integer arithmetic the optimizer already knows.
             *
             * But only when the float side is ACTUALLY in the float
             * class. The whitelist above hands a value to the integer
             * file as soon as one use treats it as an integer, and a
             * bitcast's own use does not count -- `float r = fabsf(x);
             * memcpy(&b, &r, 4)` reads r as four bytes and takes it
             * back. When that has happened both ends are integers and
             * the bits are already where they belong, so this emits a
             * plain move, or nothing at all. That is the whole reason to
             * ask in_freg rather than assume: the earlier version read
             * the float's slot, which a register-resident value does not
             * have, and the frame guard said so. */
            cg_reset();
            if (i->sign) {                        /* float bits -> integer */
                if (in_freg(i->a))
                    x86_movq_gpr_xmm(text, REG_RAX, g_floc[i->a], i->w);
                else
                    cg_load(text, sd, i->a, i->size, 0, i->w);
            } else {                              /* integer bits -> float */
                cg_load(text, sd, i->a, i->size, 0, i->size);
                if (in_freg(i->dst)) {
                    x86_movq_xmm_gpr(text, g_floc[i->dst], REG_RAX, i->w);
                    break;
                }
            }
            cg_store(text, sd, i->dst, i->w);
            break;
        case IR_F2F: {
            /* Convert straight into the destination's own register when
             * it has one -- cvtss2sd names both ends now. */
            cg_reset();
            int fd = x86_fwr(i->dst, X86_FSCR);
            if (in_freg(i->a))
                x86_cvts2s_reg(text, fd, g_floc[i->a], i->size);
            else
                x86_cvts2s(text, fd, sd[i->a], i->size);
            x86_fwrote(text, sd, i->dst, fd, i->w);
            break;
        }
        case IR_LDVAR:
            if (is_flt(i->dst) || is_flt(i->a)) {
                cg_reset(); x86_fmove(text, sd, i->dst, i->a, i->size); break;
            }
            /* coalesced plain load whose local and temp share a register: no-op
             * (only when no extension is emitted — see ldvar_plain). */
            /* A plain (non-extending) read of a register-resident local into a
             * register-resident temp is a direct reg-reg move — no RAX detour.
             * The narrow-value invariant holds: a 4-byte move zero-extends. */
            if (ldvar_plain(i->size, i->sign, i->w) &&
                cg_reg_move(text, i->dst, i->a, i->size == 8 ? 8 : 4))
                break;
            /* A plain (non-extending) load into a register-resident temp from a
             * MEMORY local: load straight into the temp's register instead of
             * memory->RAX->reg. x86_load_reg_mem zero-extends narrow reads, which
             * matches ldvar_plain's non-signed-widen loads exactly; regalloc
             * never hands out RAX so its residency cache is untouched. This is
             * the hot LDVAR-of-a-param/local case (`mov slot,%rax; mov %rax,%rN`
             * -> `mov slot,%rN`). */
            if (ldvar_plain(i->size, i->sign, i->w) && in_reg(i->dst)) {
                x86_load_reg_mem(text, g_loc[i->dst], REG_RBP, sd[i->a], i->size);
                break;
            }
            /* Extending read (movsx/movzx/movsxd) into a register-resident temp:
             * extend straight into it, no RAX detour. */
            if (in_reg(i->dst)) {
                cg_ext_into(text, sd, g_loc[i->dst], i->a, i->size, i->sign, i->w);
                break;
            }
            cg_load(text, sd, i->a, i->size, i->sign, i->w);
            cg_store(text, sd, i->dst, i->w);
            break;
        case IR_STVAR:
            if (is_flt(i->dst) || is_flt(i->a)) {
                cg_reset(); x86_fmove(text, sd, i->dst, i->a, i->size); break;
            }
            /* Both register-resident: a direct reg-reg move (coalesced same-reg
             * writes vanish). The written local keeps the value in its low
             * i->size bytes; a later read movsx/movzx-extends from them. */
            if (cg_reg_move(text, i->dst, i->a, i->size))
                break;
            /* register-resident source into a MEMORY local: store the register's
             * low i->size bytes straight to the slot (`mov %rN,slot`) instead of
             * `mov %rN,%rax; mov %rax,slot`. A local's slot holds only its low
             * i->size bytes (reads movsx/movzx-extend), so this writes exactly
             * what the RAX path would, byte-for-byte, at any width. RAX and its
             * residency cache are untouched. */
            if (in_reg(i->a) && !in_reg(i->dst)) {
                x86_store_mem_reg(text, REG_RBP, sd[i->dst], g_loc[i->a], i->size);
                break;
            }
            cg_load(text, sd, i->a, 8, 0, 8);
            if (in_reg(i->dst))                          /* register-resident local */
                x86_mov_rr_w(text, g_loc[i->dst], REG_RAX, i->size);
            else
                x86_store_slot(text, sd[i->dst], i->size); /* dst is a local */
            break;
        case IR_ADDR:
            /* Register-resident dest: lea straight into it, no RAX detour/store. */
            if (in_reg(i->dst)) { x86_lea_reg_slot(text, g_loc[i->dst], sd[i->a]); break; }
            x86_lea_rax_slot(text, sd[i->a]);
            cg_store(text, sd, i->dst, 8);
            break;
        case IR_STRADDR: {
            /* The lea's rel32 is relocated whether it targets RAX or a home
             * register; only the destination register differs. */
            int D = in_reg(i->dst);
            struct strsite ss;
            ss.patch_off = D ? x86_lea_reg_rip(text, g_loc[i->dst])
                             : x86_lea_rax_rip(text);
            ss.str_off = i->label;  /* resolved to an offset below */
            ss.kind = RK_PCREL32;
            PUSH(st->str, st->nstr, st->capstr, ss);
            if (!D) cg_store(text, sd, i->dst, 8);
            break;
        }
        case IR_GADDR: {
            int D = in_reg(i->dst);
            struct gsite gs;
            if (i->glob->is_tls) {
                /* Not an address in this image: an offset into a block
                 * that is different for every thread. So the thread
                 * pointer is read first and the offset added to it,
                 * and the offset is what the relocation carries. */
                int r = D ? g_loc[i->dst] : 0;   /* 0 = rax */
                x86_mov_reg_fsbase(text, r);
                gs.patch_off = x86_add_reg_imm32_reloc(text, r);
                gs.kind = RK_TPOFF32;
            } else {
                gs.patch_off = D ? x86_lea_reg_rip(text, g_loc[i->dst])
                                 : x86_lea_rax_rip(text);
                gs.kind = RK_PCREL32;
            }
            gs.glob = i->glob;
            PUSH(st->g, st->ng, st->capg, gs);
            if (!D) cg_store(text, sd, i->dst, 8);
            break;
        }
        case IR_FADDR: {
            int D = in_reg(i->dst);
            struct fsite fs;
            fs.patch_off = D ? x86_lea_reg_rip(text, g_loc[i->dst])
                             : x86_lea_rax_rip(text);
            fs.target = i->callee;
            fs.kind = RK_PCREL32;
            fs.addend = 0;
            PUSH(st->f, st->nf, st->capf, fs);
            if (!D) cg_store(text, sd, i->dst, 8);
            break;
        }
        case IR_LOAD:
            if (is_flt(i->dst)) {
                int d = x86_fwr(i->dst, X86_FSCR);
                if (afolded(i->a))
                    x86_movs_load(text, d, g_afold.disp[i->a], i->size);
                else
                    x86_movs_load_base(text, d, addr_reg(text, sd, i->a), 0,
                                       i->size);
                x86_fwrote(text, sd, i->dst, d, i->size);
                break;
            }
            /* Register-resident dest: load straight into it, no result-carrying
             * `mov %rax,%rN`. The address is either already in a register (RAX
             * wholly untouched — cache preserved) or staged into RAX as the base
             * (RAX still holds that address afterward, so its cache entry stays
             * valid — the load reads [rax], it does not overwrite rax). */
            /* Its one reader right after it, taking it as a memory operand
             * (pass_x86_loadop): one instruction for the two. */
            if (afolded(i->a)
                    ? loadop_fuse(text, fn, n, usecnt, REG_RBP, -1, 1,
                                  g_afold.disp[i->a], rmw_op)
                    : in_reg(i->a) &&
                      loadop_fuse(text, fn, n, usecnt, g_loc[i->a], -1, 1,
                                  0, rmw_op)) {
                n++;
                break;
            }
            if (afolded(i->a)
                    ? cmpmem_defer(fn, n, usecnt, REG_RBP, -1, 1,
                                   g_afold.disp[i->a])
                    : in_reg(i->a) &&
                      cmpmem_defer(fn, n, usecnt, g_loc[i->a], -1, 1, 0))
                break;                     /* the compare reads it */
            /* The address folded into rbp+disp: no base register at all. */
            if (afolded(i->a)) {
                int D = in_reg(i->dst) ? g_loc[i->dst] : REG_RAX;
                x86_load_reg_basedisp(text, D, REG_RBP, g_afold.disp[i->a],
                                      i->size, i->sign, i->w);
                if (D == REG_RAX) cg_store(text, sd, i->dst, i->w);
                else              cg_reset();
                break;
            }
            if (in_reg(i->dst)) {
                if (in_reg(i->a)) {
                    x86_load_base_reg(text, g_loc[i->dst], g_loc[i->a],
                                      i->size, i->sign, i->w);
                    break;
                }
                cg_load(text, sd, i->a, 8, 0, 8);       /* the address -> rax */
                x86_load_base_reg(text, g_loc[i->dst], REG_RAX,
                                  i->size, i->sign, i->w);
                break;
            }
            /* Address already in a register: load straight from [reg], skipping
             * the `mov reg,rax`. dst is a temp (cacheable), so cg_store below
             * fixes the residency cache. */
            if (in_reg(i->a)) {
                x86_load_base_rax(text, g_loc[i->a], i->size, i->sign, i->w);
                cg_store(text, sd, i->dst, i->w);
                break;
            }
            cg_load(text, sd, i->a, 8, 0, 8);       /* the address */
            x86_load_mem_rax(text, i->size, i->sign, i->w);
            cg_store(text, sd, i->dst, i->w);
            break;
        case IR_STORE: {
            if (is_flt(i->b)) {
                int v = x86_frd(text, sd, i->b, X86_FSCR, i->size);
                if (afolded(i->a))
                    x86_movs_store(text, v, g_afold.disp[i->a], i->size);
                else
                    x86_movs_store_base(text, addr_reg(text, sd, i->a), 0, v,
                                        i->size);
                break;
            }
            /* Address already in a register (mirrors IR_LOAD): store straight to
             * [reg], skipping the slot->rcx load — which is what lets the address
             * temp be register-allocated at all (its OPAQUE marking is dropped). */
            /* And the VALUE the same way. A register-resident one was
             * moved to RAX and stored from there -- two instructions
             * where `mov %rN,(%rM)` is one, on every store of a value
             * the allocator had already placed. RAX is left alone,
             * which also leaves its residency cache standing: a store
             * cannot change what RAX holds, because what RAX caches is
             * a temp or a register-resident local, and neither of those
             * is reachable through a pointer. */
            int vreg = in_reg(i->b) ? g_loc[i->b] : REG_RAX;
            if (afolded(i->a)) {
                if (vreg == REG_RAX)
                    cg_load(text, sd, i->b, 8, 0, 8);
                x86_store_mem_reg(text, REG_RBP, g_afold.disp[i->a], vreg,
                                  i->size);
                break;
            }
            if (in_reg(i->a)) {
                if (vreg == REG_RAX)
                    cg_load(text, sd, i->b, 8, 0, 8);       /* the value -> rax */
                x86_store_mem_reg(text, g_loc[i->a], 0, vreg, i->size);
                break;
            }
            x86_mov_rcx_slot(text, sd[i->a]);       /* the address -> rcx */
            if (vreg == REG_RAX) {
                cg_load(text, sd, i->b, 8, 0, 8);   /* the value -> rax */
                x86_store_mem_rcx(text, i->size);
                break;
            }
            x86_store_mem_reg(text, REG_RCX, 0, vreg, i->size);
            break;
        }
        case IR_EXT:
            /* already zero-extended in the register it shares (g_zx32) */
            if (i->size == 4 && !i->sign && i->w == 8 && g_zx32 &&
                i->a >= 0 && g_zx32[i->a] && !afolded(i->a) &&
                in_reg(i->dst) && in_reg(i->a) &&
                g_loc[i->dst] == g_loc[i->a])
                break;
            /* re-extend from the low `size` bytes of the temp's slot */
            if (in_reg(i->dst)) {
                cg_ext_into(text, sd, g_loc[i->dst], i->a, i->size, i->sign, i->w);
                break;
            }
            cg_load(text, sd, i->a, i->size, i->sign, i->w);
            cg_store(text, sd, i->dst, i->w);
            break;
        case IR_BSWAP:
            cg_load(text, sd, i->a, i->size, 0, i->size == 8 ? 8 : 4);
            x86_bswap(text, i->size);
            cg_store(text, sd, i->dst, i->w);
            break;
        case IR_FENCE:
            x86_mfence(text);   /* leaves RAX untouched: cache stays valid */
            break;
        case IR_UD2:
            x86_ud2(text);
            break;
        /* The atomics read their operands where they live (atomic_in_reg):
         * an address or a value the allocator put in a register is used
         * there, and only one left in its slot is loaded -- the address
         * into rcx, cmpxchg's desired value into rdx, ARMW's operand
         * into rsi. rax is the one fixed register every one of them
         * needs (the value exchanged, added, or compared), and it is in
         * no pool, so nothing an operand lives in is overwritten by
         * filling it; rcx is in no pool either, and x86_pool_for keeps
         * rdx and rsi out of a function whose atomics load them. */
        case IR_XCHG: case IR_XADD: {
            int A = x86_atomic_addr(text, sd, i->a);
            cg_load(text, sd, i->b, i->size, 0,
                    i->size == 8 ? 8 : 4);               /* value -> rax */
            if (i->op == IR_XCHG)
                x86_xchg_reg_mem(text, REG_RAX, A, i->size); /* rax = old */
            else
                x86_lock_xadd_reg_mem(text, REG_RAX, A, i->size);
            cg_reset();
            cg_store(text, sd, i->dst, i->w);
            break;
        }
        case IR_CMPXCHG: {
            /* *b is the expected value and gets the value seen when they
             * differ -- written back only then, as C11 says: on a match
             * `expected` is not written at all. b is an address held
             * across the compare-exchange; left in its slot it is read
             * twice, through rax before it and rcx after (the object's
             * address in rcx is finished with by then). */
            int C = x86_atomic_val(text, sd, i->c, REG_RDX, i->size);
            int A = x86_atomic_addr(text, sd, i->a);
            int B = in_reg(i->b) ? g_loc[i->b] : REG_RAX;
            if (B == REG_RAX)
                x86_load_slot(text, sd[i->b], 8, 0, 8);  /* &expected */
            x86_load_reg_mem(text, REG_RAX, B, 0, i->size); /* rax = *exp */
            x86_lock_cmpxchg_reg_mem(text, C, A, i->size); /* ZF=matched */
            int skip = x86_jz_rel8(text);
            if (B == REG_RAX) {
                x86_mov_rcx_slot(text, sd[i->b]);
                B = REG_RCX;
            }
            x86_store_mem_reg(text, B, 0, REG_RAX, i->size); /* *exp=seen */
            text->p[skip] = (unsigned char)(text->len - (skip + 1));
            x86_setcc_eax(text, 0x94);                   /* setz: dst = matched */
            cg_reset();
            cg_store(text, sd, i->dst, 4);
            break;
        }
        case IR_ARMW: {
            /* and / or / xor / nand: x86 has no locked fetch-and-OP that
             * returns the old value, so this is the compare-and-swap loop gcc
             * emits. rax is the value last seen, rdx the value to install;
             * lock cmpxchg installs rdx only if memory still holds rax, and
             * otherwise reloads rax with what it does hold, so the loop
             * recomputes from the fresh value. */
            int w = i->size == 8 ? 8 : 4;
            int op = (int)i->imm;
            int A = x86_atomic_addr(text, sd, i->a);
            int B = x86_atomic_val(text, sd, i->b, REG_RSI, i->size);
            x86_load_reg_mem(text, REG_RAX, A, 0, i->size);  /* current */
            int loop = text->len;
            x86_mov_reg_reg(text, REG_RDX, REG_RAX);
            x86_alu_rr(text, op == 'n' ? '&' : op, REG_RDX, B, w);
            if (op == 'n')
                x86_not_reg(text, REG_RDX, w);
            x86_lock_cmpxchg_reg_mem(text, REG_RDX, A, i->size); /* ZF: installed */
            int back = x86_jnz_rel32(text);
            code_patch32(text, back, (unsigned long)(long)(loop - (back + 4)));
            cg_reset();
            cg_store(text, sd, i->dst, i->w);             /* rax = the old value */
            break;
        }
        case IR_CAS: {
            /* By value: rax = expected, the desired value in a register.
             * After lock cmpxchg rax holds the value that was in memory
             * whether or not the swap happened (on success it already was
             * that value). */
            int C = x86_atomic_val(text, sd, i->c, REG_RDX, i->size);
            int A = x86_atomic_addr(text, sd, i->a);
            cg_load(text, sd, i->b, i->size, 0,
                    i->size == 8 ? 8 : 4);                /* expected -> rax */
            x86_lock_cmpxchg_reg_mem(text, C, A, i->size);
            cg_reset();
            cg_store(text, sd, i->dst, i->w);
            break;
        }
        case IR_CAS16:
            break;                      /* (gen_i128's) */
        case IR_FRAMEADDR:
            cg_reset();
            x86_mov_reg_reg(text, REG_RAX, REG_RBP);
            cg_store(text, sd, i->dst, 8);
            break;
        case IR_ALLOCA: {
            /* rsp -= round16(size + pad); the block starts above the
             * outgoing area (which moves down with rsp), at a 16-aligned
             * address: pad lifts an 8-mod-16 outgoing size to 16 without
             * reaching into the locals above the old outgoing area. */
            int og = fn->outgoing_bytes;
            int pad = ((og + 15) & ~15) - og;
            cg_load(text, sd, i->a, 8, 0, 8);
            cg_reset();
            x86_alu_reg_imm(text, '+', REG_RAX, 15 + pad, 8);
            x86_alu_reg_imm(text, '&', REG_RAX, -16, 8);
            x86_alu_rr(text, '-', REG_RSP, REG_RAX, 8);
            x86_mov_reg_reg(text, REG_RAX, REG_RSP);
            if (og + pad)
                x86_alu_reg_imm(text, '+', REG_RAX, og + pad, 8);
            cg_store(text, sd, i->dst, 8);
            break;
        }
        case IR_SPSAVE:
            cg_reset();
            x86_mov_reg_reg(text, REG_RAX, REG_RSP);
            cg_store(text, sd, i->dst, 8);
            break;
        case IR_SPRESTORE:
            cg_load(text, sd, i->a, 8, 0, 8);
            x86_mov_reg_reg(text, REG_RSP, REG_RAX);
            break;
        case IR_MEMCPY: {
            /* a struct copy: 8 bytes at a time, then the tail. A register-held
             * address is used directly as the base (no slot->rcx/rdx load) — the
             * reason its temp can be register-allocated (OPAQUE dropped). */
            cg_reset();
            /* A LARGE one is `rep movsq`. Unrolled, every eight bytes were
             * two instructions: the loop-idiom pass turns a copy loop over a
             * whole array into one of these, and EmbLinkOs's colour picker
             * copied a 113 KB buffer in 28,000 of them -- 199 KB of code in
             * one function, where gcc calls memcpy. rsi and rdi may hold
             * allocated values, so they are pushed around it; the addresses
             * are fetched into rax and rcx first, before the pushes move
             * rsp (a slot is rbp-relative, but nothing here needs to know
             * that to be right). */
            if (i->size > X86_REP_MIN) {
                if (afolded(i->a)) x86_lea_reg_slot(text, REG_RAX, g_afold.disp[i->a]);
                else if (in_reg(i->a)) x86_mov_reg_reg(text, REG_RAX, g_loc[i->a]);
                else              x86_load_slot(text, sd[i->a], 8, 0, 8);
                if (afolded(i->b)) x86_lea_reg_slot(text, REG_RCX, g_afold.disp[i->b]);
                else if (in_reg(i->b)) x86_mov_reg_reg(text, REG_RCX, g_loc[i->b]);
                else              x86_mov_rcx_slot(text, sd[i->b]);
                x86_push_reg(text, REG_RSI);
                x86_push_reg(text, REG_RDI);
                x86_mov_reg_reg(text, REG_RDI, REG_RAX);
                x86_mov_reg_reg(text, REG_RSI, REG_RCX);
                x86_mov_reg_imm(text, REG_RCX, i->size / 8, 4);
                x86_rep_movsq(text);
                for (int off = 0; off < i->size % 8; ) {   /* rsi, rdi: past it */
                    int rest = i->size % 8 - off;
                    int chunk = rest >= 4 ? 4 : rest >= 2 ? 2 : 1;
                    x86_load_reg_mem(text, REG_RAX, REG_RSI, off, chunk);
                    x86_store_mem_reg(text, REG_RDI, off, REG_RAX, chunk);
                    off += chunk;
                }
                x86_pop_reg(text, REG_RDI);
                x86_pop_reg(text, REG_RSI);
                break;
            }
            int dbase, sbase, ddisp = 0, sdisp = 0;
            if (afolded(i->a)) { dbase = REG_RBP; ddisp = g_afold.disp[i->a]; }
            else if (in_reg(i->a)) dbase = g_loc[i->a];
            else { x86_load_slot(text, sd[i->a], 8, 0, 8);
                   x86_mov_reg_reg(text, REG_RCX, REG_RAX); dbase = REG_RCX; }
            /* A source address in memory is read again for each chunk,
             * into rax. It went through rdx, which is in the pool of every
             * function that neither divides nor has an atomic -- and so held
             * some other value's home that the copy then overwrote. rax and
             * rcx are the only registers no pool contains (see the call's
             * struct-argument copy, which re-reads the same way). */
            sbase = in_reg(i->b) ? g_loc[i->b] : -1;
            if (afolded(i->b)) { sbase = REG_RBP; sdisp = g_afold.disp[i->b]; }
            int off = 0;
            /* sixteen bytes a move where SSE is allowed, through the float
             * scratch, which no pool contains */
            for (; !g_no_sse && i->size - off >= 16; off += 16) {
                int sb = sbase;
                if (sb < 0) {
                    x86_load_slot(text, sd[i->b], 8, 0, 8);
                    sb = REG_RAX;
                }
                x86_vload_base(text, X86_FSCR, sb, (sb == sbase ? sdisp : 0) + off);
                x86_vstore_base(text, dbase, ddisp + off, X86_FSCR);
            }
            while (off < i->size) {
                int chunk = i->size - off;
                chunk = chunk >= 8 ? 8 : chunk >= 4 ? 4 : chunk >= 2 ? 2 : 1;
                if (sbase < 0) {
                    x86_load_slot(text, sd[i->b], 8, 0, 8);
                    x86_load_reg_mem(text, REG_RAX, REG_RAX, off, chunk);
                } else {
                    x86_load_reg_mem(text, REG_RAX, sbase, sdisp + off, chunk);
                }
                x86_store_mem_reg(text, dbase, ddisp + off, REG_RAX, chunk);
                off += chunk;
            }
            break;
        }
        case IR_MEMZERO: {
            cg_reset();
            if (i->size > X86_REP_MIN) {        /* see IR_MEMCPY */
                if (afolded(i->a)) x86_lea_reg_slot(text, REG_RCX, g_afold.disp[i->a]);
                else if (in_reg(i->a)) x86_mov_reg_reg(text, REG_RCX, g_loc[i->a]);
                else              x86_mov_rcx_slot(text, sd[i->a]);
                x86_push_reg(text, REG_RDI);
                x86_mov_reg_reg(text, REG_RDI, REG_RCX);
                x86_mov_eax_imm(text, 0, 8);
                x86_mov_reg_imm(text, REG_RCX, i->size / 8, 4);
                x86_rep_stosq(text);
                for (int off = 0; off < i->size % 8; ) {
                    int rest = i->size % 8 - off;
                    int chunk = rest >= 4 ? 4 : rest >= 2 ? 2 : 1;
                    x86_store_mem_reg(text, REG_RDI, off, REG_RAX, chunk);
                    off += chunk;
                }
                x86_pop_reg(text, REG_RDI);
                break;
            }
            int dbase, ddisp = 0;
            if (afolded(i->a)) { dbase = REG_RBP; ddisp = g_afold.disp[i->a]; }
            else if (in_reg(i->a)) dbase = g_loc[i->a];
            else { x86_load_slot(text, sd[i->a], 8, 0, 8);
                   x86_mov_reg_reg(text, REG_RCX, REG_RAX); dbase = REG_RCX; }
            int off = 0;
            if (!g_no_sse && i->size >= 16) {           /* as IR_MEMCPY */
                x86_vzero(text, X86_FSCR);
                for (; i->size - off >= 16; off += 16)
                    x86_vstore_base(text, dbase, ddisp + off, X86_FSCR);
            }
            if (off < i->size)
                x86_mov_eax_imm(text, 0, 8);
            while (off < i->size) {
                int chunk = i->size - off;
                chunk = chunk >= 8 ? 8 : chunk >= 4 ? 4
                      : chunk >= 2 ? 2 : 1;
                x86_store_mem_reg(text, dbase, ddisp + off, REG_RAX, chunk);
                off += chunk;
            }
            break;
        }
        case IR_LABEL:
            cg_reset();      /* a merge point: RAX is unknown here */
            label_off[i->label] = text->len;
            break;
        case IR_JMP:
            /* A jump whose target label is the next thing EMITTED is
             * the instruction after it. 143 of them across lib/libc and
             * lib/libcxx -- the optimizer's CFG cleanup threads jumps
             * between blocks, and these are the ones where the block in
             * between turns out to occupy no bytes: other labels, and
             * copies whose two ends the allocator gave one register. */
            {
                int m = n + 1;
                while (m < fn->nins) {
                    struct ir_ins *x = &fn->ins[m];
                    if (x->op == IR_LABEL) {
                        if (x->label == i->label) break;
                        m++; continue;
                    }
                    if (x->op == IR_MOV && x->dst < 0) { m++; continue; }
                    if ((x->op == IR_MOV || x->op == IR_LDVAR ||
                         x->op == IR_STVAR) &&
                        in_reg(x->dst) && in_reg(x->a) &&
                        g_loc[x->dst] == g_loc[x->a]) { m++; continue; }
                    break;
                }
                if (m < fn->nins && fn->ins[m].op == IR_LABEL &&
                    fn->ins[m].label == i->label)
                    break;
            }
            /* fall through */
        case IR_BRZ:
        case IR_BRNZ: {
            int patch;
            int ord = g_brord;
            int sh;
            if (i->op == IR_JMP) {
                sh = br_short();
                patch = sh ? x86_jmp_rel8(text) : x86_jmp_rel32(text);
            } else {
                /* A register-resident condition is tested where it
                 * lives. The detour through RAX was a move and a test
                 * where one test does, on the single hottest pair of
                 * instructions a loop has. */
                if (in_reg(i->a))
                    x86_test_rr(text, g_loc[i->a], g_loc[i->a], i->w);
                else {
                    cg_load(text, sd, i->a, i->w, 0, i->w);
                    x86_test_eax(text, i->w);
                }
                sh = br_short();
                patch = i->op == IR_BRZ
                    ? (sh ? x86_jz_rel8(text)  : x86_jz_rel32(text))
                    : (sh ? x86_jnz_rel8(text) : x86_jnz_rel32(text));
            }
            cg_reset();      /* control splits: don't carry RAX across */
            if (nbrs == capbrs) {
                capbrs = capbrs ? capbrs * 2 : 16;
                brs = xrealloc(brs, (size_t)capbrs * sizeof *brs);
            }
            brs[nbrs].patch_off = patch;
            brs[nbrs].label = i->label;
            brs[nbrs].size = sh ? 1 : 4;
            brs[nbrs].ord = ord;
            brs[nbrs].tab = 0;
            nbrs++;
            break;
        }
        case IR_LABELADDR: {
            if (cg_label_mark(i))       /* static data's marker: no code */
                break;
            /* dst = &&label: `lea rax,[rip+disp32]`, the disp32 patched to the
             * label's code offset via the SAME list and formula as a rel32
             * branch (target - (patch_off + 4)). */
            int patch = x86_lea_rax_rip(text);
            if (nbrs == capbrs) {
                capbrs = capbrs ? capbrs * 2 : 16;
                brs = xrealloc(brs, (size_t)capbrs * sizeof *brs);
            }
            brs[nbrs].patch_off = patch;
            brs[nbrs].label = i->label;
            brs[nbrs].size = 4;       /* a `lea`, not a branch */
            brs[nbrs].ord = -1;
            brs[nbrs].tab = 0;
            nbrs++;
            cg_store(text, sd, i->dst, 8);
            break;
        }
        case IR_IGOTO:
            /* goto *a: jump to the computed code address held in a. */
            cg_load(text, sd, i->a, 8, 0, 8);
            cg_reset();
            x86_jmp_reg(text, REG_RAX);
            break;
        case IR_SWITCH: {
            /* A jump table, in .text right after its dispatch and
             * holding 32-bit offsets from its own start, so it needs no
             * relocation:
             *     cmp rax, n ; jae default
             *     lea rcx, [rip + table]
             *     movsxd rax, [rcx + rax*4] ; add rax, rcx ; jmp rax
             * rax is the index (zero-extended at width 4: cg_load's
             * unsigned read) and rcx the table -- both scratch, and a
             * terminator keeps nothing live in either. The entries go on
             * the branch list with `tab` set, and are patched to
             * target - table. */
            /* The index is used where it is when its register is known
             * to hold it zero-extended: always at width 8, and at width 4
             * when the instruction just before is the 32-bit load that
             * wrote it, which zeroes the upper half as every 32-bit
             * write does. That is an interpreter's `op = prog[pc++];
             * switch (op)`, where copying the opcode into rax was an
             * instruction on every dispatch. Not when the index lives in
             * rcx, which the table's address takes, and only for ELF,
             * whose dispatch indexes the table directly. */
            int idx = REG_RAX;
            {
                const struct ir_ins *pv = i > fn->ins ? i - 1 : NULL;
                if (in_reg(i->a) && g_loc[i->a] != REG_RCX &&
                    target_fmt_get() == TGT_FMT_ELF &&
                    !getenv("EMBCC_NO_SWIDX") &&
                    (i->w == 8 ||
                     (i->w == 4 && pv && pv->op == IR_LOAD &&
                      pv->dst == i->a && pv->w == 4 && pv->size <= 4 &&
                      !pv->flt)))
                    idx = g_loc[i->a];
            }
            int n = fn->jt[i->jt].n;
            if (idx == REG_RAX)
                cg_load(text, sd, i->a, i->w, 0, i->w);
            cg_reset();
            x86_alu_reg_imm(text, 'c', idx, n, i->w);
            int patch = x86_jcc_rel32(text, 0x93);      /* jae: unsigned >= n */
            if (nbrs == capbrs) {
                capbrs = capbrs ? capbrs * 2 : 16;
                brs = xrealloc(brs, (size_t)capbrs * sizeof *brs);
            }
            brs[nbrs].patch_off = patch;
            brs[nbrs].label = i->label;
            brs[nbrs].size = 4;
            brs[nbrs].ord = -1;
            brs[nbrs].tab = 0;
            nbrs++;
            int lea = x86_lea_reg_rip(text, REG_RCX);
            /* In an ELF object the entries are absolute addresses --
             * EmbCC's code is position-dependent (it refuses -fPIC), so
             * a relocation per entry costs nothing at run time -- and the
             * dispatch is one indirect jump through the table, where the
             * 32-bit offsets needed a sign-extending load and an add
             * first: two instructions on every switch taken, every
             * opcode an interpreter dispatches. The entries are relocated
             * against this function's own symbol, at each label's offset
             * into it (jtabs, resolved with the branches below). Mach-O
             * and COFF keep the offset table. */
            if (target_fmt_get() == TGT_FMT_ELF) {
                x86_jmp_rcx_reg8(text, idx);
                code_align(text, 8, 0xcc);
                int tab = text->len;
                code_patch32(text, lea,
                             (unsigned long)(unsigned int)(tab - (lea + 4)));
                for (int k = 0; k < n; k++) {
                    if (njtabs == capjtabs) {
                        capjtabs = capjtabs ? capjtabs * 2 : 16;
                        jtabs = xrealloc(jtabs, (size_t)capjtabs * sizeof *jtabs);
                    }
                    jtabs[njtabs].off = text->len;
                    jtabs[njtabs].label = fn->jt[i->jt].labels[k];
                    njtabs++;
                    code_u32(text, 0);
                    code_u32(text, 0);
                }
                break;
            }
            x86_movsxd_rax_tab(text);
            x86_alu_rr(text, '+', REG_RAX, REG_RCX, 8);
            x86_jmp_reg(text, REG_RAX);
            code_align(text, 4, 0xcc);                  /* int3 padding */
            int tab = text->len;
            code_patch32(text, lea, (unsigned long)(unsigned int)(tab - (lea + 4)));
            for (int k = 0; k < n; k++) {
                if (nbrs == capbrs) {
                    capbrs = capbrs ? capbrs * 2 : 16;
                    brs = xrealloc(brs, (size_t)capbrs * sizeof *brs);
                }
                brs[nbrs].patch_off = text->len;
                brs[nbrs].label = fn->jt[i->jt].labels[k];
                brs[nbrs].size = 4;
                brs[nbrs].ord = -1;
                brs[nbrs].tab = tab + 1;
                nbrs++;
                code_u32(text, 0);
            }
            break;
        }
        case IR_CALL: {
            /* SysV walks TWO register files independently: integers and
             * pointers take rdi..r9, floats take xmm0..7. */
            cg_reset();     /* a call clobbers every caller-saved register */
            int ireg = 0, freg = 0;
            int is_tail = g_tailcalls && tail_call_ok(fn, n);

            /* Microsoft x64: an aggregate whose size is not exactly 1,
             * 2, 4 or 8 bytes travels BY REFERENCE, and the copy is the
             * CALLER's -- the callee may write to it, so passing the
             * original would let a callee modify its caller's variable.
             * Made here, while rax and rcx are still free, into this
             * frame's scratch area; only the address goes in the slot. */
            if (target_win64_abi())
                for (int k = 0; k < i->nargs; k++) {
                    struct ir_arg *a = &i->argv[k];
                    if (!a->byref)
                        continue;
                    /* a->vreg holds the struct's ADDRESS, not its value. */
                    x86_load_reg_mem(text, REG_RCX, REG_RBP, sd[a->vreg], 8);
                    for (int off = 0; off < a->size; off += 8) {
                        int chunk = a->size - off >= 8 ? 8 : a->size - off;
                        x86_load_reg_mem(text, REG_RAX, REG_RCX, off, chunk);
                        x86_store_mem_reg(text, REG_RBP,
                                          scratch_base + a->copy_off + off,
                                          REG_RAX, chunk);
                    }
                }

            /* MEMORY-class aggregates go to the outgoing area first.
             *
             * The scratch for that copy has to be a register the
             * ALLOCATOR never hands out, not merely one the argument
             * sequence has not reached yet. This said "rax/rcx/rdx are
             * still free" and used rdx, which stopped being true when
             * rdx joined the pool: `mem(g, 1, 2, 3, 4)` materialised 3
             * into rdx and then the struct copy used rdx as its source
             * pointer and lost it (tests/exec/struct-param-scalars.c).
             * It took an ABI hint to make the allocator choose rdx here
             * reliably, but nothing stopped it choosing rdx before.
             * rax and rcx are in no pool; those two are the scratch. */
            for (int k = 0; k < i->nargs; k++) {
                struct ir_arg *a = &i->argv[k];
                if (!a->on_stack)
                    continue;
                if (!a->is_struct && a->size == 16) {
                    /* long double: its 16 bytes, from its own slot */
                    for (int q = 0; q < 16; q += 8) {
                        x86_load_slot(text, sd[a->vreg] + q, 8, 0, 8);
                        x86_store_mem_reg(text, REG_RSP, a->stk_off + q,
                                          REG_RAX, 8);
                    }
                    continue;
                }
                if (!a->is_struct) {
                    /* a scalar that ran out of registers: its slot
                     * already holds the value, extended to 8 bytes (or it is
                     * register-resident -> store the register straight out).
                     * A float in an xmm home has no slot at all: this read
                     * one, and -O2 refused `h(8 doubles, x * 3)`. */
                    if (in_freg(a->vreg)) {
                        x86_movs_store_base(text, REG_RSP, a->stk_off,
                                            g_floc[a->vreg], a->size);
                    } else if (in_reg(a->vreg)) {
                        x86_store_mem_reg(text, REG_RSP, a->stk_off,
                                          g_loc[a->vreg], 8);
                    } else {
                        x86_load_slot(text, sd[a->vreg], 8, 0, 8);
                        x86_store_mem_reg(text, REG_RSP, a->stk_off,
                                          REG_RAX, 8);
                    }
                    continue;
                }
                int sz = a->size;
                /* Built in place (x86_inplace_locals): the copy would be
                 * of the argument onto itself. */
                if (afolded(a->vreg) &&
                    g_afold.disp[a->vreg] == a->stk_off - frame)
                    continue;
                /* The source address in rcx for the whole copy: rax and
                 * rcx are in no pool, and nothing in this stretch of the
                 * call sequence -- the Win64 by-reference copy above does
                 * the same -- has put anything in rcx yet. It was re-read
                 * from its slot for every eightbyte, three instructions
                 * where two do: EmbLinkOs's UI passes a 168-byte EmProps
                 * by value to every widget call. */
                if (afolded(a->vreg))
                    x86_lea_reg_slot(text, REG_RCX, g_afold.disp[a->vreg]);
                else
                    x86_load_reg_mem(text, REG_RCX, REG_RBP, sd[a->vreg], 8);
                if (sz > X86_REP_MIN) {
                    /* rep movsq, as IR_MEMCPY: the destination is
                     * rsp-relative, so it is computed before the pushes */
                    x86_lea_reg_basedisp(text, REG_RAX, REG_RSP, a->stk_off, 8);
                    x86_push_reg(text, REG_RSI);
                    x86_push_reg(text, REG_RDI);
                    x86_mov_reg_reg(text, REG_RDI, REG_RAX);
                    x86_mov_reg_reg(text, REG_RSI, REG_RCX);
                    x86_mov_reg_imm(text, REG_RCX, sz / 8, 4);
                    x86_rep_movsq(text);
                    for (int off = 0; off < sz % 8; ) {
                        int rest = sz % 8 - off;
                        int chunk = rest >= 4 ? 4 : rest >= 2 ? 2 : 1;
                        x86_load_reg_mem(text, REG_RAX, REG_RSI, off, chunk);
                        x86_store_mem_reg(text, REG_RDI, off, REG_RAX, chunk);
                        off += chunk;
                    }
                    x86_pop_reg(text, REG_RDI);
                    x86_pop_reg(text, REG_RSI);
                    continue;
                }
                int off = 0;
                for (; !g_no_sse && sz - off >= 16; off += 16) {
                    x86_vload_base(text, X86_FSCR, REG_RCX, off);
                    x86_vstore_base(text, REG_RSP, a->stk_off + off, X86_FSCR);
                }
                for (; off < sz; ) {
                    int chunk = sz - off;
                    chunk = chunk >= 8 ? 8 : chunk >= 4 ? 4
                          : chunk >= 2 ? 2 : 1;
                    x86_load_reg_mem(text, REG_RAX, REG_RCX, off, chunk);
                    x86_store_mem_reg(text, REG_RSP,
                                      a->stk_off + off, REG_RAX, chunk);
                    off += chunk;
                }
            }
            /* A struct returned in MEMORY takes the FIRST argument
             * register as a hidden pointer to the caller's scratch,
             * before any real argument -- rdi under System V and rcx
             * under Microsoft x64. Hardcoding rdi put the pointer in a
             * register Windows does not read and left rcx holding the
             * first real argument, so the callee wrote its result
             * through whatever that happened to be. */
            int sret_lea = 0;
            if (i->retsize && i->retnclass == 0 && !i->ret_x87) {
                /* Recorded, not emitted: this writes argument register
                 * zero, and the parallel move below still has to READ
                 * the registers its sources live in -- one of which can
                 * be that one now that rdi is allocatable. Emitting it
                 * here cost every float in <format>, whose result is a
                 * std::string and so travels through this pointer. */
                sret_lea = 1;
                ireg++;
                /* Only Microsoft x64 keeps the two counters in step --
                 * the hidden pointer consumes SLOT zero there, so the
                 * first real float is xmm1. Under System V the files
                 * are independent and the pointer costs the float
                 * count nothing. */
                if (target_win64_abi())
                    freg = ireg;
            }
            /* Register arguments already in a register (r8..r15/rbx) are moved
             * as ONE parallel move: an argument sitting in r8/r9 must not be
             * clobbered by an earlier argument's write to that same register.
             * The indirect target (-> r11) joins the same shuffle. Memory- and
             * xmm-sourced placements read from the frame, so they cannot clobber
             * a register source and are emitted afterward. rax is the scratch:
             * not an argument register, and free until the al count below. */
            int mvdest[16], mvsrc[16], nmv = 0;
            {
                /* Only the INTEGER argument registers are walked here: a
                 * float argument is never register-resident (g_loc leaves
                 * it in memory), so the xmm index cannot source a move. */
                int pireg = ireg;
                for (int k = 0; k < i->nargs; k++) {
                    struct ir_arg *a = &i->argv[k];
                    if (a->on_stack)
                        continue;
                    if (target_win64_abi()) {
                        /* One slot per argument, whatever its class --
                         * so a float still CONSUMES its integer
                         * register rather than skipping it. A struct is
                         * loaded from memory either way, so it is not a
                         * parallel-move source. */
                        if (!a->is_struct && a->cls[0] != CLASS_SSE &&
                            in_reg(a->vreg)) {
                            mvdest[nmv] = x86_argreg(pireg);
                            mvsrc[nmv] = g_loc[a->vreg]; nmv++;
                        }
                        pireg++;
                        continue;
                    }
                    if (a->is_struct || a->nclass == 2) {
                        for (int q = 0; q < a->nclass; q++)
                            if (a->cls[q] == CLASS_INTEGER) pireg++;
                    } else if (a->cls[0] != CLASS_SSE) {
                        if (in_reg(a->vreg)) {
                            mvdest[nmv] = x86_argreg(pireg++);
                            mvsrc[nmv] = g_loc[a->vreg]; nmv++;
                        } else {
                            pireg++;
                        }
                    }
                }
                if (i->indirect && in_reg(i->a)) {
                    mvdest[nmv] = 11 /*r11*/; mvsrc[nmv] = g_loc[i->a]; nmv++;
                }
            }
            int afmv_dst[16], afmv_src[16], nafmv = 0;
            int afld_reg[16], afld_v[16], afld_sz[16], nafld = 0;
            emit_reg_parallel_move(text, mvdest, mvsrc, nmv, REG_RAX);
            if (sret_lea)                 /* now that nothing reads rdi */
                x86_lea_reg_slot(text, x86_argreg(0),
                                 scratch_base + i->scratch);
            for (int k = 0; k < i->nargs; k++) {
                struct ir_arg *a = &i->argv[k];
                if (a->on_stack)
                    continue; /* placed above */
                /* Microsoft x64 FIRST, because its answer for a struct
                 * is not a special case of System V's: an aggregate in
                 * a slot is an ADDRESS there, and the eightbyte
                 * classification below would put its contents in the
                 * registers the callee reads a pointer from. */
                if (target_win64_abi()) {
                    if (a->byref) {
                        /* the address of the copy made above */
                        x86_lea_reg_slot(text, x86_argreg(ireg),
                                         scratch_base + a->copy_off);
                    } else if (a->is_struct) {
                        /* 1, 2, 4 or 8 bytes: the VALUE, in one
                         * register, loaded from the struct's address. */
                        x86_load_slot(text, sd[a->vreg], 8, 0, 8);
                        x86_load_reg_mem(text, x86_argreg(ireg), REG_RAX, 0,
                                         a->size);
                    } else if (a->cls[0] == CLASS_SSE) {
                        x86_fld(text, sd, a->vreg, ireg, a->size);
                        /* a variadic double goes in the integer register
                         * too -- copied from the xmm register just loaded,
                         * since an xmm-homed value has no slot to read */
                        if (i->call_varargs)
                            x86_movq_gpr_xmm(text, x86_argreg(ireg), ireg, 8);
                    } else if (in_freg(a->vreg)) {
                        x86_movq_gpr_xmm(text, x86_argreg(ireg),
                                         g_floc[a->vreg], a->size == 8 ? 8 : 4);
                    } else if (!in_reg(a->vreg)) {
                        x86_load_arg(text, ireg, sd[a->vreg]);
                    }
                    ireg++;
                    freg = ireg;
                    continue;
                }
                if (a->is_struct) {
                    x86_load_slot(text, sd[a->vreg], 8, 0, 8);
                    for (int q = 0; q < a->nclass; q++) {
                        if (a->cls[q] == CLASS_SSE)
                            x86_movs_load_base(text, freg++, REG_RAX,
                                               q * 8, 8);
                        else if (a->cls[q] != CLASS_NONE)
                            x86_load_reg_mem(text, x86_argreg(ireg++),
                                             REG_RAX, q * 8, 8);
                    }
                    continue;
                }
                if (a->nclass == 2) {           /* an __int128 */
                    x86_load_arg(text, ireg++, sd[a->vreg]);
                    x86_load_arg(text, ireg++, sd[a->vreg] + 8);
                } else if (a->cls[0] == CLASS_SSE) {
                    /* Same story as the integer file: with the FP pool
                     * AT xmm0-7, an argument's source register and
                     * another argument's destination are drawn from one
                     * set, so the register-resident ones go out as a
                     * PERMUTATION. The memory-resident ones are loaded
                     * after it, into argument registers the permutation
                     * has finished reading. */
                    if (in_freg(a->vreg)) {
                        afmv_dst[nafmv] = freg++;
                        afmv_src[nafmv] = g_floc[a->vreg];
                        nafmv++;
                    } else {
                        afld_reg[nafld] = freg++;
                        afld_v[nafld] = a->vreg;
                        afld_sz[nafld] = a->size;
                        nafld++;
                    }
                } else if (in_reg(a->vreg))
                    ireg++;              /* already placed by the parallel move */
                else if (in_freg(a->vreg))
                    /* An integer argument whose value lives in an xmm
                     * register: its other uses are floating point, and a
                     * call's integer argument is only a SOFT vote against
                     * that (float_vregs) -- the merged `const 0` that is
                     * both a char argument and the 0.0f of a subtraction.
                     * It has no slot; its bits go across with movq/movd
                     * (fuzz seeds 7306 and 7581). */
                    x86_movq_gpr_xmm(text, x86_argreg(ireg++),
                                     g_floc[a->vreg], a->size == 8 ? 8 : 4);
                else
                    x86_load_arg(text, ireg++, sd[a->vreg]);
            }
            if (nafmv) emit_fp_parallel_move(text, afmv_dst, afmv_src, nafmv);
            for (int k = 0; k < nafld; k++)
                x86_fld(text, sd, afld_v[k], afld_reg[k], afld_sz[k]);
            if (i->indirect && !in_reg(i->a))
                x86_mov_r11_slot(text, sd[i->a]);   /* in-reg case done above */
            /* al = the number of VECTOR registers used. Zero was right
             * only while no floats existed; a variadic callee reads it
             * to find the register save area, so a wrong al is exactly
             * the kind of silent wrongness THE RULE is about. */
            if (i->call_varargs) {
                if (freg)
                    x86_mov_al_imm(text, freg);
                else
                    x86_zero_eax(text);
            }
            if (is_tail) {
                /* The frame is finished with: restore what the prologue
                 * saved, tear it down, and JUMP. The callee returns to
                 * our caller, whose return address `leave` has just left
                 * at the top of the stack.
                 *
                 * The order matters. Arguments are already in their
                 * registers, and those are caller-saved -- restoring
                 * callee-saved ones cannot disturb them. Doing it the
                 * other way round would restore over an argument. */
                restore_callee(text, used_callee, nsave, save_base);
                if (!frameless && !g_pushonly)
                    x86_leave(text);
                int patch = x86_jmp_rel32(text);
                if (cg_call_local(fn->src, i->callee)) {
                    struct callsite cs;
                    cs.patch_off = patch;
                    cs.target = i->callee;
                    PUSH(st->call, st->ncall, st->capcall, cs);
                } else {
                    struct extcall ec = { 0, NULL, 0 };
                    ec.patch_off = patch;
                    ec.callee = i->callee;
                    PUSH(st->ext, st->next, st->capext, ec);
                }
                tail_made = 1;
                break;    /* whatever follows is now unreachable, and
                           * a join label after it is still entered by
                           * the path that did not take this jump */
            }
            if (i->indirect) {
                x86_call_r11(text);
            } else {
                int patch = x86_call_rel32(text);
                if (cg_call_local(fn->src, i->callee)) {
                    struct callsite cs;
                    cs.patch_off = patch;
                    cs.target = i->callee;
                    PUSH(st->call, st->ncall, st->capcall, cs);
                } else {
                    struct extcall ec = { 0, NULL, 0 };
                    ec.patch_off = patch;
                    ec.callee = i->callee;
                    PUSH(st->ext, st->next, st->capext, ec);
                }
            }
            if (i->retsize) {
                /* The value of a struct call is the ADDRESS it landed
                 * at: the scratch we reserved. A MEMORY return already
                 * wrote there; a register return is unpacked into it. An
                 * x87 one pops st0 (the long double, or the real part)
                 * and then st1 (the imaginary part). */
                if (i->ret_x87) {
                    x86_x87_mem(text, 0xDB, 7, REG_RBP,
                                scratch_base + i->scratch);
                    if (i->ret_x87 == 2)
                        x86_x87_mem(text, 0xDB, 7, REG_RBP,
                                    scratch_base + i->scratch + 16);
                } else if (i->retnclass > 0) {
                    x86_lea_reg_slot(text, REG_RCX,
                                     scratch_base + i->scratch);
                    int ir = 0, fr = 0;
                    for (int q = 0; q < i->retnclass; q++) {
                        if (i->retcls[q] == CLASS_SSE)
                            x86_movs_store_base(text, REG_RCX, q * 8,
                                                fr++, 8);
                        else if (i->retcls[q] != CLASS_NONE)
                            x86_store_mem_reg(text, REG_RCX, q * 8,
                                              ir++ == 0 ? REG_RAX
                                                        : REG_RDX, 8);
                    }
                }
                x86_lea_rax_slot(text, scratch_base + i->scratch);
                cg_store(text, sd, i->dst, 8);
                break;
            }
            /* A result nothing reads -- every void call has a temp for
             * one -- stays in rax/xmm0: copying it home was a `mov
             * %rax,%r8` after each of them. Temps only: a local's value
             * can be read through a pointer the counts do not see. The
             * x87 one below must still be popped. */
            if (g_regalloc && usecnt && i->dst >= fn->nvars &&
                i->dst < fn->nvregs && usecnt[i->dst] == 0 &&
                !(i->flt && i->w == 16) && i->w != 16)
                break;
            if (i->flt && i->w == 16)      /* long double comes back in st0 */
                x86_x87_mem(text, 0xDB, 7, REG_RBP, sd[i->dst]);
            else if (i->w == 16) {         /* an __int128 in rax:rdx */
                x86_store_mem_reg(text, REG_RBP, sd[i->dst], REG_RAX, 8);
                x86_store_mem_reg(text, REG_RBP, sd[i->dst] + 8, REG_RDX, 8);
            } else if (i->flt)
                x86_fst(text, sd, i->dst, 0, i->w);   /* xmm0: the ABI's */
            else
                cg_store(text, sd, i->dst, i->w);
            break;
        }
        case IR_ASM: {
            /* Extended asm. Load each input from its stack slot into the
             * fixed register its constraint chose, emit the assembled
             * template, then store each output register through the
             * lvalue address (also in a slot). Clobbers need nothing:
             * every live value is in memory, never a register, across the
             * asm. A scratch that avoids the output register carries the
             * address so the result register survives the store. */
            cg_reset();   /* the template may clobber any register */
            struct ir_asm *ia = i->asm_ir;
            /* A "+" output is read AND written: its register must hold the
             * lvalue's current value when the template starts. Without this
             * `asm("addq $1,%0" : "+r"(x))` computed on whatever the register
             * held — the output's own address, as it happened. Loaded before
             * the inputs, through a scratch no operand uses. */
            {
                int busy[16] = { 0 };
                for (int k = 0; k < ia->nin; k++)
                    if (ia->in[k].reg < 16) busy[ia->in[k].reg] = 1;
                for (int k = 0; k < ia->nout; k++)
                    if (ia->out[k].reg < 16) busy[ia->out[k].reg] = 1;
                static const int pre_pool[] = { REG_RCX, REG_RDX, REG_RSI,
                                                REG_RDI, 8, 9, 10, 11, REG_RAX };
                int pre = -1;
                for (unsigned p = 0; p < sizeof pre_pool / sizeof pre_pool[0]; p++)
                    if (!busy[pre_pool[p]]) { pre = pre_pool[p]; break; }
                for (int k = 0; k < ia->nout; k++) {
                    if (!ia->out[k].inout || ia->out[k].mem)
                        continue;
                    if (pre < 0)
                        diag_fatal(f->file, i->line ? i->line : f->line,
                                   "no scratch register for a \"+\" asm "
                                   "operand in '%s'", f->name);
                    x86_load_reg_mem(text, pre, REG_RBP, sd[ia->out[k].temp], 8);
                    if (ia->out[k].reg >= 16)
                        x86_movs_load_base(text, ia->out[k].reg - 16, pre, 0,
                                           ia->out[k].size);
                    else
                        x86_load_reg_mem(text, ia->out[k].reg, pre, 0,
                                         ia->out[k].size);
                }
            }
            /* Inputs carry their VALUE: a GPR ('r'/fixed) operand loads from its
             * slot into the register; an xmm ('x', reg 16..23) uses movss/movsd
             * into the xmm register instead. */
            for (int k = 0; k < ia->nin; k++)
                if (ia->in[k].reg >= 16)
                    x86_fld(text, sd, ia->in[k].temp, ia->in[k].reg - 16,
                            ia->in[k].size);
                else
                    x86_load_reg_mem(text, ia->in[k].reg, REG_RBP,
                                     sd[ia->in[k].temp], 8);
            /* An "m" output's register holds the ADDRESS the template
             * writes through, and nothing put it there: `stmxcsr %0` in
             * <fenv.h> wrote through whatever the register last held,
             * which was the address only because the code before it
             * happened to compute it there. */
            for (int k = 0; k < ia->nout; k++)
                if (ia->out[k].mem && ia->out[k].reg < 16)
                    x86_load_reg_mem(text, ia->out[k].reg, REG_RBP,
                                     sd[ia->out[k].temp], 8);
            for (int k = 0; k < ia->codelen; k++)
                code_byte(text, ia->code[k]);
            /* The address scratch must not be an OUTPUT register, or loading
             * it would clobber a result before it is stored (e.g. cpuid's
             * four a/b/c/d outputs). Pick one free of every operand. Only GPR
             * operands (reg < 16) can collide with a GPR scratch. */
            int used16[16] = { 0 };
            for (int k = 0; k < ia->nin; k++)
                if (ia->in[k].reg < 16) used16[ia->in[k].reg] = 1;
            for (int k = 0; k < ia->nout; k++)
                if (ia->out[k].reg < 16) used16[ia->out[k].reg] = 1;
            int scr = -1;
            static const int scr_pool[] = { REG_RCX, REG_RDX, REG_RSI,
                                            REG_RDI, 8, 9, 10, 11 };
            for (unsigned p = 0; p < sizeof scr_pool / sizeof scr_pool[0]; p++)
                if (!used16[scr_pool[p]]) { scr = scr_pool[p]; break; }
            /* Outputs store the result register THROUGH the lvalue address
             * (held in the operand's slot). xmm results go out via movss/movsd. */
            for (int k = 0; k < ia->nout; k++) {
                /* An "m" output was written BY the template, through the
                 * address this register holds. Storing the register over
                 * it would destroy exactly what the asm produced. */
                if (ia->out[k].mem)
                    continue;
                x86_load_reg_mem(text, scr, REG_RBP,
                                 sd[ia->out[k].temp], 8);
                if (ia->out[k].reg >= 16)
                    x86_movs_store_base(text, scr, 0, ia->out[k].reg - 16,
                                        ia->out[k].size);
                else
                    x86_store_mem_reg(text, scr, 0, ia->out[k].reg,
                                      ia->out[k].size);
            }
            break;
        }
        case IR_VA_START:
            cg_reset();
            if (target_win64_abi()) {
                /* *ap = the first unnamed slot of the home area */
                x86_mov_rcx_slot(text, sd[i->a]);
                x86_lea_reg_slot(text, REG_RAX, 16 + va_named_int * 8);
                x86_store_mem_reg(text, REG_RCX, 0, REG_RAX, 8);
                break;
            }
            /* Build a __va_list_tag on the frame and point the va_list at
             * it. Layout (SysV): gp_offset u32, fp_offset u32,
             * overflow_arg_area ptr, reg_save_area ptr. */
            x86_mov_eax_imm(text, va_named_int * 8, 4);
            x86_store_mem_reg(text, REG_RBP, va_tag + 0, REG_RAX, 4);
            x86_mov_eax_imm(text, 48 + va_named_sse * 16, 4);
            x86_store_mem_reg(text, REG_RBP, va_tag + 4, REG_RAX, 4);
            x86_lea_reg_slot(text, REG_RAX, va_overflow);
            x86_store_mem_reg(text, REG_RBP, va_tag + 8, REG_RAX, 8);
            x86_lea_reg_slot(text, REG_RAX, va_save);
            x86_store_mem_reg(text, REG_RBP, va_tag + 16, REG_RAX, 8);
            /* *ap = &tag  (i->a holds the address of the va_list) */
            x86_mov_rcx_slot(text, sd[i->a]);
            x86_lea_reg_slot(text, REG_RAX, va_tag);
            x86_store_mem_reg(text, REG_RCX, 0, REG_RAX, 8);
            break;
        case IR_RET:
            cg_reset();
            if (i->a >= 0 && f->ret_ty->kind == TY_STRUCT) {
                enum arg_class rc[2];
                int rn = ty_classify(f->ret_ty, rc);
                int sz = ty_size(f->ret_ty);
                x86_load_slot(text, sd[i->a], 8, 0, 8);
                x86_mov_reg_reg(text, REG_RDX, REG_RAX); /* the value */
                int x87 = ty_x87_ret(f->ret_ty);
                if (x87) {
                    /* X87: st0; COMPLEX_X87: real in st0, imaginary in st1
                     * — so the imaginary part is pushed first */
                    if (x87 == 2)
                        x86_x87_mem(text, 0xDB, 5, REG_RDX, 16);
                    x86_x87_mem(text, 0xDB, 5, REG_RDX, 0);
                } else if (rn == 0) {
                    /* MEMORY: copy into the caller's buffer and hand
                     * the pointer back in rax, as the ABI requires. */
                    x86_load_slot(text, sret_slot, 8, 0, 8);
                    x86_mov_reg_reg(text, REG_RCX, REG_RAX);
                    for (int off = 0; off < sz; ) {
                        int chunk = sz - off;
                        chunk = chunk >= 8 ? 8 : chunk >= 4 ? 4
                              : chunk >= 2 ? 2 : 1;
                        x86_load_reg_mem(text, REG_RAX, REG_RDX, off,
                                         chunk);
                        x86_store_mem_reg(text, REG_RCX, off, REG_RAX,
                                          chunk);
                        off += chunk;
                    }
                    x86_load_slot(text, sret_slot, 8, 0, 8);
                } else {
                    /* Small enough to travel in registers: eightbytes
                     * take the next register of their OWN class, so
                     * INTEGER fills rax then rdx and SSE fills xmm0
                     * then xmm1. The address is held in rcx because rdx
                     * is itself a destination. */
                    x86_mov_reg_reg(text, REG_RCX, REG_RDX);
                    int ir = 0, fr = 0;
                    for (int q = 0; q < rn; q++) {
                        if (rc[q] == CLASS_SSE)
                            x86_movs_load_base(text, fr++, REG_RCX,
                                               q * 8, 8);
                        else if (rc[q] != CLASS_NONE)
                            x86_load_reg_mem(text,
                                             ir++ == 0 ? REG_RAX : REG_RDX,
                                             REG_RCX, q * 8, 8);
                    }
                }
            } else if (i->a >= 0 && f->ret_ty->kind == TY_INT128) {
                x86_load_reg_mem(text, REG_RAX, REG_RBP, sd[i->a], 8);
                x86_load_reg_mem(text, REG_RDX, REG_RBP, sd[i->a] + 8, 8);
            } else if (i->a >= 0 && i->flt && i->w == 16) {
                x86_x87_mem(text, 0xDB, 5, REG_RBP, sd[i->a]);   /* -> st0 */
            } else if (i->a >= 0 && i->flt) {
                /* xmm0 because System V says so, not because it is the
                 * scratch -- it is a pool register now. */
                x86_fld(text, sd, i->a, 0, i->w);
            } else if (i->a >= 0) {
                if (in_reg(i->a))                       /* register-resident value */
                    x86_mov_rr_w(text, REG_RAX, g_loc[i->a], 8);
                else
                    x86_load_slot(text, sd[i->a], 8, 0, 8);
            }
            if (shared_epi) {                           /* jump to the one epilogue */
                int p = x86_jmp_rel32(text);
                if (nepi == capepi) {
                    capepi = capepi ? capepi * 2 : 8;
                    epi_patch = xrealloc(epi_patch, (size_t)capepi * sizeof(int));
                }
                epi_patch[nepi++] = p;
            } else {
                restore_callee(text, used_callee, nsave, save_base);
                x86_epilogue(text, frameless || g_pushonly);
            }
            break;
        case IR_LANDING:
            /* the unwinder left the exception in rax, the selector in rdx */
            cg_reset();
            cg_store(text, sd, i->dst, 8);
            x86_mov_reg_reg(text, REG_RAX, REG_RDX);
            cg_store(text, sd, i->b, 8);
            break;
        case IR_MULH: case IR_MULW:
            /* the 32-bit machines' widening multiply: a 64-bit target
             * multiplies the extended values (target_has_mulh) */
            internal_error("a 32-bit widening multiply reached the x86-64 "
                           "code generator");
            break;
        case IR_OPCOUNT:                 /* not an opcode (ir.h) */
            internal_error("IR_OPCOUNT reached code generation");
        }
        if (i->op == IR_CALL && fn->neh)
            ir_add_csite(fn, ins_start - f->code_off, text->len - f->code_off,
                         i->eh_region - 1);
        if (g_regalloc && (tail_made || i->op == IR_RET ||
                           i->op == IR_JMP || i->op == IR_UD2))
            dead = 1;
        tail_made = 0;
    }

    /* Every function ends with an epilogue, whether or not its last
     * statement was a return. A void function may legally fall off the
     * end (sema only demands a return from value-returning ones), and
     * without this it fell straight into the NEXT function's code —
     * silently, since nothing crashes until a stray ret runs.
     *
     * But when the last instruction is an unconditional terminator (an
     * explicit return, a tail jump, or a trap) the fall-through is
     * unreachable, and at -O2 this dead tail also re-emits nsave callee
     * restores. Drop it there. -O0/-O1 keep the (2-byte) dead `leave; ret`
     * so their output — the self-host fixed point — stays byte-identical. */
    int last_terminates = fn->nins > 0 &&
        (fn->ins[fn->nins - 1].op == IR_RET ||
         fn->ins[fn->nins - 1].op == IR_JMP ||
         fn->ins[fn->nins - 1].op == IR_UD2);
    /* Emit the trailing epilogue when the returns share it (it is their jump
     * target) OR when control can fall off the end (a void function). */
    int epi_off = text->len;
    if (shared_epi || !(g_regalloc && last_terminates)) {
        restore_callee(text, used_callee, nsave, save_base);
        x86_epilogue(text, frameless || g_pushonly);
    }
    x86_no_rbp = 0;
    g_pushonly = g_pad = 0;
    for (int e = 0; e < nepi; e++) {                    /* patch shared-return jumps */
        int from = epi_patch[e] + 4;
        code_patch32(text, epi_patch[e],
                     (unsigned long)(unsigned int)(epi_off - from));
    }
    free(epi_patch);

    for (int r = 0; r < fn->neh; r++)      /* where each landing pad is */
        fn->eh[r].lp_off = label_off[fn->eh[r].lp_label] - f->code_off;
    /* the absolute jump-table entries: this function + the label's offset */
    for (int k = 0; k < njtabs; k++) {
        int target = label_off[jtabs[k].label];
        if (target < 0)
            internal_error("label %d in '%s' was never placed",
                           jtabs[k].label, f->name);
        struct fsite js;
        js.patch_off = jtabs[k].off;
        js.target = f;
        js.kind = RK_ABS64;
        js.addend = target - f->code_off;
        PUSH(st->f, st->nf, st->capf, js);
    }
    free(jtabs);
    for (int n = 0; n < nbrs; n++) {
        int target = label_off[brs[n].label];
        if (target < 0) {
            internal_error("label %d in '%s' was never placed",
                           brs[n].label, f->name);
        }
        int from = brs[n].patch_off + brs[n].size;
        long rel = brs[n].tab ? (long)target - (brs[n].tab - 1)
                              : (long)target - from;
        if (brs[n].size == 1) {
            if (rel < -128 || rel > 127) {
                /* It does not reach. This whole layout is about to be
                 * thrown away and the function emitted again with this
                 * branch long -- so do not patch, and say so. */
                g_grew = 1;
                if (brs[n].ord >= 0 && brs[n].ord < g_nshort)
                    g_short[brs[n].ord] = 0;
                continue;
            }
            text->p[brs[n].patch_off] = (unsigned char)(rel & 0xff);
            continue;
        }
        code_patch32(text, brs[n].patch_off,
                     (unsigned long)(unsigned int)(int)rel);
    }
    free(brs);
    cg_note_labels(fn, label_off);
    free(label_off);
    afold_free(&g_afold);
    g_afold.ok = NULL; g_afold.disp = NULL;
    free(sd);
    free(loc);
    free(usecnt);
    free(g_zx32);
    g_zx32 = NULL;
    free(vacc); g_vacc = NULL;
    g_loc = NULL;
    free(g_wide);
    g_wide = NULL;
    g_regalloc = saved_regalloc;      /* restore (a cgoto function forced them off) */
    g_regcache = saved_regcache;

    f->code_len = text->len - f->code_off;
}

void codegen_unit(struct ir_unit *iu, struct code *text,
                  struct extcall **ext, int *next,
                  struct strsite **strs, int *nstrs,
                  struct gsite **gs, int *ngs,
                  struct fsite **fs, int *nfs, int keep_vars, int optimize,
                  int no_sse, int regalloc)
{
    struct sites st = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    g_keep_vars = keep_vars;
    /* -O2 turns on register allocation; the RAX residency cache runs alongside
     * it (keyed on vreg, so it also elides reloads of register-resident values —
     * the store-then-reload round-trips). -O0/-O1 are unchanged (regalloc off),
     * so their output stays byte-identical. */
    g_regalloc = regalloc;
    g_tailcalls = regalloc;   /* same switch: both are -O2 */
    g_regcache = optimize;
    g_opt_frames = optimize;
    g_no_sse = no_sse;

    for (int n = 0; n < iu->nfuncs; n++) {
        struct ir_func *fn = &iu->funcs[n];
        /* Emit the function, shortening every branch that reaches (see
         * `struct brsite`). Everything gen_func appends to outside
         * `text` is rewound with it, so each attempt starts exactly
         * where the last one did. */
        int text0 = text->len;
        int ncall0 = st.ncall, next0 = st.next, nstr0 = st.nstr;
        int ng0 = st.ng, nf0 = st.nf;
        g_nshort = fn->nins + 1;
        g_short = xmalloc((size_t)g_nshort);
        memset(g_short, 1, (size_t)g_nshort);
        for (int round = 0; ; round++) {
            text->len = text0;
            st.ncall = ncall0; st.next = next0; st.nstr = nstr0;
            st.ng = ng0; st.nf = nf0;
            fn->nlines = 0;
            fn->ncsites = 0;
            free(fn->var_off); fn->var_off = NULL;
            g_brord = 0; g_grew = 0;
            /* A round per branch is the bound; past that something is
             * wrong, and all-long is the answer that always works. */
            if (round >= 12)
                memset(g_short, 0, (size_t)g_nshort);
            gen_func(fn, text, &st);
            if (!g_grew)
                break;
        }
        free(g_short); g_short = NULL; g_nshort = 0;
    }

    /* All targets are placed now; resolve the intra-unit calls.
     * rel32 is relative to the end of the call instruction. */
    for (int n = 0; n < st.ncall; n++) {
        int from = st.call[n].patch_off + 4;
        long rel = (long)st.call[n].target->code_off - from;
        code_patch32(text, st.call[n].patch_off,
                     (unsigned long)(unsigned int)rel);
    }
    free(st.call);

    cg_resolve_strsites(iu, st.str, st.nstr);

    *ext = st.ext;
    *next = st.next;
    *strs = st.str;
    *nstrs = st.nstr;
    *gs = st.g;
    *ngs = st.ng;
    *fs = st.f;
    *nfs = st.nf;
}
