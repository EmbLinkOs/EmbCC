/* Eight-byte atomics against an interrupt (tests/golden/atomic8-race.sh):
 * the Cortex-M's SysTick or RV32's machine timer interrupts main every few
 * dozen instructions, under -icount, so it lands inside the library's
 * routines thousands of times.
 *
 *   race    the handler adds 1 to an eight-byte counter with a plain
 *           read-modify-write while main fetch_adds 3, adds 5 through the
 *           operator and multiplies by 1 through a compare-exchange loop:
 *           the total must be exact
 *   loads   the handler writes a value whose halves are equal; main's
 *           atomic loads must never see them differ
 *   stores  main atomically stores such values; the handler must never
 *           see one torn
 *   nested  with interrupts masked by the caller, an atomic must leave
 *           them masked: the routines restore the mask, not enable
 *   ticks   the interrupts kept coming after the routines ran
 * A line each: the name, then 1. */
void putn(long v);
void puts_(const char *s);

typedef unsigned long long u64;
typedef unsigned int u32;

static volatile u64 c64, pat;
static _Atomic u64 ac;
static volatile u32 ticks, mode, torn_isr;

static void tick_body(void)
{
    ticks++;
    c64 = c64 + 1;
    ac = ac;                    /* (an atomic in the handler too) */
    if (mode == 1) {
        u32 k = ticks * 0x01010101u;
        pat = ((u64)k << 32) | k;
    } else if (mode == 2) {
        u64 v = pat;
        if ((u32)(v >> 32) != (u32)v)
            torn_isr++;
    }
}

#if defined(__arm__)
#define VTOR   (*(volatile u32 *)0xE000ED08u)
#define STCTRL (*(volatile u32 *)0xE000E010u)
#define STLOAD (*(volatile u32 *)0xE000E014u)
#define STVAL  (*(volatile u32 *)0xE000E018u)
static void (*vt[64])(void) __attribute__((aligned(256)));
static void tick(void) { tick_body(); }
static void timer_on(void)
{
    for (int k = 0; k < 64; k++) vt[k] = ((void (**)(void))VTOR)[k];
    vt[15] = tick;
    VTOR = (u32)vt;
    STLOAD = 97;
    STVAL = 0;
    STCTRL = 7;
}
static void timer_off(void) { STCTRL = 0; }
static void mask(void) { __asm__ volatile("cpsid i" ::: "memory"); }
static void unmask(void) { __asm__ volatile("cpsie i" ::: "memory"); }
static int masked(void)
{
    u32 m;
    __asm__ volatile("mrs %0, primask" : "=r"(m));
    return m & 1;
}
#else
#define MTIMECMP_LO (*(volatile unsigned *)0x2004000u)
#define MTIMECMP_HI (*(volatile unsigned *)0x2004004u)
#define MTIME_LO    (*(volatile unsigned *)0x200bff8u)
static void rearm(void)
{
    MTIMECMP_HI = 0xffffffffu;
    MTIMECMP_LO = MTIME_LO + 1;
    MTIMECMP_HI = 0;
}
__attribute__((interrupt)) void tick(void)
{
    tick_body();
    rearm();
}
static void timer_on(void)
{
    __asm__ volatile("csrw mtvec, %0" : : "r"(tick));
    rearm();
    __asm__ volatile("csrs mie, %0" : : "r"(0x80u));
    __asm__ volatile("csrs mstatus, %0" : : "r"(8u) : "memory");
}
static void timer_off(void)
{
    __asm__ volatile("csrc mstatus, %0" : : "r"(8u) : "memory");
    MTIMECMP_HI = 0xffffffffu;
}
static void mask(void) { __asm__ volatile("csrc mstatus, %0" : : "r"(8u) : "memory"); }
static void unmask(void) { __asm__ volatile("csrs mstatus, %0" : : "r"(8u) : "memory"); }
static int masked(void)
{
    u32 m;
    __asm__ volatile("csrr %0, mstatus" : "=r"(m));
    return !(m & 8);
}
#endif

static void line(const char *name, int ok)
{
    puts_(name);
    putn(ok);
    puts_("\n");
}

int main(void)
{
    const u32 n = 20000;
    u32 t0;
    timer_on();

    for (u32 k = 0; k < n; k++) {
        __atomic_fetch_add((u64 *)&c64, 3, __ATOMIC_SEQ_CST);
        ac += 5;
        ac *= 1;
    }
    mask();
    t0 = ticks;
    line("race ", t0 > 1000 && c64 == 3ull * n + t0 && ac == 5ull * n);
    unmask();

    u32 torn = 0;
    mode = 1;
    for (u32 k = 0; k < n; k++) {
        u64 v = __atomic_load_n((u64 *)&pat, __ATOMIC_ACQUIRE);
        if ((u32)(v >> 32) != (u32)v)
            torn++;
    }
    mode = 0;
    line("loads ", torn == 0);

    mode = 2;
    for (u32 k = 0; k < n; k++) {
        u32 j = k * 0x00010001u;
        __atomic_store_n((u64 *)&pat, ((u64)~j << 32) | ~j, __ATOMIC_RELEASE);
    }
    mode = 0;
    line("stores ", torn_isr == 0);

    mask();
    __atomic_fetch_add((u64 *)&c64, 1, __ATOMIC_SEQ_CST);
    (void)__atomic_load_n((u64 *)&c64, __ATOMIC_SEQ_CST);
    int still = masked();
    unmask();
    line("nested ", still);

    t0 = ticks;
    for (volatile u32 k = 0; k < 2000000 && ticks < t0 + 10; k++)
        ;
    line("ticks ", ticks >= t0 + 10);
    timer_off();
    puts_("DONE\n");
    return 0;
}
