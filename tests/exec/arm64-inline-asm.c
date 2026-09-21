/* aarch64 inline asm, RUN on the machine (tests/harness/aarch64): each case
 * is shaped so that a wrong encoding, a wrong register, or a missing step in
 * the operand dance changes the answer rather than merely assembling.
 *
 *   - mrs of CurrentEL: the harness runs at EL1, so it must read 1.
 *   - msr then mrs through tpidr_el0: a round trip through a real system
 *     register, both directions of the encoding.
 *   - daifset / daifclr: the PSTATE-immediate form, observed through daif.
 *   - PSCI_VERSION over hvc, in the ARM kernel's own shape (smp.c): register
 *     variables x0..x3 and a "+r" x0. If the "+" operand were not LOADED
 *     before the asm, x0 would hold garbage, and PSCI answers NOT_SUPPORTED.
 *   - `.inst %1` with an "i" operand: the word is `movz x9, #7`, so the
 *     immediate must be substituted as a literal AND land in the register
 *     variable bound to x9.
 *   - %x[name] and %w0: the named-operand and width-modifier forms, through
 *     ldr/str of an int and a long.
 *   - a bare barrier sequence and an empty "memory" asm.
 *
 * On x86-64 the body compiles away: these instructions do not exist there.
 */
// expect-exit: 42
int printf(char *fmt, ...);

#ifdef __aarch64__
static unsigned long current_el(void)
{
    unsigned long v;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(v));
    return (v >> 2) & 3;
}

static long psci_version(void)
{
    register long x0 __asm__("x0") = 0x84000000;     /* PSCI_VERSION */
    register long x1 __asm__("x1") = 0;
    register long x2 __asm__("x2") = 0;
    register long x3 __asm__("x3") = 0;
    __asm__ volatile("hvc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
    return x0;
}
#endif

int main(void)
{
#ifdef __aarch64__
    if (current_el() != 1) return 1;

    unsigned long in = 0x1234abcd5678ef01UL, out = 0;
    __asm__ volatile("msr tpidr_el0, %0" :: "r"(in));
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(out));
    if (out != in) return 2;

    unsigned long daif;
    __asm__ volatile("msr daifset, #2" ::: "memory");
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    if (!(daif & 0x80)) return 3;                 /* I is bit 7 */
    __asm__ volatile("msr daifclr, #2" ::: "memory");
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    if (daif & 0x80) return 4;

    long ver = psci_version();
    /* PSCI 0.2 is 0x2, 1.0 is 0x10000, 1.1 is 0x10001. NOT_SUPPORTED is -1. */
    if (ver < 2 || ver >= 0x20000) return 5;

    register long nine __asm__("x9") = 0;
    __asm__ volatile(".inst %1" : "+r"(nine) : "i"(0xD28000E9u)); /* movz x9,#7 */
    if (nine != 7) return 6;

    unsigned long freq = 0;
    __asm__ volatile("mrs %x[f], cntfrq_el0" : [f] "=r"(freq));
    if (freq == 0) return 7;

    int words[2] = { 11, 31 };
    int w = 0;
    __asm__ volatile("ldr %w0, [%1, #4]" : "=r"(w) : "r"(words));
    if (w != 31) return 8;
    long slot = 0, val = 42;
    __asm__ volatile("str %1, [%0]" :: "r"(&slot), "r"(val) : "memory");
    if (slot != 42) return 9;

    __asm__ volatile("dsb ishst; dsb sy; isb" ::: "memory");
    __asm__ volatile("" ::: "memory");

    printf("arm64 asm: EL%lu psci=0x%lx cntfrq=%lu\n", current_el(), ver, freq);
#endif
    return 42;
}
