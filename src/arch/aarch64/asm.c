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

/* Is this operand a general register? Unlike gpr() below it does not
 * FAIL when it is not -- an operand that may be a register or an
 * immediate has to be asked without committing. */
static int maybe_gpr(const char *s, int *w)
{
    int r = a64asm_gpr(s, (int)strlen(s));
    if (r >= 0 && w)
        *w = s[0] == 'w';
    return r;
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
        if (*p == '[') depth++;
        else if (*p == ']') depth--;
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

/* [xN] or [xN, #imm] — the base register and byte offset. */
static void mem_operand(struct actx *a, char *s, int *base, long *off)
{
    size_t len = strlen(s);
    if (len < 3 || s[0] != '[' || s[len - 1] != ']') {
        fail(a, "expected a [register{, #offset}] address, got '%s'", s);
        return;
    }
    s[len - 1] = '\0';
    char *ops[2];
    int n = split_operands(s + 1, ops, 2);
    *off = 0;
    if (n >= 1) {
        if (strcmp(ops[0], "sp") == 0)
            *base = A64_SP;
        else
            *base = gpr(a, ops[0], NULL);
    }
    if (n == 2)
        *off = imm(a, ops[1]);
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

    if (strcmp(mn, "ldr") == 0 || strcmp(mn, "str") == 0) {
        if (n != 2) { fail(a, "'%s' takes a register and an address", mn); return; }
        int base = 0;
        long off = 0;
        mem_operand(a, ops[1], &base, &off);
        if (a->failed) return;
        int load = mn[0] == 'l';
        if (ops[0][0] == 'q') {
            char *end;
            long q = strtol(ops[0] + 1, &end, 10);
            if (end == ops[0] + 1 || *end || q < 0 || q > 31) {
                fail(a, "bad q register '%s'", ops[0]);
                return;
            }
            if (off < 0 || off % 16 || off / 16 > 0xfff) {
                fail(a, "q-register offset must be a multiple of 16 in 0..65520");
                return;
            }
            if (load) a64_ldr_q(a->out, (int)q, base, off);
            else      a64_str_q(a->out, (int)q, base, off);
            return;
        }
        int w = 0, rt = gpr(a, ops[0], &w);
        int size = w ? 4 : 8;
        if (a->failed) return;
        if (off < 0 || off % size || off / size > 0xfff) {
            fail(a, "offset must be a multiple of %d in the unsigned range", size);
            return;
        }
        if (load) a64_ldr(a->out, rt, base, off, size, 0, size);
        else      a64_str(a->out, rt, base, off, size);
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

    /* ---- what a .S file needs -------------------------------------
     *
     * The encoders for all of this already existed (src/arch/aarch64/
     * emit.h) and only the inline-asm vocabulary did not reach them:
     * that vocabulary was MEASURED from the EmbLinkOS kernel, whose
     * __asm__ statements are system instructions. A file assembler
     * needs the ordinary ones too, so they are exposed here rather
     * than re-encoded anywhere.
     *
     * Branch displacements arrive as `.+N`, resolved by the file
     * assembler before this layer is called; a bare number means the
     * same. Every branch is emitted then patched to `at + N`, which
     * is what the existing patch helpers take.
     *
     * gpr() reports "this is a W register" as a flag; the encoders
     * take a byte WIDTH. `w ? 4 : 8` is the conversion the ldr/str
     * path above already makes, and passing the flag straight through
     * emitted `mov w0` for `mov x0, 0`.
     */
    {
        static const struct { const char *name; int op; } alu[] = {
            { "add", '+' }, { "sub", '-' }, { "and", '&' },
            { "orr", '|' }, { "eor", '^' }
        };
        for (unsigned k = 0; k < sizeof alu / sizeof alu[0]; k++)
            if (strcmp(mn, alu[k].name) == 0 && n == 3) {
                int w1 = 0, w2 = 0, w3 = 0;
                int rd = gpr(a, ops[0], &w1);
                int rn = gpr(a, ops[1], &w2);
                int rm = maybe_gpr(ops[2], &w3);
                if (a->failed) return;
                if (rm < 0) {
                    long v = imm(a, ops[2]);
                    if (a->failed) return;
                    if (alu[k].op == '+')      a64_add_imm(a->out, rd, rn, v, w1 ? 4 : 8);
                    else if (alu[k].op == '-') a64_sub_imm(a->out, rd, rn, v, w1 ? 4 : 8);
                    else { fail(a, "'%s' takes a register third operand", mn);
                           return; }
                    return;
                }
                (void)w2; (void)w3;
                a64_alu_reg(a->out, alu[k].op, rd, rn, rm, w1 ? 4 : 8);
                return;
            }
    }
    if (strcmp(mn, "mov") == 0 && n == 2) {
        int w1 = 0, w2 = 0;
        int rd = gpr(a, ops[0], &w1);
        int rm = maybe_gpr(ops[1], &w2);
        if (a->failed) return;
        (void)w2;
        if (rm >= 0) a64_mov_reg(a->out, rd, rm, w1 ? 4 : 8);
        else {
            long v = imm(a, ops[1]);
            if (!a->failed) a64_mov_imm(a->out, rd, v, w1 ? 4 : 8);
        }
        return;
    }
    if (strcmp(mn, "cmp") == 0 && n == 2) {
        int w1 = 0, w2 = 0;
        int rn = gpr(a, ops[0], &w1);
        int rm = maybe_gpr(ops[1], &w2);
        if (a->failed) return;
        (void)w2;
        if (rm >= 0) a64_cmp_reg(a->out, rn, rm, w1 ? 4 : 8);
        else {
            /* `cmp rn, #k` is `subs xzr, rn, #k`, which the existing
             * sub-immediate encoder gives with xzr as its result. */
            long v = imm(a, ops[0 + 1]);
            if (!a->failed) a64_sub_imm(a->out, 31, rn, v, w1 ? 4 : 8);
        }
        return;
    }
    if (strcmp(mn, "ret") == 0 && n == 0) { a64_ret(a->out); return; }
    if ((strcmp(mn, "br") == 0 || strcmp(mn, "blr") == 0) && n == 1) {
        int w1 = 0;
        int rn = gpr(a, ops[0], &w1);
        (void)w1;
        if (a->failed) return;
        if (mn[1] == 'r') a64_br(a->out, rn);
        else              a64_blr(a->out, rn);
        return;
    }
    if ((strcmp(mn, "b") == 0 || strcmp(mn, "bl") == 0) && n == 1) {
        long v = imm(a, ops[0]);
        int at;
        if (a->failed) return;
        if (v & 3) { fail(a, "'%s' offset %ld is not a multiple of 4", mn, v);
                     return; }
        at = mn[1] ? a64_bl(a->out) : a64_b(a->out);
        a64_patch_b26(a->out, at, at + (int)v);
        return;
    }
    {
        static const struct { const char *name; int cond; } bc[] = {
            { "b.eq", 0 }, { "b.ne", 1 }, { "b.cs", 2 }, { "b.hs", 2 },
            { "b.cc", 3 }, { "b.lo", 3 }, { "b.mi", 4 }, { "b.pl", 5 },
            { "b.vs", 6 }, { "b.vc", 7 }, { "b.hi", 8 }, { "b.ls", 9 },
            { "b.ge", 10 }, { "b.lt", 11 }, { "b.gt", 12 }, { "b.le", 13 }
        };
        for (unsigned k = 0; k < sizeof bc / sizeof bc[0]; k++)
            if (strcmp(mn, bc[k].name) == 0 && n == 1) {
                long v = imm(a, ops[0]);
                int at;
                if (a->failed) return;
                if (v & 3) { fail(a, "'%s' offset %ld is not a multiple of 4",
                                  mn, v); return; }
                at = a64_bcond(a->out, bc[k].cond);
                a64_patch_b19(a->out, at, at + (int)v);
                return;
            }
    }
    if ((strcmp(mn, "cbz") == 0 || strcmp(mn, "cbnz") == 0) && n == 2) {
        int w1 = 0;
        int rt = gpr(a, ops[0], &w1);
        long v;
        int at;
        if (a->failed) return;
        v = imm(a, ops[1]);
        if (a->failed) return;
        if (v & 3) { fail(a, "'%s' offset %ld is not a multiple of 4", mn, v);
                     return; }
        at = a64_cbz(a->out, rt, mn[2] == 'n', w1 ? 4 : 8);
        a64_patch_b19(a->out, at, at + (int)v);
        return;
    }

    fail(a, "aarch64 inline asm: instruction '%s' is not supported", mn);
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

    size_t len = strlen(text);
    char *buf = malloc(len + 1);
    if (!buf) {
        snprintf(err, (size_t)errlen, "out of memory");
        return -1;
    }
    memcpy(buf, text, len + 1);

    /* Strip `//` comments to end of line before splitting statements. */
    for (char *p = buf; *p; p++)
        if (p[0] == '/' && p[1] == '/')
            while (*p && *p != '\n')
                *p++ = ' ';

    char *st = buf;
    for (char *p = buf;; p++) {
        if (*p == ';' || *p == '\n' || *p == '\0') {
            int end = *p == '\0';
            *p = '\0';
            while (isspace((unsigned char)*st))
                st++;
            char *e = st + strlen(st);
            while (e > st && isspace((unsigned char)e[-1]))
                *--e = '\0';
            if (*st) {
                a.stmt = st;
                assemble_stmt(&a, st);
                if (a.failed)
                    break;
            }
            if (end)
                break;
            st = p + 1;
        }
    }
    free(buf);
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
