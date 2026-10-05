/* Inline asm whose operands are values: each result is checked against
 * the same computation in C. */
void puts_(const char *s);
void putn(long v);

#define INLINE static inline __attribute__((always_inline))

INLINE unsigned long shl3(unsigned long x)
{
    unsigned long r;
    __asm("slli %0, %1, 3" : "=r"(r) : "r"(x));
    return r;
}

/* two value outputs, the second a continuation */
INLINE unsigned long add_carry(unsigned long a, unsigned long b,
                               unsigned long *carry)
{
    unsigned long s, c;
    __asm("add %0, %2, %3\n sltu %1, %0, %2"
          : "=&r"(s), "=&r"(c) : "r"(a), "r"(b));
    *carry = c;
    return s;
}

/* two outputs from two inputs, crossed */
__attribute__((noinline))
static void swap2(unsigned long *a, unsigned long *b)
{
    unsigned long x = *a, y = *b, p, q;
    __asm("mv %0, %3\n mv %1, %2" : "=&r"(p), "=&r"(q) : "r"(x), "r"(y));
    *a = p; *b = q;
}

/* narrow outputs take the type's value, extended as C extends it */
__attribute__((noinline)) static signed char sbyte(long x)
{
    signed char r;
    __asm("mv %0, %1" : "=r"(r) : "r"(x));
    return r;
}
__attribute__((noinline)) static unsigned char ubyte(long x)
{
    unsigned char r;
    __asm("mv %0, %1" : "=r"(r) : "r"(x));
    return r;
}
__attribute__((noinline)) static short shalf(long x)
{
    short r;
    __asm("mv %0, %1" : "=r"(r) : "r"(x));
    return r;
}
/* an int whose register has something else above bit 31 (RV64) */
__attribute__((noinline)) static int lowint(void)
{
    int r;
#if __riscv_xlen == 64
    __asm("li %0, -1\n slli %0, %0, 32\n ori %0, %0, 5" : "=r"(r));
#else
    __asm("li %0, 5" : "=r"(r));
#endif
    return r;
}

/* outputs to memory, and "+" outputs */
unsigned long g_out;
unsigned long arr[4];
__attribute__((noinline)) static void to_mem(unsigned long v, int i)
{
    __asm("addi %0, %1, 1" : "=r"(arr[i]) : "r"(v));
    __asm("addi %0, %1, 2" : "=r"(g_out) : "r"(v));
}
__attribute__((noinline)) static unsigned long plus5(unsigned long x)
{
    __asm("addi %0, %0, 5" : "+r"(x));
    return x;
}
__attribute__((noinline)) static void plus7_mem(unsigned long *p)
{
    __asm("addi %0, %0, 7" : "+r"(*p));
}

/* values live across an asm that writes every caller-saved register it
 * may: they are in saved registers or memory */
__attribute__((noinline)) static unsigned long across(unsigned long a,
                                                      unsigned long b,
                                                      unsigned long n)
{
    unsigned long s = 0;
    for (unsigned long i = 0; i < n; i++) {
        unsigned long t = a * i + b;
        __asm volatile("li t0, 11\n li t1, 12\n li t2, 13\n li t3, 14\n"
                       "li t4, 15\n li t5, 16\n li t6, 17\n li a0, 18\n"
                       "li a1, 19\n li a2, 20\n li a3, 21\n li a4, 22\n"
                       "li a5, 23\n li a6, 24\n li a7, 25"
                       ::: "t0", "t1", "t2", "t3", "t4", "t5", "t6", "a0",
                       "a1", "a2", "a3", "a4", "a5", "a6", "a7");
        s += t ^ i;
    }
    return s;
}
static unsigned long across_c(unsigned long a, unsigned long b,
                              unsigned long n)
{
    unsigned long s = 0;
    for (unsigned long i = 0; i < n; i++)
        s += (a * i + b) ^ i;
    return s;
}

/* inputs whose homes are each other's operand registers: a swap */
__attribute__((noinline))
static void swapstore(unsigned a, unsigned b, unsigned *p)
{
    __asm volatile("sw %0, 0(%2)\n sw %1, 4(%2)"
                   :: "r"(b), "r"(a), "r"(p) : "memory");
}

/* the same with the operands in a0 and a1 swapped against where the
 * arguments arrive -- the asm's registers are t0-t6 first, so it takes
 * register variables to make the moves a cycle */
__attribute__((noinline))
static void swapstore2(unsigned a, unsigned b, unsigned *p)
{
    register unsigned rb __asm__("a0") = b;
    register unsigned ra_ __asm__("a1") = a;
    __asm volatile("sw %0, 0(%2)\n sw %1, 4(%2)"
                   :: "r"(rb), "r"(ra_), "r"(p) : "memory");
}

/* an asm function inlined into its caller AND called through a pointer */
unsigned long shl3_pub(unsigned long x)
{
    unsigned long r;
    __asm("slli %0, %1, 3" : "=r"(r) : "r"(x));
    return r;
}

/* a template that calls: every caller-saved register changes, whatever
 * the clobber list says */
__attribute__((noinline)) static unsigned long scramble(void)
{
    volatile unsigned long k = 0x5a5a5a5a;
    /* what any called function may do to the caller-saved registers */
    __asm volatile("li t0, 91\n li t1, 92\n li t2, 93\n li t3, 94\n"
                   "li a1, 95\n li a2, 96\n li a3, 97\n li a4, 98\n"
                   "li a5, 99\n li a6, 100\n li a7, 101"
                   ::: "t0", "t1", "t2", "t3", "a1", "a2", "a3", "a4", "a5",
                   "a6", "a7");
    return k * 3u + 1u;
}
__attribute__((noinline)) static unsigned long callthrough(unsigned long a,
                                                           unsigned long b)
{
    unsigned long (*volatile fp)(void) = scramble;
    unsigned long (*f)(void) = fp;
    unsigned long s = a * 5 + b;
    __asm volatile("jalr %0" :: "r"(f) : "memory");
    return s + a + b;
}

/* an output stored through its address uses a scratch register (t3 here:
 * t0-t2 are operands), which a value live across the asm must not be in;
 * a0-a7 are clobbered, so t3 is the first register such a value could
 * otherwise take */
unsigned long g_acc = 40;
__attribute__((noinline)) static unsigned long scratch_out(unsigned long a,
                                                           unsigned long b,
                                                           unsigned long keep)
{
    unsigned long *volatile pv = &g_acc;
    unsigned long *p = pv;
    unsigned long t = keep * 7, u = keep * 11, v = keep * 13, w = keep * 17;
    __asm volatile("add %0, %0, %1\n add %0, %0, %2"
                   : "+r"(*p) : "r"(a), "r"(b)
                   : "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7");
    return t + u + v + w + keep;
}

static int fails;
static void check(const char *what, long got, long want)
{
    if (got != want) {
        puts_("BAD ");
        puts_(what);
        puts_(" ");
        putn(got);
        putn(want);
        puts_("\n");
        fails++;
    }
}

int main(void)
{
    unsigned long h = 0;
    for (unsigned long k = 1; k < 200; k += 7)
        h += shl3(k * 0x01020304u) - (k * 0x01020304u << 3);
    check("shl3", (long)h, 0);

    unsigned long c, s = add_carry(~0UL - 15, 0x20, &c);
    check("add_carry sum", (long)s, 0x10);
    check("add_carry carry", (long)c, 1);
    s = add_carry(5, 6, &c);
    check("add_carry 2", (long)(s * 10 + c), 110);

    unsigned long x = 1, y = 2;
    swap2(&x, &y);
    check("swap2", (long)(x * 10 + y), 21);

    check("sbyte", sbyte(0x1ff), -1);
    check("ubyte", ubyte(0x1ff), 255);
    check("shalf", shalf(0x18000), -32768);
    int li = lowint();
    check("lowint", li, 5);
    check("lowint eq", li == 5, 1);
    check("lowint long", (long)li, 5);

    to_mem(40, 2);
    check("to_mem arr", (long)arr[2], 41);
    check("to_mem g", (long)g_out, 42);
    check("plus5", (long)plus5(37), 42);
    unsigned long m = 35;
    plus7_mem(&m);
    check("plus7_mem", (long)m, 42);

    check("across", (long)across(3, 5, 50), (long)across_c(3, 5, 50));

    unsigned st[2];
    swapstore(11, 22, st);
    check("swapstore", st[0] * 100 + st[1], 2211);
    swapstore2(33, 44, st);
    check("swapstore2", st[0] * 100 + st[1], 4433);

    unsigned long (*volatile rp)(unsigned long) = shl3_pub;
    check("shl3_pub inline", (long)shl3_pub(5), 40);
    check("shl3_pub pointer", (long)rp(6), 48);

    check("callthrough", (long)callthrough(6, 7), 6 * 5 + 7 + 6 + 7);

    check("scratch_out", (long)scratch_out(1, 1, 5), 5 * (7 + 11 + 13 + 17 + 1));
    check("scratch_out g", (long)g_acc, 42);

    puts_(fails ? "FAILED\n" : "all ok\n");
    puts_("==END==\n");
    return 0;
}
