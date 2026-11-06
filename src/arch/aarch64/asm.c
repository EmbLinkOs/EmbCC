/* The aarch64 inline-asm assembler — see asm.h.
 *
 * Every encoding is refereed against aarch64-elf-as by
 * tests/golden/aarch64/arm64-asm.sh, which assembles the whole vocabulary — every
 * system register in the table, every tlbi, dc and ic operation, and each of the ARM
 * kernel's own templates — with both and compares the bytes.
 */
#include "asm.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emit.h"
#include "../../elf/elf.h"

/* ---- the vocabulary -------------------------------------------------- */

/* System registers by name. Anything else can still be written in the
 * generic S<op0>_<op1>_C<n>_C<m>_<op2> form, which the kernel uses for the
 * GIC's ICC_* registers. */
static const struct { const char *name; int enc; } sysregs[] = {
    { "nzcv",             A64_SYSREG(3, 3,  4, 2, 0) },
    { "daif",             A64_SYSREG(3, 3,  4, 2, 1) },
    { "fpcr",             A64_SYSREG(3, 3,  4, 4, 0) },   /* (newlib's fenv.h) */
    { "fpsr",             A64_SYSREG(3, 3,  4, 4, 1) },
    { "currentel",        A64_SYSREG(3, 0,  4, 2, 2) },
    { "pan",              A64_SYSREG(3, 0,  4, 2, 3) },
    { "spsel",            A64_SYSREG(3, 0,  4, 2, 0) },
    { "sp_el0",           A64_SYSREG(3, 0,  4, 1, 0) },
    { "spsr_el1",         A64_SYSREG(3, 0,  4, 0, 0) },
    { "elr_el1",          A64_SYSREG(3, 0,  4, 0, 1) },
    { "esr_el1",          A64_SYSREG(3, 0,  5, 2, 0) },
    { "far_el1",          A64_SYSREG(3, 0,  6, 0, 0) },
    { "par_el1",          A64_SYSREG(3, 0,  7, 4, 0) },
    { "midr_el1",         A64_SYSREG(3, 0,  0, 0, 0) },
    /* the caches' geometry: CTR_EL0's line sizes are what a
     * __clear_cache steps by (lib/rt/clear_cache.c) */
    { "ctr_el0",          A64_SYSREG(3, 3,  0, 0, 1) },
    { "dczid_el0",        A64_SYSREG(3, 3,  0, 0, 7) },
    { "clidr_el1",        A64_SYSREG(3, 1,  0, 0, 1) },
    { "ccsidr_el1",       A64_SYSREG(3, 1,  0, 0, 0) },
    { "csselr_el1",       A64_SYSREG(3, 2,  0, 0, 0) },
    { "mpidr_el1",        A64_SYSREG(3, 0,  0, 0, 5) },
    { "id_aa64pfr0_el1",  A64_SYSREG(3, 0,  0, 4, 0) },
    { "id_aa64pfr1_el1",  A64_SYSREG(3, 0,  0, 4, 1) },
    { "id_aa64isar0_el1", A64_SYSREG(3, 0,  0, 6, 0) },
    { "id_aa64isar1_el1", A64_SYSREG(3, 0,  0, 6, 1) },
    { "id_aa64mmfr0_el1", A64_SYSREG(3, 0,  0, 7, 0) },
    { "id_aa64mmfr1_el1", A64_SYSREG(3, 0,  0, 7, 1) },
    { "id_aa64mmfr2_el1", A64_SYSREG(3, 0,  0, 7, 2) },
    { "sctlr_el1",        A64_SYSREG(3, 0,  1, 0, 0) },
    { "cpacr_el1",        A64_SYSREG(3, 0,  1, 0, 2) },
    { "ttbr0_el1",        A64_SYSREG(3, 0,  2, 0, 0) },
    { "ttbr1_el1",        A64_SYSREG(3, 0,  2, 0, 1) },
    { "tcr_el1",          A64_SYSREG(3, 0,  2, 0, 2) },
    { "mair_el1",         A64_SYSREG(3, 0, 10, 2, 0) },
    { "vbar_el1",         A64_SYSREG(3, 0, 12, 0, 0) },
    { "contextidr_el1",   A64_SYSREG(3, 0, 13, 0, 1) },
    { "tpidr_el1",        A64_SYSREG(3, 0, 13, 0, 4) },
    { "tpidr_el0",        A64_SYSREG(3, 3, 13, 0, 2) },
    { "tpidrro_el0",      A64_SYSREG(3, 3, 13, 0, 3) },
    { "cntfrq_el0",       A64_SYSREG(3, 3, 14, 0, 0) },
    { "cntpct_el0",       A64_SYSREG(3, 3, 14, 0, 1) },
    { "cntvct_el0",       A64_SYSREG(3, 3, 14, 0, 2) },
    { "cntp_tval_el0",    A64_SYSREG(3, 3, 14, 2, 0) },
    { "cntp_ctl_el0",     A64_SYSREG(3, 3, 14, 2, 1) },
    { "cntp_cval_el0",    A64_SYSREG(3, 3, 14, 2, 2) },
    { "cntv_tval_el0",    A64_SYSREG(3, 3, 14, 3, 0) },
    { "cntv_ctl_el0",     A64_SYSREG(3, 3, 14, 3, 1) },
    { "cntv_cval_el0",    A64_SYSREG(3, 3, 14, 3, 2) },
    { "cntkctl_el1",      A64_SYSREG(3, 0, 14, 1, 0) },
};

/* TLB maintenance: SYS #op1, C8, Cm, #op2, Xt. `reg` says whether the
 * operation takes an address/ASID register or stands alone. */
static const struct { const char *name; int op1, crm, op2, reg; } tlbis[] = {
    { "vmalle1",   0, 7, 0, 0 }, { "vmalle1is", 0, 3, 0, 0 },
    { "vae1",      0, 7, 1, 1 }, { "vae1is",    0, 3, 1, 1 },
    { "aside1",    0, 7, 2, 1 }, { "aside1is",  0, 3, 2, 1 },
    { "vaae1",     0, 7, 3, 1 }, { "vaae1is",   0, 3, 3, 1 },
    { "vale1",     0, 7, 5, 1 }, { "vale1is",   0, 3, 5, 1 },
    { "vaale1",    0, 7, 7, 1 }, { "vaale1is",  0, 3, 7, 1 },
};

/* Cache maintenance: SYS #op1, C7, Cm, #op2, Xt. `dc` cleans or
 * invalidates data cache lines by address (or by set/way), `ic` the
 * instruction cache; `reg` as for tlbi. */
static const struct { const char *mn, *name; int op1, crm, op2, reg; } cmaints[] = {
    { "dc", "ivac",  0, 6, 1, 1 },  { "dc", "isw",   0, 6, 2, 1 },
    { "dc", "csw",   0, 10, 2, 1 }, { "dc", "cisw",  0, 14, 2, 1 },
    { "dc", "zva",   3, 4, 1, 1 },  { "dc", "cvac",  3, 10, 1, 1 },
    { "dc", "cvau",  3, 11, 1, 1 }, { "dc", "cvap",  3, 12, 1, 1 },
    { "dc", "civac", 3, 14, 1, 1 },
    { "ic", "ialluis", 0, 1, 0, 0 }, { "ic", "iallu", 0, 5, 0, 0 },
    { "ic", "ivau",  3, 5, 1, 1 },
};

/* Barrier options, in their CRm encoding. */
static const struct { const char *name; int crm; } barrier_opts[] = {
    { "sy", 15 }, { "st", 14 }, { "ld", 13 },
    { "ish", 11 }, { "ishst", 10 }, { "ishld", 9 },
    { "nsh", 7 },  { "nshst", 6 },  { "nshld", 5 },
    { "osh", 3 },  { "oshst", 2 },  { "oshld", 1 },
};

static const struct { const char *name; int imm; } hints[] = {
    { "nop", 0 }, { "yield", 1 }, { "wfe", 2 }, { "wfi", 3 },
    { "sev", 4 }, { "sevl", 5 },
};

/* ---- error reporting ------------------------------------------------- */

struct actx {
    struct code *out;
    char *err;
    int errlen;
    const char *stmt;     /* the statement being assembled, for messages */
    int failed;
};

static void fail(struct actx *a, const char *fmt, ...)
{
    if (a->failed)
        return;
    a->failed = 1;
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    snprintf(a->err, (size_t)a->errlen, "%s (in \"%s\")", msg, a->stmt);
}

/* ---- lexical helpers ------------------------------------------------- */

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
    return *a == *b;
}

int a64asm_gpr(const char *name, int len)
{
    if (len == 3 && (strncmp(name, "xzr", 3) == 0 || strncmp(name, "wzr", 3) == 0))
        return 31;
    if (len < 2 || len > 3 || (name[0] != 'x' && name[0] != 'w'))
        return -1;
    int n = 0;
    for (int i = 1; i < len; i++) {
        if (!isdigit((unsigned char)name[i]))
            return -1;
        n = n * 10 + (name[i] - '0');
    }
    return n <= 30 ? n : -1;
}

/* A general register operand. *w is set to 1 for a W name. */
static int gpr(struct actx *a, const char *s, int *w)
{
    int r = a64asm_gpr(s, (int)strlen(s));
    if (r < 0) {
        fail(a, "expected a general register, got '%s'", s);
        return 0;
    }
    if (w)
        *w = s[0] == 'w';
    return r;
}

/* An immediate: '#' optional, decimal or 0x hex, possibly negative. */
static long imm(struct actx *a, const char *s)
{
    if (*s == '#')
        s++;
    /* `.+8` / `.-12`: a displacement from this instruction, which is
     * what the file assembler (src/as/gas.c) turns a label into
     * before calling. This layer sees no labels and does not know its
     * own address, so a branch operand can mean nothing else. */
    if (*s == '.' && (s[1] == '+' || s[1] == '-'))
        s++;
    char *end;
    long v = strtol(s, &end, 0);
    if (end == s || *end) {
        fail(a, "expected an immediate, got '%s'", s);
        return 0;
    }
    return v;
}

static int sysreg(struct actx *a, const char *s)
{
    for (unsigned i = 0; i < sizeof sysregs / sizeof sysregs[0]; i++)
        if (ieq(s, sysregs[i].name))
            return sysregs[i].enc;
    /* S<op0>_<op1>_C<n>_C<m>_<op2> */
    int op0, op1, crn, crm, op2;
    char tail;
    if ((s[0] == 's' || s[0] == 'S') &&
        sscanf(s + 1, "%d_%d_%*[cC]%d_%*[cC]%d_%d%c",
               &op0, &op1, &crn, &crm, &op2, &tail) == 5 &&
        op0 >= 2 && op0 <= 3 && op1 >= 0 && op1 <= 7 &&
        crn >= 0 && crn <= 15 && crm >= 0 && crm <= 15 && op2 >= 0 && op2 <= 7)
        return A64_SYSREG(op0, op1, crn, crm, op2);
    fail(a, "unknown system register '%s'", s);
    return 0;
}

/* Splits `s` into comma-separated operands at bracket depth 0, trimming
 * each. Returns the count; operands are written into buf. */
static int split_operands(char *s, char **ops, int maxops)
{
    int n = 0, depth = 0;
    char *start = s;
    for (char *p = s;; p++) {
        if (*p == '[' || *p == '{') depth++;
        else if (*p == ']' || *p == '}') depth--;
        if ((*p == ',' && depth == 0) || *p == '\0') {
            int end = *p == '\0';
            *p = '\0';
            while (isspace((unsigned char)*start)) start++;
            char *e = start + strlen(start);
            while (e > start && isspace((unsigned char)e[-1])) *--e = '\0';
            if (*start && n < maxops)
                ops[n++] = start;
            if (end)
                break;
            start = p + 1;
        }
    }
    return n;
}

/* ---- the general instructions ------------------------------------------
 *
 * Everything a compiler writes for A64 -- the base instruction set, the
 * scalar floating point, the Advanced SIMD a vectorizer reaches for, the
 * exclusive, acquire/release and LSE atomic memory operations -- encoded
 * from the Arm ARM's tables, and refereed against llvm-mc over clang's
 * own output for lib/libc and lib/rt (tools/asm-audit) and against
 * aarch64-elf-as for the forms listed in tests/golden/aarch64-asm-full.sh.
 *
 * An operand is parsed once into a struct op; each instruction class then
 * asks for the shapes it takes and refuses, by name, the ones it does
 * not. A symbol never reaches here: a64asm_symform rewrites a statement
 * naming one into the same statement with a zero, and gas.c relocates. */

enum { K_NONE, K_GPR, K_FPR, K_VEC, K_ELEM, K_LIST, K_IMM, K_FIMM, K_SHIFT,
       K_EXT, K_MEM, K_COND, K_WORD };

struct op {
    int k;
    int r;               /* register number; 31 for sp/xzr */
    int x;               /* GPR: a 64-bit name */
    int sp;              /* GPR: written sp or wsp */
    int fsz;             /* FPR: 1 b, 2 h, 4 s, 8 d, 16 q */
    int esz, nel;        /* VEC/ELEM/LIST: element bytes, elements (VEC) */
    int idx;             /* ELEM, and a LIST's lane (-1 none) */
    int nregs;           /* LIST */
    long long v;         /* IMM, a SHIFT's or EXT's amount, MEM's offset */
    int pcrel;           /* IMM written `.+N`: a displacement */
    double f;            /* FIMM */
    int sh;              /* SHIFT: 0 lsl 1 lsr 2 asr 3 ror 4 msl */
    int ext, has_amt;    /* EXT: 0..7 = uxtb uxth uxtw uxtx sxtb sxth sxtw sxtx */
    /* MEM: [base], [base, #off], [base, #off]!, [base], #off (post, set
     * by the caller), [base, Rm{, ext {#amt}}] */
    int base, mode, rm, rmx, rext, ramt, ramt_given;
    int cond;
    char word[24];
};
enum { M_BASE, M_OFF, M_PRE, M_POST, M_REG, M_POSTREG };

static const char *const cond_names[] = {
    "eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc",
    "hi", "ls", "ge", "lt", "gt", "le", "al", "nv"
};

static int cond_of(const char *s)
{
    for (int i = 0; i < 16; i++)
        if (ieq(s, cond_names[i]))
            return i;
    if (ieq(s, "hs")) return 2;
    if (ieq(s, "lo")) return 3;
    return -1;
}

static const char *const ext_names[] = {
    "uxtb", "uxth", "uxtw", "uxtx", "sxtb", "sxth", "sxtw", "sxtx"
};

/* `v12.4s` -> 12, 4, 4; `v3.s` -> 3, 4, 0 (an element's size alone) */
static int vec_name(const char *s, int *r, int *esz, int *nel)
{
    if ((s[0] != 'v' && s[0] != 'V') || !isdigit((unsigned char)s[1]))
        return 0;
    char *e;
    long n = strtol(s + 1, &e, 10);
    if (n < 0 || n > 31 || *e != '.')
        return 0;
    e++;
    int cnt = 0;
    while (isdigit((unsigned char)*e))
        cnt = cnt * 10 + (*e++ - '0');
    int sz = tolower((unsigned char)*e) == 'b' ? 1 : tolower((unsigned char)*e) == 'h' ? 2
           : tolower((unsigned char)*e) == 's' ? 4 : tolower((unsigned char)*e) == 'd' ? 8
           : tolower((unsigned char)*e) == 'q' ? 16 : 0;
    if (!sz || e[1])
        return 0;
    if (cnt && cnt * sz != 8 && cnt * sz != 16 && !(cnt == 1 && sz == 16))
        return 0;
    *r = (int)n; *esz = sz; *nel = cnt;
    return 1;
}

static int fpr_name(const char *s, int *r, int *sz)
{
    int c = tolower((unsigned char)s[0]);
    int z = c == 'b' ? 1 : c == 'h' ? 2 : c == 's' ? 4 : c == 'd' ? 8 : c == 'q' ? 16 : 0;
    if (!z || !isdigit((unsigned char)s[1]))
        return 0;
    char *e;
    long n = strtol(s + 1, &e, 10);
    if (*e || n < 0 || n > 31)
        return 0;
    *r = (int)n; *sz = z;
    return 1;
}

static int gpr_name(const char *s, int *r, int *x, int *sp)
{
    *sp = 0;
    if (ieq(s, "sp"))  { *r = 31; *x = 1; *sp = 1; return 1; }
    if (ieq(s, "wsp")) { *r = 31; *x = 0; *sp = 1; return 1; }
    if (ieq(s, "lr"))  { *r = 30; *x = 1; return 1; }
    if (ieq(s, "fp"))  { *r = 29; *x = 1; return 1; }
    int n = a64asm_gpr(s, (int)strlen(s));
    if (n < 0) {
        char lc[8];
        size_t l = strlen(s);
        if (l >= sizeof lc) return 0;
        for (size_t i = 0; i <= l; i++) lc[i] = (char)tolower((unsigned char)s[i]);
        n = a64asm_gpr(lc, (int)l);
        if (n < 0) return 0;
        *x = lc[0] == 'x';
    } else {
        *x = s[0] == 'x';
    }
    *r = n;
    return 1;
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

/* An integer: `#` optional; decimal, 0x, 0b; `.+N`; a simple sum of them
 * (a substituted label can read `.+8+4`). */
static int int_val(const char *s, long long *v, int *pcrel)
{
    if (*s == '#') s++;
    while (isspace((unsigned char)*s)) s++;
    *pcrel = 0;
    if (*s == '.' && (s[1] == '+' || s[1] == '-')) { *pcrel = 1; s++; }
    long long acc = 0;
    int any = 0;
    while (*s) {
        int neg = 0;
        while (*s == '+' || *s == '-' || isspace((unsigned char)*s)) {
            if (*s == '-') neg = !neg;
            s++;
        }
        if (!*s) return 0;
        char *e;
        unsigned long long u;
        if (s[0] == '0' && (s[1] == 'b' || s[1] == 'B'))
            u = strtoull(s + 2, &e, 2);
        else
            u = strtoull(s, &e, 0);
        if (e == s) return 0;
        acc += neg ? -(long long)u : (long long)u;
        any = 1;
        s = e;
        while (isspace((unsigned char)*s)) s++;
        if (*s && *s != '+' && *s != '-') return 0;
    }
    if (!any) return 0;
    *v = acc;
    return 1;
}

static int parse_op(struct actx *a, char *s, struct op *o);

static int parse_mem(struct actx *a, char *s, struct op *o)
{
    size_t len = strlen(s);
    int pre = 0;
    if (len && s[len - 1] == '!') { pre = 1; s[--len] = 0; }
    if (len < 3 || s[0] != '[' || s[len - 1] != ']')
        return 0;
    s[len - 1] = 0;
    char *ops[4];
    int n = split_operands(s + 1, ops, 4);
    int x, sp;
    o->k = K_MEM;
    if (n < 1 || !gpr_name(ops[0], &o->base, &x, &sp) || !x ||
        (o->base == 31 && !sp)) {
        fail(a, "an address's base is an X register or sp, got '%s'", n ? ops[0] : "");
        return 1;
    }
    o->mode = M_BASE;
    o->v = 0;
    if (n == 1) {
        if (pre) { fail(a, "a pre-index needs an offset"); return 1; }
        return 1;
    }
    int rx, rsp;
    if (gpr_name(ops[1], &o->rm, &rx, &rsp) && !rsp) {
        o->mode = M_REG;
        o->rmx = rx;
        o->rext = rx ? 3 : 2;          /* lsl / uxtw unless said */
        o->ramt = 0;
        o->ramt_given = 0;
        if (n >= 3) {
            char *t = ops[2];
            char w[8];
            int i = 0;
            while (*t && !isspace((unsigned char)*t) && *t != '#' && i < 7) w[i++] = *t++;
            w[i] = 0;
            int e = -1;
            if (ieq(w, "lsl")) e = 3;
            for (int k = 0; k < 8; k++) if (ieq(w, ext_names[k])) e = k;
            if (e < 0) { fail(a, "'%s' is not an extend or lsl", ops[2]); return 1; }
            o->rext = e;
            t = trim(t);
            if (*t) {
                long long v; int pc;
                if (!int_val(t, &v, &pc)) { fail(a, "bad shift amount '%s'", t); return 1; }
                o->ramt = (int)v;
                o->ramt_given = 1;
            }
        }
        if (pre) fail(a, "a register offset has no pre-index form");
        return 1;
    }
    if (n != 2) { fail(a, "'[%s, ...]' has too many parts", ops[0]); return 1; }
    long long v; int pc;
    if (!int_val(ops[1], &v, &pc)) { fail(a, "bad address offset '%s'", ops[1]); return 1; }
    o->v = v;
    o->mode = pre ? M_PRE : M_OFF;
    return 1;
}

static int parse_list(struct actx *a, char *s, struct op *o)
{
    size_t len = strlen(s);
    char *close = strrchr(s, '}');
    if (s[0] != '{' || !close)
        return 0;
    o->idx = -1;
    if (close[1] == '[') {
        o->idx = atoi(close + 2);
        if (!strchr(close, ']')) { fail(a, "bad lane in '%s'", s); return 1; }
    }
    *close = 0;
    (void)len;
    char *body = s + 1;
    char *dash = strchr(body, '-');
    o->k = K_LIST;
    if (dash) {                                   /* {v0.4s - v3.4s} */
        *dash = 0;
        int r1, e1, n1, r2, e2, n2;
        if (!vec_name(trim(body), &r1, &e1, &n1) || !vec_name(trim(dash + 1), &r2, &e2, &n2)) {
            fail(a, "bad register list '%s'", s);
            return 1;
        }
        o->r = r1; o->esz = e1; o->nel = n1; o->nregs = (r2 - r1 + 32) % 32 + 1;
        return 1;
    }
    char *ops[5];
    int n = split_operands(body, ops, 5);
    for (int i = 0; i < n; i++) {
        int r, e, c;
        if (!vec_name(ops[i], &r, &e, &c)) { fail(a, "bad register list entry '%s'", ops[i]); return 1; }
        if (i == 0) { o->r = r; o->esz = e; o->nel = c; }
        else if (r != (o->r + i) % 32 || e != o->esz || c != o->nel) {
            fail(a, "a register list is consecutive registers of one arrangement");
            return 1;
        }
    }
    o->nregs = n;
    if (n < 1 || n > 4) fail(a, "a register list holds one to four registers");
    return 1;
}

static int parse_op(struct actx *a, char *s, struct op *o)
{
    memset(o, 0, sizeof *o);
    o->idx = -1;
    s = trim(s);
    if (*s == '[')
        return parse_mem(a, s, o);
    if (*s == '{')
        return parse_list(a, s, o);
    /* a lane: v1.s[2], v0.d[1] */
    char *br = strchr(s, '[');
    if (br && (s[0] == 'v' || s[0] == 'V')) {
        *br = 0;
        int r, e, c;
        if (!vec_name(s, &r, &e, &c) || c) { fail(a, "bad lane '%s'", s); return 1; }
        o->k = K_ELEM; o->r = r; o->esz = e; o->idx = atoi(br + 1);
        if (o->idx < 0 || o->idx * e >= 16) fail(a, "lane %d is outside the register", o->idx);
        return 1;
    }
    int x, sp;
    if (gpr_name(s, &o->r, &x, &sp)) { o->k = K_GPR; o->x = x; o->sp = sp; return 1; }
    if (fpr_name(s, &o->r, &o->fsz)) { o->k = K_FPR; return 1; }
    if (vec_name(s, &o->r, &o->esz, &o->nel)) {
        o->k = K_VEC;
        if (!o->nel) fail(a, "'%s' needs an element count", s);
        return 1;
    }
    /* shifts and extends: `lsl #3`, `uxtw`, `sxtw #2`, `msl #8` */
    {
        char w[8];
        int i = 0;
        const char *t = s;
        while (*t && isalpha((unsigned char)*t) && i < 7) w[i++] = (char)tolower((unsigned char)*t++);
        w[i] = 0;
        static const char *const shn[] = { "lsl", "lsr", "asr", "ror", "msl" };
        for (int k = 0; k < 5; k++)
            if (!strcmp(w, shn[k]) && (!*t || isspace((unsigned char)*t) || *t == '#')) {
                long long v; int pc;
                o->k = K_SHIFT; o->sh = k;
                if (!int_val(trim((char *)t), &v, &pc)) fail(a, "'%s' needs an amount", s);
                o->v = v;
                return 1;
            }
        for (int k = 0; k < 8; k++)
            if (!strcmp(w, ext_names[k]) && (!*t || isspace((unsigned char)*t) || *t == '#')) {
                o->k = K_EXT; o->ext = k;
                char *rest = trim((char *)t);
                if (*rest) {
                    long long v; int pc;
                    if (!int_val(rest, &v, &pc)) fail(a, "'%s': bad amount", s);
                    o->v = v; o->has_amt = 1;
                }
                return 1;
            }
    }
    int c = cond_of(s);
    if (c >= 0) { o->k = K_COND; o->cond = c; return 1; }
    long long v; int pc;
    if (int_val(s, &v, &pc)) { o->k = K_IMM; o->v = v; o->pcrel = pc; return 1; }
    /* a floating-point immediate: #1.0, #-2.5e0, #0.0 */
    {
        const char *t = s[0] == '#' ? s + 1 : s;
        char *e;
        double d = strtod(t, &e);
        if (e != t && !*e) { o->k = K_FIMM; o->f = d; return 1; }
    }
    o->k = K_WORD;
    snprintf(o->word, sizeof o->word, "%s", s);
    return 1;
}

/* ---- encoding helpers --------------------------------------------------- */

#define EMIT(w) a64_word(a->out, (unsigned long)(w))
#define BAD(...) do { fail(a, __VA_ARGS__); return 1; } while (0)

/* A64's logical immediate: N:immr:imms for a value that repeats one
 * rotated run of ones in an element of 2, 4, ..., 64 bits; -1 when it
 * does not. (The same test as a64_bitmask_ok, made to give the fields.) */
static int bitmask_enc(unsigned long long v, int is64)
{
    if (!is64) {
        v &= 0xffffffffULL;
        v |= v << 32;
    }
    if (v == 0 || v == ~0ULL)
        return -1;
    int size = 64;
    while (size > 2) {
        int h = size / 2;
        unsigned long long m = (1ULL << h) - 1;
        if ((v & m) != ((v >> h) & m))
            break;
        size = h;
    }
    unsigned long long mask = size == 64 ? ~0ULL : (1ULL << size) - 1;
    unsigned long long e = v & mask;
    /* rotate so the run of ones starts at bit 0 */
    int rot = 0;
    while (rot < size) {
        unsigned long long r = ((e >> rot) | (e << (size - rot))) & mask;
        if ((r & 1) && !((r >> (size - 1)) & 1)) {
            int ones = 0;
            while (ones < size && ((r >> ones) & 1)) ones++;
            unsigned long long want = ones == 64 ? ~0ULL : (1ULL << ones) - 1;
            if (r != want)
                return -1;
            int immr = rot;            /* ROR(element, immr) gives v */
            int imms = ((~(size - 1) << 1) | (ones - 1)) & 0x3f;
            int nbit = size == 64;
            /* immr is the right-rotation that turns the run into e */
            immr = (size - rot) % size;
            return nbit << 12 | immr << 6 | imms;
        }
        rot++;
    }
    return -1;
}

/* An 8-bit floating-point immediate (VFPExpandImm's inverse), or -1. */
static int fp8_enc(double d)
{
    for (int i = 0; i < 256; i++) {
        int s = i >> 7, e = (i >> 4) & 7, f = i & 15;
        double m = 1.0 + f / 16.0;
        int ex = (e ^ 4) - 3;          /* e = NOT(b6):b6:b6 ... */
        if (e & 4) ex = e - 7 + 0;     /* 100..111 -> -3..0 */
        else ex = e + 1;               /* 000..011 -> 1..4 */
        double val = m;
        for (int k = 0; k < (ex > 0 ? ex : -ex); k++)
            val = ex > 0 ? val * 2 : val / 2;
        if (s) val = -val;
        if (val == d)
            return i;
    }
    return -1;
}


/* ---- the integer instructions ------------------------------------------- */

/* add/sub/adds/subs with an immediate, shifted register or extended
 * register; op 0 add 1 sub, s the flags */
static int enc_addsub(struct actx *a, int sub, int s, struct op *o, int n)
{
    struct op *d = &o[0], *nn = &o[1], *m = &o[2];
    if (n < 3 || d->k != K_GPR || nn->k != K_GPR)
        BAD("expected two registers and an operand");
    int sf = d->x;
    if (nn->x != sf && m->k != K_GPR)
        BAD("the registers are not one width");
    unsigned base = (unsigned)sf << 31 | (unsigned)sub << 30 | (unsigned)s << 29;
    if (m->k == K_IMM) {
        long long v = m->v;
        int sh = 0;
        if (n == 4) {
            if (o[3].k != K_SHIFT || o[3].sh != 0 || (o[3].v != 0 && o[3].v != 12))
                BAD("an immediate takes lsl #0 or lsl #12");
            sh = o[3].v == 12;
        }
        if (v < 0 && !sh) {                    /* add x, y, #-4 is sub #4 */
            v = -v;
            base ^= 1u << 30;
        }
        if (!sh && v > 0xfff && !(v & 0xfff) && v <= 0xfff000) { v >>= 12; sh = 1; }
        if (v < 0 || v > 0xfff)
            BAD("immediate %lld does not fit 12 bits (shifted by 0 or 12)", m->v);
        if (d->r == 31 && !d->sp && !s) BAD("an add's destination 31 is sp, not xzr");
        EMIT(0x11000000u | base | (unsigned)sh << 22 | (unsigned)v << 10 |
             (unsigned)nn->r << 5 | (unsigned)d->r);
        return 1;
    }
    if (m->k != K_GPR)
        BAD("expected a register or an immediate");
    /* the extended form: when sp is involved, or an extend is written */
    int ext = n == 4 && o[3].k == K_EXT;
    if (ext || d->sp || nn->sp) {
        int opt = sf && m->x ? 3 : 2, amt = 0;   /* uxtx / uxtw, the lsl aliases */
        if (n == 4) {
            if (o[3].k == K_EXT) { opt = o[3].ext; amt = (int)o[3].v; }
            else if (o[3].k == K_SHIFT && o[3].sh == 0) amt = (int)o[3].v;
            else BAD("an extended register takes an extend or lsl");
        }
        if (amt < 0 || amt > 4) BAD("an extend's shift is 0 to 4");
        EMIT(0x0B200000u | base | (unsigned)m->r << 16 | (unsigned)opt << 13 |
             (unsigned)amt << 10 | (unsigned)nn->r << 5 | (unsigned)d->r);
        return 1;
    }
    int sh = 0, amt = 0;
    if (n == 4) {
        if (o[3].k != K_SHIFT || o[3].sh > 2) BAD("a shifted register takes lsl, lsr or asr");
        sh = o[3].sh; amt = (int)o[3].v;
    }
    if (amt < 0 || amt > (sf ? 63 : 31)) BAD("shift %d is outside the register", amt);
    EMIT(0x0B000000u | base | (unsigned)sh << 22 | (unsigned)m->r << 16 |
         (unsigned)amt << 10 | (unsigned)nn->r << 5 | (unsigned)d->r);
    return 1;
}

/* and/orr/eor/ands with a bitmask immediate or a shifted register;
 * inv: bic, orn, eon, bics */
static int enc_logical(struct actx *a, int opc, int inv, struct op *o, int n)
{
    struct op *d = &o[0], *nn = &o[1], *m = &o[2];
    if (n < 3 || d->k != K_GPR || nn->k != K_GPR)
        BAD("expected two registers and an operand");
    int sf = d->x;
    if (m->k == K_IMM) {
        unsigned long long v = (unsigned long long)m->v;
        if (inv) v = ~v;
        int e = bitmask_enc(v, sf);
        if (e < 0 || (!sf && (e >> 12)))
            BAD("%#llx is not a logical immediate", (unsigned long long)m->v);
        EMIT(0x12000000u | (unsigned)sf << 31 | (unsigned)opc << 29 | (unsigned)e << 10 |
             (unsigned)nn->r << 5 | (unsigned)d->r);
        return 1;
    }
    if (m->k != K_GPR) BAD("expected a register or an immediate");
    int sh = 0, amt = 0;
    if (n == 4) {
        if (o[3].k != K_SHIFT || o[3].sh > 3) BAD("a logical shift is lsl, lsr, asr or ror");
        sh = o[3].sh; amt = (int)o[3].v;
    }
    if (amt < 0 || amt > (sf ? 63 : 31)) BAD("shift %d is outside the register", amt);
    EMIT(0x0A000000u | (unsigned)sf << 31 | (unsigned)opc << 29 | (unsigned)sh << 22 |
         (unsigned)inv << 21 | (unsigned)m->r << 16 | (unsigned)amt << 10 |
         (unsigned)nn->r << 5 | (unsigned)d->r);
    return 1;
}

static void movw(struct actx *a, int sf, int opc, int rd, unsigned imm16, int hw)
{
    EMIT(0x12800000u | (unsigned)sf << 31 | (unsigned)opc << 29 | (unsigned)hw << 21 |
         imm16 << 5 | (unsigned)rd);
}

/* `mov rd, #imm`, as llvm-mc and GNU as choose it: movz, movn, or orr
 * with a logical immediate -- one instruction, or refused. */
static int enc_mov_imm(struct actx *a, struct op *d, long long v)
{
    int sf = d->x;
    unsigned long long u = (unsigned long long)v;
    if (!sf) {
        if (v < -2147483648LL || v > 0xffffffffLL)
            BAD("%lld does not fit a W register", v);
        u &= 0xffffffffULL;
    }
    int nh = sf ? 4 : 2;
    for (int hw = 0; hw < nh; hw++)              /* movz */
        if ((u & ~(0xffffULL << (16 * hw))) == 0 && (u || hw == 0)) {
            movw(a, sf, 2, d->r, (unsigned)(u >> (16 * hw)) & 0xffff, hw);
            return 1;
        }
    unsigned long long nu = sf ? ~u : (~u & 0xffffffffULL);
    for (int hw = 0; hw < nh; hw++)              /* movn */
        if ((nu & ~(0xffffULL << (16 * hw))) == 0) {
            if (!sf && ((nu >> (16 * hw)) & 0xffff) == 0xffff) continue;
            movw(a, sf, 0, d->r, (unsigned)(nu >> (16 * hw)) & 0xffff, hw);
            return 1;
        }
    int e = bitmask_enc(u, sf);
    if (e >= 0) {
        EMIT(0x32000000u | (unsigned)sf << 31 | (unsigned)e << 10 | 31u << 5 | (unsigned)d->r);
        return 1;
    }
    BAD("%#llx needs more than one instruction (write movz and movk)", u);
}

static int enc_bfm(struct actx *a, int opc, struct op *d, struct op *nn, int immr, int imms)
{
    int sf = d->x, w = sf ? 64 : 32;
    if (nn->k != K_GPR) BAD("expected a register");
    if (immr < 0 || immr >= w || imms < 0 || imms >= w) BAD("a bit position is outside the register");
    EMIT(0x13000000u | (unsigned)sf << 31 | (unsigned)opc << 29 | (unsigned)sf << 22 |
         (unsigned)immr << 16 | (unsigned)imms << 10 | (unsigned)nn->r << 5 | (unsigned)d->r);
    return 1;
}

static int enc_branch(struct actx *a, const char *mn, struct op *o, int n)
{
    if (!strcmp(mn, "b") || !strcmp(mn, "bl")) {
        if (n != 1 || o[0].k != K_IMM) BAD("'%s' takes a label", mn);
        long long v = o[0].v;
        if (v & 3 || v < -(1LL << 27) || v >= (1LL << 27)) BAD("'%s' reaches 128 MiB in words", mn);
        EMIT((mn[1] ? 0x94000000u : 0x14000000u) | ((unsigned)(v >> 2) & 0x3ffffff));
        return 1;
    }
    if (mn[0] == 'b' && mn[1] == '.') {
        int c = cond_of(mn + 2);
        if (c < 0) return 0;
        if (n != 1 || o[0].k != K_IMM) BAD("'%s' takes a label", mn);
        long long v = o[0].v;
        if (v & 3 || v < -(1 << 20) || v >= (1 << 20)) BAD("'%s' reaches 1 MiB in words", mn);
        EMIT(0x54000000u | ((unsigned)(v >> 2) & 0x7ffff) << 5 | (unsigned)c);
        return 1;
    }
    if (!strcmp(mn, "cbz") || !strcmp(mn, "cbnz")) {
        if (n != 2 || o[0].k != K_GPR || o[1].k != K_IMM) BAD("'%s' takes a register and a label", mn);
        long long v = o[1].v;
        if (v & 3 || v < -(1 << 20) || v >= (1 << 20)) BAD("'%s' reaches 1 MiB in words", mn);
        EMIT((mn[2] == 'n' ? 0x35000000u : 0x34000000u) | (unsigned)o[0].x << 31 |
             ((unsigned)(v >> 2) & 0x7ffff) << 5 | (unsigned)o[0].r);
        return 1;
    }
    if (!strcmp(mn, "tbz") || !strcmp(mn, "tbnz")) {
        if (n != 3 || o[0].k != K_GPR || o[1].k != K_IMM || o[2].k != K_IMM)
            BAD("'%s' takes a register, a bit and a label", mn);
        long long bit = o[1].v, v = o[2].v;
        if (bit < 0 || bit > (o[0].x ? 63 : 31)) BAD("bit %lld is outside the register", bit);
        if (v & 3 || v < -32768 || v >= 32768) BAD("'%s' reaches 32 KiB in words", mn);
        EMIT((mn[2] == 'n' ? 0x37000000u : 0x36000000u) | (unsigned)(bit >> 5) << 31 |
             (unsigned)(bit & 31) << 19 | ((unsigned)(v >> 2) & 0x3fff) << 5 | (unsigned)o[0].r);
        return 1;
    }
    if (!strcmp(mn, "br") || !strcmp(mn, "blr") || !strcmp(mn, "ret")) {
        int rn = 30;
        if (n == 1) {
            if (o[0].k != K_GPR || !o[0].x) BAD("'%s' takes an X register", mn);
            rn = o[0].r;
        } else if (n || mn[0] != 'r') BAD("'%s' takes an X register", mn);
        EMIT((mn[0] == 'r' ? 0xD65F0000u : mn[2] == 'r' ? 0xD63F0000u : 0xD61F0000u) | (unsigned)rn << 5);
        return 1;
    }
    if (!strcmp(mn, "eret") && !n) { EMIT(0xD69F03E0u); return 1; }
    if (!strcmp(mn, "clrex")) {
        long long crm = n ? o[0].v : 15;
        if (n > 1 || (n && o[0].k != K_IMM) || crm < 0 || crm > 15) BAD("'clrex' takes #0 to #15");
        EMIT(0xD503305Fu | (unsigned)crm << 8);
        return 1;
    }
    if (!strcmp(mn, "drps") && !n) { EMIT(0xD6BF03E0u); return 1; }
    return 0;
}

/* The load/store size and kind of a mnemonic: *size 0..3 (b h w x) or the
 * FP/SIMD register's, *opc, *v. 0 when it is not one. */
struct ldst { const char *mn; int size, opc, sign, store; };
static const struct ldst ldsts[] = {
    { "strb", 0, 0, 0, 1 }, { "ldrb", 0, 1, 0, 0 }, { "ldrsb", 0, 2, 1, 0 },
    { "strh", 1, 0, 0, 1 }, { "ldrh", 1, 1, 0, 0 }, { "ldrsh", 1, 2, 1, 0 },
    { "ldrsw", 2, 2, 1, 0 }, { "str", -1, 0, 0, 1 }, { "ldr", -1, 1, 0, 0 },
    { "prfm", 3, 2, 0, 0 },
};

static const struct { const char *name; int v; } prfops[] = {
    { "pldl1keep", 0 }, { "pldl1strm", 1 }, { "pldl2keep", 2 }, { "pldl2strm", 3 },
    { "pldl3keep", 4 }, { "pldl3strm", 5 }, { "plil1keep", 8 }, { "plil1strm", 9 },
    { "plil2keep", 10 }, { "plil2strm", 11 }, { "plil3keep", 12 }, { "plil3strm", 13 },
    { "pstl1keep", 16 }, { "pstl1strm", 17 }, { "pstl2keep", 18 }, { "pstl2strm", 19 },
    { "pstl3keep", 20 }, { "pstl3strm", 21 },
};

static int enc_ldst(struct actx *a, const char *mn0, struct op *o, int n)
{
    char mn[16];
    snprintf(mn, sizeof mn, "%s", mn0);
    int unscaled = 0;
    /* ldur, sturb, ...: the unscaled forms, always imm9 */
    if (mn[0] && mn[1] && mn[2] == 'u' && (mn[0] == 'l' || mn[0] == 's' || mn[0] == 'p')) {
        memmove(mn + 2, mn + 3, strlen(mn + 3) + 1);
        unscaled = 1;
    }
    if (!strncmp(mn0, "prfum", 5)) { snprintf(mn, sizeof mn, "prfm"); unscaled = 1; }
    const struct ldst *L = NULL;
    for (unsigned i = 0; i < sizeof ldsts / sizeof ldsts[0]; i++)
        if (!strcmp(mn, ldsts[i].mn)) L = &ldsts[i];
    if (!L) return 0;
    if (n < 2) BAD("'%s' takes a register and an address", mn0);
    int size = L->size, opc = L->opc, v = 0, rt;
    int is_prfm = !strcmp(mn, "prfm");
    if (is_prfm) {
        if (o[0].k == K_IMM) rt = (int)o[0].v;
        else {
            rt = -1;
            for (unsigned i = 0; i < sizeof prfops / sizeof prfops[0]; i++)
                if (o[0].k == K_WORD && ieq(o[0].word, prfops[i].name)) rt = prfops[i].v;
            if (rt < 0) BAD("'%s' is not a prefetch operation", o[0].word);
        }
    } else if (o[0].k == K_GPR) {
        rt = o[0].r;
        if (size < 0) size = o[0].x ? 3 : 2;
        else if (L->sign) opc = o[0].x ? 2 : 3;
        else if (o[0].x && !L->store && !L->sign) BAD("'%s' loads into a W register", mn0);
        if (L->size == 2 && L->sign && !o[0].x) BAD("'ldrsw' loads into an X register");
    } else if (o[0].k == K_FPR && L->size < 0) {
        rt = o[0].r;
        v = 1;
        switch (o[0].fsz) {
        case 1: size = 0; break; case 2: size = 1; break;
        case 4: size = 2; break; case 8: size = 3; break;
        default: size = 0; opc += 2; break;       /* q */
        }
    } else BAD("'%s' takes a register to load or store", mn0);
    int scale = v && o[0].fsz == 16 ? 4 : size;
    unsigned base = (unsigned)size << 30 | (unsigned)v << 26 | (unsigned)opc << 22;
    struct op *m = &o[1];
    if (m->k == K_IMM) {                         /* a literal: ldr x0, .+N */
        if (unscaled || L->store || (L->size >= 0 && !is_prfm && strcmp(mn, "ldrsw")))
            BAD("'%s' has no literal form", mn0);
        long long d = m->v;
        if (d & 3 || d < -(1 << 20) || d >= (1 << 20)) BAD("a literal is within 1 MiB, in words");
        unsigned lopc = is_prfm ? 3 : !strcmp(mn, "ldrsw") ? 2
                      : v ? (o[0].fsz == 4 ? 0 : o[0].fsz == 8 ? 1 : o[0].fsz == 16 ? 2 : 9)
                      : o[0].x ? 1 : 0;
        if (lopc == 9) BAD("a literal load is of an S, D or Q register");
        EMIT(0x18000000u | lopc << 30 | (unsigned)v << 26 | ((unsigned)(d >> 2) & 0x7ffff) << 5 | (unsigned)rt);
        return 1;
    }
    if (m->k != K_MEM) BAD("'%s' needs an address", mn0);
    if (n == 3) {                                /* [xn], #imm: post-index */
        if (m->mode != M_BASE || o[2].k != K_IMM) BAD("a post-index is [base], #offset");
        m->mode = M_POST;
        m->v = o[2].v;
    } else if (n != 2) BAD("'%s' takes a register and an address", mn0);
    if (m->mode == M_REG) {
        int opt = m->rext, s = 0;
        if (opt != 2 && opt != 3 && opt != 6 && opt != 7) BAD("a register offset extends by uxtw, lsl, sxtw or sxtx");
        if ((opt == 3 || opt == 7) != m->rmx) BAD("the offset register's width does not match its extend");
        if (m->ramt_given) {
            if (m->ramt != 0 && m->ramt != scale) BAD("the offset's shift is 0 or %d", scale);
            s = m->ramt == scale && (m->ramt || scale == 0);
            if (scale == 0) s = 1;          /* ldrb [x0, x1, lsl #0] */
        }
        EMIT(0x38200800u | base | (unsigned)m->rm << 16 | (unsigned)opt << 13 | (unsigned)s << 12 |
             (unsigned)m->base << 5 | (unsigned)rt);
        return 1;
    }
    long long off = m->v;
    if (m->mode == M_PRE || m->mode == M_POST) {
        if (is_prfm) BAD("a prefetch has no writeback form");
        if (off < -256 || off > 255) BAD("a writeback offset is -256 to 255");
        EMIT(0x38000000u | base | ((unsigned)off & 0x1ff) << 12 | (m->mode == M_PRE ? 3u : 1u) << 10 |
             (unsigned)m->base << 5 | (unsigned)rt);
        return 1;
    }
    long long unit = 1LL << scale;
    if (!unscaled && off >= 0 && off % unit == 0 && off / unit <= 0xfff) {
        EMIT(0x39000000u | base | (unsigned)(off / unit) << 10 | (unsigned)m->base << 5 | (unsigned)rt);
        return 1;
    }
    if (off >= -256 && off <= 255) {             /* ldur, as llvm-mc picks it */
        EMIT(0x38000000u | base | ((unsigned)off & 0x1ff) << 12 | (unsigned)m->base << 5 | (unsigned)rt);
        return 1;
    }
    BAD("offset %lld is neither a multiple of %lld up to %lld nor -256 to 255", off, unit, unit * 4095);
}

static int enc_ldstp(struct actx *a, const char *mn, struct op *o, int n)
{
    int load = mn[0] == 'l', nt = mn[2] == 'n', sw = !strcmp(mn, "ldpsw");
    if (strcmp(mn, "ldp") && strcmp(mn, "stp") && strcmp(mn, "ldnp") && strcmp(mn, "stnp") && !sw)
        return 0;
    if (n < 3 || o[2].k != K_MEM || o[0].k != o[1].k) BAD("'%s' takes two registers and an address", mn);
    int v = o[0].k == K_FPR, opc, scale;
    if (v) {
        if (o[0].fsz != o[1].fsz) BAD("the pair is one size");
        opc = o[0].fsz == 4 ? 0 : o[0].fsz == 8 ? 1 : o[0].fsz == 16 ? 2 : 3;
        if (opc == 3) BAD("a pair is of S, D or Q registers");
        scale = 2 + opc;
    } else if (o[0].k == K_GPR) {
        if (o[0].x != o[1].x) BAD("the pair is one width");
        opc = sw ? 1 : o[0].x ? 2 : 0;
        if (sw && !o[0].x) BAD("'ldpsw' loads X registers");
        scale = o[0].x && !sw ? 3 : 2;           /* ldpsw reads two words */
    } else BAD("'%s' takes two registers", mn);
    struct op *m = &o[2];
    int mode;
    long long off = m->v;
    if (n == 4) {
        if (m->mode != M_BASE || o[3].k != K_IMM) BAD("a post-index is [base], #offset");
        mode = 1;
        off = o[3].v;
    } else if (m->mode == M_PRE) mode = 3;
    else if (m->mode == M_OFF || m->mode == M_BASE) mode = 2;
    else BAD("'%s' takes [base{, #imm}]", mn);
    if (nt) { if (mode != 2) BAD("'%s' has no writeback", mn); mode = 0; }
    long long unit = 1LL << scale;
    if (off % unit || off / unit < -64 || off / unit > 63)
        BAD("a pair's offset is a multiple of %lld from %lld to %lld", unit, -64 * unit, 63 * unit);
    EMIT(0x28000000u | (unsigned)opc << 30 | (unsigned)v << 26 | (unsigned)mode << 23 |
         (unsigned)load << 22 | ((unsigned)(off / unit) & 0x7f) << 15 | (unsigned)o[1].r << 10 |
         (unsigned)m->base << 5 | (unsigned)o[0].r);
    return 1;
}

/* exclusive, acquire/release, LSE: the size from the suffix and the regs */
static int size_suffix(const char *s, int *size)
{
    if (!*s) { *size = -1; return 1; }
    if (!strcmp(s, "b")) { *size = 0; return 1; }
    if (!strcmp(s, "h")) { *size = 1; return 1; }
    return 0;
}

static int enc_excl(struct actx *a, const char *mn, struct op *o, int n)
{
    static const struct { const char *p; unsigned base; int regs; } tab[] = {
        /* ld*: rt, [xn]   st*: ws, rt, [xn]   p: pairs */
        { "ldaxp", 0x887F8000u, 3 }, { "ldxp", 0x887F0000u, 3 },
        { "stlxp", 0x88208000u, 4 }, { "stxp", 0x88200000u, 4 },
        { "ldaxr", 0x085FFC00u, 2 }, { "ldxr", 0x085F7C00u, 2 },
        { "stlxr", 0x0800FC00u, 3 }, { "stxr", 0x08007C00u, 3 },
        { "ldapr", 0x38BFC000u, 2 }, { "ldar", 0x08DFFC00u, 2 },
        { "stlr", 0x089FFC00u, 2 }, { "ldlar", 0x08DF7C00u, 2 },
        { "stllr", 0x089F7C00u, 2 },
    };
    for (unsigned i = 0; i < sizeof tab / sizeof tab[0]; i++) {
        size_t l = strlen(tab[i].p);
        int size;
        if (strncmp(mn, tab[i].p, l) || !size_suffix(mn + l, &size))
            continue;
        int pair = tab[i].p[strlen(tab[i].p) - 1] == 'p';
        if (pair && size >= 0) return 0;
        if (n != tab[i].regs) BAD("'%s' takes %d operands", mn, tab[i].regs);
        struct op *m = &o[n - 1];
        if (m->k != K_MEM || (m->mode != M_BASE && !(m->mode == M_OFF && m->v == 0)))
            BAD("'%s' takes a [base] address", mn);
        /* a store-exclusive's first operand is its status register;
         * stlr and stllr have none */
        int st = tab[i].p[0] == 's' && (tab[i].regs == 3 || pair);
        struct op *rt = &o[st ? 1 : 0];
        if (rt->k != K_GPR) BAD("expected a register");
        if (size < 0) size = rt->x ? 3 : 2;
        else if (rt->x) BAD("'%s' moves a W register", mn);
        unsigned w = tab[i].base | (unsigned)m->base << 5 | (unsigned)rt->r;
        if (pair) {
            w |= (unsigned)rt->x << 30;
            w |= (unsigned)o[st ? 2 : 1].r << 10;
        } else
            w |= (unsigned)size << 30;
        if (st) {
            if (o[0].k != K_GPR || o[0].x) BAD("the status register is a W register");
            w |= (unsigned)o[0].r << 16;
        }
        EMIT(w);
        return 1;
    }
    return 0;
}

static int enc_lse(struct actx *a, const char *mn, struct op *o, int n)
{
    static const char *const ops[] = { "add", "clr", "eor", "set", "smax", "smin", "umax", "umin" };
    char b[16];
    snprintf(b, sizeof b, "%s", mn);
    int st = 0, opc = -1, o3 = 0, cas = 0, casp = 0;
    const char *p;
    if (!strncmp(b, "ld", 2) || !strncmp(b, "st", 2)) {
        st = b[0] == 's';
        p = b + 2;
        for (int k = 0; k < 8; k++)
            if (!strncmp(p, ops[k], strlen(ops[k]))) { opc = k; p += strlen(ops[k]); break; }
        if (opc < 0) return 0;
    } else if (!strncmp(b, "swp", 3)) { opc = 0; o3 = 1; p = b + 3; }
    else if (!strncmp(b, "casp", 4)) { casp = 1; p = b + 4; }
    else if (!strncmp(b, "cas", 3)) { cas = 1; p = b + 3; }
    else return 0;
    int A = 0, R = 0;
    if (!st) {
        if (!strncmp(p, "al", 2)) { A = R = 1; p += 2; }
        else if (*p == 'a') { A = 1; p++; }
        else if (*p == 'l') { R = 1; p++; }
    } else if (*p == 'l') { R = 1; p++; }
    int size;
    if (!size_suffix(p, &size)) return 0;
    if (casp && size >= 0) return 0;
    struct op *m = &o[n - 1];
    if (m->k != K_MEM || (m->mode != M_BASE && !(m->mode == M_OFF && m->v == 0)))
        BAD("'%s' takes a [base] address", mn);
    int need = st ? 2 : casp ? 5 : 3;
    if (n != need) BAD("'%s' takes %d operands", mn, need);
    if (o[0].k != K_GPR) BAD("expected registers");
    if (size < 0) size = o[0].x ? 3 : 2;
    if (casp) {
        EMIT(0x08207C00u | (unsigned)o[0].x << 30 | (unsigned)A << 22 | (unsigned)o[0].r << 16 |
             (unsigned)R << 15 | (unsigned)m->base << 5 | (unsigned)o[2].r);
        return 1;
    }
    if (cas) {
        EMIT(0x08A07C00u | (unsigned)size << 30 | (unsigned)A << 22 | (unsigned)o[0].r << 16 |
             (unsigned)R << 15 | (unsigned)m->base << 5 | (unsigned)o[1].r);
        return 1;
    }
    int rt = st ? 31 : o[1].r;
    EMIT(0x38200000u | (unsigned)size << 30 | (unsigned)A << 23 | (unsigned)R << 22 |
         (unsigned)o[0].r << 16 | (unsigned)o3 << 15 | (unsigned)opc << 12 |
         (unsigned)m->base << 5 | (unsigned)rt);
    return 1;
}

static int gp2(struct op *o, int n, int k) { (void)n; return o[k].k == K_GPR; }

/* everything integer that is not a load, a store or a branch */
static int enc_dp(struct actx *a, const char *mn, struct op *o, int n)
{
#define IS(s) (!strcmp(mn, s))
    struct op *d = &o[0];
    /* add/sub and their aliases */
    if (IS("add") || IS("sub") || IS("adds") || IS("subs"))
        return enc_addsub(a, mn[0] == 's', mn[3] == 's', o, n);
    if (IS("cmp") || IS("cmn")) {
        if (n < 2 || d->k != K_GPR) BAD("'%s' takes a register and an operand", mn);
        struct op t[4];
        t[0] = *d; t[0].r = 31; t[0].sp = 0;
        t[1] = *d; t[2] = o[1];
        if (n == 3) t[3] = o[2];
        return enc_addsub(a, mn[2] == 'p', 1, t, n + 1);
    }
    if (IS("neg") || IS("negs")) {
        if (n < 2 || !gp2(o, n, 1)) BAD("'%s' takes two registers", mn);
        struct op t[4];
        t[0] = *d; t[1] = *d; t[1].r = 31; t[1].sp = 0; t[2] = o[1];
        if (n == 3) t[3] = o[2];
        return enc_addsub(a, 1, IS("negs"), t, n + 1);
    }
    /* logical */
    static const struct { const char *m; int opc, inv; } lg[] = {
        { "and", 0, 0 }, { "orr", 1, 0 }, { "eor", 2, 0 }, { "ands", 3, 0 },
        { "bic", 0, 1 }, { "orn", 1, 1 }, { "eon", 2, 1 }, { "bics", 3, 1 },
    };
    for (unsigned i = 0; i < sizeof lg / sizeof lg[0]; i++)
        if (IS(lg[i].m) && d->k == K_GPR)
            return enc_logical(a, lg[i].opc, lg[i].inv, o, n);
    if (IS("tst")) {
        struct op t[4];
        t[0] = *d; t[0].r = 31; t[1] = *d;
        t[2] = o[1];
        if (n == 3) t[3] = o[2];
        return enc_logical(a, 3, 0, t, n + 1);
    }
    if (IS("mvn") && d->k == K_GPR) {
        struct op t[4];
        t[0] = *d; t[1] = *d; t[1].r = 31; t[2] = o[1];
        if (n == 3) t[3] = o[2];
        return enc_logical(a, 1, 1, t, n + 1);
    }
    if (IS("mov") && d->k == K_GPR) {
        if (n != 2) BAD("'mov' takes two operands");
        if (o[1].k == K_IMM) return enc_mov_imm(a, d, o[1].v);
        if (o[1].k != K_GPR) return 0;
        if (d->sp || o[1].sp) {                   /* add rd, rn, #0 */
            EMIT(0x11000000u | (unsigned)d->x << 31 | (unsigned)o[1].r << 5 | (unsigned)d->r);
            return 1;
        }
        EMIT(0x2A0003E0u | (unsigned)d->x << 31 | (unsigned)o[1].r << 16 | (unsigned)d->r);
        return 1;
    }
    if (IS("movz") || IS("movn") || IS("movk")) {
        if (n < 2 || d->k != K_GPR || o[1].k != K_IMM) BAD("'%s' takes a register and an immediate", mn);
        int hw = 0;
        if (n == 3) {
            if (o[2].k != K_SHIFT || o[2].sh != 0 || o[2].v % 16 || o[2].v > (d->x ? 48 : 16))
                BAD("'%s' shifts by lsl 0, 16%s", mn, d->x ? ", 32 or 48" : "");
            hw = (int)o[2].v / 16;
        }
        if (o[1].v < 0 || o[1].v > 0xffff) BAD("'%s' takes a 16-bit immediate", mn);
        movw(a, d->x, IS("movn") ? 0 : IS("movz") ? 2 : 3, d->r, (unsigned)o[1].v, hw);
        return 1;
    }
    if ((IS("adr") || IS("adrp")) && n == 2 && d->k == K_GPR && o[1].k == K_IMM) {
        long long v = o[1].v;
        int page = mn[3] == 'p';
        if (page) {
            if (v & 0xfff) BAD("'adrp' is to a 4 KiB page");
            v >>= 12;
        }
        if (v < -(1 << 20) || v >= (1 << 20)) BAD("'%s' reaches 1 MiB%s", mn, page ? " of pages" : "");
        EMIT((page ? 0x90000000u : 0x10000000u) | ((unsigned)v & 3) << 29 |
             ((unsigned)(v >> 2) & 0x7ffff) << 5 | (unsigned)d->r);
        return 1;
    }
    /* bitfields */
    int sf = d->k == K_GPR ? d->x : 0, w = sf ? 64 : 32;
    if ((IS("sbfm") || IS("bfm") || IS("ubfm")) && n == 4)
        return enc_bfm(a, IS("sbfm") ? 0 : IS("bfm") ? 1 : 2, d, &o[1], (int)o[2].v, (int)o[3].v);
    if ((IS("lsl") || IS("lsr") || IS("asr") || IS("ror")) && n == 3 && d->k == K_GPR) {
        if (o[2].k == K_GPR) {                    /* lslv lsrv asrv rorv */
            int op = IS("lsl") ? 8 : IS("lsr") ? 9 : IS("asr") ? 10 : 11;
            EMIT(0x1AC00000u | (unsigned)sf << 31 | (unsigned)o[2].r << 16 | (unsigned)op << 10 |
                 (unsigned)o[1].r << 5 | (unsigned)d->r);
            return 1;
        }
        if (o[2].k != K_IMM) BAD("'%s' shifts by a register or an immediate", mn);
        int s = (int)o[2].v;
        if (s < 0 || s >= w) BAD("shift %d is outside the register", s);
        if (IS("lsl")) return enc_bfm(a, 2, d, &o[1], (w - s) % w, w - 1 - s);
        if (IS("lsr")) return enc_bfm(a, 2, d, &o[1], s, w - 1);
        if (IS("asr")) return enc_bfm(a, 0, d, &o[1], s, w - 1);
        EMIT(0x13800000u | (unsigned)sf << 31 | (unsigned)sf << 22 | (unsigned)o[1].r << 16 |
             (unsigned)s << 10 | (unsigned)o[1].r << 5 | (unsigned)d->r);    /* extr */
        return 1;
    }
    if ((IS("lslv") || IS("lsrv") || IS("asrv") || IS("rorv") || IS("udiv") || IS("sdiv")) && n == 3) {
        int op = IS("lslv") ? 8 : IS("lsrv") ? 9 : IS("asrv") ? 10 : IS("rorv") ? 11 : IS("udiv") ? 2 : 3;
        EMIT(0x1AC00000u | (unsigned)sf << 31 | (unsigned)o[2].r << 16 | (unsigned)op << 10 |
             (unsigned)o[1].r << 5 | (unsigned)d->r);
        return 1;
    }
    if (IS("extr") && n == 4) {
        EMIT(0x13800000u | (unsigned)sf << 31 | (unsigned)sf << 22 | (unsigned)o[2].r << 16 |
             (unsigned)o[3].v << 10 | (unsigned)o[1].r << 5 | (unsigned)d->r);
        return 1;
    }
    if ((IS("ubfx") || IS("sbfx") || IS("ubfiz") || IS("sbfiz") || IS("bfi") || IS("bfxil")) && n == 4) {
        int lsb = (int)o[2].v, wd = (int)o[3].v;
        if (lsb < 0 || wd < 1 || lsb + wd > w) BAD("the field is outside the register");
        int opc = mn[0] == 's' ? 0 : mn[0] == 'u' ? 2 : 1;
        if (IS("ubfx") || IS("sbfx") || IS("bfxil"))
            return enc_bfm(a, opc, d, &o[1], lsb, lsb + wd - 1);
        return enc_bfm(a, opc, d, &o[1], (w - lsb) % w, wd - 1);
    }
    if (IS("bfc") && n == 3) {
        struct op z = *d; z.r = 31;
        int lsb = (int)o[1].v, wd = (int)o[2].v;
        return enc_bfm(a, 1, d, &z, (w - lsb) % w, wd - 1);
    }
    if ((IS("sxtb") || IS("sxth") || IS("sxtw") || IS("uxtb") || IS("uxth")) && n == 2) {
        int bits = mn[3] == 'b' ? 7 : mn[3] == 'h' ? 15 : 31;
        if (mn[0] == 'u') {                       /* uxt[bh] wd, wn: ubfm w */
            struct op t = *d; t.x = 0;
            return enc_bfm(a, 2, &t, &o[1], 0, bits);
        }
        return enc_bfm(a, 0, d, &o[1], 0, bits);
    }
    /* multiply */
    static const struct { const char *m; unsigned base; int nops, ra31; } mul[] = {
        { "madd", 0x1B000000u, 4, 0 }, { "msub", 0x1B008000u, 4, 0 },
        { "mul", 0x1B007C00u, 3, 1 }, { "mneg", 0x1B00FC00u, 3, 1 },
        { "smaddl", 0x9B200000u, 4, 0 }, { "smsubl", 0x9B208000u, 4, 0 },
        { "umaddl", 0x9BA00000u, 4, 0 }, { "umsubl", 0x9BA08000u, 4, 0 },
        { "smull", 0x9B207C00u, 3, 1 }, { "umull", 0x9BA07C00u, 3, 1 },
        { "smnegl", 0x9B20FC00u, 3, 1 }, { "umnegl", 0x9BA0FC00u, 3, 1 },
        { "smulh", 0x9B407C00u, 3, 1 }, { "umulh", 0x9BC07C00u, 3, 1 },
    };
    for (unsigned i = 0; i < sizeof mul / sizeof mul[0]; i++)
        if (IS(mul[i].m) && d->k == K_GPR) {
            if (n != mul[i].nops) BAD("'%s' takes %d registers", mn, mul[i].nops);
            unsigned wd = mul[i].base | (unsigned)o[2].r << 16 | (unsigned)o[1].r << 5 | (unsigned)d->r;
            if (!(mul[i].base & 0x80000000u)) wd |= (unsigned)sf << 31;
            if (!mul[i].ra31) wd |= (unsigned)o[3].r << 10;
            EMIT(wd);
            return 1;
        }
    /* conditional select and its aliases */
    static const struct { const char *m; unsigned op; } cs[] = {
        { "csel", 0x1A800000u }, { "csinc", 0x1A800400u },
        { "csinv", 0x5A800000u }, { "csneg", 0x5A800400u },
    };
    for (unsigned i = 0; i < 4; i++)
        if (IS(cs[i].m) && n == 4) {
            if (o[3].k != K_COND) BAD("'%s' ends with a condition", mn);
            EMIT(cs[i].op | (unsigned)sf << 31 | (unsigned)o[2].r << 16 | (unsigned)o[3].cond << 12 |
                 (unsigned)o[1].r << 5 | (unsigned)d->r);
            return 1;
        }
    if ((IS("cset") || IS("csetm")) && n == 2 && o[1].k == K_COND) {
        unsigned op = IS("cset") ? 0x1A800400u : 0x5A800000u;
        EMIT(op | (unsigned)sf << 31 | 31u << 16 | (unsigned)(o[1].cond ^ 1) << 12 | 31u << 5 | (unsigned)d->r);
        return 1;
    }
    if ((IS("cinc") || IS("cinv") || IS("cneg")) && n == 3 && o[2].k == K_COND) {
        unsigned op = IS("cinc") ? 0x1A800400u : IS("cinv") ? 0x5A800000u : 0x5A800400u;
        EMIT(op | (unsigned)sf << 31 | (unsigned)o[1].r << 16 | (unsigned)(o[2].cond ^ 1) << 12 |
             (unsigned)o[1].r << 5 | (unsigned)d->r);
        return 1;
    }
    if ((IS("ccmp") || IS("ccmn")) && n == 4) {
        if (o[2].k != K_IMM || o[3].k != K_COND || o[2].v < 0 || o[2].v > 15)
            BAD("'%s' takes rn, rm or #imm5, #nzcv and a condition", mn);
        unsigned op = IS("ccmp") ? 0x7A400000u : 0x3A400000u;
        unsigned m;
        if (o[1].k == K_IMM) {
            if (o[1].v < 0 || o[1].v > 31) BAD("'%s' compares with 0 to 31", mn);
            m = (unsigned)o[1].v << 16 | 1u << 11;
        } else m = (unsigned)o[1].r << 16;
        EMIT(op | (unsigned)sf << 31 | m | (unsigned)o[3].cond << 12 | (unsigned)d->r << 5 | (unsigned)o[2].v);
        return 1;
    }
    static const struct { const char *m; unsigned op; } adc[] = {
        { "adc", 0x1A000000u }, { "adcs", 0x3A000000u }, { "sbc", 0x5A000000u }, { "sbcs", 0x7A000000u },
    };
    for (unsigned i = 0; i < 4; i++)
        if (IS(adc[i].m) && n == 3) {
            EMIT(adc[i].op | (unsigned)sf << 31 | (unsigned)o[2].r << 16 | (unsigned)o[1].r << 5 | (unsigned)d->r);
            return 1;
        }
    if ((IS("ngc") || IS("ngcs")) && n == 2) {
        EMIT((IS("ngc") ? 0x5A000000u : 0x7A000000u) | (unsigned)sf << 31 | (unsigned)o[1].r << 16 |
             31u << 5 | (unsigned)d->r);
        return 1;
    }
    /* one source */
    static const struct { const char *m; int op, only64; } one[] = {
        { "rbit", 0, 0 }, { "rev16", 1, 0 }, { "rev32", 2, 1 }, { "clz", 4, 0 }, { "cls", 5, 0 },
    };
    for (unsigned i = 0; i < sizeof one / sizeof one[0]; i++)
        if (IS(one[i].m) && n == 2 && d->k == K_GPR) {
            if (one[i].only64 && !sf) BAD("'%s' is of X registers", mn);
            EMIT(0x5AC00000u | (unsigned)sf << 31 | (unsigned)one[i].op << 10 | (unsigned)o[1].r << 5 | (unsigned)d->r);
            return 1;
        }
    if ((IS("rev") || IS("rev64")) && n == 2 && d->k == K_GPR) {
        EMIT(0x5AC00000u | (unsigned)sf << 31 | (sf ? 3u : 2u) << 10 | (unsigned)o[1].r << 5 | (unsigned)d->r);
        return 1;
    }
#undef IS
    return 0;
}

/* ---- scalar floating point ----------------------------------------------- */

static int ftype(const struct op *o)
{
    return o->fsz == 4 ? 0 : o->fsz == 8 ? 1 : o->fsz == 2 ? 3 : -1;
}

static int enc_fp(struct actx *a, const char *mn, struct op *o, int n)
{
#define IS(s) (!strcmp(mn, s))
    struct op *d = &o[0];
    if (IS("fmov") && n == 2) {
        /* fmov d0, x1 / fmov x1, d0 / fmov v0.d[1], x1 / fmov x1, v0.d[1] */
        if (d->k == K_FPR && o[1].k == K_FPR) {
            int t = ftype(d);
            if (t < 0 || d->fsz != o[1].fsz) BAD("'fmov' between H, S or D registers of one size");
            EMIT(0x1E204000u | (unsigned)t << 22 | (unsigned)o[1].r << 5 | (unsigned)d->r);
            return 1;
        }
        if (d->k == K_FPR && o[1].k == K_GPR) {
            int t = ftype(d);
            if (t < 0 || ((d->fsz == 8) != o[1].x && d->fsz != 2)) BAD("'fmov' moves a W to an S, an X to a D");
            EMIT(0x1E270000u | (unsigned)o[1].x << 31 | (unsigned)t << 22 | (unsigned)o[1].r << 5 | (unsigned)d->r);
            return 1;
        }
        if (d->k == K_GPR && o[1].k == K_FPR) {
            int t = ftype(&o[1]);
            if (t < 0 || ((o[1].fsz == 8) != d->x && o[1].fsz != 2)) BAD("'fmov' moves an S to a W, a D to an X");
            EMIT(0x1E260000u | (unsigned)d->x << 31 | (unsigned)t << 22 | (unsigned)o[1].r << 5 | (unsigned)d->r);
            return 1;
        }
        if (d->k == K_ELEM && o[1].k == K_GPR && d->esz == 8 && d->idx == 1) {
            EMIT(0x9EAF0000u | (unsigned)o[1].r << 5 | (unsigned)d->r);
            return 1;
        }
        if (d->k == K_GPR && o[1].k == K_ELEM && o[1].esz == 8 && o[1].idx == 1) {
            EMIT(0x9EAE0000u | (unsigned)o[1].r << 5 | (unsigned)d->r);
            return 1;
        }
        if (d->k == K_FPR && (o[1].k == K_FIMM || o[1].k == K_IMM)) {
            double v = o[1].k == K_FIMM ? o[1].f : (double)o[1].v;
            int t = ftype(d), e = fp8_enc(v);
            if (t < 0) BAD("'fmov' an immediate to an H, S or D register");
            if (e < 0) BAD("%g is not an 8-bit floating-point immediate", v);
            EMIT(0x1E201000u | (unsigned)t << 22 | (unsigned)e << 13 | (unsigned)d->r);
            return 1;
        }
        return 0;                               /* vector fmov: SIMD */
    }
    if (d->k != K_FPR)
        return 0;
    int t = ftype(d);
    static const struct { const char *m; int op; } two[] = {
        { "fmul", 0 }, { "fdiv", 1 }, { "fadd", 2 }, { "fsub", 3 }, { "fmax", 4 },
        { "fmin", 5 }, { "fmaxnm", 6 }, { "fminnm", 7 }, { "fnmul", 8 },
    };
    for (unsigned i = 0; i < sizeof two / sizeof two[0]; i++)
        if (IS(two[i].m) && n == 3 && o[1].k == K_FPR && o[2].k == K_FPR) {
            if (t < 0) BAD("'%s' is of H, S or D registers", mn);
            EMIT(0x1E200800u | (unsigned)t << 22 | (unsigned)o[2].r << 16 | (unsigned)two[i].op << 12 |
                 (unsigned)o[1].r << 5 | (unsigned)d->r);
            return 1;
        }
    static const struct { const char *m; unsigned op; } three[] = {
        { "fmadd", 0x1F000000u }, { "fmsub", 0x1F008000u }, { "fnmadd", 0x1F200000u }, { "fnmsub", 0x1F208000u },
    };
    for (unsigned i = 0; i < 4; i++)
        if (IS(three[i].m) && n == 4) {
            if (t < 0) BAD("'%s' is of H, S or D registers", mn);
            EMIT(three[i].op | (unsigned)t << 22 | (unsigned)o[2].r << 16 | (unsigned)o[3].r << 10 |
                 (unsigned)o[1].r << 5 | (unsigned)d->r);
            return 1;
        }
    static const struct { const char *m; int op; } one[] = {
        { "fabs", 1 }, { "fneg", 2 }, { "fsqrt", 3 }, { "frintn", 8 }, { "frintp", 9 },
        { "frintm", 10 }, { "frintz", 11 }, { "frinta", 12 }, { "frintx", 14 }, { "frinti", 15 },
    };
    for (unsigned i = 0; i < sizeof one / sizeof one[0]; i++)
        if (IS(one[i].m) && n == 2 && o[1].k == K_FPR) {
            if (t < 0) BAD("'%s' is of H, S or D registers", mn);
            EMIT(0x1E204000u | (unsigned)t << 22 | (unsigned)one[i].op << 15 | (unsigned)o[1].r << 5 | (unsigned)d->r);
            return 1;
        }
    if (IS("fcvt") && n == 2 && o[1].k == K_FPR) {
        int from = ftype(&o[1]), to = ftype(d);
        if (from < 0 || to < 0 || from == to) BAD("'fcvt' converts between H, S and D");
        int opc = to == 0 ? 4 : to == 1 ? 5 : 7;
        EMIT(0x1E204000u | (unsigned)from << 22 | (unsigned)opc << 15 | (unsigned)o[1].r << 5 | (unsigned)d->r);
        return 1;
    }
    if ((IS("fcmp") || IS("fcmpe")) && n == 2) {
        if (t < 0) BAD("'%s' compares H, S or D registers", mn);
        unsigned e = IS("fcmpe") ? 0x10u : 0;
        if (o[1].k == K_FPR) {
            EMIT(0x1E202000u | (unsigned)t << 22 | (unsigned)o[1].r << 16 | (unsigned)d->r << 5 | e);
            return 1;
        }
        if ((o[1].k == K_FIMM && o[1].f == 0.0) || (o[1].k == K_IMM && o[1].v == 0)) {
            EMIT(0x1E202008u | (unsigned)t << 22 | (unsigned)d->r << 5 | e);
            return 1;
        }
        BAD("'%s' compares with a register or #0.0", mn);
    }
    if ((IS("fccmp") || IS("fccmpe")) && n == 4) {
        if (o[3].k != K_COND || o[2].k != K_IMM) BAD("'%s' takes rn, rm, #nzcv and a condition", mn);
        EMIT(0x1E200400u | (unsigned)t << 22 | (unsigned)o[1].r << 16 | (unsigned)o[3].cond << 12 |
             (unsigned)d->r << 5 | (IS("fccmpe") ? 0x10u : 0) | (unsigned)o[2].v);
        return 1;
    }
    if (IS("fcsel") && n == 4 && o[3].k == K_COND) {
        EMIT(0x1E200C00u | (unsigned)t << 22 | (unsigned)o[2].r << 16 | (unsigned)o[3].cond << 12 |
             (unsigned)o[1].r << 5 | (unsigned)d->r);
        return 1;
    }
    /* integer to floating point: scvtf/ucvtf sd, wn|xn{, #fbits} */
    if ((IS("scvtf") || IS("ucvtf")) && n >= 2 && o[1].k == K_GPR) {
        unsigned u = mn[0] == 'u';
        if (n == 3) {
            int fb = (int)o[2].v;
            if (fb < 1 || fb > (o[1].x ? 64 : 32)) BAD("fixed-point bits are 1 to %d", o[1].x ? 64 : 32);
            EMIT(0x1E020000u | (unsigned)o[1].x << 31 | (unsigned)t << 22 | u << 16 |
                 (unsigned)(64 - fb) << 10 | (unsigned)o[1].r << 5 | (unsigned)d->r);
            return 1;
        }
        EMIT(0x1E220000u | (unsigned)o[1].x << 31 | (unsigned)t << 22 | u << 16 | (unsigned)o[1].r << 5 | (unsigned)d->r);
        return 1;
    }
    return 0;
#undef IS
}

/* floating point to integer: the destination is a general register */
static int enc_fcvt_int(struct actx *a, const char *mn, struct op *o, int n)
{
    static const struct { const char *m; int rmode, op; } cv[] = {
        { "fcvtns", 0, 0 }, { "fcvtnu", 0, 1 }, { "fcvtas", 0, 4 }, { "fcvtau", 0, 5 },
        { "fcvtps", 1, 0 }, { "fcvtpu", 1, 1 }, { "fcvtms", 2, 0 }, { "fcvtmu", 2, 1 },
        { "fcvtzs", 3, 0 }, { "fcvtzu", 3, 1 },
    };
    if (n < 2 || o[0].k != K_GPR || o[1].k != K_FPR)
        return 0;
    for (unsigned i = 0; i < sizeof cv / sizeof cv[0]; i++)
        if (!strcmp(mn, cv[i].m)) {
            int t = ftype(&o[1]);
            if (t < 0) BAD("'%s' converts an H, S or D register", mn);
            if (n == 3) {
                if (cv[i].rmode != 3) BAD("'%s' has no fixed-point form", mn);
                int fb = (int)o[2].v;
                if (fb < 1 || fb > (o[0].x ? 64 : 32)) BAD("fixed-point bits are 1 to %d", o[0].x ? 64 : 32);
                EMIT(0x1E180000u | (unsigned)o[0].x << 31 | (unsigned)t << 22 | (unsigned)cv[i].op << 16 |
                     (unsigned)(64 - fb) << 10 | (unsigned)o[1].r << 5 | (unsigned)o[0].r);
                return 1;
            }
            EMIT(0x1E200000u | (unsigned)o[0].x << 31 | (unsigned)t << 22 | (unsigned)cv[i].rmode << 19 |
                 (unsigned)cv[i].op << 16 | (unsigned)o[1].r << 5 | (unsigned)o[0].r);
            return 1;
        }
    return 0;
}

/* ---- Advanced SIMD -------------------------------------------------------- */

static int log2sz(int esz) { return esz == 1 ? 0 : esz == 2 ? 1 : esz == 4 ? 2 : esz == 8 ? 3 : 4; }
static int vq(const struct op *o) { return o->esz * o->nel == 16; }

/* the three-same integer group: U, opcode, and whether the size is the
 * mnemonic's own (the bitwise ones) */
static const struct { const char *m; int u, opc, fixsz; } v3same[] = {
    { "add", 0, 0x10, -1 }, { "sub", 1, 0x10, -1 }, { "mul", 0, 0x13, -1 }, { "mla", 0, 0x12, -1 },
    { "mls", 1, 0x12, -1 }, { "cmeq", 1, 0x11, -1 }, { "cmtst", 0, 0x11, -1 }, { "cmgt", 0, 0x06, -1 },
    { "cmge", 0, 0x07, -1 }, { "cmhi", 1, 0x06, -1 }, { "cmhs", 1, 0x07, -1 }, { "sshl", 0, 0x08, -1 },
    { "ushl", 1, 0x08, -1 }, { "srshl", 0, 0x0a, -1 }, { "urshl", 1, 0x0a, -1 }, { "sqshl", 0, 0x09, -1 },
    { "uqshl", 1, 0x09, -1 }, { "smax", 0, 0x0c, -1 }, { "smin", 0, 0x0d, -1 }, { "umax", 1, 0x0c, -1 },
    { "umin", 1, 0x0d, -1 }, { "smaxp", 0, 0x14, -1 }, { "sminp", 0, 0x15, -1 }, { "umaxp", 1, 0x14, -1 },
    { "uminp", 1, 0x15, -1 }, { "addp", 0, 0x17, -1 }, { "sqadd", 0, 0x01, -1 }, { "uqadd", 1, 0x01, -1 },
    { "sqsub", 0, 0x05, -1 }, { "uqsub", 1, 0x05, -1 }, { "shadd", 0, 0x00, -1 }, { "uhadd", 1, 0x00, -1 },
    { "srhadd", 0, 0x02, -1 }, { "urhadd", 1, 0x02, -1 }, { "shsub", 0, 0x04, -1 }, { "uhsub", 1, 0x04, -1 },
    { "sabd", 0, 0x0e, -1 }, { "uabd", 1, 0x0e, -1 }, { "saba", 0, 0x0f, -1 }, { "uaba", 1, 0x0f, -1 },
    { "sqdmulh", 0, 0x16, -1 }, { "sqrdmulh", 1, 0x16, -1 }, { "pmul", 1, 0x13, -1 },
    { "and", 0, 0x03, 0 }, { "bic", 0, 0x03, 1 }, { "orr", 0, 0x03, 2 }, { "orn", 0, 0x03, 3 },
    { "eor", 1, 0x03, 0 }, { "bsl", 1, 0x03, 1 }, { "bit", 1, 0x03, 2 }, { "bif", 1, 0x03, 3 },
};
static const struct { const char *m; unsigned op; } f3same[] = {
    { "fmaxnm", 0x0E20C400u }, { "fmla", 0x0E20CC00u }, { "fadd", 0x0E20D400u }, { "fmulx", 0x0E20DC00u },
    { "fcmeq", 0x0E20E400u }, { "fmax", 0x0E20F400u }, { "frecps", 0x0E20FC00u }, { "fminnm", 0x0EA0C400u },
    { "fmls", 0x0EA0CC00u }, { "fsub", 0x0EA0D400u }, { "fmin", 0x0EA0F400u }, { "frsqrts", 0x0EA0FC00u },
    { "fmaxnmp", 0x2E20C400u }, { "faddp", 0x2E20D400u }, { "fmul", 0x2E20DC00u }, { "fcmge", 0x2E20E400u },
    { "facge", 0x2E20EC00u }, { "fmaxp", 0x2E20F400u }, { "fdiv", 0x2E20FC00u }, { "fminnmp", 0x2EA0C400u },
    { "fabd", 0x2EA0D400u }, { "fcmgt", 0x2EA0E400u }, { "facgt", 0x2EA0EC00u }, { "fminp", 0x2EA0F400u },
};
static const struct { const char *m; int u, opc, sz; } v2misc[] = {
    { "rev64", 0, 0x00, -1 }, { "rev16", 0, 0x01, -1 }, { "rev32", 1, 0x00, -1 }, { "saddlp", 0, 0x02, -2 },
    { "uaddlp", 1, 0x02, -2 }, { "suqadd", 0, 0x03, -1 }, { "usqadd", 1, 0x03, -1 }, { "cls", 0, 0x04, -1 },
    { "clz", 1, 0x04, -1 }, { "cnt", 0, 0x05, 0 }, { "not", 1, 0x05, 0 }, { "mvn", 1, 0x05, 0 },
    { "rbit", 1, 0x05, 1 }, { "sadalp", 0, 0x06, -2 }, { "uadalp", 1, 0x06, -2 }, { "sqabs", 0, 0x07, -1 },
    { "sqneg", 1, 0x07, -1 }, { "abs", 0, 0x0b, -1 }, { "neg", 1, 0x0b, -1 },
};
static const struct { const char *m; unsigned op; } f2misc[] = {
    { "fabs", 0x0EA0F800u }, { "fneg", 0x2EA0F800u }, { "fsqrt", 0x2EA1F800u }, { "frintn", 0x0E218800u },
    { "frintm", 0x0E219800u }, { "frintp", 0x0EA18800u }, { "frintz", 0x0EA19800u }, { "frinta", 0x2E218800u },
    { "frintx", 0x2E219800u }, { "frinti", 0x2EA19800u }, { "fcvtzs", 0x0EA1B800u }, { "fcvtzu", 0x2EA1B800u },
    { "scvtf", 0x0E21D800u }, { "ucvtf", 0x2E21D800u }, { "fcvtns", 0x0E21A800u }, { "fcvtnu", 0x2E21A800u },
    { "fcvtms", 0x0E21B800u }, { "fcvtmu", 0x2E21B800u }, { "fcvtas", 0x0E21C800u }, { "fcvtau", 0x2E21C800u },
    { "fcvtps", 0x0EA1A800u }, { "fcvtpu", 0x2EA1A800u }, { "frecpe", 0x0EA1D800u }, { "frsqrte", 0x2EA1D800u },
};
static const struct { const char *m; int u, opc; } vlong[] = {
    { "saddl", 0, 0 }, { "uaddl", 1, 0 }, { "saddw", 0, 1 }, { "uaddw", 1, 1 }, { "ssubl", 0, 2 },
    { "usubl", 1, 2 }, { "ssubw", 0, 3 }, { "usubw", 1, 3 }, { "addhn", 0, 4 }, { "raddhn", 1, 4 },
    { "sabal", 0, 5 }, { "uabal", 1, 5 }, { "subhn", 0, 6 }, { "rsubhn", 1, 6 }, { "sabdl", 0, 7 },
    { "uabdl", 1, 7 }, { "smlal", 0, 8 }, { "umlal", 1, 8 }, { "sqdmlal", 0, 9 }, { "smlsl", 0, 10 },
    { "umlsl", 1, 10 }, { "sqdmlsl", 0, 11 }, { "smull", 0, 12 }, { "umull", 1, 12 }, { "sqdmull", 0, 13 },
    { "pmull", 0, 14 },
};
/* shift by immediate: kind 'r' right (2*esize - n), 'l' left (esize + n),
 * 'n' narrowing right (the destination's esize), 'w' widening left (the
 * source's), 'f' fixed point (as 'r') */
static const struct { const char *m; int u, opc, kind; } vshift[] = {
    { "sshr", 0, 0x00, 'r' }, { "ushr", 1, 0x00, 'r' }, { "ssra", 0, 0x02, 'r' }, { "usra", 1, 0x02, 'r' },
    { "srshr", 0, 0x04, 'r' }, { "urshr", 1, 0x04, 'r' }, { "srsra", 0, 0x06, 'r' }, { "ursra", 1, 0x06, 'r' },
    { "sri", 1, 0x08, 'r' }, { "shl", 0, 0x0a, 'l' }, { "sli", 1, 0x0a, 'l' }, { "sqshlu", 1, 0x0c, 'l' },
    { "sqshl", 0, 0x0e, 'l' }, { "uqshl", 1, 0x0e, 'l' }, { "shrn", 0, 0x10, 'n' }, { "rshrn", 0, 0x11, 'n' },
    { "sqshrun", 1, 0x10, 'n' }, { "sqrshrun", 1, 0x11, 'n' }, { "sqshrn", 0, 0x12, 'n' }, { "uqshrn", 1, 0x12, 'n' },
    { "sqrshrn", 0, 0x13, 'n' }, { "uqrshrn", 1, 0x13, 'n' }, { "sshll", 0, 0x14, 'w' }, { "ushll", 1, 0x14, 'w' },
    { "scvtf", 0, 0x1c, 'f' }, { "ucvtf", 1, 0x1c, 'f' }, { "fcvtzs", 0, 0x1f, 'f' }, { "fcvtzu", 1, 0x1f, 'f' },
};

static unsigned imm5_of(int esz, int idx)
{
    int l = log2sz(esz);
    return (unsigned)(idx << (l + 1) | 1 << l);
}

static int enc_movi(struct actx *a, const char *mn, struct op *d, struct op *o, int n)
{
    int mvni = !strcmp(mn, "mvni"), orr = !strcmp(mn, "orr"), bic = !strcmp(mn, "bic");
    int fmov = !strcmp(mn, "fmov");
    unsigned Q, op = 0, cmode;
    unsigned long long imm;
    if (d->k == K_FPR) {                          /* movi d0, #mask */
        if (strcmp(mn, "movi") || d->fsz != 8) BAD("'%s' of a scalar is movi dN", mn);
        Q = 0; op = 1; cmode = 0xe;
    } else {
        Q = (unsigned)vq(d);
        cmode = 0;
    }
    if (fmov) {
        double v = o[1].k == K_FIMM ? o[1].f : (double)o[1].v;
        int e = fp8_enc(v);
        if (e < 0) BAD("%g is not an 8-bit floating-point immediate", v);
        if (d->esz == 8) { if (!Q) BAD("'fmov' of a .2d vector"); op = 1; }
        else if (d->esz != 4) BAD("'fmov' an immediate to .2s, .4s or .2d");
        EMIT(0x0F00F400u | Q << 30 | op << 29 | ((unsigned)e >> 5) << 16 | ((unsigned)e & 31) << 5 | (unsigned)d->r);
        return 1;
    }
    if (o[1].k != K_IMM) BAD("'%s' takes an immediate", mn);
    imm = (unsigned long long)o[1].v;
    int amt = 0, msl = 0;
    if (n == 3) {
        if (o[2].k != K_SHIFT || (o[2].sh != 0 && o[2].sh != 4)) BAD("'%s' shifts by lsl or msl", mn);
        amt = (int)o[2].v;
        msl = o[2].sh == 4;
    }
    unsigned imm8;
    int esz = d->k == K_FPR ? 8 : d->esz;
    if (esz == 8) {                               /* each byte 0x00 or 0xff */
        if (mvni || orr || bic) BAD("'%s' has no 64-bit form", mn);
        imm8 = 0;
        for (int b = 0; b < 8; b++) {
            unsigned by = (unsigned)(imm >> (8 * b)) & 0xff;
            if (by != 0 && by != 0xff) BAD("a 64-bit movi's bytes are each 0x00 or 0xff");
            if (by) imm8 |= 1u << b;
        }
        op = 1; cmode = 0xe;
    } else {
        if (imm > 0xff) BAD("'%s' takes an 8-bit immediate and a shift", mn);
        imm8 = (unsigned)imm;
        if (esz == 1) {
            if (mvni || orr || bic || amt) BAD("'%s' has no byte form with a shift", mn);
            cmode = 0xe;
        } else if (esz == 2) {
            if (amt != 0 && amt != 8) BAD("a .h immediate shifts by 0 or 8");
            cmode = 0x8 | (amt == 8 ? 2u : 0) | ((orr || bic) ? 1u : 0);
        } else {
            if (msl) {
                if (orr || bic || (amt != 8 && amt != 16)) BAD("msl shifts by 8 or 16");
                cmode = 0xc | (amt == 16 ? 1u : 0);
            } else {
                if (amt % 8 || amt > 24) BAD("a .s immediate shifts by 0, 8, 16 or 24");
                cmode = (unsigned)(amt / 8) << 1 | ((orr || bic) ? 1u : 0);
            }
        }
        op = (mvni || bic) ? 1u : 0;
    }
    EMIT(0x0F000400u | Q << 30 | op << 29 | (imm8 >> 5) << 16 | cmode << 12 | (imm8 & 31) << 5 | (unsigned)d->r);
    return 1;
}

static int enc_ldst_simd(struct actx *a, const char *mn, struct op *o, int n)
{
    int L = mn[0] == 'l';
    int nst = mn[2] - '0';                        /* ld1..ld4, st1..st4 */
    int rep = mn[3] == 'r';
    if ((strncmp(mn, "ld", 2) && strncmp(mn, "st", 2)) || nst < 1 || nst > 4 ||
        (mn[3] && !(rep && !mn[4])))
        return 0;
    if (n < 2 || o[0].k != K_LIST || o[1].k != K_MEM || o[1].mode != M_BASE)
        BAD("'%s' takes a register list and [base]", mn);
    struct op *l = &o[0];
    unsigned rm = 0, post = 0;
    int bytes = l->nregs * (l->nel ? l->esz * l->nel : l->esz);
    if (n == 3) {
        post = 1;
        if (o[2].k == K_IMM) {
            if (o[2].v != (rep || l->idx >= 0 ? (long long)nst * l->esz : (long long)bytes))
                BAD("the post-index is the bytes transferred");
            rm = 31;
        } else if (o[2].k == K_GPR && o[2].x) rm = (unsigned)o[2].r;
        else BAD("a post-index is an immediate or an X register");
    }
    if (rep) {
        if (!L || l->nregs != nst) BAD("'%s' loads %d registers", mn, nst);
        unsigned R = (nst == 2 || nst == 4), op3 = nst >= 3;
        EMIT(0x0D40C000u | (unsigned)vq(l) << 30 | post << 23 | R << 21 | rm << 16 | op3 << 13 |
             (unsigned)log2sz(l->esz) << 10 | (unsigned)o[1].base << 5 | (unsigned)l->r);
        return 1;
    }
    if (l->idx >= 0) {                            /* one lane: {v0.s}[1] */
        if (l->nregs != nst) BAD("'%s' transfers %d registers", mn, nst);
        int idx = l->idx, e = l->esz;
        unsigned Q, S, size, opc;
        if (e == 1) { Q = idx >> 3; S = (idx >> 2) & 1; size = idx & 3; opc = 0; }
        else if (e == 2) { Q = idx >> 2; S = (idx >> 1) & 1; size = (idx & 1) << 1; opc = 2; }
        else if (e == 4) { Q = idx >> 1; S = idx & 1; size = 0; opc = 4; }
        else { Q = idx; S = 0; size = 1; opc = 4; }
        unsigned R = (nst == 2 || nst == 4), op3 = nst >= 3;
        opc |= op3;
        EMIT(0x0D000000u | Q << 30 | post << 23 | (unsigned)L << 22 | R << 21 | rm << 16 | opc << 13 |
             S << 12 | size << 10 | (unsigned)o[1].base << 5 | (unsigned)l->r);
        return 1;
    }
    unsigned opc;
    if (nst == 1) opc = l->nregs == 1 ? 7 : l->nregs == 2 ? 10 : l->nregs == 3 ? 6 : 2;
    else {
        if (l->nregs != nst) BAD("'%s' transfers %d registers", mn, nst);
        opc = nst == 2 ? 8 : nst == 3 ? 4 : 0;
    }
    EMIT(0x0C000000u | (unsigned)vq(l) << 30 | post << 23 | (unsigned)L << 22 | rm << 16 | opc << 12 |
         (unsigned)log2sz(l->esz) << 10 | (unsigned)o[1].base << 5 | (unsigned)l->r);
    return 1;
}

static int enc_simd(struct actx *a, const char *mn0, struct op *o, int n)
{
    char mn[16];
    snprintf(mn, sizeof mn, "%s", mn0);
    struct op *d = &o[0];
    int r = enc_ldst_simd(a, mn, o, n);
    if (r) return r;
    /* copies and the mov aliases */
    if (!strcmp(mn, "mov") || !strcmp(mn, "ins") || !strcmp(mn, "umov") || !strcmp(mn, "smov") ||
        !strcmp(mn, "dup")) {
        if (n != 2) BAD("'%s' takes two operands", mn);
        struct op *s = &o[1];
        if (!strcmp(mn, "mov") && d->k == K_VEC && s->k == K_VEC) {      /* orr */
            EMIT(0x0EA01C00u | (unsigned)vq(d) << 30 | (unsigned)s->r << 16 | (unsigned)s->r << 5 | (unsigned)d->r);
            return 1;
        }
        if (d->k == K_ELEM && s->k == K_GPR && strcmp(mn, "dup")) {      /* ins general */
            EMIT(0x4E001C00u | imm5_of(d->esz, d->idx) << 16 | (unsigned)s->r << 5 | (unsigned)d->r);
            return 1;
        }
        if (d->k == K_ELEM && s->k == K_ELEM) {                          /* ins element */
            if (d->esz != s->esz) BAD("the lanes are one size");
            EMIT(0x6E000400u | imm5_of(d->esz, d->idx) << 16 | (unsigned)(s->idx << log2sz(s->esz)) << 11 |
                 (unsigned)s->r << 5 | (unsigned)d->r);
            return 1;
        }
        if (d->k == K_GPR && s->k == K_ELEM) {                           /* umov / smov */
            int sm = !strcmp(mn, "smov");
            if (!sm && ((s->esz == 8) != d->x || (strcmp(mn, "umov") && s->esz < 4)))
                if (!(strcmp(mn, "umov") == 0 && s->esz < 4 && !d->x))
                    BAD("'%s' moves an S lane to a W register, a D lane to an X", mn);
            EMIT((sm ? 0x0E002C00u : 0x0E003C00u) | (unsigned)d->x << 30 | imm5_of(s->esz, s->idx) << 16 |
                 (unsigned)s->r << 5 | (unsigned)d->r);
            return 1;
        }
        if (d->k == K_FPR && s->k == K_ELEM) {                           /* dup scalar */
            EMIT(0x5E000400u | imm5_of(s->esz, s->idx) << 16 | (unsigned)s->r << 5 | (unsigned)d->r);
            return 1;
        }
        if (!strcmp(mn, "dup") && d->k == K_VEC && s->k == K_ELEM) {
            EMIT(0x0E000400u | (unsigned)vq(d) << 30 | imm5_of(s->esz, s->idx) << 16 | (unsigned)s->r << 5 | (unsigned)d->r);
            return 1;
        }
        if (!strcmp(mn, "dup") && d->k == K_VEC && s->k == K_GPR) {
            EMIT(0x0E000C00u | (unsigned)vq(d) << 30 | imm5_of(d->esz, 0) << 16 | (unsigned)s->r << 5 | (unsigned)d->r);
            return 1;
        }
        BAD("'%s' does not take these operands", mn);
    }
    if (!strcmp(mn, "movi") || !strcmp(mn, "mvni") ||
        ((!strcmp(mn, "orr") || !strcmp(mn, "bic")) && n >= 2 && o[1].k == K_IMM) ||
        (!strcmp(mn, "fmov") && d->k == K_VEC))
        return enc_movi(a, mn, d, o, n);
    /* across lanes: the destination a scalar */
    static const struct { const char *m; int u, opc; } across[] = {
        { "saddlv", 0, 0x03 }, { "uaddlv", 1, 0x03 }, { "smaxv", 0, 0x0a }, { "umaxv", 1, 0x0a },
        { "sminv", 0, 0x1a }, { "uminv", 1, 0x1a }, { "addv", 0, 0x1b },
    };
    for (unsigned i = 0; i < sizeof across / sizeof across[0]; i++)
        if (!strcmp(mn, across[i].m) && n == 2 && d->k == K_FPR && o[1].k == K_VEC) {
            EMIT(0x0E300800u | (unsigned)vq(&o[1]) << 30 | (unsigned)across[i].u << 29 |
                 (unsigned)log2sz(o[1].esz) << 22 | (unsigned)across[i].opc << 12 |
                 (unsigned)o[1].r << 5 | (unsigned)d->r);
            return 1;
        }
    if ((!strcmp(mn, "fmaxv") || !strcmp(mn, "fminv") || !strcmp(mn, "fmaxnmv") || !strcmp(mn, "fminnmv")) && n == 2) {
        unsigned op = !strcmp(mn, "fmaxv") ? 0x2E30F800u : !strcmp(mn, "fminv") ? 0x2EB0F800u
                    : !strcmp(mn, "fmaxnmv") ? 0x2E30C800u : 0x2EB0C800u;
        EMIT(op | 1u << 30 | (unsigned)o[1].r << 5 | (unsigned)d->r);
        return 1;
    }
    /* pairwise scalar: addp d0, v1.2d; faddp s0, v1.2s */
    if (!strcmp(mn, "addp") && n == 2 && d->k == K_FPR) {
        EMIT(0x5EF1B800u | (unsigned)o[1].r << 5 | (unsigned)d->r);
        return 1;
    }
    if ((!strcmp(mn, "faddp") || !strcmp(mn, "fmaxp") || !strcmp(mn, "fminp") ||
         !strcmp(mn, "fmaxnmp") || !strcmp(mn, "fminnmp")) && n == 2 && d->k == K_FPR) {
        unsigned op = !strcmp(mn, "faddp") ? 0x7E30D800u : !strcmp(mn, "fmaxp") ? 0x7E30F800u
                    : !strcmp(mn, "fminp") ? 0x7EB0F800u : !strcmp(mn, "fmaxnmp") ? 0x7E30C800u : 0x7EB0C800u;
        EMIT(op | (d->fsz == 8 ? 1u << 22 : 0) | (unsigned)o[1].r << 5 | (unsigned)d->r);
        return 1;
    }
    /* permutes, ext, tbl/tbx */
    static const struct { const char *m; int opc; } perm[] = {
        { "uzp1", 1 }, { "trn1", 2 }, { "zip1", 3 }, { "uzp2", 5 }, { "trn2", 6 }, { "zip2", 7 },
    };
    for (unsigned i = 0; i < 6; i++)
        if (!strcmp(mn, perm[i].m) && n == 3) {
            EMIT(0x0E000800u | (unsigned)vq(d) << 30 | (unsigned)log2sz(d->esz) << 22 | (unsigned)o[2].r << 16 |
                 (unsigned)perm[i].opc << 12 | (unsigned)o[1].r << 5 | (unsigned)d->r);
            return 1;
        }
    if (!strcmp(mn, "ext") && n == 4) {
        if (o[3].k != K_IMM || o[3].v < 0 || o[3].v >= (vq(d) ? 16 : 8)) BAD("'ext' takes a byte index");
        EMIT(0x2E000000u | (unsigned)vq(d) << 30 | (unsigned)o[2].r << 16 | (unsigned)o[3].v << 11 |
             (unsigned)o[1].r << 5 | (unsigned)d->r);
        return 1;
    }
    if ((!strcmp(mn, "tbl") || !strcmp(mn, "tbx")) && n == 3 && o[1].k == K_LIST) {
        EMIT(0x0E000000u | (unsigned)vq(d) << 30 | (unsigned)o[2].r << 16 | (unsigned)(o[1].nregs - 1) << 13 |
             (unsigned)(mn[2] == 'x') << 12 | (unsigned)o[1].r << 5 | (unsigned)d->r);
        return 1;
    }
    /* the long, wide and narrow three-register forms, and their `2`s */
    {
        size_t l = strlen(mn);
        int two = l > 1 && mn[l - 1] == '2';
        char base[16];
        snprintf(base, sizeof base, "%.*s", (int)(two ? l - 1 : l), mn);
        for (unsigned i = 0; i < sizeof vlong / sizeof vlong[0]; i++)
            if (!strcmp(base, vlong[i].m) && n == 3 && o[2].k == K_VEC) {
                int narrow = vlong[i].opc == 4 || vlong[i].opc == 6;
                int wide = vlong[i].opc == 1 || vlong[i].opc == 3;
                int esz = narrow ? d->esz : wide ? o[2].esz : o[1].esz;
                EMIT(0x0E200000u | (unsigned)two << 30 | (unsigned)vlong[i].u << 29 | (unsigned)log2sz(esz) << 22 |
                     (unsigned)o[2].r << 16 | (unsigned)vlong[i].opc << 12 | (unsigned)o[1].r << 5 | (unsigned)d->r);
                return 1;
            }
        /* shifts by an immediate, and sxtl/uxtl */
        if ((!strcmp(base, "sxtl") || !strcmp(base, "uxtl")) && n == 2) {
            int esz = o[1].esz * 8;
            EMIT(0x0F00A400u | (unsigned)two << 30 | (unsigned)(base[0] == 'u') << 29 | (unsigned)esz << 16 |
                 (unsigned)o[1].r << 5 | (unsigned)d->r);
            return 1;
        }
        for (unsigned i = 0; i < sizeof vshift / sizeof vshift[0]; i++)
            if (!strcmp(base, vshift[i].m) && n == 3 && o[2].k == K_IMM && o[1].k != K_GPR) {
                int scalar = d->k == K_FPR;
                int k = vshift[i].kind;
                int esz = scalar ? d->fsz * 8 : k == 'n' ? d->esz * 8 : k == 'w' ? o[1].esz * 8 : d->esz * 8;
                int sh = (int)o[2].v;
                unsigned ih;
                if (k == 'l' || k == 'w') {
                    if (sh < 0 || sh >= esz) BAD("shift %d is outside the %d-bit element", sh, esz);
                    ih = (unsigned)(esz + sh);
                } else {
                    if (sh < 1 || sh > esz) BAD("shift %d is not 1 to %d", sh, esz);
                    ih = (unsigned)(2 * esz - sh);
                }
                unsigned Q = scalar ? 0 : (k == 'n' || k == 'w') ? (unsigned)two : (unsigned)vq(d);
                EMIT((scalar ? 0x5F000400u : 0x0F000400u) | Q << 30 | (unsigned)vshift[i].u << 29 | ih << 16 |
                     (unsigned)vshift[i].opc << 11 | (unsigned)o[1].r << 5 | (unsigned)d->r);
                return 1;
            }
        /* narrowing two-register: xtn, sqxtn, uqxtn, sqxtun, fcvtn, fcvtl */
        static const struct { const char *m; unsigned op; int src; } nar[] = {
            { "xtn", 0x0E212800u, 0 }, { "sqxtun", 0x2E212800u, 0 }, { "sqxtn", 0x0E214800u, 0 },
            { "uqxtn", 0x2E214800u, 0 }, { "fcvtn", 0x0E216800u, 2 }, { "fcvtxn", 0x2E616800u, 2 },
            { "fcvtl", 0x0E217800u, 1 }, { "shll", 0x2E213800u, 3 },
        };
        for (unsigned i = 0; i < sizeof nar / sizeof nar[0]; i++)
            if (!strcmp(base, nar[i].m) && n >= 2) {
                unsigned w = nar[i].op | (unsigned)two << 30 | (unsigned)o[1].r << 5 | (unsigned)d->r;
                if (nar[i].src == 0) w |= (unsigned)log2sz(d->esz) << 22;
                else if (nar[i].src == 1) w |= (d->esz == 8 ? 1u : 0) << 22;     /* fcvtl to .2d */
                else if (nar[i].src == 2) w |= (o[1].esz == 8 ? 1u : 0) << 22;   /* fcvtn from .2d */
                else w |= (unsigned)log2sz(o[1].esz) << 22;
                EMIT(w);
                return 1;
            }
    }
    /* by element: mul v0.4s, v1.4s, v2.s[1]; fmla v0.2d, v1.2d, v2.d[0];
     * smull2 v0.4s, v1.8h, v2.h[3]; and the scalar fmul s0, s1, v2.s[1] */
    if (n == 3 && o[2].k == K_ELEM) {
        static const struct { const char *m; int u, opc, fp, lng; } be[] = {
            { "mla", 1, 0x0, 0, 0 }, { "mls", 1, 0x4, 0, 0 }, { "mul", 0, 0x8, 0, 0 },
            { "sqdmulh", 0, 0xc, 0, 0 }, { "sqrdmulh", 0, 0xd, 0, 0 },
            { "smlal", 0, 0x2, 0, 1 }, { "umlal", 1, 0x2, 0, 1 }, { "smlsl", 0, 0x6, 0, 1 },
            { "umlsl", 1, 0x6, 0, 1 }, { "smull", 0, 0xa, 0, 1 }, { "umull", 1, 0xa, 0, 1 },
            { "sqdmull", 0, 0xb, 0, 1 }, { "sqdmlal", 0, 0x3, 0, 1 }, { "sqdmlsl", 0, 0x7, 0, 1 },
            { "fmla", 0, 0x1, 1, 0 }, { "fmls", 0, 0x5, 1, 0 }, { "fmul", 0, 0x9, 1, 0 },
            { "fmulx", 1, 0x9, 1, 0 },
        };
        size_t l = strlen(mn);
        int two = l > 1 && mn[l - 1] == '2';
        char base[16];
        snprintf(base, sizeof base, "%.*s", (int)(two ? l - 1 : l), mn);
        for (unsigned i = 0; i < sizeof be / sizeof be[0]; i++) {
            if (strcmp(base, be[i].m) || (two && !be[i].lng))
                continue;
            struct op *e = &o[2];
            int scalar = d->k == K_FPR;
            int esz = e->esz, size = be[i].fp ? 2 + (esz == 8) : log2sz(esz);
            unsigned H, L = 0, M = 0, rm = (unsigned)e->r;
            if (esz == 2) {
                if (e->r > 15) BAD("a .h element is of v0 to v15");
                H = (unsigned)e->idx >> 2; L = (unsigned)(e->idx >> 1) & 1; M = (unsigned)e->idx & 1;
            } else if (esz == 4) {
                H = (unsigned)e->idx >> 1; L = (unsigned)e->idx & 1; M = rm >> 4; rm &= 15;
            } else if (esz == 8 && be[i].fp) {
                H = (unsigned)e->idx; M = rm >> 4; rm &= 15;
            } else BAD("'%s' takes an .h or .s element%s", mn, be[i].fp ? ", or a .d" : "");
            unsigned Q = scalar ? 0 : be[i].lng ? (unsigned)two : (unsigned)vq(d);
            EMIT((scalar ? 0x5F000000u : 0x0F000000u) | Q << 30 | (unsigned)be[i].u << 29 |
                 (unsigned)size << 22 | L << 21 | M << 20 | rm << 16 | (unsigned)be[i].opc << 12 |
                 H << 11 | (unsigned)o[1].r << 5 | (unsigned)d->r);
            return 1;
        }
    }
    /* compares with zero: cmeq v, v, #0 (vector or scalar) */
    static const struct { const char *m; int u, opc; } cz[] = {
        { "cmgt", 0, 0x08 }, { "cmge", 1, 0x08 }, { "cmeq", 0, 0x09 }, { "cmle", 1, 0x09 }, { "cmlt", 0, 0x0a },
    };
    if (n == 3 && o[2].k == K_IMM && o[2].v == 0)
        for (unsigned i = 0; i < 5; i++)
            if (!strcmp(mn, cz[i].m)) {
                int scalar = d->k == K_FPR;
                int esz = scalar ? d->fsz : d->esz;
                EMIT((scalar ? 0x5E200800u : 0x0E200800u) | (unsigned)(scalar ? 0 : vq(d)) << 30 |
                     (unsigned)cz[i].u << 29 | (unsigned)log2sz(esz) << 22 | (unsigned)cz[i].opc << 12 |
                     (unsigned)o[1].r << 5 | (unsigned)d->r);
                return 1;
            }
    static const struct { const char *m; unsigned op; } fcz[] = {
        { "fcmgt", 0x0EA0C800u }, { "fcmeq", 0x0EA0D800u }, { "fcmlt", 0x0EA0E800u },
        { "fcmge", 0x2EA0C800u }, { "fcmle", 0x2EA0D800u },
    };
    if (n == 3 && (o[2].k == K_FIMM || o[2].k == K_IMM) && d->k == K_VEC)
        for (unsigned i = 0; i < 5; i++)
            if (!strcmp(mn, fcz[i].m)) {
                EMIT(fcz[i].op | (unsigned)vq(d) << 30 | (d->esz == 8 ? 1u : 0) << 22 |
                     (unsigned)o[1].r << 5 | (unsigned)d->r);
                return 1;
            }
    /* the three-same group, vector or scalar */
    if (n == 3 && (o[2].k == K_VEC || (d->k == K_FPR && o[2].k == K_FPR))) {
        int scalar = d->k == K_FPR;
        for (unsigned i = 0; i < sizeof f3same / sizeof f3same[0]; i++)
            if (!strcmp(mn, f3same[i].m) && !scalar) {
                if (d->esz != 4 && d->esz != 8) BAD("'%s' is of .2s, .4s or .2d", mn);
                EMIT(f3same[i].op | (unsigned)vq(d) << 30 | (d->esz == 8 ? 1u : 0) << 22 |
                     (unsigned)o[2].r << 16 | (unsigned)o[1].r << 5 | (unsigned)d->r);
                return 1;
            }
        for (unsigned i = 0; i < sizeof v3same / sizeof v3same[0]; i++)
            if (!strcmp(mn, v3same[i].m)) {
                int size = v3same[i].fixsz >= 0 ? v3same[i].fixsz : log2sz(scalar ? d->fsz : d->esz);
                EMIT((scalar ? 0x5E200400u : 0x0E200400u) | (unsigned)(scalar ? 0 : vq(d)) << 30 |
                     (unsigned)v3same[i].u << 29 | (unsigned)size << 22 | (unsigned)o[2].r << 16 |
                     (unsigned)v3same[i].opc << 11 | (unsigned)o[1].r << 5 | (unsigned)d->r);
                return 1;
            }
    }
    /* two-register misc, vector (and the scalar FP conversions) */
    if (n == 2) {
        if (d->k == K_VEC) {
            for (unsigned i = 0; i < sizeof f2misc / sizeof f2misc[0]; i++)
                if (!strcmp(mn, f2misc[i].m)) {
                    EMIT(f2misc[i].op | (unsigned)vq(d) << 30 | (d->esz == 8 ? 1u : 0) << 22 |
                         (unsigned)o[1].r << 5 | (unsigned)d->r);
                    return 1;
                }
            for (unsigned i = 0; i < sizeof v2misc / sizeof v2misc[0]; i++)
                if (!strcmp(mn, v2misc[i].m)) {
                    int size = v2misc[i].sz >= 0 ? v2misc[i].sz
                             : log2sz(v2misc[i].sz == -2 ? o[1].esz : d->esz);
                    EMIT(0x0E200800u | (unsigned)vq(d) << 30 | (unsigned)v2misc[i].u << 29 | (unsigned)size << 22 |
                         (unsigned)v2misc[i].opc << 12 | (unsigned)o[1].r << 5 | (unsigned)d->r);
                    return 1;
                }
        }
        if (d->k == K_FPR && o[1].k == K_FPR) {
            /* scalar SIMD: fcvtzs s0, s1 ; scvtf d0, d1 ; abs/neg d0, d1 */
            for (unsigned i = 0; i < sizeof f2misc / sizeof f2misc[0]; i++)
                if (!strcmp(mn, f2misc[i].m) && strncmp(mn, "frint", 5) && strcmp(mn, "fabs") &&
                    strcmp(mn, "fneg") && strcmp(mn, "fsqrt")) {
                    EMIT((f2misc[i].op | 0x50000000u) | (d->fsz == 8 ? 1u : 0) << 22 |
                         (unsigned)o[1].r << 5 | (unsigned)d->r);
                    return 1;
                }
            if (!strcmp(mn, "abs") || !strcmp(mn, "neg")) {
                EMIT((mn[0] == 'a' ? 0x5EE0B800u : 0x7EE0B800u) | (unsigned)o[1].r << 5 | (unsigned)d->r);
                return 1;
            }
        }
    }
    return 0;
}

/* ---- the dispatcher ------------------------------------------------------- */

/* 1 when the statement was one of these (assembled, or refused with a
 * message), 0 when the mnemonic is none of them. */
static int a64_general(struct actx *a, const char *mn, char *rest)
{
    char *raw[8];
    struct op o[8];
    int n = split_operands(rest, raw, 8);
    for (int i = 0; i < n; i++) {
        parse_op(a, raw[i], &o[i]);
        if (a->failed) return 1;
    }
    int simd = 0;
    for (int i = 0; i < n; i++)
        if (o[i].k == K_VEC || o[i].k == K_ELEM || o[i].k == K_LIST)
            simd = 1;
    if (simd)
        return enc_simd(a, mn, o, n);
    if (enc_fcvt_int(a, mn, o, n) || enc_ldst(a, mn, o, n) || enc_ldstp(a, mn, o, n) ||
        enc_excl(a, mn, o, n) || enc_lse(a, mn, o, n) || enc_branch(a, mn, o, n))
        return 1;
    if (n && o[0].k == K_FPR)
        return enc_fp(a, mn, o, n) || enc_simd(a, mn, o, n);
    if (enc_fp(a, mn, o, n))
        return 1;
    return enc_dp(a, mn, o, n);
}

/* ---- what gas.c asks ---------------------------------------------------- */

static int a64_is_name(const char *s, int len)
{
    char b[32];
    if (len <= 0 || len >= (int)sizeof b) return 0;
    memcpy(b, s, (size_t)len);
    b[len] = 0;
    int r, x, sp, z, e, c;
    return gpr_name(b, &r, &x, &sp) || fpr_name(b, &r, &z) || vec_name(b, &r, &e, &c);
}

/* a register's number, or -1, as gas.c's other targets answer */
int a64asm_is_reg(const char *name, int len)
{
    return a64_is_name(name, len) ? 0 : -1;
}

int a64asm_is_word(const char *stmt, const char *w, int len)
{
    (void)stmt;
    char b[40];
    if (len <= 0 || len >= (int)sizeof b) return 0;
    for (int i = 0; i < len; i++) b[i] = (char)tolower((unsigned char)w[i]);
    b[len] = 0;
    if (cond_of(b) >= 0) return 1;
    static const char *const words[] = {
        "lsl", "lsr", "asr", "ror", "msl", "uxtb", "uxth", "uxtw", "uxtx", "sxtb", "sxth",
        "sxtw", "sxtx", "daifset", "daifclr", "spsel", "pan", "uao", "dit", "ssbs", "csync",
        "lo12", "got", "got_lo12", "abs_g0", "abs_g0_nc", "abs_g1", "abs_g1_nc", "abs_g2",
        "abs_g2_nc", "abs_g3", "abs_g0_s", "abs_g1_s", "abs_g2_s", "tprel_hi12",
        "tprel_lo12", "tprel_lo12_nc", "gottprel", "gottprel_lo12", "tlsdesc", "tlsdesc_lo12",
        "pg_hi21", "pg_hi21_nc", NULL
    };
    for (int i = 0; words[i]; i++)
        if (!strcmp(b, words[i])) return 1;
    for (unsigned i = 0; i < sizeof sysregs / sizeof sysregs[0]; i++)
        if (!strcmp(b, sysregs[i].name)) return 1;
    for (unsigned i = 0; i < sizeof barrier_opts / sizeof barrier_opts[0]; i++)
        if (!strcmp(b, barrier_opts[i].name)) return 1;
    for (unsigned i = 0; i < sizeof tlbis / sizeof tlbis[0]; i++)
        if (!strcmp(b, tlbis[i].name)) return 1;
    for (unsigned i = 0; i < sizeof cmaints / sizeof cmaints[0]; i++)
        if (!strcmp(b, cmaints[i].name)) return 1;
    for (unsigned i = 0; i < sizeof prfops / sizeof prfops[0]; i++)
        if (!strcmp(b, prfops[i].name)) return 1;
    /* the generic system register, s3_0_c12_c12_5 */
    if (b[0] == 's' && isdigit((unsigned char)b[1]) && b[2] == '_') return 1;
    return 0;
}

static int mn_of(const char *stmt, char *mn, int cap, const char **rest)
{
    const char *p = stmt;
    while (isspace((unsigned char)*p)) p++;
    int i = 0;
    while (*p && !isspace((unsigned char)*p) && i < cap - 1)
        mn[i++] = (char)tolower((unsigned char)*p++);
    mn[i] = 0;
    *rest = p;
    return i;
}

/* a branch or a literal: its label, when this file defines it in the same
 * section, is a displacement, not a relocation */
int a64asm_is_transfer(const char *stmt)
{
    char mn[16];
    const char *rest;
    mn_of(stmt, mn, sizeof mn, &rest);
    if (!strcmp(mn, "b") || !strcmp(mn, "bl") || (mn[0] == 'b' && mn[1] == '.') ||
        !strcmp(mn, "cbz") || !strcmp(mn, "cbnz") || !strcmp(mn, "tbz") || !strcmp(mn, "tbnz") ||
        !strcmp(mn, "adr"))
        return 1;
    /* ldr rt, label (no bracket): a literal */
    if ((!strcmp(mn, "ldr") || !strcmp(mn, "ldrsw") || !strcmp(mn, "prfm")) && !strchr(rest, '['))
        return 1;
    return 0;
}

static int sym0(int c) { return isalpha(c) || c == '_' || c == '.' || c == '$'; }
static int symc(int c) { return isalnum(c) || c == '_' || c == '.' || c == '$'; }

/* the access size of a load or store, for its :lo12: relocation */
static int ldst_bytes(const char *mn, const char *rest)
{
    if (!strcmp(mn, "ldrb") || !strcmp(mn, "strb") || !strcmp(mn, "ldrsb")) return 1;
    if (!strcmp(mn, "ldrh") || !strcmp(mn, "strh") || !strcmp(mn, "ldrsh")) return 2;
    if (!strcmp(mn, "ldrsw")) return 4;
    if (!strcmp(mn, "prfm")) return 8;
    while (isspace((unsigned char)*rest)) rest++;
    int c = tolower((unsigned char)*rest);
    return c == 'x' || c == 'd' ? 8 : c == 'w' || c == 's' ? 4 : c == 'q' ? 16
         : c == 'h' ? 2 : c == 'b' ? 1 : 8;
}

int a64asm_symform(const char *stmt, struct asm_symform *f)
{
    char mn[16];
    const char *rest;
    memset(f, 0, sizeof *f);
    mn_of(stmt, mn, sizeof mn, &rest);
    if (!mn[0] || mn[0] == '.')
        return 0;
    /* a `:modifier:symbol` operand */
    const char *c = strchr(rest, ':');
    const char *s = NULL, *se = NULL;
    const char *rep_at = NULL, *rep_end = NULL;
    int reloc = 0, hw = -1;
    if (c) {
        const char *me = strchr(c + 1, ':');
        if (!me) return 0;
        char mod[24];
        int ml = (int)(me - c - 1);
        if (ml <= 0 || ml >= (int)sizeof mod) return 0;
        for (int i = 0; i < ml; i++) mod[i] = (char)tolower((unsigned char)c[1 + i]);
        mod[ml] = 0;
        s = me + 1;
        if (!sym0((unsigned char)*s)) return 0;
        se = s;
        while (symc((unsigned char)*se)) se++;
        rep_at = c;
        if (rep_at > rest && rep_at[-1] == '#') rep_at--;
        int is_add = !strcmp(mn, "add") || !strcmp(mn, "adds");
        int is_mem = strchr(rest, '[') != NULL;
        int lb = is_mem ? ldst_bytes(mn, rest) : 0;
        if (!strcmp(mod, "lo12"))
            reloc = is_add ? R_AARCH64_ADD_ABS_LO12_NC
                  : lb == 1 ? R_AARCH64_LDST8_ABS_LO12_NC : lb == 2 ? R_AARCH64_LDST16_ABS_LO12_NC
                  : lb == 4 ? R_AARCH64_LDST32_ABS_LO12_NC : lb == 8 ? R_AARCH64_LDST64_ABS_LO12_NC
                  : lb == 16 ? R_AARCH64_LDST128_ABS_LO12_NC : 0;
        else if (!strcmp(mod, "got") && !strcmp(mn, "adrp")) reloc = R_AARCH64_ADR_GOT_PAGE;
        else if (!strcmp(mod, "got_lo12") && is_mem) reloc = R_AARCH64_LD64_GOT_LO12_NC;
        else if (!strcmp(mod, "tprel_hi12") && is_add) reloc = R_AARCH64_TLSLE_ADD_TPREL_HI12;
        else if (!strcmp(mod, "tprel_lo12_nc") && is_add) reloc = R_AARCH64_TLSLE_ADD_TPREL_LO12_NC;
        else if (!strcmp(mod, "tprel_lo12") && is_add) reloc = R_AARCH64_TLSLE_ADD_TPREL_LO12;
        else if (!strcmp(mod, "gottprel") && !strcmp(mn, "adrp")) reloc = R_AARCH64_TLSIE_ADR_GOTTPREL_PAGE21;
        else if (!strcmp(mod, "gottprel_lo12") && is_mem) reloc = R_AARCH64_TLSIE_LD64_GOTTPREL_LO12_NC;
        else if (!strcmp(mod, "tlsdesc") && !strcmp(mn, "adrp")) reloc = R_AARCH64_TLSDESC_ADR_PAGE21;
        else if (!strcmp(mod, "tlsdesc_lo12")) reloc = is_mem ? R_AARCH64_TLSDESC_LD64_LO12 : R_AARCH64_TLSDESC_ADD_LO12;
        else if (!strcmp(mod, "pg_hi21") && !strcmp(mn, "adrp")) reloc = R_AARCH64_ADR_PREL_PG_HI21;
        else if (!strcmp(mod, "pg_hi21_nc") && !strcmp(mn, "adrp")) reloc = R_AARCH64_ADR_PREL_PG_HI21_NC;
        else if (!strncmp(mod, "abs_g", 5) && (!strcmp(mn, "movz") || !strcmp(mn, "movk") || !strcmp(mn, "movn"))) {
            int g = mod[5] - '0';
            const char *t = mod + 6;
            hw = g;
            if (!*t) reloc = g == 0 ? R_AARCH64_MOVW_UABS_G0 : g == 1 ? R_AARCH64_MOVW_UABS_G1
                           : g == 2 ? R_AARCH64_MOVW_UABS_G2 : R_AARCH64_MOVW_UABS_G3;
            else if (!strcmp(t, "_nc") && g < 3) reloc = R_AARCH64_MOVW_UABS_G0_NC + 2 * g;
            else if (!strcmp(t, "_s") && g < 3) reloc = R_AARCH64_MOVW_SABS_G0 + g;
        }
        if (!reloc) {
            snprintf(f->encode, sizeof f->encode, "\001:%s: is not a relocation '%s' can carry", mod, mn);
            f->sym_at = (int)(s - stmt);
            f->sym_len = (int)(se - s);
            return 1;
        }
    } else {
        /* a bare symbol: the branches, adr/adrp, a literal load */
        int kind = 0;
        if (!strcmp(mn, "b")) kind = R_AARCH64_JUMP26;
        else if (!strcmp(mn, "bl")) kind = R_AARCH64_CALL26;
        else if (mn[0] == 'b' && mn[1] == '.' && cond_of(mn + 2) >= 0) kind = R_AARCH64_CONDBR19;
        else if (!strcmp(mn, "cbz") || !strcmp(mn, "cbnz")) kind = R_AARCH64_CONDBR19;
        else if (!strcmp(mn, "tbz") || !strcmp(mn, "tbnz")) kind = R_AARCH64_TSTBR14;
        else if (!strcmp(mn, "adrp")) kind = R_AARCH64_ADR_PREL_PG_HI21;
        else if (!strcmp(mn, "adr")) kind = R_AARCH64_ADR_PREL_LO21;
        else if ((!strcmp(mn, "ldr") || !strcmp(mn, "ldrsw") || !strcmp(mn, "prfm")) && !strchr(rest, '['))
            kind = R_AARCH64_LD_PREL_LO19;
        if (!kind) return 0;
        /* the symbol is the last operand */
        const char *last = strrchr(rest, ',');
        const char *p = last ? last + 1 : rest;
        while (isspace((unsigned char)*p)) p++;
        if (!sym0((unsigned char)*p)) return 0;
        const char *q = p;
        while (symc((unsigned char)*q)) q++;
        if (a64_is_name(p, (int)(q - p)) || a64asm_is_word(stmt, p, (int)(q - p)))
            return 0;
        s = p; se = q; rep_at = p;
        reloc = kind;
    }
    /* an addend: sym+8, sym-4 */
    const char *t = se;
    long add = 0;
    while (isspace((unsigned char)*t)) t++;
    if (*t == '+' || *t == '-') {
        char *e;
        long v = strtol(t + 1, &e, 0);
        if (e != t + 1) { add = *t == '-' ? -v : v; t = e; }
    }
    rep_end = t;
    f->sym_at = (int)(s - stmt);
    f->sym_len = (int)(se - s);
    f->addend = add;
    const char *zero = rep_at[0] == '#' || c ? "#0" : "0";
    char shift[16] = "";
    if (hw > 0) snprintf(shift, sizeof shift, ", lsl #%d", 16 * hw);
    snprintf(f->encode, sizeof f->encode, "%.*s%s%s%s", (int)(rep_at - stmt), stmt, zero, rep_end, shift);
    f->site[0].off = 0;
    f->site[0].reloc = reloc;
    f->nsites = 1;
    return 1;
}

/* ---- one statement --------------------------------------------------- */

static void assemble_stmt(struct actx *a, char *st)
{
    char *p = st;
    while (*p && !isspace((unsigned char)*p))
        p++;
    char mn[16];
    size_t mlen = (size_t)(p - st);
    if (mlen >= sizeof mn) {
        fail(a, "unknown instruction");
        return;
    }
    for (size_t i = 0; i < mlen; i++)
        mn[i] = (char)tolower((unsigned char)st[i]);
    mn[mlen] = '\0';

    if (mlen && mn[mlen - 1] == ':') {
        fail(a, "labels in aarch64 inline asm are not supported yet");
        return;
    }

    char rest[512];
    snprintf(rest, sizeof rest, "%s", p);
    char *ops[4];
    int n = split_operands(p, ops, 4);

    for (unsigned i = 0; i < sizeof hints / sizeof hints[0]; i++)
        if (strcmp(mn, hints[i].name) == 0) {
            if (n != 0) { fail(a, "'%s' takes no operands", mn); return; }
            a64_hint(a->out, hints[i].imm);
            return;
        }

    if (strcmp(mn, "dsb") == 0 || strcmp(mn, "dmb") == 0 ||
        strcmp(mn, "isb") == 0) {
        int is_isb = mn[0] == 'i';
        int crm = -1;
        if (n == 0 && is_isb) {
            crm = 15;                 /* a bare isb means isb sy */
        } else if (n == 1) {
            for (unsigned i = 0; i < sizeof barrier_opts / sizeof barrier_opts[0]; i++)
                if (ieq(ops[0], barrier_opts[i].name))
                    crm = barrier_opts[i].crm;
            if (is_isb && crm != 15)  /* isb has only the sy option */
                crm = -1;
        }
        if (crm < 0) {
            fail(a, n ? "unknown barrier option '%s'" : "'%s' needs a barrier option",
                 n ? ops[0] : mn);
            return;
        }
        a64_barrier(a->out, is_isb ? 'i' : mn[1] == 's' ? 'd' : 'm', crm);
        return;
    }

    if (strcmp(mn, "mrs") == 0) {
        if (n != 2) { fail(a, "'mrs' takes a register and a system register"); return; }
        int w = 0, rt = gpr(a, ops[0], &w);
        if (w) { fail(a, "'mrs' needs an X register, got '%s'", ops[0]); return; }
        int sr = sysreg(a, ops[1]);
        if (!a->failed) a64_mrs(a->out, rt, sr);
        return;
    }

    if (strcmp(mn, "msr") == 0) {
        if (n != 2) { fail(a, "'msr' takes a system register and an operand"); return; }
        if (ieq(ops[0], "daifset") || ieq(ops[0], "daifclr")) {
            long v = imm(a, ops[1]);
            if (!a->failed)
                a64_msr_pstate(a->out, 3, ieq(ops[0], "daifset") ? 6 : 7, (int)v);
            return;
        }
        int sr = sysreg(a, ops[0]);
        int w = 0, rt = gpr(a, ops[1], &w);
        if (w) { fail(a, "'msr' needs an X register, got '%s'", ops[1]); return; }
        if (!a->failed) a64_msr(a->out, sr, rt);
        return;
    }

    if (strcmp(mn, "tlbi") == 0) {
        if (n < 1) { fail(a, "'tlbi' needs an operation"); return; }
        for (unsigned i = 0; i < sizeof tlbis / sizeof tlbis[0]; i++)
            if (ieq(ops[0], tlbis[i].name)) {
                int rt = 31;
                if (tlbis[i].reg) {
                    if (n != 2) { fail(a, "'tlbi %s' needs a register", ops[0]); return; }
                    int w = 0;
                    rt = gpr(a, ops[1], &w);
                    if (w) { fail(a, "'tlbi' needs an X register"); return; }
                } else if (n != 1) {
                    fail(a, "'tlbi %s' takes no register", ops[0]);
                    return;
                }
                if (!a->failed)
                    a64_sys(a->out, tlbis[i].op1, 8, tlbis[i].crm, tlbis[i].op2, rt);
                return;
            }
        fail(a, "unknown tlbi operation '%s'", ops[0]);
        return;
    }

    if (strcmp(mn, "dc") == 0 || strcmp(mn, "ic") == 0) {
        if (n < 1) { fail(a, "'%s' needs an operation", mn); return; }
        for (unsigned i = 0; i < sizeof cmaints / sizeof cmaints[0]; i++)
            if (strcmp(mn, cmaints[i].mn) == 0 &&
                ieq(ops[0], cmaints[i].name)) {
                int rt = 31;
                if (cmaints[i].reg) {
                    if (n != 2) {
                        fail(a, "'%s %s' needs a register", mn, ops[0]);
                        return;
                    }
                    int w = 0;
                    rt = gpr(a, ops[1], &w);
                    if (w) { fail(a, "'%s' needs an X register", mn); return; }
                } else if (n != 1) {
                    fail(a, "'%s %s' takes no register", mn, ops[0]);
                    return;
                }
                if (!a->failed)
                    a64_sys(a->out, cmaints[i].op1, 7, cmaints[i].crm,
                            cmaints[i].op2, rt);
                return;
            }
        fail(a, "unknown %s operation '%s'", mn, ops[0]);
        return;
    }

    if (strcmp(mn, "brk") == 0 || strcmp(mn, "hvc") == 0 ||
        strcmp(mn, "smc") == 0 || strcmp(mn, "svc") == 0) {
        if (n != 1) { fail(a, "'%s' takes one immediate", mn); return; }
        long v = imm(a, ops[0]);
        if (v < 0 || v > 0xffff) { fail(a, "'%s' immediate out of range", mn); return; }
        if (!a->failed)
            a64_exception(a->out, mn[0] == 'b' ? 'b' : mn[0] == 'h' ? 'h'
                                : mn[1] == 'm' ? 'm' : 's', (int)v);
        return;
    }

    /* hlt (semihosting's trap: `hlt #0xf000`) and the debug-state
     * exceptions, whose immediate is optional */
    if (strcmp(mn, "hlt") == 0 || strcmp(mn, "dcps1") == 0 ||
        strcmp(mn, "dcps2") == 0 || strcmp(mn, "dcps3") == 0) {
        long v = 0;
        if (n > 1 || (n == 0 && mn[0] == 'h')) {
            fail(a, "'%s' takes one immediate", mn);
            return;
        }
        if (n == 1)
            v = imm(a, ops[0]);
        if (v < 0 || v > 0xffff) { fail(a, "'%s' immediate out of range", mn); return; }
        if (!a->failed)
            a64_word(a->out, (mn[0] == 'h' ? 0xD4400000UL
                              : 0xD4A00000UL | (unsigned long)(mn[4] - '0')) |
                             (unsigned long)v << 5);
        return;
    }

    if (strcmp(mn, ".inst") == 0) {
        if (n < 1) { fail(a, "'.inst' needs a value"); return; }
        for (int i = 0; i < n && !a->failed; i++) {
            long v = imm(a, ops[i]);
            if (v < 0 || (unsigned long)v > 0xffffffffUL) {
                fail(a, "'.inst' value does not fit in 32 bits");
                return;
            }
            a64_word(a->out, (unsigned long)v);
        }
        return;
    }

    if (a64_general(a, mn, rest))
        return;
    fail(a, "'%s' is not an A64 instruction this assembler knows", mn);
}

int a64asm_assemble(const char *text, struct code *out, char *err, int errlen)
{
    struct actx a;
    a.out = out;
    a.err = err;
    a.errlen = errlen;
    a.failed = 0;
    if (errlen > 0)
        err[0] = '\0';

    /* One statement at a time, up to a `;` or a newline, less a `//`
     * comment -- neither of which counts inside a .ascii string. */
    for (const char *p = text; *p; ) {
        int n = asm_stmt_len(p, ";");
        int len = asm_cut_comment(p, n, "", 1);
        char *st = malloc((size_t)len + 1);
        if (!st) {
            snprintf(err, (size_t)errlen, "out of memory");
            return -1;
        }
        memcpy(st, p, (size_t)len);
        st[len] = '\0';
        p += n;
        if (*p)
            p++;
        char *b = st, *e = st + len;
        while (isspace((unsigned char)*b))
            b++;
        while (e > b && isspace((unsigned char)e[-1]))
            *--e = '\0';
        if (*b) {
            /* .ascii/.asciz/.string, the data directives and the
             * alignments, whose padding the backend decides where the
             * bytes land; `.inst` is an instruction, below */
            static const struct asm_dirs dirs = { asm_data_a64, 0, 0 };
            int r = code_asm_directive(b, (int)(e - b), out, &dirs, err,
                                       errlen);
            if (r < 0)
                a.failed = 1;
            else if (!r) {
                a.stmt = b;
                assemble_stmt(&a, b);
            }
        }
        free(st);
        if (a.failed)
            break;
    }
    return a.failed ? -1 : 0;
}

void a64asm_vocabulary(FILE *f)
{
    for (unsigned i = 0; i < sizeof sysregs / sizeof sysregs[0]; i++)
        fprintf(f, "mrs x9, %s\n", sysregs[i].name);
    for (unsigned i = 0; i < sizeof tlbis / sizeof tlbis[0]; i++)
        fprintf(f, tlbis[i].reg ? "tlbi %s, x10\n" : "tlbi %s\n", tlbis[i].name);
    for (unsigned i = 0; i < sizeof cmaints / sizeof cmaints[0]; i++)
        fprintf(f, cmaints[i].reg ? "%s %s, x11\n" : "%s %s\n",
                cmaints[i].mn, cmaints[i].name);
    for (unsigned i = 0; i < sizeof barrier_opts / sizeof barrier_opts[0]; i++) {
        fprintf(f, "dsb %s\n", barrier_opts[i].name);
        fprintf(f, "dmb %s\n", barrier_opts[i].name);
    }
    for (unsigned i = 0; i < sizeof hints / sizeof hints[0]; i++)
        fprintf(f, "%s\n", hints[i].name);
}
