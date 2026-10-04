// expect-exit: 42
/* An array indexed by an int, `a[i]`, at every element size and sign,
 * loaded and stored. On aarch64 the index's extension to 64 bits folds
 * into the access (`ldr w0, [x1, w2, sxtw #2]`), and the extension must
 * be the index's own: sign for an int, zero for an unsigned -- a NEGATIVE
 * int index read as unsigned lands four gigabytes away. Each access goes
 * through a pointer to the middle of its array so negative indices are
 * in bounds. A byte offset (`*(int *)(p + off)`) is unscaled, and a shift
 * that is not the element size (a char array indexed by i*4) must not be
 * mistaken for the scale. */
typedef unsigned long long u64;

__attribute__((noinline)) static long ld_c(const signed char *p, int i)   { return p[i]; }
__attribute__((noinline)) static long ld_uc(const unsigned char *p, int i) { return p[i]; }
__attribute__((noinline)) static long ld_s(const short *p, int i)         { return p[i]; }
__attribute__((noinline)) static long ld_us(const unsigned short *p, int i) { return p[i]; }
__attribute__((noinline)) static long ld_i(const int *p, int i)           { return p[i]; }
__attribute__((noinline)) static long ld_u(const unsigned *p, unsigned i) { return p[i]; }
__attribute__((noinline)) static long long ld_l(const long long *p, int i) { return p[i]; }
__attribute__((noinline)) static void st_c(char *p, int i, int v)         { p[i] = (char)v; }
__attribute__((noinline)) static void st_s(short *p, int i, int v)        { p[i] = (short)v; }
__attribute__((noinline)) static void st_i(int *p, int i, int v)          { p[i] = v; }
__attribute__((noinline)) static void st_l(long long *p, int i, long long v) { p[i] = v; }
__attribute__((noinline)) static void st_us(unsigned short *p, unsigned i, int v) { p[i] = (unsigned short)v; }
/* p + off stays within an int array and on an int boundary */
__attribute__((noinline)) static int unscaled(const char *p, int off)    { return *(const int *)(p + off); }
/* the index scaled for ints, the access a byte: the shift is not the
 * access's scale and must stay a shift */
__attribute__((noinline)) static int narrow(const int *p, int i)         { return *(const signed char *)(p + i); }
/* signed char: plain char is unsigned on aarch64 */
__attribute__((noinline)) static int skew(const signed char *p, int i)   { return p[i * 4]; }

int main(void)
{
    signed char c[16];
    short s[16];
    int w[16];
    long long l[16];
    for (int k = 0; k < 16; k++) {
        c[k] = (signed char)(k * 17 - 100);
        s[k] = (short)(k * 3001 - 20000);
        w[k] = k * 100003 - 700000;
        l[k] = (long long)k * 9000000007LL - 60000000000LL;
    }
    signed char *cm = c + 8;
    short *sm = s + 8;
    int *wm = w + 8;
    long long *lm = l + 8;
    long sum = 0;
    for (int i = -8; i < 8; i++) {
        if (ld_c(cm, i) != c[i + 8]) return 1;
        if (ld_uc((unsigned char *)cm, i) != (unsigned char)c[i + 8]) return 2;
        if (ld_s(sm, i) != s[i + 8]) return 3;
        if (ld_us((unsigned short *)sm, i) != (unsigned short)s[i + 8]) return 4;
        if (ld_i(wm, i) != w[i + 8]) return 5;
        if (ld_l(lm, i) != l[i + 8]) return 6;
        sum += ld_i(wm, i);
    }
    for (unsigned u = 0; u < 8; u++)
        if (ld_u((unsigned *)w, u) != (unsigned)w[u]) return 7;
    for (int i = -8; i < 8; i++) {
        st_c((char *)cm, i, i * 3);
        st_s(sm, i, i * 1000);
        st_i(wm, i, i * 123457);
        st_l(lm, i, (long long)i * 40000000000LL);
    }
    for (int k = 0; k < 16; k++)
        if (c[k] != (signed char)((k - 8) * 3) || s[k] != (short)((k - 8) * 1000) ||
            w[k] != (k - 8) * 123457 || l[k] != (long long)(k - 8) * 40000000000LL)
            return 8;
    for (unsigned u = 0; u < 16; u++)
        st_us((unsigned short *)s, u, (int)(u * 4000u));
    for (int k = 0; k < 16; k++)
        if ((unsigned short)s[k] != (unsigned short)(k * 4000)) return 9;
    if (unscaled((const char *)w, 8) != w[2] || unscaled((const char *)wm, -4) != w[7])
        return 10;
    for (int i = -2; i < 2; i++)
        if (skew(cm, i) != cm[i * 4]) return 11;
    for (int i = -8; i < 8; i++) {
        int lo = wm[i] & 0xff;
        if (narrow(wm, i) != (lo > 127 ? lo - 256 : lo)) return 13;
    }
    /* sum of w[k] = k * 100003 - 700000 over k = 0..15, read through
     * the negative and positive indices */
    return sum == 800360 ? 42 : 12;
}
