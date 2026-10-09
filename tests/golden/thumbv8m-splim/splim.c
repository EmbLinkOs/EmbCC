/* The ARMv8-M stack-limit registers, written and read with inline asm,
 * and enforced by the core: tests/golden/thumbv8m-splim.sh runs this on
 * QEMU's mps2-an505 (a Cortex-M33). A thread on the PSP recurses past
 * PSPLIM; the core must stop it with a UsageFault whose CFSR says STKOF,
 * having never let the PSP go below the limit. MSPLIM is written and read
 * back the same way. This is the protection an RTOS gives each task's
 * stack on ARMv8-M. Output and the exit go through semihosting, and the
 * startup is this file's own, since it needs a UsageFault vector. */
#define REG(a) (*(volatile unsigned *)(a))
#define SHCSR REG(0xE000ED24u)
#define CFSR  REG(0xE000ED28u)
#define STKOF (1u << 20)

extern unsigned __data_load, __data_start, __data_end, __bss_start, __bss_end;
void reset(void);
void fault(void);
void usage_fault(void);

#define MSP_TOP 0x10200000u

__attribute__((section(".vectors"), used))
void (*const vectors[16])(void) = {
    (void (*)(void))MSP_TOP, reset, fault, fault, fault, fault, usage_fault,
    fault, 0, 0, 0, fault, fault, 0, fault, fault,
};

static void semi(unsigned op, const void *arg)
{
    __asm__ volatile("mov r0, %0\n\tmov r1, %1\n\tbkpt #0xab"
                     : : "r"(op), "r"(arg) : "r0", "r1", "memory");
}

static void say(const char *s) { semi(4, s); }

static void sayx(unsigned v)
{
    char b[11];
    b[0] = '0';
    b[1] = 'x';
    for (int i = 0; i < 8; i++)
        b[2 + i] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 15];
    b[10] = 0;
    say(b);
}

static void leave(unsigned status)
{
    static volatile unsigned blk[2];
    blk[0] = 0x20026u;
    blk[1] = status;
    semi(0x20, (const void *)blk);
    for (;;)
        ;
}

static unsigned task_stack[256] __attribute__((aligned(8)));
#define LIMIT ((unsigned)&task_stack[64])   /* 768 bytes above the base */

void fault(void)
{
    say("unexpected exception\n");
    leave(1);
}

void usage_fault(void)
{
    unsigned psp;
    __asm__ volatile("mrs %0, psp" : "=r"(psp));
    say("usagefault");
    if (CFSR & STKOF)
        say(" STKOF");
    /* the core stops the push that would cross the limit: the PSP stays
     * at or above it */
    say(psp >= LIMIT ? " psp at or above psplim\n" : " PSP BELOW PSPLIM\n");
    leave(CFSR & STKOF && psp >= LIMIT ? 0 : 2);
}

static int deep(int n)
{
    volatile char b[64];
    b[0] = (char)n;
    return n ? deep(n - 1) + b[0] : 0;
}

int main(void)
{
    unsigned v, m = MSP_TOP - 0x8000u;

    __asm__ volatile("msr msplim, %0" : : "r"(m));
    __asm__ volatile("mrs %0, msplim" : "=r"(v));
    say("msplim ");
    sayx(v);
    say(v == m ? " read back\n" : " WRONG\n");

    __asm__ volatile("msr psplim, %0" : : "r"(LIMIT));
    __asm__ volatile("mrs %0, psplim" : "=r"(v));
    say("psplim ");
    say(v == LIMIT ? "read back\n" : "WRONG\n");

    SHCSR |= 1u << 18;                  /* UsageFault enabled */
    unsigned top = (unsigned)&task_stack[256], ctl = 2;
    __asm__ volatile("msr psp, %0" : : "r"(top));
    __asm__ volatile("msr control, %0" : : "r"(ctl));
    __asm__ volatile("isb");
    say("recursing on the PSP\n");
    (void)deep(1000);                   /* 64 KiB of frames in 1 KiB */
    say("no fault: the limit was not enforced\n");
    leave(3);
    return 0;
}

void reset(void)
{
    unsigned *d = &__data_start, *s = &__data_load;
    while (d < &__data_end)
        *d++ = *s++;
    for (d = &__bss_start; d < &__bss_end; d++)
        *d = 0;
    leave((unsigned)main());
}
