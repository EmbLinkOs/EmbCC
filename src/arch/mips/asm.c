/* The MIPS32r2 inline-asm assembler. See asm.h. The only thing this file
 * adds to emit.c is a parser: every range is checked HERE, before the
 * encoder is called, so a template's mistake is a diagnostic and not an
 * internal error. */
#include "asm.h"

#include "emit.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../elf/elf.h"

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

struct tok;
static int tok_num(const struct tok *t, long long *out);

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
        else if (i == b && i < len)
            i++;    /* a comma with no operand before it (`nop ,1`): past
                     * it, or this loop never moved again */
    }
    return n;
}

static int tok_is(const struct tok *t, const char *s)
{
    return (int)strlen(s) == t->len && strncmp(t->s, s, (size_t)t->len) == 0;
}

static int tok_reg(const struct tok *t) { return mipsasm_gpr(t->s, t->len); }

/* A constant expression -- what a .S file's macros leave in an operand,
 * `(16 + 4 * (3))` or `-(16 + 4 * 24)` -- over numbers only (a symbol has
 * been replaced by its value, or a label by `.+N`, before this sees it):
 * + - * / % << >> & | ^ ~, unary minus and parentheses, at C's
 * precedence. */
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
        if (v > 0x7fffffffffffLL) { x->bad = 1; return 0; }
        v = v * base + d;
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
        v = op == '*' ? v * r : op == '/' ? v / r : v % r;
    }
}

static long long xp_add(struct xp *x)
{
    long long v = xp_mul(x);
    for (;;) {
        char op;
        xp_ws(x);
        if (x->p >= x->e || (*x->p != '+' && *x->p != '-'))
            return v;
        op = *x->p++;
        v = op == '+' ? v + xp_mul(x) : v - xp_mul(x);
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
        if (r < 0 || r > 62) { x->bad = 1; return 0; }
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

static int tok_expr(const char *s, int len, long long *out)
{
    struct xp x;
    long long v;
    x.p = s; x.e = s + len; x.bad = 0;
    v = xp_or(&x);
    xp_ws(&x);
    if (x.bad || x.p != x.e)
        return 0;
    *out = v;
    return 1;
}

/* A signed integer: decimal or 0x, with an optional sign -- or a
 * constant expression of them. */
static int tok_imm(const struct tok *t, long long *out)
{
    return tok_num(t, out) || tok_expr(t->s, t->len, out);
}

/* A plain number: decimal or 0x, with an optional sign. */
static int tok_num(const struct tok *t, long long *out)
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

/* `%hi(N)` and `%lo(N)` of a NUMBER, which a .S file writes for a
 * constant address (`lui $t0, %hi(0xbf000900)` after the preprocessor):
 * the halves the AHL rule pairs -- %hi rounded by 0x8000 because the
 * addiu or load that adds %lo sign-extends it. 1 = %hi, 2 = %lo, 0 when
 * the token is neither. A SYMBOL's halves are relocations
 * (mipsasm_symform). */
static int tok_half(const struct tok *t, long long *out)
{
    struct tok in;
    long long v;
    int which;
    if (t->len < 5 || t->s[0] != '%' || t->s[t->len - 1] != ')')
        return 0;
    if (!strncmp(t->s, "%hi(", 4)) which = 1;
    else if (!strncmp(t->s, "%lo(", 4)) which = 2;
    else return 0;
    in.s = t->s + 4;
    in.len = t->len - 5;
    if (!tok_imm(&in, &v))
        return -1;
    if (which == 1)
        *out = ((v + 0x8000) >> 16) & 0xffff;
    else
        *out = (long long)(short)(v & 0xffff);
    return which;
}

/* `off(reg)`, `(reg)`, as a load or store writes its address; `off` may
 * be `%lo(N)`. */
static int tok_mem(const struct tok *t, int *reg, long long *off)
{
    const char *open = NULL;
    struct tok o, r;
    /* the base is the LAST parenthesised group: the offset before it may
     * have its own, `%lo(N)` or `(16 + 4 * 3)` */
    if (t->len < 3 || t->s[t->len - 1] != ')')
        return 0;
    {
        int depth = 0;
        for (const char *c = t->s + t->len - 1; c >= t->s; c--) {
            if (*c == ')') depth++;
            else if (*c == '(' && --depth == 0) { open = c; break; }
        }
    }
    if (!open)
        return 0;
    o.s = t->s; o.len = (int)(open - t->s);
    r.s = open + 1; r.len = (int)(t->s + t->len - 1 - (open + 1));
    *off = 0;
    if (o.len > 0 && !tok_imm(&o, off) && tok_half(&o, off) != 2)
        return 0;
    *reg = tok_reg(&r);
    return *reg >= 0;
}

/* A transfer's target, from the delay slot (the encoding's own zero):
 * a bare number is that already; `.+N` / `.-N` -- which the file
 * assembler writes for a label -- is from the branch itself, four
 * bytes earlier. */
static int tok_disp(const struct tok *t, long long *out)
{
    if (t->len >= 2 && t->s[0] == '.' && (t->s[1] == '+' || t->s[1] == '-')) {
        struct tok n;
        n.s = t->s + 1;
        n.len = t->len - 1;
        if (!tok_imm(&n, out))
            return 0;
        *out -= 4;
        return 1;
    }
    return tok_imm(t, out);
}

/* ---- the assembler's mode ----------------------------------------------
 *
 * GNU as starts in `.set reorder`: the programmer writes no delay slots,
 * and the assembler puts a nop after every branch and jump (gas may fill
 * the slot instead; the nop means the same). GCC's and clang's inline
 * asm starts there too -- clang wraps each template in `.set push; .set
 * reorder` -- so a template written for them behaves the same here.
 * `.set noreorder` hands the delay slots to the programmer: the
 * instruction after a transfer is its slot, as Linux's and an RTOS's
 * hand-scheduled code writes it. */
static int g_noreorder;
static int g_pushed[8], g_npushed;

void mipsasm_reset(void)
{
    g_noreorder = 0;
    g_npushed = 0;
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

/* `.set OPTION`. The mode options change what follows; the ones that
 * name what EmbCC already is, or allow instructions this assembler then
 * refuses one by one, are accepted; the ones that change how
 * instructions are ENCODED -- MIPS16, microMIPS, Release 6, 64-bit -- are
 * refused, because the bytes would be another ISA's. */
static int set_option(const struct tok *o, char *err, int errlen)
{
    static const char *const ok[] = {
        "at", "noat", "macro", "nomacro", "mips32r2", "mips32", "mips0",
        "arch=mips32r2", "nomips16", "nomicromips", "volatile",
        "novolatile", "move", "nomove", "bopt", "nobopt", "softfloat",
        "nodsp", "nodspr2", "nomt", "novirt", "noeva", "nomsa", "oddspreg",
        "nooddspreg", "nosym32", "sym32", "dsp", "dspr2", "mt", "virt",
        "mcu", "nomcu", NULL
    };
    if (tok_is(o, "reorder"))   { g_noreorder = 0; return 0; }
    if (tok_is(o, "noreorder")) { g_noreorder = 1; return 0; }
    if (tok_is(o, "push")) {
        if (g_npushed == (int)(sizeof g_pushed / sizeof g_pushed[0]))
            FAIL(".set push nests deeper than %d",
                 (int)(sizeof g_pushed / sizeof g_pushed[0]));
        g_pushed[g_npushed++] = g_noreorder;
        return 0;
    }
    if (tok_is(o, "pop")) {
        if (!g_npushed)
            FAIL(".set pop with no .set push before it");
        g_noreorder = g_pushed[--g_npushed];
        return 0;
    }
    for (int k = 0; ok[k]; k++)
        if (tok_is(o, ok[k]))
            return 0;
    FAIL(".set %.*s is not supported: this assembler emits MIPS32r2 "
         "(no MIPS16, microMIPS, Release 6, 64-bit or floating-point "
         "instructions)", o->len, o->s);
}

static int stmt_body(const char *stmt, int len, struct code *out,
                     char *err, int errlen, int *xfer)
{
    struct tok t[MAXTOK];
    int n = split(stmt, len, t, MAXTOK);
    long long v;

    if (n == 0)
        return 0;
    /* a refusal mipsasm_symform wrote for a form it recognised */
    if (stmt[0] == '\001')
        FAIL("%.*s", len - 1, stmt + 1);
    if (tok_is(&t[0], ".set")) {
        if (n != 2)
            FAIL(".set takes one option here (.set NAME, VALUE belongs to "
                 "a .S file)");
        return set_option(&t[1], err, errlen);
    }

    if (n == 1) {
        if (tok_is(&t[0], "nop"))     { mips_nop(out); return 0; }
        if (tok_is(&t[0], "ssnop"))   { mips_shift_imm(out, MIPS_SLL, 0, 0, 1);
                                        return 0; }
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
        {
            int h = tok_half(&t[3], &v);
            if (h && e->op != MIPS_ADDIU)
                FAIL("%s cannot take %%hi or %%lo: %%hi is rounded for an "
                     "addiu's sign-extended %%lo, so write addiu", e->name);
            if (d < 0 || a < 0 || (h != 2 && !tok_imm(&t[3], &v)))
                FAIL("%s wants two registers and an immediate", e->name);
        }
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
        int h = n == 3 ? tok_half(&t[2], &v) : 0;
        if (h == 2)
            FAIL("lui takes %%hi, not %%lo");
        if (d < 0 || (h != 1 && !tok_imm(&t[2], &v)) || v < 0 || v > 0xffff)
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
        else if (tok_is(&t[0], "jr")) { mips_jr(out, r); *xfer = 1; }
        else if (tok_is(&t[0], "jalr")) { mips_jalr(out, MIPS_RA, r); *xfer = 1; }
        else if (tok_is(&t[0], "di")) mips_di(out, r);
        else mips_ei(out, r);
        return 0;
    }
    if (n == 3 && tok_is(&t[0], "jalr")) {
        int d = tok_reg(&t[1]), s = tok_reg(&t[2]);
        if (d < 0 || s < 0 || d == s)
            FAIL("jalr wants two different registers");
        mips_jalr(out, d, s);
        *xfer = 1;
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
        if (!tok_disp(&t[k], &v) || (v & 3) || v < -131072 || v > 131068)
            FAIL("%s's target must be a multiple of 4 bytes away, within "
                 "-131072..131068 of the delay slot", e->name);
        mips_w(out, mips_enc_branch(e->cond, a, b, (long)v));
        *xfer = 1;
        return 0;
    }

    /* ---- j and jal ---- */
    if (tok_is(&t[0], "j") || tok_is(&t[0], "jal")) {
        int link = tok_is(&t[0], "jal");
        /* a register only `$`-spelt: `jal 0` is an address */
        int r = n == 2 && t[1].s[0] == '$' ? tok_reg(&t[1]) : -1;
        /* `j $ra` is gas's jr, `jal $t9` its jalr, `jal $rd, $rs` too */
        if (n == 3 && link) {
            int d = t[1].s[0] == '$' ? tok_reg(&t[1]) : -1;
            int s = t[2].s[0] == '$' ? tok_reg(&t[2]) : -1;
            if (d < 0 || s < 0 || d == s)
                FAIL("jal $rd, $rs wants two different registers");
            mips_jalr(out, d, s);
            *xfer = 1;
            return 0;
        }
        if (n != 2)
            FAIL("%.*s takes one target", t[0].len, t[0].s);
        if (r >= 0) {
            if (link) mips_jalr(out, MIPS_RA, r);
            else      mips_jr(out, r);
            *xfer = 1;
            return 0;
        }
        /* A label this file placed (`.+N`): PC-relative, as b and bal --
         * the j field is region-ABSOLUTE, which an object does not know
         * for its own code. */
        if (t[1].len >= 2 && t[1].s[0] == '.') {
            if (!tok_disp(&t[1], &v) || (v & 3) || v < -131072 ||
                v > 131068)
                FAIL("%.*s to a label here must reach within 128 KiB (it "
                     "is encoded as %s); name a symbol for a longer one",
                     t[0].len, t[0].s, link ? "bal" : "b");
            mips_w(out, mips_enc_branch(link ? MIPS_BAL : MIPS_BEQ,
                                        MIPS_ZERO, MIPS_ZERO, (long)v));
            *xfer = 1;
            return 0;
        }
        /* a number: the target's place in the 256 MiB region of the
         * delay slot -- the field itself */
        if (!tok_imm(&t[1], &v) || (v & 3) || v < 0 || v > 0x0ffffffcLL)
            FAIL("%.*s wants a register, a label, or a multiple of 4 below "
                 "0x10000000 (the target's place in its 256 MiB region)",
                 t[0].len, t[0].s);
        mips_w(out, mips_enc_j(link ? 3 : 2, (unsigned long)v >> 2));
        *xfer = 1;
        return 0;
    }

    /* ---- the pseudo-instructions ---- */
    if (tok_is(&t[0], "la"))
        FAIL("la takes a symbol (a lui/addiu pair relocated against it), "
             "not a numeric local label or a number: name the label, or "
             "use li");
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

/* One statement, and in `.set reorder` mode the delay slot after a
 * transfer: a nop, so the instruction written next runs after the
 * transfer and not in its slot. */
static int one_stmt(const char *stmt, int len, struct code *out,
                    char *err, int errlen)
{
    int xfer = 0;
    int rc = stmt_body(stmt, len, out, err, errlen, &xfer);
    if (rc == 0 && xfer && !g_noreorder)
        mips_nop(out);
    return rc;
}

/* An inline-asm template starts in `.set reorder`, as GCC's and
 * clang's do (each template is assembled on its own). */
int mipsasm_assemble(const char *text, struct code *out, char *err, int errlen)
{
    mipsasm_reset();
    return mipsasm_encode(text, out, err, errlen);
}

int mipsasm_encode(const char *text, struct code *out, char *err, int errlen)
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

/* ---- for the file assembler (src/as/gas.c) ---------------------------- */

/* In a .S file a register is always written with its `$`: a bare `ra` or
 * `sp` there is a symbol. */
int mipsasm_is_reg(const char *name, int len)
{
    if (len < 2 || name[0] != '$')
        return -1;
    /* a floating-point register is a register too, not a symbol -- its
     * instructions are then refused by name, as instructions */
    if (len >= 3 && len <= 4 && name[1] == 'f' && isdigit((unsigned char)name[2]) &&
        (len == 3 || isdigit((unsigned char)name[3])))
        return 32;
    return mipsasm_gpr(name, len);
}

/* `%hi`, `%lo`, `%got`...: the word after a `%` is an operator, not a
 * symbol the file defines. */
int mipsasm_is_word(const char *stmt, const char *w, int len)
{
    (void)len;
    return w > stmt && w[-1] == '%';
}

/* The symbol at p (an identifier, not a register, not a numeric local
 * `1f`), then an optional `+K` / `-K`. Returns the length consumed, 0
 * when there is no symbol there. */
static int sym_operand(const char *p, int *slen, long *add)
{
    int n = 0;
    if (!(isalpha((unsigned char)p[0]) || p[0] == '_' || p[0] == '.' ||
          p[0] == '$'))
        return 0;
    if (p[0] == '$' || (p[0] == '.' && (p[1] == '+' || p[1] == '-' ||
                                        !p[1] || p[1] == ')')))
        return 0;                         /* a register, or `.` itself */
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

/* The statements whose operand is a SYMBOL: `jal sym` and `j sym`
 * (R_MIPS_26, the target's place in its 256 MiB region), `lui rt,
 * %hi(sym)` (R_MIPS_HI16), `addiu rt, rs, %lo(sym)` and a load or store
 * at `%lo(sym)(rs)` (R_MIPS_LO16), and gas's `la rt, sym`, which is the
 * lui and the addiu. The statement is rewritten with a zero operand for
 * mipsasm_encode, and the linker pairs each HI16 with the LO16 after it
 * (the AHL rule). The position-independent and small-data operators are
 * recognised in order to refuse them by name. */
int mipsasm_symform(const char *stmt, struct asm_symform *f)
{
    const char *p = skip_sp(stmt), *m = p, *q;
    int mlen = 0, slen;
    long add;

    while (isalpha((unsigned char)p[mlen]))
        mlen++;
    if (!mlen || (p[mlen] != ' ' && p[mlen] != '\t'))
        return 0;
    q = skip_sp(p + mlen);
    memset(f, 0, sizeof *f);

    /* the operators this assembler does not relocate */
    {
        static const char *const pic[] = {
            "%got", "%call16", "%gp_rel", "%got_disp", "%got_page",
            "%got_ofst", "%call_hi", "%call_lo", "%got_hi", "%got_lo",
            "%gottprel", "%tlsgd", "%tlsldm", "%dtprel_hi", "%dtprel_lo",
            "%tprel_hi", "%tprel_lo", "%higher", "%highest", "%neg",
            "%pcrel_hi", "%pcrel_lo", NULL
        };
        const char *pc = strchr(q, '%');
        for (int k = 0; pc && pic[k]; k++) {
            size_t l = strlen(pic[k]);
            if (!strncmp(pc, pic[k], l) && pc[l] == '(')
                return refuse_form(f, stmt, pc + 1, (int)l - 1,
                                   "this operator is for PIC, small-data or "
                                   "TLS code, and this assembler emits "
                                   "-mno-abicalls code (%hi and %lo)");
        }
    }

    if ((mlen == 1 && *m == 'j') || (mlen == 3 && !strncmp(m, "jal", 3))) {
        int n = sym_operand(q, &slen, &add);
        if (!n || *skip_sp(q + n))
            return 0;
        f->sym_at = (int)(q - stmt);
        f->sym_len = slen;
        f->addend = add;
        snprintf(f->encode, sizeof f->encode, "%.*s 0", mlen, m);
        f->site[0].off = 0;
        f->site[0].reloc = R_MIPS_26;
        f->nsites = 1;
        return 1;
    }
    if (mlen == 2 && !strncmp(m, "la", 2)) {
        const char *c = strchr(q, ',');
        const char *o;
        int n;
        if (!c)
            return 0;
        o = skip_sp(c + 1);
        n = sym_operand(o, &slen, &add);
        if (!n || *skip_sp(o + n))
            return 0;
        /* MIPS64: an address is 64 bits, and lui/addiu make only the
         * sign extension of 32 (GNU as warns and widens; neither here) */
        if (mips_is_64())
            return refuse_form(f, stmt, o, slen,
                               "la loads a 32-bit address, and a MIPS64 "
                               "address is 64 bits (nor are %highest and "
                               "%higher assembled here): load it from a "
                               ".dword holding the symbol");
        f->sym_at = (int)(o - stmt);
        f->sym_len = slen;
        f->addend = add;
        snprintf(f->encode, sizeof f->encode, "lui %.*s, 0\naddiu %.*s, %.*s, 0",
                 (int)(c - q), q, (int)(c - q), q, (int)(c - q), q);
        f->site[0].off = 0;
        f->site[0].reloc = R_MIPS_HI16;
        f->site[1].off = 4;
        f->site[1].reloc = R_MIPS_LO16;
        f->nsites = 2;
        return 1;
    }

    /* %hi(sym) and %lo(sym): which instruction may carry each */
    {
        const char *h = strstr(q, "%hi(");
        const char *l = strstr(q, "%lo(");
        const char *at = h ? h : l;
        int n;
        if (!at)
            return 0;
        n = sym_operand(at + 4, &slen, &add);
        if (!n || at[4 + n] != ')')
            return 0;                     /* %hi(NUMBER): mipsasm_encode's */
        f->sym_at = (int)(at + 4 - stmt);
        f->sym_len = slen;
        f->addend = add;
        if (h) {
            if (!(mlen == 3 && !strncmp(m, "lui", 3)))
                return refuse_form(f, stmt, at + 4, slen,
                                   "%hi(symbol) is a lui's operand");
            snprintf(f->encode, sizeof f->encode, "%.*s0%s",
                     (int)(at - stmt), stmt, at + 5 + n);
            f->site[0].reloc = R_MIPS_HI16;
        } else {
            static const char *const ls[] = {
                "lb", "lbu", "lh", "lhu", "lw", "sb", "sh", "sw", "ll", "sc",
                "lwl", "lwr", "swl", "swr", NULL
            };
            int okm = mlen == 5 && !strncmp(m, "addiu", 5);
            for (int k = 0; !okm && ls[k]; k++)
                okm = (int)strlen(ls[k]) == mlen && !strncmp(m, ls[k], (size_t)mlen);
            if (!okm)
                return refuse_form(f, stmt, at + 4, slen,
                                   "%lo(symbol) is an addiu's or a load's or "
                                   "store's offset: %hi is rounded for its "
                                   "sign extension");
            snprintf(f->encode, sizeof f->encode, "%.*s0%s",
                     (int)(at - stmt), stmt, at + 5 + n);
            f->site[0].reloc = R_MIPS_LO16;
        }
        f->site[0].off = 0;
        f->nsites = 1;
        return 1;
    }
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
    fprintf(f, "\tssnop\n\tj 0x100\n\tjal 0x0ffffffc\n\tj $ra\n\tjal $t9\n");
    fprintf(f, "\tjal $s1, $t9\n");
    fprintf(f, "\tlui $t0, %%hi(0x12348000)\n\taddiu $t0, $t0, %%lo(0x12348000)\n");
    fprintf(f, "\tlw $t1, %%lo(0x10008004)($t2)\n\tsb $a0, %%lo(-1)($sp)\n");
    fprintf(f, "\taddiu $a0, $a1, (4 * 8) - 1\n\tlw $a0, (16 + 4 * 3)($sp)\n");
    fprintf(f, "\tsw $ra, -(2 * 8)($sp)\n\tandi $v0, $v1, (1 << 12) | 0xf\n");
    fprintf(f, "\tb .+8\n\tbeq $a0, $a1, .-16\n");
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
