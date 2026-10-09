/* The Secure image's vector table: the core leaves reset in the Secure
 * state and reads it at 0x10000000 (secure.ld). HardFault and SecureFault
 * report the faulting pc and stop, so a veneer the Non-secure state cannot
 * enter -- not Non-secure Callable, or no SG -- is a line of output and
 * not a hang. */
void s_main(void);
void reset(void);
void s_fault(void);

__attribute__((section(".vectors"), used))
void *const vectors[8] = {
    (void *)0x10200000u, (void *)reset, (void *)s_fault, (void *)s_fault,
    (void *)s_fault, (void *)s_fault, (void *)s_fault, (void *)s_fault
};

void reset(void)
{
    s_main();
    for (;;)
        ;
}

#define UART0_DR   (*(volatile unsigned *)0x40200000u)
#define UART0_CTRL (*(volatile unsigned *)0x40200008u)

void s_fault_c(unsigned *frame, unsigned ipsr)
{
    static const char *const msg = "\n==FAULT ";
    const char *s = msg;
    UART0_CTRL = 1u;
    while (*s)
        UART0_DR = (unsigned)*s++;
    for (int k = 28; k >= 0; k -= 4)
        UART0_DR = (unsigned)"0123456789abcdef"[(frame[6] >> k) & 15u];
    UART0_DR = ' ';
    UART0_DR = (unsigned)('0' + (ipsr & 15u));
    s = "==\n==EXIT 125==\n";
    while (*s)
        UART0_DR = (unsigned)*s++;
    for (;;)
        ;
}

__attribute__((naked)) void s_fault(void)
{
    __asm__ volatile("mrs r0, msp\n\tmrs r1, ipsr\n\tbl s_fault_c");
}
