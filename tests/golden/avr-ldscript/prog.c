/* What a linker script changes on AVR, run on the ATmega328P:
 * tests/golden/avr-ldscript.sh links this once with m328p.ld (avr-libc's
 * layout, data at 0x800100 in the linker's view) and once with
 * -Ttext/-Tdata, and both must print what the host prints. Every kind of
 * address the script moves is used: read-only tables (in RAM, copied by
 * the startup), initialised data, a pointer to data stored in data (a
 * 16-bit address the linker truncates from 0x8001xx), a table of
 * function pointers (word addresses), and .bss. */
void writec(int c);
void puts_(const char *s);
void putn(long v);

static const char *const names[] = { "zero", "one", "two", "three" };
static const unsigned char squares[8] = { 0, 1, 4, 9, 16, 25, 36, 49 };
static int counter = 7;
static long acc = 100000L;
static int *const pcounter = &counter;         /* data -> data */
static long big[4];                             /* .bss */
static char buf[16];

static int add1(int x) { return x + 1; }
static int dbl(int x) { return x * 2; }
static int neg(int x) { return -x; }
static int (*const ops[3])(int) = { add1, dbl, neg };  /* data -> code */

int main(void)
{
    int i;
    for (i = 0; i < 4; i++) {
        puts_(names[i]);
        writec(' ');
    }
    writec('\n');
    long s = 0;
    for (i = 0; i < 8; i++)
        s += squares[i];
    putn(s);
    *pcounter += 5;
    putn(counter);
    big[3] = acc * 3;
    putn(big[0] + big[3]);
    for (i = 0; i < 3; i++)
        putn(ops[i](counter));
    for (i = 0; i < 15; i++)
        buf[i] = (char)('a' + i);
    puts_(buf);
    puts_("\n==END==\n");
    return 0;
}
