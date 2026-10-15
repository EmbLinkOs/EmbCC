/* pic32-part.c -- a PIC32MZ EF program for tests/golden/embsim-pic32.sh,
 * built by EmbCC for another part of the family (--mcu=32MZ1024EFE064:
 * 1 MiB of flash, 256 KiB of RAM) and run on EmbSim's board of that part,
 * pic32mz1024efe064, which its pack's ATDF describes. It prints on UART1:
 *
 *   - DEVID: the part's own ID (0x7202053) and silicon revision;
 *   - the last word of its RAM, written and read back (the stack EmbCC's
 *     startup sets is just above it);
 *   - the last word of its program flash, erased;
 *   - then it reads the word past the RAM: a bus error on this part, which
 *     EmbCC's startup answers with sdbbp, so the run ends there (on a
 *     part with 512 KiB it would print the word and ==DONE==). */
#include <stdint.h>
#include <xc.h>

static void putc_(int c)
{
    while (U1STAbits.UTXBF)
        ;
    U1TXREG = (unsigned)c;
}

static void puts_(const char *s)
{
    while (*s)
        putc_(*s++);
}

static void line(const char *name, uint32_t v)
{
    puts_(name);
    putc_(' ');
    for (int i = 7; i >= 0; i--)
        putc_("0123456789abcdef"[(v >> (4 * i)) & 15]);
    putc_('\n');
}

int main(void)
{
    U1MODE = 0;
    U1STASET = _U1STA_UTXEN_MASK;
    U1MODESET = _U1MODE_ON_MASK;
    puts_("pic32-part\n");
    line("DEVID", DEVID);
    volatile uint32_t *top = (volatile uint32_t *)0xa003fffcu;   /* kseg1 */
    *top = 0x5a5aa5a5u;
    line("the RAM's last word", *top);
    line("the flash's last word", *(volatile uint32_t *)0xbd0ffffcu);
    puts_("past the RAM:\n");
    line("the word", *(volatile uint32_t *)0xa0040000u);
    puts_("==DONE==\n");
    for (;;)
        ;
}
