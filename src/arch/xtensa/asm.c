/* The Xtensa assembler. See asm.h. All this file adds to emit.c is a
 * parser: every range is checked HERE, before the encoder is called, so a
 * template's mistake is a diagnostic and not an internal error. */
#include "asm.h"
#include "../code.h"

#include "emit.h"
#include "../asmexpr.h"
#include "../../elf/elf.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Where the statement being assembled starts in its section, or -1. */
static long g_pc = -1;

void xtasm_set_pc(long pc)
{
    g_pc = pc;
}

/* ---- the density option, and relaxation ----------------------------------
 *
 * Every ESP32 core has the density option, and GNU as assembling a file
 * for one narrows each instruction that has a 16-bit form and fits it:
 * mov, movi, add, addi, l32i, s32i, ret, retw, nop, and beqz/bnez to a
 * target 4..67 bytes ahead -- unless written with GNU's `_` prefix, which
 * says "as written". A branch out of reach becomes the inverted branch
 * over a `j`, as GNU as relaxes it. Inline asm keeps every instruction
 * at three bytes: its template's label arithmetic counts on that.
 *
 * gas.c's relaxation level for a branch: 0 the narrowest that reaches,
 * 1 the 24-bit form, 2 the inverted branch and a jump; -1, pass one's,
 * the narrowest form whatever it reaches (a layout that starts small and
 * grows settles small). */
static int g_density = 1, g_level = -1, g_took;

static int g_transform = 1, g_longcalls;
void xtasm_set_density(int on) { g_density = on; }
int xtasm_density(void) { return g_density; }
void xtasm_set_transform(int on) { g_transform = on; }
void xtasm_set_longcalls(int on) { g_longcalls = on; }
int xtasm_longcalls(void) { return g_longcalls; }

/* `callN sym`: N, or -1 when the statement is no direct call */
int xtasm_call_operand(const char *stmt, const char **x, int *xlen)
{
    const char *p = stmt;
    int inc;
    while (isspace((unsigned char)*p)) p++;
    if (strncmp(p, "call", 4)) return -1;
    p += 4;
    inc = p[0] == '0' ? 0 : p[0] == '4' ? 4 : p[0] == '8' ? 8
        : (p[0] == '1' && p[1] == '2') ? 12 : -1;
    if (inc < 0) return -1;
    p += inc == 12 ? 2 : 1;
    if (!isspace((unsigned char)*p)) return -1;
    while (isspace((unsigned char)*p)) p++;
    *x = p;
    *xlen = (int)strlen(p);
    while (*xlen && isspace((unsigned char)p[*xlen - 1])) (*xlen)--;
    return inc;
}
void xtasm_set_level(int level) { g_level = level; g_took = 0; }
int xtasm_took_level(void) { return g_took; }

static void xt_h(struct code *c, unsigned h) { xt_h16(c, h); }

/* ---- registers ------------------------------------------------------------ */

int xtasm_gpr(const char *name, int len)
{
    int v;
    if (len == 2 && name[0] == 's' && name[1] == 'p')
        return XT_SP;
    if (len < 2 || len > 3 || name[0] != 'a' ||
        !isdigit((unsigned char)name[1]))
        return -1;
    v = name[1] - '0';
    if (len == 3) {
        if (!isdigit((unsigned char)name[2]) || name[1] == '0')
            return -1;
        v = v * 10 + (name[2] - '0');
    }
    return v < 16 ? v : -1;
}

int xtasm_is_reg(const char *name, int len)
{
    return xtasm_gpr(name, len);
}

/* fN, the FPU's registers, and bN, the boolean ones: 0..15, or -1 */
static int xt_rnum(const char *name, int len, char kind)
{
    int v = 0;
    if (len < 2 || len > 3 || name[0] != kind) return -1;
    for (int i = 1; i < len; i++) {
        if (!isdigit((unsigned char)name[i])) return -1;
        v = v * 10 + (name[i] - '0');
    }
    return v <= 15 ? v : -1;
}

/* The special registers of the ESP32 (an LX6), by the names GNU as gives
 * them and with the access it allows each: rsr (R), wsr (W), xsr (X).
 * The table is Espressif's assembler's own, read off it name by name;
 * INTERRUPT is INTSET to a wsr, and INTSET and INTCLEAR are write-only.
 * `q` says whether QEMU's de212 -- the referee -- names the register
 * (the ESP32 has a coprocessor-enable, a boolean register and two more
 * MISC registers the de212 lacks); the vocabulary checks the others with
 * GNU as only. */
#define R 1
#define W 2
#define X 4
struct sr_ent { const char *name; int num; int acc; int q; };
static const struct sr_ent sr_tab[] = {
    { "lbeg", 0, R | W | X, 1 }, { "lend", 1, R | W | X, 1 },
    { "lcount", 2, R | W | X, 1 }, { "sar", 3, R | W | X, 1 },
    { "br", 4, R | W | X, 0 }, { "scompare1", 12, R | W | X, 1 },
    { "acclo", 16, R | W | X, 1 }, { "acchi", 17, R | W | X, 1 },
    { "m0", 32, R | W | X, 1 }, { "m1", 33, R | W | X, 1 },
    { "m2", 34, R | W | X, 1 }, { "m3", 35, R | W | X, 1 },
    { "windowbase", 72, R | W | X, 1 }, { "windowstart", 73, R | W | X, 1 },
    { "mmid", 89, W, 1 },
    { "ibreakenable", 96, R | W | X, 1 }, { "memctl", 97, R | W | X, 1 },
    { "atomctl", 99, R | W | X, 1 }, { "ddr", 104, R | W | X, 1 },
    { "ibreaka0", 128, R | W | X, 1 }, { "ibreaka1", 129, R | W | X, 1 },
    { "dbreaka0", 144, R | W | X, 1 }, { "dbreaka1", 145, R | W | X, 1 },
    { "dbreakc0", 160, R | W | X, 1 }, { "dbreakc1", 161, R | W | X, 1 },
    { "configid0", 176, R | W, 1 },
    { "epc1", 177, R | W | X, 1 }, { "epc2", 178, R | W | X, 1 },
    { "epc3", 179, R | W | X, 1 }, { "epc4", 180, R | W | X, 1 },
    { "epc5", 181, R | W | X, 1 }, { "epc6", 182, R | W | X, 1 },
    { "epc7", 183, R | W | X, 1 }, { "depc", 192, R | W | X, 1 },
    { "eps2", 194, R | W | X, 1 }, { "eps3", 195, R | W | X, 1 },
    { "eps4", 196, R | W | X, 1 }, { "eps5", 197, R | W | X, 1 },
    { "eps6", 198, R | W | X, 1 }, { "eps7", 199, R | W | X, 1 },
    { "configid1", 208, R, 1 },
    { "excsave1", 209, R | W | X, 1 }, { "excsave2", 210, R | W | X, 1 },
    { "excsave3", 211, R | W | X, 1 }, { "excsave4", 212, R | W | X, 1 },
    { "excsave5", 213, R | W | X, 1 }, { "excsave6", 214, R | W | X, 1 },
    { "excsave7", 215, R | W | X, 1 },
    { "cpenable", 224, R | W | X, 0 },
    { "interrupt", 226, R | W, 1 }, { "intset", 226, W, 1 },
    { "intclear", 227, W, 1 }, { "intenable", 228, R | W | X, 1 },
    { "ps", 230, R | W | X, 1 }, { "vecbase", 231, R | W | X, 1 },
    { "exccause", 232, R | W | X, 1 }, { "debugcause", 233, R | W | X, 1 },
    { "ccount", 234, R | W | X, 1 }, { "prid", 235, R, 1 },
    { "icount", 236, R | W | X, 1 }, { "icountlevel", 237, R | W | X, 1 },
    { "excvaddr", 238, R | W | X, 1 },
    { "ccompare0", 240, R | W | X, 1 }, { "ccompare1", 241, R | W | X, 1 },
    { "ccompare2", 242, R | W | X, 1 },
    { "misc0", 244, R | W | X, 1 }, { "misc1", 245, R | W | X, 1 },
    { "misc2", 246, R | W | X, 0 }, { "misc3", 247, R | W | X, 0 },
    { NULL, 0, 0, 0 }
};

/* The user registers rur/wur name: THREADPTR, which ESP-IDF keeps the
 * thread pointer in. */
static const struct sr_ent ur_tab[] = {
    { "threadptr", XT_UR_THREADPTR, R | W, 0 },
    { "fcr", 232, R | W, 0 }, { "fsr", 233, R | W, 0 },
    { NULL, 0, 0, 0 }
};

static int name_is(const char *a, int alen, const char *b)
{
    if ((int)strlen(b) != alen)
        return 0;
    for (int i = 0; i < alen; i++)
        if (tolower((unsigned char)a[i]) != b[i])
            return 0;
    return 1;
}

static const struct sr_ent *reg_lookup(const struct sr_ent *tab,
                                       const char *name, int len)
{
    for (const struct sr_ent *e = tab; e->name; e++)
        if (name_is(name, len, e->name))
            return e;
    return NULL;
}

int xtasm_sr(const char *name, int len, int *access)
{
    const struct sr_ent *e = reg_lookup(sr_tab, name, len);
    if (!e)
        return -1;
    if (access)
        *access = e->acc;
    return e->num;
}

/* ---- a tiny tokeniser -------------------------------------------------------- */

#define MAXTOK 8

struct tok { const char *s; int len; };

/* The mnemonic, up to the first blank, then the operands, separated by
 * the commas outside parentheses; each trimmed. */
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

static int tok_reg(const struct tok *t)
{
    return xtasm_gpr(t->s, t->len);
}

static int tok_imm(const struct tok *t, long long *out)
{
    return asm_const_expr(t->s, t->len, out);
}

/* A transfer's target: `.`, `.+N` or `.-N`, the distance in bytes from
 * this instruction. A bare number would be an ABSOLUTE address to GNU as,
 * which a relocatable object cannot hold, so it is not one. */
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

/* ---- the instruction tables -------------------------------------------------- */

struct alu_ent { const char *name; int op; };
static const struct alu_ent alu_tab[] = {
    { "add", XT_ADD }, { "sub", XT_SUB }, { "and", XT_AND }, { "or", XT_OR },
    { "xor", XT_XOR }, { "addx2", XT_ADDX2 }, { "addx4", XT_ADDX4 },
    { "addx8", XT_ADDX8 }, { "subx2", XT_SUBX2 }, { "subx4", XT_SUBX4 },
    { "subx8", XT_SUBX8 }, { "mull", XT_MULL }, { "mul16u", XT_MUL16U },
    { "mul16s", XT_MUL16S }, { "quos", XT_QUOS }, { "quou", XT_QUOU },
    { "rems", XT_REMS }, { "remu", XT_REMU }, { "min", XT_MIN },
    { "max", XT_MAX }, { "minu", XT_MINU }, { "maxu", XT_MAXU },
    { "moveqz", XT_MOVEQZ }, { "movnez", XT_MOVNEZ }, { "movltz", XT_MOVLTZ },
    { "movgez", XT_MOVGEZ }, { "src", XT_SRC },
    { NULL, 0 }
};

/* loads and stores: t, base, offset */
struct ls_ent { const char *name; int size; int sign; int store; };
static const struct ls_ent ls_tab[] = {
    { "l8ui", 1, 0, 0 }, { "l16ui", 2, 0, 0 }, { "l16si", 2, 1, 0 },
    { "l32i", 4, 0, 0 }, { "s8i", 1, 0, 1 }, { "s16i", 2, 0, 1 },
    { "s32i", 4, 0, 1 },
    { NULL, 0, 0, 0 }
};

static const struct { const char *name; int cond; } br_tab[] = {
    { "bnone", XT_BNONE }, { "beq", XT_BEQ }, { "blt", XT_BLT },
    { "bltu", XT_BLTU }, { "ball", XT_BALL }, { "bbc", XT_BBC },
    { "bany", XT_BANY }, { "bne", XT_BNE }, { "bge", XT_BGE },
    { "bgeu", XT_BGEU }, { "bnall", XT_BNALL }, { "bbs", XT_BBS },
    { NULL, 0 }
};
static const char *const bz_tab[] = { "beqz", "bnez", "bltz", "bgez", NULL };
static const char *const bi_tab[] = { "beqi", "bnei", "blti", "bgei",
                                      "bltui", "bgeui", NULL };
static const char *const loop_tab[] = { "loop", "loopnez", "loopgtz", NULL };

/* no operands */
static const struct { const char *name; void (*fn)(struct code *); } op0_tab[] = {
    { "nop", xt_nop }, { "ill", xt_ill }, { "isync", xt_isync },
    { "rsync", xt_rsync }, { "esync", xt_esync }, { "dsync", xt_dsync },
    { "memw", xt_memw }, { "extw", xt_extw }, { "rfe", xt_rfe },
    { "rfde", xt_rfde }, { "rfwo", xt_rfwo }, { "rfwu", xt_rfwu },
    { "syscall", xt_syscall }, { "simcall", xt_simcall }, { "ret", xt_ret },
    { "retw", xt_retw },
    { NULL, NULL }
};

#define FAIL(...)  do { snprintf(err, (size_t)errlen, __VA_ARGS__); \
                        return -1; } while (0)

/* The registers t[first..first+n), each a0-a15. */
static int need_regs(const struct tok *t, int first, int n, int *r,
                     const char *what, char *err, int errlen)
{
    for (int k = 0; k < n; k++) {
        r[k] = tok_reg(&t[first + k]);
        if (r[k] < 0)
            FAIL("%s: \"%.*s\" is not a register (a0-a15, or sp)", what,
                 t[first + k].len, t[first + k].s);
    }
    return 0;
}

static int need_imm(const struct tok *t, long long *v, const char *what,
                    char *err, int errlen)
{
    if (!tok_imm(t, v))
        FAIL("%s: \"%.*s\" is not a constant", what, t->len, t->s);
    return 0;
}

/* The target operand of a transfer, as the distance from this
 * instruction; `raw` (a statement xtasm_symform wrote for a relocation)
 * has none, and gets 0 in the field. */
static int need_target(const struct tok *t, int raw, long long *rel,
                       const char *what, char *err, int errlen)
{
    if (raw) {
        *rel = 0;
        return 0;
    }
    if (!tok_target(t, rel)) {
        if (isalpha((unsigned char)t->s[0]) || t->s[0] == '_')
            FAIL("%s: \"%.*s\" is a symbol, and inline asm cannot reach one: "
                 "a template carries no relocation (call or jump through a "
                 "register, or write it in a .S file)", what, t->len, t->s);
        FAIL("%s: \"%.*s\" is not a branch target (a label, or .+N bytes "
             "from the instruction)", what, t->len, t->s);
    }
    return 0;
}

/* A special register by name or number, and whether `acc` may name it. */
static int need_sr(const struct tok *t, int acc, const char *mn,
                   const struct sr_ent *tab, char *err, int errlen)
{
    long long v;
    const struct sr_ent *e;
    if (tok_imm(t, &v)) {
        if (v < 0 || v > 255)
            FAIL("%s: %lld is not a register number (0..255)", mn, v);
        return (int)v;
    }
    e = reg_lookup(tab, t->s, t->len);
    if (!e)
        FAIL("%s: \"%.*s\" is not a%s register of the ESP32", mn, t->len,
             t->s, tab == ur_tab ? " user" : " special");
    if (!(e->acc & acc))
        FAIL("%s: %s cannot be %s", mn, e->name,
             acc == R ? "read (it is write-only)"
             : acc == W ? "written (it is read-only)"
             : "exchanged with xsr");
    return e->num;
}

/* The sixteen constants an immediate branch can test, for its message. */
static const char *b4_list(int unsigned_)
{
    return unsigned_ ? "2-8, 10, 12, 16, 32, 64, 128, 256, 32768 or 65536"
                     : "-1, 1-8, 10, 12, 16, 32, 64, 128 or 256";
}

/* One statement; `pc` its position in its section, or -1. */
static int stmt_body(const char *stmt, int len, struct code *out, long pc,
                     char *err, int errlen);

/* The form a conditional branch takes (see g_level): 0 the narrow one
 * (beqz.n/bnez.n), 1 the 24-bit one, 2 the inverted branch over a j. */
static int br_form(long pc, int raw, int can_narrow, long off, int reach24)
{
    int f;
    if (pc < 0 || raw)
        return 1;
    if (can_narrow && g_level <= 0 && (g_level < 0 || (off >= 0 && off <= 63)))
        f = 0;
    else if (g_level < 0 || (g_level <= 1 && reach24))
        f = 1;
    else
        f = 2;
    if (f > g_took)
        g_took = f;
    return f;
}

/* `beqi a2, 5, far`: bnei a2, 5 over a `j far`, as GNU as relaxes a branch
 * that does not reach -- the inverted branch narrow when it can be */
static int br_relax(const char *inv, const struct tok *t, int n, long rel,
                    int narrow_inv, struct code *out, long pc, char *err,
                    int errlen)
{
    char buf[200];
    int k, il = narrow_inv ? 2 : 3, lv = g_level, tk = g_took, rc;
    k = snprintf(buf, sizeof buf, "%s%s%s", narrow_inv ? "" : "_", inv,
                 narrow_inv ? ".n" : "");
    for (int i = 1; i < n - 1; i++)
        k += snprintf(buf + k, sizeof buf - (size_t)k, "%s %.*s", i > 1 ? "," : "",
                      t[i].len, t[i].s);
    k += snprintf(buf + k, sizeof buf - (size_t)k, ", .+%d", il + 3);
    g_level = 0;
    rc = stmt_body(buf, k, out, pc, err, errlen);
    if (rc == 0) {
        k = snprintf(buf, sizeof buf, "j .%+ld", rel - il);
        rc = stmt_body(buf, k, out, pc + il, err, errlen);
    }
    g_level = lv;
    g_took = tk;
    return rc;
}

static const char *br_inverse(const char *mn)
{
    static const char *const pr[][2] = {
        { "beq", "bne" }, { "blt", "bge" }, { "bltu", "bgeu" }, { "bany", "bnone" },
        { "ball", "bnall" }, { "bbc", "bbs" }, { "beqi", "bnei" }, { "blti", "bgei" },
        { "bltui", "bgeui" }, { "bbci", "bbsi" }, { "beqz", "bnez" }, { "bltz", "bgez" },
    };
    for (unsigned k = 0; k < sizeof pr / sizeof *pr; k++) {
        if (!strcmp(mn, pr[k][0])) return pr[k][1];
        if (!strcmp(mn, pr[k][1])) return pr[k][0];
    }
    return NULL;
}

static int stmt_body(const char *stmt, int len, struct code *out, long pc,
                     char *err, int errlen)
{
    struct tok t[MAXTOK];
    int raw = 0, n, r[4];
    long long v, w, rel;
    char mn[32];
    int ml;

    while (len > 0 && isspace((unsigned char)*stmt)) {
        stmt++;
        len--;
    }
    if (len > 0 && stmt[0] == '\003') {         /* xtasm_symform's */
        raw = 1;
        stmt++;
        len--;
    }
    n = split(stmt, len, t, MAXTOK);
    if (n == 0)
        return 0;
    /* a refusal xtasm_symform wrote for a form it recognised */
    if (t[0].s[0] == '\001')
        FAIL("%.*s", len - 1, stmt + 1);

    /* the mnemonic, lower-case: GNU's `_` (do not transform) changes
     * nothing here, where nothing is transformed */
    ml = t[0].len;
    int notrans = 0, want_n = 0;
    {
        const char *m = t[0].s;
        if (ml > 1 && m[0] == '_') {
            m++;
            ml--;
            notrans = 1;
        }
        if (ml >= (int)sizeof mn)
            FAIL("asm instruction \"%.*s\" is not in the Xtensa vocabulary",
                 t[0].len, t[0].s);
        for (int i = 0; i < ml; i++)
            mn[i] = (char)tolower((unsigned char)m[i]);
        mn[ml] = 0;
    }
    /* `mov.n`: the density option's form, which only it may take; in a
     * file every instruction that fits takes it anyway */
    if (ml > 2 && mn[ml - 2] == '.' && mn[ml - 1] == 'n') {
        if (!g_density)
            FAIL("%s is a 16-bit instruction of the density option, which "
                 "this core does not have: write %.*s", mn, ml - 2, mn);
        if (pc < 0)
            FAIL("%s is a 16-bit instruction of the density option, and an "
                 "inline asm template's instructions are three bytes each: "
                 "write %.*s", mn, ml - 2, mn);
        mn[ml -= 2] = 0;
        want_n = 1;
    }
    /* (a line gas.c has widened -- to align a loop after it -- stays at
     * 24 bits, written .n or not, as GNU as widens one) */
    int narrow = (want_n || (g_density && g_transform && pc >= 0 && !notrans)) &&
                 !(pc >= 0 && g_level >= 1);
    if (pc >= 0 && g_level >= 1)
        want_n = 0;
    if (!strcmp(mn, "bbci.l") || !strcmp(mn, "bbsi.l")) {
        mn[4] = 0;                   /* little-endian bit numbering: the same */
        ml = 4;
    }
#define IS(s) (strcmp(mn, s) == 0)

    /* ---- no operands: ret, retw, nop narrow ---- */
    if (narrow && n == 1 && (IS("ret") || IS("retw") || IS("nop"))) {
        if (IS("nop"))
            xt_h(out, 0xf03du);
        else
            xt_n_ret(out, IS("retw"));
        return 0;
    }
    if (want_n && IS("ill") && n == 1) { xt_h(out, 0xf06du); return 0; }
    if (want_n && IS("break")) {
        if (n != 2 || need_imm(&t[1], &v, mn, err, errlen) || v < 0 || v > 15)
            FAIL("break.n takes a code 0..15");
        xt_h(out, 0xf02du | (unsigned)v << 8);
        return 0;
    }
    if (want_n && !IS("ret") && !IS("retw") && !IS("nop") && !IS("mov") &&
        !IS("movi") && !IS("add") && !IS("addi") && !IS("l32i") && !IS("s32i") &&
        !IS("beqz") && !IS("bnez"))
        FAIL("%s.n is not a density instruction", mn);

    /* ---- no operands ---- */
    for (int k = 0; op0_tab[k].name; k++)
        if (IS(op0_tab[k].name)) {
            if (n != 1)
                FAIL("%s takes no operands", mn);
            op0_tab[k].fn(out);
            return 0;
        }

    /* ---- three registers ---- */
    for (const struct alu_ent *e = alu_tab; e->name; e++) {
        if (!IS(e->name))
            continue;
        if (n != 4)
            FAIL("%s takes three registers", mn);
        if (need_regs(t, 1, 3, r, mn, err, errlen))
            return -1;
        if (narrow && IS("add")) {
            xt_n_add(out, r[0], r[1], r[2]);
            return 0;
        }
        if (narrow && !want_n && IS("or") && r[1] == r[2]) {   /* mov */
            xt_n_mov(out, r[0], r[1]);
            return 0;
        }
        xt_alu(out, e->op, r[0], r[1], r[2]);
        return 0;
    }

    /* ---- two registers ---- */
    if (IS("mov") || IS("neg") || IS("abs") || IS("nsa") || IS("nsau") ||
        IS("sll") || IS("srl") || IS("sra") || IS("movsp")) {
        if (n != 3)
            FAIL("%s takes two registers", mn);
        if (need_regs(t, 1, 2, r, mn, err, errlen))
            return -1;
        if (IS("mov") && narrow) {
            xt_n_mov(out, r[0], r[1]);
            return 0;
        }
        if (IS("mov")) xt_mov(out, r[0], r[1]);
        else if (IS("neg")) xt_neg(out, r[0], r[1]);
        else if (IS("abs")) xt_abs(out, r[0], r[1]);
        else if (IS("nsa")) xt_nsa(out, r[0], r[1]);
        else if (IS("nsau")) xt_nsau(out, r[0], r[1]);
        else if (IS("sll")) xt_sll(out, r[0], r[1]);
        else if (IS("srl")) xt_srl(out, r[0], r[1]);
        else if (IS("sra")) xt_sra(out, r[0], r[1]);
        else xt_movsp(out, r[0], r[1]);
        return 0;
    }

    /* ---- one register ---- */
    if (IS("ssl") || IS("ssr") || IS("ssa8l") || IS("jx") || IS("callx0") ||
        IS("callx4") || IS("callx8") || IS("callx12")) {
        if (n != 2)
            FAIL("%s takes one register", mn);
        if (need_regs(t, 1, 1, r, mn, err, errlen))
            return -1;
        if (IS("ssl")) xt_ssl(out, r[0]);
        else if (IS("ssr")) xt_ssr(out, r[0]);
        else if (IS("ssa8l")) xt_ssa8l(out, r[0]);
        else if (IS("jx")) xt_jx(out, r[0]);
        else xt_callx(out, (int)(strtol(mn + 5, NULL, 10) / 4), r[0]);
        return 0;
    }

    /* ---- a register and a constant ---- */
    if (IS("movi")) {
        if (n != 3 || need_regs(t, 1, 1, r, mn, err, errlen))
            FAIL("movi takes a register and a constant");
        if (need_imm(&t[2], &v, mn, err, errlen))
            return -1;
        /* 0xfffff800 is -2048 as a 32-bit word */
        if (v >= 0xfffff800LL && v <= 0xffffffffLL)
            v -= 0x100000000LL;
        if (narrow && xt_n_movi_ok(v)) {
            xt_n_movi(out, r[0], (long)v);
            return 0;
        }
        if (want_n)
            FAIL("movi.n constant %lld is not -32..95", v);
        if (!xt_movi_ok(v))
            FAIL("movi constant %lld does not fit its signed 12 bits "
                 "(-2048..2047); a larger one is a literal, which a .S file "
                 "gets from `movi` and inline asm from an \"r\" operand", v);
        xt_movi(out, r[0], (long)v);
        return 0;
    }
    if (IS("addi") || IS("addmi")) {
        if (n != 4 || need_regs(t, 1, 2, r, mn, err, errlen))
            FAIL("%s takes two registers and a constant", mn);
        if (need_imm(&t[3], &v, mn, err, errlen))
            return -1;
        if (IS("addi") && narrow && xt_n_addi_ok(v)) {
            xt_n_addi(out, r[0], r[1], (long)v);
            return 0;
        }
        if (IS("addi") && narrow && !want_n && v == 0) {      /* mov.n */
            xt_n_mov(out, r[0], r[1]);
            return 0;
        }
        if (want_n)
            FAIL("addi.n constant %lld is not -1 or 1..15", v);
        if (IS("addi")) {
            if (v < -128 || v > 127)
                FAIL("addi constant %lld is not -128..127", v);
            xt_addi(out, r[0], r[1], (long)v);
        } else {
            if (v < -32768 || v > 32512 || (v & 255))
                FAIL("addmi constant %lld is not a multiple of 256 in "
                     "-32768..32512", v);
            xt_addmi(out, r[0], r[1], (long)v);
        }
        return 0;
    }

    /* ---- shifts ---- */
    if (IS("slli") || IS("srli") || IS("srai")) {
        int lo = IS("slli") ? 1 : 0, hi = IS("srli") ? 15 : 31;
        if (n != 4 || need_regs(t, 1, 2, r, mn, err, errlen))
            FAIL("%s takes two registers and an amount", mn);
        if (need_imm(&t[3], &v, mn, err, errlen))
            return -1;
        /* srli by 16..31 has no encoding: GNU as makes it extui */
        if (IS("srli") && v >= 16 && v <= 31) {
            xt_extui(out, r[0], r[1], (int)v, (int)(32 - v));
            return 0;
        }
        if (v < lo || v > hi)
            FAIL("%s amount %lld is not %d..%d%s", mn, v, lo, hi,
                 IS("srli") ? " (a longer logical shift is extui)"
                 : IS("slli") && v == 0 ? " (a shift by 0 is mov)" : "");
        if (IS("slli")) xt_slli(out, r[0], r[1], (int)v);
        else if (IS("srli")) xt_srli(out, r[0], r[1], (int)v);
        else xt_srai(out, r[0], r[1], (int)v);
        return 0;
    }
    if (IS("extui")) {
        if (n != 5 || need_regs(t, 1, 2, r, mn, err, errlen))
            FAIL("extui takes two registers, a shift and a width");
        if (need_imm(&t[3], &v, mn, err, errlen) ||
            need_imm(&t[4], &w, mn, err, errlen))
            return -1;
        if (v < 0 || v > 31 || w < 1 || w > 16)
            FAIL("extui shift %lld and width %lld: the shift is 0..31 and "
                 "the width 1..16", v, w);
        xt_extui(out, r[0], r[1], (int)v, (int)w);
        return 0;
    }
    if (IS("ssai")) {
        if (n != 2 || need_imm(&t[1], &v, mn, err, errlen))
            FAIL("ssai takes an amount");
        if (v < 0 || v > 31)
            FAIL("ssai amount %lld is not 0..31", v);
        xt_ssai(out, (int)v);
        return 0;
    }
    if (IS("sext") || IS("clamps")) {
        if (n != 4 || need_regs(t, 1, 2, r, mn, err, errlen))
            FAIL("%s takes two registers and a bit number", mn);
        if (need_imm(&t[3], &v, mn, err, errlen))
            return -1;
        if (v < 7 || v > 22)
            FAIL("%s bit %lld is not 7..22", mn, v);
        if (IS("sext")) xt_sext(out, r[0], r[1], (int)v);
        else xt_clamps(out, r[0], r[1], (int)v);
        return 0;
    }

    /* ---- memory ---- */
    for (const struct ls_ent *e = ls_tab; e->name; e++) {
        if (!IS(e->name))
            continue;
        if (n != 4 || need_regs(t, 1, 2, r, mn, err, errlen))
            FAIL("%s takes a register, a base register and an offset", mn);
        if (need_imm(&t[3], &v, mn, err, errlen))
            return -1;
        if (narrow && xt_n_mem_ok((long)v, e->size)) {
            xt_n_mem(out, r[0], r[1], (long)v, e->store);
            return 0;
        }
        if (want_n)
            FAIL("%s.n offset %lld is not a multiple of 4 in 0..60", mn, v);
        if (!xt_mem_ok((long)v, e->size) || v != (long)v)
            FAIL("%s offset %lld is not %s", mn, v,
                 e->size == 1 ? "0..255" : e->size == 2
                 ? "an even 0..510" : "a multiple of 4 in 0..1020");
        if (e->store)
            xt_store(out, r[0], r[1], (long)v, e->size);
        else
            xt_load(out, r[0], r[1], (long)v, e->size, e->sign);
        return 0;
    }
    if (IS("l32ai") || IS("s32ri") || IS("s32c1i") || IS("l32e") ||
        IS("s32e")) {
        int e = mn[3] == 'e';
        if (n != 4 || need_regs(t, 1, 2, r, mn, err, errlen))
            FAIL("%s takes a register, a base register and an offset", mn);
        if (need_imm(&t[3], &v, mn, err, errlen))
            return -1;
        if (e ? (v < -64 || v > -4 || (v & 3))
              : !xt_mem_ok((long)v, 4))
            FAIL("%s offset %lld is not %s", mn, v,
                 e ? "a multiple of 4 in -64..-4"
                   : "a multiple of 4 in 0..1020");
        if (IS("l32ai")) xt_l32ai(out, r[0], r[1], (long)v);
        else if (IS("s32ri")) xt_s32ri(out, r[0], r[1], (long)v);
        else if (IS("s32c1i")) xt_s32c1i(out, r[0], r[1], (long)v);
        else if (IS("l32e")) xt_l32e(out, r[0], r[1], (long)v);
        else xt_s32e(out, r[0], r[1], (long)v);
        return 0;
    }
    if (IS("l32r")) {
        long lit;
        if (n != 3 || need_regs(t, 1, 1, r, mn, err, errlen))
            FAIL("l32r takes a register and a literal's label");
        if (need_target(&t[2], raw, &rel, mn, err, errlen))
            return -1;
        if (raw) {               /* any literal: the linker rewrites it */
            xt_w(out, xt_enc_l32r(r[0], 4, 0));
            return 0;
        }
        if (pc < 0)
            FAIL("l32r: its literal's address is computed from where the "
                 "l32r is, which inline asm does not know: load the value "
                 "through an \"r\" operand");
        lit = pc + (long)rel;
        if (lit & 3)
            FAIL("l32r: the literal %+lld bytes away is not 4-aligned", rel);
        if (!xt_l32r_reaches(pc, lit))
            FAIL("l32r: the literal %+lld bytes away is out of reach; an "
                 "l32r loads from 4..262144 bytes BEFORE it", rel);
        xt_w(out, xt_enc_l32r(r[0], pc, lit));
        return 0;
    }

    /* ---- branches ---- */
    for (int k = 0; br_tab[k].name; k++) {
        if (!IS(br_tab[k].name))
            continue;
        if (n != 4 || need_regs(t, 1, 2, r, mn, err, errlen))
            FAIL("%s takes two registers and a target", mn);
        if (need_target(&t[3], raw, &rel, mn, err, errlen))
            return -1;
        if (br_form(pc, raw, 0, (long)rel - 4, xt_b_reaches((long)rel - 4)) == 2)
            return br_relax(br_inverse(mn), t, n, (long)rel, 0, out, pc, err, errlen);
        rel = raw ? 0 : rel - 4;
        if (!xt_b_reaches((long)rel) && pc >= 0 && g_level < 0)
            rel = 0;                 /* pass one: the size is all it needs */
        if (!xt_b_reaches((long)rel))
            FAIL("%s target %+lld bytes away is out of reach (-124..131 "
                 "from the branch)", mn, rel + 4);
        xt_w(out, xt_enc_b(br_tab[k].cond, r[0], r[1], (long)rel));
        return 0;
    }
    for (int k = 0; bz_tab[k]; k++) {
        if (!IS(bz_tab[k]))
            continue;
        if (n != 3 || need_regs(t, 1, 1, r, mn, err, errlen))
            FAIL("%s takes a register and a target", mn);
        if (need_target(&t[2], raw, &rel, mn, err, errlen))
            return -1;
        {
            int nb = k < 2 && narrow;       /* beqz.n, bnez.n */
            /* (an explicit .n is a preference, as GNU as reads one: GCC
             * writes bnez.n to a target behind it) */
            int f = br_form(pc, raw, nb, (long)rel - 4, xt_bz_reaches((long)rel - 4));
            if (f == 2)
                return br_relax(br_inverse(mn), t, n, (long)rel,
                                k < 2 && g_density && !notrans, out, pc, err, errlen);
            if (f == 0) {
                long o6 = (long)rel - 4;
                if (o6 < 0 || o6 > 63)
                    o6 = 0;          /* pass one: the size is all it needs */
                xt_h(out, xt_enc_bz_n(k, r[0], o6));
                return 0;
            }
        }
        rel = raw ? 0 : rel - 4;
        if (!xt_bz_reaches((long)rel) && pc >= 0 && g_level < 0)
            rel = 0;
        if (!xt_bz_reaches((long)rel))
            FAIL("%s target %+lld bytes away is out of reach (-2044..2051 "
                 "from the branch)", mn, rel + 4);
        xt_w(out, xt_enc_bz(k, r[0], (long)rel));
        return 0;
    }
    for (int k = 0; bi_tab[k]; k++) {
        if (!IS(bi_tab[k]))
            continue;
        if (n != 4 || need_regs(t, 1, 1, r, mn, err, errlen))
            FAIL("%s takes a register, a constant and a target", mn);
        if (need_imm(&t[2], &v, mn, err, errlen) ||
            need_target(&t[3], raw, &rel, mn, err, errlen))
            return -1;
        if (!xt_bi_ok(k, v))
            FAIL("%s cannot test %lld: the constants it can are %s", mn, v,
                 b4_list(k >= XT_BLTUI));
        if (br_form(pc, raw, 0, (long)rel - 4, xt_b_reaches((long)rel - 4)) == 2)
            return br_relax(br_inverse(mn), t, n, (long)rel, 0, out, pc, err, errlen);
        rel = raw ? 0 : rel - 4;
        if (!xt_b_reaches((long)rel) && pc >= 0 && g_level < 0)
            rel = 0;
        if (!xt_b_reaches((long)rel))
            FAIL("%s target %+lld bytes away is out of reach (-124..131 "
                 "from the branch)", mn, rel + 4);
        xt_w(out, xt_enc_bi(k, r[0], v, (long)rel));
        return 0;
    }
    if (IS("bbci") || IS("bbsi")) {
        if (n != 4 || need_regs(t, 1, 1, r, mn, err, errlen))
            FAIL("%s takes a register, a bit number and a target", mn);
        if (need_imm(&t[2], &v, mn, err, errlen) ||
            need_target(&t[3], raw, &rel, mn, err, errlen))
            return -1;
        if (v < 0 || v > 31)
            FAIL("%s bit %lld is not 0..31", mn, v);
        if (br_form(pc, raw, 0, (long)rel - 4, xt_b_reaches((long)rel - 4)) == 2)
            return br_relax(br_inverse(mn), t, n, (long)rel, 0, out, pc, err, errlen);
        rel = raw ? 0 : rel - 4;
        if (!xt_b_reaches((long)rel) && pc >= 0 && g_level < 0)
            rel = 0;
        if (!xt_b_reaches((long)rel))
            FAIL("%s target %+lld bytes away is out of reach (-124..131 "
                 "from the branch)", mn, rel + 4);
        xt_w(out, xt_enc_bbi(mn[2] == 's', r[0], (int)v, (long)rel));
        return 0;
    }
    for (int k = 0; loop_tab[k]; k++) {
        if (!IS(loop_tab[k]))
            continue;
        if (n != 3 || need_regs(t, 1, 1, r, mn, err, errlen))
            FAIL("%s takes a register and the loop's end", mn);
        if (need_target(&t[2], raw, &rel, mn, err, errlen))
            return -1;
        rel = raw ? 0 : rel - 4;
        if (!xt_loop_reaches((long)rel))
            FAIL("%s: the loop's end %+lld bytes away is not 4..259 bytes "
                 "past the instruction", mn, rel + 4);
        xt_w(out, xt_enc_loop(k, r[0], (long)rel));
        return 0;
    }
    if (IS("j")) {
        if (n != 2)
            FAIL("j takes a target");
        if (need_target(&t[1], raw, &rel, mn, err, errlen))
            return -1;
        rel = raw ? 0 : rel - 4;
        if (!xt_j_reaches((long)rel))
            FAIL("j target %+lld bytes away is out of reach (128 KiB)",
                 rel + 4);
        xt_w(out, xt_enc_j((long)rel));
        return 0;
    }
    if (IS("call0") || IS("call4") || IS("call8") || IS("call12")) {
        int inc = (int)(strtol(mn + 4, NULL, 10) / 4);
        long target;
        if (n != 2)
            FAIL("%s takes a target", mn);
        if (need_target(&t[1], raw, &rel, mn, err, errlen))
            return -1;
        if (raw) {
            xt_call(out, inc);
            return 0;
        }
        if (pc < 0)
            FAIL("%s: a call's target is a word address computed from where "
                 "the call is, which inline asm does not know: call through "
                 "a register with callx%d", mn, inc * 4);
        target = pc + (long)rel;
        if (target & 3)
            FAIL("%s: the target %+lld bytes away is not 4-aligned, and a "
                 "call's target must be (put .align 4 before it)", mn, rel);
        if (!xt_call_reaches(pc, target))
            FAIL("%s target %+lld bytes away is out of reach (512 KiB)", mn,
                 rel);
        xt_w(out, xt_enc_call_to(inc, pc, target));
        return 0;
    }

    /* ---- the windows, the system ---- */
    if (IS("entry")) {
        if (n != 3 || need_regs(t, 1, 1, r, mn, err, errlen))
            FAIL("entry takes a register and a frame size");
        if (need_imm(&t[2], &v, mn, err, errlen))
            return -1;
        if (v < 0 || v > 32760 || (v & 7))
            FAIL("entry frame %lld is not a multiple of 8 in 0..32760", v);
        xt_entry(out, r[0], (long)v);
        return 0;
    }
    if (IS("rotw")) {
        if (n != 2 || need_imm(&t[1], &v, mn, err, errlen))
            FAIL("rotw takes a rotation");
        if (v < -8 || v > 7)
            FAIL("rotw rotation %lld is not -8..7", v);
        xt_rotw(out, (int)v);
        return 0;
    }
    if (IS("break")) {
        if (n != 3 || need_imm(&t[1], &v, mn, err, errlen) ||
            need_imm(&t[2], &w, mn, err, errlen))
            FAIL("break takes two codes");
        if (v < 0 || v > 15 || w < 0 || w > 15)
            FAIL("break codes %lld, %lld are not 0..15 each", v, w);
        xt_break(out, (int)v, (int)w);
        return 0;
    }
    if (IS("rfi") || IS("waiti")) {
        int lo = IS("rfi") ? 1 : 0;
        if (n != 2 || need_imm(&t[1], &v, mn, err, errlen))
            FAIL("%s takes an interrupt level", mn);
        if (v < lo || v > 15)
            FAIL("%s level %lld is not %d..15", mn, v, lo);
        if (IS("rfi")) xt_rfi(out, (int)v);
        else xt_waiti(out, (int)v);
        return 0;
    }
    if (IS("rsil")) {
        if (n != 3 || need_regs(t, 1, 1, r, mn, err, errlen))
            FAIL("rsil takes a register and an interrupt level");
        if (need_imm(&t[2], &v, mn, err, errlen))
            return -1;
        if (v < 0 || v > 15)
            FAIL("rsil level %lld is not 0..15", v);
        xt_rsil(out, r[0], (int)v);
        return 0;
    }
    /* rsr/wsr/xsr at, SR -- or rsr.SR at; rur/wur likewise */
    if (!strncmp(mn, "rsr", 3) || !strncmp(mn, "wsr", 3) ||
        !strncmp(mn, "xsr", 3) || !strncmp(mn, "rur", 3) ||
        !strncmp(mn, "wur", 3)) {
        int ur = mn[1] == 'u';
        int acc = mn[0] == 'r' ? R : mn[0] == 'w' ? W : X;
        const struct sr_ent *tab = ur ? ur_tab : sr_tab;
        int sr;
        char base[4];
        memcpy(base, mn, 3);
        base[3] = 0;
        if (mn[3] == '.') {
            struct tok nt;
            nt.s = t[0].s + (t[0].s[0] == '_') + 4;
            nt.len = ml - 4;
            if (nt.len <= 0 || n != 2)
                FAIL("%s takes one register", mn);
            if (isdigit((unsigned char)nt.s[0]))
                FAIL("%s: a register named by number is `%s at, N`", mn,
                     base);
            sr = need_sr(&nt, acc, base, tab, err, errlen);
        } else if (mn[3] == 0) {
            if (n != 3)
                FAIL("%s takes a register and a %s register", mn,
                     ur ? "user" : "special");
            sr = need_sr(&t[2], acc, base, tab, err, errlen);
        } else {
            FAIL("asm instruction \"%.*s\" is not in the Xtensa vocabulary",
                 t[0].len, t[0].s);
        }
        if (sr < 0)
            return -1;
        if (need_regs(t, 1, 1, r, mn, err, errlen))
            return -1;
        if (ur) {
            if (acc == R) xt_rur(out, r[0], sr);
            else xt_wur(out, r[0], sr);
        } else if (acc == R) {
            xt_rsr(out, r[0], sr);
        } else if (acc == W) {
            xt_wsr(out, r[0], sr);
        } else {
            xt_xsr(out, r[0], sr);
        }
        return 0;
    }
    /* ---- MAC16: (umul|mul|mula|muls).(aa|ad|da|dd).(ll|hl|lh|hh)
     * [.lddec|.ldinc], and lddec/ldinc. m0/m1 are x (r bit 2), m2/m3 y
     * (t bit 2), an ld*'s target any m (r bits 1:0) ---- */
    if ((IS("lddec") || IS("ldinc")) && n == 3) {
        int m = xt_rnum(t[1].s, t[1].len, 'm'), a = xtasm_gpr(t[2].s, t[2].len);
        if (m < 0 || m > 3 || a < 0) FAIL("%s takes m0-m3 and an a register", mn);
        xt_w(out, (IS("lddec") ? 0x900004UL : 0x800004UL) | (unsigned long)m << 12 |
                  (unsigned long)a << 8);
        return 0;
    }
    {
        static const char *const ops[] = { "umul", "mul", "mula", "muls" };
        static const char *const halves[] = { "ll", "hl", "lh", "hh" };
        char base[8], kind[4], half[4], ld[8];
        int nf = sscanf(mn, "%7[a-z].%3[a-z].%3[a-z].%7[a-z]", base, kind, half, ld);
        int op = -1, hf = -1;
        if (nf >= 3) {
            for (int k = 0; k < 4; k++) if (!strcmp(base, ops[k])) op = k;
            for (int k = 0; k < 4; k++) if (!strcmp(half, halves[k])) hf = k;
        }
        if (op >= 0 && hf >= 0) {
            int lo = nf == 4, dec = lo && !strcmp(ld, "lddec");
            unsigned op1 = (unsigned)(op * 4 + hf), op2;
            int w = 0, a = 0, x = 0, y = 0, at = 1;
            if (lo && strcmp(ld, "lddec") && strcmp(ld, "ldinc"))
                FAIL("asm instruction \"%s\" is not in the Xtensa vocabulary", mn);
            if (lo && (op != 2 || (strcmp(kind, "da") && strcmp(kind, "dd"))))
                FAIL("%s: only mula.da and mula.dd load as they go", mn);
            if (op == 0 && strcmp(kind, "aa"))
                FAIL("umul is umul.aa only");
            if (lo) {                       /* mw, as first */
                w = xt_rnum(t[1].s, t[1].len, 'm');
                a = xtasm_gpr(t[2].s, t[2].len);
                if (n != 5 || w < 0 || w > 3 || a < 0)
                    FAIL("%s takes m0-m3, an a register and two sources", mn);
                at = 3;
            } else if (n != 3)
                FAIL("%s takes two sources", mn);
#define MREG(i, lo2) (xt_rnum(t[i].s, t[i].len, 'm') - (lo2))
            if (!strcmp(kind, "aa")) {
                int s1 = xtasm_gpr(t[at].s, t[at].len), s2 = xtasm_gpr(t[at + 1].s, t[at + 1].len);
                if (s1 < 0 || s2 < 0) FAIL("%s takes two a registers", mn);
                xt_w(out, 0x700004UL | (unsigned long)op1 << 16 | (unsigned long)s1 << 8 |
                          (unsigned long)s2 << 4);
                return 0;
            }
            if (!strcmp(kind, "ad")) {
                int s1 = xtasm_gpr(t[at].s, t[at].len);
                y = MREG(at + 1, 2);
                if (s1 < 0)
                    FAIL("%s: %.*s is not an a register", mn, t[at].len, t[at].s);
                if (y < 0 || y > 1)
                    FAIL("%s: %.*s is not m2 or m3", mn, t[at + 1].len, t[at + 1].s);
                xt_w(out, 0x300004UL | (unsigned long)op1 << 16 | (unsigned long)s1 << 8 |
                          (unsigned long)y << 6);
                return 0;
            }
            if (!strcmp(kind, "da")) {
                int s2 = xtasm_gpr(t[at + 1].s, t[at + 1].len);
                x = MREG(at, 0);
                if (x < 0 || x > 1)
                    FAIL("%s: %.*s is not m0 or m1", mn, t[at].len, t[at].s);
                if (s2 < 0)
                    FAIL("%s: %.*s is not an a register", mn, t[at + 1].len, t[at + 1].s);
                op2 = lo ? (dec ? 5u : 4u) : 6u;
                xt_w(out, (unsigned long)op2 << 20 | (unsigned long)op1 << 16 |
                          (unsigned long)(x << 2 | w) << 12 | (unsigned long)a << 8 |
                          (unsigned long)s2 << 4 | 4);
                return 0;
            }
            if (!strcmp(kind, "dd")) {
                x = MREG(at, 0);
                y = MREG(at + 1, 2);
                if (x < 0 || x > 1)
                    FAIL("%s: %.*s is not m0 or m1", mn, t[at].len, t[at].s);
                if (y < 0 || y > 1)
                    FAIL("%s: %.*s is not m2 or m3", mn, t[at + 1].len, t[at + 1].s);
                op2 = lo ? (dec ? 1u : 0u) : 2u;
                xt_w(out, (unsigned long)op2 << 20 | (unsigned long)op1 << 16 |
                          (unsigned long)(x << 2 | w) << 12 | (unsigned long)a << 8 |
                          (unsigned long)y << 6 | 4);
                return 0;
            }
#undef MREG
        }
    }

    /* ---- muluh, mulsh, ssa8b ---- */
    if ((IS("muluh") || IS("mulsh")) && n == 4) {
        if (need_regs(t, 1, 3, r, mn, err, errlen)) return -1;
        xt_w(out, (IS("muluh") ? 0xa20000UL : 0xb20000UL) | (unsigned long)r[0] << 12 |
                  (unsigned long)r[1] << 8 | (unsigned long)r[2] << 4);
        return 0;
    }
    if (IS("ssa8b") && n == 2) {
        if (need_regs(t, 1, 1, r, mn, err, errlen)) return -1;
        xt_w(out, 0x403000UL | (unsigned long)r[0] << 8);
        return 0;
    }

    /* ---- the floating-point coprocessor (the ESP32's) ----
     * RRR with op1 0xA (FP0) or 0xB (FP1); r the result, s and t the
     * sources; op2 0xF in FP0 is the one-operand group, t its selector */
    {
        static const struct { const char *n; unsigned op1, op2; char form; int sel; } fp[] = {
            /* form: F f,f,f  U f,f (sel)  C f,f,a  K b,f,f  I f,a,imm  T a,f,imm
             *       W f,a  R a,f  B f,f,b  Q f,imm */
            {"add.s",0xa,0x0,'F',0},{"sub.s",0xa,0x1,'F',0},{"mul.s",0xa,0x2,'F',0},
            {"madd.s",0xa,0x4,'F',0},{"msub.s",0xa,0x5,'F',0},{"maddn.s",0xa,0x6,'F',0},
            {"divn.s",0xa,0x7,'F',0},
            {"round.s",0xa,0x8,'T',0},{"trunc.s",0xa,0x9,'T',0},{"floor.s",0xa,0xa,'T',0},
            {"ceil.s",0xa,0xb,'T',0},{"float.s",0xa,0xc,'I',0},{"ufloat.s",0xa,0xd,'I',0},
            {"utrunc.s",0xa,0xe,'T',0},
            {"mov.s",0xa,0xf,'U',0},{"abs.s",0xa,0xf,'U',1},{"const.s",0xa,0xf,'Q',3},
            {"rfr",0xa,0xf,'R',4},{"wfr",0xa,0xf,'W',5},{"neg.s",0xa,0xf,'U',6},
            {"div0.s",0xa,0xf,'U',7},{"recip0.s",0xa,0xf,'U',8},{"sqrt0.s",0xa,0xf,'U',9},
            {"rsqrt0.s",0xa,0xf,'U',10},{"nexp01.s",0xa,0xf,'U',11},{"mksadj.s",0xa,0xf,'U',12},
            {"mkdadj.s",0xa,0xf,'U',13},{"addexp.s",0xa,0xf,'U',14},{"addexpm.s",0xa,0xf,'U',15},
            {"un.s",0xb,0x1,'K',0},{"oeq.s",0xb,0x2,'K',0},{"ueq.s",0xb,0x3,'K',0},
            {"olt.s",0xb,0x4,'K',0},{"ult.s",0xb,0x5,'K',0},{"ole.s",0xb,0x6,'K',0},
            {"ule.s",0xb,0x7,'K',0},
            {"moveqz.s",0xb,0x8,'C',0},{"movnez.s",0xb,0x9,'C',0},{"movltz.s",0xb,0xa,'C',0},
            {"movgez.s",0xb,0xb,'C',0},{"movf.s",0xb,0xc,'B',0},{"movt.s",0xb,0xd,'B',0},
            {NULL,0,0,0,0}
        };
        for (int k = 0; fp[k].n; k++) {
            int a = -1, b = -1, c = -1, nn;
            long long im = 0;
            if (!IS(fp[k].n)) continue;
            nn = fp[k].form == 'F' || fp[k].form == 'K' || fp[k].form == 'C' ||
                 fp[k].form == 'B' || fp[k].form == 'I' || fp[k].form == 'T' ? 4 : 3;
            if (n != nn) FAIL("%s takes %d operands", mn, nn - 1);
#define FR(i) xt_rnum(t[i].s, t[i].len, 'f')
#define AR(i) xtasm_gpr(t[i].s, t[i].len)
#define BR(i) xt_rnum(t[i].s, t[i].len, 'b')
            switch (fp[k].form) {
            case 'F': a = FR(1); b = FR(2); c = FR(3); break;
            case 'K': a = BR(1); b = FR(2); c = FR(3); break;
            case 'C': a = FR(1); b = FR(2); c = AR(3); break;
            case 'B': a = FR(1); b = FR(2); c = BR(3); break;
            case 'U': a = FR(1); b = FR(2); c = fp[k].sel; break;
            case 'W': a = FR(1); b = AR(2); c = fp[k].sel; break;
            case 'R': a = AR(1); b = FR(2); c = fp[k].sel; break;
            case 'I': a = FR(1); b = AR(2); c = 0; break;
            case 'T': a = AR(1); b = FR(2); c = 0; break;
            case 'Q': a = FR(1); b = 0; c = fp[k].sel; break;
            }
#undef FR
#undef AR
#undef BR
            if (a < 0 || b < 0 || c < 0) {
                /* name the operand, and what it should have been */
                static const char *const want[][3] = {
                    ['F'] = { "f", "f", "f" }, ['K'] = { "b", "f", "f" },
                    ['C'] = { "f", "f", "a" }, ['B'] = { "f", "f", "b" },
                    ['U'] = { "f", "f", "" }, ['W'] = { "f", "a", "" },
                    ['R'] = { "a", "f", "" }, ['I'] = { "f", "a", "" },
                    ['T'] = { "a", "f", "" }, ['Q'] = { "f", "", "" },
                };
                int bad = a < 0 ? 0 : b < 0 ? 1 : 2;
                FAIL("%s: %.*s is not %s %s register", mn, t[bad + 1].len, t[bad + 1].s,
                     want[(int)fp[k].form][bad][0] == 'a' ? "an" : "a",
                     want[(int)fp[k].form][bad]);
            }
            if (fp[k].form == 'I' || fp[k].form == 'T') {
                if (need_imm(&t[3], &im, mn, err, errlen)) return -1;
                if (im < 0 || im > 15) FAIL("%s scale %lld is not 0..15", mn, im);
                c = (int)im;
            }
            if (fp[k].form == 'Q') {
                if (need_imm(&t[2], &im, mn, err, errlen)) return -1;
                if (im < 0 || im > 15) FAIL("const.s %lld is not 0..15", im);
                b = (int)im;
            }
            xt_w(out, (unsigned long)fp[k].op2 << 20 | (unsigned long)fp[k].op1 << 16 |
                      (unsigned long)a << 12 | (unsigned long)b << 8 | (unsigned long)c << 4);
            return 0;
        }
    }
    /* lsi/ssi f, a, offset (0..1020, 4-aligned); lsx/ssx f, a, a */
    if ((IS("lsi") || IS("ssi") || IS("lsiu") || IS("ssiu")) && n == 4) {
        int f = xt_rnum(t[1].s, t[1].len, 'f'), a = xtasm_gpr(t[2].s, t[2].len);
        if (f < 0 || a < 0 || need_imm(&t[3], &v, mn, err, errlen))
            FAIL("%s takes an f register, an a register and an offset", mn);
        if (v < 0 || v > 1020 || (v & 3))
            FAIL("%s offset %lld is not a multiple of 4 in 0..1020", mn, v);
        xt_w(out, (unsigned long)(v / 4) << 16 |
                  (unsigned long)(IS("lsi") ? 0 : IS("ssi") ? 4 : IS("lsiu") ? 8 : 12) << 12 |
                  (unsigned long)a << 8 | (unsigned long)f << 4 | 3);
        return 0;
    }
    if ((IS("lsx") || IS("ssx") || IS("lsxu") || IS("ssxu")) && n == 4) {
        int f = xt_rnum(t[1].s, t[1].len, 'f'), a = xtasm_gpr(t[2].s, t[2].len),
            b = xtasm_gpr(t[3].s, t[3].len);
        if (f < 0 || a < 0 || b < 0)
            FAIL("%s takes an f register and two a registers", mn);
        xt_w(out, (unsigned long)(IS("lsx") ? 0 : IS("ssx") ? 4 : IS("lsxu") ? 1 : 5) << 20 |
                  0x80000UL | (unsigned long)f << 12 | (unsigned long)a << 8 |
                  (unsigned long)b << 4);
        return 0;
    }
    /* the boolean registers */
    {
        static const struct { const char *n; unsigned op2; } bo[] = {
            {"andb",0},{"andbc",1},{"orb",2},{"orbc",3},{"xorb",4},{NULL,0}
        };
        for (int k = 0; bo[k].n; k++)
            if (IS(bo[k].n)) {
                int a, b, c;
                if (n != 4) FAIL("%s takes three b registers", mn);
                a = xt_rnum(t[1].s, t[1].len, 'b'); b = xt_rnum(t[2].s, t[2].len, 'b');
                c = xt_rnum(t[3].s, t[3].len, 'b');
                if (a < 0 || b < 0 || c < 0) FAIL("%s takes three b registers", mn);
                xt_w(out, (unsigned long)bo[k].op2 << 20 | 0x20000UL | (unsigned long)a << 12 |
                          (unsigned long)b << 8 | (unsigned long)c << 4);
                return 0;
            }
        if ((IS("any4") || IS("all4") || IS("any8") || IS("all8")) && n == 3) {
            int d = xt_rnum(t[1].s, t[1].len, 'b'), sb = xt_rnum(t[2].s, t[2].len, 'b');
            int w8 = mn[3] == '8', al = mn[1] == 'l';
            if (d < 0 || sb < 0 || (sb & (w8 ? 7 : 3)))
                FAIL("%s takes a b register and the first of %d aligned b registers",
                     mn, w8 ? 8 : 4);
            xt_w(out, (unsigned long)(8 + al + 2 * w8) << 12 | (unsigned long)sb << 8 |
                      (unsigned long)d << 4);
            return 0;
        }
        if ((IS("movf") || IS("movt")) && n == 4) {
            if (need_regs(t, 1, 2, r, mn, err, errlen)) return -1;
            int b = xt_rnum(t[3].s, t[3].len, 'b');
            if (b < 0) FAIL("%s takes two a registers and a b register", mn);
            xt_w(out, (IS("movf") ? 0xc30000UL : 0xd30000UL) | (unsigned long)r[0] << 12 |
                      (unsigned long)r[1] << 8 | (unsigned long)b << 4);
            return 0;
        }
        if ((IS("bf") || IS("bt")) && n == 3) {
            int b = xt_rnum(t[1].s, t[1].len, 'b');
            if (b < 0) FAIL("%s takes a b register and a target", mn);
            if (need_target(&t[2], raw, &rel, mn, err, errlen))
                return -1;
            if (br_form(pc, raw, 0, (long)rel - 4, xt_b_reaches((long)rel - 4)) == 2)
                return br_relax(IS("bf") ? "bt" : "bf", t, n, (long)rel, 0, out, pc, err, errlen);
            rel = raw ? 0 : rel - 4;
            if (!xt_b_reaches((long)rel) && pc >= 0 && g_level < 0)
                rel = 0;
            if (!xt_b_reaches((long)rel))
                FAIL("%s target %+lld bytes away is out of reach (-124..131 "
                     "from the branch)", mn, rel + 4);
            xt_w(out, xt_enc_rri8(6, IS("bt"), b, 7, (unsigned)rel & 0xff));
            return 0;
        }
    }
#undef IS
    FAIL("asm instruction \"%.*s\" is not in the Xtensa vocabulary",
         (int)(t[n - 1].s + t[n - 1].len - t[0].s), t[0].s);
}

/* ---- a template ----------------------------------------------------------------
 *
 * Its statements, with GCC's numeric labels resolved: `1:` defines the
 * next instance of label 1, `1b` is the latest one before and `1f` the
 * next one after. Every instruction is three bytes, so a label's place is
 * three bytes per instruction before it -- and a directive's own bytes:
 * `.ascii`, `.asciz`/`.string` and the alignments (code_asm_directive),
 * which an RTOS's layout probe writes in a function (`.ascii "->EMB_PROBE
 * x %c0 %c1"` and `.p2align 2`); an alignment's padding is decided where
 * the bytes land (code_put_asm). A template with labels is GNU-assembled
 * whole (GAS_INLINE), so no branch here crosses one. */

#define MAXSTMT 512

struct stm { const char *s; int len; int lab[4]; int nlab; int off;
             int dir, size, align; };

/* `.word`, `.short`, `.byte` as on any 32-bit target, and `.align N` in
 * BYTES, as GNU as reads it for Xtensa */
static const struct asm_dirs xt_dirs = { asm_data_w32, 1, 0 };

/* The statements of `text`, comments stripped, labels split off. */
static int stmts(const char *text, struct stm *st, int max, char *err,
                 int errlen)
{
    int ns = 0, off = 0;
    const char *p = text;
    while (*p) {
        const char *start = p;
        int len;
        /* (a `;` or `#` inside a .ascii string ends nothing) */
        p += asm_stmt_len(p, ";");
        len = asm_cut_comment(start, (int)(p - start), "#", 1);
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
        st[ns].off = off;
        st[ns].dir = st[ns].size = st[ns].align = 0;
        {
            int any = 0, i = 0;
            for (int q = 0; q < len; q++)
                any |= !isspace((unsigned char)start[q]);
            while (i < len && isspace((unsigned char)start[i]))
                i++;
            if (any && start[i] == '.') {     /* a directive: its bytes */
                struct code tmp = { 0 };
                int r = code_asm_directive(start, len, &tmp, &xt_dirs, err,
                                           errlen);
                if (r < 0)
                    return -1;
                if (r == 0)
                    FAIL("directive \"%.*s\" is not one Xtensa inline asm "
                         "takes (.ascii, .asciz, .string, .byte, .short, "
                         ".word, .p2align, .balign, .align)", len - i,
                         start + i);
                st[ns].dir = 1;
                st[ns].size = tmp.len;
                st[ns].align = tmp.narange > 0;
                off += tmp.len;
                free(tmp.p);
                free(tmp.drange);
                free(tmp.arange);
                free(tmp.rel);
            } else if (any) {
                off += 3;
            } else {
                st[ns].len = 0;
            }
        }
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
                            s[i - 1] == '_' || s[i - 1] == '.');
        while (j < len && isdigit((unsigned char)s[j]))
            j++;
        if (!mid && j > i && j < len && (s[j] == 'b' || s[j] == 'f') &&
            (j + 1 >= len || !(isalnum((unsigned char)s[j + 1]) ||
                               s[j + 1] == '_'))) {
            int lab = atoi(s + i), fwd = s[j] == 'f';
            /* backward: the latest definition at or before this statement
             * (a label on this statement is before it); forward: the next
             * one after it */
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

static int assemble(const char *text, struct code *out, char *err, int errlen);

/* The code generator's narrowing (xt_set_narrow) is off here: an asm
 * statement is assembled as asm.c's own rules say -- a template's three
 * bytes each, a file's as GNU as narrows. */
int xtasm_assemble(const char *text, struct code *out, char *err, int errlen)
{
    int was = xt_set_narrow(0), rc = assemble(text, out, err, errlen);
    xt_set_narrow(was);
    return rc;
}

static int assemble(const char *text, struct code *out, char *err, int errlen)
{
    static struct stm st[MAXSTMT];
    long pc = g_pc;
    int ns, rc = 0, start = out->len;
    g_pc = -1;
    ns = stmts(text, st, MAXSTMT, err, errlen);
    if (ns < 0)
        return -1;
    for (int k = 0; k < ns && rc == 0; k++) {
        char buf[512];
        int len;
        if (!st[k].len)
            continue;
        len = resolve_labels(st, ns, k, buf, (int)sizeof buf, err, errlen);
        if (len < 0)
            return -1;
        int before = out->len;
        if (st[k].dir && pc < 0) {           /* a template's directive */
            if (code_asm_directive(buf, len, out, &xt_dirs, err, errlen) < 0)
                return -1;
            if (out->len - before != st[k].size)
                FAIL("internal: a directive of %d bytes, sized %d",
                     out->len - before, st[k].size);
            continue;
        }
        rc = stmt_body(buf, len, out, pc < 0 ? -1 : pc + st[k].off, err,
                       errlen);
        if (pc >= 0)
            continue;               /* a file's statement: any length */
        if (rc == 0 && out->len - before != 3 && out->len != before)
            FAIL("internal: a statement of %d bytes", out->len - before);
        if (rc == 0 && out->len - start != st[k].off + 3)
            FAIL("internal: the template's statements are not three bytes "
                 "each");
    }
    return rc;
}

/* ---- for the file assembler (src/as/gas.c) ---------------------------------- */

static const char *skip_sp(const char *p)
{
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

/* The mnemonic at p: its length, lower-cased into mn without a leading
 * `_`. 0 when there is none. */
static int mnemonic(const char *p, char *mn, int cap)
{
    int n = 0, k = 0;
    while (isalnum((unsigned char)p[n]) || p[n] == '.' || p[n] == '_')
        n++;
    for (int i = p[0] == '_'; i < n && k < cap - 1; i++)
        mn[k++] = (char)tolower((unsigned char)p[i]);
    mn[k] = 0;
    return n;
}

int xtasm_is_word(const char *stmt, const char *w, int len)
{
    char mn[16];
    const char *p = skip_sp(stmt);
    mnemonic(p, mn, (int)sizeof mn);
    if (!strcmp(mn, "rsr") || !strcmp(mn, "wsr") || !strcmp(mn, "xsr"))
        return reg_lookup(sr_tab, w, len) != NULL;
    if (!strcmp(mn, "rur") || !strcmp(mn, "wur"))
        return reg_lookup(ur_tab, w, len) != NULL;
    return xt_rnum(w, len, 'f') >= 0 || xt_rnum(w, len, 'b') >= 0 ||
           (xt_rnum(w, len, 'm') >= 0 && xt_rnum(w, len, 'm') <= 3);
}

int xtasm_is_entry(const char *stmt)
{
    char mn[16];
    int n = mnemonic(skip_sp(stmt), mn, (int)sizeof mn);
    return n && !strcmp(mn, "entry");
}

int xtasm_movi_operand(const char *stmt, const char **x, int *xlen)
{
    const char *p = skip_sp(stmt), *q;
    int r, n, e;
    if (strncmp(p, "movi", 4) || !isspace((unsigned char)p[4]))
        return -1;
    q = skip_sp(p + 4);
    n = 0;
    while (isalnum((unsigned char)q[n]))
        n++;
    r = xtasm_gpr(q, n);
    q = skip_sp(q + n);
    if (r < 0 || *q != ',')
        return -1;
    q = skip_sp(q + 1);
    e = (int)strlen(q);
    while (e > 0 && isspace((unsigned char)q[e - 1]))
        e--;
    *x = q;
    *xlen = e;
    return r;
}

/* The symbol at p (an identifier, not a register, not `.` itself), then
 * an optional `+K` / `-K`. Returns the length consumed, 0 when there is
 * no symbol there. */
static int sym_operand(const char *p, int *slen, long *add)
{
    int n = 0;
    if (!asm_sym0((unsigned char)p[0]))
        return 0;
    if (p[0] == '.' && !(isalnum((unsigned char)p[1]) || p[1] == '_'))
        return 0;                         /* `.` itself */
    while (asm_symc((unsigned char)p[n]))
        n++;
    if (xtasm_gpr(p, n) >= 0)
        return 0;
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

/* The statements whose target operand -- the last -- is a SYMBOL: every
 * branch, the loops, j, call0/4/8/12 and l32r, each one instruction
 * carrying an R_XTENSA_SLOT0_OP (the linker reads which field from the
 * opcode). The statement handed back has a zero in that field. `_movi`
 * of a symbol is refused: GNU's `_` forbids the l32r a symbol needs. */
int xtasm_symform(const char *stmt, struct asm_symform *f)
{
    static const char *const forms[] = {
        "j", "call0", "call4", "call8", "call12", "l32r",
        "beqz", "bnez", "bltz", "bgez", "beqi", "bnei", "blti", "bgei",
        "bltui", "bgeui", "bbci", "bbsi", "bbci.l", "bbsi.l",
        "bnone", "beq", "blt", "bltu", "ball", "bbc", "bany", "bne", "bge",
        "bgeu", "bnall", "bbs", "loop", "loopnez", "loopgtz", NULL
    };
    char mn[16];
    const char *p = skip_sp(stmt), *o, *c;
    int ml, slen, n, k;
    long add;

    ml = mnemonic(p, mn, (int)sizeof mn);
    if (!ml || (p[ml] != ' ' && p[ml] != '\t'))
        return 0;
    memset(f, 0, sizeof *f);
    /* the last operand */
    o = skip_sp(p + ml);
    for (c = o; *c; c++)
        if (*c == ',')
            o = skip_sp(c + 1);
    n = sym_operand(o, &slen, &add);
    if (!n || *skip_sp(o + n))
        return 0;
    if (!strcmp(mn, "movi"))
        return refuse_form(f, stmt, o, slen,
                           "_movi takes a constant: the `_` keeps it from "
                           "becoming the l32r of a literal that a symbol "
                           "needs; write movi");
    for (k = 0; forms[k]; k++)
        if (!strcmp(mn, forms[k]))
            break;
    if (!forms[k])
        return 0;
    f->sym_at = (int)(o - stmt);
    f->sym_len = slen;
    f->addend = add;
    snprintf(f->encode, sizeof f->encode, "\003%.*s0", (int)(o - stmt), stmt);
    f->site[0].off = 0;
    f->site[0].reloc = R_XTENSA_SLOT0_OP;
    f->nsites = 1;
    return 1;
}

/* ---- the referee's input --------------------------------------------------------
 *
 * Each line is a statement, an @, and what QEMU's de212 disassembler
 * prints for it, written from the same table entry -- the operands as the
 * instruction means them -- so a wrong opcode, field order or table value
 * reads back differently. tools/xtasmcheck fills in the placeholders that
 * depend on where the line lands:
 *
 *   {+N}   (in the expected text) the address of this instruction + N
 *   {C+K}  a call target, the instruction's address rounded down to 4,
 *          plus 4 + K: `.+D` in the statement, the address in the text
 *   {L-K}  an l32r's literal, the instruction's address + 3 rounded down
 *          to 4, less K
 *
 * An expected text of `!` marks a form the de212 lacks (an ESP32-only
 * register); GNU as referees it alone. */
void xtasm_vocabulary(FILE *f)
{
    static const char *const rg[] = {
        "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7", "a8", "a9", "a10",
        "a11", "a12", "a13", "a14", "a15"
    };
    int k, j;
    for (const struct alu_ent *e = alu_tab; e->name; e++)
        for (j = 0; j < 16; j += 5) {
            const char *a = rg[j], *b = rg[(j + 7) % 16],
                       *c = rg[(j + 13) % 16];
            fprintf(f, "%s %s, %s, %s@%s\t%s, %s, %s\n", e->name, a, b, c,
                    e->name, a, b, c);
        }
    for (j = 0; j < 16; j++) {
        const char *a = rg[j], *b = rg[(j + 3) % 16];
        fprintf(f, "mov %s, %s@or\t%s, %s, %s\n", a, b, a, b, b);
        fprintf(f, "neg %s, %s@neg\t%s, %s\n", a, b, a, b);
        fprintf(f, "abs %s, %s@abs\t%s, %s\n", b, a, b, a);
        fprintf(f, "nsa %s, %s@nsa\t%s, %s\n", a, b, a, b);
        fprintf(f, "nsau %s, %s@nsau\t%s, %s\n", b, a, b, a);
        fprintf(f, "sll %s, %s@sll\t%s, %s\n", a, b, a, b);
        fprintf(f, "srl %s, %s@srl\t%s, %s\n", b, a, b, a);
        fprintf(f, "sra %s, %s@sra\t%s, %s\n", a, b, a, b);
        fprintf(f, "movsp %s, %s@movsp\t%s, %s\n", a, b, a, b);
        fprintf(f, "ssl %s@ssl\t%s\nssr %s@ssr\t%s\nssa8l %s@ssa8l\t%s\n",
                a, a, b, b, a, a);
        fprintf(f, "jx %s@jx\t%s\n", a, a);
        for (k = 0; k < 4; k++)
            fprintf(f, "callx%d %s@callx%d\t%s\n", 4 * k, rg[(j + k) % 16],
                    4 * k, rg[(j + k) % 16]);
    }
    fprintf(f, "mov a2, sp@or\ta2, a1, a1\nmov sp, a7@or\ta1, a7, a7\n");
    {
        static const long mv[] = { -2048, -1, 0, 1, 2047, 1000, -999 };
        for (j = 0; j < 7; j++)
            fprintf(f, "movi %s, %ld@movi\t%s, %ld\n", rg[(j * 5) % 16],
                    mv[j], rg[(j * 5) % 16], mv[j]);
        fprintf(f, "movi a4, 0xfffff800@movi\ta4, -2048\n");
        fprintf(f, "movi a4, (1 << 10) | 3@movi\ta4, 1027\n");
    }
    {
        static const long ai[] = { -128, 127, 0, -1, 64 };
        static const long am[] = { -32768, 32512, 0, 256, -256 };
        for (j = 0; j < 5; j++) {
            fprintf(f, "addi %s, %s, %ld@addi\t%s, %s, %ld\n", rg[j + 2],
                    rg[15 - j], ai[j], rg[j + 2], rg[15 - j], ai[j]);
            fprintf(f, "addmi %s, %s, %ld@addmi\t%s, %s, %ld\n", rg[15 - j],
                    rg[j], am[j], rg[15 - j], rg[j], am[j]);
        }
    }
    for (j = 1; j <= 31; j += 6)
        fprintf(f, "slli a%d, a%d, %d@slli\ta%d, a%d, %d\n", j % 16,
                (j + 4) % 16, j, j % 16, (j + 4) % 16, j);
    fprintf(f, "slli a3, a4, 31@slli\ta3, a4, 31\n");
    for (j = 0; j <= 15; j += 5)
        fprintf(f, "srli a%d, a%d, %d@srli\ta%d, a%d, %d\n", (j + 1) % 16,
                (j + 9) % 16, j, (j + 1) % 16, (j + 9) % 16, j);
    for (j = 0; j <= 31; j += 7)
        fprintf(f, "srai a%d, a%d, %d@srai\ta%d, a%d, %d\n", (j + 2) % 16,
                (j + 5) % 16, j, (j + 2) % 16, (j + 5) % 16, j);
    fprintf(f, "srai a3, a4, 31@srai\ta3, a4, 31\n");
    fprintf(f, "extui a2, a3, 0, 1@extui\ta2, a3, 0, 1\n");
    fprintf(f, "extui a9, a12, 31, 1@extui\ta9, a12, 31, 1\n");
    fprintf(f, "extui a4, a5, 16, 16@extui\ta4, a5, 16, 16\n");
    fprintf(f, "extui a15, a0, 5, 11@extui\ta15, a0, 5, 11\n");
    for (j = 0; j < 32; j += 7)
        fprintf(f, "ssai %d@ssai\t%d\n", j, j);
    fprintf(f, "ssai 31@ssai\t31\n");
    for (j = 7; j <= 22; j += 5) {
        fprintf(f, "sext a%d, a%d, %d@sext\ta%d, a%d, %d\n", j % 16,
                (j + 3) % 16, j, j % 16, (j + 3) % 16, j);
        fprintf(f, "clamps a%d, a%d, %d@clamps\ta%d, a%d, %d\n",
                (j + 1) % 16, j % 16, j, (j + 1) % 16, j % 16, j);
    }
    fprintf(f, "sext a2, a3, 22@sext\ta2, a3, 22\n");
    for (const struct ls_ent *e = ls_tab; e->name; e++) {
        long hi = e->size == 1 ? 255 : e->size == 2 ? 510 : 1020;
        fprintf(f, "%s a2, a1, 0@%s\ta2, a1, 0\n", e->name, e->name);
        fprintf(f, "%s a15, a9, %ld@%s\ta15, a9, %ld\n", e->name, hi,
                e->name, hi);
        fprintf(f, "%s a7, sp, %d@%s\ta7, a1, %d\n", e->name, 2 * e->size,
                e->name, 2 * e->size);
        fprintf(f, "%s a0, a12, 4 * %d@%s\ta0, a12, %d\n", e->name,
                e->size, e->name, 4 * e->size);
    }
    {
        static const char *const ex[] = { "l32ai", "s32ri", "s32c1i" };
        for (k = 0; k < 3; k++)
            fprintf(f, "%s a3, a4, 0@%s\ta3, a4, 0\n%s a11, a2, 1020@%s\t"
                       "a11, a2, 1020\n", ex[k], ex[k], ex[k], ex[k]);
        fprintf(f, "l32e a0, a5, -16@l32e\ta0, a5, -16\n");
        fprintf(f, "l32e a9, a1, -64@l32e\ta9, a1, -64\n");
        fprintf(f, "s32e a3, a9, -4@s32e\ta3, a9, -4\n");
        fprintf(f, "s32e a12, a13, -36@s32e\ta12, a13, -36\n");
    }
    {
        static const long lb[] = { 4, 8, 12, 1024, 262144 };
        for (j = 0; j < 5; j++)
            fprintf(f, "l32r a%d, {L-%ld}@l32r\ta%d, {L-%ld}\n", j * 3,
                    lb[j], j * 3, lb[j]);
    }
    for (k = 0; br_tab[k].name; k++) {
        const char *s = rg[(k + 2) % 16], *t2 = rg[(k * 5 + 1) % 16];
        fprintf(f, "%s %s, %s, .+131@%s\t%s, %s, {+131}\n", br_tab[k].name,
                s, t2, br_tab[k].name, s, t2);
        fprintf(f, "%s %s, %s, .-124@%s\t%s, %s, {+-124}\n", br_tab[k].name,
                t2, s, br_tab[k].name, t2, s);
        fprintf(f, "%s %s, %s, .@%s\t%s, %s, {+0}\n", br_tab[k].name, s, t2,
                br_tab[k].name, s, t2);
    }
    for (k = 0; bz_tab[k]; k++) {
        fprintf(f, "%s a%d, .+2051@%s\ta%d, {+2051}\n", bz_tab[k], k + 3,
                bz_tab[k], k + 3);
        fprintf(f, "%s a%d, .-2044@%s\ta%d, {+-2044}\n", bz_tab[k], 15 - k,
                bz_tab[k], 15 - k);
        fprintf(f, "%s a%d, .+8@%s\ta%d, {+8}\n", bz_tab[k], k, bz_tab[k], k);
    }
    {
        static const long long b4c[] = { -1, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12,
                                         16, 32, 64, 128, 256 };
        static const long long b4cu[] = { 32768, 65536, 2, 3, 4, 5, 6, 7, 8,
                                          10, 12, 16, 32, 64, 128, 256 };
        for (k = 0; bi_tab[k]; k++)
            for (j = 0; j < 16; j++) {
                long long c = k >= XT_BLTUI ? b4cu[j] : b4c[j];
                int d = (j * 17 + k * 3) % 255 - 124;
                fprintf(f, "%s a%d, %lld, .%+d@%s\ta%d, %lld, {+%d}\n",
                        bi_tab[k], (j + k) % 16, c, d, bi_tab[k],
                        (j + k) % 16, c, d);
            }
    }
    for (j = 0; j < 32; j += 3) {
        fprintf(f, "bbci a%d, %d, .+%d@bbci\ta%d, %d, {+%d}\n", j % 16, j,
                j * 4, j % 16, j, j * 4);
        fprintf(f, "bbsi a%d, %d, .-%d@bbsi\ta%d, %d, {+-%d}\n",
                (j + 5) % 16, j, j * 4, (j + 5) % 16, j, j * 4);
    }
    fprintf(f, "bbci a3, 31, .+131@bbci\ta3, 31, {+131}\n");
    fprintf(f, "bbci.l a6, 9, .+20@bbci\ta6, 9, {+20}\n");
    fprintf(f, "bbsi.l a7, 30, .-8@bbsi\ta7, 30, {+-8}\n");
    for (k = 0; loop_tab[k]; k++) {
        fprintf(f, "%s a%d, .+4@%s\ta%d, {+4}\n", loop_tab[k], k + 2,
                loop_tab[k], k + 2);
        fprintf(f, "%s a%d, .+259@%s\ta%d, {+259}\n", loop_tab[k], 14 - k,
                loop_tab[k], 14 - k);
    }
    fprintf(f, "j .+131075@j\t{+131075}\nj .-131068@j\t{+-131068}\n");
    fprintf(f, "j .@j\t{+0}\nj .+4@j\t{+4}\n");
    {
        static const long cd[] = { 0, 4, -4, 524284, -524288, 1024 };
        for (j = 0; j < 6; j++)
            for (k = 0; k < 4; k++)
                fprintf(f, "call%d {C%+ld}@call%d\t{C%+ld}\n", 4 * k, cd[j],
                        4 * k, cd[j]);
    }
    fprintf(f, "entry a1, 32@entry\ta1, 32\nentry sp, 0@entry\ta1, 0\n");
    fprintf(f, "entry a1, 32760@entry\ta1, 32760\n");
    fprintf(f, "entry a5, 48@entry\ta5, 48\n");
    for (j = -8; j < 8; j += 3)
        fprintf(f, "rotw %d@rotw\t%d\n", j, j);
    fprintf(f, "rotw 7@rotw\t7\n");
    for (k = 0; op0_tab[k].name; k++)
        fprintf(f, "%s@%s\n", op0_tab[k].name, op0_tab[k].name);
    fprintf(f, "_nop@nop\n_l32i a2, a3, 8@l32i\ta2, a3, 8\n");
    fprintf(f, "break 0, 0@break\t0, 0\nbreak 15, 1@break\t15, 1\n");
    fprintf(f, "break 1, 15@break\t1, 15\n");
    for (j = 1; j <= 15; j += 3)
        fprintf(f, "rfi %d@rfi\t%d\n", j, j);
    for (j = 0; j <= 15; j += 5)
        fprintf(f, "waiti %d@waiti\t%d\nrsil a%d, %d@rsil\ta%d, %d\n", j, j,
                15 - j, j, 15 - j, j);
    for (const struct sr_ent *e = sr_tab; e->name; e++) {
        const char *q = e->q ? "" : "!";
        int t2 = (e->num * 7 + 2) % 16;
        if (e->acc & R) {
            fprintf(f, "rsr a%d, %s@%srsr.%s\ta%d\n", t2, e->name, q,
                    e->name, t2);
            fprintf(f, "rsr.%s a%d@%srsr.%s\ta%d\n", e->name, (t2 + 1) % 16,
                    q, e->name, (t2 + 1) % 16);
        }
        if (e->acc & W) {
            /* a wsr of INTERRUPT is INTSET, as QEMU names it */
            const char *as = e->num == 226 ? "intset" : e->name;
            fprintf(f, "wsr a%d, %s@%swsr.%s\ta%d\n", t2, e->name, q, as, t2);
            fprintf(f, "wsr.%s a%d@%swsr.%s\ta%d\n", as, (t2 + 3) % 16,
                    q, as, (t2 + 3) % 16);
        }
        if (e->acc & X)
            fprintf(f, "xsr a%d, %s@%sxsr.%s\ta%d\n", t2, e->name, q,
                    e->name, t2);
    }
    fprintf(f, "rsr a3, 230@rsr.ps\ta3\nwsr a4, 228@wsr.intenable\ta4\n");
    fprintf(f, "xsr a0, 209@xsr.excsave1\ta0\nrsr a2, PS@rsr.ps\ta2\n");
    fprintf(f, "rur a2, threadptr@!\nwur a3, threadptr@!\n");
    fprintf(f, "rur.threadptr a4@!\nwur.threadptr a5@!\n");
}
