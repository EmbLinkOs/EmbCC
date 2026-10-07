/* A Cortex-M startup that MEASURES the stack main uses: it fills the free
 * stack with a pattern before main, and afterwards finds the lowest word
 * main's calls overwrote. tests/golden/embrt.sh compares that high-water
 * mark with embrt's bound, which must never be lower. For the STM32 script
 * in tests/golden/ldscript (QEMU's lm3s6965evb has the same RAM). */
extern unsigned long _sidata, _sdata, _edata, _sbss, _ebss, _estack;
int main(void);
void Reset_Handler(void);
void puts_(const char *s);
void putn(long v);

void Default_Handler(void)
{
    puts_("==FAULT==\n==END==\n");
    for (;;)
        ;
}

__attribute__((section(".isr_vector"), used))
void (*const g_pfnVectors[16])(void) = {
    (void (*)(void))&_estack, Reset_Handler, Default_Handler, Default_Handler,
    Default_Handler, Default_Handler, Default_Handler, 0, 0, 0, 0,
    Default_Handler, Default_Handler, 0, Default_Handler, Default_Handler,
};

#define PAINT 0xa5a5a5a5ul

void Reset_Handler(void)
{
    unsigned long *src = &_sidata, *dst = &_sdata, sp;
    while (dst < &_edata)
        *dst++ = *src++;
    for (dst = &_sbss; dst < &_ebss; )
        *dst++ = 0;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    /* everything below this frame, down to the end of .bss */
    for (dst = &_ebss; (unsigned long)dst < sp; dst++)
        *dst = PAINT;
    int r = main();
    for (dst = &_ebss; (unsigned long)dst < sp && *dst == PAINT; dst++)
        ;
    puts_("STACK ");
    putn((long)(sp - (unsigned long)dst));
    puts_(" RET ");
    putn(r);
    puts_("\n==END==\n");
    for (;;)
        ;
}
