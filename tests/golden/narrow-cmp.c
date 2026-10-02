/* Comparisons of narrow values -- char and short, signed and not, against
 * each other and against constants at the edges of each type -- as values
 * and as branches, against the host (tests/golden/avr-narrow-cmp.sh).
 *
 * The AVR backend compares two extensions of the same kind at their own
 * width (extension preserves order) and a zero-extended pair UNSIGNED
 * whatever the IR asked, since at four bytes it is non-negative; it also
 * tests a narrow value for zero on its low bytes only. Each of those is
 * a place to get a sign or an edge wrong.
 *
 * Mixed-kind comparisons are written with both sides cast to int32_t:
 * with a 16-bit int (AVR) `uint16_t < int16_t` is an UNSIGNED comparison
 * and with a 32-bit int (the host) a signed one -- C really does differ
 * there -- so the host could not be the reference for it uncast. Cast, it
 * still has operands of different extension kinds, which must not be
 * narrowed. The work is split over three functions because -O0 keeps
 * every temporary in the frame, and one function's worth did not fit the
 * ATmega328P's 2 KB of SRAM. */
#include <stdint.h>
void writec(int c);
void puts_(const char *s);
static void hx(uint32_t v)
{
    for (int i = 28; i >= 0; i -= 4)
        writec("0123456789abcdef"[(v >> i) & 15]);
    writec(' ');
}
volatile uint8_t u8v[] = { 0, 1, 127, 128, 254, 255 };
volatile int8_t s8v[] = { -128, -127, -1, 0, 1, 127 };
volatile uint16_t u16v[] = { 0, 1, 255, 256, 32767, 32768, 65534, 65535 };
volatile int16_t s16v[] = { -32768, -32767, -256, -1, 0, 1, 255, 32767 };

#define CMP6(a, b) ((uint32_t)((a) < (b)) | (uint32_t)((a) <= (b)) << 1 | \
                    (uint32_t)((a) > (b)) << 2 | (uint32_t)((a) >= (b)) << 3 | \
                    (uint32_t)((a) == (b)) << 4 | (uint32_t)((a) != (b)) << 5)
static uint32_t br6(int lt, int le, int gt, int ge, int eq, int ne)
{
    uint32_t r = 0;
    if (lt) r |= 1; if (le) r |= 2; if (gt) r |= 4;
    if (ge) r |= 8; if (eq) r |= 16; if (ne) r |= 32;
    return r;
}
#define BR6(a, b) br6((a) < (b), (a) <= (b), (a) > (b), (a) >= (b), \
                      (a) == (b), (a) != (b))

__attribute__((noinline)) static void bytes(void)
{
    uint32_t h = 0, g = 0;
    for (int i = 0; i < 6; i++)
        for (int j = 0; j < 6; j++) {
            uint8_t a = u8v[i], b = u8v[j];
            int8_t c = s8v[i], d = s8v[j];
            h = h * 31 + CMP6(a, b); h = h * 31 + CMP6(c, d);
            g = g * 31 + BR6(a, b);  g = g * 31 + BR6(c, d);
            /* mixed kinds: must NOT be narrowed */
            h = h * 31 + CMP6(a, d); h = h * 31 + CMP6(c, b);
        }
    hx(h); hx(g);
}
__attribute__((noinline)) static void shorts(void)
{
    uint32_t h = 0, g = 0;
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++) {
            uint16_t a = u16v[i], b = u16v[j];
            int16_t c = s16v[i], d = s16v[j];
            h = h * 31 + CMP6(a, b); h = h * 31 + CMP6(c, d);
            g = g * 31 + BR6(a, b);  g = g * 31 + BR6(c, d);
            h = h * 31 + CMP6((int32_t)a, (int32_t)d);
            h = h * 31 + CMP6((uint8_t)a, c);
        }
    hx(h); hx(g);
}
__attribute__((noinline)) static void edges(void)
{
    uint32_t h = 0;
    for (int i = 0; i < 8; i++) {
        uint16_t a = u16v[i];
        int16_t c = s16v[i];
        h = h * 31 + CMP6(a, 32768u) + CMP6(a, 255u) * 3 + CMP6(a, 65535u) * 7;
        h = h * 31 + CMP6(c, -1) + CMP6(c, 0) * 3 + CMP6(c, -32768) * 7;
        h = h * 31 + (a ? 1u : 0u) + (c ? 2u : 0u) + ((uint8_t)a ? 4u : 0u);
        if (c < 0) h += 11;
        if (a > 1000u) h += 13;
        if ((int8_t)a < 0) h += 17;
    }
    hx(h);
}

int main(void)
{
    bytes();
    shorts();
    edges();
    puts_("\n==END==\n");
    return 0;
}
