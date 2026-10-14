/* The RISC-V inline-asm assembler. See asm.h for what its vocabulary is
 * and where it came from.
 *
 * Every instruction goes out through src/arch/riscv/emit.c rather than
 * being encoded here. That is the whole design: the code generator and
 * inline assembly then cannot disagree about a bit, and tools/riscvcheck
 * already round-trips those encoders through llvm-mc. The only thing
 * this file adds is a PARSER and the CSR vocabulary.
 */
#define _POSIX_C_SOURCE 200809L
#include "asm.h"

#include "emit.h"
#include "../target.h"
#include "../asmexpr.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* ---- registers ---------------------------------------------------------- */

static const char *const abi_name[32] = {
    "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2",
    "s0",   "s1", "a0", "a1", "a2", "a3", "a4", "a5",
    "a6",   "a7", "s2", "s3", "s4", "s5", "s6", "s7",
    "s8",   "s9", "s10", "s11", "t3", "t4", "t5", "t6"
};

int rvasm_gpr(const char *name, int len)
{
    if (len <= 0)
        return -1;
    for (int i = 0; i < 32; i++)
        if ((int)strlen(abi_name[i]) == len &&
            strncmp(name, abi_name[i], (size_t)len) == 0)
            return i;
    /* `fp` is s0 under another name, and hand-written asm uses both. */
    if (len == 2 && strncmp(name, "fp", 2) == 0)
        return 8;
    if (name[0] == 'x' && len >= 2 && len <= 3) {
        int v = 0;
        for (int i = 1; i < len; i++) {
            if (!isdigit((unsigned char)name[i]))
                return -1;
            v = v * 10 + (name[i] - '0');
        }
        return v < 32 ? v : -1;
    }
    return -1;
}

const char *rv_reg_name(int reg)
{
    return (reg >= 0 && reg < 32) ? abi_name[reg] : "?";
}

/* ---- the CSR vocabulary -------------------------------------------------
 *
 * Named rather than numeric, because that is how a program writes them
 * and because a number this file does not know is far more likely to be a
 * typo than a real register. Machine mode first -- it is where a
 * bare-metal program lives -- then the supervisor set a program using an
 * SBI would touch, then the unprivileged counters.
 */
/* `rv32` marks a CSR that exists ONLY at RV32 -- the high halves of the
 * 64-bit counters, which RV64 reads in one go, and the odd-numbered
 * pmpcfg registers, which RV64 does not have because each of its entries
 * covers twice as much. llvm-mc refuses them at RV64, and so does this:
 * encoding a register the hardware has not got is exactly the guess THE
 * RULE forbids. */
struct csr { const char *name; unsigned num; int rv32; };
static const struct csr csrs[] = {
    /* the floating-point CSRs (the F extension) */
    { "fflags", 0x001, 0 }, { "frm", 0x002, 0 }, { "fcsr", 0x003, 0 },
    /* machine information */
    { "mvendorid", 0xf11, 0 }, { "marchid", 0xf12, 0 },
    { "mimpid", 0xf13, 0 },    { "mhartid", 0xf14, 0 },
    /* machine trap setup */
    { "mstatus", 0x300, 0 }, { "misa",  0x301, 0 }, { "medeleg", 0x302, 0 },
    { "mideleg", 0x303, 0 }, { "mie",   0x304, 0 }, { "mtvec",   0x305, 0 },
    { "mcounteren", 0x306, 0 },
    /* machine trap handling */
    { "mscratch", 0x340, 0 }, { "mepc",  0x341, 0 }, { "mcause", 0x342, 0 },
    { "mtval",    0x343, 0 }, { "mip",   0x344, 0 },
    /* physical memory protection */
    { "pmpcfg0", 0x3a0, 0 }, { "pmpcfg1", 0x3a1, 1 },
    { "pmpaddr0", 0x3b0, 0 }, { "pmpaddr1", 0x3b1, 0 },
    /* supervisor */
    { "sstatus", 0x100, 0 }, { "sie",   0x104, 0 }, { "stvec", 0x105, 0 },
    { "scounteren", 0x106, 0 },
    { "sscratch", 0x140, 0 }, { "sepc",  0x141, 0 }, { "scause", 0x142, 0 },
    { "stval",    0x143, 0 }, { "sip",   0x144, 0 }, { "satp",   0x180, 0 },
    /* machine counters: what a trace's clock or a cycle count reads in
     * machine mode, where cycle/instret need mcounteren to be readable */
    { "mcycle", 0xb00, 0 }, { "minstret", 0xb02, 0 },
    { "mcycleh", 0xb80, 1 }, { "minstreth", 0xb82, 1 },
    { "mcountinhibit", 0x320, 0 },
    /* unprivileged counters, which a timing loop reads */
    { "cycle", 0xc00, 0 }, { "time", 0xc01, 0 }, { "instret", 0xc02, 0 },
    { "cycleh", 0xc80, 1 }, { "timeh", 0xc81, 1 }, { "instreth", 0xc82, 1 },
    { NULL, 0, 0 }
};

static int csr_num(const char *s, int len)
{
    for (const struct csr *c = csrs; c->name; c++)
        if ((int)strlen(c->name) == len &&
            strncmp(s, c->name, (size_t)len) == 0) {
            if (c->rv32 && target_xlen() != 32)
                return -2;          /* exists, but not on this width */
            return (int)c->num;
        }
    /* A bare number is allowed, because a new CSR appears faster than a
     * table does -- but only in range, so a typo is still caught. */
    if (len > 0 && isdigit((unsigned char)s[0])) {
        long v = 0;
        int i = 0, base = 10;
        if (len > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            base = 16; i = 2;
        }
        for (; i < len; i++) {
            int d;
            if (isdigit((unsigned char)s[i])) d = s[i] - '0';
            else if (base == 16 && isxdigit((unsigned char)s[i]))
                d = tolower((unsigned char)s[i]) - 'a' + 10;
            else return -1;
            v = v * base + d;
        }
        return (v >= 0 && v <= 0xfff) ? (int)v : -1;
    }
    return -1;
}

/* ---- a tiny tokeniser --------------------------------------------------- */

#define MAXTOK 8

struct tok { const char *s; int len; };

static int split(const char *stmt, int len, struct tok *t, int max)
{
    int n = 0, i = 0;
    while (i < len && n < max) {
        while (i < len && (isspace((unsigned char)stmt[i]) || stmt[i] == ','))
            i++;
        if (i >= len)
            break;
        t[n].s = stmt + i;
        /* An operand may be `off(reg)`, so a parenthesis is part of the
         * token rather than a separator. And past the mnemonic an operand
         * runs to the next comma, spaces and all, as in GNU as: FreeRTOS's
         * port writes `-( portCONTEXT_SIZE )` and `1 * 4( sp )`. */
        {
            int depth = 0, operand = n > 0;
            while (i < len && (depth > 0 ||
                   ((operand || !isspace((unsigned char)stmt[i])) &&
                    stmt[i] != ','))) {
                if (stmt[i] == '(') depth++;
                else if (stmt[i] == ')') depth--;
                i++;
            }
        }
        t[n].len = (int)(stmt + i - t[n].s);
        while (t[n].len > 0 && isspace((unsigned char)t[n].s[t[n].len - 1]))
            t[n].len--;
        if (t[n].len > 0)
            n++;
    }
    return n;
}

static int tok_is(const struct tok *t, const char *s)
{
    return (int)strlen(s) == t->len && strncmp(t->s, s, (size_t)t->len) == 0;
}

static int tok_reg(const struct tok *t) { return rvasm_gpr(t->s, t->len); }

/* A signed immediate: decimal or 0x with an optional sign, or a constant
 * expression (asmexpr.h). */
static int tok_imm(const struct tok *t, long long *out)
{
    int i = 0, neg = 0, base = 10, any = 0;
    long long v = 0;
    /* An expression: anything past a leading sign that is not a digit of
     * one number (`.+8`, a displacement, is the plain path below). */
    if (t->len > 0 && t->s[0] != '.') {
        for (int j = 0; j < t->len; j++)
            if (strchr("()~<>|&^*/% ", t->s[j]) ||
                (j > 0 && (t->s[j] == '+' || t->s[j] == '-')))
                return asm_const_expr(t->s, t->len, out);
    }
    /* `.+8` / `.-12`: a displacement from this instruction, which is
     * the standard spelling and the ONLY meaning a branch operand can
     * have here. This assembler sees no labels and does not know its
     * own address -- the file assembler (src/as/gas.c) resolves a
     * label into exactly this form before calling, and an inline asm
     * template could never have named a surrounding label anyway. The
     * bare number is accepted as the same thing, so `beq a0, a1, 8`
     * and `beq a0, a1, .+8` agree. */
    if (i < t->len && t->s[i] == '.')
        i++;
    if (i < t->len && (t->s[i] == '-' || t->s[i] == '+')) {
        neg = t->s[i] == '-';
        i++;
    }
    if (i + 1 < t->len && t->s[i] == '0' &&
        (t->s[i + 1] == 'x' || t->s[i + 1] == 'X')) {
        base = 16;
        i += 2;
    }
    for (; i < t->len; i++) {
        int d;
        if (isdigit((unsigned char)t->s[i])) d = t->s[i] - '0';
        else if (base == 16 && isxdigit((unsigned char)t->s[i]))
            d = tolower((unsigned char)t->s[i]) - 'a' + 10;
        else return 0;
        v = v * base + d;
        any = 1;
    }
    if (!any)
        return 0;
    *out = neg ? -v : v;
    return 1;
}

/* `off(reg)` or `(reg)`, as a load or store writes its address. The
 * register is the LAST parenthesised group -- the offset may have its
 * own parentheses, `(2 * 4)(sp)` -- and spaces may surround it. */
static int tok_mem(const struct tok *t, int *reg, long long *off)
{
    const char *open = NULL;
    struct tok o, r;
    if (t->len < 3 || t->s[t->len - 1] != ')')
        return 0;
    {
        int depth = 0;
        for (const char *q = t->s + t->len - 1; q >= t->s; q--) {
            if (*q == ')') depth++;
            else if (*q == '(' && --depth == 0) { open = q; break; }
        }
    }
    if (!open)
        return 0;
    o.s = t->s; o.len = (int)(open - t->s);
    while (o.len > 0 && isspace((unsigned char)o.s[o.len - 1]))
        o.len--;
    r.s = open + 1; r.len = (int)(t->s + t->len - 1 - (open + 1));
    while (r.len > 0 && isspace((unsigned char)*r.s)) { r.s++; r.len--; }
    while (r.len > 0 && isspace((unsigned char)r.s[r.len - 1]))
        r.len--;
    *off = 0;
    if (o.len > 0 && !tok_imm(&o, off))
        return 0;
    *reg = tok_reg(&r);
    return *reg >= 0;
}

/* ---- the instruction table ----------------------------------------------
 *
 * Every entry hands off to an rv_* encoder. The ALU and load/store forms
 * are the code generator's own; only the CSR and system instructions are
 * encoded here, and those are three fields in a fixed format.
 */
enum { OP_SYSTEM = 0x73, OP_FENCE = 0x0f,
       OP_BRANCH = 0x63, OP_JAL = 0x6f };

static void emit_csr(struct code *c, int f3, int rd, int rs1, unsigned csr)
{
    code_u32(c, (unsigned long)OP_SYSTEM | ((unsigned long)rd << 7) |
                ((unsigned long)f3 << 12) | ((unsigned long)rs1 << 15) |
                ((unsigned long)csr << 20));
}

struct alu_ent { const char *name; int op; int imm; int w; };
static const struct alu_ent alu_tab[] = {
    { "add", RV_ADD, 0, 0 },  { "addi", RV_ADD, 1, 0 },
    { "sub", RV_SUB, 0, 0 },
    { "and", RV_AND, 0, 0 },  { "andi", RV_AND, 1, 0 },
    { "or",  RV_OR,  0, 0 },  { "ori",  RV_OR,  1, 0 },
    { "xor", RV_XOR, 0, 0 },  { "xori", RV_XOR, 1, 0 },
    { "slt", RV_SLT, 0, 0 },  { "slti", RV_SLT, 1, 0 },
    { "sltu", RV_SLTU, 0, 0 }, { "sltiu", RV_SLTU, 1, 0 },
    { "addw", RV_ADD, 0, 1 }, { "addiw", RV_ADD, 1, 1 },
    { "subw", RV_SUB, 0, 1 },
    { NULL, 0, 0, 0 }
};

struct sh_ent { const char *name; int op; int imm; int w; };
static const struct sh_ent sh_tab[] = {
    { "sll", RV_SLL, 0, 0 }, { "slli", RV_SLL, 1, 0 },
    { "srl", RV_SRL, 0, 0 }, { "srli", RV_SRL, 1, 0 },
    { "sra", RV_SRA, 0, 0 }, { "srai", RV_SRA, 1, 0 },
    { "sllw", RV_SLL, 0, 1 }, { "slliw", RV_SLL, 1, 1 },
    { "srlw", RV_SRL, 0, 1 }, { "srliw", RV_SRL, 1, 1 },
    { "sraw", RV_SRA, 0, 1 }, { "sraiw", RV_SRA, 1, 1 },
    { NULL, 0, 0, 0 }
};

struct md_ent { const char *name; int op; int w; };
static const struct md_ent md_tab[] = {
    { "mul", RV_MUL, 0 },   { "mulh", RV_MULH, 0 },
    { "mulhsu", RV_MULHSU, 0 }, { "mulhu", RV_MULHU, 0 },
    { "div", RV_DIV, 0 },   { "divu", RV_DIVU, 0 },
    { "rem", RV_REM, 0 },   { "remu", RV_REMU, 0 },
    { "mulw", RV_MUL, 1 },  { "divw", RV_DIV, 1 },
    { "divuw", RV_DIVU, 1 }, { "remw", RV_REM, 1 },
    { "remuw", RV_REMU, 1 },
    { NULL, 0, 0 }
};

struct ls_ent { const char *name; int size; int sign; int store; };
static const struct ls_ent ls_tab[] = {
    { "lb", 1, 1, 0 }, { "lbu", 1, 0, 0 },
    { "lh", 2, 1, 0 }, { "lhu", 2, 0, 0 },
    { "lw", 4, 1, 0 }, { "lwu", 4, 0, 0 },
    { "ld", 8, 1, 0 },
    { "sb", 1, 0, 1 }, { "sh", 2, 0, 1 },
    { "sw", 4, 0, 1 }, { "sd", 8, 0, 1 },
    { NULL, 0, 0, 0 }
};

struct csr_ent { const char *name; int f3; int form; };
/* form: 0 = csrr rd, csr        1 = csrw csr, rs
 *       2 = csrrw rd, csr, rs   3 = an immediate variant of 1
 *       4 = an immediate variant of 2 */
static const struct csr_ent csr_tab[] = {
    { "csrr",   2, 0 }, { "csrw",   1, 1 }, { "csrs",  2, 1 },
    { "csrc",   3, 1 },
    { "csrrw",  1, 2 }, { "csrrs",  2, 2 }, { "csrrc", 3, 2 },
    { "csrwi",  5, 3 }, { "csrsi",  6, 3 }, { "csrci", 7, 3 },
    { "csrrwi", 5, 4 }, { "csrrsi", 6, 4 }, { "csrrci", 7, 4 },
    { NULL, 0, 0 }
};

/* ---- the F and D extensions ---------------------------------------------
 *
 * Every form a hand-written FPU routine uses, by the GNU spelling: loads
 * and stores, the arithmetic with an optional rounding mode, the fused
 * multiply-adds, min/max and sign injection (and fmv/fneg/fabs, which are
 * sign injections), compares and fclass, every conversion, the moves
 * between the register files, and the fcsr/frm/fflags pseudos. Each goes
 * out through emit.c's OP-FP encoders, as the code generator's do, and
 * the referee is llvm-mc (rvasm_vocabulary). An F instruction on a target
 * whose -march= names no F -- or a D one without D -- is refused, as GNU
 * as refuses it: the part would trap on it. */

/* f0-f31, or the ABI names ft0-ft11, fs0-fs11 and fa0-fa7. */
int rvasm_fpr(const char *name, int len)
{
    int v = 0, i;
    if (len < 2 || name[0] != 'f')
        return -1;
    i = isdigit((unsigned char)name[1]) ? 1 : 2;
    if (i == 2 && len < 3)
        return -1;
    for (int k = i; k < len; k++) {
        if (!isdigit((unsigned char)name[k]))
            return -1;
        v = v * 10 + (name[k] - '0');
    }
    if (len - i > 2)
        return -1;
    if (i == 1)
        return v < 32 ? v : -1;
    switch (name[1]) {
    case 't': return v < 8 ? v : v < 12 ? 28 + (v - 8) : -1;
    case 's': return v < 2 ? 8 + v : v < 12 ? 18 + (v - 2) : -1;
    case 'a': return v < 8 ? 10 + v : -1;
    default:  return -1;
    }
}

static int tok_freg(const struct tok *t) { return rvasm_fpr(t->s, t->len); }

/* A rounding-mode operand: its rm field, or -1 */
static int tok_rm(const struct tok *t)
{
    static const char *const rm[8] = { "rne", "rtz", "rdn", "rup", "rmm",
                                       NULL, NULL, "dyn" };
    for (int i = 0; i < 8; i++)
        if (rm[i] && tok_is(t, rm[i]))
            return i;
    return -1;
}

/* Split a mnemonic at its dots: "fcvt.w.s" -> "fcvt", "w", "s". */
struct fpname { char base[12], a[4], b[4]; int nparts; };

static int fp_split(const struct tok *t, struct fpname *p)
{
    int i = 0, part = 0, k = 0;
    char *dst = p->base;
    int cap = (int)sizeof p->base;
    memset(p, 0, sizeof *p);
    for (; i < t->len; i++) {
        char ch = t->s[i];
        if (ch == '.') {
            if (++part > 2)
                return 0;
            dst = part == 1 ? p->a : p->b;
            cap = 4;
            k = 0;
            continue;
        }
        if (k + 1 >= cap)
            return 0;
        dst[k++] = ch;
        dst[k] = 0;
    }
    p->nparts = part + 1;
    return 1;
}

/* the format suffix: 0 single, 1 double, -1 neither */
static int fp_fmt(const char *s)
{
    return !strcmp(s, "s") ? 0 : !strcmp(s, "d") ? 1 : -1;
}

/* an integer side of a conversion: RV_CVT_*, or -1 */
static int fp_ity(const char *s)
{
    return !strcmp(s, "w") ? RV_CVT_W : !strcmp(s, "wu") ? RV_CVT_WU
         : !strcmp(s, "l") ? RV_CVT_L : !strcmp(s, "lu") ? RV_CVT_LU : -1;
}

#define FFAIL(...) do { snprintf(err, (size_t)errlen, __VA_ARGS__); \
                        return -1; } while (0)

/* Is a format available? 0 = yes; else the message has been written. */
static int fp_have(int dbl, const struct tok *m, char *err, int errlen)
{
    int flen = target_riscv_flen();
    if (flen < (dbl ? 64 : 32))
        FFAIL("%.*s needs the %s extension, which -march= does not name",
              m->len, m->s, dbl ? "D" : "F");
    return 0;
}

/* The F/D statement t[0..n), if it is one: 1 encoded, 0 not an FP
 * mnemonic at all, -1 an error (in err). */
static int fp_stmt(const struct tok *t, int n, struct code *out,
                   char *err, int errlen)
{
    struct fpname p;
    int xlen = target_xlen();
    const struct tok *m = &t[0];
    if (m->len < 2 || m->s[0] != 'f' || tok_is(m, "fence") ||
        tok_is(m, "fence.i") || !fp_split(m, &p))
        return 0;

    /* the fcsr pseudos: frcsr rd, fscsr [rd,] rs, and the same for frm
     * and fflags; fsrmi/fsflagsi [rd,] imm */
    {
        static const struct { const char *rd_name, *wr_name, *wi_name;
                              unsigned csr; } ps[3] = {
            { "frcsr", "fscsr", NULL, 0x003 },
            { "frrm", "fsrm", "fsrmi", 0x002 },
            { "frflags", "fsflags", "fsflagsi", 0x001 },
        };
        for (int k = 0; k < 3; k++) {
            if (tok_is(m, ps[k].rd_name)) {
                int rd = n == 2 ? tok_reg(&t[1]) : -1;
                if (rd < 0)
                    FFAIL("%s takes one register", ps[k].rd_name);
                if (fp_have(0, m, err, errlen)) return -1;
                emit_csr(out, 2, rd, 0, ps[k].csr);         /* csrrs rd, csr, x0 */
                return 1;
            }
            if (tok_is(m, ps[k].wr_name)) {
                int rd = 0, rs;
                if (n == 3) {
                    if ((rd = tok_reg(&t[1])) < 0)
                        FFAIL("%s wants registers", ps[k].wr_name);
                } else if (n != 2) {
                    FFAIL("%s takes [rd,] rs", ps[k].wr_name);
                }
                if ((rs = tok_reg(&t[n - 1])) < 0)
                    FFAIL("%s wants registers", ps[k].wr_name);
                if (fp_have(0, m, err, errlen)) return -1;
                emit_csr(out, 1, rd, rs, ps[k].csr);        /* csrrw rd, csr, rs */
                return 1;
            }
            if (ps[k].wi_name && tok_is(m, ps[k].wi_name)) {
                int rd = 0;
                long long v;
                if (n == 3) {
                    if ((rd = tok_reg(&t[1])) < 0)
                        FFAIL("%s wants a register", ps[k].wi_name);
                } else if (n != 2) {
                    FFAIL("%s takes [rd,] imm", ps[k].wi_name);
                }
                if (!tok_imm(&t[n - 1], &v) || v < 0 || v > 31)
                    FFAIL("%s's immediate is 0..31", ps[k].wi_name);
                if (fp_have(0, m, err, errlen)) return -1;
                emit_csr(out, 5, rd, (int)v, ps[k].csr);    /* csrrwi */
                return 1;
            }
        }
    }

    /* loads and stores: flw/fld fd, off(rs); fsw/fsd fs, off(rs) */
    if (p.nparts == 1 && (!strcmp(p.base, "flw") || !strcmp(p.base, "fld") ||
                          !strcmp(p.base, "fsw") || !strcmp(p.base, "fsd"))) {
        int dbl = p.base[2] == 'd', store = p.base[1] == 's', fr, base;
        long long off;
        if (n != 3) FFAIL("%s takes a register and an address", p.base);
        if ((fr = tok_freg(&t[1])) < 0)
            FFAIL("\"%.*s\" is not a float register", t[1].len, t[1].s);
        if (!tok_mem(&t[2], &base, &off))
            FFAIL("\"%.*s\" is not an `off(reg)` address", t[2].len, t[2].s);
        if (!rv_fits(off, 12))
            FFAIL("%s offset %lld does not fit a 12-bit field", p.base, off);
        if (fp_have(dbl, m, err, errlen)) return -1;
        if (store) rv_fstore(out, fr, base, (int)off, dbl);
        else       rv_fload(out, fr, base, (int)off, dbl);
        return 1;
    }

    /* moves between the register files: fmv.x.w/fmv.x.s, fmv.w.x/fmv.s.x,
     * fmv.x.d, fmv.d.x */
    if (!strcmp(p.base, "fmv") && p.nparts == 3) {
        int to_x = !strcmp(p.a, "x"), dbl, fd, rs;
        const char *f = to_x ? p.b : p.a, *x = to_x ? p.a : p.b;
        if (strcmp(x, "x") || (strcmp(f, "w") && strcmp(f, "s") && strcmp(f, "d")))
            FFAIL("asm instruction \"%.*s\" is not in the RISC-V vocabulary",
                  m->len, m->s);
        dbl = !strcmp(f, "d");
        if (n != 3) FFAIL("%.*s takes two registers", m->len, m->s);
        fd = to_x ? tok_reg(&t[1]) : tok_freg(&t[1]);
        rs = to_x ? tok_freg(&t[2]) : tok_reg(&t[2]);
        if (fd < 0 || rs < 0)
            FFAIL("%.*s wants %s", m->len, m->s,
                  to_x ? "an integer register then a float one"
                       : "a float register then an integer one");
        if (dbl && xlen != 64)
            FFAIL("%.*s is an RV64 instruction and this is RV32", m->len, m->s);
        if (fp_have(dbl, m, err, errlen)) return -1;
        rv_fp_r(out, to_x ? 0x1c : 0x1e, dbl, 0, fd, rs, 0);
        return 1;
    }

    /* conversions: fcvt.<int>.<fmt>, fcvt.<fmt>.<int>, fcvt.s.d, fcvt.d.s,
     * each with an optional rounding mode. Left out, it is dyn -- except
     * where the result is always exact (fcvt.d.s, fcvt.d.w[u]), which
     * llvm-mc writes with rne. */
    if (!strcmp(p.base, "fcvt") && p.nparts == 3) {
        int ia = fp_ity(p.a), ib = fp_ity(p.b), fa = fp_fmt(p.a), fb = fp_fmt(p.b);
        int rd, rs, rm, dbl, f5, rs2, ity, exact;
        if (n != 3 && n != 4) FFAIL("%.*s takes two registers [, rm]", m->len, m->s);
        if (ia >= 0 && fb >= 0) {           /* to an integer */
            rd = tok_reg(&t[1]); rs = tok_freg(&t[2]);
            dbl = fb; f5 = 0x18; rs2 = ia; ity = ia; exact = 0;
        } else if (fa >= 0 && ib >= 0) {    /* from an integer */
            rd = tok_freg(&t[1]); rs = tok_reg(&t[2]);
            dbl = fa; f5 = 0x1a; rs2 = ib; ity = ib;
            exact = fa == 1 && ib <= RV_CVT_WU;
        } else if (fa >= 0 && fb >= 0 && fa != fb) {
            rd = tok_freg(&t[1]); rs = tok_freg(&t[2]);
            dbl = fa; f5 = 0x08; rs2 = fb; ity = -1; exact = fa == 1;
        } else {
            FFAIL("asm instruction \"%.*s\" is not in the RISC-V vocabulary",
                  m->len, m->s);
        }
        if (rd < 0 || rs < 0)
            FFAIL("%.*s: wrong kind of register", m->len, m->s);
        if (ity >= RV_CVT_L && xlen != 64)
            FFAIL("%.*s is an RV64 instruction and this is RV32", m->len, m->s);
        rm = exact ? 0 : 7;
        if (n == 4 && (rm = tok_rm(&t[3])) < 0)
            FFAIL("\"%.*s\" is not a rounding mode", t[3].len, t[3].s);
        if (fp_have(dbl || (f5 == 0x08), m, err, errlen)) return -1;
        rv_fp_r(out, f5, dbl, rm, rd, rs, rs2);
        return 1;
    }

    {
        int fmt = p.nparts == 2 ? fp_fmt(p.a) : -1;
        int r[4], nr = 0;
        if (fmt < 0)
            FFAIL("asm instruction \"%.*s\" is not in the RISC-V vocabulary",
                  m->len, m->s);

        /* compares and fclass: an integer destination */
        {
            static const struct { const char *name; int f5, rm, nsrc; } cmp[] = {
                { "feq", 0x14, 2, 2 }, { "flt", 0x14, 1, 2 },
                { "fle", 0x14, 0, 2 }, { "fclass", 0x1c, 1, 1 },
                { NULL, 0, 0, 0 }
            };
            for (int k = 0; cmp[k].name; k++) {
                int rd, a, b = 0;
                if (strcmp(p.base, cmp[k].name))
                    continue;
                if (n != 2 + cmp[k].nsrc)
                    FFAIL("%.*s takes %d registers", m->len, m->s, 1 + cmp[k].nsrc);
                rd = tok_reg(&t[1]);
                a = tok_freg(&t[2]);
                if (cmp[k].nsrc == 2)
                    b = tok_freg(&t[3]);
                if (rd < 0 || a < 0 || b < 0)
                    FFAIL("%.*s wants an integer register, then float ones",
                          m->len, m->s);
                if (fp_have(fmt, m, err, errlen)) return -1;
                rv_fp_r(out, cmp[k].f5, fmt, cmp[k].rm, rd, a, b);
                return 1;
            }
        }

        /* everything else reads and writes float registers only */
        for (int k = 1; k < n; k++) {
            if (k == n - 1 && n > 2 && tok_rm(&t[k]) >= 0)
                break;
            if ((r[nr] = tok_freg(&t[k])) < 0)
                FFAIL("\"%.*s\" is not a float register", t[k].len, t[k].s);
            if (++nr == 4)
                break;
        }
        {
            int rm = 7, have_rm = 1 + nr < n;
            if (have_rm && (rm = tok_rm(&t[n - 1])) < 0)
                FFAIL("\"%.*s\" is not a rounding mode", t[n - 1].len, t[n - 1].s);

            /* two-operand pseudos and fsqrt */
            if (!strcmp(p.base, "fmv") || !strcmp(p.base, "fneg") ||
                !strcmp(p.base, "fabs") || !strcmp(p.base, "fsqrt")) {
                if (nr != 2 || (have_rm && strcmp(p.base, "fsqrt")))
                    FFAIL("%.*s takes two float registers", m->len, m->s);
                if (fp_have(fmt, m, err, errlen)) return -1;
                if (!strcmp(p.base, "fsqrt"))
                    rv_fp_r(out, 0x0b, fmt, rm, r[0], r[1], 0);
                else
                    rv_fp_r(out, 0x04, fmt, !strcmp(p.base, "fmv") ? 0
                            : !strcmp(p.base, "fneg") ? 1 : 2, r[0], r[1], r[1]);
                return 1;
            }
            /* three operands: arithmetic (with a rounding mode), sign
             * injection and min/max (whose rm field is the operation) */
            {
                static const struct { const char *name; int f5, rm; } ar[] = {
                    { "fadd", 0x00, -1 }, { "fsub", 0x01, -1 },
                    { "fmul", 0x02, -1 }, { "fdiv", 0x03, -1 },
                    { "fsgnj", 0x04, 0 }, { "fsgnjn", 0x04, 1 },
                    { "fsgnjx", 0x04, 2 }, { "fmin", 0x05, 0 },
                    { "fmax", 0x05, 1 }, { NULL, 0, 0 }
                };
                for (int k = 0; ar[k].name; k++) {
                    if (strcmp(p.base, ar[k].name))
                        continue;
                    if (nr != 3 || (have_rm && ar[k].rm >= 0))
                        FFAIL("%.*s takes three float registers%s", m->len, m->s,
                              ar[k].rm < 0 ? " [, rm]" : "");
                    if (fp_have(fmt, m, err, errlen)) return -1;
                    rv_fp_r(out, ar[k].f5, fmt, ar[k].rm >= 0 ? ar[k].rm : rm,
                            r[0], r[1], r[2]);
                    return 1;
                }
            }
            /* the fused multiply-adds: four registers */
            {
                static const struct { const char *name; int op; } fm[] = {
                    { "fmadd", 0x43 }, { "fmsub", 0x47 },
                    { "fnmsub", 0x4b }, { "fnmadd", 0x4f }, { NULL, 0 }
                };
                for (int k = 0; fm[k].name; k++) {
                    if (strcmp(p.base, fm[k].name))
                        continue;
                    if (nr != 4)
                        FFAIL("%.*s takes four float registers [, rm]", m->len, m->s);
                    if (fp_have(fmt, m, err, errlen)) return -1;
                    rv_fp_r4(out, fm[k].op, fmt, rm, r[0], r[1], r[2], r[3]);
                    return 1;
                }
            }
        }
    }
    FFAIL("asm instruction \"%.*s\" is not in the RISC-V vocabulary",
          m->len, m->s);
}

#define FAIL(...)  do { snprintf(err, (size_t)errlen, __VA_ARGS__); \
                        return -1; } while (0)

/* ---- data directives ------------------------------------------------------
 *
 * `.word 0x0000100f` in a template: an instruction this vocabulary does
 * not have yet, written as its bytes, or a constant a trap handler finds
 * beside its code. The sizes are GNU as's for RISC-V -- `.half` two,
 * `.word` four, `.dword` eight -- and the generic spellings with them.
 * Constants only, each GNU as's range for its width (a signed or an
 * unsigned value that fits); a symbol would need a relocation, which an
 * inline asm here has no way to carry. A .s file's are src/as/gas.c's
 * and never reach this. 0 when stmt is not one, 1 when it was emitted. */
static const struct { const char *name; int size; } data_dir[] = {
    { ".byte", 1 },
    { ".half", 2 }, { ".short", 2 }, { ".hword", 2 }, { ".2byte", 2 },
    { ".word", 4 }, { ".long", 4 }, { ".int", 4 }, { ".4byte", 4 },
    { ".dword", 8 }, { ".quad", 8 }, { ".8byte", 8 },
    { NULL, 0 }
};

static int data_stmt(const char *stmt, int len, struct code *out,
                     char *err, int errlen)
{
    int i = 0, d, m, size = 0;
    while (i < len && isspace((unsigned char)stmt[i]))
        i++;
    d = i;
    for (m = i; m < len && !isspace((unsigned char)stmt[m]); m++) {}
    for (int k = 0; data_dir[k].name; k++)
        if ((int)strlen(data_dir[k].name) == m - i &&
            strncmp(stmt + i, data_dir[k].name, (size_t)(m - i)) == 0)
            size = data_dir[k].size;
    if (!size)
        return 0;
    int at = out->len;
    /* the values, one per comma, each a constant expression */
    for (i = m; i < len; ) {
        int s, e, depth = 0;
        long long v;
        while (i < len && isspace((unsigned char)stmt[i]))
            i++;
        if (i >= len)
            break;
        for (s = i; i < len && (depth > 0 || stmt[i] != ','); i++) {
            if (stmt[i] == '(') depth++;
            else if (stmt[i] == ')') depth--;
        }
        for (e = i; e > s && isspace((unsigned char)stmt[e - 1]); e--) {}
        if (e == s || !asm_const_expr(stmt + s, e - s, &v))
            FAIL("\"%.*s\" in %.*s is not a constant: inline asm data takes "
                 "numbers, and a symbol would need a relocation",
                 e - s, stmt + s, m - d, stmt + d);
        if (size < 8 && (v < -(1LL << (8 * size - 1)) ||
                         v > (long long)((1ULL << (8 * size)) - 1)))
            FAIL("%lld does not fit in %d byte%s", v, size,
                 size == 1 ? "" : "s");
        for (int b = 0; b < size; b++)
            code_byte(out, (int)(((unsigned long long)v >> (8 * b)) & 0xff));
        if (i < len)
            i++;                        /* the comma */
    }
    /* data: what a disassembler and -S show as data */
    code_mark_data(out, at, out->len);
    return 1;
}

static int one_stmt(const char *stmt, int len, struct code *out,
                    char *err, int errlen)
{
    struct tok t[MAXTOK];
    int n = split(stmt, len, t, MAXTOK);
    int xlen = target_xlen();

    if (n == 0)
        return 0;                       /* blank or comment-only */
    {
        int r = data_stmt(stmt, len, out, err, errlen);
        if (r)
            return r < 0 ? -1 : 0;
    }

    {
        int r = fp_stmt(t, n, out, err, errlen);
        if (r)
            return r < 0 ? -1 : 0;
    }

    /* ---- no operands ---- */
    if (n == 1) {
        if (tok_is(&t[0], "nop")) {
            rv_alu_imm(out, RV_ADD, RV_ZERO, RV_ZERO, 0, 0);
            return 0;
        }
        if (tok_is(&t[0], "ret")) { rv_ret(out); return 0; }
        if (tok_is(&t[0], "ebreak")) { rv_ebreak(out); return 0; }
        if (tok_is(&t[0], "unimp")) { rv_unimp(out); return 0; }
        if (tok_is(&t[0], "ecall")) { emit_csr(out, 0, 0, 0, 0); return 0; }
        if (tok_is(&t[0], "mret")) { emit_csr(out, 0, 0, 0, 0x302); return 0; }
        if (tok_is(&t[0], "sret")) { emit_csr(out, 0, 0, 0, 0x102); return 0; }
        if (tok_is(&t[0], "wfi"))  { emit_csr(out, 0, 0, 0, 0x105); return 0; }
        /* A bare `fence` is `fence iorw, iorw`: pred and succ both 0xf,
         * which is the STRONGEST one. An earlier version emitted
         * `fence rw, rw` and the referee caught it -- that orders only
         * memory and not device I/O, so a barrier written for an MMIO
         * register would not have ordered it. */
        if (tok_is(&t[0], "fence")) {
            rv_fence(out, 0xf, 0xf);
            return 0;
        }
        if (tok_is(&t[0], "fence.i")) {
            code_u32(out, (unsigned long)OP_FENCE | (1UL << 12));
            return 0;
        }
        FAIL("asm instruction \"%.*s\" is not in the RISC-V vocabulary",
             t[0].len, t[0].s);
    }

    /* ---- fence with arguments: `fence pred, succ` ----
     *
     * Each side is a subset of `iorw`: device Input, device Output,
     * memory Read, memory Write, in bits 3..0. Parsed rather than
     * collapsed to the strongest -- collapsing is CORRECT (a fence may
     * always order more than asked) but it would not match what an
     * assembler emits, and the referee compares bytes. */
    if (tok_is(&t[0], "fence")) {
        unsigned set[2] = { 0, 0 };
        if (n != 3) FAIL("fence takes two sets of i/o/r/w, or no operands");
        for (int k = 0; k < 2; k++)
            for (int j = 0; j < t[k + 1].len; j++)
                switch (t[k + 1].s[j]) {
                case 'i': set[k] |= 8; break;
                case 'o': set[k] |= 4; break;
                case 'r': set[k] |= 2; break;
                case 'w': set[k] |= 1; break;
                default:
                    FAIL("\"%.*s\" is not a set of i/o/r/w",
                         t[k + 1].len, t[k + 1].s);
                }
        rv_fence(out, set[0], set[1]);
        return 0;
    }

    /* ---- CSR ---- */
    for (const struct csr_ent *e = csr_tab; e->name; e++) {
        int rd, rs, csr;
        long long imm;
        if (!tok_is(&t[0], e->name))
            continue;
        switch (e->form) {
        case 0:                                   /* csrr rd, csr */
            if (n != 3) FAIL("%s takes a register and a CSR", e->name);
            rd = tok_reg(&t[1]);
            csr = csr_num(t[2].s, t[2].len);
            if (rd < 0) FAIL("\"%.*s\" is not a register", t[1].len, t[1].s);
            if (csr == -2) FAIL("CSR \"%.*s\" exists only on RV32",
                                t[2].len, t[2].s);
            if (csr < 0) FAIL("\"%.*s\" is not a CSR this assembler knows",
                              t[2].len, t[2].s);
            emit_csr(out, e->f3, rd, 0, (unsigned)csr);
            return 0;
        case 1:                                   /* csrw csr, rs */
            if (n != 3) FAIL("%s takes a CSR and a register", e->name);
            csr = csr_num(t[1].s, t[1].len);
            rs = tok_reg(&t[2]);
            if (csr == -2) FAIL("CSR \"%.*s\" exists only on RV32",
                                t[1].len, t[1].s);
            if (csr < 0) FAIL("\"%.*s\" is not a CSR this assembler knows",
                              t[1].len, t[1].s);
            /* An immediate makes it the immediate form, as GNU as reads
             * it: FreeRTOS masks interrupts with `csrc mstatus, 8`. */
            if (rs < 0 && tok_imm(&t[2], &imm) && imm >= 0 && imm < 32) {
                emit_csr(out, e->f3 + 4, 0, (int)imm, (unsigned)csr);
                return 0;
            }
            if (rs < 0) FAIL("\"%.*s\" is not a register", t[2].len, t[2].s);
            /* rd = x0: the old value is discarded, which is what the
             * pseudo-instruction means. */
            emit_csr(out, e->f3, 0, rs, (unsigned)csr);
            return 0;
        case 2:                                   /* csrrw rd, csr, rs */
            if (n != 4) FAIL("%s takes a register, a CSR and a register",
                             e->name);
            rd = tok_reg(&t[1]);
            csr = csr_num(t[2].s, t[2].len);
            rs = tok_reg(&t[3]);
            if (csr < 0) FAIL("\"%.*s\" is not a CSR this assembler knows",
                              t[2].len, t[2].s);
            if (rd >= 0 && rs < 0 && tok_imm(&t[3], &imm) && imm >= 0 &&
                imm < 32) {                       /* as csrw above */
                emit_csr(out, e->f3 + 4, rd, (int)imm, (unsigned)csr);
                return 0;
            }
            if (rd < 0 || rs < 0) FAIL("%s wants two registers", e->name);
            emit_csr(out, e->f3, rd, rs, (unsigned)csr);
            return 0;
        case 3:                                   /* csrwi csr, imm */
            if (n != 3) FAIL("%s takes a CSR and a 5-bit immediate", e->name);
            csr = csr_num(t[1].s, t[1].len);
            if (csr < 0 || !tok_imm(&t[2], &imm) || imm < 0 || imm > 31)
                FAIL("%s wants a known CSR and a 0..31 immediate", e->name);
            emit_csr(out, e->f3, 0, (int)imm, (unsigned)csr);
            return 0;
        default:                                  /* csrrwi rd, csr, imm */
            if (n != 4) FAIL("%s takes a register, a CSR and an immediate",
                             e->name);
            rd = tok_reg(&t[1]);
            csr = csr_num(t[2].s, t[2].len);
            if (rd < 0 || csr < 0 || !tok_imm(&t[3], &imm) ||
                imm < 0 || imm > 31)
                FAIL("%s wants a register, a known CSR and a 0..31 immediate",
                     e->name);
            emit_csr(out, e->f3, rd, (int)imm, (unsigned)csr);
            return 0;
        }
    }

    /* ---- the pseudo-instructions a program actually writes ---- */
    if (tok_is(&t[0], "mv") && n == 3) {
        int rd = tok_reg(&t[1]), rs = tok_reg(&t[2]);
        if (rd < 0 || rs < 0) FAIL("mv wants two registers");
        rv_mv(out, rd, rs);
        return 0;
    }
    if (tok_is(&t[0], "li") && n == 3) {
        long long v;
        int rd = tok_reg(&t[1]);
        if (rd < 0 || !tok_imm(&t[2], &v)) FAIL("li wants a register and a "
                                               "constant");
        rv_li(out, rd, v, xlen);
        return 0;
    }
    if (tok_is(&t[0], "not") && n == 3) {
        int rd = tok_reg(&t[1]), rs = tok_reg(&t[2]);
        if (rd < 0 || rs < 0) FAIL("not wants two registers");
        rv_alu_imm(out, RV_XOR, rd, rs, -1, 0);
        return 0;
    }
    if (tok_is(&t[0], "neg") && n == 3) {
        int rd = tok_reg(&t[1]), rs = tok_reg(&t[2]);
        if (rd < 0 || rs < 0) FAIL("neg wants two registers");
        rv_alu(out, RV_SUB, rd, RV_ZERO, rs, 0);
        return 0;
    }
    if ((tok_is(&t[0], "jr") || tok_is(&t[0], "jalr")) && n == 2) {
        int rs = tok_reg(&t[1]);
        if (rs < 0) FAIL("%.*s wants a register", t[0].len, t[0].s);
        rv_jalr(out, tok_is(&t[0], "jr") ? RV_ZERO : RV_RA, rs, 0);
        return 0;
    }

    /* ---- loads and stores ---- */
    for (const struct ls_ent *e = ls_tab; e->name; e++) {
        int r, base;
        long long off;
        if (!tok_is(&t[0], e->name))
            continue;
        if (n != 3) FAIL("%s takes a register and an address", e->name);
        r = tok_reg(&t[1]);
        if (r < 0) FAIL("\"%.*s\" is not a register", t[1].len, t[1].s);
        if (!tok_mem(&t[2], &base, &off))
            FAIL("\"%.*s\" is not an `off(reg)` address", t[2].len, t[2].s);
        if (!rv_fits(off, 12))
            FAIL("%s offset %lld does not fit a 12-bit field", e->name, off);
        /* lwu is RV64's too: at RV32 it was quietly assembled as lw */
        if ((e->size == 8 || (e->size == 4 && !e->sign && !e->store)) &&
            xlen != 64)
            FAIL("%s is an RV64 instruction and this is RV32", e->name);
        if (e->store) rv_store(out, r, base, (int)off, e->size, xlen);
        else          rv_load(out, r, base, (int)off, e->size, e->sign, xlen);
        return 0;
    }

    /* ---- shifts, before the ALU table: `slli` is not `sll` plus an i ---- */
    for (const struct sh_ent *e = sh_tab; e->name; e++) {
        long long v;
        int rd, rs;
        if (!tok_is(&t[0], e->name))
            continue;
        if (n != 4) FAIL("%s takes three operands", e->name);
        rd = tok_reg(&t[1]);
        rs = tok_reg(&t[2]);
        if (rd < 0 || rs < 0) FAIL("%s wants registers", e->name);
        if (e->w && xlen != 64)
            FAIL("%s is an RV64 instruction and this is RV32", e->name);
        if (e->imm) {
            int bits = (xlen == 64 && !e->w) ? 6 : 5;
            if (!tok_imm(&t[3], &v) || v < 0 || v >= (1 << bits))
                FAIL("%s shift amount must be 0..%d here", e->name,
                     (1 << bits) - 1);
            rv_shift_imm(out, e->op, rd, rs, (int)v, e->w, xlen);
        } else {
            int rb = tok_reg(&t[3]);
            if (rb < 0) FAIL("%s wants a register shift amount", e->name);
            rv_alu(out, e->op, rd, rs, rb, e->w);
        }
        return 0;
    }

    /* ---- the M extension ---- */
    for (const struct md_ent *e = md_tab; e->name; e++) {
        int rd, ra, rb;
        if (!tok_is(&t[0], e->name))
            continue;
        if (n != 4) FAIL("%s takes three registers", e->name);
        rd = tok_reg(&t[1]); ra = tok_reg(&t[2]); rb = tok_reg(&t[3]);
        if (rd < 0 || ra < 0 || rb < 0) FAIL("%s wants registers", e->name);
        if (e->w && xlen != 64)
            FAIL("%s is an RV64 instruction and this is RV32", e->name);
        rv_muldiv(out, e->op, rd, ra, rb, e->w);
        return 0;
    }

    /* ---- the ALU ---- */
    for (const struct alu_ent *e = alu_tab; e->name; e++) {
        int rd, ra;
        if (!tok_is(&t[0], e->name))
            continue;
        if (n != 4) FAIL("%s takes three operands", e->name);
        rd = tok_reg(&t[1]);
        ra = tok_reg(&t[2]);
        if (rd < 0 || ra < 0) FAIL("%s wants registers", e->name);
        if (e->w && xlen != 64)
            FAIL("%s is an RV64 instruction and this is RV32", e->name);
        if (e->imm) {
            long long v;
            if (!tok_imm(&t[3], &v)) FAIL("%s wants an immediate", e->name);
            if (!rv_fits(v, 12))
                FAIL("%s immediate %lld does not fit a 12-bit field",
                     e->name, v);
            rv_alu_imm(out, e->op, rd, ra, (int)v, e->w);
        } else {
            int rb = tok_reg(&t[3]);
            if (rb < 0) FAIL("%s wants a third register", e->name);
            rv_alu(out, e->op, rd, ra, rb, e->w);
        }
        return 0;
    }

    /* ---- control flow ------------------------------------------------
     *
     * These are here for the FILE assembler (src/as/gas.c), which turns
     * a label into the PC-relative displacement before calling: this
     * layer never sees a name. Inline asm can use them too, with a
     * numeric offset, which is the only form it could have used anyway
     * -- a template cannot see the surrounding function's labels.
     */
    {
        /* `bgt`/`ble` and their unsigned forms are blt/bge with the
         * operands swapped, which is how the ISA spells them at all */
        static const struct { const char *name; int f3; int swap; } br[] = {
            { "beq", 0, 0 }, { "bne", 1, 0 }, { "blt", 4, 0 }, { "bge", 5, 0 },
            { "bltu", 6, 0 }, { "bgeu", 7, 0 },
            { "bgt", 4, 1 }, { "ble", 5, 1 }, { "bgtu", 6, 1 }, { "bleu", 7, 1 }
        };
        /* The zero-comparison pseudos, each one of the above against x0. */
        static const struct { const char *name; int f3; int zfirst; } brz[] = {
            { "beqz", 0, 0 }, { "bnez", 1, 0 }, { "bltz", 4, 0 },
            { "bgez", 5, 0 }, { "blez", 5, 1 }, { "bgtz", 4, 1 }
        };
        for (unsigned k = 0; k < sizeof br / sizeof br[0]; k++)
            if (tok_is(&t[0], br[k].name)) {
                long long v;
                int r1, r2;
                if (n != 4) FAIL("%s wants two registers and an offset",
                                 br[k].name);
                r1 = tok_reg(&t[1]); r2 = tok_reg(&t[2]);
                if (r1 < 0 || r2 < 0) FAIL("%s wants two registers",
                                           br[k].name);
                if (!tok_imm(&t[3], &v)) FAIL("%s wants an offset", br[k].name);
                if ((v & 1) || !rv_fits(v, 13))
                    FAIL("%s offset %lld is odd or out of range", br[k].name, v);
                rv_w(out, rv_enc_b(OP_BRANCH, br[k].f3, br[k].swap ? r2 : r1,
                                   br[k].swap ? r1 : r2, (int)v));
                return 0;
            }
        for (unsigned k = 0; k < sizeof brz / sizeof brz[0]; k++)
            if (tok_is(&t[0], brz[k].name)) {
                long long v;
                int r1;
                if (n != 3) FAIL("%s wants a register and an offset",
                                 brz[k].name);
                r1 = tok_reg(&t[1]);
                if (r1 < 0) FAIL("%s wants a register", brz[k].name);
                if (!tok_imm(&t[2], &v)) FAIL("%s wants an offset", brz[k].name);
                if ((v & 1) || !rv_fits(v, 13))
                    FAIL("%s offset %lld is odd or out of range",
                         brz[k].name, v);
                rv_w(out, rv_enc_b(OP_BRANCH, brz[k].f3,
                                   brz[k].zfirst ? RV_ZERO : r1,
                                   brz[k].zfirst ? r1 : RV_ZERO, (int)v));
                return 0;
            }
    }
    if (tok_is(&t[0], "j") && n == 2) {           /* jal zero, off */
        long long v;
        if (!tok_imm(&t[1], &v)) FAIL("j wants an offset");
        if ((v & 1) || !rv_fits(v, 21)) FAIL("j offset %lld is out of range", v);
        rv_w(out, rv_enc_j(OP_JAL, RV_ZERO, (int)v));
        return 0;
    }
    if (tok_is(&t[0], "jal")) {
        long long v;
        int rd = RV_RA, ai = 1;
        if (n == 3) { rd = tok_reg(&t[1]); ai = 2;
                      if (rd < 0) FAIL("jal wants a register"); }
        else if (n != 2) FAIL("jal wants an offset");
        if (!tok_imm(&t[ai], &v)) FAIL("jal wants an offset");
        if ((v & 1) || !rv_fits(v, 21)) FAIL("jal offset %lld is out of range", v);
        rv_w(out, rv_enc_j(OP_JAL, rd, (int)v));
        return 0;
    }
    if (tok_is(&t[0], "jr") && n == 2) {
        int r = tok_reg(&t[1]);
        if (r < 0) FAIL("jr wants a register");
        rv_jalr(out, RV_ZERO, r, 0);
        return 0;
    }
    if (tok_is(&t[0], "jalr")) {
        int rd = RV_RA, rs, off = 0;
        if (n == 2) {                              /* jalr rs */
            rs = tok_reg(&t[1]);
            if (rs < 0) FAIL("jalr wants a register");
        } else if (n == 3) {
            rd = tok_reg(&t[1]);
            if (rd < 0) FAIL("jalr wants a register");
            /* `jalr rd, rs` or `jalr rd, off(rs)` -- the second is ONE
             * token, as every load and store operand is here. */
            rs = tok_reg(&t[2]);
            if (rs < 0) {
                long long mo;
                if (!tok_mem(&t[2], &rs, &mo))
                    FAIL("jalr wants a register or off(reg)");
                if (!rv_fits(mo, 12))
                    FAIL("jalr offset %lld does not fit 12 bits", mo);
                off = (int)mo;
            }
        } else FAIL("jalr takes one or two operands");
        rv_jalr(out, rd, rs, off);
        return 0;
    }
    if ((tok_is(&t[0], "lui") || tok_is(&t[0], "auipc")) && n == 3) {
        long long v;
        int rd = tok_reg(&t[1]);
        if (rd < 0) FAIL("%.*s wants a register", t[0].len, t[0].s);
        if (!tok_imm(&t[2], &v)) FAIL("%.*s wants an immediate",
                                      t[0].len, t[0].s);
        if (v < -524288 || v > 1048575)
            FAIL("%.*s immediate %lld does not fit 20 bits",
                 t[0].len, t[0].s, v);
        if (tok_is(&t[0], "lui")) rv_lui(out, rd, (long)(v & 0xfffff));
        else                      rv_auipc(out, rd, (long)(v & 0xfffff));
        return 0;
    }
    /* ---- the register pseudos ---------------------------------------- */
    if (n == 3) {
        int rd = tok_reg(&t[1]);
        if (rd >= 0) {
            long long v;
            if (tok_is(&t[0], "li")) {
                if (!tok_imm(&t[2], &v)) FAIL("li wants a constant");
                rv_li(out, rd, v, xlen);
                return 0;
            }
            {
                int rs = tok_reg(&t[2]);
                if (rs >= 0) {
                    if (tok_is(&t[0], "mv"))
                        { rv_mv(out, rd, rs); return 0; }
                    if (tok_is(&t[0], "not"))
                        { rv_alu_imm(out, RV_XOR, rd, rs, -1, 0); return 0; }
                    if (tok_is(&t[0], "neg"))
                        { rv_alu(out, RV_SUB, rd, RV_ZERO, rs, 0); return 0; }
                    /* negw and sext.w are RV64's: their encodings are
                     * illegal instructions on an RV32 part, and they were
                     * emitted there all the same */
                    if ((tok_is(&t[0], "negw") || tok_is(&t[0], "sext.w")) &&
                        target_xlen() != 64)
                        FAIL("%.*s is an RV64 instruction and this is RV32",
                             t[0].len, t[0].s);
                    if (tok_is(&t[0], "negw"))
                        { rv_alu(out, RV_SUB, rd, RV_ZERO, rs, 1); return 0; }
                    if (tok_is(&t[0], "seqz"))
                        { rv_alu_imm(out, RV_SLTU, rd, rs, 1, 0); return 0; }
                    if (tok_is(&t[0], "snez"))
                        { rv_alu(out, RV_SLTU, rd, RV_ZERO, rs, 0); return 0; }
                    if (tok_is(&t[0], "sext.w"))
                        { rv_alu_imm(out, RV_ADD, rd, rs, 0, 1); return 0; }
                }
            }
        }
    }

    FAIL("asm instruction \"%.*s\" is not in the RISC-V vocabulary",
         t[0].len, t[0].s);
}

/* src/as/gas.c takes every identifier in an operand for a symbol unless
 * the target says otherwise. A CSR's name is not one -- `csrr a0, mcause`
 * named an undefined symbol `mcause` -- and neither is a fence's `rw`.
 * Asked per statement, so a label called `mie` is still a label in a
 * `j mie`. */
int rvasm_is_word(const char *stmt, const char *w, int wlen)
{
    const char *m = stmt;
    int mlen;
    while (*m == ' ' || *m == '\t') m++;
    for (mlen = 0; m[mlen] && m[mlen] != ' ' && m[mlen] != '\t'; mlen++) {}
    if (mlen >= 4 && strncmp(m, "csr", 3) == 0 && csr_num(w, wlen) >= 0)
        return 1;
    /* an F/D statement's float registers and rounding mode */
    if (mlen >= 2 && m[0] == 'f') {
        static const char *const rm[6] = { "rne", "rtz", "rdn", "rup", "rmm", "dyn" };
        if (rvasm_fpr(w, wlen) >= 0)
            return 1;
        for (int k = 0; k < 6; k++)
            if (wlen == 3 && strncmp(w, rm[k], 3) == 0)
                return 1;
    }
    if (mlen == 5 && strncmp(m, "fence", 5) == 0) {
        int ok = wlen > 0 && wlen <= 4;
        for (int k = 0; k < wlen; k++)
            if (!strchr("iorw", w[k])) ok = 0;
        return ok;
    }
    return 0;
}

int rvasm_assemble(const char *text, struct code *out, char *err, int errlen)
{
    const char *p = text;
    err[0] = 0;
    while (*p) {
        const char *start = p;
        int len, r;
        /* Strip a comment: '#' anywhere, or "//" -- but not inside a
         * .ascii string, nor a `;` there */
        p += asm_stmt_len(p, ";");
        len = asm_cut_comment(start, (int)(p - start), "#", 1);
        /* .ascii/.asciz/.string, and the alignments, whose padding the
         * backend decides where the bytes land */
        {
            static const struct asm_dirs dirs = { NULL, 0, 0 };
            r = code_asm_directive(start, len, out, &dirs, err, errlen);
        }
        if (r < 0 || (!r && one_stmt(start, len, out, err, errlen) != 0))
            return -1;
        if (*p)
            p++;
    }
    return 0;
}

/* ---- the referee's input ------------------------------------------------ */

void rvasm_vocabulary(FILE *f)
{
    /* One line per entry, generated from the tables, so llvm-mc sees
     * exactly what this file claims to encode. Registers are chosen to
     * exercise a high and a low number rather than always a0. */
    fprintf(f, "\tnop\n\tret\n\tebreak\n\tunimp\n\tecall\n");
    fprintf(f, "\tmret\n\tsret\n\twfi\n\tfence\n\tfence.i\n");
    fprintf(f, "\tfence rw, rw\n\tfence iorw, iorw\n\tfence r, w\n");
    fprintf(f, "\tmv a0, t3\n\tli a1, -2048\n\tli a2, 305419896\n");
    fprintf(f, "\tnot s2, a3\n\tneg s3, a4\n\tjr t5\n\tjalr t6\n");
    /* Control flow, added for the file assembler. The displacement is
     * written `.+N` so llvm-mc and this file read the same text and
     * mean the same thing. */
    fprintf(f, "\tbeq a0, a1, .+8\n\tbne t3, t4, .-16\n");
    fprintf(f, "\tblt s2, s3, .+2048\n\tbge a4, a5, .-2048\n");
    fprintf(f, "\tbltu t0, t1, .+4\n\tbgeu a6, a7, .-4\n");
    fprintf(f, "\tbeqz a0, .+8\n\tbnez t3, .-8\n\tbltz s2, .+12\n");
    fprintf(f, "\tbgez a4, .-12\n\tblez t5, .+16\n\tbgtz t6, .-16\n");
    fprintf(f, "\tj .+2048\n\tj .-2048\n\tjal .+8\n\tjal t0, .-8\n");
    fprintf(f, "\tjalr a0, 16(t1)\n\tjalr s2, s3\n\tjalr a0, 0(ra)\n");
    fprintf(f, "\tlui a0, 4096\n\tauipc a1, 1\n\tlui t3, 1048575\n");
    fprintf(f, "\tseqz a0, a1\n\tsnez a2, a3\n");
    for (const struct csr *c = csrs; c->name; c++) {
        if (c->rv32 && target_xlen() != 32)
            continue;
        fprintf(f, "\tcsrr a0, %s\n", c->name);
        fprintf(f, "\tcsrw %s, a1\n", c->name);
        fprintf(f, "\tcsrrw s2, %s, a2\n", c->name);
        fprintf(f, "\tcsrrs s3, %s, a3\n", c->name);
        fprintf(f, "\tcsrrc s4, %s, a4\n", c->name);
        fprintf(f, "\tcsrwi %s, 31\n", c->name);
        fprintf(f, "\tcsrrwi s5, %s, 7\n", c->name);
    }
    for (const struct alu_ent *e = alu_tab; e->name; e++) {
        if (e->w && target_xlen() != 64) continue;
        if (e->imm) fprintf(f, "\t%s a0, t3, -17\n", e->name);
        else        fprintf(f, "\t%s a0, t3, s2\n", e->name);
    }
    for (const struct sh_ent *e = sh_tab; e->name; e++) {
        if (e->w && target_xlen() != 64) continue;
        if (e->imm) fprintf(f, "\t%s a0, t3, 5\n", e->name);
        else        fprintf(f, "\t%s a0, t3, s2\n", e->name);
    }
    for (const struct md_ent *e = md_tab; e->name; e++) {
        if (e->w && target_xlen() != 64) continue;
        fprintf(f, "\t%s a0, t3, s2\n", e->name);
    }
    for (const struct ls_ent *e = ls_tab; e->name; e++) {
        if (e->size == 8 && target_xlen() != 64) continue;
        if (!strcmp(e->name, "lwu") && target_xlen() != 64) continue;
        fprintf(f, "\t%s a0, -8(t3)\n", e->name);
    }
    /* The F and D forms, for a target whose -march= has them: each
     * format, each rounding mode once, registers from all four ABI
     * groups and both ends of the numbering. */
    {
        static const char *const rm[6] = { "rne", "rtz", "rdn", "rup", "rmm", "dyn" };
        static const char *const ar[4] = { "fadd", "fsub", "fmul", "fdiv" };
        static const char *const fm[4] = { "fmadd", "fmsub", "fnmsub", "fnmadd" };
        static const char *const ity[4] = { "w", "wu", "l", "lu" };
        int flen = target_riscv_flen(), nfmt = flen >= 64 ? 2 : flen >= 32 ? 1 : 0;
        for (int d = 0; d < nfmt; d++) {
            const char *F = d ? "d" : "s", *ld = d ? "fld" : "flw", *st = d ? "fsd" : "fsw";
            fprintf(f, "\t%s fs1, -8(a0)\n\t%s ft11, 2047(sp)\n", ld, st);
            fprintf(f, "\t%s f0, 0(t6)\n\t%s f31, -2048(s11)\n", ld, st);
            for (int k = 0; k < 4; k++) {
                fprintf(f, "\t%s.%s fa0, fa1, fs11\n", ar[k], F);
                fprintf(f, "\t%s.%s ft0, ft8, fs2, %s\n", ar[k], F, rm[k]);
                fprintf(f, "\t%s.%s f1, f2, f3, f4\n", fm[k], F);
                fprintf(f, "\t%s.%s fs3, fa7, ft4, ft5, %s\n", fm[k], F, rm[k + 1]);
            }
            fprintf(f, "\tfsqrt.%s fs2, ft3\n\tfsqrt.%s fs2, ft3, %s\n", F, F, rm[4]);
            fprintf(f, "\tfsgnj.%s fa0, fa1, fa2\n\tfsgnjn.%s ft9, ft10, ft11\n", F, F);
            fprintf(f, "\tfsgnjx.%s f5, f6, f7\n\tfmin.%s fs4, fs5, fs6\n", F, F);
            fprintf(f, "\tfmax.%s fs7, fs8, fs9\n\tfmv.%s fs10, fa3\n", F, F);
            fprintf(f, "\tfneg.%s fa4, fa5\n\tfabs.%s ft6, ft7\n", F, F);
            fprintf(f, "\tfeq.%s a0, fa0, fa1\n\tflt.%s s2, ft0, fs0\n", F, F);
            fprintf(f, "\tfle.%s t6, f31, f0\n\tfclass.%s a5, fs11\n", F, F);
            for (int k = 0; k < (target_xlen() == 64 ? 4 : 2); k++) {
                fprintf(f, "\tfcvt.%s.%s a0, fa0\n", ity[k], F);
                fprintf(f, "\tfcvt.%s.%s t1, fs1, %s\n", ity[k], F, rm[k + 1]);
                fprintf(f, "\tfcvt.%s.%s fa2, a3\n", F, ity[k]);
                fprintf(f, "\tfcvt.%s.%s ft2, s4, %s\n", F, ity[k], rm[k]);
            }
        }
        if (nfmt == 2)
            fprintf(f, "\tfcvt.s.d fa0, fa1\n\tfcvt.s.d ft0, fs0, rtz\n"
                       "\tfcvt.d.s fa2, fa3\n");
        if (nfmt) {
            fprintf(f, "\tfmv.x.w a0, fa0\n\tfmv.w.x ft11, s11\n");
            fprintf(f, "\tfrcsr a0\n\tfscsr a1\n\tfscsr a2, a3\n");
            fprintf(f, "\tfrrm t0\n\tfsrm t1\n\tfsrm t2, t3\n\tfsrmi 4\n");
            fprintf(f, "\tfsrmi a4, 1\n\tfrflags s1\n\tfsflags s2\n");
            fprintf(f, "\tfsflags s3, s4\n\tfsflagsi 31\n\tfsflagsi a5, 0\n");
        }
        if (nfmt == 2 && target_xlen() == 64)
            fprintf(f, "\tfmv.x.d a0, fa0\n\tfmv.d.x ft11, s11\n");
    }
    /* The data directives, last: each spelling, both ends of each
     * width's range and an expression -- 60 bytes, so whatever follows
     * stays aligned. */
    fprintf(f, "\t.byte 255, -128, 0x7f, 0\n\t.half -32768, 0xffff\n");
    fprintf(f, "\t.short 1\n\t.hword -2\n\t.2byte 0x1234, 7\n");
    fprintf(f, "\t.word 0x12345678, -2147483648\n\t.long 4294967295\n");
    fprintf(f, "\t.int (1 << 20) | 0x13\n\t.4byte 0x0000100f\n");
    fprintf(f, "\t.dword 0x123456789abcdef0\n\t.quad -1\n");
    fprintf(f, "\t.8byte 5\n");
}
