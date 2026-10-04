// expect-exit: 42
/* A store of N bytes may take an extension's source instead of the
 * extension, when the extension keeps at least those N bytes: the low
 * bytes are the same. Each function here is a case where that rule is
 * the whole story -- and two where it is not: a store WIDER than what
 * the extension kept (a sign-extended byte stored as two bytes must carry
 * its sign into the second), and a source a loop writes again after the
 * extension read it, which the store must not see. */
__attribute__((noinline)) static void bytes(unsigned char *k, unsigned v)
{
    int n = 0;
    do { k[n++] = (char)('a' + v % 26); v /= 26; } while (v && n < 10);
    k[n] = 0;
}

__attribute__((noinline)) static void wider(short *out, int x)
{
    *out = (signed char)x;              /* 0x1ff: -1, not 0x1ff */
}

/* (long: an int is sixteen bits on AVR) */
__attribute__((noinline)) static void halves(unsigned short *out,
                                             unsigned long x)
{
    out[0] = (unsigned short)x;
    out[1] = (unsigned short)(x >> 16);
}

/* y is the byte of x as it was; the loop then changes x before y is
 * stored, so the store must keep y. */
__attribute__((noinline)) static long carried(signed char *out, int n)
{
    long x = 300;
    for (int i = 0; i < n; i++) {
        signed char y = (signed char)x;
        x = x * 7 + 1;
        out[i] = y;
    }
    return x;
}

int main(void)
{
    unsigned char k[12];
    short s;
    unsigned short h[2];
    signed char c[6];
    bytes(k, 27);                       /* 27 = 1*26 + 1: "bb" */
    if (k[0] != 'b' || k[1] != 'b' || k[2]) return 1;
    wider(&s, 0x1ff);
    if (s != -1) return 2;
    halves(h, 0x12345678ul);
    if (h[0] != 0x5678 || h[1] != 0x1234) return 3;
    /* 300 -> 2101 -> 14708 -> 102957 -> 720700: bytes 44, 53, 116, 45,
     * 60. Five and three, so the loop's remainder runs as well as its
     * unrolled body: the remainder is where x is written in place
     * between the byte's extension and its store. */
    carried(c, 5);
    if (c[0] != 44 || c[1] != 53 || c[2] != 116 || c[3] != 45 || c[4] != 60)
        return 4;
    carried(c, 3);
    if (c[0] != 44 || c[1] != 53 || c[2] != 116) return 5;
    return 42;
}
