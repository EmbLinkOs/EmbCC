/* pic32mx-board.c -- a PIC32MX program for tests/golden/embsim-pic32mx.sh,
 * built by EmbCC (--mcu=32MX795F512L), run on EmbSim's
 * explorer16-32-pic32mx with S3 pressed and the temperature at 30
 * degrees. It prints on UART1 what each part of the board gave, and ends
 * with ==DONE==:
 *
 *   - the buttons: S3 (RD6) pressed, S4 (RD13) released, read through the
 *     board's pull-ups;
 *   - the LEDs: RA0-RA7 driven 0x5A, then RA0 off (the board reports
 *     each);
 *   - ADC1 (the 10-bit converter): the TC1047A on AN4 (0.8 V at 30
 *     degrees: 248) by sampling and clearing SAMP, and the potentiometer
 *     on AN5 (half way: 512) auto-converted (SSRC 111), its interrupt flag,
 *     and CLRASAM clearing ASAM at the interrupt;
 *   - the 25LC256 on SPI2, its CS on RD12: WREN, two bytes written at
 *     0x0200, STATUS polled through the write cycle, read back with the
 *     erased byte after them; SPI2STAT once SPI2BUF is read (SPITBE
 *     alone: the read cleared SPIRBF);
 *   - the LCD on the PMP: its two lines, which the board reports. */
#include <stdint.h>
#include <xc.h>

#pragma config FPBDIV = DIV_1
#pragma config FWDTEN = OFF

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

static unsigned ee_xfer(unsigned out)
{
    SPI2BUF = out;
    while (!SPI2STATbits.SPIRBF)
        ;
    return SPI2BUF & 0xff;
}

static uint32_t spistat;                /* SPI2STAT after a read of SPI2BUF */

static void ee_cs(int low)
{
    if (low)
        LATDCLR = 1u << 12;
    else
        LATDSET = 1u << 12;
}

static void lcd(int rs, int c)
{
    PMADDR = (unsigned)rs;
    PMDIN = (unsigned)c;
    while (PMMODEbits.BUSY)
        ;
}

static void lcd_text(const char *s)
{
    while (*s)
        lcd(1, *s++);
}

int main(void)
{
    U1MODE = 0;
    U1STASET = _U1STA_UTXEN_MASK;
    U1MODESET = _U1MODE_ON_MASK;
    puts_("pic32mx-board\n");

    /* the buttons, on digital-only pins */
    line("S3 (RD6)", PORTDbits.RD6);
    line("S4 (RD13)", PORTDbits.RD13);

    /* the LEDs */
    TRISACLR = 0xff;
    LATA = 0x5a;
    LATACLR = 0x02;
    line("LATA", LATA & 0xff);

    /* ADC1: AN4 sampled by hand, AN5 auto-converted */
    AD1PCFG = ~0x30u;                       /* AN4, AN5 analog */
    AD1CON1 = 0;
    AD1CON2 = 0;
    AD1CON3 = 0x0202;                       /* SAMC 2, ADCS 2 */
    AD1CHS = 4u << 16;
    AD1CON1SET = _AD1CON1_ON_MASK;
    AD1CON1SET = _AD1CON1_SAMP_MASK;
    for (volatile int i = 0; i < 20; i++)
        ;
    AD1CON1CLR = _AD1CON1_SAMP_MASK;
    while (!AD1CON1bits.DONE)
        ;
    line("AN4 (the TC1047A)", ADC1BUF0);
    IFS1CLR = _IFS1_AD1IF_MASK;
    AD1CON1CLR = _AD1CON1_ON_MASK;
    AD1CHS = 5u << 16;
    AD1CON1SET = 7u << _AD1CON1_SSRC_POSITION;
    AD1CON1SET = _AD1CON1_ON_MASK;
    AD1CON1SET = _AD1CON1_SAMP_MASK;        /* converted after SAMC TADs */
    while (!IFS1bits.AD1IF)
        ;
    line("AN5 (the potentiometer)", ADC1BUF0);
    IFS1CLR = _IFS1_AD1IF_MASK;
    AD1CON1SET = _AD1CON1_CLRASAM_MASK;
    AD1CON1SET = _AD1CON1_ASAM_MASK;
    while (!IFS1bits.AD1IF)
        ;
    line("ASAM after CLRASAM", AD1CON1bits.ASAM);

    /* the 25LC256 on SPI2 */
    TRISDCLR = 1u << 12;
    ee_cs(0);
    SPI2CON = 0;
    SPI2BRG = 4;
    SPI2CON = _SPI2CON_ON_MASK | _SPI2CON_MSTEN_MASK | _SPI2CON_CKE_MASK;
    ee_cs(1);
    ee_xfer(6);                             /* WREN */
    spistat = SPI2STAT;
    ee_cs(0);
    ee_cs(1);
    ee_xfer(2);                             /* WRITE 0x0200 */
    ee_xfer(0x02);
    ee_xfer(0x00);
    ee_xfer(0x3c);
    ee_xfer(0xc3);
    ee_cs(0);
    unsigned es, polls = 0;
    do {
        ee_cs(1);
        ee_xfer(5);
        es = ee_xfer(0);
        ee_cs(0);
        polls++;
    } while (es & 1);
    line("EEPROM busy for a while", polls > 10);
    ee_cs(1);
    ee_xfer(3);                             /* READ 0x0200 */
    ee_xfer(0x02);
    ee_xfer(0x00);
    uint32_t r = ee_xfer(0) << 16;
    r |= ee_xfer(0) << 8;
    r |= ee_xfer(0);
    ee_cs(0);
    line("EEPROM read", r);
    line("SPI2STAT after a read", spistat & 0xff);

    /* the LCD */
    PMCON = 0;
    PMMODE = 0;
    PMMODEbits.MODE = 2;
    PMMODEbits.WAITB = 1;
    PMMODEbits.WAITM = 3;
    PMMODEbits.WAITE = 1;
    PMAEN = 1;
    PMCONSET = _PMCON_PTRDEN_MASK | _PMCON_PTWREN_MASK | _PMCON_ON_MASK;
    lcd(0, 0x38);
    lcd(0, 0x0c);
    lcd(0, 0x01);
    lcd(0, 0x06);
    lcd_text("EmbSim");
    lcd(0, 0xc0);
    lcd_text("PIC32MX795F512L");
    lcd(0, 0x02);

    puts_("==DONE==\n");
    for (;;)
        ;
}
