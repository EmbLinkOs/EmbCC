/* embld — the standalone EmbLD driver.
 *
 * A thin argv wrapper over src/link/. The linker is a library so the
 * compiler can call it in-process (ARCHITECTURE §1: the target has no
 * fork/exec); this binary exists for host-side development and testing,
 * the way `embread` gives the ELF writer a testable front door.
 *
 * usage: embld [-o OUT] [-e ENTRY] [-Ttext ADDR] [-Tstack ADDR]
 *              [-T SCRIPT [-L DIR]... [--orphan-handling=MODE]] [-u SYM]...
 *              [--gc-sections [--print-gc-sections]] [-Map FILE]
 *              [--print-memory-usage]
 *              [--cmse-implib [--out-implib=FILE]]
 *              [--embx [--cap NAME]...] INPUT.o|INPUT.a ...
 *        embld --doctor INPUT.o|INPUT.a ...
 *
 * --doctor links nothing: it reads the inputs and says why a link over
 * them would fail, every undefined symbol at once, with the cause the
 * symbol tables reveal (docs/manual/diagnostics.md T6).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/link/link.h"
#include "doctor.h"
#include "../../src/embx/embx.h"

int main(int argc, char **argv)
{
    const char *out = "a.out";
    struct link_opts opts;
    const char *inputs[256];
    const char *libdirs[64], *undefs[64];
    int ninputs = 0, nlibdirs = 0, nundefs = 0;
    int doctor = 0;

    memset(&opts, 0, sizeof opts);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0) {
            if (++i == argc) { fprintf(stderr, "embld: -o needs a file\n"); return 2; }
            out = argv[i];
        } else if (strcmp(argv[i], "-e") == 0) {
            if (++i == argc) { fprintf(stderr, "embld: -e needs a symbol\n"); return 2; }
            opts.entry = argv[i];
        } else if (strncmp(argv[i], "-Ttext", 6) == 0) {
            const char *v = argv[i][6] ? argv[i] + 6
                                       : (++i < argc ? argv[i] : NULL);
            if (!v) { fprintf(stderr, "embld: -Ttext needs an address\n"); return 2; }
            opts.base = strtoul(v, NULL, 0);
            opts.have_base = 1;        /* `-Ttext 0` is a real request */
        } else if (strncmp(argv[i], "--lma-offset", 12) == 0) {
            /* L2: p_paddr = p_vaddr - OFFSET, for a higher-half kernel (OFFSET =
             * KERNEL_VIRTUAL_BASE). Accepts --lma-offset=HEX or a separate arg. */
            const char *v = argv[i][12] == '=' ? argv[i] + 13
                          : (++i < argc ? argv[i] : NULL);
            if (!v) { fprintf(stderr, "embld: --lma-offset needs a value\n"); return 2; }
            opts.lma_offset = strtoull(v, NULL, 0);
        } else if (strncmp(argv[i], "-Tdata", 6) == 0) {
            /* A FIRMWARE layout: the writable segment is addressed at
             * this address (RAM) and stored right after the text
             * (flash), and the linker provides __data_load /
             * __data_start / __data_end / __bss_start / __bss_end so a
             * plain C startup can copy and zero. Spelled -Tdata because
             * that is what every other linker calls it. */
            const char *v = argv[i][6] ? argv[i] + 6
                                       : (++i < argc ? argv[i] : NULL);
            if (!v) { fprintf(stderr, "embld: -Tdata needs an address\n"); return 2; }
            opts.data_base = strtoul(v, NULL, 0);
        } else if (strncmp(argv[i], "--rom-limit", 11) == 0) {
            /* The part's flash size: an image that does not fit is refused
             * (link.h). --rom-limit=N or --rom-limit N. */
            const char *v = argv[i][11] == '=' ? argv[i] + 12
                          : (++i < argc ? argv[i] : NULL);
            if (!v) { fprintf(stderr, "embld: --rom-limit needs a size\n"); return 2; }
            opts.rom_limit = strtoul(v, NULL, 0);
        } else if (strncmp(argv[i], "-Tstack", 7) == 0) {
            /* RISC-V only: the initial stack pointer, and with it a
             * four-instruction entry stub that sets sp and jumps to the
             * entry symbol. A Cortex-M gets this from its hardware --
             * the processor reads sp out of the first word of the image
             * -- and RISC-V has nothing equivalent, so without it the
             * first prologue subtracts from a stack pointer of zero. */
            const char *v = argv[i][7] ? argv[i] + 7
                                       : (++i < argc ? argv[i] : NULL);
            if (!v) { fprintf(stderr, "embld: -Tstack needs an address\n"); return 2; }
            opts.stack_top = strtoul(v, NULL, 0);
            opts.have_stack = 1;
        } else if (strncmp(argv[i], "--csa", 5) == 0) {
            /* TriCore: the context-save areas the -Tstack stub links into
             * the free list, START:END (link.h). */
            const char *v = argv[i][5] == '=' ? argv[i] + 6
                          : (++i < argc ? argv[i] : NULL);
            char *colon;
            if (!v || !(colon = strchr(v, ':'))) {
                fprintf(stderr, "embld: --csa needs START:END\n");
                return 2;
            }
            opts.csa_start = strtoul(v, NULL, 0);
            opts.csa_end = strtoul(colon + 1, NULL, 0);
            opts.have_csa = 1;
        } else if ((strncmp(argv[i], "-T", 2) == 0 &&
                    strncmp(argv[i], "-Tbss", 5) != 0) ||
                   strncmp(argv[i], "--script", 8) == 0) {
            /* -T SCRIPT, -TSCRIPT, --script=SCRIPT, --script SCRIPT; the
             * -Ttext/-Tdata/-Tstack spellings were taken above */
            const char *v = argv[i][1] == 'T'
                ? (argv[i][2] ? argv[i] + 2 : (++i < argc ? argv[i] : NULL))
                : (argv[i][8] == '=' ? argv[i] + 9
                                     : (++i < argc ? argv[i] : NULL));
            if (!v) { fprintf(stderr, "embld: -T needs a linker script\n"); return 2; }
            if (opts.script) { fprintf(stderr, "embld: only one linker script (-T) per link\n"); return 2; }
            opts.script = v;
        } else if (strncmp(argv[i], "-L", 2) == 0) {
            const char *v = argv[i][2] ? argv[i] + 2
                                       : (++i < argc ? argv[i] : NULL);
            if (!v) { fprintf(stderr, "embld: -L needs a directory\n"); return 2; }
            if (nlibdirs == 64) { fprintf(stderr, "embld: too many -L\n"); return 2; }
            libdirs[nlibdirs++] = v;
        } else if (strcmp(argv[i], "-u") == 0 ||
                   strncmp(argv[i], "--undefined", 11) == 0) {
            const char *v = argv[i][1] == 'u'
                ? (++i < argc ? argv[i] : NULL)
                : (argv[i][11] == '=' ? argv[i] + 12
                                      : (++i < argc ? argv[i] : NULL));
            if (!v) { fprintf(stderr, "embld: -u needs a symbol\n"); return 2; }
            if (nundefs == 64) { fprintf(stderr, "embld: too many -u\n"); return 2; }
            undefs[nundefs++] = v;
        } else if (strncmp(argv[i], "--orphan-handling=", 18) == 0) {
            const char *v = argv[i] + 18;
            if (!strcmp(v, "place")) opts.orphan_mode = 0;
            else if (!strcmp(v, "warn")) opts.orphan_mode = 1;
            else if (!strcmp(v, "error")) opts.orphan_mode = 2;
            else { fprintf(stderr, "embld: --orphan-handling is place, warn or error\n"); return 2; }
        } else if (strcmp(argv[i], "--gc-sections") == 0) {
            opts.gc_sections = 1;
        } else if (strcmp(argv[i], "--no-gc-sections") == 0) {
            opts.gc_sections = 0;
        } else if (strcmp(argv[i], "--print-gc-sections") == 0) {
            opts.print_gc_sections = 1;
        } else if (strcmp(argv[i], "--no-print-gc-sections") == 0) {
            opts.print_gc_sections = 0;
        } else if (strcmp(argv[i], "--print-memory-usage") == 0) {
            opts.print_memory_usage = 1;
        } else if (strcmp(argv[i], "-Map") == 0 || strcmp(argv[i], "--Map") == 0 ||
                   strncmp(argv[i], "-Map=", 5) == 0 ||
                   strncmp(argv[i], "--Map=", 6) == 0) {
            /* -Map FILE, -Map=FILE, --Map FILE, --Map=FILE, as ld takes it */
            const char *eq = strchr(argv[i], '=');
            const char *v = eq ? eq + 1 : (++i < argc ? argv[i] : NULL);
            if (!v || !*v) { fprintf(stderr, "embld: -Map needs a file\n"); return 2; }
            opts.map_file = v;
        } else if (strcmp(argv[i], "--embx") == 0) {
            opts.emit_embx = 1;            /* write a native EMBX, not ELF */
        } else if (strcmp(argv[i], "--cap") == 0) {
            if (++i == argc) { fprintf(stderr, "embld: --cap needs a name\n"); return 2; }
            int id = embx_cap_id(argv[i]);
            if (id <= 0) { fprintf(stderr, "embld: unknown capability '%s'\n", argv[i]); return 2; }
            opts.caps |= (1ULL << id);     /* declare it in the EMBX cap table */
        } else if (strcmp(argv[i], "--cmse-implib") == 0) {
            opts.cmse_implib = 1;          /* ARMv8-M: see link.h */
        } else if (strncmp(argv[i], "--out-implib", 12) == 0) {
            const char *v = argv[i][12] == '=' ? argv[i] + 13
                          : (++i < argc ? argv[i] : NULL);
            if (!v || !*v) { fprintf(stderr, "embld: --out-implib needs a file\n"); return 2; }
            opts.out_implib = v;
        } else if (strncmp(argv[i], "--in-implib", 11) == 0) {
            fprintf(stderr, "embld: --in-implib is not supported: it keeps "
                    "each secure gateway veneer at the address an earlier "
                    "import library gave it, and this linker lays the "
                    "veneers out in name order every link\n");
            return 2;
        } else if (strcmp(argv[i], "--doctor") == 0) {
            doctor = 1;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "embld: unknown option '%s'\n", argv[i]);
            return 2;
        } else {
            if (ninputs == 256) { fprintf(stderr, "embld: too many inputs\n"); return 2; }
            inputs[ninputs++] = argv[i];
        }
    }
    if (!ninputs) {
        fprintf(stderr, "usage: embld [-o OUT] [-e ENTRY] [-Ttext ADDR] [-Tstack ADDR]\n"
                        "             [-T SCRIPT [-L DIR]... [--orphan-handling=place|warn|error]]\n"
                        "             [--gc-sections [--print-gc-sections]] [-Map FILE]\n"
                        "             [--print-memory-usage] [--cmse-implib [--out-implib=FILE]]\n"
                        "             [--embx [--cap NAME]...] INPUT.o|INPUT.a ...\n"
                        "       embld --doctor INPUT.o|INPUT.a ...   "
                        "(why the link fails)\n");
        return 2;
    }
    opts.libdirs = libdirs;
    opts.nlibdirs = nlibdirs;
    opts.undefs = undefs;
    opts.nundefs = nundefs;
    if (doctor)
        return doctor_run((char **)inputs, ninputs);
    return embld_link(inputs, ninputs, out, &opts);
}
