/* embsim -- an instruction-set simulator: run a firmware image without a
 * board.
 *
 * It runs an ELF image the way the part does -- a Cortex-M, a RISC-V
 * core or an AVR -- on a model of one of the boards QEMU models, so an
 * image built for QEMU runs unchanged, and QEMU referees the simulator:
 * tests/golden/embsim.sh, embsim-riscv.sh and embsim-avr.sh run the exec
 * corpus on both, and the output and the instruction count must be the
 * same. It
 * counts the instructions it executes and estimates their cycles with
 * the table tools/bench uses (tools/bench/cost.h) -- on the AVR, exactly
 * -- and the timers (SysTick, DWT's CYCCNT, the CLINT's mtime, the AVR's
 * Timer/Counter1) advance by it, so a timed run gives the same answer
 * every time.
 *
 * This file is the command line. sim.h says how the rest is put
 * together; docs/manual/tools/embsim.md is the user's reference and
 * docs/internals/embsim.md the contributor's. The C is ISO C99 with no
 * dependencies, so it builds on a machine with no QEMU (the portable-host
 * goal). */
#include <stdlib.h>
#include <string.h>

#include "analysis.h"
#include "sim.h"
#include "svd-map.h"

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
          "              [--no-semihosting] [--gdb [HOST:]PORT [--gdb-wait]]\n"
          "              [--svd FILE.svd] [--trace-periph[=NAME,...]]\n"
          "              [--coverage FILE [--coverage-format=text|lcov]]\n"
          "       embsim --svd FILE.svd --svd-map\n"
          "boards: lm3s6965evb (default), mps2-an385, mps2-an386,\n"
          "        mps2-an500, microbit, stm32f405 (its SVD: --svd or\n"
          "        EMBSIM_SVD_PATH), virt (RISC-V), uno (AVR)\n"
          "cpus:   cortex-m0, cortex-m0plus, cortex-m3, cortex-m4, cortex-m7;\n"
          "        rv32, rv64 (virt's default: the image's width); atmega328p\n",
          stderr);
    exit(2);
}

static struct sim sim;

int main(int argc, char **argv)
{
    const char *image = 0, *cpu = 0, *count_path = 0, *trace_path = 0;
    const char *until = 0, *gdb = 0, *svd = 0, *trace_periph = 0;
    const char *coverage = 0;
    int svd_map = 0, tracing_periph = 0, board_given = 0, cov_lcov = 0;
    u32 ram_size = 0;
    u64 max_insns = 0;
    int stats = 0, verbose = 0, semihosting = 1, gdb_wait = 0;
    const struct board_desc *bd = &boards[0];
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        int more = i + 1 < argc;
        if (!strcmp(a, "--board") && more) {
            const char *nm = argv[++i];
            bd = board_find(nm);
            if (!bd)
                die("unknown board '%s'", nm);
            board_given = 1;
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
        else if (!strcmp(a, "--gdb") && more)
            gdb = argv[++i];
        else if (!strcmp(a, "--gdb-wait"))
            gdb_wait = 1;
        else if (!strcmp(a, "--svd") && more)
            svd = argv[++i];
        else if (!strcmp(a, "--svd-map"))
            svd_map = 1;
        else if (!strcmp(a, "--trace-periph"))
            tracing_periph = 1, trace_periph = 0;
        else if (!strncmp(a, "--trace-periph=", 15))
            tracing_periph = 1, trace_periph = a + 15;
        else if (!strcmp(a, "--coverage") && more)
            coverage = argv[++i];
        else if (!strncmp(a, "--coverage-format=", 18)) {
            if (!strcmp(a + 18, "lcov"))
                cov_lcov = 1;
            else if (!strcmp(a + 18, "text"))
                cov_lcov = 0;
            else
                die("--coverage-format is text or lcov, not '%s'", a + 18);
        } else if (!strcmp(a, "--help") || !strcmp(a, "-h"))
            usage();
        else if (a[0] == '-')
            die("unknown option '%s'", a);
        else if (!image)
            image = a;
        else
            die("one image at a time ('%s' and '%s')", image, a);
    }
    if (svd && !board_given) {
        /* --svd STM32F405.svd is the stm32f405's */
        const char *b = strrchr(svd, '/');
        b = b ? b + 1 : svd;
        for (int i = 0; i < nboards; i++) {
            const char *n = boards[i].svd;
            size_t k = 0;
            while (n && n[k] && b[k] &&
                   (n[k] | 0x20) == (b[k] | 0x20))
                k++;
            if (n && !n[k] && !b[k])
                bd = &boards[i];
        }
    }
    if (svd_map) {
        if (!svd && !bd->svd)
            die("--svd-map needs --svd FILE, or a board with an SVD");
        sim_init(&sim, bd, cpu, svd);
        svdmap_print(sim.svd, stdout);
        return 0;
    }
    if (!image)
        usage();
    char host[256];
    int port = 0;
    if (gdb) {
        /* PORT, HOST:PORT, or QEMU's tcp::PORT */
        const char *c = strrchr(gdb, ':');
        char *e;
        long v = strtol(c ? c + 1 : gdb, &e, 10);
        if (*e || v <= 0 || v > 65535)
            die("bad port in --gdb '%s'", gdb);
        port = (int)v;
        host[0] = 0;
        if (c) {
            const char *h = !strncmp(gdb, "tcp:", 4) ? gdb + 4 : gdb;
            size_t n = (size_t)(c - h);
            if (n >= sizeof host)
                die("bad host in --gdb '%s'", gdb);
            memcpy(host, h, n);
            host[n] = 0;
        }
    } else if (gdb_wait)
        die("--gdb-wait needs --gdb PORT");

    struct sim *s = &sim;
    sim_init(s, bd, cpu, svd);
    if (tracing_periph) {
        if (!s->svd)
            die("--trace-periph needs --svd FILE: the names are the SVD's");
        svdmap_trace(s->svd, trace_periph);
    }
    s->until = until;
    s->until_len = until ? strlen(until) : 0;
    s->max_insns = max_insns;
    s->semihosting = semihosting;
    if (trace_path)
        trace_open(s, trace_path);
    sim_load(s, ram_size, image);
    if (coverage) {
        struct analysis *an = an_create(s, 0);
        an->cov_path = coverage;
        an->cov_lcov = cov_lcov;
        cov_init(an);
    }

    if (gdb)
        gdb_serve(s, host[0] ? host : 0, port, gdb_wait);
    else
        sim_run(s);
    trace_report(s, count_path, stats || verbose);
    if (s->an)
        an_finish(s);
    switch (s->state) {
    case END_EXIT: return s->exit_status;
    case END_LOCKUP: return 3;
    case END_BUDGET: return 4;
    default: return 0;
    }
}
