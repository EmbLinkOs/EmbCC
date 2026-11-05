/* libgcov: what a program built with --coverage or -fprofile-generate
 * links, to write its counters out as GCC 16's .gcda files when it
 * exits (or when it calls __gcov_dump).
 *
 * Each instrumented unit's constructor hands this its record (src/ir/
 * gcov.c describes it): the unit's counters, the .gcda's path, and per
 * function the ident and checksums the .gcno has. At exit every unit's
 * file is written -- added to the counts already there when the file is
 * of the same program (src/ir/gcda.h), as GCC's is, so runs accumulate
 * until the file is removed. GCOV_PREFIX and GCOV_PREFIX_STRIP move the
 * files, as with GCC: the first N directories of the path are dropped
 * and GCOV_PREFIX put in front.
 *
 * Where the C library cannot open the file -- a board, whose library has
 * no file system -- the file is printed on the console instead, as hex
 * between two marker lines:
 *
 *   @gcda /abs/path/x.gcda 412
 *   6164636742363226...
 *   @end
 *
 * and `embcc --gcov-extract LOG` on the host writes (merges) the files.
 * On AVR, whose library has no files, that is the only way, and the file
 * is streamed rather than built in RAM. The console is putchar, through
 * __embcc_gcov_putc, which a program may replace (a UART, semihosting);
 * __embcc_gcov_output, the whole of the output, may be replaced too.
 *
 * Nothing here includes a header: the library under it may be EmbCC's,
 * glibc, libSystem or msvcrt, and the handful of functions used are the
 * same in all of them. */

#define NOI __attribute__((no_instrument_function, \
                           no_profile_instrument_function))
#define WEAK __attribute__((weak))

typedef __UINT32_TYPE__ u32;
typedef unsigned long long u64;
typedef __SIZE_TYPE__ gsize;

struct gcov_fn { u32 ident, lineno, cfg, n, base; };
struct gcov_info {
    u64 *ctrs;
    struct gcov_info *next;
    u32 magic, version, stamp, checksum, nfuncs, name_off, funcs_off, nctrs;
};

#define INFO_MAGIC 0x45474356u
#define TAG_FUNCTION 0x01000000u
#define TAG_COUNTER_ARCS 0x01a10000u
#define TAG_OBJECT_SUMMARY 0xa1000000u
#define GCDA_MAGIC_W 0x67636461u

int atexit(void (*)(void));
int putchar(int);

static struct gcov_info *g_units;
static int g_hooked, g_dumped;

static NOI const struct gcov_fn *fn_of(const struct gcov_info *gi, u32 k)
{
    return (const struct gcov_fn *)((const char *)gi + gi->funcs_off) + k;
}

static NOI const char *path_of(const struct gcov_info *gi)
{
    return (const char *)gi + gi->name_off;
}

/* GCC 12's format and later (version "B2..*" on): lengths in bytes and a
 * checksum word in the header; before, lengths in words and none */
static NOI int bytes_format(u32 v)
{
    int c0 = (int)(v >> 24) & 0xff;
    return c0 > 'B' || (c0 == 'B' && (int)((v >> 16) & 0xff) >= '2');
}

/* the file's size, from the record */
static NOI unsigned long image_size(const struct gcov_info *gi)
{
    unsigned long n = (bytes_format(gi->version) ? 16 : 12) + 16 + 4;
    for (u32 k = 0; k < gi->nfuncs; k++)
        n += 8 + 12 + 8 + 8ul * fn_of(gi, k)->n;
    return n;
}

/* The file, one word at a time, to `put` -- a buffer or the console. In
 * the target's byte order: gcov reads either, by the magic. */
struct out {
    unsigned char *buf;         /* the image, or NULL: the console */
    unsigned long at;
};

WEAK NOI int __embcc_gcov_putc(int c) { return putchar(c); }

static NOI void hex_byte(unsigned char b)
{
    static const char d[] = "0123456789abcdef";
    __embcc_gcov_putc(d[b >> 4]);
    __embcc_gcov_putc(d[b & 15]);
}

static NOI void put_word(struct out *o, u32 v)
{
    unsigned char b[4];
    __builtin_memcpy(b, &v, 4);
    for (int k = 0; k < 4; k++) {
        if (o->buf) {
            o->buf[o->at] = b[k];
        } else {
            hex_byte(b[k]);
            if ((o->at + 1) % 32 == 0)
                __embcc_gcov_putc('\n');
        }
        o->at++;
    }
}

static NOI void emit(const struct gcov_info *gi, struct out *o)
{
    int bytes = bytes_format(gi->version);
    u32 unit = bytes ? 1 : 4;
    u64 max = 0;
    for (u32 k = 0; k < gi->nctrs; k++)
        if (gi->ctrs[k] > max)
            max = gi->ctrs[k];
    put_word(o, GCDA_MAGIC_W);
    put_word(o, gi->version);
    put_word(o, gi->stamp);
    if (bytes)
        put_word(o, gi->checksum);
    put_word(o, TAG_OBJECT_SUMMARY);
    put_word(o, 8 / unit);
    put_word(o, 1);                          /* runs */
    put_word(o, max > 0xffffffffu ? 0xffffffffu : (u32)max);
    for (u32 k = 0; k < gi->nfuncs; k++) {
        const struct gcov_fn *f = fn_of(gi, k);
        put_word(o, TAG_FUNCTION);
        put_word(o, 12 / unit);
        put_word(o, f->ident);
        put_word(o, f->lineno);
        put_word(o, f->cfg);
        put_word(o, TAG_COUNTER_ARCS);
        put_word(o, 8 * f->n / unit);
        for (u32 c = 0; c < f->n; c++) {
            u64 v = gi->ctrs[f->base + c];
            put_word(o, (u32)v);
            put_word(o, (u32)(v >> 32));
        }
    }
    put_word(o, 0);
}

static NOI void console_open(const char *path, unsigned long n)
{
    const char *m = "\n@gcda ";
    while (*m) __embcc_gcov_putc(*m++);
    while (*path) __embcc_gcov_putc(*path++);
    __embcc_gcov_putc(' ');
    char d[24];
    int k = 0;
    do { d[k++] = (char)('0' + n % 10); n /= 10; } while (n);
    while (k) __embcc_gcov_putc(d[--k]);
    __embcc_gcov_putc('\n');
}

static NOI void console_close(void)
{
    const char *m = "\n@end\n";
    while (*m) __embcc_gcov_putc(*m++);
}

#if !defined(__AVR__)

#include "../../src/ir/gcda.h"

typedef struct gcov_FILE GFILE;
GFILE *fopen(const char *, const char *);
gsize fread(void *, gsize, gsize, GFILE *);
gsize fwrite(const void *, gsize, gsize, GFILE *);
int fclose(GFILE *);
void *malloc(gsize);
void free(void *);
char *getenv(const char *);
#if defined(_WIN32)
int _mkdir(const char *);
#define MKDIR(p) _mkdir(p)
#elif defined(__linux__) || defined(__APPLE__) || defined(__unix__)
int mkdir(const char *, unsigned);
#define MKDIR(p) mkdir(p, 0777)
#else
#define MKDIR(p) (-1)
#endif

/* GCOV_PREFIX / GCOV_PREFIX_STRIP: the path the file is written to */
static NOI char *place(const char *path)
{
    const char *pre = getenv("GCOV_PREFIX"), *st = getenv("GCOV_PREFIX_STRIP");
    int strip = 0;
    for (const char *s = st; s && *s >= '0' && *s <= '9'; s++)
        strip = strip * 10 + (*s - '0');
    const char *p = path;
    for (int k = 0; k < strip && *p; k++) {
        const char *q = p + (*p == '/');
        while (*q && *q != '/')
            q++;
        if (!*q)
            break;
        p = q;
    }
    gsize lp = pre ? __builtin_strlen(pre) : 0, ln = __builtin_strlen(p);
    char *r = malloc(lp + ln + 2);
    if (!r)
        return 0;
    if (lp) __builtin_memcpy(r, pre, lp);
    if (lp && *p != '/') r[lp++] = '/';
    __builtin_memcpy(r + lp, p, ln + 1);
    return r;
}

/* the directories up to the file, as GCC makes them */
static NOI void make_dirs(char *path)
{
    for (char *q = path + 1; *q; q++)
        if (*q == '/') {
            *q = 0;
            (void)MKDIR(path);
            *q = '/';
        }
}

static NOI unsigned char *slurp(const char *path, unsigned long *n)
{
    GFILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    unsigned long cap = 4096, len = 0;
    unsigned char *b = malloc(cap);
    while (b) {
        gsize got = fread(b + len, 1, cap - len, f);
        len += got;
        if (len < cap)
            break;
        unsigned char *nb = malloc(cap * 2);
        if (nb) __builtin_memcpy(nb, b, len);
        free(b);
        b = nb;
        cap *= 2;
    }
    fclose(f);
    *n = len;
    return b;
}

/* The default output: the file merged with the one there, or the
 * console where there is no file system. A program may define its own. */
WEAK NOI int __embcc_gcov_output(const char *path, unsigned char *img,
                                 unsigned long n)
{
    char *where = place(path);
    const char *p = where ? where : path;
    unsigned long on = 0;
    unsigned char *old = slurp(p, &on);
    if (old) {
        (void)gcda_merge(img, n, old, on);
        free(old);
    }
    GFILE *f = fopen(p, "wb");
    if (!f && where) {
        make_dirs(where);
        f = fopen(p, "wb");
    }
    int ok = 0;
    if (f) {
        ok = fwrite(img, 1, n, f) == n;
        ok &= fclose(f) == 0;
    }
    if (!f) {
        struct out o = { 0, 0 };
        console_open(path, n);
        for (unsigned long k = 0; k < n; k++) {
            hex_byte(img[k]);
            if ((++o.at) % 32 == 0)
                __embcc_gcov_putc('\n');
        }
        console_close();
        ok = 1;
    }
    free(where);
    return ok ? 0 : -1;
}

static NOI void dump_one(struct gcov_info *gi)
{
    unsigned long n = image_size(gi);
    unsigned char *img = malloc(n);
    if (!img) {                      /* no heap: straight to the console */
        struct out o = { 0, 0 };
        console_open(path_of(gi), n);
        emit(gi, &o);
        console_close();
        return;
    }
    struct out o = { img, 0 };
    emit(gi, &o);
    (void)__embcc_gcov_output(path_of(gi), img, n);
    free(img);
}

#else  /* AVR: no file system, no heap to spare */

static NOI void dump_one(struct gcov_info *gi)
{
    struct out o = { 0, 0 };
    console_open(path_of(gi), image_size(gi));
    emit(gi, &o);
    console_close();
}

#endif

/* GCC's interface: __gcov_dump writes every unit's file now (and the one
 * at exit is then not written again, unless __gcov_reset is called);
 * __gcov_reset zeroes the counters. */
NOI void __gcov_dump(void)
{
    for (struct gcov_info *gi = g_units; gi; gi = gi->next)
        dump_one(gi);
    g_dumped = 1;
}

NOI void __gcov_reset(void)
{
    for (struct gcov_info *gi = g_units; gi; gi = gi->next)
        for (u32 k = 0; k < gi->nctrs; k++)
            gi->ctrs[k] = 0;
    g_dumped = 0;
}

NOI void __gcov_flush(void)
{
    __gcov_dump();
    __gcov_reset();
}

static NOI void at_exit(void)
{
    if (!g_dumped)
        __gcov_dump();
}

NOI void __embcc_gcov_init(void *p)
{
    struct gcov_info *gi = p;
    if (gi->magic != INFO_MAGIC)
        return;
    gi->next = g_units;
    g_units = gi;
    if (!g_hooked) {
        g_hooked = 1;
        atexit(at_exit);
    }
}
