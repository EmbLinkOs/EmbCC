/* Hands EmbCC's whole AVR vocabulary to llvm-mc and compares the bytes.
 *
 * `--list` writes the assembly, `bytes` writes what emit.c produced, and
 * both come from ONE walk of the same tables -- so a form printed but not
 * encoded (or the reverse) cannot shift every comparison after it and
 * report the wrong instruction.
 *
 * AVR is a good machine to have a referee for. Its operand fields are
 * SPLIT -- a five-bit register across bits 8 and 7..4, a six-bit
 * displacement across three fields, an eight-bit immediate across two --
 * and a mistake in any of them encodes a DIFFERENT valid instruction
 * rather than an invalid one. Three of its groups also have restricted
 * operands (immediates reach only r16-r31, adiw only four pairs, ldd only
 * Y and Z), so a wrong register is silently a different register.
 *
 * The PC-relative forms are refereed the OTHER WAY ROUND, under
 * `--branches` and `branch-bytes`: llvm-mc leaves a relocation on a
 * branch even to a label in its own section, so its bytes carry a
 * placeholder and comparing them would grade nothing. Instead our bytes
 * are DISASSEMBLED and the text compared -- which is what catches a
 * condition that encodes cleanly and means the wrong thing, the bug that
 * made this mode exist.
 *
 * `--writes` is the decoder's turn: which registers avr_insn_writes says
 * each form writes, for a rule per mnemonic to grade.
 */
#include <stdio.h>
#include <string.h>

#include "../../src/arch/code.h"
#include "../../src/arch/avr/emit.h"

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--list")) {
        avr_vocabulary(stdout);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--branches")) {
        avr_branch_vocabulary(stdout);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--writes")) {
        /* What avr_insn_writes reads out of every form -- the plain
         * vocabulary, then the PC-relative one, in --list/--branches
         * order -- one line each, for the rule in avr-encoding.sh. */
        struct code c = { 0 };
        avr_encode_vocabulary(&c);
        avr_encode_branches(&c);
        for (long p = 0; p < c.len; ) {
            int len, any = 0;
            unsigned long w = avr_insn_writes(c.p + p, c.len - p, &len);
            for (int r = 0; r < 32; r++)
                if (w >> r & 1) {
                    printf("%sr%d", any ? " " : "", r);
                    any = 1;
                }
            printf("%s\n", any ? "" : "-");
            p += len;
        }
        return 0;
    }
    {
        struct code c = { 0 };
        if (argc > 1 && !strcmp(argv[1], "branch-bytes"))
            avr_encode_branches(&c);
        else
            avr_encode_vocabulary(&c);
        fwrite(c.p, 1, (size_t)c.len, stdout);
    }
    return 0;
}
