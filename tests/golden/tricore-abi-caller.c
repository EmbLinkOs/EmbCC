#include "tricore-abi.h"
extern void writec(int c); extern void puts_(const char *s); extern void putn(long v);
static void put64(long long v) { putn((long)(v / 1000000)); putn((long)(v % 1000000)); }
int main(void)
{
    char s[] = "abcdefgh";
    static const short tab[] = { 10, 20, 30, 40 };
    int k = 7, a1 = 3, b1 = 9;
    put64(fill(3, 123456789LL, 4));
    putn(ptrs(s, 2, s + 1, s + 2, 5, s + 3, s + 4));
    putn(*ptr_ret(s, 5)); putn(*ptr_ret2(2, tab));
    putn((long)(fd(1.5f, &k, 10.0, 3) * 4));
    writec('\n');
    {
        struct t20 t = { { 1, 2, 3, 4, 5 } };
        putn(big(t, 100)); putn(t.v[0] + t.v[4]);
    }
    { struct t3 x = { 1, 2, 3 }, y = r3(x); putn(y.a); putn(y.b); putn(y.c); }
    { struct t8 x = { 11, 22 }, y = r8(3, x); putn(y.a); putn(y.b); }
    { struct t20 y = r20(4, s); putn(y.v[0]); putn(y.v[4]); }
    put64(many(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 5000000000LL));
    writec('\n');
    {
        struct t20 t = { { 9, 0, 0, 0, 90 } };
        put64(vmixed(5, s, &k, 2.25, 4000000000LL, t, 77));
        put64(vmixed(2, s + 1, &b1, -1.5));
    }
    putn(apply(cmp_int, &a1, &b1)); putn(apply(cmp_int, &b1, &a1));
    puts_("\n==END==\n");
    return 0;
}
