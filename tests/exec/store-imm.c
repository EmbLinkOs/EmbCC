/* A constant stored to memory as one `mov $imm, mem` on x86-64: every
 * width (1, 2, 4, 8 bytes), immediates at the edges of what the 64-bit
 * form can sign-extend (0x7fffffff, -0x80000000) and just past them (so
 * the constant must be materialised), negative values, to a local's
 * frame address and through a pointer, a volatile store, and each
 * neighbour of every store checked. */
// expect-exit: 42
typedef unsigned long long u64;
struct rec {
    unsigned char g0; unsigned char b; unsigned char g1;
    short h; unsigned short g2;
    int i; unsigned g3;
    long long q; unsigned g4;
};

__attribute__((noinline)) void put(struct rec *r)
{ r->b = 0xa5; r->h = -2; r->i = -123456789; r->q = 0x7fffffffLL; }
__attribute__((noinline)) void put_edge(struct rec *r)
{ r->q = -2147483648LL; }
__attribute__((noinline)) void put_wide(struct rec *r)
{ r->q = 0x80000000LL; }
__attribute__((noinline)) void put_wider(struct rec *r)
{ r->q = 0x123456789abcdef0LL; }
__attribute__((noinline)) void put_vol(volatile int *p) { *p = 77; }

static int guards(const struct rec *r)
{ return r->g0 == 0x11 && r->g1 == 0x22 && r->g2 == 0x3333 && r->g3 == 0x44444444u && r->g4 == 0x55555555u; }

__attribute__((noinline)) long long local(int k)
{
    struct rec r = { 0x11, 1, 0x22, 2, 0x3333, 3, 0x44444444u, 4, 0x55555555u };
    r.b = 0x5a; r.h = 0x7fff; r.i = 0x7fffffff; r.q = -1;
    if (k) r.q = 0x100000000LL;
    return guards(&r) ? r.b + r.h + (long long)r.i + r.q : -999;
}

int main(void)
{
    int bad = 0;
    struct rec r = { 0x11, 1, 0x22, 2, 0x3333, 3, 0x44444444u, 4, 0x55555555u };
    put(&r);
    if (r.b != 0xa5 || r.h != -2 || r.i != -123456789 || r.q != 0x7fffffffLL || !guards(&r)) bad |= 1;
    put_edge(&r);
    if (r.q != -2147483648LL || !guards(&r)) bad |= 2;
    put_wide(&r);
    if (r.q != 0x80000000LL || !guards(&r)) bad |= 4;
    put_wider(&r);
    if ((u64)r.q != 0x123456789abcdef0ULL || !guards(&r)) bad |= 8;
    volatile int v = 0;
    put_vol(&v);
    if (v != 77) bad |= 16;
    if (local(0) != 0x5a + 0x7fff + 0x7fffffffLL - 1) bad |= 32;
    if (local(1) != 0x5a + 0x7fff + 0x7fffffffLL + 0x100000000LL) bad |= 64;
    return bad ? 100 + bad : 42;
}
