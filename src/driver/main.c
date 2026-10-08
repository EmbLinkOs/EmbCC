/* embcc driver: argv, flags, orchestration (ARCHITECTURE.md §2).
 *
 * M1 surface: -c compiles one file of the M1 subset to a relocatable
 * object; linking stays with the existing toolchain until the integrated
 * linker (M3). Everything the compiler cannot do fails loudly with a
 * diagnostic naming the milestone that brings it (THE RULE).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../arch/x86_64/emit.h"
#include "../arch/x86_64/topasm.h"
#include "../arch/backend.h"
#include "../cpp/cpp.h"
#include "../cxx/translate.h"
#include "../debug/dwarf.h"
#include "../debug/eh.h"
#include "../arch/predef.h"
#include "../arch/x86_64/as.h"
#include "../elf/write.h"
#include "../macho/write.h"
#include "../coff/write.h"
#include "../ir/ir.h"
#include "../arch/thumb/attrs.h"
#include "../arch/thumb/emit.h"
#include "../opt/opt.h"
#include "../parse/parse.h"
#include "../sema/sema.h"
#include "../arch/target.h"
#include <setjmp.h>

#include "asmout.h"
#include "iface.h"
#include "inspect.h"
#include "remark.h"
#include "util.h"
#include "../platform/platform.h"

#include "version.h"
#include "paths.h"
#include "../link/link.h"
#include "../as/gas.h"

static void print_version(void)
{
    /* What the compiler is and what it targets, in a few lines, in the
     * layout clang uses -- and only what the tree holds: the long form,
     * with what is missing, is docs/internals/status.md. */
    const char *dt = target_default_name();
    printf("EmbCC %s (the Emb toolchain's C and C++ compiler, for "
           "embedded systems and operating systems)\n", EMBCC_VERSION);
    printf("Target: %s\n", target_triple_now());
    printf("Default target: %s%s\n", dt ? dt : "x86_64-elf",
           dt ? " (as configured)" : "");
    printf("Targets: Cortex-M (ARMv6-M, ARMv7-M, ARMv7E-M, ARMv8-M "
           "Baseline and Mainline), ARMv7-A, RISC-V (RV32, RV64),\n"
           "AVR (ATmega), Xtensa (ESP32, windowed ABI), TriCore, Renesas "
           "RX, ColdFire (m68k, ISA_A), MIPS32 and MIPS64\n"
           "(both byte orders), PowerPC (32-bit EABI), SPARC V8 (LEON3), "
           "LoongArch64 (LP64S); x86-64 and AArch64\n"
           "(bare metal, EmbLinkOS, Linux, Darwin; x86-64 also Windows)\n");
    printf("Languages: C11 with the GNU extensions; C++ toward C++20 on "
           "every target but AVR\n");
    printf("Linker: embld, for every target's images; AArch64 and Darwin "
           "link with the platform's linker\n");
    printf("Not yet: position-independent executables, shared libraries, "
           "dynamic linking. See docs/internals/status.md.\n");
}

static void print_usage(FILE *out)
{
    fprintf(out,
            "usage: embcc [-c|-S|-E] FILE.c|FILE.cc|FILE.s|FILE.S|FILE.asm..."
            " [-o FILE] [-j N]\n"
            "             [--target=TRIPLE] [-x c|c++]\n"
            "             [-std=...] [--emit-c]\n"
            "             [-I DIR]... [-isystem DIR]... [-nostdinc] [-g]\n"
            "             [-O0|-O1|-O2|-O3|-Os]\n"
            "             [-mcpu=CPU] [-mno-sse] [-mno-red-zone] ...\n"
            "       embcc [FILE] OBJ.o|LIB.a|-lLIB... [-L DIR] [-T SCRIPT.ld]\n"
            "             [-nostdlib|-nodefaultlibs|-nostartfiles] -o OUT"
            "   (link)\n"
            "       embcc --version | --dump-predef"
            " | --emit-empty-object FILE\n");
}

/* What --help prints under the usage line: the options EmbCC takes, in the
 * spellings GCC and Clang use, so a build system's flags land as expected. */
static void print_options(FILE *out)
{
    fputs(
      "\nwhat to do\n"
      "  -c                     compile to an object\n"
      "  -E                     preprocess only\n"
      "  -S                     write assembly (NAME.s) instead of an object\n"
      "  -fsyntax-only          check, write nothing\n"
      "  -j N, -jN, -j          several sources: compile N at once (-j: one\n"
      "                         per processor); one at a time by default\n"
      "  --emit-c               print the C a C++ unit lowers to\n"
      "  -o FILE                where to write it\n"
      "\nthe language\n"
      "  -x c|c++               treat the input as this language\n"
      "  -std=...               accepted; EmbCC has one dialect per language\n"
      "  -I DIR, -isystem DIR   header search paths\n"
      "  -nostdinc              do not search EmbCC's own headers\n"
      "  --print-search-dirs    where EmbCC found its own files\n"
      "  -D NAME[=VALUE], -U NAME  define and undefine macros\n"
      "  -include FILE          include it before the file\n"
      "  -Wp,-D...,-U...,-I...  the same, for the preprocessor\n"
      "  -O0/-O1/-O2/-O3/-Os          optimization level (-Os: no size growth)\n"
      "  -f<pass>, -fno-<pass>        turn one optimizer pass on or off\n"
      "  -fstack-usage                write FILE.su: each function's frame\n"
      "  -fno-exceptions, -fno-rtti   C++ without them\n"
      "  -fno-access-control          do not enforce private/protected\n"
      "  -fno-jump-tables             no switch through a table of addresses\n"
      "  -fno-inline-functions        inline only what is declared inline\n"
      "  -fcommon                     tentative definitions are COMMON (C, ELF)\n"
      "  -fsingle-precision-constant  1.0 is a float (C)\n"
      "  -save-temps[=cwd|obj]        keep the .i and the .s\n"
      "\nthe target\n"
      "  --target=TRIPLE        x86_64-elf, aarch64-elf, thumbv7m-none-eabi,\n"
      "                         thumbv7em-none-eabi[hf], thumbv8m.main-none-eabi[hf],\n"
      "                         armv7a-none-eabi, riscv32/riscv64-unknown-elf, avr,\n",
      out);
    /* (two literals: one string this long passes the IR's text form's
     * limit, and tests/golden/ir-roundtrip.sh prints main.c's) */
    fputs(
      "                         thumbv6m-none-eabi, thumbv8m.base-none-eabi,\n"
      "                         mipsel-none-elf, mips-none-elf,\n"
      "                         loongarch64-unknown-elf, xtensa-none-elf,\n"
      "                         powerpc-none-eabi, rx-none-elf, sparc-none-elf,\n"
      "                         m68k-none-elf,\n"
      "                         and the\n"
      "                         -emblink, -linux-gnu, -apple-darwin and\n"
      "                         -windows-gnu spellings; an unknown one lists\n"
      "                         them all\n",
      out);
    fprintf(out,
      "                         with none, this compiler emits for %s\n",
      target_default_name() ? target_default_name() : "x86_64-elf");
    fputs(
      "  -dumpmachine           print that target\n"
      "  -g                     debug information (DWARF)\n"
      "  -mcpu=CPU -mfpu=FPU -mfloat-abi=ABI   Cortex-M part and float ABI\n"
      "  -mno-sse -mno-red-zone -mcmodel=kernel -mgeneral-regs-only\n"
      "\ndiagnostics (docs/manual/diagnostics.md)\n"
      "  -fdiagnostics-format=text|json   caret output, or GCC's JSON\n"
      "  -fdiagnostics-color=auto|always|never\n"
      "  -fmax-errors=N         stop after N\n"
      "  -w                     no warnings;  -Werror  warnings are errors\n"
      "  -Wall, -Wextra, -Wname, -Wno-name (see --help-warnings)\n"
      "\nthe link (docs/manual/invoking.md#linking)\n"
      "  OBJ.o, LIB.a           linked as they are, with the sources if any\n"
      "  -lNAME, -L DIR         libNAME.a from the -L directories\n"
      "  -T SCRIPT.ld           GNU ld linker script (ARM and RISC-V firmware)\n"
      "  -nostdlib, -nodefaultlibs, -nostartfiles   leave out crt1/libc/librt\n"
      "  -e SYM, -u SYM         entry symbol; a symbol to pull from archives\n"
      "  -Wl,ARGS, -Xlinker ARG options for EmbLD (-Ttext, -Tdata, ...)\n"
      "\ndependencies\n"
      "  -M, -MM                write the make rule instead of compiling\n"
      "  -MD, -MMD              write it beside the object\n"
      "  -MF FILE, -MT TARGET, -MP\n"
      "\nreporting\n"
      "  --version, --help, --dump-predef\n"
      "  --explain ID           what a diagnostic means, and the fix\n"
      "  --fix                  apply the fix-its it proposes\n", out);
}

/* Where this target's runtime and C library live, under the library
 * directory: the triple's own name, or for a RISC-V hardware-float ABI a
 * directory below it named for the ABI (riscv32-unknown-elf/ilp32f) --
 * GCC's multilib arrangement. The ABI and not the -march: an ilp32f
 * library is the same calls with rv32imafc or rv32imafdc, and objects of
 * two float ABIs do not link (embld refuses them). */
static const char *lib_triple(void)
{
    static char buf[128];
    int abi = target_riscv_abi_flen();
    if (!abi)
        return target_triple_now();
    snprintf(buf, sizeof buf, "%s/%s%s", target_triple_now(),
             target_get() == TARGET_RISCV64 ? "lp64" : "ilp32",
             abi == 64 ? "d" : "f");
    return buf;
}

static void dump_predef(void)
{
    int n;
    const struct predef_macro *tab = predef_table(&n);
    for (int i = 0; i < n; i++)
        printf("#define %s %s\n", tab[i].name, tab[i].value);
}

/* The object format a target needs, against the writers that exist.
 *
 * D-014 adds Mach-O and COFF targets to the triple table before their
 * writers are built, which is deliberate: the triple, the predefined
 * macros and `-E` are useful while the writer is being written, and
 * every one of them can be developed and tested without it. What must
 * NOT happen is an ELF file written for a Darwin target and named .o as
 * though it were right -- so this is the one place that says no, and it
 * says which triple and which format (THE RULE). */
/* The four ELF visibilities, by the names the attribute uses. The
 * parser has already refused anything else, so an unrecognised string
 * cannot reach here. */
static int stv_of(const char *v)
{
    if (!strcmp(v, "hidden"))    return STV_HIDDEN;
    if (!strcmp(v, "internal"))  return STV_INTERNAL;
    if (!strcmp(v, "protected")) return STV_PROTECTED;
    return STV_DEFAULT;
}

/* Mach-O's section alignment is a power-of-two EXPONENT. */
static int log2_align(int a)
{
    int n = 0;
    while ((1 << n) < a && n < 14)
        n++;
    return n;
}

static int object_format_ready(void)
{
    if (target_fmt_get() == TGT_FMT_ELF || target_fmt_get() == TGT_FMT_COFF)
        return 1;
    fprintf(stderr,
            "embcc: error: no object writer for %s yet, which is what "
            "'%s' needs\n",
            target_fmt_name(target_fmt_get()), target_triple_now());
    fprintf(stderr,
            "embcc: the triple, its predefined macros and -E work today; "
            "emitting objects for it does not (D-014)\n");
    return 0;
}

/* An empty but genuine relocatable object: the smallest output readelf,
 * objdump and the cross ld all accept. Kept from M0 so the writer stays
 * testable independently of the compiler. */
static int emit_empty_object(const char *path)
{
    if (!object_format_ready())
        return 1;
    struct elfw *w = elfw_new(target_elf_machine(target_get()));
    elfw_set_flags(w, target_elf_flags(target_get()));
    int text = elfw_add_section(w, ".text", SHT_PROGBITS,
                                SHF_ALLOC | SHF_EXECINSTR, NULL, 0, 16);
    elfw_add_symbol(w, "empty.c", 0, 0,
                    ELF64_ST_INFO(STB_LOCAL, STT_FILE), SHN_ABS);
    elfw_add_symbol(w, "", 0, 0,
                    ELF64_ST_INFO(STB_LOCAL, STT_SECTION), (Elf64_Half)text);
    int rc = elfw_write(w, path);
    elfw_free(w);
    return rc == 0 ? 0 : 1;
}

static char *read_file(const char *path)
{
    /* `-`: standard input, as `echo | cc -E -dM -` reads it */
    if (strcmp(path, "-") == 0) {
        size_t cap = 4096, n = 0, r;
        char *b = xmalloc(cap);
        while ((r = fread(b + n, 1, cap - n - 1, stdin)) > 0) {
            n += r;
            if (cap - n < 2)
                b = xrealloc(b, cap *= 2);
        }
        b[n] = 0;
        return b;
    }
    char *buf = src_read(path, NULL);   /* a source: the provider may own it */
    if (!buf)
        diag_fatal(path, 0, "cannot open file");
    return buf;
}

/* EmbIR's own textual form — an input only to `inspect ir`, which parses
 * and reprints it (the §9.1 round-trip). */
static int has_ir_suffix(const char *p)
{
    size_t n = strlen(p);
    return n > 3 && !strcmp(p + n - 3, ".ir");
}

/* The input with its suffix (.c, .cc, .cpp ...) swapped for .o. */
/* The output name GCC gives a compile with no -o: the input's last
 * path component, its suffix replaced by `sfx` (".o", ".s"), in the
 * current directory. The directory was kept, so `-c src/a.c` wrote
 * src/a.o where GCC writes ./a.o, and a dot in a directory name
 * (`v1.2/main`) was taken for the suffix. */
static const char *default_output_sfx(const char *in, const char *sfx)
{
    const char *base = strrchr(in, '/');
    base = base ? base + 1 : in;
    const char *dot = strrchr(base, '.');
    size_t n = dot && dot != base ? (size_t)(dot - base) : strlen(base);
    char *out = xmalloc(n + strlen(sfx) + 1);
    memcpy(out, base, n);
    strcpy(out + n, sfx);
    return out;
}

static const char *default_output(const char *in)
{
    return default_output_sfx(in, ".o");
}

/* The final path component. The STT_FILE symbol uses this rather than the
 * path as given, so an object depends only on the source's CONTENT, not on
 * where the build ran — build-path independence, which is what lets the
 * self-hosting fixed point hold when the host compiles src/x.c and the OS
 * compiles /data/src/embcc/x.c and the two objects must be byte-identical. */
static const char *path_basename(const char *p)
{
    const char *slash = strrchr(p, '/');
    return slash ? slash + 1 : p;
}

#define MAX_INCDIRS 16
static const char *incdirs[MAX_INCDIRS];
static int incdir_sys[MAX_INCDIRS];   /* -isystem, or EmbCC's own include */
static int nincdirs;

/* -g: emit DWARF line info (D-010 step 1). Opt-in — with it off, output is
 * byte-for-byte as before, which is what keeps the M3 self-host fixed point
 * (self-host builds without -g). */
static int want_debug;
/* -fstack-usage: write FILE.su beside the object, one line per function,
 * in gcc's format (`file:line:name<TAB>bytes<TAB>qualifier`) so the
 * tools that already read those files read these. It is the number a
 * microcontroller's stack has to be sized from, since there is no guard
 * page to catch an overflow and nothing to grow into. */
static int want_stack_usage;

/* -fcallgraph-info[=su]: write FILE.ci beside the object, the call graph
 * in GCC's format (a VCG graph: a node per function, with its frame under
 * =su, an edge per call site, and calls through a pointer to the
 * __indirect_call placeholder), so the stack analysers written for GCC's
 * files -- and embrt -- read EmbCC's. */
static int want_callgraph, callgraph_su;

/* -ffunction-sections / -fdata-sections: each function in .text.NAME,
 * each object in .data.NAME, .rodata.NAME or .bss.NAME, as GCC names
 * them, so a linker's --gc-sections can drop what nothing reaches. ELF
 * output only; the others have no sections to split into. */
static int func_sections, data_sections;

/* PFX followed by NAME, allocated */
static char *sec_named(const char *pfx, const char *name)
{
    size_t a = strlen(pfx), b = strlen(name);
    char *r = xmalloc(a + b + 1);
    memcpy(r, pfx, a);
    memcpy(r + a, name, b + 1);
    return r;
}

/* -nostdinc: do not add EmbCC's own header directories. A freestanding
 * build that supplies its own headers needs to be able to say so. */
static int no_stdinc;

/* -O level. 0 (the default) runs no optimizer, so output is byte-for-byte
 * as before — the property the self-host fixed point rests on. */
static int opt_level;
/* -Os: the optimizer wants to know (it drops vectorization), and
 * codegen must NOT -- it reads opt_level for register allocation and
 * tail calls, and a negative level turned both off, which made -Os
 * emit nearly twice the code of -O2. So the size request travels
 * separately and opt_level stays an ordinary number. */
static int opt_for_size;

/* -mno-sse: never emit an SSE/xmm instruction (no varargs xmm spill, no SSE
 * struct/float lowering). A kernel built before it enables CR4.OSFXSR needs
 * this — any SSE op faults with #UD. Off by default, so ordinary output is
 * unchanged. */
static int no_sse;

/* The source is C++ (-x c++, or a C++ suffix): it is lowered to C first. */
static int lang_cxx;

/* Unwind tables (.eh_frame), so a C++ exception can unwind through the
 * unit's functions: always for C++; for C on -funwind-tables,
 * -fasynchronous-unwind-tables or -fexceptions (-1: not asked either
 * way), off by default so C output stays as it was. */
static int want_unwind = -1;

/* C++ exceptions (-fno-exceptions turns them off, as g++'s). */
static int want_exceptions = 1;
static int want_rtti = 1;

/* --emit-c: print the C a C++ unit lowers to, instead of compiling it. */
static int emit_c_only;

/* --fix: apply the fix-its instead of only printing them. */
static int want_fix;

/* -fsyntax-only: run the front end, write nothing. What an editor asks for
 * (docs/manual/diagnostics.md T5) and what a build's "does this still compile" step
 * wants. */
static int syntax_only;

/* -ftime-report, as GCC and clang spell it: where a compile's time went,
 * phase by phase, on stderr at exit. clock() is processor time and ISO C,
 * so it means the same on every host EmbCC runs on. A phase is marked
 * when it ENDS; what was not marked by the end (writing the object, the
 * link) is reported as the rest. */
static int g_time_report;
static clock_t g_time_start, g_time_last;
static struct { const char *name; double secs; } g_phase[12];
static int g_nphase;
static void time_mark(const char *name)
{
    if (!g_time_report)
        return;
    clock_t now = clock();
    for (int k = 0; k < g_nphase; k++)
        if (!strcmp(g_phase[k].name, name)) {
            g_phase[k].secs += (double)(now - g_time_last) / CLOCKS_PER_SEC;
            g_time_last = now;
            return;
        }
    if (g_nphase < (int)(sizeof g_phase / sizeof g_phase[0])) {
        g_phase[g_nphase].name = name;
        g_phase[g_nphase++].secs = (double)(now - g_time_last) / CLOCKS_PER_SEC;
    }
    g_time_last = now;
}
static void time_report(void)
{
    if (!g_time_report)
        return;
    time_mark("object and the rest");
    double total = (double)(clock() - g_time_start) / CLOCKS_PER_SEC;
    fprintf(stderr, "\nExecution times (seconds, processor time)\n");
    for (int k = 0; k < g_nphase; k++)
        fprintf(stderr, " %-22s: %8.3f (%3.0f%%)\n", g_phase[k].name,
                g_phase[k].secs,
                total > 0 ? 100.0 * g_phase[k].secs / total : 0.0);
    fprintf(stderr, " %-22s: %8.3f\n", "TOTAL", total);
}
/* Tool mode (§17): `embcc inspect <stage> file.c` stops the pipeline at a
 * stage and prints what it built, instead of producing an object. */
static const char *inspect_stage;
/* -S: emit the assembly the backend produced, rather than an object. */
static int want_asm;
/* -Wa,-a[cdghlmns][=FILE]: GNU as's listing, which a CubeMX Makefile asks
 * for on every compile. It is the -S text of the object's own bytes,
 * written beside the object: to FILE, or to standard output when no
 * option names one (as GNU as does). NULL: none asked for. */
static const char *g_listing;
/* --emit-interfaces: the USRs and interface hashes of what this unit
 * provides and observes (§8.2, §21). */
static int want_iface;
/* -fremarks[=json]: what the passes decided, and why (R2, §13). Off by
 * default -- a pass that always built strings would slow every compile for
 * a report almost nobody asked for. */
static int want_remarks;
static int remarks_json;
/* `embcc why <decision> [subject] <file>`: the query layer (§19). */
static const char *why_decision, *why_subject;

/* -M and friends: the make rule naming what this file included. `dep_mode`
 * 0 none, 1 every header (-M/-MD), 2 only the ones that are not system
 * headers (-MM/-MMD); `dep_only` is the -M/-MM form, which replaces the
 * compile rather than accompanying it. */
static int dep_mode, dep_only, dep_phony;
static const char *dep_file, *dep_target;

/* -fcommon: a C tentative definition (`int x;` at file scope, nothing
 * else saying where it goes) is a COMMON symbol, which the linker merges
 * with the others of its name, instead of a .bss definition. ELF only. */
static int g_fcommon;
/* -fsingle-precision-constant: an unsuffixed floating constant is a
 * float (C only; see the check after the arguments). */
static int g_single_prec;
/* -specs=FILE / --specs=FILE: a GCC driver specs file, named so the link
 * can say once that EmbCC's own libraries are linked instead. */
static const char *g_specs;
/* -save-temps[=cwd|obj]: 1 beside the output, 2 in the current
 * directory. -dumpbase NAME names the files outright (multi_source passes
 * it to the compiler it runs for each source of a link). */
static int g_save_temps;
static const char *g_dumpbase;

/* The make rule: "target: source header...", wrapped as GCC wraps it, and
 * with -MP a bare rule per header so a deleted header does not break the
 * build. */
static void write_deps(const char *in, const char *obj)
{
    const char *target = dep_target ? dep_target : obj;
    char defname[4096];
    if (!target) {                    /* neither -MT nor -o: the input's .o */
        const char *base = strrchr(in, '/');
        base = base ? base + 1 : in;
        const char *dot = strrchr(base, '.');
        snprintf(defname, sizeof defname, "%.*s.o",
                 dot ? (int)(dot - base) : (int)strlen(base), base);
        target = defname;
    }
    /* Built, then written: one path for the bytes whether they go to a
     * file (through the platform layer) or to the console. */
    char path[4096];
    const char *dest = NULL;          /* NULL: stdout */
    if (dep_file) {
        dest = dep_file;
    } else if (!dep_only) {           /* -MD/-MMD: beside the object */
        const char *end = strrchr(target, '.');
        snprintf(path, sizeof path, "%.*s.d",
                 end ? (int)(end - target) : (int)strlen(target), target);
        dest = path;
    }
    struct outbuf b = { NULL, 0, 0 };
    int col = ob_fmt(&b, "%s: %s", target, in);
    for (int i = 0; i < cpp_dep_count(); i++) {
        if (dep_mode == 2 && cpp_dep_is_system(i))
            continue;
        const char *d = cpp_dep_path(i);
        if (col + (int)strlen(d) > 72) {
            ob_str(&b, " \\\n ");
            col = 1;
        }
        col += ob_fmt(&b, " %s", d);
    }
    ob_ch(&b, '\n');
    if (dep_phony)
        for (int i = 0; i < cpp_dep_count(); i++) {
            if (dep_mode == 2 && cpp_dep_is_system(i))
                continue;
            ob_fmt(&b, "\n%s:\n", cpp_dep_path(i));
        }
    if (!dest)
        fwrite(b.p, 1, b.n, stdout);
    else if (plat_write_file(dest, b.p, b.n) != 0)
        diag_fatal(dest, 0, "cannot write the file");
    ob_free(&b);
}

/* The boundary the libraries unwind to (util.h, R6). `embcc` is the one
 * program entitled to end the process over a failed unit, and it does that
 * here -- by returning 1 -- rather than letting a backend call exit() from
 * four frames down. The same libraries inside a language server install
 * their own boundary and keep serving. */
/* A defined function's st_value.
 *
 * On ARM the low bit of a function symbol's value is not part of the
 * address: it says the symbol names THUMB code, and every `bx`/`blx` to
 * it reads that bit to decide which instruction set to switch to. A
 * Cortex-M executes nothing but Thumb, so it is always set — and an
 * object that leaves it clear links without complaint and branches into
 * ARM state on the first indirect call, where the processor faults.
 * Checked against llvm-mc's own output, which gives `f` at offset 0 a
 * st_value of 1. */
/* Functions with a section attribute: codegen lays them out after every
 * other function, grouped by section (compile_unit sorts them there), so
 * the code buffer is [.text's functions][group][group]...[file-scope
 * asm]. Each group becomes a section of its own, and .text is the two
 * outer slices. text_at() is the one translation from a code-buffer
 * offset -- which every site, symbol and table records -- to the section
 * it lands in and the offset there. */
static struct tgroup { const char *name; long start, end; int ndx, sym,
                       align; }
    *g_tg;
static int g_ntg, g_captg;
static long g_plain_end, g_groups_end;

/* The group (1-based) holding code-buffer offset off, or 0 for .text;
 * *noff gets the offset within that section. */
static int text_at(long off, long *noff)
{
    for (int k = 0; k < g_ntg; k++)
        if (off >= g_tg[k].start && off < g_tg[k].end) {
            *noff = off - g_tg[k].start;
            return k + 1;
        }
    *noff = g_ntg && off >= g_groups_end ? off - (g_groups_end - g_plain_end)
                                         : off;
    return 0;
}

/* The function an alias names: the canonical definition, which sema
 * checked is in this file. */
static const struct func *alias_target(const struct unit *u,
                                       const struct func *f)
{
    for (const struct func *t = u->funcs; t; t = t->next)
        if (!t->absorbed && t->has_defn && strcmp(t->name, f->alias_of) == 0)
            return t;
    internal_error("alias '%s' of '%s': no definition", f->name, f->alias_of);
    return NULL;
}

/* ...and ARMv7-A in ARM state (armv7a-none-eabi) is the one ARM target
 * whose functions are A32: their symbols are even, as clang's are, and the
 * mapping symbol is `$a`. */
static int thumb_state(void)
{
    return target_get() == TARGET_THUMB && !target_arm_a32();
}

static long fn_sym_value(enum target_arch a, long code_off)
{
    return a == TARGET_THUMB && thumb_state() ? code_off | 1 : code_off;
}

/* A function's symbol: its value and section from its code-buffer offset
 * (text_at), for one in a section of its own as for one in .text. */
static long code_sym_value(enum target_arch a, long off)
{
    long o;
    text_at(off, &o);
    return fn_sym_value(a, o);
}

static int code_sec(long off, int text_ndx)
{
    long o;
    int gi = text_at(off, &o);
    return gi ? g_tg[gi - 1].ndx : text_ndx;
}

/* A relocation whose place is code-buffer offset off. */
static void code_rela(struct elfw *w, int text_ndx, long off, int sym,
                      int type, long addend)
{
    long o;
    int gi = text_at(off, &o);
    elfw_add_rela(w, gi ? g_tg[gi - 1].ndx : text_ndx, (Elf64_Addr)o, sym,
                  type, addend);
}

/* A reference to code-buffer offset off by section symbol: its section's
 * symbol in *sym, the offset there returned. */
static long code_ref(long off, int text_sym, int *sym)
{
    long o;
    int gi = text_at(off, &o);
    *sym = gi ? g_tg[gi - 1].sym : text_sym;
    return o;
}

static int compile_unit(const char *in, const char *out, int pp_only);

/* GNU C's static label data, once codegen has laid every function out
 * (cg_note_labels): `&&a` in a pointer slot is its function's symbol plus
 * the label's offset -- added into the relocation's addend, so the Thumb
 * bit, AVR's word address and every other target's spelling of a
 * function pointer apply to it as they do to `&f` -- and `&&b - &&a` is a
 * plain number written into the image. Before any writer reads either.
 *
 * On AVR a label's VALUE is a word address, as a function pointer's is,
 * so the difference of two is in words and `&&a + n` is n words on: the
 * code computes both that way, and the data must agree. */
static void resolve_label_data(struct unit *u)
{
    long unit = target_get() == TARGET_AVR ? 2 : 1;   /* bytes per value */
    for (struct global *g = u->globals; g; g = g->next) {
        /* A table in a function that was never generated (an unused
         * static one) is that function's alone, and nothing can read it:
         * its label slots stay zero rather than name a symbol that was
         * not emitted. */
        int keep = 0;
        for (int r = 0; r < g->nrelocs; r++) {
            struct greloc *rl = &g->relocs[r];
            if (rl->label && rl->ftarget && !rl->ftarget->label_pos)
                continue;
            g->relocs[keep++] = *rl;
        }
        g->nrelocs = keep;
        for (int r = 0; r < g->nrelocs; r++) {
            struct greloc *rl = &g->relocs[r];
            struct func *f = rl->ftarget;
            if (!rl->label)
                continue;
            if (!f || !f->label_pos || rl->label_slot < 0 ||
                rl->label_slot >= f->nlabel_pos ||
                f->label_pos[rl->label_slot] < 0)
                internal_error("%s: the address of label '%s' was never "
                               "placed", g->name, rl->label);
            rl->addend = rl->addend * unit + f->label_pos[rl->label_slot];
            rl->label_slot = -1;        /* once */
        }
        for (int d = 0; d < g->nldiffs; d++) {
            struct glabeldiff *ld = &g->ldiffs[d];
            struct func *f = ld->fn;
            if (f && !f->label_pos)
                continue;                 /* never generated, as above */
            if (!f || !f->label_pos || ld->slot < 0 || ld->minus_slot < 0 ||
                ld->slot >= f->nlabel_pos || ld->minus_slot >= f->nlabel_pos ||
                f->label_pos[ld->slot] < 0 || f->label_pos[ld->minus_slot] < 0)
                internal_error("%s: '&&%s - &&%s' was never placed", g->name,
                               ld->label, ld->minus);
            if (!g->init_bytes || ld->off + ld->size > g->init_len)
                internal_error("%s: '&&%s - &&%s' is outside the image",
                               g->name, ld->label, ld->minus);
            target_put_uint((unsigned char *)g->init_bytes +
                            ld->off, ld->size,
                            (unsigned long long)((f->label_pos[ld->slot] -
                                                  f->label_pos[ld->minus_slot])
                                                 / unit + ld->addend));
        }
    }
}

/* -Wl,... and -Xlinker: options for the link, kept until there is one. A
 * compile that does not link ignores them, as GCC's does. */
static const char *g_wl[128];
static int g_nwl;

/* What the link takes besides the compiled source, in command-line order:
 * objects, archives and -lNAME (kept as written, found at the link). A
 * build's link step is `$(CC) $(LDFLAGS) a.o b.o -lfoo -o fw.elf`, with no
 * source at all, and that is the line this serves. */
static const char *g_link_in[256];
static int g_nlink_in;
/* Each link input's place on the command line (argv index): the order
 * objects compiled from several sources are linked in (multi_source). */
static int g_link_argi[256];
/* Several sources (multi_source): each, and its place on the command
 * line. g_child_skip marks the argv words a child compile does not get:
 * the sources, the link inputs, -o and -j, with their values. */
#define MAX_SRCS 256
static const char *g_srcs[MAX_SRCS];
static int g_src_argi[MAX_SRCS];
static int g_nsrc;
static char *g_child_skip;
static int g_jobs = 1;
/* A C++ source is among them: the link needs libcxx.a (compile_and_link
 * asks this rather than lang_cxx, which is about one source). */
static int g_link_cxx;
static const char *g_libdirs[64];        /* -L */
static int g_nlibdirs;
static const char *g_undefs[64];         /* -u */
static int g_nundefs;
static const char *g_script;             /* -T */
static const char *g_entry;              /* -e */
static int g_nostdlib, g_nostartfiles, g_nodefaultlibs;

/* The value of linker option `opt` at g_wl[*k]: `opt=V`, `optV` where the
 * option is spelled that way (-Ttext0x8000), or the next word. NULL when
 * g_wl[*k] is not `opt`, or has no value. */
static const char *wl_value(const char *opt, int glued, int *k)
{
    const char *a = g_wl[*k];
    size_t n = strlen(opt);
    if (strncmp(a, opt, n) != 0)
        return NULL;
    if (a[n] == '=')
        return a + n + 1;
    if (a[n] && glued)
        return a + n;
    if (a[n] == '\0' && *k + 1 < g_nwl)
        return g_wl[++*k];
    return NULL;
}

/* Each option is EmbLD's own, or one that changes nothing about a link
 * EmbLD makes, or refused by name. An option that shapes the image and
 * were dropped -- a linker script, a section start EmbLD has no idea of
 * -- would build a different image from the one asked for, and a
 * firmware image built to the wrong memory map runs, wrongly. */
static int apply_wl(struct link_opts *lo)
{
    for (int k = 0; k < g_nwl; k++) {
        const char *a = g_wl[k], *v;
        if ((v = wl_value("-Ttext-segment", 0, &k)) ||
            (v = wl_value("-Ttext", 1, &k))) {
            lo->base = strtoul(v, NULL, 0);
            lo->have_base = 1;
        } else if ((v = wl_value("-Tdata", 1, &k))) {
            lo->data_base = strtoul(v, NULL, 0);
        } else if ((v = wl_value("-Tstack", 1, &k))) {
            lo->stack_top = strtoul(v, NULL, 0);
            lo->have_stack = 1;
        } else if ((v = wl_value("--rom-limit", 0, &k))) {
            lo->rom_limit = strtoul(v, NULL, 0);
        } else if ((v = wl_value("--csa", 0, &k))) {
            const char *colon = strchr(v, ':');
            if (!colon) {
                fprintf(stderr, "embcc: error: --csa takes START:END\n");
                return 1;
            }
            lo->csa_start = strtoul(v, NULL, 0);
            lo->csa_end = strtoul(colon + 1, NULL, 0);
            lo->have_csa = 1;
        } else if ((v = wl_value("--lma-offset", 0, &k))) {
            lo->lma_offset = strtoull(v, NULL, 0);
        } else if ((v = wl_value("--entry", 0, &k)) ||
                   (v = wl_value("-e", 0, &k))) {
            lo->entry = v;
        } else if (!strncmp(a, "-Tbss", 5)) {
            fprintf(stderr, "embcc: error: -Tbss is not an EmbLD option; a "
                            "linker script (-T FILE) places .bss\n");
            return 1;
        } else if ((v = wl_value("--script", 0, &k)) ||
                   (v = wl_value("-T", 1, &k))) {
            if (g_script && strcmp(g_script, v)) {
                fprintf(stderr, "embcc: error: two linker scripts (-T)\n");
                return 1;
            }
            g_script = v;
        } else if ((v = wl_value("-L", 1, &k))) {
            if (g_nlibdirs < 64)
                g_libdirs[g_nlibdirs++] = v;
        } else if ((v = wl_value("--undefined", 0, &k)) ||
                   (v = wl_value("-u", 0, &k))) {
            if (g_nundefs < 64)
                g_undefs[g_nundefs++] = v;
        } else if ((v = wl_value("--orphan-handling", 0, &k))) {
            if (!strcmp(v, "place")) lo->orphan_mode = 0;
            else if (!strcmp(v, "warn")) lo->orphan_mode = 1;
            else if (!strcmp(v, "error")) lo->orphan_mode = 2;
            else {
                fprintf(stderr, "embcc: error: --orphan-handling is place, "
                                "warn or error\n");
                return 1;
            }
        } else if (!strcmp(a, "--gc-sections") ||
                   !strcmp(a, "--no-gc-sections")) {
            lo->gc_sections = a[2] == 'g';
        } else if (!strcmp(a, "--print-gc-sections")) {
            lo->print_gc_sections = 1;
        } else if (!strcmp(a, "--print-memory-usage")) {
            lo->print_memory_usage = 1;
        } else if ((v = wl_value("-Map", 0, &k)) ||
                   (v = wl_value("--Map", 0, &k))) {
            lo->map_file = v;
        } else if (!strcmp(a, "--as-needed") || !strcmp(a, "--no-as-needed") ||
                   !strcmp(a, "-O0") || !strcmp(a, "-O1") || !strcmp(a, "-O2") ||
                   !strcmp(a, "--build-id") ||
                   !strncmp(a, "--build-id=", 11) ||
                   !strcmp(a, "--no-undefined") ||
                   !strcmp(a, "-s") || !strcmp(a, "--strip-all") ||
                   !strcmp(a, "-S") || !strcmp(a, "--strip-debug")) {
            /* nothing the image depends on: EmbLD has no shared
             * objects to need or not, refuses an
             * undefined symbol anyway, and the symbols it keeps change
             * no byte that runs */
        } else if (!strcmp(a, "-z") && k + 1 < g_nwl &&
                   (!strcmp(g_wl[k + 1], "noexecstack") ||
                    !strcmp(g_wl[k + 1], "relro") ||
                    !strcmp(g_wl[k + 1], "norelro") ||
                    !strcmp(g_wl[k + 1], "now") ||
                    !strcmp(g_wl[k + 1], "lazy"))) {
            k++;    /* dynamic-linking and stack-marking properties of an
                     * image that has neither */
        } else {
            fprintf(stderr, "embcc: error: linker option '%s' is not one "
                            "EmbLD has (it takes -T, -L, -u, -e, -Ttext, "
                            "-Tdata, -Tstack, --csa, --rom-limit, --lma-offset, "
                            "--orphan-handling, --gc-sections, "
                            "--print-gc-sections, -Map and "
                            "--print-memory-usage); dropping it could build a "
                            "different image from the one asked for\n",
                    a);
            return 1;
        }
    }
    return 0;
}

static int compile(const char *in, const char *out, int pp_only);
static int has_gas_suffix(const char *s);
static int assemble_file(const char *in, const char *out);

/* An object or an archive, which goes to the link as it is. */
static int has_link_input_suffix(const char *p)
{
    size_t n = strlen(p);
    return (n > 2 && !strcmp(p + n - 2, ".o")) ||
           (n > 2 && !strcmp(p + n - 2, ".a")) ||
           (n > 4 && !strcmp(p + n - 4, ".obj"));
}

/* -lNAME, as a linker finds it: libNAME.a in each -L directory in order.
 * Three names are this toolchain's own, because a Makefile written for
 * arm-none-eabi-gcc says them: -lc and -lm are EmbCC's libc (its math is
 * in it), -lgcc is the compiler runtime, librt.a. Those return 1 having
 * found nothing to add, when the driver adds them itself anyway. */
static int find_lib(const char *name, char *out, size_t cap, int *own)
{
    *own = 0;
    for (int k = 0; k < g_nlibdirs; k++) {
        snprintf(out, cap, "%s/lib%s.a", g_libdirs[k], name);
        if (plat_file_exists(out))
            return 1;
    }
    if (!strcmp(name, "c") || !strcmp(name, "m") || !strcmp(name, "gcc")) {
        *own = 1;
        return 1;
    }
    return 0;
}

/* Is this one of GCC's Xtensa options (gcc/config/xtensa/xtensa.opt and
 * elf.opt, and Espressif's), which the Xtensa target answers itself:
 * accepted when it asks for what EmbCC emits, refused by name otherwise.
 * The -m spellings every target accepts are not among them. */
static int xtensa_flag(const char *a)
{
    static const char *const names[] = {
        "-mlongcalls", "-mno-longcalls", "-mtext-section-literals",
        "-mno-text-section-literals", "-mauto-litpools", "-mno-auto-litpools",
        "-mserialize-volatile", "-mno-serialize-volatile", "-mtarget-align",
        "-mno-target-align", "-mforce-no-pic", "-mlittle-endian",
        "-mbig-endian", "-mstrict-align", "-mno-strict-align", "-mlra",
        "-mno-lra", "-mconst16", "-mno-const16", "-mforce-l32",
        "-mno-fix-esp32-psram-cache-issue"
    };
    for (unsigned k = 0; k < sizeof names / sizeof names[0]; k++)
        if (!strcmp(a, names[k]))
            return 1;
    return !strncmp(a, "-mabi=", 6) || !strncmp(a, "-mdynconfig=", 12) ||
           !strncmp(a, "-mextra-l32r-costs=", 19) ||
           !strncmp(a, "-mfix-esp32-psram-cache-issue", 29);
}

/* Is this a target the driver links firmware for: one embld links, whose
 * memory map the build supplies (-T, or -Wl,-Ttext...). */
static int firmware_target(void)
{
    return target_fmt_get() == TGT_FMT_ELF &&
           backend_get(target_get())->firmware;
}

/* `embcc [prog.c] [a.o b.a -lfoo...] -o OUT`: compile the source if there
 * is one, then link it with everything else, in ONE process.
 *
 * A library, not a subprocess. The platform seam has no process API and
 * the compile and link never need one -- EmbLinkOS has no fork/exec, and
 * a driver that spawned `ld` could not be hosted on the target at all
 * (ARCHITECTURE §1). src/link/link.c has always been written as a
 * library for exactly this, and this is where it gets used.
 *
 * What goes into the link, in the order a linker resolves:
 *
 *   crt1.o      a hosted target's entry point, which calls main and
 *               leaves through exit() -- found beside the compiler
 *   the object  just compiled, into a temporary next to the output
 *   the inputs  the objects, archives and -l libraries on the command
 *               line, in their order
 *   libcxx.a    the C++ runtime, for a C++ source
 *   libc.a      an ARCHIVE, so only the members referenced are pulled
 *   librt.a     the compiler runtime (lib/rt): the routines the backend
 *               calls for operations the machine has no instruction for.
 *               After libc, because an archive is searched once and libc
 *               calls into it
 *
 * A FIRMWARE target (ARMv7-M, ARMv8-M, RISC-V, AVR) has no crt1 -- the
 * startup is the program's own, it is where the vector table is -- and
 * its memory map comes from the build: a linker script (-T) or
 * -Wl,-Ttext/-Tdata. There is no default, because a firmware image
 * linked to a guessed map runs, wrongly. -nostdlib leaves out libc and
 * librt, -nodefaultlibs too, and -nostartfiles crt1. */
static int compile_and_link(const char *in, const char *out)
{
    int fw = firmware_target();
    if (!fw && (target_get() != TARGET_X86_64 ||
                target_fmt_get() != TGT_FMT_ELF)) {
        if (target_fmt_get() != TGT_FMT_ELF)
            fprintf(stderr,
                    "embcc: error: cannot link for %s: the driver links "
                    "ELF, and this target writes %s\n",
                    target_triple_now(), target_fmt_name(target_fmt_get()));
        else
            fprintf(stderr,
                    "embcc: error: cannot link for %s: embld does not read "
                    "AArch64 objects\n", target_triple_now());
        fprintf(stderr,
                "embcc: compile with -c, then link with %s\n",
                target_fmt_get() == TGT_FMT_ELF
                    ? "an AArch64 toolchain's linker"
                    : "the platform's linker (ld64 or lld on macOS, "
                      "link.exe or lld-link on Windows)");
        return 1;
    }

    struct link_opts lo;
    memset(&lo, 0, sizeof lo);
    if (apply_wl(&lo))
        return 1;
    if (g_entry)
        lo.entry = g_entry;
    lo.script = g_script;
    lo.libdirs = g_libdirs;
    lo.nlibdirs = g_nlibdirs;
    lo.undefs = g_undefs;
    lo.nundefs = g_nundefs;
    if (fw && !lo.script && !lo.have_base) {
        /* embld lays a script out for some targets only (the registry's
         * ld_scripts): another build is not sent looking for one */
        int scripts = backend_get(target_get())->ld_scripts;
        fprintf(stderr,
                "embcc: error: linking a %s image needs its memory map: %s"
                "-Wl,-Ttext=FLASH and -Wl,-Tdata=RAM\n", target_triple_now(),
                scripts ? "a linker script (-T FILE.ld), or " : "");
        return 1;
    }
    if (!fw && lo.script) {
        fprintf(stderr, "embcc: error: a linker script (-T) is for an ARM, "
                        "RISC-V or AVR image; %s links without one\n",
                target_triple_now());
        return 1;
    }

    /* -specs=nano.specs, --specs=nosys.specs: the GCC driver's choice of
     * newlib and its syscall stubs. Said once, here, where it would have
     * mattered; a compile that does not link has nothing to say. */
    if (g_specs)
        fprintf(stderr, "embcc: note: -specs=%s is ignored: EmbCC links its "
                        "own C library and runtime, not newlib\n", g_specs);
    const char *exe = out ? out : "a.out";
    /* The temporary lives beside the output, not in /tmp: a build that
     * cannot write next to its own output has a problem worth seeing,
     * and this keeps the whole operation inside one directory. */
    char obj[1024];
    snprintf(obj, sizeof obj, "%s.embcc-tmp.o", exe);
    int rc;
    if (in) {
        rc = has_gas_suffix(in)
            ? assemble_file(in, obj)
            : compile(in, obj, 0);
        if (rc != 0)
            return rc;
    }

    const char *inputs[300];
    char *found[256];
    int n = 0, nfound = 0, want_libc = 0, want_rt = 0;
    const char *triple = lib_triple();
    char crt1[1024], libc[1024], librt[1024], libcxx[1024];
    int std = !g_nostdlib && !g_nodefaultlibs;
    int have_crt1 = !fw && !g_nostdlib && !g_nostartfiles &&
                    paths_target_file(triple, "crt1.o", crt1, sizeof crt1);
    int have_libc = paths_target_file(triple, "libc.a", libc, sizeof libc);
    /* The C++ runtime, for a C++ source. Before libc, because it calls
     * into it -- operator new is malloc, and a thrown std::string
     * formats through the C library -- and an archive is searched
     * once. */
    int cxx = (in && lang_cxx) || g_link_cxx;
    int have_cxx = cxx && std &&
                   paths_target_file(triple, "libcxx.a", libcxx,
                                     sizeof libcxx);
    if (cxx && std && !have_cxx) {
        fprintf(stderr,
                "embcc: error: no libcxx.a for %s -- a C++ program needs "
                "the C++ runtime, and this target's is not built or not "
                "installed\n", triple);
        fprintf(stderr, "embcc: --print-search-dirs says where it looked\n");
        if (in)
            remove(obj);
        return 1;
    }
    /* The compiler runtime, if this target has one. Not an error when
     * absent: a target that links somebody else's libgcc has no librt
     * of ours, and one that needs a routine it does not have gets a
     * link error naming the routine (src/link/link.c explains those by
     * family). */
    int have_rt = paths_target_file(triple, "librt.a", librt, sizeof librt);
    /* A hosted target whose library is not installed cannot be linked,
     * and saying which file is missing is the difference between a
     * fixable message and fifty undefined symbols. */
    if (!fw && std && target_is_hosted() && (!have_crt1 || !have_libc) &&
        !g_nostartfiles) {
        fprintf(stderr,
                "embcc: error: no %s for %s -- the target's library is "
                "not built or not installed\n",
                !have_crt1 ? "crt1.o" : "libc.a", triple);
        fprintf(stderr, "embcc: --print-search-dirs says where it looked\n");
        if (in)
            remove(obj);
        return 1;
    }
    if (have_crt1)
        inputs[n++] = crt1;
    if (in)
        inputs[n++] = obj;
    for (int k = 0; k < g_nlink_in; k++) {
        const char *a = g_link_in[k];
        if (a[0] == '-' && a[1] == 'l') {
            char path[1024];
            int own;
            if (!find_lib(a + 2, path, sizeof path, &own)) {
                fprintf(stderr, "embcc: error: cannot find lib%s.a for %s "
                                "in any -L directory\n", a + 2, a);
                if (in)
                    remove(obj);
                return 1;
            }
            if (own) {              /* -lc -lm -lgcc: EmbCC's own, added below */
                if (!strcmp(a + 2, "gcc")) want_rt = 1;
                else want_libc = 1;
                continue;
            }
            if (nfound < 256)
                inputs[n++] = found[nfound++] = xstrndup(path, strlen(path));
        } else if (n < 290) {
            inputs[n++] = a;
        }
    }
    if (have_cxx)
        inputs[n++] = libcxx;
    if (have_libc && (std || want_libc))
        inputs[n++] = libc;
    /* librt AFTER libc: an archive is searched once, in order, and a
     * libc routine can call into the runtime -- printing a 128-bit
     * value divides by ten -- while nothing in the runtime calls libc. */
    if (have_rt && (std || want_rt))
        inputs[n++] = librt;
    if (!n) {
        fprintf(stderr, "embcc: error: nothing to link\n");
        return 1;
    }

    /* Inside a boundary: embld's refusals unwind to here (util.h), where
     * with none they ended the process and left OUT.embcc-tmp.o behind
     * for every undefined symbol or --rom-limit overflow. */
    {
        jmp_buf lb;
        volatile int lrc = 1;
        if (setjmp(lb) == 0) {
            fatal_set_boundary(&lb);
            lrc = embld_link(inputs, n, exe, &lo);
        }
        fatal_set_boundary(NULL);
        rc = lrc;
    }
    if (in)
        remove(obj);
    for (int k = 0; k < nfound; k++)
        free(found[k]);
    return rc;
}

/* ---- -save-temps ---------------------------------------------------------
 *
 * GCC keeps what its passes hand each other -- the preprocessed source
 * and the assembly -- and names them after the output, as GCC 11 and
 * later do: `-c x.c -o build/x.o` keeps build/x.i and build/x.s, a link
 * `x.c -o fw.elf` keeps fw-x.i and fw-x.s beside fw.elf, and with no -o
 * the input's name is used in the current directory. -save-temps=cwd
 * puts them in the current directory whatever the output; -dumpbase
 * NAME names them NAME.i and NAME.s.
 *
 * EmbCC has no files between its stages, so each is produced by running
 * this compiler again over the same source with -E and with -S -- the
 * compile is deterministic, so that .s is the assembly of the object
 * this command writes. A unit's state lives in globals (multi_source),
 * so the second and third runs are processes of their own. */
static char *strip_ext(const char *p)
{
    const char *sl = strrchr(p, '/'), *dot = strrchr(p, '.');
    size_t n = dot && (!sl || dot > sl) && dot != (sl ? sl + 1 : p)
             ? (size_t)(dot - p) : strlen(p);
    return xstrndup(p, n);
}

static const char *base_name(const char *p)
{
    const char *sl = strrchr(p, '/');
    return sl ? sl + 1 : p;
}

static int save_temps(int argc, char **argv, const char *in, const char *out,
                      int compile_mode, int want_s)
{
    char *base;
    if (g_dumpbase) {
        base = xstrndup(g_dumpbase, strlen(g_dumpbase));
    } else if (compile_mode) {
        base = strip_ext(out ? out : base_name(in));
    } else {
        char *o = strip_ext(out ? out : "a.out");
        char *s = strip_ext(base_name(in));
        size_t n = strlen(o) + strlen(s) + 2;
        base = xmalloc(n);
        snprintf(base, n, "%s-%s", o, s);
        free(o);
        free(s);
    }
    if (g_save_temps == 2) {            /* =cwd */
        char *b = xstrndup(base_name(base), strlen(base_name(base)));
        free(base);
        base = b;
    }
    if (!plat_can_run()) {
        fprintf(stderr, "embcc: error: -save-temps runs the compiler again "
                "for %s.i and %s.s, and this host cannot run a program "
                "(EmbCC was built with PROCESS=none): use -E and -S\n",
                base, base);
        free(base);
        return 1;
    }
    const char *self = plat_self_path();
    if (!self)
        self = argv[0];
    const char **av = xmalloc((size_t)(argc + 8) * sizeof *av);
    int rc = 0;
    for (int pass = 0; pass < 2 && !rc; pass++) {
        if (pass == 1 && want_s)
            break;                  /* the .s is this command's own output */
        size_t n = strlen(base) + 4;
        char *path = xmalloc(n);
        snprintf(path, n, "%s.%s", base,
                 pass ? "s" : lang_cxx ? "ii" : "i");
        if (strcmp(path, in) == 0 || (out && strcmp(path, out) == 0)) {
            free(path);             /* never over the input or the output */
            continue;
        }
        int k = 0;
        av[k++] = self;
        for (int i = 1; i < argc; i++) {
            const char *a = argv[i];
            if (g_child_skip[i])
                continue;           /* sources, link inputs, -o, -j */
            if (!strcmp(a, "-dumpbase") || !strcmp(a, "-MF") ||
                !strcmp(a, "-MT") || !strcmp(a, "-MQ")) {
                i++;
                continue;
            }
            if (!strcmp(a, "-c") || !strcmp(a, "-S") || !strcmp(a, "-E") ||
                !strncmp(a, "-save-temps", 11) ||
                !strcmp(a, "--save-temps") || !strcmp(a, "-M") ||
                !strcmp(a, "-MM") || !strcmp(a, "-MD") ||
                !strcmp(a, "-MMD") || !strcmp(a, "-MP") ||
                !strcmp(a, "-fstack-usage") || !strcmp(a, "-fremarks") ||
                !strcmp(a, "-fcallgraph-info") ||
                !strcmp(a, "-fcallgraph-info=su") ||
                !strcmp(a, "-fremarks=json"))
                continue;           /* this command's own outputs */
            av[k++] = a;
        }
        av[k++] = pass ? "-S" : "-E";
        av[k++] = "-o";
        av[k++] = path;
        av[k++] = in;
        av[k] = NULL;
        int which = -1;
        if (plat_run_start(av) < 0) {
            fprintf(stderr, "embcc: error: could not run %s for %s\n", self,
                    path);
            rc = 1;
        } else if (plat_run_wait(&which) != 0) {
            rc = 1;                 /* its diagnostics are already out */
        }
        free(path);
    }
    free(av);
    free(base);
    return rc;
}

/* ---- several sources in one command --------------------------------------
 *
 * `embcc a.c b.c -o prog`, `embcc -c a.c b.c`: what GCC and Clang take
 * from every Makefile and CMake. The compiler's state is per process -- a
 * unit's types, symbols and IR live in globals -- so a source is compiled
 * by a process of its own: this driver, run again with that one source
 * (plat_run_start), as gcc runs cc1. -j N runs N at once. Then the
 * objects are linked here, in command-line order with the other inputs,
 * and the temporaries removed.
 *
 * A host that cannot run a program (platform.h: EmbLinkOS) gets the
 * refusal it always had, with the way round it. */
static int has_cxx_suffix(const char *path);

static int multi_source(int argc, char **argv, const char *output,
                        int compile_mode, int want_s, int pp)
{
    if (!plat_can_run()) {
        fprintf(stderr, "embcc: error: more than one source file ('%s' and "
                "'%s'), and this host cannot run a compiler for each (EmbCC "
                "was built with PROCESS=none): compile each with -c and link "
                "the objects (embcc a.o b.o -o OUT)\n", g_srcs[0], g_srcs[1]);
        return 1;
    }
    if (output && (compile_mode || want_s || pp)) {
        fprintf(stderr, "embcc: error: -o names one output, and -%s with "
                "%d sources writes one each: leave -o out, or compile them "
                "one at a time\n", pp ? "E" : want_s ? "S" : "c", g_nsrc);
        return 1;
    }
    int link = !compile_mode && !want_s && !pp;
    const char *self = plat_self_path();
    if (!self)
        self = argv[0];
    const char *exe = output ? output : "a.out";

    /* the child's command line: ours, less what is not its business */
    const char **cargv = xmalloc((size_t)(argc + 8) * sizeof *cargv);
    int base = 0;
    cargv[base++] = self;
    for (int i = 1; i < argc; i++)
        if (!g_child_skip[i])
            cargv[base++] = argv[i];

    char **tmp = xcalloc((size_t)g_nsrc, sizeof *tmp);
    int next = 0, running = 0, failed = 0;
    /* -E writes to standard output, in order: one at a time */
    int jobs = pp ? 1 : g_jobs;
    while (next < g_nsrc || running) {
        while (next < g_nsrc && running < jobs && !failed) {
            int n = base;
            const char **av = xmalloc((size_t)(base + 8) * sizeof *av);
            memcpy(av, cargv, (size_t)base * sizeof *av);
            if (link) {
                size_t ln = strlen(exe) + 32;
                tmp[next] = xmalloc(ln);
                snprintf(tmp[next], ln, "%s.embcc-tmp-%d.o", exe, next + 1);
                /* -save-temps: named after the image and the source, as
                 * for one source (save_temps), not after the temporary */
                if (g_save_temps && !g_dumpbase) {
                    char *o = strip_ext(exe), *sb = strip_ext(base_name(
                                                         g_srcs[next]));
                    size_t bn = strlen(o) + strlen(sb) + 2;
                    char *db = xmalloc(bn);
                    snprintf(db, bn, "%s-%s", o, sb);
                    free(o);
                    free(sb);
                    av[n++] = "-dumpbase";
                    av[n++] = db;   /* (freed with the process) */
                }
                av[n++] = "-c";
                av[n++] = g_srcs[next];
                av[n++] = "-o";
                av[n++] = tmp[next];
            } else {
                av[n++] = g_srcs[next];
            }
            av[n] = NULL;
            int h = plat_run_start(av);
            free(av);
            if (h < 0) {
                fprintf(stderr, "embcc: error: could not run %s for '%s'\n",
                        self, g_srcs[next]);
                failed = 1;
                break;
            }
            next++;
            running++;
        }
        if (!running)
            break;
        int which = -1;
        int st = plat_run_wait(&which);
        if (st < 0)
            break;
        running--;
        if (st != 0)            /* its diagnostics are already out */
            failed = 1;
    }
    if (!failed && next < g_nsrc)
        failed = 1;
    free(cargv);

    int rc = failed;
    if (link && !failed) {
        /* the objects where their sources were, among the other inputs */
        const char *in[256 + MAX_SRCS];
        int n = 0, a = 0, b = 0;
        while (a < g_nsrc || b < g_nlink_in) {
            if (b >= g_nlink_in ||
                (a < g_nsrc && g_src_argi[a] < g_link_argi[b]))
                in[n++] = tmp[a++];
            else
                in[n++] = g_link_in[b++];
        }
        if (n > 256) {
            fprintf(stderr, "embcc: error: more than 256 link inputs\n");
            rc = 1;
        } else {
            for (int k = 0; k < n; k++)
                g_link_in[k] = in[k];
            g_nlink_in = n;
            for (int k = 0; k < g_nsrc && !g_link_cxx; k++)
                g_link_cxx = has_cxx_suffix(g_srcs[k]);
            rc = compile_and_link(NULL, output);
        }
    }
    for (int k = 0; k < g_nsrc; k++)
        if (tmp[k]) {
            remove(tmp[k]);
            free(tmp[k]);
        }
    free(tmp);
    return rc;
}

static int compile(const char *in, const char *out, int pp_only)
{
    jmp_buf boundary;
    volatile int rc;
    if (setjmp(boundary) == 0) {
        fatal_set_boundary(&boundary);
        rc = compile_unit(in, out, pp_only);
    } else {
        rc = 1;                       /* the diagnostic is already out */
    }
    fatal_set_boundary(NULL);
    /* A warning made an error (-Werror) leaves the unit compiled and its
     * file written. The compile has still failed, and the file must not
     * be left behind: make would take it as up to date and never build
     * it again. */
    if (rc == 0 && diag_error_count() > 0) {
        rc = 1;
        if (out && strcmp(out, "-") != 0)
            remove(out);
    }
    return rc;
}

/* ---- assembly in a C file, on the embedded targets ------------------
 *
 * A file-scope asm block there is read by the assembler that reads a .s
 * file (src/as/gas.c, gas_assemble_block), so it may hold the target's
 * own instructions, labels, literal pools and references to C. x86-64's
 * blocks keep their fixed vocabulary (src/arch/x86_64/topasm.c), and so
 * do AArch64's, which are data words today. */
static int blocks_by_gas(void)
{
    return backend_get(target_get())->firmware;
}

/* One asm statement of a naked function, its operands written in: only
 * constants, as gcc's documentation says is all a naked function's asm
 * may dependably take -- there is no frame to put anything else in. */
static void naked_asm_text(struct outbuf *b, const struct func *f,
                           const struct stmt *s)
{
    const struct asm_stmt *a = s->asm_s;
    if (a->is_basic) {
        ob_str(b, a->tmpl);
        ob_ch(b, '\n');
        return;
    }
    if (a->nout)
        diag_fatal(f->file, s->line,
                   "the asm in naked function '%s' has an output; a naked "
                   "function has no frame to put it in", f->name);
    for (int i = 0; i < a->nin; i++)
        if (!a->in[i].is_imm)
            diag_fatal(f->file, s->line,
                       "operand %d of the asm in naked function '%s' is not "
                       "a constant (\"i\"); a naked function has no frame "
                       "to load one from", i, f->name);
    for (const char *p = a->tmpl; *p; ) {
        if (*p != '%') {
            ob_ch(b, *p++);
            continue;
        }
        p++;
        if (*p == '%') {
            ob_ch(b, '%');
            p++;
            continue;
        }
        /* %c0: the constant without a prefix -- how every operand here is
         * written, except on RX and ColdFire, whose GCCs print an immediate
         * as `#5` */
        int bare = *p == 'c' || !backend_get(target_get())->imm_prefixed;
        if (*p == 'c')
            p++;
        int k = -1;
        if (*p == '[') {
            const char *e = strchr(p, ']');
            if (!e)
                diag_fatal(f->file, s->line,
                           "unterminated %%[name] in asm template");
            for (int i = 0; i < a->nin; i++)
                if (a->in[i].name &&
                    strlen(a->in[i].name) == (size_t)(e - p - 1) &&
                    !strncmp(a->in[i].name, p + 1, (size_t)(e - p - 1)))
                    k = i;
            if (k < 0)
                diag_fatal(f->file, s->line, "asm template names an unknown "
                           "operand '%.*s'", (int)(e - p - 1), p + 1);
            p = e + 1;
        } else if (*p >= '0' && *p <= '9') {
            k = 0;
            while (*p >= '0' && *p <= '9')
                k = k * 10 + (*p++ - '0');
            if (k >= a->nin)
                diag_fatal(f->file, s->line, "asm template refers to operand "
                           "%%%d, but there are only %d", k, a->nin);
        } else {
            diag_fatal(f->file, s->line, "asm template modifier '%%%c' is "
                       "not supported in a naked function", *p ? *p : ' ');
        }
        ob_fmt(b, bare ? "%ld" : "#%ld", a->in[k].imm);
    }
    ob_ch(b, '\n');
}

/* The statements of a naked function's body, as assembly. */
static void naked_body_text(struct outbuf *b, const struct func *f,
                            const struct stmt *s)
{
    for (; s; s = s->next) {
        if (s->kind == STMT_ASM) {
            naked_asm_text(b, f, s);
            continue;
        }
        if (s->kind == STMT_BLOCK) {
            naked_body_text(b, f, s->body);
            continue;
        }
        if (s->kind == STMT_EXPR && !s->expr)
            continue;                         /* `;` */
        /* A call with no arguments is its call instruction: AVR's
         * FreeRTOS port calls the scheduler from its naked yield between
         * the asm that saves and restores the context. */
        const struct expr *e = s->kind == STMT_EXPR ? s->expr : NULL;
        if (e && e->kind == EXPR_CALL && e->callee && e->nargs == 0) {
            /* the target's call (the registry's call_insn), and its delay
             * slot filled with a nop where it has one (SPARC): the
             * statement after the call is the next asm, not its slot */
            const struct backend_desc *bd = backend_get(target_get());
            ob_fmt(b, "%s %s%s\n%s", bd->call_insn, bd->sym_prefix,
                   e->callee->name, bd->call_delay_slot ? "nop\n" : "");
            continue;
        }
        diag_fatal(f->file, s->line,
                   "naked function '%s' holds a statement that is not an asm "
                   "or a call with no arguments; with no prologue there is "
                   "no frame for it to run in", f->name);
    }
}

/* __attribute__((naked)): no prologue and no epilogue; the body is the
 * function. How a Cortex-M RTOS writes its context switch -- FreeRTOS's
 * xPortPendSVHandler runs on the task's stack and owns every register
 * it saves, so any frame the code generator built would be wrong by
 * construction.
 *
 * gcc emits the body's asm where the function would be. So does this:
 * the function becomes a file-scope block that starts with its label,
 * and the block assembler reads it like any other. A static one's label
 * is local, and the C code's references to it go to that label. */
static void naked_to_blocks(struct unit *u)
{
    struct topasm **tail = &u->topasm;
    while (*tail)
        tail = &(*tail)->next;
    for (struct func *f = u->funcs; f; f = f->next) {
        if (f->absorbed || !f->is_naked || !f->has_defn)
            continue;
        if (f->section)
            diag_fatal(f->file, f->line, "naked function '%s' in section "
                       "'%s' is not supported yet: its body is assembled "
                       "into .text", f->name, f->section);
        struct outbuf b = { NULL, 0, 0 };
        enum target_arch t = target_get();
        /* the object's name, `_f` on RX, as the block assembler reads it */
        const char *up = backend_get(t)->sym_prefix;
        ob_fmt(&b, ".text\n.p2align %d\n", backend_get(t)->text_p2align);
        if (f->is_weak)
            ob_fmt(&b, ".weak %s%s\n", up, f->name);
        else if (!f->is_static)
            ob_fmt(&b, ".global %s%s\n", up, f->name);
        ob_fmt(&b, ".type %s%s, %%function\n", up, f->name);
        if (t == TARGET_THUMB && thumb_state())
            ob_str(&b, ".thumb_func\n");
        ob_fmt(&b, "%s%s:\n", up, f->name);
        /* a diagnostic in the body names the line of its first asm */
        int head = 0;
        for (size_t k = 0; k < b.n; k++)
            head += b.p[k] == '\n';
        const struct stmt *first = f->body;     /* the statement list */
        naked_body_text(&b, f, first);
        ob_fmt(&b, ".size %s%s, .-%s%s\n", up, f->name, up, f->name);
        ob_ch(&b, '\0');
        struct topasm *ta = xcalloc(1, sizeof *ta);
        ta->tmpl = b.p;
        ta->file = f->file;
        ta->line = (first ? first->line : f->line) - head;
        *tail = ta;
        tail = &ta->next;
        f->has_defn = 0;        /* its code is the block's */
    }
}

/* EmbCC's own headers after every -I the caller gave, once, and marked
 * system (compile_unit says why). */
static int g_incdirs_done;
static void incdirs_with_defaults(void)
{
    if (g_incdirs_done)
        return;
    g_incdirs_done = 1;
    if (!no_stdinc) {
        int ndef = 0;
        const char *const *def = paths_default_includes(&ndef);
        for (int k = 0; k < ndef && nincdirs < MAX_INCDIRS; k++) {
            incdir_sys[nincdirs] = 1;
            incdirs[nincdirs++] = def[k];
        }
    }
    cpp_set_system_dirs(incdir_sys, nincdirs);
}

/* A `.s` or `.S` file. A `.S` is preprocessed with the same search path
 * as C: the -I directories reached a C file's #include and not an
 * assembly file's, so FreeRTOS's RISC-V portASM.S could not find the
 * chip-specific header its build names with -I. */
static int assemble_file(const char *in, const char *out)
{
    int kind = has_gas_suffix(in);
    if (kind == 2)
        incdirs_with_defaults();
    return gas_assemble(in, out, kind == 2, incdirs, nincdirs);
}

/* The embedded targets whose C++ is compiled without exceptions only:
 * EmbCC writes no unwind tables for them, so a C++ unit with exceptions
 * on is refused by name. (RV64, MIPS64 little-endian and LoongArch
 * compile a unit with no landing pad either way; see cxx.md.) */
static int cxx_exceptions_unwritten(void)
{
    int c = backend_get(target_get())->cxx_exceptions;
    return c == BACKEND_CXX_EXC_NONE ||
           (c == BACKEND_CXX_EXC_BIG_ENDIAN && target_big_endian());
}

static const char *cxx_unwind_tables_name(void)
{
    return backend_get(target_get())->unwind_unwritten;
}

/* One .init_array/.fini_array section of the object: constructors
 * (kind 0) or destructors (1) of one priority (prio: N + 1, 0 none). */
struct ctorarr { int kind, prio, ndx, n, at; };

static int compile_unit(const char *in, const char *out, int pp_only)
{
    char *src = read_file(in);
    diag_register_source(in, src);   /* so diagnostics can show its lines */
    predef_set_cxx(lang_cxx);
    if (lang_cxx)
        cpp_set_cxx(cxx_has_builtin, want_exceptions);
    /* EmbCC's own headers, AFTER every -I the caller gave: a project
     * that ships its own <stdio.h> must win, or nothing we install can
     * ever be overridden. They are added here rather than at option
     * parsing so that -nostdinc and the -I order both stay simple, and
     * they are marked system so a warning inside them is not the
     * caller's problem. */
    incdirs_with_defaults();
    time_mark("startup");
    char *pp = cpp_process(in, src, incdirs, nincdirs);
    time_mark("preprocess");
    if (dep_mode && dep_only) {       /* -M/-MM: the rule is the output */
        write_deps(in, out);
        return 0;
    }
    if (pp_only) {
        /* -E -o FILE writes FILE, as GCC's does (the output used to go
         * to standard output whatever -o said, and `cc -E x.c -o x.i`
         * left no x.i). */
        if (out && plat_write_file(out, pp, strlen(pp)) != 0)
            diag_fatal(out, 0, "cannot write the file");
        if (!out)
            fputs(pp, stdout);
        return 0;
    }
    if (lang_cxx && !syntax_only && target_get() == TARGET_AVR) {
        /* The front end lays a class out by AVR's sizes, but its lowering
         * still writes a 32-bit int's promotions, enumerations and
         * literals, a `long` member-pointer adjustment and RTTI base
         * offsets one pointer wide -- each wrong where int and pointers
         * are 16 bits -- and no AVR C++ runtime is built. */
        fprintf(stderr,
                "embcc: error: C++ is not yet supported for %s: the C++ "
                "lowering assumes a 32-bit int and 4-byte pointers (integral "
                "promotions, member pointers, RTTI), and this target's are "
                "16 bits\n", target_triple_now());
        return 1;
    }
    if (lang_cxx && !syntax_only && want_exceptions &&
        cxx_exceptions_unwritten()) {
        /* Exceptions need the unwinder's tables and a personality routine
         * reading them: ARM EHABI's .ARM.exidx on ARM, DWARF .eh_frame
         * elsewhere. EmbCC writes neither for these machines yet, so a
         * throw could never be caught -- refused rather than compiled
         * into landing pads nothing would reach. */
        fprintf(stderr,
                "embcc: error: C++ exceptions are not supported for %s yet: "
                "EmbCC writes no %s; compile with -fno-exceptions\n",
                target_triple_now(), cxx_unwind_tables_name());
        return 1;
    }
    if (lang_cxx) {
        cxx_set_exceptions(want_exceptions);
        cxx_set_rtti(want_rtti);
        pp = cxx_translate(in, pp);
        time_mark("C++ front end");
        if (cx_nerrors) {
            /* Every C++ error is out; what it lowered to describes a
             * program that does not exist, so nothing downstream runs. */
            diag_terminated(cx_nerrors);
            return 1;
        }
        if (emit_c_only) {
            if (out) {
                if (plat_write_file(out, pp, strlen(pp)) != 0)
                    diag_fatal(out, 0, "cannot write the file");
            } else {
                fputs(pp, stdout);
            }
            return 0;
        }
    }
    /* `inspect tokens` is the lexer's own stage: after preprocessing, where
     * the token stream is a real thing, and before the parser consumes it. */
    if (inspect_stage && !strcmp(inspect_stage, "tokens")) {
        struct outbuf b = { NULL, 0, 0 };
        inspect_tokens(&b, in, pp, lang_cxx);
        fwrite(b.p, 1, b.n, stdout);
        ob_free(&b);
        return 0;
    }

    struct unit *u = parse_unit(in, pp);
    time_mark("parse");
    if (parse_error_count()) {
        /* Every syntax error is out; the tree is not whole, so nothing
         * downstream runs on it (a later pass would only invent errors). */
        diag_terminated(parse_error_count());
        return 1;                     /* (--fix still gets its turn) */
    }
    sema_check(u);
    time_mark("semantic analysis");
    if (sema_error_count()) {
        diag_terminated(sema_error_count());
        return 1;                     /* (--fix still gets its turn) */
    }
    /* The stages that read the analysed tree. They come after sema so the
     * types printed are the resolved ones -- an AST dump with every type
     * shown as "?" would answer a question nobody asks. */
    if (inspect_stage) {
        struct outbuf b = { NULL, 0, 0 };
        if (!strcmp(inspect_stage, "ast"))
            inspect_ast(&b, u);
        else if (!strcmp(inspect_stage, "symbols"))
            inspect_symbols(&b, u);
        else if (!strcmp(inspect_stage, "types"))
            inspect_types(&b, u);
        if (b.n) {
            fwrite(b.p, 1, b.n, stdout);
            ob_free(&b);
            return 0;
        }
        ob_free(&b);
    }

    /* Emitted after semantic analysis, because an interface hash is over
     * the RESOLVED declaration -- a layout, a signature -- not over what
     * the parser saw. */
    if (want_iface) {
        struct outbuf ib = { NULL, 0, 0 };
        iface_emit(&ib, u);
        int irc = out ? plat_write_file(out, ib.p, ib.n)
                      : (fwrite(ib.p, 1, ib.n, stdout), 0);
        if (irc != 0)
            diag_fatal(out, 0, "cannot write the file");
        ob_free(&ib);
        return 0;
    }

    if (dep_mode)                     /* -MD/-MMD: beside the object */
        write_deps(in, out);
    if (syntax_only)                  /* checked; nothing to write */
        return 0;

    /* File-scope asm (crt0's _start) can reference a function by name with
     * `call sym`. That reference has to count as a USE before irgen decides
     * which static functions to emit — otherwise a static function called
     * ONLY from asm (crt0's start_c, reached solely through _start's `call
     * start_c`) is pruned as dead, its body never generated, and the asm's
     * relocation binds to a value-0 placeholder symbol that aliases whatever
     * sits at .text offset 0. Assemble each block now — it depends only on
     * its own template, not on code layout — and mark its call targets used.
     * The placement pass further down reuses these already-assembled bytes. */
    if (blocks_by_gas())
        naked_to_blocks(u);
    for (struct topasm *ta = u->topasm; ta; ta = ta->next) {
        /* x86-64's mnemonics are what topasm.c encodes; the directives --
         * labels and .byte/.long/.quad -- are not, so on AArch64 a block
         * written as data assembles, and one written with mnemonics is
         * refused by name instead of quietly emitting x86 bytes. The
         * embedded targets have an assembler of their own. */
        if (blocks_by_gas()) {
            if (gas_assemble_block(ta))
                return 1;
        } else {
            topasm_assemble(ta, target_get() == TARGET_X86_64);
        }
        for (int r = 0; r < ta->nrels; r++) {
            if (!ta->rels[r].target)
                continue;                 /* the block's own label */
            for (struct func *f = u->funcs; f; f = f->next)
                if (!f->absorbed &&
                    strcmp(f->name, ta->rels[r].target) == 0)
                    f->used = f->is_root = 1;
            /* and an object only the asm names (`ldr r0, =counter`) */
            for (struct global *g = u->globals; g; g = g->next)
                if (!g->absorbed &&
                    strcmp(g->name, ta->rels[r].target) == 0)
                    g->used = 1;
        }
    }

    /* A constructor is called by the startup code, not by this unit, so
     * nothing here refers to it -- and a `static` one would otherwise
     * look dead, be dropped, and leave a relocation pointing at a
     * symbol that was never emitted. Being in .init_array IS the use. */
    for (struct func *f = u->funcs; f; f = f->next)
        if (!f->absorbed && (f->is_ctor || f->is_dtor || f->attr_used))
            /* __attribute__((used)) is the author saying to keep it
             * although nothing here calls it -- a handler reached only
             * from a table, or from assembly the compiler cannot see.
             * Dropping it would link and then do nothing. */
            f->used = f->is_root = 1;

    remarks_enable(want_remarks || why_decision != NULL);
    struct ir_unit *iu = irgen(u);
    time_mark("IR generation");
    opt_run(iu, opt_for_size ? OPT_SIZE : opt_level);
    time_mark("optimization");
    target_set_opt_size(opt_for_size);

    /* An inline definition (C11 6.7.4p7, decided by sema) has done its
     * job once the optimizer has had the chance to inline it: it is
     * not emitted, at any level. From here on it is a declaration, so a
     * call that remains -- every call at -O0 -- and its address name
     * the external definition another unit provides, as with gcc and
     * clang. */
    {
        int keep = 0;
        for (int k = 0; k < iu->nfuncs; k++) {
            struct func *f = iu->funcs[k].src;
            if (f && f->inline_only)
                continue;
            if (keep != k) iu->funcs[keep] = iu->funcs[k];
            keep++;
        }
        iu->nfuncs = keep;
        for (struct func *f = u->funcs; f; f = f->next)
            if (!f->absorbed && f->inline_only) {
                f->has_defn = 0;
                for (int k = 0; k < iu->nsyms; k++)
                    if (iu->syms[k].is_func &&
                        !strcmp(iu->syms[k].name, f->name))
                        iu->syms[k].defined = 0;
            }
    }

    /* ---- what is still reachable -------------------------------------
     *
     * A `static` function nothing calls is already dropped, but `used`
     * has meant "some call resolved here", which is not the same
     * question: a static called only from another dead static was
     * called, so it was kept, and so was everything IT called. One
     * unreferenced helper kept a whole private subtree alive.
     *
     * Reachability answers it properly. The roots are the things the
     * world outside this unit can reach -- anything not static, a
     * constructor or destructor (.init_array is the use), an
     * __attribute__((used)), and any target named by top-level asm,
     * which was marked above. Everything else is kept only if a
     * reachable function calls it or takes its address.
     *
     * Run after the optimizer on purpose: inlining absorbs callees and
     * dead-code elimination removes calls, so the call graph here is
     * the one that will actually be emitted rather than the one the
     * source described. */
    if (opt_level >= 1) {
        int nf = iu->nfuncs;
        char *reach = xcalloc((size_t)(nf ? nf : 1), 1);
        int *work = xmalloc((size_t)(nf ? nf : 1) * sizeof *work);
        int nw = 0;
        /* A function whose ADDRESS sits in a static initializer is
         * reached from data, not from code: a vtable entry, a handler
         * table, a designated initializer holding a function pointer.
         * Nothing in any function body mentions it, so the IR scan
         * below never sees it -- and dropping it leaves the relocation
         * in .data pointing at a symbol that was never emitted, which
         * is a link error rather than a wrong answer. */
        for (struct global *g = u->globals; g; g = g->next)
            for (int r = 0; r < g->nrelocs; r++)
                if (g->relocs[r].ftarget)
                    g->relocs[r].ftarget->is_root = 1;
        for (int k = 0; k < nf; k++) {
            struct func *f = iu->funcs[k].src;
            if (!f || f->absorbed || !f->has_defn)
                continue;
            if (!f->is_static || f->is_root)
                { reach[k] = 1; work[nw++] = k; }
        }
        while (nw) {
            struct ir_func *fn = &iu->funcs[work[--nw]];
            for (int n = 0; n < fn->nins; n++) {
                struct ir_ins *i = &fn->ins[n];
                struct func *t = NULL;
                if (i->op == IR_CALL && !i->indirect) t = i->callee;
                else if (i->op == IR_FADDR)           t = i->callee;
                if (!t)
                    continue;
                for (int k = 0; k < nf; k++)
                    if (iu->funcs[k].src == t && !reach[k]) {
                        reach[k] = 1; work[nw++] = k;
                        break;
                    }
            }
        }
        for (int k = 0; k < nf; k++) {
            struct func *f = iu->funcs[k].src;
            if (!f || f->absorbed || !f->has_defn || !f->is_static)
                continue;
            if (!reach[k] && f->used) {
                f->used = 0;               /* kept only by a dead caller */
                if (want_remarks)
                    remark_add("opt", "dropped", f->name, "unreachable",
                               f->file, f->line,
                               "no reachable caller after optimization");
            }
        }
        /* ...and actually drop them. Clearing `used` was not enough:
         * irgen decides what goes in the unit BEFORE the optimizer
         * runs, and codegen emits whatever is in the unit. So a static
         * function that inlining absorbed, or whose only caller the
         * optimizer deleted, was still being assembled into .text with
         * nothing left to call it. Compacting here is the one place
         * both backends see. */
        int keep = 0;
        for (int k = 0; k < nf; k++) {
            struct func *f = iu->funcs[k].src;
            int drop = f && f->is_static && !reach[k] && !f->used &&
                       strcmp(f->name, "main") != 0;
            if (!drop) {
                if (keep != k) iu->funcs[keep] = iu->funcs[k];
                keep++;
            }
        }
        iu->nfuncs = keep;
        free(reach); free(work);
    }

    /* A question was asked (§19): answer it and stop. The remarks exist
     * because the passes have run; rendering here means the answer is a
     * query over what the compiler actually decided, not a re-derivation. */
    if (why_decision) {
        struct outbuf b = { NULL, 0, 0 };
        int hits = remarks_render_why(&b, why_decision, why_subject);
        if (!hits)
            ob_fmt(&b, "nothing recorded: no pass made a '%s' decision%s%s\n"
                       "(remarks come from the passes that RAN -- an "
                       "optimization\ndecision needs -O2)\n",
                   why_decision, why_subject ? " about " : "",
                   why_subject ? why_subject : "");
        fwrite(b.p, 1, b.n, stdout);
        ob_free(&b);
        return 0;
    }

    /* `inspect ir` reports the IR as it stands at the current -O level, so
     * the same command shows irgen's output at -O0 and the optimizer's at
     * -O2 -- which is what makes a pass's effect visible: diff the two. */
    if (inspect_stage && (!strcmp(inspect_stage, "ir") ||
                          !strcmp(inspect_stage, "cfg") ||
                          !strcmp(inspect_stage, "callgraph"))) {
        struct outbuf b = { NULL, 0, 0 };
        if (!strcmp(inspect_stage, "ir")) {
            ir_print_unit(&b, iu);
        } else if (!strcmp(inspect_stage, "callgraph")) {
            inspect_callgraph(&b, iu);
        } else {
            for (int n = 0; n < iu->nfuncs; n++) {
                if (n)
                    ob_ch(&b, '\n');
                opt_cfg_dump(&b, &iu->funcs[n]);
            }
        }
        fwrite(b.p, 1, b.n, stdout);
        ob_free(&b);
        return 0;
    }

    /* -ffunction-sections: a section of its own for each function that
     * did not ask for one. Named here, after the optimizer, which has
     * no business with sections, and before the backend, for which a
     * callee in another section is a relocation (cg_call_local). */
    if (func_sections && target_fmt_get() == TGT_FMT_ELF)
        for (int i = 0; i < iu->nfuncs; i++) {
            struct func *f = iu->funcs[i].src;
            if (f && !f->section)
                f->section = sec_named(".text.", f->name);
        }

    /* Functions placed in a section of their own go last, grouped by
     * section in first-seen order (see g_tg); the rest keep their order. */
    g_ntg = 0;
    {
        int n = iu->nfuncs, k = 0;
        struct ir_func *sorted = xmalloc((size_t)(n ? n : 1) * sizeof *sorted);
        char *placed = xcalloc((size_t)(n ? n : 1), 1);
        for (int i = 0; i < n; i++)
            if (!iu->funcs[i].src || !iu->funcs[i].src->section) {
                sorted[k++] = iu->funcs[i];
                placed[i] = 1;
            }
        for (int i = 0; i < n; i++) {
            if (placed[i])
                continue;
            const char *sec = iu->funcs[i].src->section;
            for (int j = i; j < n; j++)
                if (!placed[j] && !strcmp(iu->funcs[j].src->section, sec)) {
                    sorted[k++] = iu->funcs[j];
                    placed[j] = 1;
                }
        }
        memcpy(iu->funcs, sorted, (size_t)n * sizeof *sorted);
        free(sorted);
        free(placed);
    }

    struct code text = { 0 };
    struct extcall *ext;
    struct strsite *strs;
    struct gsite *gs;
    struct fsite *fs;
    int next, nstrs, ngs, nfs;
    enum target_arch ta = target_get();
    /* Which machine: the registry's row (src/arch/backends.c). RISC-V is
     * ONE code generator for both widths, because the instruction set is
     * the same at both and only the data model differs (D-016), and so is
     * MIPS. Backends whose ra_at_o0 is set allocate at -O0 too, for each
     * expression's temporaries, every source variable pinned to its slot;
     * EMBCC_O0_NORA=1 is the old -O0, for bisecting a difference. */
    {
        const struct backend_desc *bd = backend_get(ta);
        bd->codegen(iu, &text, &ext, &next, &strs, &nstrs, &gs, &ngs, &fs,
                    &nfs, want_debug, opt_level >= 1, no_sse,
                    opt_level >= 1 ||
                    (bd->ra_at_o0 && !getenv("EMBCC_O0_NORA")));
    }
    time_mark("code generation");
    resolve_label_data(u);

    /* The groups, as codegen laid them out (the sort above). */
    g_plain_end = g_groups_end = (long)text.len;
    for (int i = 0; i < iu->nfuncs; i++) {
        struct func *f = iu->funcs[i].src;
        if (!f || !f->section || f->code_len <= 0)
            continue;
        if (!g_ntg || strcmp(g_tg[g_ntg - 1].name, f->section)) {
            if (g_ntg == g_captg) {
                g_captg = g_captg ? 2 * g_captg : 16;
                g_tg = xrealloc(g_tg, (size_t)g_captg * sizeof *g_tg);
            }
            /* (the sort above made each section's functions adjacent;
             * with -ffunction-sections every name is distinct, so this
             * is checked only where it can fail) */
            if (!func_sections)
                for (int k = 0; k < g_ntg; k++)
                    if (!strcmp(g_tg[k].name, f->section))
                        internal_error("function section '%s' not "
                                       "contiguous", f->section);
            g_tg[g_ntg].name = f->section;
            g_tg[g_ntg].start = f->code_off;
            g_tg[g_ntg].align = 1;
            if (!g_ntg)
                g_plain_end = f->code_off;
            g_ntg++;
        }
        g_tg[g_ntg - 1].end = f->code_off + f->code_len;
        /* the strictest start in the group; one codegen did not say is
         * given the sixteen every section had */
        if ((f->code_align ? f->code_align : 16) > g_tg[g_ntg - 1].align)
            g_tg[g_ntg - 1].align = f->code_align ? f->code_align : 16;
        g_groups_end = f->code_off + f->code_len;
    }
    if (g_ntg && target_fmt_get() != TGT_FMT_ELF)
        diag_fatal(in, 0, "a function's section attribute is not supported "
                   "for %s output", target_fmt_name(target_fmt_get()));
    for (int k = 0; k < g_ntg; k++)
        for (struct global *g = u->globals; g; g = g->next)
            if (!g->absorbed && g->section && !strcmp(g->section, g_tg[k].name))
                diag_fatal(g->file, g->line, "section '%s' holds a function, "
                           "and '%s' cannot share it: one is code, the other "
                           "data", g_tg[k].name, g->name);

    /* -fstack-usage: the frame each function ended up with, beside the
     * object. Written here, after codegen, because that is when the
     * number exists -- the frame is not known until the temporaries and
     * the outgoing-argument area have been laid out. */
    if (want_stack_usage) {
        struct outbuf sub = { NULL, 0, 0 };
        /* Beside the output, or with no -o (`-S` to stdout, say) named
         * after the source in the current directory, as GCC does: this
         * took strlen(NULL) and crashed. */
        const char *base = out;
        if (!base) {
            const char *sl = strrchr(in, '/');
            base = sl ? sl + 1 : in;
        }
        char *sup = xmalloc(strlen(base) + 4);
        const char *dot;
        /* Only functions that got code. One the inliner absorbed, or
         * that reachability dropped, still has a definition in the AST
         * and no frame — reporting it as zero would read as "this one
         * uses no stack" rather than "this one is not here". */
        /* A function with a variable-length array or alloca moves sp at
         * run time by an amount no frame size bounds: GCC calls that
         * "dynamic", and a stack analysis must not take its number as the
         * whole truth. */
        for (struct func *fn = u->funcs; fn; fn = fn->next)
            if (!fn->absorbed && fn->has_defn && fn->code_len > 0) {
                int dyn = 0;
                for (int k = 0; k < iu->nfuncs; k++)
                    if (iu->funcs[k].src == fn)
                        dyn = iu->funcs[k].has_alloca;
                ob_fmt(&sub, "%s:%d:%s\t%d\t%s\n",
                       in, fn->line, fn->name, fn->stack_bytes,
                       dyn ? "dynamic" : "static");
            }
        strcpy(sup, base);
        dot = strrchr(sup, '.');
        strcpy((char *)(dot && !strchr(dot, '/') ? dot : sup + strlen(sup)),
               ".su");
        if (plat_write_file(sup, (const unsigned char *)(sub.p ? sub.p : ""),
                            sub.n) != 0)
            fprintf(stderr, "embcc: cannot write '%s'\n", sup);
        free(sup);
        ob_free(&sub);
    }

    /* -fcallgraph-info: the calls left after optimisation, beside the
     * object, in GCC's .ci format. A function the inliner absorbed is not
     * a node, and its calls are its callers' now; a call through a pointer
     * goes to the __indirect_call placeholder, as GCC writes it. What the
     * IR cannot show -- a backend's call to a run-time helper for an
     * operation the target lacks -- is in the object's relocations, which
     * is where embrt looks for it. */
    if (want_callgraph) {
        struct outbuf cg = { NULL, 0, 0 };
        const char *base = out;
        if (!base) {
            const char *sl = strrchr(in, '/');
            base = sl ? sl + 1 : in;
        }
        char *cip = xmalloc(strlen(base) + 4);
        const char *dot;
        char *seen = xcalloc((size_t)(iu->nsyms ? iu->nsyms : 1), 1);
        int indirect_node = 0;
        ob_fmt(&cg, "graph: { title: \"%s\"\n", in);
        for (int k = 0; k < iu->nfuncs; k++) {
            struct func *fn = iu->funcs[k].src;
            if (!fn || fn->absorbed || !fn->has_defn || fn->code_len <= 0)
                continue;
            if (callgraph_su)
                ob_fmt(&cg, "node: { title: \"%s\" label: \"%s\\n%s:%d:%d"
                       "\\n%d bytes (%s)\" }\n", fn->name, fn->name, in,
                       fn->name_line ? fn->name_line : fn->line,
                       fn->name_col ? fn->name_col : 1, fn->stack_bytes,
                       iu->funcs[k].has_alloca ? "dynamic" : "static");
            else
                ob_fmt(&cg, "node: { title: \"%s\" label: \"%s\\n%s:%d:%d\" }\n",
                       fn->name, fn->name, in,
                       fn->name_line ? fn->name_line : fn->line,
                       fn->name_col ? fn->name_col : 1);
            for (int j = 0; j < iu->nsyms; j++)
                if (iu->syms[j].name && !strcmp(iu->syms[j].name, fn->name))
                    seen[j] = 1;
        }
        for (int k = 0; k < iu->nfuncs; k++) {
            struct ir_func *f = &iu->funcs[k];
            struct func *fn = f->src;
            if (!fn || fn->absorbed || !fn->has_defn || fn->code_len <= 0)
                continue;
            for (int n = 0; n < f->nins; n++) {
                const struct ir_ins *ci = &f->ins[n];
                const char *callee;
                if (ci->op != IR_CALL)
                    continue;
                if (ci->indirect || ci->callee_sym < 0 ||
                    ci->callee_sym >= iu->nsyms) {
                    if (!indirect_node) {
                        ob_fmt(&cg, "node: { title: \"__indirect_call\" label: "
                               "\"Indirect Call Placeholder\" shape : "
                               "ellipse }\n");
                        indirect_node = 1;
                    }
                    callee = "__indirect_call";
                } else {
                    callee = iu->syms[ci->callee_sym].name;
                    if (!seen[ci->callee_sym]) {
                        seen[ci->callee_sym] = 1;
                        ob_fmt(&cg, "node: { title: \"%s\" label: \"%s\" "
                               "shape : ellipse }\n", callee, callee);
                    }
                }
                ob_fmt(&cg, "edge: { sourcename: \"%s\" targetname: \"%s\" "
                       "label: \"%s:%d:%d\" }\n", fn->name, callee, in,
                       ci->line, ci->col);
            }
        }
        ob_fmt(&cg, "}\n");
        strcpy(cip, base);
        dot = strrchr(cip, '.');
        strcpy((char *)(dot && !strchr(dot, '/') ? dot : cip + strlen(cip)),
               ".ci");
        if (plat_write_file(cip, (const unsigned char *)cg.p, cg.n) != 0)
            fprintf(stderr, "embcc: cannot write '%s'\n", cip);
        free(seen);
        free(cip);
        ob_free(&cg);
    }

    /* Lay out the defined globals: initialized -> .data, zero -> .bss,
     * each aligned to its (element) size. A section("name") global goes to
     * its named section instead, in declaration order, zero-filled when it
     * has no initializer — PROGBITS like gcc's, NOBITS only for a .bss*
     * name. It is writable when anything in it is (or its name is .data or
     * .bss), and read-only when everything in it is const, as gcc makes
     * it. That is observable: a linker script places a writable orphan in
     * RAM, where the startup copies only .data, so a const table in a
     * section of its own (a command table, a driver list) read as zeros
     * until it was read-only and went to flash after .rodata. */
    struct named { const char *name; int len, align, nobits, flags, ndx;
                   char *buf; } *named = NULL;
    int nnamed = 0, capnamed = 0;
    /* -fdata-sections: the section each object would have gone to, as
     * one of its own. Decided as the layout below decides it: read-only
     * where it would be in .rodata, initialized data, or zeros. A
     * thread-local keeps .tdata/.tbss, which is what makes it one. */
    /* -fcommon (C, ELF): a TENTATIVE definition -- external, no
     * initializer, nothing saying where it goes -- is a COMMON symbol,
     * as GCC makes it: the linker merges every unit's `int x;` into one
     * object, the largest size and strictest alignment, and a real
     * definition elsewhere wins. A static, a thread-local, a weak one or
     * one with a section of its own stays a definition, as in GCC; a
     * const one is COMMON as GCC's is (it is zeros either way). */
    for (struct global *g = u->globals; g; g = g->next)
        g->is_common = g_fcommon && !lang_cxx &&
                       target_fmt_get() == TGT_FMT_ELF &&
                       !g->absorbed && g->defined && !g->has_init &&
                       !g->is_static && !g->is_tls && !g->section &&
                       !g->is_weak;
    if (data_sections && target_fmt_get() == TGT_FMT_ELF)
        for (struct global *g = u->globals; g; g = g->next) {
            if (g->absorbed || !g->defined || g->section || g->is_tls ||
                g->is_common)
                continue;
            const struct type *ot = g->ty;
            while (ot && ot->kind == TY_ARRAY)
                ot = ot->pointee;
            int ro = g->is_const && ot && !ot->is_volatile;
            g->section = sec_named(ro ? ".rodata." : g->has_init ? ".data."
                                                                 : ".bss.",
                                   g->name);
        }
    int data_len = 0, bss_len = 0;
    /* The alignment each output section must claim: the strictest of
     * anything placed in it. Emitting a fixed number here was silent
     * and wrong -- an object declared aligned(4096) was laid out
     * correctly WITHIN the section and then the linker was told the
     * section only needed 8, so it placed it wherever that allowed and
     * the object came out misaligned. Nothing failed; the address was
     * simply not what was asked for. */
    int data_align = 1, bss_align = 1;
    /* Thread-local objects get their own pair. The offsets recorded for
     * them are within the THREAD BLOCK, not within this image: the
     * linker gathers every .tdata/.tbss into PT_TLS, and each thread
     * gets a private copy made from that template. */
    int tdata_len = 0, tbss_len = 0, tdata_align = 1, tbss_align = 1;
    for (struct global *g = u->globals; g; g = g->next) {
        if (g->absorbed || !g->defined)
            continue;
        /* The object's own alignment is the stricter of its type's and
         * what it asked for with aligned(N) / _Alignas(N). */
        int align = ty_align(g->ty);
        if (g->user_align > align)
            align = g->user_align;
        align = target_object_align(g->ty && g->ty->kind == TY_ARRAY,
                                    global_size(g), align);
        g->in_bss = !g->has_init;
        g->named = 0;
        if (g->is_common) {    /* the linker places it (symbol below) */
            g->in_rodata = 0;
            g->off = align;    /* SHN_COMMON's st_value is the alignment */
            continue;
        }
        /* A const object is read-only data, where gcc puts it: in flash
         * on a microcontroller rather than copied into RAM at startup,
         * and write-protected under an MMU. Its place is fixed below,
         * after the string pool. Not for a volatile one (it may be a
         * device's), a thread's, one with a section of its own, or an
         * output format whose writer has no .rodata for it. */
        const struct type *ot = g->ty;
        while (ot && ot->kind == TY_ARRAY)
            ot = ot->pointee;
        g->in_rodata = g->is_const && !g->is_tls && !g->section &&
                       ot && !ot->is_volatile &&
                       target_fmt_get() == TGT_FMT_ELF;
        if (g->in_rodata) {
            g->in_bss = 0;
            continue;
        }
        int *len = g->in_bss ? &bss_len : &data_len;
        if (g->is_tls) {
            if (g->section)
                diag_fatal(g->file, g->line,
                           "'%s' is __thread and also names a section: a "
                           "thread-local object has to be in .tdata or "
                           ".tbss, which is what makes it per-thread",
                           g->name);
            len = g->in_bss ? &tbss_len : &tdata_len;
            int *al = g->in_bss ? &tbss_align : &tdata_align;
            if (align > *al)
                *al = align;
        } else if (g->section) {
            int k = 0;
            while (k < nnamed && strcmp(named[k].name, g->section) != 0)
                k++;
            if (k == nnamed) {
                if (nnamed == capnamed) {
                    capnamed = capnamed ? 2 * capnamed : 16;
                    named = xrealloc(named, (size_t)capnamed * sizeof *named);
                }
                const char *n = g->section;
                named[k].name = n;
                named[k].len = 0;
                named[k].align = 1;
                named[k].nobits = strncmp(n, ".bss", 4) == 0 &&
                                  (n[4] == 0 || n[4] == '.');
                named[k].flags = SHF_ALLOC;
                if (strncmp(n, ".text", 5) == 0)
                    named[k].flags |= SHF_EXECINSTR;
                else if (strncmp(n, ".data", 5) == 0 || named[k].nobits)
                    named[k].flags |= SHF_WRITE;
                named[k].buf = NULL;
                nnamed++;
            }
            if (named[k].nobits && g->has_init)
                diag_fatal(g->file, g->line,
                           "'%s' has an initializer but is placed in "
                           "NOBITS section '%s'", g->name, g->section);
            if (!(g->is_const && ot && !ot->is_volatile) &&
                !(named[k].flags & SHF_EXECINSTR) &&
                strncmp(g->section, ".rodata", 7) != 0)
                named[k].flags |= SHF_WRITE;
            g->named = k + 1;
            g->in_bss = named[k].nobits;
            len = &named[k].len;
            if (align > named[k].align)
                named[k].align = align;
        }
        *len = (*len + align - 1) & ~(align - 1);
        g->off = *len;
        *len += global_size(g);
        if (!g->is_tls && !g->section) {
            int *sa = g->in_bss ? &bss_align : &data_align;
            if (align > *sa)
                *sa = align;
        }
    }
    for (int k = 0; k < nnamed; k++)
        if (!named[k].nobits && named[k].len)
            named[k].buf = xcalloc(1, (size_t)named[k].len);
    char *data = NULL;
    if (data_len)
        data = xcalloc(1, (size_t)data_len);
    char *tdata = NULL;
    if (tdata_len)
        tdata = xcalloc(1, (size_t)tdata_len);
    if (data_len || nnamed || tdata_len) {
        for (struct global *g = u->globals; g; g = g->next) {
            if (g->absorbed || !g->defined || g->in_bss || g->in_rodata)
                continue;
            char *img = g->is_tls ? tdata
                      : g->named  ? named[g->named - 1].buf : data;
            if (!img)
                continue;                   /* a zero-length section */
            if (g->init_bytes) {
                int n = g->init_len;
                if (n > global_size(g))
                    n = global_size(g);
                memcpy(img + g->off, g->init_bytes, (size_t)n);
                continue;
            }
            unsigned long v = (unsigned long)g->init;
            target_put_uint((unsigned char *)img + g->off, ty_size(g->ty), v);
        }
    }

    /* Global initializers that point at string literals need those
     * strings in .rodata. Intern them now — before the image below is
     * built — so the pool includes them, and record each slot's target
     * offset for its relocation. Deduping shares a literal already used
     * in code. */
    for (struct global *g = u->globals; g; g = g->next) {
        if (g->absorbed || !g->defined)
            continue;
        for (int i = 0; i < g->nrelocs; i++) {
            if (!g->relocs[i].str)   /* a &global reloc needs no .rodata */
                continue;
            /* Already encoded at its real width: str_len elements of
             * str_width bytes (lit_encode). */
            int w = g->relocs[i].str_width ? g->relocs[i].str_width : 1;
            int si = ir_intern_aligned(iu, g->relocs[i].str,
                                       g->relocs[i].str_len * w,
                                       target_string_align(w));
            g->relocs[i].str_off = iu->strs[si].off;
        }
    }

    /* .rodata: the string literals, at the offsets irgen assigned, then
     * the const objects, each at its alignment. */
    int rodata_len = iu->rodata_len, rodata_align = 16;
    for (struct global *g = u->globals; g; g = g->next) {
        if (g->absorbed || !g->defined || !g->in_rodata)
            continue;
        int align = ty_align(g->ty);
        if (g->user_align > align)
            align = g->user_align;
        align = target_object_align(g->ty && g->ty->kind == TY_ARRAY,
                                    global_size(g), align);
        if (align > rodata_align)
            rodata_align = align;
        rodata_len = (rodata_len + align - 1) & ~(align - 1);
        g->off = rodata_len;
        rodata_len += global_size(g);
    }
    char *rodata = NULL;
    if (rodata_len) {
        rodata = xcalloc(1, (size_t)rodata_len);
        for (int i = 0; i < iu->nstrs; i++)
            memcpy(rodata + iu->strs[i].off, iu->strs[i].bytes,
                   (size_t)iu->strs[i].len);
        for (struct global *g = u->globals; g; g = g->next) {
            if (g->absorbed || !g->defined || !g->in_rodata)
                continue;
            if (g->init_bytes) {
                int n = g->init_len < global_size(g) ? g->init_len
                                                     : global_size(g);
                memcpy(rodata + g->off, g->init_bytes, (size_t)n);
            } else {
                unsigned long v = (unsigned long)g->init;
                target_put_uint((unsigned char *)rodata + g->off,
                                global_size(g) < 8 ? global_size(g) : 8, v);
            }
        }
    }

    /* File-scope asm blocks (crt0's _start): place each block's bytes in
     * .text after the functions (16-aligned) and record where, so its labels
     * and relocations land at the right offset. The blocks were already
     * assembled above (before irgen) and their call targets marked used. */
    for (struct topasm *ta = u->topasm; ta; ta = ta->next) {
        if (blocks_by_gas()) {
            /* what the block's own alignment directives asked for, at
             * least the target's instruction alignment; the padding is
             * never executed (the code before it ends in a return) */
            int al = target_get() == TARGET_AVR ? 2 : 4;
            code_align(&text, ta->align > al ? ta->align : al, 0);
        } else {
            code_align(&text, 16, 0x90);
        }
        ta->text_off = text.len;
        for (int k = 0; k < ta->codelen; k++)
            code_byte(&text, ta->code[k]);
        /* its literal pools and data words are data, for the mapping
         * symbols ($d) a disassembler and a linker read */
        for (int k = 0; k + 1 < ta->ndrange; k += 2)
            code_mark_data(&text, ta->text_off + ta->drange[k],
                           ta->text_off + ta->drange[k + 1]);
    }

    /* -g: build the DWARF line sections now (needs each func's code_off/len,
     * set by codegen). Off, dw stays empty and nothing below fires. */
    struct dwarf_out dw = { { 0 }, { 0 }, 0, 0, 0 };
    if (want_debug) {
        int split = 0;
        for (int i = 0; i < iu->nfuncs && !split; i++)
            split = iu->funcs[i].src && iu->funcs[i].src->section &&
                    iu->funcs[i].src->code_len > 0;
        if (split)
            dwarf_emit_split(iu, in, &dw);
        else
            dwarf_emit(iu, in, &dw);
    }
    struct eh_out eh;
    memset(&eh, 0, sizeof eh);
    /* Unwind tables: asked for, or C++, or a HOSTED target.
     *
     * The last one is the gcc default and it is not a preference. An
     * exception unwinds through whatever frames lie between the throw
     * and the catch, and some of them are C -- qsort's comparison
     * callback, a libc routine that calls back, and above all the
     * unwinder's OWN frames, which are C and which the walk has to step
     * out of before it can reach anything else. Without a table for
     * those the walk stops at the first one and every throw becomes
     * std::terminate, which is precisely what happened when lib/rt's
     * unwinder was first run.
     *
     * Freestanding targets keep the old default of off: there is
     * nothing to unwind into on a kernel's stack, and the tables are
     * pure size there. */
    int unwind = want_unwind > 0 ||
                 (lang_cxx && (want_unwind < 0 || want_exceptions)) ||
                 (want_unwind < 0 && target_is_hosted() &&
                  target_fmt_get() == TGT_FMT_ELF);
    /* eh_emit writes x86-64's and AArch64's tables -- their register
     * numbers, their CFA rules -- and nothing else's. Everywhere else a C++
     * unit's default request is dropped when nothing can read the tables
     * (-fno-exceptions): it used to reach eh_emit on RV64, which wrote an
     * x86-64 CIE into a RISC-V object, and to be refused on MIPS64, which
     * stopped every C++ unit there. An explicit request, or exceptions,
     * is refused by name below. */
    const char *unwritten = backend_get(ta)->unwind_unwritten;
    if (unwind && unwritten && lang_cxx && want_unwind < 0 && !want_exceptions)
        unwind = 0;
    /* An explicit request, or C++ with exceptions, where the tables are
     * not written: refused by name, naming the tables (the registry's
     * row). eh_emit would write an x86-64 CIE into the object. */
    if (unwind && unwritten)
        diag_fatal(NULL, 0, "unwind tables are not supported for "
                            "%s yet (-funwind-tables, "
                            "-fasynchronous-unwind-tables, -fexceptions%s): "
                            "EmbCC writes no %s",
                   target_triple_now(),
                   lang_cxx && want_exceptions
                       ? ", and C++ without -fno-exceptions" : "",
                   unwritten);
    if (unwind)
        eh_emit(iu, ta == TARGET_AARCH64, &eh);

    /* -S: the same bytes, as text (src/driver/asmout.c). Everything the
     * emitter needs is in hand here -- the code, the string pool, and the
     * relocation sites the backend recorded. */
    /* A target with no -S text (the registry's no_asm_text: Xtensa has no
     * assembler here to check the text against, and RX no disassembler
     * to write it) is refused by name rather than written unverified. */
    const char *no_asm_text = backend_get(ta)->no_asm_text;
    if (want_asm && no_asm_text)
        diag_fatal(NULL, 0, "-S is not supported for %s yet: %s",
                   target_triple_now(), no_asm_text);
    /* -Wa,-a...: the listing, the same text as -S, beside the object */
    if (g_listing && !want_asm) {
        if (no_asm_text) {
            fprintf(stderr, "embcc: warning: no assembler listing for %s "
                            "(-Wa,-a...): EmbCC has no -S text for it\n",
                    target_triple_now());
        } else {
            struct outbuf lb = { NULL, 0, 0 };
            asm_emit_unit(&lb, in, u, iu, (const unsigned char *)text.p,
                          text.len, (const unsigned char *)rodata,
                          ext, next, strs, nstrs, gs, ngs, fs, nfs);
            int lrc = strcmp(g_listing, "-")
                      ? plat_write_file(g_listing, lb.p, lb.n)
                      : (fwrite(lb.p, 1, lb.n, stdout), 0);
            if (lrc != 0)
                diag_fatal(g_listing, 0, "cannot write the listing");
            ob_free(&lb);
        }
    }
    if (want_asm) {
        /* Every target, now. What -S emits is the OBJECT's bytes as
         * .byte directives with the relocations attached explicitly --
         * not a re-rendering that an assembler would be free to encode
         * differently -- so the only per-target knowledge it needs is
         * how to group the bytes into instructions and what to call
         * each relocation. Both are answered in src/arch/target.c and
         * asmout.c now, and asmout refuses by name for anything it
         * cannot spell. The x86-64 disassembly comment stays x86-64's;
         * the other targets get the bytes without a commentary that
         * would have to be guessed. */
        struct outbuf ab = { NULL, 0, 0 };
        asm_emit_unit(&ab, in, u, iu, (const unsigned char *)text.p,
                      text.len, (const unsigned char *)rodata,
                      ext, next, strs, nstrs, gs, ngs, fs, nfs);
        int arc = out ? plat_write_file(out, ab.p, ab.n)
                      : (fwrite(ab.p, 1, ab.n, stdout), 0);
        if (arc != 0)
            diag_fatal(out, 0, "cannot write the file");
        ob_free(&ab);
        return 0;
    }

    /* ---- Mach-O (D-014) ------------------------------------------------
     *
     * A parallel path rather than a shared one, for the reason D-011
     * gave when the second backend arrived: the common shape is derived
     * from two WORKING implementations, not invented from one. Until
     * this emits everything the ELF path does, what they share is a
     * guess.
     *
     * Refused loudly rather than emitted wrong (THE RULE): debug info,
     * whose DWARF lives in a __DWARF segment with its own section names
     * and relocation rules, and the unwind tables, whose "this address
     * minus that one" is a SUBTRACTOR/UNSIGNED pair here rather than
     * one relocation.
     */
    /* Only the ELF writer and -S define an alias's symbol. */
    if (target_fmt_get() != TGT_FMT_ELF)
        for (struct func *f = u->funcs; f; f = f->next)
            if (!f->absorbed && f->alias_of)
                diag_fatal(f->file, f->line, "alias attribute on '%s' is "
                           "not supported for %s output", f->name,
                           target_fmt_name(target_fmt_get()));
    if (target_fmt_get() == TGT_FMT_MACHO) {
        if (want_debug)
            diag_fatal(in, 0,
                       "-g is not supported for a Darwin target yet: its "
                       "DWARF goes in a __DWARF segment this does not "
                       "write, and emitting the ELF layout under a Mach-O "
                       "name would be worse than refusing");
        /* A file-scope asm block's BYTES reach __text below, but its
         * labels and relocations are written by the ELF path further
         * down and have no Mach-O counterpart yet. Leaving that alone
         * would produce an object whose `_start` had no symbol and
         * whose `.quad main` was eight zeroes -- machine code that
         * links and jumps to address zero. So it is refused by name. */
        for (struct topasm *tas = u->topasm; tas; tas = tas->next)
            if (tas->nsyms || tas->nrels)
                diag_fatal(tas->file, tas->line,
                           "a file-scope asm block with labels or symbol "
                           "references is not supported for a Darwin "
                           "target yet: its bytes would be emitted but "
                           "its symbols and relocations dropped");
        /* Mach-O has no .tdata/.tbss and no PT_TLS. Its thread-locals
         * go through __thread_vars descriptors and a call to
         * tlv_get_addr, which is a different mechanism rather than a
         * different spelling -- and this writer emits none of it, so a
         * __thread object silently DISAPPEARED from the output
         * entirely. Refused by name instead. */
        for (struct global *g = u->globals; g; g = g->next)
            if (!g->absorbed && g->is_tls)
                diag_fatal(g->file, g->line,
                           "__thread is not supported for a Darwin target "
                           "yet: Mach-O addresses a thread-local through "
                           "a __thread_vars descriptor, which this writer "
                           "does not emit");
        /* Mach-O gathers these from __DATA,__mod_init_func rather than
         * from an SHT_INIT_ARRAY section, and this writer emits no such
         * section. Dropping them would give back exactly the silent
         * failure this attribute was implemented to end. */
        for (struct func *f = u->funcs; f; f = f->next)
            if (!f->absorbed && (f->is_ctor || f->is_dtor))
                diag_fatal(f->file, f->line,
                           "__attribute__((%s)) is not supported for a "
                           "Darwin target yet: it needs a "
                           "__DATA,__mod_init_func section this Mach-O "
                           "writer does not emit",
                           f->is_ctor ? "constructor" : "destructor");
        struct machow *mw = machow_new(
            ta == TARGET_AARCH64 ? CPU_TYPE_ARM64 : CPU_TYPE_X86_64,
            ta == TARGET_AARCH64 ? CPU_SUBTYPE_ARM64_ALL
                                 : CPU_SUBTYPE_X86_64_ALL);

        /* ---- 1. the bytes, before any section is handed over ---------
         *
         * machow_add_section COPIES a section's contents as it takes
         * them, so every field that carries a value has to hold it
         * already. Mach-O relocations have no addend of their own: the
         * value lives in the word being patched. */
        for (struct global *g = u->globals; g; g = g->next) {
            if (g->absorbed || !g->defined || g->in_bss || g->named)
                continue;
            for (int i = 0; i < g->nrelocs; i++) {
                long add = g->relocs[i].ftarget || g->relocs[i].gtarget
                           ? g->relocs[i].addend
                           : g->relocs[i].str_off + g->relocs[i].addend;
                long at = g->off + g->relocs[i].off;
                if (!data || at < 0 || at + 8 > data_len)
                    continue;
                unsigned char *d = (unsigned char *)data + at;
                for (int b = 0; b < 8; b++)
                    d[b] = (unsigned char)((unsigned long long)add >> (b * 8));
            }
        }
        if (unwind)
            for (int i = 0; i < eh.nrelocs; i++) {
                struct eh_reloc *r = &eh.relocs[i];
                if (!r->in_lsda || !eh.lsda.p ||
                    (size_t)r->off + 4 > (size_t)eh.lsda.len)
                    continue;
                unsigned char *f = (unsigned char *)eh.lsda.p + r->off;
                /* A type-table entry is PC-relative and the linker
                 * measures it from the slot, so the field carries
                 * nothing. Everything else is "target minus here",
                 * which a SUBTRACTOR pair answers against an anchor at
                 * the section's start -- so the field turns that
                 * anchor-relative answer into a field-relative one. */
                long v = r->target == EHT_GLOBAL ? 0 : r->addend - r->off;
                for (int b = 0; b < 4; b++)
                    f[b] = (unsigned char)((unsigned long)v >> (b * 8));
            }

        /* ---- 2. every section, before any symbol ---------------------
         * A symbol's n_value is an ADDRESS, so the section it sits in
         * has to have one first. */
        int m_text = machow_add_section(mw, "__TEXT", "__text",
                                        S_REGULAR | S_ATTR_PURE_INSTRUCTIONS |
                                        S_ATTR_SOME_INSTRUCTIONS,
                                        text.p, text.len, 4);
        int m_rodata = 0, m_data = 0, m_bss = 0, m_lsda = 0, m_cu = 0;
        if (rodata)
            m_rodata = machow_add_section(mw, "__TEXT", "__const", S_REGULAR,
                                          rodata, iu->rodata_len, 0);
        /* Mach-O records the alignment as a LOG2, so the same number
         * that is 4096 in ELF is 12 here -- and a fixed 3 (eight bytes)
         * discarded whatever an object had asked for. */
        if (data_len)
            m_data = machow_add_section(mw, "__DATA", "__data", S_REGULAR,
                                        data, (unsigned long long)data_len,
                                        log2_align(data_align));
        if (bss_len)
            m_bss = machow_add_section(mw, "__DATA", "__bss", S_ZEROFILL,
                                       NULL, (unsigned long long)bss_len,
                                       log2_align(bss_align));
        for (int k = 0; k < nnamed; k++)
            named[k].ndx = machow_add_section(
                mw, "__DATA", named[k].name,
                named[k].nobits ? S_ZEROFILL : S_REGULAR,
                named[k].nobits ? NULL : named[k].buf,
                (unsigned long long)named[k].len,
                named[k].align <= 1 ? 0 : named[k].align <= 2 ? 1
                : named[k].align <= 4 ? 2 : named[k].align <= 8 ? 3 : 4);

        /* Darwin does not use .eh_frame -- its linker refuses a CIE that
         * names a personality routine, and clang emits none. The unwind
         * description is __LD,__compact_unwind: one 32-byte entry per
         * function, which ld folds into the __unwind_info the system
         * unwinder reads. One word describes a whole function, which
         * works because every prologue EmbCC emits has one shape. */
        unsigned char *cu = NULL;
        long ncu = unwind ? eh.nfuncs : 0;
        if (ncu) {
            if (eh.lsda.len)
                m_lsda = machow_add_section(mw, "__TEXT", "__gcc_except_tab",
                                            S_REGULAR, eh.lsda.p,
                                            (unsigned long long)eh.lsda.len,
                                            2);
            unsigned long long text_at = machow_section_addr(mw, m_text);
            unsigned long long lsda_at = m_lsda
                ? machow_section_addr(mw, m_lsda) : 0;
            cu = xcalloc((size_t)ncu, 32);
            for (long k = 0; k < ncu; k++) {
                unsigned char *e = cu + k * 32;
                unsigned long long fa =
                    text_at + (unsigned long long)eh.funcs[k].code_off;
                unsigned long len = (unsigned long)eh.funcs[k].code_len;
                unsigned long enc = ta == TARGET_AARCH64 ? 0x04000000ul
                                                         : 0x01000000ul;
                if (eh.funcs[k].lsda_off >= 0)
                    enc |= 0x40000000ul;         /* UNWIND_HAS_LSDA */
                for (int b = 0; b < 8; b++) e[b] = (unsigned char)(fa >> (b*8));
                for (int b = 0; b < 4; b++) e[8+b] = (unsigned char)(len >> (b*8));
                for (int b = 0; b < 4; b++) e[12+b] = (unsigned char)(enc >> (b*8));
                if (eh.funcs[k].lsda_off >= 0) {
                    unsigned long long la =
                        lsda_at + (unsigned long long)eh.funcs[k].lsda_off;
                    for (int b = 0; b < 8; b++)
                        e[24+b] = (unsigned char)(la >> (b*8));
                }
            }
            m_cu = machow_add_section(mw, "__LD", "__compact_unwind",
                                      S_REGULAR | 0x02000000u /* S_ATTR_DEBUG */,
                                      cu, (unsigned long long)ncu * 32, 3);
            free(cu);
        }

        /* ---- 3. LOCAL symbols, all of them, first --------------------
         *
         * Mach-O orders its symbol table locals, defined externals,
         * undefined -- and ld reads it that way whether or not anything
         * declares the ranges. A local added after an undefined one is
         * not an error in the file; it is a call that lands in another
         * section. machow_add_symbol now refuses it outright, which is
         * how this ordering came to be written down rather than
         * discovered twice. */
        int m_rodata_sym = m_rodata
            ? machow_add_symbol_raw(mw, "ltmp_const", 0, m_rodata, 0) : 0;
        int lsda_anchor = m_lsda
            ? machow_add_symbol_raw(mw, "ltmp_lsda", 0, m_lsda, 0) : 0;
        int text_anchor = ncu
            ? machow_add_symbol_raw(mw, "ltmp_text", 0, m_text, 0) : 0;
        for (struct func *f = u->funcs; f; f = f->next)
            if (!f->absorbed && f->has_defn && f->is_static && f->used)
                f->sym_ndx = machow_add_symbol(mw, f->name,
                                               (unsigned long long)f->code_off,
                                               m_text, 0);
        for (struct global *g = u->globals; g; g = g->next)
            if (!g->absorbed && g->defined && g->is_static)
                g->sym_ndx = machow_add_symbol(
                    mw, g->name, (unsigned long long)g->off,
                    g->named ? named[g->named - 1].ndx
                             : g->in_bss ? m_bss : m_data, 0);

        /* ---- 4. defined externals ------------------------------------ */
        for (struct func *f = u->funcs; f; f = f->next)
            if (!f->absorbed && f->has_defn && !f->is_static)
                f->sym_ndx = (f->is_weak ? machow_add_symbol_weak
                                         : machow_add_symbol)(
                    mw, f->name, (unsigned long long)f->code_off, m_text, 1);
        for (struct global *g = u->globals; g; g = g->next)
            if (!g->absorbed && g->defined && !g->is_static)
                g->sym_ndx = (g->is_weak ? machow_add_symbol_weak
                                         : machow_add_symbol)(
                    mw, g->name, (unsigned long long)g->off,
                    g->named ? named[g->named - 1].ndx
                             : g->in_bss ? m_bss : m_data, 1);

        /* ---- 5. undefined, all of them, before any relocation --------
         * Created up front rather than lazily where each is first
         * needed, because a lazily created one would land after the
         * relocations that precede it and break the ordering above. */
        for (struct global *g = u->globals; g; g = g->next)
            if (!g->absorbed && !g->defined && g->used)
                g->sym_ndx = (g->is_weak ? machow_add_symbol_weak
                                         : machow_add_symbol)(
                    mw, g->name, 0, 0, 1);
        for (int i = 0; i < next; i++)
            if (!ext[i].callee->sym_ndx)
                ext[i].callee->sym_ndx =
                    (ext[i].callee->is_weak ? machow_add_symbol_weak
                                            : machow_add_symbol)(
                        mw, ext[i].callee->name, 0, 0, 1);
        for (int i = 0; i < nfs; i++)
            if (!fs[i].target->sym_ndx)
                fs[i].target->sym_ndx =
                    (fs[i].target->is_weak ? machow_add_symbol_weak
                                           : machow_add_symbol)(
                        mw, fs[i].target->name, 0, 0, 1);
        for (struct global *g = u->globals; g; g = g->next) {
            if (g->absorbed || !g->defined || g->in_bss)
                continue;
            for (int i = 0; i < g->nrelocs; i++) {
                struct func *ft = g->relocs[i].ftarget;
                if (ft && !ft->sym_ndx)
                    ft->sym_ndx = (ft->is_weak ? machow_add_symbol_weak
                                               : machow_add_symbol)(
                        mw, ft->name, 0, 0, 1);
            }
        }
        int pers = 0;
        if (ncu)
            for (long k = 0; k < ncu; k++)
                if (eh.funcs[k].lsda_off >= 0) {
                    pers = machow_add_symbol(mw, "__gxx_personality_v0",
                                             0, 0, 1);
                    break;
                }

        /* ---- 6. relocations ------------------------------------------ */
        int mpc = 0, mlen = 2, mt;
        for (int i = 0; i < next; i++) {
            mt = target_macho_reloc(ta, RK_CALL, &mpc, &mlen);
            if (!mt)
                diag_fatal(in, 0, "no Mach-O relocation for a call here");
            machow_add_reloc(mw, m_text,
                             (unsigned long long)ext[i].patch_off,
                             ext[i].callee->sym_ndx, mt - 1, mpc, mlen, 0);
        }
        free(ext);

        /* The offset goes in UNBIASED -- see target_macho_reloc's note:
         * Mach-O accounts for the instruction's length itself, so ELF's
         * -4 here would move every reference four bytes. */
        for (int i = 0; i < nstrs; i++) {
            mt = target_macho_reloc(ta, strs[i].kind, &mpc, &mlen);
            if (!mt)
                diag_fatal(in, 0,
                           "no Mach-O relocation for a string reference here");
            machow_add_reloc(mw, m_text,
                             (unsigned long long)strs[i].patch_off,
                             m_rodata_sym, mt - 1, mpc, mlen, strs[i].str_off);
        }
        free(strs);

        for (int i = 0; i < ngs; i++) {
            mt = target_macho_reloc(ta, gs[i].kind, &mpc, &mlen);
            if (!mt)
                diag_fatal(in, 0,
                           "no Mach-O relocation for a global reference here");
            machow_add_reloc(mw, m_text, (unsigned long long)gs[i].patch_off,
                             gs[i].glob->sym_ndx, mt - 1, mpc, mlen, 0);
        }
        free(gs);

        for (int i = 0; i < nfs; i++) {
            mt = target_macho_reloc(ta, fs[i].kind, &mpc, &mlen);
            if (!mt)
                diag_fatal(in, 0,
                           "no Mach-O relocation for a function address here");
            machow_add_reloc(mw, m_text, (unsigned long long)fs[i].patch_off,
                             fs[i].target->sym_ndx, mt - 1, mpc, mlen, 0);
        }
        free(fs);

        for (struct global *g = u->globals; g; g = g->next) {
            if (g->absorbed || !g->defined || g->in_bss)
                continue;
            for (int i = 0; i < g->nrelocs; i++) {
                struct func *ft = g->relocs[i].ftarget;
                int sym;
                if (ft)
                    sym = ft->sym_ndx;
                else if (g->relocs[i].gtarget)
                    sym = g->relocs[i].gtarget->sym_ndx;
                else
                    sym = m_rodata_sym;
                if (!sym)
                    continue;
                mt = target_macho_reloc(ta, RK_ABS64, &mpc, &mlen);
                machow_add_reloc(mw,
                                 g->named ? named[g->named - 1].ndx : m_data,
                                 (unsigned long long)(g->off +
                                                      g->relocs[i].off),
                                 sym, mt - 1, mpc, mlen, 0);
            }
        }

        if (ncu) {
            for (long k = 0; k < ncu; k++) {
                machow_add_reloc_sect(mw, m_cu, (unsigned long long)(k * 32),
                                      m_text, 3);
                if (eh.funcs[k].lsda_off < 0)
                    continue;
                machow_add_reloc(mw, m_cu,
                                 (unsigned long long)(k * 32 + 16), pers,
                                 ta == TARGET_AARCH64 ? ARM64_RELOC_UNSIGNED
                                                      : X86_64_RELOC_UNSIGNED,
                                 0, 3, 0);
                if (m_lsda)
                    machow_add_reloc_sect(mw, m_cu,
                                          (unsigned long long)(k * 32 + 24),
                                          m_lsda, 3);
            }
            if (m_lsda)
                for (int i = 0; i < eh.nrelocs; i++) {
                    struct eh_reloc *r = &eh.relocs[i];
                    if (!r->in_lsda)
                        continue;
                    if (r->target == EHT_GLOBAL && r->glob &&
                        r->glob->sym_ndx) {
                        /* Darwin names a typeinfo INDIRECTLY -- the slot
                         * holds the offset to a GOT entry pointing at
                         * it -- so the relocation is POINTER_TO_GOT and
                         * the encoding beside it says 0x9b (eh.c). */
                        machow_add_reloc(mw, m_lsda,
                                         (unsigned long long)r->off,
                                         r->glob->sym_ndx,
                                         ta == TARGET_AARCH64
                                             ? ARM64_RELOC_POINTER_TO_GOT
                                             : X86_64_RELOC_GOT,
                                         1, 2, 0);
                    } else if (r->target == EHT_TEXT && text_anchor) {
                        machow_add_reloc_sub(mw, m_lsda,
                                             (unsigned long long)r->off,
                                             lsda_anchor, text_anchor, 2);
                    }
                }
        }
        eh_free(&eh);

        int mrc = machow_write(mw, out);
        machow_free(mw);
        return mrc != 0;
    }

    if (target_fmt_get() == TGT_FMT_COFF) {
        /* ---- Windows: COFF (D-014) -------------------------------------
         *
         * The same three steps the Mach-O branch above takes, and for
         * the same reason: a COFF relocation carries NO ADDEND, so
         * every value has to be in the field before the section is
         * handed over, and coffw_add_section copies as it takes.
         *
         * What differs from Mach-O is happier. Sections are numbered
         * from one and a symbol names one directly, so there is no
         * address arithmetic to get wrong and no local-before-global
         * ordering to violate. And REL32 is measured from the END of
         * the instruction, which is what an x86 rel32 means anyway --
         * so the -4 that ELF's PC32 needs is absent here, and carrying
         * it over would displace every call by four bytes.
         */
        if (want_debug)
            diag_fatal(in, 0,
                       "-g is not supported for a Windows target yet: its "
                       "debug information goes in CodeView records this "
                       "does not write, and emitting DWARF under a COFF "
                       "name would be worse than refusing");
        for (struct topasm *tas = u->topasm; tas; tas = tas->next)
            if (tas->nsyms || tas->nrels)
                diag_fatal(tas->file, tas->line,
                           "a file-scope asm block with labels or symbol "
                           "references is not supported for a Windows "
                           "target yet");
        for (struct func *f = u->funcs; f; f = f->next)
            if (!f->absorbed && (f->is_ctor || f->is_dtor))
                diag_fatal(f->file, f->line,
                           "__attribute__((%s)) is not supported for a "
                           "Windows target yet: it needs the .ctors/.dtors "
                           "sections this COFF writer does not emit",
                           f->is_ctor ? "constructor" : "destructor");
        for (struct global *g = u->globals; g; g = g->next)
            if (!g->absorbed && g->is_tls)
                diag_fatal(g->file, g->line,
                           "__thread is not supported for a Windows target "
                           "yet: Windows reaches a thread-local through a "
                           "_tls_index and a TLS directory this writer "
                           "does not emit");
        /* The COFF writer has no named data sections: an object with one
         * was given offset 0 of .data and laid over whatever was already
         * there, so a store to either changed both. */
        for (struct global *g = u->globals; g; g = g->next)
            if (!g->absorbed && g->defined && g->section)
                diag_fatal(g->file, g->line,
                           "'%s': a variable's section attribute is not "
                           "supported for COFF output", g->name);
        if (unwind)
            diag_fatal(in, 0,
                       "C++ exceptions are not supported for a Windows "
                       "target yet: the unwind tables go in .pdata and "
                       ".xdata and neither is written");

        /* The object format is right and the argument registers are
         * now Microsoft's (refereed against clang by win-abi.sh), but
         * the ABI around them is not: the data model is still LP64
         * where Windows is LLP64, wchar_t is four bytes where it is
         * two, and rsi, rdi, xmm6 and xmm7 -- which a Windows callee
         * must preserve -- are used freely. Objects EmbCC compiles
         * are consistent with each other, so a self-contained program
         * works -- and a struct holding a `long`, or a caller in
         * MinGW's libc keeping a double in xmm6, does not.
         *
         * (This used to name the argument registers as the gap, which
         * stopped being true when the convention went in.)
         *
         * D-014 says a capability a triple lacks must be an error
         * naming the triple rather than a silent fallback to another
         * platform's behaviour, and this IS that fallback. It is a
         * warning rather than an error only because refusing would
         * leave the object writer untestable; it is on by default and
         * fires on every Windows compile, so it cannot be mistaken for
         * a finished target. */
        diag_warn_opt(in, 0, 0, "windows-abi",
                      "%s is not yet the Microsoft x64 ABI: `long` is "
                      "8 bytes and wchar_t 4 (Windows has 4 and 2), and "
                      "rsi, rdi, xmm6 and xmm7 are not preserved across "
                      "a call: "
                      "objects EmbCC compiles agree with each other and "
                      "with nothing else",
                      target_triple_now());

        /* 1. the bytes, before any section is handed over. */
        for (struct global *g = u->globals; g; g = g->next) {
            if (g->absorbed || !g->defined || g->in_bss || g->named)
                continue;
            for (int i = 0; i < g->nrelocs; i++) {
                long add = g->relocs[i].ftarget || g->relocs[i].gtarget
                           ? g->relocs[i].addend
                           : g->relocs[i].str_off + g->relocs[i].addend;
                long at = g->off + g->relocs[i].off;
                if (!data || at < 0 || at + 8 > data_len)
                    continue;
                unsigned char *d = (unsigned char *)data + at;
                for (int b = 0; b < 8; b++)
                    d[b] = (unsigned char)((unsigned long long)add >> (b * 8));
            }
        }
        /* A string's offset within .rdata goes in the rel32 field:
         * COFF computes S + field - (here + 4), and S is the section. */
        for (int i = 0; i < nstrs; i++) {
            long at = strs[i].patch_off;
            if (at < 0 || at + 4 > (long)text.len)
                continue;
            for (int b = 0; b < 4; b++)
                text.p[at + b] =
                    (unsigned char)((unsigned long)strs[i].str_off >> (b * 8));
        }

        struct coffw *cw = coffw_new(IMAGE_FILE_MACHINE_AMD64);

        /* 2. every section. */
        int c_text = coffw_add_section(cw, ".text",
                                       IMAGE_SCN_CNT_CODE |
                                       IMAGE_SCN_MEM_EXECUTE |
                                       IMAGE_SCN_MEM_READ,
                                       text.p, (unsigned)text.len, 16);
        int c_rdata = 0, c_data = 0, c_bss = 0;
        if (rodata)
            c_rdata = coffw_add_section(cw, ".rdata",
                                        IMAGE_SCN_CNT_INITIALIZED_DATA |
                                        IMAGE_SCN_MEM_READ,
                                        rodata, (unsigned)iu->rodata_len, 16);
        if (data_len)
            c_data = coffw_add_section(cw, ".data",
                                       IMAGE_SCN_CNT_INITIALIZED_DATA |
                                       IMAGE_SCN_MEM_READ |
                                       IMAGE_SCN_MEM_WRITE,
                                       data, (unsigned)data_len, data_align);
        if (bss_len)
            c_bss = coffw_add_section(cw, ".bss",
                                      IMAGE_SCN_CNT_UNINITIALIZED_DATA |
                                      IMAGE_SCN_MEM_READ |
                                      IMAGE_SCN_MEM_WRITE,
                                      NULL, (unsigned)bss_len, bss_align);

        /* 3. symbols. .rdata gets a section symbol, because a string's
         * address is relocated against the SECTION rather than against
         * a name of its own. sym_ndx is kept one-based here as in the
         * other branches, so that zero still means "none yet". */
        int c_rdata_sym = 0;
        if (c_rdata)
            c_rdata_sym = coffw_add_symbol(cw, ".rdata", 0, c_rdata,
                                           IMAGE_SYM_TYPE_NULL,
                                           IMAGE_SYM_CLASS_STATIC);
        for (struct func *f = u->funcs; f; f = f->next)
            if (!f->absorbed && f->has_defn && (!f->is_static || f->used))
                f->sym_ndx = coffw_add_symbol(
                    cw, f->name, (unsigned)f->code_off, c_text,
                    IMAGE_SYM_DTYPE_FUNCTION,
                    f->is_static ? IMAGE_SYM_CLASS_STATIC
                                 : IMAGE_SYM_CLASS_EXTERNAL) + 1;
        for (struct global *g = u->globals; g; g = g->next) {
            if (g->absorbed || !g->defined)
                continue;
            int sect = g->in_bss ? c_bss : c_data;
            if (!sect)
                continue;
            g->sym_ndx = coffw_add_symbol(
                cw, g->name, (unsigned)g->off, sect, IMAGE_SYM_TYPE_NULL,
                g->is_static ? IMAGE_SYM_CLASS_STATIC
                             : IMAGE_SYM_CLASS_EXTERNAL) + 1;
        }
        /* Undefined: section zero with a ZERO value. A nonzero value
         * there would make it a COMMON rather than a reference. */
        for (struct global *g = u->globals; g; g = g->next)
            if (!g->absorbed && !g->defined && g->used)
                g->sym_ndx = coffw_add_symbol(
                    cw, g->name, 0, IMAGE_SYM_UNDEFINED,
                    IMAGE_SYM_TYPE_NULL, IMAGE_SYM_CLASS_EXTERNAL) + 1;

        /* 4. relocations. */
        for (int i = 0; i < next; i++) {
            struct func *callee = ext[i].callee;
            if (!callee->sym_ndx)
                callee->sym_ndx = coffw_add_symbol(
                    cw, callee->name, 0, IMAGE_SYM_UNDEFINED,
                    IMAGE_SYM_DTYPE_FUNCTION,
                    IMAGE_SYM_CLASS_EXTERNAL) + 1;
            coffw_add_reloc(cw, c_text, (unsigned)ext[i].patch_off,
                            callee->sym_ndx - 1,
                            target_coff_reloc(ta, RK_CALL));
        }
        for (int i = 0; i < nstrs; i++)
            coffw_add_reloc(cw, c_text, (unsigned)strs[i].patch_off,
                            c_rdata_sym, target_coff_reloc(ta, strs[i].kind));
        for (int i = 0; i < ngs; i++) {
            struct global *g = gs[i].glob;
            if (!g->sym_ndx)
                g->sym_ndx = coffw_add_symbol(
                    cw, g->name, 0, IMAGE_SYM_UNDEFINED,
                    IMAGE_SYM_TYPE_NULL, IMAGE_SYM_CLASS_EXTERNAL) + 1;
            coffw_add_reloc(cw, c_text, (unsigned)gs[i].patch_off,
                            g->sym_ndx - 1, target_coff_reloc(ta, gs[i].kind));
        }
        for (int i = 0; i < nfs; i++) {
            struct func *tf = fs[i].target;
            if (!tf->sym_ndx)
                tf->sym_ndx = coffw_add_symbol(
                    cw, tf->name, 0, IMAGE_SYM_UNDEFINED,
                    IMAGE_SYM_DTYPE_FUNCTION,
                    IMAGE_SYM_CLASS_EXTERNAL) + 1;
            coffw_add_reloc(cw, c_text, (unsigned)fs[i].patch_off,
                            tf->sym_ndx - 1, target_coff_reloc(ta, fs[i].kind));
        }
        /* Pointer slots in .data holding an address. */
        for (struct global *g = u->globals; g; g = g->next) {
            if (g->absorbed || !g->defined || g->in_bss || !c_data)
                continue;
            for (int i = 0; i < g->nrelocs; i++) {
                struct func *ft = g->relocs[i].ftarget;
                int sym;
                if (ft) {
                    if (!ft->sym_ndx)
                        ft->sym_ndx = coffw_add_symbol(
                            cw, ft->name, 0, IMAGE_SYM_UNDEFINED,
                            IMAGE_SYM_DTYPE_FUNCTION,
                            IMAGE_SYM_CLASS_EXTERNAL) + 1;
                    sym = ft->sym_ndx - 1;
                } else if (g->relocs[i].gtarget) {
                    sym = g->relocs[i].gtarget->sym_ndx - 1;
                } else {
                    sym = c_rdata_sym;
                }
                coffw_add_reloc(cw, c_data,
                                (unsigned)(g->off + g->relocs[i].off), sym,
                                target_coff_reloc(ta, RK_ABS64));
            }
        }
        free(ext); free(strs); free(gs); free(fs);
        int crc = coffw_write(cw, out);
        coffw_free(cw);
        return crc != 0;
    }

    if (!object_format_ready())
        return 1;
    struct elfw *w = elfw_new(target_elf_machine(target_get()));
    elfw_set_flags(w, target_elf_flags(target_get()));
    /* .text is the code buffer less its groups, and each group a section
     * of its own (g_tg) */
    unsigned char *tbytes = (unsigned char *)text.p;
    long tlen = (long)text.len;
    if (g_ntg) {
        tlen = g_plain_end + ((long)text.len - g_groups_end);
        tbytes = xmalloc((size_t)(tlen ? tlen : 1));
        memcpy(tbytes, text.p, (size_t)g_plain_end);
        memcpy(tbytes + g_plain_end, (unsigned char *)text.p + g_groups_end,
               (size_t)((long)text.len - g_groups_end));
    }
    int text_ndx = elfw_add_section(w, ".text", SHT_PROGBITS,
                                    SHF_ALLOC | SHF_EXECINSTR,
                                    tbytes, (Elf64_Xword)tlen, 16);
    for (int k = 0; k < g_ntg; k++)
        g_tg[k].ndx = elfw_add_section(
            w, g_tg[k].name, SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR,
            (unsigned char *)text.p + g_tg[k].start,
            (Elf64_Xword)(g_tg[k].end - g_tg[k].start),
            (Elf64_Xword)g_tg[k].align);
    int rodata_ndx = 0;
    if (rodata)
        /* 16, not 1: .rodata holds string literals (which need 1) and
         * also long double and 128-bit constants, which do not. */
        rodata_ndx = elfw_add_section(w, ".rodata", SHT_PROGBITS,
                                      SHF_ALLOC, rodata,
                                      (Elf64_Xword)rodata_len,
                                      (Elf64_Xword)rodata_align);
    int data_ndx = 0, bss_ndx = 0;
    /* MIPS: .data and .bss at 16 at least, as clang's and GNU as's MIPS
     * objects have them -- so an object -S reassembled lays out as -c's */
    if (ta == TARGET_MIPS32 || ta == TARGET_MIPS64) {
        if (data_align < 16) data_align = 16;
        if (bss_align < 16) bss_align = 16;
    }
    if (data_len)
        data_ndx = elfw_add_section(w, ".data", SHT_PROGBITS,
                                    SHF_ALLOC | SHF_WRITE, data,
                                    (Elf64_Xword)data_len,
                                    (Elf64_Xword)data_align);
    if (bss_len)
        bss_ndx = elfw_add_section(w, ".bss", SHT_NOBITS,
                                   SHF_ALLOC | SHF_WRITE, NULL,
                                   (Elf64_Xword)bss_len,
                                   (Elf64_Xword)bss_align);
    /* The thread-block template. SHF_TLS is the whole difference: with
     * it the linker puts these in PT_TLS and every thread gets its own
     * copy; without it they would be one shared object, which is the
     * opposite of what __thread asked for. .tbss is NOBITS for the
     * usual reason and takes no file space. */
    int tdata_ndx = 0, tbss_ndx = 0;
    if (tdata_len)
        tdata_ndx = elfw_add_section(w, ".tdata", SHT_PROGBITS,
                                     SHF_ALLOC | SHF_WRITE | SHF_TLS,
                                     tdata, (Elf64_Xword)tdata_len,
                                     (Elf64_Xword)tdata_align);
    if (tbss_len)
        tbss_ndx = elfw_add_section(w, ".tbss", SHT_NOBITS,
                                    SHF_ALLOC | SHF_WRITE | SHF_TLS,
                                    NULL, (Elf64_Xword)tbss_len,
                                    (Elf64_Xword)tbss_align);
    for (int k = 0; k < nnamed; k++)
        named[k].ndx = elfw_add_section(
            w, named[k].name, named[k].nobits ? SHT_NOBITS : SHT_PROGBITS,
            (Elf64_Xword)named[k].flags, named[k].buf,
            (Elf64_Xword)named[k].len, (Elf64_Xword)named[k].align);

    /* __attribute__((constructor)) / ((destructor)): the function's
     * ADDRESS goes in .init_array / .fini_array, one eight-byte slot
     * each, and the relocations further down fill the slots in. The
     * bytes are zero here because an object file records the question,
     * not the answer -- the address is not known until the link.
     *
     * The section TYPE carries the meaning. A linker gathers these by
     * SHT_INIT_ARRAY, so the same bytes under SHT_PROGBITS would be
     * laid out as ordinary data and the constructors would never run,
     * which is exactly the failure this whole path exists to fix. */
    /* A priority -- constructor(N) -- puts the address in a section of
     * its own, `.init_array.NNNNN` (`.fini_array.NNNNN`), as GCC names it:
     * the link orders by that name (embld's default layout; a script's
     * SORT_BY_INIT_PRIORITY), the numbered ones ascending ahead of the
     * plain array. So one section per kind and priority, in the order
     * the functions first ask for it. */
    int nctor = 0, ndtor = 0, narr = 0;
    for (struct func *f = u->funcs; f; f = f->next)
        if (!f->absorbed && f->has_defn) {
            if (f->is_ctor) nctor++;
            if (f->is_dtor) ndtor++;
        }
    struct ctorarr *arr = xcalloc((size_t)(nctor + ndtor + 1), sizeof *arr);
    for (struct func *f = u->funcs; f; f = f->next)
        for (int kind = 0; kind < 2 && !f->absorbed && f->has_defn; kind++) {
            if (!(kind ? f->is_dtor : f->is_ctor))
                continue;
            int prio = kind ? f->dtor_prio : f->ctor_prio, k;
            for (k = 0; k < narr; k++)
                if (arr[k].kind == kind && arr[k].prio == prio)
                    break;
            if (k == narr) {
                arr[narr].kind = kind;
                arr[narr].prio = prio;
                narr++;
            }
            arr[k].n++;
        }
    /* One pointer per slot -- four bytes on ARMv7-M and RV32, two on AVR.
     * It was eight everywhere, so on a 32-bit part the startup's walk of
     * .init_array read the first constructor and then a zero half. */
    int ps = target_ptr_size();
    for (int k = 0; k < narr; k++) {
        char nm[32];
        if (arr[k].prio)
            snprintf(nm, sizeof nm, "%s.%05d",
                     arr[k].kind ? ".fini_array" : ".init_array",
                     arr[k].prio - 1);
        else
            snprintf(nm, sizeof nm, "%s",
                     arr[k].kind ? ".fini_array" : ".init_array");
        arr[k].ndx = elfw_add_section(w, xstrndup(nm, strlen(nm)),
                                      arr[k].kind ? SHT_FINI_ARRAY
                                                  : SHT_INIT_ARRAY,
                                      SHF_ALLOC | SHF_WRITE,
                                      xcalloc((size_t)arr[k].n, (size_t)ps),
                                      (Elf64_Xword)(arr[k].n * ps),
                                      (Elf64_Xword)ps);
    }
    /* -g: the three DWARF sections (non-alloc, so no load cost; stripped
     * from a shipped image without touching the code). Their indices feed
     * the relocation-target lookup below. */
    static const char *const dwsec_name[DWARF_NSEC] =
        { ".debug_abbrev", ".debug_info", ".debug_line", ".debug_ranges" };
    int dwsec_ndx[DWARF_NSEC] = { 0, 0, 0, 0 };
    if (want_debug)
        for (int s = 0; s < DWARF_NSEC; s++)
            if (dw.seclen[s])     /* .debug_ranges only when split */
                dwsec_ndx[s] = elfw_add_section(
                    w, dwsec_name[s], SHT_PROGBITS, 0, dw.sec[s],
                    (Elf64_Xword)dw.seclen[s], 1);
    /* the unwind tables (x86-64 gives .eh_frame its own section type) and
     * the exception tables */
    int eh_ndx = 0, lsda_ndx = 0;
    if (unwind)
        eh_ndx = elfw_add_section(w, ".eh_frame",
                                  ta == TARGET_AARCH64 ? SHT_PROGBITS
                                                       : SHT_X86_64_UNWIND,
                                  SHF_ALLOC, eh.frame.p,
                                  (Elf64_Xword)eh.frame.len, 8);
    if (eh.lsda.len)
        lsda_ndx = elfw_add_section(w, ".gcc_except_table", SHT_PROGBITS,
                                    SHF_ALLOC, eh.lsda.p,
                                    (Elf64_Xword)eh.lsda.len, 4);
    elfw_add_symbol(w, path_basename(in), 0, 0,
                    ELF64_ST_INFO(STB_LOCAL, STT_FILE), SHN_ABS);
    int lsda_sym = 0;
    if (lsda_ndx)
        lsda_sym = elfw_add_symbol(w, "", 0, 0,
                                   ELF64_ST_INFO(STB_LOCAL, STT_SECTION),
                                   (Elf64_Half)lsda_ndx);
    int text_sym = elfw_add_symbol(w, "", 0, 0,
                    ELF64_ST_INFO(STB_LOCAL, STT_SECTION),
                    (Elf64_Half)text_ndx);
    for (int k = 0; k < g_ntg; k++)
        g_tg[k].sym = elfw_add_symbol(w, "", 0, 0,
                                      ELF64_ST_INFO(STB_LOCAL, STT_SECTION),
                                      (Elf64_Half)g_tg[k].ndx);
    /* ARM's MAPPING SYMBOLS. `$t` at an offset says "Thumb instructions
     * start here", `$a` says ARM and `$d` says data; a consumer that
     * finds none assumes ARM state and disassembles Thumb as garbage,
     * and the linker uses them to decide where an interworking veneer
     * may go. One at offset zero is the whole story for a Cortex-M
     * object, which is Thumb from end to end. */
    if (ta == TARGET_THUMB || ta == TARGET_AARCH64) {
        /* ...and `$x` is AArch64's "A64 instructions start here". A
         * jump table in .text is data between two of these: `$d` where
         * it starts and the code symbol again where it ends, so that a
         * disassembler prints words, not instructions. */
        const char *codesym = ta != TARGET_THUMB ? "$x"
                            : thumb_state() ? "$t" : "$a";
        elfw_add_symbol(w, codesym, 0, 0,
                        ELF64_ST_INFO(STB_LOCAL, STT_NOTYPE),
                        (Elf64_Half)text_ndx);
        for (int k = 0; k < g_ntg; k++)
            elfw_add_symbol(w, codesym, 0, 0,
                            ELF64_ST_INFO(STB_LOCAL, STT_NOTYPE),
                            (Elf64_Half)g_tg[k].ndx);
        for (int r = 0; r + 1 < text.ndrange; r += 2) {
            long o;
            int gi = text_at(text.drange[r], &o);
            int dn = gi ? g_tg[gi - 1].ndx : text_ndx;
            elfw_add_symbol(w, "$d", (Elf64_Addr)o, 0,
                            ELF64_ST_INFO(STB_LOCAL, STT_NOTYPE),
                            (Elf64_Half)dn);
            /* the code resumes where the data ends, in the same section,
             * unless that is the section's end -- or more data starts
             * right there: an ARMv6-M literal pool can follow a switch
             * table with no instruction between, and a `$t` and a `$d` at
             * one address leave a disassembler to pick one, which it did,
             * decoding the pool as instructions. */
            long e = o + (text.drange[r + 1] - text.drange[r]);
            long send = gi ? g_tg[gi - 1].end - g_tg[gi - 1].start : tlen;
            if (e < send &&
                !(r + 3 < text.ndrange &&
                  text.drange[r + 2] == text.drange[r + 1]))
                elfw_add_symbol(w, codesym, (Elf64_Addr)e, 0,
                                ELF64_ST_INFO(STB_LOCAL, STT_NOTYPE),
                                (Elf64_Half)dn);
        }
    }
    if (ta == TARGET_THUMB) {
        /* ARM BUILD ATTRIBUTES. What the object was built for, and the
         * only place downstream that can refuse a combination which
         * cannot work: ld compares Tag_ABI_VFP_args to stop a
         * soft-float object linking against a hard-float one. With no
         * section at all there was nothing to compare, so that link
         * succeeded and the callee read its arguments from registers the
         * caller never wrote. See src/arch/thumb/attrs.h. */
        size_t alen = 0;
        unsigned char *ab = arm_build_attributes(&alen);
        elfw_add_section(w, ".ARM.attributes", SHT_ARM_ATTRIBUTES, 0,
                         ab, (Elf64_Xword)alen, 1);
        free(ab);
    }
    if (ta == TARGET_RISCV32 || ta == TARGET_RISCV64) {
        size_t alen = 0;
        unsigned char *ab = riscv_build_attributes(&alen);
        elfw_add_section(w, ".riscv.attributes", SHT_RISCV_ATTRIBUTES, 0,
                         ab, (Elf64_Xword)alen, 1);
        free(ab);
    }
    if (ta == TARGET_MIPS32 || ta == TARGET_MIPS64) {
        /* The ABI flags clang's objects carry: what ISA and register
         * sizes the code needs and which floating-point ABI it was
         * compiled for (soft), so a linker can refuse to mix it with a
         * hard-float object. src/arch/mips/codegen.c. */
        unsigned char af[24];
        mips_build_abiflags(af);
        elfw_add_section(w, ".MIPS.abiflags", SHT_MIPS_ABIFLAGS, SHF_ALLOC,
                         af, (Elf64_Xword)sizeof af, 8);
    }
    int rodata_sym = 0;
    if (rodata)
        rodata_sym = elfw_add_symbol(w, "", 0, 0,
                                     ELF64_ST_INFO(STB_LOCAL,
                                                   STT_SECTION),
                                     (Elf64_Half)rodata_ndx);
    /* -g: STT_SECTION symbols for the debug sections, so the line/info
     * fields can relocate against them (DWTGT_ABBREV/DWTGT_LINE). Added here
     * in the local block — the writer refuses a local after any global. */
    int dwsym[DWARF_NSEC] = { 0, 0, 0, 0 };
    if (want_debug)
        for (int s = 0; s < DWARF_NSEC; s++)
            if (dwsec_ndx[s])
                dwsym[s] = elfw_add_symbol(w, "", 0, 0,
                              ELF64_ST_INFO(STB_LOCAL, STT_SECTION),
                              (Elf64_Half)dwsec_ndx[s]);
    /* Locals before globals — the writer enforces the gABI ordering.
     * Only canonical, defined functions own code. */
    for (struct func *f = u->funcs; f; f = f->next)
        if (!f->absorbed && f->has_defn && f->is_static && f->used)
            f->sym_ndx = elfw_add_symbol(
                w, f->name, (Elf64_Addr)code_sym_value(ta, f->code_off + f->code_entry),
                (Elf64_Xword)(f->code_len - f->code_entry),
                ELF64_ST_INFO(STB_LOCAL, STT_FUNC),
                (Elf64_Half)code_sec(f->code_off, text_ndx));
    /* An alias is one more symbol at its target's address and size (sema
     * checked the target is a function defined here); a call or an
     * address taken goes through the alias's own symbol, so a strong
     * definition elsewhere still wins over a weak alias. */
    for (struct func *f = u->funcs; f; f = f->next)
        if (!f->absorbed && f->alias_of && f->is_static) {
            const struct func *t = alias_target(u, f);
            f->sym_ndx = elfw_add_symbol(
                w, f->name, (Elf64_Addr)code_sym_value(ta, t->code_off + t->code_entry),
                (Elf64_Xword)(t->code_len - t->code_entry), ELF64_ST_INFO(STB_LOCAL, STT_FUNC),
                (Elf64_Half)code_sec(t->code_off, text_ndx));
        }
    for (struct global *g = u->globals; g; g = g->next)
        if (!g->absorbed && g->defined && g->is_static)
            g->sym_ndx = elfw_add_symbol(
                w, g->name, (Elf64_Addr)g->off,
                (Elf64_Xword)global_size(g),
                ELF64_ST_INFO(STB_LOCAL, g->is_tls ? STT_TLS : STT_OBJECT),
                (Elf64_Half)(g->is_tls ? (g->in_bss ? tbss_ndx : tdata_ndx)
                             : g->in_rodata ? rodata_ndx
                             : g->named ? named[g->named - 1].ndx
                             : g->in_bss ? bss_ndx : data_ndx));
    /* A block's label that is not .global, with the name of a function
     * or an object this unit declares and does not define -- a static
     * naked function's, chiefly: the C code's references go to it, as
     * they do when gcc's assembler reads both from one file. */
    for (struct topasm *ta = u->topasm; ta; ta = ta->next)
        for (int k = 0; k < ta->nsyms; k++) {
            const struct asmsym *as = &ta->syms[k];
            struct func *lf = NULL;
            struct global *lg = NULL;
            if (as->is_global)
                continue;
            for (struct func *f = u->funcs; f && !lf; f = f->next)
                if (!f->absorbed && !f->has_defn && !f->sym_ndx &&
                    !f->alias_of && strcmp(f->name, as->name) == 0)
                    lf = f;
            for (struct global *g = u->globals; g && !lf && !lg; g = g->next)
                if (!g->absorbed && !g->defined && !g->sym_ndx &&
                    strcmp(g->name, as->name) == 0)
                    lg = g;
            if (!lf && !lg)
                continue;
            long loff = ta->text_off + as->off, lval;
            text_at(loff, &lval);
            if (lf)
                lval = fn_sym_value(target_get(), lval);
            int lndx = elfw_add_symbol(
                w, as->name, (Elf64_Addr)lval, (Elf64_Xword)as->size,
                ELF64_ST_INFO(STB_LOCAL, lf ? STT_FUNC : STT_OBJECT),
                (Elf64_Half)code_sec(loff, text_ndx));
            if (lf)
                lf->sym_ndx = lndx;
            else
                lg->sym_ndx = lndx;
        }
    for (struct func *f = u->funcs; f; f = f->next)
        if (!f->absorbed && f->has_defn && !f->is_static)
            f->sym_ndx = elfw_add_symbol(
                w, f->name, (Elf64_Addr)code_sym_value(ta, f->code_off + f->code_entry),
                (Elf64_Xword)(f->code_len - f->code_entry),
                ELF64_ST_INFO(f->is_weak ? STB_WEAK : STB_GLOBAL, STT_FUNC),
                (Elf64_Half)code_sec(f->code_off, text_ndx));
    /* ACLE's CMSE: a cmse_nonsecure_entry function has a second global
     * symbol at the same address, __acle_se_<name>. The linker sees the
     * pair, makes the SG veneer in .gnu.sgstubs and points <name> at it
     * (src/link/link.c), which is how a Non-secure caller enters through
     * the gateway and a Secure caller may still reach the code. */
    for (struct func *f = u->funcs; f; f = f->next)
        if (!f->absorbed && f->has_defn && !f->is_static && f->cmse_entry &&
            target_thumb_cmse()) {
            char *se = xmalloc(strlen(f->name) + sizeof "__acle_se_");
            strcpy(se, "__acle_se_");
            strcat(se, f->name);
            elfw_add_symbol(
                w, se, (Elf64_Addr)code_sym_value(ta, f->code_off + f->code_entry),
                (Elf64_Xword)(f->code_len - f->code_entry),
                ELF64_ST_INFO(STB_GLOBAL, STT_FUNC),
                (Elf64_Half)code_sec(f->code_off, text_ndx));
        }
    for (struct func *f = u->funcs; f; f = f->next)
        if (!f->absorbed && f->alias_of && !f->is_static) {
            const struct func *t = alias_target(u, f);
            f->sym_ndx = elfw_add_symbol(
                w, f->name, (Elf64_Addr)code_sym_value(ta, t->code_off + t->code_entry),
                (Elf64_Xword)(t->code_len - t->code_entry),
                ELF64_ST_INFO(f->is_weak ? STB_WEAK : STB_GLOBAL, STT_FUNC),
                (Elf64_Half)code_sec(t->code_off, text_ndx));
        }
    for (struct global *g = u->globals; g; g = g->next)
        if (!g->absorbed && g->defined && !g->is_static)
            g->sym_ndx = elfw_add_symbol(
                w, g->name, (Elf64_Addr)g->off,
                (Elf64_Xword)global_size(g),
                ELF64_ST_INFO(g->is_weak ? STB_WEAK : STB_GLOBAL,
                              g->is_tls ? STT_TLS : STT_OBJECT),
                (Elf64_Half)(g->is_common ? SHN_COMMON
                             : g->is_tls ? (g->in_bss ? tbss_ndx : tdata_ndx)
                             : g->in_rodata ? rodata_ndx
                             : g->named ? named[g->named - 1].ndx
                             : g->in_bss ? bss_ndx : data_ndx));
    /* File-scope asm's .global labels (_start): global functions at their
     * .text offset. Local labels stay internal — the assembler already
     * resolved jumps to them into rel32s.
     *
     * The index is written back onto any DECLARATION of the same name,
     * and that is not bookkeeping: the passes below add one UNDEF
     * symbol per called-but-undefined function, and without this they
     * did it for these too. The object then held both a definition and
     * an undefined reference for one name, and a linker resolving the
     * undefined one reported `__uw_capture` missing from an object that
     * defines it. C code calling a routine written in file-scope asm is
     * exactly how a libc's setjmp and an unwinder's register capture are
     * built, so it is the normal case rather than an exotic one. */
    for (struct topasm *ta = u->topasm; ta; ta = ta->next)
        for (int k = 0; k < ta->nsyms; k++)
            if (ta->syms[k].is_global) {
                long toff;
                const struct asmsym *as = &ta->syms[k];
                text_at(ta->text_off + as->off, &toff);
                /* Its type is what .type said. Untyped, it stays a
                 * function, as every label was -- except on Thumb, where
                 * a function's address carries bit 0 and gas leaves an
                 * untyped label NOTYPE and even. A typed Thumb function
                 * was even, so a call through a pointer to it switched
                 * to the ARM state a Cortex-M does not have. */
                int thumb = thumb_state();
                int st = as->type == ASMSYM_OBJECT ? STT_OBJECT
                       : as->type == ASMSYM_FUNC || !thumb ? STT_FUNC
                       : STT_NOTYPE;
                if (thumb && st == STT_FUNC)
                    toff = fn_sym_value(TARGET_THUMB, toff);
                int ndx = elfw_add_symbol(
                    w, as->name, (Elf64_Addr)toff, (Elf64_Xword)as->size,
                    ELF64_ST_INFO(as->is_weak ? STB_WEAK : STB_GLOBAL, st),
                    (Elf64_Half)text_ndx);
                for (struct func *f = u->funcs; f; f = f->next)
                    if (!f->absorbed && !f->has_defn && !f->sym_ndx &&
                        strcmp(f->name, ta->syms[k].name) == 0)
                        f->sym_ndx = ndx;
                for (struct global *g = u->globals; g; g = g->next)
                    if (!g->absorbed && !g->defined && !g->sym_ndx &&
                        strcmp(g->name, ta->syms[k].name) == 0)
                        g->sym_ndx = ndx;
            }

    /* extern-declared, used, never defined: the linker's problem --
     * unless a file-scope asm block defines it (sym_ndx, set above),
     * which gave the object a definition and an undefined symbol of
     * the same name, and the C code's references went to the second. */
    for (struct global *g = u->globals; g; g = g->next)
        if (!g->absorbed && !g->defined && g->used && !g->sym_ndx)
            g->sym_ndx = elfw_add_symbol(
                w, g->name, 0, 0,
                ELF64_ST_INFO(g->is_weak ? STB_WEAK : STB_GLOBAL,
                              STT_NOTYPE), SHN_UNDEF);

    /* Every called external gets one UNDEF symbol, and every call site
     * a PLT32 relocation against it. addend -4: rel32 is relative to
     * the END of the call instruction, four bytes past r_offset. */
    for (int i = 0; i < next; i++) {
        struct func *callee = ext[i].callee;
        if (!callee->sym_ndx)
            callee->sym_ndx =
                elfw_add_symbol(w, callee->name, 0, 0,
                                ELF64_ST_INFO(callee->is_weak ? STB_WEAK
                                                              : STB_GLOBAL,
                                              STT_NOTYPE),
                                SHN_UNDEF);
        code_rela(w, text_ndx, ext[i].patch_off, callee->sym_ndx,
                  target_reloc_type(ta, ext[i].tail ? RK_TAIL : RK_CALL),
                  target_reloc_addend(ta, RK_CALL, 0));
    }
    free(ext);

    /* __attribute__((visibility("hidden"))) and its three siblings.
     * Applied after the symbols exist, because it modifies one rather
     * than creating it -- and only where a visibility was asked for, so
     * every other symbol keeps the writer's default of STV_DEFAULT. */
    for (struct func *f = u->funcs; f; f = f->next)
        if (!f->absorbed && f->sym_ndx && f->vis)
            elfw_symbol_visibility(w, f->sym_ndx, stv_of(f->vis));
    for (struct global *g = u->globals; g; g = g->next)
        if (!g->absorbed && g->sym_ndx && g->vis)
            elfw_symbol_visibility(w, g->sym_ndx, stv_of(g->vis));

    /* Fill the constructor/destructor slots: each is the address of a
     * function of this unit, so each is an absolute 64-bit relocation
     * against that function's symbol. Source order, which is the order
     * the section was sized in -- a priority would change it, and
     * parse.c refuses one rather than quietly running them wrong. */
    if (nctor || ndtor) {
        /* a function's address at pointer width: on AVR its WORD
         * address, as for a function pointer in data */
        enum reloc_kind ck = ps == 8 ? RK_ABS64 : ps == 4 ? RK_ABS32
                                                         : RK_AVR_ABS16_PM;
        for (struct func *f = u->funcs; f; f = f->next)
            for (int kind = 0; kind < 2 && !f->absorbed && f->has_defn;
                 kind++) {
                if (!(kind ? f->is_dtor : f->is_ctor))
                    continue;
                int prio = kind ? f->dtor_prio : f->ctor_prio, k;
                for (k = 0; k < narr; k++)
                    if (arr[k].kind == kind && arr[k].prio == prio)
                        break;
                elfw_add_rela(w, arr[k].ndx, (Elf64_Addr)(arr[k].at++ * ps),
                              f->sym_ndx, target_reloc_type(target_get(), ck),
                              0);
            }
    }

    /* File-scope asm relocations against the target: a function of this
     * unit (already symboled and forced used above) or, failing that, a
     * fresh UNDEF the linker resolves.
     *
     * A `call` carries the displacement of the instruction; a `.quad
     * symbol` carries the address itself, and is the form `_start` uses
     * on aarch64, where the block is data because the built-in
     * assembler encodes no aarch64 instructions. So the relocation is
     * chosen from the kind and the architecture rather than assumed to
     * be a call. */
    const char **undef_name = NULL;
    int *undef_sym = NULL, nundef = 0;
    for (struct topasm *ta = u->topasm; ta; ta = ta->next)
        for (int r = 0; r < ta->nrels; r++) {
            /* The symbol this file already has for the name -- a
             * function, an object, or a block's own global label -- so
             * a `.quad x` against an x defined here did not add a
             * second, undefined x beside it. */
            int sym = 0;
            long addend = ta->rels[r].addend;
            const char *tn = ta->rels[r].target;
            int etype = ta->rels[r].elf_type;
            /* gas_assemble_block's field against an assembler-local
             * label: the block's start plus the label's offset, against
             * the section */
            if (!tn) {
                addend = code_ref(ta->text_off + addend, text_sym, &sym);
                code_rela(w, text_ndx, ta->text_off + ta->rels[r].off,
                          sym, etype, addend);
                continue;
            }
            for (struct func *f = u->funcs; f && !sym; f = f->next)
                if (!f->absorbed && f->sym_ndx && strcmp(f->name, tn) == 0)
                    sym = f->sym_ndx;
            for (struct global *g = u->globals; g && !sym; g = g->next)
                if (!g->absorbed && g->sym_ndx && strcmp(g->name, tn) == 0)
                    sym = g->sym_ndx;
            /* A label of a block, global or not, that C never declared:
             * its section and offset. (A local one went out as an
             * undefined symbol of the same name, which another object's
             * global could satisfy.) */
            for (struct topasm *lb = u->topasm; lb && !sym; lb = lb->next)
                for (int k = 0; k < lb->nsyms && !sym; k++)
                    if (strcmp(lb->syms[k].name, tn) == 0) {
                        addend += code_ref(lb->text_off + lb->syms[k].off,
                                           text_sym, &sym);
                        /* a Thumb function's ADDRESS carries bit 0; a
                         * branch to it does not */
                        if (thumb_state() &&
                            lb->syms[k].type == ASMSYM_FUNC &&
                            (!etype || etype == R_ARM_ABS32))
                            addend |= 1;
                    }
            /* one undefined symbol per name, however many fields name it */
            for (int k = 0; k < nundef && !sym; k++)
                if (strcmp(undef_name[k], tn) == 0)
                    sym = undef_sym[k];
            if (!sym) {
                sym = elfw_add_symbol(
                    w, tn, 0, 0,
                    ELF64_ST_INFO(STB_GLOBAL, STT_NOTYPE), SHN_UNDEF);
                undef_name = xrealloc(undef_name, (size_t)(nundef + 1) *
                                      sizeof *undef_name);
                undef_sym = xrealloc(undef_sym, (size_t)(nundef + 1) *
                                     sizeof *undef_sym);
                undef_name[nundef] = tn;
                undef_sym[nundef++] = sym;
            }
            enum reloc_kind rk = ta->rels[r].kind == ASMREL_ABS64 ? RK_ABS64
                               : ta->rels[r].kind == ASMREL_ABS32 ? RK_ABS32
                               : RK_CALL;
            code_rela(w, text_ndx, ta->text_off + ta->rels[r].off, sym,
                      etype ? etype : target_reloc_type(target_get(), rk),
                      addend);
        }
    free(undef_name);
    free(undef_sym);

    /* RISC-V's low half names the AUIPC, not the target.
     *
     * R_RISCV_PCREL_LO12_I is resolved by looking up the HIGH half's
     * relocation at the address its symbol gives, and taking the low
     * twelve bits of what THAT computed -- because the two halves have
     * to agree about the +0x800 rounding, and only the high one knows
     * the whole displacement. An assembler spells that with a local
     * `.Lpcrel_hi0` label on the auipc; here the .text section symbol
     * plus the auipc's offset resolves to the same address, and the
     * auipc is always the instruction four bytes before. */
    int riscv = ta == TARGET_RISCV32 || ta == TARGET_RISCV64;

    /* String addresses: PC32 against the .rodata section symbol.
     * addend = target offset - 4, because rel32 is measured from the
     * end of the instruction, four bytes past r_offset. */
    for (int i = 0; i < nstrs; i++) {
        int lo = riscv && strs[i].kind == RK_RISCV_PCREL_LO12_I;
        /* AVR: a jump to a label in this object's own .text, too far for the
         * 12-bit rjmp. Against the SECTION symbol with the label's offset as
         * the addend, because `jmp` carries an absolute address and nothing
         * in a relocatable object knows where its own .text will land. */
        int tx = strs[i].kind == RK_AVR_TEXT_CALL ||
                 strs[i].kind == RK_MIPS_TEXT26 ||
                 strs[i].kind == RK_XTENSA_TEXT32;
        /* (the auipc, or the jump's label, is in the same function as
         * the site, so in the same section) */
        int ssym = rodata_sym;
        long sadd = target_reloc_addend(ta, strs[i].kind, strs[i].str_off);
        if (lo)
            sadd = code_ref(strs[i].patch_off - 4, text_sym, &ssym);
        else if (tx)
            sadd = target_reloc_addend(ta, strs[i].kind,
                                       code_ref(strs[i].str_off, text_sym,
                                                &ssym));
        code_rela(w, text_ndx, strs[i].patch_off, ssym,
                  target_reloc_type(ta, strs[i].kind), sadd);
    }
    free(strs);

    /* Global-variable addresses: PC32 against the global's own symbol
     * (defined or UNDEF alike — the linker fills in either way). */
    for (int i = 0; i < ngs; i++) {
        int lo = riscv && gs[i].kind == RK_RISCV_PCREL_LO12_I;
        int gsym = gs[i].glob->sym_ndx;
        long gadd = target_reloc_addend(ta, gs[i].kind, 0);
        if (lo)
            gadd = code_ref(gs[i].patch_off - 4, text_sym, &gsym);
        code_rela(w, text_ndx, gs[i].patch_off, gsym,
                  target_reloc_type(ta, gs[i].kind), gadd);
    }
    free(gs);

    /* Pointer slots in .data initialized by an address: an absolute 64-bit
     * relocation, against the .rodata section symbol for a string literal
     * or against the target global's own symbol for an &global. A global
     * carrying relocations is initialized, hence in .data, never .bss. */
    for (struct global *g = u->globals; g; g = g->next) {
        if (g->absorbed || !g->defined || g->in_bss)
            continue;
        for (int i = 0; i < g->nrelocs; i++) {
            struct func *ft = g->relocs[i].ftarget;
            int sym;
            long add;
            if (ft) {
                /* a function pointer (a vtable): resolve to the function's
                 * symbol; an external, never-called one needs an UNDEF. */
                if (!ft->sym_ndx)
                    ft->sym_ndx = elfw_add_symbol(
                        w, ft->name, 0, 0,
                        ELF64_ST_INFO(ft->is_weak ? STB_WEAK : STB_GLOBAL,
                                      STT_NOTYPE), SHN_UNDEF);
                sym = ft->sym_ndx;
                add = g->relocs[i].addend;
            } else if (g->relocs[i].gtarget) {
                sym = g->relocs[i].gtarget->sym_ndx;
                add = g->relocs[i].addend;
            } else {
                sym = rodata_sym;
                add = g->relocs[i].str_off + g->relocs[i].addend;
            }
            /* A pointer in .data is as wide as a pointer: eight bytes
             * on LP64 and four on ARMv7-M, where asking for ABS64 would
             * get -1 back from a table that has no such relocation.
             *
             * AVR is two bytes AND two address spaces. A pointer to data
             * is R_AVR_16; a pointer to a FUNCTION is R_AVR_16_PM, whose
             * value is the WORD address, because that is what `icall`
             * reads out of Z. Using the data form for a vtable entry
             * would produce a pointer to twice as far into flash -- and
             * land on a real instruction, so nothing would fault. */
            enum reloc_kind dk;
            if (target_ptr_size() == 2)
                dk = ft ? RK_AVR_ABS16_PM : RK_AVR_ABS16;
            else
                dk = target_ptr_size() == 8 ? RK_ABS64 : RK_ABS32;
            elfw_add_rela(w, g->in_rodata ? rodata_ndx
                             : g->named ? named[g->named - 1].ndx : data_ndx,
                          (Elf64_Addr)(g->off + g->relocs[i].off), sym,
                          target_reloc_type(ta, dk), add);
        }
    }

    /* Function addresses: PC32 against the function's symbol; an
     * address-taken external gets an UNDEF symbol like a called one. */
    for (int i = 0; i < nfs; i++) {
        struct func *tf = fs[i].target;
        if (!tf->sym_ndx)
            tf->sym_ndx = elfw_add_symbol(
                w, tf->name, 0, 0,
                ELF64_ST_INFO(tf->is_weak ? STB_WEAK : STB_GLOBAL,
                              STT_NOTYPE), SHN_UNDEF);
        int lo = riscv && fs[i].kind == RK_RISCV_PCREL_LO12_I;
        int fsym = tf->sym_ndx;
        long fadd = target_reloc_addend(ta, fs[i].kind, 0) + fs[i].addend;
        if (lo)
            fadd = code_ref(fs[i].patch_off - 4, text_sym, &fsym);
        code_rela(w, text_ndx, fs[i].patch_off, fsym,
                  target_reloc_type(ta, fs[i].kind), fadd);
    }
    free(fs);

    /* -g: the DWARF relocations. Each field the emitter left zero gets an
     * absolute reloc — 8-byte .text addresses (R_X86_64_64) and 4-byte
     * section offsets (R_X86_64_32) — against the right section symbol. */
    if (want_debug) {
        for (int i = 0; i < dw.nrelocs; i++) {
            struct dwarf_reloc *r = &dw.relocs[i];
            int sym = r->target == DWTGT_ABBREV ? dwsym[DWSEC_ABBREV]
                    : r->target == DWTGT_RANGES ? dwsym[DWSEC_RANGES]
                    :                             dwsym[DWSEC_LINE];
            long add = r->addend;
            /* A code address is in whichever section its function is
             * (code_ref); an END is the byte after the function, which
             * the code buffer may give to the next section. */
            if (r->target == DWTGT_TEXT)
                add = r->end ? code_ref(r->addend - 1, text_sym, &sym) + 1
                             : code_ref(r->addend, text_sym, &sym);
            /* a global variable's DW_OP_addr: its own symbol, which
             * every defined global has by now */
            if (r->target == DWTGT_GLOBAL) {
                if (!r->glob->sym_ndx)
                    internal_error("-g: the global '%s' has no symbol to "
                                   "locate it by", r->glob->name);
                sym = r->glob->sym_ndx;
            }
            elfw_add_rela(w, dwsec_ndx[r->in_sec], (Elf64_Addr)r->off, sym,
                          target_reloc_type(ta, r->width == 8 ? RK_ABS64
                                                              : RK_ABS32),
                          add);
        }
        dwarf_free(&dw);
    }

    /* the unwind and exception tables' pointers, all PC-relative: into
     * .text and .gcc_except_table, to the personality routine, to the
     * catch types' typeinfo objects */
    int personality_sym = 0;
    for (int i = 0; i < eh.nrelocs; i++) {
        struct eh_reloc *r = &eh.relocs[i];
        int sym;
        switch (r->target) {
        case EHT_TEXT:          /* a function's start: its section's */
            r->addend = code_ref(r->addend, text_sym, &sym);
            break;
        case EHT_LSDA: sym = lsda_sym; break;
        case EHT_PERSONALITY:
            if (!personality_sym)
                personality_sym = elfw_add_symbol(
                    w, "__gxx_personality_v0", 0, 0,
                    ELF64_ST_INFO(STB_GLOBAL, STT_NOTYPE), SHN_UNDEF);
            sym = personality_sym;
            break;
        default:
            sym = r->glob->sym_ndx;
            break;
        }
        elfw_add_rela(w, r->in_lsda ? lsda_ndx : eh_ndx, (Elf64_Addr)r->off,
                      sym, target_reloc_type(ta, RK_DATA_PREL32),
                      target_reloc_addend(ta, RK_DATA_PREL32, r->addend));
    }
    eh_free(&eh);
    free(arr);

    int rc = elfw_write(w, out);
    elfw_free(w);
    return rc == 0 ? 0 : 1;
}

static int has_c_suffix(const char *s)
{
    size_t n = strlen(s);
    return n > 2 && strcmp(s + n - 2, ".c") == 0;
}

/* g++'s C++ suffixes. */
/* The status a compile ends with: its own, unless a diagnostic the engine
 * counted as an error (a -Werror warning) says otherwise. */
static int done(int rc)
{
    /* Rendered once, at the end, like diagnostics -- a remark printed as it
     * happened could not be queried, sorted or counted, and §19's premise is
     * that "why" is a query over records. */
    if (want_remarks) {
        struct outbuf b = { NULL, 0, 0 };
        if (remarks_json)
            remarks_render_json(&b);
        else
            remarks_render_text(&b);
        fwrite(b.p, 1, b.n, stderr);
        ob_free(&b);
    }
    if (want_fix) {
        /* What the diagnostics proposed, actually done. The compile has
         * failed by now: that is the normal case for --fix. */
        diag_flush();
        int n = diag_apply_fixits();
        fprintf(stderr, n ? "embcc: %d fix%s applied; compile again\n"
                          : "embcc: nothing to fix automatically\n",
                n, n == 1 ? "" : "es");
        return n ? 0 : (rc ? rc : (diag_error_count() ? 1 : 0));
    }
    return rc ? rc : (diag_error_count() ? 1 : 0);
}

static int has_cxx_suffix(const char *s)
{
    static const char *const sfx[] = { ".cc", ".cpp", ".cxx", ".C", ".c++",
                                       ".cp", ".CPP", ".ii" };
    size_t n = strlen(s);
    for (size_t i = 0; i < sizeof sfx / sizeof sfx[0]; i++) {
        size_t k = strlen(sfx[i]);
        if (n > k && strcmp(s + n - k, sfx[i]) == 0)
            return 1;
    }
    return 0;
}

static int has_asm_suffix(const char *s)
{
    size_t n = strlen(s);
    return n > 4 && strcmp(s + n - 4, ".asm") == 0;
}

/* `.s` and `.S`: GNU-syntax assembly, which src/as/gas.c assembles for
 * whichever target is selected. The case is the whole difference --
 * `.S` is preprocessed first, as it is in every other compiler, which
 * is what lets a startup file share a header with the C beside it.
 * Returns 0, 1 for `.s`, or 2 for `.S`. */
/* -x assembler (1) and -x assembler-with-cpp (2): the input is GNU
 * assembly whatever its name -- a CubeMX Makefile assembles its
 * startup_*.s with `gcc -x assembler-with-cpp`, so the .s is preprocessed.
 * Like the rest of -x here it is one setting for the command. An object
 * or an archive given with it is still linked: has_link_input_suffix
 * claims those before any source suffix is asked about. */
static int g_x_gas;

static int has_gas_suffix(const char *s)
{
    size_t n = strlen(s);
    if (g_x_gas && s[0] != '-')
        return g_x_gas;
    if (n <= 2 || s[n - 2] != '.') return 0;
    return s[n - 1] == 's' ? 1 : s[n - 1] == 'S' ? 2 : 0;
}


static int g_want_dump_predef, g_want_dumpmachine;

/* RISC-V's -march= and -mabi=, as recorded while parsing. */
static const char *g_rv_march, *g_rv_mabi;

/* What -march= and -mabi= mean together, decided once every argument has
 * been seen, as GCC and clang spell them:
 *
 *   -march=rv32i<exts>[_zicsr][_zifencei] or rv64..., the base `i` or `g`
 *       (imafd_zicsr_zifencei), then single-letter extensions in any
 *       order. EmbCC's code needs M and A (the multiply and the atomics it
 *       emits); F and D are the FPU the backend may use; C is the
 *       compressed encodings. Anything else is refused by name.
 *   -mabi=ilp32|ilp32f|ilp32d for RV32, lp64|lp64f|lp64d for RV64: where
 *       floating point travels across a call (the psABI's integer,
 *       single and double hardware-float conventions). Without -mabi= it
 *       follows -march= as clang's does: D gives the double ABI, F alone
 *       the single one, neither the integer one.
 *
 * The default is today's: rv32imac/ilp32 and rv64imac/lp64. */
static void riscv_float_resolve(void)
{
    int rv64 = target_get() == TARGET_RISCV64;
    int f = 0, d = 0, c = 1, m = 1, a = 1, zifencei = 0, abi;
    if (!rv64 && target_get() != TARGET_RISCV32)
        return;
    if (g_rv_march) {
        const char *p = g_rv_march, *want = rv64 ? "rv64" : "rv32";
        if (strncmp(p, "rv32", 4) != 0 && strncmp(p, "rv64", 4) != 0)
            diag_fatal(NULL, 0, "-march=%s is not a RISC-V ISA string: it "
                       "starts with rv32 or rv64, then i or g", g_rv_march);
        if (strncmp(p, want, 4) != 0)
            diag_fatal(NULL, 0, "-march=%s is a %.4s ISA, and %s is %s: use "
                       "--target=riscv%.2s-unknown-elf", g_rv_march, p,
                       target_triple_now(), want, p + 2);
        p += 4;
        c = m = a = 0;
        if (*p == 'g') {
            f = d = m = a = zifencei = 1;
        } else if (*p == 'e') {
            diag_fatal(NULL, 0, "-march=%s: the E base (16 registers, "
                       "ilp32e) is not supported; EmbCC emits the I base",
                       g_rv_march);
        } else if (*p != 'i') {
            diag_fatal(NULL, 0, "-march=%s: the base ISA after %s is i or g",
                       g_rv_march, want);
        }
        for (p++; *p && *p != '_'; p++) {
            switch (*p) {
            case 'm': m = 1; break;
            case 'a': a = 1; break;
            case 'f': f = 1; break;
            case 'd': d = 1; break;
            case 'c': c = 1; break;
            default:
                diag_fatal(NULL, 0, "-march=%s: the '%c' extension is not "
                           "supported: EmbCC emits I, M, A, F, D and C "
                           "(and no version numbers)", g_rv_march, *p);
            }
        }
        while (*p == '_') {
            const char *e = ++p;
            size_t n;
            while (*p && *p != '_')
                p++;
            n = (size_t)(p - e);
            if (n == 5 && strncmp(e, "zicsr", 5) == 0)
                continue;          /* the CSR instructions: implied by F */
            if (n == 8 && strncmp(e, "zifencei", 8) == 0) {
                zifencei = 1;
                continue;
            }
            diag_fatal(NULL, 0, "-march=%s: the '%.*s' extension is not "
                       "supported: EmbCC accepts zicsr and zifencei after "
                       "the single-letter ones", g_rv_march, (int)n, e);
        }
        if (d && !f)
            diag_fatal(NULL, 0, "-march=%s: the D extension needs F (double "
                       "precision is built on the single-precision "
                       "registers): add f", g_rv_march);
        if (!m)
            diag_fatal(NULL, 0, "-march=%s: EmbCC needs the M extension -- "
                       "its code multiplies and divides with mul and div, "
                       "and has no __mulsi3 path: add m", g_rv_march);
        if (!a)
            diag_fatal(NULL, 0, "-march=%s: EmbCC needs the A extension -- "
                       "its atomics are lr/sc and the amo instructions, and "
                       "it has no __atomic_* library path: add a",
                       g_rv_march);
    }
    abi = d ? 64 : f ? 32 : 0;
    if (g_rv_mabi) {
        const char *v = g_rv_mabi, *base = rv64 ? "lp64" : "ilp32";
        size_t bn = strlen(base);
        if ((rv64 && !strncmp(v, "ilp32", 5)) ||
            (!rv64 && !strncmp(v, "lp64", 4)))
            diag_fatal(NULL, 0, "-mabi=%s is a %s ABI, and %s is %s", v,
                       rv64 ? "32-bit" : "64-bit", target_triple_now(),
                       rv64 ? "RV64 (lp64, lp64f, lp64d)"
                            : "RV32 (ilp32, ilp32f, ilp32d)");
        if (strncmp(v, base, bn) != 0 ||
            (v[bn] && (v[bn + 1] || (v[bn] != 'f' && v[bn] != 'd'))))
            diag_fatal(NULL, 0, "-mabi=%s is not supported for %s: EmbCC "
                       "emits %s, %sf and %sd%s", v, target_triple_now(),
                       base, base, base,
                       v[bn] == 'e' ? " (not the E base's)" : "");
        abi = v[bn] == 'd' ? 64 : v[bn] == 'f' ? 32 : 0;
        if (abi > (d ? 64 : f ? 32 : 0))
            diag_fatal(NULL, 0, "-mabi=%s passes %s in floating-point "
                       "registers, and -march=%s has no %s extension: use "
                       "-march=%s%s", v, abi == 64 ? "doubles" : "floats",
                       g_rv_march ? g_rv_march : rv64 ? "rv64imac"
                                                      : "rv32imac",
                       abi == 64 ? "D" : "F", rv64 ? "rv64" : "rv32",
                       abi == 64 ? "imafdc" : "imafc");
    }
    target_set_riscv_isa(f, d, c, zifencei);
    target_set_riscv_abi_flen(abi);
}

/* -mfpu=, -mfloat-abi= and -mcpu=, as recorded while parsing. */
static const char *g_arm_fpu;
static const char *g_arm_float_abi;
static const char *g_arm_cpu;
static int g_arm_cmse;             /* -mcmse was given */
/* The unit -march='s +fp, +fp.dp or +nofp names: the FPU when -mfpu= is
 * not given (or is auto), as GCC takes the pair. */
static const char *g_arm_march_fpu;

/* What the two ARM float flags mean together, decided once every argument has
 * been seen.
 *
 * The FPUs EmbCC knows are the units its parts carry: FPv4-SP-D16 on a
 * Cortex-M4F and FPv5-D16 on a Cortex-M7 (both ARMv7E-M), and FPv5-SP-D16
 * on a Cortex-M33 (ARMv8-M Mainline). The first and the last are single
 * precision, so `double` stays in software there; FPv5-D16 computes
 * `double` too, and the backend emits .f64 arithmetic for it. Anything
 * else is refused BY NAME -- an unknown name is not a thing to guess at.
 *
 * The part's own unit, which an -eabihf triple implies, follows -mcpu=:
 * thumbv7em-none-eabihf alone is a Cortex-M4F, as it is to clang, and
 * with -mcpu=cortex-m7 it is the M7 and its double-precision unit.
 *
 *   soft (the default, as for arm-none-eabi-gcc): no FPU instructions, even
 *       with an -mfpu= -- which is GCC's reading of the pair.
 *   softfp: FPU instructions, float arguments in the CORE registers. Links
 *       with soft-float objects, because the calling convention is theirs.
 *   hard: FPU instructions, and floating point passed and returned in
 *       s0-s15 / d0-d7 (AAPCS-VFP); docs/manual/invoking.md, -mfloat-abi=.
 *
 * The object says which it was built for (Tag_FP_arch, Tag_ABI_HardFP_use,
 * Tag_ABI_VFP_args) and the predefined macros say so to the program
 * (__ARM_FP, __ARM_VFPV4__, __SOFTFP__); both follow from what is set here. */
static void arm_float_resolve(void)
{
    if (target_get() != TARGET_THUMB)
        return;                        /* refused where the flag was parsed */
    /* -mcmse needs the security extension, which only ARMv8-M has. */
    if (g_arm_cmse && !target_thumb_v8m())
        diag_fatal(NULL, 0, "-mcmse is the Secure side of ARMv8-M's "
                   "security extension, and %s is not ARMv8-M: use "
                   "thumbv8m.main-none-eabi or thumbv8m.base-none-eabi",
                   target_triple_now());
    /* ARMv7-A: VFPv3 or VFPv4, D16 or D32 -- every Cortex-A's unit but
     * NEON's SIMD, which nothing here emits and so nothing may claim. The
     * code is the Cortex-M7's (single and double precision on d0-d15),
     * each instruction under a condition field. armv7a-none-eabihf is
     * -mfpu=vfpv3-d16 -mfloat-abi=hard: the unit every Cortex-A with an
     * FPU has. */
    if (target_arm_a32()) {
        static const struct { const char *name; int ver, d32; } units[] = {
            { "vfpv3-d16", 3, 0 }, { "vfpv3", 3, 1 }, { "vfp3", 3, 1 },
            { "vfpv4-d16", 4, 0 }, { "vfpv4", 4, 1 }, { "vfp4", 4, 1 },
            { NULL, 0, 0 }
        };
        int hfa = target_thumb_hf_name(), unit = -1;
        const char *a = g_arm_float_abi ? g_arm_float_abi : hfa ? "hard" : "soft";
        const char *u = g_arm_fpu ? g_arm_fpu : hfa ? "vfpv3-d16" : NULL;
        int named = u && strcmp(u, "none") && strcmp(u, "soft") &&
                    strcmp(u, "auto");
        if (strcmp(a, "soft") && strcmp(a, "softfp") && strcmp(a, "hard"))
            diag_fatal(NULL, 0, "-mfloat-abi=%s is not an ARM float ABI: it "
                       "is one of soft, softfp and hard", a);
        for (int k = 0; named && units[k].name; k++)
            if (!strcmp(u, units[k].name))
                unit = k;
        if (named && unit < 0)
            diag_fatal(NULL, 0, "-mfpu=%s is not supported on %s: EmbCC "
                       "emits VFPv3 or VFPv4 (-mfpu=vfpv3-d16, vfpv3, "
                       "vfpv4-d16, vfpv4) there, and no NEON (Advanced SIMD) "
                       "instruction", u, target_triple_now());
        if (!strcmp(a, "soft"))
            return;                    /* no FPU instructions, as GCC reads it */
        if (!named)
            diag_fatal(NULL, 0, "-mfloat-abi=%s needs an FPU to use: add "
                       "-mfpu=vfpv3-d16 (or vfpv3, vfpv4-d16, vfpv4)", a);
        target_set_thumb_hard_abi(!strcmp(a, "hard"));
        target_set_thumb_fpu(1);
        target_set_thumb_fpu_dp(1);
        target_set_arm_vfp(units[unit].ver, units[unit].d32);
        return;
    }
    /* An -eabihf triple is shorthand for the part's FPU and the hard
     * convention; a flag that says otherwise wins, as with clang. */
    int hf = target_thumb_hf_name();
    int m7 = g_arm_cpu && strcmp(g_arm_cpu, "cortex-m7") == 0;
    const char *hf_fpu = target_thumb_arch() >= 8 ? "fpv5-sp-d16"
                       : m7 ? "fpv5-d16" : "fpv4-sp-d16";
    const char *abi = g_arm_float_abi ? g_arm_float_abi : hf ? "hard" : "soft";
    if ((!g_arm_fpu || !strcmp(g_arm_fpu, "auto")) && g_arm_march_fpu)
        g_arm_fpu = g_arm_march_fpu;
    if (!g_arm_fpu && hf)
        g_arm_fpu = hf_fpu;
    int fpu_named = g_arm_fpu && strcmp(g_arm_fpu, "none") != 0 &&
                    strcmp(g_arm_fpu, "soft") != 0 && strcmp(g_arm_fpu, "auto") != 0;
    if (!g_arm_fpu && !g_arm_float_abi)
        return;
    if (strcmp(abi, "soft") && strcmp(abi, "softfp") && strcmp(abi, "hard"))
        diag_fatal(NULL, 0, "-mfloat-abi=%s is not an ARM float ABI: it is "
                   "one of soft, softfp and hard", abi);
    /* No ARMv6-M part has an FPU: only the base standard means anything. */
    if (target_thumb_arch() == 6 && (fpu_named || strcmp(abi, "soft")))
        diag_fatal(NULL, 0, "%s%s on %s: %s has no FPU, so floating point "
                   "is soft and travels in the core registers",
                   fpu_named ? "-mfpu=" : "-mfloat-abi=",
                   fpu_named ? g_arm_fpu : abi, target_triple_now(),
                   target_thumb_v8m_base()
                   ? "an ARMv8-M Baseline core (Cortex-M23)"
                   : "an ARMv6-M core (Cortex-M0, M0+, M1)");
    if (fpu_named) {
        int v8 = target_thumb_arch() >= 8;
        int dp = strcmp(g_arm_fpu, "fpv5-d16") == 0;
        /* FPv5-D16 on ARMv8-M is refused with the others: the Mainline
         * part this backend knows, the Cortex-M33, has the single-precision
         * FPv5, and its attributes and tables are the only ones checked. */
        if (v8 ? strcmp(g_arm_fpu, "fpv5-sp-d16") != 0
               : strcmp(g_arm_fpu, "fpv4-sp-d16") != 0 && !dp)
            diag_fatal(NULL, 0, "-mfpu=%s is not supported on %s: EmbCC "
                       "emits VFP for %s and nothing else: another unit's "
                       "instruction set and attributes are unchecked here",
                       g_arm_fpu, target_triple_now(),
                       v8 ? "the Cortex-M33's unit (-mfpu=fpv5-sp-d16)"
                          : "the Cortex-M4F's unit (-mfpu=fpv4-sp-d16) and "
                            "the Cortex-M7's (-mfpu=fpv5-d16)");
        if (!v8 && !target_thumb_em())
            diag_fatal(NULL, 0, "-mfpu=%s is an ARMv7E-M unit, and the part "
                       "is ARMv7-M (a Cortex-M3 has no FPU); add -mcpu=%s",
                       g_arm_fpu, dp ? "cortex-m7" : "cortex-m4");
        /* The double-precision unit is the M7's and no other part's: an
         * M4 told it has one would run .f64 instructions it does not
         * implement, which is a UsageFault at the first double. */
        if (dp && g_arm_cpu && !m7)
            diag_fatal(NULL, 0, "-mfpu=fpv5-d16 is the Cortex-M7's "
                       "double-precision unit, and -mcpu=%s does not have "
                       "it; the Cortex-M4F's is -mfpu=fpv4-sp-d16", g_arm_cpu);
    }
    if (!strcmp(abi, "soft"))
        return;                        /* no FPU instructions, as GCC reads it */
    if (!fpu_named)
        diag_fatal(NULL, 0, "-mfloat-abi=%s needs an FPU to use: add "
                   "-mfpu=fpv4-sp-d16 (Cortex-M4F), -mfpu=fpv5-d16 "
                   "(Cortex-M7) or -mfpu=fpv5-sp-d16 (Cortex-M33)", abi);
    /* hard: the FPU's arithmetic, and floating point passed and
     * returned in s0-s15 / d0-d7 (AAPCS-VFP). The runtime helpers keep the
     * base convention either way, as the RTABI requires. The convention
     * is the same for every unit: a double travels in a d register on an
     * M4F too, which only cannot compute with it. */
    target_set_thumb_hard_abi(!strcmp(abi, "hard"));
    target_set_thumb_fpu(1);
    target_set_thumb_fpu_dp(strcmp(g_arm_fpu, "fpv5-d16") == 0);
}

/* -Wp,A,B,...: options for the preprocessor, split at the commas and put
 * back in the argument list for the ordinary parse. Only the ones whose
 * meaning is the same there -- -D, -U, -I with their values joined -- are
 * taken; any other is refused by name. Each was the warning "is not a
 * warning EmbCC has", and the -D it carried was simply lost. */
static int expand_wp(int *argcp, char ***argvp)
{
    int argc = *argcp, n = 0, cap = argc + 1;
    char **argv = *argvp;
    char **out = xcalloc((size_t)cap, sizeof *out);
    for (int i = 0; i < argc; i++) {
        char *list = strncmp(argv[i], "-Wp,", 4) == 0
                   ? xstrndup(argv[i] + 4, strlen(argv[i] + 4)) : NULL;
        for (char *t = list ? list : argv[i]; t; ) {
            char *c = list ? strchr(t, ',') : NULL;
            if (c) *c = '\0';
            if (list && *t && (t[0] != '-' || (t[1] != 'D' && t[1] != 'U' &&
                                               t[1] != 'I') || !t[2])) {
                fprintf(stderr, "embcc: error: preprocessor option '%s' in "
                        "%s is not supported; pass -D, -U or -I with its "
                        "value, or give the option directly\n", t, argv[i]);
                return 1;
            }
            if (*t || !list) {
                if (n + 1 >= cap) {
                    cap *= 2;
                    out = xrealloc(out, (size_t)cap * sizeof *out);
                }
                out[n++] = t;
            }
            t = c ? c + 1 : NULL;
        }
    }
    out[n] = NULL;
    *argcp = n;
    *argvp = out;
    return 0;
}

int main(int argc, char **argv)
{
    plat_set_argv0(argc > 0 ? argv[0] : NULL);   /* where this program is */
    if (expand_wp(&argc, &argv))
        return 1;
    g_child_skip = xcalloc((size_t)(argc > 0 ? argc : 1), 1);

    /* -fsanitize state: which checks, and whether trap mode was
     * asked for by name (it is the only mode, so this only has to be
     * accepted, not acted on). */
    unsigned san_mask = 0;
    int want_instr = 0;              /* -finstrument-functions */
    int short_wchar = 0;             /* -fshort-wchar, the last one wins */
    const char *instr_funcs = NULL, *instr_files = NULL;
    int san_trap_asked = 0;
    (void)san_trap_asked;
    const char *input = NULL, *output = NULL;
    int out_is_stdout = 0;          /* `-o -` */
    int compile_mode = 0, pp_only = 0;
    int lang = -1;                  /* -x: 0 C, 1 C++; -1 by suffix */
    /* GNU89 inline semantics: -fgnu89-inline, or -std=c89/gnu89, where
     * C99's are not available (clang ignores -fno-gnu89-inline there) */
    int gnu89_inline = 0, std_gnu89 = 0;

    if (argc < 2) {
        print_usage(stderr);
        return 1;
    }
    /* Tool mode (§17): `embcc inspect <stage> <file> [flags]`. A subcommand,
     * not a flag, because it does not modify a compilation -- it replaces
     * one. The remaining argv is an ordinary command line, so -I, -D and -O
     * work exactly as they do when compiling, and what you inspect is what
     * you would have built. */
    /* `embcc why <decision> [subject] <file> [options]` (§19). The answer
     * comes from remarks the passes recorded, so the compile really runs --
     * which is why the -O level matters, and why the message says so when
     * nothing was recorded. */
    if (!strcmp(argv[1], "why")) {
        if (argc < 4) {
            fprintf(stderr,
                "usage: embcc why <decision> [subject] <file> [options]\n"
                "decisions: inlined, not-inlined\n"
                "example:   embcc why not-inlined helper prog.c -O2\n");
            return 1;
        }
        why_decision = argv[2];
        int shift = 2;
        /* an optional subject: argv[3], unless that is already the file */
        if (argc > 4 && argv[3][0] != '-' && strchr(argv[3], '.') == NULL) {
            why_subject = argv[3];
            shift = 3;
        }
        argv[shift] = argv[0];
        argv += shift;
        argc -= shift;
    }
    if (!strcmp(argv[1], "inspect")) {
        static const char *const stages[] = {
            "tokens", "pp", "ast", "symbols", "types", "ir", "cfg",
            "callgraph"
        };
        if (argc < 4) {
            fprintf(stderr,
                "usage: embcc inspect <stage> <file> [options]\n"
                "stages, in pipeline order:\n"
                "  tokens   what the lexer made of the preprocessed source\n"
                "  pp       the preprocessed source itself\n"
                "  ast      the tree the parser built\n"
                "  symbols  what the unit declares, with resolved types\n"
                "  types    struct layout: offsets, bit-fields, padding\n"
                "  ir       EmbIR at the current -O level\n"
                "  cfg      the control-flow graph the passes reason about\n"
                "  callgraph who calls whom, from the IR\n");
            return 1;
        }
        /* §18 lists `mir` too, and EmbCC has no EmbMIR: instruction
         * selection writes bytes straight from EmbIR. Saying that is more
         * use than "unknown stage", because the reader is asking for a
         * level of the design (§9.2) that was never built. */
        if (!strcmp(argv[2], "mir")) {
            fprintf(stderr,
                "embcc: there is no EmbMIR to inspect.\n"
                "Instruction selection emits machine code directly from "
                "EmbIR (src/arch/<arch>/codegen.c),\n"
                "so the separate machine-IR level of the design (vision "
                "§9.2) does not exist.\n"
                "The nearest views are `inspect ir` (what codegen is "
                "given) and `embdbg` (what it produced).\n");
            return 1;
        }
        int known = 0;
        for (size_t k = 0; k < sizeof stages / sizeof stages[0]; k++)
            if (!strcmp(argv[2], stages[k]))
                known = 1;
        if (!known) {
            fprintf(stderr, "embcc: inspect: unknown stage '%s'\n", argv[2]);
            return 1;
        }
        inspect_stage = argv[2];
        argv[2] = argv[0];            /* shift: argv[2..] is now the command */
        argv += 2;
        argc -= 2;
    }
    /* The CONFIGURED default, before the command line is read, so a
     * --target= below simply overwrites it. A compiler built with
     * `make DEFAULT_TARGET=riscv32-unknown-elf` compiles for the board
     * when it is handed nothing at all. */
    {
        const char *bad = NULL;
        if (!target_apply_default(&bad)) {
            fprintf(stderr, "embcc: error: the default target '%s' is not "
                            "one EmbCC knows\n", bad);
            fprintf(stderr, "embcc: it came from %s\n",
                    plat_getenv("EMBCC_DEFAULT_TARGET")
                        ? "EMBCC_DEFAULT_TARGET in the environment"
                        : "the DEFAULT_TARGET this compiler was built with");
            fprintf(stderr, "embcc: the targets it emits for are:\n");
            for (int t = 0; t < target_triple_count(); t++)
                fprintf(stderr, "embcc:   %s\n", target_triple_name(t));
            return 1;
        }
    }
    /* Scanned ahead of everything else: --version and --dump-predef must
     * describe the target that was asked for, not the default. */
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--target=", 9) != 0)
            continue;
        enum target_arch a;
        enum target_os os;
        enum target_fmt fmt;
        if (!target_from_triple(argv[i] + 9, &a, &os, &fmt)) {
            fprintf(stderr, "embcc: error: unknown target '%s'\n",
                    argv[i] + 9);
            fprintf(stderr, "embcc: the targets it emits for are:\n");
            for (int t = 0; t < target_triple_count(); t++)
                fprintf(stderr, "embcc:   %s\n", target_triple_name(t));
            return 1;
        }
        target_set(a);
        target_os_set(os);
        target_fmt_set(fmt);
    }
    /* The backend's "this op calls a runtime helper" predicate, for the
     * optimizer's view of what a value crosses: x86-64 has no such
     * helpers and AVR's allocator does not model them. Set from the
     * target finally chosen -- it was set only for a --target=, so a
     * compiler whose DEFAULT is a board made different code from the
     * same compiler told that board by name. */
    {
        enum target_arch a = target_get();
        /* ...and the encoder the Thumb backend writes through: A32 for
         * armv7a-none-eabi. Here, before anything is compiled, because the
         * optimizer asks the encoder which constants an instruction can
         * carry (thumb_imm_foldable) long before code generation. */
        t_isa_a32 = target_arm_a32();
        target_set_calls_helper(backend_get(a)->op_calls_helper);
        /* the MIPS encoder's byte order, for the code generator and the
         * inline and file-scope assemblers alike */
        mips_set_big_endian(target_big_endian());
        /* ...and its width: the doubleword instructions at MIPS64 */
        mips_set_64(a == TARGET_MIPS64);
    }
    /* Scanned across the whole command line, not just argv[1]: these
     * describe the TARGET, so `--target=aarch64-elf --dump-predef` has to
     * mean the aarch64 table rather than an unknown-argument error. */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0) {
            print_version();
            return 0;
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(stdout);
            print_options(stdout);
            return 0;
        }
        if (strcmp(argv[i], "--help-warnings") == 0) {
            printf("the warnings EmbCC has, and the group each is in:\n\n");
            for (int k = 0; k < diag_warning_count(); k++) {
                int g = diag_warning_group(k);
                printf("  -W%-22s %s\n", diag_warning_name(k),
                       g == 1 ? "(-Wall)" : g == 2 ? "(-Wextra)" : "");
            }
            printf("\nEach can be turned off with -Wno-NAME, and every one\n"
                   "prints its name, so a diagnostic says what controls it.\n");
            return 0;
        }
        if (strcmp(argv[i], "--explain") == 0)
            return explain_print(i + 1 < argc ? argv[i + 1] : NULL);
        if (strncmp(argv[i], "--explain=", 10) == 0)
            return explain_print(argv[i] + 10);
        if (strcmp(argv[i], "-dumpmachine") == 0) {
            /* The canonical spelling of whatever --target= chose, from
             * the one table that knows them. A chain of ternaries here
             * was a second list to keep in step, and it silently
             * printed x86_64-elf for every target added after it.
             *
             * Answered after the arguments, as --dump-predef is: the
             * name also says the float ABI (thumbv7em-none-eabihf), which
             * -mfloat-abi= can change. */
            g_want_dumpmachine = 1;
            continue;
        }
        if (strcmp(argv[i], "--dump-predef") == 0)
            /* NOT answered here: this scan has applied --target= and
             * nothing else, and the table also depends on -mcpu=, -mfpu=
             * and -mfloat-abi=. Answering early printed the soft-float
             * table for an FPU build -- a tool for inspecting the
             * configuration that described a different one. */
            g_want_dump_predef = 1;
    }
    if (strcmp(argv[1], "--emit-empty-object") == 0) {
        if (argc != 3) {
            fprintf(stderr, "embcc: --emit-empty-object needs a FILE\n");
            return 1;
        }
        return emit_empty_object(argv[2]);
    }

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--target=", 9) == 0) {
            /* already applied in the pre-scan above */
        } else if (strcmp(argv[i], "-c") == 0) {
            compile_mode = 1;
        } else if (strncmp(argv[i], "-x", 2) == 0) {
            const char *l = argv[i][2] ? argv[i] + 2
                                       : (i + 1 < argc ? argv[++i] : "");
            g_x_gas = 0;
            if (strcmp(l, "c++") == 0 || strcmp(l, "c++-cpp-output") == 0)
                lang = 1;
            else if (strcmp(l, "c") == 0 || strcmp(l, "cpp-output") == 0)
                lang = 0;
            else if (strcmp(l, "none") == 0)
                lang = -1;
            else if (strcmp(l, "assembler") == 0)
                lang = -1, g_x_gas = 1;
            else if (strcmp(l, "assembler-with-cpp") == 0)
                lang = -1, g_x_gas = 2;
            else {
                fprintf(stderr, "embcc: error: unknown language '%s' for "
                                "-x (c, c++, assembler or "
                                "assembler-with-cpp)\n", l);
                return 1;
            }
        } else if (strncmp(argv[i], "-std=", 5) == 0) {
            /* C: accepted (C11 with GNU extensions, always). C++: the
             * standard __cplusplus and the feature macros say (libstdc++'s
             * sources pick their code by them); the language EmbCC reads
             * is C++20's either way */
            const char *v = argv[i] + 5;
            int strict = strncmp(v, "c++", 3) == 0;
            if (strict || strncmp(v, "gnu++", 5) == 0) {
                const char *y = v + (strict ? 3 : 5);
                int year = !strcmp(y, "98") || !strcmp(y, "03") ? 1998
                           : !strcmp(y, "11") || !strcmp(y, "0x") ? 2011
                           : !strcmp(y, "14") || !strcmp(y, "1y") ? 2014
                           : !strcmp(y, "17") || !strcmp(y, "1z") ? 2017
                           : !strcmp(y, "20") || !strcmp(y, "2a") ? 2020
                           : !strcmp(y, "23") || !strcmp(y, "2b") ? 2023
                           : !strcmp(y, "26") || !strcmp(y, "2c") ? 2026 : 0;
                if (!year) {
                    fprintf(stderr, "embcc: error: unknown C++ standard "
                                    "'%s'\n", argv[i]);
                    return 1;
                }
                cpp_set_cxx_std(year, strict);
                /* The MACROS follow the year -- libstdc++ picks its
                 * code by them -- but the language EmbCC parses is
                 * C++20 whatever was asked. A lambda under -std=c++98
                 * compiles, which is worth saying once rather than
                 * letting a build believe it checked. */
                if (year < 2020)
                    fprintf(stderr, "embcc: warning: %s sets the standard "
                                    "macros but is not enforced; EmbCC "
                                    "parses C++20 and will accept newer "
                                    "constructs\n", argv[i]);
            } else {
                /* A C standard. EmbCC has ONE C dialect -- C11 with the
                 * GNU extensions -- and says so rather than nodding:
                 * `-std=c89` used to be accepted and then compile
                 * `for (int i = ...)` happily, which is a build
                 * believing it checked something it did not. An
                 * unknown name is refused outright. */
                static const char *const known[] = {
                    "c89", "c90", "iso9899:1990", "gnu89", "gnu90",
                    "c99", "c9x", "iso9899:1999", "gnu99", "gnu9x",
                    "c11", "c1x", "iso9899:2011", "gnu11", "gnu1x",
                    "c17", "c18", "iso9899:2017", "gnu17", "gnu18",
                    "c23", "c2x", "gnu23", "gnu2x"
                };
                int ok = 0;
                for (size_t k = 0; k < sizeof known / sizeof known[0]; k++)
                    if (strcmp(v, known[k]) == 0) { ok = 1; break; }
                if (!ok) {
                    fprintf(stderr, "embcc: error: unknown standard '%s'\n",
                            argv[i]);
                    return 1;
                }
                std_gnu89 = !strcmp(v, "c89") || !strcmp(v, "c90") ||
                            !strcmp(v, "iso9899:1990") ||
                            !strcmp(v, "gnu89") || !strcmp(v, "gnu90");
                if (strncmp(v, "c1", 2) != 0 && strncmp(v, "gnu1", 4) != 0 &&
                    strcmp(v, "iso9899:2011") != 0 &&
                    strcmp(v, "iso9899:2017") != 0 &&
                    strncmp(v, "c2", 2) != 0 && strncmp(v, "gnu2", 4) != 0 &&
                    strcmp(v, "c17") != 0 && strcmp(v, "c18") != 0)
                    fprintf(stderr, "embcc: warning: %s is accepted but not "
                                    "enforced; EmbCC has one C dialect, C11 "
                                    "with the GNU extensions, and will "
                                    "compile newer constructs anyway\n",
                            argv[i]);
            }
        } else if (strcmp(argv[i], "--emit-c") == 0) {
            emit_c_only = 1;
        } else if (strcmp(argv[i], "-dM") == 0) {
            /* with -E: the macros defined at the end of preprocessing,
             * not the text (build systems ask the compiler this way) */
            cpp_set_dump_macros(1);
        } else if (strcmp(argv[i], "-E") == 0) {
            pp_only = 1;
        } else if (strcmp(argv[i], "-ggdb") == 0 ||
                   strcmp(argv[i], "-g1") == 0 ||
                   strcmp(argv[i], "-g2") == 0 ||
                   strcmp(argv[i], "-g3") == 0 ||
                   strcmp(argv[i], "-gdwarf") == 0 ||
                   strcmp(argv[i], "-gdwarf-2") == 0 ||
                   strcmp(argv[i], "-gdwarf-3") == 0 ||
                   strcmp(argv[i], "-gdwarf-4") == 0) {
            /* All of these mean -g here. EmbCC emits one kind of debug
             * information -- DWARF 4 -- so a level or a version that
             * asks for no more than that is simply -g. A version it
             * does NOT emit is refused below rather than quietly
             * downgraded: a build that asked for DWARF 5 and got 4
             * would find out from its debugger. */
            want_debug = 1;
        } else if (strncmp(argv[i], "-gdwarf-", 8) == 0 ||
                   strcmp(argv[i], "-gsplit-dwarf") == 0 ||
                   strcmp(argv[i], "-gz") == 0) {
            fprintf(stderr, "embcc: error: %s is not supported; EmbCC "
                            "emits DWARF 4, uncompressed and in one "
                            "piece\n", argv[i]);
            return 1;
        } else if (strcmp(argv[i], "-g") == 0) {
            want_debug = 1;
        } else if (strcmp(argv[i], "-funwind-tables") == 0 ||
                   strcmp(argv[i], "-fasynchronous-unwind-tables") == 0) {
            want_unwind = 1;
        } else if (strcmp(argv[i], "-fexceptions") == 0) {
            want_unwind = 1;
            want_exceptions = 1;
        } else if (strcmp(argv[i], "-fno-unwind-tables") == 0 ||
                   strcmp(argv[i], "-fno-asynchronous-unwind-tables") == 0) {
            want_unwind = 0;
        } else if (strcmp(argv[i], "-fchar8_t") == 0) {
            cpp_set_cxx_char8(1);   /* (C++: char8_t is a keyword anyway) */
        } else if (strcmp(argv[i], "-fdiagnostics-parseable-fixits") == 0) {
            diag_set_parseable_fixits(1);
        } else if (strcmp(argv[i], "--fix") == 0) {
            want_fix = 1;
            syntax_only = 1;          /* the point is the edit, not an object */
        } else if (strcmp(argv[i], "--emit-interfaces") == 0) {
            want_iface = 1;
            compile_mode = 1;      /* a report, not an object */
        } else if (strcmp(argv[i], "-S") == 0) {
            want_asm = 1;
            compile_mode = 1;      /* like -c: no link */
        } else if (strcmp(argv[i], "-fstack-usage") == 0) {
            want_stack_usage = 1;
        } else if (strcmp(argv[i], "-fsyntax-only") == 0) {
            syntax_only = 1;
        } else if (strcmp(argv[i], "-ftime-report") == 0) {
            if (!g_time_report) {
                g_time_report = 1;
                g_time_start = g_time_last = clock();
                atexit(time_report);
            }
        } else if (strcmp(argv[i], "-fremarks") == 0) {
            want_remarks = 1;
        } else if (strcmp(argv[i], "-fremarks=json") == 0) {
            want_remarks = remarks_json = 1;
        } else if (strcmp(argv[i], "-M") == 0) {
            dep_mode = 1; dep_only = 1;
        } else if (strcmp(argv[i], "-MM") == 0) {
            dep_mode = 2; dep_only = 1;
        } else if (strcmp(argv[i], "-MD") == 0) {
            dep_mode = 1;
        } else if (strcmp(argv[i], "-MMD") == 0) {
            dep_mode = 2;
        } else if (strcmp(argv[i], "-MP") == 0) {
            dep_phony = 1;
        } else if (strcmp(argv[i], "-MF") == 0 || strcmp(argv[i], "-MT") == 0 ||
                   strcmp(argv[i], "-MQ") == 0) {
            int mf = argv[i][2] == 'F';
            if (i + 1 >= argc) {
                fprintf(stderr, "embcc: %s needs a file name\n", argv[i]);
                return 1;
            }
            if (mf) dep_file = argv[++i]; else dep_target = argv[++i];
        } else if (strcmp(argv[i], "-fno-exceptions") == 0) {
            want_exceptions = 0;
        } else if (strcmp(argv[i], "-fno-access-control") == 0) {
            /* GCC's spelling, and it means what it says: private and
             * protected stop being enforced. Worth having for the same
             * reason GCC has it -- a test that reaches into a class's
             * internals, and a build bisecting a new diagnostic. */
            access_set_enabled(0);
        } else if (strcmp(argv[i], "-faccess-control") == 0) {
            access_set_enabled(1);
        } else if (strcmp(argv[i], "-fno-stack-protector") == 0) {
            /* what EmbCC does: it emits no stack protection. The positive
             * forms are refused below rather than quietly ignored. */
        } else if (strncmp(argv[i], "-fstack-protector", 17) == 0) {
            fprintf(stderr, "embcc: '%s' is not supported (EmbCC emits no "
                            "stack protection); -fno-stack-protector is\n",
                    argv[i]);
            return 1;
        } else if (strcmp(argv[i], "-fno-rtti") == 0) {
            want_rtti = 0;
        } else if (strcmp(argv[i], "-frtti") == 0) {
            want_rtti = 1;
        } else if (strncmp(argv[i], "-Wl,", 4) == 0 ||
                   strcmp(argv[i], "-Xlinker") == 0) {
            /* split at the commas, as GCC does; applied at the link */
            if (argv[i][1] == 'X') {
                if (i + 1 == argc) {
                    fprintf(stderr, "embcc: -Xlinker needs an option\n");
                    return 1;
                }
                if (g_nwl < (int)(sizeof g_wl / sizeof g_wl[0]))
                    g_wl[g_nwl++] = argv[++i];
            } else {
                char *list = xstrndup(argv[i] + 4, strlen(argv[i] + 4));
                for (char *t = list; t; ) {
                    char *c = strchr(t, ',');
                    if (c) *c = '\0';
                    if (*t && g_nwl < (int)(sizeof g_wl / sizeof g_wl[0]))
                        g_wl[g_nwl++] = t;
                    t = c ? c + 1 : NULL;
                }
            }
        } else if (strncmp(argv[i], "-Wa,", 4) == 0) {
            /* the integrated assembler takes no options; these are the
             * ones that change nothing it produces */
            char *list = xstrndup(argv[i] + 4, strlen(argv[i] + 4));
            for (char *t = list; t; ) {
                char *c = strchr(t, ',');
                if (c) *c = '\0';
                /* -a[cdghlmns][=FILE]: a listing */
                if (t[0] == '-' && t[1] == 'a') {
                    const char *q = t + 2;
                    while (*q && strchr("cdghlmns", *q))
                        q++;
                    if (*q == '=' && q[1]) {
                        g_listing = xstrndup(q + 1, strlen(q + 1));
                        t = c ? c + 1 : NULL;
                        continue;
                    }
                    if (!*q) {
                        if (!g_listing)
                            g_listing = "-";
                        t = c ? c + 1 : NULL;
                        continue;
                    }
                }
                if (*t && strcmp(t, "--noexecstack") && strcmp(t, "-g") &&
                    strncmp(t, "--gdwarf", 8) && strcmp(t, "-mrelax")) {
                    fprintf(stderr, "embcc: error: assembler option '%s' is "
                                    "not one the integrated assembler has\n",
                            t);
                    return 1;
                }
                t = c ? c + 1 : NULL;
            }
        } else if (strcmp(argv[i], "-Werror") == 0) {
            diag_set_werror(1);
        } else if (strcmp(argv[i], "-Wno-error") == 0) {
            diag_set_werror(0);
        } else if (strncmp(argv[i], "-Werror=", 8) == 0 ||
                   strncmp(argv[i], "-Wno-error=", 11) == 0) {
            /* one warning made an error, or exempted from -Werror */
            int on = argv[i][2] == 'e';
            const char *nm = argv[i] + (on ? 8 : 11);
            if (diag_always_error(nm)) {
                /* an error already; -Wno-error= cannot make it less */
                if (!on)
                    fprintf(stderr, "embcc: warning: %s: an implicit "
                                    "function declaration is always an "
                                    "error in EmbCC\n", argv[i]);
            } else if (!diag_set_werror_for(nm, on))
                fprintf(stderr, "embcc: warning: %s names no warning EmbCC "
                                "has (--help-warnings lists them)\n",
                        argv[i]);
        } else if (strcmp(argv[i], "-Werror-implicit-function-"
                                   "declaration") == 0) {
            /* GCC's old spelling of -Werror=implicit-function-declaration,
             * and as that one, what EmbCC always does */
        } else if (strcmp(argv[i], "-w") == 0) {
            diag_set_no_warnings(1);
        } else if (strcmp(argv[i], "-pedantic") == 0 ||
                   strcmp(argv[i], "-pedantic-errors") == 0) {
            /* Said, as an unknown -Wname is: EmbCC has no warnings about
             * extensions to ISO C, so this turns nothing on. It was
             * refused as an unknown argument, which stopped builds that
             * pass it out of habit. */
            fprintf(stderr, "embcc: warning: %s: EmbCC has no diagnostics "
                            "for extensions to ISO C, so this turns "
                            "nothing on\n", argv[i]);
        } else if (strncmp(argv[i], "-fdiagnostics-format=", 21) == 0) {
            const char *f = argv[i] + 21;
            if (strcmp(f, "json") == 0)
                diag_set_format(DIAG_JSON);
            else if (strcmp(f, "text") == 0)
                diag_set_format(DIAG_TEXT);
            else {
                fprintf(stderr, "embcc: unknown diagnostic format '%s' "
                                "(text, json)\n", f);
                return 1;
            }
        } else if (strncmp(argv[i], "-fdiagnostics-color", 19) == 0) {
            const char *m = argv[i] + 19;
            diag_set_color(*m == '\0' || strcmp(m, "=always") == 0 ? 1
                           : strcmp(m, "=never") == 0 ? 0 : -1);
        } else if (strcmp(argv[i], "-fno-diagnostics-color") == 0) {
            diag_set_color(0);
        } else if (strncmp(argv[i], "-fmax-errors=", 13) == 0) {
            diag_set_max_errors(atoi(argv[i] + 13));
        } else if (strcmp(argv[i], "-Wsystem-headers") == 0) {
            diag_set_warn_system(1);
        } else if (strcmp(argv[i], "-Wall") == 0) {
            diag_enable_group(1, 0);
        } else if (strcmp(argv[i], "-Wextra") == 0 ||
                   strcmp(argv[i], "-W") == 0) {
            diag_enable_group(0, 1);
        } else if (strncmp(argv[i], "-Wno-", 5) == 0) {
            diag_enable_warning(argv[i] + 5, 0);
        } else if (argv[i][0] == '-' && argv[i][1] == 'W') {
            /* -Wname turns one on; a name EmbCC does not have is accepted
             * and ignored, so a build that passes GCC's whole warning
             * vocabulary still compiles. */
            /* An unknown warning name is reported, not swallowed.
             * Every -W... used to be accepted in silence, so a build
             * turning on -Wcast-align and passing clean had learned
             * nothing -- and a TYPO in a warning name was invisible.
             * A warning rather than an error, which is what GCC does,
             * because a build should not stop over a diagnostic it
             * asked for and this compiler does not have. */
            if (!diag_enable_warning(argv[i] + 2, 1) &&
                !diag_always_error(argv[i] + 2))
                fprintf(stderr, "embcc: warning: %s is not a warning EmbCC "
                                "has, so it turns nothing on "
                                "(--help-warnings lists them)\n", argv[i]);
        } else if (strncmp(argv[i], "-O", 2) == 0) {
            /* -O/-O1, -O2, -O3 and -Os. -O0 turns the optimizer off,
             * which is what keeps the self-host fixed point. */
            /* The LAST -O wins, size mode included: `-Os -O0` kept
             * optimizing for size at -O0, and `-Os -O2` was still -Os. */
            const char *lvl = argv[i] + 2;
            if (lvl[0] == '\0')
                { opt_level = 1; opt_for_size = 0; }
            else if (lvl[0] == 's' && lvl[1] == '\0')
                { opt_level = 2; opt_for_size = 1; }
            else if (lvl[1] == '\0' && lvl[0] >= '0' && lvl[0] <= '3')
                { opt_level = lvl[0] - '0'; opt_for_size = 0; }
            else if (lvl[0] == 'z' && lvl[1] == '\0')
                { opt_level = 2; opt_for_size = 1; }   /* -Oz is -Os here */
            /* -Og, "optimize for debugging": the level that removes work
             * without moving the program around, which here is -O1.
             * -Ofast is -O3 and nothing more -- GCC adds -ffast-math,
             * which EmbCC does not do (see -ffast-math below), so no
             * result can differ from -O3's. */
            else if (!strcmp(lvl, "g"))
                { opt_level = 1; opt_for_size = 0; }
            else if (!strcmp(lvl, "fast"))
                { opt_level = 3; opt_for_size = 0; }
            else {
                fprintf(stderr, "embcc: unknown optimization flag '%s'\n",
                        argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "-fsigned-char") == 0 ||
                   strcmp(argv[i], "-funsigned-char") == 0) {
            /* Plain `char`'s signedness. Each target has a default
             * (src/arch/target.c) and this overrides it, as it does
             * everywhere else -- code that memcmp's its way through a
             * buffer of `char` gets a different answer either way, so
             * a build that asks has to be obeyed. */
            {
                int uns = argv[i][2] == 'u';
                target_set_char_signed(uns);
                /* The MACRO has to move with the type. It comes from
                 * the per-target predefined table, which knows only
                 * the default, so a header testing __CHAR_UNSIGNED__
                 * would otherwise contradict the compiler that reads
                 * it -- and that header is usually deciding whether
                 * to sign-extend by hand. */
                cpp_cmdline_define(uns ? "__CHAR_UNSIGNED__=1"
                                       : "__CHAR_UNSIGNED__", !uns);
            }
        } else if (strcmp(argv[i], "-fshort-wchar") == 0 ||
                   strcmp(argv[i], "-fno-short-wchar") == 0) {
            short_wchar = argv[i][2] == 's';
        } else if (strcmp(argv[i], "-finstrument-functions") == 0) {
            want_instr = 1;
        } else if (strcmp(argv[i], "-fno-instrument-functions") == 0) {
            want_instr = 0;
        } else if (strncmp(argv[i],
                           "-finstrument-functions-exclude-function-list=",
                           45) == 0) {
            instr_funcs = argv[i] + 45;
        } else if (strncmp(argv[i],
                           "-finstrument-functions-exclude-file-list=",
                           41) == 0) {
            instr_files = argv[i] + 41;
        } else if (strcmp(argv[i], "-ffreestanding") == 0 ||
                   strcmp(argv[i], "-fno-builtin") == 0 ||
                   strcmp(argv[i], "-fno-strict-aliasing") == 0 ||
                   strcmp(argv[i], "-fstrict-aliasing") == 0 ||
                   strcmp(argv[i], "-fwrapv") == 0 ||
                   strcmp(argv[i], "-fno-plt") == 0 ||
                   strcmp(argv[i], "-fomit-frame-pointer") == 0 ||
                   strcmp(argv[i], "-fno-omit-frame-pointer") == 0) {
            /* Accepted because EmbCC ALREADY behaves this way, not
             * because the flag is ignored:
             *
             *   -ffreestanding      it has no hosted assumptions to drop
             *   -fno-builtin        it recognises no library name as a
             *                       builtin; only __builtin_ ones
             *   -f[no-]strict-aliasing  its alias analysis is not
             *                       type-based (src/opt/opt.c), so the
             *                       permissive answer is the only one
             *                       it gives
             *   -fwrapv             signed overflow wraps; nothing here
             *                       optimises on the assumption it cannot
             *   -f[no-]omit-frame-pointer  x86-64 and aarch64 always keep
             *                       one, ARMv7-M and RISC-V never do
             *
             * The opposite spellings are NOT accepted, because those
             * would be promises: see the refusals below. */
        } else if (strcmp(argv[i], "-fno-ident") == 0 ||
                   strcmp(argv[i], "-fident") == 0 ||
                   strcmp(argv[i], "-fno-reorder-functions") == 0 ||
                   strcmp(argv[i], "-freorder-functions") == 0 ||
                   strcmp(argv[i], "-ffp-contract=off") == 0 ||
                   strcmp(argv[i], "-ffp-contract=on") == 0 ||
                   strcmp(argv[i], "-ffp-contract=fast") == 0) {
            /* What EmbCC does already, or a permission it may decline:
             *
             *   -f[no-]ident        it writes no .comment identifying
             *                       itself either way
             *   -f[no-]reorder-functions  functions stay in source
             *                       order; there are no hot or cold
             *                       subsections to move them to
             *   -ffp-contract=      it never fuses a multiply and an add
             *                       (an fma is only __builtin_fma's), so
             *                       off is kept and on/fast only permit */
        } else if (strcmp(argv[i], "-funroll-loops") == 0 ||
                   strcmp(argv[i], "-funroll-all-loops") == 0 ||
                   strcmp(argv[i], "-fno-unroll-loops") == 0) {
            /* GCC's names for the unroll pass (-funroll, -fno-unroll) */
            opt_set_pass("unroll", argv[i][2] != 'n');
        } else if (strcmp(argv[i], "-fcommon") == 0 ||
                   strcmp(argv[i], "-fno-common") == 0) {
            /* -fno-common is the default: a tentative definition is a
             * .bss definition. -fcommon makes it a COMMON symbol (C, ELF;
             * checked after the arguments), which embld merges. */
            g_fcommon = argv[i][2] == 'c';
        } else if (strcmp(argv[i], "-fno-jump-tables") == 0 ||
                   strcmp(argv[i], "-fjump-tables") == 0) {
            /* A PROMISE, kept: no switch is lowered through a table of
             * addresses. Code that runs before it is relocated, or from
             * an address other than its link address, cannot index one.
             * The decision is irgen's alone (switch_dense), so every
             * backend -- each lowers only the IR_SWITCH irgen made --
             * keeps it; a dense switch becomes the compare tree. */
            target_set_jump_tables(argv[i][2] == 'j');
        } else if (strcmp(argv[i], "-fno-inline-functions") == 0 ||
                   strcmp(argv[i], "-finline-functions") == 0) {
            /* GCC's "consider every function for inlining, not only
             * those declared inline": the -O2 default here too. The
             * negative is honoured -- the inliner then takes only
             * functions declared `inline` (or always_inline), at any
             * level -- because a build asking for it usually wants
             * each function it wrote to stay a function it can find
             * in the image. */
            opt_set_inline_declared_only(argv[i][2] == 'n');
        } else if (strcmp(argv[i], "-fsingle-precision-constant") == 0 ||
                   strcmp(argv[i], "-fno-single-precision-constant") == 0) {
            /* Changes TYPES -- sizeof(1.0) is 4, 0.1 is 0.1f -- so it
             * is implemented (src/parse/parse.c), not nodded at. */
            g_single_prec = argv[i][2] == 's';
        } else if (strcmp(argv[i], "-finline-small-functions") == 0 ||
                   strcmp(argv[i], "-fno-inline-small-functions") == 0 ||
                   strncmp(argv[i], "-finline-limit=", 15) == 0 ||
                   strcmp(argv[i], "-fno-short-enums") == 0 ||
                   strcmp(argv[i], "-fno-math-errno") == 0 ||
                   strcmp(argv[i], "-ffast-math") == 0 ||
                   strcmp(argv[i], "-funsafe-math-optimizations") == 0 ||
                   strcmp(argv[i], "-fno-signed-zeros") == 0 ||
                   strcmp(argv[i], "-fno-trapping-math") == 0 ||
                   strcmp(argv[i], "-ffinite-math-only") == 0 ||
                   strcmp(argv[i], "-fassociative-math") == 0 ||
                   strcmp(argv[i], "-freciprocal-math") == 0 ||
                   strncmp(argv[i], "-fmessage-length=", 17) == 0 ||
                   strcmp(argv[i], "-fverbose-asm") == 0 ||
                   strcmp(argv[i], "-pipe") == 0 ||
                   strcmp(argv[i], "-fno-pic") == 0 ||
                   strcmp(argv[i], "-fno-PIC") == 0 ||
                   strcmp(argv[i], "-fno-pie") == 0 ||
                   strcmp(argv[i], "-fno-PIE") == 0 ||
                   strcmp(argv[i], "-fmerge-constants") == 0 ||
                   strcmp(argv[i], "-fno-strict-overflow") == 0 ||
                   strcmp(argv[i], "-fno-delete-null-pointer-checks") == 0 ||
                   strcmp(argv[i], "-fno-tree-loop-distribute-patterns") == 0 ||
                   strcmp(argv[i], "-fno-zero-initialized-in-bss") == 0 ||
                   strcmp(argv[i], "-fzero-initialized-in-bss") == 0 ||
                   strcmp(argv[i], "-fstrict-volatile-bitfields") == 0 ||
                   strcmp(argv[i], "-fno-strict-volatile-bitfields") == 0 ||
                   strncmp(argv[i], "-fno-builtin-", 13) == 0 ||
                   strcmp(argv[i], "-fno-isolate-erroneous-paths-"
                                   "dereference") == 0 ||
                   strcmp(argv[i], "-fno-move-loop-invariants") == 0 ||
                   strcmp(argv[i], "-fno-ipa-sra") == 0 ||
                   strcmp(argv[i], "-fno-lto") == 0 ||
                   (strcmp(argv[i], "-mlittle-endian") == 0 &&
                    !target_big_endian())) {
            /* What arm-none-eabi-gcc builds pass, each accepted for a
             * reason that holds of THIS compiler (docs/manual/invoking.md;
             * tests/golden/gcc-flags.sh checks the promises):
             *
             * Hints, which a compiler may decline: -finline-small-
             * functions and its negative, -finline-limit=, and the
             * optimisation switches -fno-move-loop-invariants,
             * -fno-ipa-sra, -fno-lto (none of those passes exists here
             * under GCC's name).
             *
             * Permissions, which are kept by not using them:
             * -fno-math-errno, -ffast-math (no __FAST_MATH__: nothing
             * here relaxes IEEE arithmetic, and a header testing for it
             * takes the careful path) and the permissions it is made of
             * (-funsafe-math-optimizations, -fno-signed-zeros,
             * -fno-trapping-math, -ffinite-math-only, -fassociative-math,
             * -freciprocal-math), -fmerge-constants,
             * -fzero-initialized-in-bss, -fno-strict-volatile-bitfields.
             *
             * Formatting and plumbing that change no byte of the object:
             * -fmessage-length=, -fverbose-asm, -pipe.
             *
             * Promises EmbCC already keeps -- each was checked:
             *   -fno-short-enums      an enum is int-sized on every target
             *   -fno-pic/-fno-pie     the code is not position
             *                         independent (-fPIC is refused): an
             *                         x86-64 jump table holds absolute
             *                         addresses, a Cortex-M address is a
             *                         movw/movt pair, and embld links no
             *                         PIE. (Darwin's code is PC-relative
             *                         because Mach-O requires it, and
             *                         links the same either way.)
             *   -fno-strict-overflow  -fwrapv's rule, which always holds
             *   -fno-delete-null-pointer-checks  no pass infers that a
             *                         pointer is non-null -- not from a
             *                         dereference, not from nonnull
             *                         (an ignored attribute), not from
             *                         being an object's address -- so no
             *                         check is ever deleted, and a load
             *                         from address 0 stays a load
             *   -fno-tree-loop-distribute-patterns  no loop becomes a
             *                         CALL: the idiom pass makes a copy or
             *                         clear loop an IR_MEMCPY/IR_MEMZERO,
             *                         and every backend expands those
             *                         inline, as it does struct copies,
             *                         so a hand-written memset cannot turn
             *                         into a call to itself
             *   -fno-zero-initialized-in-bss  `int x = 0;` is already in
             *                         .data; only an object with no
             *                         initializer is .bss
             *   -fstrict-volatile-bitfields  a volatile bit-field is read
             *                         and written with one access of its
             *                         declared type's width (AAPCS)
             *   -fno-builtin-NAME     no library name is a builtin at all
             *   -fno-isolate-erroneous-paths-dereference  nothing turns
             *                         a null dereference into a trap
             *   -mlittle-endian       every target EmbCC has is, but
             *                         mips-none-elf (refused below) */
        } else if (strncmp(argv[i], "-specs=", 7) == 0 ||
                   strncmp(argv[i], "--specs=", 8) == 0) {
            /* nano.specs, nosys.specs: which newlib and which syscall
             * stubs the GCC driver links. EmbCC links its own libc and
             * runtime, so there is nothing to select; the link says so
             * once (compile_and_link) rather than every compile. */
            g_specs = strchr(argv[i], '=') + 1;
        } else if (strcmp(argv[i], "-fanalyzer") == 0) {
            fprintf(stderr, "embcc: warning: -fanalyzer: EmbCC has no "
                            "static analyzer, so this checks nothing\n");
        } else if (strcmp(argv[i], "-save-temps") == 0 ||
                   strcmp(argv[i], "-save-temps=obj") == 0 ||
                   strcmp(argv[i], "--save-temps") == 0) {
            g_save_temps = 1;
        } else if (strcmp(argv[i], "-save-temps=cwd") == 0) {
            g_save_temps = 2;
        } else if (strcmp(argv[i], "-dumpbase") == 0) {
            if (i + 1 == argc) {
                fprintf(stderr, "embcc: -dumpbase needs a name\n");
                return 1;
            }
            g_dumpbase = argv[++i];
        } else if (strcmp(argv[i], "-fcallgraph-info") == 0 ||
                   strcmp(argv[i], "-fcallgraph-info=su") == 0) {
            want_callgraph = 1;
            callgraph_su = argv[i][16] == '=';
        } else if (strncmp(argv[i], "-fdump-", 7) == 0 ||
                   strncmp(argv[i], "-fcallgraph-info", 16) == 0) {
            /* GCC's own internals -- its RTL, its trees, its call graph
             * as cc1 sees it -- which have no counterpart to write. A
             * build that asked for one would look for a file that never
             * appears, so it is told now. */
            fprintf(stderr, "embcc: error: %s is not supported: it dumps "
                            "GCC's internal representation, which EmbCC "
                            "does not have; `embcc inspect ir|cfg|callgraph` "
                            "shows EmbCC's, and -fstack-usage its frames\n",
                    argv[i]);
            return 1;
        } else if ((strncmp(argv[i], "-mabi=", 6) == 0 ||
                    strncmp(argv[i], "-march=", 7) == 0) &&
                   (target_get() == TARGET_RISCV32 ||
                    target_get() == TARGET_RISCV64)) {
            /* RECORDED, and resolved once every argument has been read
             * (riscv_float_resolve): -mabi=ilp32f -march=rv32imafc and
             * the other order mean the same thing. */
            if (argv[i][3] == 'b')
                g_rv_mabi = argv[i] + 6;
            else
                g_rv_march = argv[i] + 7;
        } else if (strcmp(argv[i], "-mbig-endian") == 0 ||
                   strcmp(argv[i], "-mlittle-endian") == 0) {
            /* The byte order is the target's: it says what it is, and a
             * flag that says the same is accepted. One that contradicts it
             * is refused rather than obeyed, because the triple decides
             * more than the order (the runtime's directory, the object's
             * name for itself) and a silent switch would leave them
             * disagreeing. */
            int be = argv[i][2] == 'b';
            if (be != target_big_endian()) {
                fprintf(stderr, "embcc: error: %s is not supported for %s, "
                                "which is %s-endian%s\n", argv[i],
                        target_triple_now(),
                        target_big_endian() ? "big" : "little",
                        target_get() == TARGET_MIPS32
                            ? (be ? " (big-endian MIPS is "
                                    "--target=mips-none-elf)"
                                  : " (little-endian MIPS is "
                                    "--target=mipsel-none-elf)")
                        : target_get() == TARGET_MIPS64
                            ? (be ? " (big-endian MIPS64 is "
                                    "--target=mips64-none-elf)"
                                  : " (little-endian MIPS64 is "
                                    "--target=mips64el-none-elf)")
                            : "");
                return 1;
            }
        } else if (strncmp(argv[i], "-fgnuc-version=", 15) == 0) {
            /* clang's spelling: present a C unit as this GCC, so a vendor
             * header that picks its compiler support by __GNUC__ (CMSIS)
             * takes the GNU one. 0 leaves it undefined, the default. */
            int v[3] = { 0, 0, 0 }, k = 0;
            const char *q = argv[i] + 15;
            char *e;
            for (; k < 3; k++) {
                long n = strtol(q, &e, 10);
                if (e == q || n < 0 || n > 999) break;
                v[k] = (int)n;
                q = e;
                if (*q != '.') { k++; break; }
                q++;
            }
            if (k == 0 || *q) {
                fprintf(stderr, "embcc: error: -fgnuc-version wants "
                                "MAJOR[.MINOR[.PATCH]], not '%s'\n",
                        argv[i] + 15);
                return 1;
            }
            cpp_set_gnuc_version(v[0], v[1], v[2]);
        } else if (strcmp(argv[i], "-ffunction-sections") == 0) {
            func_sections = 1;
        } else if (strcmp(argv[i], "-fno-function-sections") == 0) {
            func_sections = 0;
        } else if (strcmp(argv[i], "-fdata-sections") == 0) {
            data_sections = 1;
        } else if (strcmp(argv[i], "-fno-data-sections") == 0) {
            data_sections = 0;
        } else if (strcmp(argv[i], "-fgnu89-inline") == 0 ||
                   strcmp(argv[i], "-fno-gnu89-inline") == 0) {
            gnu89_inline = argv[i][2] != 'n';
        } else if (strncmp(argv[i], "-fno-", 5) == 0 &&
                   opt_set_pass(argv[i] + 5, 0)) {
            /* a named pass, off */
        } else if (strncmp(argv[i], "-f", 2) == 0 && argv[i][2] &&
                   opt_set_pass(argv[i] + 2, 1)) {
            /* a named pass, on -- so a single pass can be tried at -O1 */
        } else if (strcmp(argv[i], "--dump-predef") == 0 ||
                   strcmp(argv[i], "-dumpmachine") == 0) {
            /* answered after every argument has been applied */
        } else if (target_get() == TARGET_LOONGARCH64 &&
                   (strncmp(argv[i], "-march=", 7) == 0 ||
                    strncmp(argv[i], "-mtune=", 7) == 0 ||
                    strncmp(argv[i], "-mabi=", 6) == 0 ||
                    strncmp(argv[i], "-mfpu=", 6) == 0 ||
                    strncmp(argv[i], "-mcmodel=", 9) == 0 ||
                    strcmp(argv[i], "-msoft-float") == 0 ||
                    strcmp(argv[i], "-msingle-float") == 0 ||
                    strcmp(argv[i], "-mdouble-float") == 0 ||
                    strcmp(argv[i], "-mrelax") == 0 ||
                    strcmp(argv[i], "-mno-relax") == 0 ||
                    strcmp(argv[i], "-mstrict-align") == 0 ||
                    strcmp(argv[i], "-mno-strict-align") == 0 ||
                    strcmp(argv[i], "-mlsx") == 0 ||
                    strcmp(argv[i], "-mno-lsx") == 0 ||
                    strcmp(argv[i], "-mlasx") == 0 ||
                    strcmp(argv[i], "-mno-lasx") == 0)) {
            /* The flags a LoongArch build passes (clang's and gcc's for
             * loongarch64 bare metal). What EmbCC emits is ONE
             * configuration -- the LA64 base integer ISA, the LP64S
             * soft-float convention, the normal code model -- so each flag
             * either says that (or something it is a valid part of) and is
             * accepted, or asks for something else and is refused by name:
             * an object built for the FPU convention would link and then
             * disagree with every caller about where a double travels. */
            const char *v = strchr(argv[i], '=');
            v = v ? v + 1 : "";
            if (strncmp(argv[i], "-march=", 7) == 0) {
                /* the base ISA runs on every one of these */
                static const char *const archs[] = {
                    "loongarch64", "la64v1.0", "la64v1.1", "la464", "la664"
                };
                int ok = 0;
                for (unsigned k = 0; k < sizeof archs / sizeof archs[0]; k++)
                    ok |= strcmp(v, archs[k]) == 0;
                if (!ok)
                    diag_fatal(NULL, 0, "%s is not an LA64 architecture: "
                               "EmbCC emits the LA64 base integer ISA "
                               "(loongarch64, la64v1.0, la64v1.1, la464, "
                               "la664)", argv[i]);
            } else if (strncmp(argv[i], "-mabi=", 6) == 0) {
                if (strcmp(v, "lp64s") != 0)
                    diag_fatal(NULL, 0, "%s is not supported: EmbCC emits "
                               "the soft-float LP64S convention "
                               "(-mabi=lp64s), which passes floating point "
                               "in the integer registers", argv[i]);
            } else if (strncmp(argv[i], "-mfpu=", 6) == 0) {
                if (strcmp(v, "none") != 0 && strcmp(v, "0") != 0)
                    diag_fatal(NULL, 0, "%s is not supported: EmbCC's "
                               "LoongArch code uses no FPU (-mfpu=none)",
                               argv[i]);
            } else if (strncmp(argv[i], "-mcmodel=", 9) == 0) {
                /* normal: bl and pcalau12i reach +-128 MiB and +-2 GiB;
                 * a medium program fits in that too, and one that does
                 * not is refused by the linker, never mislinked */
                if (strcmp(v, "normal") != 0 && strcmp(v, "medium") != 0)
                    diag_fatal(NULL, 0, "%s is not supported: EmbCC emits "
                               "the normal code model (bl, pcalau12i + "
                               "addi.d)", argv[i]);
            } else if (strcmp(argv[i], "-msingle-float") == 0 ||
                       strcmp(argv[i], "-mdouble-float") == 0) {
                diag_fatal(NULL, 0, "%s is not supported: EmbCC emits "
                           "soft-float LP64S code (-msoft-float)", argv[i]);
            } else if (strcmp(argv[i], "-mstrict-align") == 0) {
                diag_fatal(NULL, 0, "-mstrict-align is not supported: "
                           "EmbCC's LoongArch code may access a packed "
                           "member unaligned, as LA64 permits");
            } else if (strcmp(argv[i], "-mlsx") == 0 ||
                       strcmp(argv[i], "-mlasx") == 0) {
                diag_fatal(NULL, 0, "%s is not supported: EmbCC emits no "
                           "LSX or LASX vector instructions", argv[i]);
            }
            continue;
        } else if (target_get() == TARGET_XTENSA && xtensa_flag(argv[i])) {
            /* The flags an ESP-IDF build passes, and the rest of GCC's
             * xtensa.opt. What EmbCC emits is ONE configuration -- the
             * windowed ABI, little-endian, literals in .text before each
             * function, direct call8s, memw before every volatile access
             * -- so a flag that says that (or only changes how GCC would
             * have placed or costed the same code) is accepted, and one
             * that asks for anything else is refused by name. */
            static const char *const ok[] = {
                "-mlongcalls", "-mno-longcalls", "-mtext-section-literals",
                "-mno-text-section-literals", "-mauto-litpools",
                "-mno-auto-litpools", "-mserialize-volatile",
                "-mno-serialize-volatile", "-mtarget-align",
                "-mno-target-align", "-mforce-no-pic", "-mabi=windowed",
                "-mlittle-endian", "-mstrict-align", "-mno-strict-align",
                "-mlra", "-mno-lra", "-mno-fix-esp32-psram-cache-issue",
                "-mno-const16"
            };
            int good = 0;
            for (unsigned k = 0; k < sizeof ok / sizeof ok[0]; k++)
                good |= strcmp(argv[i], ok[k]) == 0;
            if (!strncmp(argv[i], "-mextra-l32r-costs=", 19))
                good = 1;           /* GCC's cost model alone */
            if (!strncmp(argv[i], "-mdynconfig=", 12) &&
                (strstr(argv[i], "esp32.so") || strstr(argv[i], "esp32s3.so")))
                good = 1;
            if (!good) {
                if (!strncmp(argv[i], "-mabi=", 6) &&
                    strcmp(argv[i], "-mabi=call0"))
                    diag_fatal(NULL, 0, "%s is not an Xtensa ABI: EmbCC "
                               "emits the windowed ABI (-mabi=windowed)",
                               argv[i]);
                if (!strcmp(argv[i], "-mabi=call0"))
                    diag_fatal(NULL, 0, "-mabi=call0 is not supported: EmbCC "
                               "emits the windowed ABI (call8/entry/retw), "
                               "which ESP-IDF uses");
                if (!strcmp(argv[i], "-mbig-endian"))
                    diag_fatal(NULL, 0, "-mbig-endian is not supported: the "
                               "Xtensa target is little-endian only");
                if (!strncmp(argv[i], "-mfix-esp32-psram-cache-issue", 29))
                    diag_fatal(NULL, 0, "%s is not supported: EmbCC does not "
                               "insert the ESP32 rev. 1 PSRAM workaround",
                               argv[i]);
                diag_fatal(NULL, 0, "%s is not supported for xtensa-none-elf: "
                           "EmbCC emits the windowed ABI for the ESP32 "
                           "(LX6) and ESP32-S3 (LX7), little-endian, with "
                           "literals before each function", argv[i]);
            }
            continue;
        } else if (target_get() == TARGET_PPC32 &&
                   (strncmp(argv[i], "-mcpu=", 6) == 0 ||
                    strcmp(argv[i], "-msoft-float") == 0 ||
                    strcmp(argv[i], "-mhard-float") == 0 ||
                    strcmp(argv[i], "-mspe") == 0 ||
                    strcmp(argv[i], "-mno-spe") == 0 ||
                    strcmp(argv[i], "-mvle") == 0 ||
                    strcmp(argv[i], "-mno-vle") == 0 ||
                    strcmp(argv[i], "-meabi") == 0 ||
                    strcmp(argv[i], "-mno-eabi") == 0 ||
                    strcmp(argv[i], "-mlong-double-64") == 0 ||
                    strcmp(argv[i], "-mlong-double-128") == 0 ||
                    strcmp(argv[i], "-mno-isel") == 0 ||
                    strcmp(argv[i], "-misel") == 0 ||
                    strcmp(argv[i], "-msecure-plt") == 0 ||
                    strcmp(argv[i], "-mno-sdata") == 0 ||
                    strncmp(argv[i], "-msdata", 7) == 0 ||
                    strncmp(argv[i], "-mfloat-abi=", 12) == 0 ||
                    strncmp(argv[i], "-mabi=", 6) == 0 ||
                    strncmp(argv[i], "-G", 2) == 0)) {
            /* The flags an e500/e200 build passes (gcc's powerpc-eabi and
             * clang's). What EmbCC emits is ONE configuration -- 32-bit
             * Book E PowerPC, the EABI, soft float, no SPE or VLE, a
             * 64-bit long double, no small data -- so each flag either
             * says that and is accepted, or asks for something else and
             * is refused by name: an object built for another would link
             * and then disagree with its callers about where a double is,
             * how wide a long double is, or what r2 and r13 hold. */
            const char *v = strchr(argv[i], '=');
            v = v ? v + 1 : "";
            if (strncmp(argv[i], "-mcpu=", 6) == 0) {
                static const char *const cores[] = {
                    "e500", "8548", "e500v1", "e500v2", "8540", "e200",
                    "e200z0", "e200z2", "e200z3", "e200z4", "e200z6",
                    "e200z7", "ppc", "powerpc", "ppc32", "generic",
                    "603", "603e", "e300c2", "e300c3", "750", "7400", "440"
                };
                int ok = 0;
                for (unsigned k = 0; k < sizeof cores / sizeof cores[0]; k++)
                    ok |= strcmp(v, cores[k]) == 0;
                if (!ok)
                    diag_fatal(NULL, 0, "%s is not a 32-bit PowerPC core "
                               "EmbCC emits for: its code is 32-bit Book E "
                               "PowerPC without SPE (e500, 8548, e500v1, "
                               "e500v2, e200z0-z7, ppc, 603, 750, 440, ...)",
                               argv[i]);
            } else if (strcmp(argv[i], "-mhard-float") == 0 ||
                       (strncmp(argv[i], "-mfloat-abi=", 12) == 0 &&
                        strcmp(v, "soft") != 0)) {
                diag_fatal(NULL, 0, "%s is not supported: EmbCC emits "
                           "soft-float PowerPC code, which passes floating "
                           "point in the general registers", argv[i]);
            } else if (strcmp(argv[i], "-mspe") == 0) {
                diag_fatal(NULL, 0, "-mspe is not supported: EmbCC emits no "
                           "SPE (signal processing engine) instructions; its "
                           "e500 code is soft float (-mno-spe)");
            } else if (strcmp(argv[i], "-mvle") == 0) {
                diag_fatal(NULL, 0, "-mvle is not supported: EmbCC emits "
                           "32-bit Book E instructions, not VLE");
            } else if (strcmp(argv[i], "-mlong-double-128") == 0) {
                diag_fatal(NULL, 0, "-mlong-double-128 is not supported: "
                           "EmbCC's PowerPC long double is the 8-byte double "
                           "(-mlong-double-64)");
            } else if (strcmp(argv[i], "-mno-eabi") == 0 ||
                       strcmp(argv[i], "-msecure-plt") == 0) {
                diag_fatal(NULL, 0, "%s is not supported: EmbCC emits the "
                           "embedded ABI (-meabi) for bare metal", argv[i]);
            } else if (strncmp(argv[i], "-mabi=", 6) == 0 &&
                       strcmp(v, "ibmlongdouble") != 0 &&
                       strcmp(v, "no-spe") != 0) {
                diag_fatal(NULL, 0, "%s is not supported: EmbCC emits the "
                           "PowerPC EABI with soft float", argv[i]);
            } else if ((strncmp(argv[i], "-msdata", 7) == 0 &&
                        strcmp(argv[i], "-msdata=none") != 0) ||
                       (strncmp(argv[i], "-G", 2) == 0 &&
                        strcmp(argv[i], "-G0") != 0)) {
                diag_fatal(NULL, 0, "%s is not supported: EmbCC puts no "
                           "data in small-data sections and addresses "
                           "nothing through r2 or r13 (-msdata=none, -G0)",
                           argv[i]);
            }
            continue;
        } else if (target_get() == TARGET_RX &&
                   (strncmp(argv[i], "-mcpu=", 6) == 0 ||
                    strncmp(argv[i], "-m32bit-doubles", 15) == 0 ||
                    strcmp(argv[i], "-m64bit-doubles") == 0 ||
                    strcmp(argv[i], "-nofpu") == 0 ||
                    strcmp(argv[i], "-mnofpu") == 0 ||
                    strcmp(argv[i], "-fpu") == 0 ||
                    strcmp(argv[i], "-mlittle-endian-data") == 0 ||
                    strcmp(argv[i], "-mbig-endian-data") == 0 ||
                    strcmp(argv[i], "-mrx-abi") == 0 ||
                    strcmp(argv[i], "-mgcc-abi") == 0 ||
                    strncmp(argv[i], "-msmall-data-limit=", 19) == 0 ||
                    strcmp(argv[i], "-mpid") == 0 ||
                    strcmp(argv[i], "-mno-pid") == 0 ||
                    strncmp(argv[i], "-mint-register=", 15) == 0 ||
                    strncmp(argv[i], "-mmax-constant-size=", 20) == 0 ||
                    strcmp(argv[i], "-mallow-string-insns") == 0 ||
                    strcmp(argv[i], "-mno-allow-string-insns") == 0 ||
                    strcmp(argv[i], "-mas100-syntax") == 0 ||
                    strcmp(argv[i], "-mrelax") == 0)) {
            /* The flags an RX build passes (GCC's rx-elf ones). EmbCC
             * emits ONE configuration -- RXv1, little-endian data, GCC's
             * RX ABI with 32-bit doubles, no FPU instructions -- so each
             * flag either says that and is accepted, or asks for another
             * and is refused by name: an object built otherwise would
             * link and then disagree with its callers about doubles, byte
             * order or a reserved register. */
            const char *a = argv[i];
            if (strncmp(a, "-mcpu=", 6) == 0) {
                const char *v = a + 6;
                if (strcmp(v, "rx600") && strcmp(v, "rx610") &&
                    strcmp(v, "rx200") && strcmp(v, "rx100") &&
                    strcmp(v, "RX600") && strcmp(v, "RX610") &&
                    strcmp(v, "RX200") && strcmp(v, "RX100"))
                    diag_fatal(NULL, 0, "%s is not an RXv1 core: EmbCC "
                               "emits the RXv1 instruction set (rx600, "
                               "rx610, rx200, rx100)", a);
            } else if (!strcmp(a, "-m64bit-doubles")) {
                diag_fatal(NULL, 0, "-m64bit-doubles is not supported: EmbCC "
                           "emits GCC's rx-elf default, -m32bit-doubles "
                           "(double is binary32), and a 64-bit double "
                           "changes the ABI of every double");
            } else if (!strcmp(a, "-fpu")) {
                diag_fatal(NULL, 0, "-fpu is not supported: EmbCC's RX code "
                           "is soft float (-nofpu); the RX600 FPU "
                           "instructions are not emitted yet");
            } else if (!strcmp(a, "-mbig-endian-data")) {
                diag_fatal(NULL, 0, "-mbig-endian-data is not supported: the "
                           "RX target is little-endian only");
            } else if (!strcmp(a, "-mgcc-abi")) {
                diag_fatal(NULL, 0, "-mgcc-abi is not supported: EmbCC "
                           "passes stacked arguments naturally aligned, "
                           "GCC's default -mrx-abi");
            } else if (strncmp(a, "-msmall-data-limit=", 19) == 0 &&
                       strcmp(a + 19, "0") != 0) {
                diag_fatal(NULL, 0, "%s is not supported: EmbCC puts no data "
                           "in a small-data area addressed from a base "
                           "register", a);
            } else if (!strcmp(a, "-mpid")) {
                diag_fatal(NULL, 0, "-mpid is not supported: EmbCC's RX code "
                           "addresses data absolutely, not position-"
                           "independently");
            } else if (strncmp(a, "-mint-register=", 15) == 0 &&
                       strcmp(a + 15, "0") != 0) {
                diag_fatal(NULL, 0, "%s is not supported: EmbCC reserves no "
                           "registers for interrupt handlers", a);
            } else if (!strcmp(a, "-mno-allow-string-insns")) {
                diag_fatal(NULL, 0, "-mno-allow-string-insns is not "
                           "supported: EmbCC copies large blocks with smovf "
                           "and sstr");
            } else if (!strcmp(a, "-mas100-syntax")) {
                diag_fatal(NULL, 0, "-mas100-syntax is not supported: EmbCC "
                           "writes objects, not Renesas AS100 assembly");
            }
            continue;
        } else if (target_get() == TARGET_SPARC32 &&
                   (strncmp(argv[i], "-mcpu=", 6) == 0 ||
                    strncmp(argv[i], "-march=", 7) == 0 ||
                    strncmp(argv[i], "-mtune=", 7) == 0 ||
                    strcmp(argv[i], "-msoft-float") == 0 ||
                    strcmp(argv[i], "-mhard-float") == 0 ||
                    strcmp(argv[i], "-mfpu") == 0 ||
                    strcmp(argv[i], "-mno-fpu") == 0 ||
                    strcmp(argv[i], "-mflat") == 0 ||
                    strcmp(argv[i], "-mno-flat") == 0 ||
                    strcmp(argv[i], "-mv8") == 0 ||
                    strcmp(argv[i], "-mapp-regs") == 0 ||
                    strcmp(argv[i], "-mno-app-regs") == 0 ||
                    strcmp(argv[i], "-mfix-gr712rc") == 0 ||
                    strcmp(argv[i], "-mfix-ut699") == 0 ||
                    strcmp(argv[i], "-mfix-ut700") == 0 ||
                    strncmp(argv[i], "-mcmodel=", 9) == 0 ||
                    strcmp(argv[i], "-m32") == 0 ||
                    strcmp(argv[i], "-m64") == 0)) {
            /* The flags a LEON3 build passes (BCC's and clang's for
             * sparc bare metal). What EmbCC emits is ONE configuration --
             * SPARC V8 with LEON3's multiply and divide, register windows,
             * soft float, %g2-%g4 used as scratch -- so each flag either
             * says exactly that and is accepted, or asks for something else
             * and is refused by name: an object built for another would
             * link and then disagree with its callers about where a double
             * is or which registers survive a call. */
            const char *v = strchr(argv[i], '=');
            v = v ? v + 1 : "";
            if (strncmp(argv[i], "-mcpu=", 6) == 0 ||
                strncmp(argv[i], "-march=", 7) == 0 ||
                strncmp(argv[i], "-mtune=", 7) == 0) {
                static const char *const cores[] = {
                    "leon3", "v8", "leon4", "gr712rc", "gr740", "ut699",
                    "sparcleon3", "leon3v7"
                };
                int ok = 0;
                for (unsigned k = 0; k < sizeof cores / sizeof cores[0]; k++)
                    ok |= strcmp(v, cores[k]) == 0;
                if (!ok)
                    diag_fatal(NULL, 0, "%s is not a SPARC V8 core with "
                               "hardware multiply and divide: EmbCC emits "
                               "LEON3 code (leon3, leon4, v8, gr712rc, "
                               "gr740, ut699)", argv[i]);
            } else if (strcmp(argv[i], "-mhard-float") == 0 ||
                       strcmp(argv[i], "-mfpu") == 0) {
                diag_fatal(NULL, 0, "%s is not supported: EmbCC emits "
                           "soft-float SPARC code, which passes floating "
                           "point in the integer registers", argv[i]);
            } else if (strcmp(argv[i], "-mflat") == 0) {
                diag_fatal(NULL, 0, "-mflat is not supported: EmbCC's "
                           "SPARC code uses register windows (save and "
                           "restore)");
            } else if (strcmp(argv[i], "-mno-app-regs") == 0) {
                diag_fatal(NULL, 0, "-mno-app-regs is not supported: "
                           "EmbCC's SPARC code uses %%g2-%%g4 as scratch "
                           "registers (-mapp-regs)");
            } else if (strcmp(argv[i], "-m64") == 0 ||
                       (strncmp(argv[i], "-mcmodel=", 9) == 0 &&
                        strcmp(v, "medlow") != 0)) {
                diag_fatal(NULL, 0, "%s is not supported: EmbCC emits "
                           "32-bit SPARC V8 with absolute addresses "
                           "(sethi/or)", argv[i]);
            } else if (strncmp(argv[i], "-mfix-", 6) == 0) {
                diag_fatal(NULL, 0, "%s is not supported: EmbCC applies "
                           "no LEON errata workarounds", argv[i]);
            }
            continue;
        } else if (target_get() == TARGET_COLDFIRE &&
                   (strncmp(argv[i], "-mcpu=", 6) == 0 ||
                    strncmp(argv[i], "-march=", 7) == 0 ||
                    strncmp(argv[i], "-mtune=", 7) == 0 ||
                    strncmp(argv[i], "-m5", 3) == 0 ||
                    strncmp(argv[i], "-m68", 4) == 0 ||
                    strncmp(argv[i], "-mc68", 5) == 0 ||
                    strncmp(argv[i], "-mcpu32", 7) == 0 ||
                    strncmp(argv[i], "-mcfv", 5) == 0 ||
                    strcmp(argv[i], "-msoft-float") == 0 ||
                    strcmp(argv[i], "-mhard-float") == 0 ||
                    strcmp(argv[i], "-mdiv") == 0 ||
                    strcmp(argv[i], "-mno-div") == 0 ||
                    strcmp(argv[i], "-malign-int") == 0 ||
                    strcmp(argv[i], "-mno-align-int") == 0 ||
                    strcmp(argv[i], "-mshort") == 0 ||
                    strcmp(argv[i], "-mno-short") == 0 ||
                    strcmp(argv[i], "-mstrict-align") == 0 ||
                    strcmp(argv[i], "-mno-strict-align") == 0 ||
                    strcmp(argv[i], "-mpcrel") == 0 ||
                    strcmp(argv[i], "-mid-shared-library") == 0 ||
                    strcmp(argv[i], "-msep-data") == 0 ||
                    strcmp(argv[i], "-mxgot") == 0 ||
                    strcmp(argv[i], "-mrtd") == 0 ||
                    strcmp(argv[i], "-mno-rtd") == 0)) {
            /* The flags a ColdFire build passes (GCC's m68k-elf, which
             * selects ColdFire with -mcpu=). What EmbCC emits is ONE
             * configuration -- ColdFire ISA_A with the hardware divide,
             * soft float, int 32 bits and 2-aligned, the caller popping
             * its arguments, absolute addresses -- so each flag either says
             * that and is accepted, or asks for something else and is
             * refused by name: an object built otherwise would link and
             * then disagree with its callers about where an argument is,
             * how wide an int is or what an instruction means. */
            const char *v = strchr(argv[i], '=');
            v = v ? v + 1 : "";
            if (strncmp(argv[i], "-mcpu=", 6) == 0 ||
                strncmp(argv[i], "-mtune=", 7) == 0 ||
                strncmp(argv[i], "-m5", 3) == 0) {
                /* the ISA_A-or-later cores with a divider and no FPU */
                static const char *const cores[] = {
                    "5208", "5207", "5206e", "5210a", "5211a", "5211",
                    "5212", "5213", "5214", "5216", "5221x", "52221",
                    "52223", "52230", "52231", "52232", "52233", "52234",
                    "52235", "5224", "5225", "52252", "52254", "52255",
                    "52256", "52258", "52259", "52274", "52277", "5232",
                    "5233", "5234", "5235", "523x", "5249", "5250", "5253",
                    "5270", "5271", "5272", "5274", "5275", "5280", "5281",
                    "5282", "528x", "5307", "5327", "5328", "5329", "532x",
                    "5372", "5373", "537x", "5407", "54410", "54415",
                    "54416", "54417", "54418", "54450", "54451", "54452",
                    "54453", "54454", "54455"
                };
                const char *c = strncmp(argv[i], "-m5", 3) == 0
                              ? argv[i] + 2 : v;
                int ok = 0;
                for (unsigned k = 0; k < sizeof cores / sizeof cores[0]; k++)
                    ok |= strcmp(c, cores[k]) == 0;
                if (!ok && (strcmp(c, "5206") == 0 || strcmp(c, "5202") == 0 ||
                            strcmp(c, "5204") == 0))
                    diag_fatal(NULL, 0, "%s is not supported: that core has "
                               "no hardware divide, and EmbCC's ColdFire "
                               "code divides with divs.l/divu.l", argv[i]);
                if (!ok && strncmp(c, "547", 3) != 0 &&
                    strncmp(c, "548", 3) != 0)
                    diag_fatal(NULL, 0, "%s is not a ColdFire core EmbCC "
                               "emits for: its code is ColdFire ISA_A with the "
                               "hardware divide (5208, 5213, 5235, 5282, 5329, "
                               "5407, 54455, ...); the 68000 family proper is "
                               "not a target", argv[i]);
                if (!ok)
                    diag_fatal(NULL, 0, "%s is not supported: that core's "
                               "FPU makes GCC pass and return floating point "
                               "in its registers, and EmbCC's ColdFire code "
                               "is soft float", argv[i]);
            } else if (strncmp(argv[i], "-march=", 7) == 0) {
                if (strcmp(v, "isaa") != 0 && strcmp(v, "isaaplus") != 0 &&
                    strcmp(v, "isab") != 0 && strcmp(v, "isac") != 0)
                    diag_fatal(NULL, 0, "%s is not supported: EmbCC emits "
                               "ColdFire ISA_A (-march=isaa), which isaaplus, "
                               "isab and isac cores also run", argv[i]);
            } else if (strncmp(argv[i], "-m68", 4) == 0 ||
                       strncmp(argv[i], "-mc68", 5) == 0 ||
                       strncmp(argv[i], "-mcpu32", 7) == 0 ||
                       strncmp(argv[i], "-mcfv", 5) == 0) {
                diag_fatal(NULL, 0, "%s is not supported: EmbCC's m68k "
                           "target is ColdFire ISA_A (-mcpu=5208 and its "
                           "kin); the 68000 family proper is not a target",
                           argv[i]);
            } else if (strcmp(argv[i], "-mhard-float") == 0) {
                diag_fatal(NULL, 0, "-mhard-float is not supported: EmbCC "
                           "emits soft-float ColdFire code, which passes "
                           "floating point in the data registers");
            } else if (strcmp(argv[i], "-mno-div") == 0) {
                diag_fatal(NULL, 0, "-mno-div is not supported: EmbCC's "
                           "ColdFire code divides with divs.l/divu.l");
            } else if (strcmp(argv[i], "-malign-int") == 0) {
                diag_fatal(NULL, 0, "-malign-int is not supported: EmbCC "
                           "lays out the m68k's 2-byte alignment, which "
                           "GCC's ColdFire code has without it");
            } else if (strcmp(argv[i], "-mshort") == 0) {
                diag_fatal(NULL, 0, "-mshort is not supported: EmbCC's int "
                           "is 32 bits on the m68k");
            } else if (strcmp(argv[i], "-mrtd") == 0) {
                diag_fatal(NULL, 0, "-mrtd is not supported: EmbCC's caller "
                           "pops its arguments (the SVR4 m68k convention)");
            } else if (strcmp(argv[i], "-mpcrel") == 0 ||
                       strcmp(argv[i], "-mid-shared-library") == 0 ||
                       strcmp(argv[i], "-msep-data") == 0 ||
                       strcmp(argv[i], "-mxgot") == 0) {
                diag_fatal(NULL, 0, "%s is not supported: EmbCC's ColdFire "
                           "code takes every address absolutely", argv[i]);
            }
            continue;
        } else if (target_get() == TARGET_MIPS64 &&
                   (strncmp(argv[i], "-mcpu=", 6) == 0 ||
                    strncmp(argv[i], "-march=", 7) == 0 ||
                    strncmp(argv[i], "-mabi=", 6) == 0 ||
                    strcmp(argv[i], "-msoft-float") == 0 ||
                    strcmp(argv[i], "-mhard-float") == 0 ||
                    strcmp(argv[i], "-mno-abicalls") == 0 ||
                    strcmp(argv[i], "-mabicalls") == 0 ||
                    strcmp(argv[i], "-EL") == 0 || strcmp(argv[i], "-EB") == 0 ||
                    strncmp(argv[i], "-G", 2) == 0)) {
            /* MIPS64's one configuration, as MIPS32's below: MIPS64
             * Release 2, n64, soft float, no abicalls, no small data, in
             * the triple's byte order. */
            const char *v = strchr(argv[i], '=');
            v = v ? v + 1 : "";
            if (strncmp(argv[i], "-mcpu=", 6) == 0 ||
                strncmp(argv[i], "-march=", 7) == 0) {
                static const char *const cores[] = {
                    "mips64r2", "5kc", "5kf", "5kec", "5kef", "octeon"
                };
                int ok = 0;
                for (unsigned k = 0; k < sizeof cores / sizeof cores[0]; k++)
                    ok |= strcmp(v, cores[k]) == 0;
                if (!ok)
                    diag_fatal(NULL, 0, "%s is not a MIPS64 Release 2 core: "
                               "EmbCC emits MIPS64r2 (mips64r2, 5kc, 5kf, "
                               "5kec, 5kef, octeon)", argv[i]);
            } else if (strncmp(argv[i], "-mabi=", 6) == 0) {
                if (strcmp(v, "64") != 0)
                    diag_fatal(NULL, 0, "%s is not supported: EmbCC emits "
                               "the n64 ABI (-mabi=64) only", argv[i]);
            } else if (strcmp(argv[i], "-mhard-float") == 0) {
                diag_fatal(NULL, 0, "-mhard-float is not supported: EmbCC "
                           "emits soft-float n64, which passes floating "
                           "point in the integer registers");
            } else if (strcmp(argv[i], "-mabicalls") == 0) {
                diag_fatal(NULL, 0, "-mabicalls is not supported: EmbCC's "
                           "MIPS64 code takes addresses absolutely "
                           "(%%highest..%%lo) and keeps no $gp; it is "
                           "-mno-abicalls code");
            } else if (strcmp(argv[i], "-EB") == 0 && !target_big_endian()) {
                diag_fatal(NULL, 0, "-EB contradicts --target=%s, which is "
                           "little-endian: big-endian MIPS64 is "
                           "--target=mips64-none-elf", target_triple_now());
            } else if (strcmp(argv[i], "-EL") == 0 && target_big_endian()) {
                diag_fatal(NULL, 0, "-EL contradicts --target=%s, which is "
                           "big-endian: little-endian MIPS64 is "
                           "--target=mips64el-none-elf", target_triple_now());
            } else if (strncmp(argv[i], "-G", 2) == 0 &&
                       strcmp(argv[i], "-G0") != 0) {
                diag_fatal(NULL, 0, "%s is not supported: EmbCC puts no "
                           "data in .sdata and addresses nothing through "
                           "$gp (-G0)", argv[i]);
            }
            continue;
        } else if (target_get() == TARGET_MIPS32 &&
                   (strncmp(argv[i], "-mcpu=", 6) == 0 ||
                    strncmp(argv[i], "-march=", 7) == 0 ||
                    strncmp(argv[i], "-mabi=", 6) == 0 ||
                    strcmp(argv[i], "-msoft-float") == 0 ||
                    strcmp(argv[i], "-mhard-float") == 0 ||
                    strcmp(argv[i], "-mno-abicalls") == 0 ||
                    strcmp(argv[i], "-mabicalls") == 0 ||
                    strcmp(argv[i], "-EL") == 0 || strcmp(argv[i], "-EB") == 0 ||
                    strncmp(argv[i], "-G", 2) == 0)) {
            /* The flags a MIPS build passes (a PIC32 project's, clang's
             * and gcc's for mipsel bare metal). What EmbCC emits is ONE
             * configuration -- MIPS32 Release 2, o32, soft float, no
             * abicalls, no small data, in the triple's byte order (-EL
             * mipsel, -EB mips) -- so each flag either
             * says exactly that and is accepted, or asks for something
             * else and is refused by name: an object built for another
             * of these would link and then disagree with its callers
             * about registers, byte order or the global pointer. */
            const char *v = strchr(argv[i], '=');
            v = v ? v + 1 : "";
            if (strncmp(argv[i], "-mcpu=", 6) == 0 ||
                strncmp(argv[i], "-march=", 7) == 0) {
                static const char *const cores[] = {
                    "mips32r2", "m4k", "m14k", "m14kc", "24kc", "24kf",
                    "24kec", "24kef", "34kc", "74kc"
                };
                int ok = 0;
                for (unsigned k = 0; k < sizeof cores / sizeof cores[0]; k++)
                    ok |= strcmp(v, cores[k]) == 0;
                if (!ok)
                    diag_fatal(NULL, 0, "%s is not a MIPS32 Release 2 core: "
                               "EmbCC emits MIPS32r2 (mips32r2, m4k, m14k, "
                               "m14kc, 24kc, 24kf, 24kec, 24kef, 34kc, 74kc)",
                               argv[i]);
            } else if (strncmp(argv[i], "-mabi=", 6) == 0) {
                if (strcmp(v, "32") != 0)
                    diag_fatal(NULL, 0, "%s is not supported: EmbCC emits "
                               "the o32 ABI (-mabi=32) only", argv[i]);
            } else if (strcmp(argv[i], "-mhard-float") == 0) {
                diag_fatal(NULL, 0, "-mhard-float is not supported: EmbCC "
                           "emits soft-float o32, which passes floating "
                           "point in the integer registers");
            } else if (strcmp(argv[i], "-mabicalls") == 0) {
                diag_fatal(NULL, 0, "-mabicalls is not supported: EmbCC's "
                           "MIPS code takes addresses absolutely (lui/addiu) "
                           "and keeps no $gp; it is -mno-abicalls code");
            } else if (strcmp(argv[i], "-EB") == 0 && !target_big_endian()) {
                diag_fatal(NULL, 0, "-EB contradicts --target=%s, which is "
                           "little-endian: big-endian MIPS is "
                           "--target=mips-none-elf", target_triple_now());
            } else if (strcmp(argv[i], "-EL") == 0 && target_big_endian()) {
                diag_fatal(NULL, 0, "-EL contradicts --target=%s, which is "
                           "big-endian: little-endian MIPS is "
                           "--target=mipsel-none-elf", target_triple_now());
            } else if (strncmp(argv[i], "-G", 2) == 0 &&
                       strcmp(argv[i], "-G0") != 0) {
                diag_fatal(NULL, 0, "%s is not supported: EmbCC puts no "
                           "data in .sdata and addresses nothing through "
                           "$gp (-G0)", argv[i]);
            }
            continue;
        } else if (target_get() == TARGET_TRICORE &&
                   (strncmp(argv[i], "-mcpu=", 6) == 0 ||
                    strncmp(argv[i], "-march=", 7) == 0 ||
                    strncmp(argv[i], "-mtc", 4) == 0 ||
                    strcmp(argv[i], "-msoft-float") == 0 ||
                    strcmp(argv[i], "-mhard-float") == 0 ||
                    strcmp(argv[i], "-mlittle-endian") == 0)) {
            /* The flags a TriCore build passes (HighTec's GCC spells the
             * core -mcpu=tc27xx or -mtc161). What EmbCC emits is ONE
             * configuration -- TriCore 1.6.1 instructions, which every
             * TC2xx and TC3xx core executes, and soft float -- so each
             * flag either names a core that runs it and is accepted, or
             * asks for something else and is refused by name. */
            const char *v = strchr(argv[i], '=');
            v = v ? v + 1 : argv[i] + 2;          /* -mtc161: "tc161" */
            if (strcmp(argv[i], "-mhard-float") == 0)
                diag_fatal(NULL, 0, "-mhard-float is not supported: EmbCC "
                           "emits soft float for TriCore (the TC3xx FPU is "
                           "not used yet)");
            if (strcmp(argv[i], "-msoft-float") && strcmp(argv[i],
                                                         "-mlittle-endian")) {
                static const char *const cores[] = {
                    "tc16", "tc161", "tc162", "tc1.6", "tc1.6.1", "tc1.6.2",
                    "tc16x", "tc2xx", "tc22xx", "tc23xx", "tc26xx", "tc27xx",
                    "tc29xx", "tc3xx", "tc33xx", "tc36xx", "tc37xx",
                    "tc38xx", "tc39xx"
                };
                int ok = 0;
                for (unsigned k = 0; k < sizeof cores / sizeof cores[0]; k++)
                    ok |= strcmp(v, cores[k]) == 0;
                if (!ok)
                    diag_fatal(NULL, 0, "%s is not a TriCore 1.6 core: EmbCC "
                               "emits TriCore 1.6.1 code, for the AURIX "
                               "TC2xx and TC3xx (tc16, tc161, tc162, tc27xx, "
                               "tc37xx, ...)", argv[i]);
            }
            continue;
        } else if (strcmp(argv[i], "-mcmse") == 0) {
            /* The Secure side of ARMv8-M's security extension (ACLE's
             * CMSE): cmse_nonsecure_entry and cmse_nonsecure_call, and
             * __ARM_FEATURE_CMSE 3. Checked against the architecture once
             * every argument is read: -mcpu= may come after it. */
            if (target_get() != TARGET_THUMB)
                diag_fatal(NULL, 0, "-mcmse is an ARMv8-M option, and the "
                           "target is %s", target_triple_now());
            g_arm_cmse = 1;
            target_set_thumb_cmse(1);
            continue;
        } else if (strncmp(argv[i], "-mcpu=", 6) == 0 ||
                   strncmp(argv[i], "-mfpu=", 6) == 0 ||
                   strncmp(argv[i], "-mfloat-abi=", 12) == 0 ||
                   ((strncmp(argv[i], "-march=", 7) == 0 ||
                     strncmp(argv[i], "-mtune=", 7) == 0) &&
                    target_get() == TARGET_THUMB) ||
                   strcmp(argv[i], "-mthumb") == 0 ||
                   strcmp(argv[i], "-marm") == 0 ||
                   strcmp(argv[i], "-mthumb-interwork") == 0 ||
                   strcmp(argv[i], "-mno-thumb-interwork") == 0 ||
                   (strncmp(argv[i], "-mabi=", 6) == 0 &&
                    target_get() == TARGET_THUMB) ||
                   strcmp(argv[i], "-mslow-flash-data") == 0 ||
                   strcmp(argv[i], "-munaligned-access") == 0 ||
                   strcmp(argv[i], "-mno-unaligned-access") == 0) {
            /* The ARM machine flags every Cortex-M build passes. They
             * were "unknown argument" before, which stops a kernel's
             * existing Makefile dead -- and the two that describe the
             * FLOAT ABI are the ones that must not be guessed at,
             * because getting them wrong is an ABI mismatch the linker
             * cannot see (EmbCC emits no .ARM.attributes yet either).
             *
             * -mcpu= selects the sub-architecture, which EmbCC now
             * carries. -mthumb is the only state this backend has, so it
             * is a no-op that has to be accepted. -marm asks for the ARM
             * instruction set, which a Cortex-M does not have at all. */
            const char *v = strchr(argv[i], '=');
            v = v ? v + 1 : NULL;
            if (target_get() != TARGET_THUMB)
                diag_fatal(NULL, 0, "%s is an ARM option, and the target "
                           "is %s", argv[i], target_triple_now());
            /* ARMv7-A in ARM state: -marm is what is emitted, -mthumb
             * would be Thumb-2 on a Cortex-A, which EmbCC does not emit
             * there (the Thumb-2 it emits is the Cortex-M levels'). The
             * rest of the flags are read below as on the Cortex-M levels,
             * with the parts and units this target has. */
            if (target_arm_a32()) {
                if (strcmp(argv[i], "-marm") == 0)
                    continue;
                /* -march=armv7-a (with GCC's +ext spellings, whose units
                 * -mfpu= and arm_float_resolve decide) and -mtune= for a
                 * Cortex-A: scheduling, which EmbCC does not tune */
                if (strncmp(argv[i], "-march=", 7) == 0) {
                    if (strncmp(v, "armv7-a", 7) != 0 &&
                        strncmp(v, "armv7ve", 7) != 0)
                        diag_fatal(NULL, 0, "-march=%s is not supported on "
                                   "%s: EmbCC emits ARMv7-A code there "
                                   "(-march=armv7-a)", v, target_triple_now());
                    continue;
                }
                if (strncmp(argv[i], "-mtune=", 7) == 0) {
                    if (strncmp(v, "cortex-a", 8) != 0 &&
                        strcmp(v, "generic-armv7-a") != 0)
                        diag_fatal(NULL, 0, "-mtune=%s is not a Cortex-A core",
                                   v);
                    continue;
                }
                if (strcmp(argv[i], "-mthumb") == 0)
                    diag_fatal(NULL, 0, "-mthumb is not supported on %s: "
                               "EmbCC emits ARM (A32) code for a Cortex-A; "
                               "Thumb-2 is for the Cortex-M targets "
                               "(thumbv7m-none-eabi and the others)",
                               target_triple_now());
                if (strncmp(argv[i], "-mcpu=", 6) == 0) {
                    /* The ARMv7-A cores. All of them run what is emitted:
                     * no divide instruction is used (the A7, A12, A15 and
                     * A17 have one; the code calls __aeabi_idiv anyway),
                     * and no VFP or NEON. */
                    static const char *const a7cores[] = {
                        "cortex-a5", "cortex-a7", "cortex-a8", "cortex-a9",
                        "cortex-a12", "cortex-a15", "cortex-a17", "generic",
                        NULL
                    };
                    int known = 0;
                    for (int k = 0; a7cores[k]; k++)
                        known |= strcmp(v, a7cores[k]) == 0;
                    if (!known)
                        diag_fatal(NULL, 0, "-mcpu=%s is not supported on %s: "
                                   "EmbCC emits ARMv7-A (cortex-a5, a7, a8, "
                                   "a9, a12, a15, a17) here; a Cortex-M is "
                                   "one of the thumb targets, and an ARMv7-R "
                                   "core's profile is not this one", v,
                                   target_triple_now());
                    g_arm_cpu = v;
                    continue;
                }
            }
            if (strcmp(argv[i], "-marm") == 0)
                diag_fatal(NULL, 0, "-marm is not supported: a Cortex-M "
                           "has no ARM instruction set, only Thumb");
            if (strcmp(argv[i], "-mthumb") == 0)
                continue;          /* the only state there is */
            /* Interworking is between ARM and Thumb code, and a
             * Cortex-M runs only Thumb: every call and return here is
             * already one bx/blx would make (bit 0 set), so both
             * spellings describe what is emitted. */
            if (strcmp(argv[i], "-mthumb-interwork") == 0 ||
                strcmp(argv[i], "-mno-thumb-interwork") == 0)
                continue;
            /* A hint: keep constants out of literal pools in slow flash.
             * The compiled code has none -- constants and addresses are
             * movw/movt -- so there is nothing to move. */
            if (strcmp(argv[i], "-mslow-flash-data") == 0)
                continue;
            /* The procedure call standard. EmbCC's is AAPCS (the base
             * standard, or AAPCS-VFP under -mfloat-abi=hard); aapcs-linux
             * is the same convention with int-sized enums, which is what
             * EmbCC's enums are. The pre-EABI conventions pass and lay
             * out differently, and an object built for one links and
             * then disagrees with its callers. */
            if (strncmp(argv[i], "-mabi=", 6) == 0) {
                if (strcmp(v, "aapcs") && strcmp(v, "aapcs-linux"))
                    diag_fatal(NULL, 0, "-mabi=%s is not supported: EmbCC "
                               "emits the AAPCS (-mabi=aapcs, or "
                               "aapcs-linux, whose int-sized enums are "
                               "EmbCC's too); %s passes arguments and lays "
                               "out data differently", v, v);
                continue;
            }
            /* ARMv7-M and ARMv8-M Mainline load and store a word or a
             * halfword at any address (LDR/STR/LDRH/STRH; never
             * LDRD/STRD/LDM/STM, which this backend keeps to aligned
             * addresses). -munaligned-access says so, and is what is
             * emitted. */
            if (strcmp(argv[i], "-munaligned-access") == 0)
                continue;
            /* -mno-unaligned-access is a promise this backend does not
             * keep: a packed struct's int member is one ldr.w at its
             * odd address, and a struct whose alignment is below four
             * (a packed one, or `struct { char c[5]; }`) is copied, and
             * passed by value, a word at a time from wherever it is.
             * That is ARMv7-M's default and it is fine there; under the
             * flag the same image faults wherever unaligned accesses
             * trap (CCR.UNALIGN_TRP, or Device memory on an M7). */
            if (strcmp(argv[i], "-mno-unaligned-access") == 0)
                diag_fatal(NULL, 0, "-mno-unaligned-access is not "
                           "supported: EmbCC's ARMv7-M code uses word and "
                           "halfword loads and stores at unaligned addresses "
                           "(packed struct members; copies and by-value "
                           "passing of structs aligned below 4), which the "
                           "architecture allows and this flag forbids");
            /* -mtune= picks a core to schedule for, which EmbCC does
             * not do: any Cortex-M part is accepted, and changes nothing. */
            if (strncmp(argv[i], "-mtune=", 7) == 0) {
                static const char *const parts[] = {
                    "cortex-m0", "cortex-m0plus", "cortex-m1", "cortex-m3",
                    "cortex-m4", "cortex-m7", "cortex-m23", "cortex-m33",
                    "cortex-m35p", "cortex-m55", "cortex-m85",
                    "generic-armv7-m", "generic-armv7e-m", NULL
                };
                int ok = 0;
                for (int k = 0; parts[k]; k++)
                    ok |= !strcmp(v, parts[k]);
                if (!ok)
                    diag_fatal(NULL, 0, "-mtune=%s is not a Cortex-M core", v);
                continue;
            }
            /* -march= selects the level as -mcpu= does, with GCC's
             * extension spellings: +fp, +fp.dp and +nofp name the unit
             * (which -mfpu= overrides), +dsp and +nodsp the DSP set on
             * ARMv8-M Mainline. */
            if (strncmp(argv[i], "-march=", 7) == 0) {
                const char *plus = strchr(v, '+');
                size_t bl = plus ? (size_t)(plus - v) : strlen(v);
                int arch, em = 0, base8 = 0;
                if ((bl == 7 && !strncmp(v, "armv6-m", 7)) ||
                    (bl == 8 && !strncmp(v, "armv6s-m", 8)))
                    arch = 6;
                else if (bl == 7 && !strncmp(v, "armv7-m", 7))
                    arch = 7;
                else if (bl == 8 && !strncmp(v, "armv7e-m", 8))
                    arch = 7, em = 1;
                else if (bl == 12 && !strncmp(v, "armv8-m.base", 12))
                    arch = 6, base8 = 1;
                else if (bl == 12 && !strncmp(v, "armv8-m.main", 12))
                    arch = 8;
                else
                    diag_fatal(NULL, 0, "-march=%.*s is not an architecture "
                               "EmbCC emits for a Cortex-M: armv6-m, "
                               "armv6s-m, armv7-m, armv7e-m, armv8-m.base, "
                               "armv8-m.main", (int)bl, v);
                for (const char *x = plus; x && *x; ) {
                    const char *e = strchr(x + 1, '+');
                    size_t n = e ? (size_t)(e - x) : strlen(x);
                    if (n == 3 && !strncmp(x, "+fp", 3) && arch >= 7)
                        g_arm_march_fpu = arch == 8 ? "fpv5-sp-d16"
                                                    : "fpv4-sp-d16";
                    else if (n == 6 && !strncmp(x, "+fp.dp", 6) && arch >= 7)
                        g_arm_march_fpu = "fpv5-d16";
                    else if (n == 5 && !strncmp(x, "+nofp", 5))
                        g_arm_march_fpu = "none";
                    else if (n == 4 && !strncmp(x, "+dsp", 4) && arch == 8)
                        em = 1;
                    else if (n == 6 && !strncmp(x, "+nodsp", 6) && arch == 8)
                        em = 0;
                    else
                        diag_fatal(NULL, 0, "-march=%s: the extension '%.*s' "
                                   "is not one EmbCC emits for that "
                                   "architecture (+fp, +fp.dp, +nofp, and +dsp "
                                   "or +nodsp on armv8-m.main)", v, (int)n, x);
                    x = e;
                }
                if (base8) {
                    target_set_thumb_v8m_base();
                } else {
                    target_set_thumb_arch(arch);
                    target_set_thumb_em(em);
                }
                continue;
            }
            if (strncmp(argv[i], "-mcpu=", 6) == 0) {
                /* Only the parts whose ISA this backend really emits.
                 * An F part is refused by name rather than accepted and
                 * built soft-float: its ABI passes floats in s0-s15 and
                 * an object built the other way links and then reads its
                 * arguments from the wrong registers. */
                /* The ARMv6-M parts select that level: Thumb-1, which the
                 * backend emits for them (src/arch/thumb/v6m.c). They used
                 * to be taken as ARMv7-M, and the code that came out used
                 * ldr.w and IT blocks -- a HardFault at the first one.
                 *
                 * ARMv8-M Baseline (Cortex-M23) is that selection with
                 * the divides and the exclusives turned on (v6m.c), on
                 * any Thumb triple, as clang takes it. */
                if (!strcmp(v, "cortex-m23")) {
                    target_set_thumb_v8m_base();
                    g_arm_cpu = v;
                    continue;
                }
                if (!strcmp(v, "cortex-m0") || !strcmp(v, "cortex-m0plus") ||
                    !strcmp(v, "cortex-m1")) {
                    target_set_thumb_arch(6);
                    target_set_thumb_em(0);
                    g_arm_cpu = v;
                    continue;
                }
                /* The part has one architecture, whatever the triple
                 * said, as clang takes it: -mcpu=cortex-m33 on a thumbv7em
                 * name is ARMv8-M Mainline (__ARM_ARCH 8), and cortex-m4 on
                 * a thumbv8m.main one is ARMv7E-M. It used to raise only an
                 * ARMv6-M level, and the first case built v7-M code and
                 * macros for an M33. (ARM state's triple keeps its level:
                 * the A-profile encoder is not chosen by a Cortex-M name.) */
                if (!target_arm_a32() &&
                    (!strcmp(v, "cortex-m3") || !strcmp(v, "cortex-m4") ||
                     !strcmp(v, "cortex-m7")))
                    target_set_thumb_arch(7);
                if (!target_arm_a32() && !strcmp(v, "cortex-m33"))
                    target_set_thumb_arch(8);
                if (!strcmp(v, "cortex-m3"))
                    target_set_thumb_em(0);
                else if (!strcmp(v, "cortex-m4") || !strcmp(v, "cortex-m7") ||
                         !strcmp(v, "cortex-m33"))
                    target_set_thumb_em(1);
                else
                    diag_fatal(NULL, 0, "-mcpu=%s is not a part EmbCC knows: "
                               "it emits ARMv6-M (cortex-m0, m0plus, m1), "
                               "ARMv8-M Baseline (cortex-m23), ARMv7-M and "
                               "ARMv7E-M (cortex-m3, m4, m7) and ARMv8-M "
                               "Mainline (cortex-m33)", v);
                /* Kept for arm_float_resolve: which unit the part's
                 * -eabihf name implies depends on which part it is. */
                g_arm_cpu = v;
                continue;
            }
            /* The FPU and the float ABI are RECORDED here and resolved
             * after every argument has been read (see arm_float_resolve):
             * `-mfloat-abi=softfp -mfpu=fpv4-sp-d16` and the other order
             * mean the same thing, and neither flag decides anything alone. */
            if (strncmp(argv[i], "-mfpu=", 6) == 0) {
                g_arm_fpu = v;
                continue;
            }
            if (strncmp(argv[i], "-mfloat-abi=", 12) == 0) {
                g_arm_float_abi = v;
                continue;
            }
            continue;
        } else if (strncmp(argv[i], "-fsanitize=", 11) == 0 ||
                   strncmp(argv[i], "-fno-sanitize=", 14) == 0 ||
                   strncmp(argv[i], "-fsanitize-trap", 15) == 0 ||
                   strcmp(argv[i], "-fsanitize-undefined-trap-on-error") == 0) {
            /* TRAP mode, which is the only mode there can be here: a
             * diagnosing sanitizer needs __ubsan_handle_* and a bare
             * metal target has nowhere to print. A failed check runs
             * the target's trap instruction. Under a debugger that is a
             * breakpoint at the offending operation; without one the
             * program stops instead of continuing with a wrong value.
             *
             * -fsanitize-trap= and -fsanitize-undefined-trap-on-error
             * are accepted and mean what they say. The checks EmbCC
             * does not have are refused BY NAME below rather than
             * quietly dropped from the set, because "I asked for
             * address and got nothing" is the failure this whole file
             * exists to prevent. */
            int off = strncmp(argv[i], "-fno-", 5) == 0;
            const char *list = strchr(argv[i], '=');
            if (!list) {           /* -fsanitize-trap / ...-trap-on-error */
                san_trap_asked = 1;
                continue;
            }
            for (const char *p = list + 1; *p; ) {
                const char *e = strchr(p, ',');
                size_t n = e ? (size_t)(e - p) : strlen(p);
                unsigned bit = 0;
                if (n == 9 && !strncmp(p, "undefined", 9))
                    bit = SAN_OVERFLOW | SAN_DIVIDE | SAN_SHIFT;
                else if (n == 23 && !strncmp(p, "signed-integer-overflow", 23))
                    bit = SAN_OVERFLOW;
                else if (n == 22 && !strncmp(p, "integer-divide-by-zero", 22))
                    bit = SAN_DIVIDE;
                else if ((n == 5 && !strncmp(p, "shift", 5)) ||
                         (n == 14 && !strncmp(p, "shift-exponent", 14)))
                    bit = SAN_SHIFT;
                else if (n == 4 && !strncmp(p, "trap", 4))
                    bit = 0;       /* -fsanitize-trap=... names checks */
                else
                    diag_fatal(NULL, 0,
                        "-fsanitize=%.*s is not supported: EmbCC's "
                        "sanitizer inserts checks that TRAP, and this one "
                        "needs a runtime library to report through. The "
                        "ones it has are undefined, "
                        "signed-integer-overflow, integer-divide-by-zero "
                        "and shift", (int)n, p);
                if (off) san_mask &= ~bit; else san_mask |= bit;
                if (!e) break;
                p = e + 1;
            }
            continue;
        } else if (strncmp(argv[i], "-fsanitize", 10) == 0 ||
                   strncmp(argv[i], "-fprofile", 9) == 0 ||
                   strcmp(argv[i], "-fcoverage-mapping") == 0 ||
                   strcmp(argv[i], "--coverage") == 0 ||
                   strcmp(argv[i], "-pg") == 0 ||
                   strcmp(argv[i], "-flto") == 0 ||
                   strcmp(argv[i], "-fPIC") == 0 ||
                   strcmp(argv[i], "-fpic") == 0 ||
                   strcmp(argv[i], "-fPIE") == 0 ||
                   strcmp(argv[i], "-fpie") == 0 ||
                   strcmp(argv[i], "-fshort-enums") == 0 ||
                   strcmp(argv[i], "-fstack-clash-protection") == 0 ||
                   strncmp(argv[i], "-fcf-protection", 15) == 0) {
            /* Refused BY NAME, every one. These do not describe a
             * preference the compiler may decline -- each is a promise
             * about the code, and accepting one while emitting
             * ordinary code hands back an object that links and then
             * does the wrong thing:
             *
             *   -fsanitize=  no checks would be inserted
             *   -fprofile-*, --coverage, -pg  no counters
             *   -flto        no bitcode, so a whole-program link is a
             *                plain one and the sizes mislead
             *   -fPIC/-fpie  the code is position DEPENDENT; a shared
             *                object built from it would relocate wrong
             *   -fshort-enums  enums are `int` here, so a struct
             *                holding one is laid out differently --
             *                which is an ABI difference, not a size
             *                preference. (Note clang does NOT default
             *                to this on ARM; arm-none-eabi-gcc does.)
             *   -fstack-clash-protection, -fcf-protection  no probes,
             *                no landing pads
             */
            fprintf(stderr, "embcc: error: %s is not supported; EmbCC "
                            "would emit ordinary code and the flag's "
                            "promise would not hold\n", argv[i]);
            return 1;
        } else if (strcmp(argv[i], "-shared") == 0 ||
                   strcmp(argv[i], "-static-pie") == 0) {
            fprintf(stderr, "embcc: error: %s needs position-independent "
                            "code, which EmbCC does not emit\n", argv[i]);
            return 1;
        } else if (strcmp(argv[i], "-mno-sse") == 0 ||
                   strcmp(argv[i], "-mno-sse2") == 0 ||
                   strcmp(argv[i], "-mgeneral-regs-only") == 0) {
            no_sse = 1;   /* -mno-mmx / -mno-80387 imply it too, below */
        } else if (strcmp(argv[i], "-mno-mmx") == 0 ||
                   strcmp(argv[i], "-mno-red-zone") == 0 ||
                   strcmp(argv[i], "-mno-80387") == 0 ||
                   strncmp(argv[i], "-mcmodel=", 9) == 0) {
            /* accepted: EmbCC never uses MMX or the red zone, and its default
             * code model already suits the kernel's higher-half link. */
        } else if (strcmp(argv[i], "-include") == 0) {
            if (i + 1 == argc) {
                fprintf(stderr, "embcc: -include needs a file\n");
                return 1;
            }
            cpp_preinclude(argv[++i]);
        } else if (strncmp(argv[i], "-D", 2) == 0 ||
                   strncmp(argv[i], "-U", 2) == 0) {
            int undef = argv[i][1] == 'U';
            const char *d = argv[i][2] ? argv[i] + 2
                                       : (i + 1 < argc ? argv[++i] : 0);
            if (!d || !*d) {
                fprintf(stderr, "embcc: -%c needs a macro name\n",
                        undef ? 'U' : 'D');
                return 1;
            }
            cpp_cmdline_define(d, undef);
        } else if (strncmp(argv[i], "-I", 2) == 0) {
            const char *dir = argv[i][2] ? argv[i] + 2
                                         : (i + 1 < argc ? argv[++i] : 0);
            if (!dir) {
                fprintf(stderr, "embcc: -I needs a directory\n");
                return 1;
            }
            if (nincdirs >= MAX_INCDIRS) {
                fprintf(stderr, "embcc: too many -I directories\n");
                return 1;
            }
            incdirs[nincdirs++] = dir;
        } else if (strcmp(argv[i], "-nostdinc") == 0) {
            no_stdinc = 1;
        } else if (strcmp(argv[i], "--print-search-dirs") == 0) {
            paths_print_search_dirs();
            return 0;
        } else if (strncmp(argv[i], "-isystem", 8) == 0) {
            /* A system-include directory: searched as an -I one, but marked,
             * so -MM can leave its headers out of the dependency list. */
            const char *dir = argv[i][8] ? argv[i] + 8
                                         : (i + 1 < argc ? argv[++i] : 0);
            if (!dir) {
                fprintf(stderr, "embcc: -isystem needs a directory\n");
                return 1;
            }
            if (nincdirs >= MAX_INCDIRS) {
                fprintf(stderr, "embcc: too many include directories\n");
                return 1;
            }
            incdir_sys[nincdirs] = 1;
            incdirs[nincdirs++] = dir;
        } else if ((argv[i][0] == '-' && argv[i][1] == 'l') ||
                   (argv[i][0] == '-' && argv[i][1] == 'L') ||
                   !strcmp(argv[i], "-T") || !strcmp(argv[i], "-e") ||
                   !strcmp(argv[i], "-u")) {
            /* the link's own options, as gcc takes them: -lNAME and
             * -L DIR (attached or not), -T SCRIPT, -e SYM, -u SYM */
            char opt = argv[i][1];
            int at = i;
            const char *v = (opt == 'l' || opt == 'L') && argv[i][2]
                ? argv[i] + 2 : (i + 1 < argc ? argv[++i] : NULL);
            if (!v || !*v) {
                fprintf(stderr, "embcc: -%c needs a%s\n", opt,
                        opt == 'l' ? " library name" : opt == 'L' ? " directory"
                        : opt == 'T' ? " linker script" : " symbol");
                return 1;
            }
            if (opt == 'l') {
                /* kept in command-line order, as one -lNAME word */
                size_t ln = strlen(v) + 3;
                char *w = xmalloc(ln);
                snprintf(w, ln, "-l%s", v);
                g_child_skip[at] = g_child_skip[i] = 1;
                if (g_nlink_in < 256) {
                    g_link_argi[g_nlink_in] = at;
                    g_link_in[g_nlink_in++] = w;
                }
            } else if (opt == 'L') {
                if (g_nlibdirs < 64)
                    g_libdirs[g_nlibdirs++] = v;
            } else if (opt == 'T') {
                g_script = v;
            } else if (opt == 'e') {
                g_entry = v;
            } else if (g_nundefs < 64) {
                g_undefs[g_nundefs++] = v;
            }
        } else if (!strcmp(argv[i], "-nostdlib")) {
            g_nostdlib = 1;
        } else if (!strcmp(argv[i], "-nostartfiles")) {
            g_nostartfiles = 1;
        } else if (!strcmp(argv[i], "-nodefaultlibs")) {
            g_nodefaultlibs = 1;
        } else if (!strcmp(argv[i], "-static")) {
            /* every image EmbLD writes is static */
        } else if (argv[i][0] != '-' && has_link_input_suffix(argv[i])) {
            g_child_skip[i] = 1;
            if (g_nlink_in < 256) {
                g_link_argi[g_nlink_in] = i;
                g_link_in[g_nlink_in++] = argv[i];
            }
        } else if (!strncmp(argv[i], "-j", 2) &&
                   (!argv[i][2] || (argv[i][2] >= '0' && argv[i][2] <= '9'))) {
            /* -j N, -jN; -j alone (or before an option) is one per
             * processor */
            g_child_skip[i] = 1;
            const char *v = argv[i][2] ? argv[i] + 2
                : (i + 1 < argc && argv[i + 1][0] >= '0' &&
                   argv[i + 1][0] <= '9') ? argv[++i] : NULL;
            g_child_skip[i] = 1;
            g_jobs = v ? atoi(v) : plat_ncpus();
            if (g_jobs < 1 || g_jobs > 256) {
                fprintf(stderr, "embcc: error: -j wants 1 to 256 jobs, not "
                                "'%s'\n", v ? v : "?");
                return 1;
            }
        } else if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 == argc) {
                fprintf(stderr, "embcc: -o needs a FILE\n");
                return 1;
            }
            g_child_skip[i] = g_child_skip[i + 1] = 1;
            output = argv[++i];
            /* `-o -` means stdout, as it does in every other compiler.
             * Every text-producing mode here already writes to stdout
             * when no -o was given, so the whole of the support is to
             * map the name onto that. Without it the driver created a
             * FILE called "-" in the working directory -- which is how
             * one got committed to this repository. */
            if (strcmp(output, "-") == 0)
                output = NULL, out_is_stdout = 1;
        } else if (has_c_suffix(argv[i]) || has_asm_suffix(argv[i]) ||
                   has_gas_suffix(argv[i]) ||
                   has_cxx_suffix(argv[i]) || has_ir_suffix(argv[i]) ||
                   (lang >= 0 && argv[i][0] != '-') ||
                   strcmp(argv[i], "-") == 0) {
            if (g_nsrc == MAX_SRCS) {
                fprintf(stderr, "embcc: error: more than %d source files\n",
                        MAX_SRCS);
                return 1;
            }
            g_child_skip[i] = 1;
            g_src_argi[g_nsrc] = i;
            g_srcs[g_nsrc++] = argv[i];
            if (!input)
                input = argv[i];
        } else {
            fprintf(stderr, "embcc: error: unknown argument '%s'\n",
                    argv[i]);
            print_usage(stderr);
            return 1;
        }
    }

    /* Standard input has no suffix to say what it is: GCC wants -E
     * (which reads it as C) or -x, and so does this. */
    for (int k = 0; k < g_nsrc; k++)
        if (strcmp(g_srcs[k], "-") == 0 && !pp_only && lang < 0) {
            fprintf(stderr, "embcc: error: -E or -x required when input is "
                            "from standard input\n");
            return 1;
        }
    arm_float_resolve();
    riscv_float_resolve();
    /* -mcmse with the FPU in use: an entry function would have to clear
     * s0-s15 and FPSCR when the Secure state's FP context is active
     * (CONTROL_S.SFPA), and a call to the Non-secure state would have to
     * hand the hard-float convention's arguments over in VFP registers.
     * Neither is emitted, so the combination is refused rather than
     * leaking a Secure float into the Non-secure state. */
    if (g_arm_cmse && target_thumb_fpu())
        diag_fatal(NULL, 0, "-mcmse with an FPU (-mfpu=, -mfloat-abi=softfp "
                   "or hard, or an -eabihf triple) is not supported: EmbCC "
                   "does not clear the floating-point registers a "
                   "cmse_nonsecure_entry function must clear; build the "
                   "Secure side with -mfloat-abi=soft");
    sema_set_gnu89_inline(gnu89_inline || std_gnu89);
    /* -fno-jump-tables for a whole test suite, whose scripts spell their
     * own command lines: every dense switch takes the compare tree. */
    if (plat_getenv("EMBCC_NO_JUMP_TABLES"))
        target_set_jump_tables(0);
    /* COMMON is an ELF symbol kind (SHN_COMMON); the Mach-O and COFF
     * writers emit every tentative definition as a definition, and a
     * build that needs two of them merged would be told so only by its
     * linker. */
    if (g_fcommon && target_fmt_get() != TGT_FMT_ELF) {
        fprintf(stderr, "embcc: error: -fcommon is not supported for %s: "
                        "EmbCC writes COMMON symbols into ELF objects only, "
                        "and this target's are %s\n", target_triple_now(),
                target_fmt_name(target_fmt_get()));
        return 1;
    }

    if (g_want_dumpmachine) {
        printf("%s\n", target_triple_now());
        return 0;
    }
    if (g_want_dump_predef) {
        dump_predef();
        return 0;
    }

    if (g_nsrc > 1) {
        if (inspect_stage || why_decision || emit_c_only || want_iface) {
            fprintf(stderr, "embcc: error: %d source files: this mode reads "
                    "one\n", g_nsrc);
            return 1;
        }
        /* -fsyntax-only and --fix write no object: each source is
         * checked, and nothing is linked */
        return done(multi_source(argc, argv, output,
                                 compile_mode || syntax_only || want_fix,
                                 want_asm, pp_only));
    }
    /* Objects and archives with no source: the link step of a build. */
    if (!input && g_nlink_in && !compile_mode && !pp_only &&
        !inspect_stage && !why_decision && !syntax_only) {
        lang_cxx = 0;
        return done(compile_and_link(NULL, output));
    }
    if (!input) {
        fprintf(stderr, "embcc: error: no input file\n");
        return 1;
    }
    if (g_nlink_in && (compile_mode || pp_only))
        fprintf(stderr, "embcc: warning: %s: linker input unused because "
                        "the link is not done\n", g_link_in[0]);
    /* The checks are inserted by irgen, as ordinary IR, so at -O2 the
     * optimizer folds away the ones whose operands it knows -- a
     * constant non-zero divisor leaves nothing behind. */
    irgen_set_sanitize(san_mask);
    irgen_set_instrument(want_instr, instr_funcs, instr_files);
    irgen_set_opt_size(opt_for_size);
    if (short_wchar) {
        /* wchar_t is unsigned short (sema's ty_wchar, the lexer's L""
         * and L'' literals, C++'s wchar_t), and the macros move with it,
         * as clang's do: the per-target tables know only the default.
         * An ARM object says so in Tag_ABI_PCS_wchar_t (2). */
        target_set_short_wchar(1);
        cpp_cmdline_define("__WCHAR_TYPE__=unsigned short", 0);
        cpp_cmdline_define("__WCHAR_MAX__=65535", 0);
        cpp_cmdline_define("__WCHAR_MIN__=0", 0);
        cpp_cmdline_define("__WCHAR_WIDTH__=16", 0);
        cpp_cmdline_define("__WCHAR_UNSIGNED__=1", 0);
        cpp_cmdline_define("__SIZEOF_WCHAR_T__=2", 0);
        if (target_get() == TARGET_THUMB)
            cpp_cmdline_define("__ARM_SIZEOF_WCHAR_T=2", 0);
    }
    lang_cxx = lang >= 0 ? lang : has_cxx_suffix(input);
    /* The C parser types the constants (parse.c); the C++ front end
     * types its own, and resolves overloads by them before lowering to
     * C, so the flag would reach one and not the other. */
    if (g_single_prec) {
        if (lang_cxx) {
            fprintf(stderr, "embcc: error: -fsingle-precision-constant is "
                            "supported for C, not C++: the C++ front end "
                            "would still type 1.0 as double\n");
            return 1;
        }
        parse_set_single_precision_constant(1);
    }
    if (lang_cxx) {
        /* These analyses run in the C front end, over the C that C++ lowers
         * to — where a template instantiated from a header is attributed to
         * the .cc that instantiated it. A warning pointing at the wrong line
         * is worse than none, so they stay C-only until the C++ front end
         * grows its own (docs/manual/diagnostics.md T4).
         *
         * The test is whether the analysis is about code the LOWERING
         * invented. These five are: an unused variable or a shadowed name
         * in generated C means nothing to the reader. -Wformat is not, and
         * is deliberately absent — a format string and the arguments
         * beside it are the programmer's own, they survive lowering
         * unchanged, and the diagnostic lands on their line. */
        diag_enable_warning("unused-variable", 0);
        diag_enable_warning("unused-parameter", 0);
        diag_enable_warning("unused-function", 0);
        diag_enable_warning("shadow", 0);
        diag_enable_warning("sign-compare", 0);
    }

    /* EmbCC's own freestanding headers (stddef, stdarg, stdbool, float)
     * ship beside the binary, so <stdarg.h> resolves with no -I — exactly
     * as a compiler finds its own headers. Appended last, below every -I,
     * so a project header of the same name still wins. */
    if (nincdirs < MAX_INCDIRS) {
        static char selfinc[4096];
        const char *slash = strrchr(argv[0], '/');
        if (slash)
            snprintf(selfinc, sizeof selfinc, "%.*s/include",
                     (int)(slash - argv[0]), argv[0]);
        else
            snprintf(selfinc, sizeof selfinc, "./include");
        incdir_sys[nincdirs] = 1;
        incdirs[nincdirs++] = selfinc;
    }
    /* `inspect` produces a report, not an object, so like -fsyntax-only it
     * implies -c. This has to happen BEFORE the dispatches below read
     * pp_only: `inspect pp` IS the preprocessor's own stage, which the
     * driver already had under -E. */
    if (inspect_stage) {
        compile_mode = 1;
        if (!strcmp(inspect_stage, "pp"))
            pp_only = 1;
    }
    if (why_decision)
        compile_mode = 1;         /* a question, not an object */

    /* A `.ir` input is EmbIR's own textual form: parse it and print it back
     * (§9.1). That is the round-trip -- `print | parse | print` must produce
     * the same bytes -- and it is what makes a pass testable text-in,
     * text-out with no C source and no backend in the loop. */
    if (inspect_stage && !strcmp(inspect_stage, "ir")) {
        size_t ln = strlen(input);
        if (ln > 3 && !strcmp(input + ln - 3, ".ir")) {
            char *txt = src_read(input, NULL);
            if (!txt)
                diag_fatal(input, 0, "cannot open file");
            struct ir_unit *pu = ir_parse(input, txt);
            /* With -O, the optimizer runs on what was parsed first. A
             * pass can then be tested on IR written to provoke it -- a
             * shape C reaches only by luck, if at all. */
            if (opt_level > 0 || opt_for_size) {
                /* IR written by hand has no source to point at, and the
                 * verifier (EMBCC_VERIFY, which the test suite sets) takes
                 * an instruction with no location for one a pass built
                 * carelessly. To the optimizer such an instruction IS
                 * synthesized, so it says so -- only here, so that without
                 * -O the file still comes back exactly as written. */
                for (int f = 0; f < pu->nfuncs; f++)
                    for (int k = 0; k < pu->funcs[f].nins; k++)
                        if (!pu->funcs[f].ins[k].line)
                            pu->funcs[f].ins[k].synth = 1;
                opt_run(pu, opt_for_size ? OPT_SIZE : opt_level);
            }
            struct outbuf ob = { NULL, 0, 0 };
            ir_print_unit(&ob, pu);
            fwrite(ob.p, 1, ob.n, stdout);
            ob_free(&ob);
            return done(0);
        }
    }

    /* A `.asm` input goes to the built-in assembler (A1), not the C front-end.
     * Like gcc dispatching `.s`, embcc owns the kernel's hand-written assembly:
     * `embcc -c foo.asm -o foo.o` replaces `nasm -f elf64`. */
    if (has_gas_suffix(input)) {
        if (pp_only)
            return done(compile(input, dep_only ? NULL : output, 1));
        /* Without -c a .s/.S is assembled AND linked, as gcc does it:
         * `embcc start.S main.o -T fw.ld -o fw.elf`. It used to write the
         * object under the output's name, an "image" no loader reads. */
        if (!compile_mode && !syntax_only && !want_asm && !want_iface)
            return done(compile_and_link(input, output));
        return assemble_file(input, output ? output : default_output(input));
    }
    if (has_asm_suffix(input)) {
        if (pp_only) {
            fprintf(stderr, "embcc: error: -E does not apply to assembly\n");
            return 1;
        }
        /* NASM syntax is x86-64 assembly, and the assembler writes ELF.
         * For any other target it wrote an x86-64 object anyway -- an
         * ELF32 one whose machine was x86-64 on a 32-bit target -- and
         * the error, if any, came from the linker. */
        if (target_get() != TARGET_X86_64 || target_fmt_get() != TGT_FMT_ELF) {
            fprintf(stderr, "embcc: error: '%s' is NASM-syntax x86-64 "
                    "assembly, which EmbCC assembles to x86-64 ELF only, "
                    "and the target is %s\n", input, target_triple_now());
            return 1;
        }
        return as_assemble(input, output ? output : default_output(input),
                           AS_ELF64);
    }
    if (pp_only)
        return done(compile(input, dep_only ? NULL : output, 1));
    if (emit_c_only) {
        if (!lang_cxx) {
            fprintf(stderr, "embcc: error: --emit-c lowers C++; '%s' is C\n",
                    input);
            return 1;
        }
        return done(compile(input, output, 0));
    }
    /* -fsyntax-only (and --fix) check the file and write nothing, so there
     * is nothing to link: they imply -c, as they do in GCC. */
    if (syntax_only)
        compile_mode = 1;
    if (g_save_temps && !syntax_only && !want_iface) {
        int src = has_c_suffix(input) || has_cxx_suffix(input) || lang >= 0;
        if (src && save_temps(argc, argv, input, output, compile_mode,
                              want_asm))
            return done(1);
    }
    if (!compile_mode)
        return done(compile_and_link(input, output));
    /* An interface report goes to stdout unless the caller named a file.
     * -S writes NAME.s, as GCC does -- it went to stdout, and a build
     * that ran `cc -S x.c` found no x.s -- and `-o -` still means
     * stdout. */
    if (want_asm && !output && !out_is_stdout)
        output = default_output_sfx(input, ".s");
    if ((want_iface || want_asm) && !output)
        return done(compile(input, NULL, 0));
    /* `-o -` reached here with output == NULL, which for an OBJECT means
     * "use the default name" rather than "write to stdout" -- so it
     * would quietly produce input.o. Refuse instead of surprising the
     * caller (THE RULE); the ELF writer writes to a path, not a pipe. */
    if (out_is_stdout)
        return done((fprintf(stderr, "embcc: error: `-o -` writes to stdout, "
                                     "which -E, -S and --emit-interfaces "
                                     "support but an object file does not; "
                                     "name a file\n"), 1));
    return done(compile(input, output ? output : default_output(input), 0));
}
