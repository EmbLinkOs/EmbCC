/* The 32-bit PowerPC assembler. See asm.h. All this file adds to emit.c
 * is a parser: every range is checked HERE, before the encoder is
 * called, so a template's mistake is a diagnostic and not an internal
 * error. */
#include "asm.h"

#include "emit.h"
#include "../asmexpr.h"
#include "../../elf/elf.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FAIL(...)  do { snprintf(err, (size_t)errlen, __VA_ARGS__); \
                        return -1; } while (0)

/* What the last ppcasm_assemble named and whether it links. */
static unsigned long g_named;
static int g_links;

unsigned long ppcasm_named(void) { return g_named; }
int ppcasm_links(void) { return g_links; }

/* ---- registers ------------------------------------------------------------ */

static int ncase_is(const char *a, int alen, const char *b)
{
    if ((int)strlen(b) != alen)
        return 0;
    for (int i = 0; i < alen; i++)
        if (tolower((unsigned char)a[i]) != b[i])
            return 0;
    return 1;
}

/* rN / %rN / sp: the register; -1 when the text is none of those */
static int reg_name(const char *name, int len)
{
    int v = 0;
    if (len > 0 && name[0] == '%') {
        name++;
        len--;
    }
    if (ncase_is(name, len, "sp"))
        return 1;
    if (ncase_is(name, len, "rtoc"))
        return 2;
    if (len < 2 || len > 3 || (name[0] != 'r' && name[0] != 'R'))
        return -1;
    for (int i = 1; i < len; i++) {
        if (!isdigit((unsigned char)name[i]))
            return -1;
        v = v * 10 + (name[i] - '0');
    }
    if (len == 3 && name[1] == '0')
        return -1;
    return v < 32 ? v : -1;
}

int ppcasm_gpr(const char *name, int len)
{
    int r = reg_name(name, len);
    if (r >= 0)
        return r;
    if (len >= 1 && len <= 2 && isdigit((unsigned char)name[0]) &&
        (len == 1 || isdigit((unsigned char)name[1])) &&
        !(len == 2 && name[0] == '0')) {
        int v = atoi(name);
        return v < 32 ? v : -1;
    }
    return -1;
}

int ppcasm_is_reg(const char *name, int len)
{
    return reg_name(name, len);
}

/* ---- special-purpose registers --------------------------------------------
 *
 * By the names GNU as gives them for the e500 (Book III-E) and the classic
 * cores; each is also the mfNAME / mtNAME extended mnemonic. `rd` and
 * `wr` say which way it goes: the time base is READ at 268/269 and
 * WRITTEN at 284/285, and SPRG4-7 have user read-only copies at 260-263,
 * which mfsprg4-7 read, as GNU's do. */
struct spr_ent { const char *name; int num; int wr; };
static const struct spr_ent spr_tab[] = {
    { "xer", 1, 1 }, { "lr", 8, 1 }, { "ctr", 9, 1 }, { "dsisr", 18, 1 },
    { "dar", 19, 1 }, { "dec", 22, 1 }, { "sdr1", 25, 1 },
    { "srr0", 26, 1 }, { "srr1", 27, 1 }, { "pid", 48, 1 },
    { "decar", 54, 1 }, { "csrr0", 58, 1 }, { "csrr1", 59, 1 },
    { "dear", 61, 1 }, { "esr", 62, 1 }, { "ivpr", 63, 1 },
    { "usprg0", 256, 1 }, { "tbl", 268, 0 }, { "tbu", 269, 0 },
    { "sprg0", 272, 1 }, { "sprg1", 273, 1 }, { "sprg2", 274, 1 },
    { "sprg3", 275, 1 }, { "sprg4", 276, 1 }, { "sprg5", 277, 1 },
    { "sprg6", 278, 1 }, { "sprg7", 279, 1 }, { "ear", 282, 1 },
    { "tbwl", 284, 1 }, { "tbwu", 285, 1 }, { "pir", 286, 1 },
    { "pvr", 287, 0 }, { "dbsr", 304, 1 }, { "dbcr0", 308, 1 },
    { "dbcr1", 309, 1 }, { "dbcr2", 310, 1 }, { "iac1", 312, 1 },
    { "iac2", 313, 1 }, { "dac1", 316, 1 }, { "dac2", 317, 1 },
    { "tsr", 336, 1 }, { "tcr", 340, 1 },
    { "ivor0", 400, 1 }, { "ivor1", 401, 1 }, { "ivor2", 402, 1 },
    { "ivor3", 403, 1 }, { "ivor4", 404, 1 }, { "ivor5", 405, 1 },
    { "ivor6", 406, 1 }, { "ivor7", 407, 1 }, { "ivor8", 408, 1 },
    { "ivor9", 409, 1 }, { "ivor10", 410, 1 }, { "ivor11", 411, 1 },
    { "ivor12", 412, 1 }, { "ivor13", 413, 1 }, { "ivor14", 414, 1 },
    { "ivor15", 415, 1 }, { "spefscr", 512, 1 }, { "ivor32", 528, 1 },
    { "ivor33", 529, 1 }, { "ivor34", 530, 1 }, { "ivor35", 531, 1 },
    { "mcsrr0", 570, 1 }, { "mcsrr1", 571, 1 }, { "mcsr", 572, 1 },
    { "mcar", 573, 1 }, { "mas0", 624, 1 }, { "mas1", 625, 1 },
    { "mas2", 626, 1 }, { "mas3", 627, 1 }, { "mas4", 628, 1 },
    { "mas6", 630, 1 }, { "pid1", 633, 1 }, { "pid2", 634, 1 },
    { "tlb0cfg", 688, 0 }, { "tlb1cfg", 689, 0 }, { "mas7", 944, 1 },
    { "hid0", 1008, 1 }, { "hid1", 1009, 1 }, { "l1csr0", 1010, 1 },
    { "l1csr1", 1011, 1 }, { "mmucsr0", 1012, 1 }, { "bucsr", 1013, 1 },
    { "mmucfg", 1015, 0 }, { "svr", 1023, 0 },
    { NULL, 0, 0 }
};

int ppcasm_spr(const char *name, int len)
{
    for (const struct spr_ent *e = spr_tab; e->name; e++)
        if (ncase_is(name, len, e->name))
            return e->num;
    return -1;
}

/* ---- a tiny tokeniser ------------------------------------------------------ */

#define MAXTOK 8

struct tok { const char *s; int len; };

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
            if (stmt[i] == '(') depth++;
            else if (stmt[i] == ')') depth--;
            i++;
        }
        e = i;
        while (e > b && isspace((unsigned char)stmt[e - 1]))
            e--;
        t[n].s = stmt + b;
        t[n].len = e - b;
        n++;
        if (i < len)
            i++;
    }
    return n;
}

static int is_identc(int c) { return isalnum(c) || c == '_' || c == '.' || c == '$'; }

/* A general register: %_N (an operand irgen wrote), rN, %rN, sp, or a
 * number 0..31. 0 when the token is none of those. */
static int tok_gpr(const struct tok *t, int *r)
{
    long long v;
    if (t->len >= 3 && t->s[0] == '%' && t->s[1] == '_') {
        if (!asm_const_expr(t->s + 2, t->len - 2, &v) || v < 0 || v > 31)
            return 0;
        *r = (int)v;
        return 1;
    }
    *r = reg_name(t->s, t->len);
    if (*r < 0) {
        if (!asm_const_expr(t->s, t->len, &v) || v < 0 || v > 31)
            return 0;
        *r = (int)v;
    }
    g_named |= 1UL << *r;
    return 1;
}

/* The text of t with the CR names -- crN, lt, gt, eq, so, un -- written
 * as their numbers, then evaluated: a CR field or a CR bit. */
static int tok_crexpr(const struct tok *t, long long *out)
{
    char buf[160];
    int o = 0;
    for (int i = 0; i < t->len; ) {
        if (o > (int)sizeof buf - 8)
            return 0;
        if (isalpha((unsigned char)t->s[i]) &&
            (i == 0 || !is_identc((unsigned char)t->s[i - 1]))) {
            int j = i, v = -1;
            while (j < t->len && is_identc((unsigned char)t->s[j]))
                j++;
            if (j - i == 3 && tolower((unsigned char)t->s[i]) == 'c' &&
                tolower((unsigned char)t->s[i + 1]) == 'r' &&
                t->s[i + 2] >= '0' && t->s[i + 2] <= '7')
                v = t->s[i + 2] - '0';
            else if (ncase_is(t->s + i, j - i, "lt")) v = 0;
            else if (ncase_is(t->s + i, j - i, "gt")) v = 1;
            else if (ncase_is(t->s + i, j - i, "eq")) v = 2;
            else if (ncase_is(t->s + i, j - i, "so") ||
                     ncase_is(t->s + i, j - i, "un")) v = 3;
            if (v < 0)
                return 0;
            o += snprintf(buf + o, sizeof buf - (size_t)o, "%d", v);
            i = j;
            continue;
        }
        buf[o++] = t->s[i++];
    }
    return asm_const_expr(buf, o, out);
}

/* A constant, with an optional @ha, @h or @l: *at says which (0 none,
 * 'a', 'h', 'l'); the value is then the 16-bit half, sign-extended. */
static int tok_imm(const struct tok *t, long long *out, int *at)
{
    int len = t->len;
    *at = 0;
    for (int i = len - 1; i > 0; i--)
        if (t->s[i] == '@') {
            const char *o = t->s + i + 1;
            int ol = len - i - 1;
            long long v;
            if (ncase_is(o, ol, "ha")) *at = 'a';
            else if (ncase_is(o, ol, "h")) *at = 'h';
            else if (ncase_is(o, ol, "l")) *at = 'l';
            else return 0;
            if (!asm_const_expr(t->s, i, &v))
                return 0;
            v &= 0xffffffffLL;
            v = *at == 'a' ? ((v + 0x8000) >> 16) & 0xffff
              : *at == 'h' ? (v >> 16) & 0xffff : v & 0xffff;
            *out = v >= 0x8000 ? v - 0x10000 : v;
            return 1;
        }
    return asm_const_expr(t->s, len, out);
}

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

static int names_symbol(const struct tok *t)
{
    for (int i = 0; i < t->len; i++)
        if ((isalpha((unsigned char)t->s[i]) || t->s[i] == '_') &&
            (i == 0 || !is_identc((unsigned char)t->s[i - 1])) &&
            t->s[i - (i > 0)] != '@' && !(i > 0 && t->s[i - 1] == '%')) {
            int j = i;
            while (j < t->len && is_identc((unsigned char)t->s[j]))
                j++;
            if (reg_name(t->s + i, j - i) < 0 &&
                !(i > 0 && t->s[i - 1] == '0' && (t->s[i] == 'x' ||
                                                  t->s[i] == 'b')))
                return 1;
            i = j;
        }
    return 0;
}

static int bad_operand(const struct tok *t, const char *mn, const char *want,
                       char *err, int errlen)
{
    for (int i = 0; i + 1 < t->len; i++)
        if (t->s[i] == '@' && isalpha((unsigned char)t->s[i + 1]) &&
            !ncase_is(t->s + i + 1, t->len - i - 1, "ha") &&
            !ncase_is(t->s + i + 1, t->len - i - 1, "h") &&
            !ncase_is(t->s + i + 1, t->len - i - 1, "l"))
            FAIL("%s: \"%.*s\": the relocation operator is not one EmbLD "
                 "applies (@ha, @h and @l are)", mn, t->len, t->s);
    if (names_symbol(t))
        FAIL("%s: \"%.*s\" names a symbol, and inline asm cannot reach one: "
             "a template carries no relocation (load the address into a "
             "register operand, or write it in a .S file)", mn, t->len,
             t->s);
    FAIL("%s: \"%.*s\" is not %s", mn, t->len, t->s, want);
}

static int need_gpr(const struct tok *t, int *r, const char *mn, char *err,
                    int errlen)
{
    if (!tok_gpr(t, r))
        return bad_operand(t, mn, "a general register (r0-r31)", err, errlen);
    return 0;
}

/* A 16-bit immediate: `sign` a signed field (-32768..32767), else an
 * unsigned one (0..65535); an @ operator's half fits either. */
static int need_imm16(const struct tok *t, long long *v, int sign,
                      const char *mn, char *err, int errlen)
{
    int at;
    if (!tok_imm(t, v, &at))
        return bad_operand(t, mn, "a constant", err, errlen);
    if (at) {
        *v &= 0xffff;
        if (sign && *v >= 0x8000)
            *v -= 0x10000;
        return 0;
    }
    if (sign ? (*v < -32768 || *v > 32767) : (*v < 0 || *v > 65535))
        FAIL("%s: %lld does not fit %s 16-bit field (%s)", mn, *v,
             sign ? "a signed" : "an unsigned",
             sign ? "-32768..32767" : "0..65535");
    return 0;
}

static int need_range(const struct tok *t, long long *v, long long lo,
                      long long hi, const char *mn, const char *what,
                      char *err, int errlen)
{
    int at;
    if (!tok_imm(t, v, &at) || at)
        return bad_operand(t, mn, "a constant", err, errlen);
    if (*v < lo || *v > hi)
        FAIL("%s: the %s %lld is not %lld..%lld", mn, what, *v, lo, hi);
    return 0;
}

static int need_crf(const struct tok *t, int *f, const char *mn, char *err,
                    int errlen)
{
    long long v;
    if (!tok_crexpr(t, &v))
        FAIL("%s: \"%.*s\" is not a CR field (cr0-cr7)", mn, t->len, t->s);
    if (v < 0 || v > 7)
        FAIL("%s: CR field %lld is not 0..7", mn, v);
    *f = (int)v;
    return 0;
}

static int need_crbit(const struct tok *t, int *b, const char *mn, char *err,
                      int errlen)
{
    long long v;
    if (!tok_crexpr(t, &v))
        return bad_operand(t, mn, "a CR bit (0..31, or 4*crN+lt/gt/eq/so)",
                           err, errlen);
    if (v < 0 || v > 31)
        FAIL("%s: CR bit %lld is not 0..31", mn, v);
    *b = (int)v;
    return 0;
}

/* d(rA) */
static int need_mem(const struct tok *t, long long *d, int *ra,
                    const char *mn, char *err, int errlen)
{
    int depth = 0, open = -1;
    struct tok dt, rt;
    if (t->len < 3 || t->s[t->len - 1] != ')')
        return bad_operand(t, mn, "a memory operand, d(rA)", err, errlen);
    for (int i = t->len - 1; i >= 0; i--) {
        if (t->s[i] == ')') depth++;
        else if (t->s[i] == '(' && --depth == 0) {
            open = i;
            break;
        }
    }
    if (open < 0)
        return bad_operand(t, mn, "a memory operand, d(rA)", err, errlen);
    rt.s = t->s + open + 1;
    rt.len = t->len - open - 2;
    while (rt.len > 0 && isspace((unsigned char)*rt.s)) { rt.s++; rt.len--; }
    while (rt.len > 0 && isspace((unsigned char)rt.s[rt.len - 1])) rt.len--;
    if (need_gpr(&rt, ra, mn, err, errlen))
        return -1;
    dt.s = t->s;
    dt.len = open;
    while (dt.len > 0 && isspace((unsigned char)dt.s[dt.len - 1])) dt.len--;
    if (dt.len == 0) {
        *d = 0;
        return 0;
    }
    return need_imm16(&dt, d, 1, mn, err, errlen);
}

/* ---- the instruction tables ------------------------------------------------- */

/* XO-form arithmetic, rt, ra, rb (`swap`: the simplified sub/subc, whose
 * operands are subf's turned round). `oe`: has the o forms. */
struct xo_ent { const char *name; int xo; int oe; int swap; int nops; };
static const struct xo_ent xo_tab[] = {
    { "add", PPC_X_ADD, 1, 0, 3 }, { "addc", PPC_X_ADDC, 1, 0, 3 },
    { "adde", PPC_X_ADDE, 1, 0, 3 }, { "subf", PPC_X_SUBF, 1, 0, 3 },
    { "subfc", PPC_X_SUBFC, 1, 0, 3 }, { "subfe", PPC_X_SUBFE, 1, 0, 3 },
    { "mullw", PPC_X_MULLW, 1, 0, 3 }, { "divw", PPC_X_DIVW, 1, 0, 3 },
    { "divwu", PPC_X_DIVWU, 1, 0, 3 }, { "mulhw", PPC_X_MULHW, 0, 0, 3 },
    { "mulhwu", PPC_X_MULHWU, 0, 0, 3 }, { "sub", PPC_X_SUBF, 0, 1, 3 },
    { "subc", PPC_X_SUBFC, 0, 1, 3 },
    { "addze", PPC_X_ADDZE, 1, 0, 2 }, { "addme", PPC_X_ADDME, 1, 0, 2 },
    { "subfze", PPC_X_SUBFZE, 1, 0, 2 }, { "subfme", PPC_X_SUBFME, 1, 0, 2 },
    { "neg", PPC_X_NEG, 1, 0, 2 },
    { NULL, 0, 0, 0, 0 }
};

/* X-form logic and shifts: ra, rs, rb (rs in the first field); the
 * two-operand ones ra, rs. */
struct xl_ent { const char *name; int xo; int nops; };
static const struct xl_ent xlog_tab[] = {
    { "and", PPC_X_AND, 3 }, { "andc", PPC_X_ANDC, 3 }, { "or", PPC_X_OR, 3 },
    { "orc", PPC_X_ORC, 3 }, { "xor", PPC_X_XOR, 3 },
    { "nand", PPC_X_NAND, 3 }, { "nor", PPC_X_NOR, 3 },
    { "eqv", PPC_X_EQV, 3 }, { "slw", PPC_X_SLW, 3 }, { "srw", PPC_X_SRW, 3 },
    { "sraw", PPC_X_SRAW, 3 }, { "cntlzw", PPC_X_CNTLZW, 2 },
    { "extsb", PPC_X_EXTSB, 2 }, { "extsh", PPC_X_EXTSH, 2 },
    { NULL, 0, 0 }
};

/* D-form: rt, ra, imm (`logical`: ra, rs, uimm, the source first). */
struct d_ent { const char *name; int op; int sign; int logical; int rc; };
static const struct d_ent d_tab[] = {
    { "addi", PPC_OP_ADDI, 1, 0, 0 }, { "addis", PPC_OP_ADDIS, 1, 0, 0 },
    { "addic", PPC_OP_ADDIC, 1, 0, 0 }, { "addic.", PPC_OP_ADDIC_, 1, 0, 0 },
    { "mulli", PPC_OP_MULLI, 1, 0, 0 }, { "subfic", PPC_OP_SUBFIC, 1, 0, 0 },
    { "ori", PPC_OP_ORI, 0, 1, 0 }, { "oris", PPC_OP_ORIS, 0, 1, 0 },
    { "xori", PPC_OP_XORI, 0, 1, 0 }, { "xoris", PPC_OP_XORIS, 0, 1, 0 },
    { "andi.", PPC_OP_ANDI, 0, 1, 0 }, { "andis.", PPC_OP_ANDIS, 0, 1, 0 },
    { NULL, 0, 0, 0, 0 }
};

/* D-form loads and stores: rt, d(ra). `upd`: the update form. */
struct m_ent { const char *name; int op; int upd; int load; };
static const struct m_ent dmem_tab[] = {
    { "lwz", PPC_OP_LWZ, 0, 1 }, { "lwzu", PPC_OP_LWZU, 1, 1 },
    { "lbz", PPC_OP_LBZ, 0, 1 }, { "lbzu", PPC_OP_LBZU, 1, 1 },
    { "lhz", PPC_OP_LHZ, 0, 1 }, { "lhzu", PPC_OP_LHZU, 1, 1 },
    { "lha", PPC_OP_LHA, 0, 1 }, { "lhau", PPC_OP_LHAU, 1, 1 },
    { "stw", PPC_OP_STW, 0, 0 }, { "stwu", PPC_OP_STWU, 1, 0 },
    { "stb", PPC_OP_STB, 0, 0 }, { "stbu", PPC_OP_STBU, 1, 0 },
    { "sth", PPC_OP_STH, 0, 0 }, { "sthu", PPC_OP_STHU, 1, 0 },
    { "lmw", PPC_OP_LMW, 0, 1 }, { "stmw", PPC_OP_STMW, 0, 0 },
    { NULL, 0, 0, 0 }
};

/* X-form loads and stores: rt, ra, rb. */
struct x_ent { const char *name; int xo; int upd; int load; int rc; };
static const struct x_ent xmem_tab[] = {
    { "lwzx", PPC_X_LWZX, 0, 1, 0 }, { "lwzux", PPC_X_LWZUX, 1, 1, 0 },
    { "lbzx", PPC_X_LBZX, 0, 1, 0 }, { "lbzux", PPC_X_LBZUX, 1, 1, 0 },
    { "lhzx", PPC_X_LHZX, 0, 1, 0 }, { "lhzux", PPC_X_LHZUX, 1, 1, 0 },
    { "lhax", PPC_X_LHAX, 0, 1, 0 }, { "lhaux", PPC_X_LHAUX, 1, 1, 0 },
    { "stwx", PPC_X_STWX, 0, 0, 0 }, { "stwux", PPC_X_STWUX, 1, 0, 0 },
    { "stbx", PPC_X_STBX, 0, 0, 0 }, { "stbux", PPC_X_STBUX, 1, 0, 0 },
    { "sthx", PPC_X_STHX, 0, 0, 0 }, { "sthux", PPC_X_STHUX, 1, 0, 0 },
    { "lwbrx", PPC_X_LWBRX, 0, 1, 0 }, { "lhbrx", PPC_X_LHBRX, 0, 1, 0 },
    { "stwbrx", PPC_X_STWBRX, 0, 0, 0 }, { "sthbrx", PPC_X_STHBRX, 0, 0, 0 },
    { "lwarx", PPC_X_LWARX, 0, 1, 0 }, { "stwcx.", PPC_X_STWCX, 0, 0, 1 },
    { NULL, 0, 0, 0, 0 }
};

/* X-form: ra, rb (the cache and TLB operations) and no operands. */
static const struct { const char *name; int xo; } xab_tab[] = {
    { "dcbf", PPC_X_DCBF }, { "dcbst", PPC_X_DCBST }, { "dcbt", PPC_X_DCBT },
    { "dcbtst", PPC_X_DCBTST }, { "dcbz", PPC_X_DCBZ },
    { "dcbi", PPC_X_DCBI }, { "icbi", PPC_X_ICBI },
    { "tlbsx", PPC_X_TLBSX }, { "tlbivax", PPC_X_TLBIVAX },
    { NULL, 0 }
};
static const struct { const char *name; int op; int xo; int rt; } x0_tab[] = {
    { "sync", PPC_OP_31, PPC_X_SYNC, 0 }, { "msync", PPC_OP_31, PPC_X_SYNC, 0 },
    { "lwsync", PPC_OP_31, PPC_X_SYNC, 1 },
    { "eieio", PPC_OP_31, PPC_X_MBAR, 0 },
    { "tlbwe", PPC_OP_31, PPC_X_TLBWE, 0 }, { "tlbre", PPC_OP_31, PPC_X_TLBRE, 0 },
    { "tlbsync", PPC_OP_31, PPC_X_TLBSYNC, 0 },
    { "isync", PPC_OP_19, PPC_XL_ISYNC, 0 }, { "rfi", PPC_OP_19, PPC_XL_RFI, 0 },
    { "rfci", PPC_OP_19, PPC_XL_RFCI, 0 }, { "rfmci", PPC_OP_19, PPC_XL_RFMCI, 0 },
    { NULL, 0, 0, 0 }
};

/* The CR logic: bt, ba, bb. */
static const struct { const char *name; int xo; } crl_tab[] = {
    { "crand", PPC_XL_CRAND }, { "cror", PPC_XL_CROR },
    { "crxor", PPC_XL_CRXOR }, { "crnand", PPC_XL_CRNAND },
    { "crnor", PPC_XL_CRNOR }, { "creqv", PPC_XL_CREQV },
    { "crandc", PPC_XL_CRANDC }, { "crorc", PPC_XL_CRORC },
    { NULL, 0 }
};

/* The simplified branch conditions: BO and the bit within the field. */
static const struct { const char *name; int bo; int bit; } bcond_tab[] = {
    { "lt", 12, 0 }, { "le", 4, 1 }, { "eq", 12, 2 }, { "ge", 4, 0 },
    { "gt", 12, 1 }, { "nl", 4, 0 }, { "ne", 4, 2 }, { "ng", 4, 1 },
    { "so", 12, 3 }, { "ns", 4, 3 }, { "un", 12, 3 }, { "nu", 4, 3 },
    { NULL, 0, 0 }
};

/* The trap conditions (TO). */
static const struct { const char *name; int to; } tcond_tab[] = {
    { "lt", 16 }, { "le", 20 }, { "eq", 4 }, { "ge", 12 }, { "gt", 8 },
    { "nl", 12 }, { "ne", 24 }, { "ng", 20 }, { "llt", 2 }, { "lle", 6 },
    { "lge", 5 }, { "lgt", 1 }, { "lnl", 5 }, { "lng", 6 }, { "u", 31 },
    { NULL, 0 }
};

static void xform(struct code *out, int op, int rt, int ra, int rb, int xo,
                  int rc)
{
    ppc_w(out, ppc_enc_x(op, rt, ra, rb, xo, rc));
}

static void dform(struct code *out, int op, int rt, int ra, long long imm)
{
    ppc_w(out, ppc_enc_d(op, rt, ra, (unsigned)(imm & 0xffff)));
}

static void mform(struct code *out, int op, int rs, int ra, int sh, int mb,
                  int me, int rc)
{
    ppc_w(out, ppc_enc_m(op, rs, ra, sh, mb, me, rc));
}

/* The forms that refuse by name. */
static const char *refused_form(const char *mn)
{
    static const char *const fp[] = {
        "lfs", "lfd", "stfs", "stfd", "lfsu", "lfdu", "stfsu", "stfdu",
        "lfsx", "lfdx", "stfsx", "stfdx", "mffs", "mtfsf", "mtfsfi",
        "mtfsb0", "mtfsb1", "mcrfs", NULL
    };
    for (int k = 0; fp[k]; k++)
        if (!strcmp(mn, fp[k]))
            return "a floating-point instruction: EmbCC compiles soft "
                   "float, and this assembler has no floating-point "
                   "vocabulary";
    if (mn[0] == 'f' && strlen(mn) > 2)
        return "a floating-point instruction: EmbCC compiles soft float, "
               "and this assembler has no floating-point vocabulary";
    if (mn[0] == 'v' || !strncmp(mn, "lvx", 3) || !strncmp(mn, "stvx", 4) ||
        !strncmp(mn, "mfvscr", 6) || !strncmp(mn, "mtvscr", 6))
        return "an AltiVec instruction, which the e500 does not have";
    if (!strncmp(mn, "ev", 2) || !strncmp(mn, "efs", 3) ||
        !strncmp(mn, "efd", 3) || !strncmp(mn, "brinc", 5))
        return "an SPE instruction: EmbCC compiles soft float and does not "
               "vectorise for SPE";
    if (!strcmp(mn, "ld") || !strcmp(mn, "std") || !strcmp(mn, "ldx") ||
        !strcmp(mn, "stdx") || !strcmp(mn, "ldu") || !strcmp(mn, "stdu") ||
        !strcmp(mn, "mulld") || !strcmp(mn, "divd") || !strcmp(mn, "rldicl") ||
        !strcmp(mn, "rldicr") || !strcmp(mn, "extsw") || !strcmp(mn, "cmpd") ||
        !strcmp(mn, "cmpdi") || !strcmp(mn, "sld") || !strcmp(mn, "srd") ||
        !strcmp(mn, "ldarx") || !strcmp(mn, "stdcx."))
        return "a 64-bit instruction, and this is 32-bit PowerPC";
    if (!strcmp(mn, "lswi") || !strcmp(mn, "stswi") || !strcmp(mn, "lswx") ||
        !strcmp(mn, "stswx"))
        return "a string instruction, which Book E (the e500) removed";
    if (!strcmp(mn, "mftb") || !strcmp(mn, "mftbu"))
        return NULL;
    if (!strcmp(mn, "mcrxr"))
        return "not an e500 instruction (Book E removed it)";
    return NULL;
}

/* A branch mnemonic, decoded: `kind` 'I' (b), 'B' (bc), 'L' (bclr) or
 * 'C' (bcctr); BO and BI (`bi` -1 when the statement gives it: bt, bf,
 * bdnzt..., or a crf for a simplified condition, `crf` 1); AA and LK. */
struct br { int kind, bo, bi, crf, aa, lk, explicit_bo; };

static int decode_branch(const char *mn, struct br *b)
{
    static const struct { const char *pfx; int bo; int needbi; int dec; } pre[] = {
        { "dnzt", 8, 1, 1 }, { "dnzf", 0, 1, 1 }, { "dzt", 10, 1, 1 },
        { "dzf", 2, 1, 1 }, { "dnz", 16, 0, 1 }, { "dz", 18, 0, 1 },
        { "t", 12, 1, 0 }, { "f", 4, 1, 0 },
        { NULL, 0, 0, 0 }
    };
    const char *r = NULL;
    memset(b, 0, sizeof *b);
    b->bi = 0;
    if (mn[0] != 'b')
        return 0;
    /* the fixed ones */
    if (!strcmp(mn, "b") || !strcmp(mn, "ba") || !strcmp(mn, "bl") ||
        !strcmp(mn, "bla")) {
        b->kind = 'I';
        b->aa = strchr(mn + 1, 'a') != NULL;
        b->lk = strchr(mn + 1, 'l') != NULL;
        return 1;
    }
    if (!strcmp(mn, "bc") || !strcmp(mn, "bca") || !strcmp(mn, "bcl") ||
        !strcmp(mn, "bcla")) {
        b->kind = 'B';
        b->explicit_bo = 1;
        b->aa = strchr(mn + 2, 'a') != NULL;
        b->lk = strchr(mn + 2, 'l') != NULL;
        return 1;
    }
    if (!strcmp(mn, "bclr") || !strcmp(mn, "bclrl") ||
        !strcmp(mn, "bcctr") || !strcmp(mn, "bcctrl")) {
        b->kind = mn[2] == 'l' ? 'L' : 'C';
        b->explicit_bo = 1;
        b->lk = !strcmp(mn, "bclrl") || !strcmp(mn, "bcctrl");
        return 1;
    }
    if (!strcmp(mn, "blr") || !strcmp(mn, "blrl") || !strcmp(mn, "bctr") ||
        !strcmp(mn, "bctrl")) {
        b->kind = mn[1] == 'l' ? 'L' : 'C';
        b->bo = 20;
        b->lk = !strcmp(mn, "blrl") || !strcmp(mn, "bctrl");
        b->bi = 0;
        return 1;
    }
    /* b<prefix|cond><suffix> */
    for (int k = 0; pre[k].pfx; k++) {
        size_t n = strlen(pre[k].pfx);
        if (!strncmp(mn + 1, pre[k].pfx, n)) {
            const char *rest = mn + 1 + n;
            if (!*rest || !strcmp(rest, "l") || !strcmp(rest, "a") ||
                !strcmp(rest, "la") || !strcmp(rest, "lr") ||
                !strcmp(rest, "lrl") ||
                (!pre[k].dec && (!strcmp(rest, "ctr") || !strcmp(rest, "ctrl")))) {
                b->bo = pre[k].bo;
                b->bi = pre[k].needbi ? -1 : 0;
                r = rest;
                break;
            }
        }
    }
    if (!r)
        for (int k = 0; bcond_tab[k].name; k++)
            if (!strncmp(mn + 1, bcond_tab[k].name, 2)) {
                const char *rest = mn + 3;
                if (*rest && strcmp(rest, "l") && strcmp(rest, "a") &&
                    strcmp(rest, "la") && strcmp(rest, "lr") &&
                    strcmp(rest, "lrl") && strcmp(rest, "ctr") &&
                    strcmp(rest, "ctrl"))
                    continue;
                b->bo = bcond_tab[k].bo;
                b->bi = bcond_tab[k].bit;
                b->crf = 1;
                r = rest;
                break;
            }
    if (!r)
        return 0;
    if (!strncmp(r, "lr", 2)) {
        b->kind = 'L';
        b->lk = r[2] == 'l';
    } else if (!strncmp(r, "ctr", 3)) {
        b->kind = 'C';
        b->lk = r[3] == 'l';
    } else {
        b->kind = 'B';
        b->lk = strchr(r, 'l') != NULL;
        b->aa = strchr(r, 'a') != NULL;
    }
    return 1;
}

/* One statement. */
static int stmt_body(const char *stmt, int len, struct code *out, char *err,
                     int errlen)
{
    struct tok t[MAXTOK];
    int n, rt, ra, rb, rc = 0, oe = 0;
    long long v, w, x, rel;
    char mn[32];
    int ml;

    while (len > 0 && isspace((unsigned char)*stmt)) {
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
        FAIL("asm instruction \"%.*s\" is not in the PowerPC vocabulary",
             t[0].len, t[0].s);
    for (int i = 0; i < ml; i++)
        mn[i] = (char)tolower((unsigned char)t[0].s[i]);
    mn[ml] = 0;
    for (int i = 1; i < n; i++)
        if (t[i].len == 0)
            FAIL("%s: an empty operand", mn);
    if (ml > 1 && (mn[ml - 1] == '+' || mn[ml - 1] == '-') && mn[0] == 'b')
        FAIL("%s: a branch-prediction hint (+/-) is not supported: write "
             "the branch without it", mn);
#define IS(s) (strcmp(mn, s) == 0)
#define NOPS(k, what) do { if (n != (k) + 1) \
        FAIL("%s takes %s", mn, what); } while (0)

    /* ---- the record and overflow forms ---- */
    {
        char base[32];
        int bl;
        memcpy(base, mn, (size_t)ml + 1);
        bl = ml;
        if (bl > 1 && base[bl - 1] == '.' && strcmp(base, "andi.") &&
            strcmp(base, "andis.") && strcmp(base, "addic.") &&
            strcmp(base, "stwcx.") && strcmp(base, "subic.")) {
            base[--bl] = 0;
            rc = 1;
        }
        /* XO-form arithmetic */
        for (const struct xo_ent *e = xo_tab; e->name; e++) {
            size_t el = strlen(e->name);
            int o = 0;
            if (strncmp(base, e->name, el))
                continue;
            if (base[el] == 'o' && !base[el + 1] && e->oe)
                o = 1;
            else if (base[el])
                continue;
            oe = o;
            if (e->nops == 3) {
                NOPS(3, "three registers");
                if (need_gpr(&t[1], &rt, mn, err, errlen) ||
                    need_gpr(&t[2], &ra, mn, err, errlen) ||
                    need_gpr(&t[3], &rb, mn, err, errlen))
                    return -1;
                if (e->swap) {
                    int s = ra;
                    ra = rb;
                    rb = s;
                }
            } else {
                NOPS(2, "two registers");
                if (need_gpr(&t[1], &rt, mn, err, errlen) ||
                    need_gpr(&t[2], &ra, mn, err, errlen))
                    return -1;
                rb = 0;
            }
            xform(out, PPC_OP_31, rt, ra, rb, e->xo | (oe ? PPC_XO_OE : 0),
                  rc);
            return 0;
        }
        for (int k = 0; xlog_tab[k].name; k++) {
            if (strcmp(base, xlog_tab[k].name))
                continue;
            if (xlog_tab[k].nops == 3) {
                NOPS(3, "three registers: ra, rs, rb");
                if (need_gpr(&t[1], &ra, mn, err, errlen) ||
                    need_gpr(&t[2], &rt, mn, err, errlen) ||
                    need_gpr(&t[3], &rb, mn, err, errlen))
                    return -1;
            } else {
                NOPS(2, "two registers: ra, rs");
                if (need_gpr(&t[1], &ra, mn, err, errlen) ||
                    need_gpr(&t[2], &rt, mn, err, errlen))
                    return -1;
                rb = 0;
            }
            xform(out, PPC_OP_31, rt, ra, rb, xlog_tab[k].xo, rc);
            return 0;
        }
        if (!strcmp(base, "mr") || !strcmp(base, "not")) {
            NOPS(2, "two registers: ra, rs");
            if (need_gpr(&t[1], &ra, mn, err, errlen) ||
                need_gpr(&t[2], &rt, mn, err, errlen))
                return -1;
            xform(out, PPC_OP_31, rt, ra, rt,
                  base[0] == 'm' ? PPC_X_OR : PPC_X_NOR, rc);
            return 0;
        }
        if (!strcmp(base, "srawi")) {
            NOPS(3, "ra, rs and a shift");
            if (need_gpr(&t[1], &ra, mn, err, errlen) ||
                need_gpr(&t[2], &rt, mn, err, errlen) ||
                need_range(&t[3], &v, 0, 31, mn, "shift", err, errlen))
                return -1;
            xform(out, PPC_OP_31, rt, ra, (int)v, PPC_X_SRAWI, rc);
            return 0;
        }
        /* the M-forms and their extended mnemonics */
        if (!strcmp(base, "rlwinm") || !strcmp(base, "rlwimi") ||
            !strcmp(base, "rlwnm")) {
            int nm = !strcmp(base, "rlwnm");
            NOPS(5, "ra, rs, a shift (rb for rlwnm), mb and me");
            if (need_gpr(&t[1], &ra, mn, err, errlen) ||
                need_gpr(&t[2], &rt, mn, err, errlen))
                return -1;
            if (nm) {
                if (need_gpr(&t[3], &rb, mn, err, errlen))
                    return -1;
                v = rb;
            } else if (need_range(&t[3], &v, 0, 31, mn, "shift", err,
                                  errlen)) {
                return -1;
            }
            if (need_range(&t[4], &w, 0, 31, mn, "mask begin", err, errlen) ||
                need_range(&t[5], &x, 0, 31, mn, "mask end", err, errlen))
                return -1;
            mform(out, nm ? PPC_OP_RLWNM : base[4] == 'm' ? PPC_OP_RLWIMI
                                                          : PPC_OP_RLWINM,
                  rt, ra, (int)v, (int)w, (int)x, rc);
            return 0;
        }
        {
            static const char *const mx[] = {
                "rotlwi", "rotrwi", "rotlw", "slwi", "srwi", "extlwi",
                "extrwi", "inslwi", "insrwi", "clrlwi", "clrrwi", "clrlslwi",
                NULL
            };
            int k;
            for (k = 0; mx[k]; k++)
                if (!strcmp(base, mx[k]))
                    break;
            if (mx[k]) {
                int four = !strcmp(base, "extlwi") || !strcmp(base, "extrwi") ||
                           !strcmp(base, "inslwi") || !strcmp(base, "insrwi") ||
                           !strcmp(base, "clrlslwi");
                long long a3, a4 = 0;
                if (four)
                    NOPS(4, "ra, rs and two counts");
                else
                    NOPS(3, "ra, rs and a count");
                if (need_gpr(&t[1], &ra, mn, err, errlen) ||
                    need_gpr(&t[2], &rt, mn, err, errlen))
                    return -1;
                if (!strcmp(base, "rotlw")) {
                    if (need_gpr(&t[3], &rb, mn, err, errlen))
                        return -1;
                    mform(out, PPC_OP_RLWNM, rt, ra, rb, 0, 31, rc);
                    return 0;
                }
                if (need_range(&t[3], &a3, 0, 32, mn, "count", err, errlen) ||
                    (four && need_range(&t[4], &a4, 0, 31, mn, "bit", err,
                                        errlen)))
                    return -1;
                /* (n, b) per the ISA's table of extended mnemonics */
                if (!strcmp(base, "rotlwi") && a3 < 32)
                    mform(out, PPC_OP_RLWINM, rt, ra, (int)a3, 0, 31, rc);
                else if (!strcmp(base, "rotrwi") && a3 < 32)
                    mform(out, PPC_OP_RLWINM, rt, ra, (int)((32 - a3) & 31), 0,
                          31, rc);
                else if (!strcmp(base, "slwi") && a3 < 32)
                    mform(out, PPC_OP_RLWINM, rt, ra, (int)a3, 0,
                          (int)(31 - a3), rc);
                else if (!strcmp(base, "srwi") && a3 < 32)
                    mform(out, PPC_OP_RLWINM, rt, ra, (int)((32 - a3) & 31),
                          (int)a3, 31, rc);
                else if (!strcmp(base, "clrlwi") && a3 < 32)
                    mform(out, PPC_OP_RLWINM, rt, ra, 0, (int)a3, 31, rc);
                else if (!strcmp(base, "clrrwi") && a3 < 32)
                    mform(out, PPC_OP_RLWINM, rt, ra, 0, 0, (int)(31 - a3), rc);
                else if (!strcmp(base, "extlwi") && a3 > 0 && a3 + a4 <= 32)
                    mform(out, PPC_OP_RLWINM, rt, ra, (int)a4, 0,
                          (int)(a3 - 1), rc);
                else if (!strcmp(base, "extrwi") && a3 > 0 && a3 + a4 <= 32)
                    mform(out, PPC_OP_RLWINM, rt, ra, (int)((a4 + a3) & 31),
                          (int)(32 - a3), 31, rc);
                else if (!strcmp(base, "inslwi") && a3 > 0 && a3 + a4 <= 32)
                    mform(out, PPC_OP_RLWIMI, rt, ra, (int)((32 - a4) & 31),
                          (int)a4, (int)(a4 + a3 - 1), rc);
                else if (!strcmp(base, "insrwi") && a3 > 0 && a3 + a4 <= 32)
                    mform(out, PPC_OP_RLWIMI, rt, ra,
                          (int)((32 - (a4 + a3)) & 31), (int)a4,
                          (int)(a4 + a3 - 1), rc);
                else if (!strcmp(base, "clrlslwi") && a4 <= a3 && a3 < 32)
                    mform(out, PPC_OP_RLWINM, rt, ra, (int)a4, (int)(a3 - a4),
                          (int)(31 - a4), rc);
                else
                    FAIL("%s: the counts are out of range (n %lld, b %lld)",
                         mn, a3, a4);
                return 0;
            }
        }
        if (rc)                    /* a `.` none of the above took */
            FAIL("asm instruction \"%s\" is not in the PowerPC vocabulary "
                 "(or has no record form)", mn);
    }

    /* ---- D-form immediates ---- */
    for (const struct d_ent *e = d_tab; e->name; e++) {
        if (!IS(e->name))
            continue;
        NOPS(3, "two registers and a 16-bit constant");
        if (e->logical) {
            if (need_gpr(&t[1], &ra, mn, err, errlen) ||
                need_gpr(&t[2], &rt, mn, err, errlen))
                return -1;
        } else if (need_gpr(&t[1], &rt, mn, err, errlen) ||
                   need_gpr(&t[2], &ra, mn, err, errlen)) {
            return -1;
        }
        if (e->op == PPC_OP_ADDIS) {
            int at;
            if (!tok_imm(&t[3], &v, &at))
                return bad_operand(&t[3], mn, "a constant", err, errlen);
            if (!at && (v < -32768 || v > 65535))
                FAIL("%s: %lld does not fit 16 bits", mn, v);
        } else if (need_imm16(&t[3], &v, e->sign, mn, err, errlen)) {
            return -1;
        }
        dform(out, e->op, rt, ra, v);
        return 0;
    }
    if (IS("li") || IS("lis")) {
        int at;
        NOPS(2, "a register and a constant");
        if (need_gpr(&t[1], &rt, mn, err, errlen))
            return -1;
        if (IS("li")) {
            if (need_imm16(&t[2], &v, 1, mn, err, errlen))
                return -1;
        } else {
            if (!tok_imm(&t[2], &v, &at))
                return bad_operand(&t[2], mn, "a constant", err, errlen);
            if (!at && (v < -32768 || v > 65535))
                FAIL("lis: %lld does not fit 16 bits", v);
        }
        dform(out, IS("li") ? PPC_OP_ADDI : PPC_OP_ADDIS, rt, 0, v);
        return 0;
    }
    if (IS("la")) {
        NOPS(2, "a register and d(rA)");
        if (need_gpr(&t[1], &rt, mn, err, errlen) ||
            need_mem(&t[2], &v, &ra, mn, err, errlen))
            return -1;
        dform(out, PPC_OP_ADDI, rt, ra, v);
        return 0;
    }
    if (IS("subi") || IS("subis") || IS("subic") || IS("subic.")) {
        int at;
        NOPS(3, "two registers and a constant");
        if (need_gpr(&t[1], &rt, mn, err, errlen) ||
            need_gpr(&t[2], &ra, mn, err, errlen))
            return -1;
        if (!tok_imm(&t[3], &v, &at) || at)
            return bad_operand(&t[3], mn, "a constant", err, errlen);
        v = -v;
        if (v < -32768 || v > 32767)
            FAIL("%s: %lld does not fit a signed 16-bit field", mn, -v);
        dform(out, IS("subi") ? PPC_OP_ADDI : IS("subis") ? PPC_OP_ADDIS
                  : IS("subic") ? PPC_OP_ADDIC : PPC_OP_ADDIC_, rt, ra, v);
        return 0;
    }
    if (IS("nop")) {
        NOPS(0, "no operands");
        ppc_nop(out);
        return 0;
    }

    /* ---- compares ---- */
    if (IS("cmpw") || IS("cmplw") || IS("cmpwi") || IS("cmplwi")) {
        int crf = 0, i = 1, imm = mn[ml - 1] == 'i', logical = mn[3] == 'l';
        if (n != 3 && n != 4)
            FAIL("%s takes [crN,] ra and %s", mn, imm ? "a constant"
                                                       : "rb");
        if (n == 4 && need_crf(&t[i++], &crf, mn, err, errlen))
            return -1;
        if (need_gpr(&t[i], &ra, mn, err, errlen))
            return -1;
        if (imm) {
            if (need_imm16(&t[i + 1], &v, !logical, mn, err, errlen))
                return -1;
            dform(out, logical ? PPC_OP_CMPLI : PPC_OP_CMPI, crf << 2, ra, v);
        } else {
            if (need_gpr(&t[i + 1], &rb, mn, err, errlen))
                return -1;
            xform(out, PPC_OP_31, crf << 2, ra, rb,
                  logical ? PPC_X_CMPL : PPC_X_CMP, 0);
        }
        return 0;
    }
    if (IS("cmp") || IS("cmpl") || IS("cmpi") || IS("cmpli")) {
        int crf, imm = mn[ml - 1] == 'i', logical = mn[3] == 'l';
        NOPS(4, "crN, L, ra and rb or a constant");
        if (need_crf(&t[1], &crf, mn, err, errlen) ||
            need_range(&t[2], &w, 0, 1, mn, "L", err, errlen) ||
            need_gpr(&t[3], &ra, mn, err, errlen))
            return -1;
        if (w)
            FAIL("%s: L = 1 compares doublewords, which 32-bit PowerPC "
                 "does not have", mn);
        if (imm) {
            if (need_imm16(&t[4], &v, !logical, mn, err, errlen))
                return -1;
            dform(out, logical ? PPC_OP_CMPLI : PPC_OP_CMPI, crf << 2, ra, v);
        } else {
            if (need_gpr(&t[4], &rb, mn, err, errlen))
                return -1;
            xform(out, PPC_OP_31, crf << 2, ra, rb,
                  logical ? PPC_X_CMPL : PPC_X_CMP, 0);
        }
        return 0;
    }

    /* ---- loads and stores ---- */
    for (const struct m_ent *e = dmem_tab; e->name; e++) {
        if (!IS(e->name))
            continue;
        NOPS(2, "a register and d(rA)");
        if (need_gpr(&t[1], &rt, mn, err, errlen) ||
            need_mem(&t[2], &v, &ra, mn, err, errlen))
            return -1;
        if (e->upd && (ra == 0 || (e->load && ra == rt)))
            FAIL("%s: an update form's base may not be r0%s", mn,
                 e->load ? ", nor the register it loads" : "");
        if (e->op == PPC_OP_LMW && ra >= rt && ra != 0)
            FAIL("lmw: the base r%d is among the registers loaded", ra);
        dform(out, e->op, rt, ra, v);
        return 0;
    }
    for (const struct x_ent *e = xmem_tab; e->name; e++) {
        if (!IS(e->name))
            continue;
        NOPS(3, "three registers: rt, ra, rb");
        if (need_gpr(&t[1], &rt, mn, err, errlen) ||
            need_gpr(&t[2], &ra, mn, err, errlen) ||
            need_gpr(&t[3], &rb, mn, err, errlen))
            return -1;
        if (e->upd && (ra == 0 || (e->load && ra == rt)))
            FAIL("%s: an update form's base may not be r0%s", mn,
                 e->load ? ", nor the register it loads" : "");
        xform(out, PPC_OP_31, rt, ra, rb, e->xo, e->rc);
        return 0;
    }
    for (int k = 0; xab_tab[k].name; k++) {
        if (!IS(xab_tab[k].name))
            continue;
        NOPS(2, "two registers: ra, rb");
        if (need_gpr(&t[1], &ra, mn, err, errlen) ||
            need_gpr(&t[2], &rb, mn, err, errlen))
            return -1;
        xform(out, PPC_OP_31, 0, ra, rb, xab_tab[k].xo, 0);
        return 0;
    }
    for (int k = 0; x0_tab[k].name; k++) {
        if (!IS(x0_tab[k].name))
            continue;
        NOPS(0, "no operands");
        xform(out, x0_tab[k].op, x0_tab[k].rt, 0, 0, x0_tab[k].xo, 0);
        return 0;
    }
    if (IS("mbar")) {
        v = 0;
        if (n == 2 && need_range(&t[1], &v, 0, 31, mn, "MO", err, errlen))
            return -1;
        if (n > 2)
            FAIL("mbar takes at most an MO");
        xform(out, PPC_OP_31, (int)v, 0, 0, PPC_X_MBAR, 0);
        return 0;
    }
    if (IS("sc")) {
        NOPS(0, "no operands");
        ppc_sc(out);
        return 0;
    }

    /* ---- traps ---- */
    if (IS("trap")) {
        NOPS(0, "no operands");
        ppc_trap(out);
        return 0;
    }
    if (IS("tw") || IS("twi")) {
        NOPS(3, "TO, ra and rb or a constant");
        if (need_range(&t[1], &v, 0, 31, mn, "TO", err, errlen) ||
            need_gpr(&t[2], &ra, mn, err, errlen))
            return -1;
        if (IS("tw")) {
            if (need_gpr(&t[3], &rb, mn, err, errlen))
                return -1;
            xform(out, PPC_OP_31, (int)v, ra, rb, PPC_X_TW, 0);
        } else {
            if (need_imm16(&t[3], &w, 1, mn, err, errlen))
                return -1;
            dform(out, PPC_OP_TWI, (int)v, ra, w);
        }
        return 0;
    }
    if (mn[0] == 't' && mn[1] == 'w') {
        int imm = mn[ml - 1] == 'i';
        char c[8];
        int cl = ml - 2 - imm;
        if (cl > 0 && cl < (int)sizeof c) {
            memcpy(c, mn + 2, (size_t)cl);
            c[cl] = 0;
            for (int k = 0; tcond_tab[k].name; k++) {
                if (strcmp(c, tcond_tab[k].name))
                    continue;
                NOPS(2, "ra and rb or a constant");
                if (need_gpr(&t[1], &ra, mn, err, errlen))
                    return -1;
                if (imm) {
                    if (need_imm16(&t[2], &w, 1, mn, err, errlen))
                        return -1;
                    dform(out, PPC_OP_TWI, tcond_tab[k].to, ra, w);
                } else {
                    if (need_gpr(&t[2], &rb, mn, err, errlen))
                        return -1;
                    xform(out, PPC_OP_31, tcond_tab[k].to, ra, rb, PPC_X_TW, 0);
                }
                return 0;
            }
        }
    }

    /* ---- the condition register ---- */
    for (int k = 0; crl_tab[k].name; k++) {
        int bt, ba, bb;
        if (!IS(crl_tab[k].name))
            continue;
        NOPS(3, "three CR bits");
        if (need_crbit(&t[1], &bt, mn, err, errlen) ||
            need_crbit(&t[2], &ba, mn, err, errlen) ||
            need_crbit(&t[3], &bb, mn, err, errlen))
            return -1;
        xform(out, PPC_OP_19, bt, ba, bb, crl_tab[k].xo, 0);
        return 0;
    }
    if (IS("crset") || IS("crclr")) {
        int bx;
        NOPS(1, "a CR bit");
        if (need_crbit(&t[1], &bx, mn, err, errlen))
            return -1;
        xform(out, PPC_OP_19, bx, bx, bx,
              IS("crset") ? PPC_XL_CREQV : PPC_XL_CRXOR, 0);
        return 0;
    }
    if (IS("crmove") || IS("crnot")) {
        int bx, by;
        NOPS(2, "two CR bits");
        if (need_crbit(&t[1], &bx, mn, err, errlen) ||
            need_crbit(&t[2], &by, mn, err, errlen))
            return -1;
        xform(out, PPC_OP_19, bx, by, by,
              IS("crmove") ? PPC_XL_CROR : PPC_XL_CRNOR, 0);
        return 0;
    }
    if (IS("mcrf")) {
        int bf, bfa;
        NOPS(2, "two CR fields");
        if (need_crf(&t[1], &bf, mn, err, errlen) ||
            need_crf(&t[2], &bfa, mn, err, errlen))
            return -1;
        xform(out, PPC_OP_19, bf << 2, bfa << 2, 0, PPC_XL_MCRF, 0);
        return 0;
    }
    if (IS("mfcr")) {
        NOPS(1, "a register");
        if (need_gpr(&t[1], &rt, mn, err, errlen))
            return -1;
        ppc_mfcr(out, rt);
        return 0;
    }
    if (IS("mtcrf") || IS("mtcr")) {
        if (IS("mtcr")) {
            NOPS(1, "a register");
            v = 0xff;
        } else {
            NOPS(2, "a field mask and a register");
            if (need_range(&t[1], &v, 0, 255, mn, "field mask", err, errlen))
                return -1;
        }
        if (need_gpr(&t[n - 1], &rt, mn, err, errlen))
            return -1;
        xform(out, PPC_OP_31, rt, (int)(v >> 4), (int)((v & 15) << 1),
              PPC_X_MTCRF, 0);
        return 0;
    }
    if (IS("isel") || IS("isellt") || IS("iselgt") || IS("iseleq")) {
        int bc;
        if (IS("isel")) {
            NOPS(4, "rt, ra, rb and a CR bit");
            if (need_crbit(&t[4], &bc, mn, err, errlen))
                return -1;
        } else {
            NOPS(3, "rt, ra and rb");
            bc = IS("isellt") ? 0 : IS("iselgt") ? 1 : 2;
        }
        if (need_gpr(&t[1], &rt, mn, err, errlen) ||
            need_gpr(&t[2], &ra, mn, err, errlen) ||
            need_gpr(&t[3], &rb, mn, err, errlen))
            return -1;
        xform(out, PPC_OP_31, rt, ra, rb, (bc << 5) | PPC_X_ISEL, 0);
        return 0;
    }

    /* ---- the machine state and the special-purpose registers ---- */
    if (IS("mfmsr") || IS("mtmsr") || IS("wrtee")) {
        NOPS(1, "a register");
        if (need_gpr(&t[1], &rt, mn, err, errlen))
            return -1;
        if (IS("mfmsr"))
            ppc_mfmsr(out, rt);
        else if (IS("mtmsr"))
            ppc_mtmsr(out, rt);
        else
            xform(out, PPC_OP_31, rt, 0, 0, PPC_X_WRTEE, 0);
        return 0;
    }
    if (IS("wrteei")) {
        NOPS(1, "0 or 1");
        if (need_range(&t[1], &v, 0, 1, mn, "E bit", err, errlen))
            return -1;
        ppc_wrteei(out, (int)v);
        return 0;
    }
    if (IS("mfspr") || IS("mtspr")) {
        int spr, from = IS("mfspr");
        const struct tok *st = &t[from ? 2 : 1];
        NOPS(2, from ? "a register and an SPR" : "an SPR and a register");
        spr = ppcasm_spr(st->s, st->len);
        if (spr < 0 && need_range(st, &v, 0, 1023, mn, "SPR number", err,
                                  errlen))
            return -1;
        if (spr < 0)
            spr = (int)v;
        if (need_gpr(&t[from ? 1 : 2], &rt, mn, err, errlen))
            return -1;
        if (from)
            ppc_mfspr(out, rt, spr);
        else
            ppc_mtspr(out, spr, rt);
        if (!from && spr == 8)
            g_links = 1;
        return 0;
    }
    if (IS("mfsprg") || IS("mtsprg")) {
        int from = IS("mfsprg");
        NOPS(2, from ? "a register and 0..7" : "0..7 and a register");
        if (need_range(&t[from ? 2 : 1], &v, 0, 7, mn, "SPRG", err, errlen) ||
            need_gpr(&t[from ? 1 : 2], &rt, mn, err, errlen))
            return -1;
        if (from)
            ppc_mfspr(out, rt, v < 4 ? 272 + (int)v : 256 + (int)v);
        else
            ppc_mtspr(out, 272 + (int)v, rt);
        return 0;
    }
    if (IS("mftb") || IS("mftbu")) {
        NOPS(1, "a register");
        if (need_gpr(&t[1], &rt, mn, err, errlen))
            return -1;
        ppc_mfspr(out, rt, IS("mftb") ? 268 : 269);
        return 0;
    }
    if ((mn[0] == 'm') && (mn[1] == 'f' || mn[1] == 't') && ml > 2) {
        /* mfNAME rt / mtNAME rs, and mfsprgN / mtsprgN */
        int spr = ppcasm_spr(mn + 2, ml - 2);
        if (spr >= 0) {
            const struct spr_ent *e = spr_tab;
            while (e->name && e->num != spr)
                e++;
            NOPS(1, "a register");
            if (need_gpr(&t[1], &rt, mn, err, errlen))
                return -1;
            if (mn[1] == 'f') {
                /* SPRG4-7 are read through their user copies, 260-263 */
                ppc_mfspr(out, rt, spr >= 276 && spr <= 279 ? spr - 16 : spr);
            } else {
                if (!e->wr)
                    FAIL("%s: %s cannot be written (it is read-only)", mn,
                         mn + 2);
                ppc_mtspr(out, spr, rt);
                if (spr == 8)
                    g_links = 1;
            }
            return 0;
        }
    }

    /* ---- branches ---- */
    {
        struct br b;
        if (decode_branch(mn, &b)) {
            int i = 1, bo = b.bo, bi = b.bi;
            if (b.lk)
                g_links = 1;
            if (b.kind == 'I') {
                NOPS(1, "a target");
                if (b.aa) {
                    if (need_range(&t[1], &v, -(1LL << 25), (1LL << 25) - 4,
                                   mn, "absolute target", err, errlen))
                        return -1;
                    if (v & 3)
                        FAIL("%s: the target %lld is not 4-aligned", mn, v);
                    ppc_w(out, ppc_enc_iform((long)v, 1, b.lk));
                    return 0;
                }
                if (!tok_target(&t[1], &rel))
                    return bad_operand(&t[1], mn, "a branch target (a label, "
                                       "or .+N bytes from the branch)", err,
                                       errlen);
                if (rel & 3)
                    FAIL("%s: the target .%+lld is not 4-aligned", mn, rel);
                if (rel < -(1LL << 25) || rel > (1LL << 25) - 4)
                    FAIL("%s: .%+lld is out of reach (+-32 MiB)", mn, rel);
                ppc_w(out, ppc_enc_iform((long)rel, 0, b.lk));
                return 0;
            }
            /* BO and BI, given or implied */
            if (b.explicit_bo) {
                if (n < 3)
                    FAIL("%s takes BO, BI%s", mn, b.kind == 'B' ? " and a target"
                                                                 : "");
                if (need_range(&t[1], &v, 0, 31, mn, "BO", err, errlen) ||
                    need_crbit(&t[2], &bi, mn, err, errlen))
                    return -1;
                bo = (int)v;
                i = 3;
            } else if (bi < 0) {
                if (n < 2)
                    FAIL("%s takes a CR bit%s", mn, b.kind == 'B' ? " and a "
                         "target" : "");
                if (need_crbit(&t[1], &bi, mn, err, errlen))
                    return -1;
                i = 2;
            } else if (b.crf && n == (b.kind == 'B' ? 3 : 2)) {
                int f;
                if (need_crf(&t[1], &f, mn, err, errlen))
                    return -1;
                bi += f * 4;
                i = 2;
            }
            if (b.kind == 'B') {
                if (n != i + 1)
                    FAIL("%s: the operands are [crN,] and a target", mn);
                if (b.aa) {
                    if (need_range(&t[i], &v, -32768, 32764, mn,
                                   "absolute target", err, errlen))
                        return -1;
                    rel = v;
                } else if (!tok_target(&t[i], &rel)) {
                    return bad_operand(&t[i], mn, "a branch target (a label, "
                                       "or .+N bytes from the branch)", err,
                                       errlen);
                }
                if (rel & 3)
                    FAIL("%s: the target %lld is not 4-aligned", mn, rel);
                if (rel < -32768 || rel > 32764)
                    FAIL("%s: .%+lld is out of reach (a conditional branch "
                         "reaches -32768..32764 bytes)", mn, rel);
                ppc_w(out, ppc_enc_bform(bo, bi, (long)rel, b.aa, b.lk));
                return 0;
            }
            /* bclr / bcctr, with an optional BH */
            v = 0;
            if (n == i + 1) {
                if (need_range(&t[i], &v, 0, 3, mn, "BH", err, errlen))
                    return -1;
            } else if (n != i) {
                FAIL("%s: too many operands", mn);
            }
            if (b.kind == 'C' && !(bo & 4))
                FAIL("%s: bcctr cannot decrement CTR (BO %d)", mn, bo);
            xform(out, PPC_OP_19, bo, bi, (int)v,
                  b.kind == 'L' ? PPC_XL_BCLR : PPC_XL_BCCTR, b.lk);
            return 0;
        }
    }
    {
        const char *why = refused_form(mn);
        if (why)
            FAIL("asm instruction \"%s\" is %s", mn, why);
    }
    FAIL("asm instruction \"%s\" is not in the PowerPC vocabulary", mn);
#undef IS
#undef NOPS
}

/* ---- a template: statements, numeric labels ----------------------------------- */

#define MAXSTMT 512

struct stm { const char *s; int len; int lab[4]; int nlab; int off; };

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
            if (start[i] == '#' || (start[i] == '/' && i + 1 < len &&
                                    start[i + 1] == '/')) {
                len = i;
                break;
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

static int resolve_labels(const struct stm *st, int ns, int k, char *buf,
                          int cap, char *err, int errlen)
{
    const char *s = st[k].s;
    int len = st[k].len, o = 0;
    for (int i = 0; i < len; ) {
        int j = i, target = -1;
        int mid = i > 0 && (isalnum((unsigned char)s[i - 1]) ||
                            s[i - 1] == '_' || s[i - 1] == '.');
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

int ppcasm_assemble(const char *text, struct code *out, char *err, int errlen)
{
    static struct stm st[MAXSTMT];
    int ns, off = 0, start = out->len;
    g_named = 0;
    g_links = 0;
    ns = stmts(text, st, MAXSTMT, err, errlen);
    if (ns < 0)
        return -1;
    /* every instruction is a word */
    for (int k = 0; k < ns; k++) {
        int any = 0;
        st[k].off = off;
        for (int i = 0; i < st[k].len; i++)
            any |= !isspace((unsigned char)st[k].s[i]);
        if (any)
            off += 4;
        else
            st[k].len = 0;
    }
    for (int k = 0; k < ns; k++) {
        char buf[512];
        int len, before = out->len;
        if (!st[k].len)
            continue;
        len = resolve_labels(st, ns, k, buf, (int)sizeof buf, err, errlen);
        if (len < 0 || stmt_body(buf, len, out, err, errlen))
            return -1;
        if (out->len - before != 4 || out->len - start != st[k].off + 4)
            FAIL("internal: a PowerPC statement that is not one word");
    }
    return 0;
}

/* ---- for the file assembler (src/as/gas.c) ---------------------------------- */

static const char *skip_sp(const char *p)
{
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

int ppcasm_is_word(const char *stmt, const char *w, int len)
{
    const char *p = skip_sp(stmt);
    int ml = 0;
    if (w > stmt && (w[-1] == '@' || w[-1] == '%'))
        return 1;
    if ((len == 3 && tolower((unsigned char)w[0]) == 'c' &&
         tolower((unsigned char)w[1]) == 'r' && w[2] >= '0' && w[2] <= '7') ||
        ncase_is(w, len, "lt") || ncase_is(w, len, "gt") ||
        ncase_is(w, len, "eq") || ncase_is(w, len, "so") ||
        ncase_is(w, len, "un"))
        return 1;
    while (p[ml] && !isspace((unsigned char)p[ml]))
        ml++;
    if ((ncase_is(p, ml, "mfspr") || ncase_is(p, ml, "mtspr")) &&
        ppcasm_spr(w, len) >= 0)
        return 1;
    return 0;
}

/* The symbol at p, then an optional `+K` / `-K`; its length, 0 when
 * there is none. */
static int sym_operand(const char *p, int *slen, long *add)
{
    int n = 0;
    if (!(isalpha((unsigned char)p[0]) || p[0] == '_' || p[0] == '.'))
        return 0;
    if (p[0] == '.' && !(isalnum((unsigned char)p[1]) || p[1] == '_'))
        return 0;
    while (is_identc((unsigned char)p[n]))
        n++;
    if (reg_name(p, n) >= 0)
        return 0;
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

/* SYM@ha / SYM@h / SYM@l (SYM[+-K], or (SYM[+-K])): the address halves,
 * R_PPC_ADDR16_HA / _HI / _LO on the instruction's low halfword. */
int ppcasm_symform_abs(const char *stmt, struct asm_symform *f)
{
    const char *p = skip_sp(stmt), *at = NULL;
    int ml = 0;
    memset(f, 0, sizeof *f);
    while (p[ml] && !isspace((unsigned char)p[ml]))
        ml++;
    for (const char *q = p + ml; *q; q++)
        if (*q == '@') {
            at = q;
            break;
        }
    if (!at)
        return 0;
    {
        const char *s = at, *o;
        int slen, n, op, paren = 0, ol = 0;
        long add;
        /* back to the start of the operand */
        while (s > p + ml && s[-1] != ',' && !isspace((unsigned char)s[-1]) &&
               s[-1] != '(')
            s--;
        if (at > s && at[-1] == ')') {
            /* (SYM+K)@ha */
            s = at - 1;
            while (s > p && *s != '(')
                s--;
            paren = 1;
        }
        o = s + paren;
        n = sym_operand(o, &slen, &add);
        if (!n || o + n + paren != at)
            return 0;
        while (isalnum((unsigned char)at[1 + ol]))
            ol++;
        if (ncase_is(at + 1, ol, "ha")) op = R_PPC_ADDR16_HA;
        else if (ncase_is(at + 1, ol, "h")) op = R_PPC_ADDR16_HI;
        else if (ncase_is(at + 1, ol, "l")) op = R_PPC_ADDR16_LO;
        else
            return refuse_form(f, stmt, o, slen,
                               "this relocation operator (@sdarel, @got, "
                               "@plt, @toc, a TLS one...) is not one EmbLD "
                               "applies: @ha, @h and @l are (R_PPC_ADDR16_*)");
        if (strchr(at + 1, '@'))
            return refuse_form(f, stmt, o, slen, "two @ operators in one "
                               "instruction");
        f->sym_at = (int)(o - stmt);
        f->sym_len = slen;
        f->addend = add;
        snprintf(f->encode, sizeof f->encode, "%.*s0%s", (int)(s - stmt),
                 stmt, at + 1 + ol);
        f->site[0].off = 2;
        f->site[0].reloc = op;
        f->nsites = 1;
        return 1;
    }
}

/* ...and a branch to a symbol this file does not resolve -- one defined
 * elsewhere or in another section: R_PPC_REL24 for b and bl, R_PPC_ADDR24
 * for ba and bla, R_PPC_REL14 for a conditional branch. An absolute
 * conditional branch to a symbol (R_PPC_ADDR14) is refused. */
int ppcasm_symform(const char *stmt, struct asm_symform *f)
{
    const char *p = skip_sp(stmt), *o, *c;
    char mn[24];
    int ml = 0, slen, n;
    long add;
    struct br b;
    while (p[ml] && !isspace((unsigned char)p[ml]))
        ml++;
    if (ml > 0 && ml < (int)sizeof mn) {
        for (int i = 0; i < ml; i++)
            mn[i] = (char)tolower((unsigned char)p[i]);
        mn[ml] = 0;
        if (decode_branch(mn, &b) && (b.kind == 'I' || b.kind == 'B')) {
            /* the last operand */
            o = skip_sp(p + ml);
            for (c = o; *c; c++)
                if (*c == ',')
                    o = skip_sp(c + 1);
            n = sym_operand(o, &slen, &add);
            if (n && !*skip_sp(o + n)) {
                memset(f, 0, sizeof *f);
                f->sym_at = (int)(o - stmt);
                f->sym_len = slen;
                f->addend = add;
                if (b.kind == 'B' && b.aa)
                    return refuse_form(f, stmt, o, slen, "an absolute "
                                       "conditional branch to a symbol "
                                       "(R_PPC_ADDR14) is not one EmbLD "
                                       "applies");
                snprintf(f->encode, sizeof f->encode, "%.*s%s",
                         (int)(o - stmt), stmt, b.aa ? "0" : ".+0");
                f->site[0].reloc = b.kind == 'B' ? R_PPC_REL14
                                 : b.aa ? R_PPC_ADDR24 : R_PPC_REL24;
                f->nsites = 1;
                return 1;
            }
        }
    }
    return ppcasm_symform_abs(stmt, f);
}

/* ---- the referee's input --------------------------------------------------------
 *
 * Each line is a statement, an @, and the mnemonic llvm-objdump prints
 * for it -- the instruction as it IS, by the extended mnemonic llvm-objdump
 * prefers where it has one (li, mr, nop, blr, mflr, slwi...) -- and,
 * where llvm-objdump prints a general form (bt/bf for a conditional
 * branch, bclr with BO and BI, rlwinm for extlwi, tw with TO), its
 * operands too, which the referee checks as a prefix of what it prints. Registers
 * are written as GCC writes them, bare numbers, and some as %rN; every
 * field is varied so that a field put in the wrong place shows. Then the
 * `=` lines: an SPR named in mfspr/mtspr, and the mfNAME/mtNAME llvm-mc
 * knows for it, which must encode alike. (llvm-mc's mfdear, mfesr and
 * mftcr are the PPC405's 981, 980 and 986; the e500's DEAR, ESR and TCR
 * are Book E's 61, 62 and 340, as GNU as has them, so those names are
 * checked by number only.) */
void ppcasm_vocabulary(FILE *f)
{
    static const int rs[][3] = {
        { 3, 4, 5 }, { 31, 0, 17 }, { 12, 29, 1 }, { 0, 31, 30 }, { 9, 2, 13 }
    };
    static const long long s16[] = { 0, 1, -1, 32767, -32768, 1234 };
    static const long long u16[] = { 0, 1, 65535, 32768, 4660 };
    int k, j;
    for (const struct xo_ent *e = xo_tab; e->name; e++)
        for (j = 0; j < 5; j++) {
            static const char *const sfx[] = { "", ".", "o", "o." };
            for (k = 0; k < (e->oe ? 4 : 2); k++) {
                if (e->nops == 3)
                    fprintf(f, "%s%s %d, %d, %d@%s%s\n", e->name, sfx[k],
                            rs[j][0], rs[j][1], rs[j][2],
                            /* llvm-objdump prints subf and subfc (not
                             * their o forms) as sub and subc */
                            (e->swap || k < 2) && e->xo == PPC_X_SUBF ? "sub"
                            : (e->swap || k < 2) && e->xo == PPC_X_SUBFC
                            ? "subc" : e->name, sfx[k]);
                else
                    fprintf(f, "%s%s %d, %d@%s%s\n", e->name, sfx[k],
                            rs[j][0], rs[j][1], e->name, sfx[k]);
            }
        }
    for (k = 0; xlog_tab[k].name; k++)
        for (j = 0; j < 5; j++) {
            if (xlog_tab[k].nops == 3) {
                fprintf(f, "%s %d, %d, %d@%s\n", xlog_tab[k].name, rs[j][0],
                        rs[j][1], rs[j][2], xlog_tab[k].name);
                fprintf(f, "%s. %%r%d, %%r%d, %%r%d@%s.\n", xlog_tab[k].name,
                        rs[j][2], rs[j][0], rs[j][1], xlog_tab[k].name);
            } else {
                fprintf(f, "%s %d, %d@%s\n", xlog_tab[k].name, rs[j][0],
                        rs[j][1], xlog_tab[k].name);
                fprintf(f, "%s. %d, %d@%s.\n", xlog_tab[k].name, rs[j][1],
                        rs[j][2], xlog_tab[k].name);
            }
        }
    fprintf(f, "mr 3, 4@mr\nmr. 31, 0@mr.\nnot 5, 6@not\nnot. 7, 8@not.\n"
               "srawi 3, 4, 0@srawi\nsrawi 31, 0, 31@srawi\n"
               "srawi. 12, 13, 17@srawi.\n");
    for (const struct d_ent *e = d_tab; e->name; e++)
        for (j = 0; j < 5; j++) {
            long long v = e->sign ? s16[j] : u16[j];
            int rt = rs[j][0], ra = rs[j][1];
            const char *shown = e->name;
            if (e->op == PPC_OP_ADDI && ra == 0)
                shown = "li";
            else if (e->op == PPC_OP_ADDIS && ra == 0)
                shown = "lis";
            else if (e->op == PPC_OP_ORI && rt == 0 && ra == 0 && v == 0)
                shown = "nop";
            if (e->op == PPC_OP_ADDI && ra != 0 && v < 0)
                shown = "addi";
            fprintf(f, "%s %d, %d, %lld@%s\n", e->name, rt, ra, v, shown);
        }
    fprintf(f, "li 3, -32768@li\nli 31, 32767@li\nlis 4, -1@lis\n"
               "lis 5, 0x7fff@lis\nlis 6, 0x12345678@h@lis\n"
               "lis 7, 0x12348765@ha@lis\nli 8, 0x12348765@l@li\n"
               "addi 9, 10, 0x1234fedc@l@addi\nori 3, 3, 0xffff@l@ori\n"
               "la 3, 8(4)@addi\nla 5, -4(1)@addi\n"
               "subi 3, 4, 5@addi\nsubis 3, 4, 5@addis\n"
               "subic 3, 4, 5@addic\nsubic. 3, 4, 5@addic.\nnop@nop\n");
    /* compares */
    fprintf(f, "cmpw 3, 4@cmpw\ncmpw cr7, 31, 0@cmpw\ncmpw 5, 6, 7@cmpw\n"
               "cmplw 3, 4@cmplw\ncmplw cr1, 30, 29@cmplw\n"
               "cmpwi 3, -32768@cmpwi\ncmpwi cr6, 31, 32767@cmpwi\n"
               "cmplwi 3, 65535@cmplwi\ncmplwi cr2, 4, 0@cmplwi\n"
               "cmp 3, 0, 4, 5@cmpw\ncmpl 7, 0, 8, 9@cmplw\n"
               "cmpi 4, 0, 10, -5@cmpwi\ncmpli 5, 0, 11, 6@cmplwi\n");
    /* loads and stores */
    for (const struct m_ent *e = dmem_tab; e->name; e++)
        for (j = 0; j < 5; j++) {
            static const char *const d[] = { "0", "-32768", "32767", "8",
                                             "-4" };
            int rt = rs[j][0], ra = rs[j][1];
            if (e->upd && (ra == 0 || (e->load && ra == rt)))
                ra = rt == 31 ? 30 : rt + 1;
            if (e->op == PPC_OP_LMW && ra >= rt)
                ra = rt > 1 ? rt - 1 : 0;
            fprintf(f, "%s %d, %s(%d)@%s\n", e->name, rt, d[j], ra, e->name);
        }
    fprintf(f, "lwz %%r3, 4(%%r1)@lwz\nstw %%r31, -8(%%r1)@stw\n"
               "lwz 3, 0x12345678@l(4)@lwz\n");
    for (const struct x_ent *e = xmem_tab; e->name; e++)
        for (j = 0; j < 5; j++) {
            int rt = rs[j][0], ra = rs[j][1], rb = rs[j][2];
            if (e->upd && (ra == 0 || (e->load && ra == rt)))
                ra = rt == 31 ? 30 : rt + 1;
            fprintf(f, "%s %d, %d, %d@%s\n", e->name, rt, ra, rb, e->name);
        }
    for (k = 0; xab_tab[k].name; k++)
        for (j = 0; j < 3; j++)
            fprintf(f, "%s %d, %d@%s\n", xab_tab[k].name, rs[j][1], rs[j][2],
                    xab_tab[k].name);
    for (k = 0; x0_tab[k].name; k++)
        fprintf(f, "%s@%s\n", x0_tab[k].name,
                !strcmp(x0_tab[k].name, "msync") ? "sync" : x0_tab[k].name);
    fprintf(f, "mbar@eieio\nmbar 1@mbar 1\nsc@sc\n");
    /* traps */
    fprintf(f, "trap@trap\ntw 4, 3, 4@tweq\ntw 31, 0, 0@trap\n"
               "tw 7, 5, 6@tw\ntwi 31, 0, 0@twui\ntwi 4, 3, -1@tweqi\n"
               "twi 9, 4, 100@twi\n");
    for (k = 0; tcond_tab[k].name; k++) {
        /* llvm-objdump names lt, gt, eq, ne, llt, lgt and u, and prints
         * the others as tw/twi with the TO number */
        int to = tcond_tab[k].to, named = to == 16 || to == 8 || to == 4 ||
                 to == 24 || to == 2 || to == 1 || to == 31;
        const char *c = tcond_tab[k].name;
        if (named)
            fprintf(f, "tw%s %d, %d@tw%s\n", c, rs[k % 5][0], rs[k % 5][1],
                    c);
        else
            fprintf(f, "tw%s %d, %d@tw %d, %d, %d\n", c, rs[k % 5][0],
                    rs[k % 5][1], to, rs[k % 5][0], rs[k % 5][1]);
        if (named)
            fprintf(f, "tw%si %d, %d@tw%si\n", c, rs[k % 5][1],
                    (int)s16[k % 6], c);
        else
            fprintf(f, "tw%si %d, %d@twi %d, %d, %d\n", c, rs[k % 5][1],
                    (int)s16[k % 6], to, rs[k % 5][1], (int)s16[k % 6]);
    }
    /* the condition register */
    for (k = 0; crl_tab[k].name; k++)
        fprintf(f, "%s %d, %d, %d@%s\n%s 4*cr7+so, 4*cr1+lt, 4*cr3+eq@%s\n",
                crl_tab[k].name, k * 3, 31 - k, k + 9, crl_tab[k].name,
                crl_tab[k].name, crl_tab[k].name);
    fprintf(f, "crset 6@crset\ncrclr 31@crclr\ncrmove 1, 2@crmove\n"
               "crnot 30, 4@crnot\nmcrf 1, 7@mcrf\nmcrf cr7, cr0@mcrf\n"
               "mfcr 3@mfcr\nmtcrf 0xff, 3@mtcr\nmtcrf 0x81, 30@mtcrf\n"
               "mtcrf 1, 4@mtcrf\nmtcr 5@mtcr\n"
               "isel 3, 4, 5, 6@isel\nisel 31, 0, 30, 31@isel\n"
               "isellt 3, 4, 5@isellt\niselgt 6, 7, 8@iselgt\n"
               "iseleq 9, 10, 11@iseleq\n");
    /* the machine state and the SPRs */
    fprintf(f, "mfmsr 3@mfmsr\nmtmsr 31@mtmsr\nwrtee 4@wrtee\n"
               "wrteei 0@wrteei\nwrteei 1@wrteei\n"
               "mfspr 3, 8@mflr\nmtspr 9, 4@mtctr\nmfspr 5, 1@mfxer\n"
               "mfspr 6, 272@mfspr\nmtspr 273, 7@mtspr\n"
               "mfspr 8, 1023@mfspr\nmtspr 1008, 31@mtspr\n"
               "mfspr 0, 26@mfsrr0\nmtspr 27, 12@mtsrr1\n"
               "mflr 3@mflr\nmtlr 4@mtlr\nmfctr 5@mfctr\nmtctr 6@mtctr\n"
               "mfxer 7@mfxer\nmtxer 8@mtxer\nmfsrr0 9@mfsrr0\n"
               "mtsrr0 10@mtsrr0\nmfsrr1 11@mfsrr1\nmtsrr1 12@mtsrr1\n"
               "mfdec 13@mfdec\nmtdec 14@mtdec\nmfpvr 15@mfpvr\n"
               "mfspr 16, 61@mfspr\nmtspr 62, 17@mtspr\n"
               "mfspr 20, 340@mfspr\nmtspr 336, 21@mtspr\n"
               "mfpid 22@mfspr\nmfsprg0 23@mfspr\nmfsprg1 24@mfspr\n"
               "mfsprg2 25@mfspr\nmfsprg3 26@mfspr\n"
               "mfsprg 3, 2@mfspr\nmtsprg 3, 4@mtspr\n"
               "mftb 3@mfspr\nmftbu 4@mfspr\n");
    /* the branches */
    fprintf(f, "b .+16@b\nb .-33554432@b\nb .+33554428@b\nbl .+8@bl\n"
               "bl .@bl\nba 0x100@ba\nbla 0x1fffffc@bla\nba 0x1000000@ba\n"
               "blr@blr\nblrl@blrl\nbctr@bctr\nbctrl@bctrl\n"
               "bc 12, 2, .+8@bt\nbc 4, 30, .-32768@bf\n"
               "bc 16, 0, .+32764@bdnz\nbcl 20, 31, .+4@bcl\n"
               "bca 12, 2, 0x100@bta\nbcla 4, 6, 0x200@bfla\n"
               "bc 12, 4*cr7+eq, .+8@bt\n"
               "bclr 12, 2@bclr\nbclrl 4, 6@bclrl\nbcctr 4, 6@bcctr\n"
               "bcctrl 12, 0, 0@bcctrl\nbclr 20, 0@blr\nbcctr 20, 0@bctr\n"
               "bdnz .-8@bdnz\nbdz .+8@bdz\nbdnzl .+8@bdnzl\n"
               "bdzla 0x40@bdzla\nbdnzt 2, .+8@bdnzt\nbdnzf eq, .+8@bdnzf\n"
               "bdzt 4*cr1+gt, .+8@bdzt\nbdzf 31, .+8@bdzf\n"
               "bdnzlr@bdnzlr\nbdzlr@bdzlr\nbdnzlrl@bdnzlrl\n"
               "bt 30, .+8@bt\nbf 4*cr2+so, .+8@bf\nbtl 1, .+8@btl\n"
               "bfa 3, 0x80@bfa 3\nbtlr 2@bclr 12, 2\nbfctr 6@bcctr 4, 6\n"
               "btctrl 9@bcctrl 12, 9\nbflrl 11@bclrl 4, 11\n");
    for (k = 0; bcond_tab[k].name; k++) {
        /* llvm-objdump prints a conditional branch as bt/bf with its CR
         * bit, and bclr/bcctr with BO and the bit: the expectation names
         * the bit, so a condition or a field put wrong shows */
        const char *c = bcond_tab[k].name;
        const char *tf = bcond_tab[k].bo == 12 ? "bt" : "bf";
        int bo = bcond_tab[k].bo, bit = bcond_tab[k].bit;
        fprintf(f, "b%s .+8@%s %d\n", c, tf, bit);
        fprintf(f, "b%s cr%d, .-16@%s %d\n", c, k % 8, tf, bit + 4 * (k % 8));
        fprintf(f, "b%s %d, .+32764@%s %d\n", c, (k + 3) % 8, tf,
                bit + 4 * ((k + 3) % 8));
        fprintf(f, "b%sl .+8@%sl %d\n", c, tf, bit);
        fprintf(f, "b%sa 0x100@%sa %d\n", c, tf, bit);
        fprintf(f, "b%sla cr%d, 0x100@%sla %d\n", c, (k + 5) % 8, tf,
                bit + 4 * ((k + 5) % 8));
        fprintf(f, "b%slr@bclr %d, %d\n", c, bo, bit);
        fprintf(f, "b%slr cr%d@bclr %d, %d\n", c, (k + 1) % 8, bo,
                bit + 4 * ((k + 1) % 8));
        fprintf(f, "b%slrl@bclrl %d, %d\n", c, bo, bit);
        fprintf(f, "b%sctr@bcctr %d, %d\n", c, bo, bit);
        fprintf(f, "b%sctrl cr%d@bcctrl %d, %d\n", c, (k + 2) % 8, bo,
                bit + 4 * ((k + 2) % 8));
    }
    /* the M-forms */
    fprintf(f, "rlwinm 3, 4, 5, 6, 7@rlwinm\nrlwinm. 31, 0, 31, 0, 31@rotlwi.\n"
               "rlwimi 3, 4, 0, 16, 31@rlwimi\nrlwimi. 17, 18, 31, 1, 30@rlwimi.\n"
               "rlwnm 3, 4, 5, 0, 31@rotlw\nrlwnm. 6, 7, 8, 9, 10@rlwnm.\n"
               "rotlwi 3, 4, 5@rotlwi\nrotrwi 3, 4, 5@rotlwi\n"
               "rotlw 3, 4, 5@rotlw\nslwi 3, 4, 5@slwi\nslwi. 3, 4, 31@rlwinm. 3, 4, 31, 0, 0\n"
               "srwi 3, 4, 5@srwi\nsrwi 6, 7, 31@srwi\nclrlwi 3, 4, 16@clrlwi\n"
               "clrrwi 3, 4, 2@rlwinm 3, 4, 0, 0, 29\n"
               "extlwi 3, 4, 8, 0@rlwinm 3, 4, 0, 0, 7\n"
               "extrwi 3, 4, 8, 16@rlwinm 3, 4, 24, 24, 31\n"
               "inslwi 3, 4, 8, 8@rlwimi 3, 4, 24, 8, 15\n"
               "insrwi 3, 4, 8, 16@rlwimi 3, 4, 8, 16, 23\n"
               "clrlslwi 3, 4, 8, 2@rlwinm 3, 4, 2, 6, 29\n");
    /* the SPR names, against llvm-mc's mfNAME/mtNAME */
    fprintf(f, "=mfspr 3, lr=mflr 3\n=mtspr ctr, 4=mtctr 4\n"
               "=mfspr 5, xer=mfxer 5\n=mfspr 6, srr0=mfsrr0 6\n"
               "=mtspr srr1, 7=mtsrr1 7\n=mfspr 8, dec=mfdec 8\n"
               "=mfspr 9, pvr=mfpvr 9\n"
               "=mfspr 13, sprg0=mfsprg0 13\n=mtspr sprg1, 14=mtspr 273, 14\n"
               "=mfspr 15, pid=mfpid 15\n=mfspr 16, tbl=mftb 16\n"
               "=mfspr 17, tbu=mftbu 17\n=mtspr sprg3, 18=mtsprg 3, 18\n");
}
