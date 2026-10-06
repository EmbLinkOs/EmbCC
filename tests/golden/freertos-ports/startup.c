/* The MPS2 boards: the vector table and the reset handler. The kernel's
 * three handlers are the port's: SVC starts the first task, PendSV
 * switches, SysTick ticks. With BOARD_FPU the coprocessor is enabled
 * before main, since code built for the hard-float ABI may use it from
 * the first instruction (the port enables it again when the scheduler
 * starts, which is harmless). And the two library routines the kernel
 * calls, so the image needs no C library. */
#include <stddef.h>
extern unsigned _sidata, _sdata, _edata, _sbss, _ebss, _estack;
#if BOARD_CMSIS_NAMES
/* The ARMv8-M ports name their handlers as CMSIS does. */
#define vPortSVCHandler    SVC_Handler
#define xPortPendSVHandler PendSV_Handler
#define xPortSysTickHandler SysTick_Handler
#endif
int main(void);
void vPortSVCHandler(void);
void xPortPendSVHandler(void);
void xPortSysTickHandler(void);

void Reset_Handler(void)
{
    unsigned *s = &_sidata, *d = &_sdata;
#if BOARD_FPU
    *(volatile unsigned *)0xE000ED88u |= 0xFu << 20;     /* CPACR: CP10, CP11 */
    __asm__ volatile("dsb\n\tisb" ::: "memory");
#endif
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
    const char *m = "FAULT\n";
    *(volatile unsigned *)(BOARD_UART + 8) = 1u;          /* CTRL: TX on */
    while (*m)
        *(volatile unsigned *)BOARD_UART = (unsigned)*m++;
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
