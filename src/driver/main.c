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
#include "../ir/ir.h"
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

#define EMBCC_VERSION "1.0.0-m2.complete"

static void print_version(void)
{
    /* Honest: names what exists and what does not. */
    printf("EmbCC %s — C compiler for EmbLinkOS, target %s\n",
           EMBCC_VERSION, target_triple_now());
    printf("Language: C11 on both targets — VLAs, _Complex, long double, "
           "_Atomic and the atomic builtins, _Generic — plus the GNU "
           "extensions EmbLinkOS uses (statement expressions, typeof, "
           "computed goto, attributes, extended inline asm).\n");
    printf("Targets (--target=): x86_64-elf (default; System V AMD64, "
           "x87 long double) and aarch64-elf (AAPCS64, binary128 long "
           "double through libgcc).\n");
    printf("C++ (.cc/.cpp/.cxx/.C, or -x c++): in progress toward C++20 "
           "with libstdc++ (docs/language/cpp-levels.md) — namespaces, overloading, "
           "references, classes with constructors and destructors, "
           "new/delete, lowered through C to either target.\n");
    printf("Also: the preprocessor (-E), -O0..-O2, -g (DWARF), embas "
           "(NASM-syntax .asm, x86-64) and embld (the linker, x86-64 ELF "
           "and EMBX). Not yet: __thread, PIE, embld for aarch64 — "
           "see docs/language/compatibility.md.\n");
}

static void print_usage(FILE *out)
{
    fprintf(out,
            "usage: embcc [-E] -c FILE.c|FILE.cc|FILE.asm [-o FILE.o]\n"
            "             [--target=x86_64-elf|aarch64-elf] [-x c|c++]\n"
            "             [-std=...] [--emit-c]\n"
            "             [-I DIR]... [-isystem DIR]... [-g] [-O0|-O1|-O2]\n"
            "             [-mno-sse] [-mno-red-zone] [-mcmodel=kernel] ...\n"
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
      "  -S                     write assembly (.s) instead of an object (x86-64)\n"
      "  -fsyntax-only          check, write nothing\n"
      "  --emit-c               print the C a C++ unit lowers to\n"
      "  -o FILE                where to write it\n"
      "\nthe language\n"
      "  -x c|c++               treat the input as this language\n"
      "  -std=...               accepted; EmbCC has one dialect per language\n"
      "  -I DIR, -isystem DIR   header search paths\n"
      "  -D NAME[=VALUE], -U NAME  define and undefine macros\n"
      "  -include FILE          include it before the file\n"
      "  -fno-exceptions, -fno-rtti   C++ without them\n"
      "  -fno-access-control          do not enforce private/protected\n"
      "\nthe target\n"
      "  --target=x86_64-elf|aarch64-elf\n"
      "  -dumpmachine           print that target\n"
      "  -O0 -O1 -O2            optimisation\n"
      "  -g                     debug information (DWARF)\n"
      "  -mno-sse -mno-red-zone -mcmodel=kernel -mgeneral-regs-only\n"
      "\ndiagnostics (docs/tools/diagnostics.md)\n"
      "  -fdiagnostics-format=text|json   caret output, or GCC's JSON\n"
      "  -fdiagnostics-color=auto|always|never\n"
      "  -fmax-errors=N         stop after N\n"
      "  -w                     no warnings;  -Werror  warnings are errors\n"
      "  -Wall, -Wextra, -Wname, -Wno-name (see --help-warnings)\n"
      "\ndependencies\n"
      "  -M, -MM                write the make rule instead of compiling\n"
      "  -MD, -MMD              write it beside the object\n"
      "  -MF FILE, -MT TARGET, -MP\n"
      "\nreporting\n"
      "  --version, --help, --dump-predef\n"
      "  --explain ID           what a diagnostic means, and the fix\n"
      "  --fix                  apply the fix-its it proposes\n", out);
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
static int object_format_ready(void)
{
    if (target_fmt_get() == TGT_FMT_ELF)
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
static const char *default_output(const char *in)
{
    const char *dot = strrchr(in, '.');
    size_t n = dot ? (size_t)(dot - in) : strlen(in);
    char *out = xmalloc(n + 3);
    memcpy(out, in, n);
    memcpy(out + n, ".o", 3);
    return out;
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

/* -O level. 0 (the default) runs no optimizer, so output is byte-for-byte
 * as before — the property the self-host fixed point rests on. */
static int opt_level;

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
 * (docs/tools/diagnostics.md T5) and what a build's "does this still compile" step
 * wants. */
static int syntax_only;
/* Tool mode (§17): `embcc inspect <stage> file.c` stops the pipeline at a
 * stage and prints what it built, instead of producing an object. */
static const char *inspect_stage;
/* -S: emit the assembly the backend produced, rather than an object. */
static int want_asm;
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
static int compile_unit(const char *in, const char *out, int pp_only);

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
    return rc;
}

static int compile_unit(const char *in, const char *out, int pp_only)
{
    char *src = read_file(in);
    diag_register_source(in, src);   /* so diagnostics can show its lines */
    predef_set_cxx(lang_cxx);
    if (lang_cxx)
        cpp_set_cxx(cxx_has_builtin, want_exceptions);
    cpp_set_system_dirs(incdir_sys, nincdirs);
    char *pp = cpp_process(in, src, incdirs, nincdirs);
    if (dep_mode && dep_only) {       /* -M/-MM: the rule is the output */
        write_deps(in, out);
        return 0;
    }
    if (pp_only) {
        fputs(pp, stdout);
        return 0;
    }
    if (lang_cxx) {
        cxx_set_exceptions(want_exceptions);
        cxx_set_rtti(want_rtti);
        pp = cxx_translate(in, pp);
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
    if (parse_error_count()) {
        /* Every syntax error is out; the tree is not whole, so nothing
         * downstream runs on it (a later pass would only invent errors). */
        diag_terminated(parse_error_count());
        return 1;                     /* (--fix still gets its turn) */
    }
    sema_check(u);
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
    for (struct topasm *ta = u->topasm; ta; ta = ta->next) {
        /* The mnemonics the built-in assembler encodes are x86-64. The
         * directives -- labels and .byte/.long/.quad -- are not, so a
         * block written as data assembles on any target, and one written
         * with mnemonics is refused there by name instead of quietly
         * emitting x86 bytes into an aarch64 image. */
        topasm_assemble(ta, target_get() != TARGET_AARCH64);
        for (int r = 0; r < ta->nrels; r++)
            for (struct func *f = u->funcs; f; f = f->next)
                if (!f->absorbed &&
                    strcmp(f->name, ta->rels[r].target) == 0)
                    f->used = 1;
    }

    remarks_enable(want_remarks || why_decision != NULL);
    struct ir_unit *iu = irgen(u);
    opt_run(iu, opt_level);

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

    struct code text = { 0, 0, 0 };
    struct extcall *ext;
    struct strsite *strs;
    struct gsite *gs;
    struct fsite *fs;
    int next, nstrs, ngs, nfs;
    enum target_arch ta = target_get();
    if (ta == TARGET_AARCH64)
        codegen_unit_arm64(iu, &text, &ext, &next, &strs, &nstrs, &gs, &ngs,
                           &fs, &nfs, want_debug, opt_level >= 1, no_sse,
                           opt_level >= 2);
    else
        codegen_unit(iu, &text, &ext, &next, &strs, &nstrs, &gs, &ngs,
                     &fs, &nfs, want_debug, opt_level >= 1, no_sse,
                     opt_level >= 2);

    /* Lay out the defined globals: initialized -> .data, zero -> .bss,
     * each aligned to its (element) size. A section("name") global goes to
     * its named section instead, in declaration order, zero-filled when it
     * has no initializer — PROGBITS like gcc's, NOBITS only for a .bss*
     * name. With no const tracking a named section is writable unless its
     * name says .rodata/.text; gcc would make a const-only one read-only,
     * which changes its segment and nothing the program can observe. */
    struct named { const char *name; int len, align, nobits, flags, ndx;
                   char *buf; } named[64];
    int nnamed = 0;
    int data_len = 0, bss_len = 0;
    for (struct global *g = u->globals; g; g = g->next) {
        if (g->absorbed || !g->defined)
            continue;
        int align = ty_align(g->ty);
        g->in_bss = !g->has_init;
        g->named = 0;
        int *len = g->in_bss ? &bss_len : &data_len;
        if (g->section) {
            int k = 0;
            while (k < nnamed && strcmp(named[k].name, g->section) != 0)
                k++;
            if (k == nnamed) {
                if (nnamed == 64)
                    diag_fatal(in, 0, "more than 64 named sections");
                const char *n = g->section;
                named[k].name = n;
                named[k].len = 0;
                named[k].align = 1;
                named[k].nobits = strncmp(n, ".bss", 4) == 0 &&
                                  (n[4] == 0 || n[4] == '.');
                named[k].flags = SHF_ALLOC;
                if (strncmp(n, ".text", 5) == 0)
                    named[k].flags |= SHF_EXECINSTR;
                else if (strncmp(n, ".rodata", 7) != 0)
                    named[k].flags |= SHF_WRITE;
                named[k].buf = NULL;
                nnamed++;
            }
            if (named[k].nobits && g->has_init)
                diag_fatal(g->file, g->line,
                           "'%s' has an initializer but is placed in "
                           "NOBITS section '%s'", g->name, g->section);
            g->named = k + 1;
            g->in_bss = named[k].nobits;
            len = &named[k].len;
            if (align > named[k].align)
                named[k].align = align;
        }
        *len = (*len + align - 1) & ~(align - 1);
        g->off = *len;
        *len += ty_size(g->ty);
    }
    for (int k = 0; k < nnamed; k++)
        if (!named[k].nobits && named[k].len)
            named[k].buf = xcalloc(1, (size_t)named[k].len);
    char *data = NULL;
    if (data_len)
        data = xcalloc(1, (size_t)data_len);
    if (data_len || nnamed) {
        for (struct global *g = u->globals; g; g = g->next) {
            if (g->absorbed || !g->defined || g->in_bss)
                continue;
            char *img = g->named ? named[g->named - 1].buf : data;
            if (!img)
                continue;                   /* a zero-length section */
            if (g->init_bytes) {
                int n = g->init_len;
                if (n > ty_size(g->ty))
                    n = ty_size(g->ty);
                memcpy(img + g->off, g->init_bytes, (size_t)n);
                continue;
            }
            unsigned long v = (unsigned long)g->init;
            for (int b = 0; b < ty_size(g->ty); b++)
                img[g->off + b] = (char)((v >> (8 * b)) & 0xff);
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
            int si = ir_intern_string(iu, g->relocs[i].str,
                                      g->relocs[i].str_len * w);
            g->relocs[i].str_off = iu->strs[si].off;
        }
    }

    /* .rodata: the string literals, at the offsets irgen assigned. */
    char *rodata = NULL;
    if (iu->rodata_len) {
        rodata = xmalloc((size_t)iu->rodata_len);
        for (int i = 0; i < iu->nstrs; i++)
            memcpy(rodata + iu->strs[i].off, iu->strs[i].bytes,
                   (size_t)iu->strs[i].len);
    }

    /* File-scope asm blocks (crt0's _start): place each block's bytes in
     * .text after the functions (16-aligned) and record where, so its labels
     * and relocations land at the right offset. The blocks were already
     * assembled above (before irgen) and their call targets marked used. */
    for (struct topasm *ta = u->topasm; ta; ta = ta->next) {
        code_align(&text, 16, 0x90);
        ta->text_off = text.len;
        for (int k = 0; k < ta->codelen; k++)
            code_byte(&text, ta->code[k]);
    }

    /* -g: build the DWARF line sections now (needs each func's code_off/len,
     * set by codegen). Off, dw stays empty and nothing below fires. */
    struct dwarf_out dw = { { 0 }, { 0 }, 0, 0, 0 };
    if (want_debug)
        dwarf_emit(iu, in, &dw);
    struct eh_out eh;
    memset(&eh, 0, sizeof eh);
    int unwind = want_unwind > 0 ||
                 (lang_cxx && (want_unwind < 0 || want_exceptions));
    if (unwind)
        eh_emit(iu, ta == TARGET_AARCH64, &eh);

    /* -S: the same bytes, as text (src/driver/asmout.c). Everything the
     * emitter needs is in hand here -- the code, the string pool, and the
     * relocation sites the backend recorded. */
    if (want_asm) {
        if (ta == TARGET_AARCH64)
            diag_fatal(in, 0,
                       "-S is x86-64 only: there is no aarch64 disassembler "
                       "here, and emitting text that is not the object would "
                       "be worse than refusing");
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
    if (target_fmt_get() == TGT_FMT_MACHO) {
        if (want_debug)
            diag_fatal(in, 0,
                       "-g is not supported for a Darwin target yet: its "
                       "DWARF goes in a __DWARF segment this does not "
                       "write, and emitting the ELF layout under a Mach-O "
                       "name would be worse than refusing");
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
        if (data_len)
            m_data = machow_add_section(mw, "__DATA", "__data", S_REGULAR,
                                        data, (unsigned long long)data_len, 3);
        if (bss_len)
            m_bss = machow_add_section(mw, "__DATA", "__bss", S_ZEROFILL,
                                       NULL, (unsigned long long)bss_len, 3);
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

    if (!object_format_ready())
        return 1;
    struct elfw *w = elfw_new(target_elf_machine(target_get()));
    int text_ndx = elfw_add_section(w, ".text", SHT_PROGBITS,
                                    SHF_ALLOC | SHF_EXECINSTR,
                                    text.p, (Elf64_Xword)text.len, 16);
    int rodata_ndx = 0;
    if (rodata)
        rodata_ndx = elfw_add_section(w, ".rodata", SHT_PROGBITS,
                                      SHF_ALLOC, rodata,
                                      (Elf64_Xword)iu->rodata_len, 1);
    int data_ndx = 0, bss_ndx = 0;
    if (data_len)
        data_ndx = elfw_add_section(w, ".data", SHT_PROGBITS,
                                    SHF_ALLOC | SHF_WRITE, data,
                                    (Elf64_Xword)data_len, 8);
    if (bss_len)
        bss_ndx = elfw_add_section(w, ".bss", SHT_NOBITS,
                                   SHF_ALLOC | SHF_WRITE, NULL,
                                   (Elf64_Xword)bss_len, 8);
    for (int k = 0; k < nnamed; k++)
        named[k].ndx = elfw_add_section(
            w, named[k].name, named[k].nobits ? SHT_NOBITS : SHT_PROGBITS,
            (Elf64_Xword)named[k].flags, named[k].buf,
            (Elf64_Xword)named[k].len, (Elf64_Xword)named[k].align);
    /* -g: the three DWARF sections (non-alloc, so no load cost; stripped
     * from a shipped image without touching the code). Their indices feed
     * the relocation-target lookup below. */
    static const char *const dwsec_name[DWARF_NSEC] =
        { ".debug_abbrev", ".debug_info", ".debug_line" };
    int dwsec_ndx[DWARF_NSEC] = { 0, 0, 0 };
    if (want_debug)
        for (int s = 0; s < DWARF_NSEC; s++)
            dwsec_ndx[s] = elfw_add_section(w, dwsec_name[s], SHT_PROGBITS, 0,
                                            dw.sec[s], (Elf64_Xword)dw.seclen[s],
                                            1);
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
    int rodata_sym = 0;
    if (rodata)
        rodata_sym = elfw_add_symbol(w, "", 0, 0,
                                     ELF64_ST_INFO(STB_LOCAL,
                                                   STT_SECTION),
                                     (Elf64_Half)rodata_ndx);
    /* -g: STT_SECTION symbols for the debug sections, so the line/info
     * fields can relocate against them (DWTGT_ABBREV/DWTGT_LINE). Added here
     * in the local block — the writer refuses a local after any global. */
    int dwsym[DWARF_NSEC] = { 0, 0, 0 };
    if (want_debug)
        for (int s = 0; s < DWARF_NSEC; s++)
            dwsym[s] = elfw_add_symbol(w, "", 0, 0,
                          ELF64_ST_INFO(STB_LOCAL, STT_SECTION),
                          (Elf64_Half)dwsec_ndx[s]);
    /* Locals before globals — the writer enforces the gABI ordering.
     * Only canonical, defined functions own code. */
    for (struct func *f = u->funcs; f; f = f->next)
        if (!f->absorbed && f->has_defn && f->is_static && f->used)
            f->sym_ndx = elfw_add_symbol(
                w, f->name, (Elf64_Addr)f->code_off,
                (Elf64_Xword)f->code_len,
                ELF64_ST_INFO(STB_LOCAL, STT_FUNC),
                (Elf64_Half)text_ndx);
    for (struct global *g = u->globals; g; g = g->next)
        if (!g->absorbed && g->defined && g->is_static)
            g->sym_ndx = elfw_add_symbol(
                w, g->name, (Elf64_Addr)g->off,
                (Elf64_Xword)ty_size(g->ty),
                ELF64_ST_INFO(STB_LOCAL, STT_OBJECT),
                (Elf64_Half)(g->named ? named[g->named - 1].ndx
                             : g->in_bss ? bss_ndx : data_ndx));
    for (struct func *f = u->funcs; f; f = f->next)
        if (!f->absorbed && f->has_defn && !f->is_static)
            f->sym_ndx = elfw_add_symbol(
                w, f->name, (Elf64_Addr)f->code_off,
                (Elf64_Xword)f->code_len,
                ELF64_ST_INFO(f->is_weak ? STB_WEAK : STB_GLOBAL, STT_FUNC),
                (Elf64_Half)text_ndx);
    for (struct global *g = u->globals; g; g = g->next)
        if (!g->absorbed && g->defined && !g->is_static)
            g->sym_ndx = elfw_add_symbol(
                w, g->name, (Elf64_Addr)g->off,
                (Elf64_Xword)ty_size(g->ty),
                ELF64_ST_INFO(g->is_weak ? STB_WEAK : STB_GLOBAL, STT_OBJECT),
                (Elf64_Half)(g->named ? named[g->named - 1].ndx
                             : g->in_bss ? bss_ndx : data_ndx));
    /* File-scope asm's .global labels (_start): global functions at their
     * .text offset. Local labels stay internal — the assembler already
     * resolved jumps to them into rel32s. */
    for (struct topasm *ta = u->topasm; ta; ta = ta->next)
        for (int k = 0; k < ta->nsyms; k++)
            if (ta->syms[k].is_global)
                elfw_add_symbol(
                    w, ta->syms[k].name,
                    (Elf64_Addr)(ta->text_off + ta->syms[k].off), 0,
                    ELF64_ST_INFO(STB_GLOBAL, STT_FUNC),
                    (Elf64_Half)text_ndx);

    /* extern-declared, used, never defined: the linker's problem */
    for (struct global *g = u->globals; g; g = g->next)
        if (!g->absorbed && !g->defined && g->used)
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
        elfw_add_rela(w, text_ndx, (Elf64_Addr)ext[i].patch_off,
                      callee->sym_ndx, target_reloc_type(ta, RK_CALL),
                      target_reloc_addend(ta, RK_CALL, 0));
    }
    free(ext);

    /* File-scope asm relocations (call start_c): PLT32 against the target,
     * a function of this unit (already symboled and forced used above) or,
     * failing that, a fresh UNDEF the linker resolves. */
    for (struct topasm *ta = u->topasm; ta; ta = ta->next)
        for (int r = 0; r < ta->nrels; r++) {
            int sym = 0;
            for (struct func *f = u->funcs; f; f = f->next)
                if (!f->absorbed && f->sym_ndx &&
                    strcmp(f->name, ta->rels[r].target) == 0) {
                    sym = f->sym_ndx;
                    break;
                }
            if (!sym)
                sym = elfw_add_symbol(
                    w, ta->rels[r].target, 0, 0,
                    ELF64_ST_INFO(STB_GLOBAL, STT_NOTYPE), SHN_UNDEF);
            elfw_add_rela(w, text_ndx,
                          (Elf64_Addr)(ta->text_off + ta->rels[r].off),
                          sym, R_X86_64_PLT32, ta->rels[r].addend);
        }

    /* String addresses: PC32 against the .rodata section symbol.
     * addend = target offset - 4, because rel32 is measured from the
     * end of the instruction, four bytes past r_offset. */
    for (int i = 0; i < nstrs; i++)
        elfw_add_rela(w, text_ndx, (Elf64_Addr)strs[i].patch_off, rodata_sym,
                      target_reloc_type(ta, strs[i].kind),
                      target_reloc_addend(ta, strs[i].kind, strs[i].str_off));
    free(strs);

    /* Global-variable addresses: PC32 against the global's own symbol
     * (defined or UNDEF alike — the linker fills in either way). */
    for (int i = 0; i < ngs; i++)
        elfw_add_rela(w, text_ndx, (Elf64_Addr)gs[i].patch_off,
                      gs[i].glob->sym_ndx, target_reloc_type(ta, gs[i].kind),
                      target_reloc_addend(ta, gs[i].kind, 0));
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
            elfw_add_rela(w, g->named ? named[g->named - 1].ndx : data_ndx,
                          (Elf64_Addr)(g->off + g->relocs[i].off),
                          sym, target_reloc_type(ta, RK_ABS64), add);
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
        elfw_add_rela(w, text_ndx, (Elf64_Addr)fs[i].patch_off, tf->sym_ndx,
                      target_reloc_type(ta, fs[i].kind),
                      target_reloc_addend(ta, fs[i].kind, 0));
    }
    free(fs);

    /* -g: the DWARF relocations. Each field the emitter left zero gets an
     * absolute reloc — 8-byte .text addresses (R_X86_64_64) and 4-byte
     * section offsets (R_X86_64_32) — against the right section symbol. */
    if (want_debug) {
        for (int i = 0; i < dw.nrelocs; i++) {
            struct dwarf_reloc *r = &dw.relocs[i];
            int sym = r->target == DWTGT_TEXT   ? text_sym
                    : r->target == DWTGT_ABBREV ? dwsym[DWSEC_ABBREV]
                    :                             dwsym[DWSEC_LINE];
            elfw_add_rela(w, dwsec_ndx[r->in_sec], (Elf64_Addr)r->off, sym,
                          target_reloc_type(ta, r->width == 8 ? RK_ABS64
                                                              : RK_ABS32),
                          r->addend);
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
        case EHT_TEXT: sym = text_sym; break;
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

/* Swap `.asm` for `.o`, the default assembler output name. */
static const char *default_asm_output(const char *in)
{
    size_t n = strlen(in);
    char *out = xstrndup(in, n);        /* room for the full name + NUL */
    out[n - 3] = 'o';                   /* "....asm" -> "....o" */
    out[n - 2] = '\0';
    return out;
}

int main(int argc, char **argv)
{
    const char *input = NULL, *output = NULL;
    int compile_mode = 0, pp_only = 0;
    int lang = -1;                  /* -x: 0 C, 1 C++; -1 by suffix */

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
            printf("%s\n", target_get() == TARGET_AARCH64 ? "aarch64-elf"
                                                          : "x86_64-elf");
            return 0;
        }
        if (strcmp(argv[i], "--dump-predef") == 0) {
            dump_predef();
            return 0;
        }
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
            if (strcmp(l, "c++") == 0 || strcmp(l, "c++-cpp-output") == 0)
                lang = 1;
            else if (strcmp(l, "c") == 0 || strcmp(l, "cpp-output") == 0)
                lang = 0;
            else if (strcmp(l, "none") == 0)
                lang = -1;
            else {
                fprintf(stderr, "embcc: error: unknown language '%s' for "
                                "-x (c or c++)\n", l);
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
            }
        } else if (strcmp(argv[i], "--emit-c") == 0) {
            emit_c_only = 1;
        } else if (strcmp(argv[i], "-E") == 0) {
            pp_only = 1;
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
        } else if (strcmp(argv[i], "-fsyntax-only") == 0) {
            syntax_only = 1;
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
        } else if (strcmp(argv[i], "-Werror") == 0) {
            diag_set_werror(1);
        } else if (strcmp(argv[i], "-Wno-error") == 0) {
            diag_set_werror(0);
        } else if (strcmp(argv[i], "-w") == 0) {
            diag_set_no_warnings(1);
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
            diag_enable_warning(argv[i] + 2, 1);
        } else if (strncmp(argv[i], "-O", 2) == 0) {
            /* -O / -O1 / -O2 / -O3 enable the optimizer (one level for now);
             * -O0 turns it off. Anything else after -O is an error. */
            const char *lvl = argv[i] + 2;
            if (lvl[0] == '\0')
                opt_level = 1;
            else if (lvl[1] == '\0' && lvl[0] >= '0' && lvl[0] <= '9')
                opt_level = lvl[0] - '0';
            else {
                fprintf(stderr, "embcc: unknown optimization flag '%s'\n",
                        argv[i]);
                return 1;
            }
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
        } else if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 == argc) {
                fprintf(stderr, "embcc: -o needs a FILE\n");
                return 1;
            }
            output = argv[++i];
        } else if (has_c_suffix(argv[i]) || has_asm_suffix(argv[i]) ||
                   has_cxx_suffix(argv[i]) || has_ir_suffix(argv[i]) ||
                   (lang >= 0 && argv[i][0] != '-')) {
            if (input) {
                fprintf(stderr, "embcc: error: more than one input file "
                                "(M1: one file at a time)\n");
                return 1;
            }
            input = argv[i];
        } else {
            fprintf(stderr, "embcc: error: unknown argument '%s'\n",
                    argv[i]);
            print_usage(stderr);
            return 1;
        }
    }

    if (!input) {
        fprintf(stderr, "embcc: error: no input file\n");
        return 1;
    }
    lang_cxx = lang >= 0 ? lang : has_cxx_suffix(input);
    if (lang_cxx) {
        /* These analyses run in the C front end, over the C that C++ lowers
         * to — where a template instantiated from a header is attributed to
         * the .cc that instantiated it. A warning pointing at the wrong line
         * is worse than none, so they stay C-only until the C++ front end
         * grows its own (docs/tools/diagnostics.md T4).
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
    if (has_asm_suffix(input)) {
        if (pp_only) {
            fprintf(stderr, "embcc: error: -E does not apply to assembly\n");
            return 1;
        }
        return as_assemble(input, output ? output : default_asm_output(input),
                           AS_ELF64);
    }
    if (pp_only)
        return done(compile(input, NULL, 1));
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
    if (!compile_mode) {
        fprintf(stderr,
                "embcc: error: cannot link '%s': the integrated linker is "
                "M3 (see docs/design/roadmap.md) — compile with -c and link with "
                "the existing toolchain\n", input);
        return 1;
    }
    /* A report goes to stdout unless the caller named a file; an object
     * gets the default name. */
    if ((want_iface || want_asm) && !output)
        return done(compile(input, NULL, 0));
    return done(compile(input, output ? output : default_output(input), 0));
}
