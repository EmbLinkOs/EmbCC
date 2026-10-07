/* Hands the LoongArch64 assembler's vocabulary to it line by line, so
 * tests/golden/loongarch-asm.sh can hand the SAME lines to llvm-mc and
 * compare.
 *
 *   laasmcheck --list    the vocabulary, one statement per line
 *   laasmcheck           each statement, an @, and the words
 *                        src/arch/loongarch/asm.c assembles it to,
 *                        space-separated (a pseudo may be several)
 *   laasmcheck --refuse  each statement of the refusal list (stdin), a
 *                        an @, and the message it was refused with, or
 *                        ACCEPTED
 *   laasmcheck --bytes   each statement of stdin, an @, and its words
 *
 * The vocabulary is generated from asm.c's own tables, so an instruction
 * added there cannot escape the referee. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/arch/loongarch/asm.h"

int main(int argc, char **argv)
{
    char line[512];
    FILE *in;
    char err[512];
    int list = argc > 1 && strcmp(argv[1], "--list") == 0;
    int refuse = argc > 1 && strcmp(argv[1], "--refuse") == 0;
    int bytes = argc > 1 && strcmp(argv[1], "--bytes") == 0;

    if (refuse || bytes) {
        in = stdin;
    } else {
        in = tmpfile();
        if (!in) { fprintf(stderr, "no tmpfile\n"); return 1; }
        laasm_vocabulary(in);
        rewind(in);
    }
    while (fgets(line, sizeof line, in)) {
        struct code c = { 0 };
        line[strcspn(line, "\n")] = 0;
        if (list) { printf("%s\n", line); continue; }
        if (refuse) {
            if (laasm_assemble(line, &c, err, sizeof err) != 0)
                printf("%s@%s\n", line, err);
            else
                printf("%s@ACCEPTED\n", line);
            free(c.p);
            continue;
        }
        if (laasm_assemble(line, &c, err, sizeof err) != 0) {
            fprintf(stderr, "laasmcheck: %s\n  on: %s\n", err, line);
            return 1;
        }
        printf("%s@", line);
        for (int p = 0; p + 4 <= c.len; p += 4)
            printf("%s%02x%02x%02x%02x", p ? " " : "", c.p[p + 3], c.p[p + 2],
                   c.p[p + 1], c.p[p]);
        printf("\n");
        free(c.p);
    }
    return 0;
}
