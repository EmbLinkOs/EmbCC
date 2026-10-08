/* Hands the RX assembler's vocabulary to its referees line by line, for
 * tests/golden/rx-asm.sh.
 *
 *   rxasmcheck --vocab     one line per statement: LEN|STATEMENT|EXPECTED
 *                          LEN the bytes src/arch/rx/asm.c made for it
 *                          (alone: every target is `.+N`, so where it lands
 *                          does not change them), STATEMENT the line as
 *                          written -- for GNU as and EmbCC's .s assembler
 *                          both -- and EXPECTED what rx-elf-objdump prints
 *                          for it, `|` between the instructions of a
 *                          statement that is two, {+N} the address N bytes
 *                          from its start
 *   rxasmcheck --refuse    each statement of stdin, an @, and the message
 *                          it was refused with, or ACCEPTED
 *   rxasmcheck --bytes     each template of stdin (`|` separating its
 *                          statements), an @, and its bytes in hex
 *
 * The vocabulary is asm.c's own (rxasm_vocabulary), generated from its
 * tables, so an instruction added there cannot escape the referee. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/arch/code.h"
#include "../../src/arch/rx/asm.h"

int main(int argc, char **argv)
{
    char line[1024], err[512];
    if (argc == 2 && !strcmp(argv[1], "--vocab")) {
        FILE *t = tmpfile();
        if (!t)
            return 2;
        rxasm_vocabulary(t);
        rewind(t);
        while (fgets(line, sizeof line, t)) {
            char *at = strchr(line, '@');
            struct code c = { 0 };
            line[strcspn(line, "\n")] = 0;
            if (!at) {
                fprintf(stderr, "rxasmcheck: no @ in \"%s\"\n", line);
                return 2;
            }
            *at = 0;
            if (rxasm_assemble(line, &c, err, (int)sizeof err) != 0) {
                fprintf(stderr, "rxasmcheck: \"%s\": %s\n", line, err);
                return 1;
            }
            printf("%d|%s|%s\n", c.len, line, at + 1);
            free(c.p);
        }
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--refuse")) {
        while (fgets(line, sizeof line, stdin)) {
            struct code c = { 0 };
            line[strcspn(line, "\n")] = 0;
            if (rxasm_assemble(line, &c, err, (int)sizeof err) == 0)
                printf("%s@ACCEPTED\n", line);
            else
                printf("%s@%s\n", line, err);
            free(c.p);
        }
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--bytes")) {
        while (fgets(line, sizeof line, stdin)) {
            struct code c = { 0 };
            line[strcspn(line, "\n")] = 0;
            for (char *p = line; *p; p++)
                if (*p == '|')
                    *p = '\n';
            if (rxasm_assemble(line, &c, err, (int)sizeof err) != 0) {
                for (char *p = line; *p; p++)
                    if (*p == '\n')
                        *p = '|';
                fprintf(stderr, "rxasmcheck: \"%s\": %s\n", line, err);
                return 1;
            }
            for (char *p = line; *p; p++)
                if (*p == '\n')
                    *p = '|';
            printf("%s@", line);
            for (int k = 0; k < c.len; k++)
                printf("%02x", c.p[k]);
            printf("\n");
            free(c.p);
        }
        return 0;
    }
    fprintf(stderr, "usage: rxasmcheck --vocab | --refuse | --bytes\n");
    return 2;
}
