/* The ARMv6-M (Thumb-1) vocabulary of src/arch/thumb/emit.c, swept across
 * its operands, for llvm-mc to referee.
 *
 * The bytes the encoders produce go to stdout. The assembly each one is
 * meant to be goes to stderr, one line per instruction, with its byte
 * offset and the encoder that wrote it in a comment:
 *
 *      adds r0, r1, #3         @ 0x1c t1_addsub_imm3
 *
 * tests/golden/thumb-v6m-encoding.sh assembles the text with
 * llvm-mc -triple=thumbv6m-none-eabi and compares the two byte streams.
 * An instruction ARMv6-M does not have fails there (llvm-mc rejects the
 * line), and so does one that encodes a different ARMv6-M instruction
 * (the bytes differ) -- the second is the dangerous kind, because it
 * runs.
 *
 * Covered: every t1_* encoder, and every encoder ARMv6-M code takes from
 * the ARMv7-M set as it is (the 16-bit branches, t_mov_reg, t_ldr_lit16,
 * the system instructions, BL). Every call must write exactly the size
 * its form has -- two bytes, four for BL, MRS, MSR and the barriers -- so
 * an encoder that widened, or wrote nothing, stops the sweep here.
 *
 * The sweeps are exhaustive over each field where the product of the
 * fields is small (every register in every position, every immediate),
 * and walk the bits where it is not (BL's 24-bit offset).
 *
 * `--refuse` checks the other half of the contract instead: each
 * int-returning encoder answers 0 (-1 for push and pop) and writes nothing
 * for an operand just outside its field.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/arch/code.h"
#include "../../src/arch/thumb/emit.h"

static struct code C;
static long n_insn;

static const char *const R[16] = {
    "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
    "r8", "r9", "r10", "r11", "r12", "sp", "lr", "pc"
};
/* B<c>'s conditions in encoding order, AL excluded (1110 is UDF). */
static const char *const COND[14] = {
    "eq", "ne", "hs", "lo", "mi", "pl", "vs", "vc",
    "hi", "ls", "ge", "lt", "gt", "le"
};

static void die(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "t1check: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

/* One instruction was emitted at `at`: it must be `size` bytes, and its
 * assembly goes to the listing. */
static void say(int at, int size, const char *enc, const char *fmt, ...)
{
    char text[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    if (C.len - at != size)
        die("%s wrote %d bytes for `%s`, where the form has %d", enc,
            C.len - at, text, size);
    fprintf(stderr, "\t%-28s @ 0x%x %s\n", text, (unsigned)at, enc);
    n_insn++;
}

/* Emit with CALL, then name it. */
#define I(SIZE, ENC, CALL, ...) \
    do { int at_ = C.len; (void)(CALL); say(at_, SIZE, ENC, __VA_ARGS__); } \
    while (0)

static const char *rlist(unsigned mask)
{
    static char buf[128];
    int n = 0;
    buf[n++] = '{';
    for (int r = 0; r < 16; r++)
        if (mask & (1u << r))
            n += snprintf(buf + n, sizeof buf - (size_t)n, "%s%s",
                          n > 1 ? ", " : "", R[r]);
    buf[n++] = '}';
    buf[n] = 0;
    return buf;
}

static void data_processing(void)
{
    static const struct { int op; const char *m; } alu[] = {
        { T_OP_AND, "ands" }, { T_OP_EOR, "eors" }, { T_OP_ADC, "adcs" },
        { T_OP_SBC, "sbcs" }, { T_OP_ORR, "orrs" }, { T_OP_BIC, "bics" }
    };
    static const struct { int op; const char *m; } sh[] = {
        { T_SH_LSL, "lsls" }, { T_SH_LSR, "lsrs" }, { T_SH_ASR, "asrs" },
        { T_SH_ROR, "rors" }
    };
    for (int d = 0; d < 8; d++)
        for (int v = 0; v < 256; v++)
            I(2, "t1_movs_imm", t1_movs_imm(&C, d, v), "movs %s, #%d", R[d], v);
    for (int d = 0; d < 8; d++)
        for (int m = 0; m < 8; m++)
            I(2, "t1_movs_reg", (t1_movs_reg(&C, d, m), 0),
              "movs %s, %s", R[d], R[m]);
    for (int s = 0; s < 2; s++) {
        int op = s ? T_OP_SUB : T_OP_ADD;
        const char *m = s ? "subs" : "adds";
        for (int d = 0; d < 8; d++)
            for (int n = 0; n < 8; n++) {
                for (int k = 0; k < 8; k++)
                    I(2, "t1_addsub_reg", (t1_addsub_reg(&C, op, d, n, k), 0),
                      "%s %s, %s, %s", m, R[d], R[n], R[k]);
                for (int v = 0; v < 8; v++)
                    I(2, "t1_addsub_imm3", t1_addsub_imm3(&C, op, d, n, v),
                      "%s %s, %s, #%d", m, R[d], R[n], v);
            }
        for (int d = 0; d < 8; d++)
            for (int v = 0; v < 256; v++)
                I(2, "t1_addsub_imm8", t1_addsub_imm8(&C, op, d, v),
                  "%s %s, #%d", m, R[d], v);
    }
    for (unsigned a = 0; a < sizeof alu / sizeof alu[0]; a++)
        for (int d = 0; d < 8; d++)
            for (int m = 0; m < 8; m++)
                I(2, "t1_alu_reg", (t1_alu_reg(&C, alu[a].op, d, m), 0),
                  "%s %s, %s", alu[a].m, R[d], R[m]);
    for (unsigned a = 0; a < sizeof sh / sizeof sh[0]; a++)
        for (int d = 0; d < 8; d++)
            for (int m = 0; m < 8; m++)
                I(2, "t1_shift_reg", (t1_shift_reg(&C, sh[a].op, d, m), 0),
                  "%s %s, %s", sh[a].m, R[d], R[m]);
    for (unsigned a = 0; a < 3; a++) {
        int lo = sh[a].op == T_SH_LSL ? 0 : 1, hi = lo + 31;
        for (int d = 0; d < 8; d++)
            for (int m = 0; m < 8; m++)
                for (int k = lo; k <= hi; k++)
                    I(2, "t1_shift_imm", t1_shift_imm(&C, sh[a].op, d, m, k),
                      "%s %s, %s, #%d", sh[a].m, R[d], R[m], k);
    }
    for (int n = 0; n < 8; n++)
        for (int v = 0; v < 256; v++)
            I(2, "t1_cmp_imm", t1_cmp_imm(&C, n, v), "cmp %s, #%d", R[n], v);
    for (int d = 0; d < 8; d++)
        for (int m = 0; m < 8; m++) {
            I(2, "t1_cmn", (t1_cmn(&C, d, m), 0), "cmn %s, %s", R[d], R[m]);
            I(2, "t1_tst", (t1_tst(&C, d, m), 0), "tst %s, %s", R[d], R[m]);
            I(2, "t1_negs", (t1_negs(&C, d, m), 0),
              "rsbs %s, %s, #0", R[d], R[m]);
            I(2, "t1_mvns", (t1_mvns(&C, d, m), 0), "mvns %s, %s", R[d], R[m]);
            I(2, "t1_muls", (t1_muls(&C, d, m), 0),
              "muls %s, %s, %s", R[d], R[m], R[d]);
            I(2, "t1_ext", (t1_ext(&C, d, m, 1, 1), 0), "sxtb %s, %s", R[d], R[m]);
            I(2, "t1_ext", (t1_ext(&C, d, m, 2, 1), 0), "sxth %s, %s", R[d], R[m]);
            I(2, "t1_ext", (t1_ext(&C, d, m, 1, 0), 0), "uxtb %s, %s", R[d], R[m]);
            I(2, "t1_ext", (t1_ext(&C, d, m, 2, 0), 0), "uxth %s, %s", R[d], R[m]);
            I(2, "t1_rev", (t1_rev(&C, d, m), 0), "rev %s, %s", R[d], R[m]);
            I(2, "t1_rev16", (t1_rev16(&C, d, m), 0), "rev16 %s, %s", R[d], R[m]);
            I(2, "t1_revsh", (t1_revsh(&C, d, m), 0), "revsh %s, %s", R[d], R[m]);
        }
    /* The high-register group: every pair the architecture defines. CMP
     * is T1 for two low registers and T2 otherwise; pc is not a CMP
     * operand, and ADD pc, pc is UNPREDICTABLE. A MOV of a register to
     * itself is not emitted at all (t_mov_reg), so it is not listed. */
    for (int d = 0; d < 16; d++)
        for (int m = 0; m < 16; m++) {
            if (!(d == 15 && m == 15))
                I(2, "t1_add_hi", (t1_add_hi(&C, d, m), 0),
                  "add %s, %s", R[d], R[m]);
            if (d < 15 && m < 15)
                I(2, "t1_cmp_reg", (t1_cmp_reg(&C, d, m), 0),
                  "cmp %s, %s", R[d], R[m]);
            if (d != m)
                I(2, "t_mov_reg", (t_mov_reg(&C, d, m), 0),
                  "mov %s, %s", R[d], R[m]);
        }
}

static void memory(void)
{
    static const char *const ld[3] = { "ldrb", "ldrh", "ldr" };
    static const char *const st[3] = { "strb", "strh", "str" };
    for (int z = 0; z < 3; z++) {
        int size = 1 << z;
        for (int t = 0; t < 8; t++)
            for (int n = 0; n < 8; n++)
                for (int k = 0; k < 32; k++) {
                    I(2, "t1_ldst_imm", t1_ldst_imm(&C, t, n, k * size, size, 0),
                      "%s %s, [%s, #%d]", ld[z], R[t], R[n], k * size);
                    I(2, "t1_ldst_imm", t1_ldst_imm(&C, t, n, k * size, size, 1),
                      "%s %s, [%s, #%d]", st[z], R[t], R[n], k * size);
                }
    }
    for (int t = 0; t < 8; t++)
        for (int n = 0; n < 8; n++)
            for (int m = 0; m < 8; m++) {
                for (int z = 0; z < 3; z++) {
                    I(2, "t1_ldst_reg", (t1_ldst_reg(&C, t, n, m, 1 << z, 0, 0), 0),
                      "%s %s, [%s, %s]", ld[z], R[t], R[n], R[m]);
                    I(2, "t1_ldst_reg", (t1_ldst_reg(&C, t, n, m, 1 << z, 0, 1), 0),
                      "%s %s, [%s, %s]", st[z], R[t], R[n], R[m]);
                }
                I(2, "t1_ldst_reg", (t1_ldst_reg(&C, t, n, m, 1, 1, 0), 0),
                  "ldrsb %s, [%s, %s]", R[t], R[n], R[m]);
                I(2, "t1_ldst_reg", (t1_ldst_reg(&C, t, n, m, 2, 1, 0), 0),
                  "ldrsh %s, [%s, %s]", R[t], R[n], R[m]);
            }
    for (int t = 0; t < 8; t++)
        for (int k = 0; k <= 1020; k += 4) {
            I(2, "t1_ldst_sp", t1_ldst_sp(&C, t, k, 0),
              "ldr %s, [sp, #%d]", R[t], k);
            I(2, "t1_ldst_sp", t1_ldst_sp(&C, t, k, 1),
              "str %s, [sp, #%d]", R[t], k);
            I(2, "t1_add_sp_imm", t1_add_sp_imm(&C, t, k),
              "add %s, sp, #%d", R[t], k);
            I(2, "t1_adr", t1_adr(&C, t, k), "adr %s, #%d", R[t], k);
            I(2, "t_ldr_lit16", t_ldr_lit16(&C, t, k),
              "ldr %s, [pc, #%d]", R[t], k);
        }
    /* The patchers, which measure from Align(pc, 4): consecutive
     * instructions alternate between the two alignments a halfword can
     * have, so each target is reached from both. */
    for (int t = 0; t < 8; t++)
        for (int k = 0; k <= 1020; k += 4) {
            int at = C.len;
            t1_adr(&C, t, 0);
            if (!t1_patch_adr(&C, at, ((at + 4) & ~3) + k))
                die("t1_patch_adr refused #%d from 0x%x", k, at);
            say(at, 2, "t1_patch_adr", "adr %s, #%d", R[t], k);
            at = C.len;
            t_ldr_lit16(&C, t, 0);
            if (!t1_patch_ldr_lit(&C, at, ((at + 4) & ~3) + k))
                die("t1_patch_ldr_lit refused #%d from 0x%x", k, at);
            say(at, 2, "t1_patch_ldr_lit", "ldr %s, [pc, #%d]", R[t], k);
        }
    for (int k = 0; k <= 508; k += 4) {
        I(2, "t1_sp_adjust", t1_sp_adjust(&C, k, 0), "add sp, #%d", k);
        I(2, "t1_sp_adjust", t1_sp_adjust(&C, k, 1), "sub sp, #%d", k);
    }
    /* Every list, every base. A load writes the base back exactly when it
     * is not in the list, and the syntax says which; a store of the base
     * is defined only when it is the lowest register. */
    for (int n = 0; n < 8; n++)
        for (unsigned mask = 1; mask < 256; mask++) {
            int in = (mask >> n) & 1;
            I(2, "t1_ldm_stm", t1_ldm_stm(&C, n, mask, 1),
              "ldm %s%s, %s", R[n], in ? "" : "!", rlist(mask));
            if (!in || !(mask & ((1u << n) - 1u)))
                I(2, "t1_ldm_stm", t1_ldm_stm(&C, n, mask, 0),
                  "stm %s!, %s", R[n], rlist(mask));
        }
    for (unsigned mask = 1; mask < 512; mask++) {
        unsigned lst = (mask & 0xffu) | ((mask & 0x100u) ? 1u << 14 : 0u);
        unsigned lsp = (mask & 0xffu) | ((mask & 0x100u) ? 1u << 15 : 0u);
        I(2, "t1_push", t1_push(&C, lst), "push %s", rlist(lst));
        I(2, "t1_pop", t1_pop(&C, lsp), "pop %s", rlist(lsp));
    }
}

static void control(void)
{
    /* The 16-bit branches, at every offset each can hold. The target is
     * outside the stream, which nothing here executes; the offset is what
     * is compared, written as llvm-mc's `b #off` (from the branch + 4). */
    for (int cc = 0; cc < 14; cc++)
        for (int off = -256; off <= 254; off += 2) {
            int at = t_bcond16(&C, cc);
            if (!t_patch_bcond16(&C, at, at + 4 + off))
                die("t_patch_bcond16 refused %d", off);
            say(at, 2, "t_bcond16", "b%s #%d", COND[cc], off);
        }
    for (int off = -2048; off <= 2046; off += 2) {
        int at = t_b16(&C);
        if (!t_patch_b16(&C, at, at + 4 + off))
            die("t_patch_b16 refused %d", off);
        say(at, 2, "t_b16", "b #%d", off);
    }
    /* BL's 24-bit offset, one bit at a time and at both ends: S, J1 and J2
     * are where it goes wrong. */
    for (int b = 1; b <= 23; b++) {
        long offs[4];
        offs[0] = 1L << b;
        offs[1] = -(1L << b);
        offs[2] = (1L << b) - 2;
        offs[3] = -(1L << b) + 2;
        for (int k = 0; k < 4; k++) {
            long off = offs[k];
            if (off < -16777216L || off > 16777214L)
                continue;
            int at = t_bl(&C);
            t_patch_bl(&C, at, at + 4 + (int)off);
            say(at, 4, "t_bl", "bl #%ld", off);
        }
    }
    {
        int at = t_bl(&C);
        t_patch_bl(&C, at, at + 4 - 16777216);
        say(at, 4, "t_bl", "bl #%ld", -16777216L);
    }
    for (int m = 0; m < 16; m++) {
        I(2, "t_bx", (t_bx(&C, m), 0), "bx %s", R[m]);
        if (m < 15)
            I(2, "t_blx", (t_blx(&C, m), 0), "blx %s", R[m]);
    }
    for (int v = 0; v < 256; v++) {
        I(2, "t1_udf", t1_udf(&C, v), "udf #%d", v);
        I(2, "t_svc", (t_svc(&C, v), 0), "svc #%d", v);
        I(2, "t_bkpt", (t_bkpt(&C, v), 0), "bkpt #%d", v);
    }
}

static void system_instructions(void)
{
    /* The special registers ARMv6-M has: no BASEPRI and no FAULTMASK. */
    static const struct { int sysm; const char *n; } sys[] = {
        { T_SYS_APSR, "apsr" }, { T_SYS_IAPSR, "iapsr" },
        { T_SYS_EAPSR, "eapsr" }, { T_SYS_XPSR, "xpsr" },
        { T_SYS_IPSR, "ipsr" }, { T_SYS_EPSR, "epsr" },
        { T_SYS_IEPSR, "iepsr" }, { T_SYS_MSP, "msp" }, { T_SYS_PSP, "psp" },
        { T_SYS_PRIMASK, "primask" }, { T_SYS_CONTROL, "control" }
    };
    static const struct { int op; const char *n; } hint[] = {
        { T_HINT_NOP, "nop" }, { T_HINT_YIELD, "yield" },
        { T_HINT_WFE, "wfe" }, { T_HINT_WFI, "wfi" }, { T_HINT_SEV, "sev" }
    };
    for (unsigned s = 0; s < sizeof sys / sizeof sys[0]; s++)
        for (int r = 0; r < 15; r++) {
            if (r == 13)
                continue;                  /* sp: UNPREDICTABLE in both */
            I(4, "t_mrs", (t_mrs(&C, r, sys[s].sysm), 0),
              "mrs %s, %s", R[r], sys[s].n);
            I(4, "t_msr", (t_msr(&C, sys[s].sysm, r), 0),
              "msr %s, %s", sys[s].n, R[r]);
        }
    I(4, "t_barrier", (t_barrier(&C, T_BAR_DMB), 0), "dmb sy");
    I(4, "t_barrier", (t_barrier(&C, T_BAR_DSB), 0), "dsb sy");
    I(4, "t_barrier", (t_barrier(&C, T_BAR_ISB), 0), "isb sy");
    I(2, "t_cps", (t_cps(&C, 1, 1, 0), 0), "cpsid i");
    I(2, "t_cps", (t_cps(&C, 0, 1, 0), 0), "cpsie i");
    for (unsigned h = 0; h < sizeof hint / sizeof hint[0]; h++)
        I(2, "t_hint", (t_hint(&C, hint[h].op), 0), "%s", hint[h].n);
    I(2, "t_nop", (t_nop(&C), 0), "nop");
}

/* ---- --refuse ---------------------------------------------------------- */

static int n_refused;

/* `ret` is what the encoder answered for an operand it cannot hold. */
static void refused(const char *what, int ret, int want, int len0)
{
    if (ret != want)
        die("%s: answered %d, not %d", what, ret, want);
    if (C.len != len0)
        die("%s: refused, but wrote %d bytes", what, C.len - len0);
    n_refused++;
}

#define REFUSE(WANT, CALL) \
    do { int l_ = C.len; refused(#CALL, (CALL), (WANT), l_); } while (0)

static void refusals(void)
{
    REFUSE(0, t1_movs_imm(&C, 0, -1));
    REFUSE(0, t1_movs_imm(&C, 0, 256));
    REFUSE(0, t1_addsub_imm3(&C, T_OP_ADD, 0, 1, -1));
    REFUSE(0, t1_addsub_imm3(&C, T_OP_ADD, 0, 1, 8));
    REFUSE(0, t1_addsub_imm3(&C, T_OP_SUB, 0, 1, 8));
    REFUSE(0, t1_addsub_imm8(&C, T_OP_ADD, 0, -1));
    REFUSE(0, t1_addsub_imm8(&C, T_OP_SUB, 0, 256));
    REFUSE(0, t1_shift_imm(&C, T_SH_LSL, 0, 1, 32));
    REFUSE(0, t1_shift_imm(&C, T_SH_LSL, 0, 1, -1));
    REFUSE(0, t1_shift_imm(&C, T_SH_LSR, 0, 1, 0));
    REFUSE(0, t1_shift_imm(&C, T_SH_LSR, 0, 1, 33));
    REFUSE(0, t1_shift_imm(&C, T_SH_ASR, 0, 1, 0));
    REFUSE(0, t1_shift_imm(&C, T_SH_ASR, 0, 1, 33));
    REFUSE(0, t1_cmp_imm(&C, 0, -1));
    REFUSE(0, t1_cmp_imm(&C, 0, 256));
    REFUSE(0, t1_ldst_imm(&C, 0, 1, 128, 4, 0));
    REFUSE(0, t1_ldst_imm(&C, 0, 1, 2, 4, 1));
    REFUSE(0, t1_ldst_imm(&C, 0, 1, -4, 4, 0));
    REFUSE(0, t1_ldst_imm(&C, 0, 1, 64, 2, 0));
    REFUSE(0, t1_ldst_imm(&C, 0, 1, 1, 2, 1));
    REFUSE(0, t1_ldst_imm(&C, 0, 1, 32, 1, 0));
    REFUSE(0, t1_ldst_imm(&C, 0, 1, -1, 1, 1));
    REFUSE(0, t1_ldst_sp(&C, 0, 1024, 0));
    REFUSE(0, t1_ldst_sp(&C, 0, 2, 1));
    REFUSE(0, t1_ldst_sp(&C, 0, -4, 0));
    REFUSE(0, t1_add_sp_imm(&C, 0, 1024));
    REFUSE(0, t1_add_sp_imm(&C, 0, 6));
    REFUSE(0, t1_add_sp_imm(&C, 0, -4));
    REFUSE(0, t1_sp_adjust(&C, 512, 1));
    REFUSE(0, t1_sp_adjust(&C, 6, 0));
    REFUSE(0, t1_sp_adjust(&C, -4, 1));
    REFUSE(0, t1_adr(&C, 0, 1024));
    REFUSE(0, t1_adr(&C, 0, 2));
    REFUSE(0, t1_adr(&C, 0, -4));
    REFUSE(0, t1_ldm_stm(&C, 0, 0, 1));
    REFUSE(0, t1_ldm_stm(&C, 0, 0x100, 0));
    REFUSE(0, t1_ldm_stm(&C, 1, 0x3, 0));      /* stores r1, not lowest */
    REFUSE(-1, t1_push(&C, 0));
    REFUSE(-1, t1_push(&C, 1u << 8));
    REFUSE(-1, t1_push(&C, 1u << 15));
    REFUSE(-1, t1_pop(&C, 0));
    REFUSE(-1, t1_pop(&C, 1u << 14));
    REFUSE(-1, t1_pop(&C, 1u << 12));
    REFUSE(0, t1_udf(&C, -1));
    REFUSE(0, t1_udf(&C, 256));
    /* The patchers: a target behind, past the reach, or between words,
     * from both alignments -- and the instruction left as it was. */
    for (int pad = 0; pad < 2; pad++) {
        int at, base;
        unsigned before;
        if (pad)
            t_nop(&C);
        at = C.len;
        base = (at + 4) & ~3;
        t1_adr(&C, 3, 8);
        t_ldr_lit16(&C, 4, 8);
        before = (unsigned)(C.p[at] | C.p[at + 1] << 8 |
                            C.p[at + 2] << 16 | (unsigned)C.p[at + 3] << 24);
        REFUSE(0, t1_patch_adr(&C, at, base - 4));
        REFUSE(0, t1_patch_adr(&C, at, base + 1024));
        REFUSE(0, t1_patch_adr(&C, at, base + 2));
        REFUSE(0, t1_patch_ldr_lit(&C, at + 2, ((at + 6) & ~3) - 4));
        REFUSE(0, t1_patch_ldr_lit(&C, at + 2, ((at + 6) & ~3) + 1024));
        REFUSE(0, t1_patch_ldr_lit(&C, at + 2, ((at + 6) & ~3) + 2));
        if (before != (unsigned)(C.p[at] | C.p[at + 1] << 8 |
                                 C.p[at + 2] << 16 |
                                 (unsigned)C.p[at + 3] << 24))
            die("a refused patch changed the instruction it refused");
    }
    {
        int at = t_bcond16(&C, T_EQ);
        REFUSE(0, t_patch_bcond16(&C, at, at + 4 + 256));
        REFUSE(0, t_patch_bcond16(&C, at, at + 4 - 258));
        REFUSE(0, t_patch_bcond16(&C, at, at + 4 + 1));
        at = t_b16(&C);
        REFUSE(0, t_patch_b16(&C, at, at + 4 + 2048));
        REFUSE(0, t_patch_b16(&C, at, at + 4 - 2050));
        REFUSE(0, t_patch_b16(&C, at, at + 4 + 1));
    }
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--refuse") == 0) {
        refusals();
        printf("%d out-of-field operands refused, nothing written\n",
               n_refused);
        return 0;
    }
    if (argc > 1)
        die("usage: t1check [--refuse]  (bytes to stdout, assembly to stderr)");
    fprintf(stderr, "\t.syntax unified\n\t.thumb\n");
    data_processing();
    memory();
    control();
    system_instructions();
    fwrite(C.p, 1, (size_t)C.len, stdout);
    return 0;
}
