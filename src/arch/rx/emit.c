/* Renesas RX instruction encoding (emit.h says what and why).
 *
 * Each family of instructions shares a few field layouts; they are
 * written once here (the li immediate, the ld displacement, the "memex"
 * source operand) and every instruction is its opcode bits plus which
 * register goes where, so a new form cannot put a field in a new place.
 */
#include "emit.h"

#include "../../driver/util.h"

static EMBCC_NORETURN void bad(const char *what, long v)
{
    internal_error("rx: %s (%ld) cannot be encoded", what, v);
}

static void reg_ok(int r)
{
    if (r < 0 || r > 15)
        bad("register number", r);
}

/* A value as the 32-bit register holds it, sign-extended into a long. */
static long s32(long v)
{
    return (long)(int)(unsigned int)(unsigned long)v;
}

/* ---- the li immediate ------------------------------------------------------
 *
 * Two bits name how many bytes follow: 01 one, 10 two, 11 three, 00 four.
 * The value is sign-extended from what is stored, so the shortest field
 * is the one whose sign extension gives the 32-bit value back. */
int rx_li_bytes(long imm)
{
    long v = s32(imm);
    if (v >= -128 && v <= 127) return 1;
    if (v >= -32768 && v <= 32767) return 2;
    if (v >= -8388608 && v <= 8388607) return 3;
    return 4;
}

static int li_field(long imm)
{
    int n = rx_li_bytes(imm);
    return n == 4 ? 0 : n;
}

static void li_bytes(struct code *c, long imm)
{
    unsigned long v = (unsigned long)imm;
    int n = rx_li_bytes(imm);
    for (int k = 0; k < n; k++)
        code_byte(c, (int)(v >> (8 * k)) & 0xff);
}

/* ---- the ld displacement ---------------------------------------------------
 *
 * 00 none ([rs]), 01 one byte, 10 two; the stored value is the byte
 * displacement divided by the scale, and is unsigned. */
static int scaled(int scale, long dsp, long *units)
{
    if (dsp < 0 || dsp % scale)
        return 0;
    *units = dsp / scale;
    return *units <= 65535;
}

static int ld_field(long units)
{
    return units == 0 ? 0 : units <= 255 ? 1 : 2;
}

static void ld_bytes(struct code *c, long units)
{
    if (units == 0)
        return;
    code_byte(c, (int)(units & 0xff));
    if (units > 255)
        code_byte(c, (int)((units >> 8) & 0xff));
}

static int size_scale(int size)
{
    return size == RX_B ? 1 : size == RX_W ? 2 : 4;
}

int rx_dsp_ok(int size, long dsp)
{
    long u;
    return scaled(size_scale(size), dsp, &u);
}

/* QEMU decodes the 16-bit displacement of movu (0101 1s10) and of
 * mov #imm, dsp:16[rd] (fa) as SIGNED -- the manual's is unsigned, and
 * QEMU's mov and memex forms read it unsigned -- so above 32767 units the
 * emulator and the hardware would access different addresses. Neither
 * form is emitted there. */
int rx_load_ok(int size, int sign, long dsp)
{
    long u;
    if (!scaled(size_scale(size), dsp, &u))
        return 0;
    return sign || size == RX_L || u <= 32767;
}

int rx_store_imm_ok(int size, long dsp)
{
    long u;
    return scaled(size_scale(size), dsp, &u) && u <= 32767;
}

static long units_of(int size, long dsp)
{
    long u;
    if (size < RX_B || size > RX_L)
        bad("access size", size);
    if (!scaled(size_scale(size), dsp, &u))
        bad("memory displacement", dsp);
    return u;
}

/* ---- conditions ---------------------------------------------------------- */

int rx_cond_invert(int cond)
{
    if (cond < 0 || cond > RX_NO)
        bad("condition to invert", cond);
    return cond ^ 1;            /* the field pairs each test with its inverse */
}

const char *rx_cond_name(int cond)
{
    static const char *const n[] = {
        "eq", "ne", "c", "nc", "gtu", "leu", "pz", "n",
        "ge", "lt", "gt", "le", "o", "no", "ra"
    };
    return cond >= 0 && cond <= RX_ALWAYS ? n[cond] : "?";
}

const char *rx_op_name(int op)
{
    static const char *const n[] = {
        "mov", "add", "sub", "cmp", "and", "or", "xor", "tst", "mul",
        "adc", "sbb", "div", "divu", "emul", "emulu", "max", "min",
        "neg", "not", "abs", "shll", "shlr", "shar", "rotl", "rotr",
        "revl", "revw", "xchg", "stz", "stnz"
    };
    return op >= 0 && op <= RX_STNZ ? n[op] : "?";
}

/* ---- register to register ---------------------------------------------- */

/* The one-byte-opcode family: SUB CMP ADD MUL AND OR, `0100 op ld`. */
static int short_op(int op)
{
    switch (op) {
    case RX_SUB: return 0;
    case RX_CMP: return 1;
    case RX_ADD: return 2;
    case RX_MUL: return 3;
    case RX_AND: return 4;
    case RX_OR:  return 5;
    default:     return -1;
    }
}

/* The FC family's operand byte for `op .ub/rs`, with ld in its low bits,
 * and the 06..20 extension's opcode for the other memory sizes. */
static int fc_op(int op, int *ext)
{
    switch (op) {
    case RX_MAX:   *ext = 0x04; return 0x10;
    case RX_MIN:   *ext = 0x05; return 0x14;
    case RX_EMUL:  *ext = 0x06; return 0x18;
    case RX_EMULU: *ext = 0x07; return 0x1c;
    case RX_DIV:   *ext = 0x08; return 0x20;
    case RX_DIVU:  *ext = 0x09; return 0x24;
    case RX_TST:   *ext = 0x0c; return 0x30;
    case RX_XOR:   *ext = 0x0d; return 0x34;
    case RX_XCHG:  *ext = 0x10; return 0x40;
    default:       *ext = -1;   return -1;
    }
}

void rx_rr(struct code *c, int op, int rs, int rd)
{
    int s, ext;
    reg_ok(rs);
    reg_ok(rd);
    if ((op == RX_EMUL || op == RX_EMULU) && rd == 15)
        bad("emul destination pair r15:r16", rd);
    if (op == RX_MOV) {
        code_byte(c, 0xef);                  /* mov.l rs, rd: 1110 11 11 */
        code_byte(c, rs << 4 | rd);
        return;
    }
    s = short_op(op);
    if (s >= 0) {
        code_byte(c, 0x40 | s << 2 | 3);
        code_byte(c, rs << 4 | rd);
        return;
    }
    s = fc_op(op, &ext);
    if (s >= 0) {
        code_byte(c, 0xfc);
        code_byte(c, s | 3);
        code_byte(c, rs << 4 | rd);
        return;
    }
    switch (op) {
    case RX_SBB:  s = 0xfc03; break;
    case RX_NEG:  s = 0xfc07; break;
    case RX_ADC:  s = 0xfc0b; break;
    case RX_ABS:  s = 0xfc0f; break;
    case RX_NOT:  s = 0xfc3b; break;
    case RX_SHLR: s = 0xfd60; break;
    case RX_SHAR: s = 0xfd61; break;
    case RX_SHLL: s = 0xfd62; break;
    case RX_ROTR: s = 0xfd64; break;
    case RX_REVW: s = 0xfd65; break;
    case RX_ROTL: s = 0xfd66; break;
    case RX_REVL: s = 0xfd67; break;
    default: bad("register-register operation", op);
    }
    code_byte(c, s >> 8);
    code_byte(c, s & 0xff);
    code_byte(c, rs << 4 | rd);
}

void rx_r(struct code *c, int op, int rd)
{
    reg_ok(rd);
    code_byte(c, 0x7e);
    switch (op) {
    case RX_NOT: code_byte(c, 0x00 | rd); return;
    case RX_NEG: code_byte(c, 0x10 | rd); return;
    case RX_ABS: code_byte(c, 0x20 | rd); return;
    default: bad("one-register operation", op);
    }
}

/* ---- immediates --------------------------------------------------------- */

/* The FD 7x family: `fd 0111 li00 op rd`. */
static int fd7_op(int op)
{
    switch (op) {
    case RX_ADC:   return 0x2;
    case RX_MAX:   return 0x4;
    case RX_MIN:   return 0x5;
    case RX_EMUL:  return 0x6;
    case RX_EMULU: return 0x7;
    case RX_DIV:   return 0x8;
    case RX_DIVU:  return 0x9;
    case RX_TST:   return 0xc;
    case RX_XOR:   return 0xd;
    case RX_STZ:   return 0xe;
    case RX_STNZ:  return 0xf;
    default:       return -1;
    }
}

/* The `0111 01li op rd` family (AND OR MUL CMP with any immediate). */
static int i74_op(int op)
{
    switch (op) {
    case RX_CMP: return 0x0;
    case RX_MUL: return 0x1;
    case RX_AND: return 0x2;
    case RX_OR:  return 0x3;
    default:     return -1;
    }
}

/* #uimm4 forms: `0110 00op imm4 rd` for SUB CMP ADD MUL AND OR, and
 * `0110 0110 imm4 rd` for MOV. */
static int uimm4_op(int op)
{
    switch (op) {
    case RX_SUB: return 0x60;
    case RX_CMP: return 0x61;
    case RX_ADD: return 0x62;
    case RX_MUL: return 0x63;
    case RX_AND: return 0x64;
    case RX_OR:  return 0x65;
    case RX_MOV: return 0x66;
    default:     return -1;
    }
}

int rx_ri_len(int op, long imm)
{
    long v = s32(imm);
    unsigned long u = (unsigned long)v & 0xffffffffUL;
    if (uimm4_op(op) >= 0 && u <= 15)
        return 2;
    switch (op) {
    case RX_MOV:
        if (u <= 255)
            return 3;                       /* mov.l #uimm8, rd */
        return 2 + rx_li_bytes(v);
    case RX_CMP:
        if (u <= 255)
            return 3;                       /* cmp #uimm8, rd */
        return 2 + rx_li_bytes(v);
    case RX_ADD:
        return 2 + rx_li_bytes(v);          /* add #imm, rd, rd */
    case RX_SUB:
        return rx_ri_len(RX_ADD, s32(-v));
    case RX_AND: case RX_OR: case RX_MUL:
        return 2 + rx_li_bytes(v);
    default:
        if (fd7_op(op) < 0)
            bad("immediate operation", op);
        return 3 + rx_li_bytes(v);
    }
}

void rx_ri(struct code *c, int op, long imm, int rd)
{
    long v = s32(imm);
    unsigned long u = (unsigned long)v & 0xffffffffUL;
    int k;
    reg_ok(rd);
    if ((op == RX_EMUL || op == RX_EMULU) && rd == 15)
        bad("emul destination pair r15:r16", rd);
    k = uimm4_op(op);
    if (k >= 0 && u <= 15) {
        code_byte(c, k);
        code_byte(c, (int)u << 4 | rd);
        return;
    }
    switch (op) {
    case RX_MOV:
        if (u <= 255) {                     /* 0111 0101 0100 rd imm8 */
            code_byte(c, 0x75);
            code_byte(c, 0x40 | rd);
            code_byte(c, (int)u);
            return;
        }
        code_byte(c, 0xfb);                 /* 1111 1011 rd li 10 */
        code_byte(c, rd << 4 | li_field(v) << 2 | 2);
        li_bytes(c, v);
        return;
    case RX_CMP:
        if (u <= 255) {                     /* 0111 0101 0101 rs2 imm8 */
            code_byte(c, 0x75);
            code_byte(c, 0x50 | rd);
            code_byte(c, (int)u);
            return;
        }
        break;
    case RX_ADD:                            /* 0111 00li rs2 rd */
        code_byte(c, 0x70 | li_field(v));
        code_byte(c, rd << 4 | rd);
        li_bytes(c, v);
        return;
    case RX_SUB:
        /* no #simm form: add the negation (all of it -- -INT_MIN wraps
         * to itself, which is right modulo 2^32) */
        rx_ri(c, RX_ADD, s32(-v), rd);
        return;
    default:
        break;
    }
    k = i74_op(op);
    if (k >= 0) {
        code_byte(c, 0x74 | li_field(v));
        code_byte(c, k << 4 | rd);
        li_bytes(c, v);
        return;
    }
    k = fd7_op(op);
    if (k < 0)
        bad("immediate operation", op);
    code_byte(c, 0xfd);
    code_byte(c, 0x70 | li_field(v) << 2);
    code_byte(c, k << 4 | rd);
    li_bytes(c, v);
}

int rx_mov_abs(struct code *c, int rd, unsigned long imm)
{
    int at;
    reg_ok(rd);
    code_byte(c, 0xfb);
    code_byte(c, rd << 4 | 0 << 2 | 2);     /* li 00: four bytes */
    at = c->len;
    code_u32(c, imm & 0xffffffffUL);
    return at;
}

/* ---- three operands -------------------------------------------------------- */

void rx_rrr(struct code *c, int op, int rs, int rs2, int rd)
{
    int k;
    reg_ok(rs); reg_ok(rs2); reg_ok(rd);
    switch (op) {
    case RX_SUB: k = 0x0; break;
    case RX_ADD: k = 0x2; break;
    case RX_MUL: k = 0x3; break;
    case RX_AND: k = 0x4; break;
    case RX_OR:  k = 0x5; break;
    default: bad("three-operand operation", op);
    }
    code_byte(c, 0xff);
    code_byte(c, k << 4 | rd);
    code_byte(c, rs << 4 | rs2);
}

int rx_add3_len(long imm, int rs, int rd)
{
    long v = s32(imm);
    if (rs == rd && v >= 0 && v <= 15)
        return 2;
    return 2 + rx_li_bytes(v);
}

void rx_add3(struct code *c, long imm, int rs, int rd)
{
    long v = s32(imm);
    reg_ok(rs); reg_ok(rd);
    if (rs == rd) {
        rx_ri(c, RX_ADD, v, rd);
        return;
    }
    code_byte(c, 0x70 | li_field(v));       /* 0111 00li rs2 rd */
    code_byte(c, rs << 4 | rd);
    li_bytes(c, v);
}

/* ---- shifts ------------------------------------------------------------- */

void rx_shift_i(struct code *c, int op, int n, int rs, int rd)
{
    int k;
    reg_ok(rs); reg_ok(rd);
    if (n < 0 || n > 31)
        bad("shift count", n);
    if (op == RX_ROTL || op == RX_ROTR) {
        if (rs != rd)
            bad("rotate with a separate source", rs);
        code_byte(c, 0xfd);
        code_byte(c, (op == RX_ROTL ? 0x6e : 0x6c) | n >> 4);
        code_byte(c, (n & 15) << 4 | rd);
        return;
    }
    switch (op) {
    case RX_SHLR: k = 0; break;
    case RX_SHAR: k = 1; break;
    case RX_SHLL: k = 2; break;
    default: bad("shift operation", op);
    }
    if (rs == rd) {                          /* 0110 1kk i iiii rd */
        code_byte(c, 0x68 | k << 1 | n >> 4);
        code_byte(c, (n & 15) << 4 | rd);
        return;
    }
    code_byte(c, 0xfd);                     /* fd 1kk iiiii rs rd */
    code_byte(c, 0x80 | k << 5 | n);
    code_byte(c, rs << 4 | rd);
}

/* ---- extensions ---------------------------------------------------------- */

void rx_ext(struct code *c, int size, int sign, int rs, int rd)
{
    reg_ok(rs); reg_ok(rd);
    if (size != RX_B && size != RX_W)
        bad("extension size", size);
    if (sign) {                             /* mov.b/w rs, rd: 11sz 11 11 */
        code_byte(c, (0xc + size) << 4 | 0x0f);
    } else {                                /* movu.b/w rs, rd: 0101 1s 11 */
        code_byte(c, 0x58 | size << 2 | 3);
    }
    code_byte(c, rs << 4 | rd);
}

/* ---- memory ------------------------------------------------------------- */

void rx_load(struct code *c, int size, int sign, long dsp, int rs, int rd)
{
    long u = units_of(size, dsp);
    reg_ok(rs); reg_ok(rd);
    if (size == RX_L)
        sign = 1;
    if (!sign && u > 32767)
        bad("movu displacement QEMU would read as negative", dsp);
    if (u >= 1 && u <= 31 && rs < 8 && rd < 8) {
        /* the dsp:5 form: 10sz 1 ddd / d rs d rd (mov), 1011 s ddd ...
         * (movu) */
        int b0 = sign ? (0x8 + size) << 4 | 0x08 : 0xb0 | size << 3;
        code_byte(c, b0 | (int)(u >> 2));
        code_byte(c, (int)(u >> 1 & 1) << 7 | rs << 4 | (int)(u & 1) << 3 |
                     rd);
        return;
    }
    if (sign)
        code_byte(c, (0xc + size) << 4 | 0x0c | ld_field(u));
    else
        code_byte(c, 0x58 | size << 2 | ld_field(u));
    code_byte(c, rs << 4 | rd);
    ld_bytes(c, u);
}

void rx_store(struct code *c, int size, int rs, long dsp, int rd)
{
    long u = units_of(size, dsp);
    reg_ok(rs); reg_ok(rd);
    if (u >= 1 && u <= 31 && rs < 8 && rd < 8) {
        code_byte(c, (0x8 + size) << 4 | (int)(u >> 2));
        code_byte(c, (int)(u >> 1 & 1) << 7 | rd << 4 | (int)(u & 1) << 3 |
                     rs);
        return;
    }
    /* the BASE in the high nibble and the value in the low one, as GNU
     * as encodes it and QEMU executes it (QEMU's disassembler prints the
     * two the other way round) */
    code_byte(c, (0xc + size) << 4 | ld_field(u) << 2 | 3);
    code_byte(c, rd << 4 | rs);
    ld_bytes(c, u);
}

void rx_store_imm(struct code *c, int size, long imm, long dsp, int rd)
{
    long u = units_of(size, dsp);
    long v = s32(imm);
    reg_ok(rd);
    if (u > 32767)
        bad("mov #imm displacement QEMU would read as negative", dsp);
    /* the value as the access stores it: .b and .w keep their low bits,
     * and the li field holds them sign-extended */
    if (size == RX_B) v = (long)(signed char)(v & 0xff);
    if (size == RX_W) v = (long)(short)(v & 0xffff);
    if (u >= 1 && u <= 31 && rd < 8 && (unsigned long)(v & (size == RX_B ? 0xff :
                                                  size == RX_W ? 0xffff :
                                                  0xffffffffL)) <= 255 &&
        (size == RX_B || v >= 0)) {
        /* mov.size #uimm8, dsp:5[rd]: 0011 11sz d rd dddd imm8 */
        code_byte(c, 0x3c | size);
        code_byte(c, (int)(u >> 4 & 1) << 7 | rd << 4 | (int)(u & 15));
        code_byte(c, (int)(v & 0xff));
        return;
    }
    code_byte(c, 0xf8 | ld_field(u));
    code_byte(c, rd << 4 | li_field(v) << 2 | size);
    ld_bytes(c, u);
    li_bytes(c, v);
}

void rx_load_idx(struct code *c, int size, int sign, int ri, int rb, int rd)
{
    reg_ok(ri); reg_ok(rb); reg_ok(rd);
    if (size < RX_B || size > RX_L)
        bad("access size", size);
    code_byte(c, 0xfe);
    if (sign || size == RX_L)
        code_byte(c, 0x40 | size << 4 | ri);         /* 01 sz ri */
    else
        code_byte(c, 0xc0 | size << 4 | ri);         /* 110 s ri */
    code_byte(c, rb << 4 | rd);
}

void rx_store_idx(struct code *c, int size, int rs, int ri, int rb)
{
    reg_ok(ri); reg_ok(rb); reg_ok(rs);
    if (size < RX_B || size > RX_L)
        bad("access size", size);
    code_byte(c, 0xfe);
    code_byte(c, size << 4 | ri);                    /* 00 sz ri */
    code_byte(c, rb << 4 | rs);
}

/* The memex source: `.ub` has its own short encodings; .b .w .l .uw are
 * 06 mi..ld. mi: 0 .b, 1 .w, 2 .l, 3 .uw; the scale is the access's. */
static int memex_mi(int size, int sign, int *scale)
{
    if (size == RX_B && !sign) { *scale = 1; return 4; }
    if (size == RX_B)          { *scale = 1; return 0; }
    if (size == RX_W)          { *scale = 2; return sign ? 1 : 3; }
    if (size == RX_L)          { *scale = 4; return 2; }
    bad("memory operand size", size);
}

int rx_rm_ok(int op, int size, int sign, long dsp)
{
    int scale, ext, mi;
    long u;
    if (short_op(op) < 0 && fc_op(op, &ext) < 0)
        return 0;
    if (size < RX_B || size > RX_L)
        return 0;
    mi = memex_mi(size, sign, &scale);
    (void)mi;
    return scaled(scale, dsp, &u);
}

void rx_rm(struct code *c, int op, int size, int sign, long dsp, int rs,
           int rd)
{
    int scale, ext, mi = memex_mi(size, sign, &scale);
    int s = short_op(op), f = fc_op(op, &ext);
    long u;
    reg_ok(rs); reg_ok(rd);
    if (!scaled(scale, dsp, &u))
        bad("memory operand displacement", dsp);
    if (s < 0 && f < 0)
        bad("memory-operand operation", op);
    if ((op == RX_EMUL || op == RX_EMULU) && rd == 15)
        bad("emul destination pair r15:r16", rd);
    if (mi == 4) {                           /* .ub */
        if (s >= 0) {
            code_byte(c, 0x40 | s << 2 | ld_field(u));
        } else {
            code_byte(c, 0xfc);
            code_byte(c, f | ld_field(u));
        }
        code_byte(c, rs << 4 | rd);
        ld_bytes(c, u);
        return;
    }
    code_byte(c, 0x06);
    if (s >= 0) {
        code_byte(c, mi << 6 | s << 2 | ld_field(u));
    } else {
        code_byte(c, mi << 6 | 0x20 | ld_field(u));
        code_byte(c, ext);
    }
    code_byte(c, rs << 4 | rd);
    ld_bytes(c, u);
}

/* ---- the stack ---------------------------------------------------------- */

void rx_push(struct code *c, int rs)
{
    reg_ok(rs);
    code_byte(c, 0x7e);
    code_byte(c, 0xa0 | rs);                 /* push.l: 10 sz=10 rs */
}

void rx_pop(struct code *c, int rd)
{
    reg_ok(rd);
    code_byte(c, 0x7e);
    code_byte(c, 0xb0 | rd);
}

void rx_pushm(struct code *c, int rs, int rs2)
{
    if (rs < 1 || rs >= rs2 || rs2 > 15)
        bad("pushm register range", rs * 100 + rs2);
    code_byte(c, 0x6e);
    code_byte(c, rs << 4 | rs2);
}

void rx_popm(struct code *c, int rd, int rd2)
{
    if (rd < 1 || rd >= rd2 || rd2 > 15)
        bad("popm register range", rd * 100 + rd2);
    code_byte(c, 0x6f);
    code_byte(c, rd << 4 | rd2);
}

void rx_rts(struct code *c)
{
    code_byte(c, 0x02);
}

void rx_rtsd(struct code *c, long bytes)
{
    if (bytes < 0 || bytes > 1020 || bytes % 4)
        bad("rtsd size", bytes);
    code_byte(c, 0x67);
    code_byte(c, (int)(bytes / 4));
}

void rx_rtsd_m(struct code *c, long bytes, int rd, int rd2)
{
    if (rd < 1 || rd > rd2 || rd2 > 15)
        bad("rtsd register range", rd * 100 + rd2);
    if (bytes < 4L * (rd2 - rd + 1) || bytes > 1020 || bytes % 4)
        bad("rtsd size", bytes);
    code_byte(c, 0x3f);
    code_byte(c, rd << 4 | rd2);
    code_byte(c, (int)(bytes / 4));
}

/* ---- transfers ------------------------------------------------------------ */

void rx_jmp(struct code *c, int rs)   { reg_ok(rs); code_byte(c, 0x7f); code_byte(c, 0x00 | rs); }
void rx_jsr(struct code *c, int rs)   { reg_ok(rs); code_byte(c, 0x7f); code_byte(c, 0x10 | rs); }
void rx_bra_l(struct code *c, int rs) { reg_ok(rs); code_byte(c, 0x7f); code_byte(c, 0x40 | rs); }
void rx_bsr_l(struct code *c, int rs) { reg_ok(rs); code_byte(c, 0x7f); code_byte(c, 0x50 | rs); }

static void d24(struct code *c, long disp)
{
    if (disp < -8388608L || disp > 8388607L)
        bad("24-bit branch displacement", disp);
    code_byte(c, (int)(disp & 0xff));
    code_byte(c, (int)(disp >> 8 & 0xff));
    code_byte(c, (int)(disp >> 16 & 0xff));
}

static void d16(struct code *c, long disp)
{
    if (disp < -32768L || disp > 32767L)
        bad("16-bit branch displacement", disp);
    code_byte(c, (int)(disp & 0xff));
    code_byte(c, (int)(disp >> 8 & 0xff));
}

int rx_bsr_a(struct code *c)
{
    int at = c->len;
    code_byte(c, 0x05);
    d24(c, 0);
    return at;
}

int rx_bra_a(struct code *c)
{
    int at = c->len;
    code_byte(c, 0x04);
    d24(c, 0);
    return at;
}

void rx_bsr_w_d(struct code *c, long disp)
{
    code_byte(c, 0x39);
    d16(c, disp);
}

void rx_bsr_a_d(struct code *c, long disp)
{
    code_byte(c, 0x05);
    d24(c, disp);
}

int rx_branch_len(int kind)
{
    switch (kind) {
    case RX_BR_S: return 1;
    case RX_BR_B: return 2;
    case RX_BR_W: return 3;
    case RX_BR_A: return 4;
    default: bad("branch kind", kind);
    }
}

int rx_branch_ok(int kind, int cond)
{
    if (cond < 0 || cond > RX_ALWAYS)
        return 0;
    switch (kind) {
    case RX_BR_S:
    case RX_BR_W: return cond == RX_EQ || cond == RX_NE || cond == RX_ALWAYS;
    case RX_BR_B: return 1;
    case RX_BR_A: return cond == RX_ALWAYS;
    default:      return 0;
    }
}

int rx_branch_reaches(int kind, long disp)
{
    switch (kind) {
    case RX_BR_S: return disp >= 3 && disp <= 10;
    case RX_BR_B: return disp >= -128 && disp <= 127;
    case RX_BR_W: return disp >= -32768 && disp <= 32767;
    case RX_BR_A: return disp >= -8388608L && disp <= 8388607L;
    default:      return 0;
    }
}

/* Emit the branch with `disp` in place. */
void rx_branch_d(struct code *c, int kind, int cond, long disp)
{
    if (!rx_branch_ok(kind, cond))
        bad("branch form for this condition", cond);
    if (!rx_branch_reaches(kind, disp))
        bad("branch displacement", disp);
    switch (kind) {
    case RX_BR_S:
        /* 3..10 in three bits: 8, 9, 10 are stored as 0, 1, 2 */
        if (cond == RX_ALWAYS)
            code_byte(c, 0x08 | (int)(disp & 7));
        else
            code_byte(c, 0x10 | cond << 3 | (int)(disp & 7));
        return;
    case RX_BR_B:
        code_byte(c, 0x20 | cond);
        code_byte(c, (int)(disp & 0xff));
        return;
    case RX_BR_W:
        code_byte(c, cond == RX_ALWAYS ? 0x38 : 0x3a | cond);
        d16(c, disp);
        return;
    default:
        code_byte(c, 0x04);
        d24(c, disp);
        return;
    }
}

int rx_branch(struct code *c, int kind, int cond)
{
    int at = c->len;
    /* a placeholder that is itself a valid branch: to the next byte for
     * .s (encoded as 8, patched before anything runs), else to itself */
    rx_branch_d(c, kind, cond, kind == RX_BR_S ? 8 : 0);
    return at;
}

int rx_patch_branch(struct code *c, int at, int target)
{
    int b0 = c->p[at];
    long disp = (long)target - at;
    int kind, cond;
    if (b0 >= 0x08 && b0 <= 0x0f)      { kind = RX_BR_S; cond = RX_ALWAYS; }
    else if (b0 >= 0x10 && b0 <= 0x1f) { kind = RX_BR_S; cond = b0 >> 3 & 1; }
    else if (b0 >= 0x20 && b0 <= 0x2f) { kind = RX_BR_B; cond = b0 & 15; }
    else if (b0 == 0x38)               { kind = RX_BR_W; cond = RX_ALWAYS; }
    else if (b0 == 0x3a || b0 == 0x3b) { kind = RX_BR_W; cond = b0 & 1; }
    else if (b0 == 0x04)               { kind = RX_BR_A; cond = RX_ALWAYS; }
    else internal_error("rx: patching a branch at %d that is not one "
                        "(0x%02x)", at, b0);
    if (!rx_branch_reaches(kind, disp))
        return 0;
    {
        int len = c->len;
        c->len = at;
        rx_branch_d(c, kind, cond, disp);
        c->len = len;
    }
    return 1;
}

/* ---- flags into a register ---------------------------------------------- */

void rx_scc(struct code *c, int cond, int rd)
{
    reg_ok(rd);
    if (cond < 0 || cond > RX_NO)
        bad("sccnd condition", cond);
    code_byte(c, 0xfc);
    code_byte(c, 0xd0 | RX_L << 2 | 3);      /* 1101 sz ld=11 */
    code_byte(c, rd << 4 | cond);
}

/* ---- control ------------------------------------------------------------ */

void rx_nop(struct code *c)  { code_byte(c, 0x03); }
void rx_brk(struct code *c)  { code_byte(c, 0x00); }
void rx_wait(struct code *c) { code_byte(c, 0x7f); code_byte(c, 0x96); }
void rx_rte(struct code *c)  { code_byte(c, 0x7f); code_byte(c, 0x95); }

void rx_int(struct code *c, int n)
{
    if (n < 0 || n > 255)
        bad("int vector", n);
    code_byte(c, 0x75);
    code_byte(c, 0x60);
    code_byte(c, n);
}

static void psw_bit_ok(int bit)
{
    if (!(bit >= 0 && bit <= 3) && bit != 8 && bit != 9)
        bad("psw flag", bit);
}

void rx_setpsw(struct code *c, int bit)
{
    psw_bit_ok(bit);
    code_byte(c, 0x7f);
    code_byte(c, 0xa0 | bit);
}

void rx_clrpsw(struct code *c, int bit)
{
    psw_bit_ok(bit);
    code_byte(c, 0x7f);
    code_byte(c, 0xb0 | bit);
}

static void cr_ok(int cr, int writing)
{
    if (cr < 0 || cr > 12 || (cr > 3 && cr < 8) || (writing && cr == 1))
        bad("control register", cr);
}

void rx_mvtc(struct code *c, int rs, int cr)
{
    reg_ok(rs);
    cr_ok(cr, 1);
    code_byte(c, 0xfd);
    code_byte(c, 0x68);
    code_byte(c, rs << 4 | cr);
}

void rx_mvtc_i(struct code *c, long imm, int cr)
{
    long v = s32(imm);
    cr_ok(cr, 1);
    code_byte(c, 0xfd);
    code_byte(c, 0x73 | li_field(v) << 2);
    code_byte(c, cr);
    li_bytes(c, v);
}

void rx_mvfc(struct code *c, int cr, int rd)
{
    reg_ok(rd);
    cr_ok(cr, 0);
    code_byte(c, 0xfd);
    code_byte(c, 0x6a);
    code_byte(c, cr << 4 | rd);
}

void rx_mvtipl(struct code *c, int ipl)
{
    if (ipl < 0 || ipl > 15)
        bad("interrupt priority level", ipl);
    code_byte(c, 0x75);
    code_byte(c, 0x70);
    code_byte(c, ipl);
}

/* ---- bits ------------------------------------------------------------- */

void rx_bit_i(struct code *c, int op, int bit, int rd)
{
    reg_ok(rd);
    if (bit < 0 || bit > 31)
        bad("bit number", bit);
    switch (op) {
    case RX_BSET: code_byte(c, 0x78 | bit >> 4); break;
    case RX_BCLR: code_byte(c, 0x7a | bit >> 4); break;
    case RX_BTST: code_byte(c, 0x7c | bit >> 4); break;
    case RX_BNOT:
        code_byte(c, 0xfd);
        code_byte(c, 0xe0 | bit);
        code_byte(c, 0xf0 | rd);
        return;
    default: bad("bit operation", op);
    }
    code_byte(c, (bit & 15) << 4 | rd);
}

void rx_bmcnd(struct code *c, int cond, int bit, int rd)
{
    reg_ok(rd);
    if (bit < 0 || bit > 31)
        bad("bit number", bit);
    if (cond < 0 || cond > RX_NO)
        bad("bmcnd condition", cond);
    code_byte(c, 0xfd);
    code_byte(c, 0xe0 | bit);
    code_byte(c, cond << 4 | rd);
}
