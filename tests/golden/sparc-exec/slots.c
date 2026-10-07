// expect-exit: 42
// SPARC's delay slots and the code shapes built around them, on the
// board: each function below is one way an instruction could be moved
// wrongly -- into a slot whose transfer reads it, across a label, around a
// call's %o7 -- or one of the folds that share those rules (the test's
// cc form, the annulled if/else arm, the leaf function, the long double
// handed over in its own slot). tests/golden/sparc-slots.sh checks the
// same objects' slots by their shape; this checks what they compute.
// Every value is ILP32/big-endian-neutral: the expected sums were taken
// from the same source on another target.
#include <stdint.h>

#define NOINLINE __attribute__((noinline))

volatile int vsink;

/* A value computed before a loop's label and used after it: the loop's
 * branch has nothing of its own to put in its slot, and the add before
 * the label must not go there (it would run on every trip). */
NOINLINE int before_label(int n, int t)
{
    t = t * 5 + 3;
    do {
        n = n - 1;
    } while (n > 0);
    return t + n;
}

NOINLINE int before_label2(unsigned n, unsigned t)
{
    t ^= 0x55;
    while (n > 7)
        n = n >> 1;
    return (int)(t + n);
}

/* A compare's codes and an instruction reading them (the carry) before a
 * branch on the compare. */
NOINLINE int carries(unsigned a, unsigned b, unsigned c)
{
    int r = 0;
    if (a < b)
        r += 1;
    if (b < c)
        r += 2;
    r += (a < c);
    if (a == c)
        r += 8;
    return r;
}

NOINLINE int64_t wide(int64_t a, int64_t b)
{
    int64_t s = a + b;
    if (s < a)
        s -= 3;
    if (a < b)
        return s * 2;
    return s - b;
}

/* Indirect calls: the target is the last thing computed before the
 * jmpl, which reads it; the slot must not be the target's own load. */
static int add1(int x) { return x + 1; }
static int mul3(int x) { return x * 3; }
static int neg(int x) { return -x; }
static int (*const table[3])(int) = { add1, mul3, neg };

NOINLINE int indirect(int k, int x)
{
    int (*f)(int) = table[k % 3];
    int y = f(x);
    f = table[(k + 1) % 3];
    return y + f(y);
}

/* A frame past simm13: at -O0 every argument comes from a slot that only
 * %o7 can reach (sethi; or; ld [%fp + %o7]), right before the call that
 * writes %o7. */
NOINLINE int sum4(int a, int b, int c, int d) { return a + 2 * b + 3 * c + 4 * d; }

NOINLINE int bigframe(int x)
{
    volatile char pad[6000];
    int a = x + 1, b = x + 2, c = x + 3, d = x + 4;
    pad[0] = (char)x;
    pad[5999] = (char)(x + 1);
    return sum4(a, b, c, d) + pad[0] + pad[5999];
}

/* if/else arms of one instruction: the annulled inverse branch. */
NOINLINE int arms(int a, int b)
{
    int r;
    if (a > b)
        r = a - b;
    else
        r = b + 7;
    if (a & 4)
        r ^= 0x100;
    else
        r |= 0x20;
    return r;
}

/* Tests the cc form of an and/add/or answers. */
NOINLINE int ccform(unsigned x, unsigned y)
{
    int r = 0;
    if ((x & 6) == 0) r += 1;
    if (x + y) r += 2;
    if ((x | y) != 0) r += 4;
    if ((x ^ y) == 0) r += 8;
    if (x - y) r += 16;
    return r;
}

/* Leaf functions: no window, the arguments in the outs. */
NOINLINE unsigned leaf1(unsigned a, unsigned b, unsigned c)
{
    return (a << 3) + (b ^ c) - (a & c);
}

NOINLINE int64_t leaf64(int64_t a, int32_t b)
{
    return a * 3 + b;
}

NOINLINE int leafloop(const unsigned char *s)
{
    int n = 0;
    while (*s++)
        n++;
    return n;
}

/* A switch's table (a call .+8 that writes %o7 in its own slot). */
NOINLINE int sw(int k)
{
    switch (k) {
    case 0: return 11;
    case 1: return 13;
    case 2: return 17;
    case 3: return 19;
    case 4: return 23;
    case 5: return 29;
    default: return 31;
    }
}

/* Structs returned through the caller's buffer (the unimp after the
 * call's slot), and calls whose slot takes the last argument's move. */
struct S3 { int a, b, c; };

NOINLINE struct S3 mk(int x)
{
    struct S3 s;
    s.a = x;
    s.b = x * 2;
    s.c = x * 3;
    return s;
}

NOINLINE int usemk(int x)
{
    struct S3 s = mk(x);
    struct S3 t = mk(s.c);
    return s.a + s.b + t.c;
}

/* long doubles: a temp that dies at its operation is handed to the
 * helper in its own slot; one that lives on, or both operands the same,
 * is copied. */
NOINLINE int ld(int x)
{
    long double a = x, b = x + 2;
    long double s = a * a + b;          /* a * a: the same operand twice */
    long double t = s * b - a;          /* b and a live on */
    long double u = (s + t) / b;
    return (int)(u * 4) + (int)a + (int)b;
}

int main(void)
{
    int bad = 0;
    if (before_label(9, 4) != 23) bad |= 1;
    if (before_label2(1000, 3) != 0x56 + 7) bad |= 2;
    if (carries(1, 2, 3) != 4 || carries(3, 2, 1) != 0 ||
        carries(5, 9, 5) != 9) bad |= 4;
    if (wide(5, 7) != 24 || wide(9, 2) != 9 || wide(-5, 3) != -4 ||
        wide(7, -9) != 4)
        bad |= 8;
    if (indirect(0, 5) != 24 || indirect(1, 5) != 0 || indirect(2, 5) != -9)
        bad |= 16;
    if (bigframe(10) != 11 + 24 + 39 + 56 + 10 + 11) bad |= 32;
    if (arms(9, 4) != (5 | 0x20) || arms(4, 9) != (16 ^ 0x100) ||
        arms(12, 1) != (11 ^ 0x100)) bad |= 64;
    if (ccform(0, 0) != 1 + 8 || ccform(6, 1) != 2 + 4 + 16 ||
        ccform(9, 9) != 1 + 2 + 4 + 8) bad |= 128;
    if (leaf1(3, 5, 6) != 24 + 3 - 2) bad |= 256;
    if (leaf64(10000000000LL, -5) != 29999999995LL) bad |= 512;
    if (leafloop((const unsigned char *)"delay") != 5) bad |= 1024;
    if (sw(0) + sw(3) + sw(5) + sw(9) + sw(-1) != 11 + 19 + 29 + 31 + 31)
        bad |= 2048;
    if (usemk(4) != 4 + 8 + 36) bad |= 4096;
    if (ld(3) != 64 + 3 + 5) bad |= 8192;
    vsink = bad;
    return bad ? 1 : 42;
}
