/* Hands EmbCC's AVR assembler vocabulary to llvm-mc and compares.
 *
 * `--list` writes the assembly and `bytes` assembles what it reads, both
 * from ONE table -- so a mnemonic added to src/arch/avr/asm.c cannot be
 * printed without being encoded, or encoded without being printed, and a
 * mismatch cannot shift every comparison after it onto the wrong
 * instruction.
 *
 * `--pcrel` and `pcrel-bytes` are the other half. llvm-mc leaves a
 * relocation on an AVR branch even to a label in its own section, so its
 * bytes are a placeholder there: those forms are assembled here and
 * DISASSEMBLED, and the text compared. That is what grades the MEANING of
 * a condition rather than only its packing -- the distinction that cost a
 * shipped bug in this backend's branch enum.
 */
#include <stdio.h>
#include <string.h>

#include "../../src/arch/code.h"
#include "../../src/arch/avr/asm.h"

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "bytes";
    struct code c = { 0 };
    char err[256];

    if (!strcmp(mode, "--list")) {
        avrasm_vocabulary(stdout);
        return 0;
    }
    if (!strcmp(mode, "--pcrel")) {
        avrasm_pcrel_vocabulary(stdout);
        return 0;
    }
    if (!strcmp(mode, "pcrel-bytes")) {
        if (avrasm_pcrel_encode(&c, err, sizeof err) != 0) {
            fprintf(stderr, "%s\n", err);
            return 1;
        }
    } else {
        /* `bytes`: assemble what arrives on stdin, which is the vocabulary
         * the --list mode printed. Reading it back rather than walking the
         * table again is deliberate: it exercises the PARSER, which is the
         * half of an assembler the encoder's own referee does not reach. */
        static char buf[1 << 16];
        size_t n = fread(buf, 1, sizeof buf - 1, stdin);
        buf[n] = '\0';
        if (avrasm_assemble(buf, &c, err, sizeof err) != 0) {
            fprintf(stderr, "%s\n", err);
            return 1;
        }
    }
    fwrite(c.p, 1, (size_t)c.len, stdout);
    return 0;
}
