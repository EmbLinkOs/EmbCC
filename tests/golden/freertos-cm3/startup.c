/* lm3s6965evb: the vector table and the reset handler. The kernel's
 * three handlers are the port's: SVC starts the first task, PendSV
 * switches, SysTick ticks. And the two library routines the kernel
 * calls, so the image needs no C library. */
#include <stddef.h>
extern unsigned _sidata, _sdata, _edata, _sbss, _ebss, _estack;
int main(void);
void vPortSVCHandler(void);
void xPortPendSVHandler(void);
void xPortSysTickHandler(void);

void Reset_Handler(void)
{
    unsigned *s = &_sidata, *d = &_sdata;
    while (d < &_edata)
        *d++ = *s++;
    for (d = &_sbss; d < &_ebss; )
        *d++ = 0;
    main();
    for (;;)
        ;
}

void Fault_Handler(void)
{
    volatile unsigned *uart = (volatile unsigned *)0x4000C000u;
    const char *m = "FAULT\n";
    while (*m)
        *uart = (unsigned)*m++;
    __builtin_trap();
}

__attribute__((section(".isr_vector"), used))
void (*const vectors[16])(void) = {
    (void (*)(void))&_estack, Reset_Handler,
    Fault_Handler, Fault_Handler, Fault_Handler, Fault_Handler, Fault_Handler,
    0, 0, 0, 0,
    vPortSVCHandler, Fault_Handler, 0, xPortPendSVHandler, xPortSysTickHandler,
};

void *memset(void *d, int c, size_t n)
{
    unsigned char *p = d;
    while (n--)
        *p++ = (unsigned char)c;
    return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *p = d;
    const unsigned char *q = s;
    while (n--)
        *p++ = *q++;
    return d;
}
