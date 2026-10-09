#include "code.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asmexpr.h"
#include "../driver/util.h"

void code_mark_data(struct code *c, int start, int end)
{
    if (end <= start)
        return;
    if (c->ndrange + 2 > c->capdrange) {
        c->capdrange = c->capdrange ? c->capdrange * 2 : 16;
        c->drange = xrealloc(c->drange, (size_t)c->capdrange * sizeof *c->drange);
    }
    c->drange[c->ndrange++] = start;
    c->drange[c->ndrange++] = end;
}

void code_byte(struct code *c, int b)
{
    if (c->len == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 256;
        c->p = xrealloc(c->p, (size_t)c->cap);
    }
    c->p[c->len++] = (unsigned char)b;
}

void code_u32(struct code *c, unsigned long v)
{
    code_byte(c, (int)(v & 0xff));
    code_byte(c, (int)((v >> 8) & 0xff));
    code_byte(c, (int)((v >> 16) & 0xff));
    code_byte(c, (int)((v >> 24) & 0xff));
}

/* Two bytes, little-endian: a RISC-V compressed instruction, and the
 * halfword a Thumb one is built from. */
void code_u16(struct code *c, unsigned v)
{
    code_byte(c, (int)(v & 0xff));
    code_byte(c, (int)((v >> 8) & 0xff));
}

void code_patch32(struct code *c, int off, unsigned long v)
{
    c->p[off] = (unsigned char)(v & 0xff);
    c->p[off + 1] = (unsigned char)((v >> 8) & 0xff);
    c->p[off + 2] = (unsigned char)((v >> 16) & 0xff);
    c->p[off + 3] = (unsigned char)((v >> 24) & 0xff);
}

void code_align(struct code *c, int align, int fill)
{
    while (c->len % align)
        code_byte(c, fill);
}

/* ---- an inline asm template's directives (code.h) ---------------------- */

const struct asm_datadir asm_data_x86[] = {
    /* x86's .word is two bytes, as GNU as and llvm-mc have it there */
    { ".byte", 1 }, { ".short", 2 }, { ".value", 2 }, { ".word", 2 },
    { ".2byte", 2 }, { ".long", 4 }, { ".int", 4 }, { ".4byte", 4 },
    { ".quad", 8 }, { ".8byte", 8 }, { NULL, 0 }
};
const struct asm_datadir asm_data_a64[] = {
    { ".byte", 1 }, { ".short", 2 }, { ".hword", 2 }, { ".value", 2 },
    { ".2byte", 2 }, { ".word", 4 }, { ".long", 4 }, { ".int", 4 },
    { ".4byte", 4 }, { ".quad", 8 }, { ".xword", 8 }, { ".dword", 8 },
    { ".8byte", 8 }, { NULL, 0 }
};
const struct asm_datadir asm_data_w32[] = {
    /* a 32-bit word machine's: MIPS, LoongArch */
    { ".byte", 1 }, { ".short", 2 }, { ".half", 2 }, { ".hword", 2 },
    { ".value", 2 }, { ".2byte", 2 }, { ".word", 4 }, { ".long", 4 },
    { ".int", 4 }, { ".4byte", 4 }, { ".quad", 8 }, { ".dword", 8 },
    { ".8byte", 8 }, { NULL, 0 }
};
const struct asm_datadir asm_data_avr[] = {
    /* the MACHINE's word: two bytes on AVR, as in src/as/gas.c */
    { ".byte", 1 }, { ".short", 2 }, { ".value", 2 }, { ".word", 2 },
    { ".2byte", 2 }, { ".long", 4 }, { ".int", 4 }, { ".4byte", 4 },
    { ".quad", 8 }, { ".8byte", 8 }, { NULL, 0 }
};

int asm_stmt_len(const char *p, const char *seps)
{
    int n = 0, q = 0;
    for (; p[n]; n++) {
        if (q) {
            if (p[n] == '\\' && p[n + 1])
                n++;
            else if (p[n] == '"')
                q = 0;
            continue;
        }
        if (p[n] == '"')
            q = 1;
        else if (p[n] == '\n' || strchr(seps, p[n]))
            break;
    }
    return n;
}

int asm_cut_comment(const char *s, int len, const char *marks, int slashes)
{
    int q = 0;
    for (int i = 0; i < len; i++) {
        if (q) {
            if (s[i] == '\\' && i + 1 < len)
                i++;
            else if (s[i] == '"')
                q = 0;
            continue;
        }
        if (s[i] == '"')
            q = 1;
        else if (strchr(marks, s[i]) ||
                 (slashes && s[i] == '/' && i + 1 < len && s[i + 1] == '/'))
            return i;
    }
    return len;
}

#define DFAIL(...) do { snprintf(err, (size_t)errlen, __VA_ARGS__); \
                        return -1; } while (0)

static int dir_is(const char *w, int n, const char *name)
{
    if ((int)strlen(name) != n)
        return 0;
    for (int i = 0; i < n; i++)
        if (tolower((unsigned char)w[i]) != name[i])
            return 0;
    return 1;
}

/* The operand list s[i..len), cut at its commas (outside parentheses and
 * strings): the start and end of operand k, or 0 when there is none. */
static int dir_operand(const char *s, int len, int *i, int *b, int *e)
{
    int depth = 0, q = 0;
    while (*i < len && isspace((unsigned char)s[*i]))
        (*i)++;
    if (*i >= len)
        return 0;
    *b = *i;
    for (; *i < len; (*i)++) {
        char ch = s[*i];
        if (q) {
            if (ch == '\\' && *i + 1 < len) (*i)++;
            else if (ch == '"') q = 0;
            continue;
        }
        if (ch == '"') q = 1;
        else if (ch == '(') depth++;
        else if (ch == ')') depth--;
        else if (ch == ',' && depth <= 0) break;
    }
    *e = *i;
    while (*e > *b && isspace((unsigned char)s[*e - 1]))
        (*e)--;
    if (*i < len)
        (*i)++;                         /* the comma */
    return 1;
}

/* One "..." string at s[b..e), unescaped as llvm-mc reads one. */
static int dir_string(const char *s, int b, int e, struct code *out,
                      char *err, int errlen)
{
    if (e - b < 2 || s[b] != '"' || s[e - 1] != '"')
        DFAIL("\"%.*s\" is not a quoted string", e - b, s + b);
    for (int i = b + 1; i < e - 1; i++) {
        int ch = (unsigned char)s[i];
        if (ch == '"')
            DFAIL("a string ends inside %.*s", e - b, s + b);
        if (ch != '\\') {
            code_byte(out, ch);
            continue;
        }
        if (++i >= e - 1)
            DFAIL("a string ends in a backslash: %.*s", e - b, s + b);
        ch = (unsigned char)s[i];
        switch (ch) {
        case 'b': code_byte(out, '\b'); break;
        case 'f': code_byte(out, '\f'); break;
        case 'n': code_byte(out, '\n'); break;
        case 'r': code_byte(out, '\r'); break;
        case 't': code_byte(out, '\t'); break;
        case '"': code_byte(out, '"'); break;
        case '\\': code_byte(out, '\\'); break;
        case 'x': case 'X': {
            int v = 0, any = 0;
            while (i + 1 < e - 1 && isxdigit((unsigned char)s[i + 1])) {
                int d = (unsigned char)s[++i];
                v = v * 16 + (isdigit(d) ? d - '0' : tolower(d) - 'a' + 10);
                v &= 0xff;
                any = 1;
            }
            if (!any)
                DFAIL("\\x with no hex digits in %.*s", e - b, s + b);
            code_byte(out, v);
            break;
        }
        default:
            if (ch >= '0' && ch <= '7') {
                int v = ch - '0';
                for (int k = 0; k < 2 && i + 1 < e - 1 &&
                                s[i + 1] >= '0' && s[i + 1] <= '7'; k++)
                    v = v * 8 + (s[++i] - '0');
                code_byte(out, v & 0xff);
                break;
            }
            DFAIL("\\%c is not an escape an assembler reads, in %.*s", ch,
                  e - b, s + b);
        }
    }
    return 1;
}

static int dir_const(const char *s, int b, int e, const char *what,
                     long long *v, char *err, int errlen)
{
    if (e == b || !asm_const_expr(s + b, e - b, v))
        DFAIL("\"%.*s\" in %s is not a constant: an inline asm directive "
              "takes numbers, and a symbol would need a relocation",
              e - b, s + b, what);
    return 1;
}

int code_asm_directive(const char *stmt, int len, struct code *out,
                       const struct asm_dirs *d, char *err, int errlen)
{
    int i = 0, w, wl, b, e, at = out->len;
    char name[16];
    while (i < len && isspace((unsigned char)stmt[i]))
        i++;
    w = i;
    while (i < len && !isspace((unsigned char)stmt[i]))
        i++;
    wl = i - w;
    if (wl < 2 || stmt[w] != '.' || wl >= (int)sizeof name)
        return 0;
    snprintf(name, sizeof name, "%.*s", wl, stmt + w);

    if (dir_is(stmt + w, wl, ".ascii") || dir_is(stmt + w, wl, ".asciz") ||
        dir_is(stmt + w, wl, ".string")) {
        int nul = !dir_is(stmt + w, wl, ".ascii"), any = 0;
        while (dir_operand(stmt, len, &i, &b, &e)) {
            if (dir_string(stmt, b, e, out, err, errlen) < 0)
                return -1;
            if (nul)
                code_byte(out, 0);
            any = 1;
        }
        if (!any)
            DFAIL("%s wants a string", name);
        code_mark_data(out, at, out->len);
        return 1;
    }

    if (dir_is(stmt + w, wl, ".p2align") || dir_is(stmt + w, wl, ".balign") ||
        dir_is(stmt + w, wl, ".align")) {
        long long n, fill = -1, max = 0;
        int bytes = dir_is(stmt + w, wl, ".balign") ||
                    (dir_is(stmt + w, wl, ".align") && d->align_bytes);
        if (!dir_operand(stmt, len, &i, &b, &e))
            DFAIL("%s wants an alignment", name);
        if (dir_const(stmt, b, e, name, &n, err, errlen) < 0)
            return -1;
        if (dir_operand(stmt, len, &i, &b, &e) && e > b) {
            if (dir_const(stmt, b, e, name, &fill, err, errlen) < 0)
                return -1;
            if (fill < -128 || fill > 255)
                DFAIL("%s: the fill %lld is not a byte", name, fill);
            fill &= 0xff;
        }
        if (dir_operand(stmt, len, &i, &b, &e) &&
            dir_const(stmt, b, e, name, &max, err, errlen) < 0)
            return -1;
        if (dir_operand(stmt, len, &i, &b, &e))
            DFAIL("%s takes an alignment, a fill and a maximum", name);
        if (n < 0 || (!bytes && n > 30) || (bytes && (n & (n - 1))))
            DFAIL("%s %lld: the alignment is not a power of two", name, n);
        long long boundary = bytes ? n : 1LL << n;
        if (boundary <= 1)
            return 1;
        /* .text is sixteen-aligned in every object EmbCC writes; more
         * would need the section's alignment raised with it */
        if (boundary > 16)
            DFAIL("%s %lld asks for %lld-byte alignment, and inline asm in "
                  "a function takes at most 16: the section is aligned to "
                  "16", name, n, boundary);
        if (max < 0)
            DFAIL("%s: a negative maximum", name);
        if (out->narange + 4 > out->caparange) {
            out->caparange = out->caparange ? out->caparange * 2 : 16;
            out->arange = xrealloc(out->arange,
                                   (size_t)out->caparange * sizeof *out->arange);
        }
        out->arange[out->narange++] = out->len;
        out->arange[out->narange++] = (int)boundary;
        out->arange[out->narange++] = (int)fill;
        out->arange[out->narange++] = (int)max;
        return 1;
    }

    if (d->data) {
        int size = 0, any = 0;
        for (int k = 0; d->data[k].name; k++)
            if (dir_is(stmt + w, wl, d->data[k].name))
                size = d->data[k].size;
        if (!size)
            return 0;
        while (dir_operand(stmt, len, &i, &b, &e)) {
            long long v;
            if (dir_const(stmt, b, e, name, &v, err, errlen) < 0)
                return -1;
            if (size < 8 && (v < -(1LL << (8 * size - 1)) ||
                             v > (long long)((1ULL << (8 * size)) - 1)))
                DFAIL("%lld does not fit in %d byte%s", v, size,
                      size == 1 ? "" : "s");
            for (int k = 0; k < size; k++)
                code_byte(out, (int)(((unsigned long long)v >>
                    (8 * (d->big_endian ? size - 1 - k : k))) & 0xff));
            any = 1;
        }
        if (!any)
            DFAIL("%s wants a value", name);
        code_mark_data(out, at, out->len);
        return 1;
    }
    return 0;
}

void code_fill(struct code *c, int kind, long gap)
{
    /* x86's nops of one to ten bytes; longer ones are prefixed with 0x66 */
    static const unsigned char x86nop[10][10] = {
        { 0x90 },
        { 0x66, 0x90 },
        { 0x0f, 0x1f, 0x00 },
        { 0x0f, 0x1f, 0x40, 0x00 },
        { 0x0f, 0x1f, 0x44, 0x00, 0x00 },
        { 0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00 },
        { 0x0f, 0x1f, 0x80, 0x00, 0x00, 0x00, 0x00 },
        { 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 },
        { 0x66, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 },
        { 0x66, 0x2e, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 },
    };
    switch (kind) {
    case CODE_FILL_X86:
        while (gap > 0) {
            int n = gap > 15 ? 15 : (int)gap;
            int pre = n > 10 ? n - 10 : 0;
            for (int k = 0; k < pre; k++)
                code_byte(c, 0x66);
            for (int k = 0; k < n - pre; k++)
                code_byte(c, x86nop[n - pre - 1][k]);
            gap -= n;
        }
        return;
    case CODE_FILL_A64:
        for (; gap % 4; gap--)
            code_byte(c, 0);
        for (; gap > 0; gap -= 4)
            code_u32(c, 0xd503201fUL);
        return;
    case CODE_FILL_THUMB2:
    case CODE_FILL_THUMB1:
        for (; gap >= 2; gap -= 2)
            code_u16(c, kind == CODE_FILL_THUMB2 ? 0xbf00u : 0x46c0u);
        if (gap)
            code_byte(c, 0);
        return;
    case CODE_FILL_A32:
        for (; gap >= 4; gap -= 4)
            code_u32(c, 0xe320f000UL);
        if (gap == 3) {
            code_byte(c, 0); code_byte(c, 0); code_byte(c, 0xa0);
        } else {
            for (; gap > 0; gap--)
                code_byte(c, 0);
        }
        return;
    case CODE_FILL_RISCV:
        if (gap % 2) {
            code_byte(c, 0);
            gap--;
        }
        if (gap % 4) {
            code_u16(c, 0x0001);                         /* c.nop */
            gap -= 2;
        }
        for (; gap > 0; gap -= 4)
            code_u32(c, 0x00000013UL);                   /* nop */
        return;
    default:
        for (; gap > 0; gap--)
            code_byte(c, 0);
        return;
    }
}

/* The padding an alignment needs at pos: none when it would pass `max`. */
static int asm_pad(long pos, int boundary, int max)
{
    int p = (int)((boundary - pos % boundary) % boundary);
    return max && p > max ? 0 : p;
}

void code_put_asm(struct code *c, const unsigned char *b, int len,
                  const int *dr, int ndr, const int *ar, int nar, int fill)
{
    int base = c->len, k = 0, n = nar / 4;
    int *pad = xcalloc((size_t)(n ? n : 1), sizeof *pad);
    for (int a = 0; a < n; a++) {
        const int *p = ar + 4 * a;
        for (; k < p[0]; k++)
            code_byte(c, b[k]);
        pad[a] = asm_pad(c->len, p[1], p[3]);
        if (p[2] >= 0)
            for (int j = 0; j < pad[a]; j++)
                code_byte(c, p[2]);
        else
            code_fill(c, fill, pad[a]);
    }
    for (; k < len; k++)
        code_byte(c, b[k]);
    /* a range moves by the padding before it; it ends where its data does,
     * and then runs on over padding right after it */
    for (int r = 0; r + 1 < ndr; r += 2) {
        int s = dr[r], e = dr[r + 1], ps = 0, pe = 0;
        for (int a = 0; a < n; a++) {
            if (ar[4 * a] <= s)
                ps += pad[a];
            if (ar[4 * a] <= e)
                pe += pad[a];
        }
        code_mark_data(c, base + s + ps, base + e + pe);
    }
    free(pad);
}

int code_asm_align_max(const int *ar, int nar)
{
    int m = 0;
    for (int a = 0; a + 3 < nar; a += 4)
        if (ar[a + 1] > m)
            m = ar[a + 1];
    return m;
}

int code_asm_align_slack(const int *ar, int nar)
{
    int s = 0;
    for (int a = 0; a + 3 < nar; a += 4)
        s += ar[a + 1] - 1;
    return s;
}

/* Where the template ends when it starts at `start` */
static long asm_end_at(const struct code *c, long start)
{
    long pos = start;
    int k = 0;
    for (int a = 0; a + 3 < c->narange; a += 4) {
        pos += c->arange[a] - k;
        k = c->arange[a];
        pos += asm_pad(pos, c->arange[a + 1], c->arange[a + 3]);
    }
    return pos + (c->len - k);
}

int code_asm_settle(struct code *c, int insn, int fill, char *err, int errlen)
{
    int n = 0;
    (void)err;
    (void)errlen;
    /* Data that leaves the code after the template off an instruction
     * boundary -- `.byte 7` alone -- is followed by zeros up to one, as
     * GNU as pads an AArch64 instruction that follows data; the compiler's
     * next instruction would otherwise be misaligned, which no core runs. */
    if (insn > 1)
        for (long st = 0; st < 16; st += insn)
            if (asm_end_at(c, st) % insn) {
                if (c->narange + 4 > c->caparange) {
                    c->caparange = c->caparange ? c->caparange * 2 : 16;
                    c->arange = xrealloc(c->arange, (size_t)c->caparange *
                                                    sizeof *c->arange);
                }
                c->arange[c->narange++] = c->len;
                c->arange[c->narange++] = insn;
                c->arange[c->narange++] = 0;
                c->arange[c->narange++] = 0;
                break;
            }
    /* the alignments before the first larger one: the template's start
     * is a multiple of each, so they pad as they would at offset 0 */
    while (n + 3 < c->narange && c->arange[n + 1] <= insn)
        n += 4;
    if (n) {
        struct code s;
        memset(&s, 0, sizeof s);
        code_put_asm(&s, c->p, c->len, c->drange, c->ndrange, c->arange, n,
                     fill);
        /* the open ones move by what was settled before them (all of it:
         * they come after) */
        int shift = s.len - c->len;
        for (int a = n; a + 3 < c->narange; a += 4) {
            int *q = c->arange + a - n;
            q[0] = c->arange[a] + shift;
            q[1] = c->arange[a + 1];
            q[2] = c->arange[a + 2];
            q[3] = c->arange[a + 3];
        }
        c->narange -= n;
        free(c->p);
        free(c->drange);
        c->p = s.p;
        c->len = s.len;
        c->cap = s.cap;
        c->drange = s.drange;
        c->ndrange = s.ndrange;
        c->capdrange = s.capdrange;
    }
    return code_asm_align_max(c->arange, c->narange);
}
