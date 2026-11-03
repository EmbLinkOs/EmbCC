/* `REG |= BIT` and `REG &= ~BIT` on AVR's I/O registers, which the
 * backend makes one sbi or cbi where the register is in their reach
 * (0x20-0x3f: DDRB here) and lds/or/sts past the I/O space (OCR1BL) --
 * every bit set and cleared one at a time, and two-bit masks that stay
 * or/and, each checked against a copy kept in RAM. Run on EmbSim
 * (tests/golden/avr-bit-io.sh): QEMU's uno reads every register of the
 * I/O space as 0. */

void puts_(const char *s);
void putn(long v);

#define R0 (*(volatile unsigned char *)0x24)   /* DDRB: sbi/cbi reach it */
#define R1 (*(volatile unsigned char *)0x8a)   /* OCR1BL: lds/sts */

static int bad;
static unsigned char w0, w1;

#define SET(r, w, b) (r |= (unsigned char)(1u << (b)), w |= (unsigned char)(1u << (b)), bad |= r != w)
#define CLR(r, w, b) (r &= (unsigned char)~(1u << (b)), w &= (unsigned char)~(1u << (b)), bad |= r != w)
#define EACH(m, r, w) (m(r, w, 0), m(r, w, 1), m(r, w, 2), m(r, w, 3), \
                       m(r, w, 4), m(r, w, 5), m(r, w, 6), m(r, w, 7))

int main(void)
{
    R0 = 0; R1 = 0;
    EACH(SET, R0, w0);
    EACH(CLR, R0, w0);
    EACH(SET, R1, w1);
    EACH(CLR, R1, w1);
    /* back up, then down in another order */
    SET(R0, w0, 5); SET(R0, w0, 0); SET(R0, w0, 7); CLR(R0, w0, 0);
    SET(R1, w1, 2); SET(R1, w1, 6); CLR(R1, w1, 2);
    /* two bits at once: or and and, not sbi and cbi */
    R0 |= 0x06; w0 |= 0x06; bad |= R0 != w0;
    R0 &= 0xf9; w0 &= 0xf9; bad |= R0 != w0;
    R1 |= 0x81; w1 |= 0x81; bad |= R1 != w1;
    bad |= R0 != 0xa0 || R1 != 0xc1;
    putn(bad ? 1 : 42);
    puts_("\n==END==\n");
    for (;;)
        ;
}
