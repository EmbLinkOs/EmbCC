/* The exec corpus's startup for embsim-esp32c3.sh's core check: one
 * source, linked twice -- for QEMU's virt with an RV32IMC core
 * (corpus-virt.ld: RAM at 0x80000000) and for EmbSim's ESP32-C3-DevKitM-1
 * (corpus-esp32c3.ld: the code in IRAM, the rest in DRAM) -- so the two
 * runs execute the same instructions. Output and the exit status go
 * through RISC-V semihosting (SYS_WRITEC, SYS_EXIT), which both have:
 * neither board's UART is touched. Any trap ends the run with 125: the
 * trap vector is 256-aligned, as the ESP32-C3's mtvec needs (it is
 * vectored there, and an exception goes to its base, as on QEMU in direct
 * mode). write() and _exit() replace lib/libc's weak bare-metal defaults.
 *
 * QEMU's virt, given -bios none, starts at its RAM's first byte rather
 * than at the image's entry, which EmbSim's ESP32-C3 starts at (as the
 * ROM's loader does): corpus-virt.ld puts reset_stub there, three
 * instructions that jump to _start, which only QEMU runs. */
extern unsigned _sidata[], _sdata[], _edata[], _sbss[], _ebss[], _estack[];
extern char trap_entry[];

int main(void);
void Reset_Handler(void);
void fault(void);
void _start(void);

/* auipc t0, 0; lw t0, 12(t0); jr t0; and _start's address */
__attribute__((section(".text.reset_stub"), used))
static const struct { unsigned insn[3]; void (*to)(void); } reset_stub = {
    { 0x00000297u, 0x00c2a283u, 0x00028067u }, _start
};

__asm__(".balign 256\n"
        ".globl trap_entry\n"
        "trap_entry:\n"
        "    j fault\n"
        ".globl _start\n"
        "_start:\n"
        "    la sp, _estack\n"
        "    j Reset_Handler\n");

/* the semihosting call: an uncompressed ebreak between its two markers */
static long semihost(long op, void *arg)
{
    register long a0 __asm__("a0") = op;
    register void *a1 __asm__("a1") = arg;
    __asm__ volatile("slli x0, x0, 0x1f\n\tebreak\n\tsrai x0, x0, 7"
                     : "+r"(a0) : "r"(a1) : "memory");
    return a0;
}

void _exit(int status)
{
    static volatile unsigned blk[2];
    blk[0] = 0x20026u;
    blk[1] = (unsigned)status;
    semihost(0x20, (void *)blk);
    for (;;)
        ;
}

long write(int fd, const void *buf, unsigned long n)
{
    const char *p = buf;
    (void)fd;
    for (unsigned long i = 0; i < n; i++) {
        volatile char c = p[i];
        semihost(3, (void *)&c);
    }
    return (long)n;
}

void fault(void)
{
    _exit(125);
}

void Reset_Handler(void)
{
    __asm__ volatile("csrw mtvec, %0" : : "r"(trap_entry));
    unsigned *d = _sdata, *s = _sidata;
    while (d < _edata)
        *d++ = *s++;
    for (d = _sbss; d < _ebss; )
        *d++ = 0;
    _exit(main());
}
