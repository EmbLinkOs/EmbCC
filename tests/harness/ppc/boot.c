/* The PowerPC (powerpc-none-eabi, QEMU ppce500) test harness: a startup
 * and a way to stop, in C.
 *
 * QEMU loads the image (-kernel, no -bios) at its link address, maps the
 * first 64 MiB 1:1 with a TLB1 entry and starts at the ELF entry, which is
 * `embld -Tstack`'s stub: r1 set, a zero back chain below it, and a bctr
 * to _start, because C cannot set the stack pointer. This copies .data
 * (which moves nothing in a RAM image), zeroes .bss, maps the CCSR block
 * (the UART is there), points the exception vectors at a report, runs the
 * static constructors, then main, and reports its result.
 *
 * A bare-metal image never returns, so the end of a run is a SENTINEL on
 * the UART -- `==EXIT n ==` with main's result -- which the runner stops at
 * (tests/harness/qrun.sh --until). io.c's _exit prints it and then asks the
 * board to reset, which with -no-reboot ends QEMU at once.
 *
 * The few privileged instructions this needs -- mtspr to the MMU assist
 * registers and the interrupt vector registers, tlbwe, mfspr of SRR0, ESR
 * and DEAR -- are written as instruction WORDS into small buffers and
 * called (written before EmbCC had a PowerPC assembler, and kept so the
 * harness does not depend on what it is used to test). Each word's
 * encoding is llvm-mc's (tests/golden/ppc-encoding.sh referees the same
 * fields in src/arch/ppc/emit.c). QEMU notices code written to memory;
 * real hardware would want a dcbst/icbi first.
 *
 * Built with -DHARNESS_LIBC for a program linked with lib/libc: then main's
 * result goes through exit(), so atexit handlers run and stdio is flushed
 * before the sentinel. */
extern unsigned __data_load, __data_start, __data_end;
extern unsigned __bss_start, __bss_end;
typedef void (*initfn)(void);
extern initfn __init_array_start[], __init_array_end[];

int main(void);
void _start(void);
void _exit(int status);
void puts_(const char *s);
void writec(int c);

/* The CCSR block's reset (global utilities RSTCR, HRESET_REQ), as io.c's. */
#define RSTCR (*(volatile unsigned *)0xe00e00b0u)

/* mfspr r3, spr / mtspr spr, r3: the SPR number's two five-bit halves
 * exchanged in bits 11..20, as the ISA writes it. */
static unsigned spr_field(int spr)
{
    return (unsigned)(((spr & 31) << 5) | (spr >> 5)) << 11;
}

static unsigned spr_rd_code[2];
static unsigned spr_wr_code[2];

static unsigned spr_read(int spr)
{
    spr_rd_code[0] = 0x7c6002a6u | spr_field(spr);     /* mfspr r3, spr */
    spr_rd_code[1] = 0x4e800020u;                      /* blr */
    return ((unsigned (*)(void))(void *)spr_rd_code)();
}

static void spr_write(int spr, unsigned v)
{
    spr_wr_code[0] = 0x7c6003a6u | spr_field(spr);     /* mtspr spr, r3 */
    spr_wr_code[1] = 0x4e800020u;                      /* blr */
    ((void (*)(unsigned))(void *)spr_wr_code)(v);
}

/* isync; tlbwe; isync; blr */
static const unsigned tlbwe_code[4] = {
    0x4c00012cu, 0x7c0007a4u, 0x4c00012cu, 0x4e800020u
};

/* The CCSR block is at physical 0xF_E000_0000 (36 bits) on ppce500, and
 * nothing maps it at reset. TLB1 entry 1: EA 0xE0000000, 1 MiB, to that
 * physical address, cache-inhibited and guarded, supervisor read/write. */
static void map_ccsr(void)
{
    spr_write(624, 0x10010000u);          /* MAS0: TLB1, entry 1 */
    spr_write(625, 0xc0000500u);          /* MAS1: valid, IPROT, 1 MiB */
    spr_write(626, 0xe000000au);          /* MAS2: EPN, I, G */
    spr_write(627, 0xe0000015u);          /* MAS3: RPN, SX, SW, SR */
    spr_write(944, 0x0000000fu);          /* MAS7: physical bits 32..35 */
    ((void (*)(void))(void *)tlbwe_code)();
}

static void puthex(unsigned v)
{
    puts_("0x");
    for (int k = 28; k >= 0; k -= 4)
        writec("0123456789abcdef"[(v >> k) & 15]);
    writec(' ');
}

/* An interrupt -- a program check (an illegal or floating-point
 * instruction, a trap), a data or instruction storage error, an alignment
 * one -- reports itself instead of hanging: which vector, where (SRR0),
 * the syndrome (ESR) and the data address (DEAR). The report is followed by
 * the board's reset and never by `==EXIT`, so a run that faults cannot
 * pass. */
void harness_fault(int vec)
{
    unsigned srr0 = spr_read(26), esr = spr_read(62), dear = spr_read(61);
    puts_("\n==FAULT ivor ");
    writec('0' + vec / 10);
    writec('0' + vec % 10);
    puts_(" srr0 ");
    puthex(srr0);
    puts_("esr ");
    puthex(esr);
    puts_("dear ");
    puthex(dear);
    puts_("==\n");
    RSTCR = 2;
    for (;;)
        ;
}

/* Sixteen stubs, one per IVOR, 32 bytes apart: li r3, n; then lis/ori r12
 * with harness_fault's address, mtctr and bctr. Aligned so all sixteen
 * share the upper 16 bits IVPR supplies. */
static unsigned vectors[16 * 8] __attribute__((aligned(512)));

static void install_vectors(void)
{
    unsigned h = (unsigned)harness_fault, base = (unsigned)vectors;
    for (int k = 0; k < 16; k++) {
        unsigned *v = vectors + 8 * k;
        v[0] = 0x38600000u | (unsigned)k;             /* li r3, k */
        v[1] = 0x3d800000u | (h >> 16);               /* lis r12, hi */
        v[2] = 0x618c0000u | (h & 0xffffu);           /* ori r12, r12, lo */
        v[3] = 0x7d8903a6u;                           /* mtctr r12 */
        v[4] = 0x4e800420u;                           /* bctr */
    }
    spr_write(63, base & 0xffff0000u);                /* IVPR */
    for (int k = 0; k < 16; k++)                      /* IVOR0..15 */
        spr_write(400 + k, (base + 32u * (unsigned)k) & 0xfff0u);
}

#ifdef HARNESS_LIBC
void exit(int status);
#endif

void _start(void)
{
    unsigned *d = &__data_start, *s = &__data_load;
    int r;
    while (d < &__data_end)
        *d++ = *s++;
    for (d = &__bss_start; d < &__bss_end; )
        *d++ = 0;
    map_ccsr();
    install_vectors();
    for (initfn *f = __init_array_start; f < __init_array_end; f++)
        (*f)();
    r = main();
#ifdef HARNESS_LIBC
    exit(r);
#endif
    _exit(r);
}
