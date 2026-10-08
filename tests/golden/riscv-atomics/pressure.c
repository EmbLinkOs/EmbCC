/* One- and two-byte atomics with every caller-saved register in use
 * (tests/golden/riscv-atomics.sh). Nine values live across the atomic
 * fill a0-a7 and then t3, the one temporary in the allocator's pool -- and
 * at -O2 the atomic's own operand lands there. The sub-word sequences
 * once made the lane's shift in t3, over the operand (and over anything
 * else living there), so the byte added was the shift amount and the
 * compare-and-swap compared against it. The host computes the same. */
void puts_(const char *s);
void putn(long v);

static union { unsigned int w; unsigned char b[4]; unsigned short h[2]; } u;

__attribute__((noinline)) long many(long a, long b, long c, long d, long e,
                                    long f, long g, long h, unsigned char *p,
                                    unsigned char v)
{
    long x = a * 3 + h, y = b * 5 + g;
    unsigned char o = __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST);
    return a + b + c + d + e + f + g + h + x * 7 + y * 11 + o;
}

__attribute__((noinline)) long many_cas(long a, long b, long c, long d, long e,
                                        long f, long g, long h, unsigned short *p,
                                        unsigned short x0, unsigned short x1)
{
    long x = a * 3 + h, y = b * 5 + g;
    unsigned short o = __sync_val_compare_and_swap(p, x0, x1);
    return a + b + c + d + e + f + g + h + x * 7 + y * 11 + o;
}

int main(void)
{
    u.w = 0x11223344u;
    putn(many(1, 2, 3, 4, 5, 6, 7, 8, &u.b[2], 0x05));
    putn((long)(u.w >> 16)); putn((long)(u.w & 0xffff));
    putn(many_cas(1, 2, 3, 4, 5, 6, 7, 8, &u.h[1], 0x1127, 0xBEEF));
    putn((long)(u.w >> 16)); putn((long)(u.w & 0xffff));
    puts_("\nDONE\n");
    return 0;
}
