/* C23 constexpr: a named integer constant, usable in constant
 * expressions, at file scope (static or not) and in a block, several to
 * a declaration. It took its value with an enumerator's type, int, so a
 * `constexpr long long` past INT_MAX was truncated -- silently -- and
 * its sizeof was 4; and at file scope it was "expected a type". */
// expect-exit: 42
// no-gcc-reference: C23 constexpr; the reference builds strict C11
constexpr int N = 40;
static constexpr long long BIG = 5000000000LL, SMALL = -7;
int arr[N];
constexpr unsigned char UC = 200;
int main(void)
{
    constexpr long long big = 5000000000LL;
    constexpr unsigned char c = 200;
    constexpr _Bool t = 1;
    long long x = big / 1000000000LL + BIG / 1000000000LL;     /* 10 */
    int bad = 0;
    if (x != 10 || sizeof(big) != 8 || sizeof(BIG) != 8 || SMALL != -7) bad |= 1;
    if (c != 200 || UC + 1 != 201 || sizeof(UC) != 1 || !t) bad |= 2;
    if (sizeof arr / sizeof arr[0] != 40) bad |= 4;
    _Static_assert(N == 40 && BIG > 4000000000LL, "constant expressions");
    return bad ? bad : N + 2;
}
