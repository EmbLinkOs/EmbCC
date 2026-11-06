#include <avr/io.h>
long many(long a, long b, long c, long d, long e, long f, long g, long h, int i, char j);
long call_gcc(void);
long gcc_many(long a, long b, long c, long d, long e, long f, long g, long h, int i, char j)
{ return a * 10 + b * 9 + c * 8 + d * 7 + e * 6 + f * 5 + g * 4 + h * 3 + i * 2 + j; }
static void put(char c) { while (!(UCSR0A & (1 << UDRE0))) ; UDR0 = (unsigned char)c; }
static void puts0(const char *s) { while (*s) put(*s++); }
static void dec(long v) { char d[12]; int k = 0; do d[k++] = (char)('0' + v % 10); while (v /= 10); while (k) put(d[--k]); }
int main(void)
{
    UCSR0B = 1 << TXEN0;
    puts0("gcc calls embcc: "); dec(many(1, 2, 3, 4, 5, 6, 7, 8, 9, 10));
    puts0(" (385); embcc calls gcc: "); dec(call_gcc()); puts0(" (220)\n<<END>>\n");
    for (;;) ;
}
