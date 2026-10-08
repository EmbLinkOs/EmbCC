/* TriCore inline-asm vocabulary: the instructions an asm statement's
 * template may hold, assembled through src/arch/tricore/emit.c -- the same
 * encoder the code generator uses and tools/tricorecheck referees -- so a
 * template cannot encode an instruction differently from compiled code.
 *
 * Statements are separated by newlines or `;`. Registers are d0-d15,
 * a0-a15 (sp is a10) and the pairs e0-e14, with or without a leading `%`.
 * A memory operand is `[aN]` or `[aN]offset`. Constants are decimal or
 * 0x hex, optionally after `#`; a core special function register may be
 * written by its offset or by name (psw, pcxi, fcx, lcx, btv, biv, isp,
 * syscon). What is here:
 *
 *   nop debug isync dsync svlcx rslcx enable disable rfe ret
 *   mtcr CSFR, dA        mfcr dC, CSFR        syscall N
 *   mov dC, dB|K         mov.u dC, K          movh dC, K
 *   mov.a aC, dB         mov.d dC, aB         mov.aa aC, aB
 *   movh.a aC, K         lea aC, [aB]off      addih.a aC, aA, K
 *   add sub and or xor nor andn mul sh sha  dC, dA, dB|K
 *   eq ne lt lt.u ge ge.u min max          dC, dA, dB|K
 *   addi dC, dA, K       addih dC, dA, K
 *   ld.b ld.bu ld.h ld.hu ld.w  dC, [aB]off    ld.a aC, [aB]off
 *   st.b st.h st.w  [aB]off, dA                 st.a [aB]off, aA
 *   swap.w [aB]off, dA   cmpswap.w [aB]off, eA
 *   ji aA  calli aA  jli aA
 *   j jl call  TARGET                         (+-16 MiB)
 *   jeq jne jlt jlt.u jge jge.u  dA, dB|K4, TARGET
 *   jz jnz dA, TARGET    jeq.a jne.a aA, aB, TARGET
 *   jz.a jnz.a aA, TARGET                     loop aB, TARGET
 *                                             (+-32 KiB)
 *
 * A TARGET is `.+N` / `.-N`, the distance in bytes from the instruction --
 * what src/as/gas.c writes for a label -- or, in a template, a numeric
 * label of its own (`1:`, `1b`, `1f`). In a .s/.S file or a file-scope
 * block, tcasm_symform gives j, jl and call a symbol (R_TRICORE_24REL),
 * and movh, movh.a, addi, lea and the BOL loads and stores an address's
 * halves: `hi:sym`/`%hi(sym)` (R_TRICORE_HIADJ) and `lo:sym`/`%lo(sym)`
 * (R_TRICORE_LO, or LO2 in a [aB]offset).
 *
 * A form outside this list is refused by name, never guessed at. */
#include "asm.h"
#include "emit.h"
#include "../asmexpr.h"
#include "../../elf/elf.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct tok { const char *p; int n; };

static int teq(struct tok t, const char *s)
{
    return (int)strlen(s) == t.n && strncmp(t.p, s, (size_t)t.n) == 0;
}

/* A register by name: 'd', 'a' or 'e' in *file, the number returned, or
 * -1. */
int tcasm_reg(const char *p, int n, int *file)
{
    int r = 0, k;
    if (n > 0 && *p == '%') { p++; n--; }
    if (n == 2 && !strncmp(p, "sp", 2)) { *file = 'a'; return 10; }
    if (n < 2 || n > 3 || (p[0] != 'd' && p[0] != 'a' && p[0] != 'e'))
        return -1;
    for (k = 1; k < n; k++) {
        if (!isdigit((unsigned char)p[k]))
            return -1;
        r = r * 10 + (p[k] - '0');
    }
    if (r > 15 || (p[0] == 'e' && (r & 1)))
        return -1;
    *file = p[0];
    return r;
}

static const struct { const char *nm; unsigned v; } csfrs[] = {
    { "pcxi", TC_CSFR_PCXI }, { "psw", TC_CSFR_PSW },
    { "syscon", TC_CSFR_SYSCON }, { "biv", TC_CSFR_BIV },
    { "btv", TC_CSFR_BTV }, { "isp", TC_CSFR_ISP },
    { "fcx", TC_CSFR_FCX }, { "lcx", TC_CSFR_LCX }
};

/* The operands of one statement, split at top-level commas. */
#define MAXOPS 4
struct stmt { struct tok mn; struct tok op[MAXOPS]; int nop; };

static struct tok trim(const char *p, const char *e)
{
    struct tok t;
    while (p < e && isspace((unsigned char)*p)) p++;
    while (e > p && isspace((unsigned char)e[-1])) e--;
    t.p = p; t.n = (int)(e - p);
    return t;
}

static int number(struct tok t, long long *v)
{
    char buf[64], *end;
    int neg = 0, i = 0;
    if (t.n > 0 && *t.p == '#') { t.p++; t.n--; }
    if (t.n <= 0 || t.n >= (int)sizeof buf)
        return 0;
    memcpy(buf, t.p, (size_t)t.n);
    buf[t.n] = 0;
    if (buf[0] == '-') { neg = 1; i = 1; }
    else if (buf[0] == '+') i = 1;
    if (!isdigit((unsigned char)buf[i]))
        return 0;
    *v = (long long)strtoull(buf + i, &end, 0);
    if (*end)
        return 0;
    if (neg) *v = -*v;
    return 1;
}

static int csfr(struct tok t, long long *v)
{
    for (unsigned k = 0; k < sizeof csfrs / sizeof csfrs[0]; k++)
        if (teq(t, csfrs[k].nm)) {
            *v = csfrs[k].v;
            return 1;
        }
    return number(t, v);
}

/* `[aB]off`: the base register and the offset (0 when absent). */
static int memop(struct tok t, int *ab, long long *off)
{
    const char *close;
    int f;
    if (t.n < 4 || *t.p != '[')
        return 0;
    close = memchr(t.p, ']', (size_t)t.n);
    if (!close)
        return 0;
    {
        struct tok r = trim(t.p + 1, close);
        *ab = tcasm_reg(r.p, r.n, &f);
        if (*ab < 0 || f != 'a')
            return 0;
    }
    {
        struct tok o = trim(close + 1, t.p + t.n);
        *off = 0;
        if (o.n && !number(o, off))
            return 0;
    }
    return 1;
}

/* A transfer's target: `.`, `.+N` or `.-N`, bytes from this
 * instruction. */
static int target(struct tok t, long long *off)
{
    if (t.n == 1 && t.p[0] == '.') {
        *off = 0;
        return 1;
    }
    /* (`.+0+8`, as the file assembler writes `.+8` after a `.`) */
    if (t.n >= 3 && t.p[0] == '.' && (t.p[1] == '+' || t.p[1] == '-'))
        return asm_const_expr(t.p + 1, t.n - 1, off);
    return 0;
}

static int reg_of(struct tok t, int file)
{
    int f, r = tcasm_reg(t.p, t.n, &f);
    return r >= 0 && f == file ? r : -1;
}

static const struct { const char *nm; int op; } alus[] = {
    { "add", TC_ADD }, { "sub", TC_SUB }, { "and", TC_AND }, { "or", TC_OR },
    { "xor", TC_XOR }, { "nor", TC_NOR }, { "andn", TC_ANDN },
    { "mul", TC_MUL }, { "sh", TC_SH }, { "sha", TC_SHA }, { "eq", TC_EQ },
    { "ne", TC_NE }, { "lt", TC_LT }, { "lt.u", TC_LTU }, { "ge", TC_GE },
    { "ge.u", TC_GEU }, { "min", TC_MIN }, { "max", TC_MAX }
};

static int one(const struct stmt *s, struct code *c, char *err, size_t errlen)
{
    struct tok m = s->mn;
    int n = s->nop, a = -1, b = -1, d = -1;
    long long k = 0, off = 0;
#define BAD() do { snprintf(err, errlen, "TriCore asm: '%.*s' with these " \
                            "operands is not supported", m.n, m.p); \
                   return -1; } while (0)
    static const char *const sys0[] = { "nop", "debug", "isync", "dsync",
                                        "svlcx", "rslcx", "enable", "disable",
                                        "rfe", "ret" };
    for (unsigned j = 0; j < sizeof sys0 / sizeof sys0[0]; j++)
        if (teq(m, sys0[j])) {
            if (n) BAD();
            switch (j) {
            case 0: tc_nop(c); break;
            case 1: tc_debug(c); break;
            case 2: tc_isync(c); break;
            case 3: tc_dsync(c); break;
            case 4: tc_svlcx(c); break;
            case 5: tc_rslcx(c); break;
            case 6: tc_enable(c); break;
            case 7: tc_disable(c); break;
            case 8: tc_rfe(c); break;
            default: tc_ret(c); break;
            }
            return 0;
        }
    if (teq(m, "mtcr")) {
        if (n != 2 || !csfr(s->op[0], &k) || (a = reg_of(s->op[1], 'd')) < 0 ||
            k < 0 || k > 0xffff) BAD();
        tc_mtcr(c, (unsigned)k, a);
        return 0;
    }
    if (teq(m, "mfcr")) {
        if (n != 2 || (d = reg_of(s->op[0], 'd')) < 0 || !csfr(s->op[1], &k) ||
            k < 0 || k > 0xffff) BAD();
        tc_mfcr(c, d, (unsigned)k);
        return 0;
    }
    if (teq(m, "syscall")) {
        if (n != 1 || !number(s->op[0], &k) || k < 0 || k > 255) BAD();
        tc_syscall(c, (unsigned)k);
        return 0;
    }
    if (teq(m, "mov")) {
        if (n != 2 || (d = reg_of(s->op[0], 'd')) < 0) BAD();
        if ((b = reg_of(s->op[1], 'd')) >= 0) tc_mov(c, d, b);
        else if (number(s->op[1], &k) && k >= -32768 && k <= 32767)
            tc_mov_imm(c, d, k);
        else BAD();
        return 0;
    }
    if (teq(m, "mov.u") || teq(m, "movh")) {
        if (n != 2 || (d = reg_of(s->op[0], 'd')) < 0 ||
            !number(s->op[1], &k) || k < 0 || k > 0xffff) BAD();
        if (teq(m, "movh")) tc_movh(c, d, (unsigned)k);
        else                tc_mov_u(c, d, k);
        return 0;
    }
    if (teq(m, "mov.a")) {
        if (n != 2 || (d = reg_of(s->op[0], 'a')) < 0 ||
            (b = reg_of(s->op[1], 'd')) < 0) BAD();
        tc_mov_a(c, d, b);
        return 0;
    }
    if (teq(m, "mov.d")) {
        if (n != 2 || (d = reg_of(s->op[0], 'd')) < 0 ||
            (b = reg_of(s->op[1], 'a')) < 0) BAD();
        tc_mov_d(c, d, b);
        return 0;
    }
    if (teq(m, "mov.aa")) {
        if (n != 2 || (d = reg_of(s->op[0], 'a')) < 0 ||
            (b = reg_of(s->op[1], 'a')) < 0) BAD();
        tc_mov_aa(c, d, b);
        return 0;
    }
    if (teq(m, "movh.a")) {
        if (n != 2 || (d = reg_of(s->op[0], 'a')) < 0 ||
            !number(s->op[1], &k) || k < 0 || k > 0xffff) BAD();
        tc_movh_a(c, d, (unsigned)k);
        return 0;
    }
    if (teq(m, "lea")) {
        if (n != 2 || (d = reg_of(s->op[0], 'a')) < 0 ||
            !memop(s->op[1], &b, &off) || off < -32768 || off > 32767) BAD();
        tc_lea(c, d, b, off);
        return 0;
    }
    if (teq(m, "addih.a")) {
        if (n != 3 || (d = reg_of(s->op[0], 'a')) < 0 ||
            (a = reg_of(s->op[1], 'a')) < 0 || !number(s->op[2], &k) ||
            k < 0 || k > 0xffff) BAD();
        tc_addih_a(c, d, a, (unsigned)k);
        return 0;
    }
    if (teq(m, "addi") || teq(m, "addih")) {
        if (n != 3 || (d = reg_of(s->op[0], 'd')) < 0 ||
            (a = reg_of(s->op[1], 'd')) < 0 || !number(s->op[2], &k)) BAD();
        if (teq(m, "addi")) {
            if (k < -32768 || k > 32767) BAD();
            tc_addi(c, d, a, k);
        } else {
            if (k < 0 || k > 0xffff) BAD();
            tc_addih(c, d, a, (unsigned)k);
        }
        return 0;
    }
    for (unsigned j = 0; j < sizeof alus / sizeof alus[0]; j++)
        if (teq(m, alus[j].nm)) {
            int op = alus[j].op;
            if (n != 3 || (d = reg_of(s->op[0], 'd')) < 0 ||
                (a = reg_of(s->op[1], 'd')) < 0) BAD();
            if ((b = reg_of(s->op[2], 'd')) >= 0)
                tc_alu(c, op, d, a, b);
            else if (number(s->op[2], &k) && tc_alu_imm_ok(op, k))
                tc_alu_imm(c, op, d, a, k);
            else BAD();
            return 0;
        }
    {
        static const struct { const char *nm; int size, sign; } lds[] = {
            { "ld.b", 1, 1 }, { "ld.bu", 1, 0 }, { "ld.h", 2, 1 },
            { "ld.hu", 2, 0 }, { "ld.w", 4, 0 }
        };
        for (unsigned j = 0; j < sizeof lds / sizeof lds[0]; j++)
            if (teq(m, lds[j].nm)) {
                if (n != 2 || (d = reg_of(s->op[0], 'd')) < 0 ||
                    !memop(s->op[1], &b, &off) || off < -32768 ||
                    off > 32767) BAD();
                tc_load(c, d, b, off, lds[j].size, lds[j].sign);
                return 0;
            }
    }
    {
        static const struct { const char *nm; int size; } sts[] = {
            { "st.b", 1 }, { "st.h", 2 }, { "st.w", 4 }
        };
        for (unsigned j = 0; j < sizeof sts / sizeof sts[0]; j++)
            if (teq(m, sts[j].nm)) {
                if (n != 2 || !memop(s->op[0], &b, &off) ||
                    (a = reg_of(s->op[1], 'd')) < 0 || off < -32768 ||
                    off > 32767) BAD();
                tc_store(c, a, b, off, sts[j].size);
                return 0;
            }
    }
    if (teq(m, "ld.a")) {
        if (n != 2 || (d = reg_of(s->op[0], 'a')) < 0 ||
            !memop(s->op[1], &b, &off) || off < -32768 || off > 32767) BAD();
        tc_ld_a(c, d, b, off);
        return 0;
    }
    if (teq(m, "st.a")) {
        if (n != 2 || !memop(s->op[0], &b, &off) ||
            (a = reg_of(s->op[1], 'a')) < 0 || off < -32768 || off > 32767)
            BAD();
        tc_st_a(c, a, b, off);
        return 0;
    }
    if (teq(m, "swap.w")) {
        if (n != 2 || !memop(s->op[0], &b, &off) ||
            (a = reg_of(s->op[1], 'd')) < 0 || off < -512 || off > 511) BAD();
        tc_swap_w(c, a, b, off);
        return 0;
    }
    if (teq(m, "cmpswap.w")) {
        if (n != 2 || !memop(s->op[0], &b, &off) ||
            (a = reg_of(s->op[1], 'e')) < 0 || off < -512 || off > 511) BAD();
        tc_cmpswap_w(c, a, b, off);
        return 0;
    }
    /* ---- PC-relative transfers ---- */
    if (teq(m, "j") || teq(m, "jl") || teq(m, "call")) {
        if (n != 1)
            BAD();
        if (!target(s->op[0], &off)) {
            snprintf(err, errlen, "TriCore asm: '%.*s %.*s': a target is a "
                     "label, .+N or .-N%s", m.n, m.p, s->op[0].n, s->op[0].p,
                     isalpha((unsigned char)s->op[0].p[0]) ||
                     s->op[0].p[0] == '_'
                     ? " (a symbol needs a relocation, which an inline "
                       "template cannot carry: write it in a .S file, or "
                       "call through a register with calli)" : "");
            return -1;
        }
        if ((off & 1) || off < -16777216LL || off > 16777214LL) {
            snprintf(err, errlen, "TriCore asm: '%.*s' target %+lld is not "
                     "an even distance within +-16 MiB", m.n, m.p, off);
            return -1;
        }
        tc_w(c, teq(m, "j") ? tc_enc_j((long)off) : teq(m, "jl")
                ? tc_enc_jl((long)off) : tc_enc_call((long)off));
        return 0;
    }
    {
        static const struct { const char *nm; int cond; } brs[] = {
            { "jeq", TC_JEQ }, { "jne", TC_JNE }, { "jlt", TC_JLT },
            { "jlt.u", TC_JLTU }, { "jge", TC_JGE }, { "jge.u", TC_JGEU },
            { "jeq.a", TC_JEQ_A }, { "jne.a", TC_JNE_A },
            { "jz.a", TC_JZ_A }, { "jnz.a", TC_JNZ_A },
            { "jz", -1 }, { "jnz", -2 }, { "loop", -3 }
        };
        for (unsigned j = 0; j < sizeof brs / sizeof brs[0]; j++) {
            int cond = brs[j].cond, one_reg, areg;
            if (!teq(m, brs[j].nm))
                continue;
            one_reg = cond < 0 || cond == TC_JZ_A || cond == TC_JNZ_A;
            areg = cond == -3 || cond >= TC_JEQ_A;
            if (n != (one_reg ? 2 : 3) ||
                (a = reg_of(s->op[0], areg ? 'a' : 'd')) < 0)
                BAD();
            if (!target(s->op[n - 1], &off)) {
                snprintf(err, errlen, "TriCore asm: '%.*s': \"%.*s\" is not a "
                         "branch target (a label, .+N or .-N)%s", m.n, m.p,
                         s->op[n - 1].n, s->op[n - 1].p,
                         isalpha((unsigned char)s->op[n - 1].p[0])
                         ? "; a conditional branch reaches only this file's "
                           "own labels" : "");
                return -1;
            }
            if ((off & 1) || off < -32768 || off > 32766) {
                snprintf(err, errlen, "TriCore asm: '%.*s' target %+lld is "
                         "not an even distance within -32768..32766", m.n,
                         m.p, off);
                return -1;
            }
            if (cond == -3) {
                tc_w(c, tc_enc_loop(a, (long)off));
            } else if (cond < 0) {
                tc_w(c, tc_enc_jcci(cond == -1 ? TC_JEQ : TC_JNE, a, 0,
                                    (long)off));
            } else if (one_reg) {
                tc_w(c, tc_enc_jcc(cond, a, 0, (long)off));
            } else if ((b = reg_of(s->op[1], areg ? 'a' : 'd')) >= 0) {
                tc_w(c, tc_enc_jcc(cond, a, b, (long)off));
            } else if (!areg && number(s->op[1], &k)) {
                if (!tc_jcci_ok(cond, k)) {
                    snprintf(err, errlen, "TriCore asm: '%.*s' constant %lld "
                             "does not fit its 4 bits (%s)", m.n, m.p, k,
                             cond == TC_JLTU || cond == TC_JGEU ? "0..15"
                                                                : "-8..7");
                    return -1;
                }
                tc_w(c, tc_enc_jcci(cond, a, k, (long)off));
            } else {
                BAD();
            }
            return 0;
        }
    }
    if (teq(m, "ji") || teq(m, "calli") || teq(m, "jli")) {
        if (n != 1 || (a = reg_of(s->op[0], 'a')) < 0) BAD();
        if (teq(m, "ji")) tc_ji(c, a);
        else if (teq(m, "calli")) tc_calli(c, a);
        else tc_jli(c, a);
        return 0;
    }
    snprintf(err, errlen, "TriCore asm: '%.*s' is not an instruction EmbCC's "
             "TriCore assembler has", m.n, m.p);
    return -1;
#undef BAD
}

/* One statement, tokenised and assembled. */
static int stmt_text(const char *p, const char *e, struct code *c, char *err,
                     size_t errlen)
{
    struct stmt s;
    struct tok line = trim(p, e);
    const char *q = line.p, *end = line.p + line.n;
    memset(&s, 0, sizeof s);
    if (!line.n)
        return 0;
    while (q < end && !isspace((unsigned char)*q)) q++;
    s.mn = trim(line.p, q);
    while (q < end) {
        const char *st = q;
        int depth = 0;
        while (q < end && (depth || *q != ',')) {
            if (*q == '[') depth++;
            if (*q == ']') depth--;
            q++;
        }
        if (s.nop == MAXOPS) {
            snprintf(err, errlen, "TriCore asm: too many operands "
                     "in '%.*s'", line.n, line.p);
            return -1;
        }
        s.op[s.nop++] = trim(st, q);
        if (q < end) q++;
    }
    /* a refusal tcasm_symform wrote for a form it recognised */
    if (s.mn.n && s.mn.p[0] == '\001') {
        snprintf(err, errlen, "%.*s", line.n - 1, line.p + 1);
        return -1;
    }
    return one(&s, c, err, errlen);
}

/* ---- a template ----------------------------------------------------------
 *
 * Its statements, with GCC's numeric labels resolved: `1:` defines the
 * next instance of label 1, `1b` is the latest one at or before the
 * statement and `1f` the next one after it. The statements are assembled
 * once with every reference at `.+0` to learn where each starts, then for
 * good. */
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
    long off = 0;
    if (ns < 0)
        return -1;
    for (int pass = 0; pass < 2; pass++) {
        for (int k = 0; k < ns; k++) {
            char buf[512];
            int len = resolve(st, ns, k, pass == 0, buf, (int)sizeof buf, err,
                              errlen);
            struct code tmp = { 0 };
            if (len < 0)
                return -1;
            if (pass == 0) {
                /* a placeholder that does not encode (a loop's `.+0` does)
                 * is still one 32-bit instruction */
                char e2[16];
                st[k].off = off;
                if (stmt_text(buf, buf + len, &tmp, e2, sizeof e2) == 0)
                    off += tmp.len;
                else
                    off += 4;
                free(tmp.p);
                continue;
            }
            if (stmt_text(buf, buf + len, c, err, errlen))
                return -1;
        }
    }
    return 0;
}

/* ---- for the file assembler (src/as/gas.c) ---------------------------- */

int tcasm_encode(const char *text, struct code *c, char *err, int errlen)
{
    return tcasm_assemble(text, c, err, (size_t)(errlen > 0 ? errlen : 1));
}

int tcasm_is_reg(const char *name, int len)
{
    int f;
    return tcasm_reg(name, len, &f);
}

/* `hi`/`lo` before a `:`, or after a `%`, is an operator; a core register's
 * name is an operand of mtcr/mfcr. */
int tcasm_is_word(const char *stmt, const char *w, int len)
{
    const char *p = stmt;
    struct tok t;
    if ((len == 2 && (!strncmp(w, "hi", 2) || !strncmp(w, "lo", 2))) &&
        (w[2] == ':' || (w > stmt && w[-1] == '%')))
        return 1;
    while (isspace((unsigned char)*p)) p++;
    t.p = p;
    while (*p && !isspace((unsigned char)*p)) p++;
    t.n = (int)(p - t.p);
    if (teq(t, "mtcr") || teq(t, "mfcr")) {
        struct tok x;
        x.p = w;
        x.n = len;
        for (unsigned k = 0; k < sizeof csfrs / sizeof csfrs[0]; k++)
            if (teq(x, csfrs[k].nm))
                return 1;
    }
    return 0;
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

/* A symbol at p, then an optional +K/-K: the length consumed, or 0. */
static int sym_at(const char *p, int *slen, long *add)
{
    int n = 0;
    if (!(isalpha((unsigned char)p[0]) || p[0] == '_' ||
          (p[0] == '.' && (isalpha((unsigned char)p[1]) || p[1] == '_'))))
        return 0;
    while (isalnum((unsigned char)p[n]) || p[n] == '_' || p[n] == '.' ||
           p[n] == '$')
        n++;
    {
        int f;
        if (tcasm_reg(p, n, &f) >= 0)
            return 0;
    }
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

/* The statements whose operand is a SYMBOL, each rewritten with a zero in
 * its place and the relocation it carries:
 *
 *   j, jl, call SYM                       R_TRICORE_24REL
 *   movh dC / movh.a aC, hi:SYM           R_TRICORE_HIADJ  (or %hi(SYM))
 *   addi dC, dA, lo:SYM                   R_TRICORE_LO     (or %lo(SYM))
 *   lea aC, [aB]lo:SYM, and the loads and stores with [aB]lo:SYM
 *                                         R_TRICORE_LO2
 *
 * A conditional branch or a loop to a symbol is refused: its 15-bit
 * displacement (R_TRICORE_15REL) is not one embld applies. */
int tcasm_symform(const char *stmt, struct asm_symform *f)
{
    const char *p = stmt, *m, *o, *c;
    int mlen, slen, n;
    long add;
    struct tok mt;
    while (isspace((unsigned char)*p)) p++;
    m = p;
    while (*p && !isspace((unsigned char)*p)) p++;
    mlen = (int)(p - m);
    mt.p = m;
    mt.n = mlen;
    while (isspace((unsigned char)*p)) p++;
    memset(f, 0, sizeof *f);
    if (!mlen)
        return 0;
    if (teq(mt, "j") || teq(mt, "jl") || teq(mt, "call")) {
        n = sym_at(p, &slen, &add);
        {
            const char *r = p + n;
            while (isspace((unsigned char)*r)) r++;
            if (!n || *r)
                return 0;
        }
        f->sym_at = (int)(p - stmt);
        f->sym_len = slen;
        f->addend = add;
        snprintf(f->encode, sizeof f->encode, "%.*s .+0", mlen, m);
        f->site[0].reloc = R_TRICORE_24REL;
        f->nsites = 1;
        return 1;
    }
    /* hi:/lo: and %hi()/%lo() */
    for (o = p; *o; o++) {
        int hi, pct = 0, k;
        const char *sp;
        if (!strncmp(o, "hi:", 3) || !strncmp(o, "lo:", 3)) {
            sp = o + 3;
        } else if (!strncmp(o, "%hi(", 4) || !strncmp(o, "%lo(", 4)) {
            sp = o + 4;
            pct = 1;
        } else {
            continue;
        }
        if (o > stmt && (isalnum((unsigned char)o[-1]) || o[-1] == '_'))
            continue;
        hi = o[pct] == 'h';
        n = sym_at(sp, &slen, &add);
        if (!n)
            return 0;                   /* a number's: refused by one() */
        k = n;
        if (pct) {
            if (sp[k] != ')')
                return 0;
            k++;
        }
        c = sp + k;
        {
            /* the last operand -- but a store's [aB]lo:SYM comes first */
            const char *r = c;
            while (isspace((unsigned char)*r)) r++;
            if (*r && !(*r == ',' && o > stmt && o[-1] == ']'))
                return refuse_form(f, stmt, sp, slen, "an address half is "
                                   "a whole operand");
        }
        f->sym_at = (int)(sp - stmt);
        f->sym_len = slen;
        f->addend = add;
        if (hi) {
            if (!teq(mt, "movh") && !teq(mt, "movh.a"))
                return refuse_form(f, stmt, sp, slen, "hi:SYM (%hi) is "
                                   "movh's or movh.a's operand");
            f->site[0].reloc = R_TRICORE_HIADJ;
        } else if (teq(mt, "addi")) {
            f->site[0].reloc = R_TRICORE_LO;
        } else if (o > stmt && o[-1] == ']' &&
                   (teq(mt, "lea") || teq(mt, "ld.a") || teq(mt, "st.a") ||
                    teq(mt, "ld.b") || teq(mt, "ld.bu") || teq(mt, "ld.h") ||
                    teq(mt, "ld.hu") || teq(mt, "ld.w") || teq(mt, "st.b") ||
                    teq(mt, "st.h") || teq(mt, "st.w"))) {
            f->site[0].reloc = R_TRICORE_LO2;
        } else if (o > stmt && o[-1] == ']') {
            /* st.* puts the memory operand first: [aB]lo:SYM, dA */
            return refuse_form(f, stmt, sp, slen, "lo:SYM in a memory "
                               "operand belongs to lea or a load or store "
                               "with a 16-bit offset");
        } else {
            return refuse_form(f, stmt, sp, slen, "lo:SYM (%lo) is addi's "
                               "operand, or a [aB] offset's");
        }
        snprintf(f->encode, sizeof f->encode, "%.*s0%s", (int)(o - stmt), stmt,
                 c);
        f->nsites = 1;
        return 1;
    }
    /* a conditional branch or a loop to a symbol */
    {
        static const char *const brs[] = { "jeq", "jne", "jlt", "jlt.u",
            "jge", "jge.u", "jeq.a", "jne.a", "jz.a", "jnz.a", "jz", "jnz",
            "loop", NULL };
        for (int k = 0; brs[k]; k++)
            if (teq(mt, brs[k])) {
                const char *last = p;
                for (c = p; *c; c++)
                    if (*c == ',')
                        last = c + 1;
                while (isspace((unsigned char)*last)) last++;
                n = sym_at(last, &slen, &add);
                if (!n)
                    return 0;
                return refuse_form(f, stmt, last, slen, "a conditional branch "
                                   "or loop reaches only a label of its own "
                                   "section: its 15-bit displacement "
                                   "(R_TRICORE_15REL) is not one embld "
                                   "applies; branch over a j");
            }
    }
    return 0;
}
