/* libmsan: MemorySanitizer's runtime, for what src/ir/sanitize.c emits.
 *
 * The shadow is a byte per byte of memory, its uninitialized bits, kept
 * sparse -- a table of the 1 GiB regions, each a table of its 4 KiB
 * pages -- so memory nobody poisoned has no shadow and reads as
 * initialized. A function's locals are poisoned when it is entered (and
 * made good when it returns), a malloc'd block when it is made; a store
 * writes its value's shadow, a load reads it; arguments' and results'
 * shadows travel in thread-local slots beside the call.
 *
 * The C library is not instrumented, so what it writes into poisoned
 * memory would stay poisoned: the functions here are the library's, the
 * memory they write made good (read, fread, fgets, the printf and scanf
 * families, stat, time and the rest), and the shadow copied where they
 * copy (memcpy, strcpy, strdup). MSAN_OPTIONS: exitcode (77),
 * halt_on_error. */

#include "san.h"
#include <stdarg.h>

static int g_exitcode = 77, g_halt = 1, g_reports;

/* ---- the shadow ---------------------------------------------------- */

static unsigned char **g_l1[1u << 17];

static NOS void *raw_page(usize n)
{
    void *p = mmap(0, n, SAN_PROT_RW, SAN_MAP_ANON_NORESERVE, -1, 0);
    if (p == (void *)-1 || !p) {
        san_puts("==MSan== out of memory for the shadow\n");
        san_die(1);
    }
    return p;
}

/* the shadow of a's page, made when `make` */
static NOS unsigned char *page(uptr a, int make)
{
    uptr i1 = (a >> 30) & ((1u << 17) - 1);
    unsigned char **l2 = __atomic_load_n(&g_l1[i1], __ATOMIC_ACQUIRE);
    if (!l2) {
        if (!make)
            return 0;
        unsigned char **n = raw_page(sizeof(unsigned char *) << 18);
        unsigned char **exp = 0;
        if (__atomic_compare_exchange_n(&g_l1[i1], &exp, n, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            l2 = n;
        else
            l2 = exp;
    }
    uptr i2 = (a >> 12) & ((1u << 18) - 1);
    unsigned char *pg = __atomic_load_n(&l2[i2], __ATOMIC_ACQUIRE);
    if (!pg && make) {
        unsigned char *n = raw_page(4096);
        unsigned char *exp = 0;
        if (__atomic_compare_exchange_n(&l2[i2], &exp, n, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            pg = n;
        else
            pg = exp;
    }
    return pg;
}

static NOS void set(uptr a, uptr n, unsigned char v)
{
    while (n) {
        uptr off = a & 4095, k = 4096 - off < n ? 4096 - off : n;
        unsigned char *pg = page(a, v != 0);
        if (pg)
            __builtin_memset(pg + off, v, k);
        a += k;
        n -= k;
    }
}

static NOS unsigned char get1(uptr a)
{
    unsigned char *pg = page(a, 0);
    return pg ? pg[a & 4095] : 0;
}

static NOS void put1(uptr a, unsigned char v)
{
    unsigned char *pg = page(a, v != 0);
    if (pg)
        pg[a & 4095] = v;
}

static NOS void copy(uptr d, uptr s, uptr n)
{
    if (d == s || !n)
        return;
    if (d < s || d >= s + n) {
        for (uptr k = 0; k < n; k++)
            put1(d + k, get1(s + k));
    } else {
        for (uptr k = n; k--; )
            put1(d + k, get1(s + k));
    }
}

NOS void __embcc_msan_poison(uptr a, uptr n) { set(a, n, 0xff); }
NOS void __embcc_msan_unpoison(uptr a, uptr n) { set(a, n, 0); }
NOS void __embcc_msan_copy(uptr d, uptr s, uptr n) { copy(d, s, n); }

/* the shadow of n bytes at a, as an integer of the target's order */
NOS u64 __embcc_msan_load(uptr a, uptr n)
{
    u64 v = 0;
    unsigned char *pg = page(a, 0);
    if (!pg && ((a + n - 1) >> 12) == (a >> 12))
        return 0;
    for (uptr k = 0; k < n && k < 8; k++)
        v |= (u64)get1(a + k) << (8 * k);
    return v;
}

NOS void __embcc_msan_store(uptr a, uptr n, u64 s)
{
    if (!s && !page(a, 0) && ((a + n - 1) >> 12) == (a >> 12))
        return;
    for (uptr k = 0; k < n && k < 8; k++)
        put1(a + k, (unsigned char)(s >> (8 * k)));
}

/* ---- arguments and results ------------------------------------------ */

static __thread u64 g_param[16], g_ret;

NOS void __embcc_msan_param(uptr k, u64 s)
{
    if (k < 16)
        g_param[k] = s;
}

NOS u64 __embcc_msan_get_param(uptr k)
{
    if (k >= 16)
        return 0;
    u64 s = g_param[k];
    g_param[k] = 0;
    return s;
}

NOS void __embcc_msan_retval(u64 s) { g_ret = s; }

NOS u64 __embcc_msan_get_retval(void)
{
    u64 s = g_ret;
    g_ret = 0;
    return s;
}

/* ---- the report ---------------------------------------------------- */

static NOS void report(uptr pc)
{
    g_reports++;
    san_puts("==");
    san_dec((u64)getpid());
    san_puts("==WARNING: MemorySanitizer: use-of-uninitialized-value\n");
    uptr pcs[32];
    san_print_stack(pcs, san_backtrace(pcs, 32, pc));
    san_puts("\nSUMMARY: MemorySanitizer: use-of-uninitialized-value\n");
    if (g_halt)
        san_die(g_exitcode);
}

NOS void __embcc_msan_check(u64 s)
{
    if (s)
        report((uptr)__builtin_return_address(0));
}

NOS void __msan_unpoison(const void *p, usize n) { set((uptr)p, n, 0); }
NOS void __msan_poison(const void *p, usize n) { set((uptr)p, n, 0xff); }

NOS long __msan_test_shadow(const void *p, usize n)
{
    for (usize k = 0; k < n; k++)
        if (get1((uptr)p + k))
            return (long)k;
    return -1;
}

/* ---- the heap ------------------------------------------------------ */

#define MAGIC 0x4d53414e424d4345ull
struct hdr { u64 magic; uptr size; };
#define HSZ 16

static NOS void *blk(void *raw, usize n, int init)
{
    if (!raw)
        return 0;
    struct hdr *h = raw;
    h->magic = MAGIC;
    h->size = n;
    set((uptr)raw + HSZ, n, init ? 0 : 0xff);
    return (char *)raw + HSZ;
}

NOS void *__embcc_msan_malloc(usize n) { return blk(malloc(n + HSZ), n, 0); }

NOS void *__embcc_msan_calloc(usize n, usize m)
{
    if (m && n > (usize)-1 / m)
        return 0;
    return blk(calloc(1, n * m + HSZ), n * m, 1);
}

NOS void __embcc_msan_free(void *p)
{
    if (!p)
        return;
    struct hdr *h = (struct hdr *)((char *)p - HSZ);
    if (h->magic != MAGIC) {
        free(p);
        return;
    }
    set((uptr)p, h->size, 0);
    h->magic = 0;
    free(h);
}

NOS void *__embcc_msan_realloc(void *p, usize n)
{
    if (!p)
        return __embcc_msan_malloc(n);
    struct hdr *h = (struct hdr *)((char *)p - HSZ);
    if (h->magic != MAGIC)
        return realloc(p, n);
    void *q = __embcc_msan_malloc(n);
    if (q) {
        usize k = h->size < n ? h->size : n;
        __builtin_memcpy(q, p, k);
        copy((uptr)q, (uptr)p, k);
        __embcc_msan_free(p);
    }
    return q;
}

int posix_memalign(void **, usize, usize);
NOS int __embcc_msan_posix_memalign(void **out, usize al, usize n)
{
    int r = posix_memalign(out, al, n);
    if (!r)
        set((uptr)*out, n, 0xff);
    set((uptr)out, sizeof *out, 0);
    return r;
}

NOS void *__embcc_msan_aligned_alloc(usize al, usize n)
{
    void *p = 0;
    if (__embcc_msan_posix_memalign(&p, al < sizeof(void *) ? sizeof(void *)
                                                            : al, n))
        return 0;
    return p;
}

NOS void *__embcc_msan_memalign(usize al, usize n)
{
    return __embcc_msan_aligned_alloc(al, n);
}

NOS void *__embcc_msan_valloc(usize n)
{
    return __embcc_msan_aligned_alloc(4096, n);
}

/* ---- memory and strings -------------------------------------------- */

char *strdup(const char *);
char *strndup(const char *, usize);

NOS void *__embcc_msan_memcpy(void *d, const void *s, usize n)
{
    __builtin_memcpy(d, s, n);
    copy((uptr)d, (uptr)s, n);
    return d;
}

NOS void *__embcc_msan_memmove(void *d, const void *s, usize n)
{
    __builtin_memmove(d, s, n);
    copy((uptr)d, (uptr)s, n);
    return d;
}

NOS void *__embcc_msan_memset(void *d, int c, usize n)
{
    __builtin_memset(d, c, n);
    set((uptr)d, n, 0);
    return d;
}

NOS void __embcc_msan_bzero(void *d, usize n)
{
    __builtin_memset(d, 0, n);
    set((uptr)d, n, 0);
}

NOS char *__embcc_msan_strdup(const char *s)
{
    usize n = san_strlen(s) + 1;
    char *p = __embcc_msan_malloc(n);
    if (p) {
        __builtin_memcpy(p, s, n);
        copy((uptr)p, (uptr)s, n);
    }
    return p;
}

NOS char *__embcc_msan_strndup(const char *s, usize m)
{
    usize n = 0;
    while (n < m && s[n])
        n++;
    char *p = __embcc_msan_malloc(n + 1);
    if (p) {
        __builtin_memcpy(p, s, n);
        copy((uptr)p, (uptr)s, n);
        p[n] = 0;
        set((uptr)p + n, 1, 0);
    }
    return p;
}

NOS char *__embcc_msan_strcpy(char *d, const char *s)
{
    usize n = san_strlen(s) + 1;
    __builtin_memcpy(d, s, n);
    copy((uptr)d, (uptr)s, n);
    return d;
}

NOS char *__embcc_msan_stpcpy(char *d, const char *s)
{
    usize n = san_strlen(s);
    __embcc_msan_strcpy(d, s);
    return d + n;
}

NOS char *__embcc_msan_strncpy(char *d, const char *s, usize m)
{
    usize n = 0;
    while (n < m && s[n])
        n++;
    __builtin_memcpy(d, s, n);
    copy((uptr)d, (uptr)s, n);
    if (n < m) {
        __builtin_memset(d + n, 0, m - n);
        set((uptr)d + n, m - n, 0);
    }
    return d;
}

NOS char *__embcc_msan_strcat(char *d, const char *s)
{
    __embcc_msan_strcpy(d + san_strlen(d), s);
    return d;
}

NOS char *__embcc_msan_strncat(char *d, const char *s, usize m)
{
    usize dn = san_strlen(d), n = 0;
    while (n < m && s[n])
        n++;
    __builtin_memcpy(d + dn, s, n);
    copy((uptr)d + dn, (uptr)s, n);
    d[dn + n] = 0;
    set((uptr)d + dn + n, 1, 0);
    return d;
}

/* ---- what the C library writes ------------------------------------- */

typedef struct msan_FILE MFILE;
long read(int, void *, usize);
long pread(int, void *, usize, long);
usize fread(void *, usize, usize, MFILE *);
char *fgets(char *, int, MFILE *);
long getline(char **, usize *, MFILE *);
long getdelim(char **, usize *, int, MFILE *);
int vsprintf(char *, const char *, va_list);
int vsnprintf(char *, usize, const char *, va_list);
int vscanf(const char *, va_list);
int vsscanf(const char *, const char *, va_list);
int vfscanf(MFILE *, const char *, va_list);
int stat(const char *, void *);
int fstat(int, void *);
int lstat(const char *, void *);
long time(long *);
int gettimeofday(void *, void *);
int clock_gettime(int, void *);
int pipe(int *);
void *localtime_r(const long *, void *);
void *gmtime_r(const long *, void *);
usize strftime(char *, usize, const char *, const void *);
char *getcwd(char *, usize);
char *realpath(const char *, char *);
long readlink(const char *, char *, usize);
long recv(int, void *, usize, int);
long recvfrom(int, void *, usize, int, void *, void *);
int socketpair(int, int, int, int *);
double frexp(double, int *);
double modf(double, double *);
long strtol(const char *, char **, int);
unsigned long strtoul(const char *, char **, int);
long long strtoll(const char *, char **, int);
unsigned long long strtoull(const char *, char **, int);
double strtod(const char *, char **);
float strtof(const char *, char **);

#if defined(__APPLE__)
#define STAT_SIZE 144
#elif defined(__aarch64__)
#define STAT_SIZE 128
#else
#define STAT_SIZE 144
#endif
#define TM_SIZE 56

static NOS void good(const void *p, usize n)
{
    if (p)
        set((uptr)p, n, 0);
}

NOS long __embcc_msan_read(int fd, void *b, usize n)
{
    long r = read(fd, b, n);
    if (r > 0) good(b, (usize)r);
    return r;
}

NOS long __embcc_msan_pread(int fd, void *b, usize n, long off)
{
    long r = pread(fd, b, n, off);
    if (r > 0) good(b, (usize)r);
    return r;
}

NOS long __embcc_msan_recv(int fd, void *b, usize n, int fl)
{
    long r = recv(fd, b, n, fl);
    if (r > 0) good(b, (usize)r);
    return r;
}

NOS long __embcc_msan_recvfrom(int fd, void *b, usize n, int fl, void *a,
                               void *al)
{
    long r = recvfrom(fd, b, n, fl, a, al);
    if (r > 0) good(b, (usize)r);
    if (al) good(al, 4);
    if (a && al) good(a, *(u32 *)al);
    return r;
}

NOS usize __embcc_msan_fread(void *b, usize sz, usize n, MFILE *f)
{
    usize r = fread(b, sz, n, f);
    good(b, r * sz);
    return r;
}

NOS char *__embcc_msan_fgets(char *b, int n, MFILE *f)
{
    char *r = fgets(b, n, f);
    if (r) good(b, san_strlen(b) + 1);
    return r;
}

NOS long __embcc_msan_getdelim(char **l, usize *cap, int d, MFILE *f)
{
    long r = getdelim(l, cap, d, f);
    good(l, sizeof *l);
    good(cap, sizeof *cap);
    if (r >= 0 && *l) good(*l, (usize)r + 1);
    return r;
}

NOS long __embcc_msan_getline(char **l, usize *cap, MFILE *f)
{
    return __embcc_msan_getdelim(l, cap, '\n', f);
}

NOS int __embcc_msan_vsprintf(char *b, const char *fmt, va_list ap)
{
    int r = vsprintf(b, fmt, ap);
    if (r >= 0) good(b, (usize)r + 1);
    return r;
}

NOS int __embcc_msan_vsnprintf(char *b, usize n, const char *fmt, va_list ap)
{
    int r = vsnprintf(b, n, fmt, ap);
    if (r >= 0 && n) good(b, ((usize)r < n ? (usize)r : n - 1) + 1);
    return r;
}

NOS int __embcc_msan_sprintf(char *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = __embcc_msan_vsprintf(b, fmt, ap);
    va_end(ap);
    return r;
}

NOS int __embcc_msan_snprintf(char *b, usize n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = __embcc_msan_vsnprintf(b, n, fmt, ap);
    va_end(ap);
    return r;
}

/* what a scanf assigned: the pointers of its first `done` conversions,
 * each made good for its type's size */
static NOS void scan_good(const char *f, int done, va_list ap)
{
    for (; *f && done > 0; f++) {
        if (*f != '%')
            continue;
        f++;
        if (*f == '%')
            continue;
        int skip = 0, width = 0, len = 0;      /* len: h -1 hh -2 l 1 ll 2 L 3 z/j/t 4 */
        if (*f == '*') { skip = 1; f++; }
        while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        if (*f == 'h') { len = -1; f++; if (*f == 'h') { len = -2; f++; } }
        else if (*f == 'l') { len = 1; f++; if (*f == 'l') { len = 2; f++; } }
        else if (*f == 'L') { len = 3; f++; }
        else if (*f == 'z' || *f == 'j' || *f == 't') { len = 4; f++; }
        if (*f == '[') {
            f++;
            if (*f == '^') f++;
            if (*f == ']') f++;
            while (*f && *f != ']') f++;
            if (!skip) { char *p = va_arg(ap, char *); good(p, san_strlen(p) + 1); done--; }
            continue;
        }
        if (skip)
            continue;
        void *p = va_arg(ap, void *);
        usize n;
        switch (*f) {
        case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': case 'n':
            n = len == -2 ? 1 : len == -1 ? 2 : len == 2 ? 8
              : len == 1 || len == 4 ? sizeof(long) : 4;
            break;
        case 'f': case 'e': case 'g': case 'a': case 'E': case 'G': case 'A':
            n = len == 3 ? 16 : len == 1 ? 8 : 4;
            break;
        case 'c':
            n = width ? (usize)width : 1;
            break;
        case 's':
            n = san_strlen(p) + 1;
            break;
        case 'p':
            n = sizeof(void *);
            break;
        default:
            n = 0;
        }
        good(p, n);
        if (*f != 'n')
            done--;
    }
}

NOS int __embcc_msan_vsscanf(const char *s, const char *fmt, va_list ap)
{
    va_list cp;
    va_copy(cp, ap);
    int r = vsscanf(s, fmt, ap);
    scan_good(fmt, r, cp);
    va_end(cp);
    return r;
}

NOS int __embcc_msan_vscanf(const char *fmt, va_list ap)
{
    va_list cp;
    va_copy(cp, ap);
    int r = vscanf(fmt, ap);
    scan_good(fmt, r, cp);
    va_end(cp);
    return r;
}

NOS int __embcc_msan_vfscanf(MFILE *f, const char *fmt, va_list ap)
{
    va_list cp;
    va_copy(cp, ap);
    int r = vfscanf(f, fmt, ap);
    scan_good(fmt, r, cp);
    va_end(cp);
    return r;
}

NOS int __embcc_msan_sscanf(const char *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = __embcc_msan_vsscanf(s, fmt, ap);
    va_end(ap);
    return r;
}

NOS int __embcc_msan_scanf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = __embcc_msan_vscanf(fmt, ap);
    va_end(ap);
    return r;
}

NOS int __embcc_msan_fscanf(MFILE *f, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = __embcc_msan_vfscanf(f, fmt, ap);
    va_end(ap);
    return r;
}

NOS int __embcc_msan_stat(const char *p, void *b)
{
    int r = stat(p, b);
    if (!r) good(b, STAT_SIZE);
    return r;
}

NOS int __embcc_msan_lstat(const char *p, void *b)
{
    int r = lstat(p, b);
    if (!r) good(b, STAT_SIZE);
    return r;
}

NOS int __embcc_msan_fstat(int fd, void *b)
{
    int r = fstat(fd, b);
    if (!r) good(b, STAT_SIZE);
    return r;
}

NOS long __embcc_msan_time(long *t)
{
    long r = time(t);
    good(t, sizeof *t);
    return r;
}

NOS int __embcc_msan_gettimeofday(void *tv, void *tz)
{
    int r = gettimeofday(tv, tz);
    good(tv, 16);
    good(tz, 8);
    return r;
}

NOS int __embcc_msan_clock_gettime(int c, void *ts)
{
    int r = clock_gettime(c, ts);
    good(ts, 16);
    return r;
}

NOS int __embcc_msan_pipe(int *fds)
{
    int r = pipe(fds);
    if (!r) good(fds, 2 * sizeof *fds);
    return r;
}

NOS int __embcc_msan_socketpair(int d, int t, int p, int *sv)
{
    int r = socketpair(d, t, p, sv);
    if (!r) good(sv, 2 * sizeof *sv);
    return r;
}

NOS void *__embcc_msan_localtime_r(const long *t, void *tm)
{
    void *r = localtime_r(t, tm);
    if (r) good(tm, TM_SIZE);
    return r;
}

NOS void *__embcc_msan_gmtime_r(const long *t, void *tm)
{
    void *r = gmtime_r(t, tm);
    if (r) good(tm, TM_SIZE);
    return r;
}

NOS usize __embcc_msan_strftime(char *b, usize n, const char *f, const void *tm)
{
    usize r = strftime(b, n, f, tm);
    if (r) good(b, r + 1);
    return r;
}

NOS char *__embcc_msan_getcwd(char *b, usize n)
{
    char *r = getcwd(b, n);
    if (r) good(r, san_strlen(r) + 1);
    return r;
}

NOS char *__embcc_msan_realpath(const char *p, char *b)
{
    char *r = realpath(p, b);
    if (r) good(r, san_strlen(r) + 1);
    return r;
}

NOS long __embcc_msan_readlink(const char *p, char *b, usize n)
{
    long r = readlink(p, b, n);
    if (r > 0) good(b, (usize)r);
    return r;
}

NOS double __embcc_msan_frexp(double x, int *e)
{
    double r = frexp(x, e);
    good(e, sizeof *e);
    return r;
}

NOS double __embcc_msan_modf(double x, double *ip)
{
    double r = modf(x, ip);
    good(ip, sizeof *ip);
    return r;
}

#define STRTO(T, name)                                                      \
    NOS T __embcc_msan_##name(const char *s, char **e, int b)               \
    {                                                                       \
        T r = name(s, e, b);                                                \
        good(e, sizeof *e);                                                 \
        return r;                                                           \
    }
STRTO(long, strtol)
STRTO(unsigned long, strtoul)
STRTO(long long, strtoll)
STRTO(unsigned long long, strtoull)

NOS double __embcc_msan_strtod(const char *s, char **e)
{
    double r = strtod(s, e);
    good(e, sizeof *e);
    return r;
}

NOS float __embcc_msan_strtof(const char *s, char **e)
{
    float r = strtof(s, e);
    good(e, sizeof *e);
    return r;
}

static NOS __attribute__((constructor(101))) void msan_ctor(void)
{
    g_exitcode = (int)san_option("MSAN_OPTIONS", "exitcode", 77);
    g_halt = (int)san_option("MSAN_OPTIONS", "halt_on_error", 1);
}

void __msan_init(void) {}
