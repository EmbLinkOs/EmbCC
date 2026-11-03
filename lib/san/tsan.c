/* libtsan: ThreadSanitizer's runtime, for what src/ir/sanitize.c emits.
 *
 * A happens-before race detector with vector clocks, FastTrack's way:
 * each thread has a clock per thread, its own entry the epoch it is in;
 * a release copies the thread's clock into the synchronization object's
 * (a mutex, an atomic's address, a thread's start and end) and moves the
 * thread to a new epoch, an acquire takes the object's clock into the
 * thread's. Each eight bytes of memory keep their last four accesses --
 * the thread, its epoch, which bytes, read or write, where -- and an
 * access races with one of them that is of another thread, overlaps it,
 * is a write if this is not, and is not before this thread's clock.
 *
 * The shadow is sparse: a table of the 1 GiB regions, each a table of
 * its 4 KiB pages, made as memory is first touched. The synchronization
 * calls are the program's own -- pthread_*, C11's thrd_*, mtx_*, cnd_*
 * -- renamed to these by the instrumentation, which do the real thing
 * and record what it orders. A program with a race exits with 66 (as
 * with clang's and GCC's ThreadSanitizer). TSAN_OPTIONS: exitcode,
 * halt_on_error, report_signal_unsafe is not read. */

#include "san.h"

#define MAXT 128

struct thr {
    u32 vc[MAXT];
    u32 final_vc[MAXT];
    int tid;
    int done;
    uptr stk[256];
    int sp;
    void *(*fn)(void *);
    int (*cfn)(void *);
    void *arg;
};

static __thread struct thr *g_cur;
static int g_nthr;
static int g_races, g_exitcode = 66, g_halt;

static NOS void spin_lock(volatile int *l)
{
    while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE))
        while (*l)
            ;
}

static NOS void spin_unlock(volatile int *l)
{
    __atomic_store_n(l, 0, __ATOMIC_RELEASE);
}

/* ---- memory the runtime keeps for itself --------------------------- */

static unsigned char *g_arena;
static usize g_arena_left;
static volatile int g_arena_lock;

static NOS void *raw_alloc(usize n)
{
    n = (n + 63) & ~(usize)63;
    spin_lock(&g_arena_lock);
    if (n > g_arena_left) {
        usize chunk = n > ((usize)64 << 20) ? n : (usize)64 << 20;
        void *p = mmap(0, chunk, SAN_PROT_RW, SAN_MAP_ANON_NORESERVE, -1, 0);
        if (p == (void *)-1 || !p) {
            spin_unlock(&g_arena_lock);
            san_puts("==TSan== out of memory for the shadow\n");
            san_die(1);
        }
        g_arena = p;
        g_arena_left = chunk;
    }
    void *r = g_arena;
    g_arena += n;
    g_arena_left -= n;
    spin_unlock(&g_arena_lock);
    return r;                        /* zeroed: fresh anonymous memory */
}

/* ---- threads ------------------------------------------------------- */

static NOS struct thr *new_thr(void)
{
    struct thr *t = raw_alloc(sizeof *t);
    t->tid = __atomic_fetch_add(&g_nthr, 1, __ATOMIC_RELAXED);
    if (t->tid >= MAXT)
        t->tid = MAXT - 1;          /* past MAXT threads share the last slot */
    t->vc[t->tid] = 1;
    return t;
}

static NOS struct thr *cur(void)
{
    struct thr *t = g_cur;
    if (!t) {
        t = new_thr();
        g_cur = t;
    }
    return t;
}

/* ---- the shadow ---------------------------------------------------- */

struct cell {
    u32 epoch;                      /* 0: empty */
    unsigned short tid;
    u8 mask;                        /* the bytes of the eight accessed */
    u8 write;
    uptr pc;
};

static struct cell **g_l1[1u << 17];        /* by address >> 30 */
static volatile int g_locks[1024];

static NOS struct cell *granule(uptr a)
{
    uptr i1 = (a >> 30) & ((1u << 17) - 1);
    struct cell **l2 = __atomic_load_n(&g_l1[i1], __ATOMIC_ACQUIRE);
    if (!l2) {
        struct cell **n = raw_alloc(sizeof(struct cell *) << 18);
        struct cell **exp = 0;
        if (__atomic_compare_exchange_n(&g_l1[i1], &exp, n, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            l2 = n;
        else
            l2 = exp;
    }
    uptr i2 = (a >> 12) & ((1u << 18) - 1);
    struct cell *pg = __atomic_load_n(&l2[i2], __ATOMIC_ACQUIRE);
    if (!pg) {
        struct cell *n = raw_alloc(sizeof(struct cell) * 4 * 512);
        struct cell *exp = 0;
        if (__atomic_compare_exchange_n(&l2[i2], &exp, n, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            pg = n;
        else
            pg = exp;
    }
    return pg + 4 * ((a >> 3) & 511);
}

/* a race already reported, by its two pcs */
static uptr g_seen[256][2];
static int g_nseen;
static volatile int g_report_lock;

static NOS void report(uptr a, int size, int write, uptr pc,
                       const struct cell *prev, struct thr *t)
{
    spin_lock(&g_report_lock);
    for (int k = 0; k < g_nseen; k++)
        if (g_seen[k][0] == pc && g_seen[k][1] == prev->pc) {
            spin_unlock(&g_report_lock);
            return;
        }
    if (g_nseen < 256) {
        g_seen[g_nseen][0] = pc;
        g_seen[g_nseen][1] = prev->pc;
        g_nseen++;
    }
    g_races++;
    san_puts("==================\nWARNING: ThreadSanitizer: data race (pid=");
    san_dec((u64)getpid());
    san_puts(")\n  ");
    san_puts(write ? "Write" : "Read");
    san_puts(" of size ");
    san_dec((u64)size);
    san_puts(" at ");
    san_hex(a);
    san_puts(" by thread T");
    san_dec((u64)t->tid);
    san_puts(":\n    #0 ");
    san_hex(pc);
    san_puts("\n");
    for (int k = t->sp - 1, d = 1; k >= 0 && d < 16; k--, d++) {
        san_puts("    #");
        san_dec((u64)d);
        san_puts(" ");
        san_hex(t->stk[k]);
        san_puts("\n");
    }
    san_puts("\n  Previous ");
    san_puts(prev->write ? "write" : "read");
    san_puts(" at ");
    san_hex(a);
    san_puts(" by thread T");
    san_dec((u64)prev->tid);
    san_puts(":\n    #0 ");
    san_hex(prev->pc);
    san_puts("\n\nSUMMARY: ThreadSanitizer: data race\n==================\n");
    spin_unlock(&g_report_lock);
    if (g_halt)
        san_die(g_exitcode);
}

static NOS void access1(uptr a, unsigned mask, int size, int write, uptr pc)
{
    struct thr *t = cur();
    struct cell *c = granule(a);
    volatile int *l = &g_locks[(a >> 3) & 1023];
    u32 ep = t->vc[t->tid];
    spin_lock(l);
    int slot = -1;
    for (int k = 0; k < 4; k++) {
        struct cell *x = &c[k];
        if (!x->epoch)
            continue;
        if (x->tid == t->tid) {
            /* this thread's own: the newer access of the same kind and
             * bytes replaces it (a write replaces a read too) */
            if ((x->mask & mask) == x->mask && (write || !x->write))
                slot = k;
            continue;
        }
        if ((x->mask & mask) && (x->write || write) &&
            x->epoch > t->vc[x->tid]) {
            struct cell prev = *x;
            spin_unlock(l);
            report(a, size, write, pc, &prev, t);
            spin_lock(l);
        }
    }
    if (slot < 0)
        for (int k = 0; k < 4; k++)
            if (!c[k].epoch) { slot = k; break; }
    if (slot < 0)
        slot = (int)(ep & 3);
    c[slot].epoch = ep;
    c[slot].tid = (unsigned short)t->tid;
    c[slot].mask = (u8)mask;
    c[slot].write = (u8)write;
    c[slot].pc = pc;
    spin_unlock(l);
}

static NOS void access(uptr a, uptr n, int write, uptr pc)
{
    while (n) {
        uptr off = a & 7, k = 8 - off < n ? 8 - off : n;
        unsigned mask = ((1u << k) - 1) << off;
        access1(a, mask, (int)k, write, pc);
        a += k;
        n -= k;
    }
}

/* forget [a, a + n): a block freed or allocated afresh */
static NOS void forget(uptr a, uptr n)
{
    for (uptr p = a & ~(uptr)7; p < a + n; p += 8) {
        struct cell *c = granule(p);
        volatile int *l = &g_locks[(p >> 3) & 1023];
        spin_lock(l);
        __builtin_memset(c, 0, 4 * sizeof *c);
        spin_unlock(l);
    }
}

#define PC ((uptr)__builtin_return_address(0))
#define ACC(N)                                                              \
    NOS void __tsan_read##N(uptr a) { access(a, N, 0, PC); }                \
    NOS void __tsan_write##N(uptr a) { access(a, N, 1, PC); }
ACC(1)
ACC(2)
ACC(4)
ACC(8)
ACC(16)
NOS void __tsan_read_range(uptr a, uptr n) { access(a, n, 0, PC); }
NOS void __tsan_write_range(uptr a, uptr n) { access(a, n, 1, PC); }

NOS void __tsan_func_entry(uptr fn)
{
    struct thr *t = cur();
    if (t->sp < 256)
        t->stk[t->sp] = fn;
    t->sp++;
}

NOS void __tsan_func_exit(void)
{
    struct thr *t = cur();
    if (t->sp > 0)
        t->sp--;
}

/* ---- synchronization ----------------------------------------------- */

struct sync {
    uptr addr;
    volatile int lock;
    u32 vc[MAXT];
};

#define NSYNC 16384
static struct sync *g_sync[NSYNC];
static volatile int g_sync_lock;

static NOS struct sync *sync_of(uptr a, int make)
{
    uptr h = (a >> 3) * 0x9e3779b97f4a7c15ull >> 20;
    spin_lock(&g_sync_lock);
    for (int k = 0; k < NSYNC; k++) {
        struct sync **s = &g_sync[(h + (uptr)k) & (NSYNC - 1)];
        if (*s && (*s)->addr == a) {
            struct sync *r = *s;
            spin_unlock(&g_sync_lock);
            return r;
        }
        if (!*s) {
            if (!make)
                break;
            *s = raw_alloc(sizeof **s);
            (*s)->addr = a;
            struct sync *r = *s;
            spin_unlock(&g_sync_lock);
            return r;
        }
    }
    spin_unlock(&g_sync_lock);
    return 0;
}

static NOS void release(uptr a)
{
    struct thr *t = cur();
    struct sync *s = sync_of(a, 1);
    if (s) {
        spin_lock(&s->lock);
        for (int i = 0; i < MAXT; i++)
            if (t->vc[i] > s->vc[i])
                s->vc[i] = t->vc[i];
        spin_unlock(&s->lock);
    }
    t->vc[t->tid]++;
}

static NOS void acquire(uptr a)
{
    struct thr *t = cur();
    struct sync *s = sync_of(a, 0);
    if (!s)
        return;
    spin_lock(&s->lock);
    for (int i = 0; i < MAXT; i++)
        if (s->vc[i] > t->vc[i])
            t->vc[i] = s->vc[i];
    spin_unlock(&s->lock);
}

NOS void __embcc_tsan_acquire(uptr a) { acquire(a); }
NOS void __embcc_tsan_release(uptr a) { release(a); }
NOS void __tsan_acquire(void *a) { acquire((uptr)a); }
NOS void __tsan_release(void *a) { release((uptr)a); }

/* ---- the C library's ------------------------------------------------ */

int pthread_create(void *, const void *, void *(*)(void *), void *);
int pthread_join(uptr, void **);
int pthread_detach(uptr);
int pthread_mutex_lock(void *);
int pthread_mutex_trylock(void *);
int pthread_mutex_timedlock(void *, const void *);
int pthread_mutex_unlock(void *);
int pthread_rwlock_rdlock(void *);
int pthread_rwlock_wrlock(void *);
int pthread_rwlock_tryrdlock(void *);
int pthread_rwlock_trywrlock(void *);
int pthread_rwlock_unlock(void *);
int pthread_spin_lock(void *);
int pthread_spin_trylock(void *);
int pthread_spin_unlock(void *);
int pthread_cond_wait(void *, void *);
int pthread_cond_timedwait(void *, void *, const void *);
int pthread_cond_signal(void *);
int pthread_cond_broadcast(void *);
int pthread_once(void *, void (*)(void));
int pthread_barrier_wait(void *);
int sem_wait(void *);
int sem_trywait(void *);
int sem_timedwait(void *, const void *);
int sem_post(void *);
int thrd_create(void *, int (*)(void *), void *);
int thrd_join(uptr, int *);
int thrd_detach(uptr);
int mtx_lock(void *);
int mtx_trylock(void *);
int mtx_timedlock(void *, const void *);
int mtx_unlock(void *);
int cnd_wait(void *, void *);
int cnd_timedwait(void *, void *, const void *);
int cnd_signal(void *);
int cnd_broadcast(void *);
void call_once(void *, void (*)(void));

/* the threads' records, by their handle, for join */
#define NTH 1024
static struct { uptr h; struct thr *t; } g_handles[NTH];
static volatile int g_handles_lock;

static NOS void remember(uptr h, struct thr *t)
{
    spin_lock(&g_handles_lock);
    for (int k = 0; k < NTH; k++)
        if (!g_handles[k].t || g_handles[k].h == h) {
            g_handles[k].h = h;
            g_handles[k].t = t;
            break;
        }
    spin_unlock(&g_handles_lock);
}

static NOS struct thr *recall(uptr h)
{
    struct thr *r = 0;
    spin_lock(&g_handles_lock);
    for (int k = 0; k < NTH; k++)
        if (g_handles[k].t && g_handles[k].h == h) {
            r = g_handles[k].t;
            g_handles[k].t = 0;
            break;
        }
    spin_unlock(&g_handles_lock);
    return r;
}

static NOS struct thr *spawn(void)
{
    struct thr *p = cur(), *c = new_thr();
    for (int i = 0; i < MAXT; i++)
        if (p->vc[i] > c->vc[i])
            c->vc[i] = p->vc[i];
    c->vc[c->tid]++;
    p->vc[p->tid]++;
    return c;
}

static NOS void finish(struct thr *t)
{
    for (int i = 0; i < MAXT; i++)
        t->final_vc[i] = t->vc[i];
    __atomic_store_n(&t->done, 1, __ATOMIC_RELEASE);
}

static NOS void joined(struct thr *c)
{
    struct thr *t = cur();
    if (!c)
        return;
    for (int i = 0; i < MAXT; i++)
        if (c->final_vc[i] > t->vc[i])
            t->vc[i] = c->final_vc[i];
}

static NOS void *start(void *arg)
{
    struct thr *t = arg;
    g_cur = t;
    void *r = t->fn(t->arg);
    finish(t);
    return r;
}

NOS int __embcc_tsan_pthread_create(uptr *th, const void *attr,
                                    void *(*fn)(void *), void *arg)
{
    struct thr *c = spawn();
    c->fn = fn;
    c->arg = arg;
    int r = pthread_create(th, attr, start, c);
    if (!r)
        remember(*th, c);
    return r;
}

NOS int __embcc_tsan_pthread_join(uptr th, void **ret)
{
    int r = pthread_join(th, ret);
    if (!r)
        joined(recall(th));
    return r;
}

NOS int __embcc_tsan_pthread_detach(uptr th)
{
    (void)recall(th);
    return pthread_detach(th);
}

#if !defined(__APPLE__)
static NOS int cstart(void *arg)
{
    struct thr *t = arg;
    g_cur = t;
    int r = t->cfn(t->arg);
    finish(t);
    return r;
}

NOS int __embcc_tsan_thrd_create(uptr *th, int (*fn)(void *), void *arg)
{
    struct thr *c = spawn();
    c->cfn = fn;
    c->arg = arg;
    int r = thrd_create(th, cstart, c);
    if (!r)
        remember(*th, c);
    return r;
}

NOS int __embcc_tsan_thrd_join(uptr th, int *res)
{
    int r = thrd_join(th, res);
    if (!r)
        joined(recall(th));
    return r;
}

NOS int __embcc_tsan_thrd_detach(uptr th)
{
    (void)recall(th);
    return thrd_detach(th);
}
#endif

/* a lock: acquire once held; an unlock: release before letting go */
#define LOCK1(name)                                                         \
    NOS int __embcc_tsan_##name(void *m)                                    \
    {                                                                       \
        int r = name(m);                                                    \
        if (!r) acquire((uptr)m);                                           \
        return r;                                                           \
    }
#define UNLOCK1(name)                                                       \
    NOS int __embcc_tsan_##name(void *m)                                    \
    {                                                                       \
        release((uptr)m);                                                   \
        return name(m);                                                     \
    }
LOCK1(pthread_mutex_lock)
LOCK1(pthread_mutex_trylock)
UNLOCK1(pthread_mutex_unlock)
LOCK1(pthread_rwlock_rdlock)
LOCK1(pthread_rwlock_wrlock)
LOCK1(pthread_rwlock_tryrdlock)
LOCK1(pthread_rwlock_trywrlock)
UNLOCK1(pthread_rwlock_unlock)
LOCK1(sem_wait)
LOCK1(sem_trywait)
UNLOCK1(sem_post)
/* (macOS has no spin locks, barriers, timed mutexes or semaphores, nor
 * C11's threads) */
#if !defined(__APPLE__)
LOCK1(pthread_spin_lock)
LOCK1(pthread_spin_trylock)
UNLOCK1(pthread_spin_unlock)
LOCK1(mtx_lock)
LOCK1(mtx_trylock)
UNLOCK1(mtx_unlock)
#endif

#if !defined(__APPLE__)
NOS int __embcc_tsan_pthread_mutex_timedlock(void *m, const void *ts)
{
    int r = pthread_mutex_timedlock(m, ts);
    if (!r) acquire((uptr)m);
    return r;
}

NOS int __embcc_tsan_sem_timedwait(void *m, const void *ts)
{
    int r = sem_timedwait(m, ts);
    if (!r) acquire((uptr)m);
    return r;
}

NOS int __embcc_tsan_mtx_timedlock(void *m, const void *ts)
{
    int r = mtx_timedlock(m, ts);
    if (!r) acquire((uptr)m);
    return r;
}
#endif

/* C11: thrd_success is 0 in EmbCC's, glibc's and libSystem's */

NOS int __embcc_tsan_pthread_cond_wait(void *c, void *m)
{
    release((uptr)m);
    int r = pthread_cond_wait(c, m);
    acquire((uptr)m);
    acquire((uptr)c);
    return r;
}

NOS int __embcc_tsan_pthread_cond_timedwait(void *c, void *m, const void *ts)
{
    release((uptr)m);
    int r = pthread_cond_timedwait(c, m, ts);
    acquire((uptr)m);
    acquire((uptr)c);
    return r;
}

NOS int __embcc_tsan_pthread_cond_signal(void *c)
{
    release((uptr)c);
    return pthread_cond_signal(c);
}

NOS int __embcc_tsan_pthread_cond_broadcast(void *c)
{
    release((uptr)c);
    return pthread_cond_broadcast(c);
}

#if !defined(__APPLE__)
NOS int __embcc_tsan_cnd_wait(void *c, void *m)
{
    release((uptr)m);
    int r = cnd_wait(c, m);
    acquire((uptr)m);
    acquire((uptr)c);
    return r;
}

NOS int __embcc_tsan_cnd_timedwait(void *c, void *m, const void *ts)
{
    release((uptr)m);
    int r = cnd_timedwait(c, m, ts);
    acquire((uptr)m);
    acquire((uptr)c);
    return r;
}

NOS int __embcc_tsan_cnd_signal(void *c)
{
    release((uptr)c);
    return cnd_signal(c);
}

NOS int __embcc_tsan_cnd_broadcast(void *c)
{
    release((uptr)c);
    return cnd_broadcast(c);
}
#endif

#if !defined(__APPLE__)
NOS int __embcc_tsan_pthread_barrier_wait(void *b)
{
    release((uptr)b);
    int r = pthread_barrier_wait(b);
    acquire((uptr)b);
    return r;
}
#endif

/* once: the routine's effects are released on the once object and
 * acquired by every caller */
static __thread void (*g_once_fn)(void);
static __thread void *g_once_obj;

static NOS void once_tramp(void)
{
    g_once_fn();
    release((uptr)g_once_obj);
}

NOS int __embcc_tsan_pthread_once(void *o, void (*fn)(void))
{
    g_once_fn = fn;
    g_once_obj = o;
    int r = pthread_once(o, once_tramp);
    acquire((uptr)o);
    return r;
}

#if !defined(__APPLE__)
NOS void __embcc_tsan_call_once(void *o, void (*fn)(void))
{
    g_once_fn = fn;
    g_once_obj = o;
    call_once(o, once_tramp);
    acquire((uptr)o);
}
#endif

/* ---- the heap: a block's history is forgotten when it is freed ----- */

#define MAGIC 0x5453414e424d4345ull
struct hdr { u64 magic; uptr size; };
#define HSZ 16

static NOS void *blk(void *raw, usize n)
{
    if (!raw)
        return 0;
    struct hdr *h = raw;
    h->magic = MAGIC;
    h->size = n;
    forget((uptr)raw + HSZ, n);
    return (char *)raw + HSZ;
}

NOS void *__embcc_tsan_malloc(usize n) { return blk(malloc(n + HSZ), n); }

NOS void *__embcc_tsan_calloc(usize n, usize m)
{
    if (m && n > (usize)-1 / m)
        return 0;
    return blk(calloc(1, n * m + HSZ), n * m);
}

NOS void __embcc_tsan_free(void *p)
{
    if (!p)
        return;
    struct hdr *h = (struct hdr *)((char *)p - HSZ);
    if (h->magic != MAGIC) {
        free(p);
        return;
    }
    access((uptr)p, h->size ? 1 : 0, 1, PC);   /* a free is a write */
    forget((uptr)p, h->size);
    h->magic = 0;
    free(h);
}

NOS void *__embcc_tsan_realloc(void *p, usize n)
{
    if (!p)
        return __embcc_tsan_malloc(n);
    struct hdr *h = (struct hdr *)((char *)p - HSZ);
    if (h->magic != MAGIC)
        return realloc(p, n);
    void *q = __embcc_tsan_malloc(n);
    if (q) {
        __builtin_memcpy(q, p, h->size < n ? h->size : n);
        __embcc_tsan_free(p);
    }
    return q;
}

NOS int __embcc_tsan_posix_memalign(void **out, usize al, usize n)
{
    if (al <= 16) {
        *out = __embcc_tsan_malloc(n);
        return *out ? 0 : 12;
    }
    /* over-aligned: the library's own, its history forgotten */
    void *p = 0;
    int posix_memalign(void **, usize, usize);
    int r = posix_memalign(&p, al, n);
    if (!r) {
        forget((uptr)p, n);
        *out = p;
    }
    return r;
}

NOS void *__embcc_tsan_aligned_alloc(usize al, usize n)
{
    void *p = 0;
    return __embcc_tsan_posix_memalign(&p, al < sizeof(void *)
                                               ? sizeof(void *) : al, n)
           ? 0 : p;
}

NOS char *__embcc_tsan_strdup(const char *s)
{
    usize n = san_strlen(s) + 1;
    access((uptr)s, n, 0, PC);
    char *p = __embcc_tsan_malloc(n);
    if (p)
        __builtin_memcpy(p, s, n);
    return p;
}

NOS char *__embcc_tsan_strndup(const char *s, usize m)
{
    usize n = 0;
    while (n < m && s[n])
        n++;
    access((uptr)s, n, 0, PC);
    char *p = __embcc_tsan_malloc(n + 1);
    if (p) {
        __builtin_memcpy(p, s, n);
        p[n] = 0;
    }
    return p;
}

NOS void *__embcc_tsan_memcpy(void *d, const void *s, usize n)
{
    access((uptr)s, n, 0, PC);
    access((uptr)d, n, 1, PC);
    return __builtin_memcpy(d, s, n);
}

NOS void *__embcc_tsan_memmove(void *d, const void *s, usize n)
{
    access((uptr)s, n, 0, PC);
    access((uptr)d, n, 1, PC);
    return __builtin_memmove(d, s, n);
}

NOS void *__embcc_tsan_memset(void *d, int c, usize n)
{
    access((uptr)d, n, 1, PC);
    return __builtin_memset(d, c, n);
}

/* ---- start and end ------------------------------------------------- */

static NOS void fini(void)
{
    if (g_races) {
        fflush(0);                  /* the program's own output first */
        san_puts("ThreadSanitizer: reported ");
        san_dec((u64)g_races);
        san_puts(g_races == 1 ? " warning\n" : " warnings\n");
        san_die(g_exitcode);
    }
}

int atexit(void (*)(void));

static NOS __attribute__((constructor(101))) void tsan_ctor(void)
{
    g_exitcode = (int)san_option("TSAN_OPTIONS", "exitcode", 66);
    g_halt = (int)san_option("TSAN_OPTIONS", "halt_on_error", 0);
    (void)cur();
    atexit(fini);
}

void __tsan_init(void) {}
