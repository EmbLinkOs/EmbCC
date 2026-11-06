/* A32 Advanced SIMD (NEON), and the VFP forms beside it, from a table
 * (neon.inc) that tools/neon-isa learns from llvm-mc: every word of the
 * NEON encoding space decoded, each bit of an example flipped to tell
 * opcode from operand from reserved, each operand bit weighed, every
 * field's values offered back to llvm-mc's assembler. The assembler (asm.c)
 * asks here first on an A32 target; what the table does not hold is the
 * hand-written encoders' (VFP's loads and stores, conditional forms).
 *
 * A statement is read the way the learner read llvm-mc's output: its
 * operands reduced to a shape -- Q, D, S, R registers, `#N` and `#F`
 * immediates, a register list as `{D3}` (its first register, the count;
 * `{D2s}` spaced by two, `{D2[]}` every lane, `{D2[N]}` one), memory as
 * `[R]`, `[R:128]`, `[R]!` or `[R],R` -- and the values in order. A
 * float immediate is the eight bits VFPExpandImm takes, a vmov.i64 byte
 * mask its eight byte bits, as the table's fields hold them. */
#include "neon.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../asmexpr.h"

struct nn_fbit { unsigned char pos, shift, neg; };
struct nn_field {
    char kind;                  /* Q D S R N F */
    unsigned char nb;
    unsigned short fb;
    int v0;
    unsigned char rule;         /* 0 any value, 2 only nok at nn_oks[ok] */
    unsigned short ok, nok;
};
struct nn_insn {
    const char *mnem, *shape;
    unsigned int base;
    unsigned short f, nf;
};
struct nn_syn { const char *spelling, *canon; };

#include "neon.inc"

#define NINSN ((int)(sizeof nn_insns / sizeof nn_insns[0]))
#define NSYN ((int)(sizeof nn_syns / sizeof nn_syns[0]))

static int first_of(const char *mn)
{
    int lo = 0, hi = NINSN;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (strcmp(nn_insns[mid].mnem, mn) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < NINSN && !strcmp(nn_insns[lo].mnem, mn) ? lo : -1;
}

static const char *canonical(const char *mn)
{
    if (first_of(mn) >= 0)
        return mn;
    for (int k = 0; k < NSYN; k++)
        if (!strcmp(nn_syns[k].spelling, mn))
            return nn_syns[k].canon;
    return NULL;
}

/* ---- reading ------------------------------------------------------------ */

/* a register past the file's last (d32), seen while reading */
static char g_past[8];

#define MAXAT 8
struct st {
    char shape[96];
    int sh;
    long long v[MAXAT];
    char kind[MAXAT];
    int n;
};

static void put(struct st *s, const char *t)
{
    while (*t && s->sh < (int)sizeof s->shape - 1)
        s->shape[s->sh++] = *t++;
    s->shape[s->sh] = 0;
}

static int atom(struct st *s, char kind, long long v)
{
    if (s->n == MAXAT)
        return 0;
    s->kind[s->n] = kind;
    s->v[s->n++] = v;
    return 1;
}

/* A register at p: its file (Q D S R) in *k, its number returned, its
 * length in *len; -1 when p names none. */
static int reg_at(const char *p, char *k, int *len)
{
    static const struct { const char *n; int r; } named[] = {
        { "sp", 13 }, { "lr", 14 }, { "pc", 15 }, { "ip", 12 }, { "fp", 11 },
        { "sb", 9 }, { "sl", 10 },
    };
    int n = 0, r = 0, max;
    char c = (char)tolower((unsigned char)p[0]);
    for (unsigned j = 0; j < sizeof named / sizeof named[0]; j++)
        if (tolower((unsigned char)p[0]) == named[j].n[0] &&
            tolower((unsigned char)p[1]) == named[j].n[1] &&
            !isalnum((unsigned char)p[2]) && p[2] != '_') {
            *k = 'R';
            *len = 2;
            return named[j].r;
        }
    if (c != 'q' && c != 'd' && c != 's' && c != 'r')
        return -1;
    if (!isdigit((unsigned char)p[1]))
        return -1;
    for (n = 1; isdigit((unsigned char)p[n]); n++)
        r = r * 10 + (p[n] - '0');
    if (isalnum((unsigned char)p[n]) || p[n] == '_' || n > 3)
        return -1;
    max = c == 'q' ? 15 : c == 'r' ? 15 : 31;
    if (n == 3 && p[1] == '0')
        return -1;
    if (r > max) {                  /* d32: a register that is not there */
        snprintf(g_past, sizeof g_past, "%.*s", n, p);
        return -2;
    }
    *k = c == 'q' ? 'Q' : c == 'd' ? 'D' : c == 's' ? 'S' : 'R';
    *len = n;
    return r;
}

static const char *skip(const char *p)
{
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

/* A register list `{d0, d1}`, `{d0-d3}`, `{d0[], d1[]}`, `{d1[1], d3[1]}`. */
static const char *list(const char *p, struct st *s)
{
    int regs[32], nr = 0, lane = -2, step;
    char k0 = 0;
    char tag[24];
    p = skip(p + 1);
    while (*p && *p != '}') {
        char k;
        int len, r = reg_at(p, &k, &len), l = -2;
        if (r < 0 || (k0 && k != k0) || k == 'R')
            return NULL;
        k0 = k;
        p = skip(p + len);
        if (*p == '[') {
            p = skip(p + 1);
            if (*p == ']') {
                l = -1;
            } else {
                char *e;
                l = (int)strtol(p, &e, 0);
                p = skip(e);
                if (*p != ']')
                    return NULL;
            }
            p = skip(p + 1);
        }
        if (nr && l != lane)
            return NULL;
        lane = l;
        if (*p == '-') {
            char k2;
            int len2, r2;
            p = skip(p + 1);
            r2 = reg_at(p, &k2, &len2);
            if (r2 < r || k2 != k)
                return NULL;
            p = skip(p + len2);
            for (int x = r; x <= r2 && nr < 32; x++)
                regs[nr++] = x;
        } else if (nr < 32) {
            regs[nr++] = r;
        }
        if (*p == ',')
            p = skip(p + 1);
        else if (*p != '}')
            return NULL;
    }
    if (*p != '}' || !nr)
        return NULL;
    step = nr > 1 ? regs[1] - regs[0] : 1;
    if (step != 1 && step != 2)
        return NULL;
    for (int t = 0; t + 1 < nr; t++)
        if (regs[t + 1] - regs[t] != step)
            return NULL;
    snprintf(tag, sizeof tag, "{%c%d%s%s}", k0, nr, step == 2 ? "s" : "",
             lane == -2 ? "" : lane == -1 ? "[]" : "[N]");
    put(s, tag);
    if (!atom(s, k0, regs[0]))
        return NULL;
    if (lane >= 0 && !atom(s, 'N', lane))
        return NULL;
    return p + 1;
}

/* the eight bits VFPExpandImm makes a single-precision value of */
static int vfp_imm8(double v)
{
    for (int k = 0; k < 256; k++) {
        int a = k >> 7, b = (k >> 6) & 1, cd = (k >> 4) & 3, efgh = k & 15;
        double m = (16 + efgh) / 16.0;
        /* the exponent NOT(b):b:b:b:b:b:c:d less 127: b ? cd-3 : cd+1 */
        int e = b ? cd - 3 : cd + 1;
        double x;
        x = m;
        if (e > 0)
            for (int t = 0; t < e; t++) x *= 2;
        else
            for (int t = 0; t < -e; t++) x /= 2;
        if (a)
            x = -x;
        if (x == v)
            return k;
    }
    return -1;
}

static int parse(const char *mn, const char *p, struct st *s)
{
    memset(s, 0, sizeof *s);
    p = skip(p);
    while (*p) {
        char k;
        int len, r;
        if (*p == ' ' || *p == '\t') { p++; continue; }
        if (*p == '{') {
            p = list(p, s);
            if (!p)
                return 0;
            continue;
        }
        if (*p == ':' && isdigit((unsigned char)p[1])) {
            /* [r0:128], and GNU's [r0, :128] */
            const char *q = p + 1;
            char t[16];
            int n = 0;
            while (isdigit((unsigned char)*q) && n < 8) t[n++] = *q++;
            t[n] = 0;
            if (s->sh && s->shape[s->sh - 1] == ',')
                s->shape[--s->sh] = 0;
            put(s, ":");
            put(s, t);
            p = q;
            continue;
        }
        if (strchr("[],!", *p)) {
            char t[2] = { *p, 0 };
            put(s, t);
            p++;
            continue;
        }
        if (*p == '#') {
            const char *e = p + 1;
            char buf[64];
            int n = 0;
            while (*e && *e != ',' && *e != ']' && *e != ' ' && n < 63)
                buf[n++] = *e++;
            buf[n] = 0;
            if (strpbrk(buf, ".eE") && strncmp(buf, "0x", 2) && strncmp(buf, "-0x", 3)) {
                char *end;
                double d = strtod(buf, &end);
                int i8 = *end ? -1 : vfp_imm8(d);
                if (i8 < 0)
                    return 0;
                put(s, "#F");
                if (!atom(s, 'F', i8))
                    return 0;
            } else {
                long long v;
                if (!asm_const_expr(buf, n, &v))
                    return 0;
                if (!strcmp(mn, "vmov.i64")) {
                    long long m = 0;
                    for (int j = 0; j < 8; j++) {
                        int byte = (int)((unsigned long long)v >> (8 * j) & 0xff);
                        if (byte == 0xff) m |= 1LL << j;
                        else if (byte) return 0;
                    }
                    v = m;
                }
                put(s, "#N");
                if (!atom(s, 'N', v))
                    return 0;
            }
            p = e;
            continue;
        }
        r = reg_at(p, &k, &len);
        if (r >= 0) {
            char t[2] = { k, 0 };
            if (!atom(s, k, r))
                return 0;
            p += len;
            if (*p == '[' && isdigit((unsigned char)p[1])) {
                char *e;
                long lane = strtol(p + 1, &e, 0);
                if (*e != ']')
                    return 0;
                put(s, t);
                put(s, "[N]");
                if (!atom(s, 'N', lane))
                    return 0;
                p = e + 1;
            } else {
                put(s, t);
            }
            continue;
        }
        if (isdigit((unsigned char)*p) || *p == '-') {
            char *e;
            long long v = strtoll(p, &e, 0);
            put(s, "N");
            if (!atom(s, 'N', v))
                return 0;
            p = e;
            continue;
        }
        {
            static const char *const sys[] = { "apsr_nzcv", "fpscr", "fpexc",
                "fpsid", "mvfr0", "mvfr1", "mvfr2", "fpinst2", "fpinst", NULL };
            int j;
            for (j = 0; sys[j]; j++)
                if (!strncmp(p, sys[j], strlen(sys[j]))) {
                    put(s, sys[j]);
                    p += strlen(sys[j]);
                    break;
                }
            if (!sys[j])
                return 0;
        }
    }
    return 1;
}

/* ---- encoding ----------------------------------------------------------- */

static int fit_field(const struct nn_field *f, long long v, unsigned *w)
{
    long long x = v - f->v0;
    unsigned bits = 0;
    const struct nn_fbit *b = &nn_fbits[f->fb];
    int allneg = f->nb > 0;
    if (f->rule == 2) {
        int ok = 0;
        for (int k = 0; k < f->nok && !ok; k++)
            ok = nn_oks[f->ok + k] == v;
        if (!ok)
            return 0;
    }
    for (int k = 0; k < f->nb; k++)
        if (!b[k].neg)
            allneg = 0;
    if (allneg) {                   /* every weight negative: shift counts */
        x = -x;
        for (int k = 0; k < f->nb; k++)
            if (x > 0 && ((x >> b[k].shift) & 1)) {
                x -= 1LL << b[k].shift;
                bits |= 1u << b[k].pos;
            }
    } else {
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
    }
    if (x != 0)
        return 0;
    *w |= bits;
    return 1;
}

int neon_encode(const char *stmt, int len, unsigned long *word, char *err,
                int errlen)
{
    char mn[32];
    const char *cm, *p = stmt, *e = stmt + len;
    struct st s;
    int i0, k = 0, shape_seen = 0;
    char ops[512];
    while (p < e && isspace((unsigned char)*p)) p++;
    while (p < e && !isspace((unsigned char)*p) && k < (int)sizeof mn - 1)
        mn[k++] = (char)tolower((unsigned char)*p++);
    mn[k] = 0;
    if (mn[0] != 'v')
        return 0;
    cm = canonical(mn);
    if (!cm)
        return 0;
    if (e - p >= (long)sizeof ops)
        return 0;
    memcpy(ops, p, (size_t)(e - p));
    ops[e - p] = 0;
    {
        /* lower-case the register names; an operand has no symbol here */
        for (char *q = ops; *q; q++)
            *q = (char)tolower((unsigned char)*q);
    }
    g_past[0] = 0;
    if (!parse(cm, ops, &s)) {
        if (g_past[0]) {
            snprintf(err, (size_t)errlen, "%.*s: there is no register %s "
                     "(q0-q15, d0-d31, s0-s31)", len, stmt, g_past);
            return -1;
        }
        return 0;
    }
    /* vext's immediate counts elements of the size written; llvm-mc's
     * table (as its disassembler prints) names the largest element that
     * divides the byte offset and has a form of this shape: `vext.8 q0,
     * q1, q2, #8` is vext.64 #1 */
    if (!strncmp(cm, "vext.", 5) && s.n == 4 && s.kind[3] == 'N') {
        static char vx[16];
        int esz = atoi(cm + 5) / 8;
        long long bytes = s.v[3] * (esz > 0 ? esz : 1);
        for (int z = 8; z >= 1; z /= 2) {
            int found = 0;
            if (bytes % z)
                continue;
            snprintf(vx, sizeof vx, "vext.%d", z * 8);
            for (int i = first_of(vx); i >= 0 && i < NINSN &&
                                       !strcmp(nn_insns[i].mnem, vx); i++)
                if (!strcmp(nn_insns[i].shape, s.shape))
                    found = 1;
            if (found) {
                cm = vx;
                s.v[3] = bytes / z;
                break;
            }
        }
    }
    i0 = first_of(cm);
    for (int i = i0; i < NINSN && !strcmp(nn_insns[i].mnem, cm); i++) {
        const struct nn_insn *in = &nn_insns[i];
        unsigned w = in->base;
        int ok = 1;
        if (strcmp(in->shape, s.shape))
            continue;
        shape_seen = 1;
        if (in->nf != s.n)
            continue;
        for (int f = 0; f < s.n && ok; f++) {
            const struct nn_field *fd = &nn_fields[in->f + f];
            if (fd->kind != s.kind[f])
                ok = 0;
            else if (!fit_field(fd, s.v[f], &w) &&
                     !(fd->kind == 'N' && s.v[f] >= 0x80000000LL &&
                       s.v[f] <= 0xffffffffLL &&
                       fit_field(fd, s.v[f] - 0x100000000LL, &w)))
                ok = 0;             /* (#0x80000000 is -2^31 to a field
                                     * llvm-mc printed signed) */
        }
        if (ok) {
            *word = w;
            return 1;
        }
    }
    if (!shape_seen)
        return 0;                   /* the hand-written encoders', if anyone's */
    /* which operand: the first that fails in the form that takes most */
    {
        int best = -1, bk = -1;
        for (int i = i0; i < NINSN && !strcmp(nn_insns[i].mnem, cm); i++) {
            const struct nn_insn *in = &nn_insns[i];
            unsigned w = 0;
            int f;
            if (strcmp(in->shape, s.shape) || in->nf != s.n)
                continue;
            for (f = 0; f < s.n; f++)
                if (nn_fields[in->f + f].kind != s.kind[f] ||
                    !fit_field(&nn_fields[in->f + f], s.v[f], &w))
                    break;
            if (f > bk) { bk = f; best = i; }
        }
        if (best >= 0 && bk < s.n) {
            static const char *const what[] = { "Q", "a quadword register",
                "D", "a doubleword register", "S", "a single register",
                "R", "a core register", "N", "an immediate", "F",
                "a float immediate", NULL };
            const char *kn = "an operand";
            for (int j = 0; what[j]; j += 2)
                if (what[j][0] == s.kind[bk]) kn = what[j + 1];
            if (s.kind[bk] == 'N' || s.kind[bk] == 'F')
                snprintf(err, (size_t)errlen, "%.*s: operand %d, %s (%lld), "
                         "is not one any form of %s %s holds", len, stmt,
                         bk + 1, kn, s.v[bk], cm, s.shape);
            else
                snprintf(err, (size_t)errlen, "%.*s: operand %d, %s, is not "
                         "one %s %s takes there (an even register for a pair, "
                         "a list within d0-d31, a register other than pc)",
                         len, stmt, bk + 1, kn, cm, s.shape);
            return -1;
        }
    }
    snprintf(err, (size_t)errlen, "%.*s: no form of %s %s takes these "
             "operands", len, stmt, cm, s.shape);
    return -1;
}
