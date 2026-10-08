/* The RX assembler. See asm.h. All this file adds to emit.c is a parser:
 * every range is checked HERE, before the encoder is called, so a
 * template's mistake is a diagnostic and not an internal error. */
#include "asm.h"

#include "emit.h"
#include "../asmexpr.h"
#include "../../elf/elf.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The relaxation level gas.c set for the statement being encoded (-1: the
 * first pass's optimistic guess, the shortest form whatever it reaches),
 * and the form the statement's branch took. */
static int g_level;
static int g_took;
/* The statement being encoded is rxasm_symform's: its field is a
 * relocation's, and holds zero. */
static int g_raw;

void rxasm_set_level(int level)
{
    g_level = level;
}

int rxasm_took_level(void)
{
    return g_took;
}

#define FAIL(...)  do { snprintf(err, (size_t)errlen, __VA_ARGS__); \
                        return -1; } while (0)

/* ---- names ----------------------------------------------------------------- */

int rxasm_gpr(const char *name, int len)
{
    int v;
    if (len < 2 || len > 3 || (name[0] != 'r' && name[0] != 'R') ||
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

int rxasm_is_reg(const char *name, int len)
{
    return rxasm_gpr(name, len);
}

static int name_is(const char *a, int alen, const char *b)
{
    if ((int)strlen(b) != alen)
        return 0;
    for (int i = 0; i < alen; i++)
        if (tolower((unsigned char)a[i]) != b[i])
            return 0;
    return 1;
}

struct nament { const char *name; int v; };

/* The control registers mvtc/mvfc/pushc/popc name, by their numbers. */
static const struct nament cr_tab[] = {
    { "psw", 0 }, { "pc", 1 }, { "usp", 2 }, { "fpsw", 3 }, { "bpsw", 8 },
    { "bpc", 9 }, { "isp", 10 }, { "fintv", 11 }, { "intb", 12 },
    { NULL, 0 }
};

/* The PSW flags setpsw/clrpsw name. */
static const struct nament flag_tab[] = {
    { "c", 0 }, { "z", 1 }, { "s", 2 }, { "o", 3 }, { "i", 8 }, { "u", 9 },
    { NULL, 0 }
};

/* The condition names of bCND, bmCND and scCND, GNU's synonyms included,
 * in emit.h's numbering. */
static const struct nament cond_tab[] = {
    { "eq", RX_EQ }, { "z", RX_EQ }, { "ne", RX_NE }, { "nz", RX_NE },
    { "geu", RX_GEU }, { "c", RX_GEU }, { "ltu", RX_LTU }, { "nc", RX_LTU },
    { "gtu", RX_GTU }, { "leu", RX_LEU }, { "pz", RX_PZ }, { "n", RX_N },
    { "ge", RX_GE }, { "lt", RX_LT }, { "gt", RX_GT }, { "le", RX_LE },
    { "o", RX_O }, { "no", RX_NO },
    { NULL, 0 }
};

static int lookup(const struct nament *t, const char *s, int len)
{
    for (; t->name; t++)
        if (name_is(s, len, t->name))
            return t->v;
    return -1;
}

/* ---- operands ----------------------------------------------------------------- */

enum { O_REG, O_IMM, O_MEM, O_IDX, O_PINC, O_PDEC, O_RANGE, O_TGT, O_WORD };

struct opd {
    int k;
    int r, r2;           /* REG; MEM/PINC/PDEC the base; IDX ri, rb;
                          * RANGE r-r2 */
    long long v;         /* IMM; MEM the displacement; TGT the distance */
    int hasd;            /* MEM: a displacement was written */
    int msz, msign;      /* MEM: a memex size written (.b .w .l .ub .uw),
                          * or msz -1 */
    const char *s;
    int len;
};

static int trim(const char **s, int len)
{
    while (len > 0 && isspace((unsigned char)**s)) {
        (*s)++;
        len--;
    }
    while (len > 0 && isspace((unsigned char)(*s)[len - 1]))
        len--;
    return len;
}

/* One operand's text. Returns 0, or -1 with the reason in err. */
static int parse_opd(const char *s, int len, struct opd *o, const char *mn,
                     char *err, int errlen)
{
    memset(o, 0, sizeof *o);
    len = trim(&s, len);
    o->s = s;
    o->len = len;
    o->msz = -1;
    if (len == 0)
        FAIL("%s: an empty operand", mn);
    if (s[0] == '#') {
        o->k = O_IMM;
        if (!asm_const_expr(s + 1, len - 1, &o->v)) {
            if (isalpha((unsigned char)s[1]) || s[1] == '_')
                FAIL("%s: \"%.*s\" is a symbol, and inline asm cannot take "
                     "one: a template carries no relocation (load it with a "
                     "\"i\" operand, or write it in a .S file)", mn, len, s);
            FAIL("%s: \"%.*s\" is not a constant", mn, len, s);
        }
        return 0;
    }
    if (s[0] == '.' && (len == 1 || s[1] == '+' || s[1] == '-')) {
        o->k = O_TGT;
        if (len == 1)
            return 0;
        if (!asm_const_expr(s + 1, len - 1, &o->v))
            FAIL("%s: \"%.*s\" is not a branch target (a label, or .+N bytes "
                 "from the instruction)", mn, len, s);
        return 0;
    }
    {
        const char *lb = memchr(s, '[', (size_t)len);
        if (lb) {
            const char *rb = memchr(lb, ']', (size_t)(len - (lb - s)));
            const char *in;
            int inlen, dl = (int)(lb - s);
            if (!rb)
                FAIL("%s: \"%.*s\" has no closing ]", mn, len, s);
            in = lb + 1;
            inlen = trim(&in, (int)(rb - lb - 1));
            /* the suffix: a memex operand's size */
            {
                const char *sf = rb + 1;
                int sl = (int)(s + len - sf);
                if (sl > 0) {
                    if (name_is(sf, sl, ".b")) { o->msz = RX_B; o->msign = 1; }
                    else if (name_is(sf, sl, ".w")) { o->msz = RX_W; o->msign = 1; }
                    else if (name_is(sf, sl, ".l")) { o->msz = RX_L; o->msign = 1; }
                    else if (name_is(sf, sl, ".ub")) { o->msz = RX_B; o->msign = 0; }
                    else if (name_is(sf, sl, ".uw")) { o->msz = RX_W; o->msign = 0; }
                    else
                        FAIL("%s: \"%.*s\" is not a memory operand's size (.b, "
                             ".w, .l, .ub or .uw)", mn, sl, sf);
                }
            }
            /* [ri, rb] */
            {
                const char *cm = memchr(in, ',', (size_t)inlen);
                if (cm) {
                    const char *a = in, *b = cm + 1;
                    int al = trim(&a, (int)(cm - in));
                    int bl = trim(&b, (int)(in + inlen - b));
                    o->k = O_IDX;
                    o->r = rxasm_gpr(a, al);
                    o->r2 = rxasm_gpr(b, bl);
                    if (o->r < 0 || o->r2 < 0 || dl > 0 || o->msz >= 0)
                        FAIL("%s: \"%.*s\" is not an indexed operand "
                             "([ri, rb])", mn, len, s);
                    return 0;
                }
            }
            if (inlen > 1 && in[inlen - 1] == '+') {
                o->k = O_PINC;
                o->r = rxasm_gpr(in, trim(&in, inlen - 1));
            } else if (inlen > 1 && in[0] == '-') {
                const char *q = in + 1;
                o->k = O_PDEC;
                o->r = rxasm_gpr(q, trim(&q, inlen - 1));
            } else {
                o->k = O_MEM;
                o->r = rxasm_gpr(in, inlen);
            }
            if (o->r < 0)
                FAIL("%s: \"%.*s\" is not a register (r0-r15) in \"%.*s\"",
                     mn, inlen, in, len, s);
            if (o->k != O_MEM && (dl > 0 || o->msz >= 0))
                FAIL("%s: \"%.*s\": an auto-increment operand takes no "
                     "displacement or size", mn, len, s);
            if (dl > 0) {
                const char *d = s;
                int dlen = trim(&d, dl);
                o->hasd = 1;
                if (!asm_const_expr(d, dlen, &o->v))
                    FAIL("%s: \"%.*s\" is not a displacement", mn, dlen, d);
            }
            return 0;
        }
    }
    {
        const char *m = memchr(s, '-', (size_t)len);
        if (m && m > s) {
            const char *a = s, *b = m + 1;
            int al = trim(&a, (int)(m - s));
            int bl = trim(&b, (int)(s + len - b));
            if (rxasm_gpr(a, al) >= 0 && rxasm_gpr(b, bl) >= 0) {
                o->k = O_RANGE;
                o->r = rxasm_gpr(a, al);
                o->r2 = rxasm_gpr(b, bl);
                return 0;
            }
        }
    }
    o->r = rxasm_gpr(s, len);
    if (o->r >= 0) {
        o->k = O_REG;
        return 0;
    }
    o->k = O_WORD;
    return 0;
}

/* ---- statements ----------------------------------------------------------- */

#define MAXOPD 4

/* The mnemonic (lower case, its size suffix split off) and the operands,
 * split at the commas outside brackets. */
struct stmt {
    char mn[24];         /* without the suffix */
    char full[28];       /* as written, lower case */
    char sfx;            /* 's' 'b' 'w' 'l' 'a', or 0 */
    struct opd o[MAXOPD];
    int n;
};

static int split_stmt(const char *s, int len, struct stmt *st, char *err,
                      int errlen)
{
    int i = 0, ml = 0;
    memset(st, 0, sizeof *st);
    while (i < len && !isspace((unsigned char)s[i]))
        i++;
    if (i >= (int)sizeof st->full)
        FAIL("asm instruction \"%.*s\" is not in the RX vocabulary", i, s);
    for (int k = 0; k < i; k++)
        st->full[k] = (char)tolower((unsigned char)s[k]);
    st->full[i] = 0;
    memcpy(st->mn, st->full, (size_t)i + 1);
    {
        char *dot = strrchr(st->mn, '.');
        if (dot && dot != st->mn) {
            if (strlen(dot) != 2 || !strchr("sbwla", dot[1]))
                FAIL("asm instruction \"%s\" is not in the RX vocabulary: "
                     "\"%s\" is not a size (.s .b .w .l .a)", st->full, dot);
            st->sfx = dot[1];
            *dot = 0;
        }
    }
    ml = i;
    while (ml < len && st->n < MAXOPD) {
        int depth = 0, b;
        while (ml < len && isspace((unsigned char)s[ml]))
            ml++;
        if (ml >= len)
            break;
        b = ml;
        while (ml < len && (depth > 0 || s[ml] != ',')) {
            if (s[ml] == '[') depth++;
            else if (s[ml] == ']') depth--;
            ml++;
        }
        if (parse_opd(s + b, ml - b, &st->o[st->n], st->full, err, errlen))
            return -1;
        st->n++;
        if (ml < len)
            ml++;                         /* the comma */
    }
    if (ml < len)
        FAIL("%s: too many operands", st->full);
    return 0;
}

static const char *size_name(int sz)
{
    return sz == RX_B ? "b" : sz == RX_W ? "w" : "l";
}

static int sfx_size(char c)
{
    return c == 'b' ? RX_B : c == 'w' ? RX_W : c == 'l' ? RX_L : -1;
}

/* A memory operand's displacement: a multiple of the access, within the
 * 16-bit unsigned field (in units); `lim` units at most. */
static int need_dsp(const struct stmt *st, const struct opd *o, int size,
                    long lim, char *err, int errlen)
{
    int scale = size == RX_B ? 1 : size == RX_W ? 2 : 4;
    if (o->v < 0 || o->v % scale || o->v / scale > lim)
        FAIL("%s: displacement %lld is out of range for a .%s access (0 to "
             "%ld, a multiple of %d)%s", st->full, o->v, size_name(size),
             lim * scale, scale,
             lim < 65535 ? ": QEMU reads this field signed, so EmbCC keeps "
                           "below its sign bit" : "");
    return 0;
}

static int need_imm_range(const struct stmt *st, long long v, long long lo,
                          long long hi, char *err, int errlen)
{
    if (v < lo || v > hi)
        FAIL("%s: #%lld does not fit (%lld to %lld)", st->full, v, lo, hi);
    return 0;
}

/* a 32-bit immediate, signed or unsigned */
#define IMM32_LO (-2147483647LL - 1)
#define IMM32_HI 4294967295LL

static int is(const struct stmt *st, const char *m)
{
    return strcmp(st->mn, m) == 0;
}

/* ---- the ALU ----------------------------------------------------------------- */

#define F_RR  1          /* rs, rd */
#define F_RI  2          /* #imm, rd */
#define F_RM  4          /* memex src, rd */
#define F_RRR 8          /* rs, rs2, rd */
#define F_R1  16         /* rd alone (neg/not/abs) */
#define F_IRR 32         /* #imm, rs, rd (add) */

struct alu_ent { const char *name; int op; int forms; };
static const struct alu_ent alu_tab[] = {
    { "add", RX_ADD, F_RR | F_RI | F_RM | F_RRR | F_IRR },
    { "sub", RX_SUB, F_RR | F_RI | F_RM | F_RRR },
    { "cmp", RX_CMP, F_RR | F_RI | F_RM },
    { "and", RX_AND, F_RR | F_RI | F_RM | F_RRR },
    { "or", RX_OR, F_RR | F_RI | F_RM | F_RRR },
    { "mul", RX_MUL, F_RR | F_RI | F_RM | F_RRR },
    { "xor", RX_XOR, F_RR | F_RI | F_RM },
    { "tst", RX_TST, F_RR | F_RI | F_RM },
    { "max", RX_MAX, F_RR | F_RI | F_RM },
    { "min", RX_MIN, F_RR | F_RI | F_RM },
    { "div", RX_DIV, F_RR | F_RI | F_RM },
    { "divu", RX_DIVU, F_RR | F_RI | F_RM },
    { "emul", RX_EMUL, F_RR | F_RI | F_RM },
    { "emulu", RX_EMULU, F_RR | F_RI | F_RM },
    { "adc", RX_ADC, F_RR | F_RI | F_RM },
    { "sbb", RX_SBB, F_RR | F_RI | F_RM },
    { "neg", RX_NEG, F_RR | F_R1 },
    { "not", RX_NOT, F_RR | F_R1 },
    { "abs", RX_ABS, F_RR | F_R1 },
    { "xchg", RX_XCHG, F_RR | F_RM },
    { "stz", RX_STZ, F_RI },
    { "stnz", RX_STNZ, F_RI },
    { NULL, 0, 0 }
};

static int do_alu(const struct alu_ent *e, struct stmt *st, struct code *out,
                  char *err, int errlen)
{
    const struct opd *a = &st->o[0], *b = &st->o[1];
    int emul = e->op == RX_EMUL || e->op == RX_EMULU;
    if (st->sfx)
        FAIL("%s takes no size: a memory source says its own (%s 4[r1].w, "
             "r2)", st->full, st->mn);
    if (st->n == 1 && a->k == O_REG && (e->forms & F_R1)) {
        rx_r(out, e->op, a->r);
        return 0;
    }
    if (st->n == 2 && b->k == O_REG) {
        if (emul && b->r == 15)
            FAIL("%s: the destination is the pair rd+1:rd, so r15 cannot be "
                 "it", st->full);
        if (a->k == O_REG && (e->forms & F_RR)) {
            rx_rr(out, e->op, a->r, b->r);
            return 0;
        }
        if (a->k == O_IMM && (e->forms & F_RI)) {
            /* sub's is negated and sbb's inverted, so theirs are signed
             * 32-bit values (GNU as's li is the 64-bit result's) */
            if (need_imm_range(st, a->v, IMM32_LO,
                               e->op == RX_SUB || e->op == RX_SBB
                               ? 2147483647LL : IMM32_HI,
                               err, errlen))
                return -1;
            if (e->op == RX_SBB)          /* sbb #x is adc #~x, as GNU as */
                rx_ri(out, RX_ADC, (long)~a->v, b->r);
            else if (e->op == RX_SUB && (a->v < 0 || a->v > 15))
                /* GNU as: add #-x, rd, rd, the li form whatever -x is */
                rx_add3_li(out, (long)(0 - (unsigned long)a->v), b->r, b->r);
            else
                rx_ri(out, e->op, (long)a->v, b->r);
            return 0;
        }
        if (a->k == O_MEM && (e->forms & F_RM)) {
            int sz = a->msz >= 0 ? a->msz : RX_L;
            int sg = a->msz >= 0 ? a->msign : 1;
            if ((e->op == RX_ADC || e->op == RX_SBB) && sz != RX_L)
                FAIL("%s: a memory source is .l only", st->full);
            if (need_dsp(st, a, sz, 65535, err, errlen))
                return -1;
            rx_rm(out, e->op, sz, sg, (long)a->v, a->r, b->r);
            return 0;
        }
    }
    if (st->n == 3 && st->o[2].k == O_REG && b->k == O_REG) {
        if (a->k == O_REG && (e->forms & F_RRR)) {
            rx_rrr(out, e->op, a->r, b->r, st->o[2].r);
            return 0;
        }
        if (a->k == O_IMM && (e->forms & F_IRR)) {
            if (need_imm_range(st, a->v, IMM32_LO, IMM32_HI, err, errlen))
                return -1;
            rx_add3(out, (long)a->v, b->r, st->o[2].r);
            return 0;
        }
    }
    FAIL("%s: these operands are not one of its forms (%s%s%s%s%s%s)",
         st->full,
         e->forms & F_RR ? " rs, rd;" : "", e->forms & F_RI ? " #imm, rd;" : "",
         e->forms & F_RM ? " dsp[rs].size, rd;" : "",
         e->forms & F_RRR ? " rs, rs2, rd;" : "",
         e->forms & F_IRR ? " #imm, rs, rd;" : "", e->forms & F_R1 ? " rd;" : "");
}

/* ---- mov, movu, push ---------------------------------------------------------- */

static int do_mov(struct stmt *st, struct code *out, int unsign, char *err,
                  int errlen)
{
    const struct opd *a = &st->o[0], *b = &st->o[1];
    int sz = st->sfx ? sfx_size(st->sfx) : RX_L;
    if (sz < 0 || (unsign && (sz == RX_L || !st->sfx)))
        FAIL("%s: the size is %s", st->full, unsign ? ".b or .w" : ".b, .w or .l");
    if (st->n != 2)
        FAIL("%s takes two operands", st->full);
    if ((a->k == O_MEM && a->msz >= 0) || (b->k == O_MEM && b->msz >= 0))
        FAIL("%s: the size is the mnemonic's (%s.%s); a memory operand takes "
             "none", st->full, st->mn, size_name(sz));
    if (b->k == O_REG) {
        int rd = b->r;
        switch (a->k) {
        case O_REG:
            if (sz == RX_L)
                rx_rr(out, RX_MOV, a->r, rd);
            else
                rx_ext(out, sz, !unsign, a->r, rd);
            return 0;
        case O_IMM:
            if (unsign || sz != RX_L)
                FAIL("%s: an immediate goes into a register as mov.l", st->full);
            if (need_imm_range(st, a->v, IMM32_LO, IMM32_HI, err, errlen))
                return -1;
            if (g_raw)          /* an address: the four-byte field, zero */
                rx_mov_abs(out, rd, 0);
            else
                rx_ri(out, RX_MOV, (long)a->v, rd);
            return 0;
        case O_MEM:
            if (need_dsp(st, a, sz, unsign ? 32767 : 65535, err, errlen))
                return -1;
            rx_load_x(out, sz, !unsign, (long)a->v, a->r, rd, a->hasd);
            return 0;
        case O_IDX:
            rx_load_idx(out, sz, !unsign, a->r, a->r2, rd);
            return 0;
        case O_PINC: case O_PDEC:
            rx_mov_pi(out, 1, !unsign, a->k == O_PDEC, sz, a->r, rd);
            return 0;
        default:
            break;
        }
    } else if (!unsign && a->k == O_REG) {
        switch (b->k) {
        case O_MEM:
            if (need_dsp(st, b, sz, 65535, err, errlen))
                return -1;
            rx_store_x(out, sz, a->r, (long)b->v, b->r, b->hasd);
            return 0;
        case O_IDX:
            rx_store_idx(out, sz, a->r, b->r, b->r2);
            return 0;
        case O_PINC: case O_PDEC:
            rx_mov_pi(out, 0, 1, b->k == O_PDEC, sz, b->r, a->r);
            return 0;
        default:
            break;
        }
    } else if (!unsign && a->k == O_IMM && b->k == O_MEM) {
        long long lo = sz == RX_B ? -128 : sz == RX_W ? -32768 : IMM32_LO;
        long long hi = sz == RX_B ? 255 : sz == RX_W ? 65535 : IMM32_HI;
        if (need_imm_range(st, a->v, lo, hi, err, errlen) ||
            need_dsp(st, b, sz, 32767, err, errlen))
            return -1;
        rx_store_imm_x(out, sz, (long)a->v, (long)b->v, b->r, b->hasd, 1);
        return 0;
    }
    if (a->k == O_MEM && b->k == O_MEM)
        FAIL("%s: a memory-to-memory move is not in EmbCC's RX vocabulary: "
             "go through a register", st->full);
    FAIL("%s: these operands are not one of its forms", st->full);
}

/* ---- transfers ---------------------------------------------------------------- *
 *
 * Each family's forms, shortest first; a branch written without a size
 * takes the first that reaches (at least the level gas.c settled on). The
 * last ones of the conditional families are the inverse condition over a
 * bra, which is how GNU as relaxes a conditional branch that does not
 * reach: `bgt far` is `ble.b .+5; bra.w far`. */
enum { BF_BRA, BF_BSR, BF_BEQNE, BF_BCND };
enum { K_S, K_B, K_W, K_A, K_INVS_A, K_INVB_W, K_INVB_A };
static const int br_ladder[4][4] = {
    { K_S, K_B, K_W, K_A }, { K_W, K_A, -1, -1 },
    { K_S, K_B, K_W, K_INVS_A }, { K_B, K_INVB_W, K_INVB_A, -1 }
};

static int br_reaches(int kind, long long d)
{
    switch (kind) {
    case K_S: return d >= 3 && d <= 10;
    case K_B: return d >= -128 && d <= 127;
    case K_W: return d >= -32768 && d <= 32767;
    case K_A: return d >= -8388608LL && d <= 8388607LL;
    case K_INVS_A: return br_reaches(K_A, d - 1);
    /* GNU as takes the bra.w when the condition's own distance fits 16
     * bits, and its bra.w is 2 bytes on: -32768 and -32767 come out
     * wrapped there (GNU's bug), so they take the bra.a here */
    case K_INVB_W: return br_reaches(K_W, d) && br_reaches(K_W, d - 2);
    default: return br_reaches(K_A, d - 2);
    }
}

static void br_emit(struct code *out, int fam, int cond, int kind, long long d)
{
    static const int ek[] = { RX_BR_S, RX_BR_B, RX_BR_W, RX_BR_A };
    /* the optimistic pass only needs the size */
    if (!br_reaches(kind, d))
        d = kind == K_S ? 3 : kind == K_INVS_A ? 1 : kind >= K_INVB_W ? 2 : 0;
    if (kind == K_INVS_A) {
        rx_branch_d(out, RX_BR_S, rx_cond_invert(cond), 5);
        rx_branch_d(out, RX_BR_A, RX_ALWAYS, (long)d - 1);
        return;
    }
    if (kind == K_INVB_W || kind == K_INVB_A) {
        rx_branch_d(out, RX_BR_B, rx_cond_invert(cond),
                    kind == K_INVB_W ? 5 : 6);
        rx_branch_d(out, kind == K_INVB_W ? RX_BR_W : RX_BR_A, RX_ALWAYS,
                    (long)d - 2);
        return;
    }
    if (fam == BF_BSR) {
        if (kind == K_W)
            rx_bsr_w_d(out, (long)d);
        else
            rx_bsr_a_d(out, (long)d);
        return;
    }
    rx_branch_d(out, ek[kind], fam == BF_BRA ? RX_ALWAYS : cond, (long)d);
}

/* `raw`: a statement rxasm_symform wrote, its field a relocation's. */
static int do_branch(struct stmt *st, struct code *out, int fam, int cond,
                     int raw, char *err, int errlen)
{
    const struct opd *a = &st->o[0];
    int kind = -1;
    long long d = a->v;
    if (st->n != 1)
        FAIL("%s takes one operand", st->full);
    if (a->k == O_REG && (fam == BF_BRA || fam == BF_BSR) &&
        (!st->sfx || st->sfx == 'l')) {
        if (fam == BF_BRA)
            rx_bra_l(out, a->r);
        else
            rx_bsr_l(out, a->r);
        return 0;
    }
    if (a->k != O_TGT) {
        if (a->k == O_WORD && (isalpha((unsigned char)a->s[0]) ||
                               a->s[0] == '_'))
            FAIL("%s: \"%.*s\" is a symbol, and inline asm cannot reach one: "
                 "a template carries no relocation (jsr through a register, "
                 "or write it in a .S file)", st->full, a->len, a->s);
        FAIL("%s: \"%.*s\" is not a branch target (a label, or .+N bytes "
             "from the instruction)", st->full, a->len, a->s);
    }
    if (st->sfx) {
        switch (st->sfx) {
        case 's': kind = fam == BF_BSR || fam == BF_BCND ? -1 : K_S; break;
        case 'b': kind = fam == BF_BSR ? -1 : K_B; break;
        case 'w': kind = fam == BF_BCND ? -1 : K_W; break;
        case 'a': kind = fam == BF_BRA || fam == BF_BSR ? K_A : -1; break;
        default: kind = -1; break;
        }
        if (kind < 0)
            FAIL("%s is not a form RX has (bra .s/.b/.w/.a/.l, bsr .w/.a/.l, "
                 "beq/bne .s/.b/.w, any other condition .b)", st->full);
        if (!raw && g_level >= 0 && !br_reaches(kind, d))
            FAIL("%s: the target is %lld bytes away, out of reach of the "
                 ".%c form%s", st->full, d, st->sfx,
                 kind == K_S ? " (3 to 10)" : "");
        br_emit(out, fam, cond, kind, raw ? 0 : d);
        return 0;
    }
    {
        int lo = g_level < 0 ? 0 : g_level, k;
        if (g_level < 0) {
            k = 0;
        } else {
            for (k = lo; k < 4 && br_ladder[fam][k] >= 0; k++)
                if (br_reaches(br_ladder[fam][k], d))
                    break;
            if (k >= 4 || br_ladder[fam][k] < 0)
                FAIL("%s: the target is %lld bytes away, out of reach of "
                     "every form", st->full, d);
        }
        if (k > g_took)
            g_took = k;
        br_emit(out, fam, cond, br_ladder[fam][k], d);
    }
    return 0;
}

/* ---- the rest ------------------------------------------------------------------- */

static const struct { const char *name; void (*fn)(struct code *); } op0_tab[] = {
    { "nop", rx_nop }, { "rts", rx_rts }, { "rte", rx_rte }, { "rtfi", rx_rtfi },
    { "wait", rx_wait }, { "brk", rx_brk }, { "satr", rx_satr },
    { NULL, NULL }
};

static const struct nament str_tab[] = {
    { "suntil", RX_SUNTIL }, { "swhile", RX_SWHILE }, { "sstr", RX_SSTR },
    { "rmpa", RX_RMPA }, { "scmpu", RX_SCMPU }, { "smovu", RX_SMOVU },
    { "smovb", RX_SMOVB }, { "smovf", RX_SMOVF },
    { NULL, 0 }
};

static const struct nament bit_tab[] = {
    { "bset", RX_BSET }, { "bclr", RX_BCLR }, { "btst", RX_BTST },
    { "bnot", RX_BNOT },
    { NULL, 0 }
};

static const struct nament shift_tab[] = {
    { "shll", RX_SHLL }, { "shlr", RX_SHLR }, { "shar", RX_SHAR },
    { "rotl", RX_ROTL }, { "rotr", RX_ROTR },
    { NULL, 0 }
};

/* The FPU's and RXv2's mnemonics: refused by name, not as unknown. */
static const char *const fpu_tab[] = {
    "fadd", "fsub", "fmul", "fdiv", "fcmp", "ftoi", "itof", "round",
    "fsqrt", "ftou", "utof", "movco", "movli", "emaca", "emsba", "emula",
    "maclh", "msbhi", "msblh", "msblo", "mvfacgu", "mvtacgu", "racl", "rdacl",
    "rdacw", "bfmov", "bfmovz", "rstr", "save", "dabs", "dadd", NULL
};

static int need_n(const struct stmt *st, int n, char *err, int errlen)
{
    if (st->n != n)
        FAIL("%s takes %d operand%s", st->full, n, n == 1 ? "" : "s");
    return 0;
}

static int need_reg(const struct stmt *st, int k, char *err, int errlen)
{
    if (st->o[k].k != O_REG)
        FAIL("%s: \"%.*s\" is not a register (r0-r15)", st->full,
             st->o[k].len, st->o[k].s);
    return 0;
}

static int need_imm(const struct stmt *st, int k, long long lo, long long hi,
                    char *err, int errlen)
{
    if (st->o[k].k != O_IMM)
        FAIL("%s: \"%.*s\" is not an immediate (#n)", st->full, st->o[k].len,
             st->o[k].s);
    return need_imm_range(st, st->o[k].v, lo, hi, err, errlen);
}

static int need_cr(const struct stmt *st, int k, int writing, char *err,
                   int errlen)
{
    int cr = lookup(cr_tab, st->o[k].s, st->o[k].len);
    if (st->o[k].k != O_WORD || cr < 0)
        FAIL("%s: \"%.*s\" is not a control register (psw, pc, usp, fpsw, "
             "bpsw, bpc, isp, fintv, intb)", st->full, st->o[k].len,
             st->o[k].s);
    if (writing && cr == 1)
        FAIL("%s: pc cannot be written (jmp through a register)", st->full);
    return cr;
}

/* A bit operation's memory operand: a byte, `dsp[rd].b`. */
static int need_bytemem(const struct stmt *st, const struct opd *o, char *err,
                        int errlen)
{
    if (o->k != O_MEM || o->msz != RX_B || !o->msign)
        FAIL("%s: \"%.*s\": a bit operation's memory operand is a byte, "
             "dsp[rN].b", st->full, o->len, o->s);
    return need_dsp(st, o, RX_B, 65535, err, errlen);
}

/* The condition a bCND/bmCND/scCND names after its prefix, or -1. */
static int cond_after(const char *mn, int skip)
{
    return lookup(cond_tab, mn + skip, (int)strlen(mn + skip));
}

static int stmt_body(const char *s, int len, struct code *out, char *err,
                     int errlen)
{
    struct stmt st;
    int raw = 0, c;
    while (len > 0 && isspace((unsigned char)*s)) {
        s++;
        len--;
    }
    while (len > 0 && isspace((unsigned char)s[len - 1]))
        len--;
    if (len > 0 && s[0] == '\003') {           /* rxasm_symform's */
        raw = 1;
        s++;
        len--;
    }
    g_raw = raw;
    if (len > 0 && s[0] == '\001')             /* a refusal it wrote */
        FAIL("%.*s", len - 1, s + 1);
    if (len == 0)
        return 0;
    if (split_stmt(s, len, &st, err, errlen))
        return -1;

    for (int k = 0; op0_tab[k].name; k++)
        if (is(&st, op0_tab[k].name)) {
            if (st.n || st.sfx)
                FAIL("%s takes no operands", st.full);
            op0_tab[k].fn(out);
            return 0;
        }
    for (const struct alu_ent *e = alu_tab; e->name; e++)
        if (is(&st, e->name))
            return do_alu(e, &st, out, err, errlen);
    if (is(&st, "mov"))
        return do_mov(&st, out, 0, err, errlen);
    if (is(&st, "movu"))
        return do_mov(&st, out, 1, err, errlen);

    /* shifts and rotates */
    c = lookup(shift_tab, st.mn, (int)strlen(st.mn));
    if (c >= 0) {
        int rot = c == RX_ROTL || c == RX_ROTR;
        if (st.sfx || st.n < 2 || st.n > 3 || need_reg(&st, st.n - 1, err, errlen))
            FAIL("%s: the forms are #n, rd; %srs, rd", st.full,
                 rot ? "" : "#n, rs, rd; ");
        if (st.o[0].k == O_REG && st.n == 2) {
            rx_rr(out, c, st.o[0].r, st.o[1].r);
            return 0;
        }
        if (need_imm(&st, 0, 0, 31, err, errlen))
            return -1;
        if (st.n == 3) {
            if (rot || need_reg(&st, 1, err, errlen))
                FAIL("%s: the forms are #n, rd; rs, rd", st.full);
            rx_shift_i_x(out, c, (int)st.o[0].v, st.o[1].r, st.o[2].r, 1);
        } else {
            rx_shift_i(out, c, (int)st.o[0].v, st.o[1].r, st.o[1].r);
        }
        return 0;
    }
    if (is(&st, "rolc") || is(&st, "rorc") || is(&st, "sat")) {
        if (st.sfx || need_n(&st, 1, err, errlen) || need_reg(&st, 0, err, errlen))
            return -1;
        if (is(&st, "rolc")) rx_rolc(out, st.o[0].r);
        else if (is(&st, "rorc")) rx_rorc(out, st.o[0].r);
        else rx_sat(out, st.o[0].r);
        return 0;
    }
    if (is(&st, "revl") || is(&st, "revw")) {
        if (st.sfx || need_n(&st, 2, err, errlen) || need_reg(&st, 0, err, errlen) ||
            need_reg(&st, 1, err, errlen))
            return -1;
        rx_rr(out, is(&st, "revl") ? RX_REVL : RX_REVW, st.o[0].r, st.o[1].r);
        return 0;
    }

    /* the stack */
    if (is(&st, "push")) {
        int sz = st.sfx ? sfx_size(st.sfx) : RX_L;
        if (sz < 0 || need_n(&st, 1, err, errlen))
            FAIL("%s: push.b/.w/.l rs, or dsp[rs]", st.full);
        if (st.o[0].k == O_REG) {
            rx_push_sz(out, sz, st.o[0].r);
            return 0;
        }
        if (st.o[0].k == O_MEM && st.o[0].msz < 0) {
            if (need_dsp(&st, &st.o[0], sz, 65535, err, errlen))
                return -1;
            rx_push_m(out, sz, (long)st.o[0].v, st.o[0].r);
            return 0;
        }
        FAIL("%s: push.b/.w/.l rs, or dsp[rs]", st.full);
    }
    if (is(&st, "pop")) {
        if (st.sfx || need_n(&st, 1, err, errlen) || need_reg(&st, 0, err, errlen))
            return -1;
        rx_pop(out, st.o[0].r);
        return 0;
    }
    if (is(&st, "pushm") || is(&st, "popm")) {
        const struct opd *o = &st.o[0];
        if (st.sfx || need_n(&st, 1, err, errlen))
            return -1;
        if (o->k != O_RANGE || o->r < 1 || o->r > o->r2)
            FAIL("%s: \"%.*s\" is not a register range r1-r15 (rN-rM, N <= "
                 "M, not r0)", st.full, o->len, o->s);
        if (o->r == o->r2) {               /* one register: push/pop */
            if (is(&st, "pushm")) rx_push(out, o->r);
            else rx_pop(out, o->r);
        } else if (is(&st, "pushm")) {
            rx_pushm(out, o->r, o->r2);
        } else {
            rx_popm(out, o->r, o->r2);
        }
        return 0;
    }
    if (is(&st, "pushc") || is(&st, "popc")) {
        int cr;
        if (st.sfx || need_n(&st, 1, err, errlen))
            return -1;
        cr = need_cr(&st, 0, is(&st, "popc"), err, errlen);
        if (cr < 0)
            return -1;
        if (is(&st, "pushc")) rx_pushc(out, cr);
        else rx_popc(out, cr);
        return 0;
    }

    /* transfers */
    if (is(&st, "bra"))
        return do_branch(&st, out, BF_BRA, RX_ALWAYS, raw, err, errlen);
    if (is(&st, "bsr"))
        return do_branch(&st, out, BF_BSR, RX_ALWAYS, raw, err, errlen);
    if (is(&st, "jmp") || is(&st, "jsr")) {
        if (st.sfx || need_n(&st, 1, err, errlen) || need_reg(&st, 0, err, errlen))
            return -1;
        if (is(&st, "jmp")) rx_jmp(out, st.o[0].r);
        else rx_jsr(out, st.o[0].r);
        return 0;
    }
    if (is(&st, "rtsd")) {
        if (st.sfx || st.n < 1 || st.n > 2 ||
            need_imm(&st, 0, 0, 1020, err, errlen))
            return -1;
        if (st.o[0].v % 4)
            FAIL("rtsd: #%lld is not a multiple of 4", st.o[0].v);
        if (st.n == 1) {
            rx_rtsd(out, (long)st.o[0].v);
            return 0;
        }
        if (st.o[1].k != O_RANGE || st.o[1].r < 1 || st.o[1].r > st.o[1].r2)
            FAIL("rtsd: \"%.*s\" is not a register range r1-r15",
                 st.o[1].len, st.o[1].s);
        if (st.o[0].v < 4LL * (st.o[1].r2 - st.o[1].r + 1))
            FAIL("rtsd: #%lld is less than the %d registers popped", st.o[0].v,
                 st.o[1].r2 - st.o[1].r + 1);
        rx_rtsd_m(out, (long)st.o[0].v, st.o[1].r, st.o[1].r2);
        return 0;
    }
    if (is(&st, "int") || is(&st, "mvtipl")) {
        int mx = is(&st, "int") ? 255 : 15;
        if (st.sfx || need_n(&st, 1, err, errlen) ||
            need_imm(&st, 0, 0, mx, err, errlen))
            return -1;
        if (mx == 255) rx_int(out, (int)st.o[0].v);
        else rx_mvtipl(out, (int)st.o[0].v);
        return 0;
    }
    if (is(&st, "setpsw") || is(&st, "clrpsw")) {
        int f;
        if (st.sfx || need_n(&st, 1, err, errlen))
            return -1;
        f = lookup(flag_tab, st.o[0].s, st.o[0].len);
        if (st.o[0].k != O_WORD || f < 0)
            FAIL("%s: \"%.*s\" is not a PSW flag (c, z, s, o, i, u)", st.full,
                 st.o[0].len, st.o[0].s);
        if (is(&st, "setpsw")) rx_setpsw(out, f);
        else rx_clrpsw(out, f);
        return 0;
    }
    if (is(&st, "mvtc")) {
        int cr;
        if (st.sfx || need_n(&st, 2, err, errlen))
            return -1;
        cr = need_cr(&st, 1, 1, err, errlen);
        if (cr < 0)
            return -1;
        if (st.o[0].k == O_REG) {
            rx_mvtc(out, st.o[0].r, cr);
            return 0;
        }
        if (need_imm(&st, 0, IMM32_LO, IMM32_HI, err, errlen))
            return -1;
        rx_mvtc_i(out, (long)st.o[0].v, cr);
        return 0;
    }
    if (is(&st, "mvfc")) {
        int cr;
        if (st.sfx || need_n(&st, 2, err, errlen) || need_reg(&st, 1, err, errlen))
            return -1;
        cr = need_cr(&st, 0, 0, err, errlen);
        if (cr < 0)
            return -1;
        rx_mvfc(out, cr, st.o[1].r);
        return 0;
    }

    /* bits */
    c = lookup(bit_tab, st.mn, (int)strlen(st.mn));
    if (c >= 0) {
        const struct opd *a = &st.o[0], *b = &st.o[1];
        if (st.sfx || need_n(&st, 2, err, errlen))
            return -1;
        if (b->k == O_REG) {
            if (a->k == O_REG) {
                rx_bit_r(out, c, a->r, b->r);
                return 0;
            }
            if (need_imm(&st, 0, 0, 31, err, errlen))
                return -1;
            rx_bit_i(out, c, (int)a->v, b->r);
            return 0;
        }
        if (need_bytemem(&st, b, err, errlen))
            return -1;
        if (a->k == O_REG) {
            rx_bit_rm(out, c, a->r, (long)b->v, b->r);
            return 0;
        }
        if (need_imm(&st, 0, 0, 7, err, errlen))
            return -1;
        rx_bit_m(out, c, (int)a->v, (long)b->v, b->r);
        return 0;
    }
    c = strncmp(st.mn, "bm", 2) == 0 ? cond_after(st.mn, 2) : -1;
    if (c >= 0) {
        const struct opd *b = &st.o[1];
        if (st.sfx || need_n(&st, 2, err, errlen))
            return -1;
        if (b->k == O_REG) {
            if (need_imm(&st, 0, 0, 31, err, errlen))
                return -1;
            rx_bmcnd(out, c, (int)st.o[0].v, b->r);
            return 0;
        }
        if (need_bytemem(&st, b, err, errlen) ||
            need_imm(&st, 0, 0, 7, err, errlen))
            return -1;
        rx_bmcnd_m(out, c, (int)st.o[0].v, (long)b->v, b->r);
        return 0;
    }
    c = strncmp(st.mn, "sc", 2) == 0 ? cond_after(st.mn, 2) : -1;
    if (c >= 0) {
        if (st.sfx != 'l' || need_n(&st, 1, err, errlen) ||
            need_reg(&st, 0, err, errlen))
            FAIL("%s: the form here is sc%s.l rd (the memory forms are not in "
                 "EmbCC's RX vocabulary)", st.full, st.mn + 2);
        rx_scc(out, c, st.o[0].r);
        return 0;
    }
    c = st.mn[0] == 'b' ? cond_after(st.mn, 1) : -1;
    if (c >= 0)
        return do_branch(&st, out, c == RX_EQ || c == RX_NE ? BF_BEQNE : BF_BCND,
                         c, raw, err, errlen);

    /* strings, the accumulator */
    c = lookup(str_tab, st.mn, (int)strlen(st.mn));
    if (c >= 0) {
        int sz = st.sfx ? sfx_size(st.sfx) : RX_L;
        if (st.n || sz < 0 || (c > RX_RMPA && st.sfx))
            FAIL("%s takes no operands%s", st.full,
                 c <= RX_RMPA ? ", and a size .b, .w or .l" : " and no size");
        rx_string(out, c, sz);
        return 0;
    }
    if (!strcmp(st.mn, "mvfachi") || !strcmp(st.mn, "mvfaclo") ||
        !strcmp(st.mn, "mvfacmi") || !strcmp(st.mn, "mvtachi") ||
        !strcmp(st.mn, "mvtaclo")) {
        int w = st.mn[5] == 'h' ? 0 : st.mn[5] == 'l' ? 1 : 2;
        if (st.sfx || need_n(&st, 1, err, errlen) || need_reg(&st, 0, err, errlen))
            return -1;
        if (st.mn[2] == 'f') rx_mvfac(out, w, st.o[0].r);
        else rx_mvtac(out, w, st.o[0].r);
        return 0;
    }
    if (is(&st, "racw")) {
        if (st.sfx || need_n(&st, 1, err, errlen) ||
            need_imm(&st, 0, 1, 2, err, errlen))
            return -1;
        rx_racw(out, (int)st.o[0].v);
        return 0;
    }
    if (is(&st, "mulhi") || is(&st, "mullo") || is(&st, "machi") ||
        is(&st, "maclo")) {
        int op = is(&st, "mulhi") ? 0 : is(&st, "mullo") ? 1
               : is(&st, "machi") ? 4 : 5;
        if (st.sfx || need_n(&st, 2, err, errlen) || need_reg(&st, 0, err, errlen) ||
            need_reg(&st, 1, err, errlen))
            return -1;
        rx_mac(out, op, st.o[0].r, st.o[1].r);
        return 0;
    }
    for (int k = 0; fpu_tab[k]; k++)
        if (is(&st, fpu_tab[k]))
            FAIL("%s is an FPU or RXv2 instruction, which is not in EmbCC's "
                 "RX vocabulary (EmbCC's RX is RXv1, soft-float)", st.full);
    FAIL("asm instruction \"%s\" is not in the RX vocabulary", st.full);
}

int rxasm_encode(const char *text, struct code *out, char *err, int errlen)
{
    g_took = 0;
    return stmt_body(text, (int)strlen(text), out, err, errlen);
}

/* ---- a template: statements and numeric labels ------------------------------- */

#define MAXSTMT 256

struct stm {
    const char *s;
    int len;
    int labs[4], nlabs;     /* the numeric labels defined at its start */
};

/* Splits on newlines and `!`, drops `;` comments (and a `#` that starts a
 * line), and takes the numeric labels off the front of each statement. */
static int stmts(const char *text, struct stm *st, int max, char *err,
                 int errlen)
{
    int n = 0;
    const char *p = text;
    while (*p) {
        const char *e = p, *c;
        int len;
        while (*e && *e != '\n' && *e != '!')
            e++;
        len = (int)(e - p);
        c = memchr(p, ';', (size_t)len);
        if (c)
            len = (int)(c - p);
        while (len > 0 && isspace((unsigned char)*p)) {
            p++;
            len--;
        }
        if (len > 0 && *p == '#')
            len = 0;
        if (n >= max)
            FAIL("an asm template of more than %d statements", max);
        st[n].nlabs = 0;
        for (;;) {
            int k = 0;
            while (k < len && isdigit((unsigned char)p[k]))
                k++;
            if (k > 0 && k < len && p[k] == ':') {
                if (st[n].nlabs < 4)
                    st[n].labs[st[n].nlabs++] = atoi(p);
                p += k + 1;
                len -= k + 1;
                while (len > 0 && isspace((unsigned char)*p)) {
                    p++;
                    len--;
                }
                continue;
            }
            k = 0;
            while (k < len && (isalnum((unsigned char)p[k]) || p[k] == '_' ||
                               p[k] == '.' || p[k] == '$'))
                k++;
            if (k > 0 && k < len && p[k] == ':')
                FAIL("label \"%.*s\" in an RX asm template: labels in a "
                     "template are numeric (1:, then 1b or 1f); a named label "
                     "belongs in a .S file or a file-scope asm block", k, p);
            break;
        }
        st[n].s = p;
        st[n].len = len;
        n++;
        p = *e ? e + 1 : e;
    }
    return n;
}

/* Statement k with each `Nb`/`Nf` replaced by its distance `.+D`. */
static int resolve(const struct stm *st, int ns, int k, const long *off,
                   char *buf, int cap, char *err, int errlen)
{
    const char *s = st[k].s;
    int len = st[k].len, o = 0, i = 0;
    while (i < len) {
        int j = i;
        int word = i == 0 || !(isalnum((unsigned char)s[i - 1]) ||
                               s[i - 1] == '_' || s[i - 1] == '.');
        while (j < len && isdigit((unsigned char)s[j]))
            j++;
        if (word && j > i && j < len && (s[j] == 'b' || s[j] == 'f') &&
            (j + 1 == len || !(isalnum((unsigned char)s[j + 1]) ||
                               s[j + 1] == '_'))) {
            int lab = atoi(s + i), fwd = s[j] == 'f', at = -1;
            if (fwd) {
                for (int q = k + 1; q < ns && at < 0; q++)
                    for (int z = 0; z < st[q].nlabs; z++)
                        if (st[q].labs[z] == lab)
                            at = q;
            } else {
                for (int q = k; q >= 0 && at < 0; q--)
                    for (int z = 0; z < st[q].nlabs; z++)
                        if (st[q].labs[z] == lab)
                            at = q;
            }
            if (at < 0)
                FAIL("\"%.*s\" refers to no label %d %s it in the template",
                     j + 1 - i, s + i, lab, fwd ? "after" : "before");
            if (o >= cap - 24)
                FAIL("an asm statement too long to assemble");
            o += snprintf(buf + o, (size_t)(cap - o), ".%+ld", off[at] - off[k]);
            i = j + 1;
            continue;
        }
        if (j == i)
            j = i + 1;
        while (i < j) {
            if (o >= cap - 24)
                FAIL("an asm statement too long to assemble");
            buf[o++] = s[i++];
        }
    }
    buf[o] = 0;
    return o;
}

int rxasm_assemble(const char *text, struct code *out, char *err, int errlen)
{
    static struct stm st[MAXSTMT];
    static long off[MAXSTMT + 1], noff[MAXSTMT + 1];
    static signed char lev[MAXSTMT];
    int ns = stmts(text, st, MAXSTMT, err, errlen);
    if (ns < 0)
        return -1;
    for (int k = 0; k <= ns; k++)
        off[k] = 0;
    for (int k = 0; k < ns; k++)
        lev[k] = -1;
    /* The layout: each statement encoded with the distances the previous
     * pass gave, each branch's form only ever lengthened, until the
     * offsets and the forms stand still. Then the bytes are that pass's. */
    for (int pass = 0; pass < 40; pass++) {
        struct code c = { 0 };
        int moved = 0, rc = 0;
        char buf[512];
        noff[0] = 0;
        err[0] = 0;
        for (int k = 0; k < ns; k++) {
            int len = resolve(st, ns, k, off, buf, (int)sizeof buf, err, errlen);
            int before = c.len;
            if (len < 0) {
                free(c.p);
                return -1;
            }
            g_level = lev[k];
            g_took = 0;
            if (stmt_body(buf, len, &c, err, errlen) != 0) {
                rc = -1;
                break;
            }
            if (pass > 0 && g_took > lev[k]) {
                lev[k] = (signed char)g_took;
                moved = 1;
            }
            noff[k + 1] = noff[k] + (c.len - before);
        }
        if (pass == 0)
            for (int k = 0; k < ns; k++)
                lev[k] = 0;
        if (rc == 0)
            for (int k = 0; k <= ns; k++)
                if (noff[k] != off[k]) {
                    moved = 1;
                    off[k] = noff[k];
                }
        if (rc != 0 && pass > 0) {
            free(c.p);
            g_level = 0;
            return -1;
        }
        if (rc == 0 && pass > 0 && !moved) {
            for (int k = 0; k < c.len; k++)
                code_byte(out, c.p[k]);
            free(c.p);
            g_level = 0;
            return 0;
        }
        free(c.p);
    }
    g_level = 0;
    FAIL("the template's branches do not settle on a layout");
}

/* ---- for the file assembler (src/as/gas.c) -------------------------------- */

static const char *skip_sp(const char *p)
{
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

/* The mnemonic at p, lower case, without its size; its length. */
static int mnemonic(const char *p, char *mn, int cap, char *sfx)
{
    int n = 0, k = 0;
    while (isalnum((unsigned char)p[n]) || p[n] == '.' || p[n] == '_')
        n++;
    for (int i = 0; i < n && k < cap - 1; i++)
        mn[k++] = (char)tolower((unsigned char)p[i]);
    mn[k] = 0;
    *sfx = 0;
    {
        char *dot = strrchr(mn, '.');
        if (dot && dot != mn && strlen(dot) == 2) {
            *sfx = dot[1];
            *dot = 0;
        }
    }
    return n;
}

int rxasm_is_transfer(const char *stmt)
{
    char mn[24], sfx;
    mnemonic(skip_sp(stmt), mn, (int)sizeof mn, &sfx);
    if (!strcmp(mn, "bra") || !strcmp(mn, "bsr"))
        return 1;
    return mn[0] == 'b' && cond_after(mn, 1) >= 0;
}

int rxasm_is_word(const char *stmt, const char *w, int len)
{
    char mn[24], sfx;
    mnemonic(skip_sp(stmt), mn, (int)sizeof mn, &sfx);
    /* a memex operand's size, after its ] */
    if (w > stmt && w[-1] == ']' &&
        (name_is(w, len, ".b") || name_is(w, len, ".w") ||
         name_is(w, len, ".l") || name_is(w, len, ".ub") ||
         name_is(w, len, ".uw")))
        return 1;
    if (!strcmp(mn, "mvtc") || !strcmp(mn, "mvfc") || !strcmp(mn, "pushc") ||
        !strcmp(mn, "popc"))
        return lookup(cr_tab, w, len) >= 0;
    if (!strcmp(mn, "setpsw") || !strcmp(mn, "clrpsw"))
        return lookup(flag_tab, w, len) >= 0;
    return 0;
}

/* The symbol at p (not a register, not `.`), then an optional `+K`/`-K`:
 * the length consumed, 0 when there is none. */
static int sym_operand(const char *p, int *slen, long *add)
{
    int n = 0;
    if (!(isalpha((unsigned char)p[0]) || p[0] == '_' || p[0] == '.'))
        return 0;
    if (p[0] == '.' && !(isalnum((unsigned char)p[1]) || p[1] == '_'))
        return 0;
    while (isalnum((unsigned char)p[n]) || p[n] == '_' || p[n] == '.' ||
           p[n] == '$')
        n++;
    if (rxasm_gpr(p, n) >= 0)
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

/* The statements whose operand is a SYMBOL: `mov.l #sym, rd` (the six-byte
 * form, R_RX_DIR32 on its immediate), and a branch to a symbol this file
 * does not define here -- bra and bsr as .a (R_RX_DIR24S_PCREL), beq/bne
 * as .w (R_RX_DIR16S_PCREL), another condition as .b (R_RX_DIR8S_PCREL),
 * or the size written -- each field one byte after the opcode, as GNU as
 * places them. */
int rxasm_symform(const char *stmt, struct asm_symform *f)
{
    char mn[24], sfx;
    const char *p = skip_sp(stmt), *o;
    int ml, slen, n, c = -1, fam;
    long add;

    ml = mnemonic(p, mn, (int)sizeof mn, &sfx);
    if (!ml || (p[ml] != ' ' && p[ml] != '\t'))
        return 0;
    memset(f, 0, sizeof *f);
    o = skip_sp(p + ml);
    if (!strcmp(mn, "mov") && (sfx == 'l' || !sfx) && *o == '#') {
        const char *r;
        int rl = 0, rn;
        n = sym_operand(o + 1, &slen, &add);
        if (!n)
            return 0;
        r = skip_sp(o + 1 + n);
        if (*r != ',')
            return 0;
        r = skip_sp(r + 1);
        while (isalnum((unsigned char)r[rl]))
            rl++;
        rn = rxasm_gpr(r, rl);
        if (rn < 0 || *skip_sp(r + rl))
            return 0;
        f->sym_at = (int)(o + 1 - stmt);
        f->sym_len = slen;
        f->addend = add;
        snprintf(f->encode, sizeof f->encode, "\003mov.l #0, r%d", rn);
        f->site[0].off = 2;
        f->site[0].reloc = R_RX_DIR32;
        f->nsites = 1;
        return 1;
    }
    c = mn[0] == 'b' ? cond_after(mn, 1) : -1;
    if (!strcmp(mn, "bra"))
        fam = BF_BRA;
    else if (!strcmp(mn, "bsr"))
        fam = BF_BSR;
    else if (c >= 0)
        fam = c == RX_EQ || c == RX_NE ? BF_BEQNE : BF_BCND;
    else
        return 0;
    n = sym_operand(o, &slen, &add);
    if (!n || *skip_sp(o + n))
        return 0;
    if (!sfx)
        sfx = fam == BF_BRA || fam == BF_BSR ? 'a' : fam == BF_BEQNE ? 'w' : 'b';
    if (sfx == 's')
        return refuse_form(f, stmt, o, slen, "a .s branch reaches 3 to 10 "
                           "bytes and carries no relocation: write .b, .w or "
                           ".a, or no size");
    f->sym_at = (int)(o - stmt);
    f->sym_len = slen;
    f->addend = add;
    snprintf(f->encode, sizeof f->encode, "\003%s.%c .+0", mn, sfx);
    f->site[0].off = 1;
    f->site[0].reloc = sfx == 'a' ? R_RX_DIR24S_PCREL
                     : sfx == 'w' ? R_RX_DIR16S_PCREL : R_RX_DIR8S_PCREL;
    f->nsites = 1;
    return 1;
}

void rxasm_fill(struct code *out, long gap)
{
    static const unsigned char nops[7][7] = {
        { 0x03 }, { 0xef, 0x00 }, { 0xfc, 0x13, 0x00 },
        { 0x76, 0x10, 0x01, 0x00 }, { 0x77, 0x10, 0x01, 0x00, 0x00 },
        { 0x74, 0x10, 0x01, 0x00, 0x00, 0x00 },
        { 0xfd, 0x70, 0x40, 0x00, 0x00, 0x00, 0x80 }
    };
    while (gap > 127) {                 /* nothing GNU as is compared on */
        for (int k = 0; k < 7; k++)
            code_byte(out, nops[6][k]);
        gap -= 7;
    }
    if (gap >= 8) {
        /* a bra.b over the rest, and that pair again where it never runs */
        for (long k = 0; k + 1 < gap; k += 2) {
            code_byte(out, 0x2e);
            code_byte(out, (int)gap);
        }
        if (gap & 1)
            code_byte(out, 0x03);
        return;
    }
    for (long k = 0; k < gap; k++)
        code_byte(out, nops[gap - 1][k]);
}

/* ---- the referee's input ----------------------------------------------------
 *
 * Each line is a statement, an @, and what rx-elf-objdump prints for it,
 * written from the same table entry -- the operands as the instruction
 * means them -- so a wrong opcode, field order or size reads back
 * differently. GNU as assembles the statements too, and its bytes must be
 * EmbCC's (tests/golden/rx-asm.sh). */
/* A register pair spread over the file: low and high, both sides of r7
 * (the dsp:5 forms reach only r0-r7). */
static const int vr[][3] = {
    { 1, 2, 3 }, { 7, 0, 6 }, { 8, 15, 4 }, { 14, 3, 12 }, { 5, 9, 11 },
    { 10, 13, 2 }, { 0, 7, 15 }
};
#define NVR ((int)(sizeof vr / sizeof vr[0]))

/* A value as objdump prints a .b/.w/.l immediate stored: .b unsigned,
 * the rest sign-extended. */
static long long vsz(long long v, int sz)
{
    if (sz == RX_B) return v & 0xff;
    if (sz == RX_W) return (short)(v & 0xffff);
    return (int)(unsigned int)(v & 0xffffffffLL);
}

void rxasm_vocabulary(FILE *f)
{
    static const long long imms[] = {
        0, 1, 15, 16, 127, 128, 255, 256, -1, -2, -128, -129, 32767, 32768,
        -32768, -32769, 65535, 8388607, -8388608, 8388608, 0x12345678,
        -2147483647LL - 1, 4294967295LL, 999, -999, 1000
    };
    const int nimm = (int)(sizeof imms / sizeof imms[0]);
    static const char *const szn[] = { "b", "w", "l" };
    int j, k;

    /* ---- the ALU ---- */
    for (const struct alu_ent *e = alu_tab; e->name; e++) {
        const char *n = e->name;
        int emul = e->op == RX_EMUL || e->op == RX_EMULU;
        for (j = 0; j < NVR; j++) {
            int a = vr[j][0], b = vr[j][1], c = vr[j][2];
            if (emul && b == 15)
                b = 14;
            if (e->forms & F_RR)
                fprintf(f, "%s r%d, r%d@%s r%d, r%d\n", n, a, b, n, a, b);
            if (e->forms & F_R1)
                fprintf(f, "%s r%d@%s r%d\n", n, c, n, c);
            if (e->forms & F_RRR)
                fprintf(f, "%s r%d, r%d, r%d@%s r%d, r%d, r%d\n", n, a, b, c,
                        n, a, b, c);
        }
        if (e->forms & F_RI)
            for (k = 0; k < nimm; k++) {
                long long v = imms[k];
                int rd = vr[k % NVR][k & 1];
                long long s32v = (int)(unsigned int)(v & 0xffffffffLL);
                if (emul && rd == 15)
                    rd = 14;
                if ((e->op == RX_SUB || e->op == RX_SBB) && v > 2147483647LL)
                    continue;
                if (e->op == RX_SUB)
                    fprintf(f, "sub #%lld, r%d@", v, rd);
                else
                    fprintf(f, "%s #%lld, r%d@", n, v, rd);
                if (e->op == RX_SUB && (v < 0 || v > 15))
                    fprintf(f, "add #%lld, r%d, r%d\n",
                            (long long)(int)(unsigned int)(-s32v), rd, rd);
                else if (e->op == RX_ADD && (v < 0 || v > 15))
                    fprintf(f, "add #%lld, r%d, r%d\n", s32v, rd, rd);
                else if (e->op == RX_SBB)
                    fprintf(f, "adc #%lld, r%d\n",
                            (long long)(int)~(unsigned int)s32v, rd);
                else
                    fprintf(f, "%s #%lld, r%d\n", n, s32v, rd);
            }
        if (e->forms & F_IRR)
            for (k = 0; k < nimm; k += 3) {
                int rs = vr[k % NVR][0], rd = vr[k % NVR][2];
                long long s32v = (int)(unsigned int)(imms[k] & 0xffffffffLL);
                fprintf(f, "add #%lld, r%d, r%d@add #%lld, r%d, r%d\n",
                        imms[k], rs, rd, s32v, rs, rd);
            }
        if (e->forms & F_RM) {
            static const char *const sf[] = { ".b", ".w", ".l", ".ub", ".uw",
                                              "" };
            static const int ssz[] = { RX_B, RX_W, RX_L, RX_B, RX_W, RX_L };
            static const long dsp[] = { 0, 1, 2, 4, 252, 255, 256, 1020,
                                        65535, 131070, 262140 };
            for (j = 0; j < 6; j++) {
                int sc = ssz[j] == RX_B ? 1 : ssz[j] == RX_W ? 2 : 4;
                if ((e->op == RX_ADC || e->op == RX_SBB) && ssz[j] != RX_L)
                    continue;
                for (k = 0; k < 11; k++) {
                    long d = dsp[k];
                    int rs = vr[(j + k) % NVR][0], rd = vr[(j + k) % NVR][1];
                    const char *pn = j == 5 ? "l" : sf[j] + 1;
                    if (d % sc || d / sc > 65535)
                        continue;
                    if (emul && rd == 15)
                        rd = 14;
                    if (d)
                        fprintf(f, "%s %ld[r%d]%s, r%d@%s %ld[r%d].%s, r%d\n",
                                n, d, rs, sf[j], rd, n, d, rs, pn, rd);
                    else
                        fprintf(f, "%s [r%d]%s, r%d@%s [r%d].%s, r%d\n", n, rs,
                                sf[j], rd, n, rs, pn, rd);
                }
                fprintf(f, "%s 0[r%d]%s, r%d@%s [r%d].%s, r%d\n", n, j + 1,
                        sf[j], emul ? 14 - j : 15 - j, n, j + 1,
                        j == 5 ? "l" : sf[j] + 1, emul ? 14 - j : 15 - j);
            }
        }
    }

    /* ---- mov and movu ---- */
    for (j = 0; j < NVR; j++) {
        int a = vr[j][0], b = vr[j][1];
        fprintf(f, "mov r%d, r%d@mov.l r%d, r%d\n", a, b, a, b);
        for (k = 0; k < 3; k++)
            fprintf(f, "mov.%s r%d, r%d@mov.%s r%d, r%d\n", szn[k], b, a,
                    szn[k], b, a);
        for (k = 0; k < 2; k++)
            fprintf(f, "movu.%s r%d, r%d@movu.%s r%d, r%d\n", szn[k], a, b,
                    szn[k], a, b);
    }
    for (k = 0; k < nimm; k++)
        fprintf(f, "mov.l #%lld, r%d@mov.l #%lld, r%d\n", imms[k],
                vr[k % NVR][1], vsz(imms[k], RX_L), vr[k % NVR][1]);
    fprintf(f, "mov #-5, r3@mov.l #-5, r3\n");
    for (int sz = RX_B; sz <= RX_L; sz++) {
        int sc = sz == RX_B ? 1 : sz == RX_W ? 2 : 4;
        static const long un[] = { 0, 1, 31, 32, 255, 256, 32767, 65535 };
        for (k = 0; k < 8; k++)
            for (j = 0; j < NVR; j++) {
                long d = un[k] * sc;
                int rs = vr[j][0], rd = vr[j][1];
                const char *zs = d == 0 ? "" : NULL;
                char dt[32];
                if ((j + k) % 2)
                    continue;
                snprintf(dt, sizeof dt, "%ld", d);
                /* mov.size dsp[rs], rd: a load (movu below) */
                if (zs)
                    fprintf(f, "mov.%s [r%d], r%d@mov.%s [r%d], r%d\n",
                            szn[sz], rs, rd, szn[sz], rs, rd);
                else
                    fprintf(f, "mov.%s %s[r%d], r%d@mov.%s %s[r%d], r%d\n",
                            szn[sz], dt, rs, rd, szn[sz], dt, rs, rd);
                if (sz != RX_L && un[k] <= 32767) {
                    if (zs)
                        fprintf(f, "movu.%s [r%d], r%d@movu.%s [r%d], r%d\n",
                                szn[sz], rd, rs, szn[sz], rd, rs);
                    else
                        fprintf(f, "movu.%s %s[r%d], r%d@movu.%s %s[r%d], "
                                "r%d\n", szn[sz], dt, rd, rs, szn[sz], dt, rd,
                                rs);
                }
                if (zs)
                    fprintf(f, "mov.%s r%d, [r%d]@mov.%s r%d, [r%d]\n",
                            szn[sz], rd, rs, szn[sz], rd, rs);
                else
                    fprintf(f, "mov.%s r%d, %s[r%d]@mov.%s r%d, %s[r%d]\n",
                            szn[sz], rd, dt, rs, szn[sz], rd, dt, rs);
                if (un[k] <= 32767) {
                    long long v = imms[(j * 7 + k) % nimm];
                    if (sz == RX_B) v = v < -128 || v > 255 ? 255 - (v & 7) : v;
                    if (sz == RX_W) v = v < -32768 || v > 65535 ? -3 : v;
                    if (zs)
                        fprintf(f, "mov.%s #%lld, [r%d]@mov.%s #%lld, [r%d]\n",
                                szn[sz], v, rs, szn[sz], vsz(v, sz), rs);
                    else
                        fprintf(f, "mov.%s #%lld, %s[r%d]@mov.%s #%lld, "
                                "%s[r%d]\n", szn[sz], v, dt, rs, szn[sz],
                                vsz(v, sz), dt, rs);
                }
            }
        /* an explicit 0: the dsp:5 form where both registers are r0-r7 */
        for (j = 0; j < NVR; j++) {
            int rs = vr[j][0], rd = vr[j][2];
            const char *z = rs < 8 && rd < 8 ? "0" : "";
            fprintf(f, "mov.%s 0[r%d], r%d@mov.%s %s[r%d], r%d\n", szn[sz],
                    rs, rd, szn[sz], z, rs, rd);
            fprintf(f, "mov.%s r%d, 0[r%d]@mov.%s r%d, %s[r%d]\n", szn[sz],
                    rd, rs, szn[sz], rd, z, rs);
            fprintf(f, "mov.%s #%d, 0[r%d]@mov.%s #%d, %s[r%d]\n", szn[sz],
                    j * 30, rs, szn[sz], j * 30, rs < 8 ? "0" : "", rs);
            if (sz != RX_L)
                fprintf(f, "movu.%s 0[r%d], r%d@movu.%s %s[r%d], r%d\n",
                        szn[sz], rs, rd, szn[sz], z, rs, rd);
            fprintf(f, "mov.%s [r%d, r%d], r%d@mov.%s [r%d, r%d], r%d\n",
                    szn[sz], vr[j][0], vr[j][1], vr[j][2], szn[sz], vr[j][0],
                    vr[j][1], vr[j][2]);
            fprintf(f, "mov.%s r%d, [r%d,r%d]@mov.%s r%d, [r%d, r%d]\n",
                    szn[sz], vr[j][2], vr[j][1], vr[j][0], szn[sz], vr[j][2],
                    vr[j][1], vr[j][0]);
            if (sz != RX_L)
                fprintf(f, "movu.%s [r%d, r%d], r%d@movu.%s [r%d, r%d], r%d\n",
                        szn[sz], vr[j][1], vr[j][2], vr[j][0], szn[sz],
                        vr[j][1], vr[j][2], vr[j][0]);
            fprintf(f, "mov.%s [r%d+], r%d@mov.%s [r%d+], r%d\n", szn[sz],
                    vr[j][0], vr[j][1], szn[sz], vr[j][0], vr[j][1]);
            fprintf(f, "mov.%s [-r%d], r%d@mov.%s [-r%d], r%d\n", szn[sz],
                    vr[j][1], vr[j][2], szn[sz], vr[j][1], vr[j][2]);
            fprintf(f, "mov.%s r%d, [r%d+]@mov.%s r%d, [r%d+]\n", szn[sz],
                    vr[j][2], vr[j][0], szn[sz], vr[j][2], vr[j][0]);
            fprintf(f, "mov.%s r%d, [-r%d]@mov.%s r%d, [-r%d]\n", szn[sz],
                    vr[j][0], vr[j][2], szn[sz], vr[j][0], vr[j][2]);
            if (sz != RX_L) {
                fprintf(f, "movu.%s [r%d+], r%d@movu.%s [r%d+], r%d\n",
                        szn[sz], vr[j][2], vr[j][0], szn[sz], vr[j][2],
                        vr[j][0]);
                fprintf(f, "movu.%s [-r%d], r%d@movu.%s [-r%d], r%d\n",
                        szn[sz], vr[j][0], vr[j][1], szn[sz], vr[j][0],
                        vr[j][1]);
            }
        }
    }

    /* ---- shifts, rotates, the one-register forms ---- */
    for (const struct nament *e = shift_tab; e->name; e++) {
        int rot = e->v == RX_ROTL || e->v == RX_ROTR;
        for (j = 0; j < NVR; j++) {
            int a = vr[j][0], b = vr[j][1], n = (j * 9) % 32;
            fprintf(f, "%s #%d, r%d@%s #%d, r%d\n", e->name, n, b, e->name, n, b);
            fprintf(f, "%s r%d, r%d@%s r%d, r%d\n", e->name, a, b, e->name, a, b);
            if (!rot) {
                fprintf(f, "%s #%d, r%d, r%d@%s #%d, r%d, r%d\n", e->name,
                        31 - n, a, b, e->name, 31 - n, a, b);
                fprintf(f, "%s #%d, r%d, r%d@%s #%d, r%d, r%d\n", e->name, n,
                        a, a, e->name, n, a, a);
            }
        }
        fprintf(f, "%s #31, r15@%s #31, r15\n", e->name, e->name);
    }
    for (j = 0; j < 16; j += 3) {
        fprintf(f, "rolc r%d@rolc r%d\nrorc r%d@rorc r%d\nsat r%d@sat r%d\n",
                j, j, 15 - j, 15 - j, (j + 1) % 16, (j + 1) % 16);
        fprintf(f, "revl r%d, r%d@revl r%d, r%d\n", j, 15 - j, j, 15 - j);
        fprintf(f, "revw r%d, r%d@revw r%d, r%d\n", 15 - j, j, 15 - j, j);
    }

    /* ---- the stack ---- */
    for (j = 0; j < 16; j += 3) {
        for (k = 0; k < 3; k++)
            fprintf(f, "push.%s r%d@push.%s r%d\n", szn[k], (j + k % 2) % 16,
                    szn[k], (j + k % 2) % 16);
        fprintf(f, "push r%d@push.l r%d\npop r%d@pop r%d\n", 15 - j, 15 - j,
                j, j);
    }
    for (k = 0; k < 3; k++) {
        int sc = k == RX_B ? 1 : k == RX_W ? 2 : 4;
        fprintf(f, "push.%s [r%d]@push.%s [r%d]\n", szn[k], k + 1, szn[k], k + 1);
        fprintf(f, "push.%s %d[r%d]@push.%s %d[r%d]\n", szn[k], 4 * sc, 14 - k,
                szn[k], 4 * sc, 14 - k);
        fprintf(f, "push.%s %d[r%d]@push.%s %d[r%d]\n", szn[k], 300 * sc, k + 7,
                szn[k], 300 * sc, k + 7);
    }
    {
        static const int rg[][2] = { { 1, 2 }, { 1, 15 }, { 6, 13 }, { 14, 15 },
                                     { 3, 9 }, { 7, 7 } };
        for (j = 0; j < 6; j++) {
            int a = rg[j][0], b = rg[j][1];
            if (a == b) {
                fprintf(f, "pushm r%d-r%d@push.l r%d\n", a, b, a);
                fprintf(f, "popm r%d-r%d@pop r%d\n", a, b, a);
                continue;
            }
            fprintf(f, "pushm r%d-r%d@pushm r%d-r%d\n", a, b, a, b);
            fprintf(f, "popm r%d-r%d@popm r%d-r%d\n", a, b, a, b);
        }
    }
    for (const struct nament *e = cr_tab; e->name; e++) {
        fprintf(f, "pushc %s@pushc %s\n", e->name, e->name);
        if (e->v != 1) {
            fprintf(f, "popc %s@popc %s\n", e->name, e->name);
            fprintf(f, "mvtc r%d, %s@mvtc r%d, %s\n", e->v + 1, e->name,
                    e->v + 1, e->name);
            for (k = 0; k < nimm; k += 5)
                fprintf(f, "mvtc #%lld, %s@mvtc #%lld, %s\n", imms[k], e->name,
                        vsz(imms[k], RX_L), e->name);
        }
        fprintf(f, "mvfc %s, r%d@mvfc %s, r%d\n", e->name, 15 - e->v, e->name,
                15 - e->v);
    }
    for (const struct nament *e = flag_tab; e->name; e++)
        fprintf(f, "setpsw %s@setpsw %s\nclrpsw %s@clrpsw %s\n", e->name,
                e->name, e->name, e->name);
    for (j = 0; j <= 15; j += 5)
        fprintf(f, "mvtipl #%d@mvtipl #%d\n", j, j);
    for (j = 0; j <= 255; j += 85)
        fprintf(f, "int #%d@int #%d\n", j, j);
    for (k = 0; op0_tab[k].name; k++)
        fprintf(f, "%s@%s\n", op0_tab[k].name, op0_tab[k].name);
    for (j = 0; j <= 1020; j += 340)
        fprintf(f, "rtsd #%d@rtsd #%d\n", j, j);
    fprintf(f, "rtsd #12, r6-r8@rtsd #12, r6-r8\n");
    fprintf(f, "rtsd #1020, r1-r15@rtsd #1020, r1-r15\n");
    fprintf(f, "rtsd #4, r15-r15@rtsd #4, r15-r15\n");

    /* ---- transfers ---- */
    for (j = 0; j < 15; j += 3) {
        fprintf(f, "jmp r%d@jmp r%d\njsr r%d@jsr r%d\n", j, j, 15 - j, 15 - j);
        fprintf(f, "bra.l r%d@bra.l r%d\nbsr.l r%d@bsr.l r%d\n", j + 1, j + 1,
                14 - j, 14 - j);
    }
    {
        static const long sd[] = { 3, 10 };
        static const long bd[] = { -128, 127, 0, 2 };
        static const long wd[] = { -32768, 32767, 128, -129 };
        static const long ad[] = { -8388608L, 8388607L, 32768, -32769 };
        for (k = 0; k < 2; k++) {
            fprintf(f, "bra.s .+%ld@bra.s {+%ld}\n", sd[k], sd[k]);
            fprintf(f, "beq.s .+%ld@beq.s {+%ld}\n", sd[k], sd[k]);
            fprintf(f, "bne.s .+%ld@bne.s {+%ld}\n", sd[k], sd[k]);
        }
        for (k = 0; k < 4; k++) {
            fprintf(f, "bra.b .%+ld@bra.b {+%ld}\n", bd[k], bd[k]);
            fprintf(f, "bra.w .%+ld@bra.w {+%ld}\n", wd[k], wd[k]);
            fprintf(f, "bra.a .%+ld@bra.a {+%ld}\n", ad[k], ad[k]);
            fprintf(f, "bsr.w .%+ld@bsr.w {+%ld}\n", wd[k], wd[k]);
            fprintf(f, "bsr.a .%+ld@bsr.a {+%ld}\n", ad[k], ad[k]);
            fprintf(f, "beq.w .%+ld@beq.w {+%ld}\n", wd[k], wd[k]);
            fprintf(f, "bne.w .%+ld@bne.w {+%ld}\n", wd[k], wd[k]);
        }
        for (const struct nament *e = cond_tab; e->name; e++)
            for (k = 0; k < 4; k++)
                fprintf(f, "b%s.b .%+ld@b%s.b {+%ld}\n", e->name, bd[k],
                        rx_cond_name(e->v), bd[k]);
        /* no size: the shortest that reaches, as GNU as relaxes */
        {
            static const long rd_[] = { 3, 10, 11, 2, -1, 127, -128, 128,
                                        -129, 32767, -32768, 32768, -32769,
                                        8388607L, -8388608L };
            for (k = 0; k < 15; k++) {
                long d = rd_[k];
                fprintf(f, "bra .%+ld@bra.%s {+%ld}\n", d,
                        d >= 3 && d <= 10 ? "s" : d >= -128 && d <= 127 ? "b"
                        : d >= -32768 && d <= 32767 ? "w" : "a", d);
                fprintf(f, "bsr .%+ld@bsr.%s {+%ld}\n", d,
                        d >= -32768 && d <= 32767 ? "w" : "a", d);
                for (j = 0; j < 2; j++) {
                    const char *cn = j ? "ne" : "eq", *nc = j ? "eq" : "ne";
                    if (d >= 3 && d <= 10)
                        fprintf(f, "b%s .%+ld@b%s.s {+%ld}\n", cn, d, cn, d);
                    else if (d >= -128 && d <= 127)
                        fprintf(f, "b%s .%+ld@b%s.b {+%ld}\n", cn, d, cn, d);
                    else if (d >= -32768 && d <= 32767)
                        fprintf(f, "b%s .%+ld@b%s.w {+%ld}\n", cn, d, cn, d);
                    else if (d - 1 >= -8388608L && d - 1 <= 8388607L)
                        fprintf(f, "b%s .%+ld@b%s.s {+5}|bra.a {+%ld}\n", cn,
                                d, nc, d);
                }
                for (const struct nament *e = cond_tab + 4; e->name; e++) {
                    if ((e - cond_tab) % 3 != 1)
                        continue;
                    const char *cn = rx_cond_name(e->v);
                    const char *nc = rx_cond_name(rx_cond_invert(e->v));
                    if (d >= -128 && d <= 127)
                        fprintf(f, "b%s .%+ld@b%s.b {+%ld}\n", e->name, d, cn,
                                d);
                    else if (d == -32768 || d == -32767)
                        continue;       /* GNU as wraps these (see K_INVB_W) */
                    else if (d >= -32766 && d <= 32767)
                        fprintf(f, "b%s .%+ld@b%s.b {+5}|bra.w {+%ld}\n",
                                e->name, d, nc, d);
                    else if (d - 2 >= -8388608L)
                        fprintf(f, "b%s .%+ld@b%s.b {+6}|bra.a {+%ld}\n",
                                e->name, d, nc, d);
                }
            }
        }
    }

    /* ---- bits ---- */
    for (const struct nament *e = bit_tab; e->name; e++)
        for (j = 0; j < NVR; j++) {
            int a = vr[j][0], b = vr[j][1], n = (j * 5 + 1) % 32;
            long d = j == 0 ? 0 : j == 1 ? 255 : j == 2 ? 256 : j * 1000;
            fprintf(f, "%s #%d, r%d@%s #%d, r%d\n", e->name, n, a, e->name, n, a);
            fprintf(f, "%s r%d, r%d@%s r%d, r%d\n", e->name, a, b, e->name, a, b);
            if (d)
                fprintf(f, "%s #%d, %ld[r%d].b@%s #%d, %ld[r%d].b\n", e->name,
                        n % 8, d, b, e->name, n % 8, d, b);
            else
                fprintf(f, "%s #%d, [r%d].b@%s #%d, [r%d].b\n", e->name, n % 8,
                        b, e->name, n % 8, b);
            fprintf(f, "%s r%d, %ld[r%d].b@%s r%d, %ld[r%d].b\n", e->name, a,
                    d + 1, b, e->name, a, d + 1, b);
        }
    for (const struct nament *e = cond_tab; e->name; e++) {
        int r = (e->v * 3) % 16, n = (e->v * 7) % 32;
        fprintf(f, "bm%s #%d, r%d@bm%s #%d, r%d\n", e->name, n, r,
                rx_cond_name(e->v), n, r);
        if (e->v)
            fprintf(f, "bm%s #%d, %d[r%d].b@bm%s #%d, %d[r%d].b\n", e->name,
                    n % 8, e->v * 20, 15 - r, rx_cond_name(e->v), n % 8,
                    e->v * 20, 15 - r);
        else            /* an explicit 0 is no displacement here */
            fprintf(f, "bm%s #%d, 0[r%d].b@bm%s #%d, [r%d].b\n", e->name,
                    n % 8, 15 - r, rx_cond_name(e->v), n % 8, 15 - r);
        fprintf(f, "sc%s.l r%d@sc%s.l r%d\n", e->name, 15 - r,
                rx_cond_name(e->v), 15 - r);
    }

    /* ---- strings, the accumulator ---- */
    for (const struct nament *e = str_tab; e->name; e++) {
        if (e->v > RX_RMPA) {
            fprintf(f, "%s@%s\n", e->name, e->name);
            continue;
        }
        for (k = 0; k < 3; k++)
            fprintf(f, "%s.%s@%s.%s\n", e->name, szn[k], e->name, szn[k]);
        fprintf(f, "%s@%s.l\n", e->name, e->name);
    }
    for (j = 0; j < 14; j += 4) {
        fprintf(f, "mvfachi r%d@mvfachi #0, a0, r%d\n", j, j);
        fprintf(f, "mvfaclo r%d@mvfaclo #0, a0, r%d\n", j + 1, j + 1);
        fprintf(f, "mvfacmi r%d@mvfacmi #0, a0, r%d\n", j + 2, j + 2);
        fprintf(f, "mvtachi r%d@mvtachi r%d, a0\n", 15 - j, 15 - j);
        fprintf(f, "mvtaclo r%d@mvtaclo r%d, a0\n", j, j);
        fprintf(f, "mulhi r%d, r%d@mulhi r%d, r%d, a0\n", j, 15 - j, j, 15 - j);
        fprintf(f, "mullo r%d, r%d@mullo r%d, r%d, a0\n", 15 - j, j, 15 - j, j);
        fprintf(f, "machi r%d, r%d@machi r%d, r%d, a0\n", j + 1, j, j + 1, j);
        fprintf(f, "maclo r%d, r%d@maclo r%d, r%d, a0\n", j, j + 1, j, j + 1);
    }
    fprintf(f, "racw #1@racw #1, a0\nracw #2@racw #2, a0\n");
    /* spellings: upper case, no blanks */
    fprintf(f, "MOV.L #1,R2@mov.l #1, r2\nADD R1,R2,R3@add r1, r2, r3\n");
    fprintf(f, "Mvtc R1, PSW@mvtc r1, psw\nSETPSW I@setpsw i\n");
}
