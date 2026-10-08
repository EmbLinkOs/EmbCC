/* `if (x & BIT)` and `if (x & LOWMASK)` for every bit and every low mask:
 * on Cortex-M the test may be `lsls rT, rX, #31-k` (Z is the bit's) where
 * tst.w would be four bytes -- except bit 31, where `lsls #0` is a move
 * whose Z is the whole word's. Each constant in its own branch, both
 * senses, checked against the same test with the mask in a variable. */
// expect-exit: 42
static volatile unsigned vmask;
#define B(k) __attribute__((noinline)) int b##k(unsigned x) { if (x & (1u << k)) return 1; return 0; } \
    __attribute__((noinline)) int n##k(unsigned x) { if (!(x & (1u << k))) return 1; return 0; }
#define M(w) __attribute__((noinline)) int m##w(unsigned x) { if (x & ((1u << w) - 1)) return 1; return 0; }

B(0)
B(1)
B(2)
B(3)
B(4)
B(5)
B(6)
B(7)
B(8)
B(9)
B(10)
B(11)
B(12)
B(13)
B(14)
B(15)
B(16)
B(17)
B(18)
B(19)
B(20)
B(21)
B(22)
B(23)
B(24)
B(25)
B(26)
B(27)
B(28)
B(29)
B(30)
B(31)
M(1)
M(2)
M(3)
M(4)
M(5)
M(6)
M(7)
M(8)
M(9)
M(10)
M(11)
M(12)
M(13)
M(14)
M(15)
M(16)
M(17)
M(18)
M(19)
M(20)
M(21)
M(22)
M(23)
M(24)
M(25)
M(26)
M(27)
M(28)
M(29)
M(30)
M(31)
typedef int (*fp)(unsigned);
static const fp bs[32] = { b0, b1, b2, b3, b4, b5, b6, b7, b8, b9, b10, b11, b12, b13, b14, b15, b16, b17, b18, b19, b20, b21, b22, b23, b24, b25, b26, b27, b28, b29, b30, b31 };
static const fp ns[32] = { n0, n1, n2, n3, n4, n5, n6, n7, n8, n9, n10, n11, n12, n13, n14, n15, n16, n17, n18, n19, n20, n21, n22, n23, n24, n25, n26, n27, n28, n29, n30, n31 };
static const fp ms[32] = { 0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13, m14, m15, m16, m17, m18, m19, m20, m21, m22, m23, m24, m25, m26, m27, m28, m29, m30, m31 };
static const unsigned vals[] = { 0, 1, 2, 3, 0x80000000u, 0x7fffffffu, 0xffffffffu,
    0x55555555u, 0xaaaaaaaau, 0x00010000u, 0x0000ffffu, 0xffff0000u, 0x12345678u };
int main(void)
{
    for (unsigned j = 0; j < sizeof vals / sizeof vals[0]; j++) {
        unsigned x = vals[j];
        for (int k = 0; k < 32; k++) {
            vmask = 1u << k;
            int w = (x & vmask) != 0;
            if (bs[k](x) != w) return 1;
            if (ns[k](x) != !w) return 2;
            if (k) {
                vmask = (1u << k) - 1;
                if (ms[k](x) != ((x & vmask) != 0)) return 3;
            }
        }
    }
    return 42;
}
