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
#include <stdlib.h>
#include <string.h>

#include "../../elf/elf.h"

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

/* Case-blind, in ISO C: strncasecmp is POSIX's, and the compiler is meant
 * to build against any hosted C library (src/platform). */
static int same_nocase(const char *a, const char *b, int len)
{
    for (int i = 0; i < len; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return 0;
    return 1;
}

static int sysreg_num(const char *s, int len)
{
    for (const struct sysreg *r = sysregs; r->name; r++)
        if ((int)strlen(r->name) == len && same_nocase(s, r->name, len))
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
    /* `.+8` / `.-12`: a displacement from this instruction. The file
     * assembler (src/as/gas.c) turns a label into exactly this before
     * calling, and a branch operand can mean nothing else here --
     * this layer sees no labels and does not know its own address. */
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

/* A register list, `{r4-r11, lr}` -- one token, since `split` keeps a
 * braced group together -- as a mask. Ranges and single registers, in any
 * order; 0 when anything in it is not a core register. */
static int tok_reglist(const struct tok *t, unsigned *mask)
{
    const char *p = t->s, *end = t->s + t->len;
    *mask = 0;
    while (p < end && isspace((unsigned char)*p)) p++;
    if (p >= end || *p != '{') return 0;
    p++;
    while (end > p && isspace((unsigned char)end[-1])) end--;
    if (end <= p || end[-1] != '}') return 0;
    end--;
    while (p < end) {
        const char *a, *b;
        int r1, r2;
        while (p < end && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (p >= end) break;
        a = p;
        while (p < end && !isspace((unsigned char)*p) && *p != ',' && *p != '-')
            p++;
        r1 = tasm_gpr(a, (int)(p - a));
        if (r1 < 0) return 0;
        while (p < end && isspace((unsigned char)*p)) p++;
        r2 = r1;
        if (p < end && *p == '-') {
            p++;
            while (p < end && isspace((unsigned char)*p)) p++;
            b = p;
            while (p < end && !isspace((unsigned char)*p) && *p != ',') p++;
            r2 = tasm_gpr(b, (int)(p - b));
            if (r2 < r1) return 0;
        }
        for (int r = r1; r <= r2; r++) *mask |= 1u << r;
    }
    return *mask != 0;
}

/* `s7` -> 7, single-precision registers only. */
static int sreg(const char *s, int len)
{
    int v = 0;
    if (len < 2 || len > 3 || (s[0] != 's' && s[0] != 'S')) return -1;
    for (int i = 1; i < len; i++) {
        if (!isdigit((unsigned char)s[i])) return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v <= 31 ? v : -1;
}

/* `{s16-s31}` or `{s0, s1, s2}` -- consecutive single-precision registers,
 * which is all VLDM/VSTM/VPUSH/VPOP can name -- as its first and count. */
static int tok_sreglist(const struct tok *t, int *first, int *count)
{
    const char *p = t->s, *end = t->s + t->len;
    int next = -1;
    *first = -1; *count = 0;
    if (t->len < 3 || p[0] != '{' || end[-1] != '}') return 0;
    p++; end--;
    while (p < end) {
        const char *a;
        int r1, r2;
        while (p < end && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (p >= end) break;
        a = p;
        while (p < end && !isspace((unsigned char)*p) && *p != ',' && *p != '-')
            p++;
        r1 = sreg(a, (int)(p - a));
        if (r1 < 0) return 0;
        r2 = r1;
        while (p < end && isspace((unsigned char)*p)) p++;
        if (p < end && *p == '-') {
            p++;
            while (p < end && isspace((unsigned char)*p)) p++;
            a = p;
            while (p < end && !isspace((unsigned char)*p) && *p != ',') p++;
            r2 = sreg(a, (int)(p - a));
            if (r2 < r1) return 0;
        }
        if (next >= 0 && r1 != next) return 0;      /* not consecutive */
        if (*first < 0) *first = r1;
        next = r2 + 1;
        *count += r2 - r1 + 1;
    }
    return *count > 0;
}

/* ---- conditions and IT blocks ------------------------------------------
 *
 * An IT block makes up to four following instructions conditional, and
 * unified syntax writes the condition on each of them: `it eq` then
 * `vstmdbeq r0!, {s16-s31}`. The block outlives one statement -- the file
 * assembler hands this layer a statement at a time -- so what it still
 * owes is kept here, reset by tasm_reset() and checked by tasm_open().
 *
 * Inside a block the instruction is encoded as it would be outside, with
 * the suffix removed, because a Thumb instruction's encoding does not
 * carry the condition -- the IT does. That is right for everything this
 * assembler emits EXCEPT a flag-setting form: a 16-bit `adds` inside an
 * IT block is an `add` that sets nothing, so `addseq` would assemble and
 * not set the flags. Those are refused there by name, and so are
 * branches, whose conditional encodings are not allowed inside a block.
 */
static const struct { const char *name; int cond; } conds[] = {
    { "eq", 0 }, { "ne", 1 }, { "cs", 2 }, { "hs", 2 }, { "cc", 3 },
    { "lo", 3 }, { "mi", 4 }, { "pl", 5 }, { "vs", 6 }, { "vc", 7 },
    { "hi", 8 }, { "ls", 9 }, { "ge", 10 }, { "lt", 11 }, { "gt", 12 },
    { "le", 13 }, { "al", 14 }
};

static int cond_num(const char *s, int len)
{
    for (unsigned k = 0; k < sizeof conds / sizeof conds[0]; k++)
        if ((int)strlen(conds[k].name) == len &&
            same_nocase(s, conds[k].name, len))
            return conds[k].cond;
    return -1;
}

static const char *cond_name(int c)
{
    for (unsigned k = 0; k < sizeof conds / sizeof conds[0]; k++)
        if (conds[k].cond == c)
            return conds[k].name;
    return "?";
}

static struct { int cond[4]; int n, at; } g_it;

void tasm_reset(void) { g_it.n = g_it.at = 0; }
int tasm_open(void) { return g_it.at < g_it.n; }

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

    /* ---- an IT block, and the instructions it makes conditional ---- */
    if (t[0].len >= 2 && t[0].len <= 5 && t[0].s[0] == 'i' &&
        t[0].s[1] == 't') {
        int ok = 1, cond;
        char te[4];
        for (int k = 2; k < t[0].len; k++)
            if (t[0].s[k] != 't' && t[0].s[k] != 'e') ok = 0;
        if (ok) {
            if (tasm_open())
                FAIL("an IT block cannot begin inside another");
            if (n != 2 || (cond = cond_num(t[1].s, t[1].len)) < 0)
                FAIL("%.*s wants a condition", t[0].len, t[0].s);
            memcpy(te, t[0].s + 2, (size_t)(t[0].len - 2));
            te[t[0].len - 2] = 0;
            if (cond == 14 && strchr(te, 'e'))
                FAIL("an `al` IT block has no else");
            t_it(out, cond, te);
            g_it.cond[0] = cond;
            for (int k = 0; te[k]; k++)
                g_it.cond[k + 1] = te[k] == 't' ? cond : (cond ^ 1);
            g_it.n = t[0].len - 1;
            g_it.at = 0;
            return 0;
        }
    }
    if (tasm_open()) {
        int want = g_it.cond[g_it.at], mlen = t[0].len, wlen = 0, c, r;
        char buf[256];
        /* the width suffix follows the condition: `addeq.w` */
        if (mlen > 2 && t[0].s[mlen - 2] == '.') { wlen = 2; mlen -= 2; }
        c = mlen > 2 ? cond_num(t[0].s + mlen - 2, 2) : -1;
        if (c != want)
            FAIL("inside an IT block, \"%.*s\" must carry the condition `%s`",
                 t[0].len, t[0].s, cond_name(want));
        mlen -= 2;
        {
            static const char *const refused[] = {
                "adds", "subs", "ands", "orrs", "eors", "bics", "adcs",
                "sbcs", "rsbs", "lsls", "lsrs", "asrs", "rors", "movs",
                "mvns", "muls", "b", "bl", "cbz", "cbnz", NULL
            };
            for (int k = 0; refused[k]; k++)
                if ((int)strlen(refused[k]) == mlen &&
                    strncmp(t[0].s, refused[k], (size_t)mlen) == 0)
                    FAIL("\"%.*s\" is not supported inside an IT block: "
                         "%s", t[0].len, t[0].s,
                         refused[k][0] == 'b' || refused[k][0] == 'c'
                         ? "a branch there needs the unconditional encoding"
                         : "its 16-bit encoding sets no flags there");
            if (g_it.at + 1 < g_it.n &&
                ((mlen == 2 && strncmp(t[0].s, "bx", 2) == 0) ||
                 (mlen == 3 && strncmp(t[0].s, "blx", 3) == 0)))
                FAIL("a branch must be the last instruction of its IT block");
        }
        snprintf(buf, sizeof buf, "%.*s%.*s%.*s", mlen, t[0].s, wlen,
                 t[0].s + t[0].len - wlen,
                 (int)(stmt + len - (t[0].s + t[0].len)), t[0].s + t[0].len);
        g_it.at++;
        {
            int save_n = g_it.n, save_at = g_it.at;
            g_it.n = g_it.at = 0;       /* the inner call is outside it */
            r = one_stmt(buf, (int)strlen(buf), out, err, errlen);
            g_it.n = save_n;
            g_it.at = save_at;
        }
        return r;
    }

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

    if (mnemonic_is(&t[0], "svc") && n == 2) {
        if (!tok_imm(&t[1], &imm) || imm < 0 || imm > 255)
            FAIL("svc takes a 0..255 immediate");
        t_svc(out, (int)imm);
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
        /* `movs` and `mvns` SET the flags, which a branch after them in
         * the same template reads; they were encoded as movw/mov and
         * mvn.w, which do not. */
        if (mnemonic_is(&t[0], "movs")) {
            if (rd < 0) FAIL("movs wants a register destination");
            if (rm >= 0) { t_movs_reg(out, rd, rm); return 0; }
            if (!tok_imm(&t[2], &imm))
                FAIL("\"%.*s\" is neither a register nor an immediate",
                     t[2].len, t[2].s);
            if (t_movs_imm(out, rd, imm) < 0)
                FAIL("movs cannot set the flags with #%ld: no flag-setting "
                     "MOV encodes it (use mov, then cmp)", imm);
            return 0;
        }
        if (mnemonic_is(&t[0], "mov")) {
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
            t_mvn_reg(out, rd, rm, mnemonic_is(&t[0], "mvns"));
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
            if (rd < 0) FAIL("tst wants a register");
            if (rm >= 0) { t_tst_reg(out, rd, rm); return 0; }
            if (!tok_imm(&t[2], &imm))
                FAIL("tst wants a register or an immediate");
            if (!t_tst_imm(out, rd, imm))
                FAIL("tst cannot encode the immediate %ld", imm);
            return 0;
        }
        /* The exclusive load, and the ordinary ones. */
        for (const struct ls_ent *e = ls_tab; e->name; e++) {
            int base;
            long off;
            if (!mnemonic_is(&t[0], e->name))
                continue;
            if (rd < 0) FAIL("\"%.*s\" is not a register", t[1].len, t[1].s);
            /* `ldr rd, =value`: GNU as loads it from a literal pool; here
             * it is movw/movt, the same eight bytes for every value, so
             * a statement's length never depends on what it loads. A
             * SYMBOL there is the file assembler's to relocate. */
            if (t[2].len > 1 && t[2].s[0] == '=') {
                struct tok v = { t[2].s + 1, t[2].len - 1 };
                if (e->size != 4 || e->store || !tok_imm(&v, &imm))
                    FAIL("\"%.*s %.*s\" wants `ldr rd, =constant`",
                         t[0].len, t[0].s, t[2].len, t[2].s);
                t_movw_movt(out, rd, (unsigned)imm & 0xffffu, 0);
                t_movw_movt(out, rd, ((unsigned long)imm >> 16) & 0xffffu, 1);
                return 0;
            }
            if (!tok_mem(&t[2], &base, &off))
                FAIL("\"%.*s\" is not a [reg] or [reg, #off] address",
                     t[2].len, t[2].s);
            /* [pc, #off]: a literal, from Align(pc, 4) as the
             * architecture reads it. Words only. */
            if (base == 15) {
                if (e->size != 4 || e->store)
                    FAIL("only ldr reads a pc-relative literal");
                if (!t_ldr_lit(out, rd, off))
                    FAIL("literal offset %ld is out of reach", off);
                return 0;
            }
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

    /* ---- control flow and the rest of what a .S file needs ---------
     *
     * Added for the file assembler (src/as/gas.c), which resolves a
     * label into a PC-relative displacement before calling -- this
     * layer never sees a name. The displacement is written `.+N` as
     * it is for RISC-V, and a bare number means the same thing.
     *
     * Every branch here is the WIDE encoding. A .S file's branch
     * could often be the narrow one, but choosing per displacement
     * would make an instruction's length depend on a value the first
     * pass does not have yet -- which is what relaxation is for, and
     * this assembler does not relax. Four bytes always is correct and
     * is a size question, not a correctness one.
     */
    if (n == 1) {
        if (mnemonic_is(&t[0], "nop")) { t_nop(out); return 0; }
    }
    {
        static const struct { const char *name; int cond; } bc[] = {
            { "beq", T_EQ }, { "bne", T_NE }, { "bcs", T_CS },
            { "bhs", T_CS }, { "bcc", T_CC }, { "blo", T_CC },
            { "bmi", T_MI }, { "bpl", T_PL }, { "bvs", T_VS },
            { "bvc", T_VC }, { "bhi", T_HI }, { "bls", T_LS },
            { "bge", T_GE }, { "blt", T_LT }, { "bgt", T_GT },
            { "ble", T_LE }
        };
        for (unsigned k = 0; k < sizeof bc / sizeof bc[0]; k++)
            if (mnemonic_is(&t[0], bc[k].name)) {
                long v;
                int at;
                if (n != 2 || !tok_imm(&t[1], &v))
                    FAIL("%s wants a branch offset", bc[k].name);
                if (v & 1) FAIL("%s offset %ld is odd", bc[k].name, v);
                at = t_bcond(out, bc[k].cond);
                /* t_patch_bcond takes a TARGET offset within the code
                 * buffer; `at + v` is where the displacement points. */
                t_patch_bcond(out, at, at + (int)v);
                return 0;
            }
    }
    if (mnemonic_is(&t[0], "b") && n == 2) {
        long v;
        int at;
        if (!tok_imm(&t[1], &v)) FAIL("b wants a branch offset");
        if (v & 1) FAIL("b offset %ld is odd", v);
        at = t_b(out);
        t_patch_b(out, at, at + (int)v);
        return 0;
    }
    if (mnemonic_is(&t[0], "bl") && n == 2) {
        long v;
        int at;
        if (!tok_imm(&t[1], &v)) FAIL("bl wants a branch offset");
        if (v & 1) FAIL("bl offset %ld is odd", v);
        at = t_bl(out);
        t_patch_bl(out, at, at + (int)v);
        return 0;
    }
    if ((mnemonic_is(&t[0], "bx") || mnemonic_is(&t[0], "blx")) && n == 2) {
        int r = tok_reg(&t[1]);
        if (r < 0) FAIL("%.*s wants a register", t[0].len, t[0].s);
        if (mnemonic_is(&t[0], "bx")) t_bx(out, r);
        else                          t_blx(out, r);
        return 0;
    }
    /* vmov between a core register and a single-precision one. */
    if (mnemonic_is(&t[0], "vmov") && n == 3) {
        int r1 = tok_reg(&t[1]), r2 = tok_reg(&t[2]);
        int s1 = sreg(t[1].s, t[1].len), s2 = sreg(t[2].s, t[2].len);
        if (s1 >= 0 && r2 >= 0 && r2 < 13) { t_vmov_core(out, s1, r2, 1); return 0; }
        if (r1 >= 0 && r1 < 13 && s2 >= 0) { t_vmov_core(out, s2, r1, 0); return 0; }
        FAIL("vmov takes a core register and a single-precision register");
    }
    if ((mnemonic_is(&t[0], "push") || mnemonic_is(&t[0], "pop")) && n >= 2) {
        /* `{r4-r7, lr}` is one token: the splitter keeps a braced group
         * together, and ranges count. The braces were once refused and a
         * bare `push r4, lr` was the documented spelling, so it stays. */
        unsigned mask = 0;
        int push = mnemonic_is(&t[0], "push");
        if (t[1].s[0] == '{') {
            if (n != 2 || !tok_reglist(&t[1], &mask))
                FAIL("%.*s wants a register list", t[0].len, t[0].s);
        } else {
            for (int k = 1; k < n; k++) {
                int r = tok_reg(&t[k]);
                if (r < 0)
                    FAIL("%.*s wants a register list", t[0].len, t[0].s);
                mask |= 1u << r;
            }
        }
        if ((mask & (1u << 13)) || (push && (mask & (1u << 15))) ||
            (!push && (mask & (1u << 14)) && (mask & (1u << 15))))
            FAIL("%.*s cannot transfer that register list",
                 t[0].len, t[0].s);
        if (push) (void)t_push(out, mask);
        else      (void)t_pop(out, mask);
        return 0;
    }
    /* The multiple loads and stores an RTOS saves a context with:
     * `stmdb r0!, {r4-r11, lr}` onto a task's own stack. */
    {
        static const struct { const char *name; int load, before; } lm[] = {
            { "ldm", 1, 0 }, { "ldmia", 1, 0 }, { "ldmfd", 1, 0 },
            { "ldmdb", 1, 1 }, { "ldmea", 1, 1 },
            { "stm", 0, 0 }, { "stmia", 0, 0 }, { "stmea", 0, 0 },
            { "stmdb", 0, 1 }, { "stmfd", 0, 1 }
        };
        for (unsigned k = 0; k < sizeof lm / sizeof lm[0]; k++) {
            unsigned mask;
            int rn, wb;
            if (!mnemonic_is(&t[0], lm[k].name))
                continue;
            if (n != 3) FAIL("%s wants a base register and a list", lm[k].name);
            wb = t[1].len > 1 && t[1].s[t[1].len - 1] == '!';
            rn = tasm_gpr(t[1].s, t[1].len - wb);
            if (rn < 0 || !tok_reglist(&t[2], &mask))
                FAIL("%s wants a base register and a list", lm[k].name);
            if (!t_ldm_stm(out, rn, mask, wb, lm[k].before, lm[k].load))
                FAIL("%s cannot transfer that list: two or more registers, "
                     "never sp, no pc in a store, not pc and lr together in "
                     "a load, and not the base when it is written back",
                     lm[k].name);
            return 0;
        }
    }
    /* ...and the floating-point half of a context, on a part with an FPU:
     * `vstmdb r0!, {s16-s31}`. Single-precision registers only. */
    {
        static const struct { const char *name; int load, before; } vm[] = {
            { "vldm", 1, 0 }, { "vldmia", 1, 0 }, { "vldmdb", 1, 1 },
            { "vstm", 0, 0 }, { "vstmia", 0, 0 }, { "vstmdb", 0, 1 }
        };
        int first, count;
        if ((mnemonic_is(&t[0], "vpush") || mnemonic_is(&t[0], "vpop")) &&
            n == 2) {
            if (!tok_sreglist(&t[1], &first, &count))
                FAIL("%.*s wants consecutive single-precision registers, "
                     "{s16-s31}", t[0].len, t[0].s);
            t_vpush_s(out, first, count, mnemonic_is(&t[0], "vpop"));
            return 0;
        }
        for (unsigned k = 0; k < sizeof vm / sizeof vm[0]; k++) {
            int rn, wb;
            if (!mnemonic_is(&t[0], vm[k].name))
                continue;
            if (n != 3) FAIL("%s wants a base register and a list", vm[k].name);
            wb = t[1].len > 1 && t[1].s[t[1].len - 1] == '!';
            rn = tasm_gpr(t[1].s, t[1].len - wb);
            if (rn < 0 || !tok_sreglist(&t[2], &first, &count))
                FAIL("%s wants a base register and consecutive "
                     "single-precision registers", vm[k].name);
            if (!t_vldm_vstm(out, rn, first, count, wb, vm[k].before,
                             vm[k].load))
                FAIL("%s cannot transfer that list%s", vm[k].name,
                     vm[k].before && !wb ? ": DB needs writeback (rn!)" : "");
            return 0;
        }
    }
    if (mnemonic_is(&t[0], "mov") && n == 3) {
        int rd = tok_reg(&t[1]), rm = tok_reg(&t[2]);
        long v;
        if (rd < 0) FAIL("mov wants a destination register");
        if (rm >= 0) { t_mov_reg(out, rd, rm); return 0; }
        if (!tok_imm(&t[2], &v)) FAIL("mov wants a register or a constant");
        t_mov_imm(out, rd, v, 0);
        return 0;
    }
    if ((mnemonic_is(&t[0], "movw") || mnemonic_is(&t[0], "movt")) && n == 3) {
        int rd = tok_reg(&t[1]);
        long v;
        if (rd < 0 || !tok_imm(&t[2], &v))
            FAIL("%.*s wants a register and a 16-bit constant",
                 t[0].len, t[0].s);
        if (v < 0 || v > 0xffff)
            FAIL("%.*s constant %ld does not fit 16 bits",
                 t[0].len, t[0].s, v);
        t_movw_movt(out, rd, (unsigned)v, mnemonic_is(&t[0], "movt"));
        return 0;
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

/* ---- what the file assembler must not take for a symbol ----------------
 *
 * src/as/gas.c resolves every identifier in an operand as a symbol unless
 * the target says otherwise, and these are not symbols: the special
 * registers after mrs/msr, the masks after cpsid/cpsie, the condition
 * after it, the barrier option, and the floating-point registers. Asked
 * per statement, so a label that happens to be called `i` is still a
 * label in any other instruction. */
int tasm_is_word(const char *stmt, const char *w, int wlen)
{
    const char *m = stmt;
    int mlen;
    while (isspace((unsigned char)*m)) m++;
    for (mlen = 0; m[mlen] && !isspace((unsigned char)m[mlen]); mlen++) {}
    if (sreg(w, wlen) >= 0 || (wlen == 5 && same_nocase(w, "fpscr", 5)))
        return 1;
    if ((mlen >= 3 && (strncmp(m, "mrs", 3) == 0 || strncmp(m, "msr", 3) == 0))
        && sysreg_num(w, wlen) >= 0)
        return 1;
    if (mlen >= 5 && strncmp(m, "cps", 3) == 0) {
        int ok = wlen > 0 && wlen <= 2;
        for (int k = 0; k < wlen; k++)
            if (w[k] != 'i' && w[k] != 'f') ok = 0;
        if (ok) return 1;
    }
    if (mlen >= 2 && mlen <= 5 && m[0] == 'i' && m[1] == 't' &&
        cond_num(w, wlen) >= 0)
        return 1;
    if (mlen == 3 && (strncmp(m, "dsb", 3) == 0 || strncmp(m, "dmb", 3) == 0 ||
                      strncmp(m, "isb", 3) == 0) &&
        wlen == 2 && strncmp(w, "sy", 2) == 0)
        return 1;
    return 0;
}

/* The forms whose operand is a SYMBOL, rewritten with a zero in its place
 * and the relocations that fill it:
 *   ldr rd, =sym[+n]          movw rd, #0; movt rd, #0  (MOVW_ABS_NC, MOVT_ABS)
 *   movw rd, #:lower16:sym    movw rd, #0               (MOVW_ABS_NC)
 *   movt rd, #:upper16:sym    movt rd, #0               (MOVT_ABS)
 * `ldr rd, =sym` is GNU as's literal-pool load; movw/movt reaches the same
 * value in the same register on every ARMv7-M part, in eight bytes. */
static int sym_operand(const char *p, long *addend, const char **end)
{
    int n = 0;
    *addend = 0;
    if (!(isalpha((unsigned char)p[0]) || p[0] == '_' || p[0] == '.'))
        return 0;
    while (isalnum((unsigned char)p[n]) || p[n] == '_' || p[n] == '.' ||
           p[n] == '$')
        n++;
    *end = p + n;
    while (isspace((unsigned char)**end)) (*end)++;
    if (**end == '+' || **end == '-') {
        int neg = **end == '-';
        char *q;
        long v = strtol(*end + 1, &q, 0);
        if (q == *end + 1) return 0;
        *addend = neg ? -v : v;
        *end = q;
        while (isspace((unsigned char)**end)) (*end)++;
    }
    return n;
}

int tasm_symform(const char *stmt, struct asm_symform *f)
{
    const char *p = stmt, *mn, *rd, *q, *end;
    int mnlen, rdlen, n;
    memset(f, 0, sizeof *f);
    while (isspace((unsigned char)*p)) p++;
    mn = p;
    for (mnlen = 0; isalnum((unsigned char)mn[mnlen]) || mn[mnlen] == '.';
         mnlen++) {}
    q = mn + mnlen;
    while (isspace((unsigned char)*q)) q++;
    rd = q;
    for (rdlen = 0; isalnum((unsigned char)rd[rdlen]); rdlen++) {}
    if (!rdlen || tasm_gpr(rd, rdlen) < 0) return 0;
    q = rd + rdlen;
    while (isspace((unsigned char)*q)) q++;
    if (*q != ',') return 0;
    q++;
    while (isspace((unsigned char)*q)) q++;
    if ((mnlen == 3 || (mnlen == 5 && mn[3] == '.')) &&
        strncmp(mn, "ldr", 3) == 0 && *q == '=') {
        q++;
        while (isspace((unsigned char)*q)) q++;
        if (!(n = sym_operand(q, &f->addend, &end)) || *end) return 0;
        f->sym_at = (int)(q - stmt); f->sym_len = n;
        snprintf(f->encode, sizeof f->encode, "movw %.*s, #0; movt %.*s, #0",
                 rdlen, rd, rdlen, rd);
        f->site[0].off = 0; f->site[0].reloc = R_ARM_THM_MOVW_ABS_NC;
        f->site[1].off = 4; f->site[1].reloc = R_ARM_THM_MOVT_ABS;
        f->nsites = 2;
        return 1;
    }
    if (mnlen == 4 && (strncmp(mn, "movw", 4) == 0 || strncmp(mn, "movt", 4) == 0)) {
        int top = mn[3] == 't';
        const char *want = top ? "#:upper16:" : "#:lower16:";
        if (strncmp(q, want, 10) != 0) return 0;
        q += 10;
        if (!(n = sym_operand(q, &f->addend, &end)) || *end) return 0;
        f->sym_at = (int)(q - stmt); f->sym_len = n;
        snprintf(f->encode, sizeof f->encode, "%.4s %.*s, #0", mn, rdlen, rd);
        f->site[0].off = 0;
        f->site[0].reloc = top ? R_ARM_THM_MOVT_ABS : R_ARM_THM_MOVW_ABS_NC;
        f->nsites = 1;
        return 1;
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
    /* What an RTOS's context switch is made of. The multiple transfers
     * name high registers or the forms with no 16-bit encoding, which is
     * the only case where llvm-mc and this assembler (always 32-bit) pick
     * the same bytes. `ldr rd, =value` is left out for the reason `mov
     * rd, #imm` is: llvm-mc loads it from a literal pool, this assembler
     * with movw/movt, and both are right. */
    fprintf(f, "\tsvc #0\n\tsvc #171\n\ttst lr, #16\n\ttst r3, #0xff00\n");
    fprintf(f, "\tpush {r4-r7, lr}\n\tpop {r4-r7, pc}\n");
    fprintf(f, "\tpush {r4-r11, lr}\n\tpop {r4-r11, pc}\n");
    fprintf(f, "\tstmdb r0!, {r4-r11, lr}\n\tldmia r0!, {r4-r11, lr}\n");
    fprintf(f, "\tstmfd r1!, {r2, r3}\n\tldmfd r9!, {r2, r3}\n");
    fprintf(f, "\tstmia r2, {r8-r11}\n\tstm r6, {r8, r9}\n");
    fprintf(f, "\tldmdb r5, {r8, pc}\n\tldm r7, {r0, r10}\n");
    fprintf(f, "\tvstmdb r0!, {s16-s31}\n\tvldmia r0!, {s16-s31}\n");
    fprintf(f, "\tvstmia r3, {s0-s7}\n\tvldmdb r9!, {s1-s3}\n");
    fprintf(f, "\tvpush {s16-s31}\n\tvpop {s16-s31}\n");
    fprintf(f, "\tldr r8, [pc, #8]\n\tldr r3, [pc, #-12]\n");
    fprintf(f, "\tvmov s20, r1\n\tvmov r2, s3\n\tvmov s31, r12\n");
    /* IT blocks, each instruction under its own condition: */
    fprintf(f, "\tit eq\n\tmoveq r0, r1\n");
    fprintf(f, "\tite ne\n\tmovne r2, r3\n\tmoveq r2, r4\n");
    fprintf(f, "\titte gt\n\tmovgt r0, r9\n\tmovgt r1, r9\n"
               "\tmovle r0, r8\n");
    fprintf(f, "\tit eq\n\tvstmdbeq r0!, {s16-s31}\n");
    fprintf(f, "\tit ne\n\tbxne lr\n");
}
