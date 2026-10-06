/* The MIPS32r2 inline-asm assembler. See asm.h. The only thing this file
 * adds to emit.c is a parser: every range is checked HERE, before the
 * encoder is called, so a template's mistake is a diagnostic and not an
 * internal error. */
#include "asm.h"

#include "emit.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* ---- registers ---------------------------------------------------------- */

int mipsasm_gpr(const char *name, int len)
{
    char buf[8];
    if (len > 0 && name[0] == '$') {
        name++;
        len--;
    }
    if (len <= 0 || len >= (int)sizeof buf)
        return -1;
    memcpy(buf, name, (size_t)len);
    buf[len] = 0;
    if (isdigit((unsigned char)buf[0])) {
        int v = 0;
        for (int i = 0; i < len; i++) {
            if (!isdigit((unsigned char)buf[i]))
                return -1;
            v = v * 10 + (buf[i] - '0');
        }
        return v < 32 ? v : -1;
    }
    for (int r = 0; r < 32; r++)
        if (strcmp(buf, mips_reg_name(r)) == 0)
            return r;
    if (strcmp(buf, "s8") == 0)
        return 30;                  /* fp's other name */
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
        {
            int depth = 0;
            while (i < len && (depth > 0 ||
                   (!isspace((unsigned char)stmt[i]) && stmt[i] != ','))) {
                if (stmt[i] == '(') depth++;
                else if (stmt[i] == ')') depth--;
                i++;
            }
        }
        t[n].len = (int)(stmt + i - t[n].s);
        if (t[n].len > 0)
            n++;
    }
    return n;
}

static int tok_is(const struct tok *t, const char *s)
{
    return (int)strlen(s) == t->len && strncmp(t->s, s, (size_t)t->len) == 0;
}

static int tok_reg(const struct tok *t) { return mipsasm_gpr(t->s, t->len); }

/* A signed integer: decimal or 0x, with an optional sign. */
static int tok_imm(const struct tok *t, long long *out)
{
    int i = 0, neg = 0, base = 10, any = 0;
    long long v = 0;
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
        if (v > 0x7fffffffffffLL)
            return 0;
        v = v * base + d;
        any = 1;
    }
    if (!any)
        return 0;
    *out = neg ? -v : v;
    return 1;
}

/* `off(reg)`, `(reg)`, as a load or store writes its address. */
static int tok_mem(const struct tok *t, int *reg, long long *off)
{
    const char *open = memchr(t->s, '(', (size_t)t->len);
    struct tok o, r;
    if (!open || t->s[t->len - 1] != ')')
        return 0;
    o.s = t->s; o.len = (int)(open - t->s);
    r.s = open + 1; r.len = (int)(t->s + t->len - 1 - (open + 1));
    *off = 0;
    if (o.len > 0 && !tok_imm(&o, off))
        return 0;
    *reg = tok_reg(&r);
    return *reg >= 0;
}

/* ---- the instruction tables --------------------------------------------- */

struct r3_ent { const char *name; int op; };
static const struct r3_ent r3_tab[] = {
    { "addu", MIPS_ADDU }, { "subu", MIPS_SUBU }, { "and", MIPS_AND },
    { "or", MIPS_OR }, { "xor", MIPS_XOR }, { "nor", MIPS_NOR },
    { "slt", MIPS_SLT }, { "sltu", MIPS_SLTU }, { "movn", MIPS_MOVN },
    { "movz", MIPS_MOVZ }, { "mul", MIPS_MUL },
    { "sllv", MIPS_SLLV }, { "srlv", MIPS_SRLV }, { "srav", MIPS_SRAV },
    { "rotrv", MIPS_ROTRV },
    { NULL, 0 }
};

struct ri_ent { const char *name; int op; };
static const struct ri_ent ri_tab[] = {
    { "addiu", MIPS_ADDIU }, { "slti", MIPS_SLTI }, { "sltiu", MIPS_SLTIU },
    { "andi", MIPS_ANDI }, { "ori", MIPS_ORI }, { "xori", MIPS_XORI },
    { NULL, 0 }
};

struct sh_ent { const char *name; int op; };
static const struct sh_ent sh_tab[] = {
    { "sll", MIPS_SLL }, { "srl", MIPS_SRL }, { "sra", MIPS_SRA },
    { "rotr", MIPS_ROTR },
    { NULL, 0 }
};

struct ls_ent { const char *name; int kind; int size; int sign; };
/* kind: 0 load, 1 store, 2 ll, 3 sc, 4 lwl, 5 lwr, 6 swl, 7 swr */
static const struct ls_ent ls_tab[] = {
    { "lb", 0, 1, 1 }, { "lbu", 0, 1, 0 }, { "lh", 0, 2, 1 },
    { "lhu", 0, 2, 0 }, { "lw", 0, 4, 1 },
    { "sb", 1, 1, 0 }, { "sh", 1, 2, 0 }, { "sw", 1, 4, 0 },
    { "ll", 2, 4, 0 }, { "sc", 3, 4, 0 },
    { "lwl", 4, 4, 0 }, { "lwr", 5, 4, 0 }, { "swl", 6, 4, 0 },
    { "swr", 7, 4, 0 },
    { NULL, 0, 0, 0 }
};

struct br_ent { const char *name; int cond; int nregs; };
static const struct br_ent br_tab[] = {
    { "beq", MIPS_BEQ, 2 }, { "bne", MIPS_BNE, 2 },
    { "blez", MIPS_BLEZ, 1 }, { "bgtz", MIPS_BGTZ, 1 },
    { "bltz", MIPS_BLTZ, 1 }, { "bgez", MIPS_BGEZ, 1 },
    { "beqz", MIPS_BEQ, -1 }, { "bnez", MIPS_BNE, -1 },
    { "b", MIPS_BEQ, 0 }, { "bal", MIPS_BAL, 0 },
    { NULL, 0, 0 }
};

#define FAIL(...)  do { snprintf(err, (size_t)errlen, __VA_ARGS__); \
                        return -1; } while (0)

static int one_stmt(const char *stmt, int len, struct code *out,
                    char *err, int errlen)
{
    struct tok t[MAXTOK];
    int n = split(stmt, len, t, MAXTOK);
    long long v;

    if (n == 0)
        return 0;
    /* `.set noreorder` and the rest: the template's delay slots are its
     * own (GCC's asm is in noreorder mode too), so these say nothing */
    if (tok_is(&t[0], ".set"))
        return 0;

    if (n == 1) {
        if (tok_is(&t[0], "nop"))     { mips_nop(out); return 0; }
        if (tok_is(&t[0], "ehb"))     { mips_ehb(out); return 0; }
        if (tok_is(&t[0], "eret"))    { mips_eret(out); return 0; }
        if (tok_is(&t[0], "wait"))    { mips_wait(out); return 0; }
        if (tok_is(&t[0], "syscall")) { mips_syscall(out); return 0; }
        if (tok_is(&t[0], "break"))   { mips_break(out, 0); return 0; }
        if (tok_is(&t[0], "sync"))    { mips_sync(out, 0); return 0; }
        if (tok_is(&t[0], "di"))      { mips_di(out, MIPS_ZERO); return 0; }
        if (tok_is(&t[0], "ei"))      { mips_ei(out, MIPS_ZERO); return 0; }
    }

    /* ---- three registers ---- */
    for (const struct r3_ent *e = r3_tab; e->name; e++) {
        int d, a, b;
        if (!tok_is(&t[0], e->name))
            continue;
        if (n != 4) FAIL("%s takes three registers", e->name);
        d = tok_reg(&t[1]); a = tok_reg(&t[2]); b = tok_reg(&t[3]);
        if (d < 0 || a < 0 || b < 0) FAIL("%s wants registers", e->name);
        mips_alu(out, e->op, d, a, b);
        return 0;
    }
    /* ---- register, register, immediate ---- */
    for (const struct ri_ent *e = ri_tab; e->name; e++) {
        int d, a;
        if (!tok_is(&t[0], e->name))
            continue;
        if (n != 4) FAIL("%s takes two registers and an immediate", e->name);
        d = tok_reg(&t[1]); a = tok_reg(&t[2]);
        if (d < 0 || a < 0 || !tok_imm(&t[3], &v))
            FAIL("%s wants two registers and an immediate", e->name);
        if (!mips_alu_imm_ok(e->op, v))
            FAIL("%s immediate %lld does not fit its %s 16-bit field",
                 e->name, v, e->op <= MIPS_SLTIU ? "signed" : "unsigned");
        mips_alu_imm(out, e->op, d, a, v);
        return 0;
    }
    for (const struct sh_ent *e = sh_tab; e->name; e++) {
        int d, a;
        if (!tok_is(&t[0], e->name))
            continue;
        if (n != 4) FAIL("%s takes two registers and an amount", e->name);
        d = tok_reg(&t[1]); a = tok_reg(&t[2]);
        if (d < 0 || a < 0 || !tok_imm(&t[3], &v) || v < 0 || v > 31)
            FAIL("%s wants two registers and an amount 0..31", e->name);
        mips_shift_imm(out, e->op, d, a, (int)v);
        return 0;
    }
    if (tok_is(&t[0], "lui")) {
        int d = n == 3 ? tok_reg(&t[1]) : -1;
        if (d < 0 || !tok_imm(&t[2], &v) || v < 0 || v > 0xffff)
            FAIL("lui wants a register and a 0..65535 immediate");
        mips_lui(out, d, (unsigned)v);
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
        if (!mips_fits16(off, 1))
            FAIL("%s offset %lld does not fit a signed 16-bit field",
                 e->name, off);
        switch (e->kind) {
        case 0: mips_load(out, r, base, (int)off, e->size, e->sign); break;
        case 1: mips_store(out, r, base, (int)off, e->size); break;
        case 2: mips_ll(out, r, base, (int)off); break;
        case 3: mips_sc(out, r, base, (int)off); break;
        case 4: mips_lwl(out, r, base, (int)off); break;
        case 5: mips_lwr(out, r, base, (int)off); break;
        case 6: mips_swl(out, r, base, (int)off); break;
        default: mips_swr(out, r, base, (int)off); break;
        }
        return 0;
    }

    /* ---- multiply, divide, HI/LO ---- */
    if (tok_is(&t[0], "mult") || tok_is(&t[0], "multu") ||
        tok_is(&t[0], "div") || tok_is(&t[0], "divu")) {
        int op = tok_is(&t[0], "mult") ? MIPS_MULT
               : tok_is(&t[0], "multu") ? MIPS_MULTU
               : tok_is(&t[0], "div") ? MIPS_DIV : MIPS_DIVU;
        int a, b, k = 1;
        /* `div $zero, a, b` is the instruction itself, as gas and llvm-mc
         * write it. `div a, b` and `div d, a, b` are their MACRO, which
         * adds a divide-by-zero trap (and for two registers writes the
         * quotient back over a) -- not expanded here, so refused rather
         * than read as the bare instruction. mult and multu have no
         * macro form. */
        if (op == MIPS_DIV || op == MIPS_DIVU) {
            if (n != 4 || tok_reg(&t[1]) != MIPS_ZERO)
                FAIL("this %.*s is an assembler macro (it adds a "
                     "divide-by-zero trap); write %.*s $zero, rs, rt and "
                     "read the quotient with mflo", t[0].len, t[0].s,
                     t[0].len, t[0].s);
            k = 2;
        } else if (n != 3) {
            FAIL("%.*s takes two registers", t[0].len, t[0].s);
        }
        a = tok_reg(&t[k]); b = tok_reg(&t[k + 1]);
        if (a < 0 || b < 0) FAIL("%.*s wants registers", t[0].len, t[0].s);
        mips_muldiv(out, op, a, b);
        return 0;
    }
    if (n == 2 && (tok_is(&t[0], "mfhi") || tok_is(&t[0], "mflo") ||
                   tok_is(&t[0], "mthi") || tok_is(&t[0], "mtlo") ||
                   tok_is(&t[0], "jr") || tok_is(&t[0], "jalr") ||
                   tok_is(&t[0], "di") || tok_is(&t[0], "ei"))) {
        int r = tok_reg(&t[1]);
        if (r < 0) FAIL("%.*s wants a register", t[0].len, t[0].s);
        if (tok_is(&t[0], "mfhi")) mips_mfhi(out, r);
        else if (tok_is(&t[0], "mflo")) mips_mflo(out, r);
        else if (tok_is(&t[0], "mthi")) mips_mthi(out, r);
        else if (tok_is(&t[0], "mtlo")) mips_mtlo(out, r);
        else if (tok_is(&t[0], "jr")) mips_jr(out, r);
        else if (tok_is(&t[0], "jalr")) mips_jalr(out, MIPS_RA, r);
        else if (tok_is(&t[0], "di")) mips_di(out, r);
        else mips_ei(out, r);
        return 0;
    }
    if (n == 3 && tok_is(&t[0], "jalr")) {
        int d = tok_reg(&t[1]), s = tok_reg(&t[2]);
        if (d < 0 || s < 0 || d == s)
            FAIL("jalr wants two different registers");
        mips_jalr(out, d, s);
        return 0;
    }

    /* ---- Release 2's bit manipulation ---- */
    if (n == 3 && (tok_is(&t[0], "clz") || tok_is(&t[0], "clo") ||
                   tok_is(&t[0], "seb") || tok_is(&t[0], "seh") ||
                   tok_is(&t[0], "wsbh"))) {
        int d = tok_reg(&t[1]), s = tok_reg(&t[2]);
        if (d < 0 || s < 0) FAIL("%.*s wants two registers", t[0].len, t[0].s);
        if (tok_is(&t[0], "clz")) mips_clz(out, d, s);
        else if (tok_is(&t[0], "clo")) mips_clo(out, d, s);
        else if (tok_is(&t[0], "seb")) mips_seb(out, d, s);
        else if (tok_is(&t[0], "seh")) mips_seh(out, d, s);
        else mips_wsbh(out, d, s);
        return 0;
    }
    if (n == 5 && (tok_is(&t[0], "ext") || tok_is(&t[0], "ins"))) {
        int d = tok_reg(&t[1]), s = tok_reg(&t[2]);
        long long pos, size;
        if (d < 0 || s < 0 || !tok_imm(&t[3], &pos) || !tok_imm(&t[4], &size) ||
            pos < 0 || pos > 31 || size < 1 || pos + size > 32)
            FAIL("%.*s wants two registers, a position and a size within "
                 "the word", t[0].len, t[0].s);
        if (tok_is(&t[0], "ext")) mips_ext(out, d, s, (int)pos, (int)size);
        else                      mips_ins(out, d, s, (int)pos, (int)size);
        return 0;
    }

    /* ---- the system ---- */
    if ((tok_is(&t[0], "mfc0") || tok_is(&t[0], "mtc0")) &&
        (n == 3 || n == 4)) {
        int r = tok_reg(&t[1]), c0 = tok_reg(&t[2]);
        long long sel = 0;
        if (r < 0 || c0 < 0 || (n == 4 && (!tok_imm(&t[3], &sel) ||
                                           sel < 0 || sel > 7)))
            FAIL("%.*s wants a register, a coprocessor 0 register and a "
                 "select 0..7", t[0].len, t[0].s);
        if (tok_is(&t[0], "mfc0")) mips_mfc0(out, r, c0, (int)sel);
        else                       mips_mtc0(out, r, c0, (int)sel);
        return 0;
    }
    if (n == 2 && (tok_is(&t[0], "break") || tok_is(&t[0], "sync"))) {
        if (!tok_imm(&t[1], &v) || v < 0 ||
            v > (tok_is(&t[0], "break") ? 1023 : 31))
            FAIL("%.*s's code is out of range", t[0].len, t[0].s);
        if (tok_is(&t[0], "break")) mips_break(out, (int)v);
        else                        mips_sync(out, (int)v);
        return 0;
    }
    if (tok_is(&t[0], "teq") && (n == 3 || n == 4)) {
        int a = tok_reg(&t[1]), b = tok_reg(&t[2]);
        v = 0;
        if (a < 0 || b < 0 || (n == 4 && (!tok_imm(&t[3], &v) || v < 0 ||
                                          v > 1023)))
            FAIL("teq wants two registers and a code 0..1023");
        mips_teq(out, a, b, (int)v);
        return 0;
    }

    /* ---- branches, by a numeric displacement from the delay slot ---- */
    for (const struct br_ent *e = br_tab; e->name; e++) {
        int a = MIPS_ZERO, b = MIPS_ZERO, k = 1;
        if (!tok_is(&t[0], e->name))
            continue;
        if (n != 2 + (e->nregs < 0 ? 1 : e->nregs))
            FAIL("%s takes %d register%s and a displacement", e->name,
                 e->nregs < 0 ? 1 : e->nregs, e->nregs == 1 || e->nregs < 0
                                              ? "" : "s");
        if (e->nregs != 0) { a = tok_reg(&t[k++]); if (a < 0) FAIL("%s wants a register", e->name); }
        if (e->nregs == 2) { b = tok_reg(&t[k++]); if (b < 0) FAIL("%s wants a register", e->name); }
        if (!tok_imm(&t[k], &v) || (v & 3) || v < -131072 || v > 131068)
            FAIL("%s's displacement must be a multiple of 4 in "
                 "-131072..131068 bytes from the delay slot", e->name);
        mips_w(out, mips_enc_branch(e->cond, a, b, (long)v));
        return 0;
    }

    /* ---- the pseudo-instructions ---- */
    if (tok_is(&t[0], "move") && n == 3) {
        int d = tok_reg(&t[1]), s = tok_reg(&t[2]);
        if (d < 0 || s < 0) FAIL("move wants two registers");
        mips_mv(out, d, s);
        return 0;
    }
    if (tok_is(&t[0], "li") && n == 3) {
        int d = tok_reg(&t[1]);
        if (d < 0 || !tok_imm(&t[2], &v) || v < -2147483647LL - 1 ||
            v > 0xffffffffLL)
            FAIL("li wants a register and a 32-bit constant");
        mips_li(out, d, v);
        return 0;
    }
    /* not neg: gas and llvm-mc spell `neg` as the TRAPPING sub */
    if ((tok_is(&t[0], "not") || tok_is(&t[0], "negu")) && n == 3) {
        int d = tok_reg(&t[1]), s = tok_reg(&t[2]);
        if (d < 0 || s < 0) FAIL("%.*s wants two registers", t[0].len, t[0].s);
        if (tok_is(&t[0], "not")) mips_alu(out, MIPS_NOR, d, s, MIPS_ZERO);
        else                      mips_alu(out, MIPS_SUBU, d, MIPS_ZERO, s);
        return 0;
    }
    FAIL("asm instruction \"%.*s\" is not in the MIPS vocabulary",
         (int)(t[n - 1].s + t[n - 1].len - t[0].s), t[0].s);
}

int mipsasm_assemble(const char *text, struct code *out, char *err, int errlen)
{
    const char *p = text;
    while (*p) {
        const char *start = p;
        int len;
        while (*p && *p != ';' && *p != '\n')
            p++;
        len = (int)(p - start);
        for (int i = 0; i < len; i++)
            if (start[i] == '#' || (start[i] == '/' && i + 1 < len &&
                                    start[i + 1] == '/')) {
                len = i;
                break;
            }
        if (one_stmt(start, len, out, err, errlen) != 0)
            return -1;
        if (*p)
            p++;
    }
    return 0;
}

/* ---- the referee's input -------------------------------------------------- */

void mipsasm_vocabulary(FILE *f)
{
    fprintf(f, "\tnop\n\tehb\n\teret\n\twait\n\tsyscall\n\tbreak\n");
    fprintf(f, "\tbreak 3\n\tsync\n\tsync 4\n\tdi\n\tei\n\tdi $t0\n\tei $a1\n");
    fprintf(f, "\tmove $a0, $t9\n\tli $v0, -32768\n\tli $v1, 65535\n");
    fprintf(f, "\tli $t0, 305419896\n\tli $s7, -1\n\tnot $s2, $a3\n");
    fprintf(f, "\tnegu $t1, $t2\n\tjr $ra\n\tjalr $t9\n");
    fprintf(f, "\tjalr $s0, $t9\n\tlui $a0, 4660\n\tlui $1, 0\n");
    for (const struct r3_ent *e = r3_tab; e->name; e++)
        fprintf(f, "\t%s $v0, $a1, $t7\n\t%s $s8, $zero, $31\n",
                e->name, e->name);
    for (const struct ri_ent *e = ri_tab; e->name; e++) {
        if (e->op <= MIPS_SLTIU)
            fprintf(f, "\t%s $a0, $t3, -32768\n\t%s $s1, $sp, 32767\n",
                    e->name, e->name);
        else
            fprintf(f, "\t%s $a0, $t3, 0\n\t%s $s1, $sp, 0xffff\n",
                    e->name, e->name);
    }
    for (const struct sh_ent *e = sh_tab; e->name; e++)
        fprintf(f, "\t%s $a0, $t3, 31\n\t%s $k0, $gp, 1\n", e->name, e->name);
    for (const struct ls_ent *e = ls_tab; e->name; e++)
        fprintf(f, "\t%s $a2, -4($sp)\n\t%s $t8, 32767($a1)\n\t%s $3, ($4)\n",
                e->name, e->name, e->name);
    fprintf(f, "\tmult $a0, $a1\n\tmultu $t8, $t9\n\tdiv $zero, $s0, $s1\n");
    fprintf(f, "\tdivu $zero, $v0, $v1\n");
    fprintf(f, "\tmfhi $a0\n\tmflo $s7\n\tmthi $t3\n\tmtlo $ra\n");
    fprintf(f, "\tclz $a0, $a1\n\tclo $t0, $t1\n\tseb $s0, $s1\n");
    fprintf(f, "\tseh $v0, $a3\n\twsbh $t2, $t3\n");
    fprintf(f, "\text $a0, $a1, 0, 32\n\text $t0, $t1, 31, 1\n");
    fprintf(f, "\tins $s0, $zero, 4, 8\n\tins $v0, $a3, 0, 1\n");
    fprintf(f, "\tmfc0 $t0, $12\n\tmfc0 $a0, $15, 1\n\tmtc0 $t1, $12, 0\n");
    fprintf(f, "\tmtc0 $zero, $13, 7\n\tteq $a0, $zero\n\tteq $t0, $t1, 7\n");
    for (const struct br_ent *e = br_tab; e->name; e++) {
        const char *regs = e->nregs == 2 ? "$a0, $t1, "
                         : e->nregs != 0 ? "$s2, " : "";
        fprintf(f, "\t%s %s-131072\n\t%s %s8\n\t%s %s131068\n",
                e->name, regs, e->name, regs, e->name, regs);
    }
}
