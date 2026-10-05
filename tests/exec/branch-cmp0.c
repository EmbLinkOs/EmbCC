/* A branch on a truth value compared with zero, where the truth value is
 * merged from two arms -- C's `(x ? 1 : 0) == 0`, FreeRTOS's
 * `listLIST_IS_EMPTY(l) == pdFALSE`. The optimizer branches on the value
 * itself (inverting for `== 0`) and then threads each arm to its target;
 * every polarity and width is checked here against the plain reading. */
// expect-exit: 42
#define EMPTY(n) ((n) == 0 ? 1 : 0)

__attribute__((noinline)) static int ne0(int n) { return EMPTY(n) != 0 ? 7 : 9; }
__attribute__((noinline)) static int eq0(int n) { return EMPTY(n) == 0 ? 7 : 9; }
__attribute__((noinline)) static int both(int a, int b)
{
    int r = 0;
    if ((a > 3 && b < 2) == 0)       /* && leaves a merged 0/1 */
        r += 1;
    if ((a > 3 || b < 2) != 0)
        r += 2;
    return r;
}
__attribute__((noinline)) static int narrow(unsigned char c, short s)
{
    unsigned char t = c ? 1 : 0;      /* a byte-wide truth value */
    short u = s < 0 ? 1 : 0;
    return (t == 0) * 10 + (u != 0);
}
__attribute__((noinline)) static int nonbool(int x)
{
    int v = x > 5 ? 2 : 0;           /* not 0/1: != 0 is still true */
    return v != 0 ? 1 : 0;
}

int main(void)
{
    if (ne0(0) != 7 || ne0(5) != 9) return 1;
    if (eq0(0) != 9 || eq0(5) != 7) return 2;
    if (both(5, 1) != 2 || both(5, 3) != 3 || both(1, 1) != 3 ||
        both(1, 3) != 1) return 3;
    if (narrow(0, -1) != 11 || narrow(3, 4) != 0 || narrow(0, 2) != 10)
        return 4;
    if (nonbool(9) != 1 || nonbool(1) != 0) return 5;
    return 42;
}
