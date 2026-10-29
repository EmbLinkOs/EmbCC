/* The caller half of tests/golden/float16-abi.sh: every value made from
 * and checked as its bits, so nothing here converts a binary16 either.
 * Exits 42, or the number of the first call that came back wrong. */
typedef _Float16 h16;

h16 x_id(h16);
h16 x_pick(int, h16, float, h16, double, h16);
float x_after(h16, float);
#ifndef NO_BF16              /* (a reference compiler without __bf16) */
__bf16 x_bid(__bf16);
__bf16 x_bmix(int, __bf16, h16, float, __bf16);
#endif
int x_int(h16, int, h16);
h16 x_nth(int, h16, h16, h16, h16, h16, h16, h16, h16, h16, h16);

/* through a union: there is no C library on every side of this */
union hb { unsigned short u; h16 h;
#ifndef NO_BF16
    __bf16 b;
#endif
};
static h16 of(unsigned short u) { union hb x; x.u = u; return x.h; }
static unsigned short bits(h16 h) { union hb x; x.h = h; return x.u; }
#ifndef NO_BF16
static __bf16 bof(unsigned short u) { union hb x; x.u = u; return x.b; }
static unsigned short bbits(__bf16 b) { union hb x; x.b = b; return x.u; }
#endif

#ifdef WITH_STRUCTS
struct p2 { h16 a, b; };
struct mx { h16 a; int i; h16 b; };
struct p2 x_swap(struct p2);
struct mx x_mx(struct mx);
#endif

int main(void)
{
    /* the sign bit set, so a sign extension would show above it */
    h16 a = of(0xc248), b = of(0x3c01), c = of(0x8001);
    if (bits(x_id(a)) != 0xc248) return 1;
    if (bits(x_pick(0, a, 1.5f, b, 2.5, c)) != 0xc248) return 2;
    if (bits(x_pick(1, a, 1.5f, b, 2.5, c)) != 0x3c01) return 3;
    if (bits(x_pick(2, a, 1.5f, b, 2.5, c)) != 0x8001) return 4;
    if (x_after(a, 3.25f) != 3.25f) return 5;
    if (x_int(a, 77, b) != 77) return 6;
#ifndef NO_BF16
    if (bbits(x_bid(bof(0xc049))) != 0xc049) return 10;
    if (bbits(x_bmix(0, bof(0xbf81), a, 2.0f, bof(0x4040))) != 0xbf81) return 11;
    if (bbits(x_bmix(1, bof(0xbf81), a, 2.0f, bof(0x4040))) != 0x4040) return 12;
#endif
    for (int k = 0; k < 10; k++)
        if (bits(x_nth(k, of(0x100 + k), of(0x101), of(0x102), of(0x103),
                       of(0x104), of(0x105), of(0x106), of(0x107),
                       of(0x108), of(0x109))) != (k ? 0x100 + k : 0x100))
            return 7;
#ifdef WITH_STRUCTS
    struct p2 p = { a, b }, q = x_swap(p);
    if (bits(q.a) != 0x3c01 || bits(q.b) != 0xc248) return 8;
    struct mx m = { a, 41, c }, r = x_mx(m);
    if (bits(r.a) != 0x8001 || r.i != 42 || bits(r.b) != 0xc248) return 9;
#endif
    return 42;
}
