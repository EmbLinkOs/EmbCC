/* The TriCore assembler: every instruction of TC1.6.2, from a table.
 *
 * isa.inc holds the instruction set as data -- 912 forms, each a mnemonic,
 * the shape of its operands (`D,[A]N`: a data register, then an address
 * register with an offset), its length, its opcode bits, and where each
 * operand's bits go. The table was not copied from a manual or another
 * assembler: tools/tricore-isa learned it from GNU objdump and GNU as,
 * used as oracles -- every 16-bit word and a sample of the 32-bit space
 * decoded, each bit flipped to see whether it is opcode, operand or
 * reserved, each operand bit's weight measured, and every form's operands
 * offered to GNU as to learn which values it takes (even registers for a
 * pair, a shift of 0 or 1) and which relocation a symbol in each field
 * gets. What this file adds is reading a statement and choosing a form.
 *
 * READING. A statement is a mnemonic and operands; its operands are
 * reduced to a shape -- registers to D, A or E (with a half's l/u suffix:
 * `%d3l` is Dl), numbers and expressions to N, a core special function
 * register to $S -- and the list of their values. Registers are d0-d15,
 * a0-a15 (sp is a10) and the pairs e0-e14, with or without `%`, in either
 * case (GCC writes `%SP`). Memory is `[A]off`, `[A+]off` (post-increment),
 * `[+A]off` (pre-increment), `[A+c]off` (circular, on a pair),
 * `[A+r]` (bit-reverse) and `[A+i]`. A constant may carry `#`; `hi:`,
 * `lo:`, `up:` and `sm:` take a number's halves as they would a symbol's.
 * mfcr/mtcr take a core register by number (`0xfe2c`), `$` and a number,
 * or a name (`psw`, `$icr`).
 *
 * CHOOSING, as GNU as chooses, so that an object is byte for byte the
 * one tricore-elf-as writes from the same source:
 *   - the shortest form the operands fit, and among forms of one length
 *     the one GNU takes (the table is in that order);
 *   - `add d1, d1, d2` is also `add d1, d2`, the two-operand 16-bit form,
 *     for the mnemonics where GNU narrows so (tc_narrows) -- but when a
 *     three-operand 16-bit form fits (the d15 ones), that one;
 *   - a branch to a label is always its 32-bit form: `jz d2, L` is
 *     written `jeq d2, 0, L` (jnz, jgez, jltz, jgtz and jlez likewise),
 *     because a label's distance is not known when the size is chosen;
 *   - a conditional branch out of reach (+-32 KiB) becomes the inverse
 *     condition, in its shortest form, over a `j` (+-16 MiB); one with no
 *     inverse (loop, jned, jnei) branches to the `j` just past a 16-bit
 *     one over it: `loop A, .+6; j .+6; j L`;
 *   - after `.code16` or `.code32` the next instruction takes a form of
 *     that width or is refused.
 * A branch's target is `.+N` / `.-N`, bytes from the instruction -- what
 * src/as/gas.c writes for a label -- or, in a template, a numeric label
 * of its own (`1:`, used as `1b` or `1f`).
 *
 * SYMBOLS (tcasm_symform, for src/as/gas.c): a symbol in any numeric
 * field takes the relocation GNU as gives it there -- R_TRICORE_24REL for
 * j/jl/call, HIADJ for hi:, HI for up:, LO or LO2 for lo:, 16SM, 10SM or
 * 16SM2 for sm:, 18ABS for an absolute address, and so on (tc_symrules),
 * in the form GNU takes. A conditional branch to a symbol is the inverse
 * over a `j SYM`, as one out of reach is. A field GNU takes no symbol in
 * is refused, by name. */
#include "asm.h"
#include "../asmexpr.h"
#include "../../elf/elf.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the table ---------------------------------------------------------- */

struct tc_fbit { unsigned char pos, shift, neg; };
/* kind: D A E (a register), N (a number), L (a PC-relative target, in
 * bytes), S (a core special function register), M (another field's value
 * again). rule: 0 any value, 1
 * even only, 2 only the nok values at tc_oks[ok]. */
struct tc_field {
    char kind;
    unsigned char nb;
    unsigned short fb;
    int v0;
    unsigned char rule;
    unsigned short ok, nok;
};
struct tc_insn {
    const char *mnem, *shape;
    unsigned char len;
    unsigned int base;
    unsigned short f, nf;
};
/* A symbol in field `field` with operator `op` (sym, hi:, lo:, sm:, up:,
 * a label in another section): the form GNU takes (an index into
 * tc_insns; -1 the long branch; -2 refused), the relocation and where it
 * sits; for the long branch `extra` is the sequence's length. */
struct tc_symrule {
    const char *mnem, *shape;
    int field, op, entry, rel, reloff, extra;
    /* when GNU writes another form (`add d0, sym` as `add d0, d0, sym`),
     * the statement's operand each of that form's fields takes */
    const char *map;
};
struct tc_narrow { const char *mnem, *shape; int a, b; };

#include "isa.inc"

#define NINSN ((int)(sizeof tc_insns / sizeof tc_insns[0]))
#define NSYMR ((int)(sizeof tc_symrules / sizeof tc_symrules[0]))
#define NNARROW ((int)(sizeof tc_narrows / sizeof tc_narrows[0]))

/* The first entry of a mnemonic, or -1. */
static int first_of(const char *mn)
{
    int lo = 0, hi = NINSN;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (strcmp(tc_insns[mid].mnem, mn) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < NINSN && !strcmp(tc_insns[lo].mnem, mn) ? lo : -1;
}

/* ---- registers and names ------------------------------------------------ */

/* A register by name: 'd', 'a' or 'e' in *file, the number returned, or
 * -1. With or without `%`, either case; sp is a10. */
int tcasm_reg(const char *p, int n, int *file)
{
    int r = 0, k, c;
    if (n > 0 && *p == '%') { p++; n--; }
    if (n == 2 && tolower((unsigned char)p[0]) == 's' &&
        tolower((unsigned char)p[1]) == 'p') {
        *file = 'a';
        return 10;
    }
    if (n < 2 || n > 3)
        return -1;
    c = tolower((unsigned char)p[0]);
    if (c != 'd' && c != 'a' && c != 'e')
        return -1;
    for (k = 1; k < n; k++) {
        if (!isdigit((unsigned char)p[k]))
            return -1;
        r = r * 10 + (p[k] - '0');
    }
    if ((n == 3 && p[1] == '0') || r > 15 || (c == 'e' && (r & 1)))
        return -1;
    *file = c;
    return r;
}

/* The core special function registers by name (TC1.6.2). */
static const struct { const char *nm; int v; } sfrs[] = {
    { "pcxi", 0xfe00 }, { "psw", 0xfe04 }, { "pc", 0xfe08 },
    { "syscon", 0xfe14 }, { "cpu_id", 0xfe18 }, { "core_id", 0xfe1c },
    { "biv", 0xfe20 }, { "btv", 0xfe24 }, { "isp", 0xfe28 },
    { "icr", 0xfe2c }, { "pprs", 0xfe34 }, { "fcx", 0xfe38 },
    { "lcx", 0xfe3c }, { "compat", 0x9400 }, { "fpu_trap_con", 0xa000 },
    { "fpu_trap_pc", 0xa004 }, { "fpu_trap_opc", 0xa008 },
    { "fpu_trap_src1", 0xa010 }, { "fpu_trap_src2", 0xa014 },
    { "fpu_trap_src3", 0xa018 }, { "dbgsr", 0xfd00 }, { "exevt", 0xfd08 },
    { "crevt", 0xfd0c }, { "swevt", 0xfd10 }, { "trig_acc", 0xfd30 },
    { "dms", 0xfd40 }, { "dcx", 0xfd44 }, { "dbgtcr", 0xfd48 },
    { "pcon0", 0x920c }, { "pcon1", 0x9204 }, { "pcon2", 0x9208 },
    { "pstr", 0x9200 }, { "dcon0", 0x9040 }, { "dcon1", 0x9008 },
    { "dcon2", 0x9000 }, { "dstr", 0x9010 }, { "datr", 0x9018 },
    { "deadd", 0x901c }, { "diear", 0x9020 }, { "dietr", 0x9024 },
    { "piear", 0x9210 }, { "pietr", 0x9214 }, { "ccnt", 0xfc04 },
    { "icnt", 0xfc08 }, { "m1cnt", 0xfc0c }, { "m2cnt", 0xfc10 },
    { "m3cnt", 0xfc14 }, { "cctrl", 0xfc00 },
};

static int sfr_name(const char *p, int n, long long *v)
{
    if (n > 0 && *p == '$') { p++; n--; }
    for (unsigned k = 0; k < sizeof sfrs / sizeof sfrs[0]; k++) {
        const char *s = sfrs[k].nm;
        int i = 0;
        while (i < n && s[i] && tolower((unsigned char)p[i]) == s[i])
            i++;
        if (i == n && !s[i]) {
            *v = sfrs[k].v;
            return 1;
        }
    }
    return 0;
}

static int is_sfr_mnem(const char *mn)
{
    return !strcmp(mn, "mfcr") || !strcmp(mn, "mtcr");
}

/* ---- a statement -------------------------------------------------------- */

#define MAXAT 8
struct atom {
    char kind;                  /* D A E N S */
    char tgt;                   /* N: written `.`, `.+K` or `.-K` */
    long long v;
    const char *p;              /* its text, for symform */
    int n;
};
struct stmt {
    char mn[24];
    char shape[64];
    struct atom at[MAXAT];
    int nat;
};

/* A `hi:`/`lo:`/`up:`/`sm:` operator at p (or `%hi(`/`%lo(`): its index
 * (1 hi, 2 lo, 3 sm, 4 up) and its length in *len, or 0. */
static int halfop(const char *p, const char *e, int *len)
{
    static const char *const ops[] = { "hi:", "lo:", "sm:", "up:" };
    for (int k = 0; k < 4; k++)
        if (e - p >= 3 && !strncmp(p, ops[k], 3)) {
            *len = 3;
            return k + 1;
        }
    if (e - p >= 4 && (!strncmp(p, "%hi(", 4) || !strncmp(p, "%lo(", 4))) {
        *len = 4;
        return p[1] == 'h' ? 1 : 2;
    }
    return 0;
}

/* An expression's value: a number, `.±K` (a target), or a half of a
 * number. 0 when it is none of these (a symbol). */
static int value(const char *p, const char *e, long long *v, char *tgt)
{
    int hl = 0, op;
    while (p < e && isspace((unsigned char)*p)) p++;
    while (e > p && isspace((unsigned char)e[-1])) e--;
    *tgt = 0;
    if (p < e && *p == '#') p++;
    if (p == e)
        return 0;
    op = halfop(p, e, &hl);
    if (op) {
        p += hl;
        if (hl == 4) {
            if (e[-1] != ')')
                return 0;
            e--;
        }
        if (!asm_const_expr(p, (int)(e - p), v))
            return 0;
        switch (op) {
        case 1: *v = ((*v + 0x8000) >> 16) & 0xffff; break;
        case 2: *v = (short)(*v & 0xffff); break;
        case 4: *v = (*v >> 16) & 0xffff; break;
        default: return 0;              /* sm: is an offset from a base */
        }
        return 1;
    }
    if (*p == '.' && (e - p == 1 || p[1] == '+' || p[1] == '-')) {
        *tgt = 1;
        if (e - p == 1) {
            *v = 0;
            return 1;
        }
        return asm_const_expr(p + 1, (int)(e - p - 1), v);
    }
    return asm_const_expr(p, (int)(e - p), v);
}

/* Read [p, e) into s. With `syms`, an expression that is not a number is
 * an N atom with no value (symform's); otherwise it is an error. */
static int parse(const char *p, const char *e, struct stmt *s, int syms,
                 char *err, size_t errlen)
{
    int sh = 0, k = 0, inbr = 0;
    const char *q;
    memset(s, 0, sizeof *s);
    while (p < e && isspace((unsigned char)*p)) p++;
    while (e > p && isspace((unsigned char)e[-1])) e--;
    q = p;
    while (q < e && !isspace((unsigned char)*q)) {
        if (k < (int)sizeof s->mn - 1)
            s->mn[k++] = (char)tolower((unsigned char)*q);
        q++;
    }
    s->mn[k] = 0;
    if (!k) {
        snprintf(err, errlen, "TriCore asm: no mnemonic");
        return -1;
    }
    p = q;
#define PUT(c) do { if (sh < (int)sizeof s->shape - 3) s->shape[sh++] = (c); } while (0)
    while (p < e) {
        int c = (unsigned char)*p;
        if (isspace(c)) { p++; continue; }
        if (c == ',' || c == '[' || c == ']' || (inbr && c == '+')) {
            if (c == '[') inbr = 1;
            if (c == ']') inbr = 0;
            PUT((char)c);
            p++;
            /* `[A+c]`, `[A+r]`, `[A+i]` */
            if (c == '+' && p < e && (*p == 'c' || *p == 'r' || *p == 'i') &&
                p + 1 < e && p[1] == ']') {
                PUT(*p);
                p++;
            }
            continue;
        }
        if (s->nat == MAXAT) {
            snprintf(err, errlen, "TriCore asm: too many operands");
            return -1;
        }
        {
            struct atom *a = &s->at[s->nat];
            const char *w = p;
            int f, r, n;
            if (c == '%' || isalpha(c)) {
                /* a register, perhaps with a half's suffix */
                const char *x = c == '%' ? p + 1 : p;
                while (x < e && isalnum((unsigned char)*x)) x++;
                n = (int)(x - p);
                r = tcasm_reg(p, n, &f);
                if (r < 0 && n > 2) {
                    /* d3l, d3uu ... */
                    int m = n;
                    while (m > 0 && (p[m - 1] == 'l' || p[m - 1] == 'u'))
                        m--;
                    if (m < n && n - m <= 2) {
                        r = tcasm_reg(p, m, &f);
                        if (r >= 0 && f != 'd')
                            r = -1;
                    }
                    if (r >= 0) {
                        a->kind = 'D';
                        a->v = r;
                        PUT('D');
                        for (int i = m; i < n; i++)
                            PUT(p[i]);
                        a->p = p; a->n = n;
                        s->nat++;
                        p = x;
                        continue;
                    }
                }
                if (r >= 0) {
                    a->kind = (char)toupper(f);
                    a->v = r;
                    a->p = p; a->n = n;
                    PUT(a->kind);
                    s->nat++;
                    p = x;
                    continue;
                }
                if (is_sfr_mnem(s->mn) && sfr_name(p, n, &a->v) &&
                    (x == e || *x == ',' || isspace((unsigned char)*x))) {
                    a->kind = 'S';
                    a->p = p; a->n = n;
                    PUT('$'); PUT('S');
                    s->nat++;
                    p = x;
                    continue;
                }
            }
            if (c == '$') {
                const char *x = p + 1;
                char t;
                while (x < e && *x != ',' && !isspace((unsigned char)*x)) x++;
                if (!sfr_name(p, (int)(x - p), &a->v) &&
                    !value(p + 1, x, &a->v, &t)) {
                    snprintf(err, errlen, "TriCore asm: '%.*s' names no core "
                             "special function register", (int)(x - p), p);
                    return -1;
                }
                a->kind = 'S';
                a->p = p; a->n = (int)(x - p);
                PUT('$'); PUT('S');
                s->nat++;
                p = x;
                continue;
            }
            /* an expression, to the next top-level comma */
            {
                int depth = 0;
                const char *x = p;
                while (x < e && (depth || *x != ',')) {
                    if (*x == '(') depth++;
                    if (*x == ')') depth--;
                    if (*x == '[' && !depth) break;
                    x++;
                }
                a->kind = 'N';
                a->p = w;
                a->n = (int)(x - w);
                while (a->n && isspace((unsigned char)w[a->n - 1])) a->n--;
                if (!value(w, x, &a->v, &a->tgt)) {
                    if (!syms) {
                        snprintf(err, errlen, "TriCore asm: '%.*s' is not a "
                                 "constant", a->n, w);
                        return -1;
                    }
                    a->tgt = 2;             /* a symbol's */
                }
                /* mfcr/mtcr: a number is the register's */
                if (is_sfr_mnem(s->mn) && a->tgt == 0) {
                    a->kind = 'S';
                    PUT('$'); PUT('S');
                } else {
                    PUT('N');
                }
                s->nat++;
                p = x;
            }
        }
    }
#undef PUT
    s->shape[sh] = 0;
    return 0;
}

/* ---- encoding ----------------------------------------------------------- */

/* Field f holding v: its bits ORed into *w, or 0 when v does not fit or
 * is not one GNU takes there. */
static int fit_field(const struct tc_field *f, long long v, unsigned *w)
{
    long long x = v - f->v0;
    unsigned bits = 0;
    const struct tc_fbit *b = &tc_fbits[f->fb];
    if (f->rule == 1 && (v & 1))
        return 0;
    if (f->rule == 2) {
        int ok = 0;
        for (int k = 0; k < f->nok && !ok; k++)
            ok = tc_oks[f->ok + k] == v;
        if (!ok)
            return 0;
    }
    for (int k = 0; k < f->nb; k++)
        if (b[k].neg && x < 0) {
            x += 1LL << b[k].shift;
            bits |= 1u << b[k].pos;
        }
    for (int k = 0; k < f->nb; k++)
        if (!b[k].neg && x > 0 && ((x >> b[k].shift) & 1)) {
            x -= 1LL << b[k].shift;
            bits |= 1u << b[k].pos;
        }
    if (x != 0)
        return 0;
    *w |= bits;
    return 1;
}

/* Entry i holding atoms at[0..n): its word, or 0 (and *ok 0). */
static int fit_insn(int i, const struct atom *at, int n, unsigned *word)
{
    const struct tc_insn *e = &tc_insns[i];
    unsigned w = e->base;
    int nf = e->nf;
    /* (a mirror -- kind M, last -- writes field v0's value again at bit fb:
     * GNU puts updfl's register in two places) */
    while (nf && tc_fields[e->f + nf - 1].kind == 'M') {
        const struct tc_field *m = &tc_fields[e->f + nf - 1];
        if (m->v0 >= n)
            return 0;
        w |= (unsigned)at[m->v0].v << m->fb;
        nf--;
    }
    if (nf != n)
        return 0;
    for (int k = 0; k < n; k++) {
        const struct tc_field *f = &tc_fields[e->f + k];
        char want = f->kind == 'L' ? 'N' : f->kind;
        if (at[k].kind != want)
            return 0;
        if ((f->kind == 'L') != (at[k].tgt == 1))
            return 0;
        if (!fit_field(f, at[k].v, &w))
            return 0;
    }
    *word = w;
    return 1;
}

static int has_target(const struct stmt *s)
{
    for (int k = 0; k < s->nat; k++)
        if (s->at[k].tgt == 1)
            return 1;
    return 0;
}

/* The best entry of mnemonic mn with shape sh for these atoms: GNU's
 * choice among those of length `len` (0 any: the shortest; -1 a branch's:
 * 4 bytes first). -1 when none fits. */
static int choose(const char *mn, const char *sh, const struct atom *at,
                  int n, int len, unsigned *word)
{
    int i0 = first_of(mn);
    int order[2] = { 2, 4 };
    if (i0 < 0)
        return -1;
    if (len == -1) {
        order[0] = 4;
        order[1] = 2;
    } else if (len) {
        order[0] = order[1] = len;
    }
    for (int pass = 0; pass < 2; pass++)
        for (int i = i0; i < NINSN && !strcmp(tc_insns[i].mnem, mn); i++) {
            if (tc_insns[i].len != order[pass] || strcmp(tc_insns[i].shape, sh))
                continue;
            if (fit_insn(i, at, n, word))
                return i;
        }
    return -1;
}

static int mnem_known(const char *mn) { return first_of(mn) >= 0; }

static int shape_known(const char *mn, const char *sh)
{
    int i0 = first_of(mn);
    for (int i = i0; i0 >= 0 && i < NINSN && !strcmp(tc_insns[i].mnem, mn); i++)
        if (!strcmp(tc_insns[i].shape, sh))
            return 1;
    return 0;
}

/* The two-operand shape of a three-operand one, without operand `drop`
 * (1 or 2): "D,D,N" less operand 1 is "D,N". */
static void drop_operand(const struct stmt *s, int drop, char *out, int cap)
{
    const char *p = s->shape;
    int k = 0, o = 0;
    /* operands are separated by top-level commas in shape */
    while (*p && o < cap - 1) {
        const char *q = p;
        while (*q && *q != ',') q++;
        if (k != drop) {
            if (o && o < cap - 1) out[o++] = ',';
            while (p < q && o < cap - 1) out[o++] = *p++;
        }
        p = *q ? q + 1 : q;
        k++;
    }
    out[o] = 0;
}

static const struct tc_narrow *narrow_of(const char *mn, const char *sh)
{
    for (int k = 0; k < NNARROW; k++)
        if (!strcmp(tc_narrows[k].mnem, mn) && !strcmp(tc_narrows[k].shape, sh))
            return &tc_narrows[k];
    return NULL;
}

/* A branch to a label whose mnemonic has no 32-bit form: the one GNU
 * writes (`jz d2, L` is `jeq d2, 0, L`). */
static const struct { const char *from, *to; int k; } longer[] = {
    { "jz", "jeq", 0 }, { "jnz", "jne", 0 }, { "jgez", "jge", 0 },
    { "jltz", "jlt", 0 }, { "jgtz", "jge", 1 }, { "jlez", "jlt", 1 },
};

/* The inverse of a conditional branch, or NULL. */
static const char *inverse(const char *mn)
{
    static const char *const pairs[] = {
        "jeq", "jne", "jlt", "jge", "jlt.u", "jge.u", "jeq.a", "jne.a",
        "jz", "jnz", "jz.a", "jnz.a", "jz.t", "jnz.t", "jgez", "jltz",
        "jgtz", "jlez", NULL
    };
    for (int k = 0; pairs[k]; k++)
        if (!strcmp(pairs[k], mn))
            return pairs[k ^ 1];
    return NULL;
}

static void put(struct code *c, unsigned w, int len)
{
    if (len == 2)
        code_u16(c, w & 0xffff);
    else
        code_u32(c, w);
}

/* state across statements: .code16/.code32, and relaxation's level */
static int s_force, s_lvl, s_took;

void tcasm_set_level(int level) { s_lvl = level; s_took = 0; }
int tcasm_took_level(void) { return s_took; }
void tcasm_reset(void) { s_force = 0; }
void tcasm_width(int bits) { s_force = bits / 8; }

/* The branch at s (its target the last atom) as GNU's long sequence:
 * the inverse condition over a `j` -- or loop's. *jat is where the j
 * starts. 0, or -1 with err. */
static int long_branch(const struct stmt *s, struct code *c, int *jat,
                       int zero_j, char *err, size_t errlen)
{
    struct atom at[MAXAT];
    long long disp = s->at[s->nat - 1].v;
    unsigned w, wj;
    int i, len, ij;
    struct atom j[1];
    memcpy(at, s->at, sizeof at);
    if (!strcmp(s->mn, "j") || !strcmp(s->mn, "jl") || !strcmp(s->mn, "call") ||
        !strcmp(s->mn, "fcall")) {
        if (disp & 1)
            snprintf(err, errlen, "TriCore asm: '%s' to .%+lld: not an even "
                     "distance", s->mn, disp);
        else
            snprintf(err, errlen, "TriCore asm: '%s' to .%+lld is beyond its "
                     "reach (+-16 MiB)", s->mn, disp);
        return -1;
    }
    if (!inverse(s->mn)) {
        /* loop, jned, jnei (no inverse): the branch to a `j L` just
         * after a 16-bit `j` over it -- `loop a2, .+6; j .+6; j L` */
        at[s->nat - 1].v = 6;
        i = choose(s->mn, s->shape, at, s->nat, 4, &w);
        j[0] = s->at[s->nat - 1];
        j[0].v = 6;
        ij = choose("j", "N", j, 1, 2, &wj);
        if (i < 0 || ij < 0)
            goto bad;
        put(c, w, 4);
        put(c, wj, 2);
        len = 6;
    } else {
        const char *inv = inverse(s->mn);
        at[s->nat - 1].v = 6;
        i = -1;
        /* jne d, 0 is also jnz d -- 16-bit for any register (GNU's) */
        if (!strcmp(s->shape, "D,N,N") && (at[1].v == 0 || at[1].v == 1))
            for (unsigned k = 0; k < sizeof longer / sizeof longer[0]; k++)
                if (!strcmp(longer[k].to, inv) && longer[k].k == at[1].v) {
                    struct atom two[2] = { at[0], at[2] };
                    i = choose(longer[k].from, "D,N", two, 2, 2, &w);
                    if (i >= 0)
                        break;
                }
        if (i < 0)
            i = choose(inv, s->shape, at, s->nat, 2, &w);
        len = 2;
        if (i < 0) {
            at[s->nat - 1].v = 8;
            i = choose(inv, s->shape, at, s->nat, 4, &w);
            len = 4;
        }
        if (i < 0) {
            /* jz d2 inverted is jnz d2 -- 16-bit only for some registers */
            for (unsigned k = 0; k < sizeof longer / sizeof longer[0]; k++)
                if (!strcmp(longer[k].from, inv) && s->nat == 2) {
                    struct atom a3[3] = { at[0], at[1], at[1] };
                    char sh[64];
                    a3[1].kind = 'N'; a3[1].tgt = 0; a3[1].v = longer[k].k;
                    a3[2].v = 8;
                    snprintf(sh, sizeof sh, "%s,N,N", s->shape[0] == 'A' ? "A" : "D");
                    i = choose(longer[k].to, sh, a3, 3, 4, &w);
                    len = 4;
                    break;
                }
        }
        if (i < 0)
            goto bad;
        put(c, w, len);
    }
    *jat = len;
    j[0] = s->at[s->nat - 1];
    j[0].v = zero_j ? 0 : disp - len;
    ij = choose("j", "N", j, 1, 4, &wj);
    if (ij < 0) {
        snprintf(err, errlen, "TriCore asm: '%s' to .%+lld is beyond even "
                 "a j's reach (+-16 MiB)", s->mn, disp);
        return -1;
    }
    put(c, wj, 4);
    s_took = 1;
    return 0;
bad:
    snprintf(err, errlen, "TriCore asm: '%s %s' has no long form", s->mn,
             s->shape);
    return -1;
}

static int is_transfer_mnem(const char *mn)
{
    if (!strcmp(mn, "ja") || !strcmp(mn, "jla") || !strcmp(mn, "calla") ||
        !strcmp(mn, "fcalla") || !strcmp(mn, "ji") || !strcmp(mn, "jli") ||
        !strcmp(mn, "calli") || !strcmp(mn, "fcalli"))
        return 0;
    return mn[0] == 'j' || !strncmp(mn, "loop", 4) || !strcmp(mn, "call") ||
           !strcmp(mn, "fcall");
}

/* Why no form of s's mnemonic and shape takes its operands: the operand
 * that fails in the form that takes the most of them, and why. */
static void diagnose(const struct stmt *s, char *err, size_t errlen)
{
    int i0 = first_of(s->mn), best = -1, bk = -1;
    for (int i = i0; i0 >= 0 && i < NINSN && !strcmp(tc_insns[i].mnem, s->mn); i++) {
        unsigned w = 0;
        int k;
        if (strcmp(tc_insns[i].shape, s->shape))
            continue;
        for (k = 0; k < s->nat; k++) {
            const struct tc_field *f = &tc_fields[tc_insns[i].f + k];
            if (f->kind == 'M' || !fit_field(f, s->at[k].v, &w) ||
                (f->kind == 'L') != (s->at[k].tgt == 1))
                break;
        }
        /* (the widest field of the operand that fails, at one failing
         * position: its range is the one to report) */
        if (k > bk || (k == bk && best >= 0 &&
                       tc_fields[tc_insns[i].f + k].nb >
                       tc_fields[tc_insns[best].f + k].nb)) {
            bk = k;
            best = i;
        }
    }
    if (best < 0 || bk >= s->nat) {
        snprintf(err, errlen, "TriCore asm: '%s %s': no form takes these "
                 "operands", s->mn, s->shape);
        return;
    }
    {
        const struct tc_field *f = &tc_fields[tc_insns[best].f + bk];
        const struct tc_fbit *b = &tc_fbits[f->fb];
        const struct atom *a = &s->at[bk];
        long long lo = f->v0, hi = f->v0, step = 0;
        for (int j = 0; j < f->nb; j++) {
            long long wt = 1LL << b[j].shift;
            if (b[j].neg) lo -= wt; else hi += wt;
            if (!step || wt < step) step = wt;
        }
        if (f->kind == 'L' && a->tgt != 1)
            snprintf(err, errlen, "TriCore asm: '%s' takes a label (or .+N) "
                     "as operand %d, not '%.*s'", s->mn, bk + 1, a->n, a->p);
        else if (f->kind != 'L' && a->tgt == 1)
            snprintf(err, errlen, "TriCore asm: operand %d of '%s' is a "
                     "number, not a target", bk + 1, s->mn);
        else if (f->rule == 1 && (a->v & 1))
            snprintf(err, errlen, "TriCore asm: '%.*s': operand %d of '%s' "
                     "is an even register (a pair's first)", a->n, a->p,
                     bk + 1, s->mn);
        else if (f->rule == 2) {
            char vs[64];
            int o = 0;
            for (int j = 0; j < f->nok && o < 48; j++)
                o += snprintf(vs + o, sizeof vs - (size_t)o, "%s%d",
                              j ? " or " : "", tc_oks[f->ok + j]);
            snprintf(err, errlen, "TriCore asm: operand %d of '%s' is %s "
                     "here, not %lld", bk + 1, s->mn, vs, a->v);
        } else if (f->kind == 'L' && step > 1 && a->v % step)
            snprintf(err, errlen, "TriCore asm: '%s' to .%+lld: not an even "
                     "distance", s->mn, a->v);
        else if (f->kind == 'L')
            snprintf(err, errlen, "TriCore asm: '%s' to .%+lld is beyond its "
                     "reach (%+lld..%+lld)", s->mn, a->v, lo, hi);
        else if (f->kind == 'N' || f->kind == 'S')
            snprintf(err, errlen, "TriCore asm: %lld does not fit operand %d "
                     "of '%s %s': its %d bits hold %lld..%lld%s", a->v, bk + 1,
                     s->mn, s->shape, f->nb, lo, hi,
                     step > 1 ? (step == 2 ? ", even" : ", a multiple of 4")
                              : "");
        else
            snprintf(err, errlen, "TriCore asm: '%.*s' is not one operand %d "
                     "of '%s' can name", a->n, a->p, bk + 1, s->mn);
    }
}

/* One parsed statement into c. `entry` >= 0 forces that form (symform's
 * choice), -1 chooses; `want_long` forces the long branch. */
static int encode_stmt(struct stmt *s, struct code *c, int entry,
                       int want_long, char *err, size_t errlen)
{
    unsigned w = 0;
    int i, len = 0, branch = has_target(s), force = s_force;
    s_force = 0;
    if (entry >= 0) {
        if (!fit_insn(entry, s->at, s->nat, &w)) {
            snprintf(err, errlen, "TriCore asm: '%s %s' does not fit its "
                     "relocated form", s->mn, s->shape);
            return -1;
        }
        put(c, w, tc_insns[entry].len);
        return 0;
    }
    if (!mnem_known(s->mn)) {
        snprintf(err, errlen, "TriCore asm: '%s' is not an instruction", s->mn);
        return -1;
    }
    if (want_long || (branch && s_lvl >= 1 && !force)) {
        int jat;
        return long_branch(s, c, &jat, want_long, err, errlen);
    }
    i = choose(s->mn, s->shape, s->at, s->nat, force ? force : branch ? -1 : 0, &w);
    if (i >= 0)
        len = tc_insns[i].len;
    /* `sat.b d5, d5` as one operand */
    if (s->nat == 2 && !force && !branch && (i < 0 || len == 4) &&
        s->at[0].kind == s->at[1].kind && s->at[0].v == s->at[1].v &&
        s->at[0].kind != 'N') {
        const struct tc_narrow *nw = narrow_of(s->mn, s->shape);
        char sh1[64];
        unsigned w1;
        int i1;
        drop_operand(s, 1, sh1, sizeof sh1);
        if (nw && nw->a && (i1 = choose(s->mn, sh1, s->at, 1, 2, &w1)) >= 0) {
            i = i1;
            w = w1;
            len = 2;
        }
    }
    /* cadd/caddn d, d15, d, k: the 16-bit cadd d, d15, k */
    if (s->nat == 4 && !force && !branch && (i < 0 || len == 4) &&
        (!strcmp(s->mn, "cadd") || !strcmp(s->mn, "caddn")) &&
        s->at[2].kind == s->at[0].kind && s->at[2].v == s->at[0].v) {
        struct atom three[3] = { s->at[0], s->at[1], s->at[3] };
        char sh3[64];
        unsigned w3;
        int i3;
        drop_operand(s, 2, sh3, sizeof sh3);
        if ((i3 = choose(s->mn, sh3, three, 3, 2, &w3)) >= 0) {
            i = i3;
            w = w3;
            len = 2;
        }
    }
    /* lea a, [a]k: the 16-bit sub.a sp, -k or add.a a, k */
    if (s->nat == 3 && !force && !branch && (i < 0 || len == 4) &&
        !strcmp(s->mn, "lea") && !strcmp(s->shape, "A,[A]N") &&
        s->at[0].v == s->at[1].v) {
        struct atom two[2] = { s->at[0], s->at[2] };
        unsigned w2;
        int i2;
        two[1].v = -s->at[2].v;
        i2 = choose("sub.a", "A,N", two, 2, 2, &w2);
        if (i2 < 0) {
            two[1].v = s->at[2].v;
            i2 = choose("add.a", "A,N", two, 2, 2, &w2);
        }
        if (i2 >= 0) {
            i = i2;
            w = w2;
            len = 2;
        }
    }
    /* sel/seln whose destination is one of the values, on d15: cmov or
     * cmovn (GNU's 16-bit spelling of it) */
    if (s->nat == 4 && !force && !branch && (i < 0 || len == 4) &&
        (!strcmp(s->mn, "sel") || !strcmp(s->mn, "seln")) &&
        (!strcmp(s->shape, "D,D,D,D") || !strcmp(s->shape, "D,D,D,N"))) {
        int imm = s->shape[6] == 'N', same = imm ? 2 : 3;
        if (s->at[same].kind == 'D' && s->at[0].v == s->at[same].v) {
            struct atom three[3] = { s->at[0], s->at[1], s->at[imm ? 3 : 2] };
            /* d = c ? x : d is cmov; d = c ? d : k is cmovn; seln the other */
            const char *to = (s->mn[3] == 'n') == imm ? "cmov" : "cmovn";
            unsigned w3;
            int i3 = choose(to, imm ? "D,D,N" : "D,D,D", three, 3, 2, &w3);
            if (i3 >= 0) {
                i = i3;
                w = w3;
                len = 2;
            }
        }
    }
    /* a 3-operand form whose destination is a source, as two */
    if (s->nat == 3 && !force && !branch && (i < 0 || len == 4)) {
        const struct tc_narrow *nw = narrow_of(s->mn, s->shape);
        /* (dst = dst op x is always the 2-operand form's meaning; dst = x
         * op dst only where op commutes, which tc_narrows says) */
        if (s->at[0].kind == s->at[1].kind) {
            struct atom two[2];
            char sh2[64];
            unsigned w2;
            int i2 = -1;
            if (s->at[0].v == s->at[1].v) {
                two[0] = s->at[0];
                two[1] = s->at[2];
                drop_operand(s, 1, sh2, sizeof sh2);
                i2 = choose(s->mn, sh2, two, 2, 2, &w2);
            }
            if (i2 < 0 && nw && nw->b && s->at[2].kind == s->at[0].kind &&
                s->at[0].v == s->at[2].v) {
                two[0] = s->at[0];
                two[1] = s->at[1];
                drop_operand(s, 2, sh2, sizeof sh2);
                i2 = choose(s->mn, sh2, two, 2, 2, &w2);
            }
            if (i2 >= 0) {
                i = i2;
                w = w2;
                len = 2;
            }
        }
    }
    /* a branch to a label, 32-bit: jz d2, L is jeq d2, 0, L */
    if (branch && !force && (i < 0 || len == 2) && s->nat == 2)
        for (unsigned k = 0; k < sizeof longer / sizeof longer[0]; k++)
            if (!strcmp(longer[k].from, s->mn)) {
                struct atom a3[3] = { s->at[0], s->at[1], s->at[1] };
                unsigned w3;
                int i3;
                a3[1].kind = 'N'; a3[1].tgt = 0; a3[1].v = longer[k].k;
                i3 = choose(longer[k].to, "D,N,N", a3, 3, 4, &w3);
                if (i3 >= 0) {
                    i = i3;
                    w = w3;
                    len = 4;
                }
                break;
            }
    /* `[A]0` is also `[A]`, and `[A+]4` of a word `[A+]`: the form
     * without the offset, when it is the shorter (GNU's choice; at one
     * length GNU keeps the offset's) */
    if (!branch) {
        for (int k = 0; k < s->nat; k++) {
            struct stmt t;
            char *q;
            int i2, n2 = 0;
            long long want = 0;
            unsigned w2;
            if (s->at[k].kind != 'N' || s->at[k].tgt)
                continue;
            /* the atom must be an offset: right after `]` in the shape */
            q = s->shape;
            for (int a = 0; *q; q++)
                if (*q == 'D' || *q == 'A' || *q == 'E' || *q == 'N' ||
                    (*q == '$' && q[1] == 'S')) {
                    if (*q == 'D')
                        while (q[1] == 'l' || q[1] == 'u') q++;
                    if (*q == '$') q++;
                    if (a++ == k)
                        break;
                }
            if (!*q || *q != 'N' || q == s->shape || q[-1] != ']')
                continue;
            if (q - s->shape >= 2 && q[-2] == '+') {
                /* post-increment by the access's size */
                const char *sfx = strrchr(s->mn, '.');
                want = !sfx ? -1 : !strcmp(sfx, ".b") || !strcmp(sfx, ".bu") ? 1 :
                       !strcmp(sfx, ".h") || !strcmp(sfx, ".hu") ? 2 :
                       !strcmp(sfx, ".w") || !strcmp(sfx, ".a") ? 4 : -1;
            } else if (q - s->shape >= 2 && q[-2] != 'A') {
                break;                      /* [+A], [A+c]: no such form */
            }
            if (s->at[k].v != want)
                break;
            t = *s;
            memmove(q - s->shape + t.shape, q - s->shape + t.shape + 1,
                    strlen(q + 1) + 1);
            for (int a = k; a + 1 < s->nat; a++)
                t.at[a] = s->at[a + 1];
            t.nat--;
            i2 = choose(t.mn, t.shape, t.at, t.nat, force, &w2);
            if (i2 >= 0)
                n2 = tc_insns[i2].len;
            if (i2 >= 0 && (i < 0 || n2 < len)) {
                i = i2;
                w = w2;
                len = n2;
            }
            break;
        }
    }
    if (i >= 0) {
        put(c, w, len);
        return 0;
    }
    if (branch && !force && shape_known(s->mn, s->shape) &&
        is_transfer_mnem(s->mn) && !(s->at[s->nat - 1].v & 1)) {
        /* out of reach: the long form (a later pass keeps it) */
        int jat;
        unsigned wn;
        /* ...unless the operands are what does not fit */
        struct stmt t = *s;
        t.at[t.nat - 1].v = 0;
        if (choose(t.mn, t.shape, t.at, t.nat, 0, &wn) >= 0 ||
            !strcmp(t.mn, "jz") || !strcmp(t.mn, "jnz") ||
            !strcmp(t.mn, "jgez") || !strcmp(t.mn, "jltz") ||
            !strcmp(t.mn, "jgtz") || !strcmp(t.mn, "jlez"))
            return long_branch(s, c, &jat, 0, err, errlen);
    }
    if (!shape_known(s->mn, s->shape)) {
        char forms[160];
        int o = 0, i0 = first_of(s->mn);
        const char *last = "";
        forms[0] = 0;
        for (int k = i0; k < NINSN && !strcmp(tc_insns[k].mnem, s->mn) &&
                         o < (int)sizeof forms - 24; k++)
            if (strcmp(tc_insns[k].shape, last) && !strchr(tc_insns[k].shape, '<')) {
                o += snprintf(forms + o, sizeof forms - (size_t)o, "%s'%s'",
                              o ? ", " : "", tc_insns[k].shape);
                last = tc_insns[k].shape;
            }
        snprintf(err, errlen, "TriCore asm: '%s' takes no operands shaped "
                 "'%s' (D/A/E a register, N a number); its forms: %s",
                 s->mn, s->shape, forms);
        return -1;
    }
    if (force)
        snprintf(err, errlen, "TriCore asm: '%s %s' has no %d-bit form "
                 "these operands fit (.code%d)", s->mn, s->shape, force * 8,
                 force * 8);
    else
        diagnose(s, err, errlen);
    return -1;
}

/* One statement's text. A refusal tcasm_symform wrote ("\001why"); a
 * form it chose ("\002N\002text": entry N; "\003text": the long branch). */
static int stmt_text(const char *p, const char *e, struct code *c, char *err,
                     size_t errlen)
{
    struct stmt s;
    int entry = -1, want_long = 0;
    while (p < e && isspace((unsigned char)*p)) p++;
    if (p == e)
        return 0;
    if (*p == '\001') {
        snprintf(err, errlen, "%.*s", (int)(e - p - 1), p + 1);
        return -1;
    }
    if (*p == '\002') {
        entry = atoi(p + 1);
        p = memchr(p + 1, '\002', (size_t)(e - p - 1));
        if (!p)
            return -1;
        p++;
    } else if (*p == '\003') {
        want_long = 1;
        p++;
    }
    if (parse(p, e, &s, 0, err, errlen))
        return -1;
    return encode_stmt(&s, c, entry, want_long, err, errlen);
}

/* ---- a template ----------------------------------------------------------
 *
 * Its statements, with GCC's numeric labels resolved: `1:` defines the
 * next instance of label 1, `1b` is the latest one at or before the
 * statement and `1f` the next one after it. The statements are measured
 * with every reference at `.+0`, then again at the distances that gives
 * until no length changes, then assembled. */
#define MAXSTMT 256
struct tstm { const char *p, *e; int lab[4], nlab; long off; };

static int split_stmts(const char *text, struct tstm *st, char *err,
                       size_t errlen)
{
    int ns = 0;
    const char *p = text;
    while (*p) {
        const char *e = p;
        while (*e && *e != '\n' && *e != ';')
            e++;
        if (ns == MAXSTMT) {
            snprintf(err, errlen, "TriCore asm: a template of more than %d "
                     "statements", MAXSTMT);
            return -1;
        }
        st[ns].nlab = 0;
        for (;;) {
            const char *q = p, *k;
            while (q < e && isspace((unsigned char)*q)) q++;
            k = q;
            while (k < e && (isalnum((unsigned char)*k) || *k == '_' ||
                             *k == '.'))
                k++;
            if (k == q || k >= e || *k != ':')
                break;
            for (const char *d = q; d < k; d++)
                if (!isdigit((unsigned char)*d)) {
                    snprintf(err, errlen, "TriCore asm: label '%.*s' in a "
                             "template: a template's labels are numeric (1:, "
                             "used as 1b or 1f), since one template may be "
                             "emitted more than once", (int)(k - q), q);
                    return -1;
                }
            if (st[ns].nlab == 4) {
                snprintf(err, errlen, "TriCore asm: more than four labels on "
                         "one statement");
                return -1;
            }
            st[ns].lab[st[ns].nlab++] = atoi(q);
            p = k + 1;
        }
        st[ns].p = p;
        st[ns].e = e;
        st[ns].off = 0;
        ns++;
        p = *e ? e + 1 : e;
    }
    return ns;
}

/* Statement k with each `Nb`/`Nf` written as `.+D` (D 0 when `measure`). */
static int resolve(const struct tstm *st, int ns, int k, int measure,
                   char *buf, int cap, char *err, size_t errlen)
{
    const char *s = st[k].p;
    int len = (int)(st[k].e - st[k].p), o = 0;
    for (int i = 0; i < len; ) {
        int j = i;
        long target = -1;
        int mid = i > 0 && (isalnum((unsigned char)s[i - 1]) ||
                            s[i - 1] == '_' || s[i - 1] == '.');
        while (j < len && isdigit((unsigned char)s[j]))
            j++;
        if (!mid && j > i && j < len && (s[j] == 'b' || s[j] == 'f') &&
            (j + 1 >= len || !(isalnum((unsigned char)s[j + 1]) ||
                               s[j + 1] == '_'))) {
            int lab = atoi(s + i), fwd = s[j] == 'f';
            for (int q = fwd ? k + 1 : k; q >= 0 && q < ns && target < 0;
                 q += fwd ? 1 : -1)
                for (int m = 0; m < st[q].nlab; m++)
                    if (st[q].lab[m] == lab)
                        target = st[q].off;
            if (target < 0) {
                snprintf(err, errlen, "TriCore asm: '%.*s' refers to no label "
                         "%d %s it in the template", j + 1 - i, s + i, lab,
                         fwd ? "after" : "before");
                return -1;
            }
            o += snprintf(buf + o, (size_t)(cap - o), ".%+ld",
                          measure ? 0L : target - st[k].off);
            i = j + 1;
        } else {
            if (j == i)
                j = i + 1;
            while (i < j && o < cap - 24)
                buf[o++] = s[i++];
        }
        if (o >= cap - 24) {
            snprintf(err, errlen, "TriCore asm: a statement too long");
            return -1;
        }
    }
    buf[o] = 0;
    return o;
}

int tcasm_assemble(const char *text, struct code *c, char *err, size_t errlen)
{
    static struct tstm st[MAXSTMT];
    int ns = split_stmts(text, st, err, errlen);
    if (ns < 0)
        return -1;
    s_lvl = 0;
    for (int round = 0; round < 6; round++) {
        long off = 0;
        int moved = 0;
        for (int k = 0; k < ns; k++) {
            char buf[512], e2[200];
            int len = resolve(st, ns, k, round == 0, buf, (int)sizeof buf, err,
                              errlen);
            struct code tmp = { 0 };
            if (len < 0)
                return -1;
            if (st[k].off != off)
                moved = 1;
            st[k].off = off;
            /* a statement that does not encode yet is one 32-bit one */
            if (stmt_text(buf, buf + len, &tmp, e2, sizeof e2) == 0)
                off += tmp.len;
            else
                off += 4;
            free(tmp.p);
        }
        if (round && !moved)
            break;
    }
    for (int k = 0; k < ns; k++) {
        char buf[512];
        int len = resolve(st, ns, k, 0, buf, (int)sizeof buf, err, errlen);
        if (len < 0 || stmt_text(buf, buf + len, c, err, errlen))
            return -1;
    }
    return 0;
}

/* ---- for the file assembler (src/as/gas.c) ---------------------------- */

int tcasm_encode(const char *text, struct code *c, char *err, int errlen)
{
    size_t n = errlen > 0 ? (size_t)errlen : 1;
    return stmt_text(text, text + strlen(text), c, err, n);
}

/* (and a data register's half: d3l, d3uu) */
int tcasm_is_reg(const char *name, int len)
{
    int f, m = len;
    while (m > 2 && (name[m - 1] == 'l' || name[m - 1] == 'u'))
        m--;
    if (m < len && (len - m > 2 || tcasm_reg(name, m, &f) < 0 || f != 'd'))
        return -1;
    return tcasm_reg(name, m, &f);
}

/* `hi`/`lo`/`up`/`sm` before a `:`, or `hi`/`lo` after a `%`, is an
 * operator; the c, r or i of an addressing mode is the mode's; a core
 * register's name is an operand of mtcr/mfcr. */
int tcasm_is_word(const char *stmt, const char *w, int len)
{
    const char *p = stmt;
    char mn[8];
    int k = 0;
    long long v;
    /* [A+c], [A+r], [A+i]: circular, bit-reverse, indexed */
    if (len == 1 && (*w == 'c' || *w == 'r' || *w == 'i') && w > stmt &&
        w[-1] == '+' && w[1] == ']')
        return 1;
    if (len == 2 && (!strncmp(w, "hi", 2) || !strncmp(w, "lo", 2) ||
                     !strncmp(w, "up", 2) || !strncmp(w, "sm", 2)) &&
        (w[2] == ':' || (w > stmt && w[-1] == '%')))
        return 1;
    while (isspace((unsigned char)*p)) p++;
    while (*p && !isspace((unsigned char)*p) && k < 7)
        mn[k++] = (char)tolower((unsigned char)*p++);
    mn[k] = 0;
    return is_sfr_mnem(mn) && sfr_name(w, len, &v);
}

int tcasm_is_transfer(const char *stmt)
{
    char mn[24];
    int k = 0;
    while (isspace((unsigned char)*stmt)) stmt++;
    while (*stmt && !isspace((unsigned char)*stmt) && k < 23)
        mn[k++] = (char)tolower((unsigned char)*stmt++);
    mn[k] = 0;
    return is_transfer_mnem(mn);
}

static int refuse_form(struct asm_symform *f, const char *stmt,
                       const char *word, int wlen, const char *fmt,
                       const char *a, const char *b)
{
    char why[200];
    f->sym_at = (int)(word - stmt);
    f->sym_len = wlen;
    f->addend = 0;
    f->nsites = 0;
    snprintf(why, sizeof why, fmt, a, b);
    snprintf(f->encode, sizeof f->encode, "\001%s", why);
    return 1;
}

/* Entry e's statement from s's operands -- its field j takes operand
 * map[j] -- with operand k (the symbol's) written as zero. */
static void render(char *out, size_t cap, int e, const struct stmt *s,
                   const char *map, int k)
{
    const struct tc_insn *in = &tc_insns[e];
    size_t o = (size_t)snprintf(out, cap, "%s ", in->mnem);
    int j = 0;
    for (const char *p = in->shape; *p && o + 24 < cap; p++) {
        if (*p == 'D' || *p == 'A' || *p == 'E' || *p == 'N' ||
            (*p == '$' && p[1] == 'S')) {
            int a = map[j] ? map[j] - '0' : j;
            if (a == k)
                o += (size_t)snprintf(out + o, cap - o, "%s",
                                      tc_fields[in->f + j].kind == 'L' ? ".+0" : "0");
            else
                o += (size_t)snprintf(out + o, cap - o, "%.*s", s->at[a].n,
                                      s->at[a].p);
            j++;
            if (*p == '$')
                p++;
            if (*p == 'D')
                while (p[1] == 'l' || p[1] == 'u')
                    p++;
        } else {
            out[o++] = *p;
        }
    }
    out[o] = 0;
}

static const char *const opname[] = { "SYM", "hi:SYM", "lo:SYM", "sm:SYM",
                                      "up:SYM", "SYM" };

/* A statement with a symbol in one numeric operand: the form GNU takes
 * for it and the relocation it carries (tc_symrules), with a zero in the
 * symbol's place. */
int tcasm_symform(const char *stmt, struct asm_symform *f)
{
    struct stmt s;
    char err[160];
    int k, op = 0, hl = 0, slen, rule = -1;
    const char *sp, *se, *ae;
    long add = 0;
    memset(f, 0, sizeof *f);
    if (parse(stmt, stmt + strlen(stmt), &s, 1, err, sizeof err))
        return 0;
    for (k = 0; k < s.nat; k++)
        if (s.at[k].kind == 'N' && s.at[k].tgt == 2)
            break;
    if (k == s.nat)
        return 0;
    /* the symbol, its operator, its addend */
    sp = s.at[k].p;
    ae = sp + s.at[k].n;
    if (*sp == '#') sp++;
    op = halfop(sp, ae, &hl);
    sp += hl;
    if (hl == 4) {
        if (ae[-1] != ')')
            return 0;
        ae--;
    }
    if (!asm_sym0((unsigned char)*sp))
        return 0;
    se = sp;
    while (se < ae && asm_symc((unsigned char)*se)) se++;
    slen = (int)(se - sp);
    {
        int fl;
        if (tcasm_reg(sp, slen, &fl) >= 0)
            return 0;
    }
    if (se < ae) {
        long long v;
        if ((*se != '+' && *se != '-') ||
            !asm_const_expr(se, (int)(ae - se), &v))
            return 0;
        add = (long)v;
    }
    for (int r = 0; r < NSYMR; r++)
        if (tc_symrules[r].field == k && !strcmp(tc_symrules[r].mnem, s.mn) &&
            !strcmp(tc_symrules[r].shape, s.shape) &&
            (tc_symrules[r].op == op || (op == 0 && tc_symrules[r].op == 5 &&
                                         rule < 0))) {
            rule = r;
            if (tc_symrules[r].op == op)
                break;
        }
    if (rule < 0) {
        if (!mnem_known(s.mn) || !shape_known(s.mn, s.shape))
            return 0;                       /* refused by the encoder */
        return refuse_form(f, stmt, sp, slen, "TriCore asm: '%s' takes no %s "
                           "in this operand", s.mn, opname[op]);
    }
    if (tc_symrules[rule].entry == -2)
        return refuse_form(f, stmt, sp, slen, "TriCore asm: '%s' takes no %s "
                           "in this operand: no relocation reaches its field "
                           "(GNU as refuses it too)", s.mn, opname[op]);
    f->sym_at = (int)(sp - stmt);
    f->sym_len = slen;
    f->addend = add;
    f->nsites = 1;
    f->site[0].reloc = tc_symrules[rule].rel;
    {
        /* the statement with the operand zeroed: `.+0` for a target */
        const char *a0 = s.at[k].p;
        const char *map = tc_symrules[rule].map;
        int ent = tc_symrules[rule].entry, fk = k, isl, o = 0;
        if (ent >= 0 && map[0])
            for (fk = 0; map[fk] && map[fk] - '0' != k; fk++)
                ;
        isl = ent == -1 || tc_fields[tc_insns[ent].f + fk].kind == 'L';
        if (tc_symrules[rule].entry == -1) {
            /* the inverse over a `j SYM`: the j's offset is the inverse's
             * length, which encoding it tells */
            struct code tmp = { 0 };
            char e2[160];
            int jat = 0;
            struct stmt t = s;
            t.at[k].tgt = 1;
            t.at[k].v = 0;
            if (long_branch(&t, &tmp, &jat, 1, e2, sizeof e2)) {
                free(tmp.p);
                return refuse_form(f, stmt, sp, slen, "%s%s", e2, "");
            }
            free(tmp.p);
            s_took = 0;
            f->site[0].off = jat;
            o = snprintf(f->encode, sizeof f->encode, "\003");
        } else {
            f->site[0].off = tc_symrules[rule].reloff;
            o = snprintf(f->encode, sizeof f->encode, "\002%d\002",
                         tc_symrules[rule].entry);
        }
        if (ent >= 0 && map[0])
            render(f->encode + o, sizeof f->encode - (size_t)o, ent, &s, map, k);
        else
            snprintf(f->encode + o, sizeof f->encode - (size_t)o, "%.*s%s%s",
                     (int)(a0 - stmt), stmt, isl ? ".+0" : "0",
                     s.at[k].p + s.at[k].n);
    }
    return 1;
}
