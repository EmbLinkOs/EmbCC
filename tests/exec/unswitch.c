/* Loops with a test they cannot change, which -O3 takes out of them
 * (src/opt/unswitch.c): the loop is copied, one copy for each answer.
 * Each function is checked against a twin that reads its condition
 * through a volatile, which no copy of the loop can know, so the twin
 * stays one loop. What can go wrong is in the copy: a value carried
 * round it or read after it that is not moved in or back, an exit that
 * forgets its stub, a counter that starts in the wrong place -- each
 * gives a different answer from the twin's. Every function runs with
 * both answers of its test and with trip counts from 0 up. */
// expect-exit: 42
static volatile int vz;      /* 0, and nothing the compiler can know */
#define V(x) ((x) + vz)

static int a[64];
#define NI __attribute__((noinline))

/* the two sides of a test, each a store */
NI static void two_arms(int *p, int n, int mode)
{
    for (int i = 0; i < n; i++) {
        if (mode) p[i] += 3;
        else p[i] = p[i] * 5 - 1;
    }
}
NI static void two_arms_ref(int *p, int n, int mode)
{
    for (int i = 0; i < n; i++) {
        if (V(mode)) p[i] += 3;
        else p[i] = p[i] * 5 - 1;
    }
}

/* an accumulator read after the loop */
NI static int sum_live(int n, int neg)
{
    int s = 0;
    for (int i = 0; i < n; i++) {
        if (neg) s -= a[i];
        else s += a[i];
    }
    return s;
}
NI static int sum_live_ref(int n, int neg)
{
    int s = 0;
    for (int i = 0; i < n; i++) {
        if (V(neg)) s -= a[i];
        else s += a[i];
    }
    return s;
}

/* exits in the middle, the counter and another value read after */
NI static int find(int n, int want, int strict)
{
    int i, last = -1;
    for (i = 0; i < n; i++) {
        if (strict) { if (a[i] == want) break; }
        else if (a[i] >= want) { last = i; break; }
        last = a[i];
    }
    return i * 1000 + last;
}
NI static int find_ref(int n, int want, int strict)
{
    int i, last = -1;
    for (i = 0; i < n; i++) {
        if (V(strict)) { if (a[i] == want) break; }
        else if (a[i] >= want) { last = i; break; }
        last = a[i];
    }
    return i * 1000 + last;
}

/* a return from inside the loop */
NI static int ret_in(int n, int k)
{
    int acc = 7;
    for (int i = 0; i < n; i++) {
        acc = acc * 3 + a[i];
        if (k) { if (acc > 5000) return acc; }
        else acc ^= i;
    }
    return -acc;
}
NI static int ret_in_ref(int n, int k)
{
    int acc = 7;
    for (int i = 0; i < n; i++) {
        acc = acc * 3 + a[i];
        if (V(k)) { if (acc > 5000) return acc; }
        else acc ^= i;
    }
    return -acc;
}

/* a test the outer loop cannot change either: taken out of both */
NI static int nested(int n, int m, int flag)
{
    int t = 0;
    for (int i = 0; i < n; i++)
        for (int j = 0; j < m; j++) {
            if (flag) t += i * j;
            else t -= j;
        }
    return t;
}
NI static int nested_ref(int n, int m, int flag)
{
    int t = 0;
    for (int i = 0; i < n; i++)
        for (int j = 0; j < m; j++) {
            if (V(flag)) t += i * j;
            else t -= j;
        }
    return t;
}

/* a test the inner loop cannot change and the outer one does */
NI static int inner_only(int n, int m)
{
    int t = 0;
    for (int i = 0; i < n; i++) {
        int f = i & 1;
        for (int j = 0; j < m; j++) {
            if (f) t += j;
            else t += 2 * j + i;
        }
    }
    return t;
}
NI static int inner_only_ref(int n, int m)
{
    int t = 0;
    for (int i = 0; i < n; i++) {
        int f = i & 1;
        for (int j = 0; j < m; j++) {
            if (V(f)) t += j;
            else t += 2 * j + i;
        }
    }
    return t;
}

/* two tests: four copies */
NI static int two_tests(int n, int x, int y)
{
    int s = 1;
    for (int i = 0; i < n; i++) {
        if (x) s += a[i];
        if (y) s *= 3; else s ^= a[i];
    }
    return s;
}
NI static int two_tests_ref(int n, int x, int y)
{
    int s = 1;
    for (int i = 0; i < n; i++) {
        if (V(x)) s += a[i];
        if (V(y)) s *= 3; else s ^= a[i];
    }
    return s;
}

/* the test's variable has its address taken and is stored through
 * inside the loop: it is not invariant, and must not be taken out */
NI static int addr_taken(int n, int init)
{
    int flag = init, *fp = &flag, s = 0;
    for (int i = 0; i < n; i++) {
        if (flag) s += i;
        else s -= 1;
        if (i == 3) *fp = !*fp;
    }
    return s;
}
NI static int addr_taken_ref(int n, int init)
{
    int flag = init, s = 0;
    for (int i = 0; i < n; i++) {
        if (V(flag)) s += i;
        else s -= 1;
        if (i == 3) flag = !flag;
    }
    return s;
}

/* a 64-bit accumulator, on a pointer's test */
NI static long long ptr_cond(int n, const char *tag)
{
    long long s = 0;
    for (int i = 0; i < n; i++) {
        if (tag) s += (long long)a[i] * tag[0];
        else s -= a[i];
    }
    return s;
}
NI static long long ptr_cond_ref(int n, const char *tag)
{
    long long s = 0;
    for (int i = 0; i < n; i++) {
        if (tag + vz) s += (long long)a[i] * tag[0];
        else s -= a[i];
    }
    return s;
}

/* a do-while, the test at the top of its body */
NI static unsigned dowhile(unsigned x, int rev)
{
    unsigned r = 0;
    do {
        if (rev) r = (r << 1) | (x & 1);
        else r += x & 1;
        x >>= 1;
    } while (x);
    return r;
}
NI static unsigned dowhile_ref(unsigned x, int rev)
{
    unsigned r = 0;
    do {
        if (V(rev)) r = (r << 1) | (x & 1);
        else r += x & 1;
        x >>= 1;
    } while (x);
    return r;
}

/* floating point, and a continue */
NI static double fsum(int n, int sq)
{
    double s = 0;
    for (int i = 0; i < n; i++) {
        if (sq) { if (a[i] & 1) continue; s += (double)a[i] * a[i]; }
        else s += a[i] * 0.5;
    }
    return s;
}
NI static double fsum_ref(int n, int sq)
{
    double s = 0;
    for (int i = 0; i < n; i++) {
        if (V(sq)) { if (a[i] & 1) continue; s += (double)a[i] * a[i]; }
        else s += a[i] * 0.5;
    }
    return s;
}

/* a narrow test: only its low byte is the answer */
NI static int chcond(int n, unsigned char c)
{
    int s = 0;
    for (int i = 0; i < n; i++) { if (c) s += a[i]; else s += 1; }
    return s;
}
NI static int chcond_ref(int n, unsigned char c)
{
    int s = 0;
    for (int i = 0; i < n; i++) { if ((unsigned char)V(c)) s += a[i]; else s += 1; }
    return s;
}

int main(void)
{
    int b1[64], b2[64], bad = 0;
    for (int i = 0; i < 64; i++)
        a[i] = (i * 37) % 23 - 7;
    for (int mode = 0; mode < 2; mode++)
        for (int n = 0; n <= 21; n++) {
            for (int i = 0; i < 64; i++)
                b1[i] = b2[i] = a[i] + i;
            two_arms(b1, n, mode);
            two_arms_ref(b2, n, mode);
            for (int i = 0; i < 64; i++)
                bad |= b1[i] != b2[i];
            bad |= (sum_live(n, mode) != sum_live_ref(n, mode)) << 1;
            bad |= (find(n, 5, mode) != find_ref(n, 5, mode)) << 2;
            bad |= (find(n, -7, mode) != find_ref(n, -7, mode)) << 2;
            bad |= (ret_in(n + 5, mode) != ret_in_ref(n + 5, mode)) << 3;
            bad |= (nested(n, n / 2 + 1, mode) != nested_ref(n, n / 2 + 1, mode)) << 4;
            bad |= (inner_only(n, 5) != inner_only_ref(n, 5)) << 5;
            for (int y = 0; y < 2; y++)
                bad |= (two_tests(n, mode, y) != two_tests_ref(n, mode, y)) << 6;
            bad |= (addr_taken(n, mode) != addr_taken_ref(n, mode)) << 7;
            bad |= (ptr_cond(n, mode ? "x" : 0) != ptr_cond_ref(n, mode ? "x" : 0)) << 8;
            bad |= (dowhile(0xb5u * (unsigned)(n + 1), mode) !=
                    dowhile_ref(0xb5u * (unsigned)(n + 1), mode)) << 9;
            bad |= (fsum(n, mode) != fsum_ref(n, mode)) << 10;
            bad |= (chcond(n, (unsigned char)(mode * 256 + mode)) !=
                    chcond_ref(n, (unsigned char)(mode * 256 + mode))) << 11;
            bad |= (chcond(n, (unsigned char)(mode + 256)) !=
                    chcond_ref(n, (unsigned char)(mode + 256))) << 11;
        }
    return bad ? 1 : 42;
}
