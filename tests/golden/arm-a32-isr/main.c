/* ARMv7-A interrupt handlers, run: the generic timer's virtual interrupt
 * (PPI 27, through the GICv2 of QEMU's virt board) interrupts code that
 * keeps a pattern in r0-r12, hundreds of times, and a C computation --
 * first with interrupt("IRQ") handler that only counts (a leaf), then with
 * one that calls a function doing 64-bit arithmetic and a division (a
 * helper call), which may change r0-r3 and r12. A handler that left one
 * changed, returned to lr instead of lr - 4 (the interrupted instruction
 * would be skipped), or returned without restoring CPSR from SPSR (it
 * would stay in IRQ mode, on the IRQ stack), shows up in torture's result,
 * the computation's value, or a fault. */
#include <stdio.h>

#define GICD        0x08000000u
#define GICC        0x08010000u
#define REG(a)      (*(volatile unsigned *)(a))
#define REG8(a)     (*(volatile unsigned char *)(a))
#define VTIMER_PPI  27

volatile unsigned ticks, extra, period;
static long long acc = 1;

static __attribute__((noinline)) unsigned work(unsigned x)
{
    acc = acc * 7 + x;
    return (unsigned)(acc / 13) ^ x;
}

static void timer_arm(unsigned t)
{
    __asm__ volatile ("mcr p15, #0, %0, c14, c3, #0" : : "r"(t));   /* CNTV_TVAL */
}

/* both acknowledge the GIC, re-arm the timer, and end the interrupt */
void __attribute__((interrupt("IRQ"))) tick_leaf(void)
{
    unsigned iar = REG(GICC + 0x0c);
    if ((iar & 0x3ff) == VTIMER_PPI) {
        timer_arm(period);
        ticks++;
    }
    REG(GICC + 0x10) = iar;
}

void __attribute__((interrupt("IRQ"))) tick_calls(void)
{
    unsigned iar = REG(GICC + 0x0c);
    if ((iar & 0x3ff) == VTIMER_PPI) {
        timer_arm(period);
        ticks++;
        extra += work(ticks);
    }
    REG(GICC + 0x10) = iar;
}

static void (*volatile irq_handler)(void);
static void fault(void) { puts("\n==FAULT not an IRQ==");
                          for (;;) ; }

/* the vector table: eight `ldr pc, [pc, #24]`, each loading its handler
 * from the word 32 bytes on; VBAR wants it 32-aligned */
static unsigned vectors[16] __attribute__((aligned(32)));
static unsigned irq_stack[512] __attribute__((aligned(8)));

/* r0 iterations with a pattern in r0-r12 (lr the scratch the checks build
 * a pattern in, the count on the stack); 0, or 1 when one changed */
unsigned torture(unsigned n);
__asm__(
"    .global torture\n"
"    .type torture, %function\n"
"torture:\n"
"    push {r4-r11, lr}\n"
"    push {r0}\n"
"    movw r0, #0x0100\n    movt r0, #0x5100\n"
"    movw r1, #0x0101\n    movt r1, #0x5101\n"
"    movw r2, #0x0202\n    movt r2, #0x5202\n"
"    movw r3, #0x0303\n    movt r3, #0x5303\n"
"    movw r4, #0x0404\n    movt r4, #0x5404\n"
"    movw r5, #0x0505\n    movt r5, #0x5505\n"
"    movw r6, #0x0606\n    movt r6, #0x5606\n"
"    movw r7, #0x0707\n    movt r7, #0x5707\n"
"    movw r8, #0x0808\n    movt r8, #0x5808\n"
"    movw r9, #0x0909\n    movt r9, #0x5909\n"
"    movw r10, #0x0a0a\n   movt r10, #0x5a0a\n"
"    movw r11, #0x0b0b\n   movt r11, #0x5b0b\n"
"    movw r12, #0x0c0c\n   movt r12, #0x5c0c\n"
"1:  movw lr, #0x0100\n    movt lr, #0x5100\n    cmp r0, lr\n    bne 2f\n"
"    movw lr, #0x0101\n    movt lr, #0x5101\n    cmp r1, lr\n    bne 2f\n"
"    movw lr, #0x0202\n    movt lr, #0x5202\n    cmp r2, lr\n    bne 2f\n"
"    movw lr, #0x0303\n    movt lr, #0x5303\n    cmp r3, lr\n    bne 2f\n"
"    movw lr, #0x0404\n    movt lr, #0x5404\n    cmp r4, lr\n    bne 2f\n"
"    movw lr, #0x0505\n    movt lr, #0x5505\n    cmp r5, lr\n    bne 2f\n"
"    movw lr, #0x0606\n    movt lr, #0x5606\n    cmp r6, lr\n    bne 2f\n"
"    movw lr, #0x0707\n    movt lr, #0x5707\n    cmp r7, lr\n    bne 2f\n"
"    movw lr, #0x0808\n    movt lr, #0x5808\n    cmp r8, lr\n    bne 2f\n"
"    movw lr, #0x0909\n    movt lr, #0x5909\n    cmp r9, lr\n    bne 2f\n"
"    movw lr, #0x0a0a\n    movt lr, #0x5a0a\n    cmp r10, lr\n   bne 2f\n"
"    movw lr, #0x0b0b\n    movt lr, #0x5b0b\n    cmp r11, lr\n   bne 2f\n"
"    movw lr, #0x0c0c\n    movt lr, #0x5c0c\n    cmp r12, lr\n   bne 2f\n"
"    ldr lr, [sp]\n"
"    subs lr, lr, #1\n"
"    str lr, [sp]\n"
"    bne 1b\n"
"    mov r0, #0\n"
"    b 3f\n"
"2:  mov r0, #1\n"
"3:  add sp, sp, #4\n"
"    pop {r4-r11, pc}\n");

/* a computation whose every intermediate is in registers at -O2 */
static __attribute__((noinline)) unsigned long long mix(unsigned n)
{
    unsigned long long h = 0x9e3779b97f4a7c15ull;
    unsigned a = 1, b = 2, c = 3, d = 4, e = 5;
    for (unsigned i = 0; i < n; i++) {
        a += b ^ i; b = (b << 3) | (b >> 29); c += a * 5;
        d ^= c + e; e = e * 3 + d; h = h * 31 + (a ^ b ^ c ^ d ^ e);
    }
    return h;
}

static int phase(const char *name, void (*h)(void), unsigned p,
                 unsigned long long want)
{
    unsigned bad = 0, wrong = 0, n, rounds = 0;
    vectors[8 + 6] = (unsigned)h;               /* the IRQ slot */
    __asm__ volatile ("dsb\n\tisb" : : : "memory");
    period = p;
    ticks = 0;
    timer_arm(p);
    __asm__ volatile ("mcr p15, #0, %0, c14, c3, #1" : : "r"(1u));  /* enable */
    __asm__ volatile ("cpsie i" : : : "memory");
    /* again until twenty interrupts have landed in the work: QEMU's timer
     * keeps the host's time, and on a fast runner one round can be over
     * first (rx-isr's -O2 saw sixteen). The bound ends a timer that never
     * fires. */
    do {
        bad |= torture(300000);
        wrong |= mix(200000) != want;
    } while (ticks < 20 && ++rounds < 40);
    __asm__ volatile ("cpsid i" : : : "memory");
    __asm__ volatile ("mcr p15, #0, %0, c14, c3, #1" : : "r"(0u));
    n = ticks;
    printf("%s: %u interrupts, torture %s, mix %s\n", name, n,
           bad ? "CHANGED" : "kept", wrong ? "WRONG" : "right");
    return bad || wrong || n < 20;
}

int main(void)
{
    unsigned long long want = mix(200000);      /* uninterrupted */
    int fail = 0;
    for (int k = 0; k < 8; k++) {
        vectors[k] = 0xe59ff018u;               /* ldr pc, [pc, #24] */
        vectors[k + 8] = (unsigned)fault;
    }
    /* the IRQ mode's own stack (IRQ and FIQ masked while there) */
    __asm__ volatile ("mrs r2, cpsr\n\t"
                      "mov r3, #0xd2\n\t"
                      "msr cpsr_c, r3\n\t"
                      "mov sp, %0\n\t"
                      "msr cpsr_c, r2"
                      : : "r"(&irq_stack[512]) : "r2", "r3", "memory");
    __asm__ volatile ("mcr p15, #0, %0, c12, c0, #0\n\tisb"
                      : : "r"(vectors) : "memory");
    REG(GICD + 0x000) = 1;                      /* the distributor on */
    REG8(GICD + 0x400 + VTIMER_PPI) = 0x80;     /* its priority */
    REG(GICD + 0x100) = 1u << VTIMER_PPI;       /* and enabled */
    REG(GICC + 0x004) = 0xf0;                   /* the CPU interface: */
    REG(GICC + 0x000) = 1;                      /* any priority above */
    (void)irq_handler;
    fail |= phase("leaf", tick_leaf, 2000, want);
    fail |= phase("calls", tick_calls, 20000, want);
    return fail ? 1 : 42;
}
