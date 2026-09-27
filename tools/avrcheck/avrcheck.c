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
    {
        struct code c = { 0, 0, 0 };
        avr_encode_vocabulary(&c);
        fwrite(c.p, 1, (size_t)c.len, stdout);
    }
    return 0;
}
