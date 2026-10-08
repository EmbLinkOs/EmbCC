/* ColdFire ISA_A encoding (emit.h says what and why). Every field below
 * is the 68000 family's, read off the ColdFire Programmer's Reference
 * Manual's tables and refereed by QEMU's disassembler
 * (tests/golden/coldfire-encoding.sh); the mode restrictions are
 * ColdFire's. */
#include "emit.h"
#include "../../driver/util.h"

static const char *const g_names[16] = {
    "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7",
    "a0", "a1", "a2", "a3", "a4", "a5", "fp", "sp"
};

const char *cf_reg_name(int r)
{
    return r >= 0 && r < 16 ? g_names[r] : "?";
}

/* ---- words ------------------------------------------------------------ */

void cf_w(struct code *c, unsigned v)
{
    code_byte(c, (int)((v >> 8) & 0xff));
    code_byte(c, (int)(v & 0xff));
}

void cf_l(struct code *c, unsigned long v)
{
    cf_w(c, (unsigned)((v >> 16) & 0xffff));
    cf_w(c, (unsigned)(v & 0xffff));
}

unsigned cf_rdw(const struct code *c, int at)
{
    return ((unsigned)c->p[at] << 8) | c->p[at + 1];
}

void cf_wrw(struct code *c, int at, unsigned v)
{
    c->p[at] = (unsigned char)((v >> 8) & 0xff);
    c->p[at + 1] = (unsigned char)(v & 0xff);
}

void cf_wrl(struct code *c, int at, unsigned long v)
{
    cf_wrw(c, at, (unsigned)((v >> 16) & 0xffff));
    cf_wrw(c, at + 2, (unsigned)(v & 0xffff));
}

unsigned long cf_get32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8) | p[3];
}

void cf_put32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)((v >> 24) & 0xff);
    p[1] = (unsigned char)((v >> 16) & 0xff);
    p[2] = (unsigned char)((v >> 8) & 0xff);
    p[3] = (unsigned char)(v & 0xff);
}

void cf_put16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)((v >> 8) & 0xff);
    p[1] = (unsigned char)(v & 0xff);
}

/* ---- effective addresses ----------------------------------------------- */

static struct cf_ea ea0(enum cf_mode m, int reg)
{
    struct cf_ea e;
    e.mode = m;
    e.reg = reg;
    e.disp = 0;
    e.xreg = 0;
    e.xscale = 1;
    e.imm = 0;
    return e;
}

static void need_a(int a, const char *what)
{
    if (a < 8 || a > 15)
        internal_error("coldfire: %s wants an address register, got %d",
                       what, a);
}

static void need_d(int d, const char *what)
{
    if (d < 0 || d > 7)
        internal_error("coldfire: %s wants a data register, got %d", what, d);
}

struct cf_ea cf_dreg(int r) { need_d(r, "Dn"); return ea0(CFM_D, r); }
struct cf_ea cf_areg(int r) { need_a(r, "An"); return ea0(CFM_A, r); }
struct cf_ea cf_ind(int a)  { need_a(a, "(An)"); return ea0(CFM_IND, a); }
struct cf_ea cf_post(int a) { need_a(a, "(An)+"); return ea0(CFM_POST, a); }
struct cf_ea cf_pre(int a)  { need_a(a, "-(An)"); return ea0(CFM_PRE, a); }

struct cf_ea cf_disp16(int a, long d)
{
    struct cf_ea e;
    need_a(a, "(d16,An)");
    if (d < -32768 || d > 32767)
        internal_error("coldfire: displacement %ld does not fit 16 bits", d);
    e = ea0(CFM_DISP, a);
    e.disp = d;
    return e;
}

struct cf_ea cf_disp(int a, long d)
{
    return d == 0 ? cf_ind(a) : cf_disp16(a, d);
}

struct cf_ea cf_idx(int a, long d, int x, int scale)
{
    struct cf_ea e;
    need_a(a, "(d8,An,Xi)");
    if (d < -128 || d > 127)
        internal_error("coldfire: index displacement %ld does not fit 8 "
                       "bits", d);
    if (x < 0 || x > 15 || (scale != 1 && scale != 2 && scale != 4))
        internal_error("coldfire: index register %d scale %d", x, scale);
    e = ea0(CFM_IDX, a);
    e.disp = d;
    e.xreg = x;
    e.xscale = scale;
    return e;
}

struct cf_ea cf_absl(long addr)
{
    struct cf_ea e = ea0(CFM_ABSL, 0);
    e.disp = addr;
    return e;
}

struct cf_ea cf_pcdisp(long d)
{
    struct cf_ea e = ea0(CFM_PCDISP, 0);
    if (d < -32768 || d > 32767)
        internal_error("coldfire: pc displacement %ld does not fit 16 bits",
                       d);
    e.disp = d;
    return e;
}

struct cf_ea cf_imm(long v)
{
    struct cf_ea e = ea0(CFM_IMM, 0);
    e.imm = v;
    return e;
}

int cf_ea_is_reg(const struct cf_ea *e)
{
    return e->mode == CFM_D || e->mode == CFM_A;
}

int cf_ea_is_mem_alterable(const struct cf_ea *e)
{
    switch (e->mode) {
    case CFM_IND: case CFM_POST: case CFM_PRE: case CFM_DISP: case CFM_IDX:
    case CFM_ABSW: case CFM_ABSL:
        return 1;
    default:
        return 0;
    }
}

int cf_ea_field(const struct cf_ea *e)
{
    switch (e->mode) {
    case CFM_D:      return 0 << 3 | (e->reg & 7);
    case CFM_A:      return 1 << 3 | (e->reg & 7);
    case CFM_IND:    return 2 << 3 | (e->reg & 7);
    case CFM_POST:   return 3 << 3 | (e->reg & 7);
    case CFM_PRE:    return 4 << 3 | (e->reg & 7);
    case CFM_DISP:   return 5 << 3 | (e->reg & 7);
    case CFM_IDX:    return 6 << 3 | (e->reg & 7);
    case CFM_ABSW:   return 7 << 3 | 0;
    case CFM_ABSL:   return 7 << 3 | 1;
    case CFM_PCDISP: return 7 << 3 | 2;
    case CFM_PCIDX:  return 7 << 3 | 3;
    case CFM_IMM:    return 7 << 3 | 4;
    }
    return 0;
}

int cf_ea_ext_len(const struct cf_ea *e, int size)
{
    switch (e->mode) {
    case CFM_DISP: case CFM_IDX: case CFM_ABSW: case CFM_PCDISP:
    case CFM_PCIDX:
        return 2;
    case CFM_ABSL:
        return 4;
    case CFM_IMM:
        return size == 4 ? 4 : 2;
    default:
        return 0;
    }
}

/* The brief extension word of an indexed mode: D/A, the register, .l,
 * the scale, and an 8-bit displacement. */
static unsigned brief_ext(const struct cf_ea *e)
{
    unsigned sc = e->xscale == 4 ? 2 : e->xscale == 2 ? 1 : 0;
    return (unsigned)(CF_IS_A(e->xreg) ? 0x8000 : 0) |
           (unsigned)((e->xreg & 7) << 12) | 0x0800 | (sc << 9) |
           ((unsigned)e->disp & 0xff);
}

void cf_ea_ext(struct code *c, const struct cf_ea *e, int size)
{
    switch (e->mode) {
    case CFM_DISP: case CFM_PCDISP:
        cf_w(c, (unsigned)e->disp & 0xffff);
        return;
    case CFM_IDX: case CFM_PCIDX:
        cf_w(c, brief_ext(e));
        return;
    case CFM_ABSW:
        cf_w(c, (unsigned)e->disp & 0xffff);
        return;
    case CFM_ABSL:
        cf_l(c, (unsigned long)e->disp & 0xffffffffUL);
        return;
    case CFM_IMM:
        if (size == 4) {
            if (e->imm < -2147483648L || e->imm > 4294967295L)
                internal_error("coldfire: immediate %ld does not fit 32 "
                               "bits", e->imm);
            cf_l(c, (unsigned long)e->imm & 0xffffffffUL);
        } else if (size == 2) {
            if (e->imm < -32768 || e->imm > 65535)
                internal_error("coldfire: immediate %ld does not fit 16 "
                               "bits", e->imm);
            cf_w(c, (unsigned)e->imm & 0xffff);
        } else {
            if (e->imm < -128 || e->imm > 255)
                internal_error("coldfire: immediate %ld does not fit 8 "
                               "bits", e->imm);
            cf_w(c, (unsigned)e->imm & 0xff);
        }
        return;
    default:
        return;
    }
}

static void op_ea(struct code *c, unsigned op, const struct cf_ea *e,
                  int size)
{
    cf_w(c, op | (unsigned)cf_ea_field(e));
    cf_ea_ext(c, e, size);
}

static int size_bits_move(int size)
{
    return size == 1 ? 1 : size == 2 ? 3 : 2;
}

/* clr/tst/neg-style size field (bits 7-6) */
static unsigned size_bits67(int size)
{
    if (size != 1 && size != 2 && size != 4)
        internal_error("coldfire: operand size %d", size);
    return (unsigned)(size == 1 ? 0 : size == 2 ? 1 : 2) << 6;
}

/* ---- moves -------------------------------------------------------------- */

int cf_move_ok(int size, const struct cf_ea *src, const struct cf_ea *dst)
{
    if (size != 1 && size != 2 && size != 4)
        return 0;
    if (dst->mode == CFM_IMM || dst->mode == CFM_PCDISP ||
        dst->mode == CFM_PCIDX)
        return 0;
    if (dst->mode == CFM_A && size == 1)
        return 0;
    if (src->mode == CFM_A && size == 1)
        return 0;
    switch (src->mode) {
    case CFM_D: case CFM_A: case CFM_IND: case CFM_POST: case CFM_PRE:
        return 1;
    case CFM_DISP: case CFM_PCDISP:
        return dst->mode != CFM_IDX && dst->mode != CFM_ABSW &&
               dst->mode != CFM_ABSL;
    default:      /* IDX, PCIDX, ABSW, ABSL, IMM */
        return dst->mode == CFM_D || dst->mode == CFM_A ||
               dst->mode == CFM_IND || dst->mode == CFM_POST ||
               dst->mode == CFM_PRE;
    }
}

void cf_move(struct code *c, int size, struct cf_ea src, struct cf_ea dst)
{
    unsigned df;
    if (!cf_move_ok(size, &src, &dst))
        internal_error("coldfire: move.%c with source mode %d and "
                       "destination mode %d is not a ColdFire form",
                       size == 1 ? 'b' : size == 2 ? 'w' : 'l',
                       (int)src.mode, (int)dst.mode);
    df = (unsigned)cf_ea_field(&dst);
    /* the destination's field is written register first, then mode */
    cf_w(c, (unsigned)(size_bits_move(size) << 12) |
            ((df & 7) << 9) | ((df >> 3) << 6) |
            (unsigned)cf_ea_field(&src));
    cf_ea_ext(c, &src, size);
    cf_ea_ext(c, &dst, size);
}

void cf_moveq(struct code *c, long v, int dn)
{
    need_d(dn, "moveq");
    if (v < -128 || v > 127)
        internal_error("coldfire: moveq #%ld does not fit 8 bits", v);
    cf_w(c, 0x7000u | (unsigned)(dn << 9) | ((unsigned)v & 0xff));
}

static void need_control(const struct cf_ea *e, const char *what)
{
    switch (e->mode) {
    case CFM_IND: case CFM_DISP: case CFM_IDX: case CFM_ABSW: case CFM_ABSL:
    case CFM_PCDISP: case CFM_PCIDX:
        return;
    default:
        internal_error("coldfire: %s takes a control address (mode %d)",
                       what, (int)e->mode);
    }
}

void cf_lea(struct code *c, struct cf_ea src, int an)
{
    need_a(an, "lea");
    need_control(&src, "lea");
    op_ea(c, 0x41c0u | (unsigned)((an & 7) << 9), &src, 4);
}

void cf_pea(struct code *c, struct cf_ea src)
{
    need_control(&src, "pea");
    op_ea(c, 0x4840u, &src, 4);
}

static void need_movem_ea(const struct cf_ea *e)
{
    if (e->mode != CFM_IND && e->mode != CFM_DISP)
        internal_error("coldfire: movem takes (An) or (d16,An) (mode %d)",
                       (int)e->mode);
}

void cf_movem_store(struct code *c, unsigned mask, struct cf_ea dst)
{
    need_movem_ea(&dst);
    if (!mask)
        internal_error("coldfire: movem of no registers");
    cf_w(c, 0x48c0u | (unsigned)cf_ea_field(&dst));
    cf_w(c, mask & 0xffff);
    cf_ea_ext(c, &dst, 4);
}

void cf_movem_load(struct code *c, struct cf_ea src, unsigned mask)
{
    need_movem_ea(&src);
    if (!mask)
        internal_error("coldfire: movem of no registers");
    cf_w(c, 0x4cc0u | (unsigned)cf_ea_field(&src));
    cf_w(c, mask & 0xffff);
    cf_ea_ext(c, &src, 4);
}

/* ---- arithmetic ------------------------------------------------------- */

static unsigned alu_base(enum cf_alu op)
{
    switch (op) {
    case CF_ADD: return 0xd000u;
    case CF_SUB: return 0x9000u;
    case CF_AND: return 0xc000u;
    case CF_OR:  return 0x8000u;
    case CF_EOR: return 0xb000u;
    case CF_CMP: return 0xb000u;
    }
    return 0;
}

void cf_alu(struct code *c, enum cf_alu op, struct cf_ea src, int dn)
{
    need_d(dn, "op.l <ea>,Dn");
    if (op == CF_EOR)
        internal_error("coldfire: eor has no <ea>,Dn form");
    if ((op == CF_AND || op == CF_OR) && src.mode == CFM_A)
        internal_error("coldfire: and/or take no address register source");
    /* opmode 010: .l, <ea> op Dn -> Dn */
    op_ea(c, alu_base(op) | (unsigned)(dn << 9) | 0x0080u, &src, 4);
}

void cf_alu_mem(struct code *c, enum cf_alu op, int dn, struct cf_ea dst)
{
    need_d(dn, "op.l Dn,<ea>");
    if (op == CF_CMP)
        internal_error("coldfire: cmp has no Dn,<ea> form");
    /* eor's only form writes its <ea>, which may be a data register */
    if (!cf_ea_is_mem_alterable(&dst) && !(op == CF_EOR && dst.mode == CFM_D))
        internal_error("coldfire: op.l Dn,<ea> writes memory (mode %d)",
                       (int)dst.mode);
    /* opmode 110: .l, Dn op <ea> -> <ea> */
    op_ea(c, alu_base(op) | (unsigned)(dn << 9) | 0x0180u, &dst, 4);
}

void cf_alua(struct code *c, enum cf_alu op, struct cf_ea src, int an)
{
    need_a(an, "adda/suba/cmpa");
    if (op != CF_ADD && op != CF_SUB && op != CF_CMP)
        internal_error("coldfire: no address-register form of op %d", op);
    /* opmode 111: .l */
    op_ea(c, alu_base(op) | (unsigned)((an & 7) << 9) | 0x01c0u, &src, 4);
}

void cf_alu_imm(struct code *c, enum cf_alu op, long imm, int dn)
{
    unsigned base;
    struct cf_ea s = cf_imm(imm);
    need_d(dn, "op.l #imm,Dn");
    switch (op) {
    case CF_OR:  base = 0x0080u; break;
    case CF_AND: base = 0x0280u; break;
    case CF_SUB: base = 0x0480u; break;
    case CF_ADD: base = 0x0680u; break;
    case CF_EOR: base = 0x0a80u; break;
    default:     base = 0x0c80u; break;      /* CMP */
    }
    cf_w(c, base | (unsigned)dn);
    cf_ea_ext(c, &s, 4);
}

void cf_addq(struct code *c, int sub, int n, struct cf_ea dst)
{
    if (n < 1 || n > 8)
        internal_error("coldfire: addq/subq #%d is outside 1..8", n);
    if (!cf_ea_is_reg(&dst) && !cf_ea_is_mem_alterable(&dst))
        internal_error("coldfire: addq/subq to mode %d", (int)dst.mode);
    op_ea(c, 0x5080u | (unsigned)((n & 7) << 9) | (sub ? 0x0100u : 0),
          &dst, 4);
}

void cf_addx(struct code *c, int sub, int dy, int dx)
{
    need_d(dy, "addx");
    need_d(dx, "addx");
    cf_w(c, (sub ? 0x9180u : 0xd180u) | (unsigned)(dx << 9) | (unsigned)dy);
}

void cf_unary(struct code *c, enum cf_un op, int dn)
{
    static const unsigned base[] = {
        0x4480u, 0x4080u, 0x4680u, 0x4840u, 0x4880u, 0x48c0u, 0x49c0u
    };
    need_d(dn, "a unary operation");
    cf_w(c, base[op] | (unsigned)dn);
}

static void need_data_alterable(const struct cf_ea *e, const char *what)
{
    if (e->mode != CFM_D && !cf_ea_is_mem_alterable(e))
        internal_error("coldfire: %s of mode %d", what, (int)e->mode);
}

void cf_clr(struct code *c, int size, struct cf_ea dst)
{
    need_data_alterable(&dst, "clr");
    op_ea(c, 0x4200u | size_bits67(size), &dst, size);
}

void cf_tst(struct code *c, int size, struct cf_ea src)
{
    need_data_alterable(&src, "tst");
    op_ea(c, 0x4a00u | size_bits67(size), &src, size);
}

static unsigned shift_bits(enum cf_sh op)
{
    /* direction (bit 8: 1 left) and type (bits 4-3: 0 arithmetic, 1
     * logical), with size .l (bits 7-6 = 10) */
    switch (op) {
    case CF_ASL: return 0x0180u;
    case CF_ASR: return 0x0080u;
    case CF_LSL: return 0x0188u;
    default:     return 0x0088u;      /* LSR */
    }
}

void cf_shift_imm(struct code *c, enum cf_sh op, int n, int dn)
{
    need_d(dn, "a shift");
    if (n < 1 || n > 8)
        internal_error("coldfire: shift by #%d is outside 1..8", n);
    cf_w(c, 0xe000u | (unsigned)((n & 7) << 9) | shift_bits(op) |
            (unsigned)dn);
}

void cf_shift_reg(struct code *c, enum cf_sh op, int dcount, int dn)
{
    need_d(dn, "a shift");
    need_d(dcount, "a shift count");
    cf_w(c, 0xe020u | (unsigned)(dcount << 9) | shift_bits(op) |
            (unsigned)dn);
}

/* multiply and divide .l take only Dn, (An), (An)+, -(An), (d16,An) */
static void need_muldiv_l(const struct cf_ea *e, const char *what)
{
    switch (e->mode) {
    case CFM_D: case CFM_IND: case CFM_POST: case CFM_PRE: case CFM_DISP:
        return;
    default:
        internal_error("coldfire: %s.l takes no operand of mode %d", what,
                       (int)e->mode);
    }
}

void cf_mul(struct code *c, int sign, int size, struct cf_ea src, int dn)
{
    need_d(dn, "mul");
    if (src.mode == CFM_A)
        internal_error("coldfire: mul of an address register");
    if (size == 2) {
        op_ea(c, (sign ? 0xc1c0u : 0xc0c0u) | (unsigned)(dn << 9), &src, 2);
        return;
    }
    need_muldiv_l(&src, "mul");
    cf_w(c, 0x4c00u | (unsigned)cf_ea_field(&src));
    cf_w(c, (unsigned)(dn << 12) | (sign ? 0x0800u : 0));
    cf_ea_ext(c, &src, 4);
}

void cf_div(struct code *c, int sign, struct cf_ea src, int dq)
{
    need_d(dq, "div");
    need_muldiv_l(&src, "div");
    cf_w(c, 0x4c40u | (unsigned)cf_ea_field(&src));
    cf_w(c, (unsigned)(dq << 12) | (sign ? 0x0800u : 0) | (unsigned)dq);
    cf_ea_ext(c, &src, 4);
}

void cf_rem(struct code *c, int sign, struct cf_ea src, int dr, int dq)
{
    need_d(dq, "rem");
    need_d(dr, "rem");
    if (dr == dq)
        internal_error("coldfire: rem's remainder and dividend registers "
                       "must differ (that is div)");
    need_muldiv_l(&src, "rem");
    cf_w(c, 0x4c40u | (unsigned)cf_ea_field(&src));
    cf_w(c, (unsigned)(dq << 12) | (sign ? 0x0800u : 0) | (unsigned)dr);
    cf_ea_ext(c, &src, 4);
}

/* ---- control ----------------------------------------------------------- */

int cf_cond_invert(int cond)
{
    if (cond < 2)
        internal_error("coldfire: condition %d has no inverse here", cond);
    return cond ^ 1;
}

void cf_scc(struct code *c, int cond, int dn)
{
    need_d(dn, "scc");
    cf_w(c, 0x50c0u | (unsigned)((cond & 15) << 8) | (unsigned)dn);
}

void cf_bcc_w(struct code *c, int cond, long disp)
{
    if (cond == CF_F)
        internal_error("coldfire: condition F is bsr, not a branch");
    if (disp < -32768 || disp > 32767)
        internal_error("coldfire: branch displacement %ld does not fit 16 "
                       "bits", disp);
    cf_w(c, 0x6000u | (unsigned)((cond & 15) << 8));
    cf_w(c, (unsigned)disp & 0xffff);
}

void cf_bcc_b(struct code *c, int cond, long disp)
{
    if (cond == CF_F)
        internal_error("coldfire: condition F is bsr, not a branch");
    /* 0 selects the 16-bit form and -1 the 32-bit one */
    if (disp < -128 || disp > 127 || disp == 0 || disp == -1)
        internal_error("coldfire: short branch displacement %ld", disp);
    cf_w(c, 0x6000u | (unsigned)((cond & 15) << 8) | ((unsigned)disp & 0xff));
}

int cf_bcc_placeholder(struct code *c, int cond)
{
    int at = c->len;
    cf_bcc_w(c, cond, 0);
    return at;
}

int cf_patch_bcc(struct code *c, int at, int target)
{
    long d = (long)target - (at + 2);
    if ((cf_rdw(c, at) & 0xf0ffu) != 0x6000u)
        internal_error("coldfire: the word at %d is not a bcc.w", at);
    if (d < -32768 || d > 32767)
        return 0;
    cf_wrw(c, at + 2, (unsigned)d & 0xffff);
    return 1;
}

void cf_bsr_w(struct code *c, long disp)
{
    if (disp < -32768 || disp > 32767)
        internal_error("coldfire: bsr displacement %ld does not fit 16 "
                       "bits", disp);
    cf_w(c, 0x6100u);
    cf_w(c, (unsigned)disp & 0xffff);
}

void cf_jsr(struct code *c, struct cf_ea target)
{
    need_control(&target, "jsr");
    op_ea(c, 0x4e80u, &target, 4);
}

void cf_jmp(struct code *c, struct cf_ea target)
{
    need_control(&target, "jmp");
    op_ea(c, 0x4ec0u, &target, 4);
}

void cf_rts(struct code *c)     { cf_w(c, 0x4e75u); }
void cf_nop(struct code *c)     { cf_w(c, 0x4e71u); }
void cf_illegal(struct code *c) { cf_w(c, 0x4afcu); }
void cf_halt(struct code *c)    { cf_w(c, 0x4ac8u); }

void cf_trap(struct code *c, int vec)
{
    if (vec < 0 || vec > 15)
        internal_error("coldfire: trap #%d", vec);
    cf_w(c, 0x4e40u | (unsigned)vec);
}

void cf_link(struct code *c, int an, long disp)
{
    need_a(an, "link");
    if (disp < -32768 || disp > 32767)
        internal_error("coldfire: link.w displacement %ld does not fit 16 "
                       "bits", disp);
    cf_w(c, 0x4e50u | (unsigned)(an & 7));
    cf_w(c, (unsigned)disp & 0xffff);
}

void cf_move_from_sr(struct code *c, int dn)
{
    need_d(dn, "move from sr");
    cf_w(c, 0x40c0u | (unsigned)dn);
}

void cf_move_to_sr(struct code *c, int dn)
{
    need_d(dn, "move to sr");
    cf_w(c, 0x46c0u | (unsigned)dn);
}

void cf_unlk(struct code *c, int an)
{
    need_a(an, "unlk");
    cf_w(c, 0x4e58u | (unsigned)(an & 7));
}

/* ---- what only the assembler writes -----------------------------------------
 *
 * Forms the code generator has no use for, which inline asm and .S files
 * do (src/arch/coldfire/asm.c): the exception return, stop and tpf, the
 * status, condition-code and user-stack-pointer moves, movec, the bit
 * operations, and the short bsr. Fields as the
 * ColdFire Programmer's Reference Manual has them; QEMU's disassembler
 * referees them (tests/golden/coldfire-asm.sh). */

void cf_rte(struct code *c) { cf_w(c, 0x4e73u); }
void cf_tpf(struct code *c) { cf_w(c, 0x51fcu); }

void cf_stop(struct code *c, long imm)
{
    if (imm < 0 || imm > 0xffff)
        internal_error("coldfire: stop #%ld does not fit 16 bits", imm);
    cf_w(c, 0x4e72u);
    cf_w(c, (unsigned)imm);
}

void cf_move_to_sr_imm(struct code *c, long imm)
{
    if (imm < 0 || imm > 0xffff)
        internal_error("coldfire: move to sr #%ld does not fit 16 bits", imm);
    cf_w(c, 0x46fcu);
    cf_w(c, (unsigned)imm);
}

void cf_move_from_ccr(struct code *c, int dn)
{
    need_d(dn, "move from ccr");
    cf_w(c, 0x42c0u | (unsigned)dn);
}

void cf_move_to_ccr(struct code *c, struct cf_ea src)
{
    if (src.mode != CFM_D && src.mode != CFM_IMM)
        internal_error("coldfire: move to ccr takes Dn or #imm (mode %d)",
                       (int)src.mode);
    op_ea(c, 0x44c0u, &src, 2);
}

/* move.l An,%usp (to_usp) or move.l %usp,An */
void cf_move_usp(struct code *c, int to_usp, int an)
{
    need_a(an, "move usp");
    cf_w(c, (to_usp ? 0x4e60u : 0x4e68u) | (unsigned)(an & 7));
}

/* movec Rn,Rc: Rn any of d0-a7, Rc the 12-bit control register number */
void cf_movec(struct code *c, int rn, int rc)
{
    if (rn < 0 || rn > 15 || rc < 0 || rc > 0xfff)
        internal_error("coldfire: movec %d,%d", rn, rc);
    cf_w(c, 0x4e7bu);
    cf_w(c, (unsigned)(rn << 12) | (unsigned)rc);
}

/* btst/bchg/bclr/bset with the bit number in Dn (dn >= 0) or a constant
 * (dn < 0, `bit`), on Dn (mod 32) or a byte in memory (mod 8). */
void cf_bit(struct code *c, enum cf_bitop op, int dn, int bit,
            struct cf_ea dst)
{
    unsigned o = (unsigned)op << 6;
    if (dst.mode != CFM_D && !cf_ea_is_mem_alterable(&dst) &&
        !(op == CF_BTST && (dst.mode == CFM_PCDISP || dst.mode == CFM_PCIDX ||
                            (dst.mode == CFM_IMM && dn >= 0))))
        internal_error("coldfire: bit operation on mode %d", (int)dst.mode);
    if (dn >= 0) {
        need_d(dn, "a bit number");
        op_ea(c, 0x0100u | (unsigned)(dn << 9) | o, &dst, 1);
        return;
    }
    if (bit < 0 || bit > (dst.mode == CFM_D ? 31 : 7))
        internal_error("coldfire: bit number %d", bit);
    if (dst.mode == CFM_IDX || dst.mode == CFM_ABSW || dst.mode == CFM_ABSL ||
        dst.mode == CFM_PCIDX)
        internal_error("coldfire: a static bit operation takes no mode %d",
                       (int)dst.mode);
    cf_w(c, 0x0800u | o | (unsigned)cf_ea_field(&dst));
    cf_w(c, (unsigned)bit);
    cf_ea_ext(c, &dst, 1);
}

/* bsr.b (bra.b is cf_bcc_b with CF_T). */
void cf_bsr_b(struct code *c, long disp)
{
    if (disp < -128 || disp > 127 || disp == 0 || disp == -1)
        internal_error("coldfire: short bsr displacement %ld", disp);
    cf_w(c, 0x6100u | ((unsigned)disp & 0xff));
}
