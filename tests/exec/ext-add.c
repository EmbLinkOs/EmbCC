/* An extension feeding an add or a subtract, at every width and both
 * signs, with values whose top bit is set -- where a backend that fuses
 * the two into one instruction with an extended operand (`add x0, x1,
 * w2, sxtw`) must still pick the right extension: sxtb for a signed
 * char, uxth for an unsigned short, and so on. */
// expect-exit: 42
#define F(name, T, U) \
    __attribute__((noinline)) long name##_add(long s, T v) { return s + v; } \
    __attribute__((noinline)) long name##_sub(long s, T v) { return s - v; } \
    __attribute__((noinline)) long name##_radd(T v, long s) { return v + s; } \
    __attribute__((noinline)) int  name##_iadd(int s, T v) { return s + v; } \
    __attribute__((noinline)) int  name##_isub(int s, T v) { return s - v; }
F(sc, signed char, 1) F(uc, unsigned char, 1) F(ss, short, 1) F(us, unsigned short, 1)
F(si, int, 1) F(ui, unsigned, 1)
int main(void)
{
    long s = 1000000000000L;
    int bad = 0;
    if (sc_add(s, -1) != s - 1 || sc_sub(s, -128) != s + 128 || sc_radd(-100, s) != s - 100) bad |= 1;
    if (uc_add(s, 255) != s + 255 || uc_sub(s, 200) != s - 200 || uc_radd(255, s) != s + 255) bad |= 2;
    if (ss_add(s, -32768) != s - 32768 || ss_sub(s, -1) != s + 1 || ss_radd(-3, s) != s - 3) bad |= 4;
    if (us_add(s, 65535) != s + 65535 || us_sub(s, 40000) != s - 40000 || us_radd(65535, s) != s + 65535) bad |= 8;
    if (si_add(s, -2147483647 - 1) != s - 2147483648L || si_sub(s, -1) != s + 1 || si_radd(-7, s) != s - 7) bad |= 16;
    if (ui_add(s, 4294967295u) != s + 4294967295L || ui_sub(s, 3000000000u) != s - 3000000000L || ui_radd(4294967295u, s) != s + 4294967295L) bad |= 32;
    if (sc_iadd(5, -1) != 4 || sc_isub(5, -1) != 6 || uc_iadd(5, 255) != 260 || uc_isub(5, 255) != -250) bad |= 64;
    if (ss_iadd(1, -32768) != -32767 || us_isub(1, 65535) != -65534) bad |= 128;
    if (bad) return bad;
    return 42;
}
