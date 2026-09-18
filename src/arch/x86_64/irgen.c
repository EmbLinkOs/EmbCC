/* x86-64's share of IR generation: the SysV va_arg walk and the extended-asm
 * template assembler — the lowering that depends on the target's ABI and its
 * instruction set. Built from irgen's helpers (src/ir/irgen_int.h). */
#include "../../ir/irgen_int.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../driver/util.h"
#include "../../sema/sema.h"
#include "../../sema/type.h"

/* va_arg(ap, T) for an INTEGER-class T (SysV). ap's value is a pointer to
 * a __va_list_tag { gp_offset u32, fp_offset u32, overflow_arg_area ptr,
 * reg_save_area ptr }. If gp_offset < 48 the argument sits in the register
 * save area at reg_save_area + gp_offset and gp_offset advances by 8;
 * otherwise it is next in the overflow area, which advances by 8. */
int irg_va_arg_sysv(struct ir_func *fn, struct expr *e)
{
    struct type *rt = e->ty;
    struct type *u32 = ty_base(TY_INT, 1);
    struct type *ptr = ty_base(TY_LONG, 1); /* an 8-byte slot */
    int ap = gen_expr(fn, e->lhs);          /* pointer to the tag */

    int a_ova = emit_bin(fn, IR_ADD, ap, emit_const(fn, 8, 8), 8, 1);
    int a_rsa = emit_bin(fn, IR_ADD, ap, emit_const(fn, 16, 8), 8, 1);

    /* long double is X87 class, which SysV passes in memory — always the
     * overflow area, at a 16-aligned slot of 16 bytes. */
    if (rt->kind == TY_LDOUBLE) {
        int ova = emit_load(fn, a_ova, ptr);
        int al = emit_bin(fn, IR_AND,
                          emit_bin(fn, IR_ADD, ova, emit_const(fn, 15, 8), 8, 1),
                          emit_const(fn, -16, 8), 8, 1);
        emit_store(fn, a_ova,
                   emit_bin(fn, IR_ADD, al, emit_const(fn, 16, 8), 8, 1), ptr);
        return emit_load(fn, al, rt);
    }

    /* SSE class (float/double): the SysV register save area lays the eight xmm
     * regs AFTER the six GP regs, so fp_offset (at ap+4) runs 48..176 in strides
     * of 16 (each xmm slot is 16 bytes, of which we read the low 8 = a double).
     * A variadic float arg is promoted to double, so the overflow slot is 8. */
    if (ty_is_float(rt)) {
        struct type *dbl = ty_base(TY_DOUBLE, 0);
        int a_fp = emit_bin(fn, IR_ADD, ap, emit_const(fn, 4, 8), 8, 1);
        int fp = emit_load(fn, a_fp, u32);      /* fp_offset (at ap+4) */
        int in_reg = emit_cmp(fn, B_LT, fp, emit_const(fn, 176, 4), 4, 0);
        int addr = new_temp(fn);
        int l_over = new_label(fn), l_done = new_label(fn);
        emit_brz(fn, in_reg, 4, l_over);        /* fp_offset >= 176 -> overflow */
        /* register save area: addr = reg_save_area + fp_offset; fp_offset += 16 */
        int rsa = emit_load(fn, a_rsa, ptr);
        emit_mov(fn, addr, emit_bin(fn, IR_ADD, rsa, fp, 8, 1));
        emit_store(fn, a_fp,
                   emit_bin(fn, IR_ADD, fp, emit_const(fn, 16, 4), 4, 0), u32);
        emit_jmp(fn, l_done);
        /* overflow area: addr = overflow_arg_area; advance it by 8 */
        emit_label(fn, l_over);
        int ova = emit_load(fn, a_ova, ptr);
        emit_mov(fn, addr, ova);
        emit_store(fn, a_ova,
                   emit_bin(fn, IR_ADD, ova, emit_const(fn, 8, 8), 8, 1), ptr);
        emit_label(fn, l_done);
        int v = emit_load(fn, addr, dbl);       /* the value is a promoted double */
        if (rt->kind == TY_FLOAT) {             /* va_arg(ap,float): narrow it */
            struct ir_ins *cv = emit(fn);
            cv->op = IR_F2F; cv->a = v; cv->size = 8; cv->w = 4;
            cv->dst = new_temp(fn);
            return cv->dst;
        }
        return v;
    }

    int gp = emit_load(fn, ap, u32);        /* gp_offset (at ap+0) */
    int in_reg = emit_cmp(fn, B_LT, gp, emit_const(fn, 48, 4), 4, 0);

    int addr = new_temp(fn);
    int l_over = new_label(fn), l_done = new_label(fn);
    emit_brz(fn, in_reg, 4, l_over);        /* gp_offset >= 48 -> overflow */

    /* register save area: addr = reg_save_area + gp_offset; gp_offset += 8 */
    int rsa = emit_load(fn, a_rsa, ptr);
    emit_mov(fn, addr, emit_bin(fn, IR_ADD, rsa, gp, 8, 1));
    emit_store(fn, ap, emit_bin(fn, IR_ADD, gp, emit_const(fn, 8, 4), 4, 0),
               u32);
    emit_jmp(fn, l_done);

    /* overflow area: addr = overflow_arg_area; advance it by 8 */
    emit_label(fn, l_over);
    int ova = emit_load(fn, a_ova, ptr);
    emit_mov(fn, addr, ova);
    emit_store(fn, a_ova, emit_bin(fn, IR_ADD, ova, emit_const(fn, 8, 8),
                                   8, 1), ptr);

    emit_label(fn, l_done);
    return emit_load(fn, addr, rt);
}


/* ---- small parse helpers for the inline-asm template ---- */

static void a_ws(const char **p)
{
    while (**p == ' ' || **p == '\t')
        (*p)++;
}

/* Read an operand reference — numbered `%N` or symbolic `%[name]` — and
 * return its register (opregs[the operand's index]). */
static int a_opreg(const char **p, const int *opregs,
                   const char *const *opnames, int nops,
                   const char *file, int line, const char *tmpl)
{
    a_ws(p);
    if (**p != '%')
        diag_fatal(file, line, "asm: expected a %%N operand in \"%s\"", tmpl);
    (*p)++;
    if (**p == '[') {                       /* %[name] */
        (*p)++;
        const char *nm = *p;
        while (**p && **p != ']')
            (*p)++;
        int len = (int)(*p - nm);
        if (**p != ']')
            diag_fatal(file, line, "asm: unterminated %%[name] in \"%s\"",
                       tmpl);
        (*p)++;
        for (int i = 0; i < nops; i++)
            if (opnames[i] && (int)strlen(opnames[i]) == len &&
                strncmp(opnames[i], nm, (size_t)len) == 0)
                return opregs[i];
        diag_fatal(file, line, "asm: unknown operand %%[%.*s] in \"%s\"",
                   len, nm, tmpl);
    }
    if (!(**p >= '0' && **p <= '9'))
        diag_fatal(file, line, "asm: expected a %%N operand in \"%s\"", tmpl);
    int idx = 0;
    while (**p >= '0' && **p <= '9')
        idx = idx * 10 + (*(*p)++ - '0');
    if (idx >= nops)
        diag_fatal(file, line, "asm operand %%%d out of range in \"%s\"",
                   idx, tmpl);
    return opregs[idx];
}

/* Read a control register `%%crN`, returning N. */
static int a_creg(const char **p, const char *file, int line, const char *tmpl)
{
    a_ws(p);
    if ((*p)[0] != '%' || (*p)[1] != '%' || (*p)[2] != 'c' || (*p)[3] != 'r')
        diag_fatal(file, line, "asm: expected %%%%crN in \"%s\"", tmpl);
    *p += 4;
    int n = 0;
    while (**p >= '0' && **p <= '9')
        n = n * 10 + (*(*p)++ - '0');
    return n;
}

static void a_comma(const char **p, const char *file, int line,
                    const char *tmpl)
{
    a_ws(p);
    if (**p != ',')
        diag_fatal(file, line, "asm: expected ',' in \"%s\"", tmpl);
    (*p)++;
}

/* Read a hard 64-bit GPR name `%%rax` .. `%%r15`, returning 0..15. */
static int a_regname(const char **p, const char *file, int line,
                     const char *tmpl)
{
    static const struct { const char *n; int r; } regs[] = {
        { "rax", 0 }, { "rcx", 1 }, { "rdx", 2 }, { "rbx", 3 },
        { "rsp", 4 }, { "rbp", 5 }, { "rsi", 6 }, { "rdi", 7 },
        { "r8", 8 }, { "r9", 9 }, { "r10", 10 }, { "r11", 11 },
        { "r12", 12 }, { "r13", 13 }, { "r14", 14 }, { "r15", 15 },
    };
    a_ws(p);
    if ((*p)[0] != '%' || (*p)[1] != '%')
        diag_fatal(file, line, "asm: expected a %%%%register in \"%s\"", tmpl);
    const char *q = *p + 2;
    /* longest match first (r15 before r1), and a name boundary after it */
    int best = -1; size_t bestlen = 0;
    for (unsigned i = 0; i < sizeof regs / sizeof regs[0]; i++) {
        size_t l = strlen(regs[i].n);
        char after = q[l];
        if (strncmp(q, regs[i].n, l) == 0 && l > bestlen &&
            !((after >= 'a' && after <= 'z') || (after >= '0' && after <= '9')))
            { best = regs[i].r; bestlen = l; }
    }
    if (best < 0)
        diag_fatal(file, line, "asm: unsupported register in \"%s\"", tmpl);
    *p = q + bestlen;
    return best;
}

static int asm_phys_reg(const char *nm, int len);   /* fwd: any-width mapper */

/* Read a `%%reg` of ANY width (rax/eax/ax/al, r8/r8d/r8w/r8b, …), returning
 * its physical number 0..15. Unlike a_regname (64-bit spellings only) this is
 * for instructions whose operand size comes from a suffix, not the reg name
 * (the ALU ops' 'l'/'q' forms). */
static int a_reg_any(const char **p, const char *file, int line,
                     const char *tmpl)
{
    a_ws(p);
    if ((*p)[0] != '%' || (*p)[1] != '%')
        diag_fatal(file, line, "asm: expected a %%%%register in \"%s\"", tmpl);
    const char *q = *p + 2;
    const char *e = q;
    while ((*e >= 'a' && *e <= 'z') || (*e >= '0' && *e <= '9')) e++;
    int r = asm_phys_reg(q, (int)(e - q));
    if (r < 0)
        diag_fatal(file, line, "asm: unsupported register in \"%s\"", tmpl);
    *p = e;
    return r;
}

/* Read an immediate `$N` (decimal or 0x hex), returning its value. */
static long a_imm(const char **p, const char *file, int line, const char *tmpl)
{
    a_ws(p);
    if (**p != '$')
        diag_fatal(file, line, "asm: expected an $immediate in \"%s\"", tmpl);
    (*p)++;
    int neg = 0;
    if (**p == '-') { neg = 1; (*p)++; }
    long v = 0;
    int base = 10;
    if ((*p)[0] == '0' && ((*p)[1] == 'x' || (*p)[1] == 'X')) {
        base = 16; *p += 2;
    }
    for (;; (*p)++) {
        int d;
        if (**p >= '0' && **p <= '9') d = **p - '0';
        else if (base == 16 && **p >= 'a' && **p <= 'f') d = **p - 'a' + 10;
        else if (base == 16 && **p >= 'A' && **p <= 'F') d = **p - 'A' + 10;
        else break;
        v = v * base + d;
    }
    return neg ? -v : v;
}

/* Read a memory operand `(%N)`, returning the base register (opregs[N]). */
static int a_memreg(const char **p, const int *opregs,
                    const char *const *opnames, int nops,
                    const char *file, int line, const char *tmpl)
{
    a_ws(p);
    if (**p != '(')
        diag_fatal(file, line, "asm: expected a `(%%N)` memory operand in "
                   "\"%s\"", tmpl);
    (*p)++;
    int r = a_opreg(p, opregs, opnames, nops, file, line, tmpl);
    a_ws(p);
    if (**p != ')')
        diag_fatal(file, line, "asm: unterminated `(%%N)` in \"%s\"", tmpl);
    (*p)++;
    if ((r & 7) == 4 || (r & 7) == 5)   /* rsp/rbp base needs SIB/disp */
        diag_fatal(file, line, "asm: memory base %%rsp/%%rbp unsupported "
                   "in \"%s\"", tmpl);
    return r;
}

/* Is the operand at *p a memory reference (`disp(%base)` / `(%base)`) rather
 * than a register/immediate? True iff a '(' appears before the operand ends
 * — a %N, %%reg, or $imm never contains one. */
static int a_is_mem(const char *p)
{
    while (*p == ' ' || *p == '\t') p++;
    for (; *p && *p != ',' && *p != '\n' && *p != '\r' && *p != ';'; p++)
        if (*p == '(') return 1;
    return 0;
}

/* Parse `disp(%base)` (disp optional, decimal or 0x-hex, optional sign); base
 * is %N or %%regname. Returns the base register, displacement via *disp_out. */
static int a_mem(const char **p, const int *opregs, const char *const *opnames,
                 int nops, const char *file, int line, const char *tmpl,
                 long *disp_out)
{
    a_ws(p);
    long disp = 0;
    if (**p != '(') {
        int neg = 0;
        if (**p == '-') { neg = 1; (*p)++; }
        else if (**p == '+') (*p)++;
        int base = 10;
        if ((*p)[0] == '0' && ((*p)[1] == 'x' || (*p)[1] == 'X')) {
            base = 16; *p += 2;
        }
        int got = 0;
        for (;; (*p)++) {
            int d;
            if (**p >= '0' && **p <= '9') d = **p - '0';
            else if (base == 16 && **p >= 'a' && **p <= 'f') d = **p - 'a' + 10;
            else if (base == 16 && **p >= 'A' && **p <= 'F') d = **p - 'A' + 10;
            else break;
            disp = disp * base + d; got = 1;
        }
        if (!got)
            diag_fatal(file, line, "asm: expected a displacement or `(` in "
                       "\"%s\"", tmpl);
        if (neg) disp = -disp;
        a_ws(p);
    }
    if (**p != '(')
        diag_fatal(file, line, "asm: expected `(%%base)` in \"%s\"", tmpl);
    (*p)++;
    a_ws(p);
    int r = (**p == '%' && (*p)[1] == '%')
          ? a_regname(p, file, line, tmpl)
          : a_opreg(p, opregs, opnames, nops, file, line, tmpl);
    a_ws(p);
    if (**p != ')')
        diag_fatal(file, line, "asm: unterminated `(%%base)` in \"%s\"", tmpl);
    (*p)++;
    *disp_out = disp;
    return r;
}

/* Emit ModRM (+SIB +disp) for a memory operand [base+disp] with `reg_field`
 * (0..15; only its low 3 bits go in ModRM, the caller puts bit 3 in REX.R).
 * Handles rsp/r12 (needs a SIB) and rbp/r13 (has no disp-less form). Returns
 * the new code length. REX is emitted by the caller. */
static int emit_mem_modrm(unsigned char *code, int n, int reg_field,
                          int base, long disp)
{
    int b = base & 7;
    int need_sib   = (b == 4);   /* rsp/r12: escape to SIB */
    int force_disp = (b == 5);   /* rbp/r13: no mod=00 form, use disp8=0 */
    int mod;
    if (disp == 0 && !force_disp)          mod = 0;
    else if (disp >= -128 && disp <= 127)  mod = 1;
    else                                   mod = 2;
    code[n++] = (unsigned char)((mod << 6) | ((reg_field & 7) << 3) |
                                (need_sib ? 4 : b));
    if (need_sib)   /* scale=0, index=none(4), base=b */
        code[n++] = (unsigned char)((0 << 6) | (4 << 3) | b);
    if (mod == 1)
        code[n++] = (unsigned char)disp;
    else if (mod == 2)
        for (int i = 0; i < 4; i++)
            code[n++] = (unsigned char)(disp >> (8 * i));
    return n;
}

/* Match an ALU mnemonic (add/sub/and/or/xor/cmp, optional 'q'/'l' suffix).
 * On a hit fills *rr (the reg,reg opcode), *ext (the /digit for the $imm form)
 * and *w (1 = 64-bit REX.W, 0 = 32-bit) and returns 1; else returns 0. */
static int asm_alu_lookup(const char *m, int mlen, int *rr, int *ext, int *w)
{
    static const struct { const char *n; int rr, ext; } alu[] = {
        {"add",0x01,0}, {"or",0x09,1}, {"and",0x21,4},
        {"sub",0x29,5}, {"xor",0x31,6}, {"cmp",0x39,7},
    };
    for (unsigned i = 0; i < sizeof alu / sizeof alu[0]; i++) {
        int ln = (int)strlen(alu[i].n);
        int width = -1;
        if (mlen == ln && strncmp(m, alu[i].n, (size_t)ln) == 0)
            width = 1;
        else if (mlen == ln + 1 && strncmp(m, alu[i].n, (size_t)ln) == 0) {
            if (m[ln] == 'q') width = 1;
            else if (m[ln] == 'l') width = 0;
        }
        if (width < 0) continue;
        *rr = alu[i].rr; *ext = alu[i].ext; *w = width;
        return 1;
    }
    return 0;
}

/* Assemble an extended-asm template into machine bytes, now that every
 * operand has a register (opregs[N] is the register of %N — outputs first,
 * then inputs, as gcc numbers them). EmbCC has no general text assembler,
 * only the vocabulary real low-level C needs — the syscall trap plus the
 * kernel's hardware instructions (port I/O, control/segment/MSR access,
 * fences, TLB, descriptor tables). Anything else is refused loudly. */
static void asm_assemble(struct ir_func *fn, struct stmt *s,
                         const int *opregs, const char *const *opnames,
                         int nops, struct ir_asm *ia)
{
    const char *file = fn->src->file;
    int line = s->line;
    const char *tmpl = s->asm_s->tmpl;
    unsigned char *code = NULL;
    int n = 0, cap = 0;
    const char *p = tmpl;
    /* local labels `N:` and the RIP-relative `leaq Nf(%rip)` sites that
     * reference them — resolved within this block after assembling it. */
    struct { int num, off; } labels[16]; int nlab = 0;
    struct { int num, patch; } fixups[16]; int nfix = 0;

    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' ||
               *p == ';')
            p++;
        if (!*p)
            break;
        const char *m = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' &&
               *p != ';')
            p++;
        int mlen = (int)(p - m);
        while (*p == ' ' || *p == '\t')
            p++;

        /* one operand reg for the instructions that take a %N */
        int reg = -1;
        int alu_rr = 0, alu_ext = 0, alu_w = 0;  /* filled by asm_alu_lookup */
        if (*p == '%' && p[1] >= '0' && p[1] <= '9') {
            p++;
            int idx = 0;
            while (*p >= '0' && *p <= '9')
                idx = idx * 10 + (*p++ - '0');
            if (idx >= nops)
                diag_fatal(file, line,
                           "asm operand %%%d out of range in \"%s\"",
                           idx, tmpl);
            reg = opregs[idx];
        }

        if (n + 16 > cap) {
            cap = cap ? cap * 2 : 16;
            code = xrealloc(code, (size_t)cap);
        }
        /* a local label `N:` — record its offset for a leaq to reference */
        if (mlen >= 2 && m[mlen - 1] == ':' &&
            m[0] >= '0' && m[0] <= '9') {
            int num = 0;
            for (int i = 0; i < mlen - 1; i++)
                num = num * 10 + (m[i] - '0');
            if (nlab < 16) { labels[nlab].num = num; labels[nlab].off = n;
                             nlab++; }
            continue;
        }
        if (mlen == 3 && strncmp(m, "int", 3) == 0) {
            if (*p != '$')
                diag_fatal(file, line, "asm 'int' wants $vector: \"%s\"",
                           tmpl);
            p++;
            long imm = 0;
            int base = 10;
            if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
                base = 16;
                p += 2;
            }
            for (; ; p++) {
                int d;
                if (*p >= '0' && *p <= '9') d = *p - '0';
                else if (base == 16 && *p >= 'a' && *p <= 'f') d = *p - 'a' + 10;
                else if (base == 16 && *p >= 'A' && *p <= 'F') d = *p - 'A' + 10;
                else break;
                imm = imm * base + d;
            }
            if (imm < 0 || imm > 255)
                diag_fatal(file, line, "asm 'int' vector %ld out of range",
                           imm);
            code[n++] = 0xcd;
            code[n++] = (unsigned char)imm;
        } else if (mlen == 5 && strncmp(m, "cpuid", 5) == 0) {
            code[n++] = 0x0f;
            code[n++] = 0xa2;
        } else if (mlen == 6 && (strncmp(m, "rdrand", 6) == 0 ||
                                 strncmp(m, "rdseed", 6) == 0)) {
            /* rdrand is 0F C7 /6, rdseed the same opcode with /7 */
            if (reg < 0)
                diag_fatal(file, line, "asm '%.6s' wants %%N: \"%s\"", m, tmpl);
            code[n++] = (unsigned char)(0x48 | (reg >= 8 ? 1 : 0)); /* REX.W(.B) */
            code[n++] = 0x0f;
            code[n++] = 0xc7;
            code[n++] = (unsigned char)((m[2] == 's' ? 0xf8 : 0xf0) | (reg & 7));
        } else if (mlen == 4 && strncmp(m, "setc", 4) == 0) {
            if (reg < 0)
                diag_fatal(file, line, "asm 'setc' wants %%N: \"%s\"", tmpl);
            if (reg >= 4)  /* REX to name spl/bpl/sil/dil or r8b.. as a byte */
                code[n++] = (unsigned char)(0x40 | (reg >= 8 ? 1 : 0));
            code[n++] = 0x0f;
            code[n++] = 0x92;
            code[n++] = (unsigned char)(0xc0 | (reg & 7));          /* /0 */
        } else if (mlen == 6 && strncmp(m, "sqrts", 5) == 0 &&
                   (m[5] == 'd' || m[5] == 's')) {
            /* sqrtsd/sqrtss %src,%dst (AT&T order): F2/F3 0F 51 /r, both xmm.
             * `reg` already holds the FIRST operand (%src); parse `,%dst`. */
            int is_sd = m[5] == 'd';
            if (reg < 16)
                diag_fatal(file, line,
                           "asm '%.*s' operands must be 'x' (xmm): \"%s\"",
                           mlen, m, tmpl);
            int src = reg;
            if (*p != ',')
                diag_fatal(file, line,
                           "asm '%.*s' wants %%src,%%dst: \"%s\"", mlen, m, tmpl);
            p++;
            while (*p == ' ' || *p == '\t') p++;
            if (*p != '%' || !(p[1] >= '0' && p[1] <= '9'))
                diag_fatal(file, line,
                           "asm '%.*s' destination must be %%N: \"%s\"",
                           mlen, m, tmpl);
            p++;
            int idx2 = 0;
            while (*p >= '0' && *p <= '9')
                idx2 = idx2 * 10 + (*p++ - '0');
            if (idx2 >= nops)
                diag_fatal(file, line,
                           "asm operand %%%d out of range in \"%s\"", idx2, tmpl);
            int dst = opregs[idx2];
            if (dst < 16)
                diag_fatal(file, line,
                           "asm '%.*s' destination must be 'x' (xmm): \"%s\"",
                           mlen, m, tmpl);
            int s = src - 16, d = dst - 16;
            code[n++] = (unsigned char)(is_sd ? 0xf2 : 0xf3);
            if (d >= 8 || s >= 8)     /* REX.R names dst>=8, REX.B names src>=8 */
                code[n++] = (unsigned char)(0x40 | (d >= 8 ? 4 : 0) |
                                            (s >= 8 ? 1 : 0));
            code[n++] = 0x0f;
            code[n++] = 0x51;
            code[n++] = (unsigned char)(0xc0 | ((d & 7) << 3) | (s & 7));
        }
        /* ---- fixed-form instructions (no encoded operands) ---- */
        else if (mlen == 3 && strncmp(m, "cli", 3) == 0) { code[n++] = 0xfa; }
        /* SMAP: stac / clac set and clear EFLAGS.AC around user accesses */
        else if (mlen == 4 && strncmp(m, "stac", 4) == 0) {
            code[n++] = 0x0f; code[n++] = 0x01; code[n++] = 0xcb;
        } else if (mlen == 4 && strncmp(m, "clac", 4) == 0) {
            code[n++] = 0x0f; code[n++] = 0x01; code[n++] = 0xca;
        }
        else if (mlen == 3 && strncmp(m, "sti", 3) == 0) { code[n++] = 0xfb; }
        else if (mlen == 3 && strncmp(m, "hlt", 3) == 0) { code[n++] = 0xf4; }
        else if (mlen == 3 && strncmp(m, "nop", 3) == 0) { code[n++] = 0x90; }
        else if (mlen == 5 && strncmp(m, "pause", 5) == 0) {
            code[n++] = 0xf3; code[n++] = 0x90;
        } else if (mlen == 6 && strncmp(m, "mfence", 6) == 0) {
            code[n++] = 0x0f; code[n++] = 0xae; code[n++] = 0xf0;
        } else if (mlen == 6 && strncmp(m, "lfence", 6) == 0) {
            code[n++] = 0x0f; code[n++] = 0xae; code[n++] = 0xe8;
        } else if (mlen == 6 && strncmp(m, "sfence", 6) == 0) {
            code[n++] = 0x0f; code[n++] = 0xae; code[n++] = 0xf8;
        } else if (mlen == 6 && strncmp(m, "wbinvd", 6) == 0) {
            code[n++] = 0x0f; code[n++] = 0x09;
        } else if (mlen == 5 && strncmp(m, "rdtsc", 5) == 0) {
            code[n++] = 0x0f; code[n++] = 0x31;
        } else if (mlen == 5 && strncmp(m, "rdmsr", 5) == 0) {
            code[n++] = 0x0f; code[n++] = 0x32;
        } else if (mlen == 5 && strncmp(m, "wrmsr", 5) == 0) {
            code[n++] = 0x0f; code[n++] = 0x30;
        } else if (mlen == 6 && strncmp(m, "fninit", 6) == 0) {
            code[n++] = 0xdb; code[n++] = 0xe3;
        } else if (mlen == 6 && strncmp(m, "pushfq", 6) == 0) {
            code[n++] = 0x9c;
        } else if (mlen == 5 && strncmp(m, "popfq", 5) == 0) {
            code[n++] = 0x9d;
        }
        /* ---- port I/O: al/ax/eax with dx, both operands fixed by the
         * constraints ("a" and "Nd"), so the opcode alone encodes it ---- */
        else if (mlen == 4 && strncmp(m, "outb", 4) == 0) { code[n++] = 0xee; }
        else if (mlen == 4 && strncmp(m, "outw", 4) == 0) {
            code[n++] = 0x66; code[n++] = 0xef;
        } else if (mlen == 4 && strncmp(m, "outl", 4) == 0) { code[n++] = 0xef; }
        else if (mlen == 3 && strncmp(m, "inb", 3) == 0) { code[n++] = 0xec; }
        else if (mlen == 3 && strncmp(m, "inw", 3) == 0) {
            code[n++] = 0x66; code[n++] = 0xed;
        } else if (mlen == 3 && strncmp(m, "inl", 3) == 0) { code[n++] = 0xed; }
        /* ---- pop/push %N (64-bit; the `q` suffix is the same encoding) ---- */
        else if ((mlen == 3 && strncmp(m, "pop", 3) == 0) ||
                 (mlen == 4 && strncmp(m, "popq", 4) == 0)) {
            if (reg < 0) {
                a_ws(&p);
                reg = (p[0] == '%' && p[1] == '%')
                    ? a_regname(&p, file, line, tmpl)
                    : a_opreg(&p, opregs, opnames, nops, file, line, tmpl);
            }
            if (reg >= 8) code[n++] = 0x41;                /* REX.B */
            code[n++] = (unsigned char)(0x58 | (reg & 7));
        } else if ((mlen == 4 && strncmp(m, "push", 4) == 0) ||
                   (mlen == 5 && strncmp(m, "pushq", 5) == 0)) {
            a_ws(&p);
            if (reg < 0 && p[0] == '$') {          /* push imm32 */
                long imm = a_imm(&p, file, line, tmpl);
                code[n++] = 0x68;
                for (int b = 0; b < 4; b++)
                    code[n++] = (unsigned char)(imm >> (8 * b));
            } else {
                if (reg < 0)
                    reg = (p[0] == '%' && p[1] == '%')
                        ? a_regname(&p, file, line, tmpl)
                        : a_opreg(&p, opregs, opnames, nops, file, line, tmpl);
                if (reg >= 8) code[n++] = 0x41;
                code[n++] = (unsigned char)(0x50 | (reg & 7));
            }
        }
        else if (mlen == 5 && strncmp(m, "iretq", 5) == 0) {
            code[n++] = 0x48; code[n++] = 0xcf;
        } else if (mlen == 5 && strncmp(m, "lretq", 5) == 0) {
            code[n++] = 0x48; code[n++] = 0xcb;
        }
        /* ---- leaq Nf(%%rip), %%reg : RIP-relative address of a local label ---- */
        else if (mlen == 4 && strncmp(m, "leaq", 4) == 0) {
            a_ws(&p);
            if (!(*p >= '0' && *p <= '9'))
                diag_fatal(file, line, "asm leaq expects a local label in "
                           "\"%s\"", tmpl);
            int num = 0;
            while (*p >= '0' && *p <= '9') num = num * 10 + (*p++ - '0');
            if (*p == 'f' || *p == 'b') p++;      /* forward/backward marker */
            a_ws(&p);
            if (strncmp(p, "(%%rip)", 7) != 0)
                diag_fatal(file, line, "asm leaq expects `Nf(%%%%rip)` in "
                           "\"%s\"", tmpl);
            p += 7;
            a_comma(&p, file, line, tmpl);
            int dst = a_regname(&p, file, line, tmpl);
            code[n++] = (unsigned char)(0x48 | (dst >= 8 ? 4 : 0));  /* REX.W[R] */
            code[n++] = 0x8d;
            code[n++] = (unsigned char)(0x05 | ((dst & 7) << 3));    /* rip+disp32 */
            if (nfix < 16) { fixups[nfix].num = num; fixups[nfix].patch = n;
                             nfix++; }
            for (int b = 0; b < 4; b++) code[n++] = 0;   /* disp32 (patched) */
        }
        /* ---- str/ltr %N (task register; r/m16) ---- */
        else if (mlen == 3 && strncmp(m, "str", 3) == 0) {
            if (reg < 0) reg = a_opreg(&p, opregs, opnames, nops, file, line, tmpl);
            if (reg >= 8) code[n++] = 0x41;
            code[n++] = 0x0f; code[n++] = 0x00;
            code[n++] = (unsigned char)(0xc8 | (reg & 7));   /* /1 */
        } else if (mlen == 3 && strncmp(m, "ltr", 3) == 0) {
            if (reg < 0) reg = a_opreg(&p, opregs, opnames, nops, file, line, tmpl);
            if (reg >= 8) code[n++] = 0x41;
            code[n++] = 0x0f; code[n++] = 0x00;
            code[n++] = (unsigned char)(0xd8 | (reg & 7));   /* /3 */
        }
        /* ---- mov / movq: control registers, and reg/imm -> a GPR ---- */
        else if ((mlen == 3 && strncmp(m, "mov", 3) == 0) ||
                 (mlen == 4 && strncmp(m, "movq", 4) == 0) ||
                 (mlen == 6 && strncmp(m, "movabs", 6) == 0)) {
            /* segment-register / 16-bit accumulator forms the gdt trampoline
             * uses: `mov %%ax, %%<seg>` and `mov $imm, %%ax`. */
            static const char *const segs[] =
                { "es", "cs", "ss", "ds", "fs", "gs" };
            a_ws(&p);
            if (reg < 0 && strncmp(p, "%%ax", 4) == 0) {
                const char *q = p + 4;
                a_ws(&q);
                if (*q == ',') {
                    q++; a_ws(&q);
                    if (q[0] == '%' && q[1] == '%') {
                        for (int si = 0; si < 6; si++)
                            if (strncmp(q + 2, segs[si], 2) == 0) {
                                code[n++] = 0x8e;   /* mov Sreg, r/m16 */
                                code[n++] = (unsigned char)(0xc0 | (si << 3));
                                p = q + 4;
                                goto asm_next;
                            }
                    }
                }
            }
            if (reg < 0 && p[0] == '$') {
                const char *q = p;
                long imm = a_imm(&q, file, line, tmpl);
                a_ws(&q);
                if (*q == ',') {
                    q++;
                    a_ws(&q);
                    if (strncmp(q, "%%ax", 4) == 0) {   /* mov $imm16, %%ax */
                        code[n++] = 0x66; code[n++] = 0xb8;
                        code[n++] = (unsigned char)imm;
                        code[n++] = (unsigned char)(imm >> 8);
                        p = q + 4;
                        goto asm_next;
                    }
                }
            }
            int src_reg = -1, src_imm_valid = 0;
            long src_imm = 0;
            /* --- source operand --- */
            if (reg >= 0) {
                src_reg = reg;                        /* pre-parsed %N */
            } else {
                a_ws(&p);
                if (a_is_mem(p)) {                    /* mov disp(%base), %dst */
                    long disp;
                    int base = a_mem(&p, opregs, opnames, nops, file, line,
                                     tmpl, &disp);
                    a_comma(&p, file, line, tmpl);
                    a_ws(&p);
                    int dst = (p[0] == '%' && p[1] == '%')
                            ? a_regname(&p, file, line, tmpl)
                            : a_opreg(&p, opregs, opnames, nops, file, line, tmpl);
                    code[n++] = (unsigned char)(0x48 | (dst >= 8 ? 4 : 0) |
                                                (base >= 8 ? 1 : 0));
                    code[n++] = 0x8b;                 /* mov r64, r/m64 (load) */
                    n = emit_mem_modrm(code, n, dst, base, disp);
                    goto asm_next;
                }
                if (p[0] == '$') {
                    src_imm = a_imm(&p, file, line, tmpl);
                    src_imm_valid = 1;
                } else if (p[0] == '%' && p[1] == '%' && p[2] == 'c' &&
                           p[3] == 'r') {             /* mov %%crN, %reg */
                    int cr = a_creg(&p, file, line, tmpl);
                    a_comma(&p, file, line, tmpl);
                    a_ws(&p);
                    int gpr = (p[0] == '%' && p[1] == '%')
                            ? a_regname(&p, file, line, tmpl)
                            : a_opreg(&p, opregs, opnames, nops, file, line, tmpl);
                    int rex = 0x40 | (gpr >= 8) | (cr >= 8 ? 4 : 0);
                    if (rex != 0x40) code[n++] = (unsigned char)rex;
                    code[n++] = 0x0f; code[n++] = 0x20;
                    code[n++] = (unsigned char)(0xc0 | ((cr & 7) << 3) |
                                                (gpr & 7));
                    goto asm_next;
                } else if (p[0] == '%' && p[1] == '%') {
                    src_reg = a_regname(&p, file, line, tmpl);
                } else {
                    src_reg = a_opreg(&p, opregs, opnames, nops, file, line, tmpl);
                }
            }
            a_comma(&p, file, line, tmpl);
            a_ws(&p);
            /* --- destination: memory, a control register, or a GPR --- */
            if (a_is_mem(p)) {                        /* mov %src, disp(%base) */
                if (src_imm_valid)
                    diag_fatal(file, line, "asm: mov $imm to memory unsupported "
                               "in \"%s\"", tmpl);
                long disp;
                int base = a_mem(&p, opregs, opnames, nops, file, line, tmpl,
                                 &disp);
                code[n++] = (unsigned char)(0x48 | (src_reg >= 8 ? 4 : 0) |
                                            (base >= 8 ? 1 : 0));
                code[n++] = 0x89;                     /* mov r/m64, r64 (store) */
                n = emit_mem_modrm(code, n, src_reg, base, disp);
            } else if (p[0] == '%' && p[1] == '%' && p[2] == 'c' && p[3] == 'r') {
                int cr = a_creg(&p, file, line, tmpl);      /* mov %reg,%%crN */
                int rex = 0x40 | (src_reg >= 8) | (cr >= 8 ? 4 : 0);
                if (rex != 0x40) code[n++] = (unsigned char)rex;
                code[n++] = 0x0f; code[n++] = 0x22;
                code[n++] = (unsigned char)(0xc0 | ((cr & 7) << 3) |
                                            (src_reg & 7));
            } else {
                int dst = (p[0] == '%' && p[1] == '%')
                        ? a_regname(&p, file, line, tmpl)
                        : a_opreg(&p, opregs, opnames, nops, file, line, tmpl);
                if (src_imm_valid) {
                    int wide = mlen == 6 ||                 /* movabs, or */
                               src_imm > 0x7fffffffL ||     /* > imm32 */
                               src_imm < -0x80000000L;
                    if (wide) {                             /* movabs imm64 */
                        code[n++] = (unsigned char)(0x48 | (dst >= 8));
                        code[n++] = (unsigned char)(0xb8 | (dst & 7));
                        for (int b = 0; b < 8; b++)
                            code[n++] = (unsigned char)(src_imm >> (8 * b));
                    } else {                                /* C7 /0 imm32 */
                        code[n++] = (unsigned char)(0x48 | (dst >= 8));
                        code[n++] = 0xc7;
                        code[n++] = (unsigned char)(0xc0 | (dst & 7));
                        for (int b = 0; b < 4; b++)
                            code[n++] = (unsigned char)(src_imm >> (8 * b));
                    }
                } else {                                    /* mov reg,reg */
                    int rex = 0x48 | (src_reg >= 8 ? 4 : 0) | (dst >= 8);
                    code[n++] = (unsigned char)rex;
                    code[n++] = 0x89;
                    code[n++] = (unsigned char)(0xc0 | ((src_reg & 7) << 3) |
                                                (dst & 7));
                }
            }
            asm_next: ;
        }
        /* ---- ALU ops: add/sub/and/or/xor/cmp, `%src,%dst` or `$imm,%dst`.
         * Suffix 'q' or none = 64-bit (REX.W); 'l' = 32-bit. Register operands
         * only (no memory form yet — kernel inline asm doesn't need it). ---- */
        else if (asm_alu_lookup(m, mlen, &alu_rr, &alu_ext, &alu_w)) {
            a_ws(&p);
            /* `reg` holds a leading %N already consumed by the top-level
             * pre-parse; a $imm source leaves reg == -1. */
            if (reg < 0 && p[0] == '$') {             /* op $imm, %dst */
                long imm = a_imm(&p, file, line, tmpl);
                a_comma(&p, file, line, tmpl);
                a_ws(&p);
                int dst = (p[0] == '%' && p[1] == '%')
                        ? a_reg_any(&p, file, line, tmpl)
                        : a_opreg(&p, opregs, opnames, nops, file, line, tmpl);
                int use8 = imm >= -128 && imm <= 127;
                int rex = (alu_w ? 0x48 : 0x40) | (dst >= 8 ? 1 : 0);
                if (alu_w || dst >= 8) code[n++] = (unsigned char)rex;
                code[n++] = (unsigned char)(use8 ? 0x83 : 0x81);
                code[n++] = (unsigned char)(0xc0 | (alu_ext << 3) | (dst & 7));
                if (use8) code[n++] = (unsigned char)imm;
                else for (int b = 0; b < 4; b++)
                    code[n++] = (unsigned char)(imm >> (8 * b));
            } else {                                  /* op %src, %dst */
                int src = reg >= 0 ? reg
                        : (p[0] == '%' && p[1] == '%')
                        ? a_reg_any(&p, file, line, tmpl)
                        : a_opreg(&p, opregs, opnames, nops, file, line, tmpl);
                a_comma(&p, file, line, tmpl);
                a_ws(&p);
                int dst = (p[0] == '%' && p[1] == '%')
                        ? a_reg_any(&p, file, line, tmpl)
                        : a_opreg(&p, opregs, opnames, nops, file, line, tmpl);
                int rex = (alu_w ? 0x48 : 0x40) | (src >= 8 ? 4 : 0) |
                          (dst >= 8 ? 1 : 0);
                if (alu_w || src >= 8 || dst >= 8)
                    code[n++] = (unsigned char)rex;
                code[n++] = (unsigned char)alu_rr;
                code[n++] = (unsigned char)(0xc0 | ((src & 7) << 3) | (dst & 7));
            }
        }
        /* ---- lgdt/lidt %N and invlpg (%N): a memory operand whose address
         * is the register the "m"/"r" operand landed in ---- */
        else if (mlen == 4 && strncmp(m, "lgdt", 4) == 0) {
            int r = reg >= 0 ? reg
                  : a_opreg(&p, opregs, opnames, nops, file, line, tmpl);
            if (r >= 8) code[n++] = 0x41;
            code[n++] = 0x0f; code[n++] = 0x01;
            code[n++] = (unsigned char)(0x10 | (r & 7));     /* /2 (%r) */
        } else if (mlen == 4 && strncmp(m, "lidt", 4) == 0) {
            int r = reg >= 0 ? reg
                  : a_opreg(&p, opregs, opnames, nops, file, line, tmpl);
            if (r >= 8) code[n++] = 0x41;
            code[n++] = 0x0f; code[n++] = 0x01;
            code[n++] = (unsigned char)(0x18 | (r & 7));     /* /3 (%r) */
        } else if (mlen == 6 && strncmp(m, "invlpg", 6) == 0) {
            int r = a_memreg(&p, opregs, opnames, nops, file, line, tmpl);
            if (r >= 8) code[n++] = 0x41;
            code[n++] = 0x0f; code[n++] = 0x01;
            code[n++] = (unsigned char)(0x38 | (r & 7));     /* /7 (%r) */
        }
        /* ---- movdqa between xmm0 and memory (%N) ---- */
        else if (mlen == 6 && strncmp(m, "movdqa", 6) == 0) {
            a_ws(&p);
            int to_mem = p[0] == '%' && p[1] == '%';   /* movdqa %%xmm0,(%N) */
            int r;
            if (to_mem) {
                if (strncmp(p, "%%xmm0", 6) != 0)
                    diag_fatal(file, line, "asm movdqa source must be "
                               "%%%%xmm0 in \"%s\"", tmpl);
                p += 6;
                a_comma(&p, file, line, tmpl);
                r = a_memreg(&p, opregs, opnames, nops, file, line, tmpl);
            } else {                                   /* movdqa (%N),%%xmm0 */
                r = a_memreg(&p, opregs, opnames, nops, file, line, tmpl);
                a_comma(&p, file, line, tmpl);
                a_ws(&p);
                if (strncmp(p, "%%xmm0", 6) != 0)
                    diag_fatal(file, line, "asm movdqa dest must be "
                               "%%%%xmm0 in \"%s\"", tmpl);
                p += 6;
            }
            code[n++] = 0x66;
            if (r >= 8) code[n++] = 0x41;
            code[n++] = 0x0f;
            code[n++] = (unsigned char)(to_mem ? 0x7f : 0x6f);
            code[n++] = (unsigned char)(r & 7);        /* mod00 reg=xmm0 rm=r */
        } else {
            diag_fatal(file, line,
                       "asm instruction \"%.*s\" not supported", mlen, m);
        }
        /* discard any operand text this mnemonic did not itself consume
         * (a fixed-form instruction leaves its %N,%N in place), up to the
         * next instruction separator — a newline OR ';', since the kernel's
         * multi-line templates separate with '\n' and have no ';'. */
        while (*p && *p != ';' && *p != '\n' && *p != '\r')
            p++;
    }
    /* resolve each leaq's RIP-relative displacement to its local label: the
     * disp is relative to the END of the 4-byte field, and both label and
     * site are inside this block. */
    for (int i = 0; i < nfix; i++) {
        int off = -1;
        for (int j = 0; j < nlab; j++)
            if (labels[j].num == fixups[i].num) off = labels[j].off;
        if (off < 0)
            diag_fatal(file, line, "asm: local label %d not defined in "
                       "\"%s\"", fixups[i].num, tmpl);
        int disp = off - (fixups[i].patch + 4);
        for (int b = 0; b < 4; b++)
            code[fixups[i].patch + b] = (unsigned char)(disp >> (8 * b));
    }
    ia->code = code;
    ia->codelen = n;
}

/* Map a hard-register spelling (any width: rax/eax/ax/al/ah, r8/r8d/r8w/r8b,
 * …) of length `len` to its physical number 0..15, or -1 if it is not a GPR
 * (e.g. "cc", "memory", an xmm name). Used to EXCLUDE clobbered and
 * template-written registers from the operand allocator's free set. */
static int asm_phys_reg(const char *nm, int len)
{
    static const struct { const char *n; int r; } regs[] = {
        {"rax",0},{"eax",0},{"ax",0},{"al",0},{"ah",0},
        {"rcx",1},{"ecx",1},{"cx",1},{"cl",1},{"ch",1},
        {"rdx",2},{"edx",2},{"dx",2},{"dl",2},{"dh",2},
        {"rbx",3},{"ebx",3},{"bx",3},{"bl",3},{"bh",3},
        {"rsp",4},{"esp",4},{"sp",4},{"spl",4},
        {"rbp",5},{"ebp",5},{"bp",5},{"bpl",5},
        {"rsi",6},{"esi",6},{"si",6},{"sil",6},
        {"rdi",7},{"edi",7},{"di",7},{"dil",7},
        {"r8",8},{"r8d",8},{"r8w",8},{"r8b",8},
        {"r9",9},{"r9d",9},{"r9w",9},{"r9b",9},
        {"r10",10},{"r10d",10},{"r10w",10},{"r10b",10},
        {"r11",11},{"r11d",11},{"r11w",11},{"r11b",11},
        {"r12",12},{"r12d",12},{"r12w",12},{"r12b",12},
        {"r13",13},{"r13d",13},{"r13w",13},{"r13b",13},
        {"r14",14},{"r14d",14},{"r14w",14},{"r14b",14},
        {"r15",15},{"r15d",15},{"r15w",15},{"r15b",15},
    };
    for (unsigned i = 0; i < sizeof regs / sizeof regs[0]; i++)
        if ((int)strlen(regs[i].n) == len &&
            strncmp(regs[i].n, nm, (size_t)len) == 0)
            return regs[i].r;
    return -1;
}

/* Mark every hard register the template writes/reads as `%%reg` as used, so an
 * allocatable operand is never assigned one the asm's own instructions touch.
 * Conservative on purpose (a read-only `%%reg` is excluded too) — always sound,
 * only ever shrinks the free set. This is what stops a "r" operand from landing
 * in, say, %%rsi when the template does `movq %N,%%rsi` (K12). */
static void asm_mark_template_regs(const char *tmpl, int *used)
{
    for (const char *p = tmpl; *p; ) {
        if (p[0] == '%' && p[1] == '%') {
            p += 2;
            const char *q = p;
            while ((*q >= 'a' && *q <= 'z') || (*q >= '0' && *q <= '9')) q++;
            int r = asm_phys_reg(p, (int)(q - p));
            if (r >= 0) used[r] = 1;
            p = q;
        } else {
            p++;
        }
    }
}

/* Assign a free register to an operand whose constraint is allocatable
 * (reg == -2). The pool prefers the low, byte-addressable registers so a
 * setc destination needs no REX. */
static int asm_alloc_reg(int *used, const char *file, int line)
{
    static const int pool[] = { 0, 1, 2, 3, 6, 7, 8, 9, 10, 11 };
    for (unsigned i = 0; i < sizeof pool / sizeof pool[0]; i++)
        if (!used[pool[i]]) {
            used[pool[i]] = 1;
            return pool[i];
        }
    diag_fatal(file, line, "asm: out of registers for the operands");
    return -1;
}

/* Assign a free XMM register to an 'x' (SSE) asm operand. XMM registers are
 * encoded as 16 + n (0..7 -> 16..23) so they share one operand-register space
 * with the GPRs (0..15); codegen and asm_assemble decode reg >= 16 as xmm. */
static int asm_alloc_xmm(int *xused, const char *file, int line)
{
    for (int i = 0; i < 8; i++)
        if (!xused[i]) { xused[i] = 1; return 16 + i; }
    diag_fatal(file, line, "asm: out of xmm registers for the operands");
    return -1;
}

/* Extended asm on x86-64: assign the operands' registers (fixed ones from
 * sema, allocatable ones from what the template and clobbers leave free),
 * assemble the template, and emit the IR_ASM codegen places. */
void irg_asm_x86(struct ir_func *fn, struct stmt *s)
{
    struct asm_stmt *a = s->asm_s;
    struct ir_asm *ia = xcalloc(1, sizeof *ia);
    ia->nin = a->nin;
    ia->nout = a->nout;
    ia->in = xcalloc((size_t)(a->nin ? a->nin : 1), sizeof *ia->in);
    ia->out = xcalloc((size_t)(a->nout ? a->nout : 1),
                      sizeof *ia->out);
    /* Assign registers: fixed ones (from sema) reserve their slot;
     * allocatable ones (-2) get a free register. Then %N substitution
     * numbers outputs first, then inputs, exactly as gcc does. */
    int used[16] = { 0 };
    int xused[8] = { 0 };   /* xmm operands (reg 16..23) */
    for (int i = 0; i < a->nout; i++) {
        if (a->out[i].reg >= 16) xused[a->out[i].reg - 16] = 1;
        else if (a->out[i].reg >= 0) used[a->out[i].reg] = 1;
    }
    for (int i = 0; i < a->nin; i++) {
        if (a->in[i].reg >= 16) xused[a->in[i].reg - 16] = 1;
        else if (a->in[i].reg >= 0) used[a->in[i].reg] = 1;
    }
    /* Exclude clobbered registers, and any hard register the template
     * writes explicitly, from the allocatable pool (K12): otherwise an
     * allocatable "r" operand can land in a register the asm destroys
     * before it is used (e.g. %2 -> rdx while the template does
     * `movq %6,%%rdx`), silently corrupting the operand. */
    for (int i = 0; i < a->nclob; i++) {
        int r = asm_phys_reg(a->clob[i], (int)strlen(a->clob[i]));
        if (r >= 0) used[r] = 1;
    }
    asm_mark_template_regs(a->tmpl, used);
    int opregs[2 * MAX_PARAMS], nops = 0;
    const char *opnames[2 * MAX_PARAMS];
    for (int i = 0; i < a->nout; i++) {
        int r = a->out[i].reg;
        if (r == -2)      r = asm_alloc_reg(used, fn->src->file, s->line);
        else if (r == -3) r = asm_alloc_xmm(xused, fn->src->file, s->line);
        ia->out[i].reg = r;
        opnames[nops] = a->out[i].name;
        opregs[nops++] = r;
    }
    for (int i = 0; i < a->nin; i++) {
        int r = a->in[i].reg;
        if (r == -2)      r = asm_alloc_reg(used, fn->src->file, s->line);
        else if (r == -3) r = asm_alloc_xmm(xused, fn->src->file, s->line);
        ia->in[i].reg = r;
        opnames[nops] = a->in[i].name;
        opregs[nops++] = r;
    }
    asm_assemble(fn, s, opregs, opnames, nops, ia);
    /* An input carries its VALUE; an output the ADDRESS of its
     * lvalue. An xmm ('x') input is moved with movss/movsd, so its
     * size is the operand's own float width. */
    for (int i = 0; i < a->nin; i++) {
        ia->in[i].temp = gen_expr(fn, a->in[i].expr);
        ia->in[i].size = ia->in[i].reg >= 16
                       ? ty_size(a->in[i].expr->ty) : 8;
    }
    for (int i = 0; i < a->nout; i++) {
        ia->out[i].temp = gen_addr(fn, a->out[i].expr);
        ia->out[i].size = ty_size(a->out[i].expr->ty);
        /* "+": the register must START with the lvalue's value */
        ia->out[i].inout = strchr(a->out[i].constraint, '+') != NULL;
    }
    struct ir_ins *ins = emit(fn);
    ins->op = IR_ASM;
    ins->asm_ir = ia;
}
