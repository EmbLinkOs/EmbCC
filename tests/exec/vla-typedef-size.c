// expect-exit: 42
/* A typedef of a variably modified type fixes its size where the typedef
 * is reached (C11 6.7.8p3): changing the length afterwards changes
 * nothing. The size was evaluated wherever the NAME was used, so after
 * `typedef int row[n]; n += 7;` a `row r;` had ten elements and
 * sizeof(row) was 40 where C says 12. A typedef built on another keeps
 * the inner one's size, and one inside a loop is re-sized each time. */
__attribute__((noinline)) static int f(int n, int m)
{
    typedef int row[n];
    n += 7;
    typedef row mat[m];          /* row stays n-as-it-was elements */
    m += 1;
    mat a;
    typedef int (*rp)[n];        /* a pointer to the CURRENT n (10 here) */
    rp p = 0;
    int t = 0;
    for (int k = 1; k <= 3; k++) {
        typedef char buf[k];     /* re-sized each time it is reached */
        t += (int)sizeof(buf);
    }
    return (int)sizeof(row) * 1000 + (int)sizeof a * 10 + (int)sizeof *p + t * 100000;
}
int main(void) { int r = f(3, 2); return r == (12*1000 + 24*10 + 40 + 6*100000) ? 42 : 1; }
