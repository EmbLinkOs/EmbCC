/* A nested loop whose INNER counter does not start at zero. Induction
 * variable strength reduction rewrites `a[i]` as a walked pointer; it
 * found the inner loop's counter from the OUTER loop, since both of the
 * counter's definitions lay inside it and its "starts at zero, outside
 * the loop" check had nothing to look at -- and walked the pointer once
 * per outer iteration. The inner loop then read a[j] over and over.
 *
 * The references are closed forms, not loops of the same shape: a loop
 * shaped like the one under test is miscompiled the same way and agrees. */
// expect-exit: 42
__attribute__((noinline)) long from_k(const int *a, int n, int k)
{
    long s = 0;
    for (int j = 0; j < n; j++)
        for (int i = k; i < 16; i++)
            s += a[i];
    return s;
}
__attribute__((noinline)) long from_j(const int *a, int n)
{
    long s = 0;
    for (int j = 0; j < n; j++)          /* triangular: i starts at j */
        for (int i = j; i < n; i++)
            s += a[i] * (j + 1);
    return s;
}
__attribute__((noinline)) long from_one_store(int *a, int n)
{
    long s = 0;
    for (int j = 0; j < n; j++) {
        for (int i = 1; i < 8; i++)
            a[i] = a[i - 1] + a[i];
        s += a[7];
    }
    return s;
}
int main(void)
{
    int a[16];
    for (int i = 0; i < 16; i++) a[i] = i + 1;            /* a[i] = i + 1 */
    /* sum_{i=k}^{15} (i+1) = 136 - k(k+1)/2, times n */
    if (from_k(a, 5, 3) != 5L * (136 - 3 * 4 / 2)) return 1;
    if (from_k(a, 2, 0) != 2L * 136) return 2;
    /* sum_j (j+1) * sum_{i=j}^{7} (i+1), n = 8: sum_{i=j}^{7}(i+1) = 36 - j(j+1)/2 */
    long w = 0;
    for (int j = 0; j < 8; j++) w += (long)(j + 1) * (36 - j * (j + 1) / 2);
    if (from_j(a, 8) != w) return 3;
    /* prefix sums applied twice to 1..8: a[7] after one pass is 36, after
     * two it is sum of the prefix sums = 1+3+6+10+15+21+28+36 = 120 */
    if (from_one_store(a, 2) != 36 + 120) return 4;
    return 42;
}
