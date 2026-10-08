/* EmbTrace's recorder: the two hooks -finstrument-functions calls,
 *
 *   void __cyg_profile_func_enter(void *this_fn, void *call_site);
 *   void __cyg_profile_func_exit(void *this_fn, void *call_site);
 *
 * kept in a ring of events in RAM, and embtrace_dump(), which writes the
 * ring out as text a console log or a UART capture carries to the host,
 * where tools/embtrace turns it into call counts, times, a call tree and
 * a Chrome/Perfetto trace. <embtrace.h> is the program's side.
 *
 * Interrupt- and multi-core-safe without masking anything: an event's
 * slot is reserved with one atomic add on a 32-bit counter (an
 * instruction, or lib/rt's routine where the core has none), and a
 * reserved slot is its writer's alone. The ring keeps the newest
 * `embtrace_capacity` events; the dump says how many were lost.
 *
 * Everything is WEAK, so a program can replace any part: the ring (define
 * embtrace_events and embtrace_capacity together, e.g. in a no-init
 * section that survives a reset), the clock (embtrace_clock: a cycle
 * counter -- <embtrace.h> has DWT's and mcycle's -- or 0, when the order
 * of events is all the trace says), and the output (embtrace_putc). None
 * of it is instrumented: this file is never compiled with
 * -finstrument-functions, and says so besides. */

#define NOI __attribute__((no_instrument_function))
#define WEAK __attribute__((weak))

typedef unsigned int u32;
typedef __UINTPTR_TYPE__ uptr;

struct embtrace_ev {
    uptr fn;        /* the function entered or left */
    uptr site;      /* its return address (GCC's call_site) */
    u32 t;          /* embtrace_clock() */
    u32 kind;       /* 1 enter, 2 exit */
};

#if __SIZEOF_POINTER__ == 2
#define EMBTRACE_DEFAULT 32         /* AVR: 2 KiB of SRAM in all */
#else
#define EMBTRACE_DEFAULT 128
#endif

WEAK struct embtrace_ev embtrace_events[EMBTRACE_DEFAULT];
WEAK const u32 embtrace_capacity = EMBTRACE_DEFAULT;
WEAK const u32 embtrace_clock_hz = 0;      /* 0: the clock's unit is unknown */

static u32 embtrace_head;                   /* events ever recorded */
static volatile int embtrace_off;           /* embtrace_enable(0) */

WEAK NOI u32 embtrace_clock(void) { return 0; }
WEAK NOI void embtrace_putc(int c) { (void)c; }

NOI void embtrace_enable(int on) { embtrace_off = !on; }

NOI void embtrace_reset(void)
{
    __atomic_store_n(&embtrace_head, 0, __ATOMIC_SEQ_CST);
}

static NOI void record(u32 kind, void *fn, void *site)
{
    if (embtrace_off || !embtrace_capacity)
        return;
    u32 t = embtrace_clock();
    u32 k = __atomic_fetch_add(&embtrace_head, 1, __ATOMIC_RELAXED);
    struct embtrace_ev *e = &embtrace_events[k % embtrace_capacity];
    e->fn = (uptr)fn;
    e->site = (uptr)site;
    e->t = t;
    e->kind = kind;
}

WEAK NOI void __cyg_profile_func_enter(void *this_fn, void *call_site)
{
    record(1, this_fn, call_site);
}

WEAK NOI void __cyg_profile_func_exit(void *this_fn, void *call_site)
{
    record(2, this_fn, call_site);
}

/* ---- the dump ----------------------------------------------------------
 *
 *   EMBTRACE 1 <pointer bytes> <capacity> <recorded> <clock Hz>
 *   <kind> <fn> <site> <time>          one line per event, oldest first,
 *   ...                                in hex
 *   EMBTRACE END
 *
 * Recording is off while it runs, so the dump's own calls (an instrumented
 * embtrace_putc) are not in it. */
static NOI void put_s(const char *s)
{
    while (*s)
        embtrace_putc(*s++);
}

static NOI void put_hex(unsigned long long v)
{
    char b[17];
    int n = 0;
    do {
        b[n++] = "0123456789abcdef"[v & 15];
        v >>= 4;
    } while (v);
    while (n)
        embtrace_putc(b[--n]);
}

NOI void embtrace_dump(void)
{
    int was = embtrace_off;
    embtrace_off = 1;
    u32 head = __atomic_load_n(&embtrace_head, __ATOMIC_SEQ_CST);
    u32 cap = embtrace_capacity, n = head < cap ? head : cap;
    put_s("EMBTRACE 1 ");
    put_hex(sizeof(uptr));
    embtrace_putc(' ');
    put_hex(cap);
    embtrace_putc(' ');
    put_hex(head);
    embtrace_putc(' ');
    put_hex(embtrace_clock_hz);
    embtrace_putc('\n');
    for (u32 k = head - n; k != head; k++) {
        const struct embtrace_ev *e = &embtrace_events[k % cap];
        put_hex(e->kind);
        embtrace_putc(' ');
        put_hex(e->fn);
        embtrace_putc(' ');
        put_hex(e->site);
        embtrace_putc(' ');
        put_hex(e->t);
        embtrace_putc('\n');
    }
    put_s("EMBTRACE END\n");
    embtrace_off = was;
}
