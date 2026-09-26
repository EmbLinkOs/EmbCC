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

/* ---- the pieces of a file ------------------------------------------ */

enum { SEC_TEXT, SEC_DATA, SEC_RODATA, SEC_BSS, NSEC };

static const char *const sec_name[NSEC] = {
    ".text", ".data", ".rodata", ".bss"
};

struct sym {
    char *name;
    int sec;                /* which section, or -1 for undefined */
    long value;
    int is_global;
    int is_func;
    long size;
    int elf_ndx;            /* filled when the symbol table is built */
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
    struct code sec[NSEC];
    long bss_size;
    struct sym *syms;
    int nsyms, capsyms;
    struct fixup *fix;
    int nfix, capfix;
    const struct gas_target *tgt;
    int cur;                /* current section */
    int npcrel;             /* .Lpcrel_hiN counter */
    int line;               /* for diagnostics */
    int errors;
};

static void gerr(struct gas *g, const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "%s:%d: error: ", g->path, g->line);
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
 * a `#` inside a string does not. */
static void strip_comment(char *s)
{
    int q = 0;
    for (char *p = s; *p; p++) {
        if (q) {
            if (*p == '\\' && p[1]) { p++; continue; }
            if (*p == q) q = 0;
            continue;
        }
        if (*p == '"' || *p == '\'') { q = *p; continue; }
        if (*p == '#' || (*p == '/' && p[1] == '/')) { *p = '\0'; break; }
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

/* ---- directives ------------------------------------------------------ */

static void emit_bytes(struct gas *g, const unsigned char *p, long n)
{
    if (g->cur == SEC_BSS) {
        gerr(g, ".bss holds no data; use .space");
        return;
    }
    for (long i = 0; i < n; i++)
        code_byte(&g->sec[g->cur], p[i]);
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
    return g->cur == SEC_BSS ? g->bss_size : g->sec[g->cur].len;
}

static void advance(struct gas *g, long n)
{
    if (g->cur == SEC_BSS) { g->bss_size += n; return; }
    for (long i = 0; i < n; i++) code_byte(&g->sec[g->cur], 0);
}

static void do_align(struct gas *g, long boundary)
{
    long off = cur_off(g);
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
            g->cur = i;
            return 1;
        }
    if (DIR(".section")) {
        char *e = arg;
        while (*e && !isspace((unsigned char)*e) && *e != ',') e++;
        for (int i = 0; i < NSEC; i++)
            if ((size_t)(e - arg) == strlen(sec_name[i]) &&
                memcmp(arg, sec_name[i], (size_t)(e - arg)) == 0) {
                g->cur = i;
                return 1;
            }
        /* A section this assembler has no home for is refused rather
         * than silently folded into .text, which would place its
         * contents somewhere the linker script never expects. */
        gerr(g, ".section \"%.*s\" is not one of .text/.data/.rodata/.bss",
             (int)(e - arg), arg);
        return 1;
    }
    if (DIR(".global") || DIR(".globl")) {
        char *e = arg;
        while (*e && is_symc((unsigned char)*e)) e++;
        if (e == arg) { gerr(g, ".global needs a name"); return 1; }
        sym_get(g, arg, (size_t)(e - arg))->is_global = 1;
        return 1;
    }
    if (DIR(".type")) {
        char *e = arg;
        while (*e && is_symc((unsigned char)*e)) e++;
        if (e != arg && strstr(e, "function"))
            sym_get(g, arg, (size_t)(e - arg))->is_func = 1;
        return 1;
    }
    if (DIR(".size") || DIR(".file") || DIR(".ident") || DIR(".cfi_startproc") ||
        DIR(".cfi_endproc") || DIR(".syntax") || DIR(".thumb") ||
        DIR(".arch") || DIR(".attribute") || DIR(".option") ||
        DIR(".thumb_func") || DIR(".code") || DIR(".fpu") || DIR(".eabi_attribute"))
        return 1;      /* accepted and carried no further */
    if (DIR(".byte") || DIR(".short") || DIR(".half") || DIR(".word") ||
        DIR(".long") || DIR(".quad") || DIR(".dword")) {
        int width = DIR(".byte") ? 1
                  : (DIR(".short") || DIR(".half")) ? 2
                  : (DIR(".quad") || DIR(".dword")) ? 8 : 4;
        for (;;) {
            char *e;
            long v;
            arg = skip_ws(arg);
            if (!*arg) break;
            e = arg;
            while (*e && *e != ',') e++;
            while (e > arg && isspace((unsigned char)e[-1])) e--;
            if (parse_num(arg, e, &v)) {
                emit_int(g, v, width);
            } else if (is_sym0((unsigned char)*arg)) {
                /* A symbol's ADDRESS in a data word: a relocation. */
                int rt = width == 8 ? g->tgt->r_abs64 : g->tgt->r_abs32;
                if (width != 4 && width != 8) {
                    gerr(g, "a symbol address needs a 4- or 8-byte slot");
                } else if (!rt) {
                    gerr(g, "this target has no absolute relocation for a "
                            "%d-byte symbol address", width);
                } else if (pass == 2) {
                    char *se = arg;
                    while (se < e && is_symc((unsigned char)*se)) se++;
                    char save = *se; *se = '\0';
                    fix_add(g, g->cur, cur_off(g), arg, rt, 0);
                    *se = save;
                }
                emit_int(g, 0, width);
            } else {
                gerr(g, "expected a number or a symbol");
            }
            arg = *e == ',' ? e + 1 : e;
            while (*arg == ',') arg++;
        }
        return 1;
    }
    if (DIR(".asciz") || DIR(".string") || DIR(".ascii")) {
        unsigned char buf[4096];
        long n = parse_string(g, arg, buf, (long)sizeof buf);
        if (n >= 0) {
            emit_bytes(g, buf, n);
            if (!DIR(".ascii")) emit_int(g, 0, 1);
        }
        return 1;
    }
    if (DIR(".space") || DIR(".zero") || DIR(".skip")) {
        long v;
        char *e = arg;
        while (*e && *e != ',') e++;
        while (e > arg && isspace((unsigned char)e[-1])) e--;
        if (!parse_num(arg, e, &v)) gerr(g, "%.*s needs a size",
                                         (int)nlen, name);
        else advance(g, v);
        return 1;
    }
    if (DIR(".align") || DIR(".balign") || DIR(".p2align")) {
        long v;
        char *e = arg;
        while (*e && *e != ',') e++;
        while (e > arg && isspace((unsigned char)e[-1])) e--;
        if (!parse_num(arg, e, &v)) { gerr(g, "%.*s needs a number",
                                           (int)nlen, name); return 1; }
        /* .p2align takes an exponent; .align takes one on ARM and a byte
         * count on RISC-V, and GNU as resolves that per target. Both
         * spellings here mean what the target's own assembler means:
         * .p2align is always 2^n, .align/.balign a byte count. */
        if (DIR(".p2align")) { long b = 1; while (v-- > 0) b *= 2; do_align(g, b); }
        else if (v > 0) do_align(g, v);
        return 1;
    }
    gerr(g, "directive \"%.*s\" is not one this assembler knows",
         (int)nlen, name);
    return 1;
#undef DIR
}

/* ---- instructions ---------------------------------------------------- */

/* Rewrites a statement's operands, replacing any bare identifier that
 * names a LOCAL label with `.+N` (the displacement the statement
 * assemblers take). Returns a malloc'd string, and sets *ext to the
 * name when the identifier is an external symbol instead. */
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
        if (!is_sym0((unsigned char)*p) || mid) {
            if (len + 2 >= cap) { cap *= 2; out = xrealloc(out, cap); }
            out[len++] = *p++;
            continue;
        }
        const char *s = p;
        while (*p && is_symc((unsigned char)*p)) p++;
        size_t n = (size_t)(p - s);
        /* The mnemonic itself is never a label, and a register name is
         * never one either -- the target answers that, so this file
         * needs no register table of its own. Everything else that is
         * an identifier IS a symbol reference. */
        int first = s == skip_ws((char *)stmt);
        int reg = g->tgt->is_reg && g->tgt->is_reg(s, (int)n) >= 0;
        if (!first && !reg) {
            struct sym *sy = sym_find(g, s, n);
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

/* `call sym` and `la rd, sym`: the two forms that may name a symbol
 * this file does not define. Both are an auipc paired with a second
 * instruction, eight bytes, and both carry their relocation on the
 * auipc. Anything else naming an undefined symbol is refused. */
static int extern_form(struct gas *g, const char *stmt, long pc, int pass,
                       const char *ext)
{
    const char *p = skip_ws((char *)stmt);
    int is_call = strncmp(p, "call", 4) == 0 && isspace((unsigned char)p[4]);
    int is_la = strncmp(p, "la", 2) == 0 && isspace((unsigned char)p[2]);
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
        struct code tmp = { 0, 0, 0 };
        char err[256];
        const char *rd = "ra";
        char rdbuf[16];
        if (is_la) {
            const char *q = skip_ws((char *)p + 2);
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
        snprintf(buf, sizeof buf, is_call ? "jalr ra, 0(ra)" : "addi %s, %s, 0",
                 rd, rd);
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
    if (!((strncmp(p, "call", 4) == 0 && isspace((unsigned char)p[4])) ||
          (strncmp(p, "la", 2) == 0 && isspace((unsigned char)p[2]))))
        return NULL;
    q = p + (p[1] == 'a' && p[2] != 'l' ? 2 : 4);
    q = skip_ws((char *)q);
    if (*p == 'l') {                       /* la rd, sym -- skip rd */
        while (*q && is_symc((unsigned char)*q)) q++;
        q = skip_ws((char *)q);
        if (*q == ',') q = skip_ws((char *)(q + 1));
    }
    n = 0;
    while (q[n] && is_symc((unsigned char)q[n])) n++;
    if (!n) return NULL;
    return sym_get(g, q, n)->name;
}

static void instruction(struct gas *g, char *stmt, int pass)
{
    long pc = cur_off(g);
    char *ext = NULL;
    char *text;
    int placeheld = 0;
    const char *ps = pseudo_symbol(g, stmt);

    if (ps) {
        extern_form(g, stmt, pc, pass, ps);
        return;
    }
    text = substitute(g, stmt, pc, pass, &ext, &placeheld);
    struct code tmp = { 0, 0, 0 };
    char err[256];

    if (ext) {
        if (!extern_form(g, text, pc, pass, ext))
            gerr(g, "\"%s\" names the undefined symbol '%s'; only `call` and "
                    "`la` can carry one here", stmt, ext);
        free(text);
        return;
    }
    if (g->tgt->encode(text, &tmp, err, sizeof err) != 0) {
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

/* ---- a pass over the source ------------------------------------------ */

static void pass_over(struct gas *g, char *src, int pass)
{
    char *p = src;
    g->cur = SEC_TEXT;
    g->line = 0;
    for (int i = 0; i < NSEC; i++) { free(g->sec[i].p); memset(&g->sec[i], 0, sizeof g->sec[i]); }
    g->bss_size = 0;

    while (*p) {
        char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        char *line = xmalloc(n + 1);
        memcpy(line, p, n);
        line[n] = '\0';
        g->line++;
        strip_comment(line);

        char *q = skip_ws(line);
        /* Any number of `label:` may precede a statement on one line. */
        for (;;) {
            char *e = q;
            if (!is_sym0((unsigned char)*e)) break;
            while (*e && is_symc((unsigned char)*e)) e++;
            char *c = skip_ws(e);
            if (*c != ':') break;
            struct sym *s = sym_get(g, q, (size_t)(e - q));
            if (pass == 1 && s->sec >= 0)
                gerr(g, "label '%s' is defined twice", s->name);
            s->sec = g->cur;
            s->value = cur_off(g);
            q = skip_ws(c + 1);
        }
        if (*q == '.' && is_sym0((unsigned char)q[1]))
            directive(g, q, pass);
        else if (*q)
            instruction(g, q, pass);

        free(line);
        if (!nl) break;
        p = nl + 1;
    }
}

/* ---- the object ------------------------------------------------------ */

static int write_object(struct gas *g, const char *out_path)
{
    struct elfw *w = elfw_new(g->tgt->machine);
    int ndx[NSEC];
    int i;

    for (i = 0; i < NSEC; i++) ndx[i] = 0;
    ndx[SEC_TEXT] = elfw_add_section(w, ".text", SHT_PROGBITS,
                                     SHF_ALLOC | SHF_EXECINSTR,
                                     g->sec[SEC_TEXT].p,
                                     (Elf64_Xword)g->sec[SEC_TEXT].len, 4);
    if (g->sec[SEC_DATA].len)
        ndx[SEC_DATA] = elfw_add_section(w, ".data", SHT_PROGBITS,
                                         SHF_ALLOC | SHF_WRITE,
                                         g->sec[SEC_DATA].p,
                                         (Elf64_Xword)g->sec[SEC_DATA].len, 8);
    if (g->sec[SEC_RODATA].len)
        ndx[SEC_RODATA] = elfw_add_section(w, ".rodata", SHT_PROGBITS,
                                           SHF_ALLOC, g->sec[SEC_RODATA].p,
                                           (Elf64_Xword)g->sec[SEC_RODATA].len, 8);
    if (g->bss_size)
        ndx[SEC_BSS] = elfw_add_section(w, ".bss", SHT_NOBITS,
                                        SHF_ALLOC | SHF_WRITE, NULL,
                                        (Elf64_Xword)g->bss_size, 8);

    /* Locals before globals: the gABI orders a symbol table that way
     * and the writer refuses rather than reordering behind our back. */
    for (int pass = 0; pass < 2; pass++)
        for (i = 0; i < g->nsyms; i++) {
            struct sym *s = &g->syms[i];
            int want_global = pass == 1;
            if (!!s->is_global != want_global) continue;
            if (s->sec < 0 && !s->is_global) {
                /* Named but never defined and never exported: a symbol
                 * only a relocation refers to, which must be UNDEF and
                 * global for the linker to resolve it. */
                continue;
            }
            unsigned char bind = s->is_global ? STB_GLOBAL : STB_LOCAL;
            unsigned char type = s->is_func ? STT_FUNC : STT_NOTYPE;
            s->elf_ndx = elfw_add_symbol(w, s->name,
                                         (Elf64_Addr)(s->sec >= 0 ? s->value : 0),
                                         (Elf64_Xword)s->size,
                                         (Elf64_Uchar)((bind << 4) | type),
                                         (Elf64_Half)(s->sec >= 0 ? ndx[s->sec]
                                                                  : SHN_UNDEF));
        }
    /* Anything a relocation names and nothing defined becomes UNDEF. */
    for (i = 0; i < g->nfix; i++) {
        struct sym *s = sym_find(g, g->fix[i].sym, strlen(g->fix[i].sym));
        if (s && !s->elf_ndx)
            s->elf_ndx = elfw_add_symbol(w, s->name, 0, 0,
                                         (STB_GLOBAL << 4) | STT_NOTYPE,
                                         SHN_UNDEF);
    }
    for (i = 0; i < g->nfix; i++) {
        struct fixup *f = &g->fix[i];
        struct sym *s = sym_find(g, f->sym, strlen(f->sym));
        if (!s || !s->elf_ndx) {
            fprintf(stderr, "%s: error: '%s' is referenced and never "
                            "defined\n", g->path, f->sym);
            g->errors++;
            continue;
        }
        elfw_add_rela(w, ndx[f->sec], (Elf64_Addr)f->off, s->elf_ndx,
                      f->type, f->addend);
    }
    if (g->errors) { elfw_free(w); return 1; }
    int rc = elfw_write(w, out_path);
    elfw_free(w);
    return rc == 0 ? 0 : 1;
}

/* ---- the targets ----------------------------------------------------- */

static const struct gas_target RISCV_GAS = {
    EM_RISCV, 0, rvasm_assemble, rvasm_gpr,
    R_RISCV_CALL, R_RISCV_PCREL_HI20, R_RISCV_PCREL_LO12_I,
    R_RISCV_32, R_RISCV_64
};

static const struct gas_target *target_for(void)
{
    switch (target_get()) {
    case TARGET_RISCV32: case TARGET_RISCV64: return &RISCV_GAS;
    default: return NULL;
    }
}

int gas_assemble(const char *in_path, const char *out_path, int preprocess)
{
    struct gas g;
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
    text = preprocess ? cpp_process(in_path, src, NULL, 0) : src;

    memset(&g, 0, sizeof g);
    g.path = in_path;
    g.tgt = t;

    /* Pass 1 places the labels; pass 2 encodes with the displacements
     * they give. Two passes and not one because a branch forward names
     * a label the assembler has not reached. */
    pass_over(&g, text, 1);
    if (!g.errors)
        pass_over(&g, text, 2);
    rc = g.errors ? 1 : write_object(&g, out_path);

    for (int i = 0; i < NSEC; i++) free(g.sec[i].p);
    for (int i = 0; i < g.nsyms; i++) free(g.syms[i].name);
    for (int i = 0; i < g.nfix; i++) free(g.fix[i].sym);
    free(g.syms); free(g.fix);
    if (text != src) free(text);
    free(src);
    return rc;
}
