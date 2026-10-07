/* The 32-bit divides and the one-, two- and four-byte atomics: what
 * ARMv8-M Baseline lowers to SDIV/UDIV and to LDREX/STREX loops where
 * ARMv6-M calls the runtime (src/arch/thumb/v6m.c).
 *
 * As with embedded-int64.c, the expected answers are not written down: the
 * same source is compiled for the host and run, and its output is the
 * reference. Every operand comes through a volatile, so nothing is folded
 * and every operation is the instruction under test.
 */
extern void writec(int c);
extern void puts_(const char *s);

static void hx(unsigned v)
{
    for (int i = 28; i >= 0; i -= 4)
        writec("0123456789abcdef"[(v >> i) & 0xfu]);
    writec(' ');
}
static void nl(void) { writec('\n'); }

static volatile int si[] = { 0, 1, -1, 7, -7, 1000000, -1000000, 0x7fffffff,
                             -0x7fffffff - 1, 13, -13, 65536 };
static volatile unsigned ui[] = { 1, 3, 7, 0x80000000u, 0xffffffffu,
                                  0xdeadbeefu, 1000, 0x10000u };

static int sdiv_(int a, int b) { return a / b; }
static int smod_(int a, int b) { return a % b; }
static unsigned udiv_(unsigned a, unsigned b) { return a / b; }
static unsigned umod_(unsigned a, unsigned b) { return a % b; }

static volatile int gi;
static volatile short gs;
static volatile unsigned short gus;
static volatile signed char gc;
static volatile unsigned char guc;

int main(void)
{
    int n = (int)(sizeof si / sizeof si[0]);
    int m = (int)(sizeof ui / sizeof ui[0]);
    /* every signed pair but a zero divisor and INT_MIN / -1 */
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            int a = si[i], b = si[j];
            if (b == 0 || (a == -0x7fffffff - 1 && b == -1))
                continue;
            hx((unsigned)sdiv_(a, b));
            hx((unsigned)smod_(a, b));
        }
        nl();
    }
    for (int i = 0; i < m; i++) {
        for (int j = 0; j < m; j++) {
            hx(udiv_(ui[i], ui[j]));
            hx(umod_(ui[i], ui[j]));
        }
        nl();
    }
    /* by constants, which the optimizer may turn into multiplies */
    for (int i = 0; i < n; i++) {
        int a = si[i];
        hx((unsigned)(a / 10)); hx((unsigned)(a % 10));
        hx((unsigned)(a / -3)); hx((unsigned)(a % 1000));
        hx((unsigned)a / 6u); hx((unsigned)a % 1024u);
    }
    nl();

    /* the read-modify-writes at each width, signed and not */
    gi = 5;
    hx((unsigned)__atomic_fetch_add(&gi, 37, __ATOMIC_SEQ_CST)); hx((unsigned)gi);
    hx((unsigned)__atomic_fetch_sub(&gi, 50, __ATOMIC_SEQ_CST)); hx((unsigned)gi);
    hx((unsigned)__atomic_fetch_and(&gi, 0x0ff0, __ATOMIC_SEQ_CST)); hx((unsigned)gi);
    hx((unsigned)__atomic_fetch_or(&gi, 0x10001, __ATOMIC_SEQ_CST)); hx((unsigned)gi);
    hx((unsigned)__atomic_fetch_xor(&gi, -1, __ATOMIC_SEQ_CST)); hx((unsigned)gi);
    hx((unsigned)__atomic_fetch_nand(&gi, 0x3c3c, __ATOMIC_SEQ_CST)); hx((unsigned)gi);
    hx((unsigned)__atomic_exchange_n(&gi, -9, __ATOMIC_SEQ_CST)); hx((unsigned)gi);
    hx((unsigned)__atomic_add_fetch(&gi, 4, __ATOMIC_SEQ_CST));
    nl();
    gs = -2;
    hx((unsigned)__atomic_fetch_add(&gs, -32767, __ATOMIC_SEQ_CST)); hx((unsigned)gs);
    hx((unsigned)__atomic_exchange_n(&gs, 0x1234, __ATOMIC_SEQ_CST)); hx((unsigned)gs);
    hx((unsigned)__atomic_fetch_or(&gs, (short)0x8000, __ATOMIC_SEQ_CST)); hx((unsigned)gs);
    gus = 0xfffe;
    hx(__atomic_fetch_add(&gus, 3, __ATOMIC_SEQ_CST)); hx(gus);
    hx(__atomic_fetch_nand(&gus, 0xff, __ATOMIC_SEQ_CST)); hx(gus);
    nl();
    gc = 120;
    hx((unsigned)__atomic_fetch_add(&gc, 10, __ATOMIC_SEQ_CST)); hx((unsigned)gc);
    hx((unsigned)__atomic_fetch_sub(&gc, 1, __ATOMIC_SEQ_CST)); hx((unsigned)gc);
    guc = 250;
    hx(__atomic_fetch_add(&guc, 10, __ATOMIC_SEQ_CST)); hx(guc);
    hx(__atomic_fetch_xor(&guc, 0xaa, __ATOMIC_SEQ_CST)); hx(guc);
    hx(__atomic_exchange_n(&guc, 0x81, __ATOMIC_SEQ_CST)); hx(guc);
    nl();

    /* compare-and-swap: a hit and a miss at each width, by value and
     * with the expected value written back */
    {
        int e = -9 + 4;
        gi = e;
        hx((unsigned)__atomic_compare_exchange_n(&gi, &e, 77, 0,
                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
        hx((unsigned)e); hx((unsigned)gi);
        e = 5;
        hx((unsigned)__atomic_compare_exchange_n(&gi, &e, 99, 0,
                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
        hx((unsigned)e); hx((unsigned)gi);
    }
    {
        short e = 0x1234;
        gs = (short)0x9234;
        hx((unsigned)__atomic_compare_exchange_n(&gs, &e, -5, 0,
                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
        hx((unsigned)e); hx((unsigned)gs);
        hx((unsigned)__atomic_compare_exchange_n(&gs, &e, -5, 0,
                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
        hx((unsigned)e); hx((unsigned)gs);
    }
    {
        unsigned char e = 0x81;
        guc = 0x81;
        hx((unsigned)__atomic_compare_exchange_n(&guc, &e, 0xfe, 0,
                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
        hx(e); hx(guc);
    }
    gc = -3;
    hx((unsigned)__sync_val_compare_and_swap(&gc, (signed char)-3, (signed char)100));
    hx((unsigned)gc);
    hx((unsigned)__sync_val_compare_and_swap(&gc, (signed char)-3, (signed char)7));
    hx((unsigned)gc);
    gi = 1000;
    hx((unsigned)__sync_val_compare_and_swap(&gi, 1000, -1000));
    hx((unsigned)__sync_bool_compare_and_swap(&gi, 1000, 3));
    hx((unsigned)gi);
    nl();
    puts_("==END==\n");
    return 0;
}
