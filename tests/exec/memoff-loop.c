/* An address computed from a pointer the loop walks, used AFTER the walk
 * has moved the pointer on. The offset fold turns `q = p + 4; ... *q`
 * into a load at [p, #4], which is only right while p still holds the
 * value q was made from: here it does not, and the fold must see the
 * write in between and leave the add alone. Every case keeps the old
 * address live across the pointer's advance. */
// expect-exit: 42
__attribute__((noinline)) long after(const int *p, int n)
{
    long s = 0;
    for (int i = 0; i < n; i++) {
        const int *q = p + 1;           /* q = p + 4 bytes */
        p += 3;                         /* the base moves first... */
        s += *q * (i + 1);              /* ...and this must read the OLD p + 4 */
    }
    return s;
}
__attribute__((noinline)) long before_and_after(int *p, int n)
{
    long s = 0;
    for (int i = 0; i < n; i++) {
        int *q = p + 2;
        s += q[0];                      /* a use before the advance: foldable */
        p += 1;
        q[0] += 1;                      /* and one after it: not */
        s += q[0];
    }
    return s;
}
/* An access whose address is a register-register sum PLUS a constant.
 * The fold moves the constant into the access; a backend that then
 * fuses the sum into the access's register-offset form (`ldrb [x0,
 * x13]`) has nowhere to put the constant, and must not fuse. The
 * offsets here are the ones the vla test lost: row[1][1] read row[1][0]. */
__attribute__((noinline)) long rr_mul(const char *p, long i, long j) { const char *q = p + i * j; return q[4]; }
__attribute__((noinline)) long rr_xor(const char *p, long i) { const char *q = p + (i ^ 5); return q[4] + q[9]; }
__attribute__((noinline)) long rr_row(const int *a, long m) { int (*row)[m] = (int (*)[m])a; row += 2; row++; row--; return row[1][1]; }
__attribute__((noinline)) void rr_st(char *p, long i, long j, char v) { char *q = p + i * j; q[4] = v; q[7] = v + 1; }
int main(void)
{
    {
        char c[64]; int t[40];
        for (int i = 0; i < 64; i++) c[i] = (char)(i + 1);
        for (int i = 0; i < 40; i++) t[i] = i * 100;
        if (rr_mul(c, 3, 5) != c[19]) return 3;
        if (rr_xor(c, 6) != c[7] + c[12]) return 4;    /* 6 ^ 5 = 3 */
        if (rr_row(t, 5) != t[3 * 5 + 1]) return 5;      /* row 3, column 1 */
        rr_st(c, 4, 4, 100);
        if (c[20] != 100 || c[23] != 101 || c[19] != 20 || c[21] != 22) return 6;
    }
    int a[64];
    for (int i = 0; i < 64; i++) a[i] = i * 3 + 1;
    /* after: q reads a[1], a[4], a[7], ... weighted 1, 2, 3, ... */
    long want = 0;
    for (int i = 0; i < 10; i++) want += (long)(3 * (3 * i + 1) + 1) * (i + 1);
    if (after(a, 10) != want) return 1;
    long w2 = 0;
    {
        int b[64];
        for (int i = 0; i < 64; i++) b[i] = i * 3 + 1;
        for (int i = 0; i < 12; i++) { w2 += b[i + 2]; b[i + 2] += 1; w2 += b[i + 2]; }
    }
    if (before_and_after(a, 12) != w2) return 2;
    return 42;
}
