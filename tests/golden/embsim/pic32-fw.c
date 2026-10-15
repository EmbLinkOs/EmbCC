/* pic32-fw.c -- a PIC32MZ program for tests/golden/embsim-pic32.sh, built
 * by EmbCC (--mcu=32MZ2048EFH100) with its own startup, and run on
 * EmbSim's explorer16-32 board. It prints what each part did on UART1,
 * and ends with ==DONE== (the run's --until):
 *
 *   - the DSP ASE's $ac1, usable after EmbCC's startup (Status.MX);
 *   - the EVIC in multi-vector mode: the core's software interrupt CS0
 *     at priority 1 runs in shadow register set 1 (PRISS); Timer1's
 *     interrupts at a prescale of 8; the core timer's, from Compare;
 *     Timer2 and Timer3 as one 32-bit timer, whose interrupt is Timer3's;
 *     vector 213 (the last, an ADC's), flagged by the program, reached
 *     through OFF213 -- which the startup's table must reach;
 *   - an interrupt's priority against Status.IPL: a request at the
 *     current level waits; requests at one priority taken by
 *     subpriority, then in the natural order;
 *   - the ports: LATA's LEDs, PORTA reading what is driven, a write to
 *     PORTA landing in LATA, a button read on RF8 (S4);
 *   - the ADC: AN4, the TC1047A at the temperature the run sets, and
 *     AN45, the potentiometer, as ADC0's alternate input; an input of
 *     the shared ADC, which is off, is not converted;
 *   - the parallel master port: the LCD's two lines;
 *   - SYSKEY's lock: PB3DIV written locked, and unlocked.
 * The interrupt handlers are the assembly the test writes (isr.S): each
 * takes the stack from the interrupted register set (rdpgpr), saves what
 * a C function may change, calls isr_N and returns with eret. */
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

volatile uint32_t n_cs0, set_cs0, n_t1, n_ct, n_t3, n_213;
volatile uint32_t order;                        /* the vectors taken, a byte each */

void isr_1(void)
{
    order = order << 8 | 1;
    n_cs0++;
    set_cs0 = _CP0_GET_SRSCTL() & 15;
    _CP0_BIC_CAUSE(1u << 8);
    IFS0CLR = _IFS0_CS0IF_MASK;
}

void isr_4(void)
{
    order = order << 8 | 4;
    n_t1++;
    IFS0CLR = _IFS0_T1IF_MASK;
}

void isr_0(void)
{
    n_ct++;
    _CP0_SET_COMPARE(_CP0_GET_COUNT() + 3000);
    IFS0CLR = _IFS0_CTIF_MASK;
}

void isr_14(void)
{
    order = order << 8 | 14;
    n_t3++;
    IFS0CLR = _IFS0_T3IF_MASK;
}

void isr_213(void)
{
    n_213++;
    IFS6CLR = 1u << (213 - 192);
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

static uint32_t convert(int an)
{
    ADCCON3bits.ADINSEL = (unsigned)an;
    ADCCON3bits.RQCNVRT = 1;
    uint32_t r = an < 32 ? ADCDSTAT1 >> an & 1 : ADCDSTAT2 >> (an - 32) & 1;
    if (!r)
        return 0xffffffffu;
    return (&ADCDATA0)[an];
}

int main(void)
{
    U1MODE = 0;
    U1STASET = _U1STA_UTXEN_MASK;
    U1MODESET = _U1MODE_ON_MASK;
    puts_("pic32-fw\n");

    /* the DSP ASE's accumulator 1 */
    uint32_t hi;
    __asm__ volatile("mthi %1, $ac1\n\tmfhi %0, $ac1" : "=r"(hi) : "r"(0x12345678u));
    line("ac1", hi);

    INTCONSET = _INTCON_MVEC_MASK;
    PRISS = 0x76543210u;                        /* priority n: register set n */
    __builtin_enable_interrupts();

    /* CS0, priority 1: in shadow set 1 */
    IPC0SET = 1u << _IPC0_CS0IP_POSITION;
    IEC0SET = _IEC0_CS0IE_MASK;
    _CP0_BIS_CAUSE(1u << 8);
    line("cs0 taken", n_cs0);
    line("cs0 register set", set_cs0);

    /* a request at the current level waits until the level drops */
    uint32_t st = _CP0_GET_STATUS();
    _CP0_SET_STATUS((st & ~(0x7fu << 10)) | (1u << 10));
    _CP0_BIS_CAUSE(1u << 8);
    line("cs0 at IPL 1", n_cs0);
    _CP0_SET_STATUS(st);
    line("cs0 at IPL 0", n_cs0);

    /* Timer1: 1000 counts at 1:8, three interrupts */
    T1CON = 0;
    TMR1 = 0;
    PR1 = 999;
    T1CONbits.TCKPS = 1;
    IPC1SET = 2u << _IPC1_T1IP_POSITION;
    IEC0SET = _IEC0_T1IE_MASK;
    uint32_t c0 = _CP0_GET_COUNT();
    T1CONSET = _T1CON_ON_MASK;
    while (n_t1 < 3)
        __asm__ volatile("wait");
    uint32_t c1 = _CP0_GET_COUNT();
    T1CONCLR = _T1CON_ON_MASK;
    IEC0CLR = _IEC0_T1IE_MASK;
    line("timer1 interrupts", n_t1);
    /* 3 x 1000 counts x 8 x PB3's 2 = 48000 cycles: Count's 24000 */
    line("timer1 Count/1000", (c1 - c0) / 1000);

    /* the core timer */
    IPC0SET = 4u << _IPC0_CTIP_POSITION;
    IFS0CLR = _IFS0_CTIF_MASK;
    _CP0_SET_COMPARE(_CP0_GET_COUNT() + 3000);
    IEC0SET = _IEC0_CTIE_MASK;
    while (n_ct < 2)
        __asm__ volatile("wait");
    IEC0CLR = _IEC0_CTIE_MASK;
    line("core timer interrupts", n_ct);

    /* Timer2 and Timer3 as one: 0x10001 counts, Timer3's interrupt */
    T2CON = 0;
    T3CON = 0;
    TMR2 = 0;
    TMR3 = 0;
    PR2 = 0;
    PR3 = 1;
    T2CONSET = _T2CON_T32_MASK;
    IPC3SET = 3u << _IPC3_T3IP_POSITION;
    IEC0SET = _IEC0_T3IE_MASK;
    T2CONSET = _T2CON_ON_MASK;
    while (!n_t3)
        __asm__ volatile("wait");
    line("timer2/3 interrupts", n_t3);
    line("timer3 after the wrap", TMR3);
    T2CONCLR = _T2CON_ON_MASK;

    /* three requests at once, at priority 2: Timer3's at subpriority 1
     * first, then CS0 before Timer1, in the natural order (vector 1
     * before vector 4) */
    __builtin_disable_interrupts();
    IPC0CLR = _IPC0_CS0IP_MASK | _IPC0_CS0IS_MASK;
    IPC0SET = 2u << _IPC0_CS0IP_POSITION;
    IPC1CLR = _IPC1_T1IP_MASK | _IPC1_T1IS_MASK;
    IPC1SET = 2u << _IPC1_T1IP_POSITION;
    IPC3CLR = _IPC3_T3IP_MASK | _IPC3_T3IS_MASK;
    IPC3SET = 2u << _IPC3_T3IP_POSITION | 1u << _IPC3_T3IS_POSITION;
    IEC0SET = _IEC0_CS0IE_MASK | _IEC0_T1IE_MASK | _IEC0_T3IE_MASK;
    order = 0;
    IFS0SET = _IFS0_CS0IF_MASK | _IFS0_T1IF_MASK | _IFS0_T3IF_MASK;
    __builtin_enable_interrupts();
    line("taken in order", order);
    IEC0CLR = _IEC0_CS0IE_MASK | _IEC0_T1IE_MASK | _IEC0_T3IE_MASK;

    /* vector 213, through OFF213 */
    IPC53SET = 5u << 10;                        /* 213 is IPC53's second */
    IEC6SET = 1u << (213 - 192);
    IFS6SET = 1u << (213 - 192);
    line("vector 213 taken", n_213);
    line("INTSTAT", INTSTAT);

    /* the ports */
    ANSELA = 0;
    TRISA = 0xff00u;
    LATA = 0x55u;
    line("PORTA", PORTA & 0xffu);
    PORTA = 0x0fu;
    line("LATA after PORTA=0x0f", LATA & 0xffu);
    LATA = 0;
    TRISFSET = 1u << 8;
    line("S4 (RF8)", PORTF >> 8 & 1);

    /* the ADC: the TC1047A on AN4, the potentiometer on AN45 */
    ADCCON1 = 0x600000u;                        /* 12 bits, shared ADC */
    ADC4TIME = 0x3000001u;                      /* ADC4: 12 bits */
    ADCCON3 = 0;
    ADCCON1bits.ON = 1;
    while (!ADCCON2bits.BGVRRDY)
        ;
    ADC0TIME = 0x3000001u;                      /* ADC0: 12 bits */
    ADCTRGMODEbits.SH0ALT = 1;                  /* ADC0 converts AN45 */
    ADCANCON |= 1u << 0 | 1u << 4;
    while (!(ADCANCONbits.WKRDY0 && ADCANCONbits.WKRDY4))
        ;
    ADCCON3 |= 1u << 16 | 1u << 20;             /* DIGEN0, DIGEN4 */
    line("AN4", convert(4));
    line("AN45 (ADC0's alternate)", convert(0));
    line("AN6 (ADC7 off)", convert(6));

    /* the LCD, 8 bits, two lines */
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
    lcd_text("PIC32MZ EF");
    lcd(0, 0x02);

    /* PB3DIV: locked, then unlocked */
    PB3DIV = 0x8803u;
    line("PB3DIV locked", PB3DIV);
    SYSKEY = 0;
    SYSKEY = 0xaa996655u;
    SYSKEY = 0x556699aau;
    PB3DIV = 0x8803u;
    SYSKEY = 0x33333333u;
    line("PB3DIV unlocked", PB3DIV);

    puts_("==DONE==\n");
    for (;;)
        ;
}
