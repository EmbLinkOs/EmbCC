// expect-exit: 42
/* Every predicate, signed and unsigned, against constants at the edges of
 * a 12-bit immediate: RISC-V compares against one with slti/sltiu, turns
 * `x <= k` into `x < k + 1`, and `==` into an xori and a seqz. Each result
 * is checked against the same compare done in long long. */
#define K(n, k) \
  static int lt##n(int x) { return x < (k); } \
  static int le##n(int x) { return x <= (k); } \
  static int gt##n(int x) { return x > (k); } \
  static int ge##n(int x) { return x >= (k); } \
  static int eq##n(int x) { return x == (k); } \
  static int ne##n(int x) { return x != (k); } \
  static int ult##n(unsigned x) { return x < (unsigned)(k); } \
  static int ule##n(unsigned x) { return x <= (unsigned)(k); } \
  static int ugt##n(unsigned x) { return x > (unsigned)(k); } \
  static int uge##n(unsigned x) { return x >= (unsigned)(k); } \
  static unsigned f##n(int x) \
  { \
      unsigned u = (unsigned)x; \
      return (unsigned)lt##n(x) | (unsigned)le##n(x) << 1 | \
             (unsigned)gt##n(x) << 2 | (unsigned)ge##n(x) << 3 | \
             (unsigned)eq##n(x) << 4 | (unsigned)ne##n(x) << 5 | \
             (unsigned)ult##n(u) << 6 | (unsigned)ule##n(u) << 7 | \
             (unsigned)ugt##n(u) << 8 | (unsigned)uge##n(u) << 9; \
  } \
  static unsigned r##n(int x) \
  { \
      long long a = x, b = (k); \
      unsigned long long ua = (unsigned)x, ub = (unsigned)(k); \
      return (unsigned)(a < b) | (unsigned)(a <= b) << 1 | \
             (unsigned)(a > b) << 2 | (unsigned)(a >= b) << 3 | \
             (unsigned)(a == b) << 4 | (unsigned)(a != b) << 5 | \
             (unsigned)(ua < ub) << 6 | (unsigned)(ua <= ub) << 7 | \
             (unsigned)(ua > ub) << 8 | (unsigned)(ua >= ub) << 9; \
  }
K(0, 0) K(1, 1) K(2, -1) K(3, 2047) K(4, 2046)
K(5, -2048) K(6, -2049) K(7, 2048) K(8, -2047) K(9, 5)

static volatile int vals[] = {
    0, 1, -1, 2, -2, 5, 4, 6, 2046, 2047, 2048, 2049,
    -2047, -2048, -2049, -2050, 32767, -32767 - 1,
#if __INT_MAX__ > 32767
    0x7fffffff, -0x7fffffff - 1, -0x7fffffff, 0x7ffffffe,
#endif
};

int main(void)
{
    unsigned i;
    for (i = 0; i < sizeof vals / sizeof vals[0]; i++) {
        int x = vals[i];
#define C(n) if (f##n(x) != r##n(x)) return 100 + n;
        C(0) C(1) C(2) C(3) C(4) C(5) C(6) C(7) C(8) C(9)
    }
    return 42;
}
