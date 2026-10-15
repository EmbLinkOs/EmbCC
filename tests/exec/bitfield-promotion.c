/* A bit-field is promoted by its width (C11 6.3.1.1p2; GCC's rule for
 * the wider integer types): `unsigned u3:3` to int, so `x.u3 - 2` is
 * negative, as GCC and clang have it -- EmbCC kept the declared unsigned
 * int and the test was never true. A field as wide as unsigned int stays
 * unsigned, and one wider than int keeps its declared type (GCC's
 * bf-sign-2.c). */
// expect-exit: 42
#include <stdio.h>

struct X {
    unsigned int u3 : 3;
    signed long s31 : 31;
    unsigned long u31 : 31;
    unsigned u15 : 15;
    unsigned long long ull3 : 3;
    unsigned long long ull35 : 35;
} x;

int main(void)
{
    int ok = 0;
    ok += (x.u3 - 2) < 0;
    ok += (x.s31 - 2) < 0;
    ok += (x.u15 - 2) < 0;
    ok += (x.ull3 - 2) < 0;
    ok += (x.ull35 - 2) > 0;          /* unsigned long long */
    ok += -x.u3 == 0 && ~x.u3 == -1;  /* unary: int */
    ok += (x.u31 - 2) < 0 || sizeof(int) < 4;
    printf("%d\n", ok);
    return ok == 7 ? 42 : ok;
}
