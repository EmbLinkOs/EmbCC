/* QEMU virt: the C half of the startup (riscv-start.S calls c_start), and
 * the two library routines the kernel calls, so the image needs no C
 * library. */
#include <stddef.h>
extern unsigned _sidata, _sdata, _edata, _sbss, _ebss;
int main(void);

void c_start(void)
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

/* The port calls these for traps it does not handle itself. */
void freertos_risc_v_application_exception_handler(unsigned long cause)
{
    const char *m = "EXCEPTION\n";
    (void)cause;
    while (*m)
        *(volatile unsigned char *)0x10000000u = (unsigned char)*m++;
    for (;;)
        ;
}

void freertos_risc_v_application_interrupt_handler(unsigned long cause)
{
    freertos_risc_v_application_exception_handler(cause);
}

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
