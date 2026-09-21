/* Truncate to 32 bits, then widen back to 64 — at -O2, where the x86-64
 * register allocator used to lose the truncation. A 4-byte store into a local
 * that shares the 64-bit source's register emits no move, so the register
 * still held the high half; widening it back was then a 64-bit register
 * move, which copies that half instead of clearing it (only a 32-bit move
 * zero-extends). `(unsigned long)(unsigned int)x` came back as x.
 * tests/golden/regalloc-O2.sh runs this at -O2. The calls go through volatile
 * function pointers so inlining and constant folding cannot route around the
 * code under test.
 */
// expect-exit: 42
static int nonzero(unsigned long x) { return x != 0; }
static unsigned long low32(unsigned long v) { unsigned int u = (unsigned int)v; return u; }
static int low32_nonzero(unsigned long v) { unsigned int u = (unsigned int)v; return nonzero(u); }
static long low32_signed(long v) { int s = (int)v; return s; }

static unsigned long (*volatile p_low32)(unsigned long) = low32;
static int (*volatile p_low32_nonzero)(unsigned long) = low32_nonzero;
static long (*volatile p_low32_signed)(long) = low32_signed;

int main(void)
{
    if (p_low32(0x100000000UL) != 0) return 1;
    if (p_low32(0x1234567887654321UL) != 0x87654321UL) return 2;
    if (p_low32_nonzero(0x100000000UL) != 0) return 3;
    if (p_low32_nonzero(0x100000001UL) != 1) return 4;
    if (p_low32_signed(0x1FFFFFFFFL) != -1) return 5;   /* sign-extends */
    if (p_low32_signed(0x100000005L) != 5) return 6;
    return 42;
}
