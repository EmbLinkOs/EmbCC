/* ARMv6-M (Cortex-M0, M0+, M1) code generation.
 *
 * ARMv6-M is Thumb-1: the 16-bit encodings plus BL, MRS, MSR, DMB, DSB
 * and ISB. Any other 32-bit instruction is UNDEFINED there, and so is an
 * unaligned word or halfword access. codegen.c's lowering is Thumb-2
 * throughout -- IT, CBZ, MOVW/MOVT, modified immediates, shifted operands,
 * LDRD, TBH, the divides -- so this file is a second instruction
 * selection, sharing with codegen.c everything that is not one: AAPCS32
 * (place_one), the frame layout, the site lists and the register
 * allocator (through cg.h). docs/internals/armv6m-plan.md is the design.
 *
 * Every instruction this file writes goes through a t1_* encoder or one
 * of the encoders emit.h lists as already ARMv6-M, and v6_scan() decodes
 * each finished function and refuses it, by name, if anything else got
 * in -- an inline asm template included.
 *
 * ---- ARMv8-M Baseline ----------------------------------------------------
 *
 * The Cortex-M23 runs this same selection (target_thumb_v8m_base), with
 * what Baseline adds to ARMv6-M turned on where it replaces a call: a
 * 32-bit divide is SDIV/UDIV (and a remainder the quotient times the
 * divisor taken from the dividend -- there is no MLS), and a one-, two- or
 * four-byte atomic is an LDREX/STREX loop between two DMBs, as on ARMv7-M,
 * instead of a call to lib/rt's interrupt-masking __atomic_* routines.
 * v6_scan then admits Baseline's 32-bit encodings as well -- the divides,
 * the exclusives and the acquire/release forms, MOVW/MOVT, B.W, CLREX, TT
 * and SG -- and CBZ/CBNZ; still no IT and no other Thumb-2 form. A
 * cmse_nonsecure_entry function and a cmse_nonsecure_call are lowered
 * here too (-mcmse; see gen_ret and gen_call). Constants and addresses
 * stay in literal pools, as clang keeps them for this core.
 *
 * ---- registers ---------------------------------------------------------
 *
 * Only r0-r7 compute. r6 and r7 are this lowering's two scratch registers
 * (S0, S1) -- callee-saved, so the prologue saves them when a pass shows
 * they were used, exactly as codegen.c saves r9-r11 -- and the allocator
 * gets r0-r5 (codegen.c's T6_POOL). r12 breaks parallel-move cycles and
 * holds an indirect call's target; MOV reaches it, nothing else needs to.
 * r5 is the frame base of a function with a variable-length array.
 *
 * When a lowering needs a third register it asks tmp_get(), which hands
 * out S1, S0, and then any of r0-r5 this instruction does not touch,
 * PUSHED for the duration and popped after -- sp moves, so every
 * sp-relative offset meanwhile adds F->spb.
 *
 * ---- flags -------------------------------------------------------------
 *
 * Every data-processing instruction on low registers sets the flags.
 * codegen.c's invariant still holds -- no flag value survives from one IR
 * instruction to the next -- so only a lowering that has compared within
 * itself must not clobber them. Everything that READS a value (v_rd, fr_ld,
 * fr_addr, mem loads, push and pop) is flag-free for that reason: they
 * build an offset with a literal, never with MOVS/ADDS.
 *
 * ---- constants and addresses -------------------------------------------
 *
 * There is no MOVW/MOVT. A constant is MOVS and a second instruction where
 * that works, and a word in a LITERAL POOL otherwise; every symbol address
 * is a pool word with an R_ARM_ABS32 site. LDR (literal) reaches 1020 bytes
 * FORWARD, so pools are dumped as islands inside long functions (see
 * pool_point), at a place the first pass of a layout chooses and later
 * passes keep.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cg.h"
#include "../target.h"
#include "../../driver/util.h"

#define IP 12
#define FB6 5              /* the frame base where sp moves (a VLA) */
/* The two scratch registers. r6 and r7 -- unless r6 and r7 are in the
 * allocator's pool as well (tcg_ext, one of gen_func_best's attempts):
 * then each instruction takes two registers that hold nothing there
 * (v6_roles), and -1 for one it could not find, which fails the attempt
 * the moment the lowering asks for it (v6_role). The prologue and the
 * epilogue, where nothing but the arguments and the result is live, keep
 * r6 and r7. */
static int g6_s0 = 6, g6_s1 = 7;
static int g6_role_n;           /* role registers named this instruction */
static int v6_role(int k)
{
    int r = k ? g6_s1 : g6_s0;
    g6_role_n++;
    if (r < 0) {
        tcg_role_fail();
        return k ? 7 : 6;          /* what it emits is thrown away */
    }
    return r;
}
#define S0 v6_role(0)
#define S1 v6_role(1)
/* The callee-saved low registers a scratch may be: r6/r7 with the fixed
 * pair; with the per-instruction roles, any of r4-r7 not otherwise saved
 * (the prologue then saves it, as it does r6/r7). */
#define SCR_BOTH ((1u << 6) | (1u << 7))
#define SCR_SET (tcg_ext() ? 0xf0u : SCR_BOTH)

/* The size classes of a branch to a label (fix.ins), smallest first. */
enum { BC_SHORT = 0, BC_MED = 1, BC_FAR = 2 };
/* Fix kinds beside codegen.c's T_TAB (99): a switch table's byte or
 * halfword entry, (target - base) / 2 where cz_at holds the base. */
#define T_TAB  99
#define V6_TBB 97
#define V6_TBH 96

/* Memory copies up to this many bytes are inline; longer ones call
 * __aeabi_memcpy / __aeabi_memclr (v6_op_calls_helper says which). */
#define V6_INLINE_COPY 8

static void v6_refuse(const struct ir_func *fn, const struct ir_ins *i,
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
            fn->file ? fn->file : "?", i ? i->line : fn->line,
            target_thumb_v8m_base() ? "ARMv8-M Baseline" : "ARMv6-M", what,
            fn->name, op);
    exit(1);
}

int v6_op_calls_helper(const struct ir_ins *i)
{
    switch (i->op) {
    case IR_DIV: case IR_MOD:
        /* ARMv8-M Baseline divides 32 bits in hardware */
        return !i->flt && (i->w == 8 || !target_thumb_v8m_base());
    case IR_MUL:
        return !i->flt && i->w == 8;
    case IR_SHL: case IR_SHR:
        return i->w == 8 && !i->imm_b;
    case IR_MEMCPY: case IR_MEMZERO:
        return i->size > V6_INLINE_COPY;
    case IR_XCHG: case IR_XADD: case IR_ARMW: case IR_CAS: case IR_CMPXCHG:
        /* ...and has the exclusives (an eight-byte one is refused) */
        return !target_thumb_v8m_base();
    default:
        return 0;
    }
}

/* ---- registers ---------------------------------------------------------- */

static int in_reg6(const struct t_fn *F, int v)
{
    return F->loc && v >= 0 && F->loc[v] >= 0;
}

/* A scratch register, recorded as used: the prologue saves r6/r7 only
 * when a pass has used them. */
static int sc(struct t_fn *F, int r)
{
    if (r >= 4 && r <= 7)           /* r6/r7, or a role among r4-r7 */
        F->v6_used |= 1u << r;
    return r;
}

static void mov(struct t_fn *F, int d, int s)
{
    if (d != s)
        t_mov_reg(F->t, d, s);
}

/* ---- the slot cache -------------------------------------------------------
 *
 * A value with no register lives in a frame slot and is loaded at every
 * read: `str r6, [sp, #16]` at the end of one instruction and `ldr r7,
 * [sp, #16]` at the start of the next, or the same slot loaded again two
 * instructions on. After a load or a store of a slot's word the register
 * holds that word, and it still does at a later read if no instruction
 * since has written the register and no store has written the slot: then
 * the read takes the register (v_rdr, nothing emitted) or copies it (v_rd,
 * a MOV for the LDR). Codegen.c's ls_end and reload cache do this for
 * ARMv7-M; this is the ARMv6-M one, where every scratch is a low register
 * already and the gain is the load itself.
 *
 * What holds is decided from the CODE, not from the lowering's account of
 * itself: an entry is (register, frame offset, where it began), and a
 * lookup decodes every instruction emitted since that point (v6_writes) --
 * a register any of them writes, or a call clobbers, ends the entry. So a
 * lowering that takes a scratch, a pushed temporary, a parallel move or an
 * argument register needs no bookkeeping here. The rest is control flow:
 *   - a label is entered from elsewhere: what holds there is what holds
 *     on every way in -- the fall-through and each jump to it, whose
 *     state is taken where the jump is emitted (rc_snap) -- and nothing
 *     at a label some jump reaches from below (a loop's head), from a
 *     table or by address (g6_lfwd);
 *   - an entry begins only where everything emitted for the instruction
 *     so far is straight-line, so it dominates what follows (a select's
 *     arm, or the code after a loop's exit test, never starts one);
 *   - a pool island, an asm, and a branch taken back (v6_invert_last)
 *     empty it too, which only costs a load.
 * And memory: only the slot of a value nothing takes the address of is
 * cached (v6_rc_ok) -- a store through a pointer, a call or a copy can
 * then not reach it -- and every store to the frame (fr_st) drops the
 * entries it overlaps. Temporaries share slots (ra_coalesce_temps), which
 * is why entries are by offset, not by value. */
static long g6_rc_off[8];       /* per low register: the word's frame offset,
                                 * or -1 */
static int g6_rc_at[8];         /* ...and where in the code it began */
static int g6_ins_at;           /* where the current instruction began */
static const char *g6_atk;      /* per vreg: its address is taken */
static const struct ir_ins *g6_cur;  /* the instruction being lowered */

static void rc_reset(void)
{
    for (int r = 0; r < 8; r++)
        g6_rc_off[r] = -1;
}

/* Per label: 1 when every jump to it comes before it and is a branch this
 * file emits through v6_branch -- no switch table, no &&label -- so the
 * cache at it can be what holds on every way in. With the state each
 * jump saw (g6_lsnap, 8 per label: the offset each register held, -1 for
 * none) and whether one has been seen this pass. */
static char *g6_lfwd, *g6_lseen;
static long *g6_lsnap;

/* EMBCC_V6_NOSLOTCACHE=1: no entry is ever made (tests, bisecting). */
static int rc_off(void)
{
    static int off = -1;
    if (off < 0)
        off = getenv("EMBCC_V6_NOSLOTCACHE") != NULL;
    return off;
}

/* The far branch BL at `at` (F->far_mode), as opposed to a call. */
static int v6_far_bl(const struct t_fn *F, int at)
{
    if (!F->far_mode)
        return 0;
    for (int k = 0; k < F->nfix; k++)
        if (F->fix[k].at == at)
            return 1;
    return 0;
}

/* The registers (bit r for rr) the instructions in [from, to) write --
 * a call's r0-r3, r12 and lr included, and all of them for one this does
 * not know -- and, in *br, whether any of them is a branch. */
static unsigned v6_writes(const struct t_fn *F, int from, int to, int *br)
{
    const struct code *t = F->t;
    const unsigned call = 0xfu | (1u << 12) | (1u << 14);
    unsigned m = 0;
    int at = from, b = 0;
    if (to > t->len)
        to = t->len;
    while (at + 1 < to) {
        unsigned h = (unsigned)(t->p[at] | t->p[at + 1] << 8);
        int data = 0;
        for (int k = 0; k + 1 < t->ndrange; k += 2)
            if (at >= t->drange[k] && at < t->drange[k + 1]) {
                at = t->drange[k + 1];
                data = 1;
                break;
            }
        if (data)
            continue;
        if ((h >> 11) >= 0x1d) {
            unsigned h2 = at + 3 < t->len
                ? (unsigned)(t->p[at + 2] | t->p[at + 3] << 8) : 0;
            if ((h & 0xf800u) == 0xf000u && (h2 & 0xd000u) == 0xd000u) {
                /* BL: a call, or a far branch (which writes nothing --
                 * every pass must make the same choices here, whatever
                 * its branches' sizes) */
                if (v6_far_bl(F, at))
                    b = 1;
                else
                    m |= call;
            } else if ((h & 0xfffeu) == 0xf3eeu && (h2 & 0xc000u) == 0x8000u) {
                m |= 1u << (h2 >> 8 & 15);                    /* MRS */
            } else if ((h & 0xfff0u) == 0xf380u && (h2 & 0xc000u) == 0x8000u) {
                ;                                             /* MSR */
            } else if ((h & 0xffd0u) == 0xfb90u && (h2 & 0xf0f0u) == 0xf0f0u) {
                m |= 1u << (h2 >> 8 & 15);                    /* s/udiv */
            } else if (h == 0xf3bfu && (h2 & 0xff00u) == 0x8f00u) {
                ;                                             /* barriers */
            } else {
                m = 0xffffu;
                b = 1;
            }
            at += 4;
            continue;
        }
        at += 2;
        if (h < 0x2000u) {                       /* shifts, add/sub reg/imm3 */
            m |= 1u << (h & 7);
        } else if (h < 0x4000u) {                /* movs/cmp/adds/subs imm8 */
            if ((h >> 11) != 5)
                m |= 1u << (h >> 8 & 7);
        } else if ((h & 0xfc00u) == 0x4000u) {   /* data processing */
            unsigned op = h >> 6 & 15;
            if (op != 8 && op != 10 && op != 11)          /* tst, cmp, cmn */
                m |= 1u << (h & 7);
        } else if ((h & 0xfc00u) == 0x4400u) {   /* hi add/cmp/mov, bx/blx */
            unsigned op = h >> 8 & 3, rd = (h >> 4 & 8) | (h & 7);
            if (op == 3) {
                if (h & 0x80u) m |= call;                     /* BLX */
                else           b = 1;                         /* BX */
            } else if (op != 1) {
                m |= 1u << rd;
                if (rd == 15)
                    b = 1;
            }
        } else if (h < 0x5000u) {                /* ldr literal */
            m |= 1u << (h >> 8 & 7);
        } else if (h < 0x6000u) {                /* ld/st register offset */
            if ((h >> 9 & 7) >= 3)
                m |= 1u << (h & 7);
        } else if (h < 0x9000u) {                /* ld/st immediate */
            if (h & 0x800u)
                m |= 1u << (h & 7);
        } else if (h < 0xa000u) {                /* ld/st sp */
            if (h & 0x800u)
                m |= 1u << (h >> 8 & 7);
        } else if (h < 0xb000u) {                /* adr, add rd, sp */
            m |= 1u << (h >> 8 & 7);
        } else if (h < 0xc000u) {                /* miscellaneous */
            if ((h & 0xff00u) == 0xb000u)
                m |= 1u << 13;                                /* sp */
            else if ((h & 0xff00u) == 0xb200u || (h & 0xff00u) == 0xba00u)
                m |= 1u << (h & 7);                           /* ext, rev */
            else if ((h & 0xfe00u) == 0xb400u)
                ;                                             /* push */
            else if ((h & 0xfe00u) == 0xbc00u) {
                m |= h & 0xffu;                               /* pop */
                if (h & 0x100u)
                    b = 1;
            } else if ((h & 0xf500u) == 0xb100u)
                b = 1;                                        /* cbz */
            else if ((h & 0xff00u) == 0xbf00u || (h & 0xff00u) == 0xbe00u ||
                     (h & 0xffe8u) == 0xb660u)
                ;                                     /* hints, bkpt, cps */
            else {
                m = 0xffffu;
                b = 1;
            }
        } else if (h < 0xd000u) {                /* stm/ldm */
            m |= 1u << (h >> 8 & 7);
            if (h & 0x800u)
                m |= h & 0xffu;
        } else {                                 /* b<c>, udf, svc, b */
            if ((h & 0xff00u) == 0xdf00u)
                m |= 0xffffu;
            b = 1;
        }
    }
    if (br)
        *br = b;
    return m;
}

/* May v's slot be cached: a value in memory, its address never taken. */
static int v6_rc_ok(const struct t_fn *F, int v)
{
    long fo;
    return v >= 0 && !(F->loc && F->loc[v] >= 0) && F->slot[v] >= 0 &&
           !(g6_atk && g6_atk[v]) && !tcg_faddr(F, v, &fo);
}

/* A store wrote [off, off + size) of the frame. */
static void rc_drop(long off, int size)
{
    for (int r = 0; r < 8; r++)
        if (g6_rc_off[r] >= 0 && g6_rc_off[r] < off + size &&
            off < g6_rc_off[r] + 4)
            g6_rc_off[r] = -1;
}

/* r holds the frame word at off from here on -- when everything emitted
 * for this instruction so far is straight-line. */
static void rc_note(const struct t_fn *F, int r, long off)
{
    int br = 0;
    if (r < 0 || r > 7)
        return;
    g6_rc_off[r] = -1;
    if (rc_off())
        return;
    if (g6_ins_at > F->t->len)
        g6_ins_at = F->t->len;
    (void)v6_writes(F, g6_ins_at, F->t->len, &br);
    if (br)
        return;
    g6_rc_off[r] = off;
    g6_rc_at[r] = F->t->len;
}

static int rc_find(const struct t_fn *F, long off);

/* What each register holds now, checked: off[r], or -1. */
static void rc_state(const struct t_fn *F, long *off)
{
    for (int r = 0; r < 8; r++)
        off[r] = g6_rc_off[r] >= 0 && rc_find(F, g6_rc_off[r]) == r
                 ? g6_rc_off[r] : -1;
}

/* A jump to `label` is emitted here: what holds now is what holds at the
 * label, as far as this way in goes. */
static void rc_snap(const struct t_fn *F, int label)
{
    long cur[8], *s;
    if (!g6_lfwd || label < 0 || label >= F->fn->nlabels || !g6_lfwd[label])
        return;
    s = g6_lsnap + 8L * label;
    rc_state(F, cur);
    for (int r = 0; r < 8; r++)
        s[r] = !g6_lseen[label] || s[r] == cur[r] ? cur[r] : -1;
    g6_lseen[label] = 1;
}

/* At `label`: what holds on every way in (the fall-through unless the
 * code before ends in a jump), from here on; nothing where some way in is
 * not known. */
static void rc_label(const struct t_fn *F, int label)
{
    long cur[8], *s;
    int ft = !F->barrier;
    if (!g6_lfwd || label < 0 || label >= F->fn->nlabels || !g6_lfwd[label]) {
        rc_reset();
        return;
    }
    s = g6_lsnap + 8L * label;
    rc_state(F, cur);
    for (int r = 0; r < 8; r++) {
        long o = ft ? cur[r] : -1;
        if (g6_lseen[label])
            o = !ft || o == s[r] ? s[r] : -1;
        g6_rc_off[r] = o;
        g6_rc_at[r] = F->t->len;
    }
}

/* A register holding the frame word at off now, or -1. */
static int rc_find(const struct t_fn *F, long off)
{
    for (int r = 0; r < 8; r++) {
        if (g6_rc_off[r] != off)
            continue;
        if (g6_rc_at[r] > F->t->len ||
            (v6_writes(F, g6_rc_at[r], F->t->len, NULL) >> r & 1)) {
            g6_rc_off[r] = -1;
            continue;
        }
        return r;
    }
    return -1;
}

/* A temporary low register that nothing this instruction holds is in:
 * S1, S0, then one of r0-r5 saved on the stack meanwhile. Released in
 * reverse order. */
static int tmp_get(struct t_fn *F, unsigned avoid)
{
    /* the roles first (nothing is in them), then the rest pushed: r0-r5
     * -- and r6/r7 too when they may be homes */
    int order[8], no = 0;
    if (g6_s1 >= 0) order[no++] = g6_s1;
    if (g6_s0 >= 0) order[no++] = g6_s0;
    for (int r = 0; r < (tcg_ext() ? 8 : 6); r++)
        if (r != g6_s0 && r != g6_s1 && no < 8)
            order[no++] = r;
    avoid |= F->tbusy | F->ins_mask;
    if (F->fb == FB6)
        avoid |= 1u << FB6;
    for (int k = 0; k < no; k++) {
        int r = order[k];
        if (r < 0 || (avoid & (1u << r)))
            continue;
        if (F->ntmp >= 6)
            internal_error("thumb: %s: too many v6 temporaries",
                           F->fn->name);
        F->tbusy |= 1u << r;
        F->tmp_reg[F->ntmp] = r;
        F->tmp_pushed[F->ntmp] = r != g6_s0 && r != g6_s1;
        if (F->tmp_pushed[F->ntmp]) {
            t1_push(F->t, 1u << r);
            F->spb += 4;
        } else {
            sc(F, r);
        }
        F->ntmp++;
        return r;
    }
    internal_error("thumb: %s: no low register left for a temporary",
                   F->fn->name);
    return -1;
}

static void tmp_put(struct t_fn *F, int r)
{
    if (!F->ntmp || F->tmp_reg[F->ntmp - 1] != r)
        internal_error("thumb: %s: v6 temporaries released out of order",
                       F->fn->name);
    F->ntmp--;
    F->tbusy &= ~(1u << r);
    if (F->tmp_pushed[F->ntmp]) {
        t1_pop(F->t, 1u << r);
        F->spb -= 4;
    }
}

/* ---- the literal pool ---------------------------------------------------- */

/* LIT_LREL: &&label's word, (label | 1) - pc at its `add rD, pc`; v
 * is its index in F->lrel, so no two share a word. */
enum { LIT_K, LIT_STR, LIT_GLOB, LIT_FN, LIT_LREL };
struct v6_lit { int kind; unsigned long v; const void *p; };
struct v6_lsite { int at, k; };

static void note_pad(struct t_fn *F, int at)
{
    if (F->npads == F->cappads) {
        F->cappads = F->cappads ? F->cappads * 2 : 16;
        F->pads = xrealloc(F->pads, (size_t)F->cappads * sizeof *F->pads);
    }
    F->pads[F->npads++] = at;
}

/* rd = the pool word (kind, v, p): `ldr rd, [pc, #off]`, patched when the
 * pool is placed. Flag-free. */
static void lit_load(struct t_fn *F, int rd, int kind, unsigned long v,
                     const void *p)
{
    int k;
    v &= 0xffffffffUL;
    for (k = 0; k < F->nlit; k++)
        if (F->lit[k].kind == kind && F->lit[k].v == v && F->lit[k].p == p)
            break;
    if (k == F->nlit) {
        if (F->nlit == F->caplit) {
            F->caplit = F->caplit ? F->caplit * 2 : 16;
            F->lit = xrealloc(F->lit, (size_t)F->caplit * sizeof *F->lit);
        }
        F->lit[k].kind = kind;
        F->lit[k].v = v;
        F->lit[k].p = p;
        F->nlit++;
    }
    if (F->nlsite == F->caplsite) {
        F->caplsite = F->caplsite ? F->caplsite * 2 : 16;
        F->lsite = xrealloc(F->lsite, (size_t)F->caplsite * sizeof *F->lsite);
    }
    F->lsite[F->nlsite].at = F->t->len;
    F->lsite[F->nlsite].k = k;
    F->nlsite++;
    if (rd < 0 || rd > 7 || !t_ldr_lit16(F->t, rd, 0))
        internal_error("thumb: %s: a literal load into r%d", F->fn->name, rd);
}

/* Place the pool here: a branch around it when code continues past it,
 * a pad to a word boundary, the words (marked as data), and every pending
 * load pointed at its word. */
static void pool_dump(struct t_fn *F, int branch)
{
    struct code *t = F->t;
    int over = -1, base;
    if (!F->nlsite)
        return;
    rc_reset();
    if (branch)
        over = t_b16(t);
    note_pad(F, t->len);
    if (t->len & 2)
        t_nop(t);
    base = t->len;
    for (int k = 0; k < F->nlit; k++) {
        const struct v6_lit *l = &F->lit[k];
        switch (l->kind) {
        case LIT_STR:
            tcg_note_str(F->st, t->len, (int)l->v, RK_ABS32);
            code_u32(t, 0);
            break;
        case LIT_GLOB:
            tcg_note_glob(F->st, t->len, (struct global *)l->p, RK_ABS32);
            code_u32(t, 0);
            break;
        case LIT_FN:
            tcg_note_fn(F->st, t->len, (struct func *)l->p, RK_ABS32);
            code_u32(t, 0);
            break;
        case LIT_LREL:
            /* the jump table's word, (target | 1) - base, with the add's
             * pc as the base: patched with the branches */
            tcg_want_label(F, t->len, F->lrel[2 * l->v], T_TAB);
            F->fix[F->nfix - 1].cz_at = F->lrel[2 * l->v + 1] + 4;
            code_u32(t, 0);
            break;
        default:
            code_u32(t, l->v);
            break;
        }
    }
    code_mark_data(t, base, t->len);
    for (int s = 0; s < F->nlsite; s++)
        if (!t1_patch_ldr_lit(t, F->lsite[s].at, base + 4 * F->lsite[s].k))
            internal_error("thumb: %s: a literal is %d bytes from its load",
                           F->fn->name, base + 4 * F->lsite[s].k -
                           F->lsite[s].at);
    if (branch && !t_patch_b16(t, over, t->len))
        internal_error("thumb: %s: a literal pool too large to branch "
                       "around", F->fn->name);
    F->nlit = F->nlsite = 0;
}

static void pool_point(struct t_fn *F, long est, int natural);

/* ---- constants ----------------------------------------------------------- */

/* rd = v, rd in r0-r7, the flags dead. */
static void k32(struct t_fn *F, int rd, unsigned long v)
{
    struct code *t = F->t;
    unsigned long n;
    int sh;
    v &= 0xffffffffUL;
    n = ~v & 0xffffffffUL;
    if (v <= 255) {
        t1_movs_imm(t, rd, (long)v);
        return;
    }
    if (n <= 255) {
        t1_movs_imm(t, rd, (long)n);
        t1_mvns(t, rd, rd);
        return;
    }
    for (sh = 1; sh < 32; sh++)
        if ((v & ((1UL << sh) - 1)) == 0 && (v >> sh) <= 255 &&
            (v >> sh << sh) == v) {
            t1_movs_imm(t, rd, (long)(v >> sh));
            t1_shift_imm(t, T_SH_LSL, rd, rd, sh);
            return;
        }
    if (v <= 510) {
        t1_movs_imm(t, rd, 255);
        t1_addsub_imm8(t, T_OP_ADD, rd, (long)(v - 255));
        return;
    }
    if (((0UL - v) & 0xffffffffUL) <= 255) {
        t1_movs_imm(t, rd, (long)((0UL - v) & 0xffffffffUL));
        t1_negs(t, rd, rd);
        return;
    }
    lit_load(F, rd, LIT_K, v, NULL);
}

/* rd = v without touching the flags: a literal. */
static void k32_nf(struct t_fn *F, int rd, unsigned long v)
{
    lit_load(F, rd, LIT_K, v, NULL);
}

/* ---- the frame ------------------------------------------------------------ */

/* rd = frame base + off. Flag-free. */
static void fr_addr(struct t_fn *F, int rd, long off)
{
    struct code *t = F->t;
    if (F->fb == T_SP) {
        long o = off + F->spb;
        if (t1_add_sp_imm(t, rd, o))
            return;
        k32_nf(F, rd, (unsigned long)o);
        t1_add_hi(t, rd, T_SP);
        return;
    }
    if (off == 0) {
        mov(F, rd, F->fb);
        return;
    }
    k32_nf(F, rd, (unsigned long)off);
    t1_add_hi(t, rd, F->fb);
}

/* rt = the `size`-byte value at frame base + off, extended. Flag-free; rt
 * is its own address register when the offset does not reach. */
static void fr_ld(struct t_fn *F, int rt, long off, int size, int sign)
{
    struct code *t = F->t;
    if (F->fb == T_SP) {
        long o = off + F->spb;
        if (size == 4 && t1_ldst_sp(t, rt, o, 0))
            return;
        if (o >= 0 && (o & ~3L) <= 1020 && (o & (size - 1)) == 0) {
            t1_add_sp_imm(t, rt, o & ~3L);
            t1_ldst_imm(t, rt, rt, o & 3L, size, 0);
            if (sign && size < 4)
                t1_ext(t, rt, rt, size, 1);
            return;
        }
    } else if (t1_ldst_imm(t, rt, F->fb, off, size, 0)) {
        if (sign && size < 4)
            t1_ext(t, rt, rt, size, 1);
        return;
    }
    fr_addr(F, rt, off);
    t1_ldst_imm(t, rt, rt, 0, size, 0);
    if (sign && size < 4)
        t1_ext(t, rt, rt, size, 1);
}

/* frame base + off = the low `size` bytes of rv. Flag-free; a temporary
 * that is none of `avoid` when the address must be built. */
static void fr_st(struct t_fn *F, int rv, long off, int size, unsigned avoid)
{
    struct code *t = F->t;
    int a;
    rc_drop(off, size);
    if (F->fb == T_SP) {
        if (size == 4 && t1_ldst_sp(t, rv, off + F->spb, 1))
            return;
    } else if (t1_ldst_imm(t, rv, F->fb, off, size, 1)) {
        return;
    }
    a = tmp_get(F, avoid | (1u << rv));
    if (F->fb == T_SP) {
        long o = off + F->spb;
        if (o >= 0 && (o & ~3L) <= 1020 && (o & (size - 1)) == 0) {
            t1_add_sp_imm(t, a, o & ~3L);
            t1_ldst_imm(t, rv, a, o & 3L, size, 1);
            tmp_put(F, a);
            return;
        }
    }
    fr_addr(F, a, off);
    t1_ldst_imm(t, rv, a, 0, size, 1);
    tmp_put(F, a);
}

/* ---- values ------------------------------------------------------------- */

/* reg = the frame word at off (a slot v6_rc_ok admits): a copy of a
 * register the slot cache says holds it, or a load. Flag-free. */
static void slot_rd(struct t_fn *F, int reg, long off)
{
    int c = rc_find(F, off);
    if (c >= 0) {
        mov(F, reg, c);
    } else {
        fr_ld(F, reg, off, 4, 0);
    }
    rc_note(F, reg, off);
}

/* Where the code of the instruction being lowered is still nothing but
 * reads of its operands (v_rd): the flags hold nothing it set, so a
 * constant made there may be MOVS. No flag value survives from one IR
 * instruction to the next (the file's header). */
static int g6_fdead;
/* ...and an instruction whose whole lowering tests nothing -- a call, a
 * store, a return, a helper's call -- has dead flags throughout (as
 * codegen.c's flags_dead_ins). */
static int g6_fdead_ins;

static int v6_flagless_ins(const struct ir_ins *i)
{
    switch (i->op) {
    case IR_CALL: case IR_STORE: case IR_STVAR: case IR_RET:
    case IR_MEMCPY: case IR_MEMZERO: case IR_I2F: case IR_F2I: case IR_F2F:
        return 1;
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV:
        return i->flt;
    default:
        return 0;
    }
}

/* reg = the constant k of a value made where it is read (F->remat): MOVS
 * where the flags are dead and it fits, else its pool word -- a literal
 * load is flag-free, and the same size as the slot load it replaces. */
static void remat_k(struct t_fn *F, int reg, long k)
{
    unsigned long v = (unsigned long)k & 0xffffffffUL;
    if (v <= 255 && (g6_fdead_ins || F->t->len == g6_fdead))
        t1_movs_imm(F->t, reg, (long)v);
    else
        k32_nf(F, reg, v);
}

static void v_rd_(struct t_fn *F, int v, int reg);

/* v into exactly `reg` (r0-r7). Flag-free -- except a constant made where
 * nothing but reads came before it (g6_fdead), which keeps that so. */
static void v_rd(struct t_fn *F, int v, int reg)
{
    int dead = F->t->len == g6_fdead;
    v_rd_(F, v, reg);
    if (dead)
        g6_fdead = F->t->len;
}

static void v_rd_(struct t_fn *F, int v, int reg)
{
    long fo;
    if (F->remat && v >= 0 && F->remat[v]) {
        remat_k(F, reg, F->remat_v[v]);
        return;
    }
    if (in_reg6(F, v)) {
        mov(F, reg, F->loc[v]);
        return;
    }
    if (tcg_faddr(F, v, &fo)) {
        fr_addr(F, reg, fo);
        return;
    }
    if (v6_rc_ok(F, v)) {
        slot_rd(F, reg, F->slot[v]);
        return;
    }
    fr_ld(F, reg, tcg_slot_of(F, v), 4, 0);
}

/* May a read hand back c, a register the slot cache found v in, instead
 * of the scratch it asked for: one the lowering will not write before it
 * is done reading -- no role (it may write that one; see v_rdr for the
 * other), not the result's home, nor the frame base. It is then held
 * (tbusy) so no temporary is taken from it. */
static int rc_usable(const struct t_fn *F, int c)
{
    const struct ir_ins *i = g6_cur;
    if (c == g6_s0 || c == g6_s1 || (F->fb == FB6 && c == FB6))
        return 0;
    if (i && i->dst >= 0 && in_reg6(F, i->dst) &&
        (c == F->loc[i->dst] || (F->wide[i->dst] && c == F->loc[i->dst] + 1)))
        return 0;
    return 1;
}

/* Where v is: its register, or `scr` loaded with it -- or a register the
 * slot cache found it in. When that is the OTHER role and no role has
 * been named yet this instruction but this one (the call's own
 * argument), the two roles trade places for the rest of the lowering:
 * they are interchangeable, and the read then lands on what the lowering
 * asked for. */
static int v_rdr(struct t_fn *F, int v, int scr)
{
    int c = -1;
    if (in_reg6(F, v))
        return F->loc[v];
    if (v6_rc_ok(F, v))
        c = rc_find(F, F->slot[v]);
    if (c >= 0) {
        if (c == scr)
            return sc(F, scr);
        if (g6_role_n == 1 && g6_s0 >= 0 && g6_s1 >= 0 &&
            ((scr == g6_s0 && c == g6_s1) || (scr == g6_s1 && c == g6_s0))) {
            int x = g6_s0;
            g6_s0 = g6_s1;
            g6_s1 = x;
            return sc(F, c);
        }
        if (rc_usable(F, c)) {
            F->tbusy |= 1u << c;
            return c;
        }
    }
    v_rd(F, v, sc(F, scr));
    return scr;
}

/* Where to compute v: its register, or `scr`. */
static int v_wreg(struct t_fn *F, int v, int scr)
{
    return in_reg6(F, v) ? F->loc[v] : sc(F, scr);
}

static void v_wr(struct t_fn *F, int v, int reg)
{
    long fo;
    if (v < 0)
        return;
    if (in_reg6(F, v)) {
        mov(F, F->loc[v], reg);
        return;
    }
    if (F->slot[v] < 0 || tcg_faddr(F, v, &fo))
        return;
    (void)tcg_slot_of(F, v);
    fr_st(F, reg, F->slot[v], 4, 0);
    if (v6_rc_ok(F, v))
        rc_note(F, reg, F->slot[v]);
}

/* dl <- sl and dh <- sh as one move; r12 breaks the swap. */
static void mv2(struct t_fn *F, int dl, int sl, int dh, int sh)
{
    struct code *t = F->t;
    if (dl == sh && dh == sl) {
        if (dl == sl)
            return;
        t_mov_reg(t, IP, sl);
        t_mov_reg(t, dh, sh);
        t_mov_reg(t, dl, IP);
        return;
    }
    if (dl == sh) {
        mov(F, dh, sh);
        mov(F, dl, sl);
        return;
    }
    mov(F, dl, sl);
    mov(F, dh, sh);
}

static void v_rd64(struct t_fn *F, int v, int lo, int hi)
{
    if (in_reg6(F, v)) {
        mv2(F, lo, F->loc[v], hi, F->loc[v] + 1);
        return;
    }
    (void)tcg_slot_of(F, v);
    if (v6_rc_ok(F, v)) {
        slot_rd(F, lo, F->slot[v]);
        slot_rd(F, hi, F->slot[v] + 4);
        return;
    }
    fr_ld(F, lo, F->slot[v], 4, 0);
    fr_ld(F, hi, F->slot[v] + 4, 4, 0);
}

static void v_wr64(struct t_fn *F, int v, int lo, int hi)
{
    if (v < 0)
        return;
    if (in_reg6(F, v)) {
        mv2(F, F->loc[v], lo, F->loc[v] + 1, hi);
        return;
    }
    (void)tcg_slot_of(F, v);
    if (F->slot[v] < 0)
        return;
    fr_st(F, lo, F->slot[v], 4, 1u << hi);
    fr_st(F, hi, F->slot[v] + 4, 4, 1u << lo);
    if (v6_rc_ok(F, v)) {
        rc_note(F, lo, F->slot[v]);
        rc_note(F, hi, F->slot[v] + 4);
    }
}

/* ---- memory through a pointer ------------------------------------------ */

/* rt = `size` bytes at base + off, extended. `nat` says the address is
 * aligned to the access; a word or halfword that may not be is read a
 * byte at a time, because ARMv6-M faults on an unaligned one. Clobbers the
 * flags (it may build an offset). */
static void mem_ld(struct t_fn *F, int rt, int base, long off, int size,
                   int sign, int nat)
{
    struct code *t = F->t;
    if (size >= 4)
        sign = 0;
    if (nat || size == 1) {
        if (off >= 0 && off % size == 0 && off / size <= 31) {
            t1_ldst_imm(t, rt, base, off, size, 0);
            if (sign)
                t1_ext(t, rt, rt, size, 1);
            return;
        }
        if (rt != base) {
            k32(F, rt, (unsigned long)off);
            t1_ldst_reg(t, rt, base, rt, size, sign, 0);
            return;
        }
        {
            int x = tmp_get(F, 1u << rt);
            k32(F, x, (unsigned long)off);
            t1_ldst_reg(t, rt, base, x, size, sign, 0);
            tmp_put(F, x);
        }
        return;
    }
    {
        /* Byte by byte, most significant first: little-endian. */
        int a = base, b, own = 0;
        long o = off;
        if (off < 0 || off + size - 1 > 31 || rt == base) {
            a = tmp_get(F, (1u << rt) | (1u << base));
            own = 1;
            k32(F, a, (unsigned long)off);
            t1_addsub_reg(t, T_OP_ADD, a, a, base);
            o = 0;
        }
        b = tmp_get(F, (1u << rt) | (1u << a));
        t1_ldst_imm(t, rt, a, o + size - 1, 1, 0);
        for (int k = size - 2; k >= 0; k--) {
            t1_shift_imm(t, T_SH_LSL, rt, rt, 8);
            t1_ldst_imm(t, b, a, o + k, 1, 0);
            t1_alu_reg(t, T_OP_ORR, rt, b);
        }
        tmp_put(F, b);
        if (own)
            tmp_put(F, a);
        if (sign && size < 4)
            t1_ext(t, rt, rt, size, 1);
    }
}

/* base + off = the low `size` bytes of rv. */
static void mem_st(struct t_fn *F, int rv, int base, long off, int size,
                   int nat, unsigned avoid)
{
    struct code *t = F->t;
    avoid |= (1u << rv) | (1u << base);
    if (nat || size == 1) {
        if (off >= 0 && off % size == 0 && off / size <= 31) {
            t1_ldst_imm(t, rv, base, off, size, 1);
            return;
        }
        {
            int x = tmp_get(F, avoid);
            k32(F, x, (unsigned long)off);
            t1_ldst_reg(t, rv, base, x, size, 0, 1);
            tmp_put(F, x);
        }
        return;
    }
    {
        int a = base, b, own = 0;
        long o = off;
        if (off < 0 || off + size - 1 > 31) {
            a = tmp_get(F, avoid);
            own = 1;
            k32(F, a, (unsigned long)off);
            t1_addsub_reg(t, T_OP_ADD, a, a, base);
            o = 0;
        }
        b = tmp_get(F, avoid | (1u << a));
        t1_ldst_imm(t, rv, a, o, 1, 1);
        for (int k = 1; k < size; k++) {
            t1_shift_imm(t, T_SH_LSR, b, rv, 8 * k);
            t1_ldst_imm(t, b, a, o + k, 1, 1);
        }
        tmp_put(F, b);
        if (own)
            tmp_put(F, a);
    }
}

/* ---- branches ------------------------------------------------------------ */

static int class_of(const struct t_fn *F, int k, int cond)
{
    if (F->shortb && k < F->nshortb)
        return F->shortb[k];
    if (F->far_mode)
        return BC_FAR;
    return cond < 0 ? BC_SHORT : BC_MED;
}

/* A branch to `label` (cond < 0: unconditional), in the size class the
 * last measurement gave its ordinal. Returns the fix's index. */
static int v6_branch(struct t_fn *F, int cond, int label)
{
    struct code *t = F->t;
    int k = F->nfix, cls = class_of(F, k, cond), start = t->len, at, skip;
    rc_snap(F, label);
    if (cond < 0) {
        at = cls == BC_FAR ? t_bl(t) : t_b16(t);
    } else if (cls == BC_SHORT) {
        at = t_bcond16(t, cond);
    } else {
        skip = t_bcond16(t, cond ^ 1);
        at = cls == BC_FAR ? t_bl(t) : t_b16(t);
        if (!t_patch_bcond16(t, skip, t->len))
            internal_error("thumb: %s: a branch's skip", F->fn->name);
    }
    tcg_want_label(F, at, label, cond);
    F->fix[k].sz = t->len - start;
    F->fix[k].cz_at = start;
    F->fix[k].ins = cls;
    return k;
}

static void v6_jump_to(struct t_fn *F, int label)
{
    v6_branch(F, -1, label);
    F->bc_end = -1;
    F->barrier = 1;
}

static void v6_jump_if(struct t_fn *F, int cond, int label)
{
    F->bc_fix = v6_branch(F, cond, label);
    F->bc_end = F->t->len;
    F->barrier = 0;
}

/* codegen.c's invert_last_bcond: `b<c> L1; b label; L1:` is `b<!c> label`.
 * The same ordinal, so the same size class. */
static int v6_invert_last(struct t_fn *F, int n, int label)
{
    struct ir_func *fn = F->fn;
    int hit = 0, cond, k;
    if (F->bc_end != F->t->len || F->bc_fix != F->nfix - 1)
        return 0;
    for (int m = n + 1; m < fn->nins && fn->ins[m].op == IR_LABEL; m++)
        if (fn->ins[m].label == F->fix[F->bc_fix].label) { hit = 1; break; }
    if (!hit)
        return 0;
    cond = F->fix[F->bc_fix].cond ^ 1;
    F->t->len -= F->fix[F->bc_fix].sz;
    for (int r = 0; r < 8; r++)         /* begun after the branch removed */
        if (g6_rc_at[r] > F->t->len)
            g6_rc_off[r] = -1;
    if (g6_ins_at > F->t->len)
        g6_ins_at = F->t->len;
    F->nfix--;
    k = v6_branch(F, cond, label);
    (void)k;
    F->bc_end = -1;
    F->barrier = 0;
    return 1;
}

/* A short branch inside one lowering: emitted now, patched by the caller. */
static int bc16(struct t_fn *F, int cond) { return t_bcond16(F->t, cond); }
static void bc16_here(struct t_fn *F, int at)
{
    if (!t_patch_bcond16(F->t, at, F->t->len))
        internal_error("thumb: %s: an internal branch out of reach",
                       F->fn->name);
}
static int b16(struct t_fn *F) { return t_b16(F->t); }
static void b16_here(struct t_fn *F, int at)
{
    if (!t_patch_b16(F->t, at, F->t->len))
        internal_error("thumb: %s: an internal branch out of reach",
                       F->fn->name);
}

/* ---- calls the lowering makes ---------------------------------------------
 *
 * A runtime routine's arguments are words in r0-r3. `n` operands, each a
 * vreg of `nw[k]` words (1 or 2) bound for r`dst[k]`, or a constant when
 * vreg < 0. The register-resident ones move in PARALLEL first (one at a
 * time would overwrite a register another is still to be read from), then
 * the rest load: those only write argument registers nothing still needs. */
static void call_args(struct t_fn *F, int n, const int *vr, const int *nw,
                      const int *dst, const long *kv)
{
    int pd[8], ps[8], npm = 0;
    for (int k = 0; k < n; k++)
        if (vr[k] >= 0 && in_reg6(F, vr[k]))
            for (int q = 0; q < nw[k]; q++) {
                pd[npm] = dst[k] + q;
                ps[npm] = F->loc[vr[k]] + q;
                npm++;
            }
    if (npm) {
        int od[16], os[16];
        int m = ra_parallel_move(pd, ps, npm, IP, od, os, 16);
        if (m < 0)
            internal_error("thumb: %s: a helper's argument setup is not a "
                           "well-formed move", F->fn->name);
        for (int k = 0; k < m; k++)
            t_mov_reg(F->t, od[k], os[k]);
    }
    for (int k = 0; k < n; k++) {
        if (vr[k] >= 0 && in_reg6(F, vr[k]))
            continue;
        if (vr[k] < 0) {
            k32(F, dst[k], (unsigned long)kv[k]);
            if (nw[k] == 2)
                k32(F, dst[k] + 1, (unsigned long)kv[k] >> 32);
        } else if (nw[k] == 2) {
            v_rd64(F, vr[k], dst[k], dst[k] + 1);
        } else {
            v_rd(F, vr[k], dst[k]);
        }
    }
}

/* The parallel move a two-operand helper's setup makes with its operands
 * in this order (call_args), as a count. */
static int v6_args_cost(const struct t_fn *F, int va, int vb, int ww)
{
    int pd[4], ps[4], npm = 0, v[2] = { va, vb }, od[8], os[8];
    for (int k = 0; k < 2; k++)
        if (in_reg6(F, v[k]))
            for (int q = 0; q < ww; q++) {
                pd[npm] = (ww == 2 ? 2 * k : k) + q;
                ps[npm] = F->loc[v[k]] + q;
                npm++;
            }
    return npm ? ra_parallel_move(pd, ps, npm, IP, od, os, 8) : 0;
}

/* dst = op(a, b) by a runtime routine: one or two words each. `bw` 0 for
 * a constant second operand (i->imm). The result comes back in r0 (r0:r1
 * for `rw` 2), or r1 for `rsel` (the remainder of __aeabi_idivmod). */
static void helper2(struct t_fn *F, const struct ir_ins *i, const char *name,
                    int aw, int bw, int rw, int rsel)
{
    int vr[2], nw[2], dst[2];
    long kv[2] = { 0, 0 };
    int n = 1;
    vr[0] = i->a; nw[0] = aw; dst[0] = 0;
    if (bw) {
        vr[1] = i->imm_b ? -1 : i->b;
        nw[1] = bw;
        dst[1] = aw == 2 ? 2 : 1;
        kv[1] = (long)i->imm;
        n = 2;
    }
    call_args(F, n, vr, nw, dst, kv);
    tcg_call_helper(F, name);
    if (i->dst < 0)
        return;
    if (rw == 2)
        v_wr64(F, i->dst, 0, 1);
    else
        v_wr(F, i->dst, rsel ? 1 : 0);
}

/* ---- comparisons ----------------------------------------------------------- */

/* The flags for a 32-bit `a <pred> b`. */
static void cmp32(struct t_fn *F, const struct ir_ins *i)
{
    struct code *t = F->t;
    /* The same compare as the one a branch just took, with nothing
     * emitted since and no label placed: the flags are there already.
     * A switch's tree asks `== k` and then `> k` of one register, and a
     * far `== k` is `bne` over a `b`, neither of which sets flags. */
    if (i->imm_b && in_reg6(F, i->a) && F->fl_end >= 0 &&
        F->fl_end == t->len && F->fl_reg == F->loc[i->a] &&
        F->fl_imm == i->imm)
        return;
    int ra = v_rdr(F, i->a, S0);
    if (i->imm_b) {
        long v = (long)(int)(unsigned)((unsigned long)i->imm & 0xffffffffUL);
        if (v >= 0 && v <= 255) {
            t1_cmp_imm(t, ra, v);
        } else if (v < 0 && v >= -255) {
            t1_movs_imm(t, sc(F, S1), -v);
            t1_cmn(t, ra, S1);             /* a + -v: the flags of a - v */
        } else {
            k32(F, sc(F, S1), (unsigned long)i->imm);
            t1_cmp_reg(t, ra, S1);
        }
        return;
    }
    t1_cmp_reg(t, ra, v_rdr(F, i->b, S1));
}

/* dst = the condition's truth, 0 or 1, after a compare. No IT block and no
 * flag-free constant: the branch reads the flags before anything is
 * written. */
static void set_cc(struct t_fn *F, int dst, int cond)
{
    int d = v_wreg(F, dst, S0), yes, done;
    yes = bc16(F, cond);
    t1_movs_imm(F->t, d, 0);
    done = b16(F);
    bc16_here(F, yes);
    t1_movs_imm(F->t, d, 1);
    b16_here(F, done);
    v_wr(F, dst, d);
}

/* ---- 64-bit values ----------------------------------------------------------
 *
 * A 64-bit value is a register pair (r0:r1, r2:r3, r4:r5) or an eight-byte
 * slot. An operand in a slot is loaded into two temporaries; a result
 * bound for one is computed in two (the first operand's, when it was
 * loaded and is not read again) and stored. */
struct h64 { int lo, hi, own; };

static void src64(struct t_fn *F, int v, struct h64 *h)
{
    if (in_reg6(F, v)) {
        h->lo = F->loc[v];
        h->hi = F->loc[v] + 1;
        h->own = 0;
        return;
    }
    h->lo = tmp_get(F, 0);
    h->hi = tmp_get(F, 0);
    h->own = 1;
    v_rd64(F, v, h->lo, h->hi);
}

static void srcb64(struct t_fn *F, const struct ir_ins *i, struct h64 *h)
{
    if (!i->imm_b) {
        src64(F, i->b, h);
        return;
    }
    h->lo = tmp_get(F, 0);
    h->hi = tmp_get(F, 0);
    h->own = 1;
    k32(F, h->lo, (unsigned long)i->imm);
    k32(F, h->hi, (unsigned long)i->imm >> 32);
}

/* The result's halves: its pair, `reuse` (an operand's temporaries), or
 * two new ones. */
static void dst64(struct t_fn *F, int v, struct h64 *h, const struct h64 *reuse)
{
    if (in_reg6(F, v)) {
        h->lo = F->loc[v];
        h->hi = F->loc[v] + 1;
        h->own = 0;
        return;
    }
    if (reuse && reuse->own) {
        h->lo = reuse->lo;
        h->hi = reuse->hi;
        h->own = 0;
        return;
    }
    h->lo = tmp_get(F, 0);
    h->hi = tmp_get(F, 0);
    h->own = 1;
}

static void put64(struct t_fn *F, const struct h64 *h)
{
    if (h->own) {
        tmp_put(F, h->hi);
        tmp_put(F, h->lo);
    }
}

/* rdn = rdn OP rm for the two-operand forms, d = a OP b in general. */
static void alu2(struct t_fn *F, int op, int d, int a, int b, int comm)
{
    struct code *t = F->t;
    if (d == a) {
        t1_alu_reg(t, op, d, b);
    } else if (d == b && comm) {
        t1_alu_reg(t, op, d, a);
    } else if (d != b) {
        mov(F, d, a);
        t1_alu_reg(t, op, d, b);
    } else {
        int x = tmp_get(F, (1u << a) | (1u << b) | (1u << d));
        mov(F, x, a);
        t1_alu_reg(t, op, x, b);
        mov(F, d, x);
        tmp_put(F, x);
    }
}

/* The flags for a 64-bit comparison; the condition to branch on (GT and
 * LE swap the operands, as codegen.c's cmp64 does). */
static int cmp64(struct t_fn *F, const struct ir_ins *i, enum binop pred,
                 int sign)
{
    struct code *t = F->t;
    struct h64 a, b, a0, b0;
    int x, y, cond;
    if (i->imm_b && i->imm == 0 &&
        (pred == B_EQ || pred == B_NE ||
         (sign && (pred == B_LT || pred == B_GE)))) {
        src64(F, i->a, &a);
        if (pred == B_EQ || pred == B_NE) {
            x = tmp_get(F, 0);
            mov(F, x, a.lo);
            t1_alu_reg(t, T_OP_ORR, x, a.hi);
            tmp_put(F, x);
            put64(F, &a);
            return pred == B_EQ ? T_EQ : T_NE;
        }
        t1_cmp_imm(t, a.hi, 0);
        put64(F, &a);
        return pred == B_LT ? T_LT : T_GE;
    }
    src64(F, i->a, &a);
    srcb64(F, i, &b);
    a0 = a;
    b0 = b;
    if (pred == B_GT || pred == B_LE) {
        struct h64 s = a;
        a = b;
        b = s;
        pred = pred == B_GT ? B_LT : B_GE;
    }
    if (pred == B_EQ || pred == B_NE) {
        x = tmp_get(F, 0);
        y = tmp_get(F, 0);
        mov(F, x, a.lo);
        t1_alu_reg(t, T_OP_EOR, x, b.lo);
        mov(F, y, a.hi);
        t1_alu_reg(t, T_OP_EOR, y, b.hi);
        t1_alu_reg(t, T_OP_ORR, x, y);
        tmp_put(F, y);
        tmp_put(F, x);
        cond = pred == B_EQ ? T_EQ : T_NE;
    } else {
        x = tmp_get(F, 0);
        t1_cmp_reg(t, a.lo, b.lo);
        mov(F, x, a.hi);                    /* MOV keeps the carry */
        t1_alu_reg(t, T_OP_SBC, x, b.hi);
        tmp_put(F, x);
        cond = pred == B_LT ? (sign ? T_LT : T_CC) : (sign ? T_GE : T_CS);
    }
    /* released in the reverse of the order acquired, whatever the swap */
    put64(F, &b0);
    put64(F, &a0);
    return cond;
}

/* A 64-bit shift by a constant, from (al, ah) into (dl, dh): the same pair
 * or none of it shared. */
static void shift64_imm(struct t_fn *F, int op, int sign, long n,
                        int al, int ah, int dl, int dh)
{
    struct code *t = F->t;
    int x;
    if (n <= 0) {
        mv2(F, dl, al, dh, ah);
        return;
    }
    if (n >= 64)
        n = op == T_SH_ASR ? 63 : 64;
    if (op == T_SH_LSL) {
        if (n >= 32) {
            if (n == 64)     k32(F, dh, 0);
            else if (n > 32) t1_shift_imm(t, T_SH_LSL, dh, al, (int)(n - 32));
            else             mov(F, dh, al);
            k32(F, dl, 0);
            return;
        }
        x = tmp_get(F, (1u << al) | (1u << ah) | (1u << dl) | (1u << dh));
        t1_shift_imm(t, T_SH_LSR, x, al, (int)(32 - n));
        t1_shift_imm(t, T_SH_LSL, dh, ah, (int)n);
        t1_alu_reg(t, T_OP_ORR, dh, x);
        t1_shift_imm(t, T_SH_LSL, dl, al, (int)n);
        tmp_put(F, x);
        return;
    }
    if (n >= 32) {
        if (n == 64)     k32(F, dl, 0);
        else if (n > 32) t1_shift_imm(t, op, dl, ah, (int)(n - 32));
        else             mov(F, dl, ah);
        if (sign) t1_shift_imm(t, T_SH_ASR, dh, ah, 32);
        else      k32(F, dh, 0);
        return;
    }
    x = tmp_get(F, (1u << al) | (1u << ah) | (1u << dl) | (1u << dh));
    t1_shift_imm(t, T_SH_LSL, x, ah, (int)(32 - n));
    t1_shift_imm(t, T_SH_LSR, dl, al, (int)n);   /* the low word: logical */
    t1_alu_reg(t, T_OP_ORR, dl, x);
    t1_shift_imm(t, op, dh, ah, (int)n);
    tmp_put(F, x);
}

/* One half of a 64-bit AND/OR/XOR with a constant. */
static void logic_half(struct t_fn *F, int op, int d, int s, unsigned long c)
{
    c &= 0xffffffffUL;
    if ((op == T_OP_AND && c == 0xffffffffUL) || (op != T_OP_AND && c == 0)) {
        mov(F, d, s);
        return;
    }
    if (op == T_OP_AND && c == 0) {
        k32(F, d, 0);
        return;
    }
    if (op == T_OP_ORR && c == 0xffffffffUL) {
        k32(F, d, 0xffffffffUL);
        return;
    }
    if (op == T_OP_EOR && c == 0xffffffffUL) {
        t1_mvns(F->t, d, s);
        return;
    }
    {
        int x = tmp_get(F, (1u << d) | (1u << s));
        k32(F, x, c);
        alu2(F, op, d, s, x, 1);
        tmp_put(F, x);
    }
}

/* Memory at a 64-bit access's address: the frame base (for a local's
 * address) or a register, with *off adjusted. */
static int mem_base(struct t_fn *F, int a, long *off, int *own, unsigned avoid)
{
    long fo;
    *own = 0;
    if (tcg_faddr(F, a, &fo)) {
        int r = tmp_get(F, avoid);
        fr_addr(F, r, fo + *off);
        *off = 0;
        *own = 1;
        return r;
    }
    if (in_reg6(F, a))
        return F->loc[a];
    {
        int r = tmp_get(F, avoid);
        v_rd(F, a, r);
        *own = 1;
        return r;
    }
}

static int gen_ins64(struct t_fn *F, int n)
{
    struct ir_func *fn = F->fn;
    struct ir_ins *i = &fn->ins[n];
    struct code *t = F->t;
    struct h64 a, b, d;

    switch (i->op) {
    case IR_CONST:
        dst64(F, i->dst, &d, NULL);
        k32(F, d.lo, (unsigned long)i->imm);
        k32(F, d.hi, (unsigned long)i->imm >> 32);
        v_wr64(F, i->dst, d.lo, d.hi);
        put64(F, &d);
        return 1;

    case IR_BITCAST:
    case IR_MOV:
        if (in_reg6(F, i->dst)) {
            v_rd64(F, i->a, F->loc[i->dst], F->loc[i->dst] + 1);
        } else if (in_reg6(F, i->a)) {
            v_wr64(F, i->dst, F->loc[i->a], F->loc[i->a] + 1);
        } else {
            src64(F, i->a, &a);
            v_wr64(F, i->dst, a.lo, a.hi);
            put64(F, &a);
        }
        return 1;

    case IR_ADD: case IR_SUB: {
        int add = i->op == IR_ADD;
        src64(F, i->a, &a);
        srcb64(F, i, &b);
        dst64(F, i->dst, &d, &a);
        /* the low words, then the high ones with the carry: nothing
         * between the two may set the flags, and MOV does not */
        t1_addsub_reg(t, add ? T_OP_ADD : T_OP_SUB, d.lo, a.lo, b.lo);
        if (add) {
            if (d.hi == a.hi)       t1_alu_reg(t, T_OP_ADC, d.hi, b.hi);
            else if (d.hi == b.hi)  t1_alu_reg(t, T_OP_ADC, d.hi, a.hi);
            else { mov(F, d.hi, a.hi); t1_alu_reg(t, T_OP_ADC, d.hi, b.hi); }
        } else {
            if (d.hi == a.hi) {
                t1_alu_reg(t, T_OP_SBC, d.hi, b.hi);
            } else if (d.hi != b.hi) {
                mov(F, d.hi, a.hi);
                t1_alu_reg(t, T_OP_SBC, d.hi, b.hi);
            } else {
                int x = tmp_get(F, (1u << a.hi) | (1u << b.hi) |
                                   (1u << d.lo) | (1u << d.hi));
                mov(F, x, a.hi);
                t1_alu_reg(t, T_OP_SBC, x, b.hi);
                mov(F, d.hi, x);
                tmp_put(F, x);
            }
        }
        v_wr64(F, i->dst, d.lo, d.hi);
        put64(F, &d);
        put64(F, &b);
        put64(F, &a);
        return 1;
    }

    case IR_AND: case IR_OR: case IR_XOR: {
        int op = i->op == IR_AND ? T_OP_AND : i->op == IR_OR ? T_OP_ORR
                                                             : T_OP_EOR;
        src64(F, i->a, &a);
        dst64(F, i->dst, &d, &a);
        if (i->imm_b) {
            logic_half(F, op, d.lo, a.lo, (unsigned long)i->imm);
            logic_half(F, op, d.hi, a.hi, (unsigned long)i->imm >> 32);
            v_wr64(F, i->dst, d.lo, d.hi);
            put64(F, &d);
            put64(F, &a);
            return 1;
        }
        src64(F, i->b, &b);
        alu2(F, op, d.lo, a.lo, b.lo, 1);
        alu2(F, op, d.hi, a.hi, b.hi, 1);
        v_wr64(F, i->dst, d.lo, d.hi);
        put64(F, &b);
        put64(F, &d);
        put64(F, &a);
        return 1;
    }

    case IR_BNOT:
        src64(F, i->a, &a);
        dst64(F, i->dst, &d, &a);
        t1_mvns(t, d.lo, a.lo);
        t1_mvns(t, d.hi, a.hi);
        v_wr64(F, i->dst, d.lo, d.hi);
        put64(F, &d);
        put64(F, &a);
        return 1;

    case IR_BSWAP: {
        int x;
        src64(F, i->a, &a);
        dst64(F, i->dst, &d, &a);
        x = tmp_get(F, (1u << a.lo) | (1u << a.hi) | (1u << d.lo) |
                       (1u << d.hi));
        t1_rev(t, x, a.lo);
        t1_rev(t, d.lo, a.hi);
        mov(F, d.hi, x);
        tmp_put(F, x);
        v_wr64(F, i->dst, d.lo, d.hi);
        put64(F, &d);
        put64(F, &a);
        return 1;
    }

    case IR_NEG: {
        /* 0 - a: RSBS leaves C clear exactly when the low word borrowed,
         * and MOVS #0 keeps it for the SBCS of the high word. */
        src64(F, i->a, &a);
        dst64(F, i->dst, &d, &a);
        t1_negs(t, d.lo, a.lo);
        if (d.hi != a.hi) {
            t1_movs_imm(t, d.hi, 0);
            t1_alu_reg(t, T_OP_SBC, d.hi, a.hi);
        } else {
            int x = tmp_get(F, (1u << a.hi) | (1u << d.lo));
            t1_movs_imm(t, x, 0);
            t1_alu_reg(t, T_OP_SBC, x, a.hi);
            mov(F, d.hi, x);
            tmp_put(F, x);
        }
        v_wr64(F, i->dst, d.lo, d.hi);
        put64(F, &d);
        put64(F, &a);
        return 1;
    }

    case IR_MUL:
        helper2(F, i, "__aeabi_lmul", 2, 2, 2, 0);
        return 1;

    case IR_DIV: case IR_MOD:
        helper2(F, i, i->op == IR_DIV ? (i->sign ? "__divdi3" : "__udivdi3")
                                      : (i->sign ? "__moddi3" : "__umoddi3"),
                2, 2, 2, 0);
        return 1;

    case IR_SHL: case IR_SHR: {
        int op = i->op == IR_SHL ? T_SH_LSL : i->sign ? T_SH_ASR : T_SH_LSR;
        if (!i->imm_b) {
            helper2(F, i, op == T_SH_LSL ? "__aeabi_llsl"
                        : op == T_SH_ASR ? "__aeabi_lasr" : "__aeabi_llsr",
                    2, 1, 2, 0);
            return 1;
        }
        src64(F, i->a, &a);
        /* not into the operand's temporaries: the shifts read both of its
         * words after writing one of the result's */
        dst64(F, i->dst, &d, NULL);
        shift64_imm(F, op, i->sign, (long)i->imm, a.lo, a.hi, d.lo, d.hi);
        v_wr64(F, i->dst, d.lo, d.hi);
        put64(F, &d);
        put64(F, &a);
        return 1;
    }

    case IR_EXT:
        dst64(F, i->dst, &d, NULL);
        v_rd(F, i->a, d.lo);
        if (i->size < 4)
            t1_ext(t, d.lo, d.lo, i->size, i->sign);
        if (i->sign) t1_shift_imm(t, T_SH_ASR, d.hi, d.lo, 32);
        else         t1_movs_imm(t, d.hi, 0);
        v_wr64(F, i->dst, d.lo, d.hi);
        put64(F, &d);
        return 1;

    case IR_LDVAR:
        dst64(F, i->dst, &d, NULL);
        if (i->size == 8) {
            v_rd64(F, i->a, d.lo, d.hi);
        } else {
            if (in_reg6(F, i->a)) {
                if (i->size >= 4) mov(F, d.lo, F->loc[i->a]);
                else t1_ext(t, d.lo, F->loc[i->a], i->size, i->sign);
            } else {
                fr_ld(F, d.lo, tcg_slot_of(F, i->a), 4, 0);
                if (i->size < 4)
                    t1_ext(t, d.lo, d.lo, i->size, i->sign);
            }
            if (i->sign) t1_shift_imm(t, T_SH_ASR, d.hi, d.lo, 32);
            else         t1_movs_imm(t, d.hi, 0);
        }
        v_wr64(F, i->dst, d.lo, d.hi);
        put64(F, &d);
        return 1;

    case IR_STVAR:
        src64(F, i->a, &a);
        if (i->size == 8) {
            v_wr64(F, i->dst, a.lo, a.hi);
        } else if (in_reg6(F, i->dst)) {
            if (i->size >= 4) mov(F, F->loc[i->dst], a.lo);
            else t1_ext(t, F->loc[i->dst], a.lo, i->size, 0);
        } else {
            long s = tcg_slot_of(F, i->dst);
            if (fn->locals[i->dst].size <= 4) {
                fr_st(F, a.lo, s, 4, 1u << a.hi);
            } else {
                fr_st(F, a.lo, s, i->size, 1u << a.hi);
            }
        }
        put64(F, &a);
        return 1;

    case IR_LOAD: {
        long off = i->memoff;
        int own, base;
        dst64(F, i->dst, &d, NULL);
        base = mem_base(F, i->a, &off, &own, (1u << d.lo) | (1u << d.hi));
        if (i->size == 8) {
            if (d.lo == base) {
                mem_ld(F, d.hi, base, off + 4, 4, 0, i->natural);
                mem_ld(F, d.lo, base, off, 4, 0, i->natural);
            } else {
                mem_ld(F, d.lo, base, off, 4, 0, i->natural);
                mem_ld(F, d.hi, base, off + 4, 4, 0, i->natural);
            }
        } else {
            mem_ld(F, d.lo, base, off, i->size, i->sign, i->natural);
            if (i->sign) t1_shift_imm(t, T_SH_ASR, d.hi, d.lo, 32);
            else         t1_movs_imm(t, d.hi, 0);
        }
        if (own)
            tmp_put(F, base);
        v_wr64(F, i->dst, d.lo, d.hi);
        put64(F, &d);
        return 1;
    }

    case IR_STORE: {
        long off = i->memoff;
        int own, base;
        src64(F, i->b, &a);
        base = mem_base(F, i->a, &off, &own, (1u << a.lo) | (1u << a.hi));
        if (i->size == 8) {
            mem_st(F, a.lo, base, off, 4, i->natural, 1u << a.hi);
            mem_st(F, a.hi, base, off + 4, 4, i->natural, 1u << a.lo);
        } else {
            mem_st(F, a.lo, base, off, i->size, i->natural, 1u << a.hi);
        }
        if (own)
            tmp_put(F, base);
        put64(F, &a);
        return 1;
    }

    case IR_SELECT: {
        int take_c, done;
        if (i->size == 8) {
            src64(F, i->a, &a);
            {
                int x = tmp_get(F, 0);
                mov(F, x, a.lo);
                t1_alu_reg(t, T_OP_ORR, x, a.hi);
                tmp_put(F, x);
            }
            put64(F, &a);
        } else {
            t1_cmp_imm(t, v_rdr(F, i->a, S0), 0);
        }
        dst64(F, i->dst, &d, NULL);
        take_c = bc16(F, T_EQ);
        v_rd64(F, i->b, d.lo, d.hi);
        done = b16(F);
        bc16_here(F, take_c);
        v_rd64(F, i->c, d.lo, d.hi);
        b16_here(F, done);
        v_wr64(F, i->dst, d.lo, d.hi);
        put64(F, &d);
        return 1;
    }

    default:
        return 0;
    }
}

/* ---- calls ------------------------------------------------------------------ */

/* rt = `nb` (1-4) bytes of an aggregate at base + off, zero-extended: a
 * word or halfword load where the aggregate's alignment allows one, and
 * bytes where it does not. */
static void agg_word(struct t_fn *F, int rt, int base, long off, int nb,
                     int align)
{
    int nat = nb != 3 && align >= nb && (off % nb) == 0;
    mem_ld(F, rt, base, off, nb, 0, nat);
}

/* [sp + off] = rv, at the LIVE sp: the outgoing area is there whatever the
 * frame base is. */
static void sp_st(struct t_fn *F, int rv, long off, unsigned avoid)
{
    int x;
    if (t1_ldst_sp(F->t, rv, off + F->spb, 1))
        return;
    x = tmp_get(F, avoid | (1u << rv));
    k32_nf(F, x, (unsigned long)(off + F->spb));  /* after any push */
    t1_add_hi(F->t, x, T_SP);
    t1_ldst_imm(F->t, rv, x, 0, 4, 1);
    tmp_put(F, x);
}

static void gen_call(struct t_fn *F, int n)
{
    struct ir_func *fn = F->fn;
    struct ir_ins *i = &fn->ins[n];
    struct code *t = F->t;
    long sret = tcg_call_sret_bytes(i);
    struct argplace pl[MAX_PARAMS];
    struct abi_walk w;

    tcg_walk_init(&w, sret != 0, i->call_varargs, i->call_pcs);
    for (int k = 0; k < i->nargs; k++)
        tcg_place_one(&w, &i->argv[k], &pl[k]);
    /* The stack words first: writing one needs scratch registers, and once
     * r0-r3 are loaded only r6 and r7 are left. A struct's words are read
     * at its type's alignment only where irgen promises the address has it
     * (ir_arg.natural): a packed struct's member may be anywhere, and LDR
     * faults on a misaligned address here. */
    for (int k = 0; k < i->nargs; k++) {
        struct ir_arg *a = &i->argv[k];
        if (pl[k].vfp >= 0 && pl[k].nvfp)
            v6_refuse(fn, i, "a VFP argument (ARMv6-M has no FPU)");
        if (!pl[k].nstk)
            continue;
        if (a->is_struct) {
            v_rd(F, a->vreg, sc(F, S1));
            for (int q = 0; q < pl[k].nstk; q++) {
                long off = (long)(pl[k].nreg + q) * 4;
                int nb = a->size - off < 4 ? (int)(a->size - off) : 4;
                pool_point(F, 96, 0);
                F->tbusy |= 1u << S1;
                agg_word(F, sc(F, S0), S1, off, nb,
                         a->natural ? a->align : 1);
                F->tbusy &= ~(1u << S1);
                sp_st(F, S0, pl[k].stk + (long)q * 4, 1u << S1);
            }
        } else if (a->size > 4) {
            v_rd64(F, a->vreg, sc(F, S0), sc(F, S1));
            sp_st(F, S0, pl[k].stk, 1u << S1);
            sp_st(F, S1, pl[k].stk + 4, 1u << S0);
        } else {
            v_rd(F, a->vreg, sc(F, S0));
            sp_st(F, S0, pl[k].stk, 0);
        }
    }
    /* The register-resident scalars, as ONE parallel move. */
    {
        int pd[8], ps[8], npm = 0;
        for (int k = 0; k < i->nargs && npm < 8; k++) {
            struct ir_arg *a = &i->argv[k];
            if (!pl[k].nreg || a->is_struct || a->size > 8 ||
                !in_reg6(F, a->vreg))
                continue;
            for (int q = 0; q < (a->size > 4 ? 2 : 1) && npm < 8; q++) {
                pd[npm] = pl[k].reg + q;
                ps[npm] = F->loc[a->vreg] + q;
                npm++;
            }
        }
        if (npm) {
            int od[16], os[16];
            int m = ra_parallel_move(pd, ps, npm, IP, od, os, 16);
            if (m < 0)
                internal_error("thumb: %s: a call's argument setup is not a "
                               "well-formed move", fn->name);
            for (int k = 0; k < m; k++)
                t_mov_reg(t, od[k], os[k]);
        }
    }
    for (int k = 0; k < i->nargs; k++) {
        struct ir_arg *a = &i->argv[k];
        if (!pl[k].nreg)
            continue;
        if (!a->is_struct && a->size <= 8 && in_reg6(F, a->vreg))
            continue;
        if (a->is_struct) {
            v_rd(F, a->vreg, sc(F, S1));
            F->tbusy |= 1u << S1;
            for (int q = 0; q < pl[k].nreg; q++) {
                long off = (long)q * 4;
                int nb = a->size - off < 4 ? (int)(a->size - off) : 4;
                agg_word(F, pl[k].reg + q, S1, off, nb,
                         a->natural ? a->align : 1);
            }
            F->tbusy &= ~(1u << S1);
        } else if (a->size > 4) {
            v_rd64(F, a->vreg, pl[k].reg, pl[k].reg + 1);
        } else {
            v_rd(F, a->vreg, pl[k].reg);
        }
    }
    if (sret)
        fr_addr(F, T_R0, F->scratch_at + i->scratch);
    if (i->indirect && i->call_cmse) {
        /* CMSE's call into the Non-secure state (codegen.c says why each
         * step), in Thumb-1: r4-r7 pushed, then r8-r11 through them; the
         * target's bit 0 cleared in r4 (BICS takes low registers only);
         * every register but the arguments overwritten with it, and the
         * flags; BLXNS; and r8-r11, r4-r7 back. No VLSTM: Baseline has
         * no FPU. */
        tcg_cmse_check_call(fn, i, &w, sret);
        v_rd(F, i->a, sc(F, S0));
        t_mov_reg(t, IP, S0);
        t1_push(t, 0xf0u);
        for (int r = 4; r < 8; r++)
            t_mov_reg(t, r, r + 4);
        t1_push(t, 0xf0u);
        t_mov_reg(t, 4, IP);
        t1_movs_imm(t, 5, 1);
        t1_alu_reg(t, T_OP_BIC, 4, 5);
        for (int r = w.ncrn; r < 13; r++)
            if (r != 4)
                t_mov_reg(t, r, 4);
        t_msr_apsr(t, 4, 0);
        t_bxns(t, 4, 1);
        t1_pop(t, 0xf0u);
        for (int r = 4; r < 8; r++)
            t_mov_reg(t, r + 4, r);
        t1_pop(t, 0xf0u);
        if (i->dst >= 0 && !i->retsize &&
            (i->ret_tybytes == 1 || i->ret_tybytes == 2))
            t1_ext(t, T_R0, T_R0, i->ret_tybytes, i->ret_tysign);
    } else if (i->indirect) {
        v_rd(F, i->a, sc(F, S0));
        t_blx(t, S0);
    } else if (cg_call_local(fn->src, i->callee)) {
        tcg_note_call(F->st, t_bl(t), i->callee);
    } else {
        tcg_note_ext(F->st, t_bl(t), i->callee);
    }
    if (i->dst < 0)
        return;
    if (i->retsize) {
        long fo;
        if (!sret) {
            /* four bytes or fewer came back in r0: into the scratch */
            fr_addr(F, sc(F, S1), F->scratch_at + i->scratch);
            mem_st(F, T_R0, S1, 0, i->retsize == 3 ? 2 : i->retsize, 1, 0);
            if (i->retsize == 3) {
                t1_shift_imm(t, T_SH_LSR, sc(F, S0), T_R0, 16);
                t1_ldst_imm(t, S0, S1, 2, 1, 1);
            }
        }
        if (!tcg_faddr(F, i->dst, &fo)) {
            fr_addr(F, sc(F, S0), F->scratch_at + i->scratch);
            v_wr(F, i->dst, S0);
        }
    } else if (F->wide[i->dst]) {
        v_wr64(F, i->dst, T_R0, T_R1);
    } else {
        v_wr(F, i->dst, T_R0);
    }
}

/* ---- return ------------------------------------------------------------------ */

static void gen_ret(struct t_fn *F, int n)
{
    struct ir_func *fn = F->fn;
    struct ir_ins *i = &fn->ins[n];
    struct code *t = F->t;
    if (i->a >= 0 && fn->ret_abi.size && fn->ret_abi.is_struct) {
        long sz = fn->ret_abi.size;
        /* the value's address need not be aligned: a packed struct's
         * member (irgen's IR_RET natural), and LDR faults on ARMv6-M */
        int al = i->natural && fn->ret_abi.align ? fn->ret_abi.align : 1;
        if (F->sret_slot >= 0) {
            /* Copied to the buffer the caller named. r0-r3 hold nothing
             * at a return, so they carry the copy: r0 the destination, r1
             * the source, r3 the data -- and r0 is the buffer's address
             * again at the end, as the return value must be. */
            long k = 0;
            int step = al >= 4 ? 4 : al >= 2 ? 2 : 1;
            v_rd(F, i->a, T_R1);
            fr_ld(F, T_R0, F->sret_slot, 4, 0);
            if (sz > 32) {
                int top;
                long body = sz / step;
                k32(F, T_R2, (unsigned long)body);
                top = t->len;
                t1_ldst_imm(t, T_R3, T_R1, 0, step, 0);
                t1_ldst_imm(t, T_R3, T_R0, 0, step, 1);
                t1_addsub_imm8(t, T_OP_ADD, T_R1, step);
                t1_addsub_imm8(t, T_OP_ADD, T_R0, step);
                t1_addsub_imm8(t, T_OP_SUB, T_R2, 1);
                if (!t_patch_bcond16(t, t_bcond16(t, T_NE), top))
                    internal_error("thumb: %s: a copy loop", fn->name);
                k = body * step;
                for (long b = 0; k + b < sz; b++) {
                    t1_ldst_imm(t, T_R3, T_R1, b, 1, 0);
                    t1_ldst_imm(t, T_R3, T_R0, b, 1, 1);
                }
            } else {
                for (; k + step <= sz; k += step) {
                    t1_ldst_imm(t, T_R3, T_R1, k, step, 0);
                    t1_ldst_imm(t, T_R3, T_R0, k, step, 1);
                }
                for (; k < sz; k++) {
                    t1_ldst_imm(t, T_R3, T_R1, k, 1, 0);
                    t1_ldst_imm(t, T_R3, T_R0, k, 1, 1);
                }
            }
            fr_ld(F, T_R0, F->sret_slot, 4, 0);
        } else {
            /* Four bytes or fewer, in r0. A three-byte one is read as a
             * word only when it is word-aligned, so the fourth byte is
             * padding that exists. */
            int base = v_rdr(F, i->a, S1);
            int nb = (int)sz;
            if (nb == 3 && al >= 4)
                nb = 4;
            agg_word(F, T_R0, base, 0, nb, al);
        }
    } else if (i->a >= 0) {
        if (F->wide[i->a]) v_rd64(F, i->a, T_R0, T_R1);
        else               v_rd(F, i->a, T_R0);
    }
    if (n + 1 < fn->nins && !v6_invert_last(F, n, fn->nlabels))
        v6_jump_to(F, fn->nlabels);
}

/* ---- switch ------------------------------------------------------------------
 *
 * There is no TBB/TBH: the table is read with LDRB/LDRH and added to pc,
 * as clang does for thumbv6m --
 *
 *      cmp   rI, #n ; bhs default
 *      adr   rB, table
 *      ldrb  rT, [rB, rI]            (halfwords: lsls rT, rI, #1; ldrh)
 *      lsls  rT, rT, #1
 *      add   pc, rT                  (pc reads as this + 4)
 *
 * and entries of (case - (add + 4)) / 2. ADD to pc does not interwork, so
 * the entries are plain offsets. A case behind the dispatch, or one too
 * far for the entry, makes the switch a word table of (case | 1) - table
 * read with LDR and taken with BX -- the no_tbh restart, as on ARMv7-M:
 * 0 bytes, 1 halfwords, 2 words. */
static void gen_switch(struct t_fn *F, int n)
{
    struct ir_func *fn = F->fn;
    struct ir_ins *i = &fn->ins[n];
    struct code *t = F->t;
    int ne = fn->jt[i->jt].n;
    int ri = v_rdr(F, i->a, S0);
    int form = F->no_tbh ? F->no_tbh[n] : 0, rb, rt, at_add, adr_at, tab;
    if (form > 2)
        form = 2;
    if (ne <= 255) {
        t1_cmp_imm(t, ri, ne);
    } else {
        k32(F, sc(F, S1), (unsigned long)ne);
        t1_cmp_reg(t, ri, S1);
    }
    v6_jump_if(F, T_CS, i->label);              /* bhs: unsigned >= n */
    /* a table can be longer than a literal reaches */
    pool_point(F, 64 + (long)ne * 4, 0);
    for (int k = 0; k < ne; k++)
        if (F->label_off[fn->jt[i->jt].labels[k]] >= 0)
            form = 2;
    rb = sc(F, S1);
    rt = sc(F, S0);
    if (form < 2) {
        adr_at = t->len;
        t1_adr(t, rb, 0);
        if (form == 0) {
            t1_ldst_reg(t, rt, rb, ri, 1, 0, 0);
        } else {
            t1_shift_imm(t, T_SH_LSL, rt, ri, 1);
            t1_ldst_reg(t, rt, rb, rt, 2, 0, 0);
        }
        t1_shift_imm(t, T_SH_LSL, rt, rt, 1);
        at_add = t->len;
        t1_add_hi(t, T_PC, rt);
        note_pad(F, t->len);
        if (t->len & 2)
            t_nop(t);
        tab = t->len;
        if (!t1_patch_adr(t, adr_at, tab))
            internal_error("thumb: %s: a switch table out of adr's reach",
                           fn->name);
        for (int k = 0; k < ne; k++) {
            tcg_want_label(F, t->len, fn->jt[i->jt].labels[k],
                           form == 0 ? V6_TBB : V6_TBH);
            F->fix[F->nfix - 1].cz_at = at_add + 4;
            F->fix[F->nfix - 1].ins = n;
            if (form == 0) code_byte(t, 0);
            else           code_u16(t, 0);
        }
        if (t->len & 1)
            code_byte(t, 0);
        code_mark_data(t, tab, t->len);
    } else {
        t1_shift_imm(t, T_SH_LSL, rt, ri, 2);
        adr_at = t->len;
        t1_adr(t, rb, 0);
        t1_ldst_reg(t, rt, rb, rt, 4, 0, 0);
        t1_addsub_reg(t, T_OP_ADD, rt, rt, rb);
        t_bx(t, rt);
        note_pad(F, t->len);
        if (t->len & 2)
            t_nop(t);
        tab = t->len;
        if (!t1_patch_adr(t, adr_at, tab))
            internal_error("thumb: %s: a switch table out of adr's reach",
                           fn->name);
        for (int k = 0; k < ne; k++) {
            tcg_want_label(F, t->len, fn->jt[i->jt].labels[k], T_TAB);
            F->fix[F->nfix - 1].cz_at = tab;
            F->fix[F->nfix - 1].ins = n;
            code_u32(t, 0);
        }
        code_mark_data(t, tab, t->len);
    }
    F->barrier = 1;
    F->bc_end = -1;
}

/* ---- atomics ---------------------------------------------------------------
 *
 * ARMv6-M has no exclusives, so a read-modify-write or a compare-and-swap
 * is a call to the libatomic routine clang calls for this triple
 * (lib/rt/atomic_v6m.c defines them, masking interrupts). A plain atomic
 * load or store is an ordinary one between barriers, which irgen builds. */
static const char *const atomic_names[6][3] = {
    { "__atomic_exchange_1", "__atomic_exchange_2", "__atomic_exchange_4" },
    { "__atomic_fetch_add_1", "__atomic_fetch_add_2", "__atomic_fetch_add_4" },
    { "__atomic_fetch_and_1", "__atomic_fetch_and_2", "__atomic_fetch_and_4" },
    { "__atomic_fetch_or_1", "__atomic_fetch_or_2", "__atomic_fetch_or_4" },
    { "__atomic_fetch_xor_1", "__atomic_fetch_xor_2", "__atomic_fetch_xor_4" },
    { "__atomic_fetch_nand_1", "__atomic_fetch_nand_2", "__atomic_fetch_nand_4" },
};
static const char *const cas_names[2][3] = {
    { "__sync_val_compare_and_swap_1", "__sync_val_compare_and_swap_2",
      "__sync_val_compare_and_swap_4" },
    { "__atomic_compare_exchange_1", "__atomic_compare_exchange_2",
      "__atomic_compare_exchange_4" },
};

/* ARMv8-M Baseline: the exclusives, so codegen.c's loop (thumb_atomic),
 * in low registers:
 *
 *      dmb
 *   1: ldrex{b,h}  old, [addr]
 *      <new from old>
 *      strex{b,h}  st, new, [addr]
 *      cmp  st, #0
 *      bne  1b
 *      dmb
 *
 * Every operand is in a register before the loop: a load from the frame
 * between the LDREX and the STREX is a memory access the architecture
 * allows to clear the reservation. The temporaries are taken before the
 * loop too, so a pushed one is pushed outside it. */
static unsigned rbit(int r) { return 1u << r; }

static void v8b_atomic(struct t_fn *F, int n)
{
    struct ir_func *fn = F->fn;
    struct ir_ins *i = &fn->ins[n];
    struct code *t = F->t;
    int sz = i->size, addr, top, br;
    if (sz != 1 && sz != 2 && sz != 4)
        v6_refuse(fn, i, "an atomic wider than four bytes (ARMv8-M Baseline "
                         "has no doubleword exclusive; clang calls "
                         "__atomic_*_8 for these)");
#define LDX(rt) (sz == 4 ? t_ldrex(t, (rt), addr, 0) \
                         : t_ldrexbh(t, (rt), addr, sz))
#define STX(st, rt) (sz == 4 ? t_strex(t, (st), (rt), addr, 0) \
                             : t_strexbh(t, (st), (rt), addr, sz))
    addr = v_rdr(F, i->a, S0);
    if (i->op == IR_XCHG || i->op == IR_XADD || i->op == IR_ARMW) {
        int val = v_rdr(F, i->b, S1), old, st, nw;
        unsigned av = rbit(addr) | rbit(val);
        old = tmp_get(F, av);
        st = tmp_get(F, av | rbit(old));
        nw = i->op == IR_XCHG ? val
           : tmp_get(F, av | rbit(old) | rbit(st));
        if (i->op == IR_ARMW && i->imm != '&' && i->imm != '|' &&
            i->imm != '^' && i->imm != 'n')
            v6_refuse(fn, i, "an atomic read-modify-write of this operation");
        t_barrier(t, T_BAR_DMB);
        top = t->len;
        LDX(old);
        if (i->op == IR_XADD) {
            t1_addsub_reg(t, T_OP_ADD, nw, old, val);
        } else if (i->op == IR_ARMW) {
            mov(F, nw, old);
            t1_alu_reg(t, i->imm == '|' ? T_OP_ORR : i->imm == '^' ? T_OP_EOR
                                                    : T_OP_AND, nw, val);
            if (i->imm == 'n')
                t1_mvns(t, nw, nw);
        }
        STX(st, nw);
        t1_cmp_imm(t, st, 0);
        br = bc16(F, T_NE);
        if (!t_patch_bcond16(t, br, top))
            internal_error("thumb: an atomic's retry loop is out of reach");
        t_barrier(t, T_BAR_DMB);
        if (sz < 4 && i->sign)
            t1_ext(t, old, old, sz, 1);
        v_wr(F, i->dst, old);
        if (nw != val)
            tmp_put(F, nw);
        tmp_put(F, st);
        tmp_put(F, old);
    } else {
        /* compare-and-swap: IR_CAS by value, IR_CMPXCHG with the expected
         * value at *b and the value seen written back there */
        int p = -1, exp, des, old, st, fail, done;
        unsigned av = rbit(addr);
        /* the pointer first: a value with no register (a frame address,
         * read once) is built in S1, which tmp_get hands out first */
        if (i->op == IR_CMPXCHG) {
            p = v_rdr(F, i->b, S1);
            av |= rbit(p);
        }
        exp = tmp_get(F, av);
        av |= rbit(exp);
        if (i->op == IR_CMPXCHG) {
            if (p == exp)
                internal_error("thumb: %s: a compare-and-swap's registers",
                               fn->name);
            t1_ldst_imm(t, exp, p, 0, sz, 0);        /* zero-extended */
        } else {
            v_rd(F, i->b, exp);
            if (sz < 4)
                t1_ext(t, exp, exp, sz, 0);
        }
        des = tmp_get(F, av);
        av |= rbit(des);
        v_rd(F, i->c, des);
        old = tmp_get(F, av);
        av |= rbit(old);
        st = tmp_get(F, av);
        t_barrier(t, T_BAR_DMB);
        top = t->len;
        LDX(old);
        t1_cmp_reg(t, old, exp);
        fail = bc16(F, T_NE);
        STX(st, des);
        t1_cmp_imm(t, st, 0);
        br = bc16(F, T_NE);
        done = b16(F);
        if (!t_patch_bcond16(t, br, top))
            internal_error("thumb: a compare-and-swap loop is out of reach");
        bc16_here(F, fail);
        t_clrex(t);                  /* the failed path holds a reservation */
        b16_here(F, done);
        t_barrier(t, T_BAR_DMB);
        if (i->op == IR_CMPXCHG) {
            /* *b = the value seen; the result is whether it matched. The
             * temporaries go first (a pop leaves the flags alone), as
             * set_cc may need a register of its own. */
            t1_ldst_imm(t, old, p, 0, sz, 1);
            t1_cmp_reg(t, old, exp);
            tmp_put(F, st);
            tmp_put(F, old);
            tmp_put(F, des);
            tmp_put(F, exp);
            set_cc(F, i->dst, T_EQ);
        } else {
            if (sz < 4 && i->sign)
                t1_ext(t, old, old, sz, 1);
            v_wr(F, i->dst, old);
            tmp_put(F, st);
            tmp_put(F, old);
            tmp_put(F, des);
            tmp_put(F, exp);
        }
    }
#undef LDX
#undef STX
}

static void gen_atomic(struct t_fn *F, int n)
{
    struct ir_func *fn = F->fn;
    struct ir_ins *i = &fn->ins[n];
    struct code *t = F->t;
    int sz = i->size, z = sz == 1 ? 0 : sz == 2 ? 1 : 2;
    int vr[3], nw[3] = { 1, 1, 1 }, dst[3] = { 0, 1, 2 };
    long kv[3] = { 0, 0, 0 };
    const char *name;
    if (target_thumb_v8m_base()) {
        v8b_atomic(F, n);
        return;
    }
    if (sz != 1 && sz != 2 && sz != 4)
        v6_refuse(fn, i, "an atomic wider than four bytes (ARMv6-M has no "
                         "exclusives; it calls __atomic_* for these)");
    vr[0] = i->a;
    vr[1] = i->b;
    vr[2] = i->c;
    if (i->op == IR_CAS || i->op == IR_CMPXCHG) {
        call_args(F, 3, vr, nw, dst, kv);
        if (sz < 4)
            t1_ext(t, T_R2, T_R2, sz, 0);
        if (i->op == IR_CAS && sz < 4)
            t1_ext(t, T_R1, T_R1, sz, 0);
        name = cas_names[i->op == IR_CMPXCHG][z];
        if (i->op == IR_CMPXCHG) {
            /* weak = 0 in r3; both memory orders seq_cst (5), on the
             * stack, pushed right below the call so sp stays 8-aligned */
            t1_movs_imm(t, T_R3, 0);
            t1_movs_imm(t, sc(F, S0), 5);
            t1_movs_imm(t, sc(F, S1), 5);
            t1_push(t, (1u << S0) | (1u << S1));
            tcg_call_helper(F, name);
            t1_sp_adjust(t, 8, 0);
        } else {
            tcg_call_helper(F, name);
        }
    } else {
        int which = i->op == IR_XCHG ? 0 : i->op == IR_XADD ? 1
                  : i->imm == '&' ? 2 : i->imm == '|' ? 3
                  : i->imm == '^' ? 4 : i->imm == 'n' ? 5 : -1;
        if (which < 0)
            v6_refuse(fn, i, "an atomic read-modify-write of this operation");
        vr[2] = -1;
        kv[2] = 5;                               /* __ATOMIC_SEQ_CST */
        call_args(F, 3, vr, nw, dst, kv);
        if (sz < 4)
            t1_ext(t, T_R1, T_R1, sz, 0);
        tcg_call_helper(F, atomic_names[which][z]);
    }
    if (i->dst >= 0) {
        if (sz < 4 && i->sign && i->op != IR_CMPXCHG)
            t1_ext(t, T_R0, T_R0, sz, 1);
        v_wr(F, i->dst, T_R0);
    }
}

/* ---- inline asm -------------------------------------------------------------
 *
 * As on ARMv7-M (codegen.c, IR_ASM), the operands are values. Inputs, an
 * "m" output's address and a "+" output's address go into their registers
 * -- the register-resident ones as ONE parallel move, then the rest from
 * their slots -- and after the template the value outputs (ir_asm_op.val:
 * this asm's own, then its continuations') come out the same way. What the
 * asm may change is ir_asm.clob, and the allocator keeps every value live
 * across it out of exactly that (ra_target.asm_in_reg). This lowering
 * writes nothing else but r6, r7 and r12, which the allocator never hands
 * out, and registers tmp_get pushes and pops. An operand is in r0-r3 or
 * r12 (sema); r12, which only MOV and ADD reach, goes through a low
 * register. */

/* The registers an asm's operands are in, its continuations' included:
 * bit r for r. */
static unsigned asm_regs(const struct ir_func *fn, int n)
{
    const struct ir_asm *ia = fn->ins[n].asm_ir;
    unsigned m = 0;
    for (int k = 0; k < ia->nin; k++)
        m |= 1u << ia->in[k].reg;
    for (int k = 0; k < ia->nout; k++)
        m |= 1u << ia->out[k].reg;
    return m;
}

/* The moves dst[k] <- src[k] at once, `brk` breaking a cycle. */
static void asm_moves(struct t_fn *F, const int *dst, const int *src, int n,
                      int brk, const char *what)
{
    int od[64], os[64], m;
    if (!n)
        return;
    m = ra_parallel_move(dst, src, n, brk, od, os,
                         (int)(sizeof od / sizeof od[0]));
    if (m < 0)
        internal_error("thumb: %s: an asm's %s are not a well-formed move",
                       F->fn->name, what);
    for (int k = 0; k < m; k++)
        t_mov_reg(F->t, sc(F, od[k]), os[k]);
}

/* Operand register r = v, which is in memory (or a frame address). */
static void asm_in(struct t_fn *F, int v, int r, unsigned opm)
{
    int x;
    if (r < 8) {
        v_rd(F, v, sc(F, r));
        return;
    }
    x = tmp_get(F, opm);
    v_rd(F, v, x);
    t_mov_reg(F->t, r, x);
    tmp_put(F, x);
}

/* A "+" output's current value: operand register r holds its address and
 * is loaded through it. */
static void asm_cur(struct t_fn *F, int r, int size, unsigned opm)
{
    int x;
    if (r < 8) {
        mem_ld(F, r, r, 0, size, 0, 1);
        return;
    }
    x = tmp_get(F, opm);
    mov(F, x, r);
    mem_ld(F, x, x, 0, size, 0, 1);
    t_mov_reg(F->t, r, x);
    tmp_put(F, x);
}

static void gen_asm(struct t_fn *F, int n)
{
    struct ir_func *fn = F->fn;
    struct ir_ins *i = &fn->ins[n];
    struct code *t = F->t;
    struct ir_asm *ia = i->asm_ir;
    static const int brks[3] = { IP, 6, 7 };   /* (no asm with tcg_ext) */
    int vreg_[16], vdst[16], nval = 0, brk = -1;
    unsigned opm;

    /* A continuation's value was written by the asm before it, which must
     * be right there: nothing may run between an asm and the moment its
     * registers are read. */
    if (ia->cont) {
        int k = n - 1;
        while (k >= 0 && fn->ins[k].op == IR_ASM && fn->ins[k].asm_ir &&
               fn->ins[k].asm_ir->cont)
            k--;
        if (k < 0 || fn->ins[k].op != IR_ASM)
            internal_error("thumb: %s: an asm's further output is not "
                           "right after the asm", fn->name);
        return;
    }
    /* this asm's value outputs: its own, then its continuations' */
    opm = asm_regs(fn, n);
    for (int k = 0; k < ia->nout; k++)
        if (ia->out[k].val) {
            vreg_[nval] = ia->out[k].reg;
            vdst[nval++] = i->dst;
        }
    for (int q = n + 1; q < fn->nins && fn->ins[q].op == IR_ASM &&
                        fn->ins[q].asm_ir && fn->ins[q].asm_ir->cont; q++) {
        if (nval == 16)
            v6_refuse(fn, i, "an asm with more than 16 register outputs");
        vreg_[nval] = fn->ins[q].asm_ir->out[0].reg;
        vdst[nval++] = fn->ins[q].dst;
        opm |= asm_regs(fn, q);
    }
    for (int k = 0; k < ia->nout; k++)
        if (!ia->out[k].mem && ia->out[k].size > 4)
            v6_refuse(fn, i, "an asm output wider than a register");
    /* A cycle in a move is broken through a register no operand is in. */
    for (int k = 0; k < 3 && brk < 0; k++)
        if (!(opm >> brks[k] & 1))
            brk = brks[k];
    if (brk < 0)
        v6_refuse(fn, i, "an asm with operands in r6, r7 and r12 at once");

    /* In: an input's value, an "m" output's address and a "+" output's
     * address (its current value is loaded through it below). */
    {
        int pd[48], ps[48], npm = 0;
        for (int k = 0; k < ia->nin; k++)
            if (in_reg6(F, ia->in[k].temp)) {
                pd[npm] = ia->in[k].reg;
                ps[npm++] = F->loc[ia->in[k].temp];
            }
        for (int k = 0; k < ia->nout; k++)
            if (!ia->out[k].val && (ia->out[k].mem || ia->out[k].inout) &&
                in_reg6(F, ia->out[k].temp)) {
                pd[npm] = ia->out[k].reg;
                ps[npm++] = F->loc[ia->out[k].temp];
            }
        asm_moves(F, pd, ps, npm, brk, "operands");
        for (int k = 0; k < ia->nin; k++)
            if (!in_reg6(F, ia->in[k].temp))
                asm_in(F, ia->in[k].temp, ia->in[k].reg, opm);
        for (int k = 0; k < ia->nout; k++) {
            const struct ir_asm_op *o = &ia->out[k];
            if (o->val || !(o->mem || o->inout))
                continue;
            if (!in_reg6(F, o->temp))
                asm_in(F, o->temp, o->reg, opm);
            if (o->inout && !o->mem)
                asm_cur(F, o->reg, o->size, opm);
        }
    }
    {
        int base = t->len;
        for (int k = 0; k < ia->codelen; k++)
            code_byte(t, ia->code[k]);
        /* the template's data (`.short`, `.word`...): bytes v6_scan
         * must not read as instructions, and a disassembler sees as data */
        for (int k = 0; k + 1 < ia->ndrange; k += 2)
            code_mark_data(t, base + ia->drange[k], base + ia->drange[k + 1]);
    }
    /* Out, through an address: the address is live across the asm, out of
     * what it changes (regalloc.c), so a register still holds it. An "m"
     * output was written BY the template through the address its register
     * holds; storing over it would destroy what it wrote. */
    for (int k = 0; k < ia->nout; k++) {
        const struct ir_asm_op *o = &ia->out[k];
        int rv = o->reg, xv = -1, xa = -1, base;
        unsigned hold = opm;
        if (o->mem || o->val)
            continue;
        if (in_reg6(F, o->temp))
            hold |= 1u << F->loc[o->temp];
        if (rv >= 8) {
            xv = tmp_get(F, hold);
            t_mov_reg(t, xv, rv);
            rv = xv;
        }
        if (in_reg6(F, o->temp)) {
            base = F->loc[o->temp];
        } else {
            xa = tmp_get(F, hold | (1u << rv));
            v_rd(F, o->temp, xa);
            base = xa;
        }
        mem_st(F, rv, base, 0, o->size, 1, hold);
        if (xa >= 0)
            tmp_put(F, xa);
        if (xv >= 0)
            tmp_put(F, xv);
    }
    /* Out, as values: each to its home. Those in memory first, while every
     * operand register still holds what the asm left; then the
     * register-resident ones as one parallel move. */
    {
        int pd[16], ps[16], npm = 0;
        for (int k = 0; k < nval; k++) {
            int v = vdst[k], r = vreg_[k], x = -1;
            long fo;
            if (v < 0)
                continue;
            /* nothing reads it: no home to fill (and a dead value may
             * share its register with another, which no move can fill
             * twice) */
            if (F->usecnt && F->usecnt[v] == 0)
                continue;
            if (in_reg6(F, v)) {
                pd[npm] = F->loc[v];
                ps[npm++] = r;
                continue;
            }
            if (F->slot[v] < 0 || tcg_faddr(F, v, &fo))
                continue;
            (void)tcg_slot_of(F, v);
            if (r >= 8) {
                x = tmp_get(F, opm);
                t_mov_reg(t, x, r);
                r = x;
            }
            fr_st(F, r, F->slot[v], 4, opm);
            if (x >= 0)
                tmp_put(F, x);
        }
        asm_moves(F, pd, ps, npm, brk, "outputs");
    }
}

/* ---- memory blocks ---------------------------------------------------------- */

static void gen_block(struct t_fn *F, int n)
{
    struct ir_func *fn = F->fn;
    struct ir_ins *i = &fn->ins[n];
    int copy = i->op == IR_MEMCPY;
    long size = i->size, doff = 0, soff = 0;
    int dfa, sfa, db = -1, sb = -1, x, w;
    unsigned hold = 0;
    if (size > V6_INLINE_COPY) {
        int vr[3], nw[3] = { 1, 1, 1 }, dst[3] = { 0, 1, 2 };
        long kv[3] = { 0, 0, 0 };
        vr[0] = i->a;
        if (copy) {
            vr[1] = i->b;
            vr[2] = -1;
            kv[2] = size;
        } else {
            vr[1] = -1;
            kv[1] = size;
        }
        call_args(F, copy ? 3 : 2, vr, nw, dst, kv);
        tcg_call_helper(F, copy ? "__aeabi_memcpy" : "__aeabi_memclr");
        return;
    }
    dfa = tcg_faddr(F, i->a, &doff);
    sfa = copy && tcg_faddr(F, i->b, &soff);
    /* Words only where both sides are frame slots at word offsets: a
     * pointer's alignment is not known here, and ARMv6-M faults on an
     * unaligned word. */
    w = dfa && !(doff & 3) && (!copy || (sfa && !(soff & 3))) ? 4 : 1;
    if (!dfa) {
        if (in_reg6(F, i->a)) {
            db = F->loc[i->a];
        } else {
            db = tmp_get(F, 0);
            v_rd(F, i->a, db);
        }
        hold |= 1u << db;
    }
    if (copy && !sfa) {
        if (in_reg6(F, i->b)) {
            sb = F->loc[i->b];
        } else {
            sb = tmp_get(F, hold);
            v_rd(F, i->b, sb);
        }
        hold |= 1u << sb;
    }
    x = tmp_get(F, hold);
    if (!copy)
        t1_movs_imm(F->t, x, 0);
    for (long k = 0; k < size; ) {
        int c = (w == 4 && k + 4 <= size) ? 4 : 1;
        if (copy) {
            if (sfa) fr_ld(F, x, soff + k, c, 0);
            else     mem_ld(F, x, sb, k, c, 0, 1);
        }
        if (dfa) fr_st(F, x, doff + k, c, hold | (1u << x));
        else     mem_st(F, x, db, k, c, 1, hold);
        k += c;
    }
    tmp_put(F, x);
    if (copy && !sfa && !in_reg6(F, i->b))
        tmp_put(F, sb);
    if (!dfa && !in_reg6(F, i->a))
        tmp_put(F, db);
}

/* ---- one instruction ---------------------------------------------------------- */

/* d = a * b: MULS's destination is its second source. */
static void mul2(struct t_fn *F, int d, int a, int b)
{
    struct code *t = F->t;
    if (d == a) {
        t1_muls(t, d, b);
    } else if (d == b) {
        t1_muls(t, d, a);
    } else {
        mov(F, d, a);
        t1_muls(t, d, b);
    }
}

/* d = a <shift> b, by the low byte of b. */
static void shift2(struct t_fn *F, int op, int d, int a, int b)
{
    struct code *t = F->t;
    if (d == a) {
        t1_shift_reg(t, op, d, b);
    } else if (d != b) {
        mov(F, d, a);
        t1_shift_reg(t, op, d, b);
    } else {
        int x = tmp_get(F, (1u << a) | (1u << b));
        mov(F, x, a);
        t1_shift_reg(t, op, x, b);
        mov(F, d, x);
        tmp_put(F, x);
    }
}

/* d = a + v for a constant v (negative: a subtraction). */
static void add_k(struct t_fn *F, int d, int a, long v)
{
    struct code *t = F->t;
    int op = v < 0 ? T_OP_SUB : T_OP_ADD;
    unsigned long m = v < 0 ? (0UL - (unsigned long)v) & 0xffffffffUL
                            : (unsigned long)v & 0xffffffffUL;
    if (m <= 7) {
        t1_addsub_imm3(t, op, d, a, (long)m);
        return;
    }
    if (m <= 510) {
        mov(F, d, a);
        if (m > 255) {
            t1_addsub_imm8(t, op, d, 255);
            m -= 255;
        }
        t1_addsub_imm8(t, op, d, (long)m);
        return;
    }
    {
        int x = tmp_get(F, (1u << d) | (1u << a));
        k32(F, x, (unsigned long)v);
        t1_addsub_reg(t, T_OP_ADD, d, a, x);
        tmp_put(F, x);
    }
}

static int alu_op(enum ir_op op)
{
    return op == IR_AND ? T_OP_AND : op == IR_OR ? T_OP_ORR : T_OP_EOR;
}

static void gen_ins(struct t_fn *F, int n)
{
    struct ir_func *fn = F->fn;
    struct ir_ins *i = &fn->ins[n];
    struct code *t = F->t;

    /* -g: a line-table row wherever the source line changes. */
    if (target_debug_info() && i->line) {
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

    /* Floating point is a call: libgcc's names, as on ARMv7-M. */
    if (i->flt && (i->op == IR_ADD || i->op == IR_SUB || i->op == IR_MUL ||
                   i->op == IR_DIV || i->op == IR_MOD || i->op == IR_NEG ||
                   i->op == IR_CMP || i->op == IR_SQRT)) {
        const char *name = tcg_fp_binop_name(i->op, i->w);
        int ww = i->w == 8 ? 2 : 1;
        if (i->w != 4 && i->w != 8)
            v6_refuse(fn, i, "a long double (ARMv6-M has no 16-byte float)");
        if (name) {
            if (i->imm_b)
                v6_refuse(fn, i, "a folded floating-point immediate");
            /* a + b and a * b as (b, a) when that is fewer moves, as on
             * ARMv7-M (codegen.c's fp_swap_args) */
            if ((i->op == IR_ADD || i->op == IR_MUL) && i->a != i->b &&
                v6_args_cost(F, i->b, i->a, ww) <
                v6_args_cost(F, i->a, i->b, ww)) {
                struct ir_ins sw = *i;
                sw.a = i->b;
                sw.b = i->a;
                helper2(F, &sw, name, ww, ww, ww, 0);
                return;
            }
            helper2(F, i, name, ww, ww, ww, 0);
            return;
        }
        if (i->op == IR_NEG) {
            /* the sign bit, flipped: right for -0.0 and NaN too */
            if (i->w == 8) {
                struct h64 a, d;
                int x;
                src64(F, i->a, &a);
                dst64(F, i->dst, &d, &a);
                x = tmp_get(F, (1u << a.lo) | (1u << a.hi) | (1u << d.lo) |
                               (1u << d.hi));
                k32(F, x, 0x80000000UL);
                mov(F, d.lo, a.lo);
                alu2(F, T_OP_EOR, d.hi, a.hi, x, 1);
                tmp_put(F, x);
                v_wr64(F, i->dst, d.lo, d.hi);
                put64(F, &d);
                put64(F, &a);
            } else {
                int ra = v_rdr(F, i->a, S0), d = v_wreg(F, i->dst, S0);
                k32(F, sc(F, S1), 0x80000000UL);
                alu2(F, T_OP_EOR, d, ra, S1, 1);
                v_wr(F, i->dst, d);
            }
            return;
        }
        if (i->op == IR_CMP) {
            /* a < b as b > a when that is fewer moves (codegen.c's float
             * compare says why the mirror is exact) */
            enum binop pred = i->pred;
            int va = i->a, vb = i->b;
            if (!i->imm_b && va != vb &&
                v6_args_cost(F, vb, va, ww) < v6_args_cost(F, va, vb, ww)) {
                va = i->b;
                vb = i->a;
                pred = pred == B_LT ? B_GT : pred == B_GT ? B_LT
                     : pred == B_LE ? B_GE : pred == B_GE ? B_LE : pred;
            }
            int cond = tcg_cond_for(pred, 1);      /* the helper's signed answer */
            struct ir_ins *nx = n + 1 < fn->nins ? &fn->ins[n + 1] : NULL;
            int vr[2], nw[2], dst[2];
            long kv[2] = { 0, 0 };
            vr[0] = va; vr[1] = vb;
            nw[0] = nw[1] = ww;
            dst[0] = 0; dst[1] = ww == 2 ? 2 : 1;
            call_args(F, 2, vr, nw, dst, kv);
            tcg_call_helper(F, tcg_fp_cmp_name(pred, i->w));
            t1_cmp_imm(t, T_R0, 0);
            if (nx && (nx->op == IR_BRZ || nx->op == IR_BRNZ) &&
                nx->a == i->dst && nx->w != 8 && F->usecnt &&
                F->usecnt[i->dst] == 1) {
                v6_jump_if(F, nx->op == IR_BRNZ ? cond : (cond ^ 1),
                           nx->label);
                F->skip_next = 1;
                return;
            }
            set_cc(F, i->dst, cond);
            return;
        }
        if (i->op == IR_SQRT)
            v6_refuse(fn, i, "__builtin_sqrt (a libm routine here, not an "
                             "instruction)");
        v6_refuse(fn, i, "this floating-point operation");
    }

    if (i->w > 8)
        v6_refuse(fn, i, "a 128-bit value");
    /* The high word of a 64-bit value, shifted (ra_narrow_hishift). */
    if (i->op == IR_SHR && F->nshr && i->dst >= 0 && F->nshr[i->dst]) {
        int k = (int)i->imm - 32, d = v_wreg(F, i->dst, S0), hi;
        if (in_reg6(F, i->a)) {
            hi = F->loc[i->a] + 1;
        } else if (v6_rc_ok(F, i->a) &&
                   rc_find(F, F->slot[i->a] + 4) >= 0) {
            hi = rc_find(F, F->slot[i->a] + 4);
        } else {
            fr_ld(F, sc(F, S1), tcg_slot_of(F, i->a) + 4, 4, 0);
            hi = S1;
        }
        if (k)
            t1_shift_imm(t, i->sign ? T_SH_ASR : T_SH_LSR, d, hi, k);
        else
            mov(F, d, hi);
        v_wr(F, i->dst, d);
        return;
    }
    {
        int wide = i->w == 8;
        switch (i->op) {
        case IR_STVAR: wide = i->size == 8 || F->wide[i->a]; break;
        case IR_STORE: wide = i->size == 8 || F->wide[i->b]; break;
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
        /* an eight-byte atomic is refused by name there */
        if (wide && (i->op == IR_XCHG || i->op == IR_XADD ||
                     i->op == IR_ARMW || i->op == IR_CAS ||
                     i->op == IR_CMPXCHG)) {
            gen_atomic(F, n);
            return;
        }
        if (wide && i->op != IR_CMP && i->op != IR_BRZ &&
            i->op != IR_BRNZ && i->op != IR_CALL && i->op != IR_RET &&
            i->op != IR_I2F && i->op != IR_F2I && i->op != IR_F2F) {
            if (gen_ins64(F, n))
                return;
            v6_refuse(fn, i, "this operation at 64 bits");
        }
        if (wide && (i->op == IR_CMP || i->op == IR_BRZ || i->op == IR_BRNZ))
            i->w = 8;
    }

    switch (i->op) {
    case IR_LABEL:
        F->label_off[i->label] = t->len;
        rc_label(F, i->label);
        F->bc_end = -1;
        F->fl_end = -1;
        F->barrier = 0;
        return;
    case IR_JMP:
        if (n + 1 < fn->nins && fn->ins[n + 1].op == IR_LABEL &&
            fn->ins[n + 1].label == i->label)
            return;
        if (!v6_invert_last(F, n, i->label))
            v6_jump_to(F, i->label);
        return;
    case IR_CONST: {
        int d;
        if (F->remat && i->dst >= 0 && F->remat[i->dst])
            return;                     /* made where it is read (v_rd) */
        d = v_wreg(F, i->dst, S0);
        k32(F, d, (unsigned long)i->imm);
        v_wr(F, i->dst, d);
        return;
    }
    case IR_BITCAST:
    case IR_MOV:
        v_wr(F, i->dst, v_rdr(F, i->a, S0));
        return;

    case IR_ADD: case IR_SUB: {
        int ra = v_rdr(F, i->a, S0), d = v_wreg(F, i->dst, S0);
        if (i->imm_b) {
            long v = (long)(int)(unsigned)((unsigned long)i->imm & 0xffffffffUL);
            if (i->op == IR_SUB && v != -2147483647L - 1)
                v = -v;
            if (i->op == IR_SUB && v == -2147483647L - 1) {
                k32(F, sc(F, S1), 0x80000000UL);
                t1_addsub_reg(t, T_OP_SUB, d, ra, S1);
            } else {
                add_k(F, d, ra, v);
            }
        } else {
            t1_addsub_reg(t, i->op == IR_ADD ? T_OP_ADD : T_OP_SUB, d, ra,
                          v_rdr(F, i->b, S1));
        }
        v_wr(F, i->dst, d);
        return;
    }
    case IR_AND: case IR_OR: case IR_XOR: case IR_MUL: {
        /* `if (x & BIT)`: the bit shifted to bit 31 of a scratch, `lsls
         * r7, r0, #31-k; bmi` -- four bytes where the mask, the and and
         * the compare were eight; `if (x & LOWMASK)` the same with Z.
         * Only with the branch right after and nothing else reading the
         * result (codegen.c's ARMv7-M test says why N, not Z). */
        if (i->op == IR_AND && i->imm_b && n + 1 < fn->nins &&
            F->usecnt && F->usecnt[i->dst] == 1 &&
            (fn->ins[n + 1].op == IR_BRZ || fn->ins[n + 1].op == IR_BRNZ) &&
            fn->ins[n + 1].a == i->dst && fn->ins[n + 1].w == 4) {
            unsigned long mk = (unsigned long)i->imm & 0xffffffffUL;
            const struct ir_ins *bx = &fn->ins[n + 1];
            int sh = -1, onebit = 0;
            if (mk && !(mk & (mk - 1))) {
                for (sh = 31; !(mk >> (31 - sh) & 1); sh--)
                    ;
                onebit = 1;
            } else if (mk && mk != 0xffffffffUL && !(mk & (mk + 1))) {
                int m = 0;
                while (mk >> m) m++;
                sh = 32 - m;
            }
            if (sh >= 0) {
                int ra = v_rdr(F, i->a, S0);
                if (sh == 0)
                    t1_cmp_imm(t, ra, 0);
                else
                    t1_shift_imm(t, T_SH_LSL, sc(F, S1), ra, sh);
                v6_jump_if(F, onebit ? (bx->op == IR_BRZ ? T_PL : T_MI)
                                     : (bx->op == IR_BRZ ? T_EQ : T_NE),
                           bx->label);
                F->skip_next = 1;
                return;
            }
        }
        int ra = v_rdr(F, i->a, S0), d = v_wreg(F, i->dst, S0), rb;
        if (i->imm_b) {
            unsigned long c = (unsigned long)i->imm & 0xffffffffUL;
            if (i->op == IR_AND && (c == 0xff || c == 0xffff)) {
                t1_ext(t, d, ra, c == 0xff ? 1 : 2, 0);
                v_wr(F, i->dst, d);
                return;
            }
            k32(F, sc(F, S1), c);
            rb = S1;
        } else {
            rb = v_rdr(F, i->b, S1);
        }
        if (i->op == IR_MUL)
            mul2(F, d, ra, rb);
        else
            alu2(F, alu_op(i->op), d, ra, rb, 1);
        v_wr(F, i->dst, d);
        return;
    }
    case IR_DIV: case IR_MOD:
        /* ARMv8-M Baseline: SDIV/UDIV, and a remainder as clang makes it
         * there, a - (a / b) * b, with MULS and SUBS -- no MLS. */
        if (target_thumb_v8m_base()) {
            int ra = v_rdr(F, i->a, S0), rb, d, q;
            if (i->imm_b) {
                k32(F, sc(F, S1), (unsigned long)i->imm);
                rb = S1;
            } else {
                rb = v_rdr(F, i->b, S1);
            }
            d = v_wreg(F, i->dst, S0);
            if (i->op == IR_DIV) {
                t_div(t, d, ra, rb, i->sign);
            } else {
                q = tmp_get(F, rbit(ra) | rbit(rb) | rbit(d));
                t_div(t, q, ra, rb, i->sign);
                t1_muls(t, q, rb);
                t1_addsub_reg(t, T_OP_SUB, d, ra, q);
                tmp_put(F, q);
            }
            v_wr(F, i->dst, d);
            return;
        }
        /* No divide instruction: the RTABI routines. __aeabi_idivmod
         * leaves the quotient in r0 and the remainder in r1. */
        helper2(F, i, i->op == IR_DIV
                      ? (i->sign ? "__aeabi_idiv" : "__aeabi_uidiv")
                      : (i->sign ? "__aeabi_idivmod" : "__aeabi_uidivmod"),
                1, 1, 1, i->op == IR_MOD);
        return;
    case IR_SHL: case IR_SHR: {
        int sh = i->op == IR_SHL ? T_SH_LSL : i->sign ? T_SH_ASR : T_SH_LSR;
        int ra = v_rdr(F, i->a, S0), d = v_wreg(F, i->dst, S0);
        if (i->imm_b && i->imm >= 0 && i->imm < 32) {
            if (i->imm == 0) mov(F, d, ra);
            else t1_shift_imm(t, sh, d, ra, (int)i->imm);
        } else {
            int rb;
            if (i->imm_b) {
                k32(F, sc(F, S1), (unsigned long)i->imm);
                rb = S1;
            } else {
                rb = v_rdr(F, i->b, S1);
            }
            shift2(F, sh, d, ra, rb);
        }
        v_wr(F, i->dst, d);
        return;
    }
    case IR_NEG: {
        int ra = v_rdr(F, i->a, S0), d = v_wreg(F, i->dst, S0);
        t1_negs(t, d, ra);
        v_wr(F, i->dst, d);
        return;
    }
    case IR_BNOT: {
        int ra = v_rdr(F, i->a, S0), d = v_wreg(F, i->dst, S0);
        t1_mvns(t, d, ra);
        v_wr(F, i->dst, d);
        return;
    }
    case IR_BSWAP: {
        int ra = v_rdr(F, i->a, S0), d = v_wreg(F, i->dst, S0);
        if (i->size == 2) {
            t1_rev16(t, d, ra);
            t1_ext(t, d, d, 2, 0);
        } else {
            t1_rev(t, d, ra);
        }
        v_wr(F, i->dst, d);
        return;
    }
    case IR_EXT: {
        int ra = v_rdr(F, i->a, S0), d = v_wreg(F, i->dst, S0);
        if (i->size < 4) t1_ext(t, d, ra, i->size, i->sign);
        else             mov(F, d, ra);
        v_wr(F, i->dst, d);
        return;
    }

    case IR_CMP: {
        int cond = tcg_cond_for(i->pred, i->sign);
        struct ir_ins *nx = n + 1 < fn->nins ? &fn->ins[n + 1] : NULL;
        int fuse = nx && (nx->op == IR_BRZ || nx->op == IR_BRNZ) &&
                   nx->a == i->dst && nx->w != 8 &&
                   F->usecnt && F->usecnt[i->dst] == 1;
        if (i->w == 8)
            cond = cmp64(F, i, i->pred, i->sign);
        else
            cmp32(F, i);
        if (fuse) {
            v6_jump_if(F, nx->op == IR_BRNZ ? cond : (cond ^ 1), nx->label);
            F->skip_next = 1;
            /* what cmp32 may reuse: a register against an immediate */
            F->fl_end = -1;
            if (i->imm_b && in_reg6(F, i->a)) {
                F->fl_end = t->len;
                F->fl_reg = F->loc[i->a];
                F->fl_imm = i->imm;
            }
            return;
        }
        set_cc(F, i->dst, cond);
        return;
    }
    case IR_SELECT: {
        int take_c, done, d;
        if (i->size == 8) {
            struct h64 a;
            int x;
            src64(F, i->a, &a);
            x = tmp_get(F, 0);
            mov(F, x, a.lo);
            t1_alu_reg(t, T_OP_ORR, x, a.hi);
            tmp_put(F, x);
            put64(F, &a);
        } else {
            t1_cmp_imm(t, v_rdr(F, i->a, S0), 0);
        }
        d = v_wreg(F, i->dst, S0);
        take_c = bc16(F, T_EQ);
        v_rd(F, i->b, d);
        done = b16(F);
        bc16_here(F, take_c);
        v_rd(F, i->c, d);
        b16_here(F, done);
        v_wr(F, i->dst, d);
        return;
    }
    case IR_BRZ: case IR_BRNZ:
        if (i->w == 8) {
            struct h64 a;
            int x;
            src64(F, i->a, &a);
            x = tmp_get(F, 0);
            mov(F, x, a.lo);
            t1_alu_reg(t, T_OP_ORR, x, a.hi);
            tmp_put(F, x);
            put64(F, &a);
        } else {
            t1_cmp_imm(t, v_rdr(F, i->a, S0), 0);
        }
        v6_jump_if(F, i->op == IR_BRZ ? T_EQ : T_NE, i->label);
        return;

    /* A narrow local is read as its whole word and extended, and written
     * as a word when the store covers the local: its slot is four bytes
     * and word-aligned (layout), and sp has no byte or halfword form. */
    case IR_LDVAR: {
        int d;
        if (i->w > 4) v6_refuse(fn, i, "a 64-bit local");
        d = v_wreg(F, i->dst, S0);
        if (in_reg6(F, i->a)) {
            if (i->size >= 4) mov(F, d, F->loc[i->a]);
            else t1_ext(t, d, F->loc[i->a], i->size, i->sign);
        } else if (v6_rc_ok(F, i->a)) {
            int c = rc_find(F, F->slot[i->a]);
            if (c >= 0 && i->size < 4) {
                t1_ext(t, d, c, i->size, i->sign);
            } else {
                slot_rd(F, d, F->slot[i->a]);
                if (i->size < 4)
                    t1_ext(t, d, d, i->size, i->sign);
            }
        } else {
            fr_ld(F, d, tcg_slot_of(F, i->a), 4, 0);
            if (i->size < 4)
                t1_ext(t, d, d, i->size, i->sign);
        }
        v_wr(F, i->dst, d);
        return;
    }
    case IR_STVAR: {
        int src;
        if (i->w > 4) v6_refuse(fn, i, "a 64-bit local");
        src = v_rdr(F, i->a, S0);
        if (in_reg6(F, i->dst)) {
            if (i->size >= 4) mov(F, F->loc[i->dst], src);
            else t1_ext(t, F->loc[i->dst], src, i->size, 0);
        } else {
            long s = tcg_slot_of(F, i->dst);
            int whole = i->dst >= fn->nvars ||
                        i->size >= fn->locals[i->dst].size;
            fr_st(F, src, s, whole ? 4 : i->size, 0);
            if (whole && v6_rc_ok(F, i->dst))
                rc_note(F, src, s);
        }
        return;
    }
    case IR_LOAD: {
        long fo = 0, off;
        int fa = tcg_faddr(F, i->a, &fo), d;
        if (i->w > 4) v6_refuse(fn, i, "a 64-bit load");
        off = i->memoff + fo;
        d = v_wreg(F, i->dst, S0);
        if (fa) {
            if (i->natural || i->size == 1) {
                fr_ld(F, d, off, i->size, i->sign);
            } else {
                int x = tmp_get(F, 1u << d);
                fr_addr(F, x, off);
                mem_ld(F, d, x, 0, i->size, i->sign, 0);
                tmp_put(F, x);
            }
        } else {
            int an = v_rdr(F, i->a, S1);
            mem_ld(F, d, an, off, i->size, i->sign, i->natural);
        }
        v_wr(F, i->dst, d);
        return;
    }
    case IR_STORE: {
        long fo = 0, off;
        int fa = tcg_faddr(F, i->a, &fo);
        if (i->w > 4) v6_refuse(fn, i, "a 64-bit store");
        off = i->memoff + fo;
        if (fa) {
            int v = v_rdr(F, i->b, S0);
            if (i->natural || i->size == 1) {
                fr_st(F, v, off, i->size, 0);
            } else {
                int x = tmp_get(F, 1u << v);
                fr_addr(F, x, off);
                mem_st(F, v, x, 0, i->size, 0, 0);
                tmp_put(F, x);
            }
        } else {
            int an = v_rdr(F, i->a, S1), v = v_rdr(F, i->b, S0);
            mem_st(F, v, an, off, i->size, i->natural, 0);
        }
        return;
    }

    case IR_FRAMEADDR: {
        /* Level 0 only (irgen), as the Thumb-2 backend: the stack pointer
         * at entry, above the push (and r0-r3's, if variadic); the return
         * address in lr still where nothing was pushed, else the pushed
         * lr, the push's highest word. */
        int d;
        if (i->dst < 0)
            return;
        d = v_wreg(F, i->dst, S0);
        if (i->imm == 2 && F->nopush)
            t_mov_reg(t, d, T_LR);
        else if (i->imm == 2)
            fr_ld(F, d, F->entry_off - 4, 4, 0);
        else
            fr_addr(F, d, F->entry_off + (fn->is_varargs ? 16 : 0));
        v_wr(F, i->dst, d);
        return;
    }
    case IR_ADDR: {
        long fo;
        int d;
        if (tcg_faddr(F, i->dst, &fo))
            return;                     /* recomputed where it is read */
        d = v_wreg(F, i->dst, S0);
        fr_addr(F, d, tcg_slot_of(F, i->a));
        v_wr(F, i->dst, d);
        return;
    }
    case IR_STRADDR: case IR_GADDR: case IR_FADDR: {
        int d = v_wreg(F, i->dst, S0);
        if (i->op == IR_STRADDR)
            lit_load(F, d, LIT_STR, (unsigned long)i->label, NULL);
        else if (i->op == IR_GADDR)
            lit_load(F, d, LIT_GLOB, 0, i->glob);
        else
            lit_load(F, d, LIT_FN, 0, i->callee);
        v_wr(F, i->dst, d);
        return;
    }

    case IR_MEMCPY: case IR_MEMZERO:
        gen_block(F, n);
        return;
    case IR_CALL:
        gen_call(F, n);
        return;
    case IR_RET:
        gen_ret(F, n);
        return;
    case IR_UD2:
        t1_udf(t, 0);
        F->barrier = 1;
        F->bc_end = -1;
        return;
    case IR_FENCE:
        t_barrier(t, T_BAR_DMB);
        return;

    case IR_I2F: {
        /* size/sign: the integer source; w: the float result */
        int vr[1], nw[1], dst[1] = { 0 };
        long kv[1] = { 0 };
        vr[0] = i->a;
        nw[0] = i->size == 8 && F->wide[i->a] ? 2 : 1;
        call_args(F, 1, vr, nw, dst, kv);
        if (i->size == 8 && !F->wide[i->a])
            t1_movs_imm(t, T_R1, 0);
        tcg_call_helper(F, i->size == 8
                    ? (i->sign ? (i->w == 8 ? "__floatdidf" : "__floatdisf")
                               : (i->w == 8 ? "__floatundidf" : "__floatundisf"))
                    : (i->sign ? (i->w == 8 ? "__floatsidf" : "__floatsisf")
                               : (i->w == 8 ? "__floatunsidf" : "__floatunsisf")));
        if (i->w == 8) v_wr64(F, i->dst, T_R0, T_R1);
        else           v_wr(F, i->dst, T_R0);
        return;
    }
    case IR_F2I: {
        int vr[1], nw[1], dst[1] = { 0 };
        long kv[1] = { 0 };
        vr[0] = i->a;
        nw[0] = i->size == 8 ? 2 : 1;
        call_args(F, 1, vr, nw, dst, kv);
        tcg_call_helper(F, i->size == 8
                    ? (i->w == 8 ? (i->sign ? "__fixdfdi" : "__fixunsdfdi")
                                 : (i->sign ? "__fixdfsi" : "__fixunsdfsi"))
                    : (i->w == 8 ? (i->sign ? "__fixsfdi" : "__fixunssfdi")
                                 : (i->sign ? "__fixsfsi" : "__fixunssfsi")));
        if (i->w == 8) v_wr64(F, i->dst, T_R0, T_R1);
        else           v_wr(F, i->dst, T_R0);
        return;
    }
    case IR_F2F: {
        int vr[1], nw[1], dst[1] = { 0 };
        long kv[1] = { 0 };
        if (i->size == i->w) {
            if (i->w == 8) {
                struct h64 a;
                src64(F, i->a, &a);
                v_wr64(F, i->dst, a.lo, a.hi);
                put64(F, &a);
            } else {
                v_wr(F, i->dst, v_rdr(F, i->a, S0));
            }
            return;
        }
        vr[0] = i->a;
        nw[0] = i->size == 8 ? 2 : 1;
        call_args(F, 1, vr, nw, dst, kv);
        tcg_call_helper(F, i->size == 4 ? "__extendsfdf2" : "__truncdfsf2");
        if (i->w == 8) v_wr64(F, i->dst, T_R0, T_R1);
        else           v_wr(F, i->dst, T_R0);
        return;
    }
    case IR_SQRT:
        v6_refuse(fn, i, "__builtin_sqrt (a libm routine here, not an "
                         "instruction)");
        return;
    case IR_ASM:
        gen_asm(F, n);
        rc_reset();
        return;
    case IR_VA_START: {
        int a = v_rdr(F, i->a, S1);
        fr_addr(F, sc(F, S0), F->va_first);
        t1_ldst_imm(t, S0, a, 0, 4, 1);
        return;
    }
    case IR_ALLOCA: {
        /* sp -= round8(size); the block sits above the outgoing area,
         * which moves down with sp. The frame is addressed from r5. */
        int d;
        v_rd(F, i->a, sc(F, S0));
        t1_addsub_imm8(t, T_OP_ADD, S0, 7);
        t1_shift_imm(t, T_SH_LSR, S0, S0, 3);
        t1_shift_imm(t, T_SH_LSL, S0, S0, 3);
        t1_negs(t, S0, S0);
        t1_add_hi(t, T_SP, S0);
        d = v_wreg(F, i->dst, S0);
        if (!t1_add_sp_imm(t, d, F->out_bytes)) {
            k32_nf(F, d, (unsigned long)F->out_bytes);
            t1_add_hi(t, d, T_SP);
        }
        v_wr(F, i->dst, d);
        return;
    }
    case IR_SPSAVE: {
        int d = v_wreg(F, i->dst, S0);
        t_mov_reg(t, d, T_SP);
        v_wr(F, i->dst, d);
        return;
    }
    case IR_SPRESTORE:
        t_mov_reg(t, T_SP, v_rdr(F, i->a, S0));
        return;
    case IR_SWITCH:
        gen_switch(F, n);
        return;
    case IR_XCHG: case IR_XADD: case IR_ARMW: case IR_CAS: case IR_CMPXCHG:
        gen_atomic(F, n);
        return;
    case IR_LANDING:
        v6_refuse(fn, i, "an exception landing pad");
        return;
    case IR_LABELADDR: {
        if (cg_label_mark(i))       /* static data's marker: no code */
            return;
        /* &&label, PC-relative and with no relocation: a pool word of
         * the label's distance from the pc an `add` reads, Thumb bit
         * included --
         *     ldr rD, =(label | 1) - (1f + 4) ; 1: add rD, pc
         * -- as the word jump table makes the same sum for bx. */
        int d = v_wreg(F, i->dst, S0), k = F->nlrel++;
        if (k * 2 + 2 > F->caplrel) {
            F->caplrel = F->caplrel ? F->caplrel * 2 : 16;
            F->lrel = xrealloc(F->lrel, (size_t)F->caplrel * sizeof *F->lrel);
        }
        lit_load(F, d, LIT_LREL, (unsigned long)k, NULL);
        F->lrel[2 * k] = i->label;
        F->lrel[2 * k + 1] = t->len;
        t1_add_hi(t, d, T_PC);
        v_wr(F, i->dst, d);
        return;
    }
    case IR_IGOTO:
        t_bx(t, v_rdr(F, i->a, S0));
        return;
    case IR_CAS16:
        v6_refuse(fn, i, "a 16-byte atomic");
        return;
    default:
        v6_refuse(fn, i, "this operation");
        return;
    }
}

/* ---- islands --------------------------------------------------------------
 *
 * The most bytes one IR instruction can emit, generously: the pool must be
 * placed before the code could carry a pending load out of reach. */
static long v6_est(const struct t_fn *F, int n)
{
    const struct ir_ins *i = &F->fn->ins[n];
    switch (i->op) {
    case IR_SWITCH:
        return 64 + 4L * F->fn->jt[i->jt].n;
    case IR_CALL: {
        long e = 96;
        for (int k = 0; k < i->nargs; k++)
            e += i->argv[k].is_struct ? 48 + 40L * ((i->argv[k].size + 3) / 4)
                                      : 32;
        return e;
    }
    case IR_ASM: {
        /* a continuation emits nothing; its asm moves its output */
        const struct ir_asm *ia = i->asm_ir;
        long e = 96 + ia->codelen + 32L * (ia->nin + ia->nout);
        if (ia->cont)
            return 0;
        for (int q = n + 1; q < F->fn->nins && F->fn->ins[q].op == IR_ASM &&
                            F->fn->ins[q].asm_ir &&
                            F->fn->ins[q].asm_ir->cont; q++)
            e += 32;
        return e;
    }
    default:
        return 200;
    }
}

/* A POOL POINT: a place the pool may go. Each IR instruction starts at
 * one, and so does each word of a long copy, because a copy can run longer
 * than a literal reaches. `est` bounds the code until the next point.
 *
 * The first pass of a layout decides, at each point in turn, whether the
 * code to come could carry the first pending load out of reach (then an
 * island, with a branch around it) or -- at an instruction boundary after
 * an unconditional transfer, `natural` -- whether the pool is half way
 * there (an island needing no branch). Later passes replay the decisions
 * by the points' order, which the IR fixes: the code between a load and
 * its pool only shrinks from pass to pass, and literals can only drop out
 * (an offset that comes into reach), so a pool never moves away from a
 * load. */
static void pool_point(struct t_fn *F, long est, int natural)
{
    int k = F->npoint++;
    long first, dead;
    if (k >= F->capisl) {
        int old = F->capisl;
        F->capisl = F->capisl ? F->capisl * 2 : 64;
        while (F->capisl <= k)
            F->capisl *= 2;
        F->isl = xrealloc(F->isl, (size_t)F->capisl);
        memset(F->isl + old, 0, (size_t)(F->capisl - old));
    }
    if (F->isl_replay) {
        if (F->isl[k])
            pool_dump(F, F->isl[k] == 1);
        return;
    }
    F->isl[k] = 0;
    if (!F->nlsite)
        return;
    first = ((long)F->lsite[0].at + 4) & ~3L;
    dead = first + 1020;
    if (F->t->len + est + 4 + 2 + 4L * F->nlit + 16 > dead) {
        F->isl[k] = 1;
        pool_dump(F, 1);
    } else if (natural && F->barrier &&
               F->t->len + 4L * F->nlit + 600 > dead) {
        F->isl[k] = 2;
        pool_dump(F, 0);
    }
}

/* ---- the scan --------------------------------------------------------------
 *
 * Every instruction of the finished function, decoded: a 32-bit one that is
 * not BL, MRS, MSR, DMB, DSB or ISB, an IT block, or a CBZ/CBNZ would be
 * UNDEFINED on the core. A lowering that reached an ARMv7-M encoder, or an
 * inline asm template that used Thumb-2, stops the function here by name
 * rather than at a HardFault.
 *
 * ARMv8-M Baseline has CBZ/CBNZ and more 32-bit ones as well, each matched
 * on its fixed bits by t_thumb1_ok32 (emit.c), which the assembler asks
 * too; tests/golden/thumbv8mbase-encoding.sh checks that set against
 * llvm-mc. */
static int v6_ok32(unsigned h, unsigned h2)
{
    return t_thumb1_ok32(h, h2, target_thumb_v8m_base());
}

static void v6_scan(struct t_fn *F, int from)
{
    struct code *t = F->t;
    int at = from;
    while (at + 1 < t->len) {
        unsigned h = (unsigned)(t->p[at] | t->p[at + 1] << 8);
        int data = 0;
        for (int k = 0; k + 1 < t->ndrange; k += 2)
            if (at >= t->drange[k] && at < t->drange[k + 1]) {
                at = t->drange[k + 1];
                data = 1;
                break;
            }
        if (data)
            continue;
        if ((h >> 11) >= 0x1d) {
            unsigned h2 = at + 3 < t->len
                ? (unsigned)(t->p[at + 2] | t->p[at + 3] << 8) : 0;
            if (!v6_ok32(h, h2))
                v6_refuse(F->fn, NULL, target_thumb_v8m_base()
                          ? "an instruction ARMv8-M Baseline does not have "
                            "(a 32-bit Thumb-2 encoding, from inline asm or "
                            "the backend)"
                          : "an instruction ARMv6-M does not have "
                            "(a 32-bit Thumb-2 encoding, from inline asm or "
                            "the backend)");
            at += 4;
            continue;
        }
        if ((h & 0xff00u) == 0xbf00u && (h & 0xfu))       /* IT */
            v6_refuse(F->fn, NULL, target_thumb_v8m_base()
                      ? "an IT block, which ARMv8-M Baseline does not have"
                      : "an IT block or CBZ, which ARMv6-M does not have");
        if ((h & 0xf500u) == 0xb100u && !target_thumb_v8m_base())  /* CBZ */
            v6_refuse(F->fn, NULL, "an IT block or CBZ, which ARMv6-M does "
                      "not have");
        at += 2;
    }
}

/* ---- one function -------------------------------------------------------------- */

static unsigned save_mask6(const struct t_fn *F, unsigned scr)
{
    unsigned m = (scr & SCR_SET) | (1u << T_LR);
    int c = 0;
    for (int k = 0; k < F->nsave; k++)
        m |= 1u << F->used_callee[k];
    for (unsigned b = m; b; b &= b - 1)
        c++;
    if (c & 1)
        m |= 1u << 3;                /* r3: sp stays eight-aligned */
    return m;
}

static long mask_bytes(unsigned m)
{
    long n = 0;
    for (; m; m &= m - 1)
        n += 4;
    return n;
}

/* sp -= frame (sub) or += frame, at most 508 a step; a long frame through
 * r6, which such a function always saves (see v6_gen_func). */
static void sp_frame(struct t_fn *F, long frame, int sub)
{
    struct code *t = F->t;
    if (frame <= 1016) {
        while (frame > 0) {
            long s = frame > 508 ? 508 : frame;
            t1_sp_adjust(t, s, sub);
            frame -= s;
        }
        return;
    }
    k32_nf(F, sc(F, 6), sub ? (0UL - (unsigned long)frame) : (unsigned long)frame);
    t1_add_hi(t, T_SP, 6);
}

static void ins_mask_of(int v, void *ctx)
{
    struct t_fn *F = ctx;
    if (in_reg6(F, v)) {
        F->ins_mask |= 1u << F->loc[v];
        if (F->wide[v])
            F->ins_mask |= 2u << F->loc[v];
    }
}

/* The scratch pair for instruction n when r6/r7 are homes (tcg_ext): two
 * low registers holding no value live into or out of n..n+2 and none it
 * reads or writes (tcg_busy -- the span covers what its lowering emits
 * ahead, skip_next). r0-r3 only for an instruction whose lowering names no
 * low register of its own (tcg_lo_op_ok: a call, a helper, a 64-bit value
 * put arguments and pairs in fixed ones); r4-r7 for any, the ones the
 * prologue already saves first -- another is saved for it, as r6/r7 are
 * (sc). Never the frame base. */
static void ins_mask_of(int v, void *ctx);
static void ins_mask_of_ext(int v, void *ctx) { ins_mask_of(v, ctx); }

/* A call whose lowering (gen_call) uses its scratch only for the stack
 * arguments, which it stores before any argument register is loaded: no
 * composite argument in registers (read through S1 after the move), no
 * indirect target (read through S0 after it), no composite result (the
 * scratch after the call). Then r0-r3 that hold nothing at the call serve
 * as well as r6/r7. */
static int v6_call_lo_ok(const struct ir_ins *i)
{
    if (i->op != IR_CALL || i->indirect || i->retsize)
        return 0;
    for (int k = 0; k < i->nargs; k++)
        if (i->argv[k].is_struct)
            return 0;
    return 1;
}

static void v6_roles(struct t_fn *F, int n)
{
    unsigned busy = F->lv_busy ? tcg_busy(F, n, 2) : 0xffu;
    unsigned saved = 0;            /* by the allocator: the same every pass */
    int cand[8], nc = 0;
    if (F->fn->has_alloca)
        busy |= 1u << FB6;
    for (int k = 0; k < F->nsave; k++)
        saved |= 1u << F->used_callee[k];
    if (tcg_lo_op_ok(F, &F->fn->ins[n]) || v6_call_lo_ok(&F->fn->ins[n]))
        for (int r = 0; r < 4; r++)
            if (!(busy >> r & 1))
                cand[nc++] = r;
    for (int pass = 0; pass < 2; pass++)
        for (int r = 7; r >= 4; r--)
            if (!(busy >> r & 1) && ((saved >> r & 1) != 0) == (pass == 0))
                cand[nc++] = r;
    g6_s0 = nc > 0 ? cand[0] : -1;
    g6_s1 = nc > 1 ? cand[1] : -1;
}

/* No free register for a role: for a plain computation (tcg_lo_op_ok, and
 * neither a branch nor followed by one its lowering might fuse), one that
 * nothing it reads or writes is in, pushed before it and popped after --
 * as tmp_get does, sp-relative offsets adding F->spb meanwhile. Not
 * around a call (its stack arguments are at sp, and sp must stay
 * eight-aligned), nor a branch (the pop would not run where it goes).
 * Returns the registers pushed; 0 when the attempt is better failed. */
static unsigned v6_roles_push(struct t_fn *F, int n)
{
    const struct ir_func *fn = F->fn;
    unsigned ops = 0, pushed = 0;
    if (g6_s0 >= 0 && g6_s1 >= 0)
        return 0;
    for (int m = n; m <= n + 2 && m < fn->nins; m++) {
        enum ir_op op = fn->ins[m].op;
        if (op == IR_BRZ || op == IR_BRNZ || op == IR_JMP || op == IR_RET ||
            op == IR_SWITCH || op == IR_IGOTO || op == IR_LABEL ||
            op == IR_MEMCPY || op == IR_MEMZERO || op == IR_CALL ||
            op == IR_UD2)
            return 0;
    }
    if (!tcg_lo_op_ok(F, &fn->ins[n]))
        return 0;
    {
        /* the registers of the values n..n+2 name: not liveness, only
         * what the lowering reads and writes */
        unsigned keep = F->ins_mask;
        F->ins_mask = 0;
        for (int m = n; m <= n + 2 && m < fn->nins; m++) {
            ra_each_use(&fn->ins[m], ins_mask_of_ext, F);
            ins_mask_of_ext(fn->ins[m].dst, F);
        }
        ops = F->ins_mask;
        F->ins_mask = keep;
    }
    if (fn->has_alloca)
        ops |= 1u << FB6;
    for (int r = 7; r >= 0 && (g6_s0 < 0 || g6_s1 < 0); r--) {
        if ((ops >> r & 1) || r == g6_s0 || r == g6_s1)
            continue;
        pushed |= 1u << r;
        if (g6_s0 < 0) g6_s0 = r; else g6_s1 = r;
    }
    if (g6_s0 < 0 || g6_s1 < 0)
        return 0;
    t1_push(F->t, pushed);
    for (unsigned b = pushed; b; b &= b - 1)
        F->spb += 4;
    return pushed;
}

/* -g: the prologue as call frame information, as t_record_cfi does it
 * for ARMv7-M: the variadic save area, the push (low registers and lr,
 * the lowest at the lowest address), the frame's sub sp, and r5 as the
 * frame base where sp moves. */
static void v6_record_cfi(struct t_fn *F, long code_off)
{
    struct ir_func *fn = F->fn;
    long cfa = 0;
    fn->ncfi = 0;
    if (F->cfi_va_end >= 0) {
        cfa += 16;
        ir_cfi_add(fn, (int)(F->cfi_va_end - code_off), IR_CFI_CFA_OFFSET, 0, cfa);
    }
    if (F->cfi_push_end >= 0) {
        int at = (int)(F->cfi_push_end - code_off), n = 0, k = 0;
        for (int r = 0; r < 16; r++)
            n += (F->cfi_push_mask >> r) & 1;
        cfa += 4L * n;
        ir_cfi_add(fn, at, IR_CFI_CFA_OFFSET, 0, cfa);
        for (int r = 0; r < 16; r++)
            if ((F->cfi_push_mask >> r) & 1)
                ir_cfi_add(fn, at, IR_CFI_SAVED, r, -cfa + 4L * k++);
    }
    if (F->cfi_frame_end >= 0) {
        cfa += F->frame;
        ir_cfi_add(fn, (int)(F->cfi_frame_end - code_off), IR_CFI_CFA_OFFSET, 0, cfa);
    }
    if (F->cfi_fp_end >= 0)
        ir_cfi_add(fn, (int)(F->cfi_fp_end - code_off), IR_CFI_CFA_REG, FB6, cfa);
}

void v6_gen_func(struct ir_func *fn, struct code *t, struct t_sites *st,
                 int keep_vars)
{
    struct func *f = fn->src;
    struct t_fn F;
    int i;
    char *cls = NULL;
    int ncls = 0;

    memset(&F, 0, sizeof F);
    F.fn = fn; F.t = t; F.st = st;
    F.keep_vars = keep_vars;
    fn->nlines = 0;                 /* -g: this attempt's rows only */
    F.wide = tcg_wide64_map(fn);
    tcg_frame_addr_map(&F);
    F.nshr = ra_narrow_hishift(fn);
    for (int v = 0; v < fn->nvregs; v++)
        if (F.nshr[v]) F.wide[v] = 0;
    F.va_regsave = F.va_first = -1;
    F.bc_end = F.bc_fix = -1;
    F.scr_save = SCR_SET;
    F.fb = T_SP;
    g6_s0 = 6; g6_s1 = 7;
    if (tcg_regalloc()) {
        /* -g, and -O0: every source variable in its slot (codegen.c's
         * g_t_o0), the temporaries in registers */
        char *pin = keep_vars || tcg_o0() ? ra_debug_pin_vars(fn)
                                           : (char *)0;
        int pused[RA_MAXPOOL], npused = 0;
        int *pair = tcg_pair_alloc(fn, F.wide, pin, pused, &npused);
        char *fx = tcg_faddr_excl(&F, pin);
        F.loc = ra_allocate(fn, tcg_ra(), F.wide, fx ? fx : pin, F.used_callee,
                            &F.nsave);
        free(fx);
        tcg_reset_taken();
        if (pair) {
            for (int v = 0; v < fn->nvregs; v++)
                if (pair[v] >= 0) F.loc[v] = pair[v];
            for (int k = 0; k < npused; k++) {
                F.used_callee[F.nsave++] = pused[k];
                F.used_callee[F.nsave++] = pused[k] + 1;
            }
            free(pair);
        }
        free(pin);
        if (fn->nvregs) {
            F.usecnt = xmalloc((size_t)fn->nvregs * sizeof *F.usecnt);
            ra_count_vreg_uses(fn, F.usecnt);
        }
    }
    if (fn->has_alloca)
        F.used_callee[F.nsave++] = FB6;
    if (fn->cmse_entry)
        tcg_cmse_check_entry(fn);
    /* A leaf: nothing calls, the lowering's own calls included. An asm
     * writes lr when its template calls or names it, which irgen recorded
     * (ir_asm.clob); one whose clobbers are unknown might. */
    F.leaf = 1;
    for (i = 0; i < fn->nins; i++)
        if (fn->ins[i].op == IR_CALL ||
            (fn->ins[i].op == IR_ASM && fn->ins[i].asm_ir &&
             !fn->ins[i].asm_ir->cont &&
             (!fn->ins[i].asm_ir->clob ||
              (fn->ins[i].asm_ir->clob >> 14 & 1))) ||
            t_op_calls_helper(&fn->ins[i]))
            F.leaf = 0;
    /* REMATERIALIZATION: a temporary whose one definition is a constant
     * (the allocator's remat_ok) and that got no register is made where
     * it is read, and has no slot: LICM hoists a loop's constants to its
     * preheader, and each was stored there and loaded at every use. As
     * codegen.c's, not at -O0/-Og, where every value keeps its slot. */
    if (F.loc && !F.keep_vars && fn->nvregs && tcg_ra()->remat_ok) {
        char *rm = ra_remat_map(fn, tcg_ra()->remat_ok);
        for (int v = fn->nvars; v < fn->nvregs; v++) {
            if (!rm[v] || in_reg6(&F, v) || F.wide[v])
                continue;
            if (!F.remat) {
                F.remat = xcalloc((size_t)fn->nvregs, 1);
                F.remat_v = xcalloc((size_t)fn->nvregs, sizeof *F.remat_v);
            }
            F.remat[v] = 1;
        }
        for (i = 0; F.remat && i < fn->nins; i++) {
            int d = fn->ins[i].dst;
            if (fn->ins[i].op == IR_CONST && d >= 0 && d < fn->nvregs &&
                F.remat[d])
                F.remat_v[d] = (long)fn->ins[i].imm;
        }
        free(rm);
    }
    tcg_layout(&F);
    if (tcg_ext() && F.loc && fn->nins && fn->nvregs)
        F.lv_busy = tcg_lo_busy_map(&F);
    else if (tcg_ext())
        tcg_role_fail();           /* nothing is known free */
    F.label_off = xmalloc((size_t)(fn->nlabels + 1) * sizeof *F.label_off);
    F.no_tbh = xcalloc((size_t)fn->nins + 1, 1);
    {
        /* the values whose slots the slot cache may not keep */
        char *atk = xcalloc((size_t)(fn->nvregs ? fn->nvregs : 1), 1);
        for (i = 0; i < fn->nins; i++)
            if (fn->ins[i].op == IR_ADDR && fn->ins[i].a >= 0 &&
                fn->ins[i].a < fn->nvregs)
                atk[fn->ins[i].a] = 1;
        g6_atk = atk;
    }
    {
        /* the labels every jump to which comes before them (g6_lfwd) */
        int nl = fn->nlabels;
        int *pos = xmalloc((size_t)(nl + 1) * sizeof *pos);
        char *fwd = xmalloc((size_t)nl + 1);
        for (int l = 0; l <= nl; l++) {
            pos[l] = -1;
            fwd[l] = 1;
        }
        for (i = 0; i < fn->nins; i++)
            if (fn->ins[i].op == IR_LABEL && fn->ins[i].label >= 0 &&
                fn->ins[i].label < nl)
                pos[fn->ins[i].label] = i;
        for (i = 0; i < fn->nins; i++) {
            const struct ir_ins *in = &fn->ins[i];
            int l = in->label;
            if (in->op == IR_SWITCH)
                for (int k = 0; k < fn->jt[in->jt].n; k++) {
                    int tl = fn->jt[in->jt].labels[k];
                    if (tl >= 0 && tl < nl)
                        fwd[tl] = 0;
                }
            if (in->op != IR_JMP && in->op != IR_BRZ && in->op != IR_BRNZ &&
                in->op != IR_SWITCH && in->op != IR_LABELADDR)
                continue;
            if (l < 0 || l >= nl)
                continue;
            if (in->op == IR_LABELADDR || pos[l] < 0 || pos[l] < i)
                fwd[l] = 0;
        }
        free(pos);
        g6_lfwd = fwd;
        g6_lseen = xcalloc((size_t)nl + 1, 1);
        g6_lsnap = xmalloc((size_t)(nl + 1) * 8 * sizeof *g6_lsnap);
    }

    {
    int len0 = t->len, nl0 = fn->nlines, nd0 = t->ndrange;
    int sc0 = st->ncall, se0 = st->next, ss0 = st->nstr, sg0 = st->ng,
        sf0 = st->nf;
    int first = 1;                 /* the first pass of a layout */
    for (int pass = 0; pass < 12; pass++) {
        int redo = 0;
        long frame_push;
        t->len = len0;
        t->ndrange = nd0;
        fn->nlines = nl0;
        st->ncall = sc0; st->next = se0; st->nstr = ss0; st->ng = sg0;
        st->nf = sf0;
        F.nfix = 0;
        for (i = 0; i <= fn->nlabels; i++)
            F.label_off[i] = -1;
        F.skip_next = 0;
        F.bc_end = F.bc_fix = -1;
        F.fl_end = -1;
        F.va_regsave = F.va_first = -1;
        F.nlit = F.nlsite = 0;
        F.nlrel = 0;
        F.npads = 0;
        F.spb = 0;
        F.ntmp = 0;
        F.tbusy = 0;
        F.v6_used = 0;
        F.barrier = 0;
        F.fb = T_SP;
        F.isl_replay = !first;
        F.npoint = 0;
        F.shortb = first ? NULL : cls;
        F.nshortb = first ? 0 : ncls;
        if (target_debug_info()) {
            free(fn->var_off);
            fn->var_off = NULL;
        }
        /* A word boundary: the literal pools align themselves by the
         * buffer offset, which is the address mod 4 only when the
         * function starts on one. */
        while (t->len & 3)
            t_nop(t);
        f->code_align = 4;
        if (target_debug_info()) {
            int nv = fn->nvars ? fn->nvars : 1;
            fn->var_off = xmalloc((size_t)nv * sizeof *fn->var_off);
            for (int v = 0; v < fn->nvars; v++)
                fn->var_off[v] = ra_var_home(fn, v, F.slot[v] >= 0,
                                             F.slot[v]);
        }
        f->code_off = t->len;

        /* ---- prologue ---- */
        g6_s0 = 6; g6_s1 = 7;              /* the prologue's */
        rc_reset();
        if (g6_lseen)
            memset(g6_lseen, 0, (size_t)fn->nlabels + 1);
        g6_ins_at = t->len;
        g6_fdead = -1;
        g6_fdead_ins = 0;
        g6_cur = NULL;
        if (F.frame > 1016)
            F.scr_save |= 1u << 6;          /* sp_frame's register */
        if (F.far_mode)
            F.leaf = 0;                     /* BL is a branch: lr is spent */
        F.nopush = F.leaf && !F.nsave && !F.frame && !fn->is_varargs &&
                   !fn->has_alloca && !(F.scr_save & SCR_SET);
        F.cfi_va_end = F.cfi_push_end = F.cfi_vsave_end = -1;
        F.cfi_frame_end = F.cfi_fp_end = -1;
        if (fn->is_varargs) {
            t1_push(t, 0xfu);
            F.cfi_va_end = t->len;
        }
        if (!F.nopush) {
            F.cfi_push_mask = save_mask6(&F, F.scr_save);
            t1_push(t, F.cfi_push_mask);
            F.cfi_push_end = t->len;
        }
        if (F.frame) {
            sp_frame(&F, F.frame, 1);
            F.cfi_frame_end = t->len;
        }
        if (fn->has_alloca) {
            t_mov_reg(t, FB6, T_SP);
            F.fb = FB6;
            F.cfi_fp_end = t->len;
        }
        frame_push = F.nopush ? 0 : F.frame + mask_bytes(save_mask6(&F, F.scr_save));
        {
            struct argplace pl;
            struct abi_walk w;
            long base = frame_push;
            F.entry_off = base;
            int pmv_dst[RA_MAXPOOL * 2], pmv_src[RA_MAXPOOL * 2], npmv = 0;
            int pstk_reg[RA_MAXPOOL]; long pstk_off[RA_MAXPOOL];
            int npstk = 0;
            if (fn->is_varargs) {
                F.va_regsave = base;
                base += 16;
            }
            if (F.sret_slot >= 0)
                fr_st(&F, T_R0, F.sret_slot, 4, 0xfu);
            tcg_walk_init(&w, F.sret_slot >= 0, fn->is_varargs, fn->pcs);
            for (i = 0; i < fn->nparams; i++) {
                struct ir_arg *a = &fn->param_abi[i];
                tcg_place_one(&w, a, &pl);
                if (pl.nvfp)
                    v6_refuse(fn, NULL, "a parameter in a VFP register");
                if (pl.nreg == 1 && pl.nstk == 0 && !a->is_struct &&
                    a->size <= 4 && in_reg6(&F, i)) {
                    pmv_dst[npmv] = F.loc[i];
                    pmv_src[npmv] = pl.reg;
                    npmv++;
                    continue;
                }
                if (a->size > 4 && !a->is_struct && in_reg6(&F, i) &&
                    pl.nreg + pl.nstk == 2 && !fn->is_varargs) {
                    for (int q = 0; q < 2; q++) {
                        if (q < pl.nreg) {
                            pmv_dst[npmv] = F.loc[i] + q;
                            pmv_src[npmv] = pl.reg + q;
                            npmv++;
                        } else {
                            pstk_reg[npstk] = F.loc[i] + q;
                            pstk_off[npstk] = base + pl.stk +
                                              (long)(q - pl.nreg) * 4;
                            npstk++;
                        }
                    }
                    continue;
                }
                if (pl.nreg == 0 && pl.nstk == 1 && !a->is_struct &&
                    a->size <= 4 && in_reg6(&F, i)) {
                    pstk_reg[npstk] = F.loc[i];
                    pstk_off[npstk] = base + pl.stk;
                    npstk++;
                    continue;
                }
                /* into the local's own slot: a composite's slot IS the
                 * composite, and its words arrive whole */
                for (int q = 0; q < pl.nreg; q++) {
                    long off = F.slot[i] + (long)q * 4;
                    int wid = (long)(q + 1) * 4 > a->size ? (a->size & 3) : 4;
                    if (wid == 3) wid = 4;      /* a three-byte tail: store 4 */
                    fr_st(&F, pl.reg + q, off, wid, 0xfu);
                    if (wid == 4 && !a->is_struct && v6_rc_ok(&F, i))
                        rc_note(&F, pl.reg + q, off);
                }
                for (int q = 0; q < pl.nstk; q++) {
                    long src = base + pl.stk + (long)q * 4;
                    long dst = F.slot[i] + (long)(pl.nreg + q) * 4;
                    pool_point(&F, 64, 0);
                    fr_ld(&F, sc(&F, 6), src, 4, 0);
                    fr_st(&F, 6, dst, 4, 0xfu | (1u << 6));
                    if (!a->is_struct && v6_rc_ok(&F, i))
                        rc_note(&F, 6, dst);
                }
            }
            if (npmv) {
                int od[RA_MAXPOOL * 4], os[RA_MAXPOOL * 4];
                int m = ra_parallel_move(pmv_dst, pmv_src, npmv, IP, od, os,
                                         (int)(sizeof od / sizeof od[0]));
                if (m < 0)
                    internal_error("thumb: %s: the prologue's parameter "
                                   "placement is not a well-formed move",
                                   fn->name);
                for (int k = 0; k < m; k++)
                    t_mov_reg(t, od[k], os[k]);
            }
            for (int k = 0; k < npstk; k++)
                fr_ld(&F, pstk_reg[k], pstk_off[k], 4, 0);
            if (fn->is_varargs)
                F.va_first = F.va_regsave + (long)w.ncrn * 4 + w.stk;
        }

        /* ---- body ---- */
        for (i = 0; i < fn->nins; i++) {
            pool_point(&F, v6_est(&F, i), 1);
            unsigned rpush = 0;
            F.ins_mask = 0;
            ra_each_use(&fn->ins[i], ins_mask_of, &F);
            ins_mask_of(fn->ins[i].dst, &F);
            if (tcg_ext()) {
                v6_roles(&F, i);
                rpush = v6_roles_push(&F, i);
            }
            F.tbusy = 0;
            if (!tcg_ext()) {
                g6_s0 = 6;                 /* (v_rdr may have swapped them) */
                g6_s1 = 7;
            }
            g6_ins_at = t->len;
            g6_fdead = t->len;
            g6_fdead_ins = v6_flagless_ins(&fn->ins[i]);
            g6_role_n = 0;
            g6_cur = &fn->ins[i];
            gen_ins(&F, i);
            if (rpush) {
                t1_pop(t, rpush);
                for (unsigned b = rpush; b; b &= b - 1)
                    F.spb -= 4;
            }
            if (F.ntmp || F.spb)
                internal_error("thumb: %s: instruction %d left %d temporaries",
                               fn->name, i, F.ntmp);
            if (F.skip_next) {
                i += F.skip_next;
                F.skip_next = 0;
            }
        }

        /* ---- epilogue ---- */
        g6_s0 = 6; g6_s1 = 7;
        g6_cur = NULL;
        g6_fdead = -1;
        g6_fdead_ins = 0;
        F.label_off[fn->nlabels] = t->len;
        F.ins_mask = 0;
        if (fn->has_alloca)
            t_mov_reg(t, T_SP, FB6);
        sp_frame(&F, F.frame, 0);
        if (fn->cmse_entry) {
            /* CMSE: back to the Non-secure state (codegen.c's
             * cmse_entry_return). POP cannot name lr here, so the return
             * address comes off into r3 -- which is cleared anyway -- and
             * every cleared register then takes lr's value. */
            if (!F.nopush) {
                unsigned mask = save_mask6(&F, F.scr_save);
                if (mask & ~(1u << T_LR))
                    t1_pop(t, mask & ~(1u << T_LR));
                t1_pop(t, 1u << 3);
                t_mov_reg(t, T_LR, 3);
            }
            for (int r = tcg_cmse_ret_regs(fn); r < 4; r++)
                t_mov_reg(t, r, T_LR);
            t_mov_reg(t, IP, T_LR);
            t_msr_apsr(t, T_LR, 0);
            t_bxns(t, T_LR, 0);
        } else if (F.nopush) {
            t_bx(t, T_LR);
        } else {
            unsigned mask = save_mask6(&F, F.scr_save);
            if (fn->is_varargs) {
                /* POP has no lr: the return address comes off into r3,
                 * which no result is in, after the saved registers. */
                if (mask & ~(1u << T_LR))
                    t1_pop(t, mask & ~(1u << T_LR));
                t1_pop(t, 1u << 3);
                t1_sp_adjust(t, 16, 0);
                t_bx(t, 3);
            } else {
                t1_pop(t, (mask & ~(1u << T_LR)) | (1u << T_PC));
            }
        }
        pool_dump(&F, 0);

        /* ---- patching ---- */
        for (i = 0; i < F.nfix; i++) {
            int target = F.label_off[F.fix[i].label];
            if (target < 0)
                internal_error("thumb: %s: label %d was never placed",
                               fn->name, F.fix[i].label);
            if (F.fix[i].cond == T_TAB) {
                code_patch32(t, F.fix[i].at, (unsigned long)(unsigned int)
                             ((target | 1) - F.fix[i].cz_at));
            } else if (F.fix[i].cond == V6_TBB || F.fix[i].cond == V6_TBH) {
                long d = (long)target - F.fix[i].cz_at;
                long lim = F.fix[i].cond == V6_TBB ? 255 : 65535;
                if (d < 0 || d / 2 > lim || (d & 1)) {
                    /* the next wider entry for that switch, however many
                     * of its entries missed this pass */
                    char want = F.fix[i].cond == V6_TBB ? 1 : 2;
                    if (first) {
                        if (F.no_tbh[F.fix[i].ins] < want)
                            F.no_tbh[F.fix[i].ins] = want;
                        redo = 1;
                        continue;
                    }
                    internal_error("thumb: %s: a switch entry cannot reach "
                                   "its label", fn->name);
                }
                if (F.fix[i].cond == V6_TBB)
                    t->p[F.fix[i].at] = (unsigned char)(d / 2);
                else
                    t_patch_hw16(t, F.fix[i].at, (unsigned)(d / 2));
            } else if (F.fix[i].ins == BC_FAR) {
                t_patch_bl(t, F.fix[i].at, target);
            } else if (F.fix[i].cond < 0 || F.fix[i].ins == BC_MED) {
                if (!t_patch_b16(t, F.fix[i].at, target)) {
                    /* (every other short one this pass is past it too:
                     * far mode, again from the start) */
                    if (first) {
                        F.far_mode = 1;
                        redo = 1;
                        continue;
                    }
                    internal_error("thumb: %s: a branch no longer reaches "
                                   "its label", fn->name);
                }
            } else if (!t_patch_bcond16(t, F.fix[i].at, target)) {
                internal_error("thumb: %s: a short branch no longer reaches "
                               "its label", fn->name);
            }
        }
        if (redo) {
            first = 1;
            free(cls);
            cls = NULL;
            ncls = 0;
            F.scr_save = SCR_SET;
            continue;
        }

        /* ---- measuring: each branch's smallest class from here ---- */
        {
            int changed = 0;
            char *nc = xcalloc((size_t)(F.nfix ? F.nfix : 1), 1);
            for (i = 0; i < F.nfix; i++) {
                int c = F.fix[i].ins, target, s, slack = 0, k;
                long d;
                if (F.fix[i].cond >= V6_TBH) {     /* a table entry */
                    nc[i] = 0;
                    continue;
                }
                target = F.label_off[F.fix[i].label];
                s = F.fix[i].cz_at;
                for (k = 0; k < F.npads; k++)
                    if ((F.pads[k] > s && F.pads[k] < target) ||
                        (F.pads[k] < s && F.pads[k] > target))
                        slack += 2;
                if (F.fix[i].cond >= 0) {
                    d = (long)target - (s + 4);
                    if (d >= -256 + slack && d <= 254 - slack)
                        c = BC_SHORT;
                    else if (c > BC_MED) {
                        d = (long)target - (s + 6);
                        if (d >= -2048 + slack && d <= 2046 - slack)
                            c = BC_MED;
                    }
                } else if (c == BC_FAR) {
                    d = (long)target - (s + 4);
                    if (d >= -2048 + slack && d <= 2046 - slack)
                        c = BC_SHORT;
                }
                if (c > F.fix[i].ins)
                    c = F.fix[i].ins;
                nc[i] = (char)c;
                if (c != F.fix[i].ins)
                    changed = 1;
            }
            {
                /* An asm writes neither: its operands are r0-r3 and r12
                 * (sema), irgen refuses a template or clobber list naming
                 * r4-r11, and gen_asm takes them through sc()/tmp_get. */
                unsigned used = F.v6_used & SCR_SET;
                int scr_changed = 0;
                if (F.frame > 1016)
                    used |= 1u << 6;
                if (used & ~F.scr_save) {
                    /* a pass used a scratch register it did not save:
                     * again, saving both */
                    F.scr_save = SCR_SET;
                    scr_changed = 1;
                } else if (used != F.scr_save && first) {
                    F.scr_save = used;
                    scr_changed = 1;
                }
                if (!tcg_regalloc() || (!changed && !scr_changed)) {
                    free(nc);
                    break;
                }
            }
            free(cls);
            cls = nc;
            ncls = F.nfix;
            first = 0;
        }
    }
    }
    v6_scan(&F, f->code_off);
    f->code_len = t->len - f->code_off;
    if (target_debug_info())
        v6_record_cfi(&F, f->code_off);
    /* ...and the variadic register save area pushed before it */
    f->stack_bytes = (F.nopush ? 0 : (int)(F.frame +
                                           mask_bytes(save_mask6(&F, F.scr_save)))) +
                     (fn->is_varargs ? 16 : 0);
    free(cls);
    free(F.usecnt);
    free(F.lv_busy);
    free(F.slot);
    cg_note_labels(fn, F.label_off);
    free(F.label_off);
    free(F.fix);
    free(F.wide);
    free(F.nshr);
    free(F.loc);
    free(F.fvar);
    free(F.fscr);
    free(F.isl);
    free(F.no_tbh);
    free(F.lit);
    free(F.lsite);
    free(F.lrel);
    free(F.pads);
    free(F.remat);
    free(F.remat_v);
    free((char *)g6_atk);
    g6_atk = NULL;
    free(g6_lfwd);
    free(g6_lseen);
    free(g6_lsnap);
    g6_lfwd = g6_lseen = NULL;
    g6_lsnap = NULL;
    g6_cur = NULL;
}
