/* A byte value in a register the byte forms cannot name without REX.
 *
 * Register numbers 4..7 in an 8-bit operand mean %ah %ch %dh %bh with
 * no REX prefix, and %spl %bpl %sil %dil with one. So `movzbl %sil,%ebx`
 * and `mov %sil,(%rax)` and `sete %sil` each need a REX prefix that
 * carries no bits at all, and emitting them without it silently names a
 * different register.
 *
 * It stayed invisible while the x86-64 allocator's pool was
 * {r8..r15, rbx, rdx}: every one of those is either REX-extended
 * already or directly addressable as al/bl/cl/dl. Adding the ABI hints
 * -- which put a parameter in the register it arrived in -- made rsi
 * and rdi the ordinary home of a `char` parameter, and three emitters
 * started assembling `%sil` as `%dh`.
 *
 * What it broke was not arithmetic but EXCEPTIONS: read_encoded() in
 * lib/libcxx/src/eh.cc switches on a `unsigned char enc` parameter, so
 * every DWARF encoding dispatched as absptr, every landing-pad search
 * read the wrong bytes, and every throw reached std::terminate.
 *
 * The cases below are the three forms, over every value a byte can
 * take, referreed against gcc. They are written so the byte spends its
 * life as a PARAMETER: that is what puts it in rsi/rdi.
 */
// expect-exit: 42

#include <stdio.h>

/* movzx/movsx from a byte register: a switch on a char parameter. */
static int dispatch(const unsigned char *p, unsigned char enc, long bias)
{
    switch (enc & 0x0f) {
    case 0x00: return (int)p[0] + (int)bias;
    case 0x01: return (int)p[1] * 2;
    case 0x02: return (int)p[2] * 3;
    case 0x03: return (int)p[3] * 5;
    case 0x04: return (int)p[4] * 7;
    default:   return -(int)(enc & 0x70);
    }
}

/* a byte STORE out of a register that holds a char parameter */
static void putbyte(unsigned char *dst, unsigned char v, long i)
{
    dst[i] = v;
}

/* setcc into a register holding a byte-width value */
static int cmps(signed char a, signed char b, unsigned char c)
{
    int r = 0;
    r |= (a == b) << 0;
    r |= (a < b)  << 1;
    r |= (c > 200u) << 2;
    r |= (a != 0) << 3;
    return r;
}

/* the same byte read back at several widths, still a parameter */
static long widths(unsigned char u, signed char s, unsigned short w)
{
    return (long)u + (long)s + (long)w + (long)(unsigned)u * 3;
}

int main(void)
{
    static const unsigned char buf[8] = { 11, 22, 33, 44, 55, 66, 77, 88 };
    unsigned char out[256];
    unsigned long h = 0;

    for (int e = 0; e < 256; e++) {
        h = h * 1000003u + (unsigned)dispatch(buf, (unsigned char)e, e * 3);
        putbyte(out, (unsigned char)(e ^ 0x5a), e & 0xff);
        h = h * 1000003u + out[e & 0xff];
        h = h * 1000003u + (unsigned)cmps((signed char)e, (signed char)(e >> 1),
                                          (unsigned char)e);
        h = h * 1000003u + (unsigned long)widths((unsigned char)e,
                                                 (signed char)e,
                                                 (unsigned short)(e * 257));
    }
    printf("%lu\n", h);
    return 42;
}
