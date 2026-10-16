/* `x op= f()` with f changing x: the right operand is evaluated before
 * x's value is read, as GCC and clang do -- C leaves the read and the
 * call indeterminately sequenced, and EmbCC read first, so the bit f set
 * was lost (GCC's pr58943.c). A local, a global, an array element and a
 * bit-field each. */
// expect-exit: 42
static unsigned g = 2, arr[1] = { 2 };
static struct { unsigned f : 8; } s = { 2 };
static unsigned *pl;
static unsigned set_g(void) { g |= 128; return 1; }
static unsigned set_a(void) { arr[0] |= 128; return 1; }
static unsigned set_s(void) { s.f |= 128; return 1; }
static unsigned set_l(void) { *pl |= 128; return 1; }
int main(void)
{
    unsigned l = 2;
    pl = &l;
    g |= set_g();
    arr[0] |= set_a();
    s.f |= set_s();
    l |= set_l();
    return g == 131 && arr[0] == 131 && s.f == 131 && l == 131 ? 42 : 1;
}
