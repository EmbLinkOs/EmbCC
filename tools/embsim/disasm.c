/* disasm.c -- one instruction as text, for the fault report: Thumb
 * (ARMv6-M's 16-bit set whole, and the 32-bit forms a compiler emits for
 * loads, stores, multiples, calls and branches, divides and multiplies),
 * and RISC-V (RV32I and RV64I, M, A, the CSR and system instructions,
 * and the compressed forms as the instruction the core expands each to,
 * marked `(compressed)`), MIPS32r2 (the integer set, coprocessor 0's
 * and the DSP accumulators' moves), and the PIC24F (its whole set, in
 * XC16's syntax but for the literal base: f and addresses in hex). What it
 * does not know it prints as the encoding, `.inst 0x...`: the fault
 * report always has the address, the symbol and the source line beside
 * it. */
#include <string.h>

#include "devices.h"
#include "disasm.h"
#include "mips.h"
#include "pic24.h"
#include "riscv.h"

static const char *const rn[16] = { "r0", "r1", "r2", "r3", "r4", "r5", "r6",
                                    "r7", "r8", "r9", "r10", "r11", "r12",
                                    "sp", "lr", "pc" };
static const char *const cc[16] = { "eq", "ne", "cs", "cc", "mi", "pl", "vs",
                                    "vc", "hi", "ls", "ge", "lt", "gt", "le",
                                    "", "" };

static void reglist(char *b, size_t n, u32 mask)
{
    size_t k = strlen(b);
    int first = 1;
    snprintf(b + k, n - k, "{");
    for (int r = 0; r < 16; r++)
        if (mask >> r & 1) {
            k = strlen(b);
            snprintf(b + k, n - k, "%s%s", first ? "" : ", ", rn[r]);
            first = 0;
        }
    k = strlen(b);
    snprintf(b + k, n - k, "}");
}

static int thumb16(u32 pc, u32 h, char *b, size_t n)
{
    u32 rd = h & 7, rm = (h >> 3) & 7, rn_ = (h >> 6) & 7, imm5 = (h >> 6) & 31;
    static const char *const dp[16] = { "ands", "eors", "lsls", "lsrs", "asrs",
                                        "adcs", "sbcs", "rors", "tst", "rsbs",
                                        "cmp", "cmn", "orrs", "muls", "bics",
                                        "mvns" };
    static const char *const ls[8] = { "str", "strh", "strb", "ldrsb", "ldr",
                                       "ldrh", "ldrb", "ldrsh" };
    switch (h >> 11) {
    case 0:
        if (imm5)
            snprintf(b, n, "lsls %s, %s, #%u", rn[rd], rn[rm], imm5);
        else
            snprintf(b, n, "movs %s, %s", rn[rd], rn[rm]);
        return 1;
    case 1: snprintf(b, n, "lsrs %s, %s, #%u", rn[rd], rn[rm], imm5 ? imm5 : 32); return 1;
    case 2: snprintf(b, n, "asrs %s, %s, #%u", rn[rd], rn[rm], imm5 ? imm5 : 32); return 1;
    case 3:
        if (h & 0x400)
            snprintf(b, n, "%s %s, %s, #%u", h & 0x200 ? "subs" : "adds", rn[rd],
                     rn[rm], rn_);
        else
            snprintf(b, n, "%s %s, %s, %s", h & 0x200 ? "subs" : "adds", rn[rd],
                     rn[rm], rn[rn_]);
        return 1;
    case 4: snprintf(b, n, "movs %s, #%u", rn[(h >> 8) & 7], h & 255); return 1;
    case 5: snprintf(b, n, "cmp %s, #%u", rn[(h >> 8) & 7], h & 255); return 1;
    case 6: snprintf(b, n, "adds %s, #%u", rn[(h >> 8) & 7], h & 255); return 1;
    case 7: snprintf(b, n, "subs %s, #%u", rn[(h >> 8) & 7], h & 255); return 1;
    case 8:
        if (!(h & 0x400)) {
            snprintf(b, n, "%s %s, %s", dp[(h >> 6) & 15], rn[rd], rn[rm]);
            return 1;
        } else {
            u32 d = (h & 7) | (h >> 4 & 8), m = (h >> 3) & 15;
            switch ((h >> 8) & 3) {
            case 0: snprintf(b, n, "add %s, %s", rn[d], rn[m]); return 1;
            case 1: snprintf(b, n, "cmp %s, %s", rn[d], rn[m]); return 1;
            case 2: snprintf(b, n, "mov %s, %s", rn[d], rn[m]); return 1;
            default: snprintf(b, n, "%s %s", h & 0x80 ? "blx" : "bx", rn[m]); return 1;
            }
        }
    case 9:
        snprintf(b, n, "ldr %s, [pc, #%u]  @ 0x%08x", rn[(h >> 8) & 7], (h & 255) * 4,
                 ((pc + 4) & ~3u) + (h & 255) * 4);
        return 1;
    case 10: case 11:
        snprintf(b, n, "%s %s, [%s, %s]", ls[(h >> 9) & 7], rn[rd], rn[rm], rn[rn_]);
        return 1;
    case 12: case 13: case 14: case 15: {
        int byte = (h >> 12) & 1, ld = (h >> 11) & 1;
        snprintf(b, n, "%s %s, [%s, #%u]", byte ? (ld ? "ldrb" : "strb") : (ld ? "ldr" : "str"),
                 rn[rd], rn[rm], byte ? imm5 : imm5 * 4);
        return 1;
    }
    case 16: case 17:
        snprintf(b, n, "%s %s, [%s, #%u]", h & 0x800 ? "ldrh" : "strh", rn[rd], rn[rm],
                 imm5 * 2);
        return 1;
    case 18: case 19:
        snprintf(b, n, "%s %s, [sp, #%u]", h & 0x800 ? "ldr" : "str", rn[(h >> 8) & 7],
                 (h & 255) * 4);
        return 1;
    case 20: snprintf(b, n, "add %s, pc, #%u", rn[(h >> 8) & 7], (h & 255) * 4); return 1;
    case 21: snprintf(b, n, "add %s, sp, #%u", rn[(h >> 8) & 7], (h & 255) * 4); return 1;
    case 22: case 23:
        if ((h & 0xff00) == 0xb000) {
            snprintf(b, n, "%s sp, #%u", h & 0x80 ? "sub" : "add", (h & 127) * 4);
            return 1;
        }
        if ((h & 0xf500) == 0xb100) {
            u32 off = ((h >> 3) & 0x3e) | ((h >> 4) & 0x40);
            snprintf(b, n, "%s %s, 0x%08x", h & 0x800 ? "cbnz" : "cbz", rn[rd], pc + 4 + off);
            return 1;
        }
        if ((h & 0xff00) == 0xb200) {
            static const char *const x[4] = { "sxth", "sxtb", "uxth", "uxtb" };
            snprintf(b, n, "%s %s, %s", x[(h >> 6) & 3], rn[rd], rn[rm]);
            return 1;
        }
        if ((h & 0xfe00) == 0xb400 || (h & 0xfe00) == 0xbc00) {
            int pop = (h & 0x800) != 0;
            snprintf(b, n, "%s ", pop ? "pop" : "push");
            reglist(b, n, (h & 255) | (h & 0x100 ? (pop ? 0x8000u : 0x4000u) : 0));
            return 1;
        }
        if ((h & 0xffe8) == 0xb660) {
            snprintf(b, n, "cpsi%s %s", h & 0x10 ? "d" : "e", h & 2 ? "i" : "f");
            return 1;
        }
        if ((h & 0xff00) == 0xba00) {
            static const char *const x[4] = { "rev", "rev16", "", "revsh" };
            snprintf(b, n, "%s %s, %s", x[(h >> 6) & 3], rn[rd], rn[rm]);
            return 1;
        }
        if ((h & 0xff00) == 0xbe00) {
            snprintf(b, n, "bkpt 0x%02x", h & 255);
            return 1;
        }
        if ((h & 0xff00) == 0xbf00) {
            static const char *const hint[5] = { "nop", "yield", "wfe", "wfi", "sev" };
            if (h & 15)
                snprintf(b, n, "it 0x%x", h & 255);
            else if (((h >> 4) & 15) < 5)
                snprintf(b, n, "%s", hint[(h >> 4) & 15]);
            else
                return 0;
            return 1;
        }
        return 0;
    case 24: case 25:
        snprintf(b, n, "%s %s!, ", h & 0x800 ? "ldmia" : "stmia", rn[(h >> 8) & 7]);
        reglist(b, n, h & 255);
        return 1;
    case 26: case 27: {
        u32 c = (h >> 8) & 15;
        if (c == 14) {
            snprintf(b, n, "udf #%u", h & 255);
            return 1;
        }
        if (c == 15) {
            snprintf(b, n, "svc #%u", h & 255);
            return 1;
        }
        snprintf(b, n, "b%s 0x%08x", cc[c], pc + 4 + (u32)((s32)(h << 24) >> 23));
        return 1;
    }
    case 28:
        snprintf(b, n, "b 0x%08x", pc + 4 + (u32)((s32)(h << 21) >> 20));
        return 1;
    }
    return 0;
}

static int thumb32(u32 pc, u32 h1, u32 h2, char *b, size_t n)
{
    u32 rn_ = h1 & 15, rt = h2 >> 12;
    if ((h1 & 0xf800) == 0xf000 && (h2 & 0x8000)) {
        u32 s = (h1 >> 10) & 1, j1 = (h2 >> 13) & 1, j2 = (h2 >> 11) & 1;
        if ((h2 & 0x5000) == 0x5000 || (h2 & 0x5000) == 0x1000) {
            u32 i1 = !(j1 ^ s), i2 = !(j2 ^ s);
            s32 off = (s32)(s << 24 | i1 << 23 | i2 << 22 | (h1 & 0x3ff) << 12 |
                            (h2 & 0x7ff) << 1);
            off = (off << 7) >> 7;
            snprintf(b, n, "%s 0x%08x", h2 & 0x4000 ? "bl" : "b.w", pc + 4 + (u32)off);
            return 1;
        }
        if ((h2 & 0x5000) == 0) {
            u32 c = (h1 >> 6) & 15;
            if (c >= 14)
                return 0;
            s32 off = (s32)(s << 20 | j2 << 19 | j1 << 18 | (h1 & 0x3f) << 12 |
                            (h2 & 0x7ff) << 1);
            off = (off << 11) >> 11;
            snprintf(b, n, "b%s.w 0x%08x", cc[c], pc + 4 + (u32)off);
            return 1;
        }
        if (h1 == 0xf7f0 && (h2 & 0xf000) == 0xa000) {
            snprintf(b, n, "udf.w #%u", (h1 & 15) << 12 | (h2 & 0xfff));
            return 1;
        }
        return 0;
    }
    if ((h1 & 0xfe50) == 0xe810 || (h1 & 0xfe50) == 0xe800) {     /* LDM/STM */
        int ld = (h1 >> 4) & 1, db = (h1 >> 8) & 1, wb = (h1 >> 5) & 1;
        if (rn_ == 13 && wb && ld != db)
            snprintf(b, n, "%s ", ld ? "pop.w" : "push.w");
        else
            snprintf(b, n, "%s%s %s%s, ", ld ? "ldm" : "stm", db ? "db" : "ia",
                     rn[rn_], wb ? "!" : "");
        reglist(b, n, h2);
        return 1;
    }
    if ((h1 & 0xfe40) == 0xe840 && (h1 & 0x0120)) {               /* LDRD/STRD */
        u32 imm = (h2 & 255) * 4;
        int u = (h1 >> 7) & 1, p = (h1 >> 8) & 1, w = (h1 >> 5) & 1;
        snprintf(b, n, "%s %s, %s, [%s%s, #%s%u]%s%s", h1 & 0x10 ? "ldrd" : "strd",
                 rn[rt], rn[(h2 >> 8) & 15], rn[rn_], p ? "" : "]", u ? "" : "-", imm,
                 p ? "]" : "", p && w ? "!" : "");
        return 1;
    }
    if ((h1 & 0xfe00) == 0xf800) {                                  /* LDR/STR */
        static const char *const nm[2][4] = { { "strb", "strh", "str", "?" },
                                              { "ldrb", "ldrh", "ldr", "?" } };
        int ld = (h1 >> 4) & 1, sz = (h1 >> 5) & 3, sgn = (h1 >> 8) & 1;
        if (sz == 3)
            return 0;
        const char *op = nm[ld][sz];
        char sop[8];
        if (sgn && ld) {
            snprintf(sop, sizeof sop, "ldrs%s", sz ? "h" : "b");
            op = sop;
        }
        if (rn_ == 15 && ld) {
            u32 imm = h2 & 0xfff;
            snprintf(b, n, "%s.w %s, [pc, #%s%u]", op, rn[rt], h1 & 0x80 ? "" : "-", imm);
        } else if (h1 & 0x80) {
            snprintf(b, n, "%s.w %s, [%s, #%u]", op, rn[rt], rn[rn_], h2 & 0xfff);
        } else if ((h2 & 0x800) == 0x800) {
            int p = (h2 >> 10) & 1, u = (h2 >> 9) & 1, w = (h2 >> 8) & 1;
            snprintf(b, n, "%s %s, [%s%s, #%s%u]%s", op, rn[rt], rn[rn_], p ? "" : "]",
                     u ? "" : "-", h2 & 255, p ? (w ? "]!" : "]") : "");
        } else if (!(h2 & 0xfc0)) {
            snprintf(b, n, "%s.w %s, [%s, %s, lsl #%u]", op, rn[rt], rn[rn_],
                     rn[h2 & 15], (h2 >> 4) & 3);
        } else
            return 0;
        return 1;
    }
    if ((h1 & 0xfa00) == 0xf000 && !(h2 & 0x8000)) {             /* modified imm */
        static const char *const dp[16] = { "and", "bic", "orr", "orn", "eor", 0, 0,
                                            0, "add", 0, "adc", "sbc", 0, "sub",
                                            "rsb", 0 };
        u32 op = (h1 >> 5) & 15, s = (h1 >> 4) & 1, rd = (h2 >> 8) & 15;
        u32 i12 = ((h1 >> 10) & 1) << 11 | ((h2 >> 12) & 7) << 8 | (h2 & 255);
        u32 imm8 = i12 & 255, imm;
        switch (i12 >> 8) {
        case 0: imm = imm8; break;
        case 1: imm = imm8 << 16 | imm8; break;
        case 2: imm = imm8 << 24 | imm8 << 8; break;
        case 3: imm = imm8 * 0x01010101u; break;
        default: {
            u32 v = 0x80 | (i12 & 0x7f), rot = i12 >> 7;
            imm = v >> rot | v << (32 - rot);
        }
        }
        if (!dp[op])
            return 0;
        if (rd == 15 && s && (op == 0 || op == 4 || op == 8 || op == 13))
            snprintf(b, n, "%s %s, #%u", op == 0 ? "tst" : op == 4 ? "teq" :
                     op == 8 ? "cmn" : "cmp", rn[rn_], imm);
        else if (rn_ == 15 && (op == 2 || op == 3))
            snprintf(b, n, "%s%s.w %s, #%u", op == 2 ? "mov" : "mvn", s ? "s" : "",
                     rn[rd], imm);
        else
            snprintf(b, n, "%s%s.w %s, %s, #%u", dp[op], s ? "s" : "", rn[rd], rn[rn_], imm);
        return 1;
    }
    if ((h1 & 0xfa00) == 0xf200 && !(h2 & 0x8000)) {             /* plain imm */
        u32 op = (h1 >> 4) & 31, rd = (h2 >> 8) & 15;
        u32 i12 = ((h1 >> 10) & 1) << 11 | ((h2 >> 12) & 7) << 8 | (h2 & 255);
        if (op == 0 || op == 10)
            snprintf(b, n, "%s %s, %s, #%u", op ? "subw" : "addw", rn[rd], rn[rn_], i12);
        else if (op == 4 || op == 12)
            snprintf(b, n, "%s %s, #%u", op == 4 ? "movw" : "movt", rn[rd],
                     (h1 & 15) << 12 | i12);
        else
            return 0;
        return 1;
    }
    if ((h1 & 0xfe00) == 0xea00) {                                  /* shifted reg */
        static const char *const dp[16] = { "and", "bic", "orr", "orn", "eor", 0, 0,
                                            0, "add", 0, "adc", "sbc", 0, "sub",
                                            "rsb", 0 };
        static const char *const sh[4] = { "lsl", "lsr", "asr", "ror" };
        u32 op = (h1 >> 5) & 15, s = (h1 >> 4) & 1, rd = (h2 >> 8) & 15, rm = h2 & 15;
        u32 amt = ((h2 >> 12) & 7) << 2 | ((h2 >> 6) & 3), ty = (h2 >> 4) & 3;
        char shift[24] = "";
        if (amt || ty)
            snprintf(shift, sizeof shift, ", %s #%u", sh[ty], amt ? amt : 32);
        if (!dp[op])
            return 0;
        if (rd == 15 && s && (op == 0 || op == 4 || op == 8 || op == 13))
            snprintf(b, n, "%s.w %s, %s%s", op == 0 ? "tst" : op == 4 ? "teq" :
                     op == 8 ? "cmn" : "cmp", rn[rn_], rn[rm], shift);
        else if (rn_ == 15 && op == 2 && (amt || ty))
            snprintf(b, n, "%s%s.w %s, %s, #%u", sh[ty], s ? "s" : "", rn[rd], rn[rm],
                     amt ? amt : 32);
        else if (rn_ == 15 && (op == 2 || op == 3))
            snprintf(b, n, "%s%s.w %s, %s%s", op == 2 ? "mov" : "mvn", s ? "s" : "",
                     rn[rd], rn[rm], shift);
        else
            snprintf(b, n, "%s%s.w %s, %s, %s%s", dp[op], s ? "s" : "", rn[rd], rn[rn_],
                     rn[rm], shift);
        return 1;
    }
    if ((h1 & 0xff80) == 0xfa00 && (h2 & 0xf0f0) == 0xf000) {     /* shift by reg */
        static const char *const sh[4] = { "lsl", "lsr", "asr", "ror" };
        snprintf(b, n, "%s%s.w %s, %s, %s", sh[(h1 >> 5) & 3], h1 & 0x10 ? "s" : "",
                 rn[(h2 >> 8) & 15], rn[rn_], rn[h2 & 15]);
        return 1;
    }
    if ((h1 & 0xfff0) == 0xfb90 && (h2 & 0xf0) == 0xf0) {
        snprintf(b, n, "sdiv %s, %s, %s", rn[(h2 >> 8) & 15], rn[rn_], rn[h2 & 15]);
        return 1;
    }
    if ((h1 & 0xfff0) == 0xfbb0 && (h2 & 0xf0) == 0xf0) {
        snprintf(b, n, "udiv %s, %s, %s", rn[(h2 >> 8) & 15], rn[rn_], rn[h2 & 15]);
        return 1;
    }
    if ((h1 & 0xfff0) == 0xfb00 && (h2 & 0xf0) == 0x10) {
        snprintf(b, n, "mls %s, %s, %s, %s", rn[(h2 >> 8) & 15], rn[rn_], rn[h2 & 15], rn[rt]);
        return 1;
    }
    if ((h1 & 0xfff0) == 0xfb00 && (h2 & 0xf0) == 0) {
        if (rt == 15)
            snprintf(b, n, "mul %s, %s, %s", rn[(h2 >> 8) & 15], rn[rn_], rn[h2 & 15]);
        else
            snprintf(b, n, "mla %s, %s, %s, %s", rn[(h2 >> 8) & 15], rn[rn_],
                     rn[h2 & 15], rn[rt]);
        return 1;
    }
    return 0;
}

int dis_thumb(u32 pc, u32 h1, u32 h2, char *b, size_t n)
{
    if ((h1 >> 11) >= 0x1d) {
        if (!thumb32(pc, h1, h2, b, n))
            snprintf(b, n, ".inst.w 0x%04x%04x", h1, h2);
        return 4;
    }
    if (!thumb16(pc, h1, b, n))
        snprintf(b, n, ".inst.n 0x%04x", h1);
    return 2;
}

/* ---- RISC-V --------------------------------------------------------------- */

static const char *const xn[32] = {
    "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2", "s0", "s1", "a0", "a1",
    "a2", "a3", "a4", "a5", "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7",
    "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6" };

static int rv32(u32 pc, u32 i, int xlen, char *b, size_t n)
{
    u32 op = i & 0x7f, rd = (i >> 7) & 31, f3 = (i >> 12) & 7;
    u32 r1 = (i >> 15) & 31, r2 = (i >> 20) & 31, f7 = i >> 25;
    s32 immi = (s32)i >> 20;
    s32 imms = (s32)((i >> 25) << 5 | ((i >> 7) & 31)) << 20 >> 20;
    switch (op) {
    case 0x03: {
        static const char *const ld[8] = { "lb", "lh", "lw", "ld", "lbu", "lhu", "lwu", 0 };
        if (!ld[f3])
            return 0;
        snprintf(b, n, "%s %s, %d(%s)", ld[f3], xn[rd], immi, xn[r1]);
        return 1;
    }
    case 0x23: {
        static const char *const st[4] = { "sb", "sh", "sw", "sd" };
        if (f3 > 3)
            return 0;
        snprintf(b, n, "%s %s, %d(%s)", st[f3], xn[r2], imms, xn[r1]);
        return 1;
    }
    case 0x13: case 0x1b: {
        static const char *const ar[8] = { "addi", "slli", "slti", "sltiu", "xori",
                                           "srli", "ori", "andi" };
        const char *m = ar[f3];
        int w = op == 0x1b;
        if (f3 == 5 && (i >> 30 & 1))
            m = "srai";
        if (f3 == 1 || f3 == 5)
            snprintf(b, n, "%s%s %s, %s, %u", m, w ? "w" : "", xn[rd], xn[r1],
                     (i >> 20) & (xlen == 64 && !w ? 63u : 31u));
        else
            snprintf(b, n, "%s%s %s, %s, %d", m, w ? "w" : "", xn[rd], xn[r1], immi);
        return 1;
    }
    case 0x33: case 0x3b: {
        static const char *const ar[8] = { "add", "sll", "slt", "sltu", "xor", "srl",
                                           "or", "and" };
        static const char *const md[8] = { "mul", "mulh", "mulhsu", "mulhu", "div",
                                           "divu", "rem", "remu" };
        const char *m = f7 == 1 ? md[f3] : ar[f3];
        if (f7 == 0x20 && f3 == 0)
            m = "sub";
        else if (f7 == 0x20 && f3 == 5)
            m = "sra";
        else if (f7 != 0 && f7 != 1)
            return 0;
        snprintf(b, n, "%s%s %s, %s, %s", m, op == 0x3b ? "w" : "", xn[rd], xn[r1], xn[r2]);
        return 1;
    }
    case 0x37: snprintf(b, n, "lui %s, 0x%x", xn[rd], i >> 12); return 1;
    case 0x17: snprintf(b, n, "auipc %s, 0x%x", xn[rd], i >> 12); return 1;
    case 0x6f: {
        s32 off = (s32)((i >> 31) << 20 | ((i >> 12) & 0xff) << 12 |
                        ((i >> 20) & 1) << 11 | ((i >> 21) & 0x3ff) << 1);
        off = (off << 11) >> 11;
        snprintf(b, n, "jal %s, 0x%08x", xn[rd], pc + (u32)off);
        return 1;
    }
    case 0x67:
        snprintf(b, n, "jalr %s, %d(%s)", xn[rd], immi, xn[r1]);
        return 1;
    case 0x63: {
        static const char *const br[8] = { "beq", "bne", 0, 0, "blt", "bge", "bltu", "bgeu" };
        s32 off = (s32)((i >> 31) << 12 | ((i >> 7) & 1) << 11 | ((i >> 25) & 0x3f) << 5 |
                        ((i >> 8) & 15) << 1);
        off = (off << 19) >> 19;
        if (!br[f3])
            return 0;
        snprintf(b, n, "%s %s, %s, 0x%08x", br[f3], xn[r1], xn[r2], pc + (u32)off);
        return 1;
    }
    case 0x73:
        if (i == 0x00000073) { snprintf(b, n, "ecall"); return 1; }
        if (i == 0x00100073) { snprintf(b, n, "ebreak"); return 1; }
        if (i == 0x30200073) { snprintf(b, n, "mret"); return 1; }
        if (i == 0x10500073) { snprintf(b, n, "wfi"); return 1; }
        if (f3 && f3 != 4) {
            static const char *const cs_[8] = { 0, "csrrw", "csrrs", "csrrc", 0,
                                                "csrrwi", "csrrsi", "csrrci" };
            if (f3 >= 5)
                snprintf(b, n, "%s %s, 0x%03x, %u", cs_[f3], xn[rd], i >> 20, r1);
            else
                snprintf(b, n, "%s %s, 0x%03x, %s", cs_[f3], xn[rd], i >> 20, xn[r1]);
            return 1;
        }
        return 0;
    case 0x2f: {
        static const char *const am[32] = {
            [0] = "amoadd", [1] = "amoswap", [2] = "lr", [3] = "sc", [4] = "amoxor",
            [8] = "amoor", [12] = "amoand", [16] = "amomin", [20] = "amomax",
            [24] = "amominu", [28] = "amomaxu" };
        const char *m = am[i >> 27];
        if (!m || (f3 != 2 && f3 != 3))
            return 0;
        snprintf(b, n, "%s.%c %s, %s, (%s)", m, f3 == 2 ? 'w' : 'd', xn[rd], xn[r2], xn[r1]);
        return 1;
    }
    case 0x0f:
        snprintf(b, n, "%s", f3 == 1 ? "fence.i" : "fence");
        return 1;
    case 0x07: case 0x27:
        snprintf(b, n, "%s f%u, %d(%s)", op == 0x07 ? (f3 == 2 ? "flw" : "fld") :
                 (f3 == 2 ? "fsw" : "fsd"), op == 0x07 ? rd : r2,
                 op == 0x07 ? immi : imms, xn[r1]);
        return 1;
    }
    return 0;
}

int dis_riscv(u32 pc, u32 insn, int xlen, char *b, size_t n)
{
    if ((insn & 3) != 3) {
        u32 x = rvc_expand(insn & 0xffff, xlen);
        char t[96];
        if (x && rv32(pc, x, xlen, t, sizeof t))
            snprintf(b, n, "%s  (compressed)", t);
        else
            snprintf(b, n, ".insn 0x%04x", insn & 0xffff);
        return 2;
    }
    if (!rv32(pc, insn, xlen, b, n))
        snprintf(b, n, ".insn 0x%08x", insn);
    return 4;
}

/* ---- MIPS32 ---------------------------------------------------------------- */

static const char *const mr[32] = {
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2",
    "t3", "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5", "s6",
    "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra" };

int dis_mips(u32 pc, u32 i, char *b, size_t n)
{
    u32 op = i >> 26, rs = (i >> 21) & 31, rt = (i >> 16) & 31;
    u32 rd = (i >> 11) & 31, sa = (i >> 6) & 31, fn = i & 0x3f;
    s32 imm = (s32)(int16_t)(i & 0xffff);
    u32 t = pc + 4 + (u32)(imm << 2);
    static const char *const ld[16] = {
        "lb", "lh", "lwl", "lw", "lbu", "lhu", "lwr", 0,
        "sb", "sh", "swl", "sw", 0, 0, "swr", "cache" };
    static const char *const alu[8] = {
        0, 0, "slti", "sltiu", "andi", "ori", "xori", "lui" };
    static const char *const rr[64] = {
        [0x20] = "add", [0x21] = "addu", [0x22] = "sub", [0x23] = "subu",
        [0x24] = "and", [0x25] = "or", [0x26] = "xor", [0x27] = "nor",
        [0x2a] = "slt", [0x2b] = "sltu", [0x0a] = "movz", [0x0b] = "movn" };
    static const char *const tr[8] = {
        "tge", "tgeu", "tlt", "tltu", "teq", 0, "tne", 0 };
    if (i == 0)
        return snprintf(b, n, "nop"), 4;
    switch (op) {
    case 0:
        if (rr[fn])
            return snprintf(b, n, "%s %s, %s, %s", rr[fn], mr[rd], mr[rs], mr[rt]), 4;
        if (fn >= 0x30 && fn <= 0x36 && tr[fn - 0x30])
            return snprintf(b, n, "%s %s, %s", tr[fn - 0x30], mr[rs], mr[rt]), 4;
        switch (fn) {
        case 0x00:
            if (!rd && sa == 3)
                return snprintf(b, n, "ehb"), 4;
            return snprintf(b, n, "sll %s, %s, %u", mr[rd], mr[rt], sa), 4;
        case 0x02: return snprintf(b, n, "%s %s, %s, %u", rs & 1 ? "rotr" : "srl",
                                   mr[rd], mr[rt], sa), 4;
        case 0x03: return snprintf(b, n, "sra %s, %s, %u", mr[rd], mr[rt], sa), 4;
        case 0x04: return snprintf(b, n, "sllv %s, %s, %s", mr[rd], mr[rt], mr[rs]), 4;
        case 0x06: return snprintf(b, n, "%s %s, %s, %s", sa & 1 ? "rotrv" : "srlv",
                                   mr[rd], mr[rt], mr[rs]), 4;
        case 0x07: return snprintf(b, n, "srav %s, %s, %s", mr[rd], mr[rt], mr[rs]), 4;
        case 0x08: return snprintf(b, n, "jr %s", mr[rs]), 4;
        case 0x09: return snprintf(b, n, "jalr %s, %s", mr[rd], mr[rs]), 4;
        case 0x0c: return snprintf(b, n, "syscall"), 4;
        case 0x0d: return snprintf(b, n, "break %u", (i >> 16) & 0x3ff), 4;
        case 0x0f: return snprintf(b, n, "sync"), 4;
        case 0x10: case 0x12:
            return snprintf(b, n, "%s %s, $ac%u", fn == 0x10 ? "mfhi" : "mflo",
                            mr[rd], (i >> 21) & 3), 4;
        case 0x11: case 0x13:
            return snprintf(b, n, "%s %s, $ac%u", fn == 0x11 ? "mthi" : "mtlo",
                            mr[rs], (i >> 11) & 3), 4;
        case 0x18: case 0x19: case 0x1a: case 0x1b: {
            static const char *const md[4] = { "mult", "multu", "div", "divu" };
            return snprintf(b, n, "%s %s, %s", md[fn - 0x18], mr[rs], mr[rt]), 4;
        }
        }
        break;
    case 1: {
        static const char *const ri[32] = {
            "bltz", "bgez", "bltzl", "bgezl", 0, 0, 0, 0, "tgei", "tgeiu",
            "tlti", "tltiu", "teqi", 0, "tnei", 0, "bltzal", "bgezal",
            "bltzall", "bgezall" };
        if (rt < 32 && ri[rt]) {
            if (rt >= 8 && rt <= 14)
                return snprintf(b, n, "%s %s, %d", ri[rt], mr[rs], imm), 4;
            return snprintf(b, n, "%s %s, 0x%08x", ri[rt], mr[rs], t), 4;
        }
        break;
    }
    case 2: case 3:
        return snprintf(b, n, "%s 0x%08x", op == 2 ? "j" : "jal",
                        ((pc + 4) & 0xf0000000u) | (i & 0x03ffffffu) << 2), 4;
    case 4: case 5: case 20: case 21: {
        static const char *const be[4] = { "beq", "bne", "beql", "bnel" };
        return snprintf(b, n, "%s %s, %s, 0x%08x", be[(op & 1) | (op >> 3 & 2)],
                        mr[rs], mr[rt], t), 4;
    }
    case 6: case 7: case 22: case 23: {
        static const char *const bz[4] = { "blez", "bgtz", "blezl", "bgtzl" };
        return snprintf(b, n, "%s %s, 0x%08x", bz[(op & 1) | (op >> 3 & 2)], mr[rs], t), 4;
    }
    case 8: case 9:
        return snprintf(b, n, "%s %s, %s, %d", op == 8 ? "addi" : "addiu",
                        mr[rt], mr[rs], imm), 4;
    case 16:
        if (rs == 0 || rs == 4)
            return snprintf(b, n, "%s %s, $%u, %u", rs ? "mtc0" : "mfc0", mr[rt],
                            rd, i & 7), 4;
        if (rs == 11)
            return snprintf(b, n, "%s %s", i & 0x20 ? "ei" : "di", mr[rt]), 4;
        if (rs == 10 || rs == 14)
            return snprintf(b, n, "%s %s, %s", rs == 10 ? "rdpgpr" : "wrpgpr",
                            mr[rd], mr[rt]), 4;
        switch (i) {
        case 0x42000018: return snprintf(b, n, "eret"), 4;
        case 0x4200001f: return snprintf(b, n, "deret"), 4;
        case 0x42000020: return snprintf(b, n, "wait"), 4;
        case 0x42000001: return snprintf(b, n, "tlbr"), 4;
        case 0x42000002: return snprintf(b, n, "tlbwi"), 4;
        case 0x42000006: return snprintf(b, n, "tlbwr"), 4;
        case 0x42000008: return snprintf(b, n, "tlbp"), 4;
        }
        break;
    case 28:
        switch (fn) {
        case 0x00: case 0x01: case 0x04: case 0x05: {
            static const char *const ma[6] = { "madd", "maddu", 0, 0, "msub", "msubu" };
            return snprintf(b, n, "%s $ac%u, %s, %s", ma[fn], (i >> 11) & 3,
                            mr[rs], mr[rt]), 4;
        }
        case 0x02: return snprintf(b, n, "mul %s, %s, %s", mr[rd], mr[rs], mr[rt]), 4;
        case 0x20: case 0x21:
            return snprintf(b, n, "%s %s, %s", fn == 0x20 ? "clz" : "clo", mr[rd], mr[rs]), 4;
        case 0x3f: return snprintf(b, n, "sdbbp %u", (i >> 6) & 0xfffff), 4;
        }
        break;
    case 31:
        switch (fn) {
        case 0x00: return snprintf(b, n, "ext %s, %s, %u, %u", mr[rt], mr[rs], sa, rd + 1), 4;
        case 0x04: return snprintf(b, n, "ins %s, %s, %u, %u", mr[rt], mr[rs], sa,
                                   rd - sa + 1), 4;
        case 0x20:
            if (sa == 0x02 || sa == 0x10 || sa == 0x18)
                return snprintf(b, n, "%s %s, %s", sa == 0x02 ? "wsbh" : sa == 0x10
                                ? "seb" : "seh", mr[rd], mr[rt]), 4;
            break;
        case 0x38:
            if (sa == 0x12)
                return snprintf(b, n, "rddsp %s, 0x%x", mr[rd], (i >> 16) & 0x3ff), 4;
            if (sa == 0x13)
                return snprintf(b, n, "wrdsp %s, 0x%x", mr[rs], (i >> 11) & 0x3ff), 4;
            break;
        case 0x3b: return snprintf(b, n, "rdhwr %s, $%u", mr[rt], rd), 4;
        }
        break;
    case 48: case 56:
        return snprintf(b, n, "%s %s, %d(%s)", op == 48 ? "ll" : "sc", mr[rt], imm,
                        mr[rs]), 4;
    case 51:
        return snprintf(b, n, "pref %u, %d(%s)", rt, imm, mr[rs]), 4;
    default:
        if (op == 15)
            return snprintf(b, n, "lui %s, 0x%x", mr[rt], i & 0xffff), 4;
        if (op >= 10 && op <= 14)
            return snprintf(b, n, "%s %s, %s, %d", alu[op - 8], mr[rt], mr[rs],
                            op >= 12 ? (int)(i & 0xffff) : imm), 4;
        if (op >= 32 && op <= 47 && ld[op - 32]) {
            if (op == 47)
                return snprintf(b, n, "cache 0x%x, %d(%s)", rt, imm, mr[rs]), 4;
            return snprintf(b, n, "%s %s, %d(%s)", ld[op - 32], mr[rt], imm, mr[rs]), 4;
        }
        break;
    }
    return snprintf(b, n, ".inst 0x%08x", i), 4;
}

/* ---- PIC24F -------------------------------------------------------------- */

/* the source (ppp) and destination (qqq) operand of mode m on Wr, Wb its
 * offset register for [Wr+Wb] */
static void p24_op(char *b, size_t n, int m, u32 r, u32 wb)
{
    switch (m) {
    case 0: snprintf(b, n, "w%u", r); return;
    case 1: snprintf(b, n, "[w%u]", r); return;
    case 2: snprintf(b, n, "[w%u--]", r); return;
    case 3: snprintf(b, n, "[w%u++]", r); return;
    case 4: snprintf(b, n, "[--w%u]", r); return;
    case 5: snprintf(b, n, "[++w%u]", r); return;
    default: snprintf(b, n, "[w%u+w%u]", r, wb); return;
    }
}

/* f's destination: WREG, or f itself */
static const char *p24_wreg(u32 i)
{
    return (i >> 13 & 1) ? "" : ", wreg";
}

/* a dsPIC's MAC class prefetch (iiii for X, jjjj for Y) into Wd: ", [w8]+=2,
 * w4"; nothing for 0100 (none) */
static int p24_prefetch(char *b, size_t n, u32 f, int y, u32 d)
{
    static const char *const step[8] = { "", "+=2", "+=4", "+=6", 0, "-=6", "-=4", "-=2" };
    u32 r = (y ? 10 : 8) + (f >> 3);
    if (f == 4)
        return 0;
    if (f == 12)
        return snprintf(b, n, ", [w%u+w12], w%u", r, d);
    return snprintf(b, n, ", [w%u]%s, w%u", r, step[f & 7], d);
}

/* the DSP instructions: the MAC class (0xC0-0xC7, 0xF0-0xF3) and the
 * accumulator's (0xC8-0xCD) */
static int dis_p24_dsp(u32 i, char *b, size_t n)
{
    static const int wm[8] = { 4, 4, 4, 0, 5, 5, 6, 0 }, wn[8] = { 5, 6, 7, 0, 6, 7, 7, 0 };
    static const char *const shift_dst[2] = { "a", "b" };
    u32 op = i >> 16 & 0xff, a = i >> 15 & 1;
    char x[24] = "", y[24] = "", wb[16] = "", s1[24];
    const char *acc = shift_dst[a];
    if (op <= 0xc7 || op >= 0xf0) {
        u32 aa = i & 3;
        p24_prefetch(x, sizeof x, i >> 6 & 15, 0, 4 + (i >> 12 & 3));
        if (op >= 0xf0) {                       /* the squares, ED, EDAC */
            u32 m = 4 + (op & 3);
            static const char *const sq[4] = { "mac", "mpy", "edac", "ed" };
            if (aa >= 2) {                      /* [Wx], [Wy], Wxd */
                char yy[24] = "";
                p24_prefetch(yy, sizeof yy, i >> 2 & 15, 1, 0);
                char *comma = strrchr(yy, ',');
                if (comma)
                    *comma = 0;
                char *xc = strrchr(x, ',');
                char xd[8] = "";
                if (xc) {
                    snprintf(xd, sizeof xd, "%s", xc);
                    *xc = 0;
                }
                return snprintf(b, n, "%s w%u*w%u, %s%s%s%s", sq[aa], m, m, acc, x, yy, xd);
            }
            p24_prefetch(y, sizeof y, i >> 2 & 15, 1, 4 + (i >> 10 & 3));
            return snprintf(b, n, "%s w%u*w%u, %s%s%s", sq[aa], m, m, acc, x, y);
        }
        p24_prefetch(y, sizeof y, i >> 2 & 15, 1, 4 + (i >> 10 & 3));
        if (aa == 0)
            snprintf(wb, sizeof wb, ", w13");
        else if (aa == 1)
            snprintf(wb, sizeof wb, ", [w13]+=2");
        u32 mmm = op & 7, neg = i >> 14 & 1;
        if (mmm == 3)
            return snprintf(b, n, "clr %s%s%s%s", acc, x, y, wb);
        if (mmm == 7)
            return snprintf(b, n, "movsac %s%s%s%s", acc, x, y, wb);
        snprintf(s1, sizeof s1, "w%d*w%d", wm[mmm], wn[mmm]);
        if (aa == 3)
            return snprintf(b, n, "%s %s, %s%s%s", neg ? "mpy.n" : "mpy", s1, acc, x, y);
        return snprintf(b, n, "%s %s, %s%s%s%s", neg ? "msc" : "mac", s1, acc, x, y, wb);
    }
    int k = (int)(i >> 7 & 15);
    k = (k ^ 8) - 8;
    switch (op) {
    case 0xc8:
        if (i & 0x40) {
            int v = (int)(i & 0x3f);
            return snprintf(b, n, "sftac %s, #%d", acc, (v ^ 0x20) - 0x20);
        }
        return snprintf(b, n, "sftac %s, w%u", acc, (unsigned)(i & 15));
    case 0xc9: case 0xca:
        p24_op(s1, sizeof s1, (int)(i >> 4 & 7), i & 15, i >> 11 & 15);
        if (k)
            return snprintf(b, n, "%s %s, #%d, %s", op == 0xc9 ? "add" : "lac", s1, k, acc);
        return snprintf(b, n, "%s %s, %s", op == 0xc9 ? "add" : "lac", s1, acc);
    case 0xcb: {
        static const char *const ab[8] = { "add", "neg", 0, "sub" };
        const char *nm = ab[i >> 12 & 7];
        if (nm && !(i & 0x0fff))
            return snprintf(b, n, "%s %s", nm, acc);
        break;
    }
    case 0xcc: case 0xcd:
        p24_op(s1, sizeof s1, (int)(i >> 4 & 7), i & 15, i >> 11 & 15);
        if (k)
            return snprintf(b, n, "%s %s, #%d, %s", op == 0xcc ? "sac" : "sac.r", acc, k, s1);
        return snprintf(b, n, "%s %s, %s", op == 0xcc ? "sac" : "sac.r", acc, s1);
    }
    return snprintf(b, n, ".word 0x%06x", (unsigned)i);
}

int dis_pic24(u32 pc, u32 i, u32 i2, char *b, size_t n)
{
    static const char *const cc[16] = {
        "ov", "c", "z", "n", "le", "lt", "leu", "", "nov", "nc", "nz", "nn",
        "gt", "ge", "gtu", 0 };
    static const char *const alu2[8] = {
        "add", "addc", "sub", "subb", "and", "xor", "ior", 0 };
    static const char *const sh[16] = {
        "sl", 0, "lsr", "asr", "rlnc", "rlc", "rrnc", "rrc",
        "inc", "inc2", "dec", "dec2", "neg", "com", "clr", "setm" };
    u32 op = i >> 16 & 0xff, B = i >> 14 & 1;
    const char *bs = B ? ".b" : "";
    u32 wb = i >> 15 & 15, s = i & 15, d = i >> 7 & 15;
    int pm = (int)(i >> 4 & 7), qm = (int)(i >> 11 & 7);
    u32 f13 = i & 0x1fff;
    s32 rel = (s32)(int16_t)(i & 0xffff);
    char x[24], y[24];
    i &= 0xffffff;
    if (i == 0)
        return snprintf(b, n, "nop"), 1;
    if (i == 0xffffff)
        return snprintf(b, n, "nopr"), 1;
    switch (op) {
    case 0x01: {
        static const char *const w[4] = { "call", "rcall", "goto", "bra" };
        if ((i & 0x9ff0) == 0)
            return snprintf(b, n, "%s w%u", w[i >> 13 & 3], s), 1;
        if ((i & 0xf9f0) == 0)                  /* the PIC24E's */
            return snprintf(b, n, "%s w%u", w[i >> 9 & 3], s), 1;
        if ((i & 0x83f0) == 0x8000)
            return snprintf(b, n, "%s w%u", i & 0x400 ? "goto.l" : "call.l", s), 1;
        break;
    }
    case 0x02: case 0x04:
        return snprintf(b, n, "%s 0x%06x", op == 2 ? "call" : "goto",
                        (unsigned)((i & 0xfffe) | (i2 & 0x7f) << 16)), 2;
    case 0x05:
        return snprintf(b, n, "retlw%s #%u, w%u", bs, (unsigned)(i >> 4 & 0x3ff), s), 1;
    case 0x06:
        if (i == 0x060000)
            return snprintf(b, n, "return"), 1;
        if (i == 0x064000)
            return snprintf(b, n, "retfie"), 1;
        break;
    case 0x07:
        return snprintf(b, n, "rcall 0x%06x", (unsigned)(pc + 2 + 2 * (u32)rel)), 1;
    case 0x08:                                  /* a dsPIC's DO */
        if (i & 0x8000)
            snprintf(x, sizeof x, "w%u", s);
        else
            snprintf(x, sizeof x, "#%u", (unsigned)(i & 0x7fff));
        return snprintf(b, n, "do %s, 0x%06x", x,
                        (unsigned)((pc + 4 + 2 * (u32)(s32)(int16_t)(i2 & 0xffff)) & 0x7ffffe)), 2;
    case 0x09:
        if (i & 0x8000)
            return snprintf(b, n, "repeat w%u", s), 1;
        return snprintf(b, n, "repeat #%u", (unsigned)(i & 0x7fff)), 1;
    case 0x0c: case 0x0d: case 0x0e: case 0x0f: {
        static const char *const dc[4] = { "oa", "ob", "sa", "sb" };
        return snprintf(b, n, "bra %s, 0x%06x", dc[op - 0x0c],
                        (unsigned)(pc + 2 + 2 * (u32)rel)), 1;
    }
    case 0x80: case 0x81: case 0x82: case 0x83:
    case 0x84: case 0x85: case 0x86: case 0x87:
        return snprintf(b, n, "mov 0x%04x, w%u", (unsigned)(i >> 4 & 0x7fff) << 1, s), 1;
    case 0x88: case 0x89: case 0x8a: case 0x8b:
    case 0x8c: case 0x8d: case 0x8e: case 0x8f:
        return snprintf(b, n, "mov w%u, 0x%04x", s, (unsigned)(i >> 4 & 0x7fff) << 1), 1;
    case 0x90: case 0x91: case 0x92: case 0x93:
    case 0x94: case 0x95: case 0x96: case 0x97:
    case 0x98: case 0x99: case 0x9a: case 0x9b:
    case 0x9c: case 0x9d: case 0x9e: case 0x9f: {
        u32 k = wb << 6 | (u32)qm << 3 | (u32)pm;
        s32 off = (s32)((k ^ 0x200) - 0x200) * (B ? 1 : 2);
        if (op < 0x98)
            return snprintf(b, n, "mov%s [w%u%+d], w%u", bs, s, (int)off, d), 1;
        return snprintf(b, n, "mov%s w%u, [w%u%+d]", bs, s, d, (int)off), 1;
    }
    case 0xa0: case 0xa1: case 0xa2: case 0xa3: case 0xa4:
    case 0xa6: case 0xa7: {
        static const char *const bw[8] = { "bset", "bclr", "btg", "btst",
                                           "btsts", 0, "btss", "btsc" };
        p24_op(x, sizeof x, pm, s, 0);
        const char *z = (op == 0xa3 || op == 0xa4) ? (i & 0x800 ? ".z" : ".c") : "";
        return snprintf(b, n, "%s%s%s %s, #%u", bw[op - 0xa0],
                        op <= 0xa2 && (i & 0x400) ? ".b" : "", z, x,
                        (unsigned)(i >> 12 & 15)), 1;
    }
    case 0xa5: case 0xad:
        p24_op(x, sizeof x, pm, s, 0);
        return snprintf(b, n, "%s%s %s, w%u", op == 0xa5 ? "btst" : "bsw",
                        i & 0x8000 ? ".z" : ".c", x, (unsigned)(i >> 11 & 15)), 1;
    case 0xa8: case 0xa9: case 0xaa: case 0xab:
    case 0xac: case 0xae: case 0xaf: {
        static const char *const bf[8] = { "bset", "bclr", "btg", "btst",
                                           "btsts", 0, "btss", "btsc" };
        return snprintf(b, n, "%s 0x%04x, #%u", bf[op - 0xa8], (unsigned)(i & 0x1ffe),
                        (unsigned)((i >> 13 & 7) | (i & 1) << 3)), 1;
    }
    case 0xb0: case 0xb1: case 0xb2:
        return snprintf(b, n, "%s%s #%u, w%u", alu2[(op - 0xb0) * 2 + (i >> 15 & 1)], bs,
                        (unsigned)(i >> 4 & 0x3ff), s), 1;
    case 0xb3:
        if (!(i & 0x8000))
            return snprintf(b, n, "ior%s #%u, w%u", bs, (unsigned)(i >> 4 & 0x3ff), s), 1;
        if ((i >> 12 & 15) == 0xc)
            return snprintf(b, n, "mov.b #%u, w%u", (unsigned)(i >> 4 & 0xff), s), 1;
        break;
    case 0xb4: case 0xb5: case 0xb6:
        return snprintf(b, n, "%s%s 0x%04x%s", alu2[(op - 0xb4) * 2 + (i >> 15 & 1)], bs,
                        (unsigned)f13, p24_wreg(i)), 1;
    case 0xb7:
        if (!(i & 0x8000))
            return snprintf(b, n, "ior%s 0x%04x%s", bs, (unsigned)f13, p24_wreg(i)), 1;
        if (i & 0x2000)
            return snprintf(b, n, "mov%s wreg, 0x%04x", bs, (unsigned)f13), 1;
        break;
    case 0xb8: case 0xb9: {
        static const char *const m[4] = { "uu", "us", "su", "ss" };
        const char *v = m[(op - 0xb8) * 2 + (i >> 15 & 1)];
        if ((pm & 6) == 6)
            snprintf(x, sizeof x, "#%u", s | (u32)(pm & 1) << 4);
        else
            p24_op(x, sizeof x, pm, s, 0);
        if (d >= 14)                            /* a dsPIC33E's accumulator */
            return snprintf(b, n, "mul.%s w%u, %s, %s", v, (unsigned)(i >> 11 & 15), x,
                            d == 14 ? "a" : "b"), 1;
        if (d & 1)                              /* the PIC24E's MULW */
            return snprintf(b, n, "mulw.%s w%u, %s, w%u", v, (unsigned)(i >> 11 & 15), x,
                            d - 1), 1;
        return snprintf(b, n, "mul.%s w%u, %s, w%u", v, (unsigned)(i >> 11 & 15), x, d), 1;
    }
    case 0xba: case 0xbb:
        p24_op(x, sizeof x, pm, s, 0);
        p24_op(y, sizeof y, qm, d, 0);
        return snprintf(b, n, "%s%s%s %s, %s", op == 0xba ? "tblrd" : "tblwt",
                        i & 0x8000 ? "h" : "l", bs, x, y), 1;
    case 0xbc:
        return snprintf(b, n, "mul%s 0x%04x", bs, (unsigned)f13), 1;
    case 0xbd:
        return snprintf(b, n, "%s%s 0x%04x%s", i & 0x8000 ? "subbr" : "subr", bs,
                        (unsigned)f13, p24_wreg(i)), 1;
    case 0xbe:
        if (!(i & 0x8000)) {
            if (pm == 4 && s == 15)
                return snprintf(b, n, "pop.d w%u", (unsigned)(i >> 8 & 7) * 2), 1;
            p24_op(x, sizeof x, pm, s, 0);
            return snprintf(b, n, "mov.d %s, w%u", x, (unsigned)(i >> 8 & 7) * 2), 1;
        }
        if (qm == 3 && d == 15)
            return snprintf(b, n, "push.d w%u", (unsigned)(i >> 1 & 7) * 2), 1;
        p24_op(y, sizeof y, qm, d, 0);
        return snprintf(b, n, "mov.d w%u, %s", (unsigned)(i >> 1 & 7) * 2, y), 1;
    case 0xbf:
        return snprintf(b, n, "mov%s 0x%04x%s", bs, (unsigned)f13, p24_wreg(i)), 1;
    case 0xc0: case 0xc1: case 0xc2: case 0xc3: case 0xc4: case 0xc5:
    case 0xc6: case 0xc7: case 0xc8: case 0xc9: case 0xca: case 0xcb:
    case 0xcc: case 0xcd: case 0xf0: case 0xf1: case 0xf2: case 0xf3:
        return dis_p24_dsp(i, b, n), 1;
    case 0xd9:
        if (!(i & 0x8780))
            return snprintf(b, n, "divf w%u, w%u", (unsigned)(i >> 11 & 15), s), 1;
        break;
    case 0xcf:
        p24_op(x, sizeof x, pm, s, 0);
        return snprintf(b, n, "%s %s, w%u", i & 0x8000 ? "ff1l" : "ff1r", x, d), 1;
    case 0xd0: case 0xd1: case 0xd2: case 0xd3:
    case 0xe8: case 0xe9: case 0xea: {
        u32 k = op >= 0xe8 ? 8 + (op - 0xe8) * 2 : (op - 0xd0) * 2;
        const char *nm = sh[k + (i >> 15 & 1)];
        if (!nm)
            break;
        p24_op(x, sizeof x, pm, s, 0);
        p24_op(y, sizeof y, qm, d, 0);
        return snprintf(b, n, "%s%s %s, %s", nm, bs, x, y), 1;
    }
    case 0xd4: case 0xd5: case 0xd6: case 0xd7:
    case 0xec: case 0xed: case 0xee: {
        u32 k = op >= 0xec ? 8 + (op - 0xec) * 2 : (op - 0xd4) * 2;
        const char *nm = sh[k + (i >> 15 & 1)];
        if (!nm)
            break;
        return snprintf(b, n, "%s%s 0x%04x%s", nm, bs, (unsigned)f13, p24_wreg(i)), 1;
    }
    case 0xeb:
        p24_op(y, sizeof y, qm, d, 0);
        return snprintf(b, n, "%s%s %s", i & 0x8000 ? "setm" : "clr", bs, y), 1;
    case 0xef:
        if (!f13 && !(i & 0x2000))
            return snprintf(b, n, "%s%s wreg", i & 0x8000 ? "setm" : "clr", bs), 1;
        return snprintf(b, n, "%s%s 0x%04x%s", i & 0x8000 ? "setm" : "clr", bs,
                        (unsigned)f13, p24_wreg(i)), 1;
    case 0xd8:
        return snprintf(b, n, "div.%s%s w%u, w%u", i & 0x8000 ? "u" : "s",
                        i & 0x40 ? "d" : "", (unsigned)(i >> 7 & 15), s), 1;
    case 0xdd: case 0xde:
        if (i & 0x40)
            snprintf(x, sizeof x, "#%u", s);
        else
            snprintf(x, sizeof x, "w%u", s);
        return snprintf(b, n, "%s w%u, %s, w%u",
                        op == 0xdd ? "sl" : i & 0x8000 ? "asr" : "lsr",
                        (unsigned)(i >> 11 & 15), x, d), 1;
    case 0xdf:
        p24_op(x, sizeof x, pm, s, 0);
        return snprintf(b, n, "fbcl %s, w%u", x, d), 1;
    case 0xe0:
        p24_op(x, sizeof x, pm, s, 0);
        return snprintf(b, n, "cp0%s %s", i & 0x400 ? ".b" : "", x), 1;
    case 0xe1:
        if ((pm & 6) == 6)                      /* the PIC24E's 8 bits: 9:7 */
            snprintf(x, sizeof x, "#%u", (unsigned)((i >> 7 & 7) << 5 | (i & 31)));
        else
            p24_op(x, sizeof x, pm, s, 0);
        return snprintf(b, n, "%s%s w%u, %s", i & 0x8000 ? "cpb" : "cp",
                        i & 0x400 ? ".b" : "", (unsigned)(i >> 11 & 15), x), 1;
    case 0xe2:
        return snprintf(b, n, "cp0%s 0x%04x", bs, (unsigned)f13), 1;
    case 0xe3:
        return snprintf(b, n, "%s%s 0x%04x", i & 0x8000 ? "cpb" : "cp", bs,
                        (unsigned)f13), 1;
    case 0xe6: case 0xe7: {
        static const char *const c[4] = { "cpsgt", "cpslt", "cpsne", "cpseq" };
        static const char *const cb[4] = { "cpbgt", "cpblt", "cpbne", "cpbeq" };
        int k = (op - 0xe6) * 2 + (int)(i >> 15 & 1);
        s32 off = (s32)((i >> 4 & 63) ^ 32) - 32;
        if (off > 1 || off < 0)                 /* the PIC24E's compare and branch */
            return snprintf(b, n, "%s%s w%u, w%u, 0x%06x", cb[k], i & 0x400 ? ".b" : "",
                            (unsigned)(i >> 11 & 15), s, (unsigned)(pc + 2 + 2 * (u32)off)), 1;
        return snprintf(b, n, "%s%s w%u, w%u", c[k],
                        i & 0x400 ? ".b" : "", (unsigned)(i >> 11 & 15), s), 1;
    }
    case 0xf8: return snprintf(b, n, "push 0x%04x", (unsigned)(i & 0xfffe)), 1;
    case 0xf9: return snprintf(b, n, "pop 0x%04x", (unsigned)(i & 0xfffe)), 1;
    case 0xfa:
        if (i == 0xfa8000)
            return snprintf(b, n, "ulnk"), 1;
        return snprintf(b, n, "lnk #%u", (unsigned)(i & 0x3ffe)), 1;
    case 0xfb:
        p24_op(x, sizeof x, pm, s, 0);
        return snprintf(b, n, "%s %s, w%u", i & 0x8000 ? "ze" : "se", x, d), 1;
    case 0xda:
        if (i == 0xda4000)
            return snprintf(b, n, "break"), 1;
        break;
    case 0xfc:
        return snprintf(b, n, "disi #%u", (unsigned)(i & 0x3fff)), 1;
    case 0xfd:
        if ((i & 0xf870) == 0)
            return snprintf(b, n, "exch w%u, w%u", s, d), 1;
        if ((i & 0xfff0) == 0x4000)
            return snprintf(b, n, "daw.b w%u", s), 1;
        if ((i & 0xbff0) == 0x8000)
            return snprintf(b, n, "swap%s w%u", B ? ".b" : "", s), 1;
        break;
    case 0xfe:
        if ((i & 0xe000) == 0xc000 && (i >> 10 & 3) != 3) {    /* MOVPAG */
            static const char *const pg[3] = { "dsrpag", "dswpag", "tblpag" };
            if (i & 0x1000)
                return snprintf(b, n, "movpag w%u, %s", s, pg[i >> 10 & 3]), 1;
            return snprintf(b, n, "movpag #0x%x, %s", (unsigned)(i & 0x3ff), pg[i >> 10 & 3]), 1;
        }
        switch (i & 0xffff) {
        case 0x0000: return snprintf(b, n, "reset"), 1;
        case 0x4000: case 0x4001:
            return snprintf(b, n, "pwrsav #%u", (unsigned)(i & 1)), 1;
        case 0x6000: return snprintf(b, n, "clrwdt"), 1;
        case 0x8000: return snprintf(b, n, "pop.s"), 1;
        case 0xa000: return snprintf(b, n, "push.s"), 1;
        }
        break;
    }
    if (op >= 0x20 && op <= 0x2f)
        return snprintf(b, n, "mov #0x%04x, w%u", (unsigned)(i >> 4 & 0xffff), s), 1;
    if (op >= 0x30 && op <= 0x3e) {
        u32 t = pc + 2 + 2 * (u32)rel;
        if (op == 0x37)
            return snprintf(b, n, "bra 0x%06x", (unsigned)t), 1;
        return snprintf(b, n, "bra %s, 0x%06x", cc[op & 15], (unsigned)t), 1;
    }
    if ((op >= 0x10 && op <= 0x1f) || (op >= 0x40 && op <= 0x77)) {
        static const char *const w3[16] = { 0, 0, "subr", "subbr", 0, 0, 0, 0,
                                            "add", "addc", "sub", "subb", "and",
                                            "xor", "ior", 0 };
        const char *nm = w3[op >> 3];
        if (nm) {
            if ((pm & 6) == 6)
                snprintf(x, sizeof x, "#%u", (unsigned)(i & 31));
            else
                p24_op(x, sizeof x, pm, s, 0);
            p24_op(y, sizeof y, qm, d, 0);
            return snprintf(b, n, "%s%s w%u, %s, %s", nm, bs, wb, x, y), 1;
        }
    }
    if (op >= 0x78 && op <= 0x7f) {
        p24_op(x, sizeof x, pm, s, wb);
        p24_op(y, sizeof y, qm, d, wb);
        if (!B && pm == 4 && s == 15)                   /* pop: mov [--w15] */
            return snprintf(b, n, "pop %s", y), 1;
        if (!B && qm == 3 && d == 15)                   /* push: mov to [w15++] */
            return snprintf(b, n, "push %s", x), 1;
        return snprintf(b, n, "mov%s %s, %s", bs, x, y), 1;
    }
    return snprintf(b, n, ".word 0x%06x", (unsigned)i), 1;
}

/* ---- --disassemble ------------------------------------------------------ */

static int rd(struct sim *s, u32 a, int n, u32 *v)
{
    if (s->cpu->ops->phys)
        a = s->cpu->ops->phys(s->cpu, a);
    return bus_debug_read(&s->bus, a, n, v) == 0;
}

/* one instruction at pc: its size in the image's addresses, 0 at the end
 * of memory */
static u32 dis_one(struct sim *s, u32 pc, FILE *o)
{
    char t[128];
    u32 h1, h2 = 0;
    if (pic24_is(s->cpu)) {
        u32 i = pic24_prog(s, pc), i2 = pic24_prog(s, pc + 2);
        if (dis_pic24(pc, i, i2, t, sizeof t) == 2) {
            fprintf(o, "  %06x:  %06x %06x  %s\n", pc, i, i2, t);
            return 4;
        }
        fprintf(o, "  %06x:  %06x         %s\n", pc, i, t);
        return 2;
    }
    if (!rd(s, pc, 2, &h1))
        return 0;
    rd(s, pc + 2, 2, &h2);
    if (mips_is(s->cpu)) {
        dis_mips(pc, h1 | h2 << 16, t, sizeof t);
        fprintf(o, "  %08x:  %08x  %s\n", pc, h1 | h2 << 16, t);
        return 4;
    }
    if (riscv_is(s->cpu)) {
        int xlen = ((struct rv_state *)s->cpu)->xlen;
        if ((h1 & 3) == 3) {
            dis_riscv(pc, h1 | h2 << 16, xlen, t, sizeof t);
            fprintf(o, "  %08x:  %08x  %s\n", pc, h1 | h2 << 16, t);
            return 4;
        }
        dis_riscv(pc, h1, xlen, t, sizeof t);
        fprintf(o, "  %08x:  %04x      %s\n", pc, h1, t);
        return 2;
    }
    int sz = dis_thumb(pc, h1, h2, t, sizeof t);
    if (sz == 4)
        fprintf(o, "  %08x:  %04x %04x  %s\n", pc, h1, h2, t);
    else
        fprintf(o, "  %08x:  %04x       %s\n", pc, h1, t);
    return (u32)sz;
}

void dis_listing(struct sim *s, const struct image *m, FILE *o)
{
    if (avr_is(s->cpu))
        die("--disassemble knows the Thumb, RISC-V, MIPS32 and PIC24 "
            "instruction sets, not the AVR's");
    if (m->ncode) {
        /* the segments in address order, each from where the last ended
         * (XC16's overlap) */
        u32 lo[IMG_CODE], hi[IMG_CODE], done = 0;
        int nc = m->ncode;
        for (int c = 0; c < nc; c++) {
            int k = c;
            while (k > 0 && lo[k - 1] > m->code_lo[c]) {
                lo[k] = lo[k - 1];
                hi[k] = hi[k - 1];
                k--;
            }
            lo[k] = m->code_lo[c];
            hi[k] = m->code_hi[c];
        }
        for (int c = 0; c < nc; c++) {
            u32 pc = lo[c] > done || !c ? lo[c] : done;
            while (pc < hi[c]) {
                int f = image_fn(m, pc);
                if (f >= 0 && m->fn[f].addr == pc)
                    fprintf(o, "\n%s:\n", m->fn[f].name);
                u32 sz = dis_one(s, pc, o);
                if (!sz)
                    break;
                pc += sz;
            }
            if (hi[c] > done)
                done = hi[c];
        }
        return;
    }
    /* a HEX image: its program memory where it is not erased (a PIC24),
     * or not zero */
    if (!pic24_is(s->cpu))
        die("--disassemble of a HEX image: only a PIC24's (give the ELF file)");
    u32 end = ((struct pic24_state *)s->cpu)->prog_end;
    for (u32 pc = 0; pc < end;) {
        if (pic24_prog(s, pc) == 0xffffff) {
            pc += 2;
            continue;
        }
        fprintf(o, "\n");
        while (pc < end && pic24_prog(s, pc) != 0xffffff)
            pc += dis_one(s, pc, o);
    }
}
