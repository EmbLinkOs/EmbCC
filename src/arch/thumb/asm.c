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
#include "a32.h"

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

static int same_nocase(const char *a, const char *b, int len);

int tasm_gpr(const char *name, int len)
{
    if (len <= 0)
        return -1;
    /* (in either case, as GNU as reads them: `R0`, `SP`, `LR`) */
    for (int i = 0; i < 16; i++)
        if ((int)strlen(reg_name[i]) == len &&
            same_nocase(name, reg_name[i], len))
            return i;
    /* The alternate spellings hand-written asm uses. */
    if (len == 2 && same_nocase(name, "ip", 2)) return 12;
    if (len == 2 && same_nocase(name, "fp", 2)) return 11;
    if (len == 3 && same_nocase(name, "r13", 3)) return 13;
    if (len == 3 && same_nocase(name, "r14", 3)) return 14;
    if (len == 3 && same_nocase(name, "r15", 3)) return 15;
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

/* ARMv8-M's: the stack limits, and with the Security Extension the
 * Non-secure copies a Secure handler reaches across (SYSm bit 7). An
 * ARMv7-M part has none of them, so they are names only there. */
static const struct sysreg sysregs_v8m[] = {
    { "msplim", 10 }, { "psplim", 11 },
    { "msp_ns", 0x88 }, { "psp_ns", 0x89 },
    { "msplim_ns", 0x8a }, { "psplim_ns", 0x8b },
    { "primask_ns", 0x90 }, { "basepri_ns", 0x91 },
    { "faultmask_ns", 0x93 }, { "control_ns", 0x94 }, { "sp_ns", 0x98 },
    { NULL, 0 }
};

/* The architecture level the statements are for: 6 (ARMv6-M), 7, 8
 * (ARMv8-M Mainline) or TASM_V8M_BASE (ARMv8-M Baseline). Told by the
 * caller (the file assembler, inline asm in irgen) rather than asked of
 * the target, so the encoder links on its own (tools/tasmcheck). */
static int g_arch = 7;
void tasm_set_arch(int level) { g_arch = level; }

/* The DSP extension: ARMv7E-M, and ARMv8-M Mainline with it. Told by the
 * caller as the level is; off unless it says so, so a caller that forgets
 * refuses sadd16 rather than assembling it for a Cortex-M3. */
static int g_dsp;
void tasm_set_dsp(int on) { g_dsp = on ? 1 : 0; }
static int dsp_ok(void) { return g_dsp && (g_arch == 7 || g_arch == 8); }

/* Either ARMv8-M profile: the security extension, the acquire/release
 * forms and the stack-limit registers. */
static int arch_v8m(void) { return g_arch == 8 || g_arch == TASM_V8M_BASE; }
/* A Thumb-1 core (ARMv6-M, ARMv8-M Baseline): every 32-bit encoding a
 * statement produces is checked against what the core has. */
static int arch_thumb1(void) { return g_arch == 6 || g_arch == TASM_V8M_BASE; }

/* Other spellings of one in the tables: `apsr_nzcvq` is what an msr that
 * writes the flags is written as on a Mainline part, and the same
 * encoding (mask 0b10) as `apsr`. Not in the vocabulary listing, since
 * llvm-mc reads it only after msr. */
static const struct sysreg sysreg_alias[] = {
    { "apsr_nzcvq", T_SYS_APSR }, { NULL, 0 }
};

/* The Main Extension's: no ARMv6-M or ARMv8-M Baseline core has them. */
static int sysreg_main_only(int sysm)
{
    return sysm == T_SYS_BASEPRI || sysm == T_SYS_BASEPRI_MAX ||
           sysm == T_SYS_FAULTMASK || sysm == 0x91 || sysm == 0x93;
}

static int sysreg_num(const char *s, int len)
{
    for (const struct sysreg *r = sysregs; r->name; r++)
        if ((int)strlen(r->name) == len && same_nocase(s, r->name, len))
            return r->sysm;
    for (const struct sysreg *r = sysreg_alias; r->name; r++)
        if ((int)strlen(r->name) == len && same_nocase(s, r->name, len))
            return r->sysm;
    if (arch_v8m())
        for (const struct sysreg *r = sysregs_v8m; r->name; r++)
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
            /* An immediate runs to the next comma, spaces and all, as in
             * GNU as: `#(1 << 2) - 1` and `#0x3f0 >> 4` are one operand. */
            int imm = stmt[i] == '#';
            while (i < len && (depth > 0 ||
                   ((imm || !isspace((unsigned char)stmt[i])) &&
                    stmt[i] != ','))) {
                /* (and an immediate's expression, which may have spaces:
                 * FreeRTOS's ARM_CM4F port writes `#( 0xf << 20 )`) */
                if (stmt[i] == '[' || stmt[i] == '{' || stmt[i] == '(')
                    depth++;
                else if (stmt[i] == ']' || stmt[i] == '}' || stmt[i] == ')')
                    depth--;
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
    return (int)strlen(s) == t->len && same_nocase(t->s, s, t->len);
}

static int tok_reg(const struct tok *t) { return tasm_gpr(t->s, t->len); }

#include "../asmexpr.h"

/* `#imm`, or a bare number, or `#(` a constant expression `)`. */
static int tok_imm(const struct tok *t, long *out)
{
    int i = 0, neg = 0, base = 10, any = 0;
    long v = 0;
    if (i < t->len && t->s[i] == '#')
        i++;
    /* An expression: anything past a leading sign that is not a digit
     * of one number (`.+8`, a displacement, is the plain path below). */
    int ex = 0;
    for (int j = i; j < t->len && t->s[i] != '.'; j++)
        if (strchr("()~<>|&^*/% ", t->s[j]) ||
            (j > i && (t->s[j] == '+' || t->s[j] == '-')))
            ex = 1;
    if (ex) {
        long long ev;
        if (!asm_const_expr(t->s + i, t->len - i, &ev))
            return 0;
        *out = (long)ev;
        return 1;
    }
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

/* `[rn, rm]` or `[rn, rm, lsl #k]`, k 0..3: a register offset. */
static int tok_mem_reg(const struct tok *t, int *rn, int *rm, int *shift)
{
    struct tok inner[4];
    int n;
    long k = 0;
    if (t->len < 3 || t->s[0] != '[' || t->s[t->len - 1] != ']')
        return 0;
    n = split(t->s + 1, t->len - 2, inner, 4);
    if (n < 2 || n > 4)
        return 0;
    *rn = tok_reg(&inner[0]);
    *rm = tok_reg(&inner[1]);
    if (*rn < 0 || *rm < 0)
        return 0;
    if (n >= 3) {
        /* `lsl #k`, as one token or as two */
        struct tok sh = inner[2], amt;
        if (sh.len < 3 || strncmp(sh.s, "lsl", 3))
            return 0;
        if (n == 4) {
            if (sh.len != 3)
                return 0;
            amt = inner[3];
        } else {
            if (sh.len < 5 || !isspace((unsigned char)sh.s[3]))
                return 0;
            amt.s = sh.s + 4;
            amt.len = sh.len - 4;
            while (amt.len && isspace((unsigned char)*amt.s)) { amt.s++; amt.len--; }
        }
        if (!tok_imm(&amt, &k) || k < 0 || k > 3)
            return 0;
    }
    *shift = (int)k;
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

/* Branch relaxation, driven by the file assembler (src/as/gas.c): a
 * branch is assembled narrow when its displacement fits, unless the file
 * assembler has said this statement must be wide; and it reports when a
 * branch went wide on its own, so that statement stays wide in every
 * later pass and the layout settles. Inline asm never sets either. */
static int g_force_wide = 1, g_went_wide;   /* wide unless gas.c relaxes */
void tasm_set_wide(int wide) { g_force_wide = wide; }
int tasm_took_wide(void) { int w = g_went_wide; g_went_wide = 0; return w; }

/* A branch to `at + v`: narrow if allowed and it fits, else wide. `cond`
 * is -1 for an unconditional one. `want` is 'w' or 'n' for an explicit
 * `.w`/`.n` suffix, else 0. */
static int t_branch_relaxed(struct code *out, int cond, long v, int want,
                            char *err, int errlen)
{
    int at;
    if (want != 'w' && !g_force_wide) {
        struct code probe = { 0 };
        int ok;
        at = cond < 0 ? t_b16(&probe) : t_bcond16(&probe, cond);
        ok = cond < 0 ? t_patch_b16(&probe, at, at + (int)v)
                      : t_patch_bcond16(&probe, at, at + (int)v);
        if (ok) {
            for (int k = 0; k < probe.len; k++)
                code_byte(out, probe.p[k]);
            free(probe.p);
            return 0;
        }
        free(probe.p);
        /* ARMv6-M (and v8-M Baseline's conditional branch) has only the
         * 16-bit form: nothing to widen to */
        if (want == 'n' || (arch_thumb1() && (cond >= 0 || g_arch == 6))) {
            snprintf(err, (size_t)errlen, "the branch is %ld bytes away, "
                     "beyond the %s form's reach (%s)", v,
                     want == 'n' ? "narrow" : "only",
                     cond >= 0 ? "-256..254" : "-2048..2046");
            return -1;
        }
    }
    if (want != 'w' && !g_force_wide)
        g_went_wide = 1;
    if (cond < 0) {
        at = t_b(out);
        t_patch_b(out, at, at + (int)v);
    } else {
        at = t_bcond(out, cond);
        t_patch_bcond(out, at, at + (int)v);
    }
    return 0;
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

/* `c<n>`, a coprocessor register name, or `p<n>`, a coprocessor. */
static int tok_cnum(const struct tok *t, char letter)
{
    int v = 0;
    if (t->len < 2 || t->len > 3 || tolower((unsigned char)t->s[0]) != letter)
        return -1;
    for (int k = 1; k < t->len; k++) {
        if (!isdigit((unsigned char)t->s[k]))
            return -1;
        v = v * 10 + (t->s[k] - '0');
    }
    return v <= 15 ? v : -1;
}

/* The status register as msr names it: `cpsr_` (or `spsr_`, refused)
 * with any of c x s f, `apsr_nzcvq` and `apsr_nzcvqg`, `cpsr` alone
 * (c and f, as GNU as reads it). The field mask, bit 0 c ... bit 3 f, or
 * -1. */
static int psr_fields(const struct tok *t)
{
    int m = 0;
    if (tok_is(t, "cpsr") || tok_is(t, "apsr"))
        return 9;
    if (tok_is(t, "apsr_nzcvq"))
        return 8;
    if (tok_is(t, "apsr_nzcvqg") || tok_is(t, "apsr_g"))
        return tok_is(t, "apsr_g") ? 4 : 12;
    if (t->len < 6 || !same_nocase(t->s, "cpsr_", 5))
        return -1;
    for (int k = 5; k < t->len; k++) {
        const char *f = strchr("cxsf", tolower((unsigned char)t->s[k]));
        if (!f || !t->s[k])
            return -1;
        m |= 1 << (f - "cxsf");
    }
    return m;
}

/* ARM state's own forms. 1 when the statement is not one of them (the
 * shared vocabulary takes it), 0 when assembled, -1 with err set. */
static int a32_one_stmt(struct tok *t, int n, struct code *out, char *err,
                        int errlen)
{
    long imm;
    if (mnemonic_is(&t[0], "mrs")) {
        int rd = n == 3 ? tok_reg(&t[1]) : -1;
        if (rd < 0 || rd == 15 || !(tok_is(&t[2], "cpsr") ||
                                    tok_is(&t[2], "apsr")))
            FAIL("mrs takes a register and cpsr (or apsr) in ARM state%s",
                 n == 3 && sysreg_num(t[2].s, t[2].len) >= 0
                 ? "; the M-profile special registers do not exist here" : "");
        a32_mrs_cpsr(out, rd);
        return 0;
    }
    if (mnemonic_is(&t[0], "msr")) {
        int f = n == 3 ? psr_fields(&t[1]) : -1;
        int rn = n == 3 ? tok_reg(&t[2]) : -1;
        if (f <= 0 || rn < 0 || rn == 15)
            FAIL("msr takes cpsr_<fields> (c, x, s, f) or apsr_nzcvq and a "
                 "register in ARM state%s",
                 n == 3 && sysreg_num(t[1].s, t[1].len) >= 0
                 ? "; the M-profile special registers do not exist here" : "");
        a32_msr_cpsr(out, f, rn);
        return 0;
    }
    if (mnemonic_is(&t[0], "mrc") || mnemonic_is(&t[0], "mcr")) {
        long opc1, opc2 = 0;
        int cp = n >= 6 ? tok_cnum(&t[1], 'p') : -1;
        int rt = n >= 6 ? tok_reg(&t[3]) : -1;
        int crn = n >= 6 ? tok_cnum(&t[4], 'c') : -1;
        int crm = n >= 6 ? tok_cnum(&t[5], 'c') : -1;
        if (n < 6 || n > 7 || cp < 0 || rt < 0 || rt == 13 || rt == 15 ||
            crn < 0 || crm < 0 || !tok_imm(&t[2], &opc1) || opc1 < 0 ||
            opc1 > 7 || (n == 7 && (!tok_imm(&t[6], &opc2) || opc2 < 0 ||
                                    opc2 > 7)))
            FAIL("%.*s takes p<n>, #opc1, a register, c<n>, c<m>[, #opc2]",
                 t[0].len, t[0].s);
        a32_mrc_mcr(out, mnemonic_is(&t[0], "mrc"), cp, (int)opc1, rt, crn,
                    crm, (int)opc2);
        return 0;
    }
    if (mnemonic_is(&t[0], "svc") && n == 2) {
        if (!tok_imm(&t[1], &imm) || imm < 0 || imm > 0xffffff)
            FAIL("svc takes a 0..0xffffff immediate in ARM state");
        a32_svc(out, imm);
        return 0;
    }
    if (mnemonic_is(&t[0], "bkpt") && n == 2) {
        if (!tok_imm(&t[1], &imm) || imm < 0 || imm > 0xffff)
            FAIL("bkpt takes a 0..65535 immediate");
        a32_bkpt(out, (int)imm);
        return 0;
    }
    if (mnemonic_is(&t[0], "udf") && n == 2) {
        if (!tok_imm(&t[1], &imm) || imm < 0 || imm > 0xffff)
            FAIL("udf takes a 0..65535 immediate");
        a32_udf(out, (int)imm);
        return 0;
    }
    if (mnemonic_is(&t[0], "tbb") || mnemonic_is(&t[0], "tbh") ||
        mnemonic_is(&t[0], "cbz") || mnemonic_is(&t[0], "cbnz"))
        FAIL("%.*s is a Thumb instruction; ARM state has none", t[0].len,
             t[0].s);
    return 1;
}

/* ---- the exclusives' byte and halfword forms, and ARMv8-M's own ----
 *
 * ldrexb/ldrexh/strexb/strexh (every Thumb-2 level), clrex; and on ARMv8-M
 * the load-acquire/store-release family, the security extension's tt, sg,
 * bxns and blxns, and Mainline's vlstm/vlldm. One of these on a level that
 * lacks it is refused by name, rather than encoded for a core that would
 * take it as UNDEFINED. 1 when the statement is none of them. */
struct xa_ent { const char *name; int size, kind; };
/* kind: 0 ldrex[bh], 1 strex[bh], 2 lda*, 3 ldaex*, 4 stl*, 5 stlex* */
static const struct xa_ent xa_tab[] = {
    { "ldrexb", 1, 0 }, { "ldrexh", 2, 0 },
    { "strexb", 1, 1 }, { "strexh", 2, 1 },
    { "ldab", 1, 2 }, { "ldah", 2, 2 }, { "lda", 4, 2 },
    { "ldaexb", 1, 3 }, { "ldaexh", 2, 3 }, { "ldaex", 4, 3 },
    { "stlb", 1, 4 }, { "stlh", 2, 4 }, { "stl", 4, 4 },
    { "stlexb", 1, 5 }, { "stlexh", 2, 5 }, { "stlex", 4, 5 },
    { NULL, 0, 0 }
};

static int v8m_stmt(const struct tok *t, int n, struct code *out,
                    char *err, int errlen)
{
    const char *only = NULL;
    if (mnemonic_is(&t[0], "clrex") && n == 1) {
        t_clrex(out);
        return 0;
    }
    for (const struct xa_ent *e = xa_tab; e->name; e++) {
        int st = e->kind == 1 || e->kind == 5, rd = -1, rt, rn;
        long off;
        if (!mnemonic_is(&t[0], e->name))
            continue;
        if (e->kind >= 2 && !arch_v8m())
            FAIL("%s is an ARMv8-M instruction (load-acquire and "
                 "store-release), and this is not an ARMv8-M target",
                 e->name);
        if (n != (st ? 4 : 3))
            FAIL("%s wants %s and a [reg] address", e->name,
                 st ? "two registers" : "a register");
        if (st)
            rd = tok_reg(&t[1]);
        rt = tok_reg(&t[st ? 2 : 1]);
        if (!tok_mem(&t[st ? 3 : 2], &rn, &off) || off != 0)
            FAIL("%s wants a [reg] address with no offset", e->name);
        if (rt < 0 || (st && rd < 0))
            FAIL("%s wants registers", e->name);
        if (rt == 13 || rt == 15 || rn == 15 || rd == 13 || rd == 15)
            FAIL("%s: sp and pc are not its registers", e->name);
        if (st && (rd == rt || rd == rn))
            FAIL("%s: the status register must differ from the others",
                 e->name);
        switch (e->kind) {
        case 0: t_ldrexbh(out, rt, rn, e->size); break;
        case 1: t_strexbh(out, rd, rt, rn, e->size); break;
        case 2: t_lda(out, rt, rn, e->size, 0); break;
        case 3: t_lda(out, rt, rn, e->size, 1); break;
        case 4: t_stl(out, rt, rn, e->size); break;
        default: t_stlex(out, rd, rt, rn, e->size); break;
        }
        return 0;
    }
    if (mnemonic_is(&t[0], "tt") || mnemonic_is(&t[0], "ttt") ||
        mnemonic_is(&t[0], "tta") || mnemonic_is(&t[0], "ttat"))
        only = "tt";
    else if (mnemonic_is(&t[0], "sg") || mnemonic_is(&t[0], "bxns") ||
             mnemonic_is(&t[0], "blxns"))
        only = "sg";
    else if (mnemonic_is(&t[0], "vlstm") || mnemonic_is(&t[0], "vlldm"))
        only = "vlstm";
    if (!only)
        return 1;
    if (!arch_v8m())
        FAIL("%.*s is an instruction of ARMv8-M's security extension, and "
             "this is not an ARMv8-M target", t[0].len, t[0].s);
    if (only[0] == 't') {
        int rd = n == 3 ? tok_reg(&t[1]) : -1, rn = n == 3 ? tok_reg(&t[2]) : -1;
        int alt = mnemonic_is(&t[0], "tta") || mnemonic_is(&t[0], "ttat");
        int unpriv = mnemonic_is(&t[0], "ttt") || mnemonic_is(&t[0], "ttat");
        if (rd < 0 || rn < 0)
            FAIL("%.*s wants two registers", t[0].len, t[0].s);
        if (rd == 13 || rd == 15 || rn == 15)
            FAIL("%.*s: sp and pc are not its registers", t[0].len, t[0].s);
        t_tt(out, rd, rn, alt, unpriv);
        return 0;
    }
    if (mnemonic_is(&t[0], "sg")) {
        if (n != 1)
            FAIL("sg takes no operands");
        t_sg(out);
        return 0;
    }
    if (only[0] == 's') {
        int rm = n == 2 ? tok_reg(&t[1]) : -1;
        if (rm < 0 || rm == 15)
            FAIL("%.*s wants a register", t[0].len, t[0].s);
        t_bxns(out, rm, mnemonic_is(&t[0], "blxns"));
        return 0;
    }
    /* vlstm/vlldm rn{, {d0-d15}}: the register list is the only one
     * there is, and llvm-mc prints it */
    if (g_arch != 8)
        FAIL("%.*s is an ARMv8-M Mainline instruction", t[0].len, t[0].s);
    {
        int rn = n >= 2 ? tok_reg(&t[1]) : -1;
        if (rn < 0 || rn == 15 || (n == 3 && t[2].s[0] != '{') || n > 3)
            FAIL("%.*s wants a register", t[0].len, t[0].s);
        t_vlstm(out, rn, mnemonic_is(&t[0], "vlldm"));
    }
    return 0;
}

/* ---- the DSP extension, the saturates and the extends --------------
 *
 * What CMSIS's __SADD16 ... __SMLALD and ACLE's <arm_acle.h> wrap: the
 * parallel arithmetic, the halfword and dual multiplies, the saturating
 * adds, sel, the packs, and the extends with an add or a rotation -- the
 * DSP extension of ARMv7E-M and of ARMv8-M Mainline with it -- plus
 * ssat/usat and the plain extends, which every Thumb-2 core has and which
 * share the parsing.
 *
 * The parallel ones are a prefix (the arithmetic: s, q, sh, u, uq, uh)
 * and an operation (add16 ... sub8), two tables, so that what each half
 * encodes is said once. The rest are one table whose columns are the
 * encoder's arguments. */
static const struct { const char *name; int kind; } par_kind[] = {
    { "s", T_PAR_S }, { "q", T_PAR_Q }, { "sh", T_PAR_SH },
    { "u", T_PAR_U }, { "uq", T_PAR_UQ }, { "uh", T_PAR_UH }, { NULL, 0 }
};
static const struct { const char *name; int op; } par_op[] = {
    { "add16", T_PAR_ADD16 }, { "asx", T_PAR_ASX }, { "sax", T_PAR_SAX },
    { "sub16", T_PAR_SUB16 }, { "add8", T_PAR_ADD8 }, { "sub8", T_PAR_SUB8 },
    { NULL, 0 }
};

enum { DF_Q, DF_SEL, DF_SMLAXY, DF_SMLAW, DF_SMLAD, DF_SMMLA, DF_USADA8,
       DF_SMLALXY, DF_SMLALD, DF_EXT, DF_EXTA, DF_SAT, DF_SAT16, DF_PKH };
/* a, b: the encoder's own small arguments (see each form below); acc: the
 * accumulating form, with Ra; dsp: the DSP extension's, not Thumb-2's */
struct dsp_ent { const char *name; int form, a, b, acc, dsp; };
static const struct dsp_ent dsp_tab[] = {
    /* qadd rd, rm, rn: a is the op (t_qarith) */
    { "qadd", DF_Q, 0, 0, 0, 1 }, { "qdadd", DF_Q, 1, 0, 0, 1 },
    { "qsub", DF_Q, 2, 0, 0, 1 }, { "qdsub", DF_Q, 3, 0, 0, 1 },
    { "sel", DF_SEL, 0, 0, 0, 1 },
    /* a: rn's top half, b: rm's */
    { "smlabb", DF_SMLAXY, 0, 0, 1, 1 }, { "smlabt", DF_SMLAXY, 0, 1, 1, 1 },
    { "smlatb", DF_SMLAXY, 1, 0, 1, 1 }, { "smlatt", DF_SMLAXY, 1, 1, 1, 1 },
    { "smulbb", DF_SMLAXY, 0, 0, 0, 1 }, { "smulbt", DF_SMLAXY, 0, 1, 0, 1 },
    { "smultb", DF_SMLAXY, 1, 0, 0, 1 }, { "smultt", DF_SMLAXY, 1, 1, 0, 1 },
    { "smlawb", DF_SMLAW, 0, 0, 1, 1 }, { "smlawt", DF_SMLAW, 0, 1, 1, 1 },
    { "smulwb", DF_SMLAW, 0, 0, 0, 1 }, { "smulwt", DF_SMLAW, 0, 1, 0, 1 },
    /* a: subtract, b: exchange rm's halves */
    { "smlad", DF_SMLAD, 0, 0, 1, 1 }, { "smladx", DF_SMLAD, 0, 1, 1, 1 },
    { "smlsd", DF_SMLAD, 1, 0, 1, 1 }, { "smlsdx", DF_SMLAD, 1, 1, 1, 1 },
    { "smuad", DF_SMLAD, 0, 0, 0, 1 }, { "smuadx", DF_SMLAD, 0, 1, 0, 1 },
    { "smusd", DF_SMLAD, 1, 0, 0, 1 }, { "smusdx", DF_SMLAD, 1, 1, 0, 1 },
    /* a: subtract, b: round */
    { "smmla", DF_SMMLA, 0, 0, 1, 1 }, { "smmlar", DF_SMMLA, 0, 1, 1, 1 },
    { "smmls", DF_SMMLA, 1, 0, 1, 1 }, { "smmlsr", DF_SMMLA, 1, 1, 1, 1 },
    { "smmul", DF_SMMLA, 0, 0, 0, 1 }, { "smmulr", DF_SMMLA, 0, 1, 0, 1 },
    { "usada8", DF_USADA8, 0, 0, 1, 1 }, { "usad8", DF_USADA8, 0, 0, 0, 1 },
    /* rdlo, rdhi, rn, rm; a and b as smla<x><y>'s, and as smlad's */
    { "smlalbb", DF_SMLALXY, 0, 0, 1, 1 }, { "smlalbt", DF_SMLALXY, 0, 1, 1, 1 },
    { "smlaltb", DF_SMLALXY, 1, 0, 1, 1 }, { "smlaltt", DF_SMLALXY, 1, 1, 1, 1 },
    { "smlald", DF_SMLALD, 0, 0, 1, 1 }, { "smlaldx", DF_SMLALD, 0, 1, 1, 1 },
    { "smlsld", DF_SMLALD, 1, 0, 1, 1 }, { "smlsldx", DF_SMLALD, 1, 1, 1, 1 },
    /* a: size (1 byte, 2 halfword, 16 two bytes), b: signed */
    { "sxtb16", DF_EXT, 16, 1, 0, 1 }, { "uxtb16", DF_EXT, 16, 0, 0, 1 },
    { "sxtb", DF_EXT, 1, 1, 0, 0 }, { "uxtb", DF_EXT, 1, 0, 0, 0 },
    { "sxth", DF_EXT, 2, 1, 0, 0 }, { "uxth", DF_EXT, 2, 0, 0, 0 },
    { "sxtab16", DF_EXTA, 16, 1, 0, 1 }, { "uxtab16", DF_EXTA, 16, 0, 0, 1 },
    { "sxtab", DF_EXTA, 1, 1, 0, 1 }, { "uxtab", DF_EXTA, 1, 0, 0, 1 },
    { "sxtah", DF_EXTA, 2, 1, 0, 1 }, { "uxtah", DF_EXTA, 2, 0, 0, 1 },
    /* b: signed */
    { "ssat", DF_SAT, 0, 1, 0, 0 }, { "usat", DF_SAT, 0, 0, 0, 0 },
    { "ssat16", DF_SAT16, 0, 1, 0, 1 }, { "usat16", DF_SAT16, 0, 0, 0, 1 },
    /* a: tb */
    { "pkhbt", DF_PKH, 0, 0, 0, 1 }, { "pkhtb", DF_PKH, 1, 0, 0, 1 },
    { NULL, 0, 0, 0, 0, 0 }
};

/* A register these instructions may name: never pc, and in Thumb state
 * never sp either -- UNPREDICTABLE in every one of them (llvm-mc: r0-r12
 * or r14; in ARM state r0-r14). */
static int dsp_reg(const struct tok *t)
{
    int r = tok_reg(t);
    return r == 15 || (r == 13 && !t_isa_a32) ? -2 : r;
}

/* ...and what the messages call the registers dsp_reg refuses */
static const char *dsp_noreg(void) { return t_isa_a32 ? "pc" : "sp or pc"; }

/* A DSP instruction on a Thumb core without the extension. */
static int dsp_refuse(const struct tok *t, char *err, int errlen)
{
    FAIL("\"%.*s\" is an instruction of the DSP extension, which %s "
         "lacks: it is ARMv7E-M's (-mcpu=cortex-m4, cortex-m7, "
         "--target=thumbv7em-none-eabi) and ARMv8-M Mainline's with "
         "the extension (-mcpu=cortex-m33, -march=armv8-m.main+dsp)",
         t[0].len, t[0].s,
         g_arch == 6 ? "ARMv6-M" : g_arch == TASM_V8M_BASE
         ? "ARMv8-M Baseline" : g_arch == 8 ? "this ARMv8-M Mainline "
         "target" : "ARMv7-M");
}

/* An optional trailing `<kw> #amount` at t[at] (and t[at+1]): `ror #8`,
 * `lsl #3`, or CMSIS's `ROR 8` (inline asm substitutes an immediate
 * without the #). 1 with *amt set (0 when absent), 0 when malformed. */
static int tok_shift(const struct tok *t, int n, int at, const char *kw,
                     long *amt)
{
    *amt = 0;
    if (n == at)
        return 1;
    if (n == at + 2 && tok_is(&t[at], kw))
        return tok_imm(&t[at + 1], amt);
    /* `ror #8` kept as one token when the amount is an expression */
    if (n == at + 1 && t[at].len > 4 && same_nocase(t[at].s, kw, 3) &&
        isspace((unsigned char)t[at].s[3])) {
        struct tok v = { t[at].s + 4, t[at].len - 4 };
        while (v.len && isspace((unsigned char)*v.s)) { v.s++; v.len--; }
        return tok_imm(&v, amt);
    }
    return 0;
}

static int dsp_stmt(const struct tok *t, int n, struct code *out,
                    char *err, int errlen)
{
    const struct dsp_ent *e = NULL;
    int par_k = -1, par_o = -1, r[4] = { -1, -1, -1, -1 }, nreg;
    long imm = 0, amt = 0;
    for (int k = 0; par_kind[k].name && par_o < 0; k++) {
        int pl = (int)strlen(par_kind[k].name);
        if (t[0].len <= pl || strncmp(t[0].s, par_kind[k].name, (size_t)pl))
            continue;
        for (int o = 0; par_op[o].name; o++) {
            struct tok rest = { t[0].s + pl, t[0].len - pl };
            if (mnemonic_is(&rest, par_op[o].name)) {
                par_k = k;
                par_o = o;
                break;
            }
        }
    }
    if (par_o < 0) {
        for (e = dsp_tab; e->name; e++)
            if (mnemonic_is(&t[0], e->name))
                break;
        if (!e->name)
            return 1;
    }
    /* (every ARMv7-A part has the extension) */
    if ((par_o >= 0 || e->dsp) && !t_isa_a32 && !dsp_ok())
        return dsp_refuse(t, err, errlen);
    if (par_o >= 0) {
        for (int k = 0; k < 3; k++)
            r[k] = n == 4 ? dsp_reg(&t[k + 1]) : -1;
        if (n != 4 || r[0] < 0 || r[1] < 0 || r[2] < 0)
            FAIL("\"%.*s\" wants three registers, none of them %s",
                 t[0].len, t[0].s, dsp_noreg());
        t_parallel(out, par_op[par_o].op, par_kind[par_k].kind, r[0], r[1],
                   r[2]);
        return 0;
    }

    switch (e->form) {
    case DF_EXT: case DF_EXTA: {
        nreg = e->form == DF_EXTA ? 3 : 2;
        for (int k = 0; k < nreg; k++)
            r[k] = n > nreg ? dsp_reg(&t[k + 1]) : -1;
        if (n <= nreg || r[0] < 0 || r[1] < 0 || (nreg == 3 && r[2] < 0) ||
            !tok_shift(t, n, nreg + 1, "ror", &amt))
            FAIL("%s wants %s registers (not %s) and an optional "
                 "`ror #8|16|24`", e->name, nreg == 3 ? "three" : "two",
                 dsp_noreg());
        if (amt != 0 && amt != 8 && amt != 16 && amt != 24)
            FAIL("%s: the rotation is 8, 16 or 24, not %ld", e->name, amt);
        /* (a Thumb-1 core's 16-bit extend has no rotation field, not even
         * for `ror #0`, as llvm-mc reads it) */
        if (arch_thumb1() && !t_isa_a32 && n > nreg + 1)
            FAIL("%s: %s's extends take no rotation", e->name,
                 g_arch == 6 ? "ARMv6-M" : "ARMv8-M Baseline");
        /* the 16-bit form where there is one, as GNU as and llvm-mc take
         * it: an ARMv6-M core has only that one */
        if (nreg == 2 && e->a != 16 && amt == 0 && r[0] < 8 && r[1] < 8)
            t_ext(out, r[0], r[1], e->a, e->b);
        else
            t_extadd(out, r[0], nreg == 3 ? r[1] : 15, r[nreg - 1], e->a,
                     e->b, (int)amt);
        return 0;
    }
    case DF_SAT: case DF_SAT16: {
        long lo = e->b ? 1 : 0, hi = e->form == DF_SAT ? (e->b ? 32 : 31)
                                                       : (e->b ? 16 : 15);
        int asr = 0;
        r[0] = n >= 4 ? dsp_reg(&t[1]) : -1;
        r[1] = n >= 4 ? dsp_reg(&t[3]) : -1;
        if (n < 4 || r[0] < 0 || r[1] < 0 || !tok_imm(&t[2], &imm))
            FAIL("%s wants a register, #bound and a register (not %s)%s",
                 e->name, dsp_noreg(),
                 e->form == DF_SAT ? ", then lsl or asr #n" : "");
        if (imm < lo || imm > hi)
            FAIL("%s: the bound is %ld..%ld, not %ld", e->name, lo, hi, imm);
        if (e->form == DF_SAT16) {
            if (n != 4)
                FAIL("%s takes no shift", e->name);
            t_sat16(out, r[0], (int)imm, r[1], e->b);
            return 0;
        }
        if (n > 4 && !tok_shift(t, n, 4, "lsl", &amt)) {
            asr = 1;
            if (!tok_shift(t, n, 4, "asr", &amt))
                FAIL("%s: the shift is lsl #0..31 or asr #1..31", e->name);
        }
        /* (ARM state's asr reaches 32, amount 0, which in Thumb state is
         * the 16-bit form's encoding) */
        if (amt < asr || amt > (asr && t_isa_a32 ? 32 : 31))
            FAIL("%s: the shift is lsl #0..31 or asr #1..%d, not %s #%ld",
                 e->name, t_isa_a32 ? 32 : 31, asr ? "asr" : "lsl", amt);
        t_sat(out, r[0], (int)imm, r[1], e->b, asr, (int)amt);
        return 0;
    }
    case DF_PKH: {
        for (int k = 0; k < 3; k++)
            r[k] = n >= 4 ? dsp_reg(&t[k + 1]) : -1;
        if (n < 4 || r[0] < 0 || r[1] < 0 || r[2] < 0 ||
            !tok_shift(t, n, 4, e->a ? "asr" : "lsl", &amt))
            FAIL("%s wants three registers (not %s) and an optional "
                 "`%s`", e->name, dsp_noreg(),
                 e->a ? "asr #1..32" : "lsl #0..31");
        /* pkhtb with no shift is pkhbt with the sources the other way
         * round: the top half of rn and the bottom of rm, as llvm-mc and
         * GNU as encode it (asr #0 is not an encoding: amount 0 is #32) */
        if (e->a && n == 4) {
            t_pkh(out, r[0], r[2], r[1], 0, 0);
            return 0;
        }
        if (e->a ? (amt < 1 || amt > 32) : (amt < 0 || amt > 31))
            FAIL("%s: the shift is %s, not %ld", e->name,
                 e->a ? "asr #1..32" : "lsl #0..31", amt);
        t_pkh(out, r[0], r[1], r[2], e->a, (int)amt);
        return 0;
    }
    default:
        break;
    }

    /* the rest: registers only, three or four (the multiplies' Ra, the
     * long ones' RdLo, RdHi, Rn, Rm) */
    nreg = e->form == DF_Q || e->form == DF_SEL ? 3
         : e->form == DF_SMLALXY || e->form == DF_SMLALD ? 4 : 3 + e->acc;
    for (int k = 0; k < nreg; k++)
        r[k] = n == nreg + 1 ? dsp_reg(&t[k + 1]) : -1;
    for (int k = 0; k < nreg; k++)
        if (r[k] < 0)
            FAIL("\"%.*s\" wants %s registers, none of them %s",
                 t[0].len, t[0].s, nreg == 4 ? "four" : "three", dsp_noreg());
    switch (e->form) {
    case DF_Q:      t_qarith(out, e->a, r[0], r[1], r[2]); break;
    case DF_SEL:    t_sel(out, r[0], r[1], r[2]); break;
    case DF_SMLAXY: t_smlaxy(out, r[0], r[1], r[2], e->acc ? r[3] : 15,
                             e->a, e->b); break;
    case DF_SMLAW:  t_smlaw(out, r[0], r[1], r[2], e->acc ? r[3] : 15, e->b);
                    break;
    case DF_SMLAD:  t_smlad(out, r[0], r[1], r[2], e->acc ? r[3] : 15, e->a,
                            e->b); break;
    case DF_SMMLA:  t_smmla(out, r[0], r[1], r[2], e->acc ? r[3] : 15, e->a,
                            e->b); break;
    case DF_USADA8: t_usada8(out, r[0], r[1], r[2], e->acc ? r[3] : 15);
                    break;
    default:
        /* RdLo and RdHi the same register is UNPREDICTABLE */
        if (r[0] == r[1])
            FAIL("%s: RdLo and RdHi must be different registers", e->name);
        if (e->form == DF_SMLALXY)
            t_smlalxy(out, r[0], r[1], r[2], r[3], e->a, e->b);
        else
            t_smlald(out, r[0], r[1], r[2], r[3], e->a, e->b);
        break;
    }
    return 0;
}

/* ---- the multiplies, reversals, bit fields and pairs ----------------
 *
 * What CMSIS and hand-written asm reach for beyond the DSP extension: mla
 * and mls, the long multiplies (and umaal, which on a Cortex-M is the
 * extension's), rev16 and revsh (CMSIS's __REV16 and __REVSH), rrx, the
 * bit-field inserts and extracts, ldrd/strd in each addressing form, and
 * in ARM state the doubleword exclusives. Each goes out through the
 * encoder the code generator uses (emit.c), so the two cannot disagree;
 * on a Thumb-1 core (ARMv6-M, ARMv8-M Baseline) tasm_assemble then
 * refuses the 32-bit forms by name, as llvm-mc refuses them. */
enum { MF_MLA, MF_LMUL, MF_REV, MF_RRX, MF_BFI, MF_BFX, MF_LDRD, MF_LDREXD,
       MF_STREXD };
/* a: mls (MF_MLA); smull 0, umull 1, smlal 2, umlal 3, umaal 4 (MF_LMUL);
 * revsh (MF_REV); S (MF_RRX); bfc (MF_BFI); signed (MF_BFX); store
 * (MF_LDRD). dsp: the DSP extension's on a Cortex-M. */
struct more_ent { const char *name; int form, a, dsp; };
static const struct more_ent more_tab[] = {
    { "mla", MF_MLA, 0, 0 }, { "mls", MF_MLA, 1, 0 },
    { "smull", MF_LMUL, 0, 0 }, { "umull", MF_LMUL, 1, 0 },
    { "smlal", MF_LMUL, 2, 0 }, { "umlal", MF_LMUL, 3, 0 },
    { "umaal", MF_LMUL, 4, 1 },
    { "rev16", MF_REV, 0, 0 }, { "revsh", MF_REV, 1, 0 },
    { "rrx", MF_RRX, 0, 0 }, { "rrxs", MF_RRX, 1, 0 },
    { "bfi", MF_BFI, 0, 0 }, { "bfc", MF_BFI, 1, 0 },
    { "sbfx", MF_BFX, 1, 0 }, { "ubfx", MF_BFX, 0, 0 },
    { "ldrd", MF_LDRD, 0, 0 }, { "strd", MF_LDRD, 1, 0 },
    { "ldrexd", MF_LDREXD, 0, 0 }, { "strexd", MF_STREXD, 0, 0 },
    { NULL, 0, 0, 0 }
};

/* ldrd/strd rt, rt2, then [rn{, #off}]{!} or [rn], #off. */
static int pair_stmt(const struct more_ent *e, const struct tok *t, int n,
                     struct code *out, char *err, int errlen)
{
    int rt = n >= 4 ? tok_reg(&t[1]) : -1, rt2 = n >= 4 ? tok_reg(&t[2]) : -1;
    int rn, wb = 0, post = n == 5, ok;
    long off = 0;
    struct tok m = n >= 4 ? t[3] : t[0];
    if (m.len > 1 && m.s[m.len - 1] == '!') {
        wb = 1;
        m.len--;
    }
    if (n < 4 || n > 5 || rt < 0 || rt2 < 0 || !tok_mem(&m, &rn, &off) ||
        (post && (wb || off != 0 || !tok_imm(&t[4], &off))))
        FAIL("%s wants two registers and [rn{, #off}]{!} or [rn], #off",
             e->name);
    if (t_isa_a32) {
        if ((rt & 1) || rt == 14 || rt2 != rt + 1)
            FAIL("%s: in ARM state the pair is an even register and the next "
                 "one (r0, r1 ... r12, sp), not %s, %s", e->name,
                 t_reg_name(rt), t_reg_name(rt2));
    } else if (rt == 13 || rt == 15 || rt2 == 13 || rt2 == 15) {
        FAIL("%s: sp and pc are not its registers", e->name);
    } else if (!e->a && rt == rt2) {
        FAIL("%s: the two registers it loads must differ", e->name);
    }
    if ((wb || post) && (rn == 15 || rn == rt || rn == rt2))
        FAIL("%s with writeback: the base must be neither pc nor one of the "
             "pair", e->name);
    if (rn == 15 && e->a)
        FAIL("%s: there is no store to a pc-relative address", e->name);
    ok = t_ldst_pair_any(out, rt, rt2, rn, off, e->a,
                         post ? T_IDX_POST : wb ? T_IDX_PRE : T_IDX_OFF);
    if (!ok)
        FAIL("%s: the offset is %s, not %ld", e->name, t_isa_a32
             ? "-255..255" : "a multiple of 4 in -1020..1020", off);
    return 0;
}

static int more_stmt(const struct tok *t, int n, struct code *out,
                     char *err, int errlen)
{
    const struct more_ent *e;
    int r[4];
    long lsb, width;
    for (e = more_tab; e->name; e++)
        if (mnemonic_is(&t[0], e->name))
            break;
    if (!e->name)
        return 1;
    if (e->dsp && !t_isa_a32 && !dsp_ok())
        return dsp_refuse(t, err, errlen);
    switch (e->form) {
    case MF_MLA: case MF_LMUL:
        for (int k = 0; k < 4; k++)
            r[k] = n == 5 ? dsp_reg(&t[k + 1]) : -1;
        if (r[0] < 0 || r[1] < 0 || r[2] < 0 || r[3] < 0)
            FAIL("%s wants four registers, none of them %s", e->name,
                 dsp_noreg());
        if (e->form == MF_MLA) {
            if (e->a) t_mls(out, r[0], r[1], r[2], r[3]);
            else      t_mla(out, r[0], r[1], r[2], r[3]);
            return 0;
        }
        /* RdLo and RdHi the same register is UNPREDICTABLE */
        if (r[0] == r[1])
            FAIL("%s: RdLo and RdHi must be different registers", e->name);
        if (e->a == 4)
            t_umaal(out, r[0], r[1], r[2], r[3]);
        else if (e->a < 2)
            t_mull(out, r[0], r[1], r[2], r[3], e->a == 0);
        else
            t_mlal(out, r[0], r[1], r[2], r[3], e->a == 2);
        return 0;
    case MF_REV: case MF_RRX:
        r[0] = n == 3 ? dsp_reg(&t[1]) : -1;
        r[1] = n == 3 ? dsp_reg(&t[2]) : -1;
        if (r[0] < 0 || r[1] < 0)
            FAIL("%s wants two registers, neither of them %s", e->name,
                 dsp_noreg());
        if (e->form == MF_RRX) t_rrx(out, r[0], r[1], e->a);
        else if (e->a)         t_revsh(out, r[0], r[1]);
        else                   t_rev16(out, r[0], r[1]);
        return 0;
    case MF_BFI: case MF_BFX: {
        int nr = e->form == MF_BFI && e->a ? 1 : 2;     /* bfc: rd alone */
        for (int k = 0; k < nr; k++)
            r[k] = n == nr + 3 ? dsp_reg(&t[k + 1]) : -1;
        if (r[0] < 0 || (nr == 2 && r[1] < 0) || !tok_imm(&t[nr + 1], &lsb) ||
            !tok_imm(&t[nr + 2], &width))
            FAIL("%s wants %s (not %s), #lsb and #width", e->name,
                 nr == 1 ? "a register" : "two registers", dsp_noreg());
        if (lsb < 0 || lsb > 31 || width < 1 || width > 32 - lsb)
            FAIL("%s: the field is lsb 0..31 and width 1..32-lsb, not #%ld, "
                 "#%ld", e->name, lsb, width);
        if (e->form == MF_BFX)
            t_bfx(out, r[0], r[1], (int)lsb, (int)width, e->a);
        else
            t_bfi(out, r[0], nr == 1 ? 15 : r[1], (int)lsb, (int)width);
        return 0;
    }
    case MF_LDRD:
        return pair_stmt(e, t, n, out, err, errlen);
    default:
        break;
    }
    /* ldrexd rt, rt2, [rn] / strexd rd, rt, rt2, [rn] */
    if (!t_isa_a32)
        FAIL("%s is not an M-profile instruction: no Cortex-M core has a "
             "doubleword exclusive", e->name);
    {
        int st = e->form == MF_STREXD, rd = st && n == 5 ? tok_reg(&t[1]) : 0;
        int rt = n == 4 + st ? tok_reg(&t[1 + st]) : -1;
        int rt2 = n == 4 + st ? tok_reg(&t[2 + st]) : -1, rn;
        long off;
        if (rt < 0 || rt2 < 0 || rd < 0 || !tok_mem(&t[3 + st], &rn, &off) ||
            off != 0)
            FAIL("%s wants %s and a [reg] address with no offset", e->name,
                 st ? "three registers" : "two registers");
        if ((rt & 1) || rt == 14 || rt2 != rt + 1)
            FAIL("%s: the pair is an even register and the next one (r0, r1 "
                 "... r12, sp), not %s, %s", e->name, t_reg_name(rt),
                 t_reg_name(rt2));
        if (rn == 15 || rd == 15)
            FAIL("%s: pc is not its register", e->name);
        if (st && (rd == rn || rd == rt || rd == rt2))
            FAIL("%s: the status register must differ from the others",
                 e->name);
        if (st) a32_strex(out, rd, rt, rn, 8);
        else    a32_ldrex(out, rt, rn, 8);
    }
    return 0;
}

/* ---- Thumb-2 data processing, as an assembler chooses it ---------------
 *
 * The emitters in emit.c choose encodings for the code generator; an
 * assembler has to choose them as GNU as and llvm-mc do, or a .s file
 * assembles to other bytes than theirs. The rules, refereed instruction
 * by instruction against llvm-mc (tools/asm-audit, and the matrix in
 * tests/golden/thumb-dp.sh):
 *
 *   - `.w` is the 32-bit encoding, `.n` the 16-bit one or a refusal;
 *     neither is the shortest that means the same thing.
 *   - A 16-bit data-processing encoding sets the flags outside an IT
 *     block and leaves them inside one, so outside it is the form of
 *     `adds`, and inside of `add`. The compares always set them; the
 *     high-register add, mov and cmp and the SP forms never do.
 *   - An immediate is a modified immediate (ThumbExpandImm); when it is
 *     not one, the instruction with the inverted or negated immediate is
 *     (add/sub, adc/sbc, and/bic, orr/orn, mov/mvn, cmp/cmn); then the
 *     plain twelve-bit addw/subw, and movw for a mov.
 *   - SP is an operand of add, sub, mov and cmp only; PC of none here.
 */
static int g_in_it;       /* assembling an instruction an IT block holds */

static void t16(struct code *o, unsigned h) { code_u16(o, h & 0xffff); }
static void t32(struct code *o, unsigned a, unsigned b)
{
    code_u16(o, a & 0xffff);
    code_u16(o, b & 0xffff);
}

/* ThumbExpandImm's inverse: the twelve bits i:imm3:imm8, or -1 */
static int t_modimm(unsigned long v)
{
    v &= 0xffffffffUL;
    if (v < 256)
        return (int)v;
    unsigned b = (unsigned)(v & 0xff), b1 = (unsigned)((v >> 8) & 0xff);
    if (b && v == ((unsigned long)b | (unsigned long)b << 16))
        return 0x100 | (int)b;
    if (b1 && v == ((unsigned long)b1 << 8 | (unsigned long)b1 << 24))
        return 0x200 | (int)b1;
    if (b && v == ((unsigned long)b * 0x01010101UL))
        return 0x300 | (int)b;
    for (int rot = 8; rot < 32; rot++) {
        unsigned long u = ((v << rot) | (v >> (32 - rot))) & 0xffffffffUL;
        if (u >= 0x80 && u < 0x100)
            return rot << 7 | (int)(u & 0x7f);
    }
    return -1;
}

static unsigned mi_hw1(int imm12) { return (unsigned)(imm12 >> 11) << 10; }
static unsigned mi_hw2(int imm12) { return (unsigned)((imm12 >> 8) & 7) << 12 | (unsigned)(imm12 & 0xff); }

/* the op of a data-processing mnemonic: its T32 op, its 16-bit register
 * group op (-1 none), and its shape: 3 rd/rn/op2, c a compare (rn/op2),
 * m a move (rd/op2) */
struct t2op { const char *m; int op, op16, kind, comm; };
static const struct t2op t2ops[] = {
    { "and", 0, 0, '3', 1 }, { "bic", 1, 14, '3', 0 }, { "orr", 2, 12, '3', 1 },
    { "orn", 3, -1, '3', 0 }, { "eor", 4, 1, '3', 1 }, { "add", 8, -1, '3', 1 },
    { "adc", 10, 5, '3', 1 }, { "sbc", 11, 6, '3', 0 }, { "sub", 13, -1, '3', 0 },
    { "rsb", 14, -1, '3', 0 }, { "tst", 0, 8, 'c', 0 }, { "teq", 4, -1, 'c', 0 },
    { "cmn", 8, 11, 'c', 0 }, { "cmp", 13, 10, 'c', 0 }, { "mov", 2, -1, 'm', 0 },
    { "mvn", 3, 15, 'm', 0 },
};

/* a shift operand, `lsl #3` / `rrx`: type 0..3, amount; 0 when it is not one */
static int t2_shift_tok(const struct tok *t, int *type, int *amt)
{
    static const char *const nm[] = { "lsl", "lsr", "asr", "ror" };
    if (t->len == 3 && same_nocase(t->s, "rrx", 3)) { *type = 3; *amt = 0; return 2; }
    for (int k = 0; k < 4; k++)
        if (t->len > 3 && same_nocase(t->s, nm[k], 3) &&
            (t->s[3] == ' ' || t->s[3] == '\t' || t->s[3] == '#')) {
            struct tok n;
            long v;
            n.s = t->s + 3; n.len = t->len - 3;
            while (n.len && (*n.s == ' ' || *n.s == '\t')) { n.s++; n.len--; }
            if (!tok_imm(&n, &v)) return 0;
            *type = k; *amt = (int)v;
            return 1;
        }
    return 0;
}

/* the splitter breaks at spaces: `lsl #3` is two tokens, joined here
 * into one, as the text they came from is one operand */
static int join_shifts(const struct tok *t, int n, struct tok *m)
{
    int k = 0;
    for (int i = 0; i < n; i++) {
        m[k] = t[i];
        if (i + 1 < n && t[i].len == 3 && t[i + 1].len && t[i + 1].s[0] == '#' &&
            (same_nocase(t[i].s, "lsl", 3) || same_nocase(t[i].s, "lsr", 3) ||
             same_nocase(t[i].s, "asr", 3) || same_nocase(t[i].s, "ror", 3))) {
            m[k].len = (int)(t[i + 1].s + t[i + 1].len - t[i].s);
            i++;
        }
        k++;
    }
    return k;
}

/* 1 when the mnemonic is none of these; else 0, or -1 with err set */
static int t2_dp(const struct tok *t0, int n0, struct code *out, char *err, int errlen)
{
    struct tok t[MAXTOK];
    int n = join_shifts(t0, n0, t);
    char mn[16];
    if (t[0].len >= (int)sizeof mn) return 1;
    for (int i = 0; i < t[0].len; i++) mn[i] = (char)tolower((unsigned char)t[0].s[i]);
    mn[t[0].len] = 0;
    int width = 0;                       /* 'w', 'n' */
    size_t ml = strlen(mn);
    if (ml > 2 && mn[ml - 2] == '.' && (mn[ml - 1] == 'w' || mn[ml - 1] == 'n')) {
        width = mn[ml - 1];
        mn[ml -= 2] = 0;
    }
    const struct t2op *e = NULL;
    int s = 0;
    for (unsigned k = 0; k < sizeof t2ops / sizeof t2ops[0]; k++) {
        size_t l = strlen(t2ops[k].m);
        if (strncmp(mn, t2ops[k].m, l)) continue;
        if (!mn[l]) { e = &t2ops[k]; s = 0; break; }
        if (mn[l] == 's' && !mn[l + 1] && t2ops[k].kind != 'c') { e = &t2ops[k]; s = 1; break; }
    }
    if (!e) return 1;
    if (e->kind == 'c') s = 1;
    /* the operands: d, n, and op2 = m {, shift} | #imm */
    int d = -1, rn = -1, rm = -1, k0, isimm = 0, sht = 0, sha = 0, hasshift = 0;
    long imm = 0;
    int nops = n - 1;
    int written3 = 0;               /* rd, rn, op2 written out, not rd, op2 */
    if (e->kind == 'c' || e->kind == 'm') {
        if (nops < 2 || nops > 3) FAIL("%s takes two operands%s", t[0].len ? mn : "", "");
        if (e->kind == 'c') rn = tok_reg(&t[1]); else d = tok_reg(&t[1]);
        k0 = 2;
    } else {
        if (nops < 2 || nops > 4) FAIL("%.*s takes rd, rn and an operand", t[0].len, t[0].s);
        d = tok_reg(&t[1]);
        /* `and rd, op2` is `and rd, rd, op2` */
        if (nops == 2 || (nops == 3 && t2_shift_tok(&t[3], &sht, &sha))) {
            rn = d; k0 = 2;
        } else {
            rn = tok_reg(&t[2]); k0 = 3;
            written3 = 1;
        }
    }
    if ((e->kind != 'c' && d < 0) || (e->kind != 'm' && rn < 0))
        FAIL("%.*s wants registers", t[0].len, t[0].s);
    if (k0 >= n) FAIL("%.*s wants an operand", t[0].len, t[0].s);
    /* an immediate, with its `#` or without (as inline asm's "I"
     * operands and GNU as both allow: `adds r0, r1, 9`) */
    if (t[k0].len && (t[k0].s[0] == '#' ||
                      (tok_reg(&t[k0]) < 0 && tok_imm(&t[k0], &imm)))) {
        if (!tok_imm(&t[k0], &imm)) FAIL("\"%.*s\" is not an immediate", t[k0].len, t[k0].s);
        isimm = 1;
        if (k0 + 1 < n) FAIL("an immediate takes no shift");
    } else {
        rm = tok_reg(&t[k0]);
        if (rm < 0) FAIL("\"%.*s\" is not a register", t[k0].len, t[k0].s);
        if (k0 + 1 < n) {
            if (!t2_shift_tok(&t[k0 + 1], &sht, &sha))
                FAIL("\"%.*s\" is not a shift", t[k0 + 1].len, t[k0 + 1].s);
            hasshift = 1;
            if (k0 + 2 < n) FAIL("%.*s has one shift", t[0].len, t[0].s);
        }
    }
    int op = e->op;
    int flag16 = !g_in_it;              /* a 16-bit group form sets flags here */
    int grp = e->kind == 'c' || s == flag16;   /* ...and is this instruction */
    int lo = (d < 8) && (rn < 8) && (rm < 8);
    int wide_ok = width != 'n', narrow_ok = width != 'w';
    /* PC is no operand; SP only of add, sub, mov, cmp */
    if (d == 15 || rn == 15 || rm == 15) {
        /* the 16-bit high-register mov and add take pc */
        if (!(narrow_ok && !s && !isimm && !hasshift &&
              ((e->op == 2 && e->kind == 'm') || (e->op == 8 && e->kind == '3' && d == rn))))
            FAIL("%.*s: pc is not an operand here", t[0].len, t[0].s);
    }
    int sp_ok = (e->op == 8 || e->op == 13) && e->kind != 'c';   /* add, sub */
    if (e->kind == 'm' && e->op == 2) sp_ok = 1;                 /* mov */
    if (e->kind == 'c' && (e->op == 13 || e->op == 8))
        sp_ok = rm != 13;                                        /* cmp/cmn sp, ... */
    if ((d == 13 || rn == 13 || rm == 13) && !sp_ok)
        FAIL("%.*s: sp is not an operand of %s", t[0].len, t[0].s, e->m);
    /* a move with sp is the plain register one, mov sp, rm or mov rd, sp:
     * an immediate, a shift or the flags with sp are UNPREDICTABLE */
    if (e->kind == 'm' && (d == 13 || rm == 13) && (s || isimm || hasshift || width == 'w'))
        FAIL("%.*s: sp moves only as `mov sp, rm` or `mov rd, sp`", t[0].len, t[0].s);
    if (rm == 13 && e->kind == '3' && !(e->op == 8 && !hasshift && d == rn))
        FAIL("%.*s: sp is not a second operand here", t[0].len, t[0].s);

    /* ---- register forms ---- */
    if (!isimm) {
        if (hasshift) {
            int lim = sht == 0 ? 31 : sht == 3 ? 31 : 32;
            if (sha < (sht == 0 || (sht == 3 && t[k0 + 1].s[0] == 'r' && t[k0 + 1].len == 3) ? 0 : 1) || sha > lim)
                FAIL("shift %d is outside %s's range", sha, sht == 0 ? "lsl" : sht == 1 ? "lsr" : sht == 2 ? "asr" : "ror");
        }
        int a5 = sha & 31;
        if (narrow_ok && !hasshift) {
            /* add rdn, rm: any registers, no flags -- llvm-mc's choice for
             * a flagless add whose destination is a source, in an IT block
             * or out of one */
            if (e->op == 8 && e->kind == '3' && !s && (d == rn || d == rm) &&
                !(g_in_it && written3 && lo)) {
                int other = d == rn ? rm : rn;
                if (!(d == 13 && other == 13)) {
                    t16(out, 0x4400u | (unsigned)(d >> 3) << 7 | (unsigned)other << 3 | (unsigned)(d & 7));
                    return 0;
                }
            }
            /* add/sub rd, rn, rm (low): the three-register form */
            if ((e->op == 8 || e->op == 13) && e->kind == '3' && grp && lo) {
                t16(out, (e->op == 8 ? 0x1800u : 0x1A00u) | (unsigned)rm << 6 | (unsigned)rn << 3 | (unsigned)d);
                return 0;
            }
            /* mov rd, rm: any registers, no flags; movs (low) is lsls #0 */
            if (e->kind == 'm' && e->op == 2) {
                if (!s) {
                    t16(out, 0x4600u | (unsigned)(d >> 3) << 7 | (unsigned)rm << 3 | (unsigned)(d & 7));
                    return 0;
                }
                if (grp && lo) {
                    t16(out, (unsigned)rm << 3 | (unsigned)d);
                    return 0;
                }
            }
            /* cmp rn, rm: low, or the high-register form */
            if (e->kind == 'c' && e->op == 13) {
                if (rn < 8 && rm < 8) t16(out, 0x4280u | (unsigned)rm << 3 | (unsigned)rn);
                else t16(out, 0x4500u | (unsigned)(rn >> 3) << 7 | (unsigned)rm << 3 | (unsigned)(rn & 7));
                return 0;
            }
            /* the two-register group: rdn, rm (and rm, rdn when the op commutes) */
            if (e->op16 >= 0 && grp && lo) {
                int src = -1, dn = e->kind == 'c' ? rn : d;
                if (e->kind == 'c' || e->kind == 'm') src = rm;
                else if (d == rn) src = rm;
                else if (e->comm && d == rm) src = rn;
                if (src >= 0) {
                    t16(out, 0x4000u | (unsigned)e->op16 << 6 | (unsigned)src << 3 | (unsigned)dn);
                    return 0;
                }
            }
        }
        /* a move with a shift is the shift itself, 16-bit when it can be */
        if (narrow_ok && hasshift && e->kind == 'm' && e->op == 2 && grp && lo &&
            sht < 3 && !(sht == 0 && sha == 0)) {
            t16(out, (unsigned)sht << 11 | (unsigned)a5 << 6 | (unsigned)rm << 3 | (unsigned)d);
            return 0;
        }
        if (!wide_ok) FAIL("%.*s: no 16-bit encoding takes these operands", t[0].len, t[0].s);
        if (e->kind == 'm' && e->op == 2 && !hasshift && s == 0 && 0) { }
        int Rd = e->kind == 'c' ? 15 : d, Rn = e->kind == 'm' ? 15 : rn;
        t32(out, 0xEA00u | (unsigned)op << 5 | (unsigned)s << 4 | (unsigned)Rn,
            (unsigned)(a5 >> 2) << 12 | (unsigned)Rd << 8 | (unsigned)(a5 & 3) << 6 |
            (unsigned)sht << 4 | (unsigned)rm);
        return 0;
    }

    /* ---- immediate forms ---- */
    if (narrow_ok) {
        unsigned long u = (unsigned long)imm & 0xffffffffUL;
        /* SP: add sp, sp, #imm7*4; sub sp; add rd, sp, #imm8*4 */
        if (!s && (e->op == 8 || e->op == 13) && e->kind == '3' && d == 13 && rn == 13) {
            long v = imm;
            int sub = e->op == 13;
            if (v < 0) { v = -v; sub = !sub; }
            if (v % 4 == 0 && v <= 508) {
                t16(out, (sub ? 0xB080u : 0xB000u) | (unsigned)(v / 4));
                return 0;
            }
        }
        if (!s && e->op == 8 && e->kind == '3' && rn == 13 && d < 8 && imm >= 0 &&
            imm % 4 == 0 && imm <= 1020) {
            t16(out, 0xA800u | (unsigned)d << 8 | (unsigned)(imm / 4));
            return 0;
        }
        if ((e->op == 8 || e->op == 13) && e->kind == '3' && grp && d < 8 && rn < 8) {
            long v = imm;
            int sub = e->op == 13;
            if (v < 0 && v > -256) { v = -v; sub = !sub; }
            /* `adds rdn, #imm` is rdn's eight bits; `adds rd, rn, #imm`
             * the three-bit form for 0..7, whatever the registers, and
             * then the eight bits when rd is rn -- as llvm-mc picks them */
            if (!written3 && d == rn && v >= 0 && v < 256) {
                t16(out, (sub ? 0x3800u : 0x3000u) | (unsigned)d << 8 | (unsigned)v);
                return 0;
            }
            if (v >= 0 && v < 8) {
                t16(out, (sub ? 0x1E00u : 0x1C00u) | (unsigned)v << 6 | (unsigned)rn << 3 | (unsigned)d);
                return 0;
            }
            if (d == rn && v >= 0 && v < 256) {
                t16(out, (sub ? 0x3800u : 0x3000u) | (unsigned)d << 8 | (unsigned)v);
                return 0;
            }
        }
        if (e->kind == 'm' && e->op == 2 && grp && d < 8 && u < 256) {
            t16(out, 0x2000u | (unsigned)d << 8 | (unsigned)u);
            return 0;
        }
        /* mvn rd, #~x is mov rd, #x, in sixteen bits when x is a byte */
        if (e->kind == 'm' && e->op == 3 && grp && d < 8 && (~u & 0xffffffffUL) < 256 &&
            t_modimm(u) < 0) {
            t16(out, 0x2000u | (unsigned)d << 8 | (unsigned)(~u & 0xff));
            return 0;
        }
        if (e->kind == 'c' && e->op == 13 && rn < 8 && u < 256) {
            t16(out, 0x2800u | (unsigned)rn << 8 | (unsigned)u);
            return 0;
        }
        /* rsbs rd, rn, #0: neg */
        if (e->op == 14 && grp && d < 8 && rn < 8 && imm == 0) {
            t16(out, 0x4240u | (unsigned)rn << 3 | (unsigned)d);
            return 0;
        }
    }
    if (!wide_ok) FAIL("%.*s: no 16-bit encoding takes these operands", t[0].len, t[0].s);
    int Rd = e->kind == 'c' ? 15 : d, Rn = e->kind == 'm' ? 15 : rn;
    /* a negative immediate is its twin's positive one where llvm-mc
     * makes it so: a flagless add or sub with no .w (add #-1 is sub #1);
     * adds, add.w, cmp and cmn keep the modified immediate of the
     * negative value */
    if (imm < 0 && e->kind == '3' && (e->op == 8 || e->op == 13) && !s && width != 'w') {
        int top = e->op == 8 ? 13 : 8, a12 = t_modimm((unsigned long)-imm);
        if (a12 >= 0) {
            t32(out, 0xF000u | mi_hw1(a12) | (unsigned)top << 5 | (unsigned)s << 4 | (unsigned)Rn,
                mi_hw2(a12) | (unsigned)Rd << 8);
            return 0;
        }
    }
    /* mov rd, #256..65535 is movw, as llvm-mc writes it, unless .w asks
     * for the modified immediate */
    if (!s && e->kind == 'm' && e->op == 2 && width != 'w' && imm >= 256 && imm <= 0xffff) {
        t32(out, 0xF240u | (unsigned)(imm >> 11 & 1) << 10 | (unsigned)(imm >> 12),
            (unsigned)((imm >> 8) & 7) << 12 | (unsigned)d << 8 | (unsigned)(imm & 0xff));
        return 0;
    }
    int m12 = t_modimm((unsigned long)imm);
    if (m12 >= 0) {
        t32(out, 0xF000u | mi_hw1(m12) | (unsigned)op << 5 | (unsigned)s << 4 | (unsigned)Rn,
            mi_hw2(m12) | (unsigned)Rd << 8);
        return 0;
    }
    /* the twin with the inverted or negated immediate */
    {
        int top = -1;
        unsigned long alt = 0;
        switch (e->op) {
        case 8: top = 13; alt = (unsigned long)-imm; break;   /* add <-> sub */
        case 13: top = 8; alt = (unsigned long)-imm; break;
        case 10: top = 11; alt = ~(unsigned long)imm; break;  /* adc <-> sbc */
        case 11: top = 10; alt = ~(unsigned long)imm; break;
        case 0: if (e->kind == '3') { top = 1; alt = ~(unsigned long)imm; } break;
        case 1: top = 0; alt = ~(unsigned long)imm; break;    /* and <-> bic */
        case 2: top = 3; alt = ~(unsigned long)imm; break;    /* orr <-> orn, mov <-> mvn */
        case 3: top = 2; alt = ~(unsigned long)imm; break;
        }
        if (e->kind == 'c' && e->op == 13) { top = 8; alt = (unsigned long)-imm; }   /* cmp <-> cmn */
        else if (e->kind == 'c' && e->op == 8) { top = 13; alt = (unsigned long)-imm; }
        int a12 = top >= 0 ? t_modimm(alt) : -1;
        if (a12 >= 0) {
            t32(out, 0xF000u | mi_hw1(a12) | (unsigned)top << 5 | (unsigned)s << 4 | (unsigned)Rn,
                mi_hw2(a12) | (unsigned)Rd << 8);
            return 0;
        }
    }
    /* addw/subw, movw */
    if (!s && (e->op == 8 || e->op == 13) && e->kind == '3') {
        long v = imm;
        int sub = e->op == 13;
        if (v < 0) { v = -v; sub = !sub; }
        if (v <= 4095) {
            t32(out, (sub ? 0xF2A0u : 0xF200u) | (unsigned)(v >> 11) << 10 | (unsigned)rn,
                (unsigned)((v >> 8) & 7) << 12 | (unsigned)d << 8 | (unsigned)(v & 0xff));
            return 0;
        }
    }
    if (!s && e->kind == 'm' && e->op == 2 && imm >= 0 && imm <= 0xffff) {
        t32(out, 0xF240u | (unsigned)(imm >> 11 & 1) << 10 | (unsigned)(imm >> 12),
            (unsigned)((imm >> 8) & 7) << 12 | (unsigned)d << 8 | (unsigned)(imm & 0xff));
        return 0;
    }
    FAIL("%ld is not an immediate %.*s can encode", imm, t[0].len, t[0].s);
}

/* a barrier's option: `dmb ish` */
struct baropt { const char *name; int crm; };
static const struct baropt t_baropt[] = {
    { "sy", 15 }, { "st", 14 }, { "ld", 13 }, { "ish", 11 }, { "ishst", 10 },
    { "ishld", 9 }, { "nsh", 7 }, { "nshst", 6 }, { "nshld", 5 }, { "osh", 3 },
    { "oshst", 2 }, { "oshld", 1 }, { "un", 7 }, { "unst", 6 }, { NULL, 0 },
};

/* shifts, addw/subw, mul, neg, tbb/tbh, and the barriers' options */
static int t2_misc(const struct tok *t, int n, struct code *out, char *err, int errlen)
{
    char mn[16];
    if (t[0].len >= (int)sizeof mn) return 1;
    for (int i = 0; i < t[0].len; i++) mn[i] = (char)tolower((unsigned char)t[0].s[i]);
    mn[t[0].len] = 0;
    int width = 0;
    size_t ml = strlen(mn);
    if (ml > 2 && mn[ml - 2] == '.' && (mn[ml - 1] == 'w' || mn[ml - 1] == 'n')) {
        width = mn[ml - 1];
        mn[ml -= 2] = 0;
    }
    int narrow_ok = width != 'w', wide_ok = width != 'n';
    int flag16 = !g_in_it;
    static const char *const shn[] = { "lsl", "lsr", "asr", "ror" };
    for (int k = 0; k < 4; k++) {
        size_t l = 3;
        int s;
        if (strncmp(mn, shn[k], l)) continue;
        if (!mn[l]) s = 0;
        else if (mn[l] == 's' && !mn[l + 1]) s = 1;
        else continue;
        int d, a, b = -1;
        long v = 0;
        int isimm;
        if (n != 3 && n != 4) FAIL("%.*s takes rd, rm and a shift", t[0].len, t[0].s);
        d = tok_reg(&t[1]);
        a = n == 4 ? tok_reg(&t[2]) : d;
        isimm = t[n - 1].len && (t[n - 1].s[0] == '#' ||
                                 (tok_reg(&t[n - 1]) < 0 && tok_imm(&t[n - 1], &v)));
        if (isimm) { if (!tok_imm(&t[n - 1], &v)) FAIL("bad shift"); }
        else b = tok_reg(&t[n - 1]);
        if (d < 0 || a < 0 || (!isimm && b < 0)) FAIL("%.*s wants registers", t[0].len, t[0].s);
        if (d == 13 || d == 15 || a == 13 || a == 15 || (!isimm && (b == 13 || b == 15)))
            FAIL("%.*s: sp and pc are no operands here", t[0].len, t[0].s);
        int grp = s == flag16;
        if (isimm) {
            int lim = k == 0 ? 31 : k == 3 ? 31 : 32;
            if (v < (k == 0 ? 0 : 1) || v > lim) FAIL("shift %ld is outside %s's range", v, shn[k]);
            if (narrow_ok && grp && d < 8 && a < 8 && k < 3 && !(k == 0 && v == 0 && 0)) {
                t16(out, (unsigned)k << 11 | (unsigned)(v & 31) << 6 | (unsigned)a << 3 | (unsigned)d);
                return 0;
            }
            if (!wide_ok) FAIL("%.*s: no 16-bit encoding takes these operands", t[0].len, t[0].s);
            int a5 = (int)(v & 31);
            t32(out, 0xEA4Fu | (unsigned)s << 4,
                (unsigned)(a5 >> 2) << 12 | (unsigned)d << 8 | (unsigned)(a5 & 3) << 6 | (unsigned)k << 4 | (unsigned)a);
            return 0;
        }
        static const int g16[] = { 2, 3, 4, 7 };
        if (narrow_ok && grp && d < 8 && b < 8 && d == a) {
            t16(out, 0x4000u | (unsigned)g16[k] << 6 | (unsigned)b << 3 | (unsigned)d);
            return 0;
        }
        if (!wide_ok) FAIL("%.*s: no 16-bit encoding takes these operands", t[0].len, t[0].s);
        t32(out, 0xFA00u | (unsigned)k << 5 | (unsigned)s << 4 | (unsigned)a, 0xF000u | (unsigned)d << 8 | (unsigned)b);
        return 0;
    }
    if (!strcmp(mn, "addw") || !strcmp(mn, "subw")) {
        long v;
        if (n != 4 && n != 3) FAIL("%s takes rd, rn and a 12-bit immediate", mn);
        int d = tok_reg(&t[1]), r = n == 4 ? tok_reg(&t[2]) : d;
        if (d < 0 || r < 0 || !tok_imm(&t[n - 1], &v)) FAIL("%s wants rd, rn, #imm", mn);
        if (v < 0 || v > 4095) FAIL("%s takes 0 to 4095", mn);
        if (d == 15 || (d == 13 && r != 13)) FAIL("%s: that destination is not allowed", mn);
        t32(out, (mn[0] == 's' ? 0xF2A0u : 0xF200u) | (unsigned)(v >> 11) << 10 | (unsigned)r,
            (unsigned)((v >> 8) & 7) << 12 | (unsigned)d << 8 | (unsigned)(v & 0xff));
        return 0;
    }
    if (!strcmp(mn, "mul") || !strcmp(mn, "muls")) {
        int s = mn[3] == 's';
        if (n != 3 && n != 4) FAIL("%s takes rd, rn, rm", mn);
        int d = tok_reg(&t[1]), a = tok_reg(&t[2]), b = n == 4 ? tok_reg(&t[3]) : d;
        if (d < 0 || a < 0 || b < 0) FAIL("%s wants registers", mn);
        if (d == 13 || d == 15 || a == 13 || a == 15 || b == 13 || b == 15) FAIL("%s: sp and pc are no operands here", mn);
        if (narrow_ok && s == flag16 && d < 8 && a < 8 && b < 8 && (d == b || d == a)) {
            t16(out, 0x4340u | (unsigned)(d == b ? a : b) << 3 | (unsigned)d);
            return 0;
        }
        if (s) FAIL("muls is a 16-bit instruction: rd is one of the low registers it multiplies%s",
                    g_in_it ? ", and an IT block makes it mul" : "");
        if (!wide_ok) FAIL("%s: no 16-bit encoding takes these operands", mn);
        t32(out, 0xFB00u | (unsigned)a, 0xF000u | (unsigned)d << 8 | (unsigned)b);
        return 0;
    }
    if ((!strcmp(mn, "sdiv") || !strcmp(mn, "udiv")) && n == 3) {     /* rd, rm: rd /= rm */
        int d = tok_reg(&t[1]), m = tok_reg(&t[2]);
        if (d < 0 || m < 0 || d == 13 || d == 15 || m == 13 || m == 15) FAIL("%s wants registers, not sp or pc", mn);
        t32(out, (mn[0] == 's' ? 0xFB90u : 0xFBB0u) | (unsigned)d, 0xF0F0u | (unsigned)d << 8 | (unsigned)m);
        return 0;
    }
    if (!strcmp(mn, "neg") || !strcmp(mn, "negs")) {
        int s = mn[3] == 's';
        if (n != 3) FAIL("%s takes rd, rm", mn);
        int d = tok_reg(&t[1]), a = tok_reg(&t[2]);
        if (d < 0 || a < 0 || d == 13 || d == 15 || a == 13 || a == 15) FAIL("%s wants low or high registers, not sp or pc", mn);
        if (narrow_ok && s == flag16 && d < 8 && a < 8) {
            t16(out, 0x4240u | (unsigned)a << 3 | (unsigned)d);
            return 0;
        }
        if (!wide_ok) FAIL("%s: no 16-bit encoding takes these operands", mn);
        t32(out, 0xF1C0u | (unsigned)s << 4 | (unsigned)a, (unsigned)d << 8);
        return 0;
    }
    if (!strcmp(mn, "tbb") || !strcmp(mn, "tbh")) {
        int h = mn[2] == 'h', rn, rm, sh = 0;
        if (n != 2 || !tok_mem_reg(&t[1], &rn, &rm, &sh)) FAIL("%s takes [rn, rm%s]", mn, h ? ", lsl #1" : "");
        if (sh != (h ? 1 : 0)) FAIL("%s's index is %s", mn, h ? "shifted left by one" : "not shifted");
        if (g_in_it && 0) {}
        t32(out, 0xE8D0u | (unsigned)rn, 0xF000u | (unsigned)h << 4 | (unsigned)rm);
        return 0;
    }
    if (!strcmp(mn, "dsb") || !strcmp(mn, "dmb") || !strcmp(mn, "isb")) {
        const struct baropt *bo = t_baropt;
        long crm = 15;
        if (n == 2) {
            crm = -1;
            for (unsigned k = 0; bo[k].name; k++)
                if (tok_is(&t[1], bo[k].name)) crm = bo[k].crm;
            if (crm < 0 && (!tok_imm(&t[1], &crm) || crm < 0 || crm > 15))
                FAIL("\"%.*s\" is not a barrier option", t[1].len, t[1].s);
            if (mn[0] == 'i' && crm != 15 && t[1].s[0] != '#')
                FAIL("isb has only the sy option");
            if (t[1].s[0] != '#' && (crm & 3) == 1 && !arch_v8m())
                FAIL("\"%.*s\" is an ARMv8 barrier option", t[1].len, t[1].s);
        } else if (n != 1) FAIL("%s takes a barrier option", mn);
        t32(out, 0xF3BFu, 0x8F00u | (mn[0] == 'i' ? 0x60u : mn[1] == 's' ? 0x40u : 0x50u) | (unsigned)crm);
        return 0;
    }
    return 1;
}

/* ---- the floating-point extension (VFP) ---------------------------------
 *
 * Everything FPv4-SP and FPv5 (ARMv7-M, ARMv8-M) and VFPv3/VFPv4 (ARMv7-A)
 * have, with ARMv8's additions (vsel, vmaxnm/vminnm, vrint, vcvt with a
 * rounding mode) and the half-precision forms of FP16. One word per
 * instruction, laid out as A32 has it: the condition 1110 -- or 1111 for
 * ARMv8's, which no condition reaches -- then the bits. In Thumb state the
 * same word is the two halfwords. llvm-mc is the referee (the Thumb matrix
 * in tests/golden/thumb-vfp-asm.sh).
 *
 * A register field is split, its low or high bit apart: a single register
 * sN is (N >> 1, N & 1), a double dN is (N & 15, N >> 4). The size field
 * (bits 9:8) is 01 half, 10 single, 11 double. */

static int g_fpu = 0;       /* 0 no FPU named (anything goes: a context
                             * switch tests the FPU at run time), 1 single
                             * precision, 2 double */
static int g_fpu_d32 = 0;   /* d16-d31 exist (VFPv3/v4 without -d16) */

void tasm_set_fpu(int level, int d32)
{
    g_fpu = level;
    g_fpu_d32 = d32;
}

/* `s7` / `d12`: its number, with *dbl saying which file. -1 otherwise. */
static int vreg(const struct tok *t, int *dbl)
{
    int v = 0;
    char c;
    if (t->len < 2 || t->len > 3) return -1;
    c = (char)tolower((unsigned char)t->s[0]);
    if (c != 's' && c != 'd') return -1;
    for (int i = 1; i < t->len; i++) {
        if (!isdigit((unsigned char)t->s[i])) return -1;
        v = v * 10 + (t->s[i] - '0');
    }
    if (v > 31) return -1;
    *dbl = c == 'd';
    return v;
}

/* the field a register goes in: 'd' (bits 15:12 and 22), 'n' (19:16 and
 * 7) or 'm' (3:0 and 5) */
static unsigned long vf(int r, int dbl, int pos)
{
    unsigned long v = dbl ? (unsigned)(r & 15) : (unsigned)(r >> 1);
    unsigned long b = dbl ? (unsigned)(r >> 4) & 1u : (unsigned)r & 1u;
    if (pos == 'd') return v << 12 | b << 22;
    if (pos == 'n') return v << 16 | b << 7;
    return v | b << 5;
}

/* the data type suffixes: `.f32`, `.s32.f64`. 'f' 16/32/64, 's'/'u'
 * 16/32, and the bare `.32`/`.64` vmov and vldr take. */
struct vty { char k; int bits; };

static int vty_parse(const char *s, int len, struct vty *ty, int max)
{
    int n = 0, i = 0;
    while (i < len) {
        int b = 0, j;
        char k = 0;
        if (s[i] != '.' || n == max) return -1;
        i++;
        if (i < len && isalpha((unsigned char)s[i]))
            k = (char)tolower((unsigned char)s[i++]);
        for (j = i; j < len && isdigit((unsigned char)s[j]); j++)
            b = b * 10 + (s[j] - '0');
        if (j == i) return -1;
        i = j;
        if (k && k != 'f' && k != 's' && k != 'u' && k != 'i') return -1;
        ty[n].k = k;
        ty[n].bits = b;
        n++;
    }
    return n;
}

/* size bits 9:8 for a float type, 0 when it is none */
static unsigned long vsz(const struct vty *t)
{
    if (t->k != 'f' && t->k != 0) return 0;
    return t->bits == 16 ? 0x900u : t->bits == 32 ? 0xA00u
         : t->bits == 64 ? 0xB00u : 0;
}

static int v_emit(struct code *out, unsigned long w, const struct tok *t0,
                  char *err, int errlen)
{
    if ((w >> 28) == 0xF) {
        if (g_in_it || (t_isa_a32 && a32_it_open()))
            FAIL("\"%.*s\" is an ARMv8 instruction that cannot be "
                 "conditional", t0->len, t0->s);
        if (t_isa_a32) code_u32(out, w);
        else t32(out, (unsigned)(w >> 16), (unsigned)(w & 0xffff));
        return 0;
    }
    /* (the table's words leave the condition 0: 1110 is the Thumb
     * prefix, and a32_vfp_word puts the condition there in ARM state) */
    w |= 0xE0000000UL;
    if (t_isa_a32)
        a32_vfp_word(out, (unsigned)(w >> 16), (unsigned)(w & 0xffff));
    else
        t32(out, (unsigned)(w >> 16), (unsigned)(w & 0xffff));
    return 0;
}

/* VFPExpandImm's inverse: the 8 bits a constant packs into, or -1 */
static int vfp_imm8(const struct tok *t, int bits)
{
    char buf[64], *e;
    int i = 0;
    double d;
    if (t->len >= (int)sizeof buf) return -1;
    if (t->s[0] == '#') i = 1;
    memcpy(buf, t->s + i, (size_t)(t->len - i));
    buf[t->len - i] = 0;
    d = strtod(buf, &e);
    if (e == buf || *e) return -1;
    if (bits == 64) {
        unsigned long long u;
        memcpy(&u, &d, 8);
        if (u & 0xffffffffffffULL) return -1;
        unsigned b = (unsigned)(u >> 54) & 1u;
        if (((u >> 54) & 0xff) != (b ? 0xffu : 0u)) return -1;
        if (((u >> 62) & 1u) == b) return -1;
        return (int)((u >> 56) & 0x80u) | (int)((u >> 48) & 0x7fu);
    }
    if (bits == 32) {
        float f = (float)d;
        unsigned u;
        if ((double)f != d) return -1;
        memcpy(&u, &f, 4);
        if (u & 0x7ffffu) return -1;
        unsigned b = (u >> 25) & 1u;
        if (((u >> 25) & 0x1f) != (b ? 0x1fu : 0u)) return -1;
        if (((u >> 30) & 1u) == b) return -1;
        return (int)((u >> 24) & 0x80u) | (int)((u >> 19) & 0x7fu);
    }
    /* half: a NOT(b) bb cdefgh 000000 -- the same values as the single
     * form, which an exponent of -3..4 always fits */
    return vfp_imm8(t, 32);
}

/* `{d8-d15}`, `{s16, s17}`: consecutive registers of one file */
static int vreglist(const struct tok *t, int *first, int *count, int *dbl)
{
    const char *p = t->s, *end = t->s + t->len;
    int next = -1, d0 = -1;
    *first = -1;
    *count = 0;
    if (t->len < 3 || p[0] != '{' || end[-1] != '}') return 0;
    p++;
    end--;
    while (p < end) {
        struct tok a, b;
        int lo, hi, da, db;
        while (p < end && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (p >= end) break;
        a.s = p;
        while (p < end && isalnum((unsigned char)*p)) p++;
        a.len = (int)(p - a.s);
        lo = vreg(&a, &da);
        if (lo < 0) return 0;
        hi = lo;
        db = da;
        while (p < end && isspace((unsigned char)*p)) p++;
        if (p < end && *p == '-') {
            p++;
            while (p < end && isspace((unsigned char)*p)) p++;
            b.s = p;
            while (p < end && isalnum((unsigned char)*p)) p++;
            b.len = (int)(p - b.s);
            hi = vreg(&b, &db);
            if (hi < lo) return 0;
        }
        if (da != db || (d0 >= 0 && da != d0)) return 0;
        d0 = da;
        if (next >= 0 && lo != next) return 0;
        if (*first < 0) *first = lo;
        *count += hi - lo + 1;
        next = hi + 1;
    }
    *dbl = d0;
    return *first >= 0;
}

/* vmrs/vmsr's system registers */
static int vsysreg(const struct tok *t)
{
    static const struct { const char *n; int r; } sr[] = {
        { "fpsid", 0 }, { "fpscr", 1 }, { "fpscr_nzcvqc", 2 }, { "mvfr2", 5 },
        { "mvfr1", 6 }, { "mvfr0", 7 }, { "fpexc", 8 }, { "fpinst", 9 },
        { "fpinst2", 10 }, { "vpr", 12 }, { "p0", 13 }, { "fpcxtns", 14 },
        { "fpcxts", 15 }, { NULL, 0 }
    };
    for (int k = 0; sr[k].n; k++)
        if (tok_is(t, sr[k].n)) return sr[k].r;
    return -1;
}

static int vfp_word(const char *w, int wlen)
{
    struct tok t = { w, wlen };
    int d;
    if (vreg(&t, &d) >= 0 || vsysreg(&t) >= 0 || tok_is(&t, "apsr_nzcv"))
        return 1;
    /* a scalar's register: `d0[1]` is split at the bracket */
    return 0;
}

/* d16-d31 and double-precision arithmetic, where the FPU is known */
static int vfp_check(const struct tok *t0, int n, const struct tok *t,
                     int dp_math, char *err, int errlen)
{
    if (dp_math && g_fpu == 1)
        FAIL("\"%.*s\" is double-precision arithmetic, and this FPU is "
             "single precision (--fpu=fpv5-d16 or vfpv4-d16 has it)",
             t0->len, t0->s);
    for (int k = 1; k < n; k++) {
        int d, r = vreg(&t[k], &d);
        if (r >= 16 && d && (!t_isa_a32 || (g_fpu && !g_fpu_d32)))
            FAIL("%.*s: this FPU has d0-d15 only", t[k].len, t[k].s);
    }
    return 0;
}

static int vfp_stmt(const struct tok *t, int n, struct code *out,
                    char *err, int errlen)
{
    char mn[24];
    struct vty ty[3];
    int ml = 0, nty;
    unsigned long w;
    while (ml < t[0].len && t[0].s[ml] != '.') ml++;
    if (ml >= (int)sizeof mn || t[0].len < 2 ||
        tolower((unsigned char)t[0].s[0]) != 'v')
        return 1;
    for (int i = 0; i < ml; i++) mn[i] = (char)tolower((unsigned char)t[0].s[i]);
    mn[ml] = 0;
    nty = vty_parse(t[0].s + ml, t[0].len - ml, ty, 3);
    if (nty < 0) return 1;
#define V_IS(s) (!strcmp(mn, s))
#define VOPS(k) (n == (k) + 1)

    /* ---- the arithmetic: d = n op m ---- */
    {
        static const struct { const char *name; unsigned long op; int v8; } dp[] = {
            { "vmla", 0x0E000000, 0 }, { "vmls", 0x0E000040, 0 },
            { "vnmls", 0x0E100000, 0 }, { "vnmla", 0x0E100040, 0 },
            { "vmul", 0x0E200000, 0 }, { "vnmul", 0x0E200040, 0 },
            { "vadd", 0x0E300000, 0 }, { "vsub", 0x0E300040, 0 },
            { "vdiv", 0x0E800000, 0 }, { "vfnms", 0x0E900000, 0 },
            { "vfnma", 0x0E900040, 0 }, { "vfma", 0x0EA00000, 0 },
            { "vfms", 0x0EA00040, 0 },
            { "vmaxnm", 0xFE800000, 1 }, { "vminnm", 0xFE800040, 1 },
            { "vseleq", 0xFE000000, 1 }, { "vselvs", 0xFE100000, 1 },
            { "vselge", 0xFE200000, 1 }, { "vselgt", 0xFE300000, 1 },
            { NULL, 0, 0 }
        };
        for (int k = 0; dp[k].name; k++) {
            int rd, rn, rm, dd, dn, dm;
            if (!V_IS(dp[k].name)) continue;
            if (nty != 1 || !vsz(&ty[0]))
                FAIL("%s takes a .f16, .f32 or .f64 type", dp[k].name);
            /* `vadd.f32 s0, s1` is s0 += s1, for the plain arithmetic */
            int two = V_IS("vadd") || V_IS("vsub") || V_IS("vmul") ||
                      V_IS("vnmul") || V_IS("vdiv");
            if (!VOPS(3) && !(VOPS(2) && two))
                FAIL("%s takes three registers", dp[k].name);
            rd = vreg(&t[1], &dd);
            rn = VOPS(3) ? vreg(&t[2], &dn) : rd;
            if (!VOPS(3)) dn = dd;
            rm = vreg(&t[n - 1], &dm);
            if (rd < 0 || rn < 0 || rm < 0 || dd != dn || dd != dm ||
                dd != (ty[0].bits == 64))
                FAIL("%.*s takes %s registers", t[0].len, t[0].s,
                     ty[0].bits == 64 ? "double (d)" : "single (s)");
            if (vfp_check(&t[0], n, t, ty[0].bits == 64, err, errlen)) return -1;
            w = dp[k].op | vsz(&ty[0]) | vf(rd, dd, 'd') | vf(rn, dd, 'n') |
                vf(rm, dd, 'm');
            return v_emit(out, w, &t[0], err, errlen);
        }
    }

    /* ---- one source: d = op m ---- */
    {
        static const struct { const char *name; unsigned long op; } un[] = {
            { "vabs", 0x0EB000C0 }, { "vneg", 0x0EB10040 },
            { "vsqrt", 0x0EB100C0 }, { "vrintr", 0x0EB60040 },
            { "vrintz", 0x0EB600C0 }, { "vrintx", 0x0EB70040 },
            { "vrinta", 0xFEB80040 }, { "vrintn", 0xFEB90040 },
            { "vrintp", 0xFEBA0040 }, { "vrintm", 0xFEBB0040 },
            { NULL, 0 }
        };
        for (int k = 0; un[k].name; k++) {
            int rd, rm, dd, dm;
            if (!V_IS(un[k].name)) continue;
            if (nty != 1 || !vsz(&ty[0]))
                FAIL("%s takes a .f16, .f32 or .f64 type", un[k].name);
            if (!VOPS(2)) FAIL("%s takes two registers", un[k].name);
            rd = vreg(&t[1], &dd);
            rm = vreg(&t[2], &dm);
            if (rd < 0 || rm < 0 || dd != dm || dd != (ty[0].bits == 64))
                FAIL("%.*s takes %s registers", t[0].len, t[0].s,
                     ty[0].bits == 64 ? "double (d)" : "single (s)");
            if (vfp_check(&t[0], n, t, ty[0].bits == 64, err, errlen)) return -1;
            w = un[k].op | vsz(&ty[0]) | vf(rd, dd, 'd') | vf(rm, dd, 'm');
            return v_emit(out, w, &t[0], err, errlen);
        }
    }

    /* ---- compares: vcmp{e} d, m | d, #0 ---- */
    if (V_IS("vcmp") || V_IS("vcmpe")) {
        int rd, rm, dd, dm;
        unsigned long e = V_IS("vcmpe") ? 0x80u : 0;
        if (nty != 1 || !vsz(&ty[0]) || !VOPS(2))
            FAIL("%s takes a .f16, .f32 or .f64 type and two operands", mn);
        rd = vreg(&t[1], &dd);
        if (rd < 0 || dd != (ty[0].bits == 64))
            FAIL("%.*s compares %s registers", t[0].len, t[0].s,
                 ty[0].bits == 64 ? "double (d)" : "single (s)");
        if (vfp_check(&t[0], n, t, ty[0].bits == 64, err, errlen)) return -1;
        rm = vreg(&t[2], &dm);
        if (rm < 0) {
            char buf[16], *ep;
            snprintf(buf, sizeof buf, "%.*s", t[2].len - 1, t[2].s + 1);
            if (t[2].s[0] != '#' || strtod(buf, &ep) != 0.0 || ep == buf || *ep)
                FAIL("%.*s compares with a register or #0", t[0].len, t[0].s);
            w = 0x0EB50040 | e | vsz(&ty[0]) | vf(rd, dd, 'd');
            return v_emit(out, w, &t[0], err, errlen);
        }
        if (dm != dd)
            FAIL("%.*s compares two registers of one size", t[0].len, t[0].s);
        w = 0x0EB40040 | e | vsz(&ty[0]) | vf(rd, dd, 'd') | vf(rm, dd, 'm');
        return v_emit(out, w, &t[0], err, errlen);
    }

    /* ---- conversions ---- */
    if (V_IS("vcvt") || V_IS("vcvtr") || V_IS("vcvtb") || V_IS("vcvtt") ||
        V_IS("vcvta") || V_IS("vcvtn") || V_IS("vcvtp") || V_IS("vcvtm")) {
        int rd, rm, dd, dm;
        char x = mn[4];
        if (nty != 2)
            FAIL("%s takes a destination and a source type: %s.f32.s32", mn, mn);
        struct vty to = ty[0], from = ty[1];
        rd = vreg(&t[1], &dd);
        rm = VOPS(2) || VOPS(3) ? vreg(&t[2], &dm) : -1;
        if (rd < 0 || rm < 0)
            FAIL("%.*s takes two registers", t[0].len, t[0].s);
        /* half <-> single/double, the bottom or top half */
        if (x == 'b' || x == 't') {
            unsigned long tt = x == 't' ? 0x80u : 0;
            if (!VOPS(2)) FAIL("%s takes two registers", mn);
            if (to.k == 'f' && from.k == 'f' && from.bits == 16 &&
                (to.bits == 32 || to.bits == 64) && !dm && dd == (to.bits == 64))
                w = 0x0EB20040 | tt | (to.bits == 64 ? 0xB00u : 0xA00u) |
                    vf(rd, dd, 'd') | vf(rm, 0, 'm');
            else if (to.k == 'f' && from.k == 'f' && to.bits == 16 &&
                     (from.bits == 32 || from.bits == 64) && !dd &&
                     dm == (from.bits == 64))
                w = 0x0EB30040 | tt | (from.bits == 64 ? 0xB00u : 0xA00u) |
                    vf(rd, 0, 'd') | vf(rm, dm, 'm');
            else
                FAIL("%.*s converts between .f16 and .f32 or .f64",
                     t[0].len, t[0].s);
            if (vfp_check(&t[0], n, t, 0, err, errlen)) return -1;
            return v_emit(out, w, &t[0], err, errlen);
        }
        /* to an integer with a rounding mode of its own (ARMv8) */
        if (x == 'a' || x == 'n' || x == 'p' || x == 'm') {
            unsigned long rmode = x == 'a' ? 0 : x == 'n' ? 1 : x == 'p' ? 2 : 3;
            if (!VOPS(2) || (to.k != 's' && to.k != 'u') || to.bits != 32 ||
                !vsz(&from) || dd || dm != (from.bits == 64))
                FAIL("%s converts .f16, .f32 or .f64 to .s32 or .u32: "
                     "%s.s32.f32 s0, s1", mn, mn);
            w = 0xFEBC0040 | rmode << 16 | (to.k == 's' ? 0x80u : 0) |
                vsz(&from) | vf(rd, 0, 'd') | vf(rm, dm, 'm');
            if (vfp_check(&t[0], n, t, 0, err, errlen)) return -1;
            return v_emit(out, w, &t[0], err, errlen);
        }
        /* fixed point, in place: vcvt.f32.s16 s0, s0, #8 */
        if (VOPS(3)) {
            long fb;
            int tofix = to.k == 's' || to.k == 'u';
            struct vty fl = tofix ? from : to, fx = tofix ? to : from;
            if (x == 'r' || !vsz(&fl) || (fx.k != 's' && fx.k != 'u') ||
                (fx.bits != 16 && fx.bits != 32))
                FAIL("%s's fixed-point form converts .f16/.f32/.f64 and "
                     ".s16/.u16/.s32/.u32", mn);
            if (rd != rm || dd != dm || dd != (fl.bits == 64))
                FAIL("%.*s converts a register in place", t[0].len, t[0].s);
            if (!tok_imm(&t[3], &fb) || fb < (fx.bits == 16 ? 0 : 1) ||
                fb > fx.bits)
                FAIL("%.*s: %d-bit fixed point has %d..%d fraction bits",
                     t[0].len, t[0].s, fx.bits, fx.bits == 16 ? 0 : 1,
                     fx.bits);
            unsigned imm5 = (unsigned)(fx.bits - fb);
            w = 0x0EBA0040 | (tofix ? 0x40000u : 0) |
                (fx.k == 'u' ? 0x10000u : 0) | (fx.bits == 32 ? 0x80u : 0) |
                vsz(&fl) | vf(rd, dd, 'd') | (imm5 & 1u) << 5 | imm5 >> 1;
            if (vfp_check(&t[0], n, t, fl.bits == 64, err, errlen)) return -1;
            return v_emit(out, w, &t[0], err, errlen);
        }
        if (!VOPS(2)) FAIL("%s takes two registers", mn);
        /* between single and double */
        if (to.k == 'f' && from.k == 'f' && x != 'r') {
            if (to.bits == 64 && from.bits == 32 && dd && !dm)
                w = 0x0EB700C0 | 0xA00u | vf(rd, 1, 'd') | vf(rm, 0, 'm');
            else if (to.bits == 32 && from.bits == 64 && !dd && dm)
                w = 0x0EB700C0 | 0xB00u | vf(rd, 0, 'd') | vf(rm, 1, 'm');
            else
                FAIL("%.*s converts between an s and a d register "
                     "(.f64.f32 or .f32.f64)", t[0].len, t[0].s);
            if (vfp_check(&t[0], n, t, 1, err, errlen)) return -1;
            return v_emit(out, w, &t[0], err, errlen);
        }
        /* from an integer */
        if (vsz(&to) && to.k == 'f' && (from.k == 's' || from.k == 'u') &&
            from.bits == 32 && x != 'r') {
            if (dd != (to.bits == 64) || dm)
                FAIL("%.*s converts an s register to %s", t[0].len, t[0].s,
                     to.bits == 64 ? "a d register" : "an s register");
            w = 0x0EB80040 | (from.k == 's' ? 0x80u : 0) | vsz(&to) |
                vf(rd, dd, 'd') | vf(rm, 0, 'm');
            if (vfp_check(&t[0], n, t, to.bits == 64, err, errlen)) return -1;
            return v_emit(out, w, &t[0], err, errlen);
        }
        /* to an integer: vcvt rounds towards zero, vcvtr as FPSCR says */
        if ((to.k == 's' || to.k == 'u') && to.bits == 32 && vsz(&from) &&
            from.k == 'f') {
            if (dd || dm != (from.bits == 64))
                FAIL("%.*s converts %s to an s register", t[0].len, t[0].s,
                     from.bits == 64 ? "a d register" : "an s register");
            w = 0x0EBC0040 | (to.k == 's' ? 0x10000u : 0) |
                (x == 'r' ? 0 : 0x80u) | vsz(&from) | vf(rd, 0, 'd') |
                vf(rm, dm, 'm');
            if (vfp_check(&t[0], n, t, from.bits == 64, err, errlen)) return -1;
            return v_emit(out, w, &t[0], err, errlen);
        }
        FAIL("%.*s is not a conversion this FPU has", t[0].len, t[0].s);
    }

    /* ---- moves ---- */
    if (V_IS("vmov")) {
        int r1, r2, d1, d2;
        int f = nty == 1 ? (int)vsz(&ty[0]) : 0;
        if (nty > 1) FAIL("vmov takes one type");
        r1 = vreg(&t[1], &d1);
        /* a constant: vmov.f32 s0, #1.0 */
        if (VOPS(2) && r1 >= 0 && f && t[2].s[0] == '#') {
            int imm = vfp_imm8(&t[2], ty[0].bits);
            if (d1 != (ty[0].bits == 64))
                FAIL("vmov.f%d takes %s register", ty[0].bits,
                     ty[0].bits == 64 ? "a d" : "an s");
            if (imm < 0)
                FAIL("%.*s is not a constant vmov can hold: +-(16..31)/16 "
                     "times a power of two from 2^-3 to 2^4", t[2].len, t[2].s);
            w = 0x0EB00000 | (unsigned long)f | (unsigned long)(imm >> 4) << 16 |
                (unsigned long)(imm & 15) | vf(r1, d1, 'd');
            if (vfp_check(&t[0], n, t, 0, err, errlen)) return -1;
            return v_emit(out, w, &t[0], err, errlen);
        }
        /* register to register: a d register's copy is .f64 (a bare
         * `vmov d0, d1` is Advanced SIMD's vorr) */
        if (VOPS(2) && r1 >= 0 && (r2 = vreg(&t[2], &d2)) >= 0) {
            if (d1 != d2 || (nty && ty[0].bits != (d1 ? 64 : 32)))
                FAIL("vmov copies between registers of one size: .f32 for "
                     "s registers, .f64 for d");
            if (d1 && !nty)
                FAIL("vmov of a d register is vmov.f64 d0, d1");
            w = 0x0EB00040 | (d1 ? 0xB00u : 0xA00u) | vf(r1, d1, 'd') |
                vf(r2, d1, 'm');
            if (vfp_check(&t[0], n, t, 0, err, errlen)) return -1;
            return v_emit(out, w, &t[0], err, errlen);
        }
        /* a core register and an s register (or a d register's half) */
        if (VOPS(2)) {
            int to_fp = tok_reg(&t[1]) < 0, rt, rs, ds, lane = -1;
            const struct tok *vt = to_fp ? &t[1] : &t[2];
            struct tok base = *vt;
            if (base.len > 3 && base.s[base.len - 1] == ']') {
                const char *lb = memchr(base.s, '[', (size_t)base.len);
                if (lb && lb[1] >= '0' && lb[1] <= '1' && lb[2] == ']') {
                    lane = lb[1] - '0';
                    base.len = (int)(lb - base.s);
                }
            }
            rs = vreg(&base, &ds);
            rt = tok_reg(to_fp ? &t[2] : &t[1]);
            if (rs < 0 || rt < 0 || (lane >= 0) != ds)
                FAIL("vmov moves between a core register and an s register, "
                     "or a d register's half (d0[1])");
            if (rt == 15 || (rt == 13 && !t_isa_a32))
                FAIL("vmov: %s is not a register vmov can move",
                     rt == 15 ? "pc" : "sp");
            if (lane >= 0) {
                if (nty && !(ty[0].k == 0 && ty[0].bits == 32))
                    FAIL("vmov of a d register's half is .32");
                w = (to_fp ? 0x0E000B10u : 0x0E100B10u) |
                    (unsigned long)lane << 21 | vf(rs, 1, 'n') |
                    (unsigned long)rt << 12;
            } else {
                if (nty && ty[0].bits != 16 && !(ty[0].k == 'f' && ty[0].bits == 32))
                    FAIL("vmov between a core and an s register takes no type");
                w = (nty && ty[0].bits == 16 ? 0x0E000910u : 0x0E000A10u) |
                    (to_fp ? 0 : 0x100000u) | vf(rs, 0, 'n') |
                    (unsigned long)rt << 12;
            }
            if (vfp_check(&t[0], n, t, 0, err, errlen)) return -1;
            return v_emit(out, w, &t[0], err, errlen);
        }
        /* two core registers and a d register, or two s registers */
        if (VOPS(3) || VOPS(4)) {
            int to_fp = r1 >= 0, rt, rt2, rs, ds, rs2 = -1, ds2 = 0;
            int nv = VOPS(4) ? 2 : 1;
            const struct tok *v0 = to_fp ? &t[1] : &t[3];
            if (nty) FAIL("vmov of a core register pair takes no type");
            rs = vreg(v0, &ds);
            if (nv == 2) rs2 = vreg(v0 + 1, &ds2);
            rt = tok_reg(to_fp ? &t[1 + nv] : &t[1]);
            rt2 = tok_reg(to_fp ? &t[2 + nv] : &t[2]);
            if (rs < 0 || rt < 0 || rt2 < 0 ||
                (nv == 1 && !ds) || (nv == 2 && (ds || ds2 || rs2 != rs + 1)))
                FAIL("vmov moves two core registers to or from a d register, "
                     "or two consecutive s registers");
            if (rt == 15 || rt2 == 15 || (!t_isa_a32 && (rt == 13 || rt2 == 13)))
                FAIL("vmov: sp and pc are not registers vmov can move");
            if (!to_fp && rt == rt2)
                FAIL("vmov cannot load one register twice");
            w = (to_fp ? 0x0C400010u : 0x0C500010u) | (ds ? 0xB00u : 0xA00u) |
                (unsigned long)rt2 << 16 | (unsigned long)rt << 12 |
                vf(rs, ds, 'm');
            if (vfp_check(&t[0], n, t, 0, err, errlen)) return -1;
            return v_emit(out, w, &t[0], err, errlen);
        }
        FAIL("vmov takes two, three or four operands");
    }

    /* ---- the system registers ---- */
    if ((V_IS("vmrs") || V_IS("vmsr")) && VOPS(2) && !nty) {
        int to_core = V_IS("vmrs");
        int sr = vsysreg(&t[to_core ? 2 : 1]);
        int rt = tok_reg(&t[to_core ? 1 : 2]);
        if (to_core && tok_is(&t[1], "apsr_nzcv")) {
            if (sr != 1) FAIL("only fpscr's flags go to apsr_nzcv");
            rt = 16;
        }
        /* ARMv8.1-M's (MVE's vpr and p0, the security extension's
         * fpcxt), and the identification registers read only */
        if (sr == 2 || sr >= 12)
            FAIL("%.*s is an ARMv8.1-M register, which this target lacks",
                 t[to_core ? 2 : 1].len, t[to_core ? 2 : 1].s);
        if (!to_core && sr >= 5 && sr <= 7)
            FAIL("%.*s is read only", t[1].len, t[1].s);
        if (sr < 0 || rt < 0 || rt == 15 || (rt == 13 && !t_isa_a32))
            FAIL("%s moves between a core register and fpscr, fpexc, "
                 "fpsid, mvfr0-2 (vmrs apsr_nzcv, fpscr for the flags)", mn);
        w = (to_core ? 0x0EF00A10u : 0x0EE00A10u) | (unsigned long)sr << 16 |
            (unsigned long)(rt == 16 ? 15 : rt) << 12;
        return v_emit(out, w, &t[0], err, errlen);
    }

    /* ---- loads and stores ---- */
    if ((V_IS("vldr") || V_IS("vstr")) && VOPS(2)) {
        int rd, dd, rn;
        long off;
        int h = nty == 1 && ty[0].bits == 16;
        rd = vreg(&t[1], &dd);
        if (rd < 0) FAIL("%s takes an s or d register", mn);
        if (nty > 1 || (nty == 1 && ty[0].bits != (dd ? 64 : 32) && !h))
            FAIL("%.*s does not match its register", t[0].len, t[0].s);
        if (!tok_mem(&t[2], &rn, &off))
            FAIL("%s takes a [reg, #off] address", mn);
        if (rn == 15 && V_IS("vstr") && !t_isa_a32)
            FAIL("vstr cannot store pc-relative in Thumb state");
        long a = off < 0 ? -off : off;
        int sc = h ? 2 : 4;
        if (a % sc || a / sc > 255)
            FAIL("%s's offset is a multiple of %d within +-%d", mn, sc, sc * 255);
        int neg = off < 0 || (t[2].len > 3 && strstr(t[2].s, "#-") &&
                              strstr(t[2].s, "#-") < t[2].s + t[2].len);
        w = (V_IS("vldr") ? 0x0D100000u : 0x0D000000u) |
            (h ? 0x900u : dd ? 0xB00u : 0xA00u) | (neg ? 0 : 0x800000u) |
            (unsigned long)rn << 16 | vf(rd, dd, 'd') | (unsigned long)(a / sc);
        if (vfp_check(&t[0], n, t, 0, err, errlen)) return -1;
        return v_emit(out, w, &t[0], err, errlen);
    }
    {
        static const struct { const char *name; int load, db, sp; } vm[] = {
            { "vldm", 1, 0, 0 }, { "vldmia", 1, 0, 0 }, { "vldmdb", 1, 1, 0 },
            { "vstm", 0, 0, 0 }, { "vstmia", 0, 0, 0 }, { "vstmdb", 0, 1, 0 },
            { "vpush", 0, 1, 1 }, { "vpop", 1, 0, 1 }, { NULL, 0, 0, 0 }
        };
        for (int k = 0; vm[k].name; k++) {
            int rn = 13, wb = 1, first, count, dbl;
            const struct tok *lt;
            if (!V_IS(vm[k].name)) continue;
            if (nty > 1 || (nty == 1 && ty[0].k != 0 && ty[0].k != 'f'))
                FAIL("%.*s takes .32 or .64", t[0].len, t[0].s);
            if (vm[k].sp) {
                if (!VOPS(1)) FAIL("%s takes a register list", mn);
                lt = &t[1];
            } else {
                if (!VOPS(2)) FAIL("%s takes a base register and a list", mn);
                wb = t[1].len > 1 && t[1].s[t[1].len - 1] == '!';
                rn = tasm_gpr(t[1].s, t[1].len - wb);
                lt = &t[2];
                if (rn < 0) FAIL("%s takes a base register and a list", mn);
                if (vm[k].db && !wb) FAIL("%s needs writeback (rn!)", mn);
                if (rn == 15 && (wb || !t_isa_a32))
                    FAIL("%s cannot use pc as its base", mn);
            }
            if (!vreglist(lt, &first, &count, &dbl))
                FAIL("%s takes consecutive s or d registers: {d8-d15}", mn);
            if (nty == 1 && ty[0].bits != (dbl ? 64 : 32))
                FAIL("%.*s does not match its registers", t[0].len, t[0].s);
            if (dbl ? count > 16 : first + count > 32)
                FAIL("%s transfers at most 16 d registers", mn);
            if (dbl && first + count > 32)
                FAIL("%s: the list runs past d31", mn);
            w = 0x0C000000u | (dbl ? 0xB00u : 0xA00u) |
                (vm[k].db ? 0x1000000u : 0x800000u) | (wb ? 0x200000u : 0) |
                (vm[k].load ? 0x100000u : 0) | (unsigned long)rn << 16 |
                vf(first, dbl, 'd') | (unsigned long)(dbl ? 2 * count : count);
            {
                struct tok fake[2];
                char nm[8];
                fake[0] = t[0];
                snprintf(nm, sizeof nm, "%c%d", dbl ? 'd' : 's', first + count - 1);
                fake[1].s = nm;
                fake[1].len = (int)strlen(nm);
                if (vfp_check(&t[0], 2, fake, 0, err, errlen)) return -1;
            }
            return v_emit(out, w, &t[0], err, errlen);
        }
    }
#undef V_IS
#undef VOPS
    return 1;
}

/* adr rd, #off: the address `off` from Align(pc, 4) in Thumb state (pc +
 * 8 in ARM state) -- gas.c turns a label into it. Thumb takes the 16-bit
 * form where it reaches (r0-r7, a word 0..1020 ahead) unless .w, else
 * addw/subw rd, pc (T3/T2); ARM state add/sub rd, pc, #const. */
static int arm_adr(const struct tok *t, int n, struct code *out, char *err,
                   int errlen)
{
    int want = 0, rd;
    long off;
    if (mnemonic_is(&t[0], "adr")) {
        if (t[0].len == 5) want = t[0].s[4];
    } else
        return 1;
    if (n != 3 || (rd = tok_reg(&t[1])) < 0 || !tok_imm(&t[2], &off))
        FAIL("adr takes a register and a label");
    if (t_isa_a32) {
        long m = off < 0 ? -off : off;
        int imm = a32_encode_imm((unsigned long)m);
        if (want) FAIL("adr has no .n or .w in ARM state");
        if (imm < 0)
            FAIL("adr: %ld bytes from pc+8 is not a rotated 8-bit constant", off);
        {
            unsigned long w = (off < 0 ? 0x024F0000UL : 0x028F0000UL) |
                              (unsigned long)rd << 12 | (unsigned long)imm;
            a32_raw_word(out, w);
        }
        return 0;
    }
    if (rd == 13 || rd == 15) FAIL("adr cannot write sp or pc");
    if (want != 'w' && (!g_force_wide || arch_thumb1() || want == 'n')) {
        if (rd < 8 && off >= 0 && off <= 1020 && !(off & 3)) {
            t16(out, 0xA000u | (unsigned)rd << 8 | (unsigned)off >> 2);
            return 0;
        }
        if (arch_thumb1() || want == 'n')
            FAIL("adr reaches r0-r7 and a word 0..1020 bytes ahead%s",
                 arch_thumb1() ? " on this core" : " in its 16-bit form");
        if (!g_force_wide) g_went_wide = 1;
    }
    {
        long m = off < 0 ? -off : off;
        if (m > 4095) FAIL("adr reaches 4095 bytes either way; this is %ld", off);
        t32(out, (off < 0 ? 0xF2AFu : 0xF20Fu) | ((unsigned)m >> 11 & 1u) << 10,
            ((unsigned)m >> 8 & 7u) << 12 | (unsigned)rd << 8 | ((unsigned)m & 0xffu));
    }
    return 0;
}

/* ---- ARM state: data processing, loads and stores, multiples ----------
 *
 * As LLVM chooses them, refereed form by form against llvm-mc
 * -triple=armv7a (tests/golden/arm-a32-asm.sh). The condition is not seen
 * here: a conditional mnemonic falls through to one_stmt's suffix path,
 * which queues the condition and calls back with the bare mnemonic --
 * a32_raw_word takes it from the queue. */

/* rm, or rm with a shift: operand 2's register form as bits 11:0 (bit 4
 * set for a shift by a register, which `mem` -- an address's index --
 * cannot have). t[i] is rm. Returns the tokens used, 0 when t[i] is not
 * one, -1 with *why for a bad shift. */
static int a32_regop(const struct tok *t, int i, int n, unsigned long *bits,
                     int mem, const char **why)
{
    static const char *const nm[] = { "lsl", "lsr", "asr", "ror" };
    int rm = tok_reg(&t[i]);
    if (rm < 0) return 0;
    *bits = (unsigned long)rm;
    if (i + 1 >= n) return 1;
    if (tok_is(&t[i + 1], "rrx")) { *bits |= 0x60u; return 2; }
    for (int k = 0; k < 4; k++) {
        long v;
        int rs;
        if (!tok_is(&t[i + 1], nm[k])) continue;
        if (i + 2 >= n) { *why = "a shift needs an amount"; return -1; }
        if ((rs = tok_reg(&t[i + 2])) >= 0) {
            if (mem) { *why = "an address's index shifts by a constant"; return -1; }
            if (rs == 15 || rm == 15) {
                *why = "pc cannot take part in a register-controlled shift";
                return -1;
            }
            *bits |= (unsigned long)rs << 8 | (unsigned long)k << 5 | 0x10u;
            return 3;
        }
        if (!tok_imm(&t[i + 2], &v)) { *why = "a shift amount is #0..#32"; return -1; }
        if (k == 0 ? v < 0 || v > 31 : k == 3 ? v < 1 || v > 31 : v < 1 || v > 32) {
            *why = k == 0 ? "lsl shifts by 0..31" : k == 3 ? "ror rotates by 1..31"
                          : "lsr and asr shift by 1..32";
            return -1;
        }
        if (k == 0 && v == 0) return 3;            /* lsl #0 is rm itself */
        *bits |= (unsigned long)(v & 31) << 7 | (unsigned long)k << 5;
        return 3;
    }
    return 0;
}

struct a32_dpop { const char *name; int opc, kind; };
/* kind 3: rd, rn, op2; 1: rn, op2 (a compare: S always, no rd); 2: rd, op2 */
static const struct a32_dpop a32_dps[] = {
    { "and", 0, 3 }, { "eor", 1, 3 }, { "sub", 2, 3 }, { "rsb", 3, 3 },
    { "add", 4, 3 }, { "adc", 5, 3 }, { "sbc", 6, 3 }, { "rsc", 7, 3 },
    { "tst", 8, 1 }, { "teq", 9, 1 }, { "cmp", 10, 1 }, { "cmn", 11, 1 },
    { "orr", 12, 3 }, { "mov", 13, 2 }, { "bic", 14, 3 }, { "mvn", 15, 2 },
    { NULL, 0, 0 }
};

/* the opcode that holds `v` when this one cannot: add/sub and cmp/cmn by
 * negating, adc/sbc, and/bic and mov/mvn by inverting -- LLVM's aliases */
static int a32_twin(int opc, unsigned long v, unsigned long *tv)
{
    switch (opc) {
    case 2: case 4: case 10: case 11:
        *tv = (0UL - v) & 0xffffffffUL;
        return opc == 2 ? 4 : opc == 4 ? 2 : opc == 10 ? 11 : 10;
    case 5: case 6: case 0: case 14: case 13: case 15:
        *tv = ~v & 0xffffffffUL;
        return opc == 5 ? 6 : opc == 6 ? 5 : opc == 0 ? 14 : opc == 14 ? 0
             : opc == 13 ? 15 : 13;
    }
    return -1;
}

static int a32_dp(const struct tok *t, int n, struct code *out, char *err,
                  int errlen)
{
    char mn[12];
    int s = 0, rd = 0, rn = 0, at, opc = -1, kind = 0, used;
    unsigned long op2 = 0, w;
    const char *why = NULL;
    if (t[0].len >= (int)sizeof mn) return 1;
    for (int i = 0; i < t[0].len; i++) mn[i] = (char)tolower((unsigned char)t[0].s[i]);
    mn[t[0].len] = 0;

    /* the shifts are mov with a shift: lsl rd, rm, #n | rs */
    {
        static const char *const sh[] = { "lsl", "lsr", "asr", "ror", "rrx" };
        for (int k = 0; k < 5; k++) {
            size_t l = strlen(sh[k]);
            if (strncmp(mn, sh[k], l) || (mn[l] && strcmp(mn + l, "s"))) continue;
            s = mn[l] == 's';
            if (k == 4) {
                if (n != 3 || (rd = tok_reg(&t[1])) < 0 || (rn = tok_reg(&t[2])) < 0)
                    FAIL("rrx takes two registers");
                a32_raw_word(out, 0x01A00060UL | (unsigned long)s << 20 |
                                  (unsigned long)rd << 12 | (unsigned long)rn);
                return 0;
            }
            if (n != 3 && n != 4) FAIL("%s takes rd, rm, and #n or rs", sh[k]);
            if ((rd = tok_reg(&t[1])) < 0) FAIL("%s takes rd, rm, and #n or rs", sh[k]);
            {
                struct tok m[3];
                long v;
                m[0] = t[n == 4 ? 2 : 1];
                m[1].s = sh[k]; m[1].len = 3;
                m[2] = t[n - 1];
                /* `lsl r0, r1, #0` is mov; `lsr #0`, which is not a
                 * shift, is mov too */
                if (tok_imm(&m[2], &v) && v == 0) {
                    if ((rn = tok_reg(&m[0])) < 0) FAIL("%s takes a register", sh[k]);
                    a32_raw_word(out, 0x01A00000UL | (unsigned long)s << 20 |
                                      (unsigned long)rd << 12 | (unsigned long)rn);
                    return 0;
                }
                used = a32_regop(m, 0, 3, &op2, 0, &why);
                if (used != 3) FAIL("%s: %s", sh[k], why ? why : "takes rd, rm, and #n or rs");
                if ((op2 & 0x10u) && rd == 15)
                    FAIL("%s: pc cannot take part in a register-controlled shift", sh[k]);
            }
            a32_raw_word(out, 0x01A00000UL | (unsigned long)s << 20 |
                              (unsigned long)rd << 12 | op2);
            return 0;
        }
    }
    /* neg rd, rm is rsb rd, rm, #0 */
    if (!strcmp(mn, "neg") || !strcmp(mn, "negs")) {
        if (n != 3 || (rd = tok_reg(&t[1])) < 0 || (rn = tok_reg(&t[2])) < 0)
            FAIL("neg takes two registers");
        a32_raw_word(out, 0x02600000UL | (unsigned long)(mn[3] == 's') << 20 |
                          (unsigned long)rn << 16 | (unsigned long)rd << 12);
        return 0;
    }
    for (int k = 0; a32_dps[k].name; k++)
        if (!strncmp(mn, a32_dps[k].name, 3) &&
            (!mn[3] || (!strcmp(mn + 3, "s") && a32_dps[k].kind != 1))) {
            opc = a32_dps[k].opc;
            kind = a32_dps[k].kind;
            s = mn[3] == 's' || kind == 1;
        }
    if (opc < 0) return 1;

    /* the operands: rd and rn by kind; `add r0, r1` is add r0, r0, r1 */
    if (n < 3) FAIL("%s takes %s", mn, kind == 3 ? "rd, rn and an operand" : "two operands");
    if (kind == 1) {
        rn = tok_reg(&t[1]);
        at = 2;
        if (rn < 0) FAIL("%s's first operand is a register", mn);
    } else if (kind == 2) {
        rd = tok_reg(&t[1]);
        at = 2;
        if (rd < 0) FAIL("%s's destination is a register", mn);
    } else {
        int two;
        rd = tok_reg(&t[1]);
        if (rd < 0) FAIL("%s's destination is a register", mn);
        two = n == 3 || (n >= 4 && (tok_is(&t[3], "lsl") || tok_is(&t[3], "lsr") ||
                                    tok_is(&t[3], "asr") || tok_is(&t[3], "ror") ||
                                    tok_is(&t[3], "rrx")));
        if (two) { rn = rd; at = 2; }
        else {
            rn = tok_reg(&t[2]);
            if (rn < 0) FAIL("%s's first source is a register", mn);
            at = 3;
        }
    }
    if (at >= n) FAIL("%s needs an operand", mn);
    if (t[at].s[0] == '#' || tok_reg(&t[at]) < 0) {
        long v;
        int imm;
        if (at + 1 != n || !tok_imm(&t[at], &v))
            FAIL("\"%.*s\" is not a register or a constant", t[at].len, t[at].s);
        imm = a32_encode_imm((unsigned long)v & 0xffffffffUL);
        if (imm < 0) {
            unsigned long tv;
            int topc = a32_twin(opc, (unsigned long)v & 0xffffffffUL, &tv);
            int ti = topc >= 0 ? a32_encode_imm(tv) : -1;
            if (ti >= 0) {
                opc = topc;
                imm = ti;
            } else if (opc == 13 && !s && v >= 0 && v <= 0xffff) {
                /* mov rd, #imm16: movw */
                a32_raw_word(out, 0x03000000UL | ((unsigned long)v >> 12) << 16 |
                                  (unsigned long)rd << 12 | ((unsigned long)v & 0xfff));
                return 0;
            } else
                FAIL("%s: %ld is not an 8-bit constant rotated by an even "
                     "amount%s", mn, v, topc >= 0 ? ", nor is its twin's" : "");
        }
        w = 0x02000000UL | (unsigned long)imm;
    } else {
        used = a32_regop(t, at, n, &op2, 0, &why);
        if (used < 0) FAIL("%s: %s", mn, why);
        if (used == 0 || at + used != n)
            FAIL("\"%.*s\" is not an operand %s takes", t[at].len, t[at].s, mn);
        if ((op2 & 0x10u) && ((kind != 1 && rd == 15) || (kind != 2 && rn == 15)))
            FAIL("%s: pc cannot take part in a register-controlled shift", mn);
        w = op2;
    }
    w |= (unsigned long)opc << 21 | (unsigned long)s << 20 |
         (unsigned long)(kind == 2 ? 0 : rn) << 16 |
         (unsigned long)(kind == 1 ? 0 : rd) << 12;
    a32_raw_word(out, w);
    return 0;
}

/* ---- loads and stores ---- */

struct a32_memop { const char *name; int fam, load, b, t; unsigned long bits; };
/* fam 0: word/byte (imm12, shifted index); 1: halfword, signed, dual
 * (imm8, plain index), `bits` the S H pair at 6:5; t: the unprivileged
 * (post-indexed only) form */
static const struct a32_memop a32_mems[] = {
    { "ldr", 0, 1, 0, 0, 0 }, { "str", 0, 0, 0, 0, 0 },
    { "ldrb", 0, 1, 1, 0, 0 }, { "strb", 0, 0, 1, 0, 0 },
    { "ldrt", 0, 1, 0, 1, 0 }, { "strt", 0, 0, 0, 1, 0 },
    { "ldrbt", 0, 1, 1, 1, 0 }, { "strbt", 0, 0, 1, 1, 0 },
    { "ldrh", 1, 1, 0, 0, 0x20 }, { "strh", 1, 0, 0, 0, 0x20 },
    { "ldrsb", 1, 1, 0, 0, 0x40 }, { "ldrsh", 1, 1, 0, 0, 0x60 },
    { "ldrd", 1, 0, 0, 0, 0x40 }, { "strd", 1, 0, 0, 0, 0x60 },
    { "ldrht", 1, 1, 0, 1, 0x20 }, { "strht", 1, 0, 0, 1, 0x20 },
    { "ldrsbt", 1, 1, 0, 1, 0x40 }, { "ldrsht", 1, 1, 0, 1, 0x60 },
    { NULL, 0, 0, 0, 0, 0 }
};

/* `#-0`: a negative zero, which the U bit keeps */
static int tok_negzero(const struct tok *t)
{
    int i = t->s[0] == '#';
    return t->len == i + 2 && t->s[i] == '-' && t->s[i + 1] == '0';
}

static int a32_mem(const struct tok *t, int n, struct code *out, char *err,
                   int errlen)
{
    char mn[12];
    const struct a32_memop *e = NULL;
    int rt, rt2 = -1, rn, at, p = 1, wb = 0, u = 1, reg = 0, dual;
    unsigned long off = 0, w;
    const char *why = NULL;
    if (t[0].len >= (int)sizeof mn) return 1;
    for (int i = 0; i < t[0].len; i++) mn[i] = (char)tolower((unsigned char)t[0].s[i]);
    mn[t[0].len] = 0;
    for (int k = 0; a32_mems[k].name; k++)
        if (!strcmp(mn, a32_mems[k].name)) e = &a32_mems[k];
    if (!e) return 1;
    dual = !strcmp(mn, "ldrd") || !strcmp(mn, "strd");
    if (n < 3 || (rt = tok_reg(&t[1])) < 0) FAIL("%s takes a register and an address", mn);
    at = 2;
    if (dual) {
        if ((rt2 = tok_reg(&t[2])) >= 0) at = 3;
        else rt2 = rt + 1;
        if ((rt & 1) || rt == 14 || rt2 != rt + 1)
            FAIL("%s: in ARM state the pair is an even register and the "
                 "next (r0, r1), not lr", mn);
    }
    if (at >= n || t[at].s[0] != '[')
        return 1;               /* `ldr r0, =value`, and the like: not here */
    {
        const struct tok *a = &t[at];
        int len = a->len, nin;
        struct tok in[6];
        if (a->s[len - 1] == '!') { wb = 1; len--; }
        if (a->s[len - 1] != ']') FAIL("\"%.*s\" is not an address", a->len, a->s);
        nin = split(a->s + 1, len - 2, in, 6);
        if (nin < 1 || (rn = tok_reg(&in[0])) < 0)
            FAIL("\"%.*s\" is not an address", a->len, a->s);
        if (at + 1 < n) {
            /* post-indexed: [rn], #off | [rn], +-rm{, shift} */
            if (nin != 1 || wb || n - at - 1 > 4)
                FAIL("\"%.*s\": a post-indexed address is [rn], then the offset",
                     a->len, a->s);
            p = 0;
            memcpy(in + 1, t + at + 1, (size_t)(n - at - 1) * sizeof *in);
            nin = 1 + n - at - 1;
        } else if (e->t) {
            p = 0;      /* ldrt r0, [r1]: post-indexed by nothing */
            if (nin != 1 || wb) FAIL("%s's address is post-indexed: [rn], #off", mn);
        }
        if (nin == 1 && e->fam == 1)
            off = 0x400000u;    /* [rn]: the immediate form, offset 0 */
        if (nin > 1) {
            struct tok x = in[1];
            if (x.s[0] == '#') {
                long v;
                if (nin != 2 || !tok_imm(&x, &v))
                    FAIL("\"%.*s\" is not an offset", x.len, x.s);
                if (v < 0 || tok_negzero(&x)) { u = 0; v = -v; }
                if (e->fam == 0 ? v > 4095 : v > 255)
                    FAIL("%s's offset is within +-%d", mn, e->fam == 0 ? 4095 : 255);
                off = e->fam == 0 ? (unsigned long)v
                    : ((unsigned long)v & 0xf0u) << 4 | ((unsigned long)v & 15u) | 0x400000u;
            } else {
                int used;
                if (x.s[0] == '-' || x.s[0] == '+') {
                    u = x.s[0] == '+';
                    x.s++; x.len--;
                    in[1] = x;
                }
                used = a32_regop(in, 1, nin, &off, 1, &why);
                if (used < 0) FAIL("%s: %s", mn, why);
                if (used == 0 || 1 + used != nin)
                    FAIL("\"%.*s\" is not an index", in[1].len, in[1].s);
                if (e->fam == 1 && (off & ~15UL))
                    FAIL("%s's index register takes no shift", mn);
                if ((off & 15) == 15) FAIL("%s: pc cannot be the index", mn);
                reg = 1;
            }
        }
    }
    if ((wb || !p) && rn == 15) FAIL("%s cannot write back to pc", mn);
    if (rt == 15 && (e->b || e->fam == 1))
        FAIL("%s cannot transfer pc", mn);
    if ((wb || !p) && (rn == rt || rn == rt2))
        FAIL("%s: the base written back cannot be the register %s", mn,
             e->load ? "loaded" : "stored");
    if (e->fam == 0)
        w = 0x04000000UL | (unsigned long)reg << 25 | (unsigned long)p << 24 |
            (unsigned long)u << 23 | (unsigned long)e->b << 22 |
            (unsigned long)(wb || e->t) << 21 | (unsigned long)e->load << 20 | off;
    else
        w = 0x00000090UL | (unsigned long)p << 24 | (unsigned long)u << 23 |
            (unsigned long)(wb || e->t) << 21 | (unsigned long)e->load << 20 |
            e->bits | off;
    w |= (unsigned long)rn << 16 | (unsigned long)rt << 12;
    a32_raw_word(out, w);
    return 0;
}

/* ldm/stm, every mode, writeback and ^; push and pop */
static int a32_multi(const struct tok *t, int n, struct code *out, char *err,
                     int errlen)
{
    static const struct { const char *name; int load, p, u; } mm[] = {
        { "ldm", 1, 0, 1 }, { "ldmia", 1, 0, 1 }, { "ldmfd", 1, 0, 1 },
        { "ldmib", 1, 1, 1 }, { "ldmed", 1, 1, 1 },
        { "ldmda", 1, 0, 0 }, { "ldmfa", 1, 0, 0 },
        { "ldmdb", 1, 1, 0 }, { "ldmea", 1, 1, 0 },
        { "stm", 0, 0, 1 }, { "stmia", 0, 0, 1 }, { "stmea", 0, 0, 1 },
        { "stmib", 0, 1, 1 }, { "stmfa", 0, 1, 1 },
        { "stmda", 0, 0, 0 }, { "stmed", 0, 0, 0 },
        { "stmdb", 0, 1, 0 }, { "stmfd", 0, 1, 0 },
        { "push", 0, 1, 0 }, { "pop", 1, 0, 1 }, { NULL, 0, 0, 0 }
    };
    char mn[8];
    int k, rn = 13, wb = 1, user = 0;
    unsigned mask;
    struct tok lt;
    if (t[0].len >= (int)sizeof mn) return 1;
    for (int i = 0; i < t[0].len; i++) mn[i] = (char)tolower((unsigned char)t[0].s[i]);
    mn[t[0].len] = 0;
    for (k = 0; mm[k].name && strcmp(mm[k].name, mn); k++) {}
    if (!mm[k].name) return 1;
    if (mn[0] == 'p') {
        if (n != 2) FAIL("%s takes a register list", mn);
        lt = t[1];
    } else {
        if (n != 3) FAIL("%s takes a base register and a list", mn);
        wb = t[1].len > 1 && t[1].s[t[1].len - 1] == '!';
        rn = tasm_gpr(t[1].s, t[1].len - wb);
        if (rn < 0 || rn == 15) FAIL("%s takes a base register (not pc) and a list", mn);
        lt = t[2];
    }
    if (lt.len > 1 && lt.s[lt.len - 1] == '^') { user = 1; lt.len--; }
    if (!tok_reglist(&lt, &mask))
        FAIL("%s takes a register list: {r4-r11, lr}", mn);
    if (user && mn[0] == 'p') FAIL("%s takes no ^", mn);
    /* one register: push and pop are a str/ldr with writeback, as LLVM
     * writes them */
    if (mn[0] == 'p' && !(mask & (mask - 1))) {
        int r = 0;
        while (!(mask & (1u << r))) r++;
        if (r == 13) FAIL("%s cannot transfer sp", mn);
        a32_raw_word(out, mm[k].load ? 0x049D0004UL | (unsigned long)r << 12
                                     : 0x052D0004UL | (unsigned long)r << 12);
        return 0;
    }
    if (user && wb && !(mm[k].load && (mask & 0x8000u)))
        FAIL("%s: the user registers (^) are transferred without writeback", mn);
    if (wb && (mask & (1u << rn)) && mm[k].load)
        FAIL("%s cannot load its base when it writes it back", mn);
    a32_raw_word(out, 0x08000000UL | (unsigned long)mm[k].p << 24 |
                      (unsigned long)mm[k].u << 23 | (unsigned long)user << 22 |
                      (unsigned long)wb << 21 | (unsigned long)mm[k].load << 20 |
                      (unsigned long)rn << 16 | mask);
    return 0;
}

/* dmb, dsb and isb with an option; never conditional */
static int a32_bar_stmt(const struct tok *t, int n, struct code *out, char *err,
                       int errlen)
{
    unsigned long base;
    long crm = 15;
    if (tok_is(&t[0], "dmb")) base = 0xF57FF050UL;
    else if (tok_is(&t[0], "dsb")) base = 0xF57FF040UL;
    else if (tok_is(&t[0], "isb")) base = 0xF57FF060UL;
    else return 1;
    if (n == 2) {
        crm = -1;
        for (int k = 0; t_baropt[k].name; k++)
            if (tok_is(&t[1], t_baropt[k].name)) crm = t_baropt[k].crm;
        if (crm < 0 && (!tok_imm(&t[1], &crm) || crm < 0 || crm > 15))
            FAIL("\"%.*s\" is not a barrier option", t[1].len, t[1].s);
        if (base == 0xF57FF060UL && crm != 15 && t[1].s[0] != '#')
            FAIL("isb has only the sy option");
        if (t[1].s[0] != '#' && (crm & 3) == 1)
            FAIL("\"%.*s\" is an ARMv8 barrier option", t[1].len, t[1].s);
    } else if (n != 1)
        FAIL("%.*s takes a barrier option", t[0].len, t[0].s);
    if (a32_it_open())
        FAIL("%.*s cannot be conditional", t[0].len, t[0].s);
    code_u32(out, base | (unsigned long)crm);
    return 0;
}

static int a32_stmt(const struct tok *t, int n, struct code *out, char *err,
                    int errlen)
{
    int r = a32_dp(t, n, out, err, errlen);
    if (r == 1) r = a32_mem(t, n, out, err, errlen);
    if (r == 1) r = a32_multi(t, n, out, err, errlen);
    if (r == 1) r = a32_bar_stmt(t, n, out, err, errlen);
    return r;
}

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
            cond = n == 2 ? cond_num(t[1].s, t[1].len) : -1;
            if (cond < 0)
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
        /* the slot is taken whatever happens below, so one bad
         * instruction is reported once and not again, as the wrong
         * condition, on every instruction after it */
        int slot = g_it.at++;
        int want = g_it.cond[slot], mlen = t[0].len, wlen = 0, c, r;
        char buf[256];
        /* the suffixes follow the condition: `addeq.w`, `vmoveq.f32` */
        {
            const char *dot = memchr(t[0].s, '.', (size_t)mlen);
            if (dot) { wlen = (int)(t[0].s + mlen - dot); mlen -= wlen; }
        }
        c = mlen > 2 ? cond_num(t[0].s + mlen - 2, 2) : -1;
        if (c != want)
            FAIL("inside an IT block, \"%.*s\" must carry the condition `%s`",
                 t[0].len, t[0].s, cond_name(want));
        mlen -= 2;
        {
            /* (a flag-setting data-processing instruction takes its
             * 32-bit form here, t2_dp's choice: inside a block the 16-bit
             * one sets no flags) */
            static const char *const refused[] = {
                "b", "bl", "cbz", "cbnz", NULL
            };
            /* B<c> and BL<c> may END an IT block: the condition is the
             * block's, and the branch takes its unconditional encoding --
             * the shape of a copy loop's `bge` after `ittt ge`. */
            int last = slot + 1 == g_it.n;
            for (int k = 0; refused[k]; k++)
                if ((int)strlen(refused[k]) == mlen &&
                    strncmp(t[0].s, refused[k], (size_t)mlen) == 0 &&
                    !(last && (!strcmp(refused[k], "b") ||
                               !strcmp(refused[k], "bl"))))
                    FAIL("\"%.*s\" is not supported inside an IT block: "
                         "%s", t[0].len, t[0].s,
                         refused[k][0] == 'b' || refused[k][0] == 'c'
                         ? "a branch there needs the unconditional encoding"
                         : "its 16-bit encoding sets no flags there");
            if (slot + 1 < g_it.n &&
                ((mlen == 2 && strncmp(t[0].s, "bx", 2) == 0) ||
                 (mlen == 3 && strncmp(t[0].s, "blx", 3) == 0)))
                FAIL("a branch must be the last instruction of its IT block");
        }
        snprintf(buf, sizeof buf, "%.*s%.*s%.*s", mlen, t[0].s, wlen,
                 t[0].s + t[0].len - wlen,
                 (int)(stmt + len - (t[0].s + t[0].len)), t[0].s + t[0].len);
        {
            int save_n = g_it.n, save_at = g_it.at;
            g_it.n = g_it.at = 0;       /* the inner call is outside it */
            g_in_it = 1;                /* ...but chooses encodings as in it */
            /* (the block has moved on whether this one assembled or not,
             * so one bad instruction is reported once, not again as the
             * wrong condition on every instruction after it) */
            r = one_stmt(buf, (int)strlen(buf), out, err, errlen);
            g_in_it = 0;
            g_it.n = save_n;
            g_it.at = save_at;
        }
        return r;
    }

    /* data processing, shifts, multiplies, table branches, barriers --
     * chosen as an assembler chooses them (t2_dp) -- in Thumb state */
    if (!t_isa_a32) {
        int r = t2_dp(t, n, out, err, errlen);
        if (r == 1)
            r = t2_misc(t, n, out, err, errlen);
        if (r != 1)
            return r;
    }
    /* ...and in ARM state, A32's own (a32_stmt) */
    if (t_isa_a32) {
        int r = a32_stmt(t, n, out, err, errlen);
        if (r != 1)
            return r;
    }
    {
        int r = vfp_stmt(t, n, out, err, errlen);
        if (r == 1)
            r = arm_adr(t, n, out, err, errlen);
        if (r != 1)
            return r;
    }
    {
        int r = v8m_stmt(t, n, out, err, errlen);
        if (r == 1)
            r = dsp_stmt(t, n, out, err, errlen);
        if (r == 1)
            r = more_stmt(t, n, out, err, errlen);
        if (r != 1)
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
        if (t_isa_a32)
            goto a32_cond;      /* `nopvc`: a condition, in ARM state */
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
        long opt = -1;
        /* `sy` by name, or its number 0xF, as CMSIS's __ISB writes it */
        if (n == 2 && !tok_is(&t[1], "sy") && !tok_imm(&t[1], &opt))
            opt = -1;
        if (n != 2 || (!tok_is(&t[1], "sy") && opt != 15))
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

    /* ---- ARMv7-A in ARM state: its own system vocabulary ----
     * The status register by name (cpsr/apsr, and the fields msr
     * writes), the coprocessor moves an A-profile kernel reaches the
     * system control registers with (VBAR, SCTLR, the MMU's), and svc /
     * bkpt with their wider A32 immediates. An M-profile name is refused
     * here rather than encoded as something else. */
    if (t_isa_a32) {
        int r = a32_one_stmt(t, n, out, err, errlen);
        if (r != 1)
            return r;
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
        if (arch_thumb1() && sysreg_main_only(sys))
            FAIL("\"%.*s\" is not a special register of %s: it is the "
                 "Main Extension's", t[2].len, t[2].s,
                 g_arch == 6 ? "ARMv6-M" : "ARMv8-M Baseline");
        t_mrs(out, rd, sys);
        return 0;
    }
    if (mnemonic_is(&t[0], "msr")) {
        int rn, sys;
        if (n != 3) FAIL("msr takes a special register and a register");
        /* the flags and the DSP extension's GE bits (mask 0b11) */
        if (tok_is(&t[1], "apsr_nzcvqg")) {
            rn = tok_reg(&t[2]);
            if (rn < 0) FAIL("\"%.*s\" is not a register", t[2].len, t[2].s);
            if (arch_thumb1())
                FAIL("apsr_nzcvqg: %s has no DSP extension, and no GE bits",
                     g_arch == 6 ? "ARMv6-M" : "ARMv8-M Baseline");
            t_msr_apsr(out, rn, 1);
            return 0;
        }
        sys = sysreg_num(t[1].s, t[1].len);
        rn = tok_reg(&t[2]);
        if (sys < 0) FAIL("\"%.*s\" is not an ARMv7-M special register",
                          t[1].len, t[1].s);
        if (arch_thumb1() && sysreg_main_only(sys))
            FAIL("\"%.*s\" is not a special register of %s: it is the "
                 "Main Extension's", t[1].len, t[1].s,
                 g_arch == 6 ? "ARMv6-M" : "ARMv8-M Baseline");
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
            /* [rn, #off]!: the offset added first, and written back */
            if (t[2].len > 1 && t[2].s[t[2].len - 1] == '!') {
                struct tok m = { t[2].s, t[2].len - 1 };
                if (!tok_mem(&m, &base, &off))
                    FAIL("\"%.*s\" is not a [reg, #off]! address",
                         t[2].len, t[2].s);
                if (!t_ldst_wb(out, rd, base, off, e->size, e->sign,
                               e->store, 1))
                    FAIL("%s %.*s: the offset must be -255..255 and the "
                         "base neither pc nor the data register", e->name,
                         t[2].len, t[2].s);
                return 0;
            }
            if (!tok_mem(&t[2], &base, &off)) {
                int rn, rm, sh;
                if (!tok_mem_reg(&t[2], &rn, &rm, &sh))
                    FAIL("\"%.*s\" is not a [reg], [reg, #off] or [reg, "
                         "reg{, lsl #n}] address", t[2].len, t[2].s);
                if (rm == 13 || rm == 15 || rn == 15)
                    FAIL("\"%.*s\": sp and pc cannot be the offset register",
                         t[2].len, t[2].s);
                t_ldst_reg(out, rd, rn, rm, sh, e->size, e->sign, e->store);
                return 0;
            }
            /* [pc, #off]: a literal, from Align(pc, 4) as the
             * architecture reads it. Words only. */
            if (base == 15) {
                int dotw = t[0].len > 2 && t[0].s[t[0].len - 2] == '.' &&
                           t[0].s[t[0].len - 1] == 'w';
                if (e->size != 4 || e->store)
                    FAIL("only ldr reads a pc-relative literal");
                /* the 16-bit form when it reaches -- r0-r7, a word
                 * 0..1020 ahead -- as the file assembler relaxes */
                if (!t_isa_a32 && !dotw && !g_force_wide) {
                    if (rd < 8 && off >= 0 && off <= 1020 && !(off & 3)) {
                        t16(out, 0x4800u | (unsigned)rd << 8 | (unsigned)off >> 2);
                        return 0;
                    }
                    g_went_wide = 1;
                }
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
            if (off && t_isa_a32)
                FAIL("ldrex takes no offset in ARM state");
            t_ldrex(out, rd, base, (int)off);
            return 0;
        }
    }

    /* ---- three-operand forms ---- */
    if (n == 4) {
        int rd = tok_reg(&t[1]), rn = tok_reg(&t[2]), rm = tok_reg(&t[3]);
        /* `ldr rd, [rn], #off`: the access at rn, then rn += off */
        for (const struct ls_ent *e = ls_tab; e->name; e++) {
            int base;
            long off, post;
            if (!mnemonic_is(&t[0], e->name))
                continue;
            if (rd < 0 || !tok_mem(&t[2], &base, &off) || off != 0 ||
                !tok_imm(&t[3], &post))
                FAIL("\"%.*s\" wants a register, a [reg] address and an "
                     "#offset", t[0].len, t[0].s);
            if (!t_ldst_wb(out, rd, base, post, e->size, e->sign, e->store,
                           0))
                FAIL("%s post-indexed: the offset must be -255..255 and the "
                     "base neither pc nor the data register", e->name);
            return 0;
        }
        for (const struct alu_ent *e = alu_tab; e->name; e++) {
            if (!mnemonic_is(&t[0], e->name))
                continue;
            if (rd < 0 || rn < 0) FAIL("%s wants registers", e->name);
            /* `add rdn, rdn, rm`, either way round: the 16-bit ADD
             * (register) that leaves the flags alone, as GNU as and
             * llvm-mc pick it unless told `.w` -- and the only
             * flag-preserving register add ARMv6-M has */
            if (rm >= 0 && e->op == T_OP_ADD && !e->s && !t_isa_a32 &&
                (rd == rn || rd == rm) && !(rn == 15 && rm == 15) &&
                !(t[0].len > 2 && t[0].s[t[0].len - 2] == '.' &&
                  t[0].s[t[0].len - 1] == 'w')) {
                t1_add_hi(out, rd, rd == rn ? rm : rn);
                return 0;
            }
            if (rm >= 0) { t_alu_reg(out, e->op, rd, rn, rm, e->s); return 0; }
            if (!tok_imm(&t[3], &imm))
                FAIL("%s wants a register or an immediate", e->name);
            /* sp's own 16-bit forms, as GNU as and llvm-mc choose them
             * unless told `.w`: `add|sub sp, sp, #imm7*4` and `add rd,
             * sp, #imm8*4`. The only ones ARMv6-M has -- the wide ones
             * this took instead are UNDEFINED there, and v6_scan
             * refused lib/rt's __aeabi_uldivmod for them. */
            if (!e->s && rn == 13 && imm >= 0 && !(imm & 3) &&
                !(t[0].len > 2 && t[0].s[t[0].len - 2] == '.' &&
                  t[0].s[t[0].len - 1] == 'w')) {
                if (rd == 13 && imm / 4 <= 127 &&
                    (e->op == T_OP_ADD || e->op == T_OP_SUB)) {
                    t_sp_adjust(out, imm, e->op == T_OP_SUB);
                    return 0;
                }
                if (e->op == T_OP_ADD && rd >= 0 && rd < 8 && imm / 4 <= 255) {
                    t_add_sp(out, rd, imm);
                    return 0;
                }
            }
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
            if (off && t_isa_a32)
                FAIL("strex takes no offset in ARM state");
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
        for (unsigned k = 0; k < sizeof bc / sizeof bc[0]; k++) {
            size_t bl = strlen(bc[k].name);
            int want = 0;
            if ((size_t)t[0].len == bl + 2 && !strncmp(t[0].s, bc[k].name, bl) &&
                t[0].s[bl] == '.' && (t[0].s[bl + 1] == 'w' || t[0].s[bl + 1] == 'n'))
                want = t[0].s[bl + 1];
            else if (!mnemonic_is(&t[0], bc[k].name))
                continue;
            long v;
            if (n != 2 || !tok_imm(&t[1], &v))
                FAIL("%s wants a branch offset", bc[k].name);
            if (v & 1) FAIL("%s offset %ld is odd", bc[k].name, v);
            return t_branch_relaxed(out, bc[k].cond, v, want, err, errlen);
        }
    }
    if ((mnemonic_is(&t[0], "b") || mnemonic_is(&t[0], "b.w") ||
         mnemonic_is(&t[0], "b.n")) && n == 2) {
        long v;
        if (!tok_imm(&t[1], &v)) FAIL("b wants a branch offset");
        if (v & 1) FAIL("b offset %ld is odd", v);
        return t_branch_relaxed(out, -1, v,
                                t[0].len == 3 ? t[0].s[2] : 0, err, errlen);
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
    if ((mnemonic_is(&t[0], "cbz") || mnemonic_is(&t[0], "cbnz")) && n == 3 &&
        t_isa_a32)
        FAIL("%.*s is a Thumb instruction; ARM state has none (cmp, then "
             "beq/bne)", t[0].len, t[0].s);
    if ((mnemonic_is(&t[0], "cbz") || mnemonic_is(&t[0], "cbnz")) && n == 3) {
        long v;
        int r = tok_reg(&t[1]);
        if (r < 0 || r > 7 || !tok_imm(&t[2], &v))
            FAIL("%.*s wants a register r0-r7 and a branch offset",
                 t[0].len, t[0].s);
        /* forward only, 4..130 bytes: there is no wider form to take */
        int at = t_cbz(out, mnemonic_is(&t[0], "cbnz"), r);
        if (!t_patch_cbz(out, at, at + (int)v))
            FAIL("%.*s reaches 4..130 bytes forward; this target is %ld "
                 "bytes away", t[0].len, t[0].s, v);
        return 0;
    }
    if ((mnemonic_is(&t[0], "bx") || mnemonic_is(&t[0], "blx")) && n == 2) {
        int r = tok_reg(&t[1]);
        if (r < 0) FAIL("%.*s wants a register", t[0].len, t[0].s);
        if (mnemonic_is(&t[0], "bx")) t_bx(out, r);
        else                          t_blx(out, r);
        return 0;
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

    /* `subs r3, #4`, `adds r4, #12`, `lsls r2, #2`, `orr r0, r1`: GNU
     * as's two-operand forms, the destination doubling as the first
     * source. Rewritten to three operands and assembled as those. */
    if (n == 3) {
        int is_alu = 0;
        for (const struct alu_ent *e = alu_tab; e->name && !is_alu; e++)
            if (mnemonic_is(&t[0], e->name)) is_alu = 1;
        for (const struct sh_ent *e = sh_tab; e->name && !is_alu; e++)
            if (mnemonic_is(&t[0], e->name)) is_alu = 1;
        /* `adds rdn, #imm8` / `subs rdn, #imm8`: the two-byte form with
         * an eight-bit immediate, which is what GNU as picks for this
         * spelling (the three-operand one takes only three bits) */
        if (is_alu && !t_isa_a32 && tok_reg(&t[1]) >= 0 &&
            tok_reg(&t[1]) <= 7 &&
            (mnemonic_is(&t[0], "adds") || mnemonic_is(&t[0], "subs")) &&
            tok_imm(&t[2], &imm) && imm >= 0 && imm <= 255) {
            unsigned h = (mnemonic_is(&t[0], "adds") ? 0x3000u : 0x3800u) |
                         (unsigned)(tok_reg(&t[1]) << 8) | (unsigned)imm;
            code_byte(out, (unsigned char)(h & 0xff));
            code_byte(out, (unsigned char)(h >> 8));
            return 0;
        }
        if (is_alu && tok_reg(&t[1]) >= 0) {
            char buf[256];
            snprintf(buf, sizeof buf, "%.*s %.*s, %.*s, %.*s", t[0].len,
                     t[0].s, t[1].len, t[1].s, t[1].len, t[1].s, t[2].len,
                     t[2].s);
            return one_stmt(buf, (int)strlen(buf), out, err, errlen);
        }
    }
    /* ARM state: any instruction may carry a condition, with no IT block
     * -- `moveq r0, #1`. Tried only once the mnemonic is not one of the
     * vocabulary's own, so `bics` stays BIC with S rather than becoming
     * `bi` under CS. The condition goes through the same queue an IT
     * block fills (a32_it). */
a32_cond:
    if (t_isa_a32 && t[0].len > 2 && !tasm_open()) {
        int mlen = t[0].len, wlen = 0, c;
        {
            const char *dot = memchr(t[0].s, '.', (size_t)mlen);
            if (dot) { wlen = (int)(t[0].s + mlen - dot); mlen -= wlen; }
        }
        c = mlen > 2 ? cond_num(t[0].s + mlen - 2, 2) : -1;
        if (c >= 0 && mlen > 2) {
            char buf[256];
            struct code probe = { 0 };
            int r;
            snprintf(buf, sizeof buf, "%.*s%.*s%.*s", mlen - 2, t[0].s,
                     wlen, t[0].s + t[0].len - wlen,
                     (int)(stmt + len - (t[0].s + t[0].len)),
                     t[0].s + t[0].len);
            /* assembled once to see that it is ONE instruction (a
             * condition on a sequence would cover only its first word),
             * then for real under the condition */
            r = one_stmt(buf, (int)strlen(buf), &probe, err, errlen);
            free(probe.p);
            if (r == 0 && probe.len != 4)
                FAIL("\"%.*s\" is more than one instruction, and a "
                     "condition cannot cover them", t[0].len, t[0].s);
            if (r == 0) {
                t_it(out, c, "");
                r = one_stmt(buf, (int)strlen(buf), out, err, errlen);
                if (r == 0 && a32_it_open())
                    FAIL("\"%.*s\" took no condition", t[0].len, t[0].s);
                a32_it_reset();
            }
            return r;
        }
    }
    FAIL("asm instruction \"%.*s\" is not in the %s vocabulary",
         t[0].len, t[0].s, t_isa_a32 ? "ARMv7-A" : "ARMv7-M");
}

/* Did the statement just written at out->p[at..] produce a 32-bit
 * instruction a Thumb-1 level's core lacks (t_thumb1_ok32), or an IT --
 * or on ARMv6-M a CBZ? An instruction this assembler would otherwise widen
 * silently, as `adds r0, r1, #200` does to adds.w, is refused here by
 * name rather than becoming a HardFault on a Cortex-M0 or M23. */
static int thumb1_bad(const struct code *c, int at)
{
    while (at + 1 < c->len) {
        unsigned h = (unsigned)(c->p[at] | c->p[at + 1] << 8);
        if ((h >> 11) >= 0x1d) {
            unsigned h2 = at + 3 < c->len
                ? (unsigned)(c->p[at + 2] | c->p[at + 3] << 8) : 0;
            if (!t_thumb1_ok32(h, h2, g_arch == TASM_V8M_BASE))
                return 1;
            at += 4;
            continue;
        }
        if (((h & 0xff00u) == 0xbf00u && (h & 0xfu)) ||
            (g_arch == 6 && (h & 0xf500u) == 0xb100u))
            return 1;
        at += 2;
    }
    return 0;
}

/* ---- data directives ------------------------------------------------------
 *
 * `.short 0xbf30` or `.word 0x...` in a template: an instruction this
 * vocabulary does not have yet, written as its bytes, or a constant the
 * code finds beside itself. GNU as's ARM spellings and sizes, which are
 * llvm-mc's: `.byte` one; `.short`, `.hword`, `.2byte` two; `.word`,
 * `.long`, `.int`, `.4byte` four; `.quad`, `.8byte` eight -- not RISC-V's
 * `.half` and `.dword`, which no ARM assembler takes. Little-endian, so a
 * 32-bit Thumb instruction is two `.short`s, its first halfword first.
 * Constants only, each in its width's signed or unsigned range, as
 * llvm-mc refuses the rest; a symbol would need a relocation an inline
 * asm cannot carry. A .s file's are src/as/gas.c's and never reach this.
 * 0 when stmt is not one, 1 when emitted, -1 with err set. */
static const struct { const char *name; int size; } data_dir[] = {
    { ".byte", 1 },
    { ".short", 2 }, { ".hword", 2 }, { ".2byte", 2 },
    { ".word", 4 }, { ".long", 4 }, { ".int", 4 }, { ".4byte", 4 },
    { ".quad", 8 }, { ".8byte", 8 },
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
        if ((int)strlen(data_dir[k].name) == m - d &&
            strncmp(stmt + d, data_dir[k].name, (size_t)(m - d)) == 0)
            size = data_dir[k].size;
    if (!size)
        return 0;
    /* data is not an instruction an IT block's condition can apply to */
    if (tasm_open())
        FAIL("%.*s inside an IT block: its slots are for instructions",
             m - d, stmt + d);
    int at = out->len;
    for (i = m; i < len; ) {
        int s0, e, depth = 0;
        long long v;
        while (i < len && isspace((unsigned char)stmt[i]))
            i++;
        if (i >= len)
            break;
        for (s0 = i; i < len && (depth > 0 || stmt[i] != ','); i++) {
            if (stmt[i] == '(') depth++;
            else if (stmt[i] == ')') depth--;
        }
        for (e = i; e > s0 && isspace((unsigned char)stmt[e - 1]); e--) {}
        if (e == s0 || !asm_const_expr(stmt + s0, e - s0, &v))
            FAIL("\"%.*s\" in %.*s is not a constant: inline asm data takes "
                 "numbers, and a symbol would need a relocation",
                 e - s0, stmt + s0, m - d, stmt + d);
        if (size < 8 && (v < -(1LL << (8 * size - 1)) ||
                         v > (long long)((1ULL << (8 * size)) - 1)))
            FAIL("%lld does not fit in %d byte%s", v, size,
                 size == 1 ? "" : "s");
        for (int b = 0; b < size; b++)
            code_byte(out, (int)(((unsigned long long)v >> (8 * b)) & 0xff));
        if (i < len)
            i++;                        /* the comma */
    }
    /* data, so the ARMv6-M backend's scan passes over it (ir_asm.drange) */
    code_mark_data(out, at, out->len);
    return 1;
}

int tasm_assemble(const char *text, struct code *out, char *err, int errlen)
{
    const char *p = text;
    err[0] = 0;
    while (*p) {
        const char *start = p;
        int len;
        /* a `;` or an `@` inside a .ascii string is the string's */
        p += asm_stmt_len(p, ";");
        len = asm_cut_comment(start, (int)(p - start), "@", 1);
        /* Mnemonics are not case-sensitive in GNU as -- CMSIS writes
         * `MRS %0, primask` -- so the statement's first word is taken in
         * lower case; operands keep theirs (a symbol's case matters). */
        {
            char buf[512];
            const char *st = start;
            int k = 0, r;
            int at = out->len;
            if (len < (int)sizeof buf) {
                memcpy(buf, start, (size_t)len);
                while (k < len && isspace((unsigned char)buf[k])) k++;
                while (k < len && !isspace((unsigned char)buf[k])) {
                    buf[k] = (char)tolower((unsigned char)buf[k]);
                    k++;
                }
                st = buf;
            }
            /* data is bytes, not an instruction: it goes in as written,
             * past thumb1_bad, which would read a `.short` of a Thumb-2
             * halfword as an instruction ARMv6-M lacks */
            r = data_stmt(st, len, out, err, errlen);
            if (!r) {
                /* .ascii/.asciz/.string, and the alignments, whose
                 * padding the backend decides where the bytes land */
                static const struct asm_dirs dirs = { NULL, 0, 0 };
                int w = 0;
                while (w < len && isspace((unsigned char)st[w]))
                    w++;
                if (w < len && st[w] == '.' && tasm_open())
                    FAIL("%.*s inside an IT block: its slots are for "
                         "instructions", len - w, st + w);
                r = code_asm_directive(st, len, out, &dirs, err, errlen);
            }
            if (r) {
                if (r < 0)
                    return -1;
                if (*p)
                    p++;
                continue;
            }
            r = one_stmt(st, len, out, err, errlen);
            if (r != 0)
                return -1;
            if (arch_thumb1() && !t_isa_a32 && thumb1_bad(out, at)) {
                snprintf(err, (size_t)errlen, "\"%.*s\" is not an %s "
                         "instruction: it encodes as a 32-bit Thumb-2 form "
                         "that core does not have", len, start,
                         g_arch == 6 ? "ARMv6-M" : "ARMv8-M Baseline");
                return -1;
            }
        }
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
    if (sreg(w, wlen) >= 0 || vfp_word(w, wlen))
        return 1;
    /* a shift inside an operand: `[r1, r2, lsl #2]`, `r3, lsr #4` */
    if ((wlen == 3 && (same_nocase(w, "lsl", 3) || same_nocase(w, "lsr", 3) ||
                       same_nocase(w, "asr", 3) || same_nocase(w, "ror", 3) ||
                       same_nocase(w, "rrx", 3))))
        return 1;
    if ((mlen >= 3 && (strncmp(m, "mrs", 3) == 0 || strncmp(m, "msr", 3) == 0))
        && sysreg_num(w, wlen) >= 0)
        return 1;
    /* ARM state: the status register's names, and the coprocessor and
     * its registers (`mrc p15, #0, r0, c1, c0, #0`) */
    if (t_isa_a32 && mlen >= 3 &&
        (strncmp(m, "mrs", 3) == 0 || strncmp(m, "msr", 3) == 0)) {
        struct tok tw;
        tw.s = w;
        tw.len = wlen;
        if (psr_fields(&tw) >= 0)
            return 1;
    }
    if (t_isa_a32 && mlen >= 3 &&
        (strncmp(m, "mrc", 3) == 0 || strncmp(m, "mcr", 3) == 0)) {
        struct tok tw;
        tw.s = w;
        tw.len = wlen;
        if (tok_cnum(&tw, 'p') >= 0 || tok_cnum(&tw, 'c') >= 0)
            return 1;
    }
    if (mlen >= 5 && strncmp(m, "cps", 3) == 0) {
        int ok = wlen > 0 && wlen <= 2;
        for (int k = 0; k < wlen; k++)
            if (w[k] != 'i' && w[k] != 'f') ok = 0;
        if (ok) return 1;
    }
    if (mlen >= 2 && mlen <= 5 && m[0] == 'i' && m[1] == 't' &&
        cond_num(w, wlen) >= 0)
        return 1;
    /* `dmb ish`, and inside an IT block `dmbeq sy` */
    if ((mlen == 3 || (mlen == 5 && cond_num(m + 3, 2) >= 0)) &&
        (strncmp(m, "dsb", 3) == 0 || strncmp(m, "dmb", 3) == 0 ||
         strncmp(m, "isb", 3) == 0))
        for (int k = 0; t_baropt[k].name; k++)
            if ((int)strlen(t_baropt[k].name) == wlen &&
                same_nocase(w, t_baropt[k].name, wlen))
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
    if (!asm_sym0((unsigned char)p[0]))
        return 0;
    while (asm_symc((unsigned char)p[n]))
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
        f->site[0].off = 0;
        f->site[0].reloc = t_isa_a32 ? R_ARM_MOVW_ABS_NC : R_ARM_THM_MOVW_ABS_NC;
        f->site[1].off = 4;
        f->site[1].reloc = t_isa_a32 ? R_ARM_MOVT_ABS : R_ARM_THM_MOVT_ABS;
        f->nsites = 2;
        return 1;
    }
    /* inside an IT block the pair is conditional: `movweq r0, :lower16:x` */
    if ((mnlen == 4 || (mnlen == 6 && cond_num(mn + 4, 2) >= 0)) &&
        (strncmp(mn, "movw", 4) == 0 || strncmp(mn, "movt", 4) == 0)) {
        int top = mn[3] == 't';
        /* `#:lower16:sym`, or clang's `:lower16:sym` without the hash */
        const char *want = top ? ":upper16:" : ":lower16:";
        if (*q == '#') q++;
        if (strncmp(q, want, 9) != 0) return 0;
        q += 9;
        if (!(n = sym_operand(q, &f->addend, &end)) || *end) return 0;
        f->sym_at = (int)(q - stmt); f->sym_len = n;
        snprintf(f->encode, sizeof f->encode, "%.*s %.*s, #0", mnlen, mn, rdlen, rd);
        f->site[0].off = 0;
        f->site[0].reloc = t_isa_a32
            ? (top ? R_ARM_MOVT_ABS : R_ARM_MOVW_ABS_NC)
            : (top ? R_ARM_THM_MOVT_ABS : R_ARM_THM_MOVW_ABS_NC);
        f->nsites = 1;
        return 1;
    }
    return 0;
}

/* ---- the referee's input ------------------------------------------------ */

/* ARMv8-M's additions, for tests/golden/thumbv8m-asm.sh: with `base` 0
 * every one Mainline has beyond tasm_vocabulary's, with `base` 1 EVERY
 * 32-bit instruction ARMv8-M Baseline has (and its 16-bit bxns/blxns),
 * across the registers each field takes -- the branches excepted, which
 * need labels and which the test writes itself. The stack-limit and the
 * Non-secure special registers come from the table above, less the Main
 * Extension's at Baseline. */
static const char *const v8_regs[] = {
    "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10",
    "r11", "r12", "lr", NULL
};

void tasm_vocabulary_v8m(FILE *f, int base)
{
    for (const struct xa_ent *e = xa_tab; e->name; e++)
        for (int a = 0; v8_regs[a]; a++)
            for (int b = 0; v8_regs[b]; b++) {
                if (e->kind != 1 && e->kind != 5) {
                    fprintf(f, "\t%s %s, [%s]\n", e->name, v8_regs[a],
                            v8_regs[b]);
                    continue;
                }
                /* the status register, rotated through the rest */
                int d = (a + b + 1) % 14;
                if (d == a || d == b)
                    d = (d + 1) % 14;
                if (d == a || d == b)
                    d = (d + 1) % 14;
                fprintf(f, "\t%s %s, %s, [%s]\n", e->name, v8_regs[d],
                        v8_regs[a], v8_regs[b]);
            }
    for (int a = 0; v8_regs[a]; a++)
        for (int b = 0; v8_regs[b]; b++)
            fprintf(f, "\ttt %s, %s\n\tttt %s, %s\n\ttta %s, %s\n"
                       "\tttat %s, %s\n", v8_regs[a], v8_regs[b],
                    v8_regs[a], v8_regs[b], v8_regs[a], v8_regs[b],
                    v8_regs[a], v8_regs[b]);
    fprintf(f, "\tsg\n");
    for (int a = 0; v8_regs[a]; a++)
        fprintf(f, "\tbxns %s\n\tblxns %s\n", v8_regs[a], v8_regs[a]);
    for (const struct sysreg *r = sysregs_v8m; r->name; r++) {
        if (base && sysreg_main_only(r->sysm))
            continue;
        fprintf(f, "\tmrs r3, %s\n\tmsr %s, r4\n", r->name, r->name);
    }
    if (!base) {
        fprintf(f, "\tvlstm sp\n\tvlldm sp\n\tvlstm r4\n\tvlldm r12\n");
        return;
    }
    /* the rest of Baseline's 32-bit set */
    for (const struct sysreg *r = sysregs; r->name; r++) {
        if (sysreg_main_only(r->sysm))
            continue;
        fprintf(f, "\tmrs r5, %s\n\tmsr %s, r6\n", r->name, r->name);
    }
    fprintf(f, "\tdsb sy\n\tdmb sy\n\tisb sy\n\tclrex\n");
    for (int a = 0; v8_regs[a]; a++) {
        int b = (a + 3) % 14, c = (a + 7) % 14;
        fprintf(f, "\tsdiv %s, %s, %s\n\tudiv %s, %s, %s\n",
                v8_regs[a], v8_regs[b], v8_regs[c],
                v8_regs[c], v8_regs[a], v8_regs[b]);
        fprintf(f, "\tmovw %s, #%d\n\tmovt %s, #%d\n\tmovw %s, #65535\n",
                v8_regs[a], 0x1234 + 0x1111 * a, v8_regs[b], 0xfedc - 0x123 * a,
                v8_regs[c]);
        fprintf(f, "\tldrex %s, [%s]\n\tldrex %s, [%s, #%d]\n",
                v8_regs[a], v8_regs[b], v8_regs[c], v8_regs[a], 4 * (a * 18 % 256));
        fprintf(f, "\tstrex %s, %s, [%s]\n\tstrex %s, %s, [%s, #1020]\n",
                v8_regs[a], v8_regs[b], v8_regs[c],
                v8_regs[b], v8_regs[c], v8_regs[a]);
    }
}

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
    fprintf(f, "\tldrexb r6, [r7]\n\tldrexh r8, [r9]\n");
    fprintf(f, "\tstrexb r0, r1, [r2]\n\tstrexh r10, r11, [r12]\n\tclrex\n");
    /* What an RTOS's context switch is made of. The multiple transfers
     * name high registers or the forms with no 16-bit encoding, which is
     * the only case where llvm-mc and this assembler (always 32-bit) pick
     * the same bytes. `ldr rd, =value` is left out for the reason `mov
     * rd, #imm` is: llvm-mc loads it from a literal pool, this assembler
     * with movw/movt, and both are right. */
    fprintf(f, "\tsvc #0\n\tsvc #171\n\ttst lr, #16\n\ttst r3, #0xff00\n");
    /* sp's narrow forms, and the wide ones .w or a register past r7 asks */
    fprintf(f, "\tsub sp, sp, #16\n\tadd sp, sp, #508\n\tadd r4, sp, #8\n"
               "\tadd r7, sp, #1020\n\tsub.w sp, sp, #16\n\tadd r9, sp, #8\n"
               "\tsub sp, sp, #512\n");
    /* ADD (register)'s narrow form, which sets no flags: the destination
     * is either source, or the two-operand spelling; .w asks the wide */
    fprintf(f, "\tadd r3, r3, r4\n\tadd r5, r6, r5\n\tadd r2, r7\n"
               "\tadd r8, r8, r1\n\tadd sp, sp, r2\n\tadd r0, r0, sp\n"
               "\tadd.w r6, r6, r1\n");
    fprintf(f, "\tpush {r4-r7, lr}\n\tpop {r4-r7, pc}\n");
    fprintf(f, "\tpush {r4-r11, lr}\n\tpop {r4-r11, pc}\n");
    fprintf(f, "\tstmdb r0!, {r4-r11, lr}\n\tldmia r0!, {r4-r11, lr}\n");
    fprintf(f, "\tstmfd r1!, {r2, r3}\n\tldmfd r9!, {r2, r3}\n");
    fprintf(f, "\tstmia r2, {r8-r11}\n\tstm r6, {r8, r9}\n");
    fprintf(f, "\tldmdb r5, {r8, pc}\n\tldm r7, {r0, r10}\n");
    /* ...and the 16-bit forms, low registers written back (lib/libc's
     * ARMv6-M setjmp) */
    fprintf(f, "\tstmia r0!, {r4-r7}\n\tldmia r0!, {r2-r7}\n");
    fprintf(f, "\tldmia r1, {r1-r3}\n\tstm r6!, {r0}\n");
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
    /* The data directives, last: each spelling, both ends of each
     * width's range and an expression -- 52 bytes, so whatever follows
     * stays aligned. */
    tasm_vocabulary_data(f);
}

/* The data directives alone, which every level takes the same */
void tasm_vocabulary_data(FILE *f)
{
    fprintf(f, "\t.byte 255, -128, 0x7f, 0\n\t.short -32768, 0xffff\n");
    fprintf(f, "\t.hword 0xe92d, 0x4ff0\n\t.2byte 0x1234, 7\n");
    fprintf(f, "\t.word 0x12345678, -2147483648\n\t.long 4294967295\n");
    fprintf(f, "\t.int (1 << 20) | 0x13\n\t.4byte 0xbf30bf20\n");
    fprintf(f, "\t.quad 0x123456789abcdef0\n\t.8byte -1\n");
}

/* The DSP extension's vocabulary, ssat/usat and the extends, for
 * tests/golden/thumb-dsp.sh: every entry of par_kind x par_op and of
 * dsp_tab, with each of r0-r12 and lr in every register field (rotated so
 * the fields differ), and the immediates at their ends and between. */
void tasm_vocabulary_dsp(FILE *f)
{
    static const char *const sat_sh[] = {
        "", ", lsl #0", ", lsl #1", ", lsl #17", ", lsl #31", ", asr #1",
        ", asr #16", ", asr #31", NULL
    };
    static const char *const rot[] = {
        "", ", ror #8", ", ror #16", ", ror #24", NULL
    };
    for (int k = 0; par_kind[k].name; k++)
        for (int o = 0; par_op[o].name; o++)
            for (int a = 0; v8_regs[a]; a++)
                fprintf(f, "\t%s%s %s, %s, %s\n", par_kind[k].name,
                        par_op[o].name, v8_regs[a], v8_regs[(a + 5) % 14],
                        v8_regs[(a + 9) % 14]);
    for (const struct dsp_ent *e = dsp_tab; e->name; e++)
        for (int a = 0; v8_regs[a]; a++) {
            const char *rd = v8_regs[a], *rn = v8_regs[(a + 5) % 14];
            const char *rm = v8_regs[(a + 9) % 14], *ra = v8_regs[(a + 3) % 14];
            switch (e->form) {
            case DF_EXT:
                fprintf(f, "\t%s %s, %s%s\n", e->name, rd, rm, rot[a % 4]);
                break;
            case DF_EXTA:
                fprintf(f, "\t%s %s, %s, %s%s\n", e->name, rd, rn, rm,
                        rot[a % 4]);
                break;
            case DF_SAT:
                fprintf(f, "\t%s %s, #%d, %s%s\n", e->name, rd,
                        e->b ? 1 + a * 31 / 13 : a * 31 / 13, rm,
                        sat_sh[a % 8]);
                break;
            case DF_SAT16:
                fprintf(f, "\t%s %s, #%d, %s\n", e->name, rd,
                        e->b ? 1 + a * 15 / 13 : a * 15 / 13, rm);
                break;
            case DF_PKH:
                if (a % 4 == 0)
                    fprintf(f, "\t%s %s, %s, %s\n", e->name, rd, rn, rm);
                else
                    fprintf(f, "\t%s %s, %s, %s, %s #%d\n", e->name, rd, rn,
                            rm, e->a ? "asr" : "lsl",
                            e->a ? 1 + (a * 31) / 13 : (a * 31) / 13);
                break;
            case DF_SMLALXY: case DF_SMLALD:
                fprintf(f, "\t%s %s, %s, %s, %s\n", e->name, rd,
                        v8_regs[(a + 1) % 14], rn, rm);
                break;
            default:
                if (e->acc)
                    fprintf(f, "\t%s %s, %s, %s, %s\n", e->name, rd, rn, rm,
                            ra);
                else
                    fprintf(f, "\t%s %s, %s, %s\n", e->name, rd, rn, rm);
                break;
            }
        }
    /* the plain extends' narrow form: low registers, no rotation */
    fprintf(f, "\tsxtb r0, r7\n\tuxtb r6, r1\n\tsxth r3, r4\n\tuxth r7, r7\n");
    fprintf(f, "\tsxtb r0, r1, ror #0\n\tpkhbt r1, r2, r3, lsl #0\n");
    if (!t_isa_a32)
        return;
    /* ARM state's own: sp as an operand, the saturates' asr #32, and a
     * condition on the instruction itself */
    fprintf(f, "\tsadd16 sp, r1, r2\n\tuqsub8 r0, sp, r2\n\tqadd r3, r4, sp\n"
               "\tsmlad r0, sp, r2, sp\n\tsmlalbb sp, r1, r2, r3\n"
               "\tsxtab16 sp, r1, sp, ror #8\n\tsxtb r0, sp\n"
               "\tpkhtb r0, sp, r1, asr #32\n\tssat16 sp, #4, sp\n"
               "\tsel sp, r1, sp\n\tusada8 sp, sp, r2, sp\n"
               "\tssat r0, #8, r1, asr #32\n\tusat lr, #31, r2, asr #32\n");
    fprintf(f, "\tsadd16eq r0, r1, r2\n\tsmladxne r0, r1, r2, r3\n"
               "\tssatgt r0, #8, r1, lsl #4\n\tsxtabhi r0, r1, r2, ror #8\n"
               "\tsmlalbble r0, r1, r2, r3\n\tqdsubmi r4, r5, r6\n"
               "\tuhsaxpl r7, r8, r9\n\tpkhtbvs r1, r2, r3, asr #3\n"
               "\tsmmlsrlt r1, r2, r3, r4\n\tsxtb16ls r5, r6, ror #24\n");
}

/* The multiplies, reversals, bit fields and pairs of more_tab, for
 * tests/golden/arm-asm-more.sh: each entry with r0-r12 and lr in every
 * register field (rotated so the fields differ), the bit fields and
 * offsets at their ends and between. In ARM state (t_isa_a32) the pairs
 * are even registers and the next, the offsets A32's, and the
 * doubleword exclusives and some conditions are there too. */
void tasm_vocabulary_more(FILE *f)
{
    static const long toff[] = { 8, -1020, 1020, -4, 252, -256, 4, 64 };
    static const long aoff[] = { 8, -255, 255, -1, 254, -128, 3, 77 };
    int a32 = t_isa_a32;
    for (const struct more_ent *e = more_tab; e->name; e++)
        for (int a = 0; v8_regs[a]; a++) {
            const char *rd = v8_regs[a], *rn = v8_regs[(a + 5) % 14];
            const char *rm = v8_regs[(a + 9) % 14], *ra = v8_regs[(a + 3) % 14];
            int lsb = a * 31 / 13, width = 1 + (a * 7) % (32 - lsb);
            switch (e->form) {
            case MF_MLA:
                fprintf(f, "\t%s %s, %s, %s, %s\n", e->name, rd, rn, rm, ra);
                break;
            case MF_LMUL:
                fprintf(f, "\t%s %s, %s, %s, %s\n", e->name, rd,
                        v8_regs[(a + 1) % 14], rn, rm);
                break;
            case MF_REV: case MF_RRX:
                fprintf(f, "\t%s %s, %s\n", e->name, rd, rm);
                break;
            case MF_BFI:
                if (e->a)
                    fprintf(f, "\t%s %s, #%d, #%d\n", e->name, rd, lsb, width);
                else
                    fprintf(f, "\t%s %s, %s, #%d, #%d\n", e->name, rd, rn, lsb,
                            width);
                break;
            case MF_BFX:
                fprintf(f, "\t%s %s, %s, #%d, #%d\n", e->name, rd, rn, lsb,
                        width);
                break;
            case MF_LDRD: {
                /* Thumb: any two (different) registers; ARM: rN, rN+1 */
                int t1 = a32 ? 2 * (a % 7) : a, t2 = a32 ? t1 + 1 : (a + 6) % 14;
                const char *base = v8_regs[(t1 + (a32 ? 5 : 3)) % 14];
                long off = a32 ? aoff[a % 8] : toff[a % 8];
                const char *r1 = a32 ? reg_name[t1] : v8_regs[t1];
                const char *r2 = a32 ? reg_name[t2] : v8_regs[t2];
                switch (a % 4) {
                case 0: fprintf(f, "\t%s %s, %s, [%s]\n", e->name, r1, r2, base);
                        break;
                case 1: fprintf(f, "\t%s %s, %s, [%s, #%ld]\n", e->name, r1, r2,
                                base, off); break;
                case 2: fprintf(f, "\t%s %s, %s, [%s, #%ld]!\n", e->name, r1,
                                r2, base, off); break;
                default: fprintf(f, "\t%s %s, %s, [%s], #%ld\n", e->name, r1,
                                 r2, base, off); break;
                }
                break;
            }
            default:
                if (!a32)
                    break;
                if (e->form == MF_LDREXD)
                    fprintf(f, "\t%s %s, %s, [%s]\n", e->name,
                            reg_name[2 * (a % 7)], reg_name[2 * (a % 7) + 1],
                            v8_regs[(2 * (a % 7) + 5) % 14]);
                else
                    fprintf(f, "\t%s %s, %s, %s, [%s]\n", e->name,
                            v8_regs[(2 * (a % 7) + 2) % 14],
                            reg_name[2 * (a % 7)], reg_name[2 * (a % 7) + 1],
                            v8_regs[(2 * (a % 7) + 5) % 14]);
                break;
            }
        }
    /* the 16-bit forms of rev16 and revsh, and the ends of the fields */
    fprintf(f, "\trev16 r0, r7\n\trevsh r6, r1\n\trev16 r3, r3\n"
               "\tbfi r0, r1, #0, #32\n\tbfc r2, #31, #1\n"
               "\tsbfx r3, r4, #0, #32\n\tubfx r5, r6, #31, #1\n"
               "\tldrd r0, r1, [sp, #8]\n\tstrd r2, r3, [sp, #-16]!\n");
    fprintf(f, "\tldrd r0, r1, [pc, #8]\n\tldrd r4, r5, [pc, #%d]\n",
            a32 ? -255 : -1020);
    if (!a32)
        return;
    fprintf(f, "\tmla sp, sp, sp, sp\n\tumull r0, sp, r1, r2\n"
               "\trrx sp, sp\n\trev16 sp, r0\n\tbfi sp, sp, #4, #8\n"
               "\tldrd r12, sp, [r0, #-4]\n");
    fprintf(f, "\tmlaeq r0, r1, r2, r3\n\tumaalne r0, r1, r2, r3\n"
               "\trrxsgt r4, r5\n\trevshlt r6, r7\n\tbfcge r8, #4, #4\n"
               "\tubfxhi r9, r10, #3, #9\n\tldrdls r0, r1, [r2, #-8]!\n"
               "\tstrdmi r2, r3, [r4], #16\n\tldrexdpl r4, r5, [r6]\n"
               "\tstrexdvs r7, r8, r9, [r10]\n\tsmlalvc r1, r2, r3, r4\n");
}
