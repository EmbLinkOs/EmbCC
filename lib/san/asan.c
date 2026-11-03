/* libasan: AddressSanitizer's runtime, for what src/ir/sanitize.c emits.
 *
 * The shadow is one byte per eight bytes of memory, LLVM's encoding:
 * 0 all eight addressable, 1-7 the first k, and a negative byte a kind
 * of redzone -- 0xfa a heap block's, 0xfd a freed block, 0xf1-0xf3 the
 * stack's, 0xf9 a global's. shadow(a) = base + a / 8.
 *
 * Where the shadow is: on a 64-bit host, reserved once (2^44 bytes, not
 * backed until touched) for every user address; on a board, the RAM the
 * program says -- __embcc_asan_ram_lo and _hi -- with the shadow in
 * __embcc_asan_shadow_mem, or taken from the heap when that is not given.
 * An address outside what the shadow covers (flash, a peripheral) is not
 * checked. Before the shadow exists nothing is checked either.
 *
 * The heap: malloc and the rest, which the instrumentation renames to
 * these (so the library's own allocations are left alone and still
 * freed right), put each block between redzones and keep a freed block
 * poisoned in a quarantine for a while, so a use after free is caught.
 * A block the C library allocated is recognised and handed back to its
 * free. ASAN_OPTIONS: halt_on_error, exitcode, quarantine_size_mb,
 * abort_on_error. */

#include "san.h"

static uptr g_base;              /* shadow(a) = g_base + (a >> 3) */
static uptr g_lo, g_hi;          /* what it covers */
static int g_inited, g_recover_default, g_exitcode = 1, g_abort;
static int g_reports;

/* a board's RAM and, optionally, its shadow (hi - lo) / 8 bytes long */
WEAK uptr __embcc_asan_ram_lo, __embcc_asan_ram_hi;
WEAK unsigned char *__embcc_asan_shadow_mem;

static NOS void init(void)
{
    if (g_inited)
        return;
    g_inited = 1;
#if SAN_HOSTED && __SIZEOF_POINTER__ == 8
    usize len = (usize)1 << 44;
    void *p = mmap(0, len, SAN_PROT_RW, SAN_MAP_ANON_NORESERVE, -1, 0);
    if (p == (void *)-1 || !p) {
        san_puts("==ASan== cannot reserve the shadow memory\n");
        return;
    }
    g_base = (uptr)p;
    g_lo = 0;
    g_hi = (uptr)1 << 47;
#else
    if (__embcc_asan_ram_hi > __embcc_asan_ram_lo) {
        uptr n = (__embcc_asan_ram_hi - __embcc_asan_ram_lo + 7) / 8;
        unsigned char *s = __embcc_asan_shadow_mem;
        if (!s)
            s = calloc(1, n);
        if (s) {
            g_lo = __embcc_asan_ram_lo;
            g_hi = __embcc_asan_ram_hi;
            g_base = (uptr)s - (g_lo >> 3);
        }
    }
#endif
    g_exitcode = (int)san_option("ASAN_OPTIONS", "exitcode", 1);
    g_abort = (int)san_option("ASAN_OPTIONS", "abort_on_error", 0);
    g_recover_default = !san_option("ASAN_OPTIONS", "halt_on_error", 1);
}

#define SH(a) (*(signed char *)(g_base + ((uptr)(a) >> 3)))

static NOS int covered(uptr a)
{
    return g_base && a >= g_lo && a < g_hi;
}

/* the shadow of [a, a + n): v for whole granules (a granule-aligned) */
static NOS void poison(uptr a, uptr n, signed char v)
{
    if (!g_inited) init();
    for (uptr p = a & ~(uptr)7; p < a + n; p += 8)
        if (covered(p))
            SH(p) = v;
}

/* [a, a + n) addressable: a is granule-aligned; the granule the end is in
 * says how much of it */
static NOS void unpoison(uptr a, uptr n)
{
    if (!g_inited) init();
    uptr p = a;
    for (; p + 8 <= a + n; p += 8)
        if (covered(p))
            SH(p) = 0;
    if (p < a + n && covered(p))
        SH(p) = (signed char)(a + n - p);
}

/* the first byte of [a, a + n) that is not addressable, or 0 */
static NOS uptr first_bad(uptr a, uptr n)
{
    for (uptr p = a; p < a + n; ) {
        if (!covered(p)) { p = (p | 7) + 1; continue; }
        signed char s = SH(p);
        uptr g = p & ~(uptr)7, end = g + 8 < a + n ? g + 8 : a + n;
        if (s) {
            if (s < 0)
                return p;
            if (end > g + (uptr)s)
                return p >= g + (uptr)s ? p : g + (uptr)s;
        }
        p = end;
    }
    return 0;
}

/* ---- the heap ------------------------------------------------------ */

#define MAGIC 0x41534e434d434245ull        /* "EBCMCNSA" */
#define ST_LIVE 1
#define ST_FREED 2
struct chunk {                             /* right before the user's bytes */
    u64 magic;
    uptr size;                             /* what was asked */
    void *raw;                             /* what the library gave */
    uptr alloc_pc, free_pc;
    struct chunk *next;                    /* in the quarantine */
    u32 state;
    u32 rawsize;
};
#define HDR ((uptr)((sizeof(struct chunk) + 15) & ~(usize)15))
#define LRZ (HDR > 32 ? HDR : 32)

static struct chunk *g_q_head, *g_q_tail;  /* the quarantine, oldest first */
static uptr g_q_bytes;

static NOS struct chunk *hdr(const void *p)
{
    return (struct chunk *)((uptr)p - HDR);
}

static NOS void report(uptr addr, uptr size, int write, uptr pc,
                       const char *kind_override);

static NOS void *alloc(usize n, usize align, uptr pc)
{
    if (!g_inited) init();
    if (align < 16)
        align = 16;
    usize rz = 32 + (32 - n % 32) % 32;
    usize total = LRZ + align + n + rz;
    if (total < n)
        return 0;
    unsigned char *raw = malloc(total);
    if (!raw)
        return 0;
    uptr user = ((uptr)raw + LRZ + align - 1) & ~(uptr)(align - 1);
    struct chunk *c = hdr((void *)user);
    c->magic = MAGIC;
    c->size = n;
    c->raw = raw;
    c->alloc_pc = pc;
    c->free_pc = 0;
    c->state = ST_LIVE;
    c->rawsize = (u32)total;
    poison((uptr)raw, (uptr)(user - (uptr)raw), (signed char)0xfa);
    unpoison(user, n);
    uptr end = (user + n + 7) & ~(uptr)7;
    poison(end, (uptr)raw + total - end, (signed char)0xfa);
    return (void *)user;
}

static NOS void q_drain(uptr cap)
{
    while (g_q_head && g_q_bytes > cap) {
        struct chunk *c = g_q_head;
        g_q_head = c->next;
        if (!g_q_head)
            g_q_tail = 0;
        g_q_bytes -= c->rawsize;
        void *raw = c->raw;
        poison((uptr)raw, c->rawsize, 0);  /* the library's again */
        c->magic = 0;
        free(raw);
    }
}

static NOS void release(void *p, uptr pc)
{
    if (!p)
        return;
    if (!g_inited) init();
    struct chunk *c = hdr(p);
    if (c->magic != MAGIC) {
        free(p);                           /* the C library's block */
        return;
    }
    if (c->state != ST_LIVE) {
        report((uptr)p, 0, 0, pc, "attempting double-free");
        if (!g_recover_default)
            san_die(g_exitcode);
        return;
    }
    c->state = ST_FREED;
    c->free_pc = pc;
    poison((uptr)p, c->size ? c->size : 1, (signed char)0xfd);
    c->next = 0;
    if (g_q_tail)
        g_q_tail->next = c;
    else
        g_q_head = c;
    g_q_tail = c;
    g_q_bytes += c->rawsize;
#if SAN_HOSTED
    uptr cap = (uptr)san_option("ASAN_OPTIONS", "quarantine_size_mb", 16) << 20;
#else
    uptr cap = 4096;
#endif
    q_drain(cap);
}

#define PC ((uptr)__builtin_return_address(0))

NOS void *__embcc_asan_malloc(usize n) { return alloc(n, 16, PC); }

NOS void *__embcc_asan_calloc(usize n, usize m)
{
    if (m && n > (usize)-1 / m)
        return 0;
    unsigned char *p = alloc(n * m, 16, PC);
    if (p)
        __builtin_memset(p, 0, n * m);
    return p;
}

NOS void __embcc_asan_free(void *p) { release(p, PC); }

NOS void *__embcc_asan_realloc(void *p, usize n)
{
    if (!p)
        return alloc(n, 16, PC);
    struct chunk *c = hdr(p);
    if (c->magic != MAGIC)
        return realloc(p, n);              /* the C library's block */
    if (c->state != ST_LIVE) {
        report((uptr)p, 0, 0, PC, "attempting realloc of freed memory");
        if (!g_recover_default)
            san_die(g_exitcode);
        return 0;
    }
    if (!n) {
        release(p, PC);
        return 0;
    }
    void *q = alloc(n, 16, PC);
    if (q) {
        __builtin_memcpy(q, p, c->size < n ? c->size : n);
        release(p, PC);
    }
    return q;
}

NOS int __embcc_asan_posix_memalign(void **out, usize al, usize n)
{
    if (!al || (al & (al - 1)) || al % sizeof(void *))
        return 22;                         /* EINVAL */
    void *p = alloc(n, al, PC);
    if (!p)
        return 12;                         /* ENOMEM */
    *out = p;
    return 0;
}

NOS void *__embcc_asan_aligned_alloc(usize al, usize n)
{
    if (!al || (al & (al - 1)))
        return 0;
    return alloc(n, al, PC);
}

NOS void *__embcc_asan_memalign(usize al, usize n) { return alloc(n, al, PC); }
NOS void *__embcc_asan_valloc(usize n) { return alloc(n, 4096, PC); }
NOS void *__embcc_asan_pvalloc(usize n)
{
    return alloc((n + 4095) & ~(usize)4095, 4096, PC);
}

/* ---- the checks ---------------------------------------------------- */

static NOS const char *kind_of(uptr a)
{
    if (!covered(a))
        return "unknown-crash";
    signed char s = SH(a);
    if (s > 0 && covered(a + 8))
        s = SH(a + 8);
    switch ((unsigned char)s) {
    case 0xfa: return "heap-buffer-overflow";
    case 0xfd: return "heap-use-after-free";
    case 0xf1: case 0xf2: case 0xf3: return "stack-buffer-overflow";
    case 0xf9: return "global-buffer-overflow";
    default: return "unknown-crash";
    }
}

/* The heap block an address in a heap redzone (or a freed block) is
 * part of, by its header: above it, for an address in a block's left
 * redzone, else below it. Only for an address whose shadow says heap, so
 * only memory of a block of this allocator is read. */
static NOS struct chunk *near_chunk(uptr a)
{
    if (!covered(a))
        return 0;
    unsigned char s = (unsigned char)SH(a);
    if (s && s < 8 && covered(a + 8))
        s = (unsigned char)SH(a + 8);
    if (s != 0xfa && s != 0xfd)
        return 0;
    for (uptr p = (a + 15) & ~(uptr)15; p < a + LRZ + 64; p += 16) {
        struct chunk *c = hdr((void *)p);
        if (c->magic == MAGIC && (uptr)c->raw <= a)
            return c;
    }
    for (uptr p = a & ~(uptr)15; p >= HDR && a - p < SAN_FAR;
         p -= 16) {
        if (!covered(p - HDR))
            break;
        struct chunk *c = hdr((void *)p);
        if (c->magic == MAGIC && (uptr)c->raw <= a &&
            a < (uptr)c->raw + c->rawsize)
            return c;
    }
    return 0;
}

static NOS void report(uptr addr, uptr size, int write, uptr pc,
                       const char *kind_override)
{
    g_reports++;
    const char *kind = kind_override ? kind_override : kind_of(addr);
    san_puts("=================================================================\n");
#if SAN_HOSTED
    san_puts("==");
    san_dec((u64)getpid());
    san_puts("==");
#endif
    san_puts("ERROR: AddressSanitizer: ");
    san_puts(kind);
    san_puts(" on address ");
    san_hex(addr);
    san_puts(" at pc ");
    san_hex(pc);
    san_puts("\n");
    if (size) {
        san_puts(write ? "WRITE" : "READ");
        san_puts(" of size ");
        san_dec(size);
        san_puts(" at ");
        san_hex(addr);
        san_puts(" thread T0\n");
    }
    uptr pcs[32];
    san_print_stack(pcs, san_backtrace(pcs, 32, pc));
    struct chunk *c = near_chunk(addr);
    if (c) {
        uptr user = (uptr)c + HDR;
        san_hex(addr);
        if (addr >= user + c->size) {
            san_puts(" is located ");
            san_dec(addr - (user + c->size));
            san_puts(" bytes after ");
        } else if (addr < user) {
            san_puts(" is located ");
            san_dec(user - addr);
            san_puts(" bytes before ");
        } else {
            san_puts(" is located ");
            san_dec(addr - user);
            san_puts(" bytes inside of ");
        }
        san_dec(c->size);
        san_puts("-byte region [");
        san_hex(user);
        san_puts(",");
        san_hex(user + c->size);
        san_puts(")\n");
        if (c->state == ST_FREED) {
            san_puts("freed by thread T0 here:\n    #0 ");
            san_hex(c->free_pc);
            san_puts("\n");
        }
        san_puts("allocated by thread T0 here:\n    #0 ");
        san_hex(c->alloc_pc);
        san_puts("\n");
    }
    san_puts("SUMMARY: AddressSanitizer: ");
    san_puts(kind);
    san_puts("\n");
}

static NOS void fail(uptr a, uptr n, int write, uptr pc, int recover)
{
    uptr bad = first_bad(a, n);
    report(bad ? bad : a, n, write, pc, 0);
    if (!recover && !g_recover_default) {
        if (g_abort)
            abort();
        san_die(g_exitcode);
    }
}

static NOS void check(uptr a, uptr n, int write, uptr pc, int recover)
{
    if (!g_inited) init();
    if (!covered(a))
        return;
    if (n <= 8 && (a & 7) + n <= 8) {
        signed char s = SH(a);
        if (!s || (s > 0 && (signed char)((a & 7) + n - 1) < s))
            return;
        fail(a, n, write, pc, recover);
        return;
    }
    if (first_bad(a, n))
        fail(a, n, write, pc, recover);
}

#define CHECKS(N)                                                           \
    NOS void __asan_load##N(uptr a) { check(a, N, 0, PC, 0); }              \
    NOS void __asan_store##N(uptr a) { check(a, N, 1, PC, 0); }             \
    NOS void __asan_load##N##_noabort(uptr a) { check(a, N, 0, PC, 1); }    \
    NOS void __asan_store##N##_noabort(uptr a) { check(a, N, 1, PC, 1); }
CHECKS(1)
CHECKS(2)
CHECKS(4)
CHECKS(8)
CHECKS(16)
NOS void __asan_loadN(uptr a, uptr n) { check(a, n, 0, PC, 0); }
NOS void __asan_storeN(uptr a, uptr n) { check(a, n, 1, PC, 0); }
NOS void __asan_loadN_noabort(uptr a, uptr n) { check(a, n, 0, PC, 1); }
NOS void __asan_storeN_noabort(uptr a, uptr n) { check(a, n, 1, PC, 1); }

/* ---- the C library's memory and string functions ------------------- */

static NOS void overlap(uptr d, uptr s, uptr n, const char *what, uptr pc)
{
    if (n && ((d <= s && s < d + n) || (s <= d && d < s + n))) {
        report(d, 0, 0, pc, what);
        if (!g_recover_default)
            san_die(g_exitcode);
    }
}

NOS void *__embcc_asan_memcpy(void *d, const void *s, usize n)
{
    check((uptr)s, n, 0, PC, 0);
    check((uptr)d, n, 1, PC, 0);
    overlap((uptr)d, (uptr)s, n, "memcpy-param-overlap", PC);
    return __builtin_memcpy(d, s, n);
}

NOS void *__embcc_asan_memmove(void *d, const void *s, usize n)
{
    check((uptr)s, n, 0, PC, 0);
    check((uptr)d, n, 1, PC, 0);
    return __builtin_memmove(d, s, n);
}

NOS void *__embcc_asan_memset(void *d, int c, usize n)
{
    check((uptr)d, n, 1, PC, 0);
    return __builtin_memset(d, c, n);
}

NOS usize __embcc_asan_strlen(const char *s)
{
    usize n = san_strlen(s);
    check((uptr)s, n + 1, 0, PC, 0);
    return n;
}

NOS usize __embcc_asan_strnlen(const char *s, usize m)
{
    usize n = 0;
    while (n < m && s[n])
        n++;
    check((uptr)s, n < m ? n + 1 : n, 0, PC, 0);
    return n;
}

NOS char *__embcc_asan_strcpy(char *d, const char *s)
{
    usize n = san_strlen(s) + 1;
    check((uptr)s, n, 0, PC, 0);
    check((uptr)d, n, 1, PC, 0);
    overlap((uptr)d, (uptr)s, n, "strcpy-param-overlap", PC);
    return __builtin_memcpy(d, s, n);
}

NOS char *__embcc_asan_strncpy(char *d, const char *s, usize m)
{
    usize n = 0;
    while (n < m && s[n])
        n++;
    check((uptr)s, n < m ? n + 1 : n, 0, PC, 0);
    check((uptr)d, m, 1, PC, 0);
    __builtin_memcpy(d, s, n);
    if (n < m)
        __builtin_memset(d + n, 0, m - n);
    return d;
}

NOS char *__embcc_asan_strcat(char *d, const char *s)
{
    usize dn = san_strlen(d), sn = san_strlen(s) + 1;
    check((uptr)d, dn + sn, 1, PC, 0);
    check((uptr)s, sn, 0, PC, 0);
    __builtin_memcpy(d + dn, s, sn);
    return d;
}

NOS char *__embcc_asan_strncat(char *d, const char *s, usize m)
{
    usize dn = san_strlen(d), sn = 0;
    while (sn < m && s[sn])
        sn++;
    check((uptr)s, sn < m ? sn + 1 : sn, 0, PC, 0);
    check((uptr)d, dn + sn + 1, 1, PC, 0);
    __builtin_memcpy(d + dn, s, sn);
    d[dn + sn] = 0;
    return d;
}

NOS char *__embcc_asan_strdup(const char *s)
{
    usize n = san_strlen(s) + 1;
    check((uptr)s, n, 0, PC, 0);
    char *p = alloc(n, 16, PC);
    if (p)
        __builtin_memcpy(p, s, n);
    return p;
}

NOS char *__embcc_asan_strndup(const char *s, usize m)
{
    usize n = 0;
    while (n < m && s[n])
        n++;
    char *p = alloc(n + 1, 16, PC);
    if (p) {
        __builtin_memcpy(p, s, n);
        p[n] = 0;
    }
    return p;
}

/* ---- the stack and the globals ------------------------------------- */

static uptr g_stack_hi;              /* the highest frame block seen */

NOS void __embcc_asan_frame_poison(uptr base, uptr size)
{
    poison(base, size, (signed char)0xf2);
    if (base + size > g_stack_hi)
        g_stack_hi = base + size;
}

NOS void __embcc_asan_unpoison(uptr p, uptr n) { unpoison(p, n); }

/* a longjmp, a throw, an exit: the frames it leaves are not returned
 * from, so their blocks are made good from here up */
NOS void __asan_handle_no_return(void)
{
    uptr sp = (uptr)__builtin_frame_address(0);
    if (g_stack_hi > sp && g_stack_hi - sp < SAN_FAR)
        poison(sp & ~(uptr)7, g_stack_hi - (sp & ~(uptr)7), 0);
}

/* {n; {address, size, size with the redzone} x n} */
NOS void __embcc_asan_register_globals(void *table)
{
    uptr *t = table;
    uptr n = t[0];
    for (uptr k = 0; k < n; k++) {
        uptr a = t[1 + 3 * k], size = t[2 + 3 * k], full = t[3 + 3 * k];
        unpoison(a, size);
        uptr end = (a + size + 7) & ~(uptr)7;
        poison(end, a + full - end, (signed char)0xf9);
    }
}

/* LLVM's names for the same, for code instrumented by another compiler */
NOS void __asan_poison_memory_region(const void *p, usize n)
{
    poison((uptr)p, n, (signed char)0xf7);
}

NOS void __asan_unpoison_memory_region(const void *p, usize n)
{
    unpoison((uptr)p, n);
}

NOS int __asan_address_is_poisoned(const void *p)
{
    if (!g_inited) init();
    return first_bad((uptr)p, 1) != 0;
}

NOS void __asan_init(void) { init(); }

/* the earliest the shadow can be made: before any constructor of the
 * program that might allocate */
static NOS __attribute__((constructor(101))) void asan_ctor(void)
{
    init();
}
