/* RISC-V interrupt handlers in C, interrupting code that has a value in
 * every register (tests/golden/riscv-isr.sh), on QEMU's virt board.
 *
 * The CLINT's machine timer (mtime/mtimecmp) interrupts main in three
 * phases, each with a handler compiled by EmbCC:
 *
 *   A  tick_leaf, __attribute__((interrupt)): a machine-mode handler that
 *      calls nothing and works in many registers -- integer and, with an
 *      FPU, floating point. It must save exactly what it writes.
 *   B  tick_call, interrupt("machine"): calls clobber_all(), which writes
 *      every caller-saved register as the ABI allows a callee to, and a C
 *      function. It must save every caller-saved register.
 *   D  tick_big: a machine-mode handler whose frame is too large for
 *      one addi (3000 bytes), so it is made and released in two steps.
 *   C  main drops to supervisor mode. tick_fwd (machine) re-arms the timer
 *      and raises the supervisor software interrupt, delegated to S mode,
 *      which ssoft, interrupt("supervisor"), takes -- at whatever point
 *      of main's code the timer arrived -- and returns from with sret.
 *
 * In each phase main runs torture() (torture.S: a pattern in every
 * caller-saved register, checked over and over; a bit per register that
 * changed) and churn(), a C computation with many values live whose
 * result must equal the same computation with interrupts off. A line per
 * phase: its letter, the integer and floating-point registers that
 * changed (0 and 0), whether churn agreed, and whether enough interrupts
 * arrived; the counts follow on a line of their own. */
void putn(long v);
void puts_(const char *s);

void torture(long n, long *bad);
void clobber_all(void);

#define MTIMECMP_LO (*(volatile unsigned *)0x2004000u)
#define MTIMECMP_HI (*(volatile unsigned *)0x2004004u)
#define MTIME_LO    (*(volatile unsigned *)0x200bff8u)

#define PERIOD 37u              /* mtime ticks: ~3700 instructions */
#define ROUNDS 20000L           /* torture's checks of every register */
#define WANT   200UL            /* interrupts a phase must see at least */

volatile unsigned long ticks, sticks, unexpected, sink;
volatile long vv[8] = { 3, 5, 7, 11, 13, 17, 19, 23 };
#ifdef __riscv_flen
volatile float fv[4] = { 1.5f, 2.25f, -3.125f, 0.75f };
volatile float fsink;
#endif

static void rearm(void)
{
    MTIMECMP_HI = 0xffffffffu;
    MTIMECMP_LO = MTIME_LO + PERIOD;
    MTIMECMP_HI = 0;
}

static void stop_timer(void)
{
    MTIMECMP_HI = 0xffffffffu;
}

#if __riscv_xlen == 64
#define MCAUSE_TIMER 0x8000000000000007UL
#define SCAUSE_SOFT  0x8000000000000001UL
#else
#define MCAUSE_TIMER 0x80000007UL
#define SCAUSE_SOFT  0x80000001UL
#endif

static unsigned long rd_mcause(void)
{
    unsigned long c;
    __asm__ volatile("csrr %0, mcause" : "=r"(c));
    return c;
}

/* Many values at once, in as many registers as the allocator will give
 * them: what a handler writes and must put back. */
static void busy(void)
{
    long a = vv[0], b = vv[1], c = vv[2], d = vv[3], e = vv[4], f = vv[5],
         g = vv[6], h = vv[7], i = a * b, j = c * d, k = e * f, l = g * h,
         m = a ^ h, n = b + g, o = c - f, p = d | e;
    sink = a * i + b * j + c * k + d * l + e * m + f * n + g * o + h * p +
           i * j + k * l + m * n + o * p + (a + b + c + d) / (e - 3) +
           (i ^ j ^ k ^ l) % 9 + (m + n + o + p) * (f - g);
#ifdef __riscv_flen
    {
        float w = fv[0], x = fv[1], y = fv[2], z = fv[3];
        float q = w * x, r = y * z, s = w + z, t = x - y;
        fsink = q * r + s * t + (w - x) * (y + z) + q / (s + 7.0f) +
                r * t * w - z * x * s;
    }
#endif
}

__attribute__((interrupt)) void tick_leaf(void)
{
    if (rd_mcause() != MCAUSE_TIMER)
        unexpected++;
    ticks++;
    rearm();
    busy();
}

/* (noinline: a call is what this phase is about) */
__attribute__((noinline)) void callee(void)
{
    busy();
}

__attribute__((interrupt("machine"))) void tick_call(void)
{
    ticks++;
    rearm();
    clobber_all();
    callee();
}

/* Phase D: a frame past addi's reach, which is made in two steps -- the
 * saves first, then the rest through t0 -- and released the same way. */
__attribute__((interrupt)) void tick_big(void)
{
    volatile char buf[3000];
    ticks++;
    rearm();
    buf[0] = (char)ticks;
    buf[2999] = (char)(ticks >> 1);
    sink = (unsigned long)(buf[0] + buf[2999]);
    busy();
}

/* Phase C's machine half: re-arm, and hand the moment to S mode. */
__attribute__((interrupt("machine"))) void tick_fwd(void)
{
    ticks++;
    rearm();
    __asm__ volatile("csrs mip, %0" : : "r"(2UL));          /* SSIP */
}

__attribute__((interrupt("supervisor"))) void ssoft(void)
{
    unsigned long c;
    __asm__ volatile("csrr %0, scause" : "=r"(c));
    if (c != SCAUSE_SOFT)
        unexpected++;
    __asm__ volatile("csrc sip, %0" : : "r"(2UL));
    sticks++;
    busy();
    clobber_all();
}

/* many values live across the loop, and through mul/div */
static __attribute__((noinline)) unsigned long churn(unsigned long n)
{
    unsigned long a = 1, b = 2, c = 3, d = 4, e = 5, f = 6, g = 7, h = 8;
    unsigned long i = 9, j = 10, k = 11, l = 12;
    for (unsigned long q = 0; q < n; q++) {
        a = a * 1103515245u + 12345u;
        b ^= a >> 3;
        c += b * 7u;
        d = (d << 5) ^ c ^ (d >> 2);
        e += d * a;
        f -= e ^ q;
        g = g * 31u + f;
        h += g / (a | 1);
        i ^= h + e;
        j += i * 3 + c;
        k = k * 5 + (j >> 1);
        l += k ^ b;
    }
    return a ^ b ^ c ^ d ^ e ^ f ^ g ^ h ^ i ^ j ^ k ^ l;
}

#ifdef __riscv_flen
static __attribute__((noinline)) float fchurn(int n)
{
    float a = 1.0f, b = 0.5f, c = 0.25f, d = 2.0f, e = 1.5f, f = 0.75f;
    for (int q = 0; q < n; q++) {
        a = a * 0.999f + b;
        b = b * 0.5f + c * 0.25f;
        c = c + d * 0.001f;
        d = d * 0.9f + e * 0.1f;
        e = e - f * 0.01f + a * 0.001f;
        f = f * 0.99f + 0.01f;
    }
    return a + b + c + d + e + f;
}
#endif

static void set_mtvec(void (*h)(void))
{
    __asm__ volatile("csrw mtvec, %0" : : "r"(h));
}

/* One phase: torture and churn with the interrupts on. */
static void phase(const char *name, unsigned long want, float fwant, int s)
{
    long bad[2] = { -1, -1 };
    unsigned long t0 = ticks, s0 = sticks;
    int ok;
    (void)fwant;
    torture(ROUNDS, bad);
    ok = churn(30000) == want;
#ifdef __riscv_flen
    ok = ok && fchurn(30000) == fwant;
#endif
    puts_(name);
    putn(bad[0]);
    putn(bad[1]);
    putn(ok);
    putn(s ? sticks - s0 >= WANT : ticks - t0 >= WANT);
    puts_("\n");
}

static unsigned long want, na, nb, nd;
static float fwant;

/* Phase C, entered by mret in supervisor mode; it ends the run itself
 * (the SiFive test device, as tests/harness/riscv/boot.c does), there
 * being no machine-mode caller to return to. */
static __attribute__((noreturn)) void s_main(void)
{
    phase("C ", want, fwant, 1);
    stop_timer();
    __asm__ volatile("csrc sstatus, %0" : : "r"(2UL));
    putn((long)na);
    putn((long)nb);
    putn((long)nd);
    putn((long)sticks);
    putn((long)unexpected);
    puts_("\n");
    *(volatile unsigned *)0x100000u = 0x5555u;
    for (;;)
        ;
}

int main(void)
{
    want = churn(30000);
#ifdef __riscv_flen
    fwant = fchurn(30000);
#endif

    /* A: a leaf machine-mode handler */
    set_mtvec(tick_leaf);
    rearm();
    __asm__ volatile("csrs mie, %0" : : "r"(0x80UL));        /* MTIE */
    __asm__ volatile("csrs mstatus, %0" : : "r"(8UL));       /* MIE */
    phase("A ", want, fwant, 0);
    na = ticks;

    /* B: one that calls */
    set_mtvec(tick_call);
    phase("B ", want, fwant, 0);
    nb = ticks - na;

    /* D: a large frame */
    set_mtvec(tick_big);
    phase("D ", want, fwant, 0);
    nd = ticks - na - nb;

    /* C: main in supervisor mode, the timer forwarded to ssoft */
    __asm__ volatile("csrc mstatus, %0" : : "r"(8UL));       /* MIE off */
    set_mtvec(tick_fwd);
    __asm__ volatile("csrw stvec, %0" : : "r"(ssoft));
    __asm__ volatile("csrw mideleg, %0" : : "r"(2UL));       /* SSIP to S */
    __asm__ volatile("csrs sie, %0" : : "r"(2UL));           /* SSIE */
    __asm__ volatile("csrs sstatus, %0" : : "r"(2UL));       /* SIE */
    /* S mode reaches memory only through a PMP entry: all of it */
    __asm__ volatile("csrw pmpaddr0, %0" : : "r"(-1L));
    __asm__ volatile("csrw pmpcfg0, %0" : : "r"(0x1fUL));
    /* mret into S mode at s_main: MPP = 01 */
    __asm__ volatile("csrc mstatus, %0" : : "r"(0x1800UL));
    __asm__ volatile("csrs mstatus, %0" : : "r"(0x0800UL));
    __asm__ volatile("csrw mepc, %0" : : "r"(s_main));
    __asm__ volatile("mret");
    return 1;
}
