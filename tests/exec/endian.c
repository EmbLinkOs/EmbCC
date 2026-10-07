/* Byte order, on whichever machine this runs: the program finds the
 * target's order at run time -- a word's first byte through a union --
 * and then checks every other view of memory against what that order
 * says, so the same file passes little-endian (every board but one) and
 * big-endian (mips-none-elf) and fails wherever the compiler mixes them:
 *
 *   - a word, a halfword, a long long and a double seen as bytes, through
 *     unions and memcpy, built at run time and as static initializers
 *     (which the compiler lays out itself, not the machine);
 *   - a long long and a double as two 32-bit words: which is the high one;
 *   - a store of a wide value followed by a narrower read of part of it,
 *     the store-to-load forwarding an optimizer may do without memory;
 *   - bit-fields read back through bytes: the field declared first lies
 *     at the low end of its unit's FIRST byte little-endian and at the
 *     high end big-endian (gcc's layout and every ABI here);
 *   - wide string literals' elements as bytes;
 *   - network order made with shifts (htonl's way) against the native
 *     bytes.
 *
 * Only what C defines once the order is known is asserted: object
 * representations read as unsigned char, unions read through another
 * member (a reinterpretation, C11 6.5.2.3), and IEEE 754 doubles where a
 * double is eight bytes (AVR's is four, and checks a float instead). */
// expect-exit: 42
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static int g_be;   /* 1 big-endian, 0 little-endian */

/* byte k of an n-byte value v, in the target's order */
static unsigned byte_of(uint64_t v, int n, int k)
{
    int sh = g_be ? 8 * (n - 1 - k) : 8 * k;
    return (unsigned)(v >> sh) & 0xffu;
}

static int same_bytes(const void *p, uint64_t v, int n)
{
    const unsigned char *b = p;
    for (int k = 0; k < n; k++)
        if (b[k] != byte_of(v, n, k))
            return 0;
    return 1;
}

/* Static images: laid out by the compiler, not computed by the machine. */
static const union { uint32_t u; unsigned char b[4]; } s_word = { 0x11223344u };
static union { uint16_t h; unsigned char b[2]; } s_half = { 0xA1B2u };
static const uint64_t s_ll = 0x0102030405060708ULL;
static const uint32_t s_arr[2] = { 0xDEADBEEFu, 0x01234567u };
struct bits { unsigned a : 3; unsigned b : 5; unsigned c : 8; };
static const struct bits s_bits = { 5, 17, 0xC3 };
static const struct { unsigned char tag; uint16_t h; uint32_t w; } s_rec =
    { 0x7E, 0xBEEF, 0xCAFEF00Du };
struct with_ptr { int x; uint32_t *p; };
static const struct with_ptr s_ptr = { 1, (uint32_t *)0 };

/* Volatile inputs, so nothing below folds at compile time unless the
 * optimizer reasons about the bytes itself -- which is what is tested. */
static volatile uint32_t v_word = 0x11223344u;
static volatile uint64_t v_ll = 0x8899AABBCCDDEEFFULL;
static volatile unsigned v_a = 6, v_b = 9;

__attribute__((noinline)) static unsigned first_byte(uint32_t x)
{
    union { uint32_t u; unsigned char b[4]; } t;
    t.u = x;
    return t.b[0];        /* a store, then a narrower read of its start */
}

__attribute__((noinline)) static uint32_t word_at(uint64_t x, int k)
{
    union { uint64_t u; uint32_t w[2]; } t;
    t.u = x;
    return t.w[k];
}

__attribute__((noinline)) static uint16_t half_at(uint32_t x, int k)
{
    uint16_t h[2];
    memcpy(h, &x, 4);
    return h[k];
}

/* A double's and a long long's words at CONSTANT indices, the way fdlibm's
 * GET_HIGH_WORD reads them: a store of the whole value and a narrower
 * load of part of it, which an optimizer may forward without memory --
 * and must pick the right half to. */
__attribute__((noinline)) static uint32_t dword(double x, int which)
{
    union { double d; uint32_t w[2]; } t;
    t.d = x;
    return which ? t.w[1] : t.w[0];
}

__attribute__((noinline)) static uint32_t llword0(uint64_t x)
{
    union { uint64_t q; uint32_t w[2]; } t;
    t.q = x;
    return t.w[0];
}

__attribute__((noinline)) static uint32_t llword1(uint64_t x)
{
    union { uint64_t q; uint32_t w[2]; } t;
    t.q = x;
    return t.w[1];
}

/* network (big-endian) order with shifts, the portable way */
static uint32_t my_htonl(uint32_t x)
{
    unsigned char b[4];
    uint32_t r;
    b[0] = (unsigned char)(x >> 24);
    b[1] = (unsigned char)(x >> 16);
    b[2] = (unsigned char)(x >> 8);
    b[3] = (unsigned char)x;
    memcpy(&r, b, 4);
    return r;
}

int main(void)
{
    union { uint32_t u; unsigned char b[4]; } probe;
    probe.u = 0x01020304u;
    if (probe.b[0] == 4 && probe.b[1] == 3 && probe.b[2] == 2 && probe.b[3] == 1)
        g_be = 0;
    else if (probe.b[0] == 1 && probe.b[1] == 2 && probe.b[2] == 3 &&
             probe.b[3] == 4)
        g_be = 1;
    else
        return 1;                 /* neither order: the union is wrong */

    /* run-time views */
    uint32_t w = v_word;
    if (!same_bytes(&w, 0x11223344u, 4)) return 2;
    if (first_byte(w) != byte_of(0x11223344u, 4, 0)) return 3;
    uint64_t ll = v_ll;
    if (!same_bytes(&ll, 0x8899AABBCCDDEEFFULL, 8)) return 4;
    if (word_at(ll, g_be ? 0 : 1) != 0x8899AABBu) return 5;
    if (word_at(ll, g_be ? 1 : 0) != 0xCCDDEEFFu) return 6;
    if (llword0(ll) != (g_be ? 0x8899AABBu : 0xCCDDEEFFu)) return 31;
    if (llword1(ll) != (g_be ? 0xCCDDEEFFu : 0x8899AABBu)) return 32;
    if (sizeof(double) == 8) {
        volatile double vx = -2.5;              /* 0xC004000000000000 */
        double x = vx;
        if (dword(x, 0) != (g_be ? 0xC0040000u : 0)) return 33;
        if (dword(x, 1) != (g_be ? 0 : 0xC0040000u)) return 34;
    }
    if (half_at(w, g_be ? 0 : 1) != 0x1122u) return 7;
    if (half_at(w, g_be ? 1 : 0) != 0x3344u) return 8;

    /* static views */
    if (s_word.b[0] != byte_of(0x11223344u, 4, 0) ||
        s_word.b[3] != byte_of(0x11223344u, 4, 3)) return 9;
    if (s_half.b[0] != byte_of(0xA1B2u, 2, 0)) return 10;
    if (!same_bytes(&s_ll, 0x0102030405060708ULL, 8)) return 11;
    if (!same_bytes(&s_arr[1], 0x01234567u, 4)) return 12;
    if (!same_bytes(&s_rec.h, 0xBEEF, 2) || !same_bytes(&s_rec.w, 0xCAFEF00Du, 4))
        return 13;
    {
        unsigned char z[sizeof s_ptr.p];
        memcpy(z, (const unsigned char *)&s_ptr + offsetof(struct with_ptr, p),
               sizeof z);
        for (size_t k = 0; k < sizeof z; k++)
            if (z[k]) return 14;  /* a null pointer is zeros in either order */
    }

    /* bit-fields through bytes: a then b fill the first byte, c the next */
    {
        unsigned char b[sizeof s_bits];
        memcpy(b, &s_bits, sizeof b);
        unsigned want0 = g_be ? (5u << 5) | 17u : 5u | (17u << 3);
        if (b[0] != want0 || b[1] != 0xC3) return 15;
        struct bits r;
        memset(&r, 0, sizeof r);
        r.a = v_a;
        r.b = v_b;
        r.c = 0x5A;
        memcpy(b, &r, sizeof b);
        want0 = g_be ? (6u << 5) | 9u : 6u | (9u << 3);
        if (b[0] != want0 || b[1] != 0x5A) return 16;
        /* and back: bytes written, the fields read */
        b[0] = (unsigned char)(g_be ? (3u << 5) | 30u : 3u | (30u << 3));
        b[1] = 0x99;
        memcpy(&r, b, sizeof b);
        if (r.a != 3 || r.b != 30 || r.c != 0x99) return 17;
    }

    /* floating point: IEEE 754 bits as bytes and as words */
    if (sizeof(double) == 8) {
        double d = -2.5;          /* 0xC004000000000000 */
        volatile double vd = d;
        unsigned char b[8];
        uint64_t u;
        uint32_t halves[2];
        d = vd;
        memcpy(b, &d, 8);
        memcpy(&u, &d, 8);
        memcpy(halves, &d, 8);
        if (u != 0xC004000000000000ULL) return 18;
        if (!same_bytes(b, 0xC004000000000000ULL, 8)) return 19;
        if (halves[g_be ? 0 : 1] != 0xC0040000u || halves[g_be ? 1 : 0] != 0)
            return 20;
        static const double s_d = 1.5;        /* 0x3FF8000000000000 */
        if (!same_bytes(&s_d, 0x3FF8000000000000ULL, 8)) return 21;
        double back;
        uint64_t bits = 0x400921FB54442D18ULL;    /* pi */
        memcpy(&back, &bits, 8);
        if (back < 3.14159 || back > 3.1416) return 22;
    } else {
        float f = -2.5f;          /* 0xC0200000 */
        volatile float vf = f;
        uint32_t u;
        f = vf;
        memcpy(&u, &f, 4);
        if (u != 0xC0200000u || !same_bytes(&f, 0xC0200000u, 4)) return 18;
    }
    {
        static const float s_f = 0.75f;           /* 0x3F400000 */
        if (!same_bytes(&s_f, 0x3F400000u, 4)) return 23;
    }

    /* wide characters: each element in the target's order */
    {
        static const __CHAR16_TYPE__ s16[] = u"\x0102\x0304";
        static const __CHAR32_TYPE__ s32[] = U"\x01020304";
        if (!same_bytes(&s16[0], 0x0102, 2) || !same_bytes(&s16[1], 0x0304, 2))
            return 24;
        if (!same_bytes(&s32[0], 0x01020304u, 4)) return 25;
        const unsigned char *lit = (const unsigned char *)u"\x0a0b";
        if (lit[0] != byte_of(0x0a0b, 2, 0) || lit[1] != byte_of(0x0a0b, 2, 1))
            return 26;
    }

    /* network order */
    {
        uint32_t n = my_htonl(v_word);
        unsigned char b[4];
        memcpy(b, &n, 4);
        if (b[0] != 0x11 || b[1] != 0x22 || b[2] != 0x33 || b[3] != 0x44)
            return 27;
        if (n != (g_be ? 0x11223344u : 0x44332211u)) return 28;
    }

    /* a narrow store into a wide value, then the wide read */
    {
        union { uint32_t u; unsigned char b[4]; } t;
        t.u = 0;
        t.b[g_be ? 3 : 0] = 0x7F;           /* the low byte */
        if (t.u != 0x7Fu) return 29;
        t.b[g_be ? 0 : 3] = 0x80;           /* the high byte */
        if (t.u != 0x8000007Fu) return 30;
    }
    return 42;
}
