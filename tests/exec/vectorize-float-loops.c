// expect-exit: 42
/* The loop vectorizer's lane operations are INTEGER ones, and it did not
 * look at whether an operation was floating-point: `a[i] = b[i] + c[i]`
 * over float or double arrays was added as integers (paddd) at -O2 on
 * x86-64 -- wrong answers, no diagnostic. A float sum was already
 * refused; element-wise float arithmetic now is too. Run at -O2 by
 * tests/golden/regalloc-O2.sh and at -Os by exec-Os.sh. */
#define N 64
float a[N], b[N], c[N];
double da[N], db[N], dc[N];
__attribute__((noinline)) void addf(void) { for (int i = 0; i < N; i++) a[i] = b[i] + c[i]; }
__attribute__((noinline)) void addd(void) { for (int i = 0; i < N; i++) da[i] = db[i] + dc[i]; }
__attribute__((noinline)) void mulf(void) { for (int i = 0; i < N; i++) a[i] = b[i] * 2.0f; }
int main(void)
{
    for (int i = 0; i < N; i++) { b[i] = i * 0.5f; c[i] = 1.25f; db[i] = i * 0.25; dc[i] = 2.5; }
    addf(); addd();
    for (int i = 0; i < N; i++) if (a[i] != i * 0.5f + 1.25f || da[i] != i * 0.25 + 2.5) return 1;
    mulf();
    for (int i = 0; i < N; i++) if (a[i] != i * 1.0f) return 2;
    return 42;
}
