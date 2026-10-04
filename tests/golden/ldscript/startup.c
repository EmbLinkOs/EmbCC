/* A CMSIS-style startup, in C: the vector table in .isr_vector, and a
 * Reset_Handler that copies .data from its load address, zeroes .bss and
 * runs the constructors, by the symbols the linker script defines. */
extern unsigned long _sidata, _sdata, _edata, _sbss, _ebss, _estack;
extern void (*__init_array_start[])(void);
extern void (*__init_array_end[])(void);
int main(void);
void Reset_Handler(void);

void Default_Handler(void)
{
    for (;;)
        ;
}

__attribute__((section(".isr_vector"), used))
void (*const g_pfnVectors[16])(void) = {
    (void (*)(void))&_estack, Reset_Handler, Default_Handler, Default_Handler,
    Default_Handler, Default_Handler, Default_Handler, 0, 0, 0, 0,
    Default_Handler, Default_Handler, 0, Default_Handler, Default_Handler,
};

void Reset_Handler(void)
{
    unsigned long *src = &_sidata, *dst = &_sdata;
    while (dst < &_edata)
        *dst++ = *src++;
    for (dst = &_sbss; dst < &_ebss; )
        *dst++ = 0;
    for (void (**p)(void) = __init_array_start; p < __init_array_end; p++)
        (*p)();
    main();
    __builtin_trap();
}
