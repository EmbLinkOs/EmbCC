/* embpack -- a linked firmware image as the file a programmer or a
 * bootloader takes.
 *
 *   embpack IMAGE -o OUT [--format bin|hex|srec|uf2] [--fill BYTE]
 *                        [--pad-to SIZE] [--crc32 SYMBOL] [--append-crc32]
 *                        [--uf2-family ID] [--manifest FILE.json]
 *
 * It reads a linked ELF image (EmbLD's, GNU ld's or lld's; 32- or 64-bit,
 * either byte order) and writes the bytes the image STORES -- every
 * allocated section with contents, at its LOAD address -- as:
 *
 *   bin   raw bytes from the lowest load address to the highest, the gaps
 *         filled (--fill, 0 by default as objcopy does; 0xff is what
 *         erased flash holds), optionally padded to a size;
 *   hex   Intel HEX, the format most programmers and IDEs read;
 *   srec  Motorola S-records (S3, 32-bit addresses);
 *   uf2   the USB Flashing Format of the RP2040 and many other bootloaders
 *         that mount as a drive: 512-byte blocks of 256 bytes each, with a
 *         family ID (--uf2-family) that tells the bootloader which part.
 *
 * And it can stamp the image: --crc32 SYMBOL writes the CRC-32 (the zlib
 * polynomial) of everything stored except the symbol's own four bytes into
 * them, for firmware that checks itself at boot; --append-crc32 adds the
 * CRC of the whole binary after it. --manifest writes what was packed --
 * the segments, sizes, entry point, CRC-32 and SHA-256 -- as JSON.
 *
 * ISO C and standalone, like embmap. docs/manual/tools/embpack.md is the
 * reference. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned long long u64;
typedef unsigned int u32;

static void die(const char *fmt, const char *a)
{
    fprintf(stderr, "embpack: ");
    fprintf(stderr, fmt, a);
    fputc('\n', stderr);
    exit(2);
}

/* ---- reading the image ------------------------------------------------ */

static unsigned char *g_buf;
static size_t g_len;
static int g_be, g_is64;

static u64 rd(size_t off, int n)
{
    u64 v = 0;
    if (off + (size_t)n > g_len || off + (size_t)n < off)
        die("%s: truncated ELF file", "image");
    for (int k = 0; k < n; k++)
        v = (v << 8) | g_buf[off + (size_t)(g_be ? k : n - 1 - k)];
    return v;
}

static void wr(unsigned char *p, u64 v, int n, int be)
{
    for (int k = 0; k < n; k++)
        p[be ? n - 1 - k : k] = (unsigned char)(v >> (8 * k));
}

static unsigned char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        die("cannot open %s", path);
    size_t cap = 1 << 16, n = 0;
    unsigned char *p = malloc(cap);
    if (!p)
        die("%s", "out of memory");
    for (;;) {
        if (n == cap) {
            cap *= 2;
            p = realloc(p, cap);
            if (!p)
                die("%s", "out of memory");
        }
        size_t got = fread(p + n, 1, cap - n, f);
        if (got == 0)
            break;
        n += got;
    }
    fclose(f);
    *len = n;
    return p;
}

struct seg { const char *name; u64 lma, size; size_t off; };
static struct seg *g_seg;
static int g_nseg;
static u64 g_entry;
static unsigned g_machine;

/* the symbol --crc32 names: its load address, or -1 */
static int sym_find(const char *want, u64 *lma);

static u64 g_shoff;
static unsigned g_shnum, g_shentsize, g_shstrndx;
static u64 *g_phv, *g_php, *g_phm;
static int g_nph;

static const char *strat(size_t off)
{
    if (off >= g_len || !memchr(g_buf + off, 0, g_len - off))
        return "";
    return (const char *)g_buf + off;
}

/* An address's load address: through the load segment it runs in. */
static u64 to_lma(u64 vaddr)
{
    for (int j = 0; j < g_nph; j++)
        if (vaddr >= g_phv[j] && vaddr - g_phv[j] < g_phm[j])
            return vaddr - g_phv[j] + g_php[j];
    return vaddr;
}

static int seg_by_lma(const void *a, const void *b)
{
    const struct seg *x = a, *y = b;
    return x->lma < y->lma ? -1 : x->lma > y->lma;
}

static void load(const char *path)
{
    g_buf = slurp(path, &g_len);
    if (g_len < 52 || memcmp(g_buf, "\177ELF", 4) != 0)
        die("%s: not an ELF file", path);
    if ((g_buf[4] != 1 && g_buf[4] != 2) || (g_buf[5] != 1 && g_buf[5] != 2))
        die("%s: an ELF class or byte order this does not know", path);
    g_is64 = g_buf[4] == 2;
    g_be = g_buf[5] == 2;
    int w = g_is64 ? 8 : 4;
    if (rd(16, 2) != 2)
        die("%s: not a linked image (an object file? pack the output of "
            "the link)", path);
    g_machine = (unsigned)rd(18, 2);
    g_entry = rd(24, w);
    u64 phoff = rd(g_is64 ? 32 : 28, w);
    g_shoff = rd(g_is64 ? 40 : 32, w);
    unsigned phentsize = (unsigned)rd(g_is64 ? 54 : 42, 2);
    unsigned phnum = (unsigned)rd(g_is64 ? 56 : 44, 2);
    g_shentsize = (unsigned)rd(g_is64 ? 58 : 46, 2);
    g_shnum = (unsigned)rd(g_is64 ? 60 : 48, 2);
    g_shstrndx = (unsigned)rd(g_is64 ? 62 : 50, 2);
    if (g_shoff == 0 || g_shnum == 0)
        die("%s: no section headers (a stripped image cannot be packed "
            "here)", path);
    g_phv = calloc(phnum + 1, sizeof *g_phv);
    g_php = calloc(phnum + 1, sizeof *g_php);
    g_phm = calloc(phnum + 1, sizeof *g_phm);
    if (!g_phv || !g_php || !g_phm)
        die("%s", "out of memory");
    for (unsigned k = 0; k < phnum; k++) {
        size_t o = (size_t)phoff + (size_t)k * phentsize;
        if (rd(o, 4) != 1)              /* PT_LOAD */
            continue;
        g_phv[g_nph] = rd(o + (g_is64 ? 16 : 8), w);
        g_php[g_nph] = rd(o + (g_is64 ? 24 : 12), w);
        g_phm[g_nph] = rd(o + (g_is64 ? 40 : 20), w);
        g_nph++;
    }
    size_t shs = (size_t)g_shoff + (size_t)g_shstrndx * g_shentsize;
    u64 shstr = rd(shs + (g_is64 ? 24 : 16), w);
    g_seg = calloc(g_shnum, sizeof *g_seg);
    if (!g_seg)
        die("%s", "out of memory");
    for (unsigned k = 0; k < g_shnum; k++) {
        size_t o = (size_t)g_shoff + (size_t)k * g_shentsize;
        unsigned type = (unsigned)rd(o + 4, 4);
        u64 flags = rd(o + 8, w);
        u64 addr = rd(o + (g_is64 ? 16 : 12), w);
        u64 off = rd(o + (g_is64 ? 24 : 16), w);
        u64 size = rd(o + (g_is64 ? 32 : 20), w);
        /* allocated, with contents in the file (not .bss, not NOBITS) */
        if (!(flags & 2) || type == 8 || type == 0 || size == 0)
            continue;
        if (off + size > g_len)
            die("%s: a section runs past the end of the file", path);
        struct seg *s = &g_seg[g_nseg++];
        s->name = strat((size_t)(shstr + rd(o, 4)));
        s->lma = to_lma(addr);
        s->size = size;
        s->off = (size_t)off;
    }
    if (g_nseg == 0)
        die("%s: no section with contents to pack", path);
    qsort(g_seg, (size_t)g_nseg, sizeof *g_seg, seg_by_lma);
    for (int k = 1; k < g_nseg; k++)
        if (g_seg[k].lma < g_seg[k - 1].lma + g_seg[k - 1].size)
            die("%s: two sections' load addresses overlap", g_seg[k].name);
}

static int sym_find(const char *want, u64 *lma)
{
    int w = g_is64 ? 8 : 4;
    for (unsigned k = 0; k < g_shnum; k++) {
        size_t o = (size_t)g_shoff + (size_t)k * g_shentsize;
        if (rd(o + 4, 4) != 2)          /* SHT_SYMTAB */
            continue;
        u64 off = rd(o + (g_is64 ? 24 : 16), w);
        u64 size = rd(o + (g_is64 ? 32 : 20), w);
        unsigned link = (unsigned)rd(o + (g_is64 ? 40 : 24), 4);
        u64 ent = rd(o + (g_is64 ? 56 : 36), w);
        size_t lo = (size_t)g_shoff + (size_t)link * g_shentsize;
        u64 stroff = rd(lo + (g_is64 ? 24 : 16), w);
        if (!ent)
            ent = (u64)(g_is64 ? 24 : 16);
        for (u64 i = 1; i < size / ent; i++) {
            size_t so = (size_t)(off + i * ent);
            unsigned name = (unsigned)rd(so, 4);
            unsigned shndx = (unsigned)rd(so + (g_is64 ? 6 : 14), 2);
            u64 value = rd(so + (g_is64 ? 8 : 4), w);
            if (shndx == 0 || strcmp(strat((size_t)(stroff + name)), want))
                continue;
            *lma = to_lma(value);
            return 1;
        }
    }
    return 0;
}

/* ---- the stored bytes as one block ------------------------------------ */

static unsigned char *g_img;    /* lowest load address to highest */
static u64 g_base, g_size;

static void flatten(int fill, u64 pad_to)
{
    g_base = g_seg[0].lma;
    u64 end = g_seg[g_nseg - 1].lma + g_seg[g_nseg - 1].size;
    g_size = end - g_base;
    if (pad_to) {
        if (pad_to < g_size)
            die("--pad-to: the image is already larger than that (%s)",
                "it does not shrink");
        g_size = pad_to;
    }
    if (g_size > (1ull << 30))
        die("the stored bytes span more than 1 GiB -- two regions far "
            "apart (flash and RAM?); a raw image of that would be mostly "
            "fill. Use hex, srec or uf2, which write only %s", "the data");
    g_img = malloc((size_t)g_size + 4);
    if (!g_img)
        die("%s", "out of memory");
    memset(g_img, fill, (size_t)g_size);
    for (int k = 0; k < g_nseg; k++)
        memcpy(g_img + (g_seg[k].lma - g_base), g_buf + g_seg[k].off,
               (size_t)g_seg[k].size);
}

/* is offset o of the flat image covered by a section (vs. fill)? */
static int stored(u64 o)
{
    for (int k = 0; k < g_nseg; k++)
        if (g_base + o >= g_seg[k].lma &&
            g_base + o - g_seg[k].lma < g_seg[k].size)
            return 1;
    return 0;
}

/* ---- CRC-32 and SHA-256 ---------------------------------------------- */

static u32 crc32_update(u32 c, const unsigned char *p, size_t n)
{
    c = ~c;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
    }
    return ~c;
}

struct sha { u32 h[8]; unsigned char b[64]; u64 len; int n; };
static const u32 K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(struct sha *s)
{
    u32 w[64], a, b, c, d, e, f, g, h;
    for (int k = 0; k < 16; k++)
        w[k] = (u32)s->b[4 * k] << 24 | (u32)s->b[4 * k + 1] << 16 |
               (u32)s->b[4 * k + 2] << 8 | s->b[4 * k + 3];
    for (int k = 16; k < 64; k++) {
        u32 s0 = ROR(w[k - 15], 7) ^ ROR(w[k - 15], 18) ^ (w[k - 15] >> 3);
        u32 s1 = ROR(w[k - 2], 17) ^ ROR(w[k - 2], 19) ^ (w[k - 2] >> 10);
        w[k] = w[k - 16] + s0 + w[k - 7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3];
    e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (int k = 0; k < 64; k++) {
        u32 t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) +
                 ((e & f) ^ (~e & g)) + K256[k] + w[k];
        u32 t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) +
                 ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha256(const unsigned char *p, size_t n, unsigned char out[32])
{
    static const u32 iv[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    struct sha s;
    memcpy(s.h, iv, sizeof iv);
    s.n = 0;
    s.len = (u64)n * 8;
    while (n--) {
        s.b[s.n++] = *p++;
        if (s.n == 64) {
            sha_block(&s);
            s.n = 0;
        }
    }
    s.b[s.n++] = 0x80;
    if (s.n > 56) {
        while (s.n < 64)
            s.b[s.n++] = 0;
        sha_block(&s);
        s.n = 0;
    }
    while (s.n < 56)
        s.b[s.n++] = 0;
    for (int k = 0; k < 8; k++)
        s.b[56 + k] = (unsigned char)(s.len >> (56 - 8 * k));
    sha_block(&s);
    for (int k = 0; k < 8; k++)
        wr(out + 4 * k, s.h[k], 4, 1);
}

/* ---- the formats ------------------------------------------------------ */

static FILE *g_out;

static void out_bin(void)
{
    if (fwrite(g_img, 1, (size_t)g_size, g_out) != (size_t)g_size)
        die("%s", "write error");
}

/* Intel HEX: data records of up to 16 bytes, an extended linear address
 * record (type 04) whenever the upper 16 bits change, the start linear
 * address (type 05) for a 32-bit entry, and the end record. Only stored
 * bytes are written; the gaps stay absent rather than filled. */
static void hex_rec(int type, unsigned addr, const unsigned char *p, int n)
{
    unsigned sum = (unsigned)n + (addr >> 8) + (addr & 0xff) + (unsigned)type;
    fprintf(g_out, ":%02X%04X%02X", (unsigned)n, addr & 0xffff, (unsigned)type);
    for (int k = 0; k < n; k++) {
        fprintf(g_out, "%02X", p[k]);
        sum += p[k];
    }
    fprintf(g_out, "%02X\n", (0x100 - (sum & 0xff)) & 0xff);
}

static void out_hex(void)
{
    unsigned upper = 0xffffffffu;
    if (g_base + g_size > (1ull << 32))
        die("%s: Intel HEX reaches 4 GiB; this image's load addresses do "
            "not fit", "hex");
    for (int k = 0; k < g_nseg; k++) {
        u64 a = g_seg[k].lma, end = a + g_seg[k].size;
        const unsigned char *p = g_buf + g_seg[k].off;
        while (a < end) {
            if ((unsigned)(a >> 16) != upper) {
                unsigned char u[2];
                upper = (unsigned)(a >> 16);
                u[0] = (unsigned char)(upper >> 8);
                u[1] = (unsigned char)upper;
                hex_rec(4, 0, u, 2);
            }
            /* a record stays within one 64 KiB page */
            u64 n = end - a, room = 0x10000 - (a & 0xffff);
            if (n > 16) n = 16;
            if (n > room) n = room;
            hex_rec(0, (unsigned)(a & 0xffff), p, (int)n);
            p += n;
            a += n;
        }
    }
    {
        unsigned char e[4];
        wr(e, g_entry & 0xffffffffu, 4, 1);
        hex_rec(5, 0, e, 4);
    }
    fprintf(g_out, ":00000001FF\n");
}

/* Motorola S-records: an S0 header, S3 data records with 32-bit addresses
 * (16 bytes each), and the S7 termination carrying the entry point. */
static void srec_rec(int type, u64 addr, int alen, const unsigned char *p,
                     int n)
{
    unsigned count = (unsigned)(alen + n + 1), sum = count;
    fprintf(g_out, "S%d%02X", type, count);
    for (int k = alen - 1; k >= 0; k--) {
        unsigned b = (unsigned)(addr >> (8 * k)) & 0xff;
        fprintf(g_out, "%02X", b);
        sum += b;
    }
    for (int k = 0; k < n; k++) {
        fprintf(g_out, "%02X", p[k]);
        sum += p[k];
    }
    fprintf(g_out, "%02X\n", (~sum) & 0xff);
}

static void out_srec(const char *name)
{
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    srec_rec(0, 0, 2, (const unsigned char *)base, (int)strlen(base));
    for (int k = 0; k < g_nseg; k++) {
        u64 a = g_seg[k].lma, end = a + g_seg[k].size;
        const unsigned char *p = g_buf + g_seg[k].off;
        while (a < end) {
            u64 n = end - a > 16 ? 16 : end - a;
            srec_rec(3, a, 4, p, (int)n);
            p += n;
            a += n;
        }
    }
    srec_rec(7, g_entry & 0xffffffffu, 4, NULL, 0);
}

/* UF2 (github.com/microsoft/uf2): 512-byte blocks, each carrying 256
 * bytes for one target address; magic numbers at both ends; flag 0x2000
 * says the family ID field is present. Only 256-byte pages holding stored
 * bytes are written, so a bootloader never erases a page the image does
 * not touch. */
static void out_uf2(u32 family, int fill)
{
    u64 lo = g_base & ~(u64)255, hi = (g_base + g_size + 255) & ~(u64)255;
    u32 nblocks = 0, seq = 0;
    for (u64 a = lo; a < hi; a += 256) {
        int any = 0;
        for (u64 o = a; o < a + 256 && !any; o++)
            any = o >= g_base && o < g_base + g_size && stored(o - g_base);
        nblocks += (u32)any;
    }
    for (u64 a = lo; a < hi; a += 256) {
        unsigned char b[512];
        int any = 0;
        for (u64 o = a; o < a + 256 && !any; o++)
            any = o >= g_base && o < g_base + g_size && stored(o - g_base);
        if (!any)
            continue;
        memset(b, 0, sizeof b);
        wr(b + 0, 0x0A324655u, 4, 0);
        wr(b + 4, 0x9E5D5157u, 4, 0);
        wr(b + 8, family ? 0x00002000u : 0u, 4, 0);
        wr(b + 12, a & 0xffffffffu, 4, 0);
        wr(b + 16, 256, 4, 0);
        wr(b + 20, seq++, 4, 0);
        wr(b + 24, nblocks, 4, 0);
        wr(b + 28, family, 4, 0);
        for (int k = 0; k < 256; k++) {
            u64 o = a + (u64)k;
            b[32 + k] = o >= g_base && o < g_base + g_size
                      ? g_img[o - g_base] : (unsigned char)fill;
        }
        wr(b + 508, 0x0AB16F30u, 4, 0);
        if (fwrite(b, 1, 512, g_out) != 512)
            die("%s", "write error");
    }
}

/* The UF2 family names the bootloaders know, a few of many. */
static const struct { const char *name; u32 id; } families[] = {
    { "rp2040", 0xe48bff56u }, { "rp2350-arm-s", 0xe48bff59u },
    { "samd21", 0x68ed2b88u }, { "samd51", 0x55114460u },
    { "nrf52840", 0xada52840u }, { "stm32f4", 0x57755a57u },
    { "esp32s3", 0xc47e5767u }, { "esp32s2", 0xbfdd4eeeu },
};

static u32 parse_family(const char *s)
{
    for (size_t k = 0; k < sizeof families / sizeof families[0]; k++)
        if (strcmp(s, families[k].name) == 0)
            return families[k].id;
    char *e;
    unsigned long v = strtoul(s, &e, 0);
    if (e == s || *e)
        die("--uf2-family %s: a number (0xe48bff56) or a known name "
            "(rp2040, rp2350-arm-s, samd21, samd51, nrf52840, stm32f4, "
            "esp32s2, esp32s3)", s);
    return (u32)v;
}

/* ---- main ------------------------------------------------------------- */

static int parse_size(const char *p, u64 *out)
{
    char *e;
    u64 v = strtoull(p, &e, 0);
    if (e == p)
        return 0;
    if (*e == 'K' || *e == 'k') { v <<= 10; e++; }
    else if (*e == 'M' || *e == 'm') { v <<= 20; e++; }
    if (*e)
        return 0;
    *out = v;
    return 1;
}

static void usage(void)
{
    fprintf(stderr,
        "usage: embpack IMAGE -o OUT [--format bin|hex|srec|uf2] "
        "[--fill BYTE]\n"
        "               [--pad-to SIZE] [--crc32 SYMBOL] [--append-crc32]\n"
        "               [--uf2-family ID] [--manifest FILE.json]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    const char *image = NULL, *out = NULL, *fmt = NULL, *crcsym = NULL,
               *manifest = NULL;
    int fill = 0, append_crc = 0;
    u64 pad_to = 0;
    u32 family = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        u64 v;
        if (strcmp(a, "-o") == 0 && i + 1 < argc) {
            out = argv[++i];
        } else if (strcmp(a, "--format") == 0 && i + 1 < argc) {
            fmt = argv[++i];
        } else if (strcmp(a, "--fill") == 0 && i + 1 < argc) {
            if (!parse_size(argv[++i], &v) || v > 255)
                die("--fill %s: a byte value, 0 to 0xff", argv[i]);
            fill = (int)v;
        } else if (strcmp(a, "--pad-to") == 0 && i + 1 < argc) {
            if (!parse_size(argv[++i], &pad_to) || !pad_to)
                die("--pad-to %s: a size (e.g. 64K)", argv[i]);
        } else if (strcmp(a, "--crc32") == 0 && i + 1 < argc) {
            crcsym = argv[++i];
        } else if (strcmp(a, "--append-crc32") == 0) {
            append_crc = 1;
        } else if (strcmp(a, "--uf2-family") == 0 && i + 1 < argc) {
            family = parse_family(argv[++i]);
        } else if (strcmp(a, "--manifest") == 0 && i + 1 < argc) {
            manifest = argv[++i];
        } else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage();
        } else if (a[0] == '-') {
            die("unknown option %s (embpack --help)", a);
        } else if (!image) {
            image = a;
        } else {
            die("one image at a time (%s)", a);
        }
    }
    if (!image || !out)
        usage();
    if (!fmt) {
        const char *dot = strrchr(out, '.');
        fmt = !dot ? "bin"
            : !strcmp(dot, ".hex") || !strcmp(dot, ".ihex") ? "hex"
            : !strcmp(dot, ".srec") || !strcmp(dot, ".s19") ||
              !strcmp(dot, ".s37") || !strcmp(dot, ".mot") ? "srec"
            : !strcmp(dot, ".uf2") ? "uf2" : "bin";
    }
    if (strcmp(fmt, "bin") && strcmp(fmt, "hex") && strcmp(fmt, "srec") &&
        strcmp(fmt, "uf2"))
        die("--format %s: bin, hex, srec or uf2", fmt);
    if (append_crc && strcmp(fmt, "bin"))
        die("--append-crc32 is for a raw binary (%s): it adds four bytes "
            "after the image, which the other formats would need an "
            "address for", fmt);
    if (pad_to && strcmp(fmt, "bin") && strcmp(fmt, "uf2"))
        die("--pad-to is for bin and uf2 (%s writes only stored bytes)",
            fmt);

    load(image);
    flatten(fill, pad_to);

    /* --crc32: the CRC of every stored byte but the symbol's own four,
     * into those four, in the image's byte order */
    if (crcsym) {
        u64 at;
        if (!sym_find(crcsym, &at))
            die("--crc32 %s: no such symbol in the image", crcsym);
        if (at < g_base || at + 4 > g_base + g_size || !stored(at - g_base) ||
            !stored(at - g_base + 3))
            die("--crc32 %s: the symbol is not in a stored section (put it "
                "in flash: a const, not .bss)", crcsym);
        u64 o = at - g_base;
        u32 c = crc32_update(0, g_img, (size_t)o);
        c = crc32_update(c, g_img + o + 4, (size_t)(g_size - o - 4));
        wr(g_img + o, c, 4, g_be);
        for (int k = 0; k < g_nseg; k++)
            if (at >= g_seg[k].lma && at - g_seg[k].lma < g_seg[k].size)
                wr(g_buf + g_seg[k].off + (at - g_seg[k].lma), c, 4, g_be);
    }
    if (append_crc) {
        u32 c = crc32_update(0, g_img, (size_t)g_size);
        wr(g_img + g_size, c, 4, 0);
        g_size += 4;
    }

    g_out = fopen(out, strcmp(fmt, "bin") && strcmp(fmt, "uf2") ? "w" : "wb");
    if (!g_out)
        die("cannot write %s", out);
    if (!strcmp(fmt, "bin"))
        out_bin();
    else if (!strcmp(fmt, "hex"))
        out_hex();
    else if (!strcmp(fmt, "srec"))
        out_srec(out);
    else
        out_uf2(family, fill);
    if (fclose(g_out) != 0)
        die("write error on %s", out);

    if (manifest) {
        FILE *m = fopen(manifest, "w");
        unsigned char d[32];
        if (!m)
            die("cannot write %s", manifest);
        sha256(g_img, (size_t)g_size, d);
        fprintf(m, "{\n  \"image\": \"%s\", \"format\": \"%s\", "
                "\"output\": \"%s\",\n", image, fmt, out);
        fprintf(m, "  \"machine\": %u, \"byte_order\": \"%s\", "
                "\"entry\": %llu,\n", g_machine, g_be ? "big" : "little",
                g_entry);
        fprintf(m, "  \"base\": %llu, \"size\": %llu, \"fill\": %d,\n",
                g_base, g_size, fill);
        fprintf(m, "  \"crc32\": %u,\n  \"sha256\": \"",
                crc32_update(0, g_img, (size_t)g_size));
        for (int k = 0; k < 32; k++)
            fprintf(m, "%02x", d[k]);
        fprintf(m, "\",\n  \"sections\": [");
        for (int k = 0; k < g_nseg; k++)
            fprintf(m, "%s\n    {\"name\": \"%s\", \"load\": %llu, "
                    "\"size\": %llu}", k ? "," : "", g_seg[k].name,
                    g_seg[k].lma, g_seg[k].size);
        fprintf(m, "\n  ]\n}\n");
        if (fclose(m) != 0)
            die("write error on %s", manifest);
    }
    return 0;
}
