/* Hands the MIPS inline-asm vocabulary to its own assembler, line by
 * line, so tests/golden/mips-asm.sh can hand the SAME lines to llvm-mc
 * and compare.
 *
 *   mipsasmcheck --list    the vocabulary, one statement per line
 *   mipsasmcheck bytes     what src/arch/mips/asm.c assembles it to
 *
 * The vocabulary is generated from asm.c's own tables, so an instruction
 * added there cannot escape the referee. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/arch/mips/asm.h"

int main(int argc, char **argv)
{
    char line[512];
    FILE *tmp;
    struct code c = { 0 };
    char err[512];
    int list = argc > 1 && strcmp(argv[1], "--list") == 0;

    tmp = tmpfile();
    if (!tmp) { fprintf(stderr, "no tmpfile\n"); return 1; }
    mipsasm_vocabulary(tmp);
    rewind(tmp);
    while (fgets(line, sizeof line, tmp)) {
        if (list) { fputs(line, stdout); continue; }
        line[strcspn(line, "\n")] = 0;
        if (mipsasm_assemble(line, &c, err, sizeof err) != 0) {
            fprintf(stderr, "mipsasmcheck: %s\n  on: %s\n", err, line);
            return 1;
        }
    }
    fclose(tmp);
    if (!list)
        fwrite(c.p, 1, (size_t)c.len, stdout);
    return 0;
}
