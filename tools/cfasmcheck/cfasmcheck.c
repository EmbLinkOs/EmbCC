/* Hands the ColdFire assembler's vocabulary to its referee line by line,
 * laid out at a known address, for tests/golden/coldfire-asm.sh.
 *
 *   cfasmcheck --vocab BASE  one line per statement: EXPECTED|BYTES|STATEMENT
 *                            EXPECTED is what QEMU's m68k disassembler
 *                            prints for it at BASE + its offset (its {+N}
 *                            placeholders filled in), BYTES what
 *                            src/arch/coldfire/asm.c made of STATEMENT
 *   cfasmcheck --refuse      each statement of stdin, an @, and the message
 *                            it was refused with, or ACCEPTED
 *   cfasmcheck --bytes       each template of stdin (`|` separating its
 *                            statements), an @, and its bytes in hex
 *
 * The vocabulary is asm.c's own (cfasm_vocabulary), generated from its
 * samples, so a form added there cannot escape the referee. There is no
 * m68k assembler here to compare bytes with: QEMU's disassembler -- the
 * decoder the board executes beside -- reads each statement back. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/arch/code.h"
#include "../../src/arch/coldfire/asm.h"

static void fill(const char *s, char *out, size_t cap, unsigned long pc)
{
    size_t o = 0;
    while (*s && o + 24 < cap) {
        if (s[0] == '{' && s[1] == '+') {
            char *e;
            long v = strtol(s + 2, &e, 10);
            if (*e != '}') {
                fprintf(stderr, "cfasmcheck: a bad placeholder in \"%s\"\n", s);
                exit(2);
            }
            o += (size_t)snprintf(out + o, cap - o, "0x%lx",
                                  (pc + (unsigned long)v) & 0xffffffffUL);
            s = e + 1;
            continue;
        }
        out[o++] = *s++;
    }
    out[o] = 0;
}

int main(int argc, char **argv)
{
    char line[1024], err[512], want[1024];
    if (argc == 3 && !strcmp(argv[1], "--vocab")) {
        unsigned long pc = strtoul(argv[2], NULL, 0);
        FILE *t = tmpfile();
        if (!t)
            return 2;
        cfasm_vocabulary(t);
        rewind(t);
        while (fgets(line, sizeof line, t)) {
            char *at = strchr(line, '`');
            struct code c = { 0 };
            line[strcspn(line, "\n")] = 0;
            if (!at) {
                fprintf(stderr, "cfasmcheck: no ` in \"%s\"\n", line);
                return 2;
            }
            *at = 0;
            if (cfasm_assemble(line, &c, err, (int)sizeof err) != 0) {
                fprintf(stderr, "cfasmcheck: \"%s\": %s\n", line, err);
                return 1;
            }
            fill(at + 1, want, sizeof want, pc);
            printf("%s|", want);
            for (int k = 0; k < c.len; k++)
                printf("%02x", c.p[k]);
            printf("|%s\n", line);
            pc += (unsigned long)c.len;
            free(c.p);
        }
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--refuse")) {
        while (fgets(line, sizeof line, stdin)) {
            struct code c = { 0 };
            line[strcspn(line, "\n")] = 0;
            if (cfasm_assemble(line, &c, err, (int)sizeof err) == 0)
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
            if (cfasm_assemble(line, &c, err, (int)sizeof err) != 0) {
                for (char *p = line; *p; p++)
                    if (*p == '\n')
                        *p = '|';
                fprintf(stderr, "cfasmcheck: \"%s\": %s\n", line, err);
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
    fprintf(stderr, "usage: cfasmcheck --vocab BASE | --refuse | --bytes\n");
    return 2;
}
