/* Inline asm whose operands are values: each result is checked against
 * the same computation in C. */
void puts_(const char *s);
void putn(long v);

#define INLINE static inline __attribute__((always_inline))

INLINE unsigned rev(unsigned x)
{
    unsigned r;
    __asm("rev %0, %1" : "=r"(r) : "r"(x));
    return r;
}
static unsigned rev_c(unsigned x)
{
    return x >> 24 | (x >> 8 & 0xff00) | (x << 8 & 0xff0000) | x << 24;
}

/* two value outputs, the second a continuation */
INLINE unsigned add_carry(unsigned a, unsigned b, unsigned *carry)
{
    unsigned s, c;
    __asm("adds %0, %2, %3\n"
          "mov %1, #0\n"
          "adc %1, %1, #0\n"
          : "=&r"(s), "=&r"(c) : "r"(a), "r"(b) : "cc");
    *carry = c;
    return s;
}

/* two outputs from two inputs, crossed: the moves in and out are
 * parallel, or one of them reads a register the other already wrote */
__attribute__((noinline))
static void swap2(unsigned *a, unsigned *b)
{
    unsigned x = *a, y = *b, p, q;
    __asm("mov %0, %3\n mov %1, %2" : "=&r"(p), "=&r"(q) : "r"(x), "r"(y));
    *a = p; *b = q;
}

/* narrow outputs take the type's value, extended as C extends it */
__attribute__((noinline)) static signed char sbyte(int x)
{
    signed char r;
    __asm("mov %0, %1" : "=r"(r) : "r"(x));
    return r;
}
__attribute__((noinline)) static unsigned char ubyte(int x)
{
    unsigned char r;
    __asm("mov %0, %1" : "=r"(r) : "r"(x));
    return r;
}
__attribute__((noinline)) static short shalf(int x)
{
    short r;
    __asm("mov %0, %1" : "=r"(r) : "r"(x));
    return r;
}

/* outputs to memory: a global and an array element, whose address is
 * taken before the asm; and a "+" output */
unsigned g_out;
unsigned arr[4];
__attribute__((noinline)) static void to_mem(unsigned v, int i)
{
    __asm("add %0, %1, #1" : "=r"(arr[i]) : "r"(v));
    __asm("add %0, %1, #2" : "=r"(g_out) : "r"(v));
}
__attribute__((noinline)) static unsigned plus5(unsigned x)
{
    __asm("add %0, %0, #5" : "+r"(x));
    return x;
}
__attribute__((noinline)) static void plus7_mem(unsigned *p)
{
    __asm("add %0, %0, #7" : "+r"(*p));
}

/* values live across an asm that writes r0-r3 and r12: they must be in
 * callee-saved registers or memory */
__attribute__((noinline)) static unsigned across(unsigned a, unsigned b,
                                                 unsigned n)
{
    unsigned s = 0;
    for (unsigned i = 0; i < n; i++) {
        unsigned t = a * i + b;
        __asm volatile("mov r0, #11\n mov r1, #12\n mov r2, #13\n"
                       "mov r3, #14\n mov r12, #15"
                       ::: "r0", "r1", "r2", "r3", "r12");
        s += t ^ i;
    }
    return s;
}
static unsigned across_c(unsigned a, unsigned b, unsigned n)
{
    unsigned s = 0;
    for (unsigned i = 0; i < n; i++)
        s += (a * i + b) ^ i;
    return s;
}

/* inputs whose homes are each other's operand registers: a in r0 and b
 * in r1 arrive, and the asm wants b in r0 and a in r1 -- a swap, which
 * one move at a time gets wrong */
__attribute__((noinline))
static void swapstore(unsigned a, unsigned b, unsigned *p)
{
    __asm volatile("str %0, [%2]\n str %1, [%2, #4]"
                   :: "r"(b), "r"(a), "r"(p) : "memory");
}

/* an asm function inlined into its caller AND called through a pointer:
 * the inlined copy renames its operands, and the original keeps its own */
unsigned rev_pub(unsigned x)
{
    unsigned r;
    __asm("rev %0, %1" : "=r"(r) : "r"(x));
    return r;
}

/* a template that calls: r0-r3, r12 and lr change whatever the clobber
 * list says, so values live across it are elsewhere */
__attribute__((noinline)) static unsigned scramble(void)
{
    volatile unsigned k = 0x5a5a5a5a;
    /* what any called function may do to the caller-saved registers */
    __asm volatile("mov r1, #91\n mov r2, #92\n mov r3, #93\n mov r12, #94"
                   ::: "r1", "r2", "r3", "r12");
    return k * 3u + 1u;
}
__attribute__((noinline)) static unsigned callthrough(unsigned a,
                                                      unsigned b)
{
    unsigned (*volatile fp)(void) = scramble;
    unsigned (*f)(void) = fp;
    unsigned s = a * 5 + b;
    __asm volatile("blx %0" :: "r"(f) : "memory");
    return s + a + b;
}

/* an output stored through its address uses a scratch register (here r3:
 * r12 is clobbered and r0-r2 are operands), which a value live across the
 * asm must not be in */
unsigned g_acc = 40;
__attribute__((noinline)) static unsigned scratch_out(unsigned a,
                                                      unsigned b,
                                                      unsigned keep)
{
    unsigned *volatile pv = &g_acc;
    unsigned *p = pv;
    unsigned t = keep * 7, u = keep * 11, v = keep * 13, w = keep * 17;
    __asm volatile("mov r12, #0\n add %0, %0, %1\n add %0, %0, %2"
                   : "+r"(*p) : "r"(a), "r"(b) : "r12");
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
    unsigned h = 0;
    for (unsigned k = 1; k < 200; k += 7)
        h += rev(k * 0x01020304u) - rev_c(k * 0x01020304u);
    check("rev", h, 0);

    unsigned c, s = add_carry(0xfffffff0u, 0x20u, &c);
    check("add_carry sum", s, 0x10);
    check("add_carry carry", c, 1);
    s = add_carry(5, 6, &c);
    check("add_carry 2", s * 10 + c, 110);

    unsigned x = 1, y = 2;
    swap2(&x, &y);
    check("swap2", x * 10 + y, 21);

    check("sbyte", sbyte(0x1ff), -1);
    check("ubyte", ubyte(0x1ff), 255);
    check("shalf", shalf(0x18000), -32768);

    to_mem(40, 2);
    check("to_mem arr", arr[2], 41);
    check("to_mem g", g_out, 42);
    check("plus5", plus5(37), 42);
    unsigned m = 35;
    plus7_mem(&m);
    check("plus7_mem", m, 42);

    check("across", across(3, 5, 50), across_c(3, 5, 50));

    unsigned st[2];
    swapstore(11, 22, st);
    check("swapstore", st[0] * 100 + st[1], 2211);

    unsigned (*volatile rp)(unsigned) = rev_pub;
    check("rev_pub inline", rev_pub(0x11223344u), 0x44332211u);
    check("rev_pub pointer", rp(0x55667788u), 0x88776655u);

    check("callthrough", callthrough(6, 7), 6 * 5 + 7 + 6 + 7);

    check("scratch_out", scratch_out(1, 1, 5), 5 * (7 + 11 + 13 + 17 + 1));
    check("scratch_out g", g_acc, 42);

    puts_(fails ? "FAILED\n" : "all ok\n");
    puts_("==END==\n");
    return 0;
}
