/* embsim -- an instruction-set simulator: run a firmware image without a
 * board.
 *
 * It runs an ELF image the way the part does, on a model of one of the
 * boards QEMU models, so an image built for QEMU runs unchanged, and
 * QEMU referees the simulator: tests/golden/embsim.sh runs the exec
 * corpus on both, and the output and the instruction count must be the
 * same. It counts the instructions it executes and estimates their
 * cycles with the table tools/bench uses (tools/bench/cost.h); SysTick
 * and DWT's CYCCNT advance by that estimate, so a timed run gives the
 * same answer every time.
 *
 * This file is the command line. sim.h says how the rest is put
 * together; docs/manual/tools/embsim.md is the user's reference and
 * docs/internals/embsim.md the contributor's. The C is ISO C99 with no
 * dependencies, so it builds on a machine with no QEMU (the portable-host
 * goal). */
#include <stdlib.h>
#include <string.h>

#include "sim.h"

static u32 parse_size(const char *s)
{
    char *e;
    unsigned long v = strtoul(s, &e, 0);
    if (*e == 'K' || *e == 'k')
        v <<= 10, e++;
    else if (*e == 'M' || *e == 'm')
        v <<= 20, e++;
    if (*e || e == s)
        die("bad size '%s'", s);
    return (u32)v;
}

static void usage(void)
{
    fputs("usage: embsim IMAGE.elf [--board NAME] [--cpu NAME]\n"
          "              [--ram-size SIZE] [--until STRING] [--max-insns N]\n"
          "              [--stats] [--count FILE] [--trace FILE]\n"
          "              [--no-semihosting]\n"
          "boards: lm3s6965evb (default), mps2-an385, mps2-an386,\n"
          "        mps2-an500, microbit\n"
          "cpus:   cortex-m0, cortex-m0plus, cortex-m3, cortex-m4, cortex-m7\n",
          stderr);
    exit(2);
}

static struct sim sim;

int main(int argc, char **argv)
{
    const char *image = 0, *cpu = 0, *count_path = 0, *trace_path = 0;
    const char *until = 0;
    u32 ram_size = 0;
    u64 max_insns = 0;
    int stats = 0, verbose = 0, semihosting = 1;
    const struct board_desc *bd = &boards[0];
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        int more = i + 1 < argc;
        if (!strcmp(a, "--board") && more) {
            const char *nm = argv[++i];
            bd = board_find(nm);
            if (!bd)
                die("unknown board '%s'", nm);
        } else if (!strcmp(a, "--cpu") && more)
            cpu = argv[++i];
        else if (!strcmp(a, "--ram-size") && more)
            ram_size = parse_size(argv[++i]);
        else if (!strcmp(a, "--until") && more) {
            until = argv[++i];
            if (!*until)
                until = 0;
        } else if (!strcmp(a, "--max-insns") && more)
            max_insns = strtoull(argv[++i], 0, 0);
        else if (!strcmp(a, "--count") && more)
            count_path = argv[++i];
        else if (!strcmp(a, "--trace") && more)
            trace_path = argv[++i];
        else if (!strcmp(a, "--stats"))
            stats = 1;
        else if (!strcmp(a, "--verbose") || !strcmp(a, "-v"))
            verbose = 1;
        else if (!strcmp(a, "--no-semihosting"))
            semihosting = 0;
        else if (!strcmp(a, "--help") || !strcmp(a, "-h"))
            usage();
        else if (a[0] == '-')
            die("unknown option '%s'", a);
        else if (!image)
            image = a;
        else
            die("one image at a time ('%s' and '%s')", image, a);
    }
    if (!image)
        usage();

    struct sim *s = &sim;
    sim_init(s, bd, cpu);
    s->until = until;
    s->until_len = until ? strlen(until) : 0;
    s->max_insns = max_insns;
    s->semihosting = semihosting;
    if (trace_path)
        trace_open(s, trace_path);
    sim_load(s, ram_size, image);

    sim_run(s);
    trace_report(s, count_path, stats || verbose);
    switch (s->state) {
    case END_EXIT: return s->exit_status;
    case END_LOCKUP: return 3;
    case END_BUDGET: return 4;
    default: return 0;
    }
}
