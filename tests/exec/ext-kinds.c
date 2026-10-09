/* An extension of an extension, and of a narrow load, at every pairing of
 * kinds and widths. The optimizer folds these chains (src/opt/fold.c, IR_EXT), and
 * the fold is only a copy when the outer extension leaves the inner's
 * result as it was: WIDER than the inner, that holds when the inner
 * zero-extended or both sign-extend, and not for a zero-extension of a
 * sign-extended value -- (unsigned short)(signed char)-1 is 65535, and
 * -O2 once returned -1 for it. Each case is a function of its argument,
 * so nothing is folded to a constant before the chain is. */
// expect-exit: 42
typedef unsigned long long u64;

__attribute__((noinline)) int us_sc(signed char c)    { return (unsigned short)c; }
__attribute__((noinline)) int ss_sc(signed char c)    { return (short)c; }
__attribute__((noinline)) int us_uc(unsigned char c)  { return (unsigned short)c; }
__attribute__((noinline)) int ss_uc(unsigned char c)  { return (short)c; }
__attribute__((noinline)) int sc_ss(short s)          { return (signed char)s; }
__attribute__((noinline)) int uc_ss(short s)          { return (unsigned char)s; }
__attribute__((noinline)) int sc_us(unsigned short s) { return (signed char)s; }
__attribute__((noinline)) unsigned u_us_sc(int x)
{ signed char c = (signed char)x; unsigned short u = (unsigned short)c; return u; }
__attribute__((noinline)) u64 ull_i(int x)            { return (u64)x; }
__attribute__((noinline)) u64 ull_u(int x)            { return (u64)(unsigned)x; }
__attribute__((noinline)) u64 ull_us_sc(signed char c)
{ return (u64)(unsigned short)c; }

/* ...and of a LOAD, which extends what it reads already */
__attribute__((noinline)) int ld_us_sc(const signed char *p)   { return (unsigned short)*p; }
__attribute__((noinline)) int ld_ss_sc(const signed char *p)   { return (short)*p; }
__attribute__((noinline)) int ld_us_uc(const unsigned char *p) { return (unsigned short)*p; }
__attribute__((noinline)) int ld_sc_uc(const unsigned char *p) { return (signed char)*p; }
__attribute__((noinline)) int ld_uc_uc(const unsigned char *p) { return (unsigned char)*p; }

int main(void)
{
    signed char m1 = -1, p5 = 5;
    unsigned char ff = 0xff, u7 = 0x7f;
    if (us_sc(-1) != 65535 || us_sc(5) != 5) return 1;
    if (ss_sc(-1) != -1 || ss_sc(-128) != -128) return 2;
    if (us_uc(0xff) != 255) return 3;
    if (ss_uc(0xff) != 255) return 4;
    if (sc_ss(0x1ff) != -1 || sc_ss(0x17f) != 127) return 5;
    if (uc_ss(-1) != 255) return 6;
    if (sc_us(0xff80) != -128) return 7;
    if (u_us_sc(255) != 65535u || u_us_sc(0x17f) != 127u) return 8;
    if (ull_i(-1) != ~0ULL) return 9;
    if (ull_u(-1) != 0xffffffffULL) return 10;
    if (ull_us_sc(-1) != 65535ULL) return 11;
    if (ld_us_sc(&m1) != 65535 || ld_us_sc(&p5) != 5) return 12;
    if (ld_ss_sc(&m1) != -1) return 13;
    if (ld_us_uc(&ff) != 255) return 14;
    if (ld_sc_uc(&ff) != -1 || ld_sc_uc(&u7) != 127) return 15;
    if (ld_uc_uc(&ff) != 255) return 16;
    return 42;
}
