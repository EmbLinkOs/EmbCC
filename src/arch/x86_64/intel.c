/* See intel.h. A statement's prefixes, mnemonic and operands are read
 * into iop's, each written out in AT&T's spelling, and the mnemonic is
 * renamed or given the suffix AT&T needs. */
#include "intel.h"
#include "att.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *g_e;
static int g_el;

static int ifail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_e, (size_t)g_el, fmt, ap);
    va_end(ap);
    return -1;
}

enum { O_REG = 1, O_MEM, O_IMM, O_LBL, O_FAR };
enum { CLS_ST = 100 };              /* (an x87 register: no class in att.c) */

struct iop {
    int kind;
    int cls, rsize;                 /* O_REG: the class and size */
    int kw;                         /* a size keyword's bytes, 0 none */
    int far, near_, short_, offset;
    int stn;                        /* x87: st(N)'s N, -1 for another */
    char t[400];                    /* the AT&T operand */
    char t2[200];                   /* O_FAR: the offset (t the selector) */
};

static int is_id(int c) { return isalnum(c) || c == '_' || c == '.' || c == '$' || c == '@' || c == '?'; }

static const char *skip_sp(const char *p)
{
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

static void trim(const char **s, int *n)
{
    while (*n > 0 && isspace((unsigned char)**s)) { (*s)++; (*n)--; }
    while (*n > 0 && isspace((unsigned char)(*s)[*n - 1])) (*n)--;
}

/* the word at p, case-insensitively, is w (and ends there) */
static int word_is(const char *p, int n, const char *w)
{
    int l = (int)strlen(w);
    if (n < l) return 0;
    for (int k = 0; k < l; k++)
        if (tolower((unsigned char)p[k]) != w[k]) return 0;
    return n == l || !is_id((unsigned char)p[l]);
}

static int word_len(const char *p, int n)
{
    int k = 0;
    while (k < n && (isalnum((unsigned char)p[k]) || p[k] == '_')) k++;
    return k;
}

static int size_kw(const char *p, int n)
{
    static const struct { const char *w; int sz; } kw[] = {
        {"byte",1},{"word",2},{"dword",4},{"fword",6},{"qword",8},{"tword",10},
        {"tbyte",10},{"oword",16},{"xmmword",16},{"yword",32},{"ymmword",32},
        {"zword",64},{"zmmword",64},
    };
    for (unsigned k = 0; k < sizeof kw / sizeof *kw; k++)
        if ((int)strlen(kw[k].w) == n && word_is(p, n, kw[k].w))
            return kw[k].sz;
    return 0;
}

/* a register named by s[0..n): its class (and size, and AT&T name in
 * out), 0 when it is not one */
static int reg_of(const char *s, int n, const struct x86intel_opts *o, int *size,
                  int *stn, char *out, int cap)
{
    char low[16];
    if (n > 0 && s[0] == '%') { s++; n--; }
    else if (o->prefix_regs) return 0;
    if (n <= 0 || n >= (int)sizeof low) return 0;
    for (int k = 0; k < n; k++) {
        if (!is_id((unsigned char)s[k]) && s[k] != '(' && s[k] != ')') return 0;
        low[k] = (char)tolower((unsigned char)s[k]);
    }
    low[n] = 0;
    *stn = -1;
    if (!strcmp(low, "st")) {
        *size = 10; *stn = 0;
        snprintf(out, (size_t)cap, "%%st");
        return CLS_ST;
    }
    if ((n == 3 && !strncmp(low, "st", 2) && low[2] >= '0' && low[2] <= '7') ||
        (n == 5 && !strncmp(low, "st(", 3) && low[3] >= '0' && low[3] <= '7' && low[4] == ')')) {
        int k = low[n == 3 ? 2 : 3] - '0';
        *size = 10; *stn = k;
        if (k == 0) snprintf(out, (size_t)cap, "%%st");
        else snprintf(out, (size_t)cap, "%%st(%d)", k);
        return CLS_ST;
    }
    int c = x86att_reg_class(low, n, size);
    if (!c) return 0;
    snprintf(out, (size_t)cap, "%%%s", low);
    return c;
}

/* [ ... ]'s inside, s[0..n): disp(base,index,scale) in op->t */
static int parse_mem(const char *s, int n, const struct x86intel_opts *o,
                     const char *seg_out, struct iop *op)
{
    char seg[8], base[16] = "", index[16] = "", disp[320] = "";
    int scale = 1, rel = -1, nosplit = 0, bsz = 0, isz = 0, st;
    snprintf(seg, sizeof seg, "%s", seg_out ? seg_out : "");
    trim(&s, &n);
    for (;;) {                      /* rel, abs, nosplit, a size: words first */
        int w = word_len(s, n);
        if (!w) break;
        if (word_is(s, n, "rel")) rel = 1;
        else if (word_is(s, n, "abs")) rel = 0;
        else if (word_is(s, n, "nosplit")) nosplit = 1;
        else if (!size_kw(s, w)) break;
        s += w; n -= w;
        trim(&s, &n);
    }
    /* a segment override inside: [es:bx] */
    {
        int w = word_len(s, n), sz;
        char rn[16];
        if (w == 2 && w < n && s[w] == ':' &&
            reg_of(s, w, o, &sz, &st, rn, sizeof rn) == X86R_SEG) {
            snprintf(seg, sizeof seg, "%s", rn);
            s += w + 1; n -= w + 1;
            trim(&s, &n);
        } else if (n > 3 && s[0] == '%' && s[3] == ':' &&
                   reg_of(s, 3, o, &sz, &st, rn, sizeof rn) == X86R_SEG) {
            snprintf(seg, sizeof seg, "%s", rn);
            s += 4; n -= 4;
            trim(&s, &n);
        }
    }
    /* the terms, split at the + and - outside parentheses */
    int i = 0;
    while (i < n) {
        char sign = '+';
        while (i < n && (isspace((unsigned char)s[i]) || s[i] == '+' || s[i] == '-')) {
            if (s[i] == '-') sign = sign == '-' ? '+' : '-';
            i++;
        }
        int b = i, depth = 0;
        while (i < n) {
            if (s[i] == '(') depth++;
            else if (s[i] == ')') depth--;
            else if (depth == 0 && (s[i] == '+' || s[i] == '-') && i > b) {
                /* (an exponent's sign is not a term's: 1e-3 is no address) */
                break;
            }
            i++;
        }
        const char *t = s + b;
        int tn = i - b;
        trim(&t, &tn);
        if (!tn) continue;
        int sz, cls;
        char rn[16];
        cls = reg_of(t, tn, o, &sz, &st, rn, sizeof rn);
        if (cls == X86R_GPR || (cls == 0 && (word_is(t, tn, "rip") || word_is(t, tn, "eip")))) {
            if (sign == '-') return ifail("a register cannot be subtracted in an address");
            if (!cls) { snprintf(rn, sizeof rn, "%%%s", tolower((unsigned char)t[0]) == 'r' ? "rip" : "eip"); sz = 8; }
            if (!base[0]) { snprintf(base, sizeof base, "%s", rn); bsz = sz; }
            else if (!index[0]) { snprintf(index, sizeof index, "%s", rn); isz = sz; scale = 1; }
            else return ifail("an address has a base and an index, no third register");
            continue;
        }
        if (cls == X86R_VEC && !index[0]) {
            if (sign == '-') return ifail("a register cannot be subtracted in an address");
            snprintf(index, sizeof index, "%s", rn);
            isz = sz;
            scale = 1;
            continue;
        }
        if (cls) return ifail("'%.*s' cannot be part of an address", tn, t);
        /* reg*N or N*reg */
        const char *star = NULL;
        depth = 0;
        for (int k = 0; k < tn; k++) {
            if (t[k] == '(') depth++;
            else if (t[k] == ')') depth--;
            else if (t[k] == '*' && depth == 0) { star = t + k; break; }
        }
        if (star) {
            const char *l = t, *r = star + 1;
            int ln = (int)(star - t), rnn = tn - ln - 1;
            trim(&l, &ln); trim(&r, &rnn);
            int lsz, rsz, lst, rst;
            char lr[16], rr[16];
            int lc = reg_of(l, ln, o, &lsz, &lst, lr, sizeof lr);
            int rc = reg_of(r, rnn, o, &rsz, &rst, rr, sizeof rr);
            if (lc == X86R_GPR || rc == X86R_GPR || lc == X86R_VEC || rc == X86R_VEC) {
                int left = lc == X86R_GPR || lc == X86R_VEC;
                const char *num = left ? r : l;
                int numn = left ? rnn : ln;
                char nb[32];
                if (sign == '-') return ifail("a register cannot be subtracted in an address");
                if (index[0]) return ifail("an address has one index");
                snprintf(nb, sizeof nb, "%.*s", numn < 31 ? numn : 31, num);
                char *e;
                long v = strtol(nb, &e, 0);
                if (*skip_sp(e) || v < 1 || v > 9)
                    return ifail("'%.*s': the scale is 1, 2, 4 or 8", tn, t);
                snprintf(index, sizeof index, "%s", left ? lr : rr);
                isz = left ? lsz : rsz;
                scale = (int)v;
                continue;
            }
        }
        size_t dl = strlen(disp);
        snprintf(disp + dl, sizeof disp - dl, "%s%.*s",
                 dl == 0 && sign == '+' ? "" : (sign == '-' ? "-" : "+"), tn, t);
    }
    (void)bsz; (void)isz;
    /* an index alone at scale 1 is a base; NASM's [eax*2] is [eax+eax],
     * its [eax*3] [eax+eax*2] (unless nosplit) */
    int vsib = index[0] && (index[1] == 'x' || index[1] == 'y' || index[1] == 'z');
    if (index[0] && !base[0] && !vsib) {
        if (scale == 1) {
            memcpy(base, index, sizeof base); index[0] = 0;
        } else if (o->flavor == X86I_NASM && !nosplit &&
                   (scale == 2 || scale == 3 || scale == 5 || scale == 9)) {
            memcpy(base, index, sizeof base);
            scale -= 1;
        }
    }
    /* NASM's: an esp as the index at scale 1 is the base */
    if (o->flavor == X86I_NASM && index[0] && scale == 1 &&
        (!strcmp(index, "%esp") || !strcmp(index, "%rsp"))) {
        char t[16];
        memcpy(t, base, sizeof t); memcpy(base, index, sizeof base); memcpy(index, t, sizeof index);
    }
    /* [x] in 64-bit code: [rel x] by `default rel` or by hand (but an
     * fs/gs one, which is thread data, stays absolute) */
    if (o->mode == 64 && !base[0] && !index[0] &&
        (rel == 1 || (rel == -1 && o->default_rel)) &&
        strcmp(seg, "%fs") && strcmp(seg, "%gs"))
        snprintf(base, sizeof base, "%%rip");
    char regs[64] = "";
    if (index[0])
        snprintf(regs, sizeof regs, "(%s,%s,%d)", base, index, scale);
    else if (base[0])
        snprintf(regs, sizeof regs, "(%s)", base);
    snprintf(op->t, sizeof op->t, "%s%s%s%s", seg, seg[0] ? ":" : "",
             disp[0] ? disp : (regs[0] ? "" : "0"), regs);
    op->kind = O_MEM;
    return 0;
}

/* an expression with no name in it: a number (GNU's: a bare name is
 * memory, a bare number an immediate) */
static int numeric(const char *s, int n)
{
    for (int k = 0; k < n; k++) {
        if (isalpha((unsigned char)s[k]) || s[k] == '_' || s[k] == '.') {
            /* (0x1f and 10h are numbers) */
            if (k > 0 && isdigit((unsigned char)s[k - 1])) {
                while (k < n && isalnum((unsigned char)s[k])) k++;
                k--;
                continue;
            }
            return 0;
        }
    }
    return 1;
}

static int parse_core(const char *s, int n, const struct x86intel_opts *o, int transfer,
                      struct iop *op)
{
    memset(op, 0, sizeof *op);
    op->stn = -1;
    trim(&s, &n);
    if (!n) return ifail("an empty operand");
    for (;;) {
        int w = word_len(s, n), sz;
        if (!w) break;
        if ((sz = size_kw(s, w)) && w < n &&
            (isspace((unsigned char)s[w]) || s[w] == '[')) op->kw = sz;
        else if (word_is(s, n, "ptr")) ;
        else if (word_is(s, n, "far")) op->far = 1;
        else if (word_is(s, n, "near")) op->near_ = 1;
        else if (word_is(s, n, "short")) op->short_ = 1;
        else if (word_is(s, n, "strict")) ;
        else if (word_is(s, n, "offset") && o->flavor == X86I_GNU) op->offset = 1;
        else if (word_is(s, n, "flat") && w < n && s[w] == ':') { w++; }
        else break;
        s += w; n -= w;
        trim(&s, &n);
    }
    if (!n) return ifail("an operand with no value");
    /* a segment before the bracket (es:[di]), or GNU's bare fs:0x28 */
    {
        int w = word_len(s, n), sz, st;
        char rn[16];
        const char *p = s;
        int pn = n;
        if (pn > 0 && *p == '%') { p++; pn--; w = word_len(p, pn); }
        if (w == 2 && w < pn && p[w] == ':' &&
            reg_of(s, w + (int)(p - s), o, &sz, &st, rn, sizeof rn) == X86R_SEG) {
            const char *r = p + w + 1;
            int rn2 = pn - w - 1;
            trim(&r, &rn2);
            if (rn2 > 0 && r[0] == '[' && r[rn2 - 1] == ']')
                return parse_mem(r + 1, rn2 - 2, o, rn, op);
            if (o->flavor == X86I_GNU)
                return parse_mem(r, rn2, o, rn, op);
        }
    }
    if (s[0] == '[') {
        if (s[n - 1] != ']') return ifail("'%.*s': a ']' should end it", n, s);
        return parse_mem(s + 1, n - 2, o, NULL, op);
    }
    /* GNU's sym[rax+4]: the displacement before the bracket */
    if (s[n - 1] == ']' && memchr(s, '[', (size_t)n)) {
        const char *lb = memchr(s, '[', (size_t)n);
        char in[400];
        snprintf(in, sizeof in, "%.*s+(%.*s)", (int)(s + n - 1 - lb - 1), lb + 1,
                 (int)(lb - s), s);
        return parse_mem(in, (int)strlen(in), o, NULL, op);
    }
    {
        int sz, st;
        int c = reg_of(s, n, o, &sz, &st, op->t, (int)sizeof op->t);
        if (c) {
            op->kind = O_REG; op->cls = c; op->rsize = sz; op->stn = st;
            return 0;
        }
    }
    /* sel:off, a far pointer (jmp 0x08:start) */
    if (transfer) {
        int depth = 0;
        for (int k = 0; k < n; k++) {
            if (s[k] == '(') depth++;
            else if (s[k] == ')') depth--;
            else if (s[k] == ':' && depth == 0) {
                const char *a = s, *b = s + k + 1;
                int an = k, bn = n - k - 1;
                trim(&a, &an); trim(&b, &bn);
                op->kind = O_FAR;
                snprintf(op->t, sizeof op->t, "$%.*s", an, a);
                snprintf(op->t2, sizeof op->t2, "$%.*s", bn, b);
                return 0;
            }
        }
        op->kind = O_LBL;
        snprintf(op->t, sizeof op->t, "%.*s", n, s);
        return 0;
    }
    if (o->flavor == X86I_GNU && !op->offset && !numeric(s, n))
        return parse_mem(s, n, o, NULL, op);
    op->kind = O_IMM;
    snprintf(op->t, sizeof op->t, "$%.*s", n, s);
    return 0;
}

/* AVX-512's decorations: an operand's {k1} {z} {1to16} after it, kept
 * for its AT&T spelling; {rn-sae} or {sae} an operand of its own */
static int parse_op(const char *s, int n, const struct x86intel_opts *o, int transfer,
                    struct iop *op)
{
    trim(&s, &n);
    if (n >= 2 && s[0] == '{' && s[n - 1] == '}' && !memchr(s + 1, '{', (size_t)n - 1)) {
        memset(op, 0, sizeof *op);
        op->stn = -1;
        op->kind = O_IMM + 100;          /* (written as it is) */
        snprintf(op->t, sizeof op->t, "%.*s", n, s);
        for (char *c = op->t; *c; c++) *c = (char)tolower((unsigned char)*c);
        return 0;
    }
    char dec[128] = "";
    while (n > 0 && s[n - 1] == '}') {
        int k = n - 1;
        while (k > 0 && s[k] != '{') k--;
        if (s[k] != '{') break;
        char d[64];
        snprintf(d, sizeof d, "%.*s", n - k - 2, s + k + 1);
        for (char *c = d; *c; c++) *c = (char)tolower((unsigned char)*c);
        char t[160];
        if (d[0] == 'k' && d[1] >= '0' && d[1] <= '7' && !d[2])
            snprintf(t, sizeof t, " {%%%s}%s", d, dec);
        else if (!strncmp(d, "1to", 3))
            snprintf(t, sizeof t, "{%s}%s", d, dec);
        else
            snprintf(t, sizeof t, " {%s}%s", d, dec);
        snprintf(dec, sizeof dec, "%s", t);
        n = k;
        while (n > 0 && isspace((unsigned char)s[n - 1])) n--;
    }
    if (parse_core(s, n, o, transfer, op) < 0) return -1;
    if (dec[0]) {
        size_t l = strlen(op->t);
        snprintf(op->t + l, sizeof op->t - l, "%s", dec);
    }
    return 0;
}

static int is_transfer(const char *mn)
{
    static const char *const cc[] = {
        "o","no","b","c","nae","ae","nb","nc","e","z","ne","nz","be","na","a","nbe",
        "s","ns","p","pe","np","po","l","nge","ge","nl","le","ng","g","nle" };
    if (!strcmp(mn, "jmp") || !strcmp(mn, "call") || !strncmp(mn, "loop", 4) ||
        !strcmp(mn, "jcxz") || !strcmp(mn, "jecxz") || !strcmp(mn, "jrcxz"))
        return 1;
    if (mn[0] == 'j')
        for (unsigned k = 0; k < sizeof cc / sizeof *cc; k++)
            if (!strcmp(mn + 1, cc[k])) return 1;
    return 0;
}

static char sfx_of(int sz)
{
    return sz == 1 ? 'b' : sz == 2 ? 'w' : sz == 4 ? 'l' : sz == 8 ? 'q' : 0;
}

int x86intel_to_att(const char *stmt, const struct x86intel_opts *o,
                    char *out, int cap, char *err, int errlen)
{
    g_e = err; g_el = errlen;
    err[0] = 0;
    char pre[64] = "", mn[40];
    int osz = 0;                    /* NASM's o16/o32/o64 */
    const char *p = skip_sp(stmt);
    for (;;) {                      /* the prefixes */
        int w = 0;
        while (p[w] && !isspace((unsigned char)p[w])) w++;
        if (!w) {
            if (!pre[0]) return ifail("no instruction");
            /* a prefix alone (`lock` on a line of its own) */
            snprintf(out, (size_t)cap, "%s%s", o->flavor == X86I_NASM ? "{nasm} " : "", pre);
            return 0;
        }
        static const char *const pf[] = {
            "lock", "rep", "repe", "repz", "repne", "repnz", "data16", "data32",
            "addr16", "addr32" };
        int hit = 0;
        for (unsigned k = 0; k < sizeof pf / sizeof *pf; k++)
            if (word_is(p, w, pf[k])) {
                size_t l = strlen(pre);
                snprintf(pre + l, sizeof pre - l, "%s ", pf[k]);
                hit = 1;
            }
        if (!hit && o->flavor == X86I_NASM) {
            if (word_is(p, w, "o16")) { osz = 2; hit = 1; }
            else if (word_is(p, w, "o32")) { osz = 4; hit = 1; }
            else if (word_is(p, w, "o64")) { osz = 8; hit = 1; }
            else if (word_is(p, w, "a16") || word_is(p, w, "a32") || word_is(p, w, "a64")) {
                int a = p[1] == '1' ? 16 : p[1] == '3' ? 32 : 64;
                if (a != o->mode) {
                    size_t l = strlen(pre);
                    snprintf(pre + l, sizeof pre - l, "addr32 ");
                }
                hit = 1;
            }
        }
        if (!hit) {
            if (w >= (int)sizeof mn) return ifail("'%.*s' is too long for a mnemonic", w, p);
            for (int k = 0; k < w; k++) mn[k] = (char)tolower((unsigned char)p[k]);
            mn[w] = 0;
            p += w;
            break;
        }
        p = skip_sp(p + w);
    }
    /* the operands, split at the commas outside brackets and quotes */
    struct iop op[6];
    int n = 0;
    int transfer = is_transfer(mn);
    p = skip_sp(p);
    if (*p) {
        int depth = 0, q = 0;
        const char *st = p;
        for (const char *k = p;; k++) {
            if (*k && q) { if (*k == q) q = 0; continue; }
            if (*k == '\'' || *k == '"' || *k == '`') { q = *k; continue; }
            if (*k == '[' || *k == '(' || *k == '{') depth++;
            if (*k == ']' || *k == ')' || *k == '}') depth--;
            if (!*k || (*k == ',' && depth == 0)) {
                if (n == 6) return ifail("too many operands");
                if (parse_op(st, (int)(k - st), o, transfer, &op[n]) < 0) return -1;
                n++;
                if (!*k) break;
                st = k + 1;
            }
        }
    }
    /* the renamings */
    static const struct { const char *i, *a; } ren[] = {
        {"cbw","cbtw"},{"cwde","cwtl"},{"cdqe","cltq"},{"cwd","cwtd"},{"cdq","cltd"},
        {"cqo","cqto"},{"movsxd","movslq"},{"pushfd","pushfl"},{"popfd","popfl"},
        {"pushad","pushal"},{"popad","popal"},{"iretd","iretl"},{"retf","lret"},
        {"retn","ret"},{"stosd","stosl"},{"lodsd","lodsl"},{"scasd","scasl"},
        {"insd","insl"},{"outsd","outsl"},{"retfw","lretw"},{"retfd","lretl"},
        {"retfq","lretq"},{"sysret","sysretl"},{"jmpf","ljmp"},{"callf","lcall"},
    };
    for (unsigned k = 0; k < sizeof ren / sizeof *ren; k++)
        if (!strcmp(mn, ren[k].i)) { snprintf(mn, sizeof mn, "%s", ren[k].a); break; }
    if (n == 0 && (!strcmp(mn, "movsd") || !strcmp(mn, "cmpsd")))
        mn[4] = 'l';
    if (!strcmp(mn, "sysretl") && osz == 8) snprintf(mn, sizeof mn, "sysretq");
    /* sizes: a keyword's, where no register says it */
    int kw = 0, gpr = 0, vec = 0;
    int shift = !strcmp(mn, "shl") || !strcmp(mn, "shr") || !strcmp(mn, "sal") ||
                !strcmp(mn, "sar") || !strcmp(mn, "rol") || !strcmp(mn, "ror") ||
                !strcmp(mn, "rcl") || !strcmp(mn, "rcr");
    for (int k = 0; k < n; k++) {
        if ((op[k].kind == O_MEM || op[k].kind == O_IMM) && op[k].kw && !kw) kw = op[k].kw;
        if (op[k].kind == O_REG && op[k].cls == X86R_GPR && !(shift && k == 1))
            gpr = 1;
        if (op[k].kind == O_REG && (op[k].cls == X86R_VEC || op[k].cls == X86R_MASK))
            vec = 1;
    }
    char mo[48];
    snprintf(mo, sizeof mo, "%s", mn);
    int far = 0;
    for (int k = 0; k < n; k++)
        if (op[k].kind == O_FAR || (op[k].far && op[k].kind == O_MEM)) far = 1;
    if (transfer && far && (!strcmp(mn, "jmp") || !strcmp(mn, "call"))) {
        snprintf(mo, sizeof mo, "%s", mn[0] == 'j' ? "ljmp" : "lcall");
        int z = osz ? osz : (op[0].kind == O_FAR && op[0].kw ? op[0].kw : 0);
        if (z == 2 || z == 4) { size_t l = strlen(mo); mo[l] = sfx_of(z); mo[l + 1] = 0; }
    } else if (!strcmp(mn, "movzx") || !strcmp(mn, "movsx")) {
        if (n != 2 || op[0].kind != O_REG) return ifail("'%s' takes a register and a register or memory", mn);
        int from = op[1].kind == O_REG ? op[1].rsize : op[1].kw;
        if (!from) return ifail("'%s': the source's size is not given (byte or word)", mn);
        snprintf(mo, sizeof mo, "mov%c%c%c", mn[3] == 'z' ? 'z' : 's', sfx_of(from),
                 sfx_of(op[0].rsize));
    } else if (mn[0] == 'f' && !vec) {
        /* x87: a memory operand's size in the mnemonic (flds, fildl) */
        static const char *const fixed[] = {
            "fstcw","fnstcw","fldcw","fstsw","fnstsw","fstenv","fnstenv","fldenv",
            "fsave","fnsave","frstor","fbld","fbstp","fxsave","fxrstor","fxsave64",
            "fxrstor64", NULL };
        int fx = 0;
        for (int k = 0; fixed[k]; k++) if (!strcmp(mn, fixed[k])) fx = 1;
        int memk = -1;
        for (int k = 0; k < n; k++) if (op[k].kind == O_MEM) memk = k;
        if (!fx && memk >= 0 && op[memk].kw) {
            int z = op[memk].kw;
            const char *sf = mn[1] == 'i' ? (z == 2 ? "s" : z == 4 ? "l" : z == 8 ? "ll" : NULL)
                                          : (z == 4 ? "s" : z == 8 ? "l" : z == 10 ? "t" : NULL);
            if (!sf) return ifail("'%s' takes no %d-byte memory", mn, z);
            snprintf(mo, sizeof mo, "%s%s", mn, sf);
        }
        /* AT&T's fsub/fsubr and fdiv/fdivr are the other way round when
         * the destination is not st(0) (the System V spelling GNU as and
         * llvm-mc keep) */
        if (n == 2 && op[0].kind == O_REG && op[0].cls == CLS_ST && op[0].stn > 0 &&
            (!strncmp(mn, "fsub", 4) || !strncmp(mn, "fdiv", 4))) {
            char b[48];
            int r = mn[4] == 'r';
            int pp = mn[4 + r] == 'p';
            snprintf(b, sizeof b, "%.4s%s%s", mn, r ? "" : "r", pp ? "p" : "");
            snprintf(mo, sizeof mo, "%s", b);
        }
    } else if (!strncmp(mn, "cvtsi2s", 7) || !strncmp(mn, "vcvtsi2s", 8) ||
               !strncmp(mn, "cvtusi2s", 8) || !strncmp(mn, "vcvtusi2s", 9)) {
        if (kw == 4 || kw == 8) { size_t l = strlen(mo); mo[l] = sfx_of(kw); mo[l + 1] = 0; }
    } else if (!gpr && !vec && !transfer) {
        static const char *const nos[] = {
            "lgdt","lidt","sgdt","sidt","invlpg","clflush","clflushopt","clwb",
            "fxsave","fxrstor","xsave","xrstor","xsaveopt","prefetchnta","prefetcht0",
            "prefetcht1","prefetcht2","prefetchw","prefetch","ldmxcsr","stmxcsr",
            "cmpxchg8b","cmpxchg16b","lea","nop","int","ret","lret","enter",
            "aam","aad", NULL };
        int no = 0;
        for (int k = 0; nos[k]; k++) if (!strcmp(mn, nos[k])) no = 1;
        if (!strncmp(mn, "set", 3) || !strncmp(mn, "cmov", 4)) no = 1;
        int z = kw ? kw : 0;
        /* o16/o32 where nothing else gives a size */
        static const char *const ozs[] = {
            "iret","lret","ret","pushf","popf","pusha","popa","push","pop", NULL };
        if (osz && !z)
            for (int k = 0; ozs[k]; k++) if (!strcmp(mo, ozs[k])) { z = osz; no = 0; }
        if (z && !no && sfx_of(z)) {
            size_t l = strlen(mo);
            mo[l] = sfx_of(z); mo[l + 1] = 0;
        }
    }
    /* the statement: AT&T's order (reversed), enter's and a far pointer's
     * kept */
    int len = snprintf(out, (size_t)cap, "%s%s%s", o->flavor == X86I_NASM ? "{nasm} " : "",
                       pre, mo);
    int keep = !strcmp(mn, "enter");
    for (int j = 0; j < n; j++) {
        struct iop *q = &op[keep ? j : n - 1 - j];
        const char *star = transfer && (q->kind == O_REG || q->kind == O_MEM) ? "*" : "";
        if (q->near_ && transfer && len < cap) {
            /* `near jmp x`: the near form whatever it reaches */
            char t[512];
            snprintf(t, sizeof t, "near %s", out);
            len = snprintf(out, (size_t)cap, "%s", t);
        }
        if (q->kind == O_FAR)
            len += snprintf(out + len, (size_t)(cap > len ? cap - len : 0), "%s%s, %s",
                            j ? ", " : " ", q->t, q->t2);
        else
            len += snprintf(out + len, (size_t)(cap > len ? cap - len : 0), "%s%s%s",
                            j ? ", " : " ", star, q->t);
    }
    if (len >= cap) return ifail("the statement is too long");
    return 0;
}
