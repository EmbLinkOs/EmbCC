/* Constants at the edges of what an instruction can hold as an immediate
 * -- 12 bits and 12 bits shifted for add, subtract and compare; bitmask
 * patterns for and, or and xor; multipliers that are and are not 2^k +- 1
 * -- at 32 and 64 bits, each inside a loop (where a constant no
 * instruction holds must be built once, before it) and outside one. The
 * reference is the same arithmetic on volatile copies of the constants,
 * which no compiler folds. */
// expect-exit: 42
#define NI __attribute__((noinline))
typedef unsigned long long u64; typedef unsigned u32;

#define K(c) c,
static const u64 ks[] = {
    K(1) K(4095) K(4096) K(4097) K(0xfff000) K(0xfff001) K(0x1000000) K(0x7fffffff)
    K(0x00ff00ff) K(0x0f0f0f0f) K(0x55555555) K(0x00ff00f0) K(0xffff0000) K(0x1ffffe)
    K(16777619) K(2166136261u) K(0x123456789abcdefull) K(0xffffffffffffff00ull)
    K(3) K(5) K(9) K(7) K(15) K(12) K(40) K(31) K(33) K(0x5555555555555555ull)
};
#define NK (sizeof ks / sizeof *ks)
static volatile u64 vk[NK];

static int bad;

#define DEF(k) \
    NI u32 a32_##k(u32 x, int n) { for (int i = 0; i < n; i++) x = x + (u32)ks[k]; return x; } \
    NI u64 a64_##k(u64 x, int n) { for (int i = 0; i < n; i++) x = x + ks[k]; return x; } \
    NI u32 s32_##k(u32 x, int n) { for (int i = 0; i < n; i++) x = x - (u32)ks[k]; return x; } \
    NI u64 s64_##k(u64 x, int n) { for (int i = 0; i < n; i++) x = x - ks[k]; return x; } \
    NI u32 n32_##k(u32 x, int n) { u32 s = 0; for (int i = 0; i < n; i++) s += (x + (u32)i) & (u32)ks[k]; return s; } \
    NI u64 n64_##k(u64 x, int n) { u64 s = 0; for (int i = 0; i < n; i++) s += (x + (u64)i) & ks[k]; return s; } \
    NI u32 o32_##k(u32 x, int n) { u32 s = 0; for (int i = 0; i < n; i++) s += ((x + (u32)i) | (u32)ks[k]) ^ (u32)(ks[k] >> 1); return s; } \
    NI u64 o64_##k(u64 x, int n) { u64 s = 0; for (int i = 0; i < n; i++) s += ((x + (u64)i) | ks[k]) ^ (ks[k] >> 1); return s; } \
    NI u32 m32_##k(u32 x, int n) { for (int i = 0; i < n; i++) x = x * (u32)ks[k] + 1; return x; } \
    NI u64 m64_##k(u64 x, int n) { for (int i = 0; i < n; i++) x = x * ks[k] + 1; return x; } \
    NI int c32_##k(u32 x, int n) { int c = 0; for (int i = 0; i < n; i++) c += (x + (u32)i) > (u32)ks[k]; return c; } \
    NI int c64_##k(u64 x, int n) { int c = 0; for (int i = 0; i < n; i++) c += (x + (u64)i) > ks[k]; return c; } \
    static void chk_##k(u64 x, int n) { \
        u64 c = vk[k]; u32 c32 = (u32)c; \
        u32 r32; u64 r64; int rc; \
        r32 = (u32)x; for (int i = 0; i < n; i++) r32 = r32 + c32; if (a32_##k((u32)x, n) != r32) bad = 1000 + k; \
        r64 = x; for (int i = 0; i < n; i++) r64 = r64 + c; if (a64_##k(x, n) != r64) bad = 2000 + k; \
        r32 = (u32)x; for (int i = 0; i < n; i++) r32 = r32 - c32; if (s32_##k((u32)x, n) != r32) bad = 3000 + k; \
        r64 = x; for (int i = 0; i < n; i++) r64 = r64 - c; if (s64_##k(x, n) != r64) bad = 4000 + k; \
        r32 = 0; for (int i = 0; i < n; i++) r32 += ((u32)x + (u32)i) & c32; if (n32_##k((u32)x, n) != r32) bad = 5000 + k; \
        r64 = 0; for (int i = 0; i < n; i++) r64 += (x + (u64)i) & c; if (n64_##k(x, n) != r64) bad = 6000 + k; \
        r32 = 0; for (int i = 0; i < n; i++) r32 += (((u32)x + (u32)i) | c32) ^ (u32)(c >> 1); if (o32_##k((u32)x, n) != r32) bad = 7000 + k; \
        r64 = 0; for (int i = 0; i < n; i++) r64 += ((x + (u64)i) | c) ^ (c >> 1); if (o64_##k(x, n) != r64) bad = 8000 + k; \
        r32 = (u32)x; for (int i = 0; i < n; i++) r32 = r32 * c32 + 1; if (m32_##k((u32)x, n) != r32) bad = 9000 + k; \
        r64 = x; for (int i = 0; i < n; i++) r64 = r64 * c + 1; if (m64_##k(x, n) != r64) bad = 10000 + k; \
        rc = 0; for (int i = 0; i < n; i++) rc += ((u32)x + (u32)i) > c32; if (c32_##k((u32)x, n) != rc) bad = 11000 + k; \
        rc = 0; for (int i = 0; i < n; i++) rc += (x + (u64)i) > c; if (c64_##k(x, n) != rc) bad = 12000 + k; \
    }
DEF(0) DEF(1) DEF(2) DEF(3) DEF(4) DEF(5) DEF(6) DEF(7) DEF(8) DEF(9) DEF(10) DEF(11) DEF(12) DEF(13)
DEF(14) DEF(15) DEF(16) DEF(17) DEF(18) DEF(19) DEF(20) DEF(21) DEF(22) DEF(23) DEF(24) DEF(25) DEF(26) DEF(27)

int main(void)
{
    for (unsigned k = 0; k < NK; k++) vk[k] = ks[k];
    static const u64 xs[] = { 0, 1, 4094, 0xfff000, 0x7ffffff0, 0xfffffff0u, 0x8000000000000000ull, 0x123456789ull };
    for (unsigned j = 0; j < sizeof xs / sizeof *xs; j++) {
        u64 x = xs[j];
        chk_0(x, 7); chk_1(x, 7); chk_2(x, 7); chk_3(x, 7); chk_4(x, 7); chk_5(x, 7); chk_6(x, 7);
        chk_7(x, 7); chk_8(x, 7); chk_9(x, 7); chk_10(x, 7); chk_11(x, 7); chk_12(x, 7); chk_13(x, 7);
        chk_14(x, 7); chk_15(x, 7); chk_16(x, 7); chk_17(x, 7); chk_18(x, 7); chk_19(x, 7); chk_20(x, 7);
        chk_21(x, 7); chk_22(x, 7); chk_23(x, 7); chk_24(x, 7); chk_25(x, 7); chk_26(x, 7); chk_27(x, 7);
    }
    if (bad) return 1 + bad % 200;
    return 42;
}
