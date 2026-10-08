/* AVR atomics: every operation at one, two, four and eight bytes, which
 * the backend does with interrupts masked (src/arch/avr/codegen.c), and
 * the host computes the same values (tests/golden/avr-atomics.sh) --
 * the builtins, the _Atomic operators, pointers and atomic_flag. On the
 * board, an interrupt then races main: Timer1 matches every 200 cycles
 * and adds 1 to a four- and an eight-byte counter that main atomically
 * adds 3 to until two hundred interrupts have come, and both counts must come out exact --
 * an increment that an interrupt can split loses some. Then the handler
 * writes an eight-byte value whose halves are equal while main atomically
 * loads it, and main atomically stores one the handler checks: neither
 * may ever see a torn value. */
void writec(int c);
void puts_(const char *s);
void putn(long v);

static unsigned char b8;
static unsigned short h16;
static __UINT32_TYPE__ w32;
static signed char s8;
static short s16;
static unsigned long long d64;
static long long sd64;

/* eight bytes in 16-bit pieces: the same on the host and on AVR, whose
 * long is four bytes */
static void put64(unsigned long long v)
{
    for (int k = 48; k >= 0; k -= 16)
        putn((long)((v >> k) & 0xffff));
}

__attribute__((noinline)) unsigned long long add_d(unsigned long long *p, unsigned long long v)
{ return __atomic_fetch_add(p, v, __ATOMIC_RELAXED); }
__attribute__((noinline)) int cas_d(unsigned long long *p, unsigned long long *e, unsigned long long d)
{ return __atomic_compare_exchange_n(p, e, d, 1, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE); }

__attribute__((noinline)) unsigned short add_h(unsigned short *p, unsigned short v)
{ return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }
__attribute__((noinline)) int cas_w(__UINT32_TYPE__ *p, __UINT32_TYPE__ *e, __UINT32_TYPE__ d)
{ return __atomic_compare_exchange_n(p, e, d, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }

static void eight(void)
{
    d64 = 0x00ff00ff00ffffffull;
    put64(__atomic_fetch_add(&d64, 1, __ATOMIC_SEQ_CST)); put64(d64);
    put64(__atomic_fetch_sub(&d64, 0x0100000000000001ull, __ATOMIC_SEQ_CST)); put64(d64);
    put64(__atomic_fetch_and(&d64, 0xf0f0f0f0f0f0f0f0ull, __ATOMIC_ACQUIRE)); put64(d64);
    put64(__atomic_fetch_or(&d64, 0x0102030405060708ull, __ATOMIC_RELEASE)); put64(d64);
    put64(__atomic_fetch_xor(&d64, 0xffffffff00000000ull, __ATOMIC_SEQ_CST)); put64(d64);
    put64(__atomic_fetch_nand(&d64, 0x00000000ffff0000ull, __ATOMIC_SEQ_CST)); put64(d64);
    put64(__atomic_exchange_n(&d64, 0x8000000000000001ull, __ATOMIC_SEQ_CST)); put64(d64);
    put64(__atomic_add_fetch(&d64, 0x7fffffffffffffffull, __ATOMIC_SEQ_CST)); put64(d64);
    writec('\n');

    unsigned long long e = 5;
    d64 = 0x1122334455667788ull;
    putn(cas_d(&d64, &e, 9)); put64(e); put64(d64);
    putn(cas_d(&d64, &e, 9)); put64(e); put64(d64);
    put64(__sync_val_compare_and_swap(&d64, 9, 0xfedcba9876543210ull)); put64(d64);
    put64(__sync_val_compare_and_swap(&d64, 9, 1)); put64(d64);
    putn(__sync_bool_compare_and_swap(&d64, 0xfedcba9876543210ull, 3)); put64(d64);
    put64(add_d(&d64, 0xffffffffffffffffull)); put64(d64);
    __atomic_store_n(&d64, 0xa5a5a5a55a5a5a5aull, __ATOMIC_SEQ_CST);
    put64(__atomic_load_n(&d64, __ATOMIC_SEQ_CST));
    unsigned long long r, v = 0x0123456789abcdefull;
    __atomic_store(&d64, &v, __ATOMIC_RELEASE);
    __atomic_load(&d64, &r, __ATOMIC_ACQUIRE); put64(r);
    v = 42;
    __atomic_exchange(&d64, &v, &r, __ATOMIC_SEQ_CST); put64(r); put64(d64);
    /* a failed compare leaves nothing on the stack: eight hundred in one
     * frame would be 6400 bytes, three times the ATmega328P's RAM */
    int fails = 0;
    for (int k = 0; k < 400; k++) {
        e = 1;
        fails += !__atomic_compare_exchange_n(&d64, &e, 2, 0, __ATOMIC_SEQ_CST,
                                              __ATOMIC_SEQ_CST);
        fails += !__sync_bool_compare_and_swap(&d64, 1, 2);
    }
    putn(fails); put64(d64);
    sd64 = -2;
    put64((unsigned long long)__atomic_fetch_add(&sd64, -3, __ATOMIC_SEQ_CST));
    put64((unsigned long long)sd64);
    writec('\n');
}

/* the operators on _Atomic objects, at every size, and pointers */
static _Atomic unsigned char ab = 250;
static _Atomic short ah = -2;
static _Atomic __UINT32_TYPE__ aw = 0xfffffffful;
static _Atomic long long ad = -1;
static int arr[4] = { 10, 20, 30, 40 };
static int *_Atomic ap = arr;
static int *pp = arr;

static void operators(void)
{
    putn(ab++); putn(ab += 10); putn(ab);
    putn(ah--); putn(ah -= 30000); putn(ah *= 2); putn(ah);
    put64(aw++); put64(aw); put64(aw |= 0x80000001ul);
    put64(aw ^= 3); put64(aw /= 7);
    put64((unsigned long long)ad++); put64((unsigned long long)ad);
    put64((unsigned long long)(ad += 0x123456789ll)); put64((unsigned long long)(ad <<= 3));
    put64((unsigned long long)(ad &= 0xfff0)); put64((unsigned long long)ad);
    ad = 0x7fffffffffffffffll;
    put64((unsigned long long)(ad - 1));
    putn(*ap++); putn(*ap); ++ap; putn(*ap--); putn(*ap); putn((long)(ap - arr));
    int *old = __atomic_exchange_n(&pp, &arr[3], __ATOMIC_SEQ_CST);
    putn(*old); putn(*__atomic_load_n(&pp, __ATOMIC_SEQ_CST));
    __atomic_store_n(&pp, &arr[2], __ATOMIC_SEQ_CST); putn(*pp);
    writec('\n');
}

/* atomic_flag, through the builtins <stdatomic.h> uses */
static struct { _Atomic unsigned char v; } flag;
static void flags(void)
{
    putn(__atomic_test_and_set(&flag.v, __ATOMIC_SEQ_CST));
    putn(__atomic_test_and_set(&flag.v, __ATOMIC_ACQUIRE));
    __atomic_clear(&flag.v, __ATOMIC_RELEASE);
    putn(__atomic_test_and_set(&flag.v, __ATOMIC_RELAXED));
    putn(flag.v);
    writec('\n');
}

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
static volatile unsigned long long shared64;
static volatile unsigned int isrs;
/* the torn-value phases: 1, the handler writes pat with equal halves; 2,
 * it reads main's and counts the ones whose halves differ */
static volatile unsigned char mode;
static volatile unsigned long long pat;
static volatile unsigned int torn_isr;
__attribute__((signal)) void __vector_11(void)   /* TIMER1_COMPA */
{
    shared = shared + 1;
    shared64 = shared64 + 1;
    isrs++;
    if (mode == 1) {
        unsigned long k = (unsigned long)isrs * 0x01010101ul;
        pat = ((unsigned long long)k << 32) | k;
    } else if (mode == 2) {
        unsigned long long v = pat;
        if ((unsigned long)(v >> 32) != (unsigned long)v)
            torn_isr++;
    }
}

static void race(void)
{
    OCR1AH = 0;
    OCR1AL = 200;
    TCCR1B = 0x09;              /* CTC on OCR1A, clk/1 */
    TIMSK1 = 0x02;              /* OCIE1A */
    /* each phase runs until enough interrupts have come: QEMU's timer
     * runs on the host's clock, not the guest's cycles */
    unsigned long n;
    unsigned int n0 = isrs;
    __asm__ volatile("sei" ::: "memory");
    for (n = 0; isrs - n0 < 200; n++) {
        __atomic_fetch_add(&shared, 3, __ATOMIC_SEQ_CST);
        __atomic_fetch_add((unsigned long long *)&shared64, 3, __ATOMIC_SEQ_CST);
    }
    __asm__ volatile("cli" ::: "memory");
    puts_(shared == n + n + n + isrs ? "race exact" : "race LOST");
    writec('\n');
    puts_(shared64 == (unsigned long long)(n + n + n) + isrs ? "race64 exact" : "race64 LOST");
    writec('\n');

    /* loads: never half of one value and half of the next */
    unsigned int torn = 0;
    n0 = isrs;
    mode = 1;
    __asm__ volatile("sei" ::: "memory");
    while (isrs - n0 < 200) {
        unsigned long long v = __atomic_load_n((unsigned long long *)&pat, __ATOMIC_SEQ_CST);
        if ((unsigned long)(v >> 32) != (unsigned long)v)
            torn++;
    }
    __asm__ volatile("cli" ::: "memory");
    puts_(torn == 0 ? "loads whole" : "loads TORN");
    writec('\n');

    /* stores: the handler never sees half of one */
    mode = 2;
    n0 = isrs;
    __asm__ volatile("sei" ::: "memory");
    for (n = 0; isrs - n0 < 200; n++) {
        unsigned long j = (n << 16) | (n & 0xffff);
        __atomic_store_n((unsigned long long *)&pat,
                         ((unsigned long long)~j << 32) | ~j, __ATOMIC_SEQ_CST);
    }
    __asm__ volatile("cli" ::: "memory");
    TIMSK1 = 0;
    puts_(torn_isr == 0 ? "stores whole" : "stores TORN");
    writec('\n');
}
#else
static void race(void)
{
    puts_("race exact\nrace64 exact\nloads whole\nstores whole\n");
}
#endif

int main(void)
{
    functional();
    eight();
    operators();
    flags();
    race();
    puts_("==END==\n");
    return 0;
}
