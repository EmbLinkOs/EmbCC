/* libatomic's generic entry points on the 64-bit targets, which take the
 * object's size first:
 *
 *   void  __atomic_load(size_t n, const volatile void *p, void *ret, int order)
 *   void  __atomic_store(size_t n, volatile void *p, void *val, int order)
 *   void  __atomic_exchange(size_t n, volatile void *p, void *val,
 *                           void *ret, int order)
 *   _Bool __atomic_compare_exchange(size_t n, volatile void *p,
 *                                   void *expected, void *desired,
 *                                   int success, int failure)
 *
 * The compiler calls them for an _Atomic object one access does not move
 * -- a 12-byte struct, a 3-byte one (src/ir/irgen.c, atomic_generic) --
 * as GCC's and clang's do. The 32-bit targets have them in atomic8.c,
 * where masking interrupts is how a single core makes them atomic; here
 * the code may run unprivileged (a program on EmbLinkOS, Linux, macOS, or
 * at AArch64's EL0), where it cannot mask anything. So each object is
 * guarded by a lock -- one of 64, chosen by its address -- taken with the
 * machine's own atomic exchange, as GCC's libatomic does where it has no
 * wider instruction. That makes them atomic against every other caller of
 * these routines on any core, not against an interrupt handler that calls
 * them while the interrupted code holds the same lock (which would spin
 * for ever): don't, or replace them -- every routine is weak.
 *
 * Compiled to nothing where pointers are not eight bytes: tools/build-rt.sh
 * compiles every lib/rt file for every triple. */
#if __SIZEOF_POINTER__ == 8

typedef __SIZE_TYPE__ rt_size;
typedef volatile unsigned char vbyte;

/* Their names are builtins to the compiler, so they are defined under
 * internal ones: named by alias (as in atomic8.c), or, in Mach-O, which
 * has no aliases, by asm label (the symbol itself, with its underscore). */
#if defined(__APPLE__)
#define RT_GEN
void gen_load(rt_size, const volatile void *, void *, int)
    __asm__("___atomic_load") __attribute__((weak));
void gen_store(rt_size, volatile void *, void *, int)
    __asm__("___atomic_store") __attribute__((weak));
void gen_exchange(rt_size, volatile void *, void *, void *, int)
    __asm__("___atomic_exchange") __attribute__((weak));
_Bool gen_cmpxchg(rt_size, volatile void *, void *, void *, int, int)
    __asm__("___atomic_compare_exchange") __attribute__((weak));
#else
#define RT_GEN static
#endif

static volatile unsigned char g_locks[64];

static volatile unsigned char *lock_for(const volatile void *p)
{
    unsigned long a = (unsigned long)p;
    return &g_locks[(a >> 4 ^ a >> 10) & 63];
}

static void lock(volatile unsigned char *l)
{
    while (__atomic_test_and_set(l, __ATOMIC_ACQUIRE))
        while (*l)
            ;
}

static void unlock(volatile unsigned char *l)
{
    __atomic_clear(l, __ATOMIC_RELEASE);
}

RT_GEN void gen_load(rt_size n, const volatile void *p, void *ret, int order)
{
    const vbyte *s = (const vbyte *)p;
    vbyte *d = (vbyte *)ret;
    volatile unsigned char *l = lock_for(p);
    (void)order;
    lock(l);
    for (rt_size k = 0; k < n; k++)
        d[k] = s[k];
    unlock(l);
}

RT_GEN void gen_store(rt_size n, volatile void *p, void *val, int order)
{
    vbyte *d = (vbyte *)p;
    const vbyte *s = (const vbyte *)val;
    volatile unsigned char *l = lock_for(p);
    (void)order;
    lock(l);
    for (rt_size k = 0; k < n; k++)
        d[k] = s[k];
    unlock(l);
}

/* byte by byte, so `ret` may be `val` */
RT_GEN void gen_exchange(rt_size n, volatile void *p, void *val, void *ret,
                         int order)
{
    vbyte *q = (vbyte *)p, *v = (vbyte *)val, *r = (vbyte *)ret;
    volatile unsigned char *l = lock_for(p);
    (void)order;
    lock(l);
    for (rt_size k = 0; k < n; k++) {
        unsigned char t = q[k];
        q[k] = v[k];
        r[k] = t;
    }
    unlock(l);
}

RT_GEN _Bool gen_cmpxchg(rt_size n, volatile void *p, void *expected,
                         void *desired, int success, int failure)
{
    vbyte *q = (vbyte *)p, *e = (vbyte *)expected, *d = (vbyte *)desired;
    volatile unsigned char *l = lock_for(p);
    _Bool ok = 1;
    (void)success;
    (void)failure;
    lock(l);
    for (rt_size k = 0; k < n; k++)
        if (q[k] != e[k])
            ok = 0;
    for (rt_size k = 0; k < n; k++) {
        if (ok)
            q[k] = d[k];
        else
            e[k] = q[k];
    }
    unlock(l);
    return ok;
}

#if !defined(__APPLE__)
void __atomic_load(rt_size n, const volatile void *p, void *ret, int order)
    __attribute__((weak, alias("gen_load")));
void __atomic_store(rt_size n, volatile void *p, void *val, int order)
    __attribute__((weak, alias("gen_store")));
void __atomic_exchange(rt_size n, volatile void *p, void *val, void *ret,
                       int order) __attribute__((weak, alias("gen_exchange")));
_Bool __atomic_compare_exchange(rt_size n, volatile void *p, void *expected,
                                void *desired, int success, int failure)
    __attribute__((weak, alias("gen_cmpxchg")));
#endif

#endif /* __SIZEOF_POINTER__ == 8 */
