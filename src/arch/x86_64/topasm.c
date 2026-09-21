/* A tiny assembler for file-scope `__asm__` blocks. EmbCC has no general
 * text assembler; this recognizes exactly the vocabulary its own sources
 * are written in — directives (.global/.globl and the data directives),
 * labels (named and numeric-local), and a handful of x86-64 instructions —
 * and refuses anything else loudly (THE RULE). It is the file-scope
 * companion to the fixed-register inline asm the syscall stubs use.
 *
 * The data directives carry the weight for anything the mnemonics cannot
 * say. A library routine that must be machine code — setjmp saving the
 * callee-saved registers, longjmp jumping to a stored address — is written
 * as `.byte`/`.long` with its disassembly in a comment beside it, which is
 * the same form `embcc -S` emits and for the same reason: the bytes are
 * what runs, and nothing downstream gets to re-decide them. That half is
 * arch-neutral, so it works on a target this file could not otherwise
 * assemble a single instruction for. */
#include "topasm.h"

#include <stdlib.h>
#include <string.h>

#include "../../driver/util.h"

/* Every instruction has a fixed size, so a straight two-pass assembler
 * works: pass 1 fixes each label's offset, pass 2 emits and resolves the
 * jumps. Jumps are always E9 rel32 (5 bytes) — no rel8/rel32 size choice
 * to iterate, and functionally identical. */

struct line { char *text; int off; };

/* 64-bit register name -> encoding (0-15). The table type is file scope:
 * EmbCC's own subset (which compiles this file) has no block-scope type
 * definitions. */
struct asmreg { const char *name; int reg; };
static const struct asmreg asm_regs[] = {
    { "rax", 0 }, { "rcx", 1 }, { "rdx", 2 }, { "rbx", 3 },
    { "rsp", 4 }, { "rbp", 5 }, { "rsi", 6 }, { "rdi", 7 },
    { "r8", 8 }, { "r9", 9 }, { "r10", 10 }, { "r11", 11 },
    { "r12", 12 }, { "r13", 13 }, { "r14", 14 }, { "r15", 15 },
};
static int reg_num(const char *n)
{
    for (unsigned i = 0; i < sizeof asm_regs / sizeof asm_regs[0]; i++)
        if (strcmp(n, asm_regs[i].name) == 0)
            return asm_regs[i].reg;
    return -1;
}

static int is_ws(char c) { return c == ' ' || c == '\t'; }

static int is_alnum(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
}

/* In place: strip a trailing comment (# or /​* or ;) and surrounding
 * whitespace, and squeeze runs of whitespace to a single space so a line
 * splits on ' ' cleanly. Returns the cleaned string (same buffer). */
static char *clean(char *s)
{
    for (char *p = s; *p; p++)
        if (*p == '#' || *p == ';' ||
            (p[0] == '/' && p[1] == '*')) { *p = 0; break; }
    while (is_ws(*s)) s++;
    char *out = s, *w = s;
    int sp = 0;
    for (char *p = s; *p; p++) {
        if (is_ws(*p)) { sp = 1; continue; }
        if (sp && w != out) *w++ = ' ';
        sp = 0;
        *w++ = *p;
    }
    *w = 0;
    return out;
}

/* The token after the mnemonic, e.g. the target of `call`/`jmp` or the
 * operand list. Skips one leading word + its trailing space. */
static const char *rest(const char *line)
{
    while (*line && !is_ws(*line)) line++;
    while (is_ws(*line)) line++;
    return line;
}

/* Split a line into an array of cleaned non-empty lines. The template is
 * NUL-terminated; newlines separate. */
static struct line *split(const char *tmpl, int *nout)
{
    struct line *v = NULL;
    int n = 0, cap = 0;
    const char *p = tmpl;
    while (*p) {
        const char *e = p;
        while (*e && *e != '\n') e++;
        char *buf = xmalloc((size_t)(e - p) + 1);
        memcpy(buf, p, (size_t)(e - p));
        buf[e - p] = 0;
        char *c = clean(buf);
        if (*c) {
            if (n == cap) {
                cap = cap ? cap * 2 : 8;
                v = xrealloc(v, (size_t)cap * sizeof *v);
            }
            v[n].text = c;
            v[n].off = 0;
            n++;
        }
        p = *e ? e + 1 : e;
    }
    *nout = n;
    return v;
}

/* A data directive's element width in bytes, or 0 if `l` is not one. */
static int data_width(const char *l)
{
    if (strncmp(l, ".byte ", 6) == 0) return 1;
    if (strncmp(l, ".long ", 6) == 0) return 4;
    if (strncmp(l, ".quad ", 6) == 0) return 8;
    return 0;
}

/* How many comma-separated values a data directive carries. */
static int data_count(const char *l)
{
    int n = 1;
    for (const char *p = l; *p; p++)
        if (*p == ',')
            n++;
    return n;
}

/* One line's byte length (0 for a non-data directive or a bare label). */
static int insn_len(struct topasm *ta, const char *l, int mnemonics_ok)
{
    int w = data_width(l);
    if (w)
        return w * data_count(l);
    if (l[0] == '.')
        return 0;                                 /* directive */
    size_t n = strlen(l);
    if (n && l[n - 1] == ':')
        return 0;                                 /* label */
    /* On a target this file cannot encode for, say so in those words
     * rather than blaming the mnemonic: the block may be perfectly good
     * asm that simply needs writing as data here. */
    if (!mnemonics_ok)
        diag_fatal(ta->file, ta->line,
                   "file-scope asm instruction \"%s\": EmbCC assembles "
                   "instructions for x86-64 only. On this target write the "
                   "block as .byte/.long data (see lib/libc/src/setjmp).", l);
    if (strncmp(l, "and ", 4) == 0)
        return 4;                                 /* 48 83 /4 ib */
    if (strncmp(l, "call ", 5) == 0)
        return 5;                                 /* E8 rel32 */
    if (strncmp(l, "jmp ", 4) == 0)
        return 5;                                 /* E9 rel32 */
    if (strcmp(l, "ret") == 0)
        return 1;
    diag_fatal(ta->file, ta->line,
               "file-scope asm instruction not supported: \"%s\" (EmbCC "
               "assembles .global/labels/.byte/.long/.quad and "
               "and/call/jmp/ret)", l);
    return 0;
}

static void push_sym(struct topasm *ta, const char *name, int off, int glob)
{
    ta->syms = xrealloc(ta->syms,
                        (size_t)(ta->nsyms + 1) * sizeof *ta->syms);
    ta->syms[ta->nsyms].name = name;
    ta->syms[ta->nsyms].off = off;
    ta->syms[ta->nsyms].is_global = glob;
    ta->nsyms++;
}

/* Resolve a numeric-local (`1b`/`1f`) or named jump target to its .text
 * offset. */
static int label_off(struct topasm *ta, const char *t, int here)
{
    size_t n = strlen(t);
    if (n >= 2 && (t[n - 1] == 'b' || t[n - 1] == 'f')) {
        int back = t[n - 1] == 'b';
        char num[16];
        if (n - 1 >= sizeof num) n = sizeof num;
        memcpy(num, t, n - 1);
        num[n - 1] = 0;
        int best = -1;
        for (int i = 0; i < ta->nsyms; i++) {
            if (strcmp(ta->syms[i].name, num) != 0)
                continue;
            int o = ta->syms[i].off;
            if (back ? (o <= here && (best < 0 || o > best))
                     : (o > here && (best < 0 || o < best)))
                best = o;
        }
        if (best >= 0)
            return best;
    } else {
        for (int i = 0; i < ta->nsyms; i++)
            if (strcmp(ta->syms[i].name, t) == 0)
                return ta->syms[i].off;
    }
    diag_fatal(ta->file, ta->line,
               "file-scope asm jump target \"%s\" is not a local label", t);
    return 0;
}

/* A line may carry a leading `label:` before its instruction
 * (`1:  jmp 1b`). Define the label (pass 1 only) at `off` and return the
 * pointer past it; the instruction, if any, is at the same offset. */
static const char *strip_label(struct topasm *ta, const char *l, int off,
                               int define)
{
    const char *p = l;
    while (*p && *p != ':' && !is_ws(*p))
        p++;
    if (*p != ':')
        return l;
    if (define) {
        size_t n = (size_t)(p - l);
        char *name = xmalloc(n + 1);
        memcpy(name, l, n);
        name[n] = 0;
        push_sym(ta, name, off, 0);
    }
    p++;
    while (is_ws(*p))
        p++;
    return p;
}

void topasm_assemble(struct topasm *ta, int mnemonics_ok)
{
    int nl;
    struct line *ls = split(ta->tmpl, &nl);

    /* Pass 1: assign each instruction an offset; define every label; note
     * which names .global marks. Numeric labels can repeat, so labels are
     * kept as (name, off) pairs, not a map. */
    int off = 0;
    for (int i = 0; i < nl; i++) {
        const char *l = ls[i].text;
        if (strncmp(l, ".global ", 8) == 0 || strncmp(l, ".globl ", 7) == 0)
            continue; /* matched to its label in the fixup below */
        l = strip_label(ta, l, off, 1);
        ls[i].off = off;
        if (*l)
            off += insn_len(ta, l, mnemonics_ok);
    }
    /* apply .global to the matching label symbols */
    for (int i = 0; i < nl; i++) {
        const char *l = ls[i].text;
        const char *nm = NULL;
        if (strncmp(l, ".global ", 8) == 0) nm = l + 8;
        else if (strncmp(l, ".globl ", 7) == 0) nm = l + 7;
        if (!nm) continue;
        int found = 0;
        for (int k = 0; k < ta->nsyms; k++)
            if (strcmp(ta->syms[k].name, nm) == 0) {
                ta->syms[k].is_global = 1;
                found = 1;
            }
        if (!found)
            diag_fatal(ta->file, ta->line,
                       "asm .global names \"%s\", which has no label", nm);
    }
    /* Pass 2: emit bytes and resolve jumps/relocs. */
    ta->code = xmalloc((size_t)(off ? off : 1));
    ta->codelen = 0;
    unsigned char *c = ta->code;
    for (int i = 0; i < nl; i++) {
        const char *l = ls[i].text;
        int w = data_width(l);
        if (!w && l[0] == '.')
            continue;
        l = strip_label(ta, l, 0, 0);   /* labels defined in pass 1 */
        if (!*l)
            continue;
        int here = ls[i].off;
        w = data_width(l);
        if (w) {
            /* Little-endian, which is both targets. Values are parsed as
             * unsigned so 0xff and 0xffffffff are written as given rather
             * than overflowing a signed long on the way in. */
            const char *p = l + 6;
            int nth = 0;
            for (;;) {
                char *end;
                unsigned long v = strtoul(p, &end, 0);
                if (end == p) {
                    /* Not a number, so a symbol -- `.quad main`. This is
                     * how an asm block names a symbol on a target whose
                     * INSTRUCTIONS this file cannot encode: the address
                     * becomes data, and the code beside it loads and
                     * branches through it. Only .quad, because an address
                     * is eight bytes and a truncated one would relocate
                     * into whatever followed. */
                    if (w != 8)
                        diag_fatal(ta->file, ta->line,
                                   "asm .byte/.long wants a number: \"%s\" "
                                   "(a symbol's address is eight bytes, so "
                                   "name it with .quad)", l);
                    const char *s = p;
                    while (*end && (is_alnum(*end) || *end == '_' ||
                                    *end == '.' || *end == '$'))
                        end++;
                    if (end == s)
                        diag_fatal(ta->file, ta->line,
                                   "asm data directive wants a number or a "
                                   "symbol: \"%s\"", l);
                    ta->rels = xrealloc(ta->rels,
                                        (size_t)(ta->nrels + 1)
                                            * sizeof *ta->rels);
                    ta->rels[ta->nrels].off = here + nth * w;
                    ta->rels[ta->nrels].target = xstrndup(s, (size_t)(end - s));
                    ta->rels[ta->nrels].addend = 0;
                    ta->rels[ta->nrels].kind = ASMREL_ABS64;
                    ta->nrels++;
                    v = 0;                    /* the linker fills it in */
                }
                for (int b = 0; b < w; b++)
                    *c++ = (unsigned char)((v >> (8 * b)) & 0xff);
                nth++;
                while (is_ws(*end)) end++;
                if (*end != ',')
                    break;
                p = end + 1;
                while (is_ws(*p)) p++;
            }
        } else if (strncmp(l, "and ", 4) == 0) {
            const char *ops = l + 4;
            if (ops[0] != '$')
                diag_fatal(ta->file, ta->line,
                           "asm 'and' wants an immediate: \"%s\"", l);
            char *end;
            long imm = strtol(ops + 1, &end, 0);
            while (is_ws(*end) || *end == ',') end++;
            if (*end != '%')
                diag_fatal(ta->file, ta->line,
                           "asm 'and' wants a %%register: \"%s\"", l);
            int reg = reg_num(end + 1);
            if (reg < 0)
                diag_fatal(ta->file, ta->line,
                           "asm 'and' unknown register: \"%s\"", l);
            if (imm < -128 || imm > 127)
                diag_fatal(ta->file, ta->line,
                           "asm 'and' immediate %ld out of int8 range", imm);
            *c++ = (unsigned char)(0x48 | (reg >= 8 ? 1 : 0)); /* REX.W(.B) */
            *c++ = 0x83;
            *c++ = (unsigned char)(0xe0 | (reg & 7));          /* /4 = AND */
            *c++ = (unsigned char)imm;
        } else if (strncmp(l, "call ", 5) == 0) {
            const char *tgt = rest(l);
            *c++ = 0xe8;
            *c++ = 0; *c++ = 0; *c++ = 0; *c++ = 0;
            ta->rels = xrealloc(ta->rels,
                                (size_t)(ta->nrels + 1) * sizeof *ta->rels);
            ta->rels[ta->nrels].off = here + 1; /* the rel32 field */
            ta->rels[ta->nrels].target = xstrndup(tgt, strlen(tgt));
            ta->rels[ta->nrels].addend = -4;
            ta->rels[ta->nrels].kind = ASMREL_PC32;
            ta->nrels++;
        } else if (strncmp(l, "jmp ", 4) == 0) {
            const char *tgt = rest(l);
            int dest = label_off(ta, tgt, here);
            int rel = dest - (here + 5);
            *c++ = 0xe9;
            *c++ = (unsigned char)(rel & 0xff);
            *c++ = (unsigned char)((rel >> 8) & 0xff);
            *c++ = (unsigned char)((rel >> 16) & 0xff);
            *c++ = (unsigned char)((rel >> 24) & 0xff);
        } else if (strcmp(l, "ret") == 0) {
            *c++ = 0xc3;
        }
    }
    ta->codelen = (int)(c - ta->code);
}
