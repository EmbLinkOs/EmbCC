/* A branch whose arm changes nothing when its guard is 0 -- `if (v)
 * { n += v; v = 0; }` -- is taken out (src/opt/ifconv.c, pass_ifident):
 * with v == 0, n + 0 is n and v = 0 is v. Checked for each operation
 * whose identity is 0 (add, or, xor, and sub and shift with the guard on
 * the right), and against the near misses that must keep their branch: a
 * guard reset to 1, an arm that adds v + 1, a guard on the left of a
 * subtraction. Each against the same loop with the branch kept by a
 * volatile read. */
// expect-exit: 42
typedef unsigned int u32;

static const unsigned char in[] = "a1b22c333 4x 55yy6 0z7 89";
#define N ((int)sizeof in - 1)
static volatile int keep = 1;

__attribute__((noinline)) u32 digits(void)
{
    u32 val = 0, nums = 0, ors = 0, xs = 0, subs = 1000, sh = 1;
    for (int i = 0; i < N; i++) {
        u32 c = in[i];
        if (c >= '0' && c <= '9') { val = val * 10 + (c - '0'); continue; }
        if (val) { nums += val; val = 0; }
        if (c & 1) ors |= c - 'a';
        u32 k = c & 3;
        if (k) xs ^= k;
        if (k) subs -= k;
        if (k) sh <<= k;
    }
    return nums * 7 + ors * 5 + xs * 3 + subs + sh + val;
}

static u32 digits_ref(void)
{
    u32 val = 0, nums = 0, ors = 0, xs = 0, subs = 1000, sh = 1;
    for (int i = 0; i < N; i++) {
        u32 c = in[i];
        if (c >= '0' && c <= '9') { val = val * 10 + (c - '0'); continue; }
        if (val && keep) { nums += val; val = 0; }
        if ((c & 1) && keep) ors |= c - 'a';
        u32 k = c & 3;
        if (k && keep) xs ^= k;
        if (k && keep) subs -= k;
        if (k && keep) sh <<= k;
    }
    return nums * 7 + ors * 5 + xs * 3 + subs + sh + val;
}

/* the near misses, each an arm that is NOT the identity when the guard
 * is 0: a value zeroed that is not the guard; the guard on the left of a
 * subtraction; one more added */
__attribute__((noinline)) u32 misses(void)
{
    u32 a = 0, b = 0, c = 100, s = 0;
    for (int i = 0; i < N; i++) {
        u32 k = in[i] & 3, w = in[i] ^ 1;
        if (k) { a += k; w = 0; }        /* w is not k */
        s += w;
        if (k) { c = k - c; b += k; }    /* k - c with k == 0 is -c */
        if (k) { b += k + 1; a ^= k; }   /* k + 1 */
    }
    return a * 7 + b * 5 + c * 3 + s;
}

static u32 misses_ref(void)
{
    u32 a = 0, b = 0, c = 100, s = 0;
    for (int i = 0; i < N; i++) {
        u32 k = in[i] & 3, w = in[i] ^ 1;
        if (k && keep) { a += k; w = 0; }
        s += w;
        if (k && keep) { c = k - c; b += k; }
        if (k && keep) { b += k + 1; a ^= k; }
    }
    return a * 7 + b * 5 + c * 3 + s;
}

int main(void)
{
    if (digits() != digits_ref()) return 1;
    if (misses() != misses_ref()) return 2;
    return 42;
}
