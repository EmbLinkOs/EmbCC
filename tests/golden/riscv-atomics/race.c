/* One- and two-byte atomics against an interrupt, on QEMU's virt board
 * (tests/golden/riscv-atomics.sh).
 *
 * The machine timer interrupts main every few hundred instructions, and
 * its handler increments every OTHER lane of the word main is working on,
 * with plain stores. Main meanwhile runs one atomic operation on its own
 * lane twenty thousand times. Each lane must come out exact: main's at
 * what main did, the others at the number of interrupts. A sequence that
 * is not atomic loses main's update or writes a neighbour's stale value
 * back over the handler's; one that is wrongly masked or shifted carries
 * into a neighbour or misses its own lane.
 *
 * A phase per lane and operation: fetch_add on each byte and each
 * halfword (the LR/SC loop), a compare-exchange loop on each byte, and
 * fetch_xor (an AMO on the word) on each byte. A line per phase: the
 * operation, the lane, main's lane right (1), the neighbours right (1),
 * and enough interrupts arrived (1). */
void putn(long v);
void puts_(const char *s);

#define MTIMECMP_LO (*(volatile unsigned *)0x2004000u)
#define MTIMECMP_HI (*(volatile unsigned *)0x2004004u)
#define MTIME_LO    (*(volatile unsigned *)0x200bff8u)

#define PERIOD 3u               /* mtime ticks: ~300 instructions */
#define N      20000L
#define WANT   100UL            /* interrupts a phase must see at least */

static volatile union {
    unsigned w;
    unsigned char b[4];
    unsigned short h[2];
} s __attribute__((aligned(4)));

static volatile unsigned long ticks;
static volatile int lane, half;          /* what main is working on */

static void rearm(void)
{
    MTIMECMP_HI = 0xffffffffu;
    MTIMECMP_LO = MTIME_LO + PERIOD;
    MTIMECMP_HI = 0;
}

/* every other lane, by a plain read-modify-write: the handler cannot be
 * interrupted, so these are exact */
__attribute__((interrupt)) void tick(void)
{
    ticks++;
    if (half) {
        s.h[lane ^ 1]++;
    } else {
        for (int k = 0; k < 4; k++)
            if (k != lane)
                s.b[k]++;
    }
    rearm();
}

/* the lanes zeroed and the count read with the interrupt off, so the
 * handler sees one phase or the other */
static void irq_off(void) { __asm__ volatile("csrc mstatus, %0" : : "r"(8UL) : "memory"); }
static void irq_on(void)  { __asm__ volatile("csrs mstatus, %0" : : "r"(8UL) : "memory"); }

static unsigned long start(int l, int h)
{
    unsigned long t0;
    irq_off();
    s.w = 0;
    lane = l;
    half = h;
    t0 = ticks;
    irq_on();
    return t0;
}

static void report(const char *op, int l, unsigned long t0, unsigned mine)
{
    unsigned long n;
    int nb = 1;
    irq_off();
    n = ticks - t0;
    if (half) {
        nb = s.h[l ^ 1] == (unsigned short)n;
        mine = (unsigned short)mine;
    } else {
        for (int k = 0; k < 4; k++)
            if (k != l && s.b[k] != (unsigned char)n)
                nb = 0;
        mine = (unsigned char)mine;
    }
    puts_(op);
    putn(l);
    putn((half ? s.h[l] : s.b[l]) == mine);
    putn(nb);
    putn(n >= WANT);
    puts_("\n");
}

/* (noinline: one copy of each loop, called for every lane) */
__attribute__((noinline)) void add_b(volatile unsigned char *p)
{
    for (long k = 0; k < N; k++)
        __atomic_fetch_add(p, 3, __ATOMIC_SEQ_CST);
}

__attribute__((noinline)) void add_h(volatile unsigned short *p)
{
    for (long k = 0; k < N; k++)
        __atomic_fetch_add(p, 0x101, __ATOMIC_RELAXED);
}

__attribute__((noinline)) void cas_b(volatile unsigned char *p)
{
    for (long k = 0; k < N; k++) {
        unsigned char e = *p;
        while (!__atomic_compare_exchange_n(p, &e, (unsigned char)(e + 5), 1,
                                            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            ;
    }
}

__attribute__((noinline)) void xor_b(volatile unsigned char *p)
{
    for (long k = 0; k < N; k++)
        __atomic_fetch_xor(p, (unsigned char)(k | 1), __ATOMIC_RELEASE);
}

int main(void)
{
    unsigned long t0;
    unsigned x = 0;
    __asm__ volatile("csrw mtvec, %0" : : "r"(tick));
    rearm();
    __asm__ volatile("csrs mie, %0" : : "r"(0x80UL));        /* MTIE */

    for (int l = 0; l < 4; l++) {
        t0 = start(l, 0);
        add_b(&s.b[l]);
        report("add.b ", l, t0, 3 * N);
    }
    for (int l = 0; l < 2; l++) {
        t0 = start(l, 1);
        add_h(&s.h[l]);
        report("add.h ", l, t0, 0x101 * N);
    }
    for (int l = 0; l < 4; l++) {
        t0 = start(l, 0);
        cas_b(&s.b[l]);
        report("cas.b ", l, t0, 5 * N);
    }
    for (long k = 0; k < N; k++)
        x ^= (unsigned char)(k | 1);
    for (int l = 0; l < 4; l++) {
        t0 = start(l, 0);
        xor_b(&s.b[l]);
        report("xor.b ", l, t0, x);
    }
    MTIMECMP_HI = 0xffffffffu;
    puts_("DONE\n");
    return 0;
}
