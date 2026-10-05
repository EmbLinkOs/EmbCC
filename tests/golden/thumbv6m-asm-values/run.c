/* ARMv6-M inline asm whose operands are values (ir_asm_op.val), checked
 * against the same computation in C. The templates are Thumb-1; beside
 * what thumb-asm-values/run.c covers on ARMv7-M: operands in r12, which
 * only MOV and ADD reach, so ARMv6-M moves them through a low register;
 * a rotation of three inputs; an output that is not a value (a float),
 * stored through its address. */
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
          "movs %1, #0\n"            /* MOVS leaves C alone */
          "adcs %1, %1\n"
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
 * taken before the asm; a "+" output; and a float, which is not a value
 * output and is stored through its address after the template */
unsigned g_out;
unsigned arr[4];
__attribute__((noinline)) static void to_mem(unsigned v, int i)
{
    __asm("adds %0, %1, #1" : "=r"(arr[i]) : "r"(v));
    __asm("adds %0, %1, #2" : "=r"(g_out) : "r"(v));
}
__attribute__((noinline)) static unsigned plus5(unsigned x)
{
    __asm("adds %0, #5" : "+r"(x));
    return x;
}
__attribute__((noinline)) static void plus7_mem(unsigned *p)
{
    __asm("adds %0, #7" : "+r"(*p));
}
__attribute__((noinline)) static unsigned fbits(unsigned u)
{
    union { float f; unsigned u; } c;
    float f;
    __asm("mov %0, %1" : "=r"(f) : "r"(u));
    c.f = f;
    return c.u;
}

/* values live across an asm that writes r0-r3 and r12: they must be in
 * r4/r5 or memory */
__attribute__((noinline)) static unsigned across(unsigned a, unsigned b,
                                                 unsigned n)
{
    unsigned s = 0;
    for (unsigned i = 0; i < n; i++) {
        unsigned t = a * i + b;
        __asm volatile("movs r0, #11\n movs r1, #12\n movs r2, #13\n"
                       "movs r3, #14\n mov r12, r3"
                       ::: "r0", "r1", "r2", "r3", "r12", "cc");
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

/* ...and across one that writes r0 alone, which may keep them in r1-r3 */
__attribute__((noinline)) static unsigned across_r0(unsigned a, unsigned n)
{
    unsigned s = 0;
    for (unsigned i = 0; i < n; i++) {
        unsigned r;
        __asm volatile("movs %0, #3" : "=r"(r) :: "cc");
        s += a * i + r;
    }
    return s;
}

/* inputs whose homes are each other's operand registers: a swap and a
 * three-way rotation, which one move at a time gets wrong */
__attribute__((noinline))
static void swapstore(unsigned a, unsigned b, unsigned *p)
{
    __asm volatile("str %0, [%2]\n str %1, [%2, #4]"
                   :: "r"(b), "r"(a), "r"(p) : "memory");
}
__attribute__((noinline))
static void rotstore(unsigned a, unsigned b, unsigned c, unsigned *p)
{
    __asm volatile("str %0, [%3]\n str %1, [%3, #4]\n str %2, [%3, #8]"
                   :: "r"(b), "r"(c), "r"(a), "r"(p) : "memory");
}

/* a swap among the inputs while r12 is an operand too: the cycle is
 * broken through a register that is not one of the asm's */
__attribute__((noinline))
static void swap12(unsigned a, unsigned b, unsigned c, unsigned d,
                   unsigned *p)
{
    __asm volatile("str %0, [%3]\n str %1, [%3, #4]\n str %2, [%3, #8]\n"
                   "mov %2, %4\n str %2, [%3, #12]"
                   :: "r"(b), "r"(a), "r"(c), "r"(p), "r"(d) : "memory");
}

/* an asm function inlined into its caller AND called through a pointer */
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
    __asm volatile("movs r1, #91\n movs r2, #92\n movs r3, #93\n"
                   "mov r12, r3" ::: "r1", "r2", "r3", "r12", "cc");
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

/* ...and a function that is nothing but such an asm: lr is spent, so it
 * is not a leaf, and returns through the lr it saved */
static unsigned g_calls;
__attribute__((noinline)) static void count(void)
{
    g_calls++;
}
__attribute__((noinline)) static void call_it(void (*f)(void))
{
    __asm volatile("blx %0" :: "r"(f) : "memory");
}

/* a "+" output stored through its address after a template that clobbers
 * r12, with values live across it */
unsigned g_acc = 40;
__attribute__((noinline)) static unsigned scratch_out(unsigned a,
                                                      unsigned b,
                                                      unsigned keep)
{
    unsigned *volatile pv = &g_acc;
    unsigned *p = pv;
    unsigned t = keep * 7, u = keep * 11, v = keep * 13, w = keep * 17;
    __asm volatile("mov r12, %1\n adds %0, %0, %1\n adds %0, %0, %2"
                   : "+r"(*p) : "r"(a), "r"(b) : "r12", "cc");
    return t + u + v + w + keep;
}

/* r12: an input, a value output and a "+" output there, which ARMv6-M
 * moves through a low register */
__attribute__((noinline)) static unsigned hi_in(unsigned x)
{
    register unsigned k __asm("r12") = x * 3;
    unsigned r;
    __asm("mov %0, %1" : "=r"(r) : "r"(k));
    return r + 1;
}
__attribute__((noinline)) static unsigned hi_out(unsigned x)
{
    register unsigned r __asm("r12");
    __asm("mov %0, %1" : "=r"(r) : "r"(x));
    return r * 5;
}
__attribute__((noinline)) static unsigned hi_inout(unsigned x, unsigned y)
{
    register unsigned z __asm("r12") = x;
    __asm("mov r3, %0\n adds r3, r3, %1\n mov %0, r3"
          : "+r"(z) : "r"(y) : "r3", "cc");
    return z;
}
/* five operands: r0-r3, and r12 for the last; the second output is a
 * continuation */
__attribute__((noinline)) static unsigned five(unsigned a, unsigned b,
                                               unsigned c, unsigned d)
{
    unsigned r, t;
    __asm("adds %0, %2, %3\n mov %1, %4\n adds %0, %0, %1"
          : "=&r"(r), "=&r"(t) : "r"(a), "r"(b), "r"(d) : "cc");
    return r + c + (t - d);
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
    check("fbits", fbits(0x40490fdbu), 0x40490fdbu);

    check("across", across(3, 5, 50), across_c(3, 5, 50));
    check("across_r0", across_r0(7, 20), 7 * 190 + 3 * 20);

    unsigned st[3];
    swapstore(11, 22, st);
    check("swapstore", st[0] * 100 + st[1], 2211);
    rotstore(1, 2, 3, st);
    check("rotstore", st[0] * 100 + st[1] * 10 + st[2], 231);

    unsigned st4[4];
    swap12(1, 2, 3, 4, st4);
    check("swap12", st4[0] * 1000 + st4[1] * 100 + st4[2] * 10 + st4[3],
          2134);

    unsigned (*volatile rp)(unsigned) = rev_pub;
    check("rev_pub inline", rev_pub(0x11223344u), 0x44332211u);
    check("rev_pub pointer", rp(0x55667788u), 0x88776655u);

    check("callthrough", callthrough(6, 7), 6 * 5 + 7 + 6 + 7);
    call_it(count);
    call_it(count);
    check("call_it", g_calls, 2);

    check("scratch_out", scratch_out(1, 1, 5), 5 * (7 + 11 + 13 + 17 + 1));
    check("scratch_out g", g_acc, 42);

    check("hi_in", hi_in(14), 43);
    check("hi_out", hi_out(9), 45);
    check("hi_inout", hi_inout(30, 12), 42);
    check("five", five(1, 2, 3, 36), 42);

    puts_(fails ? "FAILED\n" : "all ok\n");
    puts_("==END==\n");
    return 0;
}
