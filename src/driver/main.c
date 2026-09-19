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
#include "../ir/ir.h"
#include "../opt/opt.h"
#include "../parse/parse.h"
#include "../sema/sema.h"
#include "../arch/target.h"
#include "util.h"

#define EMBCC_VERSION "1.0.0-m2.complete"

static void print_version(void)
{
    /* Honest: names what exists and what does not. */
    printf("EmbCC %s — C compiler for EmbLinkOS, target %s\n",
           EMBCC_VERSION, target_triple(target_get()));
    printf("Language: C11 on both targets — VLAs, _Complex, long double, "
           "_Atomic and the atomic builtins, _Generic — plus the GNU "
           "extensions EmbLinkOS uses (statement expressions, typeof, "
           "computed goto, attributes, extended inline asm).\n");
    printf("Targets (--target=): x86_64-elf (default; System V AMD64, "
           "x87 long double) and aarch64-elf (AAPCS64, binary128 long "
           "double through libgcc).\n");
    printf("C++ (.cc/.cpp/.cxx/.C, or -x c++): in progress toward C++20 "
           "with libstdc++ (docs/CXX.md) — namespaces, overloading, "
           "references, classes with constructors and destructors, "
           "new/delete, lowered through C to either target.\n");
    printf("Also: the preprocessor (-E), -O0..-O2, -g (DWARF), embas "
           "(NASM-syntax .asm, x86-64) and embld (the linker, x86-64 ELF "
           "and EMBX). Not yet: __thread, PIE, embld for aarch64 — "
           "see docs/COMPATIBILITY.md.\n");
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

static void dump_predef(void)
{
    int n;
    const struct predef_macro *tab = predef_table(&n);
    for (int i = 0; i < n; i++)
        printf("#define %s %s\n", tab[i].name, tab[i].value);
}

/* An empty but genuine relocatable object: the smallest output readelf,
 * objdump and the cross ld all accept. Kept from M0 so the writer stays
 * testable independently of the compiler. */
static int emit_empty_object(const char *path)
{
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
    FILE *f = fopen(path, "rb");
    if (!f)
        diag_fatal(path, 0, "cannot open file");
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = xmalloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n)
        diag_fatal(path, 0, "read error");
    buf[n] = 0;
    fclose(f);
    return buf;
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

static int compile(const char *in, const char *out, int pp_only)
{
    char *src = read_file(in);
    diag_register_source(in, src);   /* so diagnostics can show its lines */
    predef_set_cxx(lang_cxx);
    if (lang_cxx)
        cpp_set_cxx(cxx_has_builtin, want_exceptions);
    char *pp = cpp_process(in, src, incdirs, nincdirs);
    if (pp_only) {
        fputs(pp, stdout);
        return 0;
    }
    if (lang_cxx) {
        cxx_set_exceptions(want_exceptions);
        cxx_set_rtti(want_rtti);
        pp = cxx_translate(in, pp);
        if (emit_c_only) {
            if (out) {
                FILE *f = fopen(out, "w");
                if (!f)
                    diag_fatal(out, 0, "cannot write the file");
                fputs(pp, f);
                fclose(f);
            } else {
                fputs(pp, stdout);
            }
            return 0;
        }
    }
    struct unit *u = parse_unit(in, pp);
    sema_check(u);

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
        /* The built-in assembler is NASM/Intel x86-64 (src/as). There is no
         * aarch64 assembler yet, so file-scope asm on that target must fail
         * loudly rather than emit x86 bytes into an aarch64 image. */
        if (target_get() == TARGET_AARCH64)
            diag_fatal(in, 0,
                       "file-scope asm is not supported for aarch64 yet — "
                       "EmbCC's assembler is x86-64 NASM syntax");
        topasm_assemble(ta);
        for (int r = 0; r < ta->nrels; r++)
            for (struct func *f = u->funcs; f; f = f->next)
                if (!f->absorbed &&
                    strcmp(f->name, ta->rels[r].target) == 0)
                    f->used = 1;
    }

    struct ir_unit *iu = irgen(u);
    opt_run(iu, opt_level);

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
    /* Scanned ahead of everything else: --version and --dump-predef must
     * describe the target that was asked for, not the default. */
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--target=", 9) != 0)
            continue;
        enum target_arch a;
        if (!target_from_triple(argv[i] + 9, &a)) {
            fprintf(stderr,
                    "embcc: error: unknown target '%s' — EmbCC emits "
                    "x86_64-elf and aarch64-elf\n", argv[i] + 9);
            return 1;
        }
        target_set(a);
    }
    /* Scanned across the whole command line, not just argv[1]: these
     * describe the TARGET, so `--target=aarch64-elf --dump-predef` has to
     * mean the aarch64 table rather than an unknown-argument error. */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0) {
            print_version();
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
        } else if (strcmp(argv[i], "-fno-exceptions") == 0) {
            want_exceptions = 0;
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
        } else if (argv[i][0] == '-' && argv[i][1] == 'W') {
            /* warning options: EmbCC has one level, and its diagnostics
             * are errors, so these say nothing to it */
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
            /* A system-include directory. EmbCC keeps one search path, so
             * -isystem DIR is accepted as an -I DIR — enough to drive real
             * build scripts that pass it. */
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
            incdirs[nincdirs++] = dir;
        } else if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 == argc) {
                fprintf(stderr, "embcc: -o needs a FILE\n");
                return 1;
            }
            output = argv[++i];
        } else if (has_c_suffix(argv[i]) || has_asm_suffix(argv[i]) ||
                   has_cxx_suffix(argv[i]) ||
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
        incdirs[nincdirs++] = selfinc;
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
        return compile(input, NULL, 1);
    if (emit_c_only) {
        if (!lang_cxx) {
            fprintf(stderr, "embcc: error: --emit-c lowers C++; '%s' is C\n",
                    input);
            return 1;
        }
        return compile(input, output, 0);
    }
    if (!compile_mode) {
        fprintf(stderr,
                "embcc: error: cannot link '%s': the integrated linker is "
                "M3 (see docs/ROADMAP.md) — compile with -c and link with "
                "the existing toolchain\n", input);
        return 1;
    }
    return compile(input, output ? output : default_output(input), 0);
}
