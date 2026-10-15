/* GNU C: `x ?: y` is x when x is true, else y, with x evaluated once
 * (a call is not repeated, a volatile not read twice), and `struct E { };`
 * is complete at size 0. */
// expect-exit: 42
struct E { };
static int calls;
static int next(void) { return ++calls; }
static int zero(void) { calls++; return 0; }
int g = 0 ?: 7;
int main(void)
{
    int a = 5, z = 0;
    volatile int v = 3;
    struct E e1, e2;
    int r = (a ?: 9) + (z ?: 9) * 10 + (v ?: 1) * 100;       /* 5 + 90 + 300 */
    int s = next() ?: 100;                                    /* 1, once */
    int t = zero() ?: 40;                                     /* 40 */
    char buf[4] = "x", *p = buf ?: 0;
    if (r != 395 || s != 1 || t != 40 || calls != 2 || g != 7 || *p != 'x')
        return 1;
    if (sizeof(struct E) != 0 || &e1 == (void *)0 || &e2 == (void *)0)
        return 2;
    return 42;
}
