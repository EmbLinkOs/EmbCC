/* pic32mx-fw.c -- a PIC32MX program for tests/golden/embsim-pic32mx.sh,
 * built by EmbCC (--mcu=32MX795F512L) with its own startup, its interrupt
 * stubs written by the test in each vector's 32-byte slot. It prints on
 * UART1 what each part did, by the family reference manual (sections 2,
 * 6, 8, 12 and 14), and ends with ==DONE==:
 *
 *   - the core: an M4K (PRId 0x87), MIPS16e and EJTAG (Config1), EIC and
 *     vectored interrupts (Config3), one shadow set (SRSCtl.HSS 1);
 *   - DEVID, the part's ID with its revision;
 *   - OSCCON.PBDIV out of reset: the configuration words' FPBDIV (4);
 *   - single-vector mode (INTCON.MVEC 0): the core's software interrupt
 *     CS0 taken at vector 0's slot, INTSTAT VEC 0 and SRIPL, in the
 *     shadow set with INTCON.SS0;
 *   - multi-vector mode: CS0 at priority 6 in the shadow set (FSRSSEL
 *     6) and CS1 at 3 in the first set; the two pending at one priority
 *     taken in the natural order (vector 1 first), then by subpriority
 *     (CS1's higher); Input Capture 1's error (request 61, vector 5) and
 *     Timer2 (request 8, vector 8) at one priority: vector 5 first, the
 *     lower vector, not the lower request;
 *   - Timer1 on the peripheral bus (SYSCLK / 4), its interrupt each
 *     PR1 + 1 counts: the core timer's Count (SYSCLK / 2) between two;
 *   - UART1's transmit request (IRQ 28) at UART1's vector, 24, which
 *     its error and receive requests share: INTSTAT VEC 24;
 *   - the change notices: RD6 (CN15) driven up and down, two interrupts
 *     at vector 26 (IRQ 32); CNPUE's pull-up on RD7 (CN16);
 *   - AD1PCFG: RB0 analog (reads 0) until its bit makes it digital;
 *   - SYSKEY: OSCCON.PBDIV written locked (kept), and unlocked. */
#include <stdint.h>
#include <xc.h>

#pragma config FPBDIV = DIV_4
#pragma config FSRSSEL = PRIORITY_6
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

volatile uint32_t order, css1, css2, single_stat, single_css, n_single;
volatile uint32_t n_t1, t1_at[4], u1_stat, n_u1, n_cn;

/* single-vector mode's every interrupt (vector 0's slot) */
void isr_0(void)
{
    n_single++;
    single_stat = INTSTAT;
    single_css = _CP0_GET_SRSCTL() & 15;
    _CP0_BIC_CAUSE(1u << 8);
    IFS0CLR = _IFS0_CS0IF_MASK;
}

void isr_1(void)
{
    order = order << 8 | 1;
    css1 = _CP0_GET_SRSCTL() & 15;
    _CP0_BIC_CAUSE(1u << 8);
    IFS0CLR = _IFS0_CS0IF_MASK;
}

void isr_2(void)
{
    order = order << 8 | 2;
    css2 = _CP0_GET_SRSCTL() & 15;
    _CP0_BIC_CAUSE(1u << 9);
    IFS0CLR = _IFS0_CS1IF_MASK;
}

void isr_5(void)
{
    order = order << 8 | 5;
    IFS1CLR = _IFS1_IC1EIF_MASK;
}

void isr_8(void)
{
    order = order << 8 | 8;
    IFS0CLR = _IFS0_T2IF_MASK;
}

void isr_4(void)
{
    if (n_t1 < 4)
        t1_at[n_t1] = _CP0_GET_COUNT();
    n_t1++;
    IFS0CLR = _IFS0_T1IF_MASK;
}

void isr_24(void)
{
    u1_stat = INTSTAT;
    n_u1++;
    IEC0CLR = _IEC0_U1TXIE_MASK;
    IFS0CLR = _IFS0_U1TXIF_MASK;
}

void isr_26(void)
{
    n_cn++;
    (void)PORTD;
    IFS1CLR = _IFS1_CNIF_MASK;
}

static void wait(volatile uint32_t *n, uint32_t until)
{
    while (*n < until)
        ;
}

int main(void)
{
    U1MODE = 0;
    U1STASET = _U1STA_UTXEN_MASK;
    U1MODESET = _U1MODE_ON_MASK;
    puts_("pic32mx-fw\n");

    line("PRId", _CP0_GET_PRID());
    line("Config1", _CP0_GET_CONFIG1());
    line("Config3", _CP0_GET_CONFIG3());
    line("SRSCtl.HSS", _CP0_GET_SRSCTL() >> 26 & 15);
    line("DEVID", DEVID);
    line("OSCCON.PBDIV", OSCCONbits.PBDIV);

    /* single-vector mode, the shadow set for every interrupt */
    INTCONSET = _INTCON_SS0_MASK;
    IPC0SET = 1u << _IPC0_CS0IP_POSITION;
    IEC0SET = _IEC0_CS0IE_MASK;
    __builtin_enable_interrupts();
    _CP0_BIS_CAUSE(1u << 8);
    wait(&n_single, 1);
    line("single: INTSTAT", single_stat);
    line("single: register set", single_css);

    /* multi-vector mode: CS0 at 6 (FSRSSEL's), CS1 at 3 */
    __builtin_disable_interrupts();
    INTCONCLR = _INTCON_SS0_MASK;
    INTCONSET = _INTCON_MVEC_MASK;
    IPC0CLR = 7u << _IPC0_CS0IP_POSITION;
    IPC0SET = 6u << _IPC0_CS0IP_POSITION | 3u << _IPC0_CS1IP_POSITION;
    IEC0SET = _IEC0_CS1IE_MASK;
    __builtin_enable_interrupts();
    _CP0_BIS_CAUSE(1u << 8);
    _CP0_BIS_CAUSE(1u << 9);
    while (order != 0x0102)
        ;
    line("multi: CS0's register set", css1);
    line("multi: CS1's register set", css2);
    /* both at 2: vector 1 first; then CS1's subpriority 3 first */
    __builtin_disable_interrupts();
    IPC0CLR = 7u << _IPC0_CS0IP_POSITION | 7u << _IPC0_CS1IP_POSITION;
    IPC0SET = 2u << _IPC0_CS0IP_POSITION | 2u << _IPC0_CS1IP_POSITION;
    order = 0;
    _CP0_BIS_CAUSE(3u << 8);
    __builtin_enable_interrupts();
    while (order < 0x100)
        ;
    line("natural order", order);
    __builtin_disable_interrupts();
    IPC0SET = 3u << _IPC0_CS1IS_POSITION;
    order = 0;
    _CP0_BIS_CAUSE(3u << 8);
    __builtin_enable_interrupts();
    while (order < 0x100)
        ;
    line("subpriority", order);
    /* request 61 (vector 5) and request 8 (vector 8), both at 2 */
    __builtin_disable_interrupts();
    IPC1SET = 2u << _IPC1_IC1IP_POSITION;
    IPC2SET = 2u << _IPC2_T2IP_POSITION;
    IEC1SET = _IEC1_IC1EIE_MASK;
    IEC0SET = _IEC0_T2IE_MASK;
    order = 0;
    IFS0SET = _IFS0_T2IF_MASK;
    IFS1SET = _IFS1_IC1EIF_MASK;
    __builtin_enable_interrupts();
    while (order < 0x100)
        ;
    line("vectors before requests", order);
    IEC1CLR = _IEC1_IC1EIE_MASK;
    IEC0CLR = _IEC0_T2IE_MASK;

    /* Timer1: SYSCLK / 4, PR1 + 1 = 1000 counts a period */
    PR1 = 999;
    TMR1 = 0;
    IPC1SET = 4u << _IPC1_T1IP_POSITION;
    IEC0SET = _IEC0_T1IE_MASK;
    T1CONSET = _T1CON_ON_MASK;
    wait(&n_t1, 3);
    T1CONCLR = _T1CON_ON_MASK;
    line("Timer1: Count a period", t1_at[2] - t1_at[1]);

    /* UART1's transmit request, at UART1's vector */
    IPC6SET = 2u << _IPC6_U1IP_POSITION;
    IEC0SET = _IEC0_U1TXIE_MASK;
    wait(&n_u1, 1);
    line("UART1 TX: INTSTAT", u1_stat);

    /* change notices: RD6 (CN15) up and down */
    TRISDCLR = 1u << 6;
    LATDCLR = 1u << 6;
    (void)PORTD;
    CNENSET = 1u << 15;
    CNCONSET = _CNCON_ON_MASK;
    IPC6SET = 1u << _IPC6_CNIP_POSITION;
    IEC1SET = _IEC1_CNIE_MASK;
    LATDSET = 1u << 6;
    LATDCLR = 1u << 6;
    wait(&n_cn, 2);
    line("change notices", n_cn);
    TRISDSET = 1u << 7;
    line("RD7 without its pull-up", PORTDbits.RD7);
    CNPUESET = 1u << 16;                            /* CN16 */
    line("RD7 with it", PORTDbits.RD7);

    /* AD1PCFG: RB0 analog, then digital */
    TRISBCLR = 1u;
    LATBSET = 1u;
    line("RB0 analog", PORTBbits.RB0);
    AD1PCFGSET = 1u;
    line("RB0 digital", PORTBbits.RB0);

    /* SYSKEY: PBDIV locked, then unlocked */
    OSCCONCLR = 3u << 19;
    line("PBDIV locked", OSCCONbits.PBDIV);
    SYSKEY = 0;
    SYSKEY = 0xaa996655u;
    SYSKEY = 0x556699aau;
    OSCCONCLR = 3u << 19;
    SYSKEY = 0;
    line("PBDIV unlocked", OSCCONbits.PBDIV);

    puts_("==DONE==\n");
    for (;;)
        ;
}
