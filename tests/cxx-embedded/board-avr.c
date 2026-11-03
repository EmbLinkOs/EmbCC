/* The AVR boards' output: lib/avr's printf and puts send each character
 * to putchar, which this gives the harness's UART (tests/harness/avr/io.c). */
extern void writec(int c);

int putchar(int c)
{
    writec(c);
    return c;
}
