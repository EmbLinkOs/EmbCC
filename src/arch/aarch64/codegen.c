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
#include "../regalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emit.h"
#include "../../driver/util.h"

/* -mgeneral-regs-only / -mno-sse: the FP registers are off limits. */
static int g_no_fp;
static int g_a64_regalloc;      /* -O2 register allocation is on */
static int g_a64_opt_frames;    /* -O1+: a never-referenced temp takes no slot */

/* The register frame slots are addressed from: sp, except in a function
 * whose sp moves at run time (a VLA's IR_ALLOCA), where the prologue pins
 * the post-prologue sp in callee-saved x19 and every slot goes through it.
 * Only outgoing stack arguments are always at the live sp. */
#define A64_FBREG 19
static int g_fb = A64_SP;
#define FB g_fb

/* ---- register allocation (src/arch/regalloc.c) -------------------------
 *
 * This backend began as the naive one: every vreg in a stack slot, every
 * operation through the accumulator. That is what `(void)regalloc;` said
 * for as long as there was no allocator to hand it to -- D-011 chose to
 * rebuild rather than share, and "prove it first" applied to a second
 * backend as much as it did to the first.
 *
 * The allocator is shared now, lifted out of the x86 backend unchanged,
 * so what remains here is the machine's own description and the two
 * places a value is read and written.
 *
 * The pool leaves out more than AAPCS64 does, and each omission is a
 * register this backend already spends: x9/x10/x11 are the accumulator,
 * the second operand and the address scratch; x19 pins the frame base in
 * a function with a VLA; x16/x17 are the linker's veneer scratch and x18
 * is the platform register, neither of which a compiler may assume is
 * still there across a call. x0-x8 are arguments and the indirect
 * result. What is left is x12-x15, caller-saved and free, and x20-x28,
 * callee-saved and therefore saved in the prologue when used.
 *
 * Caller-saved first, so a short-lived value takes one and costs no
 * prologue save at all. */
#define A64_NPOOL 18
static const int A64_POOL[A64_NPOOL] = { 13, 14, 15,           /* free */
                                         0, 1, 2, 3, 4, 5, 6, 7, /* args */
                                         20, 21, 22, 23, 24, 25, 26 };
/* The same list without x13 and x14, for a function that has an atomic
 * op in it -- see the note below. */
#define A64_NPOOL_AT 16
static const int A64_POOL_AT[A64_NPOOL_AT] = { 15,
                                               0, 1, 2, 3, 4, 5, 6, 7,
                                               20, 21, 22, 23, 24, 25, 26 };
/* A VARIADIC function's pools: the argument registers are dropped,
 * because its prologue spills x0-x7 to the register-save area that
 * va_arg reads. Everything else is unchanged. */
#define A64_NPOOL_VA 10
static const int A64_POOL_VA[A64_NPOOL_VA] = { 13, 14, 15, 20, 21, 22,
                                               23, 24, 25, 26 };
#define A64_NPOOL_VA_AT 8
static const int A64_POOL_VA_AT[A64_NPOOL_VA_AT] = { 15, 20, 21, 22, 23,
                                                     24, 25, 26 };

/* CALLEE-SAVED ONLY, x20-x26, saved in the prologue and described in
 * the unwind tables. Both halves of that are required and the second is
 * the one that is easy to forget: an exception unwinding through this
 * function has to restore the caller's copy, and it can only do that
 * from a CFI rule. `struct func` holds eight such rules at one program
 * point, and x19's frame-base save shares them, so seven is the bound.
 *
 * Nothing caller-saved is in the pool, and that is not a preference --
 * there is almost nothing left. AAPCS64 gives x9-x15 as temporaries and
 * this backend has already spent six of them: x9 is the accumulator,
 * x10 the second operand, x11 the address scratch, x12 a second scratch
 * for big offsets and indirect targets, and x13/x14 the atomics' extra
 * registers. That leaves x15.
 *
 * x13, x14 and x15 go in, and FIRST, because they are caller-saved: a
 * short-lived value prefers one and skips the prologue save entirely.
 * They cost no CFI rule either -- ra_allocate reports only CALLEE-saved
 * registers in `used_out` -- so the eight-rule bound above is untouched.
 *
 * x13 and x14 are the atomics' extra registers and nothing else's, so
 * they are available to any function that has no atomic op. That is not
 * a question the shared allocator needs to learn: the backend simply
 * hands ra_allocate a different ra_target (A64_RA_ATOMIC) for the
 * functions that do.
 *
 * It could not go in until the allocator was told about the calls this
 * file makes without an IR_CALL. Long double arithmetic becomes
 * __addtf3 and friends, __int128 divide becomes __divti3, and a value
 * live across one looked to `crosses` like a value that crossed
 * nothing -- so the first attempt at this produced a data abort at 0x10
 * in tests/exec/complex.c, an address computed before a `bl __divtf3`
 * and used after it. a64_op_calls_helper is the answer, and x13/x14 can
 * follow once a function with no atomic op can say so.
 *
 * Which is how the first version of this pool was wrong. It read
 * "x9-x15 are caller-saved temporaries" off the ABI and handed out
 * x12, x13 and x14 -- registers the backend was already using -- so a
 * memcpy's scratch and an allocated value took turns in the same
 * register. `struct S s = mk(7, 35);` came back holding the address of
 * its own copy loop. The ABI says which registers a CALLER may clobber;
 * it does not say which ones a particular backend has left. */

static int a64_callee_saved(int reg) { return reg >= 19 && reg <= 28; }

/* A load that needs no extension is a plain move, so the value may stay
 * where it is: `ldr w` already zero-extends into the 64-bit register, so
 * only a SIGNED narrowing widen has to emit anything. */
static int a64_ldvar_plain(int size, int sign, int w)
{
    return size == 8 || (size == 4 && !(sign && w == 8));
}

/* The two dispatchers below handle every instruction whose operands are
 * sixteen bytes, and SOME of their arms call a libgcc helper: long
 * double arithmetic and its conversions, __int128 multiply, divide,
 * remainder and shifts. This answers for the whole of both, not just
 * the calling arms -- being wrong in the other direction costs one
 * register on one value, and being wrong in this direction costs a
 * clobbered address (regalloc.h). */
static int a64_ld_ins(const struct ir_ins *i);
static int a64_i128_ins(const struct ir_ins *i);
static int a64_op_calls_helper(const struct ir_ins *i)
{
    return a64_i128_ins(i) || a64_ld_ins(i);
}

/* Is x13/x14 spoken for? They are the atomics' scratch and nothing
 * else's, so only a function containing one has to give them up. */
static int a64_has_atomic(const struct ir_func *fn)
{
    for (int n = 0; n < fn->nins; n++)
        switch (fn->ins[n].op) {
        case IR_XCHG: case IR_XADD: case IR_ARMW:
        case IR_CAS: case IR_CAS16: case IR_CMPXCHG:
            return 1;
        default:
            break;
        }
    return 0;
}

static const struct ra_target A64_RA = {
    A64_POOL, A64_NPOOL,
    A64_POOL_VA, A64_NPOOL_VA,
    a64_callee_saved,
    a64_ldvar_plain,
    1, 1, 0,       /* a scalar-integer call argument is moved into its
                    * argument register from wherever it lives, as part
                    * of the parallel move above. A scalar return goes
                    * out through ld_slot, which
                    * takes it from a register when it has one. A
                    * memcpy's addresses are still read from their
                    * slots, so those values have to stay there. */
    a64_op_calls_helper
};

static const struct ra_target A64_RA_ATOMIC = {
    A64_POOL_AT, A64_NPOOL_AT,
    A64_POOL_VA_AT, A64_NPOOL_VA_AT,
    a64_callee_saved,
    a64_ldvar_plain,
    0, 1, 0,       /* a scalar return goes out through ld_slot, which
                    * takes it from a register when it has one. A call's
                    * arguments and a memcpy's addresses are still read
                    * from their slots, so those values have to stay
                    * there. */
    a64_op_calls_helper
};

/* Where each vreg lives: a register, or -1 for its stack slot. NULL when
 * the function is not allocated at all. */
static int *g_a64_loc;
static int a64_in_reg(int v)
{
    return g_a64_loc && v >= 0 && g_a64_loc[v] >= 0;
}

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
/* Darwin's arm64 passes every VARIADIC argument on the stack, where
 * AAPCS64 gives it a register like any other. Apple documents it as a
 * deliberate divergence, and it is invisible until you call someone
 * else's printf: our own variadic functions would read the registers we
 * wrote and agree with themselves.
 *
 * Exhausting both register files is how it is expressed here rather
 * than a fourth placement rule, because that is exactly what the
 * divergence IS -- the argument is placed by the ordinary C.11/C.12
 * stack path, having found no registers left. */
static int a64_on_stack_here(int k, int sret_first, int varargs, int nfixed)
{
    if (!varargs || target_os_get() != TGT_OS_DARWIN)
        return 0;
    return (sret_first ? k - 1 : k) >= nfixed;
}

static void a64_place_arg(const struct ir_arg *a, int k, int sret_first,
                          struct a64_cursor *cu, struct a64_argplan *p,
                          int varargs, int nfixed)
{
    if (sret_first && k == 0) {
        memset(p, 0, sizeof *p);
        p->size = 8;
        p->where = AP_X;
        p->reg = 8;
        p->nreg = 1;
        return;
    }
    if (a64_on_stack_here(k, sret_first, varargs, nfixed)) {
        cu->ngrn = 8;
        cu->nsrn = 8;
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

/* The offset a slot nothing names is given. A gigabyte above the frame
 * base is not a frame; an access that reaches it faults on the spot
 * rather than reading whatever the stack happens to hold there. THE
 * RULE, applied to an offset -- x86-64's DEAD_SLOT_OFF is the same
 * idea from the other side of rbp. */
#define A64_DEAD_SLOT 0x40000000L

static long *layout_frame(struct ir_func *fn, struct a64_frame *fr,
                          int want_debug)
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
            a64_place_arg(&i->argv[k], k, i->sret_first, &cu, &pl,
                          i->call_varargs, i->call_nfixed);
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
    /* Darwin needs none of this. Its variadic arguments arrive on the
     * stack, so there is nothing to save and nothing to record: its
     * va_list is one pointer into the caller's frame. 224 bytes and
     * sixteen stores per variadic function that would never be read. */
    if (f->is_varargs && target_os_get() != TGT_OS_DARWIN) {
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

    /* A local no instruction names any more needs no stack (regalloc.h):
     * SROA leaves exactly that behind once a split aggregate is
     * mentioned nowhere, and it would otherwise keep its full size on
     * the frame for the rest of the function. */
    char *lref = ra_locals_referenced(fn, want_debug);
    for (int v = 0; v < fn->nvars; v++) {
        /* ...and one the allocator put in a REGISTER needs none either.
         * Every read of such a local now goes through ld_slot/rd, which
         * take it from that register, so the eight bytes behind it were
         * being reserved and never touched: `add3` carried a 64-byte
         * frame for three parameters that never left x20-x22. */
        if (!lref[v] || ra_slot_dead(fn, g_a64_loc, v, want_debug)) {
            disp[v] = A64_DEAD_SLOT;
            continue;
        }
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
    free(lref);

    /* Temps share a coalesced pool of eight-byte slots (D-011's shared
     * ra_coalesce_temps, the same one x86-64 uses) rather than taking
     * one each. Two temps whose live ranges do not overlap can sit in
     * the same eight bytes, and after mem2reg has split a variable into
     * SSA versions most of them do not overlap at all.
     *
     * A sixteen-byte temp -- a long double, an __int128, a vector --
     * keeps its own aligned slot outside the pool, because the pool's
     * slots are eight. */
    running = (running + 7) & ~7L;
    int has_cgoto = 0;
    for (int n = 0; n < fn->nins; n++)
        if (fn->ins[n].op == IR_IGOTO || fn->ins[n].op == IR_LABELADDR)
            has_cgoto = 1;
    struct ra_slots so = { g_a64_loc, g_a64_opt_frames, has_cgoto };
    int npool = 0;
    int *tslot = ra_coalesce_temps(fn, fn->nvars, &so, &npool);
    long temp_base = running;
    for (int t = fn->nvars; t < fn->nvregs; t++) {
        int k = t - fn->nvars;
        if (!tslot || tslot[k] < 0) { disp[t] = temp_base; continue; }
        disp[t] = temp_base + (long)tslot[k] * 8;
    }
    running = temp_base + (long)npool * 8;
    free(tslot);
    for (int t = fn->nvars; t < fn->nvregs; t++)
        if (g_a64_wide && g_a64_wide[t]) {
            running = (running + 15) & ~15L;
            disp[t] = running;
            running += 16;
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

/* Load vreg into `reg`, extending per size/sign into a w-wide value.
 *
 * An ALLOCATED vreg is already in a register, so this is a move rather
 * than a load -- and the extension still has to happen, because the
 * caller asked for a w-wide value and the register holds whatever the
 * last store put there. A plain-width move (a64_ldvar_plain's question,
 * asked here of the same size/sign/width) needs no extension at all,
 * and a move to the register it already occupies needs nothing. */
static void ld_slot(struct code *t, const long *sd, int vreg, int reg,
                    int size, int sign, int w)
{
    if (a64_in_reg(vreg)) {
        int src = g_a64_loc[vreg];
        /* a64_ldvar_plain is the allocator's own question -- "is this
         * narrow read just a move" -- and it has to be asked with that
         * one function, not a second copy of the rule. A copy here said
         * a SIGNED four-byte read at four-byte width still needed
         * sign-extending, where the allocator had already called it
         * plain: every `int` parameter read cost an `asr w, w, #0` that
         * extends a value into the width it already has. */
        if (a64_ldvar_plain(size, sign, w)) {
            if (src != reg)
                a64_mov_reg(t, reg, src, 8);
        } else {
            a64_extend(t, reg, src, size, sign, w);
        }
        return;
    }
    a64_ldr(t, reg, FB, sd[vreg], size, sign, w);
}

/* Store `reg`'s low `size` bytes into vreg. */
static void st_slot(struct code *t, const long *sd, int vreg, int reg,
                    int size)
{
    if (a64_in_reg(vreg)) {
        int dst = g_a64_loc[vreg];
        /* The register keeps the value at its natural width; a narrower
         * store leaves the high bits of the DESTINATION as they were in
         * a slot, so they are cleared here to match. */
        if (size >= 8) {
            if (dst != reg)
                a64_mov_reg(t, dst, reg, 8);
        } else {
            a64_extend(t, dst, reg, size, 0, 8);
        }
        return;
    }
    a64_str(t, reg, FB, sd[vreg], size);
}

/* Does this comparison exist only to be branched on?
 *
 * `if (a < b)` lowers to a compare that produces a 0/1 value and a
 * branch that tests it against zero, which on this machine is
 *
 *     cmp  w0, w1
 *     cset x2, lt
 *     cbz  w2, L
 *
 * where `cmp w0, w1; b.ge L` says the same thing. The condition codes
 * are already in NZCV; materialising them into a register and testing
 * that register is a round trip. gcc emits 119 csets over this corpus
 * where this backend emitted 3140.
 *
 * The two instructions have to be ADJACENT -- anything between them
 * could write NZCV -- and the branch has to be the comparison's only
 * reader, or the value is wanted for itself. A label cannot fall
 * between them either: a label is an instruction here, so requiring the
 * branch at n+1 already says so, and nothing can jump into the middle
 * of the pair. */
static int cmp_feeds_branch(struct ir_func *fn, int n, const int *usecnt)
{
    struct ir_ins *c = &fn->ins[n];
    if (n + 1 >= fn->nins || c->dst < 0 || c->dst >= fn->nvregs)
        return 0;
    struct ir_ins *b = &fn->ins[n + 1];
    return (b->op == IR_BRZ || b->op == IR_BRNZ) && b->a == c->dst &&
           usecnt[c->dst] == 1;
}

/* ---- operating where the value already is -----------------------------
 *
 * `ld_slot` and `st_slot` answer "put this vreg in THAT register", which
 * is the right question for a site that needs a particular register --
 * an argument, a return value, the address of a copy. For ordinary
 * arithmetic it is the wrong one, because it forces a round trip that
 * the allocator already made unnecessary:
 *
 *     mov x9, x20         a's home is x20 already
 *     mov x10, x21        b's home is x21 already
 *     add x9, x9, x10
 *     mov x21, x9         and the result belongs in x21
 *
 * where one `add x21, x20, x21` does the whole thing. `add3` -- three
 * parameters, two adds -- came to 31 instructions against gcc's three,
 * and nearly all of the difference was this.
 *
 * So the three below ask the other question. `rd` says which register a
 * value can be READ from, loading into the scratch only when there is no
 * home to read. `wr` says which register a result may be COMPUTED in,
 * and `wrote` writes it back if that register was the scratch. The pool
 * (x20-x26) and the scratch registers (x9-x14) are disjoint, so a home
 * register can never be the one an operand was loaded into.
 *
 * `wr` is only safe for a value computed by a SINGLE instruction, which
 * reads its operands and writes its destination at once. A sequence that
 * writes the destination before its last read -- IR_MOD's divide and
 * msub -- would clobber an operand still to come, so those keep the
 * accumulator and the explicit store. */
static int rd(struct code *t, const long *sd, int vreg, int scratch)
{
    if (a64_in_reg(vreg))
        return g_a64_loc[vreg];
    a64_ldr(t, scratch, FB, sd[vreg], 8, 0, 8);
    return scratch;
}

static int wr(int vreg, int scratch)
{
    return a64_in_reg(vreg) ? g_a64_loc[vreg] : scratch;
}

static void wrote(struct code *t, const long *sd, int vreg, int reg)
{
    if (!a64_in_reg(vreg))
        a64_str(t, reg, FB, sd[vreg], 8);
}

/* `rd` at a requested width. A resident vreg whose register already
 * holds the value at that width is read where it is; anything else goes
 * through the scratch, extended exactly as ld_slot would have. The
 * plain-width test is ld_slot's own, so this never widens what that
 * would have left alone. */
static int rd_ext(struct code *t, const long *sd, int vreg, int scratch,
                  int size, int sign, int w)
{
    if (a64_in_reg(vreg)) {
        int src = g_a64_loc[vreg];
        if (a64_ldvar_plain(size, sign, w))
            return src;
        a64_extend(t, scratch, src, size, sign, w);
        return scratch;
    }
    a64_ldr(t, scratch, FB, sd[vreg], size, sign, w);
    return scratch;
}

/* `wrote` for a result narrower than a word: a vreg with no home gets
 * `size` bytes, never eight, because the slot beside it is somebody
 * else's. */
static void wrote_n(struct code *t, const long *sd, int vreg, int reg,
                    int size)
{
    if (!a64_in_reg(vreg))
        a64_str(t, reg, FB, sd[vreg], size);
}

/* Operand b as a register to read, folding the optimizer's immediate. */
static int rd_b(struct code *t, const long *sd, struct ir_ins *i)
{
    if (i->imm_b) {
        a64_mov_imm(t, A64_TMP, i->imm, i->w);
        return A64_TMP;
    }
    return rd(t, sd, i->b, A64_TMP);
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

/* ---- moving a whole set of registers at once -------------------------
 *
 * Homing the parameters, and setting up a call's arguments, are both
 * "put these values in those registers" -- and doing that one move at a
 * time is only safe while the sources and the destinations are disjoint
 * sets. They are today, because every allocatable register (x13-x15,
 * x20-x26) is one AAPCS64 never passes an argument in. The moment x0-x7
 * become allocatable that stops being true: `mov x0, x3` followed by
 * `mov x3, x1` is fine, and `mov x0, x3` followed by `mov x1, x0` is a
 * parameter delivered twice and another one lost.
 *
 * So emit the set as a unit: repeatedly take any move whose destination
 * is nobody else's remaining source, and when only cycles are left,
 * break one by parking its destination in the scratch. x12 is that
 * scratch and is in no pool.
 *
 * This is the x86 backend's emit_reg_parallel_move, which has wanted a
 * second caller for a while; once this one has run in anger the pair
 * should be lifted (D-011). */
static void a64_parallel_move(struct code *t, int *dst, int *src, int n,
                              int scratch)
{
    char done[MAX_PARAMS];
    int remaining = 0;
    for (int i = 0; i < n; i++) {
        done[i] = (dst[i] == src[i]);        /* an identity move is a no-op */
        if (!done[i]) remaining++;
    }
    while (remaining > 0) {
        int progressed = 0;
        for (int i = 0; i < n; i++) {
            if (done[i]) continue;
            int blocked = 0;
            for (int j = 0; j < n; j++)
                if (!done[j] && j != i && src[j] == dst[i]) { blocked = 1; break; }
            if (blocked) continue;
            a64_mov_reg(t, dst[i], src[i], 8);
            done[i] = 1; remaining--; progressed = 1;
        }
        if (progressed)
            continue;
        int c = -1;                          /* only cycles left: break one */
        for (int i = 0; i < n; i++) if (!done[i]) { c = i; break; }
        a64_mov_reg(t, scratch, dst[c], 8);
        for (int j = 0; j < n; j++)
            if (!done[j] && src[j] == dst[c]) src[j] = scratch;
    }
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
    int used_callee[A64_NPOOL], nsave = 0;

    g_a64_wide = cg_wide_vregs(fn);
    /* An indirect jump makes liveness unsound -- a computed goto's
     * targets are unknown, so a value's live range cannot be computed --
     * and the x86 backend refuses to allocate such a function for the
     * same reason. So does a landing pad, which is entered on a
     * control-flow edge the dataflow does not see. */
    g_a64_loc = NULL;
    if (g_a64_regalloc && !fn->neh) {
        int cgoto = 0, n;
        for (n = 0; n < fn->nins; n++)
            if (fn->ins[n].op == IR_IGOTO || fn->ins[n].op == IR_LABELADDR) {
                cgoto = 1;
                break;
            }
        if (!cgoto)
            g_a64_loc = ra_allocate(fn,
                                    a64_has_atomic(fn) ? &A64_RA_ATOMIC
                                                       : &A64_RA,
                                    g_a64_wide, used_callee, &nsave);
    }
    long *sd = layout_frame(fn, &fr, want_debug);
    /* Room for the callee-saved registers the allocator took. Eight
     * bytes each, rounded to sixteen: AAPCS64 wants sp 16-aligned at
     * every instruction boundary, not merely at a call. */
    long save_base = fr.size;
    if (nsave)
        fr.size += ((nsave * 8) + 15) & ~15L;

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
    /* The allocator's callee-saved registers, saved FIRST so that every
     * save in this prologue is complete by one program point -- the
     * unwind tables carry a single "and here they are all saved"
     * offset, and x19's save below joins the same list. */
    for (int k = 0; k < nsave; k++) {
        a64_str(t, used_callee[k], A64_SP, save_base + k * 8, 8);
        f->cfi_reg[f->cfi_nsaved] = used_callee[k];
        f->cfi_off[f->cfi_nsaved] = save_base + k * 8 - fr.size - 16;
        f->cfi_nsaved++;
    }
    if (nsave)
        f->cfi_saved_at = t->len - f->code_off;
    if (fn->has_alloca) {
        a64_str(t, A64_FBREG, A64_SP, fr.fb_save, 8);
        f->cfi_reg[f->cfi_nsaved] = 19;
        f->cfi_off[f->cfi_nsaved] = fr.fb_save - fr.size - 16; /* CFA = x29+16 */
        f->cfi_nsaved++;
        f->cfi_saved_at = t->len - f->code_off;
        a64_add_imm(t, A64_FBREG, A64_SP, 0, 8);     /* mov x19, sp */
        g_fb = A64_FBREG;
    }

    /* A variadic function saves every argument register first, raw, before
     * anything can disturb them: va_arg walks these areas later. Under
     * -mgeneral-regs-only (the kernel) the q registers are never touched —
     * gcc makes the same choice, and __vr_offs then says "none". */
    if (f->is_varargs && fr.gr_save >= 0) {
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
        struct a64_cursor cu;
        /* A parameter whose home is a register is delivered by a move
         * from the register it arrived in, and those moves go out as a
         * SET (a64_parallel_move): a home may be an argument register
         * that another parameter has not been read out of yet. The
         * narrowing ones are re-extended afterwards, in place on the
         * home, which reads and writes only that register and so cannot
         * disturb the rest. */
        int pmv_dst[MAX_PARAMS], pmv_src[MAX_PARAMS], npmv = 0;
        int pext_reg[MAX_PARAMS], pext_size[MAX_PARAMS], npext = 0;
        if (fr.sret >= 0)
            a64_str(t, A64_SRET, FB, fr.sret, 8);
        /* Two passes, because everything that READS an incoming
         * argument register has to happen before anything WRITES one,
         * and a home may now be an argument register. Pass one takes
         * every parameter that arrived in a register: the composites go
         * straight to memory, the scalars are collected for the move
         * below. Pass two takes the ones that arrived on the stack --
         * they read the caller's frame and write their homes, which is
         * only safe once the move has emptied those registers. Getting
         * this backwards cost `many(1..14)` six of its arguments. */
        for (int pass = 0; pass < 2; pass++) {
        cu.ngrn = 0; cu.nsrn = 0; cu.nsaa = 0; cu.byref_bytes = 0;
        for (int p = 0; p < f->nparams; p++) {
            struct a64_argplan pl;
            /* 0, 0: a function's DECLARED parameters are all named, so
             * the Darwin variadic rule has nothing to act on here. What
             * a variadic callee does with the rest is va_arg's problem,
             * not this placement's. */
            a64_place_arg(&fn->param_abi[p], p, f->sret_first, &cu, &pl,
                          0, 0);
            int on_stack = (pl.where != AP_V && pl.where != AP_X && !pl.byref)
                        || (pl.byref && pl.where == AP_STACK);
            if (on_stack != pass)
                continue;                  /* the other pass owns this one */
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
                    /* st_slot, not a64_str: an ALLOCATED parameter's
                     * home is a register, and writing its stack slot
                     * would leave that register holding whatever the
                     * caller left there. This one line was the whole
                     * bug -- `struct S s = mk(7, 35)` came back wrong
                     * because mk's two parameters were allocated and
                     * never arrived. */
                {
                    int psz = pl.size > 8 ? 8 : pl.size;
                    if (a64_in_reg(p)) {
                        pmv_dst[npmv] = g_a64_loc[p];
                        pmv_src[npmv] = pl.reg;
                        npmv++;
                        if (psz < 8) {
                            pext_reg[npext] = g_a64_loc[p];
                            pext_size[npext] = psz;
                            npext++;
                        }
                    } else {
                        st_slot(t, sd, p, pl.reg, psz);
                    }
                }
            } else if (pl.is_struct || pl.size == 16) {  /* or a long double */
                addr_of(t, A64_ADDR, FB, sd[p]);
                addr_of(t, A64_TMP, A64_FP, 16 + pl.stk_off);
                emit_copy(t, A64_ADDR, A64_TMP, pl.size);
            } else {
                a64_ldr(t, A64_ACC, A64_FP, 16 + pl.stk_off, 8, 0, 8);
                st_slot(t, sd, p, A64_ACC, pl.size > 8 ? 8 : pl.size);
            }
        }
        if (pass == 0) {
            if (npmv)
                a64_parallel_move(t, pmv_dst, pmv_src, npmv, A64_SCR);
            for (int k = 0; k < npext; k++)
                a64_extend(t, pext_reg[k], pext_reg[k], pext_size[k], 0, 8);
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
    /* The last instruction that emits anything. A `return` there needs
     * no branch to the epilogue: the epilogue starts at the next word.
     * Trailing labels emit nothing, so they do not count -- and a branch
     * to one lands on the epilogue either way. */
    int last_code = fn->nins - 1;
    while (last_code >= 0 && fn->ins[last_code].op == IR_LABEL)
        last_code--;
    /* A comparison that only feeds the next branch leaves its condition
     * here instead of in a register; the branch picks it up. -1 when
     * there is none, which is every other instruction. */
    int *usecnt = xmalloc((size_t)(fn->nvregs ? fn->nvregs : 1) * sizeof *usecnt);
    ra_count_vreg_uses(fn, usecnt);
    int fused_cc = -1;

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
        case IR_CONST: {
            int d = wr(i->dst, A64_ACC);
            a64_mov_imm(t, d, i->imm, i->w);
            wrote(t, sd, i->dst, d);
            break;
        }

        case IR_MOV: {
            int src = rd(t, sd, i->a, A64_ACC);
            if (a64_in_reg(i->dst)) {
                int d = g_a64_loc[i->dst];
                if (d != src) a64_mov_reg(t, d, src, 8);
            } else {
                a64_str(t, src, FB, sd[i->dst], 8);
            }
            break;
        }

        case IR_ADD: case IR_SUB: case IR_AND: case IR_OR: case IR_XOR: {
            if (i->flt) {
                /* Only +, - and * ever reach here as floats; the bitwise
                 * operators have no floating-point form in C. */
                fbin(t, sd, i, i->op == IR_ADD ? '+' : '-');
                break;
            }
            int op = i->op == IR_ADD ? '+' : i->op == IR_SUB ? '-'
                   : i->op == IR_AND ? '&' : i->op == IR_OR  ? '|' : '^';
            int ra = rd(t, sd, i->a, A64_ACC), rb = rd_b(t, sd, i);
            int d = wr(i->dst, A64_ACC);
            a64_alu_reg(t, op, d, ra, rb, i->w);
            wrote(t, sd, i->dst, d);
            break;
        }

        case IR_MUL:
            if (i->flt) { fbin(t, sd, i, '*'); break; }
            else {
                int ra = rd(t, sd, i->a, A64_ACC), rb = rd_b(t, sd, i);
                int d = wr(i->dst, A64_ACC);
                a64_mul(t, d, ra, rb, i->w);
                wrote(t, sd, i->dst, d);
            }
            break;

        case IR_DIV:
            if (i->flt) { fbin(t, sd, i, '/'); break; }
            else {
                int ra = rd(t, sd, i->a, A64_ACC), rb = rd_b(t, sd, i);
                int d = wr(i->dst, A64_ACC);
                a64_div(t, d, ra, rb, i->sign, i->w);
                wrote(t, sd, i->dst, d);
            }
            break;

        case IR_MOD:
            /* q = a / b ; r = a - q*b. msub does the second half. */
            ld_slot(t, sd, i->a, A64_ACC, 8, 0, 8);
            operand_b(t, sd, i);
            a64_div(t, A64_ADDR, A64_ACC, A64_TMP, i->sign, i->w);
            a64_msub(t, A64_ACC, A64_ADDR, A64_TMP, A64_ACC, i->w);
            st_slot(t, sd, i->dst, A64_ACC, 8);
            break;

        case IR_SHL: case IR_SHR: {
            int ra = rd(t, sd, i->a, A64_ACC), rb = rd_b(t, sd, i);
            int d = wr(i->dst, A64_ACC);
            a64_shift_reg(t, i->op == IR_SHL ? '<' : (i->sign ? '>' : 'u'),
                          d, ra, rb, i->w);
            wrote(t, sd, i->dst, d);
            break;
        }

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
                if (cmp_feeds_branch(fn, n, usecnt)) { fused_cc = cc; break; }
                int d = wr(i->dst, A64_ACC);
                a64_cset(t, d, cc);
                wrote(t, sd, i->dst, d);
                break;
            }
            else {
                /* cset writes the destination only after cmp has read
                 * both operands, so the destination may be one of them. */
                int ra = rd(t, sd, i->a, A64_ACC), rb = rd_b(t, sd, i);
                int cc = cond_for(i->pred, i->sign);
                a64_cmp_reg(t, ra, rb, i->w);
                if (cmp_feeds_branch(fn, n, usecnt)) { fused_cc = cc; break; }
                int d = wr(i->dst, A64_ACC);
                a64_cset(t, d, cc);
                wrote(t, sd, i->dst, d);
            }
            break;

        case IR_LDVAR: {
            int d = wr(i->dst, A64_ACC);
            if (a64_in_reg(i->a)) {
                int src = g_a64_loc[i->a];
                if (a64_ldvar_plain(i->size, i->sign, i->w)) {
                    if (d != src) a64_mov_reg(t, d, src, 8);
                } else {
                    a64_extend(t, d, src, i->size, i->sign, i->w);
                }
            } else {
                a64_ldr(t, d, FB, sd[i->a], i->size, i->sign, i->w);
            }
            wrote(t, sd, i->dst, d);
            break;
        }

        case IR_STVAR: {
            int src = rd(t, sd, i->a, A64_ACC);
            if (a64_in_reg(i->dst)) {
                int d = g_a64_loc[i->dst];
                if (i->size >= 8) {
                    if (d != src) a64_mov_reg(t, d, src, 8);
                } else {
                    a64_extend(t, d, src, i->size, 0, 8);
                }
            } else {
                a64_str(t, src, FB, sd[i->dst], i->size);
            }
            break;
        }

        case IR_ADDR: {
            int d = wr(i->dst, A64_ACC);
            addr_of(t, d, FB, sd[i->a]);
            wrote(t, sd, i->dst, d);
            break;
        }

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
            /* Through the GOT when adrp/add cannot give the answer.
             *
             * A weak undefined symbol is one case everywhere: its
             * address is meant to be 0, and adrp/add would give the
             * page of the pc instead.
             *
             * On Darwin it is EVERY undefined symbol. One that resolves
             * from a dylib has no address at static-link time at all,
             * and ld says so rather than guessing: "target does not
             * have address". So the indirection is the rule there and
             * the exception elsewhere. */
            struct gsite hi, lo;
            if (i->glob->is_tls) {
                /* Not an address in this image: an offset into a block
                 * that is different for every thread. adrp names a page
                 * of the program; this names the running thread. */
                a64_mrs_tpidr(t, A64_ACC);
                hi.patch_off = a64_add_hi12(t, A64_ACC, A64_ACC);
                hi.glob = i->glob;
                hi.kind = RK_TPREL_HI12;
                PUSH(st->g, st->ng, st->capg, hi);
                lo.patch_off = a64_add_lo12(t, A64_ACC, A64_ACC);
                lo.glob = i->glob;
                lo.kind = RK_TPREL_LO12;
                PUSH(st->g, st->ng, st->capg, lo);
                goto gaddr_done;
            }
            int got = !i->glob->defined &&
                      (i->glob->is_weak || target_os_get() == TGT_OS_DARWIN);
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
        gaddr_done:;
            st_slot(t, sd, i->dst, A64_ACC, 8);
            break;
        }

        case IR_FADDR: {
            /* As IR_GADDR: on Darwin an undefined function has no
             * link-time address, so its address comes from the GOT. */
            int got = !i->callee->has_defn &&
                      (i->callee->is_weak ||
                       target_os_get() == TGT_OS_DARWIN);
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

        case IR_LOAD: {
            int addr = rd(t, sd, i->a, A64_ADDR);
            int d = wr(i->dst, A64_ACC);
            a64_ldr(t, d, addr, 0, i->size, i->sign, i->w);
            wrote(t, sd, i->dst, d);
            break;
        }

        case IR_STORE: {
            int addr = rd(t, sd, i->a, A64_ADDR);
            int val = rd(t, sd, i->b, A64_ACC);
            a64_str(t, val, addr, 0, i->size);
            break;
        }

        case IR_EXT: {
            int src = rd(t, sd, i->a, A64_ACC);
            int d = wr(i->dst, A64_ACC);
            a64_extend(t, d, src, i->size, i->sign, i->w);
            wrote(t, sd, i->dst, d);
            break;
        }

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

        case IR_SELECT: {
            /* csel: the condition into ACC, the two arms into TMP and
             * SCR, and the choice without a branch. Both arms are
             * already values -- the pass that built this refused
             * anything that could fault -- so there is nothing to
             * guard. NE, because the condition is a 0/1 truth value. */
            int rc = rd_ext(t, sd, i->a, A64_ACC, 4, 0, 4);
            int rb = rd_ext(t, sd, i->b, A64_TMP, i->w, i->sign, i->w);
            int rs = rd_ext(t, sd, i->c, A64_SCR, i->w, i->sign, i->w);
            a64_cmp_reg(t, rc, A64_ZR, 4);
            int d = wr(i->dst, A64_ACC);
            a64_csel(t, d, rb, rs, A64_NE, i->w);
            wrote_n(t, sd, i->dst, d, i->w);
            break;
        }
        case IR_BRZ: case IR_BRNZ: {
            struct a64_fix fx;
            if (fused_cc >= 0) {
                /* BRNZ branches when the comparison was true, BRZ when
                 * it was false -- and a condition code's inverse is its
                 * low bit flipped. */
                fx.at = a64_bcond(t, i->op == IR_BRZ ? (fused_cc ^ 1)
                                                     : fused_cc);
                fused_cc = -1;
            } else {
                int rc = rd(t, sd, i->a, A64_ACC);
                fx.at = a64_cbz(t, rc, i->op == IR_BRNZ, i->w);
            }
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
            if (n != last_code) {
                struct a64_fix fx;
                fx.at = a64_b(t); fx.label = -1; fx.kind = FIX_B26;
                PUSH(retfix, nret, capret, fx.at);
            }
            break;
        }

        case IR_CALL: {
            struct a64_argplan pl[MAX_PARAMS];
            struct a64_cursor cu = { 0, 0, 0, 0 };
            for (int k = 0; k < i->nargs; k++)
                a64_place_arg(&i->argv[k], k, i->sret_first, &cu, &pl[k],
                              i->call_varargs, i->call_nfixed);

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
            /* A scalar argument already in a register is moved, and
             * those moves go out as a SET: once x0-x7 are allocatable a
             * source can be an argument register another argument has
             * not been written to yet. Everything else -- a struct, a
             * float, an __int128, a by-reference copy, a value in its
             * slot -- is loaded from memory and cannot collide. */
            int amv_dst[MAX_PARAMS], amv_src[MAX_PARAMS], namv = 0;
            for (int k = 0; k < i->nargs; k++) {
                int v = i->argv[k].vreg;
                if (pl[k].where == AP_X && !pl[k].byref && !pl[k].is_struct &&
                    pl[k].nreg != 2 && a64_in_reg(v)) {
                    amv_dst[namv] = pl[k].reg;
                    amv_src[namv] = g_a64_loc[v];
                    namv++;
                }
            }
            /* FIRST, because it is the only thing here that READS a home,
             * and a home may be an argument register something else is
             * about to write. Everything below reads memory or sp. */
            if (namv)
                a64_parallel_move(t, amv_dst, amv_src, namv, A64_SCR);
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
                    } else if (!a64_in_reg(v)) {
                        ld_slot(t, sd, v, pl[k].reg, 8, 0, 8);
                    }                    /* else: the move above did it */
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
                /* 0, 0: a function's DECLARED parameters are all named,
                 * so the Darwin variadic rule has nothing to act on
                 * here. What a variadic callee does with the rest is
                 * va_arg's problem, not this placement's. */
                a64_place_arg(&fn->param_abi[p], p, f->sret_first, &cu, &pl,
                              0, 0);
            if (target_os_get() == TGT_OS_DARWIN) {
                /* Darwin's va_list is a `char *` at the next stack
                 * argument and nothing else: every variadic argument
                 * came on the stack, so there is no register save area
                 * to point at and no record to hold the offsets into
                 * it. This is the AAPCS64 __stack value, stored into
                 * the va_list itself rather than into a record's first
                 * word -- and it is the whole of va_start there. */
                addr_of(t, A64_ACC, A64_FP, 16 + ((cu.nsaa + 7) & ~7L));
                ld_slot(t, sd, i->a, A64_ADDR, 8, 0, 8);
                a64_str(t, A64_ACC, A64_ADDR, 0, 8);
                break;
            }
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
                if (!ia->out[k].inout || ia->out[k].mem)
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
                /* An "m" output was written BY the template, through the
                 * address this register holds; storing the register over
                 * it would destroy what the asm produced. */
                if (ia->out[k].mem)
                    continue;
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
    for (int k = 0; k < nsave; k++)
        a64_ldr(t, used_callee[k], FB, save_base + k * 8, 8, 0, 8);
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
    free(g_a64_loc);
    g_a64_loc = NULL;

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
    free(usecnt);
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
    g_a64_regalloc = regalloc;
    g_a64_opt_frames = optimize;

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
