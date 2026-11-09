void puts_(const char *s);
void putn(long v);

struct acc { int sum; int n; unsigned max; };

__attribute__((noinline)) static unsigned crc8(const unsigned char *p, int len)
{
    unsigned crc = 0xff;
    for (int i = 0; i < len; i++) {
        unsigned b = p[i];
        crc ^= b;
        for (int k = 0; k < 8; k++) {
            unsigned top = crc & 0x80;
            crc = (crc << 1) & 0xff;
            if (top)
                crc ^= 0x31;
        }
    }
    return crc;
}

__attribute__((noinline)) static void add(struct acc *a, int v)
{
    int old = a->sum;
    a->sum = old + v;
    a->n++;
    if ((unsigned)v > a->max)
        a->max = (unsigned)v;
}

__attribute__((noinline)) static long long mix(long long x, int s, short t)
{
    long long y = x * 3 + s;
    int z = t * 7 - s;
    y ^= (long long)z << 5;
    return y + z;
}

__attribute__((noinline)) static int search(const int *v, int n, int want)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int got = v[mid];
        if (got == want)
            return mid;
        if (got < want)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

/* names declared again in sibling and inner blocks */
__attribute__((noinline)) static int shadow(int n)
{
    int s = 0;
    for (int i = 0; i < n; i++)
        s += i * 3;
    for (int i = n; i > 0; i--) {
        int t = i + s;
        s ^= t;
    }
    {
        int s = n * 11;
        n += s;
    }
    return s + n;
}

/* small enough to inline at -O2: its calls are inlined frames */
static int clamp(int v, int lo, int hi)
{
    int r = v;
    if (r < lo)
        r = lo;
    if (r > hi)
        r = hi;
    return r;
}

static const unsigned char msg[] = "hello, location lists";
static const int sorted[] = { 2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31 };

int main(void)
{
    struct acc a = { 0, 0, 0 };
    unsigned c = crc8(msg, 6);         /* (each byte is 8 rounds of stops) */
    for (int i = 0; i < 5; i++)
        add(&a, (int)c - i * 40);
    long long m = mix(0x123456789LL, a.sum, (short)a.n);
    int where = search(sorted, 11, 19);
    int none = search(sorted, 11, 4);
    int sh = shadow(6);
    int cl = 0;
    for (int j = -3; j < 9; j += 2)
        cl += clamp(j * (int)(c % 7), 0, 4);
    putn((long)c);
    putn(a.sum);
    putn((long)(m & 0xffff));
    putn(where + none);
    putn(sh);
    putn(cl);
    puts_("\n==END==\n");
    return 0;
}
