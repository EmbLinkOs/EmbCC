/* Values made in only the bytes their readers want (src/opt/immfold.c,
 * pass_demand). A 64-bit field loaded and masked down to a 16-bit key
 * loads two bytes; a shift that moves every wanted bit out is 0; an and
 * that keeps every wanted byte is a move. Each result is checked against
 * the same arithmetic through volatile, which nothing narrows -- and the
 * cases that must NOT narrow are here too: a carry into a byte a compare
 * reads, a right shift, a value a wider reader sees, and an eight-byte
 * value punned to a double and widened to long double (the read that
 * made the pass loop for ever on RV32). Fixed widths throughout, so GCC
 * computes the same. */
// expect-exit: 42
#include <stdint.h>

struct st { uint64_t ino, dev; };
union pun { double d; uint64_t w; };

__attribute__((noinline)) static uint16_t key(const struct st *s, int id)
{
    return (uint16_t)((s->ino & 0xffff) | (s->dev & 0xff) << 16 |
                      (uint64_t)id << 24);
}

__attribute__((noinline)) static uint8_t low_sum(uint32_t a, uint32_t b)
{
    return (uint8_t)(a + b * 3u - (a ^ b));
}

__attribute__((noinline)) static int carry_seen(uint32_t a, uint32_t b)
{
    return (a + b) > 0xffffu;        /* every byte of the sum is read */
}

__attribute__((noinline)) static uint16_t shifted_right(uint32_t a)
{
    return (uint16_t)((a * 5u) >> 12);   /* the high bytes matter */
}

__attribute__((noinline)) static uint16_t gone(uint32_t a)
{
    return (uint16_t)(a << 16);       /* nothing left in the low two */
}

__attribute__((noinline)) static uint16_t kept(uint32_t a)
{
    return (uint16_t)(a & 0x00ffffffu);
}

__attribute__((noinline)) static long double widen(const union pun *p)
{
    union pun x = *p;
    return (long double)x.d;
}

__attribute__((noinline)) static uint32_t wide_reader(uint64_t a, int n)
{
    uint64_t t = a * 7u + 1;         /* read whole by the shift below */
    return (uint32_t)(t >> n);
}

int main(void)
{
    volatile uint64_t vino = 0x123456789abcdef0ull, vdev = 0xfedcba9876543210ull;
    volatile uint32_t va = 0xfff0fff1u, vb = 0x0000f00fu;
    struct st s = { vino, vdev };
    uint64_t wk = (vino & 0xffff) | (vdev & 0xff) << 16 | (uint64_t)7 << 24;
    if (key(&s, 7) != (uint16_t)wk) return 1;
    if (low_sum(va, vb) != (uint8_t)(va + vb * 3u - (va ^ vb))) return 2;
    if (carry_seen(va, vb) != 1 || carry_seen(1u, 2u) != 0) return 3;
    if (shifted_right(va) != (uint16_t)((va * 5u) >> 12)) return 4;
    if (gone(va) != 0) return 5;
    if (kept(va) != (uint16_t)va) return 6;
    union pun p;
    p.d = 2.5;
    if (widen(&p) != 2.5L) return 7;
    if (wide_reader(vino, 35) != (uint32_t)((vino * 7u + 1) >> 35)) return 8;
    return 42;
}
