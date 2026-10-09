/* The SPARC (LEON3, QEMU leon3_generic) test harness: a startup, the
 * register-window trap handlers, and a way to stop -- in C, with the few
 * privileged instructions as hand-encoded words (written before EmbCC had
 * a SPARC assembler, and kept so the harness does not test what it runs).
 *
 * QEMU loads the image (-kernel) into RAM at its link address and starts
 * it from its own little boot loader, which enables the APBUART and jumps
 * to the ELF entry. That entry is `embld -Tstack`'s stub -- %sp set, then
 * a jmp to _start -- because C cannot set the stack pointer.
 *
 * REGISTER WINDOWS. Every function does `save`, and the ninth nested one
 * (QEMU's LEON3 has eight windows) finds the window it wants marked
 * invalid in WIM and traps (window_overflow, tt 5); the matching
 * `restore` on the way back traps too (window_underflow, tt 6). Without
 * handlers for both the processor stops at the first deep call chain. So
 * _start builds a trap table (TBR) before anything else: entries 5 and 6
 * jump to the classic handlers below -- overflow saves the OLDEST window
 * to its own %sp and rotates WIM right one place; underflow rotates it
 * left and reloads the window being returned into -- and every other
 * entry to a fault report. WIM starts with one invalid window just behind
 * the ones in use, and traps are enabled with every interrupt masked
 * (PIL 15).
 *
 * The handlers are words, each with its assembly beside it, as llvm-mc
 * (-triple=sparc -mcpu=leon3) encodes it; the exec corpus's deep
 * recursions overflow and underflow the windows thousands of times, which
 * is what tests them.
 *
 * A bare-metal image never returns, so the end of a run is a SENTINEL on
 * the UART -- `==EXIT n==` -- which the runner stops at (qrun.sh --until),
 * and then the board is stopped: traps disabled and `ta 0`, which QEMU's
 * LEON3 takes as a shutdown.
 *
 * Built with -DHARNESS_LIBC for a program linked with lib/libc: then
 * main's result goes through exit(), so atexit handlers run and stdio is
 * flushed before the sentinel. */
extern unsigned __data_load, __data_start, __data_end;
extern unsigned __bss_start, __bss_end;
typedef void (*initfn)(void);
extern initfn __init_array_start[], __init_array_end[];

int main(void);
void _start(void);
void _exit(int status);
void puts_(const char *s);
void putn(long v);
void writec(int c);
void harness_uart_init(void);

#define NWINDOWS 8      /* QEMU's LEON3, and GRLIB's default */

/* ---- the privileged instructions, as leaf functions ----------------------
 *
 * Called through function pointers; none does `save`, so each runs in its
 * caller's window. */
static const unsigned blob_get_psr[] = {
    0x81c3e008u,    /* retl */
    0x91480000u,    /* rd %psr, %o0          (in the slot) */
};
static const unsigned blob_set_tbr[] = {
    0x819a0000u,    /* wr %o0, %g0, %tbr */
    0x01000000u,    /* nop */
    0x01000000u,    /* nop */
    0x01000000u,    /* nop */
    0x81c3e008u,    /* retl */
    0x01000000u,    /* nop */
};
static const unsigned blob_set_wim[] = {
    0x81920000u,    /* wr %o0, %g0, %wim */
    0x01000000u,    /* nop */
    0x01000000u,    /* nop */
    0x01000000u,    /* nop */
    0x81c3e008u,    /* retl */
    0x01000000u,    /* nop */
};
/* ET on, PIL 15: traps enabled, every interrupt masked. A read-modify-
 * write of the PSR in one place, so its CWP field is the current one. */
static const unsigned blob_enable_traps[] = {
    0x93480000u,    /* rd %psr, %o1 */
    0x92126f20u,    /* or %o1, 0xf20, %o1 */
    0x818a4000u,    /* wr %o1, %g0, %psr */
    0x01000000u,    /* nop */
    0x01000000u,    /* nop */
    0x01000000u,    /* nop */
    0x81c3e008u,    /* retl */
    0x01000000u,    /* nop */
};
/* Traps off, then `ta 0`: a trap with traps disabled is the error state,
 * which QEMU's LEON3 turns into a clean shutdown for trap 0x80. */
static const unsigned blob_shutdown[] = {
    0x93480000u,    /* rd %psr, %o1 */
    0x922a6020u,    /* andn %o1, 0x20, %o1 */
    0x818a4000u,    /* wr %o1, %g0, %psr */
    0x01000000u,    /* nop */
    0x01000000u,    /* nop */
    0x01000000u,    /* nop */
    0x91d02000u,    /* ta 0 */
    0x01000000u,    /* nop */
};

/* ---- the window trap handlers ------------------------------------------
 *
 * Entered in the trap window, with %l1/%l2 the trapped instruction's PC
 * and nPC and traps disabled; both end by re-executing the save or
 * restore that trapped (jmp %l1; rett %l2). */
static const unsigned window_overflow[] = {
    0xa7500000u,    /* rd %wim, %l3 */
    0xae100001u,    /* mov %g1, %l7 */
    0x8334e001u,    /* srl %l3, 1, %g1 */
    0xa92ce007u,    /* sll %l3, NWINDOWS-1, %l4 */
    0x82150001u,    /* or %l4, %g1, %g1        new WIM: rotated right */
    0x81e00000u,    /* save                    into the window to store */
    0x81904000u,    /* wr %g1, %g0, %wim */
    0x01000000u,    /* nop */
    0x01000000u,    /* nop */
    0x01000000u,    /* nop */
    0xe03ba000u,    /* std %l0, [%sp] */
    0xe43ba008u,    /* std %l2, [%sp+8] */
    0xe83ba010u,    /* std %l4, [%sp+16] */
    0xec3ba018u,    /* std %l6, [%sp+24] */
    0xf03ba020u,    /* std %i0, [%sp+32] */
    0xf43ba028u,    /* std %i2, [%sp+40] */
    0xf83ba030u,    /* std %i4, [%sp+48] */
    0xfc3ba038u,    /* std %i6, [%sp+56] */
    0x81e80000u,    /* restore                 back to the trap window */
    0x82100017u,    /* mov %l7, %g1 */
    0x81c44000u,    /* jmp %l1 */
    0x81cc8000u,    /* rett %l2 */
};
static const unsigned window_underflow[] = {
    0xa7500000u,    /* rd %wim, %l3 */
    0xa92ce001u,    /* sll %l3, 1, %l4 */
    0xab34e007u,    /* srl %l3, NWINDOWS-1, %l5 */
    0xaa154014u,    /* or %l5, %l4, %l5        new WIM: rotated left */
    0x81954000u,    /* wr %l5, %g0, %wim */
    0x01000000u,    /* nop */
    0x01000000u,    /* nop */
    0x01000000u,    /* nop */
    0x81e80000u,    /* restore                 to the trapping window */
    0x81e80000u,    /* restore                 to the one to reload */
    0xe01ba000u,    /* ldd [%sp], %l0 */
    0xe41ba008u,    /* ldd [%sp+8], %l2 */
    0xe81ba010u,    /* ldd [%sp+16], %l4 */
    0xec1ba018u,    /* ldd [%sp+24], %l6 */
    0xf01ba020u,    /* ldd [%sp+32], %i0 */
    0xf41ba028u,    /* ldd [%sp+40], %i2 */
    0xf81ba030u,    /* ldd [%sp+48], %i4 */
    0xfc1ba038u,    /* ldd [%sp+56], %i6 */
    0x81e00000u,    /* save */
    0x81e00000u,    /* save                    back to the trap window */
    0x81c44000u,    /* jmp %l1 */
    0x81cc8000u,    /* rett %l2 */
};

typedef unsigned (*get_fn)(void);
typedef void (*set_fn)(unsigned);

static unsigned get_psr(void)
{
    return ((get_fn)(void *)blob_get_psr)();
}

void harness_shutdown(void)
{
    ((get_fn)(void *)blob_shutdown)();
    for (;;)
        ;
}

static void puthex(unsigned v)
{
    puts_("0x");
    for (int k = 28; k >= 0; k -= 4)
        writec("0123456789abcdef"[(v >> k) & 15]);
    writec(' ');
}

/* Any other trap -- an unaligned access (tt 7), an illegal instruction
 * (2), a division by zero (0x2a), a data access error (9) -- reports
 * itself instead of hanging, and never prints `==EXIT`, so a run that
 * faults cannot pass. Entered from fault_entry with traps disabled and
 * WIM cleared (no save can trap), on a stack below the trapped one's. */
void harness_fault(unsigned tbr, unsigned pc)
{
    puts_("\n==FAULT tt ");
    puthex((tbr >> 4) & 0xff);
    puts_("pc ");
    puthex(pc);
    puts_("==\n");
    harness_shutdown();
}

/* sethi %hi(target), %l4 ; jmpl %l4 + %lo(target), link */
static unsigned sethi_l4(unsigned a) { return 0x29000000u | (a >> 10); }
static unsigned jmpl_l4(unsigned a, unsigned link)
{
    return 0x81c52000u | (link << 25) | (a & 0x3ffu);
}

/* Built at run time, where the addresses are known: the table (4 KiB,
 * 4 KiB-aligned within a buffer twice that) and the fault entry. */
static unsigned table_buf[2048];
static unsigned fault_entry[12];

static void install_traps(void)
{
    unsigned *tt = (unsigned *)(((unsigned)table_buf + 4095u) & ~4095u);
    unsigned f = (unsigned)fault_entry;
    unsigned c = (unsigned)harness_fault;
    unsigned cwp;

    fault_entry[0] = 0x81900000u;              /* wr %g0, %g0, %wim */
    fault_entry[1] = 0x01000000u;              /* nop */
    fault_entry[2] = 0x01000000u;              /* nop */
    fault_entry[3] = 0x01000000u;              /* nop */
    fault_entry[4] = 0x90100013u;              /* mov %l3, %o0      %tbr */
    fault_entry[5] = 0x92100011u;              /* mov %l1, %o1      pc */
    fault_entry[6] = 0x9c27a200u;              /* sub %fp, 512, %sp */
    fault_entry[7] = sethi_l4(c);              /* sethi %hi(harness_fault), %l4 */
    fault_entry[8] = jmpl_l4(c, 15);           /* call %l4 + %lo(...) */
    fault_entry[9] = 0x01000000u;              /* nop */
    fault_entry[10] = 0x01000000u;
    fault_entry[11] = 0x01000000u;
    for (int k = 0; k < 256; k++) {
        unsigned h = k == 5 ? (unsigned)window_overflow
                   : k == 6 ? (unsigned)window_underflow : f;
        tt[4 * k] = sethi_l4(h);                   /* sethi %hi(h), %l4 */
        tt[4 * k + 1] = jmpl_l4(h, 0);             /* jmp %l4 + %lo(h) */
        tt[4 * k + 2] = 0xa7580000u;               /* rd %tbr, %l3 (slot) */
        tt[4 * k + 3] = 0x01000000u;               /* nop */
    }
    /* One invalid window: the one just behind this function's caller
     * (_start), whose own caller -- the reset stub -- is never returned
     * to. Read in a leaf, so the CWP is this function's. */
    cwp = get_psr() & 31u;
    ((set_fn)(void *)blob_set_wim)(1u << ((cwp + 2) % NWINDOWS));
    ((set_fn)(void *)blob_set_tbr)((unsigned)tt);
    ((get_fn)(void *)blob_enable_traps)();
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
    install_traps();
    harness_uart_init();
    for (initfn *f = __init_array_start; f < __init_array_end; f++)
        (*f)();
    r = main();
#ifdef HARNESS_LIBC
    exit(r);
#endif
    _exit(r);
}
