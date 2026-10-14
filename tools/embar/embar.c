/* embar -- EmbCC's static-library archiver.
 *
 *     embar rcs libfoo.a a.o b.o ...   create or update, with a symbol index
 *     embar t   libfoo.a               list the members
 *     embar x   libfoo.a [a.o ...]     extract members
 *     embar d   libfoo.a a.o ...       delete members
 *     embar s   libfoo.a               (re)write the symbol index
 *
 * Called by a name ending in `ranlib` (embcc-ranlib, arm-none-eabi-ranlib)
 * it is ranlib: each argument is an archive whose index is rewritten. A
 * build written for binutils runs `ar qc` and then `ranlib`, and CMake
 * does exactly that.
 *
 * Building a C library needs an archiver, and the one every build used was
 * binutils' `ar`: a host with no GCC toolchain could compile EmbCC's
 * libraries with EmbCC and still not package them. This is that last
 * piece, in ISO C, so it builds wherever the compiler does
 * (docs/internals/porting.md).
 *
 * The format is the System V / GNU one embld and every other linker read:
 * "!<arch>\n", a "/" symbol index (for linkers that use it -- embld scans
 * members itself), a "//" table for names longer than fifteen characters,
 * then the members, each padded to an even size. Output is deterministic:
 * every time, uid and gid is 0 and every mode 644, so the same objects
 * always make the same bytes (what `ar D` asks binutils for).
 *
 * The index lists every global or weak symbol a member DEFINES (and
 * common ones), for ELF32 and ELF64 little-endian members -- every target
 * EmbCC compiles for. A member in another format is archived without
 * index entries.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct mem { char *name; unsigned char *data; long size; };

static struct mem *g_mem;
static int g_nmem, g_capmem;

static void die(const char *fmt, const char *a)
{
    fprintf(stderr, "embar: ");
    fprintf(stderr, fmt, a);
    fputc('\n', stderr);
    exit(1);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) die("%s", "out of memory");
    return p;
}

static unsigned char *read_file(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    long n;
    unsigned char *b;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0) { fclose(f); return NULL; }
    rewind(f);
    b = xmalloc((size_t)n + 1);
    if (n && fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    fclose(f);
    *len = n;
    return b;
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/'), *b = strrchr(p, '\\');
    if (b && (!s || b > s)) s = b;
    return s ? s + 1 : p;
}

static void add_member(const char *name, unsigned char *data, long size, int replace)
{
    if (replace)
        for (int i = 0; i < g_nmem; i++)
            if (strcmp(g_mem[i].name, name) == 0) {
                g_mem[i].data = data;
                g_mem[i].size = size;
                return;
            }
    if (g_nmem == g_capmem) {
        g_capmem = g_capmem ? g_capmem * 2 : 64;
        g_mem = realloc(g_mem, (size_t)g_capmem * sizeof *g_mem);
        if (!g_mem) die("%s", "out of memory");
    }
    g_mem[g_nmem].name = xmalloc(strlen(name) + 1);
    strcpy(g_mem[g_nmem].name, name);
    g_mem[g_nmem].data = data;
    g_mem[g_nmem].size = size;
    g_nmem++;
}

static long dec(const char *p, int n)
{
    long v = 0;
    for (int i = 0; i < n && p[i] >= '0' && p[i] <= '9'; i++)
        v = v * 10 + (p[i] - '0');
    return v;
}

/* An existing archive's members, in order, for `r` to update. */
static void load_archive(const char *path)
{
    long len, off = 8;
    unsigned char *b = read_file(path, &len);
    const char *longnames = NULL;
    if (!b) return;
    if (len < 8 || memcmp(b, "!<arch>\n", 8) != 0)
        die("%s is not an archive", path);
    while (off + 60 <= len) {
        const char *h = (const char *)b + off;
        long size = dec(h + 48, 10), data = off + 60;
        char nm[256];
        int k = 0;
        if (h[58] != '`' || h[59] != '\n' || data + size > len)
            die("%s: a corrupt member header", path);
        if (h[0] == '/' && h[1] == '/')
            longnames = (const char *)b + data, nm[0] = 0;
        else if (h[0] == '/' && (h[1] == ' ' || h[1] == 0))
            nm[0] = 0;                               /* the old index */
        else if (h[0] == '/') {
            const char *s = longnames ? longnames + dec(h + 1, 15) : "?";
            while (s[k] && s[k] != '/' && s[k] != '\n' && k < 255) nm[k] = s[k], k++;
            nm[k] = 0;
        } else {
            while (k < 16 && h[k] != '/' && h[k] != ' ') nm[k] = h[k], k++;
            nm[k] = 0;
        }
        if (nm[0])
            add_member(nm, b + data, size, 0);
        off = data + size + (size & 1);
    }
}

/* ---- the symbol index ---- */

static unsigned long rd(const unsigned char *p, int n)
{
    unsigned long v = 0;
    for (int i = n - 1; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

struct sym { int member; const char *name; };
static struct sym *g_sym;
static int g_nsym, g_capsym;

static void index_member(int mi)
{
    const unsigned char *b = g_mem[mi].data;
    long len = g_mem[mi].size;
    if (len < 52 || memcmp(b, "\177ELF", 4) != 0 || b[5] != 1)
        return;                       /* not little-endian ELF: no entries */
    int c64 = b[4] == 2;
    unsigned long shoff = c64 ? rd(b + 0x28, 8) : rd(b + 0x20, 4);
    unsigned long shent = rd(b + (c64 ? 0x3a : 0x2e), 2);
    unsigned long shnum = rd(b + (c64 ? 0x3c : 0x30), 2);
    for (unsigned long i = 0; i < shnum; i++) {
        const unsigned char *sh = b + shoff + i * shent;
        if ((long)(shoff + (i + 1) * shent) > len || rd(sh + 4, 4) != 2)
            continue;                                     /* SHT_SYMTAB */
        unsigned long off = c64 ? rd(sh + 0x18, 8) : rd(sh + 0x10, 4);
        unsigned long size = c64 ? rd(sh + 0x20, 8) : rd(sh + 0x14, 4);
        unsigned long link = rd(sh + (c64 ? 0x28 : 0x18), 4);
        unsigned long ent = c64 ? 24 : 16;
        const unsigned char *str = b + shoff + link * shent;
        unsigned long stroff = c64 ? rd(str + 0x18, 8) : rd(str + 0x10, 4);
        for (unsigned long s = 1; s * ent < size; s++) {
            const unsigned char *y = b + off + s * ent;
            unsigned info = y[c64 ? 4 : 12];
            unsigned shndx = (unsigned)rd(y + (c64 ? 6 : 14), 2);
            unsigned bind = info >> 4;
            if ((bind != 1 && bind != 2) || shndx == 0)   /* GLOBAL/WEAK, defined */
                continue;
            if (g_nsym == g_capsym) {
                g_capsym = g_capsym ? g_capsym * 2 : 256;
                g_sym = realloc(g_sym, (size_t)g_capsym * sizeof *g_sym);
                if (!g_sym) die("%s", "out of memory");
            }
            g_sym[g_nsym].member = mi;
            g_sym[g_nsym].name = (const char *)b + stroff + rd(y, 4);
            g_nsym++;
        }
    }
}

static void put_hdr(FILE *f, const char *name, long size)
{
    char h[61];
    snprintf(h, sizeof h, "%-16s%-12s%-6s%-6s%-8s%-10ld`\n", name, "0", "0",
             "0", "644", size);
    fwrite(h, 1, 60, f);
}

static void be32(FILE *f, unsigned long v)
{
    fputc((int)(v >> 24) & 255, f); fputc((int)(v >> 16) & 255, f);
    fputc((int)(v >> 8) & 255, f);  fputc((int)v & 255, f);
}

static void write_archive(const char *path, int want_index)
{
    long longsz = 0, *lname = xmalloc((size_t)(g_nmem + 1) * sizeof *lname);
    long *at = xmalloc((size_t)(g_nmem + 1) * sizeof *at);
    long idxsz = 0, names = 0, off;
    FILE *f;
    for (int i = 0; i < g_nmem; i++) {
        lname[i] = -1;
        if (strlen(g_mem[i].name) > 15) {
            lname[i] = longsz;
            longsz += (long)strlen(g_mem[i].name) + 2;    /* "name/\n" */
        }
    }
    g_nsym = 0;
    if (want_index)
        for (int i = 0; i < g_nmem; i++)
            index_member(i);
    for (int s = 0; s < g_nsym; s++)
        names += (long)strlen(g_sym[s].name) + 1;
    if (g_nsym)
        idxsz = 4 + 4L * g_nsym + names;
    /* where each member's header lands */
    off = 8;
    if (g_nsym) off += 60 + idxsz + (idxsz & 1);
    if (longsz) off += 60 + longsz + (longsz & 1);
    for (int i = 0; i < g_nmem; i++) {
        at[i] = off;
        off += 60 + g_mem[i].size + (g_mem[i].size & 1);
    }
    f = fopen(path, "wb");
    if (!f) die("cannot write %s", path);
    fwrite("!<arch>\n", 1, 8, f);
    if (g_nsym) {
        put_hdr(f, "/", idxsz);
        be32(f, (unsigned long)g_nsym);
        for (int s = 0; s < g_nsym; s++) be32(f, (unsigned long)at[g_sym[s].member]);
        for (int s = 0; s < g_nsym; s++) fwrite(g_sym[s].name, 1, strlen(g_sym[s].name) + 1, f);
        if (idxsz & 1) fputc('\n', f);
    }
    if (longsz) {
        put_hdr(f, "//", longsz);
        for (int i = 0; i < g_nmem; i++)
            if (lname[i] >= 0) fprintf(f, "%s/\n", g_mem[i].name);
        if (longsz & 1) fputc('\n', f);
    }
    for (int i = 0; i < g_nmem; i++) {
        char nm[32];
        if (lname[i] >= 0) snprintf(nm, sizeof nm, "/%ld", lname[i]);
        else               snprintf(nm, sizeof nm, "%s/", g_mem[i].name);
        put_hdr(f, nm, g_mem[i].size);
        if (g_mem[i].size) fwrite(g_mem[i].data, 1, (size_t)g_mem[i].size, f);
        if (g_mem[i].size & 1) fputc('\n', f);
    }
    if (fclose(f) != 0) die("cannot write %s", path);
}

static void usage(void)
{
    fprintf(stderr,
        "usage: embar [-]{r|q}[cs] ARCHIVE FILE...   create or update\n"
        "       embar [-]t ARCHIVE                  list the members\n"
        "       embar [-]x ARCHIVE [MEMBER...]      extract (all, or those)\n"
        "       embar [-]d ARCHIVE MEMBER...        delete members\n"
        "       embar [-]s ARCHIVE                  rewrite the symbol index\n"
        "  (as ranlib: embar-ranlib ARCHIVE..., the index of each)\n"
        "  r  insert FILEs, replacing members of the same name\n"
        "  q  append FILEs\n"
        "  c  do not say that the archive was created\n"
        "  s  write a symbol index (the default; S suppresses it)\n"
        "  t  list the members\n"
        "Output is deterministic: times, owners and modes are fixed.\n");
    exit(2);
}

/* An archive the operation reads must be there: only r and q create. */
static void must_exist(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        die("%s: no such archive", path);
    fclose(f);
}

int main(int argc, char **argv)
{
    const char *ops, *arch;
    int op = 0, index = 1, quiet = 0;
    size_t n0 = strlen(base_name(argv[0]));
    if (n0 >= 6 && strcmp(base_name(argv[0]) + n0 - 6, "ranlib") == 0) {
        /* ranlib ARCHIVE...: each archive's index, written again */
        int k = 1;
        while (k < argc && argv[k][0] == '-' && argv[k][1])
            k++;                       /* -D, -t, -U: nothing to do here */
        if (k >= argc) {
            fprintf(stderr, "usage: %s ARCHIVE...\n", base_name(argv[0]));
            exit(2);
        }
        for (; k < argc; k++) {
            g_nmem = 0;
            must_exist(argv[k]);
            load_archive(argv[k]);
            write_archive(argv[k], 1);
        }
        return 0;
    }
    if (argc < 3) usage();
    ops = argv[1][0] == '-' ? argv[1] + 1 : argv[1];
    for (const char *p = ops; *p; p++)
        switch (*p) {
        case 'r': case 'q': case 't': case 'x': case 'd': op = *p; break;
        case 'c': quiet = 1; break;
        case 's': index = 1; if (!op) op = 's'; break;
        case 'S': index = 0; break;
        case 'D': case 'u': case 'v': case 'o': break;  /* deterministic already */
        default: usage();
        }
    if (op == 's' && strpbrk(ops, "rqtxd"))
        for (const char *p = ops; *p; p++)
            if (strchr("rqtxd", *p))
                op = *p;
    if (!op) usage();
    arch = argv[2];
    if (op != 'r' && op != 'q')
        must_exist(arch);
    load_archive(arch);
    if (op == 't') {
        for (int i = 0; i < g_nmem; i++) printf("%s\n", g_mem[i].name);
        return 0;
    }
    if (op == 's') {
        write_archive(arch, 1);
        return 0;
    }
    if (op == 'x') {
        int found = 0;
        for (int i = 0; i < g_nmem; i++) {
            int want = argc == 3;
            for (int k = 3; k < argc; k++)
                if (strcmp(base_name(argv[k]), g_mem[i].name) == 0)
                    want = 1;
            if (!want)
                continue;
            FILE *f = fopen(g_mem[i].name, "wb");
            if (!f || fwrite(g_mem[i].data, 1, (size_t)g_mem[i].size, f) !=
                          (size_t)g_mem[i].size || fclose(f) != 0)
                die("cannot write %s", g_mem[i].name);
            found++;
        }
        if (argc > 3 && found < argc - 3)
            die("%s: a member named there is not in the archive", arch);
        return 0;
    }
    if (op == 'd') {
        for (int k = 3; k < argc; k++) {
            int j = 0, gone = 0;
            for (int i = 0; i < g_nmem; i++) {
                if (!gone && strcmp(g_mem[i].name, base_name(argv[k])) == 0) {
                    gone = 1;
                    continue;
                }
                g_mem[j++] = g_mem[i];
            }
            if (!gone)
                die("no member %s", argv[k]);
            g_nmem = j;
        }
        write_archive(arch, index);
        return 0;
    }
    if (!g_nmem && !quiet && argc > 3)
        fprintf(stderr, "embar: creating %s\n", arch);
    for (int i = 3; i < argc; i++) {
        long len;
        unsigned char *b = read_file(argv[i], &len);
        if (!b) die("cannot read %s", argv[i]);
        add_member(base_name(argv[i]), b, len, op == 'r');
    }
    write_archive(arch, index);
    return 0;
}
