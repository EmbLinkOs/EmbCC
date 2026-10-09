/* Values the register allocator leaves in memory, read many times: more
 * live values than registers, across calls, so some spill. The Thumb
 * backend keeps a spilled value in a free low register for a stretch of
 * reads (its reload cache, across labels and loop back edges), and makes
 * a spilled constant again where it is read rather than loading it --
 * with `movs` only where the flags are dead. Each function's answer is
 * computed here the slow way too.
 * Exit 0, or the number of the first wrong result. */
// expect-exit: 0

typedef __UINT32_TYPE__ u32;

static volatile u32 vin[16] = { 3, 17, 0x80000001u, 255, 256, 7, 0xfffffffeu,
                                     42, 1000, 9, 0x1234, 31, 64, 5, 0, 0x7fffffffu };

__attribute__((noinline)) static u32 id(u32 x) { return x; }
__attribute__((noinline)) static void sink(u32 *p, u32 x) { *p += x; }

/* A loop body full of constants LICM hoists to the preheader, six sums
 * and a call in it, so the constants cannot all keep a register across
 * the loop: they are made again where they are read. */
__attribute__((noinline)) static u32 consts_in_loop(const char *s)
{
    u32 acc = 0, n = 0, s1 = 0, s2 = 0, s3 = 0, s4 = 0, s5 = 0, s6 = 0;
    for (const char *p = s; *p; p++) {
        u32 c = (unsigned char)*p;
        if (c == '%')
            sink(&acc, 1);
        else if (c == ' ')
            sink(&acc, 0x20u);
        else if (c & 0x80u)
            sink(&acc, 0xfffffff8u);
        else
            sink(&acc, (c & 0xfffffffeu) + 300u);
        s1 += c ^ 0x5au;
        s2 = s2 * 3u + (c | 0x100u);
        s3 ^= s1 + 0xff00u;
        s4 += s2 >> 3;
        s5 = (s5 << 1) ^ (c == '%' ? 0x20u : 1u);
        s6 += s5 & 0xfffffffeu;
        n = id(n + 0x1234u);
    }
    return acc ^ n ^ s1 ^ s2 ^ s3 ^ s4 ^ s5 ^ s6;
}

static u32 consts_ref(const char *s)
{
    u32 acc = 0, n = 0, s1 = 0, s2 = 0, s3 = 0, s4 = 0, s5 = 0, s6 = 0;
    for (const char *p = s; *p; p++) {
        u32 c = (unsigned char)*p;
        acc += c == '%' ? 1u : c == ' ' ? 0x20u : (c & 0x80u) ? 0xfffffff8u
               : (c & 0xfffffffeu) + 300u;
        s1 += c ^ 0x5au;
        s2 = s2 * 3u + (c | 0x100u);
        s3 ^= s1 + 0xff00u;
        s4 += s2 >> 3;
        s5 = (s5 << 1) ^ (c == '%' ? 0x20u : 1u);
        s6 += s5 & 0xfffffffeu;
        n += 0x1234u;
    }
    return acc ^ n ^ s1 ^ s2 ^ s3 ^ s4 ^ s5 ^ s6;
}

/* Twelve values live across a call, then each read several times on
 * branches and in a loop with no call: the reads past the first may come
 * from a register the cache holds -- which nothing in between may take. */
__attribute__((noinline)) static u32 many_live(u32 k)
{
    u32 a = vin[0] + k, b = vin[1] + k, c = vin[2] + k, d = vin[3] + k;
    u32 e = vin[4] + k, f = vin[5] + k, g = vin[6] + k, h = vin[7] + k;
    u32 i = vin[8] + k, j = vin[9] + k, m = vin[10] + k, n = vin[11] + k;
    u32 t = id(k);
    u32 r = 0;
    if (t & 1) {
        r = a * 3 + b - c;
        r ^= a >> 3;
        r += (a & 0xff) + (b >> 2);
        if (r > d)
            r ^= a + d + (a << 2);
        else
            r += b ^ d ^ (b >> 5);
    } else {
        r = e + f * 5 - g;
        r += (r < h) ? e : h;
        r ^= (e >> 1) + (f << 2) + (e & g);
    }
    for (u32 q = 0; q < (k & 7) + 2; q++) {
        r += a ^ q;
        r = (r << 1 | r >> 31) + i;
        if (r & 4)
            r -= j;
        r ^= m + n;
    }
    return r + a + b + c + d + e + f + g + h + i + j + m + n;
}

static u32 many_live_ref(u32 k)
{
    u32 v[12], r;
    for (int x = 0; x < 12; x++)
        v[x] = vin[x] + k;
    if (k & 1) {
        r = v[0] * 3 + v[1] - v[2];
        r ^= v[0] >> 3;
        r += (v[0] & 0xff) + (v[1] >> 2);
        if (r > v[3])
            r ^= v[0] + v[3] + (v[0] << 2);
        else
            r += v[1] ^ v[3] ^ (v[1] >> 5);
    } else {
        r = v[4] + v[5] * 5 - v[6];
        r += (r < v[7]) ? v[4] : v[7];
        r ^= (v[4] >> 1) + (v[5] << 2) + (v[4] & v[6]);
    }
    for (u32 q = 0; q < (k & 7) + 2; q++) {
        r += v[0] ^ q;
        r = (r << 1 | r >> 31) + v[8];
        if (r & 4)
            r -= v[9];
        r ^= v[10] + v[11];
    }
    for (int x = 0; x < 12; x++)
        r += v[x];
    return r;
}

/* A spilled value read before a loop and again at the top of its body,
 * then the body's own temporaries fill the low registers: a register
 * holding the value from the first read must stay free to the loop's
 * back edge, not only to the last read, or the next trip reads whatever
 * the temporaries left there. */
__attribute__((noinline)) static u32 backedge(u32 k)
{
    u32 a = vin[0] + k, b = vin[1] + k, c = vin[2] + k, d = vin[3] + k;
    u32 e = vin[4] + k, f = vin[5] + k, g = vin[6] + k, h = vin[7] + k;
    u32 i = vin[8] + k, j = vin[9] + k, m = vin[10] + k, n = vin[11] + k;
    u32 t = id(k);
    u32 r = a + t;
    for (u32 q = 0; q < (k & 3) + 3; q++) {
        r += a ^ q;
        u32 x1 = r * 3u, x2 = r >> 2, x3 = r ^ 0x55u, x4 = r + q;
        u32 x5 = x1 ^ x4, x6 = x2 - x3;
        r = (x1 + x2) ^ (x3 - x4) ^ (x1 & x4) ^ (x2 | x3) ^ (x5 + x6) ^ (x5 & x6);
    }
    r = id(r);
    return r + a + b + c + d + e + f + g + h + i + j + m + n;
}

static u32 backedge_ref(u32 k)
{
    u32 v[12], r;
    for (int x = 0; x < 12; x++)
        v[x] = vin[x] + k;
    r = v[0] + k;
    for (u32 q = 0; q < (k & 3) + 3; q++) {
        r += v[0] ^ q;
        u32 x1 = r * 3u, x2 = r >> 2, x3 = r ^ 0x55u, x4 = r + q;
        u32 x5 = x1 ^ x4, x6 = x2 - x3;
        r = (x1 + x2) ^ (x3 - x4) ^ (x1 & x4) ^ (x2 | x3) ^ (x5 + x6) ^ (x5 & x6);
    }
    for (int x = 0; x < 12; x++)
        r += v[x];
    return r;
}

/* A switch value spilled across calls and tested against one case after
 * another; constants chosen by a compare, so a constant made again lands
 * between a compare and what reads its flags. */
__attribute__((noinline)) static u32 dispatch(u32 x, u32 y)
{
    u32 u = id(x), w = id(y), r = 0;
    u32 p1 = vin[12], p2 = vin[13], p3 = vin[14], p4 = vin[15];
    r += id(p1 + p2);
    if (u == 3)        r = w + 0x20u;
    else if (u == 17)  r = w ^ 0xfffffffeu;
    else if (u == 255) r = (w > 100) ? 1u : 0x101u;
    else if (u == 256) r = w + (w < 7 ? 0x55u : 0xaau);
    else if (u > 1000) r = u - 0x1234u;
    else               r = w * 3u + 1u;
    r += (w & 1) ? 0x20u : 0x40u;
    r += (r == 0x20u) ? 7u : 0u;
    return r + id(p3 ^ p4) + p1 + p2 + p3 + p4;
}

static u32 dispatch_ref(u32 x, u32 y)
{
    u32 r;
    if (x == 3)        r = y + 0x20u;
    else if (x == 17)  r = y ^ 0xfffffffeu;
    else if (x == 255) r = (y > 100) ? 1u : 0x101u;
    else if (x == 256) r = y + (y < 7 ? 0x55u : 0xaau);
    else if (x > 1000) r = x - 0x1234u;
    else               r = y * 3u + 1u;
    r += (y & 1) ? 0x20u : 0x40u;
    r += (r == 0x20u) ? 7u : 0u;
    return r + (vin[14] ^ vin[15]) + vin[12] + vin[13] + vin[14] + vin[15];
}

static int nth, first_bad;

static void check(u32 got, u32 want)
{
    nth++;
    if (got != want && !first_bad)
        first_bad = nth;
}

int main(void)
{
    static const char *strs[] = { "", "%", "a b%c", "\x80\xff zz%%", "hello, world 100%" };
    for (u32 s = 0; s < sizeof strs / sizeof strs[0]; s++)
        check(consts_in_loop(strs[s]), consts_ref(strs[s]));
    for (u32 k = 0; k < 20; k++)
        check(many_live(k * 7u + 1u), many_live_ref(k * 7u + 1u));
    for (u32 k = 0; k < 12; k++)
        check(backedge(k * 5u + 2u), backedge_ref(k * 5u + 2u));
    for (u32 k = 0; k < 16; k++)
        for (u32 y = 0; y < 12; y++)
            check(dispatch(vin[k], y * 37u), dispatch_ref(vin[k], y * 37u));
    return first_bad;
}
