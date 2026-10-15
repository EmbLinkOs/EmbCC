/* pic32-curiosity.c -- a PIC32MZ program for tests/golden/embsim-pic32.sh,
 * built by EmbCC (--mcu=32MZ2048EFM144) with its own startup, and run on
 * EmbSim's curiosity-pic32mz-ef-2 board with SW1 pressed. It prints on
 * UART2 (the board's USB-UART), and ends with ==DONE== (the run's --until):
 *
 *   - ANSELJ and ANSELC out of reset: only the pins with an analog
 *     function are analog (RJ8, RJ9, RJ11; RC1-RC4), so the switches'
 *     pins (RJ4-RJ6, RC14) are digital without a write;
 *   - the switches, which have no pull-up on the board: SW1 pressed reads
 *     0 with its pin's pull-up on, SW2 released 1 with it, SW3 released 0
 *     without it (nothing drives the pin), SW4 (RC14) released 1;
 *   - the LEDs, lit when their pin drives low: LED1, LED2, LED3 and the
 *     RGB LED's three colours turned on and LED3 off again, as the board's
 *     report shows. */
#include <stdint.h>
#include <xc.h>

static void putc_(int c)
{
    while (U2STAbits.UTXBF)
        ;
    U2TXREG = (unsigned)c;
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
    U2MODE = 0;
    U2STASET = _U2STA_UTXEN_MASK;
    U2MODESET = _U2MODE_ON_MASK;
    puts_("pic32-curiosity\n");

    line("ANSELJ at reset", ANSELJ);
    line("ANSELC at reset", ANSELC);

    /* the switches: SW1, SW2 and SW4 with their pull-ups, SW3 without */
    CNPUJSET = 1u << 4 | 1u << 5;
    CNPUCSET = 1u << 14;
    line("SW1 (RJ4)", PORTJ >> 4 & 1);
    line("SW2 (RJ5)", PORTJ >> 5 & 1);
    line("SW3 (RJ6)", PORTJ >> 6 & 1);
    line("SW4 (RC14)", PORTC >> 14 & 1);

    /* the LEDs: the latches high (off), outputs, then each on */
    LATJSET = 1u << 7 | 1u << 3;
    LATKSET = 1u << 7;
    LATBSET = 7u << 7;
    TRISJCLR = 1u << 7 | 1u << 3;
    TRISKCLR = 1u << 7;
    TRISBCLR = 7u << 7;
    LATJCLR = 1u << 7;                          /* LED1 */
    LATKCLR = 1u << 7;                          /* LED2 */
    LATJCLR = 1u << 3;                          /* LED3 */
    LATBCLR = 7u << 7;                          /* red, green, blue */
    LATJSET = 1u << 3;                          /* LED3 off */
    line("LATJ", LATJ & 0x88u);
    puts_("==DONE==\n");
    for (;;)
        ;
}
