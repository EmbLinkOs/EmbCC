/* A local whose address is taken stays in memory, so writing it in a
 * loop is an `stvar` to its frame slot on every iteration, and reading
 * it through the pointer is a load. The unroller renamed every
 * definition in its copies -- the slot an stvar writes included -- so
 * three of every four stores went to slots that do not exist and the
 * reads through the pointer saw the value from before the loop: `sum`
 * returned the last element alone. libcxx's std::accumulate is this
 * shape (`init = op(std::move(init), *first)` takes init's address).
 * Every trip count from 0 past several runs of copies, through a plain
 * pointer and through a pointer to const. */
// expect-exit: 42
static volatile int vbias = 0;

__attribute__((noinline)) static int via(const int *p) { return *p; }

__attribute__((noinline)) int sum(const int *a, int n)
{
    int s = 0;
    const int *ps = &s;
    for (int i = 0; i < n; i++)
        s = *ps + a[i];
    return s + via(ps) * 0;
}

__attribute__((noinline)) long long dot(const int *a, const int *b, int n)
{
    long long acc = 0;
    long long *pa = &acc;
    for (int i = 0; i < n; i++)
        *pa = acc + (long long)a[i] * b[i];
    return acc;
}

int main(void)
{
    int a[64], b[64];
    for (int i = 0; i < 64; i++) { a[i] = i + 1 + vbias; b[i] = 2 * i - 7; }
    for (int n = 0; n <= 64; n++) {
        long long want = 0;
        for (int i = 0; i < n; i++) want += (long long)a[i] * b[i];
        if (sum(a, n) != n * (n + 1) / 2) return 1;
        if (dot(a, b, n) != want) return 2;
    }
    return 42;
}
