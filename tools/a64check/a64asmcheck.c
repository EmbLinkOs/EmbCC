/* Drives src/arch/aarch64/asm.c for tests/golden/arm64-asm.sh.
 *
 *   a64asmcheck --vocabulary        print one line per vocabulary entry
 *   a64asmcheck < lines > bytes      assemble each line, emit the raw words
 *
 * Each input line is assembled on its own, exactly as irgen hands the
 * assembler one template at a time; the test feeds the same lines to
 * aarch64-elf-as and compares the bytes.
 */
#include <stdio.h>
#include <string.h>

#include "../../src/arch/aarch64/asm.h"

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--vocabulary") == 0) {
        a64asm_vocabulary(stdout);
        return 0;
    }
    char line[1024];
    struct code c = { 0, 0, 0 };
    int lineno = 0;
    while (fgets(line, sizeof line, stdin)) {
        lineno++;
        char err[512];
        if (a64asm_assemble(line, &c, err, sizeof err) != 0) {
            fprintf(stderr, "line %d: %s\n", lineno, err);
            return 1;
        }
    }
    fwrite(c.p, 1, (size_t)c.len, stdout);
    return 0;
}
