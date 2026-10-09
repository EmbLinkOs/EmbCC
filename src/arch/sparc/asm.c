/* The SPARC V8 assembler. See asm.h. All this file adds to emit.c is a
 * parser: every range is checked HERE, before the encoder is called, so a
 * template's mistake is a diagnostic and not an internal error. */
#include "asm.h"

#include "emit.h"
#include "../asmexpr.h"
#include "../../elf/elf.h"
#include "../../driver/util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FAIL(...)  do { snprintf(err, (size_t)errlen, __VA_ARGS__); \
                        return -1; } while (0)

/* ---- registers ----------------------------------------------------------- */

int spasm_gpr(const char *name, int len)
{
    int base, v;
    if (len > 0 && name[0] == '%') {
        name++;
        len--;
    }
    if (len == 2 && name[0] == 's' && name[1] == 'p')
        return SP_SP;
    if (len == 2 && name[0] == 'f' && name[1] == 'p')
        return SP_FP;
    if (len < 2 || len > 3 || !isdigit((unsigned char)name[1]))
        return -1;
    switch (name[0]) {
    case 'g': base = 0; break;
    case 'o': base = 8; break;
    case 'l': base = 16; break;
    case 'i': base = 24; break;
    case 'r': base = -1; break;
    default: return -1;
    }
    v = name[1] - '0';
    if (len == 3) {
        if (base >= 0 || !isdigit((unsigned char)name[2]) || name[1] == '0')
            return -1;
        v = v * 10 + (name[2] - '0');
    }
    if (base >= 0)
        return v < 8 ? base + v : -1;
    return v < 32 ? v : -1;
}

/* ---- a tiny tokeniser ------------------------------------------------------ */

#define MAXTOK 8

struct tok { const char *s; int len; };

/* The mnemonic, up to the first blank (`bne,a` is one), then the operands,
 * separated by the commas outside brackets and parentheses; each trimmed.
 * Returns the count, or -1 when there are too many. */
static int split(const char *stmt, int len, struct tok *t, int max)
{
    int n, i = 0;
    while (i < len && isspace((unsigned char)stmt[i]))
        i++;
    if (i >= len)
        return 0;
    t[0].s = stmt + i;
    while (i < len && !isspace((unsigned char)stmt[i]))
        i++;
    t[0].len = (int)(stmt + i - t[0].s);
    n = 1;
    while (i < len) {
        int depth = 0, b, e;
        while (i < len && isspace((unsigned char)stmt[i]))
            i++;
        if (i >= len)
            break;
        if (n == max)
            return -1;
        b = i;
        while (i < len && (depth > 0 || stmt[i] != ',')) {
            if (stmt[i] == '(' || stmt[i] == '[') depth++;
            else if (stmt[i] == ')' || stmt[i] == ']') depth--;
            i++;
        }
        e = i;
        while (e > b && isspace((unsigned char)stmt[e - 1]))
            e--;
        t[n].s = stmt + b;
        t[n].len = e - b;
        n++;
        if (i < len)
            i++;                                     /* the comma */
    }
    return n;
}

static int tok_reg(const struct tok *t)
{
    if (t->len < 2 || t->s[0] != '%')
        return -1;
    return spasm_gpr(t->s, t->len);
}

static int is_ident0(int c) { return isalpha(c) || c == '_' || c == '.' || c == '$'; }
static int is_identc(int c) { return isalnum(c) || c == '_' || c == '.' || c == '$'; }

/* A constant: a C expression, or %hi()/%lo() of one. 0 when it is not. */
static int tok_imm(const struct tok *t, long long *out)
{
    const char *s = t->s;
    int len = t->len;
    if (len > 5 && s[0] == '%' && s[len - 1] == ')' &&
        (!strncmp(s, "%hi(", 4) || !strncmp(s, "%lo(", 4))) {
        long long v;
        if (!asm_const_expr(s + 4, len - 5, &v))
            return 0;
        v &= 0xffffffffLL;
        *out = s[1] == 'h' ? (v >> 10) & 0x3fffff : v & 0x3ff;
        return 1;
    }
    return asm_const_expr(s, len, out);
}

/* A transfer's target: `.`, `.+N` or `.-N`, the distance in bytes from
 * this instruction. */
static int tok_target(const struct tok *t, long long *rel)
{
    if (t->len == 1 && t->s[0] == '.') {
        *rel = 0;
        return 1;
    }
    if (t->len >= 2 && t->s[0] == '.' && (t->s[1] == '+' || t->s[1] == '-'))
        return asm_const_expr(t->s + 1, t->len - 1, rel);
    return 0;
}

/* Does the operand name a symbol -- an identifier that is not `.`? */
static int names_symbol(const struct tok *t)
{
    for (int i = 0; i < t->len; i++) {
        if (t->s[i] == '%') {               /* a register or an operator */
            i++;
            while (i < t->len && is_identc((unsigned char)t->s[i]))
                i++;
            continue;
        }
        if (is_ident0((unsigned char)t->s[i]) &&
            (i == 0 || !is_identc((unsigned char)t->s[i - 1]))) {
            if (t->s[i] == '.' && (i + 1 >= t->len ||
                                   !is_identc((unsigned char)t->s[i + 1])))
                continue;                   /* `.` itself */
            if ((t->s[i] == 'x' || t->s[i] == 'X' || t->s[i] == 'b' ||
                 t->s[i] == 'B') && i > 0 && t->s[i - 1] == '0')
                continue;                   /* 0x.., 0b.. */
            return 1;
        }
    }
    return 0;
}

static int bad_operand(const struct tok *t, const char *mn, const char *want,
                       char *err, int errlen)
{
    if (names_symbol(t))
        FAIL("%s: \"%.*s\" names a symbol, and inline asm cannot reach one: "
             "a template carries no relocation (load the address into a "
             "register operand, or write it in a .S file)", mn, t->len,
             t->s);
    FAIL("%s: \"%.*s\" is not %s", mn, t->len, t->s, want);
}

static int need_reg(const struct tok *t, int *r, const char *mn, char *err,
                    int errlen)
{
    *r = tok_reg(t);
    if (*r < 0)
        return bad_operand(t, mn, "a register (%g0-%i7, %r0-%r31, %sp, %fp)",
                           err, errlen);
    return 0;
}

/* A register or a signed 13-bit constant: *isimm says which. */
static int need_ri(const struct tok *t, int *r, long long *v, int *isimm,
                   const char *mn, char *err, int errlen)
{
    *r = tok_reg(t);
    if (*r >= 0) {
        *isimm = 0;
        return 0;
    }
    if (!tok_imm(t, v))
        return bad_operand(t, mn, "a register or a constant", err, errlen);
    if (!sparc_simm13_ok(*v))
        FAIL("%s: %lld does not fit the signed 13 bits of an immediate "
             "(-4096..4095)", mn, *v);
    *isimm = 1;
    return 0;
}

/* An address: %rs1, %rs1 + %rs2, %rs1 +/- simm13, simm13 + %rs1, or a
 * simm13 alone (from %g0). */
struct addr { int rs1, rs2, isimm; long long imm; };

static int parse_addr(const char *s, int len, struct addr *a, const char *mn,
                      char *err, int errlen)
{
    int i = 0, depth = 0, split_at = -1;
    struct tok t;
    while (len > 0 && isspace((unsigned char)s[len - 1]))
        len--;
    while (len > 0 && isspace((unsigned char)*s)) {
        s++;
        len--;
    }
    if (!len)
        FAIL("%s: an empty address", mn);
    /* the operator between the two parts: the first '+' or '-' at depth
     * 0 that is not a sign (it follows something) */
    for (i = 1; i < len; i++) {
        if (s[i] == '(') depth++;
        else if (s[i] == ')') depth--;
        else if (depth == 0 && (s[i] == '+' || s[i] == '-')) {
            int k = i - 1;
            while (k >= 0 && isspace((unsigned char)s[k]))
                k--;
            if (k >= 0 && s[k] != '+' && s[k] != '-' && s[k] != '*' &&
                s[k] != '/' && s[k] != '(' && s[k] != '<' && s[k] != '>' &&
                s[k] != '&' && s[k] != '|' && s[k] != '^') {
                /* only where one side is a register: `4+4` is one
                 * constant */
                struct tok l = { s, k + 1 }, r;
                int j = i + 1;
                while (j < len && isspace((unsigned char)s[j]))
                    j++;
                r.s = s + j;
                r.len = len - j;
                if (tok_reg(&l) >= 0 || tok_reg(&r) >= 0) {
                    split_at = i;
                    break;
                }
            }
        }
    }
    a->rs1 = SP_G0;
    a->rs2 = SP_G0;
    a->isimm = 0;
    a->imm = 0;
    if (split_at < 0) {
        t.s = s;
        t.len = len;
        a->rs1 = tok_reg(&t);
        if (a->rs1 >= 0)
            return 0;
        a->rs1 = SP_G0;
        if (!tok_imm(&t, &a->imm))
            return bad_operand(&t, mn, "an address", err, errlen);
        a->isimm = 1;
    } else {
        struct tok l, r;
        int lr, rr, j = split_at + 1;
        l.s = s;
        l.len = split_at;
        while (l.len > 0 && isspace((unsigned char)l.s[l.len - 1]))
            l.len--;
        while (j < len && isspace((unsigned char)s[j]))
            j++;
        r.s = s + j;
        r.len = len - j;
        lr = tok_reg(&l);
        rr = tok_reg(&r);
        if (lr >= 0 && rr >= 0) {
            if (s[split_at] == '-')
                FAIL("%s: \"%.*s\": a register cannot be subtracted", mn,
                     len, s);
            a->rs1 = lr;
            a->rs2 = rr;
            return 0;
        }
        if (lr >= 0) {
            a->rs1 = lr;
            if (!tok_imm(&r, &a->imm))
                return bad_operand(&r, mn, "a constant offset", err, errlen);
            if (s[split_at] == '-')
                a->imm = -a->imm;
        } else {
            if (s[split_at] == '-')
                FAIL("%s: \"%.*s\": a register cannot be subtracted", mn,
                     len, s);
            a->rs1 = rr;
            if (!tok_imm(&l, &a->imm))
                return bad_operand(&l, mn, "a constant offset", err, errlen);
        }
        a->isimm = 1;
    }
    if (!sparc_simm13_ok(a->imm))
        FAIL("%s: the offset %lld does not fit the signed 13 bits of an "
             "address (-4096..4095)", mn, a->imm);
    return 0;
}

/* `[address]`, optionally followed by an ASI (*asi, -1 when absent). */
static int parse_mem(const struct tok *t, struct addr *a, int *asi,
                     const char *mn, char *err, int errlen)
{
    const char *e = t->len >= 2 && t->s[0] == '['
                    ? memchr(t->s, ']', (size_t)t->len) : NULL;
    if (!e)
        return bad_operand(t, mn, "a memory operand ([address])", err,
                           errlen);
    if (parse_addr(t->s + 1, (int)(e - t->s - 1), a, mn, err, errlen))
        return -1;
    *asi = -1;
    {
        struct tok r;
        long long v;
        r.s = e + 1;
        r.len = (int)(t->s + t->len - r.s);
        while (r.len > 0 && isspace((unsigned char)*r.s)) {
            r.s++;
            r.len--;
        }
        if (r.len == 0)
            return 0;
        if (r.len == 4 && !strncmp(r.s, "%asi", 4))
            FAIL("%s: %%asi is SPARC V9's; give the ASI as a number", mn);
        if (!tok_imm(&r, &v))
            return bad_operand(&r, mn, "an address space identifier", err,
                               errlen);
        if (v < 0 || v > 255)
            FAIL("%s: ASI %lld is not 0..255", mn, v);
        *asi = (int)v;
    }
    return 0;
}

/* ---- the instruction tables ------------------------------------------------- */

/* op = 10, `op rs1, reg_or_imm, rd`. */
struct alu_ent { const char *name; int op3; };
static const struct alu_ent alu_tab[] = {
    { "add", SP_ADD }, { "addcc", SP_ADDCC }, { "addx", SP_ADDX },
    { "addxcc", SP_ADDXCC }, { "sub", SP_SUB }, { "subcc", SP_SUBCC },
    { "subx", SP_SUBX }, { "subxcc", SP_SUBXCC }, { "and", SP_AND },
    { "andcc", SP_ANDCC }, { "andn", SP_ANDN }, { "andncc", SP_ANDNCC },
    { "or", SP_OR }, { "orcc", SP_ORCC }, { "orn", SP_ORN },
    { "orncc", SP_ORNCC }, { "xor", SP_XOR }, { "xorcc", SP_XORCC },
    { "xnor", SP_XNOR }, { "xnorcc", SP_XNORCC }, { "umul", SP_UMUL },
    { "umulcc", SP_UMULCC }, { "smul", SP_SMUL }, { "smulcc", SP_SMULCC },
    { "udiv", SP_UDIV }, { "udivcc", SP_UDIVCC }, { "sdiv", SP_SDIV },
    { "sdivcc", SP_SDIVCC }, { "taddcc", SP_TADDCC },
    { "tsubcc", SP_TSUBCC }, { "taddcctv", SP_TADDCCTV },
    { "tsubcctv", SP_TSUBCCTV }, { "mulscc", SP_MULSCC },
    { "sll", SP_SLL }, { "srl", SP_SRL }, { "sra", SP_SRA },
    { "save", SP_O3_SAVE }, { "restore", SP_O3_RESTORE },
    { NULL, 0 }
};

/* op = 11: loads `op [address], rd` and stores `op rd, [address]`; each
 * has an alternate-space form, the name + "a" (op3 | SPM_ALT). */
struct mem_ent { const char *name; int op3; int store; int even; };
static const struct mem_ent mem_tab[] = {
    { "ld", SPM_LD, 0, 0 }, { "ldub", SPM_LDUB, 0, 0 },
    { "lduh", SPM_LDUH, 0, 0 }, { "ldsb", SPM_LDSB, 0, 0 },
    { "ldsh", SPM_LDSH, 0, 0 }, { "ldd", SPM_LDD, 0, 1 },
    { "ldstub", SPM_LDSTUB, 0, 0 }, { "swap", SPM_SWAP, 0, 0 },
    { "st", SPM_ST, 1, 0 }, { "stb", SPM_STB, 1, 0 },
    { "sth", SPM_STH, 1, 0 }, { "std", SPM_STD, 1, 1 },
    { NULL, 0, 0, 0 }
};

/* The integer conditions, as a Bicc's or a Ticc's suffix. The first
 * sixteen are the names llvm-objdump prints; the rest are GNU's
 * synonyms. */
static const struct { const char *name; int cond; } cond_tab[] = {
    { "a", SP_BA }, { "n", SP_BN }, { "ne", SP_BNE }, { "e", SP_BE },
    { "g", SP_BG }, { "le", SP_BLE }, { "ge", SP_BGE }, { "l", SP_BL },
    { "gu", SP_BGU }, { "leu", SP_BLEU }, { "cc", SP_BCC },
    { "cs", SP_BCS }, { "pos", SP_BPOS }, { "neg", SP_BNEG },
    { "vc", SP_BVC }, { "vs", SP_BVS },
    { "", SP_BA }, { "nz", SP_BNE }, { "z", SP_BE }, { "geu", SP_BCC },
    { "lu", SP_BCS },
    { NULL, 0 }
};

static int cond_of(const char *s)
{
    for (int k = 0; cond_tab[k].name; k++)
        if (!strcmp(s, cond_tab[k].name))
            return cond_tab[k].cond;
    return -1;
}

/* The state registers rd and wr name. %asrN is N (1..31). */
static int state_reg(const struct tok *t, int *asr)
{
    static const char *const nm[] = { "%y", "%psr", "%wim", "%tbr" };
    *asr = 0;
    for (int k = 0; k < 4; k++)
        if (t->len == (int)strlen(nm[k]) && !strncmp(t->s, nm[k], (size_t)t->len))
            return k;
    if (t->len >= 5 && t->len <= 6 && !strncmp(t->s, "%asr", 4) &&
        isdigit((unsigned char)t->s[4]) &&
        (t->len == 5 || isdigit((unsigned char)t->s[5]))) {
        int n = atoi(t->s + 4);
        if (t->len == 6 && t->s[4] == '0')
            return -1;
        if (n >= 1 && n <= 31) {
            *asr = n;
            return 4;
        }
    }
    return -1;
}

static const int rd_op3[4] = { SP_O3_RDY, SP_O3_RDPSR, SP_O3_RDWIM,
                               SP_O3_RDTBR };
static const int wr_op3[4] = { SP_O3_WRY, SP_O3_WRPSR, SP_O3_WRWIM,
                               SP_O3_WRTBR };

/* rd = rs1 OP reg_or_imm, packed by emit.c's format-3 encoders */
static void f3(struct code *out, int op, int rd, int op3, int rs1, int isimm,
               int rs2, long long imm)
{
    sparc_w(out, isimm ? sparc_enc_ri(op, rd, op3, rs1, imm)
                       : sparc_enc_rr(op, rd, op3, rs1, rs2));
}

static void f3a(struct code *out, int op, int rd, int op3, const struct addr *a)
{
    f3(out, op, rd, op3, a->rs1, a->isimm, a->rs2, a->imm);
}

/* The forms that refuse by name: floating point, the coprocessor, and
 * SPARC V9. */
static const char *refused_form(const char *mn)
{
    static const char *const fp[] = {
        "ldf", "lddf", "ldfsr", "stf", "stdf", "stfsr", "stdfq", "ldfa",
        "stfa", NULL
    };
    static const char *const v9[] = {
        "ldx", "stx", "ldsw", "lduw", "casx", "casxa", "membar", "rdpr",
        "wrpr", "done", "retry", "popc", "movcc", "movr", "brz", "brnz",
        "brlz", "brgz", "brlez", "brgez", "sllx", "srlx", "srax", "mulx",
        "sdivx", "udivx", "return", "prefetch", "flushw", "saved",
        "restored", "setx", NULL
    };
    for (int k = 0; v9[k]; k++)
        if (!strcmp(mn, v9[k]))
            return "SPARC V9's, and the LEON3 is a V8";
    for (int k = 0; fp[k]; k++)
        if (!strcmp(mn, fp[k]))
            return "a floating-point load or store: EmbCC compiles soft "
                   "float, and this assembler has no floating-point "
                   "vocabulary";
    if (mn[0] == 'f' && strcmp(mn, "flush"))
        return "a floating-point instruction: EmbCC compiles soft float, "
               "and this assembler has no floating-point vocabulary";
    if ((mn[0] == 'c' && mn[1] == 'b') || !strcmp(mn, "ldc") ||
        !strcmp(mn, "stc") || !strcmp(mn, "lddc") || !strcmp(mn, "stdc") ||
        !strncmp(mn, "cpop", 4))
        return "a coprocessor instruction, which the LEON3 does not have";
    return NULL;
}

/* One statement; `raw` (a statement spasm_symform wrote for a
 * relocation) makes `set` the two-instruction form whatever its value. */
static int stmt_body(const char *stmt, int len, struct code *out, char *err,
                     int errlen)
{
    struct tok t[MAXTOK];
    int raw = 0, n, rd, rs1, rs2, isimm, annul = 0;
    long long v, rel;
    char mn[32];
    int ml;
    struct addr a;

    while (len > 0 && isspace((unsigned char)*stmt)) {
        stmt++;
        len--;
    }
    if (len > 0 && stmt[0] == '\003') {
        raw = 1;
        stmt++;
        len--;
    }
    if (len > 0 && stmt[0] == '\001')
        FAIL("%.*s", len - 1, stmt + 1);
    n = split(stmt, len, t, MAXTOK);
    if (n == 0)
        return 0;
    if (n < 0)
        FAIL("\"%.*s\" has too many operands", len, stmt);
    ml = t[0].len;
    if (ml >= (int)sizeof mn)
        FAIL("asm instruction \"%.*s\" is not in the SPARC vocabulary",
             t[0].len, t[0].s);
    for (int i = 0; i < ml; i++)
        mn[i] = (char)tolower((unsigned char)t[0].s[i]);
    mn[ml] = 0;
    for (int i = 1; i < n; i++)
        if (t[i].len == 0)
            FAIL("%s: an empty operand", mn);
    /* the annul suffix */
    if (ml > 2 && mn[ml - 2] == ',' && mn[ml - 1] == 'a') {
        annul = 1;
        mn[ml - 2] = 0;
        ml -= 2;
    } else if (strchr(mn, ',')) {
        if (strstr(mn, ",pt") || strstr(mn, ",pn"))
            FAIL("%s: a branch prediction (,pt/,pn) is SPARC V9's, and the "
                 "LEON3 is a V8", mn);
        FAIL("asm instruction \"%s\" is not in the SPARC vocabulary", mn);
    }
#define IS(s) (strcmp(mn, s) == 0)
#define NOPS(k, what) do { if (n != (k) + 1) \
        FAIL("%s takes %s", mn, what); } while (0)

    if (annul && mn[0] != 'b')
        FAIL("%s: only a branch has an annulled (,a) form", mn);

    /* ---- no operands ---- */
    if (IS("nop")) { NOPS(0, "no operands"); sparc_nop(out); return 0; }
    if (IS("stbar")) { NOPS(0, "no operands"); sparc_stbar(out); return 0; }
    if (IS("ret") || IS("retl")) {
        NOPS(0, "no operands");
        sparc_jmpl(out, SP_G0, IS("ret") ? SP_I7 : SP_O7, 8);
        return 0;
    }
    if ((IS("save") || IS("restore")) && n == 1) {
        f3(out, 2, SP_G0, IS("save") ? SP_O3_SAVE : SP_O3_RESTORE, SP_G0, 0,
           SP_G0, 0);
        return 0;
    }

    /* ---- the ALU: rs1, reg_or_imm, rd ---- */
    for (const struct alu_ent *e = alu_tab; e->name; e++) {
        if (!IS(e->name))
            continue;
        NOPS(3, "three operands: rs1, a register or constant, rd");
        if (need_reg(&t[1], &rs1, mn, err, errlen) ||
            need_ri(&t[2], &rs2, &v, &isimm, mn, err, errlen) ||
            need_reg(&t[3], &rd, mn, err, errlen))
            return -1;
        if (isimm && (e->op3 == SP_SLL || e->op3 == SP_SRL ||
                      e->op3 == SP_SRA) && (v < 0 || v > 31))
            FAIL("%s: the shift is 0..31, not %lld", mn, v);
        f3(out, 2, rd, e->op3, rs1, isimm, rs2, v);
        return 0;
    }

    /* ---- loads and stores ---- */
    for (const struct mem_ent *e = mem_tab; e->name; e++) {
        size_t nl = strlen(e->name);
        int alt, asi, reg_at, mem_at;
        if (strncmp(mn, e->name, nl) || (mn[nl] && strcmp(mn + nl, "a")))
            continue;
        alt = mn[nl] == 'a';
        NOPS(2, e->store ? "a register and a memory operand"
                         : "a memory operand and a register");
        reg_at = e->store ? 1 : 2;
        mem_at = e->store ? 2 : 1;
        if (need_reg(&t[reg_at], &rd, mn, err, errlen) ||
            parse_mem(&t[mem_at], &a, &asi, mn, err, errlen))
            return -1;
        if (e->even && (rd & 1))
            FAIL("%s: %.*s is odd; the pair begins at an even register", mn,
                 t[reg_at].len, t[reg_at].s);
        if (!alt && asi >= 0)
            FAIL("%s: an address space is %sa's (the alternate-space form)",
                 mn, e->name);
        if (alt) {
            if (asi < 0)
                FAIL("%s: the alternate-space form names its ASI: "
                     "[address] asi", mn);
            if (a.isimm)
                FAIL("%s: the alternate-space form takes [rs1 + rs2], not "
                     "an offset", mn);
            sparc_w(out, sparc_enc_rr(3, rd, e->op3 | SPM_ALT, a.rs1, a.rs2) |
                         ((unsigned long)asi << 5));
            return 0;
        }
        f3a(out, 3, rd, e->op3, &a);
        return 0;
    }
    if (IS("casa")) {
        int asi;
        NOPS(3, "[rs1] asi, rs2, rd");
        if (parse_mem(&t[1], &a, &asi, mn, err, errlen) ||
            need_reg(&t[2], &rs2, mn, err, errlen) ||
            need_reg(&t[3], &rd, mn, err, errlen))
            return -1;
        if (a.isimm || a.rs2 != SP_G0)
            FAIL("casa: the address is one register, [rs1]");
        if (asi < 0)
            FAIL("casa: the ASI is required: [rs1] asi");
        sparc_casa(out, a.rs1, asi, rs2, rd);
        return 0;
    }
    if (IS("clr") || IS("clrb") || IS("clrh")) {
        NOPS(1, "a register or a memory operand");
        if (IS("clr") && t[1].s[0] != '[') {
            if (need_reg(&t[1], &rd, mn, err, errlen))
                return -1;
            sparc_alu(out, SP_OR, rd, SP_G0, SP_G0);
            return 0;
        }
        {
            int asi;
            if (parse_mem(&t[1], &a, &asi, mn, err, errlen))
                return -1;
            if (asi >= 0)
                FAIL("%s: no alternate space here", mn);
            f3a(out, 3, SP_G0, IS("clr") ? SPM_ST : IS("clrb") ? SPM_STB
                                                               : SPM_STH, &a);
        }
        return 0;
    }

    /* ---- sethi, set, unimp ---- */
    if (IS("sethi")) {
        NOPS(2, "a 22-bit constant and a register");
        if (need_reg(&t[2], &rd, mn, err, errlen))
            return -1;
        if (!tok_imm(&t[1], &v))
            return bad_operand(&t[1], mn, "a constant", err, errlen);
        if (v < 0 || v > 0x3fffff)
            FAIL("sethi: %lld does not fit 22 bits (write %%hi(value))", v);
        sparc_sethi(out, rd, (unsigned long)v);
        return 0;
    }
    if (IS("set")) {
        unsigned long u;
        long long s;
        NOPS(2, "a constant and a register");
        if (need_reg(&t[2], &rd, mn, err, errlen))
            return -1;
        if (!tok_imm(&t[1], &v))
            return bad_operand(&t[1], mn, "a constant", err, errlen);
        if (v < -2147483648LL || v > 0xffffffffLL)
            FAIL("set: %lld does not fit 32 bits", v);
        u = (unsigned long)v & 0xffffffffUL;
        s = (long long)(int)(unsigned int)u;
        /* GNU's choice (and llvm-mc's): mov when it fits 13 bits, sethi
         * alone when its low ten bits are 0, else both */
        if (!raw && sparc_simm13_ok(s)) {
            sparc_alu_imm(out, SP_OR, rd, SP_G0, s);
        } else {
            sparc_sethi(out, rd, (u >> 10) & 0x3fffff);
            if (raw || (u & 0x3ff))
                sparc_alu_imm(out, SP_OR, rd, rd, (long long)(u & 0x3ff));
        }
        return 0;
    }
    if (IS("unimp")) {
        if (n == 1) {
            v = 0;
        } else {
            NOPS(1, "a 22-bit constant");
            if (!tok_imm(&t[1], &v))
                return bad_operand(&t[1], mn, "a constant", err, errlen);
        }
        if (v < 0 || v > 0x3fffff)
            FAIL("unimp: %lld does not fit 22 bits", v);
        sparc_unimp(out, (unsigned long)v);
        return 0;
    }

    /* ---- moves and the other synthetic instructions ---- */
    if (IS("mov")) {
        int sr, asr;
        NOPS(2, "a source and a destination");
        sr = state_reg(&t[1], &asr);
        if (sr >= 0) {                                   /* rd %y, rd */
            if (need_reg(&t[2], &rd, mn, err, errlen))
                return -1;
            sparc_w(out, sparc_enc_rr(2, rd, rd_op3[sr == 4 ? 0 : sr],
                                      asr, SP_G0));
            return 0;
        }
        sr = state_reg(&t[2], &asr);
        if (sr >= 0) {                                   /* wr %g0, x, %y */
            if (need_ri(&t[1], &rs2, &v, &isimm, mn, err, errlen))
                return -1;
            f3(out, 2, asr, wr_op3[sr == 4 ? 0 : sr], SP_G0, isimm, rs2, v);
            return 0;
        }
        if (need_ri(&t[1], &rs2, &v, &isimm, mn, err, errlen) ||
            need_reg(&t[2], &rd, mn, err, errlen))
            return -1;
        f3(out, 2, rd, SP_OR, SP_G0, isimm, rs2, v);
        return 0;
    }
    if (IS("cmp")) {
        NOPS(2, "a register and a register or constant");
        if (need_reg(&t[1], &rs1, mn, err, errlen) ||
            need_ri(&t[2], &rs2, &v, &isimm, mn, err, errlen))
            return -1;
        f3(out, 2, SP_G0, SP_SUBCC, rs1, isimm, rs2, v);
        return 0;
    }
    if (IS("tst")) {
        NOPS(1, "a register");
        if (need_reg(&t[1], &rs1, mn, err, errlen))
            return -1;
        sparc_alu(out, SP_ORCC, SP_G0, rs1, SP_G0);
        return 0;
    }
    if (IS("not") || IS("neg")) {
        if (n != 2 && n != 3)
            FAIL("%s takes a register, or a source and a destination", mn);
        if (need_reg(&t[1], &rs1, mn, err, errlen) ||
            need_reg(&t[n - 1], &rd, mn, err, errlen))
            return -1;
        if (IS("not"))
            sparc_alu(out, SP_XNOR, rd, rs1, SP_G0);
        else
            sparc_alu(out, SP_SUB, rd, SP_G0, rs1);
        return 0;
    }
    if (IS("inc") || IS("inccc") || IS("dec") || IS("deccc")) {
        int op3 = IS("inc") ? SP_ADD : IS("inccc") ? SP_ADDCC
                : IS("dec") ? SP_SUB : SP_SUBCC;
        if (n == 2) {
            v = 1;
        } else {
            NOPS(2, "a register, or a constant and a register");
            if (!tok_imm(&t[1], &v))
                return bad_operand(&t[1], mn, "a constant", err, errlen);
            if (!sparc_simm13_ok(v))
                FAIL("%s: %lld does not fit the signed 13 bits of an "
                     "immediate (-4096..4095)", mn, v);
        }
        if (need_reg(&t[n - 1], &rd, mn, err, errlen))
            return -1;
        sparc_alu_imm(out, op3, rd, rd, v);
        return 0;
    }
    if (IS("btst") || IS("bset") || IS("bclr") || IS("btog")) {
        NOPS(2, "a register or constant and a register");
        if (need_ri(&t[1], &rs2, &v, &isimm, mn, err, errlen) ||
            need_reg(&t[2], &rs1, mn, err, errlen))
            return -1;
        if (IS("btst"))
            f3(out, 2, SP_G0, SP_ANDCC, rs1, isimm, rs2, v);
        else
            f3(out, 2, rs1, IS("bset") ? SP_OR : IS("bclr") ? SP_ANDN
                                                            : SP_XOR,
               rs1, isimm, rs2, v);
        return 0;
    }

    /* ---- the state registers ---- */
    if (IS("rd")) {
        int sr, asr;
        NOPS(2, "a state register and a register");
        sr = state_reg(&t[1], &asr);
        if (sr < 0)
            FAIL("rd: \"%.*s\" is not %%y, %%psr, %%wim, %%tbr or "
                 "%%asr1-%%asr31", t[1].len, t[1].s);
        if (need_reg(&t[2], &rd, mn, err, errlen))
            return -1;
        sparc_w(out, sparc_enc_rr(2, rd, rd_op3[sr == 4 ? 0 : sr], asr,
                                  SP_G0));
        return 0;
    }
    if (IS("wr")) {
        int sr, asr;
        if (n != 3 && n != 4)
            FAIL("wr takes rs1, a register or constant, and a state register");
        sr = state_reg(&t[n - 1], &asr);
        if (sr < 0)
            FAIL("wr: \"%.*s\" is not %%y, %%psr, %%wim, %%tbr or "
                 "%%asr1-%%asr31", t[n - 1].len, t[n - 1].s);
        if (n == 3) {
            /* wr x, %y is wr %g0, x, %y: x XOR 0 (llvm-mc's packing) */
            rs1 = SP_G0;
            if (need_ri(&t[1], &rs2, &v, &isimm, mn, err, errlen))
                return -1;
        } else if (need_reg(&t[1], &rs1, mn, err, errlen) ||
                   need_ri(&t[2], &rs2, &v, &isimm, mn, err, errlen)) {
            return -1;
        }
        f3(out, 2, asr, wr_op3[sr == 4 ? 0 : sr], rs1, isimm, rs2, v);
        return 0;
    }

    /* ---- transfers ---- */
    if (IS("call")) {
        if (n != 2 && n != 3)
            FAIL("call takes a target (and, as GNU's, an argument count)");
        if (n == 3 && !tok_imm(&t[2], &v))
            return bad_operand(&t[2], mn, "an argument count", err, errlen);
        if (tok_target(&t[1], &rel)) {
            if (rel & 3)
                FAIL("call: the target .%+lld is not 4-aligned", rel);
            if (rel < -2147483648LL || rel > 2147483647LL)
                FAIL("call: .%+lld is out of reach", rel);
            sparc_w(out, sparc_enc_call((long)rel));
            return 0;
        }
        if (t[1].s[0] == '%' && !parse_addr(t[1].s, t[1].len, &a, mn, err,
                                            errlen) &&
            (a.rs1 != SP_G0 || !a.isimm)) {
            f3a(out, 2, SP_O7, SP_O3_JMPL, &a);       /* jmpl addr, %o7 */
            return 0;
        }
        return bad_operand(&t[1], mn, "a call target (.+N, a label, or an "
                           "address in a register)", err, errlen);
    }
    if (IS("jmpl") || IS("jmp") || IS("rett") || IS("flush") ||
        IS("iflush")) {
        int want = IS("jmpl") ? 2 : 1;
        if (n != want + 1)
            FAIL("%s takes %s", mn, want == 2 ? "an address and a register"
                                              : "an address");
        if (t[1].s[0] == '[')
            FAIL("%s: the address is written without brackets", mn);
        if (parse_addr(t[1].s, t[1].len, &a, mn, err, errlen))
            return -1;
        if (IS("jmpl")) {
            if (need_reg(&t[2], &rd, mn, err, errlen))
                return -1;
            f3a(out, 2, rd, SP_O3_JMPL, &a);
        } else {
            f3a(out, 2, SP_G0, IS("jmp") ? SP_O3_JMPL : IS("rett")
                               ? SP_O3_RETT : SP_O3_FLUSH, &a);
        }
        return 0;
    }
    if (mn[0] == 'b' && cond_of(mn + 1) >= 0) {
        int cond = cond_of(mn + 1);
        NOPS(1, "a target");
        if (!tok_target(&t[1], &rel))
            return bad_operand(&t[1], mn, "a branch target (a label, or .+N "
                               "bytes from the branch)", err, errlen);
        if (rel & 3)
            FAIL("%s: the target .%+lld is not 4-aligned", mn, rel);
        if (rel < -8388608LL || rel > 8388604LL)
            FAIL("%s: .%+lld is out of reach (a branch reaches -8388608.."
                 "8388604 bytes)", mn, rel);
        sparc_w(out, sparc_enc_branch(cond, annul, (long)rel));
        return 0;
    }
    if (mn[0] == 't' && cond_of(mn + 1) >= 0) {
        int cond = cond_of(mn + 1);
        NOPS(1, "a trap number: N, %rs1, %rs1 + N or %rs1 + %rs2");
        if (parse_addr(t[1].s, t[1].len, &a, mn, err, errlen))
            return -1;
        if (a.isimm && (a.imm < 0 || a.imm > 127))
            FAIL("%s: the trap number %lld is not 0..127", mn, a.imm);
        if (tok_reg(&t[1]) >= 0) {
            /* a lone register is the second source, %g0 + rs2, as
             * llvm-mc packs it (the trap number is the same) */
            a.rs2 = a.rs1;
            a.rs1 = SP_G0;
        }
        f3a(out, 2, cond, SP_O3_TICC, &a);
        return 0;
    }
    {
        const char *why = refused_form(mn);
        if (why)
            FAIL("asm instruction \"%s\" is %s", mn, why);
    }
    FAIL("asm instruction \"%s\" is not in the SPARC vocabulary", mn);
#undef IS
#undef NOPS
}

/* ---- a template: statements, numeric labels ----------------------------------- */

#define MAXSTMT 512

struct stm { const char *s; int len; int lab[4]; int nlab; int off; };

/* The statements of `text`, comments stripped, labels split off. */
static int stmts(const char *text, struct stm *st, int max, char *err,
                 int errlen)
{
    int ns = 0;
    const char *p = text;
    while (*p) {
        const char *start = p;
        int len;
        while (*p && *p != ';' && *p != '\n')
            p++;
        len = (int)(p - start);
        for (int i = 0; i < len; i++)
            if (start[i] == '!' || (start[i] == '/' && i + 1 < len &&
                                    start[i + 1] == '/')) {
                len = i;
                break;
            }
        {
            int i = 0;
            while (i < len && isspace((unsigned char)start[i]))
                i++;
            if (i < len && start[i] == '#')
                len = 0;                  /* GNU's line comment */
        }
        if (*p)
            p++;
        if (ns == max)
            FAIL("an asm template of more than %d statements", max);
        st[ns].nlab = 0;
        for (;;) {
            int i = 0, k = 0;
            while (i < len && isspace((unsigned char)start[i]))
                i++;
            k = i;
            while (k < len && (isalnum((unsigned char)start[k]) ||
                               start[k] == '_' || start[k] == '.' ||
                               start[k] == '$'))
                k++;
            if (k == i || k >= len || start[k] != ':')
                break;
            for (int d = i; d < k; d++)
                if (!isdigit((unsigned char)start[d]))
                    FAIL("label \"%.*s\" in inline asm: a template's labels "
                         "are numeric (1:, used as 1b or 1f), since one "
                         "template may be emitted more than once", k - i,
                         start + i);
            if (st[ns].nlab == 4)
                FAIL("more than four labels on one statement");
            st[ns].lab[st[ns].nlab++] = atoi(start + i);
            start += k + 1;
            len -= k + 1;
        }
        st[ns].s = start;
        st[ns].len = len;
        st[ns].off = 0;
        ns++;
    }
    return ns;
}

/* The statement with each `Nb`/`Nf` rewritten as `.+D`. */
static int resolve_labels(const struct stm *st, int ns, int k, char *buf,
                          int cap, char *err, int errlen)
{
    const char *s = st[k].s;
    int len = st[k].len, o = 0;
    for (int i = 0; i < len; ) {
        int j = i, target = -1;
        int mid = i > 0 && (isalnum((unsigned char)s[i - 1]) ||
                            s[i - 1] == '_' || s[i - 1] == '.' ||
                            s[i - 1] == '%');
        while (j < len && isdigit((unsigned char)s[j]))
            j++;
        if (!mid && j > i && j < len && (s[j] == 'b' || s[j] == 'f') &&
            (j + 1 >= len || !(isalnum((unsigned char)s[j + 1]) ||
                               s[j + 1] == '_'))) {
            int lab = atoi(s + i), fwd = s[j] == 'f';
            if (fwd) {
                for (int q = k + 1; q < ns && target < 0; q++)
                    for (int m = 0; m < st[q].nlab; m++)
                        if (st[q].lab[m] == lab)
                            target = st[q].off;
            } else {
                for (int q = k; q >= 0 && target < 0; q--)
                    for (int m = 0; m < st[q].nlab; m++)
                        if (st[q].lab[m] == lab)
                            target = st[q].off;
            }
            if (target < 0)
                FAIL("'%.*s' refers to no label %d %s it in the template",
                     j + 1 - i, s + i, lab, fwd ? "after" : "before");
            o += snprintf(buf + o, (size_t)(cap - o), ".%+d",
                          target - st[k].off);
            i = j + 1;
            if (o >= cap - 16)
                FAIL("an asm statement too long to assemble");
            continue;
        }
        if (j == i)
            j = i + 1;
        while (i < j) {
            if (o >= cap - 16)
                FAIL("an asm statement too long to assemble");
            buf[o++] = s[i++];
        }
    }
    buf[o] = 0;
    return o;
}

int spasm_assemble(const char *text, struct code *out, char *err, int errlen)
{
    static struct stm st[MAXSTMT];
    int ns, off = 0;
    ns = stmts(text, st, MAXSTMT, err, errlen);
    if (ns < 0)
        return -1;
    /* Where each statement lands: `set` is one instruction or two, so
     * the sizes come from encoding each once with its labels at .+0. */
    for (int k = 0; k < ns; k++) {
        char buf[512];
        struct code tmp = { 0 };
        int len, rc;
        st[k].off = off;
        if (!st[k].len)
            continue;
        len = resolve_labels(st, ns, k, buf, (int)sizeof buf, err, errlen);
        if (len < 0)
            return -1;
        rc = stmt_body(buf, len, &tmp, err, errlen);
        off += tmp.len;
        free(tmp.p);
        if (rc)
            return -1;
    }
    for (int k = 0; k < ns; k++) {
        char buf[512];
        int len;
        if (!st[k].len)
            continue;
        len = resolve_labels(st, ns, k, buf, (int)sizeof buf, err, errlen);
        if (len < 0 || stmt_body(buf, len, out, err, errlen))
            return -1;
    }
    return 0;
}

/* ---- for irgen ------------------------------------------------------------------ */

int spasm_template_calls(const char *text)
{
    struct stm *st = xcalloc(MAXSTMT, sizeof *st);
    char err[256];
    int ns = stmts(text, st, MAXSTMT, err, (int)sizeof err), calls = 0;
    for (int k = 0; k < ns && !calls; k++) {
        struct tok t[MAXTOK];
        int n = split(st[k].s, st[k].len, t, MAXTOK);
        if (n < 1)
            continue;
        if (t[0].len == 4 && !strncmp(t[0].s, "call", 4))
            calls = 1;
        else if (n == 3 && t[0].len == 4 && !strncmp(t[0].s, "jmpl", 4) &&
                 tok_reg(&t[2]) == SP_O7)
            calls = 1;
    }
    free(st);
    return calls;
}

/* ---- for the file assembler (src/as/gas.c) ---------------------------------- */

/* A register or an operator after `%`, the `a` of `,a`. */
int spasm_is_word(const char *stmt, const char *w, int len)
{
    (void)len;
    if (w > stmt && w[-1] == '%')
        return 1;
    if (w > stmt && w[-1] == ',') {
        const char *p = stmt;
        while (*p == ' ' || *p == '\t')
            p++;
        while (p < w && !isspace((unsigned char)*p))
            p++;
        return p >= w;                    /* still inside the mnemonic */
    }
    return 0;
}

/* The symbol at p (an identifier, not `.` itself and not after a `%`),
 * then an optional `+K` / `-K`. Returns the length consumed, 0 when
 * there is no symbol there. */
static int sym_operand(const char *p, int *slen, long *add)
{
    int n = 0;
    if (!(isalpha((unsigned char)p[0]) || p[0] == '_' || p[0] == '.'))
        return 0;
    if (p[0] == '.' && !(isalnum((unsigned char)p[1]) || p[1] == '_'))
        return 0;
    while (is_identc((unsigned char)p[n]))
        n++;
    *slen = n;
    *add = 0;
    while (p[n] == ' ')
        n++;
    if (p[n] == '+' || p[n] == '-') {
        char *e;
        const char *q = p + n + 1;
        long k;
        while (*q == ' ')
            q++;
        k = strtol(q, &e, 0);
        if (e == q)
            return 0;
        *add = p[n] == '-' ? -k : k;
        n = (int)(e - p);
    }
    while (p[n] == ' ')
        n++;
    return n;
}

static int refuse_form(struct asm_symform *f, const char *stmt,
                       const char *word, int wlen, const char *why)
{
    f->sym_at = (int)(word - stmt);
    f->sym_len = wlen;
    f->addend = 0;
    f->nsites = 0;
    snprintf(f->encode, sizeof f->encode, "\001%s", why);
    return 1;
}

static const char *skip_sp(const char *p)
{
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

/* The forms that name a symbol's address: %hi(sym), %lo(sym) and
 * `set sym, rd` -- relocated whatever the symbol is, as a label's
 * address is the linker's to know. */
int spasm_symform_abs(const char *stmt, struct asm_symform *f)
{
    static const char *const v9ops[] = {
        "%hh(", "%hm(", "%lm(", "%h44(", "%m44(", "%l44(", "%uhi(",
        "%ulo(", "%pc22(", "%pc10(", "%got22(", "%got10(", "%gdop", "%tgd",
        "%tldm", "%tldo", "%tie", "%tle", "%r_disp", NULL
    };
    const char *p = skip_sp(stmt), *q, *o;
    int ml = 0, slen, n;
    long add;
    memset(f, 0, sizeof *f);
    while (p[ml] && !isspace((unsigned char)p[ml]))
        ml++;
    for (int k = 0; v9ops[k]; k++)
        if ((q = strstr(p, v9ops[k])) != NULL)
            return refuse_form(f, stmt, q + 1, (int)strlen(v9ops[k]) - 2,
                               "this relocation operator is SPARC V9's or "
                               "position-independent code's; EmbLD applies "
                               "%hi/%lo (R_SPARC_HI22/LO10), call and branch "
                               "displacements and data words");
    /* set sym, rd: sethi %hi(sym) and or %lo(sym), always both */
    if (ml == 3 && !strncmp(p, "set", 3)) {
        o = skip_sp(p + 3);
        n = sym_operand(o, &slen, &add);
        if (!n || o[n] != ',')
            return 0;
        f->sym_at = (int)(o - stmt);
        f->sym_len = slen;
        f->addend = add;
        snprintf(f->encode, sizeof f->encode, "\003set 0%s", o + n);
        f->site[0].off = 0;
        f->site[0].reloc = R_SPARC_HI22;
        f->site[1].off = 4;
        f->site[1].reloc = R_SPARC_LO10;
        f->nsites = 2;
        return 1;
    }
    {
        const char *hi = strstr(p, "%hi("), *lo = strstr(p, "%lo(");
        const char *at = hi ? hi : lo;
        int is_hi = at == hi;
        if (!at || (hi && lo))
            return 0;
        o = skip_sp(at + 4);
        n = sym_operand(o, &slen, &add);
        if (!n || o[n] != ')')
            return 0;
        if ((is_hi ? strstr(at + 1, "%hi(") : strstr(at + 1, "%lo(")))
            return refuse_form(f, stmt, o, slen, "two %hi/%lo operators in "
                               "one instruction");
        if (is_hi && !(ml == 5 && !strncmp(p, "sethi", 5)))
            return refuse_form(f, stmt, o, slen, "%hi(symbol) is sethi's: "
                               "its 22 bits are what R_SPARC_HI22 fills");
        f->sym_at = (int)(o - stmt);
        f->sym_len = slen;
        f->addend = add;
        snprintf(f->encode, sizeof f->encode, "%.*s0%s", (int)(o - stmt),
                 stmt, o + n);
        f->site[0].off = 0;
        f->site[0].reloc = is_hi ? R_SPARC_HI22 : R_SPARC_LO10;
        f->nsites = 1;
        return 1;
    }
}

/* ...and a call or a branch to a symbol this file does not resolve --
 * one defined elsewhere, or in another section: R_SPARC_WDISP30 and
 * R_SPARC_WDISP22, a word displacement the linker fills. */
int spasm_symform(const char *stmt, struct asm_symform *f)
{
    const char *p = skip_sp(stmt), *o;
    char mn[24];
    int ml = 0, slen, n, annul = 0;
    long add;
    while (p[ml] && !isspace((unsigned char)p[ml]))
        ml++;
    if (ml > 0 && ml < (int)sizeof mn) {
        for (int i = 0; i < ml; i++)
            mn[i] = (char)tolower((unsigned char)p[i]);
        mn[ml] = 0;
        if (ml > 2 && mn[ml - 2] == ',' && mn[ml - 1] == 'a') {
            annul = 1;
            mn[ml - 2] = 0;
        }
        o = skip_sp(p + ml);
        n = sym_operand(o, &slen, &add);
        if (n && !strcmp(mn, "call") && !annul &&
            (o[n] == 0 || o[n] == ',')) {
            memset(f, 0, sizeof *f);
            f->sym_at = (int)(o - stmt);
            f->sym_len = slen;
            f->addend = add;
            snprintf(f->encode, sizeof f->encode, "call .+0%s", o + n);
            f->site[0].reloc = R_SPARC_WDISP30;
            f->nsites = 1;
            return 1;
        }
        if (n && o[n] == 0 && mn[0] == 'b' && cond_of(mn + 1) >= 0) {
            memset(f, 0, sizeof *f);
            f->sym_at = (int)(o - stmt);
            f->sym_len = slen;
            f->addend = add;
            snprintf(f->encode, sizeof f->encode, "%.*s .+0", ml, p);
            f->site[0].reloc = R_SPARC_WDISP22;
            f->nsites = 1;
            return 1;
        }
    }
    return spasm_symform_abs(stmt, f);
}

/* ---- the referee's input --------------------------------------------------------
 *
 * Each line is a statement, an @, and the mnemonic llvm-objdump prints
 * for it -- written from the same table entry, as what the instruction
 * IS (llvm-objdump's aliases: mov, cmp, tst, ret, retl, jmp, call, nop,
 * stbar, the canonical condition names), so a wrong opcode reads back as
 * another mnemonic. A statement of two instructions (`set`) lists both,
 * joined by '+'. Every register field is varied so that a field put in
 * the wrong place shows. */
static const char *const rg[32] = {
    "%g0", "%g1", "%g2", "%g3", "%g4", "%g5", "%g6", "%g7",
    "%o0", "%o1", "%o2", "%o3", "%o4", "%o5", "%sp", "%o7",
    "%l0", "%l1", "%l2", "%l3", "%l4", "%l5", "%l6", "%l7",
    "%i0", "%i1", "%i2", "%i3", "%i4", "%i5", "%fp", "%i7"
};

/* what llvm-objdump calls format-3 op 10 with these fields */
static const char *alu_shown(const char *name, int op3, int rd, int rs1,
                             int isimm, int rs2)
{
    if (op3 == SP_OR && rs1 == SP_G0)
        return "mov";
    if (op3 == SP_SUBCC && rd == SP_G0)
        return "cmp";
    if (op3 == SP_ORCC && rd == SP_G0 && !isimm && rs2 == SP_G0)
        return "tst";
    return name;
}

void spasm_vocabulary(FILE *f)
{
    static const long long imms[] = { 0, 1, -1, 4095, -4096, 1234, -777 };
    int j, k;
    /* the ALU, register and immediate forms */
    for (const struct alu_ent *e = alu_tab; e->name; e++) {
        int sh = e->op3 == SP_SLL || e->op3 == SP_SRL || e->op3 == SP_SRA;
        for (j = 0; j < 32; j += 5) {
            int rd = (j + 3) % 32, rs1 = (j + 11) % 32, rs2 = j;
            fprintf(f, "%s %s, %s, %s@%s\n", e->name, rg[rs1], rg[rs2],
                    rg[rd], alu_shown(e->name, e->op3, rd, rs1, 0, rs2));
        }
        for (k = 0; k < (int)(sizeof imms / sizeof imms[0]); k++) {
            long long v = sh ? (imms[k] & 31) : imms[k];
            int rd = (k * 7 + 9) % 32, rs1 = (k * 5 + 17) % 32;
            fprintf(f, "%s %s, %lld, %s@%s\n", e->name, rg[rs1], v, rg[rd],
                    alu_shown(e->name, e->op3, rd, rs1, 1, 0));
        }
    }
    fprintf(f, "sll %%o1, 31, %%o2@sll\nsrl %%o1, 31, %%o2@srl\n"
               "sra %%o1, 31, %%o2@sra\n");
    fprintf(f, "save %%sp, -96, %%sp@save\nsave@save\nrestore@restore\n"
               "restore %%o0, 1, %%o0@restore\n");
    /* loads and stores, every address form */
    for (const struct mem_ent *e = mem_tab; e->name; e++) {
        static const char *const ad[] = {
            "[%o0]", "[%l1 + %i2]", "[%fp - 8]", "[%sp + 4095]",
            "[%g1 - 4096]", "[%i7 + %lo(0x12345678)]", "[64]",
            "[%r5 + %r27]"
        };
        for (k = 0; k < (int)(sizeof ad / sizeof ad[0]); k++) {
            int r = e->even ? (k * 6 + 2) % 32 & ~1 : (k * 9 + 1) % 32;
            if (e->store)
                fprintf(f, "%s %s, %s@%s\n", e->name, rg[r], ad[k], e->name);
            else
                fprintf(f, "%s %s, %s@%s\n", e->name, ad[k], rg[r], e->name);
        }
        for (k = 0; k < 3; k++) {
            static const char *const aa[] = {
                "[%o0 + %o1] 10", "[%l3] 255", "[%g7 + %i4] 0"
            };
            int r = e->even ? (k * 10 + 4) % 32 & ~1 : (k * 13 + 3) % 32;
            if (e->store)
                fprintf(f, "%sa %s, %s@%sa\n", e->name, rg[r], aa[k],
                        e->name);
            else
                fprintf(f, "%sa %s, %s@%sa\n", e->name, aa[k], rg[r],
                        e->name);
        }
    }
    fprintf(f, "casa [%%o0] 10, %%o1, %%o2@casa\n"
               "casa [%%i5] 11, %%l3, %%g7@casa\n"
               "clr [%%o0 + 4]@st\nclrb [%%l2]@stb\nclrh [%%i3 + %%o4]@sth\n"
               "clr %%l5@mov\n");
    /* sethi, set, unimp, nop, stbar */
    fprintf(f, "sethi 0, %%g1@sethi\nsethi 0x3fffff, %%o5@sethi\n"
               "sethi %%hi(0x12345678), %%l0@sethi\n"
               "set 5, %%g1@mov\nset -4096, %%o2@mov\n"
               "set 0x400000, %%l3@sethi\n"
               "set 0x12345678, %%i4@sethi+or\n"
               "set -4097, %%o0@sethi+or\n"
               "unimp 0@unimp\nunimp 0x3fffff@unimp\nunimp 12@unimp\n"
               "nop@nop\nstbar@stbar\n");
    /* the synthetic instructions */
    fprintf(f, "mov %%o1, %%o2@mov\nmov 4095, %%l7@mov\nmov -1, %%i0@mov\n"
               "cmp %%o1, %%o2@cmp\ncmp %%l0, -4096@cmp\n"
               "tst %%o3@tst\ntst %%i7@tst\n"
               "not %%o1, %%o2@xnor\nnot %%l4@xnor\n"
               "neg %%o1, %%o2@sub\nneg %%l4@sub\n"
               "inc %%o1@add\ninc 100, %%l1@add\ninccc %%o2@addcc\n"
               "inccc -5, %%i2@addcc\ndec %%o3@sub\ndec 4095, %%g4@sub\n"
               "deccc %%o4@subcc\ndeccc 7, %%l6@subcc\n"
               "btst 4, %%o1@andcc\nbtst %%l2, %%i3@andcc\n"
               "bset 8, %%o1@or\nbset %%g2, %%i4@or\n"
               "bclr 16, %%o1@andn\nbclr %%g3, %%l5@andn\n"
               "btog 32, %%o1@xor\nbtog %%g4, %%l6@xor\n");
    /* the state registers */
    {
        static const char *const sr[] = { "%y", "%psr", "%wim", "%tbr" };
        for (k = 0; k < 4; k++) {
            fprintf(f, "rd %s, %s@rd\n", sr[k], rg[(k * 7 + 8) % 32]);
            fprintf(f, "wr %s, %s, %s@wr\n", rg[(k * 5 + 9) % 32],
                    rg[(k * 3 + 17) % 32], sr[k]);
            fprintf(f, "wr %s, %d, %s@wr\n", rg[(k * 5 + 10) % 32],
                    k * 1000 - 2000, sr[k]);
            fprintf(f, "wr %s, %s@wr\n", rg[(k * 3 + 20) % 32], sr[k]);
            fprintf(f, "mov %s, %s@rd\n", sr[k], rg[(k * 7 + 3) % 32]);
            fprintf(f, "mov %s, %s@wr\n", rg[(k * 7 + 4) % 32], sr[k]);
            fprintf(f, "mov %d, %s@wr\n", k + 7, sr[k]);
        }
        for (k = 1; k < 32; k += 3) {
            fprintf(f, "rd %%asr%d, %s@%s\n", k, rg[(k + 8) % 32],
                    k == 15 && (k + 8) % 32 == 0 ? "stbar" : "rd");
            fprintf(f, "wr %s, %s, %%asr%d@wr\n", rg[(k + 1) % 32],
                    rg[(k + 2) % 32], k);
            fprintf(f, "wr %s, %d, %%asr%d@wr\n", rg[(k + 3) % 32], k * 3,
                    k);
        }
        fprintf(f, "rd %%asr17, %%o0@rd\nrd %%asr15, %%o0@rd\n"
                   "mov %%asr17, %%l0@rd\nmov %%o3, %%asr19@wr\n");
    }
    /* the transfers */
    for (k = 0; cond_tab[k].name; k++) {
        const char *shown = cond_tab[k].name;
        for (j = 0; j < 16; j++)
            if (cond_tab[j].cond == cond_tab[k].cond) {
                shown = cond_tab[j].name;
                break;
            }
        fprintf(f, "b%s .+8@b%s\n", cond_tab[k].name, shown);
        fprintf(f, "b%s,a .-4096@b%s,a\n", cond_tab[k].name, shown);
        fprintf(f, "b%s .+8388604@b%s\n", cond_tab[k].name, shown);
        fprintf(f, "b%s,a .-8388608@b%s,a\n", cond_tab[k].name, shown);
        fprintf(f, "t%s %d@t%s\n", cond_tab[k].name, (k * 9) % 128, shown);
        fprintf(f, "t%s %s@t%s\n", cond_tab[k].name, rg[(k * 3 + 1) % 32],
                shown);
        fprintf(f, "t%s %s + %s@t%s\n", cond_tab[k].name,
                rg[(k * 5 + 2) % 32], rg[(k * 7 + 3) % 32], shown);
        fprintf(f, "t%s %s + %d@t%s\n", cond_tab[k].name,
                rg[(k * 11 + 4) % 32], 127 - k, shown);
    }
    fprintf(f, "ba .@ba\nta 127@ta\n"
               "call .+16@call\ncall .-2147483648@call\n"
               "call .+2147483644@call\ncall .@call\n"
               "call %%o2@call\ncall %%l3 + 8@call\ncall %%g1 + %%g2@call\n"
               "jmpl %%o2, %%g0@jmp\njmpl %%i7 + 8, %%g0@ret\n"
               "jmpl %%o7 + 8, %%g0@retl\njmpl %%o2 + %%o3, %%g1@jmpl\n"
               "jmpl %%l4 - 4096, %%o7@call\njmpl %%i1 + 12, %%l2@jmpl\n"
               "jmp %%o2@jmp\njmp %%l3 + 4@jmp\njmp %%g1 + %%g2@jmp\n"
               "ret@ret\nretl@retl\n"
               "rett %%l2 + 4@rett\nrett %%l1@rett\nrett %%i3 + %%i4@rett\n"
               "flush %%g1@flush\nflush %%o2 + 8@flush\n"
               "flush %%l3 + %%l4@flush\niflush %%g1@flush\n");
}
