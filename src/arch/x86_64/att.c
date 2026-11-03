/* See att.h. One statement at a time: parse the operands, pick the form,
 * build the bytes in an `ib` (prefixes, REX, opcode, ModRM/SIB,
 * displacement, immediate), and append them. */
#include "att.h"

#include "../asmexpr.h"
#include "../../driver/util.h"
#include "../../elf/elf.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- registers ---------------------------------------------------------- */

enum { K_NONE, K_REG, K_XMM, K_MEM, K_IMM, K_CR, K_DR, K_SEG };

struct reg { const char *n; int num, size, hi8, rex8, kind; };

static const struct reg regs[] = {
    {"rax",0,8,0,0,K_REG},{"rcx",1,8,0,0,K_REG},{"rdx",2,8,0,0,K_REG},
    {"rbx",3,8,0,0,K_REG},{"rsp",4,8,0,0,K_REG},{"rbp",5,8,0,0,K_REG},
    {"rsi",6,8,0,0,K_REG},{"rdi",7,8,0,0,K_REG},
    {"eax",0,4,0,0,K_REG},{"ecx",1,4,0,0,K_REG},{"edx",2,4,0,0,K_REG},
    {"ebx",3,4,0,0,K_REG},{"esp",4,4,0,0,K_REG},{"ebp",5,4,0,0,K_REG},
    {"esi",6,4,0,0,K_REG},{"edi",7,4,0,0,K_REG},
    {"ax",0,2,0,0,K_REG},{"cx",1,2,0,0,K_REG},{"dx",2,2,0,0,K_REG},
    {"bx",3,2,0,0,K_REG},{"sp",4,2,0,0,K_REG},{"bp",5,2,0,0,K_REG},
    {"si",6,2,0,0,K_REG},{"di",7,2,0,0,K_REG},
    {"al",0,1,0,0,K_REG},{"cl",1,1,0,0,K_REG},{"dl",2,1,0,0,K_REG},
    {"bl",3,1,0,0,K_REG},{"spl",4,1,0,1,K_REG},{"bpl",5,1,0,1,K_REG},
    {"sil",6,1,0,1,K_REG},{"dil",7,1,0,1,K_REG},
    {"ah",4,1,1,0,K_REG},{"ch",5,1,1,0,K_REG},{"dh",6,1,1,0,K_REG},
    {"bh",7,1,1,0,K_REG},
    {"es",0,2,0,0,K_SEG},{"cs",1,2,0,0,K_SEG},{"ss",2,2,0,0,K_SEG},
    {"ds",3,2,0,0,K_SEG},{"fs",4,2,0,0,K_SEG},{"gs",5,2,0,0,K_SEG},
};

/* %name (without the %): its entry, r8..r15 and xmm/cr/dr by pattern */
static int reg_lookup(const char *s, int n, struct reg *out)
{
    for (unsigned k = 0; k < sizeof regs / sizeof *regs; k++)
        if ((int)strlen(regs[k].n) == n && !strncmp(regs[k].n, s, (size_t)n)) {
            *out = regs[k];
            return 1;
        }
    memset(out, 0, sizeof *out);
    if (n >= 2 && s[0] == 'r' && isdigit((unsigned char)s[1])) {
        int v = 0, i = 1;
        while (i < n && isdigit((unsigned char)s[i]))
            v = v * 10 + (s[i++] - '0');
        if (v < 8 || v > 15)
            return 0;
        int size = 8;
        if (i < n) {
            if (i + 1 != n) return 0;
            size = s[i] == 'd' ? 4 : s[i] == 'w' ? 2 : s[i] == 'b' || s[i] == 'l' ? 1 : 0;
            if (!size) return 0;
        }
        out->n = "r"; out->num = v; out->size = size; out->kind = K_REG;
        return 1;
    }
    const char *pfx[] = { "xmm", "cr", "dr" };
    int kinds[] = { K_XMM, K_CR, K_DR };
    for (int p = 0; p < 3; p++) {
        int pl = (int)strlen(pfx[p]);
        if (n > pl && !strncmp(s, pfx[p], (size_t)pl)) {
            int v = 0;
            for (int i = pl; i < n; i++) {
                if (!isdigit((unsigned char)s[i])) return 0;
                v = v * 10 + (s[i] - '0');
            }
            if (v > 15) return 0;
            out->n = pfx[p]; out->num = v; out->size = p == 0 ? 16 : 8;
            out->kind = kinds[p];
            return 1;
        }
    }
    if (n == 3 && !strncmp(s, "rip", 3)) {
        out->n = "rip"; out->num = 16; out->size = 8; out->kind = K_REG;
        return 1;
    }
    return 0;
}

/* (gas.c's contract: the register's number, or -1) */
int x86att_is_reg(const char *name, int len)
{
    struct reg r;
    return reg_lookup(name, len, &r) ? r.num : -1;
}

static int is_prefix(const char *s, int n)
{
    static const char *const pf[] = { "lock", "rep", "repe", "repz", "repne",
                                      "repnz", "rex64", "near" };
    for (unsigned k = 0; k < sizeof pf / sizeof *pf; k++)
        if ((int)strlen(pf[k]) == n && !strncmp(pf[k], s, (size_t)n))
            return 1;
    return 0;
}

/* where the mnemonic starts, past `lock` / `rep` prefixes */
static const char *after_prefixes(const char *s)
{
    for (;;) {
        while (isspace((unsigned char)*s)) s++;
        const char *w = s;
        while (*s && !isspace((unsigned char)*s)) s++;
        if (!is_prefix(w, (int)(s - w)))
            return w;
    }
}

/* the words in an operand that are not symbols: relocation operators,
 * registers, and the mnemonic after a prefix */
int x86att_is_word(const char *stmt, const char *w, int len)
{
    static const char *const ops[] = { "PLT", "GOTPCREL", "GOT", "GOTOFF",
                                       "TPOFF", "GOTTPOFF", "TLSGD", "near" };
    if (w > stmt && w[-1] == '%')
        return 1;                      /* %anything is a register's */
    if (len >= 1 && w[0] == '$' && (len == 1 || !asm_sym0((unsigned char)w[1])))
        return 1;                      /* `$8`: an immediate, not a name */
    if (w == after_prefixes(stmt))
        return 1;
    for (unsigned k = 0; k < sizeof ops / sizeof *ops; k++)
        if ((int)strlen(ops[k]) == len && !strncmp(ops[k], w, (size_t)len))
            return 1;
    return 0;
}

/* ---- the statement being assembled -------------------------------------- */

struct opnd {
    int k;
    int reg, size, hi8, rex8;          /* K_REG/K_XMM/K_CR/K_DR/K_SEG */
    int base, index, scale;            /* K_MEM: -1 none, 16 rip */
    long long disp;
    int dot;                           /* rip disp is .+N (from the start) */
    int seg;                           /* segment override prefix, 0 none */
    long long imm;                     /* K_IMM */
    int star;                          /* `*target` */
};

struct ib {
    unsigned char b[32];
    int n;
    int disp_at, disp_len;             /* the displacement field */
    int dotrip;                        /* fix the rip disp at the end */
    long long dotn;
    int imm_at;
};

static char *g_err;
static int g_errlen;
static int g_level = -1, g_took;
/* (x86att_symform's: where the last statement's fields were) */
static int g_last_disp_at = -1, g_last_imm_at = -1, g_last_len;

void x86att_set_level(int level) { g_level = level; g_took = 0; }
int x86att_took_level(void) { return g_took; }

static int fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err, (size_t)g_errlen, fmt, ap);
    va_end(ap);
    return -1;
}

static void put(struct ib *x, unsigned v) { x->b[x->n++] = (unsigned char)v; }
static void put_n(struct ib *x, long long v, int n)
{
    for (int i = 0; i < n; i++)
        put(x, (unsigned)((unsigned long long)v >> (8 * i)) & 0xff);
}

static int fits8(long long v) { return v >= -128 && v <= 127; }
static int fits32(long long v) { return v >= -2147483648LL && v <= 2147483647LL; }

/* ---- operands ----------------------------------------------------------- */

static int parse_num(const char *s, int n, long long *v)
{
    while (n > 0 && isspace((unsigned char)*s)) { s++; n--; }
    while (n > 0 && isspace((unsigned char)s[n - 1])) n--;
    if (n == 0) { *v = 0; return 1; }
    if (n >= 2 && s[0] == '.' && (s[1] == '+' || s[1] == '-')) {
        /* (.+N as a plain number: a branch's displacement) */
        long long r;
        if (!asm_const_expr(s + 1, n - 1, &r)) return 0;
        *v = r;
        return 1;
    }
    if (n == 1 && s[0] == '.') { *v = 0; return 1; }
    return asm_const_expr(s, n, v);
}

static int parse_reg(const char *s, int n, struct opnd *o)
{
    struct reg r;
    if (n < 2 || s[0] != '%' || !reg_lookup(s + 1, n - 1, &r))
        return 0;
    o->k = r.kind;
    o->reg = r.num;
    o->size = r.size;
    o->hi8 = r.hi8;
    o->rex8 = r.rex8;
    return 1;
}

static int parse_opnd(const char *s, int n, struct opnd *o)
{
    memset(o, 0, sizeof *o);
    o->base = o->index = -1;
    o->scale = 1;
    while (n > 0 && isspace((unsigned char)*s)) { s++; n--; }
    while (n > 0 && isspace((unsigned char)s[n - 1])) n--;
    if (n == 0)
        return fail("an empty operand");
    if (*s == '*') { o->star = 1; s++; n--; }
    if (*s == '$') {
        o->k = K_IMM;
        if (!parse_num(s + 1, n - 1, &o->imm))
            return fail("'%.*s' is not a constant", n, s);
        return 0;
    }
    /* %seg: memory */
    if (n > 4 && s[0] == '%' && s[3] == ':') {
        struct reg r;
        if (reg_lookup(s + 1, 2, &r) && r.kind == K_SEG) {
            static const unsigned char pf[] = { 0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65 };
            int seg = pf[r.num];
            if (parse_opnd(s + 4, n - 4, o) < 0) return -1;
            if (o->k != K_MEM) return fail("a segment override on a non-memory operand");
            o->seg = seg;
            return 0;
        }
    }
    if (*s == '%') {
        if (!parse_reg(s, n, o))
            return fail("'%.*s' is not an x86-64 register", n, s);
        return 0;
    }
    /* memory: disp(base, index, scale), any part optional */
    const char *lp = memchr(s, '(', (size_t)n);
    o->k = K_MEM;
    const char *dend = lp ? lp : s + n;
    int dn = (int)(dend - s);
    while (dn > 0 && isspace((unsigned char)s[dn - 1])) dn--;
    if (dn >= 2 && s[0] == '.' && (s[1] == '+' || s[1] == '-')) {
        o->dot = 1;
        long long r;
        if (!asm_const_expr(s + 1, dn - 1, &r))
            return fail("'%.*s' is not a displacement", dn, s);
        o->disp = r;
    } else if (!parse_num(s, dn, &o->disp)) {
        return fail("'%.*s' is not a displacement this assembler can take "
                    "(a symbol is the file assembler's)", dn, s);
    }
    if (!lp)
        return 0;     /* an absolute address, or (.+N) a branch's target */
    const char *rp = s + n - 1;
    if (*rp != ')')
        return fail("'%.*s': expected ')'", n, s);
    const char *q = lp + 1;
    const char *parts[3];
    int plen[3], np = 0;
    const char *st = q;
    for (; q <= rp && np < 3; q++)
        if (q == rp || *q == ',') {
            parts[np] = st;
            plen[np++] = (int)(q - st);
            st = q + 1;
        }
    for (int i = 0; i < np; i++) {
        while (plen[i] > 0 && isspace((unsigned char)*parts[i])) { parts[i]++; plen[i]--; }
        while (plen[i] > 0 && isspace((unsigned char)parts[i][plen[i] - 1])) plen[i]--;
    }
    if (np >= 1 && plen[0]) {
        struct opnd r;
        if (!parse_reg(parts[0], plen[0], &r) || r.k != K_REG || r.size != 8)
            return fail("'%.*s' is not a 64-bit base register", plen[0], parts[0]);
        o->base = r.reg;
    }
    if (np >= 2 && plen[1]) {
        struct opnd r;
        if (!parse_reg(parts[1], plen[1], &r) || r.k != K_REG || r.size != 8 ||
            r.reg == 16)
            return fail("'%.*s' is not a 64-bit index register", plen[1], parts[1]);
        if (r.reg == 4)
            return fail("%%rsp cannot be an index");
        o->index = r.reg;
    }
    if (np >= 3 && plen[2]) {
        long long sc;
        if (!parse_num(parts[2], plen[2], &sc) ||
            (sc != 1 && sc != 2 && sc != 4 && sc != 8))
            return fail("the scale must be 1, 2, 4 or 8");
        o->scale = (int)sc;
    }
    if (o->dot && o->base != 16)
        return fail("'.+N' is an address only (%%rip)-relative");
    if (o->base == 16 && o->index >= 0)
        return fail("(%%rip) takes no index");
    return 0;
}

/* ---- the encoder -------------------------------------------------------- */

enum { P66 = 1, PF2 = 2, PF3 = 4 };

/* prefixes, REX, opcode, ModRM (+SIB, disp), for an r/m operand `rm`
 * (register or memory) and a reg field `regf` (a register's number when
 * reg_is_reg, else the /digit); `w` is REX.W; `byte_regs` says the
 * registers named are byte ones (spl..dil need a REX, ah..bh refuse it) */
static int emit_rm(struct ib *x, int pfx, int w, const unsigned char *op,
                   int nop, int regf, const struct opnd *regop,
                   const struct opnd *rm)
{
    int rex = w ? 8 : 0, need = 0, hi = 0;
    if (regf >= 8) rex |= 4;
    if (regop) {
        need |= regop->rex8;
        hi |= regop->hi8;
    }
    if (rm->k == K_MEM) {
        if (rm->base >= 8 && rm->base != 16) rex |= 1;
        if (rm->index >= 8) rex |= 2;
    } else {
        if (rm->reg >= 8) rex |= 1;
        need |= rm->rex8;
        hi |= rm->hi8;
    }
    if (rm->k == K_MEM && rm->dot && rm->base != 16)
        return fail("'.+N' is an address only (%%rip)-relative");
    if (rm->k == K_MEM && rm->seg) put(x, (unsigned)rm->seg);
    if (pfx & P66) put(x, 0x66);
    if (pfx & PF2) put(x, 0xf2);
    if (pfx & PF3) put(x, 0xf3);
    if (rex || need) {
        if (hi)
            return fail("%%ah, %%bh, %%ch and %%dh cannot be used with a REX "
                        "prefix (a 64-bit operand or r8-r15)");
        put(x, 0x40u | (unsigned)rex);
    }
    for (int i = 0; i < nop; i++)
        put(x, op[i]);
    int r3 = regf & 7;
    if (rm->k != K_MEM) {
        put(x, 0xc0u | (unsigned)r3 << 3 | (unsigned)(rm->reg & 7));
        return 0;
    }
    if (rm->base == 16) {                       /* rip-relative */
        put(x, (unsigned)r3 << 3 | 5);
        x->disp_at = x->n;
        x->disp_len = 4;
        if (rm->dot) {
            x->dotrip = 1;
            x->dotn = rm->disp;
            put_n(x, 0, 4);
        } else {
            put_n(x, rm->disp, 4);
        }
        return 0;
    }
    if (rm->base < 0) {                         /* no base: disp32 */
        put(x, (unsigned)r3 << 3 | 4);
        int sc = rm->scale == 8 ? 3 : rm->scale == 4 ? 2 : rm->scale == 2 ? 1 : 0;
        put(x, (unsigned)sc << 6 | (unsigned)((rm->index >= 0 ? rm->index : 4) & 7) << 3 | 5);
        if (!fits32(rm->disp)) return fail("the displacement does not fit 32 bits");
        x->disp_at = x->n; x->disp_len = 4;
        put_n(x, rm->disp, 4);
        return 0;
    }
    int b3 = rm->base & 7;
    int mod = rm->disp == 0 && b3 != 5 ? 0 : fits8(rm->disp) ? 1 : 2;
    if (!fits32(rm->disp)) return fail("the displacement does not fit 32 bits");
    if (rm->index >= 0 || b3 == 4) {
        int sc = rm->scale == 8 ? 3 : rm->scale == 4 ? 2 : rm->scale == 2 ? 1 : 0;
        put(x, (unsigned)mod << 6 | (unsigned)r3 << 3 | 4);
        put(x, (unsigned)sc << 6 | (unsigned)((rm->index >= 0 ? rm->index : 4) & 7) << 3 |
               (unsigned)b3);
    } else {
        put(x, (unsigned)mod << 6 | (unsigned)r3 << 3 | (unsigned)b3);
    }
    if (mod == 1) { x->disp_at = x->n; x->disp_len = 1; put_n(x, rm->disp, 1); }
    if (mod == 2) { x->disp_at = x->n; x->disp_len = 4; put_n(x, rm->disp, 4); }
    return 0;
}

static void imm_put(struct ib *x, long long v, int n)
{
    x->imm_at = x->n;
    put_n(x, v, n);
}

/* a register opcode (50+r, B8+r, C8+r): prefixes, REX.B, the byte */
static int emit_oreg(struct ib *x, int pfx, int w, const unsigned char *op,
                     int nop, const struct opnd *r)
{
    int rex = (w ? 8 : 0) | (r->reg >= 8 ? 1 : 0);
    if (pfx & P66) put(x, 0x66);
    if (pfx & PF3) put(x, 0xf3);
    if (rex || r->rex8) {
        if (r->hi8) return fail("%%ah..%%bh cannot take a REX prefix");
        put(x, 0x40u | (unsigned)rex);
    }
    for (int i = 0; i < nop - 1; i++)
        put(x, op[i]);
    put(x, op[nop - 1] + (unsigned)(r->reg & 7));
    return 0;
}

/* ---- mnemonics ---------------------------------------------------------- */

/* `mn` is `base` with an optional size suffix: the suffix's size (1, 2, 4,
 * 8), 0 for none, -1 when it is not this mnemonic */
static int sized(const char *mn, const char *base)
{
    size_t n = strlen(base), m = strlen(mn);
    if (m == n && !strcmp(mn, base)) return 0;
    if (m == n + 1 && !strncmp(mn, base, n)) {
        switch (mn[n]) {
        case 'b': return 1;
        case 'w': return 2;
        case 'l': return 4;
        case 'q': return 8;
        }
    }
    return -1;
}

static const char *const ccname[16][3] = {
    {"o"}, {"no"}, {"b","c","nae"}, {"ae","nb","nc"}, {"e","z"}, {"ne","nz"},
    {"be","na"}, {"a","nbe"}, {"s"}, {"ns"}, {"p","pe"}, {"np","po"},
    {"l","nge"}, {"ge","nl"}, {"le","ng"}, {"g","nle"},
};

static int cond_of(const char *s)
{
    for (int c = 0; c < 16; c++)
        for (int k = 0; k < 3 && ccname[c][k]; k++)
            if (!strcmp(s, ccname[c][k]))
                return c;
    return -1;
}

/* the operation size: the suffix, else a register operand's */
static int opsize(int sfx, const struct opnd *o, int n)
{
    if (sfx > 0) return sfx;
    for (int i = n - 1; i >= 0; i--)
        if (o[i].k == K_REG && o[i].reg != 16)
            return o[i].size;
    return 0;
}

static int need_size(int sz, const char *mn)
{
    if (!sz)
        return fail("'%s': the operand size is unknown -- add a b, w, l or q "
                    "suffix", mn);
    return 0;
}

/* a register operand of the operation's size */
static int is_reg_sz(const struct opnd *o, int sz)
{
    return o->k == K_REG && o->reg != 16 && o->size == sz;
}

static int is_rm(const struct opnd *o, int sz)
{
    return o->k == K_MEM || is_reg_sz(o, sz);
}

static int pfx_of(int sz) { return sz == 2 ? P66 : 0; }

/* r/m + reg ALU ops: add or adc sbb and sub xor cmp (ext 0-7) */
static int alu(struct ib *x, int ext, int sz, const struct opnd *src,
               const struct opnd *dst)
{
    int w = sz == 8, p = pfx_of(sz);
    if (src->k == K_IMM) {
        if (!is_rm(dst, sz)) return fail("the destination must be a register or memory");
        long long v = src->imm;
        if (sz == 1) {
            if (dst->k == K_REG && dst->reg == 0 && !dst->hi8) {
                unsigned char o = (unsigned char)(4 + 8 * ext);
                put(x, o); imm_put(x, v, 1);
                return 0;
            }
            unsigned char o = 0x80;
            if (emit_rm(x, p, 0, &o, 1, ext, NULL, dst) < 0) return -1;
            imm_put(x, v, 1);
            return 0;
        }
        if (sz == 4 || sz == 2 ? (v < -2147483648LL || v > 4294967295LL)
                               : !fits32(v))
            return fail("the immediate does not fit %d bits", sz == 8 ? 32 : sz * 8);
        long long sv = sz == 2 ? (long long)(short)v : sz == 4 ? (long long)(int)v : v;
        if (fits8(sv)) {
            unsigned char o = 0x83;
            if (emit_rm(x, p, w, &o, 1, ext, NULL, dst) < 0) return -1;
            imm_put(x, sv, 1);
            return 0;
        }
        if (dst->k == K_REG && dst->reg == 0) {
            if (p) put(x, 0x66);
            if (w) put(x, 0x48);
            put(x, (unsigned)(5 + 8 * ext));
            imm_put(x, sv, sz == 2 ? 2 : 4);
            return 0;
        }
        unsigned char o = 0x81;
        if (emit_rm(x, p, w, &o, 1, ext, NULL, dst) < 0) return -1;
        imm_put(x, sv, sz == 2 ? 2 : 4);
        return 0;
    }
    if (is_reg_sz(src, sz) && is_rm(dst, sz)) {
        unsigned char o = (unsigned char)(8 * ext + (sz == 1 ? 0 : 1));
        return emit_rm(x, p, w, &o, 1, src->reg, src, dst);
    }
    if (src->k == K_MEM && is_reg_sz(dst, sz)) {
        unsigned char o = (unsigned char)(8 * ext + (sz == 1 ? 2 : 3));
        return emit_rm(x, p, w, &o, 1, dst->reg, dst, src);
    }
    return fail("these operands do not go together (or a register's size "
                "is not the operation's)");
}

/* the statement, its operands parsed; 0, or -1 with the error set */
static int insn(struct ib *x, const char *mn, struct opnd *o, int n)
{
    int s;
    /* ---- no operands ---- */
    static const struct { const char *n; unsigned char b[4]; int len; } plain[] = {
        {"nop",{0x90},1}, {"hlt",{0xf4},1}, {"cli",{0xfa},1}, {"sti",{0xfb},1},
        {"cld",{0xfc},1}, {"std",{0xfd},1}, {"clc",{0xf8},1}, {"stc",{0xf9},1},
        {"cmc",{0xf5},1}, {"pause",{0xf3,0x90},2}, {"int3",{0xcc},1},
        {"ud2",{0x0f,0x0b},2}, {"cpuid",{0x0f,0xa2},2}, {"rdtsc",{0x0f,0x31},2},
        {"rdtscp",{0x0f,0x01,0xf9},3}, {"rdmsr",{0x0f,0x32},2},
        {"wrmsr",{0x0f,0x30},2}, {"rdpmc",{0x0f,0x33},2},
        {"syscall",{0x0f,0x05},2}, {"sysretq",{0x48,0x0f,0x07},3},
        {"sysretl",{0x0f,0x07},2}, {"swapgs",{0x0f,0x01,0xf8},3},
        {"iretq",{0x48,0xcf},2}, {"iret",{0xcf},1}, {"iretl",{0xcf},1},
        {"lret",{0xcb},1}, {"lretq",{0x48,0xcb},2},
        {"mfence",{0x0f,0xae,0xf0},3}, {"lfence",{0x0f,0xae,0xe8},3},
        {"sfence",{0x0f,0xae,0xf8},3}, {"wbinvd",{0x0f,0x09},2},
        {"invd",{0x0f,0x08},2}, {"pushfq",{0x9c},1}, {"popfq",{0x9d},1},
        {"pushf",{0x9c},1}, {"popf",{0x9d},1}, {"sahf",{0x9e},1}, {"lahf",{0x9f},1},
        {"xgetbv",{0x0f,0x01,0xd0},3}, {"xsetbv",{0x0f,0x01,0xd1},3},
        {"monitor",{0x0f,0x01,0xc8},3}, {"mwait",{0x0f,0x01,0xc9},3},
        {"stac",{0x0f,0x01,0xcb},3}, {"clac",{0x0f,0x01,0xca},3},
        {"fninit",{0xdb,0xe3},2}, {"fwait",{0x9b},1}, {"wait",{0x9b},1},
        {"emms",{0x0f,0x77},2}, {"vzeroupper",{0xc5,0xf8,0x77},3},
        {"endbr64",{0xf3,0x0f,0x1e,0xfa},4}, {"leave",{0xc9},1},
        {"leaveq",{0xc9},1},
        {"cbtw",{0x66,0x98},2}, {"cwtl",{0x98},1}, {"cltq",{0x48,0x98},2},
        {"cwtd",{0x66,0x99},2}, {"cltd",{0x99},1}, {"cqto",{0x48,0x99},2},
        {"cdqe",{0x48,0x98},2}, {"cqo",{0x48,0x99},2}, {"cdq",{0x99},1},
        {"movsb",{0xa4},1}, {"movsw",{0x66,0xa5},2}, {"movsl",{0xa5},1},
        {"movsq",{0x48,0xa5},2}, {"stosb",{0xaa},1}, {"stosw",{0x66,0xab},2},
        {"stosl",{0xab},1}, {"stosq",{0x48,0xab},2}, {"lodsb",{0xac},1},
        {"lodsw",{0x66,0xad},2}, {"lodsl",{0xad},1}, {"lodsq",{0x48,0xad},2},
        {"cmpsb",{0xa6},1}, {"cmpsw",{0x66,0xa7},2}, {"cmpsl",{0xa7},1},
        {"cmpsq",{0x48,0xa7},2}, {"scasb",{0xae},1}, {"scasw",{0x66,0xaf},2},
        {"scasl",{0xaf},1}, {"scasq",{0x48,0xaf},2},
    };
    if (n == 0) {
        for (unsigned k = 0; k < sizeof plain / sizeof *plain; k++)
            if (!strcmp(mn, plain[k].n)) {
                for (int i = 0; i < plain[k].len; i++)
                    put(x, plain[k].b[i]);
                return 0;
            }
        if (!strcmp(mn, "ret") || !strcmp(mn, "retq")) { put(x, 0xc3); return 0; }
    }
    if ((!strcmp(mn, "ret") || !strcmp(mn, "retq")) && n == 1 && o[0].k == K_IMM) {
        put(x, 0xc2); imm_put(x, o[0].imm, 2);
        return 0;
    }
    if (!strcmp(mn, "int") && n == 1 && o[0].k == K_IMM) {
        put(x, 0xcd); imm_put(x, o[0].imm, 1);
        return 0;
    }

    /* ---- control flow ---- */
    if ((!strcmp(mn, "jmp") || !strcmp(mn, "jmpq") || !strcmp(mn, "call") ||
         !strcmp(mn, "callq")) && n == 1) {
        int call = mn[0] == 'c';
        if (o[0].star) {
            unsigned char op = 0xff;
            if (o[0].k == K_REG && o[0].size != 8)
                return fail("an indirect target is a 64-bit register");
            return emit_rm(x, 0, 0, &op, 1, call ? 2 : 4, NULL, &o[0]);
        }
        long long v;
        if (o[0].k == K_MEM && o[0].base < 0 && o[0].index < 0)
            v = o[0].disp;                      /* `.+N` or a number */
        else
            return fail("'%s' takes a label, or `*` and a register or memory", mn);
        if (call) {
            put(x, 0xe8); imm_put(x, v - 5, 4);
            return 0;
        }
        if (g_level < 1 && fits8(v - 2)) {
            put(x, 0xeb); imm_put(x, v - 2, 1);
            return 0;
        }
        g_took = 1;
        put(x, 0xe9); imm_put(x, v - 5, 4);
        return 0;
    }
    if (mn[0] == 'j' && n == 1 && cond_of(mn + 1) >= 0) {
        int c = cond_of(mn + 1);
        if (o[0].k != K_MEM || o[0].base >= 0 || o[0].index >= 0)
            return fail("'%s' takes a label", mn);
        long long v = o[0].disp;
        if (g_level < 1 && fits8(v - 2)) {
            put(x, 0x70u + (unsigned)c); imm_put(x, v - 2, 1);
            return 0;
        }
        g_took = 1;
        put(x, 0x0f); put(x, 0x80u + (unsigned)c); imm_put(x, v - 6, 4);
        return 0;
    }
    if (!strcmp(mn, "jrcxz") && n == 1) {
        put(x, 0xe3); imm_put(x, o[0].disp - 2, 1);
        return 0;
    }

    /* ---- mov ---- */
    if ((s = sized(mn, "mov")) >= 0 && n == 2 && o[0].k != K_XMM &&
        o[1].k != K_XMM) {
        struct opnd *src = &o[0], *dst = &o[1];
        if (dst->k == K_SEG || src->k == K_SEG) {
            unsigned char op = dst->k == K_SEG ? 0x8e : 0x8c;
            struct opnd *sg = dst->k == K_SEG ? dst : src, *r = dst->k == K_SEG ? src : dst;
            int w = r->k == K_REG && r->size == 8 && dst->k != K_SEG;
            return emit_rm(x, r->k == K_REG && r->size == 2 && op == 0x8c ? P66 : 0,
                           w, &op, 1, sg->reg, NULL, r);
        }
        if (dst->k == K_CR || src->k == K_CR || dst->k == K_DR || src->k == K_DR) {
            int to = dst->k == K_CR || dst->k == K_DR;
            struct opnd *sp = to ? dst : src, *r = to ? src : dst;
            unsigned char op[2] = { 0x0f, (unsigned char)((sp->k == K_DR ? 0x21 : 0x20) + (to ? 2 : 0)) };
            if (r->k != K_REG || r->size != 8)
                return fail("a control or debug register moves to or from a 64-bit register");
            return emit_rm(x, 0, 0, op, 2, sp->reg, NULL, r);
        }
        int sz = opsize(s, o, 2);
        if (need_size(sz, mn) < 0) return -1;
        int w = sz == 8, p = pfx_of(sz);
        if (src->k == K_IMM) {
            long long v = src->imm;
            if (dst->k == K_REG) {
                if (!is_reg_sz(dst, sz)) return fail("a register's size is not the operation's");
                if (sz == 8 && fits32(v)) {
                    unsigned char op = 0xc7;
                    if (emit_rm(x, 0, 1, &op, 1, 0, NULL, dst) < 0) return -1;
                    imm_put(x, v, 4);
                    return 0;
                }
                unsigned char op = sz == 1 ? 0xb0 : 0xb8;
                if (emit_oreg(x, p, w, &op, 1, dst) < 0) return -1;
                imm_put(x, v, sz);
                return 0;
            }
            if (sz == 8 && !fits32(v))
                return fail("a 64-bit immediate goes to a register only (movabs)");
            unsigned char op = sz == 1 ? 0xc6 : 0xc7;
            if (emit_rm(x, p, w, &op, 1, 0, NULL, dst) < 0) return -1;
            imm_put(x, v, sz == 8 ? 4 : sz);
            return 0;
        }
        if (is_reg_sz(src, sz) && is_rm(dst, sz)) {
            unsigned char op = sz == 1 ? 0x88 : 0x89;
            return emit_rm(x, p, w, &op, 1, src->reg, src, dst);
        }
        if (src->k == K_MEM && is_reg_sz(dst, sz)) {
            unsigned char op = sz == 1 ? 0x8a : 0x8b;
            return emit_rm(x, p, w, &op, 1, dst->reg, dst, src);
        }
        return fail("'%s': these operands do not go together", mn);
    }
    if ((s = sized(mn, "movabs")) >= 0 && n == 2 && o[0].k == K_IMM && o[1].k == K_REG &&
        o[1].size == 8) {
        unsigned char op = 0xb8;
        if (emit_oreg(x, 0, 1, &op, 1, &o[1]) < 0) return -1;
        imm_put(x, o[0].imm, 8);
        return 0;
    }

    /* ---- movzx / movsx ---- */
    {
        static const struct { const char *n; int from, to, sx; } mx[] = {
            {"movzbw",1,2,0},{"movzbl",1,4,0},{"movzbq",1,8,0},{"movzwl",2,4,0},
            {"movzwq",2,8,0},{"movsbw",1,2,1},{"movsbl",1,4,1},{"movsbq",1,8,1},
            {"movswl",2,4,1},{"movswq",2,8,1},{"movslq",4,8,2},
            {"movzx",0,0,0},{"movsx",0,0,1},{"movsxd",4,8,2},
        };
        for (unsigned k = 0; k < sizeof mx / sizeof *mx; k++) {
            if (strcmp(mn, mx[k].n) || n != 2)
                continue;
            int from = mx[k].from, to = mx[k].to;
            if (!from) from = o[0].k == K_REG ? o[0].size : 0;
            if (!to) to = o[1].k == K_REG ? o[1].size : 0;
            if (!from || !to || o[1].k != K_REG || o[1].size != to ||
                (o[0].k == K_REG && o[0].size != from))
                return fail("'%s': the source and destination sizes", mn);
            if (mx[k].sx == 2) {
                unsigned char op = 0x63;
                return emit_rm(x, 0, 1, &op, 1, o[1].reg, &o[1], &o[0]);
            }
            unsigned char op[2] = { 0x0f, (unsigned char)((mx[k].sx ? 0xbe : 0xb6) + (from == 2)) };
            return emit_rm(x, pfx_of(to), to == 8, op, 2, o[1].reg, &o[1], &o[0]);
        }
    }

    /* ---- lea, xchg, test ---- */
    if ((s = sized(mn, "lea")) >= 0 && n == 2) {
        int sz = opsize(s, o, 2);
        if (o[0].k != K_MEM || o[1].k != K_REG || o[1].size != sz || sz < 2)
            return fail("lea takes an address and a 16-, 32- or 64-bit register");
        unsigned char op = 0x8d;
        return emit_rm(x, pfx_of(sz), sz == 8, &op, 1, o[1].reg, &o[1], &o[0]);
    }
    if ((s = sized(mn, "xchg")) >= 0 && n == 2) {
        int sz = opsize(s, o, 2);
        if (need_size(sz, mn) < 0) return -1;
        struct opnd *r = o[1].k == K_REG ? &o[1] : &o[0], *m = r == &o[1] ? &o[0] : &o[1];
        if (!is_reg_sz(r, sz) || !is_rm(m, sz)) return fail("xchg takes a register and a register or memory");
        if (sz > 1 && m->k == K_REG && (m->reg == 0 || r->reg == 0)) {
            unsigned char op = 0x90;
            return emit_oreg(x, pfx_of(sz), sz == 8, &op, 1, m->reg == 0 ? r : m);
        }
        unsigned char op = sz == 1 ? 0x86 : 0x87;
        return emit_rm(x, pfx_of(sz), sz == 8, &op, 1, r->reg, r, m);
    }
    if ((s = sized(mn, "test")) >= 0 && n == 2) {
        int sz = opsize(s, o, 2);
        if (need_size(sz, mn) < 0) return -1;
        int p = pfx_of(sz), w = sz == 8;
        if (o[0].k == K_IMM) {
            if (o[1].k == K_REG && o[1].reg == 0 && !o[1].hi8) {
                if (p) put(x, 0x66);
                if (w) put(x, 0x48);
                put(x, sz == 1 ? 0xa8 : 0xa9);
                imm_put(x, o[0].imm, sz == 1 ? 1 : sz == 2 ? 2 : 4);
                return 0;
            }
            unsigned char op = sz == 1 ? 0xf6 : 0xf7;
            if (emit_rm(x, p, w, &op, 1, 0, NULL, &o[1]) < 0) return -1;
            imm_put(x, o[0].imm, sz == 1 ? 1 : sz == 2 ? 2 : 4);
            return 0;
        }
        struct opnd *r = is_reg_sz(&o[0], sz) ? &o[0] : &o[1], *m = r == &o[0] ? &o[1] : &o[0];
        if (!is_reg_sz(r, sz) || !is_rm(m, sz)) return fail("test: these operands");
        unsigned char op = sz == 1 ? 0x84 : 0x85;
        return emit_rm(x, p, w, &op, 1, r->reg, r, m);
    }

    /* ---- the ALU group ---- */
    {
        static const char *const an[] = { "add","or","adc","sbb","and","sub","xor","cmp" };
        for (int e = 0; e < 8; e++)
            if ((s = sized(mn, an[e])) >= 0 && n == 2) {
                int sz = opsize(s, o, 2);
                if (need_size(sz, mn) < 0) return -1;
                return alu(x, e, sz, &o[0], &o[1]);
            }
    }
    /* ---- one operand: not neg mul imul div idiv inc dec ---- */
    {
        static const struct { const char *n; int ext; unsigned op8, op; } un[] = {
            {"not",2,0xf6,0xf7},{"neg",3,0xf6,0xf7},{"mul",4,0xf6,0xf7},
            {"imul",5,0xf6,0xf7},{"div",6,0xf6,0xf7},{"idiv",7,0xf6,0xf7},
            {"inc",0,0xfe,0xff},{"dec",1,0xfe,0xff},
        };
        for (unsigned k = 0; k < sizeof un / sizeof *un; k++)
            if ((s = sized(mn, un[k].n)) >= 0 && n == 1) {
                int sz = opsize(s, o, 1);
                if (need_size(sz, mn) < 0) return -1;
                if (!is_rm(&o[0], sz)) return fail("'%s' takes a register or memory", mn);
                unsigned char op = (unsigned char)(sz == 1 ? un[k].op8 : un[k].op);
                return emit_rm(x, pfx_of(sz), sz == 8, &op, 1, un[k].ext, NULL, &o[0]);
            }
    }
    if ((s = sized(mn, "imul")) >= 0 && (n == 2 || n == 3)) {
        int sz = opsize(s, o, n);
        struct opnd *dst = &o[n - 1];
        if (!is_reg_sz(dst, sz) || sz == 1) return fail("imul's destination is a 16-, 32- or 64-bit register");
        if (o[0].k == K_IMM) {
            struct opnd *src = n == 3 ? &o[1] : dst;
            long long v = o[0].imm;
            unsigned char op = fits8(v) ? 0x6b : 0x69;
            if (emit_rm(x, pfx_of(sz), sz == 8, &op, 1, dst->reg, dst, src) < 0) return -1;
            imm_put(x, v, fits8(v) ? 1 : sz == 2 ? 2 : 4);
            return 0;
        }
        if (n != 2 || !is_rm(&o[0], sz)) return fail("imul: these operands");
        unsigned char op[2] = { 0x0f, 0xaf };
        return emit_rm(x, pfx_of(sz), sz == 8, op, 2, dst->reg, dst, &o[0]);
    }

    /* ---- shifts and rotates ---- */
    {
        static const struct { const char *n; int ext; } sh[] = {
            {"rol",0},{"ror",1},{"rcl",2},{"rcr",3},{"shl",4},{"sal",4},{"shr",5},{"sar",7},
        };
        for (unsigned k = 0; k < sizeof sh / sizeof *sh; k++)
            if ((s = sized(mn, sh[k].n)) >= 0 && (n == 1 || n == 2)) {
                struct opnd *dst = &o[n - 1];
                int sz = opsize(s, dst, 1);
                if (need_size(sz, mn) < 0) return -1;
                if (!is_rm(dst, sz)) return fail("'%s' shifts a register or memory", mn);
                int p = pfx_of(sz), w = sz == 8;
                if (n == 1 || (o[0].k == K_IMM && o[0].imm == 1)) {
                    unsigned char op = sz == 1 ? 0xd0 : 0xd1;
                    return emit_rm(x, p, w, &op, 1, sh[k].ext, NULL, dst);
                }
                if (o[0].k == K_REG && o[0].reg == 1 && o[0].size == 1) {
                    unsigned char op = sz == 1 ? 0xd2 : 0xd3;
                    return emit_rm(x, p, w, &op, 1, sh[k].ext, NULL, dst);
                }
                if (o[0].k != K_IMM) return fail("a shift's count is $N or %%cl");
                unsigned char op = sz == 1 ? 0xc0 : 0xc1;
                if (emit_rm(x, p, w, &op, 1, sh[k].ext, NULL, dst) < 0) return -1;
                imm_put(x, o[0].imm, 1);
                return 0;
            }
    }
    {
        static const struct { const char *n; unsigned op; } dsh[] = { {"shld",0xa4}, {"shrd",0xac} };
        for (unsigned k = 0; k < 2; k++)
            if ((s = sized(mn, dsh[k].n)) >= 0 && n == 3) {
                int sz = opsize(s, o + 1, 2);
                if (!is_reg_sz(&o[1], sz) || !is_rm(&o[2], sz) || sz == 1)
                    return fail("'%s' takes a count, a register and a register or memory", mn);
                unsigned char op[2] = { 0x0f, (unsigned char)dsh[k].op };
                if (o[0].k == K_REG && o[0].reg == 1 && o[0].size == 1) {
                    op[1]++;
                    return emit_rm(x, pfx_of(sz), sz == 8, op, 2, o[1].reg, &o[1], &o[2]);
                }
                if (o[0].k != K_IMM) return fail("the count is $N or %%cl");
                if (emit_rm(x, pfx_of(sz), sz == 8, op, 2, o[1].reg, &o[1], &o[2]) < 0) return -1;
                imm_put(x, o[0].imm, 1);
                return 0;
            }
    }

    /* ---- push / pop ---- */
    if (((s = sized(mn, "push")) >= 0 || (s = sized(mn, "pop")) >= 0) && n == 1) {
        int push = mn[1] == 'u';
        if (s && s != 8 && s != 2) return fail("'%s': push and pop are 16 or 64 bits", mn);
        if (push && o[0].k == K_IMM) {
            if (fits8(o[0].imm)) { put(x, 0x6a); imm_put(x, o[0].imm, 1); }
            else { put(x, 0x68); imm_put(x, o[0].imm, 4); }
            return 0;
        }
        if (o[0].k == K_REG) {
            if (o[0].size != 8 && o[0].size != 2) return fail("'%s' takes a 64-bit register", mn);
            unsigned char op = push ? 0x50 : 0x58;
            return emit_oreg(x, o[0].size == 2 ? P66 : 0, 0, &op, 1, &o[0]);
        }
        if (o[0].k == K_SEG && o[0].reg >= 4) {         /* fs, gs */
            put(x, 0x0f);
            put(x, (o[0].reg == 4 ? 0xa0u : 0xa8u) + (push ? 0 : 1));
            return 0;
        }
        if (o[0].k != K_MEM) return fail("'%s' takes a register, memory or $imm", mn);
        unsigned char op = push ? 0xff : 0x8f;
        return emit_rm(x, s == 2 ? P66 : 0, 0, &op, 1, push ? 6 : 0, NULL, &o[0]);
    }

    /* ---- setcc, cmovcc ---- */
    if (!strncmp(mn, "set", 3) && n == 1) {
        char c[8];
        snprintf(c, sizeof c, "%s", mn + 3);
        size_t cl = strlen(c);
        if (cl > 1 && c[cl - 1] == 'b' && cond_of(c) < 0) c[cl - 1] = 0;
        int cc = cond_of(c);
        if (cc >= 0) {
            if (!is_rm(&o[0], 1)) return fail("'%s' writes a byte register or memory", mn);
            unsigned char op[2] = { 0x0f, (unsigned char)(0x90 + cc) };
            return emit_rm(x, 0, 0, op, 2, 0, NULL, &o[0]);
        }
    }
    if (!strncmp(mn, "cmov", 4) && n == 2) {
        char c[8];
        snprintf(c, sizeof c, "%s", mn + 4);
        size_t cl = strlen(c);
        int sfx = 0, cc = cond_of(c);
        if (cc < 0 && cl > 1) {
            char t = c[cl - 1];
            c[cl - 1] = 0;
            cc = cond_of(c);
            sfx = t == 'w' ? 2 : t == 'l' ? 4 : t == 'q' ? 8 : -1;
            if (sfx < 0) cc = -1;
        }
        if (cc >= 0) {
            int sz = opsize(sfx, o, 2);
            if (!is_reg_sz(&o[1], sz) || !is_rm(&o[0], sz) || sz == 1)
                return fail("'%s' takes a register or memory and a register", mn);
            unsigned char op[2] = { 0x0f, (unsigned char)(0x40 + cc) };
            return emit_rm(x, pfx_of(sz), sz == 8, op, 2, o[1].reg, &o[1], &o[0]);
        }
    }

    /* ---- bit operations ---- */
    {
        static const struct { const char *n; unsigned pfx, op; } bo[] = {
            {"bsf",0,0xbc},{"bsr",0,0xbd},{"popcnt",PF3,0xb8},{"lzcnt",PF3,0xbd},
            {"tzcnt",PF3,0xbc},
        };
        for (unsigned k = 0; k < sizeof bo / sizeof *bo; k++)
            if ((s = sized(mn, bo[k].n)) >= 0 && n == 2) {
                int sz = opsize(s, o, 2);
                if (!is_reg_sz(&o[1], sz) || !is_rm(&o[0], sz) || sz == 1)
                    return fail("'%s' takes a register or memory and a register", mn);
                unsigned char op[2] = { 0x0f, (unsigned char)bo[k].op };
                return emit_rm(x, (int)bo[k].pfx | pfx_of(sz), sz == 8, op, 2, o[1].reg, &o[1], &o[0]);
            }
    }
    {
        static const struct { const char *n; int ext; unsigned op; } bt[] = {
            {"bt",4,0xa3},{"bts",5,0xab},{"btr",6,0xb3},{"btc",7,0xbb},
        };
        for (unsigned k = 0; k < sizeof bt / sizeof *bt; k++)
            if ((s = sized(mn, bt[k].n)) >= 0 && n == 2) {
                int sz = opsize(s, o + 1, 1);
                if (o[0].k != K_IMM && sz == 0) sz = o[0].k == K_REG ? o[0].size : 0;
                if (need_size(sz, mn) < 0) return -1;
                if (!is_rm(&o[1], sz) || sz == 1) return fail("'%s': these operands", mn);
                if (o[0].k == K_IMM) {
                    unsigned char op[2] = { 0x0f, 0xba };
                    if (emit_rm(x, pfx_of(sz), sz == 8, op, 2, bt[k].ext, NULL, &o[1]) < 0) return -1;
                    imm_put(x, o[0].imm, 1);
                    return 0;
                }
                if (!is_reg_sz(&o[0], sz)) return fail("'%s': these operands", mn);
                unsigned char op[2] = { 0x0f, (unsigned char)bt[k].op };
                return emit_rm(x, pfx_of(sz), sz == 8, op, 2, o[0].reg, &o[0], &o[1]);
            }
    }
    if ((s = sized(mn, "bswap")) >= 0 && n == 1 && o[0].k == K_REG &&
        (o[0].size == 4 || o[0].size == 8)) {
        unsigned char op[2] = { 0x0f, 0xc8 };
        return emit_oreg(x, 0, o[0].size == 8, op, 2, &o[0]);
    }
    {
        static const struct { const char *n; unsigned op; } xa[] = { {"cmpxchg",0xb0}, {"xadd",0xc0} };
        for (unsigned k = 0; k < 2; k++)
            if ((s = sized(mn, xa[k].n)) >= 0 && n == 2) {
                int sz = opsize(s, o, 2);
                if (need_size(sz, mn) < 0) return -1;
                if (!is_reg_sz(&o[0], sz) || !is_rm(&o[1], sz)) return fail("'%s': these operands", mn);
                unsigned char op[2] = { 0x0f, (unsigned char)(xa[k].op + (sz != 1)) };
                return emit_rm(x, pfx_of(sz), sz == 8, op, 2, o[0].reg, &o[0], &o[1]);
            }
    }
    if ((!strcmp(mn, "cmpxchg8b") || !strcmp(mn, "cmpxchg16b")) && n == 1 && o[0].k == K_MEM) {
        unsigned char op[2] = { 0x0f, 0xc7 };
        return emit_rm(x, 0, mn[7] == '1', op, 2, 1, NULL, &o[0]);
    }

    /* ---- port I/O ---- */
    if ((s = sized(mn, "in")) >= 0 && n == 2 && o[1].k == K_REG && o[1].reg == 0) {
        int sz = o[1].size;
        if (sz == 8) return fail("in reads 8, 16 or 32 bits");
        if (sz == 2) put(x, 0x66);
        if (o[0].k == K_IMM) { put(x, sz == 1 ? 0xe4 : 0xe5); imm_put(x, o[0].imm, 1); return 0; }
        if (o[0].k == K_REG && o[0].reg == 2 && o[0].size == 2) { put(x, sz == 1 ? 0xec : 0xed); return 0; }
        return fail("in's port is $N or %%dx");
    }
    if ((s = sized(mn, "out")) >= 0 && n == 2 && o[0].k == K_REG && o[0].reg == 0) {
        int sz = o[0].size;
        if (sz == 8) return fail("out writes 8, 16 or 32 bits");
        if (sz == 2) put(x, 0x66);
        if (o[1].k == K_IMM) { put(x, sz == 1 ? 0xe6 : 0xe7); imm_put(x, o[1].imm, 1); return 0; }
        if (o[1].k == K_REG && o[1].reg == 2 && o[1].size == 2) { put(x, sz == 1 ? 0xee : 0xef); return 0; }
        return fail("out's port is $N or %%dx");
    }

    /* ---- system instructions with an operand ---- */
    {
        static const struct { const char *n; unsigned char op[3]; int nop, ext, mem, w; } sy[] = {
            {"lgdt",{0x0f,0x01},2,2,1,0}, {"lidt",{0x0f,0x01},2,3,1,0},
            {"sgdt",{0x0f,0x01},2,0,1,0}, {"sidt",{0x0f,0x01},2,1,1,0},
            {"lgdtq",{0x0f,0x01},2,2,1,0}, {"lidtq",{0x0f,0x01},2,3,1,0},
            {"invlpg",{0x0f,0x01},2,7,1,0}, {"lldt",{0x0f,0x00},2,2,0,0},
            {"ltr",{0x0f,0x00},2,3,0,0}, {"str",{0x0f,0x00},2,1,0,0},
            {"sldt",{0x0f,0x00},2,0,0,0}, {"clflush",{0x0f,0xae},2,7,1,0},
            {"fxsave",{0x0f,0xae},2,0,1,0}, {"fxrstor",{0x0f,0xae},2,1,1,0},
            {"fxsave64",{0x0f,0xae},2,0,1,1}, {"fxrstor64",{0x0f,0xae},2,1,1,1},
            {"ldmxcsr",{0x0f,0xae},2,2,1,0}, {"stmxcsr",{0x0f,0xae},2,3,1,0},
            {"xsave",{0x0f,0xae},2,4,1,0}, {"xrstor",{0x0f,0xae},2,5,1,0},
            {"xsave64",{0x0f,0xae},2,4,1,1}, {"xrstor64",{0x0f,0xae},2,5,1,1},
            {"prefetchnta",{0x0f,0x18},2,0,1,0}, {"prefetcht0",{0x0f,0x18},2,1,1,0},
            {"prefetcht1",{0x0f,0x18},2,2,1,0}, {"prefetcht2",{0x0f,0x18},2,3,1,0},
            {"prefetchw",{0x0f,0x0d},2,1,1,0},
        };
        for (unsigned k = 0; k < sizeof sy / sizeof *sy; k++)
            if (!strcmp(mn, sy[k].n) && n == 1) {
                if (sy[k].mem && o[0].k != K_MEM) return fail("'%s' takes memory", mn);
                if (!sy[k].mem && o[0].k == K_REG && o[0].size == 1)
                    return fail("'%s' takes a 16-bit operand", mn);
                /* str and sldt into a register write its width (66 for
                 * %ax, REX.W for %rax); the loads read 16 bits whatever */
                int st = !sy[k].mem && o[0].k == K_REG &&
                         (!strcmp(mn, "str") || !strcmp(mn, "sldt"));
                return emit_rm(x, st && o[0].size == 2 ? P66 : 0,
                               sy[k].w || (st && o[0].size == 8), sy[k].op,
                               sy[k].nop, sy[k].ext, NULL, &o[0]);
            }
        static const struct { const char *n; int ext; int pfx; } rr[] = {
            {"rdrand",6,0},{"rdseed",7,0},{"rdpid",7,PF3},
        };
        for (unsigned k = 0; k < sizeof rr / sizeof *rr; k++)
            if ((s = sized(mn, rr[k].n)) >= 0 && n == 1 && o[0].k == K_REG) {
                unsigned char op[2] = { 0x0f, rr[k].pfx ? 0xc7 : 0xc7 };
                int sz = o[0].size;
                return emit_rm(x, rr[k].pfx | pfx_of(sz), rr[k].pfx ? 0 : sz == 8, op, 2,
                               rr[k].ext, NULL, &o[0]);
            }
        static const struct { const char *n; int ext; } fsb[] = {
            {"rdfsbase",0},{"rdgsbase",1},{"wrfsbase",2},{"wrgsbase",3},
        };
        for (unsigned k = 0; k < 4; k++)
            if ((s = sized(mn, fsb[k].n)) >= 0 && n == 1 && o[0].k == K_REG) {
                unsigned char op[2] = { 0x0f, 0xae };
                return emit_rm(x, PF3, o[0].size == 8, op, 2, fsb[k].ext, NULL, &o[0]);
            }
    }

    /* ---- SSE ---- */
    {
        /* load direction: op src(xmm/mem), dst(xmm) */
        static const struct { const char *n; int pfx; unsigned op; } sl[] = {
            {"addss",PF3,0x58},{"addsd",PF2,0x58},{"addps",0,0x58},{"addpd",P66,0x58},
            {"mulss",PF3,0x59},{"mulsd",PF2,0x59},{"mulps",0,0x59},{"mulpd",P66,0x59},
            {"subss",PF3,0x5c},{"subsd",PF2,0x5c},{"subps",0,0x5c},{"subpd",P66,0x5c},
            {"minss",PF3,0x5d},{"minsd",PF2,0x5d},{"minps",0,0x5d},{"minpd",P66,0x5d},
            {"divss",PF3,0x5e},{"divsd",PF2,0x5e},{"divps",0,0x5e},{"divpd",P66,0x5e},
            {"maxss",PF3,0x5f},{"maxsd",PF2,0x5f},{"maxps",0,0x5f},{"maxpd",P66,0x5f},
            {"sqrtss",PF3,0x51},{"sqrtsd",PF2,0x51},{"sqrtps",0,0x51},{"sqrtpd",P66,0x51},
            {"andps",0,0x54},{"andpd",P66,0x54},{"andnps",0,0x55},{"andnpd",P66,0x55},
            {"orps",0,0x56},{"orpd",P66,0x56},{"xorps",0,0x57},{"xorpd",P66,0x57},
            {"ucomiss",0,0x2e},{"ucomisd",P66,0x2e},{"comiss",0,0x2f},{"comisd",P66,0x2f},
            {"cvtss2sd",PF3,0x5a},{"cvtsd2ss",PF2,0x5a},{"cvtps2pd",0,0x5a},
            {"cvtpd2ps",P66,0x5a},{"cvtdq2ps",0,0x5b},{"cvtps2dq",P66,0x5b},
            {"cvttps2dq",PF3,0x5b},{"unpcklps",0,0x14},{"unpckhps",0,0x15},
            {"unpcklpd",P66,0x14},{"unpckhpd",P66,0x15},
            {"pxor",P66,0xef},{"por",P66,0xeb},{"pand",P66,0xdb},{"pandn",P66,0xdf},
            {"paddb",P66,0xfc},{"paddw",P66,0xfd},{"paddd",P66,0xfe},{"paddq",P66,0xd4},
            {"psubb",P66,0xf8},{"psubw",P66,0xf9},{"psubd",P66,0xfa},{"psubq",P66,0xfb},
            {"pcmpeqb",P66,0x74},{"pcmpeqw",P66,0x75},{"pcmpeqd",P66,0x76},
            {"pcmpgtb",P66,0x64},{"pcmpgtw",P66,0x65},{"pcmpgtd",P66,0x66},
            {"punpcklbw",P66,0x60},{"punpcklwd",P66,0x61},{"punpckldq",P66,0x62},
            {"punpcklqdq",P66,0x6c},{"punpckhbw",P66,0x68},{"punpckhwd",P66,0x69},
            {"punpckhdq",P66,0x6a},{"punpckhqdq",P66,0x6d},{"pmullw",P66,0xd5},
            {"pmuludq",P66,0xf4},{"pminub",P66,0xda},{"pmaxub",P66,0xde},
            {"pavgb",P66,0xe0},{"psadbw",P66,0xf6},
            {"movaps",0,0x28},{"movapd",P66,0x28},{"movups",0,0x10},{"movupd",P66,0x10},
            {"movdqa",P66,0x6f},{"movdqu",PF3,0x6f},{"movss",PF3,0x10},{"movsd",PF2,0x10},
            {"movhlps",0,0x12},{"movlhps",0,0x16},
        };
        /* store direction for the moves */
        static const struct { const char *n; int pfx; unsigned op; } ss[] = {
            {"movaps",0,0x29},{"movapd",P66,0x29},{"movups",0,0x11},{"movupd",P66,0x11},
            {"movdqa",P66,0x7f},{"movdqu",PF3,0x7f},{"movss",PF3,0x11},{"movsd",PF2,0x11},
            {"movntps",0,0x2b},{"movntpd",P66,0x2b},{"movntdq",P66,0xe7},
        };
        for (unsigned k = 0; k < sizeof ss / sizeof *ss; k++)
            if (!strcmp(mn, ss[k].n) && n == 2 && o[0].k == K_XMM && o[1].k == K_MEM) {
                unsigned char op[2] = { 0x0f, (unsigned char)ss[k].op };
                return emit_rm(x, ss[k].pfx, 0, op, 2, o[0].reg, NULL, &o[1]);
            }
        for (unsigned k = 0; k < sizeof sl / sizeof *sl; k++)
            if (!strcmp(mn, sl[k].n) && n == 2 && o[1].k == K_XMM &&
                (o[0].k == K_XMM || o[0].k == K_MEM)) {
                unsigned char op[2] = { 0x0f, (unsigned char)sl[k].op };
                return emit_rm(x, sl[k].pfx, 0, op, 2, o[1].reg, NULL, &o[0]);
            }
        /* with an immediate: pshufd, shufps, shufpd, pslldq/psrldq... */
        static const struct { const char *n; int pfx; unsigned op; } si[] = {
            {"pshufd",P66,0x70},{"pshuflw",PF2,0x70},{"pshufhw",PF3,0x70},
            {"shufps",0,0xc6},{"shufpd",P66,0xc6},
            {"cmpss",PF3,0xc2},{"cmpsd",PF2,0xc2},{"cmpps",0,0xc2},{"cmppd",P66,0xc2},
        };
        for (unsigned k = 0; k < sizeof si / sizeof *si; k++)
            if (!strcmp(mn, si[k].n) && n == 3 && o[0].k == K_IMM && o[2].k == K_XMM &&
                (o[1].k == K_XMM || o[1].k == K_MEM)) {
                unsigned char op[2] = { 0x0f, (unsigned char)si[k].op };
                if (emit_rm(x, si[k].pfx, 0, op, 2, o[2].reg, NULL, &o[1]) < 0) return -1;
                imm_put(x, o[0].imm, 1);
                return 0;
            }
        static const struct { const char *n; unsigned op; int ext; } shi[] = {
            {"psrlw",0x71,2},{"psraw",0x71,4},{"psllw",0x71,6},
            {"psrld",0x72,2},{"psrad",0x72,4},{"pslld",0x72,6},
            {"psrlq",0x73,2},{"psrldq",0x73,3},{"psllq",0x73,6},{"pslldq",0x73,7},
        };
        for (unsigned k = 0; k < sizeof shi / sizeof *shi; k++)
            if (!strcmp(mn, shi[k].n) && n == 2 && o[0].k == K_IMM && o[1].k == K_XMM) {
                unsigned char op[2] = { 0x0f, (unsigned char)shi[k].op };
                if (emit_rm(x, P66, 0, op, 2, shi[k].ext, NULL, &o[1]) < 0) return -1;
                imm_put(x, o[0].imm, 1);
                return 0;
            }
        if ((!strcmp(mn, "pmovmskb") || !strcmp(mn, "movmskps") || !strcmp(mn, "movmskpd")) &&
            n == 2 && o[0].k == K_XMM && o[1].k == K_REG) {
            unsigned char op[2] = { 0x0f, (unsigned char)(mn[1] == 'm' ? 0xd7 : 0x50) };
            return emit_rm(x, mn[1] == 'm' || mn[7] == 'd' ? P66 : 0, o[1].size == 8, op, 2,
                           o[1].reg, NULL, &o[0]);
        }
        /* movd / movq between general registers (or memory) and xmm */
        if ((!strcmp(mn, "movd") || !strcmp(mn, "movq")) && n == 2) {
            int q = mn[3] == 'q';
            if (o[1].k == K_XMM && (o[0].k == K_REG || (o[0].k == K_MEM && !q))) {
                unsigned char op[2] = { 0x0f, 0x6e };
                return emit_rm(x, P66, o[0].k == K_REG ? o[0].size == 8 : 0, op, 2,
                               o[1].reg, NULL, &o[0]);
            }
            if (o[0].k == K_XMM && (o[1].k == K_REG || (o[1].k == K_MEM && !q))) {
                unsigned char op[2] = { 0x0f, 0x7e };
                return emit_rm(x, P66, o[1].k == K_REG ? o[1].size == 8 : 0, op, 2,
                               o[0].reg, NULL, &o[1]);
            }
            if (q && o[1].k == K_XMM && (o[0].k == K_XMM || o[0].k == K_MEM)) {
                unsigned char op[2] = { 0x0f, 0x7e };
                return emit_rm(x, PF3, 0, op, 2, o[1].reg, NULL, &o[0]);
            }
            if (q && o[0].k == K_XMM && o[1].k == K_MEM) {
                unsigned char op[2] = { 0x0f, 0xd6 };
                return emit_rm(x, P66, 0, op, 2, o[0].reg, NULL, &o[1]);
            }
        }
        /* conversions with a general register */
        static const struct { const char *n; int pfx, tox; unsigned op; } cv[] = {
            {"cvtsi2ss",PF3,1,0x2a},{"cvtsi2sd",PF2,1,0x2a},
            {"cvtsi2ssl",PF3,1,0x2a},{"cvtsi2sdl",PF2,1,0x2a},
            {"cvtsi2ssq",PF3,1,0x2a},{"cvtsi2sdq",PF2,1,0x2a},
            {"cvttss2si",PF3,0,0x2c},{"cvttsd2si",PF2,0,0x2c},
            {"cvtss2si",PF3,0,0x2d},{"cvtsd2si",PF2,0,0x2d},
        };
        for (unsigned k = 0; k < sizeof cv / sizeof *cv; k++)
            if (!strcmp(mn, cv[k].n) && n == 2) {
                unsigned char op[2] = { 0x0f, (unsigned char)cv[k].op };
                if (cv[k].tox) {
                    char last = mn[strlen(mn) - 1];
                    int w = o[0].k == K_REG ? o[0].size == 8 : last == 'q';
                    if (o[1].k != K_XMM) return fail("'%s' converts into an xmm register", mn);
                    return emit_rm(x, cv[k].pfx, w, op, 2, o[1].reg, NULL, &o[0]);
                }
                if (o[1].k != K_REG || (o[1].size != 4 && o[1].size != 8))
                    return fail("'%s' converts into a 32- or 64-bit register", mn);
                return emit_rm(x, cv[k].pfx, o[1].size == 8, op, 2, o[1].reg, &o[1], &o[0]);
            }
    }

    return fail("'%s' with %d operand%s is not an instruction this x86-64 "
                "assembler knows", mn, n, n == 1 ? "" : "s");
}

/* ---- statements --------------------------------------------------------- */

/* One statement: prefixes, the mnemonic, the operands split at top-level
 * commas. Appends its bytes. */
static int one(const char *s, int len, struct code *out)
{
    char mn[32];
    int i = 0;
    struct ib x;
    memset(&x, 0, sizeof x);
    x.disp_at = x.imm_at = -1;
    int force_near = 0;
    for (;;) {
        while (i < len && isspace((unsigned char)s[i])) i++;
        int st = i;
        while (i < len && !isspace((unsigned char)s[i]) && s[i] != ';')
            i++;
        int ml = i - st;
        if (ml == 0)
            return 0;
        if (ml >= (int)sizeof mn) return fail("'%.*s' is too long for a mnemonic", ml, s + st);
        for (int k = 0; k < ml; k++)
            mn[k] = (char)tolower((unsigned char)s[st + k]);
        mn[ml] = 0;
        /* prefixes, on the same line or as statements of their own */
        if (!strcmp(mn, "lock")) { put(&x, 0xf0); continue; }
        if (!strcmp(mn, "rep") || !strcmp(mn, "repe") || !strcmp(mn, "repz")) { put(&x, 0xf3); continue; }
        if (!strcmp(mn, "repne") || !strcmp(mn, "repnz")) { put(&x, 0xf2); continue; }
        if (!strcmp(mn, "rex64")) { put(&x, 0x48); continue; }
        if (!strcmp(mn, "near")) { force_near = 1; continue; }   /* (symform's) */
        break;
    }
    struct opnd o[4];
    int n = 0, depth = 0, st = i;
    while (i < len && isspace((unsigned char)s[i])) i++;
    st = i;
    if (i < len) {
        for (int k = i; k <= len; k++) {
            if (k < len && s[k] == '(') depth++;
            if (k < len && s[k] == ')') depth--;
            if (k == len || (s[k] == ',' && depth == 0)) {
                if (n == 4) return fail("too many operands");
                if (parse_opnd(s + st, k - st, &o[n]) < 0) return -1;
                n++;
                st = k + 1;
            }
        }
    }
    int save = g_level;
    if (force_near) g_level = 1;
    int r = insn(&x, mn, o, n);
    g_level = save;
    if (r < 0)
        return -1;
    if (x.dotrip) {                     /* rip-relative: from the end */
        long long d = x.dotn - x.n;
        for (int k = 0; k < 4; k++)
            x.b[x.disp_at + k] = (unsigned char)((unsigned long long)d >> (8 * k));
    }
    g_last_disp_at = x.disp_at;
    g_last_imm_at = x.imm_at;
    g_last_len = x.n;
    for (int k = 0; k < x.n; k++)
        code_byte(out, x.b[k]);
    return 0;
}

int x86att_assemble(const char *text, struct code *out, char *err, int errlen)
{
    g_err = err;
    g_errlen = errlen;
    err[0] = 0;
    const char *p = text;
    while (*p) {
        const char *st = p;
        while (*p && *p != '\n' && *p != ';')
            p++;
        int len = (int)(p - st);
        /* a `#` comment runs to the end of the line */
        const char *h = memchr(st, '#', (size_t)len);
        if (h) len = (int)(h - st);
        if (one(st, len, out) < 0) {
            char tmp[256];
            snprintf(tmp, sizeof tmp, "%s (assembling \"%.*s\")", err,
                     len > 80 ? 80 : len, st);
            snprintf(err, (size_t)errlen, "%s", tmp);
            return -1;
        }
        if (*p)
            p++;
    }
    return 0;
}

/* ---- symbols, for the file assembler ------------------------------------ */

int x86att_is_transfer(const char *stmt)
{
    stmt = after_prefixes(stmt);
    char mn[16];
    int k = 0;
    while (stmt[k] && !isspace((unsigned char)stmt[k]) && k < 15) {
        mn[k] = stmt[k];
        k++;
    }
    mn[k] = 0;
    if (!strcmp(mn, "jmp") || !strcmp(mn, "call") || !strcmp(mn, "jmpq") ||
        !strcmp(mn, "callq") || !strcmp(mn, "jrcxz"))
        return stmt[k] && !strchr(stmt + k, '*');
    return mn[0] == 'j' && cond_of(mn + 1) >= 0;
}

/* `call sym`, `jmp sym`, `jcc sym`, `sym(%rip)`, `$sym`, an absolute
 * `sym`: the statement with a zero there, and where its relocation goes */
int x86att_symform(const char *stmt, struct asm_symform *f)
{
    memset(f, 0, sizeof *f);
    const char *p = after_prefixes(stmt);
    const char *m = p;
    while (*p && !isspace((unsigned char)*p)) p++;
    int ml = (int)(p - m);
    /* the first symbol in the operands */
    const char *q = p;
    const char *sym = NULL;
    int sl = 0, dollar = 0;
    while (*q) {
        if (*q == '%') {                 /* a register: skip it */
            q++;
            while (asm_symc((unsigned char)*q)) q++;
            continue;
        }
        if (asm_sym0((unsigned char)*q) && !(*q == '.' && (q[1] == '+' || q[1] == '-' || !asm_symc((unsigned char)q[1])))) {
            const char *b = q;
            while (asm_symc((unsigned char)*q)) q++;
            sym = b;
            sl = (int)(q - b);
            dollar = b > stmt && b[-1] == '$';
            break;
        }
        if (isdigit((unsigned char)*q)) {
            while (asm_symc((unsigned char)*q)) q++;
            continue;
        }
        q++;
    }
    if (!sym)
        return 0;
    /* sym@PLT / @GOTPCREL, then +N */
    const char *a = sym + sl;
    int reloc_kind = 0;                  /* 1 PLT, 2 GOTPCREL */
    if (*a == '@') {
        if (!strncmp(a + 1, "PLT", 3)) { reloc_kind = 1; a += 4; }
        else if (!strncmp(a + 1, "GOTPCREL", 8)) { reloc_kind = 2; a += 9; }
        else return 0;
    }
    long long addend = 0;
    const char *ae = a;
    if (*a == '+' || *a == '-') {
        ae = a + 1;
        while (*ae && (isalnum((unsigned char)*ae) || *ae == 'x')) ae++;
        if (!asm_const_expr(a, (int)(ae - a), &addend))
            return 0;
    }
    f->sym_at = (int)(sym - stmt);
    f->sym_len = sl;
    /* the statement with 0 in the symbol's place */
    char mn[16];
    snprintf(mn, sizeof mn, "%.*s", ml < 15 ? ml : 15, m);
    int transfer = !strcmp(mn, "call") || !strcmp(mn, "callq") ||
                   !strcmp(mn, "jmp") || !strcmp(mn, "jmpq") ||
                   (mn[0] == 'j' && cond_of(mn + 1) >= 0);
    int rip = !strncmp(ae, "(%rip)", 6);
    /* (a transfer's field holds 0: the displacement from the end, the
     * near form's length -- the addend is the relocation's) */
    int jcc = transfer && mn[0] == 'j' && strncmp(mn, "jmp", 3);
    snprintf(f->encode, sizeof f->encode, "%s%.*s%s%s", transfer ? "near " : "",
             (int)(sym - stmt), stmt, transfer ? (jcc ? ".+6" : ".+5") : "0", ae);
    /* where the field lands: encode it */
    char err[256];
    struct code c = { 0 };
    g_err = err; g_errlen = (int)sizeof err;
    if (x86att_assemble(f->encode, &c, err, (int)sizeof err) != 0) {
        free(c.p);
        snprintf(f->encode, sizeof f->encode, "\001%s", err);
        return 1;
    }
    unsigned char c_first[16] = { 0 };
    for (int k = 0; k < c.len && k < 16; k++)
        c_first[k] = c.p[k];
    free(c.p);
    f->nsites = 1;
    if (transfer) {
        f->site[0].off = g_last_len - 4;
        /* a call or a jump to a function: PLT32, as GNU as writes it; a
         * conditional jump: PC32 */
        f->site[0].reloc = mn[0] == 'c' || !strncmp(mn, "jmp", 3)
                               ? R_X86_64_PLT32 : R_X86_64_PC32;
        f->addend = addend - 4;
        return 1;
    }
    if (rip) {
        f->site[0].off = g_last_disp_at;
        /* GOTPCREL as the relaxable GOTPCRELX llvm-mc and GNU as write
         * (REX_ when the instruction has a REX prefix) */
        int has_rex = 0;
        for (int k = 0; k < g_last_disp_at - 1 && k < g_last_len; k++)
            if ((c_first[k] & 0xf0) == 0x40)
                has_rex = 1;
        f->site[0].reloc = reloc_kind == 2 ? (has_rex ? R_X86_64_REX_GOTPCRELX : R_X86_64_GOTPCRELX)
                                         : R_X86_64_PC32;
        f->addend = addend - (g_last_len - g_last_disp_at);
        return 1;
    }
    if (dollar) {
        if (g_last_imm_at < 0) return 0;
        int ilen = g_last_len - g_last_imm_at;
        f->site[0].off = g_last_imm_at;
        f->site[0].reloc = ilen == 8 ? R_X86_64_64
                         : (!strncmp(mn, "movq", 4) || mn[strlen(mn) - 1] == 'q') ? R_X86_64_32S
                         : R_X86_64_32;
        f->addend = addend;
        return 1;
    }
    if (g_last_disp_at >= 0) {           /* an absolute address */
        f->site[0].off = g_last_disp_at;
        f->site[0].reloc = R_X86_64_32S;
        f->addend = addend;
        return 1;
    }
    return 0;
}
