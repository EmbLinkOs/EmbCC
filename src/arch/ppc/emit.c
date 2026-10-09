/* 32-bit PowerPC instruction encoding. See emit.h for the shape. */
#include "emit.h"

#include "../../driver/util.h"


static void need_reg(int r)
{
    if (r < 0 || r > 31)
        internal_error("ppc: register %d is not r0-r31", r);
}

static void need_field(long long v, long long lo, long long hi,
                       const char *what)
{
    if (v < lo || v > hi)
        internal_error("ppc: %s %lld is outside %lld..%lld", what, v, lo, hi);
}

/* r0 reads as the number 0 in these fields: never the register meant. */
static void need_base(int r, const char *what)
{
    need_reg(r);
    if (r == 0)
        internal_error("ppc: r0 as the %s, which reads as 0", what);
}

int ppc_fits16(long long v, int sign)
{
    return sign ? v >= -32768 && v <= 32767 : v >= 0 && v <= 65535;
}

void ppc_put_word(unsigned char *p, unsigned long w)
{
    p[0] = (unsigned char)(w >> 24);
    p[1] = (unsigned char)(w >> 16);
    p[2] = (unsigned char)(w >> 8);
    p[3] = (unsigned char)w;
}

unsigned long ppc_get_word(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8) | (unsigned long)p[3];
}

void ppc_w(struct code *c, unsigned long w)
{
    unsigned char b[4];
    ppc_put_word(b, w & 0xffffffffUL);
    for (int k = 0; k < 4; k++)
        code_byte(c, b[k]);
}

unsigned long ppc_rdw(const struct code *c, int at)
{
    return ppc_get_word(c->p + at);
}

void ppc_wrw(struct code *c, int at, unsigned long w)
{
    ppc_put_word(c->p + at, w & 0xffffffffUL);
}

/* ---- the formats ---------------------------------------------------- */

unsigned long ppc_enc_d(int op, int rt, int ra, unsigned imm16)
{
    need_reg(rt); need_reg(ra);
    need_field(imm16, 0, 0xffff, "16-bit field");
    return ((unsigned long)op << 26) | ((unsigned long)rt << 21) |
           ((unsigned long)ra << 16) | (unsigned long)imm16;
}

unsigned long ppc_enc_x(int op, int rt, int ra, int rb, int xo, int rc)
{
    need_reg(rt); need_reg(ra); need_reg(rb);
    return ((unsigned long)op << 26) | ((unsigned long)rt << 21) |
           ((unsigned long)ra << 16) | ((unsigned long)rb << 11) |
           ((unsigned long)xo << 1) | (unsigned long)(rc & 1);
}

unsigned long ppc_enc_m(int op, int rs, int ra, int sh, int mb, int me,
                        int rc)
{
    need_reg(rs); need_reg(ra);
    need_field(sh, 0, 31, "rotate");
    need_field(mb, 0, 31, "mask begin");
    need_field(me, 0, 31, "mask end");
    return ((unsigned long)op << 26) | ((unsigned long)rs << 21) |
           ((unsigned long)ra << 16) | ((unsigned long)sh << 11) |
           ((unsigned long)mb << 6) | ((unsigned long)me << 1) |
           (unsigned long)(rc & 1);
}

unsigned long ppc_enc_bform(int bo, int bi, long bd, int aa, int lk)
{
    need_field(bo, 0, 31, "BO");
    need_field(bi, 0, 31, "BI");
    if (bd & 3)
        internal_error("ppc: a branch displacement %ld not a multiple of 4",
                       bd);
    need_field(bd, -32768, 32764, "bc displacement");
    return ((unsigned long)PPC_OP_BC << 26) | ((unsigned long)bo << 21) |
           ((unsigned long)bi << 16) | ((unsigned long)bd & 0xfffcUL) |
           (unsigned long)(aa ? 2 : 0) | (unsigned long)(lk ? 1 : 0);
}

unsigned long ppc_enc_iform(long li, int aa, int lk)
{
    if (li & 3)
        internal_error("ppc: a branch displacement %ld not a multiple of 4",
                       li);
    need_field(li, -(1L << 25), (1L << 25) - 4, "b displacement");
    return ((unsigned long)PPC_OP_B << 26) | ((unsigned long)li & 0x3fffffcUL) |
           (unsigned long)(aa ? 2 : 0) | (unsigned long)(lk ? 1 : 0);
}

static unsigned imm16(long long v, int sign, const char *what)
{
    if (!ppc_fits16(v, sign))
        internal_error("ppc: %s: %lld does not fit a %s 16-bit field", what,
                       v, sign ? "sign-extended" : "zero-extended");
    return (unsigned)(v & 0xffff);
}

/* ---- constants and moves ------------------------------------------- */

void ppc_nop(struct code *c) { ppc_w(c, ppc_enc_d(PPC_OP_ORI, 0, 0, 0)); }

void ppc_mr(struct code *c, int rd, int rs)
{
    ppc_w(c, ppc_enc_x(PPC_OP_31, rs, rd, rs, PPC_X_OR, 0));
}

void ppc_lis(struct code *c, int rd, unsigned imm)
{
    ppc_w(c, ppc_enc_d(PPC_OP_ADDIS, rd, 0, imm & 0xffff));
}

static long long low32(long long v)
{
    return (long long)(int)(unsigned int)(unsigned long long)v;
}

int ppc_li_len(long long v)
{
    v = low32(v);
    if (ppc_fits16(v, 1))
        return 4;
    return (v & 0xffff) ? 8 : 4;
}

void ppc_li(struct code *c, int rd, long long v)
{
    v = low32(v);
    if (ppc_fits16(v, 1)) {
        ppc_w(c, ppc_enc_d(PPC_OP_ADDI, rd, 0, (unsigned)(v & 0xffff)));
        return;
    }
    ppc_lis(c, rd, (unsigned)((unsigned long long)v >> 16) & 0xffff);
    if (v & 0xffff)
        ppc_w(c, ppc_enc_d(PPC_OP_ORI, rd, rd, (unsigned)(v & 0xffff)));
}

/* ---- immediates ----------------------------------------------------- */

int ppc_imm_ok(int op, long long imm)
{
    switch (op) {
    case PPC_ADDI: case PPC_ADDIS: case PPC_ADDIC: case PPC_MULLI:
    case PPC_SUBFIC:
        return ppc_fits16(imm, 1);
    default:
        return ppc_fits16(imm, 0);
    }
}

void ppc_imm(struct code *c, int op, int d, int s, long long imm)
{
    switch (op) {
    case PPC_ADDI:
        need_base(s, "source of addi");
        ppc_w(c, ppc_enc_d(PPC_OP_ADDI, d, s, imm16(imm, 1, "addi")));
        return;
    case PPC_ADDIS:
        need_base(s, "source of addis");
        ppc_w(c, ppc_enc_d(PPC_OP_ADDIS, d, s, imm16(imm, 1, "addis")));
        return;
    case PPC_ADDIC:
        ppc_w(c, ppc_enc_d(PPC_OP_ADDIC, d, s, imm16(imm, 1, "addic")));
        return;
    case PPC_MULLI:
        ppc_w(c, ppc_enc_d(PPC_OP_MULLI, d, s, imm16(imm, 1, "mulli")));
        return;
    case PPC_SUBFIC:
        ppc_w(c, ppc_enc_d(PPC_OP_SUBFIC, d, s, imm16(imm, 1, "subfic")));
        return;
    /* the logical ones: the SOURCE in the first field */
    case PPC_ORI:
        ppc_w(c, ppc_enc_d(PPC_OP_ORI, s, d, imm16(imm, 0, "ori")));
        return;
    case PPC_ORIS:
        ppc_w(c, ppc_enc_d(PPC_OP_ORIS, s, d, imm16(imm, 0, "oris")));
        return;
    case PPC_XORI:
        ppc_w(c, ppc_enc_d(PPC_OP_XORI, s, d, imm16(imm, 0, "xori")));
        return;
    case PPC_XORIS:
        ppc_w(c, ppc_enc_d(PPC_OP_XORIS, s, d, imm16(imm, 0, "xoris")));
        return;
    case PPC_ANDI_:
        ppc_w(c, ppc_enc_d(PPC_OP_ANDI, s, d, imm16(imm, 0, "andi.")));
        return;
    case PPC_ANDIS_:
        ppc_w(c, ppc_enc_d(PPC_OP_ANDIS, s, d, imm16(imm, 0, "andis.")));
        return;
    default:
        internal_error("ppc: immediate operation %d", op);
    }
}

/* ---- register operations -------------------------------------------- */

void ppc_alu(struct code *c, int op, int d, int a, int b)
{
    unsigned long w;
    switch (op) {
    /* XO-form: rt, ra, rb -- the destination first */
    case PPC_ADD:    w = ppc_enc_x(PPC_OP_31, d, a, b, PPC_X_ADD, 0); break;
    case PPC_SUB:    w = ppc_enc_x(PPC_OP_31, d, b, a, PPC_X_SUBF, 0); break;
    case PPC_ADDC:   w = ppc_enc_x(PPC_OP_31, d, a, b, PPC_X_ADDC, 0); break;
    case PPC_ADDE:   w = ppc_enc_x(PPC_OP_31, d, a, b, PPC_X_ADDE, 0); break;
    case PPC_SUBC:   w = ppc_enc_x(PPC_OP_31, d, b, a, PPC_X_SUBFC, 0); break;
    case PPC_SUBE:   w = ppc_enc_x(PPC_OP_31, d, b, a, PPC_X_SUBFE, 0); break;
    case PPC_MULLW:  w = ppc_enc_x(PPC_OP_31, d, a, b, PPC_X_MULLW, 0); break;
    case PPC_MULHW:  w = ppc_enc_x(PPC_OP_31, d, a, b, PPC_X_MULHW, 0); break;
    case PPC_MULHWU: w = ppc_enc_x(PPC_OP_31, d, a, b, PPC_X_MULHWU, 0); break;
    case PPC_DIVW:   w = ppc_enc_x(PPC_OP_31, d, a, b, PPC_X_DIVW, 0); break;
    case PPC_DIVWU:  w = ppc_enc_x(PPC_OP_31, d, a, b, PPC_X_DIVWU, 0); break;
    /* X-form logic and shifts: rs (the first source), ra (the
     * destination), rb */
    case PPC_AND:    w = ppc_enc_x(PPC_OP_31, a, d, b, PPC_X_AND, 0); break;
    case PPC_ANDC:   w = ppc_enc_x(PPC_OP_31, a, d, b, PPC_X_ANDC, 0); break;
    case PPC_OR:     w = ppc_enc_x(PPC_OP_31, a, d, b, PPC_X_OR, 0); break;
    case PPC_ORC:    w = ppc_enc_x(PPC_OP_31, a, d, b, PPC_X_ORC, 0); break;
    case PPC_XOR:    w = ppc_enc_x(PPC_OP_31, a, d, b, PPC_X_XOR, 0); break;
    case PPC_NAND:   w = ppc_enc_x(PPC_OP_31, a, d, b, PPC_X_NAND, 0); break;
    case PPC_NOR:    w = ppc_enc_x(PPC_OP_31, a, d, b, PPC_X_NOR, 0); break;
    case PPC_EQV:    w = ppc_enc_x(PPC_OP_31, a, d, b, PPC_X_EQV, 0); break;
    case PPC_SLW:    w = ppc_enc_x(PPC_OP_31, a, d, b, PPC_X_SLW, 0); break;
    case PPC_SRW:    w = ppc_enc_x(PPC_OP_31, a, d, b, PPC_X_SRW, 0); break;
    case PPC_SRAW:   w = ppc_enc_x(PPC_OP_31, a, d, b, PPC_X_SRAW, 0); break;
    default:
        internal_error("ppc: operation %d", op);
        return;
    }
    ppc_w(c, w);
}

void ppc_un(struct code *c, int op, int d, int s)
{
    unsigned long w;
    switch (op) {
    case PPC_NEG:    w = ppc_enc_x(PPC_OP_31, d, s, 0, PPC_X_NEG, 0); break;
    case PPC_ADDZE:  w = ppc_enc_x(PPC_OP_31, d, s, 0, PPC_X_ADDZE, 0); break;
    case PPC_SUBFZE: w = ppc_enc_x(PPC_OP_31, d, s, 0, PPC_X_SUBFZE, 0); break;
    case PPC_ADDME:  w = ppc_enc_x(PPC_OP_31, d, s, 0, PPC_X_ADDME, 0); break;
    case PPC_SUBFME: w = ppc_enc_x(PPC_OP_31, d, s, 0, PPC_X_SUBFME, 0); break;
    case PPC_CNTLZW: w = ppc_enc_x(PPC_OP_31, s, d, 0, PPC_X_CNTLZW, 0); break;
    case PPC_EXTSB:  w = ppc_enc_x(PPC_OP_31, s, d, 0, PPC_X_EXTSB, 0); break;
    case PPC_EXTSH:  w = ppc_enc_x(PPC_OP_31, s, d, 0, PPC_X_EXTSH, 0); break;
    default:
        internal_error("ppc: unary operation %d", op);
        return;
    }
    ppc_w(c, w);
}

void ppc_srawi(struct code *c, int d, int s, int sh)
{
    need_field(sh, 0, 31, "srawi amount");
    ppc_w(c, ppc_enc_x(PPC_OP_31, s, d, sh, PPC_X_SRAWI, 0));
}

void ppc_rlwinm(struct code *c, int d, int s, int sh, int mb, int me)
{
    ppc_w(c, ppc_enc_m(PPC_OP_RLWINM, s, d, sh, mb, me, 0));
}

void ppc_rlwimi(struct code *c, int d, int s, int sh, int mb, int me)
{
    ppc_w(c, ppc_enc_m(PPC_OP_RLWIMI, s, d, sh, mb, me, 0));
}

void ppc_rlwnm(struct code *c, int d, int s, int rb, int mb, int me)
{
    need_reg(rb);
    ppc_w(c, ppc_enc_m(PPC_OP_RLWNM, s, d, rb, mb, me, 0));
}

void ppc_slwi(struct code *c, int d, int s, int n)
{
    need_field(n, 0, 31, "slwi amount");
    ppc_rlwinm(c, d, s, n, 0, 31 - n);
}

void ppc_srwi(struct code *c, int d, int s, int n)
{
    need_field(n, 0, 31, "srwi amount");
    ppc_rlwinm(c, d, s, (32 - n) & 31, n, 31);
}

/* ---- comparisons ---------------------------------------------------- */

void ppc_cmp(struct code *c, int cr, int sign, int a, int b)
{
    need_field(cr, 0, 7, "CR field");
    ppc_w(c, ppc_enc_x(PPC_OP_31, cr << 2, a, b, sign ? PPC_X_CMP : PPC_X_CMPL, 0));
}

void ppc_cmpi(struct code *c, int cr, int sign, int a, long long imm)
{
    need_field(cr, 0, 7, "CR field");
    ppc_w(c, ppc_enc_d(sign ? PPC_OP_CMPI : PPC_OP_CMPLI, cr << 2, a,
                       imm16(imm, sign, sign ? "cmpwi" : "cmplwi")));
}

void ppc_mfcr(struct code *c, int d)
{
    ppc_w(c, ppc_enc_x(PPC_OP_31, d, 0, 0, PPC_X_MFCR, 0));
}

void ppc_crxor(struct code *c, int bt, int ba, int bb)
{
    ppc_w(c, ppc_enc_x(PPC_OP_19, bt, ba, bb, PPC_XL_CRXOR, 0));
}

/* ---- memory ---------------------------------------------------------- */

void ppc_load(struct code *c, int rt, int base, int off, int size, int sign)
{
    int op;
    need_base(base, "base of a load");
    switch (size) {
    case 1:
        if (sign)
            internal_error("ppc: a sign-extending byte load (there is none)");
        op = PPC_OP_LBZ;
        break;
    case 2: op = sign ? PPC_OP_LHA : PPC_OP_LHZ; break;
    case 4: op = PPC_OP_LWZ; break;
    default: internal_error("ppc: a %d-byte load", size); return;
    }
    ppc_w(c, ppc_enc_d(op, rt, base, imm16(off, 1, "load offset")));
}

void ppc_store(struct code *c, int rs, int base, int off, int size)
{
    int op = size == 1 ? PPC_OP_STB : size == 2 ? PPC_OP_STH : PPC_OP_STW;
    need_base(base, "base of a store");
    if (size != 1 && size != 2 && size != 4)
        internal_error("ppc: a %d-byte store", size);
    ppc_w(c, ppc_enc_d(op, rs, base, imm16(off, 1, "store offset")));
}

void ppc_loadx(struct code *c, int rt, int ra, int rb, int size, int sign)
{
    int xo;
    need_base(ra, "base of an indexed load");
    switch (size) {
    case 1:
        if (sign)
            internal_error("ppc: a sign-extending byte load (there is none)");
        xo = PPC_X_LBZX;
        break;
    case 2: xo = sign ? PPC_X_LHAX : PPC_X_LHZX; break;
    case 4: xo = PPC_X_LWZX; break;
    default: internal_error("ppc: a %d-byte load", size); return;
    }
    ppc_w(c, ppc_enc_x(PPC_OP_31, rt, ra, rb, xo, 0));
}

void ppc_storex(struct code *c, int rs, int ra, int rb, int size)
{
    int xo = size == 1 ? PPC_X_STBX : size == 2 ? PPC_X_STHX : PPC_X_STWX;
    need_base(ra, "base of an indexed store");
    if (size != 1 && size != 2 && size != 4)
        internal_error("ppc: a %d-byte store", size);
    ppc_w(c, ppc_enc_x(PPC_OP_31, rs, ra, rb, xo, 0));
}

void ppc_stwu(struct code *c, int rs, int ra, int off)
{
    need_base(ra, "base of stwu");
    ppc_w(c, ppc_enc_d(PPC_OP_STWU, rs, ra, imm16(off, 1, "stwu offset")));
}

void ppc_stwux(struct code *c, int rs, int ra, int rb)
{
    need_base(ra, "base of stwux");
    ppc_w(c, ppc_enc_x(PPC_OP_31, rs, ra, rb, PPC_X_STWUX, 0));
}

void ppc_lbrx(struct code *c, int rt, int ra, int rb, int size)
{
    if (size != 2 && size != 4)
        internal_error("ppc: a %d-byte byte-reversed load", size);
    ppc_w(c, ppc_enc_x(PPC_OP_31, rt, ra, rb, size == 2 ? PPC_X_LHBRX : PPC_X_LWBRX, 0));
}

void ppc_stbrx(struct code *c, int rs, int ra, int rb, int size)
{
    if (size != 2 && size != 4)
        internal_error("ppc: a %d-byte byte-reversed store", size);
    ppc_w(c, ppc_enc_x(PPC_OP_31, rs, ra, rb, size == 2 ? PPC_X_STHBRX : PPC_X_STWBRX,
                       0));
}

void ppc_lwarx(struct code *c, int rt, int ra, int rb)
{
    ppc_w(c, ppc_enc_x(PPC_OP_31, rt, ra, rb, PPC_X_LWARX, 0));
}

void ppc_stwcx(struct code *c, int rs, int ra, int rb)
{
    ppc_w(c, ppc_enc_x(PPC_OP_31, rs, ra, rb, PPC_X_STWCX, 1));
}

void ppc_sync(struct code *c) { ppc_w(c, ppc_enc_x(PPC_OP_31, 0, 0, 0, PPC_X_SYNC, 0)); }
void ppc_isync(struct code *c) { ppc_w(c, ppc_enc_x(PPC_OP_19, 0, 0, 0, PPC_XL_ISYNC, 0)); }

/* ---- control flow ---------------------------------------------------- */

/* BO 12: branch if the bit is set; 4: if it is clear. BI: the field's
 * bit -- 0 LT, 1 GT, 2 EQ. */
static void cond_bits(int cond, int *bo, int *bi)
{
    switch (cond) {
    case PPC_LT: *bo = 12; *bi = 0; return;
    case PPC_GE: *bo = 4;  *bi = 0; return;
    case PPC_GT: *bo = 12; *bi = 1; return;
    case PPC_LE: *bo = 4;  *bi = 1; return;
    case PPC_EQ: *bo = 12; *bi = 2; return;
    case PPC_NE: *bo = 4;  *bi = 2; return;
    default: internal_error("ppc: branch condition %d", cond);
    }
}

int ppc_cond_invert(int cond)
{
    switch (cond) {
    case PPC_LT: return PPC_GE;
    case PPC_GE: return PPC_LT;
    case PPC_GT: return PPC_LE;
    case PPC_LE: return PPC_GT;
    case PPC_EQ: return PPC_NE;
    default:     return PPC_EQ;
    }
}

unsigned long ppc_enc_bc(int cond, int cr, long off)
{
    int bo, bi;
    cond_bits(cond, &bo, &bi);
    need_field(cr, 0, 7, "CR field");
    return ppc_enc_bform(bo, cr * 4 + bi, off, 0, 0);
}

unsigned long ppc_enc_bdnz(long off)
{
    /* BO 16: decrement CTR, branch if it is not zero, ignoring the CR */
    return ppc_enc_bform(16, 0, off, 0, 0);
}

unsigned long ppc_enc_b(long off, int link)
{
    return ppc_enc_iform(off, 0, link);
}

int ppc_bc_placeholder(struct code *c, int cond, int cr)
{
    int at = c->len;
    ppc_w(c, ppc_enc_bc(cond, cr, 0));
    return at;
}

int ppc_b_placeholder(struct code *c)
{
    int at = c->len;
    ppc_w(c, ppc_enc_b(0, 0));
    return at;
}

int ppc_bc_reaches(long at, long target)
{
    long d = target - at;
    return d >= -32768 && d <= 32764;
}

int ppc_patch_branch(struct code *c, int at, int target)
{
    unsigned long w = ppc_rdw(c, at);
    long d = (long)target - (long)at;
    if ((w >> 26) == PPC_OP_BC) {
        if (!ppc_bc_reaches(at, target))
            return 0;
        ppc_wrw(c, at, (w & ~0xfffcUL) | ((unsigned long)d & 0xfffcUL));
        return 1;
    }
    if ((w >> 26) != PPC_OP_B)
        internal_error("ppc: patching a word that is not a branch");
    if (d < -(1L << 25) || d > (1L << 25) - 4)
        return 0;
    ppc_wrw(c, at, (w & ~0x3fffffcUL) | ((unsigned long)d & 0x3fffffcUL));
    return 1;
}

void ppc_bl(struct code *c) { ppc_w(c, ppc_enc_b(0, 1)); }
void ppc_b_rel(struct code *c) { ppc_w(c, ppc_enc_b(0, 0)); }
void ppc_blr(struct code *c) { ppc_w(c, ppc_enc_x(PPC_OP_19, 20, 0, 0, PPC_XL_BCLR, 0)); }
void ppc_bctr(struct code *c) { ppc_w(c, ppc_enc_x(PPC_OP_19, 20, 0, 0, PPC_XL_BCCTR, 0)); }
void ppc_bctrl(struct code *c) { ppc_w(c, ppc_enc_x(PPC_OP_19, 20, 0, 0, PPC_XL_BCCTR, 1)); }

/* The SPR number is written with its two five-bit halves exchanged. */
static int spr_field(int spr)
{
    need_field(spr, 0, 1023, "SPR number");
    return ((spr & 31) << 5) | (spr >> 5);
}

void ppc_mfspr(struct code *c, int d, int spr)
{
    int f = spr_field(spr);
    ppc_w(c, ppc_enc_x(PPC_OP_31, d, f >> 5, f & 31, PPC_X_MFSPR, 0));
}

void ppc_mtspr(struct code *c, int spr, int s)
{
    int f = spr_field(spr);
    ppc_w(c, ppc_enc_x(PPC_OP_31, s, f >> 5, f & 31, PPC_X_MTSPR, 0));
}

void ppc_mflr(struct code *c, int d) { ppc_mfspr(c, d, 8); }
void ppc_mtlr(struct code *c, int s) { ppc_mtspr(c, 8, s); }
void ppc_mtctr(struct code *c, int s) { ppc_mtspr(c, 9, s); }
void ppc_trap(struct code *c) { ppc_w(c, ppc_enc_x(PPC_OP_31, 31, 0, 0, PPC_X_TW, 0)); }
void ppc_sc(struct code *c) { ppc_w(c, ((unsigned long)PPC_OP_SC << 26) | 2UL); }
void ppc_tlbwe(struct code *c) { ppc_w(c, ppc_enc_x(PPC_OP_31, 0, 0, 0, PPC_X_TLBWE, 0)); }
void ppc_mfmsr(struct code *c, int d) { ppc_w(c, ppc_enc_x(PPC_OP_31, d, 0, 0, PPC_X_MFMSR, 0)); }
void ppc_mtmsr(struct code *c, int s) { ppc_w(c, ppc_enc_x(PPC_OP_31, s, 0, 0, PPC_X_MTMSR, 0)); }

void ppc_wrteei(struct code *c, int e)
{
    need_field(e, 0, 1, "wrteei E");
    ppc_w(c, ppc_enc_x(PPC_OP_31, 0, 0, 0, PPC_X_WRTEEI, 0) | ((unsigned long)e << 15));
}
