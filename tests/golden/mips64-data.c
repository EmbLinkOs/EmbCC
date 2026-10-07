/* Static data on MIPS64 (n64), byte for byte against clang, in both
 * orders (tests/golden/mips64-data.sh): tests/golden/be-data.c's
 * initialisers -- whose bytes depend on the byte order -- at LP64, and
 * what only a 64-bit target has: __int128 objects and arrays, binary128
 * long doubles, a packed struct whose __int128 bit-field spans a 17-byte
 * unit, pointers as eight-byte R_MIPS_64 fields with addends, and
 * longs. */
#define main be_main
#include "be-data.c"
#undef main

__int128 q_s = -((__int128)0x0123456789abcdefL << 40) - 5;
unsigned __int128 q_u[3] = { 1, (unsigned __int128)1 << 127,
                             ((unsigned __int128)0xfedcba9876543210UL << 64) | 7 };
long double q_ld[3] = { 1.5L, -0x1p-16000L, 3.141592653589793238462643383279L };
struct q_pad { char c; __int128 v; short s; } q_pad = { 1, -2, 3 };
struct __attribute__((packed)) q_bf {
    unsigned char a : 3;
    unsigned __int128 b : 125;     /* bits 3..127, then a 17-byte unit */
    unsigned __int128 c : 7;
} q_bf = { 5, ((unsigned __int128)0x1234 << 100) | 0xabcdef, 0x55 };
long q_long[2] = { -0x7766554433221100L, 0x0102030405060708L };
long *q_lp[3] = { &q_long[0], &q_long[1], (long *)0 };
char *q_cp = (char *)&q_long[1] + 3;
struct q_ps { int k; const char *s; long *p; } q_ps = { 9, "sixty-four", q_long + 1 };

extern void writec(int c); extern void puts_(const char *s); extern void putn(long v);
static void putq(unsigned __int128 v) { putn((long)(v >> 64)); putn((long)v); }
static void putmem(const void *p, unsigned n)
{
    const unsigned char *b = p;
    for (unsigned k = 0; k < n; k++)
        putn(b[k]);
    puts_("\n");
}
int main(void)
{
    be_main();
    putq((unsigned __int128)q_s); putq(q_u[0]); putq(q_u[1]); putq(q_u[2]);
    putmem(q_ld, sizeof q_ld); putmem(&q_pad, sizeof q_pad);
    putn(q_pad.c); putq((unsigned __int128)q_pad.v); putn(q_pad.s);
    putmem(&q_bf, sizeof q_bf);
    putn(q_bf.a); putq(q_bf.b); putn(q_bf.c);
    putn(q_long[0]); putn(q_long[1]); putn(*q_lp[0]); putn(*q_lp[1]);
    putn(q_lp[2] == 0); putn(*q_cp); putn(q_ps.k); puts_(q_ps.s);
    putn(*q_ps.p);
    puts_("\n");
    return 0;
}
