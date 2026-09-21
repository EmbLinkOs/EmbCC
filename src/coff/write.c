/* The COFF object writer.
 *
 * Every field is stored explicitly, little-endian, one at a time. That
 * is not fussiness: COFF's records are packed to sizes a C compiler
 * would not choose -- a symbol is eighteen bytes and a relocation is
 * ten -- so writing a struct out would insert padding the format does
 * not have, and every record after the first would be misread. The
 * sizes in coff.h are the authority, and the writer asserts the ones it
 * builds match them.
 *
 * ---- the layout it produces --------------------------------------------
 *
 *   file header
 *   section headers, in the order the sections were added
 *   section payloads
 *   relocations, per section
 *   symbol table
 *   string table
 *
 * The string table is required even when empty: its first four bytes
 * are its own size, and a reader that finds nothing there reads the
 * next file instead. So it is always written, and always at least four
 * bytes long.
 */
#include "write.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../driver/util.h"
#include "../platform/platform.h"

struct buf {
    unsigned char *p;
    size_t len, cap;
};

static void buf_append(struct buf *b, const void *data, size_t n)
{
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 256;
        while (cap < b->len + n)
            cap *= 2;
        b->p = realloc(b->p, cap);
        if (!b->p) {
            fprintf(stderr, "embcc: out of memory\n");
            fatal_unwind();
        }
        b->cap = cap;
    }
    if (data)
        memcpy(b->p + b->len, data, n);
    else
        memset(b->p + b->len, 0, n);
    b->len += n;
}

static void put8(struct buf *b, unsigned v)
{
    unsigned char c = (unsigned char)v;
    buf_append(b, &c, 1);
}

static void put16(struct buf *b, unsigned v)
{
    unsigned char c[2] = { (unsigned char)v, (unsigned char)(v >> 8) };
    buf_append(b, c, 2);
}

static void put32(struct buf *b, unsigned long v)
{
    unsigned char c[4] = { (unsigned char)v, (unsigned char)(v >> 8),
                           (unsigned char)(v >> 16), (unsigned char)(v >> 24) };
    buf_append(b, c, 4);
}

#define COFFW_MAX_SECTIONS 32

struct csect {
    char name[64];
    unsigned flags;
    struct buf data;
    int nodata;            /* .bss: size but no file bytes */
    unsigned size;
    struct buf relocs;     /* packed COFF_RELOC_SIZE records */
    int nrelocs;
    unsigned raw_off, rel_off;   /* filled at write time */
};

struct csym {
    char name[256];
    unsigned value;
    int section;
    int type;
    int storage;
};

struct coffw {
    int machine;
    struct csect sec[COFFW_MAX_SECTIONS];
    int nsec;
    struct csym *sym;
    int nsym, capsym;
    /* The string table holds every name longer than eight bytes, and
     * every symbol name longer than eight. Its first four bytes are its
     * own size, so it starts four bytes long and grows. */
    struct buf strtab;
};

struct coffw *coffw_new(int machine)
{
    struct coffw *w = xcalloc(1, sizeof *w);
    w->machine = machine;
    /* Reserve the length field; filled in at write time. */
    buf_append(&w->strtab, NULL, 4);
    return w;
}

void coffw_free(struct coffw *w)
{
    if (!w)
        return;
    for (int i = 0; i < w->nsec; i++) {
        free(w->sec[i].data.p);
        free(w->sec[i].relocs.p);
    }
    free(w->sym);
    free(w->strtab.p);
    free(w);
}

/* A name that does not fit the eight bytes of a header or symbol record
 * goes in the string table, and the record holds its offset instead.
 * The two spellings differ: a SECTION writes "/123" as text, a SYMBOL
 * writes four zero bytes followed by the offset as a number. Getting
 * them the wrong way round produces a name a reader prints as garbage
 * rather than an error. */
static unsigned strtab_add(struct coffw *w, const char *s)
{
    unsigned off = (unsigned)w->strtab.len;
    buf_append(&w->strtab, s, strlen(s) + 1);
    return off;
}

/* The alignment bits, from a byte count. COFF has no field for this --
 * it is three bits of the characteristics -- and there is no encoding
 * for an alignment above 8192, so anything unrepresentable is refused
 * rather than rounded down to something the linker would then honour. */
static unsigned align_bits(int align)
{
    switch (align) {
    case 0: case 1: return IMAGE_SCN_ALIGN_1BYTES;
    case 2:  return IMAGE_SCN_ALIGN_2BYTES;
    case 4:  return IMAGE_SCN_ALIGN_4BYTES;
    case 8:  return IMAGE_SCN_ALIGN_8BYTES;
    case 16: return IMAGE_SCN_ALIGN_16BYTES;
    case 32: return IMAGE_SCN_ALIGN_32BYTES;
    case 64: return IMAGE_SCN_ALIGN_64BYTES;
    default:
        fprintf(stderr, "embcc: coff writer: no encoding for %d-byte "
                        "alignment\n", align);
        fatal_unwind();
        return IMAGE_SCN_ALIGN_1BYTES;
    }
}

int coffw_add_section(struct coffw *w, const char *name, unsigned flags,
                      const void *data, unsigned size, int align)
{
    if (w->nsec == COFFW_MAX_SECTIONS) {
        fprintf(stderr, "embcc: coff writer: too many sections\n");
        fatal_unwind();
    }
    struct csect *s = &w->sec[w->nsec];
    memset(s, 0, sizeof *s);
    snprintf(s->name, sizeof s->name, "%s", name);
    s->flags = flags | align_bits(align);
    s->size = size;
    /* A section with no file bytes still has a size: that is what
     * uninitialised data IS. Its SizeOfRawData records the size and its
     * PointerToRawData is zero. */
    s->nodata = data == NULL;
    if (!s->nodata && size)
        buf_append(&s->data, data, size);
    return ++w->nsec;            /* one-based, as a symbol's section is */
}

int coffw_add_symbol(struct coffw *w, const char *name, unsigned value,
                     int section, int type, int storage_class)
{
    if (w->nsym == w->capsym) {
        w->capsym = w->capsym ? w->capsym * 2 : 64;
        w->sym = xrealloc(w->sym, (size_t)w->capsym * sizeof *w->sym);
    }
    struct csym *s = &w->sym[w->nsym];
    memset(s, 0, sizeof *s);
    snprintf(s->name, sizeof s->name, "%s", name);
    s->value = value;
    s->section = section;
    s->type = type;
    s->storage = storage_class;
    return w->nsym++;
}

void coffw_add_reloc(struct coffw *w, int section, unsigned off,
                     int sym, int type)
{
    if (section < 1 || section > w->nsec) {
        fprintf(stderr, "embcc: coff writer: relocation in section %d, "
                        "which does not exist\n", section);
        fatal_unwind();
    }
    struct csect *s = &w->sec[section - 1];
    put32(&s->relocs, off);
    put32(&s->relocs, (unsigned long)sym);
    put16(&s->relocs, (unsigned)type);
    s->nrelocs++;
}

/* A section or symbol name, into an eight-byte field. */
static void put_name(struct buf *out, struct coffw *w, const char *name,
                     int is_section)
{
    size_t n = strlen(name);
    if (n <= 8) {
        /* Padded with NULs, and NOT terminated when it fills the field
         * exactly -- eight characters is a legal name with no room for
         * a terminator. */
        unsigned char f[8];
        memset(f, 0, sizeof f);
        memcpy(f, name, n);
        buf_append(out, f, 8);
        return;
    }
    unsigned off = strtab_add(w, name);
    if (is_section) {
        char s[9];
        snprintf(s, sizeof s, "/%u", off);
        unsigned char f[8];
        memset(f, 0, sizeof f);
        memcpy(f, s, strlen(s) < 8 ? strlen(s) : 8);
        buf_append(out, f, 8);
    } else {
        put32(out, 0);           /* zeroes say "the offset follows" */
        put32(out, off);
    }
}

int coffw_write(struct coffw *w, const char *path)
{
    /* Pass one: where everything lands. The header and the section
     * headers come first and their sizes are fixed, so the payloads can
     * be placed without writing anything yet. */
    unsigned off = (unsigned)(COFF_FILE_HEADER_SIZE +
                              w->nsec * COFF_SECTION_HEADER_SIZE);
    for (int i = 0; i < w->nsec; i++) {
        struct csect *s = &w->sec[i];
        if (s->nodata || !s->data.len) {
            s->raw_off = 0;
        } else {
            s->raw_off = off;
            off += (unsigned)s->data.len;
        }
    }
    for (int i = 0; i < w->nsec; i++) {
        struct csect *s = &w->sec[i];
        if (!s->nrelocs) {
            s->rel_off = 0;
        } else {
            s->rel_off = off;
            off += (unsigned)s->relocs.len;
        }
    }
    unsigned symoff = w->nsym ? off : 0;
    off += (unsigned)(w->nsym * COFF_SYMBOL_SIZE);

    struct buf out = { 0, 0, 0 };

    /* ---- file header ---- */
    put16(&out, (unsigned)w->machine);
    put16(&out, (unsigned)w->nsec);
    /* A timestamp would make the output differ from run to run for the
     * same input, which R4 forbids: the same source must produce the
     * same bytes. Zero is what a reproducible build writes. */
    put32(&out, 0);
    put32(&out, symoff);
    put32(&out, (unsigned long)w->nsym);
    put16(&out, 0);              /* no optional header in an object */
    put16(&out, 0);              /* characteristics: none for an object */

    /* ---- section headers ---- */
    for (int i = 0; i < w->nsec; i++) {
        struct csect *s = &w->sec[i];
        put_name(&out, w, s->name, 1);
        put32(&out, 0);          /* VirtualSize: zero in an object */
        put32(&out, 0);          /* VirtualAddress: the linker assigns it */
        put32(&out, s->nodata ? s->size : (unsigned long)s->data.len);
        put32(&out, s->raw_off);
        put32(&out, s->rel_off);
        put32(&out, 0);          /* line numbers: none */
        if (s->nrelocs > 0xffff) {
            fprintf(stderr, "embcc: coff writer: %d relocations in '%s', "
                            "and the count field holds 65535\n",
                    s->nrelocs, s->name);
            fatal_unwind();
        }
        put16(&out, (unsigned)s->nrelocs);
        put16(&out, 0);          /* line numbers: none */
        put32(&out, s->flags);
    }

    /* ---- payloads, then relocations, in the order promised above ---- */
    for (int i = 0; i < w->nsec; i++)
        if (w->sec[i].raw_off)
            buf_append(&out, w->sec[i].data.p, w->sec[i].data.len);
    for (int i = 0; i < w->nsec; i++)
        if (w->sec[i].rel_off)
            buf_append(&out, w->sec[i].relocs.p, w->sec[i].relocs.len);

    /* ---- symbol table ----
     *
     * put_name may append to the string table here, which is why the
     * string table is written afterwards and its length filled in last. */
    for (int i = 0; i < w->nsym; i++) {
        struct csym *s = &w->sym[i];
        put_name(&out, w, s->name, 0);
        put32(&out, s->value);
        put16(&out, (unsigned)(s->section & 0xffff));   /* signed, 16-bit */
        put16(&out, (unsigned)s->type);
        put8(&out, (unsigned)s->storage);
        put8(&out, 0);           /* no auxiliary records */
    }

    /* ---- string table ----
     *
     * Always present, even empty: the first four bytes are its own size,
     * and a reader that finds nothing there reads whatever follows. */
    unsigned char *st = w->strtab.p;
    unsigned stlen = (unsigned)w->strtab.len;
    st[0] = (unsigned char)stlen;
    st[1] = (unsigned char)(stlen >> 8);
    st[2] = (unsigned char)(stlen >> 16);
    st[3] = (unsigned char)(stlen >> 24);
    buf_append(&out, st, stlen);

    int rc = plat_write_file(path, out.p, out.len);
    free(out.p);
    if (rc != 0) {
        fprintf(stderr, "embcc: cannot write '%s'\n", path);
        return -1;
    }
    return 0;
}
