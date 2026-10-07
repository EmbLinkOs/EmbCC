/* One- and two-byte atomics on LoongArch64, which are ll.w/sc.w loops on
 * the word holding the field (src/arch/loongarch/codegen.c,
 * la_atomic_narrow): every operation at every position of a field in its
 * word, checking the value returned (re-extended as the type says), the
 * field written, and that the neighbouring bytes are untouched. Each
 * check returns its own number; 42 is success. */
union w { unsigned int word; unsigned char b[4]; unsigned short h[2];
          signed char sb[4]; short sh[2]; };

static int check_bytes(void)
{
    for (int k = 0; k < 4; k++) {
        union w u;
        u.word = 0xa1b2c3d4u;
        unsigned char before = u.b[k];
        unsigned int others = u.word & ~(0xffu << (8 * k));
        unsigned char old = __atomic_fetch_add(&u.b[k], 0x31, __ATOMIC_SEQ_CST);
        if (old != before || u.b[k] != (unsigned char)(before + 0x31)) return 1;
        if ((u.word & ~(0xffu << (8 * k))) != others) return 2;
        old = __atomic_exchange_n(&u.b[k], 0x5a, __ATOMIC_SEQ_CST);
        if (old != (unsigned char)(before + 0x31) || u.b[k] != 0x5a) return 3;
        if ((u.word & ~(0xffu << (8 * k))) != others) return 4;
        __atomic_fetch_and(&u.b[k], 0x0f, __ATOMIC_SEQ_CST);
        __atomic_fetch_or(&u.b[k], 0xc0, __ATOMIC_SEQ_CST);
        __atomic_fetch_xor(&u.b[k], 0x03, __ATOMIC_SEQ_CST);
        if (u.b[k] != (((0x5a & 0x0f) | 0xc0) ^ 0x03)) return 5;
        __atomic_fetch_nand(&u.b[k], 0x3c, __ATOMIC_SEQ_CST);
        if (u.b[k] != (unsigned char)~((((0x5a & 0x0f) | 0xc0) ^ 0x03) & 0x3c))
            return 6;
        if ((u.word & ~(0xffu << (8 * k))) != others) return 7;
        unsigned char e = 0x11;
        if (__atomic_compare_exchange_n(&u.b[k], &e, 0x22, 0,
                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            return 8;                      /* a miss */
        if (e != u.b[k]) return 9;         /* writes back what it saw */
        if (!__atomic_compare_exchange_n(&u.b[k], &e, 0x22, 0,
                                         __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            return 10;
        if (u.b[k] != 0x22) return 11;
        if ((u.word & ~(0xffu << (8 * k))) != others) return 12;
        u.sb[k] = -5;
        if (__atomic_fetch_sub(&u.sb[k], 1, __ATOMIC_SEQ_CST) != -5) return 13;
        if (__sync_val_compare_and_swap(&u.sb[k], -6, -100) != -6) return 14;
        if (u.sb[k] != -100) return 15;
        if (__sync_val_compare_and_swap(&u.sb[k], 7, 8) != -100) return 16;
        if ((u.word & ~(0xffu << (8 * k))) != others) return 17;
    }
    return 0;
}

static int check_halves(void)
{
    for (int k = 0; k < 2; k++) {
        union w u;
        u.word = 0x8badf00du;
        unsigned int others = u.word & ~(0xffffu << (16 * k));
        unsigned short before = u.h[k];
        if (__atomic_fetch_add(&u.h[k], 0x1234, __ATOMIC_SEQ_CST) != before)
            return 20;
        if (u.h[k] != (unsigned short)(before + 0x1234)) return 21;
        if (__atomic_exchange_n(&u.h[k], 0xfffe, __ATOMIC_SEQ_CST) !=
            (unsigned short)(before + 0x1234)) return 22;
        if ((u.word & ~(0xffffu << (16 * k))) != others) return 23;
        u.sh[k] = -300;
        if (__atomic_add_fetch(&u.sh[k], -1, __ATOMIC_SEQ_CST) != -301) return 24;
        short e = -301;
        if (!__atomic_compare_exchange_n(&u.sh[k], &e, 4000, 0,
                                         __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            return 25;
        if (u.sh[k] != 4000) return 26;
        if (__sync_bool_compare_and_swap(&u.sh[k], 1, 2)) return 27;
        if ((u.word & ~(0xffffu << (16 * k))) != others) return 28;
    }
    return 0;
}

int main(void)
{
    int r = check_bytes();
    if (r) return r;
    r = check_halves();
    if (r) return r;
    return 42;
}
