/* Hands the RISC-V inline-asm vocabulary to its own assembler and prints
 * the bytes, so tests/golden/riscv-asm.sh can hand the SAME lines to
 * llvm-mc and compare. `--list` prints the lines instead.
 *
 * The vocabulary is generated from asm.c's own tables, so an instruction
 * added there cannot escape the referee -- which is the only thing that
 * makes a hand-written assembler trustworthy. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/arch/riscv/asm.h"
#include "../../src/arch/target.h"

int main(int argc, char **argv)
{
    char line[512];
    FILE *tmp;
    struct code c = { 0 };
    char err[512];
    int list = argc > 1 && strcmp(argv[1], "--list") == 0;

    /* The vocabulary depends on the width: the `w` forms and `ld`/`sd`
     * exist only at RV64, and asking for them at RV32 is refused. */
    target_set((argc > 2 && strcmp(argv[2], "rv32") == 0)
                   ? TARGET_RISCV32 : TARGET_RISCV64);
    /* ...and the F and D forms with the extensions on (rv64gc) */
    target_set_riscv_isa(1, 1, target_riscv_rvc(), 1);

    tmp = tmpfile();
    if (!tmp) { fprintf(stderr, "no tmpfile\n"); return 1; }
    rvasm_vocabulary(tmp);
    rewind(tmp);
    while (fgets(line, sizeof line, tmp)) {
        if (list) { fputs(line, stdout); continue; }
        line[strcspn(line, "\n")] = 0;
        if (rvasm_assemble(line, &c, err, sizeof err) != 0) {
            fprintf(stderr, "rvasmcheck: %s\n  on: %s\n", err, line);
            return 1;
        }
    }
    fclose(tmp);
    if (!list)
        fwrite(c.p, 1, (size_t)c.len, stdout);
    return 0;
}
