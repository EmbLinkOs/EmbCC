// C23's _BitInt(N): a value is its low N bits, whatever it is stored in.
// Arithmetic in the type wraps at N bits, a _BitInt is not promoted to
// int, conversions keep the low N bits, and the usual arithmetic
// conversions rank one by its width. Only defined behaviour: unsigned
// wrap-around, and conversions to signed (modulo 2^N in GCC and here).
// Printed, so tests/golden/agrees-with-gcc.sh holds it to GCC's.
// expect-exit: 42
#include <stdio.h>

typedef _BitInt(12) s12;
typedef unsigned _BitInt(12) u12;
typedef _BitInt(7) s7;
typedef unsigned _BitInt(3) u3;
typedef _BitInt(33) s33;
typedef unsigned _BitInt(40) u40;
typedef unsigned _BitInt(64) u64b;

static unsigned long sum;

static void p(const char *what, long long v)
{
    printf("%s %lld\n", what, v);
    sum = sum * 31 + (unsigned long)v;
}

struct rec { u12 a; s7 b; u40 c; };

/* folded at compile time to the same N bits */
_Static_assert((u12)4095 + (u12)1 == 0, "u12 wraps");
_Static_assert((s12)4095 == -1, "s12 keeps 12 bits");
_Static_assert((u12)-1 == 4095, "u12 of -1");
static u12 g_init = (u12)5000;    /* 904 */
static s12 g_neg = (s12)4095;     /* -1 */
static u12 g_arr[(u12)4097];      /* one element */

static int sel(u12 x)
{
    switch (x) {
    case (u12)4097: return 1;     /* case 1 */
    default: return 0;
    }
}

static u12 twice(u12 x) { return x + x; }          /* wraps at 12 bits */
static s33 neg33(s33 x) { return -x; }

int main(void)
{
    u12 b = 4095;
    b++;
    p("u12 4095+1", b);
    b--;
    p("u12 0-1", b);
    u12 d = 0;
    d = ~d;
    p("~(u12)0", d);
    p("u12 2000*3", (u12)2000 * (u12)3);
    p("twice 3000", twice(3000));

    p("(s12)4095", (s12)4095);
    p("(s12)0x1234", (s12)0x1234);
    p("(u12)-1", (u12)-1);
    p("(s7)200", (s7)200);
    p("(u3)13", (u3)13);
    p("(s12)(u12)2048", (s12)(u12)2048);

    u12 s = 1;
    s <<= 11;
    p("1<<11", s);
    s <<= 1;
    p("2048<<1", s);
    p("u12 4095>>3", (u12)4095 >> 3);
    p("s12 -2048>>3", (s12)-2048 >> 3);
    p("s12 -7/2", (s12)-7 / (s12)2);
    p("s12 -7%2", (s12)-7 % (s12)2);

    /* the usual arithmetic conversions: by rank, which is width */
    p("u12 5 > s12 -1", (u12)5 > (s12)-1);       /* -1 becomes 4095 */
    p("s12 -1 < int 0", (s12)-1 < 0);            /* int is wider: -1 */
    p("sizeof (s12)+1", sizeof((s12)0 + 1));     /* int */
    p("sizeof s12+s12", sizeof((s12)0 + (s12)0)); /* s12 itself */
    p("generic s12+s12", _Generic((s12)0 + (s12)0, s12: 1, int: 2, default: 3));
    p("generic s12+int", _Generic((s12)0 + 0, s12: 1, int: 2, default: 3));
    p("generic u40+int", _Generic((u40)0 + 0, u40: 1, default: 3));
    p("generic s33+unsigned", _Generic((s33)0 + 0u, s33: 1, default: 3));
    p("generic s12 promoted", _Generic(+(s12)0, s12: 1, int: 2, default: 3));

    /* constants with the wb suffix: the fewest bits that hold them */
    p("generic 5wb", _Generic(5wb, _BitInt(4): 1, default: 0));
    p("generic 5uwb", _Generic(5uwb, unsigned _BitInt(3): 1, default: 0));
    p("generic 0wb", _Generic(0wb, _BitInt(2): 1, default: 0));
    p("generic 0uwb", _Generic(0uwb, unsigned _BitInt(1): 1, default: 0));
    p("sizes", sizeof(u3) * 1000 + sizeof(s12) * 100 + sizeof(s33) * 10 +
                   sizeof(u64b));

    /* wider than int */
    u40 w = 0xFFFFFFFFFFuwb;
    w += 2;
    p("u40 max+2", (long long)w);
    p("neg33", neg33((s33)-4294967296 + 1));
    u64b z = 0;
    z -= 1;
    p("u64b 0-1 == ~0ull", z == ~0ull);

    /* stored in memory, through pointers and members */
    struct rec r = { 4000, -60, 0xFFFFFFFFFFuwb };
    r.a += 200;
    r.c++;
    u12 *pa = &r.a;
    (*pa)++;
    p("member a", r.a);
    p("member b", r.b);
    p("member c", (long long)r.c);
    u12 arr[3] = { 4095, 4094, 4093 };
    for (int i = 0; i < 3; i++)
        arr[i] += 3;
    p("array", arr[0] * 100 + arr[1] * 10 + arr[2]);

    /* a switch on one */
    u3 k = 7;
    k += 2;                           /* 1 */
    switch (k) {
    case 1: p("switch", 1); break;
    default: p("switch", 0); break;
    }

    p("static init", g_init);
    p("static neg", g_neg);
    p("array of (u12)4097", sizeof g_arr / sizeof g_arr[0]);
    p("case (u12)4097", sel(1));

    printf("sum %lu\n", sum);
    return 42;
}
