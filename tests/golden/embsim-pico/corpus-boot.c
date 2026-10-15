/* The exec corpus's startup for embsim-pico.sh's core check: one source,
 * linked twice -- for QEMU's micro:bit (corpus-microbit.ld: the vector
 * table at 0) and for EmbSim's Pico (corpus-pico.ld: boot2 at 0x10000000,
 * the vector table after it) -- so the two runs execute the same
 * instructions but for the boot. Output and the exit status go through
 * semihosting (SYS_WRITEC, and the driver's SYS_EXIT), which both have:
 * neither board's UART is touched, so QEMU counts every instruction once.
 * write() and _exit() replace lib/libc's weak bare-metal defaults. */
extern unsigned _sidata[], _sdata[], _edata[], _sbss[], _ebss[], _estack[];

int main(void);
void Reset_Handler(void);
void fault(void);

__attribute__((section(".isr_vector"), used))
void *const vectors[4] = { (void *)_estack, (void *)Reset_Handler, (void *)fault,
                           (void *)fault };

static void writec(int c)
{
    volatile char ch = (char)c;
    __asm__ volatile("mov r1, %0\n\tmovs r0, #3\n\tbkpt #0xab"
                     : : "r"(&ch) : "r0", "r1", "memory");
}

void _exit(int status)
{
    static volatile unsigned blk[2];
    blk[0] = 0x20026u;
    blk[1] = (unsigned)status;
    __asm__ volatile("mov r1, %0\n\tmovs r0, #0x20\n\tbkpt #0xab"
                     : : "r"(blk) : "r0", "r1", "memory");
    for (;;)
        ;
}

long write(int fd, const void *buf, unsigned long n)
{
    const char *p = buf;
    (void)fd;
    for (unsigned long i = 0; i < n; i++)
        writec(p[i]);
    return (long)n;
}

void fault(void)
{
    _exit(125);
}

void Reset_Handler(void)
{
    unsigned *d = _sdata, *s = _sidata;
    while (d < _edata)
        *d++ = *s++;
    for (d = _sbss; d < _ebss; )
        *d++ = 0;
    _exit(main());
}
