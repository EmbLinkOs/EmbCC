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

enum { K_NONE, K_REG, K_XMM, K_MEM, K_IMM, K_CR, K_DR, K_SEG, K_ST,
       K_KREG, K_RC, K_SAE };   /* (AVX-512's: %k0-%k7, {rn-sae}, {sae}) */

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
    /* (xmm16-31, ymm16-31 and the zmm are AVX-512's, EVEX alone: insn()
     * sends them nowhere else) */
    const char *pfx[] = { "xmm", "cr", "dr", "ymm", "zmm" };
    int kinds[] = { K_XMM, K_CR, K_DR, K_XMM, K_XMM };
    for (int p = 0; p < 5; p++) {
        int pl = (int)strlen(pfx[p]);
        if (n > pl && !strncmp(s, pfx[p], (size_t)pl)) {
            int v = 0;
            for (int i = pl; i < n; i++) {
                if (!isdigit((unsigned char)s[i])) return 0;
                v = v * 10 + (s[i] - '0');
            }
            if (v > (kinds[p] == K_XMM ? 31 : 15)) return 0;
            out->n = pfx[p]; out->num = v;
            out->size = p == 0 ? 16 : p == 3 ? 32 : p == 4 ? 64 : 8;
            out->kind = kinds[p];
            return 1;
        }
    }
    if (n == 2 && s[0] == 'k' && s[1] >= '0' && s[1] <= '7') {
        out->n = "k"; out->num = s[1] - '0'; out->size = 8; out->kind = K_KREG;
        return 1;
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
                                      "repnz", "rex64", "near", "{disp32}",
                                      "{disp8}", "{imm32}" };
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
    /* inside braces: AVX-512's decorations, {z}, {1to16}, {rn-sae} */
    for (const char *q = w - 1; q >= stmt; q--) {
        if (*q == '{')
            return 1;
        if (*q == '}' || *q == ',' || *q == '(' || *q == ' ')
            break;
    }
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
    long long imm;                     /* K_IMM; K_RC: the rounding mode */
    int star;                          /* `*target` */
    /* AVX-512's decorations: {%kN} (mask, 1 + N), {z}, {1toN}; a vector
     * index (VSIB): its size, 16, 32 or 64 */
    int mask, zero, bcast, vindex;
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
/* GNU as's {disp32} / {disp8}: the displacement's width, whatever its
 * value -- {disp32} is also how a relocated one keeps its field */
static int g_disp_force;
/* {imm32}, EmbCC's own: an immediate keeps its 32-bit field -- a symbol's
 * value is the linker's, and the 0 written for it fits a byte */
static int g_imm_wide;

/* the relaxation level: 0 the short jump when it reaches, 1 the near one;
 * -2, pass one's, the short one always -- a layout that starts as small as
 * it can be and grows only where it must reaches the smallest settled
 * one, as llvm-mc's does (one that starts too big can settle too big: two
 * jumps each long only because the other is) */
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
static int imm8_ok(long long v) { return !g_imm_wide && fits8(v); }
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
    /* AVX-512: {rn-sae} and {sae} are operands of their own; {%k1}, {z}
     * and {1to16} are decorations after one */
    if (s[0] == '{' && s[n - 1] == '}') {
        static const char *const rc[] = { "rn-sae", "rd-sae", "ru-sae", "rz-sae" };
        for (int k = 0; k < 4; k++)
            if (n - 2 == 6 && !strncmp(s + 1, rc[k], 6)) {
                o->k = K_RC;
                o->imm = k;
                return 0;
            }
        if (n == 5 && !strncmp(s + 1, "sae", 3)) {
            o->k = K_SAE;
            return 0;
        }
        return fail("'%.*s' is not a rounding control ({rn-sae}, {rd-sae}, "
                    "{ru-sae}, {rz-sae}, {sae})", n, s);
    }
    {
        int mask = 0, zero = 0, bcast = 0;
        while (n > 0 && s[n - 1] == '}') {
            int b = n - 1;
            while (b > 0 && s[b] != '{') b--;
            if (s[b] != '{')
                return fail("'%.*s': a '}' with no '{'", n, s);
            const char *d = s + b + 1;
            int dl = n - 1 - b - 1;
            if (dl == 3 && d[0] == '%' && d[1] == 'k' && d[2] >= '0' && d[2] <= '7')
                mask = 1 + (d[2] - '0');
            else if (dl == 1 && d[0] == 'z')
                zero = 1;
            else if (dl > 3 && !strncmp(d, "1to", 3)) {
                bcast = atoi(d + 3);
                if (bcast != 2 && bcast != 4 && bcast != 8 && bcast != 16 && bcast != 32)
                    return fail("'{%.*s}' is not a broadcast (1to2 .. 1to32)", dl, d);
            } else
                return fail("'{%.*s}' is not a decoration AVX-512 has ({%%k1}, "
                            "{z}, {1to16})", dl, d);
            n = b;
            while (n > 0 && isspace((unsigned char)s[n - 1])) n--;
        }
        if (mask || zero || bcast) {
            int r = parse_opnd(s, n, o);
            o->mask = mask;
            o->zero = zero;
            o->bcast = bcast;
            return r;
        }
    }
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
    /* the x87 stack: %st is %st(0) */
    if (n >= 3 && !strncmp(s, "%st", 3) && (n == 3 || s[3] == '(')) {
        o->k = K_ST;
        if (n == 3) return 0;
        if (n == 6 && s[3] == '(' && s[4] >= '0' && s[4] <= '7' && s[5] == ')') {
            o->reg = s[4] - '0';
            return 0;
        }
        return fail("'%.*s' is not an x87 register: %%st(0)..%%st(7)", n, s);
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
        /* (a vector register: a gather's or a scatter's index, VSIB) */
        if (parse_reg(parts[1], plen[1], &r) && r.k == K_XMM) {
            o->index = r.reg;
            o->vindex = r.size;
        } else {
            if (!parse_reg(parts[1], plen[1], &r) || r.k != K_REG || r.size != 8 ||
                r.reg == 16)
                return fail("'%.*s' is not a 64-bit index register", plen[1], parts[1]);
            if (r.reg == 4)
                return fail("%%rsp cannot be an index");
            o->index = r.reg;
        }
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
static int modrm(struct ib *x, int regf, const struct opnd *rm);

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
    return modrm(x, regf, rm);
}

/* ModRM, SIB and displacement: what follows the opcode, legacy or VEX */
static int modrm(struct ib *x, int regf, const struct opnd *rm)
{
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
    if (g_disp_force == 32) mod = 2;
    else if (g_disp_force == 8 && mod == 0) mod = 1;
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
        if (imm8_ok(sv)) {
            unsigned char o = 0x83;
            if (emit_rm(x, p, w, &o, 1, ext, NULL, dst) < 0) return -1;
            imm_put(x, sv, 1);
            return 0;
        }
        /* (the accumulator's short form, but not for a symbol's value:
         * llvm-mc and GNU as write that as 81 /n) */
        if (dst->k == K_REG && dst->reg == 0 && !g_imm_wide) {
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
/* ---- x87 -----------------------------------------------------------------
 * Memory forms by their size suffix (`flds`, `fldl`, `fldt`, `fildll`) and
 * the stack registers %st and %st(i), encoded as GNU as and llvm-mc
 * encode them (tests/golden/x86-x87.sh). */
static int x87(struct ib *x, const char *mn, const struct opnd *o, int n)
{
    static const struct { const char *n; unsigned char op; int ext, wait; } mem[] = {
        {"flds",0xd9,0,0},{"fldl",0xdd,0,0},{"fldt",0xdb,5,0},
        {"filds",0xdf,0,0},{"fildl",0xdb,0,0},{"fildll",0xdf,5,0},{"fildq",0xdf,5,0},
        {"fsts",0xd9,2,0},{"fstl",0xdd,2,0},{"fstps",0xd9,3,0},{"fstpl",0xdd,3,0},
        {"fstpt",0xdb,7,0},
        {"fists",0xdf,2,0},{"fistl",0xdb,2,0},{"fistps",0xdf,3,0},{"fistpl",0xdb,3,0},
        {"fistpll",0xdf,7,0},{"fistpq",0xdf,7,0},
        {"fisttps",0xdf,1,0},{"fisttpl",0xdb,1,0},{"fisttpll",0xdd,1,0},{"fisttpq",0xdd,1,0},
        {"fbld",0xdf,4,0},{"fbstp",0xdf,6,0},
        {"fadds",0xd8,0,0},{"faddl",0xdc,0,0},{"fmuls",0xd8,1,0},{"fmull",0xdc,1,0},
        {"fcoms",0xd8,2,0},{"fcoml",0xdc,2,0},{"fcomps",0xd8,3,0},{"fcompl",0xdc,3,0},
        {"fsubs",0xd8,4,0},{"fsubl",0xdc,4,0},{"fsubrs",0xd8,5,0},{"fsubrl",0xdc,5,0},
        {"fdivs",0xd8,6,0},{"fdivl",0xdc,6,0},{"fdivrs",0xd8,7,0},{"fdivrl",0xdc,7,0},
        {"fiadds",0xde,0,0},{"fiaddl",0xda,0,0},{"fimuls",0xde,1,0},{"fimull",0xda,1,0},
        {"ficoms",0xde,2,0},{"ficoml",0xda,2,0},{"ficomps",0xde,3,0},{"ficompl",0xda,3,0},
        {"fisubs",0xde,4,0},{"fisubl",0xda,4,0},{"fisubrs",0xde,5,0},{"fisubrl",0xda,5,0},
        {"fidivs",0xde,6,0},{"fidivl",0xda,6,0},{"fidivrs",0xde,7,0},{"fidivrl",0xda,7,0},
        {"fldcw",0xd9,5,0},{"fnstcw",0xd9,7,0},{"fstcw",0xd9,7,1},
        {"fldenv",0xd9,4,0},{"fnstenv",0xd9,6,0},{"fstenv",0xd9,6,1},
        {"frstor",0xdd,4,0},{"fnsave",0xdd,6,0},{"fsave",0xdd,6,1},
        {"fnstsw",0xdd,7,0},{"fstsw",0xdd,7,1},
    };
    static const struct { const char *n; unsigned char b0, b1; } none[] = {
        {"fchs",0xd9,0xe0},{"fabs",0xd9,0xe1},{"ftst",0xd9,0xe4},{"fxam",0xd9,0xe5},
        {"fld1",0xd9,0xe8},{"fldl2t",0xd9,0xe9},{"fldl2e",0xd9,0xea},{"fldpi",0xd9,0xeb},
        {"fldlg2",0xd9,0xec},{"fldln2",0xd9,0xed},{"fldz",0xd9,0xee},
        {"f2xm1",0xd9,0xf0},{"fyl2x",0xd9,0xf1},{"fptan",0xd9,0xf2},{"fpatan",0xd9,0xf3},
        {"fxtract",0xd9,0xf4},{"fprem1",0xd9,0xf5},{"fdecstp",0xd9,0xf6},
        {"fincstp",0xd9,0xf7},{"fprem",0xd9,0xf8},{"fyl2xp1",0xd9,0xf9},
        {"fsqrt",0xd9,0xfa},{"fsincos",0xd9,0xfb},{"frndint",0xd9,0xfc},
        {"fscale",0xd9,0xfd},{"fsin",0xd9,0xfe},{"fcos",0xd9,0xff},{"fnop",0xd9,0xd0},
        {"fcompp",0xde,0xd9},{"fucompp",0xda,0xe9},{"fnclex",0xdb,0xe2},
        {"fninit",0xdb,0xe3},
        /* the popping forms with no operand: on %st(1) */
        {"faddp",0xde,0xc1},{"fmulp",0xde,0xc9},{"fsubp",0xde,0xe1},{"fsubrp",0xde,0xe9},
        {"fdivp",0xde,0xf1},{"fdivrp",0xde,0xf9},
        {"fxch",0xd9,0xc9},{"fcom",0xd8,0xd1},{"fcomp",0xd8,0xd9},{"fucom",0xdd,0xe1},
        {"fucomp",0xdd,0xe9},
    };
    /* one stack register: base byte pair, plus i */
    static const struct { const char *n; unsigned char b0, b1; } one[] = {
        {"fld",0xd9,0xc0},{"fst",0xdd,0xd0},{"fstp",0xdd,0xd8},{"fxch",0xd9,0xc8},
        {"ffree",0xdd,0xc0},{"ffreep",0xdf,0xc0},
        {"fcom",0xd8,0xd0},{"fcomp",0xd8,0xd8},{"fucom",0xdd,0xe0},{"fucomp",0xdd,0xe8},
        {"fcomi",0xdb,0xf0},{"fcomip",0xdf,0xf0},{"fcompi",0xdf,0xf0},
        {"fucomi",0xdb,0xe8},{"fucomip",0xdf,0xe8},{"fucompi",0xdf,0xe8},
        {"fcmovb",0xda,0xc0},{"fcmove",0xda,0xc8},{"fcmovbe",0xda,0xd0},
        {"fcmovu",0xda,0xd8},{"fcmovnb",0xdb,0xc0},{"fcmovne",0xdb,0xc8},
        {"fcmovnbe",0xdb,0xd0},{"fcmovnu",0xdb,0xd8},
    };
    /* arithmetic on two stack registers: %st(i), %st (D8) and %st, %st(i)
     * (DC, sub/div reversed in AT&T), the popping forms (DE) */
    static const struct { const char *n; unsigned to_st, to_sti; int pop; } ar[] = {
        {"fadd",0xc0,0xc0,0},{"fmul",0xc8,0xc8,0},{"fsub",0xe0,0xe0,0},
        {"fsubr",0xe8,0xe8,0},{"fdiv",0xf0,0xf0,0},{"fdivr",0xf8,0xf8,0},
        {"faddp",0,0xc0,1},{"fmulp",0,0xc8,1},{"fsubp",0,0xe0,1},
        {"fsubrp",0,0xe8,1},{"fdivp",0,0xf0,1},{"fdivrp",0,0xf8,1},
    };
    if (mn[0] != 'f') return 1;
    for (unsigned k = 0; k < sizeof mem / sizeof *mem; k++)
        if (!strcmp(mn, mem[k].n) && n == 1 && o[0].k == K_MEM) {
            if (mem[k].wait) put(x, 0x9b);
            unsigned char op = mem[k].op;
            return emit_rm(x, 0, 0, &op, 1, mem[k].ext, NULL, &o[0]);
        }
    /* fnstsw %ax, fstsw %ax */
    if ((!strcmp(mn, "fnstsw") || !strcmp(mn, "fstsw")) && n == 1 &&
        o[0].k == K_REG && o[0].reg == 0 && o[0].size == 2) {
        if (mn[1] != 'n') put(x, 0x9b);
        put(x, 0xdf); put(x, 0xe0);
        return 0;
    }
    if ((!strcmp(mn, "fnstsw") || !strcmp(mn, "fstsw")) && n == 0) {
        if (mn[1] != 'n') put(x, 0x9b);
        put(x, 0xdf); put(x, 0xe0);
        return 0;
    }
    if (!strcmp(mn, "fclex") && n == 0) { put(x, 0x9b); put(x, 0xdb); put(x, 0xe2); return 0; }
    if (!strcmp(mn, "finit") && n == 0) { put(x, 0x9b); put(x, 0xdb); put(x, 0xe3); return 0; }
    if (n == 0)
        for (unsigned k = 0; k < sizeof none / sizeof *none; k++)
            if (!strcmp(mn, none[k].n)) { put(x, none[k].b0); put(x, none[k].b1); return 0; }
    /* fxsave / fxrstor, and their 64-bit forms */
    if ((!strcmp(mn, "fxsave") || !strcmp(mn, "fxrstor") || !strcmp(mn, "fxsave64") ||
         !strcmp(mn, "fxrstor64")) && n == 1 && o[0].k == K_MEM) {
        unsigned char op[2] = { 0x0f, 0xae };
        return emit_rm(x, 0, mn[strlen(mn) - 1] == '4', op, 2, mn[2] == 'r' ? 1 : 0, NULL, &o[0]);
    }
    for (unsigned k = 0; k < sizeof one / sizeof *one; k++)
        if (!strcmp(mn, one[k].n)) {
            /* fcomi/fucomi/fcmov may name %st as the second operand */
            int i, two = !strncmp(mn, "fcomi", 5) || !strncmp(mn, "fucomi", 6) ||
                         !strncmp(mn, "fcompi", 6) || !strncmp(mn, "fucompi", 7) ||
                         !strncmp(mn, "fcmov", 5);
            if (n == 1 && o[0].k == K_ST && strncmp(mn, "fcmov", 5)) i = o[0].reg;
            else if (two && n == 2 && o[0].k == K_ST && o[1].k == K_ST && o[1].reg == 0) i = o[0].reg;
            else continue;
            put(x, one[k].b0); put(x, one[k].b1 + (unsigned)i);
            return 0;
        }
    for (unsigned k = 0; k < sizeof ar / sizeof *ar; k++)
        if (!strcmp(mn, ar[k].n)) {
            if (n == 1 && o[0].k == K_ST && !ar[k].pop) {        /* op %st(i): to %st */
                put(x, 0xd8); put(x, ar[k].to_st + (unsigned)o[0].reg);
                return 0;
            }
            if (n == 1 && o[0].k == K_ST && ar[k].pop) {         /* opp %st(i) */
                put(x, 0xde); put(x, ar[k].to_sti + (unsigned)o[0].reg);
                return 0;
            }
            if (n != 2 || o[0].k != K_ST || o[1].k != K_ST) continue;
            if (o[1].reg == 0 && !ar[k].pop) {
                put(x, 0xd8); put(x, ar[k].to_st + (unsigned)o[0].reg);
                return 0;
            }
            /* `faddp %st(1), %st` is faddp %st, %st(1), as GNU as reads it */
            if (o[1].reg == 0 && ar[k].pop) {
                put(x, 0xde); put(x, ar[k].to_sti + (unsigned)o[0].reg);
                return 0;
            }
            if (o[0].reg == 0) {
                put(x, ar[k].pop ? 0xde : 0xdc); put(x, ar[k].to_sti + (unsigned)o[1].reg);
                return 0;
            }
            return fail("'%s': one of the two is %%st", mn);
        }
    return 1;
}

/* ---- VEX: AVX, AVX2, FMA, F16C, BMI1/2 -------------------------------------
 *
 * The VEX prefix carries the legacy prefix (pp), the opcode map (0F, 0F38,
 * 0F3A), W, the vector length L and a second source register (vvvv,
 * inverted). The two-byte form C5 when the map is 0F, W is 0 and neither
 * X nor B is needed, as llvm-mc and GNU as choose. AT&T writes the
 * operands backwards: `vaddps %ymm2, %ymm1, %ymm0` is ymm0 = ymm1 + ymm2,
 * ModRM.rm the first, vvvv the second, ModRM.reg the last. Checked against
 * llvm-mc form by form (tests/golden/x86-avx.sh). */

enum { V_66 = 1, V_F3 = 2, V_F2 = 3 };
enum { M_0F = 1, M_38 = 2, M_3A = 3 };

static int emit_vex(struct ib *x, int pp, int map, int w, int l, int vvvv,
                    unsigned op, int regf, const struct opnd *rm)
{
    int r = regf >= 8, xb = 0, b = 0;
    if (rm->k == K_MEM) {
        if (rm->dot && rm->base != 16)
            return fail("'.+N' is an address only (%%rip)-relative");
        if (rm->base >= 8 && rm->base != 16) b = 1;
        if (rm->index >= 8) xb = 1;
        if (rm->seg) put(x, (unsigned)rm->seg);
    } else if (rm->reg >= 8)
        b = 1;
    if (map == M_0F && !w && !xb && !b) {
        put(x, 0xc5);
        put(x, (unsigned)(!r << 7 | (~vvvv & 15) << 3 | l << 2 | pp));
    } else {
        put(x, 0xc4);
        put(x, (unsigned)(!r << 7 | !xb << 6 | !b << 5 | map));
        put(x, (unsigned)(w << 7 | (~vvvv & 15) << 3 | l << 2 | pp));
    }
    put(x, op);
    return modrm(x, regf, rm);
}

/* ---- AVX-512 (EVEX) and the opmask instructions, from a learned table ----
 *
 * evex.inc is tools/evex-isa's: every EVEX form llvm-mc decodes (a sweep
 * of the maps, prefixes, W, vector lengths and opcodes), each assembled
 * back by llvm-mc with a distinct register in every operand to see which
 * field holds which -- ModRM reg (r) or rm (m), vvvv (v), aaa (a) -- its
 * displacement scale (disp8*N) probed, and whether it takes a mask and
 * {z}; and the opmask instructions' VEX forms the same way. The prefix
 * is built here, field by field as the architecture lays it out. */
struct ev_form {
    const char *mnem, *shape;
    unsigned char vex, map, pp, W, LL, b, op;
    signed char regext;
    unsigned char imm;
    short fixed;                /* an immediate the shape does not show */
    unsigned char mem, N, mask; /* mask: 0 none, 1 merging, 2 and {z} */
    const char *roles;
};
#include "evex.inc"
#define NEV ((int)(sizeof ev_forms / sizeof ev_forms[0]))

/* Does an operand need EVEX: a zmm, a vector register past 15, a mask
 * register, a decoration, a rounding control? */
static int ev_need(const struct opnd *o, int n)
{
    for (int k = 0; k < n; k++)
        if ((o[k].k == K_XMM && (o[k].size == 64 || o[k].reg >= 16)) ||
            o[k].k == K_RC || o[k].k == K_SAE ||
            o[k].mask || o[k].zero || o[k].bcast ||
            (o[k].vindex && o[k].index >= 16))
            return 1;
    return 0;
}

/* ...or a mask register, which only the table's forms name (VEX ones, the
 * opmask instructions', or EVEX) */
static int ev_only(const struct opnd *o, int n)
{
    for (int k = 0; k < n; k++)
        if (o[k].k == K_KREG || o[k].vindex)
            return 1;
    return ev_need(o, n);
}

static int ev_shape(const struct opnd *o, int n, char *sh, int cap)
{
    int at = 0;
    sh[0] = 0;
    for (int k = 0; k < n; k++) {
        const char *t;
        char b[16];
        switch (o[k].k) {
        case K_XMM: t = o[k].size == 64 ? "Z" : o[k].size == 32 ? "Y" : "X"; break;
        case K_KREG: t = "K"; break;
        case K_REG: t = o[k].size == 8 ? "Q" : o[k].size == 4 ? "D" : o[k].size == 2 ? "W" : "B"; break;
        case K_IMM: t = "I"; break;
        case K_RC: t = "{RC}"; break;
        case K_SAE: t = "{sae}"; break;
        case K_MEM:
            if (o[k].bcast) { snprintf(b, sizeof b, "M{1to%d}", o[k].bcast); t = b; }
            else if (o[k].vindex) t = o[k].vindex == 64 ? "MZ" : o[k].vindex == 32 ? "MY" : "MX";
            else t = "M";
            break;
        default: return 0;
        }
        at += snprintf(sh + at, (size_t)(cap - at), "%s%s", k ? "," : "", t);
        if (at >= cap) return 0;
    }
    return 1;
}

/* ModRM, SIB and displacement, an EVEX displacement scaled by N */
static int ev_modrm(struct ib *x, int regf, const struct opnd *rm, int N)
{
    int r3 = regf & 7;
    if (rm->k != K_MEM) {
        put(x, 0xc0u | (unsigned)r3 << 3 | (unsigned)(rm->reg & 7));
        return 0;
    }
    if (rm->base == 16 || rm->base < 0 || N <= 1)
        return modrm(x, regf, rm);
    int b3 = rm->base & 7;
    long long d = rm->disp;
    int mod = d == 0 && b3 != 5 ? 0 : (d % N == 0 && fits8(d / N)) ? 1 : 2;
    if (g_disp_force == 32) mod = 2;
    else if (g_disp_force == 8 && mod == 0) mod = 1;
    if (!fits32(d)) return fail("the displacement does not fit 32 bits");
    if (rm->index >= 0 || b3 == 4) {
        int sc = rm->scale == 8 ? 3 : rm->scale == 4 ? 2 : rm->scale == 2 ? 1 : 0;
        put(x, (unsigned)mod << 6 | (unsigned)r3 << 3 | 4);
        put(x, (unsigned)sc << 6 | (unsigned)((rm->index >= 0 ? rm->index : 4) & 7) << 3 |
               (unsigned)b3);
    } else {
        put(x, (unsigned)mod << 6 | (unsigned)r3 << 3 | (unsigned)b3);
    }
    if (mod == 1) {
        if (d % N || !fits8(d / N)) {   /* {disp8} asked of a disp that is not */
            mod = 2;
            return fail("the displacement %lld is not a multiple of %d in a byte, "
                        "as {disp8} asks", d, N);
        }
        x->disp_at = x->n; x->disp_len = 1; put_n(x, d / N, 1);
    }
    if (mod == 2) { x->disp_at = x->n; x->disp_len = 4; put_n(x, d, 4); }
    return 0;
}

/* 0 encoded, 1 not a form the table has, -1 an error */
static int evex(struct ib *x, const char *mn, const struct opnd *o, int n)
{
    char sh[96];
    int lo = 0, hi = NEV, mask = 0, zero = 0, seen = 0, need = ev_need(o, n);
    const char *why = NULL;
    if (!ev_shape(o, n, sh, sizeof sh))
        return 1;
    for (int k = 0; k < n; k++) {
        if (o[k].mask) mask = o[k].mask;
        if (o[k].zero) zero = 1;
    }
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (strcmp(ev_forms[mid].mnem, mn) < 0) lo = mid + 1; else hi = mid;
    }
    for (int i = lo; i < NEV && !strcmp(ev_forms[i].mnem, mn); i++) {
        const struct ev_form *f = &ev_forms[i];
        int reg = 0, rm = 0, v = 0, aaa = 0, rc = -1, ri = 0;
        const struct opnd *mo = NULL, *rmo = NULL;
        long long imm = 0;
        if (strcmp(f->shape, sh))
            continue;
        if (f->vex && need)
            continue;                       /* (its EVEX form follows) */
        seen = 1;
        if ((mask && !f->mask) || (zero && f->mask != 2)) {
            why = zero ? "zeroing ({z})" : "a mask ({%k1})";
            continue;
        }
        if (f->mask == 3 && !mask)
            return fail("'%s' wants a mask: {%%k1} (not %%k0) on its destination", mn);
        if (zero && !mask)
            return fail("{z} wants a mask: {%%k1} {z}");
        for (int k = 0; k < n; k++) {
            const struct opnd *q = &o[k];
            if (q->k == K_RC) { rc = (int)q->imm; continue; }
            if (q->k == K_SAE) continue;
            if (q->k == K_IMM) { imm = q->imm; continue; }
            if (q->k == K_MEM) { mo = q; continue; }
            switch (f->roles[ri++]) {
            case 'r': reg = q->reg; break;
            case 'm': rm = q->reg; rmo = q; break;
            case 'v': v = q->reg; break;
            case 'a': aaa = q->reg; break;
            }
        }
        if (!mo && !rmo && !f->mem && strchr(f->roles, 'm'))
            return fail("'%s': no operand for ModRM rm", mn);
        if (f->regext >= 0)
            reg = f->regext;
        if (mask)
            aaa = mask - 1;
        {
            int R = (reg >> 3) & 1, Rp = (reg >> 4) & 1, B, X, Vp = (v >> 4) & 1;
            if (mo) {
                B = mo->base >= 0 && mo->base != 16 ? (mo->base >> 3) & 1 : 0;
                X = mo->index >= 0 ? (mo->index >> 3) & 1 : 0;
                if (mo->vindex) Vp = (mo->index >> 4) & 1;   /* VSIB: V' extends it */
            } else {
                B = (rm >> 3) & 1;
                X = (rm >> 4) & 1;
            }
            if (f->vex) {
                if (f->map == 1 && !f->W && !X && !B) {
                    put(x, 0xc5);
                    put(x, (unsigned)(!R) << 7 | (unsigned)(~v & 15) << 3 |
                           (unsigned)f->LL << 2 | f->pp);
                } else {
                    put(x, 0xc4);
                    put(x, (unsigned)(!R) << 7 | (unsigned)(!X) << 6 | (unsigned)(!B) << 5 | f->map);
                    put(x, (unsigned)f->W << 7 | (unsigned)(~v & 15) << 3 |
                           (unsigned)f->LL << 2 | f->pp);
                }
            } else {
                put(x, 0x62);
                put(x, (unsigned)(!R) << 7 | (unsigned)(!X) << 6 | (unsigned)(!B) << 5 |
                       (unsigned)(!Rp) << 4 | f->map);
                put(x, (unsigned)f->W << 7 | (unsigned)(~v & 15) << 3 | 4u | f->pp);
                put(x, (unsigned)zero << 7 | (unsigned)(rc >= 0 ? rc : f->LL) << 5 |
                       (unsigned)f->b << 4 | (unsigned)(!Vp) << 3 | (unsigned)(aaa & 7));
            }
        }
        put(x, f->op);
        if (mo) {
            if (ev_modrm(x, reg, mo, f->vex ? 1 : f->N) < 0)
                return -1;
        } else {
            put(x, 0xc0u | (unsigned)(reg & 7) << 3 | (unsigned)(rm & 7));
        }
        if (f->imm) {
            x->imm_at = x->n;
            imm_put(x, imm, 1);
        } else if (f->fixed >= 0) {
            put(x, (unsigned)f->fixed);
        }
        return 0;
    }
    if (seen && why)
        return fail("'%s' takes no %s", mn, why);
    if (seen)
        return -1;
    return 1;
}

static int is_v(const struct opnd *o) { return o->k == K_XMM; }
static int is_vm(const struct opnd *o) { return o->k == K_XMM || o->k == K_MEM; }
static int vlen(const struct opnd *o) { return o->k == K_XMM && o->size == 32; }

/* form: N rm,vvvv,reg  n the same, xmm only  M rm,reg  m the same, xmm only
 *       I imm,rm,vvvv,reg  J the same, xmm only  i imm,rm,reg  j the same, xmm
 *       1 N with ymm only  2 I with ymm only */
struct vop { const char *n; unsigned char pp, map, op, w; char form; };
static const struct vop vops[] = {
    /* arithmetic */
    {"vaddps",0,M_0F,0x58,0,'N'},{"vaddpd",V_66,M_0F,0x58,0,'N'},{"vaddss",V_F3,M_0F,0x58,0,'n'},{"vaddsd",V_F2,M_0F,0x58,0,'n'},
    {"vmulps",0,M_0F,0x59,0,'N'},{"vmulpd",V_66,M_0F,0x59,0,'N'},{"vmulss",V_F3,M_0F,0x59,0,'n'},{"vmulsd",V_F2,M_0F,0x59,0,'n'},
    {"vsubps",0,M_0F,0x5c,0,'N'},{"vsubpd",V_66,M_0F,0x5c,0,'N'},{"vsubss",V_F3,M_0F,0x5c,0,'n'},{"vsubsd",V_F2,M_0F,0x5c,0,'n'},
    {"vminps",0,M_0F,0x5d,0,'N'},{"vminpd",V_66,M_0F,0x5d,0,'N'},{"vminss",V_F3,M_0F,0x5d,0,'n'},{"vminsd",V_F2,M_0F,0x5d,0,'n'},
    {"vdivps",0,M_0F,0x5e,0,'N'},{"vdivpd",V_66,M_0F,0x5e,0,'N'},{"vdivss",V_F3,M_0F,0x5e,0,'n'},{"vdivsd",V_F2,M_0F,0x5e,0,'n'},
    {"vmaxps",0,M_0F,0x5f,0,'N'},{"vmaxpd",V_66,M_0F,0x5f,0,'N'},{"vmaxss",V_F3,M_0F,0x5f,0,'n'},{"vmaxsd",V_F2,M_0F,0x5f,0,'n'},
    {"vsqrtps",0,M_0F,0x51,0,'M'},{"vsqrtpd",V_66,M_0F,0x51,0,'M'},{"vsqrtss",V_F3,M_0F,0x51,0,'n'},{"vsqrtsd",V_F2,M_0F,0x51,0,'n'},
    {"vrcpps",0,M_0F,0x53,0,'M'},{"vrcpss",V_F3,M_0F,0x53,0,'n'},{"vrsqrtps",0,M_0F,0x52,0,'M'},{"vrsqrtss",V_F3,M_0F,0x52,0,'n'},
    {"vandps",0,M_0F,0x54,0,'N'},{"vandpd",V_66,M_0F,0x54,0,'N'},{"vandnps",0,M_0F,0x55,0,'N'},{"vandnpd",V_66,M_0F,0x55,0,'N'},
    {"vorps",0,M_0F,0x56,0,'N'},{"vorpd",V_66,M_0F,0x56,0,'N'},{"vxorps",0,M_0F,0x57,0,'N'},{"vxorpd",V_66,M_0F,0x57,0,'N'},
    {"vunpcklps",0,M_0F,0x14,0,'N'},{"vunpckhps",0,M_0F,0x15,0,'N'},{"vunpcklpd",V_66,M_0F,0x14,0,'N'},{"vunpckhpd",V_66,M_0F,0x15,0,'N'},
    {"vaddsubps",V_F2,M_0F,0xd0,0,'N'},{"vaddsubpd",V_66,M_0F,0xd0,0,'N'},{"vhaddps",V_F2,M_0F,0x7c,0,'N'},{"vhaddpd",V_66,M_0F,0x7c,0,'N'},
    {"vhsubps",V_F2,M_0F,0x7d,0,'N'},{"vhsubpd",V_66,M_0F,0x7d,0,'N'},
    {"vucomiss",0,M_0F,0x2e,0,'m'},{"vucomisd",V_66,M_0F,0x2e,0,'m'},{"vcomiss",0,M_0F,0x2f,0,'m'},{"vcomisd",V_66,M_0F,0x2f,0,'m'},
    {"vcvtss2sd",V_F3,M_0F,0x5a,0,'n'},{"vcvtsd2ss",V_F2,M_0F,0x5a,0,'n'},
    {"vcvtdq2ps",0,M_0F,0x5b,0,'M'},{"vcvtps2dq",V_66,M_0F,0x5b,0,'M'},{"vcvttps2dq",V_F3,M_0F,0x5b,0,'M'},
    {"vmovshdup",V_F3,M_0F,0x16,0,'M'},{"vmovsldup",V_F3,M_0F,0x12,0,'M'},{"vmovddup",V_F2,M_0F,0x12,0,'M'},
    {"vmovhlps",0,M_0F,0x12,0,'n'},{"vmovlhps",0,M_0F,0x16,0,'n'},
    /* integer, 66 0F */
    {"vpxor",V_66,M_0F,0xef,0,'N'},{"vpor",V_66,M_0F,0xeb,0,'N'},{"vpand",V_66,M_0F,0xdb,0,'N'},{"vpandn",V_66,M_0F,0xdf,0,'N'},
    {"vpaddb",V_66,M_0F,0xfc,0,'N'},{"vpaddw",V_66,M_0F,0xfd,0,'N'},{"vpaddd",V_66,M_0F,0xfe,0,'N'},{"vpaddq",V_66,M_0F,0xd4,0,'N'},
    {"vpsubb",V_66,M_0F,0xf8,0,'N'},{"vpsubw",V_66,M_0F,0xf9,0,'N'},{"vpsubd",V_66,M_0F,0xfa,0,'N'},{"vpsubq",V_66,M_0F,0xfb,0,'N'},
    {"vpaddsb",V_66,M_0F,0xec,0,'N'},{"vpaddsw",V_66,M_0F,0xed,0,'N'},{"vpaddusb",V_66,M_0F,0xdc,0,'N'},{"vpaddusw",V_66,M_0F,0xdd,0,'N'},
    {"vpsubsb",V_66,M_0F,0xe8,0,'N'},{"vpsubsw",V_66,M_0F,0xe9,0,'N'},{"vpsubusb",V_66,M_0F,0xd8,0,'N'},{"vpsubusw",V_66,M_0F,0xd9,0,'N'},
    {"vpcmpeqb",V_66,M_0F,0x74,0,'N'},{"vpcmpeqw",V_66,M_0F,0x75,0,'N'},{"vpcmpeqd",V_66,M_0F,0x76,0,'N'},
    {"vpcmpgtb",V_66,M_0F,0x64,0,'N'},{"vpcmpgtw",V_66,M_0F,0x65,0,'N'},{"vpcmpgtd",V_66,M_0F,0x66,0,'N'},
    {"vpunpcklbw",V_66,M_0F,0x60,0,'N'},{"vpunpcklwd",V_66,M_0F,0x61,0,'N'},{"vpunpckldq",V_66,M_0F,0x62,0,'N'},
    {"vpunpcklqdq",V_66,M_0F,0x6c,0,'N'},{"vpunpckhbw",V_66,M_0F,0x68,0,'N'},{"vpunpckhwd",V_66,M_0F,0x69,0,'N'},
    {"vpunpckhdq",V_66,M_0F,0x6a,0,'N'},{"vpunpckhqdq",V_66,M_0F,0x6d,0,'N'},
    {"vpacksswb",V_66,M_0F,0x63,0,'N'},{"vpackssdw",V_66,M_0F,0x6b,0,'N'},{"vpackuswb",V_66,M_0F,0x67,0,'N'},
    {"vpmullw",V_66,M_0F,0xd5,0,'N'},{"vpmulhw",V_66,M_0F,0xe5,0,'N'},{"vpmulhuw",V_66,M_0F,0xe4,0,'N'},
    {"vpmuludq",V_66,M_0F,0xf4,0,'N'},{"vpmaddwd",V_66,M_0F,0xf5,0,'N'},
    {"vpminub",V_66,M_0F,0xda,0,'N'},{"vpmaxub",V_66,M_0F,0xde,0,'N'},{"vpminsw",V_66,M_0F,0xea,0,'N'},{"vpmaxsw",V_66,M_0F,0xee,0,'N'},
    {"vpavgb",V_66,M_0F,0xe0,0,'N'},{"vpavgw",V_66,M_0F,0xe3,0,'N'},{"vpsadbw",V_66,M_0F,0xf6,0,'N'},
    {"vpsllw",V_66,M_0F,0xf1,0,'N'},{"vpslld",V_66,M_0F,0xf2,0,'N'},{"vpsllq",V_66,M_0F,0xf3,0,'N'},
    {"vpsrlw",V_66,M_0F,0xd1,0,'N'},{"vpsrld",V_66,M_0F,0xd2,0,'N'},{"vpsrlq",V_66,M_0F,0xd3,0,'N'},
    {"vpsraw",V_66,M_0F,0xe1,0,'N'},{"vpsrad",V_66,M_0F,0xe2,0,'N'},
    {"vpshufd",V_66,M_0F,0x70,0,'i'},{"vpshufhw",V_F3,M_0F,0x70,0,'i'},{"vpshuflw",V_F2,M_0F,0x70,0,'i'},
    {"vshufps",0,M_0F,0xc6,0,'I'},{"vshufpd",V_66,M_0F,0xc6,0,'I'},
    {"vcmpps",0,M_0F,0xc2,0,'I'},{"vcmppd",V_66,M_0F,0xc2,0,'I'},{"vcmpss",V_F3,M_0F,0xc2,0,'J'},{"vcmpsd",V_F2,M_0F,0xc2,0,'J'},
    /* 66 0F38 */
    {"vpshufb",V_66,M_38,0x00,0,'N'},{"vphaddw",V_66,M_38,0x01,0,'N'},{"vphaddd",V_66,M_38,0x02,0,'N'},
    {"vphaddsw",V_66,M_38,0x03,0,'N'},{"vpmaddubsw",V_66,M_38,0x04,0,'N'},{"vphsubw",V_66,M_38,0x05,0,'N'},
    {"vphsubd",V_66,M_38,0x06,0,'N'},{"vphsubsw",V_66,M_38,0x07,0,'N'},{"vpsignb",V_66,M_38,0x08,0,'N'},
    {"vpsignw",V_66,M_38,0x09,0,'N'},{"vpsignd",V_66,M_38,0x0a,0,'N'},{"vpmulhrsw",V_66,M_38,0x0b,0,'N'},
    {"vpermilps",V_66,M_38,0x0c,0,'N'},{"vpermilpd",V_66,M_38,0x0d,0,'N'},{"vtestps",V_66,M_38,0x0e,0,'M'},
    {"vtestpd",V_66,M_38,0x0f,0,'M'},{"vpermps",V_66,M_38,0x16,0,'1'},{"vptest",V_66,M_38,0x17,0,'M'},
    {"vpabsb",V_66,M_38,0x1c,0,'M'},{"vpabsw",V_66,M_38,0x1d,0,'M'},{"vpabsd",V_66,M_38,0x1e,0,'M'},
    {"vpmuldq",V_66,M_38,0x28,0,'N'},{"vpcmpeqq",V_66,M_38,0x29,0,'N'},{"vpackusdw",V_66,M_38,0x2b,0,'N'},
    {"vpermd",V_66,M_38,0x36,0,'1'},{"vpcmpgtq",V_66,M_38,0x37,0,'N'},
    {"vpminsb",V_66,M_38,0x38,0,'N'},{"vpminsd",V_66,M_38,0x39,0,'N'},{"vpminuw",V_66,M_38,0x3a,0,'N'},
    {"vpminud",V_66,M_38,0x3b,0,'N'},{"vpmaxsb",V_66,M_38,0x3c,0,'N'},{"vpmaxsd",V_66,M_38,0x3d,0,'N'},
    {"vpmaxuw",V_66,M_38,0x3e,0,'N'},{"vpmaxud",V_66,M_38,0x3f,0,'N'},{"vpmulld",V_66,M_38,0x40,0,'N'},
    {"vphminposuw",V_66,M_38,0x41,0,'m'},
    {"vpsrlvd",V_66,M_38,0x45,0,'N'},{"vpsrlvq",V_66,M_38,0x45,1,'N'},{"vpsravd",V_66,M_38,0x46,0,'N'},
    {"vpsllvd",V_66,M_38,0x47,0,'N'},{"vpsllvq",V_66,M_38,0x47,1,'N'},
    {"vaesimc",V_66,M_38,0xdb,0,'m'},{"vaesenc",V_66,M_38,0xdc,0,'N'},{"vaesenclast",V_66,M_38,0xdd,0,'N'},
    {"vaesdec",V_66,M_38,0xde,0,'N'},{"vaesdeclast",V_66,M_38,0xdf,0,'N'},
    /* 66 0F3A, with an immediate */
    {"vpermq",V_66,M_3A,0x00,1,'k'},{"vpermpd",V_66,M_3A,0x01,1,'k'},{"vpblendd",V_66,M_3A,0x02,0,'I'},
    {"vpermilps",V_66,M_3A,0x04,0,'i'},{"vpermilpd",V_66,M_3A,0x05,0,'i'},{"vperm2f128",V_66,M_3A,0x06,0,'2'},
    {"vroundps",V_66,M_3A,0x08,0,'i'},{"vroundpd",V_66,M_3A,0x09,0,'i'},{"vroundss",V_66,M_3A,0x0a,0,'J'},
    {"vroundsd",V_66,M_3A,0x0b,0,'J'},{"vblendps",V_66,M_3A,0x0c,0,'I'},{"vblendpd",V_66,M_3A,0x0d,0,'I'},
    {"vpblendw",V_66,M_3A,0x0e,0,'I'},{"vpalignr",V_66,M_3A,0x0f,0,'I'},{"vinsertps",V_66,M_3A,0x21,0,'J'},
    {"vdpps",V_66,M_3A,0x40,0,'I'},{"vdppd",V_66,M_3A,0x41,0,'J'},{"vmpsadbw",V_66,M_3A,0x42,0,'I'},
    {"vpclmulqdq",V_66,M_3A,0x44,0,'I'},{"vperm2i128",V_66,M_3A,0x46,0,'2'},
    {"vpcmpestrm",V_66,M_3A,0x60,0,'j'},{"vpcmpestri",V_66,M_3A,0x61,0,'j'},
    {"vpcmpistrm",V_66,M_3A,0x62,0,'j'},{"vpcmpistri",V_66,M_3A,0x63,0,'j'},
    {"vaeskeygenassist",V_66,M_3A,0xdf,0,'j'},
};

/* the FMA family: 132, 213, 231 at op, op+0x10, op+0x20 */
struct fma { const char *n; unsigned char op; int scalar; };
static const struct fma fmas[] = {
    {"vfmaddsub",0x96,0},{"vfmsubadd",0x97,0},{"vfmadd",0x98,0},{"vfmadd",0x99,1},
    {"vfmsub",0x9a,0},{"vfmsub",0x9b,1},{"vfnmadd",0x9c,0},{"vfnmadd",0x9d,1},
    {"vfnmsub",0x9e,0},{"vfnmsub",0x9f,1},
};

static int vex_commutes(const char *n, long long imm)
{
    static const char *const c[] = {
        "vaddps", "vaddpd", "vmulps", "vmulpd", "vandps", "vandpd", "vorps", "vorpd",
        "vxorps", "vxorpd", "vpaddb", "vpaddw", "vpaddd", "vpaddq", "vpaddsb", "vpaddsw",
        "vpaddusb", "vpaddusw", "vpand", "vpor", "vpxor", "vpavgb", "vpavgw", "vpcmpeqb",
        "vpcmpeqw", "vpcmpeqd", "vpmaddwd", "vpmaxsw", "vpmaxub", "vpminsw", "vpminub",
        "vpmulhuw", "vpmulhw", "vpmullw", "vpmuludq", "vpsadbw", NULL
    };
    for (int k = 0; c[k]; k++)
        if (!strcmp(n, c[k])) return 1;
    /* eq, unord, neq, ord and their signalling and quiet twins */
    if (!strcmp(n, "vcmpps") || !strcmp(n, "vcmppd"))
        return (imm & 3) == 0 || (imm & 3) == 3;
    return 0;
}

/* AT&T may suffix a vector operation whose operand size the registers do
 * not say: vcvtpd2psx (%rax), vcvttpd2dqy ... -- 'x' or 'y' */
static int avx(struct ib *x, const char *mn, const struct opnd *o, int n)
{
    /* vcmp<pred>{ps,pd,ss,sd}: vcmp with the predicate, AVX's 32 */
    if (!strncmp(mn, "vcmp", 4) && strlen(mn) > 6 && n == 3) {
        static const char *const pr[32] = {
            "eq", "lt", "le", "unord", "neq", "nlt", "nle", "ord",
            "eq_uq", "nge", "ngt", "false", "neq_oq", "ge", "gt", "true",
            "eq_os", "lt_oq", "le_oq", "unord_s", "neq_us", "nlt_uq", "nle_uq", "ord_s",
            "eq_us", "nge_uq", "ngt_uq", "false_os", "neq_os", "ge_oq", "gt_oq", "true_us"
        };
        size_t ml = strlen(mn);
        const char *ty = mn + ml - 2;
        if (!strcmp(ty, "ps") || !strcmp(ty, "pd") || !strcmp(ty, "ss") || !strcmp(ty, "sd"))
            for (int p = 0; p < 32; p++)
                if (ml - 6 == strlen(pr[p]) && !strncmp(mn + 4, pr[p], ml - 6)) {
                    char base[8];
                    struct opnd q[4];
                    snprintf(base, sizeof base, "vcmp%s", ty);
                    memset(&q[0], 0, sizeof q[0]);
                    q[0].k = K_IMM;
                    q[0].imm = p;
                    q[0].base = q[0].index = -1;
                    q[1] = o[0]; q[2] = o[1]; q[3] = o[2];
                    return avx(x, base, q, 4);
                }
    }
    /* the table */
    for (unsigned k = 0; k < sizeof vops / sizeof *vops; k++) {
        const struct vop *e = &vops[k];
        if (strcmp(mn, e->n)) continue;
        int imm = e->form == 'I' || e->form == 'J' || e->form == 'i' ||
                  e->form == 'j' || e->form == '2' || e->form == 'k';
        int at = imm;
        int three = e->form == 'N' || e->form == 'n' || e->form == 'I' ||
                    e->form == 'J' || e->form == '1' || e->form == '2';
        if (n != (three ? 3 : 2) + imm || (imm && o[0].k != K_IMM)) continue;
        const struct opnd *rm = &o[at], *vv = three ? &o[at + 1] : NULL,
                          *rg = &o[n - 1];
        if (!is_vm(rm) || !is_v(rg) || (vv && !is_v(vv))) continue;
        if (rm->k == K_MEM && (!strcmp(e->n, "vmovhlps") || !strcmp(e->n, "vmovlhps")))
            continue;
        int l = vlen(rg);
        int xonly = e->form == 'n' || e->form == 'm' || e->form == 'J' || e->form == 'j';
        if (xonly && (l || (rm->k == K_XMM && vlen(rm)) || (vv && vlen(vv)))) continue;
        if ((e->form == '1' || e->form == '2' || e->form == 'k') && !l) continue;
        /* the shifts by a count: always an xmm, vpsllw %xmm1, %ymm2, %ymm3 */
        int count = e->map == M_0F && (e->op == 0xd1 || e->op == 0xd2 || e->op == 0xd3 ||
                                        e->op == 0xe1 || e->op == 0xe2 || e->op == 0xf1 ||
                                        e->op == 0xf2 || e->op == 0xf3);
        if (count && rm->k == K_XMM && vlen(rm)) continue;
        if (!xonly && ((rm->k == K_XMM && vlen(rm) != l && !count) ||
                       (vv && vlen(vv) != l)))
            continue;
        /* a commutative operation whose rm needs VEX.B takes its sources
         * the other way round when that fits the 2-byte VEX, as llvm-mc
         * does (vcmp when the predicate is symmetric) */
        if (vv && rm->k == K_XMM && rm->reg >= 8 && vv->reg < 8 && e->map == M_0F &&
            !e->w && vex_commutes(e->n, imm ? o[0].imm : 0)) {
            const struct opnd *t = rm; rm = vv; vv = t;
        }
        if (emit_vex(x, e->pp, e->map, e->w, xonly ? 0 : l, vv ? vv->reg : 0,
                     e->op, rg->reg, rm) < 0)
            return -1;
        if (imm) imm_put(x, o[0].imm, 1);
        return 0;
    }
    /* FMA: vfmadd231ps */
    for (unsigned k = 0; k < sizeof fmas / sizeof *fmas; k++) {
        size_t bl = strlen(fmas[k].n);
        if (strncmp(mn, fmas[k].n, bl) || strlen(mn) != bl + 5) continue;
        const char *ord = mn + bl, *ty = mn + bl + 3;
        int row = !strncmp(ord, "132", 3) ? 0 : !strncmp(ord, "213", 3) ? 1
                : !strncmp(ord, "231", 3) ? 2 : -1;
        int sc = !strcmp(ty, "ss") || !strcmp(ty, "sd");
        int pk = !strcmp(ty, "ps") || !strcmp(ty, "pd");
        if (row < 0 || (!sc && !pk) || sc != fmas[k].scalar) continue;
        if (n != 3 || !is_vm(&o[0]) || !is_v(&o[1]) || !is_v(&o[2]))
            return fail("'%s' takes xmm/ymm/memory and two registers", mn);
        int l = vlen(&o[2]);
        if (sc && l) return fail("'%s' is scalar: xmm registers", mn);
        if (vlen(&o[1]) != l || (o[0].k == K_XMM && vlen(&o[0]) != l))
            return fail("'%s': the registers are all xmm or all ymm", mn);
        return emit_vex(x, V_66, M_38, ty[1] == 'd', l, o[1].reg,
                        (unsigned)(fmas[k].op + 0x10 * row), o[2].reg, &o[0]);
    }
    /* the moves: load (rm -> reg) and store (reg -> rm) */
    {
        static const struct { const char *n; int pp; unsigned ld, st; } mv[] = {
            {"vmovaps",0,0x28,0x29},{"vmovapd",V_66,0x28,0x29},{"vmovups",0,0x10,0x11},
            {"vmovupd",V_66,0x10,0x11},{"vmovdqa",V_66,0x6f,0x7f},{"vmovdqu",V_F3,0x6f,0x7f},
        };
        for (unsigned k = 0; k < sizeof mv / sizeof *mv; k++)
            if (!strcmp(mn, mv[k].n) && n == 2) {
                /* register to register: the load form, as llvm-mc writes it
                 * -- unless only the store form needs no 3-byte VEX */
                if (is_v(&o[1]) && is_vm(&o[0])) {
                    if (o[0].k == K_XMM && vlen(&o[0]) != vlen(&o[1]))
                        return fail("'%s' moves between registers of one size", mn);
                    if (o[0].k == K_XMM && o[0].reg >= 8 && o[1].reg < 8)
                        return emit_vex(x, mv[k].pp, M_0F, 0, vlen(&o[1]), 0, mv[k].st,
                                        o[0].reg, &o[1]);
                    return emit_vex(x, mv[k].pp, M_0F, 0, vlen(&o[1]), 0, mv[k].ld,
                                    o[1].reg, &o[0]);
                }
                if (is_v(&o[0]) && o[1].k == K_MEM)
                    return emit_vex(x, mv[k].pp, M_0F, 0, vlen(&o[0]), 0, mv[k].st,
                                    o[0].reg, &o[1]);
                return fail("'%s' takes a register and a register or memory", mn);
            }
        static const struct { const char *n; int pp, map; unsigned op; } nt[] = {
            {"vmovntps",0,M_0F,0x2b},{"vmovntpd",V_66,M_0F,0x2b},{"vmovntdq",V_66,M_0F,0xe7},
        };
        for (unsigned k = 0; k < 3; k++)
            if (!strcmp(mn, nt[k].n) && n == 2 && is_v(&o[0]) && o[1].k == K_MEM)
                return emit_vex(x, nt[k].pp, nt[k].map, 0, vlen(&o[0]), 0, nt[k].op,
                                o[0].reg, &o[1]);
        if (!strcmp(mn, "vmovntdqa") && n == 2 && o[0].k == K_MEM && is_v(&o[1]))
            return emit_vex(x, V_66, M_38, 0, vlen(&o[1]), 0, 0x2a, o[1].reg, &o[0]);
        if (!strcmp(mn, "vlddqu") && n == 2 && o[0].k == K_MEM && is_v(&o[1]))
            return emit_vex(x, V_F2, M_0F, 0, vlen(&o[1]), 0, 0xf0, o[1].reg, &o[0]);
        /* vmovss/vmovsd: memory, or three registers */
        if ((!strcmp(mn, "vmovss") || !strcmp(mn, "vmovsd"))) {
            int pp = mn[5] == 's' ? V_F3 : V_F2;
            if (n == 2 && o[0].k == K_MEM && is_v(&o[1]) && !vlen(&o[1]))
                return emit_vex(x, pp, M_0F, 0, 0, 0, 0x10, o[1].reg, &o[0]);
            if (n == 2 && is_v(&o[0]) && !vlen(&o[0]) && o[1].k == K_MEM)
                return emit_vex(x, pp, M_0F, 0, 0, 0, 0x11, o[0].reg, &o[1]);
            if (n == 3 && is_v(&o[0]) && is_v(&o[1]) && is_v(&o[2]) &&
                !vlen(&o[0]) && !vlen(&o[1]) && !vlen(&o[2])) {
                if (o[0].reg >= 8 && o[2].reg < 8)    /* the 2-byte VEX when it can */
                    return emit_vex(x, pp, M_0F, 0, 0, o[1].reg, 0x11, o[0].reg, &o[2]);
                return emit_vex(x, pp, M_0F, 0, 0, o[1].reg, 0x10, o[2].reg, &o[0]);
            }
            return fail("'%s' takes memory and xmm, or three xmm registers", mn);
        }
        /* vmovlps & co: load with a merge source, or store */
        static const struct { const char *n; int pp; unsigned op; } lh[] = {
            {"vmovlps",0,0x12},{"vmovhps",0,0x16},{"vmovlpd",V_66,0x12},{"vmovhpd",V_66,0x16},
        };
        for (unsigned k = 0; k < 4; k++)
            if (!strcmp(mn, lh[k].n)) {
                if (n == 3 && o[0].k == K_MEM && is_v(&o[1]) && is_v(&o[2]))
                    return emit_vex(x, lh[k].pp, M_0F, 0, 0, o[1].reg, lh[k].op, o[2].reg, &o[0]);
                if (n == 2 && is_v(&o[0]) && o[1].k == K_MEM)
                    return emit_vex(x, lh[k].pp, M_0F, 0, 0, 0, lh[k].op + 1, o[0].reg, &o[1]);
                return fail("'%s' loads memory merged into xmm, or stores", mn);
            }
    }
    /* vmovd / vmovq */
    if (!strcmp(mn, "vmovd") || !strcmp(mn, "vmovq")) {
        int q = mn[4] == 'q';
        for (int k = 0; k < n; k++)
            /* (vmovd of a 64-bit register is vmovq, as llvm-mc takes it) */
            if (o[k].k == K_REG && o[k].size != 8 && (q || o[k].size != 4))
                return fail("'%s' moves a %s-bit register", mn, q ? "64" : "32- or 64");
        if (n == 2 && is_v(&o[1]) && !vlen(&o[1]) &&
            (o[0].k == K_REG || (o[0].k == K_MEM && !q)))
            return emit_vex(x, V_66, M_0F, o[0].k == K_REG && o[0].size == 8, 0, 0, 0x6e,
                            o[1].reg, &o[0]);
        if (n == 2 && is_v(&o[0]) && !vlen(&o[0]) &&
            (o[1].k == K_REG || (o[1].k == K_MEM && !q)))
            return emit_vex(x, V_66, M_0F, o[1].k == K_REG && o[1].size == 8, 0, 0, 0x7e,
                            o[0].reg, &o[1]);
        if (q && n == 2 && is_v(&o[1]) && !vlen(&o[1]) && is_vm(&o[0]) &&
            !(o[0].k == K_XMM && vlen(&o[0]))) {
            if (o[0].k == K_XMM && o[0].reg >= 8 && o[1].reg < 8)
                return emit_vex(x, V_66, M_0F, 0, 0, 0, 0xd6, o[0].reg, &o[1]);
            return emit_vex(x, V_F3, M_0F, 0, 0, 0, 0x7e, o[1].reg, &o[0]);
        }
        if (q && n == 2 && is_v(&o[0]) && o[1].k == K_MEM)
            return emit_vex(x, V_66, M_0F, 0, 0, 0, 0xd6, o[0].reg, &o[1]);
        return fail("'%s': these operands", mn);
    }
    /* the shifts by a constant: vvvv is the destination */
    {
        static const struct { const char *n; unsigned op; int ext; } sh[] = {
            {"vpsrlw",0x71,2},{"vpsraw",0x71,4},{"vpsllw",0x71,6},{"vpsrld",0x72,2},
            {"vpsrad",0x72,4},{"vpslld",0x72,6},{"vpsrlq",0x73,2},{"vpsrldq",0x73,3},
            {"vpsllq",0x73,6},{"vpslldq",0x73,7},
        };
        for (unsigned k = 0; k < sizeof sh / sizeof *sh; k++)
            if (!strcmp(mn, sh[k].n) && n == 3 && o[0].k == K_IMM && is_v(&o[1]) &&
                is_v(&o[2]) && vlen(&o[1]) == vlen(&o[2])) {
                if (emit_vex(x, V_66, M_0F, 0, vlen(&o[2]), o[2].reg, sh[k].op, sh[k].ext,
                             &o[1]) < 0) return -1;
                imm_put(x, o[0].imm, 1);
                return 0;
            }
    }
    /* conversions whose two sides differ in width */
    {
        /* narrowing (ymm or xmm source, xmm destination): L from the
         * source, or from the x/y suffix for memory */
        static const struct { const char *n; int pp, map; unsigned op; } nw[] = {
            {"vcvtpd2ps",V_66,M_0F,0x5a},{"vcvtpd2dq",V_F2,M_0F,0xe6},{"vcvttpd2dq",V_66,M_0F,0xe6},
        };
        for (unsigned k = 0; k < 3; k++) {
            size_t bl = strlen(nw[k].n);
            if (strncmp(mn, nw[k].n, bl) || (mn[bl] && (mn[bl] != 'x' && mn[bl] != 'y'))) continue;
            if (n != 2 || !is_vm(&o[0]) || !is_v(&o[1]) || vlen(&o[1]))
                return fail("'%s' converts into an xmm register", mn);
            int l = o[0].k == K_XMM ? vlen(&o[0]) : mn[bl] == 'y';
            if (o[0].k == K_MEM && !mn[bl])
                return fail("'%s' of memory: say its size, %sx or %sy", mn, mn, mn);
            return emit_vex(x, nw[k].pp, nw[k].map, 0, l, 0, nw[k].op, o[1].reg, &o[0]);
        }
        /* widening (xmm source, ymm or xmm destination): L from the destination */
        static const struct { const char *n; int pp, map; unsigned op; } wd[] = {
            {"vcvtps2pd",0,M_0F,0x5a},{"vcvtdq2pd",V_F3,M_0F,0xe6},{"vcvtph2ps",V_66,M_38,0x13},
            {"vpmovsxbw",V_66,M_38,0x20},{"vpmovsxbd",V_66,M_38,0x21},{"vpmovsxbq",V_66,M_38,0x22},
            {"vpmovsxwd",V_66,M_38,0x23},{"vpmovsxwq",V_66,M_38,0x24},{"vpmovsxdq",V_66,M_38,0x25},
            {"vpmovzxbw",V_66,M_38,0x30},{"vpmovzxbd",V_66,M_38,0x31},{"vpmovzxbq",V_66,M_38,0x32},
            {"vpmovzxwd",V_66,M_38,0x33},{"vpmovzxwq",V_66,M_38,0x34},{"vpmovzxdq",V_66,M_38,0x35},
            {"vbroadcastss",V_66,M_38,0x18},{"vbroadcastsd",V_66,M_38,0x19},
            {"vbroadcastf128",V_66,M_38,0x1a},{"vbroadcasti128",V_66,M_38,0x5a},
            {"vpbroadcastd",V_66,M_38,0x58},{"vpbroadcastq",V_66,M_38,0x59},
            {"vpbroadcastb",V_66,M_38,0x78},{"vpbroadcastw",V_66,M_38,0x79},
        };
        for (unsigned k = 0; k < sizeof wd / sizeof *wd; k++)
            if (!strcmp(mn, wd[k].n)) {
                if (n != 2 || !is_vm(&o[0]) || !is_v(&o[1]) || (o[0].k == K_XMM && vlen(&o[0])))
                    return fail("'%s' takes xmm or memory, and a register", mn);
                if (wd[k].map == M_38 && (wd[k].op == 0x19 || wd[k].op == 0x1a ||
                                          wd[k].op == 0x5a) && !vlen(&o[1]))
                    return fail("'%s' writes a ymm register", mn);
                if ((wd[k].op == 0x1a || (wd[k].op == 0x5a && wd[k].map == M_38)) && o[0].k != K_MEM)
                    return fail("'%s' loads from memory", mn);
                return emit_vex(x, wd[k].pp, wd[k].map, 0, vlen(&o[1]), 0, wd[k].op,
                                o[1].reg, &o[0]);
            }
        /* vcvtps2ph: imm, register, xmm or memory */
        if (!strcmp(mn, "vcvtps2ph") && n == 3 && o[0].k == K_IMM && is_v(&o[1]) &&
            is_vm(&o[2]) && !(o[2].k == K_XMM && vlen(&o[2]))) {
            if (emit_vex(x, V_66, M_3A, 0, vlen(&o[1]), 0, 0x1d, o[1].reg, &o[2]) < 0) return -1;
            imm_put(x, o[0].imm, 1);
            return 0;
        }
    }
    /* 128-bit lanes in and out of a ymm register */
    {
        static const struct { const char *n; unsigned op; int ins; } ln[] = {
            {"vinsertf128",0x18,1},{"vextractf128",0x19,0},
            {"vinserti128",0x38,1},{"vextracti128",0x39,0},
        };
        for (unsigned k = 0; k < 4; k++)
            if (!strcmp(mn, ln[k].n)) {
                if (ln[k].ins) {
                    if (n != 4 || o[0].k != K_IMM || !is_vm(&o[1]) || (o[1].k == K_XMM && vlen(&o[1])) ||
                        !is_v(&o[2]) || !vlen(&o[2]) || !is_v(&o[3]) || !vlen(&o[3]))
                        return fail("'%s' takes an immediate, xmm or memory, and two ymm", mn);
                    if (emit_vex(x, V_66, M_3A, 0, 1, o[2].reg, ln[k].op, o[3].reg, &o[1]) < 0)
                        return -1;
                } else {
                    if (n != 3 || o[0].k != K_IMM || !is_v(&o[1]) || !vlen(&o[1]) ||
                        !is_vm(&o[2]) || (o[2].k == K_XMM && vlen(&o[2])))
                        return fail("'%s' takes an immediate, ymm, and xmm or memory", mn);
                    if (emit_vex(x, V_66, M_3A, 0, 1, 0, ln[k].op, o[1].reg, &o[2]) < 0)
                        return -1;
                }
                imm_put(x, o[0].imm, 1);
                return 0;
            }
    }
    /* the variable blends: a fourth register in the immediate's top half */
    {
        static const struct { const char *n; unsigned op; } bv[] = {
            {"vblendvps",0x4a},{"vblendvpd",0x4b},{"vpblendvb",0x4c},
        };
        for (unsigned k = 0; k < 3; k++)
            if (!strcmp(mn, bv[k].n)) {
                if (n != 4 || !is_v(&o[0]) || !is_vm(&o[1]) || !is_v(&o[2]) || !is_v(&o[3]))
                    return fail("'%s' takes a mask register, a source, and two registers", mn);
                if (emit_vex(x, V_66, M_3A, 0, vlen(&o[3]), o[2].reg, bv[k].op, o[3].reg,
                             &o[1]) < 0) return -1;
                imm_put(x, (long long)(o[0].reg << 4), 1);
                return 0;
            }
    }
    /* masked moves: load (mem, mask, reg) or store (reg, mask, mem) */
    {
        static const struct { const char *n; unsigned ld, st; int w; } mm[] = {
            {"vmaskmovps",0x2c,0x2e,0},{"vmaskmovpd",0x2d,0x2f,0},
            {"vpmaskmovd",0x8c,0x8e,0},{"vpmaskmovq",0x8c,0x8e,1},
        };
        for (unsigned k = 0; k < 4; k++)
            if (!strcmp(mn, mm[k].n) && n == 3 && is_v(&o[1])) {
                if (o[0].k == K_MEM && is_v(&o[2]))
                    return emit_vex(x, V_66, M_38, mm[k].w, vlen(&o[2]), o[1].reg, mm[k].ld,
                                    o[2].reg, &o[0]);
                if (is_v(&o[0]) && o[2].k == K_MEM)
                    return emit_vex(x, V_66, M_38, mm[k].w, vlen(&o[0]), o[1].reg, mm[k].st,
                                    o[0].reg, &o[2]);
            }
    }
    /* to and from the general registers */
    {
        /* vcvtsi2ss{l,q} / vcvtsi2sd{l,q}: gpr or memory, xmm, xmm */
        static const struct { const char *n; int pp; } cs[] = {
            {"vcvtsi2ss",V_F3},{"vcvtsi2sd",V_F2},
        };
        for (unsigned k = 0; k < 2; k++) {
            size_t bl = strlen(cs[k].n);
            if (strncmp(mn, cs[k].n, bl) || (mn[bl] && mn[bl] != 'l' && mn[bl] != 'q') ||
                (mn[bl] && mn[bl + 1])) continue;
            if (n != 3 || (o[0].k != K_REG && o[0].k != K_MEM) || !is_v(&o[1]) || !is_v(&o[2]))
                return fail("'%s' takes a register or memory, and two xmm", mn);
            if (o[0].k == K_MEM && !mn[bl])
                return fail("'%s' of memory: say its size, %sl or %sq", mn, mn, mn);
            int w = o[0].k == K_REG ? o[0].size == 8 : mn[bl] == 'q';
            return emit_vex(x, cs[k].pp, M_0F, w, 0, o[1].reg, 0x2a, o[2].reg, &o[0]);
        }
        static const struct { const char *n; int pp; unsigned op; } ci[] = {
            {"vcvttss2si",V_F3,0x2c},{"vcvttsd2si",V_F2,0x2c},{"vcvtss2si",V_F3,0x2d},
            {"vcvtsd2si",V_F2,0x2d},
        };
        for (unsigned k = 0; k < 4; k++) {
            size_t bl = strlen(ci[k].n);
            if (strncmp(mn, ci[k].n, bl) || (mn[bl] && mn[bl] != 'l' && mn[bl] != 'q') ||
                (mn[bl] && mn[bl + 1])) continue;
            if (n != 2 || !is_vm(&o[0]) || o[1].k != K_REG || (o[1].size != 4 && o[1].size != 8))
                return fail("'%s' takes xmm or memory, and a 32- or 64-bit register", mn);
            return emit_vex(x, ci[k].pp, M_0F, o[1].size == 8, 0, 0, ci[k].op, o[1].reg, &o[0]);
        }
        static const struct { const char *n; int pp; unsigned op; } mk[] = {
            {"vmovmskps",0,0x50},{"vmovmskpd",V_66,0x50},{"vpmovmskb",V_66,0xd7},
        };
        for (unsigned k = 0; k < 3; k++)
            if (!strcmp(mn, mk[k].n) && n == 2 && is_v(&o[0]) && o[1].k == K_REG &&
                (o[1].size == 4 || o[1].size == 8))
                return emit_vex(x, mk[k].pp, M_0F, 0, vlen(&o[0]), 0, mk[k].op, o[1].reg, &o[0]);
        /* extracts and inserts */
        static const struct { const char *n; int ins, w; unsigned op; } ei[] = {
            {"vpextrb",0,0,0x14},{"vpextrd",0,0,0x16},{"vpextrq",0,1,0x16},
            {"vextractps",0,0,0x17},{"vpinsrb",1,0,0x20},{"vpinsrd",1,0,0x22},
            {"vpinsrq",1,1,0x22},
        };
        for (unsigned k = 0; k < sizeof ei / sizeof *ei; k++)
            if (!strcmp(mn, ei[k].n)) {
                if (ei[k].ins) {
                    if (n != 4 || o[0].k != K_IMM || (o[1].k != K_REG && o[1].k != K_MEM) ||
                        !is_v(&o[2]) || !is_v(&o[3]))
                        return fail("'%s' takes an immediate, a register or memory, and two xmm", mn);
                    if (emit_vex(x, V_66, M_3A, ei[k].w, 0, o[2].reg, ei[k].op, o[3].reg, &o[1]) < 0)
                        return -1;
                } else {
                    if (n != 3 || o[0].k != K_IMM || !is_v(&o[1]) ||
                        (o[2].k != K_REG && o[2].k != K_MEM))
                        return fail("'%s' takes an immediate, xmm, and a register or memory", mn);
                    if (emit_vex(x, V_66, M_3A, ei[k].w, 0, 0, ei[k].op, o[1].reg, &o[2]) < 0)
                        return -1;
                }
                imm_put(x, o[0].imm, 1);
                return 0;
            }
        if (!strcmp(mn, "vpextrw") && n == 3 && o[0].k == K_IMM && is_v(&o[1])) {
            if (o[2].k == K_REG) {
                if (emit_vex(x, V_66, M_0F, 0, 0, 0, 0xc5, o[2].reg, &o[1]) < 0) return -1;
            } else if (o[2].k == K_MEM) {
                if (emit_vex(x, V_66, M_3A, 0, 0, 0, 0x15, o[1].reg, &o[2]) < 0) return -1;
            } else return fail("vpextrw stores to a register or memory");
            imm_put(x, o[0].imm, 1);
            return 0;
        }
        if (!strcmp(mn, "vpinsrw") && n == 4 && o[0].k == K_IMM &&
            (o[1].k == K_REG || o[1].k == K_MEM) && is_v(&o[2]) && is_v(&o[3])) {
            if (emit_vex(x, V_66, M_0F, 0, 0, o[2].reg, 0xc4, o[3].reg, &o[1]) < 0) return -1;
            imm_put(x, o[0].imm, 1);
            return 0;
        }
    }
    if (!strcmp(mn, "vzeroall") && n == 0) { put(x, 0xc5); put(x, 0xfc); put(x, 0x77); return 0; }
    if (!strcmp(mn, "vldmxcsr") && n == 1 && o[0].k == K_MEM)
        return emit_vex(x, 0, M_0F, 0, 0, 0, 0xae, 2, &o[0]);
    if (!strcmp(mn, "vstmxcsr") && n == 1 && o[0].k == K_MEM)
        return emit_vex(x, 0, M_0F, 0, 0, 0, 0xae, 3, &o[0]);
    return 1;
}

/* BMI1, BMI2 and MOVBE: general registers, VEX for all but movbe; the
 * size from the suffix or the registers */
static int bmi(struct ib *x, const char *mn, const struct opnd *o, int n)
{
    /* reg = dst, rm and vvvv as each one has them */
    static const struct { const char *n; int pp, map; unsigned op; char kind; int ext; } bm[] = {
        {"andn",0,M_38,0xf2,'A',0},   /* andn rm, vvvv, reg */
        {"bextr",0,M_38,0xf7,'B',0},  /* bextr vvvv, rm, reg */
        {"bzhi",0,M_38,0xf5,'B',0},
        {"sarx",V_F3,M_38,0xf7,'B',0},{"shlx",V_66,M_38,0xf7,'B',0},{"shrx",V_F2,M_38,0xf7,'B',0},
        {"pdep",V_F2,M_38,0xf5,'A',0},{"pext",V_F3,M_38,0xf5,'A',0},
        {"mulx",V_F2,M_38,0xf6,'A',0},
        {"blsr",0,M_38,0xf3,'V',1},{"blsmsk",0,M_38,0xf3,'V',2},{"blsi",0,M_38,0xf3,'V',3},
        {"rorx",V_F2,M_3A,0xf0,'R',0},
    };
    for (unsigned k = 0; k < sizeof bm / sizeof *bm; k++) {
        int s = sized(mn, bm[k].n);
        if (s < 0) continue;
        if (s && s != 4 && s != 8) return fail("'%s' is 32 or 64 bits", mn);
        int nn = bm[k].kind == 'V' ? 2 : 3;
        if (n != nn) return fail("'%s' takes %d operands", mn, nn);
        const struct opnd *d = &o[n - 1];
        if (d->k != K_REG || (d->size != 4 && d->size != 8))
            return fail("'%s' writes a 32- or 64-bit register", mn);
        int sz = s ? s : d->size, w = sz == 8;
        if (d->size != sz) return fail("'%s': the registers are %d-bit", mn, sz * 8);
        switch (bm[k].kind) {
        case 'A':   /* rm, vvvv, reg */
            if (o[1].k != K_REG || o[1].size != sz || (o[0].k != K_REG && o[0].k != K_MEM) ||
                (o[0].k == K_REG && o[0].size != sz))
                return fail("'%s': these operands", mn);
            return emit_vex(x, bm[k].pp, bm[k].map, w, 0, o[1].reg, bm[k].op, d->reg, &o[0]);
        case 'B':   /* vvvv, rm, reg */
            if (o[0].k != K_REG || o[0].size != sz || (o[1].k != K_REG && o[1].k != K_MEM) ||
                (o[1].k == K_REG && o[1].size != sz))
                return fail("'%s': these operands", mn);
            return emit_vex(x, bm[k].pp, bm[k].map, w, 0, o[0].reg, bm[k].op, d->reg, &o[1]);
        case 'V':   /* rm -> vvvv */
            if ((o[0].k != K_REG && o[0].k != K_MEM) || (o[0].k == K_REG && o[0].size != sz))
                return fail("'%s': these operands", mn);
            return emit_vex(x, bm[k].pp, bm[k].map, w, 0, d->reg, bm[k].op, bm[k].ext, &o[0]);
        case 'R':   /* rorx imm, rm, reg */
            if (o[0].k != K_IMM || (o[1].k != K_REG && o[1].k != K_MEM) ||
                (o[1].k == K_REG && o[1].size != sz))
                return fail("'%s': these operands", mn);
            if (emit_vex(x, bm[k].pp, bm[k].map, w, 0, 0, bm[k].op, d->reg, &o[1]) < 0) return -1;
            imm_put(x, o[0].imm, 1);
            return 0;
        }
    }
    {
        int s = sized(mn, "movbe");
        if (s >= 0 && n == 2) {
            int load = o[0].k == K_MEM && o[1].k == K_REG;
            const struct opnd *r = load ? &o[1] : &o[0], *m = load ? &o[0] : &o[1];
            if (r->k != K_REG || m->k != K_MEM || r->size == 1 || (s && s != r->size))
                return fail("movbe moves between a 16-, 32- or 64-bit register and memory");
            unsigned char op[3] = { 0x0f, 0x38, (unsigned char)(load ? 0xf0 : 0xf1) };
            return emit_rm(x, pfx_of(r->size), r->size == 8, op, 3, r->reg, NULL, m);
        }
    }
    return 1;
}

static int insn(struct ib *x, const char *mn, struct opnd *o, int n)
{
    int s;
    /* AVX-512: what only EVEX encodes goes nowhere else (a VEX or legacy
     * encoder would take xmm17 for xmm1); an AVX-512 mnemonic with xmm
     * operands, or a VEX one's AVX-512 form, after the VEX encoder
     * declines it; the opmask instructions */
    if (ev_only(o, n) || mn[0] == 'k') {
        int r = evex(x, mn, o, n);
        if (r == 1)
            return fail("'%s' with these operands is not an AVX-512 form this "
                        "assembler knows", mn);
        return r;
    }
    if (mn[0] == 'v') {
        char keep[256];
        int at = x->n, r;
        snprintf(keep, sizeof keep, "%s", g_err ? g_err : "");
        r = avx(x, mn, o, n);
        if (r == 0) return 0;
        x->n = at;
        {
            char err1[256];
            snprintf(err1, sizeof err1, "%s", g_err ? g_err : "");
            int e = evex(x, mn, o, n);
            if (e == 0) {
                if (g_err) snprintf(g_err, (size_t)g_errlen, "%s", keep);
                return 0;
            }
            x->n = at;
            if (r < 0) {
                if (g_err) snprintf(g_err, (size_t)g_errlen, "%s", err1);
                return -1;
            }
            if (e < 0) return -1;
        }
    }
    {
        int r = bmi(x, mn, o, n);
        if (r != 1) return r;
    }
    if (mn[0] == 'f') {
        int r = x87(x, mn, o, n);
        if (r != 1) return r;
    }
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
    /* a string instruction with its operands written: `movsq (%rsi),
     * %es:(%rdi)`, the implicit ones named -- a segment override other
     * than the source's default is the only thing they can add */
    if ((n == 1 || n == 2) && (!strncmp(mn, "movs", 4) || !strncmp(mn, "stos", 4) ||
                               !strncmp(mn, "lods", 4) || !strncmp(mn, "cmps", 4) ||
                               !strncmp(mn, "scas", 4)) && strlen(mn) == 5 &&
        strchr("bwlq", mn[4])) {
        int seg = 0;
        for (int k = 0; k < n; k++) {
            if (o[k].k == K_REG) continue;          /* stos %rax, (%rdi) */
            if (o[k].k != K_MEM || o[k].index >= 0 || o[k].disp ||
                (o[k].base != 6 && o[k].base != 7))
                return fail("'%s' operates on (%%rsi) and %%es:(%%rdi)", mn);
            if (o[k].base == 6 && o[k].seg && o[k].seg != 0x3e) seg = o[k].seg;
        }
        for (unsigned k = 0; k < sizeof plain / sizeof *plain; k++)
            if (!strcmp(mn, plain[k].n)) {
                if (seg) put(x, (unsigned)seg);
                for (int i = 0; i < plain[k].len; i++)
                    put(x, plain[k].b[i]);
                return 0;
            }
    }
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
        if (g_level < 1 && (fits8(v - 2) || g_level == -2)) {
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
        if (g_level < 1 && (fits8(v - 2) || g_level == -2)) {
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
            unsigned char op = imm8_ok(v) ? 0x6b : 0x69;
            if (emit_rm(x, pfx_of(sz), sz == 8, &op, 1, dst->reg, dst, src) < 0) return -1;
            imm_put(x, v, imm8_ok(v) ? 1 : sz == 2 ? 2 : 4);
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
            if (imm8_ok(o[0].imm)) { put(x, 0x6a); imm_put(x, o[0].imm, 1); }
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
        char c[32];                     /* (the mnemonic's size) */
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
        char c[32];                     /* (the mnemonic's size) */
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

    /* ---- SSE ---- (legacy encodings: xmm only; a ymm is AVX's, above) */
    for (int k = 0; k < n && mn[0] != 'v'; k++)
        if (o[k].k == K_XMM && o[k].size == 32)
            return fail("'%s' with a ymm register is not an instruction: AVX's is "
                        "v%s", mn, mn);
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
                (o[0].k == K_XMM || (o[0].k == K_MEM && strcmp(mn, "movhlps") &&
                                     strcmp(mn, "movlhps")))) {
                unsigned char op[2] = { 0x0f, (unsigned char)sl[k].op };
                return emit_rm(x, sl[k].pfx, 0, op, 2, o[1].reg, NULL, &o[0]);
            }
        /* ---- the rest of SSE through SSE4.2, AES, SHA, CLMUL --------------
         * map 0 is 0F, 1 0F 38, 2 0F 3A (whose forms take an immediate) */
        static const struct { const char *n; int pfx, map; unsigned op; } sx[] = {
            {"rcpps",0,0,0x53},{"rcpss",PF3,0,0x53},{"rsqrtps",0,0,0x52},{"rsqrtss",PF3,0,0x52},
            {"packsswb",P66,0,0x63},{"packssdw",P66,0,0x6b},{"packuswb",P66,0,0x67},
            {"paddsb",P66,0,0xec},{"paddsw",P66,0,0xed},{"paddusb",P66,0,0xdc},
            {"paddusw",P66,0,0xdd},{"psubsb",P66,0,0xe8},{"psubsw",P66,0,0xe9},
            {"psubusb",P66,0,0xd8},{"psubusw",P66,0,0xd9},{"pmaddwd",P66,0,0xf5},
            {"pmulhw",P66,0,0xe5},{"pmulhuw",P66,0,0xe4},{"pminsw",P66,0,0xea},
            {"pmaxsw",P66,0,0xee},{"pavgw",P66,0,0xe3},
            {"psllw",P66,0,0xf1},{"pslld",P66,0,0xf2},{"psllq",P66,0,0xf3},
            {"psrlw",P66,0,0xd1},{"psrld",P66,0,0xd2},{"psrlq",P66,0,0xd3},
            {"psraw",P66,0,0xe1},{"psrad",P66,0,0xe2},
            {"cvtdq2pd",PF3,0,0xe6},{"cvtpd2dq",PF2,0,0xe6},{"cvttpd2dq",P66,0,0xe6},
            {"addsubps",PF2,0,0xd0},{"addsubpd",P66,0,0xd0},{"haddps",PF2,0,0x7c},
            {"haddpd",P66,0,0x7c},{"hsubps",PF2,0,0x7d},{"hsubpd",P66,0,0x7d},
            {"movshdup",PF3,0,0x16},{"movsldup",PF3,0,0x12},{"movddup",PF2,0,0x12},
            {"pshufb",P66,1,0x00},{"phaddw",P66,1,0x01},{"phaddd",P66,1,0x02},
            {"phaddsw",P66,1,0x03},{"pmaddubsw",P66,1,0x04},{"phsubw",P66,1,0x05},
            {"phsubd",P66,1,0x06},{"phsubsw",P66,1,0x07},{"psignb",P66,1,0x08},
            {"psignw",P66,1,0x09},{"psignd",P66,1,0x0a},{"pmulhrsw",P66,1,0x0b},
            {"pabsb",P66,1,0x1c},{"pabsw",P66,1,0x1d},{"pabsd",P66,1,0x1e},
            {"ptest",P66,1,0x17},
            {"pmovsxbw",P66,1,0x20},{"pmovsxbd",P66,1,0x21},{"pmovsxbq",P66,1,0x22},
            {"pmovsxwd",P66,1,0x23},{"pmovsxwq",P66,1,0x24},{"pmovsxdq",P66,1,0x25},
            {"pmuldq",P66,1,0x28},{"pcmpeqq",P66,1,0x29},{"packusdw",P66,1,0x2b},
            {"pmovzxbw",P66,1,0x30},{"pmovzxbd",P66,1,0x31},{"pmovzxbq",P66,1,0x32},
            {"pmovzxwd",P66,1,0x33},{"pmovzxwq",P66,1,0x34},{"pmovzxdq",P66,1,0x35},
            {"pcmpgtq",P66,1,0x37},{"pminsb",P66,1,0x38},{"pminsd",P66,1,0x39},
            {"pminuw",P66,1,0x3a},{"pminud",P66,1,0x3b},{"pmaxsb",P66,1,0x3c},
            {"pmaxsd",P66,1,0x3d},{"pmaxuw",P66,1,0x3e},{"pmaxud",P66,1,0x3f},
            {"pmulld",P66,1,0x40},{"phminposuw",P66,1,0x41},
            {"aesimc",P66,1,0xdb},{"aesenc",P66,1,0xdc},{"aesenclast",P66,1,0xdd},
            {"aesdec",P66,1,0xde},{"aesdeclast",P66,1,0xdf},
            {"sha1nexte",0,1,0xc8},{"sha1msg1",0,1,0xc9},{"sha1msg2",0,1,0xca},
            {"sha256msg1",0,1,0xcc},{"sha256msg2",0,1,0xcd},
            {"roundps",P66,2,0x08},{"roundpd",P66,2,0x09},{"roundss",P66,2,0x0a},
            {"roundsd",P66,2,0x0b},{"blendps",P66,2,0x0c},{"blendpd",P66,2,0x0d},
            {"pblendw",P66,2,0x0e},{"palignr",P66,2,0x0f},{"insertps",P66,2,0x21},
            {"dpps",P66,2,0x40},{"dppd",P66,2,0x41},{"mpsadbw",P66,2,0x42},
            {"pclmulqdq",P66,2,0x44},{"pcmpestrm",P66,2,0x60},{"pcmpestri",P66,2,0x61},
            {"pcmpistrm",P66,2,0x62},{"pcmpistri",P66,2,0x63},
            {"aeskeygenassist",P66,2,0xdf},{"sha1rnds4",0,2,0xcc},
        };
        for (unsigned k = 0; k < sizeof sx / sizeof *sx; k++) {
            if (strcmp(mn, sx[k].n)) continue;
            int imm = sx[k].map == 2, at = imm ? 1 : 0;
            if (n != 2 + imm || (imm && o[0].k != K_IMM) || o[at + 1].k != K_XMM ||
                (o[at].k != K_XMM && o[at].k != K_MEM))
                continue;   /* (psllw $n, %xmm1 is below: the shifts by a constant) */
            unsigned char op[3] = { 0x0f, sx[k].map == 1 ? 0x38 : 0x3a, (unsigned char)sx[k].op };
            if (sx[k].map == 0) op[1] = (unsigned char)sx[k].op;
            if (emit_rm(x, sx[k].pfx, 0, op, sx[k].map ? 3 : 2, o[at + 1].reg, NULL, &o[at]) < 0)
                return -1;
            if (imm) imm_put(x, o[0].imm, 1);
            return 0;
        }
        /* the variable blends and sha256rnds2: %xmm0 implicit, written or not */
        static const struct { const char *n; int pfx; unsigned op; } sv[] = {
            {"pblendvb",P66,0x10},{"blendvps",P66,0x14},{"blendvpd",P66,0x15},
            {"sha256rnds2",0,0xcb},
        };
        for (unsigned k = 0; k < sizeof sv / sizeof *sv; k++)
            if (!strcmp(mn, sv[k].n)) {
                int at = n == 3 ? 1 : 0;
                if ((n != 2 && n != 3) || (n == 3 && (o[0].k != K_XMM || o[0].reg != 0)) ||
                    o[at + 1].k != K_XMM || (o[at].k != K_XMM && o[at].k != K_MEM))
                    return fail("'%s' takes [%%xmm0,] xmm/memory and xmm", mn);
                unsigned char op[3] = { 0x0f, 0x38, (unsigned char)sv[k].op };
                return emit_rm(x, sv[k].pfx, 0, op, 3, o[at + 1].reg, NULL, &o[at]);
            }
        /* loads and stores with one form each */
        if (!strcmp(mn, "lddqu") && n == 2 && o[0].k == K_MEM && o[1].k == K_XMM) {
            unsigned char op[2] = { 0x0f, 0xf0 };
            return emit_rm(x, PF2, 0, op, 2, o[1].reg, NULL, &o[0]);
        }
        if (!strcmp(mn, "movntdqa") && n == 2 && o[0].k == K_MEM && o[1].k == K_XMM) {
            unsigned char op[3] = { 0x0f, 0x38, 0x2a };
            return emit_rm(x, P66, 0, op, 3, o[1].reg, NULL, &o[0]);
        }
        if (!strcmp(mn, "maskmovdqu") && n == 2 && o[0].k == K_XMM && o[1].k == K_XMM) {
            unsigned char op[2] = { 0x0f, 0xf7 };
            return emit_rm(x, P66, 0, op, 2, o[1].reg, NULL, &o[0]);
        }
        {
            static const struct { const char *n; int pfx; unsigned op; } lh[] = {
                {"movlps",0,0x12},{"movhps",0,0x16},{"movlpd",P66,0x12},{"movhpd",P66,0x16},
            };
            for (unsigned k = 0; k < 4; k++)
                if (!strcmp(mn, lh[k].n) && n == 2) {
                    unsigned char op[2] = { 0x0f, (unsigned char)lh[k].op };
                    if (o[0].k == K_MEM && o[1].k == K_XMM)
                        return emit_rm(x, lh[k].pfx, 0, op, 2, o[1].reg, NULL, &o[0]);
                    op[1]++;
                    if (o[0].k == K_XMM && o[1].k == K_MEM)
                        return emit_rm(x, lh[k].pfx, 0, op, 2, o[0].reg, NULL, &o[1]);
                    return fail("'%s' moves between memory and xmm", mn);
                }
        }
        /* cmp<pred>{ss,sd,ps,pd}: cmp with the predicate as the immediate */
        if (!strncmp(mn, "cmp", 3) && strlen(mn) > 5) {
            static const char *const pr[] = { "eq", "lt", "le", "unord", "neq", "nlt", "nle", "ord" };
            size_t ml = strlen(mn);
            const char *ty = mn + ml - 2;
            int pfx = !strcmp(ty, "ss") ? PF3 : !strcmp(ty, "sd") ? PF2
                    : !strcmp(ty, "ps") ? 0 : !strcmp(ty, "pd") ? P66 : -1;
            for (int p = 0; pfx >= 0 && p < 8; p++)
                if (ml - 5 == strlen(pr[p]) && !strncmp(mn + 3, pr[p], ml - 5)) {
                    if (n != 2 || o[1].k != K_XMM || (o[0].k != K_XMM && o[0].k != K_MEM))
                        return fail("'%s' takes xmm/memory and xmm", mn);
                    unsigned char op[2] = { 0x0f, 0xc2 };
                    if (emit_rm(x, pfx, 0, op, 2, o[1].reg, NULL, &o[0]) < 0) return -1;
                    imm_put(x, p, 1);
                    return 0;
                }
        }
        /* extract to and insert from a general register or memory */
        {
            static const struct { const char *n; int ins, w; unsigned op; } ei[] = {
                {"pextrb",0,0,0x14},{"pextrd",0,0,0x16},{"pextrq",0,1,0x16},
                {"extractps",0,0,0x17},{"pinsrb",1,0,0x20},{"pinsrd",1,0,0x22},
                {"pinsrq",1,1,0x22},
            };
            for (unsigned k = 0; k < sizeof ei / sizeof *ei; k++)
                if (!strcmp(mn, ei[k].n)) {
                    const struct opnd *xr = ei[k].ins ? &o[2] : &o[1];
                    const struct opnd *g = ei[k].ins ? &o[1] : &o[2];
                    if (n != 3 || o[0].k != K_IMM || xr->k != K_XMM ||
                        (g->k != K_REG && g->k != K_MEM))
                        return fail("'%s' takes an immediate, %s", mn,
                                    ei[k].ins ? "a register or memory, and xmm"
                                              : "xmm, and a register or memory");
                    unsigned char op[3] = { 0x0f, 0x3a, (unsigned char)ei[k].op };
                    if (emit_rm(x, P66, ei[k].w, op, 3, xr->reg, NULL, g) < 0) return -1;
                    imm_put(x, o[0].imm, 1);
                    return 0;
                }
            if (!strcmp(mn, "pextrw") && n == 3 && o[0].k == K_IMM && o[1].k == K_XMM) {
                if (o[2].k == K_REG) {                    /* 0F C5: the GPR in reg */
                    unsigned char op[2] = { 0x0f, 0xc5 };
                    if (emit_rm(x, P66, 0, op, 2, o[2].reg, NULL, &o[1]) < 0) return -1;
                } else if (o[2].k == K_MEM) {
                    unsigned char op[3] = { 0x0f, 0x3a, 0x15 };
                    if (emit_rm(x, P66, 0, op, 3, o[1].reg, NULL, &o[2]) < 0) return -1;
                } else return fail("pextrw stores to a register or memory");
                imm_put(x, o[0].imm, 1);
                return 0;
            }
            if (!strcmp(mn, "pinsrw") && n == 3 && o[0].k == K_IMM && o[2].k == K_XMM &&
                (o[1].k == K_REG || o[1].k == K_MEM)) {
                unsigned char op[2] = { 0x0f, 0xc4 };
                if (emit_rm(x, P66, 0, op, 2, o[2].reg, NULL, &o[1]) < 0) return -1;
                imm_put(x, o[0].imm, 1);
                return 0;
            }
        }
        /* crc32{b,w,l,q} r/m, r32/r64 */
        if (!strncmp(mn, "crc32", 5) && n == 2 && o[1].k == K_REG &&
            (o[0].k == K_REG || o[0].k == K_MEM)) {
            char c = mn[5];
            int sz = c == 'b' ? 1 : c == 'w' ? 2 : c == 'l' ? 4 : c == 'q' ? 8
                   : o[0].k == K_REG ? o[0].size : 0;
            if (!sz || (mn[5] && mn[6])) return fail("crc32 takes a b, w, l or q suffix");
            if (o[0].k == K_REG && o[0].size != sz)
                return fail("crc32%c: the source is a %d-bit register", c ? c : ' ', sz * 8);
            if (o[1].size != (sz == 8 ? 8 : 4) && !(sz == 1 && o[1].size == 8))
                return fail("crc32: the destination is %s", sz == 8 ? "a 64-bit register"
                            : "a 32-bit register");
            unsigned char op[3] = { 0x0f, 0x38, (unsigned char)(sz == 1 ? 0xf0 : 0xf1) };
            return emit_rm(x, pfx_of(sz) | PF2, o[1].size == 8, op, 3, o[1].reg, NULL, &o[0]);
        }
        /* the cache and prefetch hints */
        {
            static const struct { const char *n; int pfx; unsigned op; int ext; } ch[] = {
                {"prefetchnta",0,0x18,0},{"prefetcht0",0,0x18,1},{"prefetcht1",0,0x18,2},
                {"prefetcht2",0,0x18,3},{"prefetchw",0,0x0d,1},{"prefetch",0,0x0d,0},
                {"clflushopt",P66,0xae,7},{"clwb",P66,0xae,6},
            };
            for (unsigned k = 0; k < sizeof ch / sizeof *ch; k++)
                if (!strcmp(mn, ch[k].n) && n == 1 && o[0].k == K_MEM) {
                    unsigned char op[2] = { 0x0f, (unsigned char)ch[k].op };
                    return emit_rm(x, ch[k].pfx, 0, op, 2, ch[k].ext, NULL, &o[0]);
                }
            if ((s = sized(mn, "movnti")) >= 0 && n == 2 && o[0].k == K_REG && o[1].k == K_MEM) {
                unsigned char op[2] = { 0x0f, 0xc3 };
                return emit_rm(x, 0, o[0].size == 8, op, 2, o[0].reg, NULL, &o[1]);
            }
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
    g_disp_force = 0;
    g_imm_wide = 0;
    for (;;) {
        while (i < len && isspace((unsigned char)s[i])) i++;
        int st = i;
        while (i < len && !isspace((unsigned char)s[i]) && s[i] != ';')
            i++;
        int ml = i - st;
        if (ml == 0) {
            /* prefixes alone, as a statement of their own (`rep;movsq`) */
            for (int k = 0; k < x.n; k++)
                code_byte(out, x.b[k]);
            g_last_len = x.n;
            return 0;
        }
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
        if (!strcmp(mn, "{disp32}")) { g_disp_force = 32; continue; }
        if (!strcmp(mn, "{disp8}")) { g_disp_force = 8; continue; }
        if (!strcmp(mn, "{imm32}")) { g_imm_wide = 1; continue; }
        break;
    }
    struct opnd o[6];          /* (AVX-512's: $imm, {sae}, and three registers) */
    int n = 0, depth = 0, st = i;
    while (i < len && isspace((unsigned char)s[i])) i++;
    st = i;
    if (i < len) {
        for (int k = i; k <= len; k++) {
            if (k < len && s[k] == '(') depth++;
            if (k < len && s[k] == ')') depth--;
            if (k == len || (s[k] == ',' && depth == 0)) {
                if (n == 6) return fail("too many operands");
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
    g_disp_force = 0;
    g_imm_wide = 0;
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
    if (text[0] == '\001') {      /* x86att_symform's refusal: its reason */
        snprintf(err, (size_t)errlen, "%s", text + 1);
        return -1;
    }
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

/* the next symbol in the operands from q (a register's name is not one),
 * with any @PLT / @GOTPCREL and +N after it: 0 when there is none */
struct x86sym { const char *at; int len, kind, dollar; long long addend; const char *end; };

static int x86_next_sym(const char *stmt, const char *q, struct x86sym *y)
{
    while (*q) {
        if (*q == '%') {                 /* a register: skip it */
            q++;
            while (asm_symc((unsigned char)*q)) q++;
            continue;
        }
        if (*q == '{') {                 /* AVX-512's decorations: {z}, {1to8} */
            while (*q && *q != '}') q++;
            continue;
        }
        if (asm_sym0((unsigned char)*q) && !(*q == '.' && (q[1] == '+' || q[1] == '-' || !asm_symc((unsigned char)q[1])))) {
            const char *b = q;
            while (asm_symc((unsigned char)*q)) q++;
            y->at = b;
            y->len = (int)(q - b);
            y->dollar = b > stmt && b[-1] == '$';
            y->kind = 0;
            const char *a = q;
            if (*a == '@') {
                if (!strncmp(a + 1, "PLT", 3)) { y->kind = 1; a += 4; }
                else if (!strncmp(a + 1, "GOTPCREL", 8)) { y->kind = 2; a += 9; }
                else return -1;
            }
            y->addend = 0;
            y->end = a;
            if (*a == '+' || *a == '-') {
                const char *ae = a + 1;
                while (*ae && (isalnum((unsigned char)*ae) || *ae == 'x')) ae++;
                if (!asm_const_expr(a, (int)(ae - a), &y->addend))
                    return -1;
                y->end = ae;
            }
            return 1;
        }
        if (isdigit((unsigned char)*q)) {
            while (asm_symc((unsigned char)*q)) q++;
            continue;
        }
        q++;
    }
    return 0;
}

/* `call sym`, `jmp sym`, `jcc sym`, `sym(%rip)`, `$sym`, an absolute
 * `sym(%reg)`: the statement with a zero there, and where its relocation
 * goes -- and a second symbol, when an immediate and an address both name
 * one (`movq $a, b+8(%rip)`) */
int x86att_symform(const char *stmt, struct asm_symform *f)
{
    memset(f, 0, sizeof *f);
    const char *p = after_prefixes(stmt);
    const char *m = p;
    while (*p && !isspace((unsigned char)*p)) p++;
    int ml = (int)(p - m);
    struct x86sym y[2];
    int ns = 0;
    {
        int r = x86_next_sym(stmt, p, &y[0]);
        if (r <= 0) return 0;
        ns = 1;
        r = x86_next_sym(stmt, y[0].end, &y[1]);
        if (r < 0) return 0;
        if (r > 0) ns = 2;
    }
    f->sym_at = (int)(y[0].at - stmt);
    f->sym_len = y[0].len;
    char mn[16];
    snprintf(mn, sizeof mn, "%.*s", ml < 15 ? ml : 15, m);
    /* a direct transfer: `jmp *sym(,%rax,8)` is an indirect one, through
     * memory, and its symbol an address like any other */
    int indirect = 0;
    for (const char *k = p; k < y[0].at; k++)
        if (*k == '*') indirect = 1;
    int transfer = !indirect && ns == 1 &&
                   (!strcmp(mn, "call") || !strcmp(mn, "callq") ||
                    !strcmp(mn, "jmp") || !strcmp(mn, "jmpq") ||
                    (mn[0] == 'j' && cond_of(mn + 1) >= 0));
    int jcc = transfer && mn[0] == 'j' && strncmp(mn, "jmp", 3);
    /* the statement with each symbol 0 (a transfer's: the near form's
     * length, the displacement from the end), and {disp32} so an
     * address keeps its field whatever the 0 would let it shrink to */
    {
        char *o = f->encode;
        size_t cap = sizeof f->encode, n = 0;
        int abs_mem = 0, imm = 0;
        for (int k = 0; k < ns; k++) {
            if (!y[k].dollar && strncmp(y[k].end, "(%rip)", 6) && !transfer)
                abs_mem = 1;
            if (y[k].dollar)
                imm = 1;
        }
        n += (size_t)snprintf(o + n, cap - n, "%s%s%s", transfer ? "near " : "",
                              abs_mem ? "{disp32} " : "", imm ? "{imm32} " : "");
        const char *from = stmt;
        for (int k = 0; k < ns && n < cap; k++) {
            n += (size_t)snprintf(o + n, cap - n, "%.*s%s", (int)(y[k].at - from),
                                  from, transfer ? (jcc ? ".+6" : ".+5") : "0");
            from = y[k].end;
        }
        if (n < cap) snprintf(o + n, cap - n, "%s", from);
    }
    /* where the fields land: encode it */
    char err[256];
    struct code c = { 0 };
    g_err = err; g_errlen = (int)sizeof err;
    if (x86att_assemble(f->encode, &c, err, (int)sizeof err) != 0) {
        free(c.p);
        snprintf(f->encode, sizeof f->encode, "\001%.*s",
                 (int)sizeof f->encode - 2, err);
        return 1;
    }
    unsigned char c_first[16] = { 0 };
    for (int k = 0; k < c.len && k < 16; k++)
        c_first[k] = c.p[k];
    free(c.p);
    if (transfer) {
        f->nsites = 1;
        f->site[0].off = g_last_len - 4;
        /* a call or a jump to a function: PLT32, as GNU as writes it; a
         * conditional jump: PC32 */
        f->site[0].reloc = mn[0] == 'c' || !strncmp(mn, "jmp", 3)
                               ? R_X86_64_PLT32 : R_X86_64_PC32;
        f->addend = y[0].addend - 4;
        return 1;
    }
    for (int k = 0; k < ns; k++) {
        struct asm_symsite *st = &f->site[k];
        long long ad = y[k].addend;
        if (!y[k].dollar && !strncmp(y[k].end, "(%rip)", 6)) {
            if (g_last_disp_at < 0) return 0;
            st->off = g_last_disp_at;
            /* GOTPCREL as the relaxable GOTPCRELX llvm-mc and GNU as write
             * (REX_ when the instruction has a REX prefix) */
            int has_rex = 0;
            for (int j = 0; j < g_last_disp_at - 1 && j < g_last_len; j++)
                if ((c_first[j] & 0xf0) == 0x40)
                    has_rex = 1;
            st->reloc = y[k].kind == 2 ? (has_rex ? R_X86_64_REX_GOTPCRELX : R_X86_64_GOTPCRELX)
                                       : R_X86_64_PC32;
            ad -= g_last_len - g_last_disp_at;
        } else if (y[k].dollar) {
            if (g_last_imm_at < 0) return 0;
            int ilen = g_last_len - g_last_imm_at;
            /* (an immediate before a rip-relative address: the field runs
             * to the end, so its length is the encoding's tail) */
            if (g_last_disp_at > g_last_imm_at) ilen = g_last_disp_at - g_last_imm_at;
            st->off = g_last_imm_at;
            st->reloc = ilen == 8 ? R_X86_64_64
                      : (!strncmp(mn, "movq", 4) || mn[strlen(mn) - 1] == 'q') ? R_X86_64_32S
                      : R_X86_64_32;
        } else {
            if (g_last_disp_at < 0) return 0;      /* an absolute address */
            st->off = g_last_disp_at;
            st->reloc = R_X86_64_32S;
        }
        if (k == 0) f->addend = (long)ad;
        else {
            f->sym2_at = (int)(y[1].at - stmt);
            f->sym2_len = y[1].len;
            f->addend2 = (long)ad;
        }
    }
    f->nsites = ns;
    return 1;
}
