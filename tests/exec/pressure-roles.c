/* Register pressure on Cortex-M with r9-r11 holding values: 64-bit
 * accumulators live across calls take the callee-saved pairs up to
 * r10:r11, so the lowering's scratch roles come from whatever low
 * registers an instruction leaves free (codegen.c's t_roles_low) -- and
 * the instruction's own low scratch (LO) must not hand the same register
 * out again. Stores and loads at offsets past the 16-bit and the negative
 * immediate forms' reach build their address in a role while the value
 * waits in a low scratch: the shape where a shared register would store
 * the address instead of the value. And a struct argument read through a
 * pointer into r0-r2, where a role in r0 would be overwritten by the
 * first word before the second is read. tests/golden/thumb-size.sh runs
 * it with the r9-r11 attempt forced (EMBCC_T_EXT=1). */
// expect-exit: 42
typedef unsigned long long u64;
static volatile int sinkv;

__attribute__((noinline)) int f(int x) { return x * 3 + sinkv; }
struct s3 { int x, y, z; };
__attribute__((noinline)) int g(struct s3 t) { return t.x - 2 * t.y + 3 * t.z; }

__attribute__((noinline)) u64 mix(int *p, int n, int k, struct s3 *q)
{
    u64 a = 1, b = 2, c = 3, d = 4, e = 5;
    int s = 1;
    for (int i = 0; i < n; i++) {
        int v = f(i + k) + g(q[i & 3]);   /* a struct in r0-r2, from memory */
        a = a * 33 + (u64)v;
        b = b * 31 + (u64)(v ^ k);
        c = c * 29 + (u64)(unsigned)(v - i);
        d = d * 27 + (u64)(unsigned)(v + s);
        e ^= a + (b >> 3) + (c << 2) + d;
        p[-1000 - i] = v + s;                /* far below: no immediate */
        p[2000 + i] = v - s;                 /* far above the 16-bit form */
        s += (p[-1000 - i] - p[2000 + i]) & 7;
    }
    return a + b + c + d + e + (u64)(unsigned)s;
}

static int buf[3200];
static struct s3 qs[4] = { { 1, 2, 3 }, { 4, 5, 6 }, { 7, 8, 9 }, { -1, -2, -3 } };

int main(void)
{
    u64 r = mix(buf + 1100, 50, 7, qs);
    /* the same computation, written plainly */
    u64 a = 1, b = 2, c = 3, d = 4, e = 5;
    int s = 1;
    for (int i = 0; i < 50; i++) {
        const struct s3 *t = &qs[i & 3];
        int v = (i + 7) * 3 + t->x - 2 * t->y + 3 * t->z;
        a = a * 33 + (u64)v;
        b = b * 31 + (u64)(v ^ 7);
        c = c * 29 + (u64)(unsigned)(v - i);
        d = d * 27 + (u64)(unsigned)(v + s);
        e ^= a + (b >> 3) + (c << 2) + d;
        if (buf[1100 - 1000 - i] != v + s || buf[1100 + 2000 + i] != v - s)
            return 1;
        s += ((v + s) - (v - s)) & 7;
    }
    if (r != a + b + c + d + e + (u64)(unsigned)s)
        return 2;
    return 42;
}
