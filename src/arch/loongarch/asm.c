/* The LoongArch64 assembler. See asm.h. The only thing this file adds to
 * emit.c is a parser: every range is checked HERE, before the encoder is
 * called, so a template's mistake is a diagnostic and not an internal
 * error. */
#include "asm.h"

#include "emit.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../elf/elf.h"

/* ---- registers ---------------------------------------------------------- */

int laasm_gpr(const char *name, int len)
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
    if (buf[0] == 'r' && isdigit((unsigned char)buf[1])) {
        int v = 0;
        for (int i = 1; i < len; i++) {
            if (!isdigit((unsigned char)buf[i]))
                return -1;
            v = v * 10 + (buf[i] - '0');
        }
        return v < 32 && (len == 2 || buf[1] != '0') ? v : -1;
    }
    for (int r = 0; r < 32; r++)
        if (r != 21 && strcmp(buf, la_reg_name(r)) == 0)
            return r;
    if (strcmp(buf, "s9") == 0)
        return LA_FP;                       /* fp's other name */
    if (strcmp(buf, "v0") == 0 || strcmp(buf, "v1") == 0)
        return buf[1] == '0' ? LA_A0 : LA_A1;   /* the old result names */
    return -1;
}

/* ---- a tiny tokeniser --------------------------------------------------- */

#define MAXTOK 8

struct tok { const char *s; int len; };

/* The mnemonic, up to the first blank, then the operands, separated by
 * the commas outside parentheses -- so an operand may be an expression
 * with blanks in it, `(4 * 8) - 1`. Each token is trimmed. */
static int split(const char *stmt, int len, struct tok *t, int max)
{
    int n = 0, i = 0;
    while (i < len && isspace((unsigned char)stmt[i]))
        i++;
    if (i >= len)
        return 0;
    t[0].s = stmt + i;
    while (i < len && !isspace((unsigned char)stmt[i]) && stmt[i] != ',')
        i++;
    t[0].len = (int)(stmt + i - t[0].s);
    n = 1;
    while (i < len && n < max) {
        int depth = 0, b, e;
        while (i < len && isspace((unsigned char)stmt[i]))
            i++;
        if (i < len && stmt[i] == ',' && n > 1)
            i++;
        while (i < len && isspace((unsigned char)stmt[i]))
            i++;
        if (i >= len)
            break;
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
        if (t[n].len > 0)
            n++;
    }
    return n;
}

static int tok_is(const struct tok *t, const char *s)
{
    return (int)strlen(s) == t->len && strncmp(t->s, s, (size_t)t->len) == 0;
}

/* In an instruction a register is always `$`-spelt, as GNU as and llvm-mc
 * require for LoongArch: a bare word is a symbol. */
static int tok_reg(const struct tok *t)
{
    return t->len > 1 && t->s[0] == '$' ? laasm_gpr(t->s, t->len) : -1;
}

/* A constant expression -- what a .S file's macros leave in an operand,
 * `(16 + 4 * (3))` or `-(16 + 4 * 24)` -- over numbers only: + - * / %
 * << >> & | ^ ~, unary minus and parentheses, at C's precedence. */
struct xp { const char *p, *e; int bad; };

static long long xp_or(struct xp *x);

static void xp_ws(struct xp *x)
{
    while (x->p < x->e && (*x->p == ' ' || *x->p == '\t'))
        x->p++;
}

static long long xp_prim(struct xp *x)
{
    long long v = 0;
    int base = 10, any = 0;
    xp_ws(x);
    if (x->p >= x->e) { x->bad = 1; return 0; }
    if (*x->p == '(') {
        x->p++;
        v = xp_or(x);
        xp_ws(x);
        if (x->p >= x->e || *x->p != ')') { x->bad = 1; return 0; }
        x->p++;
        return v;
    }
    if (*x->p == '-') { x->p++; return -xp_prim(x); }
    if (*x->p == '+') { x->p++; return xp_prim(x); }
    if (*x->p == '~') { x->p++; return ~xp_prim(x); }
    if (x->e - x->p > 1 && x->p[0] == '0' && (x->p[1] == 'x' || x->p[1] == 'X')) {
        base = 16;
        x->p += 2;
    }
    while (x->p < x->e) {
        int d;
        if (isdigit((unsigned char)*x->p)) d = *x->p - '0';
        else if (base == 16 && isxdigit((unsigned char)*x->p))
            d = tolower((unsigned char)*x->p) - 'a' + 10;
        else break;
        if ((unsigned long long)v > 0x0fffffffffffffffULL) { x->bad = 1; return 0; }
        v = (long long)((unsigned long long)v * (unsigned long long)base +
                        (unsigned long long)d);
        x->p++;
        any = 1;
    }
    if (!any) x->bad = 1;
    return v;
}

static long long xp_mul(struct xp *x)
{
    long long v = xp_prim(x);
    for (;;) {
        char op;
        long long r;
        xp_ws(x);
        if (x->p >= x->e || (*x->p != '*' && *x->p != '/' && *x->p != '%'))
            return v;
        op = *x->p++;
        r = xp_prim(x);
        if (op != '*' && r == 0) { x->bad = 1; return 0; }
        v = op == '*' ? (long long)((unsigned long long)v * (unsigned long long)r)
          : op == '/' ? v / r : v % r;
    }
}

static long long xp_add(struct xp *x)
{
    long long v = xp_mul(x);
    for (;;) {
        char op;
        long long r;
        xp_ws(x);
        if (x->p >= x->e || (*x->p != '+' && *x->p != '-'))
            return v;
        op = *x->p++;
        r = xp_mul(x);
        v = (long long)(op == '+' ? (unsigned long long)v + (unsigned long long)r
                                  : (unsigned long long)v - (unsigned long long)r);
    }
}

static long long xp_shift(struct xp *x)
{
    long long v = xp_add(x);
    for (;;) {
        int left;
        long long r;
        xp_ws(x);
        if (x->e - x->p < 2 || !((x->p[0] == '<' && x->p[1] == '<') ||
                                 (x->p[0] == '>' && x->p[1] == '>')))
            return v;
        left = x->p[0] == '<';
        x->p += 2;
        r = xp_add(x);
        if (r < 0 || r > 63) { x->bad = 1; return 0; }
        v = left ? (long long)((unsigned long long)v << r) : v >> r;
    }
}

static long long xp_and(struct xp *x)
{
    long long v = xp_shift(x);
    for (;;) {
        xp_ws(x);
        if (x->p >= x->e || *x->p != '&') return v;
        x->p++;
        v &= xp_shift(x);
    }
}

static long long xp_xor(struct xp *x)
{
    long long v = xp_and(x);
    for (;;) {
        xp_ws(x);
        if (x->p >= x->e || *x->p != '^') return v;
        x->p++;
        v ^= xp_and(x);
    }
}

static long long xp_or(struct xp *x)
{
    long long v = xp_xor(x);
    for (;;) {
        xp_ws(x);
        if (x->p >= x->e || *x->p != '|') return v;
        x->p++;
        v |= xp_xor(x);
    }
}

/* A signed integer: decimal or 0x, with an optional sign, or a constant
 * expression of them. */
static int tok_imm(const struct tok *t, long long *out)
{
    struct xp x;
    long long v;
    x.p = t->s; x.e = t->s + t->len; x.bad = 0;
    v = xp_or(&x);
    xp_ws(&x);
    if (x.bad || x.p != x.e)
        return 0;
    *out = v;
    return 1;
}

/* A transfer's target: a byte offset from the branch itself, as a bare
 * number or as `.+N` / `.-N`, which the file assembler writes for a label
 * (LoongArch has no delay slot, so the two are the same). */
static int tok_disp(const struct tok *t, long long *out)
{
    if (t->len >= 2 && t->s[0] == '.' && (t->s[1] == '+' || t->s[1] == '-')) {
        struct tok n;
        n.s = t->s + 1;
        n.len = t->len - 1;
        return tok_imm(&n, out);
    }
    return tok_imm(t, out);
}

/* ---- the instruction tables --------------------------------------------- */

struct alu_ent { const char *name; int op; int w; };
static const struct alu_ent alu_tab[] = {
    { "add.w", LA_ADD, 1 }, { "add.d", LA_ADD, 0 },
    { "sub.w", LA_SUB, 1 }, { "sub.d", LA_SUB, 0 },
    { "slt", LA_SLT, 0 }, { "sltu", LA_SLTU, 0 },
    { "and", LA_AND, 0 }, { "or", LA_OR, 0 }, { "xor", LA_XOR, 0 },
    { "nor", LA_NOR, 0 }, { "andn", LA_ANDN, 0 }, { "orn", LA_ORN, 0 },
    { "sll.w", LA_SLL, 1 }, { "sll.d", LA_SLL, 0 },
    { "srl.w", LA_SRL, 1 }, { "srl.d", LA_SRL, 0 },
    { "sra.w", LA_SRA, 1 }, { "sra.d", LA_SRA, 0 },
    { "rotr.w", LA_ROTR, 1 }, { "rotr.d", LA_ROTR, 0 },
    { "maskeqz", LA_MASKEQZ, 0 }, { "masknez", LA_MASKNEZ, 0 },
    { "mul.w", LA_MUL, 1 }, { "mul.d", LA_MUL, 0 },
    { "mulh.w", LA_MULH, 1 }, { "mulh.d", LA_MULH, 0 },
    { "mulh.wu", LA_MULHU, 1 }, { "mulh.du", LA_MULHU, 0 },
    { "div.w", LA_DIV, 1 }, { "div.d", LA_DIV, 0 },
    { "div.wu", LA_DIVU, 1 }, { "div.du", LA_DIVU, 0 },
    { "mod.w", LA_MOD, 1 }, { "mod.d", LA_MOD, 0 },
    { "mod.wu", LA_MODU, 1 }, { "mod.du", LA_MODU, 0 },
    { NULL, 0, 0 }
};

/* sign: the 12-bit field is signed (addi, slti, sltui) or unsigned
 * (andi, ori, xori) */
struct imm_ent { const char *name; int op; int w; int sign; };
static const struct imm_ent imm_tab[] = {
    { "addi.w", LA_ADD, 1, 1 }, { "addi.d", LA_ADD, 0, 1 },
    { "slti", LA_SLT, 0, 1 }, { "sltui", LA_SLTU, 0, 1 },
    { "andi", LA_AND, 0, 0 }, { "ori", LA_OR, 0, 0 }, { "xori", LA_XOR, 0, 0 },
    { NULL, 0, 0, 0 }
};

struct sh_ent { const char *name; int op; int w; };
static const struct sh_ent sh_tab[] = {
    { "slli.w", LA_SLL, 1 }, { "slli.d", LA_SLL, 0 },
    { "srli.w", LA_SRL, 1 }, { "srli.d", LA_SRL, 0 },
    { "srai.w", LA_SRA, 1 }, { "srai.d", LA_SRA, 0 },
    { "rotri.w", LA_ROTR, 1 }, { "rotri.d", LA_ROTR, 0 },
    { NULL, 0, 0 }
};

struct ls_ent { const char *name; int size; int sign; int store; };
static const struct ls_ent ls_tab[] = {
    { "ld.b", 1, 1, 0 }, { "ld.bu", 1, 0, 0 }, { "ld.h", 2, 1, 0 },
    { "ld.hu", 2, 0, 0 }, { "ld.w", 4, 1, 0 }, { "ld.wu", 4, 0, 0 },
    { "ld.d", 8, 1, 0 },
    { "st.b", 1, 0, 1 }, { "st.h", 2, 0, 1 }, { "st.w", 4, 0, 1 },
    { "st.d", 8, 0, 1 },
    { NULL, 0, 0, 0 }
};

/* nregs: 2 compares rj and rd; 1 tests rj against zero; 0 none.
 * swap: the pseudo writes its two registers the other way round (bgt is
 * blt with them exchanged); zfirst: against zero, the zero is rj (bgtz is
 * blt $zero, rj). */
struct br_ent { const char *name; int cond; int nregs; int swap; int zfirst; };
static const struct br_ent br_tab[] = {
    { "beq", LA_BEQ, 2, 0, 0 }, { "bne", LA_BNE, 2, 0, 0 },
    { "blt", LA_BLT, 2, 0, 0 }, { "bge", LA_BGE, 2, 0, 0 },
    { "bltu", LA_BLTU, 2, 0, 0 }, { "bgeu", LA_BGEU, 2, 0, 0 },
    { "bgt", LA_BLT, 2, 1, 0 }, { "ble", LA_BGE, 2, 1, 0 },
    { "bgtu", LA_BLTU, 2, 1, 0 }, { "bleu", LA_BGEU, 2, 1, 0 },
    { "beqz", LA_BEQZ, 1, 0, 0 }, { "bnez", LA_BNEZ, 1, 0, 0 },
    { "bltz", LA_BLT, 1, 0, 0 }, { "bgez", LA_BGE, 1, 0, 0 },
    { "bgtz", LA_BLT, 1, 0, 1 }, { "blez", LA_BGE, 1, 0, 1 },
    { NULL, 0, 0, 0, 0 }
};

static const struct { const char *name; int op; } pc_tab[] = {
    { "pcaddi", LA_PCADDI }, { "pcalau12i", LA_PCALAU12I },
    { "pcaddu12i", LA_PCADDU12I }, { "pcaddu18i", LA_PCADDU18I },
    { NULL, 0 }
};

static const struct { const char *name; int op; } am_tab[] = {
    { "amswap_db", LA_AMSWAP }, { "amadd_db", LA_AMADD },
    { "amand_db", LA_AMAND }, { "amor_db", LA_AMOR },
    { "amxor_db", LA_AMXOR }, { NULL, 0 }
};

#define FAIL(...)  do { snprintf(err, (size_t)errlen, __VA_ARGS__); \
                        return -1; } while (0)

static int need_regs(const struct tok *t, int n, int *r, const char *what,
                     char *err, int errlen)
{
    for (int k = 0; k < n; k++) {
        r[k] = tok_reg(&t[1 + k]);
        if (r[k] < 0)
            FAIL("%s: \"%.*s\" is not a register (they are written $a0 or "
                 "$r4)", what, t[1 + k].len, t[1 + k].s);
    }
    return 0;
}

static int fits_s(long long v, int bits)
{
    return v >= -(1LL << (bits - 1)) && v < (1LL << (bits - 1));
}

static int fits_u(long long v, int bits)
{
    return v >= 0 && v < (1LL << bits);
}

static int stmt_body(const char *stmt, int len, struct code *out,
                     char *err, int errlen)
{
    struct tok t[MAXTOK];
    int n = split(stmt, len, t, MAXTOK);
    int r[4];
    long long v, w;

    if (n == 0)
        return 0;
    /* a refusal laasm_symform wrote for a form it recognised */
    if (stmt[0] == '\001')
        FAIL("%.*s", len - 1, stmt + 1);
    /* an operator that reached here was not laasm_symform's: a number's
     * %pc_hi20 means nothing, and the TLS and extreme-model ones are not
     * assembled */
    for (int k = 1; k < n; k++)
        if (t[k].len > 1 && t[k].s[0] == '%')
            FAIL("\"%.*s\": a relocation operator takes a symbol here, and "
                 "only %%pc_hi20/%%pc_lo12, %%got_pc_hi20/%%got_pc_lo12, "
                 "%%abs_hi20/%%abs_lo12/%%abs64_lo20/%%abs64_hi12 and %%call36 "
                 "are assembled", t[k].len, t[k].s);

    if (n == 1) {
        if (tok_is(&t[0], "nop")) { la_nop(out); return 0; }
        if (tok_is(&t[0], "ret")) { la_ret(out); return 0; }
    }
    if (tok_is(&t[0], "move")) {
        if (n != 3 || need_regs(t, 2, r, "move", err, errlen))
            FAIL("move takes two registers");
        la_mv(out, r[0], r[1]);
        return 0;
    }
    if (tok_is(&t[0], "jr")) {
        if (n != 2 || need_regs(t, 1, r, "jr", err, errlen))
            FAIL("jr takes a register");
        la_jirl(out, LA_ZERO, r[0], 0);
        return 0;
    }
    if (tok_is(&t[0], "li.w") || tok_is(&t[0], "li.d")) {
        int d = t[0].s[3] == 'd';
        if (n != 3 || need_regs(t, 1, r, "li", err, errlen) ||
            !tok_imm(&t[2], &v))
            FAIL("%.*s takes a register and a constant", t[0].len, t[0].s);
        if (!d) {
            if (v < -2147483648LL || v > 4294967295LL)
                FAIL("li.w constant %lld does not fit 32 bits", v);
            v = (long long)(int)(unsigned int)(unsigned long long)v;
        }
        la_li(out, r[0], v);
        return 0;
    }

    /* ---- three registers ---- */
    for (const struct alu_ent *e = alu_tab; e->name; e++) {
        if (!tok_is(&t[0], e->name))
            continue;
        if (n != 4) FAIL("%s takes three registers", e->name);
        if (need_regs(t, 3, r, e->name, err, errlen)) return -1;
        la_alu(out, e->op, r[0], r[1], r[2], e->w);
        return 0;
    }
    /* ---- register, register, 12-bit immediate ---- */
    for (const struct imm_ent *e = imm_tab; e->name; e++) {
        if (!tok_is(&t[0], e->name))
            continue;
        if (n != 4) FAIL("%s takes two registers and an immediate", e->name);
        if (need_regs(t, 2, r, e->name, err, errlen)) return -1;
        if (!tok_imm(&t[3], &v))
            FAIL("%s: \"%.*s\" is not a constant", e->name, t[3].len, t[3].s);
        if (e->sign ? !fits_s(v, 12) : !fits_u(v, 12))
            FAIL("%s immediate %lld does not fit its %s 12-bit field",
                 e->name, v, e->sign ? "signed" : "unsigned (0..4095)");
        la_alu_imm(out, e->op, r[0], r[1], v, e->w);
        return 0;
    }
    if (tok_is(&t[0], "lu52i.d")) {
        if (n != 4 || need_regs(t, 2, r, "lu52i.d", err, errlen) ||
            !tok_imm(&t[3], &v))
            FAIL("lu52i.d takes two registers and an immediate");
        if (!fits_s(v, 12))
            FAIL("lu52i.d immediate %lld does not fit its signed 12-bit field",
                 v);
        la_lu52i(out, r[0], r[1], (long)v);
        return 0;
    }
    for (const struct sh_ent *e = sh_tab; e->name; e++) {
        if (!tok_is(&t[0], e->name))
            continue;
        if (n != 4 || need_regs(t, 2, r, e->name, err, errlen) ||
            !tok_imm(&t[3], &v))
            FAIL("%s takes two registers and an amount", e->name);
        if (v < 0 || v > (e->w ? 31 : 63))
            FAIL("%s amount %lld is not 0..%d", e->name, v, e->w ? 31 : 63);
        la_shift_imm(out, e->op, r[0], r[1], (int)v, e->w);
        return 0;
    }
    /* ---- two registers ---- */
    if (tok_is(&t[0], "ext.w.b") || tok_is(&t[0], "ext.w.h")) {
        if (n != 3 || need_regs(t, 2, r, "ext.w", err, errlen))
            FAIL("%.*s takes two registers", t[0].len, t[0].s);
        la_ext(out, r[0], r[1], t[0].s[6] == 'b' ? 1 : 2);
        return 0;
    }
    {
        static const char *const rv[] = { "revb.2h", "revb.4h", "revb.2w",
                                          "revb.d", NULL };
        for (int k = 0; rv[k]; k++) {
            if (!tok_is(&t[0], rv[k]))
                continue;
            if (n != 3 || need_regs(t, 2, r, rv[k], err, errlen))
                FAIL("%s takes two registers", rv[k]);
            la_revb(out, LA_REVB_2H + k, r[0], r[1]);
            return 0;
        }
    }
    if (tok_is(&t[0], "bstrpick.w") || tok_is(&t[0], "bstrpick.d")) {
        int d = t[0].s[9] == 'd', top = d ? 63 : 31;
        if (n != 5 || need_regs(t, 2, r, "bstrpick", err, errlen) ||
            !tok_imm(&t[3], &v) || !tok_imm(&t[4], &w))
            FAIL("%.*s takes two registers, msb and lsb", t[0].len, t[0].s);
        if (v < 0 || v > top || w < 0 || w > v)
            FAIL("%.*s field %lld:%lld is not within 0..%d with msb >= lsb",
                 t[0].len, t[0].s, v, w, top);
        la_bstrpick(out, r[0], r[1], (int)v, (int)w, d);
        return 0;
    }
    if (tok_is(&t[0], "alsl.w") || tok_is(&t[0], "alsl.d")) {
        if (n != 5 || need_regs(t, 3, r, "alsl", err, errlen) ||
            !tok_imm(&t[4], &v))
            FAIL("%.*s takes three registers and a shift", t[0].len, t[0].s);
        if (v < 1 || v > 4)
            FAIL("%.*s shift %lld is not 1..4", t[0].len, t[0].s, v);
        la_alsl(out, r[0], r[1], r[2], (int)v, t[0].s[5] == 'd');
        return 0;
    }
    /* ---- a register and a 20-bit immediate ---- */
    if (tok_is(&t[0], "lu12i.w") || tok_is(&t[0], "lu32i.d")) {
        if (n != 3 || need_regs(t, 1, r, "lu", err, errlen) ||
            !tok_imm(&t[2], &v))
            FAIL("%.*s takes a register and an immediate", t[0].len, t[0].s);
        if (!fits_s(v, 20))
            FAIL("%.*s immediate %lld does not fit its signed 20-bit field",
                 t[0].len, t[0].s, v);
        if (t[0].s[2] == '1') la_lu12i(out, r[0], (long)v);
        else                  la_lu32i(out, r[0], (long)v);
        return 0;
    }
    for (int k = 0; pc_tab[k].name; k++) {
        if (!tok_is(&t[0], pc_tab[k].name))
            continue;
        if (n != 3 || need_regs(t, 1, r, pc_tab[k].name, err, errlen) ||
            !tok_imm(&t[2], &v))
            FAIL("%s takes a register and an immediate", pc_tab[k].name);
        if (!fits_s(v, 20))
            FAIL("%s immediate %lld does not fit its signed 20-bit field",
                 pc_tab[k].name, v);
        la_pcrel(out, pc_tab[k].op, r[0], (long)v);
        return 0;
    }
    /* ---- loads and stores ---- */
    for (const struct ls_ent *e = ls_tab; e->name; e++) {
        if (!tok_is(&t[0], e->name))
            continue;
        if (n != 4 || need_regs(t, 2, r, e->name, err, errlen) ||
            !tok_imm(&t[3], &v))
            FAIL("%s takes two registers and an offset", e->name);
        if (!fits_s(v, 12))
            FAIL("%s offset %lld does not fit a signed 12-bit field",
                 e->name, v);
        if (e->store) la_store(out, r[0], r[1], (int)v, e->size);
        else          la_load(out, r[0], r[1], (int)v, e->size, e->sign);
        return 0;
    }
    if (tok_is(&t[0], "ll.w") || tok_is(&t[0], "ll.d") ||
        tok_is(&t[0], "sc.w") || tok_is(&t[0], "sc.d")) {
        int d = t[0].s[3] == 'd';
        if (n != 4 || need_regs(t, 2, r, "ll/sc", err, errlen) ||
            !tok_imm(&t[3], &v))
            FAIL("%.*s takes two registers and an offset", t[0].len, t[0].s);
        if ((v & 3) || !fits_s(v >> 2, 14))
            FAIL("%.*s offset %lld is not a multiple of 4 in -32768..32764",
                 t[0].len, t[0].s, v);
        if (t[0].s[0] == 'l') la_ll(out, r[0], r[1], (int)v, d);
        else                  la_sc(out, r[0], r[1], (int)v, d);
        return 0;
    }
    for (int k = 0; am_tab[k].name; k++) {
        size_t l = strlen(am_tab[k].name);
        int d;
        if ((size_t)t[0].len != l + 2 || strncmp(t[0].s, am_tab[k].name, l) ||
            t[0].s[l] != '.' || (t[0].s[l + 1] != 'w' && t[0].s[l + 1] != 'd'))
            continue;
        d = t[0].s[l + 1] == 'd';
        if (n != 4 || need_regs(t, 3, r, am_tab[k].name, err, errlen))
            FAIL("%.*s takes three registers", t[0].len, t[0].s);
        if (r[0] != LA_ZERO && (r[0] == r[1] || r[0] == r[2]))
            FAIL("%.*s: rd may not also be rk or rj (the instruction is "
                 "undefined then)", t[0].len, t[0].s);
        la_am(out, am_tab[k].op, r[0], r[1], r[2], d);
        return 0;
    }
    /* ---- control flow ---- */
    for (const struct br_ent *e = br_tab; e->name; e++) {
        int rj, rd;
        if (!tok_is(&t[0], e->name))
            continue;
        if (n != 2 + e->nregs || need_regs(t, e->nregs, r, e->name, err, errlen) ||
            !tok_disp(&t[1 + e->nregs], &v))
            FAIL("%s takes %s and a target", e->name,
                 e->nregs == 2 ? "two registers" : "a register");
        if (e->nregs == 2) {
            rj = e->swap ? r[1] : r[0];
            rd = e->swap ? r[0] : r[1];
        } else if (e->cond == LA_BEQZ || e->cond == LA_BNEZ) {
            rj = r[0];
            rd = LA_ZERO;
        } else {
            rj = e->zfirst ? LA_ZERO : r[0];
            rd = e->zfirst ? r[0] : LA_ZERO;
        }
        if (!la_branch_reaches(e->cond, (long)v))
            FAIL("%s target %lld is not a multiple of 4 within %s", e->name, v,
                 e->cond == LA_BEQZ || e->cond == LA_BNEZ ? "+-4 MiB"
                                                          : "+-128 KiB");
        la_w(out, la_enc_branch(e->cond, rj, rd, (long)v));
        return 0;
    }
    if (tok_is(&t[0], "b") || tok_is(&t[0], "bl")) {
        if (n != 2 || !tok_disp(&t[1], &v))
            FAIL("%.*s takes a target", t[0].len, t[0].s);
        if ((v & 3) || !fits_s(v >> 2, 26))
            FAIL("%.*s target %lld is not a multiple of 4 within +-128 MiB",
                 t[0].len, t[0].s, v);
        la_w(out, la_enc_j(t[0].len == 2, (long)v));
        return 0;
    }
    if (tok_is(&t[0], "jirl")) {
        if (n != 4 || need_regs(t, 2, r, "jirl", err, errlen) ||
            !tok_imm(&t[3], &v))
            FAIL("jirl takes two registers and an offset");
        if ((v & 3) || !fits_s(v >> 2, 16))
            FAIL("jirl offset %lld is not a multiple of 4 within +-128 KiB", v);
        la_jirl(out, r[0], r[1], (long)v);
        return 0;
    }
    /* ---- barriers and traps ---- */
    if (tok_is(&t[0], "dbar") || tok_is(&t[0], "break")) {
        if (n != 2 || !tok_imm(&t[1], &v))
            FAIL("%.*s takes a constant", t[0].len, t[0].s);
        if (!fits_u(v, 15))
            FAIL("%.*s %lld is not 0..32767", t[0].len, t[0].s, v);
        if (t[0].s[0] == 'd') la_dbar(out, (int)v);
        else                  la_break(out, (int)v);
        return 0;
    }
    /* ---- what only the assembler emits ---- */
    for (const struct la_raw *e = la_raw_insns; e->name; e++) {
        if (!tok_is(&t[0], e->name))
            continue;
        switch (e->fmt) {
        case LAF_2R:
            if (n != 3 || need_regs(t, 2, r, e->name, err, errlen))
                FAIL("%s takes two registers", e->name);
            la_w(out, la_enc_2r(e->op, r[0], r[1]));
            return 0;
        case LAF_3R:
            if (n != 4 || need_regs(t, 3, r, e->name, err, errlen))
                FAIL("%s takes three registers", e->name);
            la_w(out, la_enc_3r(e->op, r[0], r[1], r[2]));
            return 0;
        case LAF_AM:
            if (n != 4 || need_regs(t, 3, r, e->name, err, errlen))
                FAIL("%s takes three registers", e->name);
            if (r[0] != LA_ZERO && (r[0] == r[1] || r[0] == r[2]))
                FAIL("%s: rd may not also be rk or rj (the instruction is "
                     "undefined then)", e->name);
            /* written rd, rk, rj */
            la_w(out, la_enc_3r(e->op, r[0], r[2], r[1]));
            return 0;
        case LAF_PTR:
            if (n != 4 || need_regs(t, 2, r, e->name, err, errlen) ||
                !tok_imm(&t[3], &v))
                FAIL("%s takes two registers and an offset", e->name);
            if ((v & 3) || !fits_s(v >> 2, 14))
                FAIL("%s offset %lld is not a multiple of 4 in "
                     "-32768..32764", e->name, v);
            la_w(out, la_enc_2ri14(e->op, r[0], r[1],
                                   (unsigned)((unsigned long long)v >> 2) &
                                   0x3fffu));
            return 0;
        case LAF_CSRRD: case LAF_CSRWR:
            if (n != 3 || need_regs(t, 1, r, e->name, err, errlen) ||
                !tok_imm(&t[2], &v))
                FAIL("%s takes a register and a CSR number", e->name);
            if (!fits_u(v, 14))
                FAIL("%s CSR %lld is not 0..16383", e->name, v);
            la_w(out, e->op | ((unsigned long)v << 10) | (unsigned long)r[0]);
            return 0;
        case LAF_CSRXCHG:
            if (n != 4 || need_regs(t, 2, r, e->name, err, errlen) ||
                !tok_imm(&t[3], &v))
                FAIL("csrxchg takes two registers and a CSR number");
            if (r[1] < 2)
                FAIL("csrxchg's mask register may not be $r0 or $r1: those "
                     "encodings are csrrd and csrwr");
            if (!fits_u(v, 14))
                FAIL("csrxchg CSR %lld is not 0..16383", v);
            la_w(out, la_enc_2ri14(e->op, r[0], r[1], (unsigned)v));
            return 0;
        case LAF_CODE15:
            if (n != 2 || !tok_imm(&t[1], &v))
                FAIL("%s takes a constant", e->name);
            if (!fits_u(v, 15))
                FAIL("%s %lld is not 0..32767", e->name, v);
            la_w(out, e->op | (unsigned long)v);
            return 0;
        case LAF_NONE:
            if (n != 1)
                FAIL("%s takes no operand", e->name);
            la_w(out, e->op);
            return 0;
        default: {                                       /* LAF_BSTRINS */
            int top = e->d ? 63 : 31;
            if (n != 5 || need_regs(t, 2, r, e->name, err, errlen) ||
                !tok_imm(&t[3], &v) || !tok_imm(&t[4], &w))
                FAIL("%s takes two registers, msb and lsb", e->name);
            if (v < 0 || v > top || w < 0 || w > v)
                FAIL("%s field %lld:%lld is not within 0..%d with msb >= lsb",
                     e->name, v, w, top);
            la_w(out, e->op | ((unsigned long)v << 16) |
                      ((unsigned long)w << 10) |
                      ((unsigned long)r[1] << 5) | (unsigned long)r[0]);
            return 0;
        }
        }
    }
    FAIL("asm instruction \"%.*s\" is not in the LoongArch vocabulary",
         (int)(t[n - 1].s + t[n - 1].len - t[0].s), t[0].s);
}

int laasm_assemble(const char *text, struct code *out, char *err, int errlen)
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
        if (stmt_body(start, len, out, err, errlen) != 0)
            return -1;
        if (*p)
            p++;
    }
    return 0;
}

/* ---- for the file assembler (src/as/gas.c) ---------------------------- */

/* In a .S file a register is always written with its `$`: a bare `ra` or
 * `sp` there is a symbol. A floating-point register is a register as
 * well, not a symbol -- its instructions are then refused, by name, as
 * instructions outside the vocabulary. */
int laasm_is_reg(const char *name, int len)
{
    if (len < 2 || name[0] != '$')
        return -1;
    if (len >= 3 && name[1] == 'f' &&
        (isdigit((unsigned char)name[2]) || name[2] == 'a' ||
         name[2] == 't' || name[2] == 's' || name[2] == 'c'))
        return 32;
    return laasm_gpr(name, len);
}

/* `%pc_hi20`, `%got_pc_lo12`...: the word after a `%` is an operator,
 * not a symbol the file defines. */
int laasm_is_word(const char *stmt, const char *w, int len)
{
    (void)len;
    return w > stmt && w[-1] == '%';
}

/* The symbol at p (an identifier, not a register, not `.` itself), then
 * an optional `+K` / `-K`. Returns the length consumed, 0 when there is
 * no symbol there. */
static int sym_operand(const char *p, int *slen, long *add)
{
    int n = 0;
    if (!(isalpha((unsigned char)p[0]) || p[0] == '_' || p[0] == '.'))
        return 0;
    if (p[0] == '.' && (p[1] == '+' || p[1] == '-' || !p[1] || p[1] == ')' ||
                        p[1] == ' '))
        return 0;                         /* `.` itself */
    while (isalnum((unsigned char)p[n]) || p[n] == '_' || p[n] == '.' ||
           p[n] == '$')
        n++;
    *slen = n;
    *add = 0;
    if (p[n] == '+' || p[n] == '-') {
        char *e;
        long k = strtol(p + n + 1, &e, 0);
        if (e == p + n + 1)
            return 0;
        *add = p[n] == '-' ? -k : k;
        n = (int)(e - p);
    }
    return n;
}

static const char *skip_sp(const char *p)
{
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
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

/* The first operand register of a statement, as text (`$a0`), for a
 * pseudo that writes it more than once. */
static int first_reg(const char *q, const char **comma)
{
    const char *c = strchr(q, ',');
    int n = 0;
    if (!c)
        return 0;
    while (q + n < c && q[n] != ' ' && q[n] != '\t')
        n++;
    *comma = c;
    return laasm_gpr(q, n) >= 0 && q[0] == '$' ? n : 0;
}

static int mnem_is(const char *m, int mlen, const char *s)
{
    return (int)strlen(s) == mlen && strncmp(m, s, (size_t)mlen) == 0;
}

/* The statements whose operand is a SYMBOL, each rewritten with a zero
 * operand for laasm_assemble and the relocations it carries:
 *
 *   b sym, bl sym                 R_LARCH_B26
 *   call36 sym, tail36 $rd, sym   pcaddu18i + jirl, R_LARCH_CALL36
 *   pcaddu18i $rd, %call36(sym)   R_LARCH_CALL36
 *   la.pcrel / la.local $rd, sym  pcalau12i + addi.d, the PCALA pair
 *   la / la.global $rd, sym       pcalau12i + ld.d, the GOT pair (as
 *                                 llvm-mc expands them; embld rewrites
 *                                 the pair to la.pcrel's, building no GOT)
 *   la.abs $rd, sym               refused: four relocations, two sites
 *   %pc_hi20(sym), %got_pc_hi20   on pcalau12i
 *   %pc_lo12(sym)                 on addi.d or a load's or store's offset
 *   %got_pc_lo12(sym)             on ld.d
 *   %abs_hi20 / %abs_lo12 / %abs64_lo20 / %abs64_hi12
 *                                 on lu12i.w / ori / lu32i.d / lu52i.d
 *
 * The TLS, extreme-model and absolute-GOT operators are recognised in
 * order to refuse them by name. */
int laasm_symform(const char *stmt, struct asm_symform *f)
{
    const char *p = skip_sp(stmt), *m = p, *q, *comma;
    int mlen = 0, slen, n, rn;
    long add;

    while (isalnum((unsigned char)p[mlen]) || p[mlen] == '.' || p[mlen] == '_')
        mlen++;
    if (!mlen || (p[mlen] != ' ' && p[mlen] != '\t'))
        return 0;
    q = skip_sp(p + mlen);
    memset(f, 0, sizeof *f);

    {
        static const char *const bad[] = {
            "%le_hi20", "%le_lo12", "%le64_", "%ie_pc_", "%ie64_", "%gd_pc_",
            "%ld_pc_", "%gd_hi20", "%ld_hi20", "%ie_hi20", "%ie_lo12",
            "%desc", "%tls_", "%pc64_", "%got64_", "%got_hi20", "%got_lo12",
            "%pcrel_20", NULL
        };
        const char *pc = strchr(q, '%');
        for (int k = 0; pc && bad[k]; k++) {
            size_t l = strlen(bad[k]);
            if (!strncmp(pc, bad[k], l)) {
                int wl = 1;
                while (isalnum((unsigned char)pc[wl]) || pc[wl] == '_')
                    wl++;
                return refuse_form(f, stmt, pc + 1, wl - 1,
                                   "this operator is for TLS, the extreme "
                                   "code model or an absolute GOT, which this "
                                   "assembler does not relocate");
            }
        }
    }

    if (mnem_is(m, mlen, "b") || mnem_is(m, mlen, "bl")) {
        n = sym_operand(q, &slen, &add);
        if (!n || *skip_sp(q + n))
            return 0;
        f->sym_at = (int)(q - stmt);
        f->sym_len = slen;
        f->addend = add;
        snprintf(f->encode, sizeof f->encode, "%.*s 0", mlen, m);
        f->site[0].reloc = R_LARCH_B26;
        f->nsites = 1;
        return 1;
    }
    if (mnem_is(m, mlen, "call36")) {
        n = sym_operand(q, &slen, &add);
        if (!n || *skip_sp(q + n))
            return 0;
        f->sym_at = (int)(q - stmt);
        f->sym_len = slen;
        f->addend = add;
        snprintf(f->encode, sizeof f->encode,
                 "pcaddu18i $ra, 0\njirl $ra, $ra, 0");
        f->site[0].reloc = R_LARCH_CALL36;
        f->nsites = 1;
        return 1;
    }
    if (mnem_is(m, mlen, "tail36") || mnem_is(m, mlen, "la") ||
        mnem_is(m, mlen, "la.global") || mnem_is(m, mlen, "la.pcrel") ||
        mnem_is(m, mlen, "la.local") || mnem_is(m, mlen, "la.abs")) {
        const char *o;
        rn = first_reg(q, &comma);
        if (!rn)
            return 0;
        o = skip_sp(comma + 1);
        n = sym_operand(o, &slen, &add);
        if (!n || *skip_sp(o + n))
            return 0;
        f->sym_at = (int)(o - stmt);
        f->sym_len = slen;
        f->addend = add;
        if (mnem_is(m, mlen, "tail36")) {
            snprintf(f->encode, sizeof f->encode,
                     "pcaddu18i %.*s, 0\njirl $zero, %.*s, 0", rn, q, rn, q);
            f->site[0].reloc = R_LARCH_CALL36;
            f->nsites = 1;
        } else if (mnem_is(m, mlen, "la.abs")) {
            /* four instructions and four relocations, where asm_symform
             * holds two sites */
            return refuse_form(f, stmt, o, slen,
                               "la.abs is not assembled: write the "
                               "lu12i.w/ori/lu32i.d/lu52i.d sequence with "
                               "%abs_hi20, %abs_lo12, %abs64_lo20 and "
                               "%abs64_hi12, or la.pcrel");
        } else {
            int got = mnem_is(m, mlen, "la") || mnem_is(m, mlen, "la.global");
            snprintf(f->encode, sizeof f->encode,
                     got ? "pcalau12i %.*s, 0\nld.d %.*s, %.*s, 0"
                         : "pcalau12i %.*s, 0\naddi.d %.*s, %.*s, 0",
                     rn, q, rn, q, rn, q);
            f->site[0].reloc = got ? R_LARCH_GOT_PC_HI20 : R_LARCH_PCALA_HI20;
            f->site[1].off = 4;
            f->site[1].reloc = got ? R_LARCH_GOT_PC_LO12 : R_LARCH_PCALA_LO12;
            f->nsites = 2;
        }
        return 1;
    }

    /* an operator on a symbol: which instruction may carry each */
    {
        static const struct { const char *op; int reloc; const char *insn; }
        ops[] = {
            { "%pc_hi20(", R_LARCH_PCALA_HI20, "pcalau12i" },
            { "%got_pc_hi20(", R_LARCH_GOT_PC_HI20, "pcalau12i" },
            { "%pc_lo12(", R_LARCH_PCALA_LO12, NULL },
            { "%got_pc_lo12(", R_LARCH_GOT_PC_LO12, "ld.d" },
            { "%abs_hi20(", R_LARCH_ABS_HI20, "lu12i.w" },
            { "%abs_lo12(", R_LARCH_ABS_LO12, "ori" },
            { "%abs64_lo20(", R_LARCH_ABS64_LO20, "lu32i.d" },
            { "%abs64_hi12(", R_LARCH_ABS64_HI12, "lu52i.d" },
            { "%call36(", R_LARCH_CALL36, "pcaddu18i" },
            { NULL, 0, NULL }
        };
        const char *at = strchr(q, '%');
        int k;
        if (!at)
            return 0;
        for (k = 0; ops[k].op; k++)
            if (!strncmp(at, ops[k].op, strlen(ops[k].op)))
                break;
        if (!ops[k].op)
            return 0;
        {
            const char *s = at + strlen(ops[k].op);
            n = sym_operand(s, &slen, &add);
            if (!n || s[n] != ')')
                return 0;               /* a number's: refused in encode */
            f->sym_at = (int)(s - stmt);
            f->sym_len = slen;
            f->addend = add;
            if (ops[k].insn && !mnem_is(m, mlen, ops[k].insn)) {
                snprintf(f->encode, sizeof f->encode,
                         "\001%.*s) is %s's operand", (int)(s - at - 1), at,
                         ops[k].insn);
                return 1;
            }
            if (!ops[k].insn) {
                static const char *const lo[] = {
                    "addi.d", "ld.b", "ld.bu", "ld.h", "ld.hu", "ld.w",
                    "ld.wu", "ld.d", "st.b", "st.h", "st.w", "st.d", NULL
                };
                int ok = 0;
                for (int j = 0; lo[j]; j++)
                    ok |= mnem_is(m, mlen, lo[j]);
                if (!ok)
                    return refuse_form(f, stmt, s, slen,
                                       "%pc_lo12(symbol) is an addi.d's or a "
                                       "load's or store's offset: %pc_hi20 "
                                       "is rounded for its sign extension");
            }
            snprintf(f->encode, sizeof f->encode, "%.*s0%s",
                     (int)(at - stmt), stmt, s + n + 1);
            f->site[0].reloc = ops[k].reloc;
            f->nsites = 1;
            return 1;
        }
    }
}

/* ---- the referee's input -------------------------------------------------- */

void laasm_vocabulary(FILE *f)
{
    fprintf(f, "\tnop\n\tret\n\tertn\n\tmove $a0, $t8\n\tmove $s8, $zero\n");
    fprintf(f, "\tjr $ra\n\tjr $t1\n\tli.w $a0, -1\n\tli.w $t0, 0x12345678\n");
    fprintf(f, "\tli.w $s1, 4095\n\tli.w $a1, 2147483647\n");
    fprintf(f, "\tli.d $a0, 0x123456789abcdef\n\tli.d $t2, -2048\n");
    fprintf(f, "\tli.d $a7, 0x7ff0000000000000\n\tli.d $fp, 0x80000000\n");
    for (const struct alu_ent *e = alu_tab; e->name; e++)
        fprintf(f, "\t%s $a0, $t1, $s2\n\t%s $r31, $zero, $r21\n",
                e->name, e->name);
    for (const struct imm_ent *e = imm_tab; e->name; e++) {
        if (e->sign)
            fprintf(f, "\t%s $a0, $t3, -2048\n\t%s $s1, $sp, 2047\n",
                    e->name, e->name);
        else
            fprintf(f, "\t%s $a0, $t3, 0\n\t%s $s1, $sp, 0xfff\n",
                    e->name, e->name);
    }
    fprintf(f, "\tlu52i.d $a0, $a1, -2048\n\tlu52i.d $t0, $t0, 2047\n");
    for (const struct sh_ent *e = sh_tab; e->name; e++)
        fprintf(f, "\t%s $a0, $t3, %d\n\t%s $tp, $fp, 1\n", e->name,
                e->w ? 31 : 63, e->name);
    fprintf(f, "\text.w.b $a0, $a1\n\text.w.h $t8, $s0\n");
    fprintf(f, "\trevb.2h $a0, $a1\n\trevb.4h $a0, $a1\n\trevb.2w $a0, $a1\n");
    fprintf(f, "\trevb.d $t0, $s8\n");
    fprintf(f, "\tbstrpick.w $a0, $a1, 31, 0\n\tbstrpick.w $a0, $a1, 7, 7\n");
    fprintf(f, "\tbstrpick.d $a0, $a1, 63, 0\n\tbstrpick.d $s3, $t4, 40, 12\n");
    fprintf(f, "\talsl.w $a0, $a1, $a2, 1\n\talsl.d $t0, $t1, $t2, 4\n");
    fprintf(f, "\tlu12i.w $a0, -524288\n\tlu12i.w $t0, 524287\n");
    fprintf(f, "\tlu32i.d $a0, -1\n\tlu32i.d $s0, 0x12345\n");
    for (int k = 0; pc_tab[k].name; k++)
        fprintf(f, "\t%s $a0, -524288\n\t%s $ra, 524287\n", pc_tab[k].name,
                pc_tab[k].name);
    for (const struct ls_ent *e = ls_tab; e->name; e++)
        fprintf(f, "\t%s $a2, $sp, -4\n\t%s $t8, $a1, 2047\n\t%s $r3, $r4, -2048\n",
                e->name, e->name, e->name);
    fprintf(f, "\tll.w $a0, $a1, 0\n\tll.d $t0, $sp, 32764\n");
    fprintf(f, "\tsc.w $a0, $a1, -32768\n\tsc.d $t0, $t1, 4\n");
    for (int k = 0; am_tab[k].name; k++)
        fprintf(f, "\t%s.w $a0, $a1, $a2\n\t%s.d $zero, $t1, $s0\n",
                am_tab[k].name, am_tab[k].name);
    for (const struct br_ent *e = br_tab; e->name; e++) {
        const char *regs = e->nregs == 2 ? "$a0, $t1, "
                         : e->nregs == 1 ? "$s2, " : "";
        int far = e->cond == LA_BEQZ || e->cond == LA_BNEZ;
        fprintf(f, "\t%s %s%d\n\t%s %s8\n\t%s %s%d\n", e->name, regs,
                far ? -4194304 : -131072, e->name, regs, e->name, regs,
                far ? 4194300 : 131068);
    }
    fprintf(f, "\tb -134217728\n\tb 134217724\n\tbl 8\n\tbl -4\n");
    fprintf(f, "\tjirl $ra, $t0, 0\n\tjirl $zero, $a0, -131072\n");
    fprintf(f, "\tdbar 0\n\tdbar 0x14\n\tbreak 0\n\tbreak 32767\n");
    fprintf(f, "\taddi.d $a0, $a1, (4 * 8) - 1\n\tst.d $ra, $sp, -(2 * 8)\n");
    /* (a `.+N` target is the file assembler's spelling of a label; llvm-mc
     * reads it as an expression to fix up, so it is checked against the
     * plain number by tests/golden/loongarch-asm.sh instead) */
    fprintf(f, "\tandi $a0, $a0, (1 << 8) | 0xf\n");
    for (const struct la_raw *e = la_raw_insns; e->name; e++) {
        switch (e->fmt) {
        case LAF_2R:
            fprintf(f, "\t%s $a0, $t1\n\t%s $s8, $zero\n", e->name, e->name);
            break;
        case LAF_3R:
            fprintf(f, "\t%s $a0, $t1, $s2\n\t%s $r31, $sp, $zero\n",
                    e->name, e->name);
            break;
        case LAF_AM:
            fprintf(f, "\t%s $a0, $a1, $a2\n\t%s $zero, $t1, $s0\n",
                    e->name, e->name);
            break;
        case LAF_PTR:
            fprintf(f, "\t%s $a0, $sp, -32768\n\t%s $t0, $a1, 32764\n",
                    e->name, e->name);
            break;
        case LAF_CSRRD: case LAF_CSRWR:
            fprintf(f, "\t%s $a0, 0\n\t%s $t8, 16383\n\t%s $s0, 0xc\n",
                    e->name, e->name, e->name);
            break;
        case LAF_CSRXCHG:
            fprintf(f, "\t%s $a0, $a1, 0\n\t%s $t0, $s8, 16383\n", e->name,
                    e->name);
            break;
        case LAF_CODE15:
            fprintf(f, "\t%s 0\n\t%s 32767\n", e->name, e->name);
            break;
        case LAF_NONE:
            break;
        default:
            fprintf(f, "\t%s $a0, $a1, %d, 0\n\t%s $t0, $s1, 7, 3\n", e->name,
                    e->d ? 63 : 31, e->name);
            break;
        }
    }
}
