/* pic32mx-part.c -- a PIC32MX program for tests/golden/embsim-pic32mx.sh,
 * built by EmbCC for a part (--mcu=32MX250F128B, -DRAM=0x8000 and
 * -DFLASH=0x20000: its RAM's and program flash's sizes) and run on
 * EmbSim's board of that part, which its pack's EDC describes. It prints
 * on UART1 the part's DEVID, the last word of its RAM, written and read
 * back, and its program flash's, erased; then reads the word past the
 * RAM: a bus
 * error, which EmbCC's startup answers with sdbbp, so the run ends
 * there. */
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
    puts_("pic32mx-part\n");
    line("DEVID", DEVID);
    volatile uint32_t *top = (volatile uint32_t *)(0xa0000000u + RAM - 4);
    *top = 0x12345678u;
    line("the RAM's last word", *top);
    line("the flash's last word", *(volatile uint32_t *)(0xbd000000u + FLASH - 4));
    puts_("past the RAM:\n");
    line("the word", *(volatile uint32_t *)(0xa0000000u + RAM));
    puts_("==DONE==\n");
    for (;;)
        ;
}
