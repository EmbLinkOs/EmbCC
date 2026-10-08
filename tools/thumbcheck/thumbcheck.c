/* Emits one instruction per call through src/arch/thumb/emit.c and writes
 * the raw bytes to stdout, alongside the disassembly EACH ONE IS SUPPOSED
 * TO BE on stderr. tests/golden/thumb-encoding.sh disassembles the bytes
 * with llvm-objdump and diffs the two.
 *
 * This is the only defence against a wrong bit in an encoding: a backend
 * that assembles its own instructions has no assembler to catch it. On
 * Thumb it also catches a second class of mistake the other targets
 * cannot make -- emitting a two-byte form where the operands do not fit
 * it, which disassembles as a different instruction rather than as
 * nonsense.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/arch/thumb/emit.h"

static struct code C;
static int mark;

/* `text` is the expected disassembly; '|' separates one call's several
 * instructions. */
static void expect(const char *text)
{
    const char *p = text;
    for (;;) {
        const char *bar = strchr(p, '|');
        int n = bar ? (int)(bar - p) : (int)strlen(p);
        fprintf(stderr, "%.*s\n", n, p);
        if (!bar) break;
        p = bar + 1;
    }
    mark = C.len;
}

/* Every value the modified-immediate encoding can hold, emitted through
 * t_mov_imm and read back. t_imm_ok() decides by SEARCHING that space, so
 * a wrong bit in expand_imm() would not make the search fail -- it would
 * make it accept a value and emit a field that means a different one,
 * which only a disassembler notices. aarch64's bitmask encoder was caught
 * exactly this way, with its rotation the inverse of what it should be.
 *
 * Deduplicated, because several of the 4096 fields expand to the same
 * value (#0 alone has four spellings) and the point is one check per
 * value the compiler can be asked for. */
static int sweep_immediates(void)
{
    static unsigned char seen[1 << 16];
    unsigned long *vals = malloc(4096 * sizeof *vals);
    int n = 0;
    for (unsigned e = 0; e < 4096; e++) {
        unsigned long v;
        /* Reconstruct the value this field denotes, by the architecture's
         * rule rather than by asking the encoder -- otherwise the check
         * would be the encoder agreeing with itself. */
        unsigned imm8 = e & 0xff;
        if ((e >> 10) == 0) {
            switch ((e >> 8) & 3) {
            case 0:  v = imm8; break;
            case 1:  v = ((unsigned long)imm8 << 16) | imm8; break;
            case 2:  v = ((unsigned long)imm8 << 24) |
                         ((unsigned long)imm8 << 8); break;
            default: v = ((unsigned long)imm8 << 24) |
                         ((unsigned long)imm8 << 16) |
                         ((unsigned long)imm8 << 8) | imm8; break;
            }
        } else {
            unsigned u = 0x80u | (imm8 & 0x7fu), rot = e >> 7;
            v = ((unsigned long)u >> rot) |
                (((unsigned long)u << (32 - rot)) & 0xffffffffUL);
        }
        int dup = 0;
        for (int k = 0; k < n; k++)
            if (vals[k] == v) { dup = 1; break; }
        if (dup) continue;
        vals[n++] = v;
    }
    (void)seen;
    for (int k = 0; k < n; k++) {
        char want[64];
        int before = C.len;
        t_mov_imm(&C, 0, (long)vals[k], 0);
        if (C.len - before == 4 && (vals[k] >> 16) == 0)
            sprintf(want, "movw\tr0, #%lu", vals[k]);
        else if (C.len - before == 4)
            sprintf(want, "mov.w\tr0, #%lu", vals[k]);
        else
            sprintf(want, "movw\tr0, #%lu|movt\tr0, #%lu",
                    vals[k] & 0xffff, vals[k] >> 16);
        expect(want);
    }
    free(vals);
    return n;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--immediates") == 0) {
        int n = sweep_immediates();
        fprintf(stdout, "");
        fwrite(C.p, 1, (size_t)C.len, stdout);
        fprintf(stderr, "");
        (void)n;
        return 0;
    }
    (void)argc; (void)argv;
    /* Branches first, so their patched targets are small fixed addresses
     * that do not move as instructions are appended below. The
     * displacement, not just the opcode, is what is being checked --
     * and on Thumb the displacement is the part most likely to be
     * wrong, because J1/J2 are stored inverted against the sign bit. */
    {
        int b = t_b(&C);           t_patch_b(&C, b, b + 8);
        expect("b.w\t0x8");
        int bc = t_bcond(&C, T_NE); t_patch_bcond(&C, bc, bc + 8);
        expect("bne.w\t0xc");
        int bl = t_bl(&C);         t_patch_bl(&C, bl, bl - 8);
        expect("bl\t0x0");
        int bb = t_b(&C);          t_patch_b(&C, bb, bb + 0x100000);
        expect("b.w\t0x10000c");
        int bn = t_b(&C);          t_patch_b(&C, bn, bn - 0x100000);
        expect("b.w\t0xfff00010");
        int bq = t_bcond(&C, T_LT); t_patch_bcond(&C, bq, bq - 0x40000);
        expect("blt.w\t0xfffc0014");
    }

    t_mov_reg(&C, 0, 1);                expect("mov\tr0, r1");
    t_mov_reg(&C, 8, 1);                expect("mov\tr8, r1");
    t_mov_reg(&C, 1, 8);                expect("mov\tr1, r8");
    t_mov_imm(&C, 0, 7, 1);             expect("movs\tr0, #7");
    t_mov_imm(&C, 0, 255, 1);           expect("movs\tr0, #255");
    t_mov_imm(&C, 0, 7, 0);             expect("movw\tr0, #7");
    t_mov_imm(&C, 0, 0x1234, 0);        expect("movw\tr0, #4660");
    t_mov_imm(&C, 9, 0xffff, 0);        expect("movw\tr9, #65535");
    t_mov_imm(&C, 0, 0x12345678L, 0);   expect("movw\tr0, #22136|movt\tr0, #4660");
    t_mov_imm(&C, 3, 0x00ff00ffL, 0);   expect("mov.w\tr3, #16711935");
    t_mov_imm(&C, 3, -1, 0);            expect("mov.w\tr3, #4294967295");
    t_mov_addr(&C, 2, 0xdeadbeefUL);    expect("movw\tr2, #48879|movt\tr2, #57005");
    t_mvn_reg(&C, 0, 1, 0);             expect("mvn.w\tr0, r1");
    t_mvn_reg(&C, 0, 1, 1);             expect("mvns\tr0, r1");

    t_alu_reg(&C, T_OP_ADD, 0, 1, 2, 0); expect("add.w\tr0, r1, r2");
    t_alu_reg(&C, T_OP_ADD, 0, 1, 2, 1); expect("adds\tr0, r1, r2");
    t_alu_reg(&C, T_OP_SUB, 0, 1, 2, 1); expect("subs\tr0, r1, r2");
    t_alu_reg(&C, T_OP_SUB, 0, 8, 2, 0); expect("sub.w\tr0, r8, r2");
    t_alu_reg(&C, T_OP_AND, 0, 0, 1, 1); expect("ands\tr0, r1");
    t_alu_reg(&C, T_OP_AND, 0, 1, 2, 0); expect("and.w\tr0, r1, r2");
    t_alu_reg(&C, T_OP_ORR, 0, 0, 1, 1); expect("orrs\tr0, r1");
    t_alu_reg(&C, T_OP_EOR, 0, 0, 1, 1); expect("eors\tr0, r1");
    t_alu_reg(&C, T_OP_BIC, 0, 0, 1, 1); expect("bics\tr0, r1");
    t_alu_reg(&C, T_OP_ORN, 0, 1, 2, 0); expect("orn\tr0, r1, r2");
    t_alu_reg(&C, T_OP_ADC, 0, 0, 1, 1); expect("adcs\tr0, r1");
    t_alu_reg(&C, T_OP_SBC, 0, 0, 1, 1); expect("sbcs\tr0, r1");
    /* the shifted second operand: every type, both halves of the amount */
    t_alu_reg_shift(&C, T_OP_ADD, 0, 1, 2, T_SH_LSL, 2, 0);  expect("add.w\tr0, r1, r2, lsl #2");
    t_alu_reg_shift(&C, T_OP_ADD, 0, 1, 2, T_SH_LSL, 2, 1);  expect("adds.w\tr0, r1, r2, lsl #2");
    t_alu_reg_shift(&C, T_OP_SUB, 0, 1, 2, T_SH_LSR, 5, 0);  expect("sub.w\tr0, r1, r2, lsr #5");
    t_alu_reg_shift(&C, T_OP_RSB, 0, 1, 1, T_SH_LSL, 3, 0);  expect("rsb\tr0, r1, r1, lsl #3");
    t_alu_reg_shift(&C, T_OP_AND, 9, 1, 2, T_SH_ASR, 31, 0); expect("and.w\tr9, r1, r2, asr #31");
    t_alu_reg_shift(&C, T_OP_ORR, 0, 1, 12, T_SH_LSL, 1, 0); expect("orr.w\tr0, r1, r12, lsl #1");
    t_alu_reg_shift(&C, T_OP_EOR, 0, 1, 2, T_SH_ROR, 8, 0);  expect("eor.w\tr0, r1, r2, ror #8");
    t_bfx(&C, 0, 1, 0, 12, 0);  expect("ubfx\tr0, r1, #0, #12");
    t_bfx(&C, 9, 1, 4, 8, 0);   expect("ubfx\tr9, r1, #4, #8");
    t_bfx(&C, 0, 1, 0, 5, 1);   expect("sbfx\tr0, r1, #0, #5");
    t_bfx(&C, 0, 12, 31, 1, 0); expect("ubfx\tr0, r12, #31, #1");
    t_alu_reg(&C, T_OP_RSB, 0, 1, 2, 0); expect("rsb\tr0, r1, r2");

    t_alu_imm(&C, T_OP_ADD, 0, 1, 3, 1);   expect("adds\tr0, r1, #3");
    t_alu_imm(&C, T_OP_SUB, 0, 1, 3, 1);   expect("subs\tr0, r1, #3");
    t_alu_imm(&C, T_OP_ADD, 0, 0, 200, 1); expect("adds\tr0, #200");
    t_alu_imm(&C, T_OP_SUB, 0, 0, 200, 1); expect("subs\tr0, #200");
    t_alu_imm(&C, T_OP_ADD, 0, 1, 200, 0); expect("add.w\tr0, r1, #200");
    t_alu_imm(&C, T_OP_AND, 0, 1, 0xff, 0); expect("and\tr0, r1, #255");
    t_alu_imm(&C, T_OP_SUB, 9, 1, 1, 0);   expect("sub.w\tr9, r1, #1");
    t_addw(&C, 0, 1, 2000);                expect("addw\tr0, r1, #2000");
    t_subw(&C, 0, 1, 2000);                expect("subw\tr0, r1, #2000");

    t_shift_imm(&C, T_SH_LSL, 0, 1, 3, 1); expect("lsls\tr0, r1, #3");
    t_shift_imm(&C, T_SH_LSR, 0, 1, 3, 1); expect("lsrs\tr0, r1, #3");
    t_shift_imm(&C, T_SH_ASR, 0, 1, 3, 1); expect("asrs\tr0, r1, #3");
    t_shift_imm(&C, T_SH_LSL, 0, 1, 3, 0); expect("lsl.w\tr0, r1, #3");
    t_shift_imm(&C, T_SH_ROR, 0, 1, 3, 0); expect("ror.w\tr0, r1, #3");
    t_shift_imm(&C, T_SH_ASR, 9, 1, 31, 0); expect("asr.w\tr9, r1, #31");
    t_shift_reg(&C, T_SH_LSL, 0, 0, 1, 1); expect("lsls\tr0, r1");
    t_shift_reg(&C, T_SH_LSL, 0, 1, 2, 0); expect("lsl.w\tr0, r1, r2");
    t_shift_reg(&C, T_SH_ASR, 0, 1, 2, 0); expect("asr.w\tr0, r1, r2");

    t_mul(&C, 0, 1, 0);                 expect("muls\tr0, r1, r0");
    t_mul(&C, 0, 1, 2);                 expect("mul\tr0, r1, r2");
    t_mla(&C, 0, 1, 2, 3);              expect("mla\tr0, r1, r2, r3");
    t_mls(&C, 0, 1, 2, 3);              expect("mls\tr0, r1, r2, r3");
    t_div(&C, 0, 1, 2, 1);              expect("sdiv\tr0, r1, r2");
    t_div(&C, 0, 1, 2, 0);              expect("udiv\tr0, r1, r2");
    t_mull(&C, 0, 1, 2, 3, 1);          expect("smull\tr0, r1, r2, r3");
    t_mull(&C, 0, 1, 2, 3, 0);          expect("umull\tr0, r1, r2, r3");
    t_mlal(&C, 0, 1, 2, 3, 1);          expect("smlal\tr0, r1, r2, r3");
    t_mlal(&C, 4, 9, 12, 7, 0);         expect("umlal\tr4, r9, r12, r7");

    t_cmp_reg(&C, 0, 1);                expect("cmp\tr0, r1");
    t_cmp_reg(&C, 9, 1);                expect("cmp\tr9, r1");
    t_cmp_imm(&C, 0, 200);              expect("cmp\tr0, #200");
    t_cmp_imm(&C, 0, 2000);             expect("cmp.w\tr0, #2000");
    t_cmp_imm(&C, 9, 1);                expect("cmp.w\tr9, #1");
    t_tst_reg(&C, 0, 1);                expect("tst\tr0, r1");

    t_ext(&C, 0, 1, 1, 0);              expect("uxtb\tr0, r1");
    t_ext(&C, 0, 1, 2, 0);              expect("uxth\tr0, r1");
    t_ext(&C, 0, 1, 1, 1);              expect("sxtb\tr0, r1");
    t_ext(&C, 0, 1, 2, 1);              expect("sxth\tr0, r1");
    t_ext(&C, 9, 1, 1, 0);              expect("uxtb.w\tr9, r1");
    t_clz(&C, 0, 1);                    expect("clz\tr0, r1");
    t_rev(&C, 0, 1);                    expect("rev\tr0, r1");

    t_ldst_imm(&C, 0, 1, 8, 4, 0, 0);   expect("ldr\tr0, [r1, #8]");
    t_ldst_imm(&C, 0, 1, 124, 4, 0, 0); expect("ldr\tr0, [r1, #124]");
    t_ldst_imm(&C, 0, 1, 1000, 4, 0, 0); expect("ldr.w\tr0, [r1, #1000]");
    t_ldst_imm(&C, 0, 1, -4, 4, 0, 0);  expect("ldr\tr0, [r1, #-4]");
    t_ldst_imm(&C, 0, T_SP, 16, 4, 0, 0); expect("ldr\tr0, [sp, #16]");
    t_ldst_imm(&C, 0, 1, 3, 1, 0, 0);   expect("ldrb\tr0, [r1, #3]");
    t_ldst_imm(&C, 0, 1, 4, 2, 0, 0);   expect("ldrh\tr0, [r1, #4]");
    t_ldst_imm(&C, 0, 1, 3, 1, 1, 0);   expect("ldrsb.w\tr0, [r1, #3]");
    t_ldst_imm(&C, 0, 1, 4, 2, 1, 0);   expect("ldrsh.w\tr0, [r1, #4]");
    t_ldst_imm(&C, 0, 1, 8, 4, 0, 1);   expect("str\tr0, [r1, #8]");
    t_ldst_imm(&C, 0, T_SP, 16, 4, 0, 1); expect("str\tr0, [sp, #16]");
    t_ldst_imm(&C, 0, 1, 3, 1, 0, 1);   expect("strb\tr0, [r1, #3]");
    t_ldst_imm(&C, 0, 1, 4, 2, 0, 1);   expect("strh\tr0, [r1, #4]");
    t_ldst_imm(&C, 0, 1, 1000, 4, 0, 1); expect("str.w\tr0, [r1, #1000]");
    t_ldst_imm(&C, 9, 1, 8, 4, 0, 0);   expect("ldr.w\tr9, [r1, #8]");

    t_ldst_reg(&C, 0, 1, 2, 0, 4, 0, 0); expect("ldr\tr0, [r1, r2]");
    t_ldst_reg(&C, 0, 1, 2, 2, 4, 0, 0); expect("ldr.w\tr0, [r1, r2, lsl #2]");
    t_ldst_reg(&C, 0, 1, 2, 0, 1, 1, 0); expect("ldrsb\tr0, [r1, r2]");
    t_ldst_reg(&C, 0, 1, 2, 0, 1, 0, 1); expect("strb\tr0, [r1, r2]");
    t_ldst_reg(&C, 0, 1, 2, 1, 2, 0, 1); expect("strh.w\tr0, [r1, r2, lsl #1]");
    t_ldst_reg(&C, 0, 1, 2, 3, 1, 0, 0); expect("ldrb.w\tr0, [r1, r2, lsl #3]");
    t_ldst_reg(&C, 0, 1, 2, 1, 2, 1, 0); expect("ldrsh.w\tr0, [r1, r2, lsl #1]");
    t_ldst_reg(&C, 0, 1, 2, 2, 4, 0, 1); expect("str.w\tr0, [r1, r2, lsl #2]");
    t_ldst_reg(&C, 9, 10, 2, 2, 4, 0, 0); expect("ldr.w\tr9, [r10, r2, lsl #2]");

    t_add_sp(&C, 0, 16);                expect("add\tr0, sp, #16");
    t_add_sp(&C, 0, 2000);              expect("addw\tr0, sp, #2000");
    t_sp_adjust(&C, 16, 1);             expect("sub\tsp, #16");
    t_sp_adjust(&C, 16, 0);             expect("add\tsp, #16");
    t_sp_adjust(&C, 600, 1);            expect("subw\tsp, sp, #600");

    t_push(&C, (1u << 4) | (1u << 8) | (1u << T_LR));
    expect("push.w\t{r4, r8, lr}");
    t_pop(&C, (1u << 4) | (1u << 8) | (1u << T_PC));
    expect("pop.w\t{r4, r8, pc}");

    t_bx(&C, T_LR);                     expect("bx\tlr");
    t_adr_w(&C, 11, 12);                expect("adr.w\tr11, #12");
    t_adr_w(&C, 1, 4095);               expect("adr.w\tr1, #4095");
    { int at = t_adr_w(&C, 11, 0); t_patch_adr_w(&C, at, 11, 100);
                                        expect("adr.w\tr11, #100"); }
    t_alu_reg(&C, T_OP_ADD, 12, 12, 11, 0); expect("add.w\tr12, r12, r11");
    t_ldst_pair(&C, 2, 3, T_PC, 8, 0);  expect("ldrd\tr2, r3, [pc, #8]");
    t_ldst_pair(&C, 12, 11, T_PC, -4, 0); expect("ldrd\tr12, r11, [pc, #-4]");
    t_ldst_pair(&C, 0, 1, T_PC, 1020, 0); expect("ldrd\tr0, r1, [pc, #1020]");
    t_tbh(&C, 0);                       expect("tbh\t[pc, r0, lsl #1]");
    t_tbh(&C, 12);                      expect("tbh\t[pc, r12, lsl #1]");
    t_blx(&C, 3);                       expect("blx\tr3");
    t_nop(&C);                          expect("nop");

    /* The system instructions: what a Cortex-M program reaches inline
     * assembly for, and the only part of this file a C program cannot
     * otherwise express. Every special register is listed, because the
     * SYSm number is the whole encoding and a wrong one reads a
     * different register perfectly legally. */
    t_mrs(&C, 0, T_SYS_PRIMASK);        expect("mrs\tr0, primask");
    t_mrs(&C, 3, T_SYS_BASEPRI);        expect("mrs\tr3, basepri");
    t_mrs(&C, 7, T_SYS_CONTROL);        expect("mrs\tr7, control");
    t_mrs(&C, 1, T_SYS_MSP);            expect("mrs\tr1, msp");
    t_mrs(&C, 2, T_SYS_PSP);            expect("mrs\tr2, psp");
    t_mrs(&C, 4, T_SYS_IPSR);           expect("mrs\tr4, ipsr");
    t_mrs(&C, 5, T_SYS_XPSR);           expect("mrs\tr5, xpsr");
    t_mrs(&C, 6, T_SYS_FAULTMASK);      expect("mrs\tr6, faultmask");
    t_mrs(&C, 8, T_SYS_APSR);           expect("mrs\tr8, apsr");
    t_mrs(&C, 9, T_SYS_BASEPRI_MAX);    expect("mrs\tr9, basepri_max");
    t_msr(&C, T_SYS_PRIMASK, 0);        expect("msr\tprimask, r0");
    t_msr(&C, T_SYS_BASEPRI, 3);        expect("msr\tbasepri, r3");
    t_msr(&C, T_SYS_CONTROL, 7);        expect("msr\tcontrol, r7");
    t_msr(&C, T_SYS_MSP, 1);            expect("msr\tmsp, r1");
    t_msr(&C, T_SYS_PSP, 2);            expect("msr\tpsp, r2");
    t_msr(&C, T_SYS_FAULTMASK, 4);      expect("msr\tfaultmask, r4");

    t_cps(&C, 1, 1, 0);                 expect("cpsid i");
    t_cps(&C, 0, 1, 0);                 expect("cpsie i");
    t_cps(&C, 1, 0, 1);                 expect("cpsid f");
    t_cps(&C, 0, 0, 1);                 expect("cpsie f");
    t_cps(&C, 1, 1, 1);                 expect("cpsid if");

    t_barrier(&C, T_BAR_DSB);           expect("dsb\tsy");
    t_barrier(&C, T_BAR_DMB);           expect("dmb\tsy");
    t_barrier(&C, T_BAR_ISB);           expect("isb\tsy");

    t_hint(&C, T_HINT_YIELD);           expect("yield");
    t_hint(&C, T_HINT_WFE);             expect("wfe");
    t_hint(&C, T_HINT_WFI);             expect("wfi");
    t_hint(&C, T_HINT_SEV);             expect("sev");

    t_bkpt(&C, 0);                      expect("bkpt\t#0");
    t_bkpt(&C, 170);                    expect("bkpt\t#170");
    t_rbit(&C, 0, 1);                   expect("rbit\tr0, r1");

    /* The offset is in WORDS in the encoding and bytes in the syntax. */
    t_ldrex(&C, 0, 1, 0);               expect("ldrex\tr0, [r1]");
    t_ldrex(&C, 2, 3, 16);              expect("ldrex\tr2, [r3, #16]");
    t_strex(&C, 0, 1, 2, 0);            expect("strex\tr0, r1, [r2]");
    t_strex(&C, 3, 4, 5, 8);            expect("strex\tr3, r4, [r5, #8]");
    t_ldrexbh(&C, 0, 1, 1);             expect("ldrexb\tr0, [r1]");
    t_ldrexbh(&C, 12, 10, 2);           expect("ldrexh\tr12, [r10]");
    t_strexbh(&C, 2, 3, 1, 1);          expect("strexb\tr2, r3, [r1]");
    t_strexbh(&C, 9, 11, 10, 2);        expect("strexh\tr9, r11, [r10]");
    t_clrex(&C);                        expect("clrex");

    /* the 16-bit push/pop, chosen when the list is r0-r7 plus lr/pc */
    t_push(&C, (1u << 3) | (1u << T_LR)); expect("push\t{r3, lr}");
    t_pop(&C, (1u << 3) | (1u << T_PC));  expect("pop\t{r3, pc}");
    t_push(&C, 0xf0u | (1u << T_LR));     expect("push\t{r4, r5, r6, r7, lr}");

    fwrite(C.p, 1, (size_t)C.len, stdout);
    return 0;
}
