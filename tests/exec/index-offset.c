/* Constant offsets in array indices: a[i - 1], a[i + 2], p[i - 6]
 * through a pointer into the middle of an array, elements of two, four
 * and eight bytes, `long` indices, and one element written through one
 * offset and read back through another. On RISC-V the offset becomes
 * the access's displacement and the accesses share one base + (i << k),
 * so a wrong displacement, scale or sign shows up here as a wrong sum.
 * Every index is opaque to the compiler (vz is a volatile 0). */
// expect-exit: 42
static volatile int vz;

static int a[16];
static short h[16];
static long long q[8];
static int m[8];

static int sum3(const int *p, int i) { return p[i - 1] + p[i - 2] * 3 + p[i + 1]; }
static int mid(const int *p, int i)  { return p[i - 6] - p[i + 2]; }
static int hsum(int i)               { return h[i - 1] + h[i + 3] - h[i - 2]; }
static long long qd(int i)           { return q[i + 1] - q[i - 1]; }
static int lidx(const int *p, long i) { return p[i - 3] + p[i + 4]; }

/* an insertion sort's step: p[j] = p[j - 1] for j = n down to 1 */
static void shift_up(int *p, int n)
{
    for (int j = n; j > 0; j--)
        p[j] = p[j - 1];
}

/* written at i + 1, read at i - 1 and i: three offsets of one index */
static int rw(int *p, int i)
{
    p[i + 1] = p[i - 1] * 10;
    return p[i + 1] + p[i];
}

int main(void)
{
    for (int k = 0; k < 16; k++) {
        a[k] = 3 * k + 1 + vz;
        h[k] = (short)(100 - 7 * k + vz);
    }
    for (int k = 0; k < 8; k++) {
        q[k] = (long long)k * 1000000007LL + vz;
        m[k] = k + vz;
    }
    int i = 5 + vz;
    if (sum3(a, i) != 13 + 3 * 10 + 19)         /* a[4] + 3*a[3] + a[6] */
        return 1;
    if (mid(&a[8], i - 2) != 16 - 40)          /* a[5] - a[13] */
        return 2;
    if (hsum(i - 1) != 79 + 51 - 86)           /* h[3] + h[7] - h[2] */
        return 3;
    if (qd(i - 2) != 2000000014LL)             /* q[4] - q[2] */
        return 4;
    if (lidx(a, (long)(6 + vz)) != 10 + 31)    /* a[3] + a[10] */
        return 5;
    shift_up(m, 5 + vz);                       /* 0 0 1 2 3 4 6 7 */
    if (m[0] != 0 || m[1] != 0 || m[2] != 1 || m[5] != 4 || m[6] != 6)
        return 6;
    if (rw(m, 3 + vz) != 10 + 2)               /* m[4] = m[2] * 10, + m[3] */
        return 7;
    return 42;
}
