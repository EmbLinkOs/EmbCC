/* C99's array parameters: `[static N]` (the argument points at N
 * elements at least), qualifiers in the brackets (`[const]` is a
 * `*const` parameter), and `register` on a parameter. All three stopped
 * the compile -- "expected an expression, got 'static'" and "'register'
 * is not supported yet" -- and all three are in code written against
 * C99 and C89: `void sha256(uint8_t out[static 32], ...)`, and register
 * parameters in every old embedded library.
 */
// expect-exit: 42
static int sum(const int a[static 4])
{
    return a[0] + a[1] + a[2] + a[3];
}

static int first(int a[const restrict 2], int n)
{
    return a[0] + n + (int)(sizeof a == sizeof(int *));    /* a pointer */
}

static unsigned char low(register unsigned v, register unsigned char *out)
{
    *out = (unsigned char)(v & 0xff);
    return *out;
}

static int rows(int n, int m[static 2][3])
{
    return m[1][2] + n;
}

int main(void)
{
    int v[4] = { 1, 2, 3, 4 };              /* 10 */
    int m[2][3] = { { 0, 0, 0 }, { 0, 0, 5 } };
    unsigned char c;
    int r = sum(v) + first(v, 6);           /* 10 + 1 + 6 + 1 = 18 */
    r += low(0x10b, &c) + c - 22;           /* 11 + 11 - 22 = 0 */
    r += rows(19, m);                       /* 24 */
    return r;
}
