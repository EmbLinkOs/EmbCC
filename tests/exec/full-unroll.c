/* Loops with a constant trip count, which -O2 copies whole: no test, no
 * branch, no loop left. Each one is checked against a twin whose bound
 * the compiler cannot know (vz is a volatile 0), so the twin stays a
 * loop. A copy too many or too few, a counter left at the wrong value
 * after the loop, or a pointer walk that starts in the wrong place all
 * give a different answer. The trip counts straddle the limits: 2, 8,
 * 24, 32 and 33. */
// expect-exit: 42
static volatile int vz;

static short ma[24], mb[72];
static int ga[40];
static long long gl[8];

static long dot24(void)
{
    long s = 0;
    for (int k = 0; k < 24; k++)
        s += ma[k] * mb[k * 3];
    return s;
}
static long dot24_ref(void)
{
    long s = 0;
    for (int k = 0; k < 24 + vz; k++)
        s += ma[k] * mb[k * 3];
    return s;
}

/* starts at 3, and the counter is read after the loop */
static long from3(void)
{
    long s = 0;
    int i;
    for (i = 3; i < 11; i++)
        s += (long)i * ga[i];
    return s * 100 + i;
}
static long from3_ref(void)
{
    long s = 0;
    int i;
    for (i = 3 + vz; i < 11 + vz; i++)
        s += (long)i * ga[i];
    return s * 100 + i;
}

/* a pointer walk that starts two elements in, off a pointer argument */
__attribute__((noinline)) static long mid(const int *a)
{
    long s = 0;
    for (const int *p = a + 2; p != a + 10; p++)
        s = s * 3 + *p;
    return s;
}
static long mid_ref(void)
{
    long s = 0;
    for (int *p = &ga[2 + vz]; p != &ga[10 + vz]; p++)
        s = s * 3 + *p;
    return s;
}

/* the same shape off two DIFFERENT pointers: 8 apart only when they
 * are equal, and called here with them one element apart, so 9 trips */
__attribute__((noinline)) static long span(const int *a, const int *c)
{
    long s = 0;
    for (const int *p = a + 2; p != c + 10; p++)
        s = s * 3 + *p;
    return s;
}
static long span_ref(void)
{
    long s = 0;
    for (int i = 2; i < 11 + vz; i++)
        s = s * 3 + ga[i];
    return s;
}

static long two(void)
{
    long s = 5;
    for (int i = 0; i < 2; i++)
        s = s * 7 + (ga[i] ^ i);
    return s;
}
static long t32(void)
{
    long s = 0;
    for (int i = 0; i < 32; i++)
        s += ga[i] & 0xff;
    return s;
}
static long t33(void)
{
    long s = 0;
    for (int i = 0; i < 33; i++)
        s += ga[i] & 0xff;
    return s;
}
static long tn(int n)
{
    long s = 0;
    for (int i = 0; i < n; i++)
        s += ga[i] & 0xff;
    return s;
}

/* eight-byte elements, and a running value carried out of the loop */
static long long wide(void)
{
    long long s = 1;
    for (int i = 0; i < 8; i++)
        s = s * 3 + gl[i];
    return s;
}
static long long wide_ref(void)
{
    long long s = 1;
    for (int i = 0; i < 8 + vz; i++)
        s = s * 3 + gl[i];
    return s;
}

/* a nest of constant loops, the inner one inside the outer's body */
static long nest(void)
{
    long s = 0;
    for (int i = 0; i < 6; i++)
        for (int j = 0; j < 8; j++)
            s += ga[i + j] ^ (i * 8 + j);
    return s;
}
static long nest_ref(void)
{
    long s = 0;
    for (int i = 0; i < 6 + vz; i++)
        for (int j = 0; j < 8 + vz; j++)
            s += ga[i + j] ^ (i * 8 + j);
    return s;
}

/* a local whose address is taken inside the body, so it lives in memory */
static long slot(void)
{
    long s = 0;
    for (int i = 0; i < 8; i++) {
        int t = ga[i] + i;
        int *q = &t;
        *q += 1;
        s += t;
    }
    return s;
}
static long slot_ref(void)
{
    long s = 0;
    for (int i = 0; i < 8 + vz; i++) {
        int t = ga[i] + i;
        int *q = &t;
        *q += 1;
        s += t;
    }
    return s;
}

int main(void)
{
    for (int k = 0; k < 24; k++)
        ma[k] = (short)(k * 37 - 300 + vz);
    for (int k = 0; k < 72; k++)
        mb[k] = (short)(500 - k * 11 + vz);
    for (int k = 0; k < 40; k++)
        ga[k] = k * k * 13 - 7 * k + 1 + vz;
    for (int k = 0; k < 8; k++)
        gl[k] = (long long)k * 1000000007LL - 3 + vz;
    if (dot24() != dot24_ref())
        return 1;
    if (from3() != from3_ref())
        return 2;
    if (mid(ga) != mid_ref())
        return 3;
    if (span(ga, ga + 1) != span_ref())
        return 9;
    if (two() != (5 * 7 + (ga[0] ^ 0)) * 7 + (ga[1] ^ 1))
        return 4;
    if (t32() != tn(32 + vz) || t33() != tn(33 + vz))
        return 5;
    if (wide() != wide_ref())
        return 6;
    if (nest() != nest_ref())
        return 7;
    if (slot() != slot_ref())
        return 8;
    return 42;
}
