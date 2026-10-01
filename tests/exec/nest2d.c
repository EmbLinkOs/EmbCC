/* Loop nests over 2-D arrays, where the inner loop's addresses are built
 * from the OUTER loop's counter: a row (m[i][k], base + i*R + k*E) and a
 * column (m[k][j], base + k*R + j*E). The outer counter is written twice
 * -- at its loop's entry and its latch -- and loop-invariant code motion
 * now hoists what is built from it when neither write is inside the
 * inner loop; strength reduction then walks a pointer down a column as
 * well as along a row. Beside each shape sits the one that must NOT be
 * moved: a "row" whose counter the inner loop itself writes, and an
 * inner counter whose zero is shared with an accumulator that has
 * already been written. Every expectation is a closed form. */
// expect-exit: 42
#define NI __attribute__((noinline))
#define R 12
static short ms[R][R];
static int mi[R][R];
static long long ml[R][R];

NI long rows(int n)                     /* sum over i<n, k<R of ms[i][k] */
{
    long s = 0;
    for (int i = 0; i < n; i++)
        for (int k = 0; k < R; k++)
            s += ms[i][k];
    return s;
}
NI long cols(int j0, int n)             /* sum over j0<=j<n, k<R of mi[k][j] * (k + 1) */
{
    long s = 0;
    for (int j = j0; j < n; j++)
        for (int k = 0; k < R; k++)
            s += (long)mi[k][j] * (k + 1);
    return s;
}
NI long long matmul_trace(void)         /* trace of ms x ms, via mi */
{
    for (int i = 0; i < R; i++)
        for (int j = 0; j < R; j++) {
            int s = 0;
            for (int k = 0; k < R; k++)
                s += ms[i][k] * ms[k][j];
            mi[i][j] = s;
        }
    long long t = 0;
    for (int i = 0; i < R; i++) t += mi[i][i];
    return t;
}
NI long long wide(int n)                /* 8-byte elements, column walk */
{
    long long s = 0;
    for (int j = 0; j < n; j++)
        for (int k = 0; k < R; k++)
            s += ml[k][j];
    return s;
}
NI long moving_row(int n)               /* the inner loop moves i: nothing may be hoisted */
{
    long s = 0;
    for (int i = 0; i < n; ) {
        for (int k = 0; k < R; k++) {
            s += ms[i][k];
            if (k == 5 && i + 1 < n) i++;  /* the row changes mid-walk */
        }
        i++;
    }
    return s;
}
NI long shared_zero(int n)              /* s and k start from one zero; s moves first */
{
    long s = 0, t = 0;
    for (int r = 0; r < n; r++) {
        int k = 0;
        s = 0;
        for (; k < R; k++) { s += ms[r][k]; t += k; }
        t += s;
    }
    return t;
}
NI long late_start(int n, int k0)       /* the inner counter does not start at zero */
{
    long s = 0;
    for (int i = 0; i < n; i++)
        for (int k = k0; k < R; k++)
            s += ms[i][k];
    return s;
}

int main(void)
{
    /* ms[i][k] = i*R + k - 50, mi[k][j] = 3k - j, ml[k][j] = k*1000000000 + j */
    for (int i = 0; i < R; i++)
        for (int k = 0; k < R; k++) {
            ms[i][k] = (short)(i * R + k - 50);
            mi[i][k] = 3 * i - k;
            ml[i][k] = (long long)i * 1000000000LL + k;
        }
    /* rows(n) = sum_{i<n} sum_{k<R} (iR + k - 50) = R*R*n(n-1)/2 + n*R(R-1)/2 - 50nR */
    for (int n = 0; n <= R; n++)
        if (rows(n) != (long)R * R * n * (n - 1) / 2 + (long)n * R * (R - 1) / 2 - 50L * n * R)
            return 1;
    /* cols: sum_k (3k - j)(k+1) = 3 sum k(k+1) - j sum (k+1) = 3*S2 - j*S1,
     * S2 = sum_{k<R} k(k+1) = (R-1)R(R+1)/3, S1 = R(R+1)/2 */
    {
        long S2 = (long)(R - 1) * R * (R + 1) / 3, S1 = (long)R * (R + 1) / 2;
        for (int j0 = 0; j0 < 4; j0++)
            for (int n = j0; n <= R; n++) {
                long want = 0;
                for (long j = j0; j < n; j++) want += 3 * S2 - j * S1;   /* sum over j only */
                if (cols(j0, n) != want) return 2;
            }
    }
    /* trace = sum_i sum_k ms[i][k]*ms[k][i], computed the plain way into a constant */
    {
        long long want = 0;
        for (int i = 0; i < R; i++)
            for (int k = 0; k < R; k++)
                want += (long long)(i * R + k - 50) * (k * R + i - 50);
        if (matmul_trace() != want) return 3;
    }
    /* wide(n) = sum_{j<n} sum_k (k*1e9 + j) = n*1e9*R(R-1)/2 + R*n(n-1)/2 */
    for (int n = 0; n <= R; n++)
        if (wide(n) != (long long)n * 1000000000LL * R * (R - 1) / 2 + (long long)R * n * (n - 1) / 2)
            return 4;
    /* moving_row: row i for k=0..5, row i+1 for k=6..11 (while i+1 < n), then i += 2 */
    {
        for (int n = 1; n <= 7; n++) {
            long want = 0;
            for (int i = 0; i < n; ) {
                int row = i;
                for (int k = 0; k < R; k++) {
                    want += row * R + k - 50;
                    if (k == 5 && row + 1 < n) row++;
                }
                i = row + 1;
            }
            if (moving_row(n) != want) return 5;
        }
    }
    /* shared_zero(n) = sum_r (R(R-1)/2 + rowsum(r)), rowsum(r) = R*R*r + R(R-1)/2 - 50R */
    for (int n = 0; n <= R; n++) {
        long want = 0;
        for (long r = 0; r < n; r++) want += (long)R * (R - 1) / 2 + R * R * r + (long)R * (R - 1) / 2 - 50L * R;
        if (shared_zero(n) != want) return 6;
    }
    /* late_start(n, k0) = sum_{i<n} sum_{k0<=k<R} (iR + k - 50) */
    for (int k0 = 0; k0 <= R; k0 += 3)
        for (int n = 0; n <= 5; n++) {
            long m = R - k0, want = (long)R * m * n * (n - 1) / 2 + n * (m * k0 + m * (m - 1) / 2) - 50L * n * m;
            if (late_start(n, k0) != want) return 7;
        }
    return 42;
}
