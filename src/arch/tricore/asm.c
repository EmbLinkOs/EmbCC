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
 *
 * A form outside this list is refused by name, never guessed at. */
#include "asm.h"
#include "emit.h"

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
    int n = s->nop, a, b, d;
    long long k, off;
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

int tcasm_assemble(const char *text, struct code *c, char *err, size_t errlen)
{
    const char *p = text;
    while (*p) {
        const char *e = p;
        struct stmt s;
        while (*e && *e != '\n' && *e != ';')
            e++;
        {
            struct tok line = trim(p, e);
            const char *q = line.p, *end = line.p + line.n;
            memset(&s, 0, sizeof s);
            if (line.n) {
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
                if (one(&s, c, err, errlen))
                    return -1;
            }
        }
        p = *e ? e + 1 : e;
    }
    return 0;
}
