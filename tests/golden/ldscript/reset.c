/* The C half of the RISC-V start: .data from its load address, .bss
 * zeroed, main, and the SiFive test device to stop QEMU. */
extern unsigned long _sidata, _sdata, _edata, _sbss, _ebss;
int main(void);
#define SIFIVE_TEST (*(volatile unsigned *)0x100000u)
void reset_c(void)
{
    unsigned long *s = &_sidata, *d = &_sdata;
    while (d < &_edata)
        *d++ = *s++;
    for (d = &_sbss; d < &_ebss; )
        *d++ = 0;
    main();
    SIFIVE_TEST = 0x5555u;
    for (;;)
        ;
}
