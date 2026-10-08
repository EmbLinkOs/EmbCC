/* The ColdFire assembler. See asm.h. All this file adds to emit.c is a
 * parser: every mode and range is checked HERE, before the encoder is
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

/* The relaxation level gas.c set for the statement (-1: the optimistic
 * first pass), the form its branch took, and whether the statement is
 * cfasm_symform's (its relocated field, which holds RELOC_MARK while it is
 * encoded, is then zeroed). */
static int g_level;
static int g_took;
static int g_raw;
#define RELOC_MARK 0x7e5a3c1fUL

void cfasm_set_level(int level)
{
    g_level = level;
}

int cfasm_took_level(void)
{
    return g_took;
}

#define FAIL(...)  do { snprintf(err, (size_t)errlen, __VA_ARGS__); \
                        return -1; } while (0)

/* ---- names ----------------------------------------------------------------- */

static int name_is(const char *a, int alen, const char *b)
{
    if ((int)strlen(b) != alen)
        return 0;
    for (int i = 0; i < alen; i++)
        if (tolower((unsigned char)a[i]) != b[i])
            return 0;
    return 1;
}

int cfasm_gpr(const char *name, int len)
{
    if (len > 0 && name[0] == '%') {
        name++;
        len--;
    }
    if (name_is(name, len, "fp"))
        return CF_FP;
    if (name_is(name, len, "sp"))
        return CF_SP;
    if (len == 2 && (tolower((unsigned char)name[0]) == 'd' ||
                     tolower((unsigned char)name[0]) == 'a') &&
        name[1] >= '0' && name[1] <= '7')
        return (tolower((unsigned char)name[0]) == 'a' ? 8 : 0) +
               (name[1] - '0');
    return -1;
}

int cfasm_is_reg(const char *name, int len)
{
    return cfasm_gpr(name, len);
}

/* A word naming a special register: sr, ccr, usp, pc, with or without %. */
static int special(const char *s, int len, const char *what)
{
    if (len > 0 && s[0] == '%') {
        s++;
        len--;
    }
    return name_is(s, len, what);
}

struct nament { const char *name; int v; };

/* movec's control registers on the ColdFire V2 parts. */
static const struct nament ctrl_tab[] = {
    { "cacr", 0x002 }, { "asid", 0x003 }, { "acr0", 0x004 },
    { "acr1", 0x005 }, { "acr2", 0x006 }, { "acr3", 0x007 },
    { "mmubar", 0x008 }, { "vbr", 0x801 }, { "rombar", 0xc00 },
    { "rombar0", 0xc00 }, { "rambar0", 0xc04 }, { "rambar", 0xc05 },
    { "rambar1", 0xc05 }, { "mbar", 0xc0f },
    { NULL, 0 }
};

static int ctrl_reg(const char *s, int len)
{
    if (len > 0 && s[0] == '%') {
        s++;
        len--;
    }
    for (const struct nament *e = ctrl_tab; e->name; e++)
        if (name_is(s, len, e->name))
            return e->v;
    return -1;
}

/* The conditions, in the field's order; `hs`/`lo` are cc/cs. */
static const char *const cond_name[16] = {
    "t", "f", "hi", "ls", "cc", "cs", "ne", "eq",
    "vc", "vs", "pl", "mi", "ge", "lt", "gt", "le"
};

static int cond_of(const char *s)
{
    if (!strcmp(s, "hs")) return CF_CC;
    if (!strcmp(s, "lo")) return CF_CS;
    for (int k = 0; k < 16; k++)
        if (!strcmp(s, cond_name[k]))
            return k;
    return -1;
}

/* ---- operands ----------------------------------------------------------------- */

enum { K_EA, K_LIST, K_SR, K_CCR, K_USP, K_CTRL, K_TGT, K_PAIR };

struct opd {
    int k;
    struct cf_ea ea;
    int imm_ok;          /* an immediate: its value fits a long long */
    long long v;         /* the immediate as written; TGT the distance */
    int pctgt;           /* a (.+N,%pc) operand: N, resolved later */
    unsigned mask;       /* LIST */
    int ctrl;            /* CTRL */
    int r1, r2;          /* PAIR Dr:Dq */
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

/* `.+N`, `.-N` or `.`: the distance N. */
static int target(const char *s, int len, long long *n)
{
    if (len == 1 && s[0] == '.') {
        *n = 0;
        return 1;
    }
    if (len >= 2 && s[0] == '.' && (s[1] == '+' || s[1] == '-'))
        return asm_const_expr(s + 1, len - 1, n);
    return 0;
}

/* An index register: Xn, Xn.l, Xn.w, with *1, *2 or *4 (Motorola), or
 * Xn:l:4 (MIT). Returns the register, the scale in *sc. */
static int index_reg(const char *s, int len, int *sc, char *err, int errlen)
{
    int n = 0, r;
    long long v;
    if (len > 0 && s[0] == '%')
        n = 1;
    while (n < len && isalnum((unsigned char)s[n]))
        n++;
    r = cfasm_gpr(s, n);
    if (r < 0)
        FAIL("\"%.*s\" is not an index register", len, s);
    *sc = 1;
    s += n;
    len -= n;
    if (len >= 2 && (s[0] == '.' || s[0] == ':')) {
        if (tolower((unsigned char)s[1]) == 'w')
            FAIL("a word-sized index register: ColdFire indexes by the whole "
                 "register (.l)");
        if (tolower((unsigned char)s[1]) != 'l')
            FAIL("\"%.*s\" is not an index size", len, s);
        s += 2;
        len -= 2;
    }
    if (len >= 2 && (s[0] == '*' || s[0] == ':')) {
        if (!asm_const_expr(s + 1, len - 1, &v) ||
            (v != 1 && v != 2 && v != 4))
            FAIL("the index scale is 1, 2 or 4 on ColdFire, not \"%.*s\"",
                 len - 1, s + 1);
        *sc = (int)v;
        len = 0;
    }
    if (len)
        FAIL("\"%.*s\" after an index register", len, s);
    return r;
}

/* (disp, base, index): any part may be missing. Returns 0, or -1. */
static int paren_ea(const char *what, const char *dsp, int dl,
                    const char *in, int inlen, struct opd *o, char *err,
                    int errlen)
{
    const char *part[3];
    int plen[3], np = 0, base = -2, xr = -1, sc = 1, i = 0;
    long long d = 0;
    int hasd = 0;
    while (i <= inlen && np < 3) {
        int b = i;
        while (i < inlen && in[i] != ',')
            i++;
        part[np] = in + b;
        plen[np] = trim(&part[np], i - b);
        np++;
        i++;
    }
    if (i <= inlen)
        FAIL("%s: \"%.*s\" has too many parts", what, o->len, o->s);
    for (int k = 0; k < np; k++) {
        int r = cfasm_gpr(part[k], plen[k]);
        if (plen[k] == 0)
            FAIL("%s: \"%.*s\" has an empty part", what, o->len, o->s);
        if (base == -2 && (CF_IS_A(r) || special(part[k], plen[k], "pc"))) {
            base = CF_IS_A(r) ? r : -1;          /* -1: the pc */
            continue;
        }
        if (base != -2 && xr < 0) {
            xr = index_reg(part[k], plen[k], &sc, err, errlen);
            if (xr < 0)
                return -1;
            continue;
        }
        if (k == 0 && !hasd && !dl) {
            long long n;
            if (target(part[k], plen[k], &n)) {
                o->pctgt = 1;
                o->v = n;
                hasd = 1;
                continue;
            }
            if (!asm_const_expr(part[k], plen[k], &d)) {
                if (isalpha((unsigned char)part[k][0]) || part[k][0] == '_')
                    FAIL("%s: \"%.*s\" is a symbol, and inline asm cannot "
                         "reach one: a template carries no relocation (pass "
                         "its address as an \"a\" operand, or write it in a "
                         ".S file)", what, plen[k], part[k]);
                FAIL("%s: \"%.*s\" is not a displacement", what, plen[k],
                     part[k]);
            }
            hasd = 1;
            continue;
        }
        FAIL("%s: \"%.*s\" is not an effective address ColdFire has", what,
             o->len, o->s);
    }
    if (dl) {
        long long n;
        if (target(dsp, dl, &n)) {
            o->pctgt = 1;
            o->v = n;
        } else if (!asm_const_expr(dsp, dl, &d)) {
            if (isalpha((unsigned char)dsp[0]) || dsp[0] == '_')
                FAIL("%s: \"%.*s\" is a symbol, and inline asm cannot reach "
                     "one: a template carries no relocation (pass its "
                     "address as an \"a\" operand, or write it in a .S "
                     "file)", what, dl, dsp);
            FAIL("%s: \"%.*s\" is not a displacement", what, dl, dsp);
        }
        hasd = 1;
    }
    if (base == -2)
        FAIL("%s: \"%.*s\" names no base register (an address register or "
             "%%pc)", what, o->len, o->s);
    memset(&o->ea, 0, sizeof o->ea);
    o->ea.xscale = 1;
    if (base == -1) {                          /* the pc */
        if (xr >= 0) {
            o->ea.mode = CFM_PCIDX;
            o->ea.xreg = xr;
            o->ea.xscale = sc;
        } else {
            o->ea.mode = CFM_PCDISP;
        }
        o->ea.disp = (long)d;
        return 0;
    }
    if (o->pctgt)
        FAIL("%s: a label is reached from %%pc, not from %s", what,
             cf_reg_name(base));
    o->ea.reg = base;
    if (xr >= 0) {
        if (d < -128 || d > 127)
            FAIL("%s: displacement %lld does not fit an indexed mode's 8 "
                 "bits", what, d);
        o->ea.mode = CFM_IDX;
        o->ea.disp = (long)d;
        o->ea.xreg = xr;
        o->ea.xscale = sc;
        return 0;
    }
    if (d < -32768 || d > 32767)
        FAIL("%s: displacement %lld does not fit 16 bits", what, d);
    if (d == 0 && !hasd) {
        o->ea.mode = CFM_IND;
        return 0;
    }
    o->ea.mode = CFM_DISP;
    o->ea.disp = (long)d;
    return 0;
}

static int parse_opd(const char *s, int len, struct opd *o, const char *what,
                     char *err, int errlen)
{
    int r;
    memset(o, 0, sizeof *o);
    len = trim(&s, len);
    o->s = s;
    o->len = len;
    o->k = K_EA;
    if (len == 0)
        FAIL("%s: an empty operand", what);
    if (s[0] == '#') {
        o->ea.mode = CFM_IMM;
        if (!asm_const_expr(s + 1, len - 1, &o->v)) {
            if (isalpha((unsigned char)s[1]) || s[1] == '_')
                FAIL("%s: \"%.*s\" is a symbol, and inline asm cannot take "
                     "one: a template carries no relocation (load it with "
                     "an \"i\" operand, or write it in a .S file)", what,
                     len, s);
            FAIL("%s: \"%.*s\" is not a constant", what, len, s);
        }
        o->ea.imm = (long)o->v;
        o->imm_ok = 1;
        return 0;
    }
    if (special(s, len, "sr")) { o->k = K_SR; return 0; }
    if (special(s, len, "ccr")) { o->k = K_CCR; return 0; }
    if (special(s, len, "usp")) { o->k = K_USP; return 0; }
    if (ctrl_reg(s, len) >= 0) {
        o->k = K_CTRL;
        o->ctrl = ctrl_reg(s, len);
        return 0;
    }
    if (target(s, len, &o->v)) {
        o->k = K_TGT;
        return 0;
    }
    /* Dr:Dq */
    {
        const char *c = memchr(s, ':', (size_t)len);
        if (c && !memchr(s, '@', (size_t)len) && !memchr(s, '(', (size_t)len)) {
            const char *a = s, *b = c + 1;
            int al = trim(&a, (int)(c - s)), bl = trim(&b, (int)(s + len - b));
            o->r1 = cfasm_gpr(a, al);
            o->r2 = cfasm_gpr(b, bl);
            if (o->r1 < 0 || o->r2 < 0)
                FAIL("%s: \"%.*s\" is not a register pair (Dr:Dq)", what, len,
                     s);
            o->k = K_PAIR;
            return 0;
        }
    }
    /* a register list: Rn, Rn-Rm, joined by / */
    if (memchr(s, '/', (size_t)len) ||
        (memchr(s, '-', (size_t)len) && s[0] != '-' &&
         !memchr(s, '(', (size_t)len) && !memchr(s, '@', (size_t)len))) {
        const char *p = s, *e = s + len;
        o->k = K_LIST;
        while (p < e) {
            const char *q = p, *dash = NULL;
            int a, b;
            while (q < e && *q != '/') {
                if (*q == '-' && !dash)
                    dash = q;
                q++;
            }
            if (dash) {
                const char *x = p, *y = dash + 1;
                a = cfasm_gpr(x, trim(&x, (int)(dash - p)));
                b = cfasm_gpr(y, trim(&y, (int)(q - y)));
            } else {
                const char *x = p;
                a = b = cfasm_gpr(x, trim(&x, (int)(q - p)));
            }
            if (a < 0 || b < 0 || a > b)
                FAIL("%s: \"%.*s\" is not a register list (%%d2-%%d7/%%a2)",
                     what, len, s);
            for (int k = a; k <= b; k++)
                o->mask |= 1u << k;
            p = q < e ? q + 1 : q;
        }
        return 0;
    }
    r = cfasm_gpr(s, len);
    if (r >= 0) {
        o->ea.mode = CF_IS_A(r) ? CFM_A : CFM_D;
        o->ea.reg = r;
        o->ea.xscale = 1;
        return 0;
    }
    /* -(An) */
    if (len >= 4 && s[0] == '-' && s[1] == '(' && s[len - 1] == ')') {
        const char *in = s + 2;
        r = cfasm_gpr(in, trim(&in, len - 3));
        if (!CF_IS_A(r))
            FAIL("%s: \"%.*s\": predecrement takes an address register", what,
                 len, s);
        o->ea.mode = CFM_PRE;
        o->ea.reg = r;
        o->ea.xscale = 1;
        return 0;
    }
    /* MIT: An@, An@+, An@-, An@(...) */
    {
        const char *at = memchr(s, '@', (size_t)len);
        if (at) {
            const char *b = s, *rest = at + 1;
            int bl = trim(&b, (int)(at - s)), rl = (int)(s + len - rest);
            int base = cfasm_gpr(b, bl);
            if (!CF_IS_A(base) && !special(b, bl, "pc"))
                FAIL("%s: \"%.*s\" names no address register", what, len, s);
            if (rl == 0 || (rl == 1 && (rest[0] == '+' || rest[0] == '-'))) {
                if (base < 0)
                    FAIL("%s: \"%.*s\" is not a mode of %%pc", what, len, s);
                o->ea.mode = rl == 0 ? CFM_IND : rest[0] == '+' ? CFM_POST
                                                                : CFM_PRE;
                o->ea.reg = base;
                o->ea.xscale = 1;
                return 0;
            }
            if (rest[0] == '(' && rest[rl - 1] == ')') {
                /* An@(d) and An@(d,Xi:l:s): the base first */
                char buf[160];
                int n = snprintf(buf, sizeof buf, "%.*s,%.*s", rl - 2,
                                 rest + 1, bl, b);
                if (n >= (int)sizeof buf)
                    FAIL("%s: an operand too long", what);
                /* reorder to (d, base, index) */
                {
                    const char *cm = memchr(rest + 1, ',', (size_t)(rl - 2));
                    if (cm)
                        n = snprintf(buf, sizeof buf, "%.*s,%.*s,%.*s",
                                     (int)(cm - rest - 1), rest + 1, bl, b,
                                     (int)(rest + rl - 1 - cm - 1), cm + 1);
                    else
                        n = snprintf(buf, sizeof buf, "%.*s,%.*s", rl - 2,
                                     rest + 1, bl, b);
                }
                return paren_ea(what, "", 0, buf, n, o, err, errlen);
            }
            FAIL("%s: \"%.*s\" is not an effective address", what, len, s);
        }
    }
    /* (...), d(...), (...)+, (xxx).w / (xxx).l */
    {
        const char *lp = memchr(s, '(', (size_t)len);
        if (lp) {
            const char *rp = s + len - 1;
            int dl = (int)(lp - s), suf;
            while (rp > lp && *rp != ')')
                rp--;
            if (rp == lp)
                FAIL("%s: \"%.*s\" has no closing )", what, len, s);
            suf = (int)(s + len - rp - 1);
            if (suf == 1 && rp[1] == '+') {
                const char *in = lp + 1;
                r = cfasm_gpr(in, trim(&in, (int)(rp - lp - 1)));
                if (!CF_IS_A(r) || dl)
                    FAIL("%s: \"%.*s\": postincrement takes an address "
                         "register", what, len, s);
                o->ea.mode = CFM_POST;
                o->ea.reg = r;
                o->ea.xscale = 1;
                return 0;
            }
            if (suf == 2 && rp[1] == '.' && !dl &&
                (tolower((unsigned char)rp[2]) == 'w' ||
                 tolower((unsigned char)rp[2]) == 'l')) {
                long long a;
                const char *in = lp + 1;
                int il = trim(&in, (int)(rp - lp - 1));
                if (!asm_const_expr(in, il, &a)) {
                    if (isalpha((unsigned char)in[0]) || in[0] == '_')
                        FAIL("%s: \"%.*s\" is a symbol, and inline asm cannot "
                             "reach one: a template carries no relocation",
                             what, il, in);
                    FAIL("%s: \"%.*s\" is not an address", what, il, in);
                }
                if (tolower((unsigned char)rp[2]) == 'w') {
                    if (a < -32768 || a > 32767)
                        FAIL("%s: (%lld).w is not a 16-bit address", what, a);
                    o->ea.mode = CFM_ABSW;
                } else {
                    if (a < -2147483647LL - 1 || a > 4294967295LL)
                        FAIL("%s: (%lld).l is not a 32-bit address", what, a);
                    o->ea.mode = CFM_ABSL;
                }
                o->ea.disp = (long)a;
                o->ea.xscale = 1;
                return 0;
            }
            if (suf)
                FAIL("%s: \"%.*s\" is not an effective address", what, len, s);
            {
                const char *dsp = s;
                int dlen = trim(&dsp, dl);
                return paren_ea(what, dsp, dlen, lp + 1, (int)(rp - lp - 1), o,
                                err, errlen);
            }
        }
    }
    /* a bare number: an absolute long address */
    {
        long long a;
        if (asm_const_expr(s, len, &a)) {
            if (a < -2147483647LL - 1 || a > 4294967295LL)
                FAIL("%s: %lld is not a 32-bit address", what, a);
            o->ea.mode = CFM_ABSL;
            o->ea.disp = (long)a;
            o->ea.xscale = 1;
            return 0;
        }
    }
    if (isalpha((unsigned char)s[0]) || s[0] == '_')
        FAIL("%s: \"%.*s\" is a symbol, and inline asm cannot reach one: a "
             "template carries no relocation (pass its address as an \"a\" "
             "operand, or write it in a .S file)", what, len, s);
    FAIL("%s: \"%.*s\" is not an operand", what, len, s);
}

/* ---- statements -------------------------------------------------------------- */

#define MAXOPD 3

struct stmt {
    char mn[24];         /* without the size */
    char full[28];       /* as written, lower case */
    char sfx;            /* 'b' 'w' 'l' 's', or 0 */
    struct opd o[MAXOPD];
    int n;
};

static int split_stmt(const char *s, int len, struct stmt *st, char *err,
                      int errlen)
{
    int i = 0, ml;
    memset(st, 0, sizeof *st);
    while (i < len && !isspace((unsigned char)s[i]))
        i++;
    if (i >= (int)sizeof st->full)
        FAIL("asm instruction \"%.*s\" is not in the ColdFire vocabulary", i,
             s);
    for (int k = 0; k < i; k++)
        st->full[k] = (char)tolower((unsigned char)s[k]);
    st->full[i] = 0;
    memcpy(st->mn, st->full, (size_t)i + 1);
    {
        char *dot = strrchr(st->mn, '.');
        if (dot && dot != st->mn) {
            if (strlen(dot) != 2 || !strchr("bwls", dot[1]))
                FAIL("asm instruction \"%s\" is not in the ColdFire "
                     "vocabulary: \"%s\" is not a size (.b .w .l .s)",
                     st->full, dot);
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
            if (s[ml] == '(') depth++;
            else if (s[ml] == ')') depth--;
            ml++;
        }
        if (parse_opd(s + b, ml - b, &st->o[st->n], st->full, err, errlen))
            return -1;
        st->n++;
        if (ml < len)
            ml++;
    }
    if (ml < len)
        FAIL("%s: too many operands", st->full);
    return 0;
}

static int is(const struct stmt *st, const char *m)
{
    return strcmp(st->mn, m) == 0;
}

static int sfx_size(char c)
{
    return c == 'b' ? 1 : c == 'w' ? 2 : c == 'l' ? 4 : -1;
}

static int is_d(const struct opd *o)
{
    return o->k == K_EA && o->ea.mode == CFM_D;
}

static int is_a(const struct opd *o)
{
    return o->k == K_EA && o->ea.mode == CFM_A;
}

static int is_imm(const struct opd *o)
{
    return o->k == K_EA && o->ea.mode == CFM_IMM;
}

/* A (.+N,%pc) operand whose extension word is `ext` bytes from the
 * instruction's start: its displacement, from that word. */
static int fix_pc(const struct stmt *st, struct opd *o, int ext, char *err,
                  int errlen)
{
    long long d;
    if (!o->pctgt)
        return 0;
    d = o->v - ext;
    if (o->ea.mode == CFM_PCIDX ? d < -128 || d > 127
                                : d < -32768 || d > 32767)
        FAIL("%s: the target is out of reach of a %%pc displacement",
             st->full);
    o->ea.disp = (long)d;
    return 0;
}

/* An immediate of an operation of `size` bytes: signed or unsigned. */
static int imm_fits(const struct stmt *st, const struct opd *o, int size,
                    char *err, int errlen)
{
    long long lo = size == 1 ? -128 : size == 2 ? -32768 : -2147483647LL - 1;
    long long hi = size == 1 ? 255 : size == 2 ? 65535 : 4294967295LL;
    if (o->v < lo || o->v > hi)
        FAIL("%s: #%lld does not fit %d bits", st->full, o->v, 8 * size);
    return 0;
}

static int need_n(const struct stmt *st, int n, char *err, int errlen)
{
    if (st->n != n)
        FAIL("%s takes %d operand%s", st->full, n, n == 1 ? "" : "s");
    return 0;
}

static int need_dreg(const struct stmt *st, int k, char *err, int errlen)
{
    if (!is_d(&st->o[k]))
        FAIL("%s: \"%.*s\" is not a data register (%%d0-%%d7)", st->full,
             st->o[k].len, st->o[k].s);
    return 0;
}

static int need_areg(const struct stmt *st, int k, char *err, int errlen)
{
    if (!is_a(&st->o[k]))
        FAIL("%s: \"%.*s\" is not an address register (%%a0-%%a7)", st->full,
             st->o[k].len, st->o[k].s);
    return 0;
}

static int is_control(const struct cf_ea *e)
{
    return e->mode == CFM_IND || e->mode == CFM_DISP || e->mode == CFM_IDX ||
           e->mode == CFM_ABSW || e->mode == CFM_ABSL ||
           e->mode == CFM_PCDISP || e->mode == CFM_PCIDX;
}

static int need_size(const struct stmt *st, int dflt, int allowed, int *size,
                     char *err, int errlen)
{
    int sz = st->sfx ? sfx_size(st->sfx) : dflt;
    if (sz < 0 || !(allowed & sz))
        FAIL("%s: the size is %s on ColdFire", st->full,
             allowed == 4 ? ".l" : allowed == 6 ? ".w or .l"
             : allowed == 3 ? ".b or .w" : ".b, .w or .l");
    *size = sz;
    return 0;
}

/* ---- the moves ---------------------------------------------------------------- */

static int do_move(struct stmt *st, struct code *out, char *err, int errlen)
{
    struct opd *a = &st->o[0], *b = &st->o[1];
    int size;
    if (need_n(st, 2, err, errlen))
        return -1;
    /* the status, condition code and user stack pointer moves */
    if (a->k == K_SR || b->k == K_SR || a->k == K_CCR || b->k == K_CCR) {
        int sr = a->k == K_SR || b->k == K_SR;
        if (need_size(st, 2, 2, &size, err, errlen))
            return -1;
        if (a->k == K_SR || a->k == K_CCR) {
            if (!is_d(b))
                FAIL("%s: %%%s goes to a data register on ColdFire", st->full,
                     sr ? "sr" : "ccr");
            if (sr) cf_move_from_sr(out, b->ea.reg);
            else    cf_move_from_ccr(out, b->ea.reg);
            return 0;
        }
        if (is_d(a)) {
            if (sr) cf_move_to_sr(out, a->ea.reg);
            else    cf_move_to_ccr(out, a->ea);
            return 0;
        }
        if (is_imm(a)) {
            if (a->v < 0 || a->v > 0xffff)
                FAIL("%s: #%lld does not fit 16 bits", st->full, a->v);
            if (sr) {
                cf_move_to_sr_imm(out, (long)a->v);
            } else {
                struct cf_ea e = a->ea;
                e.imm = (long)(a->v & 0xff);
                cf_move_to_ccr(out, e);
            }
            return 0;
        }
        FAIL("%s: %%%s is written from a data register or an immediate on "
             "ColdFire", st->full, sr ? "sr" : "ccr");
    }
    if (a->k == K_USP || b->k == K_USP) {
        if (need_size(st, 4, 4, &size, err, errlen))
            return -1;
        if (a->k == K_USP && is_a(b)) {
            cf_move_usp(out, 0, b->ea.reg);
            return 0;
        }
        if (b->k == K_USP && is_a(a)) {
            cf_move_usp(out, 1, a->ea.reg);
            return 0;
        }
        FAIL("%s: %%usp moves to or from an address register", st->full);
    }
    if (a->k != K_EA || b->k != K_EA)
        FAIL("%s: these operands are not one of its forms", st->full);
    if (need_size(st, 2, 7, &size, err, errlen))
        return -1;
    if (is(st, "movea") && !is_a(b))
        FAIL("%s: movea writes an address register", st->full);
    if (is_a(b) && size == 1)
        FAIL("%s: a byte cannot be moved to an address register", st->full);
    if (is_imm(a) && imm_fits(st, a, size, err, errlen))
        return -1;
    if (fix_pc(st, a, 2, err, errlen))
        return -1;
    if (b->pctgt || b->ea.mode == CFM_PCDISP || b->ea.mode == CFM_PCIDX ||
        is_imm(b))
        FAIL("%s: the destination is not alterable", st->full);
    /* GNU as's quick form */
    if (size == 4 && is_imm(a) && is_d(b) && a->v >= -128 && a->v <= 127 &&
        !g_raw && !is(st, "movea")) {
        cf_moveq(out, (long)a->v, b->ea.reg);
        return 0;
    }
    if (!cf_move_ok(size, &a->ea, &b->ea))
        FAIL("%s: ColdFire cannot move between these modes (a source with "
             "an extension word to an indexed or absolute destination, or an "
             "indexed, absolute or immediate one to anything but a register, "
             "(An), (An)+ or -(An))", st->full);
    cf_move(out, size, a->ea, b->ea);
    return 0;
}

/* ---- the ALU ------------------------------------------------------------------ */

struct alu_ent { const char *name; int op; };
static const struct alu_ent alu_tab[] = {
    { "add", CF_ADD }, { "sub", CF_SUB }, { "and", CF_AND }, { "or", CF_OR },
    { "eor", CF_EOR }, { "cmp", CF_CMP },
    { NULL, 0 }
};

static int do_alu(int op, struct stmt *st, struct code *out, char *err,
                  int errlen)
{
    struct opd *a = &st->o[0], *b = &st->o[1];
    int size;
    if (need_n(st, 2, err, errlen))
        return -1;
    if (need_size(st, 4, 4, &size, err, errlen))
        FAIL("%s: ColdFire's arithmetic and logic are .l only", st->full);
    if (a->k != K_EA || b->k != K_EA)
        FAIL("%s: these operands are not one of its forms", st->full);
    if (fix_pc(st, a, 2, err, errlen))
        return -1;
    if (is_imm(a) && imm_fits(st, a, 4, err, errlen))
        return -1;
    /* an immediate: addq/subq for 1..8 (GNU as's quick form), else the
     * immediate instruction into a data register, adda/suba/cmpa into an
     * address register */
    if (is_imm(a) && (op == CF_ADD || op == CF_SUB) && a->v >= 1 &&
        a->v <= 8 && !g_raw &&
        (is_d(b) || is_a(b) || cf_ea_is_mem_alterable(&b->ea))) {
        cf_addq(out, op == CF_SUB, (int)a->v, b->ea);
        return 0;
    }
    if (is_a(b)) {
        if (op != CF_ADD && op != CF_SUB && op != CF_CMP)
            FAIL("%s: an address register is a destination of add, sub and "
                 "cmp only", st->full);
        cf_alua(out, (enum cf_alu)op, a->ea, b->ea.reg);
        return 0;
    }
    if (is_imm(a)) {
        if (!is_d(b))
            FAIL("%s: ColdFire's immediate arithmetic writes a data register",
                 st->full);
        cf_alu_imm(out, (enum cf_alu)op, (long)a->v, b->ea.reg);
        return 0;
    }
    if (is_d(b) && op != CF_EOR) {
        if ((op == CF_AND || op == CF_OR) && is_a(a))
            FAIL("%s: and/or take no address register source", st->full);
        cf_alu(out, (enum cf_alu)op, a->ea, b->ea.reg);
        return 0;
    }
    if (is_d(a) && op != CF_CMP &&
        (cf_ea_is_mem_alterable(&b->ea) || (op == CF_EOR && is_d(b)))) {
        cf_alu_mem(out, (enum cf_alu)op, a->ea.reg, b->ea);
        return 0;
    }
    FAIL("%s: these operands are not one of its forms on ColdFire (<ea>,Dn; "
         "Dn,<mem>; #imm,Dn; <ea>,An)", st->full);
}

/* ---- transfers -------------------------------------------------------------- *
 *
 * bra, bsr and bcc: .s (an 8-bit displacement that is not 0 or -1) and
 * .w; the displacement is from the instruction's start + 2. The .l form
 * (a 32-bit displacement) is ISA_B's: the MCF5208 -- QEMU's m5208 --
 * traps on it, so it is refused, and a transfer to a symbol defined
 * elsewhere is a jmp/jsr to its absolute address instead
 * (cfasm_symform). */
static int br_reaches(int kind, long long d)
{
    if (kind == 0)
        return d >= -128 && d <= 127 && d != 0 && d != -1;
    return d >= -32768 && d <= 32767;
}

static void br_emit(struct code *out, int bsr, int cond, int kind,
                    long long d)
{
    if (!br_reaches(kind, d))
        d = 2;                      /* the optimistic pass: the size only */
    if (kind == 0) {
        if (bsr) cf_bsr_b(out, (long)d);
        else     cf_bcc_b(out, cond, (long)d);
    } else {
        if (bsr) cf_bsr_w(out, (long)d);
        else     cf_bcc_w(out, cond, (long)d);
    }
}

static int do_branch(struct stmt *st, struct code *out, int bsr, int cond,
                     char *err, int errlen)
{
    const struct opd *a = &st->o[0];
    long long d;
    int kind;
    if (need_n(st, 1, err, errlen))
        return -1;
    if (a->k != K_TGT) {
        if (isalpha((unsigned char)a->s[0]) || a->s[0] == '_')
            FAIL("%s: \"%.*s\" is a symbol, and inline asm cannot reach one: "
                 "a template carries no relocation (jsr through an address "
                 "register, or write it in a .S file)", st->full, a->len,
                 a->s);
        FAIL("%s: \"%.*s\" is not a branch target (a label, or .+N bytes "
             "from the instruction)", st->full, a->len, a->s);
    }
    d = a->v - 2;
    if (st->sfx == 'l')
        FAIL("%s: a 32-bit branch displacement is ISA_B's, which the MCF5208 "
             "traps on: write .w (+-32 KiB), or jmp/jsr to the label",
             st->full);
    if (st->sfx) {
        kind = st->sfx == 's' || st->sfx == 'b' ? 0 : 1;
        if (!g_raw && g_level >= 0 && !br_reaches(kind, d))
            FAIL("%s: the target is %lld bytes away, out of reach of the .%c "
                 "form%s", st->full, a->v, st->sfx,
                 kind == 0 ? " (an 8-bit displacement, not 0 or -1 from the "
                             "next word)" : "");
        br_emit(out, bsr, cond, kind, g_raw ? 0 : d);
        return 0;
    }
    if (g_level < 0) {
        kind = 0;
    } else {
        for (kind = g_level; kind < 2; kind++)
            if (br_reaches(kind, d))
                break;
        if (kind >= 2)
            FAIL("%s: the target is %lld bytes away, out of reach of a "
                 "branch (+-32 KiB; the MCF5208 has no 32-bit one): jmp to "
                 "it", st->full, a->v);
    }
    if (kind > g_took)
        g_took = kind;
    br_emit(out, bsr, cond, kind, d);
    return 0;
}

/* ---- the rest --------------------------------------------------------------- */

static const struct { const char *name; void (*fn)(struct code *); } op0_tab[] = {
    { "nop", cf_nop }, { "rts", cf_rts }, { "rte", cf_rte }, { "tpf", cf_tpf },
    { "halt", cf_halt }, { "illegal", cf_illegal },
    { NULL, NULL }
};

/* What ColdFire lacks, refused by name rather than as unknown. */
static const struct { const char *name; const char *why; } absent_tab[] = {
    { "rol", "ColdFire has no rotate" }, { "ror", "ColdFire has no rotate" },
    { "roxl", "ColdFire has no rotate" }, { "roxr", "ColdFire has no rotate" },
    { "dbra", "ColdFire has no dbcc: subq.l #1 and a bne" },
    { "exg", "ColdFire has no exg" }, { "movep", "ColdFire has no movep" },
    { "cas", "ColdFire has no cas (mask the interrupts)" },
    { "tas", "tas is not in ISA_A+" },
    { "chk", "ColdFire has no chk" }, { "rtr", "ColdFire has no rtr" },
    { "trapv", "ColdFire has no trapv" }, { "abcd", "ColdFire has no BCD" },
    { "sbcd", "ColdFire has no BCD" }, { "nbcd", "ColdFire has no BCD" },
    { "mac", "the MAC's instructions are not in EmbCC's ColdFire vocabulary" },
    { "msac", "the MAC's instructions are not in EmbCC's ColdFire "
              "vocabulary" },
    { NULL, NULL }
};

/* The FPU's mnemonics: refused by name, before their operands (%fp0) are
 * read. */
static const char *const fpu_tab[] = {
    "fmove", "fmovem", "fadd", "fsub", "fmul", "fdiv", "fcmp", "ftst",
    "fabs", "fneg", "fsqrt", "fint", "fintrz", "fnop", "fsave", "frestore",
    "fsadd", "fdadd", "fsmul", "fdmul", "fsdiv", "fddiv", "fssub", "fdsub",
    NULL
};

static int is_fpu(const char *s, int len)
{
    char mn[16];
    int n = 0;
    while (n < len && n < 15 && (isalnum((unsigned char)s[n]))) {
        mn[n] = (char)tolower((unsigned char)s[n]);
        n++;
    }
    mn[n] = 0;
    if (n >= 3 && mn[0] == 'f' && mn[1] == 'b')           /* fbcc */
        return 1;
    for (int k = 0; fpu_tab[k]; k++)
        if (!strcmp(mn, fpu_tab[k]))
            return 1;
    return 0;
}

static int stmt_body(const char *s, int len, struct code *out, char *err,
                     int errlen)
{
    struct stmt st;
    int c, size;
    while (len > 0 && isspace((unsigned char)*s)) {
        s++;
        len--;
    }
    while (len > 0 && isspace((unsigned char)s[len - 1]))
        len--;
    g_raw = 0;
    if (len > 0 && s[0] == '\003') {          /* cfasm_symform's */
        g_raw = 1;
        s++;
        len--;
    }
    if (len > 0 && s[0] == '\001')            /* a refusal it wrote */
        FAIL("%.*s", len - 1, s + 1);
    if (len == 0)
        return 0;
    if (is_fpu(s, len))
        FAIL("%.*s: the FPU's instructions are not in EmbCC's ColdFire "
             "vocabulary (the MCF5208 has no FPU)", (int)strcspn(s, " \t"), s);
    if (split_stmt(s, len, &st, err, errlen))
        return -1;

    for (int k = 0; op0_tab[k].name; k++)
        if (is(&st, op0_tab[k].name)) {
            if (st.n || st.sfx)
                FAIL("%s takes no operands", st.full);
            op0_tab[k].fn(out);
            return 0;
        }
    if (is(&st, "move") || is(&st, "movea"))
        return do_move(&st, out, err, errlen);
    if (is(&st, "moveq")) {
        if (need_n(&st, 2, err, errlen) || need_dreg(&st, 1, err, errlen))
            return -1;
        if (!is_imm(&st.o[0]) || st.o[0].v < -128 || st.o[0].v > 127 ||
            (st.sfx && st.sfx != 'l'))
            FAIL("moveq: the source is #-128..127");
        cf_moveq(out, (long)st.o[0].v, st.o[1].ea.reg);
        return 0;
    }
    if (is(&st, "mvs") || is(&st, "mvz") || is(&st, "mov3q") ||
        is(&st, "sats"))
        FAIL("%s is an ISA_B instruction, which the MCF5208 (ISA_A+) does "
             "not have: it traps as illegal there", st.full);
    if (is(&st, "lea") || is(&st, "pea") || is(&st, "jmp") || is(&st, "jsr")) {
        int lea = is(&st, "lea");
        if (need_n(&st, lea ? 2 : 1, err, errlen) ||
            (lea && need_areg(&st, 1, err, errlen)))
            return -1;
        if (st.sfx && st.sfx != 'l')
            FAIL("%s: the size is .l", st.full);
        if (st.o[0].k != K_EA || fix_pc(&st, &st.o[0], 2, err, errlen))
            return -1;
        if (!is_control(&st.o[0].ea))
            FAIL("%s: \"%.*s\" is not a control address ((An), (d,An), "
                 "(d,An,Xi), an absolute or %%pc-relative one)", st.full,
                 st.o[0].len, st.o[0].s);
        if (lea) cf_lea(out, st.o[0].ea, st.o[1].ea.reg);
        else if (is(&st, "pea")) cf_pea(out, st.o[0].ea);
        else if (is(&st, "jsr")) cf_jsr(out, st.o[0].ea);
        else cf_jmp(out, st.o[0].ea);
        return 0;
    }

    for (const struct alu_ent *e = alu_tab; e->name; e++)
        if (is(&st, e->name))
            return do_alu(e->op, &st, out, err, errlen);
    if (is(&st, "adda") || is(&st, "suba") || is(&st, "cmpa")) {
        int op = st.mn[0] == 'a' ? CF_ADD : st.mn[0] == 's' ? CF_SUB : CF_CMP;
        if (need_n(&st, 2, err, errlen) || need_areg(&st, 1, err, errlen) ||
            need_size(&st, 4, 4, &size, err, errlen))
            return -1;
        if (st.o[0].k != K_EA || fix_pc(&st, &st.o[0], 2, err, errlen))
            return -1;
        if (is_imm(&st.o[0]) && imm_fits(&st, &st.o[0], 4, err, errlen))
            return -1;
        cf_alua(out, (enum cf_alu)op, st.o[0].ea, st.o[1].ea.reg);
        return 0;
    }
    {
        static const char *const imm_n[] = { "addi", "subi", "andi", "ori",
                                             "eori", "cmpi" };
        static const int imm_op[] = { CF_ADD, CF_SUB, CF_AND, CF_OR, CF_EOR,
                                      CF_CMP };
        for (int k = 0; k < 6; k++)
            if (is(&st, imm_n[k])) {
                if (need_n(&st, 2, err, errlen) ||
                    need_dreg(&st, 1, err, errlen) ||
                    need_size(&st, 4, 4, &size, err, errlen))
                    FAIL("%s: ColdFire's form is %s.l #imm,Dn", st.full,
                         imm_n[k]);
                if (!is_imm(&st.o[0]) ||
                    imm_fits(&st, &st.o[0], 4, err, errlen))
                    FAIL("%s: ColdFire's form is %s.l #imm,Dn", st.full,
                         imm_n[k]);
                cf_alu_imm(out, (enum cf_alu)imm_op[k], (long)st.o[0].v,
                           st.o[1].ea.reg);
                return 0;
            }
    }
    if (is(&st, "addq") || is(&st, "subq")) {
        if (need_n(&st, 2, err, errlen) || need_size(&st, 4, 4, &size, err,
                                                     errlen))
            return -1;
        if (!is_imm(&st.o[0]) || st.o[0].v < 1 || st.o[0].v > 8)
            FAIL("%s: the source is #1..8", st.full);
        if (st.o[1].k != K_EA || (!cf_ea_is_reg(&st.o[1].ea) &&
                                  !cf_ea_is_mem_alterable(&st.o[1].ea)))
            FAIL("%s: \"%.*s\" is not alterable", st.full, st.o[1].len,
                 st.o[1].s);
        cf_addq(out, st.mn[0] == 's', (int)st.o[0].v, st.o[1].ea);
        return 0;
    }
    if (is(&st, "addx") || is(&st, "subx")) {
        if (need_n(&st, 2, err, errlen) || need_dreg(&st, 0, err, errlen) ||
            need_dreg(&st, 1, err, errlen) ||
            need_size(&st, 4, 4, &size, err, errlen))
            return -1;
        cf_addx(out, st.mn[0] == 's', st.o[0].ea.reg, st.o[1].ea.reg);
        return 0;
    }
    {
        static const char *const un_n[] = { "neg", "negx", "not", "swap",
                                            "ext", "extb" };
        for (int k = 0; k < 6; k++)
            if (is(&st, un_n[k])) {
                int op = k;
                if (need_n(&st, 1, err, errlen) ||
                    need_dreg(&st, 0, err, errlen))
                    return -1;
                if (k == 3 && st.sfx && st.sfx != 'w')
                    FAIL("swap: the size is .w");
                if (k == 4) {
                    if (st.sfx != 'w' && st.sfx != 'l')
                        FAIL("ext: write ext.w (a byte to a word) or ext.l "
                             "(a word to a long)");
                    op = st.sfx == 'w' ? CF_EXTW : CF_EXTL;
                } else if (k == 5) {
                    if (st.sfx && st.sfx != 'l')
                        FAIL("extb: the size is .l");
                    op = CF_EXTBL;
                } else if (k < 3 && need_size(&st, 4, 4, &size, err, errlen)) {
                    FAIL("%s: ColdFire's form is %s.l Dn", st.full, un_n[k]);
                }
                cf_unary(out, (enum cf_un)op, st.o[0].ea.reg);
                return 0;
            }
    }
    if (is(&st, "clr") || is(&st, "tst")) {
        if (need_n(&st, 1, err, errlen) ||
            need_size(&st, 2, 7, &size, err, errlen))
            return -1;
        if (st.o[0].k != K_EA ||
            (!is_d(&st.o[0]) && !cf_ea_is_mem_alterable(&st.o[0].ea)))
            FAIL("%s: \"%.*s\" is not a data register or memory", st.full,
                 st.o[0].len, st.o[0].s);
        if (is(&st, "clr")) cf_clr(out, size, st.o[0].ea);
        else cf_tst(out, size, st.o[0].ea);
        return 0;
    }
    {
        static const char *const sh_n[] = { "asl", "asr", "lsl", "lsr" };
        for (int k = 0; k < 4; k++)
            if (is(&st, sh_n[k])) {
                if (need_n(&st, 2, err, errlen) ||
                    need_dreg(&st, 1, err, errlen) ||
                    need_size(&st, 4, 4, &size, err, errlen))
                    FAIL("%s: ColdFire's forms are %s.l #1..8,Dn and %s.l "
                         "Dx,Dn", st.full, sh_n[k], sh_n[k]);
                if (is_d(&st.o[0])) {
                    cf_shift_reg(out, (enum cf_sh)k, st.o[0].ea.reg,
                                 st.o[1].ea.reg);
                    return 0;
                }
                if (!is_imm(&st.o[0]) || st.o[0].v < 1 || st.o[0].v > 8)
                    FAIL("%s: the count is #1..8 or a data register",
                         st.full);
                cf_shift_imm(out, (enum cf_sh)k, (int)st.o[0].v,
                             st.o[1].ea.reg);
                return 0;
            }
    }
    if (is(&st, "muls") || is(&st, "mulu")) {
        struct opd *a = &st.o[0];
        if (need_n(&st, 2, err, errlen) || need_dreg(&st, 1, err, errlen) ||
            need_size(&st, 2, 6, &size, err, errlen))
            return -1;
        if (a->k != K_EA || is_a(a) ||
            fix_pc(&st, a, size == 4 ? 4 : 2, err, errlen))
            FAIL("%s: \"%.*s\" is not a source it takes", st.full, a->len,
                 a->s);
        if (size == 4 && a->ea.mode != CFM_D && a->ea.mode != CFM_IND &&
            a->ea.mode != CFM_POST && a->ea.mode != CFM_PRE &&
            a->ea.mode != CFM_DISP)
            FAIL("%s: mul.l takes Dn, (An), (An)+, -(An) or (d16,An) on "
                 "ColdFire", st.full);
        if (is_imm(a) && imm_fits(&st, a, 2, err, errlen))
            return -1;
        cf_mul(out, st.mn[3] == 's', size, a->ea, st.o[1].ea.reg);
        return 0;
    }
    if (is(&st, "divs") || is(&st, "divu") || is(&st, "rems") ||
        is(&st, "remu")) {
        struct opd *a = &st.o[0], *b = &st.o[1];
        int rem = st.mn[0] == 'r', sign = st.mn[3] == 's';
        if (need_n(&st, 2, err, errlen))
            return -1;
        if (st.sfx != 'l')
            FAIL("%s: ColdFire's form here is %s.l (the .w divide is not in "
                 "EmbCC's vocabulary)", st.full, st.mn);
        if (a->k != K_EA || (a->ea.mode != CFM_D && a->ea.mode != CFM_IND &&
                             a->ea.mode != CFM_POST && a->ea.mode != CFM_PRE &&
                             a->ea.mode != CFM_DISP))
            FAIL("%s: the divisor is Dn, (An), (An)+, -(An) or (d16,An) on "
                 "ColdFire", st.full);
        if (rem) {
            if (b->k != K_PAIR || b->r1 > 7 || b->r2 > 7 || b->r1 == b->r2)
                FAIL("%s: the destination is Dr:Dq, two data registers",
                     st.full);
            cf_rem(out, sign, a->ea, b->r1, b->r2);
            return 0;
        }
        if (need_dreg(&st, 1, err, errlen))
            return -1;
        cf_div(out, sign, a->ea, b->ea.reg);
        return 0;
    }
    c = st.mn[0] == 's' ? cond_of(st.mn + 1) : -1;
    if (c >= 0) {
        if (need_n(&st, 1, err, errlen) || need_dreg(&st, 0, err, errlen))
            FAIL("%s: ColdFire's form is s%s Dn", st.full, st.mn + 1);
        if (st.sfx && st.sfx != 'b')
            FAIL("%s: the size is .b", st.full);
        cf_scc(out, c, st.o[0].ea.reg);
        return 0;
    }
    if (is(&st, "bra"))
        return do_branch(&st, out, 0, CF_T, err, errlen);
    if (is(&st, "bsr"))
        return do_branch(&st, out, 1, CF_T, err, errlen);
    {
        static const char *const bit_n[] = { "btst", "bchg", "bclr", "bset" };
        for (int k = 0; k < 4; k++)
            if (is(&st, bit_n[k])) {
                struct opd *a = &st.o[0], *b = &st.o[1];
                if (need_n(&st, 2, err, errlen))
                    return -1;
                if (b->k != K_EA)
                    FAIL("%s: \"%.*s\" is not an operand it takes", st.full,
                         b->len, b->s);
                if (st.sfx && st.sfx != (is_d(b) ? 'l' : 'b'))
                    FAIL("%s: the size is .l on a data register and .b in "
                         "memory", st.full);
                if (!is_d(b) && !cf_ea_is_mem_alterable(&b->ea))
                    FAIL("%s: \"%.*s\" is not a data register or memory",
                         st.full, b->len, b->s);
                if (is_d(a)) {
                    cf_bit(out, (enum cf_bitop)k, a->ea.reg, 0, b->ea);
                    return 0;
                }
                if (!is_imm(a) || a->v < 0 || a->v > (is_d(b) ? 31 : 7))
                    FAIL("%s: the bit number is #0..%d or a data register",
                         st.full, is_d(b) ? 31 : 7);
                if (b->ea.mode == CFM_IDX || b->ea.mode == CFM_ABSW ||
                    b->ea.mode == CFM_ABSL)
                    FAIL("%s: a constant bit number takes Dn, (An), (An)+, "
                         "-(An) or (d16,An) on ColdFire", st.full);
                cf_bit(out, (enum cf_bitop)k, -1, (int)a->v, b->ea);
                return 0;
            }
    }
    c = st.mn[0] == 'b' ? cond_of(st.mn + 1) : -1;
    if (c >= 2)
        return do_branch(&st, out, 0, c, err, errlen);
    if (is(&st, "trap")) {
        if (need_n(&st, 1, err, errlen))
            return -1;
        if (!is_imm(&st.o[0]) || st.o[0].v < 0 || st.o[0].v > 15)
            FAIL("trap: the vector is #0..15");
        cf_trap(out, (int)st.o[0].v);
        return 0;
    }
    if (is(&st, "stop")) {
        if (need_n(&st, 1, err, errlen))
            return -1;
        if (!is_imm(&st.o[0]) || st.o[0].v < 0 || st.o[0].v > 0xffff)
            FAIL("stop: the operand is #0..0xffff, the new %%sr");
        cf_stop(out, (long)st.o[0].v);
        return 0;
    }
    if (is(&st, "link")) {
        if (need_n(&st, 2, err, errlen) || need_areg(&st, 0, err, errlen))
            return -1;
        if ((st.sfx && st.sfx != 'w') || !is_imm(&st.o[1]) ||
            st.o[1].v < -32768 || st.o[1].v > 32767)
            FAIL("link: ColdFire's form is link.w An,#-32768..32767");
        cf_link(out, st.o[0].ea.reg, (long)st.o[1].v);
        return 0;
    }
    if (is(&st, "unlk")) {
        if (need_n(&st, 1, err, errlen) || need_areg(&st, 0, err, errlen))
            return -1;
        cf_unlk(out, st.o[0].ea.reg);
        return 0;
    }
    if (is(&st, "movem")) {
        struct opd *a = &st.o[0], *b = &st.o[1];
        struct opd *l, *m;
        if (need_n(&st, 2, err, errlen) ||
            need_size(&st, 4, 4, &size, err, errlen))
            FAIL("movem: ColdFire's form is movem.l");
        l = a->k == K_LIST || (a->k == K_EA && cf_ea_is_reg(&a->ea)) ? a : b;
        m = l == a ? b : a;
        if (l->k == K_EA)
            l->mask = 1u << l->ea.reg;
        if (m->k != K_EA || (m->ea.mode != CFM_IND && m->ea.mode != CFM_DISP))
            FAIL("movem: the memory operand is (An) or (d16,An) on ColdFire "
                 "(no predecrement or postincrement)");
        if (l == a) cf_movem_store(out, l->mask, m->ea);
        else        cf_movem_load(out, m->ea, l->mask);
        return 0;
    }
    if (is(&st, "movec")) {
        if (need_n(&st, 2, err, errlen))
            return -1;
        if (st.o[0].k == K_CTRL)
            FAIL("movec: ColdFire writes control registers only (movec Rn,Rc)");
        if (st.o[0].k != K_EA || !cf_ea_is_reg(&st.o[0].ea) ||
            st.o[1].k != K_CTRL)
            FAIL("movec: the form is movec Rn,Rc, Rc one of %%cacr, %%asid, "
                 "%%acr0-3, %%mmubar, %%vbr, %%rombar, %%rambar, %%mbar");
        cf_movec(out, st.o[0].ea.reg, st.o[1].ctrl);
        return 0;
    }
    for (int k = 0; absent_tab[k].name; k++)
        if (is(&st, absent_tab[k].name) ||
            (!strncmp(absent_tab[k].name, "db", 2) &&
             !strncmp(st.mn, "db", 2)))
            FAIL("%s: %s", st.full, absent_tab[k].why);
    FAIL("asm instruction \"%s\" is not in the ColdFire vocabulary", st.full);
}

int cfasm_encode(const char *text, struct code *out, char *err, int errlen)
{
    int start = out->len, rc;
    g_took = 0;
    rc = stmt_body(text, (int)strlen(text), out, err, errlen);
    /* a relocated field: the marker it was encoded with becomes zero */
    if (rc == 0 && g_raw)
        for (int k = start; k + 4 <= out->len; k += 2)
            if (cf_rdw(out, k) == (RELOC_MARK >> 16) &&
                cf_rdw(out, k + 2) == (RELOC_MARK & 0xffff)) {
                cf_wrl(out, k, 0);
                break;
            }
    g_raw = 0;
    return rc;
}

/* ---- a template: statements and numeric labels ------------------------------ */

#define MAXSTMT 256

struct stm {
    const char *s;
    int len;
    int labs[4], nlabs;
};

/* Splits on newlines and `;`, drops `|` comments and a `#` that starts a
 * line, and takes the numeric labels off the front of each statement. */
static int stmts(const char *text, struct stm *st, int max, char *err,
                 int errlen)
{
    int n = 0;
    const char *p = text;
    while (*p) {
        const char *e = p, *c;
        int len;
        while (*e && *e != '\n' && *e != ';')
            e++;
        len = (int)(e - p);
        c = memchr(p, '|', (size_t)len);
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
            if (k > 0 && k < len && p[k] == ':' &&
                !(k + 1 < len && isalnum((unsigned char)p[k + 1])))
                FAIL("label \"%.*s\" in a ColdFire asm template: labels in a "
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

int cfasm_assemble(const char *text, struct code *out, char *err, int errlen)
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
    /* RX's scheme (rx/asm.c): every statement encoded with the distances
     * the previous pass gave, branches only ever lengthened, until the
     * offsets and the forms stand still */
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
            g_raw = 0;
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

/* ---- for the file assembler (src/as/gas.c) ---------------------------------- */

static const char *skip_sp(const char *p)
{
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

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

int cfasm_is_transfer(const char *stmt)
{
    char mn[24], sfx;
    mnemonic(skip_sp(stmt), mn, (int)sizeof mn, &sfx);
    if (!strcmp(mn, "bra") || !strcmp(mn, "bsr"))
        return 1;
    return mn[0] == 'b' && cond_of(mn + 1) >= 2;
}

int cfasm_is_word(const char *stmt, const char *w, int len)
{
    /* an index register with its size (`d1.l`), a size after a `)`
     * (`(0x400).w`), MIT's `:l` */
    if (len == 2 && w[0] == '.' && (w[1] == 'l' || w[1] == 'L' ||
                                    w[1] == 'w' || w[1] == 'W'))
        return 1;
    if (len >= 4 && w[len - 2] == '.' && cfasm_gpr(w, len - 2) >= 0)
        return 1;
    if (len == 1 && w > stmt && w[-1] == ':' &&
        (w[0] == 'l' || w[0] == 'L' || w[0] == 'w' || w[0] == 'W'))
        return 1;
    /* (xxx).w and MIT's :l are numbers' and registers' business */
    return special(w, len, "sr") || special(w, len, "ccr") ||
           special(w, len, "usp") || special(w, len, "pc") ||
           ctrl_reg(w, len) >= 0;
}

/* The symbol at p (not a register or a special name, not `.`), then an
 * optional `+K`/`-K`: the length consumed, 0 when there is none. */
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
    if (cfasm_gpr(p, n) >= 0 || special(p, n, "sr") || special(p, n, "ccr") ||
        special(p, n, "usp") || special(p, n, "pc") || ctrl_reg(p, n) >= 0)
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

/* The statements whose operand is a SYMBOL: a branch to one this file does
 * not define here -- bra and bsr as jmp and jsr to its absolute address
 * (R_68K_32), which always reach, a conditional branch or a written .w as
 * .w with R_68K_PC16 -- and any other instruction
 * with an operand that is a bare symbol or `#symbol` (+-K): jsr, jmp, lea,
 * pea, a move from or to an absolute address or of an address -- each the
 * 32-bit absolute form with R_68K_32 on its field, which is found by
 * encoding the statement with a marker in the symbol's place. */
int cfasm_symform(const char *stmt, struct asm_symform *f)
{
    char mn[24], sfx;
    const char *p = skip_sp(stmt), *o;
    int ml, slen = 0, n, depth = 0;
    long add = 0;

    ml = mnemonic(p, mn, (int)sizeof mn, &sfx);
    if (!ml || (p[ml] != ' ' && p[ml] != '\t'))
        return 0;
    memset(f, 0, sizeof *f);
    o = skip_sp(p + ml);
    if (cfasm_is_transfer(stmt)) {
        n = sym_operand(o, &slen, &add);
        if (!n || *skip_sp(o + n))
            return 0;
        if (sfx && sfx != 'w') {
            f->sym_at = (int)(o - stmt);
            f->sym_len = slen;
            snprintf(f->encode, sizeof f->encode, "\001%s.%c %.*s: %s", mn,
                     sfx, slen, o, sfx == 'l'
                     ? "a 32-bit branch displacement is ISA_B's, which the "
                       "MCF5208 traps on: write .w, or no size"
                     : "a short branch carries no relocation: write .w, or "
                       "no size");
            return 1;
        }
        f->sym_at = (int)(o - stmt);
        f->sym_len = slen;
        f->addend = add;
        if (!sfx && (!strcmp(mn, "bra") || !strcmp(mn, "bsr"))) {
            snprintf(f->encode, sizeof f->encode, "\003%s (0x%lx).l",
                     mn[1] == 'r' ? "jmp" : "jsr", RELOC_MARK);
            f->site[0].reloc = R_68K_32;
        } else {
            snprintf(f->encode, sizeof f->encode, "\003%s.w .+2", mn);
            f->site[0].reloc = R_68K_PC16;
        }
        f->site[0].off = 2;
        f->nsites = 1;
        return 1;
    }
    /* each operand: a bare symbol, or # and one */
    for (const char *q = o; *q; ) {
        const char *b = q;
        int imm = 0;
        while (*q && (depth || *q != ',')) {
            if (*q == '(') depth++;
            else if (*q == ')') depth--;
            q++;
        }
        b = skip_sp(b);
        if (*b == '#') {
            imm = 1;
            b++;
        }
        n = sym_operand(b, &slen, &add);
        if (n && skip_sp(b + n) >= q) {
            char text[128];
            struct code c = { 0 };
            char err[160];
            int at = -1;
            snprintf(text, sizeof text, "%.*s%s0x%lx%s", (int)(b - stmt), stmt,
                     imm ? "" : "(", RELOC_MARK, imm ? q : ").l");
            if (!imm) {
                int t = (int)strlen(text);
                snprintf(text + t, sizeof text - (size_t)t, "%s", q);
            }
            if (cfasm_encode(text, &c, err, (int)sizeof err) == 0)
                for (int k = 0; k + 4 <= c.len; k += 2)
                    if (cf_rdw(&c, k) == (RELOC_MARK >> 16) &&
                        cf_rdw(&c, k + 2) == (RELOC_MARK & 0xffff)) {
                        at = k;
                        break;
                    }
            free(c.p);
            if (at < 0)
                return 0;
            f->sym_at = (int)(b - stmt);
            f->sym_len = slen;
            f->addend = add;
            snprintf(f->encode, sizeof f->encode, "\003%s", text);
            f->site[0].off = at;
            f->site[0].reloc = R_68K_32;
            f->nsites = 1;
            return 1;
        }
        if (*q == ',')
            q++;
    }
    return 0;
}

void cfasm_fill(struct code *out, long gap)
{
    if (gap & 1)
        code_byte(out, 0);
    for (long k = 1; k < gap; k += 2)
        cf_nop(out);
}

/* ---- the referee's input ------------------------------------------------------
 *
 * Each line is a statement, an @, and what QEMU's m68k disassembler prints
 * for it -- binutils' MIT syntax -- written from the same sample: the
 * operand as the instruction means it, so a field in the wrong place, a
 * wrong size or mode or extension word reads back differently. The
 * statements are written in every spelling the parser takes: Motorola with
 * and without `%`, either case, `d(An)` and `(d,An)`, MIT's `An@(d)`.
 * `{+N}` is the address N bytes from the statement's start (a branch's or
 * a %pc-relative operand's target). A backquote separates the two: MIT
 * syntax has its own `@`. */

/* A sample of each kind of effective address (cfcheck's). */
static struct cf_ea v_sample(int k, int r)
{
    struct cf_ea e;
    memset(&e, 0, sizeof e);
    e.xscale = 1;
    switch (k) {
    case 0: e.mode = CFM_D; e.reg = r & 7; break;
    case 1: e.mode = CFM_A; e.reg = 8 + (r & 7); break;
    case 2: e.mode = CFM_IND; e.reg = 8 + (r & 7); break;
    case 3: e.mode = CFM_POST; e.reg = 8 + (r & 7); break;
    case 4: e.mode = CFM_PRE; e.reg = 8 + (r & 7); break;
    case 5: e.mode = CFM_DISP; e.reg = 8 + (r & 7);
            e.disp = r & 1 ? -32768 : r & 2 ? 32767 : 4 * r + 4; break;
    case 6: e.mode = CFM_IDX; e.reg = 8 + (r & 7);
            e.disp = r & 1 ? -128 : r & 2 ? 127 : 0; e.xreg = r ^ 5;
            e.xscale = r % 3 == 0 ? 1 : r % 3 == 1 ? 2 : 4; break;
    case 7: e.mode = CFM_ABSL; e.disp = 0x40001000L + 4L * r; break;
    case 8: e.mode = CFM_PCDISP; e.disp = r & 1 ? -2 : 32766; break;
    case 9: e.mode = CFM_ABSW; e.disp = 0x1000L + 2L * r; break;
    default: e.mode = CFM_IMM;
             e.imm = r & 1 ? -2147483647L - 1 : 0x7fffffffL; break;
    }
    return e;
}
#define V_NSAMPLE 11

static const char *v_reg(int r, int sty)
{
    static char buf[4][8];
    static int k;
    const char *n = cf_reg_name(r);
    char *b = buf[k++ & 3];
    if (sty == 2) {                       /* upper case, no % */
        int i = 0;
        for (; n[i]; i++)
            b[i] = (char)toupper((unsigned char)n[i]);
        b[i] = 0;
    } else {
        snprintf(b, 8, "%s%s", sty == 1 ? "" : "%", n);
    }
    return b;
}

/* The operand as a statement writes it, in style sty (0 Motorola with %,
 * 1 without, 2 upper case, 3 MIT), `size` the operation's. */
static void v_mot(char *o, size_t n, const struct cf_ea *e, int size, int sty)
{
    int mit = sty == 3;
    if (mit)
        sty = 0;
    switch (e->mode) {
    case CFM_D: case CFM_A:
        snprintf(o, n, "%s", v_reg(e->reg, sty));
        return;
    case CFM_IND:
        snprintf(o, n, mit ? "%s@" : "(%s)", v_reg(e->reg, sty));
        return;
    case CFM_POST:
        snprintf(o, n, mit ? "%s@+" : "(%s)+", v_reg(e->reg, sty));
        return;
    case CFM_PRE:
        snprintf(o, n, mit ? "%s@-" : "-(%s)", v_reg(e->reg, sty));
        return;
    case CFM_DISP:
        if (mit)
            snprintf(o, n, "%s@(%ld)", v_reg(e->reg, sty), e->disp);
        else if (sty == 1)
            snprintf(o, n, "%ld(%s)", e->disp, v_reg(e->reg, sty));
        else
            snprintf(o, n, "(%ld,%s)", e->disp, v_reg(e->reg, sty));
        return;
    case CFM_IDX:
        if (mit)
            snprintf(o, n, "%s@(%ld,%s:l:%d)", v_reg(e->reg, sty), e->disp,
                     v_reg(e->xreg, sty), e->xscale);
        else if (e->disp == 0 && sty == 2)
            snprintf(o, n, "(%s,%s.L*%d)", v_reg(e->reg, sty),
                     v_reg(e->xreg, sty), e->xscale);
        else if (sty == 1)
            snprintf(o, n, "%ld(%s,%s.l*%d)", e->disp, v_reg(e->reg, sty),
                     v_reg(e->xreg, sty), e->xscale);
        else
            snprintf(o, n, "(%ld,%s,%s.l*%d)", e->disp, v_reg(e->reg, sty),
                     v_reg(e->xreg, sty), e->xscale);
        return;
    case CFM_ABSW:
        snprintf(o, n, "(0x%lx).w", e->disp);
        return;
    case CFM_ABSL:
        if (sty == 1)
            snprintf(o, n, "0x%lx", e->disp);
        else
            snprintf(o, n, "(0x%lx).l", e->disp);
        return;
    case CFM_PCDISP:
        snprintf(o, n, mit ? "%%pc@(%ld)" : "(%ld,%%pc)", e->disp);
        return;
    case CFM_PCIDX:
        snprintf(o, n, "(%ld,%%pc,%s.l)", e->disp, v_reg(e->xreg, sty));
        return;
    case CFM_IMM: {
        long v = e->imm;
        if (size == 1) v = (long)(signed char)(v & 0xff);
        else if (size == 2) v = (long)(short)(v & 0xffff);
        snprintf(o, n, "#%ld", v);
        return;
    }
    }
}

/* ...and as QEMU prints it; `ext` the offset of its extension word from
 * the statement's start (a %pc base is measured from it). */
static void v_mit(char *o, size_t n, const struct cf_ea *e, int size, int ext)
{
    switch (e->mode) {
    case CFM_D: case CFM_A:
        snprintf(o, n, "%%%s", cf_reg_name(e->reg));
        return;
    case CFM_IND:
        snprintf(o, n, "%%%s@", cf_reg_name(e->reg));
        return;
    case CFM_POST:
        snprintf(o, n, "%%%s@+", cf_reg_name(e->reg));
        return;
    case CFM_PRE:
        snprintf(o, n, "%%%s@-", cf_reg_name(e->reg));
        return;
    case CFM_DISP:
        snprintf(o, n, "%%%s@(%ld)", cf_reg_name(e->reg), e->disp);
        return;
    case CFM_IDX:
        if (e->xscale == 1)
            snprintf(o, n, "%%%s@(%lx,%%%s:l)", cf_reg_name(e->reg),
                     (unsigned long)e->disp, cf_reg_name(e->xreg));
        else
            snprintf(o, n, "%%%s@(%lx,%%%s:l:%d)", cf_reg_name(e->reg),
                     (unsigned long)e->disp, cf_reg_name(e->xreg), e->xscale);
        return;
    case CFM_ABSW: case CFM_ABSL:
        snprintf(o, n, "0x%lx", (unsigned long)e->disp & 0xffffffffUL);
        return;
    case CFM_PCDISP:
        snprintf(o, n, "%%pc@({+%ld})", ext + e->disp);
        return;
    case CFM_PCIDX:
        snprintf(o, n, "%%pc@({+%ld},%%%s:l)", ext + e->disp,
                 cf_reg_name(e->xreg));
        return;
    case CFM_IMM: {
        long v = e->imm;
        if (size == 1) v = (long)(signed char)(v & 0xff);
        else if (size == 2) v = (long)(short)(v & 0xffff);
        else v = (long)(int)(v & 0xffffffffL);
        snprintf(o, n, "#%ld", v);
        return;
    }
    }
}

static char v_sz(int size)
{
    return size == 1 ? 'b' : size == 2 ? 'w' : 'l';
}

void cfasm_vocabulary(FILE *f)
{
    char a[96], b[96], ma[96], mb[96];
    static const char *const alu_n[] = { "add", "sub", "and", "or", "eor",
                                         "cmp" };

    /* moves: each size, each pair of kinds ColdFire has, every register in
     * each field, every spelling */
    for (int size = 1; size <= 4; size *= 2)
        for (int ks = 0; ks < V_NSAMPLE; ks++)
            for (int kd = 0; kd < 10; kd++)
                for (int r = 0; r < 8; r++) {
                    struct cf_ea s = v_sample(ks, r), d = v_sample(kd, 7 - r);
                    int sty = (r + ks + kd) % 4;
                    if (kd == 8)
                        continue;
                    if (ks == 10)
                        s.imm = size == 1 ? (r & 1 ? -128 : 127)
                              : size == 2 ? (r & 1 ? -32768 : 32767)
                              : (r & 1 ? -2147483647L - 1 : 0x7fffffffL);
                    if (!cf_move_ok(size, &s, &d))
                        continue;
                    v_mot(a, sizeof a, &s, size, sty);
                    v_mot(b, sizeof b, &d, size, sty);
                    v_mit(ma, sizeof ma, &s, size, 2);
                    v_mit(mb, sizeof mb, &d, size, 2 + cf_ea_ext_len(&s, size));
                    fprintf(f, "%s.%c %s,%s`move%s%c %s,%s\n",
                            d.mode == CFM_A && r & 1 ? "movea" : "move",
                            v_sz(size), a, b, d.mode == CFM_A ? "a" : "",
                            v_sz(size), ma, mb);
                }
    /* move without a size is move.w, as GNU as has it; move.l #small,Dn is
     * moveq */
    fprintf(f, "move %%d1,%%d2`movew %%d1,%%d2\n");
    fprintf(f, "move.l #-128,%%d5`moveq #-128,%%d5\n");
    fprintf(f, "move.l #127,%%d0`moveq #127,%%d0\n");
    fprintf(f, "move.l #128,%%d0`movel #128,%%d0\n");
    fprintf(f, "movea.l #5,%%a3`moveal #5,%%a3\n");
    for (long v = -128; v <= 127; v += 51)
        for (int r = 0; r < 8; r += 3)
            fprintf(f, "moveq #%ld,%%d%d`moveq #%ld,%%d%d\n", v, r, v, r);

    /* lea, pea, jsr, jmp: the control modes */
    for (int r = 0; r < 8; r++) {
        static const int kinds[] = { 2, 5, 6, 7, 8, 9 };
        for (int k = 0; k < 6; k++) {
            struct cf_ea s = v_sample(kinds[k], r);
            v_mot(a, sizeof a, &s, 4, (r + k) % 4);
            v_mit(ma, sizeof ma, &s, 4, 2);
            fprintf(f, "lea %s,%s`lea %s,%%%s\n", a, v_reg(8 + (7 - r), r % 3),
                    ma, cf_reg_name(8 + (7 - r)));
            fprintf(f, "pea %s`pea %s\njsr %s`jsr %s\njmp %s`jmp %s\n", a, ma,
                    a, ma, a, ma);
        }
    }
    fprintf(f, "lea.l (-60,%%sp),%%sp`lea %%sp@(-60),%%sp\n");

    /* movem: every register's bit, both ways, lists and ranges */
    for (int r = 0; r < 16; r++) {
        fprintf(f, "movem.l %s,(%d,%%sp)`moveml %%%s,%%sp@(%d)\n", v_reg(r, 0),
                4 * r, cf_reg_name(r), 4 * r);
        fprintf(f, "movem.l %d(%%fp),%s`moveml %%fp@(%d),%%%s\n", -4 * r - 4,
                v_reg(r, r & 1), -4 * r - 4, cf_reg_name(r));
    }
    fprintf(f, "movem.l %%d2-%%d7/%%a2-%%a5,(%%sp)`moveml "
               "%%d2-%%d7/%%a2-%%a5,%%sp@\n");
    fprintf(f, "movem.l (%%a0),%%d2-%%d7/%%a2-%%a5`moveml "
               "%%a0@,%%d2-%%d7/%%a2-%%a5\n");
    fprintf(f, "movem.l %%d0-%%fp,(%%sp)`moveml %%d0-%%fp,%%sp@\n");
    fprintf(f, "movem.l D6-D7,(SP)`moveml %%d6-%%d7,%%sp@\n");
    fprintf(f, "movem.l %%d0/%%d3/%%a1,(8,%%a2)`moveml "
               "%%d0/%%d3/%%a1,%%a2@(8)\n");

    /* the ALU */
    for (int op = CF_ADD; op <= CF_CMP; op++) {
        for (int k = 0; k < V_NSAMPLE; k++)
            for (int r = 0; r < 8; r++) {
                struct cf_ea s = v_sample(k, r);
                if (op == CF_EOR || k == 9)
                    continue;
                if (k == 1 && (op == CF_AND || op == CF_OR))
                    continue;
                if (k == 10)
                    s.imm = r & 1 ? -2147483647L - 1 : 0x7fffffffL;
                v_mot(a, sizeof a, &s, 4, (r + k) % 4);
                v_mit(ma, sizeof ma, &s, 4, 2);
                fprintf(f, "%s.l %s,%s`%s%sl %s,%%%s\n", alu_n[op], a,
                        v_reg(7 - r, k % 3), alu_n[op], k == 10 ? "i" : "",
                        ma, cf_reg_name(7 - r));
            }
        if (op == CF_CMP)
            continue;
        for (int k = 2; k < 8; k++)
            for (int r = 0; r < 8; r++) {
                struct cf_ea d = v_sample(k, 7 - r);
                v_mot(a, sizeof a, &d, 4, (r + k) % 4);
                v_mit(ma, sizeof ma, &d, 4, 2);
                fprintf(f, "%s.l %s,%s`%sl %%%s,%s\n", alu_n[op],
                        v_reg(r, k % 3), a, alu_n[op], cf_reg_name(r), ma);
            }
    }
    for (int r = 0; r < 8; r++)
        fprintf(f, "eor.l %%d%d,%%d%d`eorl %%d%d,%%d%d\n", r, 7 - r, r, 7 - r);
    for (int op = CF_ADD; op <= CF_CMP; op++) {
        if (op != CF_ADD && op != CF_SUB && op != CF_CMP)
            continue;
        for (int k = 0; k < V_NSAMPLE; k++)
            for (int r = 0; r < 8; r++) {
                struct cf_ea s = v_sample(k, r);
                if (k == 9)
                    continue;
                v_mot(a, sizeof a, &s, 4, (r + k) % 4);
                v_mit(ma, sizeof ma, &s, 4, 2);
                fprintf(f, "%sa.l %s,%s`%sal %s,%%%s\n", alu_n[op], a,
                        v_reg(8 + (7 - r), 0), alu_n[op], ma,
                        cf_reg_name(8 + (7 - r)));
            }
    }
    for (int op = CF_ADD; op <= CF_CMP; op++)
        for (int r = 0; r < 8; r++) {
            static const long iv[] = { -2147483647L - 1, 0x7fffffffL, -1, 9,
                                       -9, 0, 1000, 65535 };
            fprintf(f, "%si.l #%ld,%%d%d`%sil #%ld,%%d%d\n", alu_n[op], iv[r],
                    r, alu_n[op], iv[r], r);
        }
    for (int n = 1; n <= 8; n++)
        for (int k = 0; k < 8; k++) {
            struct cf_ea d = v_sample(k, n);
            v_mot(a, sizeof a, &d, 4, (n + k) % 4);
            v_mit(ma, sizeof ma, &d, 4, 2);
            fprintf(f, "addq.l #%d,%s`addql #%d,%s\n", n, a, n, ma);
            fprintf(f, "%s.l #%d,%s`subql #%d,%s\n", k & 1 ? "sub" : "subq", n,
                    a, n, ma);
        }
    fprintf(f, "add.l #9,%%d1`addil #9,%%d1\nadd.l #8,%%a1`addql #8,%%a1\n");
    fprintf(f, "add.l #9,%%a1`addal #9,%%a1\nsub.l #1,(%%a3)`subql #1,%%a3@\n");
    fprintf(f, "cmp.l #5,%%d3`cmpil #5,%%d3\ncmp.l #5,%%a3`cmpal #5,%%a3\n");
    fprintf(f, "and.l #0xff,%%d4`andil #255,%%d4\n");
    for (int x = 0; x < 8; x++)
        for (int y = 0; y < 8; y += 3) {
            fprintf(f, "addx.l %%d%d,%%d%d`addxl %%d%d,%%d%d\n", y, x, y, x);
            fprintf(f, "subx.l %%d%d,%%d%d`subxl %%d%d,%%d%d\n", x, y, x, y);
        }

    /* one register, clr, tst */
    for (int r = 0; r < 8; r++) {
        fprintf(f, "neg.l %%d%d`negl %%d%d\nnegx.l %%d%d`negxl %%d%d\n", r, r,
                r, r);
        fprintf(f, "not.l %%d%d`notl %%d%d\nswap %%d%d`swap %%d%d\n", r, r, r,
                r);
        fprintf(f, "ext.w %%d%d`extw %%d%d\next.l %%d%d`extl %%d%d\n", r, r,
                r, r);
        fprintf(f, "extb.l %%d%d`extbl %%d%d\n", r, r);
    }
    for (int size = 1; size <= 4; size *= 2)
        for (int k = 0; k < 8; k++)
            for (int r = 0; r < 8; r += 2) {
                struct cf_ea d = v_sample(k, r);
                if (k == 1)
                    continue;
                v_mot(a, sizeof a, &d, size, (r + k) % 4);
                v_mit(ma, sizeof ma, &d, size, 2);
                fprintf(f, "clr.%c %s`clr%c %s\ntst.%c %s`tst%c %s\n",
                        v_sz(size), a, v_sz(size), ma, v_sz(size), a,
                        v_sz(size), ma);
            }

    /* shifts */
    {
        static const char *const sh[] = { "asl", "asr", "lsl", "lsr" };
        for (int op = 0; op < 4; op++)
            for (int r = 0; r < 8; r++) {
                fprintf(f, "%s.l #%d,%%d%d`%sl #%d,%%d%d\n", sh[op], r + 1,
                        7 - r, sh[op], r + 1, 7 - r);
                fprintf(f, "%s.l %%d%d,%%d%d`%sl %%d%d,%%d%d\n", sh[op], r,
                        7 - r, sh[op], r, 7 - r);
            }
    }

    /* multiply, divide, remainder */
    for (int sign = 0; sign < 2; sign++)
        for (int r = 0; r < 8; r++) {
            static const int kw[] = { 0, 2, 3, 4, 5, 6, 7, 8, 10 };
            static const int kl[] = { 0, 2, 3, 4, 5 };
            char c = sign ? 's' : 'u';
            for (int k = 0; k < 9; k++) {
                struct cf_ea s = v_sample(kw[k], r);
                if (kw[k] == 10)
                    s.imm = r & 1 ? -32768 : 32767;
                v_mot(a, sizeof a, &s, 2, (r + k) % 4);
                v_mit(ma, sizeof ma, &s, 2, 2);
                fprintf(f, "mul%c%s %s,%%d%d`mul%cw %s,%%d%d\n", c,
                        k & 1 ? ".w" : "", a, 7 - r, c, ma, 7 - r);
            }
            for (int k = 0; k < 5; k++) {
                struct cf_ea s = v_sample(kl[k], r);
                int q = 7 - r, rr = r == q ? 0 : r;
                v_mot(a, sizeof a, &s, 4, (r + k) % 4);
                v_mit(ma, sizeof ma, &s, 4, 4);
                fprintf(f, "mul%c.l %s,%%d%d`mul%cl %s,%%d%d\n", c, a, q, c,
                        ma, q);
                fprintf(f, "div%c.l %s,%%d%d`div%cll %s,%%d%d,%%d%d\n", c, a,
                        q, c, ma, q, q);
                fprintf(f, "rem%c.l %s,%%d%d:%%d%d`div%cll %s,%%d%d,%%d%d\n",
                        c, a, rr, q, c, ma, rr, q);
            }
        }

    /* conditions and branches, at the ends of their reach */
    for (int cond = 0; cond < 16; cond++)
        for (int r = 0; r < 8; r += 7)
            fprintf(f, "s%s %%d%d`s%s %%d%d\n", cond_name[cond],
                    r == 7 ? cond & 7 : r, cond_name[cond],
                    r == 7 ? cond & 7 : r);
    fprintf(f, "shs %%d1`scc %%d1\nslo %%d2`scs %%d2\n");
    for (int cond = 0; cond < 16; cond++) {
        static const long wd[] = { -32766, 32769, 4 };
        static const long sd[] = { -126, 129, 4 };
        const char *cn = cond == 0 ? "ra" : cond_name[cond];
        if (cond == 1)
            continue;
        for (int k = 0; k < 3; k++) {
            fprintf(f, "b%s.w .%+ld`b%sw {+%ld}\n", cn, wd[k], cn, wd[k]);
            fprintf(f, "b%s.s .%+ld`b%ss {+%ld}\n", cn, sd[k], cn, sd[k]);
        }
        /* no size: the shortest that reaches */
        fprintf(f, "b%s .+100`b%ss {+100}\nb%s .-200`b%sw {+-200}\n", cn, cn,
                cn, cn);
        fprintf(f, "b%s .+2`b%sw {+2}\nb%s .+32769`b%sw {+32769}\n", cn,
                cn, cn, cn);
    }
    fprintf(f, "bhs.s .+8`bccs {+8}\nblo.w .+8`bcsw {+8}\n");
    fprintf(f, "bsr.s .+20`bsrs {+20}\nbsr.w .-1000`bsrw {+-1000}\n");
    fprintf(f, "bsr .+20`bsrs {+20}\n");
    fprintf(f, "bsr .+1000`bsrw {+1000}\n");

    /* the rest */
    fprintf(f, "rts`rts\nrte`rte\nnop`nop\nhalt`halt\nillegal`illegal\n");
    for (int v = 0; v < 16; v += 5)
        fprintf(f, "trap #%d`trap #%d\n", v, v);
    fprintf(f, "stop #0x2700`stop #9984\nstop #0`stop #0\n");
    for (int r = 8; r < 16; r++) {
        fprintf(f, "link %s,#%d`linkw %%%s,#%d\n", v_reg(r, r & 1),
                r & 1 ? -32768 : 32767, cf_reg_name(r), r & 1 ? -32768 : 32767);
        fprintf(f, "link.w %%%s,#-8`linkw %%%s,#-8\n", cf_reg_name(r),
                cf_reg_name(r));
        fprintf(f, "unlk %s`unlk %%%s\n", v_reg(r, r & 1), cf_reg_name(r));
    }
    for (int r = 0; r < 8; r++) {
        fprintf(f, "move.w %%sr,%%d%d`movew %%sr,%%d%d\n", r, r);
        fprintf(f, "move.w %%d%d,%%sr`movew %%d%d,%%sr\n", r, r);
        fprintf(f, "move.w %%ccr,%%d%d`movew %%ccr,%%d%d\n", r, r);
        fprintf(f, "move.w %%d%d,%%ccr`movew %%d%d,%%ccr\n", r, r);
        fprintf(f, "move.l %%usp,%%a%d`movel %%usp,%%%s\n", r,
                cf_reg_name(8 + r));
        fprintf(f, "move.l %%a%d,%%usp`movel %%%s,%%usp\n", r,
                cf_reg_name(8 + r));
    }
    fprintf(f, "move.w #0x2700,%%sr`movew #9984,%%sr\n");
    fprintf(f, "move.w SR,D7`movew %%sr,%%d7\nmove.w D7,SR`movew %%d7,%%sr\n");
    fprintf(f, "move.w #0x1f,%%ccr`movew #31,%%ccr\n");
    {
        static const char *const cr[] = { "cacr", "vbr" };
        for (int k = 0; k < 2; k++)
            for (int r = 0; r < 16; r += 5)
                fprintf(f, "movec %%%s,%%%s`movec %%%s,%%%s\n", cf_reg_name(r),
                        cr[k], cf_reg_name(r), cr[k]);
    }
    /* the bit instructions */
    {
        static const char *const bn[] = { "btst", "bchg", "bclr", "bset" };
        for (int op = 0; op < 4; op++)
            for (int r = 0; r < 8; r++) {
                static const int km[] = { 2, 3, 4, 5 };
                struct cf_ea m = v_sample(km[r & 3], r);
                /* the disassembler prints these without a size */
                fprintf(f, "%s #%d,%%d%d`%s #%d,%%d%d\n", bn[op], r * 4 + 3,
                        7 - r, bn[op], r * 4 + 3, 7 - r);
                fprintf(f, "%s %%d%d,%%d%d`%s %%d%d,%%d%d\n", bn[op], r,
                        7 - r, bn[op], r, 7 - r);
                v_mot(a, sizeof a, &m, 1, r % 4);
                v_mit(ma, sizeof ma, &m, 1, 4);
                fprintf(f, "%s.b #%d,%s`%s #%d,%s\n", bn[op], r, a, bn[op], r,
                        ma);
                v_mit(ma, sizeof ma, &m, 1, 2);
                fprintf(f, "%s %%d%d,%s`%s %%d%d,%s\n", bn[op], 7 - r, a,
                        bn[op], 7 - r, ma);
            }
    }
}
