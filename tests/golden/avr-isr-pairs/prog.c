/* prog.c -- tests/golden/avr-isr-pairs.sh: what an AVR interrupt handler
 * must give back.
 *
 * work() keeps sixteen bytes live across a loop -- eight 16-bit values,
 * each byte different -- so the allocator puts them in the call-saved
 * pairs, r10-r17 among them. Timer/Counter1's compare match interrupts
 * it dozens of times, and the handler multiplies a long long, which
 * loads __muldi3's second argument into r10-r17: the handler pushes
 * those pairs and must pop each one back, both halves, in its place.
 *
 * work(N) runs twice, with the timer off and on; the two results must be
 * the same, and the interrupt must have come many times during the
 * second. It prints both results, the ticks, and a verdict. */
void putn(long v);
void puts_(const char *s);

volatile unsigned ticks;
volatile long long acc = 7;

__attribute__((signal)) void __vector_11(void)
{
    acc = acc * 3 + 1;
    ticks++;
}

__attribute__((noinline)) unsigned work(unsigned n)
{
    unsigned a = 0x0102, b = 0x0304, c = 0x0506, d = 0x0708;
    unsigned e = 0x090a, f = 0x0b0c, g = 0x0d0e, h = 0x0f10;
    for (unsigned i = 0; i < n; i++) {
        a += 0x0101;
        b = (unsigned)(b + a + 0x0203);
        c ^= b;
        d = (unsigned)(d + (c >> 3));
        e = (unsigned)(e - d);
        f ^= (unsigned)(e << 1);
        g = (unsigned)(g + (f | 1));
        h = (unsigned)(h + (g ^ a));
    }
    return (unsigned)(a ^ (b << 1) ^ (c << 2) ^ (d << 3) ^ (e << 4) ^
                      (f << 5) ^ (g << 6) ^ (h << 7));
}

static void timer_start(void)
{
    *(volatile unsigned char *)0x89 = 0x0f;         /* OCR1AH: 4000 */
    *(volatile unsigned char *)0x88 = 0xa0;         /* OCR1AL */
    *(volatile unsigned char *)0x80 = 0;            /* TCCR1A */
    *(volatile unsigned char *)0x6f = 2;            /* TIMSK1: OCIE1A */
    *(volatile unsigned char *)0x81 = 9;            /* TCCR1B: CTC, /1 */
    __asm__ volatile("sei");
}

static void timer_stop(void)
{
    __asm__ volatile("cli");
    *(volatile unsigned char *)0x81 = 0;
    *(volatile unsigned char *)0x6f = 0;
}

int main(void)
{
    unsigned quiet = work(3000), busy;
    timer_start();
    busy = work(3000);
    timer_stop();
    putn((long)quiet);
    putn((long)busy);
    putn((long)ticks);
    puts_(quiet == busy && ticks >= 20 ? "same\n" : "DIFFERENT\n");
    return 0;
}
