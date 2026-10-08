// expect-exit: 42
/* An if whose condition is a constant generates only the arm it selects
 * (unless a label is in the other) -- so the condition's value
 * must be folded exactly as the program would compute it: unsigned and
 * 64-bit comparisons, the usual conversions, shifts and divisions. Each
 * `if` below takes the arm its constant selects, or the function says
 * which one went wrong. */
static int bad;
#define T(c) do { if (c) {} else bad = __LINE__; } while (0)
#define F(c) do { if (c) bad = __LINE__; } while (0)

static int jump_in(int k)
{
    int r = 0;
    if (k)
        goto inside;
    if (0) {
inside:
        r = 7;                       /* reached by the goto */
    }
    return r;
}

int main(void)
{
    T(0xffffffffffffffffUL > 0);
    T(-1 < 0);
    F(-1 < 0u);                      /* -1 becomes UINT_MAX */
    F(-1L < 0UL);
    T((unsigned char)-1 == 255);
    T(0x8000000000000000UL >> 63 == 1);
    T(-8 >> 1 == -4);
    T(0xffffffffffffffffUL / 3 == 0x5555555555555555UL);
    F(0xffffffffu + 1u);             /* wraps to 0 */
    T(0xffffffffu + 1UL);            /* does not */
    T((long long)-1 < 1);
    F((unsigned long long)-1 < 1);
    T(sizeof(long long) == 8 && 2 > 1);
    F(1.5 < 1);
    T(0.5 > 0);
    T((int)1.9 == 1);
    if (jump_in(1) != 7 || jump_in(0) != 0)
        return 1;
    return bad ? 100 + bad % 100 : 42;
}
