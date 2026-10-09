/* Hands the SPARC assembler's vocabulary to it line by line, so
 * tests/golden/sparc-asm.sh can hand the same statements to llvm-mc and
 * the bytes to llvm-objdump.
 *
 *   spasmcheck --vocab      one line per statement: EXPECTED|BYTES|STATEMENT
 *                           EXPECTED is the mnemonic llvm-objdump must print
 *                           for each instruction ('+' between two), BYTES
 *                           what src/arch/sparc/asm.c made, and STATEMENT
 *                           the line as written
 *   spasmcheck --refuse     each statement of stdin, an @, and the message it
 *                           was refused with, or ACCEPTED
 *   spasmcheck --bytes      each template of stdin (`|` separating its
 *                           statements), an @, and its bytes
 *
 * The vocabulary is asm.c's own (spasm_vocabulary), generated from its
 * tables, so an instruction added there cannot escape the referee. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/arch/code.h"
#include "../../src/arch/sparc/asm.h"

int main(int argc, char **argv)
{
    char line[512], err[512];
    if (argc > 1 && !strcmp(argv[1], "--vocab")) {
        FILE *in = tmpfile();
        if (!in) { fprintf(stderr, "no tmpfile\n"); return 1; }
        spasm_vocabulary(in);
        rewind(in);
        while (fgets(line, sizeof line, in)) {
            struct code c = { 0 };
            char *at = strrchr(line, '@');
            line[strcspn(line, "\n")] = 0;
            if (!at) {
                fprintf(stderr, "spasmcheck: no @ in \"%s\"\n", line);
                return 2;
            }
            *at = 0;
            if (spasm_assemble(line, &c, err, sizeof err) != 0) {
                fprintf(stderr, "spasmcheck: %s\n  on: %s\n", err, line);
                return 1;
            }
            if (c.len == 0 || c.len % 4) {
                fprintf(stderr, "spasmcheck: \"%s\" made %d bytes\n", line,
                        c.len);
                return 1;
            }
            printf("%s|", at + 1);
            for (int k = 0; k < c.len; k++)
                printf("%02x", c.p[k]);
            printf("|%s\n", line);
            free(c.p);
        }
        return 0;
    }
    if (argc > 1 && (!strcmp(argv[1], "--refuse") ||
                     !strcmp(argv[1], "--bytes"))) {
        int refuse = !strcmp(argv[1], "--refuse");
        while (fgets(line, sizeof line, stdin)) {
            struct code c = { 0 };
            line[strcspn(line, "\n")] = 0;
            for (char *p = line; *p; p++)
                if (*p == '|')
                    *p = '\n';          /* a template's lines */
            int rc = spasm_assemble(line, &c, err, sizeof err);
            for (char *p = line; *p; p++)
                if (*p == '\n')
                    *p = '|';
            if (refuse) {
                printf("%s@%s\n", line, rc ? err : "ACCEPTED");
            } else if (rc) {
                fprintf(stderr, "spasmcheck: %s\n  on: %s\n", err, line);
                return 1;
            } else {
                printf("%s@", line);
                for (int k = 0; k < c.len; k++)
                    printf("%02x", c.p[k]);
                printf("\n");
            }
            free(c.p);
        }
        return 0;
    }
    fprintf(stderr, "usage: spasmcheck --vocab | --refuse | --bytes\n");
    return 2;
}
