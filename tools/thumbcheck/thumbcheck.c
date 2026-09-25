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
    t_blx(&C, 3);                       expect("blx\tr3");
    t_nop(&C);                          expect("nop");

    fwrite(C.p, 1, (size_t)C.len, stdout);
    return 0;
}
