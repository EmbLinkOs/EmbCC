/* AVR's __flash, run on the ATmega328P: tests/golden/avr-flash.sh. Every
 * table here stays in program memory (.progmem.data) and every read is
 * an LPM: a string, int, long and long long tables (signed values
 * sign-extended),
 * a table of structs read member by member, a table of __flash pointers
 * that is itself in flash, and a function taking a __flash pointer. The
 * index is volatile, so each read happens at run time. */
void writec(int c);
void puts_(const char *s);
void putn(long v);

static const __flash char msg[] = "flash ok";
const __flash int table[6] = { 1, -2, 300, -4000, 5, 32767 };
const __flash long big[3] = { 100000L, -200000L, 2147483647L };
const __flash signed char small[4] = { -1, 2, -3, 127 };
const __flash long long wide[2] = { 0x0102030405060708LL, -5LL };

struct pin { unsigned char port; unsigned char bit; int id; };
static const __flash struct pin pins[3] = { { 1, 2, 10 }, { 3, 4, 20 },
                                            { 5, 6, 30 } };

static const __flash char n0[] = "zero";
static const __flash char n1[] = "one";
static const __flash char *const __flash names[2] = { n0, n1 };

static volatile int one = 1;

static void puts_P(const __flash char *s)
{
    char c;
    while ((c = *s++) != 0)
        writec(c);
}

int main(void)
{
    puts_P(msg);
    writec('\n');
    long s = 0;
    for (int i = 0; i < 6; i++)
        s += table[i];
    putn(s);
    putn(big[one] + big[one + 1]);
    putn(small[one - 1] + small[one + 2]);
    int ids = 0;
    for (int i = 0; i < 3; i++)
        ids += pins[i].id * pins[i].port + pins[i].bit;
    putn(ids);
    writec('\n');
    putn((long)(wide[one - 1] >> 32));
    putn((long)(wide[one - 1] & 0xffffffffLL));
    putn((long)wide[one]);
    writec('\n');
    puts_P(names[one]);
    writec(' ');
    puts_P(names[one - 1]);
    puts_("\n==END==\n");
    return 0;
}
