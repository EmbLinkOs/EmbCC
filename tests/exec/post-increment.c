/* Loops that post-increment a counter or a pointer while still reading
 * its old value: `b[m++] = t[--n]`, `*p++ = x`, a digit loop, and two
 * counters stepping together. -O2 moves each update down next to the copy
 * that ends it, so it shares the counter's register; the old value must
 * still be what every read before the update sees, and the final value
 * what the code after the loop sees. */
// expect-exit: 42
static volatile int vz;

static int rev(char *b, const char *t, int n)
{
    int m = 0;
    while (n)
        b[m++] = t[--n];
    b[m] = 0;
    return m;
}

static int utoa(char *b, unsigned v)
{
    char t[12];
    int n = 0, m = 0;
    do {
        t[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        b[m++] = t[--n];
    b[m] = 0;
    return m;
}

/* the old value read after the update would be computed */
static long weave(int *a, int n)
{
    long s = 0;
    int i = 0, j = 100;
    while (i < n) {
        s += (long)i * 3 + j;
        a[i++] = (int)s;
        j -= 2;
    }
    return s * 1000 + i * 10 + (j & 7);
}

static int fill(char *p, int n, char c)
{
    char *q = p;
    while (n--)
        *q++ = c++;
    return (int)(q - p) * 1000 + (unsigned char)c;
}

static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

int main(void)
{
    char b[32], t[16] = "embedded!";
    int a[16];
    if (rev(b, t, 9 + vz) != 9 || !same(b, "!deddebme"))
        return 1;
    if (utoa(b, 4294967295u + vz) != 10 || !same(b, "4294967295"))
        return 2;
    if (utoa(b, 0u + vz) != 1 || !same(b, "0"))
        return 3;
    if (utoa(b, 7051u + vz) != 4 || !same(b, "7051"))
        return 4;
    /* s = sum over i of 3i + (100 - 2i) = sum(i + 100) for i < 6 = 615 */
    if (weave(a, 6 + vz) != 615L * 1000 + 6 * 10 + ((100 - 12) & 7))
        return 5;
    if (a[0] != 100 || a[5] != 615)
        return 6;
    if (fill(b, 5 + vz, 'a') != 5 * 1000 + 'f' || b[0] != 'a' || b[4] != 'e')
        return 7;
    return 42;
}
