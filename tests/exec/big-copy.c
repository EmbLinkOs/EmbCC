/* Block copies and clears past the size where x86-64 stops unrolling them
 * and uses `rep movsq` / `rep stosq` (256 bytes), and below it, where the
 * chunks are sixteen bytes through an xmm register: struct assignment and
 * zeroing at sizes either side of it and with every tail length 0..7,
 * a whole-array copy loop (which the optimizer turns into one block
 * copy), and copies with many values live across them, so that rsi and
 * rdi -- which the string instructions need -- are holding someone
 * else's values and must come back intact. Bytes next to every
 * destination are checked too. */
// expect-exit: 42
#define S(N) struct s##N { unsigned char b[N]; }
S(255); S(256); S(257); S(258); S(259); S(260); S(261); S(262); S(263); S(264);
S(1000); S(4099); S(17); S(24); S(31); S(32); S(33); S(48); S(100);

static unsigned char seed = 1;
static void fill(unsigned char *p, int n) { for (int i = 0; i < n; i++) p[i] = (unsigned char)(seed = seed * 33 + 7); }
static int same(const unsigned char *a, const unsigned char *b, int n) { for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0; return 1; }
static int zero(const unsigned char *a, int n) { for (int i = 0; i < n; i++) if (a[i]) return 0; return 1; }

#define CHECK(N) do { \
    struct { unsigned char lo[8]; struct s##N d; unsigned char hi[8]; } box; \
    struct s##N src; \
    fill(src.b, N); fill(box.lo, 8); fill(box.hi, 8); \
    unsigned char lo[8], hi[8]; for (int k = 0; k < 8; k++) { lo[k] = box.lo[k]; hi[k] = box.hi[k]; } \
    copy_##N(&box.d, &src); \
    if (!same(box.d.b, src.b, N) || !same(box.lo, lo, 8) || !same(box.hi, hi, 8)) bad |= 1; \
    clear_##N(&box.d); \
    if (!zero(box.d.b, N) || !same(box.lo, lo, 8) || !same(box.hi, hi, 8)) bad |= 2; \
} while (0)
#define FUNCS(N) \
    __attribute__((noinline)) void copy_##N(struct s##N *d, const struct s##N *s) { *d = *s; } \
    __attribute__((noinline)) void clear_##N(struct s##N *d) { struct s##N z = { { 0 } }; *d = z; }
FUNCS(255) FUNCS(256) FUNCS(257) FUNCS(258) FUNCS(259) FUNCS(260) FUNCS(261) FUNCS(262)
FUNCS(263) FUNCS(264) FUNCS(1000) FUNCS(4099)
FUNCS(17) FUNCS(24) FUNCS(31) FUNCS(32) FUNCS(33) FUNCS(48) FUNCS(100)

/* the idiom shape: a copy loop over a whole array */
static unsigned a1[30000], a2[30000];
__attribute__((noinline)) void copy_arr(void) { for (int i = 0; i < 30000; i++) a2[i] = a1[i]; }
__attribute__((noinline)) void clear_arr(void) { for (int i = 0; i < 30000; i++) a2[i] = 0; }

/* many values live across a large copy: rsi and rdi are someone's */
__attribute__((noinline)) long live_across(struct s1000 *d, const struct s1000 *s,
                                           long a, long b, long c, long e, long f, long g)
{
    long x1 = a * 3 + b, x2 = b * 5 + c, x3 = c * 7 + e, x4 = e * 11 + f, x5 = f * 13 + g;
    long x6 = g * 17 + a, x7 = a ^ g, x8 = b ^ f;
    *d = *s;
    return x1 + 2 * x2 + 3 * x3 + 4 * x4 + 5 * x5 + 6 * x6 + 7 * x7 + 8 * x8 + d->b[999];
}

/* by value: a struct argument goes to the outgoing area, from a source
 * whose address is loaded once; past 256 bytes with rep movsq, while the
 * scalar arguments around it are still in the registers it borrows */
struct props { long v[21]; };                       /* 168 bytes, like EmProps */
__attribute__((noinline)) long take_props(long a, long b, struct props p, long c)
{ long s = a * 3 + b * 5 + c * 7; for (int k = 0; k < 21; k++) s += p.v[k] * (k + 1); return s; }
__attribute__((noinline)) long take_big(long a, struct s1000 p, long b, struct s257 q, long c)
{ long s = a - b + c; for (int k = 0; k < 1000; k++) s += p.b[k] * (k & 7); for (int k = 0; k < 257; k++) s ^= (long)q.b[k] << (k & 15); return s; }

int main(void)
{
    int bad = 0;
    CHECK(255); CHECK(256); CHECK(257); CHECK(258); CHECK(259); CHECK(260); CHECK(261);
    CHECK(262); CHECK(263); CHECK(264); CHECK(1000); CHECK(4099);
    CHECK(17); CHECK(24); CHECK(31); CHECK(32); CHECK(33); CHECK(48); CHECK(100);

    for (int i = 0; i < 30000; i++) { a1[i] = (unsigned)i * 2654435761u; a2[i] = 7; }
    copy_arr();
    for (int i = 0; i < 30000; i++) if (a2[i] != (unsigned)i * 2654435761u) { bad |= 4; break; }
    clear_arr();
    for (int i = 0; i < 30000; i++) if (a2[i]) { bad |= 8; break; }

    static struct s1000 d, s;
    fill(s.b, 1000);
    long a = 3, b = -7, c = 11, e = 1000003, f = -99, g = 12345;
    long want = (a * 3 + b) + 2 * (b * 5 + c) + 3 * (c * 7 + e) + 4 * (e * 11 + f) +
                5 * (f * 13 + g) + 6 * (g * 17 + a) + 7 * (a ^ g) + 8 * (b ^ f) + s.b[999];
    if (live_across(&d, &s, a, b, c, e, f, g) != want || !same(d.b, s.b, 1000)) bad |= 16;
    {
        struct props p;
        long want2 = 11 * 3 + -4 * 5 + 99 * 7;
        for (int k = 0; k < 21; k++) { p.v[k] = k * k - 50; want2 += p.v[k] * (k + 1); }
        if (take_props(11, -4, p, 99) != want2) bad |= 32;
        static struct s1000 bp; static struct s257 bq;
        fill(bp.b, 1000); fill(bq.b, 257);
        long w3 = 5 - 6 + 7;
        for (int k = 0; k < 1000; k++) w3 += bp.b[k] * (k & 7);
        for (int k = 0; k < 257; k++) w3 ^= (long)bq.b[k] << (k & 15);
        if (take_big(5, bp, 6, bq, 7) != w3) bad |= 64;
    }
    return bad ? 100 + bad : 42;
}
