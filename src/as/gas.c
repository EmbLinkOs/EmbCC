/* See gas.h. */
#include "gas.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../arch/target.h"
#include "../driver/util.h"
#include "../elf/elf.h"
#include "../elf/write.h"
#include "../platform/platform.h"
#include "../cpp/cpp.h"
#include "../arch/riscv/asm.h"
#include "../arch/avr/asm.h"
#include "../arch/thumb/asm.h"
#include "../arch/thumb/emit.h"
#include "../arch/thumb/attrs.h"
#include "../arch/aarch64/asm.h"
#include "../parse/ast.h"

/* ---- the pieces of a file ------------------------------------------ */

/* The four sections every file has, created first so their indices are
 * fixed; `.section NAME` adds more. */
enum { SEC_TEXT, SEC_DATA, SEC_RODATA, SEC_BSS, NSEC };
#define SEC_UNDEF (-1)
#define SEC_ABS   (-2)      /* `.equ x, 5`: a value, in no section */

static const char *const sec_name[NSEC] = {
    ".text", ".data", ".rodata", ".bss"
};

/* A section of the object being built. GNU as decides a section's flags
 * from its name when the directive gives none -- `.text.Reset_Handler` is
 * code, `.isr_vector` with no flags is NOT allocated -- and a startup file
 * written for it depends on that, so this does the same (sec_defaults). */
struct gsec {
    char *name;
    Elf64_Word type;        /* SHT_PROGBITS, SHT_NOBITS, SHT_INIT_ARRAY... */
    Elf64_Xword flags;      /* SHF_ALLOC, SHF_WRITE, SHF_EXECINSTR, ... */
    Elf64_Xword entsize;
    struct code c;          /* the bytes; a NOBITS section has none */
    long size;              /* a NOBITS section's size */
    long align;             /* the largest alignment asked of it */
    int ndx;                /* its index in the object, at write */
};

struct sym {
    char *name;
    int sec;                /* its section, SEC_UNDEF, or SEC_ABS */
    long value;
    int is_global;
    int is_func;
    long size;
    int is_weak;            /* .weak: an undefined reference resolves to 0
                             * rather than failing the link, which is how a
                             * vector table names a handler that may not
                             * exist */
    int elf_ndx;            /* filled when the symbol table is built */
    int is_object;          /* .type %object */
    int vis;                /* .hidden/.internal/.protected: STV_* */
    /* `.set a, b` where b is not placed yet (or is external): a is b,
     * resolved when the object is written. */
    struct sym *alias;
    long alias_add;
};

struct fixup {
    int sec;                /* section holding the field */
    long off;               /* offset within it */
    char *sym;              /* the symbol named */
    int type;               /* ELF relocation type */
    long addend;
};

struct gas {
    const char *path;
    struct gsec *secs;
    int nsecs, capsecs;
    int prev;               /* `.previous` */
    int stack[32], nstack;  /* `.pushsection` / `.popsection` */
    struct sym *syms;
    int nsyms, capsyms;
    struct fixup *fix;
    int nfix, capfix;
    const struct gas_target *tgt;
    /* How many times each numeric local label has been DEFINED so far in
     * this pass. Reset between passes so both agree. */
    int local_n[10];
    int cur;                /* current section */
    int npcrel;             /* .Lpcrel_hiN counter */
    int line;               /* for diagnostics */
    int line_base;          /* a block's own first line, less one: its
                             * statements are numbered from 1 within it */
    int errors;
    int thumb_func_next;    /* .thumb_func: the next label is a function */
    int errors_muted;       /* the expander's own look at an .equ */
    /* Relaxation: a statement assembled wide once stays wide, so the
     * passes settle (see gas_assemble). Indexed by statement. */
    char *wide;
    int nwide, li, grew;
    /* ARM mapping symbols: `$t` where Thumb code starts, `$d` where data
     * does, which is how a disassembler or debugger tells them apart. */
    struct gmap { int sec; long off; char kind; } *maps;
    int nmaps, capmaps;
    char *mapst;
    int nmapst;
    /* Literal pools: `ldr rd, =EXPR` loads a word placed at the next
     * .ltorg or at the section's end. Identical entries in one pool are
     * one word. */
    struct glit { int sec, pool, is_sym; long v; struct sym *sym; } *lits;
    int nlits, caplits;
    int *poolno, npoolno;
    struct gpool { int sec, pool; long base; } *pools, *ppools;
    int npools, cappools, nppools;
};

static void gerr(struct gas *g, const char *fmt, ...)
{
    va_list ap;
    if (g->errors_muted)
        return;
    fprintf(stderr, "%s:%d: error: ", g->path, g->line + g->line_base);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    g->errors++;
}

static struct sym *sym_find(struct gas *g, const char *name, size_t n)
{
    for (int i = 0; i < g->nsyms; i++)
        if (strlen(g->syms[i].name) == n &&
            memcmp(g->syms[i].name, name, n) == 0)
            return &g->syms[i];
    return NULL;
}

static struct sym *sym_get(struct gas *g, const char *name, size_t n)
{
    struct sym *s = sym_find(g, name, n);
    if (s)
        return s;
    if (g->nsyms == g->capsyms) {
        g->capsyms = g->capsyms ? g->capsyms * 2 : 16;
        g->syms = xrealloc(g->syms, (size_t)g->capsyms * sizeof *g->syms);
    }
    s = &g->syms[g->nsyms++];
    memset(s, 0, sizeof *s);
    s->name = xmalloc(n + 1);
    memcpy(s->name, name, n);
    s->name[n] = '\0';
    s->sec = -1;
    return s;
}

static void fix_add(struct gas *g, int sec, long off, const char *sym,
                    int type, long addend)
{
    if (g->nfix == g->capfix) {
        g->capfix = g->capfix ? g->capfix * 2 : 16;
        g->fix = xrealloc(g->fix, (size_t)g->capfix * sizeof *g->fix);
    }
    struct fixup *f = &g->fix[g->nfix++];
    f->sec = sec;
    f->off = off;
    f->sym = xstrndup(sym, strlen(sym));
    f->type = type;
    f->addend = addend;
}

/* ---- lexing a line -------------------------------------------------- */

static int is_sym0(int c) { return isalpha(c) || c == '_' || c == '.' || c == '$'; }
static int is_symc(int c) { return isalnum(c) || c == '_' || c == '.' || c == '$'; }

/* Strips comments and trailing space, in place. `#` and `//` start one;
 * a `#` inside a string does not, and with `hash_imm` (ARM, aarch64) a
 * `#` that is not the line's first character does not either. */
static void strip_comment(char *s, char extra, int hash_imm)
{
    int q = 0;
    char *first = s;
    while (*first && isspace((unsigned char)*first)) first++;
    for (char *p = s; *p; p++) {
        if (q) {
            if (*p == '\\' && p[1]) { p++; continue; }
            if (*p == q) q = 0;
            continue;
        }
        if (*p == '"' || *p == '\'') { q = *p; continue; }
        if ((*p == '#' && (!hash_imm || p == first)) ||
            (*p == '/' && p[1] == '/') ||
            (extra && *p == extra)) { *p = '\0'; break; }
    }
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
}

static char *skip_ws(char *p)
{
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

/* An integer, C-style. Returns 0 when the text is not one. */
static int parse_num(const char *s, const char *end, long *out)
{
    char buf[64];
    size_t n = (size_t)(end - s);
    char *stop;
    long v;
    if (n == 0 || n >= sizeof buf) return 0;
    memcpy(buf, s, n);
    buf[n] = '\0';
    v = strtol(buf, &stop, 0);
    if (*stop) return 0;
    *out = v;
    return 1;
}

/* ---- sections ------------------------------------------------------- */

static int has_prefix(const char *name, const char *pfx)
{
    size_t n = strlen(pfx);
    return strncmp(name, pfx, n) == 0 && (name[n] == 0 || name[n] == '.');
}

/* What GNU as gives a section when `.section NAME` says nothing more. The
 * names it knows are code, data, zeroes, constants and the constructor
 * tables; any other name gets NO flags -- not even allocated -- and a
 * linker script that wants it says so (`KEEP(*(.vectors))`). */
static void sec_defaults(const char *name, Elf64_Word *type,
                         Elf64_Xword *flags)
{
    *type = SHT_PROGBITS;
    *flags = 0;
    if (has_prefix(name, ".text") || has_prefix(name, ".init") ||
        has_prefix(name, ".fini"))
        *flags = SHF_ALLOC | SHF_EXECINSTR;
    else if (has_prefix(name, ".data") || has_prefix(name, ".sdata") ||
             !strcmp(name, ".data1"))
        *flags = SHF_ALLOC | SHF_WRITE;
    else if (has_prefix(name, ".bss") || has_prefix(name, ".sbss")) {
        *type = SHT_NOBITS;
        *flags = SHF_ALLOC | SHF_WRITE;
    } else if (has_prefix(name, ".rodata") || has_prefix(name, ".srodata") ||
               !strcmp(name, ".rodata1"))
        *flags = SHF_ALLOC;
    else if (has_prefix(name, ".tdata"))
        *flags = SHF_ALLOC | SHF_WRITE | SHF_TLS;
    else if (has_prefix(name, ".tbss")) {
        *type = SHT_NOBITS;
        *flags = SHF_ALLOC | SHF_WRITE | SHF_TLS;
    } else if (has_prefix(name, ".init_array")) {
        *type = SHT_INIT_ARRAY;
        *flags = SHF_ALLOC | SHF_WRITE;
    } else if (has_prefix(name, ".fini_array")) {
        *type = SHT_FINI_ARRAY;
        *flags = SHF_ALLOC | SHF_WRITE;
    } else if (has_prefix(name, ".preinit_array")) {
        *type = SHT_PREINIT_ARRAY;
        *flags = SHF_ALLOC | SHF_WRITE;
    } else if (has_prefix(name, ".note")) {
        *type = SHT_NOTE;
    }
}

static int sec_find(struct gas *g, const char *name, size_t n)
{
    for (int i = 0; i < g->nsecs; i++)
        if (strlen(g->secs[i].name) == n &&
            memcmp(g->secs[i].name, name, n) == 0)
            return i;
    return -1;
}

static int sec_add(struct gas *g, const char *name, size_t n,
                   Elf64_Word type, Elf64_Xword flags)
{
    if (g->nsecs == g->capsecs) {
        g->capsecs = g->capsecs ? g->capsecs * 2 : 8;
        g->secs = xrealloc(g->secs, (size_t)g->capsecs * sizeof *g->secs);
    }
    struct gsec *sc = &g->secs[g->nsecs];
    memset(sc, 0, sizeof *sc);
    sc->name = xstrndup(name, n);
    sc->type = type;
    sc->flags = flags;
    sc->align = 1;
    return g->nsecs++;
}

/* The four every file starts with, at their fixed indices. */
static void sec_init(struct gas *g)
{
    for (int i = 0; i < NSEC; i++) {
        Elf64_Word ty;
        Elf64_Xword fl;
        sec_defaults(sec_name[i], &ty, &fl);
        sec_add(g, sec_name[i], strlen(sec_name[i]), ty, fl);
    }
    /* code gets four, as it always has here, and data eight */
    g->secs[SEC_TEXT].align = 4;
}

static int sec_is_nobits(struct gas *g, int k)
{
    return g->secs[k].type == SHT_NOBITS;
}

/* ---- directives ------------------------------------------------------ */

static void emit_bytes(struct gas *g, const unsigned char *p, long n)
{
    if (sec_is_nobits(g, g->cur)) {
        gerr(g, "%s holds no data (it is NOBITS); use .space",
             g->secs[g->cur].name);
        return;
    }
    for (long i = 0; i < n; i++)
        code_byte(&g->secs[g->cur].c, p[i]);
}

static void emit_int(struct gas *g, long v, int width)
{
    unsigned char b[8];
    for (int i = 0; i < width; i++)
        b[i] = (unsigned char)((unsigned long)v >> (8 * i));
    emit_bytes(g, b, width);
}

static long cur_off(struct gas *g)
{
    return sec_is_nobits(g, g->cur) ? g->secs[g->cur].size
                                    : g->secs[g->cur].c.len;
}

static void advance_fill(struct gas *g, long n, int fill)
{
    if (sec_is_nobits(g, g->cur)) {
        if (fill)
            gerr(g, "%s is NOBITS: it can be skipped over but not filled",
                 g->secs[g->cur].name);
        g->secs[g->cur].size += n;
        return;
    }
    for (long i = 0; i < n; i++) code_byte(&g->secs[g->cur].c, (unsigned)fill);
}

static void advance(struct gas *g, long n)
{
    advance_fill(g, n, 0);
}

/* `$t`/`$d` where what this section holds changes from code to data. */
static void map_mark(struct gas *g, char kind)
{
    if (g->tgt->machine != EM_ARM)
        return;
    if (g->nmapst < g->nsecs) {
        g->mapst = xrealloc(g->mapst, (size_t)g->nsecs);
        memset(g->mapst + g->nmapst, 0, (size_t)(g->nsecs - g->nmapst));
        g->nmapst = g->nsecs;
    }
    if (g->mapst[g->cur] == kind)
        return;
    g->mapst[g->cur] = kind;
    if (g->nmaps == g->capmaps) {
        g->capmaps = g->capmaps ? g->capmaps * 2 : 16;
        g->maps = xrealloc(g->maps, (size_t)g->capmaps * sizeof *g->maps);
    }
    g->maps[g->nmaps].sec = g->cur;
    g->maps[g->nmaps].off = cur_off(g);
    g->maps[g->nmaps].kind = kind;
    g->nmaps++;
}

static void do_align(struct gas *g, long boundary)
{
    long off = cur_off(g);
    if (boundary <= 1)
        return;
    if (boundary > g->secs[g->cur].align)
        g->secs[g->cur].align = boundary;
    /* In code the padding may be EXECUTED -- straight-line code that runs
     * into an alignment -- so it is the machine's nop there, as GNU as
     * pads it. Zero bytes are an illegal instruction on RISC-V and
     * `movs r0, r0`, which writes the flags, on Thumb. Data gets zeros. */
    if (g->secs[g->cur].flags & SHF_EXECINSTR) {
        int m = g->tgt->machine;
        struct code *c = &g->secs[g->cur].c;
        while (off % boundary) {
            long left = boundary - off % boundary;
            if (m == EM_ARM && !(off & 1) && left >= 2) {
                code_byte(c, 0x00); code_byte(c, 0xbf);          /* nop */
                off += 2;
            } else if (m == EM_RISCV && !(off & 3) && left >= 4) {
                code_byte(c, 0x13); code_byte(c, 0); code_byte(c, 0);
                code_byte(c, 0);                                 /* nop */
                off += 4;
            } else if (m == EM_RISCV && !(off & 1) && left >= 2) {
                code_byte(c, 0x01); code_byte(c, 0x00);          /* c.nop */
                off += 2;
            } else if (m == EM_AARCH64 && !(off & 3) && left >= 4) {
                code_byte(c, 0x1f); code_byte(c, 0x20); code_byte(c, 0x03);
                code_byte(c, 0xd5);                              /* nop */
                off += 4;
            } else {
                code_byte(c, 0);     /* AVR's nop is 0x0000; an odd byte */
                off++;
            }
        }
        return;
    }
    while (off % boundary) { advance(g, 1); off++; }
}

/* A `"..."` operand, unescaped into buf. Returns its length, or -1. */
static long parse_string(struct gas *g, const char *p, unsigned char *buf,
                         long cap)
{
    long n = 0;
    if (*p != '"') { gerr(g, "expected a quoted string"); return -1; }
    p++;
    while (*p && *p != '"') {
        int c = (unsigned char)*p++;
        if (c == '\\' && *p) {
            int e = *p++;
            switch (e) {
            case 'n': c = '\n'; break;  case 't': c = '\t'; break;
            case 'r': c = '\r'; break;  case '0': c = '\0'; break;
            case 'b': c = '\b'; break;  case 'f': c = '\f'; break;
            case '\\': c = '\\'; break; case '"': c = '"'; break;
            default: c = e; break;
            }
        }
        if (n >= cap) { gerr(g, "string is too long"); return -1; }
        buf[n++] = (unsigned char)c;
    }
    if (*p != '"') { gerr(g, "unterminated string"); return -1; }
    return n;
}

/* ---- instructions ---------------------------------------------------- */

/* Rewrites a statement's operands, replacing any bare identifier that
 * names a LOCAL label with `.+N` (the displacement the statement
 * assemblers take). Returns a malloc'd string, and sets *ext to the
 * name when the identifier is an external symbol instead. */
/* ---- numeric local labels --------------------------------------------
 *
 * `1:` defines one, `1f` refers to the NEXT definition of 1 and `1b` to the
 * previous. GNU as has had them forever and every hand-written assembly
 * file uses them -- a loop wants a label, not a name -- so an assembler
 * without them cannot read real sources. AVR's startup was the first file
 * here to need them.
 *
 * Each definition becomes an ordinary symbol named `.L<digit>\x01<instance>`,
 * so the rest of this file needs to know nothing about them: they are
 * defined, substituted and relocated like any other label. The \x01 cannot
 * occur in a source identifier, which is what keeps a hand-written
 * `.L1.0` from colliding with a generated one.
 *
 * Counting happens in PASS ONE only and is replayed in pass two, so `1f`
 * resolves to the same instance in both. Without that the two passes
 * disagree about which `1:` a forward reference means, and the difference
 * shows up as a jump to the wrong loop.
 */
#define NLOCAL 10

static void local_name(char *buf, size_t n, int digit, int inst)
{
    snprintf(buf, n, ".L%d\x01%d", digit, inst);
}

/* Is `s` a numeric local reference -- one or more digits then 'f' or 'b'?
 * Returns the digit value, or -1. */
static int local_ref(const char *s, size_t n, int *fwd)
{
    size_t i = 0;
    int v = 0;
    if (n < 2)
        return -1;
    while (i < n - 1 && isdigit((unsigned char)s[i])) {
        v = v * 10 + (s[i] - '0');
        i++;
    }
    if (i == 0 || i != n - 1)
        return -1;
    if (s[i] == 'f') *fwd = 1;
    else if (s[i] == 'b') *fwd = 0;
    else return -1;
    return v < NLOCAL ? v : -1;
}

/* ---- expressions -------------------------------------------------------
 *
 * What a directive's operand may be: `.space (214 * 4)`, `.equ SIZE,
 * end - start`, `.long handler + 1`, `.size f, . - f`. A value is either
 * ABSOLUTE, an offset into one of this file's sections (a label plus a
 * constant), or an EXTERNAL symbol plus a constant -- which only a data
 * word can hold, as a relocation. The rules are an assembler's: a label
 * minus a label in the same section is absolute, a label plus a constant
 * stays in its section, and anything else that mixes them is refused.
 *
 * Precedence is GNU as's, which is not C's: `*` `/` `%` `<<` `>>` bind
 * tightest, then `|` `&` `^` `!`, then `+` `-` and the comparisons, then
 * `&&` `||`. `a + b | c` is `a + (b | c)` there, and a startup file
 * written for it means that. */
struct gval {
    long v;                 /* the value, or the offset, or the addend */
    int sec;                /* SEC_ABS, a section, or SEC_UNDEF */
    struct sym *base;       /* the label a section value is relative to, or
                             * the external symbol a SEC_UNDEF one names */
    int unknown;            /* pass 1: a label not placed yet */
};

struct gx {
    struct gas *g;
    const char *p;
    int pass;
    int bad;                /* an error has been reported */
};

static void gx_err(struct gx *x, const char *fmt, const char *a)
{
    if (!x->bad)
        gerr(x->g, fmt, a);
    x->bad = 1;
}

static void gx_ws(struct gx *x)
{
    while (*x->p == ' ' || *x->p == '\t')
        x->p++;
}

static struct gval gx_abs(long v)
{
    struct gval r = { v, SEC_ABS, NULL, 0 };
    return r;
}

/* A location as a value, with a label to relocate against. */
static struct gval gx_here(struct gx *x)
{
    struct gas *g = x->g;
    char nm[48];
    struct gval r;
    struct sym *s;
    /* named by section and offset, so both passes make the same one */
    snprintf(nm, sizeof nm, ".Ldot\x01%d.%ld", g->cur, cur_off(g));
    s = sym_get(g, nm, strlen(nm));
    s->sec = g->cur;
    s->value = cur_off(g);
    r.v = cur_off(g);
    r.sec = g->cur;
    r.base = s;
    r.unknown = 0;
    return r;
}

static struct gval gx_sym(struct gx *x, struct sym *s)
{
    struct gval r = { 0, SEC_UNDEF, s, 0 };
    long add = 0;
    int hops = 0;
    while (s->alias && hops++ < 64) {       /* `.set a, b` not resolved yet */
        add += s->alias_add;
        s = s->alias;
    }
    if (s->sec == SEC_ABS) {
        return gx_abs(s->value + add);
    } else if (s->sec >= 0) {
        r.v = s->value + add;
        r.sec = s->sec;
        r.base = s;
    } else {
        r.v = add;
        r.base = s;
        r.unknown = x->pass == 1;
    }
    return r;
}

static struct gval gx_or(struct gx *x);

static struct gval gx_primary(struct gx *x)
{
    struct gas *g = x->g;
    gx_ws(x);
    const char *p = x->p;
    if (*p == '(') {
        x->p++;
        struct gval v = gx_or(x);
        gx_ws(x);
        if (*x->p == ')') x->p++;
        else gx_err(x, "a `)` is missing in \"%s\"", p);
        return v;
    }
    if (*p == '-' || *p == '~' || *p == '!' || *p == '+') {
        x->p++;
        struct gval v = gx_primary(x);
        if (*p == '+')
            return v;
        if (v.sec != SEC_ABS) {
            if (!v.unknown)
                gx_err(x, "\"%s\": only a plain value can be negated or "
                       "inverted", p);
            return gx_abs(0);
        }
        return gx_abs(*p == '-' ? -v.v : *p == '~' ? ~v.v : !v.v);
    }
    if (*p == '\'') {               /* 'c, or 'c' */
        long c = (unsigned char)p[1];
        x->p += 2;
        if (p[1] == '\\' && p[2]) {
            c = p[2] == 'n' ? '\n' : p[2] == 't' ? '\t' : p[2] == '0' ? 0
              : (unsigned char)p[2];
            x->p++;
        }
        if (*x->p == '\'') x->p++;
        return gx_abs(c);
    }
    if (isdigit((unsigned char)*p)) {
        const char *e = p;
        while (isalnum((unsigned char)*e)) e++;
        /* `1f` / `1b`: a numeric local label */
        if (e - p >= 2 && (e[-1] == 'f' || e[-1] == 'b') &&
            !(p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))) {
            int fwd, d = local_ref(p, (size_t)(e - p), &fwd);
            if (d >= 0) {
                char nm[32];
                int inst = fwd ? g->local_n[d] : g->local_n[d] - 1;
                x->p = e;
                if (inst < 0) {
                    gx_err(x, "\"%s\" refers back to no earlier label", p);
                    return gx_abs(0);
                }
                local_name(nm, sizeof nm, d, inst);
                return gx_sym(x, sym_get(g, nm, strlen(nm)));
            }
        }
        long v;
        if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) {
            v = strtol(p + 2, (char **)&x->p, 2);
        } else {
            v = (long)strtoul(p, (char **)&x->p, 0);
        }
        if (is_symc((unsigned char)*x->p)) {
            gx_err(x, "\"%s\" is not a number", p);
            while (is_symc((unsigned char)*x->p)) x->p++;
        }
        return gx_abs(v);
    }
    if (*p == '.' && !is_symc((unsigned char)p[1])) {
        x->p++;
        return gx_here(x);
    }
    if (is_sym0((unsigned char)*p)) {
        const char *e = p;
        while (is_symc((unsigned char)*e)) e++;
        x->p = e;
        return gx_sym(x, sym_get(g, p, (size_t)(e - p)));
    }
    gx_err(x, "an expression was expected at \"%s\"", p);
    return gx_abs(0);
}

/* Both sides plain values: the arithmetic. Otherwise refused, unless one
 * side is not placed yet (pass 1), when the answer does not matter. */
static struct gval gx_arith(struct gx *x, struct gval a, struct gval b,
                            int op)
{
    if (a.sec != SEC_ABS || b.sec != SEC_ABS) {
        if (!a.unknown && !b.unknown)
            gx_err(x, "\"%s\": this operator takes plain values, not "
                   "addresses", x->p);
        return gx_abs(0);
    }
    switch (op) {
    case '*': return gx_abs(a.v * b.v);
    case '/': case '%':
        if (!b.v) { gx_err(x, "division by zero at \"%s\"", x->p); return gx_abs(0); }
        return gx_abs(op == '/' ? a.v / b.v : a.v % b.v);
    case 'l': return gx_abs((long)((unsigned long)a.v << (b.v & 63)));
    case 'r': return gx_abs((long)((unsigned long)a.v >> (b.v & 63)));
    case '|': return gx_abs(a.v | b.v);
    case '&': return gx_abs(a.v & b.v);
    case '^': return gx_abs(a.v ^ b.v);
    case '!': return gx_abs(a.v | ~b.v);         /* GNU as's `or not` */
    case '=': return gx_abs(a.v == b.v ? -1 : 0); /* GNU as: true is -1 */
    case 'n': return gx_abs(a.v != b.v ? -1 : 0);
    case '<': return gx_abs(a.v < b.v ? -1 : 0);
    case '>': return gx_abs(a.v > b.v ? -1 : 0);
    case 'L': return gx_abs(a.v <= b.v ? -1 : 0);
    case 'G': return gx_abs(a.v >= b.v ? -1 : 0);
    case 'A': return gx_abs(a.v && b.v);
    default:  return gx_abs(a.v || b.v);
    }
}

static int gx_op(struct gx *x, const char *const *ops, int *code)
{
    static const struct { const char *s; int c; } all[] = {
        { "<<", 'l' }, { ">>", 'r' }, { "==", '=' }, { "!=", 'n' },
        { "<>", 'n' }, { "<=", 'L' }, { ">=", 'G' }, { "&&", 'A' },
        { "||", 'O' }, { "*", '*' }, { "/", '/' }, { "%", '%' },
        { "|", '|' }, { "&", '&' }, { "^", '^' }, { "!", '!' },
        { "+", '+' }, { "-", '-' }, { "<", '<' }, { ">", '>' },
    };
    gx_ws(x);
    for (size_t k = 0; k < sizeof all / sizeof all[0]; k++) {
        size_t n = strlen(all[k].s);
        if (strncmp(x->p, all[k].s, n))
            continue;
        for (int j = 0; ops[j]; j++)
            if (!strcmp(ops[j], all[k].s)) {
                x->p += n;
                *code = all[k].c;
                return 1;
            }
        return 0;             /* the longest match is another level's */
    }
    return 0;
}

static struct gval gx_mul(struct gx *x)
{
    static const char *const ops[] = { "*", "/", "%", "<<", ">>", NULL };
    struct gval a = gx_primary(x);
    int op;
    while (gx_op(x, ops, &op))
        a = gx_arith(x, a, gx_primary(x), op);
    return a;
}

static struct gval gx_bit(struct gx *x)
{
    static const char *const ops[] = { "|", "&", "^", "!", NULL };
    struct gval a = gx_mul(x);
    int op;
    while (gx_op(x, ops, &op))
        a = gx_arith(x, a, gx_mul(x), op);
    return a;
}

static struct gval gx_add(struct gx *x)
{
    static const char *const ops[] = { "+", "-", "==", "!=", "<>", "<",
                                       ">", "<=", ">=", NULL };
    struct gval a = gx_bit(x);
    int op;
    while (gx_op(x, ops, &op)) {
        struct gval b = gx_bit(x);
        if (op == '+') {
            if (a.sec == SEC_ABS && b.sec == SEC_ABS) {
                a.v += b.v;
            } else if (b.sec == SEC_ABS) {
                a.v += b.v;
            } else if (a.sec == SEC_ABS) {
                b.v += a.v;
                a = b;
            } else {
                if (!a.unknown && !b.unknown)
                    gx_err(x, "\"%s\": two addresses cannot be added", x->p);
                a = gx_abs(0);
                a.unknown = 1;
            }
        } else if (op == '-') {
            if (b.sec == SEC_ABS) {
                a.v -= b.v;
            } else if (a.sec >= 0 && a.sec == b.sec) {
                a = gx_abs(a.v - b.v);          /* same section: a distance */
            } else {
                if (!a.unknown && !b.unknown)
                    gx_err(x, "\"%s\": the difference of two addresses in "
                           "different sections is not a value", x->p);
                a = gx_abs(0);
                a.unknown = 1;
            }
        } else {
            if (a.sec >= 0 && a.sec == b.sec) {   /* comparing two labels */
                a.sec = b.sec = SEC_ABS;
                a.base = b.base = NULL;
            }
            a = gx_arith(x, a, b, op);
        }
    }
    return a;
}

static struct gval gx_or(struct gx *x)
{
    static const char *const ops[] = { "&&", "||", NULL };
    struct gval a = gx_add(x);
    int op;
    while (gx_op(x, ops, &op))
        a = gx_arith(x, a, gx_add(x), op);
    return a;
}

/* Evaluates the expression at *pp, advancing it past. */
static struct gval gx_eval(struct gas *g, const char **pp, int pass, int *bad)
{
    struct gx x = { g, *pp, pass, 0 };
    struct gval v = gx_or(&x);
    gx_ws(&x);
    *pp = x.p;
    if (bad)
        *bad = x.bad;
    return v;
}

/* An expression that must be a plain value here and now: a count, a
 * size, an alignment. A label not placed yet is refused in pass 1 too,
 * because the layout depends on the answer. */
static int gx_abs_now(struct gas *g, const char **pp, int pass, long *out,
                      const char *what)
{
    int bad;
    const char *start = *pp;
    struct gval v = gx_eval(g, pp, pass, &bad);
    if (bad)
        return 0;
    if (v.sec != SEC_ABS) {
        gerr(g, "%s \"%s\" must be a plain value known where it stands",
             what, start);
        return 0;
    }
    *out = v.v;
    return 1;
}

/* The next comma-separated operand: [*pp, end), skipping over strings
 * and parentheses. */
static const char *gx_operand_end(const char *p)
{
    int depth = 0, q = 0;
    for (; *p; p++) {
        if (q) {
            if (*p == '\\' && p[1]) { p++; continue; }
            if (*p == q) q = 0;
            continue;
        }
        if (*p == '"') q = '"';
        else if (*p == '(') depth++;
        else if (*p == ')') depth--;
        else if (*p == ',' && depth <= 0) break;
    }
    return p;
}

static char *substitute(struct gas *g, const char *stmt, long pc,
                        int pass, char **ext, int *placeheld)
{
    size_t cap = strlen(stmt) * 2 + 64, len = 0;
    char *out = xmalloc(cap);
    const char *p = stmt;
    *ext = NULL;
    *placeheld = 0;
    while (*p) {
        /* An identifier cannot BEGIN in the middle of an alphanumeric
         * run. Without that test the `x` of `0x80800000` started one,
         * "x80800000" was not a register or a known label, and the
         * constant came out as `0.+0`. */
        int mid = p > stmt && (is_symc((unsigned char)p[-1]));
        /* A numeric local reference starts with a DIGIT, so the identifier
         * scanner below would never see it: `2f` was read as the number 2
         * followed by a stray `f`. Recognised here, and only when the digits
         * are followed by exactly one 'f' or 'b' and then a non-identifier
         * character -- so `0x1f` and a plain `63` are untouched. */
        int lref = 0;
        if (!mid && isdigit((unsigned char)*p)) {
            const char *d = p;
            while (isdigit((unsigned char)*d)) d++;
            if ((*d == 'f' || *d == 'b') && !is_symc((unsigned char)d[1]))
                lref = 1;
        }
        if ((!is_sym0((unsigned char)*p) || mid) && !lref) {
            if (len + 2 >= cap) { cap *= 2; out = xrealloc(out, cap); }
            out[len++] = *p++;
            continue;
        }
        const char *s = p;
        if (lref) {
            while (isdigit((unsigned char)*p)) p++;
            p++;                              /* the 'f' or 'b' */
        } else {
            while (*p && is_symc((unsigned char)*p)) p++;
        }
        size_t n = (size_t)(p - s);
        /* `.` alone is here: `b .` is a branch to itself */
        if (n == 1 && s[0] == '.') {
            if (len + 4 >= cap) { cap = cap * 2 + 8; out = xrealloc(out, cap); }
            memcpy(out + len, ".+0", 3);
            len += 3;
            continue;
        }
        /* The mnemonic itself is never a label, and a register name is
         * never one either -- the target answers that, so this file
         * needs no register table of its own. Everything else that is
         * an identifier IS a symbol reference. */
        int first = s == skip_ws((char *)stmt);
        int reg = (g->tgt->is_reg && g->tgt->is_reg(s, (int)n) >= 0) ||
                  (g->tgt->is_word && g->tgt->is_word(stmt, s, (int)n));
        /* `1f` / `1b`: the next or previous definition of numeric local 1.
         * Rewritten to the generated name and then treated as any other
         * label, so nothing downstream needs to know about them. `1f` is the
         * instance that has NOT been defined yet -- local_n[d] is the count
         * so far -- which is exactly what makes it forward. */
        if (!first && !reg) {
            int fwd, d = local_ref(s, n, &fwd);
            if (d >= 0) {
                char nm[32];
                int inst = fwd ? g->local_n[d] : g->local_n[d] - 1;
                if (inst < 0) {
                    gerr(g, "'%.*s' refers backwards and there is no earlier "
                            "'%d:'", (int)n, s, d);
                    inst = 0;
                }
                local_name(nm, sizeof nm, d, inst);
                struct sym *ly = sym_find(g, nm, strlen(nm));
                if (ly && ly->sec >= 0) {
                    char rep[32];
                    long disp = ly->value - pc;
                    int k = snprintf(rep, sizeof rep, ".%+ld", disp);
                    if (len + (size_t)k + 2 >= cap) {
                        cap = cap * 2 + (size_t)k; out = xrealloc(out, cap);
                    }
                    memcpy(out + len, rep, (size_t)k);
                    len += (size_t)k;
                    continue;
                }
                /* A forward one, not placed yet: pass one needs only the
                 * width, and these instructions are fixed-width. */
                if (len + 4 >= cap) { cap = cap * 2 + 8; out = xrealloc(out, cap); }
                memcpy(out + len, ".+0", 3);
                len += 3;
                *placeheld = 1;
                if (pass == 2)
                    gerr(g, "'%.*s' refers forward and there is no later "
                            "'%d:'", (int)n, s, d);
                continue;
            }
        }
        if (!first && !reg) {
            struct sym *sy = sym_find(g, s, n);
            long aadd = 0;
            int hops = 0;
            while (sy && sy->alias && hops++ < 64) {   /* `.set a, b` */
                aadd += sy->alias_add;
                sy = sy->alias;
            }
            if (sy && sy->sec == SEC_ABS) {     /* `.equ N, 5`: the value */
                char rep[32];
                int k = snprintf(rep, sizeof rep, "%ld", sy->value + aadd);
                if (len + (size_t)k + 2 >= cap) { cap = cap * 2 + (size_t)k;
                                                  out = xrealloc(out, cap); }
                memcpy(out + len, rep, (size_t)k);
                len += (size_t)k;
                continue;
            }
            if (sy && sy->sec >= 0 && aadd) {
                gerr(g, "'%.*s' is an alias with an offset, which an "
                        "instruction operand cannot take here", (int)n, s);
            }
            /* A label in ANOTHER section is not a displacement: where the
             * two sections land is the linker's business. Nor is a weak
             * symbol, whose definition the link may replace. Both go to
             * the relocating forms, as an external symbol does. */
            if (sy && sy->sec >= 0 && (sy->sec != g->cur || sy->is_weak) &&
                pass == 2) {
                if (!*ext)
                    *ext = sy->name;
                if (len + n + 2 >= cap) { cap = cap * 2 + n; out = xrealloc(out, cap); }
                memcpy(out + len, s, n);
                len += n;
                continue;
            }
            if (sy && sy->sec >= 0 && (sy->sec != g->cur || sy->is_weak)) {
                /* pass 1: the size only, as for a forward label */
                if (len + 4 >= cap) { cap = cap * 2 + 8; out = xrealloc(out, cap); }
                memcpy(out + len, ".+0", 3);
                len += 3;
                *placeheld = 1;
                continue;
            }
            if (sy && sy->sec >= 0) {          /* a label in this file */
                char rep[32];
                long disp = sy->value - pc;
                int k = snprintf(rep, sizeof rep, ".%+ld", disp);
                if (len + (size_t)k + 2 >= cap) { cap = cap * 2 + (size_t)k;
                                                  out = xrealloc(out, cap); }
                memcpy(out + len, rep, (size_t)k);
                len += (size_t)k;
                continue;
            }
            if (pass == 1) {
                /* Pass one only needs the SIZE, and a forward label is
                 * not placed yet. A zero displacement encodes to the
                 * same width as the real one will -- these are
                 * fixed-width instructions -- so the size is right and
                 * pass two does the arithmetic. */
                if (len + 4 >= cap) { cap = cap * 2 + 8; out = xrealloc(out, cap); }
                memcpy(out + len, ".+0", 3);
                len += 3;
                *placeheld = 1;
                continue;
            }
            /* Pass two, and still not a label here: an external
             * symbol, which only the relocation-carrying forms accept. */
            if (!*ext) {
                struct sym *u = sym_get(g, s, n);
                *ext = u->name;
            }
        }
        if (len + n + 2 >= cap) { cap = cap * 2 + n; out = xrealloc(out, cap); }
        memcpy(out + len, s, n);
        len += n;
    }
    out[len] = '\0';
    return out;
}

/* RISC-V's `lw rd, sym` (and lb, lbu, lh, lhu, lwu, ld): a load from a
 * symbol, which GNU as makes `auipc rd, %pcrel_hi(sym)` and
 * `lw rd, %pcrel_lo(.L)(rd)` -- the `la` pair with a load where the add
 * is. FreeRTOS's RISC-V port reads pxCurrentTCB and the critical-nesting
 * count this way. Returns the mnemonic's length when the statement is one:
 * a load, a register, and a bare symbol (an address, `0(sp)`, is not). */
static int rv_load_sym(const struct gas *g, const char *p)
{
    static const char *const ld[] = { "lbu", "lhu", "lwu", "lb", "lh",
                                      "lw", "ld" };
    if (g->tgt->machine != EM_RISCV)
        return 0;
    for (unsigned k = 0; k < sizeof ld / sizeof ld[0]; k++) {
        size_t n = strlen(ld[k]);
        if (strncmp(p, ld[k], n) != 0 || !isspace((unsigned char)p[n]))
            continue;
        const char *q = skip_ws((char *)p + n);
        while (*q && is_symc((unsigned char)*q)) q++;          /* rd */
        q = skip_ws((char *)q);
        if (*q != ',')
            return 0;
        q = skip_ws((char *)q + 1);
        if (!is_sym0((unsigned char)*q))
            return 0;
        while (*q && is_symc((unsigned char)*q)) q++;
        q = skip_ws((char *)q);
        return *q == '\0' || *q == '#' ? (int)n : 0;
    }
    return 0;
}

/* `call sym` and `la rd, sym`: the two forms that may name a symbol
 * this file does not define. Both are an auipc paired with a second
 * instruction, eight bytes, and both carry their relocation on the
 * auipc. Anything else naming an undefined symbol is refused. */
static int extern_form(struct gas *g, const char *stmt, long pc, int pass,
                       const char *ext)
{
    const char *p = skip_ws((char *)stmt);
    /* A target that owns its symbol forms answers first. AVR has eight of
     * them and they are not shaped like RISC-V's pair or ARM's single `bl`
     * -- an address loaded a byte at a time needs one relocation per byte
     * -- so the target rewrites the statement and says where each site
     * goes, rather than this file growing a branch per machine. */
    if (g->tgt->symform) {
        struct asm_symform f;
        int lfwd;
        if (g->tgt->symform(stmt, &f) &&
            local_ref(stmt + f.sym_at, (size_t)f.sym_len, &lfwd) < 0) {
            struct code tmp = { 0 };
            char err[256];
            if (g->tgt->encode(f.encode, &tmp, err, sizeof err) != 0) {
                gerr(g, "%s", err);
                free(tmp.p);
                return 1;
            }
            if (pass == 2)
                for (int k = 0; k < f.nsites; k++)
                    fix_add(g, g->cur, pc + f.site[k].off, ext,
                            f.site[k].reloc, f.addend);
            emit_bytes(g, (const unsigned char *)tmp.p, tmp.len);
            free(tmp.p);
            return 1;
        }
    }
    int is_call = strncmp(p, "call", 4) == 0 && isspace((unsigned char)p[4]);
    int is_la = strncmp(p, "la", 2) == 0 && isspace((unsigned char)p[2]);
    int ld_len = rv_load_sym(g, p);           /* `lw rd, sym` */
    if (ld_len)
        is_la = 1;                            /* the same pair, but a load */
    /* ARM's jump to a symbol defined elsewhere -- a tail call, an RTOS's
     * branch into its C half -- is `b sym`: one wide branch and one
     * R_ARM_THM_JUMP24, as a call is `bl` and R_ARM_THM_CALL. */
    if (g->tgt->machine == EM_ARM &&
        ((p[0] == 'b' && isspace((unsigned char)p[1])) ||
         (strncmp(p, "b.w", 3) == 0 && isspace((unsigned char)p[3])))) {
        struct code tmp = { 0 };
        char err[256];
        if (pass == 2)
            fix_add(g, g->cur, pc, ext, R_ARM_THM_JUMP24, 0);
        if (g->tgt->encode("b .+0", &tmp, err, sizeof err) != 0)
            gerr(g, "%s", err);
        else
            emit_bytes(g, (const unsigned char *)tmp.p, tmp.len);
        free(tmp.p);
        return 1;
    }
    /* ARM writes a call to a symbol as `bl sym`, one instruction
     * carrying one relocation -- there is no auipc pair to build. */
    if ((g->tgt->machine == EM_ARM || g->tgt->machine == EM_AARCH64) &&
        strncmp(p, "bl", 2) == 0 && isspace((unsigned char)p[2])) {
        struct code tmp = { 0 };
        char err[256];
        if (pass == 2)
            fix_add(g, g->cur, pc, ext, g->tgt->r_call, 0);
        if (g->tgt->encode("bl .+0", &tmp, err, sizeof err) != 0) {
            gerr(g, "%s", err);
        } else {
            emit_bytes(g, (const unsigned char *)tmp.p, tmp.len);
        }
        free(tmp.p);
        return 1;
    }
    if (!is_call && !is_la)
        return 0;
    if (pass == 2) {
        if (is_call && !g->tgt->r_call) {
            gerr(g, "this target has no `call` relocation");
            return 1;
        }
        if (is_la && !g->tgt->r_pcrel_hi) {
            gerr(g, "this target has no `la` relocation");
            return 1;
        }
        fix_add(g, g->cur, pc, ext,
                is_call ? g->tgt->r_call : g->tgt->r_pcrel_hi, 0);
        if (is_la) {
            /* The LOW half's relocation names the ADDRESS OF THE
             * PAIRED auipc, not the symbol -- that is how the psABI
             * lets the linker recover the displacement the high half
             * rounded. A real assembler emits a local label on the
             * auipc and points the low half at it, so that is what
             * this does; naming the symbol assembles cleanly and
             * computes the wrong address. */
            char lbl[32];
            struct sym *l;
            snprintf(lbl, sizeof lbl, ".Lpcrel_hi%d", g->npcrel++);
            l = sym_get(g, lbl, strlen(lbl));
            l->sec = g->cur;
            l->value = pc;
            fix_add(g, g->cur, pc + 4, l->name, g->tgt->r_pcrel_lo, 0);
        }
    }
    /* The bytes: auipc <rd>, 0 then the pairing instruction, both with
     * a zero field for the linker to fill. For `call` the pair is
     * `jalr ra, 0(ra)`; for `la` an `addi rd, rd, 0`. */
    {
        char buf[128];
        struct code tmp = { 0 };
        char err[256];
        const char *rd = "ra";
        char rdbuf[16];
        if (is_la) {
            const char *q = skip_ws((char *)p + (ld_len ? ld_len : 2));
            size_t n = 0;
            while (q[n] && is_symc((unsigned char)q[n]) && n < sizeof rdbuf - 1)
                n++;
            memcpy(rdbuf, q, n); rdbuf[n] = '\0';
            rd = rdbuf;
        }
        snprintf(buf, sizeof buf, "auipc %s, 0", rd);
        if (g->tgt->encode(buf, &tmp, err, sizeof err) != 0) {
            gerr(g, "%s", err); free(tmp.p); return 1;
        }
        if (ld_len)
            snprintf(buf, sizeof buf, "%.*s %s, 0(%s)", ld_len, p, rd, rd);
        else
            snprintf(buf, sizeof buf, is_call ? "jalr ra, 0(ra)"
                                              : "addi %s, %s, 0", rd, rd);
        if (g->tgt->encode(buf, &tmp, err, sizeof err) != 0) {
            gerr(g, "%s", err); free(tmp.p); return 1;
        }
        emit_bytes(g, (const unsigned char *)tmp.p, tmp.len);
        free(tmp.p);
    }
    return 1;
}

/* Is this statement one of the two relocation-carrying pseudos? Asked
 * BEFORE substitution and in both passes, because pass one needs their
 * size (eight bytes, a pair) and substitution would otherwise turn the
 * symbol they name into a displacement. */
static const char *pseudo_symbol(struct gas *g, const char *stmt)
{
    const char *p = skip_ws((char *)stmt);
    const char *q;
    size_t n;
    int arm_b = g->tgt->machine == EM_ARM &&
                ((p[0] == 'b' && isspace((unsigned char)p[1])) ||
                 (strncmp(p, "b.w", 3) == 0 && isspace((unsigned char)p[3])));
    if (((g->tgt->machine == EM_ARM || g->tgt->machine == EM_AARCH64) &&
         strncmp(p, "bl", 2) == 0 && isspace((unsigned char)p[2])) || arm_b) {
        const char *b = skip_ws((char *)(p + (p[1] == '.' ? 3 :
                                              p[1] == 'l' ? 2 : 1)));
        size_t bn = 0;
        while (b[bn] && is_symc((unsigned char)b[bn])) bn++;
        /* Only when it names a SYMBOL: `bl .+8` and `b .` are ordinary
         * displacements the statement assembler handles. */
        if (bn == 1 && b[0] == '.')
            return NULL;
        if (bn && is_sym0((unsigned char)b[0]) &&
            !sym_find(g, b, bn))
            return sym_get(g, b, bn)->name;
        if (bn && is_sym0((unsigned char)b[0])) {
            struct sym *sy = sym_find(g, b, bn);
            /* undefined, in another section, or weak: relocated */
            if (sy && (sy->sec < 0 || sy->sec != g->cur || sy->is_weak))
                return sy->name;
        }
        return NULL;
    }
    /* A target-owned symbol form, asked BEFORE the RISC-V pseudos: on AVR
     * `call sym` is one of these too, and its symbol may be a label defined
     * in this file -- which still needs a relocation, because `call` carries
     * an absolute word address and a relocatable object does not know where
     * its own section lands. */
    if (g->tgt->symform) {
        struct asm_symform f;
        if (g->tgt->symform(stmt, &f)) {
            /* ...unless the operand is a NUMERIC LOCAL reference. `1b` and
             * `2f` are identifiers as far as a target's parser can tell, so
             * `rjmp 1b` was claimed here and relocated against a symbol
             * named "1b" -- which produced a forward jump of zero where the
             * source meant the top of the loop. Numeric locals belong to
             * this file, which owns their numbering, so they are resolved by
             * substitute() and never offered to a target. */
            int fwd;
            if (local_ref(stmt + f.sym_at, (size_t)f.sym_len, &fwd) < 0)
                return sym_get(g, stmt + f.sym_at, (size_t)f.sym_len)->name;
        }
    }
    int ld_len = rv_load_sym(g, p);
    if (!((strncmp(p, "call", 4) == 0 && isspace((unsigned char)p[4])) ||
          (strncmp(p, "la", 2) == 0 && isspace((unsigned char)p[2])) ||
          ld_len))
        return NULL;
    q = p + (ld_len ? ld_len : p[1] == 'a' && p[2] != 'l' ? 2 : 4);
    q = skip_ws((char *)q);
    if (*p == 'l') {                /* la/lw rd, sym -- skip rd */
        while (*q && is_symc((unsigned char)*q)) q++;
        q = skip_ws((char *)q);
        if (*q == ',') q = skip_ws((char *)(q + 1));
    }
    n = 0;
    while (q[n] && is_symc((unsigned char)q[n])) n++;
    if (!n) return NULL;
    return sym_get(g, q, n)->name;
}

static int pool_entry(struct gas *g, int is_sym, long v, struct sym *sy);
static long pool_base_prev(struct gas *g);

/* ARM's `ldr rd, =EXPR`: a constant that a move can make is that move
 * (t_ldr_const, as GNU as and LLVM choose it); anything else is a word
 * in the literal pool, loaded pc-relative -- two bytes for r0-r7 within
 * 1020 bytes forward, four otherwise, relaxed like a branch. Returns 1
 * when the statement was one. */
static int ldr_literal(struct gas *g, const char *stmt, int pass)
{
    const char *p = skip_ws((char *)stmt), *r, *e;
    int want = 0, rd, bad, k;
    struct code tmp = { 0 };
    if (strncmp(p, "ldr", 3))
        return 0;
    p += 3;
    if (p[0] == '.' && (p[1] == 'w' || p[1] == 'n')) {
        want = p[1];
        p += 2;
    }
    if (!isspace((unsigned char)*p))
        return 0;
    p = skip_ws((char *)p);
    r = p;
    while (is_symc((unsigned char)*p)) p++;
    rd = g->tgt->is_reg ? g->tgt->is_reg(r, (int)(p - r)) : -1;
    p = skip_ws((char *)p);
    if (rd < 0 || *p != ',')
        return 0;
    p = skip_ws((char *)p + 1);
    if (*p != '=')
        return 0;
    p++;
    long pc = cur_off(g);
    e = p;
    struct gval v = gx_eval(g, &e, pass, &bad);
    if (bad)
        return 1;
    if (*skip_ws((char *)e)) {
        gerr(g, "\"%s\" is not one expression", p);
        return 1;
    }
    map_mark(g, 't');
    if (v.sec == SEC_ABS && !v.unknown && want != 'n' &&
        t_ldr_const(&tmp, rd, (unsigned long)v.v)) {
        emit_bytes(g, (const unsigned char *)tmp.p, tmp.len);
        free(tmp.p);
        return 1;
    }
    if (v.sec == SEC_ABS && !v.unknown)
        k = pool_entry(g, 0, v.v, NULL);
    else if (v.sec >= 0)
        k = pool_entry(g, 1, v.v - v.base->value, v.base);
    else
        k = pool_entry(g, 1, v.v, v.base);
    long base = pool_base_prev(g);
    int narrow = !g->wide[g->li] && rd <= 7 && want != 'w';
    long off = base < 0 ? 0 : base + 4L * k - ((pc + 4) & ~3L);
    if (narrow && t_ldr_lit16(&tmp, rd, off)) {
        emit_bytes(g, (const unsigned char *)tmp.p, tmp.len);
        free(tmp.p);
        return 1;
    }
    if (narrow && base >= 0) {             /* out of the narrow reach */
        g->wide[g->li] = 1;
        g->grew = 1;
    }
    if (!t_ldr_lit(&tmp, rd, off))
        gerr(g, "the literal pool is %ld bytes away, beyond ldr's reach: "
                "put a .ltorg nearer", off);
    else
        emit_bytes(g, (const unsigned char *)tmp.p, tmp.len);
    free(tmp.p);
    return 1;
}

static void instruction(struct gas *g, char *stmt, int pass)
{
    long pc = cur_off(g);
    char *ext = NULL;
    char *text;
    int placeheld = 0;
    if (g->tgt->machine == EM_ARM) {
        if (ldr_literal(g, stmt, pass))
            return;
        map_mark(g, 't');
    }
    const char *ps = pseudo_symbol(g, stmt);

    if (ps) {
        extern_form(g, stmt, pc, pass, ps);
        return;
    }
    text = substitute(g, stmt, pc, pass, &ext, &placeheld);
    struct code tmp = { 0 };
    char err[256];

    /* ARM's `ldr rd, label`: a word loaded from a literal this file
     * defines, PC-relative from Align(pc, 4) -- which only this layer can
     * compute, since only it knows where the instruction is. The label
     * has become `.+N`; it becomes `[pc, #off]`. Always the 32-bit form,
     * so the length is the same in both passes. */
    if (g->tgt->machine == EM_ARM && !ext) {
        const char *q = skip_ws(text);
        int wide = strncmp(q, "ldr.w", 5) == 0 && isspace((unsigned char)q[5]);
        if (wide || (strncmp(q, "ldr", 3) == 0 && isspace((unsigned char)q[3]))) {
            const char *r = skip_ws((char *)(q + (wide ? 5 : 3)));
            const char *c = strchr(r, ',');
            const char *d = c ? skip_ws((char *)(c + 1)) : NULL;
            long disp = 0;
            char *e = NULL;
            if (d && d[0] == '.' && (d[1] == '+' || d[1] == '-'))
                disp = strtol(d + 1, &e, 10);
            if (e && *skip_ws(e) == 0) {
                char buf[96];
                long off = (pc + disp) - ((pc + 4) & ~3L);
                snprintf(buf, sizeof buf, "ldr %.*s, [pc, #%ld]",
                         (int)(c - r), r, off);
                free(text);
                text = xmalloc(strlen(buf) + 1);
                memcpy(text, buf, strlen(buf) + 1);
            }
        }
    }

    if (ext) {
        if (!extern_form(g, text, pc, pass, ext))
            gerr(g, "\"%s\" names '%s', which is %s, in a form this "
                    "target's assembler cannot relocate", stmt, ext,
                 sym_find(g, ext, strlen(ext)) &&
                 sym_find(g, ext, strlen(ext))->sec >= 0
                     ? (sym_find(g, ext, strlen(ext))->is_weak
                        ? "weak (the link may replace it)"
                        : "in another section")
                     : "not defined in this file");
        free(text);
        return;
    }
    if (g->tgt->machine == EM_ARM) {
        tasm_set_wide(g->wide[g->li]);
        (void)tasm_took_wide();
    }
    int erc = g->tgt->encode(text, &tmp, err, sizeof err);
    if (g->tgt->machine == EM_ARM) {
        if (tasm_took_wide() && !g->wide[g->li]) {
            g->wide[g->li] = 1;      /* wide from now on, so passes settle */
            g->grew = 1;
        }
        tasm_set_wide(1);
    }
    if (erc != 0) {
        /* A pass-one failure on a statement that still holds a
         * PLACEHOLDER is not the statement's fault yet: the name may
         * be a label further down, and `.+0` is only valid where a
         * displacement belongs. Reserve the instruction's width and
         * let pass two, which has every label, produce the real
         * diagnostic -- otherwise every forward reference reports as
         * "wants registers". */
        if (pass == 1 && placeheld)
            advance(g, 4);
        else
            gerr(g, "%s", err);
    } else {
        emit_bytes(g, (const unsigned char *)tmp.p, tmp.len);
    }
    free(tmp.p);
    free(text);
}

/* ---- the source, expanded ---------------------------------------------
 *
 * Before the two passes the file becomes a list of statements, each with
 * the line it came from, and everything that rewrites source is done:
 * C comments and the target's line comments go, a line becomes several
 * where `;` separates statements, and the macro language runs --
 * .macro/.endm (`\param`, defaults, :req, :vararg, `\@`, `\()`),
 * .rept/.irp/.irpc/.endr, the .if family and .include. CMSIS startup
 * files declare their default handlers with a macro; a hand-written
 * RTOS port unrolls a context save with .rept. Doing it here keeps both
 * passes looking at the same statements.
 *
 * An .if is decided when it is reached, from what is known there: numbers
 * and the .equ/.set values seen so far. A label's address is not known
 * until the passes run, so an .if that depends on one is refused rather
 * than guessed. */

struct gline {
    char *text;
    int line;
};

struct gmacro {
    char *name;
    char **param, **dflt;
    int *req;
    int nparam, vararg;
    struct gline *body;
    int nbody;
};

struct gexp {
    struct gas *g;          /* for diagnostics, and the .if values */
    struct gline *out;
    int nout, cap;
    struct gmacro *mac;
    int nmac, capmac;
    long counter;           /* \@ */
    int depth;
    int exitm;              /* .exitm seen in the macro being expanded */
};

static void gx_emit(struct gexp *x, const char *text, size_t n, int line)
{
    if (x->nout == x->cap) {
        x->cap = x->cap ? x->cap * 2 : 256;
        x->out = xrealloc(x->out, (size_t)x->cap * sizeof *x->out);
    }
    x->out[x->nout].text = xstrndup(text, n);
    x->out[x->nout].line = line;
    x->nout++;
}

/* C comments out of the whole text, newlines kept so line numbers hold. */
static void strip_c_comments(char *s)
{
    int q = 0;
    for (char *p = s; *p; p++) {
        if (q) {
            if (*p == '\\' && p[1]) { p++; continue; }
            if (*p == q || *p == '\n') q = 0;
            continue;
        }
        if (*p == '"') { q = '"'; continue; }
        if (p[0] == '/' && p[1] == '*') {
            char *e = strstr(p + 2, "*/");
            char *stop = e ? e + 2 : p + strlen(p);
            for (char *k = p; k < stop; k++)
                if (*k != '\n') *k = ' ';
            p = stop - 1;
        }
    }
}

/* The first word of a statement, after any labels, and where it ends. */
static const char *gx_word(const char *s, size_t *n)
{
    const char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    const char *w = p;
    while (*p && !isspace((unsigned char)*p) && *p != ',') p++;
    *n = (size_t)(p - w);
    return w;
}

static int gx_is(const char *w, size_t n, const char *kw)
{
    return n == strlen(kw) && !strncmp(w, kw, n);
}

/* macro names are matched without regard to case, as GNU as does */
static int gx_ncase(const char *a, const char *b, size_t n)
{
    for (size_t k = 0; k < n; k++)
        if (tolower((unsigned char)a[k]) != tolower((unsigned char)b[k]))
            return 1;
    return 0;
}

static struct gmacro *gx_find(struct gexp *x, const char *w, size_t n)
{
    for (int k = 0; k < x->nmac; k++)
        if (strlen(x->mac[k].name) == n && !gx_ncase(x->mac[k].name, w, n))
            return &x->mac[k];
    return NULL;
}

static void gx_lines(struct gexp *x, const struct gline *in, int n);

/* The value of an .if operand, from numbers and the .equ/.set values
 * seen so far. 0 and an error when it names anything else. */
static int gx_cond(struct gexp *x, const char *e, int line, long *out)
{
    struct gas *g = x->g;
    int bad, save = g->line;
    g->line = line;
    struct gval v = gx_eval(g, &e, 1, &bad);
    g->line = save;
    if (bad)
        return 0;
    if (v.sec != SEC_ABS) {
        g->line = line;
        gerr(g, ".if: \"%s\" is not a value known where it stands (a label's "
             "address is not, until the file is assembled)", e);
        g->line = save;
        return 0;
    }
    *out = v.v;
    return 1;
}

/* The body of a block: the lines up to its closing word, nested blocks
 * of the same kind counted. Returns the index after the closer. */
static int gx_block(struct gexp *x, const struct gline *in, int n, int i,
                    const char *const *open, const char *close,
                    struct gline **body, int *nbody)
{
    int depth = 1, cap = 0;
    *body = NULL;
    *nbody = 0;
    for (i++; i < n; i++) {
        size_t wn;
        const char *w = gx_word(in[i].text, &wn);
        for (int k = 0; open[k]; k++)
            if (gx_is(w, wn, open[k])) depth++;
        if (gx_is(w, wn, close) && --depth == 0)
            return i + 1;
        if (*nbody == cap) {
            cap = cap ? cap * 2 : 16;
            *body = xrealloc(*body, (size_t)cap * sizeof **body);
        }
        (*body)[*nbody].text = in[i].text;
        (*body)[(*nbody)++].line = in[i].line;
    }
    x->g->line = in[n ? n - 1 : 0].line;
    gerr(x->g, "a block is never closed with %s", close);
    return n;
}

/* Text with every `\name` of `names` replaced by its value, `\@` by the
 * expansion count and `\()` by nothing. */
static char *gx_subst(const char *t, char **names, char **vals, int nv,
                      long counter)
{
    size_t cap = strlen(t) + 64, len = 0;
    char *o = xmalloc(cap);
    for (const char *p = t; *p; ) {
        const char *rep = NULL;
        char num[32];
        size_t skip = 1;
        if (*p == '\\') {
            if (p[1] == '@') {
                snprintf(num, sizeof num, "%ld", counter);
                rep = num;
                skip = 2;
            } else if (p[1] == '(' && p[2] == ')') {
                rep = "";
                skip = 3;
            } else {
                for (int k = 0; k < nv; k++) {
                    size_t ln = strlen(names[k]);
                    if (!strncmp(p + 1, names[k], ln) &&
                        !is_symc((unsigned char)p[1 + ln])) {
                        rep = vals[k];
                        skip = ln + 1;
                        break;
                    }
                }
            }
        }
        if (!rep) {
            rep = p;
            skip = 1;
            if (len + 2 >= cap) { cap *= 2; o = xrealloc(o, cap); }
            o[len++] = *p;
            p++;
            continue;
        }
        size_t rl = strlen(rep);
        if (len + rl + 2 >= cap) { cap = (cap + rl) * 2; o = xrealloc(o, cap); }
        memcpy(o + len, rep, rl);
        len += rl;
        p += skip;
    }
    o[len] = 0;
    return o;
}

static void gx_invoke(struct gexp *x, struct gmacro *m, const char *args,
                      int line)
{
    struct gas *g = x->g;
    char **vals = xcalloc((size_t)(m->nparam ? m->nparam : 1), sizeof *vals);
    int pos = 0;
    const char *p = args;
    g->line = line;
    if (x->depth > 100) {
        gerr(g, "macro %s expands more than 100 deep", m->name);
        free(vals);
        return;
    }
    /* arguments: separated by commas or blanks; "quoted" keeps both;
     * NAME=VALUE names one; the last :vararg takes the rest */
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;
        const char *s = p;
        char *val;
        int k = -1;
        const char *eq = NULL;
        for (const char *q = p; is_symc((unsigned char)*q) || *q == '='; q++)
            if (*q == '=') { eq = q; break; }
        if (eq) {
            for (int j = 0; j < m->nparam; j++)
                if (strlen(m->param[j]) == (size_t)(eq - p) &&
                    !strncmp(m->param[j], p, (size_t)(eq - p)))
                    k = j;
            if (k >= 0)
                s = p = eq + 1;
        }
        if (k < 0)
            k = pos++;
        if (k >= m->nparam) {
            gerr(g, "macro %s takes %d argument%s", m->name, m->nparam,
                 m->nparam == 1 ? "" : "s");
            break;
        }
        if (m->vararg && k == m->nparam - 1) {
            val = xstrndup(p, strlen(p));
            p += strlen(p);
        } else if (*p == '"') {
            const char *e = strchr(p + 1, '"');
            if (!e) e = p + strlen(p);
            val = xstrndup(p + 1, (size_t)(e - p - 1));
            p = *e ? e + 1 : e;
        } else {
            int depth = 0;
            while (*p && (depth || (*p != ',' && *p != ' ' && *p != '\t'))) {
                if (*p == '(') depth++;
                else if (*p == ')') depth--;
                p++;
            }
            val = xstrndup(s, (size_t)(p - s));
        }
        free(vals[k]);
        vals[k] = val;
    }
    for (int k = 0; k < m->nparam; k++)
        if (!vals[k]) {
            if (m->req[k])
                gerr(g, "macro %s: argument %s is required", m->name,
                     m->param[k]);
            vals[k] = xstrndup(m->dflt[k] ? m->dflt[k] : "",
                               strlen(m->dflt[k] ? m->dflt[k] : ""));
        }
    struct gline *body = xmalloc((size_t)(m->nbody ? m->nbody : 1) * sizeof *body);
    long count = x->counter++;
    for (int k = 0; k < m->nbody; k++) {
        body[k].text = gx_subst(m->body[k].text, m->param, vals, m->nparam,
                                count);
        body[k].line = line;
    }
    x->depth++;
    gx_lines(x, body, m->nbody);
    x->depth--;
    x->exitm = 0;
    for (int k = 0; k < m->nbody; k++)
        free(body[k].text);
    free(body);
    for (int k = 0; k < m->nparam; k++)
        free(vals[k]);
    free(vals);
}

static void gx_define(struct gexp *x, const char *rest, const struct gline *body,
                      int nbody, int line)
{
    struct gas *g = x->g;
    const char *p = rest;
    g->line = line;
    while (*p == ' ' || *p == '\t') p++;
    const char *ns = p;
    while (is_symc((unsigned char)*p)) p++;
    if (p == ns) {
        gerr(g, ".macro needs a name");
        return;
    }
    if (x->nmac == x->capmac) {
        x->capmac = x->capmac ? x->capmac * 2 : 8;
        x->mac = xrealloc(x->mac, (size_t)x->capmac * sizeof *x->mac);
    }
    struct gmacro *m = &x->mac[x->nmac++];
    memset(m, 0, sizeof *m);
    m->name = xstrndup(ns, (size_t)(p - ns));
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;
        const char *s = p;
        while (is_symc((unsigned char)*p)) p++;
        if (p == s) {
            gerr(g, ".macro %s: \"%s\" is not a parameter", m->name, s);
            break;
        }
        m->param = xrealloc(m->param, (size_t)(m->nparam + 1) * sizeof *m->param);
        m->dflt = xrealloc(m->dflt, (size_t)(m->nparam + 1) * sizeof *m->dflt);
        m->req = xrealloc(m->req, (size_t)(m->nparam + 1) * sizeof *m->req);
        m->param[m->nparam] = xstrndup(s, (size_t)(p - s));
        m->dflt[m->nparam] = NULL;
        m->req[m->nparam] = 0;
        if (*p == ':') {
            const char *q = ++p;
            while (is_symc((unsigned char)*p)) p++;
            if ((size_t)(p - q) == 3 && !strncmp(q, "req", 3))
                m->req[m->nparam] = 1;
            else if ((size_t)(p - q) == 6 && !strncmp(q, "vararg", 6))
                m->vararg = 1;
        }
        if (*p == '=') {
            const char *q = ++p;
            while (*p && *p != ',' && *p != ' ' && *p != '\t') p++;
            m->dflt[m->nparam] = xstrndup(q, (size_t)(p - q));
        }
        m->nparam++;
    }
    m->body = xmalloc((size_t)(nbody ? nbody : 1) * sizeof *m->body);
    for (int k = 0; k < nbody; k++) {
        m->body[k].text = xstrndup(body[k].text, strlen(body[k].text));
        m->body[k].line = body[k].line;
    }
    m->nbody = nbody;
}

static void gx_text(struct gexp *x, char *text, const char *path, int base);

static void gx_lines(struct gexp *x, const struct gline *in, int n)
{
    struct gas *g = x->g;
    /* .if state: each level is taking its lines, has taken some branch
     * already, and whether the level above takes any at all */
    int take[64], done[64], live[64], ncond = 0;
    static const char *const if_words[] = {
        ".if", ".ifdef", ".ifndef", ".ifnotdef", ".ifeq", ".ifne", ".ifgt",
        ".ifge", ".iflt", ".ifle", ".ifb", ".ifnb", ".ifc", ".ifnc",
        ".ifeqs", ".ifnes", NULL
    };
    static const char *const rept_words[] = { ".rept", ".irp", ".irpc", NULL };
    static const char *const mac_words[] = { ".macro", NULL };

    for (int i = 0; i < n && !x->exitm; ) {
        const char *t = in[i].text;
        int line = in[i].line;
        size_t wn;
        const char *w = gx_word(t, &wn);
        const char *rest = w + wn;
        int on = !ncond || (take[ncond - 1] && live[ncond - 1]);
        g->line = line;

        int is_if = 0;
        for (int k = 0; if_words[k]; k++)
            if (gx_is(w, wn, if_words[k])) is_if = 1;
        if (is_if) {
            int c = 0;
            if (ncond == 64) { gerr(g, ".if nests more than 64 deep"); return; }
            if (on) {
                long v = 0;
                const char *r = rest;
                while (*r == ' ' || *r == '\t') r++;
                if (gx_is(w, wn, ".ifdef") || gx_is(w, wn, ".ifndef") ||
                    gx_is(w, wn, ".ifnotdef")) {
                    size_t ln = 0;
                    while (is_symc((unsigned char)r[ln])) ln++;
                    struct sym *sy = sym_find(g, r, ln);
                    int def = sy && (sy->sec != SEC_UNDEF || sy->alias);
                    c = gx_is(w, wn, ".ifdef") ? def : !def;
                } else if (gx_is(w, wn, ".ifb") || gx_is(w, wn, ".ifnb")) {
                    c = (*r == 0) == gx_is(w, wn, ".ifb");
                } else if (gx_is(w, wn, ".ifc") || gx_is(w, wn, ".ifnc") ||
                           gx_is(w, wn, ".ifeqs") || gx_is(w, wn, ".ifnes")) {
                    const char *cm = gx_operand_end(r);
                    char *a = xstrndup(r, (size_t)(cm - r));
                    const char *bp = *cm ? cm + 1 : cm;
                    while (*bp == ' ' || *bp == '\t') bp++;
                    char *b = xstrndup(bp, strlen(bp));
                    for (char *k = a + strlen(a); k > a && isspace((unsigned char)k[-1]); ) *--k = 0;
                    for (char *k = b + strlen(b); k > b && isspace((unsigned char)k[-1]); ) *--k = 0;
                    c = !strcmp(a, b) == (gx_is(w, wn, ".ifc") ||
                                          gx_is(w, wn, ".ifeqs"));
                    free(a); free(b);
                } else if (gx_cond(x, r, line, &v)) {
                    c = gx_is(w, wn, ".if") || gx_is(w, wn, ".ifne") ? v != 0
                      : gx_is(w, wn, ".ifeq") ? v == 0
                      : gx_is(w, wn, ".ifgt") ? v > 0
                      : gx_is(w, wn, ".ifge") ? v >= 0
                      : gx_is(w, wn, ".iflt") ? v < 0 : v <= 0;
                }
            }
            live[ncond] = on;
            take[ncond] = c;
            done[ncond] = c;
            ncond++;
            i++;
            continue;
        }
        if (gx_is(w, wn, ".else") || gx_is(w, wn, ".elseif")) {
            if (!ncond) { gerr(g, "%.*s with no .if", (int)wn, w); i++; continue; }
            int c = 0;
            if (!done[ncond - 1]) {
                long v = 0;
                if (gx_is(w, wn, ".else"))
                    c = 1;
                else if (live[ncond - 1] && gx_cond(x, rest, line, &v))
                    c = v != 0;
            }
            take[ncond - 1] = c;
            if (c) done[ncond - 1] = 1;
            i++;
            continue;
        }
        if (gx_is(w, wn, ".endif")) {
            if (!ncond) gerr(g, ".endif with no .if");
            else ncond--;
            i++;
            continue;
        }
        if (!on) { i++; continue; }

        if (gx_is(w, wn, ".macro")) {
            struct gline *body;
            int nb;
            int next = gx_block(x, in, n, i, mac_words, ".endm", &body, &nb);
            gx_define(x, rest, body, nb, line);
            free(body);
            i = next;
            continue;
        }
        if (gx_is(w, wn, ".purgem")) {
            size_t ln;
            const char *nm = gx_word(rest, &ln);
            struct gmacro *m = gx_find(x, nm, ln);
            if (!m) gerr(g, ".purgem: no macro %.*s", (int)ln, nm);
            else m->name[0] = 0;
            i++;
            continue;
        }
        if (gx_is(w, wn, ".exitm")) {
            if (!x->depth) gerr(g, ".exitm outside a macro");
            x->exitm = 1;
            return;
        }
        if (gx_is(w, wn, ".rept") || gx_is(w, wn, ".irp") ||
            gx_is(w, wn, ".irpc")) {
            struct gline *body;
            int nb;
            int next = gx_block(x, in, n, i, rept_words, ".endr", &body, &nb);
            if (gx_is(w, wn, ".rept")) {
                long cnt = 0;
                if (gx_cond(x, rest, line, &cnt))
                    for (long r = 0; r < cnt && r < 100000; r++)
                        gx_lines(x, body, nb);
            } else {
                const char *r = rest;
                while (*r == ' ' || *r == '\t') r++;
                const char *ns = r;
                while (is_symc((unsigned char)*r)) r++;
                char *pname = xstrndup(ns, (size_t)(r - ns));
                while (*r == ' ' || *r == '\t' || *r == ',') r++;
                char *list = xstrndup(r, strlen(r));
                char *names[1] = { pname };
                int is_irpc = gx_is(w, wn, ".irpc");
                for (const char *p = list; *p; ) {
                    char *val;
                    if (is_irpc) {
                        val = xstrndup(p, 1);
                        p++;
                    } else {
                        const char *e = gx_operand_end(p);
                        const char *vs = p;
                        while (*vs == ' ' || *vs == '\t') vs++;
                        const char *ve = e;
                        while (ve > vs && isspace((unsigned char)ve[-1])) ve--;
                        val = xstrndup(vs, (size_t)(ve - vs));
                        p = *e ? e + 1 : e;
                    }
                    struct gline *exp = xmalloc((size_t)(nb ? nb : 1) * sizeof *exp);
                    for (int k = 0; k < nb; k++) {
                        exp[k].text = gx_subst(body[k].text, names, &val, 1,
                                               x->counter);
                        exp[k].line = body[k].line;
                    }
                    x->counter++;
                    gx_lines(x, exp, nb);
                    for (int k = 0; k < nb; k++) free(exp[k].text);
                    free(exp);
                    free(val);
                }
                free(pname);
                free(list);
            }
            free(body);
            i = next;
            continue;
        }
        if (gx_is(w, wn, ".include")) {
            const char *r = rest;
            while (*r == ' ' || *r == '\t') r++;
            char nm[1024];
            const char *e = r + 1;
            if (*r != '"' || !(e = strchr(r + 1, '"')))
                gerr(g, ".include wants a \"file\"");
            else {
                long ln;
                char path[2048];
                snprintf(nm, sizeof nm, "%.*s", (int)(e - r - 1), r + 1);
                char *src = plat_read_file(nm, &ln);
                if (!src) {
                    const char *sl = strrchr(g->path, '/');
                    snprintf(path, sizeof path, "%.*s/%s",
                             sl ? (int)(sl - g->path) : 1, sl ? g->path : ".", nm);
                    src = plat_read_file(path, &ln);
                }
                if (!src)
                    gerr(g, "cannot read the .include file '%s'", nm);
                else if (x->depth > 32)
                    gerr(g, ".include nests too deep");
                else {
                    x->depth++;
                    gx_text(x, src, nm, 0);
                    x->depth--;
                    free(src);
                }
            }
            i++;
            continue;
        }
        /* an absolute .equ/.set is a value later .ifs may test */
        if (gx_is(w, wn, ".equ") || gx_is(w, wn, ".set")) {
            const char *r = rest;
            while (*r == ' ' || *r == '\t') r++;
            const char *ne = r;
            while (is_symc((unsigned char)*ne)) ne++;
            const char *v = ne;
            while (*v == ' ' || *v == '\t') v++;
            if (*v == ',' && ne > r) {
                int bad, save = g->line;
                const char *ve = v + 1;
                g->errors_muted++;
                struct gval gv = gx_eval(g, &ve, 1, &bad);
                g->errors_muted--;
                g->line = save;
                if (!bad && gv.sec == SEC_ABS) {
                    struct sym *sy = sym_get(g, r, (size_t)(ne - r));
                    sy->sec = SEC_ABS;
                    sy->value = gv.v;
                }
            }
        }
        /* a macro call, after any labels on the line */
        {
            const char *p = w;
            const char *lab_end = NULL;
            for (;;) {
                const char *e = p;
                while (is_symc((unsigned char)*e)) e++;
                const char *c = e;
                while (*c == ' ' || *c == '\t') c++;
                if (e > p && *c == ':') {
                    lab_end = c + 1;
                    p = c + 1;
                    while (*p == ' ' || *p == '\t') p++;
                    continue;
                }
                break;
            }
            size_t mn;
            const char *mw = gx_word(p, &mn);
            struct gmacro *m = mn ? gx_find(x, mw, mn) : NULL;
            if (m && m->name[0]) {
                if (lab_end)
                    gx_emit(x, t, (size_t)(lab_end - t), line);
                gx_invoke(x, m, mw + mn, line);
                i++;
                continue;
            }
        }
        gx_emit(x, t, strlen(t), line);
        i++;
    }
    if (ncond && !x->depth)
        gerr(g, "an .if is never closed with .endif");
}

/* Splits text into statements -- comments removed, `;` separating them
 * where it is not this target's comment -- and expands them. */
static void gx_text(struct gexp *x, char *text, const char *path, int base)
{
    struct gas *g = x->g;
    const char *save_path = g->path;
    struct gline *in = NULL;
    int n = 0, cap = 0, line = base;
    (void)path;
    strip_c_comments(text);
    for (char *p = text; *p; ) {
        char *nl = strchr(p, '\n');
        size_t ln = nl ? (size_t)(nl - p) : strlen(p);
        char *l = xstrndup(p, ln);
        line++;
        strip_comment(l, g->tgt->comment_char, g->tgt->hash_is_imm);
        /* `;` separates statements, unless it is this target's comment */
        char *s = l;
        for (;;) {
            char *semi = NULL;
            int q = 0;
            if (g->tgt->comment_char != ';')
                for (char *k = s; *k; k++) {
                    if (q) { if (*k == '\\' && k[1]) k++; else if (*k == q) q = 0; continue; }
                    if (*k == '"' || *k == '\'') { q = *k; continue; }
                    if (*k == ';') { semi = k; break; }
                }
            if (n == cap) {
                cap = cap ? cap * 2 : 256;
                in = xrealloc(in, (size_t)cap * sizeof *in);
            }
            in[n].text = xstrndup(s, semi ? (size_t)(semi - s) : strlen(s));
            in[n].line = line;
            n++;
            if (!semi) break;
            s = semi + 1;
        }
        free(l);
        if (!nl) break;
        p = nl + 1;
    }
    gx_lines(x, in, n);
    for (int k = 0; k < n; k++)
        free(in[k].text);
    free(in);
    g->path = save_path;
}

/* ---- literal pools (ARM) --------------------------------------------- */

static int pool_cur(struct gas *g)
{
    if (g->npoolno < g->nsecs) {
        g->poolno = xrealloc(g->poolno, (size_t)g->nsecs * sizeof *g->poolno);
        for (int k = g->npoolno; k < g->nsecs; k++) g->poolno[k] = 0;
        g->npoolno = g->nsecs;
    }
    return g->poolno[g->cur];
}

/* The entry for a value in the current pool of the current section: its
 * index there, shared with an identical one already in it. */
static int pool_entry(struct gas *g, int is_sym, long v, struct sym *sy)
{
    int pool = pool_cur(g), k = 0;
    for (int i = 0; i < g->nlits; i++) {
        struct glit *l = &g->lits[i];
        if (l->sec != g->cur || l->pool != pool)
            continue;
        if (l->is_sym == is_sym && l->v == v && l->sym == sy)
            return k;
        k++;
    }
    if (g->nlits == g->caplits) {
        g->caplits = g->caplits ? g->caplits * 2 : 16;
        g->lits = xrealloc(g->lits, (size_t)g->caplits * sizeof *g->lits);
    }
    g->lits[g->nlits].sec = g->cur;
    g->lits[g->nlits].pool = pool;
    g->lits[g->nlits].is_sym = is_sym;
    g->lits[g->nlits].v = v;
    g->lits[g->nlits].sym = sy;
    g->nlits++;
    return k;
}

/* Where the current pool of the current section starts, as the last pass
 * placed it; -1 before it has been placed once. */
static long pool_base_prev(struct gas *g)
{
    int pool = pool_cur(g);
    for (int i = 0; i < g->nppools; i++)
        if (g->ppools[i].sec == g->cur && g->ppools[i].pool == pool)
            return g->ppools[i].base;
    return -1;
}

/* Writes the current pool here: word-aligned with zeros (it is data),
 * each entry a word -- a value, or a relocation for a symbol's. */
static void pool_dump(struct gas *g, int pass)
{
    int pool = pool_cur(g), any = 0;
    for (int i = 0; i < g->nlits; i++)
        if (g->lits[i].sec == g->cur && g->lits[i].pool == pool) any = 1;
    if (!any)
        return;
    while (cur_off(g) & 3)          /* padding, still in the code's run */
        advance(g, 1);
    map_mark(g, 'd');
    if (g->npools == g->cappools) {
        g->cappools = g->cappools ? g->cappools * 2 : 8;
        g->pools = xrealloc(g->pools, (size_t)g->cappools * sizeof *g->pools);
    }
    g->pools[g->npools].sec = g->cur;
    g->pools[g->npools].pool = pool;
    g->pools[g->npools].base = cur_off(g);
    g->npools++;
    if (g->secs[g->cur].align < 4)
        g->secs[g->cur].align = 4;
    for (int i = 0; i < g->nlits; i++) {
        struct glit *l = &g->lits[i];
        if (l->sec != g->cur || l->pool != pool)
            continue;
        if (l->is_sym && pass == 2)
            fix_add(g, g->cur, cur_off(g), l->sym->name, g->tgt->r_abs32, l->v);
        emit_int(g, l->is_sym ? 0 : l->v, 4);
    }
    g->poolno[g->cur]++;
}

/* ---- directives, continued: the ones that read expressions ----------- */

/* Returns 1 when the line was a directive. */
static int directive(struct gas *g, char *p, int pass)
{
    char *name = p;
    while (*p && !isspace((unsigned char)*p)) p++;
    size_t nlen = (size_t)(p - name);
    char *arg = skip_ws(p);

#define DIR(s) (nlen == strlen(s) && memcmp(name, s, nlen) == 0)

    for (int i = 0; i < NSEC; i++)
        if (nlen == strlen(sec_name[i]) && memcmp(name, sec_name[i], nlen) == 0) {
            if (*arg && !(arg[0] == '0' && !arg[1])) {
                gerr(g, "%s %s: subsections are not supported", sec_name[i],
                     arg);
                return 1;
            }
            g->prev = g->cur;
            g->cur = i;
            return 1;
        }
    if (DIR(".previous")) {
        int t = g->cur;
        g->cur = g->prev;
        g->prev = t;
        return 1;
    }
    if (DIR(".popsection")) {
        if (!g->nstack) {
            gerr(g, ".popsection with no .pushsection before it");
            return 1;
        }
        g->prev = g->cur;
        g->cur = g->stack[--g->nstack];
        return 1;
    }
    if (DIR(".section") || DIR(".pushsection")) {
        /* .section NAME [, "FLAGS" [, @TYPE [, ENTSIZE]]] */
        char *e = arg, *nm = arg;
        size_t nn;
        Elf64_Word ty;
        Elf64_Xword fl;
        int k, explicit_flags = 0, explicit_type = 0;
        if (*arg == '"') {
            nm = ++arg;
            e = strchr(nm, '"');
            if (!e) { gerr(g, "an unterminated section name"); return 1; }
            nn = (size_t)(e - nm);
            e++;
        } else {
            while (*e && !isspace((unsigned char)*e) && *e != ',') e++;
            nn = (size_t)(e - nm);
        }
        if (!nn) { gerr(g, "%.*s needs a name", (int)nlen, name); return 1; }
        {
            char *tmp = xstrndup(nm, nn);
            sec_defaults(tmp, &ty, &fl);
            free(tmp);
        }
        e = skip_ws(e);
        if (*e == ',') {
            e = skip_ws(e + 1);
            if (*e == '"') {
                explicit_flags = 1;
                fl = 0;
                for (e++; *e && *e != '"'; e++)
                    switch (*e) {
                    case 'a': fl |= SHF_ALLOC; break;
                    case 'w': fl |= SHF_WRITE; break;
                    case 'x': fl |= SHF_EXECINSTR; break;
                    case 'M': fl |= SHF_MERGE; break;
                    case 'S': fl |= SHF_STRINGS; break;
                    case 'T': fl |= SHF_TLS; break;
                    case 'R': break;     /* SHF_GNU_RETAIN: no GC here */
                    case 'y': break;     /* ARM purecode: execute-only */
                    default:
                        gerr(g, "section flag '%c' is not supported", *e);
                        return 1;
                    }
                if (*e == '"') e++;
                e = skip_ws(e);
            }
            if (*e == ',') {
                e = skip_ws(e + 1);
                if (*e == '@' || *e == '%') {
                    char *te = ++e;
                    while (*te && is_symc((unsigned char)*te)) te++;
                    explicit_type = 1;
#define TY(s) ((size_t)(te - e) == strlen(s) && !strncmp(e, s, strlen(s)))
                    if (TY("progbits")) ty = SHT_PROGBITS;
                    else if (TY("nobits")) ty = SHT_NOBITS;
                    else if (TY("note")) ty = SHT_NOTE;
                    else if (TY("init_array")) ty = SHT_INIT_ARRAY;
                    else if (TY("fini_array")) ty = SHT_FINI_ARRAY;
                    else if (TY("preinit_array")) ty = SHT_PREINIT_ARRAY;
                    else {
                        gerr(g, "section type %.*s is not supported",
                             (int)(te - e), e);
                        return 1;
                    }
#undef TY
                    e = skip_ws(te);
                    if (*e == ',') {       /* the entry size of a merge */
                        long es;
                        char *ee = skip_ws(e + 1), *eend = ee;
                        while (*eend && !isspace((unsigned char)*eend) &&
                               *eend != ',')
                            eend++;
                        if (parse_num(ee, eend, &es) && es > 0)
                            (void)es;
                    }
                }
            }
        }
        k = sec_find(g, nm, nn);
        if (k < 0) {
            k = sec_add(g, nm, nn, ty, fl);
        } else if ((explicit_flags && g->secs[k].flags != fl) ||
                   (explicit_type && g->secs[k].type != ty)) {
            /* GNU as warns and keeps the first; a section whose bytes
             * would be read under two meanings is refused instead */
            gerr(g, "section %s is declared again with other flags or type",
                 g->secs[k].name);
            return 1;
        }
        if (DIR(".pushsection")) {
            if (g->nstack == 32) { gerr(g, ".pushsection nests too deep"); return 1; }
            g->stack[g->nstack++] = g->cur;
        }
        g->prev = g->cur;
        g->cur = k;
        return 1;
    }
    /* .extern: GNU as accepts and ignores it -- every symbol a file uses
     * and does not define is external already. FreeRTOS's RISC-V
     * portASM.S declares the kernel's variables with it. */
    if (DIR(".extern"))
        return 1;
    /* .global/.globl/.weak/.hidden/.internal/.protected/.local take a
     * comma-separated list of names. .weak: a kernel's vector table names
     * a handler for every interrupt the part has, and a program that uses
     * three of them must still link -- an undefined WEAK reference
     * resolves to zero. */
    if (DIR(".global") || DIR(".globl") || DIR(".weak") || DIR(".hidden") ||
        DIR(".internal") || DIR(".protected") || DIR(".local") ||
        DIR(".weakref")) {
        if (DIR(".weakref")) {
            gerr(g, ".weakref is not supported");
            return 1;
        }
        for (char *q = arg; *q; ) {
            char *e;
            q = skip_ws(q);
            e = q;
            while (*e && is_symc((unsigned char)*e)) e++;
            if (e == q) {
                gerr(g, "%.*s needs a name", (int)nlen, name);
                return 1;
            }
            struct sym *sy = sym_get(g, q, (size_t)(e - q));
            if (DIR(".global") || DIR(".globl")) {
                sy->is_global = 1;
            } else if (DIR(".weak")) {
                sy->is_weak = 1;
                sy->is_global = 1;       /* a weak symbol is a global one */
            } else if (DIR(".local")) {
                sy->is_global = 0;
            } else {
                sy->vis = DIR(".hidden") ? 2 : DIR(".internal") ? 1 : 3;
            }
            q = skip_ws(e);
            if (*q == ',') q++;
            else if (*q) { gerr(g, "\"%s\" is not a list of names", arg); return 1; }
        }
        return 1;
    }
    if (DIR(".type")) {
        char *e = arg, *t;
        while (*e && is_symc((unsigned char)*e)) e++;
        t = skip_ws(e);
        if (e == arg || *t != ',') { gerr(g, ".type wants NAME, TYPE"); return 1; }
        t = skip_ws(t + 1);
        if (*t == '%' || *t == '@' || *t == '#') t++;
        struct sym *sy = sym_get(g, arg, (size_t)(e - arg));
        if (!strncmp(t, "function", 8) || !strncmp(t, "STT_FUNC", 8))
            sy->is_func = 1;
        else if (!strncmp(t, "object", 6) || !strncmp(t, "STT_OBJECT", 10))
            sy->is_object = 1;
        else if (strncmp(t, "notype", 6) && strncmp(t, "STT_NOTYPE", 10))
            gerr(g, ".type %s is not supported", t);
        return 1;
    }
    /* .thumb_func: the next label is a Thumb FUNCTION, whose symbol
     * carries the interworking bit -- what makes `.word handler` in a
     * vector table, or a C call through a pointer to it, enter Thumb
     * state instead of faulting. GNU as does the same for a label typed
     * %function in Thumb code, and so does write_object below. */
    if (DIR(".thumb_func")) {
        if (g->tgt->machine == EM_ARM)
            g->thumb_func_next = 1;
        return 1;
    }
    /* .size sym, EXPR -- `. - sym` as a rule: what a debugger reads as
     * the function's extent, and skips the symbol without. */
    if (DIR(".size")) {
        char *e = arg;
        const char *v;
        long n;
        while (*e && is_symc((unsigned char)*e)) e++;
        v = skip_ws(e);
        if (e == arg || *v != ',') { gerr(g, ".size wants NAME, SIZE"); return 1; }
        v++;
        struct sym *sy = sym_get(g, arg, (size_t)(e - arg));
        if (pass == 2 && gx_abs_now(g, &v, pass, &n, ".size"))
            sy->size = n;
        return 1;
    }
    /* .equ/.set/.thumb_set NAME, EXPR (and `NAME = EXPR`, see pass_over):
     * a plain value becomes an absolute symbol, a label plus a constant
     * an alias of it; one naming a label further down, or nothing in this
     * file, is an alias resolved when the object is written. .thumb_set
     * also makes NAME a Thumb function -- how a startup file points every
     * weak handler at Default_Handler. */
    if (DIR(".equ") || DIR(".set") || DIR(".thumb_set") || DIR(".equiv")) {
        char *e = arg;
        const char *v;
        int bad;
        while (*e && is_symc((unsigned char)*e)) e++;
        v = skip_ws(e);
        if (e == arg || *v != ',') {
            gerr(g, "%.*s wants NAME, VALUE", (int)nlen, name);
            return 1;
        }
        v++;
        struct sym *sy = sym_get(g, arg, (size_t)(e - arg));
        if (DIR(".equiv") && pass == 1 && sy->sec != SEC_UNDEF) {
            gerr(g, ".equiv: '%s' is already defined", sy->name);
            return 1;
        }
        struct gval gv = gx_eval(g, &v, pass, &bad);
        if (bad)
            return 1;
        sy->alias = NULL;
        sy->alias_add = 0;
        if (gv.sec == SEC_ABS) {
            sy->sec = SEC_ABS;
            sy->value = gv.v;
        } else if (gv.sec >= 0) {
            sy->sec = gv.sec;
            sy->value = gv.v;
            if (gv.base && gv.base->is_func) sy->is_func = 1;
        } else if (gv.base && gv.base != sy) {
            sy->sec = SEC_UNDEF;
            sy->alias = gv.base;
            sy->alias_add = gv.v;
        } else {
            gerr(g, "'%s' cannot be set to itself", sy->name);
        }
        if (DIR(".thumb_set"))
            sy->is_func = 1;
        return 1;
    }
    /* ARM state does not exist on an M-profile part: refused rather than
     * assembled as Thumb, which would be a different program. */
    if (DIR(".arm") || (DIR(".code") && skip_ws(arg)[0] == '3')) {
        gerr(g, "%s: an M-profile core has no ARM state; this file is for "
             "an A- or R-profile one", DIR(".arm") ? ".arm" : ".code 32");
        return 1;
    }
    if (DIR(".file") || DIR(".ident") || DIR(".cfi_startproc") ||
        DIR(".cfi_endproc") || DIR(".syntax") || DIR(".thumb") ||
        DIR(".arch") || DIR(".attribute") || DIR(".option") ||
        DIR(".code") || DIR(".fpu") || DIR(".eabi_attribute") ||
        DIR(".cpu") || DIR(".arch_extension") || DIR(".object_arch") ||
        DIR(".force_thumb") || DIR(".loc") || DIR(".cfi_sections") ||
        !strncmp(name, ".cfi_", 5))
        return 1;      /* accepted and carried no further */
    /* The ARM EHABI unwind directives describe how to unwind THROUGH this
     * code when a C++ exception does; they produce .ARM.exidx entries and
     * nothing that runs. A startup file marks its handlers with them.
     * Accepted, and no unwind table is written: an exception unwinding
     * through hand-written assembly here stops, as it would at
     * .cantunwind. */
    if (DIR(".fnstart") || DIR(".fnend") || DIR(".cantunwind") ||
        DIR(".save") || DIR(".vsave") || DIR(".setfp") || DIR(".pad") ||
        DIR(".movsp") || DIR(".personality") ||
        DIR(".personalityindex") || DIR(".handlerdata") ||
        DIR(".unwind_raw"))
        return 1;
    /* the literal pool so far, here (`ldr rd, =EXPR`) */
    if (DIR(".ltorg") || DIR(".pool")) {
        if (g->tgt->machine == EM_ARM)
            pool_dump(g, pass);
        return 1;
    }
    if (DIR(".error") || DIR(".warning") || DIR(".print")) {
        unsigned char buf[512];
        long n = *arg == '"' ? parse_string(g, arg, buf, (long)sizeof buf - 1)
                             : 0;
        if (n < 0) return 1;
        buf[n] = 0;
        if (pass == 1) {
            if (DIR(".error")) gerr(g, ".error: %s", buf);
            else fprintf(stderr, "%s:%d: %s%s\n", g->path, g->line,
                         DIR(".warning") ? "warning: " : "", buf);
        }
        return 1;
    }
    /* .lcomm NAME, SIZE[, ALIGN]: space in .bss, local. .comm makes a
     * global one: placed here too rather than as a COMMON symbol, since
     * this assembler writes no SHN_COMMON. */
    if (DIR(".lcomm") || DIR(".comm")) {
        char *e = arg;
        const char *v;
        long sz, al = 1;
        while (*e && is_symc((unsigned char)*e)) e++;
        v = skip_ws(e);
        if (e == arg || *v != ',') { gerr(g, "%.*s wants NAME, SIZE", (int)nlen, name); return 1; }
        v++;
        if (!gx_abs_now(g, &v, pass, &sz, "a size"))
            return 1;
        v = skip_ws((char *)v);
        if (*v == ',') {
            v++;
            if (!gx_abs_now(g, &v, pass, &al, "an alignment"))
                return 1;
        }
        int save = g->cur;
        struct sym *sy = sym_get(g, arg, (size_t)(e - arg));
        g->cur = SEC_BSS;
        do_align(g, al > 0 ? al : 1);
        sy->sec = SEC_BSS;
        sy->value = cur_off(g);
        sy->size = sz;
        sy->is_object = 1;
        if (DIR(".comm")) sy->is_global = 1;
        advance(g, sz);
        g->cur = save;
        return 1;
    }
    if (DIR(".org")) {
        const char *v = arg;
        int bad;
        struct gval gv = gx_eval(g, &v, pass, &bad);
        if (bad) return 1;
        long to = gv.sec == SEC_ABS ? gv.v : gv.sec == g->cur ? gv.v : -1;
        if (to < 0) { gerr(g, ".org needs an offset in this section"); return 1; }
        if (to < cur_off(g)) { gerr(g, ".org cannot move backwards"); return 1; }
        advance(g, to - cur_off(g));
        return 1;
    }
    if (DIR(".byte") || DIR(".short") || DIR(".half") || DIR(".hword") ||
        DIR(".word") || DIR(".long") || DIR(".int") || DIR(".quad") ||
        DIR(".dword") || DIR(".2byte") || DIR(".4byte") || DIR(".8byte")) {
        map_mark(g, 'd');
        /* `.word` is the MACHINE's word, which is two bytes on AVR and four
         * everywhere else here -- GNU as does the same, and a `.word` read
         * as four bytes silently doubles every table in an AVR source. */
        int wordw = g->tgt->word_bytes ? g->tgt->word_bytes : 4;
        int width = DIR(".byte") ? 1
                  : (DIR(".short") || DIR(".half") || DIR(".hword") ||
                     DIR(".2byte")) ? 2
                  : (DIR(".quad") || DIR(".dword") || DIR(".8byte")) ? 8
                  : DIR(".word") ? wordw : 4;
        const char *q = arg;
        while (*skip_ws((char *)q)) {
            const char *e = gx_operand_end(q);
            char *one = xstrndup(q, (size_t)(e - q));
            const char *v = one;
            int bad;
            struct gval gv = gx_eval(g, &v, pass, &bad);
            if (!bad && *skip_ws((char *)v))
                gerr(g, "\"%s\" is not one expression", one), bad = 1;
            if (bad || gv.sec == SEC_ABS || gv.unknown) {
                emit_int(g, bad || gv.unknown ? 0 : gv.v, width);
            } else {
                /* An ADDRESS in a data word: a relocation, against the
                 * label it is relative to or the external symbol. */
                int rt = width == 8 ? g->tgt->r_abs64
                       : width == 2 ? g->tgt->r_abs16 : g->tgt->r_abs32;
                if (width != 2 && width != 4 && width != 8)
                    gerr(g, "a symbol address needs a 2-, 4- or 8-byte slot");
                else if (width == 2 && !g->tgt->r_abs16)
                    gerr(g, "this target's pointer does not fit in two bytes, "
                            "so a symbol address cannot go in a %d-byte slot",
                         width);
                else if (!rt)
                    gerr(g, "this target has no absolute relocation for a "
                            "%d-byte symbol address", width);
                else if (pass == 2)
                    fix_add(g, g->cur, cur_off(g), gv.base->name, rt,
                            gv.sec >= 0 ? gv.v - gv.base->value : gv.v);
                emit_int(g, 0, width);
            }
            free(one);
            q = *e == ',' ? e + 1 : e;
        }
        return 1;
    }
    /* .inst: an instruction given as its number, the way a file says an
     * encoding its assembler does not know. On ARM .inst.n is one
     * halfword and .inst.w a 32-bit Thumb instruction, its halves in
     * instruction order. */
    if (DIR(".inst") || DIR(".inst.n") || DIR(".inst.w")) {
        map_mark(g, 't');
        const char *q = arg;
        while (*skip_ws((char *)q)) {
            long v;
            if (!gx_abs_now(g, &q, pass, &v, "an instruction"))
                return 1;
            if (g->tgt->machine == EM_ARM) {
                int wide = DIR(".inst.w") || (!DIR(".inst.n") && v > 0xffff);
                if (wide) emit_int(g, (v >> 16) & 0xffff, 2);
                emit_int(g, v & 0xffff, 2);
            } else {
                emit_int(g, v, 4);
            }
            q = skip_ws((char *)q);
            if (*q == ',') q++;
        }
        return 1;
    }
    if (DIR(".asciz") || DIR(".string") || DIR(".ascii")) {
        map_mark(g, 'd');
        unsigned char buf[4096];
        long n = parse_string(g, arg, buf, (long)sizeof buf);
        if (n >= 0) {
            emit_bytes(g, buf, n);
            if (!DIR(".ascii")) emit_int(g, 0, 1);
        }
        return 1;
    }
    if (DIR(".space") || DIR(".zero") || DIR(".skip")) {
        if (!sec_is_nobits(g, g->cur))
            map_mark(g, 'd');
        const char *q = arg;
        long v, fill = 0;
        if (!gx_abs_now(g, &q, pass, &v, "the size"))
            return 1;
        q = skip_ws((char *)q);
        if (*q == ',') {
            q++;
            if (!gx_abs_now(g, &q, pass, &fill, "the fill"))
                return 1;
        }
        if (v < 0) { gerr(g, "%.*s %ld: a negative size", (int)nlen, name, v); return 1; }
        advance_fill(g, v, (int)(fill & 0xff));
        return 1;
    }
    /* .fill REPEAT[, SIZE[, VALUE]]: REPEAT copies of VALUE, SIZE bytes
     * each (at most 8), little-endian. */
    if (DIR(".fill")) {
        map_mark(g, 'd');
        const char *q = arg;
        long rep, size = 1, val = 0;
        if (!gx_abs_now(g, &q, pass, &rep, "the repeat count"))
            return 1;
        q = skip_ws((char *)q);
        if (*q == ',') {
            q++;
            if (!gx_abs_now(g, &q, pass, &size, "the size")) return 1;
            q = skip_ws((char *)q);
            if (*q == ',') {
                q++;
                if (!gx_abs_now(g, &q, pass, &val, "the value")) return 1;
            }
        }
        if (rep < 0 || size < 0 || size > 8) {
            gerr(g, ".fill %ld, %ld: out of range", rep, size);
            return 1;
        }
        for (long k = 0; k < rep; k++)
            emit_int(g, val, (int)size);
        return 1;
    }
    if (DIR(".align") || DIR(".balign") || DIR(".p2align") ||
        DIR(".balignw") || DIR(".balignl") || DIR(".p2alignw") ||
        DIR(".p2alignl")) {
        long v;
        const char *q = arg;
        if (!gx_abs_now(g, &q, pass, &v, "the alignment"))
            return 1;
        /* a fill value is accepted only for data, where GNU as uses it;
         * code is padded with nops whatever it says */
        q = skip_ws((char *)q);
        if (*q == ',' && !(g->secs[g->cur].flags & SHF_EXECINSTR)) {
            long fill;
            const char *f = skip_ws((char *)q + 1);
            if (*f && *f != ',' && gx_abs_now(g, &f, pass, &fill, "the fill") &&
                fill) {
                gerr(g, "%.*s with a nonzero fill is not supported",
                     (int)nlen, name);
                return 1;
            }
        }
        /* .balign takes a byte count. .p2align takes an exponent, and so
         * does .align on every machine this assembles for: GNU as reads
         * `.align n` as 2^n bytes on ARM, aarch64, RISC-V and AVR (it is
         * a byte count only on x86 and a few others). Read as bytes,
         * `.align 2` before a RISC-V trap vector gave two-byte alignment
         * where mtvec needs four, and a Cortex-M vector table's `.align 7`
         * asked for a multiple of seven. */
        if (DIR(".balign") || DIR(".balignw") || DIR(".balignl")) {
            if (v > 0) do_align(g, v);
        } else {
            long b = 1;
            if (v < 0 || v > 16) {
                gerr(g, "%.*s %ld: the exponent must be 0..16", (int)nlen,
                     name, v);
                return 1;
            }
            while (v-- > 0) b *= 2;
            do_align(g, b);
        }
        return 1;
    }
    gerr(g, "directive \"%.*s\" is not one this assembler knows",
         (int)nlen, name);
    return 1;
#undef DIR
}

/* ---- a pass over the source ------------------------------------------ */

static void pass_over(struct gas *g, const struct gline *lines, int nlines,
                      int pass)
{
    /* The numeric-local counters are replayed, not carried: `1f` must
     * resolve to the same instance in both passes, and it is chosen from
     * how many `1:` have been SEEN so far. Leaving them set from pass one
     * makes every forward reference in pass two pick the instance after the
     * last one in the file, which is a jump to nowhere. */
    memset(g->local_n, 0, sizeof g->local_n);
    if (g->tgt->reset)
        g->tgt->reset();
    g->cur = SEC_TEXT;
    g->line = 0;
    for (int i = 0; i < g->nsecs; i++) {
        free(g->secs[i].c.p);
        memset(&g->secs[i].c, 0, sizeof g->secs[i].c);
        g->secs[i].size = 0;
    }
    g->prev = SEC_TEXT;
    g->nstack = 0;
    /* what a pass records is rebuilt by the next one; the `la` labels
     * are numbered again from 0 so each pass names the same ones */
    g->npcrel = 0;
    for (int i = 0; i < g->nfix; i++)
        free(g->fix[i].sym);
    g->nfix = 0;
    g->nmaps = 0;
    if (g->mapst)
        memset(g->mapst, 0, (size_t)g->nmapst);
    g->nlits = 0;
    if (g->poolno)
        memset(g->poolno, 0, (size_t)g->npoolno * sizeof *g->poolno);
    free(g->ppools);
    g->ppools = g->pools;
    g->nppools = g->npools;
    g->pools = NULL;
    g->npools = g->cappools = 0;

    for (int li = 0; li < nlines; li++) {
        char *line = xstrndup(lines[li].text, strlen(lines[li].text));
        g->line = lines[li].line;
        g->li = li;

        char *q = skip_ws(line);
        /* Any number of `label:` may precede a statement on one line. */
        for (;;) {
            char *e = q;
            /* A numeric local label begins with a DIGIT, which is not a
             * normal identifier start -- so both are allowed here and the
             * body below decides which it was. */
            if (!is_sym0((unsigned char)*e) && !isdigit((unsigned char)*e))
                break;
            while (*e && is_symc((unsigned char)*e)) e++;
            char *c = skip_ws(e);
            if (*c != ':') break;
            {
                struct sym *s;
                size_t ln = (size_t)(e - q);
                int alldig = ln > 0, k;
                for (k = 0; k < (int)ln; k++)
                    if (!isdigit((unsigned char)q[k])) { alldig = 0; break; }
                if (alldig) {
                    /* `1:` -- the nth definition of local 1. */
                    char nm[32];
                    int d = atoi(q);
                    if (d >= NLOCAL) {
                        gerr(g, "a numeric local label must be 0..%d",
                             NLOCAL - 1);
                        break;
                    }
                    local_name(nm, sizeof nm, d, g->local_n[d]++);
                    s = sym_get(g, nm, strlen(nm));
                    s->sec = g->cur;
                    s->value = cur_off(g);
                    q = skip_ws(c + 1);
                    continue;
                }
                s = sym_get(g, q, ln);
                if (pass == 1 && s->sec >= 0)
                    gerr(g, "label '%s' is defined twice", s->name);
                s->sec = g->cur;
                s->value = cur_off(g);
                if (g->thumb_func_next) {
                    s->is_func = 1;
                    g->thumb_func_next = 0;
                }
            }
            q = skip_ws(c + 1);
        }
        /* `NAME = EXPR` is `.set NAME, EXPR` */
        {
            char *e = q;
            while (*e && is_symc((unsigned char)*e)) e++;
            char *eq = skip_ws(e);
            if (e > q && is_sym0((unsigned char)*q) && *eq == '=' && eq[1] != '=') {
                size_t ln = (size_t)(e - q) + strlen(eq + 1) + 8;
                char *d = xmalloc(ln);
                snprintf(d, ln, ".set %.*s,%s", (int)(e - q), q, eq + 1);
                directive(g, d, pass);
                free(d);
                free(line);
                continue;
            }
        }
        if (*q == '.' && !strncmp(q, ".end", 4) &&
            (!q[4] || isspace((unsigned char)q[4]))) {
            free(line);
            break;                        /* the rest of the file is ignored */
        }
        if (*q == '.' && (is_sym0((unsigned char)q[1]) ||
                          isdigit((unsigned char)q[1])))   /* .4byte */
            directive(g, q, pass);
        else if (*q)
            instruction(g, q, pass);

        free(line);
    }
    /* each section's last pool, at its end */
    if (g->tgt->machine == EM_ARM) {
        int save = g->cur;
        for (int k = 0; k < g->nsecs; k++) {
            g->cur = k;
            pool_dump(g, pass);
        }
        g->cur = save;
    }
    if (g->tgt->open && g->tgt->open())
        gerr(g, "an IT block is still owed instructions at the end of the "
                "file");
}

/* ---- the object ------------------------------------------------------ */

static int write_object(struct gas *g, const char *out_path)
{
    struct elfw *w = elfw_new(g->tgt->machine);
    int *ndx = xcalloc((size_t)g->nsecs, sizeof *ndx);
    char *used = xcalloc((size_t)g->nsecs, 1);
    int i;

    /* A section goes into the object when it has bytes, or a symbol or a
     * relocation in it -- `.section .vectors` holding only a label still
     * names the place. .text always, as it always has. */
    used[SEC_TEXT] = 1;
    for (i = 0; i < g->nsecs; i++)
        if (g->secs[i].c.len || g->secs[i].size)
            used[i] = 1;
    for (i = 0; i < g->nsyms; i++)
        if (g->syms[i].sec >= 0)
            used[g->syms[i].sec] = 1;
    for (i = 0; i < g->nfix; i++)
        used[g->fix[i].sec] = 1;
    for (i = 0; i < g->nsecs; i++) {
        struct gsec *sc = &g->secs[i];
        long al = sc->align;
        if (!used[i])
            continue;
        /* the alignment these four always had, at least */
        if (i == SEC_DATA || i == SEC_RODATA || i == SEC_BSS)
            al = al > 8 ? al : 8;
        ndx[i] = elfw_add_section(w, sc->name, sc->type, sc->flags,
                                  sc->type == SHT_NOBITS ? NULL : sc->c.p,
                                  (Elf64_Xword)(sc->type == SHT_NOBITS
                                                ? sc->size : sc->c.len),
                                  (Elf64_Xword)al);
    }

    /* `.set a, b` / `.thumb_set a, b`: a takes b's place, now that b has
     * one. b may come later in the file than the directive, which is how
     * a startup file's weak handlers alias a Default_Handler defined at
     * the end. A chain is followed; an alias of nothing defined here is
     * refused, because ELF has no way to say "a is wherever b ends up". */
    for (i = 0; i < g->nsyms; i++) {
        struct sym *s = &g->syms[i], *t = s;
        long add = 0;
        int hops = 0;
        if (!s->alias)
            continue;
        while (t->alias && hops++ < 64) {
            add += t->alias_add;
            t = t->alias;
        }
        if (t->alias || t->sec == SEC_UNDEF) {
            fprintf(stderr, "%s: error: '%s' is set to '%s', which this file "
                            "does not define\n", g->path, s->name, t->name);
            g->errors++;
            continue;
        }
        s->sec = t->sec;
        s->value = t->value + add;
        if (t->is_func)
            s->is_func = 1;
        if (!s->size)
            s->size = t->size;
    }

    /* A relocation against a LOCAL label is made against its section,
     * the label's offset in the addend, as GNU as does -- which is what
     * lets an assembler-local `.L` label stay out of the symbol table. A
     * Thumb function keeps its own symbol: its address carries the
     * interworking bit, and a section symbol's does not. */
    int *secsym = xcalloc((size_t)g->nsecs, sizeof *secsym);
    char *keep = xcalloc((size_t)(g->nsyms ? g->nsyms : 1), 1);
    /* (only a DATA word: a branch or a call names the label itself, as
     * clang's and GNU's assemblers do) */
#define DATA_RELOC(f) ((f)->type == g->tgt->r_abs32 || \
                       (f)->type == g->tgt->r_abs64 || \
                       ((f)->type == g->tgt->r_abs16 && g->tgt->r_abs16))
    for (i = 0; i < g->nfix; i++) {
        struct sym *s = sym_find(g, g->fix[i].sym, strlen(g->fix[i].sym));
        if (!s) continue;
        if (!s->is_global && s->sec >= 0 && !s->is_func &&
            DATA_RELOC(&g->fix[i]))
            secsym[s->sec] = -1;            /* wanted; index below */
        else
            keep[s - g->syms] = 1;
    }
    /* Locals before globals: the gABI orders a symbol table that way
     * and the writer refuses rather than reordering behind our back. */
    for (i = 0; i < g->nsecs; i++)
        if (secsym[i] == -1 && ndx[i])
            secsym[i] = elfw_add_symbol(w, "", 0, 0,
                                        (Elf64_Uchar)((STB_LOCAL << 4) |
                                                      STT_SECTION),
                                        (Elf64_Half)ndx[i]);
    /* only in a section with code in it: data alone needs no map (and
     * clang's assembler writes none there either) */
    char *hascode = xcalloc((size_t)g->nsecs, 1);
    for (i = 0; i < g->nmaps; i++)
        if (g->maps[i].kind == 't')
            hascode[g->maps[i].sec] = 1;
    for (i = 0; i < g->nmaps; i++)
        if (ndx[g->maps[i].sec] && hascode[g->maps[i].sec])
            elfw_add_symbol(w, g->maps[i].kind == 't' ? "$t" : "$d",
                            (Elf64_Addr)g->maps[i].off, 0,
                            (Elf64_Uchar)((STB_LOCAL << 4) | STT_NOTYPE),
                            (Elf64_Half)ndx[g->maps[i].sec]);
    for (int pass = 0; pass < 2; pass++)
        for (i = 0; i < g->nsyms; i++) {
            struct sym *s = &g->syms[i];
            int want_global = pass == 1;
            if (!!s->is_global != want_global) continue;
            /* `.L` labels are the assembler's own, as are the ones made
             * for `.` and numeric locals */
            if (!s->is_global && !strncmp(s->name, ".L", 2) && !keep[i])
                continue;
            if (s->sec == SEC_UNDEF && !s->is_global) {
                /* Named but never defined and never exported: a symbol
                 * only a relocation refers to, which must be UNDEF and
                 * global for the linker to resolve it. */
                continue;
            }
            unsigned char bind = s->is_weak ? STB_WEAK
                               : s->is_global ? STB_GLOBAL : STB_LOCAL;
            unsigned char type = s->is_func ? STT_FUNC
                               : s->is_object ? STT_OBJECT : STT_NOTYPE;
            /* A function in Thumb code: the interworking bit. Every
             * function here is Thumb (M-profile has no ARM state), in
             * whichever section it was placed. */
            long thumb = g->tgt->machine == EM_ARM && s->is_func &&
                         s->sec >= 0;
            s->elf_ndx = elfw_add_symbol(w, s->name,
                                         (Elf64_Addr)(s->sec >= 0 ?
                                                      (s->value | thumb)
                                                      : s->sec == SEC_ABS ?
                                                      s->value : 0),
                                         (Elf64_Xword)s->size,
                                         (Elf64_Uchar)((bind << 4) | type),
                                         (Elf64_Half)(s->sec >= 0 ? ndx[s->sec]
                                                      : s->sec == SEC_ABS
                                                      ? SHN_ABS : SHN_UNDEF));
        }
    /* Anything a relocation names and nothing defined becomes UNDEF. */
    for (i = 0; i < g->nfix; i++) {
        struct sym *s = sym_find(g, g->fix[i].sym, strlen(g->fix[i].sym));
        if (s && !s->elf_ndx)
            s->elf_ndx = elfw_add_symbol(w, s->name, 0, 0,
                                         (Elf64_Uchar)(((s->is_weak ? STB_WEAK
                                                       : STB_GLOBAL) << 4) |
                                                       STT_NOTYPE),
                                         SHN_UNDEF);
    }
    for (i = 0; i < g->nfix; i++) {
        struct fixup *f = &g->fix[i];
        struct sym *s = sym_find(g, f->sym, strlen(f->sym));
        if (s && !s->is_global && s->sec >= 0 && !s->is_func &&
            secsym[s->sec] > 0 && DATA_RELOC(f)) {
            elfw_add_rela(w, ndx[f->sec], (Elf64_Addr)f->off, secsym[s->sec],
                          f->type, f->addend + s->value);
            continue;
        }
        if (!s || !s->elf_ndx) {
            fprintf(stderr, "%s: error: '%s' is referenced and never "
                            "defined\n", g->path, f->sym);
            g->errors++;
            continue;
        }
        elfw_add_rela(w, ndx[f->sec], (Elf64_Addr)f->off, s->elf_ndx,
                      f->type, f->addend);
    }
    free(secsym);
    free(keep);
    free(hascode);
    /* The build attributes the compiler writes for this target, so an
     * assembled object says what it was built for as a compiled one does:
     * the linker checks them across objects, and a disassembler decodes
     * Thumb-2 only when told the core has it. */
    if (g->tgt->machine == EM_ARM) {
        size_t alen = 0;
        unsigned char *ab = arm_build_attributes(&alen);
        elfw_add_section(w, ".ARM.attributes", SHT_ARM_ATTRIBUTES, 0, ab,
                         (Elf64_Xword)alen, 1);
        free(ab);
    }
    free(used);
    if (g->errors) { elfw_free(w); free(ndx); return 1; }
    int rc = elfw_write(w, out_path);
    elfw_free(w);
    free(ndx);
    return rc == 0 ? 0 : 1;
}

/* ---- the targets ----------------------------------------------------- */

static const struct gas_target RISCV_GAS = {
    EM_RISCV, 0, rvasm_assemble, rvasm_gpr,
    R_RISCV_CALL, R_RISCV_PCREL_HI20, R_RISCV_PCREL_LO12_I,
    R_RISCV_32, R_RISCV_64,
    /* no 16-bit pointer, `.word` is four bytes, and no symbol
     * forms beyond the ones above. */
    0, 0, NULL, 0,
    0, rvasm_is_word, NULL, NULL
};

/* ARMv7-M. `call` and `la` are RISC-V pseudos and have no ARM
 * spelling, so r_call/r_pcrel are zero: a symbol reference from a .S
 * file here goes through `bl sym` (R_ARM_THM_CALL) and the movw/movt
 * pair, which the driver names below. */
static const struct gas_target THUMB_GAS = {
    EM_ARM, 1, tasm_assemble, tasm_gpr,
    R_ARM_THM_CALL, 0, 0,
    R_ARM_ABS32, 0,
    /* no 16-bit pointer, `.word` is four bytes; `ldr rd, =sym` and the
     * :lower16:/:upper16: movw/movt are tasm_symform's. `@` is ARM's line
     * comment and `#` an immediate's prefix. */
    0, 0, tasm_symform, '@',
    1, tasm_is_word, tasm_reset, tasm_open
};

/* aarch64. `bl sym` carries R_AARCH64_CALL26, one instruction and one
 * relocation, the same shape ARM uses -- so the driver's ARM branch
 * covers it once the machine is allowed through. */
static const struct gas_target A64_GAS = {
    EM_AARCH64, 0, a64asm_assemble, a64asm_gpr,
    R_AARCH64_CALL26, 0, 0,
    R_AARCH64_ABS32, R_AARCH64_ABS64,
    /* no 16-bit pointer, `.word` is four bytes, and no symbol
     * forms beyond the ones above; `#` is an immediate's prefix. */
    0, 0, NULL, 0,
    1, NULL, NULL, NULL
};

/* AVR. Its symbol-bearing forms are its own -- eight of them, because a
 * sixteen-bit address is loaded a byte at a time and program space is
 * addressed in words -- so they go through `symform` rather than through
 * the r_call/r_pcrel pair, which stay zero. `.word` is TWO bytes here, and
 * a symbol's address fits in one. */
static const struct gas_target AVR_GAS = {
    EM_AVR, 1, avrasm_assemble, avrasm_gpr,
    0, 0, 0,
    R_AVR_32, 0,
    R_AVR_16, 2, avrasm_symform,
    ';',         /* AVR's line comment, as GNU as sets it for this port */
    0, NULL, NULL, NULL
};

static const struct gas_target *target_for(void)
{
    switch (target_get()) {
    case TARGET_AVR: return &AVR_GAS;
    case TARGET_RISCV32: case TARGET_RISCV64: return &RISCV_GAS;
    case TARGET_THUMB: return &THUMB_GAS;
    case TARGET_AARCH64: return &A64_GAS;
    default: return NULL;
    }
}

/* The macro language, then passes until nothing moves: the whole of
 * assembling, for a file or for a block. */
static void gas_run(struct gas *g, struct gexp *gx, const char *text,
                    const char *path)
{
    if (g->tgt->machine == EM_ARM)
        tasm_set_arch(target_thumb_arch());
    /* The macro language first, then pass 1 places the labels and pass 2
     * encodes with the displacements they give. Two passes and not one
     * because a branch forward names a label the assembler has not
     * reached. */
    memset(gx, 0, sizeof *gx);
    gx->g = g;
    {
        char *copy = xstrndup(text, strlen(text));
        gx_text(gx, copy, path, 0);
        free(copy);
    }
    g->wide = xcalloc((size_t)gx->nout + 1, 1);
    g->nwide = gx->nout;
    /* Then passes until nothing moves: a branch or a literal load is
     * tried in its two-byte form and, if it does not reach, widened for
     * good; each pass uses the label positions the one before found. The
     * output is from a pass whose layout matched the one before it. */
    if (!g->errors)
        pass_over(g, gx->out, gx->nout, 1);
    {
        unsigned long long prev = 0;
        int it;
        for (it = 0; it < 40 && !g->errors; it++) {
            g->grew = 0;
            pass_over(g, gx->out, gx->nout, 2);
            unsigned long long h = 1469598103934665603ULL;
            for (int i = 0; i < g->nsyms; i++) {
                h = (h ^ (unsigned long long)(g->syms[i].sec + 3)) * 1099511628211ULL;
                h = (h ^ (unsigned long long)g->syms[i].value) * 1099511628211ULL;
            }
            for (int i = 0; i < g->npools; i++)
                h = (h ^ (unsigned long long)g->pools[i].base) * 1099511628211ULL;
            if (it > 0 && !g->grew && h == prev)
                break;
            prev = h;
        }
        if (it == 40 && !g->errors)
            gerr(g, "the layout does not settle after 40 passes");
    }
}

static void gas_free(struct gas *g, struct gexp *gx)
{
    for (int i = 0; i < gx->nout; i++)
        free(gx->out[i].text);
    free(gx->out);
    for (int i = 0; i < gx->nmac; i++) {
        struct gmacro *m = &gx->mac[i];
        for (int k = 0; k < m->nparam; k++) {
            free(m->param[k]);
            free(m->dflt[k]);
        }
        for (int k = 0; k < m->nbody; k++)
            free(m->body[k].text);
        free(m->param); free(m->dflt); free(m->req); free(m->body);
        free(m->name);
    }
    free(gx->mac);
    for (int i = 0; i < g->nsecs; i++) {
        free(g->secs[i].c.p);
        free(g->secs[i].name);
    }
    free(g->secs);
    for (int i = 0; i < g->nsyms; i++) free(g->syms[i].name);
    for (int i = 0; i < g->nfix; i++) free(g->fix[i].sym);
    free(g->syms); free(g->fix);
}

int gas_assemble(const char *in_path, const char *out_path, int preprocess,
                 const char **incdirs, int nincdirs)
{
    struct gas g;
    struct gexp gx;
    char *src, *text;
    long len;
    int rc;

    const struct gas_target *t = target_for();
    if (!t) {
        fprintf(stderr, "embcc: error: no assembly-file support for %s yet; "
                        "its instruction encoder exists (inline __asm__ "
                        "works) but this driver has not been wired to it\n",
                target_triple_now());
        return 1;
    }

    src = plat_read_file(in_path, &len);
    if (!src) {
        fprintf(stderr, "embcc: error: cannot read '%s'\n", in_path);
        return 1;
    }
    /* `.S` is preprocessed and `.s` is not, which is the whole of the
     * difference between them. */
    text = preprocess ? cpp_process(in_path, src, incdirs, nincdirs) : src;

    memset(&g, 0, sizeof g);
    g.path = in_path;
    g.tgt = t;
    sec_init(&g);
    gas_run(&g, &gx, text, in_path);
    rc = g.errors ? 1 : write_object(&g, out_path);
    gas_free(&g, &gx);
    if (text != src) free(text);
    free(src);
    return rc;
}

/* ---- a block for the compiler ----------------------------------------
 *
 * A file-scope __asm__ and a naked function's body are assembly in a C
 * file, and on an embedded target they are what a context switch or a
 * reset handler is written in: labels, a literal word naming a global, a
 * `bl` to C. The compiler's inline assembler encodes one instruction at a
 * time with its operands already in registers; this is the whole
 * assembler, run over the block, and the driver places the result in the
 * unit's .text as it places x86-64's file-scope asm (struct topasm). */
int gas_assemble_block(struct topasm *ta)
{
    struct gas g;
    struct gexp gx;
    const struct gas_target *t = target_for();
    if (!t) {
        fprintf(stderr, "%s:%d: error: assembly in a C file is not supported "
                        "for %s\n", ta->file, ta->line, target_triple_now());
        return 1;
    }
    memset(&g, 0, sizeof g);
    g.path = ta->file;
    g.line_base = ta->line > 0 ? ta->line - 1 : 0;
    g.tgt = t;
    sec_init(&g);
    gas_run(&g, &gx, ta->tmpl, ta->file);

    /* One section: the block's bytes go into the unit's .text, between
     * its functions. */
    for (int i = 0; i < g.nsecs && !g.errors; i++)
        if (i != SEC_TEXT && (g.secs[i].c.len || g.secs[i].size))
            gerr(&g, "assembly in a C file that switches to section %s is "
                     "not supported yet: a block's bytes go in .text",
                 g.secs[i].name);
    for (int i = 0; i < g.nfix && !g.errors; i++)
        if (g.fix[i].sec != SEC_TEXT)
            gerr(&g, "a relocation outside .text in an asm block");
    /* .set aliases, resolved as the object writer does */
    for (int i = 0; i < g.nsyms && !g.errors; i++) {
        struct sym *s = &g.syms[i], *a = s;
        long add = 0;
        int hops = 0;
        if (!s->alias)
            continue;
        while (a->alias && hops++ < 64) {
            add += a->alias_add;
            a = a->alias;
        }
        if (a->alias || a->sec == SEC_UNDEF) {
            gerr(&g, "'%s' is set to '%s', which this block does not define",
                 s->name, a->name);
            break;
        }
        s->sec = a->sec;
        s->value = a->value + add;
        s->is_func |= a->is_func;
    }
    if (!g.errors) {
        struct gsec *tx = &g.secs[SEC_TEXT];
        ta->codelen = (int)tx->c.len;
        ta->code = xmalloc((size_t)(ta->codelen ? ta->codelen : 1));
        memcpy(ta->code, tx->c.p, (size_t)ta->codelen);
        ta->align = (int)(tx->align > 1 ? tx->align : 1);
        /* its labels: the ones C may name, and any a relocation names */
        ta->syms = xcalloc((size_t)(g.nsyms ? g.nsyms : 1), sizeof *ta->syms);
        for (int i = 0; i < g.nsyms; i++) {
            const struct sym *s = &g.syms[i];
            if (s->sec != SEC_TEXT)
                continue;
            if (!s->is_global && !strncmp(s->name, ".L", 2))
                continue;
            struct asmsym *as = &ta->syms[ta->nsyms++];
            as->name = xstrndup(s->name, strlen(s->name));
            as->off = (int)s->value;
            as->is_global = s->is_global;
            as->is_weak = s->is_weak;
            as->type = s->is_func ? ASMSYM_FUNC
                     : s->is_object ? ASMSYM_OBJECT : ASMSYM_UNTYPED;
            as->size = s->size;
        }
        /* its relocations, with the ELF type the assembler chose; a
         * field against an assembler-local label names the label's
         * offset in this block instead, which the driver turns into one
         * against .text */
        ta->rels = xcalloc((size_t)(g.nfix ? g.nfix : 1), sizeof *ta->rels);
        for (int i = 0; i < g.nfix; i++) {
            const struct fixup *f = &g.fix[i];
            const struct sym *s = sym_find(&g, f->sym, strlen(f->sym));
            struct asmrel *r = &ta->rels[ta->nrels++];
            r->off = (int)f->off;
            r->addend = f->addend;
            r->elf_type = f->type;
            r->kind = ASMREL_ABS32;
            if (s && !s->is_global && s->sec == SEC_TEXT &&
                !strncmp(s->name, ".L", 2)) {
                r->target = NULL;                 /* the block itself */
                r->addend += s->value;
            } else {
                r->target = xstrndup(f->sym, strlen(f->sym));
            }
        }
        /* where the data in it is ($d), for the disassemblers */
        ta->drange = xcalloc((size_t)(2 * g.nmaps + 2), sizeof *ta->drange);
        for (int i = 0; i < g.nmaps; i++) {
            if (g.maps[i].sec != SEC_TEXT || g.maps[i].kind != 'd')
                continue;
            long end = tx->c.len;
            for (int k = i + 1; k < g.nmaps; k++)
                if (g.maps[k].sec == SEC_TEXT && g.maps[k].off > g.maps[i].off) {
                    end = g.maps[k].off;
                    break;
                }
            ta->drange[ta->ndrange++] = (int)g.maps[i].off;
            ta->drange[ta->ndrange++] = (int)end;
        }
    }
    int rc = g.errors ? 1 : 0;
    gas_free(&g, &gx);
    return rc;
}
