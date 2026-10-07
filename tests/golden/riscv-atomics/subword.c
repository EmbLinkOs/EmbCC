/* One- and two-byte atomics on RISC-V, which work on the word around the
 * byte or halfword (src/arch/riscv/codegen.c, sub_lane): every operation
 * on every lane of one word, with the whole word printed after each, so
 * a merge that touched a neighbouring lane shows. Signed results are
 * extended; the compare-and-swaps take both outcomes. The host runs the
 * same program, and tests/golden/riscv-atomics.sh compares the outputs. */
void puts_(const char *s);
void putn(long v);

static union {
    unsigned int w;
    unsigned char b[4];
    unsigned short h[2];
} u;

/* in halves, so a 32-bit long prints it as a 64-bit one does */
static void word(void) { putn((long)(u.w >> 16)); putn((long)(u.w & 0xffff)); }

/* through arguments: the address and the operand arrive in a0/a1 */
__attribute__((noinline)) unsigned char add_b(unsigned char *p, unsigned char v)
{ return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }
__attribute__((noinline)) unsigned short nand_h(unsigned short *p, unsigned short v)
{ return __atomic_fetch_nand(p, v, __ATOMIC_SEQ_CST); }
__attribute__((noinline)) int cas_b(unsigned char *p, unsigned char *e, unsigned char d)
{ return __atomic_compare_exchange_n(p, e, d, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }

int main(void)
{
    u.w = 0x11223344u;
    putn(__atomic_fetch_add(&u.b[1], 0xF0, __ATOMIC_SEQ_CST)); word();
    putn(__atomic_fetch_sub(&u.b[3], 0x12, __ATOMIC_SEQ_CST)); word();
    putn(__atomic_fetch_and(&u.b[0], 0x0F, __ATOMIC_SEQ_CST)); word();
    putn(__atomic_fetch_or(&u.b[2], 0x80, __ATOMIC_SEQ_CST)); word();
    putn(__atomic_fetch_xor(&u.h[1], 0xFFFF, __ATOMIC_SEQ_CST)); word();
    putn(__atomic_fetch_nand(&u.h[0], 0x0F0F, __ATOMIC_SEQ_CST)); word();
    putn(__atomic_exchange_n(&u.b[3], 0x7E, __ATOMIC_SEQ_CST)); word();
    putn(__atomic_add_fetch(&u.h[1], 3, __ATOMIC_SEQ_CST)); word();
    puts_("\n");

    unsigned char e = 0x99;
    putn(__atomic_compare_exchange_n(&u.b[1], &e, 0x55, 0, __ATOMIC_SEQ_CST,
                                     __ATOMIC_SEQ_CST));
    putn(e); word();
    e = u.b[1];
    putn(__atomic_compare_exchange_n(&u.b[1], &e, 0x55, 0, __ATOMIC_SEQ_CST,
                                     __ATOMIC_SEQ_CST));
    putn(e); word();
    putn(__sync_val_compare_and_swap(&u.h[0], u.h[0], 0xBEEF)); word();
    putn(__sync_val_compare_and_swap(&u.h[1], 1, 2)); word();
    putn(__sync_bool_compare_and_swap(&u.b[2], u.b[2], 0x01)); word();
    puts_("\n");

    putn(add_b(&u.b[2], 0x41)); word();
    putn(nand_h(&u.h[1], 0x00FF)); word();
    e = 0x77;
    putn(cas_b(&u.b[3], &e, 0x10)); putn(e); word();
    e = u.b[3];
    putn(cas_b(&u.b[3], &e, 0x10)); word();
    puts_("\n");

    static signed char sc = -5;
    static short ss = -300;
    putn(__atomic_fetch_add(&sc, 10, __ATOMIC_SEQ_CST)); putn(sc);
    putn(__atomic_fetch_sub(&sc, 100, __ATOMIC_SEQ_CST)); putn(sc);
    putn(__atomic_fetch_sub(&ss, 1, __ATOMIC_SEQ_CST)); putn(ss);
    putn(__atomic_exchange_n(&ss, -32768, __ATOMIC_SEQ_CST)); putn(ss);
    puts_("\nDONE\n");
    return 0;
}
