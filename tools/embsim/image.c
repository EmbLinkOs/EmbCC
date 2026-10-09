/* image.c -- the image's symbols, code ranges, line table and call frame
 * information, for the analyses (coverage, the profile, the stack
 * report, the fault report). ELFCLASS32 and ELFCLASS64, little-endian;
 * DWARF versions 2 to 5 for the line table, and .debug_frame's CIE
 * versions 1, 3 and 4 with no augmentation, as EmbCC, GCC and clang
 * write them for bare-metal images. image.h says what each table is. */
#include <stdlib.h>
#include <string.h>

#include "image.h"

static u32 le16(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8; }
static u32 le32(const u8 *p) { return le16(p) | le16(p + 2) << 16; }
static u64 le64(const u8 *p) { return le32(p) | (u64)le32(p + 4) << 32; }

static void *xalloc(size_t n)
{
    void *p = calloc(1, n ? n : 1);
    if (!p)
        die("out of memory");
    return p;
}

static void *xgrow(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q)
        die("out of memory");
    return q;
}

/* ---- reading DWARF's encodings, bounded by `end` ----------------------- */

struct rd {
    const u8 *p, *end;
    int bad;
};

static u64 rd_n(struct rd *r, int n)
{
    u64 v = 0;
    if (r->end - r->p < n) {
        r->bad = 1;
        r->p = r->end;
        return 0;
    }
    for (int i = n - 1; i >= 0; i--)
        v = v << 8 | r->p[i];
    r->p += n;
    return v;
}

static u64 uleb(struct rd *r)
{
    u64 v = 0;
    int sh = 0;
    for (;;) {
        if (r->p >= r->end) {
            r->bad = 1;
            return v;
        }
        u8 b = *r->p++;
        if (sh < 64)
            v |= (u64)(b & 0x7f) << sh;
        sh += 7;
        if (!(b & 0x80))
            return v;
    }
}

static s64 sleb(struct rd *r)
{
    u64 v = 0;
    int sh = 0;
    u8 b = 0;
    do {
        if (r->p >= r->end) {
            r->bad = 1;
            return (s64)v;
        }
        b = *r->p++;
        if (sh < 64)
            v |= (u64)(b & 0x7f) << sh;
        sh += 7;
    } while (b & 0x80);
    if (sh < 64 && (b & 0x40))
        v |= ~0ull << sh;
    return (s64)v;
}

static const char *rd_str(struct rd *r)
{
    const char *s = (const char *)r->p;
    while (r->p < r->end && *r->p)
        r->p++;
    if (r->p >= r->end) {
        r->bad = 1;
        return "";
    }
    r->p++;
    return s;
}

/* ---- sections, segments and symbols ------------------------------------ */

struct sec {
    const u8 *data;
    u32 size;
};

static int find_sec(const struct image *m, const char *want, struct sec *out)
{
    const u8 *f = m->f;
    int e64 = m->e64;
    u64 shoff = e64 ? le64(f + 40) : le32(f + 32);
    u32 shentsize = le16(f + (e64 ? 58 : 46));
    u32 shnum = le16(f + (e64 ? 60 : 48));
    u32 shstrndx = le16(f + (e64 ? 62 : 50));
    if (!shoff || shstrndx >= shnum ||
        shoff + (u64)shnum * shentsize > m->len)
        return 0;
    const u8 *ss = f + shoff + (u64)shstrndx * shentsize;
    u64 stroff = e64 ? le64(ss + 24) : le32(ss + 16);
    u64 strsz = e64 ? le64(ss + 32) : le32(ss + 20);
    if (stroff + strsz > m->len)
        return 0;
    for (u32 i = 0; i < shnum; i++) {
        const u8 *sh = f + shoff + (u64)i * shentsize;
        u32 nm = le32(sh);
        u64 off = e64 ? le64(sh + 24) : le32(sh + 16);
        u64 sz = e64 ? le64(sh + 32) : le32(sh + 20);
        if (nm >= strsz || le32(sh + 4) == 8)       /* SHT_NOBITS */
            continue;
        const char *name = (const char *)f + stroff + nm;
        if (memchr(name, 0, (size_t)(strsz - nm)) && !strcmp(name, want) &&
            off + sz <= m->len) {
            out->data = f + off;
            out->size = (u32)sz;
            return 1;
        }
    }
    return 0;
}

static int in_code(const struct image *m, u32 a)
{
    for (int i = 0; i < m->ncode; i++)
        if (a >= m->code_lo[i] && a < m->code_hi[i])
            return 1;
    return 0;
}

static int by_addr(const void *a, const void *b)
{
    const struct img_sym *x = a, *y = b;
    if (x->addr != y->addr)
        return x->addr < y->addr ? -1 : 1;
    /* at one address the sized symbol first, then by name */
    if ((x->size != 0) != (y->size != 0))
        return x->size ? -1 : 1;
    return strcmp(x->name, y->name);
}

static int by_name(const void *a, const void *b)
{
    const struct img_sym *x = a, *y = b;
    return strcmp(x->name, y->name);
}

static void read_symbols(struct image *m)
{
    const u8 *f = m->f;
    int e64 = m->e64;
    u64 shoff = e64 ? le64(f + 40) : le32(f + 32);
    u32 shentsize = le16(f + (e64 ? 58 : 46));
    u32 shnum = le16(f + (e64 ? 60 : 48));
    int arm = le16(f + 18) == 40;
    if (!shoff || shoff + (u64)shnum * shentsize > m->len)
        return;
    for (u32 i = 0; i < shnum; i++) {
        const u8 *sh = f + shoff + (u64)i * shentsize;
        if (le32(sh + 4) != 2)                      /* SHT_SYMTAB */
            continue;
        u64 off = e64 ? le64(sh + 24) : le32(sh + 16);
        u64 sz = e64 ? le64(sh + 32) : le32(sh + 20);
        u32 link = le32(sh + (e64 ? 40 : 24));
        if (link >= shnum || off + sz > m->len)
            continue;
        const u8 *ls = f + shoff + (u64)link * shentsize;
        u64 stroff = e64 ? le64(ls + 24) : le32(ls + 16);
        u64 strsz = e64 ? le64(ls + 32) : le32(ls + 20);
        if (stroff + strsz > m->len)
            continue;
        u32 esz = e64 ? 24 : 16, n = (u32)(sz / esz);
        m->sym = xgrow(m->sym, (size_t)(m->nsym + (int)n) * sizeof *m->sym);
        m->fn = xgrow(m->fn, (size_t)(m->nfn + (int)n) * sizeof *m->fn);
        for (u32 k = 1; k < n; k++) {
            const u8 *s = f + off + (u64)k * esz;
            u32 nm = le32(s);
            u64 val = e64 ? le64(s + 8) : le32(s + 4);
            u64 size = e64 ? le64(s + 16) : le32(s + 8);
            int info = s[e64 ? 4 : 12], type = info & 15;
            u32 shndx = le16(s + (e64 ? 6 : 14));
            if (!nm || nm >= strsz || !shndx || type > 2)
                continue;
            const char *name = (const char *)f + stroff + nm;
            if (!memchr(name, 0, (size_t)(strsz - nm)) || name[0] == '$' ||
                (name[0] == '.' && name[1] == 'L'))
                continue;
            u32 a = (u32)val;
            m->sym[m->nsym].addr = a;
            m->sym[m->nsym].size = (u32)size;
            m->sym[m->nsym].name = name;
            m->nsym++;
            /* a function: STT_FUNC, or a label in the code (assembly) */
            if (arm && type == 2)
                a &= ~1u;
            if (type == 2 || (type == 0 && in_code(m, a))) {
                m->fn[m->nfn].addr = a;
                m->fn[m->nfn].size = type == 2 ? (u32)size : 0;
                m->fn[m->nfn].name = name;
                m->nfn++;
            }
        }
    }
    if (m->nsym)
        qsort(m->sym, (size_t)m->nsym, sizeof *m->sym, by_name);
    if (!m->nfn)
        return;
    qsort(m->fn, (size_t)m->nfn, sizeof *m->fn, by_addr);
    /* one function per address; a label with no size runs to the next */
    int w = 0;
    for (int i = 0; i < m->nfn; i++)
        if (!w || m->fn[i].addr != m->fn[w - 1].addr)
            m->fn[w++] = m->fn[i];
    m->nfn = w;
    for (int i = 0; i < m->nfn; i++) {
        struct img_sym *s = &m->fn[i];
        if (s->size)
            continue;
        u32 end = s->addr;
        for (int c = 0; c < m->ncode; c++)
            if (s->addr >= m->code_lo[c] && s->addr < m->code_hi[c])
                end = m->code_hi[c];
        if (i + 1 < m->nfn && m->fn[i + 1].addr < end)
            end = m->fn[i + 1].addr;
        s->size = end - s->addr;
    }
}

int image_fn(const struct image *m, u32 pc)
{
    int lo = 0, hi = m->nfn - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (m->fn[mid].addr <= pc) {
            best = mid;
            lo = mid + 1;
        } else
            hi = mid - 1;
    }
    /* the nearest start at or below pc; an inner symbol that ended
     * gives way to the one around it */
    for (int i = best; i >= 0 && best - i < 8; i--)
        if (pc - m->fn[i].addr < m->fn[i].size)
            return i;
    return -1;
}

int image_sym(const struct image *m, const char *name, u32 *addr)
{
    int lo = 0, hi = m->nsym - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strcmp(m->sym[mid].name, name);
        if (!c) {
            *addr = m->sym[mid].addr;
            return 1;
        }
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return 0;
}

/* ---- the line table ------------------------------------------------------ */

static int add_file(struct image *m, const char *dir, const char *name)
{
    size_t dl = dir && *dir && name[0] != '/' ? strlen(dir) : 0;
    char *p = xalloc(dl + strlen(name) + 2);
    if (dl) {
        memcpy(p, dir, dl);
        if (p[dl - 1] != '/')
            p[dl++] = '/';
    }
    strcpy(p + dl, name);
    /* the same path in two units is one file */
    for (int i = 0; i < m->nfile; i++)
        if (!strcmp(m->file[i], p)) {
            free(p);
            return i;
        }
    m->file = xgrow(m->file, (size_t)(m->nfile + 1) * sizeof *m->file);
    m->file[m->nfile] = p;
    return m->nfile++;
}

/* a v5 entry format's attribute value: a string or a number */
static const char *v5_form(struct image *m, struct rd *r, u64 form, u64 *num,
                           int dwarf64)
{
    struct sec s;
    *num = 0;
    switch (form) {
    case 0x08:                                  /* DW_FORM_string */
        return rd_str(r);
    case 0x1f:                                  /* DW_FORM_line_strp */
    case 0x0e: {                                /* DW_FORM_strp */
        u64 off = rd_n(r, dwarf64 ? 8 : 4);
        if (find_sec(m, form == 0x1f ? ".debug_line_str" : ".debug_str", &s) &&
            off < s.size && memchr(s.data + off, 0, s.size - off))
            return (const char *)s.data + off;
        return "";
    }
    case 0x0f: *num = uleb(r); return 0;        /* udata */
    case 0x0b: *num = rd_n(r, 1); return 0;     /* data1 */
    case 0x05: *num = rd_n(r, 2); return 0;     /* data2 */
    case 0x06: *num = rd_n(r, 4); return 0;     /* data4 */
    case 0x07: *num = rd_n(r, 8); return 0;     /* data8 */
    case 0x1e: rd_n(r, 8); rd_n(r, 8); return 0;   /* data16 (MD5) */
    case 0x09: {                                /* block */
        u64 n = uleb(r);
        if ((u64)(r->end - r->p) < n)
            r->bad = 1;
        else
            r->p += n;
        return 0;
    }
    }
    r->bad = 1;
    return 0;
}

static void add_row(struct image *m, int *cap, u32 lo, u32 hi, int file,
                    int line)
{
    if (hi <= lo || file < 0)
        return;
    if (m->nln == *cap) {
        *cap = *cap ? 2 * *cap : 256;
        m->ln = xgrow(m->ln, (size_t)*cap * sizeof *m->ln);
    }
    m->ln[m->nln].lo = lo;
    m->ln[m->nln].hi = hi;
    m->ln[m->nln].file = file;
    m->ln[m->nln].line = line;
    m->nln++;
}

static int by_lo(const void *a, const void *b)
{
    const struct img_line *x = a, *y = b;
    if (x->lo != y->lo)
        return x->lo < y->lo ? -1 : 1;
    return 0;
}

static void read_lines(struct image *m)
{
    struct sec ls;
    if (!find_sec(m, ".debug_line", &ls))
        return;
    int cap = 0;
    const u8 *p = ls.data, *end = ls.data + ls.size;
    while (end - p >= 4) {
        struct rd r = { p, end, 0 };
        u64 ulen = rd_n(&r, 4);
        int dwarf64 = 0;
        if (ulen == 0xffffffffu) {
            ulen = rd_n(&r, 8);
            dwarf64 = 1;
        }
        if (!ulen || ulen > (u64)(end - r.p))
            break;
        const u8 *uend = r.p + ulen;
        p = uend;
        r.end = uend;
        u32 ver = (u32)rd_n(&r, 2);
        if (ver < 2 || ver > 5)
            continue;
        if (ver >= 5)
            rd_n(&r, 2);                    /* address_size, seg_sel_size */
        u64 hlen = rd_n(&r, dwarf64 ? 8 : 4);
        if (hlen > (u64)(uend - r.p))
            continue;
        const u8 *prog = r.p + hlen;
        u32 min_inst = (u32)rd_n(&r, 1);
        if (ver >= 4)
            rd_n(&r, 1);                    /* maximum_operations_per_instruction */
        rd_n(&r, 1);                        /* default_is_stmt */
        int line_base = (signed char)rd_n(&r, 1);
        u32 line_range = (u32)rd_n(&r, 1);
        u32 opcode_base = (u32)rd_n(&r, 1);
        const u8 *std_len = r.p;
        if (opcode_base == 0 || line_range == 0 ||
            (u64)(uend - r.p) < opcode_base - 1)
            continue;
        r.p += opcode_base - 1;
        /* this unit's files, as indices into m->file */
        int *fmap = 0, nf = 0;
        if (ver < 5) {
            const char **dirs = 0;
            int nd = 0;
            while (r.p < r.end && *r.p) {
                dirs = xgrow(dirs, (size_t)(nd + 1) * sizeof *dirs);
                dirs[nd++] = rd_str(&r);
            }
            rd_n(&r, 1);
            fmap = xgrow(fmap, sizeof *fmap);
            fmap[nf++] = -1;                /* file 0 is no file before v5 */
            while (r.p < r.end && *r.p && !r.bad) {
                const char *name = rd_str(&r);
                u64 d = uleb(&r);
                uleb(&r);
                uleb(&r);
                fmap = xgrow(fmap, (size_t)(nf + 1) * sizeof *fmap);
                fmap[nf++] = add_file(m, d && d <= (u64)nd ? dirs[d - 1] : 0,
                                      name);
            }
            free(dirs);
        } else {
            const char **dirs = 0;
            int nd = 0;
            for (int pass = 0; pass < 2 && !r.bad; pass++) {
                u32 nfmt = (u32)rd_n(&r, 1);
                u64 fmt[16][2];
                for (u32 k = 0; k < nfmt; k++) {
                    u64 a = uleb(&r), fo = uleb(&r);
                    if (k < 16) {
                        fmt[k][0] = a;
                        fmt[k][1] = fo;
                    }
                }
                if (nfmt > 16) {
                    r.bad = 1;
                    break;
                }
                u64 cnt = uleb(&r);
                for (u64 e = 0; e < cnt && !r.bad; e++) {
                    const char *name = "";
                    u64 dir = 0;
                    for (u32 k = 0; k < nfmt; k++) {
                        u64 num;
                        const char *sv = v5_form(m, &r, fmt[k][1], &num, dwarf64);
                        if (fmt[k][0] == 1 && sv)           /* DW_LNCT_path */
                            name = sv;
                        else if (fmt[k][0] == 2)            /* DW_LNCT_directory_index */
                            dir = num;
                    }
                    if (pass == 0) {
                        dirs = xgrow(dirs, (size_t)(nd + 1) * sizeof *dirs);
                        dirs[nd++] = name;
                    } else {
                        fmap = xgrow(fmap, (size_t)(nf + 1) * sizeof *fmap);
                        fmap[nf++] = add_file(m, dir < (u64)nd ? dirs[dir] : 0,
                                              name);
                    }
                }
            }
            free(dirs);
        }
        if (r.bad) {
            free(fmap);
            continue;
        }
        /* the line-number program */
        r.p = prog;
        u64 addr = 0;
        int file = 1, line = 1, have = 0;
        u64 row_addr = 0;
        int row_file = 0, row_line = 0;
#define EMIT(A)                                                         \
    do {                                                                \
        if (have)                                                       \
            add_row(m, &cap, (u32)row_addr, (u32)(A),                   \
                    row_file >= 0 && row_file < nf ? fmap[row_file] : -1, \
                    row_line);                                          \
        row_addr = (A);                                                 \
        row_file = file;                                                \
        row_line = line;                                                \
        have = 1;                                                       \
    } while (0)
        while (r.p < r.end && !r.bad) {
            u32 op = (u32)rd_n(&r, 1);
            if (op >= opcode_base) {
                u32 adj = op - opcode_base;
                addr += (u64)(adj / line_range) * min_inst;
                line += line_base + (int)(adj % line_range);
                EMIT(addr);
                continue;
            }
            switch (op) {
            case 0: {                                   /* extended */
                u64 n = uleb(&r);
                if (!n || n > (u64)(r.end - r.p)) {
                    r.bad = 1;
                    break;
                }
                const u8 *next = r.p + n;
                u32 sub = (u32)rd_n(&r, 1);
                if (sub == 1) {                         /* end_sequence */
                    if (have)
                        add_row(m, &cap, (u32)row_addr, (u32)addr,
                                row_file >= 0 && row_file < nf ? fmap[row_file] : -1,
                                row_line);
                    have = 0;
                    addr = 0;
                    file = 1;
                    line = 1;
                } else if (sub == 2) {                  /* set_address */
                    addr = rd_n(&r, (int)(n - 1 > 8 ? 8 : n - 1));
                }
                r.p = next;
                break;
            }
            case 1: EMIT(addr); break;                  /* copy */
            case 2: addr += uleb(&r) * min_inst; break; /* advance_pc */
            case 3: line += (int)sleb(&r); break;       /* advance_line */
            case 4: file = (int)uleb(&r); break;        /* set_file */
            case 5: uleb(&r); break;                    /* set_column */
            case 8:                                     /* const_add_pc */
                addr += (u64)((255 - opcode_base) / line_range) * min_inst;
                break;
            case 9: addr += rd_n(&r, 2); break;         /* fixed_advance_pc */
            default:
                for (u32 a = 0; a < std_len[op - 1]; a++)
                    uleb(&r);
            }
        }
#undef EMIT
        free(fmap);
    }
    if (m->nln)
        qsort(m->ln, (size_t)m->nln, sizeof *m->ln, by_lo);
}

const struct img_line *image_line(const struct image *m, u32 pc)
{
    int lo = 0, hi = m->nln - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (m->ln[mid].lo <= pc) {
            best = mid;
            lo = mid + 1;
        } else
            hi = mid - 1;
    }
    for (int i = best; i >= 0 && best - i < 4; i--)
        if (pc >= m->ln[i].lo && pc < m->ln[i].hi)
            return &m->ln[i];
    return 0;
}

const char *image_where(const struct image *m, u32 pc, char *buf, size_t n)
{
    int f = image_fn(m, pc);
    const struct img_line *l = image_line(m, pc);
    size_t k;
    if (f >= 0 && pc != m->fn[f].addr)
        snprintf(buf, n, "%s+0x%x", m->fn[f].name, pc - m->fn[f].addr);
    else if (f >= 0)
        snprintf(buf, n, "%s", m->fn[f].name);
    else
        snprintf(buf, n, "0x%08x", pc);
    k = strlen(buf);
    if (l && k < n) {
        const char *file = m->file[l->file], *b = strrchr(file, '/');
        snprintf(buf + k, n - k, " (%s:%d)", b ? b + 1 : file, l->line);
    }
    return buf;
}

/* ---- the call frame information ----------------------------------------- */

struct cie {
    u32 code_align;
    s64 data_align;
    int ra;
    const u8 *init, *init_end;
};

static int read_cie(const struct image *m, u32 off, struct cie *c)
{
    struct rd r = { m->frame + off, m->frame + m->frame_len, 0 };
    u64 len = rd_n(&r, 4);
    if (len >= 0xfffffff0u || len > (u64)(r.end - r.p))
        return 0;
    r.end = r.p + len;
    if (rd_n(&r, 4) != 0xffffffffu)
        return 0;
    u32 ver = (u32)rd_n(&r, 1);
    const char *aug = rd_str(&r);
    if (*aug || (ver != 1 && ver != 3 && ver != 4))
        return 0;
    if (ver == 4) {
        rd_n(&r, 1);                        /* address_size */
        rd_n(&r, 1);                        /* segment_selector_size */
    }
    c->code_align = (u32)uleb(&r);
    c->data_align = sleb(&r);
    c->ra = (int)(ver == 1 ? rd_n(&r, 1) : uleb(&r));
    c->init = r.p;
    c->init_end = r.end;
    return !r.bad;
}

/* run CFI instructions from address `loc` up to the row for `pc`; `init`
 * is the CIE's row, which DW_CFA_restore goes back to (0 while running
 * the CIE's own instructions) */
static int run_cfi(const struct image *m, const u8 *p, const u8 *end,
                   const struct cie *c, u64 loc, u32 pc,
                   struct cfi_row *row, const struct cfi_row *init)
{
    struct rd r = { p, end, 0 };
    struct cfi_row stack[8];
    int sp = 0;
    int asz = m->e64 ? 8 : 4;
    while (r.p < r.end && !r.bad) {
        u32 op = (u32)rd_n(&r, 1), hi = op & 0xc0, lo = op & 0x3f;
        u64 reg = lo, adv = 0;
        s64 off;
        if (hi == 0x40) {
            adv = lo;
        } else if (hi == 0x80) {
            off = (s64)uleb(&r) * c->data_align;
            if (reg < CFI_REGS) {
                row->how[reg] = CFI_OFFSET;
                row->off[reg] = off;
            }
            continue;
        } else if (hi == 0xc0) {
            if (reg < CFI_REGS && init) {
                row->how[reg] = init->how[reg];
                row->off[reg] = init->off[reg];
            }
            continue;
        } else {
            switch (op) {
            case 0x00: continue;                        /* nop */
            case 0x01: {                                /* set_loc */
                u64 to = rd_n(&r, asz);
                if (to > pc)
                    return 1;
                loc = to;
                continue;
            }
            case 0x02: adv = rd_n(&r, 1); break;
            case 0x03: adv = rd_n(&r, 2); break;
            case 0x04: adv = rd_n(&r, 4); break;
            case 0x05:                                  /* offset_extended */
            case 0x11:                                  /* offset_extended_sf */
            case 0x14:                                  /* val_offset */
            case 0x15:                                  /* val_offset_sf */
                reg = uleb(&r);
                off = (op == 0x05 || op == 0x14 ? (s64)uleb(&r) : sleb(&r)) *
                      c->data_align;
                if (reg < CFI_REGS) {
                    row->how[reg] = op >= 0x14 ? CFI_VALOFF : CFI_OFFSET;
                    row->off[reg] = off;
                }
                continue;
            case 0x06:                                  /* restore_extended */
                reg = uleb(&r);
                if (reg < CFI_REGS && init) {
                    row->how[reg] = init->how[reg];
                    row->off[reg] = init->off[reg];
                }
                continue;
            case 0x07:                                  /* undefined */
            case 0x08:                                  /* same_value */
                reg = uleb(&r);
                if (reg < CFI_REGS)
                    row->how[reg] = op == 0x07 ? CFI_UNDEF : CFI_SAME;
                continue;
            case 0x09: {                                /* register */
                reg = uleb(&r);
                u64 r2 = uleb(&r);
                if (reg < CFI_REGS) {
                    row->how[reg] = CFI_REG;
                    row->off[reg] = (s64)r2;
                }
                continue;
            }
            case 0x0a:                                  /* remember_state */
                if (sp == 8)
                    return 0;
                stack[sp++] = *row;
                continue;
            case 0x0b:                                  /* restore_state */
                if (!sp)
                    return 0;
                *row = stack[--sp];             /* the CFA rule with them */
                continue;
            case 0x0c:                                  /* def_cfa */
                row->cfa_reg = (int)uleb(&r);
                row->cfa_off = (s64)uleb(&r);
                continue;
            case 0x0d:                                  /* def_cfa_register */
                row->cfa_reg = (int)uleb(&r);
                continue;
            case 0x0e:                                  /* def_cfa_offset */
                row->cfa_off = (s64)uleb(&r);
                continue;
            case 0x12:                                  /* def_cfa_sf */
                row->cfa_reg = (int)uleb(&r);
                row->cfa_off = sleb(&r) * c->data_align;
                continue;
            case 0x13:                                  /* def_cfa_offset_sf */
                row->cfa_off = sleb(&r) * c->data_align;
                continue;
            case 0x2e:                                  /* GNU_args_size */
                uleb(&r);
                continue;
            default:                                    /* expressions */
                return 0;
            }
        }
        /* an advance: the row so far holds up to loc + adv */
        loc += adv * c->code_align;
        if (loc > pc)
            return 1;
    }
    return !r.bad;
}

int cfi_at(const struct image *m, u32 pc, struct cfi_row *row)
{
    if (!m->frame)
        return 0;
    int asz = m->e64 ? 8 : 4;
    u32 off = 0;
    while (m->frame_len - off >= 8) {
        struct rd r = { m->frame + off, m->frame + m->frame_len, 0 };
        u64 len = rd_n(&r, 4);
        if (!len || len >= 0xfffffff0u || len > (u64)(r.end - r.p))
            return 0;
        u32 next = off + 4 + (u32)len;
        r.end = r.p + len;
        u64 cie_ptr = rd_n(&r, 4);
        if (cie_ptr == 0xffffffffu) {
            off = next;
            continue;
        }
        u64 start = rd_n(&r, asz), range = rd_n(&r, asz);
        if (pc >= start && pc - start < range && !r.bad) {
            struct cie c;
            struct cfi_row init;
            if (cie_ptr >= m->frame_len || !read_cie(m, (u32)cie_ptr, &c))
                return 0;
            memset(&init, 0, sizeof init);
            init.ra = c.ra;
            if (!run_cfi(m, c.init, c.init_end, &c, 0, 0xffffffffu, &init, 0))
                return 0;
            *row = init;
            return run_cfi(m, r.p, r.end, &c, start, pc, row, &init);
        }
        off = next;
    }
    return 0;
}

/* ---- the image ----------------------------------------------------------- */

struct image *image_open(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp)
        die("cannot open %s", path);
    struct image *m = xalloc(sizeof *m);
    size_t cap = 1 << 16;
    m->f = xalloc(cap);
    for (;;) {
        if (m->len == cap) {
            cap *= 2;
            m->f = xgrow(m->f, cap);
        }
        size_t got = fread(m->f + m->len, 1, cap - m->len, fp);
        if (!got)
            break;
        m->len += got;
    }
    fclose(fp);
    const u8 *f = m->f;
    if (m->len < 52 || memcmp(f, "\177ELF", 4))
        die("%s is not an ELF file", path);
    m->e64 = f[4] == 2;
    if (m->e64 && m->len < 64)
        die("%s is not an ELF file", path);
    u64 phoff = m->e64 ? le64(f + 32) : le32(f + 28);
    u32 phentsize = le16(f + (m->e64 ? 54 : 42));
    u32 phnum = le16(f + (m->e64 ? 56 : 44));
    for (u32 i = 0; i < phnum && m->ncode < IMG_CODE; i++) {
        if (phoff + (u64)(i + 1) * phentsize > m->len)
            break;
        const u8 *ph = f + phoff + (u64)i * phentsize;
        u32 flags = le32(ph + (m->e64 ? 4 : 24));
        u64 va = m->e64 ? le64(ph + 16) : le32(ph + 8);
        u64 msz = m->e64 ? le64(ph + 40) : le32(ph + 20);
        if (le32(ph) != 1 || !(flags & 1) || !msz || va + msz > 0x100000000ull)
            continue;
        m->code_lo[m->ncode] = (u32)va;
        m->code_hi[m->ncode] = (u32)(va + msz);
        m->ncode++;
    }
    read_symbols(m);
    read_lines(m);
    struct sec fr;
    if (find_sec(m, ".debug_frame", &fr)) {
        m->frame = fr.data;
        m->frame_len = fr.size;
    }
    return m;
}
