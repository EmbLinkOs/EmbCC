/* the PC-size-dependent parts of an AVR program, on the Mega's parts */
#include <avr/interrupt.h>
#include <avr/io.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
static void putc0(char c) { while (!(UCSR0A & (1 << UDRE0))) ; UDR0 = (unsigned char)c; }
int putchar(int c) { putc0((char)c); return c; }
static long many(long a, long b, long c, long d, long e, long f, long g, long h, int i, char j)
{ return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8 + i * 9 + j * 10; }
static int vsum(int n, ...) { va_list ap; va_start(ap, n); int t = 0; for (int k = 0; k < n; k++) t = t * 10 + va_arg(ap, int); va_end(ap); return t; }
static jmp_buf jb;
static __attribute__((noinline)) void deep(int n) { if (n == 0) longjmp(jb, 42); deep(n - 1); }
static int ctor;
__attribute__((constructor)) static void c0(void) { ctor = 7; }
static const char table[] = "rodata copied";
static int data_init = 1234;
static volatile unsigned ticks;
ISR(TIMER1_COMPA_vect) { ticks++; }
static void *ra(void) { return __builtin_return_address(0); }
int main(void)
{
    UCSR0B = 1 << TXEN0;
    printf("many=%ld vsum=%d ctor=%d data=%d %s\n", many(1, 2, 3, 4, 5, 6, 7, 8, 9, 10), vsum(4, 1, 2, 3, 4),
           ctor, data_init, table);
    int r = setjmp(jb);
    if (r == 0) { deep(5); printf("NOT REACHED\n"); }
    printf("longjmp gave %d\n", r);
    printf("return address %s\n", ra() ? "nonzero" : "ZERO");
    OCR1A = 2000; TCCR1B = (1 << WGM12) | (1 << CS10); TIMSK1 = 1 << OCIE1A; sei();
    while (ticks < 5) ;
    cli();
    printf("interrupts: %u\n<<END>>\n", ticks);
    for (;;) ;
}
