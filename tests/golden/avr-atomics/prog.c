/* AVR atomics: every operation at one, two and four bytes, which the
 * backend does with interrupts masked (src/arch/avr/codegen.c), and the
 * host computes the same values (tests/golden/avr-atomics.sh). On the
 * board, an interrupt then races main for one four-byte counter: Timer1
 * matches every 200 cycles and adds 1, main atomically adds 3 thirty
 * thousand times, and the count must come out exact -- an increment that
 * an interrupt can split loses some. */
void writec(int c);
void puts_(const char *s);
void putn(long v);

static unsigned char b8;
static unsigned short h16;
static __UINT32_TYPE__ w32;
static signed char s8;
static short s16;

__attribute__((noinline)) unsigned short add_h(unsigned short *p, unsigned short v)
{ return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }
__attribute__((noinline)) int cas_w(__UINT32_TYPE__ *p, __UINT32_TYPE__ *e, __UINT32_TYPE__ d)
{ return __atomic_compare_exchange_n(p, e, d, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }

static void functional(void)
{
    b8 = 200; h16 = 60000u; w32 = 4000000000ul;
    putn(__atomic_fetch_add(&b8, 100, __ATOMIC_SEQ_CST)); putn(b8);
    putn(__atomic_fetch_sub(&h16, 4464, __ATOMIC_SEQ_CST)); putn(h16);
    putn((long)(__atomic_fetch_add(&w32, 500000000ul, __ATOMIC_SEQ_CST) >> 8));
    putn((long)(w32 >> 8));
    putn(__atomic_fetch_and(&h16, 0x0ff0, __ATOMIC_SEQ_CST)); putn(h16);
    putn(__atomic_fetch_or(&b8, 0x81, __ATOMIC_SEQ_CST)); putn(b8);
    putn((long)(__atomic_fetch_xor(&w32, 0xffff0000ul, __ATOMIC_SEQ_CST) >> 8));
    putn((long)(w32 >> 8));
    putn(__atomic_fetch_nand(&b8, 0x0f, __ATOMIC_SEQ_CST)); putn(b8);
    putn(__atomic_exchange_n(&h16, 0x1234, __ATOMIC_SEQ_CST)); putn(h16);
    writec('\n');

    unsigned char e8 = 7;
    putn(__atomic_compare_exchange_n(&b8, &e8, 9, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
    putn(e8); putn(b8);
    putn(__atomic_compare_exchange_n(&b8, &e8, 9, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
    putn(e8); putn(b8);
    putn(__sync_val_compare_and_swap(&h16, 0x1234, 0x4321)); putn(h16);
    putn(__sync_val_compare_and_swap(&h16, 0x1234, 0x5555)); putn(h16);
    __UINT32_TYPE__ e32 = 1;
    putn(cas_w(&w32, &e32, 2)); putn((long)(e32 >> 8));
    putn(cas_w(&w32, &e32, 77)); putn((long)w32);
    putn(add_h(&h16, 0x100)); putn(h16);
    writec('\n');

    __atomic_store_n(&h16, 0xBEEF, __ATOMIC_SEQ_CST);
    putn(__atomic_load_n(&h16, __ATOMIC_SEQ_CST));
    __atomic_store_n(&w32, 123456789ul, __ATOMIC_SEQ_CST);
    putn((long)__atomic_load_n(&w32, __ATOMIC_SEQ_CST));
    s8 = -5; s16 = -300;
    putn(__atomic_fetch_add(&s8, 10, __ATOMIC_SEQ_CST)); putn(s8);
    putn(__atomic_fetch_sub(&s16, 1, __ATOMIC_SEQ_CST)); putn(s16);
    writec('\n');
}

#ifdef __AVR__
/* Timer1 in CTC mode, a compare-match interrupt every 200 cycles (QEMU's
 * ATmega328P has the 16-bit timers; Timer0 is not modelled) */
#define TCCR1B (*(volatile unsigned char *)0x81u)
#define OCR1AL (*(volatile unsigned char *)0x88u)
#define OCR1AH (*(volatile unsigned char *)0x89u)
#define TIMSK1 (*(volatile unsigned char *)0x6Fu)
static volatile __UINT32_TYPE__ shared;
static volatile unsigned int isrs;
__attribute__((signal)) void __vector_11(void)   /* TIMER1_COMPA */
{
    shared = shared + 1;
    isrs++;
}

static void race(void)
{
    OCR1AH = 0;
    OCR1AL = 200;
    TCCR1B = 0x09;              /* CTC on OCR1A, clk/1 */
    TIMSK1 = 0x02;              /* OCIE1A */
    __asm__ volatile("sei" ::: "memory");
    for (long k = 0; k < 30000; k++)
        __atomic_fetch_add(&shared, 3, __ATOMIC_SEQ_CST);
    __asm__ volatile("cli" ::: "memory");
    TIMSK1 = 0;
    puts_(shared == 90000ul + isrs && isrs > 50 ? "race exact" : "race LOST");
    writec('\n');
}
#else
static void race(void) { puts_("race exact\n"); }
#endif

int main(void)
{
    functional();
    race();
    puts_("==END==\n");
    return 0;
}
