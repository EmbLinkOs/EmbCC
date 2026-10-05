/* The atomic builtins with their operands in REGISTERS.
 *
 * atomics.c takes the address of a local for every object, so at -O2 its
 * operands are mostly frame addresses and constants. Here each operation
 * is out of line, its object pointer, value, expected and desired all
 * arriving as parameters -- which is where the allocator keeps them, so
 * every lowering reads them from registers: any of the pool, at each
 * width, the byte forms included (`lock cmpxchg %sil,(%rdi)` needs a REX
 * prefix that %dl does not), and the compare-exchange whose `expected`
 * is itself a pointer held in a register. The loops at the end keep many
 * values live across the atomics, so some operands are spilled and are
 * read from their slots instead; EMBCC_RA_MAXPOOL forces that for all of
 * them. gcc referees every value.
 */
// expect-exit: 42
#define SEQ __ATOMIC_SEQ_CST
#define NI __attribute__((noinline))

NI static signed char xchg8(signed char *p, signed char v)
{ return __atomic_exchange_n(p, v, SEQ); }
NI static short xchg16(short *p, short v)
{ return __atomic_exchange_n(p, v, SEQ); }
NI static unsigned xchg32(unsigned *p, unsigned v)
{ return __atomic_exchange_n(p, v, SEQ); }
NI static long xchg64(long *p, long v)
{ return __atomic_exchange_n(p, v, SEQ); }

NI static unsigned char xadd8(unsigned char *p, unsigned char v)
{ return __atomic_fetch_add(p, v, SEQ); }
NI static short xadd16(short *p, short v)
{ return __atomic_fetch_sub(p, v, SEQ); }
NI static int xadd32(int *p, int v)
{ return __atomic_add_fetch(p, v, SEQ); }
NI static unsigned long xadd64(unsigned long *p, unsigned long v)
{ return __atomic_fetch_add(p, v, SEQ); }

/* by value: the value seen comes back */
NI static signed char cas8(signed char *p, signed char e, signed char d)
{ return __sync_val_compare_and_swap(p, e, d); }
/* desired in the SECOND argument register */
NI static unsigned char cas8d(unsigned char *p, unsigned char d,
                              unsigned char e)
{ return __sync_val_compare_and_swap(p, e, d); }
NI static unsigned short cas16(unsigned short *p, unsigned short e,
                               unsigned short d)
{ return __sync_val_compare_and_swap(p, e, d); }
NI static int cas32b(int *p, int e, int d)
{ return __sync_bool_compare_and_swap(p, e, d); }
NI static long cas64(long *p, long e, long d)
{ return __sync_val_compare_and_swap(p, e, d); }

/* by pointer: a bool back, and a miss writes the value seen to *e */
NI static int cx8(signed char *p, signed char *e, signed char d)
{ return __atomic_compare_exchange_n(p, e, d, 0, SEQ, SEQ); }
NI static int cx8d(unsigned char *p, unsigned char d, unsigned char *e)
{ return __atomic_compare_exchange_n(p, e, d, 0, SEQ, SEQ); }
NI static int cx16(short *p, short *e, short d)
{ return __atomic_compare_exchange_n(p, e, d, 1, SEQ, SEQ); }
NI static int cx32(unsigned *p, unsigned *e, unsigned d)
{ return __atomic_compare_exchange_n(p, e, d, 0, SEQ, SEQ); }
NI static int cx64(long *p, long *e, long d)
{ return __atomic_compare_exchange_n(p, e, d, 0, SEQ, SEQ); }

/* and / or / xor / nand: the compare-exchange loop */
NI static unsigned char and8(unsigned char *p, unsigned char v)
{ return __atomic_fetch_and(p, v, SEQ); }
NI static short or16(short *p, short v)
{ return __atomic_fetch_or(p, v, SEQ); }
NI static int xor32(int *p, int v)
{ return __atomic_fetch_xor(p, v, SEQ); }
NI static long nand64(long *p, long v)
{ return __atomic_fetch_nand(p, v, SEQ); }
NI static signed char nand8(signed char *p, signed char v)
{ return __atomic_nand_fetch(p, v, SEQ); }

/* a lock's shape: the expected value is a local whose address is taken */
NI static int trylock(int *l)
{
    int c = 0;
    if (__atomic_compare_exchange_n(l, &c, 1, 0, __ATOMIC_ACQUIRE,
                                    __ATOMIC_RELAXED))
        return 0;
    return c + 10;
}

/* Many values live across the atomics, each one used after them: some
 * operands do not get a register. */
NI static unsigned long busy(long *obj, int n, unsigned long seed)
{
    unsigned long h = seed, a = seed * 3, b = seed ^ 0x55, c = seed + 7,
                  d = seed >> 1, e = seed * 5, f = seed + 11;
    for (int k = 0; k < n; k++) {
        long *p = obj + (k & 3);
        long old = __atomic_fetch_add(p, (long)(a & 15), SEQ);
        long exp = old + (long)(a & 15);
        int ok = __atomic_compare_exchange_n(p, &exp, (long)(b & 255), 0,
                                             SEQ, SEQ);
        long sw = __atomic_exchange_n(obj + ((k + 1) & 3), (long)c, SEQ);
        long an = __atomic_fetch_and(obj + ((k + 2) & 3), (long)~(d & 7),
                                     SEQ);
        long cs = __sync_val_compare_and_swap(obj + ((k + 3) & 3), sw,
                                              (long)(e & 1023));
        h = h * 31 + (unsigned long)old;
        h = h * 31 + (unsigned long)ok;
        h = h * 31 + (unsigned long)sw;
        h = h * 31 + (unsigned long)an;
        h = h * 31 + (unsigned long)cs;
        a = a * 7 + h; b = b * 13 + a; c = c * 3 + b;
        d = d + c; e = e * 5 + d; f = f ^ e;
        h ^= a + b + c + d + e + f;
    }
    return h;
}

int main(void)
{
    signed char c8 = -100;
    if (xchg8(&c8, -128) != -100 || c8 != -128) return 1;
    short s16 = -30000;
    if (xchg16(&s16, 0x7ffe) != -30000 || s16 != 0x7ffe) return 2;
    unsigned u32 = 0x80000001u;
    if (xchg32(&u32, 0xfffffffeu) != 0x80000001u || u32 != 0xfffffffeu)
        return 3;
    long l64 = 0x123456789abcdef0L;
    if (xchg64(&l64, -2) != 0x123456789abcdef0L || l64 != -2) return 4;

    unsigned char u8 = 250;
    if (xadd8(&u8, 10) != 250 || u8 != 4) return 5;
    s16 = -32768;
    if (xadd16(&s16, 1) != -32768 || s16 != 32767) return 6;
    int i32 = 0x7fffffff;
    if (xadd32(&i32, -0x7fffffff) != 0 || i32 != 0) return 7;
    unsigned long ul = 0xffffffff00000000UL;
    if (xadd64(&ul, 0x100000001UL) != 0xffffffff00000000UL || ul != 1)
        return 8;

    c8 = -5;
    if (cas8(&c8, -5, 120) != -5 || c8 != 120) return 9;
    if (cas8(&c8, -5, 1) != 120 || c8 != 120) return 10;
    u8 = 0x80;
    if (cas8d(&u8, 0xff, 0x80) != 0x80 || u8 != 0xff) return 11;
    if (cas8d(&u8, 0x01, 0x80) != 0xff || u8 != 0xff) return 12;
    unsigned short u16 = 0x8001;
    if (cas16(&u16, 0x8001, 0xfffe) != 0x8001 || u16 != 0xfffe) return 13;
    i32 = -7;
    if (!cas32b(&i32, -7, 1 << 31) || i32 != (int)(1u << 31)) return 14;
    if (cas32b(&i32, -7, 3) || i32 != (int)(1u << 31)) return 15;
    l64 = 1L << 40;
    if (cas64(&l64, 1L << 40, -(1L << 41)) != 1L << 40 || l64 != -(1L << 41))
        return 16;

    signed char e8 = 3;
    c8 = 3;
    if (!cx8(&c8, &e8, -3) || c8 != -3 || e8 != 3) return 17;
    if (cx8(&c8, &e8, 9) || c8 != -3 || e8 != -3) return 18;
    unsigned char ue8 = 0xf0;
    u8 = 0x0f;
    if (cx8d(&u8, 0xaa, &ue8) || u8 != 0x0f || ue8 != 0x0f) return 19;
    if (!cx8d(&u8, 0xaa, &ue8) || u8 != 0xaa || ue8 != 0x0f) return 20;
    short e16 = -1;
    s16 = -1;
    while (!cx16(&s16, &e16, 0x1234))   /* weak: may fail spuriously */
        if (e16 != -1) return 21;
    if (s16 != 0x1234) return 22;
    unsigned e32 = 5;
    u32 = 6;
    if (cx32(&u32, &e32, 7) || e32 != 6 || u32 != 6) return 23;
    if (!cx32(&u32, &e32, 0x90000000u) || u32 != 0x90000000u) return 24;
    long e64 = -9;
    l64 = -9;
    if (!cx64(&l64, &e64, 1L << 62) || l64 != 1L << 62 || e64 != -9) return 25;
    if (cx64(&l64, &e64, 0) || e64 != 1L << 62) return 26;

    u8 = 0xf3;
    if (and8(&u8, 0x3c) != 0xf3 || u8 != 0x30) return 27;
    s16 = (short)0x8000;
    if (or16(&s16, 0x0101) != (short)0x8000 || s16 != (short)0x8101) return 28;
    i32 = 0x0ff0;
    if (xor32(&i32, -1) != 0x0ff0 || i32 != ~0x0ff0) return 29;
    l64 = 0x00ff00ff00ff00ffL;
    if (nand64(&l64, 0x0f0f0f0f0f0f0f0fL) != 0x00ff00ff00ff00ffL ||
        l64 != ~(0x00ff00ff00ff00ffL & 0x0f0f0f0f0f0f0f0fL)) return 30;
    c8 = -1;
    if (nand8(&c8, 0x7f) != (signed char)~0x7f || c8 != (signed char)~0x7f)
        return 31;

    int lk = 0;
    if (trylock(&lk) != 0 || lk != 1) return 32;
    if (trylock(&lk) != 11 || lk != 1) return 33;
    lk = 2;
    if (trylock(&lk) != 12) return 34;

    long obj[4] = { 1, -2, 3, -4 };
    unsigned long h = busy(obj, 40, 12345);
    h = h * 31 + (unsigned long)obj[0] + (unsigned long)obj[1] * 3 +
        (unsigned long)obj[2] * 5 + (unsigned long)obj[3] * 7;
    if (h != 0xe4fb17db3673261dUL) return 35;
    return 42;
}
