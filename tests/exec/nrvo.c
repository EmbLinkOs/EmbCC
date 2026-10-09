/* A struct returned through the caller's buffer, built there in place
 * when every return gives back the same local -- and not when the returns
 * give back different locals, or the local is volatile. The local's
 * address may escape to a function that fills it; the function may call
 * another struct-returning function, itself included; and the result
 * may be large enough to be copied with a loop.
 * Exit 0, or the number of the first wrong result. */
// expect-exit: 0

typedef __INT32_TYPE__ i32;
struct two { i32 q, r; };
struct three { i32 a, b, c; };
struct big { i32 v[24]; };

static volatile i32 vk = 7;

__attribute__((noinline)) static struct two qr(i32 n, i32 d)
{
    struct two t;
    t.q = n / d;
    t.r = n % d;
    return t;
}

/* two locals, two returns: neither may be the buffer */
__attribute__((noinline)) static struct three pick(i32 x)
{
    struct three a, b;
    a.a = x; a.b = x + 1; a.c = x + 2;
    b.a = -x; b.b = -x - 1; b.c = -x - 2;
    if (x & 1)
        return a;
    return b;
}

/* one local, two returns of it */
__attribute__((noinline)) static struct three early(i32 x)
{
    struct three r;
    r.a = x;
    if (x > 100) {
        r.b = r.c = 0;
        return r;
    }
    r.b = x * 2;
    r.c = x * 3;
    return r;
}

__attribute__((noinline)) static void fill(struct three *p, i32 x)
{
    p->a = x ^ 5;
    p->b = x ^ 6;
    p->c = x ^ 7;
}

/* the address escapes to a filler, then is read back */
__attribute__((noinline)) static struct three filled(i32 x)
{
    struct three r;
    fill(&r, x);
    r.c += r.a;
    return r;
}

__attribute__((noinline)) static struct three vol(i32 x)
{
    volatile struct three r;
    r.a = x; r.b = x + x; r.c = r.a + r.b;
    return r;
}

/* recursion: the inner result lands in the outer one */
__attribute__((noinline)) static struct big ramp(i32 n)
{
    struct big r;
    if (n == 0) {
        for (i32 k = 0; k < 24; k++)
            r.v[k] = k;
        return r;
    }
    r = ramp(n - 1);
    for (i32 k = 0; k < 24; k++)
        r.v[k] += n * (k + 1);
    return r;
}

/* a struct-returning call inside, its result copied into the local */
__attribute__((noinline)) static struct three via_call(i32 x)
{
    struct three r;
    struct two t = qr(x * 10 + 3, 4);
    r.a = t.q;
    r.b = t.r;
    r.c = t.q + t.r;
    return r;
}

static struct three g3;

/* reads a global while building its result; assigned back to it below */
__attribute__((noinline)) static struct three from_global(i32 x)
{
    struct three r;
    r.a = x;
    r.b = g3.a;
    r.c = g3.b + g3.a;
    return r;
}

static int nth, first_bad;

static void check(i32 got, i32 want)
{
    nth++;
    if (got != want && !first_bad)
        first_bad = nth;
}

int main(void)
{
    i32 k = vk;
    struct two t = qr(100 + k, k);
    check(t.q, (100 + k) / k);
    check(t.r, (100 + k) % k);
    struct three p = pick(k);
    check(p.a + p.b + p.c, 3 * k + 3);
    p = pick(k + 1);
    check(p.a + p.b + p.c, -3 * (k + 1) - 3);
    p = early(k);
    check(p.a * 10000 + p.b * 100 + p.c, k * 10000 + 2 * k * 100 + 3 * k);
    p = early(k + 200);
    check(p.a + p.b + p.c, k + 200);
    p = filled(k);
    check(p.a, k ^ 5);
    check(p.c, (k ^ 7) + (k ^ 5));
    p = vol(k);
    check(p.c, 3 * k);
    struct big b = ramp(3);
    i32 s = 0;
    for (i32 n = 0; n < 24; n++)
        s += b.v[n];
    check(s, 276 + 6 * 300);
    p = via_call(k);
    check(p.a, (k * 10 + 3) / 4);
    check(p.c, (k * 10 + 3) / 4 + (k * 10 + 3) % 4);
    g3.a = 11; g3.b = 22;
    g3 = from_global(k);
    check(g3.a, k);
    check(g3.b, 11);
    check(g3.c, 33);
    return first_bad;
}
