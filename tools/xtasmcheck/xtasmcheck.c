/* Hands the Xtensa assembler's vocabulary to it line by line, laid out at
 * known addresses, so tests/golden/xtensa-asm.sh can hand the bytes to
 * QEMU's de212 disassembler and the statements to GNU as.
 *
 *   xtasmcheck --vocab BASE ORG  one line per instruction:
 *                                  EXPECTED|BYTES|STATEMENT
 *                                EXPECTED is what QEMU's monitor prints
 *                                for it at BASE + ORG + its offset (`!`
 *                                for a form the de212 lacks), BYTES the
 *                                three bytes src/arch/xtensa/asm.c made,
 *                                and STATEMENT the line as written, its
 *                                placeholders filled, for GNU as to
 *                                assemble at ORG + the same offset
 *   xtasmcheck --refuse          each statement of stdin, an @, and the
 *                                message it was refused with, or ACCEPTED
 *   xtasmcheck --bytes [PC]      each template of stdin (`;` separating
 *                                its statements), an @, and its bytes;
 *                                with PC, the first statement is there
 *
 * The vocabulary is asm.c's own (xtasm_vocabulary), generated from its
 * tables, so an instruction added there cannot escape the referee. An
 * `entry` is padded to a word with nops first: GNU as requires it, and
 * the nops are vocabulary too. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/arch/code.h"
#include "../../src/arch/xtensa/asm.h"

/* "{+N}" / "{C+K}" / "{L-K}" in s, replaced: in the statement by `.+D`
 * (D from the instruction at pc), in the text by the absolute address. */
static void fill(const char *s, char *out, size_t cap, long pc,
                 unsigned long base, int stmt)
{
    size_t o = 0;
    while (*s && o + 32 < cap) {
        if (*s != '{') {
            out[o++] = *s++;
            continue;
        }
        char kind = s[1];
        long v, target;
        char *e;
        if (kind == '+') {
            v = strtol(s + 2, &e, 10);
            target = pc + v;
        } else {
            v = strtol(s + 2, &e, 10);
            target = kind == 'C' ? (pc & ~3L) + 4 + v
                                 : ((pc + 3) & ~3L) + v;
        }
        if (*e != '}') {
            fprintf(stderr, "xtasmcheck: a bad placeholder in \"%s\"\n", s);
            exit(2);
        }
        s = e + 1;
        if (stmt)
            o += (size_t)snprintf(out + o, cap - o, ".%+ld", target - pc);
        else
            o += (size_t)snprintf(out + o, cap - o, "0x%lx",
                                  base + (unsigned long)target);
    }
    out[o] = 0;
}

static struct code C;

/* Assembles one statement at section offset pc; exits on failure. */
static void one(const char *stmt, const char *expect, long pc,
                unsigned long base)
{
    char st[256], ex[256], err[512];
    int at = C.len;
    fill(stmt, st, sizeof st, pc, base, 1);
    fill(expect, ex, sizeof ex, pc, base, 0);
    xtasm_set_pc(pc);
    if (xtasm_assemble(st, &C, err, sizeof err) != 0) {
        fprintf(stderr, "xtasmcheck: %s\n  on: %s\n", err, st);
        exit(1);
    }
    if (C.len - at != 3) {
        fprintf(stderr, "xtasmcheck: \"%s\" made %d bytes\n", st, C.len - at);
        exit(1);
    }
    printf("%s|%02x%02x%02x|%s\n", ex, C.p[at], C.p[at + 1], C.p[at + 2],
           st);
}

int main(int argc, char **argv)
{
    char line[512], err[512];
    if (argc > 3 && !strcmp(argv[1], "--vocab")) {
        unsigned long base = strtoul(argv[2], NULL, 0);
        long org = strtol(argv[3], NULL, 0);
        FILE *in = tmpfile();
        if (!in) { fprintf(stderr, "no tmpfile\n"); return 1; }
        xtasm_vocabulary(in);
        rewind(in);
        while (fgets(line, sizeof line, in)) {
            char *at = strchr(line, '@');
            line[strcspn(line, "\n")] = 0;
            if (!at) {
                fprintf(stderr, "xtasmcheck: no @ in \"%s\"\n", line);
                return 2;
            }
            *at = 0;
            if (!strncmp(line, "entry", 5))
                while ((org + C.len) & 3)
                    one("nop", "nop", org + C.len, base);
            one(line, at + 1, org + C.len, base);
        }
        return 0;
    }
    if (argc > 1 && (!strcmp(argv[1], "--refuse") ||
                     !strcmp(argv[1], "--bytes"))) {
        int refuse = !strcmp(argv[1], "--refuse");
        long pc = argc > 2 ? strtol(argv[2], NULL, 0) : -1;
        while (fgets(line, sizeof line, stdin)) {
            struct code c = { 0 };
            line[strcspn(line, "\n")] = 0;
            for (char *p = line; *p; p++)
                if (*p == '|')
                    *p = '\n';          /* a template's lines */
            xtasm_set_pc(pc);
            int rc = xtasm_assemble(line, &c, err, sizeof err);
            for (char *p = line; *p; p++)
                if (*p == '\n')
                    *p = '|';
            if (refuse) {
                printf("%s@%s\n", line, rc ? err : "ACCEPTED");
            } else if (rc) {
                fprintf(stderr, "xtasmcheck: %s\n  on: %s\n", err, line);
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
    fprintf(stderr, "usage: xtasmcheck --vocab BASE ORG | --refuse | "
                    "--bytes [PC]\n");
    return 2;
}
