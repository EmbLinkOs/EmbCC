/* su.c -- the constructs whose frames tests/golden/embsim-stack.sh holds
 * -fstack-usage to, against what EmbSim measures, on each core in its
 * harness:
 *
 *   atomics of 1, 2, 4 and 8 bytes    the AVR pushes their operands and
 *                                     pops them back inside the access
 *   a 64-bit multiply by a constant   at -Os the AVR loads the constant's
 *                                     bytes into r10-r17 through a pushed r31
 *   a large struct, by value          passed and returned in memory
 *   many arguments, varargs           stack-passed
 *   an interrupt handler that calls   on RISC-V it saves every caller-saved
 *                                     register, on the AVR seventeen
 *   a variable-length array, alloca   "dynamic": no frame bounds them
 *
 * It prints a checksum and the ticks taken. */
void putn(long v);

volatile int sink;
volatile int ticks;
volatile long long sink64;

static unsigned char a8;
static unsigned short a16;
static unsigned long a32;
static unsigned long long a64;

__attribute__((noinline)) long at8(unsigned char v)
{
    unsigned char e = 0;
    long s = __atomic_fetch_add(&a8, v, __ATOMIC_SEQ_CST);
    s += __atomic_exchange_n(&a8, v, __ATOMIC_SEQ_CST);
    s += __atomic_compare_exchange_n(&a8, &e, v, 0, __ATOMIC_SEQ_CST,
                                     __ATOMIC_SEQ_CST);
    return s + __sync_val_compare_and_swap(&a8, v, 7);
}

__attribute__((noinline)) long at16(unsigned short v)
{
    unsigned short e = 0;
    long s = __atomic_fetch_and(&a16, v, __ATOMIC_SEQ_CST);
    s += __atomic_compare_exchange_n(&a16, &e, v, 0, __ATOMIC_SEQ_CST,
                                     __ATOMIC_SEQ_CST);
    return s + __sync_val_compare_and_swap(&a16, v, 9);
}

__attribute__((noinline)) long at32(unsigned long v)
{
    unsigned long e = 3;
    long s = (long)__atomic_fetch_or(&a32, v, __ATOMIC_SEQ_CST);
    s += __atomic_compare_exchange_n(&a32, &e, v, 0, __ATOMIC_SEQ_CST,
                                     __ATOMIC_SEQ_CST);
    return s + (long)__sync_val_compare_and_swap(&a32, v, 11ul);
}

__attribute__((noinline)) long at64(unsigned long long v)
{
    unsigned long long e = 5;
    unsigned long long s = __atomic_fetch_add(&a64, v, __ATOMIC_SEQ_CST);
    s += __atomic_exchange_n(&a64, v, __ATOMIC_SEQ_CST);
    s += __atomic_compare_exchange_n(&a64, &e, v, 0, __ATOMIC_SEQ_CST,
                                     __ATOMIC_SEQ_CST);
    s += __sync_val_compare_and_swap(&a64, v, 13ull);
    return (long)(s ^ (s >> 32));
}

__attribute__((noinline)) long long mulk(long long x)
{
    return x * 1000000007LL;
}

struct big { long w[12]; };

__attribute__((noinline)) struct big mkbig(int x)
{
    struct big b;
    for (int i = 0; i < 12; i++)
        b.w[i] = x + i;
    return b;
}

__attribute__((noinline)) long sumbig(struct big b)
{
    long s = 0;
    for (int i = 0; i < 12; i++)
        s += b.w[i];
    return s;
}

__attribute__((noinline)) long many(long a, long b, long c, long d, long e,
                                    long f, long g, long h, long i, long j)
{
    return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8 + i * 9 +
           j * 10;
}

__attribute__((noinline)) long va(int n, ...)
{
    __builtin_va_list ap;
    long s = 0;
    __builtin_va_start(ap, n);
    for (int i = 0; i < n; i++)
        s += __builtin_va_arg(ap, long);
    __builtin_va_end(ap);
    return s;
}

__attribute__((noinline)) int vla(int n)
{
    volatile char buf[n];
    for (int i = 0; i < n; i++)
        buf[i] = (char)i;
    return buf[n / 2];
}

__attribute__((noinline)) int dyn(int n)
{
    volatile char *p = __builtin_alloca(n);
    for (int i = 0; i < n; i++)
        p[i] = (char)(i + 1);
    return p[n - 1];
}

__attribute__((noinline)) void bump(void)
{
    ticks++;
}

__attribute__((noinline)) void spin(void)
{
    for (int i = 0; i < 10; i++)
        sink = i;
}

#if defined(__arm__) || defined(__thumb__)
__attribute__((interrupt)) void tick(void)
{
    bump();
    sink64 = sink64 * 3 + 1;
}

static void (*vtab[16])(void) __attribute__((aligned(128)));

static void timer_start(void)
{
    vtab[15] = tick;
    *(void (***)(void))0xE000ED08u = vtab;          /* VTOR */
    *(volatile unsigned *)0xE000E014u = 499;        /* RVR */
    *(volatile unsigned *)0xE000E018u = 0;          /* CVR */
    *(volatile unsigned *)0xE000E010u = 7;          /* CSR: on, TICKINT */
}

static void timer_stop(void)
{
    *(volatile unsigned *)0xE000E010u = 0;
}
#elif defined(__riscv)
#define MTIMECMP ((volatile unsigned *)0x2004000u)
#define MTIME ((volatile unsigned *)0x200bff8u)

__attribute__((interrupt("machine"), aligned(4))) void tick(void)
{
    unsigned t = MTIME[0] + 500;
    bump();
    sink64 = sink64 * 3 + 1;
    MTIMECMP[1] = 0xffffffffu;
    MTIMECMP[0] = t;
    MTIMECMP[1] = 0;
}

static void timer_start(void)
{
    __asm__ volatile("csrw mtvec, %0" : : "r"(tick));
    MTIMECMP[1] = 0xffffffffu;
    MTIMECMP[0] = MTIME[0] + 500;
    MTIMECMP[1] = 0;
    __asm__ volatile("csrs mie, %0" : : "r"(0x80));     /* MTIE */
    __asm__ volatile("csrs mstatus, %0" : : "r"(8));    /* MIE */
}

static void timer_stop(void)
{
    __asm__ volatile("csrc mstatus, %0" : : "r"(8));
    __asm__ volatile("csrc mie, %0" : : "r"(0x80));
}
#else
/* Timer/Counter1's compare match A, CTC on OCR1A, at the core's clock */
__attribute__((signal)) void __vector_11(void)
{
    bump();
    sink64 = sink64 * 3 + 1;
}

static void timer_start(void)
{
    *(volatile unsigned char *)0x89 = 1;            /* OCR1AH: 500 */
    *(volatile unsigned char *)0x88 = 0xf4;         /* OCR1AL */
    *(volatile unsigned char *)0x80 = 0;            /* TCCR1A */
    *(volatile unsigned char *)0x6f = 2;            /* TIMSK1: OCIE1A */
    *(volatile unsigned char *)0x81 = 9;            /* TCCR1B: CTC, /1 */
    __asm__ volatile("sei");
}

static void timer_stop(void)
{
    __asm__ volatile("cli");
    *(volatile unsigned char *)0x81 = 0;
    *(volatile unsigned char *)0x6f = 0;
}
#endif

int main(void)
{
    long s = 0;
    s += at8(3);
    s += at16(5);
    s += at32(7);
    s += at64(9);
    s += (long)mulk(sink + 3);
    s += sumbig(mkbig(sink));
    s += many(1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
    s += va(4, 1L, 2L, 3L, 4L);
    s += vla(sink + 20);
    s += dyn(sink + 30);
    timer_start();
    while (ticks < 3)
        spin();
    timer_stop();
    putn(s);
    putn(ticks);
    return 0;
}
