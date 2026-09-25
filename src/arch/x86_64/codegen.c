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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../driver/remark.h"
#include "../../driver/util.h"

/* -g: when set, DWARF wants each source variable at a distinct stack location,
 * so local-slot coalescing is disabled. Defined here (used by coalesce_locals);
 * codegen_unit sets it from want_debug. Also read by the line-table pass. */
static int g_want_debug;

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
static const int LEAF_POOL[NLEAF] = { 8, 9, 10, 11, 6 /*rsi*/,
                                      3 /*rbx*/, 12, 13, 14, 15 };
/* rdi is NOT here, and it was tried. It is argument register zero AND
 * the hidden pointer a struct return travels through, so it is written
 * at more call sites than any other -- and <format>, whose every result
 * is a std::string, failed every floating-point case with it in.
 * Emitting the sret `lea` after the parallel move (below) fixes one of
 * those writes and is kept for when the rest are found; it is not
 * enough on its own. */
/* ...without rsi, for a function that has an atomic in it: IR_CMPXCHG
 * parks `&expected` there across the compare-exchange. rdi stays,
 * because nothing outside the __int128 lowering uses it -- and a
 * function containing an __int128 is kept out of the allocator
 * entirely. */
/* ...and with rdx, for a function that neither divides nor has an
 * atomic in it. */
#define NLEAF_RDX 11
static const int LEAF_POOL_RDX[NLEAF_RDX] = { 8, 9, 10, 11, 6 /*rsi*/,
                                              2 /*rdx*/, 3 /*rbx*/,
                                              12, 13, 14, 15 };

#define NLEAF_AT 9
static const int LEAF_POOL_AT[NLEAF_AT] = { 8, 9, 10, 11,
                                            3 /*rbx*/, 12, 13, 14, 15 };

/* The FLOATING-POINT pool: xmm8-15.
 *
 * SysV passes floating-point arguments and returns in xmm0-7 and
 * preserves none of the sixteen, so the eight above the argument file
 * are the ones that are nobody else's: no prologue save, no CFI rule,
 * and no collision with a call's arguments being set up. xmm0 stays the
 * scratch every float site already uses.
 *
 * Win64 is the reason this is not xmm2-15: there xmm6-15 are
 * callee-saved and would each need a sixteen-byte save and an unwind
 * rule, which is work this does not do yet -- so that ABI gets no FP
 * pool at all rather than a wrong one. */
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
 *   an atomic  -- IR_CMPXCHG parks `&expected` in rsi across the
 *                 compare-exchange, and rdx holds `desired`;
 *   a divide   -- idiv writes the rdx:rax pair, whatever the operands;
 *   variadic   -- the prologue spills the six integer argument
 *                 registers to the save area va_arg reads.
 *
 * rdx is in the pool for everything else, which is most functions: it
 * appears in 1.5% of the instructions this backend emits. */
static int x86_reserves(const struct ir_func *fn, int *div)
{
    int at = 0;
    *div = 0;
    for (int n = 0; n < fn->nins; n++)
        switch (fn->ins[n].op) {
        case IR_XCHG: case IR_XADD: case IR_ARMW:
        case IR_CAS: case IR_CAS16: case IR_CMPXCHG:
            at = 1; break;
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
    else if (at)        { nb = NLEAF_AT;  base = LEAF_POOL_AT; }
    else if (div)       { nb = NLEAF;     base = LEAF_POOL; }
    else                { nb = NLEAF_RDX; base = LEAF_POOL_RDX; }
    if (!x86_wants_rdi(fn)) { *n = nb; return base; }
    /* After the caller-saved ones already there (it is caller-saved too,
     * so it needs no prologue save) and before the callee-saved five. */
    int k = 0, o = 0;
    while (k < nb && (base[k] == 8 || base[k] == 9 || base[k] == 10 ||
                      base[k] == 11 || base[k] == 6 || base[k] == 2))
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
                    if (a->cls[q] == CLASS_SSE) freg2++; else ireg2++;
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
    x86_fp_pool_for, x86_fp_callee_saved
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
char *cg_float_vregs(struct ir_func *fn)
{
    int nv = fn->nvregs ? fn->nvregs : 1;
    char *w = xcalloc((size_t)nv, 1);
    char *wide = cg_wide_vregs(fn);
    int any = 0;
#define MARK(v) do { int _v=(v); if (_v>=0 && _v<nv && !w[_v]) { w[_v]=1; any=1; } } while (0)
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
#define BAD(v) do { int _v=(v); if (_v>=0 && _v<nv) bad[_v]=1; } while (0)
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
        case IR_I2F:  BAD(i->a);   break;         /* integer in */
        case IR_F2I:  BAD(i->dst); break;         /* integer out */
        case IR_F2F:
            /* A conversion with a LONG DOUBLE on either side goes
             * through the x87 unit, which loads and stores memory and
             * nothing else -- so the float/double side of it has to be
             * in a slot for x87 to reach. tests/exec/long-double.c:
             * `(double)(one + tiny)`. */
            if (i->w == 16 || i->size == 16) { BAD(i->dst); BAD(i->a); }
            break;
        case IR_SQRT: break;
        case IR_MOV: case IR_LDVAR: case IR_STVAR: break;   /* copies */
        case IR_LOAD:  BAD(i->a);   break;        /* the address */
        case IR_STORE: BAD(i->a);   break;        /* the address */
        case IR_RET:   break;
        case IR_CALL:
            if (i->indirect) BAD(i->a);
            for (int k = 0; k < i->nargs; k++)
                if (i->argv[k].cls[0] != CLASS_SSE) BAD(i->argv[k].vreg);
            if (!i->flt) BAD(i->dst);
            break;
        default:
            /* every other op is integer in and integer out */
            BAD(i->dst); BAD(i->a); BAD(i->b); BAD(i->c);
            break;
        }
    }
#undef BAD
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
    if (n == 0 || g_want_debug || g_has_cgoto || !fn->var_scope_lo) {
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
    unsigned long *lin = NULL, *lout = NULL; int *dv = NULL, lw = 0;
    lout = ra_live_intervals(fn, lf, ll, &lin, &dv, &lw);

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
    }
    free(at); free(lf); free(ll); free(lout); free(lin); free(dv);

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

static int *layout_frame(struct ir_func *fn, int *frame_out,
                         int *scratch_base_out, int *sret_slot_out,
                         int *va_save_out, int *va_tag_out,
                         int nsave, int *save_base_out, const int *loc)
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
    char *lref = ra_locals_referenced(fn, g_want_debug);
    int *ssize = xcalloc((size_t)(nls ? nls : 1), sizeof *ssize);
    int *salign = xcalloc((size_t)(nls ? nls : 1), sizeof *salign);
    for (int i = 0; i < fn->nvars; i++) {
        int s = lslot[i];
        if (!lref[i] || ra_slot_dead(fn, loc, g_floc, i, g_want_debug))
            continue;               /* in a register, or named nowhere at all */
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
        disp[i] = !lref[i] || ra_slot_dead(fn, loc, g_floc, i, g_want_debug) ? DEAD_SLOT_OFF
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
            if (g_want_debug && d < fn->nvars) continue;  /* its DWARF home */
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

/* g_want_debug (the -g flag) is declared near the top of the file — it is read
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
 * (e.g. a 4-byte store then an 8-byte reload — the IR_MOV round-trip). */
static int rc_vw, rc_zx;

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
        if (g_regalloc && rc_zx && sign == 0 && size >= rc_vw)
            return;
    }
    if (in_reg(vreg)) {
        int R = g_loc[vreg];
        if (size == 1 || size == 2)
            x86_movx_rr(text, REG_RAX, R, size, sign, w); /* char/short widen */
        else if (size == 4 && sign && w == 8)
            x86_movsxd_rr(text, REG_RAX, R);      /* signed int -> 64 */
        else
            x86_mov_rr_w(text, REG_RAX, R, size == 8 ? 8 : w);
    } else {
        x86_load_slot(text, sd[vreg], size, sign, w);
    }
    if (g_regcache && cacheable(vreg)) {
        rc_vreg = vreg; rc_size = size; rc_sign = sign; rc_w = w;
        rc_zx = (sign == 0); rc_vw = size;        /* zero-ext read: low `size` valid */
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
static void cg_icmp_flags(struct code *text, const int *sd, struct ir_ins *i)
{
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

/* The floating-point pair of cg_load/cg_store. An xmm home and a stack
 * slot are the same value and only one of them is current, so every site
 * that touches a float vreg's slot goes through these. */
static void x86_fld(struct code *text, const int *sd, int v, int xmm, int w)
{
    if (in_freg(v)) {
        if (g_floc[v] != xmm) x86_movs_reg(text, xmm, g_floc[v]);
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
    x86_movs_store(text, xmm, sd[v], w);
}

/* ...and "operate where the value already is": which xmm a value can be
 * READ from, which one a result may be COMPUTED in, and the store back
 * when that was the scratch. */
static int x86_frd(struct code *text, const int *sd, int v, int scratch, int w)
{
    if (in_freg(v)) return g_floc[v];
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
    struct extcall ec;
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
             * zero) in the high eightbyte */
            if (g_wide && g_wide[i->a])
                ld8(text, REG_RAX, sd[i->a]);
            else
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
    int any = 0;
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
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
        UNFOLD(i->a); UNFOLD(i->b); UNFOLD(i->c);
        if (i->op == IR_CALL)
            for (int k = 0; k < i->nargs; k++) UNFOLD(i->argv[k].vreg);
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
static void restore_callee(struct code *text, const int *used_callee,
                           int nsave, int save_base)
{
    if (nsave >= 2) {
        x86_lea_reg_slot(text, REG_RSP, save_base);
        for (int k = 0; k < nsave; k++)
            x86_pop_reg(text, used_callee[k]);
        return;
    }
    for (int k = 0; k < nsave; k++)
        x86_load_reg_mem(text, used_callee[k], REG_RBP, save_base + k * 8, 8);
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
    int *sd = layout_frame(fn, &frame, &scratch_base, &sret_slot,
                           &va_save, &va_tag, nsave, &save_base, loc);
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

    /* -g: expose each source variable's frame slot (rbp-relative) so the
     * DWARF emitter can write DW_OP_fbreg. sd is indexed by vreg; params and
     * locals are vregs [0, nvars), which is what dbgvars reference. */
    if (g_want_debug) {
        int nv = fn->nvars ? fn->nvars : 1;
        fn->var_off = xmalloc((size_t)nv * sizeof *fn->var_off);
        for (int v = 0; v < fn->nvars; v++)
            fn->var_off[v] = sd[v];
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
                    !fn->has_alloca && !f->is_varargs && !g_want_debug &&
                    sret_slot == 0 && f->nparams <= 4;
    for (int n = 0; n < fn->nins && frameless; n++)
        switch (fn->ins[n].op) {
        case IR_CALL: case IR_ASM: case IR_ALLOCA: case IR_VA_START:
        case IR_FRAMEADDR: case IR_SPSAVE: case IR_SPRESTORE:
            frameless = 0;
            break;
        default:
            break;
        }
    for (int p = 0; p < f->nparams && frameless; p++) {
        struct type *pt = f->param_tys[p];
        if (!pt || pt->kind == TY_STRUCT || pt->kind == TY_ARRAY ||
            pt->kind == TY_INT128 || ty_is_float(pt) || ty_size(pt) > 8)
            frameless = 0;
    }

    code_align(text, 16, 0x90);
    f->code_off = text->len;

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
    x86_prologue(text, frameless ? frame : 0, frameless);
    /* for the unwind tables: push rbp ends at +1, mov rbp,rsp at +4 */
    f->cfi_frameless = frameless;
    f->cfi_push = 1;
    f->cfi_frame = 4;
    if (!frameless) {
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
        f->cfi_off[k] = save_base + k * 8 - 16;     /* the CFA is rbp+16 */
    }
    f->cfi_saved_at = text->len - f->code_off;
    /* Variadic: spill the whole argument register file into the save area
     * FIRST, before the parameter pass below uses rcx/rax as scratch and
     * so clobbers the vararg registers. Storing a register does not alter
     * it, so the named-parameter loads that follow still see rdi..r9 and
     * xmm0..7 intact. The SSE slots are 16 apart (SysV) but only their low
     * 8 bytes — a double — are stored, which is all vfprintf reads. */
    if (f->is_varargs) {
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
    int pmove = g_regalloc && g_loc && !g_want_debug && !f->is_varargs;
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
                if (fn->param_abi && fn->param_abi[i].byref) {
                    int sz = ty_size(pt);
                    if (slot >= 4) {
                        x86_load_reg_mem(text, REG_RCX, REG_RBP, incoming, 8);
                        incoming += 8;
                    } else if (x86_argreg(slot) != REG_RCX) {
                        x86_mov_reg_reg(text, REG_RCX, x86_argreg(slot));
                    }
                    x86_lea_reg_slot(text, 11 /*r11*/, sd[i]);
                    for (int off = 0; off < sz; off += 8) {
                        int chunk = sz - off >= 8 ? 8 : sz - off;
                        x86_load_reg_mem(text, REG_RAX, REG_RCX, off, chunk);
                        x86_store_mem_reg(text, 11 /*r11*/, off, REG_RAX,
                                          chunk);
                    }
                    continue;
                }
                if (slot >= 4) {
                    if (pmove && g_loc[i] >= 0) {       /* as SysV, above */
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
                    if (pmove && g_loc[i] >= 0) {
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
                 * address is a normal local. RAX carries each eightbyte, so the
                 * destination pointer needs a DIFFERENT scratch — and not an
                 * integer arg register (RCX would drop a later scalar param that
                 * arrives in it). r11 is caller-saved, never an arg register, and
                 * free at prologue time (params reach their allocated registers
                 * only in the parallel move that runs after this loop). */
                int sz = ty_size(pt);
                if (ty_align(pt) > 8)       /* a 16-aligned stack slot */
                    incoming = (incoming + 15) & ~15;
                x86_lea_reg_slot(text, 11 /*r11*/, sd[i]);
                for (int off = 0; off < sz; off += 8) {
                    int chunk = sz - off >= 8 ? 8 : sz - off;
                    x86_load_reg_mem(text, REG_RAX, REG_RBP,
                                     incoming + off, chunk >= 8 ? 8 : chunk);
                    x86_store_mem_reg(text, 11 /*r11*/, off, REG_RAX,
                                      chunk >= 8 ? 8 : chunk);
                }
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
                else
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
    /* -O2: with two or more returns each inlining the full callee-restore
     * sequence, route them through ONE shared epilogue instead — each return
     * loads its value then `jmp`s to it. Worth the jmp only when there is more
     * than one return and something to restore; a single return stays inline. */
    int nret = 0;
    for (int t = 0; t < fn->nins; t++) if (fn->ins[t].op == IR_RET) nret++;
    int shared_epi = g_regalloc && nsave >= 1 && nret >= 2;
    int *epi_patch = NULL, nepi = 0, capepi = 0;
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        /* What xmm0 held coming in. Cleared by default, so any op that
         * is not one of the vector cases below invalidates it simply by
         * not setting it again. */
        int vrc_in = vrc_vreg;
        int vw_in = vw_src;
        int fold_in = fold_idx;
        vrc_vreg = -1;
        vw_src = -1;
        fold_idx = -1;      /* only the instruction right after may use it */
        int ins_start = text->len;
        x86_lowering_op = ir_opname(i->op);   /* for the dead-slot guard */
        /* -g: a row where the source line changes. text->len is the .text
         * offset this instruction's code begins at (the switch below emits
         * it). Multiple IR ops from one statement share a line and collapse
         * to a single row; ops with no line (0) inherit the last row. */
        if (g_want_debug && i->line) {
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
            cg_load(text, sd, i->c, w, i->sign, w);
            int cr = in_reg(i->a) ? g_loc[i->a] : REG_RCX;
            if (!in_reg(i->a))
                cg_load_rcx(text, sd, i->a, 4);
            x86_test_rr(text, cr, cr, 4);
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
                if (x87_ins(nx) ||
                    (nx->op == IR_LOAD && is_flt(nx->dst)) ||
                    (nx->op == IR_STORE && is_flt(nx->b))) {
                    /* fall through to materialise the address */
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
                if (ad->op == IR_ADD && !ad->flt && !ad->imm_b &&
                    ad->b == i->dst && in_reg(ad->a) && ad->dst >= 0 &&
                    usecnt[ad->dst] == 1 && !x87_ins(mem) &&
                    ((mem->op == IR_LOAD && mem->a == ad->dst) ||
                     (mem->op == IR_STORE && mem->a == ad->dst))) {
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
        case IR_XCHG:
            cg_reset();
            x86_mov_rcx_slot(text, sd[i->a]);            /* address -> rcx */
            x86_load_slot(text, sd[i->b], i->size, 0,
                          i->size == 8 ? 8 : 4);         /* new value -> rax */
            x86_xchg_rax_mem_rcx(text, i->size);         /* atomic; rax = old */
            cg_store(text, sd, i->dst, i->w);
            break;
        case IR_XADD:
            cg_reset();
            x86_mov_rcx_slot(text, sd[i->a]);            /* address -> rcx */
            x86_load_slot(text, sd[i->b], i->size, 0,
                          i->size == 8 ? 8 : 4);         /* addend -> rax */
            x86_lock_xadd_rcx(text, i->size);            /* atomic; rax = old */
            cg_store(text, sd, i->dst, i->w);
            break;
        case IR_CMPXCHG:
            cg_reset();
            x86_load_slot(text, sd[i->c], i->size, 0,
                          i->size == 8 ? 8 : 4);         /* desired -> rax.. */
            x86_mov_reg_reg(text, REG_RDX, REG_RAX);     /* ..-> rdx */
            x86_mov_rcx_slot(text, sd[i->a]);            /* object ptr -> rcx */
            x86_load_slot(text, sd[i->b], 8, 0, 8);      /* &expected -> rax */
            x86_mov_reg_reg(text, REG_RSI, REG_RAX);     /* save in rsi */
            x86_load_reg_mem(text, REG_RAX, REG_RSI, 0, i->size); /* rax=*exp */
            x86_lock_cmpxchg_rcx(text, i->size);         /* CAS; ZF=matched */
            x86_store_mem_reg(text, REG_RSI, 0, REG_RAX, i->size);/* *exp=seen */
            x86_setcc_eax(text, 0x94);                   /* setz: dst = matched */
            cg_store(text, sd, i->dst, 4);
            break;
        case IR_ARMW: {
            /* and / or / xor / nand: x86 has no locked fetch-and-OP that
             * returns the old value, so this is the compare-and-swap loop gcc
             * emits. rax is the value last seen, rdx the value to install;
             * lock cmpxchg installs rdx only if memory still holds rax, and
             * otherwise reloads rax with what it does hold, so the loop
             * recomputes from the fresh value. */
            cg_reset();
            int w = i->size == 8 ? 8 : 4;
            int op = (int)i->imm;
            x86_mov_rcx_slot(text, sd[i->a]);             /* address -> rcx */
            x86_load_slot(text, sd[i->b], i->size, 0, w); /* operand -> rax.. */
            x86_mov_reg_reg(text, REG_RSI, REG_RAX);      /* ..-> rsi */
            x86_load_reg_mem(text, REG_RAX, REG_RCX, 0, i->size); /* current */
            int loop = text->len;
            x86_mov_reg_reg(text, REG_RDX, REG_RAX);
            x86_alu_rr(text, op == 'n' ? '&' : op, REG_RDX, REG_RSI, w);
            if (op == 'n')
                x86_not_reg(text, REG_RDX, w);
            x86_lock_cmpxchg_rcx(text, i->size);          /* ZF: installed */
            int back = x86_jnz_rel32(text);
            code_patch32(text, back, (unsigned long)(long)(loop - (back + 4)));
            cg_store(text, sd, i->dst, i->w);             /* rax = the old value */
            break;
        }
        case IR_CAS:
            /* By value: rax = expected, rdx = desired. After lock cmpxchg rax
             * holds the value that was in memory whether or not the swap
             * happened (on success it already was that value). */
            cg_reset();
            x86_load_slot(text, sd[i->c], i->size, 0,
                          i->size == 8 ? 8 : 4);          /* desired -> rax.. */
            x86_mov_reg_reg(text, REG_RDX, REG_RAX);      /* ..-> rdx */
            x86_mov_rcx_slot(text, sd[i->a]);             /* address -> rcx */
            x86_load_slot(text, sd[i->b], i->size, 0,
                          i->size == 8 ? 8 : 4);          /* expected -> rax */
            x86_lock_cmpxchg_rcx(text, i->size);
            cg_store(text, sd, i->dst, i->w);
            break;
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
            int dbase, sbase;
            if (in_reg(i->a)) dbase = g_loc[i->a];
            else { x86_load_slot(text, sd[i->a], 8, 0, 8);
                   x86_mov_reg_reg(text, REG_RCX, REG_RAX); dbase = REG_RCX; }
            if (in_reg(i->b)) sbase = g_loc[i->b];
            else { x86_load_slot(text, sd[i->b], 8, 0, 8);
                   x86_mov_reg_reg(text, REG_RDX, REG_RAX); sbase = REG_RDX; }
            int off = 0;
            while (off < i->size) {
                int chunk = i->size - off;
                chunk = chunk >= 8 ? 8 : chunk >= 4 ? 4 : chunk >= 2 ? 2 : 1;
                x86_load_reg_mem(text, REG_RAX, sbase, off, chunk);
                x86_store_mem_reg(text, dbase, off, REG_RAX, chunk);
                off += chunk;
            }
            break;
        }
        case IR_MEMZERO: {
            cg_reset();
            int dbase;
            if (in_reg(i->a)) dbase = g_loc[i->a];
            else { x86_load_slot(text, sd[i->a], 8, 0, 8);
                   x86_mov_reg_reg(text, REG_RCX, REG_RAX); dbase = REG_RCX; }
            x86_mov_eax_imm(text, 0, 8);
            int off = 0;
            while (off < i->size) {
                int chunk = i->size - off;
                chunk = chunk >= 8 ? 8 : chunk >= 4 ? 4
                      : chunk >= 2 ? 2 : 1;
                x86_store_mem_reg(text, dbase, off, REG_RAX, chunk);
                off += chunk;
            }
            break;
        }
        case IR_LABEL:
            cg_reset();      /* a merge point: RAX is unknown here */
            label_off[i->label] = text->len;
            break;
        case IR_JMP:
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
            nbrs++;
            break;
        }
        case IR_LABELADDR: {
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
                     * register-resident -> store the register straight out). */
                    if (in_reg(a->vreg)) {
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
                for (int off = 0; off < sz; ) {
                    int chunk = sz - off;
                    chunk = chunk >= 8 ? 8 : chunk >= 4 ? 4
                          : chunk >= 2 ? 2 : 1;
                    /* The source address is re-read for each chunk
                     * rather than parked in a second register, because
                     * there is no second register to park it in: rax is
                     * the only GPR no pool contains, and rcx is the one
                     * this backend's shifts, atomics and struct copies
                     * all reach for. Three instructions an eightbyte
                     * instead of two, on an argument class the corpus
                     * uses eleven times -- and never the wrong
                     * register. */
                    x86_load_slot(text, sd[a->vreg], 8, 0, 8);
                    x86_load_reg_mem(text, REG_RAX, REG_RAX, off, chunk);
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
                            if (a->cls[q] != CLASS_SSE) pireg++;
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
                        if (i->call_varargs)
                            x86_load_arg(text, ireg, sd[a->vreg]);
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
                        else
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
                x86_leave(text);
                int patch = x86_jmp_rel32(text);
                if (i->callee->has_defn) {
                    struct callsite cs;
                    cs.patch_off = patch;
                    cs.target = i->callee;
                    PUSH(st->call, st->ncall, st->capcall, cs);
                } else {
                    struct extcall ec;
                    ec.patch_off = patch;
                    ec.callee = i->callee;
                    PUSH(st->ext, st->next, st->capext, ec);
                }
                break;    /* whatever follows is now unreachable, and
                           * a join label after it is still entered by
                           * the path that did not take this jump */
            }
            if (i->indirect) {
                x86_call_r11(text);
            } else {
                int patch = x86_call_rel32(text);
                if (i->callee->has_defn) {
                    struct callsite cs;
                    cs.patch_off = patch;
                    cs.target = i->callee;
                    PUSH(st->call, st->ncall, st->capcall, cs);
                } else {
                    struct extcall ec;
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
                        else
                            x86_store_mem_reg(text, REG_RCX, q * 8,
                                              ir++ == 0 ? REG_RAX
                                                        : REG_RDX, 8);
                    }
                }
                x86_lea_rax_slot(text, scratch_base + i->scratch);
                cg_store(text, sd, i->dst, 8);
                break;
            }
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
            /* Build a __va_list_tag on the frame and point the va_list at
             * it. Layout (SysV): gp_offset u32, fp_offset u32,
             * overflow_arg_area ptr, reg_save_area ptr. */
            cg_reset();
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
                        else
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
                x86_epilogue(text, frameless);
            }
            break;
        case IR_LANDING:
            /* the unwinder left the exception in rax, the selector in rdx */
            cg_reset();
            cg_store(text, sd, i->dst, 8);
            x86_mov_reg_reg(text, REG_RAX, REG_RDX);
            cg_store(text, sd, i->b, 8);
            break;
        case IR_OPCOUNT:                 /* not an opcode (ir.h) */
            internal_error("IR_OPCOUNT reached code generation");
        }
        if (i->op == IR_CALL && fn->neh)
            ir_add_csite(fn, ins_start - f->code_off, text->len - f->code_off,
                         i->eh_region - 1);
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
        x86_epilogue(text, frameless);
    }
    for (int e = 0; e < nepi; e++) {                    /* patch shared-return jumps */
        int from = epi_patch[e] + 4;
        code_patch32(text, epi_patch[e],
                     (unsigned long)(unsigned int)(epi_off - from));
    }
    free(epi_patch);

    for (int r = 0; r < fn->neh; r++)      /* where each landing pad is */
        fn->eh[r].lp_off = label_off[fn->eh[r].lp_label] - f->code_off;
    for (int n = 0; n < nbrs; n++) {
        int target = label_off[brs[n].label];
        if (target < 0) {
            internal_error("label %d in '%s' was never placed",
                           brs[n].label, f->name);
        }
        int from = brs[n].patch_off + brs[n].size;
        long rel = (long)target - from;
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
    free(label_off);
    afold_free(&g_afold);
    g_afold.ok = NULL; g_afold.disp = NULL;
    free(sd);
    free(loc);
    free(usecnt);
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
                  struct fsite **fs, int *nfs, int want_debug, int optimize,
                  int no_sse, int regalloc)
{
    struct sites st = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    g_want_debug = want_debug;
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

    /* String sites still carry the string INDEX; turn it into the
     * .rodata offset the driver's relocations speak. */
    for (int n = 0; n < st.nstr; n++)
        st.str[n].str_off = iu->strs[st.str[n].str_off].off;

    *ext = st.ext;
    *next = st.next;
    *strs = st.str;
    *nstrs = st.nstr;
    *gs = st.g;
    *ngs = st.ng;
    *fs = st.f;
    *nfs = st.nf;
}
