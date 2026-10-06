/* Two rewrites, checked against what C says.
 *
 * An address `(base + K) + i` used by several accesses becomes
 * `(base + i)` with K in each access's offset: arrays inside structs
 * indexed and updated in place, at offsets on both sides of where a
 * target's short encodings stop reaching, and a base reassigned after K
 * was added to it -- whose old value the accesses must still use.
 *
 * And a branch on an unsigned `x < 1`, `x >= 1`, `x > 0` or `x <= 0` is a
 * branch on x == 0, at every width, for 0, 1 and the largest value. */
// expect-exit: 42

struct rec {
    int pad[17];
    unsigned notify[4];          /* 68 bytes in */
    unsigned char state[4];      /* 84 */
    unsigned short h[4];         /* 88 */
#ifdef __AVR__
    int far_pad[8];              /* the Uno has 2 KB of RAM */
#else
    int far_pad[480];
#endif
    unsigned far[4];             /* 2016: past RISC-V's 2047 for some i */
};

static struct rec ra, rb;

/* a small offset, which AVR's ldd/std reach too */
struct small { char tag; unsigned v[4]; };
static struct small sm;
/* a base a loop walks: the address is taken before the walk moves it,
 * and the accesses after it must still use the old one */
static struct small walkv[4];
__attribute__((noinline)) static unsigned walk(struct small *p, unsigned i,
                                               int n)
{
    unsigned s = 0;
    for (int k = 0; k < n; k++) {
        unsigned *a = p->v;
        p++;
        a[i] |= 1u << k;
        s += a[i];
    }
    return s;
}
/* ...and one reassigned on one path between taking the address and
 * using it */
__attribute__((noinline)) static unsigned branchy(struct small *p,
                                                  struct small *o,
                                                  unsigned i, int c)
{
    unsigned *a = p->v;
    if (c)
        p = o;
    a[i] |= 8;
    a[i] += 1;
    return a[i] + p->v[0];
}
__attribute__((noinline)) static void smadd(struct small *t, unsigned i)
{
    t->v[i] = t->v[i] * 3 + 1;
    t->v[i] ^= 5;
}

__attribute__((noinline)) static void orin(struct rec *t, unsigned i, unsigned v)
{
    t->notify[i] |= v;
    t->notify[i] += 1;
}
__attribute__((noinline)) static void bump(struct rec *t, unsigned i)
{
    t->state[i] = (unsigned char)(t->state[i] + 3);
    t->h[i] = (unsigned short)(t->h[i] * 2 + 1);
    t->far[i] ^= 0x55u;
    t->far[i] += t->notify[i];
}
/* the base moves after K was added: the accesses use the old one */
__attribute__((noinline)) static unsigned moved(struct rec *p, struct rec *q,
                                                unsigned i)
{
    unsigned *a = p->notify;
    p = q;
    a[i] |= 0x100;
    a[i] += 2;
    return a[i] + p->notify[i];
}

__attribute__((noinline)) static int u8(unsigned char x)
{
    int r = 0;
    if (x < 1u) r |= 1;
    if (x >= 1u) r |= 2;
    if (x > 0u) r |= 4;
    if (x <= 0u) r |= 8;
    return r;
}
__attribute__((noinline)) static int u32(unsigned x)
{
    int r = 0;
    if (x < 1u) r |= 1;
    if (x >= 1u) r |= 2;
    if (x > 0u) r |= 4;
    if (x <= 0u) r |= 8;
    return r;
}
__attribute__((noinline)) static int u64(unsigned long long x)
{
    int r = 0;
    if (x < 1ull) r |= 1;
    if (x >= 1ull) r |= 2;
    if (x > 0ull) r |= 4;
    if (x <= 0ull) r |= 8;
    return r;
}
/* signed: not the same thing, and must not be rewritten */
__attribute__((noinline)) static int s32(int x)
{
    int r = 0;
    if (x < 1) r |= 1;
    if (x >= 1) r |= 2;
    if (x > 0) r |= 4;
    if (x <= 0) r |= 8;
    return r;
}

int main(void)
{
    for (unsigned i = 0; i < 4; i++) {
        ra.notify[i] = 10 * i;
        ra.state[i] = (unsigned char)(250 + i);
        ra.h[i] = (unsigned short)(1000 * i);
        ra.far[i] = i;
        rb.notify[i] = 7;
    }
    for (unsigned i = 0; i < 4; i++) {
        orin(&ra, i, 1u << i);
        bump(&ra, i);
    }
    for (unsigned i = 0; i < 4; i++) {
        if (ra.notify[i] != ((10 * i) | (1u << i)) + 1) return 1;
        if (ra.state[i] != (unsigned char)(253 + i)) return 2;
        if (ra.h[i] != (unsigned short)(2000 * i + 1)) return 3;
        if (ra.far[i] != (i ^ 0x55u) + ra.notify[i]) return 4;
    }
    for (unsigned i = 0; i < 4; i++) {
        sm.v[i] = i + 2;
        smadd(&sm, i);
    }
    for (unsigned i = 0; i < 4; i++)
        if (sm.v[i] != (((i + 2) * 3 + 1) ^ 5)) return 8;
    for (int k = 0; k < 4; k++)
        walkv[k].v[1] = 16u * (unsigned)k;
    if (walk(walkv, 1, 3) != (0u | 1) + (16u | 2) + (32u | 4)) return 9;
    if (walkv[0].v[1] != 1 || walkv[1].v[1] != 18 || walkv[2].v[1] != 36 ||
        walkv[3].v[1] != 48) return 9;
    walkv[0].v[2] = 0;
    walkv[3].v[0] = 100;
    walkv[3].v[2] = 50;
    if (branchy(&walkv[0], &walkv[3], 2, 1) != 9 + 100) return 14;
    if (walkv[0].v[2] != 9 || walkv[3].v[2] != 50) return 15;
    unsigned before = ra.notify[2];
    if (moved(&ra, &rb, 2) != ((before | 0x100) + 2) + 7) return 5;
    if (ra.notify[2] != (before | 0x100) + 2) return 6;
    if (rb.notify[2] != 7) return 7;

    if (u8(0) != 9 || u8(1) != 6 || u8(255) != 6) return 10;
    if (u32(0) != 9 || u32(1) != 6 || u32(0xffffffffu) != 6) return 11;
    if (u64(0) != 9 || u64(1) != 6 || u64(~0ull) != 6 ||
        u64(0x100000000ull) != 6) return 12;
    if (s32(0) != 9 || s32(1) != 6 || s32(-1) != 9 ||
        s32(-2147483647 - 1) != 9) return 13;
    return 42;
}
