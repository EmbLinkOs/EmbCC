#define _POSIX_C_SOURCE 200809L
/* EmbDBG — the EmbLinkOS debugger, v0: the debug-info reader and symbolizer.
 *
 * WHY THIS FIRST, AND WHY IT NEEDS NO KERNEL. A debugger answers three
 * questions in ascending cost (EMBDBG_Requirements §1): "where am I?"
 * (address <-> file:line + function), "what can I see?" (the locals in
 * scope), "what is it?" (their types). The FIRST is pure debug-info reading —
 * no live process, no ptrace, no breakpoints — so it can be built and proven
 * today, on the host, against the DWARF EmbCC already emits (steps 1-2). The
 * live-control half (breakpoints, single-step, register/memory inspect) needs
 * the kernel debug contract — CAP_DEBUG, syscalls 69-75, exception routing —
 * which myos/docs/EMBDBG_Specification.md SPECIFIES but is reserved, not yet
 * built (a kernel design, D-007). This tool is the consumer that half will sit
 * on top of, and the consumer whose real needs the native .embdbg format is
 * meant to be derived from (D-010) — DWARF is the bridge until then.
 *
 * v0 reads .symtab (function ranges) and .debug_line (address -> file:line),
 * applying an object's .rela.debug_line so a relocatable .o works too, and
 * answers:
 *     embdbg FILE funcs                 list functions with address ranges
 *     embdbg FILE lines                 dump the decoded line table
 *     embdbg FILE symbolize ADDR...     addr -> FUNC+off  FILE:LINE  (a crash
 *                                       backtrace's addresses, symbolized)
 * It is a host tool for now (full C, gcc-built, like embread); the on-OS port
 * (in-subset + EmbLinkOS syscalls, reading the native .embdbg) is the seam.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#ifndef EMBDBG_NO_MAIN
#include <unistd.h>       /* isatty/read  — interactive TUI only */
#include <termios.h>      /* raw terminal mode */
#include <sys/ioctl.h>    /* TIOCGWINSZ */
#endif

#include "../../src/elf/elf.h"
#ifndef EMBDBG_NO_MAIN
#include "remote.h"       /* the live target: a GDB remote protocol client */
#endif

/* DWARF line-program opcodes we decode. */
#define DW_LNS_copy             0x01
#define DW_LNS_advance_pc       0x02
#define DW_LNS_advance_line     0x03
#define DW_LNS_set_file         0x04
#define DW_LNS_set_column       0x05
#define DW_LNS_negate_stmt      0x06
#define DW_LNS_const_add_pc     0x08
#define DW_LNS_fixed_advance_pc 0x09
#define DW_LNE_end_sequence     0x01
#define DW_LNE_set_address      0x02

/* DIE tags / attributes / forms we read back (a subset — exactly what the
 * EmbCC emitter writes; unknown forms are skipped by their fixed size). */
#define DW_TAG_subprogram       0x2e
#define DW_TAG_formal_parameter 0x05
#define DW_TAG_variable         0x34
#define DW_TAG_base_type        0x24
#define DW_TAG_pointer_type     0x0f
#define DW_TAG_structure_type   0x13
#define DW_TAG_union_type       0x17
#define DW_TAG_array_type       0x01
#define DW_AT_name              0x03
#define DW_AT_byte_size         0x0b
#define DW_AT_encoding          0x3e
#define DW_AT_low_pc            0x11
#define DW_AT_high_pc           0x12
#define DW_AT_type              0x49
#define DW_AT_location          0x02
#define DW_FORM_addr            0x01
#define DW_FORM_block1          0x0a
#define DW_FORM_data1           0x0b
#define DW_FORM_data2           0x05
#define DW_FORM_data4           0x06
#define DW_FORM_string          0x08
#define DW_FORM_ref4            0x13
#define DW_FORM_sec_offset      0x17
#define DW_FORM_exprloc         0x18
#define DW_OP_fbreg             0x91
#define DW_OP_addr              0x03
#define DW_OP_reg0              0x50
#define DW_OP_breg0             0x70
#define DW_AT_frame_base        0x40

static void die(const char *msg) { fprintf(stderr, "embdbg: %s\n", msg); exit(1); }

/* Added to every code address the DWARF readers decode. 0 for normal tool use
 * (a .o yields .text-relative addresses, a linked image absolute ones). EmbLD
 * sets it to a debug object's FINAL .text vaddr, so a relocatable object's
 * .text-relative addresses come out absolute — the link-time producer that
 * gives .embdbg the absolute vaddrs the spec wants (§2 "producer finding"). */
static long g_addr_bias = 0;

static unsigned char *slurp(const char *path, long *len_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) die("cannot open file");
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) die("read error");
    fclose(f);
    *len_out = n;
    return b;
}

/* --- LEB128 --- */
static unsigned long uleb(const unsigned char *p, int *i)
{
    unsigned long v = 0; int s = 0; unsigned char b;
    do { b = p[(*i)++]; v |= (unsigned long)(b & 0x7f) << s; s += 7; } while (b & 0x80);
    return v;
}
static long sleb(const unsigned char *p, int *i)
{
    long v = 0; int s = 0; unsigned char b;
    do { b = p[(*i)++]; v |= (long)(b & 0x7f) << s; s += 7; } while (b & 0x80);
    if (s < 64 && (b & 0x40)) v |= -(1L << s);
    return v;
}
static unsigned long u16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned long u32(const unsigned char *p)
{ return (unsigned long)p[0] | (p[1] << 8) | ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24); }
static unsigned long u64(const unsigned char *p)
{ return u32(p) | ((unsigned long)u32(p + 4) << 32); }

/* --- the loaded image --- */
struct sec { const char *name; unsigned char *data; unsigned long size, off; unsigned type, link, info, entsize; };
struct func { const char *name; unsigned long addr, size; };
struct row  { unsigned long addr; int file, line; int end; };

/* .debug_info DIEs, decoded to what an inspector needs. */
struct dtype { unsigned long off; int is_ptr; const char *name; int size; int enc; unsigned long pointee;
               int agg; /* 1 struct, 2 union, 3 array: printed as bytes */ };
struct dvar  { const char *name; int is_param; unsigned long type_off; long fbreg; int has_loc; };
/* fb_reg: the DWARF register DW_AT_frame_base names (-1: none read), with
 * fb_off added -- or, fb_isreg, the register's value as it stands. gone[]:
 * the variables the compiler says are optimized out here. */
struct dfunc { const char *name; unsigned long lo, hi; struct dvar *vars; int nvars;
               int fb_reg, fb_isreg; long fb_off; const char **gone; int ngone; };
/* A global variable: its name, its address as DW_OP_addr gives it (on AVR
 * a data-space address, 0x800000 up), and its type. */
struct dglob { const char *name; unsigned long addr, type_off; };

struct img {
    unsigned char *b; long len;
    struct sec *sec; int nsec;
    struct func *fn; int nfn;
    struct row *rows; int nrows;
    char **files; int nfiles;   /* file_names[1..], index 1-based in DWARF */
    struct dtype *types; int ntypes;
    struct dfunc *dfn; int ndfn;
    struct dglob *globs; int nglobs;
};

static struct sec *find_sec(struct img *m, const char *name)
{
    for (int i = 0; i < m->nsec; i++)
        if (strcmp(m->sec[i].name, name) == 0) return &m->sec[i];
    return NULL;
}

/* ELFCLASS32, which the embedded targets are. Read field by field into
 * the 64-bit shapes the rest of this file uses, because Elf32_Shdr and
 * Elf32_Sym are not their 64-bit namesakes narrowed -- Elf32_Sym puts
 * st_value and st_size BEFORE st_info, where Elf64_Sym puts them after.
 * The linker does the same conversion at its own input boundary. */
static int img_is32(struct img *m) { return m->b[4] == ELFCLASS32; }

static void load_sections(struct img *m)
{
    if (m->len < 20 || memcmp(m->b, "\177ELF", 4) != 0)
        die("not an ELF file");
    int n, shstrndx;
    unsigned long shoff;
    if (img_is32(m)) {
        Elf32_Ehdr *e = (Elf32_Ehdr *)m->b;
        shoff = e->e_shoff; n = e->e_shnum; shstrndx = e->e_shstrndx;
    } else {
        Elf64_Ehdr *e = (Elf64_Ehdr *)m->b;
        shoff = (unsigned long)e->e_shoff; n = e->e_shnum;
        shstrndx = e->e_shstrndx;
    }
    if (n == 0) {
        /* A stripped image: no sections at all. Say so where it will be
         * read, rather than crashing on a null string table. */
        m->nsec = 0;
        return;
    }
    m->nsec = n;
    m->sec = calloc((size_t)n, sizeof *m->sec);
    if (img_is32(m)) {
        Elf32_Shdr *sh = (Elf32_Shdr *)(m->b + shoff);
        const char *shstr = (const char *)(m->b + sh[shstrndx].sh_offset);
        for (int i = 0; i < n; i++) {
            m->sec[i].name = shstr + sh[i].sh_name;
            m->sec[i].data = m->b + sh[i].sh_offset;
            m->sec[i].size = sh[i].sh_size;
            m->sec[i].off = sh[i].sh_offset;
            m->sec[i].type = sh[i].sh_type;
            m->sec[i].link = sh[i].sh_link;
            m->sec[i].info = sh[i].sh_info;
            m->sec[i].entsize = sh[i].sh_entsize;
        }
        return;
    }
    Elf64_Shdr *sh = (Elf64_Shdr *)(m->b + shoff);
    const char *shstr = (const char *)(m->b + sh[shstrndx].sh_offset);
    for (int i = 0; i < n; i++) {
        m->sec[i].name = shstr + sh[i].sh_name;
        m->sec[i].data = m->b + sh[i].sh_offset;
        m->sec[i].size = sh[i].sh_size;
        m->sec[i].off = sh[i].sh_offset;
        m->sec[i].type = sh[i].sh_type;
        m->sec[i].link = sh[i].sh_link;
        m->sec[i].info = sh[i].sh_info;
        m->sec[i].entsize = sh[i].sh_entsize;
    }
}

/* What DW_AT_frame_base names on this machine. EmbCC's DWARF makes every
 * location a DW_OP_fbreg offset from the frame base, and printing it as
 * `rbp` was true of the only target that existed when the printer was
 * written. It is `x29` on aarch64, `r7` on ARM and `s0` on RISC-V, and a
 * debugger that says rbp on a Cortex-M is telling the reader to look at
 * a register the machine does not have. */
static const char *frame_base_name(struct img *m)
{
    Elf64_Ehdr *e = (Elf64_Ehdr *)m->b;
    switch (e->e_machine) {
    case 183: return "x29";     /* EM_AARCH64 */
    case 40:  return "r7";      /* EM_ARM */
    case 243: return "s0";      /* EM_RISCV */
    case 83:  return "Y";       /* EM_AVR: r28:r29 */
    default:  return "rbp";
    }
}

static void load_funcs(struct img *m)
{
    struct sec *st = find_sec(m, ".symtab");
    if (!st) return;
    struct sec *strt = &m->sec[st->link];
    int wide = !img_is32(m);
    int entsz = wide ? (int)sizeof(Elf64_Sym) : (int)sizeof(Elf32_Sym);
    int n = (int)(st->size / (unsigned long)entsz);
    m->fn = calloc((size_t)n, sizeof *m->fn);
    for (int i = 0; i < n; i++) {
        const char *name;
        unsigned long value, size;
        unsigned char info;
        if (wide) {
            Elf64_Sym *sy = &((Elf64_Sym *)st->data)[i];
            name = (const char *)strt->data + sy->st_name;
            value = (unsigned long)sy->st_value;
            size = (unsigned long)sy->st_size;
            info = sy->st_info;
        } else {
            Elf32_Sym *sy = &((Elf32_Sym *)st->data)[i];
            name = (const char *)strt->data + sy->st_name;
            value = sy->st_value;
            size = sy->st_size;
            info = sy->st_info;
        }
        if (ELF64_ST_TYPE(info) != STT_FUNC || size == 0)
            continue;
        /* A Thumb function symbol's st_value carries the interworking
         * bit, which is not part of the address: leaving it on puts
         * every function one byte past where it is and no pc ever falls
         * inside one. */
        m->fn[m->nfn].name = name;
        m->fn[m->nfn].addr = (value & ~1UL) + (unsigned long)g_addr_bias;
        m->fn[m->nfn].size = size;
        m->nfn++;
    }
}

/* Relocations against a debug section: a relocatable .o writes an address
 * field as 0 and carries the real .text offset in the addend. Map a field's
 * section-offset -> addend so the reader recovers the address. Used for
 * .debug_line's set_address and .debug_info's low_pc/high_pc. */
static unsigned long reloc_lookup(struct img *m, const char *rela, unsigned long off, int *found)
{
    struct sec *r = find_sec(m, rela);
    *found = 0;
    if (!r) return 0;
    /* An ELF32 object's entries are Elf32_Rela -- twelve bytes, not the
     * twenty-four of Elf64_Rela -- and reading them as the wider form
     * found no field of a 32-bit object's DWARF at all. */
    if (img_is32(m)) {
        Elf32_Rela *rel = (Elf32_Rela *)r->data;
        int n = (int)(r->size / sizeof *rel);
        for (int i = 0; i < n; i++)
            if (rel[i].r_offset == off) {
                *found = 1;
                return (unsigned long)(long)rel[i].r_addend;
            }
        return 0;
    }
    Elf64_Rela *rel = (Elf64_Rela *)r->data;
    int n = (int)(r->size / sizeof *rel);
    for (int i = 0; i < n; i++)
        if (rel[i].r_offset == off) { *found = 1; return (unsigned long)rel[i].r_addend; }
    return 0;
}

/* An address field of `size` bytes: two, four or eight. Not always eight:
 * a linked 32-bit image keeps four-byte addresses in place (an object has
 * zeros there and the value in a relocation), and reading eight took the
 * next field in as the address's high half. */
static unsigned long addr_field(const unsigned char *p, int size)
{
    if (size == 2) return u16(p);
    if (size == 4) return u32(p);
    return u64(p);
}

static void add_row(struct img *m, unsigned long addr, int file, int line, int end)
{
    m->rows = realloc(m->rows, (size_t)(m->nrows + 1) * sizeof *m->rows);
    m->rows[m->nrows].addr = addr;
    m->rows[m->nrows].file = file;
    m->rows[m->nrows].line = line;
    m->rows[m->nrows].end = end;
    m->nrows++;
}

/* Every line-number program in the section -- one per compilation unit,
 * which a linked image has several of. Each unit's file table is appended
 * to the image's, and its rows' file numbers moved up past the units
 * before it: a second unit used to REPLACE the first one's files, so
 * every row of the first named a file of the last. */
static void decode_lines(struct img *m)
{
    struct sec *ls = find_sec(m, ".debug_line");
    if (!ls) return;
    const unsigned char *p = ls->data;
    int i = 0, n = (int)ls->size;

    m->nfiles = 1; m->files = calloc(1, sizeof(char *));  /* index 0 unused */
    while (i + 4 <= n) {
        int unit_start = i;
        unsigned long ulen = u32(p + i); i += 4;
        int unit_end = unit_start + 4 + (int)ulen;
        if (ulen == 0 || unit_end > n) break;
        unsigned ver = (unsigned)u16(p + i); i += 2;
        if (ver < 2 || ver > 4) { i = unit_end; continue; }  /* DWARF 5: not ours */
        unsigned long hlen = u32(p + i); i += 4;
        int prog = i + (int)hlen;
        unsigned min_inst = p[i++];
        if (ver >= 4) i++;                 /* maximum_operations_per_instruction */
        i++;                               /* default_is_stmt */
        int line_base = (signed char)p[i++];
        unsigned line_range = p[i++];
        unsigned opcode_base = p[i++];
        const unsigned char *std_len = p + i;
        i += (int)opcode_base - 1;
        while (p[i]) {                     /* include_directories */
            while (p[i]) i++;
            i++;
        }
        i++;
        int fbase = m->nfiles - 1;         /* this unit's file 1 is fbase + 1 */
        while (p[i]) {
            const char *name = (const char *)(p + i);
            while (p[i]) i++;
            i++;
            (void)uleb(p, &i); (void)uleb(p, &i); (void)uleb(p, &i); /* dir,mtime,size */
            m->files = realloc(m->files, (size_t)(m->nfiles + 1) * sizeof(char *));
            m->files[m->nfiles++] = (char *)name;
        }
        i = prog;

        unsigned long addr = 0; int file = 1, line = 1;
        while (i < unit_end) {
            unsigned op = p[i++];
            if (op == 0) {                 /* extended */
                int len = (int)uleb(p, &i);
                int nexti = i + len;
                unsigned sub = p[i++];
                if (sub == DW_LNE_set_address) {
                    int found;
                    unsigned long a = reloc_lookup(m, ".rela.debug_line", (unsigned long)i, &found);
                    addr = found ? (unsigned long)((long)a + g_addr_bias)
                                 : addr_field(p + i, len - 1);
                } else if (sub == DW_LNE_end_sequence) {
                    add_row(m, addr, fbase + file, line, 1);
                    addr = 0; file = 1; line = 1;
                }
                i = nexti;
            } else if (op < opcode_base) {
                switch (op) {
                case DW_LNS_copy:            add_row(m, addr, fbase + file, line, 0); break;
                case DW_LNS_advance_pc:      addr += uleb(p, &i) * min_inst; break;
                case DW_LNS_advance_line:    line += (int)sleb(p, &i); break;
                case DW_LNS_set_file:        file = (int)uleb(p, &i); break;
                case DW_LNS_set_column:      (void)uleb(p, &i); break;
                case DW_LNS_negate_stmt:     break;
                case DW_LNS_const_add_pc:    addr += ((255 - opcode_base) / line_range) * min_inst; break;
                case DW_LNS_fixed_advance_pc: addr += u16(p + i); i += 2; break;
                default:                     /* skip unknown, per std_len */
                    for (int a = 0; a < std_len[op - 1]; a++) (void)uleb(p, &i);
                }
            } else {                         /* special opcode */
                unsigned adj = op - opcode_base;
                addr += (adj / line_range) * min_inst;
                line += line_base + (int)(adj % line_range);
                add_row(m, addr, fbase + file, line, 0);
            }
        }
        i = unit_end;
    }
}

/* --- .debug_info + .debug_abbrev: functions with their params/locals, the
 * unit's global variables, and the types EmbCC describes. Scoped to the
 * forms the emitter writes, over EVERY unit in the section (a linked image
 * has one per object): each unit's abbreviations are its own, and a
 * DW_FORM_ref4 counts from the start of its unit. --- */
struct abbrev { int tag, children; int at[24], form[24]; int n; };

static void read_abbrevs(const struct sec *as, unsigned long at, struct abbrev *ab)
{
    const unsigned char *p = as->data;
    int i = (int)at, n = (int)as->size;
    memset(ab, 0, 64 * sizeof *ab);
    while (i < n) {
        int code = (int)uleb(p, &i);
        if (code == 0) break;              /* end of this unit's table */
        if (code >= 64) return;
        ab[code].tag = (int)uleb(p, &i);
        ab[code].children = p[i++];
        for (;;) {
            int at2 = (int)uleb(p, &i), form = (int)uleb(p, &i);
            if (at2 == 0 && form == 0) break;
            if (ab[code].n < 24) {
                ab[code].at[ab[code].n] = at2;
                ab[code].form[ab[code].n] = form;
                ab[code].n++;
            }
        }
    }
}

static void decode_info(struct img *m)
{
    struct sec *is = find_sec(m, ".debug_info");
    struct sec *as = find_sec(m, ".debug_abbrev");
    if (!is || !as) return;

    struct abbrev ab[64];
    const unsigned char *p = is->data;
    int n = (int)is->size, cu = 0;

    while (cu + 11 <= n) {
        unsigned long ulen = u32(p + cu);
        int end = cu + 4 + (int)ulen;
        unsigned ver = (unsigned)u16(p + cu + 4);
        if (ulen == 0 || end > n) break;
        if (ver < 2 || ver > 4) { cu = end; continue; }   /* DWARF 5: not ours */
        int found;
        unsigned long aoff = reloc_lookup(m, ".rela.debug_info",
                                          (unsigned long)(cu + 6), &found);
        if (!found) aoff = u32(p + cu + 6);
        int asz = p[cu + 10];
        read_abbrevs(as, aoff, ab);

        int i = cu + 11, depth = 0;
        struct dfunc *cur = NULL;
        while (i < end) {
            int die_off = i;               /* a ref4 is cu + its value */
            int code = (int)uleb(p, &i);
            if (code == 0) {               /* end of a DIE's children */
                if (--depth < 2) cur = NULL;
                if (depth <= 0) break;
                continue;
            }
            if (code >= 64 || ab[code].tag == 0) { i = end; break; }
            struct abbrev *a = &ab[code];

            const char *name = NULL;
            unsigned long low = 0, high = 0, tref = 0, gaddr = 0;
            long fb = 0, fboff = 0;
            int hasloc = 0, emptyloc = 0, gloc = 0, size = 0, enc = 0;
            int fbreg = -1, fbisreg = 0;
            for (int k = 0; k < a->n; k++) {
                int at = a->at[k], form = a->form[k];
                unsigned long secoff = (unsigned long)i;
                switch (form) {
                case DW_FORM_string: {
                    const char *s = (const char *)(p + i);
                    while (p[i]) i++;
                    i++;
                    if (at == DW_AT_name) name = s;
                    break; }
                case DW_FORM_data1: {
                    int v = p[i++];
                    if (at == DW_AT_byte_size) size = v;
                    else if (at == DW_AT_encoding) enc = v;
                    break; }
                case DW_FORM_data2:                    i += 2; break;
                case DW_FORM_data4: {
                    unsigned long v = u32(p + i); i += 4;
                    if (at == DW_AT_byte_size) size = (int)v;
                    break; }
                case DW_FORM_sec_offset:               i += 4; break;
                case DW_FORM_ref4: {
                    unsigned long v = u32(p + i); i += 4;
                    if (at == DW_AT_type) tref = (unsigned long)cu + v;
                    break; }
                case DW_FORM_addr: {
                    int fnd;
                    unsigned long v = reloc_lookup(m, ".rela.debug_info", secoff, &fnd);
                    if (fnd) v = (unsigned long)((long)v + g_addr_bias);
                    else v = addr_field(p + i, asz);
                    i += asz;
                    if (at == DW_AT_low_pc) low = v;
                    else if (at == DW_AT_high_pc) high = v;
                    break; }
                case DW_FORM_exprloc: {
                    int len = (int)uleb(p, &i), st = i;
                    if (at == DW_AT_location && len == 0) {
                        emptyloc = 1;      /* optimized out */
                    } else if (at == DW_AT_location && p[st] == DW_OP_fbreg) {
                        int j = st + 1; fb = sleb(p, &j); hasloc = 1;
                    } else if (at == DW_AT_location && p[st] == DW_OP_addr &&
                               len == 1 + asz) {
                        int fnd;
                        unsigned long v = reloc_lookup(m, ".rela.debug_info",
                                                       (unsigned long)(st + 1), &fnd);
                        gaddr = fnd ? v : addr_field(p + st + 1, asz);
                        gloc = 1;
                    } else if (at == DW_AT_frame_base && len >= 1) {
                        /* DW_OP_regN: the register's value is the base;
                         * DW_OP_bregN off: the register plus off. */
                        int op = p[st], j = st + 1;
                        if (op >= DW_OP_reg0 && op <= DW_OP_reg0 + 31) {
                            fbreg = op - DW_OP_reg0; fbisreg = 1;
                        } else if (op >= DW_OP_breg0 && op <= DW_OP_breg0 + 31) {
                            fbreg = op - DW_OP_breg0; fboff = sleb(p, &j);
                        }
                    }
                    i = st + len;
                    break; }
                case DW_FORM_block1: { int len = p[i++]; i += len; break; }
                default: i = end; break;   /* unknown form: cannot size safely */
                }
            }

            if (a->tag == DW_TAG_base_type || a->tag == DW_TAG_pointer_type ||
                a->tag == DW_TAG_structure_type || a->tag == DW_TAG_union_type ||
                a->tag == DW_TAG_array_type) {
                m->types = realloc(m->types, (size_t)(m->ntypes + 1) * sizeof *m->types);
                struct dtype *t = &m->types[m->ntypes++];
                memset(t, 0, sizeof *t);
                t->off = (unsigned long)die_off;
                t->is_ptr = (a->tag == DW_TAG_pointer_type);
                t->agg = a->tag == DW_TAG_structure_type ? 1
                       : a->tag == DW_TAG_union_type ? 2
                       : a->tag == DW_TAG_array_type ? 3 : 0;
                t->name = name;
                t->size = t->is_ptr && !size ? 8 : size;
                t->enc = enc;
                t->pointee = tref;
            } else if (a->tag == DW_TAG_subprogram) {
                m->dfn = realloc(m->dfn, (size_t)(m->ndfn + 1) * sizeof *m->dfn);
                cur = &m->dfn[m->ndfn++];
                memset(cur, 0, sizeof *cur);
                cur->name = name; cur->lo = low; cur->hi = high;
                cur->fb_reg = fbreg; cur->fb_off = fboff; cur->fb_isreg = fbisreg;
            } else if ((a->tag == DW_TAG_formal_parameter || a->tag == DW_TAG_variable)
                       && cur && depth >= 2 && emptyloc && name) {
                cur->gone = realloc(cur->gone, (size_t)(cur->ngone + 1) * sizeof *cur->gone);
                cur->gone[cur->ngone++] = name;
            } else if ((a->tag == DW_TAG_formal_parameter || a->tag == DW_TAG_variable)
                       && cur && depth >= 2 && hasloc) {
                cur->vars = realloc(cur->vars, (size_t)(cur->nvars + 1) * sizeof *cur->vars);
                struct dvar *v = &cur->vars[cur->nvars++];
                v->name = name;
                v->is_param = (a->tag == DW_TAG_formal_parameter);
                v->type_off = tref;
                v->fbreg = fb;
                v->has_loc = hasloc;
            } else if (a->tag == DW_TAG_variable && depth == 1 && gloc && name) {
                m->globs = realloc(m->globs, (size_t)(m->nglobs + 1) * sizeof *m->globs);
                m->globs[m->nglobs].name = name;
                m->globs[m->nglobs].addr = gaddr;
                m->globs[m->nglobs].type_off = tref;
                m->nglobs++;
            }
            if (a->children) depth++;
        }
        cu = end;
    }
}

/* A type's spelling, following a pointer one level to its pointee's name. */
static const char *type_name(struct img *m, unsigned long off)
{
    static char buf[128];
    if (off == 0) return "?";
    for (int i = 0; i < m->ntypes; i++) {
        if (m->types[i].off != off) continue;
        struct dtype *t = &m->types[i];
        if (!t->is_ptr) return t->name ? t->name : "?";
        const char *pn = t->pointee ? type_name(m, t->pointee) : "void";
        snprintf(buf, sizeof buf, "%s *", pn);
        return buf;
    }
    return "?";
}

static const struct dfunc *dfunc_at(struct img *m, unsigned long addr)
{
    for (int i = 0; i < m->ndfn; i++)
        if (addr >= m->dfn[i].lo && addr < m->dfn[i].hi) return &m->dfn[i];
    return NULL;
}

static const struct func *func_at(struct img *m, unsigned long addr)
{
    for (int i = 0; i < m->nfn; i++)
        if (addr >= m->fn[i].addr && addr < m->fn[i].addr + m->fn[i].size)
            return &m->fn[i];
    return NULL;
}

/* The last row at or before addr within a sequence (rows are in address order
 * per sequence; end-markers bound a sequence and never match). */
static const struct row *line_at(struct img *m, unsigned long addr)
{
    const struct row *best = NULL;
    for (int i = 0; i < m->nrows; i++) {
        if (m->rows[i].end) { continue; }
        if (m->rows[i].addr <= addr &&
            (i + 1 >= m->nrows || addr < m->rows[i + 1].addr))
            best = &m->rows[i];
    }
    return best;
}

static const char *file_name(struct img *m, int idx)
{
    if (idx >= 1 && idx < m->nfiles) return m->files[idx];
    return "?";
}

/* ===================================================================== *
 * Disassembler (EmbDBG spec §4.8). A native x86-64 decoder, scoped to the
 * instruction repertoire EmbCC's correct-and-slow codegen actually emits
 * (every value through rax, ModRM memory operands off rbp, the integer set,
 * control flow, and the SSE scalars for float). Correct instruction LENGTHS
 * are the load-bearing property — get a length wrong and every later
 * instruction desyncs — so unknown bytes stop as `.byte`, never guess. AT&T
 * syntax, to read like objdump. Mixed source+asm via the line table.
 * ===================================================================== */

#include "../../src/arch/x86_64/disasm.h"

/* Bytes + the source file/line a range of code belongs to. Requires an ELF
 * with a .text section (a .o, or any object carrying code) — a .embdbg holds
 * no machine code. */
static const unsigned char *text_bytes(struct img *m, unsigned long *size)
{
    struct sec *t = find_sec(m, ".text");
    if (!t) return NULL;
    *size = t->size;
    return t->data;
}

static void cmd_disassemble(struct img *m, int argc, char **argv)
{
    if (argc < 1) die("disassemble needs a function name or address");
    unsigned long size = 0;
    const unsigned char *text = text_bytes(m, &size);
    if (!text) die("no .text to disassemble (need the object/ELF, not the .embdbg)");

    unsigned long lo = 0, hi = 0;
    const struct func *f = NULL;
    for (int i = 0; i < m->nfn; i++)
        if (strcmp(m->fn[i].name, argv[0]) == 0) f = &m->fn[i];
    if (f) { lo = f->addr; hi = f->addr + f->size; }
    else {
        lo = strtoul(argv[0], NULL, 0);
        hi = lo + (argc >= 2 ? strtoul(argv[1], NULL, 0) : 32);
    }
    if (hi > size) hi = size;

    printf("%s:\n", f ? f->name : "range");
    int last_line = -1;
    for (unsigned long a = lo; a < hi; ) {
        const struct row *r = line_at(m, a);
        if (r && r->line != last_line) {          /* mixed source+asm */
            last_line = r->line;
            printf("\033[2m; %s:%d\033[0m\n", file_name(m, r->file), r->line);
        }
        char text_s[128];
        int len = embdbg_decode_one(text + a, (int)(hi - a), a, text_s);
        printf("  %6lx:\t", a);
        for (int b = 0; b < len; b++) printf("%02x ", text[a + b]);
        for (int b = len; b < 8; b++) printf("   ");
        printf("\t%s\n", text_s);
        a += (unsigned long)len;
    }
}

static void cmd_funcs(struct img *m)
{
    printf("%-24s %-10s %s\n", "FUNCTION", "ADDR", "SIZE");
    for (int i = 0; i < m->nfn; i++)
        printf("%-24s 0x%-8lx %lu\n", m->fn[i].name, m->fn[i].addr, m->fn[i].size);
}

static void cmd_lines(struct img *m)
{
    printf("%-12s %-20s %s\n", "ADDRESS", "FILE", "LINE");
    for (int i = 0; i < m->nrows; i++) {
        if (m->rows[i].end) { printf("0x%-10lx %-20s (end)\n", m->rows[i].addr, ""); continue; }
        printf("0x%-10lx %-20s %d\n", m->rows[i].addr,
               file_name(m, m->rows[i].file), m->rows[i].line);
    }
}

static void print_source(const char *path, int line, int ctx);

static void print_frame(struct img *m, unsigned long addr)
{
    const struct func *f = func_at(m, addr);
    const struct row *r = line_at(m, addr);
    if (f) printf("%s+0x%lx", f->name, addr - f->addr);
    else   printf("??");
    if (r) printf("  %s:%d", file_name(m, r->file), r->line);
    else   printf("  (no line info)");
}

static void cmd_symbolize(struct img *m, int argc, char **argv)
{
    for (int a = 0; a < argc; a++) {
        unsigned long addr = strtoul(argv[a], NULL, 0);
        printf("0x%lx  ", addr);
        print_frame(m, addr);
        printf("\n");
    }
}

/* A symbolized backtrace: the caller chain a kernel fault handler collects by
 * walking the rbp links is just a list of return addresses — name each. */
static void cmd_backtrace(struct img *m, int argc, char **argv)
{
    for (int a = 0; a < argc; a++) {
        unsigned long addr = strtoul(argv[a], NULL, 0);
        printf("#%-2d 0x%lx  ", a, addr);
        print_frame(m, addr);
        printf("\n");
    }
}

static void list_vars(struct img *m, const struct dfunc *d)
{
    for (int i = 0; i < d->nvars; i++) {
        struct dvar *v = &d->vars[i];
        printf("    %-5s %-14s %-8s @ %s%+ld\n",
               v->is_param ? "param" : "local",
               type_name(m, v->type_off), v->name,
               frame_base_name(m), v->fbreg);
    }
}

/* "where am I + what can I see": symbolize the address, then the params and
 * locals in scope at that function, each with its type and frame slot. */
static void cmd_where(struct img *m, int argc, char **argv)
{
    if (argc < 1) die("where needs an address");
    unsigned long addr = strtoul(argv[0], NULL, 0);
    printf("0x%lx  ", addr);
    print_frame(m, addr);
    printf("\n");
    const struct row *r = line_at(m, addr);
    if (r) print_source(file_name(m, r->file), r->line, 2);
    const struct dfunc *d = dfunc_at(m, addr);
    if (!d) { printf("    (no scope info here)\n"); return; }
    printf("  in %s — %d variable(s) in scope:\n", d->name, d->nvars);
    list_vars(m, d);
}

/* Print a window of source around `line`, the current line marked. The path
 * is DW_AT_name as EmbCC recorded it (the path given on its command line);
 * open it relative to the cwd, and say so plainly if it is not reachable. */
static void print_source(const char *path, int line, int ctx)
{
    FILE *f = fopen(path, "r");
    if (!f) { printf("    (source '%s' not reachable from here)\n", path); return; }
    char buf[1024];
    int ln = 0;
    while (fgets(buf, sizeof buf, f)) {
        ln++;
        if (ln > line + ctx) break;
        if (ln >= line - ctx) {
            buf[strcspn(buf, "\n")] = 0;
            printf("  %s %4d | %s\n", ln == line ? "->" : "  ", ln, buf);
        }
    }
    fclose(f);
}

/* Source-context view: symbolize the address, then show the source lines
 * around it with the current line marked — the modern "you are here". */
static void cmd_list(struct img *m, int argc, char **argv)
{
    if (argc < 1) die("list needs an address");
    unsigned long addr = strtoul(argv[0], NULL, 0);
    printf("0x%lx  ", addr);
    print_frame(m, addr);
    printf("\n");
    const struct row *r = line_at(m, addr);
    if (r) print_source(file_name(m, r->file), r->line, 2);
    else   printf("    (no line info)\n");
}

static void cmd_info(struct img *m, int argc, char **argv)
{
    if (argc < 1) die("info needs a function name");
    for (int i = 0; i < m->ndfn; i++)
        if (m->dfn[i].name && strcmp(m->dfn[i].name, argv[0]) == 0) {
            struct dfunc *d = &m->dfn[i];
            printf("%s  [0x%lx, 0x%lx)  %d variable(s):\n",
                   d->name, d->lo, d->hi, d->nvars);
            list_vars(m, d);
            return;
        }
    printf("embdbg: no function '%s' with debug info\n", argv[0]);
}

/* ===================================================================== *
 * Crash analyzer (EmbDBG spec §5). Turns a kernel fault dump into a
 * diagnosis, fully offline: exception + faulting address, a register dump, a
 * SYMBOLIZED stack trace (walking the rbp chain through the dumped stack
 * words), the locals in scope at the crash, and the instructions around RIP.
 *
 * Input: a simple, deterministic text crash report a fault handler can print.
 *   exception <NAME>
 *   fault <hexaddr>            faulting address (cr2 for a page fault); optional
 *   reg <name> <hexval>        rip/rsp/rbp/rax/... — one per line
 *   mem <hexaddr> <hexu64>     a stack word (LE u64 at addr), enough of the
 *                              rbp chain to unwind; the handler dumps the words
 *                              it walks, or a region as these lines
 * Code addresses (rip, return addresses) share the binary's address space;
 * stack addresses (rsp/rbp/mem) are their own. FILE supplies symbols/locals
 * (and .text for the disassembly window).
 * ===================================================================== */
struct crash {
    char exc[64]; int have_fault; unsigned long fault;
    char rname[40][8]; unsigned long rval[40]; int nreg;
    unsigned long maddr[256], mval[256]; int nmem;
};
static unsigned long crash_reg(struct crash *c, const char *n, int *ok)
{
    for (int i = 0; i < c->nreg; i++)
        if (strcmp(c->rname[i], n) == 0) { if (ok) *ok = 1; return c->rval[i]; }
    if (ok) *ok = 0;
    return 0;
}
static unsigned long crash_mem(struct crash *c, unsigned long a, int *ok)
{
    for (int i = 0; i < c->nmem; i++)
        if (c->maddr[i] == a) { if (ok) *ok = 1; return c->mval[i]; }
    if (ok) *ok = 0;
    return 0;
}
static void parse_crash(const char *path, struct crash *c)
{
    memset(c, 0, sizeof *c);
    FILE *f = fopen(path, "r");
    if (!f) die("cannot open crash report");
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char k[32], a[64], b[64];
        int nf = sscanf(line, "%31s %63s %63s", k, a, b);
        if (nf < 1 || k[0] == '#') continue;
        if (strcmp(k, "exception") == 0 && nf >= 2) {
            strncpy(c->exc, a, sizeof c->exc - 1);
        } else if (strcmp(k, "fault") == 0 && nf >= 2) {
            c->have_fault = 1; c->fault = strtoul(a, NULL, 0);
        } else if (strcmp(k, "reg") == 0 && nf >= 3 && c->nreg < 40) {
            strncpy(c->rname[c->nreg], a, 7);
            c->rval[c->nreg++] = strtoul(b, NULL, 0);
        } else if (strcmp(k, "mem") == 0 && nf >= 3 && c->nmem < 256) {
            c->maddr[c->nmem] = strtoul(a, NULL, 0);
            c->mval[c->nmem++] = strtoul(b, NULL, 0);
        }
    }
    fclose(f);
}

/* Symbolize a code address inline: "func+off  file:line". */
static void print_loc(struct img *m, unsigned long a)
{
    const struct func *f = func_at(m, a);
    const struct row *r = line_at(m, a);
    if (f) printf("%s+0x%lx", f->name, a - f->addr); else printf("??");
    if (r) printf("  %s:%d", file_name(m, r->file), r->line);
}

static void cmd_crash(struct img *m, int argc, char **argv)
{
    if (argc < 1) die("crash needs a report file");
    struct crash c; parse_crash(argv[0], &c);
    int ok;
    unsigned long rip = crash_reg(&c, "rip", &ok);
    unsigned long rbp = crash_reg(&c, "rbp", &ok);

    printf("=== EmbDBG Crash Analysis ===\n");
    printf("Exception: %s", c.exc[0] ? c.exc : "(unknown)");
    if (c.have_fault) printf("    faulting address 0x%lx", c.fault);
    printf("\n");
    printf("RIP: 0x%lx  ", rip); print_loc(m, rip); printf("\n\n");

    printf("Registers:\n");
    for (int i = 0; i < c.nreg; i++) {
        printf("  %-4s 0x%016lx", c.rname[i], c.rval[i]);
        if ((i % 3) == 2) printf("\n");
    }
    if (c.nreg % 3) printf("\n");
    printf("\n");

    /* Stack trace: frame 0 is RIP; walk the rbp chain through the dumped
     * words. return addr at *(rbp+8), caller rbp at *(rbp). Stop when a word
     * is missing, rbp doesn't advance, or a return address is in no function. */
    printf("Backtrace:\n");
    printf("  #0  0x%lx  ", rip); print_loc(m, rip); printf("\n");
    unsigned long fp = rbp;
    for (int depth = 1; depth < 64; depth++) {
        int okr, okf;
        unsigned long ret = crash_mem(&c, fp + 8, &okr);
        unsigned long caller = crash_mem(&c, fp, &okf);
        if (!okr || !ret) break;
        if (!func_at(m, ret)) { printf("  #%-2d 0x%lx  ??\n", depth, ret); break; }
        printf("  #%-2d 0x%lx  ", depth, ret); print_loc(m, ret); printf("\n");
        if (!okf || caller <= fp) break;      /* chain must climb */
        fp = caller;
    }
    printf("\n");

    /* Locals in scope at the crash frame. */
    const struct dfunc *d = dfunc_at(m, rip);
    if (d) {
        printf("Locals at #0 (%s):\n", d->name);
        list_vars(m, d);
        printf("\n");
    }

    /* Instructions around RIP (a window in the faulting function). */
    unsigned long tsize = 0;
    const unsigned char *text = text_bytes(m, &tsize);
    const struct func *f = func_at(m, rip);
    if (text && f) {
        printf("Near RIP:\n");
        unsigned long addrs[4096]; char texts[4096][80]; int lens[4096], nins = 0;
        for (unsigned long a = f->addr; a < f->addr + f->size && nins < 4096; ) {
            char t[128];
            int len = embdbg_decode_one(text + a, (int)(f->addr + f->size - a), a, t);
            addrs[nins] = a; lens[nins] = len;
            snprintf(texts[nins], sizeof texts[nins], "%.79s", t);
            nins++; a += (unsigned long)len;
        }
        int at = -1;
        for (int i = 0; i < nins; i++) if (addrs[i] == rip) { at = i; break; }
        int lo = at < 0 ? 0 : (at - 4 < 0 ? 0 : at - 4);
        int hi = at < 0 ? (nins < 9 ? nins : 9) : (at + 5 > nins ? nins : at + 5);
        for (int i = lo; i < hi; i++) {
            printf("  %s %6lx:\t", addrs[i] == rip ? "->" : "  ", addrs[i]);
            for (int b = 0; b < lens[i]; b++) printf("%02x ", text[addrs[i] + b]);
            for (int b = lens[i]; b < 8; b++) printf("   ");
            printf("\t%s\n", texts[i]);
        }
    }
}

/* ===================================================================== *
 * Native .embdbg — myos/docs/EMBDBG_Specification.md v1.
 *
 * The owned debug format (D-010: DWARF is the bridge, .embdbg the destination).
 * EmbDBG is the consumer the spec's byte layout was derived to serve, so the
 * honest proof is a round trip: this tool WRITES .embdbg from the DWARF it
 * parsed, then READS it back into the same model, and every command produces
 * identical output either way. Byte-exact, little-endian, deterministic; the
 * header carries a SHA-256 build_id and CRC32C checksums exactly as the spec
 * (and EMBX) require. Addresses are whatever the input holds — absolute for a
 * linked image, .text-relative for a .o; the link-time producer yields the
 * absolute form the spec's kernel symbolizer wants.
 * ===================================================================== */

static const unsigned char EMBDBG_MAGIC[8] = { 0x7F,0x45,0x4D,0x44,0x42,0x47,0x0A,0x1A };
#define DBG_KIND_STRTAB 1
#define DBG_KIND_FILES  2
#define DBG_KIND_LINE   3
#define DBG_KIND_FUNCS  4
#define DBG_KIND_FRAME  5
#define DBG_KIND_VARS   6
#define DBG_KIND_TYPES  7
#define LN_STMT         0x1000
#define LOC_FBREG       1
#define VAR_PARAM       0x1

/* CRC32C (Castagnoli) — the header/section integrity check the spec mandates. */
static unsigned long crc32c(const unsigned char *p, long n)
{
    unsigned long crc = 0xFFFFFFFFUL;
    for (long i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0x82F63B78UL & (unsigned long)(-(long)(crc & 1)));
        crc &= 0xFFFFFFFFUL;
    }
    return (crc ^ 0xFFFFFFFFUL) & 0xFFFFFFFFUL;
}

/* SHA-256 (FIPS 180-4) — the 32-byte build_id that binds .embdbg to its image. */
struct sha { unsigned int h[8]; unsigned long long total; unsigned char buf[64]; int n; };
static unsigned int rotr(unsigned int x, int c) { return (x >> c) | (x << (32 - c)); }
static void sha_block(struct sha *s, const unsigned char *p)
{
    static const unsigned int K[64] = {
      0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
      0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
      0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
      0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
      0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
      0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
      0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
      0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
    unsigned int w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (p[i*4] << 24) | (p[i*4+1] << 16) | (p[i*4+2] << 8) | p[i*4+3];
    for (int i = 16; i < 64; i++) {
        unsigned int s0 = rotr(w[i-15],7) ^ rotr(w[i-15],18) ^ (w[i-15] >> 3);
        unsigned int s1 = rotr(w[i-2],17) ^ rotr(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    unsigned int a=s->h[0],b=s->h[1],c=s->h[2],d=s->h[3],e=s->h[4],f=s->h[5],g=s->h[6],hh=s->h[7];
    for (int i = 0; i < 64; i++) {
        unsigned int S1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
        unsigned int ch = (e & f) ^ (~e & g);
        unsigned int t1 = hh + S1 + ch + K[i] + w[i];
        unsigned int S0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
        unsigned int maj = (a & b) ^ (a & c) ^ (b & c);
        unsigned int t2 = S0 + maj;
        hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    s->h[0]+=a; s->h[1]+=b; s->h[2]+=c; s->h[3]+=d;
    s->h[4]+=e; s->h[5]+=f; s->h[6]+=g; s->h[7]+=hh;
}
static void sha256(const unsigned char *data, long n, unsigned char out[32])
{
    struct sha s;
    s.h[0]=0x6a09e667; s.h[1]=0xbb67ae85; s.h[2]=0x3c6ef372; s.h[3]=0xa54ff53a;
    s.h[4]=0x510e527f; s.h[5]=0x9b05688c; s.h[6]=0x1f83d9ab; s.h[7]=0x5be0cd19;
    long i = 0;
    for (; i + 64 <= n; i += 64) sha_block(&s, data + i);
    unsigned char tail[128]; int t = (int)(n - i);
    memcpy(tail, data + i, (size_t)t);
    tail[t++] = 0x80;
    int pad = (t <= 56) ? 56 - t : 120 - t;
    memset(tail + t, 0, (size_t)pad); t += pad;
    unsigned long long bits = (unsigned long long)n * 8;
    for (int k = 0; k < 8; k++) tail[t + k] = (unsigned char)(bits >> (56 - 8*k));
    t += 8;
    for (int j = 0; j < t; j += 64) sha_block(&s, tail + j);
    for (int k = 0; k < 8; k++) {
        out[k*4]   = (unsigned char)(s.h[k] >> 24);
        out[k*4+1] = (unsigned char)(s.h[k] >> 16);
        out[k*4+2] = (unsigned char)(s.h[k] >> 8);
        out[k*4+3] = (unsigned char)(s.h[k]);
    }
}

/* Exposed for EmbLD's EMBX emitter (embdbg_core.h): the same SHA-256 that
 * stamps a .embdbg build_id stamps an EMBX build_id — one hash, one discipline. */
void embdbg_sha256(const unsigned char *data, long n, unsigned char out[32])
{
    sha256(data, n, out);
}

/* --- little-endian output buffer --- */
struct ob { unsigned char *p; long n, cap; };
static void ob_need(struct ob *b, long k) {
    if (b->n + k > b->cap) {
        b->cap = b->cap ? b->cap*2 : 256;
        while (b->n + k > b->cap) b->cap *= 2;
        b->p = realloc(b->p, (size_t)b->cap);
    }
}
static void ob_u8(struct ob *b, unsigned v){ ob_need(b,1); b->p[b->n++]=(unsigned char)v; }
static void ob_u16(struct ob *b, unsigned v){ ob_u8(b,v); ob_u8(b,v>>8); }
static void ob_u32(struct ob *b, unsigned long v){ ob_u16(b,(unsigned)v); ob_u16(b,(unsigned)(v>>16)); }
static void ob_u64(struct ob *b, unsigned long v){ ob_u32(b,v & 0xffffffffUL); ob_u32(b,(v>>32)&0xffffffffUL); }
static void ob_bytes(struct ob *b, const void *p, long k){ ob_need(b,k); memcpy(b->p+b->n,p,(size_t)k); b->n+=k; }

/* STRTAB builder with dedup (deterministic: first-referenced order). */
struct strtab { struct ob b; };
static unsigned long str_intern(struct strtab *s, const char *str)
{
    if (!str) str = "";
    if (s->b.n == 0) ob_u8(&s->b, 0);          /* offset 0 = "" */
    for (long i = 0; i < s->b.n; ) {
        const char *e = (const char *)(s->b.p + i);
        if (strcmp(e, str) == 0) return (unsigned long)i;
        i += (long)strlen(e) + 1;
    }
    unsigned long off = (unsigned long)s->b.n;
    ob_bytes(&s->b, str, (long)strlen(str) + 1);
    return off;
}

/* Flatten a DWARF-parsed type into the TYPES blob, returning its byte offset
 * (0 = void). Pointee first, so a T_POINTER references an existing entry. */
static unsigned long types_emit(struct img *m, struct ob *tb, struct strtab *st,
                                unsigned long *map_from, unsigned long *map_to,
                                int *nmap, unsigned long dwoff)
{
    if (dwoff == 0) return 0;
    for (int i = 0; i < *nmap; i++) if (map_from[i] == dwoff) return map_to[i];
    struct dtype *t = NULL;
    for (int i = 0; i < m->ntypes; i++) if (m->types[i].off == dwoff) { t = &m->types[i]; break; }
    if (!t) return 0;
    unsigned long off;
    if (t->is_ptr) {
        unsigned long tgt = types_emit(m, tb, st, map_from, map_to, nmap, t->pointee);
        if (tb->n == 0) ob_u8(tb, 0);          /* reserve offset 0 for void */
        off = (unsigned long)tb->n;
        ob_u8(tb, 2);                            /* T_POINTER */
        ob_u32(tb, tgt);
    } else {
        if (tb->n == 0) ob_u8(tb, 0);
        off = (unsigned long)tb->n;
        /* DWARF DW_ATE -> spec encoding: 1 signed,2 unsigned,3 float,5 char,6 uchar */
        int enc = t->enc == 4 ? 3 : t->enc == 6 ? 5 : t->enc == 8 ? 6
                : t->enc == 7 ? 2 : 1;
        ob_u8(tb, 1);                            /* T_BASE */
        ob_u8(tb, (unsigned)enc);
        ob_u8(tb, (unsigned)t->size);
        ob_u32(tb, str_intern(st, t->name));
    }
    map_from[*nmap] = dwoff; map_to[*nmap] = off; (*nmap)++;
    return off;
}

static int cmp_rows(const void *a, const void *b)
{
    unsigned long x = ((const struct row *)a)->addr, y = ((const struct row *)b)->addr;
    return x < y ? -1 : x > y ? 1 : 0;
}

static void write_embdbg(struct img *m, const char *path,
                         const unsigned char *bid_src, long bid_len)
{
    struct strtab st; memset(&st, 0, sizeof st);
    struct ob files = {0,0,0}, line = {0,0,0}, funcs = {0,0,0},
              frame = {0,0,0}, vars = {0,0,0}, types = {0,0,0};

    /* FILES: DWARF file index f (1-based) -> row f-1. */
    for (int i = 1; i < m->nfiles; i++) {
        ob_u32(&files, str_intern(&st, m->files[i]));
        ob_u32(&files, 0);                       /* dir_str (empty) */
        for (int k = 0; k < 32; k++) ob_u8(&files, 0);  /* content_hash */
    }
    int filecount = m->nfiles > 0 ? m->nfiles - 1 : 0;

    /* LINE: 16-byte rows, ascending by addr. */
    struct row *sorted = malloc((size_t)(m->nrows ? m->nrows : 1) * sizeof *sorted);
    memcpy(sorted, m->rows, (size_t)m->nrows * sizeof *sorted);
    qsort(sorted, (size_t)m->nrows, sizeof *sorted, cmp_rows);
    for (int i = 0; i < m->nrows; i++) {
        ob_u64(&line, sorted[i].addr);
        ob_u32(&line, sorted[i].end ? 0 : (unsigned long)sorted[i].line);
        ob_u16(&line, sorted[i].end ? 0 : (unsigned)(sorted[i].file - 1));
        ob_u16(&line, sorted[i].end ? 0 : LN_STMT);
    }
    free(sorted);

    /* TYPES + VARS + FRAME + FUNCS (VARS ordered by function). */
    unsigned long mf[256], mt[256]; int nmap = 0;
    int var_run = 0;
    for (int fi = 0; fi < m->ndfn; fi++) {
        struct dfunc *d = &m->dfn[fi];
        int first = var_run;
        for (int v = 0; v < d->nvars; v++) {
            struct dvar *dv = &d->vars[v];
            unsigned long toff = nmap < 250
                ? types_emit(m, &types, &st, mf, mt, &nmap, dv->type_off) : 0;
            ob_u32(&vars, str_intern(&st, dv->name));
            ob_u32(&vars, toff);
            ob_u64(&vars, d->lo);                /* scope_lo = function bounds */
            ob_u64(&vars, d->hi);                /* scope_hi */
            ob_u8(&vars, LOC_FBREG);
            ob_u8(&vars, dv->is_param ? VAR_PARAM : 0);
            for (int k = 0; k < 6; k++) ob_u8(&vars, 0);
            ob_u64(&vars, (unsigned long)dv->fbreg);
            var_run++;
        }
        /* FRAME: one per function. prologue_end = first line row past low_pc. */
        unsigned long pe = 0;
        for (int i = 0; i < m->nrows; i++)
            if (!m->rows[i].end && m->rows[i].addr >= d->lo && m->rows[i].addr < d->hi) {
                pe = m->rows[i].addr - d->lo; break;
            }
        ob_u32(&frame, pe);
        ob_u32(&frame, (unsigned long)(d->hi - d->lo));  /* epilogue_start_off */
        ob_u8(&frame, 0);                        /* FRAME_RBP */
        ob_u8(&frame, 0);
        ob_u16(&frame, 0);
        ob_u32(&frame, 0);
        /* FUNCS */
        ob_u64(&funcs, d->lo);
        ob_u64(&funcs, d->hi);
        ob_u32(&funcs, str_intern(&st, d->name));
        ob_u32(&funcs, (unsigned long)fi);       /* frame_idx */
        ob_u32(&funcs, (unsigned long)first);    /* first_var */
        ob_u32(&funcs, (unsigned long)d->nvars); /* var_count */
    }

    /* Assemble sections (kind, entsize, count, body). */
    struct { int kind; unsigned entsize, count; struct ob *body; } S[7] = {
        { DBG_KIND_STRTAB, 0, 0, &st.b },
        { DBG_KIND_FILES, 40, (unsigned)filecount, &files },
        { DBG_KIND_LINE,  16, (unsigned)m->nrows, &line },
        { DBG_KIND_FUNCS, 32, (unsigned)m->ndfn, &funcs },
        { DBG_KIND_FRAME, 16, (unsigned)m->ndfn, &frame },
        { DBG_KIND_VARS,  40, (unsigned)var_run, &vars },
        { DBG_KIND_TYPES, 0, 0, &types },
    };
    int nsec = 7;
    long table_off = 64;
    long body_off = table_off + (long)nsec * 24;

    struct ob out = {0,0,0};
    /* header (0x00..0x3F) */
    ob_bytes(&out, EMBDBG_MAGIC, 8);
    ob_u16(&out, 1);                             /* format_version */
    ob_u16(&out, 64);                            /* header_size */
    ob_u32(&out, 0);                             /* flags */
    unsigned char bid[32]; sha256(bid_src, bid_len, bid);
    ob_bytes(&out, bid, 32);                     /* build_id = SHA-256(image) */
    ob_u16(&out, (unsigned)nsec);
    ob_u16(&out, 0);                             /* reserved */
    ob_u32(&out, (unsigned long)table_off);
    long fsize_pos = out.n; ob_u32(&out, 0);     /* file_size (patched) */
    long hcrc_pos = out.n; ob_u32(&out, 0);      /* header_checksum (patched) */

    /* section table + running body offsets */
    long cur = body_off;
    long body_start = out.n + (long)nsec * 24;   /* where bodies begin in file */
    (void)body_start;
    for (int i = 0; i < nsec; i++) {
        long sz = S[i].body->n;
        ob_u16(&out, (unsigned)S[i].kind);
        ob_u16(&out, 0);                         /* flags */
        ob_u32(&out, S[i].entsize);
        ob_u32(&out, S[i].count);
        ob_u32(&out, (unsigned long)cur);        /* offset */
        ob_u32(&out, (unsigned long)sz);         /* size */
        ob_u32(&out, crc32c(S[i].body->p, sz));  /* checksum */
        cur += sz;
    }
    for (int i = 0; i < nsec; i++) ob_bytes(&out, S[i].body->p, S[i].body->n);

    /* patch file_size, then header_checksum over 0x00..0x3B with the field 0 */
    unsigned long fsz = (unsigned long)out.n;
    out.p[fsize_pos]=(unsigned char)fsz; out.p[fsize_pos+1]=(unsigned char)(fsz>>8);
    out.p[fsize_pos+2]=(unsigned char)(fsz>>16); out.p[fsize_pos+3]=(unsigned char)(fsz>>24);
    unsigned long hc = crc32c(out.p, 60);
    out.p[hcrc_pos]=(unsigned char)hc; out.p[hcrc_pos+1]=(unsigned char)(hc>>8);
    out.p[hcrc_pos+2]=(unsigned char)(hc>>16); out.p[hcrc_pos+3]=(unsigned char)(hc>>24);

    FILE *f = fopen(path, "wb");
    if (!f) die("cannot write .embdbg");
    fwrite(out.p, 1, (size_t)out.n, f);
    fclose(f);
    free(out.p); free(st.b.p); free(files.p); free(line.p);
    free(funcs.p); free(frame.p); free(vars.p); free(types.p);
}

/* Read a type (and its chain) out of the TYPES blob into m->types, keyed by
 * its blob offset so type_name() resolves it the same as the DWARF path. */
static void read_type(struct img *m, const unsigned char *tb, long tn,
                      const char *strtab, unsigned long off)
{
    if (off == 0 || (long)off >= tn) return;
    for (int i = 0; i < m->ntypes; i++) if (m->types[i].off == off) return;
    int tag = tb[off];
    m->types = realloc(m->types, (size_t)(m->ntypes+1) * sizeof *m->types);
    struct dtype *t = &m->types[m->ntypes++];
    memset(t, 0, sizeof *t);
    t->off = off;
    if (tag == 2) {                              /* T_POINTER */
        unsigned long tgt = u32(tb + off + 1);
        t->is_ptr = 1; t->size = 8; t->pointee = tgt;
        read_type(m, tb, tn, strtab, tgt);
    } else if (tag == 1) {                        /* T_BASE */
        int enc = tb[off+1]; t->size = tb[off+2];
        unsigned long ns = u32(tb + off + 3);
        t->name = strtab + ns;
        t->enc = enc;
    }
}

static void load_embdbg(struct img *m)
{
    const unsigned char *b = m->b;
    unsigned nsec = u16(b + 0x30);
    unsigned long tab = u32(b + 0x34);
    const char *strtab = NULL; const unsigned char *typesb = NULL; long typesn = 0;
    const unsigned char *filesb=NULL,*lineb=NULL,*funcsb=NULL,*varsb=NULL;
    unsigned filec=0, linec=0, funcc=0, varc=0;
    for (unsigned i = 0; i < nsec; i++) {
        const unsigned char *e = b + tab + i*24;
        int kind = u16(e); unsigned count = u32(e+8);
        unsigned long off = u32(e+12), size = u32(e+16);
        const unsigned char *body = b + off;
        if (kind == DBG_KIND_STRTAB) strtab = (const char *)body;
        else if (kind == DBG_KIND_FILES) { filesb = body; filec = count; }
        else if (kind == DBG_KIND_LINE)  { lineb = body; linec = count; }
        else if (kind == DBG_KIND_FUNCS) { funcsb = body; funcc = count; }
        else if (kind == DBG_KIND_VARS)  { varsb = body; varc = count; (void)varc; }
        else if (kind == DBG_KIND_TYPES) { typesb = body; typesn = (long)size; }
    }
    /* files: FILES row i -> m->files[i+1] (1-based, matching file_name). */
    m->nfiles = (int)filec + 1;
    m->files = calloc((size_t)m->nfiles, sizeof(char *));
    for (unsigned i = 0; i < filec; i++)
        m->files[i+1] = (char *)(strtab + u32(filesb + i*40));
    /* lines */
    for (unsigned i = 0; i < linec; i++) {
        const unsigned char *r = lineb + i*16;
        unsigned long addr = u64(r); unsigned line = (unsigned)u32(r+8);
        unsigned file = u16(r+12);
        add_row(m, addr, line ? (int)file + 1 : 0, (int)line, line ? 0 : 1);
    }
    /* funcs + their vars + coarse symtab-equivalent */
    m->dfn = calloc((size_t)(funcc ? funcc : 1), sizeof *m->dfn);
    m->fn  = calloc((size_t)(funcc ? funcc : 1), sizeof *m->fn);
    for (unsigned i = 0; i < funcc; i++) {
        const unsigned char *e = funcsb + i*32;
        unsigned long lo = u64(e), hi = u64(e+8);
        const char *nm = strtab + u32(e+16);
        unsigned first = u32(e+24), vc = u32(e+28);
        struct dfunc *d = &m->dfn[m->ndfn++];
        d->name = nm; d->lo = lo; d->hi = hi; d->nvars = (int)vc; d->vars = NULL;
        d->fb_reg = -1;
        if (vc) d->vars = calloc((size_t)vc, sizeof *d->vars);
        for (unsigned v = 0; v < vc; v++) {
            const unsigned char *r = varsb + (first+v)*40;
            struct dvar *dv = &d->vars[v];
            dv->name = strtab + u32(r);
            dv->type_off = u32(r+4);
            dv->fbreg = (long)u64(r+32);
            dv->is_param = (r[0x19] & VAR_PARAM) ? 1 : 0;
            dv->has_loc = 1;
            if (typesb) read_type(m, typesb, typesn, strtab, dv->type_off);
        }
        struct func *cf = &m->fn[m->nfn++];
        cf->name = nm; cf->addr = lo; cf->size = hi - lo;
    }
}

/* Validate a .embdbg's structural integrity: magic, the header CRC32C, and
 * every section's CRC32C — the stale-/corrupt-info trap the spec builds in. */
static void cmd_verify(struct img *m)
{
    const unsigned char *b = m->b;
    if (m->len < 64 || memcmp(b, EMBDBG_MAGIC, 8) != 0) die("not a .embdbg");
    int ok = 1;
    unsigned long hc = u32(b + 0x3c), calc = crc32c(b, 60);
    printf("magic            OK\n");
    printf("header_checksum  %s\n", hc == calc ? "OK" : "BAD");
    if (hc != calc) ok = 0;
    unsigned nsec = u16(b + 0x30);
    unsigned long tab = u32(b + 0x34);
    for (unsigned i = 0; i < nsec; i++) {
        const unsigned char *e = b + tab + i*24;
        int kind = u16(e);
        unsigned long off = u32(e+12), size = u32(e+16), csum = u32(e+20);
        unsigned long c = crc32c(b + off, (long)size);
        printf("section kind %-2d  %s\n", kind, c == csum ? "OK" : "BAD");
        if (c != csum) ok = 0;
    }
    printf("build_id         ");
    for (int i = 0; i < 32; i++) printf("%02x", b[0x10 + i]);
    printf("\n");
    if (!ok) { printf("VERIFY FAILED\n"); exit(1); }
    printf("verify OK\n");
}

/* ===================================================================== *
 * TUI — an interactive browser over the debug info. A function list on the
 * left, a detail pane on the right (signature, source, typed locals). It is
 * static inspection (no live process — that is the kernel-gated half), so it
 * browses what .embdbg/DWARF hold. When stdout is not a terminal it prints a
 * plain full dump instead, so it stays scriptable and testable.
 * ===================================================================== */

/* The source file and 1-based line span of a function, from its line rows. */
static int func_span(struct img *m, const struct dfunc *d,
                     const char **file, int *lo, int *hi)
{
    int found = 0; *lo = 1 << 30; *hi = 0; *file = NULL;
    for (int i = 0; i < m->nrows; i++) {
        if (m->rows[i].end) continue;
        if (m->rows[i].addr >= d->lo && m->rows[i].addr < d->hi) {
            found = 1;
            if (m->rows[i].line < *lo) *lo = m->rows[i].line;
            if (m->rows[i].line > *hi) *hi = m->rows[i].line;
            if (!*file) *file = file_name(m, m->rows[i].file);
        }
    }
    return found;
}

/* Build the right-pane detail for a function into `lines`, returning count. */
static int build_detail(struct img *m, const struct dfunc *d,
                        char lines[][256], int maxlines)
{
    int n = 0;
    if (n < maxlines)
        snprintf(lines[n++], 256, "%s   [0x%lx, 0x%lx)", d->name, d->lo, d->hi);
    const char *file; int lo, hi;
    if (func_span(m, d, &file, &lo, &hi) && file) {
        if (n < maxlines) snprintf(lines[n++], 256, "%s:%d", file, lo);
        FILE *f = fopen(file, "r");
        if (f) {
            char buf[512]; int ln = 0;
            while (fgets(buf, sizeof buf, f) && n < maxlines) {
                ln++;
                if (ln < lo || ln > hi) continue;
                buf[strcspn(buf, "\n")] = 0;
                snprintf(lines[n++], 256, "  %4d | %.240s", ln, buf);
            }
            fclose(f);
        }
    }
    if (n < maxlines) lines[n++][0] = 0;
    if (n < maxlines) snprintf(lines[n++], 256, "%d variable(s):", d->nvars);
    for (int v = 0; v < d->nvars && n < maxlines; v++) {
        struct dvar *dv = &d->vars[v];
        snprintf(lines[n++], 256, "  %-5s %-12s %-8s @ %s%+ld",
                 dv->is_param ? "param" : "local",
                 type_name(m, dv->type_off), dv->name,
                 frame_base_name(m), dv->fbreg);
    }
    return n;
}

static void tui_plain(struct img *m)
{
    printf("EmbDBG — %d function(s)\n", m->ndfn);
    for (int i = 0; i < m->ndfn; i++) {
        char lines[256][256];
        int n = build_detail(m, &m->dfn[i], lines, 256);
        printf("\n== %s ==\n", m->dfn[i].name);
        for (int j = 1; j < n; j++) printf("%s\n", lines[j]);
    }
}

#ifndef EMBDBG_NO_MAIN   /* interactive TUI — raw mode, needs termios/ioctl.
                            Excluded when embdbg.c is linked into embld as the
                            link-time .embdbg emitter (embdbg_emit_objects). */
static struct termios g_oldt;
static int g_raw = 0;
static void raw_off(void)
{
    if (g_raw) { tcsetattr(0, TCSANOW, &g_oldt); g_raw = 0;
                 printf("\033[?25h\033[0m\033[2J\033[H"); fflush(stdout); }
}
static void raw_on(void)
{
    tcgetattr(0, &g_oldt);
    struct termios t = g_oldt;
    t.c_lflag &= ~(ICANON | ECHO);
    t.c_cc[VMIN] = 1; t.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &t);
    g_raw = 1; atexit(raw_off);
    printf("\033[?25l");
}

/* ======================================================================
 * Rich multi-panel TUI (VS/CLion-style): a Call-Stack / Functions list, a
 * Source + Assembly center, and a Registers + Variables right column, over a
 * loaded image (+ an optional crash dump for the live-ish register/stack/value
 * data). Panel navigation, incremental search, and a ':' command palette.
 * Falls back to tui_plain() when stdout is not a tty (scriptable).
 * ==================================================================== */

/* One rendered screen cell-line: text + an optional ANSI attribute for the
 * whole line (selected row, current source line, faulting insn, rip reg). */
struct cell { char t[256]; const char *a; };
static void cell_set(struct cell *c, const char *attr, const char *fmt, ...)
{
    c->a = attr;
    va_list ap; va_start(ap, fmt);
    vsnprintf(c->t, sizeof c->t, fmt, ap);
    va_end(ap);
}
static void put_cell(const struct cell *c, int w)
{
    if (c->a) fputs(c->a, stdout);
    int n = 0;
    for (const char *p = c->t; *p && n < w; p++, n++) putchar(*p);
    for (; n < w; n++) putchar(' ');
    if (c->a) fputs("\033[0m", stdout);
}

/* A one-file source-line cache (the panel only ever shows one file at a time). */
static char  g_src_path[512];
static char *g_src_lines[16384];
static int   g_src_n;
static void src_load(const char *path)
{
    if (path && g_src_path[0] && strcmp(path, g_src_path) == 0) return;   /* cached */
    for (int i = 0; i < g_src_n; i++) free(g_src_lines[i]);
    g_src_n = 0; g_src_path[0] = 0;
    if (!path) return;
    FILE *f = fopen(path, "r");
    if (!f) return;
    strncpy(g_src_path, path, sizeof g_src_path - 1);
    char buf[1024];
    while (fgets(buf, sizeof buf, f) && g_src_n < 16384) {
        buf[strcspn(buf, "\n")] = 0;
        g_src_lines[g_src_n++] = strdup(buf);
    }
    fclose(f);
}

struct tframe { unsigned long addr, rbp; };

static void tui_interactive(struct img *m, struct crash *cr, int crash_avail)
{
    struct winsize ws; int W = 100, H = 30;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 10 && ws.ws_col > 60) { W = ws.ws_col; H = ws.ws_row; }
    if (W > 240) W = 240;
    int stackW = (W < 110) ? 18 : 26;
    int regW   = (W < 110) ? 30 : 42;
    int centerW = W - stackW - regW - 2;
    if (centerW < 24) { centerW = 24; regW = W - stackW - centerW - 2; if (regW < 10) regW = 10; }
    int bh = H - 2;                         /* body rows (title + status take 2) */

    /* Unwind the crash into frames {return addr, that frame's rbp} -- same walk
     * cmd_crash does, but keeping each frame's rbp so Variables can read values
     * for ANY selected frame, not just #0. */
    struct tframe fr[80]; int nfr = 0;
    if (crash_avail) {
        int ok; unsigned long rip = crash_reg(cr, "rip", &ok), rbp = crash_reg(cr, "rbp", &ok);
        fr[nfr].addr = rip; fr[nfr].rbp = rbp; nfr++;
        unsigned long fp = rbp;
        for (int d = 1; d < 80; d++) {
            int okr, okf;
            unsigned long ret = crash_mem(cr, fp + 8, &okr), cal = crash_mem(cr, fp, &okf);
            if (!okr || !ret) break;
            fr[nfr].addr = ret; fr[nfr].rbp = (okf ? cal : 0); nfr++;
            if (!func_at(m, ret)) break;
            if (!okf || cal <= fp) break;
            fp = cal;
        }
    }

    int mode_crash = crash_avail;           /* Tab toggles when a crash is loaded */
    int sel = 0;
    char search[64] = {0}; int searching = 0;
    char palette[128] = {0}; int in_palette = 0;
    char status[160] = {0};
    int *vis = malloc((size_t)(m->ndfn ? m->ndfn : 1) * sizeof(int));

    raw_on();
    for (;;) {
        int nvis = 0;
        for (int i = 0; i < m->ndfn; i++)
            if (!search[0] || (m->dfn[i].name && strstr(m->dfn[i].name, search)))
                vis[nvis++] = i;
        int nlist = mode_crash ? nfr : nvis;
        if (sel >= nlist) sel = nlist ? nlist - 1 : 0;
        if (sel < 0) sel = 0;

        /* current address + this frame's rbp (for variable values). */
        unsigned long cur = 0, cur_rbp = 0;
        if (mode_crash) { if (nfr) { cur = fr[sel].addr; cur_rbp = fr[sel].rbp; } }
        else if (nvis)  { cur = m->dfn[vis[sel]].lo; }

        const struct func  *cf = func_at(m, cur);
        const struct row   *cl = line_at(m, cur);
        const struct dfunc *cd = dfunc_at(m, cur);
        const char *cfile = cl ? file_name(m, cl->file) : NULL;
        int cline = cl ? cl->line : 0;

        struct cell *L = calloc((size_t)bh, sizeof *L);
        struct cell *C = calloc((size_t)bh, sizeof *C);
        struct cell *R = calloc((size_t)bh, sizeof *R);

        /* ---- LEFT: call stack (crash) or function list (browse) ---- */
        cell_set(&L[0], "\033[7m", mode_crash ? " CALL STACK" : " FUNCTIONS");
        int lrows = bh - 1, ltop = 0;
        if (sel >= lrows) ltop = sel - lrows + 1;
        for (int r = 1; r < bh; r++) {
            int i = ltop + (r - 1);
            const char *attr = (i == sel) ? "\033[7m" : NULL;
            if (mode_crash) {
                if (i < nfr) { const struct func *ff = func_at(m, fr[i].addr);
                    cell_set(&L[r], attr, " #%d %s", i, ff ? ff->name : "??"); }
            } else if (i < nvis) {
                cell_set(&L[r], attr, " %s", m->dfn[vis[i]].name ? m->dfn[vis[i]].name : "?");
            }
        }

        /* ---- CENTER: SOURCE (top) + ASSEMBLY (bottom) ---- */
        int srcH = bh / 2;
        cell_set(&C[0], "\033[7m", " SOURCE  %s:%d", cfile ? cfile : "(no source)", cline);
        src_load(cfile);
        int swin = srcH - 1;
        int sstart = cline > 0 ? cline - swin / 2 : 1; if (sstart < 1) sstart = 1;
        for (int r = 1; r < srcH; r++) {
            int ln = sstart + (r - 1);
            if (ln >= 1 && ln <= g_src_n)
                cell_set(&C[r], ln == cline ? "\033[1;7m" : "\033[2m",
                         " %4d %s", ln, g_src_lines[ln - 1]);
        }
        cell_set(&C[srcH], "\033[7m", " ASSEMBLY  %s", cf ? cf->name : "(no func)");
        unsigned long tsz = 0; const unsigned char *text = text_bytes(m, &tsz);
        if (text && cf) {
            static unsigned long ad[16384]; static char tx[16384][72]; int nins = 0;
            for (unsigned long a = cf->addr; a < cf->addr + cf->size && nins < 16384; ) {
                char t[128]; int l = embdbg_decode_one(text + a, (int)(cf->addr + cf->size - a), a, t);
                ad[nins] = a; snprintf(tx[nins], sizeof tx[0], "%.71s", t); nins++;
                a += (unsigned long)l;
            }
            int at = -1; for (int i = 0; i < nins; i++) if (ad[i] == cur) { at = i; break; }
            int awin = bh - srcH - 1;
            int astart = at < 0 ? 0 : (at - awin / 2 < 0 ? 0 : at - awin / 2);
            for (int r = srcH + 1; r < bh; r++) {
                int i = astart + (r - srcH - 1);
                if (i < nins)
                    cell_set(&C[r], ad[i] == cur ? "\033[1;7m" : "\033[2m",
                             " %6lx  %s", ad[i], tx[i]);
            }
        } else {
            cell_set(&C[srcH + 1], "\033[2m", "  (.text unavailable -- pass the ELF/.o, not the .embdbg)");
        }

        /* ---- RIGHT: REGISTERS (top) + VARIABLES (bottom) ---- */
        int regH = bh / 2;
        cell_set(&R[0], "\033[7m", " REGISTERS");
        if (mode_crash) {
            for (int r = 1; r < regH; r++) { int i = r - 1;
                if (i < cr->nreg)
                    cell_set(&R[r], strcmp(cr->rname[i], "rip") == 0 ? "\033[1m" : NULL,
                             " %-4s 0x%016lx", cr->rname[i], cr->rval[i]);
            }
        } else {
            cell_set(&R[1], "\033[2m", " (no crash dump -- browse mode)");
        }
        cell_set(&R[regH], "\033[7m", " VARIABLES  %s", cd ? cd->name : "");
        if (cd) {
            for (int r = regH + 1; r < bh; r++) { int v = r - regH - 1;
                if (v < cd->nvars) {
                    struct dvar *dv = &cd->vars[v];
                    char val[40] = "";
                    if (mode_crash && cur_rbp) {
                        int okv; unsigned long a = cur_rbp + (unsigned long)dv->fbreg;
                        unsigned long x = crash_mem(cr, a, &okv);
                        if (okv) snprintf(val, sizeof val, " = 0x%lx", x);
                        else     snprintf(val, sizeof val, " = ?");
                    }
                    cell_set(&R[r], NULL, " %-5s %-8s %s%s",
                             dv->is_param ? "param" : "local",
                             type_name(m, dv->type_off), dv->name, val);
                }
            }
        }

        /* ---- paint ---- */
        printf("\033[H");
        const char *modestr = mode_crash ? (cr->exc[0] ? cr->exc : "crash") : "browse";
        char right_sc[128];
        snprintf(right_sc, sizeof right_sc,
                 "[Tab] %s  [j/k] sel  [/] search  [:] cmd  [q] quit ",
                 mode_crash ? "browse" : "crash");
        char title[288];
        int tl = snprintf(title, sizeof title, " EmbDBG \342\226\270 %s ", modestr);
        printf("\033[7m%s", title);
        for (int i = tl; i < W - (int)strlen(right_sc); i++) putchar(' ');
        printf("%s\033[0m\n", right_sc);

        for (int r = 0; r < bh; r++) {
            put_cell(&L[r], stackW); printf("\033[2m\342\224\202\033[0m");
            put_cell(&C[r], centerW); printf("\033[2m\342\224\202\033[0m");
            put_cell(&R[r], regW); printf("\n");
        }
        if (in_palette)      printf("\033[7m :%s \033[0m", palette);
        else if (searching)  printf("\033[7m /%s \033[0m", search);
        else printf("\033[2m %s \033[0m",
                    status[0] ? status
                    : (mode_crash ? "stack frame view -- j/k selects a frame, values follow"
                                  : "function browser -- j/k, / to search, : for commands"));
        fflush(stdout);
        free(L); free(C); free(R);

        /* ---- input ---- */
        unsigned char ch; if (read(0, &ch, 1) != 1) break;
        if (in_palette) {
            if (ch == '\r' || ch == '\n') {
                in_palette = 0; status[0] = 0;
                if (strcmp(palette, "q") == 0) break;
                else if (palette[0] == '0' && palette[1] == 'x') {
                    unsigned long a = strtoul(palette, NULL, 0);
                    for (int i = 0; i < m->ndfn; i++)
                        if (a >= m->dfn[i].lo && a < m->dfn[i].hi) {
                            mode_crash = 0; search[0] = 0; sel = i;
                            snprintf(status, sizeof status, "goto 0x%lx -> %s", a, m->dfn[i].name);
                            break;
                        }
                } else if (palette[0]) {
                    for (int i = 0; i < m->ndfn; i++)
                        if (m->dfn[i].name && strcmp(m->dfn[i].name, palette) == 0) {
                            mode_crash = 0; search[0] = 0; sel = i;
                            snprintf(status, sizeof status, "jump to %s", palette); break;
                        }
                }
                palette[0] = 0;
            } else if (ch == 27) { in_palette = 0; palette[0] = 0; }
            else if (ch == 127 || ch == 8) { int l = (int)strlen(palette); if (l) palette[l-1] = 0; }
            else if (ch >= 32 && ch < 127) { int l = (int)strlen(palette);
                if (l < (int)sizeof palette - 1) { palette[l] = (char)ch; palette[l+1] = 0; } }
            continue;
        }
        if (searching) {
            if (ch == '\r' || ch == '\n' || ch == 27) searching = 0;
            else if (ch == 127 || ch == 8) { int l = (int)strlen(search); if (l) search[l-1] = 0; }
            else if (ch >= 32 && ch < 127) { int l = (int)strlen(search);
                if (l < (int)sizeof search - 1) { search[l] = (char)ch; search[l+1] = 0; } }
            continue;
        }
        if (ch == 'q') break;
        else if (ch == 'j') { if (sel + 1 < nlist) sel++; }
        else if (ch == 'k') { if (sel > 0) sel--; }
        else if (ch == 'g') sel = 0;
        else if (ch == 'G') sel = nlist ? nlist - 1 : 0;
        else if (ch == '\t') { if (crash_avail) { mode_crash = !mode_crash; sel = 0; status[0] = 0; } }
        else if (ch == '/') { searching = 1; search[0] = 0; }
        else if (ch == ':') { in_palette = 1; palette[0] = 0; }
        else if (ch == 27) {                   /* arrow keys */
            unsigned char s1, s2;
            if (read(0, &s1, 1) == 1 && s1 == '[' && read(0, &s2, 1) == 1) {
                if (s2 == 'A' && sel > 0) sel--;
                else if (s2 == 'B' && sel + 1 < nlist) sel++;
            }
        }
    }
    raw_off();
    free(vis);
    for (int i = 0; i < g_src_n; i++) free(g_src_lines[i]);
    g_src_n = 0; g_src_path[0] = 0;
}

static void cmd_tui(struct img *m, int argc, char **argv)
{
    struct crash c; int have = 0;
    if (argc >= 1) { parse_crash(argv[0], &c); have = 1; }   /* optional crash dump */
    if (isatty(0) && isatty(1)) tui_interactive(m, have ? &c : NULL, have);
    else tui_plain(m);            /* piped/non-tty: a scriptable full dump */
}
#endif /* EMBDBG_NO_MAIN — interactive TUI */

#ifndef EMBDBG_NO_MAIN
/* ===================================================================== *
 * LIVE DEBUGGING, over the GDB remote serial protocol.
 *
 * Everything above this point reads a file. This drives a RUNNING
 * target through a stub -- QEMU's `-gdb tcp::PORT`, or OpenOCD's over
 * JTAG/SWD to a real chip -- and hands what it reads to exactly the
 * same symbolizer, line table and variable lists. That is the point of
 * doing it here rather than writing a second debugger: `where` on a
 * live Cortex-M prints what `where ADDR` prints on a crash dump,
 * because it IS the same function underneath.
 *
 * EmbDBG's original design parked live debugging behind the EmbLinkOS
 * kernel's own contract, which was right for a process running on
 * EmbLinkOS and is not the only live target this compiler has any
 * more. tools/embdbg/remote.c's header argues that at length.
 *
 * The session reads commands from stdin, one per line, so a script is
 * a heredoc and the golden test is an ordinary diff. There is no
 * readline and no raw mode here; the TUI above already owns that.
 * ===================================================================== */

struct bp { unsigned long addr; int len; int active; };

struct live {
    struct rsp r;
    struct img *m;
    const struct rsp_regdef *tab;
    const char *arch;
    int wb;                       /* bytes in a register */
    struct bp bp[64];
    int nbp;
    int running;                  /* 0 once the target has exited */
};

/* Which register table, from the ELF the symbols came out of -- not
 * from a flag, because the two cannot then disagree. */
static const char *live_arch(struct img *m, int *wb)
{
    Elf64_Ehdr *e = (Elf64_Ehdr *)m->b;
    int cls32 = e->e_ident[4] == 1;
    *wb = cls32 ? 4 : 8;
    switch (e->e_machine) {
    case 183: return "aarch64";          /* EM_AARCH64 */
    case 40:  return "arm";              /* EM_ARM */
    case 243: return cls32 ? "riscv32" : "riscv64";
    case 83:  *wb = 1; return "avr";     /* EM_AVR: one-byte registers */
    default:  return "x86_64";
    }
}

/* ---- reading variables off a live target ------------------------------
 *
 * A local is at frame base + its DW_OP_fbreg offset; the frame base is the
 * register DW_AT_frame_base names (plus its offset), read from the stub
 * now. A global is at its DW_OP_addr. What the stub is then asked for is
 * a DATA address -- which on AVR is not the number the code uses: data
 * memory is its own space, and QEMU's stub (like GDB) puts it at 0x800000
 * up, below which `m` reads flash. A frame address is a bare 16-bit Y + q,
 * so it is moved up; a global's DW_OP_addr already carries the offset. */
#define AVR_DATA_SPACE 0x800000UL

static int live_pc(struct live *L, unsigned long *pc);

static unsigned long live_data_addr(struct live *L, unsigned long a)
{
    if (strcmp(L->arch, "avr") == 0 && a < AVR_DATA_SPACE)
        return a + AVR_DATA_SPACE;
    return a;
}

/* The value of DWARF register n, as a pointer -- each architecture numbers
 * its registers for DWARF its own way, and the stub's table is by name. */
static int live_dwreg(struct live *L, int n, unsigned long *out)
{
    static const char *const x86[16] = {
        "rax", "rdx", "rcx", "rbx", "rsi", "rdi", "rbp", "rsp",
        "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15" };
    char nm[16];
    const char *name = NULL;
    unsigned long long v, hi;
    if (rsp_read_regs(&L->r) < 0) return -1;
    if (strcmp(L->arch, "avr") == 0) {
        /* AVR's pointer registers are PAIRS, rN the low byte and rN+1 the
         * high one, and DWARF names the pair by its low register: breg28
         * is Y, r28:r29. Reading r28 alone took Y's low byte for all of it. */
        if (n < 0 || n > 30) return -1;
        snprintf(nm, sizeof nm, "r%d", n);
        if (rsp_reg(&L->r, L->tab, nm, &v) < 0) return -1;
        snprintf(nm, sizeof nm, "r%d", n + 1);
        if (rsp_reg(&L->r, L->tab, nm, &hi) < 0) return -1;
        *out = (unsigned long)(v | (hi << 8));
        return 0;
    }
    if (strcmp(L->arch, "x86_64") == 0) {
        name = n >= 0 && n < 16 ? x86[n] : NULL;
    } else if (strcmp(L->arch, "aarch64") == 0) {
        if (n == 31) name = "sp";
        else if (n >= 0 && n < 31) { snprintf(nm, sizeof nm, "x%d", n); name = nm; }
    } else {
        /* ARM r0-r15 and RISC-V x0-x31 are the tables' first entries, in
         * DWARF order */
        int lim = strcmp(L->arch, "arm") == 0 ? 16 : 32;
        if (n >= 0 && n < lim) name = L->tab[n].name;
    }
    if (!name || rsp_reg(&L->r, L->tab, name, &v) < 0) return -1;
    *out = (unsigned long)v;
    return 0;
}

static const struct dtype *type_at(struct img *m, unsigned long off)
{
    for (int i = 0; i < m->ntypes; i++)
        if (m->types[i].off == off) return &m->types[i];
    return NULL;
}

/* A value's bytes as its type says: a base type by its encoding and size, a
 * pointer in hex, an aggregate as its bytes. Little-endian, as every
 * target this client talks to is. */
static void print_value(struct img *m, unsigned long toff,
                        const unsigned char *b, int n)
{
    const struct dtype *t = type_at(m, toff);
    unsigned long long u = 0;
    for (int i = n - 1; i >= 0 && i < 8; i--) u = (u << 8) | b[i];
    if (!t || t->agg) {
        printf("{");
        for (int i = 0; i < n; i++) printf(" %02x", b[i]);
        printf(" }");
        return;
    }
    if (t->is_ptr) { printf("0x%llx", u); return; }
    if (t->enc == 4 && n == 4) { float f; memcpy(&f, b, 4); printf("%g", (double)f); return; }
    if (t->enc == 4 && n == 8) { double d; memcpy(&d, b, 8); printf("%g", d); return; }
    if (t->enc == 5 || t->enc == 6) {         /* signed, signed char */
        long long sv = (long long)u;
        if (n < 8 && (u >> (8 * n - 1)) & 1) sv = (long long)(u | (~0ULL << (8 * n)));
        printf("%lld", sv);
        return;
    }
    printf("%llu", u);
}

static int var_size(struct img *m, unsigned long toff)
{
    const struct dtype *t = type_at(m, toff);
    return t && t->size > 0 ? t->size : 4;
}

/* Read and print NAME: a parameter or local of the function the pc is in,
 * else a global. Returns 0 if it named something. */
static int live_print(struct live *L, const char *name)
{
    unsigned long pc;
    unsigned char buf[256];
    if (live_pc(L, &pc) < 0) { printf("embdbg: cannot read the pc\n"); return -1; }
    const struct dfunc *d = dfunc_at(L->m, pc);
    for (int k = 0; d && k < d->nvars; k++) {
        const struct dvar *v = &d->vars[k];
        unsigned long base;
        if (strcmp(v->name, name) != 0) continue;
        if (d->fb_reg < 0 || live_dwreg(L, d->fb_reg, &base) < 0) {
            printf("embdbg: %s: no frame base to locate it by\n", name);
            return -1;
        }
        if (!d->fb_isreg) base += (unsigned long)d->fb_off;
        int n = var_size(L->m, v->type_off);
        if (n > (int)sizeof buf) n = (int)sizeof buf;
        unsigned long a = live_data_addr(L, base + (unsigned long)v->fbreg);
        if (rsp_read_mem(&L->r, a, buf, n) < n) {
            printf("embdbg: cannot read %s at 0x%lx\n", name, a);
            return -1;
        }
        printf("%s = ", name);
        print_value(L->m, v->type_off, buf, n);
        printf("\n");
        return 0;
    }
    for (int k = 0; d && k < d->ngone; k++)
        if (strcmp(d->gone[k], name) == 0) {
            printf("%s = <optimized out>\n", name);
            return 0;
        }
    for (int k = 0; k < L->m->nglobs; k++) {
        const struct dglob *g = &L->m->globs[k];
        if (strcmp(g->name, name) != 0) continue;
        int n = var_size(L->m, g->type_off);
        if (n > (int)sizeof buf) n = (int)sizeof buf;
        unsigned long a = live_data_addr(L, g->addr);
        if (rsp_read_mem(&L->r, a, buf, n) < n) {
            printf("embdbg: cannot read %s at 0x%lx\n", name, a);
            return -1;
        }
        printf("%s = ", name);
        print_value(L->m, g->type_off, buf, n);
        printf("\n");
        return 0;
    }
    printf("embdbg: no variable '%s' here\n", name);
    return -1;
}

static void live_locals(struct live *L)
{
    unsigned long pc;
    if (live_pc(L, &pc) < 0) return;
    const struct dfunc *d = dfunc_at(L->m, pc);
    if (!d) { printf("embdbg: no scope information here\n"); return; }
    for (int k = 0; k < d->nvars; k++) live_print(L, d->vars[k].name);
    for (int k = 0; k < d->ngone; k++) printf("%s = <optimized out>\n", d->gone[k]);
}

static int live_pc(struct live *L, unsigned long *pc)
{
    unsigned long long v;
    if (rsp_read_regs(&L->r) < 0) return -1;
    if (rsp_reg(&L->r, L->tab, rsp_pc_name(L->arch), &v) < 0) return -1;
    *pc = (unsigned long)v;
    return 0;
}

/* Resolve a breakpoint location: a function name, FILE:LINE, or *ADDR.
 * A function's name resolves PAST ITS PROLOGUE where the line table can
 * say where that is -- the second row inside the function -- because a
 * breakpoint on the entry address stops before the frame exists and
 * every local reads as garbage. */
static int resolve_loc(struct img *m, const char *spec, unsigned long *out)
{
    if (spec[0] == '*') { *out = strtoul(spec + 1, NULL, 0); return 0; }

    const char *colon = strrchr(spec, ':');
    if (colon && colon[1] >= '0' && colon[1] <= '9') {
        int want = atoi(colon + 1);
        size_t flen = (size_t)(colon - spec);
        for (int i = 0; i < m->nrows; i++) {
            if (m->rows[i].end || m->rows[i].line != want) continue;
            const char *f = file_name(m, m->rows[i].file);
            size_t n = strlen(f);
            /* Match on the tail, so `live.c:12` finds `out/live.c`. */
            if (n >= flen && strncmp(f + n - flen, spec, flen) == 0) {
                *out = m->rows[i].addr;
                return 0;
            }
        }
        return -1;
    }
    for (int i = 0; i < m->nfn; i++) {
        if (strcmp(m->fn[i].name, spec) != 0) continue;
        unsigned long lo = m->fn[i].addr, hi = lo + m->fn[i].size;
        unsigned long best = lo;
        /* The first row strictly after the entry is the body. */
        for (int k = 0; k < m->nrows; k++)
            if (!m->rows[k].end && m->rows[k].addr > lo &&
                m->rows[k].addr < hi &&
                (best == lo || m->rows[k].addr < best))
                best = m->rows[k].addr;
        *out = best;
        return 0;
    }
    return -1;
}

static void live_where(struct live *L)
{
    unsigned long pc;
    if (live_pc(L, &pc) < 0) { printf("embdbg: cannot read the pc\n"); return; }
    printf("stopped at 0x%lx  ", pc);
    print_loc(L->m, pc);
    printf("\n");
    const struct row *r = line_at(L->m, pc);
    if (r) print_source(file_name(L->m, r->file), r->line, 2);
    const struct dfunc *d = dfunc_at(L->m, pc);
    if (d && d->nvars) {
        printf("  in %s — %d variable(s) in scope:\n", d->name, d->nvars);
        list_vars(L->m, d);
    }
}

/* One source-line step: single-step instructions until the line table
 * says the line changed. Bounded, because a step that leaves the code
 * with line info would otherwise run to the end of the program one
 * instruction at a time. */
static void live_step_line(struct live *L)
{
    unsigned long pc;
    if (live_pc(L, &pc) < 0) return;
    const struct row *start = line_at(L->m, pc);
    int startline = start ? start->line : -1;
    for (int n = 0; n < 20000; n++) {
        int sig = 0;
        if (rsp_step(&L->r, &sig) < 0 || sig < 0) { L->running = 0; return; }
        if (live_pc(L, &pc) < 0) return;
        const struct row *r = line_at(L->m, pc);
        if (!r) continue;
        if (r->line != startline) return;
    }
    printf("embdbg: 20000 instructions without reaching another source line\n");
}

static void live_regs(struct live *L)
{
    if (rsp_read_regs(&L->r) < 0) { printf("embdbg: cannot read registers\n"); return; }
    /* AVR's 35 registers are mostly one byte: eight to a row */
    int col = 0, per = strcmp(L->arch, "avr") == 0 ? 8 : 4;
    for (const struct rsp_regdef *d = L->tab; d->name; d++) {
        unsigned long long v;
        if (rsp_reg(&L->r, L->tab, d->name, &v) < 0) continue;
        printf("%-5s %0*llx%s", d->name, d->size * 2, v,
               (++col % per) ? "  " : "\n");
    }
    if (col % per) printf("\n");
}

/* A backtrace, as far as this target honestly allows.
 *
 * Frame 0 is always the pc. Beyond it, x86-64 has a frame-pointer chain
 * this can walk -- the same one the crash analyser walks. The embedded
 * backends address everything from sp and set up NO frame pointer, so
 * there is nothing to walk there: the return-address register is frame
 * 1 and is only right before the prologue has spilled it, which is said
 * rather than glossed over. Proper unwinding needs .debug_frame, which
 * -g does not emit for those targets yet. */
static void live_bt(struct live *L)
{
    unsigned long pc;
    unsigned long long fp, ra;
    if (live_pc(L, &pc) < 0) return;
    printf("  #0  0x%lx  ", pc); print_loc(L->m, pc); printf("\n");

    if (strcmp(L->arch, "avr") == 0) {
        /* The return address is on the stack above the frame, the saved
         * registers and Y -- at a distance only the frame's size gives,
         * and nothing in the DWARF says it. */
        printf("  (no deeper: -g emits no .debug_frame for AVR yet, so where\n"
               "   the return address sits above Y is not recorded)\n");
        return;
    }
    if (strcmp(L->arch, "x86_64") != 0) {
        if (rsp_reg(&L->r, L->tab, strcmp(L->arch, "arm") == 0 ? "lr" : "ra",
                    &ra) == 0 && ra) {
            /* An ARM return address carries the interworking bit, which
             * is not part of the address: leaving it on reports every
             * caller one byte past the instruction it will return to. */
            unsigned long a = (unsigned long)ra;
            if (strcmp(L->arch, "arm") == 0) a &= ~1UL;
            printf("  #1  0x%lx  ", a); print_loc(L->m, a);
            printf("   (from the return-address register)\n");
        }
        printf("  (no deeper: this backend keeps no frame pointer and -g\n"
               "   emits no .debug_frame for it yet)\n");
        return;
    }
    if (rsp_reg(&L->r, L->tab, rsp_fp_name(L->arch), &fp) < 0) return;
    for (int n = 1; n < 32 && fp; n++) {
        unsigned char w[16];
        unsigned long long next = 0, retn = 0;
        if (rsp_read_mem(&L->r, fp, w, 16) < 16) break;
        for (int i = 7; i >= 0; i--) next = (next << 8) | w[i];
        for (int i = 15; i >= 8; i--) retn = (retn << 8) | w[i];
        if (!retn) break;
        printf("  #%-2d 0x%llx  ", n, retn);
        print_loc(L->m, (unsigned long)retn);
        printf("\n");
        if (next <= fp) break;           /* a chain must grow upward */
        fp = next;
    }
}

static void live_mem(struct live *L, const char *as, const char *ls)
{
    unsigned long addr = strtoul(as, NULL, 0);
    int len = ls ? atoi(ls) : 32;
    unsigned char buf[512];
    if (len < 1) len = 1;
    if (len > (int)sizeof buf) len = (int)sizeof buf;
    int got = rsp_read_mem(&L->r, addr, buf, len);
    if (got <= 0) { printf("embdbg: cannot read 0x%lx\n", addr); return; }
    for (int i = 0; i < got; i += 16) {
        printf("  %08lx ", addr + (unsigned)i);
        for (int k = 0; k < 16 && i + k < got; k++) printf(" %02x", buf[i + k]);
        printf("\n");
    }
}

/* Off a breakpoint before running on. A stub that implements a software
 * breakpoint with a trap instruction (OpenOCD does, on a real part) traps
 * again at once when resumed AT it; so the breakpoint under the pc is
 * taken out, one instruction stepped, and put back. Returns -1 if the
 * target was lost, 1 if it exited, else 0. */
static int live_step_off(struct live *L)
{
    unsigned long pc;
    int sig = 0, rc = 0, at = -1;
    if (live_pc(L, &pc) < 0) return 0;
    for (int i = 0; i < L->nbp; i++)
        if (L->bp[i].active && L->bp[i].addr == pc) at = i;
    if (at < 0) return 0;
    rsp_break(&L->r, L->bp[at].addr, L->bp[at].len, 0);
    if (rsp_step(&L->r, &sig) < 0) rc = -1;
    else if (sig < 0) rc = 1;
    if (rc == 0) rsp_break(&L->r, L->bp[at].addr, L->bp[at].len, 1);
    return rc;
}

static void cmd_remote(struct img *m, int argc, char **argv)
{
    struct live L;
    char host[128] = "localhost";
    const char *port = "1234";
    char line[512];

    if (argc < 1) die("remote needs HOST:PORT or PORT");
    {
        const char *spec = argv[0], *c = strrchr(spec, ':');
        if (c) {
            size_t n = (size_t)(c - spec);
            if (n >= sizeof host) n = sizeof host - 1;
            memcpy(host, spec, n); host[n] = 0;
            port = c + 1;
        } else {
            port = spec;                 /* a bare port means localhost */
        }
    }
    memset(&L, 0, sizeof L);
    L.m = m;
    L.arch = live_arch(m, &L.wb);
    L.tab = rsp_regs_for(L.arch);
    L.running = 1;
    if (rsp_connect(&L.r, host, port) < 0) {
        fprintf(stderr, "embdbg: cannot reach a gdb stub at %s:%s\n",
                host, port);
        exit(1);
    }
    printf("connected to %s:%s — %s target\n", host, port, L.arch);
    {
        int sig = 0;
        rsp_halt_reason(&L.r, &sig);
    }
    live_where(&L);

    while (fgets(line, sizeof line, stdin)) {
        char cmd[64] = "", a1[256] = "", a2[64] = "";
        int nf = sscanf(line, "%63s %255s %63s", cmd, a1, a2);
        if (nf < 1 || cmd[0] == '#') continue;

        if (!strcmp(cmd, "quit") || !strcmp(cmd, "q")) break;
        if (!strcmp(cmd, "where") || !strcmp(cmd, "w")) { live_where(&L); continue; }
        if (!strcmp(cmd, "regs")) { live_regs(&L); continue; }
        if (!strcmp(cmd, "print") || !strcmp(cmd, "p")) {
            if (nf < 2) printf("embdbg: print needs a variable's name\n");
            else live_print(&L, a1);
            continue;
        }
        if (!strcmp(cmd, "locals") ||
            (!strcmp(cmd, "info") && nf >= 2 && !strcmp(a1, "locals"))) {
            live_locals(&L);
            continue;
        }
        if (!strcmp(cmd, "set")) {
            /* set REG VALUE: a register, by the name `regs` shows */
            if (nf < 3) { printf("embdbg: set needs a register and a value\n"); continue; }
            if (rsp_write_reg(&L.r, L.tab, a1, strtoull(a2, NULL, 0)) < 0)
                printf("embdbg: could not write %s\n", a1);
            else
                printf("%s = 0x%llx\n", a1, strtoull(a2, NULL, 0));
            continue;
        }
        if (!strcmp(cmd, "bt")) { live_bt(&L); continue; }
        if (!strcmp(cmd, "mem")) {
            if (nf < 2) printf("embdbg: mem needs an address\n");
            else live_mem(&L, a1, nf >= 3 ? a2 : NULL);
            continue;
        }
        if (!strcmp(cmd, "break") || !strcmp(cmd, "b")) {
            unsigned long addr;
            if (nf < 2) { printf("embdbg: break needs FUNC, FILE:LINE or *ADDR\n"); continue; }
            if (resolve_loc(m, a1, &addr) < 0) {
                printf("embdbg: cannot resolve '%s'\n", a1);
                continue;
            }
            /* The length is the instruction the stub replaces. Four is
             * right for aarch64 and RISC-V, two for Thumb and AVR (whose
             * `break` is one word), and one is what a variable-length
             * machine wants. */
            int blen = !strcmp(L.arch, "x86_64") ? 1
                     : !strcmp(L.arch, "arm") || !strcmp(L.arch, "avr") ? 2
                     : 4;
            int rc = rsp_break(&L.r, addr, blen, 1);
            if (rc == -2) { printf("embdbg: this stub has no software breakpoints\n"); continue; }
            if (rc < 0)   { printf("embdbg: the stub refused a breakpoint at 0x%lx\n", addr); continue; }
            if (L.nbp < (int)(sizeof L.bp / sizeof L.bp[0])) {
                L.bp[L.nbp].addr = addr; L.bp[L.nbp].len = blen;
                L.bp[L.nbp].active = 1; L.nbp++;
            }
            printf("breakpoint %d at 0x%lx  ", L.nbp, addr);
            print_loc(m, addr);
            printf("\n");
            continue;
        }
        if (!strcmp(cmd, "delete")) {
            for (int i = 0; i < L.nbp; i++)
                if (L.bp[i].active) {
                    rsp_break(&L.r, L.bp[i].addr, L.bp[i].len, 0);
                    L.bp[i].active = 0;
                }
            printf("all breakpoints removed\n");
            continue;
        }
        if (!strcmp(cmd, "continue") || !strcmp(cmd, "c") ||
            !strcmp(cmd, "step") || !strcmp(cmd, "s") ||
            !strcmp(cmd, "stepi") || !strcmp(cmd, "si")) {
            if (!L.running) { printf("the target has exited\n"); continue; }
            int sig = 0;
            if (strcmp(cmd, "stepi") && strcmp(cmd, "si")) {
                int off = live_step_off(&L);
                if (off < 0) { printf("embdbg: lost the target\n"); break; }
                if (off > 0) { printf("the target exited\n"); L.running = 0; continue; }
            }
            if (!strcmp(cmd, "continue") || !strcmp(cmd, "c")) {
                if (rsp_cont(&L.r, &sig) < 0) { printf("embdbg: lost the target\n"); break; }
            } else if (!strcmp(cmd, "stepi") || !strcmp(cmd, "si")) {
                if (rsp_step(&L.r, &sig) < 0) { printf("embdbg: lost the target\n"); break; }
            } else {
                live_step_line(&L);
                if (L.running) live_where(&L);
                continue;
            }
            if (sig < 0) { printf("the target exited\n"); L.running = 0; continue; }
            live_where(&L);
            continue;
        }
        printf("embdbg: unknown command '%s' "
               "(break continue step stepi where bt regs set print locals "
               "mem delete quit)\n", cmd);
    }
    rsp_close(&L.r);
}
#endif /* EMBDBG_NO_MAIN — live debugging */

/* Parse one relocatable object's DWARF into a fresh model, biasing every code
 * address by `bias` (its final .text vaddr) so a .o's .text-relative addresses
 * come out absolute. */
static void img_parse(struct img *t, const unsigned char *obj, long len, long bias)
{
    memset(t, 0, sizeof *t);
    t->b = (unsigned char *)obj; t->len = len;
    g_addr_bias = bias;
    load_sections(t);
    load_funcs(t);
    decode_lines(t);
    decode_info(t);
    g_addr_bias = 0;
}

/* Intern a source file into the accumulator's 1-based file list (dedup by
 * path, so the same source shared by two objects is one FILES row). */
static int acc_file(struct img *acc, const char *path)
{
    for (int i = 1; i < acc->nfiles; i++)
        if (acc->files[i] && strcmp(acc->files[i], path) == 0) return i;
    acc->files = realloc(acc->files, (size_t)(acc->nfiles + 1) * sizeof(char *));
    acc->files[acc->nfiles] = (char *)path;
    return acc->nfiles++;
}

/* Link-time entry point (used by EmbLD): merge the DWARF of one OR MORE debug
 * objects — each biased by its own final .text vaddr — into a single model and
 * write a .embdbg whose build_id is SHA-256 of the linked `image`. Merging
 * needs two rebases so nothing collides: each object's type-offset keys are
 * shifted into their own 2^32 band (the keys are opaque — write_embdbg assigns
 * the real TYPES-blob offsets), and its file indices are remapped through the
 * deduped accumulator list. Addresses are already absolute via the bias, so
 * funcs and line rows just concatenate. */
int embdbg_emit_objects(const unsigned char **objs, const long *lens,
                        const long *biases, int n,
                        const unsigned char *image, long imagelen,
                        const char *out)
{
    struct img acc; memset(&acc, 0, sizeof acc);
    acc.nfiles = 1;
    acc.files = calloc(1, sizeof(char *));      /* index 0 unused */

    for (int i = 0; i < n; i++) {
        struct img t;
        img_parse(&t, objs[i], lens[i], biases[i]);
        unsigned long tb = (unsigned long)(i + 1) << 32;   /* type-key band */

        int *fmap = calloc((size_t)(t.nfiles > 0 ? t.nfiles : 1), sizeof(int));
        for (int f = 1; f < t.nfiles; f++) fmap[f] = acc_file(&acc, t.files[f]);

        for (int r = 0; r < t.nrows; r++) {
            int fi = 0;
            if (!t.rows[r].end && t.rows[r].file >= 1 && t.rows[r].file < t.nfiles)
                fi = fmap[t.rows[r].file];
            add_row(&acc, t.rows[r].addr, fi, t.rows[r].line, t.rows[r].end);
        }
        for (int k = 0; k < t.ntypes; k++) {
            acc.types = realloc(acc.types, (size_t)(acc.ntypes + 1) * sizeof *acc.types);
            struct dtype dt = t.types[k];
            dt.off += tb;
            if (dt.is_ptr && dt.pointee) dt.pointee += tb;
            acc.types[acc.ntypes++] = dt;
        }
        for (int d = 0; d < t.ndfn; d++) {
            struct dfunc df = t.dfn[d];             /* transfers the vars array */
            for (int v = 0; v < df.nvars; v++)
                if (df.vars[v].type_off) df.vars[v].type_off += tb;
            acc.dfn = realloc(acc.dfn, (size_t)(acc.ndfn + 1) * sizeof *acc.dfn);
            acc.dfn[acc.ndfn++] = df;
        }
        for (int fi = 0; fi < t.nfn; fi++) {
            acc.fn = realloc(acc.fn, (size_t)(acc.nfn + 1) * sizeof *acc.fn);
            acc.fn[acc.nfn++] = t.fn[fi];
        }
        free(fmap);
    }

    write_embdbg(&acc, out, image, imagelen);
    return 0;
}

#ifndef EMBDBG_NO_MAIN
/* --- emit-kernel: a func+line .embdbg for an ELF whose DWARF this tool can't
 * fully parse (a DWARF-5 gcc kernel). Functions come from the ELF .symtab
 * (reliable, complete); the line table comes from `readelf --debug-dump=
 * decodedline`, letting binutils decode any DWARF version. No VARS/TYPES —
 * the kernel panic symbolizer needs only address -> func + file:line + a
 * backtrace (EMBDBG_Specification.md §7). --- */
static int cmp_dfn_lo(const void *a, const void *b)
{
    unsigned long x = ((const struct dfunc *)a)->lo, y = ((const struct dfunc *)b)->lo;
    return x < y ? -1 : x > y ? 1 : 0;
}
static int kfile_intern(struct img *m, const char *path)
{
    for (int i = 1; i < m->nfiles; i++)
        if (m->files[i] && strcmp(m->files[i], path) == 0) return i;
    m->files = realloc(m->files, (size_t)(m->nfiles + 1) * sizeof(char *));
    m->files[m->nfiles] = strdup(path);        /* strtok buffer is reused */
    return m->nfiles++;
}
static void emit_kernel(struct img *m, const char *elfpath, const char *out)
{
    /* FUNCS from the symtab (already in m->fn), as dfn sorted by low_pc. */
    m->nfiles = 1; m->files = calloc(1, sizeof(char *));
    m->dfn = calloc((size_t)(m->nfn ? m->nfn : 1), sizeof *m->dfn);
    for (int i = 0; i < m->nfn; i++) {
        struct dfunc *d = &m->dfn[m->ndfn++];
        d->name = m->fn[i].name;
        d->lo = m->fn[i].addr;
        d->hi = m->fn[i].addr + m->fn[i].size;
        d->vars = NULL; d->nvars = 0; d->fb_reg = -1;
    }
    qsort(m->dfn, (size_t)m->ndfn, sizeof *m->dfn, cmp_dfn_lo);

    /* LINE from readelf's decoded table (handles DWARF 5).
     *
     * WHICH readelf, THOUGH. A host that cross-compiles need not have a native
     * one at all: macOS ships none, and the binutils installed for the target
     * are named for it -- x86_64-elf-readelf, aarch64-elf-readelf. Hard-coding
     * the bare name does not fail there, it succeeds quietly and emits ZERO
     * LINE ROWS, so the kernel gets a symbolizer that can name a function and
     * never a line, and nothing says why. $READELF lets the caller name the
     * one that matches the ELF it is handing over. */
    const char *readelf = getenv("READELF");
    if (!readelf || !*readelf) readelf = "readelf";
    char cmd[8192];
    snprintf(cmd, sizeof cmd, "%s --debug-dump=decodedline '%s' 2>/dev/null",
             readelf, elfpath);
    FILE *f = popen(cmd, "r");
    if (f) {
        char line[2048], last_file[512] = "";
        while (fgets(line, sizeof line, f)) {
            char *toks[24]; int nt = 0;
            for (char *p = strtok(line, " \t\n"); p && nt < 24; p = strtok(NULL, " \t\n"))
                toks[nt++] = p;
            int ai = -1;                       /* the address column */
            for (int k = 0; k < nt; k++)
                if (toks[k][0] == '0' && toks[k][1] == 'x') { ai = k; break; }
            if (ai < 1) continue;
            char *lns = toks[ai - 1];
            if (lns[0] < '0' || lns[0] > '9') continue;   /* not a data row */
            const char *file = (ai >= 2) ? toks[ai - 2] : last_file;
            if (ai >= 2) snprintf(last_file, sizeof last_file, "%s", file);
            add_row(m, strtoul(toks[ai], NULL, 0),
                    kfile_intern(m, file), atoi(lns), 0);
        }
        pclose(f);
    }
    write_embdbg(m, out, m->b, m->len);
    fprintf(stderr, "embdbg: kernel .embdbg — %d funcs, %d line rows\n", m->ndfn, m->nrows);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr,
            "EmbDBG v0 — EmbLinkOS debug-info reader\n"
            "usage: embdbg FILE funcs               list functions + ranges\n"
            "       embdbg FILE lines               the address->file:line table\n"
            "       embdbg FILE symbolize ADDR...   addr -> func+off  file:line\n"
            "       embdbg FILE backtrace ADDR...   symbolize a caller chain\n"
            "       embdbg FILE where ADDR          source context + locals in scope\n"
            "       embdbg FILE list ADDR           source lines around addr\n"
            "       embdbg FILE info FUNC           a function's params/locals\n"
            "       embdbg FILE disassemble FUNC    x86-64 disassembly + mixed source\n"
            "       embdbg FILE crash REPORT        analyze a kernel fault dump\n"
            "       embdbg FILE tui [CRASH]         rich multi-panel TUI (source/asm/regs/vars/\n"
            "                                       stack); pass a crash dump for live regs+stack\n"
            "       embdbg FILE remote [HOST:]PORT  debug a RUNNING target through a gdb stub\n"
            "                                       (QEMU -gdb tcp::PORT, or OpenOCD over SWD/JTAG);\n"
            "                                       reads commands from stdin\n"
            "       embdbg FILE.o emit OUT.embdbg   convert DWARF -> native .embdbg\n"
            "   FILE may be an ELF (reads DWARF) or a .embdbg (reads it natively).\n");
        return 1;
    }
    struct img m; memset(&m, 0, sizeof m);
    m.b = slurp(argv[1], &m.len);
    int is_embdbg = (m.len >= 8 && memcmp(m.b, EMBDBG_MAGIC, 8) == 0);
    if (is_embdbg) {
        load_embdbg(&m);
    } else {
        load_sections(&m);
        load_funcs(&m);
        decode_lines(&m);
        decode_info(&m);
    }

    const char *cmd = argv[2];
    if (strcmp(cmd, "emit") == 0) {
        if (is_embdbg) die("input is already .embdbg");
        if (argc < 4) die("emit needs an output path");
        write_embdbg(&m, argv[3], m.b, m.len);   /* build_id = hash of this ELF */
        return 0;
    }
    if (strcmp(cmd, "emit-kernel") == 0) {
        if (is_embdbg) die("input is already .embdbg");
        if (argc < 4) die("emit-kernel needs an output path");
        emit_kernel(&m, argv[1], argv[3]);       /* funcs from symtab, lines via readelf */
        return 0;
    }
    if (strcmp(cmd, "verify") == 0) {
        if (!is_embdbg) die("verify needs a .embdbg file");
        cmd_verify(&m);
        return 0;
    }
    if (strcmp(cmd, "funcs") == 0)          cmd_funcs(&m);
    else if (strcmp(cmd, "lines") == 0)     cmd_lines(&m);
    else if (strcmp(cmd, "symbolize") == 0) cmd_symbolize(&m, argc - 3, argv + 3);
    else if (strcmp(cmd, "backtrace") == 0) cmd_backtrace(&m, argc - 3, argv + 3);
    else if (strcmp(cmd, "where") == 0)     cmd_where(&m, argc - 3, argv + 3);
    else if (strcmp(cmd, "list") == 0)      cmd_list(&m, argc - 3, argv + 3);
    else if (strcmp(cmd, "info") == 0)      cmd_info(&m, argc - 3, argv + 3);
    else if (strcmp(cmd, "disassemble") == 0) cmd_disassemble(&m, argc - 3, argv + 3);
    else if (strcmp(cmd, "crash") == 0)     cmd_crash(&m, argc - 3, argv + 3);
    else if (strcmp(cmd, "tui") == 0)       cmd_tui(&m, argc - 3, argv + 3);
    else if (strcmp(cmd, "remote") == 0)    cmd_remote(&m, argc - 3, argv + 3);
    else { fprintf(stderr, "embdbg: unknown command '%s'\n", cmd); return 1; }
    return 0;
}
#endif /* EMBDBG_NO_MAIN */
