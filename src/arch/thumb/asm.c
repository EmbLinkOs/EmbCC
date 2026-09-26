/* The ARMv7-M inline-asm assembler. See asm.h for the vocabulary and why
 * it is that vocabulary.
 *
 * Every instruction goes out through src/arch/thumb/emit.c rather than
 * being encoded here, so the code generator and inline assembly cannot
 * disagree about a bit -- and tools/thumbcheck already round-trips those
 * encoders through llvm-objdump. What this file adds is a PARSER and the
 * special-register vocabulary.
 */
#define _POSIX_C_SOURCE 200809L
#include "asm.h"

#include "emit.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

/* ---- registers ---------------------------------------------------------- */

static const char *const reg_name[16] = {
    "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
    "r8", "r9", "r10", "r11", "r12", "sp", "lr", "pc"
};

int tasm_gpr(const char *name, int len)
{
    if (len <= 0)
        return -1;
    for (int i = 0; i < 16; i++)
        if ((int)strlen(reg_name[i]) == len &&
            strncmp(name, reg_name[i], (size_t)len) == 0)
            return i;
    /* The alternate spellings hand-written asm uses. */
    if (len == 2 && strncmp(name, "ip", 2) == 0) return 12;
    if (len == 2 && strncmp(name, "fp", 2) == 0) return 11;
    if (len == 3 && strncmp(name, "r13", 3) == 0) return 13;
    if (len == 3 && strncmp(name, "r14", 3) == 0) return 14;
    if (len == 3 && strncmp(name, "r15", 3) == 0) return 15;
    return -1;
}

const char *t_reg_name(int reg)
{
    return (reg >= 0 && reg < 16) ? reg_name[reg] : "?";
}

/* ---- the special registers ---------------------------------------------
 *
 * The SYSm number IS the encoding, so a wrong one reads a different
 * register perfectly legally -- which is why every name is listed rather
 * than a number being accepted. thumbcheck round-trips all of them.
 */
struct sysreg { const char *name; int sysm; };
static const struct sysreg sysregs[] = {
    { "apsr", T_SYS_APSR }, { "iapsr", T_SYS_IAPSR },
    { "eapsr", T_SYS_EAPSR }, { "xpsr", T_SYS_XPSR },
    { "ipsr", T_SYS_IPSR }, { "epsr", T_SYS_EPSR },
    { "iepsr", T_SYS_IEPSR },
    { "msp", T_SYS_MSP }, { "psp", T_SYS_PSP },
    { "primask", T_SYS_PRIMASK }, { "basepri", T_SYS_BASEPRI },
    { "basepri_max", T_SYS_BASEPRI_MAX },
    { "faultmask", T_SYS_FAULTMASK }, { "control", T_SYS_CONTROL },
    { NULL, 0 }
};

static int sysreg_num(const char *s, int len)
{
    for (const struct sysreg *r = sysregs; r->name; r++)
        if ((int)strlen(r->name) == len &&
            strncasecmp(s, r->name, (size_t)len) == 0)
            return r->sysm;
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
                if (stmt[i] == '[' || stmt[i] == '{') depth++;
                else if (stmt[i] == ']' || stmt[i] == '}') depth--;
                i++;
            }
        }
        t[n].len = (int)(stmt + i - t[n].s);
        if (t[n].len > 0)
            n++;
    }
    return n;
}

/* A mnemonic, with any `.w`/`.n` width suffix removed. The encoders pick
 * the width themselves -- t_alu_imm says when a narrow form will not hold
 * a value and the caller widens -- so a suffix is a request for something
 * already decided, and honouring it would mean a second width policy that
 * could disagree with the first. */
static int mnemonic_is(const struct tok *t, const char *name)
{
    int len = t->len;
    if (len > 2 && t->s[len - 2] == '.' &&
        (t->s[len - 1] == 'w' || t->s[len - 1] == 'n'))
        len -= 2;
    return (int)strlen(name) == len && strncmp(t->s, name, (size_t)len) == 0;
}

static int tok_is(const struct tok *t, const char *s)
{
    return (int)strlen(s) == t->len && strncmp(t->s, s, (size_t)t->len) == 0;
}

static int tok_reg(const struct tok *t) { return tasm_gpr(t->s, t->len); }

/* `#imm`, or a bare number. */
static int tok_imm(const struct tok *t, long *out)
{
    int i = 0, neg = 0, base = 10, any = 0;
    long v = 0;
    if (i < t->len && t->s[i] == '#')
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

/* `[rN]` or `[rN, #off]`, which `split` kept as one token. */
static int tok_mem(const struct tok *t, int *reg, long *off)
{
    struct tok inner[3];
    int n;
    if (t->len < 3 || t->s[0] != '[' || t->s[t->len - 1] != ']')
        return 0;
    inner[0].s = t->s + 1;
    inner[0].len = t->len - 2;
    n = split(inner[0].s, inner[0].len, inner, 3);
    if (n < 1)
        return 0;
    *reg = tok_reg(&inner[0]);
    *off = 0;
    if (*reg < 0)
        return 0;
    if (n >= 2 && !tok_imm(&inner[1], off))
        return 0;
    return 1;
}

/* ---- the instruction table ---------------------------------------------- */

/* `s` is the flag-setting suffix, and it is a separate column rather
 * than a suffix stripped and forgotten. An earlier version mapped `adds`
 * onto the same entry as `add` and passed s = 0, so a program that wrote
 * `adds` to set the flags for a following branch got an `add` that set
 * nothing -- assembled, ran, and took the wrong branch. The referee
 * caught it because llvm-mc disassembled our bytes as `add.w`. */
struct alu_ent { const char *name; int op; int s; };
static const struct alu_ent alu_tab[] = {
    { "add", T_OP_ADD, 0 }, { "adds", T_OP_ADD, 1 },
    { "sub", T_OP_SUB, 0 }, { "subs", T_OP_SUB, 1 },
    { "and", T_OP_AND, 0 }, { "ands", T_OP_AND, 1 },
    { "orr", T_OP_ORR, 0 }, { "orrs", T_OP_ORR, 1 },
    { "eor", T_OP_EOR, 0 }, { "eors", T_OP_EOR, 1 },
    { "bic", T_OP_BIC, 0 }, { "bics", T_OP_BIC, 1 },
    { "adc", T_OP_ADC, 0 }, { "adcs", T_OP_ADC, 1 },
    { "sbc", T_OP_SBC, 0 }, { "sbcs", T_OP_SBC, 1 },
    { "rsb", T_OP_RSB, 0 }, { "rsbs", T_OP_RSB, 1 },
    { NULL, 0, 0 }
};

struct sh_ent { const char *name; int op; int s; };
static const struct sh_ent sh_tab[] = {
    { "lsl", T_SH_LSL, 0 }, { "lsls", T_SH_LSL, 1 },
    { "lsr", T_SH_LSR, 0 }, { "lsrs", T_SH_LSR, 1 },
    { "asr", T_SH_ASR, 0 }, { "asrs", T_SH_ASR, 1 },
    { "ror", T_SH_ROR, 0 }, { "rors", T_SH_ROR, 1 },
    { NULL, 0, 0 }
};

struct ls_ent { const char *name; int size; int sign; int store; };
static const struct ls_ent ls_tab[] = {
    { "ldrb", 1, 0, 0 }, { "ldrsb", 1, 1, 0 },
    { "ldrh", 2, 0, 0 }, { "ldrsh", 2, 1, 0 },
    { "ldr",  4, 0, 0 },
    { "strb", 1, 0, 1 }, { "strh", 2, 0, 1 }, { "str", 4, 0, 1 },
    { NULL, 0, 0, 0 }
};

#define FAIL(...)  do { snprintf(err, (size_t)errlen, __VA_ARGS__); \
                        return -1; } while (0)

static int one_stmt(const char *stmt, int len, struct code *out,
                    char *err, int errlen)
{
    struct tok t[MAXTOK];
    int n = split(stmt, len, t, MAXTOK);
    long imm;

    if (n == 0)
        return 0;

    /* ---- no operands ---- */
    if (n == 1) {
        if (mnemonic_is(&t[0], "nop"))   { t_hint(out, T_HINT_NOP); return 0; }
        if (mnemonic_is(&t[0], "yield")) { t_hint(out, T_HINT_YIELD); return 0; }
        if (mnemonic_is(&t[0], "wfe"))   { t_hint(out, T_HINT_WFE); return 0; }
        if (mnemonic_is(&t[0], "wfi"))   { t_hint(out, T_HINT_WFI); return 0; }
        if (mnemonic_is(&t[0], "sev"))   { t_hint(out, T_HINT_SEV); return 0; }
        if (mnemonic_is(&t[0], "dsb")) { t_barrier(out, T_BAR_DSB); return 0; }
        if (mnemonic_is(&t[0], "dmb")) { t_barrier(out, T_BAR_DMB); return 0; }
        if (mnemonic_is(&t[0], "isb")) { t_barrier(out, T_BAR_ISB); return 0; }
        if (mnemonic_is(&t[0], "bx"))  FAIL("bx needs a register");
        FAIL("asm instruction \"%.*s\" is not in the ARMv7-M vocabulary",
             t[0].len, t[0].s);
    }

    /* ---- barriers with an explicit option ----
     * Only `sy` exists here: a narrower barrier that is wrong looks
     * exactly like one that works until it does not, and a full barrier
     * is never incorrect. Anything else is refused rather than widened
     * silently. */
    if (mnemonic_is(&t[0], "dsb") || mnemonic_is(&t[0], "dmb") ||
        mnemonic_is(&t[0], "isb")) {
        if (n != 2 || !tok_is(&t[1], "sy"))
            FAIL("only the `sy` barrier option is supported; \"%.*s\" is not",
                 t[1].len, t[1].s);
        t_barrier(out, mnemonic_is(&t[0], "dsb") ? T_BAR_DSB :
                       mnemonic_is(&t[0], "dmb") ? T_BAR_DMB : T_BAR_ISB);
        return 0;
    }

    /* ---- the interrupt masks: what a critical section is here ---- */
    if (mnemonic_is(&t[0], "cpsid") || mnemonic_is(&t[0], "cpsie")) {
        int dis = mnemonic_is(&t[0], "cpsid"), mi = 0, mf = 0;
        if (n != 2) FAIL("%.*s takes i, f, or if", t[0].len, t[0].s);
        for (int k = 0; k < t[1].len; k++) {
            if (t[1].s[k] == 'i') mi = 1;
            else if (t[1].s[k] == 'f') mf = 1;
            else FAIL("\"%.*s\" is not a set of i/f", t[1].len, t[1].s);
        }
        t_cps(out, dis, mi, mf);
        return 0;
    }

    /* ---- the special registers ---- */
    if (mnemonic_is(&t[0], "mrs")) {
        int rd, sys;
        if (n != 3) FAIL("mrs takes a register and a special register");
        rd = tok_reg(&t[1]);
        sys = sysreg_num(t[2].s, t[2].len);
        if (rd < 0) FAIL("\"%.*s\" is not a register", t[1].len, t[1].s);
        if (sys < 0) FAIL("\"%.*s\" is not an ARMv7-M special register",
                          t[2].len, t[2].s);
        t_mrs(out, rd, sys);
        return 0;
    }
    if (mnemonic_is(&t[0], "msr")) {
        int rn, sys;
        if (n != 3) FAIL("msr takes a special register and a register");
        sys = sysreg_num(t[1].s, t[1].len);
        rn = tok_reg(&t[2]);
        if (sys < 0) FAIL("\"%.*s\" is not an ARMv7-M special register",
                          t[1].len, t[1].s);
        if (rn < 0) FAIL("\"%.*s\" is not a register", t[2].len, t[2].s);
        t_msr(out, sys, rn);
        return 0;
    }

    if (mnemonic_is(&t[0], "bkpt") && n == 2) {
        if (!tok_imm(&t[1], &imm) || imm < 0 || imm > 255)
            FAIL("bkpt takes a 0..255 immediate");
        t_bkpt(out, (int)imm);
        return 0;
    }
    if (mnemonic_is(&t[0], "bx") && n == 2) {
        int rm = tok_reg(&t[1]);
        if (rm < 0) FAIL("bx wants a register");
        t_bx(out, rm);
        return 0;
    }
    if (mnemonic_is(&t[0], "blx") && n == 2) {
        int rm = tok_reg(&t[1]);
        if (rm < 0) FAIL("blx wants a register");
        t_blx(out, rm);
        return 0;
    }

    /* ---- two-register forms ---- */
    if (n == 3) {
        int rd = tok_reg(&t[1]), rm = tok_reg(&t[2]);
        if (mnemonic_is(&t[0], "mov") || mnemonic_is(&t[0], "movs")) {
            if (rd < 0) FAIL("mov wants a register destination");
            if (rm >= 0) { t_mov_reg(out, rd, rm); return 0; }
            if (!tok_imm(&t[2], &imm))
                FAIL("\"%.*s\" is neither a register nor an immediate",
                     t[2].len, t[2].s);
            t_mov_imm(out, rd, imm, 0);
            return 0;
        }
        if (mnemonic_is(&t[0], "mvn") || mnemonic_is(&t[0], "mvns")) {
            if (rd < 0 || rm < 0) FAIL("mvn wants two registers");
            t_mvn_reg(out, rd, rm, 0);
            return 0;
        }
        if (mnemonic_is(&t[0], "clz")) {
            if (rd < 0 || rm < 0) FAIL("clz wants two registers");
            t_clz(out, rd, rm);
            return 0;
        }
        if (mnemonic_is(&t[0], "rbit")) {
            if (rd < 0 || rm < 0) FAIL("rbit wants two registers");
            t_rbit(out, rd, rm);
            return 0;
        }
        if (mnemonic_is(&t[0], "rev")) {
            if (rd < 0 || rm < 0) FAIL("rev wants two registers");
            t_rev(out, rd, rm);
            return 0;
        }
        if (mnemonic_is(&t[0], "cmp")) {
            if (rd < 0) FAIL("cmp wants a register");
            if (rm >= 0) { t_cmp_reg(out, rd, rm); return 0; }
            if (!tok_imm(&t[2], &imm)) FAIL("cmp wants a register or an "
                                            "immediate");
            t_cmp_imm(out, rd, imm);
            return 0;
        }
        if (mnemonic_is(&t[0], "tst")) {
            if (rd < 0 || rm < 0) FAIL("tst wants two registers");
            t_tst_reg(out, rd, rm);
            return 0;
        }
        /* The exclusive load, and the ordinary ones. */
        for (const struct ls_ent *e = ls_tab; e->name; e++) {
            int base;
            long off;
            if (!mnemonic_is(&t[0], e->name))
                continue;
            if (rd < 0) FAIL("\"%.*s\" is not a register", t[1].len, t[1].s);
            if (!tok_mem(&t[2], &base, &off))
                FAIL("\"%.*s\" is not a [reg] or [reg, #off] address",
                     t[2].len, t[2].s);
            if (!t_ldst_imm(out, rd, base, off, e->size, e->sign, e->store))
                FAIL("%s offset %ld does not fit its encoding", e->name, off);
            return 0;
        }
        if (mnemonic_is(&t[0], "ldrex")) {
            int base;
            long off;
            if (rd < 0 || !tok_mem(&t[2], &base, &off))
                FAIL("ldrex wants a register and a [reg, #off] address");
            if (off < 0 || off > 1020 || (off & 3))
                FAIL("ldrex offset %ld must be a multiple of 4 in 0..1020",
                     off);
            t_ldrex(out, rd, base, (int)off);
            return 0;
        }
    }

    /* ---- three-operand forms ---- */
    if (n == 4) {
        int rd = tok_reg(&t[1]), rn = tok_reg(&t[2]), rm = tok_reg(&t[3]);
        for (const struct alu_ent *e = alu_tab; e->name; e++) {
            if (!mnemonic_is(&t[0], e->name))
                continue;
            if (rd < 0 || rn < 0) FAIL("%s wants registers", e->name);
            if (rm >= 0) { t_alu_reg(out, e->op, rd, rn, rm, e->s); return 0; }
            if (!tok_imm(&t[3], &imm))
                FAIL("%s wants a register or an immediate", e->name);
            /* addw/subw reach any 0..4095 where the modified immediate
             * reaches only what it can rotate into place. The encoder
             * says which; this does not second-guess it. */
            /* addw/subw reach any 0..4095 but do NOT set flags, so they
             * are only an option when none was asked for. */
            if (!e->s && e->op == T_OP_ADD && imm >= 0 && imm <= 4095 &&
                !t_imm_ok(imm)) {
                t_addw(out, rd, rn, imm);
                return 0;
            }
            if (!e->s && e->op == T_OP_SUB && imm >= 0 && imm <= 4095 &&
                !t_imm_ok(imm)) {
                t_subw(out, rd, rn, imm);
                return 0;
            }
            if (!t_alu_imm(out, e->op, rd, rn, imm, e->s))
                FAIL("%s cannot encode the immediate %ld", e->name, imm);
            return 0;
        }
        for (const struct sh_ent *e = sh_tab; e->name; e++) {
            if (!mnemonic_is(&t[0], e->name))
                continue;
            if (rd < 0 || rn < 0) FAIL("%s wants registers", e->name);
            if (rm >= 0) { t_shift_reg(out, e->op, rd, rn, rm, e->s); return 0; }
            if (!tok_imm(&t[3], &imm) || imm < 0 || imm > 31)
                FAIL("%s wants a 0..31 shift amount", e->name);
            t_shift_imm(out, e->op, rd, rn, (int)imm, e->s);
            return 0;
        }
        if (mnemonic_is(&t[0], "mul")) {
            if (rd < 0 || rn < 0 || rm < 0) FAIL("mul wants three registers");
            t_mul(out, rd, rn, rm);
            return 0;
        }
        if (mnemonic_is(&t[0], "udiv") || mnemonic_is(&t[0], "sdiv")) {
            if (rd < 0 || rn < 0 || rm < 0) FAIL("%.*s wants three registers",
                                                 t[0].len, t[0].s);
            t_div(out, rd, rn, rm, mnemonic_is(&t[0], "sdiv"));
            return 0;
        }
        if (mnemonic_is(&t[0], "strex")) {
            int base;
            long off;
            if (rd < 0 || rn < 0 || !tok_mem(&t[3], &base, &off))
                FAIL("strex wants two registers and a [reg, #off] address");
            if (off < 0 || off > 1020 || (off & 3))
                FAIL("strex offset %ld must be a multiple of 4 in 0..1020",
                     off);
            t_strex(out, rd, rn, base, (int)off);
            return 0;
        }
    }

    FAIL("asm instruction \"%.*s\" is not in the ARMv7-M vocabulary",
         t[0].len, t[0].s);
}

int tasm_assemble(const char *text, struct code *out, char *err, int errlen)
{
    const char *p = text;
    err[0] = 0;
    while (*p) {
        const char *start = p;
        int len;
        while (*p && *p != ';' && *p != '\n')
            p++;
        len = (int)(p - start);
        for (int i = 0; i < len; i++)
            if (start[i] == '@' || (start[i] == '/' && i + 1 < len &&
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

/* ---- the referee's input ------------------------------------------------ */

void tasm_vocabulary(FILE *f)
{
    fprintf(f, "\tnop\n\tyield\n\twfe\n\twfi\n\tsev\n");
    fprintf(f, "\tdsb sy\n\tdmb sy\n\tisb sy\n");
    fprintf(f, "\tcpsid i\n\tcpsie i\n\tcpsid f\n\tcpsie f\n\tcpsid if\n");
    for (const struct sysreg *r = sysregs; r->name; r++) {
        fprintf(f, "\tmrs r0, %s\n", r->name);
        /* The read-only ones are still written here: the ENCODING is
         * what this checks, and the hardware's opinion of the write is
         * not the assembler's business. */
        fprintf(f, "\tmsr %s, r1\n", r->name);
    }
    fprintf(f, "\tbkpt #0\n\tbkpt #170\n");
    /* `mov rd, #imm` is left out on purpose. t_mov_imm prefers `movw`
     * where llvm-mc prefers the modified-immediate `mov.w`; both are
     * four bytes and identical in effect, so a byte comparison would
     * report a house style as a fault. The encoder itself is proven by
     * tools/thumbcheck, which asks what the bytes MEAN rather than
     * whether they match another assembler's preference. */
    fprintf(f, "\tmov r0, r9\n\tmvn r2, r3\n");
    fprintf(f, "\tclz r4, r5\n\trbit r6, r7\n\trev r0, r1\n");
    fprintf(f, "\tcmp r0, r1\n\tcmp r2, #7\n\ttst r3, r4\n");
    fprintf(f, "\tbx lr\n\tblx r3\n");
    fprintf(f, "\tmul r0, r1, r2\n\tudiv r3, r4, r5\n\tsdiv r6, r7, r8\n");
    for (const struct alu_ent *e = alu_tab; e->name; e++) {
        fprintf(f, "\t%s r0, r1, r2\n", e->name);
        fprintf(f, "\t%s r3, r4, #12\n", e->name);
    }
    for (const struct sh_ent *e = sh_tab; e->name; e++) {
        fprintf(f, "\t%s r0, r1, r2\n", e->name);
        fprintf(f, "\t%s r3, r4, #5\n", e->name);
    }
    for (const struct ls_ent *e = ls_tab; e->name; e++)
        fprintf(f, "\t%s r0, [r1, #8]\n", e->name);
    fprintf(f, "\tldrex r0, [r1]\n\tldrex r2, [r3, #16]\n");
    fprintf(f, "\tstrex r0, r1, [r2]\n\tstrex r3, r4, [r5, #8]\n");
}
