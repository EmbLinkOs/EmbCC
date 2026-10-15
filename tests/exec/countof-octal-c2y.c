// C2y's _Countof and 0o octal constants, which GCC accepts in every C
// mode. Of a variable-length array, _Countof reads the length the
// declaration computed: `n` changing afterwards does not change it.
// expect-exit: 42

int g[7];
static int sum[_Countof(g) == 7 ? 1 : -1];    /* a constant expression */

static unsigned long countof_vla(int n)
{
    int v[n];
    int m[3][n];
    int k = n - 1;
    int w[k][n];
    n = 100;
    v[0] = 0;
    m[0][0] = 0;
    w[0][0] = 0;
    return _Countof(v) * 1000000 + _Countof(m) * 10000 + _Countof(w) * 100 +
           _Countof(w[0]) + v[0] + m[0][0] + w[0][0];
}

int main(void)
{
    if (_Countof(g) != 7 || _Countof(int[9]) != 9 || _Countof sum != 1)
        return 1;
    if (_Countof(int[2][5]) != 2)
        return 2;
    if (sizeof(_Countof(g)) != sizeof(unsigned long) &&
        sizeof(_Countof(g)) != sizeof(unsigned))
        return 3;
    if (countof_vla(5) != 5030405)
        return 4;
    if (0o17 != 15 || 0O777u != 511u || 0o0 != 0 || 0o17L != 15L)
        return 5;
    return 42;
}
