/* Hands the ARMv7-M inline-asm vocabulary to its own assembler and prints
 * the bytes, so tests/golden/thumb-asm.sh can hand the SAME lines to
 * llvm-mc and compare. `--list` prints the lines instead.
 *
 * The vocabulary is generated from asm.c's own tables, so an instruction
 * added there cannot escape the referee -- which is the only thing that
 * makes a hand-written assembler trustworthy. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/arch/thumb/asm.h"
#include "../../src/arch/thumb/emit.h"

/* --each LEVEL FILE: assemble each line of FILE on its own at that level
 * (6, 7, 8, or 9 for ARMv8-M Baseline; 7e and 8e with the DSP extension;
 * `a` for ARM state, ARMv7-A) and print `N ok` or `N no` for line N, so a test can compare what this
 * assembler refuses with what llvm-mc refuses for the same core. Each
 * line starts outside an IT block. */
static int each(const char *lv, const char *path)
{
    int level = lv[0] == 'a' ? 7 : atoi(lv), dsp = strchr(lv, 'e') != NULL;
    char line[512], err[512];
    FILE *f = fopen(path, "r");
    int n = 0;
    if (!f) { fprintf(stderr, "tasmcheck: cannot read %s\n", path); return 1; }
    tasm_set_arch(level);
    tasm_set_dsp(dsp);
    t_isa_a32 = lv[0] == 'a';
    while (fgets(line, sizeof line, f)) {
        struct code c = { 0 };
        line[strcspn(line, "\n")] = 0;
        n++;
        tasm_reset();
        printf("%d %s\n", n,
               tasm_assemble(line, &c, err, sizeof err) == 0 ? "ok" : "no");
        free(c.p);
    }
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    char line[512];
    FILE *tmp;
    struct code c = { 0 };
    char err[512];
    int list = 0, v8 = -1, dsp = 0, more = 0;

    if (argc == 4 && strcmp(argv[1], "--each") == 0)
        return each(argv[2], argv[3]);
    /* --list: print the lines; --v8m-main / --v8m-base: ARMv8-M's
     * vocabulary (tasm_vocabulary_v8m) at that level instead; --dsp: the
     * DSP extension's (tasm_vocabulary_dsp), at ARMv7E-M; --more: the
     * multiplies, bit fields and pairs (tasm_vocabulary_more), at ARMv7E-M;
     * --a32: in ARM state, both of those */
    for (int k = 1; k < argc; k++) {
        if (strcmp(argv[k], "--list") == 0) list = 1;
        else if (strcmp(argv[k], "--v8m-main") == 0) v8 = 0;
        else if (strcmp(argv[k], "--v8m-base") == 0) v8 = 1;
        else if (strcmp(argv[k], "--dsp") == 0) dsp = 1;
        else if (strcmp(argv[k], "--more") == 0) more = 1;
        else if (strcmp(argv[k], "--a32") == 0) t_isa_a32 = 1;
    }
    tmp = tmpfile();
    if (!tmp) { fprintf(stderr, "no tmpfile\n"); return 1; }
    if (t_isa_a32) {
        tasm_set_arch(7);
        tasm_vocabulary_dsp(tmp);
        tasm_vocabulary_more(tmp);
    } else if (dsp || more) {
        tasm_set_arch(7);
        tasm_set_dsp(1);
        if (dsp)
            tasm_vocabulary_dsp(tmp);
        else
            tasm_vocabulary_more(tmp);
    } else if (v8 >= 0) {
        tasm_set_arch(v8 ? TASM_V8M_BASE : 8);
        tasm_vocabulary_v8m(tmp, v8);
    } else {
        tasm_vocabulary(tmp);
    }
    rewind(tmp);
    while (fgets(line, sizeof line, tmp)) {
        if (list) { fputs(line, stdout); continue; }
        line[strcspn(line, "\n")] = 0;
        if (tasm_assemble(line, &c, err, sizeof err) != 0) {
            fprintf(stderr, "tasmcheck: %s\n  on: %s\n", err, line);
            return 1;
        }
    }
    fclose(tmp);
    if (!list)
        fwrite(c.p, 1, (size_t)c.len, stdout);
    return 0;
}
