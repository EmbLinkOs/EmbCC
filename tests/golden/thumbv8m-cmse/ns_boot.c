/* The Non-secure image's vector table and reset handler. The Secure boot
 * sets VTOR_NS and MSP_NS from this table and calls the reset handler. */
extern unsigned char __data_load[], __data_start[], __data_end[];
extern unsigned char __bss_start[], __bss_end[];
int main(void);
void ns_reset(void);
void ns_hang(void);

__attribute__((section(".vectors"), used))
void *const ns_vectors[4] = {
    (void *)0x00400000u, (void *)ns_reset, (void *)ns_hang, (void *)ns_hang
};

void ns_hang(void)
{
    for (;;)
        ;
}

void ns_reset(void)
{
    unsigned char *d = __data_start, *s = __data_load;
    while (d < __data_end)
        *d++ = *s++;
    for (d = __bss_start; d < __bss_end; d++)
        *d = 0;
    main();
    for (;;)
        ;
}
