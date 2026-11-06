/* What lib/san's runtimes share: output, dying, the options string, the
 * memory they map, and a frame-pointer backtrace. No header of a C
 * library is included -- the library under them may be EmbCC's, glibc,
 * libSystem or a board's -- and the handful of functions used are
 * declared here, the same in all of them. */
#ifndef EMBCC_SAN_H
#define EMBCC_SAN_H

#define NOS __attribute__((no_sanitize("address", "thread", "memory"), \
                           no_instrument_function,                     \
                           no_profile_instrument_function))
#define WEAK __attribute__((weak))
/* a helper here that one runtime or another does not use */
#define NOSH NOS __attribute__((unused))

typedef __UINTPTR_TYPE__ uptr;
typedef __SIZE_TYPE__ usize;
typedef unsigned long long u64;
typedef __UINT32_TYPE__ u32;
typedef unsigned char u8;

/* how far apart two frames of one stack can be, at most */
#define SAN_FAR ((uptr)(sizeof(uptr) > 2 ? 0x4000000ul : 0x4000u))

#if defined(__linux__) || defined(__APPLE__) || defined(__unix__)
#define SAN_HOSTED 1
#else
#define SAN_HOSTED 0
#endif

void *malloc(usize);
void *calloc(usize, usize);
void *realloc(void *, usize);
void free(void *);
char *getenv(const char *);
void abort(void);
#if SAN_HOSTED
long write(int, const void *, usize);
void _exit(int);
int getpid(void);
void *mmap(void *, usize, int, int, int, long);
#define SAN_PROT_RW 3
#if defined(__APPLE__)
#define SAN_MAP_ANON_NORESERVE (0x0002 | 0x1000 | 0x0040)
#else
#define SAN_MAP_ANON_NORESERVE (0x0002 | 0x20 | 0x4000)
#endif
#else
int putchar(int);
#endif

/* the console: standard error, or a board's putchar; a program may
 * replace it */
WEAK NOS void __embcc_san_write(const char *s, usize n)
{
#if SAN_HOSTED
    while (n) {
        long w = write(2, s, n);
        if (w <= 0)
            break;
        s += w;
        n -= (usize)w;
    }
#else
    while (n--)
        putchar(*s++);
#endif
}

static NOSH usize san_strlen(const char *s)
{
    usize n = 0;
    while (s[n])
        n++;
    return n;
}

static NOSH void san_puts(const char *s)
{
    __embcc_san_write(s, san_strlen(s));
}

static NOSH void san_hex(uptr v)
{
    char b[2 + 2 * sizeof(uptr) + 1];
    int k = 0;
    b[k++] = '0';
    b[k++] = 'x';
    int started = 0;
    for (int sh = (int)(8 * sizeof v) - 4; sh >= 0; sh -= 4) {
        int d = (int)(v >> sh) & 15;
        if (d || started || sh == 0) {
            b[k++] = "0123456789abcdef"[d];
            started = 1;
        }
    }
    __embcc_san_write(b, (usize)k);
}

static NOSH void san_dec(u64 v)
{
    char b[24];
    int k = 0;
    do { b[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    char o[24];
    for (int i = 0; i < k; i++)
        o[i] = b[k - 1 - i];
    __embcc_san_write(o, (usize)k);
}

/* NAME=VALUE in the options variable, as an integer, or dflt */
static NOSH long san_option(const char *env, const char *name, long dflt)
{
#if SAN_HOSTED
    const char *s = getenv(env);
    usize n = san_strlen(name);
    while (s && *s) {
        while (*s == ':' || *s == ' ' || *s == ',')
            s++;
        usize k = 0;
        while (s[k] && s[k] == name[k] && k < n)
            k++;
        if (k == n && s[k] == '=') {
            const char *v = s + k + 1;
            long r = 0;
            int neg = *v == '-';
            if (neg) v++;
            if (*v == 't') return 1;        /* true */
            if (*v == 'f') return 0;        /* false */
            while (*v >= '0' && *v <= '9')
                r = r * 10 + (*v++ - '0');
            return neg ? -r : r;
        }
        while (*s && *s != ':' && *s != ' ' && *s != ',')
            s++;
    }
#else
    (void)env;
    (void)name;
#endif
    return dflt;
}

int fflush(void *);

static NOSH void san_die(int code)
{
#if SAN_HOSTED
    fflush(0);
    _exit(code);
#else
    (void)code;
    abort();
#endif
    for (;;)
        ;
}

/* The pcs of the stack by its frame records ([saved fp][return address]),
 * from the caller of the runtime function that asks: what a target that
 * keeps a frame chain has (AArch64, and x86-64 where rbp is kept); where
 * the chain is not kept, the first frame only. */
static NOSH int san_backtrace(uptr *pcs, int max, uptr first_pc)
{
    uptr raw[64];
    int nr = 0;
    uptr *fp = (uptr *)__builtin_frame_address(0);
    uptr lo = (uptr)fp;
    for (int guard = 0; nr < 64 && fp && guard < 64; guard++) {
        if (((uptr)fp & (sizeof(uptr) - 1)) || (uptr)fp < lo ||
            (uptr)fp - lo > SAN_FAR)
            break;
        uptr ret = fp[1];
        if (!ret)
            break;
        raw[nr++] = ret;
        uptr *next = (uptr *)fp[0];
        if (next <= fp)
            break;
        fp = next;
    }
    /* the runtime's own frames are left out: from the faulting pc on */
    int start = -1;
    for (int k = 0; k < nr; k++)
        if (raw[k] == first_pc) { start = k; break; }
    int n = 0;
    if (start < 0) {
        pcs[n++] = first_pc;
        return n;
    }
    for (int k = start; k < nr && n < max; k++)
        pcs[n++] = raw[k];
    return n;
}

#if SAN_HOSTED
/* dladdr's answer: the same four fields first in glibc's and libSystem's
 * Dl_info. Every hosted C library has dladdr -- EmbCC's own reads the
 * program's .symtab (lib/libc/src/posix/dl.c) -- and the reference is
 * strong: a weak one does not pull an archive member, as ld and lld (and
 * now embld) have it, so EmbCC's static libc's dladdr was never linked
 * and every frame of a report was a bare address. */
struct san_dlinfo {
    const char *fname;
    void *fbase;
    const char *sname;
    void *saddr;
};
int dladdr(const void *, struct san_dlinfo *);
#endif

/* Each frame as LLVM's runtimes print it before symbolizing:
 *   #1 0x100003f40 in main+0x24 (/path/prog+0x3f40)
 * the function where the dynamic symbol table names it, and always the
 * module and the offset into it, which addr2line, atos and
 * llvm-symbolizer turn into a file and line. A return address is looked
 * up one byte back, in the call. */
static NOSH void san_print_stack(const uptr *pcs, int n)
{
    for (int i = 0; i < n; i++) {
        san_puts("    #");
        san_dec((u64)i);
        san_puts(" ");
        san_hex(pcs[i]);
#if SAN_HOSTED
        struct san_dlinfo di;
        di.fname = di.sname = 0;
        di.fbase = di.saddr = 0;
        if (dladdr((const void *)(pcs[i] - (i ? 1 : 0)), &di)) {
            if (di.sname && di.saddr) {
                san_puts(" in ");
                san_puts(di.sname);
                san_puts("+");
                san_hex(pcs[i] - (uptr)di.saddr);
            }
            if (di.fname) {
                san_puts(" (");
                san_puts(di.fname);
                san_puts("+");
                san_hex(pcs[i] - (uptr)di.fbase);
                san_puts(")");
            }
        }
#endif
        san_puts("\n");
    }
}

#endif
