/* MIPS32 interrupt handlers in C, interrupting code that has a value in
 * every register (tests/golden/mips-isr.sh), on QEMU's malta (24Kc).
 *
 * The CP0 timer (Count/Compare, interrupt line 7) interrupts main in
 * three phases, each with handlers compiled by EmbCC:
 *
 *   A  tick_masked, interrupt + keep_interrupts_masked, at the general
 *      exception vector: calls nothing, works in many registers and in
 *      HI/LO. It must save exactly what it writes.
 *   B  tick_call, interrupt("vector=hw5"): re-enables interrupts (with
 *      IM0..IM7 cleared, so none nests) and calls clobber_all(), which
 *      writes every caller-saved register, HI and LO, and a C function.
 *      It must save all of them.
 *   C  vectored interrupts (Cause.IV): the timer's handler tick_raise,
 *      vector=hw5, raises software interrupt 0 on every other tick, and
 *      sw0's handler soft0, vector=sw0, runs at whatever point of main
 *      the timer arrived and works until the timer interrupts IT -- it
 *      clears only IM0, so line 7 nests -- which only works if both save
 *      EPC and Status as well as the registers.
 *
 * In each phase main runs torture() (torture.S: a pattern in every
 * caller-saved register and HI/LO, checked over and over; a bit per
 * register that changed) and churn(), a C computation with many values
 * live whose result must equal the same computation with interrupts off.
 * A line per phase: its letter, the registers that changed (0 and 0),
 * whether churn agreed, and whether enough interrupts arrived (in C, also
 * whether the timer nested inside soft0); then the counts. */
void putn(long v);
void puts_(const char *s);

void torture(long n, long *bad);
void clobber_all(void);

#define PERIOD 2000u            /* Count ticks between interrupts */
#define ROUNDS 20000L
#define WANT   100UL

volatile unsigned long ticks, softs, nested, unexpected, sink;
volatile long vv[8] = { 3, 5, 7, 11, 13, 17, 19, 23 };

static unsigned rd_count(void)
{
    unsigned c;
    __asm__ volatile("mfc0 %0, $9" : "=r"(c));
    return c;
}

static void wr_compare(unsigned v)
{
    __asm__ volatile("mtc0 %0, $11" : : "r"(v));
}

static unsigned rd_cause(void)
{
    unsigned c;
    __asm__ volatile("mfc0 %0, $13" : "=r"(c));
    return c;
}

static void wr_cause(unsigned c)
{
    __asm__ volatile("mtc0 %0, $13" : : "r"(c));
}

static unsigned rd_status(void)
{
    unsigned s;
    __asm__ volatile("mfc0 %0, $12" : "=r"(s));
    return s;
}

static void wr_status(unsigned s)
{
    __asm__ volatile("mtc0 %0, $12; ehb" : : "r"(s));
}

/* Many values at once, in as many registers as the allocator will give
 * them, and HI/LO through the multiplies and divisions. */
static void busy(void)
{
    long a = vv[0], b = vv[1], c = vv[2], d = vv[3], e = vv[4], f = vv[5],
         g = vv[6], h = vv[7], i = a * b, j = c * d, k = e * f, l = g * h,
         m = a ^ h, n = b + g, o = c - f, p = d | e;
    sink = (unsigned long)(a * i + b * j + c * k + d * l + e * m + f * n +
           g * o + h * p + i * j + k * l + m * n + o * p +
           (a + b + c + d) / (e - 3) + (i ^ j ^ k ^ l) % 9 +
           (m + n + o + p) * (f - g));
}

/* Is this the timer (an interrupt, line 7 pending)? */
static int is_timer(void)
{
    unsigned c = rd_cause();
    return ((c >> 2) & 31) == 0 && (c & (1u << 15));
}

__attribute__((interrupt, keep_interrupts_masked)) void tick_masked(void)
{
    if (!is_timer())
        unexpected++;
    ticks++;
    wr_compare(rd_count() + PERIOD);           /* re-arm, and acknowledge */
    busy();
}

__attribute__((noinline)) void callee(void)
{
    busy();
}

__attribute__((interrupt("vector=hw5"))) void tick_call(void)
{
    ticks++;
    wr_compare(rd_count() + PERIOD);
    clobber_all();
    callee();
}

/* Phase C: the timer, and software interrupt 0 on every other tick */
__attribute__((interrupt("vector=hw5"))) void tick_raise(void)
{
    if (!is_timer())
        unexpected++;
    ticks++;
    wr_compare(rd_count() + PERIOD);
    if (ticks & 1)
        wr_cause(rd_cause() | (1u << 8));      /* IP0 */
}

/* ...and its handler, which the timer may interrupt */
__attribute__((interrupt("vector=sw0"))) void soft0(void)
{
    unsigned long t0 = ticks;
    unsigned c = rd_cause();
    if (((c >> 2) & 31) != 0 || !(c & (1u << 8)))
        unexpected++;
    wr_cause(c & ~(1u << 8));
    softs++;
    /* until the timer has interrupted this handler (if it can), and at
     * most two periods or so */
    for (int k = 0; k < 400 && ticks == t0; k++)
        busy();
    if (ticks != t0)
        nested++;
}

/* many values live across the loop, and HI/LO through the multiplies */
static __attribute__((noinline)) unsigned churn(unsigned n)
{
    unsigned a = 1, b = 2, c = 3, d = 4, e = 5, f = 6, g = 7, h = 8;
    unsigned i = 9, j = 10, k = 11, l = 12;
    for (unsigned q = 0; q < n; q++) {
        a = a * 1103515245u + 12345u;
        b ^= a >> 3;
        c += b * 7u;
        d = (d << 5) ^ c ^ (d >> 2);
        e += d * a;
        f -= e ^ q;
        g = g * 31u + f;
        h += (unsigned)((unsigned long long)g * a >> 32);
        i ^= h + e;
        j += i * 3 + c;
        k = k * 5 + (j >> 1);
        l += k ^ b;
    }
    return a ^ b ^ c ^ d ^ e ^ f ^ g ^ h ^ i ^ j ^ k ^ l;
}

/* lui k0, %hi(h) ; ori k0, k0, %lo(h) ; jr k0 ; nop -- at a vector */
static void install(unsigned long at, void (*h)(void))
{
    volatile unsigned *v = (volatile unsigned *)at;
    unsigned a = (unsigned)(unsigned long)h;
    v[0] = 0x3c1a0000u | (a >> 16);
    v[1] = 0x375a0000u | (a & 0xffffu);
    v[2] = 0x03400008u;
    v[3] = 0;
}

static void phase(const char *name, unsigned want, int c)
{
    long bad[2] = { -1, -1 };
    unsigned long t0 = ticks, s0 = softs;
    torture(ROUNDS, bad);
    int ok = churn(40000) == want;
    puts_(name);
    putn(bad[0]);
    putn(bad[1]);
    putn(ok);
    putn(c ? softs - s0 >= WANT / 2 && nested > 0 : ticks - t0 >= WANT);
    puts_("\n");
}

int main(void)
{
    unsigned want = churn(40000);
    unsigned long na, nb;

    /* A: masked, at the general exception vector (EBase + 0x180) */
    install(0x80000180u, tick_masked);
    wr_compare(rd_count() + PERIOD);
    wr_status(rd_status() | (1u << 15) | 1u);          /* IM7, IE */
    phase("A ", want, 0);
    na = ticks;

    /* B: one that calls */
    wr_status(rd_status() & ~1u);
    install(0x80000180u, tick_call);
    wr_status(rd_status() | 1u);
    phase("B ", want, 0);
    nb = ticks - na;

    /* C: vectored -- IntCtl.VS = 32 bytes, Cause.IV: line n's vector at
     * EBase + 0x200 + 32n */
    wr_status(rd_status() & ~1u);
    install(0x80000200u, soft0);
    install(0x80000200u + 7 * 32, tick_raise);
    {
        unsigned ic;
        __asm__ volatile("mfc0 %0, $12, 1" : "=r"(ic));
        ic = (ic & ~(0x1fu << 5)) | (1u << 5);
        __asm__ volatile("mtc0 %0, $12, 1" : : "r"(ic));
    }
    wr_cause(rd_cause() | (1u << 23));
    wr_status(rd_status() | (1u << 8) | 1u);           /* IM0, IE */
    phase("C ", want, 1);
    wr_status(rd_status() & ~1u);
    wr_compare(rd_count() - 1);

    putn((long)na);
    putn((long)nb);
    putn((long)(ticks - na - nb));
    putn((long)softs);
    putn((long)nested);
    putn((long)unexpected);
    puts_("\n");
    return 42;
}
