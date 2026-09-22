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
static int g_has_cgoto;         /* function has a computed goto: liveness is
                                 * imprecise -> no regalloc / no slot coalescing */

static int *coalesce_temps(struct ir_func *fn, int nvars, int *npool_out)
{
    int nins = fn->nins, nvr = fn->nvregs;
    int ntemp = nvr - nvars;
    *npool_out = 0;
    if (ntemp <= 0)
        return NULL;

    /* basic-block id per instruction: a new block begins at a label and after
     * any branch/jump/return/ud2. */
    int *blk = xmalloc((size_t)(nins ? nins : 1) * sizeof *blk);
    int b = 0;
    for (int i = 0; i < nins; i++) {
        enum ir_op op = fn->ins[i].op;
        if (op == IR_LABEL) b++;
        blk[i] = b;
        if (op == IR_JMP || op == IR_BRZ || op == IR_BRNZ ||
            op == IR_RET || op == IR_UD2)
            b++;
    }

    /* [first,last] instruction index over every appearance of each temp. */
    int *first = xmalloc((size_t)ntemp * sizeof *first);
    int *last  = xmalloc((size_t)ntemp * sizeof *last);
    for (int k = 0; k < ntemp; k++) { first[k] = -1; last[k] = -1; }
    for (int i = 0; i < nins; i++) {
        struct ir_ins *in = &fn->ins[i];
        int vs[4]; int nv = 0;
        vs[nv++] = in->dst; vs[nv++] = in->a; vs[nv++] = in->b; vs[nv++] = in->c;
        for (int j = 0; j < nv; j++) {
            int v = vs[j];
            if (v >= nvars && v < nvr) {
                int k = v - nvars;
                if (first[k] < 0) first[k] = i;
                last[k] = i;
            }
        }
        if (in->op == IR_CALL)
            for (int a = 0; a < in->nargs; a++) {
                int v = in->argv[a].vreg;
                if (v >= nvars && v < nvr) {
                    int k = v - nvars;
                    if (first[k] < 0) first[k] = i;
                    last[k] = i;
                }
            }
        if (in->op == IR_ASM && in->asm_ir) {
            struct ir_asm *ia = in->asm_ir;
            for (int a = 0; a < ia->nin; a++) {
                int v = ia->in[a].temp;
                if (v >= nvars && v < nvr) {
                    int k = v - nvars;
                    if (first[k] < 0) first[k] = i;
                    last[k] = i;
                }
            }
            for (int a = 0; a < ia->nout; a++) {
                int v = ia->out[a].temp;
                if (v >= nvars && v < nvr) {
                    int k = v - nvars;
                    if (first[k] < 0) first[k] = i;
                    last[k] = i;
                }
            }
        }
    }

    /* order temps by first-appearance (ties by temp index), via buckets keyed
     * on the first index — O(nins+ntemp) and deterministic. Never-appearing
     * temps bucket at `nins`. */
    int *head = xmalloc((size_t)(nins + 1) * sizeof *head);
    for (int i = 0; i <= nins; i++) head[i] = -1;
    int *nxt = xmalloc((size_t)ntemp * sizeof *nxt);
    for (int k = ntemp - 1; k >= 0; k--) {
        int fi = first[k] < 0 ? nins : first[k];
        nxt[k] = head[fi]; head[fi] = k;
    }

    /* linear scan: reuse a freed pool index for a coalescable temp once its
     * previous occupant is dead; a non-coalescable temp takes a fresh index it
     * never gives back. */
    int *slot = xmalloc((size_t)ntemp * sizeof *slot);
    int *freelist = xmalloc((size_t)ntemp * sizeof *freelist);
    int *act_last = xmalloc((size_t)ntemp * sizeof *act_last);
    int *act_idx  = xmalloc((size_t)ntemp * sizeof *act_idx);
    int nfree = 0, nact = 0, pool = 0;
    for (int i = 0; i <= nins; i++) {
        for (int k = head[i]; k >= 0; k = nxt[k]) {
            /* A register-resident temp (-O2 regalloc) never touches memory, so
             * it needs no stack slot — skip it, keeping the frame to the temps
             * that actually spill. (This also caps mem2reg's SSA-temp inflation:
             * the extra versions live in registers, not the frame.) */
            if (g_regalloc && g_loc && g_loc[k + nvars] >= 0) {
                slot[k] = -1;
                continue;
            }
            if (first[k] < 0) {          /* never referenced: no slot needed */
                /* A temp that appears in no instruction is dead — nothing ever
                 * loads or stores it, so it needs no frame slot. This is common
                 * once the optimizer's immediate-fold detaches a CONST and DCE
                 * drops its defining instruction, leaving the temp unreferenced;
                 * giving each one an 8-byte throwaway slot inflates the frame for
                 * nothing, and a recursive kernel function (path walk, tree
                 * sweep) then overflows the kernel stack. Gated to optimizing
                 * builds so -O0 stays byte-identical (its throwaway layout is
                 * unchanged, which the self-host fixed point relies on). */
                slot[k] = g_opt_frames ? -1 : pool++;
                continue;
            }
            int coalescable = (blk[first[k]] == blk[last[k]]) && !g_has_cgoto;
            /* expire actives dead before this temp is defined */
            for (int a = 0; a < nact; ) {
                if (act_last[a] < first[k]) {
                    freelist[nfree++] = act_idx[a];
                    act_last[a] = act_last[nact - 1];
                    act_idx[a]  = act_idx[nact - 1];
                    nact--;
                } else {
                    a++;
                }
            }
            int idx = (coalescable && nfree > 0) ? freelist[--nfree] : pool++;
            slot[k] = idx;
            if (coalescable) {
                act_last[nact] = last[k];
                act_idx[nact]  = idx;
                nact++;
            }
        }
    }
    *npool_out = pool;

    free(blk); free(first); free(last); free(head); free(nxt);
    free(freelist); free(act_last); free(act_idx);
    return slot;
}

/* ---- -O2 register allocation ----
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
#define NLEAF 9
static const int LEAF_POOL[NLEAF] = { 8, 9, 10, 11, 3 /*rbx*/, 12, 13, 14, 15 };

/* Does a register need callee-save preservation (rbx, r12..r15)? r8..r11 are
 * caller-saved — free to clobber, so no prologue slot. */
/* The x86-64 side of the shared allocator (src/arch/regalloc.c). What
 * a machine has to say for itself is small: which registers may be
 * handed out and in what order, which survive a call, and whether a
 * narrow load is a plain move. */
static int ldvar_plain(int size, int sign, int w);
static int is_callee_saved(int reg);

static const struct ra_target X86_RA = {
    LEAF_POOL, NLEAF,
    VARIADIC_POOL, NVARIADIC,
    is_callee_saved,
    ldvar_plain,
    1, 1, 1        /* this backend reads call arguments, scalar returns
                    * and memcpy addresses straight out of a register */
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
    for (int changed = any; changed; ) {
        changed = 0;
        for (int n = 0; n < fn->nins; n++) {
            struct ir_ins *i = &fn->ins[n];
            if (i->op == IR_MOV && i->a >= 0 && w[i->a] && !w[i->dst])
                w[i->dst] = changed = 1;
        }
    }
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
/* A local the allocator put in a register, whose slot therefore holds
 * nothing. Every read of such a local goes through in_reg(), so the slot
 * is dead storage -- and a function whose locals are ALL like this needs
 * no frame at all, which is what makes the leaf prologue below possible.
 *
 * The conditions are deliberately narrower than "in a register": an
 * aggregate is addressed as memory whatever the allocator thinks, an
 * address that escapes has to point at something, and a variadic
 * function's prologue writes the argument file to the frame. The
 * allocator already refuses most of these; the check does not rely on
 * that, because a slot that turns out to be live reads as garbage rather
 * than failing, and the whole point is that nothing quietly reads it. */
static int slot_dead(struct ir_func *fn, const int *loc, int v)
{
    struct func *f = fn->src;
    if (!loc || loc[v] < 0 || f->is_varargs || fn->has_alloca || g_want_debug)
        return 0;
    const struct type *t = f->var_tys[v];
    if (t->kind == TY_STRUCT || t->kind == TY_ARRAY || ty_size(t) > 8 ||
        ty_is_float(t))
        return 0;
    for (int n = 0; n < fn->nins; n++)
        if (fn->ins[n].op == IR_ADDR && fn->ins[n].a == v)
            return 0;
    return 1;
}

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
    int *ssize = xcalloc((size_t)(nls ? nls : 1), sizeof *ssize);
    int *salign = xcalloc((size_t)(nls ? nls : 1), sizeof *salign);
    for (int i = 0; i < fn->nvars; i++) {
        int s = lslot[i];
        if (slot_dead(fn, loc, i))
            continue;               /* it lives in a register; size nothing */
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
        disp[i] = slot_dead(fn, loc, i) ? DEAD_SLOT_OFF : soff[lslot[i]];
    free(lslot); free(ssize); free(salign); free(soff);
    /* Temporaries share a coalesced pool of 8-byte slots (K13) instead of one
     * slot each — the temp region is `npool` slots wide, not (nvregs-nvars). */
    int npool = 0;
    int *tslot = coalesce_temps(fn, fn->nvars, &npool);
    int temp_base = running;
    for (int t = fn->nvars; t < fn->nvregs; t++)
        disp[t] = -(temp_base + (tslot[t - fn->nvars] + 1) * 8);
    running = temp_base + npool * 8;
    free(tslot);
    /* a long double temp: its own 16-aligned 16-byte slot, outside the pool */
    for (int t = fn->nvars; t < fn->nvregs; t++)
        if (g_wide && g_wide[t]) {
            running = (running + 16 + 15) & ~15;
            disp[t] = -running;
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
static void count_vreg_uses(struct ir_func *fn, int *cnt)
{
    for (int v = 0; v < fn->nvregs; v++) cnt[v] = 0;
#define UZ(x) do { int _v=(x); if (_v>=0 && _v<fn->nvregs) cnt[_v]++; } while (0)
    for (int i = 0; i < fn->nins; i++) {
        struct ir_ins *s = &fn->ins[i];
        switch (s->op) {
        case IR_MOV: case IR_NEG: case IR_BNOT: case IR_EXT: case IR_BSWAP:
        case IR_SQRT:
        case IR_I2F: case IR_F2I: case IR_F2F: case IR_LOAD: case IR_LDVAR:
        case IR_ADDR: case IR_STVAR: case IR_VA_START:
        case IR_RET: case IR_BRZ: case IR_BRNZ:
        case IR_ALLOCA: case IR_SPRESTORE:
        case IR_VLOAD: case IR_VSPLAT: case IR_VREDADD: case IR_VWIDEN:
            UZ(s->a); break;
        case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
        case IR_AND: case IR_OR: case IR_XOR: case IR_SHL: case IR_SHR:
        case IR_CMP: case IR_STORE: case IR_MEMCPY: case IR_MEMZERO:
        case IR_XCHG: case IR_XADD: case IR_ARMW:
        case IR_VSTORE: case IR_VBIN:
            UZ(s->a); UZ(s->b); break;
        case IR_CMPXCHG: case IR_CAS: case IR_CAS16:
            UZ(s->a); UZ(s->b); UZ(s->c); break;
        case IR_CALL:
            if (s->indirect) UZ(s->a);
            for (int k = 0; k < s->nargs; k++) UZ(s->argv[k].vreg);
            break;
        case IR_ASM:
            if (s->asm_ir) {
                for (int k = 0; k < s->asm_ir->nin; k++) UZ(s->asm_ir->in[k].temp);
                for (int k = 0; k < s->asm_ir->nout; k++) UZ(s->asm_ir->out[k].temp);
            }
            break;
        default: break;
        }
    }
#undef UZ
}

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
};

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
#define VACC_FIRST 4
#define VACC_N     4
#define VW_SRC     2      /* the vector being widened */
#define VW_SIGN    3      /* and its lanes' extension bits */

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
 * Only when the very next instruction is the single use it has -- then
 * nothing else can ever read the slot, and nothing can come between. */
static int vec_keep(struct ir_func *fn, int n, const int *usecnt, int dst)
{
    if (dst < 0 || !usecnt || usecnt[dst] != 1 || n + 1 >= fn->nins)
        return 0;
    return vec_reads_xmm(&fn->ins[n + 1], dst);
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

/* Copy 16 bytes [sbase+soff] -> [dbase+doff] through rax (neither base may
 * be rax). */
static void copy16(struct code *text, int dbase, int doff, int sbase, int soff)
{
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
            x86_movs_store(text, 0, d, i->w);
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
            x86_movs_load(text, 0, sd[i->a], i->size);
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
    if (fn->has_i128) { g_regalloc = 0; g_regcache = 0; }   /* (gen_i128) */
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
        loc = ra_allocate(fn, &X86_RA, g_wide, used_callee, &nsave);
        g_loc = loc;
    } else {
        g_loc = NULL;
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

    x86_prologue(text, frame, frameless);
    /* for the unwind tables: push rbp ends at +1, mov rbp,rsp at +4 */
    f->cfi_frameless = frameless;
    f->cfi_push = 1;
    f->cfi_frame = 4;
    /* -O2: preserve the callee-saved registers the allocator uses (this
     * function is responsible for them across its own body and its callers). */
    for (int k = 0; k < nsave; k++)
        x86_store_mem_reg(text, REG_RBP, save_base + k * 8, used_callee[k], 8);
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
                        x86_load_reg_mem(text, g_loc[i], REG_RBP, incoming,
                                         ty_size(pt));
                        pmoved[i] = 1;
                    } else {
                        x86_load_reg_mem(text, REG_RAX, REG_RBP, incoming, 8);
                        x86_store_slot(text, sd[i], 8);
                    }
                    incoming += 8;
                } else if (ty_is_float(pt)) {
                    x86_movs_store(text, slot, sd[i], ty_size(pt));
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
                        x86_load_reg_mem(text, g_loc[i], REG_RBP, incoming,
                                         ty_size(pt));
                        pmoved[i] = 1;
                    } else {
                        x86_load_reg_mem(text, REG_RAX, REG_RBP, incoming, 8);
                        x86_store_slot(text, sd[i], 8);
                    }
                    incoming += 8;
                } else if (ty_is_float(pt)) {
                    x86_movs_store(text, freg++, sd[i], ty_size(pt));
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

    rc_nvars = fn->nvars;
    cg_reset();
    /* Use counts drive comparison/branch fusion below (a compare feeding only
     * the next branch). Built once; freed after the loop. */
    int *usecnt = fn->nvregs
        ? xmalloc((size_t)fn->nvregs * sizeof *usecnt) : (int *)0;
    if (usecnt) count_vreg_uses(fn, usecnt);
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
        vrc_vreg = -1;
        vw_src = -1;
        int ins_start = text->len;
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
            x86_vload_base(text, 0, base, 0);
            rc_vreg = -1;
            if (!vec_keep(fn, n, usecnt, i->dst))
                x86_vstore_slot(text, sd[i->dst], 0);
            vrc_vreg = i->dst;   /* still in xmm0, stored or not */
            break;
        }
        case IR_VSTORE: {
            vw_src = vw_in;   /* xmm2/xmm3 untouched by this */
            int base;
            /* The ADDRESS first when the value is already in xmm0, so
             * loading it cannot be what evicts the value. */
            if (vrc_in != i->b)
                x86_vload_slot(text, 0, sd[i->b]);
            if (in_reg(i->a)) {
                base = g_loc[i->a];
            } else {
                cg_load(text, sd, i->a, 8, 0, 8);
                base = REG_RAX;
            }
            x86_vstore_base(text, base, 0, 0);
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
                    x86_vbin_rr(text, R, 0, (int)i->imm, i->size);
                else
                    x86_vbin_slot(text, R, (int)i->imm, i->size, sd[i->b]);
                break;
            }
            if (vacc_of(i->a) >= 0)
                x86_vmov_rr(text, 0, vacc_of(i->a));
            else if (vrc_in != i->a)
                x86_vload_slot(text, 0, sd[i->a]);
            if (i->imm == '<' || i->imm == '>')
                x86_vshift_imm(text, 0, i->imm == '<', i->sign, i->size,
                               i->c);
            else if (vacc_of(i->b) >= 0)
                x86_vbin_rr(text, 0, vacc_of(i->b), (int)i->imm, i->size);
            else
                x86_vbin_slot(text, 0, (int)i->imm, i->size, sd[i->b]);
            if (!vec_keep(fn, n, usecnt, i->dst))
                x86_vstore_slot(text, sd[i->dst], 0);
            vrc_vreg = i->dst;
            break;
        case IR_VSPLAT:
            vw_src = vw_in;   /* xmm2/xmm3 untouched by this */
            cg_reset();
            cg_load(text, sd, i->a, i->size, 0, i->size == 8 ? 8 : 4);
            x86_vmov_xmm_reg(text, 0, REG_RAX, i->size == 8 ? 8 : 4);
            /* 0x00 repeats lane 0 across all four; 0x44 repeats the low
             * QUADword, which is the same thing at eight bytes a lane. */
            x86_vshufd(text, 0, 0, i->size == 8 ? 0x44 : 0x00);
            rc_vreg = -1;
            if (vacc_of(i->dst) >= 0) {
                x86_vmov_rr(text, vacc_of(i->dst), 0);
            } else {
                if (!vec_keep(fn, n, usecnt, i->dst))
                    x86_vstore_slot(text, sd[i->dst], 0);
                vrc_vreg = i->dst;
            }
            break;
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
                    x86_vmov_rr(text, VW_SRC, 0);
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
            x86_vmov_rr(text, 0, VW_SRC);
            x86_vunpck(text, 0, VW_SIGN, i->c, i->size);
            vw_src = i->a;
            rc_vreg = -1;
            if (!vec_keep(fn, n, usecnt, i->dst))
                x86_vstore_slot(text, sd[i->dst], 0);
            vrc_vreg = i->dst;
            break;
        case IR_VREDADD:
            vw_src = vw_in;
            /* Fold the vector against a permuted copy of itself, halving
             * the live lanes each time, until lane 0 holds the sum. */
            cg_reset();
            if (vacc_of(i->a) >= 0)
                x86_vmov_rr(text, 0, vacc_of(i->a));
            else if (vrc_in != i->a)
                x86_vload_slot(text, 0, sd[i->a]);
            x86_vshufd(text, 1, 0, 0x4e);          /* swap the 64-bit halves */
            x86_vbin_rr(text, 0, 1, '+', i->size);
            if (i->size == 4) {
                x86_vshufd(text, 1, 0, 0xb1);      /* and the 32-bit pairs */
                x86_vbin_rr(text, 0, 1, '+', 4);
            }
            x86_vmov_reg_xmm(text, REG_RAX, 0, i->size == 8 ? 8 : 4);
            cg_store(text, sd, i->dst, i->w);
            break;
        case IR_CONST:
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
                cg_reset();
                x86_movs_load(text, 0, sd[i->a], i->w);
                x86_sse_alu_mem(text,
                                i->op == IR_ADD ? '+' :
                                i->op == IR_SUB ? '-' : '*',
                                sd[i->b], i->w);
                x86_movs_store(text, 0, sd[i->dst], i->w);
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
                /* a 16-byte (long double) access is gen_x87's, never fused */
                if (x87_ins(nx)) {
                    /* fall through to materialise the address */
                } else if (nx->op == IR_LOAD && nx->a == i->dst) {
                    if (i->imm_b)
                        x86_load_basedisp_rax(text, base, (int)i->imm,
                                              nx->size, nx->sign, nx->w);
                    else
                        x86_load_baseindex_rax(text, base, index, 1,
                                               nx->size, nx->sign, nx->w);
                    cg_store(text, sd, nx->dst, nx->w);
                    n++;                           /* consume the fused load */
                    break;
                } else if (nx->op == IR_STORE && nx->a == i->dst) {
                    /* value -> rax (rax never aliases base/index), then store
                     * through the folded address. */
                    cg_load(text, sd, nx->b, 8, 0, 8);
                    if (i->imm_b)
                        x86_store_basedisp_rax(text, base, (int)i->imm, nx->size);
                    else
                        x86_store_baseindex_rax(text, base, index, 1, nx->size);
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
            x86_sse_alu_mem(text, 'q', sd[i->a], i->w);
            x86_movs_store(text, 0, sd[i->dst], i->w);
            break;
        case IR_DIV:
        case IR_MOD:
            if (i->flt) { /* only DIV is ever float; MOD is integers */
                cg_reset();
                x86_movs_load(text, 0, sd[i->a], i->w);
                x86_sse_alu_mem(text, '/', sd[i->b], i->w);
                x86_movs_store(text, 0, sd[i->dst], i->w);
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
                x86_movs_load(text, 0, sd[swap ? i->b : i->a], i->w);
                x86_ucomis_mem(text, sd[swap ? i->a : i->b], i->w);
                if (i->pred == B_EQ || i->pred == B_NE)
                    x86_set_float_eq(text, i->pred == B_NE);
                else
                    x86_setcc_eax(text,
                                  cc_for(i->pred == B_LT ? B_GT :
                                         i->pred == B_LE ? B_GE : i->pred,
                                         0));
                cg_store(text, sd, i->dst, 4);   /* the 0/1 result is an int */
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
                int patch = x86_jcc_rel32(text, cc_for(jp, i->sign));
                cg_reset();                       /* control splits here */
                if (nbrs == capbrs) {
                    capbrs = capbrs ? capbrs * 2 : 16;
                    brs = xrealloc(brs, (size_t)capbrs * sizeof *brs);
                }
                brs[nbrs].patch_off = patch;
                brs[nbrs].label = br->label;
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
        case IR_I2F:
            cg_reset();
            x86_cvtsi2s(text, sd[i->a], i->size, i->w);
            x86_movs_store(text, 0, sd[i->dst], i->w);
            break;
        case IR_F2I:
            cg_reset();
            x86_cvtts2si(text, sd[i->a], i->size, i->w);
            cg_store(text, sd, i->dst, i->w);
            break;
        case IR_F2F:
            cg_reset();
            x86_cvts2s(text, sd[i->a], i->size);
            x86_movs_store(text, 0, sd[i->dst], i->w);
            break;
        case IR_LDVAR:
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
            /* Register-resident dest: load straight into it, no result-carrying
             * `mov %rax,%rN`. The address is either already in a register (RAX
             * wholly untouched — cache preserved) or staged into RAX as the base
             * (RAX still holds that address afterward, so its cache entry stays
             * valid — the load reads [rax], it does not overwrite rax). */
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
        case IR_STORE:
            /* Address already in a register (mirrors IR_LOAD): store straight to
             * [reg], skipping the slot->rcx load — which is what lets the address
             * temp be register-allocated at all (its OPAQUE marking is dropped). */
            if (in_reg(i->a)) {
                cg_load(text, sd, i->b, 8, 0, 8);           /* the value -> rax */
                x86_store_mem_reg(text, g_loc[i->a], 0, REG_RAX, i->size);
                break;
            }
            x86_mov_rcx_slot(text, sd[i->a]);       /* the address -> rcx */
            cg_load(text, sd, i->b, 8, 0, 8);       /* the value -> rax */
            x86_store_mem_rcx(text, i->size);
            break;
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
            if (i->op == IR_JMP) {
                patch = x86_jmp_rel32(text);
            } else {
                cg_load(text, sd, i->a, i->w, 0, i->w);
                x86_test_eax(text, i->w);
                patch = i->op == IR_BRZ ? x86_jz_rel32(text)
                                        : x86_jnz_rel32(text);
            }
            cg_reset();      /* control splits: don't carry RAX across */
            if (nbrs == capbrs) {
                capbrs = capbrs ? capbrs * 2 : 16;
                brs = xrealloc(brs, (size_t)capbrs * sizeof *brs);
            }
            brs[nbrs].patch_off = patch;
            brs[nbrs].label = i->label;
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

            /* MEMORY-class aggregates go to the outgoing area first,
             * while rax/rcx/rdx are still free to copy with. */
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
                x86_load_slot(text, sd[a->vreg], 8, 0, 8);
                x86_mov_reg_reg(text, REG_RDX, REG_RAX); /* src */
                int sz = a->size;
                for (int off = 0; off < sz; ) {
                    int chunk = sz - off;
                    chunk = chunk >= 8 ? 8 : chunk >= 4 ? 4
                          : chunk >= 2 ? 2 : 1;
                    x86_load_reg_mem(text, REG_RAX, REG_RDX, off, chunk);
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
            if (i->retsize && i->retnclass == 0 && !i->ret_x87) {
                x86_lea_reg_slot(text, x86_argreg(0),
                                 scratch_base + i->scratch);
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
            emit_reg_parallel_move(text, mvdest, mvsrc, nmv, REG_RAX);
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
                        x86_movs_load(text, ireg, sd[a->vreg], a->size);
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
                } else if (a->cls[0] == CLASS_SSE)
                    x86_movs_load(text, freg++, sd[a->vreg], a->size);
                else if (in_reg(a->vreg))
                    ireg++;              /* already placed by the parallel move */
                else
                    x86_load_arg(text, ireg++, sd[a->vreg]);
            }
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
                for (int k = 0; k < nsave; k++)
                    x86_load_reg_mem(text, used_callee[k], REG_RBP,
                                     save_base + k * 8, 8);
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
                x86_movs_store(text, 0, sd[i->dst], i->w);   /* stays reset */
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
                    x86_movs_load(text, ia->in[k].reg - 16,
                                  sd[ia->in[k].temp], ia->in[k].size);
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
                x86_movs_load(text, 0, sd[i->a], i->w);
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
                for (int k = 0; k < nsave; k++)         /* -O2: restore callee regs */
                    x86_load_reg_mem(text, used_callee[k], REG_RBP,
                                     save_base + k * 8, 8);
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
        for (int k = 0; k < nsave; k++)                 /* -O2: restore callee regs */
            x86_load_reg_mem(text, used_callee[k], REG_RBP, save_base + k * 8, 8);
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
        int from = brs[n].patch_off + 4;
        code_patch32(text, brs[n].patch_off,
                     (unsigned long)(unsigned int)(target - from));
    }
    free(brs);
    free(label_off);
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

    for (int n = 0; n < iu->nfuncs; n++)
        gen_func(&iu->funcs[n], text, &st);

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
