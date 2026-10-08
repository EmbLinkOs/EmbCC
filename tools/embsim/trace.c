/* trace.c -- what a run reports: --trace's line per instruction, and at
 * the end --count's file (the format of tools/bench's QEMU plugin: the
 * instruction count, then the estimated cycles) and the --stats line. */
#include <string.h>

#include "sim.h"

void trace_open(struct sim *s, const char *path)
{
    s->trace = strcmp(path, "-") ? fopen(path, "w") : stderr;
    if (!s->trace)
        die("cannot write %s", path);
}

void trace_insn(struct sim *s, u32 pc, const u32 *units, int n, int unit)
{
    fprintf(s->trace, "%08x:", pc);
    for (int i = 0; i < n; i++)
        fprintf(s->trace, " %0*x", 2 * unit, units[i]);
    fputc('\n', s->trace);
}

void trace_report(struct sim *s, const char *count_path, int stats)
{
    fflush(stdout);
    if (count_path) {
        FILE *f = fopen(count_path, "w");
        if (!f)
            die("cannot write %s", count_path);
        fprintf(f, "%llu\n%llu\n", (unsigned long long)s->insns,
                (unsigned long long)s->cycles);
        fclose(f);
    }
    if (stats || s->state == END_LOCKUP || s->state == END_BUDGET)
        fprintf(stderr, "embsim: %s; %llu instructions, %llu cycles (est.)\n",
                s->end_why, (unsigned long long)s->insns,
                (unsigned long long)s->cycles);
    if (s->trace && s->trace != stderr)
        fclose(s->trace);
}
