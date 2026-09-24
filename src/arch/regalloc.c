/* Register allocation: see regalloc.h for why this is shared.
 *
 * Moved here from src/arch/x86_64/codegen.c with no behavioural change.
 * The proof of that is `tools/x86-identity.sh`, which compiles a corpus
 * with the compiler before the move and after it and requires the
 * OBJECTS to be byte-identical -- a stronger statement than "the tests
 * still pass", because it says nothing changed at all.
 *
 * What the move did change is the interface: the pool of registers, the
 * callee-saved predicate and the narrow-load question now arrive in a
 * `struct ra_target` instead of being file-scope constants of one
 * backend.
 */
#include "regalloc.h"

#include <stdlib.h>
#include <string.h>

#include "../driver/util.h"
#include "../driver/remark.h"

/* The vreg WRITTEN by an instruction (its def), or -1. Kept in lockstep with
 * what codegen actually stores (cg_store / the STVAR store). Each temp is a
 * single-def SSA value; a local may be redefined, which liveness handles. */
int ra_ins_def(const struct ir_ins *in)
{
    switch (in->op) {
    case IR_CONST: case IR_MOV: case IR_ADD: case IR_SUB: case IR_MUL:
    case IR_DIV: case IR_MOD: case IR_AND: case IR_OR: case IR_XOR:
    case IR_NEG: case IR_BNOT: case IR_CMP: case IR_LDVAR: case IR_ADDR:
    case IR_STRADDR: case IR_GADDR: case IR_FADDR: case IR_LOAD: case IR_EXT:
    case IR_I2F: case IR_F2I: case IR_F2F: case IR_BSWAP: case IR_SQRT:
    case IR_SHL:
    case IR_SHR: case IR_XCHG: case IR_XADD: case IR_CMPXCHG:
    case IR_ARMW: case IR_CAS: case IR_CAS16: case IR_FRAMEADDR:
    case IR_ALLOCA: case IR_SPSAVE:
    case IR_STVAR:            /* the local written */
    case IR_CALL:             /* always stores a (possibly-unused) result temp */
    case IR_LABELADDR:        /* dst = &&label */
    /* A vector result is a 16-byte value in a slot, never a register the
     * integer allocator hands out -- but it IS a definition, and
     * liveness has to know that or the temp it overwrites looks live
     * across it. */
    case IR_VLOAD: case IR_VBIN: case IR_VSPLAT: case IR_VREDADD:
    case IR_VWIDEN: case IR_SELECT:
        return in->dst;
    default:
        return -1;            /* STORE, RET, LABEL, JMP, branches, MEMCPY, ... */
    }
}

/* Backward liveness dataflow. Fills first[v]/last[v] with the min/max
 * instruction index at which vreg v is live — a sound over-approximation of its
 * live range that spans loop back-edges (a naive first/last-appearance interval
 * does NOT, and would let a loop-carried value's register be clobbered mid-loop).
 * -1 for a vreg that is never live. ALSO returns, for the interference graph,
 * the per-instruction live-IN and live-OUT bitsets (each nins*words) and the def
 * vreg per instruction (all malloc'd, caller frees), and *words_out. Returns
 * NULL bitsets (and leaves the outputs NULL) for an empty function. */
unsigned long *ra_live_intervals(struct ir_func *fn, int *first,
                                             int *last, unsigned long **livein_out,
                                             int **defv_out, int *words_out)
{
    int nins = fn->nins, nvr = fn->nvregs;
    for (int v = 0; v < nvr; v++) { first[v] = -1; last[v] = -1; }
    *defv_out = NULL; *livein_out = NULL; *words_out = 0;
    if (nins == 0 || nvr == 0) return NULL;
    int words = (nvr + 63) / 64;

    unsigned long *use = xcalloc((size_t)nins * words, sizeof *use);
    unsigned long *in  = xcalloc((size_t)nins * words, sizeof *in);
    unsigned long *out = xcalloc((size_t)nins * words, sizeof *out);
    int *defv = xmalloc((size_t)nins * sizeof *defv);
    int *labelidx = xmalloc((size_t)(fn->nlabels ? fn->nlabels : 1) *
                            sizeof *labelidx);
    for (int l = 0; l < fn->nlabels; l++) labelidx[l] = -1;
    for (int i = 0; i < nins; i++)
        if (fn->ins[i].op == IR_LABEL) labelidx[fn->ins[i].label] = i;

#define USE(v) do { int _v = (v); if (_v >= 0 && _v < nvr)                   \
                        use[(size_t)i * words + (_v >> 6)] |= 1UL << (_v & 63); \
                  } while (0)
    for (int i = 0; i < nins; i++) {
        struct ir_ins *s = &fn->ins[i];
        defv[i] = ra_ins_def(s);
        switch (s->op) {
        case IR_MOV: case IR_NEG: case IR_BNOT: case IR_EXT: case IR_BSWAP:
        case IR_SQRT:
        case IR_I2F: case IR_F2I: case IR_F2F: case IR_LOAD: case IR_LDVAR:
        case IR_ADDR:
        case IR_VLOAD:            /* the ADDRESS read, an integer temp */
        case IR_VSPLAT: case IR_VREDADD: case IR_VWIDEN:
            USE(s->a); break;
        case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
        case IR_AND: case IR_OR: case IR_XOR: case IR_SHL: case IR_SHR:
        case IR_CMP: case IR_STORE: case IR_MEMCPY: case IR_MEMZERO:
        case IR_XCHG: case IR_XADD: case IR_ARMW:
        case IR_VSTORE: case IR_VBIN:
            USE(s->a); USE(s->b); break;
        case IR_CMPXCHG: case IR_CAS: case IR_CAS16: case IR_SELECT:
            USE(s->a); USE(s->b); USE(s->c); break;
        case IR_STVAR: case IR_VA_START:
        case IR_ALLOCA: case IR_SPRESTORE:
            USE(s->a); break;
        case IR_RET: case IR_BRZ: case IR_BRNZ:
        case IR_IGOTO:            /* `goto *p` reads p */
            USE(s->a); break;
        case IR_CALL:
            if (s->indirect) USE(s->a);
            for (int k = 0; k < s->nargs; k++) USE(s->argv[k].vreg);
            break;
        case IR_ASM:
            if (s->asm_ir) {
                for (int k = 0; k < s->asm_ir->nin; k++) USE(s->asm_ir->in[k].temp);
                for (int k = 0; k < s->asm_ir->nout; k++) USE(s->asm_ir->out[k].temp);
            }
            break;
        default: break;   /* CONST/STRADDR/GADDR/FADDR/LABEL/JMP/FENCE/UD2 */
        }
    }
#undef USE

    /* iterate to a fixpoint: in[i] = use[i] ∪ (out[i] − def[i]);
     * out[i] = ∪ in[succ]. */
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int i = nins - 1; i >= 0; i--) {
            struct ir_ins *s = &fn->ins[i];
            unsigned long *oi = out + (size_t)i * words;
            for (int w = 0; w < words; w++) oi[w] = 0;
            /* successors */
            if (s->op != IR_JMP && s->op != IR_RET && s->op != IR_UD2 &&
                i + 1 < nins) {
                unsigned long *si = in + (size_t)(i + 1) * words;
                for (int w = 0; w < words; w++) oi[w] |= si[w];
            }
            if (s->op == IR_JMP || s->op == IR_BRZ || s->op == IR_BRNZ) {
                int t = labelidx[s->label];
                if (t >= 0) {
                    unsigned long *si = in + (size_t)t * words;
                    for (int w = 0; w < words; w++) oi[w] |= si[w];
                }
            }
            /* in = use ∪ (out − def) */
            unsigned long *ii = in + (size_t)i * words;
            unsigned long *ui = use + (size_t)i * words;
            int dv = defv[i];
            for (int w = 0; w < words; w++) {
                unsigned long nv = oi[w];
                if (dv >= 0 && (dv >> 6) == w) nv &= ~(1UL << (dv & 63));
                nv |= ui[w];
                if (nv != ii[w]) { ii[w] = nv; changed = 1; }
            }
        }
    }

    /* occupied(v,i) = v ∈ in[i] ∪ out[i] ∪ {def[i]} -> update first/last */
    for (int i = 0; i < nins; i++) {
        unsigned long *ii = in + (size_t)i * words;
        unsigned long *oi = out + (size_t)i * words;
        for (int w = 0; w < words; w++) {
            unsigned long bits = ii[w] | oi[w];
            while (bits) {
                int b = 0; unsigned long t = bits;
                while (!(t & 1)) { t >>= 1; b++; }
                int v = w * 64 + b;
                if (first[v] < 0) first[v] = i;
                last[v] = i;
                bits &= bits - 1;
            }
        }
        int dv = defv[i];
        if (dv >= 0) {
            if (first[dv] < 0) first[dv] = i;
            if (i > last[dv]) last[dv] = i;
        }
    }

    free(use); free(labelidx);
    *livein_out = in;
    *defv_out = defv;
    *words_out = words;
    return out;
}

/* mark vreg v ineligible (used at an opaque site) */
#define OPAQUE(v) do { int _v = (v); if (_v >= 0 && _v < nvr) elig[_v] = 0; } while (0)

int *ra_allocate(struct ir_func *fn, const struct ra_target *t,
                 const char *g_wide, int *used_out, int *nused_out)
{
    int nins = fn->nins, nvr = fn->nvregs;
    int nvars = fn->nvars;
    int *loc = xmalloc((size_t)(nvr ? nvr : 1) * sizeof *loc);
    for (int v = 0; v < nvr; v++) loc[v] = -1;
    *nused_out = 0;
    if (nvr == 0) return loc;

    /* Non-variadic functions get the full nine-register pool; caller-saved
     * registers in it are then masked per value by `crosses`/`is_arg` below (a
     * leaf, having no calls, is never masked). A variadic function reserves the
     * argument register file, so it drops r8/r9. */
    int variadic = fn->is_varargs;
    const int *POOL = variadic && t->pool_varargs ? t->pool_varargs
                                                  : t->pool;
    int NP = variadic && t->pool_varargs ? t->npool_varargs
                                          : t->npool;

    int *first = xmalloc((size_t)nvr * sizeof *first);
    int *last  = xmalloc((size_t)nvr * sizeof *last);
    char *elig = xmalloc((size_t)nvr);
    /* live ranges from real dataflow (spans loops); appearance intervals would
     * be unsound across a back-edge. `liveout`/`defv` drive the interference
     * graph below. */
    int *defv = NULL, lwords = 0;
    unsigned long *livein = NULL;
    unsigned long *liveout = ra_live_intervals(fn, first, last,
                                                    &livein, &defv, &lwords);

    /* A value LIVE-OUT of a call survives it, so it cannot sit in a caller-saved
     * register (the call clobbers all of them) — it takes a callee-saved reg or
     * memory. The call's own result (defv[i]) is born at the call, so it does
     * not cross THIS one. A value that is merely a call ARGUMENT may still use
     * r8/r9: the parallel move in case IR_CALL shuffles arguments already held
     * in r8/r9 without clobbering. A leaf has no calls, so `crosses` stays 0. */
    char *crosses = xcalloc((size_t)(nvr ? nvr : 1), 1);
    for (int i = 0; i < nins; i++) {
        /* ...and a call the IR does not spell IR_CALL: a backend that
         * lowers some op to a runtime helper says so here, or its
         * caller-saved registers are not safe (regalloc.h). */
        if (fn->ins[i].op != IR_CALL &&
            !(t->op_calls_helper && t->op_calls_helper(&fn->ins[i])))
            continue;
        unsigned long *lo = liveout + (size_t)i * lwords;
        for (int w = 0; w < lwords; w++) {
            unsigned long bits = lo[w];
            while (bits) {
                int b = 0; unsigned long t = bits;
                while (!(t & 1)) { t >>= 1; b++; }
                int v = w * 64 + b;
                if (v < nvr && v != defv[i]) crosses[v] = 1;
                bits &= bits - 1;
            }
        }
    }
    for (int v = 0; v < nvr; v++) {
        if (v >= nvars) {
            elig[v] = 1;                          /* a temp */
        } else {
            const struct ir_local *L = &fn->locals[v]; /* param or local */
            int sz = L->size;
            /* any scalar int/pointer that fits a GPR — char/short included: a
             * narrow write keeps the low bytes, a read movsx/movzx-extends. */
            elig[v] = L->is_int_or_ptr &&
                      (sz == 1 || sz == 2 || sz == 4 || sz == 8);
        }
    }

    /* a long double lives in its 16-byte slot, never a register */
    for (int v = 0; v < nvr; v++)
        if (g_wide && g_wide[v])
            elig[v] = 0;

    for (int i = 0; i < nins; i++) {
        struct ir_ins *in = &fn->ins[i];
        int is_float = in->flt || in->op == IR_I2F || in->op == IR_F2I ||
                       in->op == IR_F2F;
        if (is_float) { OPAQUE(in->dst); OPAQUE(in->a); OPAQUE(in->b); }
        switch (in->op) {
        case IR_ADDR:      OPAQUE(in->a); break;          /* address-taken */
        /* IR_STORE's address is register-aware now (codegen stores to [reg]),
         * so it is NOT opaque — only its raw value path was. */
        case IR_VA_START:  OPAQUE(in->a); break;
        case IR_XCHG: case IR_XADD: case IR_ARMW:
            OPAQUE(in->a); OPAQUE(in->b); break;          /* raw addr/val slots */
        case IR_CMPXCHG: case IR_CAS: case IR_CAS16:
            OPAQUE(in->a); OPAQUE(in->b); OPAQUE(in->c); break;
        case IR_FRAMEADDR:
            OPAQUE(in->dst); break;                        /* a raw-slot result */
        case IR_MEMCPY: case IR_MEMZERO:
            /* The operands are ADDRESSES, used as the copy base. A
             * backend that takes them from a register leaves nothing
             * opaque here; one that reads the slot needs them there. */
            if (!t->memcpy_addr_in_reg) { OPAQUE(in->a); OPAQUE(in->b); }
            break;
        case IR_RET:
            /* A struct or float return reads its slot raw on every
             * backend; a scalar one only where the backend can take it
             * from a register. */
            if (fn->ret_abi.is_struct || in->flt || !t->ret_scalar_in_reg)
                OPAQUE(in->a);
            break;
        case IR_CALL:
            /* A struct or float (SSE) argument loads its slot raw
             * everywhere. A scalar-integer one is moved straight into
             * its argument register only by a backend that knows how. */
            for (int k = 0; k < in->nargs; k++)
                if (in->argv[k].is_struct ||
                    in->argv[k].cls[0] == CLASS_SSE ||
                    !t->call_int_arg_in_reg)
                    OPAQUE(in->argv[k].vreg);
            if (in->flt || in->retsize) OPAQUE(in->dst);  /* float/struct ret */
            break;
        case IR_ASM:
            if (in->asm_ir) {
                for (int k = 0; k < in->asm_ir->nin; k++)
                    OPAQUE(in->asm_ir->in[k].temp);
                for (int k = 0; k < in->asm_ir->nout; k++)
                    OPAQUE(in->asm_ir->out[k].temp);
            }
            break;
        default: break;
        }
    }

    /* A vreg live across inline asm cannot sit in a callee reg the asm might
     * clobber (the clobber set is not visible here), so exclude it. And a vreg
     * that never appears has nothing to allocate. */
    for (int i = 0; i < nins; i++)
        if (fn->ins[i].op == IR_ASM)
            for (int v = 0; v < nvr; v++)
                if (elig[v] && first[v] >= 0 && first[v] <= i && i <= last[v])
                    elig[v] = 0;
    for (int v = 0; v < nvr; v++)
        if (first[v] < 0) elig[v] = 0;

    /* Number the eligible vregs 0..E-1 and build the PRECISE interference graph:
     * at each instruction the vregs in live-out(i) ∪ {def(i)} are simultaneously
     * live and so interfere pairwise. This is tighter than interval overlap —
     * two vregs whose ranges overlap but are never live at the same point don't
     * interfere, and a result may reuse a dying operand's register. */
    int *eof = xmalloc((size_t)nvr * sizeof *eof);   /* vreg -> eligible index */
    int E = 0;
    for (int v = 0; v < nvr; v++) eof[v] = elig[v] ? E++ : -1;
    int *eidx = xmalloc((size_t)(E ? E : 1) * sizeof *eidx);
    for (int v = 0; v < nvr; v++) if (eof[v] >= 0) eidx[eof[v]] = v;

    int ew = (E + 63) / 64;
    unsigned long *adj = E ? xcalloc((size_t)E * ew, sizeof *adj) : NULL;
    int *members = xmalloc((size_t)(E ? E : 1) * sizeof *members);
    /* Vregs simultaneously live interfere. Precisely: those live at an
     * instruction's ENTRY (live_in) are mutually live, and those live at its
     * EXIT (live_out, plus a dead def that still clobbers a register) are
     * mutually live — but a value dying at i (live_in only) and one born at i
     * (the def, live_out only) are NOT simultaneously live, so no edge crosses
     * the two groups. That precision is what lets a copy's source (which dies)
     * share a register with its result (move coalescing, below). */
    for (int pass = 0; adj && pass < 2; pass++) {
        for (int i = 0; i < nins; i++) {
            unsigned long *grp = (pass == 0 ? livein : liveout)
                                 + (size_t)i * lwords;
            int m = 0;
            for (int w = 0; w < lwords; w++) {
                unsigned long bits = grp[w];
                while (bits) {
                    int b = 0; unsigned long t = bits;
                    while (!(t & 1)) { t >>= 1; b++; }
                    int v = w * 64 + b;
                    if (v < nvr && eof[v] >= 0) members[m++] = eof[v];
                    bits &= bits - 1;
                }
            }
            if (pass == 1) {   /* a dead def joins the live-out group */
                int dv = defv[i];
                if (dv >= 0 && dv < nvr && eof[dv] >= 0) {
                    int had = 0;
                    for (int j = 0; j < m; j++) if (members[j] == eof[dv]) had = 1;
                    if (!had) members[m++] = eof[dv];
                }
            }
            for (int p = 0; p < m; p++)
                for (int q = p + 1; q < m; q++) {
                    int a = members[p], b = members[q];
                    adj[(size_t)a * ew + (b >> 6)] |= 1UL << (b & 63);
                    adj[(size_t)b * ew + (a >> 6)] |= 1UL << (a & 63);
                }
        }
    }

    /* Move-preference (coalescing) graph: two vregs that share a register let
     * codegen drop a move, so record a preference edge between them when they do
     * NOT interfere; colouring then biases each toward a move-partner's colour.
     * Two sources of a preferred pair:
     *  - a plain copy `dst = a` (IR_MOV / IR_STVAR / non-extending IR_LDVAR):
     *    same register makes the copy a self-move that vanishes.
     *  - a two-address binop `dst = a OP b`: codegen emits `OP b,dst` with no
     *    setup move when dst holds operand a (`dst == a`), or the commuted
     *    `OP a,dst` when dst holds b and OP commutes. So dst prefers a, and for a
     *    commutative op with a register b, dst prefers b too. SUB/SHL/SHR do not
     *    commute (dst == b would force the RAX fallback), so only a is preferred.
     * Each edge is a BIAS, not a constraint, and is dropped when the pair
     * interferes — which is exactly when operand a is still live after the op, so
     * a genuinely reusable (dying) operand is the only one ever coalesced. */
    unsigned long *pref = E ? xcalloc((size_t)E * ew, sizeof *pref) : NULL;
    for (int i = 0; pref && i < nins; i++) {
        struct ir_ins *in = &fn->ins[i];
        enum ir_op op = in->op;
        int d = in->dst, partner[2], np = 0;
        if (op == IR_MOV || op == IR_STVAR ||
            (op == IR_LDVAR && t->ldvar_plain(in->size, in->sign, in->w))) {
            partner[np++] = in->a;
        } else if (op == IR_ADD || op == IR_SUB || op == IR_MUL ||
                   op == IR_AND || op == IR_OR || op == IR_XOR ||
                   op == IR_SHL || op == IR_SHR) {
            partner[np++] = in->a;                       /* dst prefers a */
            int commut = op == IR_ADD || op == IR_MUL || op == IR_AND ||
                         op == IR_OR || op == IR_XOR;
            if (commut && !in->imm_b) partner[np++] = in->b;
        }
        for (int k = 0; k < np; k++) {
            int a = partner[k];
            if (d < 0 || d >= nvr || a < 0 || a >= nvr) continue;
            int ed = eof[d], ea = eof[a];
            if (ed < 0 || ea < 0 || ed == ea) continue;
            if (adj[(size_t)ed * ew + (ea >> 6)] & (1UL << (ea & 63))) continue;
            pref[(size_t)ed * ew + (ea >> 6)] |= 1UL << (ea & 63);
            pref[(size_t)ea * ew + (ed >> 6)] |= 1UL << (ed & 63);
        }
    }

    /* Chaitin-Briggs simplify order. Repeatedly remove a node of degree < NCALLEE
     * (trivially colourable) onto a stack; when none remains, remove the highest-
     * degree node as an OPTIMISTIC spill candidate. Colouring then pops the stack
     * (below) — a spill candidate popped early may still find a free colour, so
     * fewer values actually spill than a fixed first-appearance order gives.
     * Deterministic: ties broken by the lowest eligible index. */
    int *order = xmalloc((size_t)(E ? E : 1) * sizeof *order);
    {
        int *deg = xmalloc((size_t)(E ? E : 1) * sizeof *deg);
        for (int e = 0; e < E; e++) {
            int d = 0;
            unsigned long *row = adj + (size_t)e * ew;
            for (int w = 0; w < ew; w++) {
                unsigned long b = row[w];
                while (b) { d++; b &= b - 1; }
            }
            deg[e] = d;
        }
        char *gone = xcalloc((size_t)(E ? E : 1), 1);
        int sp = 0;
        for (int cnt = 0; cnt < E; cnt++) {
            int pick = -1;
            for (int e = 0; e < E; e++)          /* a trivially-colourable node */
                if (!gone[e] && deg[e] < NP) { pick = e; break; }
            if (pick < 0)                        /* else the most-constrained one */
                for (int e = 0; e < E; e++)
                    if (!gone[e] && (pick < 0 || deg[e] > deg[pick])) pick = e;
            gone[pick] = 1;
            order[sp++] = pick;                  /* push */
            unsigned long *row = adj + (size_t)pick * ew;
            for (int w = 0; w < ew; w++) {
                unsigned long b = row[w];
                while (b) {
                    int bit = 0; unsigned long t = b;
                    while (!(t & 1)) { t >>= 1; bit++; }
                    int ne = w * 64 + bit;
                    if (!gone[ne]) deg[ne]--;
                    b &= b - 1;
                }
            }
        }
        for (int i = 0; i < E / 2; i++) {        /* pop order = reverse of push */
            int t = order[i]; order[i] = order[E - 1 - i]; order[E - 1 - i] = t;
        }
        free(deg); free(gone);
    }

    int reg_used[RA_MAXPOOL];
    int nspill = 0;
    for (int k = 0; k < NP; k++) reg_used[k] = 0;
    for (int oi = 0; oi < E; oi++) {
        int e = order[oi];
        int taken = 0;                        /* bitmask of neighbour registers */
        unsigned long *row = adj + (size_t)e * ew;
        for (int w = 0; w < ew; w++) {
            unsigned long bits = row[w];
            while (bits) {
                int b = 0; unsigned long t = bits;
                while (!(t & 1)) { t >>= 1; b++; }
                int ne = w * 64 + b;
                int nl = loc[eidx[ne]];
                if (nl >= 0)
                    for (int k = 0; k < NP; k++)
                        if (POOL[k] == nl) taken |= 1 << k;
                bits &= bits - 1;
            }
        }
        /* A value that crosses a call may not take a caller-saved register: the
         * call clobbers them, so forbid them here (leaving callee-saved/spill). */
        if (crosses[eidx[e]])
            for (int k = 0; k < NP; k++)
                if (!t->is_callee_saved(POOL[k])) taken |= 1 << k;
        /* preferred colours: registers a colored, non-interfering move-partner
         * already holds (and that are still free) */
        int want = 0;
        unsigned long *prow = pref + (size_t)e * ew;
        for (int w = 0; w < ew; w++) {
            unsigned long bits = prow[w];
            while (bits) {
                int b = 0; unsigned long t = bits;
                while (!(t & 1)) { t >>= 1; b++; }
                int pe = w * 64 + b;
                int pl = loc[eidx[pe]];
                if (pl >= 0)
                    for (int k = 0; k < NP; k++)
                        if (POOL[k] == pl && !(taken & (1 << k)))
                            want |= 1 << k;
                bits &= bits - 1;
            }
        }
        int pick = -1;
        for (int k = 0; k < NP; k++)                  /* a free preferred reg */
            if ((want & (1 << k)) && !(taken & (1 << k))) { pick = k; break; }
        if (pick < 0)
            for (int k = 0; k < NP; k++)               /* else lowest free */
                if (!(taken & (1 << k))) { pick = k; break; }
        if (pick >= 0) { loc[eidx[e]] = POOL[pick]; reg_used[pick] = 1; }
        else nspill++;               /* no colour: this value lives in memory */
    }
    /* Why a value ended up in memory is the other question people ask of an
     * optimizer, and the answer is a property of the whole function -- how
     * many values were live at once against how many registers exist -- not
     * of any one value. So it is reported once, with both numbers. */
    if (nspill && remarks_on())
        remark_add("regalloc", "spilled-to-stack",
                   fn->src ? fn->name : NULL, "no-register-free",
                   fn->src ? fn->file : NULL,
                   fn->src ? fn->line : 0,
                   "%d of %d values did not get one of the %d allocatable "
                   "registers", nspill, E, NP);

    /* Inline asm can hard-code a callee-saved register the allocator never sees
     * — cpuid writes RBX (its "=b" output), and any operand fixed to rbx/r12..r15
     * loads or overwrites that register. Such a register is clobbered by this
     * function all the same, so the prologue must preserve it: mark it used.
     * (Without this a caller that keeps a live value in rbx across the call gets
     * it silently corrupted — invisible until an optimization puts one there.) */
    for (int n = 0; n < fn->nins; n++) {
        if (fn->ins[n].op != IR_ASM || !fn->ins[n].asm_ir)
            continue;
        struct ir_asm *a = fn->ins[n].asm_ir;
        for (int j = 0; j < a->nout; j++)
            for (int k = 0; k < NP; k++)
                if (POOL[k] == a->out[j].reg) reg_used[k] = 1;
        for (int j = 0; j < a->nin; j++)
            for (int k = 0; k < NP; k++)
                if (POOL[k] == a->in[j].reg) reg_used[k] = 1;
    }

    /* Only the callee-saved registers actually used need a prologue save. */
    int nu = 0;
    for (int k = 0; k < NP; k++)
        if (reg_used[k] && t->is_callee_saved(POOL[k])) used_out[nu++] = POOL[k];
    *nused_out = nu;

    free(first); free(last); free(elig); free(crosses);
    free(eof); free(eidx); free(adj); free(pref);
    free(members); free(order);
    free(liveout); free(livein); free(defv);
    return loc;
}

#undef OPAQUE

/* ==== temp-slot coalescing, shared (D-011) =================================
 *
 * Two temps whose live ranges do not overlap can share one frame slot.
 * The x86-64 backend has done this since it was written; aarch64 gave
 * every temp its own eight bytes, which on a function the optimizer has
 * inflated with SSA versions is a frame several times larger than it
 * needs to be -- and on a recursive kernel function, a stack overflow.
 *
 * D-011 said what justifies lifting: a real algorithm duplicated
 * between two WORKING backends. This is that, and the check is that the
 * x86 objects come out byte-identical after the move -- tools/
 * x86-identity.sh compares emitted bytes, so "nothing changed" is a
 * statement about the code and not about the tests.
 *
 * What a backend supplies is the three things it knows and this does
 * not: which vregs live in registers (those need no slot at all), and
 * the two policy questions below. */
/* Which locals the code still names (regalloc.h). */
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
int ra_slot_dead(const struct ir_func *fn, const int *loc, int v,
                 int want_debug)
{
    const struct func *f = fn->src;
    if (!loc || loc[v] < 0 || f->is_varargs || fn->has_alloca || want_debug)
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

char *ra_locals_referenced(const struct ir_func *fn, int want_debug)
{
    size_t n = (size_t)(fn->nvars > 0 ? fn->nvars : 1);
    char *r = xcalloc(n, 1);
    if (want_debug || fn->has_alloca || (fn->src && fn->src->is_varargs)) {
        memset(r, 1, n);
        return r;
    }
    for (int i = 0; i < fn->nparams && i < fn->nvars; i++)
        r[i] = 1;
    /* IR_LDVAR's and IR_ADDR's `a` and IR_STVAR's `dst` are the only
     * operands anywhere in EmbIR that name a frame slot. */
    for (int i = 0; i < fn->nins; i++) {
        const struct ir_ins *in = &fn->ins[i];
        int v = in->op == IR_ADDR || in->op == IR_LDVAR ? in->a
              : in->op == IR_STVAR                      ? in->dst : -1;
        if (v >= 0 && v < fn->nvars)
            r[v] = 1;
    }
    return r;
}

int *ra_coalesce_temps(struct ir_func *fn, int nvars,
                       const struct ra_slots *o, int *npool_out)
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
            if (o->loc && o->loc[k + nvars] >= 0) {
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
                slot[k] = o->opt_frames ? -1 : pool++;
                continue;
            }
            int coalescable = (blk[first[k]] == blk[last[k]]) && !o->has_cgoto;
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

